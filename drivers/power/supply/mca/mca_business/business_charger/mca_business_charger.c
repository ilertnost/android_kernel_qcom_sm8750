// SPDX-License-Identifier: GPL-2.0
/*
 * mca_business_charger.c
 *
 * mca charger business driver
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
#define MCA_LOG_TAG "mca_business_charger"
#endif

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <mca/common/mca_charge_mievent.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/common/mca_sysfs.h>
#include <mca/hardware/hw_connector_antiburn.h>
#include <mca/hardware/hw_lpd_detect.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/platform/platform_cp_class.h>
#include <mca/platform/platform_wireless_class.h>
#include <mca/protocol/protocol_class.h>
#include <mca/shared_memory/charger_partition_class.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include "../../mca_strategy/strategy_wireless/inc/mca_wireless_revchg.h"
#include "inc/mca_business_charger.h"
#include "inc/mca_charger_usb_psy.h"
#include "inc/mca_charger_wireless_psy.h"

#define BUSINESS_CHARGER_THREAD_ACTIVE		1

/* Adapter power (W) at which a verified PD adapter gets each icon. */
#define QUICK_CHG_TYPE_FLASH_CHG_POWER		20
#define QUICK_CHG_TYPE_TURBO_CHG_POWER		30
#define QUICK_CHG_TYPE_SUPER_CHG_POWER		50

#define WLS_REPORT_STATUS_DELAY_MS		2000
#define WLS_ENABLE_RX_DELAY_MS			1500
#define RERUN_USB_SNS_DELAY_MS			10000
#define PMIC_INIT_DONE_RETRY_MS			1000
#define REPORT_QC_TYPE_DELAY_MS			1000

/* Last value written to reserved[0] of charger partition info 2. */
#define CHARGER_PARTITION_INFO_2_MAGIC		0x34567890

static const char quick_charge_type_zero[] = "POWER_SUPPLY_QUICK_CHARGE_TYPE=0";

static void business_charger_update_wired_quick_charge_type(struct business_charger *charger);

static void business_charger_report_quick_charge_type_zero(void)
{
	struct mca_event_notify_data n_data = {
		.event = quick_charge_type_zero,
		.event_len = sizeof(quick_charge_type_zero) - 1,
	};

	mca_event_report_uevent(&n_data);
}

static void update_usb_pd_type(struct business_charger *charger)
{
	static int last_type;
	int cur_type;

	protocol_class_get_adapter_type(ADAPTER_PROTOCOL_BC12,
					(u32 *)&charger->bc12_type);
	protocol_class_get_adapter_type(ADAPTER_PROTOCOL_PD,
					(u32 *)&charger->pd_type);
	mca_log_info("last_bc12_type = %d, bc12_type = %d, pd_type = %d\n",
		     last_type, charger->bc12_type, charger->pd_type);

	if (!charger->bc12_active)
		cur_type = XM_CHARGER_TYPE_UNKNOW;
	else if (charger->pd_type)
		cur_type = charger->pd_type;
	else
		cur_type = charger->bc12_type;

	if (last_type == cur_type)
		return;

	mca_log_err("real_type changed: %d => %d\n", last_type, cur_type);
	charger->real_type = cur_type;
	business_charger_update_usb_psy(charger->real_type, charger->usb_psy_info);
	if (charger->real_type == XM_CHARGER_TYPE_FLOAT)
		mca_charge_mievent_report(CHARGE_DFX_NOT_STANDARD_ADAPTER, NULL, 0);
	last_type = cur_type;
}

static int business_charger_parse_dt(struct business_charger *charger)
{
	struct device_node *node = charger->dev->of_node;
	int ret;

	ret = of_property_read_u32(node, "business,charger-core-test",
				   &charger->dt.test);
	if (ret < 0) {
		mca_log_err("not find property %s\n", "business,charger-core-test");
		return ret;
	}
	mca_log_info("%s: %d\n", "business,charger-core-test", charger->dt.test);

	mca_parse_dts_u32(node, "support-wls", &charger->support_wls, 0);
	mca_log_info("support-wls %d\n", charger->support_wls);
	mca_parse_dts_u32(node, "cp_type", &charger->cp_type, 0);
	mca_log_err("cp_type %d\n", charger->cp_type);

	return 0;
}

/*
 * Called from notifier context: the payload is copied (size bytes) so the
 * event thread can use it after the notifier returns.
 */
static void business_charger_add_event_node(unsigned long event, void *data,
					    int size,
					    struct business_charger *charger)
{
	struct charger_event_lis_node *event_node;

	event_node = kmalloc(sizeof(*event_node), GFP_ATOMIC);
	if (!event_node)
		return;

	event_node->event = event;
	event_node->data = NULL;
	if (data) {
		event_node->data = kmalloc(size, GFP_ATOMIC);
		if (event_node->data)
			memcpy(event_node->data, data, size);
	}

	spin_lock(&charger->list_lock);
	list_add_tail(&event_node->lnode, &charger->header);
	spin_unlock(&charger->list_lock);
	charger->thread_active = BUSINESS_CHARGER_THREAD_ACTIVE;
	wake_up_interruptible(&charger->wait_que);
}

static int business_charger_notifier_connect_cb(struct notifier_block *nb,
						unsigned long event, void *data)
{
	struct business_charger *charger =
		container_of(nb, struct business_charger, connect_nb);
	int size = 0;

	if (event == MCA_EVENT_WIRELESS_REVCHG || event == MCA_EVENT_USB_SUSPEND)
		size = sizeof(bool);

	mca_log_info("event %lu\n", event);
	business_charger_add_event_node(event, data, size, charger);

	return NOTIFY_OK;
}

static int business_charger_notifier_type_cb(struct notifier_block *nb,
					     unsigned long event, void *data)
{
	struct business_charger *charger =
		container_of(nb, struct business_charger, type_change_nb);
	int size = 0;

	switch (event) {
	case MCA_EVENT_CHARGE_TYPE_CHANGE:
	case MCA_EVENT_CHARGE_VERIFY_PROCESS_END:
		size = sizeof(int);
		break;
	case MCA_EVENT_SINK_PWR_SUSPEND_CHANGE:
		size = sizeof(bool);
		break;
	default:
		break;
	}

	mca_log_info("event %lu\n", event);
	business_charger_add_event_node(event, data, size, charger);

	return NOTIFY_OK;
}

static int business_charger_notifier_hw_cb(struct notifier_block *nb,
					   unsigned long event, void *data)
{
	struct business_charger *charger =
		container_of(nb, struct business_charger, hw_info_nb);
	int size = 0;

	switch (event) {
	case MCA_EVENT_BATT_BTB_CHANGE:
		size = sizeof(bool);
		break;
	case MCA_EVENT_LPD_STATUS_CHANGE:
	case MCA_EVENT_CC_SHORT_VBUS:
	case MCA_EVENT_VBAT_OVP_CHANGE:
	case MCA_EVENT_IBAT_OCP_CHANGE:
	case MCA_EVENT_QUICK_REVCHG_CHANGE:
		size = sizeof(int);
		break;
	default:
		break;
	}

	mca_log_info("event %lu\n", event);
	business_charger_add_event_node(event, data, size, charger);

	return NOTIFY_OK;
}

static int business_charger_notifier_chg_sts_cb(struct notifier_block *nb,
						unsigned long event, void *data)
{
	struct business_charger *charger =
		container_of(nb, struct business_charger, chg_sts_nb);
	int size = 0;

	mca_log_info("event %lu\n", event);
	switch (event) {
	case MCA_EVENT_SOC_LIMIT:
	case MCA_EVENT_BATTERY_DTPT:
	case MCA_EVENT_CSD_SEND_PULSE:
	case MCA_EVENT_FG_OTA_PROCESS:
		size = sizeof(int);
		break;
	default:
		break;
	}
	business_charger_add_event_node(event, data, size, charger);

	return NOTIFY_OK;
}

static int business_charger_notifier_cp_info_cb(struct notifier_block *nb,
						unsigned long event, void *data)
{
	struct business_charger *charger =
		container_of(nb, struct business_charger, cp_info_nb);
	int size = 0;

	mca_log_info("event %lu\n", event);
	if (event == MCA_EVENT_CP_TSHUT_FLAG)
		size = sizeof(int) * 2;
	else if (event == MCA_EVENT_CP_CBOOT_FAIL)
		size = sizeof(int);
	business_charger_add_event_node(event, data, size, charger);

	return NOTIFY_OK;
}

static int business_charger_notifier_debug_cb(struct notifier_block *nb,
					      unsigned long event, void *data)
{
	struct business_charger *charger =
		container_of(nb, struct business_charger, debug_nb);
	int size = 0;

	mca_log_info("debug event %lu\n", event);
	switch (event) {
	case MCA_EVENT_DEBUG_CTRL_DOUBLE85:
	case MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT:
	case MCA_EVENT_DEBUG_CTRL_MEMORY_TEST:
	case MCA_EVENT_DEBUG_CTRL_SOC_LIMIT:
		size = sizeof(int);
		break;
	default:
		break;
	}
	business_charger_add_event_node(event, data, size, charger);

	return NOTIFY_OK;
}

