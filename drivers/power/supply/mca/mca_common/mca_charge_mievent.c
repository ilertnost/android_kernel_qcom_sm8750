// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Reporting charging faults.  See
 * include/mca/common/mca_charge_mievent.h.
 */

#define MCA_LOG_TAG "mca_charge_mievent"

#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <mca/common/mca_charge_mievent.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <linux/module.h>
#include <linux/string.h>


/* The most a fault carries, and how long each of its parts may be. */
#define MIEVENT_PARA_MAX	16
#define MIEVENT_NAME_LEN	30

/* The most a string parameter may come to. */
#define MIEVENT_STR_MAX		128

/* How many faults are rate limited each way. */
#define MIEVENT_PLUG_MAX	PLUG_TYPE_MAX_NUM
#define MIEVENT_TIME_MAX	8

/* Which log a fault that stopped charging working belongs to. */
#define MIEVENT_ERR_LOG_NAME	"chgErrInfo"

/*
 * A charging error is also written with a tag of its own at the very start of
 * the line, ahead of this module's.  What reads it is looking for hardware
 * faults across the whole phone rather than for the charging log, and finds
 * them by that tag.
 */
#define mca_log_err_arch_tf(fmt, ...)					\
	__mca_log_err("[ARCH-TF-CHARGER][" MCA_LOG_TAG "]%s:%d " fmt,	\
		      __func__, __LINE__, ##__VA_ARGS__)

/*
 * Which slot of g_upload_plug a fault reported per plug is counted in.  The
 * order is not the order of the faults themselves, so it is stated rather
 * than assumed.
 */
enum mievent_upload_type_plug_ele {
	PLUG_TYPE_PD_AUTH_FAILED,
	PLUG_TYPE_CP_OPEN_FAILED,
	PLUG_TYPE_NOT_STANDARD_ADAPTER,
	PLUG_TYPE_RP_SHORT_VBUS_DETECTED,
	PLUG_TYPE_LPD_DETECTED,
	PLUG_TYPE_CP_VBUS_OVP,
	PLUG_TYPE_CP_IBUS_OCP,
	PLUG_TYPE_CP_VBAT_OVP,
	PLUG_TYPE_CP_IBAT_OCP,
	PLUG_TYPE_CP_VAC_OVP,
	PLUG_TYPE_ANTI_BURN_TRIGGERED,
	PLUG_TYPE_SOC_NOT_FULL,
	PLUG_TYPE_SMART_ENDURANCE_TRIGGERED,
	PLUG_TYPE_SMART_NAVIGATION_TRIGGERED,
	PLUG_TYPE_BATTERY_MISSING,
	PLUG_TYPE_CP_TDIE_HOT,
	PLUG_TYPE_VBUS_UVLO,
	PLUG_TYPE_LOW_TEMP_DISCHARGING,
	PLUG_TYPE_HIGH_TEMP_DISCHARGING,
	PLUG_TYPE_DUAL_BATTERY_MISSING,
	PLUG_TYPE_SMART_ENDURANCE_SOC_ERR,
	PLUG_TYPE_SMART_NAVIGATION_SOC_ERR,
	PLUG_TYPE_BATTERY_AUTH_FAIL,
	PLUG_TYPE_DUAL_BATTERY_AUTH_FAIL,
	PLUG_TYPE_ANTIBURN_ERR,
	PLUG_TYPE_WLS_FASTCHG_FAIL,
	PLUG_TYPE_WLS_FOD_LOW_POWER,
	PLUG_TYPE_WLS_RX_OTP,
	PLUG_TYPE_WLS_RX_OVP,
	PLUG_TYPE_WLS_RX_OCP,
	PLUG_TYPE_WLS_TRX_FOD,
	PLUG_TYPE_WLS_TRX_OCP,
	PLUG_TYPE_WLS_TRX_UVLO,
	PLUG_TYPE_WLS_TRX_IIC_ERR,
	PLUG_TYPE_WLS_RX_IIC_ERR,
	PLUG_TYPE_LOAD_SWITCH_I2C_ERR,
	PLUG_TYPE_WLS_FW_UPGRADE_FAIL,
	PLUG_TYPE_XMPPS_CHARGER_SLOWLY,
	PLUG_TYPE_NON_STANDARD_CHARGER,
	PLUG_TYPE_BATTERY_OCD,
	PLUG_TYPE_BATTERY_CUV,
	PLUG_TYPE_BATTERY_SCD,
	PLUG_TYPE_MAX_NUM,
};

/**
 * struct charge_mievent_info - one fault worth recording
 * @event_code:      the code it is filed under
 * @event_name:      which log it belongs to
 * @event_describe:  what happened
 * @upload_type:     whether it is limited per plug or on a timer
 * @upload_index:    its slot in the corresponding limit table
 * @data_type:       what its parameters are
 * @data_count:      how many it carries
 * @para_name:       what each one is called
 */
struct charge_mievent_info {
	int	event_code;
	char	event_name[MIEVENT_NAME_LEN];
	char	event_describe[MIEVENT_NAME_LEN];
	int	upload_type;
	int	upload_index;
	int	data_type;
	int	data_count;
	char	para_name[MIEVENT_PARA_MAX][MIEVENT_NAME_LEN];
};

/**
 * struct mievent_upload_type_plug - a fault limited per charger plugged in
 * @max_count: how many times it may be reported before the next plug
 * @count:     how many times it has been
 */
struct mievent_upload_type_plug {
	int	max_count;
	int	count;
};

/**
 * struct mievent_upload_type_time - a fault limited by how recently it was seen
 * @time_last:     when it was last reported, in seconds of boot time
 * @time_interval: how long to wait before reporting it again, in seconds
 * @count:         how many times it has happened, reported or not
 *
 * @count is what says whether @time_last means anything yet: a fault that has
 * never happened has neither, and the first one is always reported.
 */
struct mievent_upload_type_time {
	s64	time_last;
	int	time_interval;
	int	count;
};

static struct mievent_upload_type_plug g_upload_plug[MIEVENT_PLUG_MAX] = {
	{ .max_count = 1 },
	{ .max_count = 3 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 3 },
	{ .max_count = 3 },
	{ .max_count = 3 },
	{ .max_count = 3 },
	{ .max_count = 3 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 3 },
	{ .max_count = 3 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 3 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 5 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
	{ .max_count = 1 },
};

static struct mievent_upload_type_time g_upload_type_time_info[MIEVENT_TIME_MAX] = {
	{ .time_interval = 604800 },
	{ .time_interval = 600 },
	{ .time_interval = 600 },
	{ .time_interval = 300 },
	{ .time_interval = 300 },
	{ .time_interval = 300 },
	{ .time_interval = 300 },
	{ .time_interval = 604800 },
};

static const struct charge_mievent_info g_charge_mievent_info[CHARGE_DFX_MAX_NUM] = {
	[CHARGE_DFX_PD_AUTH_FAILED] = {
		.event_code	= MIEVENT_CODE_PD_AUTH_FAILED,
		.event_name	= "chgErrInfo",
		.event_describe	= "PdAuthFail",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_PD_AUTH_FAILED,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "adapterId" },
	},
	[CHARGE_DFX_CP_OPEN_FAILED] = {
		.event_code	= MIEVENT_CODE_CP_OPEN_FAILED,
		.event_name	= "chgErrInfo",
		.event_describe	= "CpEnFail",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_CP_OPEN_FAILED,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "cpId" },
	},
	[CHARGE_DFX_NOT_STANDARD_ADAPTER] = {
		.event_code	= MIEVENT_CODE_NON_STANDARD_ADAPTER,
		.event_name	= "chgErrInfo",
		.event_describe	= "NoneStandartChg",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_NOT_STANDARD_ADAPTER,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_RP_SHORT_VBUS_DETECTED] = {
		.event_code	= MIEVENT_CODE_RP_SHORT_VBUS_DETECTED,
		.event_name	= "chgErrInfo",
		.event_describe	= "CorrosionDischarge",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_RP_SHORT_VBUS_DETECTED,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_LPD_DETECTED] = {
		.event_code	= MIEVENT_CODE_LPD_DETECTED,
		.event_name	= "chgErrInfo",
		.event_describe	= "LpdDetected",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_LPD_DETECTED,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "lpdFlag" },
	},
	[CHARGE_DFX_CP_VBUS_OVP] = {
		.event_code	= MIEVENT_CODE_CP_VBUS_OVP,
		.event_name	= "chgErrInfo",
		.event_describe	= "CpVbusOvp",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_CP_VBUS_OVP,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_CP_IBUS_OCP] = {
		.event_code	= MIEVENT_CODE_CP_IBUS_OCP,
		.event_name	= "chgErrInfo",
		.event_describe	= "CpIbusOcp",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_CP_IBUS_OCP,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_CP_VBAT_OVP] = {
		.event_code	= MIEVENT_CODE_CP_VBAT_OVP,
		.event_name	= "chgErrInfo",
		.event_describe	= "CpVbatOvp",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_CP_VBAT_OVP,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 2,
		.para_name	= { "vbat", "vbatMax" },
	},
	[CHARGE_DFX_CP_IBAT_OCP] = {
		.event_code	= MIEVENT_CODE_CP_IBAT_OCP,
		.event_name	= "chgErrInfo",
		.event_describe	= "CpIbatOcp",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_CP_IBAT_OCP,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_CP_VAC_OVP] = {
		.event_code	= MIEVENT_CODE_CP_VAC_OVP,
		.event_name	= "chgErrInfo",
		.event_describe	= "CpVacOvp",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_CP_VAC_OVP,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_ANTI_BURN_TRIGGERED] = {
		.event_code	= MIEVENT_CODE_ANTI_BURN_TRIGGERED,
		.event_name	= "chgStatInfo",
		.event_describe	= "AntiBurnTirg",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_ANTI_BURN_TRIGGERED,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "tconn" },
	},
	[CHARGE_DFX_SOC_NOT_FULL] = {
		.event_code	= MIEVENT_CODE_SOC_NOT_FULL,
		.event_name	= "chgErrInfo",
		.event_describe	= "SocNotFull",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_SOC_NOT_FULL,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 3,
		.para_name	= { "vbat", "soc", "rsoc" },
	},
	[CHARGE_DFX_SMART_ENDURANCE_TRIGGERED] = {
		.event_code	= MIEVENT_CODE_SMART_ENDURANCE_TRIGGERED,
		.event_name	= "chgStatInfo",
		.event_describe	= "SmartEnduraTrig",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_SMART_ENDURANCE_TRIGGERED,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "soc" },
	},
	[CHARGE_DFX_SMART_NAVIGATION_TRIGGERED] = {
		.event_code	= MIEVENT_CODE_SMART_NAVIGATION_TRIGGERED,
		.event_name	= "chgStatInfo",
		.event_describe	= "SmartNaviTrig",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_SMART_NAVIGATION_TRIGGERED,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "soc" },
	},
	[CHARGE_DFX_BATTERY_MISSING] = {
		.event_code	= MIEVENT_CODE_BATTERY_MISSING,
		.event_name	= "chgErrInfo",
		.event_describe	= "BattLinkerAbsent",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_BATTERY_MISSING,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_CP_TDIE_HOT] = {
		.event_code	= MIEVENT_CODE_CP_TDIE_HOT,
		.event_name	= "chgStatInfo",
		.event_describe	= "CpTdieHot",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_CP_TDIE_HOT,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 2,
		.para_name	= { "masterTdie", "slaveTdie" },
	},
	[CHARGE_DFX_VBUS_UVLO] = {
		.event_code	= MIEVENT_CODE_VBUS_UVLO,
		.event_name	= "chgErrInfo",
		.event_describe	= "VbusUvlo",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_VBUS_UVLO,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 3,
		.para_name	= { "vbat", "vbus", "aicl" },
	},
	[CHARGE_DFX_LOW_TEMP_DISCHARGING] = {
		.event_code	= MIEVENT_CODE_LOW_TEMP_DISCHARGING,
		.event_name	= "chgErrInfo",
		.event_describe	= "NotChgInLowTemp",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_LOW_TEMP_DISCHARGING,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "tbat" },
	},
	[CHARGE_DFX_HIGH_TEMP_DISCHARGING] = {
		.event_code	= MIEVENT_CODE_HIGH_TEMP_DISCHARGING,
		.event_name	= "chgErrInfo",
		.event_describe	= "NotChgInHighTemp",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_HIGH_TEMP_DISCHARGING,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "tbat" },
	},
	[CHARGE_DFX_DUAL_BATTERY_MISSING] = {
		.event_code	= MIEVENT_CODE_DUAL_BATTERY_MISSING,
		.event_name	= "chgErrInfo",
		.event_describe	= "DualBattLinkerAbsent",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_DUAL_BATTERY_MISSING,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "status" },
	},
	[CHARGE_DFX_SMART_ENDURANCE_SOC_ERR] = {
		.event_code	= MIEVENT_CODE_SMART_ENDURANCE_SOC_ERR,
		.event_name	= "chgErrInfo",
		.event_describe	= "SmartEnduraSocErr",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_SMART_ENDURANCE_SOC_ERR,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "soc" },
	},
	[CHARGE_DFX_SMART_NAVIGATION_SOC_ERR] = {
		.event_code	= MIEVENT_CODE_SMART_NAVIGATION_SOC_ERR,
		.event_name	= "chgErrInfo",
		.event_describe	= "SmartNaviSocErr",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_SMART_NAVIGATION_SOC_ERR,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "soc" },
	},
	[CHARGE_DFX_BATTERY_AUTH_FAIL] = {
		.event_code	= MIEVENT_CODE_BATTERY_AUTH_FAIL,
		.event_name	= "chgErrInfo",
		.event_describe	= "BattAuthFail",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_BATTERY_AUTH_FAIL,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_DUAL_BATTERY_AUTH_FAIL] = {
		.event_code	= MIEVENT_CODE_DUAL_BATTERY_AUTH_FAIL,
		.event_name	= "chgErrInfo",
		.event_describe	= "ChgBattAuthFail",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_DUAL_BATTERY_AUTH_FAIL,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "status" },
	},
	[CHARGE_DFX_ANTIBURN_ERR] = {
		.event_code	= MIEVENT_CODE_ANTIBURN_ERR,
		.event_name	= "chgErrInfo",
		.event_describe	= "AntiFail",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_ANTIBURN_ERR,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_WLS_FASTCHG_FAIL] = {
		.event_code	= MIEVENT_CODE_WLS_FASTCHG_FAIL,
		.event_name	= "chgErrInfo",
		.event_describe	= "WlsFastChgFail",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_WLS_FASTCHG_FAIL,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_WLS_FOD_LOW_POWER] = {
		.event_code	= MIEVENT_CODE_WLS_FOD_LOW_POWER,
		.event_name	= "chgErrInfo",
		.event_describe	= "WlsQLow",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_WLS_FOD_LOW_POWER,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 2,
		.para_name	= { "chgQbase", "chgQreal" },
	},
	[CHARGE_DFX_WLS_RX_OTP] = {
		.event_code	= MIEVENT_CODE_WLS_RX_OTP,
		.event_name	= "chgErrInfo",
		.event_describe	= "WlsRxOTP",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_WLS_RX_OTP,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "chgRxtemp" },
	},
	[CHARGE_DFX_WLS_RX_OVP] = {
		.event_code	= MIEVENT_CODE_WLS_RX_OVP,
		.event_name	= "chgErrInfo",
		.event_describe	= "WlsRxOVP",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_WLS_RX_OVP,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_WLS_RX_OCP] = {
		.event_code	= MIEVENT_CODE_WLS_RX_OCP,
		.event_name	= "chgErrInfo",
		.event_describe	= "WlsRxOCP",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_WLS_RX_OCP,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_WLS_TRX_FOD] = {
		.event_code	= MIEVENT_CODE_WLS_TRX_FOD,
		.event_name	= "chgErrInfo",
		.event_describe	= "WlsTrxFod",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_WLS_TRX_FOD,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_WLS_TRX_OCP] = {
		.event_code	= MIEVENT_CODE_WLS_TRX_OCP,
		.event_name	= "chgErrInfo",
		.event_describe	= "WlsTrxOCP",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_WLS_TRX_OCP,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_WLS_TRX_UVLO] = {
		.event_code	= MIEVENT_CODE_WLS_TRX_UVLO,
		.event_name	= "chgErrInfo",
		.event_describe	= "WlsTrxUVLO",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_WLS_TRX_UVLO,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_WLS_TRX_IIC_ERR] = {
		.event_code	= MIEVENT_CODE_WLS_TRX_IIC_ERR,
		.event_name	= "chgErrInfo",
		.event_describe	= "WlsTrxI2cErr",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_WLS_TRX_IIC_ERR,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_WLS_RX_IIC_ERR] = {
		.event_code	= MIEVENT_CODE_WLS_RX_IIC_ERR,
		.event_name	= "chgErrInfo",
		.event_describe	= "WlsRxI2cErr",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_WLS_RX_IIC_ERR,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_LOAD_SWITCH_I2C_ERR] = {
		.event_code	= MIEVENT_CODE_LOAD_SWITCH_I2C_ERR,
		.event_name	= "chgErrInfo",
		.event_describe	= "LoadSwitchI2cErr",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_LOAD_SWITCH_I2C_ERR,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
	[CHARGE_DFX_WLS_FW_UPGRADE_FAIL] = {
		.event_code	= MIEVENT_CODE_WLS_FW_UPGRADE_FAIL,
		.event_name	= "chgErrInfo",
		.event_describe	= "WlsFwUpgradeErr",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_WLS_FW_UPGRADE_FAIL,
		.data_type	= MIEVENT_DATA_TYPE_STRING,
		.data_count	= 1,
		.para_name	= { "errReason" },
	},
	[CHARGE_DFX_XMPPS_CHARGER_SLOWLY] = {
		.event_code	= MIEVENT_CODE_XMPPS_CHARGER_SLOWLY,
		.event_name	= "chgStatInfo",
		.event_describe	= "ChargeSlowly",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_XMPPS_CHARGER_SLOWLY,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 5,
		.para_name	= { "maxPower", "initialRm", "endRm", "initialSoc", "endSoc" },
	},
	[CHARGE_DFX_NON_STANDARD_CHARGER] = {
		.event_code	= MIEVENT_CODE_NON_STANDARD_CHARGER,
		.event_name	= "chgStatInfo",
		.event_describe	= "NonStandardCharger",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_NON_STANDARD_CHARGER,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "chgType" },
	},
	[CHARGE_DFX_BATTERY_OCD] = {
		.event_code	= MIEVENT_CODE_BATTERY_OCD,
		.event_name	= "chgErrInfo",
		.event_describe	= "BatteryOcd",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_BATTERY_OCD,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 7,
		.para_name	= { "ocdCount", "ocdLastCycle", "hocdCount", "hocdLastCycle", "uiSoc", "voltage", "temp" },
	},
	[CHARGE_DFX_BATTERY_CUV] = {
		.event_code	= MIEVENT_CODE_BATTERY_CUV,
		.event_name	= "chgErrInfo",
		.event_describe	= "BatteryCuv",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_BATTERY_CUV,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 7,
		.para_name	= { "cuvCount", "cuvLastCycle", "hcuvCount", "hcuvLastCycle", "uiSoc", "voltage", "temp" },
	},
	[CHARGE_DFX_BATTERY_SCD] = {
		.event_code	= MIEVENT_CODE_BATTERY_SCD,
		.event_name	= "chgErrInfo",
		.event_describe	= "BatteryScd",
		.upload_type	= MIEVENT_UPLOAD_TYPE_PLUG,
		.upload_index	= PLUG_TYPE_BATTERY_SCD,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 2,
		.para_name	= { "hscdCount", "hscdLastCycle" },
	},
	[CHARGE_DFX_BATTERY_CYCLECOUNT] = {
		.event_code	= MIEVENT_CODE_BATTERY_CYCLECOUNT,
		.event_name	= "chgStatInfo",
		.event_describe	= "chgBattCycle",
		.upload_type	= MIEVENT_UPLOAD_TYPE_TIME,
		.upload_index	= 0,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "cycleCnt" },
	},
	[CHARGE_DFX_FG_IIC_ERR] = {
		.event_code	= MIEVENT_CODE_FG_IIC_ERR,
		.event_name	= "chgErrInfo",
		.event_describe	= "FgI2cErr",
		.upload_type	= MIEVENT_UPLOAD_TYPE_TIME,
		.upload_index	= 1,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 1,
		.para_name	= { "soc" },
	},
	[CHARGE_DFX_CP_ABSENT] = {
		.event_code	= MIEVENT_CODE_CP_ABSENT,
		.event_name	= "chgErrInfo",
		.event_describe	= "CpI2cErr",
		.upload_type	= MIEVENT_UPLOAD_TYPE_TIME,
		.upload_index	= 2,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 2,
		.para_name	= { "masterOk", "slaveOk" },
	},
	[CHARGE_DFX_VBATT_SOC_NOT_MATCH] = {
		.event_code	= MIEVENT_CODE_VBATT_SOC_NOT_MATCH,
		.event_name	= "chgErrInfo",
		.event_describe	= "VbatSocNotMatch",
		.upload_type	= MIEVENT_UPLOAD_TYPE_TIME,
		.upload_index	= 3,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 3,
		.para_name	= { "vbat", "soc", "cycleCnt" },
	},
	[CHARGE_DFX_BATTERY_TEMP_HOT] = {
		.event_code	= MIEVENT_CODE_BATTERY_TEMP_HOT,
		.event_name	= "chgStatInfo",
		.event_describe	= "TbatHot",
		.upload_type	= MIEVENT_UPLOAD_TYPE_TIME,
		.upload_index	= 4,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 4,
		.para_name	= { "tbat", "tbatMax", "isCharging", "tboard" },
	},
	[CHARGE_DFX_BATTERY_TEMP_COLD] = {
		.event_code	= MIEVENT_CODE_BATTERY_TEMP_COLD,
		.event_name	= "chgStatInfo",
		.event_describe	= "TbatCold",
		.upload_type	= MIEVENT_UPLOAD_TYPE_TIME,
		.upload_index	= 5,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 4,
		.para_name	= { "tbat", "tbatMin", "isCharging", "tboard" },
	},
	[CHARGE_DFX_BATTERY_VOLTAGE_DIFFER] = {
		.event_code	= MIEVENT_CODE_BATTERY_VOLTAGE_DIFFER,
		.event_name	= "chgErrInfo",
		.event_describe	= "DualVbatDiff",
		.upload_type	= MIEVENT_UPLOAD_TYPE_TIME,
		.upload_index	= 6,
		.data_type	= MIEVENT_DATA_TYPE_INT,
		.data_count	= 2,
		.para_name	= { "chgBaseBattVol", "chgFlipBattVol" },
	},
	[CHARGE_DFX_WLS_MAGNETIC_CASE_ATTACH] = {
		.event_code	= MIEVENT_CODE_WLS_MAGNETIC_CASE_ATTACH,
		.event_name	= "chgStatInfo",
		.event_describe	= "WlsMagCaseAttach",
		.upload_type	= MIEVENT_UPLOAD_TYPE_TIME,
		.upload_index	= 7,
		.data_type	= MIEVENT_DATA_TYPE_NULL,
		.data_count	= 0,
	},
};

