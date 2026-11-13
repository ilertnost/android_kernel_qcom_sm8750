/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Where userspace asks for charging to change.
 *
 * Several things outside the kernel have a say in how the phone charges: the
 * settings app, a factory test, a thermal daemon.  Each writes here naming
 * itself and what it wants, and the charging stack keeps track of who asked
 * for what, so that a test that suspends charging and then crashes leaves a
 * record of which client is holding it suspended.
 *
 * A phone with several charging paths lets a client name one -- the wired
 * buck charger, one of the charge pumps, the wireless path -- or all of them.
 */

#ifndef __MCA_CHARGE_INTERFACE_H
#define __MCA_CHARGE_INTERFACE_H

#include <linux/types.h>

/* How much room a getter has to write its answer into. */
#define MCA_CHARGE_IF_MAX_VALUE_BUFF	128

/* Which charging path a request is about. */
enum mca_charge_if_charge_type {
	MCA_CHARGE_IF_CHG_TYPE_BEGIN = 0,
	MCA_CHARGE_IF_CHG_TYPE_BUCK = MCA_CHARGE_IF_CHG_TYPE_BEGIN,
	MCA_CHARGE_IF_CHG_TYPE_MAIN_BUCK,
	MCA_CHARGE_IF_CHG_TYPE_AUX_BUCK,
	MCA_CHARGE_IF_CHG_TYPE_QC,
	MCA_CHARGE_IF_CHG_TYPE_QC_MAIN_PATH,
	MCA_CHARGE_IF_CHG_TYPE_QC_AUX_PATH,
	MCA_CHARGE_IF_CHG_TYPE_QC_DIV1,
	MCA_CHARGE_IF_CHG_TYPE_QC_DIV2,
	MCA_CHARGE_IF_CHG_TYPE_QC_DIV4,
	MCA_CHARGE_IF_CHG_TYPE_WL_BUCK,
	MCA_CHARGE_IF_CHG_TYPE_WL_MAIN_BUCK,
	MCA_CHARGE_IF_CHG_TYPE_WL_AUX_BUCK,
	MCA_CHARGE_IF_CHG_TYPE_WL_QC,
	MCA_CHARGE_IF_CHG_TYPE_WL_QC_MAIN_PATH,
	MCA_CHARGE_IF_CHG_TYPE_WL_QC_AUX_PATH,
	MCA_CHARGE_IF_CHG_TYPE_WL_QC_DIV1,
	MCA_CHARGE_IF_CHG_TYPE_WL_QC_DIV2,
	MCA_CHARGE_IF_CHG_TYPE_WL_QC_DIV4,
	MCA_CHARGE_IF_CHG_TYPE_ALL,
	MCA_CHARGE_IF_CHG_TYPE_END,
};

/**
 * struct mca_charge_if_ops - what the charging stack offers userspace
 * @type_name: the charging path it implements, one of the names userspace
 *             uses ("buck", "quick", "wl_buck", ...)
 * @data:      handed back to every call
 *
 * The setters take the client's name so that whoever asked can be reported
 * back.  The getters return 0 on success and fill a buffer of
 * MCA_CHARGE_IF_MAX_VALUE_BUFF bytes with "eff_client:<client> <value>";
 * the charge interface prints it after the path's name.
 */
struct mca_charge_if_ops {
	const char	*type_name;
	void		*data;

	int (*set_input_suspend)(const char *user, char *value, void *data);
	int (*get_input_suspend)(char *buf, void *data);
	int (*set_charge_enable)(const char *user, u32 value, void *data);
	int (*get_charge_enable)(char *buf, void *data);
	int (*set_input_current_limit)(const char *user, char *value,
				       void *data);
	int (*get_input_current_limit)(char *buf, void *data);
	int (*set_charge_current_limit)(const char *user, char *value,
					void *data);
	int (*get_charge_current_limit)(char *buf, void *data);
	int (*set_charge_power_limit)(const char *user, u32 value, void *data);
	int (*get_charge_power_limit)(char *buf, void *data);
	int (*set_ship_mode_en)(const char *user, u32 value, void *data);
	int (*get_ship_mode_status)(bool *status, void *data);
};

int mca_charge_if_ops_register(struct mca_charge_if_ops *ops);

#endif /* __MCA_CHARGE_INTERFACE_H */
