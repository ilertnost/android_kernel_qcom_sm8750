/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * USB Power Delivery adapters.
 *
 * Power Delivery is the protocol the charging stack gets the most out of: an
 * adapter offers a list of what it can supply and the strategy picks from it,
 * so unlike Quick Charge there is a great deal to ask about.  The driver that
 * owns the Type-C port registers here, and everything the stack wants to know
 * about a Power Delivery charger -- what it offers, what was negotiated, which
 * way round the cable is, whether the adapter proved it is genuine -- is asked
 * through this one interface.
 *
 * A phone may have more than one Type-C port, so every call names the port it
 * is about.
 */

#ifndef __MCA_PROTOCOL_PD_H
#define __MCA_PROTOCOL_PD_H

#include <mca/protocol/protocol_class.h>
#include <linux/types.h>

/* Which Type-C port. */
enum typec_port_num {
	TYPEC_PORT_0,
	TYPEC_PORT_1,
	TYPEC_PORT_MAX,
};

/* What is attached, and which way round. */
enum typec_snk_src_mode {
	TYPEC_UNATTACHED,
	TYPEC_SNK_MODE,
	TYPEC_SRC_MODE,
	TYPEC_AUDIO_ACCESS_MODE,
	TYPEC_MODE_MAX,
};


/* How much a programmable adapter is rated for. */
enum mca_apdo_max {
	MCA_APDO_MAX_30W,
	MCA_APDO_MAX_33W,
	MCA_APDO_MAX_40W,
	MCA_APDO_MAX_50W,
	MCA_APDO_MAX_55W,
	MCA_APDO_MAX_65W,
	MCA_APDO_MAX_67W,
	MCA_APDO_MAX_90W,
	MCA_APDO_MAX_100W,
	MCA_APDO_MAX_120W,
	MCA_APDO_MAX_INVALID,
};

/* How far a charge pump divides its input. */
enum mca_cp_support_mode {
	MCA_CP_SUPPORT_MODE_DIV_2,
	MCA_CP_SUPPORT_MODE_DIV_4,
};

/*
 * The exchange a Xiaomi adapter is asked to go through to prove it is
 * genuine, and the readings it reports while charging.
 */
enum uvdm_state {
	USBPD_UVDM_DISCONNECT,
	USBPD_UVDM_CHARGER_VERSION,
	USBPD_UVDM_CHARGER_VOLTAGE,
	USBPD_UVDM_CHARGER_TEMP,
	USBPD_UVDM_SESSION_SEED,
	USBPD_UVDM_AUTHENTICATION,
	USBPD_UVDM_VERIFIED,
	USBPD_UVDM_REMOVE_COMPENSATION,
	USBPD_UVDM_REVERSE_AUTHEN,
	USBPD_UVDM_CONNECT,
	USBPD_UVDM_NAN_ACK,
	USBPD_UVDM_CMD_INIT = 50,
	USBPD_UVDM_CMD_NAK = 100,
	USBPD_UVDM_REQUEST_PD_DR = 200,
	USBPD_UVDM_RESET_VSAFE0V = 255,
};

/* How many words each half of the authentication exchange carries. */
#define USBPD_UVDM_SS_LEN		4

/* Xiaomi's USB-IF vendor id, which its own adapters report. */
#define XIAOMI_USB_SVID			0x2717

/* How many power data objects an adapter may offer. */
#define PD_PDO_MAX_NUM			7

/*
 * Which end of the link carries the data role.  This is what userspace asks
 * for with %USBPD_UVDM_REQUEST_PD_DR, so the values are the vendor's.
 */
enum xm_request_pd_dr_type {
	XM_REQUEST_PD_DR_UNKNOWN,
	XM_REQUEST_PD_DR_UFP,
	XM_REQUEST_PD_DR_DFP,
};

/* Which end of the link is supplying power. */
enum pd_power_role {
	PD_ROLE_SINK,
	PD_ROLE_SOURCE,
};

