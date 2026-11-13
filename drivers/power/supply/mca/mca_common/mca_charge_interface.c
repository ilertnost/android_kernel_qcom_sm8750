// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Where userspace asks for charging to change.  See
 * include/mca/common/mca_charge_interface.h.
 */

#define MCA_LOG_TAG "mca_charge_if"

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <mca/common/mca_charge_interface.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_sysfs.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>

/* Size of the buffers one request is parsed into and one line built in. */
#define MCA_CHARGE_IF_BUF_SIZE		256

/* Who a bare ship-mode write is recorded as, and which path it is for. */
#define MCA_CHARGE_IF_SHIPMODE_USER	"micharger"
#define MCA_CHARGE_IF_SHIPMODE_TYPE	"buck"

/* The subdirectory of xm_power/charger the attributes appear under. */
#define MCA_CHARGE_IF_DIR_NAME		"charge_interface"

/* What userspace can ask about. */
enum mca_charge_if_sysfs_type {
	MCA_CHRAGE_IF_SYSFS_INPUT_SUSPEND,
	MCA_CHRAGE_IF_SYSFS_CHARGE_ENABLE,
	MCA_CHARGE_IF_SYSFS_IIN_LIMIT,
	MCA_CHRAGE_IF_SYSFS_ICHG_LIMIT,
	MCA_CHRAGE_IF_SYSFS_POWER_LIMIT,
	MCA_CHRAGE_IF_SYSFS_SUSPEND_STATUS,
	MCA_CHRAGE_IF_SYSFS_SHIPMODE,
};

/* The names userspace uses for the charging paths. */
static const char * const g_mca_charge_if_op_type_table[MCA_CHARGE_IF_CHG_TYPE_END] = {
	[MCA_CHARGE_IF_CHG_TYPE_BUCK]			= "buck",
	[MCA_CHARGE_IF_CHG_TYPE_MAIN_BUCK]		= "main_buck",
	[MCA_CHARGE_IF_CHG_TYPE_AUX_BUCK]		= "aux_buck",
	[MCA_CHARGE_IF_CHG_TYPE_QC]			= "quick",
	[MCA_CHARGE_IF_CHG_TYPE_QC_MAIN_PATH]		= "quick_main",
	[MCA_CHARGE_IF_CHG_TYPE_QC_AUX_PATH]		= "quick_aux",
	[MCA_CHARGE_IF_CHG_TYPE_QC_DIV1]		= "div1",
	[MCA_CHARGE_IF_CHG_TYPE_QC_DIV2]		= "div2",
	[MCA_CHARGE_IF_CHG_TYPE_QC_DIV4]		= "div4",
	[MCA_CHARGE_IF_CHG_TYPE_WL_BUCK]		= "wl_buck",
	[MCA_CHARGE_IF_CHG_TYPE_WL_MAIN_BUCK]		= "wl_main_buck",
	[MCA_CHARGE_IF_CHG_TYPE_WL_AUX_BUCK]		= "wl_aux_buck",
	[MCA_CHARGE_IF_CHG_TYPE_WL_QC]			= "wl_quick",
	[MCA_CHARGE_IF_CHG_TYPE_WL_QC_MAIN_PATH]	= "wl_quick_main",
	[MCA_CHARGE_IF_CHG_TYPE_WL_QC_AUX_PATH]		= "wl_quick_aux",
	[MCA_CHARGE_IF_CHG_TYPE_WL_QC_DIV1]		= "wl_div1",
	[MCA_CHARGE_IF_CHG_TYPE_WL_QC_DIV2]		= "wl_div2",
	[MCA_CHARGE_IF_CHG_TYPE_WL_QC_DIV4]		= "wl_div4",
	[MCA_CHARGE_IF_CHG_TYPE_ALL]			= "all",
};

/* One implementation per charging path. */
static struct mca_charge_if_ops *g_mca_charge_if_ops[MCA_CHARGE_IF_CHG_TYPE_END];

