// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The buck charger.  See include/mca/platform/platform_buckchg_class.h.
 */

#define MCA_LOG_TAG "platform_buckchg_class"

#include <linux/errno.h>
#include <mca/common/mca_log.h>
#include <mca/platform/platform_buckchg_class.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/string.h>

/**
 * struct platform_class_buckchg_data - one registered buck charger
 * @data: handed back to every call
 * @ops:  what the charger driver provides
 */
struct platform_class_buckchg_data {
	void					*data;
	const struct platform_class_buckchg_ops	*ops;
};

static struct platform_class_buckchg_data platform_buckchg_ops_data[MAX_BUCK_CHARGER];

/*
 * Registering a NULL @ops takes the charger out again: every call checks for
 * it and reports -1.
 */
int platform_class_buckchg_ops_register(enum platform_class_buckchg_role role,
					void *data,
					const struct platform_class_buckchg_ops *ops)
{
	if (role >= MAX_BUCK_CHARGER)
		return -1;

	platform_buckchg_ops_data[role].ops = ops;
	platform_buckchg_ops_data[role].data = data;

	return 0;
}
EXPORT_SYMBOL(platform_class_buckchg_ops_register);

/*
 * Look up the driver for one charging input.  A charger that has not probed
 * gives NULL, and the caller reports -1: a strategy told the input
 * measured zero volts would act on it, where being told it cannot say is
 * harmless.
 *
 * The value is -1 rather than an errno: callers propagate it unchanged,
 * and some of it reaches userspace through sysfs.
 */
static struct platform_class_buckchg_data *
platform_class_buckchg_lookup(enum platform_class_buckchg_role role)
{
	if (role >= MAX_BUCK_CHARGER)
		return NULL;

	if (!platform_buckchg_ops_data[role].ops)
		return NULL;

	return &platform_buckchg_ops_data[role];
}

int platform_class_buckchg_ops_adc_enable(
					  enum platform_class_buckchg_role role,
					  bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->adc_enable)
		return -1;

	return p->ops->adc_enable(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_adc_enable);

int platform_class_buckchg_ops_enable_hvdcp(
					    enum platform_class_buckchg_role role,
					    int hvdcp)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->enable_hvdcp)
		return -1;

	return p->ops->enable_hvdcp(p->data, hvdcp);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_enable_hvdcp);

int platform_class_buckchg_ops_force_dpdm(
					  enum platform_class_buckchg_role role,
					  int force_dpdm)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->force_dpdm)
		return -1;

	return p->ops->force_dpdm(p->data, force_dpdm);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_force_dpdm);

int platform_class_buckchg_ops_get_ac_volt(
					   enum platform_class_buckchg_role role,
					   int *ac_volt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_ac_volt)
		return -1;

	return p->ops->get_ac_volt(p->data, ac_volt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_ac_volt);

int platform_class_buckchg_ops_get_aicl_status(
					       enum platform_class_buckchg_role role,
					       int *aicl_status)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_aicl_status)
		return -1;

	return p->ops->get_aicl_status(p->data, aicl_status);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_aicl_status);

int platform_class_buckchg_ops_get_batt_curr(
					     enum platform_class_buckchg_role role,
					     int *batt_curr)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_batt_curr)
		return -1;

	return p->ops->get_batt_curr(p->data, batt_curr);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_batt_curr);

int platform_class_buckchg_ops_get_batt_id(
					   enum platform_class_buckchg_role role,
					   int *batt_id)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_batt_id)
		return -1;

	return p->ops->get_batt_id(p->data, batt_id);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_batt_id);

int platform_class_buckchg_ops_get_batt_tsns(
					     enum platform_class_buckchg_role role,
					     int *sns)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_batt_tsns)
		return -1;

	return p->ops->get_batt_tsns(p->data, sns);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_batt_tsns);

