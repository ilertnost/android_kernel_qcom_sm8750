// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Kernel side of Xiaomi's thermal daemon (mi_thermald). It publishes
 * /sys/class/thermal/thermal_message, through which the daemon
 *  - caps CPU frequencies ("cpuN <kHz>" written to cpu_limits),
 *  - hands its thermal scene and board temperature to the charging stack
 *    (sconfig, board_sensor_temp),
 *  - follows the screen and USB state (screen_state, usb_online, which
 *    are notified on change),
 *  - and exchanges a set of plain values with other components (camera,
 *    display, modem and WLAN limits, sensor readings, ...).
 * /sys/class/thermal/power_save holds the power save mode and level.
 */

#define pr_fmt(fmt) "mi_thermal_interface: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm_qos.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/soc/qcom/panel_event_notifier.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>
#include <drm/drm_panel.h>

#define MI_THERMAL_STR_LEN		128
#define SCREEN_CHECK_DELAY		(5 * HZ)
#define SCREEN_CHECK_RETRIES		10

#if IS_ENABLED(CONFIG_MI_THERMAL_INTERFACE_MCA)
/* Provided by the MCA charging stack */
int mca_smartchg_set_scene(int scene);
void mca_event_block_notify(int type, unsigned long event, void *data);

#define MCA_EVENT_TYPE_THERMAL		6
#define MCA_EVENT_BOARD_TEMP		62
#endif

struct mi_thermal_cpu {
	struct list_head node;
	/* CPU owning the cpufreq policy, as matched by cpu_limits */
	unsigned int cpu;
	/* Distinct frequencies of the CPU, highest first */
	unsigned int *freqs;
	unsigned int num_freqs;
	unsigned int cur_level;
	struct cpufreq_policy *policy;
	struct freq_qos_request qos_req;
};

struct mi_thermal_str_attr {
	struct device_attribute attr;
	char value[MI_THERMAL_STR_LEN];
};

static LIST_HEAD(cpu_list);
static DEFINE_MUTEX(cpu_list_lock);

static struct device *thermal_message_dev;
static struct device *power_save_dev;

static const char *board_sensor;

static struct workqueue_struct *screen_state_wq;
static struct delayed_work screen_state_dw;
static int screen_check_retries = SCREEN_CHECK_RETRIES;
static void *panel_cookie;
static bool screen_light;
static int screen_state;
static int screen_last_state;

static struct work_struct usb_online_work;
static struct notifier_block usb_online_nb;
static struct power_supply *usb_psy;
static int usb_online;

/* Values exchanged with userspace as is */
static int temp_state;
static int balance_mode;
static int modem_limit;
static int wifi_limit;
static int flash_state;
static int torch_level;
static int torch_real_level = -1;
static int thermal_max_brightness;
static int display_therm_temp;
static int voice_limit;
static int super_hdr;
static int cloud_game;
static int dynamic_tj;
static int market_download_limit;
static int board_sensor_temp_comp;
static int poor_modem_limit;
static int cpu_nolimit_temp;
static int ntn_limit;
static int modem_level;
static int boot_complete;
static int charger_temp = -1;
static int sconfig = -1;
static int powersave_mode = -1;
static int power_level = -1;

static int mi_thermal_cmp_desc(const void *a, const void *b)
{
	unsigned int x = *(const unsigned int *)a;
	unsigned int y = *(const unsigned int *)b;

	return x < y ? 1 : x > y ? -1 : 0;
}

