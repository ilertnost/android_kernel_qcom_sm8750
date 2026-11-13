// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Noticing that the battery is not there.
 *
 * A phone whose battery has been unplugged is running from the charger, and
 * charging into nothing is worth refusing rather than attempting.  The
 * connector between the pack and the board is watched for it, in whichever
 * way the board provides: a voltage on a sense pin, a level on a GPIO, or
 * asking the gauge whether it answers.  A phone with two cells has a
 * connector for each and needs both.
 */

#define MCA_LOG_TAG "mca_bmd"

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/consumer.h>
#include <mca/common/mca_charge_mievent.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/platform/platform_wireless_class.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include <mca/common/mca_sysfs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

/* How often the connectors are looked at while a charger is attached. */
#define BMD_POLL_MS			2000

/* How long after probe the first verdict is published. */
#define BMD_REPORT_DELAY_MS		10000

/* How long to wait before asking for a pin that was not free. */
#define BMD_RESOURCE_RETRY_MS		100

/* How many times that is worth trying. */
#define BMD_RESOURCE_RETRY_MAX		3

/*
 * A connector with nothing behind it floats into this band; a connector with
 * a pack behind it is pulled clear of it either way.  In millivolts.
 */
#define BMD_ADC_ABSENT_MIN_MV		1701
#define BMD_ADC_ABSENT_MAX_MV		1900

/* Which connector. */
enum batt_connectot {
	MASTER_BTB,
	SLAVE_BTB,
	MAX_BTB,
};

/* How the board lets us tell. */
enum bmd_scheme {
	ADC_SCHEME,
	GPIO_SCHEME,
	INT_SCHEME,
	IIC_SCHEME,
	MAX_SCHEME,
};

/* What userspace can look at. */
enum btb_attr_list {
	BTB_PROP_MASTER,
	BTB_PROP_SLAVE,
	BTB_PROP_MISSING,
};

/**
 * struct bmd_scheme_data - how one connector is watched
 * @scheme:     which of the ways
 * @chan:       the sense channel, when it is measured
 * @gpio:       the pin, when it is a level
 * @cfg_failed: the pin or channel could not be had, so nothing can be read
 */
struct bmd_scheme_data {
	int			scheme;
	struct iio_channel	*chan;
	int			gpio;
	bool			cfg_failed;
};

/**
 * struct mca_bmd_dev - the connectors being watched
 * @dev:                       this device
 * @bmd_scheme:                how each connector is watched
 * @monitor_bmd_work:          looks at them
 * @request_hw_resource_work:  asks again for a pin that was not free
 * @delay_report_bmd_sts_work: publishes the first verdict once the rest of
 *                             the stack has had time to come up
 * @btb_online:                whether each connector has a pack behind it
 * @batt_btb_status:           whether the phone is running without a battery
 * @fake_batt:                 whether it is running on no cell at all
 * @fg_type:                   how many cells this phone has
 */
struct mca_bmd_dev {
	struct device		*dev;
	struct bmd_scheme_data	bmd_scheme[MAX_BTB];
	struct delayed_work	monitor_bmd_work;
	struct delayed_work	request_hw_resource_work;
	struct delayed_work	delay_report_bmd_sts_work;
	bool			btb_online[MAX_BTB];
	bool			batt_btb_status;
	int			fake_batt;
	int			fg_type;
};

static int bmd_retry_cnt;

/*
 * Whether one connector has a pack behind it.  A connector that could not be
 * set up reads as absent rather than as present, so a board that was wired
 * for a scheme it does not have does not report a battery that is not there.
 */
static noinline bool mca_bmd_get_btb_status(struct mca_bmd_dev *chip, int btb)
{
	struct bmd_scheme_data *s;
	int val = 0;

	if (!chip) {
		mca_log_err("null pointer\n");

		return true;
	}

	s = &chip->bmd_scheme[btb];
	if (s->cfg_failed) {
		mca_log_info("cfg failed\n");

		return false;
	}

	switch (s->scheme) {
	case ADC_SCHEME:
		if (iio_read_channel_processed(s->chan, &val) < 0) {
			mca_log_err("Error in reading btb_adc_voltage channel\n");

			return false;
		}

		return !(val >= BMD_ADC_ABSENT_MIN_MV &&
			 val < BMD_ADC_ABSENT_MAX_MV);
	case GPIO_SCHEME:
		return gpiod_get_raw_value(gpio_to_desc(s->gpio)) == 0;
	case IIC_SCHEME:
		/*
		 * The gauge answering at all is the evidence: a pack that is
		 * not there cannot reply.
		 */
		if (chip->fg_type == 1)
			return strategy_class_fg_dual_is_chip_ok(btb) > 0;

		return strategy_class_fg_is_chip_ok() > 0;
	default:
		return false;
	}
}

