/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Gunyah message queue transport between Linux and the MiTEE VM.
 */

#ifndef MITEE_MSGQ_H
#define MITEE_MSGQ_H

#include <linux/types.h>

#include "optee_msg.h"
#include "tee_drv.h"

/**
 * struct mitee_regs - arguments and results of a call into the TEE VM
 *
 * Calls look like secure monitor calls: @a0 selects the function. Calls with
 * bit 31 of @a0 set enter the TEE (yielding calls), the others are fast calls
 * served without scheduling a TEE thread. A few fast calls are answered by
 * the transport itself.
 */
struct mitee_regs {
	u64 a0;
	u64 a1;
	u64 a2;
	u64 a3;
	u64 a4;
};

/* Fast calls answered locally by the transport */
#define MITEE_CALL_GET_SHM_POOL		0x0a	/* a0 = pa, a1 = size, a2 = 1 */
#define MITEE_CALL_GET_LOG_BUF		0x0b	/* a0 = pa, a1 = size */
#define MITEE_CALL_SET_QUEUE		0x0c	/* a1 = pa, a2 = size */

/* Fast calls passed to the VM */
#define MITEE_CALL_NOTIFY		0x0d

/* Yielding calls: process the request ring, resume after a yield */
#define MITEE_CALL_PROCESS_QUEUE	0x80000000
#define MITEE_CALL_RESUME		0x80000002
#define MITEE_CALL_RET_YIELD		2

/* VM state as seen by the transport */
#define MITEE_VM_STATE_DOWN		0
#define MITEE_VM_STATE_READY		1	/* VM booted, memory shared */
#define MITEE_VM_STATE_CONNECTED	2	/* message queue handshake done */
#define MITEE_VM_STATE_RUNNING		3	/* OP-TEE initialized */

int mitee_msgq_driver_init(void);
void mitee_msgq_driver_exit(void);

int mitee_msgq_vm_state(void);
int mitee_msgq_call(struct mitee_regs *regs);

int mitee_msgq_to_msg_param(struct optee_msg_param *mp,
			    const struct tee_param *p);
void mitee_msgq_from_msg_param(struct tee_param *p, u32 attr,
			       const struct optee_msg_param *mp);

int mitee_msgq_shm_register(struct tee_context *ctx, struct tee_shm *shm,
			    struct page **pages, size_t num_pages,
			    unsigned long start);
int mitee_msgq_shm_unregister(struct tee_context *ctx, struct tee_shm *shm);
int mitee_msgq_shm_unregister_supp(struct tee_context *ctx,
				   struct tee_shm *shm);

#endif /* MITEE_MSGQ_H */
