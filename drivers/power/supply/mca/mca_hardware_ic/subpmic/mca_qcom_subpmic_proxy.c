// SPDX-License-Identifier: GPL-2.0-only
/*
 * mca_qcom_subpmic_proxy.c
 *
 * Buck charger, BC1.2 detector and Quick Charge protocol backed by the ADSP.
 *
 * The charger hardware is owned by the ADSP charger firmware; every request
 * from the charging stack is turned into a property read or write over
 * mca_adsp_glink, and the notifications the firmware sends are turned into
 * charging stack events.  The board wiring the firmware needs (OTG boost
 * source and gate pins, cable-id pin, charge pump presence, EU model) is
 * pushed to it at probe and again every time the link comes back up.
 *
 * This driver also provides the QTI battery charger interface used by the
 * flash LED and haptics drivers, as the QTI battery charger driver is not
 * used on this platform.
 *
 * Copyright (c) 2023-2023 Xiaomi Technologies Co., Ltd.
 */

#define MCA_LOG_TAG "qcom_subpmic"

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/printk.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/soc/qcom/battery_charger.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <mca/common/mca_adsp_glink.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_hwid.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/platform/platform_bc12_class.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/platform/platform_cp_class.h>
#include <mca/protocol/protocol_class.h>
#include <mca/protocol/protocol_pd_class.h>
#include <mca/protocol/protocol_qc_class.h>
#include <mca/strategy/strategy_class.h>

#define QC_VOLT_MIN_MV			5000
#define QC_VOLT_MAX_MV			12000

/* Adapter step size, time per step and settle band (QC3.5 vs. others). */
#define QC3P5_STEP_MV			20
#define QC3P5_STEP_MS			5
#define QC3P5_SETTLE_MV			500
#define QC_STEP_MV			200
#define QC_STEP_MS			50
#define QC_SETTLE_MV			800

#define QC_POLL_MS			50
#define QC_VBUS_REMOVED_MV		4000

#define SUBPMIC_WAKEUP_MS		1000
#define SUBPMIC_USB_TYPE_RETRY_MS	2000
#define SUBPMIC_SYNC_CFG_START_MS	100
#define SUBPMIC_SYNC_CFG_RETRY_MS	1000
#define SUBPMIC_SYNC_CFG_RETRY_MAX	60

#define SUBPMIC_UEVENT_BUF_SIZE		128

/* Bus resistance reported to the flash LED driver, in micro-ohms. */
#define SUBPMIC_BATTERY_RESISTANCE_UOHM	100000

/* revchg_sts value telling the reverse charge path the boost is off. */
#define SUBPMIC_REVCHG_BOOST_OFF	(0x100 << 16)

/**
 * struct otg_cfg_info - OTG boost wiring, sent to the ADSP as is
 * @boost_src:      boost source (enum otg_src_cfg)
 * @gpio_chip_type: which chip the gate pins belong to (enum gpio_chip_cfg)
 * @vdd_boost_gpio: boost supply enable pin
 * @ovp_en_gpio:    OVP gate enable pin
 */
struct otg_cfg_info {
	int boost_src;
	int gpio_chip_type;
	int vdd_boost_gpio;
	int ovp_en_gpio;
};

/**
 * struct adsp_notify_change_node - an ADSP notification deferred to a work
 * @notify_header: link in qcom_subpmic_data.adsp_notify_list
 * @type:          the notification (enum mca_adsp_charger_notification)
 * @value:         its payload
 */
struct adsp_notify_change_node {
	struct list_head notify_header;
	int type;
	int value;
};

/**
 * struct qcom_subpmic_data - driver state
 * @dev:                 this device
 * @usb_type_work:       reads USB online state and adapter type
 * @notify_change_work:  handles the notifications queued on @adsp_notify_list
 * @ship_mode_en:        enter ship mode on the next power off or restart
 * @reboot_notifier:     enters ship mode
 * @shutdown_notifier:   tells the ADSP the system is going down
 * @sync_cfg_work:       sends the board configuration to the ADSP
 * @adsp_notify_list:    deferred notifications
 * @notify_lock:         protects @adsp_notify_list
 * @support_2s_charging: "mi,support-2s-charging"
 * @support_dual_panel:  "mi,support-dual-panel"
 * @support_multi_bc12:  "mi,support-multi-bc12"
 * @support_cid:         "mi,support-cid"
 * @cid_gpio_int:        cable-id interrupt pin, sent to the ADSP
 * @otg_cfg:             OTG boost wiring, sent to the ADSP
 * @online:              USB online as last read from the ADSP
 * @real_type:           adapter type as last read from the ADSP
 * @force_notify:        report the adapter type again even if unchanged
 * @adsp_init_done:      the ADSP has answered once
 * @lpd_status:          last moisture detection status
 * @cc_short_vbus:       last CC-shorted-to-VBUS status
 * @pps_ptf:             last PPS adapter temperature flag
 * @revchg_sts:          reverse charge status passed with
 *                       MCA_EVENT_QUICK_REVCHG_CHANGE
 * @bsinkpowersuspend:   last sink power suspend state
 * @eu_model:            EU model, sent to the ADSP
 * @cid_sts:             last cable-id status
 */
struct qcom_subpmic_data {
	struct device *dev;
	struct delayed_work usb_type_work;
	struct work_struct notify_change_work;
	bool ship_mode_en;
	struct notifier_block reboot_notifier;
	struct notifier_block shutdown_notifier;
	struct delayed_work sync_cfg_work;
	struct list_head adsp_notify_list;
	spinlock_t notify_lock;
	bool support_2s_charging;
	bool support_dual_panel;
	bool support_multi_bc12;
	bool support_cid;
	int cid_gpio_int;
	struct otg_cfg_info otg_cfg;
	int online;
	int real_type;
	int force_notify;
	int adsp_init_done;
	int lpd_status;
	int cc_short_vbus;
	int pps_ptf;
	int revchg_sts;
	bool bsinkpowersuspend;
	bool eu_model;
	int cid_sts;
};

static int qcom_subpmic_is_init_ok(void *data)
{
	struct qcom_subpmic_data *subpmic = data;

	if (!subpmic || !subpmic->adsp_init_done)
		return -1;

	return subpmic->adsp_init_done;
}

static int qcom_subpmic_get_online(void *data, int *online)
{
	struct qcom_subpmic_data *subpmic = data;

	*online = subpmic->online;

	return 0;
}

