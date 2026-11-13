// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The load switches between the charger and the battery.  See
 * include/mca/platform/platform_loadsw_class.h.
 *
 * Each switch also gets a sysfs directory of its own, named from the node's
 * loadsw-dir-list, so userspace can see whether it answered.
 */

#define MCA_LOG_TAG "loadsw_class"

#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/platform/platform_loadsw_class.h>
#include <mca/common/mca_sysfs.h>

/**
 * struct platform_class_loadsw_ops_data - one registered load switch
 * @ops:  what its driver provides
 * @data: handed back to every call
 */
struct platform_class_loadsw_ops_data {
	const struct platform_class_loadsw_ops	*ops;
	void					*data;
};

static struct platform_class_loadsw_ops_data mca_loadsw[LOADSW_ROLE_MAX];

int platform_class_loadsw_register_ops(
				       enum platform_class_load_switch_role role,
				       const struct platform_class_loadsw_ops *ops,
				       void *data)
{
	if (role >= LOADSW_ROLE_MAX || !ops || !data)
		return -EOPNOTSUPP;

	mca_loadsw[role].ops = ops;
	mca_loadsw[role].data = data;

	return 0;
}
EXPORT_SYMBOL(platform_class_loadsw_register_ops);

#define MCA_LOADSW_CALL(role, method, ...)				\
({									\
	int __ret = -1;						\
									\
	if ((role) < LOADSW_ROLE_MAX &&					\
	    mca_loadsw[role].ops && mca_loadsw[role].ops->method)	\
		__ret = mca_loadsw[role].ops->method(			\
				__VA_ARGS__, mca_loadsw[role].data);	\
	__ret;								\
})

int platform_class_loadsw_get_present(
				      enum platform_class_load_switch_role role,
				      bool *present)
{
	return MCA_LOADSW_CALL(role, loadsw_get_present, present);
}
EXPORT_SYMBOL(platform_class_loadsw_get_present);

int platform_class_loadsw_get_ibat_limit(
					 enum platform_class_load_switch_role role,
					 int *ibat_ma)
{
	return MCA_LOADSW_CALL(role, loadsw_get_ibat_limit, ibat_ma);
}
EXPORT_SYMBOL(platform_class_loadsw_get_ibat_limit);

int platform_class_loadsw_set_ibat_limit(
					 enum platform_class_load_switch_role role,
					 int ibat_ma)
{
	return MCA_LOADSW_CALL(role, loadsw_set_ibat_limit, ibat_ma);
}
EXPORT_SYMBOL(platform_class_loadsw_set_ibat_limit);

int platform_class_loadsw_get_lowpower_mode(
					    enum platform_class_load_switch_role role,
					    bool *enable)
{
	return MCA_LOADSW_CALL(role, loadsw_get_lowpower_mode, enable);
}
EXPORT_SYMBOL(platform_class_loadsw_get_lowpower_mode);

int platform_class_loadsw_set_lowpower_mode(
					    enum platform_class_load_switch_role role,
					    bool enable)
{
	return MCA_LOADSW_CALL(role, loadsw_set_lowpower_mode, enable);
}
EXPORT_SYMBOL(platform_class_loadsw_set_lowpower_mode);

/**
 * struct platform_loadsw_dev - the switches this board has
 * @dev:              this device
 * @loadsw_num:       how many of them
 * @loadsw_dir_list:  the sysfs directory each one appears under
 * @sysfs_dev:        the device created for each directory
 * @loadsw_dev_index: which switch each of those directories belongs to
 * @low_power:        whether low power mode was asked for
 *
 * @loadsw_dev_index is what the attribute handlers are given as the device's
 * driver data, so an attribute read on one directory knows which switch it is
 * being asked about without a handler per switch.
 */
struct platform_loadsw_dev {
	struct device	*dev;
	int		loadsw_num;
	const char	*loadsw_dir_list[LOADSW_ROLE_MAX];
	struct device	*sysfs_dev[LOADSW_ROLE_MAX];
	int		loadsw_dev_index[LOADSW_ROLE_MAX];
	bool		low_power;
};

