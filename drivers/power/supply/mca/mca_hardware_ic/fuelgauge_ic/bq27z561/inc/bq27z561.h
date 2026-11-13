/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bq27z561.h
 *
 * fuelgauge reg
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
#ifndef __BQ27Z561_H
#define __BQ27Z561_H

#include <linux/atomic.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define PROBE_CNT_MAX		50

#define INVALID_REG_ADDR	0xFF

#define FG_FLAGS_CO		BIT(2)
#define FG_FLAGS_FD		BIT(4)
#define FG_FLAGS_FC		BIT(5)
#define FG_FLAGS_DSG		BIT(6)
#define FG_FLAGS_RCA		BIT(9)

#define BATTERY_DIGEST_LEN	32
#define BATTERY_RANDOM_LEN	64
#define BATTERY_UNSEAL_KEY_LEN	4

#define DEFUALT_FULL_DESIGN	5000

#define FG_ERROR_CHECK_MAX_TIMES	3
#define FG_ERROR_FAKE_TEMP		250
#define FG_ERROR_FAKE_VOLT		3700
#define FG_ERROR_FAKE_CURR		(-500 * 1000)
#define FG_FAKE_TEMP_NONE		(-999)
#define FG_FAKE_SOH_NONE		(-1)

enum bq_fg_reg_idx {
	BQ_FG_REG_CTRL = 0,
	BQ_FG_REG_TEMP,		/* Battery Temperature */
	BQ_FG_REG_VOLT,		/* Battery Voltage */
	BQ_FG_REG_CN,		/* Current Now */
	BQ_FG_REG_AVER_CURRENT,	/* Average Current */
	BQ_FG_REG_BATT_STATUS,	/* BatteryStatus */
	BQ_FG_REG_PACK_VOLT,	/* Pack Voltage */
	BQ_FG_REG_ORIGINAL_TEMP,	/* InternalTemperature */
	BQ_FG_REG_TTE,		/* Time to Empty */
	BQ_FG_REG_TTF,		/* Time to Full */
	BQ_FG_REG_FCC,		/* Full Charge Capacity */
	BQ_FG_REG_RM,		/* Remaining Capacity */
	BQ_FG_REG_CC,		/* Cycle Count */
	BQ_FG_REG_SOC,		/* Relative State of Charge */
	BQ_FG_REG_SOH,		/* State of Health */
	BQ_FG_REG_CHG_VOL,	/* Charging Voltage */
	BQ_FG_REG_CHG_CUR,	/* Charging Current */
	BQ_FG_REG_DC,		/* Design Capacity */
	BQ_FG_REG_ALT_MAC,	/* AltManufacturerAccess */
	BQ_FG_REG_MAC_DATA,	/* MACData */
	BQ_FG_REG_MAC_CHKSUM,	/* MACChecksum */
	BQ_FG_REG_MAC_DATA_LEN,	/* MACDataLen */
	NVT_FG_REG_OVER_PEAK,	/* over peak flag */
	NVT_FG_REG_CUR_DEV,	/* current deviation */
	NVT_FG_REG_POW_DEV,	/* power deviation */
	NVT_FG_AVE_CUR,		/* 10s average current */
	NVT_FG_AVE_TEMP,	/* 10s average temperature */
	NVT_FG_REG_SOA_L,	/* soa alert */
	NVT_FG_REG_SOA_H,	/* soa alert */
	NVT_FG_REG_ISC,		/* isc alert */
	NVT_FG_REG_START_LEARNING,	/* start learning */
	NVT_FG_REG_STOP_LEARNING,	/* stop learning */
	NVT_FG_REG_EST_POWER,	/* estimated power input */
	NVT_FG_REG_ACT_POWER,	/* actual power output */
	NVT_FG_REG_POWER_DEV,	/* estimated - actual power */
	NVT_FG_REG_TIME_DEV,	/* estimated - actual power time */
	NVT_FG_REG_CONST_POWER,	/* constant power value */
	NVT_FG_REG_CONST_POWER_TM,	/* constant power value time */
	NVT_FG_REG_REF_POWER,	/* recommended reference power */
	NVT_FG_REG_REF_CURRENT,	/* recommended discharge current */
	NVT_FG_REG_NVT_REF_CURRENT,	/* recommended discharge power */
	NVT_FG_REG_START_LEARNING_B,	/* start learning B */
	NVT_FG_REG_STOP_LEARNING_B,	/* stop learning B */
	NVT_FG_REG_EST_POWER_B,	/* estimated power input B */
	NVT_FG_REG_ACT_POWER_B,	/* actual power output B */
	NVT_FG_REG_POWER_DEV_B,	/* estimated - actual power B */
	NUM_REGS,
};

