// SPDX-License-Identifier: GPL-2.0
/*
 * mca_smart_charge.c
 *
 * smart_charge driver
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
#define MCA_LOG_TAG "mca_smart_charge"
#endif

#include <linux/bits.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/hwid.h>
#include <linux/jiffies.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <mca/common/mca_charge_mievent.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_hwid.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/common/mca_sysfs.h>
#include <mca/common/mca_voter.h>
#include <mca/shared_memory/charger_partition_class.h>
#include <mca/smartchg/smart_chg_class.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include "inc/mca_smart_charge.h"

#define SMART_CHG_SOCLMT_TRIG_DEFAULT 80
#define SMART_CHG_SOCLMT_CANCEL_DEFAULT 5
#define SMART_CHG_SOCLMT_REPORT_HYS 3
#define SMART_CHARGE_DELTA_FV_DEFAULT_VALUE 0
#define SMART_CHARGE_DELTA_ICHG_DEFAULT_VALUE 0
#define SMART_CHARGE_DTPT_DELTA_VALUE 15
#define SMART_CHARGE_FAKE_SCENE_DEFAULT -1
#define SMART_CHARGE_WORK_PERIOD_MS 5000
#define SMART_CHARGE_SOC_LIMIT_PERIOD_MS 1000

/* The float voltage step userspace asks for, and what one project takes. */
#define SMARTCHG_FV_STEP_DEFAULT_MV 15
#define SMARTCHG_FV_STEP_LARGE_MV 20
#define SMARTCHG_FV_STEP_LARGE_PROJECT 7

/* Battery swelling check (CSD): when the gauge is asked for a pulse. */
#define SMART_CHG_CSD_SOC_MIN 50
#define SMART_CHG_CSD_SOC_MAX 80
#define SMART_CHG_CSD_TEMP_MIN 20
#define SMART_CHG_CSD_TEMP_MAX 45
#define SMART_CHG_CSD_PLUGIN_RSOC_MAX 45
#define SMART_CHG_CSD_CYCLE_MIN 100
#define SMART_CHG_CSD_CYCLE_PERIOD 25
#define SMART_CHG_CSD_PULSE_RSOC_LOW 50
#define SMART_CHG_CSD_PULSE_RSOC_HIGH 76

enum mca_smart_charge_ioctrl_cmd {
	MCA_SMARTCHG_CMD_DATA = 1,
	MCA_SMARTCHG_CMD_INVALID,
};

#define MCA_SMARTCHG_MAGIC 0x10
#define SMARTCHG_DATA_CMD                                \
	_IOC(_IOC_READ | _IOC_WRITE, MCA_SMARTCHG_MAGIC, \
	     MCA_SMARTCHG_CMD_DATA, PAGE_SIZE)

static struct smart_charge_info *global_smartchg_info;
static struct mca_smartchg_if_ops
	*g_mca_smartchg_if_ops[MCA_SMARTCHG_IF_CHG_TYPE_END];

static void smartcharging_handle_algorithm_conflict(struct smart_charge_info *info);

static struct mca_smartchg_if_ops *mca_smartchg_if_get_ops(unsigned int type)
{
	return g_mca_smartchg_if_ops[type];
}

int mca_smartchg_if_ops_register(struct mca_smartchg_if_ops *ops)
{
	if (!ops) {
		mca_log_err("ops invalid\n");
		return -1;
	}

	if (ops->type < 0 || ops->type >= MCA_SMARTCHG_IF_CHG_TYPE_END) {
		mca_log_err("ops->type invalid %d\n", ops->type);
		return -1;
	}

	g_mca_smartchg_if_ops[ops->type] = ops;
	return 0;
}
EXPORT_SYMBOL(mca_smartchg_if_ops_register);

static ssize_t smart_charge_sysfs_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf);
static ssize_t smart_charge_sysfs_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count);

struct mca_sysfs_attr_info smartchg_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_SMARTCHG,
			  smart_chg),
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_SMARTCHG_FV,
			  smart_fv),
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_SMARTCHG_ICHG,
			  smart_ichg),
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_SMARTBATT,
			  smart_batt),
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_SMARTNIGHT,
			  smart_night),
	mca_sysfs_attr_ro(smart_charge_sysfs, 0440, MCA_PROP_POSTURE, posture),
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_SCENE, scene),
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_BOARD_TEMP,
			  board_temp),
	mca_sysfs_attr_rw(smart_charge_sysfs, 0664, MCA_PROP_SMART_SIC_MODE,
			  smart_sic_mode),
};

#define SMARTCHG_SYSFS_ATTRS_SIZE ARRAY_SIZE(smartchg_sysfs_field_tbl)

static struct attribute *smartchg_sysfs_attrs[SMARTCHG_SYSFS_ATTRS_SIZE + 1];

static const struct attribute_group smartchg_sysfs_attr_group = {
	.attrs = smartchg_sysfs_attrs,
};

/*
 * Tell userspace when the phone enters or leaves one of the scenes it
 * changes its charging behaviour for; every other scene reads as 0.
 */
static void smart_charge_report_scene(struct smart_charge_info *info, int scene)
{
	static int last_report_scene;
	char event[MCA_EVENT_NOTIFY_SIZE] = { 0 };
	struct mca_event_notify_data event_data = { 0 };
	int report_scene;

	if (scene == info->scene)
		return;

	switch (scene) {
	case SMART_CHG_SCENE_PHONE:
	case SMART_CHG_SCENE_CLASS0:
	case SMART_CHG_SCENE_YOUTUBE:
	case SMART_CHG_SCENE_VIDEO:
	case SMART_CHG_SCENE_VIDEOCHAT:
	case SMART_CHG_SCENE_TGAME:
	case SMART_CHG_SCENE_MGAME:
	case SMART_CHG_SCENE_YUANSHEN:
	case SMART_CHG_SCENE_XINGTIE:
	case SMART_CHG_SCENE_DANMU:
	case SMART_CHG_SCENE_PER_CLASS0:
	case SMART_CHG_SCENE_PER_YOUTUBE:
	case SMART_CHG_SCENE_PER_VIDEO:
	case SMART_CHG_SCENE_PER_DANMU:
	case SMART_CHG_SCENE_CGAME:
	case SMART_CHG_SCENE_CGAME_2:
		report_scene = scene;
		break;
	default:
		report_scene = 0;
		break;
	}

	if (report_scene == last_report_scene)
		return;

	mca_log_info("report scene: %d => %d\n", last_report_scene,
		     report_scene);
	last_report_scene = report_scene;

	event_data.event = event;
	event_data.event_len = snprintf(event, MCA_EVENT_NOTIFY_SIZE,
					"MCA_SMARTCHG_SCENE=%d", report_scene);
	mca_event_report_uevent(&event_data);
}

int mca_smartchg_set_scene(int scene)
{
	struct smart_charge_info *info = global_smartchg_info;

	if (!info) {
		mca_log_err("info is null\n");
		return -EINVAL;
	}

	mca_log_info("set scene: %d:%d => %d\n", info->scene, info->fake_scene,
		     scene);
	if (scene >= SMART_CHG_SCENE_NORMAL &&
	    scene < SMART_CHG_SCENE_INDEX_MAX) {
		smart_charge_report_scene(info, scene);
		info->scene = scene;
	} else if (scene >= SMART_CHG_SCENE_REDIR_START &&
		   scene < SMART_CHG_SCENE_REDIR_INDEX_MAX) {
		info->fake_scene = scene - SMART_CHG_SCENE_REDIR_START;
	} else if (scene == SMART_CHARGE_FAKE_SCENE_DEFAULT) {
		info->fake_scene = SMART_CHARGE_FAKE_SCENE_DEFAULT;
	}

	cancel_delayed_work_sync(&info->smart_sense_chg_work);
	schedule_delayed_work(&info->smart_sense_chg_work, 0);

	return 0;
}
EXPORT_SYMBOL(mca_smartchg_set_scene);

int mca_smartchg_get_scene(void)
{
	struct smart_charge_info *info = global_smartchg_info;

	if (!info) {
		mca_log_err("info is null\n");
		return 0;
	}

	if (info->fake_scene != SMART_CHARGE_FAKE_SCENE_DEFAULT)
		return info->fake_scene;

	return info->scene;
}
EXPORT_SYMBOL(mca_smartchg_get_scene);

