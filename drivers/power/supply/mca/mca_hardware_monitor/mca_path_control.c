// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Which way the power flows.  See
 * include/mca/hardware/hw_path_control.h.
 */

#define MCA_LOG_TAG "mca_path_control"

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/kstrtox.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/hardware/hw_path_control.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/platform/platform_cp_class.h>
#include <mca/platform/platform_wireless_class.h>
#include "../mca_strategy/strategy_wireless/inc/mca_wireless_revchg.h"
#include <mca/common/mca_sysfs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

/* How many gates a board may have, and how many combinations of sources. */
#define PATH_CONTROL_ROLE_MAX		5
#define PATH_CONTROL_PATH_MAX		16

/* How long to leave the providers to register before the first poll. */
#define PATH_CONTROL_FIRST_POLL_MS	10000

/* How many gates one row may name. */
#define PATH_CONTROL_GATE_MAX		5

/* How many strings describe one entry of each list. */
#define PATH_CONTROL_SCHEME_COLS	3
#define PATH_CONTROL_CONDITION_COLS	2
#define PATH_CONTROL_PATH_COLS		5
#define PATH_CONTROL_GATE_COLS		2

/* A field of a row that this board leaves out. */
#define PATH_CONTROL_NULL_PARA		"null"

/* Which gate. */
enum path_control_role_para {
	OVPGATE_ROLE,
	WPCGATE_ROLE,
	EXT_BST_GATE_ROLE,
	WLS_VDD_GATE_ROLE,
	HBST_GATE_ROLE,
	MAX_ROLE,
};

/* How a gate is driven. */
enum path_control_scheme {
	GPIO_SCHEME,
	PMIC_REGISTER_SCHEME,
	CP_CHIP_SCHEME,
	MAX_SCHEME,
};

/* The columns of a "control_scheme" row. */
enum path_control_scheme_item {
	GATE_ROLE,
	CONTROL_SCHEME,
	PROCESS_WAY,
	ITEM_MAX,
};

/* The boosts a path row can be conditioned on. */
enum boost_type_info {
	OTG_BOOST_TYPE,
	WLS_REV_BOOST_TYPE,
	BOOST_TYPE_MAX,
};

/* The column of a path row that names its list of gates. */
#define PATH_CONTROL_PROCESS_METHOD	4

/* A boost value in a path row that matches whatever the phone is running. */
#define PATH_CONTROL_BOOST_ANY		(-1)

/* What userspace can look at. */
enum path_control_attr_list {
	USB_IN_PATH_CONTROL,
	WLS_IN_PATH_CONTROL,
	WLS_REV_PATH_CONTROL,
	OTG_IN_PATH_CONTROL,
	WLS_VDD_PATH_CONTROL,
	TOTAL_PATH_CONTROL,
	PATH_CONTROL_MAX,
};

/**
 * struct gate_enable_info - one gate, and what to do with it
 * @control_gate_role:   which gate
 * @control_gate_enable: whether to open it
 */
struct gate_enable_info {
	int control_gate_role;
	int control_gate_enable;
};

/**
 * struct path_control_boost_para - what to do under one boost arrangement
 * @boost_type:            what the OTG boost source and the reverse wireless
 *                         boost source must be for this row to apply
 * @gate_enable_para_size: how many gates the row drives
 * @gate_enable_info:      the gates and what to do with them
 */
struct path_control_boost_para {
	int			boost_type[BOOST_TYPE_MAX];
	int			gate_enable_para_size;
	struct gate_enable_info	*gate_enable_info;
};

/**
 * struct path_control_condition_cfg - what one combination of sources means
 * @condition:     the sources that are in force
 * @boost_cfg_num: how many boost arrangements it distinguishes
 * @boost_cfg:     what to do in each
 */
struct path_control_condition_cfg {
	int				condition;
	int				boost_cfg_num;
	struct path_control_boost_para	*boost_cfg;
};

/**
 * struct path_control_scheme_cfg - how one gate is reached
 * @gate_role:           which gate
 * @scheme:              how it is driven
 * @gpio:                the pin, when it is one
 * @process_way:         the register setting, when the charger drives it
 * @cp_chip_role:        which charge pump owns it, when one does
 * @control_enable_func: what to call to drive it; NULL when the gate needs
 *                       nothing done or could not be set up
 */
