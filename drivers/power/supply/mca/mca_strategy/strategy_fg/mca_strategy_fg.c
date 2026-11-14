// SPDX-License-Identifier: GPL-2.0
/*
 *mca_strategy_fg.c
 *
 * mca fuelgauge strategy driver
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
#define MCA_LOG_TAG "mca_strategy_fg"
#endif

#include <linux/device.h>
#include <linux/hwid.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>
#include <mca/common/mca_charge_mievent.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_hwid.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/common/mca_smem.h>
#include <mca/common/mca_sysfs.h>
#include <mca/common/mca_voter.h>
#include <mca/common/mca_workqueue.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/platform/platform_fg_ic_ops.h>
#include <mca/platform/platform_loadsw_class.h>
#include <mca/protocol/protocol_class.h>
#include <mca/protocol/protocol_pd_class.h>
#include <mca/shared_memory/charger_partition_class.h>
#include <mca/smartchg/smart_chg_class.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include "inc/strategy_fg.h"
#include "inc/mca_soc_smooth.h"

/* Monitor periods, in ms. */
#define STRATEGY_FG_WORK_INTERVAL_FAST 1000
#define STRATEGY_FG_WORK_INTERVAL_NORMAL 5000
#define STRATEGY_FG_WORK_INTERVAL_SCREEN_ON 10000
#define STRATEGY_FG_WORK_INTERVAL_SCREEN_OFF 30000
#define STRATEGY_FG_WORK_INTERVAL_LOW_VOLT 5000
#define STRATEGY_FG_WORK_INTERVAL_LOW_TEMP 5000
#define STRATEGY_FG_WORK_INTERVAL_LOW_TEMP_FAST 2000
#define STRATEGY_FG_WORK_INTERVAL_DTPT 5000
#define STRATEGY_FG_WORK_INTERVAL_PROBE 1000
#define STRATEGY_FG_RESUME_UPDATE_TIME_MS 3000
#define STRATEGY_FG_RESET_FULL_FLAG_MS 60000
#define STRATEGY_FG_ABNORMAL_DFX_DELAY_MS 70000
#define STRATEGY_FG_FORCE_FULL_RECHECK_MS 8000
#define STRATEGY_FG_FL4P0_CALIBRATION_MS 5000

/* DT defaults. */
#define STRATEGY_FG_REPORT_FULL_RSOC 9700
#define STRATEGY_FG_SOC_PROPORTION 97
#define STRATEGY_FG_SOC_PROPORTION_C 96
#define STRATEGY_FG_COOL_THRESHOLD 150
#define STRATEGY_FG_FORCE_FULL_HYS 150
#define STRATEGY_FG_HIGH_TEMP_VTERM 4400
#define STRATEGY_FG_VTERM_DELTA 7
#define STRATEGY_FG_FAST_FORCE_FULL_VOL_DELTA 15
#define DEFAULT_DESIGN_CAPACITY 5000

/* What the strategies are told while the gauge cannot be read. */
#define STRATEGY_FG_ERROR_FAKE_SOC 15
#define STRATEGY_FG_ERROR_FAKE_TEMP 250
#define STRATEGY_FG_ERROR_FAKE_CURR -500000
#define STRATEGY_FG_ERROR_FAKE_VOLT 3700
#define STRATEGY_FG_UPDATE_PERIOD_NONE -1
#define STRATEGY_FG_SELF_EQUAL_MAX_COUNT 15

/* Used while the voters are not up yet. */
#define STRATEGY_FG_DEFAULT_VTERM 4400
#define STRATEGY_FG_DEFAULT_ITERM 250

#define BATT_TEMP_COLD_TH 0
#define BATT_LOW_VOLT_HY 200
#define BATT_LOW_VOLT_SW_HY 50
#define VBATT_EMPTY_HY 100
#define VBATT_EMPTY_VALID_MV 1000
#define DISCHARGING_MAX_EMPTY_COUNT 2
#define FAST_CHARGING_MAX_EMPTY_COUNT 10
#define ZERO_SPEED_WINDOW_S 120

#define EU_RECHARGER_SOC 95
#define EU_LOSSES_START_CYCLECOUNT 300
#define FG_FORCE_REPORT_FULL_TIMES 2

#define NVT1000_OTA_UPDATE_FLAG 0xAA5555AA
#define NVT1000_OTA_MONITOR_INTERVAL 20000
#define NVT1000_OTA_SOC_MIN 60
#define NVT1000_OTA_TEMP_MIN 100
#define NVT1000_OTA_CURR_MIN 100
#define NVT1000_OTA_CURR_MAX 1000
#define NVT1000_OTA_TRIGGER_COUNT 9

void onewire_check_auth_pass(bool *auth_pass)
{
	*auth_pass = true;
}

void onewire_check_chip_ok(bool *chip_ok)
{
	*chip_ok = true;
}

static void strategy_fg_init_voter(struct strategy_fg *fg);
static int strategy_fg_ops_get_soc_decimal(void *data, int *soc_decimal,
					   int *rate);
static void strategy_fg_get_dod_count(struct strategy_fg *fg, int *value);
static int strategy_fg_get_parallel_rsoc(struct strategy_fg *fg, int *rsoc);
static void strategy_fg_check_battery_adapt_power(struct strategy_fg *fg,
						  int *match);
static ssize_t strategy_fg_sysfs_show(struct device *dev,
				      struct device_attribute *attr, char *buf);
static ssize_t strategy_fg_sysfs_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count);

static struct mca_sysfs_attr_info strategy_fg_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(strategy_fg_sysfs, 0664, FG_PROP_MAIN_AUTH,
			  authentic),
	mca_sysfs_attr_rw(strategy_fg_sysfs, 0664, FG_PROP_SLAVE_AUTH,
			  slave_authentic),
	mca_sysfs_attr_rw(strategy_fg_sysfs, 0664, FG_PROP_BATT_INDEX,
			  verify_slave_flag),
	mca_sysfs_attr_rw(strategy_fg_sysfs, 0664, FG_PROP_VERIFY_DIGEST,
			  verify_digest),
	mca_sysfs_attr_rw(strategy_fg_sysfs, 0664, FG_PROP_BATT_POWER_MATCH,
			  batt_power_match),
	mca_sysfs_attr_ro(strategy_fg_sysfs, 0440, FG_PROP_FAST_CHARGE,
			  fast_charge),
	mca_sysfs_attr_ro(strategy_fg_sysfs, 0440, FG_PROP_SOC_DECIMAL,
			  soc_decimal),
	mca_sysfs_attr_ro(strategy_fg_sysfs, 0440, FG_PROP_SOC_DECIMAL_RATE,
			  soc_decimal_rate),
	mca_sysfs_attr_rw(strategy_fg_sysfs, 0664, FG_PROP_FAKE_SOC, fake_soc),
	mca_sysfs_attr_rw(strategy_fg_sysfs, 0664, FG_PROP_FAKE_TEMP,
			  fake_temp),
	mca_sysfs_attr_ro(strategy_fg_sysfs, 0440, FG_PROP_BATTERY_NUM,
			  battery_num),
	mca_sysfs_attr_rw(strategy_fg_sysfs, 0664, FG_PROP_UPDATE_PERIOD,
			  update_period),
	mca_sysfs_attr_ro(strategy_fg_sysfs, 0440, FG_PROP_PACK_VOLTAGE,
			  pack_vbat),
	mca_sysfs_attr_rw(strategy_fg_sysfs, 0664, FG_PROP_DOD_COUNT,
			  dod_count),
	mca_sysfs_attr_rw(strategy_fg_sysfs, 0664, FG_PROP_ENABLE_ROLLBACK,
			  enable_rollback),
	mca_sysfs_attr_ro(strategy_fg_sysfs, 0440, FG_PROP_SOH_NEW, soh_new),
	mca_sysfs_attr_ro(strategy_fg_sysfs, 0440,
			  FG_PROP_MASTER_SELF_EQUAL_COUNT,
			  master_self_equal_count),
	mca_sysfs_attr_ro(strategy_fg_sysfs, 0440,
			  FG_PROP_SLAVE_SELF_EQUAL_COUNT,
			  slave_self_equal_count),
	mca_sysfs_attr_rw(strategy_fg_sysfs, 0664,
			  FG_PROP_SELF_EQUAL_MAX_COUNT, self_equal_max_count),
	mca_sysfs_attr_ro(strategy_fg_sysfs, 0440, FG_PROP_PACK_TEMP,
			  pack_tbat),
	mca_sysfs_attr_ro(strategy_fg_sysfs, 0440, FG_PROP_RAW_SOC, raw_soc),
	mca_sysfs_attr_ro(strategy_fg_sysfs, 0440, FG_PROP_CALC_RVALUE,
			  calc_rvalue),
	mca_sysfs_attr_ro(strategy_fg_sysfs, 0440, FG_PROP_MANUFACTURING_DATE,
			  manufacturing_date),
	mca_sysfs_attr_rw(strategy_fg_sysfs, 0664, FG_PROP_FIRST_USAGE_DATE,
			  first_usage_date),
};

#define FG_AUTH_ATTRS_SIZE ARRAY_SIZE(strategy_fg_sysfs_field_tbl)

static struct attribute *strategy_fg_sysfs_attrs[FG_AUTH_ATTRS_SIZE + 1];

static const struct attribute_group strategy_fg_sysfs_attr_group = {
	.attrs = strategy_fg_sysfs_attrs,
};

static void strategy_fg_set_fake_soc(struct strategy_fg *fg, int val)
{
	mca_log_err("set fake_soc: %d\n", val);
	if (val < STRATEGY_FG_FAKE_SOC_NONE || val > 100 || val == fg->fake_soc)
		return;

	fg->fake_soc = val;
	cancel_delayed_work_sync(&fg->monitor_work);
	mca_queue_delayed_work(&fg->monitor_work, 0);
	mca_event_block_notify(MCA_EVENT_TYPE_BATTERY_INFO,
			       MCA_EVENT_BATTERY_STS_CHANGE, NULL);
}

static void strategy_fg_set_fake_temp(struct strategy_fg *fg, int val)
{
	mca_log_err("set fake_temp: %d\n", val);
	if (val == fg->fake_temp)
		return;

	fg->fake_temp = val;
	platform_fg_ops_set_temp(FG_IC_MASTER, val);
	if (fg->cfg.fg_type == MCA_FG_TYPE_PARALLEL)
		platform_fg_ops_set_temp(FG_IC_SLAVE, val);
	cancel_delayed_work_sync(&fg->monitor_work);
	mca_queue_delayed_work(&fg->monitor_work, 0);
	mca_event_block_notify(MCA_EVENT_TYPE_BATTERY_INFO,
			       MCA_EVENT_BATTERY_STS_CHANGE, NULL);
}

/*
 * Of two packs' dates, the manufacturing date shown is the older one; a pack
 * that has none is ignored.  Only the year-and-later bytes are compared.
 */
static void mca_strategy_get_manufacturing_date(struct strategy_fg *fg,
						char *date)
{
	char date_master[8] = { 0 };
	char date_slave[8] = { 0 };
	static const char zero[8];
	bool master_empty, slave_empty;
	const char *pick;
	int i;

	if (!fg->fg_init_flag)
		return;

	if (fg->cfg.fg_type == MCA_FG_TYPE_SINGLE) {
		platform_fg_ops_get_manufacturing_date(FG_IC_MASTER, (u8 *)date);
		return;
	}
	if (fg->cfg.fg_type != MCA_FG_TYPE_PARALLEL)
		return;

	platform_fg_ops_get_manufacturing_date(FG_IC_MASTER, (u8 *)date_master);
	platform_fg_ops_get_manufacturing_date(FG_IC_SLAVE, (u8 *)date_slave);
	mca_log_err("get parallel_manufacturing_date fail: fg_master is %.8s fg_slave is %.8s\n",
		    date_master, date_slave);

	master_empty = !memcmp(date_master, zero, sizeof(zero));
	slave_empty = !memcmp(date_slave, zero, sizeof(zero));
	if (master_empty && slave_empty) {
		memset(date, 0, sizeof(zero));
		return;
	}

	if (master_empty || slave_empty) {
		pick = master_empty ? date_slave : date_master;
	} else {
		pick = date_master;
		for (i = 3; i < 8; i++) {
			if (date_master[i] == date_slave[i])
				continue;
			mca_log_info(" Warning: inconsistent fg_master is %.8s fg_slave is %.8s, choose %d\n",
				     date_master, date_slave,
				     (u8)date_slave[i] <= (u8)date_master[i]);
			if ((u8)date_slave[i] <= (u8)date_master[i])
				pick = date_slave;
			break;
		}
	}
	memcpy(date, pick, sizeof(zero));
}

/*
 * Of two packs' dates the first-usage date shown is the later one; an unset
 * date reads as all nines.
 */
static void mca_strategy_get_first_usage_date(struct strategy_fg *fg,
					      char *date)
{
	char date_master[8] = { 0 };
	char date_slave[8] = { 0 };
	bool master_unset, slave_unset;
	const char *pick;

	if (!fg->fg_init_flag)
		return;

	if (fg->cfg.fg_type == MCA_FG_TYPE_SINGLE) {
		platform_fg_ops_get_first_usage_date(FG_IC_MASTER, (u8 *)date);
		return;
	}
	if (fg->cfg.fg_type != MCA_FG_TYPE_PARALLEL)
		return;

	platform_fg_ops_get_first_usage_date(FG_IC_MASTER, (u8 *)date_master);
	platform_fg_ops_get_first_usage_date(FG_IC_SLAVE, (u8 *)date_slave);

	master_unset = !memcmp(date_master, "99999999", 8);
	slave_unset = !memcmp(date_slave, "99999999", 8);
	if (master_unset && slave_unset)
		pick = date_master;
	else if (master_unset)
		pick = date_slave;
	else if (slave_unset)
		pick = date_master;
	else
		pick = memcmp(date_slave, date_master, 8) > 0 ?
		       date_slave : date_master;
	memcpy(date, pick, 8);
}

static ssize_t strategy_fg_sysfs_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct strategy_fg *info = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *attr_info;
	int match = 0;
	int val = 0;

	if (!info)
		return -1;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  strategy_fg_sysfs_field_tbl,
					  FG_AUTH_ATTRS_SIZE);
	if (!attr_info)
		return -1;

	switch (attr_info->sysfs_attr_name) {
	case FG_PROP_MAIN_AUTH:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;
		platform_fg_ops_set_authentic(FG_IC_MASTER, val);
		info->batt_auth = val;
		strategy_fg_check_battery_adapt_power(info, &match);
		if (val && info->cfg.fg_type == MCA_FG_TYPE_SINGLE && match)
			mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
					       MCA_EVENT_BATT_AUTH_PASS, NULL);
		break;
	case FG_PROP_SLAVE_AUTH:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;
		platform_fg_ops_set_authentic(FG_IC_SLAVE, val);
		info->batt_slave_auth = val;
		strategy_fg_check_battery_adapt_power(info, &match);
		if (info->batt_auth && info->batt_slave_auth && match)
			mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
					       MCA_EVENT_BATT_AUTH_PASS, NULL);
		break;
	case FG_PROP_BATT_INDEX:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;
		info->batt_index = val;
		break;
	case FG_PROP_VERIFY_DIGEST:
		platform_fg_ops_set_verify_digest(info->batt_index, (char *)buf);
		break;
	case FG_PROP_BATT_POWER_MATCH:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;
		info->fake_bap_match = val;
		mca_log_info("set fake_bap_match: %d\n", val);
		break;
	case FG_PROP_FAKE_SOC:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;
		strategy_fg_set_fake_soc(info, val);
		break;
	case FG_PROP_FAKE_TEMP:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;
		strategy_fg_set_fake_temp(info, val);
		break;
	case FG_PROP_UPDATE_PERIOD:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;
		mca_log_err("set fg update period: %d\n", val);
		if (val == info->update_period)
			break;
		info->update_period = val;
		cancel_delayed_work_sync(&info->monitor_work);
		mca_queue_delayed_work(&info->monitor_work, 0);
		mca_event_block_notify(MCA_EVENT_TYPE_BATTERY_INFO,
				       MCA_EVENT_BATTERY_STS_CHANGE, NULL);
		break;
	case FG_PROP_DOD_COUNT:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;
		mca_log_err("set fake_dod_count: %d\n", val);
		info->fake_dod_count = val;
		break;
	case FG_PROP_ENABLE_ROLLBACK:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;
		mca_log_err("set enable_rollback: %d\n", val);
		info->enable_rollback = !!val;
		break;
	case FG_PROP_SELF_EQUAL_MAX_COUNT:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;
		mca_log_err("set self_equal_count_max: %d\n", val);
		info->self_equal_max_count = val;
		break;
	case FG_PROP_FIRST_USAGE_DATE:
		if (!info->fg_init_flag)
			break;
		if (info->cfg.fg_type == MCA_FG_TYPE_SINGLE) {
			platform_fg_ops_set_first_usage_date(FG_IC_MASTER, buf);
		} else if (info->cfg.fg_type == MCA_FG_TYPE_PARALLEL) {
			platform_fg_ops_set_first_usage_date(FG_IC_MASTER, buf);
			platform_fg_ops_set_first_usage_date(FG_IC_SLAVE, buf);
		}
		break;
	default:
		break;
	}

	return count;
}

static void strategy_fg_get_soh_new(struct strategy_fg *fg, int *soh_new);
static unsigned long strategy_fg_get_calc_rvalue(struct strategy_fg *fg);

