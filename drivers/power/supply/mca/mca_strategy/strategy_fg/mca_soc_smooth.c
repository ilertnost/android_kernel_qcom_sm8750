// SPDX-License-Identifier: GPL-2.0
/*
 *mca_soc_smooth.c
 *
 * mca soc smooth driver
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
#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_soc_smooth"
#endif

#include <mca/common/mca_log.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <mca/strategy/strategy_fg_class.h>
#include <mca/common/mca_parse_dts.h>
#include "inc/strategy_fg.h"
#include "inc/mca_soc_smooth.h"
#include <mca/protocol/protocol_class.h>
#include <linux/minmax.h>
#include <mca/smartchg/smart_chg_class.h>

#define EU_SOC_PROPORTION_C 100
#define EU_SOC_PROPORTION 101

#define SOC_KEEP_TIME_DEFAULT_S 10
#define SOC_KEEP_TIME_SMOOTH_FULL_S 30
#define SOC_KEEP_TIME_MIN_S 10
#define SOC_KEEP_TIME_MAX_S 500
#define PLUGIN_HOLD_UP_S 15
#define PLUGIN_HOLD_DOWN_S 30
#define PLUGIN_HOLD_DOWN_SDP_S 70

int strategy_soc_smooth_parse_dt(struct strategy_fg *fg)
{
	struct device_node *node = fg->dev->of_node;
	int ret, len;

	mca_parse_dts_u32(node, "suppot_RSOC_0_smooth",
			  &fg->smooth.suppot_RSOC_0_smooth, 0);
	if (!fg->smooth.suppot_RSOC_0_smooth)
		return 0;

	ret = mca_parse_dts_u32_array(node, "smooth_curr_unit",
				      fg->smooth.smooth_curr_unit,
				      CURR_SMOOTH_LEN);
	if (ret < 0)
		return -1;

	ret = mca_parse_dts_u32_array(node, "smooth_vol_offset",
				      fg->smooth.smooth_vol_offset,
				      VOL_SMOOTH_LEN);
	if (ret < 0)
		return -1;

	ret = mca_parse_dts_u32_array(node, "smooth_temp_unit",
				      fg->smooth.smooth_temp_unit,
				      TEMP_SMOOTH_LEN);
	if (ret < 0)
		return -1;

	len = mca_parse_dts_u32_count(node, "dischg_smooth_low_temp_unit",
				      VOL_SMOOTH_LEN, CURR_SMOOTH_LEN);
	if (len < 0) {
		mca_log_err("parse dischg_smooth_low_temp_unit failed\n");
		return -1;
	}
	ret = mca_parse_dts_u32_array(node, "dischg_smooth_low_temp_unit",
				      &fg->smooth.dischg_smooth_low_temp_unit[0][0],
				      len);
	if (ret < 0)
		return -1;

	len = mca_parse_dts_u32_count(node, "dischg_smooth_high_temp_unit",
				      VOL_SMOOTH_LEN, CURR_SMOOTH_LEN);
	if (len < 0) {
		mca_log_err("parse dischg_smooth_high_temp_unit failed\n");
		return -1;
	}
	ret = mca_parse_dts_u32_array(node, "dischg_smooth_high_temp_unit",
				      &fg->smooth.dischg_smooth_high_temp_unit[0][0],
				      len);
	if (ret < 0)
		return -1;

	return 0;
}

/*
 * With the raw reading at 0% the displayed value is walked down at a pace
 * that depends on how far the voltage is above the shutdown point and how
 * hard the pack is being discharged.
 */
static void get_unit_time_low_soc(struct strategy_fg *fg, int *soc_keep_time)
{
	int (*unit)[CURR_SMOOTH_LEN];
	int unit_time = 0;
	int i, j;

	if (fg->batt_temperature < fg->smooth.smooth_temp_unit[0] ||
	    fg->batt_temperature < fg->smooth.smooth_temp_unit[1])
		unit = fg->smooth.dischg_smooth_low_temp_unit;
	else
		unit = fg->smooth.dischg_smooth_high_temp_unit;

	for (i = VOL_SMOOTH_LEN - 1; i >= 0 && !unit_time; i--) {
		if (fg->batt_voltage_mean <= fg->vcutoff_shutdown_delay +
					     fg->smooth.smooth_vol_offset[i])
			continue;
		for (j = CURR_SMOOTH_LEN - 1; j >= 0; j--) {
			if (fg->batt_current_mean >
			    fg->smooth.smooth_curr_unit[j]) {
				unit_time = unit[i][j];
				break;
			}
		}
	}

	if (unit_time >= SOC_KEEP_TIME_MIN_S && unit_time <= SOC_KEEP_TIME_MAX_S)
		*soc_keep_time = unit_time;
	else
		mca_log_info("data error unit_time:%d\n", unit_time);
}

