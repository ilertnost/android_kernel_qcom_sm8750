// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * JEITA charging limits for the buck charger.  See inc/mca_buckchg_jeita.h.
 *
 * A lithium cell will take its full current only over a narrow band of
 * temperatures.  Outside it the cell is charged more slowly and to a lower
 * float voltage, and outside that not at all.  Every few seconds while an
 * adapter is attached this looks up the band the battery is in, caps the
 * current further by the battery voltage, and votes the result.
 *
 * Both lookups have hysteresis: a band is only left once the temperature has
 * gone past it by the margin the band gives, and a voltage step is only
 * taken back once the voltage has moved past it, so that the charger does not
 * spend its time changing its mind.
 *
 * A foldable has a second cell.  When the board says so, the base and flip
 * cells are looked at through their own fuel gauges as well: the flip cell's
 * limit is applied through its load switch, and the base cell's measured
 * current is watched so that the charger can be pulled down if it is
 * overrunning the limit.
 */

#define MCA_LOG_TAG "mca_buckchg_jeita"

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/hwid.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "inc/mca_buckchg_jeita.h"
#include <mca/common/mca_event.h>
#include <mca/common/mca_hwid.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/common/mca_voter.h>
#include <mca/platform/platform_fg_ic_ops.h>
#include <mca/platform/platform_loadsw_class.h>
#include <mca/smartchg/smart_chg_class.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>

/* How long after probe to look for the elections when nothing is attached. */
#define JEITA_PROBE_DELAY_MS		1000

/* How long to wait before probing again for the tables, and how often. */
#define JEITA_PARSE_RETRY_DELAY_MS	100
#define JEITA_PARSE_RETRY_MAX		50

/*
 * A band whose float voltage is below this is a hot or cold one: charging is
 * stopped on reaching the float voltage rather than tapered towards it.
 */
#define JEITA_NORMAL_VTERM_MIN_MV	4201

/* How far the voltage must fall before a stopped hot band charges again. */
#define JEITA_HOT_RESUME_MARGIN_MV	150

/* Bands 1 to 3 are the cold ones and use the cold voltage margin. */
#define JEITA_COLD_BAND_FIRST		1
#define JEITA_COLD_BAND_COUNT		3

/* The float-voltage offset a global O9 unit may be given at most. */
#define JEITA_DELTA_FV_GLOBAL_MAX_MV	20

/* The fuel gauge name a test fixture's cell reports. */
#define JEITA_TMP_TEST_FG_NAME		"2@BP"

/* Longest table property name, with the fast-charge suffix. */
#define JEITA_PROP_NAME_LEN		50

/* Who this module votes as. */
#define JEITA_VOTER			"jeita"
#define JEITA_HOT_VOTER			"jeita-hot"
#define JEITA_SWOCP_VOTER		"swocp"

/* Consecutive over-current samples before the charger is pulled down. */
#define JEITA_SWOCP_DEBOUNCE		3

/* How far the charger is pulled down each time. */
#define JEITA_SWOCP_STEP_MA		100

/* How far under the limit the current must come back before letting go. */
#define JEITA_SWOCP_RELEASE_MA		500

/*
 * Temperatures come in tenths of a degree.  Negative readings are taken a
 * whole degree colder (-0.5 is -1, -1.0 is -2), so a cell at the edge of a
 * cold band is put in the colder one.
 */
static int jeita_temp_to_degree(int temp)
{
	if (temp < 0)
		return -(-temp / 10) - 1;

	return temp / 10;
}

/* The first band whose window holds @temp, or -1 if none does. */
static int jeita_find_index(const struct mca_buckchg_jeita_data *data,
			    int size, int temp)
{
	int i;

	for (i = 0; i < size; i++) {
		if (temp < data[i].temp_high && temp >= data[i].temp_low)
			return i;
	}

	return -1;
}

/*
 * What a band allows at this battery voltage.  The steps are given lowest
 * voltage first: the first one the voltage is at or under decides, and above
 * every step the least of them applies.  @volt_index is set to the deciding
 * step, or -1 when no step was at or above the voltage.
 */
static int jeita_volt_limit(const struct mca_buckchg_jeita_data *row,
			    int vbat, int *volt_index)
{
	const struct mca_buckchg_jeita_volt_para *para = &row->volt_para;
	int curr = row->max_current;
	int i;

	*volt_index = -1;

	for (i = 0; i < para->size; i++) {
		if (vbat <= para->volt_data[i].voltage) {
			*volt_index = i;
			return para->volt_data[i].max_current;
		}
		curr = min(curr, para->volt_data[i].max_current);
	}

	return curr;
}

/*
 * Temperature hysteresis.  When the temperature has moved into another band
 * (in the same charging mode), the band in force is kept until the
 * temperature has gone past its edge by its own margin.  Warming out of the
 * coldest band is not held back while extreme-cold charging is on.
 */