static void business_charger_wireless_report_status_work(struct work_struct *work)
{
	struct business_charger *charger = container_of(work,
		struct business_charger, delay_report_status_work.work);
	static const char tx_adapter_zero[] = "POWER_SUPPLY_TX_ADAPTER=0";
	struct mca_event_notify_data event_data = { 0 };
	char event[MCA_EVENT_NOTIFY_SIZE] = { 0 };
	union power_supply_propval prop = { 0 };
	int usb_present = 0;
	int ret;

	mca_log_info("wireless delay report status work\n");

	prop.intval = charger->wls_active;
	ret = power_supply_set_property(charger->wls_psy_info->wireless_psy,
					POWER_SUPPLY_PROP_ONLINE, &prop);
	if (ret < 0)
		mca_log_err("failed to set wls_online");
	business_charger_update_wireless_psy(charger->wls_psy_info);

	strscpy(event, tx_adapter_zero, sizeof(event));
	event_data.event = event;
	event_data.event_len = sizeof(tx_adapter_zero) - 1;
	mca_event_report_uevent(&event_data);

	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				  MCA_EVENT_WIRELESS_DISCONNECT,
				  charger->wls_active);

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				     STRATEGY_STATUS_TYPE_ONLINE, &usb_present);
	if (usb_present)
		return;

	__pm_relax(charger->online_wake_lock);
	business_charger_report_quick_charge_type_zero();
}

static void business_charger_wireless_delay_enable_rx_work(struct work_struct *work)
{
	int usb_online = 0;

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				     STRATEGY_STATUS_TYPE_ONLINE, &usb_online);
	if (!usb_online)
		platform_class_wireless_set_enable_mode(WIRELESS_ROLE_MASTER, true);

	mca_log_info("delay_enable_rx when usb out\n");
}

/* Power cycles the wireless receiver once the PMIC is up, unless on USB. */
static void business_charger_wireless_reset_rx_work(struct work_struct *work)
{
	int usb_present = 0;

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				     STRATEGY_STATUS_TYPE_ONLINE, &usb_present);
	mca_log_info("get usb_present %d\n", usb_present);
	if (!usb_present) {
		platform_class_wireless_set_enable_mode(WIRELESS_ROLE_MASTER, false);
		usleep_range(125000, 150000);
		platform_class_wireless_set_enable_mode(WIRELESS_ROLE_MASTER, true);
	}
}

static void business_charger_report_wired_quick_charge_type(struct business_charger *charger,
							    int icon_type)
{
	struct mca_event_notify_data event_data = { 0 };
	char event[MCA_EVENT_NOTIFY_SIZE] = { 0 };

	charger->wired_qucik_charge_type = icon_type;

	if (charger->abnormal_info.asuint32) {
		mca_log_info("force normal icon 0x%x", charger->abnormal_info.asuint32);
		icon_type = ADP_ICON_TYPE_NORMAL;
	}

	event_data.event_len = snprintf(event, MCA_EVENT_NOTIFY_SIZE,
					"POWER_SUPPLY_QUICK_CHARGE_TYPE=%d",
					icon_type);
	event_data.event = event;
	mca_event_report_uevent(&event_data);
}

/* The icon of a verified PD adapter follows the power it advertises. */
static void business_charger_report_quick_charge_type_work(struct work_struct *work)
{
	struct business_charger *charger = container_of(work,
		struct business_charger, report_quick_charge_type_work.work);
	int power_max = 0;
	int icon_type;

	if (charger->real_type != XM_CHARGER_TYPE_PD_VERIFY)
		return;

	protocol_class_get_adapter_max_power(ADAPTER_PROTOCOL_PPS,
					     (u32 *)&power_max);
	charger->wired_power_max = power_max;
	if (power_max >= QUICK_CHG_TYPE_SUPER_CHG_POWER)
		icon_type = ADP_ICON_TYPE_SUPER;
	else if (power_max >= QUICK_CHG_TYPE_TURBO_CHG_POWER)
		icon_type = ADP_ICON_TYPE_TURBO;
	else if (power_max >= QUICK_CHG_TYPE_FLASH_CHG_POWER)
		icon_type = ADP_ICON_TYPE_FLASH;
	else
		icon_type = ADP_ICON_TYPE_FAST;

	mca_log_info("power_max: %d, icon_type: %d\n", power_max, icon_type);
	business_charger_report_wired_quick_charge_type(charger, icon_type);
}

static void business_charger_update_wired_quick_charge_type(struct business_charger *charger)
{
	union power_supply_propval pval = { 0 };
	u32 power_max = 0;
	int icon_type = ADP_ICON_TYPE_NORMAL;

	if (!charger->bc12_active)
		return;

	if (charger->usb_psy_info && charger->usb_psy_info->batt_psy)
		power_supply_get_property(charger->usb_psy_info->batt_psy,
					  POWER_SUPPLY_PROP_HEALTH, &pval);
	if (pval.intval == POWER_SUPPLY_HEALTH_OVERHEAT) {
		charger->abnormal_info.batt_overhot = 1;
	} else if (pval.intval == POWER_SUPPLY_HEALTH_COLD) {
		charger->abnormal_info.batt_cold = 1;
	} else {
		charger->abnormal_info.batt_cold = 0;
		charger->abnormal_info.batt_overhot = 0;
	}

	switch (charger->real_type) {
	case XM_CHARGER_TYPE_HVDCP2:
	case XM_CHARGER_TYPE_HVDCP3:
	case XM_CHARGER_TYPE_PD:
		icon_type = ADP_ICON_TYPE_FAST;
		break;
	case XM_CHARGER_TYPE_HVDCP3_B:
	case XM_CHARGER_TYPE_HVDCP3P5:
		icon_type = ADP_ICON_TYPE_FLASH;
		break;
	case XM_CHARGER_TYPE_PPS:
	case XM_CHARGER_TYPE_PD_VERIFY:
		icon_type = ADP_ICON_TYPE_FAST;
		protocol_class_get_adapter_max_power(ADAPTER_PROTOCOL_PPS,
						     &power_max);
		charger->wired_power_max = power_max;
		break;
	default:
		break;
	}

	business_charger_report_wired_quick_charge_type(charger, icon_type);
}

static void business_charger_process_wireless_online_change(struct business_charger *charger,
							    unsigned int event)
{
	static int wls_active_flag;
	union power_supply_propval prop = { 0 };
	int ret;

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
				     STRATEGY_STATUS_TYPE_ONLINE,
				     &charger->wls_active);
	mca_log_info("wireless online %d\n", charger->wls_active);
	if (wls_active_flag == charger->wls_active)
		return;

	wls_active_flag = charger->wls_active;
	if (!wls_active_flag) {
		/* The wake lock is dropped by the delayed status report. */
		charger->wls_adapter_type = 0;
		mca_log_info("relax wake lock\n");
		mca_charge_mievent_set_state(MIEVENT_STATE_PLUG, 0);
		event = MCA_EVENT_WIRELESS_DISCONNECT;
	} else if (!charger->dam_test_flag) {
		mca_log_info("stay wake lock\n");
		__pm_stay_awake(charger->online_wake_lock);
		event = MCA_EVENT_WIRELESS_CONNECT;
	}

	if (wls_active_flag) {
		prop.intval = charger->wls_active;
		ret = power_supply_set_property(charger->wls_psy_info->wireless_psy,
						POWER_SUPPLY_PROP_ONLINE, &prop);
		if (ret < 0)
			mca_log_err("failed to set wls_online");
		cancel_delayed_work_sync(&charger->delay_report_status_work);
		business_charger_update_wireless_psy(charger->wls_psy_info);
		mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event,
					  wls_active_flag);
	} else {
		schedule_delayed_work(&charger->delay_report_status_work,
				      msecs_to_jiffies(WLS_REPORT_STATUS_DELAY_MS));
	}

	mca_strategy_func_process(STRATEGY_FUNC_TYPE_REV_WIRELESS, event, wls_active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BASIC_WIRELESS, event, wls_active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_WIRELESS, event, wls_active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_JEITA, event, wls_active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_FG, event, wls_active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_SMARTCHG, event, wls_active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_THERMAL, event, wls_active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BMD, event, wls_active_flag);
}

/*
 * USB presence also drives the wireless receiver: it is disabled while USB
 * is present and re-enabled a while after removal (unless its firmware is
 * being updated).
 */
static noinline void business_charger_process_usb_sns_func(struct business_charger *charger,
							   unsigned int event,
							   int *data)
{
	int usb_present = *data;
	bool fw_update = false;

	mca_wireless_rev_get_fw_update(&fw_update);
	mca_log_err("usb_resent: %d, fw_update: %d\n", usb_present, fw_update);

	mca_strategy_func_process(STRATEGY_FUNC_TYPE_REV_WIRELESS, event, usb_present);

	if (usb_present)
		platform_class_wireless_set_enable_mode(WIRELESS_ROLE_MASTER, false);
	else if (!fw_update)
		schedule_delayed_work(&charger->delay_enable_rx_work,
				      msecs_to_jiffies(WLS_ENABLE_RX_DELAY_MS));

	mca_wireless_rev_set_usb_plugin(usb_present != 0);
}

