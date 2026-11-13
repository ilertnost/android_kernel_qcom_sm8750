// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Watching the cell for over-voltage.
 *
 * A lithium cell taken above its rated voltage is damaged, and a charger that
 * has gone wrong will keep pushing.  Nothing else in the stack is watching for
 * that: the charge pumps report their own faults, but a fault that arrives
 * because the gauge and the charger disagree shows up as neither.  So the
 * voltage is read back independently and compared against what the cell is
 * rated for, which is higher while the battery is being topped off at constant
 * voltage than it is the rest of the time.
 */

#define MCA_LOG_TAG "mca_vbat_ovp_mon"

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_hwid.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/platform/platform_wireless_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include <mca/common/mca_sysfs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

/*
 * How often the voltage is read back.  A cell that is already over its rating
 * is looked at six times as often, because that is when the reading is about
 * to change and userspace is waiting to hear that it has.
 */
#define VBAT_OVP_POLL_MS		60000
#define VBAT_OVP_POLL_TRIGGERED_MS	10000

/*
 * Above this the battery is too hot for the reading to mean anything: the
 * thermal limits will already have cut the charge back, and a cell this warm
 * reads high for reasons that are not over-voltage.  In tenths of a degree.
 */
#define VBAT_OVP_TEMP_MAX_DECIC		479

/* What the thresholds fall back to when the device tree says nothing. */
#define VBAT_OVP_THRESHOLD_FFC_DEFAULT		4580
#define VBAT_OVP_THRESHOLD_NORMAL_DEFAULT	4530
#define VBAT_OVP_THRESHOLD_FFC_GL_DEFAULT	4530
#define VBAT_OVP_THRESHOLD_NORMAL_GL_DEFAULT	4480
#define VBAT_OVP_THRESHOLD_HYS_DEFAULT		15
#define VBAT_OVP_RECHARGE_DELTA_DEFAULT		50

/*
 * A test forces the reading by writing a voltage far above anything a cell
 * reaches; past this the monitor reports over-voltage whatever it measures.
 */
#define VBAT_OVP_FAKE_TRIGGER_MV	4600

/* What userspace can force, for the tests that check the monitor works. */
enum vbat_ovp_mon_attr_list {
	FAKE_VBAT_FOR_DEBUG,
	FAKE_VBAT_OVERRIDE,
	FAKE_VBAT_MON_ATTR_MAX,
};

/**
 * struct mca_vbat_ovp_mon_dev - the cell being watched
 * @dev:                          this device
 * @monitor_vbat_ovp_work:        reads the voltage back
 * @vbat_ovp_thr_ffc_mv:          the threshold in force while topping off
 * @vbat_ovp_thr_nor_mv:          the threshold in force otherwise
 * @vbat_ovp_threshold_ffc:       what a domestic cell is rated for, topping off
 * @vbat_ovp_threshold_normal:    and the rest of the time
 * @vbat_ovp_threshold_ffc_gl:    what a cell sold abroad is rated for
 * @vbat_ovp_threshold_normal_gl: and the rest of the time
 * @vbat_ovp_threshold_hys:       how far above the threshold it must go
 * @vbat_ovp_recharge_delta:      and how far back below before it clears
 * @fake_vbat_for_debug:          a test is forcing the voltage
 * @fake_vbat_override:           what the test set it to
 * @fg_type:                      which gauge the voltage is read from
 * @fake_vbat_ovp:                the verdict a test forced
 * @batt_ovp_status:              the verdict last announced
 * @support_global_fv:            the board has separate thresholds by region
 */
struct mca_vbat_ovp_mon_dev {
	struct device		*dev;
	struct delayed_work	monitor_vbat_ovp_work;
	int			vbat_ovp_thr_ffc_mv;
	int			vbat_ovp_thr_nor_mv;
	int			vbat_ovp_threshold_ffc;
	int			vbat_ovp_threshold_normal;
	int			vbat_ovp_threshold_ffc_gl;
	int			vbat_ovp_threshold_normal_gl;
	int			vbat_ovp_threshold_hys;
	int			vbat_ovp_recharge_delta;
	int			fake_vbat_for_debug;
	int			fake_vbat_override;
	int			fg_type;
	int			fake_vbat_ovp;
	int			batt_ovp_status;
	bool			support_global_fv;
};