static const struct mca_buckchg_jeita_data *
jeita_select_band(const struct mca_buckchg_jeita_data *data, int cur_index,
		  int index, int temp, bool *hys_affect)
{
	const struct mca_buckchg_jeita_data *cur_row = &data[cur_index];

	*hys_affect = false;

	mca_log_info("temp_low: %d, temp_high: %d, temp: %d\n",
		     cur_row->temp_low - cur_row->low_temp_hys,
		     cur_row->temp_high + cur_row->high_temp_hys, temp);

	if (cur_index < index) {
		if (cur_index == 0 && mca_smartchg_is_extreme_cold_enabled())
			return &data[index];
		if (temp < cur_row->temp_high + cur_row->high_temp_hys) {
			*hys_affect = true;
			return cur_row;
		}
		return &data[index];
	}

	if (temp >= cur_row->temp_low - cur_row->low_temp_hys) {
		*hys_affect = true;
		return cur_row;
	}

	return &data[index];
}

/*
 * Voltage hysteresis, for a pass that stays in the band in force.  A higher
 * current (the voltage fell below a step) is only taken once the voltage is
 * @hyst under that step; a lower one (the voltage rose past a step) once it
 * is above the step below.  Returns whether the new current is applied.
 */
static bool jeita_step_apply(const struct mca_buckchg_jeita_data *row,
			     int curr, int last_curr, int volt_index, int vbat,
			     int hyst)
{
	const struct mca_buckchg_jeita_volt_data *steps = row->volt_para.volt_data;

	if (curr > last_curr) {
		if (volt_index < 0 ||
		    vbat >= steps[volt_index].voltage - hyst)
			return false;
		mca_log_info("vbat_thre[l->s]: max_chg_curr:%d, last_chg_curr:%d, vbat_index:%d, vbat:%d\n",
			     curr, last_curr, volt_index, vbat);
		return true;
	}

	if (curr < last_curr) {
		if (volt_index < 0)
			return false;
		/*
		 * The first step has none below it to compare against.  Stock
		 * reads the entry before the array here; the lower current is
		 * applied instead, which is the conservative choice.
		 */
		if (volt_index > 0 && vbat <= steps[volt_index - 1].voltage)
			return false;
		mca_log_info("vbat_thre[s->l]: max_chg_curr:%d, last_chg_curr:%d, vbat_index:%d\n",
			     curr, last_curr, volt_index);
		return true;
	}

	return false;
}

static void mca_buckchg_jeita_update(struct mca_buckchg_jeita_dev *jeita)
{
	static int last_fastcharge_mode;
	static int last_chg_curr;
	struct mca_buckchg_jeita_para *para = &jeita->jeita_para;
	struct mca_buckchg_jeita_proc_data *proc = &jeita->proc_data;
	const struct mca_buckchg_jeita_data *data, *row;
	int temp = 0, vbat = 0;
	int fastcharge, size, index, cur_index, curr, volt_index, vterm, fcc;
	bool hys_affect = false;
	bool apply;
	int hyst;

	if (!para->jeita_data) {
		mca_log_err("jeita data not ready\n");
		return;
	}

	if (strategy_class_fg_ops_get_temperature(&temp)) {
		mca_log_err("get battery temp failed\n");
		return;
	}
	temp = jeita_temp_to_degree(temp);

	fastcharge = strategy_class_fg_get_fastcharge();

	if (strategy_class_fg_ops_get_voltage(&vbat)) {
		mca_log_err("get battery volt failed\n");
		return;
	}

	if (fastcharge) {
		data = para->jeita_ffc_data;
		size = para->fcc_size;
	} else {
		data = para->jeita_data;
		size = para->size;
	}

	index = jeita_find_index(data, size, temp);
	if (index < 0) {
		if (fastcharge)
			mca_log_err("can not find jeita fcc para\n");
		else
			mca_log_err("can not find jeita para\n");
		return;
	}

	cur_index = proc->cur_jeita_index;
	row = &data[index];
	if (cur_index != -1 && !jeita->baacfg_update && cur_index != index &&
	    fastcharge == last_fastcharge_mode)
		row = jeita_select_band(data, cur_index, index, temp,
					&hys_affect);

	curr = jeita_volt_limit(row, vbat, &volt_index);
	proc->max_chg_curr = curr;

	if (jeita->baacfg_update) {
		jeita->baacfg_update = false;
		mca_log_info("BAA config data update\n");
		apply = true;
	} else if (!hys_affect && cur_index != index) {
		mca_log_info("jeita_index: cur_jeita_index:%d, i:%d, hys_affect:%d\n",
			     cur_index, index, 0);
		apply = true;
	} else {
		if (index - JEITA_COLD_BAND_FIRST >= 0 &&
		    index - JEITA_COLD_BAND_FIRST < JEITA_COLD_BAND_COUNT)
			hyst = jeita->vbat_low_cold_hyst;
		else
			hyst = jeita->vbat_low_hyst;
		apply = jeita_step_apply(row, curr, last_chg_curr, volt_index,
					 vbat, hyst);
	}
	if (apply)
		last_chg_curr = curr;

	/*
	 * A hot or cold band stops charging at its float voltage and resumes
	 * well below it; a normal one charges to it, less what smart charging
	 * took off.
	 */
	vterm = row->vterm;
	if (vterm < JEITA_NORMAL_VTERM_MIN_MV) {
		if (vbat >= vterm - jeita->jeita_hot_termination_hyst)
			mca_vote(jeita->en_voter, JEITA_HOT_VOTER, true, 0);
		else if (vbat < vterm - JEITA_HOT_RESUME_MARGIN_MV)
			mca_vote(jeita->en_voter, JEITA_HOT_VOTER, true, 1);
	} else {
		vterm -= jeita->smartchg_data.delta_fv;
		mca_vote(jeita->en_voter, JEITA_HOT_VOTER, true, 1);
		if (vterm != jeita->vterm) {
			mca_vote(jeita->vterm_voter, JEITA_VOTER, true, vterm);
			jeita->vterm = vterm;
		}
	}

	/* A battery under a temperature test charges at four fifths. */
	fcc = proc->max_chg_curr;
	if (jeita->dtpt_status)
		fcc = fcc * 8 / 10;

	mca_log_info("cur index %d/%d last_chg_curr %d chg_curr %d/%d fastcharge %d jeita_vterm %d jeita_iterm %d vterm %d delta_fv %d hys_affect %d\n",
		     index, cur_index, last_chg_curr, proc->max_chg_curr, fcc,
		     fastcharge, row->vterm, row->iterm, vterm,
		     jeita->smartchg_data.delta_fv, hys_affect);

	last_fastcharge_mode = fastcharge;

	mca_vote(jeita->en_voter, JEITA_VOTER, true, fcc != 0);

	if (!apply)
		return;

	if (!hys_affect)
		proc->cur_jeita_index = index;

	mca_vote(jeita->fcc_voter, JEITA_VOTER, true, fcc);
	mca_vote(jeita->vterm_voter, JEITA_VOTER, true, vterm);
	if (!jeita->support_base_flip && !jeita->base_flip_same)
		mca_vote(jeita->iterm_voter, JEITA_VOTER, true, row->iterm);
}