static const u8 bq27z561_regs[NUM_REGS] = {
	0x00,	/* CONTROL */
	0x06,	/* TEMP */
	0x08,	/* VOLT */
	0x0C,	/* CURRENT NOW */
	0x14,	/* AVG CURRENT */
	0x0A,	/* FLAGS */
	0x0E,	/* PACK VOLT */
	0x1E,	/* InternalTemperature */
	0x16,	/* Time to empty */
	0x18,	/* Time to full */
	0x12,	/* Full charge capacity */
	0x10,	/* Remaining Capacity */
	0x2A,	/* CycleCount */
	0x2C,	/* State of Charge */
	0x2E,	/* State of Health */
	0x30,	/* Charging Voltage */
	0x32,	/* Charging Current */
	0x3C,	/* Design Capacity */
	0x3E,	/* AltManufacturerAccess */
	0x40,	/* MACData */
	0x60,	/* MACChecksum */
	0x61,	/* MACDataLen */
	0x78,	/* over peak flag */
	0x79,	/* current deviation */
	0x7B,	/* power deviation */
	0x81,	/* 10s average current */
	0x83,	/* 10s average temperature */
	0x70,	/* soa alert */
	0x71,	/* soa alert */
	0x72,	/* isc alert level */
	0x85,	/* start learning */
	0x86,	/* stop learning */
	0x87,	/* estimated power input */
	0x89,	/* actual power output */
	0x8B,	/* estimated - actual power */
	0x8D,	/* estimated - actual power time */
	0xA1,	/* constant power value */
	0xA3,	/* constant power value time */
	0xA5,	/* recommended reference power */
	0xA7,	/* recommended discharge current */
	0xA9,	/* recommended discharge power */
	0x93,	/* start learning B */
	0x94,	/* stop learning B */
	0x95,	/* estimated power input B */
	0x97,	/* actual power output B */
	0x99,	/* estimated - actual power B */
};

