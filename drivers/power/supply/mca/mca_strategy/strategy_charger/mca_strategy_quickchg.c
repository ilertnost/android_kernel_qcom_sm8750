// SPDX-License-Identifier: GPL-2.0
/*
 * mca_strategy_quickchg.c
 *
 * mca quick charger strategy driver
 *
 * Copyright (c) 2023-2023 Xiaomi Technologies Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */
#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_quick_charge"
#endif

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>
#include <mca/common/mca_charge_interface.h>
#include <mca/common/mca_charge_mievent.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_hwid.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/common/mca_sysfs.h>
#include <mca/common/mca_voter.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/platform/platform_cp_class.h>
#include <mca/platform/platform_fg_ic_ops.h>
#include <mca/platform/platform_loadsw_class.h>
#include <mca/protocol/protocol_class.h>
#include <mca/protocol/protocol_pd_class.h>
#include <mca/protocol/protocol_qc_class.h>
#include <mca/smartchg/smart_chg_class.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include "inc/mca_quick_charger.h"

/*
 * Board projects (hwid platform_version) some decisions single out.  The
 * numbering is the hwid driver's own; only O9 has a name in mca_hwid.h.
 */
#define QC_HWID_PROJECT_EN_BUCK		HARDWARE_PROJECT_O9
#define QC_HWID_PROJECT_CURR_MONITOR	4
#define QC_HWID_PROJECT_FORCE_DONE_A	7
#define QC_HWID_PROJECT_FORCE_DONE_B	10
#define QC_HWID_PROJECT_BUCK_FULL	11
#define QC_HWID_PROJECT_VSTEP_CAP	7
/* projects 1, 2, 4 and 7 move on from the max-current stage after a while */
#define QC_HWID_CURR_MONITOR_MASK	0x96
#define QC_HWID_CURR_MONITOR_MAX	8

/* DTPT scales the stage current to 8/10 */
#define MCA_DTPT_MOLECULE_SCALE		8
#define MCA_DTPT_DENOM_SCALE		10
/* tenths: the flip cell's termination current, scaled to a charger target */
#define MCA_BASE_CLOSE_CURR_RATIO	13
#define MCA_EU_PPS_PEAK_TIME_S		120
#define MCA_PPS_BOOST_FCC_MAX		11000
#define MCA_PPS_FCC_MARGIN		200
#define MCA_QUICK_CHG_FAST_CHG_MAX_CURR	15600
#define MCA_QUICK_CHG_HIGH_TEMP_RESET	47
#define MCA_QUICK_CHG_VBAT_SECURE_MIN	4400

static int mca_quick_charge_parse_batt_info(struct mca_quick_charge_info *info, int mode);
static int mca_quick_charge_open_path(struct mca_quick_charge_info *info);

static int mca_quick_charge_msleep(int ms, struct mca_quick_charge_info *info)
{
	int count = ms / 10;
	int buck_online = 1;

	do {
		(void)mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
			STRATEGY_STATUS_TYPE_ONLINE, &buck_online);
		if (!info->online || info->force_stop || info->proc_data.type_chg ||
		    !buck_online || !info->sysfs_data.chg_enable) {
			mca_log_err("sleep fast return, online: %d, force_stop: %d, type_chg: %d, buck_online: %d, sysfs_chg_enable %d\n",
				info->online, info->force_stop, info->proc_data.type_chg,
				buck_online, info->sysfs_data.chg_enable);
			return -1;
		}
		usleep_range(9900, 11000);
	} while (--count);

	return 0;
}

static void mca_quick_charge_update_vbat(struct mca_quick_charge_info *info)
{
	if (strategy_class_fg_ops_get_voltage(&info->proc_data.vbat[FG_IC_MASTER]))
		return;

	if (info->batt_type <= MCA_BATTERY_TYPE_SINGLE)
		return;

	if (platform_fg_ops_get_volt(FG_IC_MASTER, &info->proc_data.parall_vbat[FG_IC_BASE]))
		return;

	(void)platform_fg_ops_get_volt(FG_IC_SLAVE, &info->proc_data.parall_vbat[FG_IC_FLIP]);
}

/* The gauges report discharge as positive microamps; keep charge as positive mA. */
static void mca_quick_charge_update_ibat(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;

	if (strategy_class_fg_ops_get_current(&proc->ibat[FG_IC_MASTER]))
		return;

	if (info->batt_type > MCA_BATTERY_TYPE_SINGLE &&
	    (platform_fg_ops_get_curr(FG_IC_MASTER, &proc->parall_ibat[FG_IC_BASE]) ||
	     platform_fg_ops_get_curr(FG_IC_SLAVE, &proc->parall_ibat[FG_IC_FLIP])))
		return;

	proc->ibat[FG_IC_MASTER] = -proc->ibat[FG_IC_MASTER] / 1000;
	proc->parall_ibat[FG_IC_BASE] = -proc->parall_ibat[FG_IC_BASE] / 1000;
	proc->parall_ibat[FG_IC_FLIP] = -proc->parall_ibat[FG_IC_FLIP] / 1000;
	proc->ibat_total = proc->ibat[FG_IC_MASTER];
}

static int mca_quick_charge_update_ibus(struct mca_quick_charge_info *info)
{
	int ibus[MCA_QUICK_CHG_CP_MODE_MAX] = { 0 };
	int role = info->proc_data.cur_work_cp;
	int buck_ibus = 0;

	if (role == MCA_QUICK_CHG_CP_DUAL) {
		if (platform_class_cp_get_bus_current(CP_ROLE_MASTER, &ibus[CP_ROLE_MASTER])) {
			info->proc_data.error_num[MCA_QUICK_CHG_CP_MASTER]++;
			return -1;
		}
		if (platform_class_cp_get_bus_current(CP_ROLE_SLAVE, &ibus[CP_ROLE_SLAVE])) {
			info->proc_data.error_num[MCA_QUICK_CHG_CP_SLAVE]++;
			return -1;
		}
	} else {
		if (role < 0 || role >= MCA_QUICK_CHG_CP_DUAL)
			return -1;
		if (platform_class_cp_get_bus_current(role, &ibus[role])) {
			info->proc_data.error_num[role]++;
			return -1;
		}
	}
	info->proc_data.ibus = ibus[CP_ROLE_MASTER] + ibus[CP_ROLE_SLAVE];

	if (!info->en_buck_parallel_chg && !info->support_base_flip && !info->base_flip_same)
		return 0;

	if (platform_class_buckchg_ops_get_bus_curr(MAIN_BUCK_CHARGER, &buck_ibus)) {
		mca_log_err("get buck ibus failed!\n");
		return -1;
	}
	info->proc_data.ibus += buck_ibus / 1000;

	return 0;
}

static int mca_qc_quick_charge_update_ibus(struct mca_quick_charge_info *info)
{
	int ibus = 0;

	(void)platform_class_cp_enable_adc(CP_ROLE_MASTER, true);
	if (platform_class_cp_get_bus_current(CP_ROLE_MASTER, &ibus)) {
		info->proc_data.error_num[MCA_QUICK_CHG_CP_MASTER]++;
		return -1;
	}
	info->proc_data.ibus = ibus;

	return 0;
}

static void mca_quick_charge_set_cp_default_fsw(struct mca_quick_charge_info *info)
{
	if (info->proc_data.cur_work_cp == MCA_QUICK_CHG_CP_DUAL) {
		(void)platform_class_cp_set_default_fsw(CP_ROLE_MASTER);
		(void)platform_class_cp_set_default_fsw(CP_ROLE_SLAVE);
	} else {
		(void)platform_class_cp_set_default_fsw(info->proc_data.cur_work_cp);
	}
}

static void mca_quick_charge_set_cp_mode_2_1(struct mca_quick_charge_info *info)
{
	if (info->proc_data.cur_work_cp == MCA_QUICK_CHG_CP_DUAL) {
		(void)platform_class_cp_set_mode(CP_ROLE_MASTER, CP_MODE_FORWARD_2_1);
		(void)platform_class_cp_set_mode(CP_ROLE_SLAVE, CP_MODE_FORWARD_2_1);
	} else {
		(void)platform_class_cp_set_mode(info->proc_data.cur_work_cp, CP_MODE_FORWARD_2_1);
	}
}

static bool stategy_quickchg_is_pdo_9v(struct mca_quick_charge_info *info)
{
	struct adapter_power_cap_info cap = { 0 };
	int i;

	if (info->proc_data.adp_type != XM_CHARGER_TYPE_PD &&
	    info->proc_data.adp_type != XM_CHARGER_TYPE_PPS &&
	    info->proc_data.adp_type != XM_CHARGER_TYPE_PD_VERIFY)
		return false;

	if (protocol_class_get_adapter_power_cap(ADAPTER_PROTOCOL_PD, &cap))
		return false;

	if (!cap.pdo_nums) {
		mca_log_info(" pd 2.0 pwr_cap.nums is null\n");
		return false;
	}

	for (i = 0; i < cap.pdo_nums && i < ADAPTER_POWER_CAP_MAX; i++) {
		if (cap.cap[i].max_voltage == MCA_QUICK_CHG_ADP_FIXED_9V &&
		    cap.cap[i].min_voltage == MCA_QUICK_CHG_ADP_FIXED_9V) {
			mca_log_info("pdo[%d] can support 9v: %d\n", i, 1);
			return true;
		}
	}

	return false;
}

static int mca_quick_charge_req_pd_fixed_volt(struct mca_quick_charge_info *info)
{
	int volt = MCA_QUICK_CHG_ADP_DEFAULT_VOLT;

	if (stategy_quickchg_is_pdo_9v(info) && !info->trigger_antr_burn &&
	    !info->shutdown_mode)
		volt = MCA_QUICK_CHG_ADP_FIXED_9V;

	(void)protocol_class_pd_set_fixed_volt(TYPEC_PORT_0, volt);

	return volt;
}

static int mca_quick_charge_req_adp_volt_and_cur(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;

	mca_log_info("req adp volt=%d, cur=%d\n", proc->cur_adp_volt, proc->cur_adp_cur);

	if (proc->max_adp_curr <= 0 || proc->max_adp_volt <= 0)
		return -1;

	proc->cur_adp_cur = min(proc->cur_adp_cur, proc->max_adp_curr);
	proc->cur_adp_volt = min(proc->cur_adp_volt, proc->max_adp_volt);

	if (info->is_eu_model && proc->adp_type == XM_CHARGER_TYPE_PPS &&
	    info->support_eu_power && info->eu_vbus_max && info->eu_ibus_max) {
		proc->cur_adp_cur = min(proc->cur_adp_cur, info->eu_ibus_max);
		proc->cur_adp_volt = min(proc->cur_adp_volt, info->eu_vbus_max);
		mca_log_err("eu pps %d, cur req adp volt=%d, cur=%d\n",
			info->is_eu_model, proc->cur_adp_volt, proc->cur_adp_cur);
	}

	if (proc->cur_adp_volt < proc->min_adp_volt) {
		mca_log_err("volt req too low, return err\n");
		return -1;
	}

	switch (proc->adp_type) {
	case XM_CHARGER_TYPE_HVDCP3_B:
	case XM_CHARGER_TYPE_HVDCP3P5:
		(void)protocol_class_qc_set_volt(ADAPTER_PROTOCOL_QC, proc->cur_adp_volt);
		break;
	case XM_CHARGER_TYPE_PPS:
	case XM_CHARGER_TYPE_PD_VERIFY:
		if (!info->pd_switch_to_pmic) {
			(void)protocol_class_set_adapter_volt_and_curr(proc->cur_protocol,
				proc->cur_adp_volt, proc->cur_adp_cur);
			break;
		}
		mca_log_err("PD2.0 REQUEST VOLT = %d\n", mca_quick_charge_req_pd_fixed_volt(info));
		break;
	case XM_CHARGER_TYPE_PD:
		mca_log_err("PD2.0 REQUEST VOLT = %d\n", mca_quick_charge_req_pd_fixed_volt(info));
		break;
	default:
		break;
	}

	return 0;
}

/*
 * Let the buck charger carry part of the load beside the pump once the bus
 * current is high enough, and take it back once the current has dropped.
 */
static void strategy_quickchg_enable_buck_charging(struct mca_quick_charge_info *info,
	int buck_icl_val, int buck_fcc_val, bool enable)
{
	static bool enable_once;
	static bool disable_once;
	bool cp_enabled = false;
	int ibus;

	(void)platform_class_cp_get_charging_enabled(info->proc_data.cur_work_cp, &cp_enabled);
	(void)mca_quick_charge_update_ibus(info);
	ibus = info->proc_data.ibus;

	if (enable && cp_enabled && !enable_once && ibus > 2500) {
		enable_once = true;
		disable_once = true;
		(void)platform_class_buckchg_ops_set_buck_fsw(MAIN_BUCK_CHARGER, 18000);
		(void)mca_vote(info->input_suspend_voter, "wire_qc", false, 0);
		(void)mca_vote_override(info->buck_input_voter, "icl_limit", true, buck_icl_val);
		(void)mca_vote_override(info->buck_charge_curr_voter, "fcc_limit", true, buck_fcc_val);
		mca_log_err("enable buck parallel charging! ibus :%d\n", ibus);
	} else if (!enable && disable_once && ibus >= 1 && ibus <= 3849) {
		disable_once = false;
		enable_once = false;
		(void)platform_class_buckchg_ops_set_buck_fsw(MAIN_BUCK_CHARGER, 5000);
		(void)mca_vote(info->input_suspend_voter, "wire_qc", true, 1);
		(void)mca_vote_override(info->buck_input_voter, "icl_limit", false, buck_icl_val);
		(void)mca_vote_override(info->buck_charge_curr_voter, "fcc_limit", false, buck_fcc_val);
		mca_log_err("disable buck parallel charging!, ibus: %d\n", ibus);
	}
}

/* Bring the bus back under the 12V the input stage is rated for. */
static void mca_quick_charge_avoid_vusb_ovp(struct mca_quick_charge_info *info, int sleep_ms)
{
	int vbus = 0;
	int i;

	for (i = 0; i < MCA_QUICK_CHG_STOP_REQ_COUNT; i++) {
		(void)platform_class_cp_get_bus_voltage(CP_ROLE_MASTER, &vbus);
		mca_log_info("avoid vusb_ovp. cp_vbus: %d\n", vbus);
		if (vbus < MCA_QUICK_CHG_VBUS_OK_HIGH_TH)
			break;

		if (mca_quick_charge_req_adp_volt_and_cur(info))
			mca_log_err("stop req volt failed\n");

		(void)platform_class_cp_get_bus_voltage(CP_ROLE_MASTER, &vbus);
		mca_log_info("avoid vusb_ovp. after request, cp_vbus: %d\n", vbus);
		if (vbus < MCA_QUICK_CHG_VBUS_OK_HIGH_TH)
			break;

		(void)mca_quick_charge_msleep(sleep_ms, info);
	}
}

static void mca_quick_charge_stop_charging(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int jeita_vterm;

	mca_log_err("stop charge\n");
	(void)platform_class_cp_set_charging_enable(CP_ROLE_MASTER, false);
	(void)platform_class_cp_set_charging_enable(CP_ROLE_SLAVE, false);
	if (info->vfc_para.support_cp_vfc)
		cancel_delayed_work_sync(&info->vfc_work);
	mca_quick_charge_set_cp_default_fsw(info);

	info->check_vbat_ov = false;
	info->pmih_fcc = 0;
	proc->cur_adp_volt = info->trigger_antr_burn ? MCA_QUICK_CHG_ADP_DEFAULT_VOLT :
		MCA_QUICK_CHG_ADP_FIXED_9V;
	proc->cur_adp_cur = MCA_QUICK_CHG_OPEN_PATH_CURR;
	info->stop_pmic = false;
	proc->stop_charging = false;
	proc->dual_cp_count = 0;
	proc->count = 0;
	memset(&proc->ibus_queue, 0, sizeof(proc->ibus_queue));
	info->pd_switch_to_pmic = true;

	if ((proc->adp_type == XM_CHARGER_TYPE_PPS ||
	     proc->adp_type == XM_CHARGER_TYPE_PD_VERIFY) &&
	    info->pmic_chg_need_fixed_volt) {
		(void)protocol_class_pd_set_fixed_volt(TYPEC_PORT_0, MCA_QUICK_CHG_ADP_DEFAULT_VOLT);
		if (stategy_quickchg_is_pdo_9v(info) && !info->trigger_antr_burn)
			(void)protocol_class_pd_set_fixed_volt(TYPEC_PORT_0,
				MCA_QUICK_CHG_ADP_FIXED_9V);
	} else {
		mca_quick_charge_avoid_vusb_ovp(info, 300);
	}

	mca_quick_charge_set_cp_mode_2_1(info);

	if (info->en_buck_parallel_chg)
		strategy_quickchg_enable_buck_charging(info, 0, 0, false);

	mca_log_err("charge_flag = %d\n", proc->charge_flag);
	if (proc->charge_flag == MCA_QUICK_CHG_STS_CHARGING &&
	    info->support_pmic_vterm_dynamics_adjust) {
		jeita_vterm = mca_get_client_vote(info->buck_vterm_voter, "jeita");
		(void)mca_vote_override(info->buck_vterm_voter, "pmic_temp_term", true,
			jeita_vterm + 30);
		(void)mca_vote_override(info->buck_input_voter, "pmic_temp_term", true, 500);
		(void)mca_quick_charge_msleep(200, info);
	}

	(void)mca_vote(info->input_suspend_voter, "wire_qc", false, 0);
	if (info->support_base_flip || info->base_flip_same) {
		(void)mca_vote_override(info->buck_input_voter, "icl_limit", false, 0);
		(void)mca_vote_override(info->buck_charge_curr_voter, "fcc_limit", false, 0);
		if (proc->base_batt_close) {
			jeita_vterm = mca_get_client_vote(info->buck_vterm_voter, "jeita");
			(void)platform_class_buckchg_ops_set_term_volt(MAIN_BUCK_CHARGER,
				jeita_vterm + 60);
			(void)mca_vote(info->flip_fcc_voter, "base_close_curr", true,
				proc->base_close_curr);
			proc->base_batt_close = false;
		}
		(void)mca_rerun_election(info->flip_fcc_voter);
	}

	(void)platform_class_cp_enable_adc(CP_ROLE_MASTER, false);
	(void)platform_class_cp_enable_adc(CP_ROLE_SLAVE, false);
	proc->cur_stage[FG_IC_MASTER] = 0;
	proc->cur_stage[FG_IC_SLAVE] = 0;
	proc->secure_info.max_vbatt = 0;
	proc->secure_info.secure_cur = 0;

	if (info->pmic_single_cp_chg) {
		(void)mca_vote_override(info->buck_vterm_voter, "single_cp_pmic", false, 0);
		(void)mca_rerun_election(info->buck_vterm_voter);
		info->final_vterm = 0;
		info->last_override_vterm = 0;
	}

	if (proc->charge_flag != MCA_QUICK_CHG_STS_CHARGE_DONE)
		proc->charge_flag = MCA_QUICK_CHG_STS_NO_CHARGING;
}

/* Stop the pump and release the PMIC term overrides stop_charging may set. */
static void mca_quick_charge_exit_charging(struct mca_quick_charge_info *info)
{
	cancel_delayed_work_sync(&info->monitor_work);
	mca_quick_charge_stop_charging(info);
	if (info->support_pmic_vterm_dynamics_adjust) {
		(void)mca_vote_override(info->buck_vterm_voter, "pmic_temp_term", false, 0);
		(void)mca_vote_override(info->buck_input_voter, "pmic_temp_term", false, 0);
	}
}

/*
 * Hold the EU PPS adapter to two thirds of its rating once it has run at its
 * rated current for two minutes, so it does not disconnect on overload.
 */
static void mca_eu_pps_anti_disconnection_strategy(struct mca_quick_charge_info *info)
{
	static bool start_flag;
	static bool trigger;
	static int count;
	static int timer_start;
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	time64_t now;

	if (!info->is_eu_model)
		return;

	if (!info->online) {
		start_flag = false;
		trigger = false;
		count = 0;
		proc->max_adap_ibat = 0;
		mca_log_info("reset param\n");
		return;
	}

	if (proc->adp_type != XM_CHARGER_TYPE_PPS)
		return;

	if (proc->ibus >= proc->max_adp_curr - 200) {
		count = 0;
		if (trigger) {
			mca_log_info("eu_pps has trigged\n");
			return;
		}

		now = ktime_get_coarse_boottime() / NSEC_PER_SEC;
		if (start_flag) {
			if (now - timer_start < MCA_EU_PPS_PEAK_TIME_S)
				return;
			proc->max_adap_ibat = proc->max_ibat_final * 2 / 3;
			trigger = true;
			mca_log_info("trigger:%d max_adap_ibat:[%d] max_ibat_final[%d]\n",
				trigger, proc->max_adap_ibat, proc->max_ibat_final);
			return;
		}

		timer_start = (int)now;
		mca_log_info("timer_start: %lld\n", (long long)now);
		start_flag = true;
		return;
	}

	if (start_flag && count > 2) {
		start_flag = false;
		count = 0;
		mca_log_info(" count %d reset timer for eu_pps max ibus\n", 0);
	} else if (start_flag) {
		count++;
		mca_log_info(" count %d \n", count);
	}
}

static int mca_quick_charge_adjust_adapter_mode(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct adapter_power_cap_info cap = { 0 };
	struct adapter_power_cap *pcap;
	bool series = info->batt_type == MCA_BATTERY_TYPE_SERIES;
	int div1_min_volt = series ? 7200 : 3600;
	int mode = 0;
	int power;
	int i;

	if (protocol_class_get_adapter_power_cap(proc->cur_protocol, &cap))
		return -1;

	memset(proc->adp_info, 0, sizeof(proc->adp_info));
	for (i = 0; i < cap.pdo_nums && i < ADAPTER_CAP_MAX_NR; i++) {
		pcap = &cap.cap[i];
		mca_log_info("volt max %d,volt min %d,cur %d\n",
			pcap->max_voltage, pcap->min_voltage, pcap->max_current);
		if (!pcap->max_voltage)
			break;

		proc->adp_info[i].adp_mode = 0;
		memcpy(&proc->adp_info[i].cap_info, pcap, sizeof(*pcap));

		if (pcap->max_voltage >= (18000 << series) &&
		    pcap->min_voltage <= (12000 << series) &&
		    (proc->adp_type == XM_CHARGER_TYPE_PD_VERIFY || info->is_eu_model)) {
			mode |= MCA_QUICK_CHG_MODE_DIV_4;
			proc->adp_info[i].adp_mode = MCA_QUICK_CHG_MODE_DIV_4;
			power = pcap->max_current * min(pcap->max_voltage, 21000) / 1000;
			proc->adp_mode_power[CHG_MODE_DIV4] =
				max(proc->adp_mode_power[CHG_MODE_DIV4], power);
		}

		if (pcap->max_voltage >= (9000 << series) &&
		    pcap->min_voltage <= (6000 << series)) {
			mode |= MCA_QUICK_CHG_MODE_DIV_2;
			proc->adp_info[i].adp_mode |= MCA_QUICK_CHG_MODE_DIV_2;
			power = pcap->max_current * min(pcap->max_voltage, 11000) / 1000;
			proc->adp_mode_power[CHG_MODE_DIV2] =
				max(proc->adp_mode_power[CHG_MODE_DIV2], power);
		}

		if (pcap->max_voltage >= (4500 << series) &&
		    pcap->min_voltage <= div1_min_volt) {
			mode |= MCA_QUICK_CHG_MODE_DIV_1;
			proc->adp_info[i].adp_mode |= MCA_QUICK_CHG_MODE_DIV_1;
			power = pcap->max_current * min(pcap->max_voltage, 5500) / 1000;
			proc->adp_mode_power[CHG_MODE_DIV1] =
				max(proc->adp_mode_power[CHG_MODE_DIV1], power);
		}

		mca_log_info("index %d, max_power %d %d %d\n", i,
			proc->adp_mode_power[CHG_MODE_DIV1],
			proc->adp_mode_power[CHG_MODE_DIV2],
			proc->adp_mode_power[CHG_MODE_DIV4]);
	}

	if (!mode)
		return -1;

	proc->adp_mode = mode;

	return 0;
}

/*
 * Pick, per division the board and adapter both support, the adapter
 * operating point giving the most battery current, then the division whose
 * best point gives the most; a tie goes to the lower division.
 */
static void mca_quick_charge_select_cur_work_mode(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	static const int ratio[CHG_MODE_MAX] = { 1, 2, 4 };
	int ibat_max[CHG_MODE_MAX] = { 0 };
	int adp_curr[CHG_MODE_MAX] = { 0 };
	struct adapter_power_cap *cap;
	int i, mode, modes, cur, adp_index;

	for (i = 0; i < ADAPTER_CAP_MAX_NR; i++) {
		cap = &proc->adp_info[i].cap_info;
		if (!cap->max_voltage)
			break;

		modes = info->cur_support_mode & proc->adp_info[i].adp_mode;
		for (mode = CHG_MODE_DIV4; mode >= CHG_MODE_DIV1; mode--) {
			if (!(modes & BIT(mode)))
				continue;
			cur = min(cap->max_current * ratio[mode], info->div_max_curr[mode]);
			if (ibat_max[mode] <= cur && adp_curr[mode] <= cap->max_current) {
				proc->adp_info_index[mode] = i;
				ibat_max[mode] = cur;
				adp_curr[mode] = cap->max_current;
			}
		}
	}

	for (mode = CHG_MODE_DIV1; mode < CHG_MODE_MAX; mode++)
		mca_log_info("div%ld ibat_max %d index %d", (long)BIT(mode), ibat_max[mode],
			proc->adp_info_index[mode]);

	mode = ibat_max[CHG_MODE_DIV1] < ibat_max[CHG_MODE_DIV2] ? CHG_MODE_DIV2 : CHG_MODE_DIV1;
	if (ibat_max[CHG_MODE_DIV4] > ibat_max[mode])
		mode = CHG_MODE_DIV4;

	adp_index = proc->adp_info_index[mode];
	proc->max_ibat_final = ibat_max[mode];
	mca_log_info("index: %d, adp_index: %d\n", mode, adp_index);
	info->cp_work_mode.cur_mode = mode;
	info->cp_work_mode.default_mode = mode;
	proc->work_mode = BIT(mode);

	if (adp_index < 0 || adp_index >= ADAPTER_CAP_MAX_NR)
		return;

	cap = &proc->adp_info[adp_index].cap_info;
	proc->min_adp_volt = cap->min_voltage;
	proc->max_adp_volt = cap->max_voltage;
	proc->max_adp_curr = cap->max_current;
	proc->max_power = cap->max_current * cap->max_voltage / 1000000;
	mca_log_info("min_adp_volt: %d, max_adp_volt: %d, max_adp_curr: %d, max_power: %d\n",
		proc->min_adp_volt, proc->max_adp_volt, proc->max_adp_curr, proc->max_power);
}

