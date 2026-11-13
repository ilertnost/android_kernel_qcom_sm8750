/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Events shared between the charging drivers.
 *
 * A driver that notices something the rest of the stack cares about -- the
 * screen turning off, a cable arriving, a temperature threshold -- announces
 * it here, and the drivers that care register for the types they want.  The
 * same events can also be sent to userspace as a uevent.
 */

#ifndef __MCA_EVENT_H
#define __MCA_EVENT_H

#include <linux/notifier.h>
#include <linux/types.h>

/*
 * Which chain an event travels on.  A driver waiting on one kind of event is
 * not woken for every other kind, so a charger interrupt does not run the
 * panel notifiers.
 */
enum mca_event_notify_type {
	MCA_EVENT_TYPE_BEGIN = 0,
	MCA_EVENT_TYPE_CHARGER_CONNECT = MCA_EVENT_TYPE_BEGIN,
	MCA_EVENT_TYPE_CHARGE_TYPE,
	MCA_EVENT_TYPE_BATTERY_INFO,
	MCA_EVENT_TYPE_CP_INFO,
	MCA_EVENT_TYPE_HW_INFO,
	MCA_EVENT_CHARGE_STATUS,
	MCA_EVENT_TYPE_THERMAL_TEMP,
	MCA_EVENT_TYPE_PANEL,
	MCA_EVENT_TYPE_TYPEC_PORT_STATUS,
	MCA_EVENT_TYPE_DEBUG,
	MCA_EVENT_TYPE_END,
};