enum bq_fg_mac_cmd {
	FG_MAC_CMD_CTRL_STATUS		= 0x0000,
	FG_MAC_CMD_DEV_TYPE		= 0x0001,
	FG_MAC_CMD_FW_VER		= 0x0002,
	FG_MAC_CMD_HW_VER		= 0x0003,
	FG_MAC_CMD_IF_SIG		= 0x0004,
	FG_MCA_CMD_DF			= 0x0005,
	FG_MAC_CMD_CHEM_ID		= 0x0006,
	FG_MAC_CMD_CHEM_DF_SIGN		= 0x0008,
	FG_MAC_CMD_GAUGING		= 0x0021,
	FG_MAC_CMD_SEAL			= 0x0030,
	FG_MAC_CMD_FASTCHARGE_EN	= 0x003E,
	FG_MAC_CMD_FASTCHARGE_DIS	= 0x003F,
	FG_MAC_CMD_DEV_RESET		= 0x0041,
	FG_MAC_CMD_DEVICE_NAME		= 0x004A,
	FG_MAC_CMD_CHEM_NAME		= 0x004B,
	FG_MAC_CMD_MANU_NAME		= 0x004C,
	FG_MAC_CMD_CUTOFF_VOLTAGE	= 0x0050,
	FG_MCA_CMD_SEAL_STATE		= 0x0054,
	FG_MAC_CMD_CHARGING_STATUS	= 0x0055,
	FG_MAC_CMD_GAGUE_STATUS		= 0x0056,
	FG_MAC_CMD_CO_AUTO_OPEN		= 0x0057,
	FG_MAC_CMD_CUV_OCD		= 0x005E,
	FG_MAC_CMD_HCUV_HOCD		= 0x005F,
	FG_MAC_CMD_LIFETIME1		= 0x0060,
	FG_MAC_CMD_LIFETIME2		= 0x0061,
	FG_MAC_CMD_LIFETIME3		= 0x0062,
	FG_MAC_CMD_CALC_RVALUE		= 0x0066,
	FG_MAC_CMD_OVER_VOL_DURATION	= 0x0067,
	FG_MAC_CMD_REL_SOH		= 0x0069,
	FG_MAC_CMD_BATT_SN		= 0x0070,
	FG_MAC_CMD_QMAX_CYCLECOUNT	= 0x0071,
	FG_MAC_CMD_ITSTATUS1		= 0x0073,
	FG_MAC_CMD_QMAX			= 0x0075,
	FG_MAC_CMD_FCC_SOH		= 0x0077,
	FG_MAC_CMD_UI_SOH		= 0x007B,
	FG_MAC_CMD_EIS_SOH		= 0x009D,
	FG_MAC_CMD_CLOSE_CO		= 0x00C9,
	FG_MAC_CMD_OPEN_CO		= 0x00CC,
	FG_MAC_CMD_CALIBRATION_INFO_1	= 0x00D1,
	FG_MAC_CMD_CALIBRATION_INFO_2	= 0x00D2,
	FG_MAC_CMD_CALIBRATION_INFO_3	= 0x00D3,
	FG_MAC_CMD_CALIBRATION_INFO_4	= 0x00D4,
	/* MPC7012 keeps some readings behind its own command numbers. */
	FG_MAC_CMD_MPC7012_CUTOFF_VOLTAGE	= 0x004F,
	FG_MAC_CMD_MPC7012_CALC_RVALUE		= 0x1030,
	FG_MAC_CMD_MPC7012_OVER_VOL_DURATION	= 0x1031,
	FG_MAC_CMD_MIN_VOLTAGE		= 0x1031,
	FG_MAC_CMD_CLEAR_COUNT_DATA	= 0x1054,
	FG_MAC_CMD_FORCE_SIM_SOH	= 0x1056,
	FG_MAC_CMD_DCR_SLOPE		= 0x10A0,
	FG_MAC_CMD_CSD			= 0x10A2,
	FG_MAC_CMD_MIXDATARECORD1	= 0x2001,
	FG_MAC_CMD_FEATURE_INFO		= 0x40E0,
	FG_MAC_CMD_UPDATE_VERSION	= 0x440C,
	FG_MAC_CMD_FW_RUNTIME		= 0x4441,
	FG_MAC_CMD_RECORD_VOLTAGE_LEVEL	= 0x448A,
	FG_MAC_CMD_TERM_CURRENTS	= 0x456E,
};

enum fg_seal_status {
	SEAL_STATE_RSVED,
	SEAL_STATE_FA,
	SEAL_STATE_UNSEALED,
	SEAL_STATE_SEALED,
};

enum bq_fg_device {
	BQ27Z561_MASTER = 0,
	BQ27Z561_SLAVE,
	BQ28Z610,
};

enum fg_device_role {
	FG_MASTER,
	FG_SLAVE,
};

enum fg_fw_ver {
	FW_DEFAULT,
	FW_R0,
	FW_R1,
};

enum fg_pack_vendor {
	PACK_SUPPLIER_BYD = 0,
	PACK_SUPPLIER_COSLIGHT,
	PACK_SUPPLIER_SUNWODA,
	PACK_SUPPLIER_NVT,
	PACK_SUPPLIER_SCUD,
	PACK_SUPPLIER_TWS,
	PACK_SUPPLIER_LISHEN,
	PACK_SUPPLIER_DESAY,
};

