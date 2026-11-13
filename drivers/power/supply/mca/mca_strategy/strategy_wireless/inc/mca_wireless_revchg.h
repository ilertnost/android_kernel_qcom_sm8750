/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Reverse wireless charging.
 *
 * The coil that charges this phone can also charge something placed on it.
 * Doing so takes a voltage higher than the battery's and a receiver that is
 * actually there, so it is not something the stack turns on lightly, and what
 * is exposed here is mostly the questions other modules need answered before
 * they act: is the coil running, is a firmware update in progress, did the
 * user ask for this.
 */

#ifndef __MCA_WIRELESS_REVCHG_H
#define __MCA_WIRELESS_REVCHG_H

#define MCA_WLS_REV_CHG_VOLTAGE_DEFAULT 9000
#define REVERSE_PING_TIMEOUT_TIMER		(20 * 1000)
#define REVERSE_TRANSFER_TIMEOUT_TIMER	(100 * 1000)
#define REVERSE_TEST_DELAY_MS			(2 * 1000)
#define REVERSE_FW_UPDATE_CNT_NUM 		(15 * 1000)
#define REVERSE_PPE_TIMEOUT_TIMER		(3 * 1000)
#define REVERSE_PEN_DELAY_TIMER			(10 * 1000)
#define RX_CHECK_SUCCESS				(1 << 0)
#define TX_CHECK_SUCCESS				(1 << 1)
#define BOOT_CHECK_SUCCESS				(1 << 2)
#define POWER_ON_UPDATE_TIMER			(10 * 1000)
#define PEN_SOC_FULL_COUNT 18

#include <linux/types.h>

struct notifier_block;

/* How far a firmware update has got. */
enum fw_update_status {
	FIRMWARE_NO_UPDATE,
	FIRMWARE_NEED_UPDATE,
	FIRMWARE_UPDATING,
	FIRMWARE_UPDATE_FINISH,
	FIRMWARE_UPDATE_ERROR,
};

/*
 * What kind of firmware update was asked for.  The numbering is the vendor's
 * and reaches this module as a number written to sysfs.
 */
enum FW_UPDATE_CMD {
	FW_UPDATE_POWER_ON	= 0,
	FW_UPDATE_ERASE		= 97,
	FW_UPDATE_USER		= 98,
	FW_UPDATE_CHECK		= 99,
	FW_UPDATE_FORCE		= 100,
	FW_UPDATE_FROM_BIN	= 101,
	FW_UPDATE_MAX		= 102,
};

/* What the coil is doing. */
enum reverse_charge_state {
	REVERSE_STATE_OPEN,
	REVERSE_STATE_TIMEOUT,
	REVERSE_STATE_ENDTRANS,
	REVERSE_STATE_FORWARD,
	REVERSE_STATE_TRANSFER,
	REVERSE_STATE_WAITPING,
};

/* Whether reverse charging is on, as userspace states it. */
enum reverse_charge_mode {
	REVERSE_CHARGE_CLOSE,
	REVERSE_CHARGE_OPEN,
};

/*
 * What the receiver chip reported.  It raises one interrupt for everything and
 * names the cause in a register.
 */
enum mca_rev_chg_int_flag {
	RTX_INT_UNKNOWN,
	RTX_INT_PING,
	RTX_INT_GET_RX,
	RTX_INT_CEP_TIMEOUT,
	RTX_INT_EPT,
	RTX_INT_PROTECTION,
	RTX_INT_GET_TX,
	RTX_INT_REVERSE_TEST_READY,
	RTX_INT_REVERSE_TEST_DONE,
	RTX_INT_FOD,
	RTX_INT_EPT_PKT,
	RTX_INT_ERR_CODE,
	RTX_INT_TX_DET_RX,
	RTX_INT_TX_CONFIG,
	RTX_INT_TX_CHS_UPDATE,
	RTX_INT_TX_BLE_CONNECT,
};

/* What userspace can read and write about reverse charging. */
enum mca_rev_chg_attr_list {
	MCA_REV_CHG_WIRELESS_CHIP_FW,
	MCA_REV_CHG_WLS_FW_STATE,
	MCA_REV_CHG_REVERSE_CHG_MODE,
	MCA_REV_CHG_REVERSE_CHG_STATE,
	MCA_REV_CHG_PEN_SOC,
	MCA_REV_CHG_PEN_HALL3,
	MCA_REV_CHG_PEN_HALL4,
	MCA_REV_CHG_PEN_HALL3_S,
	MCA_REV_CHG_PEN_HALL4_S,
	MCA_REV_CHG_PEN_PPE_HALL_N,
	MCA_REV_CHG_PEN_PPE_HALL_S,
	MCA_REV_CHG_PEN_SS_VOLTAGE,
	MCA_REV_CHG_WLS_TX_VOUT,
	MCA_REV_CHG_WLS_TX_IOUT,
	MCA_REV_CHG_WLS_TX_TDIE,
	MCA_REV_CHG_PEN_PLACE_ERR,
};

