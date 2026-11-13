/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mca_quick_charger.h
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
#ifndef __MCA_QUICK_CHARGE_H__
#define __MCA_QUICK_CHARGE_H__

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <mca/protocol/protocol_class.h>
#include <mca/strategy/strategy_class.h>
#include <mca/common/mca_voter.h>
#include <mca/platform/platform_cp_class.h>
#include <mca/platform/platform_fg_ic_ops.h>

#define MCA_QUICK_CHG_NAME_LEN				16

/* device tree defaults */
#define MCA_QUICK_CHG_MIN_VBAT_DEFAULT			3300
#define MCA_QUICK_CHG_MAX_VBAT_DEFAULT			4400
#define MCA_QUICK_CHG_RECHARGE_VBAT_DEFAULT		4300
#define MCA_QUICK_CHG_MAX_TDIE_DEFAULT			125
#define MCA_QUICK_CHG_MAX_TADP_DEFAULT			125
#define MCA_QUICK_CHG_CURR_TERM_RATIO_DEFAULT		110
#define MCA_QUICK_CHG_CURR_TERM_SECTION_DEFAULT		3000
#define MCA_QUICK_CHG_MAX_VBAT_THRESHOLD_DEFAULT	5000
#define MCA_QUICK_CHG_DIV1_VOLT_DELTA_DEFAULT		300
#define MCA_QUICK_CHG_DIV2_VOLT_DELTA_DEFAULT		300
#define MCA_QUICK_CHG_DIV4_VOLT_DELTA_DEFAULT		500
#define MCA_QUICK_CHG_DEFAULT_IBAT_DELTA		200
#define MCA_QUICK_CHG_SINGLE_DIV1_CURR_TH		5000
#define MCA_QUICK_CHG_SINGLE_DIV2_CURR_TH		12000
#define MCA_QUICK_CHG_SINGLE_DIV4_CURR_TH		14000
#define MCA_QUICK_CHG_SINGLE_DIV4_CURR_EN_BUCK		19000
#define MCA_QUICK_CHG_IBUS_TH_DEFAULT			1500
#define MCA_QUICK_CHG_AGAIN_IBUS_TH_DEFAULT		2800
#define MCA_QUICK_CHG_IBUS_INC_HYS_DEFAULT		200
#define MCA_QUICK_CHG_IBUS_DEC_HYS_DEFAULT		200
#define MCA_QUICK_CHG_OPEN_PATH_IBUS_TH			300
#define MCA_QUICK_CHG_QC3B_VBUS_LIMIT_DEFAULT		9500
#define MCA_QUICK_CHG_QC3B_IBUS_LIMIT_DEFAULT		2000
#define MCA_QUICK_CHG_QC3P5_VBUS_LIMIT_DEFAULT		10000
#define MCA_QUICK_CHG_QC3P5_IBUS_LIMIT_DEFAULT		2000
#define MCA_QUICK_CHG_QC3_IBAT_LIMIT_DEFAULT		4000
#define MCA_QUICK_CHG_QC_FV_LIMIT_DEFAULT		4440
#define MCA_QUICK_CHG_QC_TAPER_FCC_THR_DEFAULT		2500
#define MCA_QUICK_CHG_QC_TAPER_SOC_THR_DEFAULT		100
#define MCA_QUICK_CHG_QC_TAPER_HYS_QC3B			30
#define MCA_QUICK_CHG_QC_TAPER_HYS_QC3P5		20
#define MCA_QUICK_CHG_PPS_TAPER_HYS			10
#define MCA_QUICK_CHG_SWITCH_PMIC_TH			1500
#define MCA_QUICK_CHG_FV_HYS_DEFAULT			1
#define MCA_QUICK_CHG_MULT_TO_SINGLE_COUNT_DEFAULT	2
#define MCA_QUICK_CHG_USE_BUCK_FCC_TH_DEFAULT		16000
#define MCA_QUICK_CHG_THIRD_PPS_IBAT_LIMIT_DEFAULT	6000
#define MCA_QUICK_CHG_CUR_MAX_TH_DEFAULT_0		10000
#define MCA_QUICK_CHG_CUR_MAX_TH_DEFAULT_1		8000
#define MCA_QUICK_CHG_CUR_MAX_TH_DEFAULT_2		5000
#define MCA_QUICK_CHG_IBUS_TH_PMIH_DEFAULT_0		5000
#define MCA_QUICK_CHG_IBUS_TH_PMIH_DEFAULT_1		4200
#define MCA_QUICK_CHG_IBUS_TH_PMIH_DEFAULT_2		2700
#define MCA_QUICK_CHG_PMIH_FCC_DEFAULT_0		4000
#define MCA_QUICK_CHG_PMIH_FCC_DEFAULT_1		3000
#define MCA_QUICK_CHG_PMIH_FCC_DEFAULT_2		2000
#define MCA_QUICK_CHG_PMIH_FCC_DEFAULT_3		1000
#define MCA_QUICK_CHG_VBAT_THRESHOLD_DEFAULT		4480
#define MCA_QUICK_CHG_OCP_THRESHOLD_DEFAULT_0		9280000
#define MCA_QUICK_CHG_OCP_THRESHOLD_DEFAULT_1		3540000

