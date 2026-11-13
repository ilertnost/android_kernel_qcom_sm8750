// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Wireless charging.  See
 * include/mca/platform/platform_wireless_class.h.
 */

#define MCA_LOG_TAG "wireless_class"

#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/platform/platform_wireless_class.h>
#include <mca/common/mca_sysfs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

/**
 * struct platform_wireless_class_ops_data - one registered coil driver
 * @ops:  what the coil driver provides
 * @data: handed back to every call
 */
struct platform_wireless_class_ops_data {
	const struct platform_class_wireless_ops	*ops;
	void						*data;
};

static struct platform_wireless_class_ops_data
	platform_wireless_data[WIRELESS_ROLE_MAX];

int platform_class_wireless_register_ops(enum platform_class_wireless_role role,
					 void *data,
					 const struct platform_class_wireless_ops *ops)
{
	if (role >= WIRELESS_ROLE_MAX || !data || !ops)
		return -EOPNOTSUPP;

	platform_wireless_data[role].ops = ops;
	platform_wireless_data[role].data = data;

	return 0;
}
EXPORT_SYMBOL(platform_class_wireless_register_ops);

/*
 * Look up the driver for one coil.  A coil that has not probed gives NULL,
 * and the caller reports -EOPNOTSUPP rather than a made up answer: a phone told
 * the pad is at zero volts would stop charging on a working pad.
 */
static struct platform_wireless_class_ops_data *
wireless_lookup(enum platform_class_wireless_role role)
{
	if (role >= WIRELESS_ROLE_MAX)
		return NULL;

	if (!platform_wireless_data[role].ops)
		return NULL;

	return &platform_wireless_data[role];
}

int platform_class_wireless_check_firmware_state(
						 enum platform_class_wireless_role role,
						 bool *check_firmware_state)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_check_firmware_state)
		return -EOPNOTSUPP;

	return p->ops->wls_check_firmware_state(check_firmware_state, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_check_firmware_state);

int platform_class_wireless_check_i2c_is_ok(
					    enum platform_class_wireless_role role)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_check_i2c_is_ok)
		return -EOPNOTSUPP;

	return p->ops->wls_check_i2c_is_ok(p->data);
}
EXPORT_SYMBOL(platform_class_wireless_check_i2c_is_ok);

int platform_class_wireless_do_renego(enum platform_class_wireless_role role,
				      u8 do_renego)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_do_renego)
		return -EOPNOTSUPP;

	return p->ops->wls_do_renego(do_renego, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_do_renego);

int platform_class_wireless_download_fw(enum platform_class_wireless_role role)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_download_fw)
		return -EOPNOTSUPP;

	return p->ops->wls_download_fw(p->data);
}
EXPORT_SYMBOL(platform_class_wireless_download_fw);

int platform_class_wireless_download_fw_from_bin(
						 enum platform_class_wireless_role role)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_download_fw_from_bin)
		return -EOPNOTSUPP;

	return p->ops->wls_download_fw_from_bin(p->data);
}
EXPORT_SYMBOL(platform_class_wireless_download_fw_from_bin);

int platform_class_wireless_enable_rev_fod(
					   enum platform_class_wireless_role role,
					   bool enable)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_enable_rev_fod)
		return -EOPNOTSUPP;

	return p->ops->wls_enable_rev_fod(enable, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_enable_rev_fod);

int platform_class_wireless_enable_reverse_chg(
					       enum platform_class_wireless_role role,
					       bool enable)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_enable_reverse_chg)
		return -EOPNOTSUPP;

	return p->ops->wls_enable_reverse_chg(enable, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_enable_reverse_chg);

int platform_class_wireless_enable_vsys_ctrl(
					     enum platform_class_wireless_role role,
					     bool enable)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_enable_vsys_ctrl)
		return -EOPNOTSUPP;

	return p->ops->wls_enable_vsys_ctrl(enable, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_enable_vsys_ctrl);

int platform_class_wireless_erase_fw(enum platform_class_wireless_role role)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_erase_fw)
		return -EOPNOTSUPP;

	return p->ops->wls_erase_fw(p->data);
}
EXPORT_SYMBOL(platform_class_wireless_erase_fw);

int platform_class_wireless_get_auth_value(
					   enum platform_class_wireless_role role,
					   int *auth_value)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_auth_value)
		return -EOPNOTSUPP;

	return p->ops->wls_get_auth_value(auth_value, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_auth_value);

