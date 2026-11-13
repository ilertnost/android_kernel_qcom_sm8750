// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Keeping the connector from burning.  See
 * include/mca/hardware/hw_connector_antiburn.h.
 */

#define MCA_LOG_TAG "mca_connector_antiburn"

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/kstrtox.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <mca/common/mca_charge_mievent.h>
#include <mca/hardware/hw_connector_antiburn.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_hwid.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/protocol/protocol_class.h>
#include <mca/protocol/protocol_pd_class.h>
#include <mca/common/mca_sysfs.h>
#include <mca/common/mca_workqueue.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

/* One thermistor at the connector, and a second one on boards that have it. */
#define ANTIBURN_NTC_NUM		2

/* How often the thermistors are read while the port is in use, in ms. */
#define ANTIBURN_FAST_INTERVAL_MS	1000

/*
 * And how often once nothing is attached.  The elaborated strategy only backs
 * off after this many consecutive readings with nothing attached and both
 * thermistors below ANTIBURN_IDLE_TEMP_MAX (millidegrees).
 */
#define ANTIBURN_IDLE_INTERVAL_MS	5000
#define ANTIBURN_IDLE_CYCLES		5
#define ANTIBURN_IDLE_TEMP_MAX		55000

/* How soon to take a reading after being told something was plugged in. */
#define ANTIBURN_KICK_DELAY_MS		1000

/* Readings closer together than this are not differentiated, in ns. */
#define ANTIBURN_MIN_TIME_GAP_NS	NSEC_PER_MSEC

/*
 * The connector temperature is never reported below room temperature, in
 * degrees for the plain strategy and millidegrees for the elaborated one.
 */
#define ANTIBURN_TEMP_FLOOR		25
#define ANTIBURN_TEMP_FLOOR_MILLI	25000

/*
 * Past this the elaborated strategy cuts the port whatever is attached: the
 * thermistor is either reading a real fire or has failed, and neither is a
 * connector to keep powered.  In millidegrees.
 */
#define ANTIBURN_TEMP_ABS_MAX_MILLI	75000

/* VBUS is still up above this while an adapter is asked to drop it, in uV. */
#define ANTIBURN_VBUS_HIGH_UV		6000000
/* VBUS counts as gone to vSafe0V below this, in uV. */
#define ANTIBURN_VBUS_SAFE0V_UV		3600000
/* VBUS still present after the MOS opened means the MOS failed, in uV. */
#define ANTIBURN_VBUS_MOS_FAIL_UV	4100000

/* How long to wait for VBUS to fall after each request. */
#define ANTIBURN_VSAFE0V_SETTLE_MS	200
/* And between the steps of the trigger sequence. */
#define ANTIBURN_TRIGGER_STEP_MS	50
/* How many times to ask before giving up on VBUS coming down. */
#define ANTIBURN_VSAFE0V_TRIES		3

/* What userspace can look at and force. */
enum connector_antiburn_attr_list {
	CONNECTOR_PROP_TEMP_1,
	CONNECTOR_PROP_TEMP_2,
	CONNECTOR_PROP_TEMP_MAX,
	CONNECTOR_PROP_RESET_VSAFE0V,
	CONNECTOR_PROP_NTC_ALARM,
	CONNECTOR_PROP_MOS_CTRL,
};

/**
 * struct connector_antiburn - the connector being watched
 * @dev:                    this device
 * @tzd_conn:               the connector's thermistor
 * @tzd_conn2:              the second one, on boards that have it
 * @thermal_board_nb:       told how warm the board is
 * @debug_nb:               told when a test turns this off
 * @connect_nb:             told when a charger is plugged in
 * @hw_nb:                  told when the port sources or senses a cable
 * @triggered:              the port has been cut for heat
 * @is_reset_vsafe0V:       the adapter took VBUS down to vSafe0V
 * @ntc_alarm:              the connector was found too hot and the cable has
 *                          not been removed since
 * @connector_temp:         the last reading from each thermistor
 * @temp_increase_rate:     how fast each is climbing, per second
 * @fake_connector_temp:    what a test is forcing, per thermistor
 * @thermal_board_temp:     how warm the board is, in degrees
 * @max_thermal_board_temp: the board temperature up to which a hot connector
 *                          counts as heat local to the connector
 * @comb_sensorboard_con_trigger_temp: the connector temperature that counts as
 *                          a fault while the board is not that warm
 * @comb_rate_conn_trigger_temp: the temperature that counts as a fault while it
 *                          is climbing fast
 * @trigger_temp:           the temperature that counts as a fault on its own
 * @recharge_temp:          the temperature below which the port is restored
 * @mos_ctrl_gpio:          the pin driving the MOS that cuts VBUS
 * @otg_detect_en:          "antiburn_otg_detect" from the device tree
 * @support_soft_antiburn:  "support_soft_antiburn" from the device tree
 * @support_hw_antiburn:    the board has the MOS
 * @use_double_ntc:         the board has two thermistors
 * @monitor_interval:       the current polling period, in milliseconds
 * @max_temp_increase_rate: the fastest climb that is not a fault
 * @thermal_zone_name:      the first thermistor's thermal zone
 * @thermal_zone_name2:     and the second one's
 * @otg_plugin_status:      something is being supplied from the port
 * @usb_online:             charging by cable
 * @cid_status:             a cable is detected in the port
 * @otg_boost_src:          which boost supplies the port
 * @en_src:                 which enable controls it
 * @support_base_flip:      "support-base-flip" is set in the device tree
 * @disable_antiburn:       a test has turned the protection off
 * @support_elaboration_anti_strategy: temperatures are in millidegrees and
 *                          judged per thermistor against the rate as well
 * @monitor_work:           reads the thermistors
 */
