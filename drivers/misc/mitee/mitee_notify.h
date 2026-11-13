/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MiTEE notifications: short messages from normal world drivers to services
 * in the TEE, passed as fast calls.
 */

#ifndef MITEE_NOTIFY_H
#define MITEE_NOTIFY_H

#include <linux/types.h>

int mitee_smc_notify_connect(u8 type, u32 id);
int mitee_smc_notify_signal(u8 type, u32 token, u8 signal);
int mitee_smc_notify_data(u8 type, u32 token, const void *data, u16 len);

#endif /* MITEE_NOTIFY_H */