static ssize_t strategy_fg_sysfs_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct strategy_fg *info = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *attr_info;
	char date_buf[64] = { 0 };
	int val = 0, rate = 0;

	if (!info)
		return -1;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  strategy_fg_sysfs_field_tbl,
					  FG_AUTH_ATTRS_SIZE);
	if (!attr_info)
		return -1;

	switch (attr_info->sysfs_attr_name) {
	case FG_PROP_MAIN_AUTH:
		platform_fg_ops_get_authentic(FG_IC_MASTER, &val);
		return scnprintf(buf, PAGE_SIZE, "%x\n", val);
	case FG_PROP_SLAVE_AUTH:
		platform_fg_ops_get_authentic(FG_IC_SLAVE, &val);
		return scnprintf(buf, PAGE_SIZE, "%x\n", val);
	case FG_PROP_BATT_INDEX:
		return scnprintf(buf, PAGE_SIZE, "%x\n", info->batt_index);
	case FG_PROP_VERIFY_DIGEST:
		return platform_fg_ops_get_verify_digest(info->batt_index, buf);
	case FG_PROP_BATT_POWER_MATCH:
		strategy_fg_check_battery_adapt_power(info, &val);
		return scnprintf(buf, PAGE_SIZE, "%x\n", val);
	case FG_PROP_FAST_CHARGE:
		val = info->fast_charge;
		break;
	case FG_PROP_SOC_DECIMAL:
		strategy_fg_ops_get_soc_decimal(info, &val, &rate);
		break;
	case FG_PROP_SOC_DECIMAL_RATE:
		strategy_fg_ops_get_soc_decimal(info, &rate, &val);
		break;
	case FG_PROP_FAKE_SOC:
		val = info->fake_soc;
		break;
	case FG_PROP_FAKE_TEMP:
		val = info->fake_temp;
		break;
	case FG_PROP_BATTERY_NUM:
		val = info->cfg.fg_type == MCA_FG_TYPE_PARALLEL ? 1 : 0;
		break;
	case FG_PROP_UPDATE_PERIOD:
		val = info->update_period;
		break;
	case FG_PROP_PACK_VOLTAGE:
		platform_class_buckchg_ops_get_pack_vbat(MAIN_BUCK_CHARGER, &val);
		break;
	case FG_PROP_DOD_COUNT:
		strategy_fg_get_dod_count(info, &val);
		break;
	case FG_PROP_ENABLE_ROLLBACK:
		val = info->enable_rollback;
		break;
	case FG_PROP_SOH_NEW:
		strategy_fg_get_soh_new(info, &val);
		break;
	case FG_PROP_MASTER_SELF_EQUAL_COUNT:
		val = info->self_equal_count[FG_IC_MASTER];
		break;
	case FG_PROP_SLAVE_SELF_EQUAL_COUNT:
		val = info->self_equal_count[FG_IC_SLAVE];
		break;
	case FG_PROP_SELF_EQUAL_MAX_COUNT:
		val = info->self_equal_max_count;
		break;
	case FG_PROP_PACK_TEMP:
		platform_class_buckchg_ops_get_pack_tbat(MAIN_BUCK_CHARGER, &val);
		break;
	case FG_PROP_RAW_SOC:
		val = info->batt_raw_soc;
		break;
	case FG_PROP_CALC_RVALUE:
		return scnprintf(buf, PAGE_SIZE, "%lu\n",
				 strategy_fg_get_calc_rvalue(info));
	case FG_PROP_MANUFACTURING_DATE:
		mca_strategy_get_manufacturing_date(info, date_buf);
		return scnprintf(buf, PAGE_SIZE, "%s\n", date_buf);
	case FG_PROP_FIRST_USAGE_DATE:
		mca_strategy_get_first_usage_date(info, date_buf);
		return scnprintf(buf, PAGE_SIZE, "%s\n", date_buf);
	default:
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

#define MEAN_CUR_NUM 5
static void strategy_fg_record_curr_mean(struct strategy_fg *fg)
{
	static int current_nums[MEAN_CUR_NUM];
	static int cur_num;
	int sum = 0, i;

	if (!fg || !fg->fg_init_flag)
		return;

	current_nums[cur_num] = fg->batt_current;
	cur_num = (cur_num + 1) % MEAN_CUR_NUM;
	mca_log_debug("record_i:%d, record_cur [0]= %d, [1]= %d, [2]= %d, [3]= %d, [4]= %d\n",
		      cur_num, current_nums[0], current_nums[1],
		      current_nums[2], current_nums[3], current_nums[4]);

	for (i = 0; i < MEAN_CUR_NUM; i++)
		sum += current_nums[i];
	fg->batt_current_mean = sum / MEAN_CUR_NUM;
	mca_log_info("mean_cur:%d\n", fg->batt_current_mean);
}

/*
 * Average the voltage over the last five readings and decide whether the
 * pack is empty: a few readings at or under the software cut-off in a row
 * (more while fast charging, where the voltage sags under load), cleared once
 * the average is back above the cut-off with some margin.
 */
#define MEAN_VOL_NUM 5
void strategy_fg_record_volt_mean(struct strategy_fg *fg)
{
	static int voltage_nums[MEAN_VOL_NUM];
	static int vol_i, empty_count;
	static u32 is_zero_speed;
	int empty_count_th = DISCHARGING_MAX_EMPTY_COUNT;
	int sum = 0, cnt_i;
	struct timespec64 ts;

	if (!fg || !fg->fg_init_flag)
		return;
	if (fg->batt_voltage < VBATT_EMPTY_VALID_MV)
		return;

	voltage_nums[vol_i] = fg->batt_voltage;
	mca_log_debug("record_i:%d record_vol [0]= %d, [1]= %d, [2]= %d, [3]= %d, [4]= %d\n",
		      vol_i, voltage_nums[0], voltage_nums[1], voltage_nums[2],
		      voltage_nums[3], voltage_nums[4]);
	vol_i = (vol_i + 1) % MEAN_VOL_NUM;

	for (cnt_i = 0; cnt_i < MEAN_VOL_NUM; cnt_i++) {
		if (!voltage_nums[cnt_i])
			break;
		if (voltage_nums[cnt_i] > 0)
			sum += voltage_nums[cnt_i];
	}
	fg->batt_voltage_mean = cnt_i ? sum / cnt_i : 0;
	mca_log_info("mean_vol:%d\n", fg->batt_voltage_mean);

	if (fg->chg_status == POWER_SUPPLY_STATUS_CHARGING && fg->fast_charge)
		empty_count_th = FAST_CHARGING_MAX_EMPTY_COUNT;

	/* A zero-speed boot charges from a pack too low to read reliably. */
	ktime_get_boottime_ts64(&ts);
	if (!is_zero_speed && fg->chg_status == POWER_SUPPLY_STATUS_CHARGING &&
	    ts.tv_sec < ZERO_SPEED_WINDOW_S)
		get_smem_battery_info(&is_zero_speed);
	if (fg->chg_status == POWER_SUPPLY_STATUS_CHARGING && is_zero_speed &&
	    ts.tv_sec < ZERO_SPEED_WINDOW_S) {
		mca_log_err("chg_status = %d, is_zero_speed = %d, tv_sec = %u, ignore vbatt_empty...\n",
			    fg->chg_status, is_zero_speed, (u32)ts.tv_sec);
	} else if (fg->batt_voltage_mean <= fg->vcutoff_sw) {
		empty_count++;
		fg->vbatt_empty = empty_count > empty_count_th;
	} else if (fg->batt_voltage_mean > fg->vcutoff_sw + VBATT_EMPTY_HY) {
		empty_count = 0;
		fg->vbatt_empty = false;
	}

	mca_log_info("empty_count_th: %d, vbatt_empty: %d\n", empty_count_th,
		     fg->vbatt_empty);
}

static int strategy_fg_get_volt_mean(void *data, int *vol_mean)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return -1;

	*vol_mean = fg->batt_voltage_mean;
	return 0;
}

/* Depth-of-discharge count: weighted sum of the gauge's level counters. */
#define WEIGHT_SCALE 10
static int strategy_fg_calc_dod_count(struct strategy_fg *fg,
				      enum fg_ic_role role)
{
	int level1 = 0, level2 = 0, level3 = 0, lowtemp = 0;

	platform_fg_ops_get_dod_count(role);
	platform_fg_ops_get_count_level1(role, &level1);
	platform_fg_ops_get_count_level2(role, &level2);
	platform_fg_ops_get_count_level3(role, &level3);
	platform_fg_ops_get_count_lowtemp(role, &lowtemp);

	return (fg->weight[WEIGHT_LEVEL_1] * level1 +
		fg->weight[WEIGHT_LEVEL_2] * level2 +
		fg->weight[WEIGHT_LEVEL_3] * level3) / WEIGHT_SCALE;
}

static void strategy_fg_get_dod_count(struct strategy_fg *fg, int *value)
{
	int slave_count;

	if (!fg->support_dod_vcutoff)
		return;

	if (fg->fake_dod_count) {
		*value = fg->fake_dod_count;
		return;
	}

	*value = strategy_fg_calc_dod_count(fg, FG_IC_MASTER);
	if (fg->cfg.fg_type == MCA_FG_TYPE_SINGLE)
		return;

	/* The pack is only as good as its worse cell. */
	slave_count = strategy_fg_calc_dod_count(fg, FG_IC_SLAVE);
	mca_log_info("master_dod_count: %d, slave_dod_count:%d", *value,
		     slave_count);
	*value = max(*value, slave_count);
}

/*
 * A pair's health is each cell's health weighted by its design capacity.  A
 * capacity outside 1..10000 mAh means that gauge could not be read and its
 * cell is left out; if neither can be read the nominal split is used.
 */
#define SOH_DESIGN_CAP_MIN_MAH		1
#define SOH_DESIGN_CAP_MAX_MAH		10000
#define SOH_DESIGN_CAP_MASTER_DEFAULT	3920
#define SOH_DESIGN_CAP_SLAVE_DEFAULT	1235

static void strategy_fg_get_soh_new(struct strategy_fg *fg, int *soh_new)
{
	int master_ui_soh = 0, slave_ui_soh = 0;
	int master_cap = 0, slave_cap = 0;
	bool master_ok, slave_ok;

	if (fg->cfg.fg_type == MCA_FG_TYPE_SINGLE) {
		platform_fg_ops_get_ui_soh(FG_IC_MASTER, &master_ui_soh);
		*soh_new = master_ui_soh;
		return;
	}
	if (fg->cfg.fg_type != MCA_FG_TYPE_PARALLEL)
		return;

	platform_fg_ops_get_ui_soh(FG_IC_MASTER, &master_ui_soh);
	platform_fg_ops_get_ui_soh(FG_IC_SLAVE, &slave_ui_soh);
	platform_fg_ops_get_full_design(FG_IC_MASTER, &master_cap);
	platform_fg_ops_get_full_design(FG_IC_SLAVE, &slave_cap);
	master_cap /= 1000;
	slave_cap /= 1000;

	master_ok = master_cap >= SOH_DESIGN_CAP_MIN_MAH &&
		    master_cap <= SOH_DESIGN_CAP_MAX_MAH;
	slave_ok = slave_cap >= SOH_DESIGN_CAP_MIN_MAH &&
		   slave_cap <= SOH_DESIGN_CAP_MAX_MAH;
	if (!master_ok && !slave_ok) {
		master_cap = SOH_DESIGN_CAP_MASTER_DEFAULT;
		slave_cap = SOH_DESIGN_CAP_SLAVE_DEFAULT;
	} else if (!master_ok) {
		master_cap = 0;
	} else if (!slave_ok) {
		slave_cap = 0;
	}

	*soh_new = master_cap + slave_cap ?
		   (master_ui_soh * master_cap + slave_ui_soh * slave_cap) /
		   (master_cap + slave_cap) : 0;
	mca_log_info("master_cap=%d, slave_cap=%d, master_ui_soh=%d, slave_ui_soh=%d, soh_new=%d\n",
		     master_cap, slave_cap, master_ui_soh, slave_ui_soh,
		     *soh_new);
}

static unsigned long strategy_fg_get_calc_rvalue(struct strategy_fg *fg)
{
	unsigned long master_rvalue, slave_rvalue, rvalue;

	switch (fg->cfg.fg_type) {
	case MCA_FG_TYPE_SINGLE:
		return platform_fg_ops_get_calc_rvalue(FG_IC_MASTER);
	case MCA_FG_TYPE_PARALLEL:
		master_rvalue = platform_fg_ops_get_calc_rvalue(FG_IC_MASTER);
		slave_rvalue = platform_fg_ops_get_calc_rvalue(FG_IC_SLAVE);
		rvalue = max(master_rvalue, slave_rvalue);
		mca_log_info("rvalue=%lu, master_rvalue=%lu, slave_rvalue=%lu\n",
			     rvalue, master_rvalue, slave_rvalue);
		return rvalue;
	case MCA_FG_TYPE_SERIES:
		return 0;
	default:
		return -1;
	}
}

/* Keep the gauge's own cut-off in step with the one chosen here. */
static void strategy_fg_update_vcuttoff_voltage(struct strategy_fg *fg)
{
	int vcutoff_reg = 0;

	if (mca_battery_shutdown_update_vcutoff_para(fg))
		return;

	platform_fg_ops_get_cutoff_voltage(FG_IC_MASTER, &vcutoff_reg);
	if (vcutoff_reg == fg->vcutoff_fw)
		return;

	platform_fg_ops_set_cutoff_voltage(FG_IC_MASTER, fg->vcutoff_fw);
	if (fg->cfg.fg_type != MCA_FG_TYPE_SINGLE)
		platform_fg_ops_set_cutoff_voltage(FG_IC_SLAVE, fg->vcutoff_fw);
	mca_log_err("vcutoff_reg:%d, vcutoff_fw %d", vcutoff_reg,
		    fg->vcutoff_fw);
}

/*
 * In extreme-cold mode a young pack is reported colder than it reads, so
 * the strategies treat it more gently.
 */
static int strategy_fg_check_extreme_cold_temp_compensation(
	struct strategy_fg *fg, int real_temp)
{
	struct fg_extreme_cold_para *para;
	int i;

	if (!mca_smartchg_is_extreme_cold_enabled())
		return real_temp;

	for (i = 0; i < fg->cfg.extreme_cold_para_size; i++) {
		para = &fg->cfg.extreme_cold_para[i];
		if (fg->batt_cyclecount < para->cycle_count_max &&
		    real_temp < para->temp_max) {
			mca_log_err("cc: %d, real_temp: %d, compensation: %d\n",
				    fg->batt_cyclecount, real_temp,
				    para->compensation);
			return real_temp - para->compensation;
		}
	}

	return real_temp;
}

/*
 * Of two cells, the warmer one's temperature is reported once both are above
 * the cool threshold (unless that is absurdly hot); otherwise the cooler one.
 */
static int strategy_fg_pick_parallel_temp(struct strategy_fg *fg, int master,
					  int slave)
{
	if (min(master, slave) <= 1000 && master > fg->cfg.cool_threshold &&
	    slave > fg->cfg.cool_threshold)
		return max(master, slave);
	return min(master, slave);
}

static void dfx_fg_check_vbat_diff(struct strategy_fg *fg)
{
	static bool check_started;
	s64 time_now;
	int diff;
	int param[2];

	if (!check_started) {
		time_now = ktime_get_boottime_seconds();
		if (time_now < 180)
			mca_log_err("time_now: %lld\n", time_now);
		else
			check_started = true;
		return;
	}

	diff = abs(fg->master_batt_info.volt - fg->slave_batt_info.volt);
	mca_log_err("batt volt diff = %d\n", diff);
	if (diff <= 500) {
		mca_charge_mievent_set_state(MIEVENT_STATE_END,
					     CHARGE_DFX_BATTERY_VOLTAGE_DIFFER);
		return;
	}

	param[0] = fg->master_batt_info.volt;
	param[1] = fg->slave_batt_info.volt;
	mca_charge_mievent_report(CHARGE_DFX_BATTERY_VOLTAGE_DIFFER, param, 2);
}

/* Read both gauges of a pair and combine them into batt_info. */
static int strategy_fg_get_parallel_batt_info(struct strategy_fg *fg)
{
	struct fg_batt_info *m = &fg->master_batt_info;
	struct fg_batt_info *s = &fg->slave_batt_info;
	struct fg_batt_info *info;
	int ret = 0, r1, r2, i;

	r1 = platform_fg_ops_get_curr(FG_IC_SLAVE, &s->curr);
	r2 = platform_fg_ops_get_curr(FG_IC_MASTER, &m->curr);
	if (r1 || r2)
		ret = -1;
	else
		fg->batt_info.curr = m->curr + s->curr;

	r1 = platform_fg_ops_get_temp(FG_IC_SLAVE, &s->temp);
	r2 = platform_fg_ops_get_temp(FG_IC_MASTER, &m->temp);
	pr_err("%s master %d slave %d\n", "strategy_fg_get_parallel_temp",
	       m->temp, s->temp);
	if (r1 || r2)
		ret = -1;
	else
		fg->batt_info.temp = strategy_fg_pick_parallel_temp(fg, m->temp,
								    s->temp);

	r1 = platform_fg_ops_get_original_temp(FG_IC_SLAVE, &s->original_temp);
	r2 = platform_fg_ops_get_original_temp(FG_IC_MASTER, &m->original_temp);
	pr_err("%s master %d slave %d\n",
	       "strategy_fg_get_parallel_original_temp", m->original_temp,
	       s->original_temp);
	if (r1 || r2)
		ret = -1;
	else
		fg->batt_info.original_temp = strategy_fg_pick_parallel_temp(
			fg, m->original_temp, s->original_temp);

	ret |= strategy_fg_get_parallel_rsoc(fg, &fg->batt_info.rsoc);

	platform_fg_ops_get_volt(FG_IC_MASTER, &m->volt);
	if (platform_fg_ops_get_volt(FG_IC_SLAVE, &s->volt))
		ret = -1;
	else
		fg->batt_info.volt = max(m->volt, s->volt);

	r1 = platform_fg_ops_get_cyclecount(FG_IC_SLAVE, &s->cycle_count);
	r2 = platform_fg_ops_get_cyclecount(FG_IC_MASTER, &m->cycle_count);
	if (r1 || r2)
		ret = -1;
	else
		fg->batt_info.cycle_count = max(m->cycle_count, s->cycle_count);

	r1 = platform_fg_ops_get_soh(FG_IC_SLAVE, &s->soh);
	r2 = platform_fg_ops_get_soh(FG_IC_MASTER, &m->soh);
	if (r1 || r2)
		ret = -1;
	else
		fg->batt_info.soh = max(m->soh, s->soh);

	r1 = platform_fg_ops_get_rm(FG_IC_SLAVE, &s->rm);
	r2 = platform_fg_ops_get_rm(FG_IC_MASTER, &m->rm);
	if (r1 || r2)
		ret = -1;
	else
		fg->batt_info.rm = m->rm + s->rm;

	r1 = platform_fg_ops_get_fcc(FG_IC_SLAVE, &s->fcc);
	r2 = platform_fg_ops_get_fcc(FG_IC_MASTER, &m->fcc);
	if (r1 || r2)
		ret = -1;
	else
		fg->batt_info.fcc = m->fcc + s->fcc;

	for (i = 0; i <= fg->cfg.fg_type; i++) {
		info = i ? s : m;
		mca_log_err("fg_index[%d], CURRENT:%d RSOC:%d VOLTAGE:%d TEMP:%d ORIGINAL_TEMP:%d\n",
			    i, info->curr, info->rsoc, info->volt, info->temp,
			    info->original_temp);
		mca_log_info("fg_index[%d], CYCLECOUNT:%d S:%d RM:%d F:%d\n", i,
			     info->cycle_count, info->soh, info->rm, info->fcc);
	}

	return ret;
}

static void strategy_fg_report_battery_status_changed(struct strategy_fg *fg)
{
	static int last_soc, last_temp;
	int batt_temp = fg->batt_temperature / 10;

	if (fg->batt_ui_soc == last_soc && batt_temp == last_temp)
		return;

	mca_log_err("ui_soc: %d => %d, temp: %d => %d\n", last_soc,
		    fg->batt_ui_soc, last_temp, batt_temp);
	last_soc = fg->batt_ui_soc;
	last_temp = batt_temp;
	mca_event_block_notify(MCA_EVENT_TYPE_BATTERY_INFO,
			       MCA_EVENT_BATTERY_STS_CHANGE, NULL);
}

/*
 * Read everything the gauge knows.  On a gauge error the strategies are given
 * fixed safe values instead.
 */