static int mi_thermal_add_cpu(unsigned int cpu)
{
	struct cpufreq_frequency_table *pos;
	struct cpufreq_policy *policy;
	struct mi_thermal_cpu *tcpu;
	unsigned int count = 0, i, n;
	int ret;

	policy = cpufreq_cpu_get(cpu);
	if (!policy) {
		pr_err("%s: cpufreq policy not found for cpu%d\n", __func__, cpu);
		return -ENODEV;
	}

	if (!policy->freq_table) {
		ret = -EINVAL;
		goto put_policy;
	}

	cpufreq_for_each_valid_entry(pos, policy->freq_table)
		count++;
	if (!count) {
		ret = -EINVAL;
		goto put_policy;
	}

	tcpu = kzalloc(sizeof(*tcpu), GFP_KERNEL);
	if (!tcpu) {
		ret = -ENOMEM;
		goto put_policy;
	}

	tcpu->freqs = kcalloc(count, sizeof(*tcpu->freqs), GFP_KERNEL);
	if (!tcpu->freqs) {
		ret = -ENOMEM;
		goto free_tcpu;
	}

	n = 0;
	cpufreq_for_each_valid_entry(pos, policy->freq_table)
		tcpu->freqs[n++] = pos->frequency;
	sort(tcpu->freqs, n, sizeof(*tcpu->freqs), mi_thermal_cmp_desc, NULL);

	/* Drop duplicates */
	for (i = 1, count = 1; i < n; i++) {
		if (tcpu->freqs[i] == tcpu->freqs[count - 1]) {
			pr_warn("%s: table has duplicate entries\n", __func__);
			continue;
		}
		tcpu->freqs[count++] = tcpu->freqs[i];
	}

	tcpu->cpu = policy->cpu;
	tcpu->num_freqs = count;
	tcpu->policy = policy;

	ret = freq_qos_add_request(&policy->constraints, &tcpu->qos_req,
				   FREQ_QOS_MAX, tcpu->freqs[0]);
	if (ret < 0) {
		pr_err("%s: Failed to add freq constraint (%d)\n", __func__, ret);
		goto free_freqs;
	}

	mutex_lock(&cpu_list_lock);
	list_add(&tcpu->node, &cpu_list);
	mutex_unlock(&cpu_list_lock);

	return 0;

free_freqs:
	kfree(tcpu->freqs);
free_tcpu:
	kfree(tcpu);
put_policy:
	cpufreq_cpu_put(policy);
	return ret;
}

static void mi_thermal_remove_cpus(void)
{
	struct mi_thermal_cpu *tcpu, *tmp;

	mutex_lock(&cpu_list_lock);
	list_for_each_entry_safe(tcpu, tmp, &cpu_list, node) {
		list_del(&tcpu->node);
		freq_qos_remove_request(&tcpu->qos_req);
		cpufreq_cpu_put(tcpu->policy);
		kfree(tcpu->freqs);
		kfree(tcpu);
	}
	mutex_unlock(&cpu_list_lock);
}

/* Cap the CPU to the highest frequency not above max_freq */
static void mi_thermal_set_cpu_limit(unsigned int cpu, unsigned int max_freq)
{
	struct mi_thermal_cpu *tcpu;
	unsigned int level;

	mutex_lock(&cpu_list_lock);
	list_for_each_entry(tcpu, &cpu_list, node) {
		if (tcpu->cpu != cpu)
			continue;

		for (level = 0; level < tcpu->num_freqs; level++)
			if (tcpu->freqs[level] <= max_freq)
				break;

		if (level < tcpu->num_freqs && level != tcpu->cur_level) {
			tcpu->cur_level = level;
			freq_qos_update_request(&tcpu->qos_req,
						tcpu->freqs[level]);
		}
		break;
	}
	mutex_unlock(&cpu_list_lock);
}

/*
 * Integers written by userspace are decimal. Like the original interface,
 * every write is accepted and anything that does not parse reads as 0.
 */
static int mi_thermal_parse_int(const char *buf)
{
	int val;

	if (kstrtoint(buf, 10, &val))
		return 0;

	return val;
}

static ssize_t cpu_limits_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t cpu_limits_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int cpu, max_freq;

	if (sscanf(buf, "cpu%u %u", &cpu, &max_freq) != 2) {
		pr_err("input param error, can not prase param\n");
		return -EINVAL;
	}

	mi_thermal_set_cpu_limit(cpu, max_freq);

	return count;
}
static DEVICE_ATTR_RW(cpu_limits);

static ssize_t sconfig_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(sconfig));
}

static ssize_t sconfig_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	int val = mi_thermal_parse_int(buf);

	WRITE_ONCE(sconfig, val);
#if IS_ENABLED(CONFIG_MI_THERMAL_INTERFACE_MCA)
	mca_smartchg_set_scene(val);
#endif

	return count;
}
static DEVICE_ATTR(sconfig, 0664, sconfig_show, sconfig_store);

static ssize_t screen_state_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(screen_state));
}
static DEVICE_ATTR(screen_state, 0664, screen_state_show, NULL);

static ssize_t usb_online_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(usb_online));
}
static DEVICE_ATTR(usb_online, 0664, usb_online_show, NULL);