int platform_class_wireless_get_debug_fod_type(enum platform_class_wireless_role role,
					       WLS_DEBUG_SET_FOD_TYPE *type)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_debug_fod_type)
		return -EOPNOTSUPP;

	return p->ops->wls_get_debug_fod_type(type, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_debug_fod_type);

int platform_class_wireless_get_fw_upgrade_fail_info(
						     enum platform_class_wireless_role role,
						     const char **fail_info)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_fw_upgrade_fail_info)
		return -EOPNOTSUPP;

	return p->ops->wls_get_fw_upgrade_fail_info(fail_info, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_fw_upgrade_fail_info);

int platform_class_wireless_get_fw_version(
					   enum platform_class_wireless_role role,
					   char *fw_version)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_fw_version)
		return -EOPNOTSUPP;

	return p->ops->wls_get_fw_version(fw_version, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_fw_version);

int platform_class_wireless_get_fw_version_check(
						 enum platform_class_wireless_role role,
						 u8 *check_result)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_fw_version_check)
		return -EOPNOTSUPP;

	return p->ops->wls_get_fw_version_check(check_result, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_fw_version_check);

int platform_class_wireless_get_hall_gpio_status(
						 enum platform_class_wireless_role role,
						 bool *hall_gpio_status)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_hall_gpio_status)
		return -EOPNOTSUPP;

	return p->ops->wls_get_hall_gpio_status(hall_gpio_status, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_hall_gpio_status);

int platform_class_wireless_get_iout(enum platform_class_wireless_role role,
				     int *iout)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_iout)
		return -EOPNOTSUPP;

	return p->ops->wls_get_iout(iout, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_iout);

int platform_class_wireless_get_magnetic_case_flag(
						   enum platform_class_wireless_role role,
						   bool *magnetic_case_flag)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_magnetic_case_flag)
		return -EOPNOTSUPP;

	return p->ops->wls_get_magnetic_case_flag(magnetic_case_flag, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_magnetic_case_flag);

int platform_class_wireless_get_pen_full_flag(
					      enum platform_class_wireless_role role,
					      int *pen_full)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_pen_full_flag)
		return -EOPNOTSUPP;

	return p->ops->wls_get_pen_full_flag(pen_full, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_pen_full_flag);

int platform_class_wireless_get_pen_hall3(
					  enum platform_class_wireless_role role,
					  int *pen_hall3)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_pen_hall3)
		return -EOPNOTSUPP;

	return p->ops->wls_get_pen_hall3(pen_hall3, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_pen_hall3);

int platform_class_wireless_get_pen_hall3_s(
					    enum platform_class_wireless_role role,
					    int *pen_hall3_s)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_pen_hall3_s)
		return -EOPNOTSUPP;

	return p->ops->wls_get_pen_hall3_s(pen_hall3_s, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_pen_hall3_s);

int platform_class_wireless_get_pen_hall4(
					  enum platform_class_wireless_role role,
					  int *pen_hall4)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_pen_hall4)
		return -EOPNOTSUPP;

	return p->ops->wls_get_pen_hall4(pen_hall4, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_pen_hall4);

int platform_class_wireless_get_pen_hall4_s(
					    enum platform_class_wireless_role role,
					    int *pen_hall4_s)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_pen_hall4_s)
		return -EOPNOTSUPP;

	return p->ops->wls_get_pen_hall4_s(pen_hall4_s, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_pen_hall4_s);

int platform_class_wireless_get_pen_hall_ppe_n(
					       enum platform_class_wireless_role role,
					       int *pen_hall_ppe_n)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_pen_hall_ppe_n)
		return -EOPNOTSUPP;

	return p->ops->wls_get_pen_hall_ppe_n(pen_hall_ppe_n, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_pen_hall_ppe_n);

int platform_class_wireless_get_pen_hall_ppe_s(
					       enum platform_class_wireless_role role,
					       int *pen_hall_ppe_s)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_pen_hall_ppe_s)
		return -EOPNOTSUPP;

	return p->ops->wls_get_pen_hall_ppe_s(pen_hall_ppe_s, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_pen_hall_ppe_s);

int platform_class_wireless_get_pen_mac(enum platform_class_wireless_role role,
					u64 *pen_mac)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_pen_mac)
		return -EOPNOTSUPP;

	return p->ops->wls_get_pen_mac(pen_mac, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_pen_mac);

int platform_class_wireless_get_pen_place_err(
					      enum platform_class_wireless_role role,
					      int *pen_place_err)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_pen_place_err)
		return -EOPNOTSUPP;

	return p->ops->wls_get_pen_place_err(pen_place_err, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_pen_place_err);

