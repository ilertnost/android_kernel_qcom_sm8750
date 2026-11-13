// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Charging within a thermal budget.  See inc/mca_charger_thermal.h.
 *
 * The thermal framework (or the thermal daemon, through sysfs) hands down one
 * number -- how far to back off -- and this turns it into a limit for every
 * way the phone can be charged, because the same amount of heat comes from
 * very different currents depending on whether the charge is going through
 * the buck, through a charge pump, or in over a coil.  Each of those has an
 * election of its own, and this module is one voter in each.
 */

#define MCA_LOG_TAG "mca_thermal"

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thermal.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#include "inc/mca_charger_thermal.h"
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/common/mca_smem.h>
#include <mca/common/mca_sysfs.h>
#include <mca/common/mca_voter.h>
#include <mca/platform/platform_fg_ic_ops.h>
#include <mca/protocol/protocol_class.h>
#include <mca/smartchg/smart_chg_class.h>
#include <mca/strategy/strategy_class.h>

/* How many thermal levels the tables describe. */
#define MCA_THERMAL_LEVEL_MAX		16

/* The base cell tables always hold this many levels above zero. */
#define MCA_THERMAL_FLIP_LEVEL_MAX	(MCA_THERMAL_LEVEL_MAX - 1)

/* How many battery cells a board may name for the userspace current. */
#define MCA_THERMAL_SIC_SELECT_MAX	3

/* Who is voting, as the votables record it. */
#define MCA_THERMAL_VOTER		"mca_thermal"
#define MCA_WIRELESS_THERMAL_VOTER	"mca_wireless_thermal"

/* How long to wait before looking for the elections again. */
#define MCA_THERMAL_VOTER_RETRY_MS	1000

/*
 * A factory boot is only honoured this early: once the phone has been up for
 * longer it is in a user's hands and the limits are not optional.
 */
#define MCA_THERMAL_BOOT_WINDOW_SEC	60

/* The wireless strategies' own code for "the thermal level moved". */
#define STRATEGY_PROCESS_THERMAL_LEVEL	15

/* What a Quick Charge 3 adapter is assumed to support, in milliamps. */
#define MCA_THERMAL_QC3_SIC_INIT_FCC	6000

/*
 * Above this many watts the fast-charge strategy reports, the adapter's
 * maximum is taken from what it advertises rather than from the power
 * capability it was negotiated at.
 */
#define MCA_THERMAL_PD_MAX_POWER_TH	67

/* Which charging mode a level is being applied to. */
enum mca_thermal_mode_ele {
	THERMAL_MODE_BUCK_5V_IN = 0,
	THERMAL_MODE_BUCK_9V_IN,
	THERMAL_MODE_BUCK_5V_ICH,
	THERMAL_MODE_BUCK_9V_ICH,
	THERMAL_MODE_DIV1_SINGLE_CURR,
	THERMAL_MODE_DIV1_MULTI_CURR,
	THERMAL_MODE_DIV2_SINGLE_CURR,
	THERMAL_MODE_DIV2_MULTI_CURR,
	THERMAL_MODE_DIV4_SINGLE_CURR,
	THERMAL_MODE_DIV4_MULTI_CURR,
	THERMAL_MODE_MAX,
};

/* The same, for the wireless paths, which are told apart by what they proved. */
enum mca_wireless_thermal_mode_ele {
	THERMAL_MODE_WIRELESS_BPP_IN = 0,
	THERMAL_MODE_WIRELESS_BPPQC2_IN,
	THERMAL_MODE_WIRELESS_BPPQC3_IN,
	THERMAL_MODE_WIRELESS_EPP_IN,
	THERMAL_MODE_WIRELESS_AUTHEN_20W,
	THERMAL_MODE_WIRELESS_AUTHEN_30W,
	THERMAL_MODE_WIRELESS_AUTHEN_50W,
	THERMAL_MODE_WIRELESS_AUTHEN_80W,
	THERMAL_MODE_WIRELESS_AUTHEN_VOICE_BOX,
	THERMAL_MODE_WIRELESS_AUTHEN_MAGNET_30W,
	THERMAL_MODE_WIRELESS_MAX,
};

/* What userspace can look at and set. */
enum mca_thermal_sysfs_attr_ele {
	MCA_THERMAL_WIRED_CHG_CURR,
	MCA_THERMAL_WIRED_CHG_CURR2,
	MCA_THERMAL_WIRED_CTRL_LIMIT,
	MCA_THERMAL_WIRED_THERMAL_REMOVE,
	MCA_THERMAL_WIRELESS_CHG_CURR,
	MCA_THERMAL_WIRELESS_CTRL_LIMIT,
	MCA_THERMAL_WLS_QUICK_CHG_CTRL_LIMIT,
	MCA_THERMAL_WIRELESS_THERMAL_REMOVE,
	MCA_THERMAL_WIRELESS_MAG_CTRL_LIMIT,
	MCA_THERMAL_ATTR_MAX,
};

/**
 * struct mca_thermal_ctrl_info - where one charging path currently stands
 * @limit_level:          the highest level its table describes
 * @voter_ok:             every election for this path has been found
 * @sic_chg_curr:         the current userspace last asked for, in milliamps
 * @sic_init_fcc:         what the adapter is rated to give, in milliamps
 * @chg_curr:             the current cap in force, in milliamps (0: none)
 * @cur_level:            the level that was asked for
 * @magnet_limit_level:   the level to use under a magnetic case instead
 * @quickchg_limit_level: the level to use while fast charging wirelessly
 * @thermal_remove:       the wired limits are lifted for a test
 * @wls_thermal_remove:   the wireless limits are lifted for a test
 * @thermal_removed:      and the votes have already been withdrawn
 */
struct mca_thermal_ctrl_info {
	int	limit_level;
	int	voter_ok;
	int	sic_chg_curr;
	int	sic_init_fcc;
	int	chg_curr;
	int	cur_level;
	int	magnet_limit_level;
	int	quickchg_limit_level;
	int	thermal_remove;
	int	wls_thermal_remove;
	bool	thermal_removed;
};

/**
 * struct mca_thermal_info - the thermal budget for charging
 * @dev:                     this device
 * @cdev:                    what the thermal framework governs this through
 * @init_voter_work:         finds the elections once the paths have registered
 * @support_wireless:        the board charges wirelessly
 * @wireless_phone_level:    above this level the pad is told to stand down
 * @support_base_flip:       the board has a second cell in its base
 * @support_mag_wls_thermal: the board tells magnetic cases apart
 * @wired_thermal_data:      the wired table, one row per level above zero
 * @wireless_thermal_data:   the wireless table
 * @wired_flip_thermal:      the base cell's wired limit, per level
 * @wireless_flip_thermal:   and its wireless one
 * @wired_voter:             the election each wired mode's limit is decided in
 * @wireless_voter:          the same, per wireless mode
 * @flip_voter:              the base cell's wired election
 * @wls_flip_voter:          and its wireless one
 * @wired_ctrl_info:         where the wired path stands
 * @wireless_ctrl_info:      where the wireless path stands
 * @flip_ctrl_info:          where the base cell's wired path stands
 * @wls_flip_ctrl_info:      and its wireless one
 * @wls_super_sts:           the wireless path is fast charging
 * @batt_cell_name:          which cell this phone was built with
 * @sic_select_cnt:          how many cells the board names (negative: none)
 * @sic_select:              the cells a userspace current may be applied to
 * @usb_online:              charging by cable
 * @wls_online:              charging over a coil
 * @real_type:               what the attached charger turned out to be
 * @not_support_sic:         userspace may not set a current on this board
 * @not_support_sic_curr:    what to use instead when it may not
 * @use_magnet:              a magnetic case is attached
 * @use_magnet_tx:           and it is the pad that is magnetic
 */