int mca_smartchg_set_board_temp(int board_temp)
{
	struct smart_charge_info *info = global_smartchg_info;

	if (!info) {
		mca_log_err("info is null\n");
		return -EINVAL;
	}

	mca_log_info("set board_temp: %d\n", board_temp);
	info->board_temp = board_temp;

	return 0;
}
EXPORT_SYMBOL(mca_smartchg_set_board_temp);

int mca_smartchg_get_board_temp(void)
{
	struct smart_charge_info *info = global_smartchg_info;

	if (!info) {
		mca_log_err("info is null\n");
		return 0;
	}

	return info->board_temp;
}
EXPORT_SYMBOL(mca_smartchg_get_board_temp);

int mca_smartchg_is_extreme_cold_enabled(void)
{
	if (global_smartchg_info)
		return global_smartchg_info->smart_chg_data
			       .smart_chgcfg[SMART_CHG_EXTREME_COLD]
			       .enable == 1;
	return false;
}
EXPORT_SYMBOL(mca_smartchg_is_extreme_cold_enabled);

int mca_smartchg_get_limit_soc(void)
{
	struct smart_charge_info *info = global_smartchg_info;

	if (!info) {
		mca_log_err("info is null\n");
		return 0;
	}
	if (info->soc_limit_enable)
		return info->soc_limit;
	return 0;
}
EXPORT_SYMBOL(mca_smartchg_get_limit_soc);

static void smart_charge_handle_sense_chg(struct smart_charge_info *info)
{
	struct SMART_CHG_DATA *sense =
		&info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG];
	char event[MCA_EVENT_NOTIFY_SIZE] = { 0 };
	struct mca_event_notify_data event_data = { 0 };
	int sic_mode = SMART_CHG_SIC_MODE_BALANCED;
	int posture_st;
	int scene;

	scene = info->fake_scene != SMART_CHARGE_FAKE_SCENE_DEFAULT ?
			info->fake_scene : info->scene;
	posture_st = sense->use_fake_value ? sense->fake_value :
					     sense->func_value;

	if (!sense->enable || posture_st <= SMART_CHG_POSTURE_UNKNOW ||
	    posture_st >= SMART_CHG_POSTURE_INDEX_MAX)
		return;

	switch (scene) {
	case SMART_CHG_SCENE_PHONE:
		if (posture_st <= SMART_CHG_POSTURE_HOLDER)
			sic_mode = SMART_CHG_SIC_MODE_SUPERCHG;
		else if (posture_st <= SMART_CHG_POSTURE_TWOHAND_V)
			sic_mode = SMART_CHG_SIC_MODE_MIDDLE;
		else
			sic_mode = SMART_CHG_SIC_MODE_BALANCED;
		break;
	case SMART_CHG_SCENE_YOUTUBE:
	case SMART_CHG_SCENE_VIDEO:
	case SMART_CHG_SCENE_DANMU:
	case SMART_CHG_SCENE_PER_YOUTUBE:
	case SMART_CHG_SCENE_PER_VIDEO:
	case SMART_CHG_SCENE_PER_DANMU:
		/*
		 * Watching something two-handed leaves the phone somewhere it
		 * can still lose heat, so a board that asks for the finer
		 * scheme charges harder there than it would in the hand.
		 */
		if (info->support_sensechg_v3) {
			if (posture_st <= SMART_CHG_POSTURE_HOLDER)
				sic_mode = SMART_CHG_SIC_MODE_SUPERCHG;
			else if (posture_st == SMART_CHG_POSTURE_TWOHAND_H ||
				 posture_st == SMART_CHG_POSTURE_TWOHAND_V)
				sic_mode = SMART_CHG_SIC_MODE_MIDDLE;
			else
				sic_mode = SMART_CHG_SIC_MODE_BALANCED;
			break;
		}
		fallthrough;
	case SMART_CHG_SCENE_VIDEOCHAT:
		if (posture_st <= SMART_CHG_POSTURE_HOLDER)
			sic_mode = SMART_CHG_SIC_MODE_SUPERCHG;
		else
			sic_mode = SMART_CHG_SIC_MODE_BALANCED;
		break;
	case SMART_CHG_SCENE_CLASS0:
	case SMART_CHG_SCENE_PER_CLASS0:
		if (!info->support_sensechg_v2) {
			mca_log_err("unsupport scene: %d\n", info->scene);
			return;
		}
		if (posture_st <= SMART_CHG_POSTURE_HOLDER)
			sic_mode = SMART_CHG_SIC_MODE_SUPERCHG;
		else
			sic_mode = SMART_CHG_SIC_MODE_BALANCED;
		break;
	case SMART_CHG_SCENE_TGAME:
	case SMART_CHG_SCENE_MGAME:
	case SMART_CHG_SCENE_YUANSHEN:
	case SMART_CHG_SCENE_XINGTIE:
	case SMART_CHG_SCENE_PER_XINGTIE:
	case SMART_CHG_SCENE_CGAME:
	case SMART_CHG_SCENE_CGAME_2:
		/*
		 * A game is played held, so only the two postures that put a
		 * hand over the back of the phone hold the charge back.
		 */
		if (!info->support_sensechg_v3) {
			mca_log_err("unsupport scene: %d\n", info->scene);
			return;
		}
		if (posture_st == SMART_CHG_POSTURE_TWOHAND_H ||
		    posture_st == SMART_CHG_POSTURE_ANS_CALL)
			sic_mode = SMART_CHG_SIC_MODE_BALANCED;
		else
			sic_mode = SMART_CHG_SIC_MODE_SUPERCHG;
		break;
	default:
		mca_log_err("unsupport scene: %d\n", info->scene);
		return;
	}

	/* With the screen off, one step gentler. */
	if (info->screen_state == 0)
		sic_mode /= 2;

	mca_log_info("active_stat: %d, online: %d, screen_state: %d, scene: %d, posture: %d, smart_sic_mode: %d -> %d\n",
		     true, info->online, info->screen_state, scene, posture_st,
		     info->smart_sic_mode, sic_mode);
	if (sic_mode == info->smart_sic_mode)
		return;

	info->smart_sic_mode = sic_mode;
	if (!info->online)
		return;

	event_data.event = event;
	event_data.event_len = snprintf(event, MCA_EVENT_NOTIFY_SIZE,
					"POWER_SUPPLY_SMART_SIC_MODE=%d",
					info->smart_sic_mode);
	mca_event_report_uevent(&event_data);
}

static void smart_charge_set_soc_limit(bool enable)
{
	static struct mca_smartchg_if_ops *if_ops;

	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		if_ops = mca_smartchg_if_get_ops(i);
		if (!if_ops || !if_ops->set_soc_limit_sts)
			continue;
		if_ops->set_soc_limit_sts(if_ops->data, enable);
	}
}

/*
 * Hold the battery at soc_limit while charging: stop at the limit and only
 * start again once it has dropped SMART_CHG_SOCLMT_CANCEL_DEFAULT below it.
 */
static void smart_charge_soc_limit_workfunc(struct work_struct *work)
{
	struct smart_charge_info *info = container_of(
		work, struct smart_charge_info, smart_soc_limit_work.work);
	struct SMART_CHG_DATA *chgcfg = info->smart_chg_data.smart_chgcfg;
	int curr_soc = 0;

	if (!info->online) {
		mca_log_err("adapter is plugout, don't handle soc limit\n");
		return;
	}

	if (!info->soc_limit) {
		mca_log_info("disable soc limit\n");
		info->soc_limit_enable = false;
		smart_charge_set_soc_limit(info->soc_limit_enable);
		goto out;
	}

	curr_soc = strategy_class_fg_ops_get_soc();
	mca_log_info("curr_soc = %d, soc_limit_thre = %d\n", curr_soc,
		     info->soc_limit);
	if (curr_soc >= info->soc_limit) {
		if (!info->soc_limit_enable) {
			info->soc_limit_enable = true;
			smart_charge_set_soc_limit(info->soc_limit_enable);
		}

		if (curr_soc >= info->soc_limit + SMART_CHG_SOCLMT_REPORT_HYS) {
			if (chgcfg[SMART_CHG_ENDURANCE_PRO].enable &&
			    !info->ignore_upload.endurance_pro) {
				mca_charge_mievent_report(CHARGE_DFX_SMART_ENDURANCE_SOC_ERR,
							  &curr_soc, 1);
				info->ignore_upload.endurance_pro = true;
			}
			/*
			 * This marks the endurance report as sent, not the
			 * navigation one, so navigation keeps reporting.
			 */
			if (chgcfg[SMART_CHG_NAVIGATION].enable &&
			    !info->ignore_upload.navigation) {
				mca_charge_mievent_report(CHARGE_DFX_SMART_NAVIGATION_SOC_ERR,
							  &curr_soc, 1);
				info->ignore_upload.endurance_pro = true;
			}
		}
	} else if (curr_soc <=
		   info->soc_limit - SMART_CHG_SOCLMT_CANCEL_DEFAULT &&
		   info->soc_limit_enable) {
		info->soc_limit_enable = false;
		smart_charge_set_soc_limit(info->soc_limit_enable);
		info->ignore_upload.navigation = false;
		info->ignore_upload.endurance_pro = false;
		mca_log_info("recover smart_chg endurance_pro and navigation report\n");
	}
	schedule_delayed_work(&info->smart_soc_limit_work,
			      msecs_to_jiffies(SMART_CHARGE_SOC_LIMIT_PERIOD_MS));

out:
	mca_log_info("soc_limit_enable = %d\n", info->soc_limit_enable);
}