int platform_class_wireless_get_pen_soc(enum platform_class_wireless_role role,
					int *pen_soc)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_pen_soc)
		return -EOPNOTSUPP;

	return p->ops->wls_get_pen_soc(pen_soc, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_pen_soc);

int platform_class_wireless_get_phone_case_category(
						    enum platform_class_wireless_role role,
						    int *phone_case_category)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_phone_case_category)
		return -EOPNOTSUPP;

	return p->ops->wls_get_phone_case_category(phone_case_category, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_phone_case_category);

int platform_class_wireless_get_poweroff_err_code(
						  enum platform_class_wireless_role role,
						  u8 *poweroff_err_code)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_poweroff_err_code)
		return -EOPNOTSUPP;

	return p->ops->wls_get_poweroff_err_code(poweroff_err_code, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_poweroff_err_code);

int platform_class_wireless_get_project_vendor(
					       enum platform_class_wireless_role role,
					       int *project_vendor)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_project_vendor)
		return -EOPNOTSUPP;

	return p->ops->wls_get_project_vendor(project_vendor, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_project_vendor);

int platform_class_wireless_get_reverse_chg_en(
					       enum platform_class_wireless_role role,
					       int *reverse_chg_en)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_reverse_chg_en)
		return -EOPNOTSUPP;

	return p->ops->wls_get_reverse_chg_en(reverse_chg_en, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_reverse_chg_en);

int platform_class_wireless_get_rsv_eppmode_fail(
						 enum platform_class_wireless_role role,
						 int *fail_info)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_rsv_eppmode_fail)
		return -EOPNOTSUPP;

	return p->ops->wls_get_rsv_eppmode_fail(fail_info, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_rsv_eppmode_fail);

int platform_class_wireless_get_rx_brg_status(
					      enum platform_class_wireless_role role,
					      u8 *brg_status)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_rx_brg_status)
		return -EOPNOTSUPP;

	return p->ops->wls_get_rx_brg_status(brg_status, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_rx_brg_status);

int platform_class_wireless_get_rx_err_code(
					    enum platform_class_wireless_role role,
					    u8 *rx_err_code, u8 *dfx_code)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_rx_err_code)
		return -EOPNOTSUPP;

	return p->ops->wls_get_rx_err_code(rx_err_code, dfx_code, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_rx_err_code);

int platform_class_wireless_get_rx_fastcharge_status(
						     enum platform_class_wireless_role role,
						     u8 *fc_flag)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_rx_fastcharge_status)
		return -EOPNOTSUPP;

	return p->ops->wls_get_rx_fastcharge_status(fc_flag, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_rx_fastcharge_status);

int platform_class_wireless_get_rx_int_flag(
					    enum platform_class_wireless_role role,
					    int *int_flag)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_rx_int_flag)
		return -EOPNOTSUPP;

	return p->ops->wls_get_rx_int_flag(int_flag, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_rx_int_flag);

int platform_class_wireless_get_rx_offset(
					  enum platform_class_wireless_role role,
					  int *rx_offset)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_rx_offset)
		return -EOPNOTSUPP;

	return p->ops->wls_get_rx_offset(rx_offset, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_rx_offset);

int platform_class_wireless_get_rx_power_mode(
					      enum platform_class_wireless_role role,
					      u8 *rx_power_mode)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_rx_power_mode)
		return -EOPNOTSUPP;

	return p->ops->wls_get_rx_power_mode(rx_power_mode, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_rx_power_mode);

int platform_class_wireless_get_rx_rtx_mode(
					    enum platform_class_wireless_role role,
					    int *rx_rtx_mode)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_rx_rtx_mode)
		return -EOPNOTSUPP;

	return p->ops->wls_get_rx_rtx_mode(rx_rtx_mode, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_rx_rtx_mode);

int platform_class_wireless_get_ss_voltage(
					   enum platform_class_wireless_role role,
					   int *ss_voltage)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_ss_voltage)
		return -EOPNOTSUPP;

	return p->ops->wls_get_ss_voltage(ss_voltage, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_ss_voltage);

int platform_class_wireless_get_temp(enum platform_class_wireless_role role,
				     int *temp)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_temp)
		return -EOPNOTSUPP;

	return p->ops->wls_get_temp(temp, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_temp);

int platform_class_wireless_get_trx_isense(
					   enum platform_class_wireless_role role,
					   int *isense)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_trx_isense)
		return -EOPNOTSUPP;

	return p->ops->wls_get_trx_isense(isense, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_trx_isense);

