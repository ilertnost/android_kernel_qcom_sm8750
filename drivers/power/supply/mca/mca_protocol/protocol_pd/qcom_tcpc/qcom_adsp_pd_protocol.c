// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Power Delivery, as reached through the ADSP.
 *
 * The Type-C port and the Power Delivery policy engine belong to the ADSP on
 * this platform.  It runs the negotiation, holds the contract, and is the
 * only thing that talks to the port controller.  Nothing here decides
 * anything: each call the charging stack makes becomes the property read or
 * write that asks the firmware the same question.
 *
 * The one place that is more than a single property is the vendor message
 * exchange.  Xiaomi's adapters authenticate over unstructured vendor-defined
 * messages, and that is a conversation rather than a value: a command goes
 * out, the adapter answers, and the answer arrives as a notification rather
 * than as the result of the write.  So a request and its reply are separate
 * properties, and the caller reads the reply when the firmware says one has
 * come.
 */

#define MCA_LOG_TAG "qcom_adsp_pd"

#include <linux/device.h>
#include <linux/errno.h>
#include <mca/common/mca_adsp_glink.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/protocol/protocol_pd_class.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>

/**
 * struct adsp_protocol_pd_data - this driver's state
 * @dev:            this device
 * @pd_active:      whether a Power Delivery contract is in force
 * @pd_verifed:     whether the adapter proved itself
 * @event_nb:       hears the charger being unplugged
 * @abnormal_nb:    hears the firmware reporting a fault
 * @verify_process: whether the adapter authentication is in progress
 *
 * @pd_active, @pd_verifed and @verify_process are kept here rather than asked
 * of the firmware every time: the kernel is the one that decided them, and
 * the firmware only needs telling.
 */
struct adsp_protocol_pd_data {
	struct device		*dev;
	int			pd_active;
	int			pd_verifed;
	struct notifier_block	event_nb;
	struct notifier_block	abnormal_nb;
	int			verify_process;
};

static int adsp_pd_read_u32(u32 prop, u32 *val)
{
	return mca_adsp_glink_read_prop(prop, val, sizeof(*val));
}

/*
 * A handful of the firmware's properties are a single byte.  Reading four
 * bytes into a u32 would copy three more out of the response buffer, which
 * holds whatever the last message carried, so ask for exactly the byte the
 * property is.
 */
static int adsp_pd_read_bool(u32 prop, bool *val)
{
	return mca_adsp_glink_read_prop(prop, val, sizeof(*val));
}

static int adsp_pd_write_u32(u32 prop, u32 val)
{
	return mca_adsp_glink_write_prop(prop, &val, sizeof(val));
}

/* The single-byte properties are written as narrowly as they are read. */
static int adsp_pd_write_bool(u32 prop, bool val)
{
	u8 raw = val;

	return mca_adsp_glink_write_prop(prop, &raw, sizeof(raw));
}

static int adsp_pd_protocol_get_pps_max_power(u32 *max_power, void *data)
{
	/*
	 * This is a write of the current value: only the return code says
	 * how the firmware took it, and *max_power is not updated.
	 */
	return mca_adsp_glink_write_prop(ADSP_PROP_ID_TYPEC_PPS_POWER_MAX,
					 max_power, sizeof(*max_power));
}

/*
 * PPS voltages and currents cross the glink in the units the PD specification
 * uses rather than in millivolts and milliamps, packed into a single word with
 * the voltage in the top half.  Both the request and the status report use
 * this encoding.
 */
#define ADSP_PPS_VOLT_STEP_MV	20
#define ADSP_PPS_CURR_STEP_MA	50
#define ADSP_PPS_VOLT_SHIFT	16
#define ADSP_PPS_CURR_MASK	0xffff

/**
 * adsp_pd_protocol_select_pps_pdo() - ask the adapter for a voltage
 * @vbus_mv: the voltage, in millivolts
 * @ibus_ma: the current to allow with it, in milliamps
 * @data:    this driver's state
 *
 * Both go across together because they are one request as far as the adapter
 * is concerned: asking for a voltage without saying what current goes with it
 * gets whatever the last request said.
 *
 * Return: 0, or a negative error.
 */
