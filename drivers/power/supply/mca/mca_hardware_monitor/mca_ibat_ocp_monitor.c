// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Watching the cells for over-current.
 *
 * A cell drawn harder than it is rated for heats and ages, and on a phone with
 * two cells the pair can be within their limit together while one of them is
 * over it.  So each gauge is read separately against its own threshold, and
 * which of them went over is what is reported: a cell that later fails is
 * explained by what it was put through.
 */

#define MCA_LOG_TAG "mca_ibat_ocp_mon"

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <mca/common/mca_charge_mievent.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/platform/platform_fg_ic_ops.h>
#include <mca/platform/platform_loadsw_class.h>
#include <mca/platform/platform_wireless_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include <mca/common/mca_sysfs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

/*
 * How often the currents are read back.  Once one of them is over its
 * threshold the reading is taken more than twice as often, because that is
 * when it is about to change and userspace is waiting to hear that it has.
 */
#define IBAT_OCP_POLL_MS		5000
#define IBAT_OCP_POLL_TRIGGERED_MS	2000

/* What the thresholds fall back to when the device tree says nothing, in uA. */
#define IBAT_OCP_THRESHOLD_MASTER_DEFAULT	9280000
#define IBAT_OCP_THRESHOLD_SLAVE_DEFAULT	3540000

/*
 * A test forces the reading by writing a current no cell draws; past this the
 * monitor reports over-current whatever it measures.
 */
#define IBAT_OCP_FAKE_TRIGGER_UA	13000000

/*
 * Which cell went over, as a pair of bits, so that a single value says both
 * whether anything is wrong and which gauge saw it.
 */
#define IBAT_OCP_FLAG_MASTER		BIT(1)
#define IBAT_OCP_FLAG_SLAVE		BIT(0)

/*
 * Only one gauge arrangement reports a current this can be compared against;
 * on the others the pack reports its own over-current and there is nothing
 * for this to add.
 */
#define IBAT_OCP_FG_TYPE_SUPPORTED	1

/* What userspace can force, for the tests that check the monitor works. */
enum ibat_ocp_mon_attr_list {
	FAKE_IBAT_FOR_DEBUG,
	FAKE_MASTER_IBAT_OVERRIDE,
	FAKE_SLAVE_IBAT_OVERRIDE,
	FAKE_IBAT_MON_ATTR_MAX,
};

/**
 * struct mca_ibat_ocp_mon_dev - the cells being watched
 * @dev:                          this device
 * @monitor_ibat_ocp_work:        reads the currents back
 * @fake_ibat_for_debug:          a test is forcing the verdict
 * @fake_master_ibat_override:    what the test set the first cell's current to
 * @fake_slave_ibat_override:     and the second
 * @fg_type:                      which gauge arrangement this board has
 * @fake_ibat_ocp:                the verdict a test forced
 * @ibat_ocp_status:              the verdict last announced
 * @ocp_threshold:                the threshold for each cell, in microamps
 */
struct mca_ibat_ocp_mon_dev {
	struct device		*dev;
	struct delayed_work	monitor_ibat_ocp_work;
	int			fake_ibat_for_debug;
	int			fake_master_ibat_override;
	int			fake_slave_ibat_override;
	int			fg_type;
	int			fake_ibat_ocp;
	int			ibat_ocp_status;
	int			ocp_threshold[FG_IC_MAX];
};

/*
 * Whether a cell is over its rating.  This is what the thresholds are applied
 * against rather than the value last announced, so a current that keeps
 * crossing the line does not announce it every time.
 */
static bool ibat_ocp_status;

static __always_inline int
mca_ibat_mon_get_ibat_ocp_status(struct mca_ibat_ocp_mon_dev *chip)
{
	int master = 0;
	int slave = 0;
	int flag;

	platform_fg_ops_get_curr(FG_IC_MASTER, &master);
	platform_fg_ops_get_curr(FG_IC_SLAVE, &slave);

	if (chip->fake_master_ibat_override > 0)
		master = chip->fake_master_ibat_override;
	if (chip->fake_slave_ibat_override > 0)
		slave = chip->fake_slave_ibat_override;

	/*
	 * The gauge reports discharge as a negative current; what matters
	 * here is how hard the cell is being worked, either way.
	 */
	flag = 0;
	if (abs(master) > chip->ocp_threshold[FG_IC_MASTER])
		flag |= IBAT_OCP_FLAG_MASTER;
	if (abs(slave) > chip->ocp_threshold[FG_IC_SLAVE])
		flag |= IBAT_OCP_FLAG_SLAVE;

	mca_log_info("ocp_flag = %d, ibat_ocp_triggered: %d\n", flag,
		     ibat_ocp_status);

	if (flag && !ibat_ocp_status) {
		ibat_ocp_status = true;
		mca_log_err("ibat ocp triggered: ibat_ocp_triggered = %d\n",
			    true);

		return flag;
	}

	if (!flag && ibat_ocp_status) {
		ibat_ocp_status = false;
		mca_log_info("ibat ocp cleared: ibat_ocp_triggered = %d\n",
			     false);

		return 0;
	}

	return ibat_ocp_status ? flag : 0;
}