int platform_class_wireless_get_trx_vrect(
					  enum platform_class_wireless_role role,
					  int *vrect)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_trx_vrect)
		return -EOPNOTSUPP;

	return p->ops->wls_get_trx_vrect(vrect, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_trx_vrect);

int platform_class_wireless_get_tx_adapter(
					   enum platform_class_wireless_role role,
					   int *tx_adapter)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_tx_adapter)
		return -EOPNOTSUPP;

	return p->ops->wls_get_tx_adapter(tx_adapter, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_tx_adapter);

int platform_class_wireless_get_tx_adapter_by_i2c(
						  enum platform_class_wireless_role role,
						  int *tx_adapter_by_i2c)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_tx_adapter_by_i2c)
		return -EOPNOTSUPP;

	return p->ops->wls_get_tx_adapter_by_i2c(tx_adapter_by_i2c, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_tx_adapter_by_i2c);

int platform_class_wireless_get_tx_err_code(
					    enum platform_class_wireless_role role,
					    u8 *tx_err_code, u8 *dfx_code)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_tx_err_code)
		return -EOPNOTSUPP;

	return p->ops->wls_get_tx_err_code(tx_err_code, dfx_code, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_tx_err_code);

int platform_class_wireless_get_tx_fan_speed(
					     enum platform_class_wireless_role role,
					     int *tx_fan_speed)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_tx_fan_speed)
		return -EOPNOTSUPP;

	return p->ops->wls_get_tx_fan_speed(tx_fan_speed, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_tx_fan_speed);

int platform_class_wireless_get_tx_iout(enum platform_class_wireless_role role,
					int *tx_iout)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_tx_iout)
		return -EOPNOTSUPP;

	return p->ops->wls_get_tx_iout(tx_iout, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_tx_iout);

int platform_class_wireless_get_tx_max_power(
					     enum platform_class_wireless_role role,
					     u8 *tx_max_power)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_tx_max_power)
		return -EOPNOTSUPP;

	return p->ops->wls_get_tx_max_power(tx_max_power, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_tx_max_power);

int platform_class_wireless_get_tx_uuid(enum platform_class_wireless_role role,
					u8 *tx_uuid)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_tx_uuid)
		return -EOPNOTSUPP;

	return p->ops->wls_get_tx_uuid(tx_uuid, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_tx_uuid);

int platform_class_wireless_get_tx_vout(enum platform_class_wireless_role role,
					int *tx_vout)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_tx_vout)
		return -EOPNOTSUPP;

	return p->ops->wls_get_tx_vout(tx_vout, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_tx_vout);

int platform_class_wireless_get_vout(enum platform_class_wireless_role role,
				     int *vout)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_vout)
		return -EOPNOTSUPP;

	return p->ops->wls_get_vout(vout, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_vout);

int platform_class_wireless_get_vout_setted(
					    enum platform_class_wireless_role role,
					    int *vout_setted)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_vout_setted)
		return -EOPNOTSUPP;

	return p->ops->wls_get_vout_setted(vout_setted, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_vout_setted);

int platform_class_wireless_get_vrect(enum platform_class_wireless_role role,
				      int *vrect)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_get_vrect)
		return -EOPNOTSUPP;

	return p->ops->wls_get_vrect(vrect, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_get_vrect);

int platform_class_wireless_is_car_adapter(
					   enum platform_class_wireless_role role,
					   bool *is_car_adapter)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_is_car_adapter)
		return -EOPNOTSUPP;

	return p->ops->wls_is_car_adapter(is_car_adapter, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_is_car_adapter);

int platform_class_wireless_is_present(enum platform_class_wireless_role role,
				       int *is_present)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_is_present)
		return -EOPNOTSUPP;

	return p->ops->wls_is_present(is_present, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_is_present);

int platform_class_wireless_process_factory_cmd(
						enum platform_class_wireless_role role,
						u8 process_factory_cmd)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_process_factory_cmd)
		return -EOPNOTSUPP;

	return p->ops->wls_process_factory_cmd(process_factory_cmd, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_process_factory_cmd);

int platform_class_wireless_receive_test_cmd(
					     enum platform_class_wireless_role role,
					     u8 *rev_data,
					     int *receive_test_cmd)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_receive_test_cmd)
		return -EOPNOTSUPP;

	return p->ops->wls_receive_test_cmd(rev_data, receive_test_cmd, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_receive_test_cmd);