static int mca_strategy_update_fg_info(struct strategy_fg *fg)
{
	static bool master_i2c_error;
	int ffc = 0, slave_ffc = 0;
	bool err_master = false, err_slave = false;
	int temp, fcc, ret;

	switch (fg->cfg.fg_type) {
	case MCA_FG_TYPE_SINGLE:
		platform_fg_ops_get_error_state(FG_IC_MASTER, &fg->fg_error);
		break;
	case MCA_FG_TYPE_PARALLEL:
		ret = platform_fg_ops_get_error_state(FG_IC_MASTER, &err_master);
		ret |= platform_fg_ops_get_error_state(FG_IC_SLAVE, &err_slave);
		pr_err("%s master %d slave %d\n",
		       "strategy_fg_get_parallel_error_state", err_master,
		       err_slave);
		if (!ret) {
			fg->dual_error[FG_IC_MASTER] = err_master;
			fg->dual_error[FG_IC_SLAVE] = err_slave;
			fg->fg_error = err_master || err_slave;
		}
		break;
	default:
		break;
	}

	if (fg->fg_error) {
		/*
		 * A base pack whose master gauge dropped off the bus is put
		 * back to normal power once both answer again.
		 */
		if ((fg->cfg.support_base_flip || fg->cfg.base_flip_same) &&
		    fg->dual_error[FG_IC_MASTER] && !fg->dual_error[FG_IC_SLAVE])
			master_i2c_error = true;

		mca_charge_mievent_report(CHARGE_DFX_FG_IIC_ERR,
					  &fg->batt_ui_soc, 1);
		fg->batt_ui_soc = STRATEGY_FG_ERROR_FAKE_SOC;
		fg->batt_temperature = STRATEGY_FG_ERROR_FAKE_TEMP;
		strategy_fg_report_battery_status_changed(fg);
		return -1;
	}

	if (master_i2c_error && fg->power_present) {
		mca_log_err("last master i2c error and slave i2c is ok, now dual i2c is ok, so set exit lowpower \n");
		platform_class_loadsw_set_lowpower_mode(LOADSW_ROLE_MASTER, false);
		master_i2c_error = false;
	}

	if (!fg->fg_init_flag)
		return -1;

	switch (fg->cfg.fg_type) {
	case MCA_FG_TYPE_SINGLE:
		ret = platform_fg_ops_get_batt_info(FG_IC_MASTER, &fg->batt_info);
		if (ret)
			return ret;
		break;
	case MCA_FG_TYPE_PARALLEL:
		ret = strategy_fg_get_parallel_batt_info(fg);
		if (ret)
			return ret;
		dfx_fg_check_vbat_diff(fg);
		break;
	case MCA_FG_TYPE_SERIES:
		break;
	default:
		return -1;
	}

	/* Every hundred cycles: tell the DFX log and clear the gauge's counters. */
	if (fg->cyclecount_hundred != fg->batt_cyclecount / 100 &&
	    fg->batt_cyclecount % 100 == 0) {
		fg->cyclecount_hundred = fg->batt_cyclecount / 100;
		mca_charge_mievent_report(CHARGE_DFX_BATTERY_CYCLECOUNT,
					  &fg->batt_cyclecount, 1);
		if (fg->cfg.clear_CC_per_hundred) {
			mca_log_err("clear_CC_per_hundred: %d\n",
				    fg->cyclecount_hundred);
			if (fg->fg_init_flag &&
			    fg->cfg.fg_type == MCA_FG_TYPE_SINGLE)
				platform_fg_ops_set_clear_count_data(FG_IC_MASTER);
		}
	}

	fg->batt_current = fg->batt_info.curr;
	fg->batt_voltage = fg->batt_info.volt;
	temp = strategy_fg_check_extreme_cold_temp_compensation(fg,
							fg->batt_info.temp);
	fg->batt_temperature = temp;
	fg->batt_rsoc = fg->batt_info.rsoc;
	fg->original_temp = fg->batt_info.original_temp;
	fg->batt_cyclecount = fg->batt_info.cycle_count;
	fg->batt_rm = fg->batt_info.rm;
	fg->batt_fcc = fg->batt_info.fcc;
	fg->batt_soh = fg->batt_info.soh;

	if (fg->cfg.fg_type == MCA_FG_TYPE_PARALLEL) {
		int master_fcc = fg->master_batt_info.fcc;
		int slave_fcc = fg->slave_batt_info.fcc;
		int master_rate, master_raw_soc, slave_raw_soc;

		master_rate = master_fcc + slave_fcc ?
			      master_fcc * 100 / (master_fcc + slave_fcc) : 0;
		fcc = slave_fcc / 1000;
		slave_raw_soc = fcc ? fg->slave_batt_info.rm * 10 / fcc : 0;
		fcc = master_fcc / 1000;
		master_raw_soc = fcc ? fg->master_batt_info.rm * 10 / fcc : 0;
		fg->batt_raw_soc = (slave_raw_soc * (100 - master_rate) +
				    master_raw_soc * master_rate + 50) / 100;
		mca_log_info("master_rate:%d, slave_rate:%d, master_raw_soc:%d, slave_raw_soc:%d\n",
			     master_rate, 100 - master_rate, master_raw_soc,
			     slave_raw_soc);
	} else {
		fcc = fg->batt_info.fcc / 1000;
		fg->batt_raw_soc = fcc ?
				   (fg->batt_info.rm / 1000) * 10000 / fcc : 0;
	}

	strategy_fg_get_dod_count(fg, &fg->dod_count);

	if (fg->fg_init_flag) {
		platform_fg_ops_get_fastcharge(FG_IC_MASTER, &ffc);
		if (fg->cfg.fg_type != MCA_FG_TYPE_SINGLE)
			platform_fg_ops_get_fastcharge(FG_IC_SLAVE, &slave_ffc);
		fg->fast_charge = ffc &&
				  (fg->cfg.fg_type == MCA_FG_TYPE_SINGLE ||
				   slave_ffc);
	} else {
		fg->fast_charge = false;
	}

	return 0;
}

#define BATTERY_TEMP_HOT_THRESHOLD 550
#define BATTERY_TEMP_COLD_THRESHOLD -100

enum mievent_battery_temp_abnormal_ele {
	MIEVENT_BATTERY_TEMP_PARAM_TBAT,
	MIEVENT_BATTERY_TEMP_PARAM_TBAT_LIMIT,
	MIEVENT_BATTERY_TEMP_PARAM_IS_CHARGING,
	MIEVENT_BATTERY_TEMP_PARAM_TBOARD,
	MIEVENT_BATTERY_TEMP_PARAM_MAX,
};

static void fg_abnormal_temp_notify_mievent(struct strategy_fg *fg)
{
	int param[MIEVENT_BATTERY_TEMP_PARAM_MAX] = { 0 };

	if (fg->batt_temperature <= BATTERY_TEMP_HOT_THRESHOLD &&
	    fg->batt_temperature >= BATTERY_TEMP_COLD_THRESHOLD) {
		mca_log_info("battery temp normal ignore");
		return;
	}

	param[MIEVENT_BATTERY_TEMP_PARAM_TBAT] = fg->batt_temperature;
	param[MIEVENT_BATTERY_TEMP_PARAM_IS_CHARGING] =
		fg->chg_status == POWER_SUPPLY_STATUS_CHARGING;
	param[MIEVENT_BATTERY_TEMP_PARAM_TBOARD] = fg->thermal_board_temp;
	if (fg->batt_temperature < BATTERY_TEMP_COLD_THRESHOLD) {
		platform_fg_ops_get_temp_min(FG_IC_MASTER,
			&param[MIEVENT_BATTERY_TEMP_PARAM_TBAT_LIMIT]);
		mca_charge_mievent_report(CHARGE_DFX_BATTERY_TEMP_COLD, param,
					  MIEVENT_BATTERY_TEMP_PARAM_MAX);
	} else {
		platform_fg_ops_get_temp_max(FG_IC_MASTER,
			&param[MIEVENT_BATTERY_TEMP_PARAM_TBAT_LIMIT]);
		mca_charge_mievent_report(CHARGE_DFX_BATTERY_TEMP_HOT, param,
					  MIEVENT_BATTERY_TEMP_PARAM_MAX);
	}
}

/* Five minutes after boot, report once if the pack never authenticated. */
#define DFS_CHECK_BATT_AUTH_TIME_SEC 300
static noinline void fg_check_batt_auth(struct strategy_fg *fg)
{
	static bool checked;
	time64_t time_now;
	int fg_index;

	if (checked)
		return;

	time_now = ktime_get_boottime_seconds();
	if (time_now <= DFS_CHECK_BATT_AUTH_TIME_SEC)
		return;

	mca_log_err("check batt_auth: %d, time_now: %lld\n", fg->batt_auth,
		    time_now);
	checked = true;

	if (fg->cfg.fg_type != MCA_FG_TYPE_PARALLEL) {
		if (!fg->batt_auth)
			mca_charge_mievent_report(CHARGE_DFX_BATTERY_AUTH_FAIL,
						  NULL, 0);
		return;
	}

	if (!fg->batt_auth) {
		fg_index = FG_IC_MASTER;
		mca_charge_mievent_report(CHARGE_DFX_DUAL_BATTERY_AUTH_FAIL,
					  &fg_index, 1);
	}
	mca_log_err("check slave batt auth: %d, time_now: %lld\n",
		    fg->batt_slave_auth, time_now);
	if (!fg->batt_slave_auth) {
		fg_index = FG_IC_SLAVE;
		mca_charge_mievent_report(CHARGE_DFX_DUAL_BATTERY_AUTH_FAIL,
					  &fg_index, 1);
	}
}

#define BATT_OVERHEAT_THRESHOLD 580
#define BATT_WARM_THRESHOLD 480
#define BATT_COOL_THRESHOLD 150
#define BATT_COLD_THRESHOLD 0

static void fg_update_batt_health(struct strategy_fg *fg)
{
	int temp = fg->batt_temperature;
	int health;

	if (temp >= BATT_OVERHEAT_THRESHOLD)
		health = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (temp >= BATT_WARM_THRESHOLD)
		health = POWER_SUPPLY_HEALTH_WARM;
	else if (temp >= BATT_COOL_THRESHOLD)
		health = POWER_SUPPLY_HEALTH_GOOD;
	else if (temp >= BATT_COLD_THRESHOLD)
		health = POWER_SUPPLY_HEALTH_COOL;
	else
		health = POWER_SUPPLY_HEALTH_COLD;

	if (health != fg->batt_health) {
		fg->batt_health = health;
		mca_event_block_notify(MCA_EVENT_CHARGE_STATUS,
				       MCA_EVENT_BATTERY_HEALTH_CHANGE, NULL);
	}
}

/*
 * Boards with a software constant-voltage loop are told when the cell has
 * reached within 10 mV of the float voltage on a wired charge that is still
 * delivering.
 */
#define BATT_OV_MARGIN_MV 10
static void strategy_fg_report_batt_ov(struct strategy_fg *fg)
{
	int chg_status = 0;

	if (!fg->cfg.wr_for_sw_cv_wake || fg->wls_online)
		return;

	if (!fg->input_suspend_voter)
		fg->input_suspend_voter = mca_find_votable("input_suspend");
	if (fg->input_suspend_voter &&
	    mca_get_client_vote(fg->input_suspend_voter, "wire_qc"))
		return;

	platform_class_buckchg_ops_get_chg_status(MAIN_BUCK_CHARGER,
						  &chg_status);
	if (chg_status > MCA_BATT_CHGR_STATUS_TAPER &&
	    chg_status != MCA_BATT_CHGR_STATUS_FAST_LINEAR)
		return;

	if (fg->batt_voltage <
	    mca_get_effective_result(fg->vterm_voter) - BATT_OV_MARGIN_MV)
		return;

	mca_log_err("batt_voltage: %d, fg_ov_voltage: %d\n", fg->batt_voltage,
		    mca_get_effective_result(fg->vterm_voter) -
		    BATT_OV_MARGIN_MV);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				  MCA_EVENT_BUCKCHG_BATT_OV, 1);
}

static int fg_update_status(struct strategy_fg *fg, bool prohibit_jump)
{
	static bool last_fg_error;
	union power_supply_propval pval = { 0 };
	int ret;

	if (fg->batt_psy) {
		power_supply_get_property(fg->batt_psy,
					  POWER_SUPPLY_PROP_STATUS, &pval);
		fg->chg_status = pval.intval;
	} else {
		fg->batt_psy = power_supply_get_by_name("battery");
	}

	ret = mca_strategy_update_fg_info(fg);

	/* The missing-battery monitor tells a dead gauge from a pulled pack. */
	if (fg->fg_error != last_fg_error) {
		mca_log_err("fg_error:%d\n", fg->fg_error);
		last_fg_error = fg->fg_error;
		mca_strategy_func_process(STRATEGY_FUNC_TYPE_BMD,
					  MCA_EVENT_BMD_STSTUS_CHANGE,
					  fg->fg_error);
	}

	if (ret || fg->fg_error) {
		mca_log_err("get batt info fail\n");
		return -1;
	}

	fg_update_batt_health(fg);
	strategy_fg_record_volt_mean(fg);
	strategy_fg_record_curr_mean(fg);
	strategy_fg_update_vcuttoff_voltage(fg);

	if (fg->cfg.fg_type == MCA_FG_TYPE_SINGLE) {
		platform_fg_ops_update_fw(FG_IC_MASTER);
	} else if (fg->cfg.fg_type == MCA_FG_TYPE_PARALLEL) {
		platform_fg_ops_update_fw(FG_IC_MASTER);
		platform_fg_ops_update_fw(FG_IC_SLAVE);
	}

	strategy_fg_report_batt_ov(fg);

	fg->batt_ui_soc = fg_ui_soc_smooth(fg, prohibit_jump);
	strategy_fg_report_battery_status_changed(fg);

	platform_class_buckchg_ops_get_pack_vbat(MAIN_BUCK_CHARGER,
						 &fg->pack_vbat);
	platform_class_buckchg_ops_get_pack_ibat(MAIN_BUCK_CHARGER,
						 &fg->pack_ibat);
	platform_class_buckchg_ops_get_pack_tbat(MAIN_BUCK_CHARGER,
						 &fg->pack_tbat);

	fg_abnormal_temp_notify_mievent(fg);

	mca_log_err("UISOC:%d CURRENT:%d RSOC:%d VOLTAGE:%d TEMP:%d PVBAT:%d PIBAT:%d PTBAT:%d HEALTH:%d\n",
		    fg->batt_ui_soc, fg->batt_current, fg->batt_rsoc,
		    fg->batt_voltage, fg->batt_temperature, fg->pack_vbat,
		    fg->pack_ibat, fg->pack_tbat, fg->batt_health);
	mca_log_info("CC:%d S:%d RM:%d F:%d RAWSOC:%d, DODCOUNT:%d\n",
		     fg->batt_cyclecount, fg->batt_soh, fg->batt_rm,
		     fg->batt_fcc, fg->batt_raw_soc, fg->dod_count);

	return 0;
}

void strategy_fg_update_batt_info(struct strategy_fg *fg)
{
	int dc_master = 0, dc_slave = 0;
	int ret, r1, r2;

	if (fg->cfg.design_capacity > 0) {
		fg->batt_dc = fg->cfg.design_capacity * 1000;
	} else {
		ret = platform_fg_ops_get_full_design(FG_IC_MASTER, &dc_master);
		if (fg->cfg.fg_type != MCA_FG_TYPE_SINGLE)
			ret |= platform_fg_ops_get_full_design(FG_IC_SLAVE,
							       &dc_slave);
		if (!ret)
			fg->batt_dc = fg->cfg.fg_type != MCA_FG_TYPE_SINGLE ?
				      dc_master + dc_slave : dc_master;
	}

	if (fg->cfg.fg_type == MCA_FG_TYPE_PARALLEL) {
		r1 = platform_fg_ops_get_cyclecount(FG_IC_SLAVE,
				&fg->slave_batt_info.cycle_count);
		r2 = platform_fg_ops_get_cyclecount(FG_IC_MASTER,
				&fg->master_batt_info.cycle_count);
		if (!r1 && !r2)
			fg->batt_info.cycle_count =
				max(fg->master_batt_info.cycle_count,
				    fg->slave_batt_info.cycle_count);

		r1 = platform_fg_ops_get_rm(FG_IC_SLAVE, &fg->slave_batt_info.rm);
		r2 = platform_fg_ops_get_rm(FG_IC_MASTER,
					    &fg->master_batt_info.rm);
		if (!r1 && !r2)
			fg->batt_info.rm = fg->master_batt_info.rm +
					   fg->slave_batt_info.rm;
	} else if (fg->cfg.fg_type == MCA_FG_TYPE_SINGLE) {
		platform_fg_ops_get_cyclecount(FG_IC_MASTER, &fg->batt_cyclecount);
		platform_fg_ops_get_rm(FG_IC_MASTER, &fg->batt_rm);
	}
}

static void strategy_fg_clear_pmic_temp_term(struct strategy_fg *fg)
{
	if (!fg->cfg.support_pmic_vterm_dynamics_adjust)
		return;

	mca_vote_override(fg->vterm_voter, "pmic_temp_term", false, 0);
	mca_vote_override(fg->buck_input_voter, "pmic_temp_term", false, 0);
}

/*
 * Charging is called done when the buck says it terminated, or when the
 * current has stayed under a little above the termination current with the
 * cell at the float voltage (less vterm_delta) for five readings.
 */
#define SOFT_TERM_ITERM_HIGH_MA		1000
#define SOFT_TERM_ITERM_HIGH_PCT	110
#define SOFT_TERM_ITERM_LOW_PCT		115
#define SOFT_TERM_DEFAULT_ITERM		287
#define SOFT_TERM_COUNT			5

static noinline void mca_strategy_check_sigle_termination(struct strategy_fg *fg)
{
	static int count;
	int quick_charge_status = 0, online = 0, chgr_status = 0;
	int iterm, vterm, soft_iterm, soft_vterm;

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
				     STRATEGY_STATUS_TYPE_ONLINE, &online);
	mca_strategy_func_get_status(online ? STRATEGY_FUNC_TYPE_QUICK_WIRELESS :
				     STRATEGY_FUNC_TYPE_QUICK_CHARGE,
				     STRATEGY_STATUS_TYPE_CHARGING,
				     &quick_charge_status);

	if (fg->voter_ok) {
		iterm = mca_get_effective_result(fg->iterm_voter);
		vterm = mca_get_effective_result(fg->vterm_voter);
		soft_iterm = iterm * (iterm < SOFT_TERM_ITERM_HIGH_MA ?
				      SOFT_TERM_ITERM_LOW_PCT :
				      SOFT_TERM_ITERM_HIGH_PCT) / 100;
	} else {
		iterm = STRATEGY_FG_DEFAULT_ITERM;
		vterm = STRATEGY_FG_DEFAULT_VTERM;
		soft_iterm = SOFT_TERM_DEFAULT_ITERM;
	}

	if (!fg->power_present ||
	    quick_charge_status == MCA_QUICK_CHG_STS_CHARGING ||
	    fg->charging_done || vterm <= fg->cfg.fg_hightemp_vterm)
		return;

	platform_class_buckchg_ops_get_chg_status(MAIN_BUCK_CHARGER,
						  &chgr_status);
	if (chgr_status == MCA_BATT_CHGR_STATUS_TERMINATION) {
		strategy_fg_clear_pmic_temp_term(fg);
		strategy_class_fg_ops_set_charging_done(true);
		mca_log_err("vterm: %d, iterm: %d, vbat: %d, ibat: %d, pvbat: %d, chgr_status: %d, charging_done: %d\n",
			    vterm, iterm, fg->batt_voltage, fg->batt_current,
			    fg->pack_vbat, chgr_status, fg->charging_done);
		return;
	}

	soft_vterm = vterm - fg->cfg.vterm_delta;
	if (-fg->batt_current / 1000 < soft_iterm &&
	    fg->batt_voltage >= soft_vterm) {
		fg->near_vterm = true;
		if (++count >= SOFT_TERM_COUNT) {
			count = 0;
			strategy_fg_clear_pmic_temp_term(fg);
			strategy_class_fg_ops_set_charging_done(true);
		}
		mca_log_err("soft_vterm: %d, soft_iterm: %d, vbat: %d, ibat: %d, pvbat: %d, count: %d, charging_done: %d\n",
			    soft_vterm, soft_iterm, fg->batt_voltage,
			    fg->batt_current, fg->pack_vbat, count,
			    fg->charging_done);
		return;
	}

	fg->near_vterm = false;
	count = 0;
}

/*
 * Put a two-cell pack back where it started: both gauges off their
 * charge-over hold, the first-termination bookkeeping cleared and every
 * limit placed while one cell finished ahead of the other taken off.
 */
static void strategy_fg_reset_co_to_default(struct strategy_fg *fg)
{
	platform_fg_ops_set_co(FG_IC_MASTER, false);
	platform_fg_ops_set_co(FG_IC_SLAVE, false);
	fg->monitor_soc_flag = false;
	fg->first_termination = false;
	fg->fg1_batt_ctr_enabled = false;
	fg->fg2_batt_ctr_enabled = false;
	fg->fg_lock_flag = false;
	fg->switch_pmic_term = false;
	mca_vote_override(fg->vterm_voter, "para_term", false, 0);
	mca_vote(fg->charge_limit_voter, "losslessRec", false, 0);
	mca_vote(fg->flip_charge_curr_voter, "losslessRec", false, 0);
	mca_vote(fg->flip_charge_curr_voter, "base_close_curr", false, 0);
}

static void strategy_fg_reset_default_work(struct work_struct *work)
{
	platform_fg_ops_set_co(FG_IC_MASTER, false);
	platform_fg_ops_set_co(FG_IC_SLAVE, false);
}

/*
 * The first cell of a pair to report full is put on charge-over hold so the
 * other can catch up.  If the two drift apart while one is held, the hold is
 * dropped and not taken again until they are back within 4%.
 */
