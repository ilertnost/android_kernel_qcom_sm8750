/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 */

#ifndef __MCA_LOG_H
#define __MCA_LOG_H

#include <linux/compiler_attributes.h>
#include <linux/printk.h>
#include <linux/types.h>

/*
 * Every caller tags its messages, and every message carries the function and
 * line it came from: a charging log is read long after the fact, by someone
 * matching it against the source.  A caller may set its own tag before
 * including this header; the module name is a reasonable default.
 */
#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG KBUILD_MODNAME
#endif

/*
 * How much a message matters.  Errors always go to the kernel log and the
 * charging log; info and debug messages go to each according to the
 * console_log_level and log_level attributes of xm_power/charge_log.
 */
enum mca_log_level {
	MCA_LOG_LEVEL_ERROR,
	MCA_LOG_LEVEL_INFO,
	MCA_LOG_LEVEL_DEBUG,
	MCA_LOG_MAX_LEVEL,
};

/*
 * The uevent the charging log sends when a buffer has filled and userspace
 * should collect it.  A fault reported through mca_charge_mievent sends the
 * same event, so that the log covering the fault is collected with it.
 */
#define MCA_LOG_FULL_EVENT	"MCA_LOG_FULL_EVENT"

__printf(1, 2) void __mca_log_err(const char *fmt, ...);
__printf(1, 2) void __mca_log_info(const char *fmt, ...);
__printf(1, 2) void __mca_log_debug(const char *fmt, ...);

#define mca_log_err(fmt, ...)						\
	__mca_log_err("[" MCA_LOG_TAG "]%s:%d " fmt, __func__, __LINE__, \
		      ##__VA_ARGS__)
#define mca_log_info(fmt, ...)						\
	__mca_log_info("[" MCA_LOG_TAG "]%s:%d " fmt, __func__, __LINE__, \
		       ##__VA_ARGS__)
#define mca_log_debug(fmt, ...)						\
	__mca_log_debug("[" MCA_LOG_TAG "]%s:%d " fmt, __func__, __LINE__, \
			##__VA_ARGS__)

/*
 * The sources the charge log is assembled from.  The order is the order the
 * columns appear in, which is why a source is identified by its place here
 * rather than by a name it chooses for itself.
 */
enum mca_charge_log_id_ele {
	MCA_CHARGE_LOG_ID_BUSINESS_CHG,
	MCA_CHARGE_LOG_ID_THERMAL,
	MCA_CHARGE_LOG_ID_BATTERY_INFO,
	MCA_CHARGE_LOG_ID_FG_MASTER_IC,
	MCA_CHARGE_LOG_ID_FG_SLAVE_IC,
	MCA_CHARGE_LOG_ID_CP_MASTER_IC,
	MCA_CHARGE_LOG_ID_CP_SLAVE_IC,
	MCA_CHARGE_LOG_ID_USCP,
	MCA_CHARGE_LOG_ID_MAX,
};

/**
 * struct mca_log_charge_log_ops - how a source writes itself into the log
 * @dump_log_head:    writes the column headings
 * @dump_log_context: writes one line of values under them
 *
 * The log is a table, so the headings and the values come from the same
 * source and have to agree on their widths; keeping them in one place is what
 * makes that possible.  Both are given a buffer and its size and return how
 * much they wrote.
 */
struct mca_log_charge_log_ops {
	int (*dump_log_head)(void *data, char *buf, int size);
	int (*dump_log_context)(void *data, char *buf, int size);
};

/**
 * mca_log_charge_log_register() - add a source to the charge log
 * @id:   which column group the source fills
 * @ops:  how it writes itself
 * @data: passed back to @ops
 */
void mca_log_charge_log_register(enum mca_charge_log_id_ele id,
				 struct mca_log_charge_log_ops *ops,
				 void *data);

/**
 * mca_log_get_charge_boot_mode() - whether this is a charging-only boot
 *
 * The value of the mca_log.charge_boot_mode module parameter, which the
 * bootloader passes on the kernel command line.
 */
int mca_log_get_charge_boot_mode(void);

#endif /* __MCA_LOG_H */