static int qcom_subpmic_is_charge_done(void *data, bool *charge_done)
{
	int status = 0;
	int ret;

	ret = mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_CHGR_STATUS, &status,
				       sizeof(status));
	if (ret < 0)
		return ret;

	*charge_done = status == CHGR_STATUS_TERMINATION;

	return 0;
}

static int qcom_subpmic_get_input_current_limit(void *data, int *input_curr_lmt)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_INPUT_CURR_LIMIT,
					input_curr_lmt, sizeof(int));
}

static int qcom_subpmic_get_bus_curr(void *data, int *bus_curr)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_USB_CURRENT_NOW, bus_curr,
					sizeof(int));
}

static int qcom_subpmic_get_bus_volt(void *data, int *bus_volt)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_USB_VOLTAGE_NOW, bus_volt,
					sizeof(int));
}

static int qcom_subpmic_get_usb_sns_volt(void *data, int *usb_sns_volt)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_USB_SNS_VOLTAGE_NOW,
					usb_sns_volt, sizeof(int));
}

static int qcom_subpmic_get_vsys_volt(void *data, int *sys_volt)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_VSYSMIN, sys_volt,
					sizeof(int));
}

static int qcom_subpmic_get_chg_status(void *data, int *chg_status)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_CHGR_STATUS,
					chg_status, sizeof(int));
}

static int qcom_subpmic_get_chg_type(void *data, int *chg_type)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_CHGR_TYPE, chg_type,
					sizeof(int));
}

static int qcom_subpmic_get_term_current(void *data, int *term_curr)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_TERM_CURR, term_curr,
					sizeof(int));
}

static int qcom_subpmic_get_term_volt(void *data, int *term_volt)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_TERM_VOLT, term_volt,
					sizeof(int));
}

static int qcom_subpmic_get_wls_curr(void *data, int *wls_curr)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_DC_CURRENT_NOW, wls_curr,
					sizeof(int));
}

static int qcom_subpmic_set_input_suspend(void *data, bool enable)
{
	mca_log_info("set input suspend %d\n", enable);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_INPUT_SUSPEND,
					 &enable, sizeof(enable));
}

static int qcom_subpmic_set_wireless_input_suspend(void *data, bool enable)
{
	mca_log_info("set wls input suspend %d\n", enable);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_DC_INPUT_SUSPEND,
					 &enable, sizeof(enable));
}

static int qcom_subpmic_set_input_current_limit(void *data, int input_curr_lmt)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_INPUT_CURR_LIMIT,
					 &input_curr_lmt,
					 sizeof(input_curr_lmt));
}

static int qcom_subpmic_set_wls_input_current_limit(void *data,
						    int wls_input_curr_lmt)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_DC_INPUT_CURR_LIMIT,
					 &wls_input_curr_lmt,
					 sizeof(wls_input_curr_lmt));
}

static int qcom_subpmic_set_input_volt_limit(void *data, int input_volt_lmt)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_INPUT_VOLT_LIMIT,
					 &input_volt_lmt,
					 sizeof(input_volt_lmt));
}

static int qcom_subpmic_set_charge_current(void *data, int ichg)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_CHARGE_CURR, &ichg,
					 sizeof(ichg));
}

static int qcom_subpmic_enable_charging(void *data, bool enable)
{
	mca_log_info("enable charging: %d\n", enable);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_ENABLE_CHARGING,
					 &enable, sizeof(enable));
}

static int qcom_subpmic_set_buck_fsw(void *data, int buck_fsw)
{
	mca_log_info("set buck fsw: %d\n", buck_fsw);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_SET_FSW, &buck_fsw,
					 sizeof(buck_fsw));
}

static int qcom_subpmic_set_term_current(void *data, int term_curr)
{
	mca_log_info("set iterm: %d\n", term_curr);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_TERM_CURR,
					 &term_curr, sizeof(term_curr));
}

static int qcom_subpmic_set_term_volt(void *data, int term_volt)
{
	mca_log_info("set vterm: %d\n", term_volt);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_TERM_VOLT,
					 &term_volt, sizeof(term_volt));
}

static int qcom_subpmic_set_prechg_volt(void *data, int prechg_volt)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_PRECHG_VOLT,
					 &prechg_volt, sizeof(prechg_volt));
}

static int qcom_subpmic_set_prechg_current(void *data, int prechg_curr)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_PRECHG_CURR,
					 &prechg_curr, sizeof(prechg_curr));
}

static int qcom_subpmic_set_usb_aicl_cont_thd(void *data, int aicl_cont_thd)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_AICL_CONT_THD,
					 &aicl_cont_thd, sizeof(aicl_cont_thd));
}

static int qcom_subpmic_get_usb_aicl_cont_thd(void *data, int *aicl_cont_thd)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_AICL_CONT_THD,
					aicl_cont_thd, sizeof(int));
}

static int qcom_subpmic_set_opt_fws(void *data, int opt_fws)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_OPT_FWS, &opt_fws,
					 sizeof(opt_fws));
}

static int qcom_subpmic_usb_adapter_allow_override(void *data, bool enable)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_USB_ADAPTER_ALLOW_OVERRIDE,
					 &enable, sizeof(enable));
}

static int qcom_subpmic_set_qc3_volt(void *data, int qc3_volt)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_QC3_VOLT, &qc3_volt,
					 sizeof(qc3_volt));
}

/* Returns the boost source itself rather than 0. */
static int qcom_subpmic_get_otg_boost_src(void *data, int *otg_boost_src)
{
	struct qcom_subpmic_data *subpmic = data;

	*otg_boost_src = subpmic->otg_cfg.boost_src;
	mca_log_info("get otg_boost_src: %d\n", subpmic->otg_cfg.boost_src);

	return subpmic->otg_cfg.boost_src;
}

static int qcom_subpmic_get_otg_gate_enable_status(void *data, int *status)
{
	mca_log_info("get otg plugin status\n");

	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_OTG_GATE_ENABLE_STATUS,
					status, sizeof(int));
}

static int qcom_subpmic_get_otg_boost_enable_status(void *data, int *status)
{
	mca_log_info("get otg enable status\n");

	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_OTG_BOOST_ENABLE_STATUS,
					status, sizeof(int));
}