struct connector_antiburn {
	struct device			*dev;
	struct thermal_zone_device	*tzd_conn;
	struct thermal_zone_device	*tzd_conn2;
	struct notifier_block		thermal_board_nb;
	struct notifier_block		debug_nb;
	struct notifier_block		connect_nb;
	struct notifier_block		hw_nb;
	int				triggered;
	int				is_reset_vsafe0V;
	int				ntc_alarm;
	int				connector_temp[ANTIBURN_NTC_NUM];
	int				temp_increase_rate[ANTIBURN_NTC_NUM];
	int				fake_connector_temp[ANTIBURN_NTC_NUM];
	int				thermal_board_temp;
	int				max_thermal_board_temp;
	int				comb_sensorboard_con_trigger_temp;
	int				comb_rate_conn_trigger_temp;
	int				trigger_temp;
	int				recharge_temp;
	int				mos_ctrl_gpio;
	int				otg_detect_en;
	int				support_soft_antiburn;
	int				support_hw_antiburn;
	int				use_double_ntc;
	int				monitor_interval;
	int				max_temp_increase_rate;
	const char			*thermal_zone_name;
	const char			*thermal_zone_name2;
	bool				otg_plugin_status;
	int				usb_online;
	bool				cid_status;
	int				otg_boost_src;
	int				en_src;
	int				support_base_flip;
	int				disable_antiburn;
	int				support_elaboration_anti_strategy;
	struct delayed_work		monitor_work;
};

static struct connector_antiburn *g_conn;

/*
 * A thermistor read that failed leaves the previous reading in place; the
 * first good reading after it is differentiated against that stale value, so
 * one rate above the limit right after a recovery is discarded.
 */
static bool adc_err_flag;
static bool flag_restore;

/**
 * connector_antiburn_is_triggered() - whether the port is cut for heat
 *
 * The charging strategies ask before deciding why charging stopped, so that a
 * hot connector is reported as such rather than as a charger fault.
 */
int connector_antiburn_is_triggered(void)
{
	if (!g_conn)
		return 0;

	return g_conn->triggered;
}
EXPORT_SYMBOL(connector_antiburn_is_triggered);

/*
 * The thermal core reports millidegrees.  The elaborated strategy keeps them;
 * the plain one works in whole degrees.  A value forced by a test wins, and a
 * failed read keeps the previous reading.
 */
static int connector_antiburn_get_temperature(struct connector_antiburn *conn,
					      int index)
{
	struct thermal_zone_device *tzd;
	int temp = conn->support_elaboration_anti_strategy ?
		   ANTIBURN_TEMP_FLOOR_MILLI : ANTIBURN_TEMP_FLOOR;
	int rc;

	if (index < 0 || index >= ANTIBURN_NTC_NUM)
		return temp;

	if (conn->fake_connector_temp[index])
		return conn->fake_connector_temp[index];

	tzd = index ? conn->tzd_conn2 : conn->tzd_conn;
	rc = thermal_zone_get_temp(tzd, &temp);
	if (rc) {
		mca_log_err("iio get temp error, index is %d, connector_temp is %d, ret is %d",
			    index, temp, rc);
		adc_err_flag = true;
		return conn->connector_temp[index];
	}

	if (adc_err_flag) {
		adc_err_flag = false;
		flag_restore = true;
	}

	mca_log_info(" CONNECTOR_PROP_TEMP read %d, index is %d\n", temp, index);

	if (!conn->support_elaboration_anti_strategy)
		temp /= 1000;

	return temp;
}

/*
 * Both thermistors are read, and each one's rate of climb is taken against
 * the previous reading, per second.  Readings less than a millisecond apart
 * are not differentiated.
 */
static void
connector_antiburn_get_temp_increase_rate(struct connector_antiburn *conn)
{
	static int last_connector_temp[ANTIBURN_NTC_NUM];
	static bool first_boot_flag;
	static ktime_t last_time;
	s64 time_gap_ms;
	ktime_t now;
	int temp_gap;
	int i;

	for (i = 0; i < ANTIBURN_NTC_NUM; i++)
		conn->connector_temp[i] =
			connector_antiburn_get_temperature(conn, i);

	if (!first_boot_flag) {
		last_time = ktime_get();
		for (i = 0; i < ANTIBURN_NTC_NUM; i++) {
			last_connector_temp[i] = conn->connector_temp[i];
			conn->temp_increase_rate[i] = 0;
		}
		first_boot_flag = true;
	}

	now = ktime_get();
	time_gap_ms = ktime_ms_delta(now, last_time);
	if (ktime_to_ns(ktime_sub(now, last_time)) < ANTIBURN_MIN_TIME_GAP_NS) {
		mca_log_err("timestamp is error, time_gap is %lld, current time is %lld, last time is %lld\n",
			    time_gap_ms, ktime_to_ns(now), ktime_to_ns(last_time));
		return;
	}

	for (i = 0; i < ANTIBURN_NTC_NUM; i++) {
		temp_gap = conn->connector_temp[i] - last_connector_temp[i];
		conn->temp_increase_rate[i] = time_gap_ms ?
			(int)div64_s64((s64)(temp_gap * 1000), time_gap_ms) : 0;
		last_connector_temp[i] = conn->connector_temp[i];

		if (conn->temp_increase_rate[i] > conn->max_temp_increase_rate &&
		    flag_restore) {
			conn->temp_increase_rate[i] = 0;
			flag_restore = false;
			mca_log_info("temp_gap is %d, current temp is %d, last temp is %d, temp increase rate is %d, index is %d\n",
				     temp_gap, conn->connector_temp[i],
				     last_connector_temp[i],
				     conn->temp_increase_rate[i], i);
		}
	}

	last_time = now;
}

