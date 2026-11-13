/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The buck charger strategy: what the buck charger is told to do while a
 * cable is attached, for every kind of charger that can be on the port.
 */

#ifndef __MCA_STRATEGY_BUCKCHG_H__
#define __MCA_STRATEGY_BUCKCHG_H__

#include <linux/ktime.h>
#include <linux/notifier.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <mca/protocol/protocol_class.h>

/* Defaults of the votables this strategy owns. */
#define STATEGY_INPUT_DEFAULT_VALUE		100
#define STATEGY_INPUT_5V_DEFAULT_VALUE		2000
#define STATEGY_INPUT_9V_DEFAULT_VALUE		1600
#define STATEGY_CHARGE_CURRENT_DEFAULT_VALUE	4000
#define STATEGY_ITERM_DEFAULT_VALUE		500
#define STATEGY_VTERM_DEFAULT_VALUE		4400
#define STATEGY_CHARGE_DISENABLE		0
#define STATEGY_CHARGE_INPUT_SUSPEND		0

/* Platforms whose term current is the smallest asked for, as a pair. */
#define STRATEGY_ITERM_MIN_PLATFORM		4

/* Bus voltages the adapter is asked for, in millivolts. */
#define STATEGY_CHARGE_VBUS_5V			5000
#define STATEGY_CHARGE_VBUS_9V			9000
#define STATEGY_CHARGE_VBUS_12V			12000

/* What the charger is put back to with nothing attached. */
#define STATEGY_CHARGE_INPUT_VOLT_DEFAULT	4600
#define STATEGY_CHARGE_FWS_DEFAULT		5000

/* AICL thresholds, and the battery voltages they switch at, in millivolts. */
#define STATEGY_CHARGE_AICL_VBAT_TH		4100
#define STATEGY_CHARGE_AICL_VBAT_TH_4P2V	4200
#define STATEGY_CHARGE_AICL_TH_4P1V		4100
#define STATEGY_CHARGE_AICL_TH_4P4V		4400
#define STATEGY_CHARGE_AICL_TH_4P5V		4500

/* With the battery missing. */
#define STAEGY_BATT_MISS_ICL			500
#define STAEGY_BATT_MISS_ICL_PARALLEL		1000
#define STAEGY_BATT_MISS_FV			4200

#define STAEGY_CHARGE_PLATE_SHOCK		600

/* Adapter voltages for charging a wireless receiver off the port. */
#define STATEGY_USBIN_WLS_REV_CHG_VOLTAGE_DEFAULT	9000
#define STATEGY_USBIN_WLS_REV_CHG_VOLTAGE_BUF_H		9200
#define STATEGY_USBIN_WLS_REV_CHG_VOLTAGE_BUF_L		8400

/* Input and charge current defaults per charger type, in milliamps. */
#define CHARGE_PPS_INPUT_DEFAULT		3000
#define CHARGE_DCP_INPUT_DEFAULT		1500
#define CHARGE_DCP_INPUT_BOOST			2000
#define CHARGE_CDP_INPUT_DEFAULT		1500
#define CHARGE_SDP_INPUT_DEFAULT		500
#define CHARGE_FLOAT_INPUT_DEFAULT		1000
#define CHARGE_PPS_PTF_INPUT_DEFAULT		1000
#define CHARGE_PPS_PTF_CHARGE_DEFAULT		1100
#define CHARGE_DCP_CHARGE_DEFAULT		2000
#define CHARGE_DCP_CHARGE_BOOST			2000
#define CHARGE_CDP_CHARGE_DEFAULT		1500
#define CHARGE_SDP_CHARGE_DEFAULT		500
#define CHARGE_FLOAT_CHARGE_DEFAULT		500
#define CHARGE_BATT_AUTH_FAIL_DEFAULT		2000
#define CHARGE_WLS_REVCHG_INPUT_DEFAULT		1000
#define CHARGE_WLS_REVCHG_INPUT_QC2		500

