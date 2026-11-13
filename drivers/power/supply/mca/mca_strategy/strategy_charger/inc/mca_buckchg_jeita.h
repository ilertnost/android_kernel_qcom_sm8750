/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * JEITA charging limits for the buck charger.
 *
 * The board's device tree gives the temperature bands as string tables, one
 * row per band: the temperatures it covers, the hysteresis to leave it by,
 * the current and float voltage allowed there, the current charging finishes
 * at, and the name of a further table that caps the current by battery
 * voltage ("null" when there is none).  A second table with "_ffc" appended
 * to its name applies while fast charging.
 */

#ifndef __MCA_BUCKCHG_JEITA_H__
#define __MCA_BUCKCHG_JEITA_H__

#include <linux/types.h>
#include <linux/workqueue.h>

/* How often the limits are looked at again, in milliseconds. */
#define MCA_BUCKCHG_JEITA_INIT_VOTER_INTERVAL		1000
#define MCA_BUCKCHG_JEITA_UPDATE_NORMAL_INTERVAL	3000

/* What the flip half is allowed until its own table says otherwise. */
#define MCA_BUCKCHG_JEITA_FLIP_CURRENT_DEFAULT_VALUE	2950

/* Default margins for a battery-voltage step, in millivolts. */
#define MCA_BUCKCHG_JEITA_VBAT_HIGH_HYST		50
#define MCA_BUCKCHG_JEITA_VBAT_LOW_HYST			50

/* The most voltage steps a band may give. */
#define JEITA_VOLT_DATA_MAX_GROUP			4

/* The columns of one row of a band's battery-voltage table. */
enum jeita_volt_para_ele {
	BUCKCHG_VOLTAGE_TH = 0,
	BUCKCHG_CURRENT_MAX,
	BUCKCHG_VOLTAGE_PARA_MAX,
};

/**
 * struct mca_buckchg_jeita_volt_data - one step of a band's voltage table
 * @voltage:     the battery voltage this step covers up to, in millivolts
 * @max_current: what the cell may take up to that voltage, in milliamps
 */
struct mca_buckchg_jeita_volt_data {
	int voltage;
	int max_current;
};

/**
 * struct mca_buckchg_jeita_volt_para - a band's battery-voltage table
 * @size:      how many steps it has
 * @volt_data: the steps, lowest voltage first
 */
struct mca_buckchg_jeita_volt_para {
	int size;
	struct mca_buckchg_jeita_volt_data *volt_data;
};

/* The most bands a table may give. */
#define JEITA_DATA_MAX_GROUP				15

/* The columns of one row of a jeita table, in the order the property gives. */
enum jeita_temp_para_ele {
	JEITA_TEMP_PARA_TEMP_LOW = 0,
	JEITA_TEMP_PARA_TEMP_HIGH,
	JEITA_TEMP_PARA_LOW_TEMP_HYS,
	JEITA_TEMP_PARA_HIGH_TEMP_HYS,
	JEITA_TEMP_PARA_MAX_CURRENT,
	JEITA_TEMP_PARA_VTERM,
	JEITA_TEMP_PARA_ITERM,
	JEITA_TEMP_PARA_VOLT_PARA_NAME,
	JEITA_TEMP_PARA_MAX,
};

/**
 * struct mca_buckchg_jeita_data - one temperature band
 * @temp_low:      the band starts here, in degrees
 * @temp_high:     and ends below here
 * @low_temp_hys:  how far below @temp_low the cell must cool to leave it
 * @high_temp_hys: how far above @temp_high it must warm to leave it
 * @max_current:   what the band allows, in milliamps
 * @vterm:         the float voltage it allows, in millivolts
 * @iterm:         the current charging finishes at, in milliamps
 * @volt_para:     a further limit by battery voltage, empty if there is none
 */
struct mca_buckchg_jeita_data {
	int temp_low;
	int temp_high;
	int low_temp_hys;
	int high_temp_hys;
	int max_current;
	int vterm;
	int iterm;
	struct mca_buckchg_jeita_volt_para volt_para;
};