struct path_control_scheme_cfg {
	int	gate_role;
	int	scheme;
	int	gpio;
	int	process_way;
	int	cp_chip_role;
	int	(*control_enable_func)(int role, bool enable);
};

/* Where the gates appear to userspace. */
#define PATH_CONTROL_DIR_NAME	"path_control"

/**
 * struct mca_path_control - the gates on this board
 * @dev:                  this device
 * @enable_handling_lock: one caller changes the gates at a time
 * @update_condition_work: rereads what is connected and what is boosting
 * @gate_sts:             what each gate was last driven to, indexed by role
 * @condition_value:      the sources currently in force, as bits
 * @boost_src_default:    which boost supplies OTG, and which supplies reverse
 *                        wireless charging
 * @otg_boost_enable_sts: whether the OTG boost is running
 * @usb_online:           charging by cable
 * @wireless_online:      charging by pad
 * @wireless_rev_en:      supplying a pad
 * @wireless_vdd_en:      the wireless rail is powered
 * @control_scheme:       how each gate is reached
 * @control_path:         what each combination of sources means
 */
struct mca_path_control {
	struct device			*dev;
	struct mutex			enable_handling_lock;
	struct delayed_work		update_condition_work;
	int				*gate_sts;
	int				condition_value;
	int				boost_src_default[BOOST_TYPE_MAX];
	int				otg_boost_enable_sts;
	int				usb_online;
	int				wireless_online;
	bool				wireless_rev_en;
	bool				wireless_vdd_en;
	struct path_control_scheme_cfg	control_scheme[PATH_CONTROL_ROLE_MAX];
	struct path_control_condition_cfg control_path[PATH_CONTROL_PATH_MAX];
};

static struct mca_path_control *g_info;

/* Which of the gate descriptions is for one gate. */
static struct path_control_scheme_cfg *
mca_path_control_find_scheme(struct mca_path_control *chip, int role)
{
	int i;

	for (i = 0; i < PATH_CONTROL_ROLE_MAX; i++) {
		if (chip->control_scheme[i].gate_role == role)
			return &chip->control_scheme[i];
	}

	return NULL;
}

/* A gate on a pin, driven high to open it. */
static int mac_path_control_gpio_scheme_func(int role, bool enable)
{
	struct path_control_scheme_cfg *cfg;
	int gpio;
	int ret;

	cfg = mca_path_control_find_scheme(g_info, role);
	if (!cfg)
		return 0;

	gpio = cfg->gpio;
	ret = gpiod_direction_output_raw(gpio_to_desc(gpio), enable);
	if (ret)
		mca_log_err("set direction for enable-gate-gpio[%d] failed\n",
			    gpio);

	mca_log_info("enable-gate-gpio[%d] val is :%d\n", gpio,
		     gpiod_get_raw_value(gpio_to_desc(gpio)));

	return ret;
}

/*
 * A gate inside the charger itself.  Nothing is wired this way; stock leaves
 * it unimplemented as well.
 */
static int mac_path_control_pmic_register_scheme_func(int role, bool enable)
{
	return 0;
}

/* A gate inside a charge pump. */
static int mac_path_control_cp_chip_scheme_func(int role, bool enable)
{
	struct path_control_scheme_cfg *cfg;

	cfg = mca_path_control_find_scheme(g_info, role);
	if (!cfg)
		return 0;

	switch (role) {
	case OVPGATE_ROLE:
		mca_log_info("set ovpgate: %d\n", enable);
		platform_class_cp_enable_ovpgate(cfg->cp_chip_role, enable);
		break;
	case WPCGATE_ROLE:
		/* the wireless gate is driven by the wireless receiver itself */
		mca_log_info("set wpcgate: %d\n", enable);
		break;
	default:
		mca_log_err("this switch cp cannot control, exit\n");
		break;
	}

	return 0;
}

/*
 * A source being connected or disconnected changes which gates should be
 * open, and the board's table says which.  The row is picked on the sources
 * in force and then on which boosts are supplying them; each gate it names is
 * then driven, unless it is already where the row wants it.
 */