/* adapter requests */
#define MCA_QUICK_CHG_ADP_DEFAULT_VOLT			5000
#define MCA_QUICK_CHG_ADP_FIXED_9V			9000
#define MCA_QUICK_CHG_OPEN_PATH_CURR			2000
#define MCA_QUICK_CHG_VBUS_OK_HIGH_TH			12000
#define MCA_QUICK_CHG_STOP_REQ_COUNT			10
#define MCA_QUICK_CHG_DEFAULT_VSTEP			20
#define MCA_QUICK_CHG_PPS_OPEN_PATH_VSTEP		40
#define MCA_QUICK_CHG_QC3B_DEFAULT_VSTEP		200
#define MCA_QUICK_CHG_MAX_VSTEP_DOWN			100
#define MCA_QUICK_CHG_TUNE_VBUS_COUNT			15
#define MCA_QUICK_CHG_TUNE_VBUS_LOW_MARGIN		50
#define MCA_QUICK_CHG_TUNE_VBUS_HIGH_MARGIN		200
#define MCA_QUICK_CHG_OPEN_PATH_COUNT			10
#define MCA_QC_QUICK_CHG_OPEN_PATH_COUNT		40
#define MCA_QC_QUICK_CHG_TUNE_WINDOW_MV			1500
#define MCA_QC_QUICK_CHG_RETRY_COUNT			5
#define MCA_QC_QUICK_CHG_IBUS_UCP_RISING_MA		250
#define MCA_QC_ADP_VOLT_DEFAULT				9000
#define MCA_QC_ADP_MIN_VOLT				5000
#define MCA_QC_ADP_MAX_VOLT				9500
#define MCA_QC_ADP_MAX_CURR				2500
#define MCA_QC_VOLTAGE_MAX_DEFAULT_MV			9000
#define MCA_QC_CURRENT_MAX_DEFAULT_MA			3000
#define MCA_QC_CURRENT_MAX_HIGH_MA			5000
#define BUS_VOLTAGE_MIN					3000

/* charge pump */
#define MCA_QUICK_CHG_CP_DEFAULT_FSW			480
#define MCA_QUICK_CHG_MULTI_PATH_IBUS_TH		300
#define MCA_QUICK_CHG_MAX_ERR_COUNT			10
#define MCA_QUICK_CHG_SINGLE_MAX_ERR_COUNT		5
#define MCA_QUICK_CHG_MULTI_PATH_SOC_TH			60
#define MCA_QUICK_CHG_START_SOC_TH			91
#define MCA_QUICK_CHG_FFC_SWITCH_PMIC_SOC		94
#define MCA_QUICK_CHG_MODE_SWITCH_POWER_MIN		64
#define MCA_QUICK_CHG_TAPER_COUNT			3
#define MCA_QUICK_CHG_DONE_COUNT			6

/* schedule value */
#define MCA_QUICK_CHG_MONITOR_INTERVAL			700
#define MCA_QUICK_CHG_PPS_PTF_INTERVAL			10000
#define MCA_QUICK_CHG_PPS_PTF_MAX_CHECK			4
#define MCA_QUICK_CHG_VFC_INTERVAL			5000

/* batt_para */
#define BATT_PARA_MAX_GROUP				16
enum mca_quick_charge_batt_para_ele {
	MCA_QUICK_CHG_BATT_ROLE = 0,
	MCA_QUICK_CHG_BATT_ID,
	MCA_QUICK_CHG_TEMP_PARA_NAME,
	MCA_QUICK_CHG_BATT_PARA_MAX,
};