/**
 * struct usbpd_vdm_data - what an adapter answered
 * @ta_version: the adapter's firmware version
 * @ta_temp:    how warm the adapter has become
 * @ta_voltage: what it is putting out
 * @reauth:     it wants to be asked to prove itself again
 * @s_secert:   the seed for this session
 * @digest:     the answer to the challenge
 */
struct usbpd_vdm_data {
	int ta_version;
	int ta_temp;
	int ta_voltage;
	int reauth;
	unsigned long s_secert[USBPD_UVDM_SS_LEN];
	unsigned long digest[USBPD_UVDM_SS_LEN];
};

/**
 * struct pd_pdo - one of the adapter's power data objects, as offered
 * @min_voltage: the lowest voltage it supplies, in millivolts
 * @max_voltage: the highest, in millivolts
 * @max_current: the current it will supply there, in milliamps
 *
 * A fixed object gives the same voltage for both; only a programmable one
 * offers a range, which is how the two are told apart.
 */
struct pd_pdo {
	int min_voltage;
	int max_voltage;
	int max_current;
};

/* What userspace can read about the port. */
enum pd_attr_list {
	PD_PROP_CC_ORIENTATION,
	PD_PROP_APDO_MAX,
	PD_PROP_HAS_DP,
	PD_PROP_CID_STATUS,
	PD_PROP_OTG_UI_SUPPORT,
	PD_PROP_CC_TOGGLE,
	PD_PROP_CC_SHORT_VBUS,
	PD_PROP_PPS_PTF,
	PD_PROP_TYPEC_MODE,
};

/**
 * struct protocol_class_pd_ops - what the Type-C port driver provides
 *
 * Every call takes the @data the port driver registered as its last argument.
 * An entry left NULL means the port cannot answer that, and the caller is
 * told so rather than being given a wrong answer.
 */