static void mca_ibat_ocp_monitor_workfunc(struct work_struct *work)
{
	struct mca_ibat_ocp_mon_dev *chip =
		container_of(work, struct mca_ibat_ocp_mon_dev,
			     monitor_ibat_ocp_work.work);
	bool loadsw_present = true;
	int wireless = 0;
	int online = 0;
	int flag = 0;
	int delay_ms;

	mca_log_info("fake_ibat_for_debug = %d\n", chip->fake_ibat_for_debug);

	/*
	 * A test drives the monitor by writing a current no cell draws.  That
	 * verdict is announced on its own, without reading anything.
	 */
	flag = chip->fake_ibat_for_debug > IBAT_OCP_FAKE_TRIGGER_UA;
	if (chip->fake_ibat_ocp != flag) {
		chip->fake_ibat_ocp = flag;
		mca_log_err("fake_ibat_ocp = %d\n", flag);
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_IBAT_OCP_CHANGE,
				       &chip->fake_ibat_ocp);
		delay_ms = IBAT_OCP_POLL_MS;
		goto again;
	}

	flag = 0;

	if (chip->fg_type != IBAT_OCP_FG_TYPE_SUPPORTED)
		goto publish;

	/*
	 * Only what a charger is doing is worth watching: the current a cell
	 * gives up on its own is the phone's to draw.
	 */
	if (platform_class_buckchg_ops_get_online(MAIN_BUCK_CHARGER, &online) |
	    platform_class_wireless_is_present(WIRELESS_ROLE_MASTER, &wireless))
		goto publish;

	if (!online && !wireless)
		goto publish;

	/*
	 * The load switch is what shares the charge between the two cells.
	 * If it cannot be reached the currents about to be read mean nothing,
	 * and the reason they mean nothing is worth recording.
	 */
	platform_class_loadsw_get_present(LOADSW_ROLE_MASTER, &loadsw_present);
	if (!loadsw_present)
		mca_charge_mievent_report(CHARGE_DFX_LOAD_SWITCH_I2C_ERR, NULL,
					  0);

	/* Below fast charging the currents are nowhere near the threshold. */
	if (!strategy_class_fg_get_fastcharge())
		goto publish;

	flag = mca_ibat_mon_get_ibat_ocp_status(chip);

publish:
	if (chip->ibat_ocp_status != flag) {
		chip->ibat_ocp_status = flag;
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_IBAT_OCP_CHANGE,
				       &chip->ibat_ocp_status);
	}

	delay_ms = flag ? IBAT_OCP_POLL_TRIGGERED_MS : IBAT_OCP_POLL_MS;

again:
	queue_delayed_work(system_wq, &chip->monitor_ibat_ocp_work,
			   msecs_to_jiffies(delay_ms));
}

static ssize_t ibat_ocp_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf);
static ssize_t ibat_ocp_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count);

static struct mca_sysfs_attr_info ibat_ocp_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(ibat_ocp_sysfs, 0664, FAKE_IBAT_FOR_DEBUG,
			  fake_ibat_for_debug),
	mca_sysfs_attr_rw(ibat_ocp_sysfs, 0664, FAKE_MASTER_IBAT_OVERRIDE,
			  fake_master_ibat_override),
	mca_sysfs_attr_rw(ibat_ocp_sysfs, 0664, FAKE_SLAVE_IBAT_OVERRIDE,
			  fake_slave_ibat_override),
};

static struct attribute *ibat_ocp_attrs[ARRAY_SIZE(ibat_ocp_sysfs_field_tbl) + 1];
static const struct attribute_group ibat_ocp_group = {
	.attrs = ibat_ocp_attrs,
};

static ssize_t ibat_ocp_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct mca_ibat_ocp_mon_dev *chip = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *info;

	info = mca_sysfs_lookup_attr(attr->attr.name,
				     ibat_ocp_sysfs_field_tbl,
				     ARRAY_SIZE(ibat_ocp_sysfs_field_tbl));
	if (!info)
		return -1;

	if (!chip) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	switch (info->sysfs_attr_name) {
	case FAKE_IBAT_FOR_DEBUG:
		return scnprintf(buf, PAGE_SIZE, "%d\n",
				 chip->fake_ibat_for_debug);
	case FAKE_MASTER_IBAT_OVERRIDE:
		return scnprintf(buf, PAGE_SIZE, "%d\n",
				 chip->fake_master_ibat_override);
	case FAKE_SLAVE_IBAT_OVERRIDE:
		return scnprintf(buf, PAGE_SIZE, "%d\n",
				 chip->fake_slave_ibat_override);
	default:
		return 0;
	}
}

