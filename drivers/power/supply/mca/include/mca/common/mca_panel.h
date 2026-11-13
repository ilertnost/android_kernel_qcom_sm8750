/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Panel state for the charging drivers.
 */

#ifndef __MCA_PANEL_H
#define __MCA_PANEL_H

#include <linux/types.h>

/**
 * mca_panel_get_screen_state() - whether the screen is on
 *
 * Return: 1 while the panel is unblanked, 0 otherwise.
 */
int mca_panel_get_screen_state(void);

/**
 * mca_panel_get_hbm_state() - whether high brightness mode is on
 *
 * Return: 1 while the panel is in high brightness mode, 0 otherwise.
 */
int mca_panel_get_hbm_state(void);

#endif /* __MCA_PANEL_H */