static void mca_quick_charge_update_work_mode_para(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct adapter_power_cap *cap;
	int zimi_cypress_flag = 0;
	int adp_index = -1;
	int mode;

	switch (proc->work_mode) {
	case MCA_QUICK_CHG_MODE_DIV_4:
		mode = CHG_MODE_DIV4;
		proc->ratio = 4;
		proc->cur_cp_mode = CP_MODE_FORWARD_4_1;
		break;
	case MCA_QUICK_CHG_MODE_DIV_2:
		mode = CHG_MODE_DIV2;
		proc->ratio = 2;
		proc->cur_cp_mode = CP_MODE_FORWARD_2_1;
		break;
	case MCA_QUICK_CHG_MODE_DIV_1:
		mode = CHG_MODE_DIV1;
		proc->ratio = 1;
		proc->cur_cp_mode = CP_MODE_FORWARD_1_1;
		break;
	default:
		mode = -1;
		break;
	}

	if (mode >= 0) {
		proc->delta_volt = info->div_delta_volt[mode];
		proc->single_curr = info->div_single_curr[mode];
		proc->max_curr = info->div_max_curr[mode];
		proc->delta_ibat = info->div_delta_ibat[mode];
		proc->multi_ibus_th = info->multi_ibus_th[mode];
		proc->again_multi_ibus_th = info->again_multi_ibus_th[mode];
		proc->open_path = info->open_path_th[mode];
		proc->ibus_inc = info->ibus_inc_hysteresis[mode];
		proc->ibus_dec = info->ibus_dec_hysteresis[mode];
		proc->thermal_cur = info->thermal_cur[mode];
		proc->cp_path_enable = info->sysfs_data.cp_path_enable[mode];
		proc->ibus_compensation = info->ibus_compensation[mode];
		adp_index = proc->adp_info_index[mode];
		if (adp_index >= 0 && adp_index < ADAPTER_CAP_MAX_NR) {
			cap = &proc->adp_info[adp_index].cap_info;
			proc->min_adp_volt = cap->min_voltage;
			proc->max_adp_volt = cap->max_voltage;
			proc->max_adp_curr = cap->max_current;
		}
	}

	/*
	 * Some adapters cannot be run at the top of their advertised range;
	 * back off 1V there.
	 */
	(void)protocol_class_pd_get_zimi_cypress_flag(TYPEC_PORT_0, &zimi_cypress_flag);
	if (proc->min_adp_volt != proc->max_adp_volt &&
	    (zimi_cypress_flag == 1 ||
	     (!zimi_cypress_flag && proc->adp_type == XM_CHARGER_TYPE_PPS &&
	      proc->max_adp_volt > 10000)))
		proc->max_adp_volt -= 1000;

	mca_log_info("delta_volt: %d, single_curr: %d, max_curr: %d, work_mode: %d, zimi_cypress_flag: %d\n",
		proc->delta_volt, proc->single_curr, proc->max_curr, proc->work_mode,
		zimi_cypress_flag);
	mca_log_info("adp_index: %d, adp_min_volt: %d, adp_max_volt: %d, adp_max_curr: %d\n",
		adp_index, proc->min_adp_volt, proc->max_adp_volt, proc->max_adp_curr);
}

static int mca_quick_charge_jump_stage(int stage, struct mca_quick_charge_volt_para_info *volt_info)
{
	if (!volt_info->stage_para)
		return stage;

	if (stage >= volt_info->volt_para_size * 2)
		return stage - 1;

	if (volt_info->stage_para[stage])
		return mca_quick_charge_jump_stage(stage + 1, volt_info);

	return stage;
}

/*
 * Stage 2n is constant current up to step n's voltage, 2n + 1 constant
 * voltage there until the current falls to step n's minimum.  The last
 * step's voltage is lowered by the smart-charge float voltage trim.
 */
static int mca_quick_charge_calc_stage(struct mca_quick_charge_info *info,
	struct mca_quick_charge_volt_para_info *volt_info, int vbat, int ibat)
{
	struct mca_quick_charge_volt_para *para;
	int stage = volt_info->volt_para_size * 2;
	int i, voltage;

	for (i = volt_info->volt_para_size - 1; i >= 0; i--) {
		para = &volt_info->volt_para[i];
		voltage = para->voltage;
		if (i == volt_info->volt_para_size - 1)
			voltage -= info->smartchg_data.delta_fv;

		if (vbat >= voltage) {
			if (ibat > para->current_min)
				stage--;
			return stage;
		}
		stage -= 2;
		mca_log_info("cc_max_volatge %d cv_min_current %d stage %d",
			voltage, para->current_min, stage);
	}

	return 0;
}

static void mca_quick_charge_select_stage(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	struct mca_quick_charge_volt_para_info *volt_info;
	bool cp_enabled = false;
	time64_t now;
	int i, stage, new_stage, ffc;

	mca_quick_charge_update_vbat(info);
	mca_quick_charge_update_ibat(info);
	if (!info->is_qcom_qc)
		(void)mca_quick_charge_update_ibus(info);
	else
		(void)mca_qc_quick_charge_update_ibus(info);

	for (i = 0; i < FG_IC_MAX; i++) {
		volt_info = proc->cur_volt_para[i];
		if (!volt_info)
			break;

		stage = mca_quick_charge_calc_stage(info, volt_info, proc->vbat[i], proc->ibat[i]);
		now = ktime_get_coarse_boottime() / NSEC_PER_SEC;
		(void)platform_class_cp_get_charging_enabled(CP_ROLE_MASTER, &cp_enabled);
		/*
		 * Where the board asks for it, the max-current stage is left
		 * once it has been held for curr_monitor_time_s.
		 */
		if (info->fastchg_temp_flag && proc->cur_stage[i] < 2 && cp_enabled &&
		    proc->ffc_flag && hwid && hwid->platform_version < QC_HWID_CURR_MONITOR_MAX &&
		    (BIT(hwid->platform_version) & QC_HWID_CURR_MONITOR_MASK) &&
		    now - info->time_start >= info->curr_monitor_time_s) {
			mca_log_info("max current duration too long, switch next stage\n");
			stage = 2;
		}
		mca_log_info("fastchg_temp_flag %d, stage %d, cp_en %d, time_now %lld, time_start %lld, time_diff %lld\n",
			info->fastchg_temp_flag, stage, cp_enabled, (long long)now,
			(long long)info->time_start, (long long)(now - info->time_start));

		new_stage = mca_quick_charge_jump_stage(stage, volt_info);
		ffc = strategy_class_fg_get_fastcharge();
		if (proc->cur_stage[i] < new_stage || ffc != proc->ffc_flag || proc->zone_changed) {
			proc->zone_changed = 0;
			proc->cur_stage[i] = new_stage;
		}
	}

	if (!info->support_base_flip)
		return;

	mca_log_info("get base and flip stage for ocp");
	for (i = 0; i < FG_SITE_MAX; i++) {
		volt_info = proc->cur_volt_paraller[i];
		if (!volt_info)
			continue;

		stage = mca_quick_charge_calc_stage(info, volt_info, proc->parall_vbat[i],
			proc->parall_ibat[i]);
		new_stage = mca_quick_charge_jump_stage(stage, volt_info);
		ffc = strategy_class_fg_get_fastcharge();
		if (proc->parall_cur_stage[i] < new_stage || ffc != proc->ffc_flag ||
		    proc->parall_zone_changed[i]) {
			proc->parall_cur_stage[i] = new_stage;
			proc->parall_zone_changed[i] = 0;
		}
		mca_log_info("parall_cur_stage[%d] %d", i, proc->parall_cur_stage[i]);
	}
}

static int mca_quick_charge_can_tbat_do_charge(struct mca_quick_charge_info *info, bool init)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct mca_quick_charge_batt_para_info *batt_para = &info->batt_para[FG_IC_MASTER];
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	struct mca_quick_charge_temp_para_info *temp_info = NULL;
	struct mca_quick_charge_temp_para *tp = NULL;
	bool found = false;
	int temp = 0;
	int ret, i;

	if (init && proc->adp_type == XM_CHARGER_TYPE_PD_VERIFY) {
		proc->to_pd_verify = false;
		mca_log_info("PD VERIFY SUCCESS\n");
	}

	ret = strategy_class_fg_ops_get_temperature(&temp);
	if (ret) {
		mca_log_err("get %d batt temp fail\n", 0);
		return ret;
	}
	temp /= 10;
	mca_log_info("temp=%d\n", temp);

	for (i = 0; i < batt_para->temp_para_size; i++) {
		temp_info = &batt_para->temp_info[i];
		if (temp_info->temp_para.temp_low <= temp && temp < temp_info->temp_para.temp_high) {
			found = true;
			break;
		}
	}
	if (temp_info)
		tp = &temp_info->temp_para;

	if (info->base_flip_same) {
		mca_quick_charge_update_ibat(info);
		if (tp && temp > MCA_QUICK_CHG_HIGH_TEMP_RESET && tp->max_current < proc->ibat_total) {
			mca_log_err("High temperature, high battery current - CP reset\n");
			return -1;
		}
	}

	if (!found) {
		mca_log_err("%d can not find temp_para\n", 0);
		return -1;
	}
	mca_log_info("temp_para_index new %d, old %d\n", i, proc->temp_para_index[FG_IC_MASTER]);

	if (!tp->max_current) {
		mca_log_info("%d temp is out of range\n", 0);
		mca_log_info("max_current is 0, should set fast charge to false\n");
		proc->ffc_flag = 0;
		(void)strategy_class_fg_set_fastcharge(false);
		if (!init) {
			if (proc->temp_para_index[FG_IC_MASTER] < i)
				proc->temp_hys_en = MCA_QUICK_TEMP_HYS_HIGH;
			if (i < proc->temp_para_index[FG_IC_MASTER])
				proc->temp_hys_en = MCA_QUICK_TEMP_HYS_LOW;
		}
		return -1;
	}

	if (!init) {
		if (i == proc->temp_para_index[FG_IC_MASTER] || !proc->temp_max_cur[FG_IC_MASTER])
			return 0;

		if (proc->temp_para_index[FG_IC_MASTER] < i) {
			if (temp <= tp->temp_low + tp->low_temp_hysteresis) {
				mca_log_info("temperature not low enough, zone not change\n");
				return 0;
			}
		} else if (temp >= tp->temp_high - tp->high_temp_hysteresis) {
			mca_log_info("temperature not high enough, zone not change\n");
			return 0;
		}
	} else {
		if (proc->temp_hys_en == MCA_QUICK_TEMP_HYS_LOW) {
			if (temp <= tp->temp_low + tp->low_temp_hysteresis) {
				mca_log_info("hys high not satisfied\n");
				return -1;
			}
		} else if (proc->temp_hys_en == MCA_QUICK_TEMP_HYS_HIGH) {
			if (temp >= tp->temp_high - tp->high_temp_hysteresis) {
				mca_log_info("hys high not satisfied\n");
				return -1;
			}
		}
		proc->temp_hys_en = MCA_QUICK_TEMP_HYS_DIS;
	}

	proc->temp_para_index[FG_IC_MASTER] = i;
	proc->zone_changed = 1;
	proc->temp_max_cur[FG_IC_MASTER] = tp->max_current;
	if (proc->adp_type == XM_CHARGER_TYPE_PD_VERIFY &&
	    temp_info->volt_ffc_info.volt_para_size && !info->fcc_too_low) {
		mca_log_info("select ffc volt para\n");
		proc->ffc_flag = 1;
		proc->temp_max_fv[FG_IC_MASTER] = tp->ffc_max_fv;
		proc->cur_volt_para[FG_IC_MASTER] = &temp_info->volt_ffc_info;
	} else if (!temp_info->volt_info.volt_para_size) {
		mca_log_info("no volt para\n");
		proc->ffc_flag = 0;
		info->fcc_too_low = false;
		return -1;
	} else {
		mca_log_info("select normal volt para\n");
		proc->ffc_flag = 0;
		proc->temp_max_fv[FG_IC_MASTER] = tp->normal_max_fv;
		proc->cur_volt_para[FG_IC_MASTER] = &temp_info->volt_info;
		info->fcc_too_low = false;
	}

	if (hwid && (hwid->platform_version == QC_HWID_PROJECT_CURR_MONITOR ||
		     hwid->country_version == CountryCN) &&
	    info->support_curr_monitor && info->curr_monitor_time_s &&
	    tp->max_current == MCA_QUICK_CHG_FAST_CHG_MAX_CURR) {
		info->fastchg_temp_flag = true;
		mca_log_info("enter fast charging temp range\n");
	} else {
		info->fastchg_temp_flag = false;
	}

	mca_log_info("temp_para_index=%d, max_cur=%d, max_fv = %d\n", i, tp->max_current,
		proc->temp_max_fv[FG_IC_MASTER]);

	return 0;
}

static int mca_quick_get_parallel_volt_para(struct mca_quick_charge_info *info, int fg_index,
	bool init)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct mca_quick_charge_batt_para_info *batt_para = &info->base_flip_para[fg_index];
	struct mca_quick_charge_temp_para_info *temp_info;
	struct mca_quick_charge_temp_para *tp;
	int temp = 0;
	int i, old;

	if (platform_fg_ops_get_temp(fg_index, &temp))
		mca_log_err("get flip batt temp fail\n");
	temp /= 10;
	mca_log_info("fg[%d]_temp = %d\n", fg_index, temp);

	for (i = 0; i < batt_para->temp_para_size; i++) {
		temp_info = &batt_para->temp_info[i];
		tp = &temp_info->temp_para;
		if (tp->temp_low > temp || temp >= tp->temp_high)
			continue;

		old = proc->parall_temp_para_index[fg_index];
		mca_log_info("fg_index %d, temp_para_index new %d, old %d\n", fg_index, i, old);
		if (!tp->max_current) {
			mca_log_info("%d temp is out of range\n", fg_index);
			if (!init) {
				if (old < i)
					proc->parallel_temp_hys_en[fg_index] = MCA_QUICK_TEMP_HYS_HIGH;
				if (i < old)
					proc->parallel_temp_hys_en[fg_index] = MCA_QUICK_TEMP_HYS_LOW;
			}
			return -1;
		}

		if (!init) {
			if (old == i)
				return 0;
			/* the zone is judged against the gauge's own (not the site's) index */
			if (proc->temp_para_index[fg_index] < i) {
				if (temp <= tp->temp_low + tp->low_temp_hysteresis) {
					mca_log_info("temperature not low enough, zone not change\n");
					return 0;
				}
			} else if (i < proc->temp_para_index[fg_index] &&
				   temp >= tp->temp_high - tp->high_temp_hysteresis) {
				mca_log_info("temperature not high enough, zone not change\n");
				return 0;
			}
		} else {
			if (proc->parallel_temp_hys_en[fg_index] == MCA_QUICK_TEMP_HYS_LOW) {
				if (temp <= tp->temp_low + tp->low_temp_hysteresis) {
					mca_log_info("hys high not satisfied\n");
					return -1;
				}
			} else if (proc->parallel_temp_hys_en[fg_index] == MCA_QUICK_TEMP_HYS_HIGH &&
				   temp >= tp->temp_high - tp->high_temp_hysteresis) {
				mca_log_info("hys high not satisfied\n");
				return -1;
			}
			proc->parallel_temp_hys_en[fg_index] = MCA_QUICK_TEMP_HYS_DIS;
		}

		proc->parall_temp_para_index[fg_index] = i;
		proc->parall_zone_changed[fg_index] = 1;
		if (proc->adp_type == XM_CHARGER_TYPE_PD_VERIFY &&
		    temp_info->volt_ffc_info.volt_para_size &&
		    !info->parallel_fcc_too_low[fg_index]) {
			mca_log_info("select ffc volt para\n");
			proc->cur_volt_paraller[fg_index] = &temp_info->volt_ffc_info;
			return 0;
		}

		if (temp_info->volt_info.volt_para_size) {
			mca_log_info("select normal volt para\n");
			proc->cur_volt_paraller[fg_index] = &temp_info->volt_info;
			info->parallel_fcc_too_low[fg_index] = false;
			return 0;
		}

		mca_log_err("no volt para\n");
		info->parallel_fcc_too_low[fg_index] = false;
		return -1;
	}

	mca_log_err("%d can not find temp_para\n", fg_index);
	return -1;
}

static int mca_quick_charge_parallel_judge_temp(struct mca_quick_charge_info *info)
{
	return mca_quick_get_parallel_volt_para(info, FG_IC_BASE, true) |
		mca_quick_get_parallel_volt_para(info, FG_IC_FLIP, true);
}

static int mca_quick_charge_select_parallel_volt_para(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int ret;

	if (!proc->parallel_to_pd_verify)
		return mca_quick_get_parallel_volt_para(info, FG_IC_BASE, false) |
			mca_quick_get_parallel_volt_para(info, FG_IC_FLIP, false);

	ret = mca_quick_get_parallel_volt_para(info, FG_IC_BASE, true) |
		mca_quick_get_parallel_volt_para(info, FG_IC_FLIP, true);
	if (proc->adp_type == XM_CHARGER_TYPE_PD_VERIFY) {
		proc->parallel_to_pd_verify = false;
		mca_log_info("PD VERIFY SUCCESS\n");
	}

	return ret;
}

/* The thermal limit of the pump(s) this board charges through. */
static int mca_quick_charge_judge_thermal_vote(struct mca_quick_charge_info *info)
{
	int *thermal_cur = info->proc_data.thermal_cur;
	int cur;

	if (info->cp_type == MCA_CP_TYPE_SINGLE)
		cur = thermal_cur[MCA_QUICK_CHG_CH_SINGLE];
	else if (info->cp_type == MCA_CP_TYPE_PARALLEL)
		cur = thermal_cur[MCA_QUICK_CHG_CH_MULTI];
	else
		cur = 0;

	mca_log_info("info->thermal_cur: %d %d\n", thermal_cur[MCA_QUICK_CHG_CH_SINGLE],
		thermal_cur[MCA_QUICK_CHG_CH_MULTI]);
	if (cur > 0 && cur < info->cp_switch_pmic_th) {
		mca_log_info("skip cp for thermal");
		return -1;
	}

	return 0;
}

static int mca_quick_charge_tune_vbus(struct mca_quick_charge_info *info, int *stat)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int target = proc->cur_adp_volt;
	int vbus = 0;
	int errorhl_stat = 0;
	int count, step;

	for (count = 0; count < MCA_QUICK_CHG_TUNE_VBUS_COUNT; count++) {
		(void)platform_class_cp_get_bus_voltage(proc->cur_work_cp, &vbus);
		if (platform_class_cp_get_errorhl_stat(proc->cur_work_cp, &errorhl_stat))
			return -1;
		mca_log_info("open path tune vbus: %d, pmid_errorhl_stat: %d\n", vbus, errorhl_stat);

		if (vbus >= target - MCA_QUICK_CHG_TUNE_VBUS_LOW_MARGIN &&
		    vbus <= target + MCA_QUICK_CHG_TUNE_VBUS_HIGH_MARGIN) {
			mca_log_info("tune vbus OK: %d\n", vbus);
			*stat = XM_VBUS_TUNE_OK;
			goto out;
		}

		step = proc->adp_type == XM_CHARGER_TYPE_HVDCP3_B ?
			MCA_QUICK_CHG_QC3B_DEFAULT_VSTEP : MCA_QUICK_CHG_PPS_OPEN_PATH_VSTEP;
		if (vbus >= target - MCA_QUICK_CHG_TUNE_VBUS_LOW_MARGIN)
			step = -step;
		proc->cur_adp_volt += step;
		(void)protocol_class_set_adapter_volt_and_curr(proc->cur_protocol,
			proc->cur_adp_volt, proc->cur_adp_cur);
		if (mca_quick_charge_msleep(300, info))
			return -1;
	}
	*stat = XM_VBUS_TUNE_FAIL;

out:
	mca_log_info("tune vbus end vbus_stat: %d\n", *stat);
	return 0;
}

static int mca_quick_charge_open_path(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int stat = XM_VBUS_TUNE_INIT;
	bool cp_enabled = false;
	int ibus = 0, vbat = 0, vbus = 0;
	int ret, ret_mode, ret_en, volt, i;

	ret = platform_class_cp_enable_adc(proc->cur_work_cp, true);
	ret_mode = platform_class_cp_set_mode(CP_ROLE_MASTER, proc->cur_cp_mode);
	if (info->cp_type == MCA_CP_TYPE_PARALLEL)
		(void)platform_class_cp_set_mode(CP_ROLE_SLAVE, proc->cur_cp_mode);
	else
		ret |= ret_mode;
	if (ret) {
		mca_log_err("set cp_mode or adc failed\n");
		return ret;
	}

	ret = platform_class_cp_get_battery_voltage(proc->cur_work_cp, &vbat);
	if (ret) {
		mca_log_err("open path get vbat fail\n");
		return ret;
	}

	(void)mca_vote(info->input_suspend_voter, "wire_qc", true, 1);
	proc->cur_adp_cur = MCA_QUICK_CHG_OPEN_PATH_CURR;
	proc->cur_adp_volt = proc->delta_volt + proc->ratio * vbat;
	(void)protocol_class_set_adapter_volt_and_curr(proc->cur_protocol,
		proc->cur_adp_volt, MCA_QUICK_CHG_OPEN_PATH_CURR);
	if (mca_quick_charge_msleep(300, info) || info->is_qcom_qc)
		return -1;

	(void)mca_quick_charge_tune_vbus(info, &stat);
	if (stat != XM_VBUS_TUNE_OK)
		return -1;

	ret_en = platform_class_cp_set_charging_enable(proc->cur_work_cp, true);
	if (mca_quick_charge_msleep(300, info))
		return -1;

	for (i = 0; i < MCA_QUICK_CHG_OPEN_PATH_COUNT; i++) {
		ret = platform_class_cp_get_bus_current(proc->cur_work_cp, &ibus);
		ret |= ret_en;
		ret |= platform_class_cp_get_battery_voltage(proc->cur_work_cp, &vbat);
		ret |= platform_class_cp_get_bus_voltage(proc->cur_work_cp, &vbus);
		ret |= platform_class_cp_get_charging_enabled(proc->cur_work_cp, &cp_enabled);
		if (ret) {
			mca_log_err("open path get ibus and cp anbale status fail\n");
			return ret;
		}

		if (!cp_enabled) {
			volt = proc->delta_volt + proc->ratio * vbat;
			mca_log_info("open_path_th: cur_adapt_volt %d, last_cur_adp_volt %d\n",
				volt, proc->cur_adp_volt);
			proc->cur_adp_volt = max(volt, proc->cur_adp_volt);
			ret_en = platform_class_cp_set_charging_enable(proc->cur_work_cp, true);
			if (mca_quick_charge_msleep(300, info))
				return -1;
		} else {
			ret_en = 0;
		}

		mca_log_info("open_path_th: %d, ibus[%d]: %d, cp_enabled: %d, vbus: %d, vbat:%d\n",
			proc->open_path, proc->cur_work_cp, ibus, cp_enabled, vbus, vbat);
		if (ibus > proc->open_path)
			break;

		proc->cur_adp_volt += proc->adp_type == XM_CHARGER_TYPE_HVDCP3_B ?
			MCA_QUICK_CHG_QC3B_DEFAULT_VSTEP : MCA_QUICK_CHG_PPS_OPEN_PATH_VSTEP;
		(void)protocol_class_set_adapter_volt_and_curr(proc->cur_protocol,
			proc->cur_adp_volt, proc->cur_adp_cur);
		if (mca_quick_charge_msleep(200, info))
			return -1;
	}

	if (info->fastchg_temp_flag && cp_enabled)
		info->time_start = ktime_get_coarse_boottime() / NSEC_PER_SEC;

	if (cp_enabled)
		return 0;

	mca_charge_mievent_report(CHARGE_DFX_CP_OPEN_FAILED, &proc->cur_work_cp, 1);
	if (proc->cur_work_cp >= 0 && proc->cur_work_cp < MCA_QUICK_CHG_CP_MODE_MAX)
		proc->error_num[proc->cur_work_cp]++;

	return -1;
}

static int mca_get_qc_vbus_tune_status(struct mca_quick_charge_info *info, int *stat)
{
	static int vbus_cp_mv_before;
	static int vbus_counts;
	int vbus = 0;
	int errorhl_stat = 0;
	int target = info->proc_data.cur_adp_volt;
	int ret;

	*stat = XM_VBUS_TUNE_INIT;
	ret = platform_class_cp_get_bus_voltage(CP_ROLE_MASTER, &vbus);
	if (vbus == vbus_cp_mv_before && vbus_counts < 5) {
		vbus_counts++;
		*stat = XM_VBUS_TUNE_WAIT;
		return ret;
	}

	vbus_cp_mv_before = vbus;
	vbus_counts = 0;
	if (vbus < BUS_VOLTAGE_MIN) {
		*stat = XM_VBUS_TUNE_VBUS_LOW;
		return ret;
	}

	(void)platform_class_cp_get_errorhl_stat(CP_ROLE_MASTER, &errorhl_stat);
	if (errorhl_stat == CP_PMID_ERROR_OK && abs(vbus - target) <= MCA_QC_QUICK_CHG_TUNE_WINDOW_MV) {
		*stat = XM_VBUS_TUNE_OK;
		return ret;
	}

	if (errorhl_stat == CP_PMID_ERROR_LOW || target - vbus >= MCA_QC_QUICK_CHG_TUNE_WINDOW_MV) {
		*stat = XM_VBUS_TUNE_READY_UP;
		info->tune_vbus_retry++;
	} else if (errorhl_stat == CP_PMID_ERROR_HIGH ||
		   vbus - target >= MCA_QC_QUICK_CHG_TUNE_WINDOW_MV) {
		*stat = XM_VBUS_TUNE_READY_DOWN;
		info->tune_vbus_retry++;
	}

	if (info->tune_vbus_retry >= MCA_QC_QUICK_CHG_OPEN_PATH_COUNT) {
		info->tune_vbus_retry = 0;
		mca_log_err("XM_VBUS_TUNE_FAIL Reset QC 5V\n");
		(void)protocol_class_qc_set_volt_cmd(ADAPTER_PROTOCOL_QC, QC2_FORCE_5V);
		*stat = XM_VBUS_TUNE_FAIL;
	}

	return ret;
}

static int mca_qc_retry_to_enable_cp(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int i;

	(void)platform_class_cp_set_charging_enable(CP_ROLE_MASTER, true);
	if (mca_quick_charge_msleep(200, info))
		return -1;

	(void)mca_qc_quick_charge_update_ibus(info);
	for (i = 0; i < MCA_QC_QUICK_CHG_RETRY_COUNT; i++) {
		if (proc->ibus >= MCA_QC_QUICK_CHG_IBUS_UCP_RISING_MA)
			break;
		proc->cur_adp_volt += proc->adp_type == XM_CHARGER_TYPE_HVDCP3_B ?
			MCA_QUICK_CHG_QC3B_DEFAULT_VSTEP : MCA_QUICK_CHG_DEFAULT_VSTEP;
		(void)protocol_class_set_adapter_volt_and_curr(proc->cur_protocol,
			proc->cur_adp_volt, proc->cur_adp_cur);
		if (mca_quick_charge_msleep(100, info))
			return -1;
		(void)mca_qc_quick_charge_update_ibus(info);
	}

	return 0;
}

