/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Board identification, as the charging drivers want it.
 *
 * They adjust for hardware revision and sales region, and would otherwise
 * each call the individual accessors in drivers/misc/hwid; this hands them
 * the whole description at once.
 */

#ifndef __MCA_HWID_H
#define __MCA_HWID_H

#include <linux/hwid.h>
#include <linux/types.h>

/*
 * get_hw_version_platform() value of the O9 (miro) project, which some
 * charging drivers single out besides the projects <linux/hwid.h> names.
 */
#define HARDWARE_PROJECT_O9	3

/**
 * struct mca_hwid_info - what board this is
 * @platform_version: which project
 * @country_version:  which region it was sold in
 * @major_version:    hardware revision, major
 * @minor_version:    hardware revision, minor
 * @build_version:    hardware revision, build
 * @build_adc:        the reading the build is told from
 * @product_adc:      the reading the product is told from
 * @hwid_value:       the raw identifier
 * @product_name:     the product's name
 */
struct mca_hwid_info {
	u32		platform_version;
	u32		country_version;
	u32		major_version;
	u32		minor_version;
	u32		build_version;
	u32		build_adc;
	u32		product_adc;
	u32		hwid_value;
	const char	*product_name;
};

/**
 * mca_get_hwid_info() - describe this board
 *
 * The board does not change while the phone is running, so this is read once
 * and every caller is handed the same description.  It belongs to this module;
 * a caller must not free it.
 *
 * Return: the description, or NULL.
 */
struct mca_hwid_info *mca_get_hwid_info(void);

#endif /* __MCA_HWID_H */
