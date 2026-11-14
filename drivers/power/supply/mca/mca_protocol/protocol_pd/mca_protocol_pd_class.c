// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * USB Power Delivery adapters.  See
 * include/mca/protocol/protocol_pd_class.h.
 */

#define MCA_LOG_TAG "protocol_pd_class"

#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <mca/shared_memory/charger_partition_class.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/protocol/protocol_class.h>
#include <mca/protocol/protocol_pd_class.h>
#include <mca/common/mca_sysfs.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/**
 * struct protocol_class_pd_ops_data - one registered Type-C port driver
 * @data: handed back to every call
 * @ops:  what the port driver provides
 */
struct protocol_class_pd_ops_data {
	void					*data;
	const struct protocol_class_pd_ops	*ops;
};

static struct protocol_class_pd_ops_data g_protocol_pd_data[TYPEC_PORT_MAX];

/*
 * Which port a charger is on.  A board with two Type-C ports charges from
 * whichever one an adapter was last seen on, and everything above here asks
 * about "the port" without caring which that is.
 */
static u32 g_active_port;

/* The most this phone will draw, whatever the adapter offers. */
static u32 g_device_max_power;

/*
 * Which charging arrangement the board has.  It changes which of an adapter's
 * operating points can actually be used: the two charge pump path cannot take
 * the highest voltages a verified adapter offers.
 */
static u32 g_support_mode;

#define PD_SUPPORT_MODE_DUAL_CP		2

/* Above this a verified adapter's operating point is out of reach, in mV. */
#define PD_CP_VOLT_MAX_MV		11001

int protocol_class_pd_register_ops(enum typec_port_num port,
				   const struct protocol_class_pd_ops *ops,
				   void *data)
{
	if (port >= TYPEC_PORT_MAX || !ops)
		return -1;

	g_protocol_pd_data[port].ops = ops;
	g_protocol_pd_data[port].data = data;

	return 0;
}
EXPORT_SYMBOL(protocol_class_pd_register_ops);

/**
 * protocol_class_pd_get_port_num() - which port a charger is on
 *
 * Despite the name this is the port in use, not a count: a board with a
 * second Type-C port follows whichever one an adapter was last seen on.
 */
int protocol_class_pd_get_port_num(void)
{
	return g_active_port;
}
EXPORT_SYMBOL(protocol_class_pd_get_port_num);

/*
 * Look up the port driver that answers for a port.  A port nobody registered
 * for gives NULL, and the caller reports -1 rather than a made up
 * answer.
 *
 * The value is -1 rather than an errno: that is what the vendor's class
 * layer answers, callers propagate it unchanged, and some of it reaches
 * userspace through sysfs.
 */
static struct protocol_class_pd_ops_data *
protocol_class_pd_lookup(enum typec_port_num port)
{
	if (port >= TYPEC_PORT_MAX)
		return NULL;

	if (!g_protocol_pd_data[port].ops)
		return NULL;

	return &g_protocol_pd_data[port];
}

int protocol_class_pd_get_pps_max_power(enum typec_port_num port, u32 *max_power)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_pps_get_max_power)
		return -1;

	return p->ops->protocol_pd_pps_get_max_power(max_power, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_pps_max_power);

int protocol_class_pd_get_pps_ptf(enum typec_port_num port, int *pps_ptf)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_pps_ptf)
		return -1;

	return p->ops->protocol_pd_get_pps_ptf(pps_ptf, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_pps_ptf);

int protocol_class_pd_set_gear_shift(enum typec_port_num port, int gear_shift)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_gear_shift)
		return -1;

	return p->ops->protocol_pd_set_gear_shift(gear_shift, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_gear_shift);

int protocol_class_pd_set_pps_max_cur(enum typec_port_num port, u32 curr)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_pps_max_cur)
		return -1;

	return p->ops->protocol_pd_set_pps_max_cur(curr, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_pps_max_cur);

int protocol_class_pd_get_pps_max_cur(enum typec_port_num port, u32 *curr)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_pps_max_cur)
		return -1;

	return p->ops->protocol_pd_get_pps_max_cur(curr, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_pps_max_cur);

