/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The fuel gauge.
 *
 * Everything the phone believes about its battery -- how full it is, how much
 * it will hold now compared to when it was new, how many cycles it has been
 * through, how warm the cell is -- comes from a gauge chip that watches the
 * current in and out and models the cell from it.  A phone with two cells in
 * series has a gauge for each.
 *
 * The gauge also holds what the pack says about itself, including the
 * challenge and response that tell a genuine pack from a counterfeit one.
 */

#ifndef __MCA_PLATFORM_FG_IC_H
#define __MCA_PLATFORM_FG_IC_H

#include <linux/types.h>

/* Which gauge. */
enum fg_ic_role {
	FG_IC_MASTER,
	FG_IC_SLAVE,
	FG_IC_MAX,
};

/*
 * Where a cell physically is, on a phone that has more than one.  A gauge's
 * role says which of two parts is being talked to; its site says which half
 * of the phone the cell it measures is in, and the two are not the same
 * thing on a board where one gauge reads both.
 */
enum fg_ic_site {
	FG_IC_BASE,
	FG_IC_FLIP,
	FG_SITE_MAX,
};

/*
 * Every value a gauge can be asked for by name.  These are the indices of
 * the debug files a gauge driver exposes, so the numbering is the interface
 * and not an internal ordering: a value's position matters, its name does
 * not.
 */
