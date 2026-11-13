// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The charging strategies.  See include/mca/strategy/strategy_class.h.
 */

#include <linux/errno.h>
#include <mca/strategy/strategy_class.h>
#include <linux/module.h>
#include <linux/platform_device.h>

/**
 * struct mca_strategy_func_data - one registered strategy
 * @func:     run it again
 * @get_func: ask it where it stands
 * @set_func: change how it decides
 * @data:     handed back to each of them
 */
struct mca_strategy_func_data {
	mca_strategy_func	func;
	mca_strategy_get_status	get_func;
	mca_strategy_set_config	set_func;
	void			*data;
};

static struct mca_strategy_func_data g_mca_stg_func[STRATEGY_FUNC_TYPE_MAX];

/**
 * mca_strategy_ops_register() - offer a strategy to the stack
 * @type:     what this strategy decides
 * @func:     called to run it again
 * @get_func: called to ask where it stands, may be NULL
 * @set_func: called to change how it decides, may be NULL
 * @data:     handed back to each of them
 */
int mca_strategy_ops_register(enum mca_strategy_func_type type,
			      mca_strategy_func func,
			      mca_strategy_get_status get_func,
			      mca_strategy_set_config set_func, void *data)
{
	if (type >= STRATEGY_FUNC_TYPE_MAX)
		return -1;

	g_mca_stg_func[type].func = func;
	g_mca_stg_func[type].get_func = get_func;
	g_mca_stg_func[type].set_func = set_func;
	g_mca_stg_func[type].data = data;

	return 0;
}
EXPORT_SYMBOL(mca_strategy_ops_register);

/*
 * A strategy that has not loaded yet gives -1 rather than a made up
 * answer: the caller is usually an event handler that will be called again,
 * and a wrong answer would be acted on.
 *
 * The value is -1 rather than an errno: callers propagate it unchanged, and
 * some of it reaches userspace through sysfs.
 */
int mca_strategy_func_process(enum mca_strategy_func_type type, int func,
			      int value)
{
	if (type >= STRATEGY_FUNC_TYPE_MAX || !g_mca_stg_func[type].func)
		return -1;

	return g_mca_stg_func[type].func(func, value,
					 g_mca_stg_func[type].data);
}
EXPORT_SYMBOL(mca_strategy_func_process);

int mca_strategy_func_get_status(enum mca_strategy_func_type type, int func,
				 int *status)
{
	if (type >= STRATEGY_FUNC_TYPE_MAX || !g_mca_stg_func[type].get_func)
		return -1;

	return g_mca_stg_func[type].get_func(func, status,
					     g_mca_stg_func[type].data);
}
EXPORT_SYMBOL(mca_strategy_func_get_status);

int mca_strategy_func_set_config(enum mca_strategy_func_type type, int conifg,
				 int value)
{
	if (type >= STRATEGY_FUNC_TYPE_MAX || !g_mca_stg_func[type].set_func)
		return -1;

	return g_mca_stg_func[type].set_func(conifg, value,
					     g_mca_stg_func[type].data);
}
EXPORT_SYMBOL(mca_strategy_func_set_config);

/*
 * Nothing in the device tree describes the strategy class: it holds no
 * hardware and exists so that the strategies and the drivers that prompt them
 * can find each other.  The driver is still registered, so that the class
 * shows up beside the rest of the stack and a board that wants to bind
 * something to it can.
 */
static struct platform_driver mca_strategy_class_driver = {
	.driver = {
		.name = "mca_strategy_class",
	},
};
module_platform_driver(mca_strategy_class_driver);

MODULE_DESCRIPTION("mca strategy class");
MODULE_AUTHOR("getian@xiaomi.com");
MODULE_LICENSE("GPL v2");
