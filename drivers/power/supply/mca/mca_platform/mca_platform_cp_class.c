// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Charge pumps.  See include/mca/platform/platform_cp_class.h.
 */

#define MCA_LOG_TAG "cp_class"

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/platform/platform_cp_class.h>
#include <mca/common/mca_sysfs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

/**
 * struct platform_cp_class_ops_data - one registered charge pump
 * @ops:  what the pump driver provides
 * @data: handed back to every call
 */
struct platform_cp_class_ops_data {
	const struct platform_class_cp_ops	*ops;
	void					*data;
};

/**
 * struct platform_cp_dev - the pumps this board has
 * @dev:          the class device
 * @cp_num:       how many pumps
 * @cp_dir_list:  what each one's sysfs directory is called
 * @sysfs_dev:    the device each directory hangs off
 * @cp_dev_index: which role each directory describes
 * @force_fsw:    a switching frequency forced from userspace, 0 for none
 */
struct platform_cp_dev {
	struct device	*dev;
	int		cp_num;
	const char	*cp_dir_list[CP_ROLE_MAX];
	struct device	*sysfs_dev[CP_ROLE_MAX];
	int		cp_dev_index[CP_ROLE_MAX];
	int		force_fsw;
};

static struct platform_cp_class_ops_data g_platform_cp_data[CP_ROLE_MAX];
static struct platform_cp_dev *g_cp_dev;

int platform_class_cp_register_ops(enum platform_class_cp_role role,
				   const struct platform_class_cp_ops *ops,
				   void *data)
{
	if (role >= CP_ROLE_MAX || !ops || !data)
		return -EOPNOTSUPP;

	g_platform_cp_data[role].ops = ops;
	g_platform_cp_data[role].data = data;

	return 0;
}
EXPORT_SYMBOL(platform_class_cp_register_ops);

/*
 * Look up the driver for one pump.  A pump that has not probed gives NULL,
 * and the caller reports -1: a strategy that is told a pump drew no
 * current would act on it, where being told it cannot say is harmless.
 *
 * The value is -1 rather than an errno: callers propagate it unchanged,
 * and some of it reaches userspace through sysfs.
 */
static struct platform_cp_class_ops_data *
platform_class_cp_lookup(enum platform_class_cp_role role)
{
	if (role >= CP_ROLE_MAX)
		return NULL;

	if (!g_platform_cp_data[role].ops)
		return NULL;

	return &g_platform_cp_data[role];
}

int platform_class_cp_device_init(enum platform_class_cp_role role,
				  int device_init)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_device_init)
		return -1;

	return p->ops->cp_device_init(device_init, p->data);
}
EXPORT_SYMBOL(platform_class_cp_device_init);

int platform_class_cp_dump_register(enum platform_class_cp_role role)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_dump_register)
		return -1;

	return p->ops->cp_dump_register(p->data);
}
EXPORT_SYMBOL(platform_class_cp_dump_register);

int platform_class_cp_enable_acdrv_manual(enum platform_class_cp_role role,
					  bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_enable_acdrv_manual)
		return -1;

	return p->ops->cp_enable_acdrv_manual(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_enable_acdrv_manual);

int platform_class_cp_enable_adc(enum platform_class_cp_role role, bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_enable_adc)
		return -1;

	return p->ops->cp_enable_adc(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_enable_adc);

int platform_class_cp_enable_busucp(enum platform_class_cp_role role,
				    bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_enable_busucp)
		return -1;

	return p->ops->cp_enable_busucp(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_enable_busucp);

int platform_class_cp_enable_ovpgate(enum platform_class_cp_role role,
				     bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_enable_ovpgate)
		return -1;

	return p->ops->cp_enable_ovpgate(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_enable_ovpgate);

int platform_class_cp_enable_ovpgate_with_check(
						enum platform_class_cp_role role,
						int type_temp, bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_enable_ovpgate_with_check)
		return -1;

	return p->ops->cp_enable_ovpgate_with_check(type_temp, enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_enable_ovpgate_with_check);

