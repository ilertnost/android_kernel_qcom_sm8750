// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The charger partition: what the charging stack remembers across boots.
 * See include/mca/shared_memory/charger_partition_class.h.
 */

#define MCA_LOG_TAG "charger_partition"

#include <linux/blkdev.h>
#include <linux/kstrtox.h>
#include <linux/log2.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <mca/shared_memory/charger_partition_class.h>
#include <mca/common/mca_event.h>
#include <mca/strategy/strategy_class.h>
#include <linux/hwid.h>
#include <mca/common/mca_hwid.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_sysfs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include <asm/unaligned.h>

#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

/* The partition is read and written a block at a time. */
#define CHARGER_PARTITION_BLOCK_SIZE	4096

/* How long to give one command, and how often to retry it. */
#define CHARGER_PARTITION_TIMEOUT	(15 * HZ)
#define CHARGER_PARTITION_RETRIES	3

/* The partition lives on the boot LUN, which is one of the first few. */
#define CHARGER_PARTITION_LUN_MAX	6

/* Its volume name, and the SCSI host it must be on. */
#define PARTITION_NAME			"charger"
#define UFSHCD				"ufshcd"

/*
 * Storage comes up well after this driver, so the worker keeps looking for
 * it: every five seconds, twenty times.
 */
#define CHARGER_PARTITION_POLL_MS	5000
#define CHARGER_PARTITION_POLL_MAX	20

/*
 * Which partition holds it depends on the board: the two platforms this
 * driver serves lay their storage out differently.
 */
#define CHARGER_PARTITION_PART_NUMBER	22
#define CHARGER_PARTITION_PART_NUMBER_ALT 25
#define CHARGER_PARTITION_PLATFORM_ALT	9

struct charger_partition_chip {
	struct device			*dev;
	struct scsi_device		*sdev;
	struct delayed_work		charger_partition_work;
	int				part_info_part_number;
	const char			*part_info_part_name;
	struct charger_partition_info_1	info_1;
	struct charger_partition_info_2	info_2;
	bool				is_charger_partition_rdy;
	bool				not_notify_module;
};

/* What the sysfs attributes below are told apart by. */
enum charger_partition_attr_list {
	MCA_PROP_CHARGER_PARTITION_MISHOW,
	MCA_PROP_CHARGER_PARTITION_POWEROFFMODE,
	MCA_PROP_CHARGER_PARTITION_PROP_EU_MODE,
};

static struct charger_partition_chip *g_chip;

/* The scratch block every read and write goes through. */
static void *g_rw_buf;

/* Where the partition starts, in blocks. */
static u32 g_partition_start;

static int charger_partition_retry;


/*
 * Read or write one block with a plain SCSI command rather than through the
 * block layer: the partition holds no filesystem, several processors reach it
 * by block number, and this driver has to do the same.
 */
static int charger_scsi_rw_partition(struct scsi_device *sdev, void *buf,
				     u32 lba, bool write)
{
	struct scsi_sense_hdr sshdr = {};
	struct scsi_exec_args args = { .sshdr = &sshdr };
	unsigned char cdb[10] = {};
	unsigned long flags;
	int ret;

	spin_lock_irqsave(sdev->host->host_lock, flags);
	ret = scsi_device_get(sdev);
	if (!ret && !scsi_device_online(sdev)) {
		scsi_device_put(sdev);
		mca_log_err("get device fail\n");
		ret = -ENODEV;
	}
	spin_unlock_irqrestore(sdev->host->host_lock, flags);
	if (ret) {
		if (write)
			mca_log_err("get scsi fail\n");
		else
			mca_log_err("failed to get scsi device\n");
		return ret;
	}

	cdb[0] = write ? WRITE_10 : READ_10;
	put_unaligned_be32(lba, &cdb[2]);
	put_unaligned_be16(1, &cdb[7]);

	/*
	 * Error recovery must not try to resume the host from inside this
	 * command.
	 */
	sdev->host->eh_noresume = 1;
	ret = scsi_execute_cmd(sdev, cdb,
			       write ? REQ_OP_DRV_OUT : REQ_OP_DRV_IN, buf,
			       CHARGER_PARTITION_BLOCK_SIZE,
			       CHARGER_PARTITION_TIMEOUT,
			       CHARGER_PARTITION_RETRIES, &args);
	if (ret) {
		if (write)
			mca_log_err("write error %d\n", ret);
		else
			mca_log_err("read error %d\n", ret);
	}

	scsi_device_put(sdev);
	sdev->host->eh_noresume = 0;

	return ret;
}