static void business_charger_process_online_change(struct business_charger *charger,
						   unsigned int event)
{
	static int active_flag;
	union power_supply_propval pval = { 0 };

	power_supply_get_property(charger->wls_psy_info->wireless_psy,
				  POWER_SUPPLY_PROP_ONLINE, &pval);

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				     STRATEGY_STATUS_TYPE_ONLINE,
				     &charger->bc12_active);
	mca_log_info("online %d\n", charger->bc12_active);

	active_flag = charger->bc12_active;
	if (!active_flag) {
		charger->pd_type = 0;
		charger->real_type = 0;
		charger->dam_test_flag = 0;
		charger->wired_qucik_charge_type = 0;
		charger->wired_power_max = 0;
		__pm_relax(charger->online_wake_lock);
		mca_charge_mievent_set_state(MIEVENT_STATE_PLUG, 0);
		mca_log_info("relax wake lock\n");
		event = MCA_EVENT_USB_DISCONNECT;
	} else if (!charger->dam_test_flag) {
		mca_log_info("stay wake lock\n");
		__pm_stay_awake(charger->online_wake_lock);
		event = MCA_EVENT_USB_CONNECT;
	}

	update_usb_pd_type(charger);
	business_charger_process_usb_sns_func(charger, event, &active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_JEITA, event, active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_FG, event, active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_SMARTCHG, event, active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_THERMAL, event, active_flag);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BMD, event, active_flag);

	if (active_flag)
		mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					  MCA_EVENT_CHARGE_TYPE_CHANGE,
					  charger->real_type);
	else if (!pval.intval)
		business_charger_report_quick_charge_type_zero();
}

/*
 * handle_state and stop_handle_charge are written by userspace for a
 * charging handle accessory.  Only a change of either decides whether the
 * USB port may charge: allow_charge is -1 when nothing changed.
 */
static noinline void business_charger_process_handle_logic(struct business_charger *charger)
{
	static int last_handle_state;
	static int last_stop_handle_charge;
	int handle_state = charger->handle_state;
	int stop_handle_charge = charger->stop_handle_charge;
	int allow_charge;

	if (last_stop_handle_charge && !stop_handle_charge && handle_state)
		allow_charge = 0;
	else if (stop_handle_charge && last_stop_handle_charge != stop_handle_charge)
		allow_charge = 1;
	else
		allow_charge = -1;

	if (!handle_state && last_handle_state)
		allow_charge = 1;
	if (!stop_handle_charge && handle_state &&
	    last_handle_state != handle_state)
		allow_charge = 0;

	mca_log_info("last_allow_charge = %d, last_handle_state = %d, last_stop_handle_charge = %d, allow_charge = %d, handle_state = %d, stop_handle_charge = %d\n",
		     charger->usb_psy_info->last_allow_charge, last_handle_state,
		     last_stop_handle_charge, allow_charge, handle_state,
		     stop_handle_charge);

	last_handle_state = charger->handle_state;
	last_stop_handle_charge = charger->stop_handle_charge;

	if (allow_charge == -1 ||
	    allow_charge == charger->usb_psy_info->last_allow_charge)
		return;

	charger->usb_psy_info->last_allow_charge = allow_charge;
	business_usb_psy_event_process(charger->usb_psy_info);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				  MCA_EVENT_HANDLE_ALLOW_CHARGE, allow_charge);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
				  MCA_EVENT_HANDLE_ALLOW_CHARGE, allow_charge);

	if (!allow_charge) {
		if (charger->real_type)
			business_charger_report_quick_charge_type_zero();
	} else {
		if (charger->real_type != XM_CHARGER_TYPE_PD_VERIFY)
			business_charger_update_wired_quick_charge_type(charger);
		if (charger->bc12_active) {
			mca_log_info("stay wake lock\n");
			__pm_stay_awake(charger->online_wake_lock);
			return;
		}
	}

	mca_log_info("relax wake lock\n");
	__pm_relax(charger->online_wake_lock);
}

/*
 * Debug controls, written as "<name> <value>..." to debug_ctrl.  The values
 * in force are kept here; they change once the event thread has applied a
 * request.
 */
#define DEBUG_CTRL_NAME_LEN	20
#define DEBUG_CTRL_VAL_MAX	5
#define DEBUG_CTRL_SOC_MAX	100
#define DEBUG_CTRL_SOC_SHIFT	8
#define DEBUG_CTRL_SOC_MASK	0xff

struct business_charger_debug_ctrl {
	char name[DEBUG_CTRL_NAME_LEN];
	int argc;
	int val[DEBUG_CTRL_VAL_MAX];
};

enum debug_ctrl_index {
	DEBUG_CTRL_INDEX_DOUBLE85,
	DEBUG_CTRL_INDEX_REMOVE_TEMP_LIMIT,
	DEBUG_CTRL_INDEX_SOC_LIMIT,
	DEBUG_CTRL_INDEX_MEMORY_TEST,
	DEBUG_CTRL_INDEX_MAX,
};

static struct business_charger_debug_ctrl g_debug_ctrl[DEBUG_CTRL_INDEX_MAX] = {
	[DEBUG_CTRL_INDEX_DOUBLE85]		= { "double85", 1 },
	[DEBUG_CTRL_INDEX_REMOVE_TEMP_LIMIT]	= { "remove_temp_limit", 1 },
	[DEBUG_CTRL_INDEX_SOC_LIMIT]		= { "soc_limit", 2 },
	[DEBUG_CTRL_INDEX_MEMORY_TEST]		= { "memory_test", 1 },
};

static bool business_charger_debug_ctrl_named(const char *buf, int id)
{
	const char *name = g_debug_ctrl[id].name;

	return !strncmp(buf, name, strnlen(name, DEBUG_CTRL_NAME_LEN));
}

/*
 * A new value is sent as an event (applied by the event thread) and saved
 * in the charger partition so it survives a reboot.
 */
static void business_charger_process_debug_ctrl_input(const char *buf)
{
	char name[DEBUG_CTRL_NAME_LEN] = { 0 };
	int val = 0, stop = 0;
	int packed;

	mca_log_err("debug_ctrl input: %s\n", buf);

	if (business_charger_debug_ctrl_named(buf, DEBUG_CTRL_INDEX_DOUBLE85)) {
		if (sscanf(buf, "%19s %d", name, &val) != 2) {
			mca_log_err("invalid input\n");
			return;
		}
		if (val == g_debug_ctrl[DEBUG_CTRL_INDEX_DOUBLE85].val[0])
			return;
		mca_event_block_notify(MCA_EVENT_TYPE_DEBUG,
				       MCA_EVENT_DEBUG_CTRL_DOUBLE85, &val);
		charger_partition_write_double85(val);
	} else if (business_charger_debug_ctrl_named(buf,
						     DEBUG_CTRL_INDEX_REMOVE_TEMP_LIMIT)) {
		if (sscanf(buf, "%19s %d", name, &val) != 2) {
			mca_log_err("invalid input\n");
			return;
		}
		if (val == g_debug_ctrl[DEBUG_CTRL_INDEX_REMOVE_TEMP_LIMIT].val[0])
			return;
		mca_event_block_notify(MCA_EVENT_TYPE_DEBUG,
				       MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT, &val);
		charger_partition_write_remove_temp_limit(val);
	} else if (business_charger_debug_ctrl_named(buf, DEBUG_CTRL_INDEX_SOC_LIMIT)) {
		if (sscanf(buf, "%19s %d %d", name, &val, &stop) != 3) {
			mca_log_err("invalid input\n");
			return;
		}
		if (val < 0 || val > DEBUG_CTRL_SOC_MAX ||
		    stop < 0 || stop > DEBUG_CTRL_SOC_MAX) {
			mca_log_err("invalid soc value: %d %d\n", val, stop);
			return;
		}
		if (val == g_debug_ctrl[DEBUG_CTRL_INDEX_SOC_LIMIT].val[0] &&
		    stop == g_debug_ctrl[DEBUG_CTRL_INDEX_SOC_LIMIT].val[1])
			return;
		packed = (val << DEBUG_CTRL_SOC_SHIFT) | stop;
		mca_event_block_notify(MCA_EVENT_TYPE_DEBUG,
				       MCA_EVENT_DEBUG_CTRL_SOC_LIMIT, &packed);
		charger_partition_write_soc_limit(packed);
	} else if (business_charger_debug_ctrl_named(buf, DEBUG_CTRL_INDEX_MEMORY_TEST)) {
		if (sscanf(buf, "%19s %d", name, &val) != 2) {
			mca_log_err("invalid input\n");
			return;
		}
		if (val == g_debug_ctrl[DEBUG_CTRL_INDEX_MEMORY_TEST].val[0])
			return;
		mca_event_block_notify(MCA_EVENT_TYPE_DEBUG,
				       MCA_EVENT_DEBUG_CTRL_MEMORY_TEST, &val);
		charger_partition_write_memory_test(val);
	} else {
		mca_log_err("invalid name\n");
	}
}

/*
 * double85, remove_temp_limit and memory_test lift thermal limits: thermal,
 * the gauge (and the buck strategy for memory_test) are told, and the ADSP
 * too-hot limit is disabled while one is set.
 */
