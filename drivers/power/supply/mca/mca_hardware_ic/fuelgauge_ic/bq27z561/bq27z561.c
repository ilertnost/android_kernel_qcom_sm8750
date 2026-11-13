// SPDX-License-Identifier: GPL-2.0
/*
 * bq27z561 fuel gauge driver
 *
 * Copyright (C) 2017 Texas Instruments Incorporated - http://www.ti.com/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "fuelgauge_ic"
#endif

#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/unaligned.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_hwid.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/common/mca_sysfs.h>
#include <mca/platform/platform_fg_ic_ops.h>
#include <mca/strategy/strategy_class.h>
#include "inc/bq27z561.h"
#include "inc/bq27z561_nfg1000_fw.h"

#define FG_I2C_RETRY_CNT		5
#define FG_MAC_BLOCK_LEN		36
#define REG_BLOCK_LENGTH		32

#define FG_IC_SOH_TH_DEFAUL		92
#define FG_IC_CYCLE_TH_DEFAUL		400
#define FG_IC_CYCLE_STEP_DEFAUL		50
#define FG_IC_RESISTANCE_ID_DEFAUL	100000

#define FG_TEMP_LOW_TH			(-1280)
#define FG_TEMP_HIGH_TH			1280
#define FG_VOLT_HIGH_TH			5000
#define FG_CURR_LOW_TH			(-25000)
#define FG_CURR_HIGH_TH			6000
#define FG_RM_FCC_HIGH_TH		8000
#define FG_RM_FCC_DEFAULT		2500000

#define FG_TERM_CURRENT			1500

static int fg_set_seal(struct bq_fg_chip *bq, int value);
static int fg_get_dod_count(struct bq_fg_chip *bq);

/*
 * Low level transfers
 */
static int __fg_read_block(struct i2c_client *client, u8 reg, u8 *buf, u8 len)
{
	struct i2c_msg msgs[2];
	int ret;

	if (!buf)
		return -EINVAL;

	if (!client || !client->adapter)
		return -ENODEV;

	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &reg;

	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = buf;

	ret = i2c_transfer(client->adapter, msgs, 2);
	if (ret < 0)
		mca_log_err("I2C transfer failed, ret=%d\n", ret);

	return ret;
}

static int __fg_write_block(struct i2c_client *client, u8 reg, u8 *buf, u8 len)
{
	struct i2c_msg msg;
	u8 *wbuf;
	int ret;

	if (!buf)
		return -EINVAL;

	if (!client || !client->adapter)
		return -ENODEV;

	wbuf = kmalloc(len + 1, GFP_KERNEL);
	if (!wbuf)
		return -ENOMEM;

	wbuf[0] = reg;
	memcpy(wbuf + 1, buf, len);

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = len + 1;
	msg.buf = wbuf;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		mca_log_err("I2C transfer failed, ret=%d\n", ret);

	kfree(wbuf);

	return ret;
}

static int __fg_write_byte(struct i2c_client *client, u8 reg, u8 val)
{
	int retry_cnt = 1;
	s32 ret = 0;

	while (retry_cnt <= FG_I2C_RETRY_CNT) {
		ret = i2c_smbus_write_byte_data(client, reg, val);
		if (ret >= 0)
			return ret;
		mca_log_err("i2c write byte fail: can't write 0x%02X to reg 0x%02X, ret:%d, cnt:%d\n",
			    val, reg, ret, retry_cnt);
		usleep_range(4000, 4100);
		retry_cnt++;
	}
	mca_log_err("i2c write byte fail: can't write 0x%02X to reg 0x%02X, ret:%d, cnt max:%d\n",
		    val, reg, ret, FG_I2C_RETRY_CNT);

	return ret;
}

static int __fg_read_byte(struct i2c_client *client, u8 reg, u8 *val)
{
	int retry_cnt = 1;
	s32 ret = 0;

	while (retry_cnt <= FG_I2C_RETRY_CNT) {
		ret = i2c_smbus_read_byte_data(client, reg);
		if (ret >= 0) {
			*val = (u8)ret;
			return ret;
		}
		mca_log_err("i2c read byte fail: can't read reg 0x%02X\n, ret:%d, cnt:%d",
			    reg, ret, retry_cnt);
		usleep_range(4000, 4100);
		retry_cnt++;
	}
	mca_log_err("i2c read byte fail: can't read reg 0x%02X\n, ret:%d, cnt max:%d",
		    reg, ret, FG_I2C_RETRY_CNT);

	return ret;
}

static int __fg_read_word(struct i2c_client *client, u8 reg, u16 *val)
{
	int retry_cnt = 1;
	s32 ret = 0;

	while (retry_cnt <= FG_I2C_RETRY_CNT) {
		ret = i2c_smbus_read_word_data(client, reg);
		if (ret >= 0) {
			*val = (u16)ret;
			return ret;
		}
		mca_log_err("i2c read word fail: can't read from reg 0x%02X, ret:%d, cnt:%d\n",
			    reg, ret, retry_cnt);
		usleep_range(4000, 4100);
		retry_cnt++;
	}
	mca_log_err("i2c read word fail: can't read from reg 0x%02X, ret:%d, cnt max:%d\n",
		    reg, ret, FG_I2C_RETRY_CNT);

	return ret;
}

static int __fg_write_word(struct i2c_client *client, u8 reg, u16 val)
{
	int retry_cnt = 1;
	s32 ret = 0;

	while (retry_cnt <= FG_I2C_RETRY_CNT) {
		ret = i2c_smbus_write_word_data(client, reg, val);
		if (ret >= 0)
			return ret;
		mca_log_err("i2c write word fail: can't write 0x%04X to reg 0x%02X, ret:%d, cnt:%d\n",
			    val, reg, ret, retry_cnt);
		usleep_range(4000, 4100);
		retry_cnt++;
	}
	mca_log_err("i2c write word fail: can't write 0x%04X to reg 0x%02X, ret:%d, cnt max:%d\n",
		    val, reg, ret, FG_I2C_RETRY_CNT);

	return ret;
}

/*
 * Take the bus for an ordinary transfer.  While a firmware image is being
 * written to the gauge the update owns the bus for a long time, so the lock
 * is only tried for, and the wait is abandoned as soon as an update is seen.
 * A caller that does not get the bus skips its transfer and carries on as if
 * it had read nothing (all zeroes); the conversions fall back on the last
 * good value while an update runs.
 *
 * Return: true once the bus is held.
 */
static bool fg_i2c_lock(struct bq_fg_chip *bq)
{
	if (READ_ONCE(bq->ota_updating))
		return false;

	while (!mutex_trylock(&bq->i2c_rw_lock)) {
		if (READ_ONCE(bq->ota_updating))
			return false;
		usleep_range(40, 50);
	}

	return true;
}

static void fg_i2c_unlock(struct bq_fg_chip *bq)
{
	mutex_unlock(&bq->i2c_rw_lock);
}

static int fg_write_byte(struct bq_fg_chip *bq, u8 reg, u8 val)
{
	int ret;

	if (!fg_i2c_lock(bq))
		return 0;

	ret = __fg_write_byte(bq->client, reg, val);
	fg_i2c_unlock(bq);

	return ret;
}

static int fg_read_byte(struct bq_fg_chip *bq, u8 reg, u8 *val)
{
	int ret;

	if (!fg_i2c_lock(bq))
		return 0;

	ret = __fg_read_byte(bq->client, reg, val);
	fg_i2c_unlock(bq);

	return ret;
}

static int fg_read_word(struct bq_fg_chip *bq, u8 reg, u16 *val)
{
	int ret;

	if (!fg_i2c_lock(bq))
		return 0;

	ret = __fg_read_word(bq->client, reg, val);
	fg_i2c_unlock(bq);

	return ret;
}

static int fg_write_word(struct bq_fg_chip *bq, u8 reg, u16 val)
{
	int ret;

	if (!fg_i2c_lock(bq))
		return 0;

	ret = __fg_write_word(bq->client, reg, val);
	fg_i2c_unlock(bq);

	return ret;
}

static int fg_write_block(struct bq_fg_chip *bq, u8 reg, u8 *data, u8 len)
{
	int ret;

	if (!fg_i2c_lock(bq))
		return 0;

	ret = __fg_write_block(bq->client, reg, data, len);
	fg_i2c_unlock(bq);

	return ret;
}

static noinline int fg_read_block(struct bq_fg_chip *bq, u8 reg, u8 *buf, u8 len)
{
	int ret;

	if (!fg_i2c_lock(bq))
		return 0;

	ret = __fg_read_block(bq->client, reg, buf, len);
	fg_i2c_unlock(bq);

	return ret;
}

static u8 checksum(u8 *data, u8 len)
{
	u16 sum = 0;
	u8 i;

	for (i = 0; i < len; i++)
		sum += data[i];

	sum &= 0xFF;

	return 0xFF - sum;
}

static void fg_print_buf(const char *msg, u8 *buf, u8 len)
{
	static char strbuf[128];
	int idx = 0;
	int i;

	for (i = 0; i < len && idx < (int)sizeof(strbuf) - 3; i++)
		idx += scnprintf(&strbuf[idx], sizeof(strbuf) - idx, "%02X ",
				 buf[i]);
	mca_log_debug("%s %s\n", msg, strbuf);
}

/*
 * ManufacturerAccess block read: the command goes to AltManufacturerAccess,
 * the answer comes back from the same register as the command echo, 32 data
 * bytes, the checksum and the length.
 */
static int __fg_mac_read_block(struct bq_fg_chip *bq, u16 cmd, u8 *buf, u8 len)
{
	u8 t_buf[40] = { 0 };
	u8 cksum_calc, cksum;
	u8 t_len;
	int ret;
	int i;

	t_buf[0] = (u8)cmd;
	t_buf[1] = (u8)(cmd >> 8);
	ret = __fg_write_block(bq->client, bq->regs[BQ_FG_REG_ALT_MAC], t_buf, 2);
	if (ret >= 0) {
		usleep_range(4000, 4100);
		ret = __fg_read_block(bq->client, bq->regs[BQ_FG_REG_ALT_MAC],
				      t_buf, FG_MAC_BLOCK_LEN);
	}
	if (ret < 0) {
		mca_log_err("mac write read fail, ret: %d\n", ret);
		return ret;
	}

	if (t_buf[0] != (u8)cmd || t_buf[1] != (u8)(cmd >> 8)) {
		mca_log_err("mac read wrong, read: %02x%02x, out: %02x%02x\n",
			    t_buf[1], t_buf[0], (u8)(cmd >> 8), (u8)cmd);
		return -1;
	}

	if (cmd == FG_MAC_CMD_LIFETIME1)
		fg_print_buf("mac_read_block FG_MAC_CMD_LIFETIME1:", t_buf,
			     FG_MAC_BLOCK_LEN);
	else if (cmd == FG_MAC_CMD_LIFETIME3)
		fg_print_buf("mac_read_block FG_MAC_CMD_LIFETIME3:", t_buf,
			     FG_MAC_BLOCK_LEN);

	cksum = t_buf[34];
	t_len = t_buf[35];
	if (t_len > 42) {
		mca_log_err("cksum:%d, t_len err:%d.\n", cksum, t_len);
		t_len = 42;
	} else if (t_len < 2) {
		mca_log_err("cksum:%d, t_len err:%d.\n", cksum, t_len);
		t_len = 2;
	}

	cksum_calc = checksum(t_buf, t_len - 2);
	if (cksum_calc != cksum) {
		mca_log_err("cksum:%d, t_len err:%d.\n", cksum, t_len);
		return -1;
	}

	for (i = 0; i < len && i < (int)sizeof(t_buf) - 2; i++)
		buf[i] = t_buf[i + 2];

	return ret;
}

static noinline int fg_mac_read_block(struct bq_fg_chip *bq, u16 cmd, u8 *buf,
				      u8 len)
{
	int ret;

	if (!fg_i2c_lock(bq))
		return 0;

	ret = __fg_mac_read_block(bq, cmd, buf, len);
	fg_i2c_unlock(bq);

	return ret;
}

static int __fg_mac_write_block(struct bq_fg_chip *bq, u16 cmd, u8 *data, u8 len)
{
	u8 t_buf[40] = { 0 };
	int ret;

	t_buf[0] = (u8)cmd;
	t_buf[1] = (u8)(cmd >> 8);
	memcpy(&t_buf[2], data, len);

	ret = __fg_write_block(bq->client, bq->regs[BQ_FG_REG_ALT_MAC], t_buf,
			       len + 2);
	if (ret < 0) {
		mca_log_err("wirte block failed %d %d\n", t_buf[0], t_buf[1]);
		return ret;
	}

	t_buf[0] = checksum(t_buf, len + 2);
	t_buf[1] = len + 4;	/* cmd, data, checksum and the length itself */
	ret = __fg_write_block(bq->client, bq->regs[BQ_FG_REG_MAC_CHKSUM],
			       t_buf, 2);

	/* data flash writes need time before the gauge takes the next one */
	if (cmd > 0x4000 || cmd == FG_MAC_CMD_CLEAR_COUNT_DATA ||
	    cmd == FG_MAC_CMD_UI_SOH)
		msleep(100);

	return ret;
}

static noinline int fg_mac_write_block(struct bq_fg_chip *bq, u16 cmd, u8 *data,
				       u8 len)
{
	int ret;

	if (len > REG_BLOCK_LENGTH)
		return -EINVAL;

	if (!fg_i2c_lock(bq))
		return 0;

	ret = __fg_mac_write_block(bq, cmd, data, len);
	fg_i2c_unlock(bq);

	return ret;
}

/*
 * Value conversions; an out-of-range value (or one taken while a firmware
 * update holds the bus) is replaced by the last good one.
 */
static int fg_convert_bytes_to_volt(struct bq_fg_chip *bq, u16 raw)
{
	int volt = raw;

	if (volt > FG_VOLT_HIGH_TH || bq->ota_updating) {
		mca_log_err("volt out of range: %d\n", volt);
		volt = bq->batt_volt;
	}

	if (!volt)
		volt = max(bq->batt_volt, 0);

	bq->batt_volt = volt;
	mca_log_debug("volt: %d\n", bq->batt_volt);

	return bq->batt_volt;
}

static int fg_convert_bytes_to_curr(struct bq_fg_chip *bq, u16 raw)
{
	int curr;

	if (raw > 32768)
		curr = 65536 - raw;
	else
		curr = -raw;

	if (curr < FG_CURR_LOW_TH || curr > FG_CURR_HIGH_TH || bq->ota_updating) {
		mca_log_err("current out of range: %d\n", curr);
		curr = bq->batt_curr / 1000;
	}

	bq->batt_curr = curr * 1000;
	mca_log_debug("temp_curr: %d, curr: %d\n", raw, bq->batt_curr);

	return bq->batt_curr;
}

static int fg_convert_bytes_to_temp(struct bq_fg_chip *bq, u16 raw)
{
	int temp = raw - 2730;

	if (temp < FG_TEMP_LOW_TH || temp > FG_TEMP_HIGH_TH || bq->ota_updating) {
		mca_log_err("temperature out of range: %d\n", temp);
		temp = bq->batt_temp;
	}

	if (bq->fake_temp != FG_FAKE_TEMP_NONE)
		temp = bq->fake_temp;

	bq->batt_temp = temp;

	return bq->batt_temp;
}

static int fg_convert_bytes_to_original_temp(struct bq_fg_chip *bq, u16 raw)
{
	int temp = raw - 2730;

	if (temp < FG_TEMP_LOW_TH || temp > FG_TEMP_HIGH_TH || bq->ota_updating) {
		mca_log_err("temperature out of range: %d\n", temp);
		temp = bq->original_temp;
	}

	if (bq->fake_temp != FG_FAKE_TEMP_NONE)
		temp = bq->fake_temp;

	bq->original_temp = temp;
	mca_log_info("temperature: %d\n", bq->original_temp);

	return bq->original_temp;
}

static int fg_convert_bytes_to_rm(struct bq_fg_chip *bq, u16 raw)
{
	int rm;

	if (raw <= FG_RM_FCC_HIGH_TH && !bq->ota_updating) {
		rm = raw * 1000;
	} else {
		mca_log_err("rm out of range: %d\n", raw);
		rm = bq->batt_rm;
	}

	if (!raw && bq->batt_rm >= FG_RM_FCC_DEFAULT)
		rm = bq->batt_rm;

	bq->batt_rm = rm;

	return bq->batt_rm;
}

static int fg_convert_bytes_to_fcc(struct bq_fg_chip *bq, u16 raw)
{
	int fcc;

	if (raw <= FG_RM_FCC_HIGH_TH && !bq->ota_updating) {
		fcc = raw * 1000;
	} else {
		mca_log_err("fcc out of range: %d\n", raw);
		fcc = bq->batt_fcc;
	}

	if (!raw) {
		if (bq->batt_fcc > 0)
			fcc = bq->batt_fcc;
		else if (!bq->batt_fcc)
			fcc = FG_RM_FCC_DEFAULT;
	}

	bq->batt_fcc = fcc;

	return bq->batt_fcc;
}

/*
 * Standard commands
 */
static int fg_get_chip_ok(struct bq_fg_chip *bq, int *ok)
{
	u16 flags = 0;
	int times;
	int ret;

	for (times = 0; times < FG_ERROR_CHECK_MAX_TIMES; times++) {
		ret = fg_read_word(bq, bq->regs[BQ_FG_REG_BATT_STATUS], &flags);
		if (ret >= 0) {
			bq->fg_error = false;
			break;
		}
		mca_log_info("i2c read fail, retry %d\n", times);
		bq->fg_error = true;
	}

	*ok = !bq->fg_error;

	return 0;
}

static int fg_read_volt(struct bq_fg_chip *bq, int *vbat)
{
	u16 volt = 0;
	int ret;

	if (bq->fg_error)
		return FG_ERROR_FAKE_VOLT;

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_VOLT], &volt);
	if (ret < 0) {
		mca_log_err("could not read volt ret = %d\n", ret);
		return ret;
	}

	*vbat = fg_convert_bytes_to_volt(bq, volt);

	return 0;
}

static void fg_read_packvolt(struct bq_fg_chip *bq, int *packvolt)
{
	u16 volt = 0;
	int ret;

	if (bq->fg_error)
		return;

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_PACK_VOLT], &volt);
	if (ret < 0) {
		mca_log_err("could not read volt ret = %d\n", ret);
		return;
	}

	*packvolt = fg_convert_bytes_to_volt(bq, volt);
}

static void fg_read_co_status(struct bq_fg_chip *bq, int *co_status)
{
	u8 t_buf[1] = { 0 };

	if (fg_mac_read_block(bq, FG_MCA_CMD_SEAL_STATE, t_buf, 1) < 0)
		mca_log_err("Failed to read CO state\n");

	*co_status = !!(t_buf[0] & FG_FLAGS_CO);
}

static int fg_read_current(struct bq_fg_chip *bq, int *curr)
{
	u16 temp_curr = 0;
	int ret;

	if (bq->fg_error) {
		*curr = FG_ERROR_FAKE_CURR;
		return 0;
	}

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_CN], &temp_curr);
	if (ret < 0) {
		mca_log_err("could not read current, ret = %d\n", ret);
		*curr = 0;
		return ret;
	}

	*curr = fg_convert_bytes_to_curr(bq, temp_curr);

	return 0;
}

static int fg_read_rsoc(struct bq_fg_chip *bq, int *rsoc)
{
	u16 soc = 0;
	int ret;

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_SOC], &soc);
	if (ret < 0) {
		mca_log_info("could not read RSOC, ret = %d\n", ret);
		return ret;
	}

	if (!bq->ota_updating)
		bq->batt_rsoc = soc;
	*rsoc = bq->batt_rsoc;

	return 0;
}

static int fg_read_temperature(struct bq_fg_chip *bq, int *temp)
{
	u16 temp_now = 0;
	int ret;

	if (bq->fake_temp != FG_FAKE_TEMP_NONE) {
		*temp = bq->fake_temp;
		return 0;
	}

	if (bq->fg_error) {
		*temp = FG_ERROR_FAKE_TEMP;
		return 0;
	}

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_TEMP], &temp_now);
	if (ret < 0) {
		mca_log_info("could not read temperature, ret = %d\n", ret);
		return ret;
	}

	*temp = fg_convert_bytes_to_temp(bq, temp_now);

	return 0;
}

static noinline int fg_read_original_temperature(struct bq_fg_chip *bq, int *temp)
{
	u16 temp_now = 0;
	int ret;

	if (bq->fg_error) {
		*temp = FG_ERROR_FAKE_TEMP;
		return 0;
	}

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_ORIGINAL_TEMP], &temp_now);
	if (ret < 0) {
		mca_log_info("could not read temperature, ret = %d\n", ret);
		return ret;
	}

	*temp = fg_convert_bytes_to_original_temp(bq, temp_now);

	return 0;
}

static int fg_read_cyclecount(struct bq_fg_chip *bq, int *cycle_count)
{
	u16 cc = 0;
	int ret;

	if (bq->fake_cycle) {
		*cycle_count = bq->fake_cycle;
		return 0;
	}

	if (bq->regs[BQ_FG_REG_CC] == INVALID_REG_ADDR) {
		mca_log_info("Cycle Count not supported!\n");
		return -1;
	}

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_CC], &cc);
	if (ret < 0) {
		mca_log_info("could not read Cycle Count, ret=%d\n", ret);
		return ret;
	}

	if (bq->fake_cycle)
		bq->batt_cycle_count = bq->fake_cycle;
	else if (!bq->ota_updating)
		bq->batt_cycle_count = cc;
	*cycle_count = bq->batt_cycle_count;

	return 0;
}

static void fg_set_cyclecount(struct bq_fg_chip *bq, int value)
{
	bq->fake_cycle = value;
	mca_log_info("set fake cyclecount:: %d\n", bq->fake_cycle);
}

static void fg_read_qmax(struct bq_fg_chip *bq, int *qmax)
{
	u8 t_buf[64] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_QMAX_CYCLECOUNT, t_buf, 10) < 0)
		return;

	*qmax = (t_buf[9] << 8) | t_buf[8];
	if (*qmax)
		return;

	if (fg_mac_read_block(bq, FG_MAC_CMD_QMAX, t_buf, 2) < 0)
		return;

	*qmax = (t_buf[1] << 8) | t_buf[0];
}

static int fg_read_rm(struct bq_fg_chip *bq, int *rm)
{
	u16 data = 0;
	int ret;

	if (bq->regs[BQ_FG_REG_RM] == INVALID_REG_ADDR) {
		mca_log_info("RemainingCapacity command not supported!\n");
		return 0;
	}

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_RM], &data);
	if (ret < 0) {
		mca_log_info("could not read DC, ret=%d\n", ret);
		return ret;
	}

	*rm = fg_convert_bytes_to_rm(bq, data);

	return 0;
}

