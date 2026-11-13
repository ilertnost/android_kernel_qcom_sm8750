/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The buck charger.
 *
 * Whatever else is going on, one chip sits between the charging input and the
 * cell and decides how much of what arrives actually reaches the battery.  It
 * is what charges the phone slowly from a laptop port, what runs the OTG
 * boost, what measures the input, and what the stack falls back to when no
 * charge pump is engaged.
 *
 * A phone with two charging inputs has a buck charger on each, so every call
 * names which one it is about.
 */

#ifndef __MCA_PLATFORM_BUCKCHG_H
#define __MCA_PLATFORM_BUCKCHG_H

#include <linux/types.h>

/* Which charging input. */
enum platform_class_buckchg_role {
	MAIN_BUCK_CHARGER,
	AUX_BUCK_CHARGER,
	MAX_BUCK_CHARGER,
};

/* What the charger reports it is doing. */
enum charge_status {
	CHGR_STATUS_INHIBIT,
	CHGR_STATUS_TRICKLE,
	CHGR_STATUS_PRECHARGE,
	CHGR_STATUS_FULLON,
	CHGR_STATUS_TAPER,
	CHGR_STATUS_TERMINATION,
	CHGR_STATUS_PAUSE,
	CHGR_STATUS_CHARGING_DISABLED,
	CHGR_STATUS_INVALID,
};

/* Where the boost that feeds an attached device comes from. */
enum otg_src_cfg {
	BOOST_SRC_CHARGER,
	BOOST_SRC_HBOOST,
	BOOST_SRC_EXTERNAL,
	BOOST_SRC_INVALID,
};

/* Which chip a gate pin belongs to. */
enum gpio_chip_cfg {
	GPIO_SOC_TYPE,
	GPIO_PMIC_TYPE,
};

/**
 * struct platform_class_buckchg_ops - what a buck charger driver provides
 *
 * Every call takes the @data the charger driver registered as its first
 * argument.  An entry left NULL means the charger cannot do that, and the
 * caller is told so rather than being given a wrong answer.
 */