int platform_class_wireless_receive_transparent_data(
						     enum platform_class_wireless_role role,
						     u8 *rcv_value,
						     int receive_transparent_data,
						     int *rcv_len)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_receive_transparent_data)
		return -EOPNOTSUPP;

	return p->ops->wls_receive_transparent_data(rcv_value,
					     receive_transparent_data, rcv_len,
					     p->data);
}
EXPORT_SYMBOL(platform_class_wireless_receive_transparent_data);

int platform_class_wireless_send_transparent_data(
						  enum platform_class_wireless_role role,
						  u8 *send_transparent_data,
						  u8 send_transparent_data2)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_send_transparent_data)
		return -EOPNOTSUPP;

	return p->ops->wls_send_transparent_data(send_transparent_data,
					  send_transparent_data2, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_send_transparent_data);

int platform_class_wireless_send_tx_q_value(
					    enum platform_class_wireless_role role,
					    u8 send_tx_q_value)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_send_tx_q_value)
		return -EOPNOTSUPP;

	return p->ops->wls_send_tx_q_value(send_tx_q_value, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_send_tx_q_value);

int platform_class_wireless_set_adapter_voltage(
						enum platform_class_wireless_role role,
						int adapter_voltage)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_adapter_voltage)
		return -EOPNOTSUPP;

	return p->ops->wls_set_adapter_voltage(adapter_voltage, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_adapter_voltage);

int platform_class_wireless_set_charge_type(
					    enum platform_class_wireless_role role,
					    u8 charge_type)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_charge_type)
		return -EOPNOTSUPP;

	return p->ops->wls_set_charge_type(charge_type, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_charge_type);

int platform_class_wireless_set_confirm_data(
					     enum platform_class_wireless_role role,
					     u8 confirm_data)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_confirm_data)
		return -EOPNOTSUPP;

	return p->ops->wls_set_confirm_data(p->data, confirm_data);
}
EXPORT_SYMBOL(platform_class_wireless_set_confirm_data);

int platform_class_wireless_set_debug_fod(
					  enum platform_class_wireless_role role,
					  int *params, int size)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_debug_fod)
		return -EOPNOTSUPP;

	return p->ops->wls_set_debug_fod(params, size, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_debug_fod);

int platform_class_wireless_set_debug_fod_params(
						 enum platform_class_wireless_role role)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_debug_fod_params)
		return -EOPNOTSUPP;

	return p->ops->wls_set_debug_fod_params(p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_debug_fod_params);

int platform_class_wireless_set_enable_mode(
					    enum platform_class_wireless_role role,
					    bool enable)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_enable_mode)
		return -EOPNOTSUPP;

	return p->ops->wls_set_enable_mode(enable, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_enable_mode);

int platform_class_wireless_set_external_boost_enable(
						      enum platform_class_wireless_role role,
						      bool enable)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_external_boost_enable)
		return -EOPNOTSUPP;

	return p->ops->wls_set_external_boost_enable(enable, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_external_boost_enable);

int platform_class_wireless_set_fod_params(
					   enum platform_class_wireless_role role,
					   int fod_params)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_fod_params)
		return -EOPNOTSUPP;

	return p->ops->wls_set_fod_params(fod_params, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_fod_params);

int platform_class_wireless_set_fw_bin(enum platform_class_wireless_role role,
				       const char *name, int size)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_fw_bin)
		return -EOPNOTSUPP;

	return p->ops->wls_set_fw_bin(name, size, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_fw_bin);

int platform_class_wireless_set_hboost_enable(
					      enum platform_class_wireless_role role,
					      int hboost_enable)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_hboost_enable)
		return -EOPNOTSUPP;

	return p->ops->wls_set_hboost_enable(hboost_enable, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_hboost_enable);

int platform_class_wireless_set_input_current_limit(
						    enum platform_class_wireless_role role,
						    int input_current_limit)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_input_current_limit)
		return -EOPNOTSUPP;

	return p->ops->wls_set_input_current_limit(input_current_limit, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_input_current_limit);

int platform_class_wireless_set_parallel_charge(
						enum platform_class_wireless_role role,
						bool enable)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_parallel_charge)
		return -EOPNOTSUPP;

	return p->ops->wls_set_parallel_charge(enable, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_parallel_charge);

int platform_class_wireless_set_pen_place_err(
					      enum platform_class_wireless_role role,
					      int pen_place_err)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_pen_place_err)
		return -EOPNOTSUPP;

	return p->ops->wls_set_pen_place_err(pen_place_err, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_pen_place_err);