static void fg_update_mapped_soc(struct strategy_fg *fg)
{
	int raw_soc = fg->batt_raw_soc;
	int mapped_soc, mapped_soc_decimal;
	int num;

	if (!fg->is_eu_model) {
		if (raw_soc >= fg->cfg.report_full_raw_soc)
			goto full;
		num = raw_soc + fg->cfg.soc_proportion_c;
		mapped_soc = fg->cfg.soc_proportion ?
			     num / fg->cfg.soc_proportion : 0;
		mapped_soc_decimal = num - mapped_soc * fg->cfg.soc_proportion;
		if (mapped_soc > 99)
			mapped_soc = 99;
		goto out;
	}

	if (fg->charging_done) {
		if (fg->batt_rsoc == 100)
			goto full;
		if (fg->en_smooth_full) {
			if (fg->batt_ui_soc == 100)
				fg->en_smooth_full = false;
			goto full;
		}
	}

	if (fg->lossless_recharge && fg->batt_rsoc == 100)
		goto full;

	num = raw_soc + EU_SOC_PROPORTION_C;
	if (fg->chg_status == POWER_SUPPLY_STATUS_CHARGING) {
		mapped_soc = num / EU_SOC_PROPORTION;
		mapped_soc_decimal = num % EU_SOC_PROPORTION;
		if (mapped_soc > 99)
			mapped_soc = 99;
	} else {
		mapped_soc = fg->batt_rsoc;
		mapped_soc_decimal = num % EU_SOC_PROPORTION;
	}
	goto out;

full:
	mapped_soc = 100;
	mapped_soc_decimal = 0;
out:
	fg->batt_mapped_soc = mapped_soc;
	fg->batt_mapped_soc_decimal = mapped_soc_decimal;

	mca_log_info("is_eu_model[%d], en_smooth_full[%d], mapped_soc[%d]\n",
		     fg->is_eu_model, fg->en_smooth_full, mapped_soc);
}

/*
 * The checks a smoothed value goes through before it is shown.  The low
 * voltage shutdown decision is the last word, so it comes after the others.
 */
static int fg_ui_soc_process_special_conditions(struct strategy_fg *fg,
						int last_ui_soc, int ui_soc)
{
	time64_t time_now = ktime_get_boottime_seconds();
	int down_update_duration;
	int soc_limit;

	soc_limit = mca_smartchg_get_limit_soc();
	if (ui_soc > last_ui_soc) {
		if (!fg->power_present ||
		    fg->chg_status != POWER_SUPPLY_STATUS_CHARGING ||
		    (soc_limit > 0 && last_ui_soc >= soc_limit)) {
			mca_log_info("WARNING: keep ui_soc constant by: power_present=%d, chg_status=%d, soc_limit=%d\n",
				     fg->power_present, fg->chg_status,
				     soc_limit);
			ui_soc = last_ui_soc;
		}
	} else if (ui_soc < last_ui_soc && fg->batt_current_mean < 0) {
		mca_log_info("WARNING: keep ui_soc constant by: batt_current_mean=%d\n",
			     fg->batt_current_mean);
		ui_soc = last_ui_soc;
	}

	if (fg->chg_status == POWER_SUPPLY_STATUS_CHARGING) {
		down_update_duration = fg->real_type == XM_CHARGER_TYPE_SDP ?
				       PLUGIN_HOLD_DOWN_SDP_S :
				       PLUGIN_HOLD_DOWN_S;
		if ((ui_soc > last_ui_soc &&
		     time_now - fg->plugin_time < PLUGIN_HOLD_UP_S) ||
		    (ui_soc < last_ui_soc &&
		     time_now - fg->plugin_time < down_update_duration)) {
			mca_log_info("WARNING: keep ui_soc constant by: plugin_time [down_update_duration][%d]\n",
				     down_update_duration);
			ui_soc = last_ui_soc;
		}
	}

	if (last_ui_soc == 100 && fg->keep_full_flag &&
	    fg->batt_raw_soc >
		    fg->cfg.report_full_raw_soc - FG_HOLD_UISOC_100_GAP) {
		mca_log_info("WARNING: set ui_soc to 100 by: keep_full_flag\n");
		ui_soc = 100;
	}

	if (last_ui_soc == 100 && fg->is_eu_model &&
	    fg->batt_raw_soc > FG_RAW_SOC_FULL - FG_HOLD_UISOC_100_GAP &&
	    fg->chg_status == POWER_SUPPLY_STATUS_FULL) {
		mca_log_info("WARNING: set ui_soc to 100 by: POWER_SUPPLY_STATUS_FULL\n");
		ui_soc = 100;
	}

	if (fg->vbatt_empty) {
		mca_log_info("WARNING: set ui_soc to 0 by: vbatt_empty\n");
		return 0;
	}

	mca_battery_shutdown_check_shutdown_delay(fg, &ui_soc);

	return clamp(ui_soc, 0, 100);
}

