// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Liquid detection on the Type-C port.  See
 * include/mca/hardware/hw_lpd_detect.h.
 */

#define MCA_LOG_TAG "mca_lpd_detect"

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <mca/common/mca_log.h>
#include <mca/hardware/hw_lpd_detect.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/common/mca_sysfs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/* What userspace can look at, and the two controls a test needs. */
enum lpd_attr_list {
	LPD_PROP_EN,
	LPD_PROP_STATUS,
	LPD_PROP_SBU1,
	LPD_PROP_SBU2,
	LPD_PROP_CC1,
	LPD_PROP_CC2,
	LPD_PROP_DP,
	LPD_PROP_DM,
	LPD_PROP_CHARGING,
	LPD_PROP_CONTROL,
	LPD_PROP_UART_CONTROL,
};

/**
 * struct mca_lpd_dev - the port being watched
 * @dev: this device
 * @lpd_charging: whether charging is being held off because the port is wet,
 *	as set by userspace through the lpd_charging attribute
 */
struct mca_lpd_dev {
	struct device	*dev;
	int		lpd_charging;
};

static struct mca_lpd_dev *g_lpd;

/**
 * lpd_is_charging_limit() - whether a wet port is holding charging off
 *
 * The charging strategies ask this before deciding why charging is not
 * happening, so that a wet port is reported as such rather than as a fault.
 */
int lpd_is_charging_limit(void)
{
	if (!g_lpd)
		return 0;

	return g_lpd->lpd_charging;
}
EXPORT_SYMBOL(lpd_is_charging_limit);

static ssize_t lpd_sysfs_show(struct device *dev,
			      struct device_attribute *attr, char *buf);
static ssize_t lpd_sysfs_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count);

static struct mca_sysfs_attr_info lpd_sysfs_field_tbl[] = {
	mca_sysfs_attr_ro(lpd_sysfs, 0440, LPD_PROP_EN, enable),
	mca_sysfs_attr_ro(lpd_sysfs, 0440, LPD_PROP_STATUS, lpd_status),
	mca_sysfs_attr_rw(lpd_sysfs, 0664, LPD_PROP_SBU1, sbu1),
	mca_sysfs_attr_ro(lpd_sysfs, 0440, LPD_PROP_SBU2, sbu2),
	mca_sysfs_attr_ro(lpd_sysfs, 0440, LPD_PROP_CC1, cc1),
	mca_sysfs_attr_ro(lpd_sysfs, 0440, LPD_PROP_CC2, cc2),
	mca_sysfs_attr_ro(lpd_sysfs, 0440, LPD_PROP_DP, dp),
	/* stock serves "dm" with the D+ reading as well */
	mca_sysfs_attr_ro(lpd_sysfs, 0440, LPD_PROP_DP, dm),
	mca_sysfs_attr_rw(lpd_sysfs, 0664, LPD_PROP_CHARGING, lpd_charging),
	mca_sysfs_attr_rw(lpd_sysfs, 0664, LPD_PROP_CONTROL, lpd_control),
	mca_sysfs_attr_rw(lpd_sysfs, 0664, LPD_PROP_UART_CONTROL,
			  uart_control),
};