static int mca_qc_quick_charge_open_path(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	bool cp_enabled = false;
	int stat = XM_VBUS_TUNE_INIT;
	int vbat = 0;
	int ret, i;

	ret = platform_class_cp_enable_adc(CP_ROLE_MASTER, true);
	ret |= platform_class_cp_set_mode(CP_ROLE_MASTER, CP_MODE_FORWARD_2_1);
	if (ret) {
		mca_log_err("set cp_mode or adc failed\n");
		return ret;
	}

	ret = platform_class_cp_get_battery_voltage(CP_ROLE_MASTER, &vbat);
	if (ret) {
		mca_log_err("open path get vbat fail\n");
		return ret;
	}

	proc->cur_adp_cur = MCA_QUICK_CHG_OPEN_PATH_CURR;
	proc->cur_adp_volt = proc->delta_volt + vbat * 2;
	(void)mca_vote(info->input_suspend_voter, "wire_qc", true, 1);
	mca_log_info("set adp volt %d cur %d\n", proc->cur_adp_volt, proc->cur_adp_cur);
	(void)protocol_class_set_adapter_volt_and_curr(proc->cur_protocol, proc->cur_adp_volt,
		proc->cur_adp_cur);
	if (mca_quick_charge_msleep(3000, info))
		return -1;

	for (i = 0; i < MCA_QC_QUICK_CHG_OPEN_PATH_COUNT; i++) {
		ret = mca_get_qc_vbus_tune_status(info, &stat);
		mca_log_info("qc vbus tune status[%d]: %d\n", i, stat);

		switch (stat) {
		case XM_VBUS_TUNE_VBUS_LOW:
		case XM_VBUS_TUNE_FAIL:
			return -1;
		case XM_VBUS_TUNE_READY_UP:
			proc->cur_adp_volt += proc->adp_type == XM_CHARGER_TYPE_HVDCP3_B ?
				MCA_QUICK_CHG_QC3B_DEFAULT_VSTEP : MCA_QUICK_CHG_DEFAULT_VSTEP;
			(void)mca_quick_charge_req_adp_volt_and_cur(info);
			break;
		case XM_VBUS_TUNE_READY_DOWN:
			proc->cur_adp_volt -= proc->adp_type == XM_CHARGER_TYPE_HVDCP3_B ?
				MCA_QUICK_CHG_QC3B_DEFAULT_VSTEP : MCA_QUICK_CHG_DEFAULT_VSTEP;
			(void)mca_quick_charge_req_adp_volt_and_cur(info);
			break;
		case XM_VBUS_TUNE_OK:
			ret = platform_class_cp_set_charging_enable(CP_ROLE_MASTER, true);
			if (ret) {
				mca_log_err("open path enable cp charging fail\n");
				return ret;
			}
			if (mca_quick_charge_msleep(300, info))
				return -1;

			(void)platform_class_cp_get_charging_enabled(CP_ROLE_MASTER, &cp_enabled);
			if (cp_enabled) {
				mca_log_info("open path enable cp charging success\n");
				return 0;
			}

			mca_log_err("open path enable cp charging failed, retry\n");
			info->master_cp_enable_count++;
			if (mca_qc_retry_to_enable_cp(info))
				return -1;

			(void)platform_class_cp_get_charging_enabled(CP_ROLE_MASTER, &cp_enabled);
			if (!cp_enabled) {
				if (info->master_cp_enable_count >= 5)
					info->master_cp_enable_count = 0;
				mca_charge_mievent_report(CHARGE_DFX_CP_OPEN_FAILED,
					&proc->cur_work_cp, 1);
				return -1;
			}
			info->master_cp_enable_count = 0;
			mca_log_info("retry enable cp charging success\n");
			return 0;
		default:
			break;
		}

		if (ret)
			return ret;

		if (mca_quick_charge_msleep(100, info))
			return -1;
	}

	return 0;
}

static int mca_get_cpmode_unrelated_fcc(struct mca_quick_charge_info *info, bool use_secure)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int stage = proc->cur_stage[FG_IC_MASTER];
	int delta_ichg = info->smartchg_data.delta_ichg;
	int cur_max = proc->cur_volt_para[FG_IC_MASTER]->volt_para[stage / 2].current_max;
	int secure_cur = use_secure ? proc->secure_info.secure_cur : 0;
	int thermal_cur, fcc;

	mca_log_info("cur_stage %d cur_max %d delta_cur %d cur_work_cp %d\n",
		stage, cur_max, delta_ichg, proc->cur_work_cp);

	if (!info->dtpt_status) {
		fcc = cur_max - delta_ichg;
	} else if (!info->support_dtpt_ichg_dec) {
		fcc = cur_max * MCA_DTPT_MOLECULE_SCALE / MCA_DTPT_DENOM_SCALE;
	} else {
		mca_log_info("support_dtpt_ichg_dec:dtpt_ichg_dec %d\n", delta_ichg);
		fcc = cur_max * MCA_DTPT_MOLECULE_SCALE / MCA_DTPT_DENOM_SCALE - delta_ichg;
	}

	thermal_cur = proc->thermal_cur[proc->cur_work_cp == MCA_QUICK_CHG_CP_DUAL];
	if (thermal_cur)
		fcc = min(fcc, thermal_cur);
	if (secure_cur)
		fcc = min(fcc, secure_cur);
	fcc = min(fcc, proc->temp_max_cur[FG_IC_MASTER]);

	mca_log_info("cur_max %d secure_cur %d temp_max_cur %d thermal_cur %d\n",
		fcc, secure_cur, proc->temp_max_cur[FG_IC_MASTER], thermal_cur);

	return fcc;
}

/* Whether a current lies in the window a division covers, either way round. */
static bool mca_cp_mode_covers(const struct cp_work_mode_unit *unit, int curr)
{
	return (unit->h_thres <= curr && curr <= unit->l_thres) ||
	       (unit->l_thres <= curr && curr <= unit->h_thres);
}

static int mca_select_cp_work_mode_by_current(struct mca_quick_charge_info *info, int curr)
{
	struct mca_cp_work_mode *cp_mode = &info->cp_work_mode;
	int mode = cp_mode->cur_mode;

	if (mode < CHG_MODE_DIV1 || mode >= CHG_MODE_MAX) {
		mca_log_err("current work mode abnormal, try to get from proc_data\n");
		switch (info->proc_data.work_mode) {
		case MCA_QUICK_CHG_MODE_DIV_1:
			mode = CHG_MODE_DIV1;
			break;
		case MCA_QUICK_CHG_MODE_DIV_2:
			mode = CHG_MODE_DIV2;
			break;
		case MCA_QUICK_CHG_MODE_DIV_4:
			mode = CHG_MODE_DIV4;
			break;
		default:
			mca_log_err("proc_data work mode abnormal!\n");
			return -1;
		}
		cp_mode->cur_mode = mode;
	}

	if (mca_cp_mode_covers(&cp_mode->mode_table[mode], curr))
		return mode;

	if (curr > cp_mode->mode_table[mode].h_thres) {
		for (mode = mode + 1; mode < CHG_MODE_MAX; mode++) {
			if (mca_cp_mode_covers(&cp_mode->mode_table[mode], curr))
				return mode;
		}
	} else {
		for (mode = mode - 1; mode >= CHG_MODE_DIV1; mode--) {
			if (mca_cp_mode_covers(&cp_mode->mode_table[mode], curr))
				return mode;
		}
	}

	return -1;
}

static void mca_cp_check_initial_mode(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct mca_cp_work_mode *cp_mode = &info->cp_work_mode;
	int adp_index = proc->adp_info_index[CHG_MODE_DIV1];
	int vbat = proc->vbat[FG_IC_MASTER];
	int stage = proc->cur_stage[FG_IC_MASTER];
	int size = proc->cur_volt_para[FG_IC_MASTER]->volt_para_size;
	int adp_min_volt, fcc, mode;

	if (adp_index < 0 || adp_index >= ADAPTER_CAP_MAX_NR)
		return;
	adp_min_volt = proc->adp_info[adp_index].cap_info.min_voltage;

	mca_log_info("volt_para_size: %d, cur_stage: %d, vbat: %d, adp_min_volt: %d",
		size, stage, vbat, adp_min_volt);
	cp_mode->div1_vbat_low = vbat <= adp_min_volt;

	if (!cp_mode->last_stage) {
		if (stage < size * 2 - 2) {
			fcc = mca_get_cpmode_unrelated_fcc(info, false);
			mode = mca_select_cp_work_mode_by_current(info, fcc);
			mca_log_info("mode_unrelated_fcc: %d, current mode: %d, select mode: %d\n",
				fcc, cp_mode->cur_mode, mode);
			if (mode == CHG_MODE_DIV1 && cp_mode->div1_vbat_low) {
				mca_log_info("vbat too low, won't use div1 mode\n");
				return;
			}

			if (mode > cp_mode->default_mode) {
				mca_log_info("selected mode higher than default mode, use default mode\n");
				mode = cp_mode->default_mode;
			}
			if (mode < CHG_MODE_DIV1 || mode >= CHG_MODE_MAX) {
				mca_log_err("selected mode abnormal!\n");
				return;
			}
			if (mode == cp_mode->cur_mode)
				return;

			cp_mode->cur_mode = mode;
			proc->work_mode = BIT(mode);
			mca_quick_charge_update_work_mode_para(info);
			return;
		}
		cp_mode->last_stage = true;
	}

	mca_log_info("last charging stage, use default mode\n");
}

/* Change the division the pump runs at while charging. */
static void mca_cp_switch_mode(struct mca_quick_charge_info *info, int mode)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int ret;

	mca_log_err("stop charge\n");
	(void)platform_class_cp_set_charging_enable(CP_ROLE_MASTER, false);
	(void)platform_class_cp_set_charging_enable(CP_ROLE_SLAVE, false);
	cancel_delayed_work_sync(&info->vfc_work);
	mca_quick_charge_set_cp_default_fsw(info);
	info->pd_switch_to_pmic = true;
	info->check_vbat_ov = false;
	memset(&proc->ibus_queue, 0, sizeof(proc->ibus_queue));
	mca_quick_charge_avoid_vusb_ovp(info, 100);
	mca_quick_charge_set_cp_mode_2_1(info);
	(void)platform_class_cp_enable_adc(CP_ROLE_MASTER, false);
	(void)platform_class_cp_enable_adc(CP_ROLE_SLAVE, false);

	proc->work_mode = BIT(mode);
	mca_quick_charge_update_work_mode_para(info);
	if (info->vfc_para.support_cp_vfc) {
		(void)platform_class_cp_set_fsw(CP_ROLE_MASTER, MCA_QUICK_CHG_CP_DEFAULT_FSW);
		if (info->cp_type == MCA_CP_TYPE_PARALLEL)
			(void)platform_class_cp_set_fsw(CP_ROLE_SLAVE, MCA_QUICK_CHG_CP_DEFAULT_FSW);
	}

	(void)platform_class_cp_enable_busucp(proc->cur_work_cp, false);
	ret = mca_quick_charge_open_path(info);
	(void)platform_class_cp_enable_busucp(proc->cur_work_cp, true);
	if (!ret) {
		info->pd_switch_to_pmic = false;
		info->boost_done = false;
		if (info->vfc_para.support_cp_vfc)
			schedule_delayed_work(&info->vfc_work,
				msecs_to_jiffies(MCA_QUICK_CHG_VFC_INTERVAL));
		info->cp_work_mode.cur_mode = mode;
		return;
	}

	proc->total_err++;
	(void)mca_vote(info->input_suspend_voter, "wire_qc", false, 0);
	(void)platform_class_cp_set_charging_enable(proc->cur_work_cp, false);
	info->pd_switch_to_pmic = true;
	(void)mca_quick_charge_req_adp_volt_and_cur(info);
	mca_log_err("open path fail\n");
	proc->stop_charging = true;
	mca_log_err("mode switch start charging failed\n");
}

static void mca_check_cp_work_mode(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct mca_cp_work_mode *cp_mode = &info->cp_work_mode;
	int adp_index = proc->adp_info_index[CHG_MODE_DIV1];
	int vbat = proc->vbat[FG_IC_MASTER];
	int stage = proc->cur_stage[FG_IC_MASTER];
	int size = proc->cur_volt_para[FG_IC_MASTER]->volt_para_size;
	int adp_min_volt, fcc, mode;

	if (adp_index < 0 || adp_index >= ADAPTER_CAP_MAX_NR)
		return;
	adp_min_volt = proc->adp_info[adp_index].cap_info.min_voltage;

	mca_log_info("volt_para_size: %d, cur_stage:%d, vbat: %d, adp_min_volt: %d",
		size, stage, vbat, adp_min_volt);

	if (cp_mode->last_stage || stage >= size * 2 - 2) {
		cp_mode->last_stage = true;
		mode = cp_mode->default_mode;
		mca_log_info("last charging stage, use default mode: %d\n", mode);
		goto check_range;
	}

	fcc = mca_get_cpmode_unrelated_fcc(info, true);
	mode = mca_select_cp_work_mode_by_current(info, fcc);
	if (mode > cp_mode->default_mode) {
		mca_log_info("selected mode higher than default mode, use default mode\n");
		mode = cp_mode->default_mode;
	}
	mca_log_info("mode_unrelated_fcc: %d, current mode: %d, select mode: %d\n",
		fcc, cp_mode->cur_mode, mode);

	if (cp_mode->div1_vbat_low && vbat > adp_min_volt) {
		cp_mode->div1_vbat_low = false;
		if (mode == CHG_MODE_DIV1)
			goto switch_mode;
		goto check_range;
	}

	if (mode == CHG_MODE_DIV1) {
		if (!cp_mode->div1_vbat_low)
			goto switch_mode;
		mca_log_info("vbat too low, won't use mode div1\n");
		if (cp_mode->cur_mode != CHG_MODE_DIV1)
			return;
		mode = cp_mode->default_mode;
	}

check_range:
	if (mode < CHG_MODE_DIV1 || mode >= CHG_MODE_MAX) {
		mca_log_err("cp mode selected result abnormal!\n");
		return;
	}

switch_mode:
	if (mode == cp_mode->cur_mode)
		return;

	mca_cp_switch_mode(info, mode);
}

static void mca_quick_charge_start_charging(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int vbat = 0, vbat_base = 0, vbat_flip = 0;
	int ret;

	proc->soc = strategy_class_fg_ops_get_soc();
	if (proc->soc >= MCA_QUICK_CHG_START_SOC_TH) {
		mca_log_info("soc too high\n");
		return;
	}

	(void)strategy_class_fg_ops_get_voltage(&vbat);
	if (info->support_base_flip || info->base_flip_same) {
		(void)platform_fg_ops_get_volt(FG_IC_MASTER, &vbat_base);
		(void)platform_fg_ops_get_volt(FG_IC_SLAVE, &vbat_flip);
		vbat = min(vbat_base, vbat_flip);
	}
	if (vbat < info->min_vbat || vbat > info->max_vbat) {
		mca_log_info("vbat %d is out of range, try next loop\n", vbat);
		mca_log_info("vbatt range invalid\n");
		return;
	}

	if (mca_quick_charge_can_tbat_do_charge(info, true)) {
		mca_log_info("temp range invalid\n");
		return;
	}

	if (mca_quick_charge_judge_thermal_vote(info)) {
		mca_log_info("thermal fcc too low\n");
		return;
	}

	proc->cur_work_cp = MCA_QUICK_CHG_CP_MASTER;
	if (info->cp_type != MCA_CP_TYPE_SINGLE &&
	    proc->error_num[MCA_QUICK_CHG_CP_MASTER] >= MCA_QUICK_CHG_SINGLE_MAX_ERR_COUNT) {
		if (proc->error_num[MCA_QUICK_CHG_CP_SLAVE] >= MCA_QUICK_CHG_SINGLE_MAX_ERR_COUNT) {
			mca_log_err("select cp mode fail\n");
			return;
		}
		proc->cur_work_cp = MCA_QUICK_CHG_CP_SLAVE;
	}

	if (info->support_base_flip && mca_quick_charge_parallel_judge_temp(info)) {
		mca_log_err("get_parallel_volt_para fail\n");
		return;
	}

	if (info->bypass_curr && info->bypass_curr < info->cp_switch_pmic_th) {
		mca_log_info("bypass curr %d is low, exit cp\n", info->bypass_curr);
		return;
	}

	mca_quick_charge_select_stage(info);
	if (info->vfc_para.support_cp_vfc) {
		(void)platform_class_cp_set_fsw(CP_ROLE_MASTER, MCA_QUICK_CHG_CP_DEFAULT_FSW);
		if (info->cp_type == MCA_CP_TYPE_PARALLEL)
			(void)platform_class_cp_set_fsw(CP_ROLE_SLAVE, MCA_QUICK_CHG_CP_DEFAULT_FSW);
	}

	mca_log_info("adp_mode: %d, cur_support_mode: %d\n", proc->adp_mode, info->cur_support_mode);
	if (info->cp_work_mode.support_mode_switch && proc->adp_type == XM_CHARGER_TYPE_PD_VERIFY &&
	    proc->max_power > MCA_QUICK_CHG_MODE_SWITCH_POWER_MIN &&
	    !(info->cur_support_mode & ~proc->adp_mode))
		mca_cp_check_initial_mode(info);

	if (info->support_pmic_vterm_dynamics_adjust) {
		(void)mca_vote_override(info->vterm_voter, "pmic_temp_term", false, 0);
		(void)mca_vote_override(info->buck_input_voter, "pmic_temp_term", false, 0);
	}

	(void)platform_class_cp_enable_busucp(proc->cur_work_cp, false);
	if (!info->is_qcom_qc)
		ret = mca_quick_charge_open_path(info);
	else
		ret = mca_qc_quick_charge_open_path(info);
	(void)platform_class_cp_enable_busucp(proc->cur_work_cp, true);

	if (!ret) {
		info->pd_switch_to_pmic = false;
		proc->charge_flag = MCA_QUICK_CHG_STS_CHARGING;
		info->boost_done = false;
		if (proc->ffc_flag)
			(void)strategy_class_fg_set_fastcharge(true);
		schedule_delayed_work(&info->monitor_work, 0);
		if (info->vfc_para.support_cp_vfc)
			schedule_delayed_work(&info->vfc_work,
				msecs_to_jiffies(MCA_QUICK_CHG_VFC_INTERVAL));
		return;
	}

	proc->total_err++;
	(void)mca_vote(info->input_suspend_voter, "wire_qc", false, 0);
	(void)platform_class_cp_set_charging_enable(proc->cur_work_cp, false);
	info->pd_switch_to_pmic = true;
	if (info->is_qcom_qc)
		proc->cur_adp_volt = MCA_QC_ADP_VOLT_DEFAULT;
	(void)mca_quick_charge_req_adp_volt_and_cur(info);
	mca_log_err("open path fail\n");
}

/*
 * After a completed charge the pump stays off while the cell sits below the
 * recharge voltage.
 */
static bool mca_quick_charge_check_chg_done(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;

	if (proc->charge_flag != MCA_QUICK_CHG_STS_CHARGE_DONE || info->hardware_cv)
		return false;

	mca_quick_charge_update_vbat(info);
	if (proc->vbat[FG_IC_MASTER] >= info->recharge_vbat)
		return false;

	if (info->batt_type == MCA_BATTERY_TYPE_SINGLE)
		return true;

	return proc->vbat[FG_IC_SLAVE] < info->recharge_vbat;
}

static void mca_quick_charge_pre_charge_check(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	bool cp_present = false;

	if (proc->charge_flag == MCA_QUICK_CHG_STS_CHARGING)
		return;

	if (!info->online) {
		(void)mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
			STRATEGY_STATUS_TYPE_ONLINE, &info->online);
		mca_log_err("check charger online: %d\n", info->online);
		if (!info->online)
			return;
	}

	if (!strategy_class_fg_is_chip_ok()) {
		mca_log_info("fg i2c error, exit pre charge check\n");
		return;
	}

	(void)platform_class_cp_get_present(CP_ROLE_MASTER, &cp_present);
	if (!cp_present) {
		mca_log_info("cp not present, exit pre charge check\n");
		return;
	}

	proc->soc = strategy_class_fg_ops_get_soc();
	if (proc->soc >= MCA_QUICK_CHG_START_SOC_TH)
		return;

	if (proc->total_err > MCA_QUICK_CHG_MAX_ERR_COUNT) {
		proc->charge_flag = MCA_QUICK_CHG_STS_CHARGE_FAILED;
		return;
	}

	if (!info->batt_auth || info->batt_missing) {
		(void)mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_FG,
			STRATEGY_STATUS_TYPE_AUTH_PASS, &info->batt_auth);
		(void)mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BMD,
			STRATEGY_STATUS_TYPE_BMD, &info->batt_missing);
		mca_log_err("check batt auth: %d, batt missing: %d\n",
			info->batt_auth, info->batt_missing);
	}

	if (info->force_stop || !info->cur_support_mode || !info->sysfs_data.chg_enable ||
	    info->taper_done_no_retry || !info->batt_auth || info->batt_missing)
		return;

	if (mca_quick_charge_check_chg_done(info)) {
		mca_log_info("chg done, check next loop\n");
		return;
	}

	switch (proc->adp_type) {
	case XM_CHARGER_TYPE_HVDCP3_B:
	case XM_CHARGER_TYPE_HVDCP3P5:
		proc->work_mode = MCA_QUICK_CHG_MODE_DIV_2;
		proc->min_adp_volt = MCA_QC_ADP_MIN_VOLT;
		proc->max_adp_volt = MCA_QC_ADP_MAX_VOLT;
		proc->max_adp_curr = MCA_QC_ADP_MAX_CURR;
		break;
	case XM_CHARGER_TYPE_PPS:
	case XM_CHARGER_TYPE_PD_VERIFY:
		if (mca_quick_charge_adjust_adapter_mode(info)) {
			proc->total_err++;
			mca_log_err("get adapt mode fail,err=%d\n", proc->total_err);
			return;
		}
		mca_quick_charge_select_cur_work_mode(info);
		break;
	default:
		return;
	}

	mca_quick_charge_update_work_mode_para(info);
	mutex_lock(&info->data_lock);
	mca_log_info("Acquire mutex_lock\n");
	mca_quick_charge_start_charging(info);
	mutex_unlock(&info->data_lock);
	mca_log_info("Release mutex_lock\n");
}

/*
 * Once the cell has sat at its float voltage for four passes, cap the charge
 * current 1A below where it was (never below the termination current), and
 * give up on the pump if that is already at the taper current.
 */
static void mca_quick_charger_get_secure_cur(struct mca_quick_charge_info *info, int iterm)
{
	static int over_volt_count;
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct secure_quick_charge_data *secure = &proc->secure_info;
	int delta_fv = info->smartchg_data.delta_fv;
	int max_vbatt, cur;

	max_vbatt = proc->temp_max_fv[FG_IC_MASTER] - delta_fv;
	if (max_vbatt <= MCA_QUICK_CHG_VBAT_SECURE_MIN)
		max_vbatt = proc->temp_max_fv[FG_IC_MASTER];

	over_volt_count = proc->vbat[FG_IC_MASTER] >= max_vbatt ? over_volt_count + 1 : 0;
	secure->max_vbatt = max_vbatt;
	if (over_volt_count > 3 && !info->check_vbat_ov) {
		over_volt_count = 0;
		info->check_vbat_ov = true;
		cur = secure->secure_cur ? secure->secure_cur : proc->ibat[FG_IC_MASTER];
		secure->secure_cur = max(cur - 1000, iterm);
		if (secure->secure_cur <= info->pps_taper_fcc_ma)
			proc->stop_charging = true;
		mca_log_info("trigger over voltage secure_cur: %d, pps_taper_fcc_ma: %d\n",
			secure->secure_cur, info->pps_taper_fcc_ma);
	}

	mca_log_info("max_vbatt:%d delta_fv:%d secure_cur:%d\n", secure->max_vbatt, delta_fv,
		secure->secure_cur);
}

/* The step section a site's stage is in, clamped to its last step. */
static int mca_quick_charger_parallel_section(struct mca_quick_charge_volt_para_info *volt_info,
	int stage)
{
	int last = volt_info->volt_para_size - 1;

	if (stage <= last * 2)
		return (stage + 1) / 2;

	return last;
}

static int mca_quick_charger_get_base_curr(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_volt_para_info *volt_info =
		info->proc_data.cur_volt_paraller[FG_IC_BASE];
	int stage = info->proc_data.parall_cur_stage[FG_IC_BASE];
	int section, cur;

	if (!volt_info || !volt_info->volt_para)
		return 0;

	section = mca_quick_charger_parallel_section(volt_info, stage);
	cur = volt_info->volt_para[section].current_max;
	mca_log_err("cur_base_stage:%d cur_volt_section:%d base_curr_max:%d\n", stage, section, cur);

	return cur;
}

/*
 * The flip cell is fed through the load switch; its limit only goes back up
 * once the cell is clear of its float voltage.
 */
static int mca_quick_charger_set_flip_cur(struct mca_quick_charge_info *info)
{
	static int last_flip_curr_max = -1;
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct mca_quick_charge_volt_para_info *volt_info = proc->cur_volt_paraller[FG_IC_FLIP];
	int stage = proc->parall_cur_stage[FG_IC_FLIP];
	int vbat = proc->parall_vbat[FG_IC_FLIP];
	int thermal = info->thermal_flip_cur;
	int section, last, flip;

	if (!volt_info || !volt_info->volt_para)
		return 0;

	last = volt_info->volt_para_size - 1;
	section = mca_quick_charger_parallel_section(volt_info, stage);
	flip = volt_info->volt_para[section].current_max;
	if (thermal)
		flip = min(flip, thermal);

	if (last_flip_curr_max < flip && last_flip_curr_max != -1 &&
	    vbat > volt_info->volt_para[last].voltage - 30)
		flip = last_flip_curr_max;

	last_flip_curr_max = flip;
	(void)platform_class_loadsw_set_ibat_limit(LOADSW_ROLE_MASTER, flip);
	mca_log_err("last_flip_curr_max %d, thermal_flip_cur %d, flip_curr_max %d, cur_stage %d, cur_volt_section:%d, ibat_slave %d, vbat_slave %d\n",
		last_flip_curr_max, thermal, flip, stage, section,
		proc->parall_ibat[FG_IC_FLIP], vbat);

	return flip;
}