int protocol_class_pd_set_pd_active(enum typec_port_num port, int pd_active)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_pd_active)
		return -1;

	return p->ops->protocol_pd_set_pd_active(pd_active, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_pd_active);

int protocol_class_pd_get_pd_active(enum typec_port_num port, int *pd_active)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_pd_active)
		return -1;

	return p->ops->protocol_pd_get_pd_active(pd_active, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_pd_active);

int protocol_class_pd_set_pps_min_volt(enum typec_port_num port, u32 volt)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_pps_min_volt)
		return -1;

	return p->ops->protocol_pd_set_pps_min_volt(volt, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_pps_min_volt);

int protocol_class_pd_get_pps_min_volt(enum typec_port_num port, u32 *volt)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_pps_min_volt)
		return -1;

	return p->ops->protocol_pd_get_pps_min_volt(volt, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_pps_min_volt);

int protocol_class_pd_set_pps_max_volt(enum typec_port_num port, u32 volt)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_pps_max_volt)
		return -1;

	return p->ops->protocol_pd_set_pps_max_volt(volt, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_pps_max_volt);

int protocol_class_pd_get_pps_max_volt(enum typec_port_num port, u32 *volt)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_pps_max_volt)
		return -1;

	return p->ops->protocol_pd_get_pps_max_volt(volt, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_pps_max_volt);

int protocol_class_pd_set_pps_apdo_max(enum typec_port_num port, u32 apdo_max)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_pps_apdo_max)
		return -1;

	return p->ops->protocol_pd_set_pps_apdo_max(apdo_max, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_pps_apdo_max);

int protocol_class_pd_get_pps_apdo_max(enum typec_port_num port, u32 *apdo_max)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_pps_apdo_max)
		return -1;

	return p->ops->protocol_pd_get_pps_apdo_max(apdo_max, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_pps_apdo_max);

int protocol_class_pd_get_pd_type(enum typec_port_num port, int *pd_type)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_pd_type)
		return -1;

	return p->ops->protocol_pd_get_pd_type(pd_type, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_pd_type);

int protocol_class_pd_set_typec_mode(enum typec_port_num port, int typec_mode)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_typec_mode)
		return -1;

	return p->ops->protocol_pd_set_typec_mode(typec_mode, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_typec_mode);

int protocol_class_pd_get_typec_mode(enum typec_port_num port, int *typec_mode)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_typec_mode)
		return -1;

	return p->ops->protocol_pd_get_typec_mode(typec_mode, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_typec_mode);

int protocol_class_pd_get_typec_cc_orientation(enum typec_port_num port, int *cc_orientation)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_typec_cc_orientation)
		return -1;

	return p->ops->protocol_pd_get_typec_cc_orientation(cc_orientation, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_typec_cc_orientation);

int protocol_class_pd_set_typec_cc_orientation(enum typec_port_num port, int cc_orientation)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_typec_cc_orientation)
		return -1;

	return p->ops->protocol_pd_set_typec_cc_orientation(cc_orientation, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_typec_cc_orientation);

int protocol_class_pd_set_pd_in_hard_reset(enum typec_port_num port, int in_hard_reset)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_in_hard_reset)
		return -1;

	return p->ops->protocol_pd_set_in_hard_reset(in_hard_reset, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_pd_in_hard_reset);

int protocol_class_pd_get_pd_in_hard_reset(enum typec_port_num port, int *in_hard_reset)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_in_hard_reset)
		return -1;

	return p->ops->protocol_pd_get_in_hard_reset(in_hard_reset, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_pd_in_hard_reset);

int protocol_class_pd_set_usb_suspend_supported(enum typec_port_num port, int supported)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_usb_suspend_supported)
		return -1;

	return p->ops->protocol_pd_set_usb_suspend_supported(supported, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_usb_suspend_supported);

int protocol_class_pd_get_usb_suspend_supported(enum typec_port_num port, int *supported)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_usb_suspend_supported)
		return -1;

	return p->ops->protocol_pd_get_usb_suspend_supported(supported, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_usb_suspend_supported);