static void mca_strategy_parallel_first_termination(struct strategy_fg *fg)
{
	static int soc_delt;
	int chgr_status = 0, rsoc_master = 0, rsoc_slave = 0, delta;
	bool fc_master = false, fc_slave = false;

	if (platform_class_buckchg_ops_get_chg_status(MAIN_BUCK_CHARGER,
						      &chgr_status)) {
		mca_log_err("get chg status fail");
		return;
	}

	platform_fg_ops_get_rsoc(FG_IC_MASTER, &rsoc_master);
	platform_fg_ops_get_rsoc(FG_IC_SLAVE, &rsoc_slave);
	mca_log_err("fg_master_rsoc:%d, fg_slave_rsoc:%d", rsoc_master,
		    rsoc_slave);

	if (fg->fg1_batt_ctr_enabled || fg->fg2_batt_ctr_enabled ||
	    fg->fg_lock_flag) {
		delta = abs(rsoc_master - rsoc_slave);
		if (delta <= 5) {
			if (!fg->fg_lock_flag)
				fg->monitor_soc_flag = false;
			else if (delta <= 4)
				fg->fg_lock_flag = false;
		} else if (!fg->fg_lock_flag) {
			if (!fg->monitor_soc_flag) {
				soc_delt = delta;
				fg->monitor_soc_flag = true;
			} else if (delta - 1 <= soc_delt) {
				fg->fg_lock_flag = false;
			} else {
				strategy_fg_reset_co_to_default(fg);
				fg->fg_lock_flag = true;
				fg->monitor_soc_flag = false;
			}
		}
	}

	platform_fg_ops_get_fc(FG_IC_MASTER, &fc_master);
	platform_fg_ops_get_fc(FG_IC_SLAVE, &fc_slave);
	mca_log_err("fc_master:%d, fc_slave:%d", fc_master, fc_slave);
	if (fg->fg1_batt_ctr_enabled || fg->fg2_batt_ctr_enabled)
		return;

	if (fg->lossless_recharge) {
		if (fg->dual_force_full[FG_IC_MASTER]) {
			platform_fg_ops_set_co(FG_IC_MASTER, true);
			fg->fg1_batt_ctr_enabled = true;
			fg->first_termination = false;
			mca_log_err("FG_MASTER lossless_recharge_full is 1");
		} else if (fg->dual_force_full[FG_IC_SLAVE]) {
			platform_fg_ops_set_co(FG_IC_SLAVE, true);
			fg->fg2_batt_ctr_enabled = true;
			fg->first_termination = false;
			mca_log_err("FG_SLAVE lossless_recharge_full is 1");
		}
	} else if (fc_master && !fg->fg_lock_flag) {
		platform_fg_ops_set_co(FG_IC_MASTER, true);
		fg->fg1_batt_ctr_enabled = true;
		fg->first_termination = true;
		mca_log_err("FG_MASTER FC is 1");
	} else if (fc_slave && !fg->fg_lock_flag) {
		platform_fg_ops_set_co(FG_IC_SLAVE, true);
		fg->fg2_batt_ctr_enabled = true;
		fg->first_termination = true;
		mca_log_err("FG_SLAVE FC is 1");
	}
}

/*
 * The termination current for one cell of a pair, from its temperature
 * band; a band is only left downwards once the temperature is under it by
 * the band's offset.
 */
static int strategy_fg_get_term_current_new(struct strategy_fg *fg,
					    int fg_index)
{
	static int last_temp_index[FG_IC_MAX] = { -1, -1 };
	struct term_curr_para *tc = &fg->cfg.term_curr_data[fg_index];
	struct term_curr_data *d = tc->iterm_data;
	int temp = (fg_index ? fg->slave_batt_info.temp :
			       fg->master_batt_info.temp) / 10;
	int last_idx = last_temp_index[fg_index];
	bool offset_flag = false;
	int idx;

	for (idx = tc->iterm_para_size - 1; idx >= 0; idx--)
		if (temp >= d[idx].batt_temp)
			break;
	if (idx < 0)
		idx = 0;

	mca_log_info("fg[%d] temp_index %d, last_index %d", fg_index, idx,
		     last_idx);

	if (last_idx != -1 && idx < last_idx &&
	    temp >= d[last_idx].batt_temp - d[last_idx].batt_temp_offset) {
		offset_flag = true;
		idx = last_idx;
	}
	last_temp_index[fg_index] = idx;
	fg->temp_offset_flag[fg_index] = offset_flag;
	mca_log_info("temp_offset_flag[%d]: %d", fg_index, offset_flag);

	return fg->fast_charge ? d[idx].ffc_iterm : d[idx].normal_iterm;
}

#define PARA_TERM_TEMP_WARM 300
#define PARA_TERM_TEMP_HOT 400
#define PARA_TERM_VTERM_SLAVE_HOT 20
#define PARA_TERM_VTERM_WARM 50
#define PARA_TERM_VTERM_HOT 65
#define PARA_TERM_VTERM_WLS_COLD 20
#define PARA_TERM_VTERM_WLS_WARM 30
#define PARA_TERM_VTERM_WLS_HOT 60
#define PARA_TERM_HIGH_TEMP 400
#define PARA_TERM_HIGH_TEMP_ITERM 200

static void strategy_fg_check_parallel_termination(struct strategy_fg *fg,
						   int vterm)
{
	static bool last_first_termination;
	static int last_term[FG_IC_MAX] = { 187, 61 };
	int vterm_target, offset, final_term_curr, total_iterm, i;
	bool fc_master = false;

	mca_strategy_parallel_first_termination(fg);

	/* Once one cell is held, the other may be charged a little higher. */
	if (fg->cfg.support_base_flip &&
	    last_first_termination != fg->first_termination) {
		last_first_termination = fg->first_termination;
		if (!last_first_termination) {
			mca_log_err("last_first_termination: clean para_term");
			mca_vote_override(fg->vterm_voter, "para_term", false, 0);
		} else {
			vterm_target = 0;
			if (fg->fg1_batt_ctr_enabled) {
				offset = !fg->wls_online &&
					 fg->slave_batt_info.temp >= PARA_TERM_TEMP_HOT ?
					 PARA_TERM_VTERM_SLAVE_HOT : 0;
				vterm_target = (fg->fast_charge ? offset : 0) + vterm;
			} else if (fg->fg2_batt_ctr_enabled) {
				int temp = fg->master_batt_info.temp;

				if (!fg->wls_online)
					offset = temp < PARA_TERM_TEMP_HOT ?
						 PARA_TERM_VTERM_WARM :
						 PARA_TERM_VTERM_HOT;
				else if (temp < PARA_TERM_TEMP_WARM)
					offset = PARA_TERM_VTERM_WLS_COLD;
				else
					offset = temp < PARA_TERM_TEMP_HOT ?
						 PARA_TERM_VTERM_WLS_WARM :
						 PARA_TERM_VTERM_WLS_HOT;
				vterm_target = (fg->fast_charge ? offset : 0) + vterm;
			}
			mca_log_err("last_first_termination:vterm_target:%d",
				    vterm_target);
			if (vterm_target != vterm || fg->switch_pmic_term)
				mca_vote_override(fg->vterm_voter, "para_term",
						  true, vterm_target);
		}
	}

	for (i = 0; i < FG_IC_MAX; i++) {
		fg->dual_iterm[i] = strategy_fg_get_term_current_new(fg, i);
		mca_log_err("fg_index: %d, last_term: %d, dual_iterm :%d\n", i,
			    last_term[i], fg->dual_iterm[i]);
		last_term[i] = fg->dual_iterm[i];
	}

	if (fg->fg2_batt_ctr_enabled) {
		final_term_curr = fg->fg1_batt_ctr_enabled ?
				  0 : last_term[FG_IC_MASTER];
		total_iterm = final_term_curr;
	} else if (fg->fg1_batt_ctr_enabled) {
		final_term_curr = last_term[FG_IC_SLAVE];
		total_iterm = final_term_curr;
	} else {
		final_term_curr = last_term[FG_IC_SLAVE] ?
				  last_term[FG_IC_SLAVE] : last_term[FG_IC_MASTER];
		total_iterm = fg->dual_iterm[FG_IC_MASTER] +
			      fg->dual_iterm[FG_IC_SLAVE];
	}

	/* Both charge loops terminate on the pack as a whole. */
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				  MCA_EVENT_BATTERY_TOTAL_ITERM, total_iterm);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
				  MCA_EVENT_BATTERY_TOTAL_ITERM, total_iterm);

	if (fg->cfg.support_base_flip) {
		platform_fg_ops_get_fc(FG_IC_MASTER, &fc_master);
		if (fg->fast_charge &&
		    fg->batt_temperature > PARA_TERM_HIGH_TEMP &&
		    fg->fg2_batt_ctr_enabled && !fc_master)
			mca_vote(fg->iterm_voter, "high_temp_para", true,
				 PARA_TERM_HIGH_TEMP_ITERM);
		else
			mca_vote(fg->iterm_voter, "high_temp_para", false, 0);
	}

	mca_log_info("final_term_curr:%d", final_term_curr);
	if (final_term_curr != fg->last_final_term_curr) {
		fg->last_final_term_curr = final_term_curr;
		mca_vote(fg->iterm_voter, "para_term", true, final_term_curr);
	}
}

/*
 * The gauge is told to report the pack full a little before it would decide
 * so itself, once the current has stayed under a threshold near the float
 * voltage for a few readings.  It is only called done after the gauge has
 * had a few seconds to act on it.
 */
#define FORCE_FULL_RSOC_MIN			90
#define FORCE_FULL_FFC_ITERM_HIGH_MA		3000
#define FORCE_FULL_FFC_ITERM_HIGH_PCT		13
#define FORCE_FULL_FFC_ITERM_LOW_PCT		24
#define FORCE_FULL_FFC_ITERM_MIN_MA		500
#define FORCE_FULL_PMIC_FFC_ITERM_HIGH_PCT	113
#define FORCE_FULL_PMIC_FFC_ITERM_LOW_PCT	124
#define FORCE_FULL_NORMAL_ITERM_HYS_MA		120
#define FORCE_FULL_QUICK_ITERM_HIGH_MA		1000
#define FORCE_FULL_QUICK_ITERM_HIGH_PCT		115
#define FORCE_FULL_QUICK_ITERM_LOW_PCT		120
#define FORCE_FULL_VTERM_DELTA_MV		15
#define FORCE_FULL_VTERM_MIN_MV			4200
#define FORCE_FULL_COUNT_FAST			3
#define FORCE_FULL_COUNT_NORMAL			2
#define FORCE_FULL_SETTLE_SEC			6

static void mca_strategy_single_force_fw_report_full(struct strategy_fg *fg)
{
	static int force_full_count;
	static time64_t start_force_full_time;
	int iterm = STRATEGY_FG_DEFAULT_ITERM;
	int vterm = STRATEGY_FG_DEFAULT_VTERM;
	int quick_charge_status = 0, online = 0, jeita_iterm = 0;
	int force_iterm, force_vterm, count_threshold, add;
	enum mca_strategy_func_type qc_type;
	time64_t now;

	if (!fg->power_present || fg->batt_rsoc < FORCE_FULL_RSOC_MIN ||
	    (!fg->fast_charge && !fg->support_normal_force_full))
		goto reset;

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
				     STRATEGY_STATUS_TYPE_ONLINE, &online);
	qc_type = online ? STRATEGY_FUNC_TYPE_QUICK_WIRELESS :
			   STRATEGY_FUNC_TYPE_QUICK_CHARGE;
	mca_strategy_func_get_status(qc_type, STRATEGY_STATUS_TYPE_CHARGING,
				     &quick_charge_status);

	if (fg->voter_ok) {
		iterm = mca_get_effective_result(fg->iterm_voter);
		vterm = mca_get_effective_result(fg->vterm_voter);
		/* A float voltage this low is a jeita band, not a full pack. */
		if (vterm <= FORCE_FULL_VTERM_MIN_MV) {
			if (fg->charging_done) {
				force_full_count = 0;
				fg->start_force_full = false;
				fg->done_force_full = false;
				start_force_full_time = 0;
			}
			mca_log_info("vterm=%d, return for vetrm abormal", vterm);
			return;
		}
	}

	if (!fg->cfg.terminated_by_cp) {
		if (quick_charge_status == MCA_QUICK_CHG_STS_CHARGING)
			return;
		if (fg->fast_charge)
			force_iterm = iterm * (iterm < FORCE_FULL_QUICK_ITERM_HIGH_MA ?
					       FORCE_FULL_QUICK_ITERM_LOW_PCT :
					       FORCE_FULL_QUICK_ITERM_HIGH_PCT) / 100;
		else
			force_iterm = iterm + fg->cfg.normal_force_full_hys;
		force_vterm = vterm - FORCE_FULL_VTERM_DELTA_MV;
	} else if (fg->fast_charge &&
		   quick_charge_status == MCA_QUICK_CHG_STS_CHARGING) {
		mca_log_info("wire quick charge force full state");
		mca_strategy_func_get_status(qc_type,
					     STRATEGY_STATUS_TYPE_JEITA_FFC_ITERM,
					     &jeita_iterm);
		add = jeita_iterm * (jeita_iterm <= FORCE_FULL_FFC_ITERM_HIGH_MA ?
				     FORCE_FULL_FFC_ITERM_LOW_PCT :
				     FORCE_FULL_FFC_ITERM_HIGH_PCT) / 100;
		force_iterm = jeita_iterm + max(add, FORCE_FULL_FFC_ITERM_MIN_MA);
		force_vterm = vterm - fg->cfg.fast_force_full_vol_delta;
	} else if (fg->fast_charge) {
		mca_log_info("PMIC FFC force full state");
		force_iterm = iterm * (iterm <= FORCE_FULL_FFC_ITERM_HIGH_MA ?
				       FORCE_FULL_PMIC_FFC_ITERM_LOW_PCT :
				       FORCE_FULL_PMIC_FFC_ITERM_HIGH_PCT) / 100;
		force_vterm = vterm - fg->cfg.fast_force_full_vol_delta;
	} else {
		mca_log_info("wire normal charge force full state");
		force_iterm = iterm + FORCE_FULL_NORMAL_ITERM_HYS_MA;
		force_vterm = vterm - FORCE_FULL_VTERM_DELTA_MV;
	}

	count_threshold = fg->fast_charge ? FORCE_FULL_COUNT_FAST :
					    FORCE_FULL_COUNT_NORMAL;
	mca_log_info("terminated_by_cp = %d, force_iterm = %d, force_vterm = %d, start_force_full = %d",
		     fg->cfg.terminated_by_cp, force_iterm, force_vterm,
		     fg->start_force_full);
	mca_log_info("count_threshold = %d, fg->fast_charge = %d",
		     count_threshold, fg->fast_charge);

	if (fg->charging_done)
		goto reset;

	if (-fg->batt_current / 1000 < force_iterm &&
	    fg->batt_voltage > force_vterm && !fg->start_force_full) {
		force_full_count++;
		mca_log_info("force_full_count = %d", force_full_count);
		if (force_full_count < count_threshold)
			return;
		platform_fg_ops_set_force_report_full(FG_IC_MASTER);
		force_full_count = 0;
		fg->start_force_full = true;
		start_force_full_time = ktime_get_boottime_seconds();
		mca_log_info("force fw report full start_force_full_time = %d",
			     (int)start_force_full_time);
		schedule_delayed_work(&fg->force_report_full_work,
			msecs_to_jiffies(STRATEGY_FG_FORCE_FULL_RECHECK_MS));
		return;
	}

	force_full_count = 0;
	if (!start_force_full_time)
		return;
	now = ktime_get_boottime_seconds() - start_force_full_time;
	mca_log_info("force fw report full cur_time = %d", (int)now);
	if (now > FORCE_FULL_SETTLE_SEC) {
		start_force_full_time = 0;
		fg->done_force_full = true;
	}
	return;

reset:
	force_full_count = 0;
	fg->start_force_full = false;
	fg->done_force_full = false;
	start_force_full_time = 0;
}

static noinline void mca_strategy_parallel_force_fw_report_full(
	struct strategy_fg *fg, int fg_index)
{
	static int force_full_count[FG_IC_MAX];
	struct fg_batt_info *info = fg_index ? &fg->slave_batt_info :
					       &fg->master_batt_info;
	int iterm = STRATEGY_FG_DEFAULT_ITERM;
	int vterm = STRATEGY_FG_DEFAULT_VTERM;
	int force_iterm, force_vterm, count, status = 0;

	if (!fg->power_present || !mca_get_effective_result(fg->en_voter))
		return;

	count = fg->lossless_recharge &&
		(fg->cfg.support_base_flip || fg->cfg.base_flip_same) ? 2 : 6;

	if (fg->voter_ok) {
		iterm = fg->dual_iterm[fg_index];
		vterm = mca_get_client_vote(fg->vterm_voter, "jeita");
	}

	if (fg->fg1_batt_ctr_enabled || fg->fg2_batt_ctr_enabled) {
		force_iterm = iterm * 130 / 100;
		force_vterm = vterm - 30;
	} else if (fg->cfg.base_flip_same) {
		force_vterm = vterm - 15;
		force_iterm = fg->fast_charge ? iterm * 115 / 100 : iterm + 30;
	} else if (fg->fast_charge) {
		force_iterm = iterm * 115 / 100;
		force_vterm = vterm - 30;
	} else {
		force_vterm = vterm - 25;
		if (fg->wls_online && fg->batt_rsoc > 97)
			force_iterm = iterm + 33;
		else if (fg->wls_online || fg->batt_temperature > 30)
			force_iterm = iterm + (fg_index ? 12 : 33);
		else
			force_iterm = iterm + (fg_index ? 12 : 13);
	}

	mca_log_info("fg_index[%d], vterm: %d, iterm: %d, force_iterm: %d, force_vterm: %d, start_force_full: %d\n",
		     fg_index, vterm, iterm, force_iterm, force_vterm,
		     fg->dual_force_full[fg_index]);

	if (!fg->power_present || info->temp >= 480 || fg->charging_done ||
	    -info->curr / 1000 >= force_iterm || info->volt <= force_vterm ||
	    fg->dual_force_full[fg_index]) {
		force_full_count[fg_index] = 0;
		fg->dual_force_full[fg_index] = false;
		return;
	}

	force_full_count[fg_index]++;
	mca_log_err("force_full_count[%d] =  %d", fg_index,
		    force_full_count[fg_index]);
	if (force_full_count[fg_index] < count)
		return;

	/* A wired fast charge on the master cell of a base pack. */
	if (!fg->wls_online && fg->cfg.support_base_flip &&
	    !fg->fg1_batt_ctr_enabled && fg_index == FG_IC_MASTER &&
	    !fg->fg2_batt_ctr_enabled) {
		status = 3;
		mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					     STRATEGY_STATUS_TYPE_CHARGING,
					     &status);
		if (status == MCA_QUICK_CHG_STS_CHARGING) {
			mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
						  MCA_EVENT_MASTER_BATT_CLOSE,
						  fg->dual_iterm[FG_IC_SLAVE]);
			force_full_count[FG_IC_MASTER] = 0;
			fg->switch_pmic_term = true;
			mca_log_err("switch to pmic termination\n");
			return;
		}
	}

	platform_fg_ops_set_force_report_full(fg_index);
	force_full_count[fg_index] = 0;
	fg->dual_force_full[fg_index] = true;
	mca_log_err("start report full");
	if (!fg->self_equal_flag[fg_index]) {
		fg->self_equal_count[fg_index] = 0;
		return;
	}
	fg->self_equal_count[fg_index] = 1;
	if (fg->self_equal_max_count <= 1) {
		fg->self_equal_count[fg_index] = 0;
		fg->self_equal_flag[fg_index] = false;
	}
	mca_log_err("start equal report full, self_equal_count[fg_index]: %d",
		    fg->self_equal_max_count > 1);
}

/*
 * Every 50 cycles, a pair whose cells have drifted apart in health (or, if
 * not, in cycle count) marks the worse one for self-equalising.
 */
