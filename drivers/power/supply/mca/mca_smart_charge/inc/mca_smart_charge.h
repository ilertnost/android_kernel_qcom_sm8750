/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mca_smart_charge.h
 *
 * smart charge driver
 *
 * Copyright (c) 2023-2023 Xiaomi Technologies Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#ifndef __SMART_CHARGE_H__
#define __SMART_CHARGE_H__

#include <linux/cdev.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <mca/smartchg/smart_chg_class.h>

/*
 * Which "reached the cap" reports have already been sent this session.  The
 * bits sit where the modes sit in the control word.
 */
union SMART_CHG_MIEVENT {
	u32 AsUINT32;
	struct {
		u32 : 1;
		u32 navigation : 1;
		u32 outdoor : 1;
		u32 lowfast : 1;
		u32 endurance_pro : 1;
		u32 wls_super_chg : 1;
		u32 sense_chg : 1;
		u32 wls_quiet : 1;
		u32 extreme_cold : 1;
		u32 travel_wait : 1;
	};
};

/* How hard userspace is told it may charge for the current scene and posture. */
enum smart_chg_sic_mode {
	SMART_CHG_SIC_MODE_BALANCED = 0,
	SMART_CHG_SIC_MODE_SLIGHTCHG = 2,
	SMART_CHG_SIC_MODE_MIDDLE = 4,
	SMART_CHG_SIC_MODE_SUPERCHG = 8,
	SMART_CHG_SIC_MODE_MODE_MAX_INDEX,
};

/* Up to @cc_max battery cycles, charge @delta_ichg less. */
struct ICHG_CC_CFG {
	int cc_max;
	int delta_ichg;
};

#define CC_ICHG_MAX_GROUP 2

#define SMART_BYPASS_TEMP_SECTION_MAX 10
#define SMART_BYPASS_DEBOUNCE_NS (10 * NSEC_PER_SEC)
#define SMART_CHG_BOARD_TEMP_SCALE 100

/*
 * One board temperature band of the bypass charging current table, in tenths
 * of a degree.  The band is [temp_low, temp_high); leaving it for the band
 * above needs hyst_high more, for the band below hyst_low less.
 */
struct smart_bypass_temp_section {
	int temp_low;
	int temp_high;
	int hyst_low;
	int hyst_high;
	int fcc;
};

#define SMART_CHG_MISHOW_SOC_LIMIT	80
/* The hardware platform versions that can be configured as a shop display. */
#define SMART_CHG_MISHOW_PROJECT_LOW	2
#define SMART_CHG_MISHOW_PROJECT_HIGH	4

struct smart_charge_info {
	struct class *smart_charge_class;
	struct cdev pri_dev;
	struct device *sys_dev;
	struct device *dev;
	dev_t dev_num;
	struct delayed_work smart_charge_work;
	struct delayed_work smart_sense_chg_work;
	struct delayed_work smart_soc_limit_work;
	struct mca_votable *smartchg_delta_fv_voter;
	struct mca_votable *smartchg_delta_ichg_voter;
	struct mca_votable *smartchg_set_fcc_voter;
	struct notifier_block panel_nb;
	struct notifier_block thermal_nb;

	int delta_fv;
	int delta_ichg;
	int soc_limit;
	int online;
	int cycle_count;
	int wls_online;
	int cell_type;
	int smart_sic_mode;
	int screen_state;
	int scene;
	int fake_scene;
	int board_temp;
	int enable_fv_dec_by_cc;

	int smart_night_val;
	int smart_batt_val;
	int smart_delta_ichg;

	bool support_cc_ichg;
	bool support_sensechg_v2;
	bool support_sensechg_v3;
	bool soc_limit_enable;
	struct ICHG_CC_CFG ichg_cc_table[CC_ICHG_MAX_GROUP];

	/* Smart bypass charging */
	int support_bypass;
	int bypass_active;
	int bypass_temp_index;
	int last_bypass_fcc;
	u32 last_bypass_state;
	int bypass_entry_soc;
	int bypass_high_num;
	int bypass_med_num;
	int bypass_low_num;
	struct smart_bypass_temp_section
		bypass_high_lmt[SMART_BYPASS_TEMP_SECTION_MAX];
	struct smart_bypass_temp_section
		bypass_med_lmt[SMART_BYPASS_TEMP_SECTION_MAX];
	struct smart_bypass_temp_section
		bypass_low_lmt[SMART_BYPASS_TEMP_SECTION_MAX];
	ktime_t bypass_start_time;
	bool bypass_exit_flag;

	/* Smart Charging Engine */
	union SMART_CHG_HEADER smart_chg_control;
	struct SMART_CHG_INFO smart_chg_data;

	/* Buffer userspace hands battery aging data in through the cdev. */
	struct mutex mmap_lock;
	char *mmap_addr;
	int map_flag;
	int open_cnt;

	int pulse_mode;
	int support_csd;
	int plugin_rsoc;
	int night_enable_rsoc;
	union SMART_CHG_MIEVENT ignore_upload;
	int mishow_config;
};

enum smartchg_attr_list {
	MCA_PROP_SMARTCHG,
	MCA_PROP_SMARTCHG_FV,
	MCA_PROP_SMARTCHG_ICHG,
	MCA_PROP_SMARTBATT,
	MCA_PROP_SMARTNIGHT,
	MCA_PROP_POSTURE,
	MCA_PROP_SCENE,
	MCA_PROP_BOARD_TEMP,
	MCA_PROP_SMART_SIC_MODE,
};

enum smartchg_cell_type {
	MCA_CELL_TYPE_1S = 1,
	MCA_CELL_TYPE_2S,
	MCA_CELL_TYPE_MAX,
};

enum smartchg_cc_ichg_cfg_mode_ele {
	SMARTCHG_MODE_CYCLECOUNT,
	SMARTCHG_MODE_DELTA_ICHG,
	SMARTCHG_MODE_PARA_MAX,
};

#endif /* __SMART_CHARGE_H__ */
