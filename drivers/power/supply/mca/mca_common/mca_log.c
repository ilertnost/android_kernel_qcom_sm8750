// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The charging log.
 *
 * The charging drivers log here as well as to dmesg: every line goes into a
 * ring of buffers this module owns, which the charge logger in userspace
 * collects through xm_power/charge_log.  Each line carries a wall-clock
 * timestamp, because it is read against a bug report and a battery history
 * rather than against other kernel messages.  When enough buffers have
 * filled up, userspace is told with a MCA_LOG_FULL_EVENT uevent.
 *
 * The same attributes also give userspace the charge log table: one row of
 * values assembled from every driver that registered a source.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/time64.h>

#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_sysfs.h>

/* How many filled buffers can be held for userspace. */
#define MCA_LOG_MAX_CACHE_NUM		20

/* How much one buffer holds. */
#define MCA_LOG_BUFF_SIZE		4096

/* The most one line may come to, NUL included. */
#define MCA_LOG_LINE_SIZE		256

/* How many buffers fill up before userspace is told to collect them. */
#define MCA_LOG_FULL_NOTIFY_NUM		4

/* The ring depth once userspace has taken over log collection. */
#define MCA_LOG_USER_CACHE_NUM		4

/* Where the attributes appear. */
#define MCA_LOG_CLASS_NAME		"xm_power"
#define MCA_LOG_DEV_NAME		"charge_log"

/* What userspace can read and write here. */
enum mca_log_attr_list {
	MCA_LOG_ATTR_LOG_LEVEL,
	MCA_LOG_ATTR_LOG_ENABLE,
	MCA_LOG_ATTR_LOG_INDEX,
	MCA_LOG_ATTR_CUR_BUFF,
	MCA_LOG_ATTR_MAX_BUFFER_NUM,
	MCA_LOG_ATTR_DUMP_LOG_BUFF,
	MCA_LOG_ATTR_CHARGE_LOG_HEAD,
	MCA_LOG_ATTR_CHARGE_LOG_INFO,
	MCA_LOG_ATTR_CONSOLE_LOG_LEVEL,
	MCA_LOG_ATTR_WRITE_LOG,
	MCA_LOG_ATTR_TIME_OFFSET,
};

/**
 * struct mca_log_buf_info - the ring and what governs it
 * @dev:           the device the attributes hang off
 * @init_flag:     the ring is usable; until then errors only reach dmesg
 * @log_level:     the most verbose level kept in the ring
 * @console_level: info lines reach dmesg when above 0, debug lines when 2
 * @log_enable:    userspace collects the log, so tell it when buffers fill
 * @log_index:     which buffer dump_log_buff hands out; one past the last
 *                 ring slot names the buffer being filled
 * @used:          how much of @log_buff is filled
 * @cache_index:   the ring slot the next filled buffer goes to
 * @cache_num:     how many ring slots hold a buffer not yet collected
 * @max_cache_num: how many ring slots are in use
 * @time_offset:   seconds added to the timestamp of every line
 * @log_buff_cache: the ring of filled buffers
 * @log_buff:      the buffer being filled
 */
struct mca_log_buf_info {
	struct device	*dev;
	int		init_flag;
	int		log_level;
	int		console_level;
	int		log_enable;
	int		log_index;
	int		used;
	int		cache_index;
	int		cache_num;
	int		max_cache_num;
	int		time_offset;
	char		*log_buff_cache[MCA_LOG_MAX_CACHE_NUM];
	char		*log_buff;
};

static struct mca_log_buf_info g_mca_log_info;

/* Lines are logged from interrupt handlers too, so this cannot sleep. */
static DEFINE_SPINLOCK(g_mca_log_lock);

/* The sources the charge log table is assembled from. */
static struct mca_charge_log_ops_data {
	struct mca_log_charge_log_ops	*ops;
	void				*data;
} g_mca_charge_log_data[MCA_CHARGE_LOG_ID_MAX];

/* Whether this is a charging-only boot; set on the kernel command line. */
static unsigned int charge_boot_mode;
module_param(charge_boot_mode, uint, 0444);
MODULE_PARM_DESC(charge_boot_mode, "charge_boot_mode");