static int fg_read_fcc(struct bq_fg_chip *bq, int *fcc)
{
	u16 data = 0;
	int ret;

	if (bq->regs[BQ_FG_REG_FCC] == INVALID_REG_ADDR) {
		mca_log_info("FCC command not supported!\n");
		return 0;
	}

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_FCC], &data);
	if (ret < 0) {
		mca_log_info("could not read FCC ret %d\n", ret);
		return ret;
	}

	*fcc = fg_convert_bytes_to_fcc(bq, data);

	return 0;
}

static int fg_read_soh(struct bq_fg_chip *bq, int *soh)
{
	u16 data = 0;
	int ret;

	if (bq->fake_soh != FG_FAKE_SOH_NONE) {
		*soh = bq->fake_soh;
		return 0;
	}

	if (bq->regs[BQ_FG_REG_SOH] == INVALID_REG_ADDR) {
		mca_log_info("SOH command not supported!\n");
		return 0;
	}

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_SOH], &data);
	if (ret < 0) {
		mca_log_info("could not read DC, ret=%d\n", ret);
		return ret;
	}

	*soh = data;

	return 0;
}

static void fg_set_fake_soh(struct bq_fg_chip *bq, int value)
{
	bq->fake_soh = value;
	mca_log_info("set fake soh:: %d\n", bq->fake_soh);
}

static void fg_read_fcc_soh(struct bq_fg_chip *bq, int *fcc_soh)
{
	u8 t_buf[64] = { 0 };
	int ret;

	if (bq->fg_vendor != BQ27Z561)
		return;

	ret = fg_mac_read_block(bq, FG_MAC_CMD_FCC_SOH, t_buf, 2);
	if (ret < 0) {
		mca_log_info("could not read FCC_SOH, ret=%d\n", ret);
		return;
	}

	*fcc_soh = (t_buf[0] << 8) | t_buf[1];
}

static void fg_read_tsim(struct bq_fg_chip *bq, int *tsim)
{
	u8 t_buf[64] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_ITSTATUS1, t_buf, 36) < 0)
		return;

	if (bq->fg_vendor == BQ27Z561)
		*tsim = (t_buf[13] << 8) | t_buf[12];
	else
		*tsim = (t_buf[17] << 8) | t_buf[16];
}

static void fg_read_tambient(struct bq_fg_chip *bq, int *tambient)
{
	u8 t_buf[64] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_ITSTATUS1, t_buf, 36) < 0)
		return;

	if (bq->fg_vendor == BQ27Z561)
		*tambient = (t_buf[15] << 8) | t_buf[14];
	else
		*tambient = (t_buf[9] << 8) | t_buf[8];
}

static void fg_read_tremq(struct bq_fg_chip *bq, int *tremq)
{
	u8 t_buf[64] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_ITSTATUS1, t_buf, 36) < 0)
		return;

	*tremq = (t_buf[1] << 8) | t_buf[0];
}

static void fg_read_tfullchgq(struct bq_fg_chip *bq, int *tfullchgq)
{
	u8 t_buf[64] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_ITSTATUS1, t_buf, 36) < 0)
		return;

	*tfullchgq = (t_buf[3] << 8) | t_buf[2];
}

static void fg_read_avercurrent(struct bq_fg_chip *bq, int *avercurrent)
{
	u16 curr = 0;

	if (fg_read_word(bq, bq->regs[BQ_FG_REG_AVER_CURRENT], &curr) < 0)
		return;

	if (curr > 32768)
		*avercurrent = (65536 - curr) * 1000;
	else
		*avercurrent = -curr * 1000;
}

static void fg_get_seal(struct bq_fg_chip *bq, int *value)
{
	u8 t_buf[64] = { 0 };

	if (fg_mac_read_block(bq, FG_MCA_CMD_SEAL_STATE, t_buf, 32) < 0)
		return;

	*value = t_buf[1] & 0x03;
}

static void fg_read_df_check(struct bq_fg_chip *bq, int *value)
{
	u8 t_buf[32] = { 0 };

	if (fg_mac_read_block(bq, FG_MCA_CMD_DF, t_buf, 32) < 0)
		return;

	*value = (t_buf[1] << 8) | t_buf[0];
}

static void fg_read_chem_df_sign(struct bq_fg_chip *bq, int *chem_df_sign)
{
	u8 t_buf[32] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_CHEM_DF_SIGN, t_buf, 32) < 0)
		return;

	*chem_df_sign = (t_buf[0] << 8) | t_buf[1];
}

static void fg_read_max_temp_occur_time(struct bq_fg_chip *bq, int *value)
{
	u8 t_buf[32] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_LIFETIME3, t_buf, 32) < 0)
		return;

	*value = t_buf[20] | (t_buf[21] << 8) | (t_buf[22] << 16) |
		 (t_buf[23] << 24);
	mca_log_info("%d", *value);
}

static int fg_read_dc(struct bq_fg_chip *bq, int *dc)
{
	u16 data = 0;
	int ret;

	if (bq->regs[BQ_FG_REG_DC] == INVALID_REG_ADDR) {
		mca_log_info("DesignCapacity command not supported!\n");
		return 0;
	}

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_DC], &data);
	if (ret < 0) {
		mca_log_info("could not read DC, ret=%d\n", ret);
		return -1;
	}

	*dc = data * 1000;

	return 0;
}

static unsigned long fg_get_calc_rvalue(struct bq_fg_chip *bq)
{
	u8 data[32] = { 0 };
	unsigned long rvalue;
	u16 cmd;

	if (bq->fg_vendor == MPC7012)
		cmd = FG_MAC_CMD_MPC7012_CALC_RVALUE;
	else
		cmd = FG_MAC_CMD_CALC_RVALUE;

	if (fg_mac_read_block(bq, cmd, data, 32) < 0)
		return 0;

	rvalue = (get_unaligned_le16(&data[4]) * 45UL +
		  get_unaligned_le16(&data[10]) * 45UL +
		  get_unaligned_le16(&data[16]) * 70UL +
		  get_unaligned_le16(&data[18]) * 45UL +
		  get_unaligned_le16(&data[24]) * 100UL +
		  get_unaligned_le16(&data[26]) * 70UL +
		  get_unaligned_le16(&data[28]) * 45UL) / 10;
	mca_log_info("[%u]calr_rvalue=%lu\n", bq->dev_role, rvalue);

	return rvalue;
}

/* BATT_SN carries the date as year digit, month code and two day digits. */
static int fg_read_manufacturing_date(struct bq_fg_chip *bq, u8 *buf)
{
	u8 t_buf[64] = { 0 };
	char date[8] = { '2', '0', '2', '0', '0', '0', '0', '0' };
	int ret;

	ret = fg_mac_read_block(bq, FG_MAC_CMD_BATT_SN, t_buf, 32);
	if (ret < 0) {
		mca_log_info("failed to get BATT_SN\n");
		return ret;
	}

	date[3] = '0' + (t_buf[6] & 0xF);
	switch (t_buf[7]) {
	case 'A':
		date[4] = '1';
		date[5] = '0';
		break;
	case 'B':
		date[4] = '1';
		date[5] = '1';
		break;
	case 'C':
		date[4] = '1';
		date[5] = '2';
		break;
	default:
		date[4] = '0';
		date[5] = '0' + (t_buf[7] & 0xF);
		break;
	}
	date[6] = '0' + (t_buf[8] & 0xF);
	date[7] = '0' + (t_buf[9] & 0xF);

	mca_log_err("read manufacturing_date=%c,%c,%c,%c,%c,len=%d\n",
		    date[3], date[4], date[5], date[6], date[7],
		    (int)sizeof(date));

	memcpy(buf, date, sizeof(date));

	return 0;
}

/*
 * The first usage date is kept in the UI_SOH block as year (two digits),
 * month and day bytes at 11, 12 and 13.  The result is written as eight
 * characters, YYYYMMDD; the caller provides the terminating zero.
 */
static int fg_read_first_usage_date(struct bq_fg_chip *bq, u8 *buf)
{
	u8 t_buf[64] = { 0 };
	int ret, i;

	ret = fg_mac_read_block(bq, FG_MAC_CMD_UI_SOH, t_buf, 32);
	if (ret < 0) {
		memset(buf, '9', 8);
		mca_log_info("read failed %.*s\n", 8, buf);
		return ret;
	}

	buf[0] = '2';
	buf[1] = '0';
	buf[2] = '0' + t_buf[11] / 10;
	buf[3] = '0' + t_buf[11] % 10;
	buf[4] = '0' + t_buf[12] / 10;
	buf[5] = '0' + t_buf[12] % 10;
	buf[6] = '0' + t_buf[13] / 10;
	buf[7] = '0' + t_buf[13] % 10;

	if (!strncmp(&buf[2], "000000", 6)) {
		memset(buf, '0', 8);
		mca_log_err(" usage_date init %.*s\n", 8, buf);
		return 0;
	}

	if (bq->support_version_compatible) {
		if (!strncmp(buf, "2004", 4)) {
			buf[2] = '2';
			mca_log_info("11-11 version compatiable\n");
		} else if (!strncmp(buf, "2052", 4)) {
			memcpy(&buf[2], "240901", 6);
			mca_log_info("09-13 version compatiable\n");
		}
	}

	for (i = 0; i < 8; i++) {
		if (!isdigit(buf[i])) {
			mca_log_info("usage data has invalid char [%.*s]\n", 8, buf);
			return 0;
		}
	}
	mca_log_err("read successs = %.*s\n", 8, buf);

	return 0;
}

#define FG_USAGE_DATE_LEN	8
#define FG_USAGE_DATE_RETRY	3

static void fg_write_first_usage_date(struct bq_fg_chip *bq, const char *buf,
				      size_t size)
{
	u8 data[32] = { 0 };
	u8 data_temp[32] = { 0 };
	u8 year, month, day;
	char *tmp_buf;
	size_t i, j;
	int ret, retry;
	int len;

	tmp_buf = kzalloc(size + 1, GFP_KERNEL);
	if (!tmp_buf)
		return;

	for (i = 0, j = 0; i < size && buf[i]; i++) {
		if (!isspace(buf[i]))
			tmp_buf[j++] = buf[i];
	}
	tmp_buf[j] = '\0';
	len = strnlen(tmp_buf, size + 1);
	mca_log_info("write first_usage_date=%s size=%d, len = %d\n", tmp_buf,
		     (int)size, len);

	if (len != FG_USAGE_DATE_LEN) {
		mca_log_err("Invalid date input: %s\n", tmp_buf);
		goto out;
	}

	for (i = 0; i < FG_USAGE_DATE_LEN; i++) {
		if (!isdigit(tmp_buf[i])) {
			mca_log_err("input is not all digits! errorstr = %s size = [%d]\n",
				    tmp_buf, (int)size);
			goto out;
		}
	}

	ret = fg_mac_read_block(bq, FG_MAC_CMD_UI_SOH, data, 32);
	if (ret < 0) {
		mca_log_err("failed to get first_usage\n");
		goto out;
	}

	year = (tmp_buf[2] - '0') * 10 + (tmp_buf[3] - '0');
	month = (tmp_buf[4] - '0') * 10 + (tmp_buf[5] - '0');
	day = (tmp_buf[6] - '0') * 10 + (tmp_buf[7] - '0');
	data[11] = year;
	data[12] = month;
	data[13] = day;

	for (retry = 0; retry < FG_USAGE_DATE_RETRY; retry++) {
		ret = fg_mac_write_block(bq, FG_MAC_CMD_UI_SOH, data, 32);
		msleep(100);
		if (ret < 0)
			continue;

		memset(data_temp, 0, sizeof(data_temp));
		ret = fg_mac_read_block(bq, FG_MAC_CMD_UI_SOH, data_temp, 32);
		if (ret < 0) {
			mca_log_info("failed to read first_usage_date\n");
			continue;
		}

		if (data_temp[11] == year && data_temp[12] == month &&
		    data_temp[13] == day) {
			mca_log_err("write first_usage_date success\n");
			goto out;
		}
		mca_log_err("write fail Updata[%d][%d][%d] = Reg[%d][%d][%d]\n",
			    year, month, day, data_temp[11], data_temp[12],
			    data_temp[13]);
	}

	/* Could not get the date to stick: leave the field cleared instead. */
	data[11] = 0;
	data[12] = 0;
	data[13] = 0;
	for (retry = 0; retry < FG_USAGE_DATE_RETRY; retry++) {
		ret = fg_mac_write_block(bq, FG_MAC_CMD_UI_SOH, data, 32);
		msleep(100);
		if (ret < 0)
			continue;

		memset(data_temp, 0, sizeof(data_temp));
		fg_mac_read_block(bq, FG_MAC_CMD_UI_SOH, data_temp, 32);
		if (!data_temp[11] && !data_temp[12] && !data_temp[13]) {
			mca_log_err("reset success\n");
			goto out;
		}
		mca_log_err("reset fail Reg[%d][%d][%d]\n", data_temp[11],
			    data_temp[12], data_temp[13]);
	}

out:
	kfree(tmp_buf);
}

static int fg_get_dod_count(struct bq_fg_chip *bq)
{
	u8 data[32] = { 0 };
	int ret;

	ret = fg_mac_read_block(bq, FG_MAC_CMD_MIXDATARECORD1, data, 32);
	if (ret < 0) {
		mca_log_err("could not read dod_count = %d\n", ret);
		return ret;
	}

	bq->count_level1 = get_unaligned_le16(&data[8]);
	bq->count_level2 = get_unaligned_le16(&data[10]);
	bq->count_level3 = get_unaligned_le16(&data[12]);
	bq->count_lowtemp = get_unaligned_le16(&data[20]);
	mca_log_info("dod_count: %d, %d, %d, %d\n", bq->count_level1,
		     bq->count_level2, bq->count_level3, bq->count_lowtemp);

	return 0;
}

static int fg_get_cutoff_voltage(struct bq_fg_chip *bq, int *volt)
{
	u8 data[2] = { 0 };
	u16 cmd;
	int ret;

	if (bq->fg_vendor == MPC7012)
		cmd = FG_MAC_CMD_MPC7012_CUTOFF_VOLTAGE;
	else
		cmd = FG_MAC_CMD_CUTOFF_VOLTAGE;

	ret = fg_mac_read_block(bq, cmd, data, 2);
	if (ret < 0) {
		mca_log_err("could not read cutoff_voltage = %d\n", ret);
		return ret;
	}

	*volt = (data[1] << 8) | data[0];

	return 0;
}

static noinline const char *fg_get_device_name(struct bq_fg_chip *bq)
{
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	char data[8] = { 0 };
	int ret;

	if (bq->fake_device_name[0])
		return bq->fake_device_name;

	ret = fg_mac_read_block(bq, FG_MAC_CMD_DEVICE_NAME, data, 8);
	if (ret < 0) {
		mca_log_err("could not read device_name, ret=%d\n", ret);
		memset(bq->device_name, 0, sizeof(bq->device_name));
		strscpy(bq->device_name, "NULL", sizeof(bq->device_name));
		return bq->device_name;
	}

	/* e.g. "O2@BP54#" or "XM34@BMX0#" */
	data[7] = '\0';
	memset(bq->device_name, 0, sizeof(bq->device_name));
	if (hwid && hwid->platform_version == HARDWARE_PROJECT_O9)
		memcpy(bq->device_name, data, 5);
	else
		memcpy(bq->device_name, &data[3], 4);

	mca_log_info("0x004A: %.*s, model_name: %.*s\n",
		     (int)strnlen(data, 7), data,
		     (int)strnlen(bq->device_name, 7), bq->device_name);

	return bq->device_name;
}

static void fg_read_run_time(struct bq_fg_chip *bq, int *value)
{
	u8 t_buf[32] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_ITSTATUS1, t_buf, 32) < 0)
		return;

	*value = t_buf[20] | (t_buf[21] << 8) | (t_buf[22] << 16) |
		 (t_buf[23] << 24);
	mca_log_info("%d", *value);
}

static void fg_read_max_temp_time(struct bq_fg_chip *bq, int *value)
{
	int max_temp_time = 0;
	int run_time = 0;

	fg_read_max_temp_occur_time(bq, &max_temp_time);
	fg_read_run_time(bq, &run_time);

	*value = run_time > max_temp_time ? (run_time - max_temp_time) / 60 : 0;
}

static void fg_read_total_fw_runtime(struct bq_fg_chip *bq, int *value)
{
	u8 t_buf[32] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_FW_RUNTIME, t_buf, 32) < 0)
		return;

	*value = t_buf[8] | (t_buf[9] << 8);
	mca_log_info("%d", *value);
}

/*
 * Highest cell temperature seen.  A reading outside what a cell plausibly
 * records (20..60) is taken again once before being passed on.
 */
static int fg_get_temp_max(struct bq_fg_chip *bq, int *temp_max)
{
	u8 data[32] = { 0 };
	int val = 0;
	int ret, retry;

	for (retry = 1; retry <= 2; retry++) {
		ret = fg_mac_read_block(bq, FG_MAC_CMD_LIFETIME1, data, 32);
		if (ret >= 0)
			break;
		mca_log_err("the address FG_MAC_CMD_LIFETIME1 is error, retry: %d\n",
			    retry);
	}

	if (ret >= 0) {
		val = data[6];
		if (val >= 20 && val <= 60)
			goto out;
	}

	ret = fg_mac_read_block(bq, FG_MAC_CMD_LIFETIME1, data, 32);
	if (ret < 0)
		mca_log_err("failed to get FG_MAC_CMD_LIFETIME1\n");
	else
		val = data[6];

out:
	*temp_max = val;
	mca_log_info("fg get temperature max is: %d\n", val);

	return 0;
}

static int fg_read_time_ht(struct bq_fg_chip *bq, int *value)
{
	u8 t_buf[32] = { 0 };
	int ret;

	ret = fg_mac_read_block(bq, FG_MAC_CMD_LIFETIME3, t_buf, 32);
	if (ret < 0)
		return ret;

	switch (bq->fg_vendor) {
	case BQ27Z561:
		*value = ((t_buf[19] << 8) | t_buf[18]) * 2;
		break;
	case NFG1000A:
	case NFG1000B:
	case MPC8011B:
	case MPC7012:
		*value = (t_buf[13] << 8) | t_buf[12];
		break;
	default:
		break;
	}

	return ret;
}

static int fg_read_time_ot(struct bq_fg_chip *bq, int *value)
{
	u8 t_buf[32] = { 0 };
	int ret;

	ret = fg_mac_read_block(bq, FG_MAC_CMD_LIFETIME3, t_buf, 32);
	if (ret < 0)
		return ret;

	switch (bq->fg_vendor) {
	case BQ27Z561:
		*value = ((t_buf[21] << 8) | t_buf[20]) * 2;
		break;
	case NFG1000A:
	case NFG1000B:
	case MPC8011B:
	case MPC7012:
		*value = (t_buf[15] << 8) | t_buf[14];
		break;
	default:
		break;
	}

	return ret;
}

/*
 * The power-learning registers of the NVT gauges.  Signed words are reported
 * negated, as 16-bit values.
 */
static u8 fg_nvt_read_byte(struct bq_fg_chip *bq, int idx, const char *name)
{
	u8 val = 0;
	int ret;

	if (bq->regs[idx] == INVALID_REG_ADDR) {
		mca_log_info("%s command not supported!\n", name);
		return 0;
	}

	ret = fg_read_byte(bq, bq->regs[idx], &val);
	if (ret < 0)
		mca_log_info("could not read %s, ret=%d\n", name, ret);

	return val;
}

static u16 fg_nvt_read_word(struct bq_fg_chip *bq, int idx, const char *name)
{
	u16 val = 0;
	int ret;

	if (bq->regs[idx] == INVALID_REG_ADDR) {
		mca_log_info("%s command not supported!\n", name);
		return 0;
	}

	ret = fg_read_word(bq, bq->regs[idx], &val);
	if (ret < 0)
		mca_log_info("could not read %s, ret=%d\n", name, ret);

	return val;
}

static void fg_nvt_start(struct bq_fg_chip *bq, int idx, const char *name)
{
	int ret;

	if (bq->regs[idx] == INVALID_REG_ADDR) {
		mca_log_info("%s command not supported!\n", name);
		return;
	}

	ret = fg_write_byte(bq, bq->regs[idx], 0x01);
	if (ret < 0)
		mca_log_info("could not write %s, ret=%d\n", name, ret);
}

static void fg_nvt_set_power(struct bq_fg_chip *bq, int idx, const char *name,
			     int val)
{
	int ret;

	if (bq->regs[idx] == INVALID_REG_ADDR) {
		mca_log_info("%s command not supported!\n", name);
		return;
	}

	if (val > 32766)
		val = 32767;

	ret = fg_write_word(bq, bq->regs[idx], (u16)-val);
	if (ret < 0)
		mca_log_info("could not write %s, ret=%d\n", name, ret);
}

static int fg_get_over_peak_flag(struct bq_fg_chip *bq)
{
	return fg_nvt_read_byte(bq, NVT_FG_REG_OVER_PEAK, "OVER_PEAK");
}

static void fg_get_isc_alert_level(struct bq_fg_chip *bq, int *level)
{
	u8 isc_alert_level = 0;
	int ret;

	if (bq->fake_isc)
		return;

	if (bq->regs[NVT_FG_REG_ISC] == INVALID_REG_ADDR) {
		mca_log_info("ISC command not supported!\n");
		return;
	}

	ret = fg_read_byte(bq, bq->regs[NVT_FG_REG_ISC], &isc_alert_level);
	if (ret < 0)
		mca_log_info("could not read ISC, ret=%d\n", ret);

	*level = isc_alert_level;
}

static void fg_set_isc_alert_level(struct bq_fg_chip *bq, int value)
{
	bq->fake_isc = value;
	mca_log_info("set fake isc:: %d\n", bq->fake_isc);
}

static int fg_get_soa_alert_level(struct bq_fg_chip *bq, int *level)
{
	u8 soa_alert_level = 0;
	int ret;

	if (bq->regs[NVT_FG_REG_SOA_L] == INVALID_REG_ADDR) {
		mca_log_info("SOA command not supported!\n");
		return 0;
	}

	ret = fg_read_byte(bq, bq->regs[NVT_FG_REG_SOA_L], &soa_alert_level);
	if (ret < 0) {
		mca_log_info("could not read SOA, ret=%d\n", ret);
		return ret;
	}

	*level = soa_alert_level;

	return 0;
}

