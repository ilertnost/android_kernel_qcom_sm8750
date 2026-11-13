// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The buck charger strategy.  See inc/mca_strategy_buckchg.h.
 *
 * Every limit the buck charger runs under is the outcome of an election:
 * the charger type, thermal, JEITA, userspace, smart charging and so on each
 * cast a vote, and the callbacks here are what put the result into the
 * hardware.  While a cable is attached a monitor looks at the charger every
 * few seconds, picks the limits for the charger type, keeps the bus at the
 * voltage asked for, decides when fast charge mode starts and stops, and
 * runs a software constant-voltage phase near the float voltage.
 */

#define MCA_LOG_TAG "mca_strategy_buckchg"

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/hwid.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>

#include "inc/mca_strategy_buckchg.h"
#include "../strategy_wireless/inc/mca_wireless_revchg.h"
#include <mca/common/mca_charge_interface.h>
#include <mca/common/mca_charge_mievent.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_hwid.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/common/mca_smem.h>
#include <mca/common/mca_voter.h>
#include <mca/common/mca_workqueue.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/platform/platform_cp_class.h>
#include <mca/platform/platform_fg_ic_ops.h>
#include <mca/platform/platform_loadsw_class.h>
#include <mca/protocol/protocol_class.h>
#include <mca/protocol/protocol_pd_class.h>
#include <mca/smartchg/smart_chg_class.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>

/* Above this charge level the monitor manages fast charge mode instead. */
#define QUICK_CHG_SOC_MAX			90
/* Fast charge mode is only started below this charge level. */
#define FFC_START_SOC_MAX			94

/* An EU unit retries quick charge on a PPS adapter this often. */
#define EU_PPS_QUICKCHG_RETRY_MS		7000

/*
 * A flip pack on a verified adapter is hot above 34 degrees, or above 32
 * when the gauge says its reading is offset.
 */
#define BUCKCHG_WARM_PACK_TEMP_C		32
#define BUCKCHG_HOT_PACK_TEMP_C			34

/* The pump's bus over-voltage while discharging, and on a float charger. */
#define BUCKCHG_DISCHARGE_CP_BUSOVP_MV		13000
#define BUCKCHG_FLOAT_CP_BUSOVP_MV		7000

/* Bus voltage checks, in millivolts. */
#define CHECK_VBUS_9V_LOW_TH			7600
#define CHECK_VBUS_5V_HIGH_TH			6000
#define VBUS_9V_OVP_VOLTAGE			10000
#define VBUS_5V_OVP_VOLTAGE			7500
#define PMIC_INDUCTOR_SECURE_ICL		1000
#define NON_COMPLIANT_QC_ICL			1500
#define VBUS_QC_VOL_THRESHOLD_LOW		7200
#define NON_COMPLIANT_QC_IBUS_THR_LOW_UA	200000
#define NON_COMPLIANT_QC_COUNT_MAX		2

/* Passes to wait for a silent charge pump before giving up on it. */
#define BUCKCHG_CP_ABSENT_RETRY_MAX		6

/* A charger stuck off above this bus voltage is asked down to 5 V. */
#define STATEGY_CHARGE_VBUS_ABNORMAL_UV		6000000

/* AICL is rerun when the input looks starved. */
#define IBUS_LOW_RESTART_AICL_UA		150000
#define IBUS_LOW_RESTART_AICL_ICL_MIN		500

/* How far under jeita's target the buck charger finishes after the pump. */
#define BUCKCHG_CP_TO_PMIC_VTERM_DROP		5

/* The software constant-voltage phase. */
#define SW_CV_VTERM_MIN				4300
#define SW_CV_START_DELTA			10
#define SW_CV_STOP_DELTA			20

/* Cold and hot zones the charger is watched in, in tenths of a degree. */
#define COLD_ZONE_LOW				-100
#define COLD_ZONE_HIGH				50
#define HOT_ZONE_LOW				480
#define HOT_ZONE_HIGH				550
#define ABNORMAL_ZONE_COUNT_MAX			3

/* Charging a wireless receiver off the port. */
#define WLS_REVCHG_DCP_WAIT_MS			6000
#define WLS_REVCHG_HVDCP3_WAIT_MS		3000
#define WLS_REVCHG_PD_WAIT_MS			4000
#define WLS_REVCHG_VBUS_SETTLE_MS		300
#define WLS_REVCHG_VBUS_RETRY			6

/* The CSD pulse. */
#define CSD_PULSE_FCC				1500
#define CSD_PULSE_HOLD_MS			75000

/* Reverse quick charge. */
#define REVCHG_BOARD_TEMP_MAX			400
#define REVCHG_SOC_START			69
#define REVCHG_SOC_KEEP				29
#define REVCHG_TIMEOUT				40
#define REVCHG_SETTLE_MS			400
#define REVCHG_POS_BOOST			1
#define REVCHG_POS_DIV2				2
#define REVCHG_GEAR_NORMAL			0
#define REVCHG_GEAR_QUICK			1
#define REVCHG_GEAR_STEPDOWN			2
#define REVCHG_UEVENT_REQUEST_BCL		3
#define REVCHG_IBAT_THRESHOLD_UA		3000000
#define REVCHG_IBAT_CHECK_MAX			1
#define REVCHG_IBUS_SETTLED_MA			1500
#define REVCHG_IBUS_SETTLE_RETRY		5
#define REVCHG_ADC_SETTLE_MS			20

/* PD verification. */
#define PD_SECRET_CHECK_MS			10000
#define PD_SECRET_BOOT_SEC			60
#define PD_AUTH_RERUN_MS			1000
#define PD_UNVERIFIED_ICL			100

/* The SOC limit stepper. */
#define SOC_LIMIT_STEP_MS			4000

/* The input current floor after a zero-speed boot. */
#define BUCKCHG_ZERO_SPEED_BOOT_WINDOW_SEC	60
#define BUCKCHG_ZERO_SPEED_INPUT_MIN		1500

/* USB current limits userspace may set on an EU unit, in milliamps. */
#define USB_ICL_SUSPEND_MA			2
#define USB_ICL_UNSUSPEND_MA			-1
#define USB_ICL_100_MA				100
#define USB_ICL_500_MA				500
#define USB_ICL_900_MA				900
#define USB_ICL_UNENUMERATED			100

static struct strategy_buckchg_dev *g_buckchg_info;

/*
 * How far under the target the software constant-voltage phase starts
 * pulling the current back, by how much the battery is still taking.
 */
struct sw_cv_volt_delta {
	int ibat_ma;
	int delta_mv;
};

static const struct sw_cv_volt_delta sw_cv_volt_delta_map[] = {
	{ 1000, 4 },
	{    1, 2 },
};
#define SW_CV_VOLT_DELTA_DEFAULT		4

static const struct sw_cv_volt_delta base_flip_sw_cv_volt_delta_map[] = {
	{ 1000, 7 },
	{    1, 2 },
};
#define BASE_FLIP_SW_CV_VOLT_DELTA_DEFAULT	7

/* Charge current and input limit per SOC limit step, in milliamps. */
struct soc_limit_step {
	int fcc;
	int icl;
};

static const struct soc_limit_step soc_limit_stepper_table[] = {
	{    0,   0 },
	{ 1990, 850 },
	{ 1400, 750 },
	{ 1100, 650 },
	{  900, 550 },
	{  700, 450 },
	{  500, 350 },
	{  300, 350 },
	{  200, 350 },
	{  100, 250 },
	{    0, 250 },
};
#define SOC_LIMIT_MAX_STEP	(ARRAY_SIZE(soc_limit_stepper_table) - 1)

/* What the charger reports, as this strategy sees it. */
static const int buck_chg_status_map[] = {
	[MCA_BATT_CHGR_STATUS_INHIBIT]		 = MCA_BUCK_CHG_STS_CHARGING,
	[MCA_BATT_CHGR_STATUS_TRICKLE]		 = MCA_BUCK_CHG_STS_CHARGING,
	[MCA_BATT_CHGR_STATUS_PRECHARGE]	 = MCA_BUCK_CHG_STS_CHARGING,
	[MCA_BATT_CHGR_STATUS_FULLON]		 = MCA_BUCK_CHG_STS_CHARGING,
	[MCA_BATT_CHGR_STATUS_TAPER]		 = MCA_BUCK_CHG_STS_CHARGING,
	[MCA_BATT_CHGR_STATUS_TERMINATION]	 = MCA_BUCK_CHG_STS_CHARGE_DONE,
	[MCA_BATT_CHGR_STATUS_PAUSE]		 = MCA_BUCK_CHG_NO_CHARGING,
	[MCA_BATT_CHGR_STATUS_CHARGING_DISABLED] = MCA_BUCK_CHG_NO_CHARGING,
	[MCA_BATT_CHGR_STATUS_FAST_LINEAR]	 = MCA_BUCK_CHG_STS_CHARGING,
};

static void strategy_buckchg_set_charge_volt(struct strategy_buckchg_dev *info,
					     int target_volt);
static void strategy_buck_update_req_volt(struct strategy_buckchg_dev *info);
static void strategy_wls_revchg_monitor_workfunc(struct work_struct *work);
static void strategy_buckchg_process_soc_limit_change_more(int value,
							   struct strategy_buckchg_dev *info);

static bool strategy_buckchg_is_qc(int real_type)
{
	return real_type >= XM_CHARGER_TYPE_HVDCP2 &&
	       real_type <= XM_CHARGER_TYPE_HVDCP3P5;
}

static bool strategy_buckchg_is_pd(int real_type)
{
	return real_type >= XM_CHARGER_TYPE_PD &&
	       real_type <= XM_CHARGER_TYPE_PD_VERIFY;
}

/* Seconds since boot. */
static time64_t strategy_buckchg_uptime(void)
{
	return ns_to_timespec64(ktime_get_with_offset(TK_OFFS_BOOT)).tv_sec;
}

/* The main charger, and the second one where the board has it. */
static int strategy_class_buckchg_ops_set_chg(struct strategy_buckchg_dev *info,
					      bool en)
{
	int ret;

	ret = platform_class_buckchg_ops_set_chg(MAIN_BUCK_CHARGER, en);
	if (info && info->support_multi_buck)
		ret |= platform_class_buckchg_ops_set_chg(AUX_BUCK_CHARGER, en);

	return ret;
}

static void strategy_class_buckchg_ops_set_input_volt(struct strategy_buckchg_dev *info,
						      int mv)
{
	platform_class_buckchg_ops_set_input_volt_lmt(MAIN_BUCK_CHARGER, mv);
	if (info->support_multi_buck)
		platform_class_buckchg_ops_set_input_volt_lmt(AUX_BUCK_CHARGER, mv);
}

static void strategy_class_buckchg_ops_set_opt_fws(struct strategy_buckchg_dev *info,
						   int mv)
{
	platform_class_buckchg_ops_set_opt_fws(MAIN_BUCK_CHARGER, mv);
	if (info->support_multi_buck)
		platform_class_buckchg_ops_set_opt_fws(AUX_BUCK_CHARGER, mv);
}

static void strategy_class_buckchg_ops_adc_enable(struct strategy_buckchg_dev *info,
						  bool en)
{
	platform_class_buckchg_ops_adc_enable(MAIN_BUCK_CHARGER, en);
	if (info->support_multi_buck)
		platform_class_buckchg_ops_adc_enable(AUX_BUCK_CHARGER, en);
}

static void strategy_class_buckchg_ops_set_usb_aicl_cont_thd(struct strategy_buckchg_dev *info,
							     int mv)
{
	platform_class_buckchg_ops_set_usb_aicl_cont_thd(MAIN_BUCK_CHARGER, mv);
	if (info && info->support_multi_buck)
		platform_class_buckchg_ops_set_usb_aicl_cont_thd(AUX_BUCK_CHARGER, mv);
}

static void strategy_buckchg_parse_dt(struct strategy_buckchg_dev *info)
{
	struct device_node *np = info->dev->of_node;
	u32 idata[REV_USBIN_TYPE_MAX] = { 0 };

	mca_parse_dts_u32(np, "batt_type", (u32 *)&info->batt_type,
			  MCA_BATTERY_TYPE_SINGLE);
	mca_parse_dts_u32(np, "support_multi_buck",
			  (u32 *)&info->support_multi_buck, 0);
	mca_parse_dts_u32(np, "in_dcp", (u32 *)&info->in_dcp,
			  CHARGE_DCP_INPUT_DEFAULT);
	mca_parse_dts_u32(np, "in_pd", (u32 *)&info->in_pd,
			  CHARGE_DCP_INPUT_DEFAULT);
	mca_parse_dts_u32(np, "in_pps", (u32 *)&info->in_pps, info->in_pd);
	mca_parse_dts_u32(np, "in_hvdcp", (u32 *)&info->in_hvdcp,
			  CHARGE_DCP_INPUT_DEFAULT);
	mca_parse_dts_u32(np, "in_hvdcp3", (u32 *)&info->in_hvdcp3,
			  CHARGE_DCP_INPUT_DEFAULT);
	mca_parse_dts_u32(np, "in_hvdcp3p5", (u32 *)&info->in_hvdcp3p5,
			  CHARGE_DCP_INPUT_DEFAULT);
	mca_parse_dts_u32(np, "in_cdp", (u32 *)&info->in_cdp,
			  CHARGE_CDP_INPUT_DEFAULT);
	mca_parse_dts_u32(np, "in_sdp", (u32 *)&info->in_sdp,
			  CHARGE_SDP_INPUT_DEFAULT);
	mca_parse_dts_u32(np, "in_float", (u32 *)&info->in_float,
			  CHARGE_FLOAT_INPUT_DEFAULT);
	mca_parse_dts_u32(np, "chg_dcp", (u32 *)&info->chg_dcp,
			  CHARGE_DCP_CHARGE_DEFAULT);
	mca_parse_dts_u32(np, "chg_pd", (u32 *)&info->chg_pd,
			  CHARGE_DCP_CHARGE_DEFAULT);
	mca_parse_dts_u32(np, "chg_pps", (u32 *)&info->chg_pps, info->chg_pd);
	mca_parse_dts_u32(np, "chg_hvdcp", (u32 *)&info->chg_hvdcp,
			  CHARGE_DCP_CHARGE_DEFAULT);
	mca_parse_dts_u32(np, "chg_hvdcp3", (u32 *)&info->chg_hvdcp3,
			  CHARGE_DCP_CHARGE_DEFAULT);
	mca_parse_dts_u32(np, "chg_hvdcp3p5", (u32 *)&info->chg_hvdcp3p5,
			  CHARGE_DCP_CHARGE_DEFAULT);
	mca_parse_dts_u32(np, "chg_cdp", (u32 *)&info->chg_cdp,
			  CHARGE_CDP_CHARGE_DEFAULT);
	mca_parse_dts_u32(np, "chg_sdp", (u32 *)&info->chg_sdp,
			  CHARGE_SDP_CHARGE_DEFAULT);
	mca_parse_dts_u32(np, "chg_float", (u32 *)&info->chg_float,
			  CHARGE_FLOAT_CHARGE_DEFAULT);
	mca_parse_dts_u32(np, "chg_batt_auth_failed",
			  (u32 *)&info->chg_batt_auth_failed,
			  CHARGE_BATT_AUTH_FAIL_DEFAULT);
	mca_parse_dts_u32(np, "ffc_temp_low", (u32 *)&info->ffc_temp_low,
			  ALLOW_FFC_TEMP_LOW_THR);
	mca_parse_dts_u32(np, "ffc_temp_high", (u32 *)&info->ffc_temp_high,
			  ALLOW_FFC_TEMP_HIGH_THR);
	mca_parse_dts_u32(np, "curr_terminate_compensation",
			  (u32 *)&info->curr_term_compensation, 0);
	mca_parse_dts_u32(np, "allow_start_ffc_batt_soc_thr",
			  (u32 *)&info->allow_start_ffc_batt_soc_thr,
			  ALLOW_START_FFC_BATT_SOC_THR);
	mca_parse_dts_u32(np, "pmic_fv_compensation",
			  (u32 *)&info->pmic_fv_compensation, 0);
	mca_parse_dts_u32(np, "pmic_fv_compensation_cold",
			  (u32 *)&info->pmic_fv_compensation_cold, 0);
	mca_parse_dts_u32(np, "bat_temp_fv_comp_cold_th",
			  (u32 *)&info->bat_temp_fv_comp_cold_th, 0);
	mca_parse_dts_u32(np, "pmic_fv_compensation_hot",
			  (u32 *)&info->pmic_fv_compensation_hot, 0);
	mca_parse_dts_u32(np, "bat_temp_fv_comp_hot_th",
			  (u32 *)&info->bat_temp_fv_comp_hot_th,
			  BAT_TEMP_FV_COMP_HOT_TH_DEFAULT);
	mca_parse_dts_u32(np, "pmic_middle_fv_compensation",
			  (u32 *)&info->pmic_middle_fv_compensation, 0);
	mca_parse_dts_u32(np, "pmic_high_fv_compensation",
			  (u32 *)&info->pmic_high_fv_compensation, 0);
	mca_parse_dts_u32(np, "pmic_wls_fv_compensation",
			  (u32 *)&info->pmic_wls_fv_compensation, 0);
	mca_parse_dts_u32(np, "pmic_iterm_compensation",
			  (u32 *)&info->pmic_iterm_compensation,
			  PMIC_ITERM_COMPENSATION_DEFAULT);
	mca_parse_dts_u32(np, "vusb_ovp_location",
			  (u32 *)&info->vusb_ovp_location,
			  BUCKCHG_VUSB_OVP_BEFORE_CP);
	mca_parse_dts_u32(np, "terminated_by_cp",
			  (u32 *)&info->terminated_by_cp, 0);
	mca_parse_dts_u32(np, "sw_cv_fcc_step", (u32 *)&info->sw_cv_fcc_step,
			  SW_CV_FCC_STEP_DEFAULT);
	mca_parse_dts_u32(np, "sw_cv_fv_step", (u32 *)&info->sw_cv_fv_step,
			  SW_CV_FV_STEP_DEFAULT);

	info->support_diff_temp_comp =
		of_property_read_bool(np, "support_diff_temp_comp");
	info->support_reverse_quick_charge =
		of_property_read_bool(np, "support_reverse_quick_charge");
	info->support_revchg_screenon =
		of_property_read_bool(np, "support_revchg_screenon");
	info->support_base_flip = of_property_read_bool(np, "support-base-flip");
	info->base_flip_same = of_property_read_bool(np, "base-flip-same");
	info->support_pmic_vterm_dynamics_adjust =
		of_property_read_bool(np, "support-pmic-vterm-dynamics-adjust");
	info->need_cp_to_pmic = of_property_read_bool(np, "need-cp-to-pmic");
	info->support_charge_more = of_property_read_bool(np,
							  "support-charge-more");

	if (mca_parse_dts_u32_array(np, "rev_req_vadp", idata,
				    REV_USBIN_TYPE_MAX)) {
		info->rev_req_vadp[REV_USBIN_TYPE_PPS] =
			STATEGY_USBIN_WLS_REV_CHG_VOLTAGE_DEFAULT;
		info->rev_req_vadp[REV_USBIN_TYPE_OTHER] =
			STATEGY_USBIN_WLS_REV_CHG_VOLTAGE_DEFAULT;
	} else {
		memcpy(info->rev_req_vadp, idata, sizeof(idata));
	}
	mca_log_debug("rev_req_vadp %d %d\n",
		      info->rev_req_vadp[REV_USBIN_TYPE_PPS],
		      info->rev_req_vadp[REV_USBIN_TYPE_OTHER]);

	if (mca_parse_dts_u32_array(np, "rev_vadp_valid_h", idata,
				    REV_USBIN_TYPE_MAX)) {
		info->rev_vadp_valid_h[REV_USBIN_TYPE_PPS] =
			STATEGY_USBIN_WLS_REV_CHG_VOLTAGE_BUF_H;
		info->rev_vadp_valid_h[REV_USBIN_TYPE_OTHER] =
			STATEGY_USBIN_WLS_REV_CHG_VOLTAGE_BUF_H;
	} else {
		memcpy(info->rev_vadp_valid_h, idata, sizeof(idata));
	}
	mca_log_debug("rev_vadp_valid_h %d %d\n",
		      info->rev_vadp_valid_h[REV_USBIN_TYPE_PPS],
		      info->rev_vadp_valid_h[REV_USBIN_TYPE_OTHER]);

	if (mca_parse_dts_u32_array(np, "rev_vadp_valid_l", idata,
				    REV_USBIN_TYPE_MAX)) {
		info->rev_vadp_valid_l[REV_USBIN_TYPE_PPS] =
			STATEGY_USBIN_WLS_REV_CHG_VOLTAGE_BUF_L;
		info->rev_vadp_valid_l[REV_USBIN_TYPE_OTHER] =
			STATEGY_USBIN_WLS_REV_CHG_VOLTAGE_BUF_L;
	} else {
		memcpy(info->rev_vadp_valid_l, idata, sizeof(idata));
	}
	mca_log_debug("rev_vadp_valid_l %d %d\n",
		      info->rev_vadp_valid_l[REV_USBIN_TYPE_PPS],
		      info->rev_vadp_valid_l[REV_USBIN_TYPE_OTHER]);
}