/* Where a switch's directory appears, and what its name is matched against. */
#define LOADSW_CLASS_NAME	"xm_power"
#define LOADSW_NAME_MASTER	"master"
#define LOADSW_NAME_SLAVE	"slave"

/*
 * What an attribute read reports back.  A single value, but reached through
 * calls that answer in different types.
 */
union platform_loadsw_propval {
	unsigned int	uintval;
	int		intval;
	char		strval[200];
	bool		boolval;
};

static struct platform_loadsw_dev *g_loadsw_dev;
static int probe_cnt;

static ssize_t loadsw_sysfs_show(struct device *dev,
				 struct device_attribute *attr, char *buf);
static ssize_t loadsw_sysfs_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count);

static struct mca_sysfs_attr_info loadsw_sysfs_field_tbl[] = {
	mca_sysfs_attr_ro(loadsw_sysfs, 0440, LOADSW_PROP_CHIP_OK, chip_ok),
	mca_sysfs_attr_rw(loadsw_sysfs, 0664, LOADSW_PROP_IBAT_LIMIT,
			  ibat_limit),
	mca_sysfs_attr_rw(loadsw_sysfs, 0664, LOADSW_PROP_LOW_POWER,
			  low_power),
};

static struct attribute *loadsw_attrs[ARRAY_SIZE(loadsw_sysfs_field_tbl) + 1];
static const struct attribute_group loadsw_group = {
	.attrs = loadsw_attrs,
};

static ssize_t loadsw_sysfs_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	union platform_loadsw_propval val = { 0 };
	struct mca_sysfs_attr_info *info;
	int *role;
	int ret;

	info = mca_sysfs_lookup_attr(attr->attr.name, loadsw_sysfs_field_tbl,
				     ARRAY_SIZE(loadsw_sysfs_field_tbl));
	if (!info)
		return -1;

	role = dev_get_drvdata(dev);
	if (!role) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	mca_log_info("%s dev_driverdata is %d\n", __func__, *role);

	switch (info->sysfs_attr_name) {
	case LOADSW_PROP_CHIP_OK:
		platform_class_loadsw_get_present(*role, &val.boolval);
		ret = val.boolval;
		break;
	case LOADSW_PROP_IBAT_LIMIT:
		platform_class_loadsw_get_ibat_limit(*role, &val.intval);
		ret = val.intval;
		break;
	case LOADSW_PROP_LOW_POWER:
		platform_class_loadsw_get_lowpower_mode(*role, &val.boolval);
		ret = val.boolval;
		break;
	default:
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", ret);
}

static ssize_t loadsw_sysfs_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct mca_sysfs_attr_info *info;
	int *role;
	int val = 0;

	info = mca_sysfs_lookup_attr(attr->attr.name, loadsw_sysfs_field_tbl,
				     ARRAY_SIZE(loadsw_sysfs_field_tbl));
	if (!info)
		return -1;

	role = dev_get_drvdata(dev);
	if (!role) {
		mca_log_err("dev_driverdata is null\n");
		return -1;
	}

	mca_log_info("dev_driverdata is: %d, attr: %d, buf: %s\n", *role,
		     info->sysfs_attr_name, buf);

	switch (info->sysfs_attr_name) {
	case LOADSW_PROP_IBAT_LIMIT:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;

		platform_class_loadsw_set_ibat_limit(*role, val);
		mca_log_info("set ibat limit: %d\n", val);
		break;
	case LOADSW_PROP_LOW_POWER:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;

		platform_class_loadsw_set_lowpower_mode(*role, !!val);
		mca_log_info("set low power %d\n", val);
		break;
	default:
		break;
	}

	return count;
}

/*
 * Each switch gets a directory of its own, named from the node's
 * loadsw-dir-list.  Which switch a directory belongs to is taken from the
 * name it was given rather than from its position, so a board that lists
 * them the other way round still reaches the right one.
 */
