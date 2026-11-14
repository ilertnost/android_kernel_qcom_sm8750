// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * What the charging strategies are told about the battery.  See
 * include/mca/strategy/strategy_fg_class.h.
 */

#include <linux/errno.h>
#include <mca/strategy/strategy_fg_class.h>
#include <linux/module.h>
#include <linux/platform_device.h>

/**
 * struct strategy_fg_class_info - the registered reconciling module
 * @data: handed back to every call
 * @ops:  what it provides
 */
struct strategy_fg_class_info {
	void					*data;
	const struct strategy_fg_class_ops	*ops;
};

/*
 * There is one battery, so there is one of these: unlike the gauge chips
 * below it, nothing above wants to ask a particular instance.
 */
static struct strategy_fg_class_info g_strategy_fg;

/* Registering NULL ops withdraws the strategy, e.g. when it is removed. */
int strategy_class_fg_ops_register(void *data,
				   const struct strategy_fg_class_ops *ops)
{
	g_strategy_fg.ops = ops;
	g_strategy_fg.data = ops ? data : NULL;

	return 0;
}
EXPORT_SYMBOL(strategy_class_fg_ops_register);

int strategy_class_fg_ops_is_init_ok(void)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_is_init_ok)
		return -1;

	return p->ops->strategy_fg_is_init_ok(p->data);
}
EXPORT_SYMBOL(strategy_class_fg_ops_is_init_ok);

int strategy_class_fg_is_chip_ok(void)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_is_chip_ok)
		return -1;

	return p->ops->strategy_fg_is_chip_ok(p->data);
}
EXPORT_SYMBOL(strategy_class_fg_is_chip_ok);

int strategy_class_fg_dual_is_chip_ok(int ic)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_dual_is_chip_ok)
		return -1;

	return p->ops->strategy_fg_dual_is_chip_ok(p->data, ic);
}
EXPORT_SYMBOL(strategy_class_fg_dual_is_chip_ok);

int strategy_class_fg_ops_get_rsoc(int *rsoc)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_rsoc)
		return -1;

	return p->ops->strategy_fg_get_rsoc(p->data, rsoc);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_rsoc);

int strategy_class_fg_ops_get_soc(void)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_soc)
		return -1;

	return p->ops->strategy_fg_get_soc(p->data);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_soc);

int strategy_class_fg_ops_get_temperature(int *temp)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_temp)
		return -1;

	return p->ops->strategy_fg_get_temp(p->data, temp);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_temperature);

int strategy_class_fg_ops_get_thermal_temperature(int *temp)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_thermal_temp)
		return -1;

	return p->ops->strategy_fg_get_thermal_temp(p->data, temp);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_thermal_temperature);

int strategy_class_fg_ops_get_current(int *curr)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_current)
		return -1;

	return p->ops->strategy_fg_get_current(p->data, curr);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_current);

int strategy_class_fg_ops_get_voltage(int *volt)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_voltage)
		return -1;

	return p->ops->strategy_fg_get_voltage(p->data, volt);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_voltage);

int strategy_class_fg_ops_get_cyclecount(int *cycle)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_cycle)
		return -1;

	return p->ops->strategy_fg_get_cycle(p->data, cycle);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_cyclecount);

int strategy_class_fg_get_rm(int *rm)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_rm)
		return -1;

	return p->ops->strategy_fg_get_rm(p->data, rm);
}
EXPORT_SYMBOL(strategy_class_fg_get_rm);

int strategy_class_fg_get_voltage_mean(int *vol_mean)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_voltage_mean)
		return -1;

	return p->ops->strategy_fg_get_voltage_mean(p->data, vol_mean);
}
EXPORT_SYMBOL(strategy_class_fg_get_voltage_mean);

int strategy_class_fg_ops_get_soc_decimal(int *soc_decimal,
					  int *soc_decimal_rate)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_soc_decimal_info)
		return -1;

	return p->ops->strategy_fg_get_soc_decimal_info(p->data, soc_decimal,
						       soc_decimal_rate);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_soc_decimal);

