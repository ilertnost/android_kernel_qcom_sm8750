// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The charge adapters the stack can talk to.  See
 * include/mca/protocol/protocol_class.h.
 */

#define pr_fmt(fmt) "[protocol_class]%s:%d " fmt, __func__, __LINE__

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/types.h>

#include <mca/protocol/protocol_class.h>

/**
 * struct adapter_protocol_class_data - one registered protocol
 * @ops:  what the implementation can do
 * @data: handed back to every call
 */
struct adapter_protocol_class_data {
	const struct adapter_protocol_class_ops	*ops;
	void					*data;
};

static struct adapter_protocol_class_data
	g_protocol_class_data[ADAPTER_PROTOCOL_MAX];

/**
 * protocol_class_register_ops() - offer a protocol to the stack
 * @protocol: which protocol this implements
 * @ops:      what it can do
 * @data:     handed back to every call
 */
int protocol_class_register_ops(enum adatper_protocol protocol,
				const struct adapter_protocol_class_ops *ops,
				void *data)
{
	if (protocol >= ADAPTER_PROTOCOL_MAX || !ops)
		return -1;

	g_protocol_class_data[protocol].ops = ops;
	g_protocol_class_data[protocol].data = data;

	return 0;
}
EXPORT_SYMBOL(protocol_class_register_ops);

/*
 * Each call below finds the implementation for a protocol and forwards to it.
 * A protocol that is not registered, or that does not implement the call,
 * gives -1 rather than a made up answer, so a caller can tell the
 * difference between "no" and "cannot say".
 *
 * The value is -1 rather than an errno: that is what the vendor's class
 * layer answers, callers propagate it unchanged, and some of it reaches
 * userspace through sysfs.
 */
#define ADAPTER_PROTOCOL_CALL(protocol, method, ...)			\
({									\
	struct adapter_protocol_class_data *__p;			\
	int __ret = -1;						\
									\
	if ((protocol) < ADAPTER_PROTOCOL_MAX) {			\
		__p = &g_protocol_class_data[protocol];			\
		if (__p->ops && __p->ops->method)			\
			__ret = __p->ops->method(__p->data,		\
						 ##__VA_ARGS__);	\
	}								\
									\
	__ret;								\
})

int protocol_class_det_adapter_type(enum adatper_protocol protocol, int enable)
{
	return ADAPTER_PROTOCOL_CALL(protocol, adapter_det_en, enable);
}
EXPORT_SYMBOL(protocol_class_det_adapter_type);

int protocol_class_set_adapter_verified(enum adatper_protocol protocol,
					int verified)
{
	return ADAPTER_PROTOCOL_CALL(protocol, set_adapter_verified, verified);
}
EXPORT_SYMBOL(protocol_class_set_adapter_verified);

int protocol_class_get_adapter_verified(enum adatper_protocol protocol,
					int *verified)
{
	return ADAPTER_PROTOCOL_CALL(protocol, get_adapter_verified, verified);
}
EXPORT_SYMBOL(protocol_class_get_adapter_verified);

int protocol_class_get_adapter_type(enum adatper_protocol protocol, u32 *type)
{
	return ADAPTER_PROTOCOL_CALL(protocol, get_adapter_type, (int *)type);
}
EXPORT_SYMBOL(protocol_class_get_adapter_type);

int protocol_class_get_adapter_max_power(enum adatper_protocol protocol,
					 u32 *max_power_mw)
{
	return ADAPTER_PROTOCOL_CALL(protocol, get_adapter_max_power,
				     max_power_mw);
}
EXPORT_SYMBOL(protocol_class_get_adapter_max_power);

int protocol_class_get_adapter_pwr_max_power(enum adatper_protocol protocol,
					     u32 *max_power_mw)
{
	return ADAPTER_PROTOCOL_CALL(protocol, get_adapter_pwr_max_power,
				     max_power_mw);
}
EXPORT_SYMBOL(protocol_class_get_adapter_pwr_max_power);

int protocol_class_get_adapter_power_cap(enum adatper_protocol protocol,
					 struct adapter_power_cap_info *cap)
{
	return ADAPTER_PROTOCOL_CALL(protocol, get_adapter_pwr_cap, cap);
}
EXPORT_SYMBOL(protocol_class_get_adapter_power_cap);

int protocol_class_set_adapter_volt_and_curr(enum adatper_protocol protocol,
					     int volt_mv, int curr_ma)
{
	return ADAPTER_PROTOCOL_CALL(protocol, set_adapter_volt_and_curr,
				     volt_mv, curr_ma);
}
EXPORT_SYMBOL(protocol_class_set_adapter_volt_and_curr);

int protocol_class_get_adapter_volt_and_curr(enum adatper_protocol protocol,
					     int *volt_mv, int *curr_ma)
{
	return ADAPTER_PROTOCOL_CALL(protocol, get_adapter_volt_and_curr,
				     volt_mv, curr_ma);
}
EXPORT_SYMBOL(protocol_class_get_adapter_volt_and_curr);

int protocol_class_get_adapter_pps_ptf(enum adatper_protocol protocol,
				       int *pps_ptf)
{
	return ADAPTER_PROTOCOL_CALL(protocol, get_adapter_pps_ptf, pps_ptf);
}
EXPORT_SYMBOL(protocol_class_get_adapter_pps_ptf);

int protocol_class_get_adapter_info(enum adatper_protocol protocol,
				    struct adapter_vendor_info *info)
{
	return ADAPTER_PROTOCOL_CALL(protocol, get_adapter_info, info);
}
EXPORT_SYMBOL(protocol_class_get_adapter_info);

int protocol_class_get_adapter_power_curve(enum adatper_protocol protocol,
					   struct adapter_power_curve *pwr_curve)
{
	return ADAPTER_PROTOCOL_CALL(protocol, get_adapter_power_curve,
				     pwr_curve);
}
EXPORT_SYMBOL(protocol_class_get_adapter_power_curve);

/*
 * The registries above are reached by exported symbol rather than through a
 * device, so this driver matches nothing and has no probe.  It is registered
 * because the vendor registers it: what it leaves behind is an entry under
 * /sys/bus/platform/drivers, which is where anything looking for the charging
 * stack expects to find it.
 */
static struct platform_driver protocol_class_driver = {
	.driver = {
		.name = "protocol_class",
	},
};
module_platform_driver(protocol_class_driver);

MODULE_DESCRIPTION("mca protocol class");
MODULE_AUTHOR("getian@xiaomi.com");
MODULE_LICENSE("GPL v2");