static int fg_get_current_dev(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_REG_CUR_DEV, "CURR_DEV");
}

static int fg_get_power_dev(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_REG_POW_DEV, "POW_DEV");
}

static int fg_get_average_current(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_AVE_CUR, "AVE_CUR");
}

static int fg_get_average_temp(struct bq_fg_chip *bq)
{
	u16 temp;

	if (bq->regs[NVT_FG_AVE_TEMP] == INVALID_REG_ADDR) {
		mca_log_info("AVE_TEMP command not supported!\n");
		return 0;
	}

	temp = fg_nvt_read_word(bq, NVT_FG_AVE_TEMP, "AVE_TEMP");

	return (u16)(temp - 2730);
}

static int fg_get_start_learning(struct bq_fg_chip *bq)
{
	return fg_nvt_read_byte(bq, NVT_FG_REG_START_LEARNING, "START_LEARNING");
}

static int fg_get_stop_learning(struct bq_fg_chip *bq)
{
	return fg_nvt_read_byte(bq, NVT_FG_REG_STOP_LEARNING, "STOP_LEARNING");
}

static int fg_get_actual_power(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_REG_ACT_POWER, "ACT_POWER");
}

static int fg_get_learning_power(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_REG_EST_POWER, "EST_POWER");
}

static int fg_get_learning_power_dev(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_REG_POWER_DEV, "POWER_DEV");
}

static int fg_get_learning_time_dev(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_REG_TIME_DEV, "TIME_DEV");
}

static int fg_get_constant_power(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_REG_CONST_POWER, "CONST_POWER");
}

static int fg_get_remaining_time(struct bq_fg_chip *bq)
{
	return fg_nvt_read_word(bq, NVT_FG_REG_CONST_POWER_TM, "POWER_TM");
}

static int fg_get_referance_power(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_REG_REF_POWER, "REF_POWER");
}

static int fg_get_nvt_referance_power(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_REG_NVT_REF_CURRENT,
				      "NVT_REF_CURR");
}

static int fg_get_referance_current(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_REG_REF_CURRENT, "REF_CURR");
}

static int fg_get_start_learning_b(struct bq_fg_chip *bq)
{
	return fg_nvt_read_byte(bq, NVT_FG_REG_START_LEARNING_B,
				"START_LEARNING_B");
}

static int fg_get_stop_learning_b(struct bq_fg_chip *bq)
{
	return fg_nvt_read_byte(bq, NVT_FG_REG_STOP_LEARNING_B,
				"STOP_LEARNING_B");
}

static int fg_get_learning_power_b(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_REG_EST_POWER_B, "EST_POWER_B");
}

static int fg_get_actual_power_b(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_REG_ACT_POWER_B, "ACT_POWER_B");
}

static int fg_get_learning_power_dev_b(struct bq_fg_chip *bq)
{
	return (u16)-fg_nvt_read_word(bq, NVT_FG_REG_POWER_DEV_B,
				      "POWER_DEV_B");
}

/*
 * Health and lifetime records
 */
static void fg_get_mac_u16(struct bq_fg_chip *bq, u16 cmd, u8 len, int off,
			   int *val)
{
	u8 t_buf[64] = { 0 };

	if (fg_mac_read_block(bq, cmd, t_buf, len) < 0)
		return;

	*val = get_unaligned_le16(&t_buf[off]);
}

static void fg_get_rel_soh(struct bq_fg_chip *bq, int *val)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_REL_SOH, 2, 0, val);
}

static void fg_get_rel_soh_cyclecount(struct bq_fg_chip *bq, int *val)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_REL_SOH, 4, 2, val);
}

static void fg_get_eis_soh(struct bq_fg_chip *bq, int *val)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_EIS_SOH, 4, 0, val);
}

static void fg_get_eis_soh_cyclecount(struct bq_fg_chip *bq, int *val)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_EIS_SOH, 4, 2, val);
}

static void fg_get_qmax_cyclecount(struct bq_fg_chip *bq, int *val)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_QMAX_CYCLECOUNT, 12, 10, val);
}

#define FG_BATT_USE_ENV_LEN	38

static int fg_get_batt_use_environment(struct bq_fg_chip *bq, u8 *env)
{
	u8 t_buf[64] = { 0 };
	u16 cmd;
	int ret;

	if (bq->fg_vendor == MPC7012)
		cmd = FG_MAC_CMD_MPC7012_CALC_RVALUE;
	else
		cmd = FG_MAC_CMD_CALC_RVALUE;
	ret = fg_mac_read_block(bq, cmd, t_buf, 32);
	if (ret < 0)
		return ret;
	memcpy(env, t_buf, 32);

	memset(t_buf, 0, sizeof(t_buf));
	if (bq->fg_vendor == MPC7012)
		cmd = FG_MAC_CMD_MPC7012_OVER_VOL_DURATION;
	else
		cmd = FG_MAC_CMD_OVER_VOL_DURATION;
	ret = fg_mac_read_block(bq, cmd, t_buf, 6);
	if (ret < 0)
		return ret;
	memcpy(&env[32], t_buf, 6);

	return 0;
}

static void fg_get_max_life_vol(struct bq_fg_chip *bq, int *val)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_LIFETIME1, 2, 0, val);
}

static void fg_get_min_life_vol(struct bq_fg_chip *bq, int *val)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_LIFETIME3, 20, 18, val);
}

static void fg_get_max_life_temp(struct bq_fg_chip *bq, u8 *val)
{
	u8 t_buf[64] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_LIFETIME1, t_buf, 8) < 0)
		return;

	*val = t_buf[6];
}

static void fg_get_min_life_temp(struct bq_fg_chip *bq, u8 *val)
{
	u8 t_buf[64] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_LIFETIME1, t_buf, 8) < 0)
		return;

	*val = t_buf[7];
}

static void fg_get_over_vol_duration(struct bq_fg_chip *bq, int *val)
{
	u8 t_buf[64] = { 0 };
	u16 cmd;

	if (bq->fg_vendor == MPC7012)
		cmd = FG_MAC_CMD_MPC7012_OVER_VOL_DURATION;
	else
		cmd = FG_MAC_CMD_OVER_VOL_DURATION;

	if (fg_mac_read_block(bq, cmd, t_buf, 12) < 0)
		return;

	*val = get_unaligned_le32(&t_buf[8]);
}

static void fg_get_csd_flag(struct bq_fg_chip *bq, int *val)
{
	u8 t_buf[64] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_CSD, t_buf, 8) < 0)
		return;

	*val = t_buf[1];
}

static void fg_get_csd_r1(struct bq_fg_chip *bq, int *val)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_CSD, 8, 2, val);
}

static void fg_get_csd_r2(struct bq_fg_chip *bq, int *val)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_CSD, 8, 4, val);
}

static void fg_get_dcr_slope1(struct bq_fg_chip *bq, int *val)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_DCR_SLOPE, 12, 4, val);
}

static void fg_get_dcr_slope2(struct bq_fg_chip *bq, int *val)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_DCR_SLOPE, 12, 6, val);
}

static void fg_get_dcr_slope3(struct bq_fg_chip *bq, int *val)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_DCR_SLOPE, 12, 8, val);
}

static void fg_get_charge_absoc(struct bq_fg_chip *bq, int *val)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_QMAX_CYCLECOUNT, 8, 2, val);
}

static void fg_get_max_dsg_current(struct bq_fg_chip *bq)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_LIFETIME1, 8, 4, &bq->max_dsg_current);
}

static void fg_get_min_temp(struct bq_fg_chip *bq)
{
	u8 t_buf[64] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_LIFETIME1, t_buf, 8) < 0)
		return;

	bq->min_temp = t_buf[7];
}

static void fg_get_min_voltage(struct bq_fg_chip *bq)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_MIN_VOLTAGE, 8, 6, &bq->min_voltage);
}

static void fg_get_cuv_count(struct bq_fg_chip *bq)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_CUV_OCD, 32, 16, &bq->cuv_count);
}

static void fg_get_cuv_last_cycle(struct bq_fg_chip *bq)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_CUV_OCD, 32, 18, &bq->cuv_last_cycle);
}

static void fg_get_hcuv_count(struct bq_fg_chip *bq)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_HCUV_HOCD, 8, 4, &bq->hcuv_count);
}

static void fg_get_hcuv_last_cycle(struct bq_fg_chip *bq)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_HCUV_HOCD, 8, 6, &bq->hcuv_last_cycle);
}

static void fg_get_ocd_count(struct bq_fg_chip *bq)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_CUV_OCD, 32, 28, &bq->ocd_count);
}

static void fg_get_ocd_last_cycle(struct bq_fg_chip *bq)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_CUV_OCD, 32, 30, &bq->ocd_last_cycle);
}

static void fg_get_hocd_count(struct bq_fg_chip *bq)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_HCUV_HOCD, 32, 12, &bq->hocd_count);
}

static void fg_get_hocd_last_cycle(struct bq_fg_chip *bq)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_HCUV_HOCD, 32, 14, &bq->hocd_last_cycle);
}

static void fg_get_hscd_count(struct bq_fg_chip *bq)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_HCUV_HOCD, 32, 16, &bq->hscd_count);
}

static void fg_get_hscd_last_cycle(struct bq_fg_chip *bq)
{
	fg_get_mac_u16(bq, FG_MAC_CMD_HCUV_HOCD, 32, 18, &bq->hscd_last_cycle);
}

static void fg_dump_battery_abnormal_info(struct bq_fg_chip *bq)
{
	fg_get_max_dsg_current(bq);
	fg_get_min_temp(bq);
	fg_get_min_voltage(bq);
	fg_get_cuv_count(bq);
	fg_get_cuv_last_cycle(bq);
	fg_get_hcuv_count(bq);
	fg_get_hcuv_last_cycle(bq);
	fg_get_ocd_count(bq);
	fg_get_ocd_last_cycle(bq);
	fg_get_hocd_count(bq);
	fg_get_hocd_last_cycle(bq);
	fg_get_hscd_count(bq);
	fg_get_hscd_last_cycle(bq);

	mca_log_err("max_dsg_current = %hd, min_temp = %d, min_voltage = %d, cuv_count = %d, cuv_last_cycle = %d, hscd_count=%d, hscd_last_cycle=%d\n",
		    (short)bq->max_dsg_current, bq->min_temp, bq->min_voltage,
		    bq->cuv_count, bq->cuv_last_cycle, bq->hscd_count,
		    bq->hscd_last_cycle);
	mca_log_err("hcuv_count = %d, hcuv_last_cycle = %d, ocd_count = %d, ocd_last_cycle = %d, hocd_count = %d, hocd_last_cycle = %d\n",
		    bq->hcuv_count, bq->hcuv_last_cycle, bq->ocd_count,
		    bq->ocd_last_cycle, bq->hocd_count, bq->hocd_last_cycle);
}

static void fg_read_co_auto_open(struct bq_fg_chip *bq, int *co_auto_open)
{
	u8 t_buf[2] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_CO_AUTO_OPEN, t_buf, 2) < 0)
		mca_log_err("Failed to read Co Atuo Open\n");

	*co_auto_open = (t_buf[1] >> 2) & 1;
}

static int fg_read_status(struct bq_fg_chip *bq)
{
	u16 flags = 0;
	int ret;

	ret = fg_read_word(bq, bq->regs[BQ_FG_REG_BATT_STATUS], &flags);
	if (ret < 0)
		return ret;

	bq->batt_fc = !!(flags & FG_FLAGS_FC);
	bq->batt_fd = !!(flags & FG_FLAGS_FD);
	bq->batt_rca = !!(flags & FG_FLAGS_RCA);
	bq->batt_dsg = !!(flags & FG_FLAGS_DSG);

	return 0;
}

/*
 * Sealing
 */
static int fg_unseal_send_key_for_ti(struct bq_fg_chip *bq)
{
	u8 cmd[2] = { 0x2e, 0x00 };
	u16 value;
	int ret;

	value = (bq->key[2] << 8) | bq->key[3];
	ret = fg_write_word(bq, bq27z561_regs[BQ_FG_REG_ALT_MAC], value);
	if (ret < 0) {
		mca_log_err(" step 1 first word write fail");
		return ret;
	}

	msleep(4000);

	ret |= fg_write_word(bq, bq27z561_regs[BQ_FG_REG_ALT_MAC], value);
	if (ret < 0) {
		mca_log_err("step 2 first word write fail");
		return ret;
	}

	usleep_range(5000, 5100);

	value = (bq->key[0] << 8) | bq->key[1];
	ret = fg_write_word(bq, bq27z561_regs[BQ_FG_REG_ALT_MAC], value);
	if (ret < 0) {
		mca_log_err(" step 3 second word write fail");
		return ret;
	}

	ret |= fg_write_word(bq, bq27z561_regs[BQ_FG_REG_ALT_MAC], value);
	if (ret < 0) {
		mca_log_err("step 4 second word write fail");
		return ret;
	}

	msleep(200);
	mca_log_err("fg sunseal success");

	ret |= fg_write_block(bq, bq27z561_regs[BQ_FG_REG_ALT_MAC], cmd, 2);
	if (ret < 0) {
		mca_log_err("step 5 data flush failed,");
		return ret;
	}

	return ret;
}

static int fg_unseal_send_key_for_nvt(struct bq_fg_chip *bq)
{
	u16 value;
	int ret;

	value = (bq->key[1] << 8) | bq->key[0];
	ret = fg_write_word(bq, bq27z561_regs[BQ_FG_REG_ALT_MAC], value);
	if (ret < 0) {
		mca_log_err(" step 1 first word write fail");
		return ret;
	}

	msleep(300);

	value = (bq->key[3] << 8) | bq->key[2];
	ret = fg_write_word(bq, bq27z561_regs[BQ_FG_REG_ALT_MAC], value);
	if (ret < 0) {
		mca_log_err(" step 2 second word write fail");
		return ret;
	}

	msleep(300);
	mca_log_err("fg unseal success");

	return ret;
}

#define SEAL_RETRY_MAX	3

static int fg_set_seal(struct bq_fg_chip *bq, int value)
{
	int seal_status = 0;
	int count;

	if (value) {
		fg_get_seal(bq, &seal_status);
		if (seal_status == SEAL_STATE_FA ||
		    seal_status == SEAL_STATE_UNSEALED) {
			mca_log_info("FG is unsealed");
			return 0;
		}

		for (count = 0; count < SEAL_RETRY_MAX; count++) {
			switch (bq->fg_vendor) {
			case NFG1000A:
			case NFG1000B:
			case MPC8011B:
			case MPC7012:
				fg_unseal_send_key_for_nvt(bq);
				break;
			case BQ27Z561:
				fg_unseal_send_key_for_ti(bq);
				break;
			default:
				mca_log_info("unseal don't support for venddor %d\n",
					     bq->fg_vendor);
				return 0;
			}

			fg_get_seal(bq, &seal_status);
			if (seal_status == SEAL_STATE_FA ||
			    seal_status == SEAL_STATE_UNSEALED) {
				mca_log_info("FG is unsealed");
				return 0;
			}
			msleep(100);
		}
	} else {
		for (count = 0; count < SEAL_RETRY_MAX; count++) {
			if (fg_write_word(bq, bq->regs[BQ_FG_REG_ALT_MAC],
					  FG_MAC_CMD_SEAL) < 0)
				mca_log_err("Failed to send seal command");

			seal_status = 0;
			fg_get_seal(bq, &seal_status);
			if (seal_status == SEAL_STATE_SEALED) {
				mca_log_info("FG is sealed");
				return 0;
			}
			msleep(100);
		}
	}

	return 0;
}

/*
 * Identification
 */
static int fg_get_manu_info(unsigned char val, int base, int step)
{
	int index = 0;

	mca_log_info("val:%d, '0':%d, 'A':%d, 'a':%d\n", val, '0', 'A', 'a');
	if (val > '0' && val < '9')
		index = val - '0';
	if (val > 'A' && val < 'Z')
		index = val - 'A' + 10;
	if (val > 'a' && val < 'z')
		index = val - 'a' + 36;

	mca_log_info("base:%d, index:%d, step:%d\n", base, index, step);

	return base + index * step;
}

static void fg_get_manufacture_data(struct bq_fg_chip *bq)
{
	static const struct {
		u8 byte;
		int base;
		int step;
	} manu_info[] = {
		{ 6, 30, 5 },		/* termination current */
		{ 7, 4200, 5 },		/* recharge voltage */
		{ 8, 400, 20 },		/* ffc termination current */
		{ 3, 0x0C, 1 },		/* manufacturer */
	};
	u8 t_buf[40] = { 0 };
	int i;

	if (fg_mac_read_block(bq, FG_MAC_CMD_MANU_NAME, t_buf, 32) < 0) {
		mca_log_info("failed to get MANE NAME\n");
		return;
	}

	if (strncmp(t_buf, "MI", 2)) {
		mca_log_info("Can not get MI battery data\n");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(manu_info); i++)
		mca_log_info("manu_info[%d].data %d\n", i,
			     fg_get_manu_info(t_buf[manu_info[i].byte],
					      manu_info[i].base,
					      manu_info[i].step));
}

static void fg_read_cell_vendor(struct bq_fg_chip *bq)
{
	static const char * const cell_names[] = {
		"samsung", "sony", "sanyo", "lg", "panasonic", "atl",
		"byd", "tws", "lishen", "coslight", "default", "lwn",
	};
	u8 t_buf[40] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_CHEM_NAME, t_buf, 4) < 0) {
		mca_log_info("failed to get MANE NAME\n");
		return;
	}
	mca_log_info("%s\n", t_buf);

	if (t_buf[1] >= 'A' && t_buf[1] - 'A' < ARRAY_SIZE(cell_names))
		bq->cell_name = cell_names[t_buf[1] - 'A'];
	else
		bq->cell_name = "default";
}

static void fg_read_pack_vendor(struct bq_fg_chip *bq)
{
	u8 t_buf[40] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_CHEM_NAME, t_buf, 4) < 0) {
		mca_log_info("failed to get MANE NAME\n");
		return;
	}
	mca_log_info("%s\n", t_buf);

	switch (t_buf[2]) {
	case 'B':
		bq->pack_vendor = PACK_SUPPLIER_BYD;
		break;
	case 'C':
		bq->pack_vendor = PACK_SUPPLIER_COSLIGHT;
		break;
	case 'S':
		bq->pack_vendor = PACK_SUPPLIER_SUNWODA;
		break;
	case 'U':
		bq->pack_vendor = PACK_SUPPLIER_SCUD;
		break;
	case 'T':
		bq->pack_vendor = PACK_SUPPLIER_TWS;
		break;
	case 'I':
		bq->pack_vendor = PACK_SUPPLIER_LISHEN;
		break;
	case 'K':
		bq->pack_vendor = PACK_SUPPLIER_DESAY;
		break;
	default:
		bq->pack_vendor = PACK_SUPPLIER_NVT;
		break;
	}
}

static void fg_read_vendor(struct bq_fg_chip *bq)
{
	u8 t_buf[32] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_MANU_NAME, t_buf, 32) < 0)
		return;

	mca_log_info("fg_vendor = %d", t_buf[2]);

	switch (t_buf[2]) {
	case '4':
		bq->fg_vendor = NFG1000B;
		break;
	case '5':
		bq->fg_vendor = NFG1000A;
		break;
	case '7':
		bq->fg_vendor = MPC7012;
		break;
	case 'C':
		bq->fg_vendor = BQ27Z561;
		break;
	default:
		bq->fg_vendor = MPC8011B;
		break;
	}
}

#define FG_FW_R0	0x0001
#define FG_FW_R1	0x0102

static void fg_read_fw_version(struct bq_fg_chip *bq)
{
	u8 t_buf[4] = { 0 };
	int value;

	if (fg_mac_read_block(bq, FG_MAC_CMD_FW_VER, t_buf, 4) < 0)
		return;

	value = (t_buf[2] << 8) | t_buf[3];
	if (value == FG_FW_R0)
		bq->fw_ver = FW_R0;
	else if (value == FG_FW_R1)
		bq->fw_ver = FW_R1;
	else
		bq->fw_ver = FW_DEFAULT;
}

static void fg_read_eeprom_version(struct bq_fg_chip *bq)
{
	u8 t_buf[4] = { 0 };

	if (fg_mac_read_block(bq, FG_MAC_CMD_CHEM_NAME, t_buf, 4) < 0) {
		mca_log_info("failed to get MANE NAME\n");
		return;
	}
	mca_log_info("%c", t_buf[3]);

	bq->eeprom_version = t_buf[3];
}

static void fg_get_ui_soh(void *data, int *ui_soh)
{
	struct bq_fg_chip *bq = data;

	*ui_soh = bq->ui_soh;
}

static void fg_store_ui_soh(struct bq_fg_chip *bq, const char *buf, size_t size)
{
	u8 data_uisoh[11] = { 0 };
	u8 data[32] = { 0 };
	char *tmp_buf, *str, *pchar;
	unsigned int temp;
	int count = 0;

	tmp_buf = kzalloc(size + 1, GFP_KERNEL);
	if (!tmp_buf)
		return;

	strscpy(tmp_buf, buf, size + 1);
	mca_log_info("%s\n", tmp_buf);

	str = tmp_buf;
	while (count < ARRAY_SIZE(data_uisoh)) {
		pchar = strsep(&str, " ");
		if (!pchar)
			break;
		if (kstrtouint(pchar, 0, &temp)) {
			mca_log_err("pchar cover to int fail\n");
			break;
		}
		data_uisoh[count] = (u8)temp;
		mca_log_info("data_uisoh[%d]=%d\n", count, data_uisoh[count]);
		count++;
	}

	bq->ui_soh = data_uisoh[0];

	if (fg_mac_read_block(bq, FG_MAC_CMD_UI_SOH, data, 32) < 0) {
		mca_log_info("failed to get first_usage\n");
		goto out;
	}

	memcpy(data, data_uisoh, sizeof(data_uisoh));
	if (fg_mac_write_block(bq, FG_MAC_CMD_UI_SOH, data, 32) < 0)
		mca_log_err("write ui_soh failed\n");

out:
	kfree(tmp_buf);
}

/*
 * Periodic update
 */
static void fg_update_aged_flag(struct bq_fg_chip *bq)
{
	int cc = 0;

	fg_read_cyclecount(bq, &cc);
	if (cc > (int)bq->cycle_th) {
		bq->aged_flag = 0;
		bq->soh_th -= 1;
		bq->cycle_th += bq->cycle_step;
	}

	mca_log_info("ui_soh %d soh_th %d\n", bq->ui_soh, bq->soh_th);
	if (bq->ui_soh < bq->soh_th)
		bq->aged_flag = 1;
}

