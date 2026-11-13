/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The charge adapters the stack can talk to.
 *
 * A charger is negotiated with over one of several protocols -- USB Power
 * Delivery, Quick Charge, plain BC1.2 detection -- and the drivers above do
 * not care which.  Each protocol registers here, and callers ask by protocol
 * rather than calling into a particular one.
 */

#ifndef __MCA_PROTOCOL_CLASS_H
#define __MCA_PROTOCOL_CLASS_H

#include <linux/types.h>

/* How many operating points an adapter may offer. */
#define ADAPTER_POWER_CAP_MAX		7

/* How many segments a power curve is described in. */
#define ADAPTER_POWER_CURVE_MAX		3

/* The protocols an adapter can be spoken to over. */
/* What kind of charger the stack decided is attached. */
enum xm_adapter_type {
	XM_CHARGER_TYPE_UNKNOW,
	XM_CHARGER_TYPE_SDP,
	XM_CHARGER_TYPE_CDP,
	XM_CHARGER_TYPE_DCP,
	XM_CHARGER_TYPE_FLOAT,
	XM_CHARGER_TYPE_HVDCP2,
	XM_CHARGER_TYPE_HVDCP3,
	XM_CHARGER_TYPE_HVDCP3_B,
	XM_CHARGER_TYPE_HVDCP3P5,
	XM_CHARGER_TYPE_TYPEC,
	XM_CHARGER_TYPE_PD,
	XM_CHARGER_TYPE_PPS,
	XM_CHARGER_TYPE_PD_VERIFY,
	XM_CHARGER_TYPE_SRC_UFP,
	XM_CHARGER_TYPE_ACA,
	XM_CHARGER_TYPE_OCP,
};

enum adatper_protocol {
	ADAPTER_PROTOCOL_PD,
	ADAPTER_PROTOCOL_PPS,
	ADAPTER_PROTOCOL_UFCS,
	ADAPTER_PROTOCOL_QC,
	ADAPTER_PROTOCOL_QI2,
	ADAPTER_PROTOCOL_BC12,
	ADAPTER_PROTOCOL_MAX,
};

/**
 * struct adapter_power_cap - one operating point an adapter offers
 * @min_current: lowest current of the range, in milliamps
 * @max_current: highest current of the range, in milliamps
 * @min_voltage: lowest voltage of the range, in millivolts
 * @max_voltage: highest voltage of the range, in millivolts
 * @max_power:   what the two together come to, in milliwatts
 */
/* What the strategies size their own copies of an adapter's list by. */
#define ADAPTER_CAP_MAX_NR		ADAPTER_POWER_CAP_MAX

struct adapter_power_cap {
	int min_current;
	int max_current;
	int min_voltage;
	int max_voltage;
	int max_power;
};

/**
 * struct adapter_power_cap_info - every operating point an adapter offers
 * @pdo_nums: how many of @cap are filled in
 * @cap:     the operating points themselves
 */
struct adapter_power_cap_info {
	int pdo_nums;
	struct adapter_power_cap cap[ADAPTER_POWER_CAP_MAX];
};

/**
 * struct adapter_vendor_info - what an adapter says it is
 * @vid:    vendor id it reports
 * @pid:    product id it reports
 * @hw_rev: hardware revision
 * @fw_rev: firmware revision
 */
struct adapter_vendor_info {
	int vid;
	int pid;
	int hw_rev;
	int fw_rev;
};

/**
 * struct adapter_power_curve - how much an adapter supplies as it warms
 * @pwr_curve_num: how many segments are filled in
 * @curve:         each segment, as five values the charging strategy reads
 *
 * An adapter that derates over time describes the derating rather than a
 * single figure, so the strategy can plan for the power it will still have
 * later rather than the power it has now.
 */
struct adapter_power_curve {
	int pwr_curve_num;
	int curve[ADAPTER_POWER_CURVE_MAX][5];
};

/**
 * struct adapter_protocol_class_ops - what a protocol implementation provides
 *
 * Every call takes the @data the implementation registered.  A protocol that
 * cannot answer something leaves the entry NULL, and the caller is told so
 * rather than being given a wrong answer.
 */
struct adapter_protocol_class_ops {
	int (*adapter_det_en)(void *data, int enable);
	int (*set_adapter_verified)(void *data, int verified);
	int (*get_adapter_verified)(void *data, int *verified);
	int (*get_adapter_type)(void *data, int *type);
	int (*get_adapter_max_power)(void *data, u32 *max_power_mw);
	int (*get_adapter_pwr_cap)(void *data,
				   struct adapter_power_cap_info *cap);
	int (*set_adapter_volt_and_curr)(void *data, int volt_mv, int curr_ma);
	int (*get_adapter_volt_and_curr)(void *data, int *volt_mv,
					 int *curr_ma);
	int (*get_adapter_pps_ptf)(void *data, int *ptf);
	int (*get_adapter_info)(void *data, struct adapter_vendor_info *info);
	int (*get_adapter_power_curve)(void *data,
				       struct adapter_power_curve *pwr_curve);
	int (*get_adapter_pwr_max_power)(void *data, u32 *max_power_mw);
};

int protocol_class_register_ops(enum adatper_protocol protocol,
				const struct adapter_protocol_class_ops *ops,
				void *data);

int protocol_class_det_adapter_type(enum adatper_protocol protocol,
				    int enable);
int protocol_class_set_adapter_verified(enum adatper_protocol protocol,
					int verified);
int protocol_class_get_adapter_verified(enum adatper_protocol protocol,
					int *verified);
int protocol_class_get_adapter_type(enum adatper_protocol protocol, u32 *type);
int protocol_class_get_adapter_max_power(enum adatper_protocol protocol,
					 u32 *max_power_mw);
int protocol_class_get_adapter_pwr_max_power(enum adatper_protocol protocol,
					     u32 *max_power_mw);
int protocol_class_get_adapter_power_cap(enum adatper_protocol protocol,
					 struct adapter_power_cap_info *cap);
int protocol_class_set_adapter_volt_and_curr(enum adatper_protocol protocol,
					     int volt_mv, int curr_ma);
int protocol_class_get_adapter_volt_and_curr(enum adatper_protocol protocol,
					     int *volt_mv, int *curr_ma);
int protocol_class_get_adapter_pps_ptf(enum adatper_protocol protocol,
				       int *pps_ptf);
int protocol_class_get_adapter_info(enum adatper_protocol protocol,
				    struct adapter_vendor_info *info);
int protocol_class_get_adapter_power_curve(enum adatper_protocol protocol,
					   struct adapter_power_curve *pwr_curve);

#endif /* __MCA_PROTOCOL_CLASS_H */