static int charger_scsi_read_partition(struct scsi_device *sdev, void *buf,
				       u32 lba)
{
	return charger_scsi_rw_partition(sdev, buf, lba, false);
}

static int charger_scsi_write_partition(struct scsi_device *sdev, void *buf,
					u32 lba)
{
	return charger_scsi_rw_partition(sdev, buf, lba, true);
}

/* The argument checks every partition access starts with. */
static bool charger_partition_args_ok(enum charger_partition_host_type host,
				      enum charger_partition_info_type info,
				      u32 size)
{
	if (host >= CHARGER_PARTITION_HOST_LAST) {
		mca_log_err("charger_partition_host_type not support!");
		return false;
	}

	if (info >= CHARGER_PARTITION_INFO_LAST) {
		mca_log_err("charger_partition_info_type not support!");
		return false;
	}

	if (size >= CHARGER_PARTITION_BLOCK_SIZE) {
		mca_log_err("read %u size not support!", size);
		return false;
	}

	return true;
}

/*
 * Set the header's availability flag to @avaliable, read-modify-write of the
 * header block through the shared buffer.
 */
static int charger_partition_set_avaliable(u32 avaliable)
{
	struct charger_partition_header *header = g_rw_buf;
	int ret;

	memset(g_rw_buf, 0, CHARGER_PARTITION_BLOCK_SIZE);
	ret = charger_scsi_read_partition(g_chip->sdev, g_rw_buf,
					  g_partition_start);
	if (ret) {
		mca_log_err("charger read error %d\n", ret);
		return -1;
	}

	if (!avaliable && !header->avaliable) {
		mca_log_err("not avaliable, can't do rw now!\n");
		return -1;
	}

	header->avaliable = avaliable;
	ret = charger_scsi_write_partition(g_chip->sdev, g_rw_buf,
					   g_partition_start);
	if (ret) {
		mca_log_err("charger write error %d\n", ret);
		return -1;
	}

	return 0;
}

/*
 * Take the partition before reading or writing an information block, so that
 * the bootloader and the ADSP do not write it underneath us, and give it back
 * with charger_partition_dealloc() as soon as the access is done.  Taking it
 * fails while someone else holds it.
 */
int charger_partition_alloc(enum charger_partition_host_type host,
			    enum charger_partition_info_type info, u32 size)
{
	if (!g_chip || !g_chip->is_charger_partition_rdy) {
		mca_log_err("charger partition not rdy, can't do rw!");
		return -1;
	}

	if (!charger_partition_args_ok(host, info, size))
		return -1;

	if (!g_rw_buf) {
		g_rw_buf = kmalloc(CHARGER_PARTITION_BLOCK_SIZE, GFP_KERNEL);
		if (!g_rw_buf) {
			mca_log_err("malloc buf error!\n");
			return -1;
		}
	}

	return charger_partition_set_avaliable(0);
}
EXPORT_SYMBOL(charger_partition_alloc);

int charger_partition_dealloc(enum charger_partition_host_type host,
			      enum charger_partition_info_type info, u32 size)
{
	if (!g_chip || !g_chip->is_charger_partition_rdy) {
		mca_log_err("charger partition not rdy, can't do rw!");
		return -1;
	}

	if (!charger_partition_args_ok(host, info, size))
		return -1;

	if (!g_rw_buf) {
		mca_log_err("rw_buf null, please alloc first!");
		return -1;
	}

	return charger_partition_set_avaliable(1);
}
EXPORT_SYMBOL(charger_partition_dealloc);

/**
 * charger_partition_read() - read one information block
 * @host: who is asking
 * @info: which block
 * @size: how much of it the caller will look at
 *
 * Return: the block, valid until the next access, or NULL.  The buffer is
 * shared, so the caller must copy anything it means to keep.
 */