static void connector_report_uevent(const char *fmt, int value)
{
	struct mca_event_notify_data n_data;
	char buf[128] = { 0 };

	n_data.event = buf;
	n_data.event_len = snprintf(buf, sizeof(buf), fmt, value);
	mca_event_report_uevent(&n_data);
}

static void connector_temp_uevent(int temp)
{
	mca_log_info("connector temp uevent notify\n");
	connector_report_uevent("POWER_SUPPLY_CONNECTOR_TEMP=%d", temp);
}

static void connector_ntc_alarm_uevent(int alarm)
{
	mca_log_info("ntc alarm notify\n");
	connector_report_uevent("POWER_SUPPLY_NTC_ALARM=%d", alarm);
}

static void adapter_reset_vsafe0V_uevent(int reset)
{
	mca_log_info("adapter vbus drop 0V notify\n");
	connector_report_uevent("POWER_SUPPLY_ADAPTER_RESET_VSAFE0V=%d", reset);
}

static int connector_antiburn_get_real_type(void)
{
	u32 real_type_pd = 0;
	u32 real_type_bc = 0;

	protocol_class_get_adapter_type(ADAPTER_PROTOCOL_PD, &real_type_pd);
	protocol_class_get_adapter_type(ADAPTER_PROTOCOL_BC12, &real_type_bc);

	return real_type_pd ? real_type_pd : real_type_bc;
}

static bool connector_antiburn_is_qc(int real_type)
{
	return real_type >= XM_CHARGER_TYPE_HVDCP2 &&
	       real_type <= XM_CHARGER_TYPE_HVDCP3P5;
}

static bool connector_antiburn_is_pd(int real_type)
{
	return real_type >= XM_CHARGER_TYPE_PD &&
	       real_type <= XM_CHARGER_TYPE_PD_VERIFY;
}

/*
 * Make sure the adapter has stopped pushing a raised voltage before the port
 * is cut.  A PD adapter is asked to go to vSafe0V; a QC adapter cannot be
 * asked, so it is only waited on.  Up to three rounds.  A PD adapter that has
 * gone below vSafe0V is reported to userspace.
 */
static void connector_antiburn_ensure_vbus_sense5V(struct connector_antiburn *conn)
{
	u32 data_out = 0;
	int bus_volt = 0;
	int real_type;
	int volt = 0;
	int i;

	for (i = 0; i < ANTIBURN_VSAFE0V_TRIES; i++) {
		real_type = connector_antiburn_get_real_type();
		mca_log_err("real_type is %d, bus_volt is %d", real_type, volt);

		if (connector_antiburn_is_qc(real_type)) {
			platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER,
								&bus_volt);
			volt = bus_volt / 1000;
			if (bus_volt < ANTIBURN_VBUS_HIGH_UV)
				return;
			msleep(ANTIBURN_VSAFE0V_SETTLE_MS);
			continue;
		}

		if (!connector_antiburn_is_pd(real_type))
			return;

		protocol_class_pd_request_vdm_cmd(TYPEC_PORT_0,
						  USBPD_UVDM_RESET_VSAFE0V,
						  &data_out, 0);
		msleep(ANTIBURN_VSAFE0V_SETTLE_MS);
		platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER,
							&bus_volt);
		volt = bus_volt / 1000;
		if (bus_volt < ANTIBURN_VBUS_HIGH_UV) {
			if (bus_volt < ANTIBURN_VBUS_SAFE0V_UV) {
				conn->is_reset_vsafe0V = 1;
				adapter_reset_vsafe0V_uevent(1);
			}
			return;
		}
	}
}

/*
 * Cut the port: tell userspace, get the adapter's voltage down, stop the OTG
 * boost if the port is sourcing, then open the MOS and check VBUS went away.
 */
static void connector_antiburn_trigger(struct connector_antiburn *conn,
				       int temp)
{
	u32 data_out = 0;
	int bus_volt = 0;
	int real_type;

	conn->triggered = 1;
	conn->ntc_alarm = 1;
	connector_temp_uevent(temp * 10);
	connector_ntc_alarm_uevent(conn->ntc_alarm);

	real_type = connector_antiburn_get_real_type();
	mca_log_info("real_type:%d\n", real_type);

	if (connector_antiburn_is_pd(real_type)) {
		protocol_class_pd_request_vdm_cmd(TYPEC_PORT_0,
						  USBPD_UVDM_RESET_VSAFE0V,
						  &data_out, 0);
		msleep(ANTIBURN_TRIGGER_STEP_MS);
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_CONN_ANTIBURN_CHANGE, NULL);
		mca_charge_mievent_report(CHARGE_DFX_ANTI_BURN_TRIGGERED,
					  &temp, 1);
		msleep(ANTIBURN_VSAFE0V_SETTLE_MS);
		connector_antiburn_ensure_vbus_sense5V(conn);
		platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER,
							&bus_volt);
		if (bus_volt < ANTIBURN_VBUS_SAFE0V_UV) {
			conn->is_reset_vsafe0V = 1;
			adapter_reset_vsafe0V_uevent(1);
		}
	} else {
		mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
				       MCA_EVENT_CONN_ANTIBURN_CHANGE, NULL);
		mca_charge_mievent_report(CHARGE_DFX_ANTI_BURN_TRIGGERED,
					  &temp, 1);
		msleep(ANTIBURN_VSAFE0V_SETTLE_MS);
		connector_antiburn_ensure_vbus_sense5V(conn);
	}

	if (conn->otg_plugin_status)
		platform_class_buckchg_ops_set_boost_enable(MAIN_BUCK_CHARGER,
				conn->otg_boost_src << 8 | conn->en_src << 16);

	if (conn->support_hw_antiburn) {
		gpiod_direction_output_raw(gpio_to_desc(conn->mos_ctrl_gpio), 1);
		mca_log_err("triggering hw anti burn");
		msleep(ANTIBURN_TRIGGER_STEP_MS);
		platform_class_buckchg_ops_get_bus_volt(MAIN_BUCK_CHARGER,
							&bus_volt);
		if (bus_volt >= ANTIBURN_VBUS_MOS_FAIL_UV)
			mca_charge_mievent_report(CHARGE_DFX_ANTIBURN_ERR,
						  NULL, 0);
	}
}

