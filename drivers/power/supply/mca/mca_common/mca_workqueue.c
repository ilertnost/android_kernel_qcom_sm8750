// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * A workqueue of the charging stack's own.  See
 * include/mca/common/mca_workqueue.h.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#include <mca/common/mca_log.h>
#include <mca/common/mca_workqueue.h>

static struct workqueue_struct *mca_wq;

bool mca_queue_work(struct work_struct *work)
{
	if (!mca_wq || !work)
		return false;

	return queue_work(mca_wq, work);
}
EXPORT_SYMBOL(mca_queue_work);

bool mca_queue_delayed_work(struct delayed_work *dwork, unsigned long delay)
{
	if (!mca_wq || !dwork)
		return false;

	return queue_delayed_work(mca_wq, dwork, delay);
}
EXPORT_SYMBOL(mca_queue_delayed_work);

bool mca_mod_delayed_work(struct delayed_work *dwork, unsigned long delay)
{
	if (!mca_wq || !dwork)
		return false;

	return mod_delayed_work(mca_wq, dwork, delay);
}
EXPORT_SYMBOL(mca_mod_delayed_work);

bool mca_cancel_work(struct work_struct *work)
{
	if (!work)
		return false;

	return cancel_work(work);
}
EXPORT_SYMBOL(mca_cancel_work);

bool mca_cancel_delayed_work(struct delayed_work *dwork)
{
	if (!dwork)
		return false;

	return cancel_delayed_work(dwork);
}
EXPORT_SYMBOL(mca_cancel_delayed_work);

static int __init mca_workqueue_init(void)
{
	/*
	 * Unbound so a charging work item is not tied to the CPU that queued
	 * it, and marked for memory reclaim so the charging path keeps
	 * running when the system is short of memory.
	 */
	mca_wq = alloc_workqueue("mca_wq",
				 WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_CPU_INTENSIVE,
				 0);
	if (!mca_wq) {
		mca_log_err("alloc_workqueue failed\n");
		return -ENOMEM;
	}

	mca_log_err("mca workqueue init done\n");

	return 0;
}

static void __exit mca_workqueue_exit(void)
{
	destroy_workqueue(mca_wq);
}

module_init(mca_workqueue_init);
module_exit(mca_workqueue_exit);

MODULE_DESCRIPTION("mca workqueue driver");
MODULE_AUTHOR("muxinyi1@xiaomi.com");
MODULE_LICENSE("GPL v2");