int mca_log_get_charge_boot_mode(void)
{
	return charge_boot_mode;
}
EXPORT_SYMBOL(mca_log_get_charge_boot_mode);

/**
 * mca_log_proc_log_info() - put one finished line into the ring
 * @buf: the line, in a buffer of MCA_LOG_LINE_SIZE bytes
 * @len: how long it is
 *
 * A buffer that cannot take the line is moved into the ring and a new one is
 * started.  Once enough buffers have filled, userspace is told to collect
 * them, but only from a context the uevent can be sent from; otherwise a
 * later line sends it.
 */
static void mca_log_proc_log_info(char *buf, int len)
{
	struct mca_log_buf_info *info = &g_mca_log_info;
	static int full_num;
	struct mca_event_notify_data n_data = { 0 };
	unsigned long flags;
	int num;

	if (len <= MCA_LOG_LINE_SIZE - 2 && buf[len - 1] != '\n')
		buf[len++] = '\n';

	spin_lock_irqsave(&g_mca_log_lock, flags);

	if (!info->log_buff) {
		info->log_buff = kzalloc(MCA_LOG_BUFF_SIZE, GFP_ATOMIC);
		if (!info->log_buff)
			goto unlock;
	}

	if (info->used + len >= MCA_LOG_BUFF_SIZE - 1) {
		full_num++;

		info->cache_num++;
		if (info->cache_num >= info->max_cache_num)
			info->cache_num = info->max_cache_num;

		/* The oldest uncollected buffer in this slot is dropped. */
		kfree(info->log_buff_cache[info->cache_index]);
		info->log_buff_cache[info->cache_index] = info->log_buff;
		info->log_buff = NULL;
		info->cache_index++;

		/*
		 * max_cache_num can shrink under a running ring when userspace
		 * enables collection, so wrap on anything past the end.
		 */
		if (info->cache_index >= info->max_cache_num) {
			info->cache_index = 0;
			kfree(info->log_buff_cache[0]);
			info->log_buff_cache[0] = NULL;
		}

		info->used = 0;
		info->log_buff = kzalloc(MCA_LOG_BUFF_SIZE, GFP_ATOMIC);
		if (!info->log_buff)
			goto unlock;
	}

	memcpy(info->log_buff + info->used, buf, len);
	info->used += len;
	num = full_num;

	spin_unlock_irqrestore(&g_mca_log_lock, flags);

	if (num < MCA_LOG_FULL_NOTIFY_NUM || in_interrupt())
		return;

	full_num = 0;
	if (info->log_enable) {
		n_data.event = MCA_LOG_FULL_EVENT;
		n_data.event_len = strlen(MCA_LOG_FULL_EVENT);
		mca_event_report_uevent(&n_data);
	}

	return;

unlock:
	spin_unlock_irqrestore(&g_mca_log_lock, flags);
}

/**
 * mca_log_format() - timestamp a message
 * @buf:   where to put it, MCA_LOG_LINE_SIZE bytes
 * @level: the level's letter, as it appears in the line
 * @fmt:   the message
 * @args:  its arguments
 *
 * The line starts with the local wall-clock time, shifted by time_offset,
 * and the pid of the task that logged it.
 *
 * Return: the length of the line, or -1 if the timestamp did not fit.
 */
static int mca_log_format(char *buf, char level, const char *fmt,
			  va_list args)
{
	struct timespec64 ts = { 0 };
	struct rtc_time tm = { 0 };
	int n, len;

	ktime_get_real_ts64(&ts);
	ts.tv_sec = ts.tv_sec - sys_tz.tz_minuteswest * 60 +
		    g_mca_log_info.time_offset;
	rtc_time64_to_tm(ts.tv_sec, &tm);

	n = snprintf(buf, MCA_LOG_LINE_SIZE - 1,
		     "[%02d:%02d:%02d:%03ld-%c][%5d]", tm.tm_hour, tm.tm_min,
		     tm.tm_sec, ts.tv_nsec / NSEC_PER_MSEC, level,
		     current->pid);
	if (n < 0 || n >= MCA_LOG_LINE_SIZE - 1)
		return -1;

	len = vsnprintf(buf + n, MCA_LOG_LINE_SIZE - 1 - n, fmt, args);

	/*
	 * vsnprintf() returns what the line would have come to; only what
	 * fits is kept.
	 */
	return min(n + len, MCA_LOG_LINE_SIZE - 2);
}