enum mca_quick_usbpd_dpm_port_pps_ptf_type {
	USBPD_QUICK_DPM_PORT_PPS_PTF_NOT_SUPPORTED = 0,
	USBPD_QUICK_DPM_PORT_PPS_PTF_NORMAL,
	USBPD_QUICK_DPM_PORT_PPS_PTF_WARNING,
	USBPD_QUICK_DPM_PORT_PPS_PTF_OVERTEMP,
};

/* temp para */
#define TEMP_PARA_MAX_GROUP				10
enum mca_quick_charge_temp_para_ele {
	MCA_QUICK_CHG_TEMP_LOW = 0,
	MCA_QUICK_CHG_TEMP_HIGH,
	MCA_QUICK_CHG_LOW_TEMP_HYS,
	MCA_QUICK_CHG_HIGH_TEMP_HYS,
	MCA_QUICK_CHG_TEMP_MAX_CURRENT,
	MCA_QUICK_CHG_TEMP_NROMAL_FV,
	MCA_QUICK_CHG_TEMP_FFC_FV,
	MCA_QUICK_CHG_VOLT_PARA_NAME,
	MCA_QUICK_CHG_VOLT_FFC_PARA_NAME,
	MCA_QUICK_CHG_STAGE_PARA_NAME,
	MCA_QUICK_CHG_FFC_STAGE_PARA_NAME,
	MCA_QUICK_CHG_TEMP_PARA_MAX,
};

struct mca_quick_charge_temp_para {
	int temp_low;
	int temp_high;
	int low_temp_hysteresis;
	int high_temp_hysteresis;
	int max_current;
	int normal_max_fv;
	int ffc_max_fv;
};

/* volt para */
#define VOLT_PARA_MAX_GROUP				10
enum mca_quick_charge_volt_para_ele {
	MCA_QUICK_CHG_VOLTAGE = 0,
	MCA_QUICK_CHG_CURRENT_MAX,
	MCA_QUICK_CHG_CURRENT_MIN,
	MCA_QUICK_CHG_VOLT_PARA_MAX,
};

struct mca_quick_charge_volt_para {
	int voltage;
	int current_max;
	int current_min;
};

/* volt step para */
#define VSTEP_PARA_MAX_GROUP				16
#define VSTEP_PARA_EX_MAX_GROUP				8
enum mca_quick_charge_volt_step_para_ele {
	MCA_QUICK_CHG_VOLT_RATIO = 0,
	MCA_QUICK_CHG_CURRENT,
	MCA_QUICK_CHG_VSTEP_RATIO,
	MCA_QUICK_CHG_VSTEP_PARA_MAX,
};

enum mca_quick_charge_volt_step_para_ex_ele {
	MCA_QUICK_CHG_DELTA_V_THR = 0,
	MCA_QUICK_CHG_VSTEP_MULTIPLIER,
	MCA_QUICK_CHG_VBAT_CHANGE_THR,
	MCA_QUICK_CHG_BOOST_TIME_THR,
	MCA_QUICK_CHG_VSTEP_PARA_EX_MAX,
};

struct mca_quick_charge_volt_step_para {
	int volt_ratio;
	int cur_gap;
	int vstep_ratio;
};

struct mca_quick_charge_volt_step_ex_para {
	int delta_v_thr;
	int vstep_multiplier;
	int vbat_change_thr;
	u64 boost_time_thr;
};

enum mca_quick_charge_battery_type {
	MCA_BATTERY_TYPE_SINGLE = 0,
	MCA_BATTERY_TYPE_PARALLEL,
	MCA_BATTERY_TYPE_SERIES,
	MCA_BATTERY_TYPE_MAX = MCA_BATTERY_TYPE_SERIES,
};

enum mca_quick_charge_cp_type {
	MCA_CP_TYPE_SINGLE = 0,
	MCA_CP_TYPE_PARALLEL,
	MCA_CP_TYPE_SERIES,
	MCA_CP_TYPE_MAX = MCA_CP_TYPE_SERIES,
};

enum mca_quick_charge_work_cp {
	MCA_QUICK_CHG_CP_MASTER = 0,
	MCA_QUICK_CHG_CP_SLAVE,
	MCA_QUICK_CHG_CP_DUAL,
	MCA_QUICK_CHG_CP_MODE_MAX,
};

enum mac_quick_charge_chn_ele {
	MCA_QUICK_CHG_CH_SINGLE = 0,
	MCA_QUICK_CHG_CH_MULTI,
	MCA_QUICK_CHG_CH_MAX,
};

