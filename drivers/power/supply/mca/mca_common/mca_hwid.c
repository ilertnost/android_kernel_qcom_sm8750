// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Board identification, as the charging drivers want it.  See
 * include/mca/common/mca_hwid.h.
 */

#include <linux/module.h>
#include <linux/slab.h>

#include <mca/common/mca_hwid.h>
#include <mca/common/mca_log.h>

#include <linux/hwid.h>

/*
 * The board does not change while the phone is running, so it is read once
 * and every later caller is given the same answer.  Callers keep the pointer
 * and none of them frees it.
 */
static struct mca_hwid_info *mca_hwid;

struct mca_hwid_info *mca_get_hwid_info(void)
{
	if (mca_hwid)
		return mca_hwid;

	mca_hwid = kzalloc(sizeof(*mca_hwid), GFP_KERNEL);
	if (!mca_hwid) {
		mca_log_err("kzalloc phwid failed\n");
		return NULL;
	}

	mca_hwid->platform_version = get_hw_version_platform();
	mca_hwid->country_version = get_hw_country_version();
	mca_hwid->major_version = get_hw_version_major();
	mca_hwid->minor_version = get_hw_version_minor();
	mca_hwid->build_version = get_hw_version_build();
	mca_hwid->product_adc = get_hw_project_adc();
	mca_hwid->build_adc = get_hw_build_adc();
	mca_hwid->hwid_value = get_hw_id_value();
	mca_hwid->product_name = product_name_get();

	/* Project 7 boards sold in India are charged as Chinese boards. */
	if (mca_hwid->platform_version == 7 &&
	    mca_hwid->country_version == CountryIndia)
		mca_hwid->country_version = CountryCN;

	mca_log_err("platform_version: %d, country_version: %d, major_version: %d, minor_version: %d, build_version: %d",
		    mca_hwid->platform_version, mca_hwid->country_version,
		    mca_hwid->major_version, mca_hwid->minor_version,
		    mca_hwid->build_version);

	return mca_hwid;
}
EXPORT_SYMBOL(mca_get_hwid_info);

MODULE_DESCRIPTION("mca get hwid info");
MODULE_AUTHOR("wangyun5@xiaomi.com");
MODULE_LICENSE("GPL v2");