/*
 * Software over-current guard for the base cell: after two passes at or over
 * its step current, back the charge current off in proportion to the
 * overshoot; lift the cap once the cell is 150mA below its step current.
 */
static int mca_quick_charger_get_base_limit_curr(struct mca_quick_charge_info *info,
	int base_curr, int cur_max)
{
	static const struct {
		int over;
		int dec;
	} ocp_step[] = {
		{ 1000, 900 },
		{ 500, 600 },
		{ 300, 350 },
		{ 100, 150 },
	};
	static int over_curr_count;
	static bool runSwOcp;
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int base_ibat = proc->parall_ibat[FG_IC_BASE];
	int diff = base_ibat - base_curr;
	int final_cur = cur_max;
	int i;

	over_curr_count = diff >= 0 ? over_curr_count + 1 : 0;
	if (cur_max && over_curr_count > 1) {
		for (i = 0; i < ARRAY_SIZE(ocp_step); i++) {
			if (diff > ocp_step[i].over) {
				final_cur = cur_max - ocp_step[i].dec;
				break;
			}
		}
		over_curr_count = 0;
		runSwOcp = true;
	} else if (base_ibat < base_curr - 150 && runSwOcp) {
		final_cur = 0;
		runSwOcp = false;
	}

	mca_log_err("runSwOcp:%d base_ibat:%d base_vbat:%d base_curr_max:%d over_curr_count:%d cur_max:%d final_cur:%d\n",
		runSwOcp, base_ibat, proc->parall_vbat[FG_IC_BASE], base_curr, over_curr_count,
		cur_max, max(final_cur, 0));
	if (final_cur > 0)
		cur_max = min(cur_max, final_cur);

	return cur_max;
}

static int mca_quick_charge_select_max_ibat(struct mca_quick_charge_info *info, int iterm)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct mca_quick_charge_volt_para_info *volt_info = proc->cur_volt_para[FG_IC_MASTER];
	int stage = proc->cur_stage[FG_IC_MASTER];
	int stage_cur = volt_info->volt_para[stage / 2].current_max;
	int delta_ichg = info->smartchg_data.delta_ichg;
	int secure_cur = proc->secure_info.secure_cur;
	bool dual = proc->cur_work_cp == MCA_QUICK_CHG_CP_DUAL;
	int channel_cur = dual ? proc->max_curr : proc->single_curr;
	int thermal_cur = proc->thermal_cur[dual];
	int cur_max, base_curr, flip_curr, pps_cur;

	mca_log_info("cur_stage %d cur_max %d delta_cur %d cur_work_cp %d\n",
		stage, stage_cur, delta_ichg, proc->cur_work_cp);

	if (!info->dtpt_status) {
		cur_max = stage_cur - delta_ichg;
	} else if (!info->support_dtpt_ichg_dec) {
		cur_max = stage_cur * MCA_DTPT_MOLECULE_SCALE / MCA_DTPT_DENOM_SCALE;
	} else {
		mca_log_info("support_dtpt_ichg_dec:dtpt_ichg_dec %d\n", delta_ichg);
		cur_max = stage_cur * MCA_DTPT_MOLECULE_SCALE / MCA_DTPT_DENOM_SCALE - delta_ichg;
	}

	mca_log_info("cur_max %d secure_cur %d channel_cur %d thermal_cur %d bypass_curr %d\n",
		cur_max, secure_cur, channel_cur, thermal_cur, info->bypass_curr);

	if (thermal_cur)
		cur_max = min(cur_max, thermal_cur);
	if (secure_cur)
		cur_max = min(cur_max, secure_cur);
	if (proc->max_adap_ibat)
		cur_max = min(cur_max, proc->max_adap_ibat);
	cur_max = min(cur_max, channel_cur);
	cur_max = min(cur_max, proc->temp_max_cur[FG_IC_MASTER]);
	cur_max = min(cur_max, proc->ratio * proc->max_adp_curr);
	if ((info->support_base_flip || info->base_flip_same) && proc->sw_ocp_curr)
		cur_max = min(cur_max, proc->sw_ocp_curr);
	if (info->bypass_curr)
		cur_max = min(cur_max, info->bypass_curr);
	if (info->support_ichg_cutoff_priority)
		cur_max = max(cur_max, iterm);

	/* leave the non-EU PPS adapter some headroom, more without power boost */
	if (proc->adp_type == XM_CHARGER_TYPE_PPS && !info->is_eu_model) {
		pps_cur = cur_max - MCA_PPS_FCC_MARGIN;
		if (info->smartchg_data.pwr_boost_state)
			cur_max = min(pps_cur, MCA_PPS_BOOST_FCC_MAX);
		else
			cur_max = min(pps_cur, info->third_pps_ibat_limit);
	}

	if (info->support_base_flip) {
		base_curr = mca_quick_charger_get_base_curr(info);
		flip_curr = mca_quick_charger_set_flip_cur(info);
		if (base_curr + flip_curr)
			cur_max = min(cur_max, base_curr + flip_curr);
		cur_max = mca_quick_charger_get_base_limit_curr(info, base_curr, cur_max);
		mca_log_info("cur_max: %d\n", cur_max);
	}

	return cur_max;
}

/*
 * The finer step table: the row is chosen by how far the cell is from its
 * target, and the bigger step only holds while the cell keeps moving; a cell
 * that has barely moved since the last boost is given no step at all.
 */
static int mca_quick_charge_calculate_vstep_ex(struct mca_quick_charge_info *info,
	int vbat, int delta_v, u64 boost_time_diff_ms)
{
	static int last_vbat;
	static int stage = -1;
	struct mca_quick_charge_volt_step_ex_para *ex;
	int vbat_change = vbat - last_vbat;
	int final_vstep = MCA_QUICK_CHG_DEFAULT_VSTEP;
	int i;

	if (vbat_change > 0) {
		for (i = 0; i < VSTEP_PARA_EX_MAX_GROUP; i++) {
			ex = &info->vstep_para_ex[i];
			if (delta_v > ex->delta_v_thr)
				continue;
			stage = i;
			if (vbat_change > ex->vbat_change_thr) {
				final_vstep = ex->vstep_multiplier * MCA_QUICK_CHG_DEFAULT_VSTEP;
				break;
			}
		}
	}

	if (stage >= 0 && stage < VSTEP_PARA_EX_MAX_GROUP) {
		ex = &info->vstep_para_ex[stage];
		if (vbat_change < ex->vbat_change_thr && boost_time_diff_ms < ex->boost_time_thr) {
			mca_log_info("vbat change too small (%d < %d) within %llu ms (threshold: %llu ms), stopping boost\n",
				vbat_change, ex->vbat_change_thr, boost_time_diff_ms,
				ex->boost_time_thr);
			final_vstep = 0;
		}
	}

	last_vbat = vbat;
	mca_log_info("Returning final_vstep: %d\n", final_vstep);

	return final_vstep;
}

static bool mca_quick_charge_is_en_buck_hwid(void)
{
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	u32 id;

	if (!hwid) {
		mca_log_err("get hwid info failed\n");
		return true;
	}

	if (hwid->platform_version != QC_HWID_PROJECT_EN_BUCK)
		return false;

	id = hwid->hwid_value;
	if (id >= 0x100000)
		return id - 0x100000 < 2 || id == 0x110000;
	if (id - 0x10000 <= 3)
		return id - 0x10000 != 1;

	return id == 1;
}

/*
 * On a flip-base phone the buck feeds the base cell while both pumps feed
 * the main one; what the buck is given is stepped by the charge current and
 * bus load, and it is shut off for good once the base cell nears its float
 * voltage.
 */
static void strategy_quickchg_open_buck_charging(struct mca_quick_charge_info *info,
	int cur_max, int ibat)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int ibus[CP_ROLE_MAX] = { 0 };
	int ibus_buck = 0, aicl_status = 0, pvbat = 0;
	int vterm, fcc, icl;

	(void)platform_class_cp_get_bus_current(CP_ROLE_MASTER, &ibus[CP_ROLE_MASTER]);
	(void)platform_class_cp_get_bus_current(CP_ROLE_SLAVE, &ibus[CP_ROLE_SLAVE]);
	(void)platform_class_buckchg_ops_get_bus_curr(MAIN_BUCK_CHARGER, &ibus_buck);
	(void)platform_class_buckchg_ops_get_aicl_status(MAIN_BUCK_CHARGER, &aicl_status);
	vterm = mca_get_effective_result(info->vterm_voter);
	(void)platform_class_buckchg_ops_get_pack_vbat(MAIN_BUCK_CHARGER, &pvbat);
	if (pvbat >= vterm - 30)
		info->stop_pmic = true;

	mca_log_info("ibus_buck :%d, aicl_status :%d, master_ibus :%d, slave_ibus :%d, vterm :%d, pvbat :%d, stop_pmic: %d\n",
		ibus_buck, aicl_status, ibus[CP_ROLE_MASTER], ibus[CP_ROLE_SLAVE], vterm, pvbat,
		info->stop_pmic);

	if (proc->adp_type != XM_CHARGER_TYPE_PD_VERIFY ||
	    proc->cur_work_cp != MCA_QUICK_CHG_CP_DUAL || info->stop_pmic) {
		info->pmih_fcc = 0;
		(void)mca_vote(info->input_suspend_voter, "wire_qc", true, 1);
		(void)mca_vote_override(info->buck_input_voter, "icl_limit", false, 0);
		(void)mca_vote_override(info->buck_charge_curr_voter, "fcc_limit", false, 0);
		return;
	}

	if (cur_max > 11000 && proc->ibus > 5200)
		fcc = 3000;
	else if (cur_max > 8000 && proc->ibus > 4200)
		fcc = 2000;
	else if (cur_max > 5000 && proc->ibus > 2700)
		fcc = 1000;
	else
		fcc = 400;

	if (cur_max <= ibat || fcc == info->pmih_fcc)
		return;

	icl = fcc / 2 + 300;
	info->pmih_fcc = fcc;
	(void)mca_vote(info->input_suspend_voter, "wire_qc", false, 0);
	(void)mca_vote_override(info->buck_input_voter, "icl_limit", true, icl);
	(void)mca_vote_override(info->buck_charge_curr_voter, "fcc_limit", true, fcc);
	(void)platform_class_buckchg_ops_set_restart_aicl(MAIN_BUCK_CHARGER, true);
	mca_log_err("enable buck parallel charging, pmih_fcc :%d, pmih_icl :%d\n", fcc, icl);
}

/*
 * With one pump and the buck charging together in FFC, the buck's float
 * voltage tracks the JEITA FFC target, nudged by what the pack actually
 * reads every fifth pass.
 */
static void strategy_quickchg_adjust_vterm(struct mca_quick_charge_info *info)
{
	int ffc_vterm = 0, pvbat = 0;
	int target, actual_vterm, diff;

	(void)mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_JEITA,
		STRATEGY_STATUS_TYPE_JEITA_FFC_VTERM, &ffc_vterm);
	(void)platform_class_buckchg_ops_get_pack_vbat(MAIN_BUCK_CHARGER, &pvbat);
	target = ffc_vterm - info->smartchg_data.delta_fv;
	if (info->final_vterm != target) {
		info->last_override_vterm = target;
		info->final_vterm = target;
	}
	mca_log_info("ffc_vterm :%d, final_vterm :%d\n", ffc_vterm, info->final_vterm);

	if (++info->vterm_count < 5 || info->final_vterm >= pvbat)
		return;

	actual_vterm = info->pmic_fv_compensation + info->last_override_vterm;
	diff = actual_vterm - pvbat;
	if (diff < 25)
		info->override_vterm = info->last_override_vterm + 10;
	else if ((u32)diff > 35)
		info->override_vterm = info->last_override_vterm - 5;
	else
		info->override_vterm = info->last_override_vterm;
	mca_log_info("actual_vterm :%d, override_vterm :%d, last_override_vterm :%d, pvbat :%d\n",
		actual_vterm, info->override_vterm, info->last_override_vterm, pvbat);

	if (info->override_vterm != info->last_override_vterm) {
		(void)mca_vote_override(info->buck_vterm_voter, "single_cp_pmic", true,
			info->override_vterm);
		info->last_override_vterm = info->override_vterm;
	}
	info->vterm_count = 0;
}

static void strategy_quickchg_pmic_single_cp_charging(struct mca_quick_charge_info *info,
	int cur_max)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int ibus_cp = 0, ibus_buck = 0, pvbat = 0, curr_vbat = 0;
	int ffc, fcc, icl;

	ffc = strategy_class_fg_get_fastcharge();
	if (!ffc || (proc->adp_type == XM_CHARGER_TYPE_PPS && cur_max < 9000 &&
		     info->is_eu_model)) {
		(void)mca_vote_override(info->buck_vterm_voter, "single_cp_pmic", false, 0);
		info->final_vterm = 0;
		info->last_override_vterm = 0;
		return;
	}

	(void)platform_class_cp_get_bus_current(CP_ROLE_MASTER, &ibus_cp);
	(void)platform_class_buckchg_ops_get_bus_curr(MAIN_BUCK_CHARGER, &ibus_buck);
	(void)platform_class_buckchg_ops_get_pack_vbat(MAIN_BUCK_CHARGER, &pvbat);
	(void)strategy_class_fg_ops_get_voltage(&curr_vbat);
	mca_log_info("ffc_flag :%d\n", ffc);
	strategy_quickchg_adjust_vterm(info);
	mca_log_info("ibus :%d, ibus_buck :%d, ibus_cp :%d, curr_vbat :%d, pvbat :%d\n",
		proc->ibus, ibus_buck, ibus_cp, curr_vbat, pvbat);

	if (curr_vbat >= info->vbat_threshold) {
		info->pmih_fcc = 0;
		(void)mca_vote(info->input_suspend_voter, "wire_qc", true, 1);
		(void)mca_vote_override(info->buck_input_voter, "icl_limit", false, 0);
		(void)mca_vote_override(info->buck_charge_curr_voter, "fcc_limit", false, 0);
		return;
	}

	if (cur_max > info->cur_max_threshold[0] && proc->ibus > info->ibus_threshold[0])
		fcc = info->pmih_fcc_value[0];
	else if (cur_max > info->cur_max_threshold[1] && proc->ibus > info->ibus_threshold[1])
		fcc = info->pmih_fcc_value[1];
	else if (cur_max > info->cur_max_threshold[2] && proc->ibus > info->ibus_threshold[2])
		fcc = info->pmih_fcc_value[2];
	else
		fcc = info->pmih_fcc_value[3];

	if (fcc == info->pmih_fcc)
		return;

	info->pmih_fcc = fcc;
	icl = fcc / 2 + 500;
	(void)mca_vote(info->input_suspend_voter, "wire_qc", false, 0);
	(void)mca_vote_override(info->buck_input_voter, "icl_limit", true, icl);
	(void)mca_vote_override(info->buck_charge_curr_voter, "fcc_limit", true, fcc);
	(void)platform_class_buckchg_ops_set_restart_aicl(MAIN_BUCK_CHARGER, true);
	mca_log_err("enable cp and buck charging, pmih_fcc :%d, pmih_icl :%d\n", fcc, icl);
}

static int mca_quick_charge_regulation(struct mca_quick_charge_info *info)
{
	static u64 last_boost_time_ms;
	static u64 last_buck_time_ms;
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct mca_quick_charge_volt_para_info *volt_info = proc->cur_volt_para[FG_IC_MASTER];
	struct mca_quick_charge_volt_step_para *vstep_para;
	struct mca_hwid_info *hwid;
	int stage = proc->cur_stage[FG_IC_MASTER];
	int vbat = proc->vbat[FG_IC_MASTER];
	int ibat = proc->ibat_total;
	int size = volt_info->volt_para_size;
	int stage_iterm = volt_info->volt_para[stage / 2].current_min;
	int iterm, ibus, cur_max, vbat_th, delta_ibat, comp_ibus, limit, step;
	int vstep, final_vstep, icl, fcc, i;
	int cp_vbus = 0;
	u64 now_ms, boost_time_diff, buck_time_diff;

	now_ms = ktime_to_ms(ktime_get_boottime());
	hwid = mca_get_hwid_info();
	if (mca_quick_charge_update_ibus(info)) {
		mca_log_err("regulation get ibus failed\n");
		return -1;
	}
	ibus = proc->ibus;
	mca_eu_pps_anti_disconnection_strategy(info);

	iterm = volt_info->volt_para[size - 1].current_min;
	mca_quick_charger_get_secure_cur(info, iterm);
	cur_max = mca_quick_charge_select_max_ibat(info, iterm);
	info->current_fcc = cur_max;
	if (cur_max < info->cp_switch_pmic_th) {
		mca_log_info("cur_max too low, stop quick charging: %d <= %d\n", cur_max,
			info->cp_switch_pmic_th);
		proc->stop_charging = true;
	}

	vbat_th = volt_info->volt_para[stage / 2].voltage;
	if (stage / 2 == size - 1) {
		mca_log_info("vbatt_th %d delta_fv %d pps_taper_vol_hys:%d\n", vbat_th,
			info->smartchg_data.delta_fv, info->pps_taper_vol_hys);
		vbat_th -= info->smartchg_data.delta_fv + info->pps_taper_vol_hys;
	}

	boost_time_diff = now_ms - last_boost_time_ms;
	delta_ibat = cur_max - ibat;

	/* the step size, by how far the battery current is from its target */
	vstep = MCA_QUICK_CHG_DEFAULT_VSTEP;
	if (delta_ibat < 1 || !(stage & 1)) {
		for (i = 0; i < VSTEP_PARA_MAX_GROUP; i++) {
			vstep_para = &info->vstep_para[i];
			if (!vstep_para->volt_ratio)
				break;
			if (vstep_para->volt_ratio != proc->ratio ||
			    vstep_para->cur_gap > abs(delta_ibat))
				continue;

			step = vstep_para->vstep_ratio * MCA_QUICK_CHG_DEFAULT_VSTEP;
			if (info->base_flip_same && proc->temp_para_index[FG_IC_MASTER] == 1 &&
			    vstep_para->vstep_ratio >= 5)
				step = step / vstep_para->vstep_ratio * 4;
			if (step)
				vstep = step;
			break;
		}
	}

	/* bus current compensation only applies to a verified adapter before the last stage */
	if (proc->adp_type == XM_CHARGER_TYPE_PD_VERIFY && stage != size * 2 - 1) {
		if (info->support_cancel_compensation && vbat > info->max_vbat_threshold) {
			proc->ibus_compensation = 0;
			mca_log_info("max_vbat_threshold %d, vbat %d\n", info->max_vbat_threshold, vbat);
		} else if (!proc->ibus_compensation) {
			if (proc->work_mode == MCA_QUICK_CHG_MODE_DIV_4)
				proc->ibus_compensation = info->ibus_compensation[CHG_MODE_DIV4];
			else if (proc->work_mode == MCA_QUICK_CHG_MODE_DIV_2)
				proc->ibus_compensation = info->ibus_compensation[CHG_MODE_DIV2];
			else if (proc->work_mode == MCA_QUICK_CHG_MODE_DIV_1)
				proc->ibus_compensation = info->ibus_compensation[CHG_MODE_DIV1];
			mca_log_info("reset ibus_compensation\n");
		}
	} else {
		proc->ibus_compensation = 0;
	}

	comp_ibus = ibus - proc->ibus_compensation;
	if (!(stage & 1)) {
		/* constant current */
		if (comp_ibus > cur_max / proc->ratio || cur_max < ibat) {
			final_vstep = -vstep;
		} else {
			final_vstep = 0;
			limit = cur_max - proc->delta_ibat;
			if (comp_ibus < limit / proc->ratio && ibat < limit && vbat < vbat_th) {
				final_vstep = vbat < vbat_th - 10 ? vstep : MCA_QUICK_CHG_DEFAULT_VSTEP;
				if (info->enable_vstep_ex &&
				    ((info->support_base_flip && vbat > vbat_th - 100) ||
				     stage == size * 2 - 2))
					final_vstep = min(final_vstep,
						mca_quick_charge_calculate_vstep_ex(info, vbat,
							vbat_th - vbat, boost_time_diff));
			}
		}
	} else if (vbat < vbat_th) {
		/* constant voltage, below the target */
		if (comp_ibus > cur_max / proc->ratio || cur_max < ibat) {
			final_vstep = -vstep;
		} else {
			limit = cur_max - proc->delta_ibat;
			final_vstep = ibat < limit && comp_ibus < limit / proc->ratio ? vstep : 0;
		}
	} else {
		/* constant voltage, at or over the target */
		if (proc->ratio * (vbat - vbat_th) < MCA_QUICK_CHG_DEFAULT_VSTEP)
			final_vstep = -MCA_QUICK_CHG_DEFAULT_VSTEP;
		else
			final_vstep = max(proc->ratio * (vbat_th - vbat), -MCA_QUICK_CHG_MAX_VSTEP_DOWN);
	}

	/* once the bus has been stepped down, space the next steps out */
	buck_time_diff = now_ms - last_buck_time_ms;
	if (info->boost_done) {
		if (final_vstep == MCA_QUICK_CHG_DEFAULT_VSTEP &&
		    (boost_time_diff < 2000 || buck_time_diff < 1000))
			final_vstep = 0;
		else if (buck_time_diff < 1000 && final_vstep == -MCA_QUICK_CHG_DEFAULT_VSTEP)
			final_vstep = 0;

		if (hwid && hwid->country_version == CountryGlobal &&
		    hwid->platform_version == QC_HWID_PROJECT_VSTEP_CAP &&
		    final_vstep >= 80 && boost_time_diff < 2000) {
			mca_log_err("final_vstep >= 80, boost_time_diff_ms < 2000, set final_vstep = 0\n");
			final_vstep = 0;
		}
	}

	if (info->en_buck_parallel_chg) {
		if (cur_max > info->use_buck_fcc_threshold) {
			icl = info->buck_icl_fcc_curr[0];
			fcc = info->buck_icl_fcc_curr[1];
			if (mca_quick_charge_is_en_buck_hwid() &&
			    !(hwid && hwid->platform_version == QC_HWID_PROJECT_BUCK_FULL)) {
				icl -= 500;
				fcc -= 2000;
			}
			strategy_quickchg_enable_buck_charging(info, icl, fcc, true);
		} else {
			strategy_quickchg_enable_buck_charging(info, 0, 0, false);
		}
	}

	if (info->support_base_flip)
		strategy_quickchg_open_buck_charging(info, cur_max, ibat);

	if (info->pmic_single_cp_chg)
		strategy_quickchg_pmic_single_cp_charging(info, cur_max);

	if (final_vstep > 0) {
		last_boost_time_ms = now_ms;
	} else if (final_vstep < 0) {
		info->boost_done = true;
		last_buck_time_ms = now_ms;
	}

	proc->cur_adp_cur = proc->max_adp_curr;
	proc->cur_adp_volt += final_vstep;
	(void)platform_class_cp_get_bus_voltage(CP_ROLE_MASTER, &cp_vbus);
	mca_log_info("cur_stage[%d]: adp_volt: %d/%d, ibat: %d/%d/%d/%d, vbat: %d/%d, ibus: %d/%d, time_diff: %llu/%llu, final_vstep: %d\n",
		stage, proc->cur_adp_volt, cp_vbus, cur_max, stage_iterm, ibat, delta_ibat,
		vbat_th, vbat, ibus, proc->ibus_compensation, boost_time_diff, buck_time_diff,
		final_vstep);

	if (mca_quick_charge_req_adp_volt_and_cur(info)) {
		mca_log_err("regulation req adp volt failed\n");
		return -1;
	}

	return 0;
}

static int mca_qc_quick_charge_regulation(struct mca_quick_charge_info *info)
{
	static int ibatt_count;
	static int vbatt_count;
	static int ibat_now_ma_before;
	static int vbat_now_mv_before;
	static int decrement_count;
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int vbat = proc->vbat[FG_IC_MASTER];
	int ibat = proc->ibat_total;
	int delta_fv = info->smartchg_data.delta_fv;
	int thermal = info->thermal_cur[CHG_MODE_DIV2][MCA_QUICK_CHG_CH_SINGLE];
	int ibat_qc_max, ibus_qc_max, fcc_limit, ibus_limit;
	int ibus, vbus = 0, fv, cmd, ret;
	bool ibat_stale, vbat_stale, up = false, down = false;

	if (info->bypass_curr && info->bypass_curr < info->cp_switch_pmic_th) {
		mca_log_info("bypass curr %d is low, exit cp\n", info->bypass_curr);
		return -1;
	}

	info->qc_normal_charge_fv_max_mv = proc->temp_max_fv[FG_IC_MASTER];
	fv = info->qc_normal_charge_fv_max_mv;
	if (mca_qc_quick_charge_update_ibus(info)) {
		mca_log_err("regulation qc get ibus failed\n");
		return 0;
	}
	ibus = proc->ibus;

	if (platform_class_cp_get_bus_voltage(CP_ROLE_MASTER, &vbus)) {
		proc->error_num[MCA_QUICK_CHG_CP_MASTER]++;
		mca_log_err("regulation qc get vbus failed\n");
		return 0;
	}
	proc->vbus = vbus;
	mca_log_info("ibat: %d, ibus: %d, vbus:%d\n", ibat, ibus, vbus);

	if (proc->adp_type == XM_CHARGER_TYPE_HVDCP3_B) {
		ibat_qc_max = info->qc3_ibat_max_limit_ma;
		ibus_qc_max = info->qc3_max_ibus_limit_ma;
	} else {
		ibat_qc_max = info->qc3p5_max_ibat_limit_ma;
		ibus_qc_max = info->qc3p5_max_ibus_limit_ma;
	}
	if (info->bypass_curr)
		ibat_qc_max = min(ibat_qc_max, info->bypass_curr);

	if (thermal >= 1500) {
		fcc_limit = min(thermal, ibat_qc_max);
		ibus_limit = min(thermal / 2 + 100, ibus_qc_max);
		mca_log_info("target_limit_fcc_ma: %d, target_limit_ibus_ma:%d, ibat_qc_max_ma: %d\n",
			fcc_limit, ibus_limit, ibat_qc_max);
	} else if (!thermal) {
		fcc_limit = ibat_qc_max;
		ibus_limit = ibus_qc_max;
	} else {
		return 0;
	}

	/* a gauge reading that has not moved for a few passes is not trusted */
	ibat_stale = ibat == ibat_now_ma_before && ibatt_count < 5;
	if (ibat_stale) {
		ibatt_count++;
	} else {
		ibatt_count = 0;
		ibat_now_ma_before = ibat;
	}

	vbat_stale = vbat == vbat_now_mv_before && vbatt_count < 5;
	if (vbat_stale) {
		vbatt_count++;
	} else {
		vbatt_count = 0;
		vbat_now_mv_before = vbat;
	}

	switch (proc->adp_type) {
	case XM_CHARGER_TYPE_HVDCP3P5:
		if (vbus <= info->qc3p5_max_vbus_limit_mv && ibus < ibus_limit - 100 &&
		    !vbat_stale && vbat < fv - delta_fv - 20 && !ibat_stale && ibat < fcc_limit - 50) {
			mca_log_info("QC3P5 D plus pluse sent\n");
			up = true;
		}
		if ((!vbat_stale && vbat > fv - delta_fv - 10) ||
		    (!ibat_stale && ibat > fcc_limit + 50) || ibus > ibus_limit + 50) {
			mca_log_info("QC3P5 D mius pluse sent\n");
			down = true;
		}
		break;
	case XM_CHARGER_TYPE_HVDCP3_B:
		if (vbus <= info->qc3_max_vbus_limit_mv && ibus < ibus_limit - 800 &&
		    !vbat_stale && vbat < fv - delta_fv - 50 && !ibat_stale && ibat < fcc_limit - 700) {
			mca_log_info("QC3 D plus pluse sent\n");
			up = true;
		}
		if ((!vbat_stale && vbat > fv - delta_fv - 15) ||
		    (!ibat_stale && ibat > fcc_limit) || ibus > ibus_limit + 300) {
			if (++decrement_count == 2) {
				decrement_count = 0;
				mca_log_info("QC3 D mius pluse sent\n");
				down = true;
			}
		}
		break;
	default:
		return 0;
	}

	if (down)
		cmd = QC3_SINGLE_DECREMENT;
	else if (up)
		cmd = QC3_SINGLE_INCREMENT;
	else
		return 0;

	mca_log_info("QC3/3+ set hvdcp cmd %d\n", cmd);
	ret = protocol_class_qc_set_volt_cmd(ADAPTER_PROTOCOL_QC, cmd);
	if (ret)
		mca_log_err("QC3/3+ set pluse hvdcp cmd failed\n");

	return ret;
}

