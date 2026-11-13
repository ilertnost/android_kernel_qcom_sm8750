/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Liquid detection on the Type-C port.
 *
 * Water bridging the connector's pins corrodes them under charging current,
 * so the charger measures each pin against ground and refuses to charge while
 * any of them reads wet.  The measurements are exposed for the tests that
 * characterise the port, and the one thing the rest of the stack needs to
 * know -- whether charging is being held off -- is offered as a call.
 */

#ifndef __MCA_LPD_DETECT_H
#define __MCA_LPD_DETECT_H

int lpd_is_charging_limit(void);

#endif /* __MCA_LPD_DETECT_H */