enum fg_ic_prop_id {
	FG_IC_PROP_CHIP_OK = 0,
	FG_IC_PROP_VOL = 1,
	FG_IC_PROP_PACKVOL = 2,
	FG_IC_PROP_CO_STATUS = 3,
	FG_IC_PROP_CURRENT = 4,
	FG_IC_PROP_RSOC = 5,
	FG_IC_PROP_TEMP = 6,
	FG_IC_PROP_ORIGINAL_TEMP = 7,
	FG_IC_PROP_CYCLE = 8,
	FG_IC_PROP_QMAX = 9,
	FG_IC_PROP_RM = 10,
	FG_IC_PROP_FCC = 11,
	FG_IC_PROP_SOH = 12,
	FG_IC_PROP_FCC_SOH = 13,
	FG_IC_PROP_FAST_CHARGE = 14,
	FG_IC_PROP_RESISTANCE_ID = 15,
	FG_IC_PROP_TSIM = 16,
	FG_IC_PROP_TAMBIENT = 17,
	FG_IC_PROP_TREMQ = 18,
	FG_IC_PROP_TFULLCHGQ = 19,
	FG_IC_PROP_AVERCURRENT = 20,
	FG_IC_PROP_SEAL = 21,
	FG_IC_PROP_DF_CHECK = 22,
	FG_IC_PROP_CHEM_DF_SIGN = 23,
	FG_IC_PROP_VENDOR_ID = 24,
	FG_IC_PROP_PACK_VENDOR_ID = 25,
	FG_IC_PROP_CELL_VENDOR_ID = 26,
	FG_IC_PROP_EEPROM_VERSION = 27,
	FG_IC_PROP_DESIGN_CAPACITY = 28,
	FG_IC_PROP_CALC_RVALUE = 29,
	FG_IC_PROP_AGED_FLAG = 30,
	FG_IC_PROP_UI_SOH = 31,
	FG_IC_PROP_BATT_SN = 32,
	FG_IC_PROP_MANUFACTURING_DATE = 33,
	FG_IC_PROP_FIRST_USAGE_DATE = 34,
	FG_IC_PROP_FAKE_FIRST_USAGE_DATE = 35,
	FG_IC_PROP_SOH_NEW = 36,
	FG_IC_PROP_DOD_COUNT = 37,
	FG_IC_PROP_COUNT_LEVEL1 = 38,
	FG_IC_PROP_COUNT_LEVEL2 = 39,
	FG_IC_PROP_COUNT_LEVEL3 = 40,
	FG_IC_PROP_COUNT_LOWTEMP = 41,
	FG_IC_PROP_CUTOFF_VOL = 42,
	FG_IC_PROP_DEVICE_NAME = 43,
	FG_IC_PROP_MAX_TEMP_OCCUR_TIME = 44,
	FG_IC_PROP_RUN_TIME = 45,
	FG_IC_PROP_MAX_TEMP_TIME = 46,
	FG_IC_PROP_TOTAL_FW_RUN_TIME = 47,
	FG_IC_PROP_TEMP_MAX = 48,
	FG_IC_PROP_TIME_HT = 49,
	FG_IC_PROP_TIME_OT = 50,
	FG_IC_PROP_OVER_PEAK_FLAG = 51,
	FG_IC_PROP_MONITOR_ISC = 52,
	FG_IC_PROP_MONITOR_SOA = 53,
	FG_IC_PROP_CURRENT_DEVIATION = 54,
	FG_IC_PROP_POWER_DEVIATION = 55,
	FG_IC_PROP_AVERAGE_CURRENT = 56,
	FG_IC_PROP_AVERAGE_TEMPERATURE = 57,
	FG_IC_PROP_START_LEARNING = 58,
	FG_IC_PROP_STOP_LEARNING = 59,
	FG_IC_PROP_ACTUAL_POWER = 60,
	FG_IC_PROP_LEARNING_POWER = 61,
	FG_IC_PROP_LEARNING_POWER_DEV = 62,
	FG_IC_PROP_LEARNING_TIME_DEV = 63,
	FG_IC_PROP_CONSTANT_POWER = 64,
	FG_IC_PROP_REMAINING_TIME = 65,
	FG_IC_PROP_REFERANCE_POWER = 66,
	FG_IC_PROP_NVT_REFERANCE_POWER = 67,
	FG_IC_PROP_REFERANCE_CURRENT = 68,
	FG_IC_PROP_START_LEARNING_B = 69,
	FG_IC_PROP_STOP_LEARNING_B = 70,
	FG_IC_PROP_LEARNING_POWER_B = 71,
	FG_IC_PROP_ACTUAL_POWER_B = 72,
	FG_IC_PROP_LEARNING_POWER_DEV_B = 73,
	FG_IC_PROP_REL_SOH = 74,
	FG_IC_PROP_REL_SOH_CYCLECOUNT = 75,
	FG_IC_PROP_EIS_SOH = 76,
	FG_IC_PROP_EIS_SOH_CYCLECOUNT = 77,
	FG_IC_PROP_QMAX_CYCLECOUNT = 78,
	FG_IC_PROP_BATT_USE_ENVIRRONMENT = 79,
	FG_IC_PROP_MAX_LIFE_VOL = 80,
	FG_IC_PROP_MIN_LIFE_VOL = 81,
	FG_IC_PROP_MAX_LIFE_TEMP = 82,
	FG_IC_PROP_MIN_LIFE_TEMP = 83,
	FG_IC_PROP_OVER_VOL_DURATION = 84,
	FG_IC_PROP_FC = 85,
	FG_IC_PROP_CSD_FLAG = 86,
	FG_IC_PROP_CSD_R1 = 87,
	FG_IC_PROP_CSD_R2 = 88,
	FG_IC_PROP_DCR_SLOPE1 = 89,
	FG_IC_PROP_DCR_SLOPE2 = 90,
	FG_IC_PROP_DCR_SLOPE3 = 91,
	FG_IC_PROP_CHARGE_ABSOC = 92,
	FG_IC_PROP_MAX_DSG_CURRENT = 93,
	FG_IC_PROP_MIN_TEMP = 94,
	FG_IC_PROP_MIN_VOLTAGE = 95,
	FG_IC_PROP_CUV_COUNT = 96,
	FG_IC_PROP_CUV_LAST_CYCLE = 97,
	FG_IC_PROP_HCUV_COUNT = 98,
	FG_IC_PROP_HCUV_LAST_CYCLE = 99,
	FG_IC_PROP_OCD_COUNT = 100,
	FG_IC_PROP_OCD_LAST_CYCLE = 101,
	FG_IC_PROP_HOCD_COUNT = 102,
	FG_IC_PROP_HOCD_LAST_CYCLE = 103,
	FG_IC_PROP_HSCD_COUNT = 104,
	FG_IC_PROP_HSCD_LAST_CYCLE = 105,
	FG_IC_PROP_CO_AUTO_OPEN = 106,
	FG_IC_PROP_MAX,
};