static int strategy_buckchg_enable(struct mca_votable *votable, void *data,
				   int effective_result,
				   const char *effective_client)
{
	struct strategy_buckchg_dev *info = data;

	if (!info)
		return -1;

	mca_log_err("%d\n", effective_result);

	return strategy_class_buckchg_ops_set_chg(info, !!effective_result);
}

static int strategy_buckchg_input_suspend(struct mca_votable *votable,
					  void *data, int effective_result,
					  const char *effective_client)
{
	struct strategy_buckchg_dev *info = data;

	if (!info)
		return -1;

	mca_log_err("%d\n", effective_result);
	mca_vote(info->input_limit_voter, "input_suspend", !!effective_result,
		 0);

	return 0;
}

/*
 * Only records which rail the input is on; the per-rail votables apply their
 * limits when they next run.
 */
static int strategy_buckchg_input_voltage(struct mca_votable *votable,
					  void *data, int effective_result,
					  const char *effective_client)
{
	struct strategy_buckchg_dev *info = data;

	if (!info)
		return -1;

	info->hvdcp_allow_flag = effective_result;
	info->proc_data.voltage = effective_result ? STATEGY_CHARGE_VBUS_9V :
						     STATEGY_CHARGE_VBUS_5V;

	return 0;
}

/*
 * A battery that UEFI found flat (a zero-speed boot) cannot be fed a small
 * input without collapsing again, so for the first minute after boot the
 * input limit has a floor.
 */
static int strategy_buckchg_input_limit(struct mca_votable *votable,
					void *data, int effective_result,
					const char *effective_client)
{
	struct strategy_buckchg_dev *info = data;
	u32 is_zero_speed = 0;
	int result = effective_result;

	if (!info)
		return -1;

	if (strategy_buckchg_uptime() < BUCKCHG_ZERO_SPEED_BOOT_WINDOW_SEC) {
		get_smem_battery_info(&is_zero_speed);
		if (effective_result && is_zero_speed) {
			if (effective_result <= BUCKCHG_ZERO_SPEED_INPUT_MIN)
				result = BUCKCHG_ZERO_SPEED_INPUT_MIN;
			mca_log_err("is_zero_speed, result = %d, effective_result = %d\n",
				    result, effective_result);
		}
	}

	mca_log_err("set input current %d\n", result);

	if (info->support_multi_buck)
		return 0;

	return platform_class_buckchg_ops_set_input_curr_lmt(MAIN_BUCK_CHARGER,
							     result);
}

static int strategy_buckchg_charge_limit(struct mca_votable *votable,
					 void *data, int effective_result,
					 const char *effective_client)
{
	struct strategy_buckchg_dev *info = data;

	if (!info)
		return -1;

	mca_log_err("set chg current %d\n", effective_result);

	if (info->support_multi_buck)
		return 0;

	return platform_class_buckchg_ops_set_ichg(MAIN_BUCK_CHARGER,
						   effective_result);
}

/*
 * The termination current the charger is given is the voted one plus a
 * margin for the difference between where the gauge and the charger measure.
 * On a foldable the margin depends on the pack and its temperature.
 */
static int strategy_buckchg_set_iterm(struct mca_votable *votable, void *data,
				      int effective_result,
				      const char *effective_client)
{
	struct strategy_buckchg_dev *info = data;
	int temp = 0;

	if (!info)
		return -1;

	if (info->support_base_flip) {
		if (!strategy_class_fg_get_fastcharge()) {
			info->pmic_iterm_compensation =
				info->parallel_iterm == 187 ? 20 : 14;
		} else {
			strategy_class_fg_ops_get_temperature(&temp);
			if (temp >= 300 && temp <= 399 &&
			    info->parallel_iterm == 1046)
				info->pmic_iterm_compensation = 104;
			else if (temp / 10 >= 18 && temp / 10 <= 29 &&
				 info->parallel_iterm == 598)
				info->pmic_iterm_compensation = 82;
			else
				info->pmic_iterm_compensation = 50;
		}
	}

	if (info->base_flip_same) {
		if (!strategy_class_fg_get_fastcharge()) {
			info->pmic_iterm_compensation = 20;
		} else {
			strategy_class_fg_ops_get_temperature(&temp);
			info->pmic_iterm_compensation = temp > 349 ? 68 : 75;
		}
	}

	if (info->support_multi_buck)
		return 0;

	return platform_class_buckchg_ops_set_term_curr(MAIN_BUCK_CHARGER,
			info->pmic_iterm_compensation + effective_result);
}

/*
 * The float voltage the charger is given is the voted one plus a margin for
 * the drop between the gauge's sense point and the charger's.  The margin
 * depends on the pack, on how cold or hot it is and, where the board asks
 * for it, on which of three temperature bands it is in while fast charging.
 */
static int strategy_buckchg_set_vterm(struct mca_votable *votable, void *data,
				      int effective_result,
				      const char *effective_client)
{
	struct strategy_buckchg_dev *info = data;
	int temp = 0, temp_c;
	int vterm;

	if (!info)
		return -1;

	if (info->support_base_flip) {
		if (!strategy_class_fg_get_fastcharge())
			info->pmic_fv_compensation = 10;
		else if (!info->wls_online)
			info->pmic_fv_compensation = 20;
		else
			info->pmic_fv_compensation =
				info->pmic_wls_fv_compensation;
	}

	strategy_class_fg_ops_get_temperature(&temp);
	temp_c = temp / 10;

	if (temp_c < info->bat_temp_fv_comp_cold_th &&
	    info->support_diff_temp_comp)
		vterm = info->pmic_fv_compensation_cold + effective_result;
	else if (temp_c > info->bat_temp_fv_comp_hot_th &&
		 info->support_diff_temp_comp)
		vterm = info->pmic_fv_compensation_hot + effective_result;
	else
		vterm = info->pmic_fv_compensation + effective_result;

	if (info->support_pmic_vterm_dynamics_adjust) {
		if (!strategy_class_fg_get_fastcharge()) {
			vterm = effective_result;
		} else {
			strategy_class_fg_ops_get_temperature(&temp);
			temp_c = temp / 10;
			if (temp >= 180 && temp <= 299)
				vterm = info->pmic_fv_compensation +
					effective_result;
			if (temp_c >= 30 && temp_c <= 39)
				vterm = info->pmic_middle_fv_compensation +
					effective_result;
			else if (temp >= 400)
				vterm = info->pmic_high_fv_compensation +
					effective_result;
		}
	}

	if (info->base_flip_same) {
		if (strategy_class_fg_get_fastcharge())
			vterm = info->pmic_fv_compensation + effective_result;
		else if (temp_c >= 48 && temp_c <= 54)
			vterm = effective_result + 30;
		else
			vterm = effective_result;
	}

	if (info->support_multi_buck)
		return 0;

	return platform_class_buckchg_ops_set_term_volt(MAIN_BUCK_CHARGER, vterm);
}

/*
 * The per-rail limits are recorded always, but only passed on while their
 * rail is the one in use.
 */
static int strategy_buckchg_set_5v_input(struct mca_votable *votable,
					 void *data, int effective_result,
					 const char *effective_client)
{
	struct strategy_buckchg_dev *info = data;

	if (!info)
		return -1;

	info->buck_5v_in = effective_result;
	if (info->proc_data.voltage == STATEGY_CHARGE_VBUS_5V)
		mca_vote(info->input_limit_voter, "volt_limit",
			 !!effective_result, effective_result);

	return 0;
}

static int strategy_buckchg_set_5v_ichg(struct mca_votable *votable,
					void *data, int effective_result,
					const char *effective_client)
{
	struct strategy_buckchg_dev *info = data;

	if (!info)
		return -1;

	info->buck_5v_ich = effective_result;
	if (info->proc_data.voltage == STATEGY_CHARGE_VBUS_5V)
		mca_vote(info->charge_limit_voter, "volt_thermal_limit",
			 !!effective_result, effective_result);

	return 0;
}

static int strategy_buckchg_set_9v_input(struct mca_votable *votable,
					 void *data, int effective_result,
					 const char *effective_client)
{
	struct strategy_buckchg_dev *info = data;

	if (!info)
		return -1;

	info->buck_9v_in = effective_result;
	if (info->proc_data.voltage == STATEGY_CHARGE_VBUS_9V)
		mca_vote(info->input_limit_voter, "volt_limit",
			 !!effective_result, effective_result);

	return 0;
}

static int strategy_buckchg_set_9v_ichg(struct mca_votable *votable,
					void *data, int effective_result,
					const char *effective_client)
{
	struct strategy_buckchg_dev *info = data;

	if (!info)
		return -1;

	info->buck_9v_ich = effective_result;
	if (info->proc_data.voltage == STATEGY_CHARGE_VBUS_9V)
		mca_vote(info->charge_limit_voter, "volt_thermal_limit",
			 !!effective_result, effective_result);

	return 0;
}

static int strategy_buckchg_init_voter(struct strategy_buckchg_dev *info)
{
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	int iterm_type;

	info->chg_enable_voter = mca_create_votable("chg_enable", MCA_VOTE_AND,
						    strategy_buckchg_enable,
						    STATEGY_CHARGE_DISENABLE,
						    info);
	if (IS_ERR(info->chg_enable_voter))
		return -1;

	info->input_suppend_voter =
		mca_create_votable("input_suspend", MCA_VOTE_OR,
				   strategy_buckchg_input_suspend,
				   STATEGY_CHARGE_INPUT_SUSPEND, info);
	if (IS_ERR(info->input_suppend_voter))
		return -1;

	info->input_limit_voter =
		mca_create_votable("buck_input", MCA_VOTE_MIN,
				   strategy_buckchg_input_limit,
				   STATEGY_INPUT_DEFAULT_VALUE, info);
	if (IS_ERR(info->input_limit_voter))
		return -1;

	info->charge_limit_voter =
		mca_create_votable("buck_charge_curr", MCA_VOTE_MIN,
				   strategy_buckchg_charge_limit,
				   STATEGY_CHARGE_CURRENT_DEFAULT_VALUE, info);
	if (IS_ERR(info->charge_limit_voter))
		return -1;

	/*
	 * On two platforms charging stops at the first client whose term
	 * current is reached rather than the last.
	 */
	iterm_type = (hwid && (hwid->platform_version & ~1u) ==
			      STRATEGY_ITERM_MIN_PLATFORM) ?
		     MCA_VOTE_MIN : MCA_VOTE_MAX;
	info->iterm_voter = mca_create_votable("term_curr", iterm_type,
					       strategy_buckchg_set_iterm,
					       STATEGY_ITERM_DEFAULT_VALUE,
					       info);
	if (IS_ERR(info->iterm_voter))
		return -1;

	info->vterm_voter = mca_create_votable("term_volt", MCA_VOTE_MIN,
					       strategy_buckchg_set_vterm,
					       STATEGY_VTERM_DEFAULT_VALUE,
					       info);
	if (IS_ERR(info->vterm_voter))
		return -1;

	info->input_voltage_voter =
		mca_create_votable("input_voltage", MCA_VOTE_AND,
				   strategy_buckchg_input_voltage, 1, info);
	if (IS_ERR(info->input_voltage_voter))
		return -1;

	info->buck_5v_in_voter =
		mca_create_votable("buck_5v_in", MCA_VOTE_MIN,
				   strategy_buckchg_set_5v_input,
				   STATEGY_INPUT_5V_DEFAULT_VALUE, info);
	if (IS_ERR(info->buck_5v_in_voter))
		return -1;

	info->buck_5v_ich_voter =
		mca_create_votable("buck_5v_ich", MCA_VOTE_MIN,
				   strategy_buckchg_set_5v_ichg,
				   STATEGY_CHARGE_CURRENT_DEFAULT_VALUE, info);
	if (IS_ERR(info->buck_5v_ich_voter))
		return -1;

	info->buck_9v_in_voter =
		mca_create_votable("buck_9v_in", MCA_VOTE_MIN,
				   strategy_buckchg_set_9v_input,
				   STATEGY_INPUT_9V_DEFAULT_VALUE, info);
	if (IS_ERR(info->buck_9v_in_voter))
		return -1;

	info->buck_9v_ich_voter =
		mca_create_votable("buck_9v_ich", MCA_VOTE_MIN,
				   strategy_buckchg_set_9v_ichg,
				   STATEGY_CHARGE_CURRENT_DEFAULT_VALUE, info);
	if (IS_ERR(info->buck_9v_ich_voter))
		return -1;

	return 0;
}

static void strategy_buckchg_sw_cv_stop(struct strategy_buckchg_dev *info)
{
	mca_log_err("chg_status: %d, stop sw_cv_work\n",
		    info->proc_data.chg_status);
	cancel_delayed_work(&info->sw_cv_work);
	cancel_delayed_work(&info->base_flip_sw_cv_work);
	mca_vote(info->charge_limit_voter, "sw_cv", false, 0);
	info->sw_cv_running = false;
	info->vbat_ov_count = 0;
}

static void strategy_buckchg_start_charging(struct strategy_buckchg_dev *info)
{
	mca_log_info("start charging\n");
	strategy_buck_update_req_volt(info);
	cancel_delayed_work_sync(&info->monitor_work);
	strategy_class_buckchg_ops_adc_enable(info, true);
	queue_delayed_work(system_wq, &info->monitor_work, 0);
}