static int mca_path_control_handle_func(struct mca_path_control *chip,
					int condition)
{
	struct path_control_condition_cfg *path = NULL;
	struct path_control_boost_para *boost;
	struct path_control_scheme_cfg *cfg;
	int i, j;

	for (i = 0; i < PATH_CONTROL_PATH_MAX; i++) {
		if (chip->control_path[i].condition == condition) {
			path = &chip->control_path[i];
			break;
		}
	}

	if (!path) {
		mca_log_err("this condition is not exit\n");
		return -1;
	}

	mca_log_info("condition is %d\n", condition);

	for (i = 0; i < path->boost_cfg_num; i++) {
		boost = &path->boost_cfg[i];

		if ((boost->boost_type[OTG_BOOST_TYPE] == PATH_CONTROL_BOOST_ANY ||
		     boost->boost_type[OTG_BOOST_TYPE] ==
		     chip->boost_src_default[OTG_BOOST_TYPE]) &&
		    (boost->boost_type[WLS_REV_BOOST_TYPE] == PATH_CONTROL_BOOST_ANY ||
		     boost->boost_type[WLS_REV_BOOST_TYPE] ==
		     chip->boost_src_default[WLS_REV_BOOST_TYPE]))
			break;
	}

	if (i == path->boost_cfg_num) {
		mca_log_err("this boost combination is not exit\n");
		return -1;
	}

	mca_log_info("boost combination index is %d\n", i);

	boost = &path->boost_cfg[i];
	for (j = 0; j < boost->gate_enable_para_size; j++) {
		int role = boost->gate_enable_info[j].control_gate_role;
		int enable = boost->gate_enable_info[j].control_gate_enable;

		mca_log_info("gate[%d] ready to enable[%d]\n", role, enable);

		if (role < 0 || role >= PATH_CONTROL_ROLE_MAX)
			continue;

		if (enable == chip->gate_sts[role])
			continue;

		/* the description for a gate is the row of its own number */
		cfg = &chip->control_scheme[role];
		if (!cfg->control_enable_func)
			continue;

		cfg->control_enable_func(role, enable != 0);
		chip->gate_sts[role] = enable;
	}

	return 0;
}

/**
 * mca_path_control_enable_gate() - connect or disconnect one source
 * @src:    which source
 * @enable: whether it should be connected
 *
 * The caller names the source rather than the gates, because which gates a
 * source needs is a property of the board and not of the caller.  A value
 * that is not a single source leaves the sources alone and applies the row
 * for no source at all.
 */
int mca_path_control_enable_gate(CONTROL_SRC src, bool enable)
{
	struct mca_path_control *chip = g_info;
	int condition = 0;
	int ret;

	if (!chip)
		return -1;

	mca_log_info("src: %d, enable: %d\n", src, enable);

	mutex_lock(&chip->enable_handling_lock);

	switch (src) {
	case PATH_CONTROL_USB:
	case PATH_CONTROL_WLS:
	case PATH_CONTROL_WLS_REV:
	case PATH_CONTROL_OTG:
	case PATH_CONTROL_VDD:
		if (enable)
			chip->condition_value |= src;
		else
			chip->condition_value &= ~src;
		condition = chip->condition_value;
		mca_log_info("path's value is %d\n", condition);
		break;
	default:
		break;
	}

	ret = mca_path_control_handle_func(chip, condition);

	mutex_unlock(&chip->enable_handling_lock);

	return ret;
}
EXPORT_SYMBOL(mca_path_control_enable_gate);

/*
 * What the phone is doing, as one number.  This only records it: the gates
 * are driven when a source is switched through mca_path_control_enable_gate().
 */