/* Work periods, in milliseconds. */
#define CHARGE_MONITOR_WORK_NORMAL_INTERVAL	10000
#define CHARGE_MONITOR_WORK_FAST_INTERVAL	5000
#define CHARGE_SW_CV_WORK_NORMAL_INTERVAL	5000
#define CHARGE_SW_CV_WORK_FAST_INTERVAL		2000
#define WLS_REVCHG_MONITOR_INTERVAL		10000
#define SOURCE_STATUS_MONITOR_INTERVAL		5000
#define SOURCE_STATUS_MONITOR_FAST_INTERVAL	1000

#define ALLOW_START_FFC_BATT_SOC_THR		95
#define ALLOW_FFC_TEMP_LOW_THR			20
#define ALLOW_FFC_TEMP_HIGH_THR			48
#define PMIC_ITERM_COMPENSATION_DEFAULT		30
#define BAT_TEMP_FV_COMP_HOT_TH_DEFAULT		50
#define SW_CV_FCC_STEP_DEFAULT			50
#define SW_CV_FV_STEP_DEFAULT			5

/* Where the port's over-voltage protection sits relative to the pump. */
#define BUCKCHG_VUSB_OVP_BEFORE_CP		1

/* How the pack is put together, as the device tree numbers it. */
enum mca_buckchg_battery_type {
	MCA_BATTERY_TYPE_SINGLE = 0,
	MCA_BATTERY_TYPE_PARALLEL,
	MCA_BATTERY_TYPE_SERIES,
};

/* Which adapter voltage set applies while charging a receiver off the port. */
enum usbin_wlsrevchg_type {
	REV_USBIN_TYPE_PPS = 0,
	REV_USBIN_TYPE_OTHER,
	REV_USBIN_TYPE_MAX,
};

/* What a PPS adapter says about its own temperature. */
enum mca_buck_usbpd_dpm_port_pps_ptf_type {
	USBPD_BUCK_DPM_PORT_PPS_PTF_NOT_SUPPORTED = 0,
	USBPD_BUCK_DPM_PORT_PPS_PTF_NOT_NORMAL,
	USBPD_BUCK_DPM_PORT_PPS_PTF_NOT_OVERTEMP,
};

/**
 * struct strategy_buckchg_proc_data - where the current charging session stands
 * @online:               a cable is attached
 * @qc_type:              the quick charge type reported to the stack
 * @real_type:            what the attached charger turned out to be
 * @chg_en:               the "online" enable vote has been cast
 * @chg_status:           enum mca_buck_chg_status
 * @ibus_limit:           the input limit last voted for the charger type
 * @ibat_limit:           the charge current last voted for the charger type
 * @voltage:              the bus voltage asked for, in millivolts
 * @vbus:                 bus voltage, in microvolts
 * @ibus:                 bus current, in microamps
 * @vbat:                 battery voltage, in millivolts
 * @ibat:                 battery current, in microamps (negative charging)
 * @curr_pd_pos:          the PDO the PD limits are taken from
 * @wls_revchg_init_done: the bus is up for charging a receiver
 * @pdsuspendsupported:   the PD adapter supports USB suspend
 * @charge_done_force_5v: the bus was dropped to 5 V at end of charge
 * @is_pd_9v:             the PD adapter has a fixed 9 V PDO
 * @eu_start_time:        when a PPS adapter on an EU unit was attached
 * @pre_volt:             the bus voltage the type limits were voted at
 */
struct strategy_buckchg_proc_data {
	int online;
	int qc_type;
	int real_type;
	int chg_en;
	int chg_status;
	int ibus_limit;
	int ibat_limit;
	int voltage;
	int vbus;
	int ibus;
	int vbat;
	int ibat;
	int curr_pd_pos;
	bool wls_revchg_init_done;
	bool pdsuspendsupported;
	bool charge_done_force_5v;
	bool is_pd_9v;
	ktime_t eu_start_time;
	int pre_volt;
};

/**
 * struct strategy_buckchg_dev - the buck charger strategy
 *
 * The device tree properties keep their own names; the rest is described
 * where it is used.
 */