static int mca_charge_if_get_op_type(const char *type_name)
{
	int i;

	for (i = MCA_CHARGE_IF_CHG_TYPE_BEGIN; i < MCA_CHARGE_IF_CHG_TYPE_END;
	     i++) {
		if (!strcmp(type_name, g_mca_charge_if_op_type_table[i]))
			return i;
	}

	mca_log_err("fail to find type_name %s\n", type_name);

	return -1;
}

int mca_charge_if_ops_register(struct mca_charge_if_ops *ops)
{
	int type;

	if (!ops || !ops->type_name)
		return -1;

	type = mca_charge_if_get_op_type(ops->type_name);
	if (type < 0) {
		mca_log_err("type is invalid %s\n", ops->type_name);
		return -1;
	}

	g_mca_charge_if_ops[type] = ops;

	return 0;
}
EXPORT_SYMBOL(mca_charge_if_ops_register);

static ssize_t mca_charge_if_sysfs_show(struct device *dev,
					struct device_attribute *attr,
					char *buf);
static ssize_t mca_charge_if_sysfs_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count);

static struct mca_sysfs_attr_info mca_charge_if_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(mca_charge_if_sysfs, 0640,
			  MCA_CHRAGE_IF_SYSFS_INPUT_SUSPEND, input_suspend),
	mca_sysfs_attr_rw(mca_charge_if_sysfs, 0640,
			  MCA_CHRAGE_IF_SYSFS_CHARGE_ENABLE, charge_enable),
	mca_sysfs_attr_rw(mca_charge_if_sysfs, 0640,
			  MCA_CHARGE_IF_SYSFS_IIN_LIMIT, iin_limit),
	mca_sysfs_attr_rw(mca_charge_if_sysfs, 0640,
			  MCA_CHRAGE_IF_SYSFS_ICHG_LIMIT, ichg_limit),
	mca_sysfs_attr_rw(mca_charge_if_sysfs, 0640,
			  MCA_CHRAGE_IF_SYSFS_POWER_LIMIT, power_limit),
	mca_sysfs_attr_rw(mca_charge_if_sysfs, 0640,
			  MCA_CHRAGE_IF_SYSFS_SUSPEND_STATUS, suspend_status),
	mca_sysfs_attr_rw(mca_charge_if_sysfs, 0640,
			  MCA_CHRAGE_IF_SYSFS_SHIPMODE, shipmode_count_reset),
};

/*
 * Whether any charging path has its input suspended.  Each path answers in
 * the form "eff_client:<client> <0|1>", and only the final digit counts.
 */
static ssize_t mca_charge_suspend_state_show(char *buf)
{
	char state[MCA_CHARGE_IF_BUF_SIZE];
	char line[MCA_CHARGE_IF_BUF_SIZE];
	struct mca_charge_if_ops *ops;
	bool suspended = false;
	size_t len;
	int i;

	for (i = MCA_CHARGE_IF_CHG_TYPE_BEGIN; i < MCA_CHARGE_IF_CHG_TYPE_ALL;
	     i++) {
		ops = g_mca_charge_if_ops[i];
		if (!ops || !ops->get_input_suspend)
			continue;

		memset(state, 0, sizeof(state));
		if (ops->get_input_suspend(state, ops->data))
			continue;

		scnprintf(line, sizeof(line), "%s %s\n",
			  g_mca_charge_if_op_type_table[i], state);
		len = strnlen(line, sizeof(line));
		if (len >= 2 && line[len - 2] != '0')
			suspended = true;
	}

	mca_log_info("suspend sts = %d", suspended);

	return snprintf(buf, PAGE_SIZE, "%d\n", suspended);
}

