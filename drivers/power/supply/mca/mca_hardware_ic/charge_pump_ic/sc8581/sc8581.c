// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Southchip SC8561/SC8581/SC8585 charge pump.
 *
 * The pump divides the adapter voltage by four, two or one so the battery
 * can be charged at high current; it also carries the OVP and WPC gates in
 * front of the inputs, which is why the reverse-charging and wireless paths
 * reach into it.  The charging strategy drives it through the platform
 * charge pump class; this driver programs the protections from the device
 * tree, reports faults as MCA events and exposes the ADC.
 */

#define MCA_LOG_TAG "cp_sc8581"

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/swab.h>
#include <linux/workqueue.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_hwid.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/common/mca_sysfs.h>
#include <mca/platform/platform_cp_class.h>

#include "inc/sc8581.h"
#include "inc/sc8581_reg.h"

/* Boards that move the pump to 0x6f to make room for an extra buck. */
#define EN_BUCK_PLATFORM_VERSION	3
#define EN_BUCK_HWID_VALUE		0x10004
#define EN_BUCK_CP_ADDR			0x6f

#define CP_DEBUGFS_MAX_COUNT		0x42

enum cp_debugfs_attr_list {
	CP_DEBUGFS_ADDRESS,
	CP_DEBUGFS_COUNT,
	CP_DEBUGFS_DATA,
};

/* Register window shown by the debugfs files, shared by every instance. */
static struct {
	int address;
	int count;
} reg_info;

/* Per operating mode: which set of per-division thresholds applies. */
static const int sc8581_work_mode[] = {
	CP_MODE_DIV4, CP_MODE_DIV2, CP_MODE_DIV1, CP_MODE_DIV1,
	CP_MODE_DIV4, CP_MODE_DIV2, CP_MODE_DIV1, CP_MODE_DIV1,
};

/* Per operating mode: the value of the REG_0E mode field. */
static const u8 sc8581_mode_reg[] = {
	SC8581_FORWARD_4_1_CHARGER_MODE,
	SC8581_FORWARD_2_1_CHARGER_MODE,
	SC8581_FORWARD_1_1_CHARGER_MODE,
	SC8581_FORWARD_1_1_CHARGER_MODE,
	SC8581_REVERSE_1_4_CONVERTER_MODE,
	SC8581_REVERSE_1_2_CONVERTER_MODE,
	SC8581_REVERSE_1_1_CONVERTER_MODE,
	SC8581_REVERSE_1_1_CONVERTER_MODE,
};

/* Bus OVP encoding per work mode, indexed by chip_vendor - 1. */
static const int sc8581_bus_ovp_base[CP_MODE_DIV_MAX][3] = {
	{ SC8581_BUS_OVP_41MODE_BASE, SC8581_BUS_OVP_41MODE_BASE,
	  SC8585_BUS_OVP_41MODE_BASE },
	{ SC8581_BUS_OVP_21MODE_BASE, SC8581_BUS_OVP_21MODE_BASE,
	  SC8585_BUS_OVP_21MODE_BASE },
	{ SC8581_BUS_OVP_11MODE_BASE, SC8581_BUS_OVP_11MODE_BASE,
	  SC8585_BUS_OVP_11MODE_BASE },
};

static const int sc8581_bus_ovp_lsb[CP_MODE_DIV_MAX][3] = {
	{ SC8581_BUS_OVP_41MODE_LSB, SC8581_BUS_OVP_41MODE_LSB,
	  SC8585_BUS_OVP_41MODE_LSB },
	{ SC8581_BUS_OVP_21MODE_LSB, SC8581_BUS_OVP_21MODE_LSB,
	  SC8585_BUS_OVP_21MODE_LSB },
	{ SC8581_BUS_OVP_11MODE_LSB, SC8581_BUS_OVP_11MODE_LSB,
	  SC8585_BUS_OVP_11MODE_LSB },
};

static const u8 sc8581_bus_ovp_mask[3] = {
	SC8581_BUS_OVP_MASK, SC8581_BUS_OVP_MASK, SC8585_BUS_OVP_MASK,
};

static int cp_read_byte(struct sc8581_device *chip, u8 reg, u8 *data)
{
	int ret = i2c_smbus_read_byte_data(chip->client, reg);

	if (ret < 0) {
		mca_log_info("i2c read byte fail: can't read from reg 0x%02X, errcode=%d\n",
			     reg, ret);
		return ret;
	}

	*data = ret;

	return 0;
}

static int cp_write_byte(struct sc8581_device *chip, u8 reg, u8 data)
{
	int ret = i2c_smbus_write_byte_data(chip->client, reg, data);

	if (ret < 0)
		mca_log_info("i2c write byte fail: can't write to reg 0x%02X, errcode=%d\n",
			     reg, ret);

	return ret;
}

static int cp_read_word(struct sc8581_device *chip, u8 reg, u16 *data)
{
	int ret = i2c_smbus_read_word_data(chip->client, reg);

	if (ret < 0) {
		mca_log_info("i2c read word fail: can't read from reg 0x%02X, errcode=%d\n",
			     reg, ret);
		return ret;
	}

	*data = ret;

	return 0;
}

/*
 * Read-modify-write.  A failed read is treated as 0x00 and the write still
 * goes out, so the field being set always ends up programmed.
 */
static int sc8581_update_bits(struct sc8581_device *chip, u8 reg, u8 mask,
			      u8 val)
{
	u8 data = 0;

	cp_read_byte(chip, reg, &data);
	data = (data & ~mask) | (val & mask);

	return cp_write_byte(chip, reg, data);
}

static int cp_read_i2c_block_data(struct sc8581_device *chip, u8 reg, u8 len,
				  u8 *data)
{
	int ret;

	if (!chip->client) {
		mca_log_err("null pointer read\n");
		return -EINVAL;
	}

	ret = i2c_smbus_read_i2c_block_data(chip->client, reg, len, data);
	if (ret < 0) {
		mca_log_err("I2C SMBus read failed, ret=%d\n", ret);
		return ret;
	}

	return 0;
}

static void sc8581_enable_charge(struct sc8581_device *chip, bool enable)
{
	sc8581_update_bits(chip, SC8581_REG_0B, SC8581_CHG_EN_MASK,
			   enable ? SC8581_CHG_EN_MASK : 0);
	mca_log_info("%s enable cp: %d\n", chip->log_tag, enable);
}

static void sc8581_enable_qb(struct sc8581_device *chip, bool enable)
{
	sc8581_update_bits(chip, SC8581_REG_0B, SC8581_QB_EN_MASK,
			   enable ? SC8581_QB_EN_MASK : 0);
	mca_log_info("%s enable qb: %d\n", chip->log_tag, enable);
}

static void sc8581_disable_rcp(struct sc8581_device *chip, bool disable)
{
	u8 val = disable ? SC8581_IBUS_RCP_DIS_MASK : 0;

	sc8581_update_bits(chip, SC8581_REG_40, SC8581_IBUS_RCP_DIS_MASK, val);
	mca_log_info("%s disable rcp: %d\n", chip->log_tag, val);
}

static void sc8581_set_key(struct sc8581_device *chip, bool enable)
{
	static const u8 key[] = { 'D', 'A', 'G', 'A', 'M', 'A' };
	u8 val = 0;
	int i, j;

	for (i = 0; i < 10; i++) {
		cp_read_byte(chip, SC8581_REG_KEY, &val);
		mca_log_info("%s register 0x76 = 0x%x\n", chip->log_tag, val);

		if ((val & 0x01) && enable) {
			mca_log_info("enter key mode\n");
			return;
		}
		if (!(val & 0x01) && !enable) {
			mca_log_info("exit key mode\n");
			return;
		}

		/* The same sequence toggles the key mode in and out. */
		for (j = 0; j < ARRAY_SIZE(key); j++)
			cp_write_byte(chip, SC8581_REG_KEY, key[j]);
	}
}

static void sc8581_set_vout2out_uvp(struct sc8581_device *chip)
{
	sc8581_set_key(chip, true);
	cp_write_byte(chip, SC8585_REG_VOUT2OUT_UVP, SC8585_VOUT2OUT_UVP_HIGH);
	sc8581_set_key(chip, false);
	mca_log_info("cp set vout2out uvp high: %d\n", 1);
}