void __mca_log_err(const char *fmt, ...)
{
	char buf[MCA_LOG_LINE_SIZE] = { 0 };
	va_list args;
	int len;

	va_start(args, fmt);

	if (!g_mca_log_info.init_flag) {
		vsnprintf(buf, MCA_LOG_LINE_SIZE - 1, fmt, args);
		pr_err("%s\n", buf);
		goto out;
	}

	len = mca_log_format(buf, 'E', fmt, args);
	if (len < 0)
		goto out;

	pr_err("%s", buf);
	mca_log_proc_log_info(buf, len);

out:
	va_end(args);
}
EXPORT_SYMBOL(__mca_log_err);

void __mca_log_info(const char *fmt, ...)
{
	char buf[MCA_LOG_LINE_SIZE] = { 0 };
	va_list args;
	int len;

	if (!g_mca_log_info.init_flag)
		return;

	va_start(args, fmt);
	len = mca_log_format(buf, 'I', fmt, args);
	va_end(args);
	if (len < 0)
		return;

	if (g_mca_log_info.console_level > 0 || charge_boot_mode)
		pr_info("%s", buf);

	if (g_mca_log_info.log_level >= MCA_LOG_LEVEL_INFO)
		mca_log_proc_log_info(buf, len);
}
EXPORT_SYMBOL(__mca_log_info);

void __mca_log_debug(const char *fmt, ...)
{
	char buf[MCA_LOG_LINE_SIZE] = { 0 };
	va_list args;
	int len;

	if (!g_mca_log_info.init_flag ||
	    g_mca_log_info.log_level < MCA_LOG_LEVEL_DEBUG)
		return;

	va_start(args, fmt);
	len = mca_log_format(buf, 'D', fmt, args);
	va_end(args);
	if (len < 0)
		return;

	if (g_mca_log_info.console_level == MCA_LOG_LEVEL_DEBUG)
		pr_info("%s", buf);

	if (g_mca_log_info.log_level >= MCA_LOG_LEVEL_DEBUG)
		mca_log_proc_log_info(buf, len);
}
EXPORT_SYMBOL(__mca_log_debug);

void mca_log_charge_log_register(enum mca_charge_log_id_ele id,
				 struct mca_log_charge_log_ops *ops,
				 void *data)
{
	if (id >= MCA_CHARGE_LOG_ID_MAX)
		return;

	g_mca_charge_log_data[id].ops = ops;
	g_mca_charge_log_data[id].data = data;
}
EXPORT_SYMBOL(mca_log_charge_log_register);

static ssize_t mca_log_sysfs_show(struct device *dev,
				  struct device_attribute *attr, char *buf);
static ssize_t mca_log_sysfs_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count);

static struct mca_sysfs_attr_info mca_log_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(mca_log_sysfs, 0660, MCA_LOG_ATTR_LOG_LEVEL,
			  log_level),
	mca_sysfs_attr_rw(mca_log_sysfs, 0660, MCA_LOG_ATTR_LOG_ENABLE,
			  log_enable),
	mca_sysfs_attr_wo(mca_log_sysfs, 0220, MCA_LOG_ATTR_LOG_INDEX,
			  log_index),
	mca_sysfs_attr_ro(mca_log_sysfs, 0440, MCA_LOG_ATTR_CUR_BUFF,
			  cur_buff),
	mca_sysfs_attr_ro(mca_log_sysfs, 0440, MCA_LOG_ATTR_MAX_BUFFER_NUM,
			  max_buffer_num),
	mca_sysfs_attr_ro(mca_log_sysfs, 0440, MCA_LOG_ATTR_DUMP_LOG_BUFF,
			  dump_log_buff),
	mca_sysfs_attr_ro(mca_log_sysfs, 0440, MCA_LOG_ATTR_CHARGE_LOG_HEAD,
			  charge_log_head),
	mca_sysfs_attr_ro(mca_log_sysfs, 0440, MCA_LOG_ATTR_CHARGE_LOG_INFO,
			  charge_log_info),
	mca_sysfs_attr_rw(mca_log_sysfs, 0660, MCA_LOG_ATTR_CONSOLE_LOG_LEVEL,
			  console_log_level),
	mca_sysfs_attr_wo(mca_log_sysfs, 0220, MCA_LOG_ATTR_WRITE_LOG,
			  write_log),
	mca_sysfs_attr_rw(mca_log_sysfs, 0660, MCA_LOG_ATTR_TIME_OFFSET,
			  time_offset),
};