/*
 * Restore the port.  The NTC alarm stays raised until the cable is removed,
 * see connector_antiburn_clear_alarm().
 */
static void connector_antiburn_recover(struct connector_antiburn *conn,
				       int temp)
{
	mca_log_err("recovery antiburn conn->usb_online is %d, conn->otg_plugin_status is %d, conn->cid_status is %d\n",
		    conn->usb_online, conn->otg_plugin_status,
		    conn->cid_status);

	conn->triggered = 0;
	conn->is_reset_vsafe0V = 0;
	connector_temp_uevent(temp * 10);
	adapter_reset_vsafe0V_uevent(conn->is_reset_vsafe0V);

	if (conn->support_hw_antiburn) {
		gpiod_direction_output_raw(gpio_to_desc(conn->mos_ctrl_gpio), 0);
		mca_log_err("close hw anti burn");
	}

	mca_event_block_notify(MCA_EVENT_TYPE_HW_INFO,
			       MCA_EVENT_CONN_ANTIBURN_CHANGE, NULL);
	mca_charge_mievent_set_state(MIEVENT_STATE_END,
				     CHARGE_DFX_ANTI_BURN_TRIGGERED);
}

static void connector_antiburn_clear_alarm(struct connector_antiburn *conn,
					   int temp)
{
	if (conn->ntc_alarm != 1 || conn->cid_status)
		return;

	conn->ntc_alarm = 0;
	connector_temp_uevent(temp * 10);
	connector_ntc_alarm_uevent(conn->ntc_alarm);
}

static void connector_antiburn_update_port_status(struct connector_antiburn *conn)
{
	protocol_class_pd_get_otg_plugin_status(TYPEC_PORT_0,
						&conn->otg_plugin_status);
	platform_class_buckchg_ops_get_online(MAIN_BUCK_CHARGER,
					      &conn->usb_online);
	protocol_class_pd_get_cid_status(TYPEC_PORT_0, &conn->cid_status);
}

/*
 * Plain strategy, whole degrees, judged on the hotter thermistor.  The port is
 * cut while a cable is detected or a charger is online and the connector is
 * past trigger_temp, or past the combined threshold while the board is not as
 * warm, or past the rate threshold while climbing fast.
 */
static int connector_antiburn_check_status(struct connector_antiburn *conn)
{
	int t0 = conn->connector_temp[0];
	int t1 = conn->connector_temp[1];
	int temp = max(t0, t1);
	int rate = conn->temp_increase_rate[t0 < t1 ? 1 : 0];
	bool hot;

	if (temp <= ANTIBURN_TEMP_FLOOR)
		temp = ANTIBURN_TEMP_FLOOR;

	connector_antiburn_update_port_status(conn);
	mca_log_info("connector_temp:%d triggered:%d, cid_status:%d, otg_plugin_status: %d, usb_online:%d\n",
		     temp, conn->triggered, conn->cid_status,
		     conn->otg_plugin_status, conn->usb_online);

	hot = temp >= conn->trigger_temp ||
	      (temp >= conn->comb_sensorboard_con_trigger_temp &&
	       conn->thermal_board_temp <= conn->max_thermal_board_temp) ||
	      (temp >= conn->comb_rate_conn_trigger_temp &&
	       rate >= conn->max_temp_increase_rate);

	if (hot && !conn->disable_antiburn && !conn->triggered &&
	    (conn->cid_status || conn->usb_online)) {
		mca_log_err("triggering antiburn conn->usb_online is %d, conn->otg_plugin_status is %d, conn->cid_status is %d\n",
			    conn->usb_online, conn->otg_plugin_status,
			    conn->cid_status);
		mca_log_err("usb_online: %d, otg_plugin_status: %d, conn_therm: %d/%d, temp_increase_rate: %d, thermal_board_temp: %d\n",
			    conn->usb_online, conn->otg_plugin_status, t0, t1,
			    rate, conn->thermal_board_temp);
		connector_antiburn_trigger(conn, temp);
	} else if (temp < conn->recharge_temp &&
		   rate < conn->max_temp_increase_rate && conn->triggered &&
		   !conn->otg_plugin_status && !conn->cid_status) {
		connector_antiburn_recover(conn, temp);
	}

	connector_antiburn_clear_alarm(conn, temp);

	if (conn->otg_plugin_status || conn->usb_online == 1)
		return ANTIBURN_FAST_INTERVAL_MS;

	return ANTIBURN_IDLE_INTERVAL_MS;
}

/*
 * Elaborated strategy, millidegrees, each thermistor judged against its own
 * rate.  Above ANTIBURN_TEMP_ABS_MAX_MILLI the port is cut even with nothing
 * detected in it.
 */