static void sc8581_set_pmid2outuvp_th(struct sc8581_device *chip, int th)
{
	u8 val;

	if (chip->chip_vendor == SC8585_VENDOR) {
		val = th < 8 ? 0 : 5;
	} else {
		if (th < SC8581_PMID2OUT_UVP_MIN)
			th = SC8581_PMID2OUT_UVP_MIN;
		val = (th - SC8581_PMID2OUT_UVP_MIN) / SC8581_PMID2OUT_UVP_LSB;
	}

	sc8581_update_bits(chip, SC8581_REG_09, SC8581_PMID2OUT_UVP_MASK, val);

	if (chip->chip_vendor == SC8585_VENDOR &&
	    chip->support_reverse_quick_charge)
		sc8581_set_vout2out_uvp(chip);
}

static void sc8581_enable_busucp(struct sc8581_device *chip, bool enable)
{
	sc8581_update_bits(chip, SC8581_REG_07, SC8581_BUS_UCP_DIS_MASK,
			   enable ? 0 : SC8581_BUS_UCP_DIS_MASK);
	sc8581_update_bits(chip, SC8581_REG_07, SC8581_BUS_UCP_FALL_MASK_MASK,
			   enable ? 0 : SC8581_BUS_UCP_FALL_MASK_MASK);
	sc8581_update_bits(chip, SC8581_REG_07, SC8581_BUS_UCP_RISE_MASK_MASK,
			   enable ? 0 : SC8581_BUS_UCP_RISE_MASK_MASK);
}

static void sc8581_set_fsw(struct sc8581_device *chip, int fsw)
{
	int val = 0;

	if (fsw > chip->fsw_cfg.max)
		fsw = chip->fsw_cfg.max;
	else if (fsw < chip->fsw_cfg.min)
		fsw = chip->fsw_cfg.min;

	if (chip->fsw_cfg.step)
		val = (fsw - chip->fsw_cfg.min) / chip->fsw_cfg.step;

	mca_log_info("%s fsw: %d, val: %d\n", chip->log_tag, fsw, val);

	if (chip->chip_vendor == SC8585_VENDOR)
		sc8581_update_bits(chip, SC8581_REG_0C, SC8585_FSW_SET_MASK,
				   val << SC8585_FSW_SET_SHIFT);
	else
		sc8581_update_bits(chip, SC8581_REG_0C, SC8581_FSW_SET_MASK,
				   val << SC8581_FSW_SET_SHIFT);
}

static int sc8581_get_fsw(struct sc8581_device *chip, int *fsw)
{
	u8 data = 0;
	int ret, val;

	ret = cp_read_byte(chip, SC8581_REG_0C, &data);
	if (chip->chip_vendor == SC8585_VENDOR)
		val = (data & SC8585_FSW_SET_MASK) >> SC8585_FSW_SET_SHIFT;
	else
		val = (data & SC8581_FSW_SET_MASK) >> SC8581_FSW_SET_SHIFT;

	*fsw = chip->fsw_cfg.min + chip->fsw_cfg.step * val;
	mca_log_info("%s byte: %02x, val: %d, fsw: %d\n", chip->log_tag, data,
		     val, *fsw);

	return ret;
}

static void sc8581_set_busovp_th(struct sc8581_device *chip, int th)
{
	static const int min_mv[CP_MODE_DIV_MAX][2] = {
		{ SC8581_BUS_OVP_41MODE_MIN, SC8585_BUS_OVP_41MODE_MIN },
		{ SC8581_BUS_OVP_21MODE_MIN, SC8585_BUS_OVP_21MODE_MIN },
		{ SC8581_BUS_OVP_11MODE_MIN, SC8585_BUS_OVP_11MODE_MIN },
	};
	static const int max_mv[CP_MODE_DIV_MAX] = {
		SC85XX_BUS_OVP_41MODE_MAX,
		SC85XX_BUS_OVP_21MODE_MAX,
		SC85XX_BUS_OVP_11MODE_MAX,
	};
	int wm = chip->work_mode;
	int is_8585 = chip->chip_vendor == SC8585_VENDOR;
	int min = min_mv[wm][is_8585];
	int max = max_mv[wm];
	unsigned int idx = chip->chip_vendor - 1;
	int val = 0;

	mca_log_info("%s threshold= %d, min = %d, max =%d\n", chip->log_tag, th,
		     min, max);

	if (th < min)
		th = min;
	else if (th > max)
		th = max;

	if (idx >= ARRAY_SIZE(sc8581_bus_ovp_mask)) {
		mca_log_info("%s chip_vendor =%x no valid\n", chip->log_tag,
			     chip->chip_vendor);
		return;
	}

	if (sc8581_bus_ovp_lsb[wm][idx])
		val = (th - sc8581_bus_ovp_base[wm][idx]) /
		      sc8581_bus_ovp_lsb[wm][idx];

	mca_log_info("%s bus_ovpth= %d, val = %d\n", chip->log_tag, th,
		     (u8)val);
	sc8581_update_bits(chip, SC8581_REG_05, sc8581_bus_ovp_mask[idx],
			   val << SC8581_BUS_OVP_SHIFT);
}

/*
 * USB and WPC OVP share an encoding: 1 V steps from 11 V, with the top code
 * selecting the low 6.5 V (7.5 V on SC8585) setting used for 1:1 mode.
 */
static u8 sc8581_input_ovp_val(struct sc8581_device *chip, int th)
{
	int low = chip->chip_vendor == SC8585_VENDOR ?
		  SC8585_USB_OVP_7PV5_MV : SC8581_USB_OVP_6PV5_MV;

	if (th == low)
		return SC8581_USB_OVP_6PV5;

	return (th - SC8581_USB_OVP_BASE) / SC8581_USB_OVP_LSB;
}

static int sc8581_set_operation_mode(struct sc8581_device *chip, int mode)
{
	struct sc8581_cfg *cfg = &chip->cfg;
	int th, wm;

	if (mode < 0 || mode >= ARRAY_SIZE(sc8581_mode_reg)) {
		mca_log_info("%s operation mode error%d\n", chip->log_tag, mode);
		return -1;
	}

	wm = sc8581_work_mode[mode];
	chip->work_mode = wm;
	chip->operation_mode = sc8581_mode_reg[mode];

	/* Enable the protections. */
	sc8581_update_bits(chip, SC8581_REG_01, SC8581_BAT_OVP_DIS_MASK, 0);
	if (chip->chip_vendor != SC8585_VENDOR)
		sc8581_update_bits(chip, SC8581_REG_02, SC8581_BAT_OCP_DIS_MASK,
				   SC8581_BAT_OCP_DIS_MASK);
	sc8581_update_bits(chip, SC8581_REG_06, SC8581_BUS_OCP_DIS_MASK, 0);
	sc8581_enable_busucp(chip, true);
	sc8581_update_bits(chip, SC8581_REG_08, SC8581_PMID2OUT_OVP_DIS_MASK, 0);
	sc8581_update_bits(chip, SC8581_REG_09, SC8581_PMID2OUT_UVP_DIS_MASK, 0);
	sc8581_update_bits(chip, SC8581_REG_6C, SC8581_BAT_OVP_ALM_DIS_MASK, 0);
	sc8581_update_bits(chip, SC8581_REG_6D, SC8581_BUS_OCP_ALM_DIS_MASK, 0);

	/* Thresholds. */
	th = max_t(int, cfg->bat_ovp_th, SC8581_BAT_OVP_BASE);
	sc8581_update_bits(chip, SC8581_REG_01, SC8581_BAT_OVP_MASK,
			   (th - SC8581_BAT_OVP_BASE) / SC8581_BAT_OVP_LSB);

	if (chip->chip_vendor != SC8585_VENDOR)
		sc8581_update_bits(chip, SC8581_REG_02, SC8581_BAT_OCP_MASK,
				   (BAT_OCP_TH - SC8581_BAT_OCP_BASE) /
				   SC8581_BAT_OCP_LSB);

	th = max_t(int, cfg->bat_ovp_alarm_th, SC8581_BAT_OVP_ALM_BASE);
	sc8581_update_bits(chip, SC8581_REG_6C, SC8581_BAT_OVP_ALM_MASK,
			   (th - SC8581_BAT_OVP_ALM_BASE) / SC8581_BAT_OVP_ALM_LSB);

	sc8581_set_busovp_th(chip, cfg->bus_ovp_th[wm]);

	sc8581_update_bits(chip, SC8581_REG_03, SC8581_USB_OVP_MASK,
			   sc8581_input_ovp_val(chip, cfg->usb_ovp_th[wm]));

	th = cfg->bus_ocp_th[wm];
	if (chip->chip_vendor == SC8585_VENDOR) {
		th = max_t(int, th, SC8585_BUS_OCP_BASE);
		th = (th - SC8585_BUS_OCP_BASE) / SC8585_BUS_OCP_LSB;
	} else {
		th = max_t(int, th, SC8581_BUS_OCP_BASE);
		th = (th - SC8581_BUS_OCP_BASE) / SC8581_BUS_OCP_LSB;
	}
	sc8581_update_bits(chip, SC8581_REG_06, SC8581_BUS_OCP_MASK, th);

	sc8581_update_bits(chip, SC8581_REG_04, SC8581_WPC_OVP_MASK,
			   sc8581_input_ovp_val(chip, cfg->wpc_ovp_th));

	th = max_t(int, cfg->out_ovp_th, SC8581_OUT_OVP_BASE);
	sc8581_update_bits(chip, SC8581_REG_05, SC8581_OUT_OVP_MASK,
			   (th - SC8581_OUT_OVP_BASE) / SC8581_OUT_OVP_LSB);

	sc8581_set_pmid2outuvp_th(chip, cfg->pmid2out_uvp_th);

	th = max_t(int, cfg->pmid2out_ovp_th, SC8581_PMID2OUT_OVP_BASE);
	sc8581_update_bits(chip, SC8581_REG_08, SC8581_PMID2OUT_OVP_MASK,
			   (th - SC8581_PMID2OUT_OVP_BASE) / SC8581_PMID2OUT_OVP_LSB);

	mca_log_info("%s set operation mode %d reg %d work_mode %d\n",
		     chip->log_tag, mode, chip->operation_mode, chip->work_mode);

	sc8581_update_bits(chip, SC8581_REG_0E, SC8581_MODE_MASK,
			   sc8581_mode_reg[mode]);

	return 0;
}