/*
 * Whether the cell is over its rating.  This is what the threshold is applied
 * against rather than the value last announced, so that a reading which keeps
 * crossing the line does not announce it every time.
 */
static bool vbat_ovp_status;

/*
 * The threshold is crossed on the way up and cleared lower down, so a cell
 * sitting right at the limit does not report the fault appearing and clearing
 * on alternate readings.
 */
static __always_inline bool
mca_vbat_mon_get_vbat_ovp_status(struct mca_vbat_ovp_mon_dev *chip,
					     int vbat_now_mv)
{
	int threshold;

	threshold = strategy_class_fg_get_fastcharge() ?
		    chip->vbat_ovp_thr_ffc_mv : chip->vbat_ovp_thr_nor_mv;

	if (chip->fake_vbat_override > 0)
		vbat_now_mv = chip->fake_vbat_override;

	mca_log_info("vbat_now_mv = %d, vbat_ovp_triggered: %d\n", vbat_now_mv,
		     vbat_ovp_status);

	if (vbat_ovp_status) {
		if (vbat_now_mv > threshold - chip->vbat_ovp_recharge_delta)
			return true;

		vbat_ovp_status = false;
		mca_log_info("vbat ovp cleared: vbat_ovp_triggered = %d\n",
			     false);

		return false;
	}

	if (vbat_now_mv <= threshold + chip->vbat_ovp_threshold_hys)
		return false;

	vbat_ovp_status = true;
	mca_log_err("vbat ovp triggered: vbat_ovp_triggered = %d\n", true);

	return true;
}

static void mca_vbat_ovp_monitor_workfunc(struct work_struct *work)
{
	struct mca_vbat_ovp_mon_dev *chip =
		container_of(work, struct mca_vbat_ovp_mon_dev,
			     monitor_vbat_ovp_work.work);
	int vbat_now_mv = 0;
	int wireless = 0;
	int online = 0;
	int temp = 0;
	bool triggered = false;
	int delay_ms;

	mca_log_info("fake_vbat_for_debug = %d\n", chip->fake_vbat_for_debug);

	/*
	 * A test drives the monitor by writing a voltage no cell reaches.
	 * That verdict is announced on its own, without reading anything.
	 */
	triggered = chip->fake_vbat_for_debug > VBAT_OVP_FAKE_TRIGGER_MV;
	if (chip->fake_vbat_ovp != triggered) {
		chip->fake_vbat_ovp = triggered;
		mca_log_err("fake_vbat_ovp = %d\n", triggered);
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_VBAT_OVP_CHANGE,
				       &chip->fake_vbat_ovp);
		delay_ms = VBAT_OVP_POLL_MS;
		goto again;
	}
	triggered = false;

	/*
	 * A gauge that watches its own cell voltage reports over-voltage
	 * itself, so there is nothing to do here for one.
	 */
	if (chip->fg_type > 0)
		goto publish;

	strategy_class_fg_ops_get_temperature(&temp);
	if (temp > VBAT_OVP_TEMP_MAX_DECIC)
		goto publish;

	strategy_class_fg_ops_get_voltage(&vbat_now_mv);

	/*
	 * Only what a charger is doing is worth watching: a cell that is
	 * being discharged cannot be driven over its rating.
	 */
	if (platform_class_buckchg_ops_get_online(MAIN_BUCK_CHARGER, &online) |
	    platform_class_wireless_is_present(WIRELESS_ROLE_MASTER, &wireless))
		goto publish;

	if (!online && !wireless)
		goto publish;

	triggered = mca_vbat_mon_get_vbat_ovp_status(chip, vbat_now_mv);