struct mca_thermal_info {
	struct device			*dev;
	struct thermal_cooling_device	*cdev;
	struct delayed_work		init_voter_work;
	int				support_wireless;
	int				wireless_phone_level;
	int				support_base_flip;
	int				support_mag_wls_thermal;
	int wired_thermal_data[MCA_THERMAL_LEVEL_MAX][THERMAL_MODE_MAX];
	int wireless_thermal_data[MCA_THERMAL_LEVEL_MAX][THERMAL_MODE_WIRELESS_MAX];
	int				wired_flip_thermal[MCA_THERMAL_LEVEL_MAX];
	int				wireless_flip_thermal[MCA_THERMAL_LEVEL_MAX];
	struct mca_votable		*wired_voter[THERMAL_MODE_MAX];
	struct mca_votable		*wireless_voter[THERMAL_MODE_WIRELESS_MAX];
	struct mca_votable		*flip_voter;
	struct mca_votable		*wls_flip_voter;
	struct mca_thermal_ctrl_info	wired_ctrl_info;
	struct mca_thermal_ctrl_info	wireless_ctrl_info;
	struct mca_thermal_ctrl_info	flip_ctrl_info;
	struct mca_thermal_ctrl_info	wls_flip_ctrl_info;
	int				wls_super_sts;
	const char			*batt_cell_name;
	int				sic_select_cnt;
	const char			*sic_select[MCA_THERMAL_SIC_SELECT_MAX];
	int				usb_online;
	int				wls_online;
	int				real_type;
	int				not_support_sic;
	int				not_support_sic_curr;
	int				use_magnet;
	int				use_magnet_tx;
};

/**
 * struct mca_thermal_sic_init - what a PD adapter of a given power may carry
 * @power: up to this many watts
 * @fcc:   the charge current cap, in milliamps
 */
struct mca_thermal_sic_init {
	int power;
	int fcc;
};

static const struct mca_thermal_sic_init sic_init_list[] = {
	{  33,  6600 },
	{  67, 12400 },
	{  90, 15600 },
	{ 120, 22000 },
};

static struct mca_thermal_info *g_wlscharger_thermal_info;

/*
 * Levels come from userspace and from the thermal framework; the handlers
 * below refuse a negative one along with one above the table, since it would
 * index in front of the table.
 */

/*
 * Two limits, either of which may be "no limit at all".  Zero is what the
 * tables use for that, so it cannot simply go through min().
 */
static int mca_thermal_min(int a, int b)
{
	if (!a)
		return b;
	if (!b)
		return a;

	return min(a, b);
}

/*
 * A phone coming off the line is charged flat out to test it, and the thermal
 * tables would get in the way of that.  Shared memory says whether this is
 * such a boot; it is only believed early after boot.  Returns the mode read
 * from shared memory, or zero when the limits apply.
 */
static u32 mca_charger_thermal_zero_speed(void)
{
	struct timespec64 ts;
	u32 is_zero_speed = 0;

	ts = ns_to_timespec64(ktime_get_with_offset(TK_OFFS_BOOT));
	if (ts.tv_sec >= MCA_THERMAL_BOOT_WINDOW_SEC)
		return 0;

	get_smem_battery_info(&is_zero_speed);

	return is_zero_speed;
}

/*
 * Every wired mode is capped at whichever is smaller: what this thermal level
 * allows it, or the current userspace asked for.  Level zero means only the
 * userspace current applies.  The buck input limits come straight from the
 * table: they are a property of the adapter's voltage, not of the cell.
 */
static void mca_charger_thermal_handle_limit(struct mca_thermal_ctrl_info *ctrl,
					     int (*data)[THERMAL_MODE_MAX],
					     struct mca_votable **voter)
{
	struct mca_thermal_info *info = g_wlscharger_thermal_info;
	int limit[THERMAL_MODE_MAX] = { 0 };
	int level = ctrl->cur_level;
	const int *row = NULL;
	u32 is_zero_speed;
	int i;

	is_zero_speed = mca_charger_thermal_zero_speed();
	if (is_zero_speed) {
		mca_log_err("is_zero_speed = %u, ignore thermal...\n",
			    is_zero_speed);
		return;
	}

	if (!info) {
		mca_log_err("thermal info is NULL\n");
		return;
	}
	if (!ctrl->voter_ok) {
		mca_log_err("voter is not ready\n");
		return;
	}
	if (!info->usb_online) {
		mca_log_err("charger is not online\n");
		return;
	}
	if (level < 0 || level > ctrl->limit_level) {
		mca_log_err("thermal level invalid\n");
		return;
	}

	if (info->not_support_sic) {
		ctrl->chg_curr = info->not_support_sic_curr;
		mca_log_info("chg_curr = %d\n", ctrl->chg_curr);
	}

	if (level)
		row = data[level - 1];

	for (i = THERMAL_MODE_BUCK_5V_ICH; i < THERMAL_MODE_MAX; i++)
		limit[i] = row ? mca_thermal_min(row[i], ctrl->chg_curr) :
				 ctrl->chg_curr;

	if (ctrl->thermal_remove) {
		if (ctrl->thermal_removed)
			return;

		for (i = 0; i < THERMAL_MODE_MAX; i++)
			mca_vote(voter[i], MCA_THERMAL_VOTER, false, limit[i]);

		ctrl->thermal_removed = true;
		mca_log_err("wired thermal_removed: %d\n", 1);
		return;
	}

	if (ctrl->thermal_removed) {
		ctrl->thermal_removed = false;
		mca_log_err("wired thermal_removed: %d\n", 0);
	}

	for (i = THERMAL_MODE_BUCK_5V_ICH; i < THERMAL_MODE_MAX; i++)
		mca_vote(voter[i], MCA_THERMAL_VOTER, !!limit[i], limit[i]);

	/*
	 * On an adapter the charge pump can take, capping the buck input
	 * would throttle a path the heat is not coming from.  At level zero
	 * there is nothing to cap.
	 */
	if (!level ||
	    info->real_type == XM_CHARGER_TYPE_HVDCP3_B ||
	    info->real_type == XM_CHARGER_TYPE_HVDCP3P5 ||
	    info->real_type == XM_CHARGER_TYPE_PPS ||
	    info->real_type == XM_CHARGER_TYPE_PD_VERIFY) {
		mca_log_err("real_type = %d wired remove thermal for buck_in\n",
			    info->real_type);
		mca_vote(voter[THERMAL_MODE_BUCK_5V_IN], MCA_THERMAL_VOTER,
			 false, 0);
		mca_vote(voter[THERMAL_MODE_BUCK_9V_IN], MCA_THERMAL_VOTER,
			 false, 0);
		return;
	}