int protocol_class_pd_set_pd_typec_accessory_mode(enum typec_port_num port, int typec_accessory_mode)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_pd_typec_accessory_mode)
		return -1;

	return p->ops->protocol_pd_set_pd_typec_accessory_mode(typec_accessory_mode, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_pd_typec_accessory_mode);

int protocol_class_pd_get_pd_typec_accessory_mode(enum typec_port_num port, int *typec_accessory_mode)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_pd_typec_accessory_mode)
		return -1;

	return p->ops->protocol_pd_get_pd_typec_accessory_mode(typec_accessory_mode, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_pd_typec_accessory_mode);

int protocol_class_pd_get_adapter_id(enum typec_port_num port, u32 *adapter_id)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_adapter_id)
		return -1;

	return p->ops->protocol_pd_get_adapter_id(adapter_id, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_adapter_id);

int protocol_class_pd_get_adapter_svid(enum typec_port_num port, u32 *adapter_svid)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_adapter_svid)
		return -1;

	return p->ops->protocol_pd_get_adapter_svid(adapter_svid, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_adapter_svid);

int protocol_class_pd_get_has_dp(enum typec_port_num port, bool *has_dp)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_has_dp)
		return -1;

	return p->ops->protocol_pd_get_has_dp(has_dp, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_has_dp);

int protocol_class_pd_get_power_role(enum typec_port_num port, int *power_role)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_power_role)
		return -1;

	return p->ops->protocol_pd_get_power_role(power_role, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_power_role);

int protocol_class_pd_get_data_role(enum typec_port_num port, int *data_role)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_data_role)
		return -1;

	return p->ops->protocol_pd_get_data_role(data_role, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_data_role);

int protocol_class_pd_set_verify_process(enum typec_port_num port, int verify_process)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_verify_process)
		return -1;

	return p->ops->protocol_pd_set_verify_process(verify_process, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_verify_process);

int protocol_class_pd_get_verify_process(enum typec_port_num port, int *verify_process)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_verify_process)
		return -1;

	return p->ops->protocol_pd_get_verify_process(verify_process, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_verify_process);

int protocol_class_pd_set_pd_verifed(enum typec_port_num port, int pd_verifed)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_pd_verifed)
		return -1;

	return p->ops->protocol_pd_set_pd_verifed(pd_verifed, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_pd_verifed);

int protocol_class_pd_get_pd_verifed(enum typec_port_num port, int *pd_verifed)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_pd_verifed)
		return -1;

	return p->ops->protocol_pd_get_pd_verifed(pd_verifed, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_pd_verifed);

int protocol_class_pd_get_cid_status(enum typec_port_num port, bool *cid_status)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_cid_status)
		return -1;

	return p->ops->protocol_pd_get_cid_status(cid_status, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_cid_status);

int protocol_class_pd_get_otg_plugin_status(enum typec_port_num port, bool *plugin)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_otg_plugin_status)
		return -1;

	return p->ops->protocol_pd_get_otg_plugin_status(plugin, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_otg_plugin_status);

int protocol_class_pd_set_cc_toggle(enum typec_port_num port, bool cc_toggle)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_set_cc_toggle)
		return -1;

	return p->ops->protocol_pd_set_cc_toggle(cc_toggle, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_cc_toggle);

int protocol_class_pd_get_cc_toggle(enum typec_port_num port, bool *cc_toggle)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_cc_toggle)
		return -1;

	return p->ops->protocol_pd_get_cc_toggle(cc_toggle, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_cc_toggle);

int protocol_class_pd_get_snk_src_mode(enum typec_port_num port, int *snk_src_mode)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_snk_src_mode)
		return -1;

	return p->ops->protocol_pd_get_snk_src_mode(snk_src_mode, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_snk_src_mode);

int protocol_class_pd_get_cc_status(enum typec_port_num port, bool *cc_status)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_cc_status)
		return -1;

	return p->ops->protocol_pd_get_cc_status(cc_status, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_cc_status);