int platform_class_cp_enable_wpcgate(enum platform_class_cp_role role,
				     bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_enable_wpcgate)
		return -1;

	return p->ops->cp_enable_wpcgate(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_enable_wpcgate);

int platform_class_cp_get_alarm_status(enum platform_class_cp_role role,
				       int *alarm_status)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_alarm_status)
		return -1;

	return p->ops->cp_get_alarm_status(alarm_status, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_alarm_status);

int platform_class_cp_get_battery_current(enum platform_class_cp_role role,
					  int *ibat)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_battery_current)
		return -1;

	return p->ops->cp_get_battery_current((u32 *)ibat, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_battery_current);

int platform_class_cp_get_battery_present(enum platform_class_cp_role role,
					  bool *battery_present)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_battery_present)
		return -1;

	return p->ops->cp_get_battery_present(battery_present, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_battery_present);

int platform_class_cp_get_battery_temperature(enum platform_class_cp_role role,
					      int *tbat)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_battery_temperature)
		return -1;

	return p->ops->cp_get_battery_temperature((u32 *)tbat, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_battery_temperature);

int platform_class_cp_get_battery_voltage(enum platform_class_cp_role role,
					  int *vbat)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_battery_voltage)
		return -1;

	return p->ops->cp_get_battery_voltage((u32 *)vbat, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_battery_voltage);

int platform_class_cp_get_bus_current(enum platform_class_cp_role role,
				      int *ibus)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_bus_current)
		return -1;

	return p->ops->cp_get_bus_current((u32 *)ibus, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_bus_current);

int platform_class_cp_get_bus_error_status(enum platform_class_cp_role role,
					   int *bus_error_status)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_bus_error_status)
		return -1;

	return p->ops->cp_get_bus_error_status(bus_error_status, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_bus_error_status);

int platform_class_cp_get_bus_temperature(enum platform_class_cp_role role,
					  int *tbus)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_bus_temperature)
		return -1;

	return p->ops->cp_get_bus_temperature(tbus, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_bus_temperature);

int platform_class_cp_get_bus_voltage(enum platform_class_cp_role role,
				      int *bus_voltage)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_bus_voltage)
		return -1;

	return p->ops->cp_get_bus_voltage((u32 *)bus_voltage, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_bus_voltage);

int platform_class_cp_get_bypass_support(enum platform_class_cp_role role,
					 bool *bypass_support)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_bypass_support)
		return -1;

	return p->ops->cp_get_bypass_support(bypass_support, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_bypass_support);

int platform_class_cp_get_charging_enabled(enum platform_class_cp_role role,
					   bool *charging_enabled)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_enabled)
		return -1;

	return p->ops->cp_get_enabled(charging_enabled, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_charging_enabled);

int platform_class_cp_get_chip_vendor(enum platform_class_cp_role role,
				      int *chip_vendor)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_chip_vendor)
		return -1;

	return p->ops->cp_get_chip_vendor(chip_vendor, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_chip_vendor);

int platform_class_cp_get_die_temperature(enum platform_class_cp_role role,
					  int *tdie)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_die_temperature)
		return -1;

	return p->ops->cp_get_die_temperature(tdie, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_die_temperature);

int platform_class_cp_get_errorhl_stat(enum platform_class_cp_role role,
				       int *errorhl_stat)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_errorhl_stat)
		return -1;

	return p->ops->cp_get_errorhl_stat(errorhl_stat, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_errorhl_stat);

int platform_class_cp_get_fault_status(enum platform_class_cp_role role,
				       int *fault_status)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_fault_status)
		return -1;

	return p->ops->cp_get_fault_status(fault_status, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_fault_status);

int platform_class_cp_get_fsw(enum platform_class_cp_role role, int *fsw)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_fsw)
		return -1;

	return p->ops->cp_get_fsw(fsw, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_fsw);

