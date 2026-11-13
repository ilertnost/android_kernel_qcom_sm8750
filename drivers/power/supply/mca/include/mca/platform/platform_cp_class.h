/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Charge pumps.
 *
 * Fast charging works by handing the battery a high voltage at low current
 * and halving or quartering it close to the cell, so that the loss is in the
 * cable rather than in the phone.  The chip that does the dividing is a
 * charge pump, and a phone that charges very fast has more than one of them
 * working in parallel.
 *
 * Every call names which pump it is about.  The pumps are otherwise identical
 * to the strategy above them, which asks all of them the same questions and
 * adds up the answers.
 */

#ifndef __MCA_PLATFORM_CP_H
#define __MCA_PLATFORM_CP_H

#include <linux/types.h>

/* Which charge pump. */
enum platform_class_cp_role {
	CP_ROLE_MASTER,
	CP_ROLE_SLAVE,
	CP_ROLE_THIRD,
	CP_ROLE_MAX,
};

/*
 * How a pump divides its input.  The numbering is the hardware's, and the
 * reverse modes are the same ratios run the other way, for feeding a device
 * from this phone.
 */
enum platform_class_cp_mode {
	CP_MODE_FORWARD_4_1,
	CP_MODE_FORWARD_2_1,
	CP_MODE_FORWARD_1_1,
	CP_MODE_RESERVED_3,
	CP_MODE_REVERSE_1_4,
	CP_MODE_REVERSE_1_2,
	CP_MODE_REVERSE_1_1,
	CP_MODE_RESERVED_7,
	CP_MODE_MAX,
};

/*
 * Where the flying capacitor is sitting relative to where it should.  Either
 * side of correct means the pump is not switching cleanly, and which side
 * says whether the input or the output is at fault.
 */
/*
 * The interrupt lines a pump can raise, as they are asked about one at a
 * time.  Whether a voltage is present is a level rather than an event, so
 * the same numbering serves for reading the current state.
 */
enum platform_class_cp_sts {
	VOUT_OK_REV_STAT,
	VOUT_OK_CHG_STAT,
	VOUT_INSERT_STAT,
	VBUS_PRESENT_STAT,
	VWPC_PRESENT_STAT,
	VUSB_PRESENT_STAT,
};

enum platform_class_cp_pmid_error_stat {
	CP_PMID_ERROR_OK,
	CP_PMID_ERROR_LOW,
	CP_PMID_ERROR_HIGH,
};

/*
 * What a gate is being opened for.  A pump's over-voltage gate is shared
 * between these, so a caller says which one it is holding it open for and the
 * pump keeps it open while any of them still wants it.
 */
enum enable_ovpgate_type {
	OTG_TYPE,
	REVCHG_TYPE,
	WLS_CHG,
	OVPGATE_TYPE_MAX,
};

/* The charge pumps this stack knows how to drive. */
enum cp_vendor {
	SC8541_VENDOR,
	SC8561_VENDOR,
	SC8581_VENDOR,
	SC8585_VENDOR,
	BQ25960_VENDOR,
	CP_VENDOR_MAX,
};

/* What userspace can read about one pump. */
enum cp_attr_list {
	CP_PROP_CHIP_OK,
	CP_PROP_VBUS,
	CP_PROP_VUSB,
	CP_PROP_IBUS,
	CP_PROP_BATT_PRESENT,
	CP_PROP_BATT_TEMP,
	CP_PROP_VPACK,
	CP_PROP_OVPGATE,
	CP_PROP_FSW,
	CP_PROP_TDIE,
};

/* What userspace can read about the pumps together. */
enum platform_cp_attr_list {
	CP_PROP_IBUS_DELTA,
	CP_RORP_IBUS_TOTAL,
	CP_PROP_WORK_MODE,
};

/**
 * struct platform_class_cp_ops - what a charge pump driver provides
 *
 * Every call takes the @data the pump driver registered as its last argument.
 * An entry left NULL means the pump cannot do that, and the caller is told so
 * rather than being given a wrong answer.  The voltages and currents a pump
 * measures are unsigned; the class hands them on as int.
 */
