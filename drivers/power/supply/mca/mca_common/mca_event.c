// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Events shared between the charging drivers.  See
 * include/mca/common/mca_event.h.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/string.h>

#include <mca/common/mca_event.h>
#include <mca/common/mca_sysfs.h>

/* Size of the uevent buffer; the event string must leave room for its NUL. */
#define MCA_EVENT_UEVENT_MAX	128

/* Where the trigger attribute appears. */
#define MCA_EVENT_CLASS_NAME	"xm_power"
#define MCA_EVENT_DEV_NAME	"mca_event"

/* One chain per type, so a listener only sees what it asked for. */
static struct blocking_notifier_head g_mca_event_bnh[MCA_EVENT_TYPE_END];

/*
 * Announcements are serialised: listeners react by talking to hardware, and
 * two announcements running at once would have two of them driving the same
 * charger from different threads.
 */
static DEFINE_MUTEX(g_mca_event_block_notify_mutex);

/* The xm_power/mca_event device uevents are sent from. */
static struct device *g_mca_event_dev;

/**
 * mca_event_block_notify_register() - listen for events of one type
 * @type: which events to receive
 * @nb:   the caller's notifier block
 *
 * Return: 0, or the notifier core's error.  An invalid @type or @nb is
 * ignored.
 */
int mca_event_block_notify_register(enum mca_event_notify_type type,
				    struct notifier_block *nb)
{
	if (type >= MCA_EVENT_TYPE_END || !nb)
		return 0;

	return blocking_notifier_chain_register(&g_mca_event_bnh[type], nb);
}
EXPORT_SYMBOL(mca_event_block_notify_register);

/**
 * mca_event_block_notify_unregister() - stop listening
 * @type: the type registered for
 * @nb:   the caller's notifier block
 */
int mca_event_block_notify_unregister(enum mca_event_notify_type type,
				      struct notifier_block *nb)
{
	if (type >= MCA_EVENT_TYPE_END || !nb)
		return 0;

	return blocking_notifier_chain_unregister(&g_mca_event_bnh[type], nb);
}
EXPORT_SYMBOL(mca_event_block_notify_unregister);

/**
 * mca_event_block_notify() - announce an event
 * @type:  what kind of event this is
 * @event: the event itself
 * @data:  passed to the listeners, may be NULL
 *
 * Listeners run in the caller's context and may sleep, so this cannot be
 * called from atomic context.
 */
void mca_event_block_notify(enum mca_event_notify_type type,
			    unsigned long event, void *data)
{
	if (type >= MCA_EVENT_TYPE_END)
		return;

	mutex_lock(&g_mca_event_block_notify_mutex);

	pr_info("receive blocking event type=%u event=%lu\n", type, event);

	blocking_notifier_call_chain(&g_mca_event_bnh[type], event, data);

	mutex_unlock(&g_mca_event_block_notify_mutex);
}
EXPORT_SYMBOL(mca_event_block_notify);

/**
 * mca_event_report_uevent() - send an event to userspace
 * @n_data: the environment string, "KEY=value", and its length
 */
void mca_event_report_uevent(const struct mca_event_notify_data *n_data)
{
	char buf[MCA_EVENT_UEVENT_MAX] = { 0 };
	char *envp[2] = { buf, NULL };
	int ret;

	if (!g_mca_event_dev) {
		pr_err("l_dev or sysfs_ne is null\n");
		return;
	}

	if (!n_data || !n_data->event) {
		pr_err("n_data or event is null\n");
		return;
	}

	if (n_data->event_len < 0 ||
	    n_data->event_len >= MCA_EVENT_UEVENT_MAX) {
		pr_err("event_len is invalid\n");
		return;
	}

	memcpy(buf, n_data->event, n_data->event_len);

	pr_info("receive uevent_buf %d,%s\n", n_data->event_len, buf);

	ret = kobject_uevent_env(&g_mca_event_dev->kobj, KOBJ_CHANGE, envp);
	if (ret < 0)
		pr_err("notify uevent fail, ret=%d\n", ret);
}
EXPORT_SYMBOL(mca_event_report_uevent);

/* What userspace can write here. */
enum mca_event_attr_list {
	MCA_EVENT_ATTR_TRIGGER,
};

static ssize_t mca_event_sysfs_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count);

static struct mca_sysfs_attr_info mca_event_sysfs_field_tbl[] = {
	mca_sysfs_attr_wo(mca_event_sysfs, 0220, MCA_EVENT_ATTR_TRIGGER,
			  trigger),
};

static struct attribute *mca_event_sysfs_attrs[ARRAY_SIZE(mca_event_sysfs_field_tbl) + 1];

static const struct attribute_group mca_event_sysfs_attr_group = {
	.attrs = mca_event_sysfs_attrs,
};

/*
 * What is written to trigger is sent verbatim as a uevent.  The charge
 * logger writes MCA_LOG_FULL_EVENT here to have the charging log collected
 * on demand.
 */
static ssize_t mca_event_sysfs_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct mca_event_notify_data n_data;
	struct mca_sysfs_attr_info *field;

	field = mca_sysfs_lookup_attr(attr->attr.name,
				      mca_event_sysfs_field_tbl,
				      ARRAY_SIZE(mca_event_sysfs_field_tbl));
	if (!field)
		return -1;

	if (field->sysfs_attr_name != MCA_EVENT_ATTR_TRIGGER)
		return count;

	if (count >= MCA_EVENT_UEVENT_MAX)
		return -1;

	n_data.event = buf;
	n_data.event_len = count;
	mca_event_report_uevent(&n_data);

	return count;
}

static int __init mca_event_init(void)
{
	int i;

	for (i = 0; i < MCA_EVENT_TYPE_END; i++)
		BLOCKING_INIT_NOTIFIER_HEAD(&g_mca_event_bnh[i]);

	mca_sysfs_init_attrs(mca_event_sysfs_attrs, mca_event_sysfs_field_tbl,
			     ARRAY_SIZE(mca_event_sysfs_field_tbl));

	/*
	 * Without the device there are no uevents, but the notifier chains
	 * still work, so the module loads either way.
	 */
	g_mca_event_dev = mca_sysfs_create_group(MCA_EVENT_CLASS_NAME,
						 MCA_EVENT_DEV_NAME,
						 &mca_event_sysfs_attr_group);

	return 0;
}

static void __exit mca_event_exit(void)
{
	if (!g_mca_event_dev)
		return;

	mca_sysfs_remove_group(MCA_EVENT_CLASS_NAME, g_mca_event_dev,
			       &mca_event_sysfs_attr_group);
	g_mca_event_dev = NULL;
}

module_init(mca_event_init);
module_exit(mca_event_exit);

MODULE_DESCRIPTION("mca event driver");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