static void strategy_buckchg_stop_charging(struct strategy_buckchg_dev *info)
{
	mca_log_info("stop charging\n");
	cancel_delayed_work_sync(&info->monitor_work);
	cancel_delayed_work_sync(&info->wls_revchg_monitor_work);
	cancel_delayed_work_sync(&info->check_pd_secret_work);
	cancel_delayed_work_sync(&info->rerun_handle_pd_auth_work);
	strategy_buckchg_sw_cv_stop(info);

	memset(&info->proc_data, 0, sizeof(info->proc_data));
	memset(&info->pwr_cap, 0, sizeof(info->pwr_cap));
	info->proc_data.voltage = STATEGY_CHARGE_VBUS_5V;
	info->proc_data.pre_volt = STATEGY_CHARGE_VBUS_5V;
	info->is_non_compliant_qc = false;
	info->non_compliant_run_once = false;

	mca_vote(info->input_voltage_voter, "non_compliant_qc", false, 0);
	mca_vote(info->input_limit_voter, "non_compliant_qc", false, 0);
	if (info->support_pmic_vterm_dynamics_adjust) {
		mca_vote_override(info->vterm_voter, "pmic_temp_term", false, 0);
		mca_vote_override(info->input_limit_voter, "pmic_temp_term",
				  false, 0);
		info->pmic_temp_term_flag = false;
	}
	mca_vote(info->chg_enable_voter, "online", true, 0);
	mca_vote(info->buck_5v_in_voter, "wire_chg_type", true,
		 STATEGY_INPUT_DEFAULT_VALUE);
	mca_vote(info->buck_9v_in_voter, "wire_chg_type", true,
		 STATEGY_INPUT_DEFAULT_VALUE);
	mca_vote(info->input_limit_voter, "icl_limit", true,
		 PD_UNVERIFIED_ICL);
	mca_vote(info->buck_9v_in_voter, "pmic_inductor", false, 0);
	mca_vote(info->buck_5v_in_voter, "pmic_inductor", false, 0);
	mca_vote(info->buck_5v_ich_voter, "wire_chg_type", true,
		 STATEGY_INPUT_DEFAULT_VALUE);
	mca_vote(info->buck_9v_ich_voter, "wire_chg_type", true,
		 STATEGY_CHARGE_CURRENT_DEFAULT_VALUE);
	mca_vote(info->input_voltage_voter, "real_type", true, 0);
	mca_vote(info->input_voltage_voter, "eoc_5v", false, 0);
	mca_vote(info->chg_enable_voter, "vbat_ovp", false, 1);
	mca_vote(info->input_voltage_voter, "lpd", false, 0);
	cancel_delayed_work_sync(&info->csd_pulse_process_work);
	mca_vote(info->charge_limit_voter, "csd_pulse", false, 0);
	mca_vote(info->chg_enable_voter, "csd_pulse", false, 0);
	mca_vote(info->input_voltage_voter, "recover_force_5v", false, 0);
	mca_vote(info->vterm_voter, "cp_to_pmic", false,
		 STATEGY_VTERM_DEFAULT_VALUE);
	info->csd_flag = false;

	strategy_class_buckchg_ops_set_input_volt(info,
						  STATEGY_CHARGE_INPUT_VOLT_DEFAULT);
	strategy_class_buckchg_ops_adc_enable(info, false);
	strategy_class_buckchg_ops_set_opt_fws(info, STATEGY_CHARGE_FWS_DEFAULT);
	strategy_class_buckchg_ops_set_usb_aicl_cont_thd(info,
							 STATEGY_CHARGE_AICL_TH_4P1V);
	info->aicl_thd = 0;
	info->last_pdo_nums = 0;
}

static void strategy_buckchg_process_online_change(int value,
						   struct strategy_buckchg_dev *info)
{
	bool err_base = false, err_flip = false;
	bool cp_enabled = false;

	if (info->proc_data.online == value)
		return;

	info->proc_data.online = value;
	if (value) {
		if (info->support_base_flip) {
			platform_fg_ops_get_error_state(FG_IC_MASTER, &err_base);
			platform_fg_ops_get_error_state(FG_IC_SLAVE, &err_flip);
			if (!err_base || err_flip)
				platform_class_loadsw_set_lowpower_mode(0, false);
		}
		strategy_buckchg_start_charging(info);
		return;
	}

	platform_class_loadsw_set_lowpower_mode(0, true);
	info->pps_ptf = USBPD_BUCK_DPM_PORT_PPS_PTF_NOT_SUPPORTED;
	strategy_buckchg_stop_charging(info);
	info->verify_process_end = 0;
	info->proc_data.charge_done_force_5v = false;
	info->buck_abnormal_cnt = 0;

	/* A pack of two cells discharges through the pump. */
	if (info->base_flip_same) {
		platform_class_cp_set_busovp(CP_ROLE_MASTER,
					     BUCKCHG_DISCHARGE_CP_BUSOVP_MV);
		platform_class_cp_get_charging_enabled(CP_ROLE_MASTER,
						       &cp_enabled);
		mca_log_info("cp_enabled = %d in discharging\n", cp_enabled);
	}
}

/*
 * An adapter that comes back with more operating points than it first offered
 * has just finished authenticating; the USB supply is told a second later.
 */
static void strategy_rerun_handle_pd_auth_workfunc(struct work_struct *work)
{
	mca_event_block_notify(MCA_EVENT_CHARGE_STATUS, MCA_EVENT_USB_STS_CHANGE,
			       NULL);
	mca_log_info("send usb power_supply trigger handle pd auth\n");
}

/* Look for a fixed 9 V operating point on a PD adapter. */
static void stategy_buckchg_is_pdo_9v(struct strategy_buckchg_dev *info)
{
	int nums, i;

	if (!strategy_buckchg_is_pd(info->proc_data.real_type))
		return;

	if (info->last_pdo_nums == 1 && info->pwr_cap.pdo_nums > 1)
		queue_delayed_work(system_wq, &info->rerun_handle_pd_auth_work,
				   msecs_to_jiffies(PD_AUTH_RERUN_MS));

	nums = info->pwr_cap.pdo_nums;
	info->last_pdo_nums = nums;
	if (!nums) {
		mca_log_info(" pwr_cap.nums is null\n");
		return;
	}

	for (i = 0; i < min_t(int, nums, ADAPTER_POWER_CAP_MAX); i++) {
		if (info->pwr_cap.cap[i].max_voltage == STATEGY_CHARGE_VBUS_9V &&
		    info->pwr_cap.cap[i].min_voltage == STATEGY_CHARGE_VBUS_9V) {
			info->proc_data.is_pd_9v = true;
			info->proc_data.curr_pd_pos = i;
			mca_log_info("pdo[%d] can support 9v: %d\n", i, 1);
			return;
		}
	}

	info->proc_data.is_pd_9v = false;
	info->proc_data.curr_pd_pos = 0;
}

/*
 * On an EU unit userspace enumerates an SDP before it may draw more than
 * 100 mA; any other charger type cancels that.
 */
static void strategy_buckchg_check_charger_change(struct strategy_buckchg_dev *info)
{
	static bool sdp_vote_completed;
	int boot_mode = mca_log_get_charge_boot_mode();

	if (info->proc_data.real_type == XM_CHARGER_TYPE_UNKNOW) {
		mca_vote(info->input_limit_voter, "usbicl", false, 0);
		mca_vote(info->input_limit_voter, "sdpicl", false, 0);
		sdp_vote_completed = false;
		mca_log_info("charger have been removed reset vote");
		return;
	}

	if (boot_mode || !info->is_eu_model) {
		mca_log_info(" charge_boot_mode[%d] cancel sdp enumerated process",
			     boot_mode);
		return;
	}

	if (info->proc_data.real_type == XM_CHARGER_TYPE_SDP) {
		if (sdp_vote_completed)
			return;
		mca_vote(info->input_limit_voter, "sdpicl", true,
			 USB_ICL_UNENUMERATED);
		sdp_vote_completed = true;
		mca_log_info("sdp charger unenumerated");
		return;
	}

	if (!sdp_vote_completed)
		return;

	mca_vote(info->input_limit_voter, "sdpicl", false, 0);
	mca_vote(info->input_limit_voter, "usbicl", false, 0);
	sdp_vote_completed = false;
	mca_log_info("sdp charger error detected reset vote%d\n",
		     info->proc_data.real_type);
}

static void strategy_buckchg_process_type_change(int value,
						 struct strategy_buckchg_dev *info)
{
	if (info->proc_data.real_type == value) {
		if (!value)
			strategy_buckchg_check_charger_change(info);
		return;
	}

	info->proc_data.real_type = value;
	strategy_buckchg_check_charger_change(info);
	if (!value)
		return;

	if (strategy_buckchg_is_pd(info->proc_data.real_type)) {
		protocol_class_get_adapter_power_cap(ADAPTER_PROTOCOL_PD,
						     &info->pwr_cap);
		protocol_class_pd_get_suspend_support_status(TYPEC_PORT_0,
				&info->proc_data.pdsuspendsupported);
	}

	if (info->is_eu_model &&
	    info->proc_data.real_type == XM_CHARGER_TYPE_PPS) {
		info->proc_data.eu_start_time = ktime_get_boottime();
		mca_log_info("is_eu_model for PPS eu_start_time = %lld\n",
			     info->proc_data.eu_start_time);
	}

	stategy_buckchg_is_pdo_9v(info);
	strategy_buck_update_req_volt(info);
	mod_delayed_work(system_wq, &info->monitor_work, 0);

	mca_log_info("verify_process_end: %d\n", info->verify_process_end);

	/*
	 * An unverified PD adapter is held at a low input until verification
	 * has had time to finish; a type that is not PD drops the hold.
	 */
	if (info->proc_data.real_type == XM_CHARGER_TYPE_PD) {
		if (!info->verify_process_end &&
		    strategy_buckchg_uptime() > PD_SECRET_BOOT_SEC) {
			mca_vote(info->input_limit_voter, "icl_limit", true,
				 PD_UNVERIFIED_ICL);
			queue_delayed_work(system_wq, &info->check_pd_secret_work,
					   msecs_to_jiffies(PD_SECRET_CHECK_MS));
		}
	} else {
		mca_vote(info->input_limit_voter, "icl_limit", false, 0);
	}
}

static void strategy_buckchg_process_cap_change(struct strategy_buckchg_dev *info)
{
	if (strategy_buckchg_is_pd(info->proc_data.real_type)) {
		protocol_class_get_adapter_power_cap(ADAPTER_PROTOCOL_PD,
						     &info->pwr_cap);
		protocol_class_pd_get_suspend_support_status(TYPEC_PORT_0,
				&info->proc_data.pdsuspendsupported);
	}

	stategy_buckchg_is_pdo_9v(info);
	strategy_buck_update_req_volt(info);
	mod_delayed_work(system_wq, &info->monitor_work, 0);
}

/* The connector is overheating: drop to 5 V and stop, or resume. */
static void strategy_buckchg_process_antiburn_change(int value,
						     struct strategy_buckchg_dev *info)
{
	mca_log_info("value: %d\n", value);

	strategy_buckchg_set_charge_volt(info, STATEGY_CHARGE_VBUS_5V);

	if (value)
		strategy_buckchg_stop_charging(info);
	else if (info->proc_data.online)
		strategy_buckchg_start_charging(info);
}

static void strategy_buckchg_process_batt_btb_change(int value,
						     struct strategy_buckchg_dev *info)
{
	if (!value) {
		mca_vote(info->input_voltage_voter, "batt_miss", false, 0);
		mca_vote(info->input_limit_voter, "batt_miss", false,
			 STATEGY_CHARGE_CURRENT_DEFAULT_VALUE);
		mca_vote(info->vterm_voter, "batt_miss", false,
			 STATEGY_VTERM_DEFAULT_VALUE);
		mca_vote_override(info->input_limit_voter,
				  "master_batt_missing", false, 0);
		return;
	}

	mca_vote(info->input_voltage_voter, "batt_miss", true, 0);
	mca_vote(info->input_limit_voter, "batt_miss", true,
		 info->batt_type ? STAEGY_BATT_MISS_ICL_PARALLEL :
				   STAEGY_BATT_MISS_ICL);
	mca_vote(info->vterm_voter, "batt_miss", true, STAEGY_BATT_MISS_FV);
}

/*
 * With support-charge-more, the charge current and input limit are stepped
 * down towards the SOC cap one step every few seconds rather than charging
 * being stopped dead; at the last step charging is disabled.
 */
static void strategy_buckchg_process_soc_limit_change_more(int value,
							   struct strategy_buckchg_dev *info)
{
	static u8 cur_step;

	if (!value) {
		cur_step = 0;
		mca_log_info("SOC limit released\n");
		mca_vote(info->charge_limit_voter, "soc_limit", false, 0);
		mca_vote(info->input_limit_voter, "soc_limit", false, 0);
		mca_vote(info->chg_enable_voter, "soc_limit", false, 0);
		goto out;
	}

	mca_log_info("SOC limit triggered\n");
	if (cur_step < SOC_LIMIT_MAX_STEP)
		cur_step++;

	mca_vote(info->charge_limit_voter, "soc_limit", true,
		 soc_limit_stepper_table[cur_step].fcc);
	mca_vote(info->input_limit_voter, "soc_limit", true,
		 soc_limit_stepper_table[cur_step].icl);

	if (cur_step == SOC_LIMIT_MAX_STEP) {
		mca_vote(info->chg_enable_voter, "soc_limit", true, 0);
	} else {
		mca_vote(info->chg_enable_voter, "soc_limit", false, 0);
		queue_delayed_work(system_wq, &info->soc_limit_stepper_work,
				   msecs_to_jiffies(SOC_LIMIT_STEP_MS));
	}

out:
	mca_log_info("cur_step = %d\n", cur_step);
}

static void strategy_buckchg_soc_limit_stepper_workfunc(struct work_struct *work)
{
	struct strategy_buckchg_dev *info =
		container_of(to_delayed_work(work), struct strategy_buckchg_dev,
			     soc_limit_stepper_work);

	strategy_buckchg_process_soc_limit_change_more(info->soc_limit_sts,
						       info);
}

/*
 * The CSD pulse: fast charge is stopped, the charge current held down, and
 * a while later charging is briefly disabled before the quick charge
 * strategy is told to resume.
 */
static void strategy_buckchg_process_csd_pulse(int value,
					       struct strategy_buckchg_dev *info)
{
	int ffc_sts = strategy_class_fg_get_fastcharge();

	mca_log_info("value: %d, ffc_sts: %d\n", value, ffc_sts);

	if (value > 0 && ffc_sts == 1)
		strategy_class_fg_set_fastcharge(false);

	if (value == 1 || value == 2) {
		mca_vote(info->charge_limit_voter, "csd_pulse", true,
			 CSD_PULSE_FCC);
		info->csd_flag = true;
		queue_delayed_work(system_wq, &info->csd_pulse_process_work,
				   msecs_to_jiffies(CSD_PULSE_HOLD_MS));
	} else if (value == 0) {
		cancel_delayed_work_sync(&info->csd_pulse_process_work);
		mca_vote(info->charge_limit_voter, "csd_pulse", false, 0);
		mca_vote(info->chg_enable_voter, "csd_pulse", false, 0);
		info->csd_flag = false;
	} else if (value == 3) {
		mca_vote(info->charge_limit_voter, "csd_pulse", true,
			 CSD_PULSE_FCC);
	}
}

static void strategy_csd_pulse_process_workfunc(struct work_struct *work)
{
	struct strategy_buckchg_dev *info =
		container_of(to_delayed_work(work), struct strategy_buckchg_dev,
			     csd_pulse_process_work);

	if (info->csd_flag) {
		mca_vote(info->charge_limit_voter, "csd_pulse", false, 0);
		mca_vote(info->chg_enable_voter, "csd_pulse", true, 0);
		info->csd_flag = false;
		queue_delayed_work(system_wq, &info->csd_pulse_process_work,
				   msecs_to_jiffies(CSD_PULSE_HOLD_MS));
		mca_log_info("vote fcc and discharge logic\n");
		return;
	}

	mca_vote(info->chg_enable_voter, "csd_pulse", false, 0);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
				  MCA_EVENT_CSD_SEND_PULSE, 0);
	mca_log_info("resume charging logic\n");
}

static void strategy_buckchg_process_pmic_init_done(struct strategy_buckchg_dev *info)
{
	mca_log_info("deal with pmic_init_done\n");
	mca_rerun_election(info->chg_enable_voter);
	mca_rerun_election(info->input_limit_voter);
	mca_rerun_election(info->charge_limit_voter);
	mca_rerun_election(info->iterm_voter);
	mca_rerun_election(info->vterm_voter);
}

/*
 * Whether quick reverse charging may run: a device on the port, nothing
 * wireless going on, a battery that is not cold, a board that is not hot,
 * and enough charge (less once the device has authenticated).
 */