int platform_class_buckchg_ops_get_batt_volt(
					     enum platform_class_buckchg_role role,
					     int *batt_volt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_batt_volt)
		return -1;

	return p->ops->get_batt_volt(p->data, batt_volt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_batt_volt);

int platform_class_buckchg_ops_get_batt_volt_sns(
						 enum platform_class_buckchg_role role,
						 int *sns)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_batt_volt_sns)
		return -1;

	return p->ops->get_batt_volt_sns(p->data, sns);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_batt_volt_sns);

int platform_class_buckchg_ops_get_bus_curr(
					    enum platform_class_buckchg_role role,
					    int *bus_curr)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_bus_curr)
		return -1;

	return p->ops->get_bus_curr(p->data, bus_curr);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_bus_curr);

int platform_class_buckchg_ops_get_bus_tsns(
					    enum platform_class_buckchg_role role,
					    int *sns)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_bus_tsns)
		return -1;

	return p->ops->get_bus_tsns(p->data, sns);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_bus_tsns);

int platform_class_buckchg_ops_get_bus_volt(
					    enum platform_class_buckchg_role role,
					    int *bus_volt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_bus_volt)
		return -1;

	return p->ops->get_bus_volt(p->data, bus_volt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_bus_volt);

int platform_class_buckchg_ops_get_chg_status(
					      enum platform_class_buckchg_role role,
					      int *chg_status)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_chg_status)
		return -1;

	return p->ops->get_chg_status(p->data, chg_status);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_chg_status);

int platform_class_buckchg_ops_get_chg_type(
					    enum platform_class_buckchg_role role,
					    int *chg_type)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_chg_type)
		return -1;

	return p->ops->get_chg_type(p->data, chg_type);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_chg_type);

int platform_class_buckchg_ops_get_die_temp(
					    enum platform_class_buckchg_role role,
					    int *die_temp)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_die_temp)
		return -1;

	return p->ops->get_die_temp(p->data, die_temp);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_die_temp);

int platform_class_buckchg_ops_get_hiz_status(
					      enum platform_class_buckchg_role role,
					      int *hiz_status)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_hiz_status)
		return -1;

	return p->ops->get_hiz_status(p->data, hiz_status);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_hiz_status);

int platform_class_buckchg_ops_get_input_curr_lmt(
						  enum platform_class_buckchg_role role,
						  int *input_curr_lmt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_input_curr_lmt)
		return -1;

	return p->ops->get_input_curr_lmt(p->data, input_curr_lmt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_input_curr_lmt);

int platform_class_buckchg_ops_get_input_volt_lmt(
						  enum platform_class_buckchg_role role,
						  int *input_volt_lmt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_input_volt_lmt)
		return -1;

	return p->ops->get_input_volt_lmt(p->data, input_volt_lmt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_input_volt_lmt);

int platform_class_buckchg_ops_get_lpd_cc1(
					   enum platform_class_buckchg_role role,
					   int *lpd_cc1)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_lpd_cc1)
		return -1;

	return p->ops->get_lpd_cc1(p->data, lpd_cc1);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_lpd_cc1);

int platform_class_buckchg_ops_get_lpd_cc2(
					   enum platform_class_buckchg_role role,
					   int *lpd_cc2)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_lpd_cc2)
		return -1;

	return p->ops->get_lpd_cc2(p->data, lpd_cc2);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_lpd_cc2);

int platform_class_buckchg_ops_get_lpd_control(
					       enum platform_class_buckchg_role role,
					       int *lpd_control)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_lpd_control)
		return -1;

	return p->ops->get_lpd_control(p->data, lpd_control);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_lpd_control);

int platform_class_buckchg_ops_get_lpd_dm(
					  enum platform_class_buckchg_role role,
					  int *lpd_dm)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_lpd_dm)
		return -1;

	return p->ops->get_lpd_dm(p->data, lpd_dm);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_lpd_dm);