static int fg_read_lifetime_block(struct bq_fg_chip *bq, u16 cmd, u8 *buf)
{
	int ret, retry;

	for (retry = 1; retry <= 2; retry++) {
		ret = fg_mac_read_block(bq, cmd, buf, 32);
		if (ret >= 0)
			return ret;
		if (cmd == FG_MAC_CMD_LIFETIME1)
			mca_log_info("the address FG_MAC_CMD_LIFETIME1 is error, retry:%d.\n",
				     retry);
		else
			mca_log_info("the address FG_MAC_CMD_LIFETIME3 is error, retry:%d.\n",
				     retry);
	}

	return ret;
}

static void fg_update_lifetime_data(struct bq_fg_chip *bq)
{
	u8 t_buf[40] = { 0 };

	if (fg_read_lifetime_block(bq, FG_MAC_CMD_LIFETIME1, t_buf) < 0)
		return;

	bq->cell1_max = get_unaligned_le16(&t_buf[0]);
	bq->max_charge_current = get_unaligned_le16(&t_buf[2]);
	bq->max_discharge_current = (s16)get_unaligned_le16(&t_buf[4]);
	bq->max_temp_cell = t_buf[6];
	bq->min_temp_cell = t_buf[7];

	memset(t_buf, 0, sizeof(t_buf));
	if (fg_read_lifetime_block(bq, FG_MAC_CMD_LIFETIME3, t_buf) < 0)
		return;

	bq->total_fw_runtime = get_unaligned_le16(&t_buf[0]);
	bq->time_spent_in_lt = get_unaligned_le16(&t_buf[4]);
	bq->time_spent_in_ht = get_unaligned_le16(&t_buf[12]);
	bq->time_spent_in_ot = get_unaligned_le16(&t_buf[14]);
}

static void fg_get_gague_mode(struct bq_fg_chip *bq)
{
	u8 data[4] = { 0 };
	int ret;

	ret = fg_mac_read_block(bq, FG_MAC_CMD_GAGUE_STATUS, data, 4);
	if (ret < 0) {
		mca_log_info("could not write fastcharge = %d\n", ret);
		return;
	}

	bq->batt_fd_1 = !!(data[0] & BIT(0));
	bq->batt_fc_1 = !!(data[0] & BIT(1));
	bq->batt_td_1 = !!(data[0] & BIT(2));
	bq->batt_tc_1 = !!(data[0] & BIT(3));
	mca_log_info("gague status FD %d, FC = %d, TD %d TC %d\n",
		     bq->batt_fd_1, bq->batt_fc_1, bq->batt_td_1, bq->batt_tc_1);
}

#define FG_RECORD_RETRY_MAX	3

/*
 * Some packs need the voltage levels the gauge records at rewritten once,
 * with a version byte marking that it was done.  This runs once per boot.
 */
static void fg_update_record_voltage_level(struct bq_fg_chip *bq)
{
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	u8 address = bq->start_byte_address;
	u8 t_buf[36] = { 0 };
	int seal_status = 0;
	int ret, count, i;
	bool mismatch;

	if (hwid && hwid->platform_version == HARDWARE_PROJECT_O9 &&
	    hwid->country_version != 0)
		bq->support_voltage_record_level = false;

	if (!bq->support_voltage_record_level || bq->fg_vendor != MPC8011B) {
		mca_log_err("can't support ota\n");
		bq->update_fw_status = FG_FW_INVALID;
		return;
	}

	if (address + bq->byte_length > sizeof(t_buf) ||
	    bq->byte_length > bq->record_voltage_len) {
		mca_log_err("can't support ota\n");
		bq->update_fw_status = FG_FW_INVALID;
		return;
	}

	fg_mac_read_block(bq, FG_MAC_CMD_CHEM_NAME, t_buf, 6);
	if (t_buf[3] >= bq->version_number) {
		mca_log_info("it has been upteded version_number[0x%x][0x%x]\n",
			     t_buf[3], bq->version_number);
		bq->update_fw_status = FG_FW_SUCCESS;
		return;
	}

	fg_set_seal(bq, true);
	fg_get_seal(bq, &seal_status);
	if (seal_status != SEAL_STATE_UNSEALED && seal_status != SEAL_STATE_FA) {
		mca_log_info("Fg Unseal Failed\n");
		bq->update_fw_status = FG_FW_ERROR;
		return;
	}

	for (count = 1; count <= FG_RECORD_RETRY_MAX; count++) {
		memset(t_buf, 0, sizeof(t_buf));
		ret = fg_mac_read_block(bq, FG_MAC_CMD_RECORD_VOLTAGE_LEVEL, t_buf, 32);
		if (ret < 0) {
			mca_log_err("could not read record_voltage_level, ret=%d\n", ret);
			msleep(100);
			continue;
		}

		for (i = 0; i < bq->byte_length; i++) {
			t_buf[address + i] = bq->record_voltage[i];
			mca_log_info("Reg:t_buf[%d]=[0x%x]\n", address + i,
				     t_buf[address + i]);
		}

		ret = fg_mac_write_block(bq, FG_MAC_CMD_RECORD_VOLTAGE_LEVEL, t_buf, 32);
		if (ret < 0) {
			mca_log_err("could not write record_voltage_level, ret=%d\n", ret);
			msleep(100);
			continue;
		}

		msleep(100);
		memset(t_buf, 0, sizeof(t_buf));
		ret = fg_mac_read_block(bq, FG_MAC_CMD_RECORD_VOLTAGE_LEVEL, t_buf, 32);
		if (ret < 0) {
			mca_log_err("could not read record_voltage_level, ret=%d\n", ret);
			msleep(100);
			continue;
		}

		mismatch = false;
		for (i = 0; i < bq->byte_length; i++) {
			if (t_buf[address + i] != bq->record_voltage[i]) {
				mismatch = true;
				break;
			}
			mca_log_info("Update:t_buf[%d]=[0x%x]\n", address + i,
				     t_buf[address + i]);
		}

		if (mismatch) {
			mca_log_err("Modify record_voltage_level Error\n");
			msleep(100);
			continue;
		}

		mca_log_info("Modify record_voltage_level Suceess\n");
		break;
	}

	/* Only a success before the last attempt counts. */
	if (count >= FG_RECORD_RETRY_MAX) {
		bq->update_fw_status = FG_FW_ERROR;
		goto seal;
	}

	for (count = 1; count <= FG_RECORD_RETRY_MAX; count++) {
		memset(t_buf, 0, sizeof(t_buf));
		fg_mac_read_block(bq, FG_MAC_CMD_UPDATE_VERSION, t_buf, 32);
		t_buf[3] = bq->version_number;
		ret = fg_mac_write_block(bq, FG_MAC_CMD_UPDATE_VERSION, t_buf, 32);
		if (ret < 0) {
			mca_log_err("could not write update_version, ret=%d\n", ret);
			msleep(100);
			continue;
		}

		memset(t_buf, 0, sizeof(t_buf));
		ret = fg_mac_read_block(bq, FG_MAC_CMD_UPDATE_VERSION, t_buf, 32);
		if (ret < 0) {
			mca_log_err("could not read update_version, ret=%d\n", ret);
			msleep(100);
			continue;
		}

		if (t_buf[3] != bq->version_number) {
			mca_log_err("Modify update_version Error\n");
			msleep(100);
			continue;
		}

		mca_log_info("Modify update_version Sucess\n");
		break;
	}

	if (count >= FG_RECORD_RETRY_MAX)
		bq->update_fw_status = FG_FW_ERROR;
	else
		bq->update_fw_status = FG_FW_SUCCESS;

seal:
	fg_set_seal(bq, false);
}

static void fg_update_data_work(void *data)
{
	struct bq_fg_chip *bq = data;
	int chip_ok = 0;

	fg_get_chip_ok(bq, &chip_ok);
	if (!chip_ok)
		return;

	fg_update_aged_flag(bq);
	fg_update_lifetime_data(bq);
	fg_get_gague_mode(bq);

	if (bq->update_fw_status == FG_FW_INIT)
		fg_update_record_voltage_level(bq);
}

/*
 * Battery authentication
 */
static int StringToHex(char *str, unsigned char *out, unsigned int *outlen)
{
	char *p = str;
	char high, low;
	int tmplen, cnt = 0;

	tmplen = strlen(p);
	while (cnt < (tmplen / 2)) {
		high = ((*p > '9') && (*p <= 'f')) ? *p - 48 - 7 : *p - 48;
		p++;
		low = ((*p > '9') && (*p <= 'f')) ? *p - 48 - 7 : *p - 48;
		out[cnt] = ((high & 0x0f) << 4) | (low & 0x0f);
		p++;
		cnt++;
	}
	if (tmplen % 2 != 0)
		out[cnt] = ((*p > '9') && (*p <= 'f')) ? *p - 48 - 7 : *p - 48;

	if (outlen)
		*outlen = tmplen / 2 + tmplen % 2;

	return tmplen / 2 + tmplen % 2;
}

/*
 * SHA-256 challenge: the host writes 0x0000 and the 32-byte challenge to
 * AltManufacturerAccess/MACData with its checksum and length, waits for the
 * gauge to compute, and reads the 32-byte digest back from MACData.
 */
static void fg_sha256_auth(struct bq_fg_chip *bq, u8 *rand_num, int length)
{
	u8 t_buf[36] = { 0 };
	int ret;

	t_buf[0] = 0x00;
	t_buf[1] = 0x00;
	memcpy(&t_buf[2], rand_num, length);
	t_buf[2 + length] = checksum(rand_num, length);
	t_buf[3 + length] = 0x24;

	atomic_set(&bq->digest_in_process, 1);
	ret = fg_write_block(bq, bq->regs[BQ_FG_REG_ALT_MAC], t_buf, length + 4);
	if (ret < 0) {
		mca_log_err("batt_auth write BQ_FG_REG_ALT_MAC fail\n");
		goto out;
	}

	msleep(400);
	ret = fg_read_block(bq, bq->regs[BQ_FG_REG_MAC_DATA], bq->digest,
			    BATTERY_DIGEST_LEN);
	if (ret < 0)
		mca_log_err("batt_auth read verify_digest fail\n");

out:
	atomic_set(&bq->digest_in_process, 0);
}

static int fg_set_verify_digest(void *data, char *buf)
{
	struct bq_fg_chip *bq = data;
	u8 random[BATTERY_DIGEST_LEN] = { 0 };
	char kbuf[BATTERY_RANDOM_LEN + 1] = { 0 };

	if (!bq)
		return 0;

	strscpy(kbuf, buf, sizeof(kbuf));
	StringToHex(kbuf, random, NULL);
	fg_sha256_auth(bq, random, BATTERY_DIGEST_LEN);

	return 0;
}

static int fg_get_verify_digest(void *data, char *buf)
{
	struct bq_fg_chip *bq = data;
	char digest_buf[4];
	int i;

	if (!bq)
		return 0;

	for (i = 0; i < BATTERY_DIGEST_LEN; i++) {
		memset(digest_buf, 0, sizeof(digest_buf));
		snprintf(digest_buf, sizeof(digest_buf) - 1, "%02x",
			 bq->digest[i]);
		strlcat(buf, digest_buf, BATTERY_DIGEST_LEN * 2 + 1);
	}

	mca_log_info("buf = %s\n", buf);

	return strlen(buf) + 1;
}

static int fg_set_authentic(void *data, int value)
{
	struct bq_fg_chip *bq = data;

	bq->batt_auth = value;

	return 0;
}

static int fg_get_authentic(void *data, int *value)
{
	struct bq_fg_chip *bq = data;

	*value = bq->batt_auth;

	return 0;
}

/*
 * Charge outputs
 */
static int fg_set_fastcharge_mode(struct bq_fg_chip *bq, bool enable)
{
	u8 data[2] = { 0 };
	int ret;

	if (bq->fast_mode == enable)
		return 0;

	if (atomic_read(&bq->digest_in_process)) {
		mca_log_info("wait fg auth to be done\n");
		return 0;
	}

	data[0] = enable;
	mca_log_info("set fastcharge mode: enable: %d\n", enable);
	ret = fg_mac_write_block(bq, enable ? FG_MAC_CMD_FASTCHARGE_EN :
				 FG_MAC_CMD_FASTCHARGE_DIS, data, 2);
	if (ret < 0) {
		mca_log_info("could not write fastcharge = %d\n", ret);
		return ret;
	}

	bq->fast_mode = enable;

	return ret;
}

static int fg_set_one_fastcharge_mode(void *data, bool enable)
{
	return fg_set_fastcharge_mode(data, enable);
}

static int fg_get_one_fastcharge_mode(void *data, int *ffc)
{
	struct bq_fg_chip *bq = data;

	*ffc = bq->fast_mode;

	return 0;
}

static int fg_read_word_reg(struct bq_fg_chip *bq, int idx, int *val)
{
	u16 data = 0;
	int ret;

	ret = fg_read_word(bq, bq->regs[idx], &data);
	if (ret < 0) {
		mca_log_info("could not read Time To Empty, ret=%d\n", ret);
		return ret;
	}

	if (ret == 0xFFFF)
		return -ENODATA;

	*val = data;

	return 0;
}

static int fg_read_one_charging_voltage(void *data, int *volt)
{
	struct bq_fg_chip *bq = data;

	if (bq->regs[BQ_FG_REG_CHG_VOL] == INVALID_REG_ADDR) {
		mca_log_info(" not supported!\n");
		return -1;
	}

	return fg_read_word_reg(bq, BQ_FG_REG_CHG_VOL, volt);
}

static int fg_read_one_tte(void *data, int *tte)
{
	struct bq_fg_chip *bq = data;

	if (bq->regs[BQ_FG_REG_TTE] == INVALID_REG_ADDR) {
		mca_log_info("Time To Empty not supported!\n");
		return -1;
	}

	return fg_read_word_reg(bq, BQ_FG_REG_TTE, tte);
}

static int fg_read_one_ttf(void *data, int *ttf)
{
	struct bq_fg_chip *bq = data;

	if (bq->regs[BQ_FG_REG_TTE] == INVALID_REG_ADDR) {
		mca_log_info("Time To Empty not supported!\n");
		return -1;
	}

	return fg_read_word_reg(bq, BQ_FG_REG_TTF, ttf);
}

static void fg_set_force_report_full(void *data)
{
	struct bq_fg_chip *bq = data;
	u8 t_buf[2] = { 0x31, 0x00 };

	if (fg_write_block(bq, bq->regs[BQ_FG_REG_ALT_MAC], t_buf, 2) < 0) {
		mca_log_err("set force_report_full fail\n");
		return;
	}
	mca_log_info("set force_report_full success\n");
}

/*
 * Charge-off (CO) control.  The gauge is told to close or open the pack's
 * charge FET and the CO bit in the seal-state word is read back until it
 * agrees, giving it a second and then three more tries a tenth apart.
 */
static int fg_toggle_co(struct bq_fg_chip *bq, u8 co_cmd, int expect, u8 *data)
{
	u8 flag = 0;
	int co_state, retry, ret;

	ret = fg_mac_write_block(bq, co_cmd, data, 4);
	if (ret < 0)
		mca_log_err("CO command: 0x%x failed\n", co_cmd);

	msleep(1100);
	if (fg_mac_read_block(bq, FG_MCA_CMD_SEAL_STATE, &flag, 1) < 0)
		mca_log_err("Failed to read CO state\n");
	co_state = !!(flag & FG_FLAGS_CO);
	mca_log_err("CO command: 0x%x, bit2: %d\n", co_cmd, co_state);
	if (co_state == (expect & 1))
		return 0;

	for (retry = 0; retry < 3; retry++) {
		msleep(100);
		if (fg_mac_read_block(bq, FG_MCA_CMD_SEAL_STATE, &flag, 1) < 0) {
			mca_log_info("Failed to read CO state during retry\n");
			continue;
		}
		co_state = !!(flag & FG_FLAGS_CO);
		if (co_state == (expect & 1)) {
			mca_log_err("CO command: 0x%x success, retry count: %d\n",
				    co_cmd, retry);
			return 0;
		}
	}

	return -1;
}

static int fg_set_co(struct bq_fg_chip *bq, bool value)
{
	u8 buf[4] = { 0xa1, 0xb2, 0xc3, 0xd4 };

	if (value) {
		if (fg_toggle_co(bq, FG_MAC_CMD_CLOSE_CO, 0, buf) >= 0)
			return 0;
		mca_log_err("Failed to close CO on first attempt\n");
		if (fg_toggle_co(bq, FG_MAC_CMD_CLOSE_CO, 0, buf) >= 0)
			return 0;
		mca_log_err("Failed to close CO on second attempt\n");
	} else {
		if (fg_toggle_co(bq, FG_MAC_CMD_OPEN_CO, 1, buf) >= 0)
			return 0;
		mca_log_err("Failed to open CO on first attempt\n");
		if (fg_toggle_co(bq, FG_MAC_CMD_OPEN_CO, 1, buf) >= 0)
			return 0;
		mca_log_err("Failed to open CO on second attempt\n");
	}

	return -1;
}

static int fg_set_co_by_gpio(struct bq_fg_chip *bq, bool value)
{
	int ret;

	ret = gpiod_direction_output_raw(gpio_to_desc(bq->ctl_co_gpio), value);
	if (ret)
		mca_log_err("Failed to set ctl_co_gpio[%d]\n", bq->ctl_co_gpio);

	return ret;
}

static int fg_set_one_co(void *data, bool value)
{
	struct bq_fg_chip *bq = data;

	if (bq->co_by_gpio)
		return fg_set_co_by_gpio(bq, value);

	return fg_set_co(bq, value);
}

/*
 * Operations for the gauge class
 */
static int fg_read_one_probe_ok(void *data, bool *ok)
{
	*ok = true;

	return 0;
}

static int fg_get_batt_info(struct bq_fg_chip *bq, struct fg_batt_info *info)
{
	u8 buf[32];
	int ret, i;

	for (i = 0; i < 5; i++) {
		if (!atomic_read(&bq->pm_suspend))
			break;
		mca_log_info("fg_get_batt_info wait resume\n");
		msleep(30);
	}

	/* Temperature (0x06) through InternalTemperature (0x1E) in one go */
	memset(buf, 0, sizeof(buf));
	ret = fg_read_block(bq, bq27z561_regs[BQ_FG_REG_TEMP], buf, sizeof(buf));
	if (ret < 0) {
		mca_log_err("could not read batt data, reg=0x%02x, ret=%d\n",
			    bq27z561_regs[BQ_FG_REG_TEMP], ret);
		return ret;
	}
	for (i = 0; i < sizeof(buf); i += 8)
		mca_log_debug("%02x regs[%d] = %02x%02x%02x%02x %02x%02x%02x%02x\n",
			      bq27z561_regs[BQ_FG_REG_TEMP], i, buf[i], buf[i + 1],
			      buf[i + 2], buf[i + 3], buf[i + 4], buf[i + 5],
			      buf[i + 6], buf[i + 7]);

	info->curr = fg_convert_bytes_to_curr(bq, get_unaligned_le16(&buf[0x0C - 0x06]));
	info->volt = fg_convert_bytes_to_volt(bq, get_unaligned_le16(&buf[0x08 - 0x06]));
	info->temp = fg_convert_bytes_to_temp(bq, get_unaligned_le16(&buf[0x06 - 0x06]));
	info->original_temp = fg_convert_bytes_to_original_temp(bq,
					get_unaligned_le16(&buf[0x1E - 0x06]));
	info->rm = fg_convert_bytes_to_rm(bq, get_unaligned_le16(&buf[0x10 - 0x06]));
	info->fcc = fg_convert_bytes_to_fcc(bq, get_unaligned_le16(&buf[0x12 - 0x06]));

	/* CycleCount (0x2A), RelativeStateOfCharge (0x2C), StateOfHealth (0x2E) */
	memset(buf, 0, sizeof(buf));
	ret = fg_read_block(bq, bq27z561_regs[BQ_FG_REG_CC], buf, 8);
	if (ret < 0) {
		mca_log_err("could not read batt data, reg=0x%02x, ret=%d\n",
			    bq27z561_regs[BQ_FG_REG_CC], ret);
		return ret;
	}
	mca_log_debug("%02x regs[%d] = %02x%02x%02x%02x %02x%02x%02x%02x\n",
		      bq27z561_regs[BQ_FG_REG_CC], 0, buf[0], buf[1], buf[2],
		      buf[3], buf[4], buf[5], buf[6], buf[7]);

	if (bq->fake_cycle)
		bq->batt_cycle_count = bq->fake_cycle;
	else if (!bq->ota_updating)
		bq->batt_cycle_count = get_unaligned_le16(&buf[0]);
	info->cycle_count = bq->batt_cycle_count;

	if (!bq->ota_updating)
		bq->batt_rsoc = get_unaligned_le16(&buf[2]);
	info->rsoc = bq->batt_rsoc;

	if (bq->fake_soh != FG_FAKE_SOH_NONE)
		bq->batt_soh = bq->fake_soh;
	else if (!bq->ota_updating)
		bq->batt_soh = get_unaligned_le16(&buf[4]);
	info->soh = bq->batt_soh;

	mca_log_debug("curr: %d, volt: %d, temp: %d, rsoc: %d, rm: %d, fcc: %d, cycle_count: %d, soh: %d, original_temp: %d\n",
		      info->curr, info->volt, info->temp, info->rsoc, info->rm,
		      info->fcc, info->cycle_count, info->soh,
		      info->original_temp);

	return 0;
}

static int fg_read_batt_info(void *data, void *info)
{
	return fg_get_batt_info(data, info);
}

static int fg_read_one_rsoc(void *data, int *rsoc)
{
	return fg_read_rsoc(data, rsoc);
}

static int fg_read_one_current(void *data, int *curr)
{
	return fg_read_current(data, curr);
}

static int fg_read_one_volt(void *data, int *volt)
{
	return fg_read_volt(data, volt);
}

static int fg_set_temperature(void *data, int value)
{
	struct bq_fg_chip *bq = data;

	if (bq) {
		bq->fake_temp = value;
		mca_log_err("set fake_temp: %d\n", bq->fake_temp);
	}

	return 0;
}

static int fg_read_one_temperature(void *data, int *temp)
{
	return fg_read_temperature(data, temp);
}

static int fg_read_one_original_temperature(void *data, int *temp)
{
	return fg_read_original_temperature(data, temp);
}

static int fg_read_one_status(void *data)
{
	return fg_read_status(data);
}

static int fg_read_one_rm(void *data, int *rm)
{
	return fg_read_rm(data, rm);
}

static int fg_get_one_chip_ok(void *data, int *ok)
{
	return fg_get_chip_ok(data, ok);
}