static int adsp_pd_protocol_select_pps_pdo(u32 vbus_mv, u32 ibus_ma,
					   void *data)
{
	u32 req;

	mca_log_info("vbus_mv: %d, ibus_ma: %d\n", vbus_mv, ibus_ma);

	req = (vbus_mv / ADSP_PPS_VOLT_STEP_MV) << ADSP_PPS_VOLT_SHIFT |
	      (ibus_ma / ADSP_PPS_CURR_STEP_MA);

	return mca_adsp_glink_write_prop(ADSP_PROP_ID_TYPEC_PPS_REQ, &req,
					 sizeof(req));
}

/*
 * While the adapter is being authenticated the contract must not change under
 * the exchange, so a fixed voltage request is dropped (and reported as done).
 */
static int adsp_pd_protocol_set_fixed_pd_volt(u32 vbus_mv, void *data)
{
	struct adsp_protocol_pd_data *chip = data;

	mca_log_info("vbus_mv: %d\n", vbus_mv);

	if (chip->verify_process) {
		mca_log_err("pd verify processing, won't set fixed pd volt\n");
		return 0;
	}

	return adsp_pd_write_u32(ADSP_PROP_ID_TYPEC_FIXED_VOL_REQ, vbus_mv);
}

static int adsp_pd_protocol_set_gear_shift(int gear_shift, void *data)
{
	return adsp_pd_write_u32(ADSP_PROP_ID_TYPEC_GEAR_SHIFT, gear_shift);
}

static int adsp_pd_protocol_get_pps_ptf(int *pps_ptf, void *data)
{
	return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_PPS_PTF, pps_ptf);
}

static int adsp_pd_protocol_get_pps_max_cur(u32 *curr, void *data)
{
	return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_PPS_ADV_CURRENT, curr);
}

/**
 * adsp_pd_protocol_get_pps_status() - what the adapter is actually supplying
 * @volt: filled in with the voltage
 * @curr: filled in with the current
 * @data: this driver's state
 *
 * The two are read together because they are one message from the adapter,
 * and reading them separately would pair a voltage with a current measured at
 * a different moment.
 *
 * Return: 0, or a negative error.
 */
static int adsp_pd_protocol_get_pps_status(u32 *volt, u32 *curr, void *data)
{
	u32 status = 0;
	int rc;

	rc = mca_adsp_glink_read_prop(ADSP_PROP_ID_TYPEC_PPS_STATUS, &status,
				      sizeof(status));
	if (rc)
		return rc;

	*curr = (status & ADSP_PPS_CURR_MASK) * ADSP_PPS_CURR_STEP_MA;
	*volt = (status >> ADSP_PPS_VOLT_SHIFT) * ADSP_PPS_VOLT_STEP_MV;

	return 0;
}

static int adsp_pd_protocol_get_pps_apdo_max(u32 *apdo_max, void *data)
{
	return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_PPS_APDO_MAX, apdo_max);
}

static int adsp_pd_protocol_get_pd_type(int *pd_type, void *data)
{
	struct adsp_protocol_pd_data *chip = data;
	u32 real_type = 0;
	int rc;

	if (!chip)
		return -1;

	rc = adsp_pd_read_u32(ADSP_PROP_ID_USB_USB_REAL_TYPE, &real_type);
	if (rc)
		return rc;

	/*
	 * PPS is the only type the firmware reports here that the charging
	 * strategies can act on, and once the adapter has been authenticated
	 * it becomes PD_VERIFY -- which is what selects Xiaomi's own charging
	 * profile rather than plain PPS.  Anything else is reported as
	 * unknown: plain PD and legacy chargers are handled through the BC1.2
	 * and QC paths instead.
	 */
	if (real_type == XM_CHARGER_TYPE_PPS)
		*pd_type = chip->pd_verifed ? XM_CHARGER_TYPE_PD_VERIFY
					    : XM_CHARGER_TYPE_PPS;
	else
		*pd_type = XM_CHARGER_TYPE_UNKNOW;

	return 0;
}