	mca_vote(voter[THERMAL_MODE_BUCK_5V_IN], MCA_THERMAL_VOTER,
		 !!row[THERMAL_MODE_BUCK_5V_IN], row[THERMAL_MODE_BUCK_5V_IN]);
	mca_vote(voter[THERMAL_MODE_BUCK_9V_IN], MCA_THERMAL_VOTER,
		 !!row[THERMAL_MODE_BUCK_9V_IN], row[THERMAL_MODE_BUCK_9V_IN]);
}

/*
 * The base cell of a foldable has one limit per level rather than a table:
 * it is fed through a load switch, so there is only one current to cap.
 */
static void mca_charger_thermal_flip_handle_limit(struct mca_thermal_ctrl_info *ctrl,
						  const int *data,
						  struct mca_votable *voter)
{
	struct mca_thermal_info *info = g_wlscharger_thermal_info;
	int level = ctrl->cur_level;
	u32 is_zero_speed;

	is_zero_speed = mca_charger_thermal_zero_speed();
	if (is_zero_speed) {
		mca_log_err("is_zero_speed = %u, ignore thermal...\n",
			    is_zero_speed);
		return;
	}

	if (!info) {
		mca_log_err("thermal info is NULL\n");
		return;
	}
	if (!ctrl->voter_ok) {
		mca_log_err("voter is not ready\n");
		return;
	}
	if (!info->usb_online) {
		mca_log_err("charger is not online\n");
		return;
	}
	if (level < 0 || level > ctrl->limit_level) {
		mca_log_err("thermal level invalid\n");
		return;
	}

	if (ctrl->thermal_remove) {
		if (ctrl->thermal_removed)
			return;
		mca_vote(voter, MCA_THERMAL_VOTER, false, data[level]);
		ctrl->thermal_removed = true;
		mca_log_err("wired flip thermal_removed: %d\n", 1);
		return;
	}

	if (ctrl->thermal_removed) {
		ctrl->thermal_removed = false;
		mca_log_err("wired flip thermal_removed: %d\n", 0);
	}

	mca_vote(voter, MCA_THERMAL_VOTER, true, data[level]);
}

static void
mca_wireless_charger_thermal_flip_handle_limit(struct mca_thermal_ctrl_info *ctrl,
					       const int *data,
					       struct mca_votable *voter)
{
	struct mca_thermal_info *info = g_wlscharger_thermal_info;
	int level = ctrl->cur_level;
	u32 is_zero_speed;

	is_zero_speed = mca_charger_thermal_zero_speed();
	if (is_zero_speed) {
		mca_log_err("is_zero_speed = %u, ignore thermal...\n",
			    is_zero_speed);
		return;
	}

	if (!info) {
		mca_log_err("thermal info is NULL\n");
		return;
	}
	if (!ctrl->voter_ok) {
		mca_log_err("voter is not ready\n");
		return;
	}
	if (!info->wls_online) {
		mca_log_err("charger is not online\n");
		return;
	}
	if (level < 0 || level > ctrl->limit_level) {
		mca_log_err("thermal level invalid\n");
		return;
	}

	if (ctrl->wls_thermal_remove) {
		if (ctrl->thermal_removed)
			return;
		mca_vote(voter, MCA_THERMAL_VOTER, false, data[level]);
		ctrl->thermal_removed = true;
		mca_log_err("wireless flip thermal_removed: %d\n", 1);
		return;
	}

	if (ctrl->thermal_removed) {
		ctrl->thermal_removed = false;
		mca_log_err("wireless flip thermal_removed: %d\n", 0);
	}

	mca_vote(voter, MCA_THERMAL_VOTER, true, data[level]);
}

/*
 * The wireless path picks its level from one of three places: the level that
 * was asked for, the magnet level while a magnetic case sits between the
 * coils (on a pad that is not itself magnetic), or the fast-charge level
 * while the pad is fast charging.
 *
 * The four input limits (BPP, QC2, QC3, EPP) only apply when the table gives
 * one; the authenticated paths fall back to the userspace current.
 */
static void
mca_wireless_charger_thermal_handle_limit(struct mca_thermal_ctrl_info *ctrl,
					  int (*data)[THERMAL_MODE_WIRELESS_MAX],
					  struct mca_votable **voter)
{
	struct mca_thermal_info *info = g_wlscharger_thermal_info;
	int limit[THERMAL_MODE_WIRELESS_MAX];
	const int *row = NULL;
	bool phone_limit;
	int level;
	int i;

	if (!info) {
		mca_log_err("thermal info is NULL\n");
		return;
	}
	if (!ctrl->voter_ok) {
		mca_log_err("voter is not ready\n");
		return;
	}

	if (info->support_mag_wls_thermal && info->use_magnet &&
	    !info->use_magnet_tx)
		level = ctrl->magnet_limit_level;
	else
		level = ctrl->cur_level;
	mca_log_err("thermal_levle %d\n", level);

	if (info->wls_super_sts)
		level = ctrl->quickchg_limit_level;
	mca_log_err("thermal_levle %d\n", level);

	if (level < 0 || level > ctrl->limit_level) {
		mca_log_err("thermal level invalid\n");
		return;
	}

	/*
	 * Past this level the pad is asked to stop offering the power rather
	 * than the phone being asked to refuse it.
	 */
	phone_limit = level > info->wireless_phone_level;
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_WIRELESS,
				  STRATEGY_PROCESS_THERMAL_LEVEL, phone_limit);
	mca_strategy_func_process(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
				  STRATEGY_PROCESS_THERMAL_LEVEL, phone_limit);

	if (!info->wls_online) {
		mca_log_err("charger is not online\n");
		return;
	}

	if (level)
		row = data[level - 1];

	for (i = 0; i < THERMAL_MODE_WIRELESS_MAX; i++) {
		if (row && row[i])
			limit[i] = mca_thermal_min(row[i], ctrl->chg_curr);
		else if (i < THERMAL_MODE_WIRELESS_AUTHEN_20W)
			limit[i] = 0;
		else
			limit[i] = ctrl->chg_curr;
	}

	if (ctrl->wls_thermal_remove) {
		if (!ctrl->thermal_removed) {
			for (i = 0; i < THERMAL_MODE_WIRELESS_MAX; i++)
				mca_vote(voter[i], MCA_WIRELESS_THERMAL_VOTER,
					 false, limit[i]);
			ctrl->thermal_removed = true;
		}
		mca_log_err("set disable vote %d\n", 1);
		return;
	}

	ctrl->thermal_removed = false;
	mca_log_err("set enable vote\n");

	/*
	 * A vote that came out the same as last time does not re-run its own
	 * election, so each is run here: a level that resolved to an unchanged
	 * limit still has to reach the receiver.
	 */
	for (i = 0; i < THERMAL_MODE_WIRELESS_MAX; i++) {
		mca_vote(voter[i], MCA_WIRELESS_THERMAL_VOTER, !!limit[i],
			 limit[i]);
		mca_rerun_election(voter[i]);
	}
}

static void mca_charger_thermal_apply_wired(struct mca_thermal_info *info)
{
	mca_charger_thermal_handle_limit(&info->wired_ctrl_info,
					 info->wired_thermal_data,
					 info->wired_voter);
}

static void mca_charger_thermal_apply_wireless(struct mca_thermal_info *info)
{
	mca_wireless_charger_thermal_handle_limit(&info->wireless_ctrl_info,
						  info->wireless_thermal_data,
						  info->wireless_voter);
}