static void strategy_buckchg_check_reverse_quick_charge(struct strategy_buckchg_dev *info,
							int *allowed)
{
	bool otg_present = false;
	int batt_temp = 0;
	int vbus = 0;
	int soc_thres;
	int soc;

	strategy_class_fg_ops_get_temperature(&batt_temp);
	soc = strategy_class_fg_ops_get_soc();
	batt_temp /= 10;
	protocol_class_pd_get_otg_plugin_status(TYPEC_PORT_0, &otg_present);

	mca_log_info("otg_present = %d, batt_temp = %d, thermal_board_temp =%d\n",
		     otg_present, batt_temp, info->thermal_board_temp);

	if (!otg_present || info->wls_online || info->wls_revchg_online ||
	    batt_temp < 0 || info->thermal_board_temp > REVCHG_BOARD_TEMP_MAX) {
		*allowed = 0;
		return;
	}

	soc_thres = info->reverse_auth_sts ? REVCHG_SOC_KEEP : REVCHG_SOC_START;
	*allowed = soc > soc_thres;

	if (info->cp_vendor != SC8541_VENDOR || soc <= soc_thres)
		return;

	platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER, &vbus);
	vbus /= 1000;
	platform_class_cp_get_pmid2out_uvp_dis(CP_ROLE_MASTER, false);
	mca_log_err("quick revchg, check vbus: %d uvp_dis_enable %d\n", vbus,
		    0);
}

static void strategy_buckchg_report_revchg(int value)
{
	char event[MCA_EVENT_NOTIFY_SIZE] = { 0 };
	struct mca_event_notify_data event_data = { 0 };

	event_data.event_len = snprintf(event, sizeof(event),
					"POWER_SUPPLY_REVERSE_QUICK_CHARGE=%d",
					value);
	event_data.event = event;
	mca_event_report_uevent(&event_data);
}

/*
 * The adapter's reverse authentication state, packed one byte per field:
 * position, whether it has authenticated, whether it is done, and whether
 * the OTG boost is disabled.  Position 1 brings the external boost up and
 * takes the pump out of the way; position 2 turns the pump around to halve
 * the battery voltage into the port.  Losing the device at any point ends it
 * and puts everything back.
 */
static void strategy_buckchg_cp_revert_handler(int auth_pos,
					       struct strategy_buckchg_dev *info)
{
	static int last_pos = REVCHG_POS_BOOST;
	static bool bq_revchg_switch_flag;
	int otg_boost_disable = (auth_pos >> 24) & 0xff;
	int auth_done = (auth_pos >> 16) & 0xff;
	int pos = auth_pos & 0xff;
	bool otg_present = false;
	int allowed = 0;

	info->reverse_auth_sts = (auth_pos & 0xff00) != 0;

	if (!otg_boost_disable)
		protocol_class_pd_get_otg_plugin_status(TYPEC_PORT_0,
							&otg_present);

	mca_log_err("otg_present: %d, otg_boost_disable: %d, reverse_auth_sts: %d, pos: %d, last_pos: %d, auth_done: %d\n",
		    otg_present, otg_boost_disable, info->reverse_auth_sts, pos,
		    last_pos, auth_done);

	if (auth_done && otg_present) {
		strategy_buckchg_check_reverse_quick_charge(info, &allowed);
		strategy_buckchg_report_revchg(allowed);
		mca_log_err("status: %d\n", allowed);
		return;
	}

	if (otg_present && last_pos == pos) {
		mca_log_err("same handler, ignore...");
		goto dump;
	}

	last_pos = pos;
	if (!otg_present)
		cancel_delayed_work_sync(&info->source_status_monitor_work);

	platform_class_buckchg_ops_get_otg_boost_src(MAIN_BUCK_CHARGER,
						     &info->otg_boost_src);

	if (!otg_present) {
		mca_log_err("end revert 1_2 cp");
		if (!info->otg_boost_src) {
			platform_class_cp_set_adjustadble_timeout(CP_ROLE_MASTER,
								  REVCHG_TIMEOUT);
			if (info->cp_vendor == SC8541_VENDOR) {
				mca_log_err("exit sc8541 1_2 cp manual mode");
				platform_class_cp_set_manual_revchg_mode(CP_ROLE_MASTER,
									 false);
			}
			if (info->cp_vendor == BQ25960_VENDOR) {
				mca_log_err("exit bq25960 1_2 cp reverse mode");
				platform_class_cp_set_cp_reverse_mode(CP_ROLE_MASTER,
								      false);
				bq_revchg_switch_flag = false;
			}
		} else {
			platform_class_cp_set_charging_enable(CP_ROLE_MASTER,
							      false);
			platform_class_cp_enable_ovpgate(CP_ROLE_MASTER, true);
			platform_class_cp_enable_wpcgate(CP_ROLE_MASTER, false);
			platform_class_cp_set_qb(CP_ROLE_MASTER, false);
			platform_class_cp_set_mode(CP_ROLE_MASTER,
						   CP_MODE_FORWARD_2_1);
		}

		last_pos = REVCHG_POS_BOOST;
		info->last_gear_shift = REVCHG_GEAR_NORMAL;
		info->start_quick_revchg = false;
		info->revchg_bcl = false;
		info->ibat_check_cnt = 0;
		info->gear_shift_sent = 0;
		strategy_buckchg_report_revchg(0);
		goto dump;
	}

	platform_class_cp_get_chip_vendor(CP_ROLE_MASTER, &info->cp_vendor);

	if (pos == REVCHG_POS_DIV2) {
		mca_log_err("start revert 1_2 cp");
		if (info->cp_vendor == BQ25960_VENDOR && !bq_revchg_switch_flag) {
			mca_log_err("entry bq25960 1_2 cp reverse mode");
			platform_class_cp_set_cp_reverse_mode(CP_ROLE_MASTER, true);
			bq_revchg_switch_flag = true;
		} else {
			platform_class_cp_enable_acdrv_manual(CP_ROLE_MASTER, true);
			platform_class_cp_set_adjustadble_timeout(CP_ROLE_MASTER, 0);
			platform_class_cp_set_mode(CP_ROLE_MASTER,
						   CP_MODE_REVERSE_1_2);
			platform_class_cp_enable_wpcgate(CP_ROLE_MASTER, false);
			platform_class_cp_enable_ovpgate(CP_ROLE_MASTER, true);
			platform_class_cp_set_qb(CP_ROLE_MASTER, true);
			platform_class_cp_set_charging_enable(CP_ROLE_MASTER, true);
			if (info->cp_vendor == SC8541_VENDOR) {
				mca_log_err("entry sc8541 1_2 cp manual mode");
				platform_class_cp_set_manual_revchg_mode(CP_ROLE_MASTER,
									 true);
			}
		}
		if (info->otg_boost_src) {
			msleep(REVCHG_SETTLE_MS);
			platform_class_buckchg_ops_set_boost_enable(MAIN_BUCK_CHARGER,
				(OTG_EN_BOOST << 16) | (EXTERNAL_BOOST << 8) | 0);
		}
	} else if (pos == REVCHG_POS_BOOST && info->otg_boost_src) {
		mca_log_err("start external boost");
		platform_class_buckchg_ops_set_boost_enable(MAIN_BUCK_CHARGER,
			(OTG_EN_BOOST << 16) | (EXTERNAL_BOOST << 8) | 1);
		msleep(REVCHG_SETTLE_MS);
		platform_class_cp_set_charging_enable(CP_ROLE_MASTER, false);
		platform_class_cp_enable_ovpgate(CP_ROLE_MASTER, false);
		platform_class_cp_enable_wpcgate(CP_ROLE_MASTER, false);
		platform_class_cp_set_qb(CP_ROLE_MASTER, false);
		platform_class_cp_set_mode(CP_ROLE_MASTER, CP_MODE_FORWARD_2_1);
	}

dump:
	platform_class_cp_dump_register(CP_ROLE_MASTER);
}

/*
 * While quick reverse charging, pick the gear the port sources at.  With
 * support_revchg_screenon the screen being off drops to the step-down gear
 * once the battery current has stayed low, and asks userspace for the BCL
 * first.  When the step down is taken, wait for the pump to stop sourcing
 * before telling userspace.
 */
static void strategy_source_status_monitor_workfunc(struct work_struct *work)
{
	struct strategy_buckchg_dev *info =
		container_of(to_delayed_work(work), struct strategy_buckchg_dev,
			     source_status_monitor_work);
	int gear = REVCHG_GEAR_NORMAL;
	int ibus = 0;
	int i, ret;

	strategy_buckchg_check_reverse_quick_charge(info, &gear);
	if (!info->support_revchg_screenon)
		goto shift;

	if (gear == REVCHG_GEAR_QUICK) {
		if (info->screen_status) {
			gear = REVCHG_GEAR_STEPDOWN;
			info->ibat_check_cnt = 0;
			goto shift;
		}
		if (info->ibat_check_cnt > REVCHG_IBAT_CHECK_MAX) {
			if (!info->revchg_bcl) {
				strategy_buckchg_report_revchg(REVCHG_UEVENT_REQUEST_BCL);
				mca_log_err("revchg request bcl\n");
				gear = REVCHG_GEAR_STEPDOWN;
			}
			goto shift;
		}
		gear = REVCHG_GEAR_STEPDOWN;
	} else if (info->screen_status) {
		info->ibat_check_cnt = 0;
		goto shift;
	}

	strategy_class_fg_ops_get_current(&info->proc_data.ibat);
	mca_log_info("source_status_monitor ibat: %d\n", info->proc_data.ibat);
	if (info->proc_data.ibat < REVCHG_IBAT_THRESHOLD_UA)
		info->ibat_check_cnt++;
	else
		info->ibat_check_cnt = 0;

shift:
	if (gear == info->last_gear_shift)
		goto requeue;

	if (!info->support_revchg_screenon) {
		info->last_gear_shift = gear;
		mca_log_err("set gear shift: %d\n", gear);
		protocol_class_pd_set_gear_shift(TYPEC_PORT_0, info->last_gear_shift);
		goto requeue;
	}

	if (info->gear_shift_sent != gear) {
		mca_log_err("set gear shift: %d\n", gear);
		protocol_class_pd_set_gear_shift(TYPEC_PORT_0, gear);
		info->gear_shift_sent = gear;
	}

	if (gear == REVCHG_GEAR_STEPDOWN &&
	    info->last_gear_shift == REVCHG_GEAR_QUICK) {
		info->revchg_bcl = false;
		for (i = 0; i < REVCHG_IBUS_SETTLE_RETRY; i++) {
			platform_class_cp_enable_adc(CP_ROLE_MASTER, true);
			mdelay(REVCHG_ADC_SETTLE_MS);
			ret = platform_class_cp_get_bus_current(CP_ROLE_MASTER,
								&ibus);
			mca_log_err("ret: %d, ibus: %d\n", ret, ibus);
			/* Still sourcing: try again on the next pass. */
			if (ret || ibus > REVCHG_IBUS_SETTLED_MA)
				goto requeue;
		}
		strategy_buckchg_report_revchg(REVCHG_GEAR_STEPDOWN);
	}
	info->last_gear_shift = gear;

requeue:
	queue_delayed_work(system_wq, &info->source_status_monitor_work,
			   msecs_to_jiffies(!info->support_revchg_screenon ||
					    gear == REVCHG_GEAR_NORMAL ?
					    SOURCE_STATUS_MONITOR_INTERVAL :
					    SOURCE_STATUS_MONITOR_FAST_INTERVAL));
}

static void strategy_buckchg_check_pdsecret_workfunc(struct work_struct *work)
{
	struct strategy_buckchg_dev *info =
		container_of(to_delayed_work(work), struct strategy_buckchg_dev,
			     check_pd_secret_work);

	mca_vote(info->input_limit_voter, "icl_limit", false,
		 PD_UNVERIFIED_ICL);
	mca_log_info("cancel icl_limit vote when using pd charger\n");
}

/*
 * With soc_limit_low/high set (debug_ctrl or the memory test), charging
 * stops at or above the high mark and resumes at or below the low one.
 */
static void strategy_buckchg_check_debug_soc_limit(struct strategy_buckchg_dev *info,
						   int soc)
{
	bool limit;

	if (info->soc_limit_low < 1 || info->soc_limit_high < 1 ||
	    info->soc_limit_high <= info->soc_limit_low) {
		if (!mca_is_client_vote_enabled(info->chg_enable_voter,
						"debug_soc_limit"))
			return;
		limit = false;
	} else if (soc >= info->soc_limit_high) {
		limit = true;
	} else if (soc <= info->soc_limit_low) {
		limit = false;
	} else {
		return;
	}

	mca_vote(info->chg_enable_voter, "debug_soc_limit", limit, 0);
	mca_vote(info->input_limit_voter, "debug_soc_limit", limit, 0);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
				  MCA_EVENT_DEBUG_CTRL_SOC_LIMIT, limit);
}

/* Below 91 % the quick charge strategy is asked to (re)start on its adapters. */
static void strategy_buckchg_trigger_quick_charge(struct strategy_buckchg_dev *info,
						  struct mca_hwid_info *hwid)
{
	ktime_t time_now;

	switch (info->proc_data.real_type) {
	case XM_CHARGER_TYPE_HVDCP3P5:
		/* QC3+ quick charging is only offered on Chinese units. */
		if (!hwid || hwid->country_version != CountryCN)
			break;
		fallthrough;
	case XM_CHARGER_TYPE_HVDCP3_B:
	case XM_CHARGER_TYPE_PD_VERIFY:
		mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					  MCA_EVENT_CHARGE_ACTION, 0);
		break;
	case XM_CHARGER_TYPE_PPS:
		time_now = ktime_get_boottime();
		if (!info->is_eu_model) {
			if (info->verify_process_end)
				mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
							  MCA_EVENT_CHARGE_ACTION, 0);
			break;
		}
		mca_log_info("eu_start_time %lld, time_now %lld  delta = %lld\n",
			     info->proc_data.eu_start_time, time_now,
			     ktime_ms_delta(time_now, info->proc_data.eu_start_time));
		if (ktime_to_ns(ktime_sub(time_now, info->proc_data.eu_start_time)) >=
		    (s64)EU_PPS_QUICKCHG_RETRY_MS * NSEC_PER_MSEC) {
			mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
						  MCA_EVENT_CHARGE_ACTION, 0);
			info->proc_data.eu_start_time = time_now;
			mca_log_info("trigger MCA_EVENT_CHARGE_ACTION\n");
		}
		break;
	default:
		break;
	}
}

/*
 * Above 90 % on a verified adapter the buck charger owns the charge, and
 * decides when fast charge mode (the higher float voltage) starts and stops:
 * it starts below 95 % in the allowed temperature range once the battery has
 * authenticated, stops outside that range, and stops when the charge current
 * has tapered to the term current.
 */
static void strategy_buckchg_enable_fast_charge_mode(struct strategy_buckchg_dev *info,
						     int soc)
{
	int iterm = mca_get_effective_result(info->iterm_voter);
	int temp = 0, temp_offset_flag = 0;
	struct mca_votable *ich;
	const char *client;
	int fastcharge, fcc;

	if (info->terminated_by_cp)
		return;

	strategy_class_fg_ops_get_temperature(&temp);
	temp /= 10;
	fastcharge = strategy_class_fg_get_fastcharge();

	if (info->support_base_flip || info->base_flip_same) {
		iterm = info->parallel_iterm;
		mca_log_info("iterm: %d\n", iterm);

		/*
		 * A flip pack that has tapered to its term current while hot
		 * is moved onto the PPS limits at once.
		 */
		if (info->base_flip_same && fastcharge) {
			fcc = mca_get_effective_result(info->charge_limit_voter);
			strategy_class_fg_get_temp_offset_flag(&temp_offset_flag);
			if (info->proc_data.real_type == XM_CHARGER_TYPE_PD_VERIFY &&
			    fcc <= iterm &&
			    (temp > BUCKCHG_HOT_PACK_TEMP_C ||
			     (temp > BUCKCHG_WARM_PACK_TEMP_C && temp_offset_flag))) {
				if (info->proc_data.voltage == STATEGY_CHARGE_VBUS_5V) {
					mca_vote(info->buck_5v_in_voter,
						 "wire_chg_type", true,
						 info->in_pps);
					ich = info->buck_5v_ich_voter;
				} else {
					mca_vote(info->buck_9v_in_voter,
						 "wire_chg_type", true,
						 info->in_pps);
					ich = info->buck_9v_ich_voter;
				}
				mca_vote(ich, "wire_chg_type", true,
					 info->chg_pps);
			}
		}
	}

	mca_log_info("allow_start_ffc_batt_soc_thr: %d\n",
		     info->allow_start_ffc_batt_soc_thr);

	if (info->proc_data.real_type != XM_CHARGER_TYPE_PD_VERIFY)
		return;

	fcc = mca_get_effective_result(info->charge_limit_voter);
	mca_log_info("batt_temp = %d, soc = %d, fcc = %d\n", temp, soc, fcc);

	if (soc <= FFC_START_SOC_MAX && !fastcharge &&
	    temp >= info->ffc_temp_low && temp <= info->ffc_temp_high &&
	    info->batt_auth) {
		strategy_class_fg_set_fastcharge(true);
		mca_log_info("buck charger enable fast charge mode\n");
	} else if (temp < info->ffc_temp_low || temp > info->ffc_temp_high) {
		strategy_class_fg_set_fastcharge(false);
		mca_log_info("buck charger disable fast charge mode\n");
	} else if (fastcharge && soc >= info->allow_start_ffc_batt_soc_thr &&
		   fcc <= iterm + info->curr_term_compensation) {
		/*
		 * On a flip pack the pump runs alongside the buck charger and
		 * holds its share down through this very election, so a low
		 * current from that client says nothing about how full it is.
		 */
		if (info->support_base_flip || info->base_flip_same) {
			client = mca_get_effective_client(info->charge_limit_voter);
			if (client && !strcmp(client, "fcc_limit")) {
				mca_log_info("client_srt:%s is pmic + cp charging, skip\n",
					     client);
				return;
			}
		}
		strategy_class_fg_set_fastcharge(false);
		mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					  MCA_EVENT_FCC_TOO_LOW, 0);
		mca_log_info("buck charger disable fast charge mode by fcc too low\n");
	}
}