bool strategy_class_fg_ops_get_charging_done(void)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	/* With nobody registered the battery is reported as done charging. */
	if (!p->ops || !p->ops->strategy_fg_get_charging_done)
		return true;

	return p->ops->strategy_fg_get_charging_done(p->data);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_charging_done);

int strategy_class_fg_ops_set_charging_done(bool charging_done)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_set_charging_done)
		return -1;

	return p->ops->strategy_fg_set_charging_done(p->data, charging_done);
}
EXPORT_SYMBOL(strategy_class_fg_ops_set_charging_done);

int strategy_class_fg_get_model_name(const char **model_name)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_model_name)
		return -1;

	return p->ops->strategy_fg_get_model_name(p->data, model_name);
}
EXPORT_SYMBOL(strategy_class_fg_get_model_name);

int strategy_class_fg_set_fastcharge(bool fastcharge)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_set_fastcharge)
		return -1;

	return p->ops->strategy_fg_set_fastcharge(p->data, fastcharge);
}
EXPORT_SYMBOL(strategy_class_fg_set_fastcharge);

int strategy_class_fg_get_fastcharge(void)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_fastcharge)
		return -1;

	return p->ops->strategy_fg_get_fastcharge(p->data);
}
EXPORT_SYMBOL(strategy_class_fg_get_fastcharge);

int strategy_class_fg_get_authentic(bool *authentic)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_authentic)
		return -1;

	return p->ops->strategy_fg_get_authentic(p->data, authentic);
}
EXPORT_SYMBOL(strategy_class_fg_get_authentic);

int strategy_class_fg_get_dc(int *dc)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_dc)
		return -1;

	return p->ops->strategy_fg_get_dc(p->data, dc);
}
EXPORT_SYMBOL(strategy_class_fg_get_dc);

int strategy_class_fg_get_fcc(int *fcc)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_fcc)
		return -1;

	return p->ops->strategy_fg_get_fcc(p->data, fcc);
}
EXPORT_SYMBOL(strategy_class_fg_get_fcc);

int strategy_class_fg_get_health(int *health)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_health)
		return -1;

	return p->ops->strategy_fg_get_health(p->data, health);
}
EXPORT_SYMBOL(strategy_class_fg_get_health);

int strategy_class_fg_get_soh(int *soh)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_soh)
		return -1;

	return p->ops->strategy_fg_get_soh(p->data, soh);
}
EXPORT_SYMBOL(strategy_class_fg_get_soh);

int strategy_class_fg_get_first_termination(int *first_termination)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_first_termiation)
		return -1;

	return p->ops->strategy_fg_get_first_termiation(p->data, first_termination);
}
EXPORT_SYMBOL(strategy_class_fg_get_first_termination);

int strategy_class_fg_get_pack_vendor_id(int *pack_vendor_id)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_pack_vendor_id)
		return -1;

	return p->ops->strategy_fg_get_pack_vendor_id(p->data, pack_vendor_id);
}
EXPORT_SYMBOL(strategy_class_fg_get_pack_vendor_id);

int strategy_class_fg_get_temp_offset_flag(int *temp_offset_flag)
{
	struct strategy_fg_class_info *p = &g_strategy_fg;

	if (!p->ops || !p->ops->strategy_fg_get_temp_offset_flag)
		return -1;

	return p->ops->strategy_fg_get_temp_offset_flag(p->data, temp_offset_flag);
}
EXPORT_SYMBOL(strategy_class_fg_get_temp_offset_flag);
/*
 * The registries above are reached by exported symbol rather than through a
 * device, so this driver matches nothing and has no probe.  It is registered
 * because the vendor registers it: what it leaves behind is an entry under
 * /sys/bus/platform/drivers, which is where anything looking for the charging
 * stack expects to find it.
 */
static struct platform_driver strategy_fg_class_driver = {
	.driver = {
		.name = "strategy_fg_class",
	},
};
module_platform_driver(strategy_fg_class_driver);

MODULE_DESCRIPTION("platform fg class");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
