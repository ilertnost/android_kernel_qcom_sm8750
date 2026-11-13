// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The fuel gauge.  See include/mca/platform/platform_fg_ic_ops.h.
 */

#define MCA_LOG_TAG "platform_fg_ic_ops"

#include <linux/errno.h>
#include <mca/common/mca_log.h>
#include <mca/platform/platform_fg_ic_ops.h>
#include <linux/module.h>
#include <linux/platform_device.h>

/**
 * struct fuelguage_info - one registered gauge
 * @data: handed back to every call
 * @ops:  what the gauge driver provides
 */
struct fuelguage_info {
	void				*data;
	const struct fuelguage_ic_ops	*ops;
};

static struct fuelguage_info g_fg_ic_ops[FG_IC_MAX];

int platform_fg_ic_ops_register(enum fg_ic_role ic_role, void *data,
				const struct fuelguage_ic_ops *platform_fg_ops)
{
	if (ic_role >= FG_IC_MAX || !data || !platform_fg_ops)
		return -EOPNOTSUPP;

	g_fg_ic_ops[ic_role].ops = platform_fg_ops;
	g_fg_ic_ops[ic_role].data = data;

	return 0;
}
EXPORT_SYMBOL(platform_fg_ic_ops_register);

/*
 * Look up the driver for one gauge.  A gauge that has not probed gives NULL,
 * and the caller reports -EOPNOTSUPP rather than a made up value: a phone told
 * the battery is at zero percent would shut down on a full cell.  The calls
 * that return nothing simply do nothing.
 */
static struct fuelguage_info *fg_ic_lookup(enum fg_ic_role ic_role)
{
	if (ic_role >= FG_IC_MAX)
		return NULL;

	if (!g_fg_ic_ops[ic_role].ops)
		return NULL;

	return &g_fg_ic_ops[ic_role];
}

int platform_fg_ops_probe_ok(enum fg_ic_role ic_role, bool *probe_ok)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_probe_ok)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_probe_ok(p->data, probe_ok);
}
EXPORT_SYMBOL(platform_fg_ops_probe_ok);

int platform_fg_ops_get_batt_info(enum fg_ic_role ic_role, void *batt_info)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_batt_info)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_batt_info(p->data, batt_info);
}
EXPORT_SYMBOL(platform_fg_ops_get_batt_info);

int platform_fg_ops_get_soc(enum fg_ic_role ic_role)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_soc)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_soc(p->data);
}
EXPORT_SYMBOL(platform_fg_ops_get_soc);

int platform_fg_ops_get_rsoc(enum fg_ic_role ic_role, int *rsoc)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_rsoc)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_rsoc(p->data, rsoc);
}
EXPORT_SYMBOL(platform_fg_ops_get_rsoc);

int platform_fg_ops_get_curr(enum fg_ic_role ic_role, int *curr)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_curr)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_curr(p->data, curr);
}
EXPORT_SYMBOL(platform_fg_ops_get_curr);

int platform_fg_ops_get_volt(enum fg_ic_role ic_role, int *volt)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_volt)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_volt(p->data, volt);
}
EXPORT_SYMBOL(platform_fg_ops_get_volt);

int platform_fg_ops_set_temp(enum fg_ic_role ic_role, int temp)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_set_temp)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_set_temp(p->data, temp);
}
EXPORT_SYMBOL(platform_fg_ops_set_temp);

int platform_fg_ops_get_temp(enum fg_ic_role ic_role, int *temp)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_temp)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_temp(p->data, temp);
}
EXPORT_SYMBOL(platform_fg_ops_get_temp);

int platform_fg_ops_get_original_temp(enum fg_ic_role ic_role,
				      int *original_temp)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_original_temp)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_original_temp(p->data, original_temp);
}
EXPORT_SYMBOL(platform_fg_ops_get_original_temp);

int platform_fg_ops_set_iterm(enum fg_ic_role ic_role, int iterm)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_set_iterm)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_set_iterm(p->data, iterm);
}
EXPORT_SYMBOL(platform_fg_ops_set_iterm);

int platform_fg_ops_get_charge_status(enum fg_ic_role ic_role)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_charge_status)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_charge_status(p->data);
}
EXPORT_SYMBOL(platform_fg_ops_get_charge_status);

int platform_fg_ops_get_rm(enum fg_ic_role ic_role, int *rm)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_rm)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_rm(p->data, rm);
}
EXPORT_SYMBOL(platform_fg_ops_get_rm);

int platform_fg_ops_get_fastcharge(enum fg_ic_role ic_role, int *ffc)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_fastcharge)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_fastcharge(p->data, ffc);
}
EXPORT_SYMBOL(platform_fg_ops_get_fastcharge);