int platform_class_buckchg_ops_get_lpd_dp(
					  enum platform_class_buckchg_role role,
					  int *lpd_dp)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_lpd_dp)
		return -1;

	return p->ops->get_lpd_dp(p->data, lpd_dp);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_lpd_dp);

int platform_class_buckchg_ops_get_lpd_enable(
					      enum platform_class_buckchg_role role,
					      int *lpd_en)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_lpd_enable)
		return -1;

	return p->ops->get_lpd_enable(p->data, lpd_en);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_lpd_enable);

int platform_class_buckchg_ops_get_lpd_sbu1(
					    enum platform_class_buckchg_role role,
					    int *lpd_sbu1)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_lpd_sbu1)
		return -1;

	return p->ops->get_lpd_sbu1(p->data, lpd_sbu1);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_lpd_sbu1);

int platform_class_buckchg_ops_get_lpd_sbu2(
					    enum platform_class_buckchg_role role,
					    int *lpd_sbu2)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_lpd_sbu2)
		return -1;

	return p->ops->get_lpd_sbu2(p->data, lpd_sbu2);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_lpd_sbu2);

int platform_class_buckchg_ops_get_lpd_status(
					      enum platform_class_buckchg_role role,
					      int *lpd_status)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_lpd_status)
		return -1;

	return p->ops->get_lpd_status(p->data, lpd_status);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_lpd_status);

int platform_class_buckchg_ops_get_lpd_uart_control(
						    enum platform_class_buckchg_role role,
						    int *lpd_uart_control)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_lpd_uart_control)
		return -1;

	return p->ops->get_lpd_uart_control(p->data, lpd_uart_control);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_lpd_uart_control);

int platform_class_buckchg_ops_get_online(
					  enum platform_class_buckchg_role role,
					  int *online)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_online)
		return -1;

	return p->ops->get_online(p->data, online);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_online);

int platform_class_buckchg_ops_get_otg_boost_enable_status(
							   enum platform_class_buckchg_role role,
							   int *otg_boost_enable_sts)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_otg_boost_enable_status)
		return -1;

	return p->ops->get_otg_boost_enable_status(p->data, otg_boost_enable_sts);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_otg_boost_enable_status);

int platform_class_buckchg_ops_get_otg_boost_src(
						 enum platform_class_buckchg_role role,
						 int *otg_boost_src)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_otg_boost_src)
		return -1;

	return p->ops->get_otg_boost_src(p->data, otg_boost_src);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_otg_boost_src);

int platform_class_buckchg_ops_get_otg_gate_enable_status(
							  enum platform_class_buckchg_role role,
							  int *otg_gate_enable_sts)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_otg_gate_enable_status)
		return -1;

	return p->ops->get_otg_gate_enable_status(p->data, otg_gate_enable_sts);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_otg_gate_enable_status);

int platform_class_buckchg_ops_get_pack_ibat(
					     enum platform_class_buckchg_role role,
					     int *pibat)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_pack_ibat)
		return -1;

	return p->ops->get_pack_ibat(p->data, pibat);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_pack_ibat);

int platform_class_buckchg_ops_get_pack_tbat(
					     enum platform_class_buckchg_role role,
					     int *ptbat)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_pack_tbat)
		return -1;

	return p->ops->get_pack_tbat(p->data, ptbat);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_pack_tbat);

int platform_class_buckchg_ops_get_pack_vbat(
					     enum platform_class_buckchg_role role,
					     int *pvbat)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_pack_vbat)
		return -1;

	return p->ops->get_pack_vbat(p->data, pvbat);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_pack_vbat);

int platform_class_buckchg_ops_get_ship_mode(
					     enum platform_class_buckchg_role role,
					     bool *ship_mode)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_ship_mode)
		return -1;

	return p->ops->get_ship_mode(p->data, ship_mode);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_ship_mode);