/*
 * The base cell is the one the charger feeds directly.  Its own band is
 * tracked but not voted; what it is used for is to tell whether the cell is
 * drawing more than its band allows, in which case the charger is pulled
 * down (software over-current protection) until the reading comes back.
 */
static void mca_buckchg_base_jeita_update(struct mca_buckchg_jeita_dev *jeita)
{
	static int last_fastcharge_mode;
	static int last_chg_curr;
	static int base_over_curr_count;
	static bool runswocp;
	struct mca_buckchg_jeita_para *para = &jeita->base_jeita_para;
	struct mca_buckchg_jeita_proc_data *proc = &jeita->base_proc_data;
	const struct mca_buckchg_jeita_data *data, *row;
	int temp = 0, now_curr = 0, vbat = 0;
	int effective_curr, fastcharge, size, index, cur_index, curr;
	int volt_index, now_ma;
	bool hys_affect = false;
	bool apply;
	int ret_temp, ret_curr;

	if (!para->jeita_data || !jeita->voter_ok) {
		mca_log_err("jeita data not ready\n");
		return;
	}

	effective_curr = mca_get_effective_result(jeita->fcc_voter);
	ret_temp = platform_fg_ops_get_temp(FG_IC_MASTER, &temp);
	ret_curr = platform_fg_ops_get_curr(FG_IC_MASTER, &now_curr);
	if (ret_temp || ret_curr) {
		mca_log_err("get battery temp or currfailed\n");
		return;
	}
	temp = jeita_temp_to_degree(temp);

	fastcharge = strategy_class_fg_get_fastcharge();
	mca_log_err("fastcharge_mode is %d\n", fastcharge);

	platform_fg_ops_get_volt(FG_IC_MASTER, &vbat);
	mca_log_info("vbat is %d\n", vbat);

	if (fastcharge) {
		data = para->jeita_ffc_data;
		size = para->fcc_size;
	} else {
		data = para->jeita_data;
		size = para->size;
	}

	index = jeita_find_index(data, size, temp);
	if (index < 0) {
		if (fastcharge)
			mca_log_err("can not find flip jeita fcc para\n");
		else
			mca_log_err("can not find flip jeita para\n");
		return;
	}

	cur_index = proc->cur_jeita_index;
	row = &data[index];
	if (cur_index != -1 && cur_index != index &&
	    fastcharge == last_fastcharge_mode)
		row = jeita_select_band(data, cur_index, index, temp,
					&hys_affect);

	curr = jeita_volt_limit(row, vbat, &volt_index);
	proc->max_chg_curr = curr;

	if (!hys_affect && cur_index != index) {
		mca_log_info("jeita_index: cur_jeita_index:%d, i:%d, hys_affect:%d\n",
			     cur_index, index, 0);
		apply = true;
	} else {
		apply = jeita_step_apply(row, curr, last_chg_curr, volt_index,
					 vbat, jeita->base_vbat_low_hyst);
	}
	if (apply)
		last_chg_curr = curr;

	mca_log_info("cur index %d/%d max_chg_curr %d chg_curr %d hys_affect %d, now_curr %d, effective_curr %d\n",
		     index, cur_index, curr, curr, hys_affect, now_curr,
		     effective_curr);

	last_fastcharge_mode = fastcharge;
	if (!hys_affect && apply)
		proc->cur_jeita_index = index;

	/*
	 * The gauge reports microamps, negative while charging.  One sample
	 * is not enough to act on; the vote is only dropped once the current
	 * has come back with room to spare.
	 */
	now_ma = now_curr / 1000;
	if (effective_curr && now_ma < -curr)
		base_over_curr_count++;
	else
		base_over_curr_count = 0;

	if (base_over_curr_count > JEITA_SWOCP_DEBOUNCE && effective_curr) {
		base_over_curr_count = 0;
		mca_vote(jeita->fcc_voter, JEITA_SWOCP_VOTER, true,
			 effective_curr - JEITA_SWOCP_STEP_MA);
		runswocp = true;
		mca_log_err("reduce fcc %d by swocp\n",
			    effective_curr - JEITA_SWOCP_STEP_MA);
	}

	if (now_ma > JEITA_SWOCP_RELEASE_MA - curr && runswocp) {
		runswocp = false;
		mca_vote(jeita->fcc_voter, JEITA_SWOCP_VOTER, false, 0);
		mca_log_err("disable swocp\n");
	}
}