int platform_fg_ops_set_fastcharge(enum fg_ic_role ic_role, bool enable)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_set_fastcharge)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_set_fastcharge(p->data, enable);
}
EXPORT_SYMBOL(platform_fg_ops_set_fastcharge);

int platform_fg_ops_get_chg_vol(enum fg_ic_role ic_role, int *volt)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_chg_vol)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_chg_vol(p->data, volt);
}
EXPORT_SYMBOL(platform_fg_ops_get_chg_vol);

int platform_fg_ops_get_chip_ok(enum fg_ic_role ic_role, int *chip_ok)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_chip_ok)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_chip_ok(p->data, chip_ok);
}
EXPORT_SYMBOL(platform_fg_ops_get_chip_ok);

int platform_fg_ops_get_cyclecount(enum fg_ic_role ic_role, int *cyclecount)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_cyclecount)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_cyclecount(p->data, cyclecount);
}
EXPORT_SYMBOL(platform_fg_ops_get_cyclecount);

int platform_fg_ops_get_tte(enum fg_ic_role ic_role, int *tte)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_tte)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_tte(p->data, tte);
}
EXPORT_SYMBOL(platform_fg_ops_get_tte);

int platform_fg_ops_get_ttf(enum fg_ic_role ic_role, int *ttf)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_ttf)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_ttf(p->data, ttf);
}
EXPORT_SYMBOL(platform_fg_ops_get_ttf);

int platform_fg_ops_get_fcc(enum fg_ic_role ic_role, int *fcc)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_fcc)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_fcc(p->data, fcc);
}
EXPORT_SYMBOL(platform_fg_ops_get_fcc);

int platform_fg_ops_get_full_design(enum fg_ic_role ic_role, int *full_design)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_full_design)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_full_design(p->data, full_design);
}
EXPORT_SYMBOL(platform_fg_ops_get_full_design);

int platform_fg_ops_get_decimal_rate(enum fg_ic_role ic_role, int *decimal_rate)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_decimal_rate)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_decimal_rate(p->data, decimal_rate);
}
EXPORT_SYMBOL(platform_fg_ops_get_decimal_rate);

int platform_fg_ops_get_decimal(enum fg_ic_role ic_role, int *decimal)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_decimal)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_decimal(p->data, decimal);
}
EXPORT_SYMBOL(platform_fg_ops_get_decimal);

int platform_fg_ops_get_soh(enum fg_ic_role ic_role, int *soh)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_soh)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_soh(p->data, soh);
}
EXPORT_SYMBOL(platform_fg_ops_get_soh);

int platform_fg_ops_get_temp_max(enum fg_ic_role ic_role, int *temp_max)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_temp_max)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_temp_max(p->data, temp_max);
}
EXPORT_SYMBOL(platform_fg_ops_get_temp_max);

int platform_fg_ops_get_time_ot(enum fg_ic_role ic_role, int *time_ot)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_time_ot)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_time_ot(p->data, time_ot);
}
EXPORT_SYMBOL(platform_fg_ops_get_time_ot);

int platform_fg_ops_get_batt_cell_info(enum fg_ic_role ic_role,
				       const char **batt_cell_info)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_batt_cell_info)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_batt_cell_info(p->data, batt_cell_info);
}
EXPORT_SYMBOL(platform_fg_ops_get_batt_cell_info);

int platform_fg_ops_set_verify_digest(enum fg_ic_role ic_role,
				      char *verify_digest)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_set_verify_digest)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_set_verify_digest(p->data, verify_digest);
}
EXPORT_SYMBOL(platform_fg_ops_set_verify_digest);

int platform_fg_ops_get_verify_digest(enum fg_ic_role ic_role,
				      char *verify_digest)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_verify_digest)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_verify_digest(p->data, verify_digest);
}
EXPORT_SYMBOL(platform_fg_ops_get_verify_digest);

int platform_fg_ops_set_authentic(enum fg_ic_role ic_role, int authentic)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_set_authentic)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_set_authentic(p->data, authentic);
}
EXPORT_SYMBOL(platform_fg_ops_set_authentic);

int platform_fg_ops_get_authentic(enum fg_ic_role ic_role, int *authentic)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_authentic)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_authentic(p->data, authentic);
}
EXPORT_SYMBOL(platform_fg_ops_get_authentic);

int platform_fg_ops_get_error_state(enum fg_ic_role ic_role, bool *error_state)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_error_state)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_error_state(p->data, error_state);
}
EXPORT_SYMBOL(platform_fg_ops_get_error_state);

int platform_fg_ops_get_cutoff_voltage(enum fg_ic_role ic_role, int *volt)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_cutoff_voltage)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_cutoff_voltage(p->data, volt);
}
EXPORT_SYMBOL(platform_fg_ops_get_cutoff_voltage);

