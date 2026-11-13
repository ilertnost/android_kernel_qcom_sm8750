/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mca_business_charger.h
 *
 * mca charge business driver
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
#ifndef __CHARGER_CORE_H__
#define __CHARGER_CORE_H__

#include <linux/types.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <mca/strategy/strategy_class.h>
#include <mca/common/mca_voter.h>
#include "mca_charger_usb_psy.h"
#include "mca_charger_wireless_psy.h"

enum business_charger_attr_list {
	BUSINESS_CHARGER_DEBUG_PROP_DAM_TEST = 0,
};

struct chg_dt_props {
	int test;
};

struct charger_event_lis_node {
	struct list_head lnode;
	unsigned int event;
	void *data;
};

/* Any bit set forces the normal (not fast) charging icon. */
union charger_abnormal_info {
	u32 asuint32;
	struct {
		u32 batt_auth_failed	: 1; /* bit 0 */
		u32 batt_missing	: 1; /* bit 1 */
		u32 batt_cold		: 1; /* bit 2 */
		u32 batt_overhot	: 1; /* bit 3 */
		u32 reserved		: 28;
	};
};

/* cp_type values */
#define BUSINESS_CHARGER_CP_TYPE_SINGLE		0
#define BUSINESS_CHARGER_CP_TYPE_PARALLEL	1

struct business_charger {
	struct device *dev;
	struct chg_dt_props dt;
	struct usb_psy_info *usb_psy_info;
	struct wireless_psy_info *wls_psy_info;
	struct notifier_block connect_nb;
	struct notifier_block type_change_nb;
	struct notifier_block hw_info_nb;
	struct notifier_block chg_sts_nb;
	struct notifier_block cp_info_nb;
	struct notifier_block debug_nb;
	struct list_head header;
	spinlock_t list_lock;
	wait_queue_head_t wait_que;
	struct wakeup_source *online_wake_lock;
	struct delayed_work delay_report_status_work;
	struct delayed_work delay_enable_rx_work;
	struct delayed_work reset_rx_work;
	struct delayed_work report_quick_charge_type_work;
	struct delayed_work delay_rerun_usb_sns_work;
	struct delayed_work pmic_init_done_notify_work;
	union charger_abnormal_info abnormal_info;
	int wired_qucik_charge_type;
	int wired_power_max;
	int bc12_active;
	int pd_active;
	int bc12_type;
	int pd_type;
	int real_type;
	int thread_active;
	int wls_active;
	int wls_adapter_type;
	int otg_present;
	int dam_test_flag;
	int is_eu_model;
	int plate_shock;
	int start_quick_revchg;
	int revchg_bcl;
	int handle_state;
	int stop_handle_charge;
	int support_wls;
	int cp_type;
	/* Event thread, kept so that remove can stop it. */
	struct task_struct *event_task;
};

/* Charging icon reported in POWER_SUPPLY_QUICK_CHARGE_TYPE. */
enum adapter_icon_type {
	ADP_ICON_TYPE_NORMAL,
	ADP_ICON_TYPE_FAST,
	ADP_ICON_TYPE_FLASH,
	ADP_ICON_TYPE_TURBO,
	ADP_ICON_TYPE_SUPER,
	ADP_ICON_TYPE_MAX,
};

enum mca_charger_partition_type {
	MCA_CHARGER_PARTITION_TEST,
	MCA_CHARGER_PARTITION_POWEROFFMODE,
	MCA_CHARGER_PARTITION_EU_MODE,
};

#endif /* __CHARGER_CORE_H__ */
