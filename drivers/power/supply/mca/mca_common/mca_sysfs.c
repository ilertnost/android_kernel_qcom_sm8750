// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Where the charging stack shows itself to userspace.  See
 * include/mca/common/mca_sysfs.h.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <mca/common/mca_sysfs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

/* The debugfs directory the whole stack writes under. */
#define MCA_DEBUGFS_ROOT	"charger_debug"

/* Size of the buffers debugfs reads and writes go through. */
#define MCA_DEBUGFS_BUF_SIZE	4096

/*
 * The devices the charging stack shows itself through.  They are created up
 * front rather than by whichever driver happens to want one first, because
 * the attributes are spread across a dozen modules and every one of them
 * expects to find its device already there.
 */
static const char * const g_mca_sysfs_dev_name_list[] = {
	MCA_SYSFS_DEV_CHARGER,
	MCA_SYSFS_DEV_FUELGAUGE,
	MCA_SYSFS_DEV_TYPEC,
	MCA_SYSFS_DEV_BATTERY,
	MCA_SYSFS_DEV_HW_MONITOR,
};

/**
 * struct mca_sysfs_dev_node - one device in a class
 * @node: links it into its class's device list
 * @name: what the device is called
 * @dev:  the device itself
 */
struct mca_sysfs_dev_node {
	struct list_head	node;
	const char		*name;
	struct device		*dev;
};

/**
 * struct mca_sysfs_class_node - one class
 * @node:            links it into the list of classes
 * @name:            what the class is called
 * @mca_class:       the class itself
 * @dev_list_header: the devices created under it
 */
struct mca_sysfs_class_node {
	struct list_head	node;
	const char		*name;
	struct class		*mca_class;
	struct list_head	dev_list_header;
};

static LIST_HEAD(g_mca_sysfs_list_header);
static DEFINE_MUTEX(g_mca_sysfs_class_lock);
static DEFINE_MUTEX(g_mca_debugfs_dir_lock);

static struct dentry *g_mca_dbg_dir;

static struct mca_sysfs_class_node *mca_sysfs_find_class(const char *cls_name)
{
	struct mca_sysfs_class_node *cls;

	list_for_each_entry(cls, &g_mca_sysfs_list_header, node) {
		if (!strcmp(cls->name, cls_name))
			return cls;
	}

	return NULL;
}

/*
 * Modules load in whatever order the dependency graph allows, so whichever
 * one asks for a class first creates it and the rest find it.
 */
static struct mca_sysfs_class_node *
mca_sysfs_get_or_create_class(const char *cls_name)
{
	struct mca_sysfs_class_node *cls;

	mutex_lock(&g_mca_sysfs_class_lock);

	cls = mca_sysfs_find_class(cls_name);
	if (cls)
		goto out;

	cls = kzalloc(sizeof(*cls), GFP_KERNEL);
	if (!cls)
		goto out;

	cls->name = cls_name;
	cls->mca_class = class_create(cls_name);
	if (IS_ERR_OR_NULL(cls->mca_class)) {
		kfree(cls);
		cls = NULL;
		goto out;
	}

	INIT_LIST_HEAD(&cls->dev_list_header);
	list_add_tail(&cls->node, &g_mca_sysfs_list_header);

out:
	mutex_unlock(&g_mca_sysfs_class_lock);

	return cls;
}

/* The device called @dev_name in the xm_power class, or NULL. */
static struct device *mca_sysfs_find_dev(const char *dev_name)
{
	struct mca_sysfs_class_node *cls;
	struct mca_sysfs_dev_node *dev_node;
	struct device *dev = NULL;

	mutex_lock(&g_mca_sysfs_class_lock);

	cls = mca_sysfs_find_class(MCA_SYSFS_CLASS);
	if (!cls || !cls->mca_class)
		goto out;

	list_for_each_entry(dev_node, &cls->dev_list_header, node) {
		if (!strcmp(dev_node->name, dev_name)) {
			dev = dev_node->dev;
			break;
		}
	}

out:
	mutex_unlock(&g_mca_sysfs_class_lock);

	return dev;
}

/**
 * mca_sysfs_create_group() - create a device and give it a group of attributes
 * @cls_name: the class it belongs to, created if this is the first caller
 * @dev_name: what the device is called
 * @group:    the attributes
 *
 * Return: the device, which later callers reach by name, or NULL.
 */