int protocol_class_pd_get_cc_short_vbus(enum typec_port_num port, int *cc_short_vbus)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_cc_short_vbus)
		return -1;

	return p->ops->protocol_pd_get_cc_short_vbus(cc_short_vbus, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_cc_short_vbus);

int protocol_class_pd_get_suspend_support_status(enum typec_port_num port, bool *pdsuspendsupported)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_suspend_support_status)
		return -1;

	return p->ops->protocol_pd_get_suspend_support_status(pdsuspendsupported, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_suspend_support_status);

int protocol_class_pd_get_zimi_cypress_flag(enum typec_port_num port, int *zimi_cypress_flag)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_zimi_cypress_flag)
		return -1;

	return p->ops->protocol_pd_get_zimi_cypress_flag(zimi_cypress_flag, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_zimi_cypress_flag);

int protocol_class_pd_set_fixed_volt(enum typec_port_num port, int volt)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_fixed_pdo_set_vol)
		return -1;

	return p->ops->protocol_pd_fixed_pdo_set_vol(volt, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_fixed_volt);

int protocol_class_pd_get_pps_status(enum typec_port_num port, int *volt, int *curr)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_pps_status)
		return -1;

	return p->ops->protocol_pd_get_pps_status(volt, curr, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_pps_status);

int protocol_class_pd_set_pps_pdo_select(enum typec_port_num port, int volt, int curr)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_pps_pdo_select)
		return -1;

	return p->ops->protocol_pd_pps_pdo_select(volt, curr, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_set_pps_pdo_select);

int protocol_class_pd_get_cap(enum typec_port_num port, int cap_type, struct adapter_power_cap *tacap)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_cap)
		return -1;

	return p->ops->protocol_pd_get_cap(cap_type, tacap, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_cap);

int protocol_class_pd_get_vdm_cmd(enum typec_port_num port, int *cmd, struct usbpd_vdm_data *vdm_data)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_vdm_cmd)
		return -1;

	return p->ops->protocol_pd_get_vdm_cmd(cmd, vdm_data, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_vdm_cmd);

int protocol_class_pd_get_current_state(enum typec_port_num port, char *buf, int size)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_current_state)
		return -1;

	return p->ops->protocol_pd_get_current_state(buf, size, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_current_state);

int protocol_class_pd_get_pdos(enum typec_port_num port, struct pd_pdo *pdo, int size)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_get_pdos)
		return -1;

	return p->ops->protocol_pd_get_pdos(pdo, size, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_get_pdos);

int protocol_class_pd_request_vdm_cmd(enum typec_port_num port, enum uvdm_state cmd, u32 *data, u32 size)
{
	struct protocol_class_pd_ops_data *p = protocol_class_pd_lookup(port);

	if (!p || !p->ops->protocol_pd_request_vdm_cmd)
		return -1;

	return p->ops->protocol_pd_request_vdm_cmd(cmd, data, size, p->data);
}
EXPORT_SYMBOL(protocol_class_pd_request_vdm_cmd);

/*
 * Operating points of the adapter on the active port.  The power data objects
 * come back as { min_voltage, max_voltage, max_current }: a fixed object has
 * min_voltage == max_voltage, a programmable one offers a range.  Plain Power
 * Delivery steers between fixed objects, the programmable protocol within a
 * range, so each picks its own kind.
 */
static int protocol_class_pd_read_pdos(struct pd_pdo *pdo)
{
	return protocol_class_pd_get_pdos(g_active_port, pdo, PD_PDO_MAX_NUM);
}

static void protocol_class_pd_add_cap(struct adapter_power_cap_info *cap,
				      const struct pd_pdo *pdo)
{
	struct adapter_power_cap *c = &cap->cap[cap->pdo_nums++];

	c->max_current = pdo->max_current;
	c->min_voltage = pdo->min_voltage;
	c->max_voltage = pdo->max_voltage;
	c->max_power = pdo->max_current * pdo->max_voltage / 1000;
}