static int sc8581_get_operation_mode(struct sc8581_device *chip, int *mode)
{
	u8 data;
	int ret;

	ret = cp_read_byte(chip, SC8581_REG_0E, &data);
	if (ret) {
		mca_log_info("%s get operation mode fail\n", chip->log_tag);
		return ret;
	}

	*mode = data & SC8581_MODE_MASK;

	return 0;
}

static void sc8581_enable_ovpgate(struct sc8581_device *chip, bool enable)
{
	u8 val = enable ? SC8581_OVPGATE_EN_MASK : 0;
	u8 data;
	int penable = 1;
	int count;

	chip->ovpgate_en = enable;

	/* Give the bus time to come back if it is still resuming. */
	if (!chip->i2c_is_working)
		msleep(30);

	sc8581_update_bits(chip, SC8581_REG_0B, SC8581_OVPGATE_EN_MASK, val);

	for (count = 0; count < 10; count++) {
		if (!cp_read_byte(chip, SC8581_REG_0B, &data)) {
			penable = !!(data & SC8581_OVPGATE_EN_MASK);
			if (penable == chip->ovpgate_en)
				break;
		}
		mca_log_info("%s count %d, ovpgate_en %d, penable %d\n",
			     chip->log_tag, count, chip->ovpgate_en, penable);
		sc8581_update_bits(chip, SC8581_REG_0B, SC8581_OVPGATE_EN_MASK,
				   val);
		msleep(10);
	}

	mca_log_err("%s enable %d\n", chip->log_tag, enable);
}

static int sc8581_get_ovpgate_enable(struct sc8581_device *chip, bool *enable)
{
	u8 data = 0;
	int ret;

	ret = cp_read_byte(chip, SC8581_REG_0B, &data);
	mca_log_info("%s ovpgate_status SC8581_REG_0B=0x%x\n", chip->log_tag,
		     data);
	if (ret) {
		mca_log_info("%s get ovpgate enable fail\n", chip->log_tag);
		return ret;
	}

	*enable = !!(data & SC8581_OVPGATE_EN_MASK);

	return 0;
}

static int sc8581_get_ovpgate_status(struct sc8581_device *chip, bool *status)
{
	u8 data = 0;
	int ret;

	ret = cp_read_byte(chip, SC8581_REG_0F, &data);
	mca_log_info("%s ovpgate_status SC8581_REG_0F=0x%x\n", chip->log_tag,
		     data);
	if (ret) {
		mca_log_info("%s get ovpgate status fail\n", chip->log_tag);
		return ret;
	}

	*status = !!(data & SC8581_OVPGATE_STAT_MASK);

	return 0;
}

static int sc8581_get_adc_data(struct sc8581_device *chip, int channel,
			       u32 *result)
{
	u8 reg = 0;
	u16 raw;
	u32 val;
	int ret;

	mca_log_info("adc channel = %d", channel);
	cp_read_byte(chip, SC8581_REG_15, &reg);
	mca_log_info("SC8581_REG_15 = %d", reg);
	cp_read_byte(chip, SC8581_REG_16, &reg);
	mca_log_info("SC8581_REG_16 = %d", reg);

	ret = cp_read_word(chip, SC8581_REG_17 + channel * 2, &raw);
	if (ret) {
		mca_log_info("adc read error");
		return ret;
	}

	/* The ADC registers are big-endian. */
	val = swab16(raw);

	switch (channel) {
	case ADC_IBUS:
		if (chip->chip_vendor == SC8585_VENDOR)
			val = val * SC8585_IBUS_ADC_LSB / SC8585_CURRENT_SCALE;
		else
			val = val * SC8581_IBUS_ADC_LSB / SC8581_CURRENT_SCALE;
		break;
	case ADC_VBUS:
	case ADC_VUSB:
	case ADC_VWPC:
		val = val * SC8581_VBUS_ADC_LSB / SC8581_VOLTAGE_SCALE;
		break;
	case ADC_VOUT:
	case ADC_VBAT:
		val = val * SC8581_VBAT_ADC_LSB / SC8581_VOLTAGE_SCALE;
		break;
	case ADC_IBAT:
		val = val * SC8581_IBAT_ADC_LSB / SC8581_IBAT_SCALE;
		break;
	case ADC_TBAT:
		val = val * SC8581_TSBAT_ADC_LSB / SC8581_TEMP_SCALE;
		break;
	case ADC_TDIE:
		val = val / 2;
		break;
	default:
		break;
	}

	/* The result is a 16-bit quantity. */
	*result = (u16)val;
	mca_log_debug("%s channel %d adc %x, result: %d\n", chip->log_tag,
		      channel, raw, *result);

	return 0;
}

static int cp_get_adc_data(struct sc8581_device *chip, int channel,
			   u32 *result)
{
	int ret = sc8581_get_adc_data(chip, channel, result);

	if (ret)
		mca_log_info(" %s failed get ADC value\n", chip->log_tag);

	return ret;
}

static int sc8581_get_tdie(struct sc8581_device *chip, int *tdie)
{
	u8 byte_h = 0, byte_l = 0;
	int ret_h, ret_l;

	*tdie = 0;

	/* Only the SC8585 has a usable die temperature reading here. */
	if (chip->chip_vendor == SC8585_VENDOR) {
		ret_h = cp_read_byte(chip, SC8581_REG_27, &byte_h);
		ret_l = cp_read_byte(chip, SC8581_REG_28, &byte_l);
		if (ret_h || ret_l) {
			mca_log_err("read tdie adc failed, ret=%d\n",
				    ret_h | ret_l);
			return 0;
		}
	}

	*tdie = byte_l * SC8581_TDIE_ADC_LSB;
	mca_log_info("%s byte_h: %02x, byte_l: %02x, val: %d, tdie: %d\n",
		     chip->log_tag, byte_h, byte_l, byte_l, *tdie);

	return 0;
}

/*
 * The pump stopped switching: look at what it latched and tell the
 * strategies which protection fired.
 */