/* Drop to one pump once the bus current has stayed low enough for long enough. */
static void mca_quick_charge_try2_single_path(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	bool master_path = proc->cp_path_enable[CP_ROLE_MASTER];

	if (mca_quick_charge_update_ibus(info))
		return;

	if (proc->ibus > proc->multi_ibus_th - proc->ibus_dec)
		return;

	if (++proc->count <= info->mult_to_single_count)
		return;

	(void)platform_class_cp_set_charging_enable(master_path ? CP_ROLE_SLAVE : CP_ROLE_MASTER,
		false);
	(void)platform_class_cp_enable_adc(master_path ? CP_ROLE_SLAVE : CP_ROLE_MASTER, false);
	proc->cur_work_cp = master_path ? MCA_QUICK_CHG_CP_MASTER : MCA_QUICK_CHG_CP_SLAVE;
}

/* Bring the second pump in once the bus current is high enough. */
static void mca_quick_charge_try2_multi_path(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int role = proc->cur_work_cp == MCA_QUICK_CHG_CP_MASTER ? CP_ROLE_SLAVE : CP_ROLE_MASTER;
	int ibus_th, soc = 0, ibus = 0;

	if (mca_quick_charge_update_ibus(info))
		return;

	ibus = proc->ibus;
	ibus_th = proc->dual_cp_count ? proc->again_multi_ibus_th : proc->multi_ibus_th;
	if (info->support_base_flip) {
		soc = strategy_class_fg_ops_get_soc();
		if (soc <= MCA_QUICK_CHG_MULTI_PATH_SOC_TH)
			ibus_th = proc->multi_ibus_th;
	}
	mca_log_info("soc:%d, multi_ibus_th:%d, dual_cp_count:%d\n", soc, ibus_th,
		proc->dual_cp_count);

	if (ibus < proc->ibus_inc + ibus_th)
		return;

	if (proc->error_num[role] >= MCA_QUICK_CHG_SINGLE_MAX_ERR_COUNT)
		return;

	(void)platform_class_cp_enable_adc(role, true);
	if (platform_class_cp_set_mode(role, proc->cur_cp_mode)) {
		mca_log_err("open path set cp mode fail\n");
		return;
	}

	(void)platform_class_cp_set_charging_enable(role, true);
	if (mca_quick_charge_msleep(500, info))
		return;

	if (platform_class_cp_get_bus_current(role, &ibus)) {
		proc->error_num[role]++;
		(void)platform_class_cp_set_charging_enable(role, false);
		return;
	}

	if (ibus >= MCA_QUICK_CHG_MULTI_PATH_IBUS_TH) {
		proc->cur_work_cp = MCA_QUICK_CHG_CP_DUAL;
		proc->dual_cp_count++;
	}
}

static void mca_quick_charge_change_path(struct mca_quick_charge_info *info)
{
	if (info->cp_type != MCA_CP_TYPE_PARALLEL || info->is_qcom_qc)
		return;

	mca_log_debug("cur_work_cp: %d\n", info->proc_data.cur_work_cp);
	if (info->proc_data.cur_work_cp == MCA_QUICK_CHG_CP_DUAL)
		mca_quick_charge_try2_single_path(info);
	else
		mca_quick_charge_try2_multi_path(info);
}

static void mca_quick_charge_ibus_queue_push(struct mca_quick_charge_info *info, int ibus)
{
	struct mca_quick_charge_ibus_queue_info *queue = &info->proc_data.ibus_queue;

	if (queue->index < 0 || queue->index >= MCA_QUICK_CHG_IBUS_QUEUE_SIZE)
		return;

	queue->sum -= queue->data[queue->index];
	queue->data[queue->index] = ibus;
	queue->sum += ibus;
	queue->count = min(queue->count + 1, MCA_QUICK_CHG_IBUS_QUEUE_SIZE);
	queue->avg = queue->count ? queue->sum / queue->count : 0;
	queue->index = (queue->index + 1) % MCA_QUICK_CHG_IBUS_QUEUE_SIZE;
	mca_log_info("push ibus: %d, index: %d, count: %d, sum: %d, avg: %d",
		ibus, queue->index, queue->count, queue->sum, queue->avg);
}

static int mca_quick_charge_is_charge_abnormal(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	bool master_en = false, slave_en = false;
	bool fg_err = false, fg_err_slave = false;

	if (proc->cur_work_cp == MCA_QUICK_CHG_CP_DUAL) {
		(void)platform_class_cp_get_charging_enabled(CP_ROLE_MASTER, &master_en);
		(void)platform_class_cp_get_charging_enabled(CP_ROLE_SLAVE, &slave_en);
		if (master_en && slave_en)
			return 0;

		mca_log_err("cp work abnormal, master %d slave %d\n", master_en, slave_en);
		proc->total_err++;
		proc->error_num[master_en ? MCA_QUICK_CHG_CP_SLAVE : MCA_QUICK_CHG_CP_MASTER]++;
		(void)platform_class_cp_dump_register(CP_ROLE_MASTER);
		(void)platform_class_cp_dump_register(CP_ROLE_SLAVE);
		return -1;
	}

	(void)platform_class_cp_get_charging_enabled(proc->cur_work_cp, &master_en);
	if (!master_en) {
		mca_log_err("cp work abnormal, role %d enbale %d\n", proc->cur_work_cp, master_en);
		(void)platform_class_cp_dump_register(CP_ROLE_MASTER);
		proc->total_err++;
		if (proc->cur_work_cp >= 0 && proc->cur_work_cp < MCA_QUICK_CHG_CP_MODE_MAX)
			proc->error_num[proc->cur_work_cp]++;
		return -1;
	}

	(void)platform_fg_ops_get_error_state(FG_IC_MASTER, &fg_err);
	if (info->batt_type <= MCA_BATTERY_TYPE_SINGLE) {
		if (!fg_err)
			return 0;
		mca_log_err("fg work abnormal, fg_err %d\n", fg_err);
		return -1;
	}

	(void)platform_fg_ops_get_error_state(FG_IC_SLAVE, &fg_err_slave);
	if (!fg_err && !fg_err_slave)
		return 0;

	mca_log_err("fg work abnormal, fg_err %d fg_err_slave %d\n", fg_err, fg_err_slave);
	return -1;
}

static int mca_qc_quick_charge_is_charge_abnormal(struct mca_quick_charge_info *info)
{
	bool enabled = false;

	(void)platform_class_cp_get_charging_enabled(CP_ROLE_MASTER, &enabled);
	if (enabled)
		return 0;

	mca_log_err("cp work abnormal, enbale %d\n", enabled);
	info->proc_data.total_err++;
	info->proc_data.error_num[MCA_QUICK_CHG_CP_MASTER]++;
	return -1;
}

/*
 * With hardware CV, hand the last stage over once the cell has held its
 * float voltage with the current below the taper threshold for five passes.
 */
static bool mca_check_if_goto_hw_taper(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct mca_quick_charge_volt_para_info *volt_info = proc->cur_volt_para[FG_IC_MASTER];
	int stage = proc->cur_stage[FG_IC_MASTER];
	int ibat_th = info->pps_taper_fcc_ma;
	int vbat_th = volt_info->volt_para[stage / 2].voltage;
	int temp = 0, temp_offset_flag = 0;
	int ffc;

	ffc = strategy_class_fg_get_fastcharge();
	if (!info->hardware_cv)
		return false;

	(void)strategy_class_fg_ops_get_temperature(&temp);
	if (!info->base_flip_same) {
		if (temp >= 300 && temp < 400)
			ibat_th = info->pps_middle_taper_fcc_ma;
		else if (temp >= 400)
			ibat_th = info->pps_high_taper_fcc_ma;
	} else {
		temp /= 10;
		(void)strategy_class_fg_get_temp_offset_flag(&temp_offset_flag);
		if ((temp > 34 || (temp > 32 && temp_offset_flag)) && ffc)
			ibat_th = info->pps_high_taper_fcc_ma;
	}

	if (stage / 2 != volt_info->volt_para_size - 1)
		return false;

	vbat_th -= info->smartchg_data.delta_fv + info->pps_taper_vol_hys + info->quick_chg_fv_hys;
	mca_log_info("check if goto hw taper vbat: %d, vbat_th: %d, ibat: %d, ibat_th: %d, count: %d, quick_chg_fv_hys: %d\n",
		proc->vbat[FG_IC_MASTER], vbat_th, proc->ibat_total, ibat_th,
		info->fc2_taper_timer, info->quick_chg_fv_hys);
	if (proc->vbat[FG_IC_MASTER] < vbat_th || proc->ibat_total >= ibat_th) {
		info->fc2_taper_timer = 0;
		return false;
	}

	if (info->fc2_taper_timer++ > MCA_QUICK_CHG_TAPER_COUNT) {
		info->taper_done_no_retry = true;
		mca_log_info("pps taper done\n");
		return true;
	}

	return false;
}

static bool mca_qc_check_if_goto_taper(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	int hys = proc->adp_type == XM_CHARGER_TYPE_HVDCP3P5 ?
		info->qc3p5_taper_vol_hys : info->qc3_taper_vol_hys;
	int vbat_th;

	info->qc_normal_charge_fv_max_mv = proc->temp_max_fv[FG_IC_MASTER];
	vbat_th = info->qc_normal_charge_fv_max_mv - (hys + info->smartchg_data.delta_fv) - 1;
	mca_log_info("qc_normal_charge_fv_max_mv: %d, vbat_th:%d\n",
		info->qc_normal_charge_fv_max_mv, vbat_th);

	if ((proc->vbat[FG_IC_MASTER] > vbat_th && proc->ibat_total < info->qc_taper_fcc_ma) ||
	    strategy_class_fg_ops_get_soc() > info->qc_taper_soc_thr) {
		if (info->fc2_taper_timer++ > MCA_QUICK_CHG_TAPER_COUNT) {
			info->taper_done_no_retry = true;
			mca_log_info("qc taper done\n");
			return true;
		}
		return false;
	}

	info->fc2_taper_timer = 0;
	return false;
}

/*
 * Without hardware CV, call the charge done once the cell has held its last
 * step's voltage with the current below the termination current for six
 * passes, or straight away when the gauge forces a full report.
 */
static void mca_quick_charge_check_charge_done(struct mca_quick_charge_info *info)
{
	static int count;
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct mca_quick_charge_volt_para_info *volt_info = proc->cur_volt_para[FG_IC_MASTER];
	struct mca_quick_charge_volt_para *last = &volt_info->volt_para[volt_info->volt_para_size - 1];
	struct mca_hwid_info *hwid;
	int iterm = last->current_min;
	int vterm = last->voltage - (info->smartchg_data.delta_fv + info->fv_hys_delta_mv);
	int force_full_status = 0, rsoc = 0;
	int ratio;

	hwid = mca_get_hwid_info();
	(void)mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_FG,
		STRATEGY_STATUS_TYPE_FORCE_FULL_STATUS, &force_full_status);
	(void)strategy_class_fg_ops_get_rsoc(&rsoc);

	ratio = iterm > info->curr_terminate_section ? info->curr_terminate_ratio - 10 :
		info->curr_terminate_ratio;
	if (proc->vbat[FG_IC_MASTER] >= vterm && ratio * iterm / 100 >= proc->ibat[FG_IC_MASTER]) {
		count++;
	} else if (force_full_status && iterm >= proc->ibat[FG_IC_MASTER]) {
		count = MCA_QUICK_CHG_DONE_COUNT;
		mca_log_err("triggle overcharging for termination force_full_status = %d, rsoc =%d\n",
			force_full_status, rsoc);
	} else {
		count = 0;
	}

	mca_log_info("termination iterm: %d, vterm: %d, count:%d, force_full_status:%d, rsoc:%d\n",
		iterm, vterm, count, force_full_status, rsoc);
	if (count < MCA_QUICK_CHG_DONE_COUNT)
		return;

	proc->charge_flag = MCA_QUICK_CHG_STS_CHARGE_DONE;
	if (proc->ffc_flag || (hwid && (hwid->platform_version == QC_HWID_PROJECT_FORCE_DONE_B ||
					hwid->platform_version == QC_HWID_PROJECT_FORCE_DONE_A)))
		(void)strategy_class_fg_ops_set_charging_done(true);
	mca_log_err("quick charge done exit\n");
	count = 0;
}

static void mca_quick_charge_monitor_work(struct work_struct *work)
{
	static int last_ffc_flag;
	struct mca_quick_charge_info *info = container_of(to_delayed_work(work),
		struct mca_quick_charge_info, monitor_work);
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	bool taper_done;
	int abnormal, ret;

	if (!strategy_class_fg_is_chip_ok()) {
		mca_log_info("fg i2c error, exit quick charging\n");
		goto stop;
	}

	if (info->vfc_para.support_cp_vfc)
		mca_quick_charge_ibus_queue_push(info, proc->ibus);

	/* an EU third-party PPS adapter hands FFC over to the PMIC near full */
	if (info->is_eu_model && !proc->ffc_flag && info->third_pps_switch_pmic) {
		mca_quick_charge_update_ibat(info);
		proc->soc = strategy_class_fg_ops_get_soc();
		if (proc->ibat_total < info->cp_switch_pmic_th &&
		    proc->soc >= MCA_QUICK_CHG_FFC_SWITCH_PMIC_SOC) {
			mca_log_err("Normal FFC switch to PMIC Now!\n");
			goto stop;
		}
	}

	if (mca_quick_charge_judge_thermal_vote(info))
		goto stop;

	if (!info->is_qcom_qc) {
		abnormal = mca_quick_charge_is_charge_abnormal(info);
		taper_done = mca_check_if_goto_hw_taper(info);
	} else {
		abnormal = mca_qc_quick_charge_is_charge_abnormal(info);
		taper_done = mca_qc_check_if_goto_taper(info);
	}
	if (abnormal || taper_done)
		goto stop;

	if (mca_quick_charge_can_tbat_do_charge(info, proc->to_pd_verify)) {
		mca_log_info("monitor temp out of range\n");
		goto stop;
	}

	if (info->support_base_flip && mca_quick_charge_select_parallel_volt_para(info)) {
		mca_log_err("get_parallel_volt_para fail\n");
		goto stop;
	}

	mca_quick_charge_select_stage(info);
	if (proc->ffc_flag != last_ffc_flag)
		(void)strategy_class_fg_set_fastcharge(proc->ffc_flag);

	if (!info->hardware_cv)
		mca_quick_charge_check_charge_done(info);

	if (proc->charge_flag == MCA_QUICK_CHG_STS_CHARGE_DONE) {
		mca_log_info("monitor qc charge done\n");
		goto stop;
	}

	if (!info->pmic_single_cp_chg)
		mca_quick_charge_change_path(info);

	if (info->cp_work_mode.support_mode_switch && proc->adp_type == XM_CHARGER_TYPE_PD_VERIFY &&
	    proc->max_power > MCA_QUICK_CHG_MODE_SWITCH_POWER_MIN &&
	    !(info->cur_support_mode & ~proc->adp_mode))
		mca_check_cp_work_mode(info);

	if (!info->is_qcom_qc)
		ret = mca_quick_charge_regulation(info);
	else
		ret = mca_qc_quick_charge_regulation(info);
	if (ret) {
		proc->total_err++;
		mca_log_info("regulation total_err: %d, ret: %d\n", proc->total_err, ret);
		goto stop;
	}

	if (proc->stop_charging) {
		mca_log_info("monitor stop charging\n");
		goto stop;
	}

	last_ffc_flag = proc->ffc_flag;
	schedule_delayed_work(&info->monitor_work, msecs_to_jiffies(MCA_QUICK_CHG_MONITOR_INTERVAL));
	return;

stop:
	mca_quick_charge_stop_charging(info);
}

static void mca_quick_charge_pps_ptf_work(struct work_struct *work)
{
	static int check_mum;
	struct mca_quick_charge_info *info = container_of(to_delayed_work(work),
		struct mca_quick_charge_info, pps_ptf_work);

	if (!((info->proc_data.adp_type == XM_CHARGER_TYPE_PPS && info->is_eu_model) ||
	      info->fake_pps_ptf)) {
		check_mum = 0;
		return;
	}

	(void)protocol_class_get_adapter_pps_ptf(info->proc_data.cur_protocol, &info->pps_ptf);
	if (!info->pps_ptf) {
		info->pps_ptf = info->fake_pps_ptf;
		check_mum++;
		mca_log_info("pps_ptf: %d, check_num: %d\n", info->pps_ptf, check_mum);
		if (check_mum > MCA_QUICK_CHG_PPS_PTF_MAX_CHECK) {
			check_mum = 0;
			mca_log_info("this pps charger not support to get ptf, stop get action\n");
			return;
		}
	} else {
		check_mum = 0;
	}

	schedule_delayed_work(&info->pps_ptf_work, msecs_to_jiffies(MCA_QUICK_CHG_PPS_PTF_INTERVAL));
}

static int strategy_quickchg_map_ibus_to_fsw(struct mca_quick_charge_info *info)
{
	struct mca_quick_charge_process_data *proc = &info->proc_data;
	struct mca_quick_charge_vfc_para *vfc = &info->vfc_para;
	int ibus_avg = proc->cur_work_cp == MCA_QUICK_CHG_CP_DUAL ?
		proc->ibus_queue.avg / 2 : proc->ibus_queue.avg;
	int cp_iout = ibus_avg * proc->ratio;
	int fsw_target = MCA_QUICK_CHG_CP_DEFAULT_FSW;
	int i, map_iout = 0, map_fsw = 0;

	for (i = 0; i < vfc->vfc_para_size && i < MCA_VFC_PARA_MAX_GROUP; i++) {
		if (cp_iout > vfc->iout_fsw_map[i].iout) {
			fsw_target = vfc->iout_fsw_map[i].fsw;
			break;
		}
	}
	if (i < MCA_VFC_PARA_MAX_GROUP) {
		map_iout = vfc->iout_fsw_map[i].iout;
		map_fsw = vfc->iout_fsw_map[i].fsw;
	}

	proc->vfc_iout = cp_iout;
	mca_log_info("ibus_avg: %d, ratio: %d, cp_iout: %d, map_iout_fsw: [%d:%d], fsw_target: %d\n",
		ibus_avg, proc->ratio, cp_iout, map_iout, map_fsw, fsw_target);

	return fsw_target;
}

/* Walk the pump's switching frequency to the target one step at a time. */
static void strategy_quickchg_update_cp_fsw(struct mca_quick_charge_info *info, int fsw_target)
{
	int cur_work_cp = info->proc_data.cur_work_cp;
	int role = cur_work_cp == MCA_QUICK_CHG_CP_DUAL ? CP_ROLE_MASTER : cur_work_cp;
	int cur_fsw = 0, fsw_step = 0;
	int steps, direction, count, fsw;

	(void)platform_class_cp_get_fsw_step(CP_ROLE_MASTER, &fsw_step);
	(void)platform_class_cp_get_fsw(role, &cur_fsw);
	mca_log_info("fsw_target: %d, cur_fsw: %d, fsw_step: %d\n", fsw_target, cur_fsw, fsw_step);
	if (fsw_target == cur_fsw)
		return;

	direction = fsw_target - cur_fsw >= 0 ? 1 : -1;
	steps = fsw_step ? abs(fsw_target - cur_fsw) / fsw_step : 0;
	for (count = 1; count <= steps; count++) {
		fsw = cur_fsw + fsw_step * direction * count;
		if (info->proc_data.cur_work_cp == MCA_QUICK_CHG_CP_DUAL) {
			(void)platform_class_cp_set_fsw(CP_ROLE_MASTER, fsw);
			(void)platform_class_cp_set_fsw(CP_ROLE_SLAVE, fsw);
		} else {
			(void)platform_class_cp_set_fsw(info->proc_data.cur_work_cp, fsw);
		}
	}

	role = info->proc_data.cur_work_cp == MCA_QUICK_CHG_CP_DUAL ? CP_ROLE_MASTER :
		info->proc_data.cur_work_cp;
	(void)platform_class_cp_get_fsw(role, &cur_fsw);
	mca_log_info("after: cur_fsw: %d, direction: %d\n", cur_fsw, direction);
}

static void mca_quick_charge_vfc_work(struct work_struct *work)
{
	struct mca_quick_charge_info *info = container_of(to_delayed_work(work),
		struct mca_quick_charge_info, vfc_work);

	if (!info->vfc_para.support_cp_vfc)
		return;

	strategy_quickchg_update_cp_fsw(info, strategy_quickchg_map_ibus_to_fsw(info));
	schedule_delayed_work(&info->vfc_work, msecs_to_jiffies(MCA_QUICK_CHG_VFC_INTERVAL));
}

/*
 * The flip-base FFC tables are kept per cycle-count band; reload them once
 * either cell has crossed into a new band.
 */
static void update_batt_para(struct mca_quick_charge_info *info)
{
	static const int cycle_band[] = { 0, 50, 100, 150, 300, 800 };
	static int last_batt_index[FG_SITE_MAX];
	static int curr_batt_index[FG_SITE_MAX];
	int cycle[FG_SITE_MAX] = { 0 };
	int i, site;

	(void)platform_fg_ops_get_cyclecount(FG_IC_SLAVE, &cycle[FG_IC_FLIP]);
	(void)platform_fg_ops_get_cyclecount(FG_IC_MASTER, &cycle[FG_IC_BASE]);
	mca_log_err("base_flip_cc, update_batt_para, master:%d, slave:%d\n",
		cycle[FG_IC_BASE], cycle[FG_IC_FLIP]);

	for (i = 0; i < ARRAY_SIZE(cycle_band); i++) {
		for (site = FG_IC_FLIP; site >= FG_IC_BASE; site--) {
			if (cycle[site] > cycle_band[i]) {
				curr_batt_index[site] = i;
				info->curr_batt_cycle[site] = cycle_band[i];
			}
		}
	}

	if (last_batt_index[FG_IC_FLIP] != curr_batt_index[FG_IC_FLIP] ||
	    last_batt_index[FG_IC_BASE] != curr_batt_index[FG_IC_BASE]) {
		(void)mca_quick_charge_parse_batt_info(info, info->batt_type);
		mca_log_err("base_flip_cc, update cc data\n");
		last_batt_index[FG_IC_BASE] = curr_batt_index[FG_IC_BASE];
		last_batt_index[FG_IC_FLIP] = curr_batt_index[FG_IC_FLIP];
	}

	mca_log_err("base_flip_cc, curr_batt_index_master:%d, curr_batt_index_slave:%d\n",
		curr_batt_index[FG_IC_BASE], curr_batt_index[FG_IC_FLIP]);
}