static ssize_t lpd_sysfs_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct mca_lpd_dev *chip = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *info;
	int val = 0;

	info = mca_sysfs_lookup_attr(attr->attr.name, lpd_sysfs_field_tbl,
				     ARRAY_SIZE(lpd_sysfs_field_tbl));
	if (!info)
		return -1;

	switch (info->sysfs_attr_name) {
	case LPD_PROP_EN:
		platform_class_buckchg_ops_get_lpd_enable(MAIN_BUCK_CHARGER,
							  &val);
		break;
	case LPD_PROP_STATUS:
		platform_class_buckchg_ops_get_lpd_status(MAIN_BUCK_CHARGER,
							  &val);
		break;
	case LPD_PROP_SBU1:
		platform_class_buckchg_ops_get_lpd_sbu1(MAIN_BUCK_CHARGER,
							&val);
		break;
	case LPD_PROP_SBU2:
		platform_class_buckchg_ops_get_lpd_sbu2(MAIN_BUCK_CHARGER,
							&val);
		break;
	case LPD_PROP_CC1:
		platform_class_buckchg_ops_get_lpd_cc1(MAIN_BUCK_CHARGER, &val);
		break;
	case LPD_PROP_CC2:
		platform_class_buckchg_ops_get_lpd_cc2(MAIN_BUCK_CHARGER, &val);
		break;
	case LPD_PROP_DP:
		platform_class_buckchg_ops_get_lpd_dp(MAIN_BUCK_CHARGER, &val);
		break;
	case LPD_PROP_DM:
		platform_class_buckchg_ops_get_lpd_dm(MAIN_BUCK_CHARGER, &val);
		break;
	case LPD_PROP_CHARGING:
		val = chip->lpd_charging;
		break;
	case LPD_PROP_CONTROL:
		platform_class_buckchg_ops_get_lpd_control(MAIN_BUCK_CHARGER,
							   &val);
		break;
	case LPD_PROP_UART_CONTROL:
		platform_class_buckchg_ops_get_lpd_uart_control(MAIN_BUCK_CHARGER,
								&val);
		break;
	default:
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t lpd_sysfs_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct mca_sysfs_attr_info *info;
	struct mca_lpd_dev *chip;
	int val = 0;

	info = mca_sysfs_lookup_attr(attr->attr.name, lpd_sysfs_field_tbl,
				     ARRAY_SIZE(lpd_sysfs_field_tbl));
	if (!info)
		return -1;

	chip = dev_get_drvdata(dev);
	if (!chip) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	switch (info->sysfs_attr_name) {
	case LPD_PROP_SBU1:
		platform_class_buckchg_ops_set_lpd_sbu1(MAIN_BUCK_CHARGER,
							val);
		break;
	case LPD_PROP_CHARGING:
		chip->lpd_charging = val;
		break;
	case LPD_PROP_CONTROL:
		platform_class_buckchg_ops_set_lpd_control(MAIN_BUCK_CHARGER,
							   val);
		break;
	case LPD_PROP_UART_CONTROL:
		platform_class_buckchg_ops_set_lpd_uart_control(MAIN_BUCK_CHARGER,
								val);
		break;
	default:
		break;
	}

	mca_log_info("set the %d ntc = %d\n", info->sysfs_attr_name, val);

	return count;
}

static struct attribute *lpd_attrs[ARRAY_SIZE(lpd_sysfs_field_tbl) + 1];
static const struct attribute_group lpd_sysfs_attr_group = {
	.attrs = lpd_attrs,
};

/* The subdirectory the port's readings appear under. */
#define LPD_DIR_NAME	"lpd"

static int mca_lpd_detect_probe(struct platform_device *pdev)
{
	struct mca_lpd_dev *chip;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	platform_set_drvdata(pdev, chip);
	chip->lpd_charging = 0;
	g_lpd = chip;

	mca_sysfs_init_attrs(lpd_attrs, lpd_sysfs_field_tbl,
			     ARRAY_SIZE(lpd_sysfs_field_tbl));
	mca_sysfs_create_link_group(MCA_SYSFS_DEV_HW_MONITOR, LPD_DIR_NAME,
				    chip->dev, &lpd_sysfs_attr_group);

	mca_log_err("%s success\n", __func__);

	return 0;
}

static int mca_lpd_detect_remove(struct platform_device *pdev)
{
	mca_sysfs_remove_link_group(MCA_SYSFS_DEV_HW_MONITOR, LPD_DIR_NAME,
				    &pdev->dev, &lpd_sysfs_attr_group);
	g_lpd = NULL;

	return 0;
}

static void mca_lpd_detect_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,lpd_detect" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver mca_lpd_detect_driver = {
	.driver = {
		.name		= "mca_lpd_detect",
		.of_match_table	= match_table,
	},
	.probe		= mca_lpd_detect_probe,
	.remove		= mca_lpd_detect_remove,
	.shutdown	= mca_lpd_detect_shutdown,
};
module_platform_driver(mca_lpd_detect_driver);

MODULE_DESCRIPTION("usb moisture detection");
MODULE_AUTHOR("zhouhongfei@xiaomi.com");
MODULE_LICENSE("GPL v2");