static int qcom_subpmic_set_boost_enable(void *data, int boost_enable)
{
	mca_log_info("set otg enable: 0x%2x\n", boost_enable);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_ENABLE_BOOST,
					 &boost_enable, sizeof(boost_enable));
}

static int qcom_subpmic_set_boost_voltage(void *data, int boost_voltage)
{
	mca_log_info("set boost voltage: 0x%2x\n", boost_voltage);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_ADJUST_BOOST_VOLTAGE,
					 &boost_voltage, sizeof(boost_voltage));
}

static int qcom_subpmic_set_aicl_enable(void *data, bool enable)
{
	mca_log_info("set aicl enable: %d\n", enable);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_ENABLE_AICL,
					 &enable, sizeof(enable));
}

static int qcom_subpmic_set_rerun_aicl(void *data, bool enable)
{
	mca_log_info("set rerun aicl: %d\n", enable);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_RERUN_AICL,
					 &enable, sizeof(enable));
}

static int qcom_subpmic_set_restart_aicl(void *data, bool enable)
{
	mca_log_info("set restart aicl: %d\n", enable);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_RESTART_AICL,
					 &enable, sizeof(enable));
}

static int qcom_subpmic_is_support_cid(void *data, bool *support_cid)
{
	struct qcom_subpmic_data *subpmic = data;

	*support_cid = subpmic->support_cid;

	return 0;
}

/*
 * Ship mode cannot be entered while the system runs: the request is only
 * recorded here and sent to the ADSP from the reboot notifier.
 */
static int qcom_subpmic_set_ship_mode(void *data, bool enable)
{
	struct qcom_subpmic_data *subpmic = data;

	subpmic->ship_mode_en = enable;
	mca_log_err("is_enable_shipmode = %d\n", enable);

	return 0;
}

static int qcom_subpmic_get_ship_mode(void *data, bool *ship_mode)
{
	struct qcom_subpmic_data *subpmic = data;

	*ship_mode = subpmic->ship_mode_en;

	return 0;
}

static int qcom_subpmic_set_wls_vdd_flag(void *data, bool enable)
{
	mca_log_info("set wls vdd flag: %d\n", enable);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_DC_SET_VDD_FLAG, &enable,
					 sizeof(enable));
}

static int qcom_subpmic_get_lpd_enable(void *data, int *lpd_enable)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_USB_MOISTURE_DETECTION_ENABLE,
					lpd_enable, sizeof(int));
}

static int qcom_subpmic_get_lpd_status(void *data, int *lpd_status)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_USB_MOISTURE_DETECTION_STATUS,
					lpd_status, sizeof(int));
}

static int qcom_subpmic_get_lpd_sbu1(void *data, int *lpd_sbu1)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_LPD_SBU1, lpd_sbu1,
					sizeof(int));
}

static int qcom_subpmic_get_lpd_sbu2(void *data, int *lpd_sbu2)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_LPD_SBU2, lpd_sbu2,
					sizeof(int));
}

static int qcom_subpmic_get_lpd_cc1(void *data, int *lpd_cc1)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_LPD_CC1, lpd_cc1,
					sizeof(int));
}

static int qcom_subpmic_get_lpd_cc2(void *data, int *lpd_cc2)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_LPD_CC2, lpd_cc2,
					sizeof(int));
}

static int qcom_subpmic_get_lpd_dp(void *data, int *lpd_dp)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_LPD_DP, lpd_dp,
					sizeof(int));
}

static int qcom_subpmic_get_lpd_dm(void *data, int *lpd_dm)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_LPD_DM, lpd_dm,
					sizeof(int));
}

static int qcom_subpmic_set_lpd_sbu1(void *data, int lpd_sbu1)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_LPD_SBU1, &lpd_sbu1,
					 sizeof(lpd_sbu1));
}

static int qcom_subpmic_set_lpd_control(void *data, int lpd_control)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_LPD_CONTROL,
					 &lpd_control, sizeof(lpd_control));
}

static int qcom_subpmic_get_lpd_control(void *data, int *lpd_control)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_LPD_CONTROL,
					lpd_control, sizeof(int));
}

static int qcom_subpmic_set_lpd_uart_control(void *data, int lpd_uart_control)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_LPD_UART_CONTROL,
					 &lpd_uart_control,
					 sizeof(lpd_uart_control));
}

static int qcom_subpmic_get_lpd_uart_control(void *data, int *lpd_uart_control)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_LPD_UART_CONTROL,
					lpd_uart_control, sizeof(int));
}

static int qcom_subpmic_get_pack_vbat(void *data, int *pack_vbat)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_PACK_VBAT, pack_vbat,
					sizeof(int));
}

static int qcom_subpmic_get_pack_ibat(void *data, int *pack_ibat)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_PACK_IBAT, pack_ibat,
					sizeof(int));
}

/*
 * The value is kept even if the ADSP could not be told: it is sent again
 * by the configuration sync and gates the sink power suspend notification.
 */
static int qcom_subpmic_set_eu_model(void *data, bool enable)
{
	struct qcom_subpmic_data *subpmic = data;
	int ret;

	ret = mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_EU_MODEL, &enable,
					sizeof(enable));
	if (ret < 0)
		mca_log_err("Failed to write eu model to adsp: %d\n", ret);

	subpmic->eu_model = enable;
	mca_log_info("set_eu_model = %d\n", enable);

	return 0;
}

static int qcom_subpmic_get_aicl_status(void *data, int *aicl_status)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_AICL_STATUS,
					aicl_status, sizeof(int));
}

static int qcom_subpmic_set_too_hot_limit(void *data, int too_hot_limit)
{
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_TOO_HOT_LIMIT,
					 &too_hot_limit, sizeof(too_hot_limit));
}

static int qcom_subpmic_get_pack_tbat(void *data, int *pack_tbat)
{
	return mca_adsp_glink_read_prop(ADSP_PROP_ID_BUCK_PACK_TBAT, pack_tbat,
					sizeof(int));
}

static int qcom_subpmic_bc12_det_en(int enable, void *data)
{
	return 0;
}

/* Falls back to the last type reported by the ADSP if the read fails. */
static int qcom_subpmic_get_usb_real_type(int *type, void *data)
{
	struct qcom_subpmic_data *subpmic = data;

	if (mca_adsp_glink_read_prop(ADSP_PROP_ID_USB_USB_REAL_TYPE, type,
				     sizeof(int)))
		*type = subpmic->real_type;

	return 0;
}

