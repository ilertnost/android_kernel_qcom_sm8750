// SPDX-License-Identifier: GPL-2.0-only
/*
 * MiTEE notifications.
 *
 * MITEE_CALL_NOTIFY packs the request in a1:
 *   [31:0]  connection id or token
 *   [39:32] notification type
 *   [47:40] operation
 *   [63:48] payload length
 * and up to 24 bytes of payload in a2..a4.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bits.h>
#include <linux/export.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include "mitee_msgq.h"
#include "mitee_notify.h"

#define MITEE_NOTIFY_OP_CONNECT		1
#define MITEE_NOTIFY_OP_SIGNAL		2
#define MITEE_NOTIFY_OP_DATA_CONT	3
#define MITEE_NOTIFY_OP_DATA		4

#define MITEE_NOTIFY_MAX_PAYLOAD	24

static u64 mitee_notify_arg(u8 type, u32 id, u8 op, u16 len)
{
	return id | (u64)type << 32 | (u64)op << 40 | (u64)len << 48;
}

/**
 * mitee_smc_notify_connect() - open a notification channel
 * @type:	notification type
 * @id:		id of the service in the TEE
 *
 * Returns the token of the channel, or a negative value on failure.
 */
int mitee_smc_notify_connect(u8 type, u32 id)
{
	struct mitee_regs regs = {
		.a0 = MITEE_CALL_NOTIFY,
		.a1 = mitee_notify_arg(type, id, MITEE_NOTIFY_OP_CONNECT, 0),
	};
	int rc;

	rc = mitee_msgq_call(&regs);
	if (!rc)
		rc = (int)regs.a0;
	if (rc == -1)
		pr_err("mitee smc notify connect failed\n");

	return rc;
}
EXPORT_SYMBOL_GPL(mitee_smc_notify_connect);

/**
 * mitee_smc_notify_signal() - send a signal over a notification channel
 * @type:	notification type
 * @token:	channel token from mitee_smc_notify_connect()
 * @signal:	signal number
 */
int mitee_smc_notify_signal(u8 type, u32 token, u8 signal)
{
	struct mitee_regs regs = {
		.a0 = MITEE_CALL_NOTIFY,
		.a1 = mitee_notify_arg(type, token, MITEE_NOTIFY_OP_SIGNAL, 1),
		.a2 = signal,
	};
	int rc;

	rc = mitee_msgq_call(&regs);
	if (rc)
		pr_err("mitee smc signal failed\n");

	return rc;
}
EXPORT_SYMBOL_GPL(mitee_smc_notify_signal);

/**
 * mitee_smc_notify_data() - send data over a notification channel
 * @type:	notification type
 * @token:	channel token from mitee_smc_notify_connect()
 * @data:	data to send
 * @len:	length of @data
 *
 * The first call carries the total length, the following ones the length
 * of their chunk.
 */
int mitee_smc_notify_data(u8 type, u32 token, const void *data, u16 len)
{
	struct mitee_regs regs;
	u64 payload[3];
	u16 done = 0, n;
	u8 op = MITEE_NOTIFY_OP_DATA;

	do {
		n = min_t(u16, len - done, MITEE_NOTIFY_MAX_PAYLOAD);
		memset(payload, 0, sizeof(payload));
		memcpy(payload, data + done, n);

		regs = (struct mitee_regs){
			.a0 = MITEE_CALL_NOTIFY,
			.a1 = mitee_notify_arg(type, token, op,
					       op == MITEE_NOTIFY_OP_DATA ? len : n),
			.a2 = payload[0],
			.a3 = payload[1],
			.a4 = payload[2],
		};
		mitee_msgq_call(&regs);

		done += n;
		op = MITEE_NOTIFY_OP_DATA_CONT;
	} while (done < len);

	return 0;
}
EXPORT_SYMBOL_GPL(mitee_smc_notify_data);