/* The programmable objects only; -1 when there are none. */
static int protocol_class_pd_read_apdo_cap(struct adapter_power_cap_info *cap)
{
	struct pd_pdo pdo[PD_PDO_MAX_NUM] = { 0 };
	int i;

	if (protocol_class_pd_read_pdos(pdo))
		return -1;

	cap->pdo_nums = 0;
	for (i = 0; i < PD_PDO_MAX_NUM; i++) {
		if (pdo[i].max_voltage == pdo[i].min_voltage)
			continue;

		protocol_class_pd_add_cap(cap, &pdo[i]);
	}

	return cap->pdo_nums ? 0 : -1;
}

/* The fixed objects only; -1 when there are none. */
static int protocol_class_pd_get_pwr_cap(void *data,
					 struct adapter_power_cap_info *pwr_cap)
{
	struct pd_pdo pdo[PD_PDO_MAX_NUM] = { 0 };
	int i;

	if (protocol_class_pd_read_pdos(pdo))
		return -1;

	pwr_cap->pdo_nums = 0;
	for (i = 0; i < PD_PDO_MAX_NUM; i++) {
		if (!pdo[i].max_voltage ||
		    pdo[i].max_voltage != pdo[i].min_voltage)
			continue;

		protocol_class_pd_add_cap(pwr_cap, &pdo[i]);

		mca_log_info("pwr_cap[%d] vmin: %d, vmax: %d, imax: %d\n",
			     i, pdo[i].max_voltage, pdo[i].max_voltage,
			     pdo[i].max_current);
	}

	return pwr_cap->pdo_nums ? 0 : -1;
}

static int protocol_class_pps_get_pwr_cap(void *data,
					  struct adapter_power_cap_info *pwr_cap)
{
	return protocol_class_pd_read_apdo_cap(pwr_cap);
}

/* Only a programmable or a verified Xiaomi adapter has a power figure. */
static bool protocol_class_pd_type_has_power(int pd_type)
{
	return pd_type == XM_CHARGER_TYPE_PPS ||
	       pd_type == XM_CHARGER_TYPE_PD_VERIFY;
}

/*
 * The highest power, in watts, among the programmable operating points.  On a
 * board in the second charge pump mode a verified adapter's points from
 * PD_CP_VOLT_MAX_MV up are left out: that path cannot take them.
 */
static int protocol_class_pps_cap_max_power(const struct adapter_power_cap_info *cap,
					    int pd_type)
{
	int max_power = 0;
	int i;

	for (i = 0; i < cap->pdo_nums; i++) {
		if (g_support_mode == PD_SUPPORT_MODE_DUAL_CP &&
		    cap->cap[i].max_voltage >= PD_CP_VOLT_MAX_MV &&
		    pd_type == XM_CHARGER_TYPE_PD_VERIFY)
			continue;

		max_power = max(max_power, cap->cap[i].max_power / 1000);
	}

	return max_power;
}

/*
 * What the adapter is sold as.  The power computed from the operating points
 * rounds down from the rating (a 120 W adapter may compute to 110 W), so it
 * is mapped onto the rated classes; the current offered on the range that
 * covers 10 V separates a 100 W adapter from a 90 W one near the boundary.
 */
static int protocol_class_pps_rated_power(int max_power, int adv_curr)
{
	if (max_power >= 96 && adv_curr == 4800)
		return 100;
	if (max_power == 100 || (max_power >= 96 && adv_curr == 5000))
		return 100;
	if (max_power >= 101 && max_power <= 129)
		return 120;
	if (max_power >= 70 && max_power <= 99)
		return 90;
	if (max_power >= 66 && max_power <= 69)
		return 67;
	if (max_power >= 60 && max_power <= 65)
		return 65;
	if (max_power >= 55 && max_power <= 59)
		return 55;
	if (max_power >= 50 && max_power <= 54)
		return 50;

	return 33;
}

/*
 * The power the attached adapter is rated for, never more than the device
 * takes ("max_power" in DT).  A programmable adapter on a European model
 * reports what it computes to rather than its rating.  Always succeeds;
 * the figure is 0 for an adapter that has none.
 */