/*
 * A charge pump that has stopped answering leaves the port without
 * over-voltage protection on boards whose protection sits behind the pump,
 * so once it has failed to answer for long enough the adapter is asked back
 * down to 5 V.
 */
static void strategy_buckchg_check_cp_i2c_err(struct strategy_buckchg_dev *info)
{
	bool cp_present = false;

	if (!info->vusb_ovp_location)
		return;

	if (!platform_class_cp_get_present(CP_ROLE_MASTER, &cp_present) &&
	    cp_present) {
		info->cp_absent_retry_count = 0;
		info->cp_i2c_error_reported = false;
		return;
	}

	if (info->cp_absent_retry_count < BUCKCHG_CP_ABSENT_RETRY_MAX) {
		info->cp_absent_retry_count++;
		mca_log_info("CP not present, retrying... retry_count: %d",
			     info->cp_absent_retry_count);
		return;
	}

	if (info->cp_i2c_error_reported ||
	    info->vusb_ovp_location != BUCKCHG_VUSB_OVP_BEFORE_CP)
		return;

	mca_log_err("Max retries reached, CP still not present.");
	mca_log_info("vusb_ovp_location is BEFORE_CP, forcing 5V, reporting error.");
	mca_vote(info->input_voltage_voter, "cp_i2c_error", true, 0);
	info->cp_i2c_error_reported = true;
}

/*
 * Keep the bus at the voltage asked for, protect the inductor from an
 * adapter that overshoots, and ask a non-compliant QC adapter down once.
 */
static void strategy_buckchg_check_charge_volt(struct strategy_buckchg_dev *info)
{
	int target_volt = info->proc_data.voltage;
	int vbus = 0;

	if (info->wls_revchg_en) {
		mca_log_info("wireless reverse is charging, do not request volt\n");
		return;
	}

	if (strategy_buckchg_is_pd(info->proc_data.real_type) &&
	    !info->pwr_cap.pdo_nums) {
		mca_log_info("pd pwr_cap nums is null\n");
		return;
	}

	platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER, &vbus);
	vbus /= 1000;
	if ((target_volt == STATEGY_CHARGE_VBUS_9V && vbus < CHECK_VBUS_9V_LOW_TH) ||
	    (target_volt == STATEGY_CHARGE_VBUS_5V && vbus > CHECK_VBUS_5V_HIGH_TH)) {
		mca_log_info("target_volt: %d, vbus: %d\n", target_volt, vbus);
		strategy_buckchg_set_charge_volt(info, target_volt);
	}

	platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER, &vbus);
	vbus /= 1000;
	if (target_volt == STATEGY_CHARGE_VBUS_9V && vbus >= VBUS_9V_OVP_VOLTAGE) {
		mca_vote(info->buck_9v_in_voter, "pmic_inductor", true,
			 PMIC_INDUCTOR_SECURE_ICL);
		mca_log_err("hvdcp_9v vbus_ovp decrease input current limit\n");
		mca_vote(info->buck_5v_in_voter, "pmic_inductor", false, 0);
	} else {
		mca_vote(info->buck_9v_in_voter, "pmic_inductor", false, 0);
		if (target_volt == STATEGY_CHARGE_VBUS_5V &&
		    vbus > VBUS_5V_OVP_VOLTAGE) {
			mca_vote(info->buck_5v_in_voter, "pmic_inductor", true,
				 PMIC_INDUCTOR_SECURE_ICL);
			mca_log_err("lvdcp_5v vbus_ovp decrease input current limit\n");
		} else {
			mca_vote(info->buck_5v_in_voter, "pmic_inductor", false,
				 0);
		}
	}

	if (info->is_non_compliant_qc && !info->non_compliant_run_once) {
		mca_log_err("non_compliant_qc: target_volt: %d\n", target_volt);
		strategy_buckchg_set_charge_volt(info, target_volt);
		mca_vote(info->input_limit_voter, "non_compliant_qc", true,
			 NON_COMPLIANT_QC_ICL);
		strategy_class_buckchg_ops_set_chg(info, false);
		strategy_class_buckchg_ops_set_chg(info, true);
		mca_log_err("restart aicl and recover buck charging\n");
		platform_class_buckchg_ops_set_rerun_aicl(MAIN_BUCK_CHARGER, true);
		info->non_compliant_run_once = true;
	}
}

/*
 * The input limit and charge current for the charger type, voted into the
 * pair of votables for the bus voltage in use.  A QC adapter not (yet) at
 * 9 V is treated as a DCP.
 */
static void strategy_buckchg_select_charg_para(struct strategy_buckchg_dev *info)
{
	int real_type = info->proc_data.real_type;
	int volt = info->proc_data.voltage;
	int in_pd = info->in_pd, chg_pd = info->chg_pd;
	int temp = 0, temp_offset_flag = 0;
	int ibus_limit, ibat_limit, cap_curr;
	bool cp_enabled = false;
	bool ibus_changed = false;
	int fastcharge;

	if (strategy_buckchg_is_qc(real_type) && volt != STATEGY_CHARGE_VBUS_9V)
		real_type = XM_CHARGER_TYPE_DCP;

	/* A float charger on a two-cell pack leaves the pump on the pack. */
	if (info->base_flip_same && real_type == XM_CHARGER_TYPE_FLOAT) {
		platform_class_cp_set_busovp(CP_ROLE_MASTER,
					     BUCKCHG_FLOAT_CP_BUSOVP_MV);
		platform_class_cp_get_charging_enabled(CP_ROLE_MASTER,
						       &cp_enabled);
		mca_log_info("cp_enabled = %d \n", cp_enabled);
	}

	fastcharge = strategy_class_fg_get_fastcharge();
	strategy_class_fg_ops_get_temperature(&temp);
	temp /= 10;
	strategy_class_fg_get_temp_offset_flag(&temp_offset_flag);

	/* A hot flip pack on a verified adapter takes the PPS limits. */
	if (info->base_flip_same && fastcharge &&
	    real_type == XM_CHARGER_TYPE_PD_VERIFY &&
	    (temp > BUCKCHG_HOT_PACK_TEMP_C ||
	     (temp > BUCKCHG_WARM_PACK_TEMP_C && temp_offset_flag))) {
		in_pd = info->in_pps;
		chg_pd = info->chg_pps;
	}
	mca_log_info("iin_pd = %d, ichg_pd = %d\n", in_pd, chg_pd);

	switch (real_type) {
	case XM_CHARGER_TYPE_UNKNOW:
		ibus_limit = 100;
		ibat_limit = 100;
		break;
	case XM_CHARGER_TYPE_SDP:
		ibus_limit = info->is_eu_model ? info->in_sdp - 50 : info->in_sdp;
		ibat_limit = CHARGE_SDP_CHARGE_DEFAULT;
		break;
	case XM_CHARGER_TYPE_CDP:
		ibus_limit = info->in_cdp;
		ibat_limit = info->chg_cdp;
		break;
	case XM_CHARGER_TYPE_DCP:
		if (info->pwr_boost_state) {
			ibus_limit = CHARGE_DCP_INPUT_BOOST;
			ibat_limit = CHARGE_DCP_CHARGE_BOOST;
		} else {
			ibus_limit = info->in_dcp;
			ibat_limit = info->chg_dcp;
		}
		break;
	case XM_CHARGER_TYPE_FLOAT:
	case XM_CHARGER_TYPE_OCP:
		ibus_limit = info->in_float;
		ibat_limit = info->chg_float;
		break;
	case XM_CHARGER_TYPE_HVDCP2:
		ibus_limit = info->in_hvdcp;
		ibat_limit = info->chg_hvdcp;
		break;
	case XM_CHARGER_TYPE_HVDCP3:
	case XM_CHARGER_TYPE_HVDCP3_B:
		ibus_limit = info->in_hvdcp3;
		ibat_limit = info->chg_hvdcp3;
		break;
	case XM_CHARGER_TYPE_HVDCP3P5:
		ibus_limit = info->in_hvdcp3p5;
		ibat_limit = info->chg_hvdcp3p5;
		break;
	case XM_CHARGER_TYPE_TYPEC:
		ibus_limit = info->in_dcp;
		ibat_limit = info->chg_dcp;
		break;
	case XM_CHARGER_TYPE_PD:
	case XM_CHARGER_TYPE_PPS:
	case XM_CHARGER_TYPE_PD_VERIFY:
		if (info->pps_ptf == USBPD_BUCK_DPM_PORT_PPS_PTF_NOT_OVERTEMP) {
			ibus_limit = CHARGE_PPS_PTF_INPUT_DEFAULT;
			ibat_limit = CHARGE_PPS_PTF_CHARGE_DEFAULT;
			break;
		}
		cap_curr = info->pwr_cap.cap[info->proc_data.curr_pd_pos].max_current;
		ibus_limit = min_t(unsigned int, in_pd, cap_curr);
		ibat_limit = chg_pd;
		if (!cap_curr) {
			mca_log_info("pdo broadcast abnormal %d \n",
				     info->proc_data.curr_pd_pos);
			ibus_limit = in_pd;
		}
		break;
	default:
		ibus_limit = info->in_sdp;
		ibat_limit = info->chg_sdp;
		break;
	}

	/* Until the battery is known to be there, the input stays low. */
	if (info->batt_miss)
		ibus_limit = info->batt_type ? STAEGY_BATT_MISS_ICL_PARALLEL :
					       STAEGY_BATT_MISS_ICL;

	if (ibus_limit != info->proc_data.ibus_limit ||
	    volt != info->proc_data.pre_volt) {
		mca_vote(volt == STATEGY_CHARGE_VBUS_5V ? info->buck_5v_in_voter :
							  info->buck_9v_in_voter,
			 "wire_chg_type", true, ibus_limit);
		info->proc_data.ibus_limit = ibus_limit;
		ibus_changed = true;
	}

	if (ibat_limit != info->proc_data.ibat_limit ||
	    volt != info->proc_data.pre_volt) {
		/*
		 * The pair for the new rail is asked again, so the limits it
		 * recorded while unused reach the charger now.
		 */
		if (volt == STATEGY_CHARGE_VBUS_5V) {
			mca_vote(info->buck_5v_ich_voter, "wire_chg_type", true,
				 ibat_limit);
			info->proc_data.ibat_limit = ibat_limit;
			if (info->proc_data.pre_volt != STATEGY_CHARGE_VBUS_5V) {
				mca_rerun_election(info->buck_5v_in_voter);
				mca_rerun_election(info->buck_5v_ich_voter);
				info->proc_data.pre_volt = volt;
			}
		} else {
			mca_vote(info->buck_9v_ich_voter, "wire_chg_type", true,
				 ibat_limit);
			info->proc_data.ibat_limit = ibat_limit;
			if (volt != info->proc_data.pre_volt) {
				mca_rerun_election(info->buck_9v_in_voter);
				mca_rerun_election(info->buck_9v_ich_voter);
				info->proc_data.pre_volt = volt;
			}
		}
	} else if (!ibus_changed) {
		return;
	}

	mca_log_info("ibus_limit = %d, ibat_limit = %d\n", ibus_limit,
		     ibat_limit);
	platform_class_buckchg_ops_set_restart_aicl(MAIN_BUCK_CHARGER, true);
}

static void strategy_buckchg_set_charge_volt(struct strategy_buckchg_dev *info,
					     int target_volt)
{
	int real_type = info->proc_data.real_type;

	if (!target_volt) {
		mca_log_info("target_volt = 0v is invalid\n");
		return;
	}

	if (strategy_buckchg_is_qc(real_type)) {
		platform_class_buckchg_ops_set_qc_volt(MAIN_BUCK_CHARGER,
						       target_volt);
		return;
	}

	switch (real_type) {
	case XM_CHARGER_TYPE_PD:
		if (!info->pwr_cap.pdo_nums) {
			mca_log_info("pwr_cap nums is null\n");
			return;
		}
		break;
	case XM_CHARGER_TYPE_PPS:
	case XM_CHARGER_TYPE_PD_VERIFY:
		if (!info->pwr_cap.pdo_nums) {
			mca_log_info("pwr_cap nums is null\n");
			return;
		}
		if (target_volt != STATEGY_CHARGE_VBUS_5V &&
		    target_volt != STATEGY_CHARGE_VBUS_9V &&
		    target_volt != STATEGY_CHARGE_VBUS_12V) {
			mca_log_info("set no fix pdo\n");
			protocol_class_set_adapter_volt_and_curr(ADAPTER_PROTOCOL_PPS,
								 target_volt,
								 CHARGE_PPS_INPUT_DEFAULT);
			return;
		}
		break;
	default:
		return;
	}

	protocol_class_pd_set_fixed_volt(TYPEC_PORT_0, target_volt);
}

static void strategy_buck_update_req_volt(struct strategy_buckchg_dev *info)
{
	int real_type = info->proc_data.real_type;
	int allow_9v;

	if (strategy_buckchg_is_qc(real_type))
		allow_9v = 1;
	else if (strategy_buckchg_is_pd(real_type))
		allow_9v = info->proc_data.is_pd_9v;
	else
		allow_9v = 0;

	mca_vote(info->input_voltage_voter, "real_type", true, allow_9v);
	mca_log_info("real_type = %d, input_voltage_voter effective_result: %d\n",
		     real_type,
		     mca_get_effective_result(info->input_voltage_voter));
}

/*
 * A QC adapter that stays under 7.2 V with almost no current at 9 V is not
 * delivering what it promised; after three such passes it is asked to 5 V.
 */
static void strategy_buckchg_check_non_compliant_qc_charger(struct strategy_buckchg_dev *info)
{
	static int count;
	int target_volt = info->proc_data.voltage;
	int vbus = 0;

	if (target_volt == STATEGY_CHARGE_VBUS_5V || info->is_non_compliant_qc)
		return;

	if (info->proc_data.chg_status == MCA_BUCK_CHG_STS_CHARGE_DONE) {
		mca_log_info("charge done, no need check non_compliant_qc_charger\n");
		return;
	}

	if (info->proc_data.real_type != XM_CHARGER_TYPE_HVDCP2 &&
	    info->proc_data.real_type != XM_CHARGER_TYPE_HVDCP3) {
		mca_log_info("only qc2/qc3 need check\n");
		return;
	}

	platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER, &vbus);
	vbus /= 1000;
	platform_class_buckchg_ops_get_bus_curr(MAIN_BUCK_CHARGER,
						&info->proc_data.ibus);
	if (target_volt == STATEGY_CHARGE_VBUS_9V &&
	    vbus < VBUS_QC_VOL_THRESHOLD_LOW &&
	    info->proc_data.ibus < NON_COMPLIANT_QC_IBUS_THR_LOW_UA) {
		mca_log_info("target_volt: %d, vbus: %d\n", target_volt, vbus);
		count++;
	}

	if (count > NON_COMPLIANT_QC_COUNT_MAX && !info->is_non_compliant_qc) {
		info->is_non_compliant_qc = true;
		count = 0;
		mca_log_err("qc is non-compliant, set request vol to 5V\n");
		mca_vote(info->input_voltage_voter, "non_compliant_qc", true, 0);
	}
}

static void strategy_buckchg_update_charge_status(struct strategy_buckchg_dev *info)
{
	int main_chg_status = 0, aux_chg_status = 0;
	int adsp_icl = 0;
	int icl, ichg, suspend;

	platform_class_buckchg_ops_get_chg_status(MAIN_BUCK_CHARGER,
						  &main_chg_status);
	platform_class_buckchg_ops_get_chg_status(AUX_BUCK_CHARGER,
						  &aux_chg_status);
	platform_class_buckchg_ops_get_bus_curr(MAIN_BUCK_CHARGER,
						&info->proc_data.ibus);
	platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER,
						&info->proc_data.vbus);
	strategy_class_fg_ops_get_voltage(&info->proc_data.vbat);
	platform_class_buckchg_ops_get_input_curr_lmt(MAIN_BUCK_CHARGER,
						      &adsp_icl);
	icl = mca_get_effective_result(info->input_limit_voter);
	ichg = mca_get_effective_result(info->charge_limit_voter);
	suspend = mca_get_effective_result(info->input_suppend_voter);

	if ((unsigned int)main_chg_status < ARRAY_SIZE(buck_chg_status_map))
		info->proc_data.chg_status = buck_chg_status_map[main_chg_status];
	else
		info->proc_data.chg_status = MCA_BUCK_CHG_STS_NA;

	if (strategy_class_fg_ops_get_charging_done())
		info->proc_data.chg_status = MCA_BUCK_CHG_STS_CHARGE_DONE;

	mca_log_err("pmic_chg_status: %d, chg_status: %d, chg_en: [%d][%s], chg_type: %d, vbat: %d, vbus: %d, ibus: %d\n",
		    main_chg_status, info->proc_data.chg_status,
		    mca_get_effective_result(info->chg_enable_voter),
		    mca_get_effective_client(info->chg_enable_voter),
		    info->proc_data.real_type, info->proc_data.vbat,
		    info->proc_data.vbus, info->proc_data.ibus);
	mca_log_err("ap_icl:[%s][%d], adsp_icl[%d], ichg:[%s][%d], input_suspend:[%s][%d]\n",
		    mca_get_effective_client(info->input_limit_voter), icl,
		    adsp_icl, mca_get_effective_client(info->charge_limit_voter),
		    ichg, mca_get_effective_client(info->input_suppend_voter),
		    suspend);
}

