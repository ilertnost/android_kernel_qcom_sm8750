// SPDX-License-Identifier: GPL-2.0
/*
 * mca_battery_feature.c
 *
 * mca battery feature driver
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
#define MCA_LOG_TAG "mca_battery_shutdown"
#endif

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>
#include <linux/platform_device.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_hwid.h>
#include <linux/hwid.h>
#include <mca/common/mca_parse_dts.h>

#include <mca/strategy/strategy_fg_class.h>
#include <mca/platform/platform_fg_ic_ops.h>
#include "inc/strategy_fg.h"

#define BATT_SHUTDOWN_NAME_RETRY 20
#define BATT_SHUTDOWN_NAME_RETRY_MS 100
#define BATT_SHUTDOWN_NVT_NAME "3XM31"

#define MCA_BATTERY_SHUTDOWN_VCUTTOFF_FW 3050
#define MCA_BATTERY_SHUTDOWN_VCUTTOFF_SHUTDOWN_DELAY 3050
#define MCA_BATTERY_SHUTDOWN_VCUTTOFF_SW 2900
/* Table values at or under these are taken as unset. */
#define MCA_BATTERY_SHUTDOWN_VCUTTOFF_MIN 2500
#define MCA_BATTERY_SHUTDOWN_VCUTTOFF_SW_MIN 2500
/* A detachable base pack is never cut off above this. */
#define MCA_BATTERY_SHUTDOWN_VCUTTOFF_SW_BASE_FLIP 2900
#define ROLLBACK_SHUTDOWN_VOLT_MAX 3000
#define MCA_BATTERY_SHUTDOWN_VOLTAGE_HYST 50
#define MCA_HEAVY_LOAD_DISCHARGING_CURRENT 2000000
#define MCA_HEAVY_LOAD_DISCHARGING_COUNT 2
#define MCA_VPACK_LOW_SHUTDOWN_MV 3000
#define MCA_VPACK_LOW_SHUTDOWN_COUNT 10

enum shutdown_strategy_type {
	SHUTDOWN_STRATEGY_BATTERY_HEALTH,
	SHUTDOWN_STRATEGY_BATTERY_SPEC,
	SHUTDOWN_STRATEGY_MAX,
};

static noinline int mca_battery_shutdown_parse_vcutoff_para(
	struct device_node *node, const char *name,
	struct mca_battery_shutdown_vcutoff_para_info *vcutoff_info)
{
	struct mca_battery_shutdown_vcutoff_para *vcutoff_para;
	const char *tmp_string = NULL;
	int array_len, row, col, i;
	int *field;

	if (!strcmp(name, "null")) {
		mca_log_info("no need parse volt para\n");
		return 0;
	}

	array_len = mca_parse_dts_count_strings(node, name,
						VCUTOFF_PARA_MAX_GROUP,
						MCA_BATTERY_SHUTDOWN_VCUTOFF_PARA_MAX);
	if (array_len < 0) {
		mca_log_err("parse %s failed\n", name);
		return -1;
	}

	vcutoff_info->vcutoff_para_size =
		array_len / MCA_BATTERY_SHUTDOWN_VCUTOFF_PARA_MAX;
	vcutoff_para = kcalloc(vcutoff_info->vcutoff_para_size,
			       sizeof(*vcutoff_para), GFP_KERNEL);
	vcutoff_info->vcutoff_para = vcutoff_para;
	if (!vcutoff_para) {
		mca_log_err("vcutoff para no mem\n");
		return -ENOMEM;
	}

	for (i = 0; i < array_len; i++) {
		if (mca_parse_dts_string_index(node, name, i, &tmp_string))
			goto error;

		row = i / MCA_BATTERY_SHUTDOWN_VCUTOFF_PARA_MAX;
		col = i % MCA_BATTERY_SHUTDOWN_VCUTOFF_PARA_MAX;
		switch (col) {
		case MCA_BATTERY_SHUTDOWN_RANGE_UPPER:
			field = &vcutoff_para[row].range_upper;
			break;
		case MCA_BATTERY_SHUTDOWN_VCUTOFF_FOR_FW:
			field = &vcutoff_para[row].vcutoff_fw;
			break;
		case MCA_BATTERY_SHUTDOWN_VCUTOFF_FOR_SHUTDOWN_DELAY:
			field = &vcutoff_para[row].vcutoff_shutdown_delay;
			break;
		default:
			field = &vcutoff_para[row].vcutoff_sw;
			break;
		}
		if (kstrtoint(tmp_string, 10, field))
			goto error;
	}