static int connector_antiburn_check_status_v2(struct connector_antiburn *conn)
{
	static int not_attched_count;
	int t0 = conn->connector_temp[0];
	int t1 = conn->connector_temp[1];
	int r0 = conn->temp_increase_rate[0];
	int r1 = conn->temp_increase_rate[1];
	int temp = max(t0, t1);
	bool attached;
	bool hot;

	if (temp <= ANTIBURN_TEMP_FLOOR_MILLI)
		temp = ANTIBURN_TEMP_FLOOR_MILLI;

	connector_antiburn_update_port_status(conn);
	mca_log_info("connector_temp:%d:%d, temp_increase_rate:%d:%d, triggered:%d, cid_status:%d, otg_plugin_status: %d, usb_online:%d\n",
		     t0, t1, r0, r1, conn->triggered, conn->cid_status,
		     conn->otg_plugin_status, conn->usb_online);

	attached = conn->cid_status || conn->usb_online;
	hot = temp >= conn->trigger_temp ||
	      (temp >= conn->comb_sensorboard_con_trigger_temp &&
	       conn->thermal_board_temp <= conn->max_thermal_board_temp) ||
	      (t0 >= conn->comb_rate_conn_trigger_temp &&
	       r0 >= conn->max_temp_increase_rate) ||
	      (t1 >= conn->comb_rate_conn_trigger_temp &&
	       r1 >= conn->max_temp_increase_rate);

	if (!conn->disable_antiburn && !conn->triggered &&
	    (temp >= ANTIBURN_TEMP_ABS_MAX_MILLI || (attached && hot))) {
		mca_log_err("triggering antiburn conn->usb_online is %d, conn->otg_plugin_status is %d, conn->cid_status is %d\n",
			    conn->usb_online, conn->otg_plugin_status,
			    conn->cid_status);
		mca_log_err("usb_online: %d, otg_plugin_status: %d, conn_therm: %d/%d, temp_increase_rate: %d, thermal_board_temp: %d\n",
			    conn->usb_online, conn->otg_plugin_status, t0, t1,
			    0, conn->thermal_board_temp);
		connector_antiburn_trigger(conn, temp);
	} else if (t0 < conn->recharge_temp &&
		   r0 < conn->max_temp_increase_rate &&
		   t1 < conn->recharge_temp &&
		   r1 < conn->max_temp_increase_rate &&
		   conn->triggered && !conn->otg_plugin_status &&
		   !conn->cid_status) {
		connector_antiburn_recover(conn, temp);
	}

	connector_antiburn_clear_alarm(conn, temp);

	if (!conn->otg_plugin_status && conn->usb_online != 1 &&
	    !conn->cid_status &&
	    conn->connector_temp[0] < ANTIBURN_IDLE_TEMP_MAX &&
	    conn->connector_temp[1] < ANTIBURN_IDLE_TEMP_MAX)
		return not_attched_count++ < ANTIBURN_IDLE_CYCLES ?
		       ANTIBURN_FAST_INTERVAL_MS : ANTIBURN_IDLE_INTERVAL_MS;

	not_attched_count = 0;

	return ANTIBURN_FAST_INTERVAL_MS;
}

static void connector_antiburn_monitor_workfunc(struct work_struct *work)
{
	struct connector_antiburn *conn = container_of(to_delayed_work(work),
						       struct connector_antiburn,
						       monitor_work);

	connector_antiburn_get_temp_increase_rate(conn);

	if (conn->support_elaboration_anti_strategy)
		conn->monitor_interval = connector_antiburn_check_status_v2(conn);
	else
		conn->monitor_interval = connector_antiburn_check_status(conn);

	mca_queue_delayed_work(&conn->monitor_work,
			       msecs_to_jiffies(conn->monitor_interval));
}

static int connector_antiburn_thermal_notifier_event(struct notifier_block *nb,
						     unsigned long event,
						     void *data)
{
	struct connector_antiburn *conn = container_of(nb,
						       struct connector_antiburn,
						       thermal_board_nb);

	if (event != MCA_EVENT_THERMAL_BOARD_TEMP_CHANGE) {
		mca_log_info("not supported thermal board notifier event: %lu\n",
			     event);
		return NOTIFY_DONE;
	}

	conn->thermal_board_temp = *(int *)data / 1000;
	mca_log_info("get thermal_board_temp: %d\n", conn->thermal_board_temp);

	return NOTIFY_DONE;
}

static int connector_antiburn_debug_notifier_cb(struct notifier_block *nb,
						unsigned long event,
						void *data)
{
	struct connector_antiburn *conn = container_of(nb,
						       struct connector_antiburn,
						       debug_nb);

	/* 85/85 soak, lifted temperature limits and memory soak runs */
	if (event < MCA_EVENT_DEBUG_CTRL_DOUBLE85 ||
	    event > MCA_EVENT_DEBUG_CTRL_MEMORY_TEST)
		return NOTIFY_DONE;

	conn->disable_antiburn = *(int *)data;
	mca_log_info("debug[%lu] disable_antiburn: %d\n", event,
		     conn->disable_antiburn);

	return NOTIFY_DONE;
}

