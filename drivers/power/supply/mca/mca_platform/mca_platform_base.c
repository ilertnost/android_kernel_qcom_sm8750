// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The charging chips on the bus.
 *
 * Several of the charging chips answer at more than one I2C address -- a
 * charge pump with a master and a slave half, a fuel gauge with a second
 * page -- and the drivers for them want a client for each.  This creates one
 * dummy client per address listed on the node and then populates the child
 * nodes, so each driver finds its own.
 *
 * It also puts a regmap over the whole set.  Its register address is two
 * bytes: which of the listed addresses to talk to, then the register on that
 * chip.  A driver that wants to reach a sibling chip can therefore do so
 * through one map rather than by holding a client of its own.
 */

#define MCA_LOG_TAG "mca_platform_base"

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <mca/common/mca_log.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/slab.h>

/* An I2C device has a seven bit address, so this is more than enough. */
#define MCA_PLATFORM_MAX_SLAVES		8

/**
 * struct mca_platform_base - the chips behind one node
 * @dev:         this device
 * @slaves:      a client for each address
 * @regmap:      the map that reaches all of them
 * @slave_addrs: the addresses themselves, as the node listed them
 * @slave_num:   how many there are
 */
struct mca_platform_base {
	struct device		*dev;
	struct i2c_client	**slaves;
	struct regmap		*regmap;
	u8			*slave_addrs;
	int			slave_num;
};

/*
 * The first byte of a register address picks the chip and the second is the
 * register on it, so a bulk transfer is turned into a block transfer to one
 * of the clients.
 */
static int mca_platform_base_regmap_read(void *context, const void *reg,
					 size_t reg_size, void *val,
					 size_t val_size)
{
	struct mca_platform_base *base = context;
	const u8 *addr = reg;
	struct i2c_client *client;
	int rc;

	if (base->slave_num <= addr[0])
		return -EINVAL;

	client = base->slaves[addr[0]];
	if (!client)
		return -EINVAL;

	rc = i2c_smbus_read_i2c_block_data(client, addr[1], val_size, val);
	if (rc < 0)
		return rc;

	return rc == val_size ? 0 : -EIO;
}

static int mca_platform_base_regmap_write(void *context, const void *data,
					  size_t count)
{
	struct mca_platform_base *base = context;
	const u8 *addr = data;
	struct i2c_client *client;

	if (base->slave_num <= addr[0])
		return -EINVAL;

	client = base->slaves[addr[0]];
	if (!client)
		return -EINVAL;

	return i2c_smbus_write_i2c_block_data(client, addr[1], count - 2,
					      addr + 2);
}

static const struct regmap_bus mca_platform_base_regmap_bus = {
	.write	= mca_platform_base_regmap_write,
	.read	= mca_platform_base_regmap_read,
};

static const struct regmap_config mca_platform_base_regmap_config = {
	.reg_bits		= 16,
	.val_bits		= 8,
	.reg_format_endian	= REGMAP_ENDIAN_BIG,
};

static int mca_platform_base_parse_dt(struct mca_platform_base *base)
{
	struct device_node *np = base->dev->of_node;
	int count = 0;
	int rc;

	if (!of_find_property(np, "platform_slave_addrs", &count))
		return 0;

	base->slave_addrs = devm_kzalloc(base->dev, count, GFP_KERNEL);
	if (!base->slave_addrs)
		return -ENOMEM;

	base->slave_num = count;

	rc = of_property_read_variable_u8_array(np, "platform_slave_addrs",
						base->slave_addrs, count, 0);
	if (rc < 0) {
		mca_log_err("couldn't read platform_slave_addrs rc = %d\n", rc);
		/* Without the addresses there is nothing to create clients for. */
		base->slave_num = 0;

		return 0;
	}

	/* One client per listed address, filled in by the probe. */
	base->slaves = devm_kcalloc(base->dev, count, sizeof(*base->slaves),
				    GFP_KERNEL);
	if (!base->slaves) {
		base->slave_num = 0;

		return -ENOMEM;
	}

	return 0;
}

static int mca_platform_base_probe(struct i2c_client *client)
{
	struct mca_platform_base *base;
	int rc;
	int i;

	mca_log_info("probe start\n");

	base = devm_kzalloc(&client->dev, sizeof(*base), GFP_KERNEL);
	if (!base)
		return -ENOMEM;

	base->dev = &client->dev;
	i2c_set_clientdata(client, base);

	rc = mca_platform_base_parse_dt(base);
	if (rc)
		return rc;

	for (i = 0; i < base->slave_num; i++) {
		base->slaves[i] = devm_i2c_new_dummy_device(base->dev,
							    client->adapter,
							    base->slave_addrs[i]);
		if (IS_ERR(base->slaves[i])) {
			mca_log_err("failed to create new i2c[0x%02x] dev\n",
				    base->slave_addrs[i]);

			return PTR_ERR(base->slaves[i]);
		}
	}

	base->regmap = devm_regmap_init(base->dev,
					&mca_platform_base_regmap_bus, base,
					&mca_platform_base_regmap_config);
	if (IS_ERR(base->regmap)) {
		mca_log_info("failed to init regmap\n");

		return PTR_ERR(base->regmap);
	}

	mca_log_err("probe end\n");

	/* Now that every address has a client, bring up the child nodes. */
	return devm_of_platform_populate(base->dev);
}

static const struct of_device_id mca_platform_base_match[] = {
	{ .compatible = "mca,platform_base" },
	{ }
};
MODULE_DEVICE_TABLE(of, mca_platform_base_match);

static struct i2c_driver mca_platform_base_driver = {
	.driver = {
		.name = "mca_platform_base",
		.of_match_table = mca_platform_base_match,
	},
	.probe = mca_platform_base_probe,
};
module_i2c_driver(mca_platform_base_driver);

MODULE_DESCRIPTION("mca platform base");
MODULE_AUTHOR("getian@xiaomi.com");
MODULE_LICENSE("GPL v2");