static ssize_t ibat_ocp_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct mca_ibat_ocp_mon_dev *chip = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *info;
	int val = 0;

	info = mca_sysfs_lookup_attr(attr->attr.name,
				     ibat_ocp_sysfs_field_tbl,
				     ARRAY_SIZE(ibat_ocp_sysfs_field_tbl));
	if (!info)
		return -1;

	if (!chip) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	switch (info->sysfs_attr_name) {
	case FAKE_IBAT_FOR_DEBUG:
		chip->fake_ibat_for_debug = val;
		mca_log_err("set the %d fake vbat for debug = %d\n",
			    info->sysfs_attr_name, val);
		break;
	case FAKE_MASTER_IBAT_OVERRIDE:
		chip->fake_master_ibat_override = val;
		mca_log_err("set the %d fake vbat override = %d\n",
			    info->sysfs_attr_name, val);
		break;
	case FAKE_SLAVE_IBAT_OVERRIDE:
		chip->fake_slave_ibat_override = val;
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
	cancel_delayed_work(&chip->monitor_ibat_ocp_work);
	queue_delayed_work(system_wq, &chip->monitor_ibat_ocp_work, 0);

	return count;
}

static int mca_ibat_ocp_mon_parse_dt(struct mca_ibat_ocp_mon_dev *chip)
{
	struct device_node *np = chip->dev->of_node;
	int rc;

	if (!np) {
		mca_log_err("device tree info missing\n");
		return -1;
	}

	rc = mca_parse_dts_u32(np, "fg_type", &chip->fg_type, 0);
	if (rc)
		mca_log_err("parse fg_type failed\n");

	if (rc | mca_parse_dts_u32_array(np, "ocp_threshold",
					 chip->ocp_threshold, FG_IC_MAX)) {
		chip->ocp_threshold[FG_IC_MASTER] =
			IBAT_OCP_THRESHOLD_MASTER_DEFAULT;
		chip->ocp_threshold[FG_IC_SLAVE] =
			IBAT_OCP_THRESHOLD_SLAVE_DEFAULT;
		mca_log_err("parse ocp_threshold failed, use default value\n");
		mca_log_info("ocp_threshold : %d %d\n",
			     chip->ocp_threshold[FG_IC_MASTER],
			     chip->ocp_threshold[FG_IC_SLAVE]);

		return -1;
	}

	mca_log_info("ocp_threshold : %d %d\n",
		     chip->ocp_threshold[FG_IC_MASTER],
		     chip->ocp_threshold[FG_IC_SLAVE]);

	return 0;
}

static int mca_ibat_ocp_mon_probe(struct platform_device *pdev)
{
	struct mca_ibat_ocp_mon_dev *chip;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	platform_set_drvdata(pdev, chip);

	if (mca_ibat_ocp_mon_parse_dt(chip)) {
		mca_log_err("parse dt faile\n");
		return -1;
	}

	/* Nothing is forced until a test writes one of these. */
	chip->fake_ibat_for_debug = -EINVAL;
	chip->fake_master_ibat_override = -EINVAL;
	chip->fake_slave_ibat_override = -EINVAL;
	chip->fake_ibat_ocp = 0;
	chip->ibat_ocp_status = 0;

	INIT_DELAYED_WORK(&chip->monitor_ibat_ocp_work,
			  mca_ibat_ocp_monitor_workfunc);
	queue_delayed_work(system_wq, &chip->monitor_ibat_ocp_work,
			   msecs_to_jiffies(IBAT_OCP_POLL_MS));

	mca_sysfs_init_attrs(ibat_ocp_attrs, ibat_ocp_sysfs_field_tbl,
			     ARRAY_SIZE(ibat_ocp_sysfs_field_tbl));
	mca_sysfs_create_link_group(MCA_SYSFS_DEV_HW_MONITOR, "ibat_ocp",
				    chip->dev, &ibat_ocp_group);

	mca_log_err("%s success\n", __func__);

	return 0;
}

static int mca_ibat_ocp_mon_remove(struct platform_device *pdev)
{
	return 0;
}

static void mca_ibat_ocp_mon_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,ibat_ocp_monitor" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver mca_ibat_ocp_monitor_driver = {
	.driver = {
		.name		= "mca_ibat_ocp_monitor",
		.of_match_table	= match_table,
	},
	.probe		= mca_ibat_ocp_mon_probe,
	.remove		= mca_ibat_ocp_mon_remove,
	.shutdown	= mca_ibat_ocp_mon_shutdown,
};
module_platform_driver(mca_ibat_ocp_monitor_driver);

MODULE_DESCRIPTION("battery ocp software protection");
MODULE_AUTHOR("yinshunan@xiaomi.com");
MODULE_LICENSE("GPL v2");