struct device *mca_sysfs_create_group(const char *cls_name,
				      const char *dev_name,
				      const struct attribute_group *group)
{
	struct mca_sysfs_class_node *cls;
	struct mca_sysfs_dev_node *dev_node;
	struct device *dev;

	if (!cls_name || !dev_name || !group)
		return NULL;

	cls = mca_sysfs_get_or_create_class(cls_name);
	if (!cls) {
		pr_err("can not find class %s\n", cls_name);
		return NULL;
	}

	dev = device_create(cls->mca_class, NULL, 0, NULL, "%s", dev_name);
	if (IS_ERR_OR_NULL(dev)) {
		pr_err("create device %s fai\n", dev_name);
		return NULL;
	}

	if (sysfs_create_group(&dev->kobj, group)) {
		pr_err("%s creat sys group fail\n", dev_name);
		goto err;
	}

	dev_node = kzalloc(sizeof(*dev_node), GFP_KERNEL);
	if (!dev_node)
		goto err;

	dev_node->name = dev_name;
	dev_node->dev = dev;

	mutex_lock(&g_mca_sysfs_class_lock);
	list_add_tail(&dev_node->node, &cls->dev_list_header);
	mutex_unlock(&g_mca_sysfs_class_lock);

	pr_info("group %s/%s create succ\n", cls_name, dev_name);

	return dev;

err:
	/*
	 * device_unregister() drops the only reference device_create() took;
	 * an extra put_device() here would free the device before it is
	 * unregistered.
	 */
	device_unregister(dev);

	return NULL;
}
EXPORT_SYMBOL(mca_sysfs_create_group);

void mca_sysfs_remove_group(const char *cls_name, struct device *dev,
			    const struct attribute_group *group)
{
	struct mca_sysfs_class_node *cls;
	struct mca_sysfs_dev_node *dev_node, *tmp;

	if (!dev || !group)
		return;

	sysfs_remove_group(&dev->kobj, group);

	mutex_lock(&g_mca_sysfs_class_lock);

	cls = cls_name ? mca_sysfs_find_class(cls_name) : NULL;
	if (cls) {
		list_for_each_entry_safe(dev_node, tmp, &cls->dev_list_header,
					 node) {
			if (dev_node->dev != dev)
				continue;

			list_del(&dev_node->node);
			kfree(dev_node);
			break;
		}
	}

	mutex_unlock(&g_mca_sysfs_class_lock);

	device_unregister(dev);
}
EXPORT_SYMBOL(mca_sysfs_remove_group);

/**
 * mca_sysfs_create_link_group() - add a subdirectory to an existing device
 * @dev_name:   the xm_power device to add it to
 * @link_name:  what the subdirectory is called
 * @target_dev: the device whose attributes it holds
 * @group:      the attributes
 *
 * The group is created on @target_dev and a link to it named @link_name is
 * added to the xm_power device, so that userspace finds every attribute of
 * the stack under one class however many drivers provide them.
 *
 * Return: 0, or -1 if the device does not exist or a step failed.
 */
int mca_sysfs_create_link_group(const char *dev_name, const char *link_name,
				struct device *target_dev,
				const struct attribute_group *group)
{
	struct device *dev;

	if (!dev_name || !link_name || !target_dev || !group)
		return -1;

	dev = mca_sysfs_find_dev(dev_name);
	if (!dev)
		return -1;

	if (sysfs_create_group(&target_dev->kobj, group))
		return -1;

	if (sysfs_create_link(&dev->kobj, &target_dev->kobj, link_name))
		return -1;

	pr_info("creat %s/%s success\n", dev_name, link_name);

	return 0;
}
EXPORT_SYMBOL(mca_sysfs_create_link_group);

void mca_sysfs_remove_link_group(const char *dev_name, const char *link_name,
				 struct device *target_dev,
				 const struct attribute_group *group)
{
	struct device *dev;

	if (!dev_name || !link_name || !target_dev || !group)
		return;

	dev = mca_sysfs_find_dev(dev_name);
	if (!dev)
		return;

	sysfs_remove_link(&dev->kobj, link_name);
	sysfs_remove_group(&target_dev->kobj, group);
}
EXPORT_SYMBOL(mca_sysfs_remove_link_group);