static void mca_charger_thermal_apply_flip(struct mca_thermal_info *info)
{
	mca_charger_thermal_flip_handle_limit(&info->flip_ctrl_info,
					      info->wired_flip_thermal,
					      info->flip_voter);
}

static void mca_charger_thermal_apply_wls_flip(struct mca_thermal_info *info)
{
	mca_wireless_charger_thermal_flip_handle_limit(&info->wls_flip_ctrl_info,
						       info->wireless_flip_thermal,
						       info->wls_flip_voter);
}

/**
 * mca_set_wls_charger_thermal_remove() - lift or restore the wireless limits
 * @wls_thermal_remove: whether to lift them
 */
int mca_set_wls_charger_thermal_remove(bool wls_thermal_remove)
{
	struct mca_thermal_info *info = g_wlscharger_thermal_info;

	if (!info)
		return -1;

	info->wireless_ctrl_info.wls_thermal_remove = wls_thermal_remove;
	mca_charger_thermal_apply_wireless(info);
	mca_log_err("set wireless thermal remove %d\n",
		    info->wireless_ctrl_info.wls_thermal_remove);

	return 0;
}
EXPORT_SYMBOL(mca_set_wls_charger_thermal_remove);

/**
 * mca_get_wls_charger_thermal_remove() - whether the wireless limits are lifted
 * @wls_thermal_remove: filled in
 */
int mca_get_wls_charger_thermal_remove(bool *wls_thermal_remove)
{
	struct mca_thermal_info *info = g_wlscharger_thermal_info;

	if (!info)
		return -1;

	*wls_thermal_remove = !!info->wireless_ctrl_info.wls_thermal_remove;

	return 0;
}
EXPORT_SYMBOL(mca_get_wls_charger_thermal_remove);

/*
 * Nothing is attached any more, so every limit this module was holding comes
 * off, and the currents learnt from the last adapter are forgotten.
 */
static void mca_charger_thermal_poweroff_clear_voter(struct mca_thermal_info *info)
{
	int i;

	mca_log_info("usb disconnect, clear voter\n");

	if (info->usb_online)
		return;

	info->wired_ctrl_info.chg_curr = 0;
	info->wired_ctrl_info.sic_chg_curr = 0;
	info->wired_ctrl_info.sic_init_fcc = 0;
	info->real_type = XM_CHARGER_TYPE_UNKNOW;

	for (i = 0; i < THERMAL_MODE_MAX; i++)
		mca_vote(info->wired_voter[i], MCA_THERMAL_VOTER, false, 0);

	if (!info->support_base_flip)
		return;

	mca_vote(info->flip_voter, MCA_THERMAL_VOTER, false, 0);
	mca_vote(info->wls_flip_voter, MCA_THERMAL_VOTER, false, 0);
}

static void
mca_wireless_charger_thermal_poweroff_clear_voter(struct mca_thermal_info *info)
{
	int i;

	mca_log_info("wireless disconnect, clear voter\n");

	if (info->wls_online)
		return;

	for (i = 0; i < THERMAL_MODE_WIRELESS_MAX; i++)
		mca_vote(info->wireless_voter[i], MCA_WIRELESS_THERMAL_VOTER,
			 false, 0);
}

/*
 * What userspace asked for and what the adapter can give are two different
 * caps; the one that ends up in force is whichever is smaller, and neither
 * being set leaves the current one alone.
 */
static void mca_charger_thermal_update_chg_curr(struct mca_thermal_info *info)
{
	struct mca_thermal_ctrl_info *ctrl = &info->wired_ctrl_info;
	int curr;

	curr = mca_thermal_min(ctrl->sic_chg_curr, ctrl->sic_init_fcc);
	if (!curr) {
		mca_log_info("invalid chg_curr\n");
		return;
	}

	ctrl->chg_curr = curr;
	mca_log_info("chg_curr = %d, sic_init_fcc = %d sic_chg_curr = %d \n",
		     ctrl->chg_curr, ctrl->sic_init_fcc, ctrl->sic_chg_curr);
}

/*
 * How much the adapter is rated for decides where the fast-charge path can
 * start from.  A Quick Charge 3 adapter does not say, so it gets a fixed
 * figure; a PPS one is asked, and its answer is mapped onto sic_init_list.
 */
static void mca_charger_thermal_sic_initial_chg_curr(struct mca_thermal_info *info)
{
	struct mca_thermal_ctrl_info *ctrl = &info->wired_ctrl_info;
	int power_max = 0;
	u32 max_power = 0;
	int i;

	switch (info->real_type) {
	case XM_CHARGER_TYPE_HVDCP3:
	case XM_CHARGER_TYPE_HVDCP3_B:
	case XM_CHARGER_TYPE_HVDCP3P5:
		ctrl->sic_init_fcc = MCA_THERMAL_QC3_SIC_INIT_FCC;
		mca_charger_thermal_update_chg_curr(info);
		if (info->usb_online)
			mca_charger_thermal_apply_wired(info);
		mca_log_info("QC3.0 sic_init_fcc %d\n", ctrl->sic_init_fcc);
		return;
	case XM_CHARGER_TYPE_PPS:
	case XM_CHARGER_TYPE_PD_VERIFY:
		break;
	default:
		mca_log_info("not support type %d\n", info->real_type);
		return;
	}

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
				     STRATEGY_STATUS_TYPE_POWER_MAX, &power_max);
	if (power_max > MCA_THERMAL_PD_MAX_POWER_TH)
		protocol_class_get_adapter_max_power(ADAPTER_PROTOCOL_PPS,
						     &max_power);
	else
		protocol_class_get_adapter_pwr_max_power(ADAPTER_PROTOCOL_PPS,
							 &max_power);

	for (i = 0; i < ARRAY_SIZE(sic_init_list); i++) {
		if ((int)max_power <= sic_init_list[i].power) {
			ctrl->sic_init_fcc = sic_init_list[i].fcc;
			mca_charger_thermal_update_chg_curr(info);
			break;
		}
	}

	if (info->usb_online)
		mca_charger_thermal_apply_wired(info);
}

static int mca_charger_thermal_get_max_level(struct thermal_cooling_device *cdev,
					     unsigned long *state)
{
	struct mca_thermal_info *info = cdev->devdata;

	if (!info)
		return -1;

	*state = info->wired_ctrl_info.limit_level + 1;

	return 0;
}

static int mca_charger_thermal_get_cur_level(struct thermal_cooling_device *cdev,
					     unsigned long *state)
{
	struct mca_thermal_info *info = cdev->devdata;

	if (!info)
		return -1;

	*state = info->wired_ctrl_info.cur_level;

	return 0;
}

static int mca_charger_thermal_set_cur_level(struct thermal_cooling_device *cdev,
					     unsigned long state)
{
	struct mca_thermal_info *info = cdev->devdata;

	if (!info)
		return -1;

	info->wired_ctrl_info.cur_level = state;
	mca_charger_thermal_apply_wired(info);

	if (info->support_base_flip) {
		info->flip_ctrl_info.cur_level = state;
		mca_charger_thermal_apply_flip(info);
	}

	mca_log_info("tcd set wired level %lu\n", state);

	return 0;
}