static void strategy_fg_check_parallel_force_fw_report_full(
	struct strategy_fg *fg)
{
	int soh_diff, cycle_diff;

	if (fg->master_batt_info.cycle_count % 50 == 0 ||
	    fg->slave_batt_info.cycle_count % 50 == 0) {
		soh_diff = fg->master_batt_info.soh - fg->slave_batt_info.soh;
		cycle_diff = fg->master_batt_info.cycle_count -
			     fg->slave_batt_info.cycle_count;
		if (abs(soh_diff) >= 5)
			fg->self_equal_flag[soh_diff < 0 ?
					    FG_IC_MASTER : FG_IC_SLAVE] = true;
		else if (abs(cycle_diff) >= 10)
			fg->self_equal_flag[cycle_diff > 0 ?
					    FG_IC_MASTER : FG_IC_SLAVE] = true;
		mca_log_info("soh_diff: %d, cycle_diff: %d", soh_diff,
			     cycle_diff);
	}

	mca_strategy_parallel_force_fw_report_full(fg, FG_IC_MASTER);
	mca_strategy_parallel_force_fw_report_full(fg, FG_IC_SLAVE);
}

static noinline void mca_strategy_start_recharging(struct strategy_fg *fg)
{
	mca_log_err("start to recharging\n");
	strategy_class_fg_ops_set_charging_done(false);
	strategy_class_fg_set_fastcharge(false);
	fg->recharging = true;
	mca_vote(fg->en_voter, "term_recharge", false, 0);
	mca_mod_delayed_work(&fg->monitor_work,
			     msecs_to_jiffies(STRATEGY_FG_WORK_INTERVAL_FAST));
}

static void mca_strategy_check_recharge(struct strategy_fg *fg)
{
	if (!fg->power_present || !fg->charging_done || fg->recharging)
		return;

	if (!fg->is_eu_model) {
		if (fg->batt_raw_soc <= fg->cfg.report_full_raw_soc) {
			mca_strategy_start_recharging(fg);
			mca_log_err("ok to recharging, raw_soc: %d\n",
				    fg->batt_raw_soc);
		}
	} else if (fg->batt_ui_soc < EU_RECHARGER_SOC) {
		mca_strategy_start_recharging(fg);
		mca_log_err("eu start recharging ui_soc: %d rsoc:%d\n",
			    fg->batt_ui_soc, fg->batt_rsoc);
	}
}

static int mca_strategy_get_ffc_safe_item(struct strategy_fg *fg)
{
	int cycle_index, temp_index;

	if (fg->batt_cyclecount <= 100)
		cycle_index = 0;
	else if (fg->batt_cyclecount <= 300)
		cycle_index = 1;
	else if (fg->batt_cyclecount <= 500)
		cycle_index = 2;
	else if (fg->batt_cyclecount <= 800)
		cycle_index = 3;
	else
		cycle_index = 4;

	if (fg->batt_temperature < 200)
		temp_index = 0;
	else if (fg->batt_temperature < 250)
		temp_index = 1;
	else if (fg->batt_temperature < 300)
		temp_index = 2;
	else if (fg->batt_temperature < 350)
		temp_index = 3;
	else if (fg->batt_temperature < 400)
		temp_index = 4;
	else if (fg->batt_temperature < 450)
		temp_index = 5;
	else
		temp_index = 6;

	return fg->cfg.ffc_safe_item[cycle_index][temp_index];
}

static noinline int mca_strategy_get_fl4p0_iterm(struct strategy_fg *fg)
{
	int safe_item = mca_strategy_get_ffc_safe_item(fg);
	int calibration_ffc_iterm = 0;
	int iterm;

	platform_fg_ops_get_calibration_ffc_iterm(FG_IC_MASTER,
						  &calibration_ffc_iterm);
	iterm = max(safe_item, calibration_ffc_iterm);
	mca_log_info("safe_item: %d, calibration_ffc_iterm: %d, iterm: %d\n",
		     safe_item, calibration_ffc_iterm, iterm);

	return iterm;
}

/*
 * FL4.0 packs calibrate their termination current: near the float voltage
 * on a fast charge the charge carries on until the gauge has put back the
 * energy it measured.
 */
#define FL4P0_SUPPLEMENT_DONE 0xFFFF
static void mca_strategy_check_fl4p0_status(struct strategy_fg *fg)
{
	static int count;
	int real_supplement_energy = 0;
	int iterm, vterm;

	if (fg->fast_charge)
		mca_vote(fg->iterm_voter, "calibration", true,
			 mca_strategy_get_ffc_safe_item(fg));
	else
		mca_vote(fg->iterm_voter, "calibration", false, 0);

	if (fg->charging_done || fg->recharging || !fg->voter_ok ||
	    !fg->fast_charge || fg->ffc_continue_charge)
		return;

	iterm = mca_strategy_get_fl4p0_iterm(fg);
	vterm = mca_get_effective_result(fg->vterm_voter);
	if (-fg->batt_current / 1000 < iterm &&
	    vterm >= STRATEGY_FG_DEFAULT_VTERM &&
	    fg->batt_voltage >= vterm - 11 && !fg->ffc_continue_charge) {
		if (count++ >= 2) {
			count = 0;
			fg->ffc_continue_charge = true;
		}
		mca_log_info("vterm: %d, iterm: %d, vbat: %d, ibat: %d", vterm,
			     iterm, fg->batt_voltage, fg->batt_current);
	} else {
		count = 0;
	}

	if (!fg->ffc_continue_charge)
		return;

	fg->calibration_temp = fg->batt_temperature;
	platform_fg_ops_get_real_supplement_energy(FG_IC_MASTER,
						   &real_supplement_energy);
	if (real_supplement_energy == FL4P0_SUPPLEMENT_DONE) {
		mca_log_info("ok to calibration iterm");
		strategy_class_fg_ops_set_charging_done(true);
	} else {
		mca_log_info("continue calibration");
		schedule_delayed_work(&fg->fl4p0_calibration_work, 0);
	}
}

/*
 * Lossless recharge: a pack that finished on a fast charge and then sat at
 * a moderate temperature for the configured rest time is topped up again.
 */
#define LOSSLESS_BASE_CHARGE_LIMIT 600
#define LOSSLESS_BASE_FLIP_CURR 100
static void mca_strategy_check_lossless_recharge(struct strategy_fg *fg)
{
	static ktime_t last_time;
	ktime_t time_now = ktime_get();
	s64 time_diff;
	int batt_temp = 0;

	if (!fg->charging_done) {
		last_time = time_now;
		return;
	}

	if (fg->is_eu_model && fg->batt_cyclecount <= EU_LOSSES_START_CYCLECOUNT) {
		fg->lossless_recharge = false;
		mca_log_info("eu model CycleCount Too Low Disable\n");
		return;
	}

	/* A suspended input is deliberately not charging. */
	if (!fg->input_suspend_voter)
		fg->input_suspend_voter = mca_find_votable("input_suspend");
	if (fg->input_suspend_voter &&
	    mca_get_effective_result(fg->input_suspend_voter)) {
		mca_log_info("set input_suspend, skip check_lossless_recharge \n");
		return;
	}

	time_diff = ktime_divns(ktime_sub(time_now, last_time), NSEC_PER_SEC);
	mca_log_info("time_now: %lld, last_time: %lld, time_diff: %lld\n",
		     time_now, last_time, time_diff);

	strategy_class_fg_ops_get_temperature(&batt_temp);
	batt_temp /= 10;
	if (fg->charging_done && fg->fast_charge &&
	    batt_temp >= fg->cfg.rechg_cfg.temp_low &&
	    batt_temp <= fg->cfg.rechg_cfg.temp_high &&
	    time_diff >= fg->cfg.rechg_cfg.rest_time) {
		fg->lossless_recharge = true;
		if (fg->cfg.support_base_flip || fg->cfg.base_flip_same) {
			mca_vote(fg->charge_limit_voter, "losslessRec", true,
				 LOSSLESS_BASE_CHARGE_LIMIT);
			mca_vote(fg->flip_charge_curr_voter, "losslessRec", true,
				 LOSSLESS_BASE_FLIP_CURR);
		} else if (fg->cfg.support_lossless_curr_limit) {
			mca_vote(fg->charge_limit_voter, "losslessRec", true,
				 fg->cfg.lossless_curr_limit);
		}
	}
	mca_log_info("charging_done: %d, fast_charge: %d, temp: %d, lossless_recharge: %d\n",
		     fg->charging_done, fg->fast_charge, batt_temp,
		     fg->lossless_recharge);

	if (fg->lossless_recharge) {
		mca_strategy_start_recharging(fg);
		mca_log_err("ok to lossless_recharge\n");
	}
}

/*
 * Charge-speed DFX: a Xiaomi PPS charge that adds less than its rated power
 * should over ten minutes is reported, and so is any other charger type that
 * stays attached for a minute.
 */
#define CHARGE_SLOWLY_SOC_MAX 80
#define CHARGE_SLOWLY_POWER_MIN_W 32
#define CHARGE_SLOWLY_POWER_MID_MW 67000
#define CHARGE_SLOWLY_POWER_HIGH_MW 100000
#define CHARGE_SLOWLY_RM_THR_LOW 200
#define CHARGE_SLOWLY_RM_THR_MID 420
#define CHARGE_SLOWLY_RM_THR_HIGH 500
#define CHARGE_SLOWLY_CHECK_PERIOD_S 600
#define CHARGE_SLOWLY_CAPACITY_MARGIN 100
#define CHARGE_SLOWLY_DFX_PARA_NUM 5
#define NON_STD_CHARGER_PLUGIN_TIME_S 60

static int charge_get_batt_rm_threshold(void)
{
	static int device_power_max;
	int batt_rm_thr;

	if (!device_power_max)
		mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					     STRATEGY_STATUS_TYPE_DEVICE_POWER_MAX,
					     &device_power_max);

	if (device_power_max > CHARGE_SLOWLY_POWER_HIGH_MW)
		batt_rm_thr = CHARGE_SLOWLY_RM_THR_HIGH;
	else if (device_power_max >= CHARGE_SLOWLY_POWER_MID_MW)
		batt_rm_thr = CHARGE_SLOWLY_RM_THR_MID;
	else
		batt_rm_thr = CHARGE_SLOWLY_RM_THR_LOW;

	mca_log_info("device_power_max:%d, batt_rm_thr:%d\n", device_power_max,
		     batt_rm_thr);
	return batt_rm_thr;
}

static void charge_slowly_monitor_func(struct strategy_fg *fg)
{
	static time64_t nowtime, nowplugintime;
	static int now_rm, now_soc;
	u32 powermax = 0;
	int batt_rm_thr, usable_capacity, expect_soc;
	int dfx_data[CHARGE_SLOWLY_DFX_PARA_NUM] = { 0 };

	mca_log_err("realtype:%d, soc:%d, rm:%d\n", fg->real_type,
		    fg->batt_ui_soc, now_rm);
	mca_log_err("time:%d, timeplugin:%d plugin:%d\n", (int)nowtime,
		    (int)nowplugintime, (int)fg->online_time);
	nowplugintime = ktime_get_boottime_seconds();

	if (fg->real_type != XM_CHARGER_TYPE_PD_VERIFY) {
		if (fg->real_type >= XM_CHARGER_TYPE_SDP &&
		    fg->real_type <= XM_CHARGER_TYPE_PPS &&
		    !fg->non_std_charger_reported &&
		    nowplugintime - fg->online_time > NON_STD_CHARGER_PLUGIN_TIME_S) {
			dfx_data[0] = fg->real_type;
			mca_charge_mievent_report(CHARGE_DFX_NON_STANDARD_CHARGER,
						  dfx_data, 1);
			fg->non_std_charger_reported = true;
			mca_log_err("report dfx\n");
		}
		goto reset;
	}

	if (fg->batt_ui_soc > CHARGE_SLOWLY_SOC_MAX)
		goto reset;

	fg->online_time = ktime_get_boottime_seconds();
	protocol_class_get_adapter_max_power(ADAPTER_PROTOCOL_PPS, &powermax);
	batt_rm_thr = charge_get_batt_rm_threshold();
	mca_log_err("powermax:%d designcapacity:%d curr_batt_rm_thr:%d\n",
		    powermax, fg->batt_dc, batt_rm_thr);
	if ((int)powermax <= CHARGE_SLOWLY_POWER_MIN_W ||
	    batt_rm_thr * 1000 > fg->batt_dc)
		return;

	if (!fg->slow_chg_start_time) {
		fg->slow_chg_initial_rm = fg->batt_rm / 1000;
		fg->slow_chg_initial_soc = fg->batt_ui_soc;
		fg->slow_chg_start_time = ktime_get_boottime_seconds();
	}

	nowtime = ktime_get_boottime_seconds();
	now_rm = fg->batt_rm / 1000;
	now_soc = fg->batt_ui_soc;
	if (nowtime - fg->slow_chg_start_time < CHARGE_SLOWLY_CHECK_PERIOD_S)
		return;

	usable_capacity = fg->batt_dc / 1000 - CHARGE_SLOWLY_CAPACITY_MARGIN;
	expect_soc = usable_capacity ? batt_rm_thr * 100 / usable_capacity : 0;
	if (now_soc - fg->slow_chg_initial_soc < expect_soc) {
		mca_log_err("ntime:%d, initialtime:%d\n", (int)nowtime,
			    (int)fg->slow_chg_start_time);
		mca_log_err("nrm:%d, initialrm:%d\n", now_rm,
			    fg->slow_chg_initial_rm);
		mca_log_err("report dfx\n");
		dfx_data[0] = powermax;
		dfx_data[1] = fg->slow_chg_initial_rm;
		dfx_data[2] = now_rm;
		dfx_data[3] = fg->slow_chg_initial_soc;
		dfx_data[4] = now_soc;
		mca_charge_mievent_report(CHARGE_DFX_XMPPS_CHARGER_SLOWLY,
					  dfx_data, CHARGE_SLOWLY_DFX_PARA_NUM);
	}
	fg->slow_chg_initial_rm = now_rm;
	fg->slow_chg_initial_soc = now_soc;
	fg->slow_chg_start_time = nowtime;
	return;

reset:
	fg->slow_chg_start_time = 0;
	nowtime = 0;
	now_rm = 0;
	now_soc = 0;
}

/*
 * A pack behind the Qualcomm gauge authenticates itself before this driver
 * sees it, so it is taken as genuine once; the gauge is then kept told how
 * the pack is being charged.
 */
static void strategy_fg_update_qbg(struct strategy_fg *fg)
{
	int quick_charge_status = 0;
	int ibat_max = 0;

	if (!fg->batt_auth) {
		platform_fg_ops_set_authentic(FG_IC_MASTER, true);
		fg->batt_auth = 1;
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_BATT_AUTH_PASS, NULL);
		mca_log_err("onewire batt auth pass\n");
	}

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
				     STRATEGY_STATUS_TYPE_CHARGING,
				     &quick_charge_status);
	if (quick_charge_status)
		mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					     STRATEGY_STATUS_TYPE_QC_IBAT_MAX,
					     &ibat_max);
	else if (fg->charge_limit_voter)
		ibat_max = mca_get_effective_result(fg->charge_limit_voter);

	platform_fg_ops_qbg_send_chg_data(FG_IC_MASTER, fg->chg_status,
					  ibat_max);
}

/* The gauge does not land on 100% the instant charging stops. */
#define SOC_NOT_FULL_SETTLE_SEC 100
enum mievent_socnotfull_ele {
	MIEVENT_SOCNOTFULL_PARAM_VOLTAGE,
	MIEVENT_SOCNOTFULL_PARAM_UISOC,
	MIEVENT_SOCNOTFULL_PARAM_RSOC,
	MIEVENT_SOCNOTFULL_PARAM_MAX,
};

static void strategy_fg_check_soc_not_full(struct strategy_fg *fg)
{
	int param[MIEVENT_SOCNOTFULL_PARAM_MAX];
	time64_t curr_time;

	if (!fg->charging_done_time)
		return;

	curr_time = ktime_get_boottime_seconds() - fg->charging_done_time;
	if (curr_time < SOC_NOT_FULL_SETTLE_SEC)
		return;

	if (fg->batt_rsoc != 100 &&
	    (fg->batt_ui_soc != 100 || fg->is_eu_model)) {
		param[MIEVENT_SOCNOTFULL_PARAM_VOLTAGE] = fg->batt_voltage;
		param[MIEVENT_SOCNOTFULL_PARAM_UISOC] = fg->batt_ui_soc;
		param[MIEVENT_SOCNOTFULL_PARAM_RSOC] = fg->batt_rsoc;
		mca_charge_mievent_report(CHARGE_DFX_SOC_NOT_FULL, param,
					  MIEVENT_SOCNOTFULL_PARAM_MAX);
	}
	fg->charging_done_time = 0;
	mca_log_info("check report dfx soc_not_full curr_time=%d\n",
		     (int)curr_time);
}

static void strategy_fg_adjust_interval(struct strategy_fg *fg)
{
	if (fg->update_period != STRATEGY_FG_UPDATE_PERIOD_NONE) {
		fg->update_interval = fg->update_period;
		return;
	}

	if (fg->near_vterm) {
		fg->update_interval = STRATEGY_FG_WORK_INTERVAL_FAST;
	} else if (fg->chg_status == POWER_SUPPLY_STATUS_CHARGING) {
		fg->update_interval = fg->fast_charge ?
				      STRATEGY_FG_WORK_INTERVAL_FAST :
				      STRATEGY_FG_WORK_INTERVAL_NORMAL;
	} else {
		fg->update_interval = fg->screen_status ?
				      STRATEGY_FG_WORK_INTERVAL_SCREEN_ON :
				      STRATEGY_FG_WORK_INTERVAL_SCREEN_OFF;
		if (fg->batt_voltage_mean <
		    fg->vcutoff_shutdown_delay + BATT_LOW_VOLT_HY)
			fg->update_interval = fg->batt_temperature < BATT_TEMP_COLD_TH ?
					      STRATEGY_FG_WORK_INTERVAL_LOW_TEMP_FAST :
					      STRATEGY_FG_WORK_INTERVAL_LOW_VOLT;
		else if (fg->batt_temperature < BATT_TEMP_COLD_TH)
			fg->update_interval = STRATEGY_FG_WORK_INTERVAL_LOW_TEMP;
	}

	if (fg->batt_voltage < fg->vcutoff_sw + BATT_LOW_VOLT_SW_HY)
		fg->update_interval = STRATEGY_FG_WORK_INTERVAL_FAST;
}

