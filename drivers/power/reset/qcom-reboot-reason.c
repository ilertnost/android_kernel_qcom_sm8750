// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019, 2021 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/pm.h>
#include <linux/of_address.h>
#include <linux/nvmem-consumer.h>
#include <linux/panic_notifier.h>
#include <linux/input/qpnp-power-on.h>

struct qcom_reboot_reason {
	struct device *dev;
	struct notifier_block reboot_nb;
	struct notifier_block panic_nb;
	struct nvmem_cell *nvmem_cell;
};

struct poweroff_reason {
	const char *cmd;
	unsigned char pon_reason;
};

static struct poweroff_reason reasons[] = {
	{ "recovery",			0x01 },
	{ "bootloader",			0x02 },
	{ "rtc",			0x03 },
	{ "dm-verity device corrupted",	0x04 },
	{ "dm-verity enforcing",	0x05 },
	{ "keys clear",			0x06 },
	{ "panic",			0x21 },
	{}
};

#define DM_VERITY_CORRUPTED_CMD		"dm-verity device corrupted"

/* Reasons the bootloader expects for a plain reboot and after a panic */
#define PON_REASON_DM_VERITY_CORRUPTED	0x04
#define PON_REASON_NORMAL		0x20
#define PON_REASON_PANIC		0x21

static void qcom_reboot_reason_write(struct qcom_reboot_reason *reboot,
				     unsigned char pon_reason)
{
	if (reboot->nvmem_cell)
		nvmem_cell_write(reboot->nvmem_cell, &pon_reason,
				 sizeof(pon_reason));
	else
		qpnp_pon_set_restart_reason((enum pon_restart_reason)pon_reason);
}

static int qcom_reboot_reason_reboot(struct notifier_block *this,
				     unsigned long event, void *ptr)
{
	char *cmd = ptr;
	struct qcom_reboot_reason *reboot = container_of(this,
		struct qcom_reboot_reason, reboot_nb);
	struct poweroff_reason *reason;

	if (!cmd)
		goto normal;

	for (reason = reasons; reason->cmd; reason++) {
		if (strcmp(cmd, reason->cmd))
			continue;

		/*
		 * Panic on dm-verity corruption so the failure is logged; the
		 * panic notifier then records the reason for the bootloader.
		 */
		if (!strcmp(cmd, DM_VERITY_CORRUPTED_CMD)) {
			pr_err("!!! trigger dm-verity panic !!!");
			panic(cmd);
		}

		qcom_reboot_reason_write(reboot, reason->pon_reason);
		return NOTIFY_OK;
	}

normal:
	qcom_reboot_reason_write(reboot, PON_REASON_NORMAL);

	return NOTIFY_OK;
}

static int qcom_reboot_reason_panic(struct notifier_block *this,
				    unsigned long event, void *ptr)
{
	struct qcom_reboot_reason *reboot = container_of(this,
		struct qcom_reboot_reason, panic_nb);
	const char *msg = ptr;

	if (!strcmp(msg, DM_VERITY_CORRUPTED_CMD))
		qcom_reboot_reason_write(reboot, PON_REASON_DM_VERITY_CORRUPTED);
	else
		qcom_reboot_reason_write(reboot, PON_REASON_PANIC);

	return NOTIFY_DONE;
}

static int qcom_reboot_reason_probe(struct platform_device *pdev)
{
	struct qcom_reboot_reason *reboot;

	reboot = devm_kzalloc(&pdev->dev, sizeof(*reboot), GFP_KERNEL);
	if (!reboot)
		return -ENOMEM;

	reboot->dev = &pdev->dev;

	if (of_property_read_bool(pdev->dev.of_node, "qcom,no-nvmem-cell-support"))
		reboot->nvmem_cell = NULL;
	else {
		reboot->nvmem_cell = nvmem_cell_get(reboot->dev, "restart_reason");
		if (IS_ERR(reboot->nvmem_cell))
			return PTR_ERR(reboot->nvmem_cell);
	}

	reboot->reboot_nb.notifier_call = qcom_reboot_reason_reboot;
	reboot->reboot_nb.priority = 255;
	register_reboot_notifier(&reboot->reboot_nb);

	platform_set_drvdata(pdev, reboot);

	reboot->panic_nb.notifier_call = qcom_reboot_reason_panic;
	reboot->panic_nb.priority = INT_MAX;
	atomic_notifier_chain_register(&panic_notifier_list, &reboot->panic_nb);

	return 0;
}

static int qcom_reboot_reason_remove(struct platform_device *pdev)
{
	struct qcom_reboot_reason *reboot = platform_get_drvdata(pdev);

	atomic_notifier_chain_unregister(&panic_notifier_list, &reboot->panic_nb);
	unregister_reboot_notifier(&reboot->reboot_nb);

	return 0;
}

static const struct of_device_id of_qcom_reboot_reason_match[] = {
	{ .compatible = "qcom,reboot-reason", },
	{},
};
MODULE_DEVICE_TABLE(of, of_qcom_reboot_reason_match);

static struct platform_driver qcom_reboot_reason_driver = {
	.probe = qcom_reboot_reason_probe,
	.remove = qcom_reboot_reason_remove,
	.driver = {
		.name = "qcom-reboot-reason",
		.of_match_table = of_match_ptr(of_qcom_reboot_reason_match),
	},
};

module_platform_driver(qcom_reboot_reason_driver);

MODULE_DESCRIPTION("MSM Reboot Reason Driver");
MODULE_LICENSE("GPL");