static int mca_quick_charge_process_event(int event, int value, void *data)
{
	struct mca_quick_charge_info *info = data;
	struct mca_quick_charge_process_data *proc;

	if (!info)
		return -1;

	proc = &info->proc_data;
	mca_log_info("event: %d, value: %d\n", event, value);

	switch (event) {
	case MCA_EVENT_USB_DISCONNECT:
		info->online = 0;
		info->is_qcom_qc = false;
		info->taper_done_no_retry = false;
		info->pd_switch_to_pmic = false;
		info->fc2_taper_timer = 0;
		proc->sw_ocp_curr = 0;
		info->cp_work_mode.last_stage = false;
		(void)mca_vote(info->chg_en_voter, "vbat_ovp", false, 1);
		(void)mca_vote(info->chg_en_voter, "lpd", false, 0);
		mca_eu_pps_anti_disconnection_strategy(info);
		mca_log_err("exit quick charge\n");
		cancel_delayed_work_sync(&info->pps_ptf_work);
		mca_quick_charge_exit_charging(info);
		memset(proc, 0, sizeof(*proc));
		(void)strategy_class_fg_set_fastcharge(false);
		break;
	case MCA_EVENT_USB_CONNECT:
		info->online = 1;
		if (info->support_base_flip)
			update_batt_para(info);
		break;
	case MCA_EVENT_CHARGE_TYPE_CHANGE:
		mca_log_info("change type change event %d cur %d\n", value, proc->adp_type);
		if (proc->adp_type == value)
			break;

		proc->adp_type = value;
		switch (value) {
		case XM_CHARGER_TYPE_PD_VERIFY:
			proc->to_pd_verify = true;
			proc->parallel_to_pd_verify = true;
			proc->cur_protocol = ADAPTER_PROTOCOL_PPS;
			if (info->fake_pps_ptf)
				schedule_delayed_work(&info->pps_ptf_work,
					msecs_to_jiffies(MCA_QUICK_CHG_PPS_PTF_INTERVAL));
			break;
		case XM_CHARGER_TYPE_PPS:
			proc->cur_protocol = ADAPTER_PROTOCOL_PPS;
			if (info->fake_pps_ptf || info->is_eu_model)
				schedule_delayed_work(&info->pps_ptf_work,
					msecs_to_jiffies(MCA_QUICK_CHG_PPS_PTF_INTERVAL));
			break;
		case XM_CHARGER_TYPE_HVDCP3_B:
		case XM_CHARGER_TYPE_HVDCP3P5:
			info->is_qcom_qc = true;
			proc->cur_protocol = ADAPTER_PROTOCOL_QC;
			break;
		default:
			break;
		}
		break;
	case MCA_EVENT_CHARGE_CAP_CHANGE:
		proc->type_chg = 1;
		mutex_lock(&info->data_lock);
		mca_log_info("Acquire mutex_lock\n");
		mca_quick_charge_exit_charging(info);
		mutex_unlock(&info->data_lock);
		mca_log_info("Release mutex_lock\n");
		proc->type_chg = 0;
		break;
	case MCA_EVENT_WIRELESS_USB_REVCHG:
		mca_log_info("wireless revchg event %d\n", value);
		(void)mca_vote(info->chg_disable_voter, "wireless_revchg", true, value);
		break;
	case MCA_EVENT_CP_CBOOT_FAIL:
		(void)mca_vote(info->chg_en_voter, "cp_cboot_short", value != 0, value == 0);
		mca_log_info("check cp cboot short to determine whether to exit quick charge");
		break;
	case MCA_EVENT_CONN_ANTIBURN_CHANGE:
		info->trigger_antr_burn = value;
		mca_log_info("anti-burn change event %d\n", value);
		(void)mca_vote(info->chg_disable_voter, "antiburn", true, value);
		break;
	case MCA_EVENT_BATT_BTB_CHANGE:
		info->batt_missing = value;
		(void)mca_vote(info->chg_disable_voter, "batt_miss", true, value);
		break;
	case MCA_EVENT_BATT_AUTH_PASS:
		info->batt_auth = 1;
		break;
	case MCA_EVENT_LPD_STATUS_CHANGE:
		(void)mca_vote(info->chg_en_voter, "lpd", value != 0, value == 0);
		mca_log_info("check lpd status to determine whether to exit quick charge");
		break;
	case MCA_EVENT_CC_SHORT_VBUS:
		(void)mca_vote(info->chg_en_voter, "cc_short_vbus", value != 0, value == 0);
		mca_log_info("check cc short vbus to determine whether to exit quick charge");
		break;
	case MCA_EVENT_VBAT_OVP_CHANGE:
		if (value)
			(void)mca_vote(info->chg_en_voter, "vbat_ovp", true, 0);
		else
			(void)mca_vote(info->chg_en_voter, "vbat_ovp", false, 1);
		break;
	case MCA_EVENT_IBAT_OCP_CHANGE:
		if (value == 1)
			proc->sw_ocp_curr = proc->ocp_thr[1] / 1000;
		else if (value == 2)
			proc->sw_ocp_curr = proc->ocp_thr[0] / 1000;
		else if (value == 3)
			proc->sw_ocp_curr = min(proc->ocp_thr[0], proc->ocp_thr[1]) / 1000;
		else
			proc->sw_ocp_curr = 0;
		mca_log_info("sw_ocp_curr: %d", proc->sw_ocp_curr);
		break;
	case MCA_EVENT_CHARGE_ABNORMAL:
		mca_quick_charge_exit_charging(info);
		break;
	case MCA_EVENT_BATTERY_DTPT:
		info->dtpt_status = value;
		break;
	case MCA_EVENT_CSD_SEND_PULSE:
		mca_log_info("csd pulse event %d\n", value);
		(void)mca_vote(info->chg_disable_voter, "csd_pulse", true, value);
		break;
	case MCA_EVENT_PPS_PTF:
		info->pps_ptf = value;
		mca_log_info("pps_ptf= %d\n", value);
		if (info->pps_ptf == USBPD_QUICK_DPM_PORT_PPS_PTF_WARNING) {
			mca_log_info("exit quick charge when PPS_PTF too high %d\n", value);
			(void)mca_vote(info->chg_en_voter, "ptf", true, 0);
		} else {
			(void)mca_vote(info->chg_en_voter, "ptf", false, 1);
		}
		break;
	case MCA_EVENT_IS_EU_MODEL:
		mca_log_err("set is_eu_model %d\n", value);
		info->is_eu_model = value;
		break;
	case MCA_EVENT_PLATE_SHOCK:
		(void)mca_vote(info->chg_en_voter, "plate_shock", value != 0, 0);
		break;
	case MCA_EVENT_HANDLE_ALLOW_CHARGE:
		(void)mca_vote(info->chg_en_voter, "handle", value == 0, 0);
		break;
	case MCA_EVENT_MASTER_BATT_CLOSE:
		mca_log_err("master batt close event %d\n", value);
		proc->base_close_curr = value * MCA_BASE_CLOSE_CURR_RATIO / 10;
		proc->stop_charging = true;
		proc->base_batt_close = true;
		break;
	case MCA_EVENT_CHARGE_ACTION:
		mca_quick_charge_pre_charge_check(info);
		break;
	case MCA_EVENT_SINK_PWR_SUSPEND_CHANGE:
		if (value) {
			(void)platform_class_cp_set_charging_enable(CP_ROLE_MASTER, false);
			(void)platform_class_cp_set_charging_enable(CP_ROLE_SLAVE, false);
			mca_log_info("Enter bSinkPowerSuspend\n");
		}
		(void)mca_vote(info->chg_disable_voter, "bSinkPowerSuspend", true, value);
		break;
	case MCA_EVENT_FG_OTA_PROCESS:
		mca_log_info("fg_ota_process event %d\n", value);
		(void)mca_vote(info->chg_disable_voter, "fg_ota_process", true, value);
		break;
	case MCA_EVENT_FCC_TOO_LOW:
		mca_log_info("fcc too low event, update normal volt para\n");
		info->fcc_too_low = true;
		if (info->support_base_flip) {
			(void)mca_quick_charge_can_tbat_do_charge(info, true);
			info->parallel_fcc_too_low[FG_IC_BASE] = true;
			info->parallel_fcc_too_low[FG_IC_FLIP] = true;
			if (mca_quick_charge_parallel_judge_temp(info))
				mca_log_err("temp range invalid, update fail!\n");
		} else if (mca_quick_charge_can_tbat_do_charge(info, true)) {
			mca_log_err("temp range invalid, update fail!\n");
		}
		break;
	case MCA_EVENT_DEBUG_CTRL_SOC_LIMIT:
		(void)mca_vote(info->chg_disable_voter, "debug_soc_limit", true, value);
		break;
	default:
		break;
	}

	return 0;
}

static int mca_quick_charge_get_status(int status, int *value, void *data)
{
	struct mca_quick_charge_info *info = data;
	struct mca_quick_charge_process_data *proc;
	struct mca_quick_charge_volt_para_info *volt_info;
	struct mca_quick_charge_sysfs_data *sysfs;

	if (!info)
		return -1;

	proc = &info->proc_data;
	sysfs = &info->sysfs_data;
	switch (status) {
	case STRATEGY_STATUS_TYPE_CHARGING:
		*value = proc->charge_flag;
		break;
	case STRATEGY_STATUS_TYPE_QC_TYPE:
		*value = proc->quick_charge_type;
		break;
	case STRATEGY_STATUS_TYPE_QC_IBAT_MAX:
		*value = info->current_fcc;
		break;
	case STRATEGY_STATUS_TYPE_POWER_MAX:
		*value = proc->ui_power / 1000;
		break;
	case STRATEGY_STATUS_TYPE_ENABLE:
		if (info->force_stop)
			*value = 0;
		else
			*value = (sysfs->chg_enable << 9) |
				(sysfs->mode_enable[CHG_MODE_DIV1] << 8) |
				(sysfs->cp_path_enable[CHG_MODE_DIV1][CP_ROLE_MASTER] << 7) |
				(sysfs->cp_path_enable[CHG_MODE_DIV1][CP_ROLE_SLAVE] << 6) |
				(sysfs->mode_enable[CHG_MODE_DIV2] << 5) |
				(sysfs->cp_path_enable[CHG_MODE_DIV2][CP_ROLE_MASTER] << 4) |
				(sysfs->cp_path_enable[CHG_MODE_DIV2][CP_ROLE_SLAVE] << 3) |
				(sysfs->mode_enable[CHG_MODE_DIV4] << 2) |
				(sysfs->cp_path_enable[CHG_MODE_DIV4][CP_ROLE_MASTER] << 1) |
				sysfs->cp_path_enable[CHG_MODE_DIV4][CP_ROLE_MASTER];
		break;
	case STRATEGY_STATUS_TYPE_MODE:
		*value = proc->cur_cp_mode;
		break;
	case STRATEGY_STATUS_TYPE_VBUS:
		*value = proc->vbus;
		break;
	case STRATEGY_STATUS_TYPE_IBUS:
		*value = proc->ibus;
		break;
	case STRATEGY_STATUS_TYPE_JEITA_FFC_ITERM:
		volt_info = proc->cur_volt_para[FG_IC_MASTER];
		if (!volt_info || !volt_info->volt_para_size)
			*value = 1000;
		else
			*value = volt_info->volt_para[volt_info->volt_para_size - 1].current_min;
		break;
	case STRATEGY_STATUS_TYPE_CP_TO_PMIC:
		*value = info->taper_done_no_retry;
		break;
	case STRATEGY_STATUS_TYPE_DEVICE_POWER_MAX:
		*value = info->max_power;
		break;
	case STRATEGY_STATUS_TYPE_GET_VOLTAGE_MAX:
		if (!info->is_qcom_qc)
			*value = proc->cur_adp_volt;
		else if (proc->adp_type == XM_CHARGER_TYPE_HVDCP3P5)
			*value = proc->vbus;
		else
			*value = MCA_QC_VOLTAGE_MAX_DEFAULT_MV;
		break;
	case STRATEGY_STATUS_TYPE_GET_CURRENT_MAX:
		if (!info->is_qcom_qc)
			*value = proc->max_adp_curr;
		else if (proc->adp_type == XM_CHARGER_TYPE_HVDCP3P5 &&
			 proc->vbus > 8400 && proc->vbus <= 9900)
			*value = MCA_QC_CURRENT_MAX_HIGH_MA;
		else
			*value = MCA_QC_CURRENT_MAX_DEFAULT_MA;
		break;
	default:
		return -1;
	}

	return 0;
}

static int mca_quick_charge_parse_volt_para(struct device_node *node, const char *name,
	struct mca_quick_charge_volt_para_info *volt_info)
{
	struct mca_quick_charge_volt_para *volt_para;
	const char *tmp_string = NULL;
	int count, rows, i, ret;
	int *field;

	if (!strcmp(name, "null")) {
		mca_log_info("no need parse volt para\n");
		return 0;
	}

	count = mca_parse_dts_count_strings(node, name, VOLT_PARA_MAX_GROUP,
		MCA_QUICK_CHG_VOLT_PARA_MAX);
	if (count < 0) {
		mca_log_err("parse %s failed\n", name);
		return -1;
	}

	rows = count / MCA_QUICK_CHG_VOLT_PARA_MAX;
	volt_info->volt_para_size = rows;
	volt_para = kcalloc(rows, sizeof(*volt_para), GFP_KERNEL);
	volt_info->volt_para = volt_para;
	if (!volt_para) {
		mca_log_err("volt para no mem\n");
		return -ENOMEM;
	}

	for (i = 0; i < count; i++) {
		if (mca_parse_dts_string_index(node, name, i, &tmp_string))
			return -1;

		switch (i % MCA_QUICK_CHG_VOLT_PARA_MAX) {
		case MCA_QUICK_CHG_VOLTAGE:
			field = &volt_para[i / MCA_QUICK_CHG_VOLT_PARA_MAX].voltage;
			break;
		case MCA_QUICK_CHG_CURRENT_MAX:
			field = &volt_para[i / MCA_QUICK_CHG_VOLT_PARA_MAX].current_max;
			break;
		default:
			field = &volt_para[i / MCA_QUICK_CHG_VOLT_PARA_MAX].current_min;
			break;
		}

		ret = kstrtoint(tmp_string, 10, field);
		if (ret) {
			kfree(volt_para);
			volt_info->volt_para = NULL;
			volt_info->volt_para_size = 0;
			return -1;
		}
	}

	for (i = 0; i < rows; i++)
		mca_log_err("volt_para %d %d %d\n", volt_para[i].voltage,
			volt_para[i].current_max, volt_para[i].current_min);

	return 0;
}

/* Which of a step table's stages are skipped, two per step. */
static void mca_quick_charge_parse_stage_para(struct device_node *node, const char *name,
	struct mca_quick_charge_volt_para_info *volt_info)
{
	int count = volt_info->volt_para_size * 2;

	if (!strcmp(name, "null")) {
		mca_log_info("no need parse stage para\n");
		return;
	}

	volt_info->stage_para = count >= 0 ? kcalloc(count, sizeof(int), GFP_KERNEL) : NULL;
	if (!volt_info->volt_para || !volt_info->stage_para) {
		mca_log_err("volt para no mem\n");
		return;
	}

	(void)mca_parse_dts_u32_array(node, name, (u32 *)volt_info->stage_para, count);
}

/* Flip-base FFC tables that follow the cell's cycle count, by band start (degC). */
static bool mca_quick_charge_is_cc_ffc_temp(int temp_low)
{
	return temp_low == 23 || temp_low == 30 || temp_low == 40;
}

static int mca_quick_charge_parse_temp_para(struct mca_quick_charge_info *info,
	struct device_node *node, const char *name, int role, int mode)
{
	struct mca_quick_charge_batt_para_info *batt_para;
	struct mca_quick_charge_temp_para_info *temp_info, *ti;
	struct device_node *cc_node = info->dev->of_node;
	const char *tmp_string = NULL;
	char cc_name[50] = { 0 };
	int count, rows, i, ret;

	if (info->support_base_flip) {
		cc_node = of_find_node_by_name(NULL, "mca_parallel_cyclecount_para");
		if (!cc_node) {
			mca_log_err("node in null\n");
			return -1;
		}
	}

	count = mca_parse_dts_count_strings(node, name, TEMP_PARA_MAX_GROUP,
		MCA_QUICK_CHG_TEMP_PARA_MAX);
	if (count < 0) {
		mca_log_err("parse %s failed\n", name);
		return -1;
	}

	batt_para = mode ? &info->base_flip_para[role] : &info->batt_para[role];
	rows = count / MCA_QUICK_CHG_TEMP_PARA_MAX;
	batt_para->temp_para_size = rows;
	temp_info = kcalloc(rows, sizeof(*temp_info), GFP_KERNEL);
	batt_para->temp_info = temp_info;
	if (!temp_info) {
		mca_log_err("temp para no mem\n");
		return -1;
	}

	for (i = 0; i < count; i++) {
		if (mca_parse_dts_string_index(node, name, i, &tmp_string))
			return -1;

		ti = &temp_info[i / MCA_QUICK_CHG_TEMP_PARA_MAX];
		switch (i % MCA_QUICK_CHG_TEMP_PARA_MAX) {
		case MCA_QUICK_CHG_TEMP_LOW:
			ret = kstrtoint(tmp_string, 10, &ti->temp_para.temp_low);
			break;
		case MCA_QUICK_CHG_TEMP_HIGH:
			ret = kstrtoint(tmp_string, 10, &ti->temp_para.temp_high);
			break;
		case MCA_QUICK_CHG_LOW_TEMP_HYS:
			ret = kstrtoint(tmp_string, 10, &ti->temp_para.low_temp_hysteresis);
			break;
		case MCA_QUICK_CHG_HIGH_TEMP_HYS:
			ret = kstrtoint(tmp_string, 10, &ti->temp_para.high_temp_hysteresis);
			break;
		case MCA_QUICK_CHG_TEMP_MAX_CURRENT:
			ret = kstrtoint(tmp_string, 10, &ti->temp_para.max_current);
			break;
		case MCA_QUICK_CHG_TEMP_NROMAL_FV:
			ret = kstrtoint(tmp_string, 10, &ti->temp_para.normal_max_fv);
			break;
		case MCA_QUICK_CHG_TEMP_FFC_FV:
			ret = kstrtoint(tmp_string, 10, &ti->temp_para.ffc_max_fv);
			break;
		case MCA_QUICK_CHG_VOLT_PARA_NAME:
			ret = mca_quick_charge_parse_volt_para(node, tmp_string, &ti->volt_info);
			break;
		case MCA_QUICK_CHG_VOLT_FFC_PARA_NAME:
			if (mode && info->support_base_flip &&
			    mca_quick_charge_is_cc_ffc_temp(ti->temp_para.temp_low)) {
				snprintf(cc_name, sizeof(cc_name), "%s_cc%d",
					role ? "flip_ffc_volt_para" : "base_ffc_volt_para",
					info->curr_batt_cycle[role]);
				mca_log_err("base/flip FFC cc cv para:%s\n", cc_name);
				ret = mca_quick_charge_parse_volt_para(cc_node, cc_name,
					&ti->volt_ffc_info);
			} else {
				ret = mca_quick_charge_parse_volt_para(node, tmp_string,
					&ti->volt_ffc_info);
			}
			break;
		case MCA_QUICK_CHG_STAGE_PARA_NAME:
			mca_quick_charge_parse_stage_para(node, tmp_string, &ti->volt_info);
			ret = 0;
			break;
		default:
			mca_quick_charge_parse_stage_para(node, tmp_string, &ti->volt_ffc_info);
			ret = 0;
			break;
		}

		if (ret) {
			kfree(temp_info);
			batt_para->temp_info = NULL;
			batt_para->temp_para_size = 0;
			return -1;
		}
	}

	for (i = 0; i < rows; i++) {
		ti = &temp_info[i];
		mca_log_err("temp_para %d %d %d %d %d %d %d %d %d\n",
			ti->temp_para.temp_low, ti->temp_para.temp_high,
			ti->temp_para.low_temp_hysteresis, ti->temp_para.high_temp_hysteresis,
			ti->temp_para.max_current, ti->temp_para.normal_max_fv,
			ti->temp_para.ffc_max_fv, ti->volt_info.volt_para_size,
			ti->volt_ffc_info.volt_para_size);
	}

	return 0;
}

/* Two packs share the "atl" cell name but need their own tables. */
static void mca_quick_charge_check_model_name(const char **cell_name)
{
	const char *device_name = NULL;

	(void)platform_fg_ops_get_device_name(FG_IC_MASTER, &device_name);
	if (!device_name) {
		mca_log_err("get device name  fail\n");
		return;
	}

	mca_log_err("device name: %s, name: %s\n", device_name, *cell_name);
	if (strcmp("atl", *cell_name))
		return;

	if (!strcmp("3XM31", device_name)) {
		*cell_name = "atl2";
	} else if (!strcmp("2@BP", device_name)) {
		mca_log_info("project O9 matched XM22@BP, adjust cell_name\n");
		*cell_name = "atl2";
	}
}

static int mca_quick_charge_parse_batt_info(struct mca_quick_charge_info *info, int mode)
{
	static const char * const battpara[] = { "batt_para", "base_flip_para" };
	struct device_node *node = info->dev->of_node;
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	const char *main_cell_name = NULL;
	const char *slave_cell_name = NULL;
	const char *tmp_string = NULL;
	const char *name;
	int count, i, role = 0;

	/* outside China the tables come from the global node */
	if (hwid && info->has_gbl_batt_para && hwid->country_version != CountryCN)
		node = of_find_node_by_name(NULL, "mca_quick_charge_batt_para_gbl");
	if (!node) {
		mca_log_err("node in null\n");
		return -1;
	}

	if (mode < 0 || mode >= ARRAY_SIZE(battpara))
		return -1;
	name = battpara[mode];

	count = mca_parse_dts_count_strings(node, name, BATT_PARA_MAX_GROUP,
		MCA_QUICK_CHG_BATT_PARA_MAX);
	if (count < 0) {
		mca_log_err("parse batt_para failed\n");
		return -1;
	}

	(void)platform_fg_ops_get_batt_cell_info(FG_IC_MASTER, &main_cell_name);
	if (!main_cell_name) {
		mca_log_err("get main cell name fail\n");
		return -EINVAL;
	}
	mca_quick_charge_check_model_name(&main_cell_name);
	mca_log_info("main_cell_name %s\n", main_cell_name);

	if (info->batt_type > MCA_BATTERY_TYPE_SINGLE) {
		(void)platform_fg_ops_get_batt_cell_info(FG_IC_SLAVE, &slave_cell_name);
		if (!slave_cell_name) {
			mca_log_err("get slave cell name fail\n");
			return -EINVAL;
		}
		mca_log_info("slave_cell_name %s\n", slave_cell_name);
	}

	for (i = 0; i < count; i++) {
		if (mca_parse_dts_string_index(node, name, i, &tmp_string))
			return -1;

		switch (i % MCA_QUICK_CHG_BATT_PARA_MAX) {
		case MCA_QUICK_CHG_BATT_ROLE:
			if (kstrtoint(tmp_string, 10, &role))
				return -1;
			if (role < 0 || role >= FG_IC_MAX)
				i += 2;
			break;
		case MCA_QUICK_CHG_BATT_ID:
			if ((role == FG_IC_MASTER && strcmp(tmp_string, main_cell_name)) ||
			    (role == FG_IC_SLAVE && slave_cell_name &&
			     strcmp(tmp_string, slave_cell_name))) {
				mca_log_info("can not match cell name\n");
				i++;
			}
			break;
		default:
			if (mca_quick_charge_parse_temp_para(info, node, tmp_string, role, mode))
				return -1;
			break;
		}
	}

	if (!info->batt_para[FG_IC_MASTER].temp_info) {
		mca_log_err("parse master batt para failed\n");
		return -1;
	}

	return 0;
}

static void mca_quick_charge_parse_u32_array(struct device_node *node, const char *prop,
	int *dst, int count, const int *def)
{
	u32 data[4] = { 0 };

	if (mca_parse_dts_u32_array(node, prop, data, count))
		memcpy(dst, def, count * sizeof(int));
	else
		memcpy(dst, data, count * sizeof(int));
}

static void mca_quick_charge_parse_chg_mode_info(struct device_node *node,
	struct mca_quick_charge_info *info)
{
	static const int def_delta_volt[] = {
		MCA_QUICK_CHG_DIV1_VOLT_DELTA_DEFAULT,
		MCA_QUICK_CHG_DIV2_VOLT_DELTA_DEFAULT,
		MCA_QUICK_CHG_DIV4_VOLT_DELTA_DEFAULT,
	};
	static const int def_delta_ibat[] = {
		MCA_QUICK_CHG_DEFAULT_IBAT_DELTA,
		MCA_QUICK_CHG_DEFAULT_IBAT_DELTA,
		MCA_QUICK_CHG_DEFAULT_IBAT_DELTA,
	};
	static const int def_multi_ibus_th[] = {
		MCA_QUICK_CHG_IBUS_TH_DEFAULT,
		MCA_QUICK_CHG_IBUS_TH_DEFAULT,
		MCA_QUICK_CHG_IBUS_TH_DEFAULT,
	};
	static const int def_again_multi_ibus_th[] = {
		MCA_QUICK_CHG_AGAIN_IBUS_TH_DEFAULT,
		MCA_QUICK_CHG_AGAIN_IBUS_TH_DEFAULT,
		MCA_QUICK_CHG_AGAIN_IBUS_TH_DEFAULT,
	};
	static const int def_ibus_inc[] = {
		MCA_QUICK_CHG_IBUS_INC_HYS_DEFAULT,
		MCA_QUICK_CHG_IBUS_INC_HYS_DEFAULT,
		MCA_QUICK_CHG_IBUS_INC_HYS_DEFAULT,
	};
	static const int def_ibus_dec[] = {
		MCA_QUICK_CHG_IBUS_DEC_HYS_DEFAULT,
		MCA_QUICK_CHG_IBUS_DEC_HYS_DEFAULT,
		MCA_QUICK_CHG_IBUS_DEC_HYS_DEFAULT,
	};
	static const int def_open_path_th[] = {
		MCA_QUICK_CHG_OPEN_PATH_IBUS_TH,
		MCA_QUICK_CHG_OPEN_PATH_IBUS_TH,
		MCA_QUICK_CHG_OPEN_PATH_IBUS_TH,
	};
	static const int def_zero[] = { 0, 0, 0 };
	u32 data[CHG_MODE_MAX] = { 0 };

	mca_quick_charge_parse_u32_array(node, "div_delta_volt", info->div_delta_volt,
		CHG_MODE_MAX, def_delta_volt);
	mca_log_info("div_delta_volt %d %d %d\n", info->div_delta_volt[CHG_MODE_DIV1],
		info->div_delta_volt[CHG_MODE_DIV2], info->div_delta_volt[CHG_MODE_DIV4]);

	mca_quick_charge_parse_u32_array(node, "div_delta_ibat", info->div_delta_ibat,
		CHG_MODE_MAX, def_delta_ibat);
	mca_log_info("div_delta_ibat %d %d %d\n", info->div_delta_ibat[CHG_MODE_DIV1],
		info->div_delta_ibat[CHG_MODE_DIV2], info->div_delta_ibat[CHG_MODE_DIV4]);

	if (mca_parse_dts_u32_array(node, "div_single_curr", data, CHG_MODE_MAX)) {
		info->div_single_curr[CHG_MODE_DIV1] = MCA_QUICK_CHG_SINGLE_DIV1_CURR_TH;
		info->div_single_curr[CHG_MODE_DIV2] = MCA_QUICK_CHG_SINGLE_DIV2_CURR_TH;
		info->div_single_curr[CHG_MODE_DIV4] = MCA_QUICK_CHG_SINGLE_DIV4_CURR_TH;
	} else {
		memcpy(info->div_single_curr, data, sizeof(data));
		if (info->en_buck_parallel_chg && mca_quick_charge_is_en_buck_hwid())
			info->div_single_curr[CHG_MODE_DIV4] = MCA_QUICK_CHG_SINGLE_DIV4_CURR_EN_BUCK;
	}
	mca_log_info("div_single_curr %d %d %d\n", info->div_single_curr[CHG_MODE_DIV1],
		info->div_single_curr[CHG_MODE_DIV2], info->div_single_curr[CHG_MODE_DIV4]);

	mca_quick_charge_parse_u32_array(node, "buck_icl_fcc_curr", info->buck_icl_fcc_curr,
		2, def_zero);
	mca_log_info("buck_icl_fcc_curr %d %d\n", info->buck_icl_fcc_curr[0],
		info->buck_icl_fcc_curr[1]);

	mca_quick_charge_parse_u32_array(node, "div_max_curr", info->div_max_curr,
		CHG_MODE_MAX, info->div_single_curr);
	mca_log_info("div_max_curr %d %d %d\n", info->div_max_curr[CHG_MODE_DIV1],
		info->div_max_curr[CHG_MODE_DIV2], info->div_max_curr[CHG_MODE_DIV4]);

	mca_quick_charge_parse_u32_array(node, "multi_ibus_th", info->multi_ibus_th,
		CHG_MODE_MAX, def_multi_ibus_th);
	mca_log_info("multi_ibus_th %d %d %d\n", info->multi_ibus_th[CHG_MODE_DIV1],
		info->multi_ibus_th[CHG_MODE_DIV2], info->multi_ibus_th[CHG_MODE_DIV4]);