static void loadsw_sysfs_create_group(struct platform_loadsw_dev *chip)
{
	const char *name;
	int role;
	int i;

	for (i = 0; i < chip->loadsw_num && i < LOADSW_ROLE_MAX; i++) {
		chip->sysfs_dev[i] =
			mca_sysfs_create_group(LOADSW_CLASS_NAME,
					       chip->loadsw_dir_list[i],
					       &loadsw_group);
		if (!chip->sysfs_dev[i])
			mca_log_err("creat loadsw[%d] sysfs fail\n", i);
	}

	for (i = 0; i < chip->loadsw_num && i < LOADSW_ROLE_MAX; i++) {
		name = dev_name(chip->sysfs_dev[i]);

		if (strstr(name, LOADSW_NAME_MASTER))
			role = LOADSW_ROLE_MASTER;
		else if (strstr(name, LOADSW_NAME_SLAVE))
			role = LOADSW_ROLE_SLAVE;
		else
			continue;

		chip->loadsw_dev_index[i] = role;
		dev_set_drvdata(chip->sysfs_dev[i],
				&chip->loadsw_dev_index[i]);
		mca_log_err("success match loadsw_dev_name = %s, loadsw_dev_list[%d]=%s\n",
			    name, role,
			    role == LOADSW_ROLE_MASTER ? LOADSW_NAME_MASTER :
							 LOADSW_NAME_SLAVE);
	}
}

static int platform_loadsw_dev_parse_dt(struct platform_loadsw_dev *chip,
					struct device_node *np)
{
	int count;
	int rc;
	int i;

	if (!np) {
		mca_log_err("device tree info missing\n");
		return -1;
	}

	rc = mca_parse_dts_u32(np, "loadsw-num", &chip->loadsw_num, 1);
	if (rc) {
		mca_log_err("get loadsw-num fail\n");
		return rc;
	}

	count = mca_parse_dts_count_strings(np, "loadsw-dir-list",
					    LOADSW_ROLE_MAX, 1);
	mca_log_info("loadsw dir list max count: %d, %d\n", count,
		     chip->loadsw_num);
	if (count != chip->loadsw_num)
		mca_log_err("loadsw_num can't match loadsw_dir_list count\n");

	for (i = 0; i < count; i++) {
		rc = mca_parse_dts_string_index(np, "loadsw-dir-list", i,
						&chip->loadsw_dir_list[i]);
		if (rc < 0) {
			mca_log_err("Unable to read loadsw-dir-list strings[%d]\n",
				    i);
			return rc;
		}
	}

	mca_log_info("%s success\n", __func__);

	return 0;
}

static int platform_loadsw_class_probe(struct platform_device *pdev)
{
	struct platform_loadsw_dev *chip;
	int rc;

	mca_log_err("%s begin cnt %d\n", __func__, probe_cnt++);

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	platform_set_drvdata(pdev, chip);

	rc = platform_loadsw_dev_parse_dt(chip, pdev->dev.of_node);
	if (rc) {
		mca_log_err("%s Couldn't parse device tree rc=%d\n", __func__,
			    rc);
		return rc;
	}

	g_loadsw_dev = chip;

	mca_sysfs_init_attrs(loadsw_attrs, loadsw_sysfs_field_tbl,
			     ARRAY_SIZE(loadsw_sysfs_field_tbl));
	loadsw_sysfs_create_group(chip);

	mca_log_err("%s success %d\n", __func__, probe_cnt++);

	return 0;
}

static int platform_loadsw_class_remove(struct platform_device *pdev)
{
	return 0;
}

static void platform_loadsw_class_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id mca_loadsw_match[] = {
	{ .compatible = "mca,platform_loadsw" },
	{ }
};
MODULE_DEVICE_TABLE(of, mca_loadsw_match);

static struct platform_driver mca_loadsw_driver = {
	.driver = {
		.name = "platform_loadsw_class",
		.of_match_table = mca_loadsw_match,
	},
	.probe = platform_loadsw_class_probe,
	.remove = platform_loadsw_class_remove,
	.shutdown = platform_loadsw_class_shutdown,
};
module_platform_driver(mca_loadsw_driver);

MODULE_DESCRIPTION("platform loadsw class");
MODULE_AUTHOR("yinshunan@xiaomi.com");
MODULE_LICENSE("GPL v2");