/**
 * mca_sysfs_create_files() - add attributes to a device another module made
 * @dev_name:  the xm_power device to add them to
 * @attr_info: the attributes
 * @size:      how many
 *
 * The device must already exist: a module that owns a handful of attributes
 * on the charger does not create the charger.  An attribute that cannot be
 * created is skipped and the rest are still added.
 *
 * Return: the result of creating the last attribute, or -1 if the device does
 * not exist.
 */
int mca_sysfs_create_files(const char *dev_name,
			   struct mca_sysfs_attr_info *attr_info, int size)
{
	struct device *dev;
	int i, ret = 0;

	if (!dev_name || !attr_info)
		return -1;

	dev = mca_sysfs_find_dev(dev_name);
	if (!dev)
		return -1;

	for (i = 0; i < size; i++) {
		ret = sysfs_create_file(&dev->kobj, &attr_info[i].attr.attr);
		if (ret) {
			pr_err("creat %s file failed\n",
			       attr_info[i].attr.attr.name);
			sysfs_remove_file(&dev->kobj, &attr_info[i].attr.attr);
		}
	}

	return ret;
}
EXPORT_SYMBOL(mca_sysfs_create_files);

/**
 * mca_sysfs_init_attrs() - build an attribute array from a driver's table
 * @attrs:     where to put them, with room for @size plus a terminator
 * @attr_info: the table
 * @size:      how many entries it has
 */
void mca_sysfs_init_attrs(struct attribute **attrs,
			  struct mca_sysfs_attr_info *attr_info, int size)
{
	int i;

	if (!attrs || !attr_info || size < 0)
		return;

	for (i = 0; i < size; i++)
		attrs[i] = &attr_info[i].attr.attr;

	attrs[size] = NULL;
}
EXPORT_SYMBOL(mca_sysfs_init_attrs);

/**
 * mca_sysfs_lookup_attr() - find the table entry a handler was called for
 * @name:      the attribute's name, as sysfs passed it
 * @attr_info: the table
 * @size:      how many entries it has
 *
 * Return: the entry, whose @sysfs_attr_name says which attribute it is, or
 * NULL.
 */
struct mca_sysfs_attr_info *mca_sysfs_lookup_attr(const char *name,
						  struct mca_sysfs_attr_info *attr_info,
						  int size)
{
	int i;

	if (!name || !attr_info)
		return NULL;

	for (i = 0; i < size; i++) {
		if (!strcmp(name, attr_info[i].attr.attr.name))
			return &attr_info[i];
	}

	return NULL;
}
EXPORT_SYMBOL(mca_sysfs_lookup_attr);

