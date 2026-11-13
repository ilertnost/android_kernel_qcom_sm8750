// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Quick Charge adapters.  See include/mca/protocol/protocol_qc_class.h.
 */

#include <linux/errno.h>
#include <mca/protocol/protocol_class.h>
#include <mca/protocol/protocol_qc_class.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/**
 * struct protocol_class_qc_ops_data - one registered charger driver
 * @data: handed back to every call
 * @ops:  what the charger driver provides
 */
struct protocol_class_qc_ops_data {
	void					*data;
	const struct protocol_class_qc_ops	*ops;
};

/*
 * The charger driver probes on its own schedule, so keep what it registers
 * here rather than in the platform device: callers reach Quick Charge by
 * protocol, not by device, and get -1 until the charger has arrived.
 */
static struct protocol_class_qc_ops_data g_protocol_qc_data[ADAPTER_PROTOCOL_MAX];

int protocol_class_qc_register_ops(enum adatper_protocol protocol,
				   const struct protocol_class_qc_ops *ops,
				   void *data)
{
	if (protocol >= ADAPTER_PROTOCOL_MAX || !ops)
		return -1;

	g_protocol_qc_data[protocol].ops = ops;
	g_protocol_qc_data[protocol].data = data;

	return 0;
}
EXPORT_SYMBOL(protocol_class_qc_register_ops);

/*
 * Look up the charger driver that answers for a protocol.  A protocol nobody
 * registered for gives NULL, and the caller reports -1 rather than a made
 * up answer.
 *
 * The value is -1 rather than an errno: that is what the vendor's class
 * layer answers, callers propagate it unchanged, and some of it reaches
 * userspace through sysfs.
 */
static struct protocol_class_qc_ops_data *
protocol_class_qc_lookup(enum adatper_protocol protocol)
{
	if (protocol >= ADAPTER_PROTOCOL_MAX)
		return NULL;

	if (!g_protocol_qc_data[protocol].ops)
		return NULL;

	return &g_protocol_qc_data[protocol];
}

int protocol_class_qc_set_volt(enum adatper_protocol protocol, int volt)
{
	struct protocol_class_qc_ops_data *p = protocol_class_qc_lookup(protocol);

	if (!p || !p->ops->protocol_qc_set_volt)
		return -1;

	return p->ops->protocol_qc_set_volt(p->data, volt);
}
EXPORT_SYMBOL(protocol_class_qc_set_volt);

int protocol_class_qc_set_volt_cmd(enum adatper_protocol protocol,
				   int hvdcp_cmd)
{
	struct protocol_class_qc_ops_data *p = protocol_class_qc_lookup(protocol);

	if (!p || !p->ops->protocol_qc_set_volt_cmd)
		return -1;

	return p->ops->protocol_qc_set_volt_cmd(p->data, hvdcp_cmd);
}
EXPORT_SYMBOL(protocol_class_qc_set_volt_cmd);

int protocol_class_qc_get_qc_type(enum adatper_protocol protocol, int *qc_type)
{
	struct protocol_class_qc_ops_data *p = protocol_class_qc_lookup(protocol);

	if (!p || !p->ops->protocol_qc_get_qc_type)
		return -1;

	return p->ops->protocol_qc_get_qc_type(qc_type, p->data);
}
EXPORT_SYMBOL(protocol_class_qc_get_qc_type);

int protocol_class_qc3_check_class_type(enum adatper_protocol protocol,
					int *class_type)
{
	struct protocol_class_qc_ops_data *p = protocol_class_qc_lookup(protocol);

	if (!p || !p->ops->protocol_qc3_check_class_type)
		return -1;

	return p->ops->protocol_qc3_check_class_type(class_type, p->data);
}
EXPORT_SYMBOL(protocol_class_qc3_check_class_type);

/*
 * Quick Charge has no current limit to set -- the adapter supplies what it
 * can at the voltage it was asked for -- so the requested current is dropped
 * rather than being reported as unsupported.
 */
static int protocol_class_qc_set_volt_and_curr(void *data, int volt, int curr)
{
	return protocol_class_qc_set_volt(ADAPTER_PROTOCOL_QC, volt);
}

static int protocol_class_qc_get_adapter_type(void *data, int *type)
{
	return protocol_class_qc_get_qc_type(ADAPTER_PROTOCOL_QC, type);
}

static const struct adapter_protocol_class_ops g_protocol_qc_ops = {
	.get_adapter_type		= protocol_class_qc_get_adapter_type,
	.set_adapter_volt_and_curr	= protocol_class_qc_set_volt_and_curr,
};

static int protocol_qc_class_probe(struct platform_device *pdev)
{
	return protocol_class_register_ops(ADAPTER_PROTOCOL_QC,
					   &g_protocol_qc_ops, NULL);
}

static int protocol_qc_class_remove(struct platform_device *pdev)
{
	return 0;
}

static void protocol_qc_class_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,protocol_qc" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver protocol_qc_class_driver = {
	.driver = {
		.name		= "protocol_qc_class",
		.of_match_table	= match_table,
	},
	.probe		= protocol_qc_class_probe,
	.remove		= protocol_qc_class_remove,
	.shutdown	= protocol_qc_class_shutdown,
};
module_platform_driver(protocol_qc_class_driver);

MODULE_DESCRIPTION("protocol qc class");
MODULE_AUTHOR("muxinyi1@xiaomi.com");
MODULE_LICENSE("GPL v2");