struct protocol_class_pd_ops {
	int (*protocol_pd_pps_get_max_power)(u32 *max_power, void *data);
	int (*protocol_pd_pps_pdo_select)(u32 volt, u32 curr, void *data);
	int (*protocol_pd_get_pps_ptf)(int *pps_ptf, void *data);
	int (*protocol_pd_fixed_pdo_set_vol)(u32 volt, void *data);
	int (*protocol_pd_set_gear_shift)(int gear_shift, void *data);
	int (*protocol_pd_set_pps_max_cur)(u32 curr, void *data);
	int (*protocol_pd_get_pps_max_cur)(u32 *curr, void *data);
	int (*protocol_pd_get_pps_status)(u32 *volt, u32 *curr, void *data);
	int (*protocol_pd_set_pd_active)(int pd_active, void *data);
	int (*protocol_pd_get_pd_active)(int *pd_active, void *data);
	int (*protocol_pd_set_pps_min_volt)(u32 volt, void *data);
	int (*protocol_pd_get_pps_min_volt)(u32 *volt, void *data);
	int (*protocol_pd_set_pps_max_volt)(u32 volt, void *data);
	int (*protocol_pd_get_pps_max_volt)(u32 *volt, void *data);
	int (*protocol_pd_set_pps_apdo_max)(u32 apdo_max, void *data);
	int (*protocol_pd_get_pps_apdo_max)(u32 *apdo_max, void *data);
	int (*protocol_pd_get_pd_type)(int *pd_type, void *data);
	int (*protocol_pd_set_typec_mode)(int typec_mode, void *data);
	int (*protocol_pd_get_typec_mode)(int *typec_mode, void *data);
	int (*protocol_pd_get_typec_cc_orientation)(int *cc_orientation,
						    void *data);
	int (*protocol_pd_set_typec_cc_orientation)(int cc_orientation,
						    void *data);
	int (*protocol_pd_set_in_hard_reset)(int in_hard_reset, void *data);
	int (*protocol_pd_get_in_hard_reset)(int *in_hard_reset, void *data);
	int (*protocol_pd_set_usb_suspend_supported)(int supported,
						     void *data);
	int (*protocol_pd_get_usb_suspend_supported)(int *supported,
						     void *data);
	int (*protocol_pd_set_pd_typec_accessory_mode)(int mode, void *data);
	int (*protocol_pd_get_pd_typec_accessory_mode)(int *mode, void *data);
	int (*protocol_pd_get_cap)(int cap_type,
				   struct adapter_power_cap *tacap,
				   void *data);
	int (*protocol_pd_get_adapter_id)(u32 *adapter_id, void *data);
	int (*protocol_pd_get_adapter_svid)(u32 *adapter_svid, void *data);
	int (*protocol_pd_get_has_dp)(bool *has_dp, void *data);
	int (*protocol_pd_request_vdm_cmd)(enum uvdm_state cmd, u32 *data_out,
					   u32 size, void *data);
	int (*protocol_pd_get_vdm_cmd)(int *cmd,
				       struct usbpd_vdm_data *vdm_data,
				       void *data);
	int (*protocol_pd_get_power_role)(int *power_role, void *data);
	int (*protocol_pd_get_data_role)(int *data_role, void *data);
	int (*protocol_pd_get_current_state)(char *buf, int size, void *data);
	int (*protocol_pd_get_pdos)(struct pd_pdo *pdo, int size, void *data);
	int (*protocol_pd_set_verify_process)(int verify_process, void *data);
	int (*protocol_pd_get_verify_process)(int *verify_process, void *data);
	int (*protocol_pd_set_pd_verifed)(int pd_verifed, void *data);
	int (*protocol_pd_get_pd_verifed)(int *pd_verifed, void *data);
	int (*protocol_pd_get_cid_status)(bool *cid_status, void *data);
	int (*protocol_pd_get_otg_plugin_status)(bool *plugin, void *data);
	int (*protocol_pd_set_cc_toggle)(bool cc_toggle, void *data);
	int (*protocol_pd_get_cc_toggle)(bool *cc_toggle, void *data);
	int (*protocol_pd_get_snk_src_mode)(int *snk_src_mode, void *data);
	int (*protocol_pd_get_cc_status)(bool *cc_status, void *data);
	int (*protocol_pd_get_cc_short_vbus)(int *cc_short_vbus, void *data);
	int (*protocol_pd_get_suspend_support_status)(bool *supported,
						      void *data);
	int (*protocol_pd_get_zimi_cypress_flag)(int *flag, void *data);
};

int protocol_class_pd_register_ops(enum typec_port_num port,
				   const struct protocol_class_pd_ops *ops,
				   void *data);

/* Which port a charger is on -- despite the name, not how many there are. */
int protocol_class_pd_get_port_num(void);

int protocol_class_pd_get_pps_max_power(enum typec_port_num port,
					u32 *max_power);
int protocol_class_pd_set_pps_pdo_select(enum typec_port_num port, int volt,
					 int curr);
int protocol_class_pd_get_pps_ptf(enum typec_port_num port, int *pps_ptf);
int protocol_class_pd_set_fixed_volt(enum typec_port_num port, int volt);
int protocol_class_pd_set_gear_shift(enum typec_port_num port, int gear_shift);
int protocol_class_pd_set_pps_max_cur(enum typec_port_num port, u32 curr);
int protocol_class_pd_get_pps_max_cur(enum typec_port_num port, u32 *curr);
int protocol_class_pd_get_pps_status(enum typec_port_num port, int *volt,
				     int *curr);
int protocol_class_pd_set_pd_active(enum typec_port_num port, int pd_active);
int protocol_class_pd_get_pd_active(enum typec_port_num port, int *pd_active);
int protocol_class_pd_set_pps_min_volt(enum typec_port_num port, u32 volt);
int protocol_class_pd_get_pps_min_volt(enum typec_port_num port, u32 *volt);
int protocol_class_pd_set_pps_max_volt(enum typec_port_num port, u32 volt);
int protocol_class_pd_get_pps_max_volt(enum typec_port_num port, u32 *volt);
int protocol_class_pd_set_pps_apdo_max(enum typec_port_num port, u32 apdo_max);
int protocol_class_pd_get_pps_apdo_max(enum typec_port_num port,
				       u32 *apdo_max);