int platform_class_cp_get_fsw_step(enum platform_class_cp_role role,
				   int *fsw_step)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_fsw_step)
		return -1;

	return p->ops->cp_get_fsw_step(fsw_step, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_fsw_step);

int platform_class_cp_get_int_stat(enum platform_class_cp_role role,
				   int stat, bool *val)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_int_stat)
		return -1;

	return p->ops->cp_get_int_stat(stat, val, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_int_stat);

int platform_class_cp_get_mode(enum platform_class_cp_role role, int *mode)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_mode)
		return -1;

	return p->ops->cp_get_mode(mode, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_mode);

int platform_class_cp_get_ovpgate_status(enum platform_class_cp_role role,
					 bool *ovpgate_status)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_ovpgate_status)
		return -1;

	return p->ops->cp_get_ovpgate_status(ovpgate_status, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_ovpgate_status);

int platform_class_cp_get_pmid2out_uvp_dis(enum platform_class_cp_role role,
					   bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_pmid2out_uvp_dis)
		return -1;

	return p->ops->cp_get_pmid2out_uvp_dis(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_pmid2out_uvp_dis);

int platform_class_cp_get_present(enum platform_class_cp_role role,
				  bool *present)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_present)
		return -1;

	return p->ops->cp_get_present(present, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_present);

int platform_class_cp_get_probe_ok(enum platform_class_cp_role role)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_probe_status)
		return -1;

	return p->ops->cp_get_probe_status(p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_probe_ok);

int platform_class_cp_get_reg_status(enum platform_class_cp_role role,
				     int *reg_status)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_reg_status)
		return -1;

	return p->ops->cp_get_reg_status(reg_status, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_reg_status);

int platform_class_cp_get_tdie(enum platform_class_cp_role role, int *tdie)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_tdie)
		return -1;

	return p->ops->cp_get_tdie(tdie, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_tdie);

int platform_class_cp_get_usb_voltage(enum platform_class_cp_role role,
				      int *vusb)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_usb_voltage)
		return -1;

	return p->ops->cp_get_usb_voltage((u32 *)vusb, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_usb_voltage);

int platform_class_cp_get_vbus_present(enum platform_class_cp_role role,
				       bool *vbus_present)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_get_vbus_present)
		return -1;

	return p->ops->cp_get_vbus_present(vbus_present, p->data);
}
EXPORT_SYMBOL(platform_class_cp_get_vbus_present);

int platform_class_cp_set_adjustadble_timeout(enum platform_class_cp_role role,
					      int adjustadble_timeout)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_set_adjustadble_timeout)
		return -1;

	return p->ops->cp_set_adjustadble_timeout(adjustadble_timeout, p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_adjustadble_timeout);

int platform_class_cp_set_busovp(enum platform_class_cp_role role, int busovp)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_set_busovp)
		return -1;

	return p->ops->cp_set_busovp(busovp, p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_busovp);

int platform_class_cp_set_charging_enable(enum platform_class_cp_role role,
					  bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_set_enable)
		return -1;

	return p->ops->cp_set_enable(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_charging_enable);

int platform_class_cp_set_cp_reverse_mode(enum platform_class_cp_role role,
					  bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_set_cp_reverse_mode)
		return -1;

	return p->ops->cp_set_cp_reverse_mode(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_cp_reverse_mode);

int platform_class_cp_set_default_fsw(enum platform_class_cp_role role)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_set_default_fsw)
		return -1;

	return p->ops->cp_set_default_fsw(p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_default_fsw);

/*
 * The switching frequency can be pinned from userspace, so that a
 * measurement is not spoiled by the charging strategy moving it underneath.
 * A pin is armed by writing the wanted frequency as a negative number: the
 * first call through here takes it, makes it positive, and every later call
 * is turned away.
 */