void *charger_partition_read(enum charger_partition_host_type host,
			     enum charger_partition_info_type info, u32 size)
{
	int ret;

	if (!g_chip || !g_chip->is_charger_partition_rdy) {
		mca_log_err("charger partition not rdy, can't do read!");
		return NULL;
	}

	if (!g_rw_buf) {
		mca_log_err("rw_buf null, please alloc first!");
		return NULL;
	}

	if (!charger_partition_args_ok(host, info, size))
		return NULL;

	memset(g_rw_buf, 0, CHARGER_PARTITION_BLOCK_SIZE);
	ret = charger_scsi_read_partition(g_chip->sdev, g_rw_buf,
					  g_partition_start + info);
	if (ret) {
		mca_log_err("charger read error %d\n", ret);
		return NULL;
	}

	return g_rw_buf;
}
EXPORT_SYMBOL(charger_partition_read);

/**
 * charger_partition_write() - write the start of one information block
 * @host: who is asking
 * @info: which block
 * @buf:  what to write
 * @size: how much of @buf to write
 *
 * The block is read back first so that the bytes past @size keep whatever
 * they held.
 */
int charger_partition_write(enum charger_partition_host_type host,
			    enum charger_partition_info_type info, void *buf,
			    u32 size)
{
	int ret;

	if (!g_chip || !g_chip->is_charger_partition_rdy) {
		mca_log_err("charger partition not rdy, can't do read!");
		return -1;
	}

	if (!g_rw_buf) {
		mca_log_err("rw_buf null, please alloc first!");
		return -1;
	}

	if (!charger_partition_args_ok(host, info, size))
		return -1;

	memset(g_rw_buf, 0, CHARGER_PARTITION_BLOCK_SIZE);
	ret = charger_scsi_read_partition(g_chip->sdev, g_rw_buf,
					  g_partition_start + info);
	if (ret) {
		mca_log_err("charger read error %d\n", ret);
		return -1;
	}

	memcpy(g_rw_buf, buf, size);

	ret = charger_scsi_write_partition(g_chip->sdev, g_rw_buf,
					   g_partition_start + info);
	if (ret) {
		mca_log_err("charger write error %d\n", ret);
		return -1;
	}

	return 0;
}
EXPORT_SYMBOL(charger_partition_write);

/*
 * The two information blocks this driver keeps a copy of.  Each refresh takes
 * the partition, copies the block, and gives the partition back; on failure
 * the copy keeps what it held.  Each write-back does the same the other way.
 */
static void charger_partition_get_info_1(void)
{
	struct charger_partition_info_1 *info = &g_chip->info_1;
	void *src;
	int ret;

	ret = charger_partition_alloc(CHARGER_PARTITION_HOST_KERNEL,
				      CHARGER_PARTITION_INFO_1, sizeof(*info));
	if (ret < 0) {
		mca_log_err("failed to alloc\n");
		return;
	}

	src = charger_partition_read(CHARGER_PARTITION_HOST_KERNEL,
				     CHARGER_PARTITION_INFO_1, sizeof(*info));
	if (!src) {
		mca_log_err("failed to read\n");
	} else {
		memcpy(info, src, sizeof(*info));
		mca_log_err("ret: %d, mishow: 0x%0x, zero_speed_mode: %u, power_off_mode: %u, double85: %u, remove_temp_limit: %u, memory_test: %u, soc_limit: %d\n",
			    ret, info->mishow, info->zero_speed_mode,
			    info->power_off_mode, info->double85,
			    info->remove_temp_limit, info->memory_test,
			    info->soc_limit);
	}

	if (charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL,
				      CHARGER_PARTITION_INFO_1,
				      sizeof(*info)) < 0)
		mca_log_err("failed to dealloc\n");
}

static void charger_partition_set_info_1(void)
{
	struct charger_partition_info_1 *info = &g_chip->info_1;

	if (charger_partition_alloc(CHARGER_PARTITION_HOST_KERNEL,
				    CHARGER_PARTITION_INFO_1,
				    sizeof(*info)) < 0) {
		mca_log_err("failed to alloc\n");
		return;
	}

	if (charger_partition_write(CHARGER_PARTITION_HOST_KERNEL,
				    CHARGER_PARTITION_INFO_1, info,
				    sizeof(*info)) < 0)
		mca_log_err("failed to write\n");

	if (charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL,
				      CHARGER_PARTITION_INFO_1,
				      sizeof(*info)) < 0)
		mca_log_err("failed to dealloc\n");
}