static ssize_t mca_charge_if_sysfs_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	int (*get)(char *buf, void *data);
	char value[MCA_CHARGE_IF_BUF_SIZE];
	char line[MCA_CHARGE_IF_BUF_SIZE];
	struct mca_sysfs_attr_info *info;
	struct mca_charge_if_ops *ops;
	bool status = false;
	size_t len, used;
	int i;

	info = mca_sysfs_lookup_attr(attr->attr.name,
				     mca_charge_if_sysfs_field_tbl,
				     ARRAY_SIZE(mca_charge_if_sysfs_field_tbl));
	if (!info)
		return -EINVAL;

	switch (info->sysfs_attr_name) {
	case MCA_CHRAGE_IF_SYSFS_SUSPEND_STATUS:
		return mca_charge_suspend_state_show(buf);
	case MCA_CHRAGE_IF_SYSFS_SHIPMODE:
		ops = g_mca_charge_if_ops[MCA_CHARGE_IF_CHG_TYPE_BUCK];
		if (ops && ops->get_ship_mode_status)
			ops->get_ship_mode_status(&status, ops->data);
		return snprintf(buf, PAGE_SIZE, "%d\n", status);
	default:
		break;
	}

	/* Every path that answers gets a line: "<path> <answer>". */
	for (i = MCA_CHARGE_IF_CHG_TYPE_BEGIN; i < MCA_CHARGE_IF_CHG_TYPE_ALL;
	     i++) {
		ops = g_mca_charge_if_ops[i];
		if (!ops)
			continue;

		switch (info->sysfs_attr_name) {
		case MCA_CHRAGE_IF_SYSFS_INPUT_SUSPEND:
			get = ops->get_input_suspend;
			break;
		case MCA_CHRAGE_IF_SYSFS_CHARGE_ENABLE:
			get = ops->get_charge_enable;
			break;
		case MCA_CHARGE_IF_SYSFS_IIN_LIMIT:
			get = ops->get_input_current_limit;
			break;
		case MCA_CHRAGE_IF_SYSFS_ICHG_LIMIT:
			get = ops->get_charge_current_limit;
			break;
		case MCA_CHRAGE_IF_SYSFS_POWER_LIMIT:
			get = ops->get_charge_power_limit;
			break;
		default:
			get = NULL;
			break;
		}
		if (!get)
			continue;

		memset(value, 0, sizeof(value));
		if (get(value, ops->data))
			continue;

		scnprintf(line, sizeof(line), "%s %s\n",
			  g_mca_charge_if_op_type_table[i], value);
		len = strnlen(line, sizeof(line));
		used = strnlen(buf, PAGE_SIZE);
		if (used + len >= PAGE_SIZE)
			break;

		memcpy(buf + used, line, len);
		buf[used + len] = '\0';
	}

	return strnlen(buf, PAGE_SIZE);
}