static int fg_read_one_cyclecount(void *data, int *cc)
{
	return fg_read_cyclecount(data, cc);
}

static int fg_read_one_fcc(void *data, int *fcc)
{
	return fg_read_fcc(data, fcc);
}

static int fg_read_one_dc(void *data, int *dc)
{
	return fg_read_dc(data, dc);
}

static int fg_get_one_soc_decimal_rate(void *data, int *rate)
{
	struct bq_fg_chip *bq = data;
	int soc = 0;
	int i;

	if (bq->dec_rate_len <= 0)
		return 0;

	if (fg_read_rsoc(bq, &soc))
		return -1;

	for (i = 0; i < bq->dec_rate_len; i += 2) {
		if (soc < bq->dec_rate_seq[i])
			break;
	}

	/* the table starts at soc 0, so a match is never the first entry */
	*rate = bq->dec_rate_seq[i > 0 ? i - 1 : 0];

	return 0;
}

static int fg_get_raw_soc(struct bq_fg_chip *bq, int *raw_soc)
{
	int rm = 0, fcc = 0;
	int ret;

	ret = fg_read_rm(bq, &rm);
	ret |= fg_read_fcc(bq, &fcc);
	if (ret)
		return -1;

	rm /= 1000;
	fcc /= 1000;
	*raw_soc = fcc ? rm * 10000 / fcc : 0;

	return 0;
}

static int fg_get_one_soc_decimal(void *data, int *soc_decimal)
{
	struct bq_fg_chip *bq = data;
	int rsoc = 0, raw_soc = 0;

	if (!bq)
		return 0;

	fg_read_rsoc(bq, &rsoc);
	if (fg_get_raw_soc(bq, &raw_soc))
		raw_soc = 0;
	*soc_decimal = raw_soc % 100;

	return 0;
}

static int fg_read_one_soh(void *data, int *soh)
{
	return fg_read_soh(data, soh);
}

static int fg_get_one_temp_max(void *data, int *temp_max)
{
	return fg_get_temp_max(data, temp_max);
}

static int fg_get_one_time_ot(void *data, int *time_ot)
{
	return fg_read_time_ot(data, time_ot);
}

static int fg_get_one_batt_cell_info(void *data, const char **name)
{
	struct bq_fg_chip *bq = data;

	if (!bq->cell_name)
		return -1;

	*name = bq->cell_name;

	return 0;
}

static int fg_get_error_state(void *data, bool *error)
{
	struct bq_fg_chip *bq = data;
	int ok;

	fg_get_chip_ok(bq, &ok);
	*error = bq->fg_error;

	return 0;
}

static int fg_get_one_cutoff_voltage(void *data, int *volt)
{
	return fg_get_cutoff_voltage(data, volt);
}

static int fg_set_cutoff_voltage(struct bq_fg_chip *bq, int value)
{
	u8 data[2];
	u16 cmd;
	int ret;

	data[0] = value & 0xFF;
	data[1] = (value >> 8) & 0xFF;
	mca_log_info("data[0] = %d, data[1] = %d, value = %d", data[0], data[1],
		     value);

	if (bq->fg_vendor == MPC7012)
		cmd = FG_MAC_CMD_MPC7012_CUTOFF_VOLTAGE;
	else
		cmd = FG_MAC_CMD_CUTOFF_VOLTAGE;

	ret = fg_mac_write_block(bq, cmd, data, 2);
	if (ret < 0)
		mca_log_err("could not write term_voltage = %d\n", ret);

	return ret;
}

static int fg_set_one_cutoff_voltage(void *data, int value)
{
	return fg_set_cutoff_voltage(data, value);
}

static int fg_get_one_dod_count(void *data)
{
	return fg_get_dod_count(data);
}

static int fg_get_one_count_level1(void *data, int *count)
{
	struct bq_fg_chip *bq = data;

	*count = bq->fake_count_level1 ? bq->fake_count_level1 : bq->count_level1;

	return 0;
}

static int fg_get_one_count_level2(void *data, int *count)
{
	struct bq_fg_chip *bq = data;

	*count = bq->fake_count_level2 ? bq->fake_count_level2 : bq->count_level2;

	return 0;
}

static int fg_get_one_count_level3(void *data, int *count)
{
	struct bq_fg_chip *bq = data;

	*count = bq->fake_count_level3 ? bq->fake_count_level3 : bq->count_level3;

	return 0;
}

static int fg_get_one_count_lowtemp(void *data, int *count)
{
	struct bq_fg_chip *bq = data;

	*count = bq->fake_count_lowtemp ? bq->fake_count_lowtemp :
					  bq->count_lowtemp;

	return 0;
}

static int fg_clear_count_data(struct bq_fg_chip *bq)
{
	u8 t_buf[4] = { 0xD3, 0xC2, 0xB1, 0xA0 };
	int ret;

	ret = fg_mac_write_block(bq, FG_MAC_CMD_CLEAR_COUNT_DATA, t_buf, 4);
	if (ret < 0) {
		mca_log_err("could not write clear_cout_data = %d\n", ret);
		return ret;
	}

	return 0;
}

static int fg_set_one_clear_count_data(void *data)
{
	return fg_clear_count_data(data);
}

static int fg_get_adapt_power(struct bq_fg_chip *bq, int *adapt_power)
{
	static const struct {
		char ch;
		int power;
	} adapt_power_map[] = {
		{ '0', 10 }, { '1', 15 }, { '2', 18 }, { '3', 25 },
		{ '4', 33 }, { '5', 35 }, { '6', 40 }, { '7', 55 },
		{ '8', 60 }, { '9', 67 }, { 'A', 80 }, { 'B', 90 },
		{ 'C', 100 }, { 'D', 120 }, { 'E', 140 }, { 'F', 160 },
		{ 'G', 180 }, { 'H', 200 }, { 'I', 220 }, { 'J', 240 },
	};
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	const char *device_name;
	u8 t_buf[5] = { 0 };
	char ch;
	int ret, i;

	ret = fg_mac_read_block(bq, FG_MAC_CMD_MANU_NAME, t_buf, 5);
	if (ret < 0) {
		mca_log_err("failed to get adapt power = %d\n", ret);
		return ret;
	}

	ch = t_buf[4];
	bq->adapt_power = 0;
	for (i = 0; i < ARRAY_SIZE(adapt_power_map); i++) {
		if (adapt_power_map[i].ch == ch) {
			bq->adapt_power = adapt_power_map[i].power;
			break;
		}
	}
	mca_log_info("ch: %c, adapt_power: %d\n", ch, bq->adapt_power);

	/*
	 * One O2 pack reports a manufacturer code that maps to a far lower
	 * figure than it can take.
	 */
	if (hwid && hwid->platform_version == HARDWARE_PROJECT_O2) {
		device_name = fg_get_device_name(bq);
		if (device_name && !strncmp(device_name, "BP54", 4)) {
			bq->adapt_power = 90;
			mca_log_err("project O2 matched BP54, set adapt_power: 90\n");
		}
	}

	*adapt_power = bq->adapt_power;

	return ret;
}

static int fg_get_one_adapt_power(void *data, int *adapt_power)
{
	return fg_get_adapt_power(data, adapt_power);
}

static int fg_get_aged_flag(void *data, int *aged_flag)
{
	struct bq_fg_chip *bq = data;

	if (!bq)
		return -1;

	*aged_flag = bq->aged_flag;

	return 0;
}

static int fg_get_one_isc_alert_level(void *data, int *level)
{
	struct bq_fg_chip *bq = data;

	if (bq->fake_isc)
		return bq->fake_isc;

	fg_get_isc_alert_level(bq, level);

	return 0;
}

static int fg_get_one_soa_alert_level(void *data, int *level)
{
	return fg_get_soa_alert_level(data, level);
}

static int fg_get_one_raw_soc(void *data, int *raw_soc)
{
	return fg_get_raw_soc(data, raw_soc);
}

static int fg_get_one_device_name(void *data, const char **device_name)
{
	struct bq_fg_chip *bq = data;

	if (!bq || !device_name)
		return -1;

	*device_name = fg_get_device_name(bq);

	return *device_name ? 0 : -1;
}

static int fg_get_temp_min(struct bq_fg_chip *bq, int *temp_min)
{
	u8 t_buf[64] = { 0 };
	int ret;

	ret = fg_mac_read_block(bq, FG_MAC_CMD_LIFETIME1, t_buf, 36);
	if (ret < 0)
		return ret;

	*temp_min = t_buf[7];

	return ret;
}

static int fg_get_one_temp_min(void *data, int *temp_min)
{
	return fg_get_temp_min(data, temp_min);
}

static int fg_read_one_fc(void *data, bool *fc)
{
	struct bq_fg_chip *bq = data;
	int ret;

	ret = fg_read_status(bq);
	*fc = bq->batt_fc;

	return ret;
}

/* The calibration figures come in eight temperature bands. */
static int fg_temp_band(int temp)
{
	static const int band_top[] = { 200, 250, 300, 350, 400, 450, 480 };
	int i;

	for (i = 0; i < ARRAY_SIZE(band_top); i++) {
		if (temp < band_top[i])
			break;
	}

	return i;
}

static int fg_get_one_calibration_ffc_iterm(void *data, int *iterm)
{
	struct bq_fg_chip *bq = data;
	u8 t_buf[64] = { 0 };
	int ret, band;

	ret = fg_mac_read_block(bq, FG_MAC_CMD_CALIBRATION_INFO_1, t_buf, 36);
	if (ret < 0)
		return ret;

	band = fg_temp_band(bq->batt_temp);
	*iterm = get_unaligned_le16(&t_buf[band * 4]);

	return ret;
}

static int fg_get_one_real_supplement_energy(void *data, int *energy)
{
	struct bq_fg_chip *bq = data;
	u8 t_buf[64] = { 0 };
	int ret;

	ret = fg_mac_read_block(bq, FG_MAC_CMD_CALIBRATION_INFO_4, t_buf, 36);
	if (ret < 0)
		return ret;

	*energy = get_unaligned_le16(&t_buf[1]);

	return ret;
}

static int fg_get_one_calibration_charge_energy(void *data, int *energy)
{
	struct bq_fg_chip *bq = data;
	u8 t_buf[64] = { 0 };
	int ret, band;

	ret = fg_mac_read_block(bq, FG_MAC_CMD_CALIBRATION_INFO_2, t_buf, 36);
	if (ret < 0)
		return ret;

	band = fg_temp_band(bq->batt_temp);
	*energy = get_unaligned_le16(&t_buf[band * 4 + 2]);

	return ret;
}

#define FL4P0_RETRY_MAX		3
#define FL4P0_FEATURE_BYTE0	0x78

static void fg_fl4p0_enable_check(void *data)
{
	struct bq_fg_chip *bq = data;
	u8 t_buf[36] = { 0 };
	int seal_status = 0;
	int ret, count;

	fg_set_seal(bq, true);
	fg_get_seal(bq, &seal_status);
	if (seal_status != SEAL_STATE_UNSEALED && seal_status != SEAL_STATE_FA) {
		mca_log_err("Fg Unseal Failed");
		return;
	}

	for (count = 0; count < FL4P0_RETRY_MAX; count++) {
		memset(t_buf, 0, sizeof(t_buf));
		ret = fg_mac_read_block(bq, FG_MAC_CMD_FEATURE_INFO, t_buf, 32);
		if (ret < 0) {
			mca_log_err("could not read feature_info, ret=%d", ret);
			msleep(100);
			continue;
		}

		t_buf[0] = FL4P0_FEATURE_BYTE0;
		t_buf[1] = 0x00;
		ret = fg_mac_write_block(bq, FG_MAC_CMD_FEATURE_INFO, t_buf, 32);
		if (ret < 0) {
			mca_log_err("could not write feature_info, ret=%d", ret);
			msleep(100);
			continue;
		}

		mca_log_err("write feature_info success");
		break;
	}

	for (count = 0; count < FL4P0_RETRY_MAX; count++) {
		memset(t_buf, 0, sizeof(t_buf));
		ret = fg_mac_read_block(bq, FG_MAC_CMD_FEATURE_INFO, t_buf, 32);
		if (ret < 0) {
			mca_log_err("could not read feature_info, ret=%d", ret);
			msleep(100);
			continue;
		}

		if (t_buf[0] == FL4P0_FEATURE_BYTE0 && t_buf[1] == 0x00) {
			mca_log_err("Enable FL4P0 Sucess");
			break;
		}

		mca_log_err("Enable FL4P0 failed");
		msleep(100);
	}

	fg_set_seal(bq, false);
}

static void fg_get_pack_vendor(void *data, int *vendor)
{
	struct bq_fg_chip *bq = data;

	*vendor = bq->pack_vendor;
}

static unsigned long fg_get_one_calc_rvalue(void *data)
{
	return fg_get_calc_rvalue(data);
}

/*
 * NFG1000 firmware update
 *
 * The gauge's boot loader takes framed commands: 0xaa, a command register,
 * a payload and a CRC-8 over all of it.  Each frame is sent to the command
 * register, preceded by a write to register 0 that resets the address
 * pointer, and acknowledged in the status register (0x3f) with 01 'U' 'H'.
 */
#define NFG1000_FRAME_HEAD		0xaa
#define NFG1000_STATUS_REG		0x3f
#define NFG1000_BOOT_REG		0x36
#define NFG1000_WRITE_REG		0x30
#define NFG1000_READ_REG		0x31
#define NFG1000_PAGE_REG		0x32
#define NFG1000_ERASE_REG		0x37
#define NFG1000_CRC_REG			0x38
#define NFG1000_EXIT_REG		0x3d
#define NFG1000_CHIP_ID_REG		0x50
#define NFG1000_SHA_RANDOM_REG		0x81
#define NFG1000_SHA_PASSWORD_REG	0x82

#define NFG1000_PAGE_SIZE		0x80
#define NFG1000_PAGES			4
#define NFG1000_STEP_RETRY		5

#define NFG1000_INFO_VER_MARK		20
#define NFG1000_INFO_DEV_NAME		64
#define NFG1000_INFO_STAMP		99
#define NFG1000_INFO_DF_SIG		120
#define NFG1000_INFO_FLAG		231
#define NFG1000_INFO_CRC		511

#define NFG1000_IMAGE_SIZE		0xe00
#define NFG1000_SECTION_SIZE		0x200

/* The info section was only valid in its 0x1e000 copy. */
#define NFG1000_INFO_FROM_COPY		0x33666633
/* Set once step 5 starts writing flash; the part is then in its boot loader. */
#define NFG1000_SECTION_MARKER		0xaaaaaaaa
/* Probe found a gauge that wants a new image. */
#define NFG1000_OTA_UPDATE_PENDING	0xaa5555aa

#define NFG1000_ATTEMPTS		3
#define NFG1000_EXIT_SETTLE_MS		1500

static const u8 sha_256_ramdom_data[32] = {
	0x12, 0x12, 0xe3, 0x39, 0x36, 0x86, 0x49, 0xf0,
	0x3f, 0xc7, 0x6b, 0x4c, 0x17, 0xfb, 0xb3, 0x15,
	0x32, 0xbe, 0x5c, 0xa5, 0xfc, 0x63, 0x99, 0x22,
	0x71, 0xe9, 0xf9, 0x36, 0x91, 0x35, 0xb1, 0x72,
};

static const u8 sha_256_pass_word[32] = {
	0x46, 0x6c, 0xea, 0x38, 0xbd, 0x1f, 0x52, 0x73,
	0x69, 0x96, 0x79, 0x0e, 0x53, 0xd5, 0xa2, 0x89,
	0xb8, 0x6b, 0x91, 0x77, 0xd6, 0xee, 0x82, 0x94,
	0x6b, 0x78, 0x94, 0x53, 0xbe, 0xbb, 0x32, 0xf5,
};

/* static data flash signature written for each gauge version */
static const u8 static_df_sig[16] = {
	0x08, 0x00, 0x0e, 0x00, 0x10, 0x00, 0x0d, 0x00,
	0xf4, 0x00, 0x19, 0x00, 0xb9, 0x00, 0x7c, 0x00,
};

/* application code CRC-16 for each gauge version */
static const u8 code_version_crc16[16] = {
	0x23, 0x0e, 0x23, 0x0e, 0x23, 0x0e, 0x23, 0x0e,
	0xe2, 0x1a, 0xfb, 0x9e, 0x38, 0xc2, 0x38, 0xc2,
};

static u8 GetCRC8(const u8 *buf, int len)
{
	u8 crc = 0;
	int i;

	while (len--) {
		crc ^= *buf++;
		for (i = 0; i < 8; i++)
			crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
	}

	return crc;
}

