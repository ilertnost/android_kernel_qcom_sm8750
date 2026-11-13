/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Wireless charging.
 *
 * The receiver coil is a charger like any other as far as the strategy is
 * concerned, but getting anything out of it means talking to the transmitter
 * through the coil itself: renegotiating power, reading how warm the pad has
 * become, telling a fan to spin faster.  All of that goes through the chip
 * that drives the coil, and this is what the stack asks it.
 *
 * The same interface serves reverse charging, where the phone is the
 * transmitter and something else -- a stylus, a pair of earbuds -- is the
 * receiver.
 */

#ifndef __MCA_PLATFORM_WIRELESS_H
#define __MCA_PLATFORM_WIRELESS_H

#include <linux/types.h>

/* Which coil. */
enum platform_class_wireless_role {
	WIRELESS_ROLE_MASTER,
	WIRELESS_ROLE_SLAVE,
	WIRELESS_ROLE_MAX,
};

/* What userspace can read and set on one coil. */
enum wireless_attr_list {
	RX_SYSFS_RX_VOUT,
	RX_SYSFS_RX_VRECT,
	RX_SYSFS_RX_IOUT,
	RX_SYSFS_FW_VERSION,
	RX_SYSFS_FW_BIN,
	RX_SYSFS_SLEEP_RX,
	RX_SYSFS_TX_ADAPTER,
	RX_SYSFS_BT_STATE,
	RX_SYSFS_RX_CEP,
	RX_SYSFS_RX_CR,
	RX_SYSFS_TX_MAC,
	RX_SYSFS_WLS_DIE_TEMP,
	RX_SYSFS_WLS_TX_SPEED,
	RX_SYSFS_RX_SS,
	RX_SYSFS_RX_OFFSET,
	RX_SYSFS_RX_SLEEP_MODE,
	RX_SYSFS_TX_UUID,
};

/* How much of the foreign object detection tuning a test is overriding. */
typedef enum {
	WLS_DEBUG_SET_FOD_NONE,
	WLS_DEBUG_SET_FOD_EPP_ONE,
	WLS_DEBUG_SET_FOD_EPP_ALL,
	WLS_DEBUG_SET_FOD_ALL_DIRECTLY,
} WLS_DEBUG_SET_FOD_TYPE;

/**
 * struct platform_class_wireless_ops - what a coil driver provides
 *
 * Every call takes the @data the coil driver registered as its last argument.
 * An entry left NULL means the coil cannot do that, and the caller is told so
 * rather than being given a wrong answer.
 */