static struct attribute *mca_log_sysfs_attrs[ARRAY_SIZE(mca_log_sysfs_field_tbl) + 1];

static const struct attribute_group mca_log_sysfs_attr_group = {
	.attrs = mca_log_sysfs_attrs,
};

/**
 * mca_log_dump_charge_log() - assemble the charge log table
 * @buf:  the sysfs buffer
 * @head: whether to write the headings or the values
 *
 * Every registered source writes its part, in the order the identifiers are
 * numbered, so the headings and the values line up whatever set of sources
 * happens to be present.
 *
 * Return: how much was written.
 */
static int mca_log_dump_charge_log(char *buf, bool head)
{
	struct mca_charge_log_ops_data *s;
	int len = 0;
	int i;

	for (i = 0; i < MCA_CHARGE_LOG_ID_MAX; i++) {
		s = &g_mca_charge_log_data[i];
		if (!s->ops || !s->ops->dump_log_head)
			continue;

		if (head)
			len += s->ops->dump_log_head(s->data, buf + len,
						     PAGE_SIZE - len);
		else if (s->ops->dump_log_context)
			len += s->ops->dump_log_context(s->data, buf + len,
							PAGE_SIZE - len);
	}

	buf[min(len, (int)PAGE_SIZE - 1)] = '\n';

	return len;
}

/*
 * Reading a buffer hands it over: a ring slot is freed once read, and the
 * buffer being filled is emptied, so a collector never reads the same lines
 * twice.
 */
static ssize_t mca_log_dump_log_buff(char *buf)
{
	struct mca_log_buf_info *info = &g_mca_log_info;
	unsigned long flags;
	char *cache;
	int idx, len;

	spin_lock_irqsave(&g_mca_log_lock, flags);

	if (!info->log_buff)
		goto empty;

	idx = info->log_index;
	if (idx == MCA_LOG_MAX_CACHE_NUM) {
		memcpy(buf, info->log_buff, MCA_LOG_BUFF_SIZE - 1);
		len = strlen(buf);
		memset(info->log_buff, 0, MCA_LOG_BUFF_SIZE);
		info->used = 0;
		spin_unlock_irqrestore(&g_mca_log_lock, flags);
		return len;
	}

	if (idx < 0 || idx > MCA_LOG_MAX_CACHE_NUM)
		goto empty;

	cache = info->log_buff_cache[idx];
	if (!cache)
		goto empty;

	info->cache_num = max(info->cache_num - 1, 0);
	memcpy(buf, cache, MCA_LOG_BUFF_SIZE - 1);
	len = strlen(buf);
	kfree(cache);
	info->log_buff_cache[idx] = NULL;

	spin_unlock_irqrestore(&g_mca_log_lock, flags);

	return len;

empty:
	spin_unlock_irqrestore(&g_mca_log_lock, flags);

	return 0;
}