/*
 * Pick the battery level to stop charging at.  Endurance protection wins over
 * navigation, navigation over night charging, and a shop display phone is
 * held part-charged when nothing else asks for a limit.
 */
static void
smartcharging_handle_algorithm_conflict(struct smart_charge_info *info)
{
	struct SMART_CHG_DATA *chgcfg = info->smart_chg_data.smart_chgcfg;
	int boot_mode = mca_log_get_charge_boot_mode();
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	u32 soc_upper_thre;

	if (chgcfg[SMART_CHG_ENDURANCE_PRO].enable) {
		soc_upper_thre = chgcfg[SMART_CHG_ENDURANCE_PRO].func_value;
		mca_log_info("smart_endurance %d, soc_upper_thre:%d", 1,
			     soc_upper_thre);
	} else if (chgcfg[SMART_CHG_NAVIGATION].enable) {
		soc_upper_thre = chgcfg[SMART_CHG_NAVIGATION].func_value;
		mca_log_info("smart_navigation %d, soc_upper_thre:%d", 1,
			     soc_upper_thre);
	} else if (chgcfg[SMART_CHG_NIGHT].enable) {
		soc_upper_thre = chgcfg[SMART_CHG_NIGHT].func_value;
		mca_log_info("smart_night %d, soc_upper_thre:%d", 1,
			     soc_upper_thre);
	} else if (info->mishow_config && boot_mode && hwid &&
		   hwid->platform_version >= SMART_CHG_MISHOW_PROJECT_LOW &&
		   hwid->platform_version <= SMART_CHG_MISHOW_PROJECT_HIGH) {
		soc_upper_thre = SMART_CHG_MISHOW_SOC_LIMIT;
		mca_log_info("mishow %d, soc_upper_thre:%d", info->mishow_config,
			     soc_upper_thre);
	} else {
		soc_upper_thre = 0;
	}

	info->soc_limit = soc_upper_thre;
	cancel_delayed_work_sync(&info->smart_soc_limit_work);
	schedule_delayed_work(&info->smart_soc_limit_work, 0);
}

static void smartcharging_handle_wls_super(struct smart_charge_info *info)
{
	static struct mca_smartchg_if_ops *if_ops;

	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		if_ops = mca_smartchg_if_get_ops(i);
		if (!if_ops || !if_ops->set_wls_super_sts)
			continue;
		if_ops->set_wls_super_sts(if_ops->data,
					  info->smart_chg_data.smart_chgcfg[SMART_CHG_WLS_SUPER].enable);
	}
}

static void smartcharging_handle_power_boost(struct smart_charge_info *info,
					     int type)
{
	static struct mca_smartchg_if_ops *if_ops;
	int mode = type == SMART_CHG_OUTDOOR_PWR_BOOST ? SMART_CHG_OUTDOOR :
							 SMART_CHG_TRAVELWAIT;

	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		if_ops = mca_smartchg_if_get_ops(i);
		if (!if_ops || !if_ops->set_pwr_boost_sts)
			continue;
		if_ops->set_pwr_boost_sts(if_ops->data,
					  info->smart_chg_data.smart_chgcfg[mode].enable);
	}
}

static void smartcharging_handle_wls_quiet(struct smart_charge_info *info)
{
	static struct mca_smartchg_if_ops *if_ops;

	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		if_ops = mca_smartchg_if_get_ops(i);
		if (!if_ops || !if_ops->set_wls_quiet_sts)
			continue;
		if_ops->set_wls_quiet_sts(if_ops->data,
					  info->smart_chg_data.smart_chgcfg[SMART_CHG_WLS_QUIET].enable);
	}
}

/* Turn a mode that takes no value on or off. */
static void smartcharging_set_mode(struct smart_charge_info *info, int mode)
{
	info->smart_chg_data.smart_chgcfg[mode].enable =
		info->smart_chg_control.enable;
	info->smart_chg_data.smart_chgcfg[mode].func_value = 0;
}

/* Turn a battery level cap on or off; report_index is its mievent. */
static void smartcharging_set_soc_cap(struct smart_charge_info *info, int mode,
				      int report_index, int soc)
{
	info->smart_chg_data.smart_chgcfg[mode].enable =
		info->smart_chg_control.enable;
	info->smart_chg_data.smart_chgcfg[mode].func_value =
		info->smart_chg_control.soc_limit;
	smartcharging_handle_algorithm_conflict(info);

	if (info->smart_chg_control.enable)
		mca_charge_mievent_report(report_index, &soc, 1);
	else
		mca_charge_mievent_set_state(MIEVENT_STATE_END, report_index);
}

static void smartcharging_handle_controlmessage(void)
{
	struct smart_charge_info *info = global_smartchg_info;
	union SMART_CHG_HEADER *ctrl;
	struct SMART_CHG_DATA *sense;
	u32 smart_header;
	int soc;

	soc = strategy_class_fg_ops_get_soc();
	if (!info)
		return;

	ctrl = &info->smart_chg_control;
	sense = &info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG];

	/* Exactly one mode may be named per request. */
	smart_header = ctrl->AsUINT32 & 0xFFFE;
	smart_header = (smart_header - 1) & smart_header;
	mca_log_info("smart_header: %d", smart_header);
	if (smart_header) {
		info->smart_chg_data.ret_code = true;
		mca_log_err("error input invalid\n");
		return;
	}
	info->smart_chg_data.ret_code = false;
	mca_log_err("input correct\n");

	mca_log_info("smart_chg_control: enable: %d, func_type: 0x%04x, func_value: %d\n",
		     ctrl->enable, ctrl->func_type << 1, ctrl->func_value);

	if (ctrl->navigation) {
		smartcharging_set_soc_cap(info, SMART_CHG_NAVIGATION,
					  CHARGE_DFX_SMART_NAVIGATION_TRIGGERED,
					  soc);
		info->ignore_upload.navigation = soc >= ctrl->soc_limit;
	} else if (ctrl->endurance_pro) {
		smartcharging_set_soc_cap(info, SMART_CHG_ENDURANCE_PRO,
					  CHARGE_DFX_SMART_ENDURANCE_TRIGGERED,
					  soc);
		info->ignore_upload.endurance_pro = soc >= ctrl->soc_limit;
	} else if (ctrl->outdoor) {
		smartcharging_set_mode(info, SMART_CHG_OUTDOOR);
		smartcharging_handle_power_boost(info,
						 SMART_CHG_OUTDOOR_PWR_BOOST);
	} else if (ctrl->wls_super_chg) {
		smartcharging_set_mode(info, SMART_CHG_WLS_SUPER);
		smartcharging_handle_wls_super(info);
	} else if (ctrl->lowfast) {
		smartcharging_set_mode(info, SMART_CHG_LOWFAST);
	} else if (ctrl->wls_quiet) {
		smartcharging_set_mode(info, SMART_CHG_WLS_QUIET);
		smartcharging_handle_wls_quiet(info);
	} else if (ctrl->sense_chg) {
		sense->enable = ctrl->enable;
		if (ctrl->func_value > SMART_CHG_POSTURE_FAKE_CLEAR) {
			mca_log_info("debug sense charge, set value: %d\n",
				     ctrl->func_value);
			sense->use_fake_value = true;
			sense->fake_value = ctrl->func_value -
					    SMART_CHG_POSTURE_FAKE_CLEAR;
		} else if (ctrl->func_value == SMART_CHG_POSTURE_FAKE_CLEAR) {
			mca_log_info("debug sense charge, cancel fake value: %d\n",
				     ctrl->func_value);
			sense->use_fake_value = false;
			sense->fake_value = 0;
		} else {
			sense->func_value = ctrl->func_value;
		}
		smart_charge_handle_sense_chg(info);
	} else if (ctrl->extreme_cold) {
		smartcharging_set_mode(info, SMART_CHG_EXTREME_COLD);
	} else if (ctrl->travel_wait) {
		smartcharging_set_mode(info, SMART_CHG_TRAVELWAIT);
		smartcharging_handle_power_boost(info,
						 SMART_CHG_TRAVELWAIT_PWR_BOOST);
	} else if (ctrl->smart_bypass) {
		smartcharging_set_mode(info, SMART_CHG_BYPASS);
	}
}