static int fg_ota_write_block(struct bq_fg_chip *bq, u8 reg, u8 *buf, u8 len)
{
	u8 zero_addr[2] = { 0x00, 0x00 };
	int ret;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_write_block(bq->client, 0x00, zero_addr, 2);
	if (ret < 0)
		mca_log_err("could not write zero_addr, ret=%d", ret);
	else
		ret = __fg_write_block(bq->client, reg, buf, len);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static int nfg1000_ota_read(struct bq_fg_chip *bq, u8 reg, u8 *buf, u8 len)
{
	int ret;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_read_block(bq->client, reg, buf, len);
	mutex_unlock(&bq->i2c_rw_lock);
	usleep_range(2000, 2100);

	return ret;
}

/* Return: < 0 on a bus error, 0 when acknowledged, 1 otherwise. */
static int nfg1000_read_status(struct bq_fg_chip *bq)
{
	u8 status[3] = { 0 };
	int ret;

	ret = nfg1000_ota_read(bq, NFG1000_STATUS_REG, status, sizeof(status));
	if (ret < 0)
		return ret;

	return (status[0] == 0x01 && status[1] == 'U' && status[2] == 'H') ? 0 : 1;
}

static int nfg1000_ota_program_step1_EnterBootLoad(struct bq_fg_chip *bq)
{
	u8 unlock[2] = { 0x00, 0x0f };
	u8 boot_cmd[3] = { 0x01, 0x22, 0x04 };
	int ret = 0, i;

	for (i = 0; i < 3; i++) {
		mutex_lock(&bq->i2c_rw_lock);
		ret = __fg_write_block(bq->client, bq->regs[BQ_FG_REG_ALT_MAC],
				       unlock, 2);
		mutex_unlock(&bq->i2c_rw_lock);
		msleep(50);
		if (ret >= 0)
			break;
		mca_log_err("could not write alt_mac\n");
	}
	if (ret < 0) {
		mca_log_err("ota_program_step1_EnterBootLoad:write reg: %x error!!\n",
			    bq->regs[BQ_FG_REG_ALT_MAC]);
		return -1;
	}

	msleep(50);

	for (i = 0; i < NFG1000_STEP_RETRY; i++) {
		ret = fg_ota_write_block(bq, NFG1000_BOOT_REG, boot_cmd, 3);
		usleep_range(2000, 2100);
		if (ret < 0) {
			mca_log_err("could not write boot_cmd1\n");
			continue;
		}

		ret = fg_ota_write_block(bq, NFG1000_BOOT_REG, boot_cmd, 3);
		usleep_range(2000, 2100);
		if (ret < 0) {
			mca_log_err("could not write boot_cmd2\n");
			continue;
		}

		ret = nfg1000_read_status(bq);
		if (ret < 0) {
			mca_log_err("could not read boot_cmd2\n");
			continue;
		}
		if (!ret)
			return 0;
	}

	mca_log_err("ota_program_step1_EnterBootLoad:return code error:%x!!\n",
		    NFG1000_STATUS_REG);

	return -1;
}

static int nfg1000_ota_program_step2_ShaAuth(struct bq_fg_chip *bq)
{
	u8 frame[36];
	int ret, i;

	for (i = 0; i < NFG1000_STEP_RETRY; i++) {
		frame[0] = NFG1000_FRAME_HEAD;
		frame[1] = NFG1000_SHA_RANDOM_REG;
		frame[2] = 0x20;
		memcpy(&frame[3], sha_256_ramdom_data, sizeof(sha_256_ramdom_data));
		frame[35] = GetCRC8(frame, 35);
		ret = fg_ota_write_block(bq, frame[1], &frame[2], 34);
		usleep_range(2000, 2100);
		if (ret < 0) {
			mca_log_err("could not write sha_data\n");
			continue;
		}

		frame[1] = NFG1000_SHA_PASSWORD_REG;
		memcpy(&frame[3], sha_256_pass_word, sizeof(sha_256_pass_word));
		frame[35] = GetCRC8(frame, 35);
		ret = fg_ota_write_block(bq, frame[1], &frame[2], 34);
		usleep_range(2000, 2100);
		if (ret < 0) {
			mca_log_err("could not write sha_data\n");
			continue;
		}

		ret = nfg1000_read_status(bq);
		if (ret < 0)
			mca_log_err("could not read boot_cmd\n");
		else if (!ret)
			return 0;
		usleep_range(10000, 10100);
	}

	mca_log_err("ota_program_step2_ShaAuth:write reg: %x error!!\n",
		    NFG1000_STATUS_REG);

	return -1;
}

static int nfg1000_ota_program_step3_mcuAuth(struct bq_fg_chip *bq)
{
	static const u8 chip_id[6] = { 0x04, 0x11, 0x83, 0x00, 0x00, 'R' };
	u8 id[6];
	int ret, i;

	for (i = 0; i < NFG1000_STEP_RETRY; i++) {
		memset(id, 0, sizeof(id));
		ret = nfg1000_ota_read(bq, NFG1000_CHIP_ID_REG, id, sizeof(id));
		if (ret < 0) {
			mca_log_err("could not read boot_cmd\n");
			continue;
		}
		if (!memcmp(id, chip_id, sizeof(chip_id)))
			return 0;
	}

	mca_log_err("ota_program_step3_CHIP ID:return code error:%x!!\n",
		    NFG1000_CHIP_ID_REG);

	return -1;
}

static int nfg1000_erase_flash_step(struct bq_fg_chip *bq, u32 addr, u8 sectors)
{
	u8 frame[10];
	int ret, i;

	frame[0] = NFG1000_FRAME_HEAD;
	frame[1] = NFG1000_ERASE_REG;
	frame[2] = 0x06;
	frame[3] = addr & 0xff;
	frame[4] = (addr >> 8) & 0xff;
	frame[5] = (addr >> 16) & 0xff;
	frame[6] = 0;
	frame[7] = sectors;
	frame[8] = 0;
	frame[9] = GetCRC8(frame, 9);

	for (i = 0; i < NFG1000_STEP_RETRY; i++) {
		ret = fg_ota_write_block(bq, frame[1], &frame[2], 8);
		msleep(sectors * 10);
		if (ret < 0) {
			mca_log_err("could not write flash\n");
			continue;
		}

		ret = nfg1000_read_status(bq);
		if (ret < 0)
			mca_log_err("could not read boot_cmd\n");
		else if (!ret)
			return 0;
		usleep_range(10000, 10100);
	}

	mca_log_err("nfg1000_erase_flash_step: %x error!!\n", NFG1000_STATUS_REG);

	return -1;
}

/* Read one 512-byte section, as four CRC-checked 128-byte pages. */
static int nfg1000_read_flash_step(struct bq_fg_chip *bq, u32 addr, u8 *buf)
{
	u8 frame[8];
	u8 page[3 + 2 + NFG1000_PAGE_SIZE];
	int ret, i, p;

	frame[0] = NFG1000_FRAME_HEAD;
	frame[1] = NFG1000_READ_REG;
	frame[2] = 0x04;
	frame[3] = addr & 0xff;
	frame[4] = (addr >> 8) & 0xff;
	frame[5] = (addr >> 16) & 0xff;
	frame[6] = 0;
	frame[7] = GetCRC8(frame, 7);

	for (i = 0; i < NFG1000_STEP_RETRY; i++) {
		ret = fg_ota_write_block(bq, frame[1], &frame[2], 6);
		usleep_range(2000, 2100);
		if (ret < 0) {
			mca_log_err("could not write flash\n");
			continue;
		}

		/* each page: a header byte, 128 data bytes and a CRC */
		for (p = 0; p < NFG1000_PAGES; p++) {
			memset(page, 0, sizeof(page));
			page[0] = NFG1000_FRAME_HEAD;
			page[1] = NFG1000_PAGE_REG + p;
			page[2] = 0xab;
			ret = nfg1000_ota_read(bq, page[1], &page[3],
					       2 + NFG1000_PAGE_SIZE);
			if (ret < 0) {
				mca_log_err("could not read boot_cmd\n");
				break;
			}
			if (GetCRC8(page, sizeof(page) - 1) != page[sizeof(page) - 1]) {
				ret = -EIO;
				break;
			}
			memcpy(&buf[p * NFG1000_PAGE_SIZE], &page[4],
			       NFG1000_PAGE_SIZE);
		}
		if (p < NFG1000_PAGES)
			continue;

		ret = nfg1000_read_status(bq);
		if (ret < 0)
			mca_log_err("could not read boot_cmd\n");
		else if (!ret)
			return 0;
		usleep_range(10000, 10100);
	}

	mca_log_err("nfg1000_read_flash_step: %x error!!\n", NFG1000_STATUS_REG);

	return -1;
}

/* Write one 512-byte section, as four CRC-framed 128-byte pages. */
static int nfg1000_write_flash_step(struct bq_fg_chip *bq, u32 addr,
				    const u8 *src)
{
	u8 frame[8];
	u8 page[3 + NFG1000_PAGE_SIZE + 1];
	int ret, i, p;

	for (i = 0; i < NFG1000_STEP_RETRY; i++) {
		frame[0] = NFG1000_FRAME_HEAD;
		frame[1] = NFG1000_WRITE_REG;
		frame[2] = 0x04;
		frame[3] = addr & 0xff;
		frame[4] = (addr >> 8) & 0xff;
		frame[5] = (addr >> 16) & 0xff;
		frame[6] = 0;
		frame[7] = GetCRC8(frame, 7);
		ret = fg_ota_write_block(bq, frame[1], &frame[2], 6);
		usleep_range(2000, 2100);
		if (ret < 0) {
			mca_log_err("could not write flash\n");
			continue;
		}

		for (p = 0; p < NFG1000_PAGES; p++) {
			page[0] = NFG1000_FRAME_HEAD;
			page[1] = NFG1000_PAGE_REG + p;
			page[2] = NFG1000_PAGE_SIZE;
			memcpy(&page[3], &src[p * NFG1000_PAGE_SIZE],
			       NFG1000_PAGE_SIZE);
			page[sizeof(page) - 1] = GetCRC8(page, sizeof(page) - 1);
			ret = fg_ota_write_block(bq, page[1], &page[2],
						 sizeof(page) - 2);
			usleep_range(10000, 10100);
			if (ret < 0) {
				mca_log_err("could not write flash\n");
				break;
			}
		}
		if (p < NFG1000_PAGES)
			continue;

		ret = nfg1000_read_status(bq);
		if (ret < 0) {
			mca_log_err("could not read boot_cmd\n");
			continue;
		}
		if (!ret)
			return 0;
	}

	mca_log_err("nfg1000_write_flash_step: %x error!!\n", NFG1000_STATUS_REG);

	return -1;
}

/* Erase, write and read back one section until it verifies. */
static int nfg1000_update_flash_step(struct bq_fg_chip *bq, u32 addr,
				     const u8 *src)
{
	u8 verify[NFG1000_SECTION_SIZE];
	int i;

	for (i = 0; i < NFG1000_STEP_RETRY; i++) {
		if (nfg1000_erase_flash_step(bq, addr, 1))
			continue;
		if (nfg1000_write_flash_step(bq, addr, src))
			continue;
		memset(verify, 0, sizeof(verify));
		if (nfg1000_read_flash_step(bq, addr, verify))
			continue;
		if (!memcmp(verify, src, sizeof(verify)))
			return 0;
	}

	mca_log_err("%x update error!!\n", addr);

	return -1;
}

static bool nfg1000_info_valid(const u8 *buf)
{
	return GetCRC8(buf, NFG1000_INFO_CRC) == buf[NFG1000_INFO_CRC] &&
	       buf[NFG1000_INFO_VER_MARK] == 0x06 &&
	       buf[NFG1000_INFO_VER_MARK + 1] == 0x00 &&
	       buf[NFG1000_INFO_CRC] != 0xff;
}

/*
 * Find out which of the eight supported images the gauge runs from the
 * static data flash signature in its info section, and check that the part
 * and its application code are what that image expects.
 */
static int nfg1000_ota_program_step4_gauge_version(struct bq_fg_chip *bq)
{
	u8 *buf = bq->nfg1000_ota_buf;
	u8 code[NFG1000_SECTION_SIZE];
	const char *name;
	unsigned int ver;

	if (nfg1000_read_flash_step(bq, 0x1f000, buf)) {
		mca_log_err("failed read flash step1\n");
		return -1;
	}

	if (nfg1000_info_valid(buf)) {
		bq->nfg1000_magic = 0;
	} else {
		if (nfg1000_read_flash_step(bq, 0x1e000, buf)) {
			mca_log_err("failed read flash step2\n");
			return -1;
		}
		if (!nfg1000_info_valid(buf)) {
			mca_log_err("read error: %x\n", buf[NFG1000_INFO_CRC]);
			return -1;
		}
		bq->nfg1000_magic = NFG1000_INFO_FROM_COPY;
	}

	switch (buf[NFG1000_INFO_DF_SIG + 1] ? 0 : buf[NFG1000_INFO_DF_SIG]) {
	case 0x47:
		ver = 0;
		break;
	case 0x41:
		ver = 1;
		break;
	case 0xc3:
		ver = 2;
		break;
	case 0x7f:
		ver = 3;
		break;
	case 0xa8:
		ver = 4;
		break;
	case 0xf6:
		ver = 5;
		break;
	case 0x14:
		ver = 6;
		break;
	case 0x2b:
		ver = 7;
		break;
	default:
		mca_log_err("static df signature check error: %x\n",
			    buf[NFG1000_INFO_DF_SIG]);
		return -1;
	}
	bq->nfg1000_ver = ver;

	name = (ver & 1) ? "XM15@BP5B#" : "XM15@BM6A#";
	if (memcmp(&buf[NFG1000_INFO_DEV_NAME], name, 10)) {
		mca_log_err("device name error\n");
		return -1;
	}

	memset(code, 0, sizeof(code));
	if (nfg1000_read_flash_step(bq, 0x3600, code)) {
		mca_log_err("failed read flash step3\n");
		return -1;
	}

	if (memcmp(&nfg1000_code_data[ver * NFG1000_IMAGE_SIZE + 0x200], code,
		   sizeof(code))) {
		mca_log_err("app code check error\n");
		return -1;
	}

	return 0;
}

static int nfg1000_ota_program_step5_update_gauge(struct bq_fg_chip *bq)
{
	const u8 *image = &nfg1000_code_data[bq->nfg1000_ver * NFG1000_IMAGE_SIZE];
	u8 *buf = bq->nfg1000_ota_buf;
	u32 df_addr = bq->nfg1000_ver < 4 ? 0xda00 : 0xd800;

	bq->nfg1000_section_marker = NFG1000_SECTION_MARKER;

	if (nfg1000_erase_flash_step(bq, 0x3400, 1)) {
		mca_log_err("failed earse flash step1\n");
		return -1;
	}

	if (nfg1000_erase_flash_step(bq, df_addr, 1)) {
		if (bq->nfg1000_ver < 4)
			mca_log_err("failed earse flash step2\n");
		else
			mca_log_err("failed earse flash step3\n");
		return -1;
	}

	if (bq->nfg1000_ver < 2) {
		if (nfg1000_erase_flash_step(bq, 0x1e200, 2)) {
			mca_log_err("failed earse flash step5\n");
			return -1;
		}
	} else {
		if (nfg1000_erase_flash_step(bq, 0x1e200, 4)) {
			mca_log_err("failed earse flash step4\n");
			return -1;
		}
	}

	/* restore the 0x1f000 copy of the info section if only 0x1e000 was good */
	if (bq->nfg1000_magic == NFG1000_INFO_FROM_COPY &&
	    nfg1000_update_flash_step(bq, 0x1f000, buf)) {
		mca_log_err("failed earse flash step6\n");
		return -1;
	}

	buf[NFG1000_INFO_STAMP] = 0x42;
	buf[NFG1000_INFO_DF_SIG] = static_df_sig[bq->nfg1000_ver * 2];
	buf[NFG1000_INFO_DF_SIG + 1] = static_df_sig[bq->nfg1000_ver * 2 + 1];
	buf[NFG1000_INFO_FLAG] &= 0xfb;
	buf[NFG1000_INFO_CRC] = GetCRC8(buf, NFG1000_INFO_CRC);

	if (nfg1000_update_flash_step(bq, 0x1e000, buf)) {
		mca_log_err("failed write flash step11\n");
		return -1;
	}

	if (nfg1000_update_flash_step(bq, 0x1e200, image + 0x600) ||
	    nfg1000_update_flash_step(bq, 0x1e400, image + 0x800) ||
	    (bq->nfg1000_ver > 1 &&
	     (nfg1000_update_flash_step(bq, 0x1e600, image + 0xa00) ||
	      nfg1000_update_flash_step(bq, 0x1e800, image + 0xc00)))) {
		mca_log_err("update 0x0001E200 error\n");
		return -1;
	}

	if (nfg1000_update_flash_step(bq, df_addr, image + 0x400)) {
		if (bq->nfg1000_ver < 4)
			mca_log_err("update 0x0000DA00 error\n");
		else
			mca_log_err("update 0x0000D800 error\n");
		return -1;
	}

	if (nfg1000_update_flash_step(bq, 0x3400, image)) {
		mca_log_err("update 0x00003400 error\n");
		return -1;
	}

	return 0;
}

static int nfg1000_crc16_check_step(struct bq_fg_chip *bq)
{
	u8 frame[14] = { 0 };
	int ret, i;

	frame[0] = NFG1000_FRAME_HEAD;
	frame[1] = NFG1000_CRC_REG;
	frame[2] = 0x0a;
	frame[3] = 0x04;
	frame[4] = 0x34;
	frame[7] = 0xfc;
	frame[8] = 0x8b;
	frame[9] = 0x01;
	frame[11] = code_version_crc16[bq->nfg1000_ver * 2];
	frame[12] = code_version_crc16[bq->nfg1000_ver * 2 + 1];
	frame[13] = GetCRC8(frame, 13);

	for (i = 0; i < NFG1000_STEP_RETRY; i++) {
		ret = fg_ota_write_block(bq, frame[1], &frame[2], 12);
		usleep_range(2000, 2100);
		if (ret < 0) {
			mca_log_err("could not write flash\n");
			continue;
		}

		msleep(100);
		if (!nfg1000_read_status(bq))
			return 0;
		usleep_range(10000, 10100);
	}

	mca_log_err("nfg1000_crc16_check_step: %x error!!\n", NFG1000_STATUS_REG);

	return -1;
}

static int nfg1000_ota_program_step6_CheckCrc(struct bq_fg_chip *bq)
{
	return nfg1000_crc16_check_step(bq);
}

static int nfg1000_ota_program_step7_ExitBoot(struct bq_fg_chip *bq)
{
	u8 frame[5] = { NFG1000_FRAME_HEAD, NFG1000_EXIT_REG, 0x01, 0x00, 0x00 };
	int ret, i;

	frame[4] = GetCRC8(frame, 4);

	for (i = 0; i < NFG1000_STEP_RETRY; i++) {
		ret = fg_ota_write_block(bq, frame[1], &frame[2], 3);
		usleep_range(2000, 2100);
		if (ret < 0)
			continue;

		if (!nfg1000_read_status(bq))
			return 0;
		usleep_range(10000, 10100);
	}

	mca_log_err("write reg 0x3D error\n");

	return -1;
}

static int nfg1000_update_APP(struct bq_fg_chip *bq)
{
	int ret = -1;

	/* ordinary transfers stay off the bus until the update is done */
	WRITE_ONCE(bq->ota_updating, true);

	if (nfg1000_ota_program_step1_EnterBootLoad(bq)) {
		mca_log_err("nfg1000 ota program step1 fail\n");
		goto out;
	}

	if (nfg1000_ota_program_step2_ShaAuth(bq)) {
		mca_log_err("nfg1000 ota program step2 fail\n");
		goto out;
	}

	if (nfg1000_ota_program_step3_mcuAuth(bq)) {
		mca_log_err("nfg1000 ota program step3 fail\n");
		goto out;
	}

	if (nfg1000_ota_program_step4_gauge_version(bq)) {
		mca_log_err("nfg1000 ota program step4 fail\n");
		goto out;
	}

	if (nfg1000_ota_program_step5_update_gauge(bq)) {
		mca_log_err("nfg1000 ota program step5 fail\n");
		goto out;
	}

	if (nfg1000_ota_program_step6_CheckCrc(bq)) {
		mca_log_err("nfg1000 ota program step6 fail\n");
		goto out;
	}

	if (nfg1000_ota_program_step7_ExitBoot(bq)) {
		mca_log_err("nfg1000 ota program step7 fail\n");
		goto out;
	}

	msleep(NFG1000_EXIT_SETTLE_MS);
	ret = 0;

out:
	WRITE_ONCE(bq->ota_updating, false);

	return ret;
}

/*
 * Decide whether the gauge runs an image that wants replacing: an
 * "XM15..." NFG1000 pack whose chemistry name is FFN5..FFNA, with an old
 * data flash version.
 *
 * Return: 0 when an update is wanted.
 */
static int nfg1000_update_judge(struct bq_fg_chip *bq)
{
	u8 name[16] = { 0 };
	u8 chem[6] = { 0 };
	u8 ver[2] = { 0 };
	int ret;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_mac_read_block(bq, FG_MAC_CMD_DEVICE_NAME, name, 16);
	mutex_unlock(&bq->i2c_rw_lock);
	if (ret < 0) {
		mca_log_err("could not read device_name, ret=%d\n", ret);
		return 1;
	}

	if (memcmp(name, "XM15", 4))
		return 1;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_mac_read_block(bq, FG_MAC_CMD_CHEM_NAME, chem, 6);
	mutex_unlock(&bq->i2c_rw_lock);
	if (ret < 0) {
		mca_log_err("could not read chem_name, ret=%d\n", ret);
		return 1;
	}

	if (memcmp(chem, "FFN", 3)) {
		mca_log_err("chem name ffn\n");
		return 1;
	}

	if (chem[3] < '5' || chem[3] > 'A')
		return 1;

	mutex_lock(&bq->i2c_rw_lock);
	if (chem[3] == 'A') {
		ret = __fg_mac_read_block(bq, FG_MCA_CMD_DF, ver, 2);
		mutex_unlock(&bq->i2c_rw_lock);
		if (ret < 0) {
			mca_log_err("could not read chem_name, ret=%d\n", ret);
			return 1;
		}
		return ((ver[0] == 'A' || ver[0] == 'G') && !ver[1]) ? 0 : 1;
	}

	ret = __fg_mac_read_block(bq, FG_MAC_CMD_CHEM_DF_SIGN, ver, 2);
	mutex_unlock(&bq->i2c_rw_lock);
	if (ret < 0) {
		mca_log_err("could not read 0x0008, ret=%d\n", ret);
		return 1;
	}

	return ver[1] ? 1 : 0;
}

static int nfg1000_write_alt_mac(struct bq_fg_chip *bq, u16 value)
{
	u8 cmd[2] = { value & 0xff, value >> 8 };
	int ret;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_write_block(bq->client, bq->regs[BQ_FG_REG_ALT_MAC], cmd, 2);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static int nfg1000_read_seal_state(struct bq_fg_chip *bq, int *state)
{
	u8 t_buf[4] = { 0 };
	int ret;

	mutex_lock(&bq->i2c_rw_lock);
	ret = __fg_mac_read_block(bq, FG_MCA_CMD_SEAL_STATE, t_buf, 4);
	mutex_unlock(&bq->i2c_rw_lock);
	if (ret < 0)
		return ret;

	*state = t_buf[1] & 0x03;

	return 0;
}

/* The gauge takes flash writes only in full access. */
static void nfg1000_ota_unseal_full_access(struct bq_fg_chip *bq)
{
	int state = 0;
	int ret, i;

	for (i = 0; i < NFG1000_ATTEMPTS; i++) {
		if (nfg1000_write_alt_mac(bq, 0x303b) < 0) {
			mca_log_err("write alt_mac fail\n");
			return;
		}
		usleep_range(3000, 3100);

		if (nfg1000_write_alt_mac(bq, 0x8ab9) < 0) {
			mca_log_err("write alt_mac2 fail\n");
			return;
		}
		usleep_range(3000, 3100);

		nfg1000_write_alt_mac(bq, 0xc32e);
		usleep_range(3000, 3100);

		nfg1000_write_alt_mac(bq, 0x5947);
		usleep_range(3000, 3100);

		ret = nfg1000_read_seal_state(bq, &state);
		if (ret < 0) {
			mca_log_err("could not read seal_state, ret=%d\n", ret);
			msleep(2000);
			return;
		}

		if (state == SEAL_STATE_FA)
			return;
	}

	mca_log_err("nfg1000 ota unseal fail\n");
}

static void nfg1000_ota_seal_full_access(struct bq_fg_chip *bq)
{
	int state = 0;
	int ret, i;

	for (i = 0; i < NFG1000_ATTEMPTS; i++) {
		if (nfg1000_write_alt_mac(bq, FG_MAC_CMD_SEAL) < 0) {
			mca_log_err("write no_reg_data fail\n");
			return;
		}

		ret = nfg1000_read_seal_state(bq, &state);
		if (ret < 0) {
			mca_log_err("could not read seal_state, ret=%d\n", ret);
			msleep(2000);
			return;
		}

		if (state == SEAL_STATE_SEALED)
			return;
	}

	mca_log_err("nfg1000 ota seal fail\n");
}

/*
 * Write a new image to the gauge if probe found it wanted one.  The gauge
 * class calls this when it judges the moment right; the request is answered
 * once, whether or not the update took.
 */
static void nfg1000_ota_update_check(void *data)
{
	struct bq_fg_chip *bq = data;
	int i;

	if (bq->ota_update_pending != NFG1000_OTA_UPDATE_PENDING)
		return;

	if (!nfg1000_update_judge(bq)) {
		nfg1000_ota_unseal_full_access(bq);

		for (i = 0; i < NFG1000_ATTEMPTS; i++) {
			if (!nfg1000_update_APP(bq))
				break;
		}

		if (bq->nfg1000_section_marker != NFG1000_SECTION_MARKER)
			nfg1000_ota_program_step7_ExitBoot(bq);

		msleep(NFG1000_EXIT_SETTLE_MS);
		nfg1000_ota_seal_full_access(bq);
	}

	bq->ota_update_pending = 0;
}

/*
 * A gauge left in its boot loader by an interrupted update reads 0 V and
 * answers 1 in the status register; the update is finished right away.
 * Otherwise only note whether one is wanted.
 */
static void nfg1000_boot_app_judge(struct bq_fg_chip *bq)
{
	u16 volt = 0;
	u8 boot_cmd = 0;
	u8 cmd[2] = { 0x00, 0x02 };
	int ret = 0, i;

	for (i = 0; i < 3; i++) {
		mutex_lock(&bq->i2c_rw_lock);
		ret = __fg_read_word(bq->client, bq->regs[BQ_FG_REG_VOLT], &volt);
		mutex_unlock(&bq->i2c_rw_lock);
		if (ret >= 0)
			break;
		mca_log_err("could not read volt ret = %d\n", ret);
		msleep(100);
	}

	if (ret >= 0) {
		for (i = 0; i < 3; i++) {
			mutex_lock(&bq->i2c_rw_lock);
			__fg_write_block(bq->client, bq->regs[BQ_FG_REG_ALT_MAC],
					 cmd, 2);
			mutex_unlock(&bq->i2c_rw_lock);
			usleep_range(1000, 1100);
			mutex_lock(&bq->i2c_rw_lock);
			ret = __fg_read_byte(bq->client, NFG1000_STATUS_REG, &boot_cmd);
			mutex_unlock(&bq->i2c_rw_lock);
			if (ret >= 0)
				break;
			mca_log_err("could not read boot_cmd = %d\n",
				    NFG1000_STATUS_REG);
			msleep(100);
		}
	}

	if (ret >= 0 && !volt && boot_cmd == 1) {
		for (i = 0; i < NFG1000_ATTEMPTS; i++) {
			if (!nfg1000_update_APP(bq))
				return;
			if (bq->nfg1000_section_marker != NFG1000_SECTION_MARKER)
				nfg1000_ota_program_step7_ExitBoot(bq);
			msleep(NFG1000_EXIT_SETTLE_MS);
		}
		return;
	}

	if (!nfg1000_update_judge(bq))
		bq->ota_update_pending = NFG1000_OTA_UPDATE_PENDING;
}

static void fg_get_ota_update_flag(void *data, u32 *flag)
{
	struct bq_fg_chip *bq = data;

	*flag = bq->ota_update_pending;
	mca_log_info("get ota update_flag: %x\n", bq->ota_update_pending);
}

static void fg_get_one_average_current(void *data, int *val)
{
	*val = fg_get_average_current(data);
}

static void fg_get_batt_abnormal_info(void *data, void *buf)
{
	struct bq_fg_chip *bq = data;
	int *info = buf;

	info[0] = bq->ocd_count;
	info[1] = bq->ocd_last_cycle;
	info[2] = bq->hocd_count;
	info[3] = bq->hocd_last_cycle;
	info[4] = bq->cuv_count;
	info[5] = bq->cuv_last_cycle;
	info[6] = bq->hcuv_count;
	info[7] = bq->hcuv_last_cycle;
	info[8] = bq->hscd_count;
	info[9] = bq->hscd_last_cycle;
}

static int fg_get_one_manufacturing_date(void *data, u8 *buf)
{
	return fg_read_manufacturing_date(data, buf);
}

static void fg_set_one_first_usage_date(void *data, const char *date)
{
	fg_write_first_usage_date(data, date, strlen(date));
}

static int fg_get_one_first_usage_date(void *data, u8 *buf)
{
	return fg_read_first_usage_date(data, buf);
}

static const struct fuelguage_ic_ops g_bq_fg_ops = {
	.fg_ic_probe_ok = fg_read_one_probe_ok,
	.fg_ic_get_batt_info = fg_read_batt_info,
	.fg_ic_get_rsoc = fg_read_one_rsoc,
	.fg_ic_get_curr = fg_read_one_current,
	.fg_ic_get_volt = fg_read_one_volt,
	.fg_ic_set_temp = fg_set_temperature,
	.fg_ic_get_temp = fg_read_one_temperature,
	.fg_ic_get_original_temp = fg_read_one_original_temperature,
	.fg_ic_get_charge_status = fg_read_one_status,
	.fg_ic_get_rm = fg_read_one_rm,
	.fg_ic_get_fastcharge = fg_get_one_fastcharge_mode,
	.fg_ic_set_fastcharge = fg_set_one_fastcharge_mode,
	.fg_ic_get_chg_vol = fg_read_one_charging_voltage,
	.fg_ic_get_chip_ok = fg_get_one_chip_ok,
	.fg_ic_get_cyclecount = fg_read_one_cyclecount,
	.fg_ic_get_tte = fg_read_one_tte,
	.fg_ic_get_ttf = fg_read_one_ttf,
	.fg_ic_get_fcc = fg_read_one_fcc,
	.fg_ic_get_full_design = fg_read_one_dc,
	.fg_ic_get_decimal_rate = fg_get_one_soc_decimal_rate,
	.fg_ic_get_decimal = fg_get_one_soc_decimal,
	.fg_ic_get_soh = fg_read_one_soh,
	.fg_ic_get_temp_max = fg_get_one_temp_max,
	.fg_ic_get_time_ot = fg_get_one_time_ot,
	.fg_ic_get_batt_cell_info = fg_get_one_batt_cell_info,
	.fg_ic_set_verify_digest = fg_set_verify_digest,
	.fg_ic_get_verify_digest = fg_get_verify_digest,
	.fg_ic_set_authentic = fg_set_authentic,
	.fg_ic_get_authentic = fg_get_authentic,
	.fg_ic_get_error_state = fg_get_error_state,
	.fg_ic_get_cutoff_voltage = fg_get_one_cutoff_voltage,
	.fg_ic_set_cutoff_voltage = fg_set_one_cutoff_voltage,
	.fg_ic_get_dod_count = fg_get_one_dod_count,
	.fg_ic_get_count_level1 = fg_get_one_count_level1,
	.fg_ic_get_count_level2 = fg_get_one_count_level2,
	.fg_ic_get_count_level3 = fg_get_one_count_level3,
	.fg_ic_get_count_lowtemp = fg_get_one_count_lowtemp,
	.fg_ic_set_clear_count_data = fg_set_one_clear_count_data,
	.fg_ic_get_adapt_power = fg_get_one_adapt_power,
	.fg_ic_get_aged_flag = fg_get_aged_flag,
	.fg_ic_get_isc_alert_level = fg_get_one_isc_alert_level,
	.fg_ic_get_soa_alert_level = fg_get_one_soa_alert_level,
	.fg_ic_get_raw_soc = fg_get_one_raw_soc,
	.fg_ic_update_fw = fg_update_data_work,
	.fg_ic_get_device_name = fg_get_one_device_name,
	.fg_ic_get_temp_min = fg_get_one_temp_min,
	.fg_ic_set_force_report_full = fg_set_force_report_full,
	.fg_ic_get_fc = fg_read_one_fc,
	.fg_ic_set_co = fg_set_one_co,
	.fg_ic_get_calibration_ffc_iterm = fg_get_one_calibration_ffc_iterm,
	.fg_ic_get_real_supplement_energy = fg_get_one_real_supplement_energy,
	.fg_ic_get_calibration_charge_energy = fg_get_one_calibration_charge_energy,
	.fg_ic_fl4p0_enable_check = fg_fl4p0_enable_check,
	.fg_ic_get_ui_soh = fg_get_ui_soh,
	.fg_ic_get_pack_vendor = fg_get_pack_vendor,
	.fg_ic_get_calc_rvalue = fg_get_one_calc_rvalue,
	.fg_ic_get_ota_update_flag = fg_get_ota_update_flag,
	.fg_ic_ota_update_check = nfg1000_ota_update_check,
	.fg_ic_get_average_current = fg_get_one_average_current,
	.fg_ic_get_batt_abnormal_info = fg_get_batt_abnormal_info,
	.fg_ic_get_manufacturing_date = fg_get_one_manufacturing_date,
	.fg_ic_set_first_usage_date = fg_set_one_first_usage_date,
	.fg_ic_get_first_usage_date = fg_get_one_first_usage_date,
};

/*
 * Debug files
 */
static ssize_t fg_sysfs_show(struct device *dev, struct device_attribute *attr,
			     char *buf);
static ssize_t fg_sysfs_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count);

static struct mca_sysfs_attr_info fg_sysfs_field_tbl[] = {
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_CHIP_OK, chip_ok),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_VOL, vbatt),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_PACKVOL, pack_vbat),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_CO_STATUS, co_status),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_CURRENT, ibatt),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_RSOC, rsoc),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_TEMP, temp),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_ORIGINAL_TEMP, original_temp),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_CYCLE, cyclecount),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_QMAX, qmax),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_RM, rm),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_FCC, charger_full),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_SOH, soh),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_FCC_SOH, fcc_soh),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_FAST_CHARGE, fast_charge),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_RESISTANCE_ID, resistance_id),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_TSIM, tsim),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_TAMBIENT, tambinet),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_TREMQ, tremq),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_TFULLCHGQ, tfullchgq),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_AVERAGE_CURRENT, avercurrent),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_SEAL, seal),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_DF_CHECK, df_check),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_CHEM_DF_SIGN, chem_df_sign),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_VENDOR_ID, vendor),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_PACK_VENDOR_ID, pack_vendor),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_CELL_VENDOR_ID, cell_vendor),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_EEPROM_VERSION, eeprom_version),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_DESIGN_CAPACITY, design_capacity),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_CALC_RVALUE, calc_rvalue),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_AGED_FLAG, aged_flag),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_UI_SOH, ui_soh),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_BATT_SN, batt_sn),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_MANUFACTURING_DATE, manufacturing_date),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_FIRST_USAGE_DATE, first_usage_date),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_FAKE_FIRST_USAGE_DATE, fake_first_usage_date),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_SOH_NEW, soh_new),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_DOD_COUNT, dod_count),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_COUNT_LEVEL1, count_level1),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_COUNT_LEVEL2, count_level2),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_COUNT_LEVEL3, count_level3),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_COUNT_LOWTEMP, count_lt),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_CUTOFF_VOL, cutoff_vol),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_DEVICE_NAME, device_name),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_MAX_TEMP_OCCUR_TIME, max_temp_occur_time),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_RUN_TIME, run_time),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_MAX_TEMP_TIME, max_temp_time),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_TOTAL_FW_RUN_TIME, total_fw_runtime),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_TEMP_MAX, temp_max),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_TIME_HT, time_ht),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_TIME_OT, time_ot),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_OVER_PEAK_FLAG, peak_flag),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_MONITOR_ISC, isc),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_MONITOR_SOA, soa),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_CURRENT_DEVIATION, current_deviation),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_POWER_DEVIATION, power_deviation),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_AVERAGE_CURRENT, average_current),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_AVERAGE_TEMPERATURE, average_temperature),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_START_LEARNING, start_learning),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_STOP_LEARNING, stop_learning),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_ACTUAL_POWER, action_power),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_LEARNING_POWER, learning_power),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_LEARNING_POWER_DEV, learning_power_dev),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_LEARNING_TIME_DEV, learning_time_dev),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_CONSTANT_POWER, constant_power),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_REMAINING_TIME, remaining_time),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_REFERANCE_POWER, referance_power),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_NVT_REFERANCE_POWER, nvt_referance_power),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_REFERANCE_CURRENT, referance_current),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_START_LEARNING_B, start_learning_b),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_STOP_LEARNING_B, stop_learning_b),
	mca_sysfs_attr_rw(fg_sysfs, 0664, FG_IC_PROP_LEARNING_POWER_B, learning_power_b),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_ACTUAL_POWER_B, action_power_b),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_LEARNING_POWER_DEV_B, learning_power_dev_b),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_REL_SOH, rel_soh),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_REL_SOH_CYCLECOUNT, rel_soh_cyclecount),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_EIS_SOH, eis_soh),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_EIS_SOH_CYCLECOUNT, eis_soh_cyclecount),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_QMAX_CYCLECOUNT, qmax_cyclecount),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_BATT_USE_ENVIRRONMENT, batt_use_environment),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_MAX_LIFE_VOL, max_life_vol),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_MIN_LIFE_VOL, min_life_vol),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_MAX_LIFE_TEMP, max_life_temp),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_MIN_LIFE_TEMP, min_life_temp),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_OVER_VOL_DURATION, over_vol_duration),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_FC, fc),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_CSD_FLAG, csd_flag),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_CSD_R1, csd_r1),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_CSD_R2, csd_r2),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_DCR_SLOPE1, dcr_slope1),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_DCR_SLOPE2, dcr_slope2),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_DCR_SLOPE3, dcr_slope3),
	mca_sysfs_attr_ro(fg_sysfs, 0440, FG_IC_PROP_CHARGE_ABSOC, charge_absoc),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_MAX_DSG_CURRENT, max_dsg_current),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_MIN_TEMP, min_temp),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_MIN_VOLTAGE, min_voltage),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_CUV_COUNT, cuv_count),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_CUV_LAST_CYCLE, cuv_last_cycle),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_HCUV_COUNT, hcuv_count),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_HCUV_LAST_CYCLE, hcuv_last_cycle),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_OCD_COUNT, ocd_count),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_OCD_LAST_CYCLE, ocd_last_cycle),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_HOCD_COUNT, hocd_count),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_HOCD_LAST_CYCLE, hocd_last_cycle),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_HSCD_COUNT, hscd_count),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_HSCD_LAST_CYCLE, hscd_last_cycle),
	mca_sysfs_attr_ro(fg_sysfs, 0444, FG_IC_PROP_CO_AUTO_OPEN, co_auto_open),
};

