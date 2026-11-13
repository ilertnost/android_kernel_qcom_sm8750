// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * HL7603 buck-boost converter in bypass mode, as used on Xiaomi phones. The
 * converter boosts the system rail whenever the battery drops below the
 * configured threshold; this driver only programs that threshold from the
 * device tree.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>

#define HL7603_REG_VOUT			0x02

#define HL7603_VOUT_MIN_MV		2850
#define HL7603_VOUT_MAX_MV		5500
#define HL7603_VOUT_STEP_MV		50
#define HL7603_VOUT_DEFAULT_MV		3400

static int hl7603_set_vout_threshold(struct i2c_client *client, u32 mv)
{
	int ret;

	if (mv < HL7603_VOUT_MIN_MV || mv > HL7603_VOUT_MAX_MV) {
		dev_err(&client->dev, "vout_threshold %u mV out of range\n", mv);
		return -EINVAL;
	}

	ret = i2c_smbus_write_byte_data(client, HL7603_REG_VOUT,
			(mv - HL7603_VOUT_MIN_MV) / HL7603_VOUT_STEP_MV);
	if (ret < 0) {
		dev_err(&client->dev, "failed to set vout threshold: %d\n", ret);
		return ret;
	}

	dev_dbg(&client->dev, "VOUT register 0x%02x\n",
		i2c_smbus_read_byte_data(client, HL7603_REG_VOUT));

	return 0;
}

static int hl7603_probe(struct i2c_client *client)
{
	u32 vout_mv = HL7603_VOUT_DEFAULT_MV;

	of_property_read_u32(client->dev.of_node, "vout_threshold", &vout_mv);

	return hl7603_set_vout_threshold(client, vout_mv);
}

static const struct of_device_id hl7603_of_match[] = {
	{ .compatible = "hl7603" },
	{ }
};
MODULE_DEVICE_TABLE(of, hl7603_of_match);

static struct i2c_driver hl7603_driver = {
	.driver = {
		.name = "hl7603_boost_bypass",
		.of_match_table = hl7603_of_match,
	},
	.probe = hl7603_probe,
};
module_i2c_driver(hl7603_driver);

MODULE_DESCRIPTION("hl7603 boot_bypass driver");
MODULE_LICENSE("GPL v2");