static const struct thermal_cooling_device_ops g_mca_charger_tcd_ops = {
	.get_max_state	= mca_charger_thermal_get_max_level,
	.get_cur_state	= mca_charger_thermal_get_cur_level,
	.set_cur_state	= mca_charger_thermal_set_cur_level,
};

static int mca_charger_thermal_wls_super_sts_callback(void *data,
						      int effective_result)
{
	struct mca_thermal_info *info = data;

	if (!info)
		return -1;

	info->wls_super_sts = effective_result;
	mca_charger_thermal_apply_wireless(info);
	mca_log_info("effective_result: %d\n", effective_result);

	return 0;
}

static int mca_charger_thermal_init_wired_voter(struct mca_thermal_info *info)
{
	struct mca_votable **voter = info->wired_voter;

	if (info->wired_ctrl_info.voter_ok)
		return 0;

	voter[THERMAL_MODE_BUCK_5V_IN] = mca_find_votable("buck_5v_in");
	if (!voter[THERMAL_MODE_BUCK_5V_IN])
		goto err;
	voter[THERMAL_MODE_BUCK_5V_ICH] = mca_find_votable("buck_5v_ich");
	if (!voter[THERMAL_MODE_BUCK_5V_ICH])
		goto err;
	voter[THERMAL_MODE_BUCK_9V_IN] = mca_find_votable("buck_9v_in");
	if (!voter[THERMAL_MODE_BUCK_9V_IN])
		goto err;
	voter[THERMAL_MODE_BUCK_9V_ICH] = mca_find_votable("buck_9v_ich");
	if (!voter[THERMAL_MODE_BUCK_9V_ICH])
		goto err;
	voter[THERMAL_MODE_DIV1_SINGLE_CURR] = mca_find_votable("div1_single");
	if (!voter[THERMAL_MODE_DIV1_SINGLE_CURR])
		goto err;
	voter[THERMAL_MODE_DIV1_MULTI_CURR] = mca_find_votable("div1_multi");
	if (!voter[THERMAL_MODE_DIV1_MULTI_CURR])
		goto err;
	voter[THERMAL_MODE_DIV2_SINGLE_CURR] = mca_find_votable("div2_single");
	if (!voter[THERMAL_MODE_DIV2_SINGLE_CURR])
		goto err;
	voter[THERMAL_MODE_DIV2_MULTI_CURR] = mca_find_votable("div2_multi");
	if (!voter[THERMAL_MODE_DIV2_MULTI_CURR])
		goto err;
	voter[THERMAL_MODE_DIV4_SINGLE_CURR] = mca_find_votable("div4_single");
	if (!voter[THERMAL_MODE_DIV4_SINGLE_CURR])
		goto err;
	voter[THERMAL_MODE_DIV4_MULTI_CURR] = mca_find_votable("div4_multi");
	if (!voter[THERMAL_MODE_DIV4_MULTI_CURR])
		goto err;

	info->wired_ctrl_info.voter_ok = 1;

	return 0;

err:
	mca_log_err("init wire voter failed\n");
	return -1;
}

static const char * const mca_wireless_voter_name[THERMAL_MODE_WIRELESS_MAX] = {
	[THERMAL_MODE_WIRELESS_BPP_IN]		  = "wireless_bpp_in",
	[THERMAL_MODE_WIRELESS_BPPQC2_IN]	  = "wireless_bppqc2_in",
	[THERMAL_MODE_WIRELESS_BPPQC3_IN]	  = "wireless_bppqc3_in",
	[THERMAL_MODE_WIRELESS_EPP_IN]		  = "wireless_epp_in",
	[THERMAL_MODE_WIRELESS_AUTHEN_20W]	  = "wireless_auth_20w",
	[THERMAL_MODE_WIRELESS_AUTHEN_30W]	  = "wireless_auth_30w",
	[THERMAL_MODE_WIRELESS_AUTHEN_50W]	  = "wireless_auth_50w",
	[THERMAL_MODE_WIRELESS_AUTHEN_80W]	  = "wireless_auth_80w",
	[THERMAL_MODE_WIRELESS_AUTHEN_VOICE_BOX]  = "wireless_auth_voice_box",
	[THERMAL_MODE_WIRELESS_AUTHEN_MAGNET_30W] = "wireless_auth_magnet_30w",
};

static int mca_charger_thermal_init_wireless_voter(struct mca_thermal_info *info)
{
	int i;

	if (info->wireless_ctrl_info.voter_ok)
		return 0;

	for (i = 0; i < THERMAL_MODE_WIRELESS_MAX; i++) {
		info->wireless_voter[i] =
			mca_find_votable(mca_wireless_voter_name[i]);
		if (!info->wireless_voter[i]) {
			mca_log_err("init wireless voter failed\n");
			return -1;
		}
	}

	info->wireless_ctrl_info.voter_ok = 1;

	return 0;
}

static int mca_charger_thermal_init_flip_voter(struct mca_thermal_info *info)
{
	info->flip_voter = mca_find_votable("thermal_flip");
	if (!info->flip_voter)
		goto err;
	info->wls_flip_voter = mca_find_votable("wls_thermal_flip");
	if (!info->wls_flip_voter)
		goto err;

	info->flip_ctrl_info.voter_ok = 1;
	info->wls_flip_ctrl_info.voter_ok = 1;

	return 0;

err:
	mca_log_err("init flip voter failed\n");
	return -1;
}

/*
 * The charging paths register their elections as they probe.  Keep looking
 * until every election this board needs exists; the current level is only
 * applied once nothing earlier in the chain is still missing.
 */
static void mca_charger_thermal_init_voter_work(struct work_struct *work)
{
	struct mca_thermal_info *info =
		container_of(work, struct mca_thermal_info,
			     init_voter_work.work);
	int ret = 0;

	if (!info->wired_ctrl_info.voter_ok) {
		ret = mca_charger_thermal_init_wired_voter(info);
		if (!ret)
			mca_charger_thermal_apply_wired(info);
	}

	if (info->support_wireless && !info->wireless_ctrl_info.voter_ok) {
		ret |= mca_charger_thermal_init_wireless_voter(info);
		if (!ret)
			mca_charger_thermal_apply_wireless(info);
	}

	if (info->support_base_flip) {
		if (info->flip_ctrl_info.voter_ok &&
		    info->wls_flip_ctrl_info.voter_ok)
			return;

		if (!info->flip_ctrl_info.voter_ok)
			ret |= mca_charger_thermal_init_flip_voter(info);

		if (!ret) {
			mca_charger_thermal_apply_flip(info);
			mca_charger_thermal_apply_wls_flip(info);
			return;
		}
	}

	if (ret)
		queue_delayed_work(system_wq, &info->init_voter_work,
				   msecs_to_jiffies(MCA_THERMAL_VOTER_RETRY_MS));
}

static ssize_t mca_charger_thermal_sysfs_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf);
static ssize_t mca_charger_thermal_sysfs_store(struct device *dev,
					       struct device_attribute *attr,
					       const char *buf, size_t count);