struct platform_class_wireless_ops {
	int (*wls_enable_reverse_chg)(bool enable, void *data);
	int (*wls_is_present)(int *present, void *data);
	int (*wls_set_vout)(int vout, void *data);
	int (*wls_get_vout)(int *vout, void *data);
	int (*wls_get_iout)(int *iout, void *data);
	int (*wls_get_vrect)(int *vrect, void *data);
	int (*wls_get_temp)(int *temp, void *data);
	int (*wls_get_tx_adapter)(int *tx_adapter, void *data);
	int (*wls_get_tx_adapter_by_i2c)(int *tx_adapter_by_i2c, void *data);
	int (*wls_set_enable_mode)(bool enable, void *data);
	int (*wls_is_car_adapter)(bool *car_adapter, void *data);
	int (*wls_set_fw_bin)(const char *name, int size, void *data);
	int (*wls_get_rx_rtx_mode)(int *rx_rtx_mode, void *data);
	int (*wls_set_input_current_limit)(int input_current_limit, void *data);
	int (*wls_get_rx_int_flag)(int *rx_int_flag, void *data);
	int (*wls_get_rx_power_mode)(u8 *rx_power_mode, void *data);
	int (*wls_get_tx_max_power)(u8 *tx_max_power, void *data);
	int (*wls_get_auth_value)(int *auth_value, void *data);
	int (*wls_set_adapter_voltage)(int adapter_voltage, void *data);
	int (*wls_get_tx_uuid)(u8 *tx_uuid, void *data);
	int (*wls_set_fod_params)(int fod_params, void *data);
	int (*wls_get_rx_fastcharge_status)(u8 *rx_fastcharge_status, void *data);
	int (*wls_receive_transparent_data)(u8 *buf, int size, int *len, void *data);
	int (*wls_send_transparent_data)(u8 *buf, u8 size, void *data);
	int (*wls_get_ss_voltage)(int *ss_voltage, void *data);
	int (*wls_do_renego)(u8 do_renego, void *data);
	int (*wls_set_parallel_charge)(bool enable, void *data);
	int (*wls_get_vout_setted)(int *vout_setted, void *data);
	int (*wls_get_poweroff_err_code)(u8 *poweroff_err_code, void *data);
	int (*wls_get_rx_err_code)(u8 *err_code, u8 *sub_err_code, void *data);
	int (*wls_get_tx_err_code)(u8 *err_code, u8 *sub_err_code, void *data);
	int (*wls_get_project_vendor)(int *project_vendor, void *data);
	int (*wls_get_fw_version)(char *fw_version, void *data);
	int (*wls_check_i2c_is_ok)(void *data);
	int (*wls_enable_rev_fod)(bool enable, void *data);
	int (*wls_send_tx_q_value)(u8 send_tx_q_value, void *data);
	int (*wls_download_fw_from_bin)(void *data);
	int (*wls_erase_fw)(void *data);
	int (*wls_get_fw_version_check)(u8 *fw_version_check, void *data);
	int (*wls_download_fw)(void *data);
	int (*wls_set_confirm_data)(void *data, u8 confirm_data);
	int (*wls_receive_test_cmd)(u8 *buf, int *len, void *data);
	int (*wls_process_factory_cmd)(u8 process_factory_cmd, void *data);
	int (*wls_get_hall_gpio_status)(bool *hall_gpio_status, void *data);
	int (*wls_get_magnetic_case_flag)(bool *magnetic_case_flag, void *data);
	int (*wls_check_firmware_state)(bool *check_firmware_state, void *data);
	int (*wls_set_debug_fod)(int *params, int size, void *data);
	int (*wls_get_debug_fod_type)(WLS_DEBUG_SET_FOD_TYPE *type,
				      void *data);
	int (*wls_set_debug_fod_params)(void *data);
	int (*wls_set_tx_fan_speed)(int tx_fan_speed, void *data);
	int (*wls_get_tx_fan_speed)(int *tx_fan_speed, void *data);
	int (*wls_get_rx_offset)(int *rx_offset, void *data);
	int (*wls_set_rx_offset)(int rx_offset, void *data);
	int (*wls_set_rx_sleep_mode)(int rx_sleep_mode, void *data);
	int (*wls_enable_vsys_ctrl)(bool enable, void *data);
	int (*wls_get_trx_isense)(int *trx_isense, void *data);
	int (*wls_get_trx_vrect)(int *trx_vrect, void *data);
	int (*wls_get_phone_case_category)(int *phone_case_category, void *data);
	int (*wls_set_phone_case_category)(int phone_case_category, void *data);
	int (*wls_get_fw_upgrade_fail_info)(const char **info, void *data);
	int (*wls_get_rsv_eppmode_fail)(int *rsv_eppmode_fail, void *data);
	int (*wls_switch_bridge)(u8 switch_bridge, void *data);
	int (*wls_get_rx_brg_status)(u8 *rx_brg_status, void *data);
	int (*wls_set_external_boost_enable)(bool enable, void *data);
	int (*wls_get_tx_vout)(int *tx_vout, void *data);
	int (*wls_get_tx_iout)(int *tx_iout, void *data);
	int (*wls_get_pen_mac)(u64 *pen_mac, void *data);
	int (*wls_get_pen_soc)(int *pen_soc, void *data);
	int (*wls_get_pen_full_flag)(int *pen_full_flag, void *data);
	int (*wls_get_pen_hall3)(int *pen_hall3, void *data);
	int (*wls_get_pen_hall4)(int *pen_hall4, void *data);
	int (*wls_get_pen_hall3_s)(int *pen_hall3_s, void *data);
	int (*wls_get_pen_hall4_s)(int *pen_hall4_s, void *data);
	int (*wls_get_pen_hall_ppe_n)(int *pen_hall_ppe_n, void *data);
	int (*wls_get_pen_hall_ppe_s)(int *pen_hall_ppe_s, void *data);
	int (*wls_get_pen_place_err)(int *pen_place_err, void *data);
	int (*wls_set_pen_place_err)(int pen_place_err, void *data);
	int (*wls_set_hboost_enable)(int hboost_enable, void *data);
	int (*wls_set_charge_type)(u8 charge_type, void *data);
	int (*wls_get_reverse_chg_en)(int *reverse_chg_en, void *data); };

int platform_class_wireless_register_ops(enum platform_class_wireless_role role,
					 void *data,
					 const struct platform_class_wireless_ops *ops);

int platform_class_wireless_check_firmware_state(
						 enum platform_class_wireless_role role,
						 bool *check_firmware_state);
int platform_class_wireless_check_i2c_is_ok(
					    enum platform_class_wireless_role role);
int platform_class_wireless_do_renego(enum platform_class_wireless_role role,
				      u8 do_renego);
int platform_class_wireless_download_fw(enum platform_class_wireless_role role);
int platform_class_wireless_download_fw_from_bin(
						 enum platform_class_wireless_role role);
int platform_class_wireless_enable_rev_fod(
					   enum platform_class_wireless_role role,
					   bool enable);