static int smartcharging_receive_control_message(int val)
{
	struct smart_charge_info *info = global_smartchg_info;

	info->smart_chg_control.AsUINT32 = val;
	mca_log_info("smart_chg value is %08x\n", val);
	smartcharging_handle_controlmessage();
	return 0;
}

/* Bit 0: whether the last request was refused; bit n: mode n is on. */
static int get_smart_chg_message(void)
{
	struct smart_charge_info *info = global_smartchg_info;
	u32 control_header = info->smart_chg_data.ret_code & 0x01;

	for (int index = SMART_CHG_NAVIGATION; index < SMART_CHG_MAX; index++) {
		if (info->smart_chg_data.smart_chgcfg[index].enable)
			control_header |= BIT(index);
	}
	mca_log_info("control_header = %d\n", control_header);
	return control_header;
}

static int handle_smart_batt_message(int val)
{
	struct smart_charge_info *info = global_smartchg_info;
	int vote_value;

	info->smart_batt_val = val;
	vote_value = info->smart_batt_val * info->cell_type;

	mca_log_info("smart_batt_val is %d vote_value %d\n", val, vote_value);
	mca_vote(info->smartchg_delta_fv_voter, "smart_batt", true, vote_value);

	return 0;
}

static int handle_smart_night_message(int val)
{
	struct smart_charge_info *info = global_smartchg_info;
	struct SMART_CHG_DATA *night =
		&info->smart_chg_data.smart_chgcfg[SMART_CHG_NIGHT];

	info->smart_night_val = val;
	mca_log_info("smart_night_val is %d\n", !!val);
	if (val)
		strategy_class_fg_ops_get_rsoc(&info->night_enable_rsoc);

	night->enable = val;
	night->func_value = SMART_CHG_SOCLMT_TRIG_DEFAULT;
	smartcharging_handle_algorithm_conflict(info);

	return 0;
}

static void smart_charge_plugin_or_plugout_reset(struct smart_charge_info *info)
{
	mca_vote(info->smartchg_delta_ichg_voter, "batt_health",
		 !!info->online, info->smart_delta_ichg);

	/*
	 * A phone plugged in at or above the level the user capped it to was
	 * never going to charge, so the two features that report having held
	 * it back have nothing to report and are marked as already sent.
	 */
	if (info->online && info->soc_limit &&
	    strategy_class_fg_ops_get_soc() >= info->soc_limit) {
		info->ignore_upload.navigation = 1;
		info->ignore_upload.endurance_pro = 1;
		mca_log_info("ignore_upload mievent");
	}
}

/*
 * Ask the gauge for a measurement pulse, from which battery swelling is
 * estimated, when the battery is in a state where the pulse means something.
 */
static void smart_charge_csd_send_pulse(struct smart_charge_info *info)
{
	int soc = 0, rsoc = 0;
	int batt_temp = 0;
	bool smart_night;
	int pulse_mode = 0;

	soc = strategy_class_fg_ops_get_soc();
	strategy_class_fg_ops_get_rsoc(&rsoc);
	strategy_class_fg_ops_get_temperature(&batt_temp);
	batt_temp /= 10;
	smart_night = info->smart_chg_data.smart_chgcfg[SMART_CHG_NIGHT].enable;

	if (!info->pulse_mode) {
		if (soc < SMART_CHG_CSD_SOC_MIN || soc > SMART_CHG_CSD_SOC_MAX ||
		    batt_temp < SMART_CHG_CSD_TEMP_MIN ||
		    batt_temp > SMART_CHG_CSD_TEMP_MAX)
			goto out;

		if (info->plugin_rsoc <= SMART_CHG_CSD_PLUGIN_RSOC_MAX &&
		    !info->screen_state &&
		    info->cycle_count > SMART_CHG_CSD_CYCLE_MIN &&
		    info->cycle_count % SMART_CHG_CSD_CYCLE_PERIOD == 0 &&
		    rsoc == SMART_CHG_CSD_PULSE_RSOC_LOW)
			pulse_mode = 1;
		else if (!smart_night)
			goto out;
		else if (info->night_enable_rsoc <= SMART_CHG_CSD_PLUGIN_RSOC_MAX &&
			 rsoc == SMART_CHG_CSD_PULSE_RSOC_LOW)
			pulse_mode = 2;
		else if (info->night_enable_rsoc > SMART_CHG_CSD_PLUGIN_RSOC_MAX &&
			 rsoc == SMART_CHG_CSD_PULSE_RSOC_HIGH)
			pulse_mode = 3;
		else
			goto out;
	} else if (info->cycle_count % SMART_CHG_CSD_CYCLE_PERIOD == 0 ||
		   smart_night) {
		goto out;
	}

	info->pulse_mode = pulse_mode;
	mca_event_block_notify(MCA_EVENT_CHARGE_STATUS, MCA_EVENT_CSD_SEND_PULSE,
			       &info->pulse_mode);

out:
	mca_log_info("soc: %d, rsoc: %d, plugin_rsoc: %d, night_enable_rsoc: %d, batt_temp: %d, cycle: %d, night: %d, pulse_mode: %d",
		     soc, rsoc, info->plugin_rsoc, info->night_enable_rsoc,
		     batt_temp, info->cycle_count, smart_night, info->pulse_mode);
}