	return 0;

error:
	/* Nothing may be left pointing at a table that is gone. */
	kfree(vcutoff_para);
	vcutoff_info->vcutoff_para = NULL;
	vcutoff_info->vcutoff_para_size = 0;
	return -1;
}

/*
 * Read one temperature-banded cut-off table: rows of (low, high, low
 * hysteresis, high hysteresis, name of the per-band table).
 */
static int mca_battery_shutdown_parse_temp_para(
	struct device_node *node, const char *name, bool is_gl,
	struct mca_battery_shutdown_para_info *para_info)
{
	struct mca_battery_shutdown_temp_para_info *temp_info;
	const char *tmp_string = NULL;
	int array_len, row, col, i;
	int *field;

	array_len = mca_parse_dts_count_strings(node, name,
						BATTERY_TEMP_PARA_MAX_GROUP,
						MCA_BATTERY_SHUTDOWN_TEMP_PARA_MAX);
	if (array_len < 0)
		return -EINVAL;

	para_info->temp_para_size = array_len / MCA_BATTERY_SHUTDOWN_TEMP_PARA_MAX;
	temp_info = kcalloc(para_info->temp_para_size, sizeof(*temp_info),
			    GFP_KERNEL);
	para_info->temp_info = temp_info;
	if (!temp_info)
		return -ENOMEM;

	for (i = 0; i < array_len; i++) {
		if (mca_parse_dts_string_index(node, name, i, &tmp_string)) {
			if (is_gl && name[0] == 'c')
				mca_log_err("gl cc config is not ready, check pls!\n");
			else if (is_gl)
				mca_log_err("gl dod config is not ready, check pls!\n");
			goto error;
		}

		row = i / MCA_BATTERY_SHUTDOWN_TEMP_PARA_MAX;
		col = i % MCA_BATTERY_SHUTDOWN_TEMP_PARA_MAX;
		switch (col) {
		case MCA_BATTERY_SHUTDOWN_TEMP_LOW:
			field = &temp_info[row].temp_para.temp_low;
			break;
		case MCA_BATTERY_SHUTDOWN_TEMP_HIGH:
			field = &temp_info[row].temp_para.temp_high;
			break;
		case MCA_BATTERY_SHUTDOWN_LOW_TEMP_HYS:
			field = &temp_info[row].temp_para.low_temp_hysteresis;
			break;
		case MCA_BATTERY_SHUTDOWN_HIGH_TEMP_HYS:
			field = &temp_info[row].temp_para.high_temp_hysteresis;
			break;
		default:
			if (mca_battery_shutdown_parse_vcutoff_para(
				    node, tmp_string, &temp_info[row].vcutoff_info))
				goto error;
			continue;
		}
		if (kstrtoint(tmp_string, 10, field))
			goto error;
	}

	return 0;

error:
	for (row = 0; row < para_info->temp_para_size; row++)
		kfree(temp_info[row].vcutoff_info.vcutoff_para);
	kfree(temp_info);
	para_info->temp_info = NULL;
	para_info->temp_para_size = 0;
	return -1;
}

