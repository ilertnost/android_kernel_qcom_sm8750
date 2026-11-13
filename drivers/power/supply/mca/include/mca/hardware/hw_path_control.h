/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Which way the power flows.
 *
 * A phone that charges by cable and by pad, and that can charge something else
 * from either, has a handful of gates deciding which of those is connected at
 * any moment.  Getting that wrong shorts one supply into another, so the gates
 * are not driven directly: a caller says which source it is talking about and
 * whether it wants it, and the board's own table says which gates that means.
 */

#ifndef __MCA_PATH_CONTROL_H
#define __MCA_PATH_CONTROL_H

#include <linux/types.h>

/* Where power is coming from or going to.  A phone can be in several at once. */
enum path_control_src {
	PATH_CONTROL_NONE	= 0,
	PATH_CONTROL_USB	= BIT(0),
	PATH_CONTROL_WLS	= BIT(1),
	PATH_CONTROL_WLS_REV	= BIT(2),
	PATH_CONTROL_OTG	= BIT(3),
	PATH_CONTROL_VDD	= BIT(4),
};

typedef enum path_control_src CONTROL_SRC;

int mca_path_control_enable_gate(CONTROL_SRC src, bool enable);

#endif /* __MCA_PATH_CONTROL_H */