static void sc8581_abnormal_charging_judge(struct sc8581_device *chip,
					   const u8 *regs_00, u8 reg_10,
					   u8 reg_13)
{
	static int cp_cboot_short;
	u8 reg_0a = regs_00[SC8581_REG_0A];
	int tdie[2] = { 0 };

	if (!(reg_10 & SC_VOUT_INSERT)) {
		mca_log_info("VOUT UVLO\n");
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_VOUT_UVLO, NULL);
	}

	if ((reg_10 & 0x3f) != 0x3f)
		mca_log_info("%s VIN have problem\n", chip->log_tag);
	if (reg_0a & SC8581_VBUS_ERRORHI_STAT_MASK)
		mca_log_info("%s VBUS_ERRORHI_STAT\n", chip->log_tag);
	if (reg_0a & SC8581_VBUS_ERRORLO_STAT_MASK)
		mca_log_info("%sVBUS_ERRORLO_STAT\n", chip->log_tag);

	/* Report each change of the CBOOT pin diagnostic once. */
	if (reg_0a & SC8581_PIN_DIAG_FALL_FLAG_MASK) {
		mca_log_info("%s CBOOT SHORT/OPEN 111\n", chip->log_tag);
		if (!cp_cboot_short) {
			cp_cboot_short = 1;
			mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
					       MCA_EVENT_CP_CBOOT_FAIL,
					       &cp_cboot_short);
		}
	} else if (cp_cboot_short) {
		mca_log_info("%s CBOOT SHORT/OPEN 000\n", chip->log_tag);
		cp_cboot_short = 0;
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_CBOOT_FAIL, &cp_cboot_short);
	}

	if (regs_00[SC8581_REG_01] & SC8581_BAT_OVP_FLAG_MASK) {
		mca_log_info("%s VBAT_OVP\n", chip->log_tag);
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_VBAT_OVP, NULL);
	}
	if ((regs_00[SC8581_REG_02] & SC8581_BAT_OCP_FLAG_MASK) &&
	    chip->chip_vendor != SC8585_VENDOR) {
		mca_log_info("%s IBAT_OCP\n", chip->log_tag);
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_IBAT_OCP, NULL);
	}
	if (regs_00[SC8581_REG_03] & SC8581_USB_OVP_FLAG_MASK) {
		mca_log_info("%s VUSB_OVP\n", chip->log_tag);
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_VUSB_OVP, NULL);
	}
	if (regs_00[SC8581_REG_04] & SC8581_WPC_OVP_FLAG_MASK) {
		mca_log_info("%s VWPC_OVP\n", chip->log_tag);
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_VWPC_OVP, NULL);
	}
	if (regs_00[SC8581_REG_06] & SC8581_BUS_OCP_FLAG_MASK) {
		mca_log_info("%s IBUS_OCP\n", chip->log_tag);
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_IBUS_OCP, NULL);
	}
	if (regs_00[SC8581_REG_07] & SC8581_BUS_UCP_FALL_FLAG_MASK) {
		mca_log_info("%s IBUS_UCP\n", chip->log_tag);
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_IBUS_UCP, NULL);
	}
	if (regs_00[SC8581_REG_08] & SC8581_PMID2OUT_OVP_FLAG_MASK) {
		mca_log_info("%s PMID2OUT_OVP\n", chip->log_tag);
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_PMID2OUT_OVP, NULL);
	}
	if (regs_00[SC8581_REG_09] & SC8581_PMID2OUT_UVP_FLAG_MASK) {
		mca_log_info("%s PMID2OUT_UVP\n", chip->log_tag);
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_PMID2OUT_UVP, NULL);
	}
	if (reg_0a & SC8581_POR_FLAG_MASK) {
		mca_log_info("%s POR_FLAG\n", chip->log_tag);
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_POR_FLAG, NULL);
	}
	if (reg_13 & SC8581_TSHUT_FLAG_MASK) {
		mca_log_info("%s TSHUT\n", chip->log_tag);
		/* The listener reads two ints: master and slave die temp. */
		cp_get_adc_data(chip, ADC_TDIE, (u32 *)&tdie[0]);
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_TSHUT_FLAG, tdie);
	}
	if (reg_13 & SC8581_VBUS_OVP_FLAG_MASK) {
		mca_log_info("%s vbus ovp\n", chip->log_tag);
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_VBUS_OVP, NULL);
	}
}

static void sc8581_log_regs8(struct sc8581_device *chip, u8 base, const u8 *d)
{
	mca_log_info("%s: [0x%02X]=0x%02X, [0x%02X]=0x%02X, [0x%02X]=0x%02X, [0x%02X]=0x%02X, [0x%02X]=0x%02X, [0x%02X]=0x%02X, [0x%02X]=0x%02X, [0x%02X]=0x%02X",
		     chip->log_tag, base, d[0], base + 1, d[1], base + 2, d[2],
		     base + 3, d[3], base + 4, d[4], base + 5, d[5],
		     base + 6, d[6], base + 7, d[7]);
}

static void sc8581_dump_important_regs(struct sc8581_device *chip)
{
	u8 regs_00[16] = { 0 };
	u8 regs_10[16] = { 0 };
	u8 regs_6e[4] = { 0 };
	u8 reg_10 = 0, reg_13 = 0;

	if (cp_read_i2c_block_data(chip, SC8581_REG_00, sizeof(regs_00),
				   regs_00)) {
		mca_log_err("dump registers failed, base_addr: 0x%02X\n",
			    SC8581_REG_00);
		memset(regs_00, 0, sizeof(regs_00));
	} else {
		sc8581_log_regs8(chip, SC8581_REG_00, &regs_00[0]);
		sc8581_log_regs8(chip, SC8581_REG_08, &regs_00[8]);
	}

	/* The gate did not follow the last request: insist. */
	if (!!(regs_00[SC8581_REG_0B] & SC8581_OVPGATE_EN_MASK) !=
	    chip->ovpgate_en) {
		mca_log_info("set cp ovpgate not effective, repeat set ovpgate\n");
		sc8581_enable_ovpgate(chip, chip->ovpgate_en);
	}

	if (cp_read_i2c_block_data(chip, SC8581_REG_10, sizeof(regs_10),
				   regs_10)) {
		mca_log_err("dump registers failed, base_addr: 0x%02X\n",
			    SC8581_REG_10);
	} else {
		sc8581_log_regs8(chip, SC8581_REG_10, &regs_10[0]);
		sc8581_log_regs8(chip, SC8581_REG_18, &regs_10[8]);
		reg_10 = regs_10[0];
		reg_13 = regs_10[3];
	}

	if (cp_read_i2c_block_data(chip, SC8581_REG_6E, sizeof(regs_6e),
				   regs_6e))
		mca_log_err("dump registers failed, base_addr: 0x%02X\n",
			    SC8581_REG_6E);
	else
		mca_log_info("%s: [0x%02X]=0x%02X, [0x%02X]=0x%02X, [0x%02X]=0x%02X, [0x%02X]=0x%02X",
			     chip->log_tag, SC8581_REG_6E, regs_6e[0],
			     SC8581_REG_6E + 1, regs_6e[1],
			     SC8581_REG_70, regs_6e[2],
			     SC8581_REG_70 + 1, regs_6e[3]);

	if (regs_00[SC8581_REG_0A] & SC8581_CP_SWITCHING_STAT_MASK)
		return;

	mca_log_info("%s cp switching stop, enter abnormal charging judge\n",
		     chip->log_tag);
	sc8581_abnormal_charging_judge(chip, regs_00, reg_10, reg_13);
}

/* Read back the alarm configuration registers; a failure aborts init. */
static int sc8581_init_int_src(struct sc8581_device *chip)
{
	u8 data;
	int ret;

	ret = cp_read_byte(chip, SC8581_REG_6C, &data);
	if (!ret)
		ret = cp_write_byte(chip, SC8581_REG_6C, data);
	if (ret) {
		mca_log_info(" %s failed to set alarm mask:%d\n", chip->log_tag,
			     ret);
		return ret;
	}

	ret = cp_read_byte(chip, SC8581_REG_6D, &data);
	if (!ret)
		ret = cp_write_byte(chip, SC8581_REG_6D, data);
	if (ret) {
		mca_log_info("%s failed to set alarm mask:%d\n", chip->log_tag,
			     ret);
		return ret;
	}

	return 0;
}