static struct mca_sysfs_attr_info mca_charger_thermal_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRED_CHG_CURR, wired_chg_curr),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRED_CHG_CURR2, wired_chg_curr2),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRED_CTRL_LIMIT, wired_ctrl_limit),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRED_THERMAL_REMOVE,
			  wired_thermal_remove),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRELESS_CHG_CURR, wireless_chg_curr),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRELESS_CTRL_LIMIT, wireless_ctrl_limit),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WLS_QUICK_CHG_CTRL_LIMIT,
			  wls_quick_chg_control_limit),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRELESS_THERMAL_REMOVE,
			  wireless_thermal_remove),
	mca_sysfs_attr_rw(mca_charger_thermal_sysfs, 0664,
			  MCA_THERMAL_WIRELESS_MAG_CTRL_LIMIT,
			  wireless_mag_ctrl_limit),
};

static struct attribute *mca_charger_thermal_sysfs_attrs[
	ARRAY_SIZE(mca_charger_thermal_sysfs_field_tbl) + 1];

static const struct attribute_group mca_charger_thermal_sysfs_attr_group = {
	.attrs = mca_charger_thermal_sysfs_attrs,
};

static ssize_t mca_charger_thermal_sysfs_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct mca_thermal_info *info = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *attr_info;
	int val;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  mca_charger_thermal_sysfs_field_tbl,
					  ARRAY_SIZE(mca_charger_thermal_sysfs_field_tbl));
	if (!attr_info || !info)
		return -1;

	switch (attr_info->sysfs_attr_name) {
	case MCA_THERMAL_WIRED_CHG_CURR:
	case MCA_THERMAL_WIRED_CHG_CURR2:
		/* Reported in microamps, the unit it is written in. */
		val = info->wired_ctrl_info.chg_curr * 1000;
		break;
	case MCA_THERMAL_WIRED_CTRL_LIMIT:
		val = info->wired_ctrl_info.cur_level;
		break;
	case MCA_THERMAL_WIRED_THERMAL_REMOVE:
		val = info->wired_ctrl_info.thermal_remove;
		break;
	case MCA_THERMAL_WIRELESS_CHG_CURR:
		val = info->wireless_ctrl_info.chg_curr;
		break;
	case MCA_THERMAL_WIRELESS_CTRL_LIMIT:
		val = info->wireless_ctrl_info.cur_level;
		break;
	case MCA_THERMAL_WLS_QUICK_CHG_CTRL_LIMIT:
		val = info->wireless_ctrl_info.quickchg_limit_level;
		break;
	case MCA_THERMAL_WIRELESS_THERMAL_REMOVE:
		val = info->wireless_ctrl_info.wls_thermal_remove;
		break;
	case MCA_THERMAL_WIRELESS_MAG_CTRL_LIMIT:
		val = info->wireless_ctrl_info.magnet_limit_level;
		break;
	default:
		return 0;
	}

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

/*
 * A magnetic case on a pad that is not itself magnetic is governed by the
 * magnet level rather than the ordinary one.
 */
static bool mca_charger_thermal_magnet_limited(struct mca_thermal_info *info)
{
	return info->support_mag_wls_thermal && info->use_magnet &&
	       !info->use_magnet_tx;
}

/*
 * A board may name the battery cells the userspace current was measured
 * against; then only those cells may be given it.  wired_chg_curr checks the
 * first name (and is open when the board names none), wired_chg_curr2 the
 * second (and is closed unless the board names it).
 */
static bool mca_charger_thermal_sic_matches(struct mca_thermal_info *info,
					    int index)
{
	const char *sel = info->sic_select[index];

	return !strncmp(sel, info->batt_cell_name, strlen(sel));
}

static ssize_t mca_charger_thermal_sysfs_store(struct device *dev,
					       struct device_attribute *attr,
					       const char *buf, size_t count)
{
	struct mca_thermal_info *info = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *attr_info;
	int val;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  mca_charger_thermal_sysfs_field_tbl,
					  ARRAY_SIZE(mca_charger_thermal_sysfs_field_tbl));
	if (!attr_info || !info)
		return -1;

	if (!info->batt_cell_name) {
		platform_fg_ops_get_batt_cell_info(FG_IC_MASTER,
						   &info->batt_cell_name);
		mca_log_info("batt_cell_name: %s\n", info->batt_cell_name);
	}

	switch (attr_info->sysfs_attr_name) {
	case MCA_THERMAL_WIRED_CHG_CURR:
		if (info->not_support_sic)
			return count;
		if (info->sic_select_cnt > 0 && info->sic_select[0] &&
		    info->batt_cell_name &&
		    !mca_charger_thermal_sic_matches(info, 0))
			return count;
		if (sscanf(buf, "%d\n", &val) != 1)
			return -1;
		/* Written in microamps; the tables are in milliamps. */
		info->wired_ctrl_info.sic_chg_curr = val / 1000;
		mca_log_info("set wired_chg_curr: %d\n", val);
		mca_charger_thermal_update_chg_curr(info);
		mca_charger_thermal_apply_wired(info);
		break;
	case MCA_THERMAL_WIRED_CHG_CURR2:
		if (info->not_support_sic || info->sic_select_cnt < 2 ||
		    !info->sic_select[1] || !info->batt_cell_name ||
		    !mca_charger_thermal_sic_matches(info, 1))
			return count;
		if (sscanf(buf, "%d\n", &val) != 1)
			return -1;
		info->wired_ctrl_info.sic_chg_curr = val / 1000;
		mca_log_info("set wired_chg_curr2: %d\n", val);
		mca_charger_thermal_update_chg_curr(info);
		mca_charger_thermal_apply_wired(info);
		break;
	case MCA_THERMAL_WIRED_CTRL_LIMIT:
		if (sscanf(buf, "%d\n", &val) != 1)
			return -1;
		info->wired_ctrl_info.cur_level = val;
		mca_log_info("set wired limit level %d\n", val);
		mca_charger_thermal_apply_wired(info);
		if (info->support_base_flip) {
			info->flip_ctrl_info.cur_level = val;
			mca_charger_thermal_apply_flip(info);
		}
		break;
	case MCA_THERMAL_WIRED_THERMAL_REMOVE:
		if (sscanf(buf, "%d\n", &val) != 1)
			return -1;
		info->wired_ctrl_info.thermal_remove = val;
		mca_log_err("set wired thermal remove: %d\n", val);
		mca_charger_thermal_apply_wired(info);
		if (info->support_base_flip) {
			info->flip_ctrl_info.thermal_remove = val;
			mca_charger_thermal_apply_flip(info);
		}
		break;
	case MCA_THERMAL_WIRELESS_CHG_CURR:
		if (sscanf(buf, "%d\n", &val) != 1)
			return -1;
		info->wireless_ctrl_info.chg_curr = val / 1000;
		mca_log_info("set wireless chg cur %d\n",
			     info->wireless_ctrl_info.chg_curr);
		mca_charger_thermal_apply_wireless(info);
		break;
	case MCA_THERMAL_WIRELESS_CTRL_LIMIT:
		if (sscanf(buf, "%d\n", &val) != 1)
			return -1;
		info->wireless_ctrl_info.cur_level = val;
		/* Under a magnetic case the magnet level is in charge. */
		if (mca_charger_thermal_magnet_limited(info))
			return count;
		mca_log_info(" %d %d %d\n", info->support_mag_wls_thermal,
			     info->use_magnet, info->use_magnet_tx);
		mca_log_info("set wireless limit level %d\n", val);
		mca_charger_thermal_apply_wireless(info);
		if (info->support_base_flip) {
			info->wls_flip_ctrl_info.cur_level = val;
			mca_charger_thermal_apply_wls_flip(info);
		}
		break;
	case MCA_THERMAL_WLS_QUICK_CHG_CTRL_LIMIT:
		if (sscanf(buf, "%d\n", &val) != 1)
			return -1;
		info->wireless_ctrl_info.quickchg_limit_level = val;
		mca_log_info("set super wireless limit level %d\n", val);
		mca_charger_thermal_apply_wireless(info);
		break;
	case MCA_THERMAL_WIRELESS_THERMAL_REMOVE:
		if (sscanf(buf, "%d\n", &val) != 1)
			return -1;
		info->wireless_ctrl_info.wls_thermal_remove = val;
		mca_log_err("set wireless thermal remove: %d\n", val);
		mca_charger_thermal_apply_wireless(info);
		if (info->support_base_flip) {
			info->wls_flip_ctrl_info.wls_thermal_remove = val;
			mca_charger_thermal_apply_wls_flip(info);
		}
		break;
	case MCA_THERMAL_WIRELESS_MAG_CTRL_LIMIT:
		if (sscanf(buf, "%d\n", &val) != 1)
			return -1;
		info->wireless_ctrl_info.magnet_limit_level = val;
		mca_log_info("%d %d %d\n", info->support_mag_wls_thermal,
			     info->use_magnet, info->use_magnet_tx);
		if (!mca_charger_thermal_magnet_limited(info))
			return count;
		mca_log_info("set wireless mag limit level %d\n", val);
		mca_charger_thermal_apply_wireless(info);
		if (info->support_base_flip) {
			info->wls_flip_ctrl_info.magnet_limit_level = val;
			mca_charger_thermal_apply_wls_flip(info);
		}
		break;
	default:
		break;
	}

	return count;
}