static void mca_path_control_update_status_work(struct work_struct *work)
{
	struct mca_path_control *chip =
		container_of(work, struct mca_path_control,
			     update_condition_work.work);
	int condition;

	mca_log_info("update condition status work\n");

	platform_class_buckchg_ops_get_otg_boost_src(MAIN_BUCK_CHARGER,
			&chip->boost_src_default[OTG_BOOST_TYPE]);
	mca_wireless_rev_get_rev_boost_default(
			&chip->boost_src_default[WLS_REV_BOOST_TYPE]);
	mca_log_info("otg_boost_src is %d, wls_rev_boost_default is %d\n",
		     chip->boost_src_default[OTG_BOOST_TYPE],
		     chip->boost_src_default[WLS_REV_BOOST_TYPE]);

	platform_class_buckchg_ops_get_online(MAIN_BUCK_CHARGER,
					      &chip->usb_online);
	platform_class_buckchg_ops_get_otg_boost_enable_status(MAIN_BUCK_CHARGER,
							       &chip->otg_boost_enable_sts);
	platform_class_wireless_is_present(WIRELESS_ROLE_MASTER,
					   &chip->wireless_online);
	mca_wireless_rev_get_reverse_chg(&chip->wireless_rev_en);
	mca_log_info("usb_online: %d, wireless_online: %d, otg_boost_enable_sts: %d, wireless_rev_en: %d\n",
		     chip->usb_online, chip->wireless_online,
		     chip->otg_boost_enable_sts, chip->wireless_rev_en);

	condition = chip->condition_value & ~(PATH_CONTROL_USB | PATH_CONTROL_WLS |
					      PATH_CONTROL_WLS_REV |
					      PATH_CONTROL_OTG | PATH_CONTROL_VDD);

	if (chip->usb_online)
		condition |= PATH_CONTROL_USB;
	if (chip->wireless_online)
		condition |= PATH_CONTROL_WLS;
	if (chip->wireless_rev_en)
		condition |= PATH_CONTROL_WLS_REV;
	/* the OTG boost counts while it is being enabled and once it is on */
	if (chip->otg_boost_enable_sts == 1 || chip->otg_boost_enable_sts == 2)
		condition |= PATH_CONTROL_OTG;
	if (chip->wireless_vdd_en)
		condition |= PATH_CONTROL_VDD;

	chip->condition_value = condition;
}

static ssize_t path_control_sysfs_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf);
static ssize_t path_control_sysfs_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count);

static struct mca_sysfs_attr_info path_control_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(path_control_sysfs, 0664, USB_IN_PATH_CONTROL,
			  usb_in),
	mca_sysfs_attr_rw(path_control_sysfs, 0664, WLS_IN_PATH_CONTROL,
			  wls_in),
	mca_sysfs_attr_rw(path_control_sysfs, 0664, WLS_REV_PATH_CONTROL,
			  wls_rev),
	mca_sysfs_attr_rw(path_control_sysfs, 0664, OTG_IN_PATH_CONTROL,
			  otg_in),
	mca_sysfs_attr_rw(path_control_sysfs, 0664, WLS_VDD_PATH_CONTROL,
			  wls_vdd),
	mca_sysfs_attr_rw(path_control_sysfs, 0664, TOTAL_PATH_CONTROL,
			  control_value),
};

static struct attribute *path_control_attrs[ARRAY_SIZE(path_control_sysfs_field_tbl) + 1];
static const struct attribute_group path_control_sysfs_attr_group = {
	.attrs = path_control_attrs,
};

static ssize_t path_control_sysfs_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct mca_path_control *chip = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *info;
	int val;

	info = mca_sysfs_lookup_attr(attr->attr.name,
				     path_control_sysfs_field_tbl,
				     ARRAY_SIZE(path_control_sysfs_field_tbl));
	if (!info)
		return -1;

	if (!chip) {
		mca_log_err("dev_driverdata is null\n");
		return -1;
	}

	switch (info->sysfs_attr_name) {
	case USB_IN_PATH_CONTROL:
		val = chip->condition_value & PATH_CONTROL_USB;
		break;
	case WLS_IN_PATH_CONTROL:
		val = chip->condition_value & PATH_CONTROL_WLS;
		break;
	case WLS_REV_PATH_CONTROL:
		val = chip->condition_value & PATH_CONTROL_WLS_REV;
		break;
	case OTG_IN_PATH_CONTROL:
		val = chip->condition_value & PATH_CONTROL_OTG;
		break;
	case WLS_VDD_PATH_CONTROL:
		val = chip->condition_value & PATH_CONTROL_VDD;
		break;
	case TOTAL_PATH_CONTROL:
		val = chip->condition_value;
		break;
	default:
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t path_control_sysfs_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct mca_path_control *chip = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *info;
	int val = 0;

	info = mca_sysfs_lookup_attr(attr->attr.name,
				     path_control_sysfs_field_tbl,
				     ARRAY_SIZE(path_control_sysfs_field_tbl));
	if (!info)
		return -1;

	if (!chip) {
		mca_log_err("dev_driverdata is null\n");
		return -1;
	}

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	switch (info->sysfs_attr_name) {
	case USB_IN_PATH_CONTROL:
		mca_path_control_enable_gate(PATH_CONTROL_USB, val);
		break;
	case WLS_IN_PATH_CONTROL:
		mca_path_control_enable_gate(PATH_CONTROL_WLS, val);
		break;
	case WLS_REV_PATH_CONTROL:
		mca_path_control_enable_gate(PATH_CONTROL_WLS_REV, val);
		break;
	case OTG_IN_PATH_CONTROL:
		mca_path_control_enable_gate(PATH_CONTROL_OTG, val);
		break;
	case WLS_VDD_PATH_CONTROL:
		mca_path_control_enable_gate(PATH_CONTROL_VDD, val);
		break;
	case TOTAL_PATH_CONTROL:
		/*
		 * Applies the row for the value written without recording it
		 * as the sources in force, for testing a combination.
		 */
		mca_path_control_handle_func(chip, val);
		break;
	default:
		break;
	}

	mca_log_info("set %d, enable = %d\n", info->sysfs_attr_name, val);

	return count;
}