struct mca_quick_charge_volt_para_info {
	int volt_para_size;
	int *stage_para;
	struct mca_quick_charge_volt_para *volt_para;
};

struct mca_quick_charge_temp_para_info {
	struct mca_quick_charge_temp_para temp_para;
	struct mca_quick_charge_volt_para_info volt_info;
	struct mca_quick_charge_volt_para_info volt_ffc_info;
};

struct mca_quick_charge_batt_para_info {
	int temp_para_size;
	struct mca_quick_charge_temp_para_info *temp_info;
};

#define MCA_VFC_PARA_MAX_GROUP				15
enum mca_quick_charge_vfc_para_ele {
	MCA_QUICK_CHG_VFC_PARA_IOUT = 0,
	MCA_QUICK_CHG_VFC_PARA_FSW,
	MCA_QUICK_CHG_VFC_PARA_SIZE,
};

struct mca_quick_charge_vfc_iout_fsw_map {
	int iout;
	int fsw;
};

struct mca_quick_charge_vfc_para {
	int support_cp_vfc;
	int vfc_para_size;
	struct mca_quick_charge_vfc_iout_fsw_map iout_fsw_map[MCA_VFC_PARA_MAX_GROUP];
};

enum mca_quick_charge_support_mode {
	MCA_QUICK_CHG_MODE_DIV_1 = 1,
	MCA_QUICK_CHG_MODE_DIV_2 = 2,
	MCA_QUICK_CHG_MODE_DIV_4 = 4,
};

enum mca_quick_charge_chg_mode {
	CHG_MODE_DIV1 = 0,
	CHG_MODE_DIV2,
	CHG_MODE_DIV4,
	CHG_MODE_MAX,
};

/* The window of charge current one charge-pump division is used for. */
struct cp_work_mode_unit {
	int l_thres;
	int h_thres;
};

/**
 * struct mca_cp_work_mode - which division the charge pump runs at
 * @support_mode_switch: the division may change while charging
 * @cur_mode:            the division running now, as a CHG_MODE_*
 * @default_mode:        the division picked at start, also the highest allowed
 * @div1_vbat_low:       the cell is below the adapter's lowest voltage, so the
 *                       1:1 division cannot be used
 * @last_stage:          the last charge stage was reached; the division no
 *                       longer moves
 * @mode_table:          the current window of each division
 */
struct mca_cp_work_mode {
	bool support_mode_switch;
	int cur_mode;
	int default_mode;
	bool div1_vbat_low;
	bool last_stage;
	struct cp_work_mode_unit mode_table[CHG_MODE_MAX];
};

struct mca_quick_charge_adp_info {
	int adp_mode;
	struct adapter_power_cap cap_info;
};

#define MCA_QUICK_CHG_IBUS_QUEUE_SIZE			20
struct mca_quick_charge_ibus_queue_info {
	int data[MCA_QUICK_CHG_IBUS_QUEUE_SIZE];
	int count;
	int index;
	u32 sum;
	int avg;
};

enum mca_quick_charge_temp_hys_ele {
	MCA_QUICK_TEMP_HYS_DIS = 0,
	MCA_QUICK_TEMP_HYS_HIGH,
	MCA_QUICK_TEMP_HYS_LOW,
};

enum mca_quick_chg_attr_list {
	MCA_QUICK_CHG_MODE_ENABLE = 0,
	MCA_QUICK_CHG_CP_PATH_ENABLE,
	MCA_QUICK_CHG_POWER_MAX,
	MCA_QUICK_CHG_FAKE_PPS_PTF,
	MCA_QUICK_CHG_TYPE,
	MCA_QUICK_CHG_CURR_LIMIT,
	MCA_QUICK_CHG_CURR_RATIO,
	MCA_QUICK_CHG_VOLT_DEC,
	MCA_QUICK_CHG_VFC_IOUT,
};

enum qc_vbus_voltage_tune {
	DIR_HOLD,
	DIR_UP,
	DIR_DOWN,
};

enum VBUS_TUNE_STAT {
	XM_VBUS_TUNE_INIT,
	XM_VBUS_TUNE_VBUS_LOW,
	XM_VBUS_TUNE_WAIT,
	XM_VBUS_TUNE_READY_UP,
	XM_VBUS_TUNE_READY_DOWN,
	XM_VBUS_TUNE_OK,
	XM_VBUS_TUNE_FAIL,
};

struct secure_quick_charge_data {
	int max_vbatt;
	int secure_cur;
};