/**
 * struct fuelguage_ic_ops - what a fuel gauge driver provides
 *
 * The member set and its order are the gauge class's interface: a driver
 * fills in what its part can answer and leaves the rest NULL, and the class
 * checks before calling.  Every call is handed the driver's own @data.
 */
struct fuelguage_ic_ops {
	int (*fg_ic_probe_ok)(void *data, bool *ok);
	int (*fg_ic_get_batt_info)(void *data, void *info);
	int (*fg_ic_get_soc)(void *data);
	int (*fg_ic_get_rsoc)(void *data, int *rsoc);
	int (*fg_ic_get_curr)(void *data, int *curr);
	int (*fg_ic_get_volt)(void *data, int *volt);
	int (*fg_ic_set_temp)(void *data, int value);
	int (*fg_ic_get_temp)(void *data, int *temp);
	int (*fg_ic_get_original_temp)(void *data, int *temp);
	int (*fg_ic_set_iterm)(void *data, int iterm);
	int (*fg_ic_get_charge_status)(void *data);
	int (*fg_ic_get_rm)(void *data, int *rm);
	int (*fg_ic_get_fastcharge)(void *data, int *ffc);
	int (*fg_ic_set_fastcharge)(void *data, bool en);
	int (*fg_ic_get_chg_vol)(void *data, int *volt);
	int (*fg_ic_get_chip_ok)(void *data, int *ok);
	int (*fg_ic_get_cyclecount)(void *data, int *cc);
	int (*fg_ic_get_capacity_level)(void *data);
	int (*fg_ic_get_tte)(void *data, int *tte);
	int (*fg_ic_get_ttf)(void *data, int *ttf);
	int (*fg_ic_get_fcc)(void *data, int *fcc);
	int (*fg_ic_get_full_design)(void *data, int *dc);
	int (*fg_ic_get_decimal_rate)(void *data, int *rate);
	int (*fg_ic_get_decimal)(void *data, int *soc_decimal);
	int (*fg_ic_get_soh)(void *data, int *soh);
	int (*fg_ic_get_temp_max)(void *data, int *temp_max);
	int (*fg_ic_get_time_ot)(void *data, int *time_ot);
	int (*fg_ic_get_batt_cell_info)(void *data, const char **name);
	int (*fg_ic_set_verify_digest)(void *data, char *buf);
	int (*fg_ic_get_verify_digest)(void *data, char *buf);
	int (*fg_ic_set_authentic)(void *data, int value);
	int (*fg_ic_get_authentic)(void *data, int *value);
	int (*fg_ic_get_error_state)(void *data, bool *error);
	int (*fg_ic_get_cutoff_voltage)(void *data, int *volt);
	int (*fg_ic_set_cutoff_voltage)(void *data, int value);
	int (*fg_ic_get_dod_count)(void *data);
	int (*fg_ic_get_count_level1)(void *data, int *count);
	int (*fg_ic_get_count_level2)(void *data, int *count);
	int (*fg_ic_get_count_level3)(void *data, int *count);
	int (*fg_ic_get_count_lowtemp)(void *data, int *count);
	int (*fg_ic_set_clear_count_data)(void *data);
	int (*fg_ic_get_adapt_power)(void *data, int *adapt_power);
	int (*fg_ic_get_aged_flag)(void *data, int *aged_flag);
	int (*fg_ic_get_isc_alert_level)(void *data, int *level);
	int (*fg_ic_get_soa_alert_level)(void *data, int *level);
	int (*fg_ic_get_raw_soc)(void *data, int *raw_soc);
	void (*fg_ic_update_fw)(void *data);
	int (*fg_ic_get_device_name)(void *data, const char **device_name);
	int (*fg_ic_get_temp_min)(void *data, int *temp_min);
	void (*fg_ic_set_force_report_full)(void *data);
	int (*fg_ic_get_fc)(void *data, bool *fc);
	int (*fg_ic_set_co)(void *data, bool value);
	int (*fg_ic_get_calibration_ffc_iterm)(void *data, int *iterm);
	int (*fg_ic_get_real_supplement_energy)(void *data, int *energy);
	int (*fg_ic_get_calibration_charge_energy)(void *data, int *energy);
	void (*fg_ic_fl4p0_enable_check)(void *data);
	void (*fg_ic_get_ui_soh)(void *data, int *ui_soh);
	void (*fg_ic_qbg_send_chg_data)(void *data, int chg_status,
					int ibat_max);
	void (*fg_ic_get_pack_vendor)(void *data, int *vendor);
	unsigned long (*fg_ic_get_calc_rvalue)(void *data);
	void (*fg_ic_get_ota_update_flag)(void *data, u32 *flag);
	void (*fg_ic_ota_update_check)(void *data);
	void (*fg_ic_get_average_current)(void *data, int *curr);
	void (*fg_ic_get_batt_abnormal_info)(void *data, void *info);
	int (*fg_ic_get_manufacturing_date)(void *data, u8 *date);
	void (*fg_ic_set_first_usage_date)(void *data, const char *date);
	int (*fg_ic_get_first_usage_date)(void *data, u8 *date);
};