int platform_class_wireless_enable_reverse_chg(
					       enum platform_class_wireless_role role,
					       bool enable);
int platform_class_wireless_enable_vsys_ctrl(
					     enum platform_class_wireless_role role,
					     bool enable);
int platform_class_wireless_erase_fw(enum platform_class_wireless_role role);
int platform_class_wireless_get_auth_value(
					   enum platform_class_wireless_role role,
					   int *auth_value);
int platform_class_wireless_get_debug_fod_type(enum platform_class_wireless_role role,
					       WLS_DEBUG_SET_FOD_TYPE *type);
int platform_class_wireless_get_fw_upgrade_fail_info(
						     enum platform_class_wireless_role role,
						     const char **fail_info);
int platform_class_wireless_get_fw_version(
					   enum platform_class_wireless_role role,
					   char *fw_version);
int platform_class_wireless_get_fw_version_check(
						 enum platform_class_wireless_role role,
						 u8 *check_result);
int platform_class_wireless_get_hall_gpio_status(
						 enum platform_class_wireless_role role,
						 bool *hall_gpio_status);
int platform_class_wireless_get_iout(enum platform_class_wireless_role role,
				     int *iout);
int platform_class_wireless_get_magnetic_case_flag(
						   enum platform_class_wireless_role role,
						   bool *magnetic_case_flag);
int platform_class_wireless_get_pen_full_flag(
					      enum platform_class_wireless_role role,
					      int *pen_full);
int platform_class_wireless_get_pen_hall3(
					  enum platform_class_wireless_role role,
					  int *pen_hall3);
int platform_class_wireless_get_pen_hall3_s(
					    enum platform_class_wireless_role role,
					    int *pen_hall3_s);
int platform_class_wireless_get_pen_hall4(
					  enum platform_class_wireless_role role,
					  int *pen_hall4);
int platform_class_wireless_get_pen_hall4_s(
					    enum platform_class_wireless_role role,
					    int *pen_hall4_s);
int platform_class_wireless_get_pen_hall_ppe_n(
					       enum platform_class_wireless_role role,
					       int *pen_hall_ppe_n);
int platform_class_wireless_get_pen_hall_ppe_s(
					       enum platform_class_wireless_role role,
					       int *pen_hall_ppe_s);
int platform_class_wireless_get_pen_mac(enum platform_class_wireless_role role,
					u64 *pen_mac);
int platform_class_wireless_get_pen_place_err(
					      enum platform_class_wireless_role role,
					      int *pen_place_err);
int platform_class_wireless_get_pen_soc(enum platform_class_wireless_role role,
					int *pen_soc);
int platform_class_wireless_get_phone_case_category(
						    enum platform_class_wireless_role role,
						    int *phone_case_category);
int platform_class_wireless_get_poweroff_err_code(
						  enum platform_class_wireless_role role,
						  u8 *poweroff_err_code);
int platform_class_wireless_get_project_vendor(
					       enum platform_class_wireless_role role,
					       int *project_vendor);
int platform_class_wireless_get_reverse_chg_en(
					       enum platform_class_wireless_role role,
					       int *reverse_chg_en);
int platform_class_wireless_get_rsv_eppmode_fail(
						 enum platform_class_wireless_role role,
						 int *fail_info);
int platform_class_wireless_get_rx_brg_status(
					      enum platform_class_wireless_role role,
					      u8 *brg_status);
int platform_class_wireless_get_rx_err_code(
					    enum platform_class_wireless_role role,
					    u8 *rx_err_code, u8 *dfx_code);
int platform_class_wireless_get_rx_fastcharge_status(
						     enum platform_class_wireless_role role,
						     u8 *fc_flag);
int platform_class_wireless_get_rx_int_flag(
					    enum platform_class_wireless_role role,
					    int *int_flag);
int platform_class_wireless_get_rx_offset(
					  enum platform_class_wireless_role role,
					  int *rx_offset);
int platform_class_wireless_get_rx_power_mode(
					      enum platform_class_wireless_role role,
					      u8 *rx_power_mode);
int platform_class_wireless_get_rx_rtx_mode(
					    enum platform_class_wireless_role role,
					    int *rx_rtx_mode);
int platform_class_wireless_get_ss_voltage(
					   enum platform_class_wireless_role role,
					   int *ss_voltage);
int platform_class_wireless_get_temp(enum platform_class_wireless_role role,
				     int *temp);
int platform_class_wireless_get_trx_isense(
					   enum platform_class_wireless_role role,
					   int *isense);
int platform_class_wireless_get_trx_vrect(
					  enum platform_class_wireless_role role,
					  int *vrect);
int platform_class_wireless_get_tx_adapter(
					   enum platform_class_wireless_role role,
					   int *tx_adapter);
int platform_class_wireless_get_tx_adapter_by_i2c(
						  enum platform_class_wireless_role role,
						  int *tx_adapter_by_i2c);
