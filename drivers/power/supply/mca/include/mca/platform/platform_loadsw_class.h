/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The load switches between the charger and the battery.
 *
 * A load switch gates one charging path.  A board can have more than one --
 * a main path and a parallel one -- so every call names which, and the
 * driver for each registers under its own index.
 */

#ifndef __MCA_PLATFORM_LOADSW_H
#define __MCA_PLATFORM_LOADSW_H

#include <linux/types.h>

/* Which path a load switch gates. */
enum platform_class_load_switch_role {
	LOADSW_ROLE_MASTER,
	LOADSW_ROLE_SLAVE,
	LOADSW_ROLE_MAX,
};

/* What userspace can read and set on one. */
enum loadsw_attr_list {
	LOADSW_PROP_CHIP_OK,
	LOADSW_PROP_IBAT_LIMIT,
	LOADSW_PROP_LOW_POWER,
};

/**
 * struct platform_class_loadsw_ops - what a load switch driver provides
 * @loadsw_get_present:       whether the switch answered at all
 * @loadsw_get_ibat_limit:    the current limit it is set to, in milliamps
 * @loadsw_set_ibat_limit:    set that limit
 * @loadsw_set_lowpower_mode: put it in or take it out of low power mode
 * @loadsw_get_lowpower_mode: whether it is in low power mode
 */
struct platform_class_loadsw_ops {
	int (*loadsw_get_present)(bool *present, void *data);
	int (*loadsw_get_ibat_limit)(int *ibat_ma, void *data);
	int (*loadsw_set_ibat_limit)(int ibat_ma, void *data);
	int (*loadsw_set_lowpower_mode)(bool enable, void *data);
	int (*loadsw_get_lowpower_mode)(bool *enable, void *data);
};

int platform_class_loadsw_register_ops(
				       enum platform_class_load_switch_role role,
				       const struct platform_class_loadsw_ops *ops,
				       void *data);
int platform_class_loadsw_get_present(
				      enum platform_class_load_switch_role role,
				      bool *present);
int platform_class_loadsw_get_ibat_limit(
					 enum platform_class_load_switch_role role,
					 int *ibat_ma);
int platform_class_loadsw_set_ibat_limit(
					 enum platform_class_load_switch_role role,
					 int ibat_ma);
int platform_class_loadsw_get_lowpower_mode(
					    enum platform_class_load_switch_role role,
					    bool *enable);
int platform_class_loadsw_set_lowpower_mode(
					    enum platform_class_load_switch_role role,
					    bool enable);

#endif /* __MCA_PLATFORM_LOADSW_H */
