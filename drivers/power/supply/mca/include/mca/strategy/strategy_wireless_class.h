/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * What the wireless charging strategy tells the rest of the stack.
 *
 * Wireless charging is negotiated rather than measured: how much a pad can
 * supply is something it says over the coil, and until it has said so nobody
 * else can decide anything.  The strategy that runs that negotiation answers
 * the few questions the quick-charging path needs before it can start.
 */

#ifndef __MCA_STRATEGY_WIRELESS_H
#define __MCA_STRATEGY_WIRELESS_H

#include <linux/types.h>

/* How the transmitter was identified. */
enum xm_wireless_adapter_type {
	XM_WLS_CHARGER_TYPE_UNKNOWN,
	XM_WLS_CHARGER_TYPE_BPP,
	XM_WLS_CHARGER_TYPE_EPP,
	XM_WLS_CHARGER_TYPE_HPP,
};

/* How many cells the charge pump divides the input across. */
enum WLS_FORWARD_CHARGER_MODE {
	FORWARD_2_1_CHARGER_MODE = 2,
	FORWARD_4_1_CHARGER_MODE = 4,
};

/**
 * struct wls_adapter_power_cap - what the transmitter says it can supply
 * @max_fcc:   the charge current it will support, in milliamps
 * @max_power: and the power it will deliver, in watts
 */
struct wls_adapter_power_cap {
	int	max_fcc;
	int	max_power;
};

int strategy_class_wireless_ops_get_adapter_power(struct wls_adapter_power_cap *adapter_power);
int strategy_class_wireless_ops_get_adapter_charger_mode(int *cp_charger_mode);
int strategy_class_wireless_ops_get_wls_type(int *wls_type);
int strategy_class_wireless_ops_set_parallel_charge(bool parallel_charge_flag);
void strategy_class_wireless_op_get_rx_iout_limit(int *rx_iout_limit_ma);

/* Which receiver a board was built with. */
enum mca_wireless_project_vendor {
	WLS_CHIP_VENDOR_NONE,
	WLS_CHIP_VENDOR_FUDA1651,
	WLS_CHIP_VENDOR_FUDA1665,
	WLS_CHIP_VENDOR_FUDA1661,
	WLS_CHIP_VENDOR_SC96281,
	WLS_CHIP_VENDOR_SC96231,
};

/*
 * What the pad turned out to be.  A pad proves what it is over the coil
 * before it is allowed to supply more than the plain standard, so these are
 * the answers to that exchange rather than anything measured.
 */
enum mca_wireless_adapter_type {
	ADAPTER_NONE		= 0,
	ADAPTER_SDP		= 1,
	ADAPTER_CDP		= 2,
	ADAPTER_DCP		= 3,
	ADAPTER_QC2		= 5,
	ADAPTER_QC3		= 6,
	ADAPTER_PD		= 7,
	ADAPTER_AUTH_FAILED	= 8,
	ADAPTER_XIAOMI_QC3	= 9,
	ADAPTER_XIAOMI_PD	= 10,
	ADAPTER_ZIMI_CAR_POWER	= 11,
	ADAPTER_XIAOMI_PD_40W	= 12,
	ADAPTER_VOICE_BOX	= 13,
	ADAPTER_XIAOMI_PD_50W	= 14,
	ADAPTER_XIAOMI_PD_60W	= 15,
	ADAPTER_XIAOMI_PD_100W	= 16,
	ADAPTER_MAX		= 17,
};

/* What a pad has proved it can supply, in watts. */
enum WLS_APDO_MAX {
	WLS_SSDEV_POWER_MAX_INVALID = 0,
	WLS_SSDEV_POWER_MAX_20W = 20,
	WLS_SSDEV_POWER_MAX_30W = 30,
	WLS_SSDEV_POWER_MAX_50W = 50,
	WLS_SSDEV_POWER_MAX_80W = 80,
};

#endif /* __MCA_STRATEGY_WIRELESS_H */