static void charger_partition_get_info_2(void)
{
	struct charger_partition_info_2 *info = &g_chip->info_2;
	void *src;
	int ret;

	ret = charger_partition_alloc(CHARGER_PARTITION_HOST_KERNEL,
				      CHARGER_PARTITION_INFO_2, sizeof(*info));
	if (ret < 0) {
		mca_log_err("failed to alloc\n");
		return;
	}

	src = charger_partition_read(CHARGER_PARTITION_HOST_KERNEL,
				     CHARGER_PARTITION_INFO_2, sizeof(*info));
	if (!src) {
		mca_log_err("failed to read\n");
	} else {
		memcpy(info, src, sizeof(*info));
		mca_log_err("ret: %d, info_2->eu_mode: %u\n", ret,
			    info->eu_mode);
	}

	if (charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL,
				      CHARGER_PARTITION_INFO_2,
				      sizeof(*info)) < 0)
		mca_log_err("[ChgPartition] failed to dealloc\n");
}

static void charger_partition_set_info_2(void)
{
	struct charger_partition_info_2 *info = &g_chip->info_2;

	if (charger_partition_alloc(CHARGER_PARTITION_HOST_KERNEL,
				    CHARGER_PARTITION_INFO_2,
				    sizeof(*info)) < 0) {
		mca_log_err("failed to alloc\n");
		return;
	}

	if (charger_partition_write(CHARGER_PARTITION_HOST_KERNEL,
				    CHARGER_PARTITION_INFO_2, info,
				    sizeof(*info)) < 0)
		mca_log_err("failed to write\n");

	if (charger_partition_dealloc(CHARGER_PARTITION_HOST_KERNEL,
				      CHARGER_PARTITION_INFO_2,
				      sizeof(*info)) < 0)
		mca_log_err("failed to dealloc\n");
}

/*
 * These two answer from the copy in memory: a caller asking whether this is a
 * European model is deciding what to ask an adapter for, and must not wait
 * on storage to find out.
 */
int charger_partition_get_eu_model(bool *is_eu_model)
{
	if (!g_chip) {
		mca_log_err("charger_partition init error\n");
		return -1;
	}

	*is_eu_model = !!g_chip->info_2.eu_mode;

	return 0;
}
EXPORT_SYMBOL(charger_partition_get_eu_model);

int charger_partition_get_mishow(bool *mishow)
{
	if (!g_chip) {
		mca_log_err("charger_partition init error\n");
		return -1;
	}

	*mishow = !!g_chip->info_1.mishow;

	return 0;
}
EXPORT_SYMBOL(charger_partition_get_mishow);

/*
 * Everything below refreshes the block before answering, and writes it back
 * after changing it.  When the partition cannot be reached the answer is the
 * last value seen; these always report success.
 */
#define CHARGER_PARTITION_INFO_1_ACCESSOR(name)				\
int charger_partition_read_##name(int *name)				\
{									\
	charger_partition_get_info_1();					\
	*name = g_chip->info_1.name;					\
									\
	return 0;							\
}									\
									\
int charger_partition_write_##name(int name)				\
{									\
	charger_partition_get_info_1();					\
	g_chip->info_1.name = name;					\
	charger_partition_set_info_1();					\
									\
	return 0;							\
}

CHARGER_PARTITION_INFO_1_ACCESSOR(double85);
EXPORT_SYMBOL(charger_partition_read_double85);
EXPORT_SYMBOL(charger_partition_write_double85);

CHARGER_PARTITION_INFO_1_ACCESSOR(remove_temp_limit);
EXPORT_SYMBOL(charger_partition_write_remove_temp_limit);

CHARGER_PARTITION_INFO_1_ACCESSOR(memory_test);
EXPORT_SYMBOL(charger_partition_read_memory_test);
EXPORT_SYMBOL(charger_partition_write_memory_test);

