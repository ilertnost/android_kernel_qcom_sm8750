/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Keeping the connector from burning.
 *
 * A Type-C connector with dirt or moisture across its pins heats where the
 * short is, and it can reach a temperature that damages the phone or hurts
 * whoever is holding it long before anything else in the stack notices --
 * the charger reads a normal current, because the current is going where it
 * is supposed to.  So the connector has a thermistor of its own, and both how
 * hot it is and how fast it is getting hotter are watched.
 */

#ifndef __MCA_CONNECTOR_ANTIBURN_H
#define __MCA_CONNECTOR_ANTIBURN_H

int connector_antiburn_is_triggered(void);

#endif /* __MCA_CONNECTOR_ANTIBURN_H */