/**
 * struct mca_buckchg_jeita_para - a cell's bands
 * @size:           how many normal bands there are
 * @fcc_size:       how many fast-charge bands there are
 * @jeita_data:     the bands used when not fast charging
 * @jeita_ffc_data: the bands used when fast charging
 */
struct mca_buckchg_jeita_para {
	int size;
	int fcc_size;
	struct mca_buckchg_jeita_data *jeita_data;
	struct mca_buckchg_jeita_data *jeita_ffc_data;
};

/**
 * struct mca_buckchg_jeita_proc_data - where one cell's limits stand
 * @cur_jeita_index: the band in force, -1 before the first pass
 * @max_chg_curr:    the current the last pass came to, in milliamps
 */
struct mca_buckchg_jeita_proc_data {
	int cur_jeita_index;
	int max_chg_curr;
};

/**
 * struct mca_buck_jeita_smartchg_data - what smart charging has asked for
 * @delta_fv: lower the float voltage by this many millivolts
 */
struct mca_buck_jeita_smartchg_data {
	int delta_fv;
};

/**
 * struct mca_buckchg_jeita_dev - this module's state
 * @dev:                        this device
 * @online:                     whether an adapter is connected
 * @voter_ok:                   whether every election was found
 * @dtpt_status:                the battery is under a temperature test
 * @has_gbl_batt_para:          global units read their bands from their own node
 * @has_tmp_batt_para:          a test fixture's cell reads its bands from its own node
 * @vbat_high_hyst:             margin for a voltage step (read, not used)
 * @vbat_low_hyst:              margin for a voltage step
 * @vbat_low_cold_hyst:         margin for a voltage step in the cold bands
 * @base_vbat_low_hyst:         margin for a voltage step of a foldable's base cell
 * @flip_vbat_low_hyst:         the same, for its flip cell
 * @support_base_flip:          the board has a base and a flip cell
 * @base_flip_same:             both cells share the main cell's term current
 * @monitor_work:               looks at the temperature again every few seconds
 * @flip_fcc_voter:             what the flip cell is allowed
 * @en_voter:                   whether charging happens at all
 * @fcc_voter:                  what the charger is allowed
 * @vterm_voter:                what float voltage it charges to
 * @iterm_voter:                what current it finishes at
 * @proc_data:                  where the main limits stand
 * @base_proc_data:             where the base cell's stand
 * @flip_proc_data:             where the flip cell's stand
 * @jeita_para:                 the main bands
 * @base_jeita_para:            the base cell's bands
 * @flip_jeita_para:            the flip cell's bands
 * @smartchg_data:              what smart charging has asked for
 * @baacfg_update:              the bands were just rewritten, skip hysteresis
 * @vterm:                      the float voltage last voted for a normal band
 * @jeita_hot_termination_hyst: how far below a hot band's float voltage
 *                              charging stops
 */
struct mca_buckchg_jeita_dev {
	struct device *dev;
	int online;
	int voter_ok;
	int dtpt_status;
	int has_gbl_batt_para;
	int has_tmp_batt_para;
	int vbat_high_hyst;
	int vbat_low_hyst;
	int vbat_low_cold_hyst;
	int base_vbat_low_hyst;
	int flip_vbat_low_hyst;
	int support_base_flip;
	int base_flip_same;
	struct delayed_work monitor_work;
	struct mca_votable *flip_fcc_voter;
	struct mca_votable *en_voter;
	struct mca_votable *fcc_voter;
	struct mca_votable *vterm_voter;
	struct mca_votable *iterm_voter;
	struct mca_buckchg_jeita_proc_data proc_data;
	struct mca_buckchg_jeita_proc_data base_proc_data;
	struct mca_buckchg_jeita_proc_data flip_proc_data;
	struct mca_buckchg_jeita_para jeita_para;
	struct mca_buckchg_jeita_para base_jeita_para;
	struct mca_buckchg_jeita_para flip_jeita_para;
	struct mca_buck_jeita_smartchg_data smartchg_data;
	bool baacfg_update;
	int vterm;
	int jeita_hot_termination_hyst;
};

#endif /* __MCA_BUCKCHG_JEITA_H__ */