/**
 * mca_charge_mievent_report() - record a charging fault
 * @event_index: which fault
 * @data:        its parameters: an array of int, or of pointers to string,
 *               according to what the fault declares
 * @data_len:    how many parameters @data holds
 *
 * A fault that keeps happening is worth knowing about once, not continuously.
 * The faults tied to a particular charger are counted per plug, so a bad
 * adapter is reported while it is attached and not again until the next one;
 * the rest are held off for an interval, which for the slowest -- a cell that
 * has aged past its cycle count -- is a week.  A fault held off is still
 * counted, so the count says how often it really happened.
 */
void mca_charge_mievent_report(int event_index, void *data, int data_len)
{
	const struct charge_mievent_info *info;
	struct mievent_upload_type_time *t;
	struct mievent_upload_type_plug *p;
	struct mca_event_notify_data ndata;
	const char *str;
	int *count;
	s64 now_sec;
	int i;

	if (event_index < 0 || event_index >= CHARGE_DFX_MAX_NUM) {
		mca_log_err("event_index[%d] invalid", event_index);
		return;
	}

	info = &g_charge_mievent_info[event_index];

	if (info->data_count != data_len) {
		mca_log_err("data_type is int param size invalid\n");
		return;
	}

	if (!data && data_len) {
		mca_log_err("data_type is int param null invalid\n");
		return;
	}

	/*
	 * A fault that stopped charging from working is one the phone as a
	 * whole wants to hear about, so it goes out under the tag that is
	 * read for hardware faults as well.  The adapter merely not being a
	 * standard one is left out: it is common, and nothing is broken.
	 */
	if (!strcmp(info->event_name, MIEVENT_ERR_LOG_NAME) &&
	    info->event_code != MIEVENT_CODE_NON_STANDARD_ADAPTER)
		mca_log_err_arch_tf("%s %s\n", info->event_name,
				    info->event_describe);

	if (info->upload_type == MIEVENT_UPLOAD_TYPE_TIME) {
		if (info->upload_index >= MIEVENT_TIME_MAX)
			return;

		t = &g_upload_type_time_info[info->upload_index];
		now_sec = div_s64(ktime_get_coarse_boottime(), NSEC_PER_SEC);

		if (t->count && now_sec - t->time_last < t->time_interval) {
			t->count++;
			return;
		}

		mca_log_err("event_code[%d] must report fault\n",
			    info->event_code);
		t->time_last = now_sec;
		count = &t->count;
	} else {
		if (info->upload_index >= MIEVENT_PLUG_MAX)
			return;

		p = &g_upload_plug[info->upload_index];
		if (p->count >= p->max_count) {
			p->count++;
			return;
		}

		mca_log_err("event_code[%d][%s] must report fault\n",
			    info->event_code, info->event_describe);
		count = &p->count;
	}

	/*
	 * The charging log covering the fault is only useful if it is
	 * collected, so reporting one asks for it in the same breath.
	 */
	ndata.event = MCA_LOG_FULL_EVENT;
	ndata.event_len = strlen(MCA_LOG_FULL_EVENT);
	mca_event_report_uevent(&ndata);

	(*count)++;

	/*
	 * MIUI's telemetry service is not part of this build, so the event
	 * is only logged rather than handed to the miev driver.
	 */
	mca_log_err("[%d] [%s] [%s]\n", info->event_code, info->event_name,
		    info->event_describe);

	if (info->data_type == MIEVENT_DATA_TYPE_INT) {
		for (i = 0; i < info->data_count; i++)
			mca_log_err("[%s] [%d]\n", info->para_name[i],
				    ((int *)data)[i]);
	} else if (info->data_type == MIEVENT_DATA_TYPE_STRING) {
		for (i = 0; i < info->data_count; i++) {
			str = ((char **)data)[i];
			if (strlen(str) >= MIEVENT_STR_MAX)
				mca_log_err("[%s] [%s] String Len overflow\n",
					    info->para_name[i], str);
			else
				mca_log_err("[%s] [%s]\n", info->para_name[i],
					    str);
		}
	}
}
EXPORT_SYMBOL_GPL(mca_charge_mievent_report);