static void business_charger_process_debug_ctrl(unsigned int event, int value,
						int id)
{
	if (value == g_debug_ctrl[id].val[0])
		return;

	g_debug_ctrl[id].val[0] = value;
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_THERMAL, event, value);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_FG, event, value);
	if (id == DEBUG_CTRL_INDEX_MEMORY_TEST)
		mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, value);
	platform_class_buckchg_ops_set_too_hot_limit(MAIN_BUCK_CHARGER, !value);
}

static void business_charger_process_debug_ctrl_soc_limit(unsigned int event,
							  u32 packed)
{
	u32 start = packed >> DEBUG_CTRL_SOC_SHIFT;
	u32 stop = packed & DEBUG_CTRL_SOC_MASK;

	if (start > DEBUG_CTRL_SOC_MAX || stop > DEBUG_CTRL_SOC_MAX) {
		mca_log_err("soc_limit value is invalid: %d\n", packed);
		return;
	}

	if (start == g_debug_ctrl[DEBUG_CTRL_INDEX_SOC_LIMIT].val[0] &&
	    stop == g_debug_ctrl[DEBUG_CTRL_INDEX_SOC_LIMIT].val[1])
		return;

	g_debug_ctrl[DEBUG_CTRL_INDEX_SOC_LIMIT].val[0] = start;
	g_debug_ctrl[DEBUG_CTRL_INDEX_SOC_LIMIT].val[1] = stop;
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, packed);
}

static void business_charger_process_is_eu_model(struct business_charger *charger,
						 int value)
{
	mca_log_info("is_eu_model: %d\n", value);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_FG, MCA_EVENT_IS_EU_MODEL, value);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, MCA_EVENT_IS_EU_MODEL, value);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, MCA_EVENT_IS_EU_MODEL, value);
}

static void business_charger_process_plate_shock(struct business_charger *charger,
						 int value)
{
	mca_log_info("plate_shock: %d\n", value);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
				  MCA_EVENT_PLATE_SHOCK, value == 1);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				  MCA_EVENT_PLATE_SHOCK, value == 1);
}

static void business_charger_process_start_quick_revchg(struct business_charger *charger,
							int value)
{
	mca_log_info("start_quick_revchg: %d\n", value);
	if (!value) {
		mca_log_info("user cancel start_quick_revchg, do nothing\n");
		return;
	}

	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				  MCA_EVENT_START_QUICK_REVCHG, value);
}

static void business_charger_process_set_revchg_bcl(struct business_charger *charger,
						    int value)
{
	mca_log_info("set_revchg_bcl: %d\n", value);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				  MCA_EVENT_REVCHG_BCL, value);
}

static void business_charger_process_cap_change(struct business_charger *charger,
						unsigned int event)
{
	unsigned long delay = 0;

	if (charger->real_type != XM_CHARGER_TYPE_PD_VERIFY)
		delay = msecs_to_jiffies(REPORT_QC_TYPE_DELAY_MS);

	cancel_delayed_work_sync(&charger->report_quick_charge_type_work);
	schedule_delayed_work(&charger->report_quick_charge_type_work, delay);

	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, 0);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, 0);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_THERMAL, event, 0);
}

static void business_charger_process_type_change(struct business_charger *charger,
						 unsigned int event)
{
	update_usb_pd_type(charger);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, charger->real_type);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, charger->real_type);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_FG, event, charger->real_type);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_THERMAL, event, charger->real_type);
	if (charger->real_type != XM_CHARGER_TYPE_PD_VERIFY)
		business_charger_update_wired_quick_charge_type(charger);
}

static void business_charger_process_usb_suspend_change(struct business_charger *charger,
							bool usb_suspend)
{
	if (usb_suspend) {
		__pm_relax(charger->online_wake_lock);
		mca_log_info("relax wake lock\n");
	} else if (charger->bc12_active) {
		__pm_stay_awake(charger->online_wake_lock);
		mca_log_info("stay wake lock\n");
	}
}

static void business_charger_process_verify_process_change(struct business_charger *charger,
							   unsigned int event,
							   int verify_process_end)
{
	mca_log_info("pd verify process done: %d\n", !verify_process_end);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event,
				  !verify_process_end);
}

static void business_charger_process_batt_health_change(struct business_charger *charger,
							unsigned int event)
{
	business_charger_update_wired_quick_charge_type(charger);
	if (charger->real_type == XM_CHARGER_TYPE_PD_VERIFY)
		schedule_delayed_work(&charger->report_quick_charge_type_work, 0);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BASIC_WIRELESS, event, 0);
}

static void business_charger_mievent_report_cp_vbat_ovp(void)
{
	int event_data[2] = { 0 };

	strategy_class_fg_ops_get_voltage(&event_data[0]);
	platform_class_cp_get_battery_voltage(CP_ROLE_MASTER, &event_data[1]);
	mca_charge_mievent_report(CHARGE_DFX_CP_VBAT_OVP, event_data, 2);
}

static void business_charger_process_cp_cboot_short(struct business_charger *charger,
						    unsigned int event, int value)
{
	mca_log_info("occur cp cboot short: %d\n", value);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, value);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, value);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_REV_WIRELESS, event, value);
}

static void business_charger_process_cp_iic_error(struct business_charger *charger,
						  unsigned int event)
{
	int event_data[2] = { 0 };

	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, 0);
	mca_charge_mievent_report(CHARGE_DFX_CP_ABSENT, event_data, 2);
}

static void business_charger_process_cp_tshut(struct business_charger *charger,
					      int *tdie)
{
	if (charger->cp_type == BUSINESS_CHARGER_CP_TYPE_PARALLEL) {
		platform_class_cp_get_tdie(CP_ROLE_MASTER, &tdie[0]);
		platform_class_cp_get_tdie(CP_ROLE_SLAVE, &tdie[1]);
	}
	mca_charge_mievent_report(CHARGE_DFX_CP_TDIE_HOT, tdie, 2);
}

static void business_charger_process_antiburn_change(struct business_charger *charger,
						     unsigned int event)
{
	int antiburn = connector_antiburn_is_triggered();

	mca_log_info("antiburn: %d\n", antiburn);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, antiburn);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, antiburn);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_REV_WIRELESS, event, antiburn);
}

static void business_charger_process_batt_btb_change(struct business_charger *charger,
						     unsigned int event,
						     bool batt_missing)
{
	mca_log_info("batt_missing: %d\n", batt_missing);
	charger->abnormal_info.batt_missing = batt_missing;
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, batt_missing);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, batt_missing);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_REV_WIRELESS, event, batt_missing);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BASIC_WIRELESS, event, batt_missing);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_WIRELESS, event, batt_missing);
}

static void business_charger_process_batt_auth_pass(struct business_charger *charger,
						    unsigned int event)
{
	mca_log_err("reviced batt_auth pass\n");
	charger->abnormal_info.batt_auth_failed = 0;
	business_charger_update_wired_quick_charge_type(charger);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, 1);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, 1);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BASIC_WIRELESS, event, 1);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_WIRELESS, event, 1);
}

static void business_charger_process_lpd_status_change(struct business_charger *charger,
						       unsigned int event,
						       int is_liquited)
{
	int force_5v_charging = lpd_is_charging_limit() & is_liquited;

	mca_log_info("%s: occur liquited need drop charging power: %d\n",
		     __func__, force_5v_charging);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, force_5v_charging);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, force_5v_charging);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_REV_WIRELESS, event, is_liquited);
	mca_charge_mievent_report(CHARGE_DFX_LPD_DETECTED, &is_liquited, 1);
}

static void business_charger_process_pmic_init_done(struct business_charger *charger,
						    unsigned int event)
{
	mca_log_err("pmic init done\n");
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, 1);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BASIC_WIRELESS, event, 1);
	if (charger->support_wls)
		schedule_delayed_work(&charger->reset_rx_work, 0);
}

static void business_charger_process_rp_short_vbus_change(struct business_charger *charger,
							  unsigned int event,
							  int cc_short_vbus)
{
	mca_log_info("occur cc short vbus: %d\n", cc_short_vbus);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, cc_short_vbus);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, cc_short_vbus);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_REV_WIRELESS, event, cc_short_vbus);
	if (cc_short_vbus)
		mca_charge_mievent_report(CHARGE_DFX_RP_SHORT_VBUS_DETECTED, NULL, 0);
}

static void business_charger_process_vbat_ovp_status_change(struct business_charger *charger,
							    unsigned int event,
							    int vbat_ovp_status)
{
	mca_log_info("occur vbat_ovp_status: %d\n", vbat_ovp_status);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, vbat_ovp_status);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, vbat_ovp_status);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BASIC_WIRELESS, event, vbat_ovp_status);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_WIRELESS, event, vbat_ovp_status);
}

static void business_charger_process_ibat_ocp_status_change(struct business_charger *charger,
							    unsigned int event,
							    int ibat_ocp_status)
{
	mca_log_info("occur ibat_ocp_status: %d\n", ibat_ocp_status);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, ibat_ocp_status);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_WIRELESS, event, ibat_ocp_status);
}

static void business_charger_process_quick_revchg(struct business_charger *charger,
						  unsigned int event, int value)
{
	mca_log_info("handle quick revchg changed: %#x\n", value);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, value);
}