/*
 * The flip cell sits behind a load switch, so its limit is voted rather than
 * applied: the election's callback is what programs the switch.
 */
static void mca_buckchg_flip_jeita_update(struct mca_buckchg_jeita_dev *jeita)
{
	static int last_fastcharge_mode;
	static int last_chg_curr;
	struct mca_buckchg_jeita_para *para = &jeita->flip_jeita_para;
	struct mca_buckchg_jeita_proc_data *proc = &jeita->flip_proc_data;
	const struct mca_buckchg_jeita_data *data, *row;
	int temp = 0, vbat = 0;
	int fastcharge, size, index, cur_index, curr, volt_index;
	bool hys_affect = false;
	bool apply;

	if (!para->jeita_data || !jeita->voter_ok) {
		mca_log_err("jeita data not ready\n");
		return;
	}

	if (platform_fg_ops_get_temp(FG_IC_SLAVE, &temp)) {
		mca_log_err("get battery temp failed\n");
		return;
	}
	temp = jeita_temp_to_degree(temp);

	fastcharge = strategy_class_fg_get_fastcharge();
	mca_log_err("fastcharge_mode is %d\n", fastcharge);

	platform_fg_ops_get_volt(FG_IC_SLAVE, &vbat);
	mca_log_info("vbat is %d\n", vbat);

	if (fastcharge) {
		data = para->jeita_ffc_data;
		size = para->fcc_size;
	} else {
		data = para->jeita_data;
		size = para->size;
	}

	index = jeita_find_index(data, size, temp);
	if (index < 0) {
		if (fastcharge)
			mca_log_err("can not find flip jeita fcc para\n");
		else
			mca_log_err("can not find flip jeita para\n");
		return;
	}

	cur_index = proc->cur_jeita_index;
	row = &data[index];
	if (cur_index != -1 && cur_index != index &&
	    fastcharge == last_fastcharge_mode)
		row = jeita_select_band(data, cur_index, index, temp,
					&hys_affect);

	curr = jeita_volt_limit(row, vbat, &volt_index);
	proc->max_chg_curr = curr;

	if (!hys_affect && cur_index != index) {
		mca_log_info("jeita_index: cur_jeita_index:%d, i:%d, hys_affect:%d\n",
			     cur_index, index, 0);
		apply = true;
	} else {
		apply = jeita_step_apply(row, curr, last_chg_curr, volt_index,
					 vbat, jeita->flip_vbat_low_hyst);
	}

	last_fastcharge_mode = fastcharge;
	mca_log_info("cur index %d/%d max_chg_curr %d chg_curr %d hys_affect %d\n",
		     index, cur_index, curr, curr, hys_affect);

	if (!apply)
		return;

	last_chg_curr = curr;
	if (!hys_affect)
		proc->cur_jeita_index = index;
	mca_vote(jeita->flip_fcc_voter, JEITA_VOTER, true, curr);
}

static int mca_buckchg_jeita_flip_charge_limit(struct mca_votable *votable,
					       void *data, int effective_result,
					       const char *effective_client)
{
	if (!data)
		return -1;

	mca_log_err("set flip limit current %d\n", effective_result);

	return platform_class_loadsw_set_ibat_limit(0, effective_result);
}

/*
 * The flip cell's election is created here; the others belong to the buck
 * charger and are only joined once it has created them, so this is retried
 * from the monitor until it succeeds.
 */
static void mca_buckchg_jeita_init_voter(struct mca_buckchg_jeita_dev *jeita)
{
	jeita->flip_fcc_voter =
		mca_create_votable("flip_charge_curr", MCA_VOTE_MIN,
				   mca_buckchg_jeita_flip_charge_limit,
				   MCA_BUCKCHG_JEITA_FLIP_CURRENT_DEFAULT_VALUE,
				   jeita);
	if (IS_ERR(jeita->flip_fcc_voter))
		return;

	jeita->en_voter = mca_find_votable("chg_enable");
	if (!jeita->en_voter)
		return;

	jeita->fcc_voter = mca_find_votable("buck_charge_curr");
	if (!jeita->fcc_voter)
		return;

	jeita->vterm_voter = mca_find_votable("term_volt");
	if (!jeita->vterm_voter)
		return;

	jeita->iterm_voter = mca_find_votable("term_curr");
	if (!jeita->iterm_voter)
		return;

	jeita->voter_ok = 1;
}