static int adsp_pd_protocol_get_typec_mode(int *typec_mode, void *data)
{
	return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_TYPEC_MODE, typec_mode);
}

static int adsp_pd_protocol_get_typec_cc_orientation(int *cc_orientation,
						     void *data)
{
	return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_CC_ORIENTATION,
				cc_orientation);
}

static int adsp_pd_protocol_get_adapter_id(u32 *adapter_id, void *data)
{
	return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_ADAPTER_ID, adapter_id);
}

static int adsp_pd_protocol_get_adapter_svid(u32 *adapter_svid, void *data)
{
	return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_ADAPTER_SVID, adapter_svid);
}

static int adsp_pd_protocol_get_has_dp(bool *has_dp, void *data)
{
	return adsp_pd_read_bool(ADSP_PROP_ID_TYPEC_HAS_DP, has_dp);
}

/*
 * The authentication payload: four words, sent big-endian on the wire.  A
 * shorter payload than one word is refused; the caller's buffer must hold all
 * four words whatever @size says, as they are swapped in place.
 */
#define ADSP_VDM_AUTH_WORDS	4
#define ADSP_VDM_AUTH_MIN_SIZE	sizeof(u32)

static void adsp_pd_protocol_swap_vdm_payload(u32 *data_out)
{
	int i;

	for (i = 0; i < ADSP_VDM_AUTH_WORDS; i++)
		data_out[i] = swab32(data_out[i]);
}

/**
 * adsp_pd_protocol_request_vdm_cmd() - send a vendor message to the adapter
 * @cmd:      which message
 * @data_out: what goes with it; for the three authentication messages it
 *            must hold ADSP_VDM_AUTH_WORDS words
 * @size:     how many bytes of @data_out to send
 * @data:     this driver's state
 *
 * Each command is its own property.  The reply does not come back here: the
 * adapter answers in its own time and the firmware raises a notification
 * when it has, after which adsp_pd_protocol_get_vdm_cmd() collects it.
 *
 * The three authentication payloads are byte swapped in place to the order
 * the adapter expects.  A command without a property of its own is sent as a
 * charger version request.
 *
 * Return: 0, or a negative error.
 */
static int adsp_pd_protocol_request_vdm_cmd(enum uvdm_state cmd, u32 *data_out,
					    u32 size, void *data)
{
	u32 prop = ADSP_PROP_ID_TYPEC_VDM_CMD_CHARGER_VERSION;

	if (!data_out)
		return -1;

	mca_log_info("data_len=%d, cmd=%d, data=%d\n", size, cmd, data_out[0]);

	switch (cmd) {
	case USBPD_UVDM_CHARGER_VERSION:
		prop = ADSP_PROP_ID_TYPEC_VDM_CMD_CHARGER_VERSION;
		break;
	case USBPD_UVDM_CHARGER_VOLTAGE:
		prop = ADSP_PROP_ID_TYPEC_VDM_CMD_CHARGER_VOLTAGE;
		break;
	case USBPD_UVDM_CHARGER_TEMP:
		prop = ADSP_PROP_ID_TYPEC_VDM_CMD_CHARGER_TEMP;
		break;
	case USBPD_UVDM_SESSION_SEED:
		if (size < ADSP_VDM_AUTH_MIN_SIZE)
			return -1;
		adsp_pd_protocol_swap_vdm_payload(data_out);
		mca_log_info("SESSION_SEED:data = 0x%x 0x%x 0x%x 0x%x\n",
			     data_out[0], data_out[1], data_out[2], data_out[3]);
		prop = ADSP_PROP_ID_TYPEC_VDM_CMD_SESSION_SEED;
		break;
	case USBPD_UVDM_AUTHENTICATION:
		if (size < ADSP_VDM_AUTH_MIN_SIZE)
			return -1;
		adsp_pd_protocol_swap_vdm_payload(data_out);
		mca_log_info("AUTHENTICATION:data =  0x%x 0x%x 0x%x 0x%x\n",
			     data_out[0], data_out[1], data_out[2], data_out[3]);
		prop = ADSP_PROP_ID_TYPEC_VDM_CMD_AUTHENTICATION;
		break;
	case USBPD_UVDM_VERIFIED:
		prop = ADSP_PROP_ID_TYPEC_VDM_CMD_VERIFIED;
		break;
	case USBPD_UVDM_REMOVE_COMPENSATION:
		prop = ADSP_PROP_ID_TYPEC_VDM_CMD_REMOVE_COMPENSATION;
		break;
	case USBPD_UVDM_REVERSE_AUTHEN:
		if (size < ADSP_VDM_AUTH_MIN_SIZE)
			return -1;
		adsp_pd_protocol_swap_vdm_payload(data_out);
		mca_log_info("REVERSE_AUTHEN:data = %d\n", data_out[0]);
		mca_log_info("REVERSE_AUTHEN:data = %d\n", data_out[1]);
		mca_log_info("REVERSE_AUTHEN:data = %d\n", data_out[2]);
		mca_log_info("REVERSE_AUTHEN:data = %d\n", data_out[3]);
		prop = ADSP_PROP_ID_TYPEC_VDM_CMD_REVERSE_AUTHEN;
		break;
	case USBPD_UVDM_REQUEST_PD_DR:
		prop = ADSP_PROP_ID_TYPEC_VDM_CMD_REQUEST_PD_DR;
		break;
	case USBPD_UVDM_RESET_VSAFE0V:
		prop = ADSP_PROP_ID_TYPEC_VDM_CMD_RESET_VSAFE0V;
		break;
	default:
		pr_info("cmd:%d is not support\n", cmd);
		break;
	}

	return mca_adsp_glink_write_prop(prop, data_out, size);
}