int mca_battery_shutdown_parse_dt(void *data)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;
	struct device_node *node = fg->dev->of_node;
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	bool is_gl_unit = hwid && hwid->country_version != CountryCN;
	const char *dev_name = NULL;
	const char *name;
	bool use_cc_gl, use_dod_gl;
	int rc, retry;

	rc = mca_parse_dts_u32(node, "vcutoff_shutdown_delay",
			       &fg->vcutoff_shutdown_delay,
			       MCA_BATTERY_SHUTDOWN_VCUTTOFF_SHUTDOWN_DELAY);
	rc |= mca_parse_dts_u32(node, "vcutoff_sw", &fg->vcutoff_sw,
				MCA_BATTERY_SHUTDOWN_VCUTTOFF_SW);
	rc |= mca_parse_dts_u32(node, "vcutoff_fw", &fg->vcutoff_fw,
				MCA_BATTERY_SHUTDOWN_VCUTTOFF_FW);
	if (rc) {
		mca_log_err(" normal vcutoff parse failed ret=%d\n", rc);
		return rc;
	}

	fg->support_cc_vcutoff =
		of_property_read_bool(node, "support-cc-vcutoff");
	fg->support_cc_vcutoff_gl =
		of_property_read_bool(node, "support-cc-vcutoff-global");
	use_cc_gl = is_gl_unit && fg->support_cc_vcutoff_gl;
	fg->notsupport_dod_vcutoff_gl =
		of_property_read_bool(node, "notsupport-dod-vcutoff-global");
	if (is_gl_unit && fg->notsupport_dod_vcutoff_gl)
		fg->support_cc_vcutoff = false;

	/*
	 * One Chinese cell vendor's pack has tables of its own.  The board says
	 * the pack may be one, and the gauge's own name says whether it is.
	 */
	fg->is_nvt_unique_scheme =
		of_property_read_bool(node, "support-cc-unique-cn-nvt");
	if (fg->is_nvt_unique_scheme) {
		if (is_gl_unit) {
			fg->is_nvt_unique_scheme = false;
			mca_log_err("GL device no need check\n");
		} else {
			for (retry = 1; retry <= BATT_SHUTDOWN_NAME_RETRY;
			     retry++) {
				platform_fg_ops_get_device_name(FG_IC_MASTER,
								&dev_name);
				msleep(BATT_SHUTDOWN_NAME_RETRY_MS);
				mca_log_err("retry get device name:%d\n", retry);
				if (dev_name)
					break;
			}
			if (!dev_name) {
				mca_log_err("get device name  fail\n");
				return -1;
			}
			mca_log_err("CN device name: %s\n", dev_name);
			if (strcmp(BATT_SHUTDOWN_NVT_NAME, dev_name))
				fg->is_nvt_unique_scheme = false;
		}
	}
	mca_log_err("is nvt unique scheme %d\n", fg->is_nvt_unique_scheme);

	if (fg->support_cc_vcutoff) {
		if (use_cc_gl)
			name = "cc_vcutoff_cfg_gl";
		else if (fg->is_nvt_unique_scheme)
			name = "cc_vcutoff_cfg_cn_nvt";
		else
			name = "cc_vcutoff_cfg";
		rc = mca_battery_shutdown_parse_temp_para(node, name, use_cc_gl,
							  &fg->batt_health_para);
		if (rc == -ENOMEM) {
			mca_log_err("vcutoff_para_info->temp_para no mem\n");
			return rc;
		}
		if (rc) {
			mca_log_err("parse cc_vcutoff_cfg failed\n");
			return -1;
		}
	}

	fg->support_dod_vcutoff =
		of_property_read_bool(node, "support-dod-vcutoff");
	fg->support_dod_vcutoff_gl =
		of_property_read_bool(node, "support-dod-vcutoff-global");
	use_dod_gl = is_gl_unit && fg->support_dod_vcutoff_gl;
	if (is_gl_unit && fg->notsupport_dod_vcutoff_gl)
		fg->support_dod_vcutoff = false;
	if (fg->support_dod_vcutoff) {
		name = use_dod_gl ? "dod_vcutoff_cfg_gl" : "dod_vcutoff_cfg";
		if (use_dod_gl)
			mca_log_err("parse dod_vcutoff_cfg-gl, %d\n",
				    mca_parse_dts_count_strings(node, name,
					BATTERY_TEMP_PARA_MAX_GROUP,
					MCA_BATTERY_SHUTDOWN_TEMP_PARA_MAX));
		rc = mca_battery_shutdown_parse_temp_para(node, name, use_dod_gl,
							  &fg->batt_spec_para);
		if (rc == -ENOMEM) {
			mca_log_err("batt_para_info->temp_para no mem\n");
			return rc;
		}
		if (rc) {
			mca_log_err("parse dod_vcutoff_cfg failed\n");
			return -1;
		}

		if (use_dod_gl)
			name = "weight-global";
		else if (fg->is_nvt_unique_scheme)
			name = "weight_cn_nvt";
		else
			name = "weight";
		mca_parse_dts_u32_array(node, name, fg->weight, WEIGHT_LEVEL_MAX);
	}

	fg->support_vpack_low_shutdown =
		of_property_read_bool(node, "support-vpack-low-shutdown");
	fg->disable_rollback_shutdown_volt =
		of_property_read_bool(node, "disable-rollback-shutdown-volt");

	return 0;
}