struct platform_class_cp_ops {
	int (*cp_set_enable)(bool enable, void *data);
	int (*cp_get_enabled)(bool *enabled, void *data);
	int (*cp_set_present)(bool present, void *data);
	int (*cp_get_present)(bool *present, void *data);
	int (*cp_get_vbus_present)(bool *present, void *data);
	int (*cp_get_battery_voltage)(u32 *vbat, void *data);
	int (*cp_get_battery_current)(u32 *ibat, void *data);
	int (*cp_get_battery_temperature)(u32 *tbat, void *data);
	int (*cp_get_battery_present)(bool *present, void *data);
	int (*cp_get_bus_voltage)(u32 *vbus, void *data);
	int (*cp_get_bus_current)(u32 *ibus, void *data);
	int (*cp_get_bus_temperature)(int *tbus, void *data);
	int (*cp_get_die_temperature)(int *tdie, void *data);
	int (*cp_get_alarm_status)(int *alarm_status, void *data);
	int (*cp_get_fault_status)(int *fault_status, void *data);
	int (*cp_get_bus_error_status)(int *bus_error_status, void *data);
	int (*cp_get_reg_status)(int *reg_status, void *data);
	int (*cp_enable_wpcgate)(bool enable, void *data);
	int (*cp_enable_ovpgate)(bool enable, void *data);
	int (*cp_enable_ovpgate_with_check)(int type_temp, bool enable,
					    void *data);
	int (*cp_get_ovpgate_status)(bool *status, void *data);
	int (*cp_get_usb_voltage)(u32 *vusb, void *data);
	int (*cp_set_busovp)(int busovp, void *data);
	int (*cp_set_mode)(int mode, void *data);
	int (*cp_get_mode)(int *mode, void *data);
	int (*cp_device_init)(int device_init, void *data);
	int (*cp_enable_adc)(bool enable, void *data);
	int (*cp_get_bypass_support)(bool *support, void *data);
	int (*cp_dump_register)(void *data);
	int (*cp_get_chip_vendor)(int *chip_vendor, void *data);
	int (*cp_enable_acdrv_manual)(bool enable, void *data);
	int (*cp_set_adjustadble_timeout)(int timeout, void *data);
	int (*cp_get_int_stat)(int stat, bool *val, void *data);
	int (*cp_get_errorhl_stat)(int *stat, void *data);
	int (*cp_enable_busucp)(bool enable, void *data);
	int (*cp_set_fsw)(int fsw, void *data);
	int (*cp_set_default_fsw)(void *data);
	int (*cp_get_fsw)(int *fsw, void *data);
	int (*cp_get_fsw_step)(int *fsw_step, void *data);
	int (*cp_get_tdie)(int *tdie, void *data);
	int (*cp_set_qb)(bool enable, void *data);
	int (*cp_set_pmid2outuvp_th)(int th, void *data);
	int (*cp_set_rcp)(bool enable, void *data);
	int (*cp_set_revchg)(bool enable, void *data);
	int (*cp_get_probe_status)(void *data);
	int (*cp_set_pmid2out_uvp_dis)(bool dis, void *data);
	int (*cp_get_pmid2out_uvp_dis)(bool dis, void *data);
	int (*cp_set_manual_revchg_mode)(bool enable, void *data);
	int (*cp_set_cp_reverse_mode)(bool enable, void *data);
};

int platform_class_cp_register_ops(enum platform_class_cp_role role,
				   const struct platform_class_cp_ops *ops,
				   void *data);

/*
 * How evenly the master and slave pumps are sharing, and how much they are
 * drawing between them.  A pump that cannot be read counts as drawing
 * nothing; the value is always written, and the return is that of the slave
 * pump's reading.
 */
int platform_class_cp_get_ibus_delta(int *ibus_delta);
int platform_class_cp_get_ibus_total(int *ibus_total);

int platform_class_cp_device_init(enum platform_class_cp_role role,
				  int device_init);
int platform_class_cp_dump_register(enum platform_class_cp_role role);
int platform_class_cp_enable_acdrv_manual(enum platform_class_cp_role role,
					  bool enable);
int platform_class_cp_enable_adc(enum platform_class_cp_role role, bool enable);
int platform_class_cp_enable_busucp(enum platform_class_cp_role role,
				    bool enable);
int platform_class_cp_enable_ovpgate(enum platform_class_cp_role role,
				     bool enable);
int platform_class_cp_enable_ovpgate_with_check(
						enum platform_class_cp_role role,
						int type_temp, bool enable);
