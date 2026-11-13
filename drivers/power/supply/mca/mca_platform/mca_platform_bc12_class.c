// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * BC1.2 charger detection.  See
 * include/mca/platform/platform_bc12_class.h.
 */

#define MCA_LOG_TAG "platform_bc12_class"

#include <linux/device.h>
#include <linux/errno.h>
#include <mca/common/mca_log.h>
#include <mca/platform/platform_bc12_class.h>
#include <mca/protocol/protocol_class.h>
#include <mca/protocol/protocol_pd_class.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/**
 * struct platform_class_bc12_ops_data - one registered charging path
 * @ops:  what the detecting driver provides
 * @data: handed back to every call
 */
struct platform_class_bc12_ops_data {
	const struct platform_bc12_class_ops	*ops;
	void					*data;
};

/**
 * struct platform_class_bc12_info - what this board detects with
 * @dev:                 the class device
 * @support_multi_bc12:  the board detects on more than one charging path
 */
struct platform_class_bc12_info {
	struct device	*dev;
	bool		support_multi_bc12;
};

static struct platform_class_bc12_ops_data g_bc12_ops_data[BC12_MAX_ROLE];

/*
 * Which of the charging paths a detection goes to.  A board with only one
 * path always uses the main one.  On a board with two, the path that matters
 * is the one an adapter has negotiated Power Delivery on, because that is
 * where the charger actually is; with nothing negotiated there, the main path
 * is still the one to ask.
 */
static enum platform_class_bc12_class_role_type
bc12_active_role(const struct platform_class_bc12_info *info)
{
	int pd_active = 0;

	if (!info->support_multi_bc12)
		return BC12_MAIN_ROLE;

	protocol_class_pd_get_pd_active(TYPEC_PORT_1, &pd_active);

	return pd_active ? BC12_AUX_ROLE : BC12_MAIN_ROLE;
}

int platform_bc12_class_ops_register(enum platform_class_bc12_class_role_type role,
				     const struct platform_bc12_class_ops *ops,
				     void *data)
{
	if (role >= BC12_MAX_ROLE || !ops)
		return -1;

	g_bc12_ops_data[role].ops = ops;
	g_bc12_ops_data[role].data = data;

	return 0;
}
EXPORT_SYMBOL(platform_bc12_class_ops_register);

static int platform_class_bc12_det_en(void *data, int enable)
{
	const struct platform_class_bc12_ops_data *p;

	if (!data)
		return -1;

	p = &g_bc12_ops_data[bc12_active_role(data)];
	if (!p->ops || !p->ops->bc12_det_en)
		return -1;

	return p->ops->bc12_det_en(enable, p->data);
}

static int platform_class_bc12_get_real_type(void *data, int *type)
{
	const struct platform_class_bc12_ops_data *p;

	if (!data)
		return -1;

	p = &g_bc12_ops_data[bc12_active_role(data)];
	if (!p->ops || !p->ops->get_charge_type)
		return -1;

	return p->ops->get_charge_type(type, p->data);
}

static const struct adapter_protocol_class_ops g_bc12_ops_table = {
	.adapter_det_en		= platform_class_bc12_det_en,
	.get_adapter_type	= platform_class_bc12_get_real_type,
};

static int platform_class_bc12_probe(struct platform_device *pdev)
{
	struct platform_class_bc12_info *info;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = &pdev->dev;
	info->support_multi_bc12 =
		of_find_property(pdev->dev.of_node, "support-multi-bc12",
				 NULL);

	protocol_class_register_ops(ADAPTER_PROTOCOL_BC12, &g_bc12_ops_table,
				    info);
	platform_set_drvdata(pdev, info);
	mca_log_err("probe ok\n");

	return 0;
}

static int platform_class_bc12_remove(struct platform_device *pdev)
{
	return 0;
}

static void platform_class_bc12_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,bc12_class" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver platform_bc12_class_driver = {
	.driver = {
		.name		= "platform_bc12_class",
		.of_match_table	= match_table,
	},
	.probe		= platform_class_bc12_probe,
	.remove		= platform_class_bc12_remove,
	.shutdown	= platform_class_bc12_shutdown,
};
module_platform_driver(platform_bc12_class_driver);

MODULE_DESCRIPTION("mca protocol class");
MODULE_AUTHOR("liyuze@xiaomi.com");
MODULE_LICENSE("GPL v2");