/**
 * qcom_subpmic_set_qc_volt() - request a Quick Charge voltage
 * @data: driver state
 * @volt: requested VBUS in mV, 5000..12000
 *
 * After the request is sent, VBUS is polled until it is within the settle
 * band of the target (approached from the side it started on), the cable is
 * removed, or the time the adapter needs for the steps has elapsed.  No wait
 * is done for a QC2 adapter, for a return to 5 V, or for a change smaller
 * than one step.
 *
 * Return: 0 on success, -1 for an out of range voltage, or the glink error.
 */
static int qcom_subpmic_set_qc_volt(void *data, int volt)
{
	struct qcom_subpmic_data *subpmic = data;
	int step_mv, step_ms, settle_mv;
	int diff, settled_mv, steps;
	int vbus = 0;
	ktime_t start;
	int ret;

	start = ktime_get_boottime();

	if (subpmic->real_type == XM_CHARGER_TYPE_HVDCP3P5) {
		step_mv = QC3P5_STEP_MV;
		step_ms = QC3P5_STEP_MS;
		settle_mv = QC3P5_SETTLE_MV;
	} else {
		step_mv = QC_STEP_MV;
		step_ms = QC_STEP_MS;
		settle_mv = QC_SETTLE_MV;
	}

	if (volt < QC_VOLT_MIN_MV || volt > QC_VOLT_MAX_MV) {
		mca_log_err("invalid qc voltage: %d\n", volt);
		return -1;
	}

	mca_adsp_glink_read_prop(ADSP_PROP_ID_USB_VOLTAGE_NOW, &vbus,
				 sizeof(vbus));
	vbus /= 1000;
	diff = volt - vbus;
	if (diff > 0)
		settled_mv = volt - settle_mv;
	else
		settled_mv = volt + settle_mv;

	mca_log_info("real_type: %d, volt: %d, vbus: %d\n", subpmic->real_type,
		     volt, vbus);

	ret = mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_QC_VOLT, &volt,
					sizeof(volt));
	if (ret)
		return ret;

	if (subpmic->real_type == XM_CHARGER_TYPE_HVDCP2 ||
	    volt == QC_VOLT_MIN_MV || abs(diff) < step_mv)
		return 0;

	steps = abs(diff) / step_mv;
	do {
		msleep(QC_POLL_MS);
		mca_adsp_glink_read_prop(ADSP_PROP_ID_USB_VOLTAGE_NOW, &vbus,
					 sizeof(vbus));
		vbus /= 1000;
		mca_log_info("vbus: %d\n", vbus);

		if (vbus < QC_VBUS_REMOVED_MV || !subpmic->online) {
			mca_log_err("usb removed, break\n");
			return 0;
		}

		if ((diff > 0 && vbus > settled_mv) ||
		    (diff < 0 && vbus < settled_mv))
			return 0;
	} while (ktime_ms_delta(ktime_get_boottime(), start) <=
		 (s64)steps * step_ms);

	mca_log_info("wait time out, break\n");

	return 0;
}

static int qcom_subpmic_set_qc_volt_cmd(void *data, int hvdcp_cmd)
{
	mca_log_info("set_qc_volt_cmd: %d\n", hvdcp_cmd);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_QC_VOLT_CMD,
					 &hvdcp_cmd, sizeof(hvdcp_cmd));
}

static void qcom_subpmic_update_usb_type_work(struct work_struct *work)
{
	struct qcom_subpmic_data *subpmic =
		container_of(to_delayed_work(work), struct qcom_subpmic_data,
			     usb_type_work);
	int pd_active = 0;
	int real_type = 0;
	int online = 0;
	int ret;

	/* With a second BC1.2 port, a PD contract there owns the type. */
	if (subpmic->support_multi_bc12) {
		protocol_class_pd_get_pd_active(TYPEC_PORT_1, &pd_active);
		if (pd_active)
			return;
	}

	ret = mca_adsp_glink_read_prop(ADSP_PROP_ID_USB_ONLINE, &online,
				       sizeof(online));
	if (ret < 0) {
		mca_log_err("Failed to read usb_online rc=%d\n", ret);
		goto retry;
	}

	if (!subpmic->adsp_init_done) {
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_PMIC_INIT_DONE, NULL);
		subpmic->adsp_init_done = 1;
	}

	if (subpmic->online != online) {
		mca_log_err("usb online: %d\n", online);
		subpmic->online = online;
		if (online) {
			protocol_class_pd_set_pd_active(TYPEC_PORT_0, 1);
			mca_event_block_notify(MCA_EVENT_TYPE_CHARGER_CONNECT,
					       MCA_EVENT_USB_CONNECT, NULL);
		} else {
			protocol_class_pd_set_pd_active(TYPEC_PORT_0, 0);
			mca_event_block_notify(MCA_EVENT_TYPE_CHARGER_CONNECT,
					       MCA_EVENT_USB_DISCONNECT, NULL);
			pm_relax(subpmic->dev);
		}
	}

	ret = mca_adsp_glink_read_prop(ADSP_PROP_ID_USB_USB_REAL_TYPE,
				       &real_type, sizeof(real_type));
	if (ret < 0) {
		mca_log_err("Failed to read USB_ADAP_TYPE rc=%d\n", ret);
		goto retry;
	}

	if (subpmic->real_type != real_type) {
		mca_log_err("usb real_type: %d\n", real_type);
		subpmic->real_type = real_type;
	} else if (!subpmic->force_notify) {
		return;
	}

	subpmic->force_notify = 0;
	mca_event_block_notify(MCA_EVENT_TYPE_CHARGE_TYPE,
			       MCA_EVENT_CHARGE_TYPE_CHANGE,
			       &subpmic->real_type);
	return;

retry:
	queue_delayed_work(system_wq, &subpmic->usb_type_work,
			   msecs_to_jiffies(SUBPMIC_USB_TYPE_RETRY_MS));
}

/*
 * The retry budget is shared by every run of the work for the lifetime of the
 * module, including runs after an ADSP restart.
 */