static void business_charger_process_chg_sts_change(struct business_charger *charger,
						    unsigned int event)
{
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, 0);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, 0);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BASIC_WIRELESS, event, 0);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_WIRELESS, event, 0);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_REV_WIRELESS, event, 0);
}

static void business_charger_process_soc_limit_change(struct business_charger *charger,
						      unsigned int event,
						      int smartchg_pmic)
{
	mca_log_info("smartchg_pmic: %d\n", smartchg_pmic);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, smartchg_pmic);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, smartchg_pmic);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BASIC_WIRELESS, event, smartchg_pmic);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_WIRELESS, event, smartchg_pmic);
}

static void business_charger_process_dtpt_change(struct business_charger *charger,
						 unsigned int event,
						 int dtpt_status)
{
	mca_log_info("dtpt_status: %d\n", dtpt_status);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_SMARTCHG, event, dtpt_status);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_JEITA, event, dtpt_status);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, dtpt_status);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_WIRELESS, event, dtpt_status);
}

static void business_charger_process_csd_pulse_change(struct business_charger *charger,
						      unsigned int event,
						      int *data)
{
	int csd_pulse;

	if (charger->real_type != XM_CHARGER_TYPE_PD_VERIFY) {
		mca_log_info("not standard pd verify, ignore csd pulse event\n");
		return;
	}

	csd_pulse = *data;
	mca_log_info("receive csd pulse request: %d\n", csd_pulse);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, csd_pulse);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event, csd_pulse);
}

static void business_charger_process_snk_power_suspend(struct business_charger *charger,
						       unsigned int event,
						       bool bsinkpowersuspend)
{
	mca_log_info("bsinkpowersuspend: %d\n", bsinkpowersuspend);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event,
				  bsinkpowersuspend);
}

static void business_charger_process_fg_ota(struct business_charger *charger,
					    unsigned int event, int fg_ota_process)
{
	mca_log_info("fg_ota_process: %d\n", fg_ota_process);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE, event, fg_ota_process);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_WIRELESS, event, fg_ota_process);
}

/* Events whose payload is read; add_event_node may have failed to copy it. */
static bool business_charger_event_has_data(unsigned int event)
{
	switch (event) {
	case MCA_EVENT_WIRELESS_REVCHG:
	case MCA_EVENT_CHARGE_VERIFY_PROCESS_END:
	case MCA_EVENT_CP_CBOOT_FAIL:
	case MCA_EVENT_CP_TSHUT_FLAG:
	case MCA_EVENT_BATT_BTB_CHANGE:
	case MCA_EVENT_LPD_STATUS_CHANGE:
	case MCA_EVENT_CC_SHORT_VBUS:
	case MCA_EVENT_VBAT_OVP_CHANGE:
	case MCA_EVENT_IBAT_OCP_CHANGE:
	case MCA_EVENT_QUICK_REVCHG_CHANGE:
	case MCA_EVENT_SOC_LIMIT:
	case MCA_EVENT_BATTERY_DTPT:
	case MCA_EVENT_CSD_SEND_PULSE:
	case MCA_EVENT_SINK_PWR_SUSPEND_CHANGE:
	case MCA_EVENT_USB_SUSPEND:
	case MCA_EVENT_FG_OTA_PROCESS:
	case MCA_EVENT_DEBUG_CTRL_DOUBLE85:
	case MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT:
	case MCA_EVENT_DEBUG_CTRL_MEMORY_TEST:
	case MCA_EVENT_DEBUG_CTRL_SOC_LIMIT:
		return true;
	default:
		return false;
	}
}

static void business_charger_handle_event(struct business_charger *charger,
					  unsigned int event, void *data)
{
	if (business_charger_event_has_data(event) && !data) {
		mca_log_err("event %u without data\n", event);
		return;
	}

	switch (event) {
	case MCA_EVENT_USB_DISCONNECT:
	case MCA_EVENT_USB_CONNECT:
		business_charger_process_online_change(charger, event);
		break;
	case MCA_EVENT_WIRELESS_DISCONNECT:
	case MCA_EVENT_WIRELESS_CONNECT:
		business_charger_process_wireless_online_change(charger, event);
		break;
	case MCA_EVENT_WIRELESS_REVCHG:
		mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE, event,
					  *(bool *)data);
		break;
	case MCA_EVENT_CHARGE_TYPE_CHANGE:
		business_charger_process_type_change(charger, event);
		break;
	case MCA_EVENT_CHARGE_CAP_CHANGE:
		business_charger_process_cap_change(charger, event);
		break;
	case MCA_EVENT_CHARGE_VERIFY_PROCESS_END:
		mca_log_info("receive verify_process_end notify: %d\n", *(int *)data);
		business_charger_process_verify_process_change(charger, event,
							       *(int *)data);
		break;
	case MCA_EVENT_BATTERY_HEALTH_CHANGE:
		business_charger_process_batt_health_change(charger, event);
		break;
	case MCA_EVENT_CP_VUSB_OVP:
		mca_charge_mievent_report(CHARGE_DFX_CP_VAC_OVP, NULL, 0);
		break;
	case MCA_EVENT_CP_VBAT_OVP:
		business_charger_mievent_report_cp_vbat_ovp();
		break;
	case MCA_EVENT_CP_VBUS_OVP:
		mca_charge_mievent_report(CHARGE_DFX_CP_VBUS_OVP, NULL, 0);
		break;
	case MCA_EVENT_CP_IBAT_OCP:
		mca_charge_mievent_report(CHARGE_DFX_CP_IBAT_OCP, NULL, 0);
		break;
	case MCA_EVENT_CP_IBUS_OCP:
		mca_charge_mievent_report(CHARGE_DFX_CP_IBUS_OCP, NULL, 0);
		break;
	case MCA_EVENT_CP_CBOOT_FAIL:
		business_charger_process_cp_cboot_short(charger, event, *(int *)data);
		break;
	case MCA_EVENT_CP_IIC_ERROR:
		business_charger_process_cp_iic_error(charger, event);
		break;
	case MCA_EVENT_CP_TSHUT_FLAG:
		business_charger_process_cp_tshut(charger, data);
		break;
	case MCA_EVENT_CONN_ANTIBURN_CHANGE:
		business_charger_process_antiburn_change(charger, event);
		break;
	case MCA_EVENT_BATT_BTB_CHANGE:
		business_charger_process_batt_btb_change(charger, event, *(bool *)data);
		break;
	case MCA_EVENT_BATT_AUTH_PASS:
		business_charger_process_batt_auth_pass(charger, event);
		break;
	case MCA_EVENT_LPD_STATUS_CHANGE:
		business_charger_process_lpd_status_change(charger, event, *(int *)data);
		break;
	case MCA_EVENT_PMIC_INIT_DONE:
		business_charger_process_pmic_init_done(charger, event);
		break;
	case MCA_EVENT_CC_SHORT_VBUS:
		business_charger_process_rp_short_vbus_change(charger, event, *(int *)data);
		break;
	case MCA_EVENT_VBAT_OVP_CHANGE:
		business_charger_process_vbat_ovp_status_change(charger, event, *(int *)data);
		break;
	case MCA_EVENT_IBAT_OCP_CHANGE:
		business_charger_process_ibat_ocp_status_change(charger, event, *(int *)data);
		break;
	case MCA_EVENT_QUICK_REVCHG_CHANGE:
		business_charger_process_quick_revchg(charger, event, *(int *)data);
		break;
	case MCA_EVENT_USB_STS_CHANGE:
		business_usb_psy_event_process(charger->usb_psy_info);
		break;
	case MCA_EVENT_CHARGE_ABNORMAL:
	case MCA_EVENT_CHARGE_RESTORE:
		business_charger_process_chg_sts_change(charger, event);
		break;
	case MCA_EVENT_SOC_LIMIT:
		business_charger_process_soc_limit_change(charger, event, *(int *)data);
		break;
	case MCA_EVENT_BATTERY_DTPT:
		business_charger_process_dtpt_change(charger, event, *(int *)data);
		break;
	case MCA_EVENT_CSD_SEND_PULSE:
		business_charger_process_csd_pulse_change(charger, event, data);
		break;
	case MCA_EVENT_SINK_PWR_SUSPEND_CHANGE:
		business_charger_process_snk_power_suspend(charger, event, *(bool *)data);
		break;
	case MCA_EVENT_USB_SUSPEND:
		business_charger_process_usb_suspend_change(charger, *(bool *)data);
		break;
	case MCA_EVENT_FG_OTA_PROCESS:
		business_charger_process_fg_ota(charger, event, *(int *)data);
		break;
	case MCA_EVENT_DEBUG_CTRL_DOUBLE85:
		business_charger_process_debug_ctrl(event, *(int *)data,
						    DEBUG_CTRL_INDEX_DOUBLE85);
		break;
	case MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT:
		business_charger_process_debug_ctrl(event, *(int *)data,
						    DEBUG_CTRL_INDEX_REMOVE_TEMP_LIMIT);
		break;
	case MCA_EVENT_DEBUG_CTRL_MEMORY_TEST:
		business_charger_process_debug_ctrl(event, *(int *)data,
						    DEBUG_CTRL_INDEX_MEMORY_TEST);
		break;
	case MCA_EVENT_DEBUG_CTRL_SOC_LIMIT:
		business_charger_process_debug_ctrl_soc_limit(event, *(u32 *)data);
		break;
	default:
		break;
	}
}