static int smart_charge_thermal_notifier_cb(struct notifier_block *nb,
					    unsigned long event, void *val)
{
	struct smart_charge_info *info =
		container_of(nb, struct smart_charge_info, thermal_nb);

	switch (event) {
	case MCA_EVENT_THERMAL_BOARD_TEMP_CHANGE:
		info->board_temp = *(int *)val / SMART_CHG_BOARD_TEMP_SCALE;
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

/*
 * Find the band the board temperature is in.  Moving to another band needs
 * the temperature to clear the current band's hysteresis first.
 */
static void
update_smart_bypass_temp_section(struct smart_charge_info *info,
				 struct smart_bypass_temp_section *sections,
				 int count)
{
	int board_temp = info->board_temp;
	int cur = info->bypass_temp_index;
	int report_index;
	int i;

	for (i = 0; i < count; i++) {
		if (board_temp >= sections[i].temp_high ||
		    board_temp < sections[i].temp_low)
			continue;

		report_index = cur;
		if (cur < i) {
			if (board_temp >= sections[cur].temp_high +
					  sections[cur].hyst_high) {
				report_index = i;
				info->bypass_temp_index = i;
			}
		} else if (cur > i) {
			if (board_temp < sections[cur].temp_low -
					 sections[cur].hyst_low) {
				report_index = i;
				info->bypass_temp_index = i;
			}
		}

		mca_log_err("for_bypass board_temp:%d, cur_temp_index:%d\n",
			    board_temp, report_index);
		return;
	}

	mca_log_err("temp over range invalid, current:%d\n", -1);
}

static void smart_charge_check_bypass_status(struct smart_charge_info *info)
{
	static bool once_flag;
	bool prev = once_flag;

	if (info->bypass_exit_flag) {
		once_flag = false;
		return;
	}

	if (!once_flag) {
		mca_log_err("for_bypass start time\n");
		info->bypass_start_time = ktime_get_boottime();
	} else {
		if (ktime_get_boottime() <=
		    info->bypass_start_time + SMART_BYPASS_DEBOUNCE_NS)
			goto debounce;

		mca_log_err("for_bypass exit bypass\n");
		mca_vote(info->smartchg_set_fcc_voter, "smart_bypass", false, 0);
		info->bypass_exit_flag = true;
	}
	once_flag = !prev;
debounce:
	mca_log_err("for_bypass Debounce: %lld s",
		    (ktime_get_boottime() - info->bypass_start_time) /
			    NSEC_PER_SEC);
}

/*
 * Bypass charging feeds the system from the charger while the battery is
 * mostly left alone.  The charging current it allows depends on the board
 * temperature, and how much heat the current scene adds decides which of the
 * three tables applies.
 */
static void smart_charge_handle_bypass_chg(struct smart_charge_info *info)
{
	struct smart_bypass_temp_section *sections;
	u32 state;
	int count;
	int fcc = 0;
	int soc;

	if (!info->support_bypass)
		return;

	state = info->smart_chg_data.smart_chgcfg[SMART_CHG_BYPASS].enable;
	if (!state)
		goto report;

	soc = strategy_class_fg_ops_get_soc();
	if (soc < info->bypass_entry_soc) {
		mca_log_err("for_bypass soc not in range, quit bypass mode\n");
		state = 0;
		goto report;
	}
	if (info->wls_online) {
		mca_log_err("for_bypass wlschg online, quit bypass mode\n");
		state = 0;
		goto report;
	}

	switch (info->scene) {
	case SMART_CHG_SCENE_YOUTUBE:
	case SMART_CHG_SCENE_VIDEO:
	case SMART_CHG_SCENE_DANMU:
	case SMART_CHG_SCENE_PER_YOUTUBE:
	case SMART_CHG_SCENE_PER_VIDEO:
	case SMART_CHG_SCENE_PER_DANMU:
		sections = info->bypass_med_lmt;
		count = info->bypass_med_num;
		break;
	case SMART_CHG_SCENE_TGAME:
	case SMART_CHG_SCENE_MGAME:
	case SMART_CHG_SCENE_YUANSHEN:
	case SMART_CHG_SCENE_XINGTIE:
	case SMART_CHG_SCENE_PER_XINGTIE:
	case SMART_CHG_SCENE_HP_GAME:
	case SMART_CHG_SCENE_CGAME:
	case SMART_CHG_SCENE_CGAME_2:
		sections = info->bypass_high_lmt;
		count = info->bypass_high_num;
		break;
	default:
		sections = info->bypass_low_lmt;
		count = info->bypass_low_num;
		break;
	}

	update_smart_bypass_temp_section(info, sections, count);
	info->bypass_active = 1;
	fcc = sections[info->bypass_temp_index].fcc;

	if (fcc != 0 && fcc != info->last_bypass_fcc) {
		mca_vote(info->smartchg_set_fcc_voter, "smart_bypass", true,
			 fcc);
		info->bypass_exit_flag = false;
		if (!info->bypass_active)
			smart_charge_check_bypass_status(info);
	}

report:
	mca_log_err("for_bypass [%d->%d] fcc[%d %d]\n", info->last_bypass_state,
		    state, fcc, info->last_bypass_fcc);
	if (info->last_bypass_state != state) {
		info->last_bypass_state = state;
		if (state == 0)
			mca_vote(info->smartchg_set_fcc_voter, "smart_bypass",
				 false, 0);
	}
	if (fcc != info->last_bypass_fcc)
		info->last_bypass_fcc = fcc;
}

/*
 * A phone configured as a shop display is charged all day and never used,
 * so it is held part-charged rather than left sitting full.
 */
static void handle_mishow_config(struct smart_charge_info *info,
				 struct mca_hwid_info *hwid, int boot_mode)
{
	bool mishow = false;

	if (!hwid || !boot_mode ||
	    hwid->platform_version < SMART_CHG_MISHOW_PROJECT_LOW ||
	    hwid->platform_version > SMART_CHG_MISHOW_PROJECT_HIGH)
		return;

	charger_partition_get_mishow(&mishow);
	info->mishow_config = mishow;
	mca_log_info("mishow config is %d\n", info->mishow_config);
	if (info->mishow_config)
		smartcharging_handle_algorithm_conflict(info);
}

static void smart_charge_workfunc(struct work_struct *work)
{
	struct smart_charge_info *info = container_of(
		work, struct smart_charge_info, smart_charge_work.work);
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	int boot_mode = mca_log_get_charge_boot_mode();
	int delta_ichg = 0;

	if (!info->online) {
		mca_log_err("adapter is plugout\n");
		return;
	}

	/* Charge an aged battery more gently. */
	strategy_class_fg_ops_get_cyclecount(&info->cycle_count);
	for (int i = 0; i < CC_ICHG_MAX_GROUP; i++) {
		if (info->cycle_count <= info->ichg_cc_table[i].cc_max) {
			delta_ichg = info->ichg_cc_table[i].delta_ichg;
			break;
		}
	}

	if (delta_ichg != info->smart_delta_ichg) {
		info->smart_delta_ichg = delta_ichg;
		mca_vote(info->smartchg_delta_ichg_voter, "batt_health", true,
			 delta_ichg);
		mca_log_info("cycle_count %d smart_delta_ichg %d\n",
			     info->cycle_count, info->smart_delta_ichg);
	}

	if (hwid && hwid->country_version == CountryCN && info->support_csd)
		smart_charge_csd_send_pulse(info);

	smart_charge_handle_bypass_chg(info);
	handle_mishow_config(info, hwid, boot_mode);

	schedule_delayed_work(&info->smart_charge_work,
			      msecs_to_jiffies(SMART_CHARGE_WORK_PERIOD_MS));
}

static void smart_charge_sense_chg_workfunc(struct work_struct *work)
{
	struct smart_charge_info *info = container_of(
		work, struct smart_charge_info, smart_sense_chg_work.work);

	smart_charge_handle_sense_chg(info);
}

static int mca_smart_charge_process_event(int event, int value, void *data)
{
	struct smart_charge_info *info = data;

	if (!info)
		return -1;

	mca_log_info("receive event %d, value %d\n", event, value);
	switch (event) {
	case MCA_EVENT_WIRELESS_CONNECT:
		info->wls_online = 1;
		fallthrough;
	case MCA_EVENT_USB_CONNECT:
		info->online = 1;
		strategy_class_fg_ops_get_rsoc(&info->plugin_rsoc);
		smart_charge_plugin_or_plugout_reset(info);
		cancel_delayed_work_sync(&info->smart_charge_work);
		schedule_delayed_work(&info->smart_charge_work, 0);
		cancel_delayed_work_sync(&info->smart_soc_limit_work);
		schedule_delayed_work(&info->smart_soc_limit_work, 0);
		break;
	case MCA_EVENT_WIRELESS_DISCONNECT:
		info->wls_online = 0;
		fallthrough;
	case MCA_EVENT_USB_DISCONNECT:
		info->online = 0;
		smart_charge_plugin_or_plugout_reset(info);
		cancel_delayed_work_sync(&info->smart_charge_work);
		cancel_delayed_work_sync(&info->smart_soc_limit_work);
		info->smart_chg_control.AsUINT32 = 0;
		info->soc_limit_enable = false;
		info->smart_sic_mode = -1;
		info->ignore_upload.AsUINT32 = 0;
		smart_charge_set_soc_limit(info->soc_limit_enable);
		info->bypass_active = 0;
		break;
	case MCA_EVENT_BATTERY_DTPT:
		if (value)
			mca_vote(info->smartchg_delta_fv_voter, "dtpt", true,
				 SMART_CHARGE_DTPT_DELTA_VALUE);
		else
			mca_vote(info->smartchg_delta_fv_voter, "dtpt", false,
				 0);
		break;
	default:
		break;
	}

	return 0;
}

static int smart_charge_fv_control(struct mca_votable *votable, void *data,
				   int effective_result,
				   const char *effective_client)
{
	struct smart_charge_info *info = data;
	static struct mca_smartchg_if_ops *if_ops;

	if (!data)
		return -1;

	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		if_ops = mca_smartchg_if_get_ops(i);
		if (!if_ops || !if_ops->set_delta_fv) {
			mca_log_info(" %d %p\n", i, if_ops);
			continue;
		}
		if_ops->set_delta_fv(if_ops->data, effective_result);
	}
	info->delta_fv = effective_result;
	mca_log_err("smartchg_delta_fv %d\n", effective_result);

	return 0;
}

static int smart_charge_ichg_control(struct mca_votable *votable, void *data,
				     int effective_result,
				     const char *effective_client)
{
	struct smart_charge_info *info = data;
	static struct mca_smartchg_if_ops *if_ops;

	if (!data)
		return -1;

	info->delta_ichg = effective_result;
	mca_log_err("smartchg_delta_ichg %d\n", effective_result);

	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		if_ops = mca_smartchg_if_get_ops(i);
		if (!if_ops || !if_ops->set_delta_ichg)
			continue;
		if_ops->set_delta_ichg(if_ops->data, effective_result);
	}

	return 0;
}