static void qcom_subpmic_sync_cfg_work(struct work_struct *work)
{
	static int count;
	struct qcom_subpmic_data *subpmic =
		container_of(to_delayed_work(work), struct qcom_subpmic_data,
			     sync_cfg_work);
	bool cp_present = false;
	int ret;

	ret = platform_class_cp_get_present(CP_ROLE_MASTER, &cp_present);
	if (ret) {
		mca_log_err("Failed to get cp state, rc=%d\n", ret);
		goto retry;
	}

	ret = mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_CP_STATE, &cp_present,
					sizeof(cp_present));
	if (ret) {
		mca_log_err("Failed to send cp state, rc=%d\n", ret);
		goto retry;
	}
	mca_log_err("Success to send cp state, rc=%d\n", ret);

	ret = mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_OTG_CFG,
					&subpmic->otg_cfg,
					sizeof(subpmic->otg_cfg));
	if (ret) {
		mca_log_err("Failed to send otg cfg, rc=%d\n", ret);
		goto retry;
	}
	mca_log_err("Success to send otg cfg, rc=%d\n", ret);

	/* The cable-id pin and the EU model are only sent on cable-id boards. */
	if (!subpmic->support_cid)
		return;

	ret = mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_CID_CFG,
					&subpmic->cid_gpio_int,
					sizeof(subpmic->cid_gpio_int));
	if (ret) {
		mca_log_err("Failed to send cid cfg, rc=%d\n", ret);
		goto retry;
	}
	mca_log_err("Success to send cid cfg, rc=%d\n", ret);

	ret = mca_adsp_glink_write_prop(ADSP_PROP_ID_BUCK_EU_MODEL,
					&subpmic->eu_model,
					sizeof(subpmic->eu_model));
	if (ret) {
		mca_log_err("Failed to send eu model, rc=%d\n", ret);
		goto retry;
	}
	mca_log_err("Success to send eu model, rc=%d\n", ret);

	return;

retry:
	if (count++ < SUBPMIC_SYNC_CFG_RETRY_MAX)
		queue_delayed_work(system_wq, &subpmic->sync_cfg_work,
				   msecs_to_jiffies(SUBPMIC_SYNC_CFG_RETRY_MS));
}

static void qcom_subpmic_handle_boost_notify(struct qcom_subpmic_data *subpmic,
					     struct adsp_notify_change_node *node)
{
	int boost_en = node->value;

	mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO, MCA_EVENT_BOOST_STS,
			       &node->value);

	if (!subpmic->otg_cfg.boost_src) {
		if (!boost_en)
			subpmic->revchg_sts = 0;
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_QUICK_REVCHG_CHANGE,
				       &subpmic->revchg_sts);
		return;
	}

	/* The charge pump OVP gate is closed while the boost runs. */
	mca_log_err("enable_ovpgate: %d\n", !boost_en);
	platform_class_cp_enable_ovpgate_with_check(CP_ROLE_MASTER, OTG_TYPE,
						    !boost_en);
	if (boost_en)
		return;

	subpmic->revchg_sts = (subpmic->revchg_sts & 0xffff) |
			      SUBPMIC_REVCHG_BOOST_OFF;
	mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
			       MCA_EVENT_QUICK_REVCHG_CHANGE,
			       &subpmic->revchg_sts);
	subpmic->revchg_sts = 0;
}

static void qcom_subpmic_notify_change_work(struct work_struct *work)
{
	struct qcom_subpmic_data *subpmic =
		container_of(work, struct qcom_subpmic_data, notify_change_work);
	struct adsp_notify_change_node *node;

	for (;;) {
		spin_lock(&subpmic->notify_lock);
		node = list_first_entry_or_null(&subpmic->adsp_notify_list,
						struct adsp_notify_change_node,
						notify_header);
		if (node)
			list_del(&node->notify_header);
		spin_unlock(&subpmic->notify_lock);

		if (!node)
			break;

		switch (node->type) {
		case MCA_CHARGER_ADT_CAP_CHANGE_NOTIFY:
			mca_event_block_notify(MCA_EVENT_TYPE_CHARGE_TYPE,
					       MCA_EVENT_CHARGE_CAP_CHANGE,
					       NULL);
			break;
		case MCA_CHARGER_ENABLE_BOOST_NOTIFY:
			qcom_subpmic_handle_boost_notify(subpmic, node);
			break;
		case MCA_CHARGER_TYPEC_CHANGE_NOTIFY:
			mca_event_block_notify(MCA_EVENT_TYPE_TYPEC_PORT_STATUS,
					       MCA_EVENT_TYPEC_PORT_CHANGE,
					       NULL);
			break;
		default:
			break;
		}

		kfree(node);
	}
}

static void qcom_subpmic_report_uevent(const char *fmt, int value)
{
	char buf[SUBPMIC_UEVENT_BUF_SIZE] = { 0 };
	struct mca_event_notify_data n_data = { 0 };

	n_data.event_len = snprintf(buf, sizeof(buf), fmt, value);
	n_data.event = buf;
	mca_event_report_uevent(&n_data);
}

static void moisture_detection_uevent(int lpd_status)
{
	mca_log_info("moisture detection uevent notify\n");
	qcom_subpmic_report_uevent("POWER_SUPPLY_MOISTURE_DET_STS=%d",
				   lpd_status);
}

static void cc_short_vbus_uevent(int cc_short_vbus)
{
	mca_log_info("cc short vbus uevent notify\n");
	qcom_subpmic_report_uevent("POWER_SUPPLY_CC_SHORT_VBUS=%d",
				   cc_short_vbus);
}

/*
 * Called from the pmic_glink rx work, so it may sleep.  Notifications that
 * need further exchanges with the ADSP are deferred to notify_change_work.
 */