static void mca_buckchg_jeita_monitor_work(struct work_struct *work)
{
	struct mca_buckchg_jeita_dev *jeita =
		container_of(to_delayed_work(work), struct mca_buckchg_jeita_dev,
			     monitor_work);
	unsigned int interval = MCA_BUCKCHG_JEITA_UPDATE_NORMAL_INTERVAL;

	if (!jeita->voter_ok)
		mca_buckchg_jeita_init_voter(jeita);

	if (!jeita->voter_ok) {
		mca_log_info("voter not ok\n");
		interval = MCA_BUCKCHG_JEITA_INIT_VOTER_INTERVAL;
		goto requeue;
	}

	if (!jeita->online) {
		mca_log_info("adapter is plugout\n");
		return;
	}

	mca_buckchg_jeita_update(jeita);

	if (jeita->support_base_flip) {
		mca_buckchg_base_jeita_update(jeita);
		mca_buckchg_flip_jeita_update(jeita);
	}

requeue:
	queue_delayed_work(system_wq, &jeita->monitor_work,
			   msecs_to_jiffies(interval));
}

static int mca_buckchg_jeita_process_event(int event, int value, void *data)
{
	struct mca_buckchg_jeita_dev *jeita = data;

	if (!jeita)
		return -1;

	mca_log_info("receive event %d, value %d\n", event, value);

	switch (event) {
	case MCA_EVENT_USB_DISCONNECT:
	case MCA_EVENT_WIRELESS_DISCONNECT:
		jeita->online = 0;
		cancel_delayed_work_sync(&jeita->monitor_work);
		break;
	case MCA_EVENT_USB_CONNECT:
	case MCA_EVENT_WIRELESS_CONNECT:
		jeita->online = 1;
		/* A new session starts without hysteresis. */
		jeita->proc_data.cur_jeita_index = -1;
		jeita->proc_data.max_chg_curr = 0;
		jeita->base_proc_data.cur_jeita_index = -1;
		jeita->base_proc_data.max_chg_curr = 0;
		jeita->flip_proc_data.cur_jeita_index = -1;
		jeita->flip_proc_data.max_chg_curr = 0;
		cancel_delayed_work_sync(&jeita->monitor_work);
		queue_delayed_work(system_wq, &jeita->monitor_work, 0);
		break;
	case MCA_EVENT_BATTERY_DTPT:
		jeita->dtpt_status = value;
		break;
	default:
		break;
	}

	return 0;
}

/*
 * The float voltage or term current the band the battery is in allows,
 * looked up afresh.  A battery that cannot be read, or a temperature the
 * tables do not cover, answers zero.
 */
static int mca_strategy_buckchg_jeita_get_status(int func, int *status,
						 void *data)
{
	struct mca_buckchg_jeita_dev *jeita = data;
	const struct mca_buckchg_jeita_data *table;
	int size, index;
	int temp = 0;

	if (!status || !jeita)
		return -1;

	switch (func) {
	case STRATEGY_STATUS_TYPE_JEITA_FFC_ITERM:
	case STRATEGY_STATUS_TYPE_JEITA_FFC_VTERM:
		table = jeita->jeita_para.jeita_ffc_data;
		size = jeita->jeita_para.fcc_size;
		break;
	case STRATEGY_STATUS_TYPE_JEITA_NORMAL_VTERM:
		table = jeita->jeita_para.jeita_data;
		size = jeita->jeita_para.size;
		break;
	default:
		return -1;
	}

	if (strategy_class_fg_ops_get_temperature(&temp)) {
		mca_log_err("get battery temp failed\n");
		*status = 0;
		return 0;
	}
	temp /= 10;

	index = jeita_find_index(table, size, temp);
	if (index < 0) {
		if (func == STRATEGY_STATUS_TYPE_JEITA_NORMAL_VTERM)
			mca_log_err("can not find jeita para\n");
		else
			mca_log_err("can not find jeita fcc para\n");
		*status = 0;
		return 0;
	}

	if (func == STRATEGY_STATUS_TYPE_JEITA_FFC_ITERM)
		*status = table[index].iterm;
	else
		*status = table[index].vterm;

	return 0;
}

/*
 * Smart charging may raise the float voltage of the normal bands.  The cells
 * in global O9 units are rated lower, so they are given at most 20 mV.
 */
static int mca_jeita_smartchg_delta_fv_callback(void *data,
						int effective_result)
{
	struct mca_buckchg_jeita_dev *jeita = data;
	struct mca_hwid_info *hwid = mca_get_hwid_info();

	if (!jeita)
		return -1;

	if (hwid && hwid->platform_version == HARDWARE_PROJECT_O9 &&
	    hwid->country_version == CountryGlobal)
		effective_result = min(effective_result,
				       JEITA_DELTA_FV_GLOBAL_MAX_MV);

	jeita->smartchg_data.delta_fv = effective_result;
	mca_log_err("effective_result: %d\n", effective_result);

	return 0;
}

/*
 * Rewrite where one band finishes.  The top step of its voltage table moves
 * with the float voltage, so the taper ends where charging now does.
 */
static void
mca_buckchg_jeita_smartchg_baa_update_jeita_data(struct mca_buckchg_jeita_data *jeita_data,
						 struct smart_batt_jeita_term_para *para)
{
	struct mca_buckchg_jeita_volt_para *volt_para = &jeita_data->volt_para;
	int last = volt_para->size - 1;