#define FG_SYSFS_ATTRS_SIZE	ARRAY_SIZE(fg_sysfs_field_tbl)

static struct attribute *fg_sysfs_attrs[FG_SYSFS_ATTRS_SIZE + 1];

static const struct attribute_group fg_sysfs_attr_group = {
	.attrs = fg_sysfs_attrs,
};

static ssize_t fg_sysfs_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct bq_fg_chip *bq = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *attr_info;
	u8 data[64] = { 0 };
	u8 byte_val = 0;
	int val = 0;
	int i;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name, fg_sysfs_field_tbl,
					  FG_SYSFS_ATTRS_SIZE);
	if (!attr_info)
		return -1;

	switch (attr_info->sysfs_attr_name) {
	case FG_IC_PROP_CHIP_OK:
		fg_get_chip_ok(bq, &val);
		break;
	case FG_IC_PROP_VOL:
		fg_read_volt(bq, &val);
		break;
	case FG_IC_PROP_PACKVOL:
		fg_read_packvolt(bq, &val);
		break;
	case FG_IC_PROP_CO_STATUS:
		fg_read_co_status(bq, &val);
		break;
	case FG_IC_PROP_CURRENT:
		fg_read_current(bq, &val);
		break;
	case FG_IC_PROP_RSOC:
		fg_read_rsoc(bq, &val);
		break;
	case FG_IC_PROP_TEMP:
		fg_read_temperature(bq, &val);
		break;
	case FG_IC_PROP_ORIGINAL_TEMP:
		fg_read_original_temperature(bq, &val);
		break;
	case FG_IC_PROP_CYCLE:
		fg_read_cyclecount(bq, &val);
		break;
	case FG_IC_PROP_QMAX:
		fg_read_qmax(bq, &val);
		break;
	case FG_IC_PROP_RM:
		fg_read_rm(bq, &val);
		break;
	case FG_IC_PROP_FCC:
		fg_read_fcc(bq, &val);
		break;
	case FG_IC_PROP_SOH:
		fg_read_soh(bq, &val);
		break;
	case FG_IC_PROP_FCC_SOH:
		fg_read_fcc_soh(bq, &val);
		break;
	case FG_IC_PROP_FAST_CHARGE:
		val = bq->fast_mode;
		break;
	case FG_IC_PROP_RESISTANCE_ID:
		val = bq->resistance_id;
		break;
	case FG_IC_PROP_TSIM:
		fg_read_tsim(bq, &val);
		break;
	case FG_IC_PROP_TAMBIENT:
		fg_read_tambient(bq, &val);
		break;
	case FG_IC_PROP_TREMQ:
		fg_read_tremq(bq, &val);
		break;
	case FG_IC_PROP_TFULLCHGQ:
		fg_read_tfullchgq(bq, &val);
		break;
	case FG_IC_PROP_AVERCURRENT:
		fg_read_avercurrent(bq, &val);
		break;
	case FG_IC_PROP_SEAL:
		fg_get_seal(bq, &val);
		break;
	case FG_IC_PROP_DF_CHECK:
		fg_read_df_check(bq, &val);
		return scnprintf(buf, PAGE_SIZE, "%x\n", val);
	case FG_IC_PROP_CHEM_DF_SIGN:
		fg_read_chem_df_sign(bq, &val);
		return scnprintf(buf, PAGE_SIZE, "%x\n", val);
	case FG_IC_PROP_VENDOR_ID:
		val = bq->fg_vendor;
		break;
	case FG_IC_PROP_PACK_VENDOR_ID:
		val = bq->pack_vendor;
		break;
	case FG_IC_PROP_CELL_VENDOR_ID:
		return scnprintf(buf, PAGE_SIZE, "%s\n", bq->cell_name);
	case FG_IC_PROP_EEPROM_VERSION:
		return scnprintf(buf, PAGE_SIZE, "%c\n", bq->eeprom_version);
	case FG_IC_PROP_DESIGN_CAPACITY:
		fg_read_dc(bq, &val);
		break;
	case FG_IC_PROP_CALC_RVALUE:
		return scnprintf(buf, PAGE_SIZE, "%lu\n", fg_get_calc_rvalue(bq));
	case FG_IC_PROP_AGED_FLAG:
		val = bq->aged_flag;
		break;
	case FG_IC_PROP_UI_SOH:
		if (fg_mac_read_block(bq, FG_MAC_CMD_UI_SOH, data, 11) < 0)
			return -1;
		return scnprintf(buf, PAGE_SIZE,
				 "%u %u %u %u %u %u %u %u %u %u %u\n",
				 data[0], data[1], data[2], data[3], data[4],
				 data[5], data[6], data[7], data[8], data[9],
				 data[10]);
	case FG_IC_PROP_BATT_SN:
		if (fg_mac_read_block(bq, FG_MAC_CMD_BATT_SN, data, 32) < 0)
			return -1;
		return scnprintf(buf, PAGE_SIZE, "%s\n", data);
	case FG_IC_PROP_MANUFACTURING_DATE:
		if (fg_read_manufacturing_date(bq, data))
			return -1;
		return scnprintf(buf, PAGE_SIZE, "%s\n", data);
	case FG_IC_PROP_FIRST_USAGE_DATE:
		fg_read_first_usage_date(bq, data);
		return scnprintf(buf, PAGE_SIZE, "%s\n", data);
	case FG_IC_PROP_FAKE_FIRST_USAGE_DATE:
		val = bq->fake_first_usage_date;
		break;
	case FG_IC_PROP_SOH_NEW:
		val = bq->ui_soh;
		break;
	case FG_IC_PROP_DOD_COUNT:
		val = fg_get_dod_count(bq);
		break;
	case FG_IC_PROP_COUNT_LEVEL1:
		fg_get_one_count_level1(bq, &val);
		break;
	case FG_IC_PROP_COUNT_LEVEL2:
		fg_get_one_count_level2(bq, &val);
		break;
	case FG_IC_PROP_COUNT_LEVEL3:
		fg_get_one_count_level3(bq, &val);
		break;
	case FG_IC_PROP_COUNT_LOWTEMP:
		fg_get_one_count_lowtemp(bq, &val);
		break;
	case FG_IC_PROP_CUTOFF_VOL:
		fg_get_cutoff_voltage(bq, &val);
		break;
	case FG_IC_PROP_DEVICE_NAME:
		return scnprintf(buf, PAGE_SIZE, "%s", fg_get_device_name(bq));
	case FG_IC_PROP_MAX_TEMP_OCCUR_TIME:
		fg_read_max_temp_occur_time(bq, &val);
		break;
	case FG_IC_PROP_RUN_TIME:
		fg_read_run_time(bq, &val);
		break;
	case FG_IC_PROP_MAX_TEMP_TIME:
		fg_read_max_temp_time(bq, &val);
		break;
	case FG_IC_PROP_TOTAL_FW_RUN_TIME:
		fg_read_total_fw_runtime(bq, &val);
		break;
	case FG_IC_PROP_TEMP_MAX:
		fg_get_temp_max(bq, &val);
		break;
	case FG_IC_PROP_TIME_HT:
		fg_read_time_ht(bq, &val);
		break;
	case FG_IC_PROP_TIME_OT:
		fg_read_time_ot(bq, &val);
		break;
	case FG_IC_PROP_OVER_PEAK_FLAG:
		val = fg_get_over_peak_flag(bq);
		break;
	case FG_IC_PROP_MONITOR_ISC:
		fg_get_isc_alert_level(bq, &val);
		break;
	case FG_IC_PROP_MONITOR_SOA:
		fg_get_soa_alert_level(bq, &val);
		break;
	case FG_IC_PROP_CURRENT_DEVIATION:
		val = fg_get_current_dev(bq);
		break;
	case FG_IC_PROP_POWER_DEVIATION:
		val = fg_get_power_dev(bq);
		break;
	case FG_IC_PROP_AVERAGE_CURRENT:
		val = fg_get_average_current(bq);
		break;
	case FG_IC_PROP_AVERAGE_TEMPERATURE:
		val = fg_get_average_temp(bq);
		break;
	case FG_IC_PROP_START_LEARNING:
		val = fg_get_start_learning(bq);
		break;
	case FG_IC_PROP_STOP_LEARNING:
		val = fg_get_stop_learning(bq);
		break;
	case FG_IC_PROP_ACTUAL_POWER:
		val = fg_get_actual_power(bq);
		break;
	case FG_IC_PROP_LEARNING_POWER:
		val = fg_get_learning_power(bq);
		break;
	case FG_IC_PROP_LEARNING_POWER_DEV:
		val = fg_get_learning_power_dev(bq);
		break;
	case FG_IC_PROP_LEARNING_TIME_DEV:
		val = fg_get_learning_time_dev(bq);
		break;
	case FG_IC_PROP_CONSTANT_POWER:
		val = fg_get_constant_power(bq);
		break;
	case FG_IC_PROP_REMAINING_TIME:
		val = fg_get_remaining_time(bq);
		break;
	case FG_IC_PROP_REFERANCE_POWER:
		val = fg_get_referance_power(bq);
		break;
	case FG_IC_PROP_NVT_REFERANCE_POWER:
		val = fg_get_nvt_referance_power(bq);
		break;
	case FG_IC_PROP_REFERANCE_CURRENT:
		val = fg_get_referance_current(bq);
		break;
	case FG_IC_PROP_START_LEARNING_B:
		val = fg_get_start_learning_b(bq);
		break;
	case FG_IC_PROP_STOP_LEARNING_B:
		val = fg_get_stop_learning_b(bq);
		break;
	case FG_IC_PROP_LEARNING_POWER_B:
		val = fg_get_learning_power_b(bq);
		break;
	case FG_IC_PROP_ACTUAL_POWER_B:
		val = fg_get_actual_power_b(bq);
		break;
	case FG_IC_PROP_LEARNING_POWER_DEV_B:
		val = fg_get_learning_power_dev_b(bq);
		break;
	case FG_IC_PROP_REL_SOH:
		fg_get_rel_soh(bq, &val);
		break;
	case FG_IC_PROP_REL_SOH_CYCLECOUNT:
		fg_get_rel_soh_cyclecount(bq, &val);
		break;
	case FG_IC_PROP_EIS_SOH:
		fg_get_eis_soh(bq, &val);
		break;
	case FG_IC_PROP_EIS_SOH_CYCLECOUNT:
		fg_get_eis_soh_cyclecount(bq, &val);
		break;
	case FG_IC_PROP_QMAX_CYCLECOUNT:
		fg_get_qmax_cyclecount(bq, &val);
		break;
	case FG_IC_PROP_BATT_USE_ENVIRRONMENT: {
		u16 env[FG_BATT_USE_ENV_LEN / 2];

		if (fg_get_batt_use_environment(bq, data))
			return -1;
		for (i = 0; i < ARRAY_SIZE(env); i++)
			env[i] = get_unaligned_le16(&data[i * 2]);
		return scnprintf(buf, PAGE_SIZE,
				 "%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
				 env[0], env[1], env[2], env[3], env[4], env[5],
				 env[6], env[7], env[8], env[9], env[10], env[11],
				 env[12], env[13], env[14], env[15], env[16],
				 env[17], env[18]);
	}
	case FG_IC_PROP_MAX_LIFE_VOL:
		fg_get_max_life_vol(bq, &val);
		break;
	case FG_IC_PROP_MIN_LIFE_VOL:
		fg_get_min_life_vol(bq, &val);
		break;
	case FG_IC_PROP_MAX_LIFE_TEMP:
		fg_get_max_life_temp(bq, &byte_val);
		val = byte_val;
		break;
	case FG_IC_PROP_MIN_LIFE_TEMP:
		fg_get_min_life_temp(bq, &byte_val);
		val = byte_val;
		break;
	case FG_IC_PROP_OVER_VOL_DURATION:
		fg_get_over_vol_duration(bq, &val);
		break;
	case FG_IC_PROP_FC:
		fg_read_status(bq);
		val = bq->batt_fc;
		break;
	case FG_IC_PROP_CSD_FLAG:
		fg_get_csd_flag(bq, &val);
		break;
	case FG_IC_PROP_CSD_R1:
		fg_get_csd_r1(bq, &val);
		break;
	case FG_IC_PROP_CSD_R2:
		fg_get_csd_r2(bq, &val);
		break;
	case FG_IC_PROP_DCR_SLOPE1:
		fg_get_dcr_slope1(bq, &val);
		break;
	case FG_IC_PROP_DCR_SLOPE2:
		fg_get_dcr_slope2(bq, &val);
		break;
	case FG_IC_PROP_DCR_SLOPE3:
		fg_get_dcr_slope3(bq, &val);
		break;
	case FG_IC_PROP_CHARGE_ABSOC:
		fg_get_charge_absoc(bq, &val);
		break;
	case FG_IC_PROP_MAX_DSG_CURRENT:
		fg_get_max_dsg_current(bq);
		return scnprintf(buf, PAGE_SIZE, "%hd\n",
				 (short)bq->max_dsg_current);
	case FG_IC_PROP_MIN_TEMP:
		fg_get_min_temp(bq);
		val = bq->min_temp;
		break;
	case FG_IC_PROP_MIN_VOLTAGE:
		fg_get_min_voltage(bq);
		val = bq->min_voltage;
		break;
	case FG_IC_PROP_CUV_COUNT:
		fg_get_cuv_count(bq);
		val = bq->cuv_count;
		break;
	case FG_IC_PROP_CUV_LAST_CYCLE:
		fg_get_cuv_last_cycle(bq);
		val = bq->cuv_last_cycle;
		break;
	case FG_IC_PROP_HCUV_COUNT:
		fg_get_hcuv_count(bq);
		val = bq->hcuv_count;
		break;
	case FG_IC_PROP_HCUV_LAST_CYCLE:
		fg_get_hcuv_last_cycle(bq);
		val = bq->hcuv_last_cycle;
		break;
	case FG_IC_PROP_OCD_COUNT:
		fg_get_ocd_count(bq);
		val = bq->ocd_count;
		break;
	case FG_IC_PROP_OCD_LAST_CYCLE:
		fg_get_ocd_last_cycle(bq);
		val = bq->ocd_last_cycle;
		break;
	case FG_IC_PROP_HOCD_COUNT:
		fg_get_hocd_count(bq);
		val = bq->hocd_count;
		break;
	case FG_IC_PROP_HOCD_LAST_CYCLE:
		fg_get_hocd_last_cycle(bq);
		val = bq->hocd_last_cycle;
		break;
	case FG_IC_PROP_HSCD_COUNT:
		fg_get_hscd_count(bq);
		val = bq->hscd_count;
		break;
	case FG_IC_PROP_HSCD_LAST_CYCLE:
		fg_get_hscd_last_cycle(bq);
		val = bq->hscd_last_cycle;
		break;
	case FG_IC_PROP_CO_AUTO_OPEN:
		fg_read_co_auto_open(bq, &val);
		break;
	default:
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t fg_sysfs_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct bq_fg_chip *bq = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *attr_info;
	int val = 0;

	attr_info = mca_sysfs_lookup_attr(attr->attr.name, fg_sysfs_field_tbl,
					  FG_SYSFS_ATTRS_SIZE);
	if (!attr_info)
		return -1;

	if (kstrtoint(buf, 10, &val))
		val = 0;

	switch (attr_info->sysfs_attr_name) {
	case FG_IC_PROP_CYCLE:
		fg_set_cyclecount(bq, val);
		break;
	case FG_IC_PROP_SOH:
		fg_set_fake_soh(bq, val);
		break;
	case FG_IC_PROP_SEAL:
		fg_set_seal(bq, val);
		break;
	case FG_IC_PROP_UI_SOH:
		fg_store_ui_soh(bq, buf, count);
		break;
	case FG_IC_PROP_FIRST_USAGE_DATE:
		fg_write_first_usage_date(bq, buf, count);
		break;
	case FG_IC_PROP_FAKE_FIRST_USAGE_DATE:
		bq->fake_first_usage_date = !!val;
		break;
	case FG_IC_PROP_COUNT_LEVEL1:
		bq->fake_count_level1 = val;
		break;
	case FG_IC_PROP_COUNT_LEVEL2:
		bq->fake_count_level2 = val;
		break;
	case FG_IC_PROP_COUNT_LEVEL3:
		bq->fake_count_level3 = val;
		break;
	case FG_IC_PROP_COUNT_LOWTEMP:
		/* stock writes the live counter here, not the fake one */
		bq->count_lowtemp = val;
		break;
	case FG_IC_PROP_DEVICE_NAME:
		strscpy(bq->fake_device_name, buf, sizeof(bq->fake_device_name));
		break;
	case FG_IC_PROP_MONITOR_ISC:
		fg_set_isc_alert_level(bq, val);
		break;
	case FG_IC_PROP_START_LEARNING:
		fg_nvt_start(bq, NVT_FG_REG_START_LEARNING, "START_LEARNING");
		break;
	case FG_IC_PROP_STOP_LEARNING:
		fg_nvt_start(bq, NVT_FG_REG_STOP_LEARNING, "STOP_LEARNING");
		break;
	case FG_IC_PROP_LEARNING_POWER:
		fg_nvt_set_power(bq, NVT_FG_REG_EST_POWER, "EST_POWER", val);
		break;
	case FG_IC_PROP_CONSTANT_POWER:
		fg_nvt_set_power(bq, NVT_FG_REG_CONST_POWER, "CONST_POWER", val);
		break;
	case FG_IC_PROP_REFERANCE_POWER:
		fg_nvt_set_power(bq, NVT_FG_REG_REF_POWER, "REF_POWER", val);
		break;
	case FG_IC_PROP_START_LEARNING_B:
		fg_nvt_start(bq, NVT_FG_REG_START_LEARNING_B, "START_LEARNING_B");
		break;
	case FG_IC_PROP_STOP_LEARNING_B:
		fg_nvt_start(bq, NVT_FG_REG_STOP_LEARNING_B, "STOP_LEARNING_B");
		break;
	case FG_IC_PROP_LEARNING_POWER_B:
		fg_nvt_set_power(bq, NVT_FG_REG_EST_POWER_B, "EST_POWER_B", val);
		break;
	default:
		break;
	}

	return count;
}