struct platform_class_buckchg_ops {
	int (*is_init_ok)(void *data);
	int (*enable_hvdcp)(void *data, int hvdcp);
	int (*get_online)(void *data, int *online);
	int (*is_charge_done)(void *data, bool *charge_done);
	int (*get_hiz_status)(void *data, int *hiz_status);
	int (*get_input_volt_lmt)(void *data, int *input_volt_lmt);
	int (*get_input_curr_lmt)(void *data, int *input_curr_lmt);
	int (*get_bus_curr)(void *data, int *bus_curr);
	int (*get_bus_volt)(void *data, int *bus_volt);
	int (*get_usb_sns_volt)(void *data, int *usb_sns_volt);
	int (*get_ac_volt)(void *data, int *ac_volt);
	int (*get_batt_volt_sns)(void *data, int *batt_volt_sns);
	int (*get_batt_volt)(void *data, int *batt_volt);
	int (*get_batt_curr)(void *data, int *batt_curr);
	int (*get_sys_volt)(void *data, int *sys_volt);
	int (*get_bus_tsns)(void *data, int *bus_tsns);
	int (*get_batt_tsns)(void *data, int *batt_tsns);
	int (*get_die_temp)(void *data, int *die_temp);
	int (*get_batt_id)(void *data, int *batt_id);
	int (*get_chg_status)(void *data, int *chg_status);
	int (*get_chg_type)(void *data, int *chg_type);
	int (*get_term_curr)(void *data, int *term_curr);
	int (*get_term_volt)(void *data, int *term_volt);
	int (*get_wls_curr)(void *data, int *wls_curr);
	int (*set_hiz)(void *data, bool enable);
	int (*set_wls_hiz)(void *data, bool enable);
	int (*set_input_curr_lmt)(void *data, int input_curr_lmt);
	int (*set_wls_input_curr_lmt)(void *data, int wls_input_curr_lmt);
	int (*set_input_volt_lmt)(void *data, int input_volt_lmt);
	int (*set_ichg)(void *data, int ichg);
	int (*set_chg)(void *data, bool enable);
	int (*set_buck_fsw)(void *data, int buck_fsw);
	int (*set_otg)(void *data, bool enable);
	int (*set_otg_curr)(void *data, int otg_curr);
	int (*set_otg_volt)(void *data, int otg_volt);
	int (*set_term)(void *data, bool enable);
	int (*set_term_curr)(void *data, int term_curr);
	int (*set_term_volt)(void *data, int term_volt);
	int (*adc_enable)(void *data, bool enable);
	int (*set_prechg_volt)(void *data, int prechg_volt);
	int (*set_prechg_curr)(void *data, int prechg_curr);
	int (*force_dpdm)(void *data, int dpdm);
	int (*request_dpdm)(void *data, bool enable);
	int (*set_wd_timeout)(void *data, int wd_timeout);
	int (*kick_wd)(void *data);
	int (*set_qc_volt)(void *data, int qc_volt);
	int (*set_usb_aicl_cont_thd)(void *data, int usb_aicl_cont_thd);
	int (*get_usb_aicl_cont_thd)(void *data, int *usb_aicl_cont_thd);
	int (*set_opt_fws)(void *data, int opt_fws);
	int (*usb_adapter_allow_override)(void *data, bool enable);
	int (*set_qc3_volt)(void *data, int qc3_volt);
	int (*get_otg_boost_src)(void *data, int *otg_boost_src);
	int (*get_otg_gate_enable_status)(void *data, int *otg_gate_enable_status);
	int (*get_otg_boost_enable_status)(void *data, int *otg_boost_enable_status);
	int (*set_boost_enable)(void *data, int boost_enable);
	int (*set_boost_voltage)(void *data, int boost_voltage);
	int (*set_aicl_enable)(void *data, bool enable);
	int (*set_rerun_aicl)(void *data, bool enable);
	int (*set_restart_aicl)(void *data, bool enable);
	int (*is_support_cid)(void *data, bool *support_cid);
	int (*set_ship_mode)(void *data, bool enable);
	int (*get_ship_mode)(void *data, bool *ship_mode);
	int (*set_wls_vdd_flag)(void *data, bool enable);
	int (*get_lpd_enable)(void *data, int *lpd_enable);
	int (*get_lpd_status)(void *data, int *lpd_status);
	int (*get_lpd_sbu1)(void *data, int *lpd_sbu1);
	int (*get_lpd_sbu2)(void *data, int *lpd_sbu2);
	int (*get_lpd_cc1)(void *data, int *lpd_cc1);
	int (*get_lpd_cc2)(void *data, int *lpd_cc2);
	int (*get_lpd_dp)(void *data, int *lpd_dp);
	int (*get_lpd_dm)(void *data, int *lpd_dm);
	int (*set_lpd_sbu1)(void *data, int lpd_sbu1);
	int (*set_lpd_control)(void *data, int lpd_control);
	int (*get_lpd_control)(void *data, int *lpd_control);
	int (*set_lpd_uart_control)(void *data, int lpd_uart_control);
	int (*get_lpd_uart_control)(void *data, int *lpd_uart_control);
	int (*get_pack_vbat)(void *data, int *pack_vbat);
	int (*get_pack_ibat)(void *data, int *pack_ibat);
	int (*set_eu_model)(void *data, bool enable);
	int (*get_aicl_status)(void *data, int *aicl_status);
	int (*set_too_hot_limit)(void *data, int too_hot_limit);
	int (*get_pack_tbat)(void *data, int *pack_tbat);
};

int platform_class_buckchg_ops_register(enum platform_class_buckchg_role role,
					void *data,
					const struct platform_class_buckchg_ops *ops);

int platform_class_buckchg_ops_adc_enable(
					  enum platform_class_buckchg_role role,
					  bool enable);