int platform_class_wireless_get_tx_err_code(
					    enum platform_class_wireless_role role,
					    u8 *tx_err_code, u8 *dfx_code);
int platform_class_wireless_get_tx_fan_speed(
					     enum platform_class_wireless_role role,
					     int *tx_fan_speed);
int platform_class_wireless_get_tx_iout(enum platform_class_wireless_role role,
					int *tx_iout);
int platform_class_wireless_get_tx_max_power(
					     enum platform_class_wireless_role role,
					     u8 *tx_max_power);
int platform_class_wireless_get_tx_uuid(enum platform_class_wireless_role role,
					u8 *tx_uuid);
int platform_class_wireless_get_tx_vout(enum platform_class_wireless_role role,
					int *tx_vout);
int platform_class_wireless_get_vout(enum platform_class_wireless_role role,
				     int *vout);
int platform_class_wireless_get_vout_setted(
					    enum platform_class_wireless_role role,
					    int *vout_setted);
int platform_class_wireless_get_vrect(enum platform_class_wireless_role role,
				      int *vrect);
int platform_class_wireless_is_car_adapter(
					   enum platform_class_wireless_role role,
					   bool *is_car_adapter);
int platform_class_wireless_is_present(enum platform_class_wireless_role role,
				       int *is_present);
int platform_class_wireless_process_factory_cmd(
						enum platform_class_wireless_role role,
						u8 process_factory_cmd);
int platform_class_wireless_receive_test_cmd(
					     enum platform_class_wireless_role role,
					     u8 *rev_data,
					     int *receive_test_cmd);
int platform_class_wireless_receive_transparent_data(
						     enum platform_class_wireless_role role,
						     u8 *rcv_value,
						     int receive_transparent_data,
						     int *rcv_len);
int platform_class_wireless_send_transparent_data(
						  enum platform_class_wireless_role role,
						  u8 *send_transparent_data,
						  u8 send_transparent_data2);
int platform_class_wireless_send_tx_q_value(
					    enum platform_class_wireless_role role,
					    u8 send_tx_q_value);
int platform_class_wireless_set_adapter_voltage(
						enum platform_class_wireless_role role,
						int adapter_voltage);
int platform_class_wireless_set_charge_type(
					    enum platform_class_wireless_role role,
					    u8 charge_type);
int platform_class_wireless_set_confirm_data(
					     enum platform_class_wireless_role role,
					     u8 confirm_data);
int platform_class_wireless_set_debug_fod(
					  enum platform_class_wireless_role role,
					  int *params, int size);
int platform_class_wireless_set_debug_fod_params(
						 enum platform_class_wireless_role role);
int platform_class_wireless_set_enable_mode(
					    enum platform_class_wireless_role role,
					    bool enable);
int platform_class_wireless_set_external_boost_enable(
						      enum platform_class_wireless_role role,
						      bool enable);
int platform_class_wireless_set_fod_params(
					   enum platform_class_wireless_role role,
					   int fod_params);
int platform_class_wireless_set_fw_bin(enum platform_class_wireless_role role,
				       const char *name, int size);
int platform_class_wireless_set_hboost_enable(
					      enum platform_class_wireless_role role,
					      int hboost_enable);
int platform_class_wireless_set_input_current_limit(
						    enum platform_class_wireless_role role,
						    int input_current_limit);
int platform_class_wireless_set_parallel_charge(
						enum platform_class_wireless_role role,
						bool enable);
int platform_class_wireless_set_pen_place_err(
					      enum platform_class_wireless_role role,
					      int pen_place_err);
int platform_class_wireless_set_phone_case_category(
						    enum platform_class_wireless_role role,
						    int phone_case_category);
int platform_class_wireless_set_rx_offset(
					  enum platform_class_wireless_role role,
					  int rx_offset);
int platform_class_wireless_set_rx_sleep_mode(
					      enum platform_class_wireless_role role,
					      int sleep_for_dam);
int platform_class_wireless_set_tx_fan_speed(
					     enum platform_class_wireless_role role,
					     int tx_fan_speed);
int platform_class_wireless_set_vout(enum platform_class_wireless_role role,
				     int vout);
int platform_class_wireless_switch_bridge(
					  enum platform_class_wireless_role role,
					  u8 switch_bridge);

/*
 * What a debug write to the receiver is asking it to change.  These are the
 * numbers userspace writes, so the values are the interface.
 */
enum wls_debug_params {
	DEBUG_SET_FCC = 1,
	DEBUG_SET_ICL,
	DEBUG_SET_ONE_EPP_FOD,
	DEBUG_SET_ALL_EPP_FOD,
	DEBUG_SET_ALL_FOD,
	DEBUG_SET_VOUT,
};

#endif /* __MCA_PLATFORM_WIRELESS_H */