noinline int platform_class_cp_set_fsw(enum platform_class_cp_role role,
				      int fsw)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (g_cp_dev) {
		if (g_cp_dev->force_fsw >= 1) {
			mca_log_info("force_fsw has set: %d\n",
				     g_cp_dev->force_fsw);

			return 0;
		}

		if (g_cp_dev->force_fsw < 0)
			g_cp_dev->force_fsw = -g_cp_dev->force_fsw;
	}

	if (!p || !p->ops->cp_set_fsw)
		return -1;

	return p->ops->cp_set_fsw(fsw, p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_fsw);

int platform_class_cp_set_manual_revchg_mode(enum platform_class_cp_role role,
					     bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_set_manual_revchg_mode)
		return -1;

	return p->ops->cp_set_manual_revchg_mode(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_manual_revchg_mode);

int platform_class_cp_set_mode(enum platform_class_cp_role role, int mode)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_set_mode)
		return -1;

	return p->ops->cp_set_mode(mode, p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_mode);

int platform_class_cp_set_pmid2out_uvp_dis(enum platform_class_cp_role role,
					   bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_set_pmid2out_uvp_dis)
		return -1;

	return p->ops->cp_set_pmid2out_uvp_dis(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_pmid2out_uvp_dis);

int platform_class_cp_set_pmid2outuvp_th(enum platform_class_cp_role role,
					 int pmid2outuvp_th)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_set_pmid2outuvp_th)
		return -1;

	return p->ops->cp_set_pmid2outuvp_th(pmid2outuvp_th, p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_pmid2outuvp_th);

int platform_class_cp_set_present(enum platform_class_cp_role role, bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_set_present)
		return -1;

	return p->ops->cp_set_present(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_present);

int platform_class_cp_set_qb(enum platform_class_cp_role role, bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_set_qb)
		return -1;

	return p->ops->cp_set_qb(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_qb);

int platform_class_cp_set_rcp(enum platform_class_cp_role role, bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_set_rcp)
		return -1;

	return p->ops->cp_set_rcp(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_rcp);

int platform_class_cp_set_revchg(enum platform_class_cp_role role, bool enable)
{
	struct platform_cp_class_ops_data *p = platform_class_cp_lookup(role);

	if (!p || !p->ops->cp_set_revchg)
		return -1;

	return p->ops->cp_set_revchg(enable, p->data);
}
EXPORT_SYMBOL(platform_class_cp_set_revchg);

/*
 * The master and slave pumps work in parallel, so what matters to the
 * strategy is what they draw between them, and how far apart they have
 * drifted: a pair sharing unevenly is a fault, because the one carrying more
 * is the one that will overheat.  A pump that cannot be read counts as
 * drawing nothing, and the answer given back is whether the slave could be
 * read.
 */
static int platform_class_cp_get_ibus_pair(int *master, int *slave)
{
	int ret;

	*master = 0;
	*slave = 0;

	if (platform_class_cp_get_bus_current(CP_ROLE_MASTER, master))
		*master = 0;

	ret = platform_class_cp_get_bus_current(CP_ROLE_SLAVE, slave);
	if (ret)
		*slave = 0;

	return ret;
}

int platform_class_cp_get_ibus_total(int *ibus_total)
{
	int master, slave, ret;

	ret = platform_class_cp_get_ibus_pair(&master, &slave);
	*ibus_total = master + slave;

	return ret;
}
EXPORT_SYMBOL(platform_class_cp_get_ibus_total);

int platform_class_cp_get_ibus_delta(int *ibus_delta)
{
	int master, slave, ret;

	ret = platform_class_cp_get_ibus_pair(&master, &slave);
	*ibus_delta = abs(master - slave);

	return ret;
}
EXPORT_SYMBOL(platform_class_cp_get_ibus_delta);

/* Where a pump's directory appears, and what its name is matched against. */
#define CP_CLASS_NAME		"xm_power"
#define CP_NAME_MASTER		"master"
#define CP_NAME_SLAVE		"slave"
#define CP_NAME_THIRD		"third"

/* Where the figures for the pumps together appear. */
#define CP_TOTAL_DIR_NAME	"chargerpump"