/**
 * adsp_pd_protocol_get_vdm_cmd() - collect the adapter's answer
 * @cmd:      filled in with which message it answers
 * @vdm_data: filled in with the answer
 * @data:     this driver's state
 *
 * Return: 0, or a negative error.
 */
static int adsp_pd_protocol_get_vdm_cmd(int *cmd,
					struct usbpd_vdm_data *vdm_data,
					void *data)
{
	u32 digest[USBPD_UVDM_SS_LEN] = { 0 };
	int rc;
	int i;

	if (!vdm_data)
		return -1;

	rc = adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_UVDM_STATE, cmd);
	if (rc)
		return rc;

	mca_log_info("current uvdm_state: %d\n", *cmd);

	/*
	 * Each command has an answer of its own, so only the one that was
	 * asked is read back: the rest hold whatever the last exchange left.
	 */
	switch (*cmd) {
	case USBPD_UVDM_CHARGER_VERSION:
		return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_VDM_CMD_CHARGER_VERSION,
					&vdm_data->ta_version);
	case USBPD_UVDM_CHARGER_VOLTAGE:
		return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_VDM_CMD_CHARGER_VOLTAGE,
					&vdm_data->ta_voltage);
	case USBPD_UVDM_CHARGER_TEMP:
		return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_VDM_CMD_CHARGER_TEMP,
					&vdm_data->ta_temp);
	case USBPD_UVDM_AUTHENTICATION:
		rc = mca_adsp_glink_read_prop(ADSP_PROP_ID_TYPEC_VDM_CMD_AUTHENTICATION,
					      digest, sizeof(digest));
		mca_log_info("get auth data[0]=%u, data[1]=%u, data[2]=%u, data[3]=%u",
			     digest[0], digest[1], digest[2], digest[3]);

		for (i = 0; i < USBPD_UVDM_SS_LEN; i++)
			vdm_data->digest[i] = digest[i];

		return rc;
	default:
		return rc;
	}
}

/*
 * The data role is a single byte (enum xm_request_pd_dr_type); one that
 * cannot be read is reported as UFP.
 */
static int adsp_pd_protocol_get_data_role(int *data_role, void *data)
{
	u32 role = XM_REQUEST_PD_DR_UFP;
	int rc;

	rc = adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_VDM_CMD_REQUEST_PD_DR, &role);
	*data_role = role & 0xff;
	mca_log_info("get data_role: %d\n", *data_role);

	return rc;
}

static const char * const adsp_pd_state_name[] = {
	"SRC_Ready",
	"SNK_STARTUP",
	"SNK_Ready",
};