int platform_class_buckchg_ops_get_sys_volt(
					    enum platform_class_buckchg_role role,
					    int *vsys_min)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_sys_volt)
		return -1;

	return p->ops->get_sys_volt(p->data, vsys_min);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_sys_volt);

int platform_class_buckchg_ops_get_term_curr(
					     enum platform_class_buckchg_role role,
					     int *term_curr)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_term_curr)
		return -1;

	return p->ops->get_term_curr(p->data, term_curr);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_term_curr);

int platform_class_buckchg_ops_get_term_volt(
					     enum platform_class_buckchg_role role,
					     int *term_volt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_term_volt)
		return -1;

	return p->ops->get_term_volt(p->data, term_volt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_term_volt);

int platform_class_buckchg_ops_get_usb_aicl_cont_thd(
						     enum platform_class_buckchg_role role,
						     int *usb_aicl_cont_thd)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_usb_aicl_cont_thd)
		return -1;

	return p->ops->get_usb_aicl_cont_thd(p->data, usb_aicl_cont_thd);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_usb_aicl_cont_thd);

int platform_class_buckchg_ops_get_usb_sns_volt(
						enum platform_class_buckchg_role role,
						int *bus_volt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_usb_sns_volt)
		return -1;

	return p->ops->get_usb_sns_volt(p->data, bus_volt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_usb_sns_volt);

int platform_class_buckchg_ops_get_wls_curr(
					    enum platform_class_buckchg_role role,
					    int *wls_curr)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->get_wls_curr)
		return -1;

	return p->ops->get_wls_curr(p->data, wls_curr);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_get_wls_curr);

int platform_class_buckchg_ops_is_charge_done(
					      enum platform_class_buckchg_role role,
					      bool *charge_done)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->is_charge_done)
		return -1;

	return p->ops->is_charge_done(p->data, charge_done);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_is_charge_done);

int platform_class_buckchg_ops_is_init_ok(enum platform_class_buckchg_role role)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->is_init_ok)
		return -1;

	return p->ops->is_init_ok(p->data);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_is_init_ok);

int platform_class_buckchg_ops_is_support_cid(
					      enum platform_class_buckchg_role role,
					      bool *is_support_cid)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->is_support_cid)
		return -1;

	return p->ops->is_support_cid(p->data, is_support_cid);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_is_support_cid);

int platform_class_buckchg_ops_kick_wd(enum platform_class_buckchg_role role)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->kick_wd)
		return -1;

	return p->ops->kick_wd(p->data);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_kick_wd);

int platform_class_buckchg_ops_request_dpdm(
					    enum platform_class_buckchg_role role,
					    bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->request_dpdm)
		return -1;

	return p->ops->request_dpdm(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_request_dpdm);

int platform_class_buckchg_ops_set_aicl_enable(
					       enum platform_class_buckchg_role role,
					       bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_aicl_enable)
		return -1;

	return p->ops->set_aicl_enable(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_aicl_enable);

int platform_class_buckchg_ops_set_boost_enable(
						enum platform_class_buckchg_role role,
						int src_enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_boost_enable)
		return -1;

	return p->ops->set_boost_enable(p->data, src_enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_boost_enable);

int platform_class_buckchg_ops_set_boost_voltage(
						 enum platform_class_buckchg_role role,
						 int src_value)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_boost_voltage)
		return -1;

	return p->ops->set_boost_voltage(p->data, src_value);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_boost_voltage);

int platform_class_buckchg_ops_set_buck_fsw(
					    enum platform_class_buckchg_role role,
					    int buck_fsw)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_buck_fsw)
		return -1;

	return p->ops->set_buck_fsw(p->data, buck_fsw);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_buck_fsw);

int platform_class_buckchg_ops_set_chg(enum platform_class_buckchg_role role,
				       bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_chg)
		return -1;

	return p->ops->set_chg(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_chg);

int platform_class_buckchg_ops_set_eu_model(
					    enum platform_class_buckchg_role role,
					    bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_eu_model)
		return -1;

	return p->ops->set_eu_model(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_eu_model);