	mca_log_info("jeita_term_para idx: %d, temp: %d:%d => %d:%d, vterm: %d => %d, iterm: %d => %d\n",
		     para->t_range.idx, para->t_range.min, para->t_range.max,
		     jeita_data->temp_low, jeita_data->temp_high,
		     jeita_data->vterm, para->vterm, jeita_data->iterm,
		     para->iterm);

	jeita_data->vterm = para->vterm;
	jeita_data->iterm = para->iterm;

	if (volt_para->size <= 0)
		return;

	mca_log_info("volt_data[%d].voltage: %d => %d\n", last,
		     volt_para->volt_data[last].voltage, jeita_data->vterm);
	volt_para->volt_data[last].voltage = jeita_data->vterm;
}

/*
 * Battery-aware adjustment redraws where the bands finish as the cell ages.
 * @baa_para holds @ffc_size fast-charge entries followed by @normal_size
 * normal ones; each names the band it revises.
 */
static int mca_buckchg_jeita_smartchg_update_baa_para(void *data, char *baa_para,
						      int ffc_size,
						      int normal_size)
{
	struct mca_buckchg_jeita_dev *jeita = data;
	struct smart_batt_jeita_term_para *para;
	int i, idx;

	if (!jeita || !baa_para) {
		mca_log_err("data or baa_para is NULL\n");
		return -1;
	}

	para = (struct smart_batt_jeita_term_para *)baa_para;

	for (i = 0; i < ffc_size; i++) {
		idx = para[i].t_range.idx;
		if (idx < 0 || idx >= jeita->jeita_para.fcc_size) {
			mca_log_err("jeita_para.fcc_size: %d, invalid idx: %d\n",
				    jeita->jeita_para.fcc_size, idx);
			continue;
		}
		mca_buckchg_jeita_smartchg_baa_update_jeita_data(
			&jeita->jeita_para.jeita_ffc_data[idx], &para[i]);
	}

	para += ffc_size;
	for (i = 0; i < normal_size; i++) {
		idx = para[i].t_range.idx;
		if (idx < 0 || idx >= jeita->jeita_para.size) {
			mca_log_err("jeita_para.size: %d, invalid idx: %d\n",
				    jeita->jeita_para.size, idx);
			continue;
		}
		mca_buckchg_jeita_smartchg_baa_update_jeita_data(
			&jeita->jeita_para.jeita_data[idx], &para[i]);
	}

	jeita->baacfg_update = true;

	return 0;
}

static struct mca_smartchg_if_ops g_jeita_smartchg_if_ops = {
	.type		 = MCA_SMARTCHG_IF_CHG_TYPE_JEITA,
	.set_delta_fv	 = mca_jeita_smartchg_delta_fv_callback,
	.update_baa_para = mca_buckchg_jeita_smartchg_update_baa_para,
};

/* Read the voltage table a band names; "null" means it has none. */
static int mca_buckchg_parse_volt_para(struct device_node *np, const char *name,
				       struct mca_buckchg_jeita_volt_para *volt_para)
{
	struct mca_buckchg_jeita_volt_data *steps;
	const char *str;
	int count, i;

	if (!strcmp(name, "null")) {
		mca_log_info("no need parse volt para\n");
		return 0;
	}

	count = mca_parse_dts_count_strings(np, name, JEITA_VOLT_DATA_MAX_GROUP,
					    BUCKCHG_VOLTAGE_PARA_MAX);
	if (count < 0) {
		mca_log_err("parse %s failed\n", name);
		return -1;
	}

	steps = kcalloc(count / BUCKCHG_VOLTAGE_PARA_MAX, sizeof(*steps),
			GFP_KERNEL);
	if (!steps) {
		mca_log_err("volt para no mem\n");
		return -ENOMEM;
	}

	for (i = 0; i < count; i++) {
		struct mca_buckchg_jeita_volt_data *step =
			&steps[i / BUCKCHG_VOLTAGE_PARA_MAX];
		int *val = (i % BUCKCHG_VOLTAGE_PARA_MAX) == BUCKCHG_VOLTAGE_TH ?
			   &step->voltage : &step->max_current;

		if (mca_parse_dts_string_index(np, name, i, &str) ||
		    kstrtoint(str, 10, val)) {
			kfree(steps);
			return -1;
		}
	}

	volt_para->size = count / BUCKCHG_VOLTAGE_PARA_MAX;
	volt_para->volt_data = steps;

	for (i = 0; i < volt_para->size; i++)
		mca_log_debug("volt_para %d %d\n", steps[i].voltage,
			      steps[i].max_current);

	return 0;
}

static void jeita_free_table(struct mca_buckchg_jeita_data *data, int size)
{
	int i;

	if (!data)
		return;

	for (i = 0; i < size; i++)
		kfree(data[i].volt_para.volt_data);
	kfree(data);
}

/*
 * Read one table.  A table that fails to read is dropped whole, so nothing
 * is left pointing at a half-filled one.
 */
static int jeita_parse_table(struct device_node *np, const char *prop,
			     struct mca_buckchg_jeita_data **out, int *out_size)
{
	struct mca_buckchg_jeita_data *data;
	const char *str;
	int count, size, i, ret;

	count = mca_parse_dts_count_strings(np, prop, JEITA_DATA_MAX_GROUP,
					    JEITA_TEMP_PARA_MAX);
	if (count < 0)
		return -EINVAL;