/* The room a caller must provide for the state name. */
#define ADSP_PD_STATE_BUF_MIN	64

/**
 * adsp_pd_protocol_get_current_state() - what the port is doing, in words
 * @buf:  filled in with the state
 * @size: how much room there is
 * @data: this driver's state
 *
 * The property is a number, not a name -- the names live here.  A state the
 * firmware reports that this table does not cover is reported as unknown
 * rather than guessed at.
 *
 * Return: 0, or a negative error.
 */
static int adsp_pd_protocol_get_current_state(char *buf, int size, void *data)
{
	const char *name = "UNKNOWN";
	u32 state = 0;
	int rc;

	if (size < ADSP_PD_STATE_BUF_MIN)
		return -1;

	rc = adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_CURRENT_STATE, &state);
	if (rc)
		return rc;

	if (state < ARRAY_SIZE(adsp_pd_state_name))
		name = adsp_pd_state_name[state];

	strscpy(buf, name, size);
	mca_log_info("get current_state: %u => %s\n", state, buf);

	return 0;
}

static int adsp_pd_protocol_get_pdos(struct pd_pdo *pdo, int size, void *data)
{
	int rc;
	int i;

	if (size < 0 || size * sizeof(*pdo) > MCA_GLINK_DATA_MAX)
		return -EINVAL;

	rc = mca_adsp_glink_read_prop(ADSP_PROP_ID_TYPEC_PDO, pdo,
				      size * sizeof(*pdo));
	if (rc)
		return rc;

	for (i = 0; i < size; i++)
		mca_log_info("pdo[%d] min_volt: %d, max_volt: %d, max_current: %d\n",
			     i, pdo[i].min_voltage, pdo[i].max_voltage,
			     pdo[i].max_current);

	return 0;
}

static int adsp_pd_protocol_set_verify_process(int verify_process, void *data)
{
	struct adsp_protocol_pd_data *chip = data;

	chip->verify_process = verify_process;
	mca_log_err("verify_process: %d\n", verify_process);

	return adsp_pd_write_u32(ADSP_PROP_ID_TYPEC_VERIFY_PROCESS,
				 verify_process);
}

static int adsp_pd_protocol_get_verify_process(int *verify_process, void *data)
{
	return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_VERIFY_PROCESS,
				verify_process);
}

static int adsp_pd_protocol_set_pd_verifed(int pd_verifed, void *data)
{
	struct adsp_protocol_pd_data *chip = data;

	mca_log_err("set pd_verifed: %d\n", pd_verifed);

	if (chip)
		chip->pd_verifed = pd_verifed;

	adsp_pd_write_u32(ADSP_PROP_ID_TYPEC_PD_VERIFED, pd_verifed);

	return 0;
}

static int adsp_pd_protocol_get_pd_verifed(int *pd_verifed, void *data)
{
	struct adsp_protocol_pd_data *chip = data;

	mca_log_info("get verifed %d\n", chip->pd_verifed);

	*pd_verifed = chip->pd_verifed;

	return 0;
}

static int adsp_pd_protocol_get_cid_status(bool *cid_status, void *data)
{
	return adsp_pd_read_bool(ADSP_PROP_ID_TYPEC_CID_STATUS, cid_status);
}

static int adsp_pd_protocol_get_otg_plugin_status(bool *plugin, void *data)
{
	return adsp_pd_read_bool(ADSP_PROP_ID_TYPEC_OTG_PLUGIN_STATUS, plugin);
}

static int adsp_protocol_set_cc_toggle(bool cc_toggle, void *data)
{
	mca_log_info("set cc toggle: 0x%2x\n", cc_toggle);

	return adsp_pd_write_bool(ADSP_PROP_ID_TYPEC_CC_TOGGLE, cc_toggle);
}

static int adsp_protocol_get_cc_toggle(bool *cc_toggle, void *data)
{
	return adsp_pd_read_bool(ADSP_PROP_ID_TYPEC_CC_TOGGLE, cc_toggle);
}