/**
 * mca_charge_mievent_set_state() - tell the reporter what the charger is doing
 * @state: what happened
 * @value: what goes with it -- whether a charger is attached for
 *         %MIEVENT_STATE_PLUG, which fault has finished for %MIEVENT_STATE_END
 *
 * A charger being unplugged starts a fresh count for every fault that is
 * reported per plug, so a fault seen with the last adapter does not suppress
 * the same fault with the next one.  Nothing is reset while a charger is
 * still attached: the counts are what is holding the log down.
 */
void mca_charge_mievent_set_state(enum charge_mievent_state_ele state,
				  int value)
{
	const struct charge_mievent_info *info;
	int i;

	switch (state) {
	case MIEVENT_STATE_PLUG:
		if (value) {
			mca_log_info("don't plug out can't reset fault status");
			return;
		}

		for (i = 0; i < MIEVENT_PLUG_MAX; i++)
			g_upload_plug[i].count = 0;

		return;
	case MIEVENT_STATE_END:
		if (value < 0 || value >= CHARGE_DFX_MAX_NUM) {
			mca_log_err("event_index[%d] invalid", value);
			return;
		}

		info = &g_charge_mievent_info[value];

		if (info->upload_type == MIEVENT_UPLOAD_TYPE_TIME) {
			if (info->upload_index >= MIEVENT_TIME_MAX)
				return;

			g_upload_type_time_info[info->upload_index].time_last = 0;
			g_upload_type_time_info[info->upload_index].count = 0;
		} else if (info->upload_type == MIEVENT_UPLOAD_TYPE_PLUG) {
			if (info->upload_index >= MIEVENT_PLUG_MAX)
				return;

			g_upload_plug[info->upload_index].count = 0;
		}

		return;
	default:
		return;
	}
}
EXPORT_SYMBOL_GPL(mca_charge_mievent_set_state);

MODULE_DESCRIPTION("mca charge mievent");
MODULE_AUTHOR("lvxiaofeng@xiaomi.com");
MODULE_LICENSE("GPL v2");