static int smart_charge_set_fcc_control(struct mca_votable *votable, void *data,
					int effective_result,
					const char *effective_client)
{
	static struct mca_smartchg_if_ops *if_ops;

	if (!data)
		return -1;

	mca_log_err("smartchg_set_fcc %d\n", effective_result);

	for (int i = 0; i < MCA_SMARTCHG_IF_CHG_TYPE_END; i++) {
		if_ops = mca_smartchg_if_get_ops(i);
		if (!if_ops || !if_ops->set_fcc)
			continue;
		if_ops->set_fcc(if_ops->data, effective_result);
	}

	return 0;
}

static int smart_charge_init_voter(struct smart_charge_info *info)
{
	info->smartchg_delta_fv_voter = mca_create_votable(
		"smartchg_delta_fv", MCA_VOTE_MAX, smart_charge_fv_control,
		SMART_CHARGE_DELTA_FV_DEFAULT_VALUE, info);
	if (IS_ERR(info->smartchg_delta_fv_voter)) {
		mca_log_err("smartchg_delta_fv voteable failed\n");
		return -1;
	}

	info->smartchg_delta_ichg_voter = mca_create_votable(
		"smartchg_delta_ichg", MCA_VOTE_MAX, smart_charge_ichg_control,
		SMART_CHARGE_DELTA_ICHG_DEFAULT_VALUE, info);
	if (IS_ERR(info->smartchg_delta_ichg_voter)) {
		mca_log_err("smartchg_delta_ichg voteable failed\n");
		return -1;
	}

	info->smartchg_set_fcc_voter = mca_create_votable(
		"smartchg_set_fcc", MCA_VOTE_MIN, smart_charge_set_fcc_control, 0,
		info);
	if (IS_ERR(info->smartchg_set_fcc_voter)) {
		mca_log_err("smartchg_fcc_voter voteable failed\n");
		return -1;
	}

	return 0;
}

static ssize_t smart_charge_sysfs_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct smart_charge_info *info = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *attr_info;
	struct SMART_CHG_DATA *sense;
	int val;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  smartchg_sysfs_field_tbl,
					  SMARTCHG_SYSFS_ATTRS_SIZE);
	if (!attr_info)
		return -1;

	if (!info) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	switch (attr_info->sysfs_attr_name) {
	case MCA_PROP_SMARTCHG:
		val = get_smart_chg_message();
		break;
	case MCA_PROP_SMARTCHG_FV:
		val = max(mca_get_client_vote(info->smartchg_delta_fv_voter,
					      "smart_chg"), 0);
		break;
	case MCA_PROP_SMARTCHG_ICHG:
		val = max(mca_get_client_vote(info->smartchg_delta_ichg_voter,
					      "smart_chg"), 0);
		break;
	case MCA_PROP_SMARTBATT:
		val = global_smartchg_info->smart_batt_val;
		break;
	case MCA_PROP_SMARTNIGHT:
		val = global_smartchg_info->smart_night_val;
		break;
	case MCA_PROP_POSTURE:
		sense = &info->smart_chg_data.smart_chgcfg[SMART_CHG_SENSE_CHG];
		val = sense->use_fake_value ? sense->fake_value :
					      sense->func_value;
		break;
	case MCA_PROP_SCENE:
		val = mca_smartchg_get_scene();
		break;
	case MCA_PROP_BOARD_TEMP:
		val = info->board_temp;
		break;
	case MCA_PROP_SMART_SIC_MODE:
		val = max(info->smart_sic_mode, 0);
		break;
	default:
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t smart_charge_sysfs_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct smart_charge_info *info = dev_get_drvdata(dev);
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	struct mca_sysfs_attr_info *attr_info;
	int val = 0;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name,
					  smartchg_sysfs_field_tbl,
					  SMARTCHG_SYSFS_ATTRS_SIZE);
	if (!attr_info)
		return -1;

	if (!info) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	switch (attr_info->sysfs_attr_name) {
	case MCA_PROP_SMARTCHG:
		smartcharging_receive_control_message(val);
		break;
	case MCA_PROP_SMARTCHG_FV:
		/*
		 * The step userspace asks for is per cell, and one project's
		 * cells take a larger one than the number it sends.
		 */
		if (val == SMARTCHG_FV_STEP_DEFAULT_MV && hwid &&
		    hwid->platform_version == SMARTCHG_FV_STEP_LARGE_PROJECT)
			val = SMARTCHG_FV_STEP_LARGE_MV;
		val = val * info->cell_type;
		mca_vote(info->smartchg_delta_fv_voter, "smart_chg", true, val);
		break;
	case MCA_PROP_SMARTCHG_ICHG:
		mca_vote(info->smartchg_delta_ichg_voter, "smart_chg", true,
			 val);
		break;
	case MCA_PROP_SMARTBATT:
		handle_smart_batt_message(val);
		break;
	case MCA_PROP_SMARTNIGHT:
		handle_smart_night_message(val);
		break;
	case MCA_PROP_SCENE:
		mca_smartchg_set_scene(val);
		break;
	case MCA_PROP_BOARD_TEMP:
		mca_smartchg_set_board_temp(val);
		break;
	default:
		break;
	}

	mca_log_info("set the %d value = %d\n", attr_info->sysfs_attr_name,
		     val);

	return count;
}

static int smart_charge_sysfs_create_group(struct device *dev)
{
	mca_sysfs_init_attrs(smartchg_sysfs_attrs, smartchg_sysfs_field_tbl,
			     SMARTCHG_SYSFS_ATTRS_SIZE);
	return mca_sysfs_create_link_group("charger", "smart_charge", dev,
					   &smartchg_sysfs_attr_group);
}

static void smart_charge_sysfs_remove_group(struct device *dev)
{
	mca_sysfs_remove_link_group("charger", "smart_charge", dev,
				    &smartchg_sysfs_attr_group);
}

/*
 * smart_charge_check_baa_layout() - is every record inside the blob?
 * @hdr: the header, at the head of the buffer it describes
 *
 * Every count and step_size is written by userspace, and the walk that
 * follows, like the consumers it hands records to, takes its stride from
 * them.  Walk the layout once up front and refuse the blob whole if it does
 * not close inside @hdr->total_len, rather than reading past the buffer or
 * overrunning a consumer's staging array part-way through applying it.
 *
 * Return: 0 if the layout closes inside @hdr->total_len, -EINVAL otherwise.
 */
static int smart_charge_check_baa_layout(const struct smart_basp_header *hdr)
{
	size_t total_len = hdr->total_len;
	size_t off = sizeof(*hdr);
	size_t n;

	if (total_len > PAGE_SIZE || total_len < off)
		return -EINVAL;

	/* The jeita terms are fixed-size, so they bound in one step. */
	n = (size_t)hdr->jeita_ffc_term_size + hdr->jeita_normal_term_size;
	if (n > (total_len - off) / sizeof(struct smart_batt_jeita_term_para))
		return -EINVAL;
	off += n * sizeof(struct smart_batt_jeita_term_para);

	/*
	 * The wired and wireless specs are variable-length and laid end to
	 * end, so the only way to bound them is to walk them: each record's
	 * step_size decides where the next one starts.
	 */
	n = (size_t)hdr->wired_ffc_size + hdr->wired_normal_size +
	    hdr->wls_ffc_size + hdr->wls_normal_size;
	while (n--) {
		const struct smart_batt_spec *spec;

		if (total_len - off < offsetof(struct smart_batt_spec, steps))
			return -EINVAL;
		spec = (const struct smart_batt_spec *)((const char *)hdr + off);
		off += offsetof(struct smart_batt_spec, steps);

		if (spec->step_size > SMART_BATT_SPEC_MAX_STEPS ||
		    spec->step_size > (total_len - off) /
				      sizeof(struct smart_batt_spec_curve))
			return -EINVAL;
		off += (size_t)spec->step_size *
		       sizeof(struct smart_batt_spec_curve);
	}

	return 0;
}

static size_t smart_charge_spec_len(const char *buf, size_t offset)
{
	const struct smart_batt_spec *spec =
		(const struct smart_batt_spec *)(buf + offset);

	return offsetof(struct smart_batt_spec, steps) +
	       spec->step_size * sizeof(struct smart_batt_spec_curve);
}