int platform_class_buckchg_ops_enable_hvdcp(
					    enum platform_class_buckchg_role role,
					    int hvdcp);
int platform_class_buckchg_ops_force_dpdm(
					  enum platform_class_buckchg_role role,
					  int force_dpdm);
int platform_class_buckchg_ops_get_ac_volt(
					   enum platform_class_buckchg_role role,
					   int *ac_volt);
int platform_class_buckchg_ops_get_aicl_status(
					       enum platform_class_buckchg_role role,
					       int *aicl_status);
int platform_class_buckchg_ops_get_batt_curr(
					     enum platform_class_buckchg_role role,
					     int *batt_curr);
int platform_class_buckchg_ops_get_batt_id(
					   enum platform_class_buckchg_role role,
					   int *batt_id);
int platform_class_buckchg_ops_get_batt_tsns(
					     enum platform_class_buckchg_role role,
					     int *sns);
int platform_class_buckchg_ops_get_batt_volt(
					     enum platform_class_buckchg_role role,
					     int *batt_volt);
int platform_class_buckchg_ops_get_batt_volt_sns(
						 enum platform_class_buckchg_role role,
						 int *sns);
int platform_class_buckchg_ops_get_bus_curr(
					    enum platform_class_buckchg_role role,
					    int *bus_curr);
int platform_class_buckchg_ops_get_bus_tsns(
					    enum platform_class_buckchg_role role,
					    int *sns);
int platform_class_buckchg_ops_get_bus_volt(
					    enum platform_class_buckchg_role role,
					    int *bus_volt);
int platform_class_buckchg_ops_get_chg_status(
					      enum platform_class_buckchg_role role,
					      int *chg_status);
int platform_class_buckchg_ops_get_chg_type(
					    enum platform_class_buckchg_role role,
					    int *chg_type);
int platform_class_buckchg_ops_get_die_temp(
					    enum platform_class_buckchg_role role,
					    int *die_temp);
int platform_class_buckchg_ops_get_hiz_status(
					      enum platform_class_buckchg_role role,
					      int *hiz_status);
int platform_class_buckchg_ops_get_input_curr_lmt(
						  enum platform_class_buckchg_role role,
						  int *input_curr_lmt);
int platform_class_buckchg_ops_get_input_volt_lmt(
						  enum platform_class_buckchg_role role,
						  int *input_volt_lmt);
int platform_class_buckchg_ops_get_lpd_cc1(
					   enum platform_class_buckchg_role role,
					   int *lpd_cc1);
int platform_class_buckchg_ops_get_lpd_cc2(
					   enum platform_class_buckchg_role role,
					   int *lpd_cc2);
int platform_class_buckchg_ops_get_lpd_control(
					       enum platform_class_buckchg_role role,
					       int *lpd_control);
int platform_class_buckchg_ops_get_lpd_dm(
					  enum platform_class_buckchg_role role,
					  int *lpd_dm);
int platform_class_buckchg_ops_get_lpd_dp(
					  enum platform_class_buckchg_role role,
					  int *lpd_dp);
int platform_class_buckchg_ops_get_lpd_enable(
					      enum platform_class_buckchg_role role,
					      int *lpd_en);
int platform_class_buckchg_ops_get_lpd_sbu1(
					    enum platform_class_buckchg_role role,
					    int *lpd_sbu1);
int platform_class_buckchg_ops_get_lpd_sbu2(
					    enum platform_class_buckchg_role role,
					    int *lpd_sbu2);
int platform_class_buckchg_ops_get_lpd_status(
					      enum platform_class_buckchg_role role,
					      int *lpd_status);
int platform_class_buckchg_ops_get_lpd_uart_control(
						    enum platform_class_buckchg_role role,
						    int *lpd_uart_control);
int platform_class_buckchg_ops_get_online(
					  enum platform_class_buckchg_role role,
					  int *online);