struct mca_quick_charge_process_data {
	int adp_type;
	int charge_flag;
	int type_chg;
	int temp_hys_en;
	int parallel_temp_hys_en[FG_SITE_MAX];
	int total_err;
	int error_num[MCA_QUICK_CHG_CP_MODE_MAX];
	int cur_protocol;
	int work_mode;
	int cur_cp_mode;
	int cur_work_cp;
	int dual_cp_count;
	int adp_mode;
	int adp_mode_power[CHG_MODE_MAX];
	int ui_power;
	int max_power;
	int quick_charge_type;
	int cur_adp_volt;
	int cur_adp_cur;
	int min_adp_volt;
	int max_adp_volt;
	int max_adp_curr;
	int max_ibat_final;
	/* ceiling an EU PPS adapter is held to once it ran at its rating */
	int max_adap_ibat;
	int ratio;
	int delta_volt;
	int delta_ibat;
	int single_curr;
	int max_curr;
	int open_path;
	int multi_ibus_th;
	int again_multi_ibus_th;
	int ibus_inc;
	int ibus_dec;
	int temp_max_cur[FG_IC_MAX];
	int temp_max_fv[FG_IC_MAX];
	int vbat[FG_IC_MAX];
	int parall_vbat[FG_SITE_MAX];
	int ibat[FG_IC_MAX];
	int parall_ibat[FG_SITE_MAX];
	int soc;
	int ibat_total;
	int vbus;
	int ibus;
	/* passes spent below the threshold for leaving dual-pump charging */
	int count;
	int adp_info_index[CHG_MODE_MAX];
	int *thermal_cur;
	int *cp_path_enable;
	int temp_para_index[FG_IC_MAX];
	int parall_temp_para_index[FG_SITE_MAX];
	int cur_stage[FG_IC_MAX];
	int parall_cur_stage[FG_SITE_MAX];
	int ffc_flag;
	int parall_ffc_flag[FG_SITE_MAX];
	int parall_zone_changed[FG_SITE_MAX];
	int zone_changed;
	int vfc_iout;
	int ibus_compensation;
	struct mca_quick_charge_adp_info adp_info[ADAPTER_CAP_MAX_NR];
	struct mca_quick_charge_volt_para_info *cur_volt_para[FG_IC_MAX];
	struct mca_quick_charge_volt_para_info *cur_volt_paraller[FG_SITE_MAX];
	struct secure_quick_charge_data secure_info;
	struct mca_quick_charge_ibus_queue_info ibus_queue;
	int sw_ocp_curr;
	int ocp_thr[2];
	bool stop_charging;
	bool base_batt_close;
	bool to_pd_verify;
	bool parallel_to_pd_verify;
	int base_close_curr;
};

struct mca_quick_charge_sysfs_data {
	int chg_enable;
	int chg_limit[MCA_QUICK_CHG_CH_MAX];
	int mode_enable[CHG_MODE_MAX];
	int cp_path_enable[CHG_MODE_MAX][CP_ROLE_MAX];
	int curr_limit[CHG_MODE_MAX][MCA_QUICK_CHG_CH_MAX];
	int cur_ratio;
	int volt_dec;
};

struct mca_quick_charge_smartchg_data {
	int delta_fv;
	int delta_ichg;
	int pwr_boost_state;
};