static ssize_t board_sensor_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s", board_sensor ?: "invalid");
}
static DEVICE_ATTR(board_sensor, 0664, board_sensor_show, NULL);

static ssize_t ambient_sensor_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "ABT-SENSOR");
}
static DEVICE_ATTR(ambient_sensor, 0664, ambient_sensor_show, NULL);

/* String values are kept as written, trailing newline included */
static ssize_t mi_thermal_str_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct mi_thermal_str_attr *sa = container_of(attr,
					struct mi_thermal_str_attr, attr);

	return sysfs_emit(buf, "%s", sa->value);
}

static ssize_t mi_thermal_str_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct mi_thermal_str_attr *sa = container_of(attr,
					struct mi_thermal_str_attr, attr);

	strscpy(sa->value, buf, sizeof(sa->value));

	return count;
}

#define MI_THERMAL_STR_ATTR(_name, _mode)				\
	struct mi_thermal_str_attr dev_attr_##_name = {			\
		.attr = __ATTR(_name, _mode, mi_thermal_str_show,	\
			       mi_thermal_str_store),			\
	}

static MI_THERMAL_STR_ATTR(boost, 0644);
static MI_THERMAL_STR_ATTR(board_sensor_second_temp, 0664);
static MI_THERMAL_STR_ATTR(board_sensor_other_temp, 0664);
static MI_THERMAL_STR_ATTR(ambient_sensor_temp, 0664);

static ssize_t board_sensor_temp_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	int temp = mi_thermal_parse_int(buf);

	mi_thermal_str_store(dev, attr, buf, count);

	/* Forward the board temperature to the charging stack */
#if IS_ENABLED(CONFIG_MI_THERMAL_INTERFACE_MCA)
	mca_event_block_notify(MCA_EVENT_TYPE_THERMAL, MCA_EVENT_BOARD_TEMP,
			       &temp);
#endif

	return count;
}

static struct mi_thermal_str_attr dev_attr_board_sensor_temp = {
	.attr = __ATTR(board_sensor_temp, 0664, mi_thermal_str_show,
		       board_sensor_temp_store),
};

static ssize_t mi_thermal_int_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *ea = container_of(attr,
					struct dev_ext_attribute, attr);

	return sysfs_emit(buf, "%d\n", READ_ONCE(*(int *)ea->var));
}

static ssize_t mi_thermal_int_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct dev_ext_attribute *ea = container_of(attr,
					struct dev_ext_attribute, attr);

	WRITE_ONCE(*(int *)ea->var, mi_thermal_parse_int(buf));

	return count;
}

#define MI_THERMAL_INT_ATTR(_name, _var)				\
	static struct dev_ext_attribute dev_attr_##_name = {		\
		.attr = __ATTR(_name, 0664, mi_thermal_int_show,	\
			       mi_thermal_int_store),			\
		.var = &(_var),						\
	}

MI_THERMAL_INT_ATTR(temp_state, temp_state);
MI_THERMAL_INT_ATTR(balance_mode, balance_mode);
MI_THERMAL_INT_ATTR(modem_limit, modem_limit);
MI_THERMAL_INT_ATTR(wifi_limit, wifi_limit);
MI_THERMAL_INT_ATTR(flash_state, flash_state);
MI_THERMAL_INT_ATTR(torch_level, torch_level);
MI_THERMAL_INT_ATTR(torch_real_level, torch_real_level);
MI_THERMAL_INT_ATTR(thermal_max_brightness, thermal_max_brightness);
MI_THERMAL_INT_ATTR(display_therm_temp, display_therm_temp);
MI_THERMAL_INT_ATTR(voice_limit, voice_limit);
MI_THERMAL_INT_ATTR(super_hdr, super_hdr);
MI_THERMAL_INT_ATTR(cloud_game, cloud_game);
MI_THERMAL_INT_ATTR(dynamic_tj, dynamic_tj);
MI_THERMAL_INT_ATTR(market_download_limit, market_download_limit);
MI_THERMAL_INT_ATTR(board_sensor_temp_comp, board_sensor_temp_comp);
MI_THERMAL_INT_ATTR(poor_modem_limit, poor_modem_limit);
MI_THERMAL_INT_ATTR(cpu_nolimit_temp, cpu_nolimit_temp);
MI_THERMAL_INT_ATTR(ntn_limit, ntn_limit);
MI_THERMAL_INT_ATTR(modem_level, modem_level);
MI_THERMAL_INT_ATTR(boot_complete, boot_complete);
MI_THERMAL_INT_ATTR(charger_temp, charger_temp);
MI_THERMAL_INT_ATTR(powersave_mode, powersave_mode);
MI_THERMAL_INT_ATTR(power_level, power_level);