static ssize_t mca_charge_if_sysfs_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	char value[MCA_CHARGE_IF_BUF_SIZE] = { 0 };
	char type_name[MCA_CHARGE_IF_BUF_SIZE] = { 0 };
	char user[MCA_CHARGE_IF_BUF_SIZE] = { 0 };
	struct mca_sysfs_attr_info *info;
	struct mca_charge_if_ops *ops;
	int type, first, last, i;
	u32 val;

	info = mca_sysfs_lookup_attr(attr->attr.name,
				     mca_charge_if_sysfs_field_tbl,
				     ARRAY_SIZE(mca_charge_if_sysfs_field_tbl));
	if (!info)
		return -EINVAL;

	if (info->sysfs_attr_name == MCA_CHRAGE_IF_SYSFS_SHIPMODE) {
		/* Service tooling writes the bare value. */
		if (strscpy(value, buf, sizeof(value)) < 0)
			return -EINVAL;
		strscpy(type_name, MCA_CHARGE_IF_SHIPMODE_TYPE,
			sizeof(type_name));
		strscpy(user, MCA_CHARGE_IF_SHIPMODE_USER, sizeof(user));
	} else {
		if (count >= MCA_CHARGE_IF_BUF_SIZE - 2) {
			mca_log_err("input too long\n");
			return -EINVAL;
		}

		if (sscanf(buf, "%255s %255s %255s", user, type_name,
			   value) != 3) {
			mca_log_err("unable to parse input:%s\n", buf);
			return -EINVAL;
		}
	}

	type = mca_charge_if_get_op_type(type_name);
	if (type < 0)
		return -EINVAL;

	mca_log_info("user %s set node %d type %d value %s\n", user,
		     info->sysfs_attr_name, type, value);

	/* "all" is every path but itself. */
	if (type == MCA_CHARGE_IF_CHG_TYPE_ALL) {
		first = MCA_CHARGE_IF_CHG_TYPE_BEGIN;
		last = MCA_CHARGE_IF_CHG_TYPE_ALL;
	} else {
		first = type;
		last = type + 1;
	}

	for (i = first; i < last; i++) {
		ops = g_mca_charge_if_ops[i];
		val = 0;
		if (!ops)
			continue;

		/* A value that does not parse is taken as 0. */
		switch (info->sysfs_attr_name) {
		case MCA_CHRAGE_IF_SYSFS_INPUT_SUSPEND:
			mca_log_err("set_input_suspend user: %s, value: %s\n",
				    user, value);
			if (ops->set_input_suspend)
				ops->set_input_suspend(user, value, ops->data);
			break;
		case MCA_CHRAGE_IF_SYSFS_CHARGE_ENABLE:
			if (kstrtouint(value, 0, &val))
				val = 0;
			if (ops->set_charge_enable)
				ops->set_charge_enable(user, val, ops->data);
			break;
		case MCA_CHARGE_IF_SYSFS_IIN_LIMIT:
			if (ops->set_input_current_limit)
				ops->set_input_current_limit(user, value,
							     ops->data);
			break;
		case MCA_CHRAGE_IF_SYSFS_ICHG_LIMIT:
			if (ops->set_charge_current_limit)
				ops->set_charge_current_limit(user, value,
							      ops->data);
			break;
		case MCA_CHRAGE_IF_SYSFS_POWER_LIMIT:
			if (kstrtouint(value, 0, &val))
				val = 0;
			if (ops->set_charge_power_limit)
				ops->set_charge_power_limit(user, val,
							    ops->data);
			break;
		case MCA_CHRAGE_IF_SYSFS_SHIPMODE:
			if (kstrtouint(value, 10, &val))
				val = 0;
			if (ops->set_ship_mode_en)
				ops->set_ship_mode_en(user, val, ops->data);
			break;
		default:
			break;
		}
	}

	/* Whatever watches the battery re-reads its state. */
	if (info->sysfs_attr_name == MCA_CHRAGE_IF_SYSFS_INPUT_SUSPEND)
		mca_event_block_notify(MCA_EVENT_TYPE_BATTERY_INFO,
				       MCA_EVENT_BATTERY_STS_CHANGE, NULL);

	return count;
}

static struct attribute *mca_charge_if_sysfs_attrs[ARRAY_SIZE(mca_charge_if_sysfs_field_tbl) + 1];

static const struct attribute_group mca_charge_if_sysfs_attr_group = {
	.attrs = mca_charge_if_sysfs_attrs,
};

static int mca_charge_if_probe(struct platform_device *pdev)
{
	mca_sysfs_init_attrs(mca_charge_if_sysfs_attrs,
			     mca_charge_if_sysfs_field_tbl,
			     ARRAY_SIZE(mca_charge_if_sysfs_field_tbl));

	if (mca_sysfs_create_link_group(MCA_SYSFS_DEV_CHARGER,
					MCA_CHARGE_IF_DIR_NAME, &pdev->dev,
					&mca_charge_if_sysfs_attr_group)) {
		mca_log_err("create sysfs failed\n");
		return -1;
	}

	return 0;
}

static int mca_charge_if_remove(struct platform_device *pdev)
{
	mca_sysfs_remove_link_group(MCA_SYSFS_DEV_CHARGER,
				    MCA_CHARGE_IF_DIR_NAME, &pdev->dev,
				    &mca_charge_if_sysfs_attr_group);

	return 0;
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,charge_interface" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver charge_interface_driver = {
	.driver = {
		.name		= "mca_charge_interface",
		.of_match_table	= match_table,
	},
	.probe		= mca_charge_if_probe,
	.remove		= mca_charge_if_remove,
};
module_platform_driver(charge_interface_driver);

MODULE_DESCRIPTION("xiaomi charge interface driver");
MODULE_AUTHOR("lipeng43@xiaomi.com");
MODULE_LICENSE("GPL v2");