int platform_class_wireless_set_phone_case_category(
						    enum platform_class_wireless_role role,
						    int phone_case_category)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_phone_case_category)
		return -EOPNOTSUPP;

	return p->ops->wls_set_phone_case_category(phone_case_category, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_phone_case_category);

int platform_class_wireless_set_rx_offset(
					  enum platform_class_wireless_role role,
					  int rx_offset)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_rx_offset)
		return -EOPNOTSUPP;

	return p->ops->wls_set_rx_offset(rx_offset, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_rx_offset);

int platform_class_wireless_set_rx_sleep_mode(
					      enum platform_class_wireless_role role,
					      int sleep_for_dam)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_rx_sleep_mode)
		return -EOPNOTSUPP;

	return p->ops->wls_set_rx_sleep_mode(sleep_for_dam, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_rx_sleep_mode);

int platform_class_wireless_set_tx_fan_speed(
					     enum platform_class_wireless_role role,
					     int tx_fan_speed)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_tx_fan_speed)
		return -EOPNOTSUPP;

	return p->ops->wls_set_tx_fan_speed(tx_fan_speed, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_tx_fan_speed);

int platform_class_wireless_set_vout(enum platform_class_wireless_role role,
				     int vout)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_set_vout)
		return -EOPNOTSUPP;

	return p->ops->wls_set_vout(vout, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_set_vout);

int platform_class_wireless_switch_bridge(
					  enum platform_class_wireless_role role,
					  u8 switch_bridge)
{
	struct platform_wireless_class_ops_data *p = wireless_lookup(role);

	if (!p || !p->ops->wls_switch_bridge)
		return -EOPNOTSUPP;

	return p->ops->wls_switch_bridge(switch_bridge, p->data);
}
EXPORT_SYMBOL(platform_class_wireless_switch_bridge);

/**
 * struct platform_wireless_dev - the coils this board has
 * @dev:               this device
 * @wireless_num:      how many of them
 * @wireless_dir_list: the sysfs directory each one appears under
 * @sysfs_dev:         the device created for each directory
 * @wireless_dev_index: which coil each of those directories belongs to
 *
 * @wireless_dev_index is what the attribute handlers are given as the
 * device's driver data, so an attribute read on one directory knows which
 * coil it is being asked about without a handler per coil.
 */
struct platform_wireless_dev {
	struct device	*dev;
	int		wireless_num;
	const char	*wireless_dir_list[WIRELESS_ROLE_MAX];
	struct device	*sysfs_dev[WIRELESS_ROLE_MAX];
	int		wireless_dev_index[WIRELESS_ROLE_MAX];
};

/* Where a coil's directory appears, and what its name is matched against. */
#define WIRELESS_CLASS_NAME	"xm_power"
#define WIRELESS_NAME_MASTER	"master"
#define WIRELESS_NAME_SLAVE	"slave"

/* How much room a firmware version string is given. */
#define WIRELESS_FW_VERSION_LEN	200

/* How many bytes of the transmitter's identifier are printed. */
#define WIRELESS_TX_UUID_LEN	4

static struct platform_wireless_dev *g_wireless_dev;
static int probe_cnt;

static ssize_t wireless_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf);
static ssize_t wireless_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count);

static struct mca_sysfs_attr_info wireless_sysfs_field_tbl[] = {
	mca_sysfs_attr_ro(wireless_sysfs, 0440, RX_SYSFS_RX_VOUT, rx_vout),
	mca_sysfs_attr_ro(wireless_sysfs, 0440, RX_SYSFS_RX_VRECT, rx_vrect),
	mca_sysfs_attr_ro(wireless_sysfs, 0440, RX_SYSFS_RX_IOUT, rx_iout),
	mca_sysfs_attr_rw(wireless_sysfs, 0660, RX_SYSFS_FW_VERSION,
			  fw_version),
	mca_sysfs_attr_wo(wireless_sysfs, 0220, RX_SYSFS_FW_BIN, wls_bin),
	mca_sysfs_attr_rw(wireless_sysfs, 0660, RX_SYSFS_SLEEP_RX, sleep_rx),
	mca_sysfs_attr_rw(wireless_sysfs, 0660, RX_SYSFS_TX_ADAPTER,
			  tx_adapter),
	mca_sysfs_attr_rw(wireless_sysfs, 0660, RX_SYSFS_BT_STATE, bt_state),
	mca_sysfs_attr_rw(wireless_sysfs, 0660, RX_SYSFS_RX_CEP, rx_cep),
	mca_sysfs_attr_rw(wireless_sysfs, 0660, RX_SYSFS_RX_CR, rx_cr),
	mca_sysfs_attr_rw(wireless_sysfs, 0660, RX_SYSFS_TX_MAC, tx_mac),
	mca_sysfs_attr_rw(wireless_sysfs, 0660, RX_SYSFS_WLS_DIE_TEMP,
			  wls_die_temp),
	mca_sysfs_attr_rw(wireless_sysfs, 0660, RX_SYSFS_WLS_TX_SPEED,
			  wls_tx_speed),
	mca_sysfs_attr_rw(wireless_sysfs, 0660, RX_SYSFS_RX_SS, rx_ss),
	mca_sysfs_attr_rw(wireless_sysfs, 0660, RX_SYSFS_RX_OFFSET, rx_offset),
	mca_sysfs_attr_rw(wireless_sysfs, 0660, RX_SYSFS_RX_SLEEP_MODE,
			  rx_sleep_mode),
	mca_sysfs_attr_ro(wireless_sysfs, 0440, RX_SYSFS_TX_UUID, tx_uuid),
};