publish:
	if (chip->batt_ovp_status != triggered) {
		chip->batt_ovp_status = triggered;
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_VBAT_OVP_CHANGE,
				       &chip->batt_ovp_status);
	}

	delay_ms = triggered ? VBAT_OVP_POLL_TRIGGERED_MS : VBAT_OVP_POLL_MS;

again:
	queue_delayed_work(system_wq, &chip->monitor_vbat_ovp_work,
			   msecs_to_jiffies(delay_ms));
}

static ssize_t vbat_ovp_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf);
static ssize_t vbat_ovp_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count);

static struct mca_sysfs_attr_info vbat_ovp_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(vbat_ovp_sysfs, 0664, FAKE_VBAT_FOR_DEBUG,
			  fake_vbat_for_debug),
	mca_sysfs_attr_rw(vbat_ovp_sysfs, 0664, FAKE_VBAT_OVERRIDE,
			  fake_vbat_override),
};

static struct attribute *vbat_ovp_attrs[ARRAY_SIZE(vbat_ovp_sysfs_field_tbl) + 1];
static const struct attribute_group vbat_ovp_group = {
	.attrs = vbat_ovp_attrs,
};

static ssize_t vbat_ovp_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct mca_vbat_ovp_mon_dev *chip = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *info;

	info = mca_sysfs_lookup_attr(attr->attr.name,
				     vbat_ovp_sysfs_field_tbl,
				     ARRAY_SIZE(vbat_ovp_sysfs_field_tbl));
	if (!info)
		return -1;

	if (!chip) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	switch (info->sysfs_attr_name) {
	case FAKE_VBAT_FOR_DEBUG:
		return scnprintf(buf, PAGE_SIZE, "%d\n",
				 chip->fake_vbat_for_debug);
	case FAKE_VBAT_OVERRIDE:
		return scnprintf(buf, PAGE_SIZE, "%d\n",
				 chip->fake_vbat_override);
	default:
		return 0;
	}
}

static ssize_t vbat_ovp_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct mca_vbat_ovp_mon_dev *chip = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *info;
	int val = 0;

	info = mca_sysfs_lookup_attr(attr->attr.name,
				     vbat_ovp_sysfs_field_tbl,
				     ARRAY_SIZE(vbat_ovp_sysfs_field_tbl));
	if (!info)
		return -1;

	if (!chip) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	switch (info->sysfs_attr_name) {
	case FAKE_VBAT_FOR_DEBUG:
		chip->fake_vbat_for_debug = val;
		mca_log_err("set the %d fake vbat for debug = %d\n",
			    info->sysfs_attr_name, val);
		break;
	case FAKE_VBAT_OVERRIDE:
		chip->fake_vbat_override = val;
		mca_log_err("set the %d fake vbat override = %d\n",
			    info->sysfs_attr_name, val);
		break;
	default:
		return count;
	}

	/*
	 * The test that wrote this is waiting to see the monitor react, so
	 * the next reading is taken now rather than at the next poll.
	 */
	cancel_delayed_work(&chip->monitor_vbat_ovp_work);
	queue_delayed_work(system_wq, &chip->monitor_vbat_ovp_work, 0);

	return count;
}

static int mca_vbat_ovp_mon_parse_dt(struct mca_vbat_ovp_mon_dev *chip,
				     struct mca_hwid_info *hwid)
{
	struct device_node *np = chip->dev->of_node;
	int *ffc = &chip->vbat_ovp_threshold_ffc;
	int *normal = &chip->vbat_ovp_threshold_normal;
	int rc;

	if (!np) {
		mca_log_err("device tree info missing\n");
		return -1;
	}

	chip->support_global_fv = of_find_property(np, "support_global_fv",
						   NULL);