/* A charger plugged in drops the polling back to the fast period at once. */
static int connector_antiburn_connect_cb(struct notifier_block *nb,
					 unsigned long event, void *data)
{
	struct connector_antiburn *conn = container_of(nb,
						       struct connector_antiburn,
						       connect_nb);

	if (!conn->support_elaboration_anti_strategy)
		return NOTIFY_DONE;

	if (conn->monitor_interval == ANTIBURN_FAST_INTERVAL_MS) {
		mca_log_info("usb connect, monitor_work already quick, return...");
		return NOTIFY_DONE;
	}

	if (event != MCA_EVENT_USB_CONNECT)
		return NOTIFY_DONE;

	mca_log_info("usb connect, run monitor_work now");
	cancel_delayed_work_sync(&conn->monitor_work);
	mca_queue_delayed_work(&conn->monitor_work,
			       msecs_to_jiffies(ANTIBURN_KICK_DELAY_MS));
	conn->monitor_interval = ANTIBURN_FAST_INTERVAL_MS;

	return NOTIFY_DONE;
}

/* And so does the port starting to source, or a cable being sensed. */
static int connector_antiburn_hw_cb(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	struct connector_antiburn *conn = container_of(nb,
						       struct connector_antiburn,
						       hw_nb);
	int value;

	if (!conn->support_elaboration_anti_strategy)
		return NOTIFY_DONE;

	if (conn->monitor_interval == ANTIBURN_FAST_INTERVAL_MS) {
		mca_log_info("cid/otg connect, monitor_work already quick, return...");
		return NOTIFY_DONE;
	}

	switch (event) {
	case MCA_EVENT_BOOST_STS:
		value = *(int *)data;
		mca_log_info("boost_sts = %d\n", value);
		if (value != 1)
			return NOTIFY_DONE;
		mca_log_info("otg connect, run monitor_work now");
		break;
	case MCA_EVENT_CID_STS:
		value = *(int *)data;
		mca_log_info("cid_sts = %d\n", value);
		if (value != 1)
			return NOTIFY_DONE;
		mca_log_info("cid_sts, run monitor_work now");
		break;
	default:
		return NOTIFY_DONE;
	}

	cancel_delayed_work_sync(&conn->monitor_work);
	mca_queue_delayed_work(&conn->monitor_work,
			       msecs_to_jiffies(ANTIBURN_KICK_DELAY_MS));
	conn->monitor_interval = ANTIBURN_FAST_INTERVAL_MS;

	return NOTIFY_DONE;
}

static ssize_t antiburn_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf);
static ssize_t antiburn_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count);

static struct mca_sysfs_attr_info antiburn_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(antiburn_sysfs, 0664, CONNECTOR_PROP_TEMP_1,
			  connector_temp_1),
	mca_sysfs_attr_rw(antiburn_sysfs, 0664, CONNECTOR_PROP_TEMP_2,
			  connector_temp_2),
	mca_sysfs_attr_rw(antiburn_sysfs, 0664, CONNECTOR_PROP_RESET_VSAFE0V,
			  reset_vsafe0V),
	mca_sysfs_attr_rw(antiburn_sysfs, 0664, CONNECTOR_PROP_NTC_ALARM,
			  ntc_alarm),
	mca_sysfs_attr_rw(antiburn_sysfs, 0664, CONNECTOR_PROP_MOS_CTRL,
			  mos_ctrl),
};

static struct attribute *antiburn_sysfs_attrs[
	ARRAY_SIZE(antiburn_sysfs_field_tbl) + 1];

static const struct attribute_group antiburn_sysfs_attr_group = {
	.attrs = antiburn_sysfs_attrs,
};

static ssize_t antiburn_sysfs_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct connector_antiburn *conn = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *info;
	int value;

	info = mca_sysfs_lookup_attr(attr->attr.name, antiburn_sysfs_field_tbl,
				     ARRAY_SIZE(antiburn_sysfs_field_tbl));
	if (!info)
		return -1;

	if (!conn) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	switch (info->sysfs_attr_name) {
	case CONNECTOR_PROP_TEMP_1:
	case CONNECTOR_PROP_TEMP_2:
		value = connector_antiburn_get_temperature(conn,
						info->sysfs_attr_name);
		/* tenths of a degree for the plain strategy */
		if (!conn->support_elaboration_anti_strategy)
			value *= 10;
		break;
	case CONNECTOR_PROP_RESET_VSAFE0V:
		value = conn->is_reset_vsafe0V;
		break;
	case CONNECTOR_PROP_NTC_ALARM:
		value = conn->ntc_alarm;
		break;
	case CONNECTOR_PROP_MOS_CTRL:
		if (!conn->support_hw_antiburn) {
			mca_log_err("not support_hw_antiburn\n");
			return 0;
		}
		value = gpiod_get_raw_value(gpio_to_desc(conn->mos_ctrl_gpio));
		mca_log_err("show mos_ctrl:%d\n", value);
		break;
	default:
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", value);
}

static ssize_t antiburn_sysfs_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct connector_antiburn *conn = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *info;
	int value = 0;

	info = mca_sysfs_lookup_attr(attr->attr.name, antiburn_sysfs_field_tbl,
				     ARRAY_SIZE(antiburn_sysfs_field_tbl));
	if (!info)
		return -1;

	if (!conn) {
		mca_log_err("%s dev_driverdata is null\n", __func__);
		return -1;
	}

	if (kstrtoint(buf, 10, &value))
		return -EINVAL;

	switch (info->sysfs_attr_name) {
	case CONNECTOR_PROP_TEMP_1:
	case CONNECTOR_PROP_TEMP_2:
		/* written in tenths of a degree, 0 stops forcing */
		conn->fake_connector_temp[info->sysfs_attr_name] = value / 10;
		mca_log_err("set the %d ntc = %d\n", info->sysfs_attr_name,
			    value);
		cancel_delayed_work_sync(&conn->monitor_work);
		mca_queue_delayed_work(&conn->monitor_work,
				       msecs_to_jiffies(ANTIBURN_KICK_DELAY_MS));
		break;
	case CONNECTOR_PROP_MOS_CTRL:
		if (!conn->support_hw_antiburn) {
			mca_log_err("not support_hw_antiburn\n");
			break;
		}
		if ((unsigned int)value < 2)
			gpiod_direction_output_raw(
				gpio_to_desc(conn->mos_ctrl_gpio), value);
		mca_log_err("set mos_ctrl:%d\n", value);
		break;
	default:
		break;
	}

	return count;
}

