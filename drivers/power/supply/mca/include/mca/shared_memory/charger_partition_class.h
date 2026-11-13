/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * The charger partition: what the charging stack remembers across boots.
 *
 * A few facts about a battery outlive any one boot -- how many times it was
 * driven under voltage, whether the phone is a European model whose charging
 * is capped by regulation, whether a factory test is in progress.  They live
 * in a small raw partition that the bootloader, the ADSP, the kernel and
 * userspace all read, so the layout below is shared rather than private.
 *
 * The partition is a sequence of 4096-byte blocks: a header, then one block
 * per information block.  Because several processors reach it, a writer takes
 * the header's availability flag before touching anything and releases it
 * afterwards -- charger_partition_alloc() and charger_partition_dealloc().
 */

#ifndef __CHARGER_PARTITION_H
#define __CHARGER_PARTITION_H

#include <linux/types.h>

/* Who is asking.  The flag in the header records that someone is writing. */
enum charger_partition_host_type {
	CHARGER_PARTITION_HOST_LK,
	CHARGER_PARTITION_HOST_UEFI,
	CHARGER_PARTITION_HOST_ABL,
	CHARGER_PARTITION_HOST_ADSP,
	CHARGER_PARTITION_HOST_KERNEL,
	CHARGER_PARTITION_HOST_HAL,
	CHARGER_PARTITION_HOST_FRAMEWORK,
	CHARGER_PARTITION_HOST_APPLICATION,
	CHARGER_PARTITION_HOST_LAST,
	CHARGER_PARTITION_HOST_INVALID,
};

/* Which block.  The header is the first, each information block follows. */
enum charger_partition_info_type {
	CHARGER_PARTITION_HEADER,
	CHARGER_PARTITION_INFO_1,
	CHARGER_PARTITION_INFO_2,
	CHARGER_PARTITION_INFO_3,
	CHARGER_PARTITION_INFO_4,
	CHARGER_PARTITION_INFO_5,
	CHARGER_PARTITION_INFO_6,
	CHARGER_PARTITION_INFO_7,
	CHARGER_PARTITION_INFO_8,
	CHARGER_PARTITION_INFO_9,
	CHARGER_PARTITION_INFO_10,
	CHARGER_PARTITION_INFO_LAST,
	CHARGER_PARTITION_INFO_INVALID,
};

/**
 * struct charger_partition_header - the first block
 * @magic:     %CHARGER_PARTITION_MAGIC once the partition has been set up
 * @version:   layout version
 * @info_num:  how many information blocks follow
 * @avaliable: clear while someone is writing, set when the partition is free
 * @reserved:  kept zero
 *
 * The misspelling of @avaliable is the name every other reader of this
 * partition uses, so it is kept.
 */
struct charger_partition_header {
	u32 magic;
	u32 version;
	u32 info_num;
	u32 avaliable;
	u32 reserved;
};

#define CHARGER_PARTITION_MAGIC		0x20240725

/**
 * struct charger_partition_info_1 - what the phone was told to do
 * @power_off_mode:    charging behaviour while powered off
 * @zero_speed_mode:   the battery was shipped in storage mode
 * @mishow:            demo-unit mode
 * @double85:          the 85 degree, 85 percent humidity soak test is running
 * @remove_temp_limit: temperature limits are lifted for a test
 * @soc_limit:         cap the state of charge at this percentage, -1 for none
 * @memory_test:       a memory test is running and may not be interrupted
 * @reserved:          kept zero
 */
struct charger_partition_info_1 {
	u32 power_off_mode;
	u32 zero_speed_mode;
	u32 mishow;
	u32 double85;
	u32 remove_temp_limit;
	u32 soc_limit;
	u32 memory_test;
	u32 reserved;
};

/**
 * struct charger_partition_info_2 - what the battery has been through
 * @eu_mode:     the phone is a European model, whose charging is capped
 * @ocd_count:   times the battery was drawn over its current limit
 * @hocd_count:  times it was drawn far enough over to matter
 * @cuv_count:   times the cell was taken under voltage
 * @hcuv_count:  times it was taken far enough under to matter
 * @hscd_count:  times a hard short circuit was detected
 * @reserved:    kept zero
 *
 * These counts are what a later failure is explained by, so they are only
 * ever added to.
 */
struct charger_partition_info_2 {
	u32 eu_mode;
	u32 ocd_count;
	u32 hocd_count;
	u32 cuv_count;
	u32 hcuv_count;
	u32 hscd_count;
	u32 reserved[2];
};

int charger_partition_alloc(enum charger_partition_host_type host,
			    enum charger_partition_info_type info, u32 size);
int charger_partition_dealloc(enum charger_partition_host_type host,
			      enum charger_partition_info_type info, u32 size);
void *charger_partition_read(enum charger_partition_host_type host,
			     enum charger_partition_info_type info, u32 size);
int charger_partition_write(enum charger_partition_host_type host,
			    enum charger_partition_info_type info, void *buf,
			    u32 size);

int charger_partition_get_eu_model(bool *is_eu_model);
int charger_partition_get_mishow(bool *mishow);

int charger_partition_read_double85(int *double85);
int charger_partition_write_double85(int double85);
int charger_partition_read_remove_temp_limit(int *remove_temp_limit);
int charger_partition_write_remove_temp_limit(int remove_temp_limit);
int charger_partition_read_memory_test(int *memory_test);
int charger_partition_write_memory_test(int memory_test);
int charger_partition_read_soc_limit(int *soc_limit);
int charger_partition_write_soc_limit(int soc_limit);

int charger_partition_read_ocd_count(int *ocd_count, int *hocd_count);
int charger_partition_write_ocd_count(int ocd_count, int hocd_count);
int charger_partition_read_cuv_count(int *cuv_count, int *hcuv_count);
int charger_partition_write_cuv_count(int cuv_count, int hcuv_count);
int charger_partition_read_hscd_count(int *hscd_count);
int charger_partition_write_hcsd_count(int hscd_count);

#endif /* __CHARGER_PARTITION_H */