static void strategy_buckchg_update_aicl_cfg(struct strategy_buckchg_dev *info)
{
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	int vbat = info->proc_data.vbat;
	int reg_aicl_thd = 0;
	int aicl_thd;

	if (info->proc_data.chg_status == MCA_BUCK_CHG_NO_CHARGING ||
	    info->proc_data.chg_status == MCA_BUCK_CHG_STS_CHARGE_DONE) {
		mca_log_info("discharging or done, should set aicl vth to 4.1V\n");
		aicl_thd = STATEGY_CHARGE_AICL_TH_4P1V;
	} else if (info->proc_data.real_type >= XM_CHARGER_TYPE_HVDCP3 &&
		   info->proc_data.real_type <= XM_CHARGER_TYPE_HVDCP3P5) {
		return;
	} else {
		aicl_thd = vbat < STATEGY_CHARGE_AICL_VBAT_TH ?
			   STATEGY_CHARGE_AICL_TH_4P4V :
			   STATEGY_CHARGE_AICL_TH_4P5V;
		/* One board splits at 4.2 V and drops to 4.1 V below it. */
		if (hwid && hwid->platform_version == 1 &&
		    hwid->major_version == 1 && hwid->minor_version == 8)
			aicl_thd = vbat >= STATEGY_CHARGE_AICL_VBAT_TH_4P2V ?
				   STATEGY_CHARGE_AICL_TH_4P5V :
				   STATEGY_CHARGE_AICL_TH_4P1V;
	}

	platform_class_buckchg_ops_get_usb_aicl_cont_thd(MAIN_BUCK_CHARGER,
							 &reg_aicl_thd);
	if (aicl_thd != info->aicl_thd || aicl_thd != reg_aicl_thd) {
		mca_log_info("vbat: %d, aicl_thd: %d, reg_aicl_thd: %d\n", vbat,
			     aicl_thd, reg_aicl_thd);
		info->aicl_thd = aicl_thd;
		strategy_class_buckchg_ops_set_usb_aicl_cont_thd(info, aicl_thd);
	}
}

/*
 * Where the pump hands the pack back near the top of the charge, the buck
 * charger finishes a little under the target the pump was working to, until
 * the pack has terminated once.
 */
static void strategy_buckchg_cp_to_pmic_decrease_vterm(struct strategy_buckchg_dev *info)
{
	static int last_vterm;
	int first_termination = 0;
	int taper_cp_to_pmic = 0;
	int target_vterm = 0;
	int jeita_vterm;

	if (!info->need_cp_to_pmic ||
	    info->proc_data.real_type != XM_CHARGER_TYPE_HVDCP3_B)
		return;

	strategy_class_fg_get_first_termination(&first_termination);
	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
				     STRATEGY_STATUS_TYPE_CP_TO_PMIC,
				     &taper_cp_to_pmic);

	if (first_termination) {
		mca_vote(info->vterm_voter, "cp_to_pmic", false,
			 STATEGY_VTERM_DEFAULT_VALUE);
	} else {
		jeita_vterm = mca_get_client_vote(info->vterm_voter, "jeita");
		if (taper_cp_to_pmic && jeita_vterm != last_vterm) {
			target_vterm = jeita_vterm - BUCKCHG_CP_TO_PMIC_VTERM_DROP;
			mca_vote(info->vterm_voter, "cp_to_pmic", true,
				 target_vterm);
			last_vterm = jeita_vterm;
		}
	}

	mca_log_err("first_termination_flag: %d, taper_cp_to_pmic: %d, target_vterm: %d\n",
		    first_termination, taper_cp_to_pmic, target_vterm);
}

/*
 * In the cold or hot zone a charger that should be charging but is not is
 * counted, and reported after a few passes.
 */
static void strategy_buckchg_charge_abnormal_cold_or_hot_zone(struct strategy_buckchg_dev *info)
{
	int temp = 0;
	int icl, chg_en, vterm;
	bool cold, hot;

	strategy_class_fg_ops_get_temperature(&temp);
	cold = temp >= COLD_ZONE_LOW && temp <= COLD_ZONE_HIGH;
	hot = temp >= HOT_ZONE_LOW && temp <= HOT_ZONE_HIGH;
	if (!cold && !hot) {
		info->buck_abnormal_cnt = 0;
		mca_log_info("temp: %d, not in cold or hot zone\n", temp);
		return;
	}

	icl = mca_get_effective_result(info->input_limit_voter);
	chg_en = mca_get_effective_result(info->chg_enable_voter);
	vterm = mca_get_effective_result(info->vterm_voter);
	if (!icl || !chg_en || info->proc_data.vbat >= vterm) {
		info->buck_abnormal_cnt = 0;
		mca_log_info("user can stop charging icl: %d, chg_en: %d, vterm: %d, vbat: %d\n",
			     icl, chg_en, vterm, info->proc_data.vbat);
		return;
	}

	if (info->proc_data.chg_status != MCA_BUCK_CHG_NO_CHARGING) {
		info->buck_abnormal_cnt = 0;
		return;
	}

	if (info->buck_abnormal_cnt++ < ABNORMAL_ZONE_COUNT_MAX) {
		mca_log_err("maybe buck enable later, cnt:%d\n",
			    info->buck_abnormal_cnt);
		return;
	}

	mca_charge_mievent_report(cold ? CHARGE_DFX_LOW_TEMP_DISCHARGING :
					 CHARGE_DFX_HIGH_TEMP_DISCHARGING,
				  &temp, 1);
}

static void strategy_buckchg_monitor_workfunc(struct work_struct *work)
{
	struct strategy_buckchg_dev *info =
		container_of(to_delayed_work(work), struct strategy_buckchg_dev,
			     monitor_work);
	int quick_charge_status = MCA_QUICK_CHG_STS_CHARGE_FAILED;
	int interval = CHARGE_MONITOR_WORK_NORMAL_INTERVAL;
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	int soc, vterm, suspend, chg_en;

	soc = strategy_class_fg_ops_get_soc();
	strategy_buckchg_check_debug_soc_limit(info, soc);

	if (soc <= QUICK_CHG_SOC_MAX)
		strategy_buckchg_trigger_quick_charge(info, hwid);
	else
		strategy_buckchg_enable_fast_charge_mode(info, soc);

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
				     STRATEGY_STATUS_TYPE_CHARGING,
				     &quick_charge_status);
	if (quick_charge_status == MCA_QUICK_CHG_STS_CHARGING) {
		info->buck_abnormal_cnt = 0;
		if (info->support_pmic_vterm_dynamics_adjust)
			info->pmic_temp_term_flag = false;
		goto out;
	}

	strategy_buckchg_check_cp_i2c_err(info);
	strategy_buckchg_check_charge_volt(info);
	strategy_buckchg_select_charg_para(info);
	strategy_buckchg_update_charge_status(info);
	strategy_buckchg_check_non_compliant_qc_charger(info);
	strategy_buckchg_update_aicl_cfg(info);

	mca_log_info("pmic_chg_status: %d\n", info->proc_data.chg_status);

	if (info->proc_data.chg_status == MCA_BUCK_CHG_STS_CHARGING) {
		/* The temperature hold on the pump's handover comes off. */
		if (info->support_pmic_vterm_dynamics_adjust &&
		    !info->pmic_temp_term_flag) {
			mca_vote_override(info->vterm_voter, "pmic_temp_term",
					  false, 0);
			mca_vote_override(info->input_limit_voter,
					  "pmic_temp_term", false, 0);
			info->pmic_temp_term_flag = true;
		}
		if (info->proc_data.charge_done_force_5v) {
			info->proc_data.charge_done_force_5v = false;
			mca_vote(info->input_voltage_voter, "eoc_5v", false, 0);
		}
		if (info->proc_data.ibus < IBUS_LOW_RESTART_AICL_UA &&
		    mca_get_effective_result(info->input_limit_voter) >=
		    IBUS_LOW_RESTART_AICL_ICL_MIN) {
			mca_log_info("ibus abnormally low: %d, restart aicl\n",
				     info->proc_data.ibus);
			platform_class_buckchg_ops_set_restart_aicl(MAIN_BUCK_CHARGER,
								    true);
		}
		strategy_buckchg_cp_to_pmic_decrease_vterm(info);
	}

	if (info->proc_data.chg_status == MCA_BUCK_CHG_STS_CHARGE_DONE &&
	    !info->proc_data.charge_done_force_5v) {
		strategy_buckchg_sw_cv_stop(info);
		mca_vote(info->input_voltage_voter, "eoc_5v", true, 0);
		info->proc_data.charge_done_force_5v = true;
	}

	if (!info->proc_data.chg_en) {
		mca_vote(info->chg_enable_voter, "online", true, 1);
		info->proc_data.chg_en = 1;
	}

	if (mca_is_client_vote_enabled(info->input_voltage_voter,
				       "recover_force_5v") &&
	    info->proc_data.chg_status == MCA_BUCK_CHG_STS_CHARGING) {
		mca_log_err("buck charging recovered, disable force 5V\n");
		mca_vote(info->input_voltage_voter, "recover_force_5v", false, 0);
	}

	if (info->support_base_flip || info->base_flip_same)
		vterm = mca_get_client_vote(info->vterm_voter, "jeita");
	else
		vterm = mca_get_effective_result(info->vterm_voter);
	suspend = mca_get_effective_result(info->input_suppend_voter);
	chg_en = mca_get_effective_result(info->chg_enable_voter);

	/*
	 * A charger that is enabled but not charging is bounced; above 6 V
	 * the bus is asked down to 5 V first, the usual reason it will not
	 * start.
	 */
	if (!suspend && chg_en &&
	    info->proc_data.chg_status == MCA_BUCK_CHG_NO_CHARGING) {
		if (info->proc_data.vbus > STATEGY_CHARGE_VBUS_ABNORMAL_UV) {
			mca_log_err("9V buck charging abnormal, set to 5v\n");
			mca_vote(info->input_voltage_voter, "recover_force_5v",
				 true, 0);
		}
		strategy_class_buckchg_ops_set_chg(info, false);
		strategy_class_buckchg_ops_set_chg(info, true);
		mca_log_err("recover buck charging\n");
	}

	strategy_buckchg_charge_abnormal_cold_or_hot_zone(info);

	if (info->proc_data.chg_status == MCA_BUCK_CHG_STS_CHARGING &&
	    !info->sw_cv_running && vterm >= SW_CV_VTERM_MIN) {
		if (info->proc_data.vbat >= vterm - SW_CV_START_DELTA) {
			mca_log_err("vbat: %d, vterm: %d, start sw_cv_work\n",
				    info->proc_data.vbat, vterm);
			info->sw_cv_running = true;
			mca_vote(info->charge_limit_voter, "sw_cv", false, 0);
			mca_queue_delayed_work(info->support_base_flip ||
					       info->base_flip_same ?
					       &info->base_flip_sw_cv_work :
					       &info->sw_cv_work,
					       msecs_to_jiffies(CHARGE_SW_CV_WORK_FAST_INTERVAL));
		}
	} else if (info->sw_cv_running &&
		   (!chg_en ||
		    info->proc_data.vbat <= vterm - SW_CV_STOP_DELTA)) {
		mca_log_err("vbat: %d, vterm: %d, chg_en: %d, stop sw_cv_work\n",
			    info->proc_data.vbat, vterm, chg_en);
		strategy_buckchg_sw_cv_stop(info);
	}

	if (quick_charge_status == MCA_QUICK_CHG_STS_CHARGE_DONE)
		interval = CHARGE_MONITOR_WORK_FAST_INTERVAL;

out:
	queue_delayed_work(system_wq, &info->monitor_work,
			   msecs_to_jiffies(interval));
}

static int sw_cv_volt_delta(const struct sw_cv_volt_delta *map, int size,
			    int ibat_ma, int def)
{
	int i;

	for (i = 0; i < size; i++) {
		if (ibat_ma >= map[i].ibat_ma)
			return map[i].delta_mv;
	}

	return def;
}

/*
 * The software constant-voltage phase.  Near the float voltage the charge
 * current is stepped down to what the battery is actually taking, so the
 * charger's own constant-voltage loop has less to do; if the battery
 * overshoots anyway, the charger's float voltage is pulled back directly.
 */
static void strategy_buckchg_sw_cv_workfunc(struct work_struct *work)
{
	struct strategy_buckchg_dev *info =
		container_of(to_delayed_work(work), struct strategy_buckchg_dev,
			     sw_cv_work);
	int vterm = mca_get_effective_result(info->vterm_voter);
	int iterm = mca_get_effective_result(info->iterm_voter);
	int fcc = mca_get_effective_result(info->charge_limit_voter);
	int fv_comp = info->pmic_fv_compensation;
	int interval, vbat, ibat_ua, ibat_ma, step, delta, target;
	int temp = 0, old_count;
	int fastcharge;

	strategy_class_fg_ops_get_voltage(&info->proc_data.vbat);
	strategy_class_fg_ops_get_current(&info->proc_data.ibat);
	vbat = info->proc_data.vbat;
	ibat_ua = -info->proc_data.ibat;
	ibat_ma = ibat_ua / 1000;

	mca_log_info("vbat: %d, ibat: %d, vterm: %d, iterm: %d, fcc: %d\n",
		     vbat, ibat_ma, vterm, iterm, fcc);

	delta = sw_cv_volt_delta(sw_cv_volt_delta_map,
				 ARRAY_SIZE(sw_cv_volt_delta_map), ibat_ma,
				 SW_CV_VOLT_DELTA_DEFAULT);
	if (vbat < vterm - delta) {
		interval = CHARGE_SW_CV_WORK_NORMAL_INTERVAL;
	} else {
		step = info->sw_cv_fcc_step;
		if (ibat_ma - step > iterm) {
			if (fcc - ibat_ma < step * 2)
				target = fcc - step;
			else
				target = step ? (ibat_ma / step) * step : 0;
			mca_vote(info->charge_limit_voter, "sw_cv", true, target);
		}
		interval = CHARGE_SW_CV_WORK_FAST_INTERVAL;
	}

	if (info->support_pmic_vterm_dynamics_adjust) {
		fastcharge = strategy_class_fg_get_fastcharge();
		strategy_class_fg_ops_get_temperature(&temp);
		if (fastcharge && temp >= 180 && temp <= 299)
			fv_comp = info->pmic_fv_compensation;
		else if (fastcharge && temp >= 300 && temp <= 399)
			fv_comp = info->pmic_middle_fv_compensation;
		else if (fastcharge && temp >= 400 && temp <= 479)
			fv_comp = info->pmic_high_fv_compensation;
		else
			fv_comp = 0;
	}

	if (vbat >= vterm - 1) {
		mca_log_err("WARNING: batt ov, reduce fv, count: %d\n",
			    info->vbat_ov_count);
		old_count = info->vbat_ov_count++;
		if (!old_count)
			platform_class_buckchg_ops_set_term_volt(MAIN_BUCK_CHARGER,
				fv_comp + vterm - info->sw_cv_fv_step);
		else
			platform_class_buckchg_ops_set_term_volt(MAIN_BUCK_CHARGER,
				fv_comp + vterm - 2 * info->sw_cv_fv_step);
	}

	mca_queue_delayed_work(&info->sw_cv_work, msecs_to_jiffies(interval));
}