int platform_class_buckchg_ops_set_hiz(enum platform_class_buckchg_role role,
				       bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_hiz)
		return -1;

	return p->ops->set_hiz(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_hiz);

int platform_class_buckchg_ops_set_ichg(enum platform_class_buckchg_role role,
					int ichg)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_ichg)
		return -1;

	return p->ops->set_ichg(p->data, ichg);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_ichg);

int platform_class_buckchg_ops_set_input_curr_lmt(
						  enum platform_class_buckchg_role role,
						  int input_curr_lmt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_input_curr_lmt)
		return -1;

	return p->ops->set_input_curr_lmt(p->data, input_curr_lmt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_input_curr_lmt);

int platform_class_buckchg_ops_set_input_volt_lmt(
						  enum platform_class_buckchg_role role,
						  int input_volt_lmt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_input_volt_lmt)
		return -1;

	return p->ops->set_input_volt_lmt(p->data, input_volt_lmt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_input_volt_lmt);

int platform_class_buckchg_ops_set_lpd_control(
					       enum platform_class_buckchg_role role,
					       int lpd_control)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_lpd_control)
		return -1;

	return p->ops->set_lpd_control(p->data, lpd_control);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_lpd_control);

int platform_class_buckchg_ops_set_lpd_sbu1(
					    enum platform_class_buckchg_role role,
					    int lpd_sbu1)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_lpd_sbu1)
		return -1;

	return p->ops->set_lpd_sbu1(p->data, lpd_sbu1);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_lpd_sbu1);

int platform_class_buckchg_ops_set_lpd_uart_control(
						    enum platform_class_buckchg_role role,
						    int lpd_uart_control)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_lpd_uart_control)
		return -1;

	return p->ops->set_lpd_uart_control(p->data, lpd_uart_control);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_lpd_uart_control);

int platform_class_buckchg_ops_set_opt_fws(
					   enum platform_class_buckchg_role role,
					   int opt_fws)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_opt_fws)
		return -1;

	return p->ops->set_opt_fws(p->data, opt_fws);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_opt_fws);

int platform_class_buckchg_ops_set_otg(enum platform_class_buckchg_role role,
				       bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_otg)
		return -1;

	return p->ops->set_otg(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_otg);

int platform_class_buckchg_ops_set_otg_curr(
					    enum platform_class_buckchg_role role,
					    int otg_curr)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_otg_curr)
		return -1;

	return p->ops->set_otg_curr(p->data, otg_curr);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_otg_curr);

int platform_class_buckchg_ops_set_otg_volt(
					    enum platform_class_buckchg_role role,
					    int otg_volt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_otg_volt)
		return -1;

	return p->ops->set_otg_volt(p->data, otg_volt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_otg_volt);

int platform_class_buckchg_ops_set_prechg_curr(
					       enum platform_class_buckchg_role role,
					       int prechg_curr)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_prechg_curr)
		return -1;

	return p->ops->set_prechg_curr(p->data, prechg_curr);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_prechg_curr);

int platform_class_buckchg_ops_set_prechg_volt(
					       enum platform_class_buckchg_role role,
					       int prechg_volt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_prechg_volt)
		return -1;

	return p->ops->set_prechg_volt(p->data, prechg_volt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_prechg_volt);

int platform_class_buckchg_ops_set_qc3_volt(
					    enum platform_class_buckchg_role role,
					    int qc3_volt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_qc3_volt)
		return -1;

	return p->ops->set_qc3_volt(p->data, qc3_volt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_qc3_volt);

int platform_class_buckchg_ops_set_qc_volt(
					   enum platform_class_buckchg_role role,
					   int qc_volt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_qc_volt)
		return -1;

	return p->ops->set_qc_volt(p->data, qc_volt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_qc_volt);