/* Hand the new jeita terms and charging curves to the strategies. */
static int smart_charge_handle_baa_data(struct smart_charge_info *info)
{
	struct smart_basp_header *basp_header =
		(struct smart_basp_header *)info->mmap_addr;
	static struct mca_smartchg_if_ops *if_ops;
	size_t offset;

	if (smart_charge_check_baa_layout(basp_header)) {
		mca_log_err("Invalid baa layout, ignoring blob\n");
		return -1;
	}

	offset = sizeof(struct smart_basp_header);
	if_ops = mca_smartchg_if_get_ops(MCA_SMARTCHG_IF_CHG_TYPE_JEITA);
	if (!if_ops || !if_ops->update_baa_para) {
		mca_log_err("jeita update_baa_para is null\n");
	} else {
		mca_log_err("jeita update_baa_para offset: %zu, size: %d/%d\n",
			    offset, basp_header->jeita_ffc_term_size,
			    basp_header->jeita_normal_term_size);
		if_ops->update_baa_para(if_ops->data, info->mmap_addr + offset,
					basp_header->jeita_ffc_term_size,
					basp_header->jeita_normal_term_size);
	}

	offset += ((size_t)basp_header->jeita_ffc_term_size +
		   basp_header->jeita_normal_term_size) *
		  sizeof(struct smart_batt_jeita_term_para);

	if_ops = mca_smartchg_if_get_ops(MCA_SMARTCHG_IF_CHG_TYPE_QC);
	if (!if_ops || !if_ops->update_baa_para) {
		mca_log_err("wired qc update_baa_para is null\n");
	} else {
		mca_log_err("wired qc update_baa_para offset: %zu, size: %d/%d\n",
			    offset, basp_header->wired_ffc_size,
			    basp_header->wired_normal_size);
		if_ops->update_baa_para(if_ops->data, info->mmap_addr + offset,
					basp_header->wired_ffc_size,
					basp_header->wired_normal_size);
	}

	for (u32 i = 0; i < basp_header->wired_ffc_size; i++)
		offset += smart_charge_spec_len(info->mmap_addr, offset);
	for (u32 i = 0; i < basp_header->wired_normal_size; i++)
		offset += smart_charge_spec_len(info->mmap_addr, offset);

	if_ops = mca_smartchg_if_get_ops(MCA_SMARTCHG_IF_CHG_TYPE_WL_QC);
	if (!if_ops || !if_ops->update_baa_para) {
		mca_log_err("wls qc update_baa_para is null\n");
	} else {
		mca_log_err("wls qc update_baa_para offset: %zu, size: %d/%d\n",
			    offset, basp_header->wls_ffc_size,
			    basp_header->wls_normal_size);
		if_ops->update_baa_para(if_ops->data, info->mmap_addr + offset,
					basp_header->wls_ffc_size,
					basp_header->wls_normal_size);
	}

	return 0;
}

static int smart_charge_cal_checksum(struct smart_charge_info *info)
{
	struct smart_basp_header *basp_header =
		(struct smart_basp_header *)info->mmap_addr;
	u32 checksum = 0;
	u32 total_len;

	mca_log_err("header: %d, %d, %d; %d, %d; %d, %d; %d, %d\n",
		    basp_header->checksum, basp_header->type,
		    basp_header->total_len, basp_header->jeita_ffc_term_size,
		    basp_header->jeita_normal_term_size,
		    basp_header->wired_ffc_size, basp_header->wired_normal_size,
		    basp_header->wls_ffc_size, basp_header->wls_normal_size);

	/* total_len comes from userspace and must stay inside the page. */
	total_len = basp_header->total_len;
	if (total_len > PAGE_SIZE) {
		mca_log_err("Invalid total_len: %u\n", total_len);
		return -1;
	}

	for (u32 i = sizeof(basp_header->checksum); i < total_len; i++)
		checksum += (u8)info->mmap_addr[i];

	mca_log_err("cal checksum %d, origin checksum %d\n", checksum,
		    basp_header->checksum);
	if (checksum != basp_header->checksum)
		return -1;

	return 0;
}

static void smart_charge_handle_mmap_data(struct smart_charge_info *info)
{
	if (smart_charge_cal_checksum(info)) {
		mca_log_err("checksum failed\n");
		return;
	}

	if (info->enable_fv_dec_by_cc)
		smart_charge_handle_baa_data(info);
}

/*
 * The buffer is shared by every open file and applied, then freed, when the
 * last one is closed.  A mapping holds its file open, so nothing can still
 * reach the buffer from userspace by then.
 */
static int smart_charge_file_open(struct inode *inode, struct file *filp)
{
	struct smart_charge_info *info =
		container_of(inode->i_cdev, struct smart_charge_info, pri_dev);

	mutex_lock(&info->mmap_lock);
	if (!info->mmap_addr) {
		info->mmap_addr = (char *)get_zeroed_page(GFP_KERNEL);
		if (!info->mmap_addr) {
			mutex_unlock(&info->mmap_lock);
			return -ENOMEM;
		}
		mca_log_err("malloc memery succ\n");
	}
	info->open_cnt++;
	filp->private_data = info;
	mutex_unlock(&info->mmap_lock);

	return 0;
}

static int smart_charge_file_close(struct inode *inode, struct file *filp)
{
	struct smart_charge_info *info = filp->private_data;

	mutex_lock(&info->mmap_lock);
	if (--info->open_cnt == 0 && info->mmap_addr) {
		smart_charge_handle_mmap_data(info);
		info->map_flag = 0;
		free_page((unsigned long)info->mmap_addr);
		info->mmap_addr = NULL;
	}
	mutex_unlock(&info->mmap_lock);
	mca_log_err("release memery succ\n");

	return 0;
}

static int smart_charge_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct smart_charge_info *info = filp->private_data;
	int ret;

	mutex_lock(&info->mmap_lock);
	if (!info->mmap_addr || info->map_flag) {
		mca_log_err("map addr error\n");
		ret = -1;
		goto out;
	}
	if (vma->vm_end - vma->vm_start > PAGE_SIZE) {
		mca_log_err("map addr len error\n");
		ret = -1;
		goto out;
	}

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	ret = remap_pfn_range(vma, vma->vm_start,
			      virt_to_phys(info->mmap_addr) >> PAGE_SHIFT,
			      vma->vm_end - vma->vm_start, vma->vm_page_prot);
	mca_log_err("vma start 0x%lx, prot 0x%llx\n", vma->vm_start,
		    (u64)pgprot_val(vma->vm_page_prot));
	info->map_flag = 1;
	if (ret)
		mca_log_err("remap pfn failed %d\n", ret);

out:
	mutex_unlock(&info->mmap_lock);
	return ret;
}

static long smart_charge_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct smart_charge_info *info = file->private_data;
	unsigned long ret;
	void *buf;

	if (cmd != SMARTCHG_DATA_CMD) {
		mca_log_err("invalid cmd\n");
		return -EINVAL;
	}

	/*
	 * Copy through a bounce page: faulting in user memory with mmap_lock
	 * held would invert the order smart_charge_mmap() takes it in, under
	 * the caller's mm lock.  copy_from_user() zeroes whatever it could not
	 * copy, so the shared buffer gets the same contents either way.
	 */
	buf = (void *)__get_free_page(GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = copy_from_user(buf, (void __user *)arg, PAGE_SIZE);

	mutex_lock(&info->mmap_lock);
	if (!info->mmap_addr) {
		mutex_unlock(&info->mmap_lock);
		free_page((unsigned long)buf);
		mca_log_err("mmap addr is null\n");
		return -EINVAL;
	}
	memcpy(info->mmap_addr, buf, PAGE_SIZE);
	mutex_unlock(&info->mmap_lock);
	free_page((unsigned long)buf);

	mca_log_info("data ret = %lu\n", ret);
	return ret ? -EINVAL : 0;
}