static int adsp_pd_protocol_get_snk_src_mode(int *snk_src_mode, void *data)
{
	return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_SNK_SRC_MODE, snk_src_mode);
}

static int adsp_protocol_get_cc_status(bool *cc_status, void *data)
{
	return adsp_pd_read_bool(ADSP_PROP_ID_TYPEC_CC_STATUS, cc_status);
}

static int adsp_protocol_get_cc_short_vbus(int *cc_short_vbus, void *data)
{
	return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_CC_SHORT_VBUS,
				cc_short_vbus);
}

static int adsp_pd_protocol_get_suspend_support_status(bool *supported,
						       void *data)
{
	return adsp_pd_read_bool(ADSP_PROP_ID_TYPEC_PD_SUSPEND_SUPPORT_STATUS, supported);
}

static int adsp_pd_protocol_get_zimi_cypress_flag(int *flag, void *data)
{
	return adsp_pd_read_u32(ADSP_PROP_ID_TYPEC_PPS_ZIMI_CYPRESS_FLAG, flag);
}

static int adsp_pd_protocol_set_pd_active(int pd_active, void *data)
{
	struct adsp_protocol_pd_data *chip = data;

	if (!chip)
		return -1;

	chip->pd_active = pd_active;

	return 0;
}

static int adsp_pd_protocol_get_pd_active(int *pd_active, void *data)
{
	struct adsp_protocol_pd_data *chip = data;

	if (!chip)
		return -1;

	*pd_active = chip->pd_active;

	return 0;
}

static const struct protocol_class_pd_ops adsp_pd_protocol_ops = {
	.protocol_pd_pps_get_max_power	= adsp_pd_protocol_get_pps_max_power,
	.protocol_pd_pps_pdo_select	= adsp_pd_protocol_select_pps_pdo,
	.protocol_pd_get_pps_ptf	= adsp_pd_protocol_get_pps_ptf,
	.protocol_pd_fixed_pdo_set_vol	= adsp_pd_protocol_set_fixed_pd_volt,
	.protocol_pd_set_gear_shift	= adsp_pd_protocol_set_gear_shift,
	.protocol_pd_get_pps_max_cur	= adsp_pd_protocol_get_pps_max_cur,
	.protocol_pd_get_pps_status	= adsp_pd_protocol_get_pps_status,
	.protocol_pd_get_pps_apdo_max	= adsp_pd_protocol_get_pps_apdo_max,
	.protocol_pd_get_pd_type	= adsp_pd_protocol_get_pd_type,
	.protocol_pd_get_typec_mode	= adsp_pd_protocol_get_typec_mode,
	.protocol_pd_get_typec_cc_orientation =
		adsp_pd_protocol_get_typec_cc_orientation,
	.protocol_pd_get_adapter_id	= adsp_pd_protocol_get_adapter_id,
	.protocol_pd_get_adapter_svid	= adsp_pd_protocol_get_adapter_svid,
	.protocol_pd_get_has_dp		= adsp_pd_protocol_get_has_dp,
	.protocol_pd_request_vdm_cmd	= adsp_pd_protocol_request_vdm_cmd,
	.protocol_pd_get_vdm_cmd	= adsp_pd_protocol_get_vdm_cmd,
	.protocol_pd_get_data_role	= adsp_pd_protocol_get_data_role,
	.protocol_pd_get_current_state	= adsp_pd_protocol_get_current_state,
	.protocol_pd_get_pdos		= adsp_pd_protocol_get_pdos,
	.protocol_pd_set_verify_process	= adsp_pd_protocol_set_verify_process,
	.protocol_pd_get_verify_process	= adsp_pd_protocol_get_verify_process,
	.protocol_pd_set_pd_verifed	= adsp_pd_protocol_set_pd_verifed,
	.protocol_pd_get_pd_verifed	= adsp_pd_protocol_get_pd_verifed,
	.protocol_pd_set_pd_active	= adsp_pd_protocol_set_pd_active,
	.protocol_pd_get_pd_active	= adsp_pd_protocol_get_pd_active,
	.protocol_pd_get_cid_status	= adsp_pd_protocol_get_cid_status,
	.protocol_pd_get_otg_plugin_status =
		adsp_pd_protocol_get_otg_plugin_status,
	.protocol_pd_set_cc_toggle	= adsp_protocol_set_cc_toggle,
	.protocol_pd_get_cc_toggle	= adsp_protocol_get_cc_toggle,
	.protocol_pd_get_snk_src_mode	= adsp_pd_protocol_get_snk_src_mode,
	.protocol_pd_get_cc_status	= adsp_protocol_get_cc_status,
	.protocol_pd_get_cc_short_vbus	= adsp_protocol_get_cc_short_vbus,
	.protocol_pd_get_suspend_support_status =
		adsp_pd_protocol_get_suspend_support_status,
	.protocol_pd_get_zimi_cypress_flag =
		adsp_pd_protocol_get_zimi_cypress_flag,
};