int platform_fg_ic_ops_register(enum fg_ic_role ic_role, void *data,
				const struct fuelguage_ic_ops *platform_fg_ops);

int platform_fg_ops_probe_ok(enum fg_ic_role ic_role, bool *probe_ok);
int platform_fg_ops_get_batt_info(enum fg_ic_role ic_role, void *batt_info);
int platform_fg_ops_get_soc(enum fg_ic_role ic_role);
int platform_fg_ops_get_rsoc(enum fg_ic_role ic_role, int *rsoc);
int platform_fg_ops_get_curr(enum fg_ic_role ic_role, int *curr);
int platform_fg_ops_get_volt(enum fg_ic_role ic_role, int *volt);
int platform_fg_ops_set_temp(enum fg_ic_role ic_role, int temp);
int platform_fg_ops_get_temp(enum fg_ic_role ic_role, int *temp);
int platform_fg_ops_get_original_temp(enum fg_ic_role ic_role,
				      int *original_temp);
int platform_fg_ops_set_iterm(enum fg_ic_role ic_role, int iterm);
int platform_fg_ops_get_charge_status(enum fg_ic_role ic_role);
int platform_fg_ops_get_rm(enum fg_ic_role ic_role, int *rm);
int platform_fg_ops_get_fastcharge(enum fg_ic_role ic_role, int *ffc);
int platform_fg_ops_set_fastcharge(enum fg_ic_role ic_role, bool enable);
int platform_fg_ops_get_chg_vol(enum fg_ic_role ic_role, int *volt);
int platform_fg_ops_get_chip_ok(enum fg_ic_role ic_role, int *chip_ok);
int platform_fg_ops_get_cyclecount(enum fg_ic_role ic_role, int *cyclecount);
int platform_fg_ops_get_tte(enum fg_ic_role ic_role, int *tte);
int platform_fg_ops_get_ttf(enum fg_ic_role ic_role, int *ttf);
int platform_fg_ops_get_fcc(enum fg_ic_role ic_role, int *fcc);
int platform_fg_ops_get_full_design(enum fg_ic_role ic_role, int *full_design);
int platform_fg_ops_get_decimal_rate(enum fg_ic_role ic_role,
				     int *decimal_rate);
int platform_fg_ops_get_decimal(enum fg_ic_role ic_role, int *decimal);
int platform_fg_ops_get_soh(enum fg_ic_role ic_role, int *soh);
int platform_fg_ops_get_temp_max(enum fg_ic_role ic_role, int *temp_max);
int platform_fg_ops_get_time_ot(enum fg_ic_role ic_role, int *time_ot);
int platform_fg_ops_get_batt_cell_info(enum fg_ic_role ic_role,
				       const char **batt_cell_info);
int platform_fg_ops_set_verify_digest(enum fg_ic_role ic_role,
				      char *verify_digest);