static int mca_charger_thermal_process_event(int event, int value, void *data)
{
	struct mca_thermal_info *info = data;

	if (!info)
		return -1;

	mca_log_info("receive event %d, value %d\n", event, value);

	switch (event) {
	case MCA_EVENT_USB_DISCONNECT:
	case MCA_EVENT_USB_CONNECT:
		info->usb_online = value;
		if (!value) {
			mca_charger_thermal_poweroff_clear_voter(info);
			break;
		}
		mca_charger_thermal_apply_wired(info);
		if (info->support_base_flip)
			mca_charger_thermal_apply_flip(info);
		break;
	case MCA_EVENT_WIRELESS_DISCONNECT:
	case MCA_EVENT_WIRELESS_CONNECT:
		info->wls_online = value;
		/* The level for a coil is written by userspace once it is up. */
		if (value)
			break;
		mca_wireless_charger_thermal_poweroff_clear_voter(info);
		if (info->support_base_flip)
			mca_charger_thermal_apply_wls_flip(info);
		break;
	case MCA_EVENT_CHARGE_TYPE_CHANGE:
		info->real_type = value;
		if (!info->not_support_sic)
			mca_charger_thermal_sic_initial_chg_curr(info);
		break;
	case MCA_EVENT_CHARGE_CAP_CHANGE:
		if (!info->not_support_sic)
			mca_charger_thermal_sic_initial_chg_curr(info);
		break;
	case MCA_EVENT_WIRELESS_EPP_MODE:
		if (info->wls_online)
			mca_charger_thermal_apply_wireless(info);
		break;
	case MCA_EVENT_WIRELESS_MAGNETIC_CASE_INT:
		mca_log_info("use magnet: %d\n", value);
		info->use_magnet = value;
		mca_charger_thermal_apply_wireless(info);
		break;
	case MCA_EVENT_WIRELESS_MAGNETIC_TX_INT:
		mca_log_info("use magnet TX: %d\n", value);
		info->use_magnet_tx = value;
		break;
	case MCA_EVENT_DEBUG_CTRL_DOUBLE85:
	case MCA_EVENT_DEBUG_CTRL_MEMORY_TEST:
		/*
		 * Both of these run the phone hot on purpose, which is exactly
		 * what the limits exist to prevent.
		 */
		mca_log_info("double85/memory_test remove thermal limit: %d\n",
			     value);
		if (!value) {
			info->wired_ctrl_info.thermal_remove = 0;
			info->wireless_ctrl_info.wls_thermal_remove = 0;
			break;
		}
		info->wired_ctrl_info.thermal_remove = 1;
		info->wireless_ctrl_info.wls_thermal_remove = 1;
		mca_charger_thermal_poweroff_clear_voter(info);
		mca_wireless_charger_thermal_poweroff_clear_voter(info);
		break;
	default:
		break;
	}

	return 0;
}

static int mca_charge_thermal_dump_log_head(void *data, char *buf, int size)
{
	return snprintf(buf, size, "W_TL W_SIC   WL_TL WL_STL WL_SIC   ");
}

static int mca_charge_thermal_dump_log_context(void *data, char *buf, int size)
{
	struct mca_thermal_info *info = data;

	if (!info)
		return snprintf(buf, size, "%-5d%-8d%-6d%-7d%-9d", -1, -1, -1,
				-1, -1);

	return snprintf(buf, size, "%-5d%-8d%-6d%-7d%-9d",
			info->wired_ctrl_info.cur_level,
			info->wired_ctrl_info.chg_curr,
			info->wireless_ctrl_info.cur_level,
			info->wireless_ctrl_info.quickchg_limit_level,
			info->wireless_ctrl_info.chg_curr);
}

static struct mca_log_charge_log_ops g_mca_thermal_log_ops = {
	.dump_log_head		= mca_charge_thermal_dump_log_head,
	.dump_log_context	= mca_charge_thermal_dump_log_context,
};

static struct mca_smartchg_if_ops g_thermal_smartchg_if_ops = {
	.type			= MCA_SMARTCHG_IF_CHG_TYPE_THERMAL,
	.set_wls_super_sts	= mca_charger_thermal_wls_super_sts_callback,
};

/*
 * One row of the property per thermal level above zero, each row as wide as
 * there are modes.  Returns the number of levels, or a negative error.
 */
static int mca_charger_thermal_parse_table(struct device_node *np,
					   const char *prop, int *table,
					   int row_size)
{
	int count, rows;
	int i;

	count = mca_parse_dts_u32_count(np, prop, MCA_THERMAL_LEVEL_MAX,
					row_size);
	if (count < 0) {
		mca_log_err("parse %s failed", prop);
		return -EINVAL;
	}

	if (mca_parse_dts_u32_array(np, prop, (u32 *)table, count)) {
		mca_log_err("parse %s data failed", prop);
		return -EINVAL;
	}

	rows = count / row_size;
	for (i = 0; i < rows; i++) {
		const int *row = &table[i * row_size];

		mca_log_debug("[%s]level-%d %d %d %d %d %d %d %d %d %d %d\n",
			      prop, i, row[0], row[1], row[2], row[3], row[4],
			      row[5], row[6], row[7], row[8], row[9]);
	}

	return rows;
}

