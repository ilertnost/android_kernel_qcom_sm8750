/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The charging strategies.
 *
 * How fast the battery is charged is not one decision but several, each made
 * by a module that knows one thing well: what the cell will take at this
 * temperature, what the adapter can supply, how warm the phone has become,
 * whether the user asked for it to charge slowly overnight.  Each registers
 * here under what it decides, and whoever notices a change asks the strategy
 * to run again rather than working out the answer itself.
 */

#ifndef __MCA_STRATEGY_CLASS_H
#define __MCA_STRATEGY_CLASS_H

#include <linux/types.h>

/* What a strategy decides. */
enum mca_strategy_func_type {
	STRATEGY_FUNC_TYPE_BUCK_CHARGE,
	STRATEGY_FUNC_TYPE_QUICK_CHARGE,
	STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
	STRATEGY_FUNC_TYPE_QUICK_WIRELESS,
	STRATEGY_FUNC_TYPE_REV_WIRELESS,
	STRATEGY_FUNC_TYPE_THERMAL,
	STRATEGY_FUNC_TYPE_FG,
	STRATEGY_FUNC_TYPE_JEITA,
	STRATEGY_FUNC_TYPE_SMARTCHG,
	STRATEGY_FUNC_TYPE_BMD,
	STRATEGY_FUNC_TYPE_ANTIBURN,
	STRATEGY_FUNC_TYPE_MAX,
};

/*
 * What a strategy can be asked about.  A strategy answers only the few of
 * these that are its own; the rest belong to other strategies and are listed
 * here because the numbering is shared across all of them.
 */
enum mca_strategy_status {
	STRATEGY_STATUS_TYPE_ONLINE,
	STRATEGY_STATUS_TYPE_INIT_OK,
	STRATEGY_STATUS_TYPE_CHARGING,
	STRATEGY_STATUS_TYPE_QC_ENABLE,
	STRATEGY_STATUS_TYPE_QC_TYPE,
	STRATEGY_STATUS_TYPE_QC_IBAT_MAX,
	STRATEGY_STATUS_TYPE_QC_START_FLAG,
	STRATEGY_STATUS_TYPE_REV_TEST,
	STRATEGY_STATUS_TYPE_WLS_MAGNET_LIMIT,
	STRATEGY_STATUS_TYPE_POWER_MAX,
	STRATEGY_STATUS_TYPE_ENABLE,
	STRATEGY_STATUS_TYPE_MODE,
	STRATEGY_STATUS_TYPE_VBUS,
	STRATEGY_STATUS_TYPE_IBUS,
	STRATEGY_STATUS_TYPE_JEITA_FFC_ITERM,
	STRATEGY_STATUS_TYPE_JEITA_NORMAL_VTERM,
	STRATEGY_STATUS_TYPE_JEITA_FFC_VTERM,
	STRATEGY_STATUS_TYPE_CP_TO_PMIC,
	STRATEGY_STATUS_TYPE_AUTH_PASS,
	STRATEGY_STATUS_TYPE_BMD,
	STRATEGY_STATUS_TYPE_DEVICE_POWER_MAX,
	STRATEGY_STATUS_TYPE_FORCE_FULL_STATUS,
	STRATEGY_STATUS_TYPE_GET_VOLTAGE_MAX,
	STRATEGY_STATUS_TYPE_GET_CURRENT_MAX,
	STRATEGY_STATUS_TYPE_MAX,
};

/* Where the fast-charging path stands. */
enum mca_quick_charge_charge_status {
	MCA_QUICK_CHG_STS_NO_CHARGING = 0,
	MCA_QUICK_CHG_STS_CHARGING,
	MCA_QUICK_CHG_STS_CHARGE_DONE,
	MCA_QUICK_CHG_STS_CHARGE_FAILED,
};

/*
 * Where the buck charger stands.  "Not charging" and "not available" are
 * different answers: the first means there is a charger and it has stopped,
 * the second that there is nothing to ask.
 */
enum mca_buck_chg_status {
	MCA_BUCK_CHG_NO_CHARGING = 0,
	MCA_BUCK_CHG_STS_CHARGING,
	MCA_BUCK_CHG_STS_CHARGE_DONE,
	MCA_BUCK_CHG_STS_NA,
};

/*
 * What a strategy can be told to change.  Only the input limit is settable
 * from outside: everything else a strategy decides is its own to decide.
 */
enum mca_strategy_config_type {
	STRATEGY_CONFIG_INPUT_CURRENT_LIMIT = 0,
	STRATEGY_CONFIG_MAX,
};

/**
 * typedef mca_strategy_func - run the strategy again
 * @func:  which of the strategy's decisions to make
 * @value: what prompted it
 * @data:  what the strategy registered
 */
typedef int (*mca_strategy_func)(int func, int value, void *data);

/**
 * typedef mca_strategy_get_status - ask a strategy where it stands
 * @func:   which of the strategy's decisions to ask about
 * @status: filled in with the answer
 * @data:   what the strategy registered
 */
typedef int (*mca_strategy_get_status)(int func, int *status, void *data);

/**
 * typedef mca_strategy_set_config - change how a strategy decides
 * @config: which setting
 * @value:  what to set it to
 * @data:   what the strategy registered
 */
typedef int (*mca_strategy_set_config)(int config, int value, void *data);

int mca_strategy_ops_register(enum mca_strategy_func_type type,
			      mca_strategy_func func,
			      mca_strategy_get_status get_func,
			      mca_strategy_set_config set_func, void *data);

int mca_strategy_func_process(enum mca_strategy_func_type type, int func,
			      int value);
int mca_strategy_func_get_status(enum mca_strategy_func_type type, int func,
				 int *status);
int mca_strategy_func_set_config(enum mca_strategy_func_type type, int conifg,
				 int value);

#endif /* __MCA_STRATEGY_CLASS_H */