int mca_wireless_rev_enable_reverse_charge(bool enable);

int mca_wireless_rev_get_reverse_chg(bool *reverse_chg_en);
int mca_wireless_rev_set_reverse_chg(bool reverse_chg_en);
int mca_wireless_rev_get_reverse_chg_state(int *reverse_chg_sts);

int mca_wireless_rev_get_user_reverse_chg(bool *user_reverse_chg);
int mca_wireless_rev_set_user_reverse_chg(bool user_reverse_chg);

int mca_wireless_rev_get_rev_boost_default(int *rev_boost_default);
int mca_wireless_rev_set_usb_plugin(bool wls_sleep_usb_insert);
int mca_wireless_rev_set_wired_chg_ok(bool wired_chg_ok);

int mca_wireless_rev_get_fw_update(bool *fw_update);
int mca_wireless_rev_set_firmware_state(int fw_update_state);
int mca_wireless_rev_get_firmware_state(int *fw_update_state);
int mca_wireless_rev_update_fw_version(int cmd);

/*
 * The stylus reports its own charge level over the coil, and the input driver
 * that owns the stylus is not part of the charging stack.
 */
int pen_charge_state_notifier_register_client(struct notifier_block *nb);
int pen_charge_state_notifier_unregister_client(struct notifier_block *nb);

/* Where the voltage a coil is driven from comes from. */
enum mca_wireless_rev_boost_src {
	PMIC_REV_BOOST,
	PMIC_HBOOST,
	EXTERNAL_BOOST,
	CHARGER_ADAPTER,
	BOOST_SRC_MAX,
};

struct mca_wireless_rev_proc_data {
	bool wireless_reverse_closing;
	bool reverse_chg_en;
	bool user_reverse_chg;
	bool bc12_reverse_chg;
	bool batt_missing;
	bool wired_chg_ok;
	int reverse_chg_sts;
	int int_flag;
	//firmware update
	bool fw_updating;
	bool only_check;
	bool from_bin;
	bool force_download;
	bool user_update;
	bool fw_erase;
	bool power_on_update;
	bool wls_sleep_fw_update;
	bool wls_sleep_usb_insert;
	int firmware_update_state;
	bool fw_update_pen_online;
	int pen_ss_voltage;
	bool revchg_config_close;
};

struct mca_wireless_revchg {
	struct device *dev;
	struct mca_votable *usbin_rev_disable_voter;

	//notifier
	struct notifier_block shutdown_notifier;
	//delayed_work
	struct delayed_work monitor_work;
	struct delayed_work reverse_charge_config_work;
	struct delayed_work tx_ping_timeout_work;
	struct delayed_work tx_transfer_timeout_work;
	struct delayed_work disable_tx_work;
	struct delayed_work enable_tx_work;
	struct delayed_work rev_update_to_wire_work;
	struct delayed_work rev_update_to_boost_work;
	struct delayed_work fw_update_work;
	struct delayed_work poweron_update_work;
	struct delayed_work reverse_test_start_work;
	struct delayed_work reverse_test_stop_work;
	struct delayed_work pen_place_err_check_work;
	struct delayed_work pen_data_handle_work;

	//dt config
	int rev_boost_src;
	int rev_boost_default;
	int rev_boost_voltage;
	int support_tx_only;

	struct mca_wireless_rev_proc_data proc_data;

	bool is_switching;
	bool wait_for_reverse_test;
	int force_stop;
	bool tx_timeout_flag;
};

/*
 * Charge type handed to the coil driver once reverse charging is up in
 * stylus-only mode.  This is the only value the stock module ever passes and
 * no shipped coil driver implements the op, so the rest of the space is
 * unknown.
 */
#define WLS_TX_CHARGE_TYPE_PEN	4

/* The one transmitter DFX code the driver acts on itself. */
#define RTX_DFX_CODE_TRX_OCP	0x1f

#endif /* __MCA_WIRELESS_REVCHG_H */