int platform_fg_ops_set_cutoff_voltage(enum fg_ic_role ic_role,
				       int cutoff_voltage)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_set_cutoff_voltage)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_set_cutoff_voltage(p->data, cutoff_voltage);
}
EXPORT_SYMBOL(platform_fg_ops_set_cutoff_voltage);

int platform_fg_ops_get_dod_count(enum fg_ic_role ic_role)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_dod_count)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_dod_count(p->data);
}
EXPORT_SYMBOL(platform_fg_ops_get_dod_count);

int platform_fg_ops_get_count_level1(enum fg_ic_role ic_role, int *count_level1)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_count_level1)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_count_level1(p->data, count_level1);
}
EXPORT_SYMBOL(platform_fg_ops_get_count_level1);

int platform_fg_ops_get_count_level2(enum fg_ic_role ic_role, int *count_level2)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_count_level2)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_count_level2(p->data, count_level2);
}
EXPORT_SYMBOL(platform_fg_ops_get_count_level2);

int platform_fg_ops_get_count_level3(enum fg_ic_role ic_role, int *count_level3)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_count_level3)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_count_level3(p->data, count_level3);
}
EXPORT_SYMBOL(platform_fg_ops_get_count_level3);

int platform_fg_ops_get_count_lowtemp(enum fg_ic_role ic_role,
				      int *count_lowtemp)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_count_lowtemp)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_count_lowtemp(p->data, count_lowtemp);
}
EXPORT_SYMBOL(platform_fg_ops_get_count_lowtemp);

int platform_fg_ops_set_clear_count_data(enum fg_ic_role ic_role)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_set_clear_count_data)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_set_clear_count_data(p->data);
}
EXPORT_SYMBOL(platform_fg_ops_set_clear_count_data);

int platform_fg_ops_get_adapt_power(enum fg_ic_role ic_role, int *adapt_power)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_adapt_power)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_adapt_power(p->data, adapt_power);
}
EXPORT_SYMBOL(platform_fg_ops_get_adapt_power);

int platform_fg_ops_get_aged_flag(enum fg_ic_role ic_role, int *aged_flag)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_aged_flag)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_aged_flag(p->data, aged_flag);
}
EXPORT_SYMBOL(platform_fg_ops_get_aged_flag);

int platform_fg_ops_get_isc_alert_level(enum fg_ic_role ic_role,
					int *isc_alert_level)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_isc_alert_level)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_isc_alert_level(p->data, isc_alert_level);
}
EXPORT_SYMBOL(platform_fg_ops_get_isc_alert_level);

int platform_fg_ops_get_soa_alert_level(enum fg_ic_role ic_role,
					int *soa_alert_level)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_soa_alert_level)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_soa_alert_level(p->data, soa_alert_level);
}
EXPORT_SYMBOL(platform_fg_ops_get_soa_alert_level);

int platform_fg_ops_get_raw_soc(enum fg_ic_role ic_role, int *raw_soc)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_raw_soc)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_raw_soc(p->data, raw_soc);
}
EXPORT_SYMBOL(platform_fg_ops_get_raw_soc);

void platform_fg_ops_update_fw(enum fg_ic_role ic_role)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_update_fw)
		return;

	p->ops->fg_ic_update_fw(p->data);
}
EXPORT_SYMBOL(platform_fg_ops_update_fw);

int platform_fg_ops_get_device_name(enum fg_ic_role ic_role,
				    const char **device_name)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_device_name)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_device_name(p->data, device_name);
}
EXPORT_SYMBOL(platform_fg_ops_get_device_name);

int platform_fg_ops_get_temp_min(enum fg_ic_role ic_role, int *temp_min)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_temp_min)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_temp_min(p->data, temp_min);
}
EXPORT_SYMBOL(platform_fg_ops_get_temp_min);

void platform_fg_ops_set_force_report_full(enum fg_ic_role ic_role)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_set_force_report_full)
		return;

	p->ops->fg_ic_set_force_report_full(p->data);
}
EXPORT_SYMBOL(platform_fg_ops_set_force_report_full);

int platform_fg_ops_get_fc(enum fg_ic_role ic_role, bool *fc)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_fc)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_fc(p->data, fc);
}
EXPORT_SYMBOL(platform_fg_ops_get_fc);

int platform_fg_ops_set_co(enum fg_ic_role ic_role, bool enable)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_set_co)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_set_co(p->data, enable);
}
EXPORT_SYMBOL(platform_fg_ops_set_co);

int platform_fg_ops_get_calibration_ffc_iterm(enum fg_ic_role ic_role,
					      int *calibration_ffc_iterm)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_calibration_ffc_iterm)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_calibration_ffc_iterm(p->data, calibration_ffc_iterm);
}
EXPORT_SYMBOL(platform_fg_ops_get_calibration_ffc_iterm);