int platform_class_buckchg_ops_set_rerun_aicl(
					      enum platform_class_buckchg_role role,
					      bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_rerun_aicl)
		return -1;

	return p->ops->set_rerun_aicl(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_rerun_aicl);

int platform_class_buckchg_ops_set_restart_aicl(
						enum platform_class_buckchg_role role,
						bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_restart_aicl)
		return -1;

	return p->ops->set_restart_aicl(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_restart_aicl);

int platform_class_buckchg_ops_set_ship_mode(
					     enum platform_class_buckchg_role role,
					     bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_ship_mode)
		return -1;

	return p->ops->set_ship_mode(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_ship_mode);

int platform_class_buckchg_ops_set_term(enum platform_class_buckchg_role role,
					bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_term)
		return -1;

	return p->ops->set_term(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_term);

int platform_class_buckchg_ops_set_term_curr(
					     enum platform_class_buckchg_role role,
					     int term_curr)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_term_curr)
		return -1;

	return p->ops->set_term_curr(p->data, term_curr);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_term_curr);

int platform_class_buckchg_ops_set_term_volt(
					     enum platform_class_buckchg_role role,
					     int term_volt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_term_volt)
		return -1;

	return p->ops->set_term_volt(p->data, term_volt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_term_volt);

int platform_class_buckchg_ops_set_too_hot_limit(
						 enum platform_class_buckchg_role role,
						 int too_hot_limit)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_too_hot_limit)
		return -1;

	return p->ops->set_too_hot_limit(p->data, too_hot_limit);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_too_hot_limit);

int platform_class_buckchg_ops_set_usb_aicl_cont_thd(
						     enum platform_class_buckchg_role role,
						     int usb_aicl_cont_thd)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_usb_aicl_cont_thd)
		return -1;

	return p->ops->set_usb_aicl_cont_thd(p->data, usb_aicl_cont_thd);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_usb_aicl_cont_thd);

int platform_class_buckchg_ops_set_wd_timeout(
					      enum platform_class_buckchg_role role,
					      int wd_timeout)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_wd_timeout)
		return -1;

	return p->ops->set_wd_timeout(p->data, wd_timeout);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_wd_timeout);

int platform_class_buckchg_ops_set_wls_hiz(
					   enum platform_class_buckchg_role role,
					   bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_wls_hiz)
		return -1;

	return p->ops->set_wls_hiz(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_wls_hiz);

int platform_class_buckchg_ops_set_wls_input_curr_lmt(
						      enum platform_class_buckchg_role role,
						      int wls_input_curr_lmt)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_wls_input_curr_lmt)
		return -1;

	return p->ops->set_wls_input_curr_lmt(p->data, wls_input_curr_lmt);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_wls_input_curr_lmt);

int platform_class_buckchg_ops_set_wls_vdd_flag(
						enum platform_class_buckchg_role role,
						bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->set_wls_vdd_flag)
		return -1;

	return p->ops->set_wls_vdd_flag(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_set_wls_vdd_flag);

int platform_class_buckchg_ops_usb_adapter_allow_override(
							  enum platform_class_buckchg_role role,
							  bool enable)
{
	struct platform_class_buckchg_data *p = platform_class_buckchg_lookup(role);

	if (!p || !p->ops->usb_adapter_allow_override)
		return -1;

	return p->ops->usb_adapter_allow_override(p->data, enable);
}
EXPORT_SYMBOL(platform_class_buckchg_ops_usb_adapter_allow_override);

/*
 * The registry above is reached by exported symbol rather than through a
 * device, so this driver matches nothing and has no probe; registering it
 * only leaves an entry under /sys/bus/platform/drivers.
 */
static struct platform_driver platform_class_buckchg_driver = {
	.driver = {
		.name = "platform_class_buckchg",
	},
};
module_platform_driver(platform_class_buckchg_driver);

MODULE_DESCRIPTION("platform buckchg class");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