static int connector_antiburn_dump_log_head(void *data, char *buf, int size)
{
	return snprintf(buf, size, "port_temp port_temp1 shell_temp ");
}

static int connector_antiburn_dump_log_context(void *data, char *buf, int size)
{
	struct connector_antiburn *conn = data;
	int temp1 = -1;
	int temp2 = -1;
	int board = -1;

	if (conn) {
		temp1 = connector_antiburn_get_temperature(conn, 0);
		if (conn->use_double_ntc)
			temp2 = connector_antiburn_get_temperature(conn, 1);
		board = conn->thermal_board_temp;
	}

	return snprintf(buf, size, "%-10d%-11d%-11d", temp1, temp2, board);
}

static struct mca_log_charge_log_ops connector_antiburn_log_ops = {
	.dump_log_head = connector_antiburn_dump_log_head,
	.dump_log_context = connector_antiburn_dump_log_context,
};

static void connector_antiburn_parse_dt(struct connector_antiburn *conn)
{
	struct device_node *np = conn->dev->of_node;
	bool milli;

	/* decides whether the temperature defaults are degrees or millidegrees */
	mca_parse_dts_u32(np, "support_elaboration_anti_strategy",
			  (u32 *)&conn->support_elaboration_anti_strategy, 0);
	milli = conn->support_elaboration_anti_strategy;

	mca_parse_dts_u32(np, "trigger_temp", (u32 *)&conn->trigger_temp,
			  milli ? 65000 : 65);
	mca_parse_dts_u32(np, "recharge_temp", (u32 *)&conn->recharge_temp,
			  milli ? 55000 : 55);
	mca_parse_dts_u32(np, "support_soft_antiburn",
			  (u32 *)&conn->support_soft_antiburn, 1);
	mca_parse_dts_u32(np, "support_hw_antiburn",
			  (u32 *)&conn->support_hw_antiburn, 1);
	mca_parse_dts_u32(np, "use_double_ntc",
			  (u32 *)&conn->use_double_ntc, 0);
	mca_parse_dts_u32(np, "antiburn_otg_detect",
			  (u32 *)&conn->otg_detect_en, 1);
	mca_parse_dts_u32(np, "monitor_interval",
			  (u32 *)&conn->monitor_interval, 1000);
	mca_parse_dts_u32(np, "max_temp_increase_rate",
			  (u32 *)&conn->max_temp_increase_rate,
			  milli ? 4000 : 4);
	mca_parse_dts_string(np, "thermal-zone-name", &conn->thermal_zone_name);
	mca_parse_dts_string(np, "thermal-zone-name2",
			     &conn->thermal_zone_name2);
	mca_parse_dts_u32(np, "comb_sensorboard_con_trigger_temp",
			  (u32 *)&conn->comb_sensorboard_con_trigger_temp,
			  milli ? 60000 : 60);
	mca_parse_dts_u32(np, "comb_rate_conn_trigger_temp",
			  (u32 *)&conn->comb_rate_conn_trigger_temp,
			  milli ? 35000 : 35);
	mca_parse_dts_u32(np, "max_thermal_board_temp",
			  (u32 *)&conn->max_thermal_board_temp, 50);
	mca_parse_dts_u32(np, "otg_boost_src", (u32 *)&conn->otg_boost_src, 2);
	mca_parse_dts_u32(np, "en_src", (u32 *)&conn->en_src, 0);

	conn->support_base_flip = of_find_property(np, "support-base-flip",
						   NULL) ? 1 : 0;

	mca_log_err("%d:%d:%d:%d:%d:%d",
		    conn->support_elaboration_anti_strategy,
		    conn->trigger_temp, conn->recharge_temp,
		    conn->max_temp_increase_rate,
		    conn->comb_sensorboard_con_trigger_temp,
		    conn->comb_rate_conn_trigger_temp);
}

/* The MOS on VBUS, claimed and left closed (port connected). */
static int connector_antiburn_gpio_init(struct connector_antiburn *conn,
					bool *requested)
{
	int rc;

	mca_log_info("Hw antiburn init gpio\n");

	if (!conn->support_hw_antiburn) {
		mca_log_info("No gpio config\n");
		return -1;
	}

	conn->mos_ctrl_gpio = of_get_named_gpio(conn->dev->of_node,
						"mos-ctrl-gpio", 0);
	if (conn->mos_ctrl_gpio < 0)
		mca_log_err("failed to parse mos ctrl gpio\n");

	rc = gpio_request(conn->mos_ctrl_gpio, "mos-ctrl-gpio");
	if (rc) {
		mca_log_err("unable to request antiburn mos ctrl gpio ret is %d\n",
			    rc);
		return rc;
	}
	*requested = true;

	rc = gpiod_direction_output_raw(gpio_to_desc(conn->mos_ctrl_gpio), 0);
	if (rc) {
		mca_log_err("unable to set direction for pmic gpio\n");
		return rc;
	}

	return 0;
}