static void fg_ui_soc_update_decimal(struct strategy_fg *fg, int last_ui_soc,
				     int ui_soc)
{
	if (ui_soc == last_ui_soc && ui_soc < fg->batt_mapped_soc)
		fg->batt_ui_soc_decimal = 99;
	else if (ui_soc == last_ui_soc && ui_soc > fg->batt_mapped_soc)
		fg->batt_ui_soc_decimal = 0;
	else
		fg->batt_ui_soc_decimal = fg->batt_mapped_soc_decimal;
}

int fg_ui_soc_smooth(struct strategy_fg *fg, bool prohibit_jump)
{
	static int last_ui_soc = -1;
	static time64_t time_last;
	time64_t time_now = ktime_get_boottime_seconds();
	int soc_keep_time = SOC_KEEP_TIME_DEFAULT_S;
	int soc_delta, soc_changed, delta_time;
	int ui_soc;

	fg_update_mapped_soc(fg);
	ui_soc = fg->batt_mapped_soc;

	/* The first reading is shown as it is. */
	if (last_ui_soc == -1) {
		time_last = time_now;
		last_ui_soc = ui_soc > 0 ? ui_soc : fg->batt_rsoc;
		return last_ui_soc;
	}

	if (fg->en_smooth_full) {
		soc_keep_time = SOC_KEEP_TIME_SMOOTH_FULL_S;
		mca_log_info("start en_smooth_full soc increase per 30s \n");
	}

	/* A jump is walked one percent per soc_keep_time seconds. */
	soc_delta = abs(ui_soc - last_ui_soc);
	if (soc_delta >= 1 && prohibit_jump) {
		if (fg->smooth.suppot_RSOC_0_smooth && fg->batt_raw_soc == 0 &&
		    fg->batt_current > 0)
			get_unit_time_low_soc(fg, &soc_keep_time);
		delta_time = (time_now - time_last) / soc_keep_time;
		soc_changed = delta_time > 0 ? 1 : 0;
		if (ui_soc > last_ui_soc)
			ui_soc = last_ui_soc + soc_changed;
		else
			ui_soc = last_ui_soc - soc_changed;

		mca_log_info("ui_soc: %d, last_ui_soc: %d, soc_delta: %d, soc_changed: %d, batt_mapped_soc:%d, is_eu_model=%d, soc_keep_time:%d\n",
			     ui_soc, last_ui_soc, soc_delta, soc_changed,
			     fg->batt_mapped_soc, fg->is_eu_model,
			     soc_keep_time);
	}

	if (prohibit_jump) {
		ui_soc = fg_ui_soc_process_special_conditions(fg, last_ui_soc,
							      ui_soc);
		fg_ui_soc_update_decimal(fg, last_ui_soc, ui_soc);
	}

	mca_log_info("ui_soc: %d => %d, time_now: %lld prohibit_jump:%d\n",
		     last_ui_soc, ui_soc, time_now, prohibit_jump);
	if (ui_soc != last_ui_soc) {
		if (last_ui_soc == 100)
			fg->keep_full_flag = false;
		time_last = time_now;
		last_ui_soc = ui_soc;
	}

	return ui_soc;
}

MODULE_DESCRIPTION("Strategy Fuel Gauge Smooth");
MODULE_AUTHOR("liweiwei9@xiaomi.com");
MODULE_LICENSE("GPL v2");