	rc = mca_parse_dts_u32(np, "vbat_ovp_threshold_ffc",
			       &chip->vbat_ovp_threshold_ffc,
			       VBAT_OVP_THRESHOLD_FFC_DEFAULT);
	rc |= mca_parse_dts_u32(np, "vbat_ovp_threshold_normal",
				&chip->vbat_ovp_threshold_normal,
				VBAT_OVP_THRESHOLD_NORMAL_DEFAULT);
	mca_parse_dts_u32(np, "vbat_ovp_threshold_ffc_gl",
			  &chip->vbat_ovp_threshold_ffc_gl,
			  VBAT_OVP_THRESHOLD_FFC_GL_DEFAULT);
	mca_parse_dts_u32(np, "vbat_ovp_threshold_normal_gl",
			  &chip->vbat_ovp_threshold_normal_gl,
			  VBAT_OVP_THRESHOLD_NORMAL_GL_DEFAULT);
	rc |= mca_parse_dts_u32(np, "vbat_ovp_threshold_hys",
				&chip->vbat_ovp_threshold_hys,
				VBAT_OVP_THRESHOLD_HYS_DEFAULT);
	rc |= mca_parse_dts_u32(np, "vbat_ovp_recharge_delta",
				&chip->vbat_ovp_recharge_delta,
				VBAT_OVP_RECHARGE_DELTA_DEFAULT);
	rc |= mca_parse_dts_u32(np, "fg_type", &chip->fg_type, 0);

	/*
	 * A phone sold abroad may ship with a cell held to a lower voltage,
	 * so which pair of thresholds applies is decided once, here, rather
	 * than every time the voltage is read.
	 */
	if (hwid && chip->support_global_fv && hwid->country_version) {
		ffc = &chip->vbat_ovp_threshold_ffc_gl;
		normal = &chip->vbat_ovp_threshold_normal_gl;
	}

	chip->vbat_ovp_thr_ffc_mv = *ffc;
	chip->vbat_ovp_thr_nor_mv = *normal;

	return rc;
}

static int mca_vbat_ovp_mon_probe(struct platform_device *pdev)
{
	struct mca_vbat_ovp_mon_dev *chip;
	struct mca_hwid_info *hwid;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	platform_set_drvdata(pdev, chip);

	hwid = mca_get_hwid_info();

	if (mca_vbat_ovp_mon_parse_dt(chip, hwid)) {
		mca_log_err("parse dt faile\n");
		return -1;
	}

	/* Nothing is forced until a test writes one of these. */
	chip->fake_vbat_for_debug = -EINVAL;
	chip->fake_vbat_override = -EINVAL;
	chip->fake_vbat_ovp = 0;
	chip->batt_ovp_status = 0;

	INIT_DELAYED_WORK(&chip->monitor_vbat_ovp_work,
			  mca_vbat_ovp_monitor_workfunc);
	queue_delayed_work(system_wq, &chip->monitor_vbat_ovp_work,
			   msecs_to_jiffies(VBAT_OVP_POLL_MS));

	mca_sysfs_init_attrs(vbat_ovp_attrs, vbat_ovp_sysfs_field_tbl,
			     ARRAY_SIZE(vbat_ovp_sysfs_field_tbl));
	mca_sysfs_create_link_group(MCA_SYSFS_DEV_HW_MONITOR, "vbat_ovp",
				    chip->dev, &vbat_ovp_group);

	mca_log_err("%s success\n", __func__);

	return 0;
}

static int mca_vbat_ovp_mon_remove(struct platform_device *pdev)
{
	return 0;
}

static void mca_vbat_ovp_mon_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,vbat_ovp_monitor" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver mca_vbat_ovp_monitor_driver = {
	.driver = {
		.name		= "mca_vbat_ovp_monitor",
		.of_match_table	= match_table,
	},
	.probe		= mca_vbat_ovp_mon_probe,
	.remove		= mca_vbat_ovp_mon_remove,
	.shutdown	= mca_vbat_ovp_mon_shutdown,
};
module_platform_driver(mca_vbat_ovp_monitor_driver);

MODULE_DESCRIPTION("battery ovp software protection");
MODULE_AUTHOR("jiangfei1@xiaomi.com");
MODULE_LICENSE("GPL v2");