/*
 * What an attribute read reports back.  A single value, but reached through
 * calls that answer in different types.
 */
union platform_cp_propval {
	unsigned int	uintval;
	int		intval;
	char		strval[200];
	bool		boolval;
};

static int probe_cnt;

static ssize_t cp_sysfs_show(struct device *dev, struct device_attribute *attr,
			     char *buf);
static ssize_t cp_sysfs_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count);

static struct mca_sysfs_attr_info cp_sysfs_field_tbl[] = {
	mca_sysfs_attr_ro(cp_sysfs, 0440, CP_PROP_CHIP_OK, chip_ok),
	mca_sysfs_attr_ro(cp_sysfs, 0440, CP_PROP_VBUS, vbus),
	mca_sysfs_attr_ro(cp_sysfs, 0440, CP_PROP_VUSB, vusb),
	mca_sysfs_attr_ro(cp_sysfs, 0440, CP_PROP_IBUS, ibus),
	mca_sysfs_attr_ro(cp_sysfs, 0440, CP_PROP_BATT_PRESENT, batt_present),
	mca_sysfs_attr_ro(cp_sysfs, 0440, CP_PROP_BATT_TEMP, batt_temp),
	mca_sysfs_attr_ro(cp_sysfs, 0440, CP_PROP_VPACK, vpack),
	mca_sysfs_attr_rw(cp_sysfs, 0664, CP_PROP_OVPGATE, ovpgate),
	mca_sysfs_attr_rw(cp_sysfs, 0664, CP_PROP_FSW, fsw),
	mca_sysfs_attr_ro(cp_sysfs, 0440, CP_PROP_TDIE, tdie),
};

static struct attribute *cp_attrs[ARRAY_SIZE(cp_sysfs_field_tbl) + 1];
static const struct attribute_group cp_sysfs_attr_group = {
	.attrs = cp_attrs,
};

static ssize_t platform_cp_sysfs_show(struct device *dev,
				      struct device_attribute *attr, char *buf);
static ssize_t platform_cp_sysfs_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count);

static struct mca_sysfs_attr_info platform_cp_sysfs_field_tbl[] = {
	mca_sysfs_attr_ro(platform_cp_sysfs, 0440, CP_PROP_IBUS_DELTA,
			  ibus_delta),
	mca_sysfs_attr_ro(platform_cp_sysfs, 0440, CP_RORP_IBUS_TOTAL,
			  ibus_total),
	mca_sysfs_attr_rw(platform_cp_sysfs, 0664, CP_PROP_WORK_MODE, cp_mode),
};

static struct attribute *platform_cp_attrs[ARRAY_SIZE(platform_cp_sysfs_field_tbl) + 1];
static const struct attribute_group platform_cp_sysfs_attr_group = {
	.attrs = platform_cp_attrs,
};

/*
 * Each pump reports through a directory of its own, named by the device tree,
 * so that userspace reads the same attribute names under different names
 * rather than the same names with different suffixes.
 */