static int connector_antiburn_probe(struct platform_device *pdev)
{
	static int probe_cnt;
	struct mca_hwid_info *hwid;
	struct connector_antiburn *conn;
	bool gpio_requested = false;
	int rc;

	hwid = mca_get_hwid_info();
	mca_log_info("probe_cnt = %d\n", ++probe_cnt);
	if (!hwid)
		return -ENOMEM;

	/* P0.1 boards have no usable connector thermistor */
	if (hwid->platform_version == 1 && hwid->build_version == 0 &&
	    hwid->minor_version == 1) {
		mca_log_err("Do not support antiburn in %s P%d.%d\n",
			    hwid->product_name, 0, 1);
		return 0;
	}

	conn = devm_kzalloc(&pdev->dev, sizeof(*conn), GFP_KERNEL);
	if (!conn) {
		mca_log_err("out of memory\n");
		return -ENOMEM;
	}

	conn->dev = &pdev->dev;
	platform_set_drvdata(pdev, conn);

	connector_antiburn_parse_dt(conn);

	/* the early builds supply OTG from the external boost */
	if (hwid->platform_version == 1 &&
	    (hwid->build_version == 0 ||
	     (hwid->build_version == 1 && hwid->minor_version == 0))) {
		conn->otg_boost_src = 2;
		mca_log_err("start to use external boost in O2\n");
	}

	rc = connector_antiburn_gpio_init(conn, &gpio_requested);
	if (rc) {
		mca_log_err("connector_antiburn_gpio_init failed, err is %d\n",
			    rc);
		goto err;
	}

	conn->tzd_conn = thermal_zone_get_zone_by_name(conn->thermal_zone_name);
	if (IS_ERR(conn->tzd_conn)) {
		mca_log_err("thermal zone get conn_therm failed\n");
		goto err;
	}

	if (conn->use_double_ntc) {
		conn->tzd_conn2 =
			thermal_zone_get_zone_by_name(conn->thermal_zone_name2);
		if (IS_ERR(conn->tzd_conn2)) {
			mca_log_err("thermal zone get conn_therm2 failed\n");
			goto err;
		}
	}

	conn->thermal_board_nb.notifier_call =
		connector_antiburn_thermal_notifier_event;
	mca_event_block_notify_register(MCA_EVENT_TYPE_THERMAL_TEMP,
					&conn->thermal_board_nb);
	conn->debug_nb.notifier_call = connector_antiburn_debug_notifier_cb;
	mca_event_block_notify_register(MCA_EVENT_TYPE_DEBUG, &conn->debug_nb);
	conn->connect_nb.notifier_call = connector_antiburn_connect_cb;
	mca_event_block_notify_register(MCA_EVENT_TYPE_CHARGER_CONNECT,
					&conn->connect_nb);
	conn->hw_nb.notifier_call = connector_antiburn_hw_cb;
	mca_event_block_notify_register(MCA_EVENT_TYPE_HW_INFO, &conn->hw_nb);

	INIT_DELAYED_WORK(&conn->monitor_work,
			  connector_antiburn_monitor_workfunc);
	mca_queue_delayed_work(&conn->monitor_work,
			       msecs_to_jiffies(conn->monitor_interval));

	mca_log_charge_log_register(MCA_CHARGE_LOG_ID_USCP,
				    &connector_antiburn_log_ops, conn);

	mca_sysfs_init_attrs(antiburn_sysfs_attrs, antiburn_sysfs_field_tbl,
			     ARRAY_SIZE(antiburn_sysfs_field_tbl));
	mca_sysfs_create_link_group(MCA_SYSFS_DEV_HW_MONITOR, "connector",
				    conn->dev, &antiburn_sysfs_attr_group);

	conn->triggered = 0;
	g_conn = conn;
	conn->disable_antiburn = 0;

	mca_log_err("probe OK");

	return 0;

err:
	/* stock retries every failure, including a board without the MOS */
	if (gpio_requested)
		gpio_free(conn->mos_ctrl_gpio);
	devm_kfree(&pdev->dev, conn);

	return -EPROBE_DEFER;
}

static int connector_antiburn_remove(struct platform_device *pdev)
{
	struct connector_antiburn *conn = platform_get_drvdata(pdev);

	if (!conn)
		return 0;

	g_conn = NULL;
	cancel_delayed_work_sync(&conn->monitor_work);

	if (conn->mos_ctrl_gpio >= 0) {
		gpio_free(conn->mos_ctrl_gpio);
		mca_log_info("remove mos ctrl gpio success");
	}

	/* everything below points into conn, which is about to be freed */
	mca_sysfs_remove_link_group(MCA_SYSFS_DEV_HW_MONITOR, "connector",
				    conn->dev, &antiburn_sysfs_attr_group);
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_THERMAL_TEMP,
					  &conn->thermal_board_nb);
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_DEBUG,
					  &conn->debug_nb);
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_CHARGER_CONNECT,
					  &conn->connect_nb);
	mca_event_block_notify_unregister(MCA_EVENT_TYPE_HW_INFO,
					  &conn->hw_nb);

	devm_kfree(&pdev->dev, conn);

	return 0;
}

static void connector_antiburn_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id connector_antiburn_match_table[] = {
	{ .compatible = "xiaomi,connector_antiburn" },
	{},
};
MODULE_DEVICE_TABLE(of, connector_antiburn_match_table);

static struct platform_driver connector_antiburn_driver = {
	.driver = {
		.name = "connector_antiburn",
		.of_match_table = connector_antiburn_match_table,
	},
	.probe = connector_antiburn_probe,
	.remove = connector_antiburn_remove,
	.shutdown = connector_antiburn_shutdown,
};
module_platform_driver(connector_antiburn_driver);

MODULE_DESCRIPTION("Connector antiburn");
MODULE_AUTHOR("muxinyi1@xiaomi.com");
MODULE_LICENSE("GPL v2");