static int mca_debugfs_template_show(struct seq_file *s, void *unused)
{
	struct mca_debugfs_attr_data *d = s->private;
	char *buf;
	int rc;

	if (!d || !d->attr_info->show) {
		pr_err("invalid show\n");
		return -EINVAL;
	}

	buf = kzalloc(MCA_DEBUGFS_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	rc = d->attr_info->show(d, buf);
	if (rc > 0)
		seq_write(s, buf, rc);

	kfree(buf);

	pr_err("show:%s ret=%d\n", d->attr_info->name, rc);

	return 0;
}

static int mca_debugfs_template_open(struct inode *inode, struct file *file)
{
	return single_open(file, mca_debugfs_template_show, inode->i_private);
}

static ssize_t mca_debugfs_template_write(struct file *file,
					  const char __user *buf, size_t count,
					  loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct mca_debugfs_attr_data *d = s->private;
	char *kbuf;
	int rc;

	if (!d || !d->attr_info->store) {
		pr_err("invalid store\n");
		return -EINVAL;
	}

	kbuf = kzalloc(MCA_DEBUGFS_BUF_SIZE, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	if (count >= MCA_DEBUGFS_BUF_SIZE) {
		pr_err("input too long\n");
		kfree(kbuf);
		return -ENOMEM;
	}

	if (copy_from_user(kbuf, buf, count)) {
		pr_err("can not copy data form user to kernel\n");
		kfree(kbuf);
		return -ENOSPC;
	}
	kbuf[count] = '\0';

	rc = d->attr_info->store(d, kbuf, count);
	kfree(kbuf);

	pr_err("store: %s ret=%d\n", d->attr_info->name, rc);

	return rc;
}

static const struct file_operations mca_debugfs_template_fops = {
	.owner		= THIS_MODULE,
	.open		= mca_debugfs_template_open,
	.read		= seq_read,
	.write		= mca_debugfs_template_write,
	.release	= single_release,
};

/**
 * mca_debugfs_create_group() - give a driver a directory of debugfs files
 * @name:      what the directory is called
 * @attr_info: the files
 * @size:      how many
 * @data:      passed back to every handler
 *
 * A driver that registers twice -- two instances of the same chip, say --
 * shares one directory rather than getting a second of the same name.
 *
 * Return: 0, or -1 on failure.
 */
int mca_debugfs_create_group(const char *name,
			     struct mca_debugfs_attr_info *attr_info, int size,
			     void *data)
{
	struct mca_debugfs_attr_data *d;
	struct dentry *dir, *file;
	bool looked_up = true;
	int i, ret = 0;

	if (!g_mca_dbg_dir) {
		pr_err("root directory is null\n");
		return -1;
	}

	mutex_lock(&g_mca_debugfs_dir_lock);

	dir = debugfs_lookup(name, g_mca_dbg_dir);
	if (IS_ERR_OR_NULL(dir)) {
		looked_up = false;
		dir = debugfs_create_dir(name, g_mca_dbg_dir);
	}
	if (IS_ERR_OR_NULL(dir)) {
		pr_err("%s dir create fail\n", name);
		mutex_unlock(&g_mca_debugfs_dir_lock);
		return -1;
	}

	mutex_unlock(&g_mca_debugfs_dir_lock);

	if (size < 0) {
		ret = -1;
		goto out;
	}

	/*
	 * One handle per file, kept for as long as the files exist: the
	 * handlers are given it on every read and write.
	 */
	d = kcalloc(size, sizeof(*d), GFP_KERNEL);
	if (!d) {
		ret = -1;
		goto out;
	}

	for (i = 0; i < size; i++) {
		d[i].attr_info = &attr_info[i];
		d[i].private = data;

		file = debugfs_create_file(attr_info[i].name, attr_info[i].mode,
					   dir, &d[i],
					   &mca_debugfs_template_fops);
		if (IS_ERR_OR_NULL(file)) {
			pr_err("%s file create fail\n", attr_info[i].name);
			ret = -1;
			break;
		}
	}

out:
	/* debugfs_lookup() hands back a reference of its own. */
	if (looked_up)
		dput(dir);

	return ret;
}
EXPORT_SYMBOL(mca_debugfs_create_group);

static int __init mca_sysfs_init(void)
{
	struct mca_sysfs_class_node *cls;
	struct mca_sysfs_dev_node *dev_node;
	struct device *dev;
	int i;

	g_mca_dbg_dir = debugfs_create_dir(MCA_DEBUGFS_ROOT, NULL);
	if (IS_ERR(g_mca_dbg_dir)) {
		pr_err("Failed to create charger debugfs root directory: %d\n",
		       (int)PTR_ERR(g_mca_dbg_dir));
		g_mca_dbg_dir = NULL;
	}

	cls = mca_sysfs_get_or_create_class(MCA_SYSFS_CLASS);
	if (!cls)
		return 0;

	for (i = 0; i < ARRAY_SIZE(g_mca_sysfs_dev_name_list); i++) {
		dev_node = kzalloc(sizeof(*dev_node), GFP_KERNEL);
		if (!dev_node)
			return 0;

		dev_node->name = g_mca_sysfs_dev_name_list[i];
		dev = device_create(cls->mca_class, NULL, 0, NULL, "%s",
				    dev_node->name);
		if (IS_ERR_OR_NULL(dev)) {
			pr_err("create device %s fail\n", dev_node->name);
			kfree(dev_node);
			continue;
		}
		dev_node->dev = dev;

		mutex_lock(&g_mca_sysfs_class_lock);
		list_add_tail(&dev_node->node, &cls->dev_list_header);
		mutex_unlock(&g_mca_sysfs_class_lock);
	}

	return 0;
}

static void __exit mca_sysfs_exit(void)
{
	debugfs_remove(g_mca_dbg_dir);
}

module_init(mca_sysfs_init);
module_exit(mca_sysfs_exit);

MODULE_DESCRIPTION("sysfs for xiaomi power driver");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