static void business_charger_process_event(struct business_charger *charger)
{
	struct charger_event_lis_node *event_node, *temp_node;

	while (!list_empty(&charger->header)) {
		spin_lock(&charger->list_lock);
		list_for_each_entry_safe(event_node, temp_node, &charger->header, lnode) {
			list_del(&event_node->lnode);
			spin_unlock(&charger->list_lock);

			mca_log_info("event_node->event: %d\n", event_node->event);
			business_charger_handle_event(charger, event_node->event,
						      event_node->data);

			spin_lock(&charger->list_lock);
			kfree(event_node->data);
			kfree(event_node);
		}
		spin_unlock(&charger->list_lock);
	}
}

static void business_charger_wireless_rerun_usb_sns_work(struct work_struct *work)
{
	struct business_charger *charger = container_of(work,
		struct business_charger, delay_rerun_usb_sns_work.work);
	int online = 0;

	mca_log_info("wireless rerun cp vusb work\n");
	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				     STRATEGY_STATUS_TYPE_ONLINE, &online);
	business_charger_process_usb_sns_func(charger,
					      online ? MCA_EVENT_USB_CONNECT :
						       MCA_EVENT_USB_DISCONNECT,
					      &online);
}

/*
 * Sends the PMIC init done handling once the buck (and, with wireless, the
 * basic wireless) strategy and the buck charger are ready.  If the charger
 * is not ready, the MCA_EVENT_PMIC_INIT_DONE event does it later.
 */
static void business_charger_pmic_init_done_notify_work(struct work_struct *work)
{
	struct business_charger *charger = container_of(work,
		struct business_charger, pmic_init_done_notify_work.work);
	int wls_init_done = 0, buckchg_init_done = 0;

	mca_log_info("pmic init done notify work\n");

	if (charger->support_wls)
		mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					     STRATEGY_STATUS_TYPE_INIT_OK,
					     &wls_init_done);
	else
		wls_init_done = 1;

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				     STRATEGY_STATUS_TYPE_INIT_OK,
				     &buckchg_init_done);

	if (!wls_init_done || !buckchg_init_done) {
		mca_log_err("wls_basic or buckchg not init done, rerun work\n");
		schedule_delayed_work(&charger->pmic_init_done_notify_work,
				      msecs_to_jiffies(PMIC_INIT_DONE_RETRY_MS));
		return;
	}

	if (platform_class_buckchg_ops_is_init_ok(MAIN_BUCK_CHARGER) <= 0) {
		mca_log_err("pmic not init done, return & wait event\n");
		return;
	}

	mca_log_err("pmic init done, process event\n");
	business_charger_process_pmic_init_done(charger, MCA_EVENT_PMIC_INIT_DONE);
}

static const char * const power_supply_usb_type_text[] = {
	"Unknown", "SDP", "CDP", "DCP", "USB_FLOAT", "HVDCP",
	"HVDCP_3", "HVDCP_3_B", "HVDCP_3P5", "C", "PD", "PD_PPS",
	"PD_PPS", "Unknown", "ACA", "DCP",
};

static const char *get_usb_type_name(u32 usb_type)
{
	if (usb_type < ARRAY_SIZE(power_supply_usb_type_text))
		return power_supply_usb_type_text[usb_type];

	return "Unknown";
}

enum bussiness_charger_attr_list {
	BUSSINESS_CHARGER_PROP_REAL_TYPE,
	BUSSINESS_CHARGER_PROP_POWER_MAX,
	BUSSINESS_CHARGER_PROP_QUICK_CHARGE_TYPE,
	BUSSINESS_CHARGER_PROP_IS_EU_MODEL,
	BUSSINESS_CHARGER_PROP_PLATE_SHOCK,
	BUSSINESS_CHARGER_PROP_REVERSE_QUICK_CHARGE,
	BUSSINESS_CHARGER_PROP_REVCHG_BCL,
	BUSSINESS_CHARGER_PROP_HANDLE_STATE,
	BUSSINESS_CHARGER_PROP_STOP_HADNLE_CHARGE,
	BUSSINESS_CHARGER_PROP_DEBUG_CTRL,
};

static ssize_t bussiness_charger_sysfs_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf);
static ssize_t bussiness_charger_sysfs_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count);

struct mca_sysfs_attr_info bussiness_charger_sysfs_field_tbl[] = {
	mca_sysfs_attr_ro(bussiness_charger_sysfs, 0444, BUSSINESS_CHARGER_PROP_REAL_TYPE, real_type),
	mca_sysfs_attr_ro(bussiness_charger_sysfs, 0444, BUSSINESS_CHARGER_PROP_POWER_MAX, power_max),
	mca_sysfs_attr_ro(bussiness_charger_sysfs, 0444, BUSSINESS_CHARGER_PROP_QUICK_CHARGE_TYPE, quick_charge_type),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664, BUSSINESS_CHARGER_PROP_IS_EU_MODEL, is_eu_model),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664, BUSSINESS_CHARGER_PROP_PLATE_SHOCK, plate_shock),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664, BUSSINESS_CHARGER_PROP_REVERSE_QUICK_CHARGE, reverse_quick_charge),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664, BUSSINESS_CHARGER_PROP_REVCHG_BCL, revchg_bcl),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664, BUSSINESS_CHARGER_PROP_HANDLE_STATE, handle_state),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664, BUSSINESS_CHARGER_PROP_STOP_HADNLE_CHARGE, stop_handle_charge),
	mca_sysfs_attr_rw(bussiness_charger_sysfs, 0664, BUSSINESS_CHARGER_PROP_DEBUG_CTRL, debug_ctrl),
};

#define BUSSINESS_CHARGER_SYSFS_ATTRS_SIZE	ARRAY_SIZE(bussiness_charger_sysfs_field_tbl)

static struct attribute *bussiness_charger_sysfs_attrs[BUSSINESS_CHARGER_SYSFS_ATTRS_SIZE + 1];

static const struct attribute_group bussiness_charger_sysfs_attr_group = {
	.attrs = bussiness_charger_sysfs_attrs,
};

static int business_charger_get_power_max(struct business_charger *charger)
{
	union power_supply_propval pval = { 0 };
	int power_max = 0;

	if (charger->bc12_active)
		return charger->wired_power_max;

	power_supply_get_property(charger->wls_psy_info->wireless_psy,
				  POWER_SUPPLY_PROP_ONLINE, &pval);
	if (pval.intval)
		mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					     STRATEGY_STATUS_TYPE_POWER_MAX,
					     &power_max);

	return power_max;
}

static int business_charger_get_quick_charge_type(struct business_charger *charger)
{
	int online = 0;
	int type = 0;

	if (charger->bc12_active)
		return charger->wired_qucik_charge_type;

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
				     STRATEGY_STATUS_TYPE_ONLINE, &online);
	if (online)
		mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					     STRATEGY_STATUS_TYPE_QC_TYPE, &type);

	return type;
}

/*
 * Only the EU model is stored from here.  The whole info 2 block is
 * written, with reserved[0] set to CHARGER_PARTITION_INFO_2_MAGIC.
 */
static int business_charger_partition_set_status(int type, int val)
{
	struct charger_partition_info_2 info_2 = { 0 };
	int ret;

	if (type != MCA_CHARGER_PARTITION_EU_MODE)
		return 0;

	ret = charger_partition_alloc(CHARGER_PARTITION_HOST_KERNEL,
				      CHARGER_PARTITION_INFO_2, sizeof(info_2));
	if (ret < 0) {
		mca_log_err("failed to alloc\n");
		return -1;
	}

	info_2.eu_mode = val;
	info_2.reserved[0] = CHARGER_PARTITION_INFO_2_MAGIC;
	ret = charger_partition_write(CHARGER_PARTITION_HOST_KERNEL,
				      CHARGER_PARTITION_INFO_2, &info_2,
				      sizeof(info_2));
	if (ret < 0) {
		mca_log_err("failed to write\n");
		if (charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL,
					      CHARGER_PARTITION_INFO_2,
					      sizeof(info_2)) < 0)
			mca_log_err("failed to dealloc\n");
		return -1;
	}
	mca_log_info("ret: %d, info_2.eu_mode: %u\n", ret, info_2.eu_mode);

	ret = charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL,
					CHARGER_PARTITION_INFO_2, sizeof(info_2));
	if (ret < 0) {
		mca_log_err("failed to dealloc\n");
		return -1;
	}

	return ret;
}