static int sc8581_init_device(struct sc8581_device *chip)
{
	int ret = 0;
	int retry;

	sc8581_update_bits(chip, SC8581_REG_0E, SC8581_REG_RST_MASK,
			   SC8581_REG_RST_MASK);
	sc8581_update_bits(chip, SC8581_REG_0E, SC8581_SYNC_FUNCTION_EN_MASK, 0);

	/*
	 * Errors accumulate across attempts, so a failed first attempt is
	 * retried but still reported.
	 */
	for (retry = 0; retry < ERROR_RECOVERY_COUNT; retry++) {
		/* ACDRV manual, WPC gate and OVP gate on. */
		sc8581_update_bits(chip, SC8581_REG_0B,
				   SC8581_ACDRV_MANUAL_EN_MASK |
				   SC8581_WPCGATE_EN_MASK |
				   SC8581_OVPGATE_EN_MASK,
				   SC8581_ACDRV_MANUAL_EN_MASK |
				   SC8581_WPCGATE_EN_MASK |
				   SC8581_OVPGATE_EN_MASK);
		sc8581_update_bits(chip, SC8581_REG_0D,
				   SC8581_SS_TIMEOUT_SET_MASK,
				   SC8581_SS_TIMEOUT_5120MS <<
				   SC8581_SS_TIMEOUT_SET_SHIFT);
		sc8581_update_bits(chip, SC8581_REG_07,
				   SC8581_BUS_UCP_FALL_DG_MASK,
				   SC8581_BUS_UCP_FALL_DG_5MS <<
				   SC8581_BUS_UCP_FALL_DG_SHIFT);

		if (chip->chip_vendor != SC8585_VENDOR) {
			sc8581_update_bits(chip, SC8581_REG_7C,
					   SC8581_ACDRV_UP_MASK,
					   SC8581_ACDRV_UP_MASK);
			sc8581_update_bits(chip, SC8581_REG_0E,
					   SC8581_IBAT_SNS_RES_MASK, 0);
			sc8581_update_bits(chip, SC8581_REG_70,
					   SC8581_TSBAT_EN_MASK, 0);
		}

		sc8581_update_bits(chip, SC8581_REG_0C, SC8581_SYNC_MASK,
				   SC8581_SYNC_NO_SHIFT);
		sc8581_update_bits(chip, SC8581_REG_03,
				   SC8581_OVPGATE_ON_DG_MASK, 0);

		/* ADC: continuous, IBUS and every channel enabled, then off. */
		sc8581_update_bits(chip, SC8581_REG_15, SC8581_ADC_RATE_MASK, 0);
		sc8581_update_bits(chip, SC8581_REG_15,
				   SC8581_IBUS_ADC_DIS_MASK, 0);
		sc8581_update_bits(chip, SC8581_REG_16,
				   SC8581_VBUS_ADC_DIS_MASK, 0);
		sc8581_update_bits(chip, SC8581_REG_16,
				   SC8581_VUSB_ADC_DIS_MASK, 0);
		sc8581_update_bits(chip, SC8581_REG_16,
				   SC8581_VWPC_ADC_DIS_MASK, 0);
		sc8581_update_bits(chip, SC8581_REG_16,
				   SC8581_VOUT_ADC_DIS_MASK, 0);
		sc8581_update_bits(chip, SC8581_REG_16,
				   SC8581_VBAT_ADC_DIS_MASK, 0);
		if (chip->chip_vendor != SC8585_VENDOR) {
			sc8581_update_bits(chip, SC8581_REG_16,
					   SC8581_IBAT_ADC_DIS_MASK, 0);
			sc8581_update_bits(chip, SC8581_REG_16,
					   SC8581_TSBAT_ADC_DIS_MASK, 0);
		}
		sc8581_update_bits(chip, SC8581_REG_16,
				   SC8581_TDIE_ADC_DIS_MASK, 0);
		sc8581_update_bits(chip, SC8581_REG_15, SC8581_ADC_EN_MASK, 0);

		ret |= sc8581_init_int_src(chip);
		ret |= sc8581_set_operation_mode(chip, CP_MODE_FORWARD_2_1);

		if (chip->chip_vendor == SC8581_VENDOR) {
			sc8581_update_bits(chip, SC8581_REG_0F,
					   SC8581_TSBAT_FLT_DIS_MASK,
					   SC8581_TSBAT_FLT_DIS_MASK);
			sc8581_update_bits(chip, SC8581_REG_7C,
					   SC8581_PIN_DIAG_DIS_MASK,
					   SC8581_PIN_DIAG_DIS_MASK);
		}

		if (chip->chip_vendor == SC8585_VENDOR) {
			chip->fsw_cfg.max = SC8585_FSW_MAX;
			chip->fsw_cfg.min = SC8585_FSW_MIN;
			chip->fsw_cfg.step = SC8585_FSW_STEP;
			cp_write_byte(chip, SC8581_REG_42, SC8585_ACDRV_INIT);
		} else {
			chip->fsw_cfg.max = SC8581_FSW_MAX;
			chip->fsw_cfg.min = SC8581_FSW_MIN;
			chip->fsw_cfg.step = SC8581_FSW_STEP;
		}

		sc8581_set_fsw(chip, CP_DEFAULT_FSW);

		if (ret >= 0) {
			mca_log_err("%s success to init CP device\n",
				    chip->log_tag);
			break;
		}
	}

	return ret;
}

static int ops_cp_enable_charge(bool enable, void *data)
{
	sc8581_enable_charge(data, enable);

	return 0;
}

static int ops_cp_get_charge_enable(bool *enable, void *data)
{
	struct sc8581_device *chip = data;
	u8 val;
	int ret;

	ret = cp_read_byte(chip, SC8581_REG_0B, &val);
	if (ret) {
		mca_log_err("%s failed get enable cp charge status ret =%d\n",
			    chip->log_tag, ret);
		return ret;
	}

	*enable = !!(val & SC8581_CHG_EN_MASK);

	return 0;
}

static int ops_cp_get_present(bool *present, void *data)
{
	struct sc8581_device *chip = data;

	*present = chip->chip_ok;

	return 0;
}

static int ops_cp_get_vbatt(u32 *vbatt, void *data)
{
	return cp_get_adc_data(data, ADC_VBAT, vbatt);
}

static int ops_cp_get_ibatt(u32 *ibatt, void *data)
{
	return cp_get_adc_data(data, ADC_IBAT, ibatt);
}

static int ops_cp_get_battery_temmperature(u32 *temp, void *data)
{
	return cp_get_adc_data(data, ADC_TBAT, temp);
}

static int ops_cp_get_battery_present(bool *present, void *data)
{
	struct sc8581_device *chip = data;
	u8 val;
	int ret;

	ret = cp_read_byte(chip, SC8581_REG_10, &val);
	if (ret) {
		*present = false;
		return ret;
	}

	*present = !!(val & SC8581_VOUT_INSERT_STAT_MASK);

	return 0;
}

static int ops_cp_get_vbus(u32 *vbus, void *data)
{
	return cp_get_adc_data(data, ADC_VBUS, vbus);
}

static int ops_cp_get_ibus(u32 *ibus, void *data)
{
	return cp_get_adc_data(data, ADC_IBUS, ibus);
}

static int ops_cp_enable_wpcgate(bool enable, void *data)
{
	sc8581_update_bits(data, SC8581_REG_0B, SC8581_WPCGATE_EN_MASK,
			   enable ? SC8581_WPCGATE_EN_MASK : 0);

	return 0;
}

static int ops_cp_enable_ovpgate(bool enable, void *data)
{
	sc8581_enable_ovpgate(data, enable);

	return 0;
}

/*
 * The OVP gate is shared between OTG, reverse charging and wireless: it is
 * closed as soon as one user asks, and reopened once none of them holds it
 * closed any more.  The state is shared by every pump instance.
 */
static int ops_cp_enable_ovpgate_with_check(int type, bool enable, void *data)
{
	static unsigned int ovp_type;
	static bool ovp_enable_flag = true;
	unsigned int bit = 1U << (type & 0x1f);

	if (enable) {
		ovp_type &= ~bit;
		if (!!ovp_type == !ovp_enable_flag)
			return 0;
	} else {
		ovp_type |= bit;
		if (!ovp_enable_flag)
			return 0;
	}

	ovp_enable_flag = enable;
	sc8581_enable_ovpgate(data, enable);

	return 0;
}

static int ops_cp_get_ovpgate_status(bool *status, void *data)
{
	struct sc8581_device *chip = data;
	int ret;

	ret = sc8581_get_ovpgate_status(chip, status);
	if (ret)
		mca_log_info("%s failed get cp ovpgate status\n", chip->log_tag);

	return ret;
}

static int ops_cp_get_vusb(u32 *vusb, void *data)
{
	return cp_get_adc_data(data, ADC_VUSB, vusb);
}

static int ops_cp_set_mode(int mode, void *data)
{
	struct sc8581_device *chip = data;
	int ret;

	ret = sc8581_set_operation_mode(chip, mode);
	if (ret)
		mca_log_err("%s failed set cp charge mode\n", chip->log_tag);

	return ret;
}

static int ops_cp_get_mode(int *mode, void *data)
{
	struct sc8581_device *chip = data;
	int ret;

	ret = sc8581_get_operation_mode(chip, mode);
	if (ret)
		mca_log_err("%s failed to get div_mode\n", chip->log_tag);

	return ret;
}

static int ops_cp_device_init(int value, void *data)
{
	struct sc8581_device *chip = data;
	int ret;

	ret = sc8581_init_device(chip);
	if (ret)
		mca_log_err("%s failed init cp init device\n", chip->log_tag);

	return ret;
}

static int ops_cp_enable_adc(bool enable, void *data)
{
	sc8581_update_bits(data, SC8581_REG_15, SC8581_ADC_EN_MASK,
			   enable ? SC8581_ADC_EN_MASK : 0);

	return 0;
}