	mca_quick_charge_parse_u32_array(node, "again_multi_ibus_th", info->again_multi_ibus_th,
		CHG_MODE_MAX, def_again_multi_ibus_th);
	mca_log_info("again_multi_ibus_th %d %d %d\n", info->again_multi_ibus_th[CHG_MODE_DIV1],
		info->again_multi_ibus_th[CHG_MODE_DIV2], info->again_multi_ibus_th[CHG_MODE_DIV4]);

	mca_quick_charge_parse_u32_array(node, "ibus_inc_hysteresis", info->ibus_inc_hysteresis,
		CHG_MODE_MAX, def_ibus_inc);
	mca_log_info("ibus_inc_hysteresis %d %d %d\n", info->ibus_inc_hysteresis[CHG_MODE_DIV1],
		info->ibus_inc_hysteresis[CHG_MODE_DIV2], info->ibus_inc_hysteresis[CHG_MODE_DIV4]);

	mca_quick_charge_parse_u32_array(node, "ibus_dec_hysteresis", info->ibus_dec_hysteresis,
		CHG_MODE_MAX, def_ibus_dec);
	mca_log_info("ibus_dec_hysteresis %d %d %d\n", info->ibus_dec_hysteresis[CHG_MODE_DIV1],
		info->ibus_dec_hysteresis[CHG_MODE_DIV2], info->ibus_dec_hysteresis[CHG_MODE_DIV4]);

	mca_quick_charge_parse_u32_array(node, "open_path_th", info->open_path_th,
		CHG_MODE_MAX, def_open_path_th);
	mca_log_info("open_path_th %d %d %d\n", info->open_path_th[CHG_MODE_DIV1],
		info->open_path_th[CHG_MODE_DIV2], info->open_path_th[CHG_MODE_DIV4]);

	mca_quick_charge_parse_u32_array(node, "ibus_compensation", info->ibus_compensation,
		CHG_MODE_MAX, def_zero);
	mca_log_info("ibus_compensation: %d %d %d\n", info->ibus_compensation[CHG_MODE_DIV1],
		info->ibus_compensation[CHG_MODE_DIV2], info->ibus_compensation[CHG_MODE_DIV4]);

	info->cp_work_mode.support_mode_switch = of_property_read_bool(node,
		"support-cp-mode-switch");
	if (info->cp_work_mode.support_mode_switch &&
	    mca_parse_dts_u32_array(node, "cp-mode-switch-threshold",
				    (u32 *)info->cp_work_mode.mode_table, CHG_MODE_MAX * 2))
		info->cp_work_mode.support_mode_switch = false;
	mca_log_info("support_mode_switch: %d, threshold: [%d %d] [%d %d] [%d %d]\n",
		info->cp_work_mode.support_mode_switch,
		info->cp_work_mode.mode_table[CHG_MODE_DIV1].l_thres,
		info->cp_work_mode.mode_table[CHG_MODE_DIV1].h_thres,
		info->cp_work_mode.mode_table[CHG_MODE_DIV2].l_thres,
		info->cp_work_mode.mode_table[CHG_MODE_DIV2].h_thres,
		info->cp_work_mode.mode_table[CHG_MODE_DIV4].l_thres,
		info->cp_work_mode.mode_table[CHG_MODE_DIV4].h_thres);
}

static void mca_quick_charge_parse_vstep_para(struct mca_quick_charge_info *info)
{
	struct device_node *node = info->dev->of_node;
	int data[VSTEP_PARA_MAX_GROUP * MCA_QUICK_CHG_VSTEP_PARA_MAX] = { 0 };
	int len, row, *col;

	len = mca_parse_dts_string_array(node, "vstep_para", data, VSTEP_PARA_MAX_GROUP,
		MCA_QUICK_CHG_VSTEP_PARA_MAX);
	for (row = 0; row < len / MCA_QUICK_CHG_VSTEP_PARA_MAX && row < VSTEP_PARA_MAX_GROUP; row++) {
		col = &data[row * MCA_QUICK_CHG_VSTEP_PARA_MAX];
		info->vstep_para[row].volt_ratio = col[MCA_QUICK_CHG_VOLT_RATIO];
		info->vstep_para[row].cur_gap = col[MCA_QUICK_CHG_CURRENT];
		info->vstep_para[row].vstep_ratio = col[MCA_QUICK_CHG_VSTEP_RATIO];
	}
}

static void mca_quick_charge_parse_vstep_para_ex(struct mca_quick_charge_info *info)
{
	struct device_node *node = info->dev->of_node;
	int data[VSTEP_PARA_EX_MAX_GROUP * MCA_QUICK_CHG_VSTEP_PARA_EX_MAX] = { 0 };
	int len, row, *col;

	mca_log_err("parse vstep_ex_para\n");
	len = mca_parse_dts_string_array(node, "vstep_para_ex", data, VSTEP_PARA_EX_MAX_GROUP,
		MCA_QUICK_CHG_VSTEP_PARA_EX_MAX);
	for (row = 0; row < len / MCA_QUICK_CHG_VSTEP_PARA_EX_MAX && row < VSTEP_PARA_EX_MAX_GROUP;
	     row++) {
		col = &data[row * MCA_QUICK_CHG_VSTEP_PARA_EX_MAX];
		info->vstep_para_ex[row].delta_v_thr = col[MCA_QUICK_CHG_DELTA_V_THR];
		info->vstep_para_ex[row].vstep_multiplier = col[MCA_QUICK_CHG_VSTEP_MULTIPLIER];
		info->vstep_para_ex[row].vbat_change_thr = col[MCA_QUICK_CHG_VBAT_CHANGE_THR];
		info->vstep_para_ex[row].boost_time_thr = (s64)col[MCA_QUICK_CHG_BOOST_TIME_THR];
	}
}

/* Used only on boards that charge through one pump with the buck alongside. */
static void mca_quick_charge_parse_pmic_single_cp_charging_info(struct device_node *node,
	struct mca_quick_charge_info *info)
{
	static const int def_cur_max_th[] = {
		MCA_QUICK_CHG_CUR_MAX_TH_DEFAULT_0,
		MCA_QUICK_CHG_CUR_MAX_TH_DEFAULT_1,
		MCA_QUICK_CHG_CUR_MAX_TH_DEFAULT_2,
	};
	static const int def_ibus_th[] = {
		MCA_QUICK_CHG_IBUS_TH_PMIH_DEFAULT_0,
		MCA_QUICK_CHG_IBUS_TH_PMIH_DEFAULT_1,
		MCA_QUICK_CHG_IBUS_TH_PMIH_DEFAULT_2,
	};
	static const int def_pmih_fcc[] = {
		MCA_QUICK_CHG_PMIH_FCC_DEFAULT_0,
		MCA_QUICK_CHG_PMIH_FCC_DEFAULT_1,
		MCA_QUICK_CHG_PMIH_FCC_DEFAULT_2,
		MCA_QUICK_CHG_PMIH_FCC_DEFAULT_3,
	};

	mca_quick_charge_parse_u32_array(node, "cur_max_threshold", info->cur_max_threshold,
		3, def_cur_max_th);
	mca_log_info("cur_max_threshold %d %d %d\n", info->cur_max_threshold[0],
		info->cur_max_threshold[1], info->cur_max_threshold[2]);

	mca_quick_charge_parse_u32_array(node, "ibus_threshold", info->ibus_threshold,
		3, def_ibus_th);
	mca_log_info("ibus_threshold %d %d %d\n", info->ibus_threshold[0],
		info->ibus_threshold[1], info->ibus_threshold[2]);

	mca_quick_charge_parse_u32_array(node, "pmih_fcc_value", info->pmih_fcc_value,
		4, def_pmih_fcc);
	mca_log_info("pmih_fcc_value %d %d %d %d\n", info->pmih_fcc_value[0],
		info->pmih_fcc_value[1], info->pmih_fcc_value[2], info->pmih_fcc_value[3]);

	(void)mca_parse_dts_u32(node, "vbat_threshold", &info->vbat_threshold,
		MCA_QUICK_CHG_VBAT_THRESHOLD_DEFAULT);
	mca_log_info("vbat_threshold :%d\n", info->vbat_threshold);
}

static void mca_quick_charge_parse_vfc_info(struct mca_quick_charge_info *info)
{
	struct device_node *node = info->dev->of_node;
	struct mca_quick_charge_vfc_para *vfc = &info->vfc_para;
	int data[MCA_VFC_PARA_MAX_GROUP * MCA_QUICK_CHG_VFC_PARA_SIZE] = { 0 };
	int len, row;

	(void)mca_parse_dts_u32(node, "support_cp_vfc", &vfc->support_cp_vfc, 0);
	if (!vfc->support_cp_vfc) {
		mca_log_err("not support cp vfc\n");
		return;
	}

	len = mca_parse_dts_string_array(node, "vfc_iout_fsw_map", data, MCA_VFC_PARA_MAX_GROUP,
		MCA_QUICK_CHG_VFC_PARA_SIZE);
	if (len < 0) {
		mca_log_err("parse vfc_iout_fsw_map failed\n");
		return;
	}

	vfc->vfc_para_size = min(len / MCA_QUICK_CHG_VFC_PARA_SIZE, MCA_VFC_PARA_MAX_GROUP);
	for (row = 0; row < vfc->vfc_para_size; row++) {
		vfc->iout_fsw_map[row].iout = data[row * MCA_QUICK_CHG_VFC_PARA_SIZE +
			MCA_QUICK_CHG_VFC_PARA_IOUT];
		vfc->iout_fsw_map[row].fsw = data[row * MCA_QUICK_CHG_VFC_PARA_SIZE +
			MCA_QUICK_CHG_VFC_PARA_FSW];
	}
}

static int mca_quick_charge_parse_dt(struct mca_quick_charge_info *info)
{
	struct device_node *node = info->dev->of_node;
	int ret;

	if (!node) {
		pr_err("device tree node missing\n");
		return -EINVAL;
	}

	(void)mca_parse_dts_u32(node, "batt_type", &info->batt_type, 0);
	(void)mca_parse_dts_u32(node, "cp_type", &info->cp_type, 0);
	(void)mca_parse_dts_u32(node, "en_buck_parallel_chg", &info->en_buck_parallel_chg, 0);
	(void)mca_parse_dts_u32(node, "fv_hys_delta_mv", &info->fv_hys_delta_mv, 0);
	(void)mca_parse_dts_u32(node, "curr_terminate_ratio", &info->curr_terminate_ratio,
		MCA_QUICK_CHG_CURR_TERM_RATIO_DEFAULT);
	(void)mca_parse_dts_u32(node, "curr_terminate_section", &info->curr_terminate_section,
		MCA_QUICK_CHG_CURR_TERM_SECTION_DEFAULT);
	(void)mca_parse_dts_u32(node, "min_vbat", &info->min_vbat, MCA_QUICK_CHG_MIN_VBAT_DEFAULT);
	(void)mca_parse_dts_u32(node, "max_vbat", &info->max_vbat, MCA_QUICK_CHG_MAX_VBAT_DEFAULT);
	(void)mca_parse_dts_u32(node, "recharge_vbat", &info->recharge_vbat,
		MCA_QUICK_CHG_RECHARGE_VBAT_DEFAULT);
	(void)mca_parse_dts_u32(node, "die_temp_max", &info->die_temp_max,
		MCA_QUICK_CHG_MAX_TDIE_DEFAULT);
	(void)mca_parse_dts_u32(node, "adp_temp_max", &info->adp_temp_max,
		MCA_QUICK_CHG_MAX_TADP_DEFAULT);
	(void)mca_parse_dts_u32(node, "support_mode", &info->support_mode, 0);
	(void)mca_parse_dts_u32(node, "support_ichg_cutoff_priority",
		&info->support_ichg_cutoff_priority, 1);
	(void)mca_parse_dts_u32(node, "max_power", &info->max_power, 0);
	(void)mca_parse_dts_u32(node, "pmic_fv_compensation", &info->pmic_fv_compensation, 0);
	(void)mca_parse_dts_u32(node, "ui_max_power_limit", &info->ui_max_power_limit,
		info->max_power);
	(void)mca_parse_dts_u32(node, "qc3_max_vbus_limit", &info->qc3_max_vbus_limit_mv,
		MCA_QUICK_CHG_QC3B_VBUS_LIMIT_DEFAULT);
	(void)mca_parse_dts_u32(node, "qc3_max_ibus_limit", &info->qc3_max_ibus_limit_ma,
		MCA_QUICK_CHG_QC3B_IBUS_LIMIT_DEFAULT);
	(void)mca_parse_dts_u32(node, "qc3p5_max_vbus_limit", &info->qc3p5_max_vbus_limit_mv,
		MCA_QUICK_CHG_QC3P5_VBUS_LIMIT_DEFAULT);
	(void)mca_parse_dts_u32(node, "qc3p5_max_ibus_limit", &info->qc3p5_max_ibus_limit_ma,
		MCA_QUICK_CHG_QC3P5_IBUS_LIMIT_DEFAULT);
	(void)mca_parse_dts_u32(node, "qc3_ibat_max_limit", &info->qc3_ibat_max_limit_ma,
		MCA_QUICK_CHG_QC3_IBAT_LIMIT_DEFAULT);
	(void)mca_parse_dts_u32(node, "qc3p5_ibat_max_limit", &info->qc3p5_max_ibat_limit_ma,
		MCA_QUICK_CHG_QC3_IBAT_LIMIT_DEFAULT);
	(void)mca_parse_dts_u32(node, "qc_normal_charge_fv", &info->qc_normal_charge_fv_max_mv,
		MCA_QUICK_CHG_QC_FV_LIMIT_DEFAULT);
	(void)mca_parse_dts_u32(node, "qc_taper_fcc_thr", &info->qc_taper_fcc_ma,
		MCA_QUICK_CHG_QC_TAPER_FCC_THR_DEFAULT);
	(void)mca_parse_dts_u32(node, "qc_taper_soc_thr", &info->qc_taper_soc_thr,
		MCA_QUICK_CHG_QC_TAPER_SOC_THR_DEFAULT);
	(void)mca_parse_dts_u32(node, "pps_taper_fcc_thr", &info->pps_taper_fcc_ma,
		MCA_QUICK_CHG_QC_TAPER_FCC_THR_DEFAULT);
	(void)mca_parse_dts_u32(node, "pps_middle_taper_fcc_thr", &info->pps_middle_taper_fcc_ma,
		info->pps_taper_fcc_ma);
	(void)mca_parse_dts_u32(node, "pps_high_taper_fcc_thr", &info->pps_high_taper_fcc_ma,
		info->pps_taper_fcc_ma);
	(void)mca_parse_dts_u32(node, "qc3_taper_vol_hys", &info->qc3_taper_vol_hys,
		MCA_QUICK_CHG_QC_TAPER_HYS_QC3B);
	(void)mca_parse_dts_u32(node, "qc3p5_taper_vol_hys", &info->qc3p5_taper_vol_hys,
		MCA_QUICK_CHG_QC_TAPER_HYS_QC3P5);
	(void)mca_parse_dts_u32(node, "pps_taper_vol_hys", &info->pps_taper_vol_hys,
		MCA_QUICK_CHG_PPS_TAPER_HYS);
	(void)mca_parse_dts_u32(node, "hardware_cv", &info->hardware_cv, 0);
	(void)mca_parse_dts_u32(node, "support_curr_monitor", &info->support_curr_monitor, 0);
	(void)mca_parse_dts_u32(node, "curr_monitor_time_s", &info->curr_monitor_time_s, 0);
	(void)mca_parse_dts_u32(node, "cp_switch_pmic_th", &info->cp_switch_pmic_th,
		MCA_QUICK_CHG_SWITCH_PMIC_TH);
	(void)mca_parse_dts_u32(node, "third_pps_switch_pmic", &info->third_pps_switch_pmic, 0);
	(void)mca_parse_dts_u32(node, "quick_chg_fv_hys", &info->quick_chg_fv_hys,
		MCA_QUICK_CHG_FV_HYS_DEFAULT);
	(void)mca_parse_dts_u32(node, "max_vbat_threshold", &info->max_vbat_threshold,
		MCA_QUICK_CHG_MAX_VBAT_THRESHOLD_DEFAULT);
	(void)mca_parse_dts_u32(node, "mult_to_single_count", &info->mult_to_single_count,
		MCA_QUICK_CHG_MULT_TO_SINGLE_COUNT_DEFAULT);
	info->has_gbl_batt_para = of_property_read_bool(node, "has-global-batt-para");
	info->support_pmic_vterm_dynamics_adjust =
		of_property_read_bool(node, "support-pmic-vterm-dynamics-adjust");
	info->support_dtpt_ichg_dec = of_property_read_bool(node, "support-dtpt-ichg-dec");
	info->enable_vstep_ex = of_property_read_bool(node, "enable-vstep-ex");
	info->support_base_flip = of_property_read_bool(node, "support-base-flip");
	info->support_cancel_compensation =
		of_property_read_bool(node, "support_cancel_compensation");
	info->pmic_chg_need_fixed_volt = of_property_read_bool(node, "pmic-chg-need-fixed-volt");
	info->base_flip_same = of_property_read_bool(node, "base-flip-same");
	info->pmic_single_cp_chg = of_property_read_bool(node, "pmic_single_cp_chg");
	(void)mca_parse_dts_u32(node, "use_buck_fcc_threshold", &info->use_buck_fcc_threshold,
		MCA_QUICK_CHG_USE_BUCK_FCC_TH_DEFAULT);
	(void)mca_parse_dts_u32(node, "third_pps_ibat_limit", &info->third_pps_ibat_limit,
		MCA_QUICK_CHG_THIRD_PPS_IBAT_LIMIT_DEFAULT);
	info->support_eu_power = of_property_read_bool(node, "support-eu-power");
	(void)mca_parse_dts_u32(node, "eu-vbus-max", &info->eu_vbus_max, 0);
	(void)mca_parse_dts_u32(node, "eu-ibus-max", &info->eu_ibus_max, 0);

	mca_quick_charge_parse_chg_mode_info(node, info);
	mca_quick_charge_parse_vstep_para(info);
	if (info->enable_vstep_ex)
		mca_quick_charge_parse_vstep_para_ex(info);

	ret = mca_quick_charge_parse_batt_info(info, 0);
	if (info->support_base_flip) {
		mca_log_err("parse mode for paraller batt\n");
		ret = mca_quick_charge_parse_batt_info(info, info->batt_type);
	}

	if (info->pmic_single_cp_chg)
		mca_quick_charge_parse_pmic_single_cp_charging_info(node, info);

	mca_quick_charge_parse_vfc_info(info);

	if (mca_parse_dts_u32_array(node, "ocp_threshold", (u32 *)info->proc_data.ocp_thr, 2)) {
		info->proc_data.ocp_thr[0] = MCA_QUICK_CHG_OCP_THRESHOLD_DEFAULT_0;
		info->proc_data.ocp_thr[1] = MCA_QUICK_CHG_OCP_THRESHOLD_DEFAULT_1;
		mca_log_err("parse ocp_threshold failed, use default value\n");
	}
	mca_log_info("ocp_threshold : %d %d\n", info->proc_data.ocp_thr[0],
		info->proc_data.ocp_thr[1]);

	return ret;
}

static ssize_t mca_quick_charge_sysfs_show(struct device *dev,
	struct device_attribute *attr, char *buf);
static ssize_t mca_quick_charge_sysfs_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count);

static struct mca_sysfs_attr_info mca_quick_charge_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(mca_quick_charge_sysfs, 0664, MCA_QUICK_CHG_MODE_ENABLE, mode_enable),
	mca_sysfs_attr_rw(mca_quick_charge_sysfs, 0664, MCA_QUICK_CHG_CP_PATH_ENABLE, cp_path_enable),
	mca_sysfs_attr_ro(mca_quick_charge_sysfs, 0440, MCA_QUICK_CHG_POWER_MAX, power_max),
	mca_sysfs_attr_rw(mca_quick_charge_sysfs, 0664, MCA_QUICK_CHG_FAKE_PPS_PTF, fake_pps_ptf),
	mca_sysfs_attr_ro(mca_quick_charge_sysfs, 0440, MCA_QUICK_CHG_TYPE, quick_charge_type),
	mca_sysfs_attr_rw(mca_quick_charge_sysfs, 0664, MCA_QUICK_CHG_CURR_LIMIT, current_limit),
	mca_sysfs_attr_rw(mca_quick_charge_sysfs, 0664, MCA_QUICK_CHG_CURR_RATIO, current_ration),
	mca_sysfs_attr_rw(mca_quick_charge_sysfs, 0664, MCA_QUICK_CHG_VOLT_DEC, volt_dec),
	mca_sysfs_attr_ro(mca_quick_charge_sysfs, 0440, MCA_QUICK_CHG_VFC_IOUT, vfc_iout),
};

#define MCA_QUICK_CHG_ATTRS_SIZE ARRAY_SIZE(mca_quick_charge_sysfs_field_tbl)

static struct attribute *mca_quick_charge_sysfs_attrs[MCA_QUICK_CHG_ATTRS_SIZE + 1];

static const struct attribute_group mca_quick_charge_sysfs_attr_group = {
	.attrs = mca_quick_charge_sysfs_attrs,
};

static ssize_t mca_quick_charge_sysfs_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct mca_quick_charge_info *info = dev_get_drvdata(dev);
	struct mca_quick_charge_sysfs_data *sysfs;
	struct mca_sysfs_attr_info *attr_info;

	if (!info)
		return -1;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name, mca_quick_charge_sysfs_field_tbl,
		MCA_QUICK_CHG_ATTRS_SIZE);
	if (!attr_info)
		return -1;

	sysfs = &info->sysfs_data;
	switch (attr_info->sysfs_attr_name) {
	case MCA_QUICK_CHG_MODE_ENABLE:
		return snprintf(buf, PAGE_SIZE, "div4_en=%d div2_en=%d div1_en=%d\n",
			sysfs->mode_enable[CHG_MODE_DIV4], sysfs->mode_enable[CHG_MODE_DIV2],
			sysfs->mode_enable[CHG_MODE_DIV1]);
	case MCA_QUICK_CHG_CP_PATH_ENABLE:
		return snprintf(buf, PAGE_SIZE,
			"div4_m=%d div4_s=%d div2_m=%d div2_s=%d div1_m=%d div1_s=%d\n",
			sysfs->cp_path_enable[CHG_MODE_DIV4][CP_ROLE_MASTER],
			sysfs->cp_path_enable[CHG_MODE_DIV4][CP_ROLE_SLAVE],
			sysfs->cp_path_enable[CHG_MODE_DIV2][CP_ROLE_MASTER],
			sysfs->cp_path_enable[CHG_MODE_DIV2][CP_ROLE_SLAVE],
			sysfs->cp_path_enable[CHG_MODE_DIV1][CP_ROLE_MASTER],
			sysfs->cp_path_enable[CHG_MODE_DIV1][CP_ROLE_SLAVE]);
	case MCA_QUICK_CHG_POWER_MAX:
		return snprintf(buf, PAGE_SIZE, "%d\n", info->proc_data.ui_power);
	case MCA_QUICK_CHG_FAKE_PPS_PTF:
		return snprintf(buf, PAGE_SIZE, "%d\n", info->fake_pps_ptf);
	case MCA_QUICK_CHG_TYPE:
		return snprintf(buf, PAGE_SIZE, "%d\n", info->proc_data.quick_charge_type);
	case MCA_QUICK_CHG_CURR_LIMIT:
		return snprintf(buf, PAGE_SIZE,
			"div4_s=%d div4_m=%d div2_s=%d div2_m=%d div1_s=%d div1_m=%d\n",
			sysfs->curr_limit[CHG_MODE_DIV4][MCA_QUICK_CHG_CH_SINGLE],
			sysfs->curr_limit[CHG_MODE_DIV4][MCA_QUICK_CHG_CH_MULTI],
			sysfs->curr_limit[CHG_MODE_DIV2][MCA_QUICK_CHG_CH_SINGLE],
			sysfs->curr_limit[CHG_MODE_DIV2][MCA_QUICK_CHG_CH_MULTI],
			sysfs->curr_limit[CHG_MODE_DIV1][MCA_QUICK_CHG_CH_SINGLE],
			sysfs->curr_limit[CHG_MODE_DIV1][MCA_QUICK_CHG_CH_MULTI]);
	case MCA_QUICK_CHG_CURR_RATIO:
		return snprintf(buf, PAGE_SIZE, "%d\n", sysfs->cur_ratio);
	case MCA_QUICK_CHG_VOLT_DEC:
		return snprintf(buf, PAGE_SIZE, "%d\n", sysfs->volt_dec);
	case MCA_QUICK_CHG_VFC_IOUT:
		return snprintf(buf, PAGE_SIZE, "%d\n", info->proc_data.vfc_iout);
	default:
		return 0;
	}
}

static void mca_quick_charge_set_chg_enable(const char *buf, struct mca_quick_charge_info *info)
{
	char type_name[MCA_QUICK_CHG_NAME_LEN] = { 0 };
	int value = 0;
	int mode;

	if (sscanf(buf, "%15s %d", type_name, &value) != 2) {
		mca_log_err("unable to parse input:%s\n", buf);
		return;
	}

	if (!strcmp(type_name, "all")) {
		mca_log_info("set all mode enable %d\n", value);
		for (mode = CHG_MODE_DIV1; mode < CHG_MODE_MAX; mode++)
			info->sysfs_data.mode_enable[mode] = value;
		if (value) {
			info->cur_support_mode = info->support_mode;
			return;
		}
		info->cur_support_mode = 0;
	} else {
		if (!strcmp(type_name, "div4"))
			mode = CHG_MODE_DIV4;
		else if (!strcmp(type_name, "div2"))
			mode = CHG_MODE_DIV2;
		else if (!strcmp(type_name, "div1"))
			mode = CHG_MODE_DIV1;
		else
			return;

		if (info->sysfs_data.mode_enable[mode] == value)
			return;

		info->sysfs_data.mode_enable[mode] = value;
		if (value) {
			info->cur_support_mode |= BIT(mode);
			return;
		}
		info->cur_support_mode &= ~BIT(mode);
		if (info->proc_data.work_mode != BIT(mode))
			return;
	}

	if (info->proc_data.charge_flag == MCA_QUICK_CHG_STS_CHARGING)
		mca_quick_charge_exit_charging(info);
}

static void mca_quick_charge_set_cp_path_enable(const char *buf,
	struct mca_quick_charge_info *info)
{
	char path_name[MCA_QUICK_CHG_NAME_LEN] = { 0 };
	int mode, cp_role, value;

