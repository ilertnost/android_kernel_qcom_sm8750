/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * BC1.2 charger detection.
 *
 * Before any protocol is negotiated, the charger is classified by the oldest
 * method there is: what the D+ and D- lines do when the phone probes them.
 * That tells a standard downstream port from a dedicated charger, and it is
 * what the stack falls back on when Power Delivery and Quick Charge both
 * come to nothing.
 *
 * A phone with two charging paths detects on each of them, so the driver that
 * owns each path registers under its role.
 */

#ifndef __MCA_PLATFORM_BC12_H
#define __MCA_PLATFORM_BC12_H

#include <linux/types.h>

/* Which charging path is detecting. */
enum platform_class_bc12_class_role_type {
	BC12_MAIN_ROLE,
	BC12_AUX_ROLE,
	BC12_MAX_ROLE,
};

/**
 * struct platform_bc12_class_ops - what the detecting driver provides
 * @bc12_det_en:     start or stop detection
 * @get_charge_type: what the last detection concluded
 */
struct platform_bc12_class_ops {
	int (*bc12_det_en)(int enable, void *data);
	int (*get_charge_type)(int *type, void *data);
};

int platform_bc12_class_ops_register(enum platform_class_bc12_class_role_type role,
				     const struct platform_bc12_class_ops *ops,
				     void *data);

#endif /* __MCA_PLATFORM_BC12_H */