static ssize_t bussiness_charger_sysfs_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct business_charger *charger = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *attr_info;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  bussiness_charger_sysfs_field_tbl,
					  BUSSINESS_CHARGER_SYSFS_ATTRS_SIZE);
	if (!attr_info)
		return -EINVAL;

	switch (attr_info->sysfs_attr_name) {
	case BUSSINESS_CHARGER_PROP_IS_EU_MODEL:
		(void)sscanf(buf, "%d", &charger->is_eu_model);
		mca_log_info("is_eu_model set: %d\n", charger->is_eu_model);
		business_charger_partition_set_status(MCA_CHARGER_PARTITION_EU_MODE,
						      charger->is_eu_model);
		business_charger_process_is_eu_model(charger, charger->is_eu_model);
		break;
	case BUSSINESS_CHARGER_PROP_PLATE_SHOCK:
		(void)sscanf(buf, "%d", &charger->plate_shock);
		business_charger_process_plate_shock(charger, charger->plate_shock);
		break;
	case BUSSINESS_CHARGER_PROP_REVERSE_QUICK_CHARGE:
		(void)sscanf(buf, "%d", &charger->start_quick_revchg);
		business_charger_process_start_quick_revchg(charger,
							    charger->start_quick_revchg);
		break;
	case BUSSINESS_CHARGER_PROP_REVCHG_BCL:
		(void)sscanf(buf, "%d", &charger->revchg_bcl);
		mca_log_info("revchg_bcl set: %d\n", charger->revchg_bcl);
		business_charger_process_set_revchg_bcl(charger, charger->revchg_bcl);
		break;
	case BUSSINESS_CHARGER_PROP_HANDLE_STATE:
		(void)sscanf(buf, "%d", &charger->handle_state);
		business_charger_process_handle_logic(charger);
		break;
	case BUSSINESS_CHARGER_PROP_STOP_HADNLE_CHARGE:
		(void)sscanf(buf, "%d", &charger->stop_handle_charge);
		business_charger_process_handle_logic(charger);
		break;
	case BUSSINESS_CHARGER_PROP_DEBUG_CTRL:
		business_charger_process_debug_ctrl_input(buf);
		break;
	default:
		break;
	}

	return count;
}

static ssize_t bussiness_charger_sysfs_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct business_charger *charger = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *attr_info;
	ssize_t count = 0;
	int value;
	int i, j;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  bussiness_charger_sysfs_field_tbl,
					  BUSSINESS_CHARGER_SYSFS_ATTRS_SIZE);
	if (!attr_info)
		return -1;

	if (!charger)
		return 0;

	switch (attr_info->sysfs_attr_name) {
	case BUSSINESS_CHARGER_PROP_REAL_TYPE:
		count = snprintf(buf, PAGE_SIZE, "%s\n",
				 get_usb_type_name(charger->real_type));
		break;
	case BUSSINESS_CHARGER_PROP_POWER_MAX:
		value = business_charger_get_power_max(charger);
		mca_log_info("power_max: %d\n", value);
		count = snprintf(buf, PAGE_SIZE, "%d\n", value);
		break;
	case BUSSINESS_CHARGER_PROP_QUICK_CHARGE_TYPE:
		value = business_charger_get_quick_charge_type(charger);
		count = snprintf(buf, PAGE_SIZE, "%d\n", value);
		break;
	case BUSSINESS_CHARGER_PROP_IS_EU_MODEL:
		count = snprintf(buf, PAGE_SIZE, "%d\n", charger->is_eu_model);
		mca_log_info("get eu_model: %d\n", charger->is_eu_model);
		break;
	case BUSSINESS_CHARGER_PROP_PLATE_SHOCK:
		count = snprintf(buf, PAGE_SIZE, "%d\n", charger->plate_shock);
		mca_log_info("plate_shock: %d\n", charger->plate_shock);
		break;
	case BUSSINESS_CHARGER_PROP_REVERSE_QUICK_CHARGE:
		count = snprintf(buf, PAGE_SIZE, "%d\n", charger->start_quick_revchg);
		mca_log_info("start_quick_revchg: %d\n", charger->start_quick_revchg);
		break;
	case BUSSINESS_CHARGER_PROP_REVCHG_BCL:
		count = snprintf(buf, PAGE_SIZE, "%d\n", charger->revchg_bcl);
		mca_log_info("revchg_bcl: %d\n", charger->revchg_bcl);
		break;
	case BUSSINESS_CHARGER_PROP_HANDLE_STATE:
		count = snprintf(buf, PAGE_SIZE, "%d\n", charger->handle_state);
		mca_log_info("handle_state: %d\n", charger->handle_state);
		break;
	case BUSSINESS_CHARGER_PROP_STOP_HADNLE_CHARGE:
		count = snprintf(buf, PAGE_SIZE, "%d\n", charger->stop_handle_charge);
		mca_log_info("stop_handle_charge: %d\n", charger->stop_handle_charge);
		break;
	case BUSSINESS_CHARGER_PROP_DEBUG_CTRL:
		for (i = 0; i < DEBUG_CTRL_INDEX_MAX; i++) {
			count += scnprintf(buf + count, PAGE_SIZE - count, "%s",
					   g_debug_ctrl[i].name);
			for (j = 0; j < g_debug_ctrl[i].argc; j++)
				count += scnprintf(buf + count, PAGE_SIZE - count,
						   " %d", g_debug_ctrl[i].val[j]);
			count += scnprintf(buf + count, PAGE_SIZE - count, "\n");
		}
		break;
	default:
		break;
	}

	return count;
}

static int business_charger_sysfs_create_group(struct business_charger *charger)
{
	mca_sysfs_init_attrs(bussiness_charger_sysfs_attrs,
			     bussiness_charger_sysfs_field_tbl,
			     BUSSINESS_CHARGER_SYSFS_ATTRS_SIZE);
	return mca_sysfs_create_link_group(MCA_SYSFS_DEV_CHARGER, "charger_common",
					   charger->dev,
					   &bussiness_charger_sysfs_attr_group);
}

static void business_charger_sysfs_remove_group(struct device *dev)
{
	mca_sysfs_remove_link_group(MCA_SYSFS_DEV_CHARGER, "charger_common", dev,
				    &bussiness_charger_sysfs_attr_group);
}

static int business_charger_event_thread(void *args)
{
	struct business_charger *charger = args;

	business_charger_process_online_change(charger, MCA_EVENT_USB_CONNECT);
	business_charger_process_wireless_online_change(charger,
							MCA_EVENT_WIRELESS_CONNECT);

	while (!kthread_should_stop()) {
		wait_event_interruptible(charger->wait_que,
					 charger->thread_active == BUSINESS_CHARGER_THREAD_ACTIVE ||
					 kthread_should_stop());
		if (kthread_should_stop())
			break;
		charger->thread_active = 0;
		business_charger_process_event(charger);
	}

	return 0;
}

static ssize_t business_debugfs_show(void *priv_data, char *buf)
{
	struct mca_debugfs_attr_data *attr_data = priv_data;
	struct mca_debugfs_attr_info *attr_info = attr_data->attr_info;
	struct business_charger *charger = attr_data->private;
	ssize_t count = 0;

	if (!charger || !attr_info) {
		mca_log_err("null pointer store\n");
		return count;
	}

	switch (attr_info->debugfs_attr_name) {
	case BUSINESS_CHARGER_DEBUG_PROP_DAM_TEST:
		count = scnprintf(buf, PAGE_SIZE, "%d\n", charger->dam_test_flag);
		break;
	default:
		break;
	}

	return count;
}

/* dam_test: while set, the charger wake lock is not held. */
static ssize_t business_debugfs_store(void *priv_data, const char *buf,
				      size_t count)
{
	struct mca_debugfs_attr_data *attr_data = priv_data;
	struct mca_debugfs_attr_info *attr_info = attr_data->attr_info;
	struct business_charger *charger = attr_data->private;
	int val = 0;

	if (kstrtoint(buf, 16, &val))
		return -EINVAL;

	switch (attr_info->debugfs_attr_name) {
	case BUSINESS_CHARGER_DEBUG_PROP_DAM_TEST:
		charger->dam_test_flag = val;
		if (charger->dam_test_flag) {
			mca_log_info("relax wake_lock\n");
			__pm_relax(charger->online_wake_lock);
		} else if (charger->bc12_active) {
			__pm_stay_awake(charger->online_wake_lock);
			mca_log_info("stay wake_lock\n");
		}
		break;
	default:
		break;
	}

	return count;
}

static struct mca_debugfs_attr_info g_business_charge_debugfs_field_tbl[] = {
	mca_debugfs_attr(business_debugfs, 0664, BUSINESS_CHARGER_DEBUG_PROP_DAM_TEST, dam_test),
};

#define BUSINESS_CHARGER_DEBUGFS_ATTRS_SIZE	ARRAY_SIZE(g_business_charge_debugfs_field_tbl)

static int business_charge_dump_log_head(void *data, char *buf, int size)
{
	return snprintf(buf, size,
			"Online ChargeType ChargeEnable ChargeMode Vbus   Ibus   BTB_Status Batt_Auth ");
}

/*
 * ChargeEnable packs the buck (bit 0), quick charge (bit 4) and quick
 * wireless (bit 14) enable states.  Vbus/Ibus are in mV/mA.
 */