struct strategy_buckchg_dev {
	struct device *dev;
	/* device tree */
	int batt_type;
	int support_multi_buck;
	int in_dcp;
	int in_pd;
	int in_pps;
	int in_hvdcp;
	int in_hvdcp3;
	int in_hvdcp3p5;
	int in_cdp;
	int in_sdp;
	int in_float;
	int chg_dcp;
	int chg_pd;
	int chg_pps;
	int chg_hvdcp;
	int chg_hvdcp3;
	int chg_hvdcp3p5;
	int chg_cdp;
	int chg_sdp;
	int chg_float;
	int chg_batt_auth_failed;
	int ffc_temp_low;
	int ffc_temp_high;
	int pmic_fv_compensation;
	int bat_temp_fv_comp_cold_th;
	int pmic_fv_compensation_cold;
	int bat_temp_fv_comp_hot_th;
	int pmic_fv_compensation_hot;
	bool support_diff_temp_comp;
	int pmic_middle_fv_compensation;
	int pmic_high_fv_compensation;
	int support_pmic_vterm_dynamics_adjust;
	int pmic_wls_fv_compensation;
	int pmic_iterm_compensation;
	int support_reverse_quick_charge;
	int support_revchg_screenon;
	int support_base_flip;
	int base_flip_same;
	int need_cp_to_pmic;
	int support_charge_more;
	int curr_term_compensation;
	int rev_req_vadp[REV_USBIN_TYPE_MAX];
	int rev_vadp_valid_h[REV_USBIN_TYPE_MAX];
	int rev_vadp_valid_l[REV_USBIN_TYPE_MAX];
	int vusb_ovp_location;
	int allow_start_ffc_batt_soc_thr;
	int terminated_by_cp;
	int sw_cv_fcc_step;
	int sw_cv_fv_step;

	/* charge pump watch */
	int cp_absent_retry_count;
	bool cp_i2c_error_reported;
	bool pmic_temp_term_flag;

	/* votables */
	struct mca_votable *chg_enable_voter;
	struct mca_votable *input_suppend_voter;
	struct mca_votable *input_voltage_voter;
	struct mca_votable *input_limit_voter;
	struct mca_votable *charge_limit_voter;
	struct mca_votable *iterm_voter;
	struct mca_votable *vterm_voter;
	struct mca_votable *buck_5v_in_voter;
	struct mca_votable *buck_5v_ich_voter;
	struct mca_votable *buck_9v_in_voter;
	struct mca_votable *buck_9v_ich_voter;
	int buck_5v_in;
	int buck_5v_ich;
	int buck_9v_in;
	int buck_9v_ich;
	int hvdcp_allow_flag;
	int vbat_ov_count;
	int buck_abnormal_cnt;
	int pwr_boost_state;

	/* session */
	struct strategy_buckchg_proc_data proc_data;
	struct adapter_power_cap_info pwr_cap;

	struct delayed_work monitor_work;
	struct delayed_work sw_cv_work;
	struct delayed_work base_flip_sw_cv_work;
	struct delayed_work wls_revchg_monitor_work;
	struct delayed_work csd_pulse_process_work;
	struct delayed_work source_status_monitor_work;
	struct delayed_work check_pd_secret_work;
	struct delayed_work rerun_handle_pd_auth_work;
	struct delayed_work soc_limit_stepper_work;
	struct notifier_block thermal_board_nb;
	struct notifier_block panel_nb;

	int init_ok;
	int thermal_board_temp;
	int last_gear_shift;
	int aicl_thd;
	int wls_revchg_en;
	int verify_process_end;
	int batt_auth;
	int last_pdo_nums;
	bool rev_icl_for_qc2;
	bool csd_flag;
	int pps_ptf;
	bool is_eu_model;
	bool reverse_auth_sts;
	int otg_boost_src;
	bool sw_cv_running;
	bool is_non_compliant_qc;
	bool non_compliant_run_once;
	int batt_miss;
	bool start_quick_revchg;
	bool revchg_bcl;
	int parallel_iterm;
	bool screen_status;
	int ibat_check_cnt;
	int gear_shift_sent;
	bool wls_online;
	bool wls_revchg_online;
	int soc_limit_low;
	int soc_limit_high;
	int soc_limit_sts;
	int cp_vendor;
};

#endif /* __MCA_STRATEGY_BUCKCHG_H__ */