static ssize_t cp_sysfs_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	union platform_cp_propval val = { 0 };
	struct mca_sysfs_attr_info *info;
	int *role;

	info = mca_sysfs_lookup_attr(attr->attr.name, cp_sysfs_field_tbl,
				     ARRAY_SIZE(cp_sysfs_field_tbl));
	if (!info)
		return -1;

	role = dev_get_drvdata(dev);
	if (!role) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	mca_log_err("%s dev_driverdata is %d\n", __func__, *role);

	switch (info->sysfs_attr_name) {
	case CP_PROP_CHIP_OK:
		platform_class_cp_get_present(*role, &val.boolval);

		return scnprintf(buf, PAGE_SIZE, "%d\n", val.boolval);
	case CP_PROP_VBUS:
		/*
		 * Nothing reads these often enough to leave the converter
		 * running, so it is turned on for the reading and the pump
		 * puts it back to sleep on its own.
		 */
		platform_class_cp_enable_adc(*role, true);
		platform_class_cp_get_bus_voltage(*role, &val.intval);
		break;
	case CP_PROP_VUSB:
		platform_class_cp_enable_adc(*role, true);
		platform_class_cp_get_usb_voltage(*role, &val.intval);
		break;
	case CP_PROP_IBUS:
		platform_class_cp_enable_adc(*role, true);
		platform_class_cp_get_bus_current(*role, &val.intval);
		break;
	case CP_PROP_BATT_PRESENT:
		platform_class_cp_get_battery_present(*role, &val.boolval);

		return scnprintf(buf, PAGE_SIZE, "%d\n", val.boolval);
	case CP_PROP_BATT_TEMP:
		platform_class_cp_get_battery_temperature(*role, &val.intval);
		break;
	case CP_PROP_VPACK:
		platform_class_cp_enable_adc(*role, true);
		platform_class_cp_get_battery_voltage(*role, &val.intval);
		break;
	case CP_PROP_OVPGATE:
		platform_class_cp_get_ovpgate_status(*role, &val.boolval);

		return scnprintf(buf, PAGE_SIZE, "%d\n", val.boolval);
	case CP_PROP_FSW:
		platform_class_cp_get_fsw(*role, &val.intval);
		break;
	case CP_PROP_TDIE:
		platform_class_cp_get_tdie(*role, &val.intval);
		break;
	default:
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", val.intval);
}

static ssize_t cp_sysfs_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct mca_sysfs_attr_info *info;
	int *role;
	int val = 0;

	info = mca_sysfs_lookup_attr(attr->attr.name, cp_sysfs_field_tbl,
				     ARRAY_SIZE(cp_sysfs_field_tbl));
	if (!info)
		return -1;

	role = dev_get_drvdata(dev);
	if (!role) {
		mca_log_err("dev_driverdata is null\n");
		return -1;
	}

	mca_log_err("dev_driverdata is: %d, attr: %d, buf: %s\n", *role,
		    info->sysfs_attr_name, buf);

	switch (info->sysfs_attr_name) {
	case CP_PROP_OVPGATE:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;

		platform_class_cp_enable_ovpgate(*role, !!val);
		break;
	case CP_PROP_FSW:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;

		if (!g_cp_dev || val < 0)
			break;

		/*
		 * Armed rather than pinned: the call below is let through and
		 * pins it on the way past, so what was asked for is what the
		 * pump ends up running at.
		 */
		g_cp_dev->force_fsw = -val;
		platform_class_cp_set_fsw(*role, val);
		break;
	default:
		break;
	}

	return count;
}

/*
 * What the pumps are doing between them.  Both halves of a pair should be
 * carrying the same current; how far apart they are is what says one of them
 * is in trouble, and what they add up to is what the strategy is spending.
 */
static ssize_t platform_cp_sysfs_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct mca_sysfs_attr_info *info;
	int val;

	info = mca_sysfs_lookup_attr(attr->attr.name,
				     platform_cp_sysfs_field_tbl,
				     ARRAY_SIZE(platform_cp_sysfs_field_tbl));
	if (!info)
		return -1;

	switch (info->sysfs_attr_name) {
	case CP_PROP_IBUS_DELTA:
		platform_class_cp_get_ibus_delta(&val);
		break;
	case CP_RORP_IBUS_TOTAL:
		platform_class_cp_get_ibus_total(&val);
		break;
	default:
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

/* cp_mode takes no writes; the write is answered with nothing consumed. */
static ssize_t platform_cp_sysfs_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	return 0;
}

/*
 * Which pump a directory belongs to is taken from the name it was given
 * rather than from its position, so a board that lists them in another order
 * still reaches the right one.
 */