struct mca_quick_charge_info {
	struct device *dev;
	struct mca_votable *input_suspend_voter;
	struct mca_votable *buck_input_voter;
	struct mca_votable *buck_charge_curr_voter;
	struct mca_votable *buck_vterm_voter;
	struct delayed_work monitor_work;
	struct delayed_work pps_ptf_work;
	struct delayed_work vfc_work;
	struct notifier_block shutdown_notifier;
	struct mca_votable *voter[MCA_QUICK_CHG_CH_MAX * CHG_MODE_MAX];
	struct mca_votable *chg_disable_voter;
	struct mca_votable *chg_en_voter;
	struct mca_votable *single_chg_cur_voter;
	struct mca_votable *multi_chg_cur_voter;
	struct mca_votable *vterm_voter;
	struct mca_votable *thermal_flip_voter;
	struct mca_votable *flip_fcc_voter;
	struct mca_quick_charge_smartchg_data smartchg_data;
	/* dts config */
	int batt_type;
	int cp_type;
	int min_vbat;
	int max_vbat;
	int recharge_vbat;
	int die_temp_max;
	int adp_temp_max;
	int en_buck_parallel_chg;
	int fv_hys_delta_mv;
	int curr_terminate_ratio;
	int curr_terminate_section;
	int buck_icl_fcc_curr[2];
	int div_delta_ibat[CHG_MODE_MAX];
	int div_delta_volt[CHG_MODE_MAX];
	int div_single_curr[CHG_MODE_MAX];
	int div_max_curr[CHG_MODE_MAX];
	int open_path_th[CHG_MODE_MAX];
	int multi_ibus_th[CHG_MODE_MAX];
	int again_multi_ibus_th[CHG_MODE_MAX];
	int ibus_inc_hysteresis[CHG_MODE_MAX];
	int ibus_dec_hysteresis[CHG_MODE_MAX];
	int ibus_compensation[CHG_MODE_MAX];
	int support_mode;
	int support_ichg_cutoff_priority;
	int max_power;
	int pmic_fv_compensation;
	int ui_max_power_limit;
	bool is_qcom_qc;
	int qc3_max_vbus_limit_mv;
	int qc3p5_max_vbus_limit_mv;
	int qc3_ibat_max_limit_ma;
	int qc3_max_ibus_limit_ma;
	int qc3p5_max_ibus_limit_ma;
	int qc3p5_max_ibat_limit_ma;
	int qc_taper_fcc_ma;
	int qc_taper_soc_thr;
	int pps_taper_fcc_ma;
	int pps_middle_taper_fcc_ma;
	int pps_high_taper_fcc_ma;
	int qc_normal_charge_fv_max_mv;
	int qc3_taper_vol_hys;
	int qc3p5_taper_vol_hys;
	int pps_taper_vol_hys;
	/* consecutive passes the taper condition held */
	int fc2_taper_timer;
	int hardware_cv;
	int has_gbl_batt_para;
	int support_pmic_vterm_dynamics_adjust;
	int support_dtpt_ichg_dec;
	int enable_vstep_ex;
	/* charge current ceiling asked for by smart charging */
	int bypass_curr;
	bool taper_done_no_retry;
	bool pd_switch_to_pmic;
	struct mca_quick_charge_volt_step_para vstep_para[VSTEP_PARA_MAX_GROUP];
	struct mca_quick_charge_volt_step_ex_para vstep_para_ex[VSTEP_PARA_EX_MAX_GROUP];
	struct mca_quick_charge_batt_para_info batt_para[FG_IC_MAX];
	struct mca_quick_charge_batt_para_info base_flip_para[FG_SITE_MAX];
	struct mca_quick_charge_vfc_para vfc_para;
	/* process data */
	int online;
	int cur_support_mode;
	int thermal_cur[CHG_MODE_MAX][MCA_QUICK_CHG_CH_MAX];
	int thermal_flip_cur;
	struct mca_quick_charge_process_data proc_data;
	struct mca_quick_charge_sysfs_data sysfs_data;
	int batt_auth;
	int batt_missing;
	int force_stop;
	int tune_vbus_retry;
	int master_cp_enable_count;
	int dtpt_status;
	int trigger_antr_burn;
	int shutdown_mode;
	int is_eu_model;
	int pps_ptf;
	int fake_pps_ptf;
	int support_curr_monitor;
	int curr_monitor_time_s;
	int cp_switch_pmic_th;
	int third_pps_switch_pmic;
	int quick_chg_fv_hys;
	int mult_to_single_count;
	int use_buck_fcc_threshold;
	bool fastchg_temp_flag;
	time64_t time_start;
	bool boost_done;
	bool check_vbat_ov;
	int support_base_flip;
	int base_flip_same;
	int pmih_fcc;
	bool stop_pmic;
	int current_fcc;
	struct mutex data_lock;
	bool support_cancel_compensation;
	int max_vbat_threshold;
	int cp_vendor;
	struct mca_cp_work_mode cp_work_mode;
	bool pmic_chg_need_fixed_volt;
	bool fcc_too_low;
	int curr_batt_cycle[FG_SITE_MAX];
	bool parallel_fcc_too_low[FG_SITE_MAX];
	int pmic_single_cp_chg;
	int cur_max_threshold[3];
	int ibus_threshold[3];
	int pmih_fcc_value[4];
	int vbat_threshold;
	int final_vterm;
	int override_vterm;
	int last_override_vterm;
	int vterm_count;
	int third_pps_ibat_limit;
	bool support_eu_power;
	int eu_vbus_max;
	int eu_ibus_max;
};

#endif /* __MCA_QUICK_CHARGE_H__ */