CHARGER_PARTITION_INFO_1_ACCESSOR(soc_limit);
EXPORT_SYMBOL(charger_partition_read_soc_limit);
EXPORT_SYMBOL(charger_partition_write_soc_limit);

int charger_partition_read_ocd_count(int *ocd_count, int *hocd_count)
{
	charger_partition_get_info_2();
	*ocd_count = g_chip->info_2.ocd_count;
	*hocd_count = g_chip->info_2.hocd_count;

	return 0;
}
EXPORT_SYMBOL(charger_partition_read_ocd_count);

int charger_partition_write_ocd_count(int ocd_count, int hocd_count)
{
	charger_partition_get_info_2();
	g_chip->info_2.ocd_count = ocd_count;
	g_chip->info_2.hocd_count = hocd_count;
	charger_partition_set_info_2();

	return 0;
}
EXPORT_SYMBOL(charger_partition_write_ocd_count);

int charger_partition_read_cuv_count(int *cuv_count, int *hcuv_count)
{
	charger_partition_get_info_2();
	*cuv_count = g_chip->info_2.cuv_count;
	*hcuv_count = g_chip->info_2.hcuv_count;

	return 0;
}
EXPORT_SYMBOL(charger_partition_read_cuv_count);

int charger_partition_write_cuv_count(int cuv_count, int hcuv_count)
{
	charger_partition_get_info_2();
	g_chip->info_2.cuv_count = cuv_count;
	g_chip->info_2.hcuv_count = hcuv_count;
	charger_partition_set_info_2();

	return 0;
}
EXPORT_SYMBOL(charger_partition_write_cuv_count);

int charger_partition_read_hscd_count(int *hscd_count)
{
	charger_partition_get_info_2();
	*hscd_count = g_chip->info_2.hscd_count;

	return 0;
}
EXPORT_SYMBOL(charger_partition_read_hscd_count);

int charger_partition_write_hcsd_count(int hscd_count)
{
	charger_partition_get_info_2();
	g_chip->info_2.hscd_count = hscd_count;
	charger_partition_set_info_2();

	return 0;
}
EXPORT_SYMBOL(charger_partition_write_hcsd_count);

static ssize_t charger_partition_sysfs_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf);
static ssize_t charger_partition_sysfs_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count);

static struct mca_sysfs_attr_info charger_partition_sysfs_field_tbl[] = {
	mca_sysfs_attr_rw(charger_partition_sysfs, 0664,
			  MCA_PROP_CHARGER_PARTITION_MISHOW,
			  charger_partition_mishow),
	mca_sysfs_attr_rw(charger_partition_sysfs, 0664,
			  MCA_PROP_CHARGER_PARTITION_POWEROFFMODE,
			  charger_partition_poweroffmode),
	mca_sysfs_attr_rw(charger_partition_sysfs, 0664,
			  MCA_PROP_CHARGER_PARTITION_PROP_EU_MODE,
			  charger_partition_prop_eu_mode),
};

static struct attribute *charger_partition_sysfs_attrs[
	ARRAY_SIZE(charger_partition_sysfs_field_tbl) + 1];

static const struct attribute_group charger_partition_sysfs_attr_group = {
	.attrs = charger_partition_sysfs_attrs,
};

/*
 * Read back from the partition before answering: the bootloader and recovery
 * write these too.
 */