/*
 * Everything the board tells us is written as lists of strings, because a
 * row names the property that holds the next list down.
 */

/* The gates one path row drives: pairs of gate role and wanted state. */
static int mca_path_control_parse_condition_process(struct mca_path_control *chip,
						    struct path_control_boost_para *boost,
						    const char *prop)
{
	struct device_node *np = chip->dev->of_node;
	struct gate_enable_info *info;
	const char *field = NULL;
	int count;
	int i;

	count = mca_parse_dts_count_strings(np, prop, PATH_CONTROL_GATE_MAX,
					    PATH_CONTROL_GATE_COLS);
	if (count < 0) {
		mca_log_err("parse %s failed\n", prop);
		return -1;
	}

	boost->gate_enable_para_size = count / PATH_CONTROL_GATE_COLS;
	info = kcalloc(boost->gate_enable_para_size, sizeof(*info),
		       GFP_KERNEL);
	boost->gate_enable_info = info;
	if (!info) {
		mca_log_err("gate_enable para no mem\n");
		return -1;
	}

	for (i = 0; i < count; i++) {
		int *val;

		if (mca_parse_dts_string_index(np, prop, i, &field))
			goto err;

		mca_log_debug("[%d]control_gate_para %s\n", i, field);

		if (i % PATH_CONTROL_GATE_COLS == 0)
			val = &info[i / PATH_CONTROL_GATE_COLS].control_gate_role;
		else
			val = &info[i / PATH_CONTROL_GATE_COLS].control_gate_enable;

		if (kstrtoint(field, 10, val))
			goto err;
	}

	return 0;

err:
	kfree(info);
	boost->gate_enable_info = NULL;
	boost->gate_enable_para_size = 0;

	return -1;
}

/*
 * The boost arrangements one combination of sources distinguishes.  Each row
 * is "<boost type> <source> <boost type> <source> <gate list>", the boost
 * type picking which of the two boosts the following value is matched with.
 */
static int mca_path_control_parse_condition_path(struct mca_path_control *chip,
						 struct path_control_condition_cfg *path,
						 const char *prop)
{
	struct device_node *np = chip->dev->of_node;
	struct path_control_boost_para *boost_cfg;
	const char *field = NULL;
	int boost_type = 0;
	int count;
	int i;

	if (!strcmp(prop, PATH_CONTROL_NULL_PARA)) {
		mca_log_info("no need parse process para\n");
		return 0;
	}

	count = mca_parse_dts_count_strings(np, prop, PATH_CONTROL_PATH_MAX,
					    PATH_CONTROL_PATH_COLS);
	if (count < 0) {
		mca_log_err("parse %s failed\n", prop);
		return -1;
	}

	path->boost_cfg_num = count / PATH_CONTROL_PATH_COLS;
	boost_cfg = kcalloc(path->boost_cfg_num, sizeof(*boost_cfg),
			    GFP_KERNEL);
	path->boost_cfg = boost_cfg;
	if (!boost_cfg) {
		mca_log_err("boost cfg no mem\n");
		return -ENOMEM;
	}

	for (i = 0; i < count; i++) {
		struct path_control_boost_para *boost =
			&boost_cfg[i / PATH_CONTROL_PATH_COLS];
		int col = i % PATH_CONTROL_PATH_COLS;

		if (mca_parse_dts_string_index(np, prop, i, &field))
			goto err;

		mca_log_debug("[%d]process para %s\n", i, field);

		if (col == PATH_CONTROL_PROCESS_METHOD) {
			if (!strcmp(field, PATH_CONTROL_NULL_PARA)) {
				mca_log_info("no need parse process_action para\n");
				continue;
			}

			if (mca_path_control_parse_condition_process(chip,
								     boost,
								     field))
				goto err;
		} else if (!(col & 1)) {
			/* an unreadable boost type keeps the previous one */
			if (kstrtoint(field, 10, &boost_type) &&
			    boost_type >= BOOST_TYPE_MAX)
				goto err;
		} else {
			if (boost_type < 0 || boost_type >= BOOST_TYPE_MAX)
				goto err;

			if (kstrtoint(field, 10, &boost->boost_type[boost_type]))
				goto err;
		}
	}

	return 0;

err:
	for (i = 0; i < path->boost_cfg_num; i++)
		kfree(boost_cfg[i].gate_enable_info);
	kfree(boost_cfg);
	path->boost_cfg = NULL;
	path->boost_cfg_num = 0;

	return -1;
}