	while (true) {
		while (*buf == ' ')
			buf++;
		if (!*buf)
			return;

		if (sscanf(buf, "div%d_%15s %d", &mode, path_name, &value) != 3)
			return;

		switch (mode) {
		case MCA_QUICK_CHG_MODE_DIV_1:
			mode = CHG_MODE_DIV1;
			break;
		case MCA_QUICK_CHG_MODE_DIV_2:
			mode = CHG_MODE_DIV2;
			break;
		case MCA_QUICK_CHG_MODE_DIV_4:
			mode = CHG_MODE_DIV4;
			break;
		default:
			return;
		}

		/* one "divN_<path> V" entry, V a single digit */
		if (!strcmp(path_name, "master")) {
			cp_role = CP_ROLE_MASTER;
			buf += strlen("div1_master 1");
		} else if (!strcmp(path_name, "slave")) {
			cp_role = CP_ROLE_SLAVE;
			buf += strlen("div1_slave 1");
		} else {
			return;
		}

		info->sysfs_data.cp_path_enable[mode][cp_role] = value;
	}
}

static ssize_t mca_quick_charge_sysfs_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct mca_quick_charge_info *info = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *attr_info;

	if (!info)
		return -1;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name, mca_quick_charge_sysfs_field_tbl,
		MCA_QUICK_CHG_ATTRS_SIZE);
	if (!attr_info)
		return -1;

	switch (attr_info->sysfs_attr_name) {
	case MCA_QUICK_CHG_MODE_ENABLE:
		mca_quick_charge_set_chg_enable(buf, info);
		break;
	case MCA_QUICK_CHG_CP_PATH_ENABLE:
		mca_quick_charge_set_cp_path_enable(buf, info);
		break;
	case MCA_QUICK_CHG_FAKE_PPS_PTF:
		(void)sscanf(buf, "%d", &info->fake_pps_ptf);
		info->pps_ptf = info->fake_pps_ptf;
		(void)mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, MCA_EVENT_PPS_PTF,
			info->pps_ptf);
		(void)mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, MCA_EVENT_PPS_PTF,
			info->pps_ptf);
		break;
	case MCA_QUICK_CHG_CURR_RATIO:
		(void)sscanf(buf, "%d", &info->sysfs_data.cur_ratio);
		break;
	case MCA_QUICK_CHG_VOLT_DEC:
		(void)sscanf(buf, "%d", &info->sysfs_data.volt_dec);
		break;
	default:
		break;
	}

	return count;
}

static void mca_quick_charge_create_group(struct device *dev)
{
	mca_sysfs_init_attrs(mca_quick_charge_sysfs_attrs, mca_quick_charge_sysfs_field_tbl,
		MCA_QUICK_CHG_ATTRS_SIZE);
	(void)mca_sysfs_create_link_group("charger", "quick_charge", dev,
		&mca_quick_charge_sysfs_attr_group);
}

static void mca_quick_charge_remove_group(struct device *dev)
{
	mca_sysfs_remove_link_group("charger", "quick_charge", dev,
		&mca_quick_charge_sysfs_attr_group);
}

/* Put the port back to 5V and the pumps to their defaults before power-off. */
static int mca_quick_charge_shutdown(struct notifier_block *nb, unsigned long action, void *data)
{
	struct mca_quick_charge_info *info = container_of(nb, struct mca_quick_charge_info,
		shutdown_notifier);
	struct mca_event_notify_data n_data = { 0 };

	mca_log_err("receive shutdown\n");
	(void)platform_class_cp_set_charging_enable(CP_ROLE_MASTER, false);
	(void)platform_class_cp_set_charging_enable(CP_ROLE_SLAVE, false);
	(void)platform_class_cp_enable_adc(CP_ROLE_MASTER, false);
	(void)platform_class_cp_enable_adc(CP_ROLE_SLAVE, false);
	mca_quick_charge_set_cp_default_fsw(info);

	info->pd_switch_to_pmic = true;
	info->proc_data.cur_adp_volt = MCA_QUICK_CHG_ADP_DEFAULT_VOLT;
	info->shutdown_mode = 1;
	if (mca_quick_charge_req_adp_volt_and_cur(info))
		mca_log_err("stop req volt failed\n");

	if (info->en_buck_parallel_chg)
		strategy_quickchg_enable_buck_charging(info, 0, 0, false);

	if (info->proc_data.ffc_flag)
		(void)strategy_class_fg_set_fastcharge(false);

	n_data.event = "MCA_LOG_FULL_EVENT";
	n_data.event_len = 18;
	mca_event_report_uevent(&n_data);
	msleep(20);

	mca_log_err("end shutdown\n");
	return 0;
}

static int mca_quick_charge_thermal_cb(struct mca_quick_charge_info *info, int mode, int ch,
	int effective_result)
{
	if (!info)
		return -1;

	info->thermal_cur[mode][ch] = effective_result;
	return 0;
}

static int mca_quick_charge_div1_single_voter_cb(struct mca_votable *votable,
	void *data, int effective_result, const char *effective_client)
{
	mca_log_info("target thermal effective_result :%d\n", effective_result);
	return mca_quick_charge_thermal_cb(data, CHG_MODE_DIV1, MCA_QUICK_CHG_CH_SINGLE,
		effective_result);
}

static int mca_quick_charge_div1_multi_voter_cb(struct mca_votable *votable,
	void *data, int effective_result, const char *effective_client)
{
	mca_log_info("target thermal effective_result :%d\n", effective_result);
	return mca_quick_charge_thermal_cb(data, CHG_MODE_DIV1, MCA_QUICK_CHG_CH_MULTI,
		effective_result);
}

static int mca_quick_charge_div2_single_voter_cb(struct mca_votable *votable,
	void *data, int effective_result, const char *effective_client)
{
	mca_log_info("target thermal effective_result :%d\n", effective_result);
	return mca_quick_charge_thermal_cb(data, CHG_MODE_DIV2, MCA_QUICK_CHG_CH_SINGLE,
		effective_result);
}

static int mca_quick_charge_div2_multi_voter_cb(struct mca_votable *votable,
	void *data, int effective_result, const char *effective_client)
{
	mca_log_info("target thermal effective_result :%d\n", effective_result);
	return mca_quick_charge_thermal_cb(data, CHG_MODE_DIV2, MCA_QUICK_CHG_CH_MULTI,
		effective_result);
}

static int mca_quick_charge_div4_single_voter_cb(struct mca_votable *votable,
	void *data, int effective_result, const char *effective_client)
{
	mca_log_info("target thermal effective_result :%d\n", effective_result);
	return mca_quick_charge_thermal_cb(data, CHG_MODE_DIV4, MCA_QUICK_CHG_CH_SINGLE,
		effective_result);
}

static int mca_quick_charge_div4_multi_voter_cb(struct mca_votable *votable,
	void *data, int effective_result, const char *effective_client)
{
	mca_log_info("target thermal effective_result :%d\n", effective_result);
	return mca_quick_charge_thermal_cb(data, CHG_MODE_DIV4, MCA_QUICK_CHG_CH_MULTI,
		effective_result);
}

static int mca_quick_charge_thermal_flip_voter_cb(struct mca_votable *votable,
	void *data, int effective_result, const char *effective_client)
{
	struct mca_quick_charge_info *info = data;

	if (!info)
		return -1;

	mca_log_info("target thermal flip effective_result :%d\n", effective_result);
	info->thermal_flip_cur = effective_result;
	return 0;
}

static int mca_quick_charge_chg_disable_voter_cb(struct mca_votable *votable,
	void *data, int effective_result, const char *effective_client)
{
	struct mca_quick_charge_info *info = data;

	if (!info)
		return -1;

	info->force_stop = effective_result;
	mca_log_info("force_stop: %d\n", effective_result);
	if (info->force_stop)
		mca_quick_charge_exit_charging(info);

	return 0;
}

static int mca_quick_charge_chg_en_voter_cb(struct mca_votable *votable,
	void *data, int effective_result, const char *effective_client)
{
	struct mca_quick_charge_info *info = data;

	if (!info)
		return -1;

	info->sysfs_data.chg_enable = effective_result;
	if (!effective_result)
		mca_quick_charge_exit_charging(info);

	return 0;
}

/* Hand an interface current limit to each division's thermal voter of one kind. */
static void mca_quick_charge_vote_chg_cur(struct mca_quick_charge_info *info, int ch, int cur)
{
	int mode;

	for (mode = CHG_MODE_DIV1; mode < CHG_MODE_MAX; mode++)
		(void)mca_vote(info->voter[mode * MCA_QUICK_CHG_CH_MAX + ch], "if_ops", cur != 0, cur);
}

static int mca_quick_charge_single_chg_cur_voter_cb(struct mca_votable *votable,
	void *data, int effective_result, const char *effective_client)
{
	struct mca_quick_charge_info *info = data;

	if (!info)
		return -1;

	info->sysfs_data.chg_limit[MCA_QUICK_CHG_CH_SINGLE] = effective_result;
	mca_quick_charge_vote_chg_cur(info, MCA_QUICK_CHG_CH_SINGLE, effective_result);
	return 0;
}

static int mca_quick_charge_multi_chg_cur_voter_cb(struct mca_votable *votable,
	void *data, int effective_result, const char *effective_client)
{
	struct mca_quick_charge_info *info = data;

	if (!info)
		return -1;

	info->sysfs_data.chg_limit[MCA_QUICK_CHG_CH_MULTI] = effective_result;
	mca_quick_charge_vote_chg_cur(info, MCA_QUICK_CHG_CH_MULTI, effective_result);
	return 0;
}

static int mca_quick_charge_create_voter(struct mca_quick_charge_info *info)
{
	static const char * const thermal_name[] = {
		"div1_single", "div1_multi", "div2_single", "div2_multi", "div4_single", "div4_multi",
	};
	static int (* const thermal_cb[])(struct mca_votable *, void *, int, const char *) = {
		mca_quick_charge_div1_single_voter_cb,
		mca_quick_charge_div1_multi_voter_cb,
		mca_quick_charge_div2_single_voter_cb,
		mca_quick_charge_div2_multi_voter_cb,
		mca_quick_charge_div4_single_voter_cb,
		mca_quick_charge_div4_multi_voter_cb,
	};
	struct mca_votable *delta_ichg_voter;
	int i;

	for (i = 0; i < ARRAY_SIZE(thermal_name); i++) {
		info->voter[i] = mca_create_votable(thermal_name[i], MCA_VOTE_MIN, thermal_cb[i],
			0, info);
		if (IS_ERR(info->voter[i]))
			goto err;
	}

	info->thermal_flip_voter = mca_create_votable("thermal_flip", MCA_VOTE_MIN,
		mca_quick_charge_thermal_flip_voter_cb, 0, info);
	if (IS_ERR(info->thermal_flip_voter))
		goto err;

	info->chg_disable_voter = mca_create_votable("quick_chg_disable", MCA_VOTE_OR,
		mca_quick_charge_chg_disable_voter_cb, 0, info);
	if (IS_ERR(info->chg_disable_voter))
		goto err;

	info->chg_en_voter = mca_create_votable("quick_chg_en", MCA_VOTE_AND,
		mca_quick_charge_chg_en_voter_cb, 1, info);
	if (IS_ERR(info->chg_en_voter))
		goto err;

	info->single_chg_cur_voter = mca_create_votable("single_chg_cur", MCA_VOTE_MIN,
		mca_quick_charge_single_chg_cur_voter_cb, 0, info);
	if (IS_ERR(info->single_chg_cur_voter))
		goto err;

	info->multi_chg_cur_voter = mca_create_votable("multi_chg_cur", MCA_VOTE_MIN,
		mca_quick_charge_multi_chg_cur_voter_cb, 0, info);
	if (IS_ERR(info->multi_chg_cur_voter))
		goto err;

	delta_ichg_voter = mca_find_votable("smartchg_delta_ichg");
	if (delta_ichg_voter)
		info->smartchg_data.delta_ichg = mca_get_effective_result(delta_ichg_voter);

	mca_log_info("create voter success\n");
	return 0;

err:
	mca_log_err("init voter failed\n");
	return -1;
}

static int strategy_quickchg_if_set_input_suspend(const char *user, char *value, void *data)
{
	struct mca_quick_charge_info *info = data;
	int suspend = 0;

	if (!user || !value || !info)
		return -1;

	if (kstrtoint(value, 0, &suspend))
		return -1;

	mca_log_err("set_input_suspend: %d\n", suspend);
	if (suspend) {
		info->proc_data.charge_flag = MCA_QUICK_CHG_STS_NO_CHARGING;
		info->cp_work_mode.last_stage = false;
		info->taper_done_no_retry = false;
	}
	(void)mca_vote(info->chg_en_voter, user, true, !suspend);

	return 0;
}

static int strategy_quickchg_if_get_input_suspend(char *buf, void *data)
{
	struct mca_quick_charge_info *info = data;
	const char *client;

	if (!buf || !info)
		return -1;

	client = mca_get_effective_client(info->chg_en_voter);
	if (!client)
		return -1;

	scnprintf(buf, MCA_CHARGE_IF_MAX_VALUE_BUFF, "eff_client:%s %d", client,
		!mca_get_effective_result(info->chg_en_voter));

	return 0;
}

static int strategy_quickchg_if_set_chg_en(const char *user, u32 value, void *data)
{
	struct mca_quick_charge_info *info = data;

	if (!user || !info)
		return -1;

	(void)mca_vote(info->chg_en_voter, user, true, value != 0);

	return 0;
}

static int strategy_quickchg_if_get_chg_en(char *buf, void *data)
{
	struct mca_quick_charge_info *info = data;
	const char *client;

	if (!buf || !info)
		return -1;

	client = mca_get_effective_client(info->chg_en_voter);
	if (!client)
		return -1;

	scnprintf(buf, MCA_CHARGE_IF_MAX_VALUE_BUFF, "eff_client:%s %d", client,
		mca_get_effective_result(info->chg_en_voter));

	return 0;
}

static int strategy_quickchg_if_set_chg_cur(const char *user, char *value, void *data)
{
	struct mca_quick_charge_info *info = data;
	int single_cur = 0, multi_cur = 0;

	if (!user || !value || !info)
		return -1;

	if (sscanf(value, "%d#%d", &single_cur, &multi_cur) != 2)
		return -1;

	(void)mca_vote(info->single_chg_cur_voter, user, true, single_cur);
	(void)mca_vote(info->multi_chg_cur_voter, user, true, multi_cur);

	return 0;
}

static int strategy_quickchg_if_get_chg_cur(char *buf, void *data)
{
	struct mca_quick_charge_info *info = data;
	const char *single_client, *multi_client;

	if (!buf || !info)
		return -1;

	single_client = mca_get_effective_client(info->single_chg_cur_voter);
	if (!single_client)
		return -1;
	multi_client = mca_get_effective_client(info->multi_chg_cur_voter);
	if (!multi_client)
		return -1;

	scnprintf(buf, MCA_CHARGE_IF_MAX_VALUE_BUFF,
		"single:eff_client:%s %d multi:eff_client:%s %d",
		single_client, mca_get_effective_result(info->single_chg_cur_voter),
		multi_client, mca_get_effective_result(info->multi_chg_cur_voter));

	return 0;
}

static struct mca_charge_if_ops g_strategy_quickchg_if_ops = {
	.type_name = "quick",
	.set_input_suspend = strategy_quickchg_if_set_input_suspend,
	.get_input_suspend = strategy_quickchg_if_get_input_suspend,
	.set_charge_enable = strategy_quickchg_if_set_chg_en,
	.get_charge_enable = strategy_quickchg_if_get_chg_en,
	.set_charge_current_limit = strategy_quickchg_if_set_chg_cur,
	.get_charge_current_limit = strategy_quickchg_if_get_chg_cur,
};

static int strategy_quickchg_smartchg_delta_fv_callback(void *data, int effective_result)
{
	struct mca_quick_charge_info *info = data;
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	int delta_fv = effective_result;

	if (!info)
		return -1;

	/* the O9 global build gives up at most 20mV to the smart-charge trim */
	if (hwid && hwid->platform_version == HARDWARE_PROJECT_O9 &&
	    hwid->country_version == CountryGlobal)
		delta_fv = min(effective_result, 20);

	info->smartchg_data.delta_fv = delta_fv;
	mca_log_err("effective_result: %d, info->smartchg_data.delta_fv %d\n",
		effective_result, delta_fv);

	return 0;
}

static int strategy_quickchg_smartchg_delta_ichg_callback(void *data, int effective_result)
{
	struct mca_quick_charge_info *info = data;

	if (!info)
		return -1;

	mca_log_err("effective_result: %d\n", effective_result);
	info->smartchg_data.delta_ichg = effective_result;

	return 0;
}

static int strategy_quickchg_smartchg_set_fcc_callback(void *data, int fcc)
{
	struct mca_quick_charge_info *info = data;

	if (!info)
		return -1;

	info->bypass_curr = fcc;

	return 0;
}

static int strategy_quickchg_smartchg_soc_limit_callback(void *data, int effective_result)
{
	struct mca_quick_charge_info *info = data;

	if (!info)
		return -1;

	mca_log_err("effective_result: %d\n", effective_result);
	(void)mca_vote(info->chg_disable_voter, "soc_limit", true, effective_result);

	return 0;
}

static int strategy_quickchg_smartchg_pwr_boost_sts_callback(void *data, int enable)
{
	struct mca_quick_charge_info *info = data;

	if (!info)
		return -1;

	info->smartchg_data.pwr_boost_state = enable;

	return 0;
}

static int strategy_quickchg_smartchg_baa_update_volt_info(
	struct mca_quick_charge_temp_para_info *temp_info, struct smart_batt_spec *spec)
{
	struct mca_quick_charge_volt_para_info *volt_info;
	struct mca_quick_charge_volt_para *new_para, *old_para;
	int i;

	volt_info = spec->ffc ? &temp_info->volt_ffc_info : &temp_info->volt_info;
	mca_log_info("wired para type: %d, ffc: %d, idx: %d, size: %d => %d, temp: %d:%d => %d:%d\n",
		spec->type, spec->ffc, spec->t_range.idx, volt_info->volt_para_size,
		spec->step_size, temp_info->temp_para.temp_low, temp_info->temp_para.temp_high,
		spec->t_range.min, spec->t_range.max);

	new_para = kcalloc(spec->step_size, sizeof(*new_para), GFP_KERNEL);
	if (!new_para) {
		mca_log_err("volt para no mem\n");
		return -ENOMEM;
	}

	for (i = 0; i < volt_info->volt_para_size; i++)
		mca_log_info("volt_para[%d] voltage: %d, current_max: %d, current_min: %d\n",
			i, volt_info->volt_para[i].voltage, volt_info->volt_para[i].current_max,
			volt_info->volt_para[i].current_min);

	for (i = 0; i < spec->step_size; i++) {
		new_para[i].voltage = spec->steps[i].mv;
		new_para[i].current_max = spec->steps[i].ma_h;
		new_para[i].current_min = spec->steps[i].ma_l;
		mca_log_info("new_volt_para[%d] voltage: %d, current_max: %d, current_min: %d\n",
			i, new_para[i].voltage, new_para[i].current_max, new_para[i].current_min);
	}

	old_para = volt_info->volt_para;
	volt_info->volt_para = new_para;
	volt_info->volt_para_size = spec->step_size;
	kfree(old_para);

	return 0;
}

/*
 * Replace step tables with the ones smart charging hands down: per entry a
 * smart_batt_spec header followed by its curve points.
 */
static int strategy_quickchg_smartchg_update_baa_para(void *data, char *baa_para, int ffc_size,
	int normal_size)
{
	struct mca_quick_charge_info *info = data;
	struct mca_quick_charge_batt_para_info *batt_para;
	struct smart_batt_spec_curve curve[SMART_BATT_SPEC_MAX_STEPS];
	struct smart_batt_spec *spec;
	int offset = 0;
	int i, idx;

	if (!info || !baa_para) {
		mca_log_err("data or baa_para is NULL\n");
		return -1;
	}

	batt_para = &info->batt_para[FG_IC_MASTER];
	for (i = 0; i < ffc_size + normal_size; i++) {
		spec = (struct smart_batt_spec *)(baa_para + offset);
		offset += offsetof(struct smart_batt_spec, steps);
		if (spec->step_size > SMART_BATT_SPEC_MAX_STEPS) {
			mca_log_err("step_size: %d is invalid\n", spec->step_size);
			return -1;
		}

		memset(curve, 0, sizeof(curve));
		memcpy(curve, baa_para + offset, spec->step_size * sizeof(*curve));
		spec->steps = curve;
		offset += spec->step_size * sizeof(*curve);

		idx = spec->t_range.idx;
		if (idx < 0 || idx >= batt_para->temp_para_size) {
			mca_log_err("temp_para_size: %d, temp_range.idx: %d is invalid\n",
				idx >= batt_para->temp_para_size, idx);
			continue;
		}

		(void)strategy_quickchg_smartchg_baa_update_volt_info(&batt_para->temp_info[idx],
			spec);
	}

	return 0;
}

static struct mca_smartchg_if_ops g_quickchg_smartchg_if_ops = {
	.type = MCA_SMARTCHG_IF_CHG_TYPE_QC,
	.set_delta_fv = strategy_quickchg_smartchg_delta_fv_callback,
	.set_delta_ichg = strategy_quickchg_smartchg_delta_ichg_callback,
	.set_fcc = strategy_quickchg_smartchg_set_fcc_callback,
	.set_soc_limit_sts = strategy_quickchg_smartchg_soc_limit_callback,
	.set_pwr_boost_sts = strategy_quickchg_smartchg_pwr_boost_sts_callback,
	.update_baa_para = strategy_quickchg_smartchg_update_baa_para,
};

static void mca_quick_charge_init_data(struct mca_quick_charge_info *info)
{
	int mode, role;

	info->batt_missing = 1;
	info->cur_support_mode = info->support_mode;
	info->sysfs_data.chg_enable = 1;
	for (mode = CHG_MODE_DIV1; mode < CHG_MODE_MAX; mode++) {
		info->sysfs_data.mode_enable[mode] = 1;
		for (role = CP_ROLE_MASTER; role < CP_ROLE_MAX; role++)
			info->sysfs_data.cp_path_enable[mode][role] = 1;
	}
}

static int mca_quick_charge_probe(struct platform_device *pdev)
{
	struct mca_quick_charge_info *info;
	int ret;

	mca_log_info("probe begin\n");
	if (strategy_class_fg_ops_is_init_ok() <= 0) {
		mca_log_info("fg is not ready, wait for it\n");
		return -EPROBE_DEFER;
	}

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	/*
	 * The SC8541 pairs are only usable once both halves probed.  This runs
	 * before the device tree is read, so it looks at the master alone.
	 */
	(void)platform_class_cp_get_chip_vendor(CP_ROLE_MASTER, &info->cp_vendor);
	if (info->cp_vendor == SC8541_VENDOR) {
		ret = platform_class_cp_get_probe_ok(CP_ROLE_MASTER);
		if (info->cp_type == MCA_CP_TYPE_PARALLEL)
			ret |= platform_class_cp_get_probe_ok(CP_ROLE_SLAVE);
		if (ret) {
			mca_log_err("cp is not ready, wait for it\n");
			return -EPROBE_DEFER;
		}
	}

	info->input_suspend_voter = mca_find_votable("input_suspend");
	if (!info->input_suspend_voter) {
		mca_log_info("get voter fail, wait for it\n");
		return -EPROBE_DEFER;
	}
	info->buck_input_voter = mca_find_votable("buck_input");
	if (!info->buck_input_voter) {
		mca_log_info("get buck_input voter fail, wait for it\n");
		return -EPROBE_DEFER;
	}
	info->buck_charge_curr_voter = mca_find_votable("buck_charge_curr");
	if (!info->buck_charge_curr_voter) {
		mca_log_info("get buck_charge_curr voter fail, wait for it\n");
		return -EPROBE_DEFER;
	}
	info->buck_vterm_voter = mca_find_votable("term_volt");
	if (!info->buck_vterm_voter) {
		mca_log_info("get buck_charge_vterm voter fail, wait for it\n");
		return -EPROBE_DEFER;
	}
	info->vterm_voter = mca_find_votable("term_volt");
	if (!info->vterm_voter) {
		mca_log_info("get vterm_voter voter fail, wait for it\n");
		return -EPROBE_DEFER;
	}
	info->flip_fcc_voter = mca_find_votable("flip_charge_curr");
	if (!info->flip_fcc_voter) {
		mca_log_info("get flip_fcc_voter voter fail, wait for it\n");
		return -EPROBE_DEFER;
	}

	info->dev = &pdev->dev;
	platform_set_drvdata(pdev, info);
	ret = mca_quick_charge_parse_dt(info);
	if (ret) {
		mca_log_err("parst dts failed, ret: %d\n", ret);
		return ret == -1 ? -1 : -EPROBE_DEFER;
	}

	if (mca_quick_charge_create_voter(info))
		return -EPROBE_DEFER;

	mca_quick_charge_init_data(info);
	mca_quick_charge_create_group(info->dev);
	mutex_init(&info->data_lock);
	INIT_DELAYED_WORK(&info->monitor_work, mca_quick_charge_monitor_work);
	INIT_DELAYED_WORK(&info->pps_ptf_work, mca_quick_charge_pps_ptf_work);
	INIT_DELAYED_WORK(&info->vfc_work, mca_quick_charge_vfc_work);

	(void)mca_strategy_ops_register(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
		mca_quick_charge_process_event, mca_quick_charge_get_status, NULL, info);
	g_strategy_quickchg_if_ops.data = info;
	(void)mca_charge_if_ops_register(&g_strategy_quickchg_if_ops);
	g_quickchg_smartchg_if_ops.data = info;
	(void)mca_smartchg_if_ops_register(&g_quickchg_smartchg_if_ops);

	info->shutdown_notifier.notifier_call = mca_quick_charge_shutdown;
	info->shutdown_notifier.priority = 255;
	if (register_reboot_notifier(&info->shutdown_notifier) < 0)
		mca_log_err("register reboot notify failed\n");

	mca_log_err("probe end\n");
	return 0;
}

static int mca_quick_charge_remove(struct platform_device *pdev)
{
	struct mca_quick_charge_info *info = platform_get_drvdata(pdev);

	mca_quick_charge_remove_group(&pdev->dev);
	if (!info)
		return 0;

	/* the works and the notifier must not outlive the devm-allocated info */
	(void)unregister_reboot_notifier(&info->shutdown_notifier);
	cancel_delayed_work_sync(&info->monitor_work);
	cancel_delayed_work_sync(&info->pps_ptf_work);
	cancel_delayed_work_sync(&info->vfc_work);

	return 0;
}

static const struct of_device_id mca_quick_charge_info_of_match[] = {
	{ .compatible = "mca,quick_charger", },
	{},
};

static struct platform_driver mca_quick_charge_info_driver = {
	.driver = {
		.name = "strtategy-quick-charge",
		.owner = THIS_MODULE,
		.of_match_table = mca_quick_charge_info_of_match,
	},
	.probe = mca_quick_charge_probe,
	.remove = mca_quick_charge_remove,
};

module_platform_driver(mca_quick_charge_info_driver);

MODULE_DESCRIPTION("strategy quick charger");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("getian@xiaomi.com");
MODULE_AUTHOR("lvchen@xiaomi.com");