/* An unplugged adapter is no longer authenticated, nor being authenticated. */
static int adsp_pd_protocol_event_cb(struct notifier_block *nb,
				     unsigned long event, void *data)
{
	struct adsp_protocol_pd_data *chip =
		container_of(nb, struct adsp_protocol_pd_data, event_nb);

	if (event == MCA_EVENT_USB_DISCONNECT) {
		chip->pd_verifed = 0;
		chip->verify_process = 0;
	}

	return NOTIFY_DONE;
}

/*
 * The firmware restarted and kept none of what it had been told, so whatever
 * authentication the adapter passed no longer holds.
 */
static int adsp_pd_protocol_abnormal_cb(struct notifier_block *nb,
					unsigned long event, void *data)
{
	struct adsp_protocol_pd_data *chip =
		container_of(nb, struct adsp_protocol_pd_data, abnormal_nb);

	if (event == MCA_EVENT_CHARGE_ABNORMAL)
		chip->pd_verifed = 0;

	return NOTIFY_DONE;
}

static int adsp_pd_protocol_probe(struct platform_device *pdev)
{
	struct adsp_protocol_pd_data *chip;
	int rc;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	platform_set_drvdata(pdev, chip);

	rc = protocol_class_pd_register_ops(TYPEC_PORT_0,
					    &adsp_pd_protocol_ops, chip);
	if (rc) {
		mca_log_err("register ops fail\n");
		return rc;
	}

	chip->event_nb.notifier_call = adsp_pd_protocol_event_cb;
	mca_event_block_notify_register(MCA_EVENT_TYPE_CHARGER_CONNECT,
					&chip->event_nb);

	chip->abnormal_nb.notifier_call = adsp_pd_protocol_abnormal_cb;
	mca_event_block_notify_register(MCA_EVENT_CHARGE_STATUS,
					&chip->abnormal_nb);

	mca_log_err("probe ok\n");

	return 0;
}

/*
 * The notifier blocks live in devm memory that goes with the device, so they
 * are taken off their chains first.
 */
static int adsp_pd_protocol_remove(struct platform_device *pdev)
{
	struct adsp_protocol_pd_data *chip = platform_get_drvdata(pdev);

	mca_event_block_notify_unregister(MCA_EVENT_TYPE_CHARGER_CONNECT,
					  &chip->event_nb);
	mca_event_block_notify_unregister(MCA_EVENT_CHARGE_STATUS,
					  &chip->abnormal_nb);

	return 0;
}

static void adsp_pd_protocol_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id adsp_pd_protocol_match[] = {
	{ .compatible = "mca,adsp_pd_protocol" },
	{ }
};
MODULE_DEVICE_TABLE(of, adsp_pd_protocol_match);

static struct platform_driver adsp_pd_protocol_driver = {
	.driver = {
		.name		= "adsp_pd_protocol",
		.of_match_table	= adsp_pd_protocol_match,
	},
	.probe	= adsp_pd_protocol_probe,
	.remove	= adsp_pd_protocol_remove,
	.shutdown = adsp_pd_protocol_shutdown,
};
module_platform_driver(adsp_pd_protocol_driver);

MODULE_DESCRIPTION("qcom pd protocol");
MODULE_AUTHOR("lvchen@xiaomi.com");
MODULE_LICENSE("GPL v2");