/*
 * Pick the temperature band, moving to a neighbour only once the reading is
 * past the current band's edge by its hysteresis.  Returns the band index,
 * or -1 when the temperature is in none.
 */
static int mca_battery_shutdown_select_temp(
	struct mca_battery_shutdown_para_info *para, int temp)
{
	struct mca_battery_shutdown_temp_para *cur;
	int i;

	for (i = 0; i < para->temp_para_size; i++) {
		if (temp < para->temp_info[i].temp_para.temp_high &&
		    temp >= para->temp_info[i].temp_para.temp_low)
			break;
	}
	if (i >= para->temp_para_size)
		return -1;

	cur = &para->temp_info[para->cur_temp_index].temp_para;
	if (i > para->cur_temp_index) {
		if (temp >= cur->temp_high + cur->high_temp_hysteresis)
			para->cur_temp_index = i;
	} else if (i < para->cur_temp_index) {
		if (temp < cur->temp_low - cur->low_temp_hysteresis)
			para->cur_temp_index = i;
	}

	return i;
}

int mca_battery_shutdown_update_vcutoff_para(void *data)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;
	int vcutoff_fw[SHUTDOWN_STRATEGY_MAX] = { 0 };
	int vcutoff_shutdown_delay[SHUTDOWN_STRATEGY_MAX] = { 0 };
	int vcutoff_sw[SHUTDOWN_STRATEGY_MAX] = { 0 };
	struct mca_battery_shutdown_vcutoff_para_info *vcutoff_info;
	struct mca_battery_shutdown_vcutoff_para *para;
	int temp = fg->batt_temperature / 10;
	int cur_temp_index, i, index, sw;

	if (!fg->support_cc_vcutoff && !fg->support_dod_vcutoff)
		return -1;

	/* By cycle count: the cell's health. */
	if (fg->support_cc_vcutoff) {
		cur_temp_index = mca_battery_shutdown_select_temp(
			&fg->batt_health_para, temp);
		if (cur_temp_index < 0) {
			mca_log_err("temp over range invalid\n");
			return -1;
		}

		vcutoff_info = &fg->batt_health_para
				.temp_info[fg->batt_health_para.cur_temp_index]
				.vcutoff_info;
		for (i = 0; i < vcutoff_info->vcutoff_para_size; i++) {
			if (fg->batt_cyclecount <=
			    vcutoff_info->vcutoff_para[i].range_upper)
				break;
		}
		if (i >= vcutoff_info->vcutoff_para_size) {
			mca_log_err("cyclecount over range invalid\n");
			return -1;
		}

		para = &vcutoff_info->vcutoff_para[i];
		vcutoff_fw[SHUTDOWN_STRATEGY_BATTERY_HEALTH] = para->vcutoff_fw;
		vcutoff_shutdown_delay[SHUTDOWN_STRATEGY_BATTERY_HEALTH] =
			para->vcutoff_shutdown_delay;
		vcutoff_sw[SHUTDOWN_STRATEGY_BATTERY_HEALTH] = para->vcutoff_sw;
		mca_log_info("BatteryHealth:cur_temp_index[%d], last_temp_index[%d] cur_vcuttoff_index[%d]\n",
			     cur_temp_index, fg->batt_health_para.cur_temp_index,
			     i);
	}

	/* By depth of discharge: the cell's specification. */
	if (fg->support_dod_vcutoff) {
		for (i = 0; i < fg->batt_spec_para.temp_para_size; i++)
			mca_log_err("temp_para_size:%d, temp: %d, high: %d, low: %d\n",
				    fg->batt_spec_para.temp_para_size, temp,
				    fg->batt_spec_para.temp_info[i].temp_para.temp_high,
				    fg->batt_spec_para.temp_info[i].temp_para.temp_low);

		cur_temp_index = mca_battery_shutdown_select_temp(
			&fg->batt_spec_para, temp);
		if (cur_temp_index < 0) {
			mca_log_err("temp over range invalid, current:%d\n",
				    cur_temp_index);
			return -1;
		}

		vcutoff_info = &fg->batt_spec_para
				.temp_info[fg->batt_spec_para.cur_temp_index]
				.vcutoff_info;
		for (i = 0; i < vcutoff_info->vcutoff_para_size; i++) {
			if (fg->dod_count <
			    vcutoff_info->vcutoff_para[i].range_upper)
				break;
		}
		if (i >= vcutoff_info->vcutoff_para_size) {
			mca_log_err("dod_count over range invalid\n");
			return -1;
		}

		para = &vcutoff_info->vcutoff_para[i];
		vcutoff_fw[SHUTDOWN_STRATEGY_BATTERY_SPEC] = para->vcutoff_fw;
		vcutoff_shutdown_delay[SHUTDOWN_STRATEGY_BATTERY_SPEC] =
			para->vcutoff_shutdown_delay;
		vcutoff_sw[SHUTDOWN_STRATEGY_BATTERY_SPEC] = para->vcutoff_sw;
		mca_log_info("BatterySpec:cur_temp_index[%d], last_temp_index[%d] cur_vcuttoff_index[%d]\n",
			     cur_temp_index, fg->batt_spec_para.cur_temp_index,
			     i);
	}

	/* The stricter of the two, judged by the shutdown voltage. */
	for (i = 0; i < SHUTDOWN_STRATEGY_MAX; i++)
		mca_log_info("Max-Strategy[%d], vcutoff_fw %d, vcutoff_shutdown_delay %d vcutoff_sw %d\n",
			     i, vcutoff_fw[i], vcutoff_shutdown_delay[i],
			     vcutoff_sw[i]);
	index = vcutoff_shutdown_delay[SHUTDOWN_STRATEGY_BATTERY_SPEC] >
		max(vcutoff_shutdown_delay[SHUTDOWN_STRATEGY_BATTERY_HEALTH], 0) ?
		SHUTDOWN_STRATEGY_BATTERY_SPEC : SHUTDOWN_STRATEGY_BATTERY_HEALTH;

	if (!fg->disable_rollback_shutdown_volt && fg->enable_rollback &&
	    vcutoff_shutdown_delay[index] < ROLLBACK_SHUTDOWN_VOLT_MAX) {
		fg->vcutoff_fw = MCA_BATTERY_SHUTDOWN_VCUTTOFF_FW;
		fg->vcutoff_shutdown_delay =
			MCA_BATTERY_SHUTDOWN_VCUTTOFF_SHUTDOWN_DELAY;
		fg->vcutoff_sw = MCA_BATTERY_SHUTDOWN_VCUTTOFF_SW;
		mca_log_info("[Enable_Rollback][%d] vcutoff_fw[%d], vcutoff_shutdown_delay[%d], vcutoff_sw[%d]\n",
			     index, fg->vcutoff_fw, fg->vcutoff_shutdown_delay,
			     fg->vcutoff_sw);
		return 0;
	}

	fg->vcutoff_fw = vcutoff_fw[index] > MCA_BATTERY_SHUTDOWN_VCUTTOFF_MIN ?
			 vcutoff_fw[index] : MCA_BATTERY_SHUTDOWN_VCUTTOFF_FW;
	fg->vcutoff_shutdown_delay =
		vcutoff_shutdown_delay[index] > MCA_BATTERY_SHUTDOWN_VCUTTOFF_MIN ?
		vcutoff_shutdown_delay[index] :
		MCA_BATTERY_SHUTDOWN_VCUTTOFF_SHUTDOWN_DELAY;
	sw = vcutoff_sw[index];
	if (fg->cfg.support_base_flip)
		fg->vcutoff_sw = min(sw, MCA_BATTERY_SHUTDOWN_VCUTTOFF_SW_BASE_FLIP);
	else
		fg->vcutoff_sw = sw >= MCA_BATTERY_SHUTDOWN_VCUTTOFF_SW_MIN ?
				 sw : MCA_BATTERY_SHUTDOWN_VCUTTOFF_SW;
	mca_log_info("Strategy[%d]:vcutoff_fw[%d], vcutoff_shutdown_delay[%d], vcutoff_sw[%d]\n",
		     index, fg->vcutoff_fw, fg->vcutoff_shutdown_delay,
		     fg->vcutoff_sw);

	return 0;
}

