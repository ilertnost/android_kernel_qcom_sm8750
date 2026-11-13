// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Capping the state of charge.  See
 * include/mca/strategy/strategy_soc_limit.h.
 */

#define MCA_LOG_TAG "mca_soc_limit"

#include <linux/device.h>
#include <linux/errno.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/strategy/strategy_soc_limit.h>
#include <mca/strategy/strategy_fg_class.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/**
 * struct soc_limit_info - the cap in force
 * @dev:              this device
 * @soc_limit_enable: whether a cap is being applied
 * @curr_soc:         the last state of charge seen
 */
struct soc_limit_info {
	struct device	*dev;
	bool		soc_limit_enable;
	int		curr_soc;
};

static struct soc_limit_info *g_soc_limit;

/**
 * soc_limit_process() - apply or lift a cap on the state of charge
 * @en:             whether a cap is requested
 * @soc_limit_thre: the cap, in percent
 *
 * Announces whether the cap is in force rather than acting on it: the
 * strategies decide what stopping means.  While a cap is requested but the
 * state of charge has not passed it yet, nothing is announced.
 */
void soc_limit_process(bool en, int soc_limit_thre)
{
	struct soc_limit_info *info = g_soc_limit;

	if (!info)
		return;

	if (!en) {
		info->soc_limit_enable = false;
		mca_event_block_notify(MCA_EVENT_CHARGE_STATUS,
				       MCA_EVENT_SOC_LIMIT,
				       &info->soc_limit_enable);
		mca_log_info("disable soc limit\n");
		return;
	}

	info->curr_soc = strategy_class_fg_ops_get_soc();
	if (info->curr_soc <= soc_limit_thre)
		return;

	info->soc_limit_enable = true;
	mca_event_block_notify(MCA_EVENT_CHARGE_STATUS, MCA_EVENT_SOC_LIMIT,
			       &info->soc_limit_enable);
	mca_log_info("soc_limit_thre = %d\n", soc_limit_thre);
}
EXPORT_SYMBOL(soc_limit_process);

static int soc_limit_probe(struct platform_device *pdev)
{
	static int probe_cnt;

	mca_log_info("probe_cnt = %d\n", ++probe_cnt);

	g_soc_limit = devm_kzalloc(&pdev->dev, sizeof(*g_soc_limit),
				   GFP_KERNEL);
	if (!g_soc_limit) {
		mca_log_err("out of memory\n");
		return -ENOMEM;
	}

	g_soc_limit->dev = &pdev->dev;
	platform_set_drvdata(pdev, g_soc_limit);

	mca_log_info("done\n");

	return 0;
}

static int soc_limit_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id match_table[] = {
	{ .compatible = "xiaomi,soc_limit" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver soc_limit_driver = {
	.driver = {
		.name		= "soc_limit",
		.of_match_table	= match_table,
	},
	.probe		= soc_limit_probe,
	.remove		= soc_limit_remove,
};
module_platform_driver(soc_limit_driver);

MODULE_DESCRIPTION("soc limit driver");
MODULE_AUTHOR("liuzhengqing@xiaomi.com");
MODULE_LICENSE("GPL v2");