static struct attribute *wireless_attrs[ARRAY_SIZE(wireless_sysfs_field_tbl) + 1];
static const struct attribute_group wireless_sysfs_attr_group = {
	.attrs = wireless_attrs,
};

static ssize_t wireless_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	char fw_version[WIRELESS_FW_VERSION_LEN] = { 0 };
	u8 uuid[WIRELESS_TX_UUID_LEN] = { 0 };
	struct mca_sysfs_attr_info *info;
	int *role;
	int val = 0;

	info = mca_sysfs_lookup_attr(attr->attr.name, wireless_sysfs_field_tbl,
				     ARRAY_SIZE(wireless_sysfs_field_tbl));
	if (!info)
		return -1;

	role = dev_get_drvdata(dev);
	if (!role) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	mca_log_err("%s dev_driverdata is %d\n", __func__, *role);

	switch (info->sysfs_attr_name) {
	case RX_SYSFS_RX_VOUT:
		platform_class_wireless_get_vout(*role, &val);
		break;
	case RX_SYSFS_RX_VRECT:
		platform_class_wireless_get_vrect(*role, &val);
		break;
	case RX_SYSFS_RX_IOUT:
		platform_class_wireless_get_iout(*role, &val);
		break;
	case RX_SYSFS_FW_VERSION:
		platform_class_wireless_get_fw_version(*role, fw_version);

		return scnprintf(buf, PAGE_SIZE, "%s\n", fw_version);
	case RX_SYSFS_TX_ADAPTER:
		platform_class_wireless_get_tx_adapter(*role, &val);
		break;
	case RX_SYSFS_WLS_DIE_TEMP:
		platform_class_wireless_get_temp(*role, &val);
		break;
	case RX_SYSFS_WLS_TX_SPEED:
		platform_class_wireless_get_tx_fan_speed(*role, &val);
		break;
	case RX_SYSFS_RX_SS:
		platform_class_wireless_get_ss_voltage(*role, &val);
		break;
	case RX_SYSFS_RX_OFFSET:
		platform_class_wireless_get_rx_offset(*role, &val);
		break;
	case RX_SYSFS_TX_UUID:
		platform_class_wireless_get_tx_uuid(*role, uuid);

		return scnprintf(buf, PAGE_SIZE, "%02x.%02x.%02x.%02x\n",
				 uuid[0], uuid[1], uuid[2], uuid[3]);
	default:
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t wireless_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct mca_sysfs_attr_info *info;
	int *role;
	int val = 0;

	info = mca_sysfs_lookup_attr(attr->attr.name, wireless_sysfs_field_tbl,
				     ARRAY_SIZE(wireless_sysfs_field_tbl));
	if (!info)
		return -1;

	role = dev_get_drvdata(dev);
	if (!role) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	mca_log_err("%s dev_driverdata is %d\n", __func__, *role);

	switch (info->sysfs_attr_name) {
	case RX_SYSFS_FW_BIN:
		/*
		 * The firmware image is written here whole rather than as a
		 * number, so what userspace wrote is handed over as it is.
		 */
		platform_class_wireless_set_fw_bin(*role, buf, count);
		break;
	case RX_SYSFS_SLEEP_RX:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;

		platform_class_wireless_set_enable_mode(*role, val);
		break;
	case RX_SYSFS_WLS_TX_SPEED:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;

		platform_class_wireless_set_tx_fan_speed(*role, val);
		break;
	case RX_SYSFS_RX_OFFSET:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;

		platform_class_wireless_set_rx_offset(*role, val);
		break;
	case RX_SYSFS_RX_SLEEP_MODE:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;

		platform_class_wireless_set_rx_sleep_mode(*role, val);
		break;
	default:
		break;
	}

	return count;
}