int platform_fg_ops_get_real_supplement_energy(enum fg_ic_role ic_role,
					       int *supplement_energy)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_real_supplement_energy)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_real_supplement_energy(p->data, supplement_energy);
}
EXPORT_SYMBOL(platform_fg_ops_get_real_supplement_energy);

int platform_fg_ops_get_calibration_charge_energy(enum fg_ic_role ic_role,
						  int *charge_energy)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_calibration_charge_energy)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_calibration_charge_energy(p->data, charge_energy);
}
EXPORT_SYMBOL(platform_fg_ops_get_calibration_charge_energy);

void platform_fg_ops_fl4p0_enable_check(enum fg_ic_role ic_role)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_fl4p0_enable_check)
		return;

	p->ops->fg_ic_fl4p0_enable_check(p->data);
}
EXPORT_SYMBOL(platform_fg_ops_fl4p0_enable_check);

void platform_fg_ops_get_ui_soh(enum fg_ic_role ic_role, int *ui_soh)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_ui_soh)
		return;

	p->ops->fg_ic_get_ui_soh(p->data, ui_soh);
}
EXPORT_SYMBOL(platform_fg_ops_get_ui_soh);

void platform_fg_ops_qbg_send_chg_data(enum fg_ic_role ic_role, int chg_status,
				       int ibat_max)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_qbg_send_chg_data)
		return;

	p->ops->fg_ic_qbg_send_chg_data(p->data, chg_status, ibat_max);
}
EXPORT_SYMBOL(platform_fg_ops_qbg_send_chg_data);

void platform_fg_ops_get_pack_vendor(enum fg_ic_role ic_role, int *pack_vendor)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_pack_vendor)
		return;

	p->ops->fg_ic_get_pack_vendor(p->data, pack_vendor);
}
EXPORT_SYMBOL(platform_fg_ops_get_pack_vendor);

unsigned long platform_fg_ops_get_calc_rvalue(enum fg_ic_role ic_role)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_calc_rvalue)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_calc_rvalue(p->data);
}
EXPORT_SYMBOL(platform_fg_ops_get_calc_rvalue);

void platform_fg_ops_get_ota_update_flag(enum fg_ic_role ic_role, u32 *flag)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_ota_update_flag)
		return;

	p->ops->fg_ic_get_ota_update_flag(p->data, flag);
}
EXPORT_SYMBOL(platform_fg_ops_get_ota_update_flag);

void platform_fg_ops_ota_update_check(enum fg_ic_role ic_role)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_ota_update_check)
		return;

	p->ops->fg_ic_ota_update_check(p->data);
}
EXPORT_SYMBOL(platform_fg_ops_ota_update_check);

void platform_fg_ops_get_average_current(enum fg_ic_role ic_role,
					 int *average_current)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_average_current)
		return;

	p->ops->fg_ic_get_average_current(p->data, average_current);
}
EXPORT_SYMBOL(platform_fg_ops_get_average_current);

void platform_fg_ops_get_batt_abnormal_info(enum fg_ic_role ic_role,
					    void *info)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_batt_abnormal_info)
		return;

	p->ops->fg_ic_get_batt_abnormal_info(p->data, info);
}
EXPORT_SYMBOL(platform_fg_ops_get_batt_abnormal_info);

int platform_fg_ops_get_manufacturing_date(enum fg_ic_role ic_role,
					   u8 *manufacturing_date)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_manufacturing_date)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_manufacturing_date(p->data, manufacturing_date);
}
EXPORT_SYMBOL(platform_fg_ops_get_manufacturing_date);

void platform_fg_ops_set_first_usage_date(enum fg_ic_role ic_role,
					  const char *date)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_set_first_usage_date)
		return;

	p->ops->fg_ic_set_first_usage_date(p->data, date);
}
EXPORT_SYMBOL(platform_fg_ops_set_first_usage_date);

int platform_fg_ops_get_first_usage_date(enum fg_ic_role ic_role,
					 u8 *first_usage_date)
{
	struct fuelguage_info *p = fg_ic_lookup(ic_role);

	if (!p || !p->ops->fg_ic_get_first_usage_date)
		return -EOPNOTSUPP;

	return p->ops->fg_ic_get_first_usage_date(p->data, first_usage_date);
}
EXPORT_SYMBOL(platform_fg_ops_get_first_usage_date);
/*
 * The registry above is reached by exported symbol rather than through a
 * device, so this driver matches nothing and has no probe; registering it
 * only leaves an entry under /sys/bus/platform/drivers.
 */
static struct platform_driver platform_fg_ops_driver = {
	.driver = {
		.name = "platform_fg_ops",
	},
};
module_platform_driver(platform_fg_ops_driver);

MODULE_DESCRIPTION("platform fg ops");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