int platform_class_buckchg_ops_get_otg_boost_enable_status(
							   enum platform_class_buckchg_role role,
							   int *otg_boost_enable_sts);
int platform_class_buckchg_ops_get_otg_boost_src(
						 enum platform_class_buckchg_role role,
						 int *otg_boost_src);
int platform_class_buckchg_ops_get_otg_gate_enable_status(
							  enum platform_class_buckchg_role role,
							  int *otg_gate_enable_sts);
int platform_class_buckchg_ops_get_pack_ibat(
					     enum platform_class_buckchg_role role,
					     int *pibat);
int platform_class_buckchg_ops_get_pack_tbat(
					     enum platform_class_buckchg_role role,
					     int *ptbat);
int platform_class_buckchg_ops_get_pack_vbat(
					     enum platform_class_buckchg_role role,
					     int *pvbat);
int platform_class_buckchg_ops_get_ship_mode(
					     enum platform_class_buckchg_role role,
					     bool *ship_mode);
int platform_class_buckchg_ops_get_sys_volt(
					    enum platform_class_buckchg_role role,
					    int *vsys_min);
int platform_class_buckchg_ops_get_term_curr(
					     enum platform_class_buckchg_role role,
					     int *term_curr);
int platform_class_buckchg_ops_get_term_volt(
					     enum platform_class_buckchg_role role,
					     int *term_volt);
int platform_class_buckchg_ops_get_usb_aicl_cont_thd(
						     enum platform_class_buckchg_role role,
						     int *usb_aicl_cont_thd);
int platform_class_buckchg_ops_get_usb_sns_volt(
						enum platform_class_buckchg_role role,
						int *bus_volt);
int platform_class_buckchg_ops_get_wls_curr(
					    enum platform_class_buckchg_role role,
					    int *wls_curr);
int platform_class_buckchg_ops_is_charge_done(
					      enum platform_class_buckchg_role role,
					      bool *charge_done);
int platform_class_buckchg_ops_is_init_ok(
					  enum platform_class_buckchg_role role);
int platform_class_buckchg_ops_is_support_cid(
					      enum platform_class_buckchg_role role,
					      bool *is_support_cid);
int platform_class_buckchg_ops_kick_wd(enum platform_class_buckchg_role role);
int platform_class_buckchg_ops_request_dpdm(
					    enum platform_class_buckchg_role role,
					    bool enable);
int platform_class_buckchg_ops_set_aicl_enable(
					       enum platform_class_buckchg_role role,
					       bool enable);
int platform_class_buckchg_ops_set_boost_enable(
						enum platform_class_buckchg_role role,
						int src_enable);
int platform_class_buckchg_ops_set_boost_voltage(
						 enum platform_class_buckchg_role role,
						 int src_value);
int platform_class_buckchg_ops_set_buck_fsw(
					    enum platform_class_buckchg_role role,
					    int buck_fsw);
int platform_class_buckchg_ops_set_chg(enum platform_class_buckchg_role role,
				       bool enable);
int platform_class_buckchg_ops_set_eu_model(
					    enum platform_class_buckchg_role role,
					    bool enable);
int platform_class_buckchg_ops_set_hiz(enum platform_class_buckchg_role role,
				       bool enable);
int platform_class_buckchg_ops_set_ichg(enum platform_class_buckchg_role role,
					int ichg);
int platform_class_buckchg_ops_set_input_curr_lmt(
						  enum platform_class_buckchg_role role,
						  int input_curr_lmt);
int platform_class_buckchg_ops_set_input_volt_lmt(
						  enum platform_class_buckchg_role role,
						  int input_volt_lmt);
int platform_class_buckchg_ops_set_lpd_control(
					       enum platform_class_buckchg_role role,
					       int lpd_control);
int platform_class_buckchg_ops_set_lpd_sbu1(
					    enum platform_class_buckchg_role role,
					    int lpd_sbu1);