static void fg_sysfs_create_group(struct bq_fg_chip *bq)
{
	mca_sysfs_init_attrs(fg_sysfs_attrs, fg_sysfs_field_tbl,
			     FG_SYSFS_ATTRS_SIZE);

	if (bq->dev_role == FG_MASTER)
		bq->sysfs_dev = mca_sysfs_create_group("xm_power", "fg_master",
						       &fg_sysfs_attr_group);
	else if (bq->dev_role == FG_SLAVE)
		bq->sysfs_dev = mca_sysfs_create_group("xm_power", "fg_slave",
						       &fg_sysfs_attr_group);
	else
		mca_log_info("invalid device");

	if (bq->sysfs_dev) {
		dev_set_drvdata(bq->sysfs_dev, bq);
		mca_log_err("create fg sysfs success\n");
	}
}

/*
 * Charge log
 */
static int bq27z561_dump_log_head(void *data, char *buf, int size)
{
	struct bq_fg_chip *bq = data;

	if (!bq)
		return 0;

	if (bq->dev_role == 0)
		return snprintf(buf, size,
				"fg_type Cell      RSoc Soh Volt Curr    RM    FCC   Qmax  Temp SOA ");

	return snprintf(buf, size,
			"fg_type1 Cell1     RSoc1 Soh1 Volt1 Curr1   RM1   FCC1  Qmax1 Temp1 SOA1 ");
}

static int bq27z561_dump_log_context(void *data, char *buf, int size)
{
	struct bq_fg_chip *bq = data;
	int rsoc = 0, soh = 0, volt = 0, curr = 0, rm = 0, fcc = 0;
	int qmax = 0, temp = 0, soa = 0;

	if (!bq)
		return 0;

	fg_read_rsoc(bq, &rsoc);
	fg_read_soh(bq, &soh);
	fg_read_volt(bq, &volt);
	fg_read_current(bq, &curr);
	fg_read_rm(bq, &rm);
	fg_read_fcc(bq, &fcc);
	fg_read_qmax(bq, &qmax);
	fg_read_temperature(bq, &temp);
	fg_get_soa_alert_level(bq, &soa);

	if (bq->dev_role == 0)
		return snprintf(buf, size,
				"%-8d%-10s%-5d%-4d%-5d%-8d%-6d%-6d%-6d%-5d%-4d",
				bq->fg_vendor, bq->cell_name, rsoc, soh, volt,
				curr / 1000, rm / 1000, fcc / 1000, qmax, temp,
				soa);

	return snprintf(buf, size, "%-9d%-10s%-6d%-5d%-6d%-8d%-6d%-6d%-6d%-6d%-5d",
			bq->fg_vendor, bq->cell_name, rsoc, soh, volt,
			curr / 1000, rm / 1000, fcc / 1000, qmax, temp, soa);
}

static struct mca_log_charge_log_ops g_bq27z516_log_ops = {
	.dump_log_head = bq27z561_dump_log_head,
	.dump_log_context = bq27z561_dump_log_context,
};

/*
 * Probe
 */
static void bq_parse_dt(struct bq_fg_chip *bq)
{
	struct device_node *node = bq->dev->of_node;
	int size = 0;
	int ret;

	of_get_property(node, "bq,soc_decimal_rate", &size);
	if (size) {
		bq->dec_rate_seq = devm_kzalloc(bq->dev, size, GFP_KERNEL);
		if (bq->dec_rate_seq) {
			bq->dec_rate_len = size / sizeof(*bq->dec_rate_seq);
			if (bq->dec_rate_len % 2) {
				mca_log_info("invalid soc decimal rate seq\n");
				return;
			}
			ret = of_property_read_u32_array(node, "bq,soc_decimal_rate",
							 bq->dec_rate_seq,
							 bq->dec_rate_len);
			if (ret) {
				mca_log_info("Failed to get bq,soc_decimal_rate!\n");
				return;
			}
		} else {
			mca_log_info("error allocating memory for dec_rate_seq\n");
		}
	}

	mca_parse_dts_u32(node, "bq,charge-full-design", (u32 *)&bq->batt_dc,
			  DEFUALT_FULL_DESIGN);
	mca_parse_dts_u32(node, "ic_role", &bq->dev_role, FG_MASTER);
	mca_parse_dts_u32(node, "soh_th", &bq->soh_th, FG_IC_SOH_TH_DEFAUL);
	mca_parse_dts_u32(node, "cycle_th", &bq->cycle_th, FG_IC_CYCLE_TH_DEFAUL);
	mca_parse_dts_u32(node, "cycle_step", &bq->cycle_step,
			  FG_IC_CYCLE_STEP_DEFAUL);
	mca_parse_dts_u32(node, "resistance-id", (u32 *)&bq->resistance_id,
			  FG_IC_RESISTANCE_ID_DEFAUL);
	mca_parse_dts_u32(node, "support_nvt1000_ota", &bq->support_nvt1000_ota, 0);
	mca_parse_dts_u8_array(node, "unseal_key", bq->key, BATTERY_UNSEAL_KEY_LEN);
	mca_log_err("0x%x,0x%x,0x%x,0x%x\n", bq->key[0], bq->key[1], bq->key[2],
		    bq->key[3]);

	bq->support_voltage_record_level =
		of_property_read_bool(node, "support-voltage-record-level");
	if (bq->support_voltage_record_level) {
		mca_parse_dts_u8(node, "version_number", &bq->version_number, 0);
		mca_parse_dts_u8(node, "start_byte_address", &bq->start_byte_address, 0);
		mca_parse_dts_u8(node, "byte_length", &bq->byte_length, 0);
		size = of_property_count_u8_elems(node, "record_voltage");
		if (size < 0) {
			mca_log_err("record_voltage is not exist\n");
			bq->update_fw_status = FG_FW_INVALID;
		} else {
			mca_log_info("record_voltage size %d, reg_length %d\n",
				     size, bq->byte_length);
			bq->record_voltage = devm_kzalloc(bq->dev, size, GFP_KERNEL);
			if (bq->record_voltage) {
				bq->record_voltage_len = size;
				mca_parse_dts_u8_array(node, "record_voltage",
						       bq->record_voltage, size);
			} else {
				bq->update_fw_status = FG_FW_INVALID;
			}
		}
		if (!bq->version_number || !bq->start_byte_address ||
		    !bq->byte_length) {
			bq->update_fw_status = FG_FW_INVALID;
			mca_log_err("OTA INFO ERROR [0x%x][0x%x][0x%x]\n",
				    bq->version_number, bq->start_byte_address,
				    bq->byte_length);
		}
	}

	bq->support_version_compatible =
		of_property_read_bool(node, "support-version-compatible");

	bq->co_by_gpio = of_property_read_bool(node, "co_by_gpio");
	if (bq->co_by_gpio) {
		bq->ctl_co_gpio = of_get_named_gpio(node, "batt_ctl", 0);
		if (bq->ctl_co_gpio < 0) {
			mca_log_err("failed to parse ctl_co_gpio\n");
		} else {
			if (devm_gpio_request(bq->dev, bq->ctl_co_gpio,
					      dev_name(bq->dev)))
				mca_log_err("%s unable to request ctl_co gpio [%d]\n",
					    __func__, bq->ctl_co_gpio);
			else if (gpiod_direction_output_raw(gpio_to_desc(bq->ctl_co_gpio), 0))
				mca_log_err("%s unable to set direction for ctl_co gpio [%d]\n",
					    __func__, bq->ctl_co_gpio);
			mca_log_info("ctl_co_gpio[%d] val is :%d\n", bq->ctl_co_gpio,
				     gpiod_get_raw_value(gpio_to_desc(bq->ctl_co_gpio)));
		}
	}
}

static void fg_set_term_config(struct bq_fg_chip *bq)
{
	u8 data[2] = { FG_TERM_CURRENT & 0xff, FG_TERM_CURRENT >> 8 };
	int ret;

	if (bq->fg_vendor != BQ27Z561)
		return;

	ret = fg_mac_write_block(bq, FG_MAC_CMD_TERM_CURRENTS, data, 2);
	if (ret < 0)
		mca_log_info("could not write term current = %d\n", ret);
}

static void fg_set_force_sim_soh(struct bq_fg_chip *bq)
{
	u8 wbuf[2] = { 0 };
	u8 flag[2] = { 0 };
	int ret, i;

	for (i = 2; i >= 0; i--) {
		ret = fg_mac_write_block(bq, FG_MAC_CMD_FORCE_SIM_SOH, wbuf, 0);
		if (ret < 0) {
			mca_log_info("could not write 0x56 to 0x3E %d\n", ret);
			return;
		}
		msleep(30);

		if (fg_mac_read_block(bq, FG_MAC_CMD_MIXDATARECORD1, flag, 2) < 0) {
			mca_log_info("failed to get 0x2001 data\n");
			return;
		}
		mca_log_info("flag = %d, try_conut = %d\n", flag[0] & 4, i);
		if (flag[0] & 4)
			return;
	}
}

/*
 * On Chinese O9 units carrying an NVT "3XM31" pack, ask the gauge to use its
 * simulated state of health.
 */
static void fg_force_sim_soh_if_needed(struct bq_fg_chip *bq,
				       struct mca_hwid_info *hwid)
{
	u8 name[8] = { 0 };
	int ret;

	if (!hwid || hwid->platform_version != HARDWARE_PROJECT_O9 ||
	    hwid->country_version != 0) {
		mca_log_err("not miro CN device, none to do!\n");
		goto out;
	}

	memset(bq->device_name, 0, sizeof(bq->device_name));
	ret = fg_mac_read_block(bq, FG_MAC_CMD_DEVICE_NAME, name, 8);
	if (ret < 0) {
		mca_log_err("could not read device_name, ret=%d\n", ret);
		return;
	}
	memcpy(bq->device_name, name, 5);

	if (!strcmp("3XM31", bq->device_name))
		fg_set_force_sim_soh(bq);
	else
		mca_log_err("not CN NVT device, none to do!\n");

out:
	mca_log_err("FuelGaugw probe successfully, role = %u\n", bq->dev_role);
}

static int bq_fg_probe(struct i2c_client *client)
{
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	static int probe_cnt;
	struct bq_fg_chip *bq;
	u8 data[32] = { 0 };
	u16 flags = 0;
	int ret;

	mca_log_info("probe_cnt = %d\n", ++probe_cnt);

	bq = devm_kzalloc(&client->dev, sizeof(*bq), GFP_KERNEL);
	if (!bq)
		return -ENOMEM;

	bq->dev = &client->dev;
	bq->client = client;
	bq->ui_soh = 100;
	bq->fake_temp = FG_FAKE_TEMP_NONE;
	bq->fake_soh = FG_FAKE_SOH_NONE;
	bq->batt_fcc = FG_RM_FCC_DEFAULT;
	i2c_set_clientdata(client, bq);
	mutex_init(&bq->i2c_rw_lock);
	mutex_init(&bq->data_lock);
	atomic_set(&bq->pm_suspend, 0);
	atomic_set(&bq->digest_in_process, 0);
	bq->update_fw_status = FG_FW_INIT;
	bq->fake_device_name[0] = '\0';

	bq_parse_dt(bq);
	memcpy(bq->regs, bq27z561_regs, NUM_REGS);

	/* a gauge sitting in its boot loader does not answer MAC reads */
	if (bq->support_nvt1000_ota)
		ret = fg_read_word(bq, bq->regs[BQ_FG_REG_BATT_STATUS], &flags);
	else
		ret = fg_mac_read_block(bq, FG_MAC_CMD_MANU_NAME, data, 32);
	if (ret < 0) {
		mca_log_err("fg device found: %d\n", ret);
		ret = -EPROBE_DEFER;
		if (probe_cnt < PROBE_CNT_MAX)
			return ret;
		goto out;
	} else if (ret) {
		mca_log_err("fg cksum_calc failed\n");
	}

	if (bq->support_nvt1000_ota)
		nfg1000_boot_app_judge(bq);

	device_init_wakeup(bq->dev, true);
	fg_set_term_config(bq);
	fg_get_manufacture_data(bq);
	fg_set_fastcharge_mode(bq, false);
	fg_read_cell_vendor(bq);
	fg_read_pack_vendor(bq);
	fg_read_vendor(bq);
	fg_read_fw_version(bq);
	fg_read_eeprom_version(bq);

	ret = platform_fg_ic_ops_register(bq->dev_role, bq, &g_bq_fg_ops);
	if (ret) {
		mca_log_err("reg ops fail, role = %u\n", bq->dev_role);
		goto out;
	}

	fg_dump_battery_abnormal_info(bq);
	fg_force_sim_soh_if_needed(bq, hwid);

out:
	fg_sysfs_create_group(bq);
	i2c_set_clientdata(client, bq);
	if (bq->dev_role)
		mca_log_charge_log_register(MCA_CHARGE_LOG_ID_FG_MASTER_IC,
					    &g_bq27z516_log_ops, bq);
	else
		mca_log_charge_log_register(MCA_CHARGE_LOG_ID_FG_SLAVE_IC,
					    &g_bq27z516_log_ops, bq);
	if (ret == -EPROBE_DEFER)
		mca_strategy_func_process(STRATEGY_FUNC_TYPE_FG,
					  MCA_EVENT_BQ_FG_ERROR, 0);
	mca_log_err("%s !!\n", ret == -EPROBE_DEFER ? "Over probe cnt max" : "OK");

	return 0;
}

static int bq_fg_suspend(struct device *dev)
{
	struct bq_fg_chip *bq = dev_get_drvdata(dev);

	if (!bq)
		return 0;

	mca_log_info("suspend\n");
	atomic_set(&bq->pm_suspend, 1);

	return 0;
}

static int bq_fg_resume(struct device *dev)
{
	struct bq_fg_chip *bq = dev_get_drvdata(dev);

	if (!bq)
		return 0;

	mca_log_info("resume\n");
	atomic_set(&bq->pm_suspend, 0);

	return 0;
}

static void bq_fg_remove(struct i2c_client *client)
{
	struct bq_fg_chip *bq = i2c_get_clientdata(client);

	mca_sysfs_remove_group("xm_power", bq->sysfs_dev, &fg_sysfs_attr_group);
}

static void bq_fg_shutdown(struct i2c_client *client)
{
	mca_log_info("bq fuel gauge driver shutdown!\n");
}

static const struct dev_pm_ops bq_fg_pm_ops = {
	.suspend	= bq_fg_suspend,
	.resume		= bq_fg_resume,
};

static int bq_mode_data[] = {
	[BQ27Z561_MASTER] = FG_MASTER,
	[BQ27Z561_SLAVE] = FG_SLAVE,
	[BQ28Z610] = FG_MASTER,
};

static const struct of_device_id bq_fg_match_table[] = {
	{
		.compatible = "ti,bq27z561_master",
		.data = &bq_mode_data[BQ27Z561_MASTER],
	},
	{
		.compatible = "ti,bq27z561_slave",
		.data = &bq_mode_data[BQ27Z561_SLAVE],
	},
	{
		.compatible = "ti,bq28z610",
		.data = &bq_mode_data[BQ28Z610],
	},
	{},
};
MODULE_DEVICE_TABLE(of, bq_fg_match_table);

static const struct i2c_device_id bq_fg_id[] = {
	{ "bq27z561_master", BQ27Z561_MASTER },
	{ "bq27z561_slave", BQ27Z561_SLAVE },
	{ "bq28z610", BQ28Z610 },
	{},
};
MODULE_DEVICE_TABLE(i2c, bq_fg_id);

static struct i2c_driver bq_fg_driver = {
	.driver	= {
		.name = "bq27z561",
		.owner = THIS_MODULE,
		.of_match_table = bq_fg_match_table,
		.pm = &bq_fg_pm_ops,
	},
	.id_table = bq_fg_id,
	.probe = bq_fg_probe,
	.remove = bq_fg_remove,
	.shutdown = bq_fg_shutdown,
};

module_i2c_driver(bq_fg_driver);

MODULE_DESCRIPTION("TI BQ27Z561 Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Texas Instruments");