static void mca_bmd_monitor_workfunc(struct work_struct *work)
{
	struct mca_bmd_dev *chip = container_of(work, struct mca_bmd_dev,
						monitor_bmd_work.work);
	bool missing = true;
	bool fake;
	int val;

	chip->btb_online[MASTER_BTB] = mca_bmd_get_btb_status(chip, MASTER_BTB);
	chip->btb_online[SLAVE_BTB] = mca_bmd_get_btb_status(chip, SLAVE_BTB);
	mca_log_info("btb_online[0]: %d, btb_online[1]: %d\n",
		     chip->btb_online[MASTER_BTB], chip->btb_online[SLAVE_BTB]);

	if (chip->btb_online[MASTER_BTB])
		missing = !chip->btb_online[SLAVE_BTB];

	if (chip->batt_btb_status != missing) {
		chip->batt_btb_status = missing;
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_BATT_BTB_CHANGE,
				       &chip->batt_btb_status);

		/*
		 * A connector that has come apart is worth recording with the
		 * state of the other one, because on a two cell phone which
		 * of them went is what explains the failure.
		 */
		if (missing) {
			if (chip->fg_type == 1) {
				val = chip->btb_online[MASTER_BTB];
				mca_charge_mievent_report(CHARGE_DFX_DUAL_BATTERY_MISSING,
							  &val, 1);
			} else {
				mca_charge_mievent_report(CHARGE_DFX_BATTERY_MISSING,
							  NULL, 0);
			}
		} else {
			mca_charge_mievent_set_state(MIEVENT_STATE_END,
						     chip->fg_type == 1 ?
						     CHARGE_DFX_DUAL_BATTERY_MISSING :
						     CHARGE_DFX_BATTERY_MISSING);
		}
	}

	/*
	 * Running on no cell at all is a different thing from one connector
	 * having come apart: the phone is being held up by the charger, and
	 * what reads the battery has to be told to stop believing itself.
	 */
	if (chip->fg_type == 1)
		fake = !(chip->btb_online[MASTER_BTB] &&
			 chip->btb_online[SLAVE_BTB]);
	else
		fake = !(chip->btb_online[MASTER_BTB] ||
			 chip->btb_online[SLAVE_BTB]);

	if (chip->fake_batt != fake) {
		chip->fake_batt = fake;
		mca_log_err("fake_batt = %d\n", fake);
		mca_event_block_notify(MCA_EVENT_TYPE_BATTERY_INFO,
				       MCA_EVENT_BATTERY_FAKE_POWER,
				       &chip->fake_batt);
	}

	queue_delayed_work(system_wq, &chip->monitor_bmd_work,
			   msecs_to_jiffies(BMD_POLL_MS));
}

/*
 * The first verdict, published once rather than on a change, so that whatever
 * came up after this driver still learns what the connectors look like.
 */
static void mca_bmd_delay_report_bmd_sts_work(struct work_struct *work)
{
	struct mca_bmd_dev *chip = container_of(work, struct mca_bmd_dev,
						delay_report_bmd_sts_work.work);
	bool missing = true;
	int fake;

	chip->btb_online[MASTER_BTB] = mca_bmd_get_btb_status(chip, MASTER_BTB);
	chip->btb_online[SLAVE_BTB] = mca_bmd_get_btb_status(chip, SLAVE_BTB);
	mca_log_err("init notify:btb_online[0]: %d, btb_online[1]: %d\n",
		    chip->btb_online[MASTER_BTB], chip->btb_online[SLAVE_BTB]);

	if (chip->btb_online[MASTER_BTB])
		missing = !chip->btb_online[SLAVE_BTB];

	mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
			       MCA_EVENT_BATT_BTB_CHANGE, &missing);

	if (chip->fg_type == 1) {
		fake = !(chip->btb_online[MASTER_BTB] &&
			 chip->btb_online[SLAVE_BTB]);
		mca_event_block_notify(MCA_EVENT_TYPE_BATTERY_INFO,
				       MCA_EVENT_BATTERY_FAKE_POWER, &fake);
	}
}

/*
 * Every connector watched by a level reads the first "btb_gpio" entry.  A pin
 * that cannot be set as an input is still used: only failing to get the pin
 * at all marks the connector as unconfigured.
 */