static void qcom_subpmic_notify_cb(int notification, void *buf, int len,
				   void *data)
{
	struct qcom_subpmic_data *subpmic = data;
	struct adsp_notify_change_node *node;
	int value = *(int *)buf;

	if (!subpmic)
		return;

	pm_wakeup_dev_event(subpmic->dev, SUBPMIC_WAKEUP_MS, true);

	switch (notification) {
	case MCA_CHARGER_TYPE_CHANGE_NOTIFY:
		queue_delayed_work(system_wq, &subpmic->usb_type_work, 0);
		break;
	case MCA_CHARGER_ADT_CAP_CHANGE_NOTIFY:
	case MCA_CHARGER_ENABLE_BOOST_NOTIFY:
	case MCA_CHARGER_TYPEC_CHANGE_NOTIFY:
		node = kmalloc(sizeof(*node), GFP_ATOMIC);
		if (!node)
			break;

		node->type = notification;
		node->value = 0;
		if (notification == MCA_CHARGER_ENABLE_BOOST_NOTIFY) {
			node->value = value;
			mca_log_err("recv enable boost notify: %d\n", value);
		}

		spin_lock(&subpmic->notify_lock);
		list_add_tail(&node->notify_header, &subpmic->adsp_notify_list);
		spin_unlock(&subpmic->notify_lock);
		queue_work(system_wq, &subpmic->notify_change_work);
		break;
	case MCA_CHARGER_LPD_CHANGE_NOTIFY:
		subpmic->lpd_status = value;
		mca_log_err("recv adsp report lpd status: %d\n", value);
		moisture_detection_uevent(subpmic->lpd_status);
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_LPD_STATUS_CHANGE,
				       &subpmic->lpd_status);
		break;
	case MCA_CHARGER_CC_SHORT_VBUS_NOTIFY:
		subpmic->cc_short_vbus = value;
		mca_log_err("recv cc short vbus notify: %d\n", value);
		cc_short_vbus_uevent(subpmic->cc_short_vbus);
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_CC_SHORT_VBUS,
				       &subpmic->cc_short_vbus);
		break;
	case MCA_CHARGER_PPS_PTF_NOTIFY:
		subpmic->pps_ptf = value;
		mca_log_err("recv pps ptf notify: %d\n", value);
		mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					  MCA_EVENT_PPS_PTF, subpmic->pps_ptf);
		mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					  MCA_EVENT_PPS_PTF, subpmic->pps_ptf);
		break;
	case MCA_CHARGER_SINK_PWR_SUSPEND_NOTIFY:
		if (!subpmic->eu_model)
			break;
		mca_log_err("recv bsinkpowersuspend change\n");
		subpmic->bsinkpowersuspend = !!(value & 0xff);
		mca_event_block_notify(MCA_EVENT_TYPE_CHARGE_TYPE,
				       MCA_EVENT_SINK_PWR_SUSPEND_CHANGE,
				       &subpmic->bsinkpowersuspend);
		break;
	case MCA_CHARGER_QUICK_REVCHG_NOTIFY:
		subpmic->revchg_sts = value;
		mca_log_info("recv cp revert notify\n");
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_QUICK_REVCHG_CHANGE,
				       &subpmic->revchg_sts);
		break;
	case MCA_CHARGER_OTG_CP_CONFIG_NOTIFY:
		mca_log_info("recv otg cp config notify: %d\n", value);
		platform_class_cp_set_revchg(CP_ROLE_MASTER, value != 0);
		break;
	case MCA_CHARGER_CID_STS_NOTIFY:
		subpmic->cid_sts = value;
		mca_log_info("recv cid_sts: %d\n", value);
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO, MCA_EVENT_CID_STS,
				       &subpmic->cid_sts);
		break;
	default:
		break;
	}
}

static int qcom_subpmic_glink_down_cb(void *data)
{
	struct qcom_subpmic_data *subpmic = data;

	mca_log_info("glink down\n");
	subpmic->force_notify = 1;
	mca_event_block_notify(MCA_EVENT_CHARGE_STATUS,
			       MCA_EVENT_CHARGE_ABNORMAL, NULL);

	return 0;
}

static int qcom_subpmic_sync_cb(void *data)
{
	struct qcom_subpmic_data *subpmic = data;

	mca_log_info("glink up\n");
	mca_event_block_notify(MCA_EVENT_CHARGE_STATUS,
			       MCA_EVENT_CHARGE_RESTORE, NULL);
	queue_delayed_work(system_wq, &subpmic->usb_type_work, 0);
	queue_delayed_work(system_wq, &subpmic->sync_cfg_work, 0);

	return 0;
}

static struct mca_adsp_call_back g_qcom_subpmic_glink_cb = {
	.glink_down_cb = qcom_subpmic_glink_down_cb,
	.sync_data_cb = qcom_subpmic_sync_cb,
	.notify_cb = qcom_subpmic_notify_cb,
};

/*
 * Ship mode is entered on power off and restart if it was requested.  The
 * charge pump OVP gate is opened first unless the pump is an SC8541.  The
 * reboot action is passed to the ADSP as the property value.
 */
static int qcom_subpmic_ship_mode(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	struct qcom_subpmic_data *subpmic =
		container_of(nb, struct qcom_subpmic_data, reboot_notifier);
	int chip_vendor = -1;
	int ret;

	if ((action != SYS_RESTART && action != SYS_POWER_OFF) ||
	    !subpmic->ship_mode_en)
		return NOTIFY_DONE;

	mca_log_info("set adsp shipmode\n");
	platform_class_cp_get_chip_vendor(CP_ROLE_MASTER, &chip_vendor);
	mca_log_info("cp type: %d\n", chip_vendor);
	if (chip_vendor != SC8541_VENDOR)
		platform_class_cp_enable_ovpgate(CP_ROLE_MASTER, false);

	ret = mca_adsp_glink_write_prop(ADSP_PROP_ID_SHIP_MODE, &action,
					sizeof(action));
	if (ret < 0)
		pr_emerg("Failed to write ship mode: %d\n", ret);

	return NOTIFY_DONE;
}

static int qcom_subpmic_shutdown_cb(struct notifier_block *nb,
				    unsigned long action, void *data)
{
	int ret;

	if (action != SYS_RESTART && action != SYS_POWER_OFF)
		return NOTIFY_DONE;

	mca_log_info("start adsp shutdown\n");
	ret = mca_adsp_glink_write_prop(ADSP_PROP_ID_SHUTDOWN_MODE, &action,
					sizeof(action));
	if (ret < 0)
		pr_emerg("Failed to write shutdown cmd to adsp: %d\n", ret);

	return NOTIFY_DONE;
}