/* The base cell has a single limit per level rather than a row of them. */
static void mca_charger_thermal_flip_parse_data(const char *prop,
						struct device_node *np,
						int *data, int *limit_level)
{
	int i;

	mca_parse_dts_u32_array(np, prop, (u32 *)data, MCA_THERMAL_LEVEL_MAX);
	for (i = 0; i < MCA_THERMAL_LEVEL_MAX; i++)
		mca_log_err("thermal:%d flip curr:%d", i, data[i]);

	*limit_level = MCA_THERMAL_FLIP_LEVEL_MAX;
}

static int mca_charger_thermal_parse_dt(struct mca_thermal_info *info)
{
	struct device_node *np = info->dev->of_node;
	u32 not_support_sic[2] = { 0 };
	int ret;
	int i;

	mca_parse_dts_u32(np, "support_wireless",
			  (u32 *)&info->support_wireless, 0);
	mca_parse_dts_u32(np, "wireless_phone_level",
			  (u32 *)&info->wireless_phone_level, 12);

	/*
	 * Two numbers: whether userspace may set a current at all, and what
	 * to use instead when it may not.
	 */
	if (mca_parse_dts_u32_array(np, "not_support_sic", not_support_sic,
				    ARRAY_SIZE(not_support_sic))) {
		not_support_sic[0] = 0;
		not_support_sic[1] = 0;
	}
	info->not_support_sic = not_support_sic[0];
	info->not_support_sic_curr = not_support_sic[1];

	info->support_base_flip = of_property_read_bool(np,
							 "support-base-flip");
	info->support_mag_wls_thermal =
		of_property_read_bool(np, "support-wls-mag-thermal");
	mca_log_err("parse support_mag_wls_thermal %d\n",
		    info->support_mag_wls_thermal);

	ret = mca_charger_thermal_parse_table(np, "wired_thermal",
					      &info->wired_thermal_data[0][0],
					      THERMAL_MODE_MAX);
	if (ret < 0) {
		mca_log_err("parse wire thermal failed\n");
		return ret;
	}
	info->wired_ctrl_info.limit_level = ret;

	if (info->support_wireless) {
		ret = mca_charger_thermal_parse_table(np, "wireless_thermal",
						      &info->wireless_thermal_data[0][0],
						      THERMAL_MODE_WIRELESS_MAX);
		if (ret < 0) {
			mca_log_err("parse wireless thermal failed\n");
			return ret;
		}
		info->wireless_ctrl_info.limit_level = ret;
	}

	if (info->support_base_flip) {
		mca_charger_thermal_flip_parse_data("wired_flip_thermal", np,
						    info->wired_flip_thermal,
						    &info->flip_ctrl_info.limit_level);
		mca_charger_thermal_flip_parse_data("wireless_flip_thermal", np,
						    info->wireless_flip_thermal,
						    &info->wls_flip_ctrl_info.limit_level);
	}

	info->sic_select_cnt =
		of_property_read_string_array(np, "wired_sic_select",
					      info->sic_select,
					      ARRAY_SIZE(info->sic_select));
	if (info->sic_select_cnt < 0) {
		mca_log_err("get wired_sic_select count failed\n");
		return info->sic_select_cnt;
	}

	for (i = 0; i < info->sic_select_cnt; i++)
		mca_log_err("wired_sic_select[%d] = %s\n", i,
			    info->sic_select[i]);

	return 0;
}

static int mca_charger_thermal_probe(struct platform_device *pdev)
{
	struct mca_thermal_info *info;
	int ret;

	mca_log_info("probe begin\n");

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info) {
		mca_log_err("out of memory\n");
		return -ENOMEM;
	}

	info->dev = &pdev->dev;
	platform_set_drvdata(pdev, info);

	/* A table that fails to parse leaves its path unlimited, not absent. */
	if (info->dev->of_node)
		mca_charger_thermal_parse_dt(info);

	INIT_DELAYED_WORK(&info->init_voter_work,
			  mca_charger_thermal_init_voter_work);

	mca_sysfs_init_attrs(mca_charger_thermal_sysfs_attrs,
			     mca_charger_thermal_sysfs_field_tbl,
			     ARRAY_SIZE(mca_charger_thermal_sysfs_field_tbl));
	mca_sysfs_create_link_group(MCA_SYSFS_DEV_CHARGER, "charger_thermal",
				    info->dev,
				    &mca_charger_thermal_sysfs_attr_group);

	mca_strategy_ops_register(STRATEGY_FUNC_TYPE_THERMAL,
				  mca_charger_thermal_process_event, NULL,
				  NULL, info);

	ret = mca_charger_thermal_init_wired_voter(info);
	if (info->support_wireless)
		ret |= mca_charger_thermal_init_wireless_voter(info);
	if (ret)
		queue_delayed_work(system_wq, &info->init_voter_work,
				   msecs_to_jiffies(MCA_THERMAL_VOTER_RETRY_MS));

	/*
	 * One cooling device covers every mode: the framework asks for one
	 * level, and which currents that level means is decided here.
	 */
	info->cdev = devm_thermal_of_cooling_device_register(info->dev,
							     info->dev->of_node,
							     "battery", info,
							     &g_mca_charger_tcd_ops);
	if (IS_ERR_OR_NULL(info->cdev)) {
		mca_log_err("register cooling device failed\n");
		/* Nothing may keep a pointer to the memory devm frees now. */
		cancel_delayed_work_sync(&info->init_voter_work);
		mca_strategy_ops_register(STRATEGY_FUNC_TYPE_THERMAL, NULL,
					  NULL, NULL, NULL);
		mca_sysfs_remove_link_group(MCA_SYSFS_DEV_CHARGER,
					    "charger_thermal", info->dev,
					    &mca_charger_thermal_sysfs_attr_group);
		return -1;
	}

	g_thermal_smartchg_if_ops.data = info;
	mca_smartchg_if_ops_register(&g_thermal_smartchg_if_ops);

	g_wlscharger_thermal_info = info;
	mca_log_charge_log_register(MCA_CHARGE_LOG_ID_THERMAL,
				    &g_mca_thermal_log_ops, info);

	mca_log_err("probe end\n");

	return 0;
}

static int mca_charger_thermal_remove(struct platform_device *pdev)
{
	struct mca_thermal_info *info = platform_get_drvdata(pdev);

	mca_sysfs_remove_link_group(MCA_SYSFS_DEV_CHARGER, "charger_thermal",
				    &pdev->dev,
				    &mca_charger_thermal_sysfs_attr_group);

	/*
	 * The driver data is freed after this returns: drop every reference
	 * other modules or the workqueue could still follow into it.
	 */
	mca_strategy_ops_register(STRATEGY_FUNC_TYPE_THERMAL, NULL, NULL, NULL,
				  NULL);
	g_thermal_smartchg_if_ops.data = NULL;
	mca_log_charge_log_register(MCA_CHARGE_LOG_ID_THERMAL,
				    &g_mca_thermal_log_ops, NULL);
	g_wlscharger_thermal_info = NULL;
	cancel_delayed_work_sync(&info->init_voter_work);

	return 0;
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca_charger_thermal" },
	{ }
};

static struct platform_driver mca_charger_thermal_driver = {
	.driver = {
		.name		= "mca_charger_thermal",
		.of_match_table	= match_table,
	},
	.probe		= mca_charger_thermal_probe,
	.remove		= mca_charger_thermal_remove,
};
module_platform_driver(mca_charger_thermal_driver);

MODULE_DESCRIPTION("charge thermal");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
