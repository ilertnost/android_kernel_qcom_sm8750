/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Charging within a thermal budget.
 *
 * The thermal framework decides how warm the phone may get; charging is one
 * of the things it turns down to stay there.  One thermal level maps to a
 * different current for each charging mode -- five volts through the buck
 * charger, a charge pump, a wireless pad that authenticated as eighty watts --
 * so the device tree gives a table per level and mode.
 *
 * A test rig needs the limits out of the way, so they can be removed.
 */

#ifndef __MCA_CHARGER_THERMAL_H
#define __MCA_CHARGER_THERMAL_H

#include <linux/types.h>

int mca_get_wls_charger_thermal_remove(bool *wls_thermal_remove);
int mca_set_wls_charger_thermal_remove(bool wls_thermal_remove);

#endif /* __MCA_CHARGER_THERMAL_H */