enum fg_vendor {
	BQ27Z561 = 0,	/* TI */
	NFG1000A,	/* NVT-eastsoft */
	NFG1000B,	/* NVT-chipsea */
	MPC8011B,
	MPC7012,
};

enum fw_status_elem {
	FG_FW_INIT,
	FG_FW_ERROR,
	FG_FW_SUCCESS,
	FG_FW_INVALID,
};

#define NFG1000_OTA_BUF_LEN	512

struct bq_fg_chip {
	struct device *dev;
	struct device *sysfs_dev;
	struct i2c_client *client;

	/* dts config */
	unsigned int dev_role;
	int batt_dc;
	unsigned int soh_th;
	unsigned int cycle_th;
	unsigned int cycle_step;
	int resistance_id;
	int *dec_rate_seq;
	int dec_rate_len;
	u32 support_nvt1000_ota;
	u8 key[BATTERY_UNSEAL_KEY_LEN];
	bool support_voltage_record_level;
	bool support_version_compatible;
	u8 version_number;
	u8 start_byte_address;
	u8 byte_length;
	u8 *record_voltage;
	int record_voltage_len;
	int co_by_gpio;
	int ctl_co_gpio;

	struct mutex i2c_rw_lock;
	struct mutex data_lock;
	atomic_t pm_suspend;
	atomic_t digest_in_process;

	int fw_ver;
	u8 regs[NUM_REGS];
	char device_name[8];
	char fake_device_name[8];
	char eeprom_version;

	/* BatteryStatus */
	bool batt_fc;
	bool batt_fd;	/* full depleted */
	bool batt_dsg;
	bool batt_rca;	/* remaining capacity alarm */

	/* GaugingStatus */
	bool batt_fc_1;
	bool batt_fd_1;
	bool batt_tc_1;
	bool batt_td_1;

	/* last good readings, used when a reading is out of range */
	int batt_temp;
	int original_temp;
	int batt_volt;
	int batt_curr;
	int batt_rm;
	int batt_fcc;
	int batt_rsoc;
	int batt_cycle_count;
	int batt_soh;

	u8 digest[BATTERY_DIGEST_LEN];
	int batt_auth;

	bool fg_error;
	bool fake_first_usage_date;
	bool fast_mode;

	/* lifetime data */
	int cell1_max;
	int max_charge_current;
	int max_discharge_current;
	int max_temp_cell;
	int min_temp_cell;
	int total_fw_runtime;
	int time_spent_in_lt;
	int time_spent_in_ht;
	int time_spent_in_ot;

	const char *cell_name;
	int fg_vendor;
	int pack_vendor;
	int fake_cycle;
	int fake_temp;
	int fake_isc;
	int fake_soh;
	int adapt_power;
	int aged_flag;
	unsigned int ui_soh;
	int count_level1;
	int count_level2;
	int count_level3;
	int count_lowtemp;
	int fake_count_level1;
	int fake_count_level2;
	int fake_count_level3;
	int fake_count_lowtemp;
	int update_fw_status;

	/* abnormal event records */
	int max_dsg_current;
	int min_temp;
	int min_voltage;
	int cuv_count;
	int cuv_last_cycle;
	int hcuv_count;
	int hcuv_last_cycle;
	int ocd_count;
	int ocd_last_cycle;
	int hocd_count;
	int hocd_last_cycle;
	int hscd_count;
	int hscd_last_cycle;

	/* NFG1000 firmware update */
	bool ota_updating;
	unsigned int nfg1000_ver;
	u32 ota_update_pending;
	u32 nfg1000_section_marker;
	u32 nfg1000_magic;
	u8 nfg1000_ota_buf[NFG1000_OTA_BUF_LEN];
};

/*
 * What fg_ic_get_batt_info() fills in; the gauge strategy reads it with the
 * same layout.
 */
struct fg_batt_info {
	int curr;
	int volt;
	int temp;
	int original_temp;
	int rsoc;
	int rm;
	int fcc;
	int cycle_count;
	int soh;
};

#endif /* __BQ27Z561_H */