static int mca_bmd_request_gpio(struct mca_bmd_dev *chip, int btb)
{
	struct bmd_scheme_data *s = &chip->bmd_scheme[btb];

	s->gpio = of_get_named_gpio(chip->dev->of_node, "btb_gpio", 0);
	if (s->gpio < 0) {
		mca_log_err("failed to gpio is invalid %d\n", s->gpio);
		s->cfg_failed = true;

		return -1;
	}

	if (gpio_request(s->gpio, "btb_gpio")) {
		s->cfg_failed = true;
		mca_log_err("unable to request btb_gpio gpio [%d]\n", s->gpio);

		return -1;
	}

	s->cfg_failed = false;
	if (gpiod_direction_input(gpio_to_desc(s->gpio)))
		mca_log_err("unable to set direction btb_gpio [%d]\n", s->gpio);

	return 0;
}

/*
 * A pin that was not free when this driver probed may be freed later, by a
 * driver that took it and then failed.  It is asked for again a few times
 * before the connector is given up on.
 */
static void mca_bmd_request_hw_resource_work(struct work_struct *work)
{
	struct mca_bmd_dev *chip = container_of(work, struct mca_bmd_dev,
						request_hw_resource_work.work);
	bool pending = false;
	int i;

	for (i = 0; i < MAX_BTB; i++) {
		if (!chip->bmd_scheme[i].cfg_failed ||
		    chip->bmd_scheme[i].scheme != GPIO_SCHEME)
			continue;

		if (mca_bmd_request_gpio(chip, i))
			pending = true;
	}

	if (pending && bmd_retry_cnt < BMD_RESOURCE_RETRY_MAX) {
		bmd_retry_cnt++;
		queue_delayed_work(system_wq, &chip->request_hw_resource_work,
				   msecs_to_jiffies(BMD_RESOURCE_RETRY_MS));
	}

	mca_log_err("retry bmd resource request %d\n", bmd_retry_cnt);
}

/*
 * The connectors are only worth looking at while something is charging: a
 * phone running from its battery cannot be running without one.
 */
static int mca_bmd_process_event(int event, int value, void *info)
{
	struct mca_bmd_dev *chip = info;
	struct delayed_work *dwork;

	if (!chip) {
		mca_log_err("%s: info is null", __func__);

		return -1;
	}

	switch (event) {
	case MCA_EVENT_USB_DISCONNECT:
	case MCA_EVENT_WIRELESS_DISCONNECT:
		cancel_delayed_work_sync(&chip->monitor_bmd_work);

		return 0;
	case MCA_EVENT_USB_CONNECT:
	case MCA_EVENT_WIRELESS_CONNECT:
		dwork = &chip->monitor_bmd_work;
		break;
	case MCA_EVENT_BMD_STSTUS_CHANGE:
		dwork = &chip->delay_report_bmd_sts_work;
		break;
	default:
		return 0;
	}

	cancel_delayed_work_sync(dwork);
	queue_delayed_work(system_wq, dwork, 0);

	return 0;
}

static int mca_bmd_get_status(int func, int *status, void *info)
{
	struct mca_bmd_dev *chip = info;

	if (func != STRATEGY_STATUS_TYPE_BMD || !status || !chip)
		return -1;

	*status = chip->batt_btb_status;

	return 0;
}

static ssize_t btb_sysfs_show(struct device *dev,
			      struct device_attribute *attr, char *buf);

static struct mca_sysfs_attr_info btb_sysfs_field_tbl[] = {
	mca_sysfs_attr_ro(btb_sysfs, 0440, BTB_PROP_MASTER, btb_master_status),
	mca_sysfs_attr_ro(btb_sysfs, 0440, BTB_PROP_SLAVE, btb_slave_status),
	mca_sysfs_attr_ro(btb_sysfs, 0440, BTB_PROP_MISSING,
			  btb_missing_status),
};

static struct attribute *btb_attrs[ARRAY_SIZE(btb_sysfs_field_tbl) + 1];
static const struct attribute_group btb_sysfs_attr_group = {
	.attrs = btb_attrs,
};