/*
 * "path_condition": pairs of a combination of sources and the property that
 * says what it means.  As on stock, a row whose description fails to parse
 * fails the probe only when it is the last one.
 */
static int mca_path_control_parse_condition(struct mca_path_control *chip)
{
	struct device_node *np = chip->dev->of_node;
	const char *field = NULL;
	int count;
	int ret = 0;
	int i;

	count = mca_parse_dts_count_strings(np, "path_condition",
					    PATH_CONTROL_PATH_MAX,
					    PATH_CONTROL_CONDITION_COLS);
	if (count < 0) {
		mca_log_err("parse path_condition failed\n");
		return -1;
	}

	for (i = 0; i < count; i++) {
		struct path_control_condition_cfg *path =
			&chip->control_path[i / PATH_CONTROL_CONDITION_COLS];

		if (mca_parse_dts_string_index(np, "path_condition", i,
					       &field))
			return -1;

		mca_log_debug("[%d]path condition %s\n", i, field);

		if (i % PATH_CONTROL_CONDITION_COLS == 0) {
			if (kstrtoint(field, 10, &path->condition))
				return -1;
			continue;
		}

		ret = mca_path_control_parse_condition_path(chip, path, field);
	}

	return ret;
}

/* A gate on a pin: the pin is claimed and driven low until it is wanted. */
static int mca_path_control_parse_gpio_scheme(struct mca_path_control *chip,
					      struct path_control_scheme_cfg *cfg,
					      const char *prop)
{
	struct device_node *np = chip->dev->of_node;

	cfg->gpio = of_get_named_gpio(np, prop, 0);
	if (cfg->gpio < 0) {
		mca_log_err("failed to parse gpio_scheme\n");
		return -1;
	}

	if (gpio_request(cfg->gpio, prop)) {
		mca_log_err("request enable-gate-gpio[%d] failed\n", cfg->gpio);
		gpio_free(cfg->gpio);
		return -1;
	}

	if (gpiod_direction_output_raw(gpio_to_desc(cfg->gpio), 0)) {
		mca_log_err("set direction for enable-gate-gpio[%d] failed\n",
			    cfg->gpio);
		gpio_free(cfg->gpio);
		return -1;
	}

	cfg->control_enable_func = mac_path_control_gpio_scheme_func;

	return 0;
}

/*
 * The last column of a "control_scheme" row names the property holding what
 * the scheme needs.  A gate whose row says "null" is left without a function
 * and is never driven.
 */
static int mca_path_control_parse_process_way(struct mca_path_control *chip,
					      struct path_control_scheme_cfg *cfg,
					      const char *prop)
{
	struct device_node *np = chip->dev->of_node;

	if (!strcmp(prop, PATH_CONTROL_NULL_PARA)) {
		mca_log_info("no need parse scheme para\n");
		return 0;
	}

	switch (cfg->scheme) {
	case GPIO_SCHEME:
		return mca_path_control_parse_gpio_scheme(chip, cfg, prop);
	case PMIC_REGISTER_SCHEME:
		mca_parse_dts_u32(np, prop, &cfg->process_way, 0);
		cfg->control_enable_func =
			mac_path_control_pmic_register_scheme_func;
		return 0;
	case CP_CHIP_SCHEME:
		mca_parse_dts_u32(np, prop, &cfg->cp_chip_role, 0);
		cfg->control_enable_func = mac_path_control_cp_chip_scheme_func;
		return 0;
	default:
		return 0;
	}
}

