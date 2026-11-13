/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sc8581.h
 *
 * charge-pump ic driver
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

#ifndef __SC8581_H__
#define __SC8581_H__

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#define SC8581_ROLE_MASTER		0
#define SC8581_ROLE_SLAVE		1
#define ERROR_RECOVERY_COUNT		5

/* Defaults for the thresholds the board does not set, in mV. */
#define WPC_OVP_TH			22000
#define OUT_OVP_TH			5000
#define PMID2OUT_UVP_TH			100
#define PMID2OUT_OVP_TH			600
#define BAT_OVP_TH			4500
#define BAT_OVP_ALARM_TH		4400
#define CP_DEFAULT_FSW			600

/* Bus OVP window accepted per division, in mV. */
#define SC8581_BUS_OVP_41MODE_MIN	14000
#define SC8585_BUS_OVP_41MODE_MIN	15000
#define SC85XX_BUS_OVP_41MODE_MAX	22000
#define SC8581_BUS_OVP_21MODE_MIN	7000
#define SC8585_BUS_OVP_21MODE_MIN	7500
#define SC85XX_BUS_OVP_21MODE_MAX	13300
#define SC8581_BUS_OVP_11MODE_MIN	3500
#define SC8585_BUS_OVP_11MODE_MIN	3750
#define SC85XX_BUS_OVP_11MODE_MAX	5500

/* USB/WPC OVP values that select the 6.5 V (7.5 V on SC8585) setting. */
#define SC8581_USB_OVP_6PV5_MV		6500
#define SC8585_USB_OVP_7PV5_MV		7500

/* REG_02 is always programmed to an 8 A battery OCP (not on SC8585). */
#define BAT_OCP_TH			8000

/* PMID2OUT UVP is encoded from 100 mV in 50 mV steps (not on SC8585). */
#define SC8581_PMID2OUT_UVP_MIN		100

/* REG_42 value written at init on SC8585. */
#define SC8585_ACDRV_INIT		0x82

#define SC8585_REG_VOUT2OUT_UVP		0x84
#define SC8585_VOUT2OUT_UVP_HIGH	0x03

#define SC8581_REG_KEY			0x76

enum sc8581_adc_ch {
	ADC_IBUS,
	ADC_VBUS,
	ADC_VUSB,
	ADC_VWPC,
	ADC_VOUT,
	ADC_VBAT,
	ADC_IBAT,
	ADC_TBAT,
	ADC_TDIE,
	ADC_MAX_NUM,
};

enum chg_mode {
	CP_MODE_DIV4,
	CP_MODE_DIV2,
	CP_MODE_DIV1,
	CP_MODE_DIV_MAX,
};

struct sc8581_cfg {
	u32 bat_ovp_th;
	u32 bat_ovp_alarm_th;
	u32 wpc_ovp_th;
	u32 out_ovp_th;
	u32 pmid2out_uvp_th;
	u32 pmid2out_ovp_th;
	u32 bus_ovp_th[CP_MODE_DIV_MAX];
	u32 usb_ovp_th[CP_MODE_DIV_MAX];
	u32 bus_ocp_th[CP_MODE_DIV_MAX];
};

struct sc8581_fsw_cfg {
	int max;
	int min;
	int step;
};

struct sc8581_device {
	struct i2c_client *client;
	struct device *dev;
	struct sc8581_cfg cfg;
	struct sc8581_fsw_cfg fsw_cfg;
	bool chip_ok;
	char log_tag[25];
	int work_mode;
	int operation_mode;
	int chip_vendor;
	u32 cp_role;

	struct delayed_work irq_handle_work;
	int irq_gpio;
	int irq;
	int nlpm_gpio;
	bool ovpgate_en;
	bool i2c_is_working;
	bool support_reverse_quick_charge;
};

#endif /* __SC8581_H__ */