static const struct platform_class_buckchg_ops g_qcom_buckchg_ops = {
	.is_init_ok = qcom_subpmic_is_init_ok,
	.get_online = qcom_subpmic_get_online,
	.is_charge_done = qcom_subpmic_is_charge_done,
	.get_input_curr_lmt = qcom_subpmic_get_input_current_limit,
	.get_bus_curr = qcom_subpmic_get_bus_curr,
	.get_bus_volt = qcom_subpmic_get_bus_volt,
	.get_usb_sns_volt = qcom_subpmic_get_usb_sns_volt,
	.get_sys_volt = qcom_subpmic_get_vsys_volt,
	.get_chg_status = qcom_subpmic_get_chg_status,
	.get_chg_type = qcom_subpmic_get_chg_type,
	.get_term_curr = qcom_subpmic_get_term_current,
	.get_term_volt = qcom_subpmic_get_term_volt,
	.get_wls_curr = qcom_subpmic_get_wls_curr,
	.set_hiz = qcom_subpmic_set_input_suspend,
	.set_wls_hiz = qcom_subpmic_set_wireless_input_suspend,
	.set_input_curr_lmt = qcom_subpmic_set_input_current_limit,
	.set_wls_input_curr_lmt = qcom_subpmic_set_wls_input_current_limit,
	.set_input_volt_lmt = qcom_subpmic_set_input_volt_limit,
	.set_ichg = qcom_subpmic_set_charge_current,
	.set_chg = qcom_subpmic_enable_charging,
	.set_buck_fsw = qcom_subpmic_set_buck_fsw,
	.set_term_curr = qcom_subpmic_set_term_current,
	.set_term_volt = qcom_subpmic_set_term_volt,
	.set_prechg_volt = qcom_subpmic_set_prechg_volt,
	.set_prechg_curr = qcom_subpmic_set_prechg_current,
	.set_qc_volt = qcom_subpmic_set_qc_volt,
	.set_usb_aicl_cont_thd = qcom_subpmic_set_usb_aicl_cont_thd,
	.get_usb_aicl_cont_thd = qcom_subpmic_get_usb_aicl_cont_thd,
	.set_opt_fws = qcom_subpmic_set_opt_fws,
	.usb_adapter_allow_override = qcom_subpmic_usb_adapter_allow_override,
	.set_qc3_volt = qcom_subpmic_set_qc3_volt,
	.get_otg_boost_src = qcom_subpmic_get_otg_boost_src,
	.get_otg_gate_enable_status = qcom_subpmic_get_otg_gate_enable_status,
	.get_otg_boost_enable_status = qcom_subpmic_get_otg_boost_enable_status,
	.set_boost_enable = qcom_subpmic_set_boost_enable,
	.set_boost_voltage = qcom_subpmic_set_boost_voltage,
	.set_aicl_enable = qcom_subpmic_set_aicl_enable,
	.set_rerun_aicl = qcom_subpmic_set_rerun_aicl,
	.set_restart_aicl = qcom_subpmic_set_restart_aicl,
	.is_support_cid = qcom_subpmic_is_support_cid,
	.set_ship_mode = qcom_subpmic_set_ship_mode,
	.get_ship_mode = qcom_subpmic_get_ship_mode,
	.set_wls_vdd_flag = qcom_subpmic_set_wls_vdd_flag,
	.get_lpd_enable = qcom_subpmic_get_lpd_enable,
	.get_lpd_status = qcom_subpmic_get_lpd_status,
	.get_lpd_sbu1 = qcom_subpmic_get_lpd_sbu1,
	.get_lpd_sbu2 = qcom_subpmic_get_lpd_sbu2,
	.get_lpd_cc1 = qcom_subpmic_get_lpd_cc1,
	.get_lpd_cc2 = qcom_subpmic_get_lpd_cc2,
	.get_lpd_dp = qcom_subpmic_get_lpd_dp,
	.get_lpd_dm = qcom_subpmic_get_lpd_dm,
	.set_lpd_sbu1 = qcom_subpmic_set_lpd_sbu1,
	.set_lpd_control = qcom_subpmic_set_lpd_control,
	.get_lpd_control = qcom_subpmic_get_lpd_control,
	.set_lpd_uart_control = qcom_subpmic_set_lpd_uart_control,
	.get_lpd_uart_control = qcom_subpmic_get_lpd_uart_control,
	.get_pack_vbat = qcom_subpmic_get_pack_vbat,
	.get_pack_ibat = qcom_subpmic_get_pack_ibat,
	.set_eu_model = qcom_subpmic_set_eu_model,
	.get_aicl_status = qcom_subpmic_get_aicl_status,
	.set_too_hot_limit = qcom_subpmic_set_too_hot_limit,
	.get_pack_tbat = qcom_subpmic_get_pack_tbat,
};

static const struct platform_bc12_class_ops g_qcom_pmic_bc12_ops = {
	.bc12_det_en = qcom_subpmic_bc12_det_en,
	.get_charge_type = qcom_subpmic_get_usb_real_type,
};

static const struct protocol_class_qc_ops g_adsp_qc_ops = {
	.protocol_qc_get_qc_type = qcom_subpmic_get_usb_real_type,
	.protocol_qc_set_volt = qcom_subpmic_set_qc_volt,
	.protocol_qc_set_volt_cmd = qcom_subpmic_set_qc_volt_cmd,
};

/*
 * QTI battery charger interface for leds-qti-flash and the haptics drivers.
 * The ADSP is not asked: a fixed battery resistance is reported and the
 * flash is reported inactive.
 */
int qti_battery_charger_get_prop(const char *name,
				 enum battery_charger_prop prop_id, int *val)
{
	if (!val)
		return -EINVAL;

	*val = prop_id == BATTERY_RESISTANCE ?
		SUBPMIC_BATTERY_RESISTANCE_UOHM : 0;

	return 0;
}
EXPORT_SYMBOL(qti_battery_charger_get_prop);

/* Nothing raises hboost events on this platform; the chain only exists. */
static RAW_NOTIFIER_HEAD(hboost_notifier);

int register_hboost_event_notifier(struct notifier_block *nb)
{
	return raw_notifier_chain_register(&hboost_notifier, nb);
}
EXPORT_SYMBOL(register_hboost_event_notifier);

int unregister_hboost_event_notifier(struct notifier_block *nb)
{
	return raw_notifier_chain_unregister(&hboost_notifier, nb);
}
EXPORT_SYMBOL(unregister_hboost_event_notifier);

static void qcom_subpmic_parse_dt(struct qcom_subpmic_data *subpmic,
				  struct mca_hwid_info *hwid)
{
	struct device_node *np = subpmic->dev->of_node;

	/*
	 * O2 P1.1 and earlier boards have no external OTG boost; force the
	 * haptics boost there whatever the DT says.
	 */
	if (hwid->platform_version == HARDWARE_PROJECT_O2 &&
	    (hwid->build_version == 0 ||
	     (hwid->build_version == 1 && hwid->minor_version == 0))) {
		subpmic->otg_cfg.boost_src = BOOST_SRC_EXTERNAL;
		mca_log_err("O2 p1.1 change otg boost from external to haptic\n");
	}