static int protocol_class_pps_adapter_get_max_power(void *data, u32 *max_power)
{
	struct adapter_power_cap_info pwr_cap = { 0 };
	bool is_eu_model = false;
	int computed = 0, adv_curr = 0;
	int pd_type = 0;
	int i;

	*max_power = 0;

	protocol_class_pd_get_pd_type(g_active_port, &pd_type);
	if (!protocol_class_pd_type_has_power(pd_type))
		return 0;

	charger_partition_get_eu_model(&is_eu_model);

	if (!protocol_class_pd_read_apdo_cap(&pwr_cap)) {
		computed = protocol_class_pps_cap_max_power(&pwr_cap, pd_type);

		/* The current offered by the first range covering 10 V. */
		for (i = 0; i < ADAPTER_POWER_CAP_MAX; i++) {
			if (pwr_cap.cap[i].min_voltage <= 10000 &&
			    pwr_cap.cap[i].max_voltage >= 10000) {
				adv_curr = pwr_cap.cap[i].max_current;
				break;
			}
		}
	}

	mca_log_info("is_eu_model: %d,device_max_power: %d, max_power: %d, adv_curr: %d\n",
		     is_eu_model, g_device_max_power, computed, adv_curr);

	if (is_eu_model && pd_type == XM_CHARGER_TYPE_PPS)
		*max_power = min_t(int, computed, g_device_max_power);
	else
		*max_power = min_t(int,
				   protocol_class_pps_rated_power(computed, adv_curr),
				   g_device_max_power);

	mca_log_info("caulate max_power %d\n", *max_power);

	return 0;
}

/*
 * The most the adapter's programmable operating points add up to, in watts,
 * not mapped to a rating and not capped: what a charge pump is planned
 * against.  Always succeeds; the figure is 0 for an adapter that has none.
 */
static int protocol_class_pps_adapter_get_pwr_max_power(void *data,
							u32 *max_power)
{
	struct adapter_power_cap_info pwr_cap = { 0 };
	int pd_type = 0;
	int watts = 0;

	*max_power = 0;

	protocol_class_pd_get_pd_type(g_active_port, &pd_type);
	if (!protocol_class_pd_type_has_power(pd_type))
		return 0;

	if (!protocol_class_pd_read_apdo_cap(&pwr_cap))
		watts = protocol_class_pps_cap_max_power(&pwr_cap, pd_type);

	*max_power = watts;
	mca_log_info("get_pwr_max_power %d\n", watts);

	return 0;
}

/* Plain Power Delivery has no power figure of its own. */
static int protocol_class_pd_get_max_power(void *data, u32 *max_power)
{
	return 0;
}

static int protocol_class_pd_get_adapter_type(void *data, int *type)
{
	return protocol_class_pd_get_pd_type(g_active_port, type);
}

static int protocol_class_pd_get_adapter_verified(void *data, int *verified)
{
	return protocol_class_pd_get_pd_verifed(g_active_port, verified);
}

static int protocol_class_pd_set_adapter_verified(void *data, int verified)
{
	return protocol_class_pd_set_pd_verifed(g_active_port, verified);
}

static int protocol_class_pps_set_volt_and_curr(void *data, int volt, int curr)
{
	return protocol_class_pd_set_pps_pdo_select(g_active_port, volt, curr);
}

static int protocol_class_pps_get_volt_and_curr(void *data, int *volt,
						int *curr)
{
	return protocol_class_pd_get_pps_status(g_active_port, volt, curr);
}

static int protocol_class_pps_get_pps_ptf(void *data, int *ptf)
{
	return protocol_class_pd_get_pps_ptf(g_active_port, ptf);
}

/* Vendor information is not available over Power Delivery; nothing is filled. */
static int protocol_class_pd_get_adapter_info(void *data,
					      struct adapter_vendor_info *info)
{
	return 0;
}

static const struct adapter_protocol_class_ops g_protocol_pd_ops = {
	.set_adapter_verified		= protocol_class_pd_set_adapter_verified,
	.get_adapter_verified		= protocol_class_pd_get_adapter_verified,
	.get_adapter_type		= protocol_class_pd_get_adapter_type,
	.get_adapter_max_power		= protocol_class_pd_get_max_power,
	.get_adapter_pwr_cap		= protocol_class_pd_get_pwr_cap,
	.set_adapter_volt_and_curr	= protocol_class_pps_set_volt_and_curr,
	.get_adapter_pps_ptf		= protocol_class_pps_get_pps_ptf,
	.get_adapter_info		= protocol_class_pd_get_adapter_info,
};