int protocol_class_pd_get_pd_type(enum typec_port_num port, int *pd_type);
int protocol_class_pd_set_typec_mode(enum typec_port_num port, int typec_mode);
int protocol_class_pd_get_typec_mode(enum typec_port_num port, int *typec_mode);
int protocol_class_pd_get_typec_cc_orientation(enum typec_port_num port,
					       int *cc_orientation);
int protocol_class_pd_set_typec_cc_orientation(enum typec_port_num port,
					       int cc_orientation);
int protocol_class_pd_set_pd_in_hard_reset(enum typec_port_num port,
					   int in_hard_reset);
int protocol_class_pd_get_pd_in_hard_reset(enum typec_port_num port,
					   int *in_hard_reset);
int protocol_class_pd_set_usb_suspend_supported(enum typec_port_num port,
						int supported);
int protocol_class_pd_get_usb_suspend_supported(enum typec_port_num port,
						int *supported);
int protocol_class_pd_set_pd_typec_accessory_mode(enum typec_port_num port,
						  int typec_accessory_mode);
int protocol_class_pd_get_pd_typec_accessory_mode(enum typec_port_num port,
						  int *typec_accessory_mode);
int protocol_class_pd_get_cap(enum typec_port_num port, int cap_type,
			      struct adapter_power_cap *tacap);
int protocol_class_pd_get_adapter_id(enum typec_port_num port, u32 *adapter_id);
int protocol_class_pd_get_adapter_svid(enum typec_port_num port,
				       u32 *adapter_svid);
int protocol_class_pd_get_has_dp(enum typec_port_num port, bool *has_dp);
int protocol_class_pd_request_vdm_cmd(enum typec_port_num port,
				      enum uvdm_state cmd, u32 *data, u32 size);
int protocol_class_pd_get_vdm_cmd(enum typec_port_num port, int *cmd,
				  struct usbpd_vdm_data *vdm_data);
int protocol_class_pd_get_power_role(enum typec_port_num port,
				     int *power_role);
int protocol_class_pd_get_data_role(enum typec_port_num port, int *data_role);
int protocol_class_pd_get_current_state(enum typec_port_num port, char *buf,
					int size);
int protocol_class_pd_get_pdos(enum typec_port_num port, struct pd_pdo *pdo,
			       int size);
int protocol_class_pd_set_verify_process(enum typec_port_num port,
					 int verify_process);
int protocol_class_pd_get_verify_process(enum typec_port_num port,
					 int *verify_process);
int protocol_class_pd_set_pd_verifed(enum typec_port_num port, int pd_verifed);
int protocol_class_pd_get_pd_verifed(enum typec_port_num port,
				     int *pd_verifed);
int protocol_class_pd_get_cid_status(enum typec_port_num port,
				     bool *cid_status);
int protocol_class_pd_get_otg_plugin_status(enum typec_port_num port,
					    bool *plugin);
int protocol_class_pd_set_cc_toggle(enum typec_port_num port, bool cc_toggle);
int protocol_class_pd_get_cc_toggle(enum typec_port_num port, bool *cc_toggle);
int protocol_class_pd_get_snk_src_mode(enum typec_port_num port,
				       int *snk_src_mode);
int protocol_class_pd_get_cc_status(enum typec_port_num port, bool *cc_status);
int protocol_class_pd_get_cc_short_vbus(enum typec_port_num port,
					int *cc_short_vbus);
int protocol_class_pd_get_suspend_support_status(enum typec_port_num port,
						 bool *pdsuspendsupported);
int protocol_class_pd_get_zimi_cypress_flag(enum typec_port_num port,
					    int *zimi_cypress_flag);

#endif /* __MCA_PROTOCOL_PD_H */
