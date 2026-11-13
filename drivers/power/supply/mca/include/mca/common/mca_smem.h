/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Battery state the bootloader leaves behind.
 *
 * UEFI checks the battery before Linux starts and records what it found in a
 * shared memory item.  The charging drivers read it from here rather than
 * repeating the check.
 */

#ifndef __MCA_SMEM_H
#define __MCA_SMEM_H

#include <linux/types.h>

/**
 * get_smem_battery_info() - how the battery was started
 * @mode: filled with the zero speed start mode
 */
int get_smem_battery_info(u32 *mode);

/**
 * get_smem_battery_verify_result() - whether UEFI authenticated the battery
 * @verified: filled with the result of the check
 * @chip_ok:  filled with whether the authentication chip answered
 */
int get_smem_battery_verify_result(u8 *verified, u8 *chip_ok);

/**
 * get_smem_battery_cell_sn() - whether the cell serial number was readable
 * @cell_sn: filled with the result
 */
int get_smem_battery_cell_sn(u32 *cell_sn);

#endif /* __MCA_SMEM_H */