static ssize_t mca_log_sysfs_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct mca_sysfs_attr_info *field;
	int val;

	field = mca_sysfs_lookup_attr(attr->attr.name, mca_log_sysfs_field_tbl,
				      ARRAY_SIZE(mca_log_sysfs_field_tbl));
	if (!field)
		return -EINVAL;

	switch (field->sysfs_attr_name) {
	case MCA_LOG_ATTR_LOG_LEVEL:
		val = g_mca_log_info.log_level;
		break;
	case MCA_LOG_ATTR_LOG_ENABLE:
		val = g_mca_log_info.log_enable;
		break;
	case MCA_LOG_ATTR_CUR_BUFF:
		val = g_mca_log_info.cache_index;
		break;
	case MCA_LOG_ATTR_MAX_BUFFER_NUM:
		val = g_mca_log_info.max_cache_num;
		break;
	case MCA_LOG_ATTR_CONSOLE_LOG_LEVEL:
		val = g_mca_log_info.console_level;
		break;
	case MCA_LOG_ATTR_TIME_OFFSET:
		val = g_mca_log_info.time_offset;
		break;
	case MCA_LOG_ATTR_DUMP_LOG_BUFF:
		return mca_log_dump_log_buff(buf);
	case MCA_LOG_ATTR_CHARGE_LOG_HEAD:
		return mca_log_dump_charge_log(buf, true);
	case MCA_LOG_ATTR_CHARGE_LOG_INFO:
		return mca_log_dump_charge_log(buf, false);
	default:
		return 0;
	}

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t mca_log_sysfs_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct mca_sysfs_attr_info *field;
	int val = 0;

	field = mca_sysfs_lookup_attr(attr->attr.name, mca_log_sysfs_field_tbl,
				      ARRAY_SIZE(mca_log_sysfs_field_tbl));
	if (!field)
		return -EINVAL;

	/*
	 * Text attributes are not numbers; val then stays 0 and is not used,
	 * so a parse failure is not an error here.
	 */
	if (kstrtoint(buf, 0, &val))
		val = 0;

	switch (field->sysfs_attr_name) {
	case MCA_LOG_ATTR_LOG_LEVEL:
		g_mca_log_info.log_level = val;
		break;
	case MCA_LOG_ATTR_LOG_ENABLE:
		g_mca_log_info.log_enable = val;
		g_mca_log_info.max_cache_num = MCA_LOG_USER_CACHE_NUM;
		break;
	case MCA_LOG_ATTR_LOG_INDEX:
		if ((unsigned int)val > MCA_LOG_MAX_CACHE_NUM)
			return -1;
		g_mca_log_info.log_index = val;
		break;
	case MCA_LOG_ATTR_CONSOLE_LOG_LEVEL:
		g_mca_log_info.console_level = val;
		pr_err("console level is %d\n", val);
		break;
	case MCA_LOG_ATTR_WRITE_LOG:
		__mca_log_info("[write_log] %s", buf);
		break;
	case MCA_LOG_ATTR_TIME_OFFSET:
		/* Written in milliseconds, kept in seconds. */
		__mca_log_err("update time offset: %s", buf);
		g_mca_log_info.time_offset = val / 1000;
		break;
	default:
		break;
	}

	return count;
}

static int __init mca_log_init(void)
{
	g_mca_log_info.log_buff = kzalloc(MCA_LOG_BUFF_SIZE, GFP_KERNEL);
	if (!g_mca_log_info.log_buff)
		return -ENOMEM;

	mca_sysfs_init_attrs(mca_log_sysfs_attrs, mca_log_sysfs_field_tbl,
			     ARRAY_SIZE(mca_log_sysfs_field_tbl));
	g_mca_log_info.dev = mca_sysfs_create_group(MCA_LOG_CLASS_NAME,
						    MCA_LOG_DEV_NAME,
						    &mca_log_sysfs_attr_group);

	g_mca_log_info.max_cache_num = MCA_LOG_MAX_CACHE_NUM;
	g_mca_log_info.log_level = MCA_LOG_LEVEL_INFO;
	g_mca_log_info.console_level = MCA_LOG_LEVEL_ERROR;
	g_mca_log_info.log_enable = 1;
	g_mca_log_info.init_flag = 1;

	pr_err("charge_boot_mode %d\n", charge_boot_mode);

	return 0;
}

static void __exit mca_log_exit(void)
{
	mca_sysfs_remove_group(MCA_LOG_CLASS_NAME, g_mca_log_info.dev,
			       &mca_log_sysfs_attr_group);
}

module_init(mca_log_init);
module_exit(mca_log_exit);

MODULE_DESCRIPTION("strategy buckchg class");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