static void strategy_fg_monitor_workfunc(struct work_struct *work)
{
	struct strategy_fg *fg = container_of(work, struct strategy_fg,
					      monitor_work.work);
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	bool was_init = fg->fg_init_flag;
	bool master_ok = false, slave_ok = false;
	bool resume_update, boot_update;
	int flag, vterm;

	if (!was_init) {
		flag = platform_fg_ops_probe_ok(FG_IC_MASTER, &master_ok);
		fg->dual_present[FG_IC_MASTER] = master_ok;
		mca_log_err("master flag: %d\n", flag);
		if (fg->cfg.fg_type != MCA_FG_TYPE_SINGLE) {
			flag |= platform_fg_ops_probe_ok(FG_IC_SLAVE, &slave_ok);
			fg->dual_present[FG_IC_SLAVE] = slave_ok;
		}
		mca_log_err("master and slave flag: %d\n", flag);
		if (flag) {
			fg_check_batt_auth(fg);
			mca_queue_delayed_work(&fg->monitor_work,
				msecs_to_jiffies(STRATEGY_FG_WORK_INTERVAL_PROBE));
			return;
		}

		strategy_fg_update_batt_info(fg);
		if (fg->cfg.fg_type == MCA_FG_TYPE_SINGLE) {
			fg->fg_init_flag = master_ok;
		} else {
			fg->fg_init_flag = master_ok && slave_ok;
			if (fg->cfg.support_base_flip || fg->cfg.base_flip_same)
				schedule_delayed_work(&fg->reset_default_work, 0);
		}
		if (hwid && hwid->country_version == CountryCN &&
		    fg->fg_init_flag && fg->cfg.support_fl4p0)
			platform_fg_ops_fl4p0_enable_check(FG_IC_MASTER);
	}

	/* After a long sleep or at boot the reading is shown as it is. */
	resume_update = fg->resume_update_soc;
	fg->resume_update_soc = false;
	boot_update = fg->boot_update_soc;
	if (boot_update) {
		fg->boot_update_soc = false;
		mca_log_info("boot_update_soc update is_eu_model done\n");
	}

	if (!fg->voter_ok)
		strategy_fg_init_voter(fg);

	fg_update_status(fg, was_init && !resume_update && !boot_update);

	if (fg->cfg.support_qbg)
		strategy_fg_update_qbg(fg);

	strategy_fg_check_soc_not_full(fg);

	if (fg->cfg.fg_type == MCA_FG_TYPE_PARALLEL) {
		vterm = mca_get_effective_result(fg->vterm_voter);
		if (fg->power_present && !fg->charging_done &&
		    vterm >= STRATEGY_FG_DEFAULT_VTERM) {
			strategy_fg_check_parallel_termination(fg, vterm);
			mca_strategy_check_sigle_termination(fg);
		}
	} else if (fg->cfg.fg_type == MCA_FG_TYPE_SINGLE) {
		mca_strategy_check_sigle_termination(fg);
	}

	if (fg->fg_init_flag) {
		if (fg->cfg.fg_type == MCA_FG_TYPE_PARALLEL)
			strategy_fg_check_parallel_force_fw_report_full(fg);
		else if (fg->cfg.fg_type == MCA_FG_TYPE_SINGLE)
			mca_strategy_single_force_fw_report_full(fg);
	}
	mca_strategy_check_recharge(fg);

	if (hwid && hwid->country_version == CountryCN && fg->cfg.support_fl4p0)
		mca_strategy_check_fl4p0_status(fg);
	if (fg->cfg.support_lossless_rechg)
		mca_strategy_check_lossless_recharge(fg);
	fg_check_batt_auth(fg);
	charge_slowly_monitor_func(fg);

	strategy_fg_adjust_interval(fg);
	mca_queue_delayed_work(&fg->monitor_work,
			       msecs_to_jiffies(fg->update_interval));
}

/* Pack internal short and safe-operating-area alerts from the gauge. */
static void strategy_fg_dtpt_monitor_work(struct work_struct *work)
{
	struct strategy_fg *fg = container_of(work, struct strategy_fg,
					      dtpt_monitor_work.work);
	static int last_dtpt_status;
	int master_isc = 0, slave_isc = 0, master_soa = 0, slave_soa = 0;
	int dtpt_status;

	platform_fg_ops_get_isc_alert_level(FG_IC_MASTER, &master_isc);
	platform_fg_ops_get_soa_alert_level(FG_IC_MASTER, &master_soa);
	if (fg->cfg.fg_type != MCA_FG_TYPE_SINGLE) {
		platform_fg_ops_get_isc_alert_level(FG_IC_SLAVE, &slave_isc);
		platform_fg_ops_get_soa_alert_level(FG_IC_SLAVE, &slave_soa);
	}
	master_isc = max(master_isc, slave_isc);
	master_soa = max(master_soa, slave_soa);
	mca_log_info("isc_alert = %d, soa_alert = %d\n", master_isc,
		     master_soa);

	dtpt_status = master_isc == 3 || master_soa;
	if (dtpt_status != last_dtpt_status) {
		last_dtpt_status = dtpt_status;
		mca_event_block_notify(MCA_EVENT_CHARGE_STATUS,
				       MCA_EVENT_BATTERY_DTPT, &dtpt_status);
	}

	schedule_delayed_work(&fg->dtpt_monitor_work,
			      msecs_to_jiffies(STRATEGY_FG_WORK_INTERVAL_DTPT));
}

static void strategy_fl4p0_calibration_work(struct work_struct *work)
{
	struct strategy_fg *fg = container_of(work, struct strategy_fg,
					      fl4p0_calibration_work.work);
	int supplement_energy = 0, charge_energy = 0;

	if (fg->charging_done)
		return;

	platform_fg_ops_get_real_supplement_energy(FG_IC_MASTER,
						   &supplement_energy);
	platform_fg_ops_get_calibration_charge_energy(FG_IC_MASTER,
						      &charge_energy);
	mca_log_info("supplement_energy = %d, charge_energy = %d",
		     supplement_energy, charge_energy);

	if (supplement_energy != FL4P0_SUPPLEMENT_DONE &&
	    supplement_energy < charge_energy &&
	    fg->batt_temperature >= fg->calibration_temp - 50 &&
	    fg->batt_temperature <= fg->calibration_temp + 50) {
		schedule_delayed_work(&fg->fl4p0_calibration_work,
			msecs_to_jiffies(STRATEGY_FG_FL4P0_CALIBRATION_MS));
		return;
	}

	mca_log_info("ok to calibration iterm");
	strategy_class_fg_ops_set_charging_done(true);
}

static void strategy_force_report_full_work(struct work_struct *work)
{
	struct strategy_fg *fg = container_of(work, struct strategy_fg,
					      force_report_full_work.work);
	static int count;

	if (fg->power_present && fg->fast_charge && !fg->charging_done) {
		if (fg->batt_rsoc == 100) {
			count = 0;
			mca_log_info("fg check report full ok");
			return;
		}
		platform_fg_ops_set_force_report_full(FG_IC_MASTER);
		mca_log_info("fg not report full, send force_report_full cmd again...");
		if (++count < FG_FORCE_REPORT_FULL_TIMES) {
			schedule_delayed_work(&fg->force_report_full_work,
				msecs_to_jiffies(STRATEGY_FG_FORCE_FULL_RECHECK_MS));
			return;
		}
	}
	count = 0;
}

static void strategy_nvt1000_ota_monitor_work(struct work_struct *work)
{
	struct strategy_fg *fg = container_of(work, struct strategy_fg,
					      ota_update_work.work);
	static int update_count;
	union power_supply_propval pval = { 0 };
	u32 ota_flag = 0;
	int average_current = 0;
	bool cid_status = false;
	int event_val;

	if (fg->batt_psy) {
		power_supply_get_property(fg->batt_psy, POWER_SUPPLY_PROP_STATUS,
					  &pval);
		fg->chg_status = pval.intval;
	}

	if (fg->chg_status == POWER_SUPPLY_STATUS_FULL ||
	    fg->chg_status == POWER_SUPPLY_STATUS_CHARGING) {
		mca_log_err("do not ota when charging\n");
		update_count = 0;
		return;
	}

	platform_fg_ops_get_ota_update_flag(FG_IC_MASTER, &ota_flag);
	if (ota_flag != NVT1000_OTA_UPDATE_FLAG) {
		mca_log_err("already ota update done\n");
		update_count = 0;
		return;
	}

	platform_fg_ops_get_average_current(FG_IC_MASTER, &average_current);
	protocol_class_pd_get_cid_status(TYPEC_PORT_0, &cid_status);

	if (fg->batt_rsoc >= NVT1000_OTA_SOC_MIN &&
	    fg->batt_temperature >= NVT1000_OTA_TEMP_MIN &&
	    average_current >= NVT1000_OTA_CURR_MIN &&
	    average_current <= NVT1000_OTA_CURR_MAX && !cid_status)
		update_count++;
	else
		update_count = 0;

	mca_log_err("cid_status = %d, batt_rsoc = %d, batt_temperature = %d, average_current = %d, update_count = %d",
		    cid_status, fg->batt_rsoc, fg->batt_temperature,
		    average_current, update_count);

	if (update_count < NVT1000_OTA_TRIGGER_COUNT) {
		schedule_delayed_work(&fg->ota_update_work,
			msecs_to_jiffies(NVT1000_OTA_MONITOR_INTERVAL));
		return;
	}

	mca_log_err("start nvt1000 ota\n");
	event_val = 1;
	mca_event_block_notify(MCA_EVENT_CHARGE_STATUS,
			       MCA_EVENT_FG_OTA_PROCESS, &event_val);
	platform_fg_ops_ota_update_check(FG_IC_MASTER);
	event_val = 0;
	mca_event_block_notify(MCA_EVENT_CHARGE_STATUS,
			       MCA_EVENT_FG_OTA_PROCESS, &event_val);
	mca_log_err("end nvt1000 ota\n");
}

/* Over-current, under-voltage and short-circuit counters from the gauge. */
enum batt_abnormal_info_ele {
	BATT_ABNORMAL_OCD,
	BATT_ABNORMAL_OCD_DATA,
	BATT_ABNORMAL_HOCD,
	BATT_ABNORMAL_HOCD_DATA,
	BATT_ABNORMAL_CUV,
	BATT_ABNORMAL_CUV_DATA,
	BATT_ABNORMAL_HCUV,
	BATT_ABNORMAL_HCUV_DATA,
	BATT_ABNORMAL_HSCD,
	BATT_ABNORMAL_HSCD_DATA,
	BATT_ABNORMAL_MAX,
};

static void strategy_batt_abnormal_dfx_work(struct work_struct *work)
{
	struct strategy_fg *fg = container_of(to_delayed_work(work),
					      struct strategy_fg,
					      batt_abnormal_dfx_work);
	int info[BATT_ABNORMAL_MAX] = { 0 };
	int last_ocd = 0, last_hocd = 0, last_cuv = 0, last_hcuv = 0;
	int last_hscd = 0;
	int report[7] = { 0 };

	platform_fg_ops_get_batt_abnormal_info(FG_IC_MASTER, info);
	charger_partition_read_ocd_count(&last_ocd, &last_hocd);
	charger_partition_read_cuv_count(&last_cuv, &last_hcuv);
	charger_partition_read_hscd_count(&last_hscd);

	mca_log_info("last_ocd_count:%d, last_hocd_count:%d, last_cuv_count:%d, last_hcuv_count:%d, last_hscd_count:%d\n",
		     last_ocd, last_hocd, last_cuv, last_hcuv, last_hscd);
	mca_log_info("ocd_count:%d, hocd_count:%d, cuv_count:%d, hcuv_count:%d, hscd_count:%d\n",
		     info[BATT_ABNORMAL_OCD], info[BATT_ABNORMAL_HOCD],
		     info[BATT_ABNORMAL_CUV], info[BATT_ABNORMAL_HCUV],
		     info[BATT_ABNORMAL_HSCD]);

	if (last_ocd != info[BATT_ABNORMAL_OCD] ||
	    last_hocd != info[BATT_ABNORMAL_HOCD]) {
		memcpy(report, &info[BATT_ABNORMAL_OCD], 4 * sizeof(int));
		report[4] = fg->batt_ui_soc;
		report[5] = fg->batt_voltage;
		report[6] = fg->batt_temperature;
		mca_charge_mievent_report(CHARGE_DFX_BATTERY_OCD, report, 7);
		charger_partition_write_ocd_count(info[BATT_ABNORMAL_OCD],
						  info[BATT_ABNORMAL_HOCD]);
	}

	if (last_cuv != info[BATT_ABNORMAL_CUV] ||
	    last_hcuv != info[BATT_ABNORMAL_HCUV]) {
		memcpy(report, &info[BATT_ABNORMAL_CUV], 4 * sizeof(int));
		report[4] = fg->batt_ui_soc;
		report[5] = fg->batt_voltage;
		report[6] = fg->batt_temperature;
		mca_charge_mievent_report(CHARGE_DFX_BATTERY_CUV, report, 7);
		charger_partition_write_cuv_count(info[BATT_ABNORMAL_CUV],
						  info[BATT_ABNORMAL_HCUV]);
	}

	if (last_hscd != info[BATT_ABNORMAL_HSCD]) {
		report[0] = info[BATT_ABNORMAL_HSCD];
		report[1] = info[BATT_ABNORMAL_HSCD_DATA];
		mca_charge_mievent_report(CHARGE_DFX_BATTERY_SCD, report, 2);
		charger_partition_write_hcsd_count(info[BATT_ABNORMAL_HSCD]);
	}
}

static int strategy_fg_get_parallel_rsoc(struct strategy_fg *fg, int *rsoc)
{
	int master_fcc = 0, slave_fcc = 0, master_weight;
	int r1, r2;

	r1 = platform_fg_ops_get_fcc(FG_IC_MASTER, &master_fcc);
	r2 = platform_fg_ops_get_fcc(FG_IC_SLAVE, &slave_fcc);
	if (r1 || r2)
		return -1;

	r1 = platform_fg_ops_get_rsoc(FG_IC_MASTER,
				      &fg->master_batt_info.rsoc);
	r2 = platform_fg_ops_get_rsoc(FG_IC_SLAVE, &fg->slave_batt_info.rsoc);
	if (r1 || r2)
		return -1;

	master_weight = master_fcc + slave_fcc ?
			master_fcc * 100 / (master_fcc + slave_fcc) : 0;
	*rsoc = (fg->slave_batt_info.rsoc * (100 - master_weight) +
		 fg->master_batt_info.rsoc * master_weight + 50) / 100;
	return 0;
}

static int strategy_fg_ops_get_rsoc(void *data, int *rsoc)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return -1;

	switch (fg->cfg.fg_type) {
	case MCA_FG_TYPE_SINGLE:
		return platform_fg_ops_get_rsoc(FG_IC_MASTER, rsoc);
	case MCA_FG_TYPE_PARALLEL:
		return strategy_fg_get_parallel_rsoc(fg, rsoc);
	case MCA_FG_TYPE_SERIES:
		return 0;
	default:
		return -1;
	}
}

static int strategy_fg_ops_get_soc(void *data)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg)
		return STRATEGY_FG_ERROR_FAKE_SOC;
	if (fg->fake_soc != STRATEGY_FG_FAKE_SOC_NONE)
		return fg->fake_soc;
	if (!fg->fg_init_flag)
		return STRATEGY_FG_ERROR_FAKE_SOC;

	return fg->batt_ui_soc;
}

static int strategy_fg_ops_get_temp(void *data, int *temp)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		*temp = STRATEGY_FG_ERROR_FAKE_TEMP;
	else
		*temp = fg->batt_temperature;

	return 0;
}

static int strategy_fg_ops_get_thermal_temp(void *data, int *temp)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		*temp = STRATEGY_FG_ERROR_FAKE_TEMP;
	else if (!fg->cfg.support_pnc)
		*temp = fg->batt_temperature;
	else
		*temp = fg->original_temp;

	return 0;
}

static int strategy_fg_ops_get_curr(void *data, int *curr)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;
	int r1, r2;

	if (!fg || !fg->fg_init_flag) {
		*curr = STRATEGY_FG_ERROR_FAKE_CURR;
		return 0;
	}

	/* At the fast period the last reading is fresh enough. */
	if (fg->update_interval == STRATEGY_FG_WORK_INTERVAL_FAST) {
		*curr = fg->batt_current;
		return 0;
	}

	switch (fg->cfg.fg_type) {
	case MCA_FG_TYPE_SINGLE:
		return platform_fg_ops_get_curr(FG_IC_MASTER, curr);
	case MCA_FG_TYPE_PARALLEL:
		r1 = platform_fg_ops_get_curr(FG_IC_SLAVE,
					      &fg->slave_batt_info.curr);
		r2 = platform_fg_ops_get_curr(FG_IC_MASTER,
					      &fg->master_batt_info.curr);
		if (r1 || r2)
			return -1;
		*curr = fg->master_batt_info.curr + fg->slave_batt_info.curr;
		return 0;
	case MCA_FG_TYPE_SERIES:
		return 0;
	default:
		return -1;
	}
}

static int strategy_fg_ops_get_volt(void *data, int *volt)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag) {
		*volt = STRATEGY_FG_ERROR_FAKE_VOLT;
		return 0;
	}

	if (fg->update_interval != STRATEGY_FG_WORK_INTERVAL_FAST) {
		if (fg->cfg.fg_type == MCA_FG_TYPE_PARALLEL) {
			platform_fg_ops_get_volt(FG_IC_MASTER,
						 &fg->master_batt_info.volt);
			if (!platform_fg_ops_get_volt(FG_IC_SLAVE,
						      &fg->slave_batt_info.volt))
				fg->batt_info.volt =
					max(fg->master_batt_info.volt,
					    fg->slave_batt_info.volt);
		} else if (fg->cfg.fg_type == MCA_FG_TYPE_SINGLE) {
			platform_fg_ops_get_volt(FG_IC_MASTER, &fg->batt_voltage);
		}
	}

	*volt = fg->batt_voltage;
	return 0;
}

static int strategy_fg_ops_get_cycle(void *data, int *cycle)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return -1;

	*cycle = fg->batt_cyclecount;
	return 0;
}

static int strategy_fg_ops_get_model_name(void *data, const char **model_name)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	const char *device_name = NULL;

	if (!fg || !fg->fg_init_flag)
		return -1;

	if (!strcmp(fg->cfg.model_name, fg->cfg.model_name_gl)) {
		*model_name = fg->cfg.model_name;
		goto country_select;
	}

	/* Which pack is fitted decides between the CN and global names. */
	platform_fg_ops_get_device_name(FG_IC_MASTER, &device_name);
	if (device_name && !strncmp(device_name, "BP4U", 4)) {
		*model_name = fg->cfg.model_name;
		mca_log_info("project O3 matched BP4U, set cn model name\n");
	} else if (device_name && (!strncmp(device_name, "BP4V", 4) ||
				   !strcmp("5XM31", device_name) ||
				   !strncmp("5@BP", device_name, 4))) {
		*model_name = fg->cfg.model_name_gl;
		mca_log_info("Not cn project, set global model name\n");
	} else {
		*model_name = fg->cfg.model_name;
	}

country_select:
	if (!fg->cfg.show_model_by_country || !hwid)
		return 0;

	if (hwid->country_version == CountryCN)
		*model_name = fg->cfg.model_name;
	else if (hwid->country_version == CountryIndia)
		*model_name = fg->cfg.model_name_in;
	else
		*model_name = fg->cfg.model_name_gl;

	return 0;
}

int strategy_lossless_rechg_parse_dt(struct strategy_fg *fg)
{
	struct device_node *node = fg->dev->of_node;
	int idata[3] = { 0 };
	int ret;

	ret = mca_parse_dts_u32(node, "support-lossless-rechg",
				&fg->cfg.support_lossless_rechg, 0);
	if (ret < 0) {
		mca_log_err("parse support-lossless-rechg failed %d\n", ret);
		return ret;
	}

	if (fg->cfg.support_lossless_rechg) {
		ret = mca_parse_dts_u32_array(node, "lossless_rechg_cfg",
					      idata, 3);
		if (ret < 0) {
			mca_log_err("parse lossless-rechg_cfg failed %d\n", ret);
			return ret;
		}
		fg->cfg.rechg_cfg.temp_low = idata[0];
		fg->cfg.rechg_cfg.temp_high = idata[1];
		fg->cfg.rechg_cfg.rest_time = idata[2];
	}

	fg->cfg.support_lossless_curr_limit =
		of_property_read_bool(node, "support-lossless-curr-limit");
	if (fg->cfg.support_lossless_curr_limit)
		ret = mca_parse_dts_u32(node, "lossless_curr_limit",
					&fg->cfg.lossless_curr_limit, 0);

	return ret;
}

int strategy_full_design_parse_dt(struct strategy_fg *fg)
{
	struct device_node *node = fg->dev->of_node;
	struct mca_hwid_info *hwid = mca_get_hwid_info();

	fg->support_full_design_gl =
		of_property_read_bool(node, "support_full_design_gl");
	mca_parse_dts_u32(node,
			  hwid && fg->support_full_design_gl &&
			  hwid->country_version != CountryCN ?
			  "charge-full-design-gl" : "charge-full-design",
			  &fg->cfg.design_capacity, DEFAULT_DESIGN_CAPACITY);

	return 0;
}