static int ops_cp_get_bypass_support(bool *support, void *data)
{
	*support = true;
	mca_log_info("%s %d\n", __func__, *support);

	return 0;
}

static int ops_cp_dump_register(void *data)
{
	sc8581_dump_important_regs(data);

	return 0;
}

static int ops_cp_get_chip_vendor(int *vendor, void *data)
{
	struct sc8581_device *chip = data;

	*vendor = chip->chip_vendor;
	mca_log_info("%s %d\n", chip->log_tag, *vendor);

	return 0;
}

static int ops_enable_acdrv_manual(bool enable, void *data)
{
	sc8581_update_bits(data, SC8581_REG_0B, SC8581_ACDRV_MANUAL_EN_MASK,
			   enable ? SC8581_ACDRV_MANUAL_EN_MASK : 0);

	return 0;
}

static int ops_cp_set_adjustadble_timeout(int timeout, void *data)
{
	u8 val;

	cp_read_byte(data, SC8581_REG_0D, &val);
	cp_write_byte(data, SC8581_REG_0D, timeout);

	return 0;
}

static int ops_cp_get_int_stat(int stat, bool *val, void *data)
{
	u8 data_10 = 0;
	int ret;

	ret = cp_read_byte(data, SC8581_REG_10, &data_10);

	switch (stat) {
	case VOUT_OK_REV_STAT:
		*val = !!(data_10 & SC8581_VOUT_OK_REV_STAT_MASK);
		break;
	case VOUT_OK_CHG_STAT:
		*val = !!(data_10 & SC8581_VOUT_OK_CHG_STAT_MASK);
		break;
	case VOUT_INSERT_STAT:
		*val = !!(data_10 & SC8581_VOUT_INSERT_STAT_MASK);
		break;
	case VBUS_PRESENT_STAT:
		*val = !!(data_10 & SC8581_VBUS_PRESENT_STAT_MASK);
		break;
	case VWPC_PRESENT_STAT:
		*val = !!(data_10 & SC8581_VWPC_INSERT_STAT_MASK);
		break;
	case VUSB_PRESENT_STAT:
		*val = !!(data_10 & SC8581_VUSB_INSERT_STAT_MASK);
		break;
	default:
		*val = false;
		break;
	}

	if (ret)
		mca_log_info("failed get int stat %d\n", stat);

	return ret;
}

static int ops_cp_get_errorhl_stat(int *stat, void *data)
{
	struct sc8581_device *chip = data;
	u8 val;
	int ret;

	ret = cp_read_byte(chip, SC8581_REG_0A, &val);
	if (ret) {
		mca_log_err("%s get pmid error stat fail\n", chip->log_tag);
		return ret;
	}

	if (val & SC8581_VBUS_ERRORLO_STAT_MASK)
		*stat = CP_PMID_ERROR_LOW;
	else if (val & SC8581_VBUS_ERRORHI_STAT_MASK)
		*stat = CP_PMID_ERROR_HIGH;
	else
		*stat = CP_PMID_ERROR_OK;

	mca_log_info("%s val: 0x%02x, stat: %d", chip->log_tag, val, *stat);

	return 0;
}

static int ops_cp_enable_busucp(bool enable, void *data)
{
	sc8581_enable_busucp(data, enable);

	return 0;
}

static int ops_cp_set_fsw(int fsw, void *data)
{
	sc8581_set_fsw(data, fsw);

	return 0;
}

static int ops_cp_set_default_fsw(void *data)
{
	sc8581_set_fsw(data, CP_DEFAULT_FSW);

	return 0;
}

static int ops_cp_get_fsw(int *fsw, void *data)
{
	return sc8581_get_fsw(data, fsw);
}

static int ops_cp_get_fsw_step(int *step, void *data)
{
	struct sc8581_device *chip = data;

	*step = chip ? chip->fsw_cfg.step : CP_DEFAULT_FSW;

	return 0;
}

static int ops_cp_get_tdie(int *tdie, void *data)
{
	return sc8581_get_tdie(data, tdie);
}

static int ops_cp_enable_qb(bool enable, void *data)
{
	sc8581_enable_qb(data, enable);

	return 0;
}

static int ops_cp_set_pmid2outuvp_th(int th, void *data)
{
	sc8581_set_pmid2outuvp_th(data, th);

	return 0;
}

static int ops_cp_set_rcp(bool enable, void *data)
{
	sc8581_disable_rcp(data, enable);

	return 0;
}

/*
 * Reverse 1:2 with the OVP gate on, or forward 2:1 with switching off; the
 * pump is asked until it reports the requested state.
 */
static int sc8581_set_revchg(struct sc8581_device *chip, bool enable)
{
	bool gate = false;
	u8 val = 0;
	int mode = 0;

	if (enable) {
		sc8581_set_operation_mode(chip, CP_MODE_REVERSE_1_2);
		sc8581_enable_ovpgate(chip, true);
		sc8581_get_ovpgate_enable(chip, &gate);
		sc8581_get_operation_mode(chip, &mode);
		mca_log_info("cp mode: %d, ovpgate_enable status: %d", mode,
			     gate);

		return gate && mode == SC8581_REVERSE_1_2_CONVERTER_MODE ?
		       0 : -EAGAIN;
	}

	sc8581_enable_charge(chip, false);
	sc8581_enable_qb(chip, false);
	sc8581_set_operation_mode(chip, CP_MODE_FORWARD_2_1);
	sc8581_get_operation_mode(chip, &mode);
	cp_read_byte(chip, SC8581_REG_0B, &val);
	val = !!(val & SC8581_CHG_EN_MASK);
	mca_log_info("cp mode: %d, charging_enable: %d", mode, val);

	return mode == SC8581_FORWARD_2_1_CHARGER_MODE && !val ? 0 : -EAGAIN;
}

static int ops_cp_set_revchg(bool enable, void *data)
{
	struct sc8581_device *chip = data;
	int retry;

	for (retry = 0; retry < 10; retry++) {
		if (!sc8581_set_revchg(chip, enable))
			return 0;
		mca_log_info("%s failed set revchg, retry: %d\n", chip->log_tag,
			     retry);
		mdelay(20);
	}

	return -1;
}

static const struct platform_class_cp_ops sc8581_chg_ops = {
	.cp_set_enable = ops_cp_enable_charge,
	.cp_get_enabled = ops_cp_get_charge_enable,
	.cp_get_present = ops_cp_get_present,
	.cp_get_battery_voltage = ops_cp_get_vbatt,
	.cp_get_battery_current = ops_cp_get_ibatt,
	.cp_get_battery_temperature = ops_cp_get_battery_temmperature,
	.cp_get_battery_present = ops_cp_get_battery_present,
	.cp_get_bus_voltage = ops_cp_get_vbus,
	.cp_get_bus_current = ops_cp_get_ibus,
	.cp_enable_wpcgate = ops_cp_enable_wpcgate,
	.cp_enable_ovpgate = ops_cp_enable_ovpgate,
	.cp_enable_ovpgate_with_check = ops_cp_enable_ovpgate_with_check,
	.cp_get_ovpgate_status = ops_cp_get_ovpgate_status,
	.cp_get_usb_voltage = ops_cp_get_vusb,
	.cp_set_mode = ops_cp_set_mode,
	.cp_get_mode = ops_cp_get_mode,
	.cp_device_init = ops_cp_device_init,
	.cp_enable_adc = ops_cp_enable_adc,
	.cp_get_bypass_support = ops_cp_get_bypass_support,
	.cp_dump_register = ops_cp_dump_register,
	.cp_get_chip_vendor = ops_cp_get_chip_vendor,
	.cp_enable_acdrv_manual = ops_enable_acdrv_manual,
	.cp_set_adjustadble_timeout = ops_cp_set_adjustadble_timeout,
	.cp_get_int_stat = ops_cp_get_int_stat,
	.cp_get_errorhl_stat = ops_cp_get_errorhl_stat,
	.cp_enable_busucp = ops_cp_enable_busucp,
	.cp_set_fsw = ops_cp_set_fsw,
	.cp_set_default_fsw = ops_cp_set_default_fsw,
	.cp_get_fsw = ops_cp_get_fsw,
	.cp_get_fsw_step = ops_cp_get_fsw_step,
	.cp_get_tdie = ops_cp_get_tdie,
	.cp_set_qb = ops_cp_enable_qb,
	.cp_set_pmid2outuvp_th = ops_cp_set_pmid2outuvp_th,
	.cp_set_rcp = ops_cp_set_rcp,
	.cp_set_revchg = ops_cp_set_revchg,
};