/*
 * Each coil gets a directory of its own, named from the node's
 * wireless-dir-list.  Which coil a directory belongs to is taken from the
 * name it was given rather than from its position, so a board that lists
 * them the other way round still reaches the right one.
 */
static void wireless_sysfs_create_group(struct platform_wireless_dev *chip)
{
	const char *name;
	int role;
	int i;

	for (i = 0; i < chip->wireless_num && i < WIRELESS_ROLE_MAX; i++) {
		chip->sysfs_dev[i] =
			mca_sysfs_create_group(WIRELESS_CLASS_NAME,
					       chip->wireless_dir_list[i],
					       &wireless_sysfs_attr_group);
		if (!chip->sysfs_dev[i])
			mca_log_err("creat wireless[%d] sysfs fail\n", i);
	}

	for (i = 0; i < chip->wireless_num && i < WIRELESS_ROLE_MAX; i++) {
		name = dev_name(chip->sysfs_dev[i]);

		if (strstr(name, WIRELESS_NAME_MASTER))
			role = WIRELESS_ROLE_MASTER;
		else if (strstr(name, WIRELESS_NAME_SLAVE))
			role = WIRELESS_ROLE_SLAVE;
		else
			continue;

		chip->wireless_dev_index[i] = role;
		dev_set_drvdata(chip->sysfs_dev[i],
				&chip->wireless_dev_index[i]);
		mca_log_err("success match wireless_dev_name = %s, wireless_dev_list[%d]=%s\n",
			    name, role,
			    role == WIRELESS_ROLE_MASTER ?
			    WIRELESS_NAME_MASTER : WIRELESS_NAME_SLAVE);
	}
}

static int platform_wireless_dev_parse_dt(struct platform_wireless_dev *chip,
					  struct device_node *np)
{
	int count;
	int rc;
	int i;

	if (!np) {
		mca_log_err("device tree info missing\n");
		return -1;
	}

	rc = mca_parse_dts_u32(np, "wireless-num", &chip->wireless_num, 1);
	if (rc) {
		mca_log_err("get wireless-num fail\n");
		return rc;
	}

	count = mca_parse_dts_count_strings(np, "wireless-dir-list",
					    WIRELESS_ROLE_MAX, 1);
	mca_log_err("wireless dir list max count: %d, %d\n", count,
		    chip->wireless_num);
	if (count != chip->wireless_num)
		mca_log_err("wireless_num can't match wireless_dir_list count\n");

	for (i = 0; i < count; i++) {
		rc = mca_parse_dts_string_index(np, "wireless-dir-list", i,
						&chip->wireless_dir_list[i]);
		if (rc < 0) {
			mca_log_err("Unable to read wireless-dir-list strings[%d]\n",
				    i);
			return rc;
		}
	}

	mca_log_info("%s success\n", __func__);

	return 0;
}

static int platform_wireless_class_probe(struct platform_device *pdev)
{
	struct platform_wireless_dev *chip;
	int rc;

	mca_log_err("begin cnt %d\n", probe_cnt++);

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	platform_set_drvdata(pdev, chip);

	rc = platform_wireless_dev_parse_dt(chip, pdev->dev.of_node);
	if (rc) {
		mca_log_err("%s Couldn't parse device tree rc=%d\n", __func__,
			    rc);
		return rc;
	}

	g_wireless_dev = chip;

	mca_sysfs_init_attrs(wireless_attrs, wireless_sysfs_field_tbl,
			     ARRAY_SIZE(wireless_sysfs_field_tbl));
	wireless_sysfs_create_group(chip);

	mca_log_err("success %d\n", probe_cnt++);

	return 0;
}

static int platform_wireless_class_remove(struct platform_device *pdev)
{
	return 0;
}

static void platform_wireless_class_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,platform_wireless" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver platform_wireless_class_driver = {
	.driver = {
		.name		= "platform_wireless_class",
		.of_match_table	= match_table,
	},
	.probe		= platform_wireless_class_probe,
	.remove		= platform_wireless_class_remove,
	.shutdown	= platform_wireless_class_shutdown,
};
module_platform_driver(platform_wireless_class_driver);

MODULE_DESCRIPTION("platform wireless charge class");
MODULE_AUTHOR("wuliyang@xiaomi.com");
MODULE_LICENSE("GPL v2");