static ssize_t charger_partition_sysfs_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct mca_sysfs_attr_info *info;
	int val;

	info = mca_sysfs_lookup_attr(attr->attr.name,
				     charger_partition_sysfs_field_tbl,
				     ARRAY_SIZE(charger_partition_sysfs_field_tbl));
	if (!info || !g_chip)
		return -1;

	switch (info->sysfs_attr_name) {
	case MCA_PROP_CHARGER_PARTITION_MISHOW:
		charger_partition_get_info_1();
		val = g_chip->info_1.mishow;
		break;
	case MCA_PROP_CHARGER_PARTITION_POWEROFFMODE:
		charger_partition_get_info_1();
		val = g_chip->info_1.power_off_mode;
		break;
	case MCA_PROP_CHARGER_PARTITION_PROP_EU_MODE:
		charger_partition_get_info_2();
		val = g_chip->info_2.eu_mode;
		break;
	default:
		return 0;
	}

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t charger_partition_sysfs_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct mca_sysfs_attr_info *info;
	int val = 0;

	info = mca_sysfs_lookup_attr(attr->attr.name,
				     charger_partition_sysfs_field_tbl,
				     ARRAY_SIZE(charger_partition_sysfs_field_tbl));
	if (!info || !g_chip)
		return -1;

	if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	switch (info->sysfs_attr_name) {
	case MCA_PROP_CHARGER_PARTITION_MISHOW:
		charger_partition_get_info_1();
		g_chip->info_1.mishow = val;
		charger_partition_set_info_1();
		break;
	case MCA_PROP_CHARGER_PARTITION_POWEROFFMODE:
		charger_partition_get_info_1();
		g_chip->info_1.power_off_mode = val;
		charger_partition_set_info_1();
		break;
	case MCA_PROP_CHARGER_PARTITION_PROP_EU_MODE:
		charger_partition_get_info_2();
		g_chip->info_2.eu_mode = val;
		charger_partition_set_info_2();
		break;
	default:
		break;
	}

	return count;
}

/* What prepare() resets the boot mode fields of the first block to. */
#define CHARGER_PARTITION_MODE_DEFAULT	2

/*
 * Once the partition is reachable: reset the power-off and zero-speed modes
 * for the bootloader, then tell the rest of the stack about the test flags
 * that are set and whether this is a European unit.  The strategies acting on
 * these are loaded independently of this driver, hence events.
 */
static void charger_partition_prepare(void)
{
	struct charger_partition_info_1 *info_1 = &g_chip->info_1;

	charger_partition_get_info_1();
	info_1->power_off_mode = CHARGER_PARTITION_MODE_DEFAULT;
	info_1->zero_speed_mode = CHARGER_PARTITION_MODE_DEFAULT;
	info_1->reserved = 0;
	charger_partition_set_info_1();

	charger_partition_get_info_1();
	if (info_1->double85)
		mca_event_block_notify(MCA_EVENT_TYPE_DEBUG,
				       MCA_EVENT_DEBUG_CTRL_DOUBLE85,
				       &info_1->double85);
	if (info_1->remove_temp_limit)
		mca_event_block_notify(MCA_EVENT_TYPE_DEBUG,
				       MCA_EVENT_DEBUG_CTRL_REMOVE_TEMP_LIMIT,
				       &info_1->remove_temp_limit);
	if (info_1->memory_test)
		mca_event_block_notify(MCA_EVENT_TYPE_DEBUG,
				       MCA_EVENT_DEBUG_CTRL_MEMORY_TEST,
				       &info_1->memory_test);
	if (info_1->soc_limit)
		mca_event_block_notify(MCA_EVENT_TYPE_DEBUG,
				       MCA_EVENT_DEBUG_CTRL_SOC_LIMIT,
				       &info_1->soc_limit);

	charger_partition_get_info_2();
	if (g_chip->info_2.eu_mode == 1) {
		mca_strategy_func_process(STRATEGY_FUNC_TYPE_FG,
					  MCA_EVENT_IS_EU_MODEL, 1);
		mca_strategy_func_process(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					  MCA_EVENT_IS_EU_MODEL,
					  g_chip->info_2.eu_mode);
		mca_strategy_func_process(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					  MCA_EVENT_IS_EU_MODEL,
					  g_chip->info_2.eu_mode);
	}
}

/*
 * Find the partition named "charger" on the storage device: its block number
 * is what every access is addressed by.
 */
