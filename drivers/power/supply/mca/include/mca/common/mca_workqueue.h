/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * A workqueue of the charging stack's own.
 *
 * Charging work can be slow -- a transfer over I2C to a charger, a round trip
 * to the fuel gauge -- and putting it on the system queue lets it hold up
 * everything else queued there.  These wrappers put it on a queue that only
 * the charging drivers use.
 */

#ifndef __MCA_WORKQUEUE_H
#define __MCA_WORKQUEUE_H

#include <linux/workqueue.h>

bool mca_queue_work(struct work_struct *work);
bool mca_queue_delayed_work(struct delayed_work *dwork, unsigned long delay);
bool mca_mod_delayed_work(struct delayed_work *dwork, unsigned long delay);
bool mca_cancel_work(struct work_struct *work);
bool mca_cancel_delayed_work(struct delayed_work *dwork);

#endif /* __MCA_WORKQUEUE_H */