static bool mca_battery_shutdown_heavy_load_check(struct strategy_fg *fg)
{
	static int heavy_load_count;

	if (fg->batt_voltage_mean < fg->vcutoff_shutdown_delay +
				    MCA_BATTERY_SHUTDOWN_VOLTAGE_HYST &&
	    fg->batt_current > MCA_HEAVY_LOAD_DISCHARGING_CURRENT)
		heavy_load_count++;
	else
		heavy_load_count = 0;

	mca_log_info("heavy_load_count: %d\n", heavy_load_count);

	return heavy_load_count > MCA_HEAVY_LOAD_DISCHARGING_COUNT;
}

/*
 * With the gauge gone the pack voltage the charger measures is all there is
 * to go on, so ten low readings in a row with the gauge not answering shut
 * the phone down.
 */
static bool mca_shutdown_due_to_vpack_low_of_vpack_inside_fg_absent(
	struct strategy_fg *fg)
{
	static int count;
	int vpack_vol_now_mv = fg->pack_vbat;
	int chip_ok;

	chip_ok = strategy_class_fg_is_chip_ok();
	mca_log_info("chip_ok:%d vpack_vol_now_mv:%d", chip_ok,
		     vpack_vol_now_mv);
	if (!chip_ok && vpack_vol_now_mv <= MCA_VPACK_LOW_SHUTDOWN_MV &&
	    count < MCA_VPACK_LOW_SHUTDOWN_COUNT)
		count++;
	else
		count = 0;