static bool get_charger_partition_info(void)
{
	struct scsi_device *sdev = g_chip->sdev;
	struct block_device *part;
	struct gendisk *disk;

	if (!sdev->request_queue) {
		mca_log_err("scsi disk is null\n");
		return false;
	}

	disk = sdev->request_queue->disk;
	if (!disk) {
		mca_log_err("gendisk is null\n");
		return false;
	}

	g_chip->part_info_part_name = disk->disk_name;
	mca_log_err("partion: %s part_number:%d\n", g_chip->part_info_part_name,
		    g_chip->part_info_part_number);

	part = xa_load(&disk->part_tbl, g_chip->part_info_part_number);
	if (!part) {
		mca_log_err("device is null\n");
		return false;
	}

	if (!part->bd_meta_info ||
	    strncmp(part->bd_meta_info->volname, PARTITION_NAME,
		    sizeof(PARTITION_NAME))) {
		mca_log_err("[charger] this is not lun0, volname is %s\n",
			    part->bd_meta_info ?
			    (const char *)part->bd_meta_info->volname :
			    "(none)");
		return false;
	}

	/*
	 * The block layer counts in 512-byte sectors and the partition is
	 * addressed in 4096-byte blocks.
	 */
	g_partition_start = part->bd_start_sect >>
			    (ilog2(CHARGER_PARTITION_BLOCK_SIZE) - SECTOR_SHIFT);

	mca_log_err("partion: %s start %llu(block) size %llu(block)\n",
		    g_chip->part_info_part_name, (u64)g_partition_start,
		    (u64)bdev_nr_sectors(part) >>
		    (ilog2(CHARGER_PARTITION_BLOCK_SIZE) - SECTOR_SHIFT));

	return true;
}

/*
 * Put the header in a known state at boot: the magic, one information block
 * announced, and the partition free -- whatever an interrupted writer left.
 */
static int check_charger_partition_header(void)
{
	struct charger_partition_header *header;
	int ret;
	void *buf;

	/*
	 * Zeroed, because the whole block is written back below: anything the
	 * read did not cover must not go to the partition as heap contents.
	 */
	buf = kzalloc(CHARGER_PARTITION_BLOCK_SIZE, GFP_KERNEL);
	if (!buf) {
		mca_log_err("malloc buf error!\n");
		return -1;
	}

	ret = charger_scsi_read_partition(g_chip->sdev, buf, g_partition_start);
	if (ret) {
		mca_log_err("charger read error %d\n", ret);
		goto err;
	}

	header = buf;
	mca_log_err("magic:0x%0x, version:%d\n", header->magic,
		    header->version);

	if (header->magic != CHARGER_PARTITION_MAGIC) {
		mca_log_err("magic error, set to default!\n");
		header->magic = CHARGER_PARTITION_MAGIC;
	}

	header->info_num = 1;
	header->avaliable = 1;

	ret = charger_scsi_write_partition(g_chip->sdev, buf, g_partition_start);
	if (ret) {
		mca_log_err("charger write error %d\n", ret);
		goto err;
	}

	mca_log_err("initiablized ok\n");
	kfree(buf);

	return 0;

err:
	kfree(buf);

	return -1;
}

/* Hold @sdev as the storage device, dropping the one held before. */
static void charger_partition_set_sdev(struct scsi_device *sdev)
{
	if (g_chip->sdev)
		scsi_device_put(g_chip->sdev);

	g_chip->sdev = sdev;
}

/*
 * Look for the partition on one LUN of the first SCSI host.
 *
 * Return: 1 when it was found (and the header set up), 0 when it is not on
 * this LUN, -ENODEV when the host is not the UFS controller.
 */
static int charger_partition_probe_lun(int lun)
{
	struct scsi_device *sdev;
	struct Scsi_Host *shost;
	const char *proc_name;

	shost = scsi_host_lookup(0);
	if (!shost) {
		mca_log_err("not find, continue...\n");
		return 0;
	}

	sdev = scsi_device_lookup(shost, 0, 0, lun);
	scsi_host_put(shost);
	if (!sdev) {
		mca_log_err("not find, continue...\n");
		return 0;
	}

	charger_partition_set_sdev(sdev);

	proc_name = sdev->host->hostt->proc_name;
	if (strncmp(proc_name, UFSHCD, sizeof(UFSHCD) - 1)) {
		mca_log_err("proc name is not ufshcd, name: %s\n", proc_name);
		return -ENODEV;
	}

	if (!get_charger_partition_info())
		return 0;

	return 1;
}

/*
 * The partition was found.  The first time, only the second block is read
 * (the EU model flag is wanted early) and the partition is marked not ready
 * again, so that the retry below runs prepare() a poll period later; from
 * then on prepare() runs right away.
 */