int platform_class_buckchg_ops_set_lpd_uart_control(
						    enum platform_class_buckchg_role role,
						    int lpd_uart_control);
int platform_class_buckchg_ops_set_opt_fws(
					   enum platform_class_buckchg_role role,
					   int opt_fws);
int platform_class_buckchg_ops_set_otg(enum platform_class_buckchg_role role,
				       bool enable);
int platform_class_buckchg_ops_set_otg_curr(
					    enum platform_class_buckchg_role role,
					    int otg_curr);
int platform_class_buckchg_ops_set_otg_volt(
					    enum platform_class_buckchg_role role,
					    int otg_volt);
int platform_class_buckchg_ops_set_prechg_curr(
					       enum platform_class_buckchg_role role,
					       int prechg_curr);
int platform_class_buckchg_ops_set_prechg_volt(
					       enum platform_class_buckchg_role role,
					       int prechg_volt);
int platform_class_buckchg_ops_set_qc3_volt(
					    enum platform_class_buckchg_role role,
					    int qc3_volt);
int platform_class_buckchg_ops_set_qc_volt(
					   enum platform_class_buckchg_role role,
					   int qc_volt);
int platform_class_buckchg_ops_set_rerun_aicl(
					      enum platform_class_buckchg_role role,
					      bool enable);
int platform_class_buckchg_ops_set_restart_aicl(
						enum platform_class_buckchg_role role,
						bool enable);
int platform_class_buckchg_ops_set_ship_mode(
					     enum platform_class_buckchg_role role,
					     bool enable);
int platform_class_buckchg_ops_set_term(enum platform_class_buckchg_role role,
					bool enable);
int platform_class_buckchg_ops_set_term_curr(
					     enum platform_class_buckchg_role role,
					     int term_curr);
int platform_class_buckchg_ops_set_term_volt(
					     enum platform_class_buckchg_role role,
					     int term_volt);
int platform_class_buckchg_ops_set_too_hot_limit(
						 enum platform_class_buckchg_role role,
						 int too_hot_limit);
int platform_class_buckchg_ops_set_usb_aicl_cont_thd(
						     enum platform_class_buckchg_role role,
						     int usb_aicl_cont_thd);
int platform_class_buckchg_ops_set_wd_timeout(
					      enum platform_class_buckchg_role role,
					      int wd_timeout);
int platform_class_buckchg_ops_set_wls_hiz(
					   enum platform_class_buckchg_role role,
					   bool enable);
int platform_class_buckchg_ops_set_wls_input_curr_lmt(
						      enum platform_class_buckchg_role role,
						      int wls_input_curr_lmt);
int platform_class_buckchg_ops_set_wls_vdd_flag(
						enum platform_class_buckchg_role role,
						bool enable);
int platform_class_buckchg_ops_usb_adapter_allow_override(
							  enum platform_class_buckchg_role role,
							  bool enable);

/* How far along a charge the buck charger says it is. */
enum MCA_BATT_CHGR_STATUS_TYPE {
	MCA_BATT_CHGR_STATUS_INHIBIT = 0,
	MCA_BATT_CHGR_STATUS_TRICKLE,
	MCA_BATT_CHGR_STATUS_PRECHARGE,
	MCA_BATT_CHGR_STATUS_FULLON,
	MCA_BATT_CHGR_STATUS_TAPER,
	MCA_BATT_CHGR_STATUS_TERMINATION,
	MCA_BATT_CHGR_STATUS_PAUSE,
	MCA_BATT_CHGR_STATUS_CHARGING_DISABLED,
	MCA_BATT_CHGR_STATUS_FAST_LINEAR,
};

/*
 * Which consumer a boost is being turned on for.  The three share one
 * regulator, so the request says who is asking as well as what for.
 */
typedef enum en_src_cfg {
	OTG_EN_BOOST,
	WIRELESS_EN_BOOST,
	REV_EN_BOOST,
} EN_SRC;

#endif /* __MCA_PLATFORM_BUCKCHG_H */