/* Which event.  The chain it travels on is named separately. */
enum mca_event_notify_list {
	MCA_EVENT_BEGIN = 0,
	MCA_EVENT_USB_DISCONNECT = MCA_EVENT_BEGIN,
	MCA_EVENT_USB_CONNECT,
	MCA_EVENT_WIRELESS_DISCONNECT,
	MCA_EVENT_WIRELESS_CONNECT,
	MCA_EVENT_WIRELESS_REVCHG,
	MCA_EVENT_CHARGE_TYPE_CHANGE,
	MCA_EVENT_CHARGE_CAP_CHANGE,
	MCA_EVENT_CHARGE_VERIFY_PROCESS_END,
	MCA_EVENT_WIRELESS_INT_CHANGE,
	MCA_EVENT_WIRELESS_SW_SET_QC_ICHG,
	MCA_EVENT_WIRELESS_SW_SET_THERMAL_ICHG,
	MCA_EVENT_WIRELESS_WLS_DEBUG,
	MCA_EVENT_WIRELESS_EPP_MODE,
	MCA_EVENT_WIRELESS_MAGNETIC_TX_F2,
	MCA_EVENT_WIRELESS_AUDIO_PHONE_STS,
	MCA_EVENT_WIRELESS_THERMAL_PHONE_FLAG,
	MCA_EVENT_WIRELESS_USB_REVCHG,
	MCA_EVENT_BATTERY_STS_CHANGE,
	MCA_EVENT_BATTERY_FAKE_POWER,
	MCA_EVENT_BATTERY_HEALTH_CHANGE,
	MCA_EVENT_BATTERY_TOTAL_ITERM,
	MCA_EVENT_CP_VUSB_INSERT,
	MCA_EVENT_CP_VUSB_OUT,
	MCA_EVENT_CP_VUSB_OVP,
	MCA_EVENT_CP_VBAT_OVP,
	MCA_EVENT_CP_VBUS_OVP,
	MCA_EVENT_CP_VWPC_OVP,
	MCA_EVENT_CP_PMID2OUT_OVP,
	MCA_EVENT_CP_PMID2OUT_UVP,
	MCA_EVENT_CP_IBAT_OCP,
	MCA_EVENT_CP_IBUS_OCP,
	MCA_EVENT_CP_IBUS_UCP,
	MCA_EVENT_CP_CBOOT_FAIL,
	MCA_EVENT_CP_VOUT_UVLO,
	MCA_EVENT_CP_POR_FLAG,
	MCA_EVENT_CP_IIC_ERROR,
	MCA_EVENT_CP_TSHUT_FLAG,
	MCA_EVENT_CP_NEW_MODE,
	MCA_EVENT_CONN_ANTIBURN_CHANGE,
	MCA_EVENT_BATT_BTB_CHANGE,
	MCA_EVENT_BATT_AUTH_PASS,
	MCA_EVENT_LPD_STATUS_CHANGE,
	MCA_EVENT_PMIC_INIT_DONE,
	MCA_EVENT_CC_SHORT_VBUS,
	MCA_EVENT_VBAT_OVP_CHANGE,
	MCA_EVENT_IBAT_OCP_CHANGE,
	MCA_EVENT_QUICK_REVCHG_CHANGE,
	MCA_EVENT_BOOST_STS,
	MCA_EVENT_CID_STS,
	MCA_EVENT_USB_STS_CHANGE,
	MCA_EVENT_CHARGE_ABNORMAL,
	MCA_EVENT_CHARGE_RESTORE,
	MCA_EVENT_SOC_LIMIT,
	MCA_EVENT_BATTERY_DTPT,
	MCA_EVENT_CSD_SEND_PULSE,
	MCA_EVENT_PPS_PTF,
	MCA_EVENT_IS_EU_MODEL,
	MCA_EVENT_PLATE_SHOCK,
	MCA_EVENT_START_QUICK_REVCHG,
	MCA_EVENT_REVCHG_BCL,
	MCA_EVENT_HANDLE_ALLOW_CHARGE,
	MCA_EVENT_MASTER_BATT_CLOSE,
	MCA_EVENT_THERMAL_BOARD_TEMP_CHANGE,
	MCA_EVENT_CHARGE_RECHARGE_CHECK,
	MCA_EVENT_PANEL_SCREEN_STATE_CHANGE,
	MCA_EVENT_PANEL_HBM_STATE_CHANGE,
	MCA_EVENT_TYPEC_PORT_CHANGE,
	MCA_EVENT_BQ_FG_ERROR,
	MCA_EVENT_CHARGE_ACTION,
	MCA_EVENT_SINK_PWR_SUSPEND_CHANGE,
	MCA_EVENT_USB_SUSPEND,
	MCA_EVENT_WIRELESS_MAGNETIC_CASE_INT,
	MCA_EVENT_WIRELESS_MAGNETIC_TX_INT,
	MCA_EVENT_FG_OTA_PROCESS,
	MCA_EVENT_FCC_TOO_LOW,
	MCA_EVENT_BUCKCHG_BATT_OV,
	MCA_EVENT_DEBUG_CTRL_DOUBLE85,
	MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT,
	MCA_EVENT_DEBUG_CTRL_MEMORY_TEST,
	MCA_EVENT_DEBUG_CTRL_SOC_LIMIT,
	MCA_EVENT_BMD_STSTUS_CHANGE,
	MCA_EVENT_WIRELESS_PEN_HALL_CHANGE,
	MCA_EVENT_WIRELESS_PEN_PPE_HALL_CHANGE,
	MCA_EVENT_MAX,
};

/**
 * struct mca_event_notify_data - one uevent, ready to send
 * @event:     the whole environment string, "KEY=value", already formatted
 * @event_len: how long that string is
 *
 * The caller formats the string rather than handing over a key and a number
 * separately, because not every event userspace listens for is one number:
 * some carry several fields in one variable.  @event_len is its length, which
 * is what bounds the copy into the uevent buffer.
 */
struct mca_event_notify_data {
	const char	*event;
	int		event_len;
};

/* How long an event string may be, and so how big a caller's buffer is. */
#define MCA_EVENT_NOTIFY_SIZE		128

int mca_event_block_notify_register(enum mca_event_notify_type type,
				    struct notifier_block *nb);
int mca_event_block_notify_unregister(enum mca_event_notify_type type,
				      struct notifier_block *nb);
void mca_event_block_notify(enum mca_event_notify_type type,
			    unsigned long event,
			    void *data);
void mca_event_report_uevent(const struct mca_event_notify_data *n_data);

#endif /* __MCA_EVENT_H */