static void charger_partition_found(void)
{
	check_charger_partition_header();
	g_chip->is_charger_partition_rdy = true;

	if (!g_chip->not_notify_module) {
		charger_partition_prepare();
	} else {
		charger_partition_get_info_2();
		g_chip->is_charger_partition_rdy = false;
		g_chip->not_notify_module = false;
	}
}

/*
 * Storage comes up after this driver does, so look for it from a worker and
 * come back later if it is not there yet.
 */
static void charger_partition_work(struct work_struct *work)
{
	struct mca_hwid_info *hwid = mca_get_hwid_info();
	int lun;
	int ret;

	mca_log_err("get hw_country_ver: %u\n", get_hw_country_version());

	g_chip->part_info_part_number =
		(hwid && hwid->platform_version == CHARGER_PARTITION_PLATFORM_ALT) ?
		CHARGER_PARTITION_PART_NUMBER_ALT :
		CHARGER_PARTITION_PART_NUMBER;

	for (lun = 0; lun < CHARGER_PARTITION_LUN_MAX; lun++) {
		ret = charger_partition_probe_lun(lun);
		if (ret < 0) {
			mca_log_err("not find finally, won't read charger partition!!!\n");
			return;
		}

		if (ret > 0) {
			mca_log_err("get partition info ok\n");
			charger_partition_found();
			break;
		}
	}

	if (!g_chip->is_charger_partition_rdy) {
		if (charger_partition_retry++ >= CHARGER_PARTITION_POLL_MAX)
			return;

		mca_log_err("charger partition not ready, retry: %d/%d\n",
			    charger_partition_retry,
			    CHARGER_PARTITION_POLL_MAX);
		queue_delayed_work(system_wq, &g_chip->charger_partition_work,
				   msecs_to_jiffies(CHARGER_PARTITION_POLL_MS));
	}

	/* One last look at LUN 0 once the retries have run out. */
	if (charger_partition_retry == CHARGER_PARTITION_POLL_MAX &&
	    charger_partition_probe_lun(0) > 0) {
		mca_log_err("get partition info ok\n");
		charger_partition_found();
	}
}

static int charger_partition_probe(struct platform_device *pdev)
{
	struct charger_partition_chip *chip;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip) {
		mca_log_err("out of memory\n");
		return -ENOMEM;
	}

	chip->dev = &pdev->dev;
	platform_set_drvdata(pdev, chip);
	g_chip = chip;

	mca_sysfs_init_attrs(charger_partition_sysfs_attrs,
			     charger_partition_sysfs_field_tbl,
			     ARRAY_SIZE(charger_partition_sysfs_field_tbl));
	mca_sysfs_create_link_group(MCA_SYSFS_DEV_CHARGER, "charger_partition",
				    chip->dev,
				    &charger_partition_sysfs_attr_group);

	chip->not_notify_module = true;
	INIT_DELAYED_WORK(&chip->charger_partition_work,
			  charger_partition_work);
	queue_delayed_work(system_wq, &chip->charger_partition_work, 0);

	mca_log_err("probe ok\n");

	return 0;
}

/*
 * The chip is devm memory and goes with the device; the worker is stopped
 * before that, and the storage device reference dropped.
 */
static int charger_partition_remove(struct platform_device *pdev)
{
	struct charger_partition_chip *chip = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&chip->charger_partition_work);
	mca_sysfs_remove_link_group(MCA_SYSFS_DEV_CHARGER, "charger_partition",
				    chip->dev,
				    &charger_partition_sysfs_attr_group);
	if (chip->sdev)
		scsi_device_put(chip->sdev);
	g_chip = NULL;
	kfree(g_rw_buf);
	g_rw_buf = NULL;

	return 0;
}

static const struct of_device_id match_table[] = {
	{ .compatible = "xiaomi,charger_partition" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver charger_partition_driver = {
	.driver = {
		.name		= "charger_partition",
		.of_match_table	= match_table,
	},
	.probe		= charger_partition_probe,
	.remove		= charger_partition_remove,
};
module_platform_driver(charger_partition_driver);

MODULE_DESCRIPTION("smart charge driver");
MODULE_AUTHOR("liluting@xiaomi.com");
MODULE_LICENSE("GPL v2");