static int business_charge_dump_log_context(void *data, char *buf, int size)
{
	struct business_charger *charger = data;
	int online = 0, chg_type = 0, mode = 0;
	int enable = 0, temp = 0;
	int vbus = 0, ibus = 0;

	if (!charger)
		return snprintf(buf, size, "%-7d%-11d0x%-11x%-11d%-7d%-7d%-11d%-10d",
				-1, -1, -1, -1, -1, -1, -1, -1);

	if (charger->bc12_active) {
		online = 1;
		chg_type = charger->real_type;
		mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					     STRATEGY_STATUS_TYPE_MODE, &mode);
	} else if (charger->wls_active) {
		online = 2;
		chg_type = charger->wls_adapter_type;
		mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_WIRELESS,
					     STRATEGY_STATUS_TYPE_MODE, &mode);
	}

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				     STRATEGY_STATUS_TYPE_ENABLE, &temp);
	enable |= temp;
	temp = 0;
	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
				     STRATEGY_STATUS_TYPE_ENABLE, &temp);
	enable |= temp << 4;
	temp = 0;
	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_WIRELESS,
				     STRATEGY_STATUS_TYPE_ENABLE, &temp);
	enable |= temp << 14;

	if (mode) {
		enum mca_strategy_func_type type = charger->bc12_active ?
			STRATEGY_FUNC_TYPE_QUICK_CHARGE :
			STRATEGY_FUNC_TYPE_QUICK_WIRELESS;

		mca_strategy_func_get_status(type, STRATEGY_STATUS_TYPE_VBUS, &vbus);
		mca_strategy_func_get_status(type, STRATEGY_STATUS_TYPE_IBUS, &ibus);
	} else {
		platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER, &vbus);
		platform_class_buckchg_ops_get_bus_curr(MAIN_BUCK_CHARGER, &ibus);
		vbus /= 1000;
		ibus /= 1000;
	}

	return snprintf(buf, size, "%-7d%-11d0x%-11x%-11d%-7d%-7d%-11d%-10d",
			online, chg_type, enable, mode, vbus, ibus,
			charger->abnormal_info.batt_missing,
			!charger->abnormal_info.batt_auth_failed);
}

static struct mca_log_charge_log_ops g_business_charger_log_ops = {
	.dump_log_head = business_charge_dump_log_head,
	.dump_log_context = business_charge_dump_log_context,
};

struct business_charger_nb_info {
	enum mca_event_notify_type type;
	size_t offset;
	notifier_fn_t call;
};

static const struct business_charger_nb_info business_charger_nbs[] = {
	{ MCA_EVENT_TYPE_CHARGER_CONNECT,
	  offsetof(struct business_charger, connect_nb),
	  business_charger_notifier_connect_cb },
	{ MCA_EVENT_TYPE_CHARGE_TYPE,
	  offsetof(struct business_charger, type_change_nb),
	  business_charger_notifier_type_cb },
	{ MCA_EVENT_TYPE_HW_INFO,
	  offsetof(struct business_charger, hw_info_nb),
	  business_charger_notifier_hw_cb },
	{ MCA_EVENT_CHARGE_STATUS,
	  offsetof(struct business_charger, chg_sts_nb),
	  business_charger_notifier_chg_sts_cb },
	{ MCA_EVENT_TYPE_CP_INFO,
	  offsetof(struct business_charger, cp_info_nb),
	  business_charger_notifier_cp_info_cb },
	{ MCA_EVENT_TYPE_DEBUG,
	  offsetof(struct business_charger, debug_nb),
	  business_charger_notifier_debug_cb },
};

static struct notifier_block *business_charger_nb(struct business_charger *charger,
						  int i)
{
	return (struct notifier_block *)((char *)charger +
					 business_charger_nbs[i].offset);
}

static void business_charger_unregister_nbs(struct business_charger *charger,
					    int count)
{
	while (count--)
		mca_event_block_notify_unregister(business_charger_nbs[count].type,
						  business_charger_nb(charger, count));
}

static int business_charger_probe(struct platform_device *pdev)
{
	struct business_charger *charger;
	static int probe_cnt;
	int rc;
	int i;

	mca_log_err("probe begin cnt %d\n", ++probe_cnt);
	charger = devm_kzalloc(&pdev->dev, sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;

	platform_set_drvdata(pdev, charger);
	charger->dev = &pdev->dev;
	rc = business_charger_parse_dt(charger);
	if (rc < 0) {
		mca_log_err(" Couldn't parse device tree rc=%d\n", rc);
		return rc;
	}

	charger->usb_psy_info = business_charger_usb_psy_init(charger->dev);
	if (!charger->usb_psy_info) {
		mca_log_err("Couldn't init usb psy rc=%d\n", 0);
		return -EINVAL;
	}

	charger->wls_psy_info = business_charger_wireless_psy_init(charger->dev);
	if (!charger->wls_psy_info) {
		mca_log_err("Couldn't init wireless psy rc=%d\n", 0);
		return -EINVAL;
	}

	INIT_LIST_HEAD(&charger->header);
	spin_lock_init(&charger->list_lock);
	init_waitqueue_head(&charger->wait_que);
	charger->usb_psy_info->last_allow_charge = 1;
	/* Fast charging icons stay off until the battery is authenticated. */
	charger->abnormal_info.asuint32 = 0;
	charger->abnormal_info.batt_auth_failed = 1;
	INIT_DELAYED_WORK(&charger->delay_report_status_work,
			  business_charger_wireless_report_status_work);
	INIT_DELAYED_WORK(&charger->delay_enable_rx_work,
			  business_charger_wireless_delay_enable_rx_work);
	INIT_DELAYED_WORK(&charger->reset_rx_work,
			  business_charger_wireless_reset_rx_work);
	INIT_DELAYED_WORK(&charger->report_quick_charge_type_work,
			  business_charger_report_quick_charge_type_work);
	INIT_DELAYED_WORK(&charger->delay_rerun_usb_sns_work,
			  business_charger_wireless_rerun_usb_sns_work);
	INIT_DELAYED_WORK(&charger->pmic_init_done_notify_work,
			  business_charger_pmic_init_done_notify_work);

	for (i = 0; i < ARRAY_SIZE(business_charger_nbs); i++) {
		struct notifier_block *nb = business_charger_nb(charger, i);

		nb->notifier_call = business_charger_nbs[i].call;
		rc = mca_event_block_notify_register(business_charger_nbs[i].type, nb);
		if (rc) {
			business_charger_unregister_nbs(charger, i);
			mca_log_info("%s !!\n", "Over probe cnt max");
			return -EPROBE_DEFER;
		}
	}

	schedule_delayed_work(&charger->delay_rerun_usb_sns_work,
			      msecs_to_jiffies(RERUN_USB_SNS_DELAY_MS));
	schedule_delayed_work(&charger->pmic_init_done_notify_work, 0);

	charger->online_wake_lock = wakeup_source_register(charger->dev,
							   "charger_wakelock");
	if (!charger->online_wake_lock)
		mca_log_err("reg wakelock failed\n");

	charger->event_task = kthread_run(business_charger_event_thread, charger,
					  "charger_event_thread");
	if (IS_ERR(charger->event_task))
		charger->event_task = NULL;

	if (business_charger_sysfs_create_group(charger))
		mca_log_err("create sysfs failed\n");
	mca_debugfs_create_group("business_charger",
				 g_business_charge_debugfs_field_tbl,
				 BUSINESS_CHARGER_DEBUGFS_ATTRS_SIZE, charger);
	mca_log_charge_log_register(MCA_CHARGE_LOG_ID_BUSINESS_CHG,
				    &g_business_charger_log_ops, charger);
	mca_log_err("probe success\n");

	return 0;
}

/*
 * Everything that can reach the devm-allocated state (event thread,
 * notifiers, works, sysfs) is torn down before it is freed.
 */
static int business_charger_remove(struct platform_device *pdev)
{
	struct business_charger *charger = platform_get_drvdata(pdev);

	business_charger_sysfs_remove_group(&pdev->dev);
	if (!charger)
		return 0;

	business_charger_unregister_nbs(charger, ARRAY_SIZE(business_charger_nbs));
	if (charger->event_task) {
		kthread_stop(charger->event_task);
		charger->event_task = NULL;
	}

	cancel_delayed_work_sync(&charger->delay_report_status_work);
	cancel_delayed_work_sync(&charger->delay_enable_rx_work);
	cancel_delayed_work_sync(&charger->reset_rx_work);
	cancel_delayed_work_sync(&charger->report_quick_charge_type_work);
	cancel_delayed_work_sync(&charger->delay_rerun_usb_sns_work);
	cancel_delayed_work_sync(&charger->pmic_init_done_notify_work);
	wakeup_source_unregister(charger->online_wake_lock);

	return 0;
}

static void business_charger_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,business_charger" },
	{ },
};

static struct platform_driver business_charger_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "business_charger",
		.of_match_table = match_table,
	},
	.probe = business_charger_probe,
	.remove = business_charger_remove,
	.shutdown = business_charger_shutdown,
};

static int __init business_charger_init(void)
{
	return platform_driver_register(&business_charger_driver);
}
module_init(business_charger_init);

static void __exit business_charger_exit(void)
{
	platform_driver_unregister(&business_charger_driver);
}
module_exit(business_charger_exit);

MODULE_DESCRIPTION("business charger core");
MODULE_AUTHOR("getian@xiaomi.com");
MODULE_LICENSE("GPL v2");