static int sc8581_dump_log_head(void *data, char *buf, int size)
{
	struct sc8581_device *chip = data;

	if (!chip)
		return 0;

	if (chip->cp_role == SC8581_ROLE_MASTER)
		return snprintf(buf, size,
				"cp_vusb cp_vbus cp_ibus cp_ibat cp_vbat cp_vout ");

	return snprintf(buf, size,
			"cp_vusb1 cp_vbus1 cp_ibus1 cp_ibat1 cp_vbat1 cp_vout1 ");
}

static int sc8581_dump_log_context(void *data, char *buf, int size)
{
	struct sc8581_device *chip = data;
	u32 vusb = 0, vbus = 0, ibus = 0, ibat = 0, vbat = 0, vout = 0;

	if (!chip)
		return snprintf(buf, size, "%-8d%-8d%-8d%-8d%-8d%-8d",
				-1, -1, -1, -1, -1, -1);

	cp_get_adc_data(chip, ADC_VBUS, &vbus);
	cp_get_adc_data(chip, ADC_VUSB, &vusb);
	cp_get_adc_data(chip, ADC_IBUS, &ibus);
	cp_get_adc_data(chip, ADC_IBAT, &ibat);
	cp_get_adc_data(chip, ADC_VBAT, &vbat);
	cp_get_adc_data(chip, ADC_VOUT, &vout);

	if (chip->cp_role == SC8581_ROLE_MASTER)
		return snprintf(buf, size, "%-8u%-8u%-8u%-8u%-8u%-8u",
				vusb, vbus, ibus, ibat, vbat, vout);

	return snprintf(buf, size, "%-9u%-9u%-9u%-9u%-9u%-9u",
			vusb, vbus, ibus, ibat, vbat, vout);
}

static struct mca_log_charge_log_ops g_sc8581_log_ops = {
	.dump_log_head = sc8581_dump_log_head,
	.dump_log_context = sc8581_dump_log_context,
};

static void sc8581_irq_handler(struct work_struct *work)
{
	static bool last_usb_present;
	struct sc8581_device *chip = container_of(work, struct sc8581_device,
						  irq_handle_work.work);
	bool usb_present;
	u8 val = 0;

	cp_read_byte(chip, SC8581_REG_10, &val);
	usb_present = !!(val & SC8581_VUSB_INSERT_STAT_MASK);
	if (usb_present != last_usb_present) {
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       usb_present ? MCA_EVENT_CP_VUSB_INSERT :
						     MCA_EVENT_CP_VUSB_OUT,
				       NULL);
		last_usb_present = usb_present;
	}

	mca_log_info("%s handler\n", chip->log_tag);

	if (chip->i2c_is_working)
		sc8581_dump_important_regs(chip);
}

static irqreturn_t sc8581_int_isr(int irq, void *data)
{
	struct sc8581_device *chip = data;

	mca_log_info("%s %s\n", chip->log_tag, __func__);
	pm_wakeup_dev_event(chip->dev, 500, true);
	queue_delayed_work(system_wq, &chip->irq_handle_work, 0);

	return IRQ_HANDLED;
}

static ssize_t cp_debugfs_show(void *data, char *buf)
{
	struct mca_debugfs_attr_data *d = data;
	struct sc8581_device *chip;
	u8 val = 0;
	ssize_t len = 0;
	int i;

	if (!d || !d->attr_info || !d->private) {
		mca_log_err("null pointer show\n");
		return 0;
	}

	chip = d->private;

	switch (d->attr_info->debugfs_attr_name) {
	case CP_DEBUGFS_ADDRESS:
		return scnprintf(buf, PAGE_SIZE, "%02x\n", reg_info.address);
	case CP_DEBUGFS_COUNT:
		return scnprintf(buf, PAGE_SIZE, "%x\n", reg_info.count);
	case CP_DEBUGFS_DATA:
		for (i = 0; i < reg_info.count; i++) {
			cp_read_byte(chip, reg_info.address + i, &val);
			len += scnprintf(buf + len, PAGE_SIZE - len,
					 "%02x: %02x\n", reg_info.address + i,
					 val);
		}
		return len;
	default:
		return 0;
	}
}

static ssize_t cp_debugfs_store(void *data, const char *buf, size_t count)
{
	struct mca_debugfs_attr_data *d = data;
	struct sc8581_device *chip;
	int val = 0;

	if (!d || !d->attr_info || !d->private) {
		mca_log_err("null pointer store\n");
		return count;
	}

	chip = d->private;

	if (kstrtoint(buf, 16, &val))
		return -EINVAL;

	switch (d->attr_info->debugfs_attr_name) {
	case CP_DEBUGFS_ADDRESS:
		reg_info.address = val;
		break;
	case CP_DEBUGFS_COUNT:
		if (val > CP_DEBUGFS_MAX_COUNT)
			reg_info.count = CP_DEBUGFS_MAX_COUNT;
		else if (reg_info.count > 0)
			reg_info.count = val;
		else
			reg_info.count = 1;
		break;
	case CP_DEBUGFS_DATA:
		cp_write_byte(chip, reg_info.address, val);
		break;
	default:
		break;
	}

	return count;
}

static struct mca_debugfs_attr_info cp_debugfs_field_tbl[] = {
	mca_debugfs_attr(cp_debugfs, 0664, CP_DEBUGFS_ADDRESS, address),
	mca_debugfs_attr(cp_debugfs, 0664, CP_DEBUGFS_COUNT, count),
	mca_debugfs_attr(cp_debugfs, 0600, CP_DEBUGFS_DATA, data),
};

static bool mca_cp_is_en_buck_project_hwid(void)
{
	struct mca_hwid_info *hwid = mca_get_hwid_info();

	if (!hwid) {
		mca_log_err("get hwid info failed\n");
		return false;
	}

	return hwid->platform_version == EN_BUCK_PLATFORM_VERSION &&
	       hwid->hwid_value == EN_BUCK_HWID_VALUE;
}

static int sc8581_parse_dt(struct sc8581_device *chip)
{
	struct device_node *np = chip->dev->of_node;
	struct sc8581_cfg *cfg = &chip->cfg;

	if (!np) {
		mca_log_err("device tree info missing\n");
		return -ENODEV;
	}

	mca_parse_dts_u32(np, "ic_role", &chip->cp_role, SC8581_ROLE_MASTER);
	strscpy(chip->log_tag,
		chip->cp_role == SC8581_ROLE_SLAVE ? "[1]" : "[0]",
		sizeof(chip->log_tag));

	chip->irq_gpio = of_get_named_gpio(np, "cp-int", 0);
	if (chip->irq_gpio < 0) {
		mca_log_err("%s failed to parse irq_gpio\n", chip->log_tag);
		return -EINVAL;
	}

	chip->nlpm_gpio = of_get_named_gpio(np, "cp-nlpm-gpio", 0);
	if (chip->nlpm_gpio < 0) {
		mca_log_err("%s failed to parse  sc858_nlpm_gpio\n",
			    chip->log_tag);
		return -EINVAL;
	}

	mca_parse_dts_u32(np, "bat-ovp-threshold", &cfg->bat_ovp_th,
			  BAT_OVP_TH);
	mca_parse_dts_u32(np, "bat-ovp-alarm-threshold",
			  &cfg->bat_ovp_alarm_th, BAT_OVP_ALARM_TH);
	mca_parse_dts_u32_array(np, "bus-ovp-threshold", cfg->bus_ovp_th,
				CP_MODE_DIV_MAX);
	mca_log_info("%s bus_ovp [%d,%d,%d]\n", chip->log_tag,
		     cfg->bus_ovp_th[0], cfg->bus_ovp_th[1], cfg->bus_ovp_th[2]);
	mca_parse_dts_u32_array(np, "usb-ovp-threshold", cfg->usb_ovp_th,
				CP_MODE_DIV_MAX);
	mca_log_info("%s usb_ovp [%d,%d,%d]\n", chip->log_tag,
		     cfg->usb_ovp_th[0], cfg->usb_ovp_th[1], cfg->usb_ovp_th[2]);
	mca_parse_dts_u32_array(np, "bus-ocp-threshold", cfg->bus_ocp_th,
				CP_MODE_DIV_MAX);
	mca_log_info("%s bus_ocp [%d,%d,%d]\n", chip->log_tag,
		     cfg->bus_ocp_th[0], cfg->bus_ocp_th[1], cfg->bus_ocp_th[2]);
	mca_parse_dts_u32(np, "wpc-ovp-threshold", &cfg->wpc_ovp_th,
			  WPC_OVP_TH);
	mca_parse_dts_u32(np, "out-ovp-threshold", &cfg->out_ovp_th,
			  OUT_OVP_TH);
	mca_parse_dts_u32(np, "pmid2-uvp-threshold", &cfg->pmid2out_uvp_th,
			  PMID2OUT_UVP_TH);
	mca_parse_dts_u32(np, "pmid2-ovp-threshold", &cfg->pmid2out_ovp_th,
			  PMID2OUT_OVP_TH);
	chip->support_reverse_quick_charge =
		of_property_read_bool(np, "support_reverse_quick_charge");

	return 0;
}