static ssize_t btb_sysfs_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct mca_bmd_dev *chip = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *info;
	int val;

	info = mca_sysfs_lookup_attr(attr->attr.name, btb_sysfs_field_tbl,
				     ARRAY_SIZE(btb_sysfs_field_tbl));
	if (!info)
		return -1;

	switch (info->sysfs_attr_name) {
	case BTB_PROP_MASTER:
		val = mca_bmd_get_btb_status(chip, MASTER_BTB);
		break;
	case BTB_PROP_SLAVE:
		val = mca_bmd_get_btb_status(chip, SLAVE_BTB);
		break;
	case BTB_PROP_MISSING:
		val = chip ? chip->batt_btb_status : 0;
		break;
	default:
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

/* The subdirectory the connectors appear under. */
#define BMD_DIR_NAME	"bmd"

static int mca_bmd_parse_dt(struct mca_bmd_dev *chip)
{
	struct device_node *np = chip->dev->of_node;
	u32 scheme[MAX_BTB] = { 0 };
	int i;

	if (!np) {
		mca_log_err("device tree info missing\n");

		return -1;
	}

	if (mca_parse_dts_u32_array(np, "btb_bmd_scheme", scheme, MAX_BTB)
	    < 0) {
		mca_log_err("parse btb_bmd_scheme failed\n");

		return -1;
	}

	for (i = 0; i < MAX_BTB; i++) {
		chip->bmd_scheme[i].scheme = scheme[i];

		switch (scheme[i]) {
		case ADC_SCHEME:
			chip->bmd_scheme[i].chan =
				devm_iio_channel_get(chip->dev, "btb_adc");
			/* a channel that is not there cannot be read */
			chip->bmd_scheme[i].cfg_failed =
				IS_ERR(chip->bmd_scheme[i].chan);
			break;
		case GPIO_SCHEME:
			mca_bmd_request_gpio(chip, i);
			break;
		case INT_SCHEME:
		case IIC_SCHEME:
			chip->bmd_scheme[i].cfg_failed = false;
			break;
		default:
			break;
		}
	}

	mca_parse_dts_u32(np, "fg_type", &chip->fg_type, 0);

	return 0;
}

static int mca_bmd_probe(struct platform_device *pdev)
{
	struct mca_bmd_dev *chip;
	int wireless = 0;
	int online = 0;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	platform_set_drvdata(pdev, chip);

	INIT_DELAYED_WORK(&chip->request_hw_resource_work,
			  mca_bmd_request_hw_resource_work);
	INIT_DELAYED_WORK(&chip->delay_report_bmd_sts_work,
			  mca_bmd_delay_report_bmd_sts_work);

	if (mca_bmd_parse_dt(chip)) {
		mca_log_err("parse dt faile\n");

		return -1;
	}

	if ((chip->bmd_scheme[MASTER_BTB].scheme == GPIO_SCHEME &&
	     chip->bmd_scheme[MASTER_BTB].cfg_failed) ||
	    (chip->bmd_scheme[SLAVE_BTB].scheme == GPIO_SCHEME &&
	     chip->bmd_scheme[SLAVE_BTB].cfg_failed))
		queue_delayed_work(system_wq, &chip->request_hw_resource_work,
				   msecs_to_jiffies(BMD_RESOURCE_RETRY_MS));

	mca_strategy_ops_register(STRATEGY_FUNC_TYPE_BMD, mca_bmd_process_event,
				  mca_bmd_get_status, NULL, chip);

	INIT_DELAYED_WORK(&chip->monitor_bmd_work, mca_bmd_monitor_workfunc);
	chip->batt_btb_status = false;

	mca_sysfs_init_attrs(btb_attrs, btb_sysfs_field_tbl,
			     ARRAY_SIZE(btb_sysfs_field_tbl));
	mca_sysfs_create_link_group(MCA_SYSFS_DEV_HW_MONITOR, BMD_DIR_NAME,
				    chip->dev, &btb_sysfs_attr_group);

	queue_delayed_work(system_wq, &chip->delay_report_bmd_sts_work,
			   msecs_to_jiffies(BMD_REPORT_DELAY_MS));

	/* A charger already attached means the monitor should be running. */
	platform_class_buckchg_ops_get_online(MAIN_BUCK_CHARGER, &online);
	platform_class_wireless_is_present(WIRELESS_ROLE_MASTER, &wireless);
	if (online || wireless)
		queue_delayed_work(system_wq, &chip->monitor_bmd_work,
				   msecs_to_jiffies(BMD_POLL_MS));

	mca_log_err("%s success\n", __func__);

	return 0;
}

static int mca_bmd_remove(struct platform_device *pdev)
{
	struct mca_bmd_dev *chip = platform_get_drvdata(pdev);

	if (!chip)
		return 0;

	cancel_delayed_work_sync(&chip->monitor_bmd_work);
	cancel_delayed_work_sync(&chip->delay_report_bmd_sts_work);
	cancel_delayed_work_sync(&chip->request_hw_resource_work);

	return 0;
}

static void mca_bmd_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,bmd" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver mca_bmd_driver = {
	.driver = {
		.name		= "mca_bmd",
		.of_match_table	= match_table,
	},
	.probe		= mca_bmd_probe,
	.remove		= mca_bmd_remove,
	.shutdown	= mca_bmd_shutdown,
};
module_platform_driver(mca_bmd_driver);

MODULE_DESCRIPTION("battery missing detection");
MODULE_AUTHOR("lvxiaofeng@xiaomi.com");
MODULE_LICENSE("GPL v2");