int platform_class_cp_enable_wpcgate(enum platform_class_cp_role role,
				     bool enable);
int platform_class_cp_get_alarm_status(enum platform_class_cp_role role,
				       int *alarm_status);
int platform_class_cp_get_battery_current(enum platform_class_cp_role role,
					  int *ibat);
int platform_class_cp_get_battery_present(enum platform_class_cp_role role,
					  bool *battery_present);
int platform_class_cp_get_battery_temperature(enum platform_class_cp_role role,
					      int *tbat);
int platform_class_cp_get_battery_voltage(enum platform_class_cp_role role,
					  int *vbat);
int platform_class_cp_get_bus_current(enum platform_class_cp_role role,
				      int *ibus);
int platform_class_cp_get_bus_error_status(enum platform_class_cp_role role,
					   int *bus_error_status);
int platform_class_cp_get_bus_temperature(enum platform_class_cp_role role,
					  int *tbus);
int platform_class_cp_get_bus_voltage(enum platform_class_cp_role role,
				      int *bus_voltage);
int platform_class_cp_get_bypass_support(enum platform_class_cp_role role,
					 bool *bypass_support);
int platform_class_cp_get_charging_enabled(enum platform_class_cp_role role,
					   bool *charging_enabled);
int platform_class_cp_get_chip_vendor(enum platform_class_cp_role role,
				      int *chip_vendor);
int platform_class_cp_get_die_temperature(enum platform_class_cp_role role,
					  int *tdie);
int platform_class_cp_get_errorhl_stat(enum platform_class_cp_role role,
				       int *errorhl_stat);
int platform_class_cp_get_fault_status(enum platform_class_cp_role role,
				       int *fault_status);
int platform_class_cp_get_fsw(enum platform_class_cp_role role, int *fsw);
int platform_class_cp_get_fsw_step(enum platform_class_cp_role role,
				   int *fsw_step);
int platform_class_cp_get_int_stat(enum platform_class_cp_role role,
				   int stat, bool *val);
int platform_class_cp_get_mode(enum platform_class_cp_role role, int *mode);
int platform_class_cp_get_ovpgate_status(enum platform_class_cp_role role,
					 bool *ovpgate_status);
int platform_class_cp_get_pmid2out_uvp_dis(enum platform_class_cp_role role,
					   bool enable);
int platform_class_cp_get_present(enum platform_class_cp_role role,
				  bool *present);
int platform_class_cp_get_probe_ok(enum platform_class_cp_role role);
int platform_class_cp_get_reg_status(enum platform_class_cp_role role,
				     int *reg_status);
int platform_class_cp_get_tdie(enum platform_class_cp_role role, int *tdie);
int platform_class_cp_get_usb_voltage(enum platform_class_cp_role role,
				      int *vusb);
int platform_class_cp_get_vbus_present(enum platform_class_cp_role role,
				       bool *vbus_present);
int platform_class_cp_set_adjustadble_timeout(enum platform_class_cp_role role,
					      int adjustadble_timeout);
int platform_class_cp_set_busovp(enum platform_class_cp_role role, int busovp);
int platform_class_cp_set_charging_enable(enum platform_class_cp_role role,
					  bool enable);
int platform_class_cp_set_cp_reverse_mode(enum platform_class_cp_role role,
					  bool enable);
int platform_class_cp_set_default_fsw(enum platform_class_cp_role role);
int platform_class_cp_set_fsw(enum platform_class_cp_role role, int fsw);
int platform_class_cp_set_manual_revchg_mode(enum platform_class_cp_role role,
					     bool enable);
int platform_class_cp_set_mode(enum platform_class_cp_role role, int mode);
int platform_class_cp_set_pmid2out_uvp_dis(enum platform_class_cp_role role,
					   bool enable);
int platform_class_cp_set_pmid2outuvp_th(enum platform_class_cp_role role,
					 int pmid2outuvp_th);
int platform_class_cp_set_present(enum platform_class_cp_role role,
				  bool enable);
int platform_class_cp_set_qb(enum platform_class_cp_role role, bool enable);
int platform_class_cp_set_rcp(enum platform_class_cp_role role, bool enable);
int platform_class_cp_set_revchg(enum platform_class_cp_role role, bool enable);

#endif /* __MCA_PLATFORM_CP_H */
