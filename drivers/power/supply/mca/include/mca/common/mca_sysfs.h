/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Where the charging stack shows itself to userspace.
 *
 * The stack is many modules that each own a few attributes, and userspace
 * wants them gathered by what they describe rather than by which driver
 * happens to provide them.  So a class is created on first use and devices
 * are added to it by name -- "charger", "battery", "typec" -- and any module
 * can add attributes to a device another module created.
 */

#ifndef __MCA_SYSFS_H
#define __MCA_SYSFS_H

#include <linux/device.h>
#include <linux/sysfs.h>

/* The class the whole stack lives under. */
#define MCA_SYSFS_CLASS			"xm_power"

/* The devices attributes are gathered under. */
#define MCA_SYSFS_DEV_CHARGER		"charger"
#define MCA_SYSFS_DEV_FUELGAUGE		"fuelgauge"
#define MCA_SYSFS_DEV_TYPEC		"typec"
#define MCA_SYSFS_DEV_BATTERY		"battery"
#define MCA_SYSFS_DEV_HW_MONITOR	"hw_monitor"

/*
 * The same five, numbered.  Xiaomi's own sources refer to them this way and
 * the numbering is theirs; both spellings name the same device.
 */
#define SYSFS_DEV_1			MCA_SYSFS_DEV_CHARGER
#define SYSFS_DEV_2			MCA_SYSFS_DEV_FUELGAUGE
#define SYSFS_DEV_3			MCA_SYSFS_DEV_TYPEC
#define SYSFS_DEV_4			MCA_SYSFS_DEV_BATTERY
#define SYSFS_DEV_5			MCA_SYSFS_DEV_HW_MONITOR

/**
 * struct mca_sysfs_attr_info - one attribute in a driver's table
 * @attr:		the attribute as sysfs sees it
 * @sysfs_attr_name:	the caller's own identifier for it, which its show and
 *			store handlers switch on
 */
struct mca_sysfs_attr_info {
	struct device_attribute	attr;
	int			sysfs_attr_name;
};

/* A read-only attribute served by <prefix>_show(). */
#define mca_sysfs_attr_ro(prefix, mode, id, name)			\
	{								\
		.attr = __ATTR(name, mode, prefix##_show, NULL),		\
		.sysfs_attr_name = (id),				\
	}

/* A write-only attribute served by <prefix>_store(). */
#define mca_sysfs_attr_wo(prefix, mode, id, name)			\
	{								\
		.attr = __ATTR(name, mode, NULL, prefix##_store),	\
		.sysfs_attr_name = (id),				\
	}

/* A read/write attribute served by <prefix>_show() and <prefix>_store(). */
#define mca_sysfs_attr_rw(prefix, mode, id, name)			\
	{								\
		.attr = __ATTR(name, mode, prefix##_show,		\
			       prefix##_store),				\
		.sysfs_attr_name = (id),				\
	}

/**
 * struct mca_debugfs_attr_info - one file in a driver's debugfs directory
 * @name:		what the file is called
 * @mode:		its permissions
 * @debugfs_attr_name:	the caller's own identifier for it
 * @show:		fills @buf with what the file should read; returns how
 *			much it wrote
 * @store:		acts on what was written; returns how much it took,
 *			may be NULL
 *
 * The callbacks take a plain buffer rather than the file itself: the core
 * does the copying to and from userspace, so a driver adding a debug file
 * writes the same code it would for a sysfs attribute.
 */
struct mca_debugfs_attr_info {
	const char	*name;
	umode_t		mode;
	int		debugfs_attr_name;
	ssize_t		(*show)(void *data, char *buf);
	ssize_t		(*store)(void *data, const char *buf, size_t count);
};

/**
 * struct mca_debugfs_attr_data - what a debugfs callback is handed
 * @attr_info:	the attribute being read or written, so one pair of handlers
 *		can serve a driver's whole table
 * @private:	what the driver registered
 */
struct mca_debugfs_attr_data {
	struct mca_debugfs_attr_info	*attr_info;
	void				*private;
};

/* One debug file, served by <prefix>_show() and <prefix>_store(). */
#define mca_debugfs_attr(prefix, _mode, id, _name)			\
	{								\
		.name = __stringify(_name),				\
		.mode = (_mode),					\
		.debugfs_attr_name = (id),				\
		.show = prefix##_show,					\
		.store = prefix##_store,				\
	}

struct device *mca_sysfs_create_group(const char *cls_name,
				      const char *dev_name,
				      const struct attribute_group *group);
void mca_sysfs_remove_group(const char *cls_name, struct device *dev,
			    const struct attribute_group *group);

int mca_sysfs_create_link_group(const char *dev_name, const char *link_name,
				struct device *target_dev,
				const struct attribute_group *group);
void mca_sysfs_remove_link_group(const char *dev_name, const char *link_name,
				 struct device *target_dev,
				 const struct attribute_group *group);

int mca_sysfs_create_files(const char *dev_name,
			   struct mca_sysfs_attr_info *attr_info, int size);

void mca_sysfs_init_attrs(struct attribute **attrs,
			  struct mca_sysfs_attr_info *attr_info, int size);
struct mca_sysfs_attr_info *mca_sysfs_lookup_attr(const char *name,
						  struct mca_sysfs_attr_info *attr_info,
						  int size);

int mca_debugfs_create_group(const char *name,
			     struct mca_debugfs_attr_info *attr_info, int size,
			     void *data);

#endif /* __MCA_SYSFS_H */