int platform_fg_ops_get_verify_digest(enum fg_ic_role ic_role,
				      char *verify_digest);
int platform_fg_ops_set_authentic(enum fg_ic_role ic_role, int authentic);
int platform_fg_ops_get_authentic(enum fg_ic_role ic_role, int *authentic);
int platform_fg_ops_get_error_state(enum fg_ic_role ic_role, bool *error_state);
int platform_fg_ops_get_cutoff_voltage(enum fg_ic_role ic_role, int *volt);
int platform_fg_ops_set_cutoff_voltage(enum fg_ic_role ic_role,
				       int cutoff_voltage);
int platform_fg_ops_get_dod_count(enum fg_ic_role ic_role);
int platform_fg_ops_get_count_level1(enum fg_ic_role ic_role,
				     int *count_level1);
int platform_fg_ops_get_count_level2(enum fg_ic_role ic_role,
				     int *count_level2);
int platform_fg_ops_get_count_level3(enum fg_ic_role ic_role,
				     int *count_level3);
int platform_fg_ops_get_count_lowtemp(enum fg_ic_role ic_role,
				      int *count_lowtemp);
int platform_fg_ops_set_clear_count_data(enum fg_ic_role ic_role);
int platform_fg_ops_get_adapt_power(enum fg_ic_role ic_role, int *adapt_power);
int platform_fg_ops_get_aged_flag(enum fg_ic_role ic_role, int *aged_flag);
int platform_fg_ops_get_isc_alert_level(enum fg_ic_role ic_role,
					int *isc_alert_level);
int platform_fg_ops_get_soa_alert_level(enum fg_ic_role ic_role,
					int *soa_alert_level);
int platform_fg_ops_get_raw_soc(enum fg_ic_role ic_role, int *raw_soc);
void platform_fg_ops_update_fw(enum fg_ic_role ic_role);
int platform_fg_ops_get_device_name(enum fg_ic_role ic_role,
				    const char **device_name);
int platform_fg_ops_get_temp_min(enum fg_ic_role ic_role, int *temp_min);
void platform_fg_ops_set_force_report_full(enum fg_ic_role ic_role);
int platform_fg_ops_get_fc(enum fg_ic_role ic_role, bool *fc);
int platform_fg_ops_set_co(enum fg_ic_role ic_role, bool enable);
int platform_fg_ops_get_calibration_ffc_iterm(enum fg_ic_role ic_role,
					      int *calibration_ffc_iterm);
int platform_fg_ops_get_real_supplement_energy(enum fg_ic_role ic_role,
					       int *supplement_energy);
int platform_fg_ops_get_calibration_charge_energy(enum fg_ic_role ic_role,
						  int *charge_energy);
void platform_fg_ops_fl4p0_enable_check(enum fg_ic_role ic_role);
void platform_fg_ops_get_ui_soh(enum fg_ic_role ic_role, int *ui_soh);
void platform_fg_ops_qbg_send_chg_data(enum fg_ic_role ic_role, int chg_status,
				       int ibat_max);
void platform_fg_ops_get_pack_vendor(enum fg_ic_role ic_role, int *pack_vendor);
unsigned long platform_fg_ops_get_calc_rvalue(enum fg_ic_role ic_role);
void platform_fg_ops_get_ota_update_flag(enum fg_ic_role ic_role, u32 *flag);
void platform_fg_ops_ota_update_check(enum fg_ic_role ic_role);
void platform_fg_ops_get_average_current(enum fg_ic_role ic_role,
					 int *average_current);
void platform_fg_ops_get_batt_abnormal_info(enum fg_ic_role ic_role,
					    void *info);
int platform_fg_ops_get_manufacturing_date(enum fg_ic_role ic_role,
					   u8 *manufacturing_date);
void platform_fg_ops_set_first_usage_date(enum fg_ic_role ic_role,
					  const char *date);
int platform_fg_ops_get_first_usage_date(enum fg_ic_role ic_role,
					 u8 *first_usage_date);
#endif /* __MCA_PLATFORM_FG_IC_H */