static void cp_sysfs_create_group(struct platform_cp_dev *chip)
{
	const char *name;
	int role;
	int i;

	for (i = 0; i < chip->cp_num && i < CP_ROLE_MAX; i++) {
		chip->sysfs_dev[i] = mca_sysfs_create_group(CP_CLASS_NAME,
							    chip->cp_dir_list[i],
							    &cp_sysfs_attr_group);
		if (!chip->sysfs_dev[i])
			mca_log_err("creat cp[%d] sysfs fail\n", i);
	}

	for (i = 0; i < chip->cp_num && i < CP_ROLE_MAX; i++) {
		name = dev_name(chip->sysfs_dev[i]);

		if (strstr(name, CP_NAME_MASTER))
			role = CP_ROLE_MASTER;
		else if (strstr(name, CP_NAME_SLAVE))
			role = CP_ROLE_SLAVE;
		else if (strstr(name, CP_NAME_THIRD))
			role = CP_ROLE_THIRD;
		else
			continue;

		chip->cp_dev_index[i] = role;
		dev_set_drvdata(chip->sysfs_dev[i], &chip->cp_dev_index[i]);
		mca_log_err("success match cp_dev_name = %s, cp_dev_list[%d]=%s\n",
			    name, role, chip->cp_dir_list[i]);
	}
}

static int platform_cp_dev_parse_dt(struct platform_cp_dev *chip,
				    struct device_node *np)
{
	int count;
	int rc;
	int i;

	if (!np) {
		mca_log_err("device tree info missing\n");
		return -1;
	}

	rc = mca_parse_dts_u32(np, "cp-num", &chip->cp_num, 1);
	if (rc) {
		mca_log_err("get cp-num fail\n");
		return rc;
	}

	count = mca_parse_dts_count_strings(np, "cp-dir-list", CP_ROLE_MAX, 1);
	mca_log_err("cp dir list max count: %d, %d\n", count, chip->cp_num);
	if (count != chip->cp_num)
		mca_log_err("cp_num can't match cp_dir_list count\n");

	for (i = 0; i < count; i++) {
		rc = mca_parse_dts_string_index(np, "cp-dir-list", i,
						&chip->cp_dir_list[i]);
		if (rc < 0) {
			mca_log_err("Unable to read cp-dir-list strings[%d]\n",
				    i);
			return rc;
		}
	}

	mca_log_info("%s success\n", __func__);

	return 0;
}

static int platform_cp_class_probe(struct platform_device *pdev)
{
	struct platform_cp_dev *chip;
	int rc;

	mca_log_err("%s begin cnt %d\n", __func__, probe_cnt++);

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	platform_set_drvdata(pdev, chip);

	rc = platform_cp_dev_parse_dt(chip, pdev->dev.of_node);
	if (rc) {
		mca_log_err("%s Couldn't parse device tree rc=%d\n", __func__,
			    rc);
		return rc;
	}

	g_cp_dev = chip;

	mca_sysfs_init_attrs(cp_attrs, cp_sysfs_field_tbl,
			     ARRAY_SIZE(cp_sysfs_field_tbl));
	cp_sysfs_create_group(chip);

	mca_sysfs_init_attrs(platform_cp_attrs, platform_cp_sysfs_field_tbl,
			     ARRAY_SIZE(platform_cp_sysfs_field_tbl));
	mca_sysfs_create_link_group(MCA_SYSFS_DEV_CHARGER, CP_TOTAL_DIR_NAME,
				    chip->dev, &platform_cp_sysfs_attr_group);

	mca_log_err("%s success %d\n", __func__, probe_cnt++);

	return 0;
}

static int platform_cp_class_remove(struct platform_device *pdev)
{
	return 0;
}

static void platform_cp_class_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,platform_cp" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver platform_cp_class_driver = {
	.driver = {
		.name		= "platform_cp_class",
		.of_match_table	= match_table,
	},
	.probe		= platform_cp_class_probe,
	.remove		= platform_cp_class_remove,
	.shutdown	= platform_cp_class_shutdown,
};
module_platform_driver(platform_cp_class_driver);

MODULE_DESCRIPTION("platform cp class");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