static int strategy_fg_parse_extreme_cold_dt(struct strategy_fg *fg)
{
	int data[FG_EXTREME_COLD_MAX_GROUP * FG_EXTREME_COLD_SIZE] = { 0 };
	struct fg_extreme_cold_para *para;
	int len, i;

	len = mca_parse_dts_string_array(fg->dev->of_node,
					 "extreme_cold_temp_compemsation", data,
					 FG_EXTREME_COLD_MAX_GROUP,
					 FG_EXTREME_COLD_SIZE);
	if (len < 0) {
		mca_log_err("parse extreme_cold_temp_compemsation failed\n");
		return -1;
	}

	fg->cfg.extreme_cold_para_size = len / FG_EXTREME_COLD_SIZE;
	for (i = 0; i < fg->cfg.extreme_cold_para_size; i++) {
		para = &fg->cfg.extreme_cold_para[i];
		para->cycle_count_max =
			data[i * FG_EXTREME_COLD_SIZE + FG_EXTREME_COLD_CYCLE_COUNT_MAX];
		para->temp_max =
			data[i * FG_EXTREME_COLD_SIZE + FG_EXTREME_COLD_TEMP_MAX];
		para->compensation =
			data[i * FG_EXTREME_COLD_SIZE + FG_EXTREME_COLD_COMPENSATION];
		mca_log_err("extreme_cold_para[%d]: %d, %d, %d\n", i,
			    para->cycle_count_max, para->temp_max,
			    para->compensation);
	}

	return 0;
}

#define TERM_CURR_PARA_MAX_GROUP 5
enum mca_fg_term_curr_para_ele {
	MCA_FG_TEMP = 0,
	MCA_FG_TEMP_HYS,
	MCA_FG_NORMAL_ITERM,
	MCA_FG_FFC_ITERM,
	MCA_FG_ITERM_PARA_MAX,
};

static int strategy_parse_term_curr_para(struct device_node *node,
					 int batt_role, const char *name,
					 struct strategy_fg *fg)
{
	struct term_curr_para *iterm_para = &fg->cfg.term_curr_data[batt_role];
	struct term_curr_data *iterm_data;
	const char *tmp_string = NULL;
	int array_len, row, i;
	int *field;

	array_len = mca_parse_dts_count_strings(node, name,
						TERM_CURR_PARA_MAX_GROUP,
						MCA_FG_ITERM_PARA_MAX);
	if (array_len < 0) {
		mca_log_err("parse %s failed\n", name);
		return -1;
	}

	iterm_para->iterm_para_size = array_len / MCA_FG_ITERM_PARA_MAX;
	iterm_data = kcalloc(iterm_para->iterm_para_size, sizeof(*iterm_data),
			     GFP_KERNEL);
	iterm_para->iterm_data = iterm_data;
	if (!iterm_data) {
		mca_log_err("iterm para no mem\n");
		return -ENOMEM;
	}

	for (i = 0; i < array_len; i++) {
		if (mca_parse_dts_string_index(node, name, i, &tmp_string))
			goto error;
		row = i / MCA_FG_ITERM_PARA_MAX;
		switch (i % MCA_FG_ITERM_PARA_MAX) {
		case MCA_FG_TEMP:
			field = &iterm_data[row].batt_temp;
			break;
		case MCA_FG_TEMP_HYS:
			field = &iterm_data[row].batt_temp_offset;
			break;
		case MCA_FG_NORMAL_ITERM:
			field = &iterm_data[row].normal_iterm;
			break;
		default:
			field = &iterm_data[row].ffc_iterm;
			break;
		}
		if (kstrtoint(tmp_string, 10, field))
			goto error;
	}

	for (i = 0; i < iterm_para->iterm_para_size; i++)
		mca_log_err("iterm para %d %d %d %d\n", iterm_data[i].batt_temp,
			    iterm_data[i].batt_temp_offset,
			    iterm_data[i].normal_iterm, iterm_data[i].ffc_iterm);

	return 0;

error:
	/* Nothing may be left pointing at a table that is gone. */
	kfree(iterm_data);
	iterm_para->iterm_data = NULL;
	iterm_para->iterm_para_size = 0;
	return -1;
}

#define ITERM_PARA_MAX_GROUP 3
enum mca_fg_iterm_para_ele {
	MCA_FG_BATT_ROLE = 0,
	MCA_FG_BATT_PARA_NAME,
	MCA_FG_BATT_PARA_MAX,
};

static int strategy_term_curr_parse_dt(struct strategy_fg *fg)
{
	struct device_node *node = fg->dev->of_node;
	const char *tmp_string = NULL;
	int array_len, i, batt_role = 0;

	array_len = mca_parse_dts_count_strings(node, "iterm_para",
						ITERM_PARA_MAX_GROUP,
						MCA_FG_BATT_PARA_MAX);
	if (array_len < 0) {
		mca_log_err("parse batt_para failed\n");
		return -1;
	}

	for (i = 0; i < array_len; i++) {
		if (mca_parse_dts_string_index(node, "iterm_para", i,
					       &tmp_string))
			return -1;

		if (i % MCA_FG_BATT_PARA_MAX == MCA_FG_BATT_ROLE) {
			if (kstrtoint(tmp_string, 10, &batt_role))
				return -1;
			/* A role this driver does not have: skip its table. */
			if (batt_role > FG_IC_SLAVE)
				i++;
		} else {
			if (batt_role < 0 || batt_role > FG_IC_SLAVE)
				return -1;
			if (strategy_parse_term_curr_para(node, batt_role,
							  tmp_string, fg))
				return -1;
		}
	}

	if (!fg->cfg.term_curr_data[FG_IC_MASTER].iterm_data) {
		mca_log_err("parse master iterm para failed\n");
		return -1;
	}
	if (!fg->cfg.term_curr_data[FG_IC_SLAVE].iterm_data) {
		mca_log_err("parse slave iterm para failed\n");
		return -1;
	}

	return 0;
}

/*
 * Returns a negative error only for what the driver cannot run without; the
 * optional parts log and leave their features off.
 */
static int strategy_fg_parse_dt(struct strategy_fg *fg)
{
	struct device_node *node = fg->dev->of_node;
	int soc_decimal[SOC_DECIMAL_MAX_LEVEL * SOC_DECIMAL_MAX] = { 0 };
	int ret, len, i;

	if (!node) {
		mca_log_err("No DT data Failing Probe\n");
		return -EINVAL;
	}

	ret = mca_parse_dts_u32(node, "report-full-rsoc",
				&fg->cfg.report_full_raw_soc,
				STRATEGY_FG_REPORT_FULL_RSOC);
	ret |= mca_parse_dts_u32(node, "soc-proportion",
				 &fg->cfg.soc_proportion,
				 STRATEGY_FG_SOC_PROPORTION);
	ret |= mca_parse_dts_u32(node, "soc-proportion-c",
				 &fg->cfg.soc_proportion_c,
				 STRATEGY_FG_SOC_PROPORTION_C);
	ret |= mca_parse_dts_u32(node, "fg_type", &fg->cfg.fg_type,
				 MCA_FG_TYPE_SINGLE);
	ret |= mca_parse_dts_u32(node, "adapt_power", &fg->cfg.adapt_power, 0);
	ret |= mca_parse_dts_u32(node, "support_dtpt", &fg->cfg.support_dtpt,
				 0);
	mca_parse_dts_u32(node, "cool_threshold", &fg->cfg.cool_threshold,
			  STRATEGY_FG_COOL_THRESHOLD);
	mca_parse_dts_u32(node, "force_full_hys", &fg->cfg.force_full_hys,
			  STRATEGY_FG_FORCE_FULL_HYS);
	mca_parse_dts_u32(node, "force_full_high_hys",
			  &fg->cfg.force_full_high_hys,
			  STRATEGY_FG_FORCE_FULL_HYS);
	mca_parse_dts_u32(node, "fg_hightemp_vterm", &fg->cfg.fg_hightemp_vterm,
			  STRATEGY_FG_HIGH_TEMP_VTERM);
	mca_parse_dts_u32(node, "vterm_delta", &fg->cfg.vterm_delta,
			  STRATEGY_FG_VTERM_DELTA);
	mca_parse_dts_u32(node, "support_nvt1000_ota",
			  &fg->cfg.support_nvt1000_ota, 0);
	mca_parse_dts_u32(node, "support_batt_xvdd_dfx",
			  &fg->cfg.support_batt_xvdd_dfx, 0);
	ret |= mca_parse_dts_u32(node, "terminated_by_cp",
				 &fg->cfg.terminated_by_cp, 0);
	ret |= mca_parse_dts_u32(node, "clear_CC_per_hundred",
				 &fg->cfg.clear_CC_per_hundred, 1);
	fg->cfg.show_model_by_country =
		of_property_read_bool(node, "show-model-by-country-version");
	ret |= mca_parse_dts_string(node, "model-name", &fg->cfg.model_name);
	ret |= mca_parse_dts_string(node, "model-name-global",
				    &fg->cfg.model_name_gl);
	ret |= mca_parse_dts_string(node, "model-name-in",
				    &fg->cfg.model_name_in);
	ret |= mca_parse_dts_u32(node, "fast_force_full_vol_delta",
				 &fg->cfg.fast_force_full_vol_delta,
				 STRATEGY_FG_FAST_FORCE_FULL_VOL_DELTA);
	fg->support_normal_force_full =
		of_property_read_bool(node, "support_normal_force_full");
	if (fg->support_normal_force_full)
		ret |= mca_parse_dts_u32(node, "normal_force_full_hys",
					 &fg->cfg.normal_force_full_hys, 0);
	if (ret)
		mca_log_err("strategy fg parse dt failed, ret=%d\n", ret);

	mca_parse_dts_u32(node, "support_fl4p0", &fg->cfg.support_fl4p0, 0);
	if (fg->cfg.support_fl4p0) {
		len = mca_parse_dts_u32_count(node, "fl4p0_ffc_safe_iterm",
					      SAFE_ITERM_CYCLE_LEVEL,
					      SAFE_ITERM_TEMP_LEVEL);
		if (len < 0) {
			mca_log_err("parse fl4p0_ffc_safe_iterm failed\n");
			return -1;
		}
		if (mca_parse_dts_u32_array(node, "fl4p0_ffc_safe_iterm",
					    &fg->cfg.ffc_safe_item[0][0],
					    len) < 0)
			return -1;
	}

	len = mca_parse_dts_u32_count(node, "soc_decimal",
				      SOC_DECIMAL_MAX_LEVEL, SOC_DECIMAL_MAX);
	if (len < 0) {
		mca_log_err("parse soc_decimal len failed\n");
		return -1;
	}
	if (mca_parse_dts_u32_array(node, "soc_decimal", soc_decimal, len) < 0) {
		mca_log_err("parse soc_decimal cfg failed\n");
		return -1;
	}
	fg->cfg.soc_decimal_cnt = len / SOC_DECIMAL_MAX;
	for (i = 0; i < fg->cfg.soc_decimal_cnt; i++) {
		fg->cfg.soc_decimal[i].soc =
			soc_decimal[i * SOC_DECIMAL_MAX + SOC_DECIMAL_SOC];
		fg->cfg.soc_decimal[i].rate =
			soc_decimal[i * SOC_DECIMAL_MAX + SOC_DECIMAL_RATE];
	}

	ret = mca_battery_shutdown_parse_dt(fg);
	if (ret < 0) {
		mca_log_err("parse battery shutdown failed %d\n", ret);
		return ret;
	}

	ret = strategy_lossless_rechg_parse_dt(fg);
	if (ret < 0) {
		mca_log_err("parse lossless recharging failed %d\n", ret);
		return 0;
	}

	strategy_full_design_parse_dt(fg);

	ret = strategy_soc_smooth_parse_dt(fg);
	if (ret < 0) {
		mca_log_err("parse soc smooth failed %d\n", ret);
		fg->smooth.suppot_RSOC_0_smooth = 0;
		return 0;
	}

	ret = strategy_fg_parse_extreme_cold_dt(fg);
	if (ret < 0) {
		mca_log_err("parse extreme cold dt failed %d\n", ret);
		return 0;
	}

	fg->cfg.support_base_flip =
		of_property_read_bool(node, "support-base-flip");
	fg->cfg.base_flip_same = of_property_read_bool(node, "base-flip-same");
	fg->cfg.support_pmic_vterm_dynamics_adjust =
		of_property_read_bool(node, "support-pmic-vterm-dynamics-adjust");
	if (fg->cfg.fg_type != MCA_FG_TYPE_SINGLE) {
		ret = strategy_term_curr_parse_dt(fg);
		if (ret < 0) {
			mca_log_err("parse iterm failed %d\n", ret);
			return 0;
		}
	}
	fg->cfg.support_pnc = of_property_read_bool(node, "support-pnc");
	fg->cfg.wr_for_sw_cv_wake =
		of_property_read_bool(node, "wr-for-sw-cv-wake");
	fg->cfg.support_qbg = of_property_read_bool(node, "support-qbg");

	return 0;
}

/*
 * The fraction shown after the percentage: how far into the current percent
 * the pack is, scaled down at the high end so it never runs past 99.
 */
#define STRATEGY_FG_SOC_DECIMAL_DEFAULT_RATE 10
static int strategy_fg_ops_get_soc_decimal(void *data, int *soc_decimal,
					   int *rate)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;
	int i;

	if (!fg)
		return -1;

	*rate = STRATEGY_FG_SOC_DECIMAL_DEFAULT_RATE;
	for (i = fg->cfg.soc_decimal_cnt - 1; i >= 0; i--) {
		if (fg->batt_ui_soc > fg->cfg.soc_decimal[i].soc) {
			*rate = fg->cfg.soc_decimal[i].rate;
			break;
		}
	}
	*soc_decimal = fg->batt_ui_soc_decimal * (100 - *rate) / 100 - 1;

	return 0;
}

static bool strategy_fg_ops_get_charging_done(void *data)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return true;

	return fg->charging_done;
}

static int strategy_fg_ops_set_charging_done(void *data, bool charging_done)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return -1;

	mca_log_info("set charging_done: %d\n", charging_done);
	if (!charging_done) {
		fg->charging_done = false;
		fg->en_smooth_full = false;
		fg->charging_done_time = 0;
		return 0;
	}

	mca_vote(fg->en_voter, "term_recharge", true, 0);
	fg->charging_done = true;
	fg->recharging = false;
	fg->lossless_recharge = false;
	fg->near_vterm = false;
	fg->ffc_continue_charge = false;
	if (fg->is_eu_model) {
		/* An EU pack is walked to 100% rather than jumped there. */
		if (fg->batt_rsoc < 100) {
			fg->en_smooth_full = true;
			mca_log_info("charging_done start en_smooth_full\n");
		} else {
			fg->en_smooth_full = false;
			mca_log_info("charging_done stop en_smooth_full\n");
		}
		fg->keep_full_flag = false;
	} else {
		fg->keep_full_flag = true;
	}
	if (fg->cfg.fg_type != MCA_FG_TYPE_SINGLE)
		strategy_fg_reset_co_to_default(fg);

	fg->charging_done_time = ktime_get_boottime_seconds();
	mca_log_info("charging_done_time: %lld\n", fg->charging_done_time);

	return 0;
}

/* Whether the fitted pack is rated for this phone's charging power. */
#define ADAPT_POWER_BASE_FLIP_MASTER	55
#define ADAPT_POWER_BASE_FLIP_SLAVE	15
#define ADAPT_POWER_BASE_FLIP_SAME	33
#define ADAPT_POWER_BASE_FLIP_TOTAL	67

static void strategy_fg_check_battery_adapt_power(struct strategy_fg *fg,
						  int *match)
{
	int adapt_power = 0, adapt_power_slave = 0;

	if (fg->fake_bap_match >= 0) {
		*match = fg->fake_bap_match;
		goto out;
	}

	platform_fg_ops_get_adapt_power(FG_IC_MASTER, &adapt_power);
	if (fg->cfg.fg_type != MCA_FG_TYPE_SINGLE) {
		platform_fg_ops_get_adapt_power(FG_IC_SLAVE, &adapt_power_slave);
		if (fg->cfg.support_base_flip) {
			if (adapt_power == ADAPT_POWER_BASE_FLIP_MASTER &&
			    adapt_power_slave == ADAPT_POWER_BASE_FLIP_SLAVE)
				adapt_power = ADAPT_POWER_BASE_FLIP_TOTAL;
		} else if (fg->cfg.base_flip_same) {
			if (adapt_power == ADAPT_POWER_BASE_FLIP_SAME &&
			    adapt_power_slave == ADAPT_POWER_BASE_FLIP_SAME)
				adapt_power = ADAPT_POWER_BASE_FLIP_TOTAL;
		} else if (adapt_power != adapt_power_slave) {
			adapt_power = -1;
		}
	}
	mca_log_info("check battery adapt_power: %d, read value %d\n",
		     fg->cfg.adapt_power, adapt_power);
	*match = fg->cfg.adapt_power == adapt_power;

out:
	mca_log_info("%s: fake_bap_match: %d, match %d\n", __func__,
		     fg->fake_bap_match, *match);
}

static int strategy_fg_ops_set_fastcharge(void *data, bool en)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;
	int ret;

	if (!fg || !fg->fg_init_flag)
		return -1;

	ret = platform_fg_ops_set_fastcharge(FG_IC_MASTER, en);
	if (fg->cfg.fg_type != MCA_FG_TYPE_SINGLE)
		ret |= platform_fg_ops_set_fastcharge(FG_IC_SLAVE, en);

	if (!en && fg->cfg.fg_type != MCA_FG_TYPE_SINGLE &&
	    mca_is_override_vote_enabled(fg->vterm_voter)) {
		mca_vote_override(fg->vterm_voter, "para_term", false, 0);
		mca_log_err("cancel para_term vote\n");
	}

	return ret;
}

static int strategy_fg_ops_get_fastcharge(void *data)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;
	int ffc = 0, slave_ffc = 0;

	if (!fg || !fg->fg_init_flag)
		return 0;

	if (fg->update_interval == STRATEGY_FG_WORK_INTERVAL_FAST)
		return fg->fast_charge;

	platform_fg_ops_get_fastcharge(FG_IC_MASTER, &ffc);
	if (fg->cfg.fg_type == MCA_FG_TYPE_SINGLE)
		return ffc;

	platform_fg_ops_get_fastcharge(FG_IC_SLAVE, &slave_ffc);
	return ffc && slave_ffc;
}

static int strategy_fg_ops_get_authentic(void *data, bool *authentic)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;
	int master_auth = 1, slave_auth = 1;
	int ret;

	if (!fg || !fg->fg_init_flag)
		return -1;

	ret = platform_fg_ops_get_authentic(FG_IC_MASTER, &master_auth);
	if (fg->cfg.fg_type != MCA_FG_TYPE_SINGLE)
		ret |= platform_fg_ops_get_authentic(FG_IC_SLAVE, &slave_auth);
	if (!ret)
		*authentic = master_auth && slave_auth;

	return ret;
}

static int strategy_fg_ops_get_dc(void *data, int *dc)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return -1;

	*dc = fg->batt_dc;
	return 0;
}

static int strategy_fg_ops_get_rm(void *data, int *rm)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return -1;

	*rm = fg->batt_rm;
	return 0;
}

static int strategy_fg_ops_get_fcc(void *data, int *fcc)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return -1;

	*fcc = fg->batt_fcc;
	return 0;
}

static int strategy_fg_ops_get_health(void *data, int *health)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return -1;

	*health = fg->batt_health;
	return 0;
}

static int strategy_fg_ops_get_soh(void *data, int *soh)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return -1;

	*soh = fg->batt_soh;
	return 0;
}

int strategy_fg_ops_is_init_ok(void *data)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return -1;

	return 1;
}

int strategy_fg_ops_is_chip_ok(void *data)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	return fg->fg_init_flag && !fg->fg_error;
}

int strategy_fg_dual_ops_is_chip_ok(void *data, int index)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (index != FG_IC_MASTER && index != FG_IC_SLAVE)
		return false;

	return fg->dual_present[index] && !fg->dual_error[index];
}