static const struct adapter_protocol_class_ops g_protocol_pps_ops = {
	.get_adapter_max_power		= protocol_class_pps_adapter_get_max_power,
	.get_adapter_pwr_cap		= protocol_class_pps_get_pwr_cap,
	.set_adapter_volt_and_curr	= protocol_class_pps_set_volt_and_curr,
	.get_adapter_volt_and_curr	= protocol_class_pps_get_volt_and_curr,
	.get_adapter_pps_ptf		= protocol_class_pps_get_pps_ptf,
	.get_adapter_info		= protocol_class_pd_get_adapter_info,
	.get_adapter_pwr_max_power	= protocol_class_pps_adapter_get_pwr_max_power,
};

/*
 * A Type-C port status change: the active port is the first one that has a
 * sink, source or audio accessory attached, port 0 when neither has.
 */
static bool protocol_pd_mode_attached(int mode)
{
	return mode >= TYPEC_SNK_MODE && mode <= TYPEC_AUDIO_ACCESS_MODE;
}

static int protocol_pd_notify_cb(struct notifier_block *nb, unsigned long event,
				 void *data)
{
	int mode = TYPEC_UNATTACHED;

	protocol_class_pd_get_snk_src_mode(TYPEC_PORT_0, &mode);
	if (protocol_pd_mode_attached(mode)) {
		g_active_port = TYPEC_PORT_0;
	} else {
		protocol_class_pd_get_snk_src_mode(TYPEC_PORT_1, &mode);
		if (protocol_pd_mode_attached(mode))
			g_active_port = TYPEC_PORT_1;
		else
			g_active_port = TYPEC_PORT_0;
	}

	mca_log_info("cur active port %d\n", g_active_port);

	return NOTIFY_OK;
}

static struct notifier_block protocol_pd_nb = {
	.notifier_call = protocol_pd_notify_cb,
};

static ssize_t pd_class_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf);
static ssize_t pd_class_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count);

static struct mca_sysfs_attr_info pd_class_sysfs_field_tbl[] = {
	mca_sysfs_attr_ro(pd_class_sysfs, 0440, PD_PROP_CC_ORIENTATION,
			  cc_orientation),
	mca_sysfs_attr_ro(pd_class_sysfs, 0440, PD_PROP_APDO_MAX, apdo_max),
	mca_sysfs_attr_ro(pd_class_sysfs, 0440, PD_PROP_HAS_DP, has_dp),
	mca_sysfs_attr_ro(pd_class_sysfs, 0440, PD_PROP_CID_STATUS,
			  cid_status),
	mca_sysfs_attr_ro(pd_class_sysfs, 0440, PD_PROP_OTG_UI_SUPPORT,
			  otg_ui_support),
	mca_sysfs_attr_rw(pd_class_sysfs, 0640, PD_PROP_CC_TOGGLE, cc_toggle),
	mca_sysfs_attr_rw(pd_class_sysfs, 0640, PD_PROP_PPS_PTF, pps_ptf),
	mca_sysfs_attr_ro(pd_class_sysfs, 0440, PD_PROP_CC_SHORT_VBUS,
			  cc_short_vbus),
	mca_sysfs_attr_ro(pd_class_sysfs, 0440, PD_PROP_TYPEC_MODE,
			  typec_mode),
};