static int smart_charge_panel_notifier_cb(struct notifier_block *nb,
					  unsigned long event, void *val)
{
	struct smart_charge_info *info =
		container_of(nb, struct smart_charge_info, panel_nb);
	int screen_state;

	switch (event) {
	case MCA_EVENT_PANEL_SCREEN_STATE_CHANGE:
		screen_state = *(int *)val;
		mca_log_info("update screen_state: %d => %d\n",
			     info->screen_state, screen_state);
		info->screen_state = screen_state;
		cancel_delayed_work_sync(&info->smart_sense_chg_work);
		schedule_delayed_work(&info->smart_sense_chg_work, 0);
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static const struct file_operations g_smt_chg_file_ops = {
	.owner = THIS_MODULE,
	.open = smart_charge_file_open,
	.release = smart_charge_file_close,
	.mmap = smart_charge_mmap,
	.unlocked_ioctl = smart_charge_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int smart_charge_parse_bypass_lmt(struct device_node *node,
					 const char *prop,
					 struct smart_bypass_temp_section *lmt,
					 int *num)
{
	const int elems =
		sizeof(struct smart_bypass_temp_section) / sizeof(u32);
	int len;

	len = mca_parse_dts_u32_count(node, prop, SMART_BYPASS_TEMP_SECTION_MAX,
				      elems);
	if (len < 0) {
		mca_log_err("parse %s failed\n", prop);
		return -1;
	}

	*num = len / elems;
	if (mca_parse_dts_u32_array(node, prop, (u32 *)lmt, len) < 0)
		return -1;

	return 0;
}

static int smart_charge_parse_dt(struct smart_charge_info *info)
{
	struct device_node *node = info->dev->of_node;
	u32 batt_ichg_cfg[CC_ICHG_MAX_GROUP * SMARTCHG_MODE_PARA_MAX] = { 0 };
	int len;

	mca_parse_dts_u32(node, "cell_type", &info->cell_type,
			  MCA_CELL_TYPE_1S);
	mca_parse_dts_u32(node, "enable_fv_dec_by_cc",
			  &info->enable_fv_dec_by_cc, 0);
	mca_parse_dts_u32(node, "support_csd", &info->support_csd, 0);
	info->support_sensechg_v2 =
		of_property_read_bool(node, "support_sensechg_v2");
	info->support_sensechg_v3 =
		of_property_read_bool(node, "support_sensechg_v3");
	info->support_cc_ichg = of_property_read_bool(node, "support_cc_ichg");
	if (info->support_cc_ichg) {
		len = mca_parse_dts_u32_count(node, "cc_ichg_cfg",
					      CC_ICHG_MAX_GROUP,
					      SMARTCHG_MODE_PARA_MAX);
		if (len < 0) {
			mca_log_err("parse cc_ichg_cfg failed\n");
			return -1;
		}

		if (mca_parse_dts_u32_array(node, "cc_ichg_cfg", batt_ichg_cfg,
					    ARRAY_SIZE(batt_ichg_cfg))) {
			mca_log_err(" parse cc_ichg_cfg failed\n");
			return -1;
		}

		for (int row = 0; row < len / SMARTCHG_MODE_PARA_MAX; row++) {
			int col = row * SMARTCHG_MODE_PARA_MAX;

			info->ichg_cc_table[row].cc_max =
				batt_ichg_cfg[col + SMARTCHG_MODE_CYCLECOUNT];
			info->ichg_cc_table[row].delta_ichg =
				batt_ichg_cfg[col + SMARTCHG_MODE_DELTA_ICHG];
			mca_log_err("%d, %d\n", info->ichg_cc_table[row].cc_max,
				    info->ichg_cc_table[row].delta_ichg);
		}
	}

	mca_parse_dts_u32(node, "smart_bypass_support", &info->support_bypass,
			  0);
	if (!info->support_bypass)
		return 0;

	mca_parse_dts_u32(node, "smart_bypass_entry_soc",
			  &info->bypass_entry_soc, 0);
	if (smart_charge_parse_bypass_lmt(node, "smart_bypass_high_lmt_cfg",
					  info->bypass_high_lmt,
					  &info->bypass_high_num) ||
	    smart_charge_parse_bypass_lmt(node, "smart_bypass_med_lmt_cfg",
					  info->bypass_med_lmt,
					  &info->bypass_med_num) ||
	    smart_charge_parse_bypass_lmt(node, "smart_bypass_low_lmt_cfg",
					  info->bypass_low_lmt,
					  &info->bypass_low_num))
		return -1;

	return 0;
}

static int smart_charge_probe(struct platform_device *pdev)
{
	struct smart_charge_info *info;
	static int probe_cnt;
	int ret = 0, online = 0, wireless_online = 0;

	mca_log_info("probe_cnt = %d\n", ++probe_cnt);
	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info) {
		mca_log_err("out of memory\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, info);
	info->dev = &pdev->dev;
	mutex_init(&info->mmap_lock);
	global_smartchg_info = info;
	ret = smart_charge_parse_dt(info);
	if (ret) {
		mca_log_err("parse dt failed\n");
		return -1;
	}
	smart_charge_sysfs_create_group(info->dev);
	ret = smart_charge_init_voter(info);
	if (ret) {
		mca_log_err("init voter err\n");
		return -1;
	}

	mca_strategy_ops_register(STRATEGY_FUNC_TYPE_SMARTCHG,
				  mca_smart_charge_process_event, NULL, NULL,
				  info);

	info->panel_nb.notifier_call = smart_charge_panel_notifier_cb;
	mca_event_block_notify_register(MCA_EVENT_TYPE_PANEL, &info->panel_nb);

	info->thermal_nb.notifier_call = smart_charge_thermal_notifier_cb;
	mca_event_block_notify_register(MCA_EVENT_TYPE_THERMAL_TEMP,
					&info->thermal_nb);

	INIT_DELAYED_WORK(&info->smart_charge_work, smart_charge_workfunc);
	INIT_DELAYED_WORK(&info->smart_sense_chg_work,
			  smart_charge_sense_chg_workfunc);
	INIT_DELAYED_WORK(&info->smart_soc_limit_work,
			  smart_charge_soc_limit_workfunc);

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
				     STRATEGY_STATUS_TYPE_ONLINE, &online);
	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
				     STRATEGY_STATUS_TYPE_ONLINE,
				     &wireless_online);

	ret = alloc_chrdev_region(&info->dev_num, 0, 1, "smart_charge");
	if (ret) {
		mca_log_err("alloc chrdev num failed\n");
		return ret;
	}

	cdev_init(&info->pri_dev, &g_smt_chg_file_ops);
	info->pri_dev.owner = THIS_MODULE;
	ret = cdev_add(&info->pri_dev, info->dev_num, 1);
	if (ret) {
		mca_log_err("cdev add failed\n");
		goto err_unregister_region;
	}

	info->smart_charge_class = class_create("smart_charge");
	if (IS_ERR(info->smart_charge_class)) {
		mca_log_err("create smart charge class failed\n");
		goto err_cdev_del;
	}

	info->sys_dev = device_create(info->smart_charge_class, NULL,
				      info->dev_num, NULL, "smart_charge_cdev");
	if (IS_ERR(info->sys_dev)) {
		mca_log_err("create smart charge device failed\n");
		goto err_class_destroy;
	}

	if (online || wireless_online) {
		info->online = 1;
		schedule_delayed_work(&info->smart_charge_work, 0);
	}

	info->fake_scene = SMART_CHARGE_FAKE_SCENE_DEFAULT;

	mca_log_info("done\n");
	return 0;

err_class_destroy:
	class_destroy(info->smart_charge_class);
err_cdev_del:
	cdev_del(&info->pri_dev);
err_unregister_region:
	unregister_chrdev_region(info->dev_num, 1);

	return -1;
}

static int smart_charge_remove(struct platform_device *pdev)
{
	struct smart_charge_info *info = platform_get_drvdata(pdev);

	unregister_chrdev_region(info->dev_num, 1);
	cdev_del(&info->pri_dev);
	device_destroy(info->smart_charge_class, info->dev_num);
	class_destroy(info->smart_charge_class);
	smart_charge_sysfs_remove_group(&pdev->dev);
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_PANEL,
					  &info->panel_nb);
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_THERMAL_TEMP,
					  &info->thermal_nb);
	cancel_delayed_work_sync(&info->smart_charge_work);
	cancel_delayed_work_sync(&info->smart_sense_chg_work);
	cancel_delayed_work_sync(&info->smart_soc_limit_work);

	return 0;
}

static const struct of_device_id match_table[] = {
	{ .compatible = "xiaomi,smart_charge" },
	{},
};

static struct platform_driver smart_charge_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "smart_charge",
		.of_match_table = match_table,
	},
	.probe = smart_charge_probe,
	.remove = smart_charge_remove,
};

static int __init smart_charge_init(void)
{
	return platform_driver_register(&smart_charge_driver);
}
module_init(smart_charge_init);

static void __exit smart_charge_exit(void)
{
	platform_driver_unregister(&smart_charge_driver);
}
module_exit(smart_charge_exit);

MODULE_DESCRIPTION("smart charge driver");
MODULE_AUTHOR("liuzhengqing@xiaomi.com");
MODULE_LICENSE("GPL v2");