/* The same for a foldable, against jeita's own float voltage. */
static void strategy_buckchg_base_flip_sw_cv_workfunc(struct work_struct *work)
{
	struct strategy_buckchg_dev *info =
		container_of(to_delayed_work(work), struct strategy_buckchg_dev,
			     base_flip_sw_cv_work);
	int jeita_vterm = mca_get_client_vote(info->vterm_voter, "jeita");
	int iterm = info->parallel_iterm;
	int fcc = mca_get_effective_result(info->charge_limit_voter);
	int first_termination = 0;
	int vbat, ibat_ua, ibat_ma, fastcharge;
	int vterm, actual_vterm, fv_step, ov_margin, delta, count;
	int interval = CHARGE_SW_CV_WORK_NORMAL_INTERVAL;

	strategy_class_fg_ops_get_voltage(&info->proc_data.vbat);
	strategy_class_fg_ops_get_current(&info->proc_data.ibat);
	fastcharge = strategy_class_fg_get_fastcharge();
	vbat = info->proc_data.vbat;
	ibat_ua = -info->proc_data.ibat;
	ibat_ma = ibat_ua / 1000;

	strategy_class_fg_get_first_termination(&first_termination);
	mca_log_info("first_termination_flag:%d", first_termination);

	vterm = first_termination ?
		mca_get_effective_result(info->vterm_voter) : jeita_vterm;
	fv_step = fastcharge ? 10 : 5;
	actual_vterm = info->pmic_fv_compensation + vterm;
	ov_margin = fastcharge ? 5 : 3;

	mca_log_info("vbat: %d, ibat: %d, vterm: %d, actual_vterm: %d, iterm: %d, fcc: %d, fast_charge: %d, fv_step: %d\n",
		     vbat, ibat_ma, jeita_vterm, actual_vterm, iterm, fcc,
		     !!fastcharge, fv_step);

	delta = sw_cv_volt_delta(base_flip_sw_cv_volt_delta_map,
				 ARRAY_SIZE(base_flip_sw_cv_volt_delta_map),
				 ibat_ma, BASE_FLIP_SW_CV_VOLT_DELTA_DEFAULT);
	if (fastcharge && vbat >= jeita_vterm - delta) {
		interval = CHARGE_SW_CV_WORK_FAST_INTERVAL;
		if (iterm < ibat_ma - 50 && fcc - ibat_ma >= 100)
			mca_vote(info->charge_limit_voter, "sw_cv", true,
				 (ibat_ua / 50000) * 50);
	}

	if (vbat >= jeita_vterm - ov_margin) {
		mca_log_err("WARNING: batt ov, reduce fv, count: %d\n",
			    info->vbat_ov_count);
		count = min(++info->vbat_ov_count, 5);
		platform_class_buckchg_ops_set_term_volt(MAIN_BUCK_CHARGER,
				actual_vterm - count * fv_step);
	}

	mca_queue_delayed_work(&info->base_flip_sw_cv_work,
			       msecs_to_jiffies(interval));
}

/* Sleep in 10 ms steps while the session still wants the bus. */
static int strategy_buckchg_wireless_revchg_msleep(int ms,
						   struct strategy_buckchg_dev *info)
{
	int i;

	for (i = 0; i < ms / 10; i++) {
		if (!info->proc_data.online || !info->wls_revchg_en)
			return -1;
		usleep_range(9900, 11000);
	}

	return 0;
}

/*
 * Bring the adapter up to the voltage a wireless receiver is charged from
 * and tell the wireless side once it is there.  A DCP is given time to turn
 * out to be something better first.
 */
static int strategy_buckchg_process_wireless_revchg(struct strategy_buckchg_dev *info)
{
	int real_type = info->proc_data.real_type;
	int vbus = 0, req_vadp, valid_h, valid_l, idx, i;

	if (real_type < XM_CHARGER_TYPE_DCP ||
	    real_type > XM_CHARGER_TYPE_PD_VERIFY)
		return -1;

	if (real_type == XM_CHARGER_TYPE_DCP) {
		if (strategy_buckchg_wireless_revchg_msleep(WLS_REVCHG_DCP_WAIT_MS,
							    info))
			return -1;
	} else if (real_type == XM_CHARGER_TYPE_HVDCP3) {
		if (strategy_buckchg_wireless_revchg_msleep(WLS_REVCHG_HVDCP3_WAIT_MS,
							    info))
			return -1;
	} else if (real_type == XM_CHARGER_TYPE_PD) {
		if (strategy_buckchg_wireless_revchg_msleep(WLS_REVCHG_PD_WAIT_MS,
							    info))
			return -1;
	}

	real_type = info->proc_data.real_type;
	if (real_type == XM_CHARGER_TYPE_DCP) {
		mca_log_info("still DCP after sleeping, abort wls revchg\n");
		return -1;
	}

	mca_vote(info->input_limit_voter, "wireless_revchg", true,
		 CHARGE_WLS_REVCHG_INPUT_DEFAULT);

	idx = (real_type == XM_CHARGER_TYPE_PPS ||
	       real_type == XM_CHARGER_TYPE_PD_VERIFY) ?
	      REV_USBIN_TYPE_PPS : REV_USBIN_TYPE_OTHER;
	req_vadp = info->rev_req_vadp[idx];
	valid_h = info->rev_vadp_valid_h[idx] * 1000;
	valid_l = info->rev_vadp_valid_l[idx] * 1000;

	strategy_buckchg_set_charge_volt(info, req_vadp);
	if (strategy_buckchg_wireless_revchg_msleep(WLS_REVCHG_VBUS_SETTLE_MS,
						    info))
		return -1;
	platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER, &vbus);
	mca_log_info("chg_type: %d, req_vadp: %d, vbus: %d, rev_vadp_valid_h: %d, rev_vadp_valid_l: %d\n",
		     real_type, req_vadp, vbus, valid_h, valid_l);

	for (i = 0; i < WLS_REVCHG_VBUS_RETRY; i++) {
		if (vbus >= valid_l && vbus <= valid_h)
			goto vbus_ok;
		if (strategy_buckchg_wireless_revchg_msleep(WLS_REVCHG_VBUS_SETTLE_MS,
							    info))
			return -1;
		platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER, &vbus);
	}
	mca_log_err("request 9V vbus fail\n");
	return -1;

vbus_ok:
	info->proc_data.wls_revchg_init_done = true;

	return mca_wireless_rev_set_wired_chg_ok(true);
}

static void strategy_wls_revchg_monitor_workfunc(struct work_struct *work)
{
	struct strategy_buckchg_dev *info =
		container_of(to_delayed_work(work), struct strategy_buckchg_dev,
			     wls_revchg_monitor_work);

	if (!info->proc_data.wls_revchg_init_done &&
	    strategy_buckchg_process_wireless_revchg(info))
		goto exit;

	if (!info->wls_revchg_en)
		goto exit;

	/* A QC2 adapter is given a lower input. */
	if (info->proc_data.real_type == XM_CHARGER_TYPE_HVDCP2 &&
	    !info->rev_icl_for_qc2) {
		mca_vote(info->input_limit_voter, "wireless_revchg", true,
			 CHARGE_WLS_REVCHG_INPUT_QC2);
		info->rev_icl_for_qc2 = true;
	}
	queue_delayed_work(system_wq, &info->wls_revchg_monitor_work,
			   msecs_to_jiffies(WLS_REVCHG_MONITOR_INTERVAL));
	return;

exit:
	mca_vote(info->input_limit_voter, "wireless_revchg", false, 0);
	info->proc_data.wls_revchg_init_done = false;
	info->rev_icl_for_qc2 = false;
}

static int strategy_buckchg_process_event(int event, int value, void *data)
{
	struct strategy_buckchg_dev *info = data;
	bool otg_present = false;

	if (!info)
		return -1;

	mca_log_info("receive event %d, value %d\n", event, value);

	switch (event) {
	case MCA_EVENT_USB_DISCONNECT:
	case MCA_EVENT_USB_CONNECT:
		strategy_buckchg_process_online_change(value, info);
		break;
	case MCA_EVENT_WIRELESS_DISCONNECT:
	case MCA_EVENT_WIRELESS_CONNECT:
	case MCA_EVENT_WIRELESS_REVCHG:
		if (event == MCA_EVENT_WIRELESS_REVCHG)
			info->wls_revchg_online = !!value;
		else
			info->wls_online = !!value;
		/* Reverse quick charge has to give way to wireless. */
		protocol_class_pd_get_otg_plugin_status(TYPEC_PORT_0,
							&otg_present);
		if (!info->support_reverse_quick_charge || !otg_present)
			break;
		if (info->start_quick_revchg) {
			cancel_delayed_work_sync(&info->source_status_monitor_work);
			queue_delayed_work(system_wq,
					   &info->source_status_monitor_work, 0);
		}
		break;
	case MCA_EVENT_CHARGE_TYPE_CHANGE:
		strategy_buckchg_process_type_change(value, info);
		break;
	case MCA_EVENT_CHARGE_CAP_CHANGE:
		strategy_buckchg_process_cap_change(info);
		break;
	case MCA_EVENT_CHARGE_VERIFY_PROCESS_END:
		mca_log_info("receive pd verify process end event, value %d\n",
			     value);
		info->verify_process_end = value;
		if (value)
			mca_vote(info->input_limit_voter, "icl_limit", false,
				 PD_UNVERIFIED_ICL);
		break;
	case MCA_EVENT_WIRELESS_USB_REVCHG:
		mca_log_info("wireless revchg event %d\n", value);
		info->wls_revchg_en = value;
		if (value) {
			strategy_wls_revchg_monitor_workfunc(&info->wls_revchg_monitor_work.work);
			break;
		}
		cancel_delayed_work_sync(&info->wls_revchg_monitor_work);
		mca_vote(info->input_limit_voter, "wireless_revchg", false, 0);
		info->proc_data.wls_revchg_init_done = false;
		info->rev_icl_for_qc2 = false;
		break;
	case MCA_EVENT_BATTERY_TOTAL_ITERM:
		mca_log_info("parallel_iterm: %d\n", value);
		info->parallel_iterm = value;
		break;
	case MCA_EVENT_CP_CBOOT_FAIL:
		if (!value) {
			mca_vote(info->input_voltage_voter, "cp_cboot_short",
				 false, 1);
			break;
		}
		mca_vote(info->input_voltage_voter, "cp_cboot_short", true, 0);
		platform_class_buckchg_ops_get_online(MAIN_BUCK_CHARGER,
						      &info->proc_data.online);
		if (info->proc_data.online)
			strategy_buckchg_start_charging(info);
		break;
	case MCA_EVENT_CONN_ANTIBURN_CHANGE:
		strategy_buckchg_process_antiburn_change(value, info);
		break;
	case MCA_EVENT_BATT_BTB_CHANGE:
		info->batt_miss = value;
		strategy_buckchg_process_batt_btb_change(value, info);
		break;
	case MCA_EVENT_BATT_AUTH_PASS:
		mca_log_err("receive batt_auth event, value %d", value);
		info->batt_auth = 1;
		mca_vote(info->input_voltage_voter, "batt_auth", false, 0);
		mca_vote(info->charge_limit_voter, "batt_auth", false,
			 info->chg_batt_auth_failed);
		if (info->proc_data.online)
			mod_delayed_work(system_wq, &info->monitor_work, 0);
		break;
	case MCA_EVENT_LPD_STATUS_CHANGE:
		mca_vote(info->input_voltage_voter, "lpd", !!value, 0);
		break;
	case MCA_EVENT_PMIC_INIT_DONE:
		strategy_buckchg_process_pmic_init_done(info);
		break;
	case MCA_EVENT_CC_SHORT_VBUS:
		mca_vote(info->input_voltage_voter, "cc_short_vbus", !!value,
			 !value);
		break;
	case MCA_EVENT_VBAT_OVP_CHANGE:
		mca_vote(info->chg_enable_voter, "vbat_ovp", !!value, !value);
		break;
	case MCA_EVENT_QUICK_REVCHG_CHANGE:
		if (info->support_reverse_quick_charge)
			strategy_buckchg_cp_revert_handler(value, info);
		break;
	case MCA_EVENT_CHARGE_ABNORMAL:
		strategy_buckchg_stop_charging(info);
		break;
	case MCA_EVENT_CHARGE_RESTORE:
		platform_class_buckchg_ops_get_online(MAIN_BUCK_CHARGER,
						      &info->proc_data.online);
		if (info->proc_data.online)
			strategy_buckchg_start_charging(info);
		break;
	case MCA_EVENT_CSD_SEND_PULSE:
		strategy_buckchg_process_csd_pulse(value, info);
		break;
	case MCA_EVENT_PPS_PTF:
		info->pps_ptf = value;
		mca_log_info("set PPS_PTF %d\n", value);
		if (info->pps_ptf == USBPD_BUCK_DPM_PORT_PPS_PTF_NOT_OVERTEMP)
			mca_vote(info->input_voltage_voter, "ptf", true, 1);
		break;
	case MCA_EVENT_IS_EU_MODEL:
		info->is_eu_model = !!value;
		mca_log_err("set buck is_eu_model %d\n", value);
		platform_class_buckchg_ops_set_eu_model(MAIN_BUCK_CHARGER,
							!!value);
		break;
	case MCA_EVENT_PLATE_SHOCK:
		mca_vote(info->input_limit_voter, "plate_shock", !!value,
			 value ? STAEGY_CHARGE_PLATE_SHOCK : 0);
		break;
	case MCA_EVENT_START_QUICK_REVCHG:
		info->start_quick_revchg = true;
		if (info->support_reverse_quick_charge)
			queue_delayed_work(system_wq,
					   &info->source_status_monitor_work, 0);
		break;
	case MCA_EVENT_REVCHG_BCL:
		info->revchg_bcl = !!value;
		break;
	case MCA_EVENT_HANDLE_ALLOW_CHARGE:
		mca_log_info("handle_allow_charge: %d\n", value);
		mca_vote(info->input_limit_voter, "handle", !value, 0);
		mca_vote(info->chg_enable_voter, "handle", !value, 0);
		break;
	case MCA_EVENT_BUCKCHG_BATT_OV:
		if (!info->sw_cv_running)
			mod_delayed_work(system_wq, &info->monitor_work, 0);
		break;
	case MCA_EVENT_DEBUG_CTRL_MEMORY_TEST:
		mca_log_info("memory_test enable soc_limit: %d\n", value);
		if (value) {
			info->soc_limit_low = 1;
			info->soc_limit_high = 3;
		} else {
			info->soc_limit_low = 0;
			info->soc_limit_high = 0;
		}
		break;
	case MCA_EVENT_DEBUG_CTRL_SOC_LIMIT:
		info->soc_limit_low = value >> 8;
		info->soc_limit_high = value & 0xff;
		mca_log_info("debug_ctrl set soc_limit: %d %d\n",
			     info->soc_limit_low, info->soc_limit_high);
		break;
	default:
		break;
	}

	return 0;
}

static int strategy_buckchg_get_current_max(struct strategy_buckchg_dev *info)
{
	int cap_curr;
	int ibus_limit;

	switch (info->proc_data.real_type) {
	case XM_CHARGER_TYPE_UNKNOW:
		ibus_limit = 100;
		break;
	case XM_CHARGER_TYPE_SDP:
		ibus_limit = info->is_eu_model ? info->in_sdp - 50 : info->in_sdp;
		break;
	case XM_CHARGER_TYPE_CDP:
		ibus_limit = info->in_cdp;
		break;
	case XM_CHARGER_TYPE_DCP:
		ibus_limit = info->pwr_boost_state ? CHARGE_DCP_INPUT_BOOST :
						     info->in_dcp;
		break;
	case XM_CHARGER_TYPE_FLOAT:
	case XM_CHARGER_TYPE_OCP:
		ibus_limit = info->in_float;
		break;
	case XM_CHARGER_TYPE_HVDCP2:
		ibus_limit = info->in_hvdcp;
		break;
	case XM_CHARGER_TYPE_HVDCP3:
	case XM_CHARGER_TYPE_HVDCP3_B:
		ibus_limit = info->in_hvdcp3;
		break;
	case XM_CHARGER_TYPE_HVDCP3P5:
		ibus_limit = info->in_hvdcp3p5;
		break;
	case XM_CHARGER_TYPE_TYPEC:
		ibus_limit = info->in_dcp;
		break;
	case XM_CHARGER_TYPE_PD:
	case XM_CHARGER_TYPE_PPS:
	case XM_CHARGER_TYPE_PD_VERIFY:
		if (info->pps_ptf == USBPD_BUCK_DPM_PORT_PPS_PTF_NOT_OVERTEMP) {
			ibus_limit = CHARGE_PPS_PTF_INPUT_DEFAULT;
			break;
		}
		cap_curr = info->pwr_cap.cap[info->proc_data.curr_pd_pos].max_current;
		ibus_limit = min_t(unsigned int, info->in_pd, cap_curr);
		if (!cap_curr) {
			mca_log_info("pdo broadcast abnormal %d \n",
				     info->proc_data.curr_pd_pos);
			ibus_limit = info->in_pd;
		}
		break;
	default:
		ibus_limit = info->in_sdp;
		break;
	}

	mca_log_info("ibus_limit = %d\n", ibus_limit);

	return ibus_limit;
}

static int strategy_buckchg_get_status(int status, int *value, void *data)
{
	struct strategy_buckchg_dev *info = data;

	if (!value || !info)
		return -1;

	switch (status) {
	case STRATEGY_STATUS_TYPE_ONLINE:
		platform_class_buckchg_ops_get_online(MAIN_BUCK_CHARGER, value);
		break;
	case STRATEGY_STATUS_TYPE_INIT_OK:
		*value = info->init_ok;
		break;
	case STRATEGY_STATUS_TYPE_QC_TYPE:
		*value = info->proc_data.qc_type;
		break;
	case STRATEGY_STATUS_TYPE_ENABLE:
		*value = mca_get_effective_result(info->chg_enable_voter);
		break;
	case STRATEGY_STATUS_TYPE_VBUS:
		*value = info->proc_data.vbus;
		break;
	case STRATEGY_STATUS_TYPE_IBUS:
		*value = info->proc_data.ibus;
		break;
	case STRATEGY_STATUS_TYPE_GET_VOLTAGE_MAX:
		*value = info->proc_data.voltage;
		break;
	case STRATEGY_STATUS_TYPE_GET_CURRENT_MAX:
		*value = strategy_buckchg_get_current_max(info);
		break;
	default:
		return -1;
	}

	return 0;
}