/*
 * "control_scheme": rows of gate role, scheme and scheme parameter.  As on
 * stock, a gate that fails to set up fails the probe only when it is the
 * last one.
 */
static int mca_path_control_parse_role(struct mca_path_control *chip)
{
	struct device_node *np = chip->dev->of_node;
	struct path_control_scheme_cfg *cfg;
	const char *field = NULL;
	int count;
	int ret = 0;
	int i;

	count = mca_parse_dts_count_strings(np, "control_scheme",
					    PATH_CONTROL_ROLE_MAX,
					    PATH_CONTROL_SCHEME_COLS);
	if (count < 0) {
		mca_log_err("parse control_scheme failed\n");
		return -1;
	}

	for (i = 0; i < count; i++) {
		cfg = &chip->control_scheme[i / PATH_CONTROL_SCHEME_COLS];

		if (mca_parse_dts_string_index(np, "control_scheme", i,
					       &field))
			return -1;

		mca_log_debug("[%d]control scheme %s\n", i, field);

		switch (i % PATH_CONTROL_SCHEME_COLS) {
		case GATE_ROLE:
			if (kstrtoint(field, 10, &cfg->gate_role))
				return -1;
			break;
		case CONTROL_SCHEME:
			if (kstrtoint(field, 10, &cfg->scheme))
				return -1;
			break;
		default:
			ret = mca_path_control_parse_process_way(chip, cfg,
								 field);
			break;
		}
	}

	return ret;
}

static int probe_cnt;

static int mca_path_control_probe(struct platform_device *pdev)
{
	struct mca_path_control *chip;
	int ret;

	mca_log_info("probe_cnt = %d\n", ++probe_cnt);

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip) {
		mca_log_err("out of memory\n");
		return -ENOMEM;
	}

	chip->gate_sts = devm_kcalloc(&pdev->dev, PATH_CONTROL_ROLE_MAX,
				      sizeof(*chip->gate_sts), GFP_KERNEL);
	if (!chip->gate_sts)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	platform_set_drvdata(pdev, chip);
	mutex_init(&chip->enable_handling_lock);

	/* both lists are parsed even when the first one fails, as on stock */
	ret = mca_path_control_parse_role(chip);
	ret |= mca_path_control_parse_condition(chip);
	if (ret) {
		mca_log_err("parst dts failed\n");
		return ret;
	}

	INIT_DELAYED_WORK(&chip->update_condition_work,
			  mca_path_control_update_status_work);
	queue_delayed_work(system_wq, &chip->update_condition_work,
			   msecs_to_jiffies(PATH_CONTROL_FIRST_POLL_MS));

	g_info = chip;

	mca_sysfs_init_attrs(path_control_attrs, path_control_sysfs_field_tbl,
			     ARRAY_SIZE(path_control_sysfs_field_tbl));
	ret = mca_sysfs_create_link_group(MCA_SYSFS_DEV_HW_MONITOR,
					  PATH_CONTROL_DIR_NAME, chip->dev,
					  &path_control_sysfs_attr_group);

	mca_log_err("probe %s\n",
		    ret == -EPROBE_DEFER ? "Over probe cnt max" : "OK");

	return ret;
}

static int mca_path_control_remove(struct platform_device *pdev)
{
	struct mca_path_control *chip = platform_get_drvdata(pdev);

	if (!chip)
		return 0;

	cancel_delayed_work_sync(&chip->update_condition_work);
	g_info = NULL;

	return 0;
}

static void mca_path_control_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,path_control" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver mca_path_control_driver = {
	.driver = {
		.name		= "mca_path_control",
		.of_match_table	= match_table,
	},
	.probe		= mca_path_control_probe,
	.remove		= mca_path_control_remove,
	.shutdown	= mca_path_control_shutdown,
};
module_platform_driver(mca_path_control_driver);

MODULE_DESCRIPTION("MCA Path Control");
MODULE_AUTHOR("wuliyang@xiaomi.com");
MODULE_LICENSE("GPL v2");