/* Only the master drives the shared nLPM pin; bring the pumps out of LPM. */
static void sc8581_init_gpio(struct sc8581_device *chip)
{
	int ret;

	if (chip->cp_role != SC8581_ROLE_MASTER)
		return;

	ret = devm_gpio_request(chip->dev, chip->nlpm_gpio,
				dev_name(chip->dev));
	if (ret) {
		mca_log_info("%s unable to request nlpm gpio [%d]\n",
			     chip->log_tag, chip->nlpm_gpio);
		return;
	}

	ret = gpio_direction_output(chip->nlpm_gpio, 1);
	if (ret)
		mca_log_info("%s unable to set direction for nlpm gpio[%d]\n",
			     chip->log_tag, chip->nlpm_gpio);
	msleep(400);
}

static int cp_charge_detect_device(struct sc8581_device *chip)
{
	u8 id;
	int ret, retry;

	for (retry = 1; retry <= ERROR_RECOVERY_COUNT; retry++) {
		ret = cp_read_byte(chip, SC8581_REG_6E, &id);
		if (!ret)
			break;
		msleep(100);
		mca_log_info(" %s failed to read device id, retry count = %d\n",
			     chip->log_tag, retry);
	}

	if (ret) {
		mca_log_info("%s failed to detect CP device. retry %d times\n",
			     chip->log_tag, ERROR_RECOVERY_COUNT);
		return ret;
	}

	mca_log_info("%s sucess read device id = %x\n", chip->log_tag, id);

	switch (id) {
	case SC8561_DEVICE_ID:
		chip->chip_vendor = SC8561_VENDOR;
		break;
	case SC8581_DEVICE_ID:
		chip->chip_vendor = SC8581_VENDOR;
		break;
	case SC8585_DEVICE_ID:
		chip->chip_vendor = SC8585_VENDOR;
		break;
	default:
		mca_log_info("%s device_id is invalid\n", chip->log_tag);
		return -ENODEV;
	}

	return 0;
}

static void sc8581_cancel_irq_work(void *data)
{
	cancel_delayed_work_sync(data);
}

static int sc8581_register_irq(struct sc8581_device *chip)
{
	int ret;

	ret = devm_gpio_request(chip->dev, chip->irq_gpio, dev_name(chip->dev));
	if (ret < 0) {
		mca_log_info(" %s failed to master request gpio\n",
			     chip->log_tag);
		return ret;
	}

	chip->irq = gpio_to_irq(chip->irq_gpio);
	if (chip->irq < 0) {
		mca_log_info("%s failed to master get gpio_irq\n",
			     chip->log_tag);
		return chip->irq;
	}

	ret = devm_request_threaded_irq(chip->dev, chip->irq, sc8581_int_isr,
					NULL,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					dev_name(chip->dev), chip);
	if (ret < 0) {
		mca_log_info("%s failed to master request irq\n",
			     chip->log_tag);
		return ret;
	}

	enable_irq_wake(chip->irq);

	return 0;
}

static int sc8581_probe(struct i2c_client *client)
{
	struct sc8581_device *chip;
	int ret;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	if (mca_cp_is_en_buck_project_hwid()) {
		mca_log_err("adjust en buck CP addr");
		client->addr = EN_BUCK_CP_ADDR;
	} else {
		mca_log_err("normal CP addr");
	}

	chip->client = client;
	chip->dev = &client->dev;
	i2c_set_clientdata(client, chip);

	ret = sc8581_parse_dt(chip);
	if (ret) {
		mca_log_err(" %s failed to parse DTS\n", chip->log_tag);
		return ret;
	}

	sc8581_init_gpio(chip);

	ret = cp_charge_detect_device(chip);
	if (ret) {
		mca_log_err("%s failed to detect device\n", chip->log_tag);
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_IIC_ERROR, NULL);
		return ret;
	}

	INIT_DELAYED_WORK(&chip->irq_handle_work, sc8581_irq_handler);
	/* Registered before the IRQ so it runs after the IRQ is released. */
	ret = devm_add_action_or_reset(chip->dev, sc8581_cancel_irq_work,
				       &chip->irq_handle_work);
	if (ret)
		return ret;

	ret = sc8581_register_irq(chip);
	if (ret) {
		mca_log_err("%s failed to int irq\n", chip->log_tag);
		return ret;
	}

	ret = sc8581_init_device(chip);
	if (ret) {
		mca_log_err("%s failed to init sc8581\n", chip->log_tag);
		mca_event_block_notify(MCA_EVENT_TYPE_CP_INFO,
				       MCA_EVENT_CP_IIC_ERROR, NULL);
		return ret;
	}

	queue_delayed_work(system_wq, &chip->irq_handle_work, 0);

	chip->ovpgate_en = true;
	chip->i2c_is_working = true;
	reg_info.address = 0;
	chip->chip_ok = true;
	reg_info.count = 1;

	mca_debugfs_create_group(chip->cp_role == SC8581_ROLE_SLAVE ?
				 "sc85xx_01" : "sc85xx_00",
				 cp_debugfs_field_tbl,
				 ARRAY_SIZE(cp_debugfs_field_tbl), chip);
	platform_class_cp_register_ops(chip->cp_role, &sc8581_chg_ops, chip);
	mca_log_charge_log_register(MCA_CHARGE_LOG_ID_CP_MASTER_IC,
				    &g_sc8581_log_ops, chip);
	device_init_wakeup(chip->dev, true);

	mca_log_err("%s probe success %d\n", chip->log_tag, 0);

	return 0;
}

static void sc8581_remove(struct i2c_client *client)
{
	struct sc8581_device *chip = i2c_get_clientdata(client);

	sc8581_update_bits(chip, SC8581_REG_15, SC8581_ADC_EN_MASK, 0);
}

static void sc8581_shutdown(struct i2c_client *client)
{
	struct sc8581_device *chip = i2c_get_clientdata(client);

	sc8581_update_bits(chip, SC8581_REG_15, SC8581_ADC_EN_MASK, 0);

	if (gpio_direction_output(chip->nlpm_gpio, 0))
		mca_log_err("%s unable to reset adc and nlpm fail\n",
			    chip->log_tag);

	mca_log_info("%s sc8581 shutdown!\n", chip->log_tag);
}

static int sc8581_suspend(struct device *dev)
{
	struct sc8581_device *chip = dev_get_drvdata(dev);

	mca_log_info("%s sc8581 suspend!\n", chip->log_tag);
	sc8581_update_bits(chip, SC8581_REG_15, SC8581_ADC_EN_MASK, 0);
	chip->i2c_is_working = false;

	return 0;
}

static int sc8581_resume(struct device *dev)
{
	return 0;
}

/* The bus is usable again only once the whole resume has completed. */
static void sc8581_i2c_complete(struct device *dev)
{
	struct sc8581_device *chip = dev_get_drvdata(dev);

	if (!chip)
		return;

	chip->i2c_is_working = true;
	mca_log_info("%s sc8581 i2c complete!\n", chip->log_tag);
}

static const struct dev_pm_ops sc8581_pm_ops = {
	.complete = sc8581_i2c_complete,
	.suspend = sc8581_suspend,
	.resume = sc8581_resume,
};

static const struct of_device_id sc8581_of_match[] = {
	{ .compatible = "sc8581" },
	{ .compatible = "sc8585_master" },
	{ .compatible = "sc8585_slave" },
	{ },
};
MODULE_DEVICE_TABLE(of, sc8581_of_match);

static struct i2c_driver sc8581_driver = {
	.driver = {
		.name = "sc8581_charger_pump",
		.of_match_table = sc8581_of_match,
		.pm = &sc8581_pm_ops,
	},
	.probe = sc8581_probe,
	.remove = sc8581_remove,
	.shutdown = sc8581_shutdown,
};
module_i2c_driver(sc8581_driver);

MODULE_DESCRIPTION("southchip SC8581 driver");
MODULE_AUTHOR("lvxiaofeng <lvxiaofeng@xiaomi.com>");
MODULE_LICENSE("GPL v2");