	if (subpmic->support_cid) {
		mca_parse_dts_u32(np, "cid_gpio_int", &subpmic->cid_gpio_int, 0);
		mca_log_err("dts sync cid_gpio_int= %#x\n",
			    subpmic->cid_gpio_int);
	}

	mca_log_err("dts sync otg_cfg_src= %d,gpio_chip_type=%d, vdd_boost_gpio=%d, ovp_en_gpio=%d\n",
		    subpmic->otg_cfg.boost_src, subpmic->otg_cfg.gpio_chip_type,
		    subpmic->otg_cfg.vdd_boost_gpio,
		    subpmic->otg_cfg.ovp_en_gpio);
}

static int qcom_subpmic_probe(struct platform_device *pdev)
{
	struct qcom_subpmic_data *subpmic;
	struct mca_hwid_info *hwid;
	struct device_node *np;
	int ret;

	mca_log_info("probe start\n");

	subpmic = devm_kzalloc(&pdev->dev, sizeof(*subpmic), GFP_KERNEL);
	if (!subpmic)
		return -ENOMEM;

	subpmic->dev = &pdev->dev;
	np = pdev->dev.of_node;
	hwid = mca_get_hwid_info();

	subpmic->support_2s_charging =
		of_property_read_bool(np, "mi,support-2s-charging");
	subpmic->support_dual_panel =
		of_property_read_bool(np, "mi,support-dual-panel");
	subpmic->support_multi_bc12 =
		of_property_read_bool(np, "mi,support-multi-bc12");
	subpmic->support_cid = of_property_read_bool(np, "mi,support-cid");
	mca_parse_dts_u32(np, "otg_boost_src", &subpmic->otg_cfg.boost_src,
			  BOOST_SRC_EXTERNAL);
	mca_parse_dts_u32(np, "gpio_chip_type",
			  &subpmic->otg_cfg.gpio_chip_type, GPIO_PMIC_TYPE);
	mca_parse_dts_u32(np, "vdd_boost_en", &subpmic->otg_cfg.vdd_boost_gpio,
			  -1);
	mca_parse_dts_u32(np, "otg_ovp_en", &subpmic->otg_cfg.ovp_en_gpio, -1);

	if (!hwid) {
		mca_log_err("%s parse dt fail\n", __func__);
		return -ENOMEM;
	}
	qcom_subpmic_parse_dt(subpmic, hwid);

	INIT_LIST_HEAD(&subpmic->adsp_notify_list);
	spin_lock_init(&subpmic->notify_lock);
	INIT_DELAYED_WORK(&subpmic->usb_type_work,
			  qcom_subpmic_update_usb_type_work);
	INIT_DELAYED_WORK(&subpmic->sync_cfg_work, qcom_subpmic_sync_cfg_work);
	INIT_WORK(&subpmic->notify_change_work,
		  qcom_subpmic_notify_change_work);

	subpmic->reboot_notifier.notifier_call = qcom_subpmic_ship_mode;
	subpmic->reboot_notifier.priority = 255;
	register_reboot_notifier(&subpmic->reboot_notifier);
	subpmic->shutdown_notifier.notifier_call = qcom_subpmic_shutdown_cb;
	subpmic->shutdown_notifier.priority = 255;
	register_reboot_notifier(&subpmic->shutdown_notifier);

	device_set_wakeup_capable(subpmic->dev, true);
	device_wakeup_enable(subpmic->dev);
	platform_set_drvdata(pdev, subpmic);

	ret = platform_bc12_class_ops_register(BC12_MAIN_ROLE,
					       &g_qcom_pmic_bc12_ops, subpmic);
	if (ret) {
		mca_log_err("%s register protocol ops fail\n", __func__);
		goto err_unregister;
	}

	ret = protocol_class_qc_register_ops(ADAPTER_PROTOCOL_QC,
					     &g_adsp_qc_ops, subpmic);
	if (ret) {
		mca_log_err("%s register qc ops fail\n", __func__);
		goto err_unregister;
	}

	ret = platform_class_buckchg_ops_register(MAIN_BUCK_CHARGER, subpmic,
						  &g_qcom_buckchg_ops);
	if (ret) {
		mca_log_err("%s register buckchg ops fail\n", __func__);
		goto err_unregister;
	}

	ret = mca_adsp_glink_resister_ops(&g_qcom_subpmic_glink_cb, subpmic);
	if (ret) {
		mca_log_err("%s register glink ops fail\n", __func__);
		goto err_unregister;
	}

	queue_delayed_work(system_wq, &subpmic->usb_type_work, 0);
	queue_delayed_work(system_wq, &subpmic->sync_cfg_work,
			   msecs_to_jiffies(SUBPMIC_SYNC_CFG_START_MS));

	mca_log_err("probe ok\n");

	return 0;

err_unregister:
	unregister_reboot_notifier(&subpmic->reboot_notifier);
	unregister_reboot_notifier(&subpmic->shutdown_notifier);

	return ret;
}

/*
 * The class and glink registrations cannot be undone, so the driver is not
 * meant to be unbound; the notifiers and works are still torn down so that
 * nothing references the freed state.
 */
static int qcom_subpmic_remove(struct platform_device *pdev)
{
	struct qcom_subpmic_data *subpmic = platform_get_drvdata(pdev);

	unregister_reboot_notifier(&subpmic->reboot_notifier);
	unregister_reboot_notifier(&subpmic->shutdown_notifier);
	cancel_delayed_work_sync(&subpmic->usb_type_work);
	cancel_delayed_work_sync(&subpmic->sync_cfg_work);
	cancel_work_sync(&subpmic->notify_change_work);

	return 0;
}

static void qcom_subpmic_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,qcom_subpmic" },
	{ }
};

static struct platform_driver qcom_subpmic_driver = {
	.driver = {
		.name = "qcom_subpmic",
		.of_match_table = match_table,
	},
	.probe = qcom_subpmic_probe,
	.remove = qcom_subpmic_remove,
	.shutdown = qcom_subpmic_shutdown,
};
module_platform_driver(qcom_subpmic_driver);

MODULE_DESCRIPTION("qcom subpmic  driver");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