	return count >= MCA_VPACK_LOW_SHUTDOWN_COUNT;
}

int mca_battery_shutdown_check_shutdown_delay(void *data, int *ui_soc)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;
	static bool shutdown_dealy, last_shutdown_delay;
	union power_supply_propval pval = { 0 };
	struct mca_event_notify_data n_data = { 0 };
	char event[MCA_EVENT_NOTIFY_SIZE] = { 0 };
	int charging_status = 0;
	int soc = *ui_soc;
	bool heavy_load_off;

	if (fg->batt_psy) {
		power_supply_get_property(fg->batt_psy,
					  POWER_SUPPLY_PROP_STATUS, &pval);
		charging_status = pval.intval;
	}

	if (fg->support_vpack_low_shutdown &&
	    mca_shutdown_due_to_vpack_low_of_vpack_inside_fg_absent(fg) &&
	    charging_status != POWER_SUPPLY_STATUS_CHARGING) {
		shutdown_dealy = true;
		soc = 1;
		goto report_shutdown;
	}

	if (soc > 1) {
		shutdown_dealy = false;
		goto report_shutdown;
	}

	/* Held at 1% until the voltage says the pack is really done. */
	heavy_load_off = mca_battery_shutdown_heavy_load_check(fg);
	shutdown_dealy = (fg->batt_voltage_mean <= fg->vcutoff_shutdown_delay ||
			  heavy_load_off) &&
			 charging_status != POWER_SUPPLY_STATUS_CHARGING;
	soc = 1;

report_shutdown:
	mca_log_info("ui_soc: %d, soc: %d, last_shutdown_delay: %d, shutdown_dealy: %d\n",
		     *ui_soc, soc, last_shutdown_delay, shutdown_dealy);

	*ui_soc = soc;

	if (last_shutdown_delay != shutdown_dealy &&
	    fg->fake_soc == STRATEGY_FG_FAKE_SOC_NONE) {
		mca_log_err("shutdown_dealy changed: %d => %d\n",
			    last_shutdown_delay, shutdown_dealy);
		n_data.event_len = snprintf(event, MCA_EVENT_NOTIFY_SIZE,
					    "POWER_SUPPLY_SHUTDOWN_DELAY=%u\n",
					    shutdown_dealy);
		n_data.event = event;
		mca_event_report_uevent(&n_data);
		last_shutdown_delay = shutdown_dealy;
	}

	return 0;
}