static struct attribute *thermal_message_attrs[] = {
	&dev_attr_temp_state.attr.attr,
	&dev_attr_cpu_limits.attr,
	&dev_attr_sconfig.attr,
	&dev_attr_screen_state.attr,
	&dev_attr_boost.attr.attr,
	&dev_attr_board_sensor.attr,
	&dev_attr_board_sensor_temp.attr.attr,
	&dev_attr_board_sensor_second_temp.attr.attr,
	&dev_attr_board_sensor_other_temp.attr.attr,
	&dev_attr_balance_mode.attr.attr,
	&dev_attr_modem_limit.attr.attr,
	&dev_attr_wifi_limit.attr.attr,
	&dev_attr_flash_state.attr.attr,
	&dev_attr_torch_level.attr.attr,
	&dev_attr_torch_real_level.attr.attr,
	&dev_attr_thermal_max_brightness.attr.attr,
	&dev_attr_display_therm_temp.attr.attr,
	&dev_attr_voice_limit.attr.attr,
	&dev_attr_super_hdr.attr.attr,
	&dev_attr_cloud_game.attr.attr,
	&dev_attr_dynamic_tj.attr.attr,
	&dev_attr_market_download_limit.attr.attr,
	&dev_attr_board_sensor_temp_comp.attr.attr,
	&dev_attr_poor_modem_limit.attr.attr,
	&dev_attr_cpu_nolimit_temp.attr.attr,
	&dev_attr_ntn_limit.attr.attr,
	&dev_attr_modem_level.attr.attr,
	&dev_attr_ambient_sensor.attr,
	&dev_attr_ambient_sensor_temp.attr.attr,
	&dev_attr_boot_complete.attr.attr,
	&dev_attr_charger_temp.attr.attr,
	&dev_attr_usb_online.attr,
	NULL
};
ATTRIBUTE_GROUPS(thermal_message);

static struct attribute *power_save_attrs[] = {
	&dev_attr_powersave_mode.attr.attr,
	&dev_attr_power_level.attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(power_save);

static void screen_state_for_thermal_callback(enum panel_event_notifier_tag tag,
		struct panel_event_notification *notification, void *data)
{
	if (!notification) {
		pr_err("%s:Invalid notification\n", __func__);
		return;
	}

	if (notification->notif_data.early_trigger)
		return;

	if (tag == PANEL_EVENT_NOTIFICATION_PRIMARY) {
		switch (notification->notif_type) {
		case DRM_PANEL_EVENT_UNBLANK:
			screen_light = true;
			break;
		case DRM_PANEL_EVENT_BLANK:
		case DRM_PANEL_EVENT_BLANK_LP:
			screen_light = false;
			break;
		default:
			return;
		}
	}

	WRITE_ONCE(screen_state, screen_light);
	if (screen_state != screen_last_state) {
		sysfs_notify(&thermal_message_dev->kobj, NULL, "screen_state");
		screen_last_state = screen_state;
	}
}

static struct drm_panel *mi_thermal_find_panel(struct device_node *np)
{
	struct drm_panel *panel;
	struct device_node *node;
	int i, count;

	count = of_count_phandle_with_args(np, "panel", NULL);
	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, "panel", i);
		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR(panel))
			return panel;
	}

	return NULL;
}

/* The panel shows up late, keep looking for it for a while */
static void screen_state_check(struct work_struct *work)
{
	struct drm_panel *panel;
	struct device_node *np;

	np = of_find_node_by_name(NULL, "thermal-screen");
	if (!np) {
		pr_err("%s ERROR: Cannot find node with panel!", __func__);
		return;
	}

	panel = mi_thermal_find_panel(np);
	of_node_put(np);

	if (!panel) {
		if (screen_check_retries-- > 0) {
			pr_err("%s:prim_panel is NULL Failed to register for panel events\n",
			       __func__);
			queue_delayed_work(screen_state_wq, &screen_state_dw,
					   SCREEN_CHECK_DELAY);
		}
		return;
	}

	panel_cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,
					PANEL_EVENT_NOTIFIER_CLIENT_THERMAL, panel,
					screen_state_for_thermal_callback, NULL);
	if (IS_ERR(panel_cookie)) {
		pr_err("%s:Failed to register for prim_panel events\n", __func__);
		panel_cookie = NULL;
	}
}