	size = count / JEITA_TEMP_PARA_MAX;
	data = kcalloc(size, sizeof(*data), GFP_KERNEL);
	if (!data) {
		mca_log_err("temp para no mem\n");
		return -ENOMEM;
	}

	for (i = 0; i < count; i++) {
		struct mca_buckchg_jeita_data *row = &data[i / JEITA_TEMP_PARA_MAX];
		int *val;

		if (mca_parse_dts_string_index(np, prop, i, &str)) {
			ret = -1;
			goto err;
		}

		switch (i % JEITA_TEMP_PARA_MAX) {
		case JEITA_TEMP_PARA_TEMP_LOW:
			val = &row->temp_low;
			break;
		case JEITA_TEMP_PARA_TEMP_HIGH:
			val = &row->temp_high;
			break;
		case JEITA_TEMP_PARA_LOW_TEMP_HYS:
			val = &row->low_temp_hys;
			break;
		case JEITA_TEMP_PARA_HIGH_TEMP_HYS:
			val = &row->high_temp_hys;
			break;
		case JEITA_TEMP_PARA_MAX_CURRENT:
			val = &row->max_current;
			break;
		case JEITA_TEMP_PARA_VTERM:
			val = &row->vterm;
			break;
		case JEITA_TEMP_PARA_ITERM:
			val = &row->iterm;
			break;
		default:
			val = NULL;
			break;
		}

		if (val)
			ret = kstrtoint(str, 10, val);
		else
			ret = mca_buckchg_parse_volt_para(np, str,
							  &row->volt_para);
		if (ret) {
			ret = -1;
			goto err;
		}
	}

	*out = data;
	*out_size = size;

	return 0;

err:
	jeita_free_table(data, size);
	return ret;
}

/*
 * A cell's normal table, then its fast-charge table, which is the same
 * property name with "_ffc" appended.
 */
static int mca_buckchg_jeita_parse_para(struct device_node *np, const char *prop,
					struct mca_buckchg_jeita_para *jeita_info)
{
	char ffc_prop[JEITA_PROP_NAME_LEN];
	const struct mca_buckchg_jeita_data *d;
	int ret, i;

	ret = jeita_parse_table(np, prop, &jeita_info->jeita_data,
				&jeita_info->size);
	if (ret == -EINVAL) {
		mca_log_err("parse jeita failed\n");
		return -1;
	}
	if (ret)
		return ret;

	for (i = 0; i < jeita_info->size; i++) {
		d = &jeita_info->jeita_data[i];
		mca_log_err("jeita para %d %d %d %d %d %d %d %d\n",
			    d->temp_low, d->temp_high, d->low_temp_hys,
			    d->high_temp_hys, d->max_current, d->vterm,
			    d->iterm, d->volt_para.size);
	}

	snprintf(ffc_prop, sizeof(ffc_prop), "%s_ffc", prop);
	mca_log_err("new name:%s\n", ffc_prop);

	ret = jeita_parse_table(np, ffc_prop, &jeita_info->jeita_ffc_data,
				&jeita_info->fcc_size);
	if (ret == -EINVAL) {
		mca_log_err("parse jeita para ffc failed\n");
		return -1;
	}
	if (ret)
		return ret;

	for (i = 0; i < jeita_info->fcc_size; i++) {
		d = &jeita_info->jeita_ffc_data[i];
		mca_log_err("jeita ffc para %d %d %d %d %d %d %d %d\n",
			    d->temp_low, d->temp_high, d->low_temp_hys,
			    d->high_temp_hys, d->max_current, d->vterm,
			    d->iterm, d->volt_para.size);
	}

	return 0;
}

/*
 * Which node the tables come from depends on the unit: units sold outside
 * China read the global node when the board has one, and a test fixture's
 * cell (known by its fuel gauge name) reads its own.  The latter needs the
 * fuel gauge to have probed, which is waited for.
 *
 * Returns -1 for a table that is malformed or missing, which is not retried.
 */