/*
 * On an EU unit outside charger-only boot, the USB input current limit HLOS
 * sets is honoured: 2 mA suspends the input, -1 mA resumes it, and the USB
 * standard currents are applied to an SDP (or resume a PD adapter).
 */
static int stategy_buckchg_set_usb_icl(int value,
				       struct strategy_buckchg_dev *info)
{
	static bool usb_suspend;
	int boot_mode = mca_log_get_charge_boot_mode();
	int real_type = info->proc_data.real_type;
	int icl_ma;

	if (boot_mode || !info->is_eu_model) {
		mca_log_info("charge_boot_mode[%d] is_eu_model[%d] exit usb_icl",
			     boot_mode, info->is_eu_model);
		return 0;
	}

	icl_ma = value / 1000;
	mca_log_info("usb type[%d]icl_ma[%d]", real_type, icl_ma);

	if (real_type == XM_CHARGER_TYPE_PD || real_type == XM_CHARGER_TYPE_PPS) {
		if (!info->proc_data.pdsuspendsupported) {
			mca_log_info("USB suspend is not supported");
		} else if (icl_ma == USB_ICL_SUSPEND_MA) {
			mca_vote(info->input_limit_voter, "usbicl", true, 0);
			mca_log_info("USB suspend for pd");
		} else if (icl_ma == USB_ICL_100_MA || icl_ma == USB_ICL_900_MA ||
			   icl_ma == USB_ICL_500_MA) {
			mca_vote(info->input_limit_voter, "usbicl", false, 0);
			mca_log_info("USB Unsuspend for pd");
		} else {
			mca_log_info("invalid ICL value for pd %d", icl_ma);
		}
		return 0;
	}

	if (real_type != XM_CHARGER_TYPE_SDP) {
		mca_log_info("ICL setting is not allowed for usb type[%d]",
			     real_type);
		return 0;
	}

	if (icl_ma == USB_ICL_SUSPEND_MA) {
		mca_log_info("USB Input Suspended");
		mca_vote(info->input_limit_voter, "usbicl", true, 0);
		usb_suspend = true;
	} else {
		if (icl_ma == USB_ICL_UNSUSPEND_MA) {
			mca_log_info("USB Input UnSuspended");
			mca_vote(info->input_limit_voter, "usbicl", false, 0);
		} else if (icl_ma == USB_ICL_900_MA || icl_ma == USB_ICL_500_MA) {
			mca_vote(info->input_limit_voter, "sdpicl", true, icl_ma);
			mca_vote(info->input_limit_voter, "usbicl", true, icl_ma);
			mca_log_info("ICL for SDP set by HLOS is %d mA", icl_ma);
		} else {
			mca_log_info("Invalid ICL for SDP set by HLOS is %d mA",
				     icl_ma);
		}
		if (!usb_suspend)
			return 0;
		usb_suspend = false;
	}

	mca_event_block_notify(MCA_EVENT_TYPE_CHARGER_CONNECT,
			       MCA_EVENT_USB_SUSPEND, &usb_suspend);

	return 0;
}

static int strategy_buckchg_set_config(int config, int value, void *data)
{
	struct strategy_buckchg_dev *info = data;

	if (!info)
		return -1;

	if (config == STRATEGY_CONFIG_INPUT_CURRENT_LIMIT)
		stategy_buckchg_set_usb_icl(value, info);

	return 0;
}

static int strategy_buckchg_thermal_notifier_cb(struct notifier_block *nb,
						unsigned long event, void *data)
{
	struct strategy_buckchg_dev *info =
		container_of(nb, struct strategy_buckchg_dev, thermal_board_nb);

	if (event == MCA_EVENT_THERMAL_BOARD_TEMP_CHANGE)
		info->thermal_board_temp = *(int *)data / 100;

	return 0;
}

static int strategy_buckchg_panel_notifier_cb(struct notifier_block *nb,
					      unsigned long event, void *data)
{
	struct strategy_buckchg_dev *info =
		container_of(nb, struct strategy_buckchg_dev, panel_nb);
	int screen_state;

	if (event == MCA_EVENT_PANEL_SCREEN_STATE_CHANGE) {
		screen_state = *(int *)data;
		mca_log_info("update screen_state: %d => %d\n",
			     info->screen_status, screen_state);
		info->screen_status = !!screen_state;
	}

	return 0;
}

/* The charge interface: userspace and the rest of the stack, by name. */
static int strategy_buckchg_if_set_input_suspend(const char *user, char *value,
						 void *data)
{
	struct strategy_buckchg_dev *info = data;
	int val = 0;

	if (!user || !value || !info)
		return -1;

	if (kstrtoint(value, 0, &val))
		return -1;

	mca_log_err("set_input_suspend: %d\n", val);
	if (!val && info->support_pmic_vterm_dynamics_adjust) {
		mca_vote_override(info->vterm_voter, "pmic_temp_term", false, 0);
		mca_vote_override(info->input_limit_voter, "pmic_temp_term",
				  false, 0);
	}
	mca_vote(info->input_suppend_voter, user, true, !!val);

	return 0;
}

static int strategy_buckchg_if_get_eff(struct mca_votable *votable, char *buf)
{
	const char *client = mca_get_effective_client(votable);

	if (!client)
		return -1;

	scnprintf(buf, MCA_CHARGE_IF_MAX_VALUE_BUFF, "eff_client:%s %d", client,
		  mca_get_effective_result(votable));

	return 0;
}

static int strategy_buckchg_if_get_input_suspend(char *buf, void *data)
{
	struct strategy_buckchg_dev *info = data;

	if (!buf || !info)
		return -1;

	return strategy_buckchg_if_get_eff(info->input_suppend_voter, buf);
}

static int strategy_buckchg_if_set_chg_en(const char *user, u32 value,
					  void *data)
{
	struct strategy_buckchg_dev *info = data;

	if (!user || !info)
		return -1;

	if (value && info->support_pmic_vterm_dynamics_adjust) {
		mca_vote_override(info->vterm_voter, "pmic_temp_term", false, 0);
		mca_vote_override(info->input_limit_voter, "pmic_temp_term",
				  false, 0);
	}
	mca_vote(info->chg_enable_voter, user, true, !!value);

	return 0;
}

static int strategy_buckchg_if_get_chg_en(char *buf, void *data)
{
	struct strategy_buckchg_dev *info = data;

	if (!buf || !info)
		return -1;

	return strategy_buckchg_if_get_eff(info->chg_enable_voter, buf);
}

/* An MTBF test overrides the input limit rather than voting on it. */
static int strategy_buckchg_if_set_input_cur(const char *user, char *value,
					     void *data)
{
	struct strategy_buckchg_dev *info = data;
	bool mtbf;
	int val = 0;

	if (!user || !value || !info)
		return -1;

	if (kstrtoint(value, 0, &val))
		return -1;

	mtbf = strstr(user, "MTBF") || strstr(user, "mtbf");
	if (mtbf)
		mca_vote_override(info->input_limit_voter, user, !!val, val);
	else
		mca_vote(info->input_limit_voter, user, !!val, val);

	return 0;
}

static int strategy_buckchg_if_get_input_cur(char *buf, void *data)
{
	struct strategy_buckchg_dev *info = data;

	if (!buf || !info)
		return -1;

	return strategy_buckchg_if_get_eff(info->input_limit_voter, buf);
}

static int strategy_buckchg_if_set_chg_cur(const char *user, char *value,
					   void *data)
{
	struct strategy_buckchg_dev *info = data;
	int val = 0;

	if (!user || !value || !info)
		return -1;

	if (kstrtoint(value, 0, &val))
		return -1;

	mca_vote(info->charge_limit_voter, user, !!val, val);

	return 0;
}

static int strategy_buckchg_if_get_chg_cur(char *buf, void *data)
{
	struct strategy_buckchg_dev *info = data;

	if (!buf || !info)
		return -1;

	return strategy_buckchg_if_get_eff(info->charge_limit_voter, buf);
}

static int strategy_buckchg_if_set_ship_mode(const char *user, u32 value,
					     void *data)
{
	int ret = platform_class_buckchg_ops_set_ship_mode(MAIN_BUCK_CHARGER,
							   !!value);

	return ret < 0 ? ret : 0;
}

static int strategy_buckchg_if_get_ship_mode(bool *status, void *data)
{
	int ret = platform_class_buckchg_ops_get_ship_mode(MAIN_BUCK_CHARGER,
							   status);

	return ret < 0 ? ret : 0;
}

static struct mca_charge_if_ops g_strategy_buckchg_if_ops = {
	.type_name			= "buck",
	.set_input_suspend		= strategy_buckchg_if_set_input_suspend,
	.get_input_suspend		= strategy_buckchg_if_get_input_suspend,
	.set_charge_enable		= strategy_buckchg_if_set_chg_en,
	.get_charge_enable		= strategy_buckchg_if_get_chg_en,
	.set_input_current_limit	= strategy_buckchg_if_set_input_cur,
	.get_input_current_limit	= strategy_buckchg_if_get_input_cur,
	.set_charge_current_limit	= strategy_buckchg_if_set_chg_cur,
	.get_charge_current_limit	= strategy_buckchg_if_get_chg_cur,
	.set_ship_mode_en		= strategy_buckchg_if_set_ship_mode,
	.get_ship_mode_status		= strategy_buckchg_if_get_ship_mode,
};

/* Smart charging. */
static int strategy_buckchg_smartchg_set_fcc_callback(void *data, int fcc)
{
	struct strategy_buckchg_dev *info = data;

	if (!info)
		return -1;

	mca_vote(info->charge_limit_voter, "smartchg", !!fcc, fcc);

	return 0;
}

static int strategy_buckchg_soc_limit_sts_callback(void *data,
						   int effective_result)
{
	struct strategy_buckchg_dev *info = data;

	if (!info)
		return -1;

	info->soc_limit_sts = effective_result;
	mca_log_info("effective_result: %d\n", effective_result);

	if (info->support_charge_more) {
		if (!effective_result || !info->proc_data.online) {
			cancel_delayed_work_sync(&info->soc_limit_stepper_work);
			effective_result = 0;
		}
		strategy_buckchg_process_soc_limit_change_more(effective_result,
							       info);
		return 0;
	}

	if (info->proc_data.online && effective_result) {
		mca_vote(info->chg_enable_voter, "soc_limit", true, 0);
		mca_vote(info->input_voltage_voter, "soc_limit", true, 0);
		mca_log_info("SOC limit triggered, stopping charge\n");
		mca_vote(info->charge_limit_voter, "csd_pulse", false, 0);
		return 0;
	}

	mca_vote(info->chg_enable_voter, "soc_limit", false, 1);
	mca_vote(info->input_voltage_voter, "soc_limit", false, 0);
	mca_log_info("SOC limit released, starting charge\n");
	if (info->proc_data.online)
		mod_delayed_work(system_wq, &info->monitor_work, 0);

	return 0;
}

static int mca_charger_buckchg_pwr_boost_sts_callback(void *data, int enable)
{
	struct strategy_buckchg_dev *info = data;

	if (!info)
		return -1;

	info->pwr_boost_state = enable;

	return 0;
}

static struct mca_smartchg_if_ops g_buck_smartchg_if_ops = {
	.type			= MCA_SMARTCHG_IF_CHG_TYPE_BUCK,
	.set_fcc		= strategy_buckchg_smartchg_set_fcc_callback,
	.set_soc_limit_sts	= strategy_buckchg_soc_limit_sts_callback,
	.set_pwr_boost_sts	= mca_charger_buckchg_pwr_boost_sts_callback,
};

static int strategy_buckchg_class_probe(struct platform_device *pdev)
{
	struct strategy_buckchg_dev *info;
	int online = 0;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = &pdev->dev;
	platform_set_drvdata(pdev, info);
	strategy_buckchg_parse_dt(info);

	if (strategy_buckchg_init_voter(info)) {
		mca_log_err("init voter err\n");
		return -1;
	}

	/* No battery and no authentication until told otherwise. */
	mca_vote(info->input_voltage_voter, "batt_auth", true, 0);
	mca_vote(info->charge_limit_voter, "batt_auth", true,
		 info->chg_batt_auth_failed);
	mca_vote(info->vterm_voter, "batt_miss", true, STAEGY_BATT_MISS_FV);

	info->proc_data.voltage = STATEGY_CHARGE_VBUS_5V;
	info->proc_data.pre_volt = STATEGY_CHARGE_VBUS_5V;
	info->cp_vendor = -1;
	info->batt_miss = 1;

	INIT_DELAYED_WORK(&info->monitor_work, strategy_buckchg_monitor_workfunc);
	INIT_DELAYED_WORK(&info->sw_cv_work, strategy_buckchg_sw_cv_workfunc);
	INIT_DELAYED_WORK(&info->base_flip_sw_cv_work,
			  strategy_buckchg_base_flip_sw_cv_workfunc);
	INIT_DELAYED_WORK(&info->wls_revchg_monitor_work,
			  strategy_wls_revchg_monitor_workfunc);
	INIT_DELAYED_WORK(&info->csd_pulse_process_work,
			  strategy_csd_pulse_process_workfunc);
	INIT_DELAYED_WORK(&info->source_status_monitor_work,
			  strategy_source_status_monitor_workfunc);
	INIT_DELAYED_WORK(&info->check_pd_secret_work,
			  strategy_buckchg_check_pdsecret_workfunc);
	INIT_DELAYED_WORK(&info->rerun_handle_pd_auth_work,
			  strategy_rerun_handle_pd_auth_workfunc);
	INIT_DELAYED_WORK(&info->soc_limit_stepper_work,
			  strategy_buckchg_soc_limit_stepper_workfunc);

	mca_strategy_ops_register(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				  strategy_buckchg_process_event,
				  strategy_buckchg_get_status,
				  strategy_buckchg_set_config, info);

	g_strategy_buckchg_if_ops.data = info;
	mca_charge_if_ops_register(&g_strategy_buckchg_if_ops);
	g_buck_smartchg_if_ops.data = info;
	mca_smartchg_if_ops_register(&g_buck_smartchg_if_ops);

	info->thermal_board_nb.notifier_call = strategy_buckchg_thermal_notifier_cb;
	mca_event_block_notify_register(MCA_EVENT_TYPE_THERMAL_TEMP,
					&info->thermal_board_nb);
	info->panel_nb.notifier_call = strategy_buckchg_panel_notifier_cb;
	mca_event_block_notify_register(MCA_EVENT_TYPE_PANEL, &info->panel_nb);

	/* A cable attached before this probed sent its event to nobody. */
	platform_class_buckchg_ops_get_online(MAIN_BUCK_CHARGER, &online);
	if (online) {
		mca_log_info("avoid missing first usb connect event\n");
		mca_event_block_notify(MCA_EVENT_TYPE_CHARGER_CONNECT,
				       MCA_EVENT_USB_CONNECT, NULL);
	}

	g_buckchg_info = info;
	mca_log_err("androidboot.mode=%d\n", mca_log_get_charge_boot_mode());
	info->init_ok = 1;
	mca_log_err("probe success\n");

	return 0;
}

static int strategy_buckchg_class_remove(struct platform_device *pdev)
{
	struct strategy_buckchg_dev *info = platform_get_drvdata(pdev);

	/* Nothing may keep a pointer to the state devm is about to free. */
	mca_strategy_ops_register(STRATEGY_FUNC_TYPE_BUCK_CHARGE, NULL, NULL,
				  NULL, NULL);
	g_strategy_buckchg_if_ops.data = NULL;
	g_buck_smartchg_if_ops.data = NULL;
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_THERMAL_TEMP,
					  &info->thermal_board_nb);
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_PANEL,
					  &info->panel_nb);
	cancel_delayed_work_sync(&info->monitor_work);
	cancel_delayed_work_sync(&info->sw_cv_work);
	cancel_delayed_work_sync(&info->base_flip_sw_cv_work);
	cancel_delayed_work_sync(&info->wls_revchg_monitor_work);
	cancel_delayed_work_sync(&info->csd_pulse_process_work);
	cancel_delayed_work_sync(&info->source_status_monitor_work);
	cancel_delayed_work_sync(&info->check_pd_secret_work);
	cancel_delayed_work_sync(&info->rerun_handle_pd_auth_work);
	cancel_delayed_work_sync(&info->soc_limit_stepper_work);
	g_buckchg_info = NULL;

	return 0;
}

static void strategy_buckchg_class_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,strategy_buckchg" },
	{ }
};

static struct platform_driver strategy_buckchg_class_driver = {
	.driver = {
		.name		= "strategy_buckchg_class",
		.of_match_table	= match_table,
	},
	.probe		= strategy_buckchg_class_probe,
	.remove		= strategy_buckchg_class_remove,
	.shutdown	= strategy_buckchg_class_shutdown,
};
module_platform_driver(strategy_buckchg_class_driver);

MODULE_DESCRIPTION("strategy buckchg class");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