static int strategy_fg_ops_get_pack_vendor_id(void *data, int *vendor_id)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return -1;

	platform_fg_ops_get_pack_vendor(FG_IC_MASTER, vendor_id);
	return 0;
}

static int strategy_fg_ops_get_first_termiation(void *data, int *value)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return -1;

	*value = fg->first_termination;
	return 0;
}

static int strategy_fg_ops_get_temp_offset_flag(void *data, int *flag)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;

	if (!fg || !fg->fg_init_flag)
		return -1;

	*flag = fg->temp_offset_flag[FG_IC_MASTER] ||
		fg->temp_offset_flag[FG_IC_SLAVE];
	return 0;
}

static int strategy_fg_get_status(int status, int *value, void *data)
{
	struct strategy_fg *fg = (struct strategy_fg *)data;
	int match = 0;

	if (!value || !fg)
		return -1;

	switch (status) {
	case STRATEGY_STATUS_TYPE_AUTH_PASS:
		strategy_fg_check_battery_adapt_power(fg, &match);
		*value = match & fg->batt_auth;
		return 0;
	case STRATEGY_STATUS_TYPE_FORCE_FULL_STATUS:
		*value = fg->done_force_full;
		return 0;
	default:
		return -1;
	}
}

static const struct strategy_fg_class_ops g_strategy_fg_ops = {
	.strategy_fg_is_init_ok = strategy_fg_ops_is_init_ok,
	.strategy_fg_is_chip_ok = strategy_fg_ops_is_chip_ok,
	.strategy_fg_dual_is_chip_ok = strategy_fg_dual_ops_is_chip_ok,
	.strategy_fg_get_rsoc = strategy_fg_ops_get_rsoc,
	.strategy_fg_get_soc = strategy_fg_ops_get_soc,
	.strategy_fg_get_temp = strategy_fg_ops_get_temp,
	.strategy_fg_get_thermal_temp = strategy_fg_ops_get_thermal_temp,
	.strategy_fg_get_current = strategy_fg_ops_get_curr,
	.strategy_fg_get_voltage = strategy_fg_ops_get_volt,
	.strategy_fg_get_cycle = strategy_fg_ops_get_cycle,
	.strategy_fg_get_rm = strategy_fg_ops_get_rm,
	.strategy_fg_get_voltage_mean = strategy_fg_get_volt_mean,
	.strategy_fg_get_soc_decimal_info = strategy_fg_ops_get_soc_decimal,
	.strategy_fg_get_charging_done = strategy_fg_ops_get_charging_done,
	.strategy_fg_set_charging_done = strategy_fg_ops_set_charging_done,
	.strategy_fg_get_model_name = strategy_fg_ops_get_model_name,
	.strategy_fg_set_fastcharge = strategy_fg_ops_set_fastcharge,
	.strategy_fg_get_fastcharge = strategy_fg_ops_get_fastcharge,
	.strategy_fg_get_authentic = strategy_fg_ops_get_authentic,
	.strategy_fg_get_dc = strategy_fg_ops_get_dc,
	.strategy_fg_get_fcc = strategy_fg_ops_get_fcc,
	.strategy_fg_get_health = strategy_fg_ops_get_health,
	.strategy_fg_get_soh = strategy_fg_ops_get_soh,
	.strategy_fg_get_first_termiation = strategy_fg_ops_get_first_termiation,
	.strategy_fg_get_pack_vendor_id = strategy_fg_ops_get_pack_vendor_id,
	.strategy_fg_get_temp_offset_flag = strategy_fg_ops_get_temp_offset_flag,
};

static void delay_reset_full_flag_work(struct work_struct *work)
{
	struct strategy_fg *fg = container_of(work, struct strategy_fg,
					      delay_reset_full_flag_work.work);

	if (fg->fg_init_flag)
		fg->keep_full_flag = false;
}

#define EU_MODEL_RECHARGE_SOC 100

/* A supply has been attached. */
static void strategy_fg_power_present(struct strategy_fg *fg)
{
	fg->power_present = true;
	fg->plugin_time = ktime_get_boottime_seconds();
	fg->online_time = ktime_get_boottime_seconds();

	/* An EU pack not showing 100% is not finished. */
	if (fg->is_eu_model && fg->batt_ui_soc < EU_MODEL_RECHARGE_SOC) {
		mca_vote(fg->en_voter, "term_recharge", false, 0);
		fg->charging_done = false;
		mca_log_info("usb connect clear charging_done\n");
	}

	mca_log_info("plugin keep_full_flag: %d, plugin_time: %lld\n",
		     fg->keep_full_flag, fg->plugin_time);
	mca_mod_delayed_work(&fg->monitor_work,
			     msecs_to_jiffies(STRATEGY_FG_WORK_INTERVAL_FAST));
	cancel_delayed_work_sync(&fg->delay_reset_full_flag_work);
	if (fg->cfg.support_dtpt)
		schedule_delayed_work(&fg->dtpt_monitor_work,
			msecs_to_jiffies(STRATEGY_FG_WORK_INTERVAL_DTPT));
	if (fg->cfg.support_nvt1000_ota)
		cancel_delayed_work_sync(&fg->ota_update_work);
}

/* The last supply has gone away. */
static void strategy_fg_power_absent(struct strategy_fg *fg)
{
	u32 ota_flag = 0;

	if (!fg->is_eu_model) {
		mca_vote(fg->en_voter, "term_recharge", false, 0);
		fg->charging_done = false;
	} else if (fg->batt_ui_soc < EU_MODEL_RECHARGE_SOC) {
		mca_vote(fg->en_voter, "term_recharge", false, 0);
		fg->charging_done = false;
		mca_log_info("usb disonnect clear charging_done\n");
	}

	mca_vote(fg->iterm_voter, "calibration", false, 0);
	mca_vote(fg->charge_limit_voter, "losslessRec", false, 0);
	fg->recharging = false;
	fg->lossless_recharge = false;
	fg->power_present = false;
	fg->en_smooth_full = false;
	fg->charging_done_time = 0;
	fg->near_vterm = false;
	fg->last_final_term_curr = 0;

	mca_log_info("plugout keep_full_flag = %d\n", fg->keep_full_flag);
	if (fg->keep_full_flag)
		schedule_delayed_work(&fg->delay_reset_full_flag_work,
			msecs_to_jiffies(STRATEGY_FG_RESET_FULL_FLAG_MS));
	if (fg->cfg.support_dtpt)
		cancel_delayed_work_sync(&fg->dtpt_monitor_work);

	if (fg->cfg.fg_type != MCA_FG_TYPE_SINGLE) {
		/* The gauges are told over i2c, from a work of their own. */
		cancel_delayed_work_sync(&fg->reset_default_work);
		schedule_delayed_work(&fg->reset_default_work, 0);
		fg->monitor_soc_flag = false;
		fg->first_termination = false;
		fg->fg1_batt_ctr_enabled = false;
		fg->fg2_batt_ctr_enabled = false;
		fg->fg_lock_flag = false;
		fg->switch_pmic_term = false;
		mca_vote_override(fg->vterm_voter, "para_term", false, 0);
		mca_vote(fg->charge_limit_voter, "losslessRec", false, 0);
		mca_vote(fg->flip_charge_curr_voter, "losslessRec", false, 0);
		mca_vote(fg->flip_charge_curr_voter, "base_close_curr", false, 0);
	}

	if (fg->cfg.support_nvt1000_ota) {
		platform_fg_ops_get_ota_update_flag(FG_IC_MASTER, &ota_flag);
		if (ota_flag == NVT1000_OTA_UPDATE_FLAG) {
			cancel_delayed_work_sync(&fg->ota_update_work);
			schedule_delayed_work(&fg->ota_update_work,
				msecs_to_jiffies(NVT1000_OTA_MONITOR_INTERVAL));
		}
	}

	fg->slow_chg_start_time = 0;
	fg->non_std_charger_reported = false;
}

static int strategy_fg_process_event(int event, int value, void *data)
{
	struct strategy_fg *fg = data;
	struct mca_hwid_info *hwid = mca_get_hwid_info();

	if (!data)
		return -1;

	mca_log_info("receive event %d, value %d", event, value);
	switch (event) {
	case MCA_EVENT_USB_CONNECT:
		fg->usb_online = true;
		strategy_fg_power_present(fg);
		break;
	case MCA_EVENT_WIRELESS_CONNECT:
		fg->wls_online = true;
		strategy_fg_power_present(fg);
		break;
	case MCA_EVENT_USB_DISCONNECT:
		fg->usb_online = false;
		strategy_fg_power_absent(fg);
		break;
	case MCA_EVENT_WIRELESS_DISCONNECT:
		fg->wls_online = false;
		/* Losing the pad with the cable still in is not losing power. */
		if (fg->usb_online) {
			mca_log_info("wireless disocnnect usb present goto end\n");
			break;
		}
		strategy_fg_power_absent(fg);
		break;
	case MCA_EVENT_CHARGE_TYPE_CHANGE:
		fg->real_type = value;
		break;
	case MCA_EVENT_IS_EU_MODEL:
		/* A Chinese board is never one, whatever it is told. */
		if (hwid && hwid->country_version == CountryCN) {
			mca_log_info("country_version = CN\n");
			break;
		}
		if (fg->is_eu_model != value) {
			fg->is_eu_model = !!value;
			mca_log_info("is_eu_model[%d] update soc\n",
				     fg->is_eu_model);
		}
		break;
	case MCA_EVENT_BQ_FG_ERROR:
		fg->fg_error = true;
		fg_update_status(fg, true);
		break;
	case MCA_EVENT_DEBUG_CTRL_DOUBLE85:
	case MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT:
		/* The soak chamber keeps the pack where it would refuse to sit. */
		strategy_fg_set_fake_soc(fg, value ? STRATEGY_FG_SOAK_SOC :
					 STRATEGY_FG_FAKE_SOC_NONE);
		strategy_fg_set_fake_temp(fg, value ? STRATEGY_FG_SOAK_TEMP :
					  STRATEGY_FG_FAKE_TEMP_NONE);
		break;
	case MCA_EVENT_DEBUG_CTRL_MEMORY_TEST:
		strategy_fg_set_fake_temp(fg, value ? STRATEGY_FG_MEM_TEST_TEMP :
					  STRATEGY_FG_FAKE_TEMP_NONE);
		break;
	default:
		break;
	}

	return 0;
}

static void strategy_fg_init_voter(struct strategy_fg *fg)
{
	fg->voter_ok = false;

	fg->en_voter = mca_find_votable("chg_enable");
	if (!fg->en_voter)
		return;
	fg->vterm_voter = mca_find_votable("term_volt");
	if (!fg->vterm_voter)
		return;
	fg->iterm_voter = mca_find_votable("term_curr");
	if (!fg->iterm_voter)
		return;
	fg->charge_limit_voter = mca_find_votable("buck_charge_curr");
	if (!fg->charge_limit_voter)
		return;
	fg->flip_charge_curr_voter = mca_find_votable("flip_charge_curr");
	if (!fg->flip_charge_curr_voter)
		return;
	fg->buck_input_voter = mca_find_votable("buck_input");
	if (!fg->buck_input_voter)
		return;

	fg->voter_ok = true;
}

static int strategy_fg_panel_notifier_cb(struct notifier_block *nb,
					 unsigned long event, void *val)
{
	struct strategy_fg *fg = container_of(nb, struct strategy_fg, panel_nb);
	int status;

	if (event != MCA_EVENT_PANEL_SCREEN_STATE_CHANGE)
		return 0;

	status = *(int *)val;
	mca_log_info("update screen_state: %d => %d\n", fg->screen_status,
		     status);
	fg->screen_status = !!status;
	return 0;
}

#define NTC_SCALE_BOARDTEMP 100
static int strategy_fg_thermal_notifier_cb(struct notifier_block *nb,
					   unsigned long event, void *val)
{
	struct strategy_fg *fg = container_of(nb, struct strategy_fg,
					      thermal_board_nb);

	if (event == MCA_EVENT_THERMAL_BOARD_TEMP_CHANGE)
		fg->thermal_board_temp = *(int *)val / NTC_SCALE_BOARDTEMP;
	return 0;
}

static int strategy_fg_probe(struct platform_device *pdev)
{
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	struct strategy_fg *fg;
	int ret;

	mca_log_info("%s probe_begin\n", __func__);
	fg = devm_kzalloc(&pdev->dev, sizeof(*fg), GFP_KERNEL);
	if (!fg) {
		mca_log_err("out of memory\n");
		return -ENOMEM;
	}
	fg->dev = &pdev->dev;
	platform_set_drvdata(pdev, fg);
	fg->batt_ui_soc = STRATEGY_FG_ERROR_FAKE_SOC;

	ret = strategy_fg_parse_dt(fg);
	if (ret)
		return ret;

	fg->batt_psy = power_supply_get_by_name("battery");
	/* Only a board sold outside China can be an EU model. */
	if (hwid && hwid->country_version != CountryCN) {
		charger_partition_get_eu_model(&fg->is_eu_model);
		if (fg->is_eu_model)
			fg->boot_update_soc = true;
	}
	mca_log_info("probe is_eu_model[%d]\n", fg->is_eu_model);

	/* Set before the monitor and the class ops can see them. */
	fg->fake_bap_match = STRATEGY_FG_FAKE_BAP_MATCH_NONE;
	fg->fake_soc = STRATEGY_FG_FAKE_SOC_NONE;
	fg->fake_temp = STRATEGY_FG_FAKE_TEMP_NONE;
	fg->update_period = STRATEGY_FG_UPDATE_PERIOD_NONE;
	fg->self_equal_max_count = STRATEGY_FG_SELF_EQUAL_MAX_COUNT;
	fg->batt_health = POWER_SUPPLY_HEALTH_GOOD;
	fg->last_final_term_curr = 0;

	INIT_DELAYED_WORK(&fg->monitor_work, strategy_fg_monitor_workfunc);
	mca_queue_delayed_work(&fg->monitor_work, 0);
	ret = strategy_class_fg_ops_register(fg, &g_strategy_fg_ops);
	mca_sysfs_init_attrs(strategy_fg_sysfs_attrs,
			     strategy_fg_sysfs_field_tbl, FG_AUTH_ATTRS_SIZE);
	mca_sysfs_create_link_group("fuelgauge", "strategy_fg", fg->dev,
				    &strategy_fg_sysfs_attr_group);
	mca_strategy_ops_register(STRATEGY_FUNC_TYPE_FG,
				  strategy_fg_process_event,
				  strategy_fg_get_status, NULL, fg);
	INIT_DELAYED_WORK(&fg->delay_reset_full_flag_work,
			  delay_reset_full_flag_work);
	INIT_DELAYED_WORK(&fg->reset_default_work,
			  strategy_fg_reset_default_work);
	strategy_fg_init_voter(fg);
	INIT_DELAYED_WORK(&fg->dtpt_monitor_work,
			  strategy_fg_dtpt_monitor_work);
	INIT_DELAYED_WORK(&fg->fl4p0_calibration_work,
			  strategy_fl4p0_calibration_work);
	INIT_DELAYED_WORK(&fg->force_report_full_work,
			  strategy_force_report_full_work);
	INIT_DELAYED_WORK(&fg->ota_update_work,
			  strategy_nvt1000_ota_monitor_work);
	INIT_DELAYED_WORK(&fg->batt_abnormal_dfx_work,
			  strategy_batt_abnormal_dfx_work);

	fg->panel_nb.notifier_call = strategy_fg_panel_notifier_cb;
	mca_event_block_notify_register(MCA_EVENT_TYPE_PANEL, &fg->panel_nb);
	fg->thermal_board_nb.notifier_call = strategy_fg_thermal_notifier_cb;
	mca_event_block_notify_register(MCA_EVENT_TYPE_THERMAL_TEMP,
					&fg->thermal_board_nb);

	if (fg->cfg.support_nvt1000_ota)
		schedule_delayed_work(&fg->ota_update_work,
			msecs_to_jiffies(NVT1000_OTA_MONITOR_INTERVAL));
	if (fg->cfg.support_batt_xvdd_dfx)
		schedule_delayed_work(&fg->batt_abnormal_dfx_work,
			msecs_to_jiffies(STRATEGY_FG_ABNORMAL_DFX_DELAY_MS));

	mca_log_info("%s probe end\n", __func__);
	return ret;
}

static int strategy_fg_remove(struct platform_device *pdev)
{
	struct strategy_fg *fg = platform_get_drvdata(pdev);

	mca_sysfs_remove_link_group("fuelgauge", "strategy_fg", &pdev->dev,
				    &strategy_fg_sysfs_attr_group);
	if (!fg)
		return 0;

	/*
	 * The class registrations, notifier chains and works point into
	 * memory that goes away with the device.
	 */
	strategy_class_fg_ops_register(NULL, NULL);
	mca_strategy_ops_register(STRATEGY_FUNC_TYPE_FG, NULL, NULL, NULL, NULL);
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_PANEL, &fg->panel_nb);
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_THERMAL_TEMP,
					  &fg->thermal_board_nb);
	cancel_delayed_work_sync(&fg->monitor_work);
	cancel_delayed_work_sync(&fg->ota_update_work);
	cancel_delayed_work_sync(&fg->dtpt_monitor_work);
	cancel_delayed_work_sync(&fg->reset_default_work);
	cancel_delayed_work_sync(&fg->delay_reset_full_flag_work);
	cancel_delayed_work_sync(&fg->force_report_full_work);
	cancel_delayed_work_sync(&fg->fl4p0_calibration_work);
	cancel_delayed_work_sync(&fg->batt_abnormal_dfx_work);

	return 0;
}

static int strategy_fg_suspend(struct platform_device *pdev,
			       pm_message_t state)
{
	struct strategy_fg *fg = platform_get_drvdata(pdev);
	ktime_t time_now = ktime_get_boottime();

	if (!fg)
		return -1;

	if (ktime_ms_delta(time_now, fg->suspend_time) >
	    STRATEGY_FG_RESUME_UPDATE_TIME_MS)
		fg->suspend_time = time_now;

	cancel_delayed_work_sync(&fg->monitor_work);
	return 0;
}

static int strategy_fg_resume(struct platform_device *pdev)
{
	struct strategy_fg *fg = platform_get_drvdata(pdev);

	if (!fg)
		return -1;

	/* After a long sleep the reading is shown without smoothing. */
	fg->resume_update_soc = ktime_ms_delta(ktime_get_boottime(),
					       fg->suspend_time) >
				STRATEGY_FG_RESUME_UPDATE_TIME_MS;
	mca_queue_delayed_work(&fg->monitor_work,
			       msecs_to_jiffies(fg->resume_update_soc ? 0 :
						STRATEGY_FG_WORK_INTERVAL_SCREEN_ON));
	return 0;
}

static void strategy_fg_shutdown(struct platform_device *pdev)
{
	struct strategy_fg *fg = platform_get_drvdata(pdev);

	mca_log_err("battery shutdown, soc: %d, rsoc: %d, vbat: %d, vbat_mean: %d, empty: %d\n",
		    fg->batt_ui_soc, fg->batt_rsoc, fg->batt_voltage,
		    fg->batt_voltage_mean, fg->vbatt_empty);
}

static const struct of_device_id match_table[] = {
	{ .compatible = "xiaomi,strategy_fg" },
	{},
};

static struct platform_driver strategy_fg_driver = {
	.driver = {
		.name = "strategy_fg",
		.of_match_table = match_table,
	},
	.probe = strategy_fg_probe,
	.remove = strategy_fg_remove,
	.suspend = strategy_fg_suspend,
	.resume = strategy_fg_resume,
	.shutdown = strategy_fg_shutdown,
};

static int __init strategy_fg_init(void)
{
	return platform_driver_register(&strategy_fg_driver);
}
module_init(strategy_fg_init);

static void __exit strategy_fg_exit(void)
{
	platform_driver_unregister(&strategy_fg_driver);
}
module_exit(strategy_fg_exit);

MODULE_DESCRIPTION("Strategy Fuel Gauge");
MODULE_AUTHOR("liweiwei9@xiaomi.com");
MODULE_LICENSE("GPL v2");