static int mca_buckchg_jeita_parse_dt(struct mca_buckchg_jeita_dev *jeita)
{
	struct device_node *np = jeita->dev->of_node;
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	const char *fg_name = NULL;
	int ret;

	mca_parse_dts_u32(np, "vbat_high_hyst", (u32 *)&jeita->vbat_high_hyst,
			  MCA_BUCKCHG_JEITA_VBAT_HIGH_HYST);
	mca_parse_dts_u32(np, "vbat_low_hyst", (u32 *)&jeita->vbat_low_hyst,
			  MCA_BUCKCHG_JEITA_VBAT_LOW_HYST);
	mca_parse_dts_u32(np, "vbat_low_cold_hyst",
			  (u32 *)&jeita->vbat_low_cold_hyst,
			  MCA_BUCKCHG_JEITA_VBAT_LOW_HYST);
	mca_parse_dts_u32(np, "base_vbat_low_hyst",
			  (u32 *)&jeita->base_vbat_low_hyst,
			  MCA_BUCKCHG_JEITA_VBAT_LOW_HYST);
	mca_parse_dts_u32(np, "flip_vbat_low_hyst",
			  (u32 *)&jeita->flip_vbat_low_hyst,
			  MCA_BUCKCHG_JEITA_VBAT_LOW_HYST);
	mca_parse_dts_u32(np, "jeita_hot_termination_hyst",
			  (u32 *)&jeita->jeita_hot_termination_hyst, 0);

	jeita->has_gbl_batt_para = of_property_read_bool(np,
							 "has-global-batt-para");
	if (hwid && jeita->has_gbl_batt_para && hwid->country_version)
		np = of_find_node_by_name(NULL, "mca_buckchg_jeita_gbl_para");
	if (!np) {
		mca_log_err("node in null\n");
		return -1;
	}

	jeita->has_tmp_batt_para = of_property_read_bool(np,
							 "has-tmp-batt-para");
	if (jeita->has_tmp_batt_para) {
		platform_fg_ops_get_device_name(FG_IC_MASTER, &fg_name);
		if (!fg_name) {
			mca_log_err("get device name fail, wait for it\n");
			return -EINVAL;
		}

		mca_log_err("project O9 tmp test: device name: %s\n", fg_name);
		if (!strcmp(JEITA_TMP_TEST_FG_NAME, fg_name)) {
			np = of_find_node_by_name(NULL,
						  "mca_buckchg_jeita_tmp_para");
			if (!np) {
				mca_log_err("has-tmp-batt-para node in null\n");
				return -1;
			}
		}
	}

	ret = mca_buckchg_jeita_parse_para(np, "jeita_para", &jeita->jeita_para);

	jeita->support_base_flip = of_property_read_bool(np,
							 "support-base-flip");
	jeita->base_flip_same = of_property_read_bool(np, "base-flip-same");
	if (jeita->support_base_flip) {
		mca_log_err("support base flip, start config buckchg jeita\n");
		mca_buckchg_jeita_parse_para(np, "base_jeita_para",
					     &jeita->base_jeita_para);
		ret = mca_buckchg_jeita_parse_para(np, "flip_jeita_para",
						   &jeita->flip_jeita_para);
	}

	return ret;
}

static int mca_buckchg_jeita_probe(struct platform_device *pdev)
{
	static int parse_dt_cnt;
	struct mca_buckchg_jeita_dev *jeita;
	int usb_online = 0;
	int wls_online = 0;
	int ret;

	jeita = devm_kzalloc(&pdev->dev, sizeof(*jeita), GFP_KERNEL);
	if (!jeita)
		return -ENOMEM;

	jeita->dev = &pdev->dev;
	platform_set_drvdata(pdev, jeita);

	ret = mca_buckchg_jeita_parse_dt(jeita);
	if (ret) {
		mca_log_err("parse dts failed, ret: %d\n", ret);
		/*
		 * A malformed or missing table is not waited for; anything
		 * else (the fuel gauge not up yet, no memory) is, for a while.
		 */
		if (ret != -1) {
			parse_dt_cnt++;
			dev_err(&pdev->dev, "%s buckchg_jeita parse_dt_cnt = %d\n",
				__func__, parse_dt_cnt);
			msleep(JEITA_PARSE_RETRY_DELAY_MS);
			return parse_dt_cnt >= JEITA_PARSE_RETRY_MAX ?
			       -1 : -EPROBE_DEFER;
		}
	}

	INIT_DELAYED_WORK(&jeita->monitor_work, mca_buckchg_jeita_monitor_work);

	mca_buckchg_jeita_init_voter(jeita);

	mca_strategy_ops_register(STRATEGY_FUNC_TYPE_JEITA,
				  mca_buckchg_jeita_process_event,
				  mca_strategy_buckchg_jeita_get_status, NULL,
				  jeita);

	g_jeita_smartchg_if_ops.data = jeita;
	mca_smartchg_if_ops_register(&g_jeita_smartchg_if_ops);

	/*
	 * The charger may already have been attached when this module
	 * loaded, in which case no connect event is coming.
	 */
	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				     STRATEGY_STATUS_TYPE_ONLINE, &usb_online);
	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
				     STRATEGY_STATUS_TYPE_ONLINE, &wls_online);

	if (usb_online || wls_online) {
		jeita->online = 1;
		queue_delayed_work(system_wq, &jeita->monitor_work, 0);
		return 0;
	}

	if (!jeita->voter_ok)
		queue_delayed_work(system_wq, &jeita->monitor_work,
				   msecs_to_jiffies(JEITA_PROBE_DELAY_MS));
	mca_log_err("probe ok\n");

	return 0;
}

static int mca_buckchg_jeita_remove(struct platform_device *pdev)
{
	struct mca_buckchg_jeita_dev *jeita = platform_get_drvdata(pdev);

	/* Nothing may keep a pointer to the state devm is about to free. */
	mca_strategy_ops_register(STRATEGY_FUNC_TYPE_JEITA, NULL, NULL, NULL,
				  NULL);
	g_jeita_smartchg_if_ops.data = NULL;
	cancel_delayed_work_sync(&jeita->monitor_work);

	return 0;
}

static void mca_buckchg_jeita_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,buckchg_jeita" },
	{ }
};

static struct platform_driver mca_buckchg_jeita_driver = {
	.driver = {
		.name		= "mca_buckchg_jeita",
		.of_match_table	= match_table,
	},
	.probe		= mca_buckchg_jeita_probe,
	.remove		= mca_buckchg_jeita_remove,
	.shutdown	= mca_buckchg_jeita_shutdown,
};
module_platform_driver(mca_buckchg_jeita_driver);

MODULE_DESCRIPTION("buckchg jeita");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