static void usb_online_work_fn(struct work_struct *work)
{
	union power_supply_propval val = { 0 };
	int ret;

	if (!usb_psy)
		usb_psy = power_supply_get_by_name("usb");
	if (!usb_psy)
		return;

	ret = power_supply_get_property(usb_psy, POWER_SUPPLY_PROP_ONLINE, &val);
	if (ret) {
		pr_err("usb online read error:%d\n", ret);
		return;
	}

	WRITE_ONCE(usb_online, val.intval);
	sysfs_notify(&thermal_message_dev->kobj, NULL, "usb_online");
}

static int usb_online_callback(struct notifier_block *nb, unsigned long event,
			       void *data)
{
	struct power_supply *psy = data;

	if (!strcmp(psy->desc->name, "usb"))
		schedule_work(&usb_online_work);

	return NOTIFY_OK;
}

static struct device *mi_thermal_create_dev(const char *name,
					    const struct attribute_group **groups)
{
	const struct class *thermal_class = thermal_class_get();

	if (!thermal_class)
		return ERR_PTR(-ENODEV);

	return device_create_with_groups(thermal_class, NULL, MKDEV(0, 0), NULL,
					 groups, name);
}

static int __init mi_thermal_interface_init(void)
{
	struct device_node *np;
	unsigned int cpu;
	int ret;

	/* CPU limits are only available for the CPUs set up so far */
	for_each_possible_cpu(cpu)
		if (mi_thermal_add_cpu(cpu))
			break;

	np = of_find_node_by_name(NULL, "thermal-message");
	if (np) {
		if (!of_property_read_string(np, "board-sensor", &board_sensor))
			pr_info("board sensor: %s\n", board_sensor);
		of_node_put(np);
	}

	thermal_message_dev = mi_thermal_create_dev("thermal_message",
						    thermal_message_groups);
	if (IS_ERR(thermal_message_dev)) {
		ret = PTR_ERR(thermal_message_dev);
		pr_err("Cannot create thermal_message: %d\n", ret);
		goto remove_cpus;
	}

	power_save_dev = mi_thermal_create_dev("power_save", power_save_groups);
	if (IS_ERR(power_save_dev)) {
		pr_err("Cannot create power_save: %ld\n", PTR_ERR(power_save_dev));
		power_save_dev = NULL;
	}

	INIT_WORK(&usb_online_work, usb_online_work_fn);
	usb_online_nb.notifier_call = usb_online_callback;
	ret = power_supply_reg_notifier(&usb_online_nb);
	if (ret < 0)
		pr_err("usb online notifier registration error. return: %d\n", ret);

	screen_state_wq = create_singlethread_workqueue("screen_state_wq");
	if (screen_state_wq) {
		INIT_DELAYED_WORK(&screen_state_dw, screen_state_check);
		queue_delayed_work(screen_state_wq, &screen_state_dw,
				   SCREEN_CHECK_DELAY);
	}

	return 0;

remove_cpus:
	mi_thermal_remove_cpus();
	return ret;
}
module_init(mi_thermal_interface_init);

static void __exit mi_thermal_interface_exit(void)
{
	if (screen_state_wq) {
		cancel_delayed_work_sync(&screen_state_dw);
		destroy_workqueue(screen_state_wq);
	}

	if (panel_cookie)
		panel_event_notifier_unregister(panel_cookie);

	power_supply_unreg_notifier(&usb_online_nb);
	cancel_work_sync(&usb_online_work);
	if (usb_psy)
		power_supply_put(usb_psy);

	if (power_save_dev)
		device_unregister(power_save_dev);
	device_unregister(thermal_message_dev);

	mi_thermal_remove_cpus();
}
module_exit(mi_thermal_interface_exit);

MODULE_DESCRIPTION("Xiaomi thermal control  interface");
MODULE_LICENSE("GPL v2");