static ssize_t pd_class_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct mca_sysfs_attr_info *info;
	bool flag = false;
	u32 val32 = 0;
	int pd_type = 0;
	int val = 0;

	info = mca_sysfs_lookup_attr(attr->attr.name, pd_class_sysfs_field_tbl,
				     ARRAY_SIZE(pd_class_sysfs_field_tbl));
	if (!info)
		return -1;

	switch (info->sysfs_attr_name) {
	case PD_PROP_CC_ORIENTATION:
		protocol_class_pd_get_typec_cc_orientation(g_active_port, &val);
		mca_log_err("read cc_orientation: cur_port %d %d\n",
			    g_active_port, val);
		break;
	case PD_PROP_APDO_MAX:
		/* The adapter's rated power in watts, not a voltage. */
		protocol_class_pps_adapter_get_max_power(NULL, &val32);
		val = val32;
		mca_log_err("read apdo_max: %d\n", val);
		break;
	case PD_PROP_HAS_DP:
		protocol_class_pd_get_has_dp(g_active_port, &flag);
		val = flag;
		mca_log_err("read has_dp: %d\n", val);
		break;
	case PD_PROP_CID_STATUS:
		protocol_class_pd_get_cid_status(g_active_port, &flag);
		val = flag;
		break;
	case PD_PROP_OTG_UI_SUPPORT:
		/* The OTG entry in the UI is always offered. */
		val = 1;
		break;
	case PD_PROP_CC_TOGGLE:
		protocol_class_pd_get_cc_toggle(g_active_port, &flag);
		val = flag;
		break;
	case PD_PROP_CC_SHORT_VBUS:
		protocol_class_pd_get_cc_short_vbus(g_active_port, &val);
		break;
	case PD_PROP_PPS_PTF:
		/* Only a programmable or verified adapter has a PTF reading. */
		protocol_class_pd_get_pd_type(g_active_port, &pd_type);
		if (protocol_class_pd_type_has_power(pd_type))
			protocol_class_pd_get_pps_ptf(g_active_port, &val);
		mca_log_err("read buf: %d\n", val);
		break;
	case PD_PROP_TYPEC_MODE:
		protocol_class_pd_get_typec_mode(g_active_port, &val);
		break;
	default:
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t pd_class_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct mca_sysfs_attr_info *info;
	int val = 0;

	info = mca_sysfs_lookup_attr(attr->attr.name, pd_class_sysfs_field_tbl,
				     ARRAY_SIZE(pd_class_sysfs_field_tbl));
	if (!info)
		return -1;

	switch (info->sysfs_attr_name) {
	case PD_PROP_CC_TOGGLE:
		if (kstrtoint(buf, 10, &val))
			return -EINVAL;
		protocol_class_pd_set_cc_toggle(g_active_port, !!val);
		break;
	default:
		break;
	}

	return count;
}

static int protocol_pd_class_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	int ret;

	ret = protocol_class_register_ops(ADAPTER_PROTOCOL_PD,
					  &g_protocol_pd_ops, NULL);
	if (ret)
		return ret;

	ret = protocol_class_register_ops(ADAPTER_PROTOCOL_PPS,
					  &g_protocol_pps_ops, NULL);
	if (ret)
		return ret;

	mca_parse_dts_u32(np, "max_power", &g_device_max_power, 0);
	mca_parse_dts_u32(np, "support_mode", &g_support_mode, 0);

	ret = mca_event_block_notify_register(MCA_EVENT_TYPE_TYPEC_PORT_STATUS,
					      &protocol_pd_nb);
	if (ret)
		return -EPROBE_DEFER;

	g_active_port = TYPEC_PORT_0;

	mca_sysfs_create_files(MCA_SYSFS_DEV_TYPEC, pd_class_sysfs_field_tbl,
			       ARRAY_SIZE(pd_class_sysfs_field_tbl));

	mca_log_err("probe ok\n");

	return 0;
}

/* Leave no notifier behind on the chain once the module text is gone. */
static int protocol_pd_class_remove(struct platform_device *pdev)
{
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_TYPEC_PORT_STATUS,
					  &protocol_pd_nb);

	return 0;
}

static void protocol_pd_class_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,protocol_pd" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver protocol_pd_class_driver = {
	.driver = {
		.name		= "protocol_pd_class",
		.of_match_table	= match_table,
	},
	.probe		= protocol_pd_class_probe,
	.remove		= protocol_pd_class_remove,
	.shutdown	= protocol_pd_class_shutdown,
};
module_platform_driver(protocol_pd_class_driver);

MODULE_DESCRIPTION("protocol pd class");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
