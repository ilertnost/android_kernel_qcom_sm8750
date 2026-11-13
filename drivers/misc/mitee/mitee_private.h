/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015-2021, Linaro Limited
 *
 * MiTEE is an OP-TEE OS instance running in a Gunyah guest VM ("oemvm").
 * The driver below is the OP-TEE driver of Linux 5.16 (FF-A flavour of the
 * parameter handling) with the secure monitor transport replaced by a
 * Gunyah message queue plus a pair of request/response rings in memory that
 * is lent to the VM.
 */

#ifndef MITEE_PRIVATE_H
#define MITEE_PRIVATE_H

#include <linux/completion.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/semaphore.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "optee_msg.h"
#include "tee_drv.h"

/* Some Global Platform error codes used in this driver */
#define TEEC_SUCCESS			0x00000000
#define TEEC_ERROR_BAD_PARAMETERS	0xFFFF0006
#define TEEC_ERROR_NOT_SUPPORTED	0xFFFF000A
#define TEEC_ERROR_COMMUNICATION	0xFFFF000E
#define TEEC_ERROR_OUT_OF_MEMORY	0xFFFF000C
#define TEEC_ERROR_BUSY			0xFFFF000D
#define TEEC_ERROR_SHORT_BUFFER		0xFFFF0010

#define TEEC_ORIGIN_COMMS		0x00000002

/*
 * A message carries the OP-TEE message argument struct inline, which limits
 * the number of parameters of a single call.
 */
#define MITEE_MSG_DATA_SIZE		0xec
#define MITEE_MSG_MAX_PARAMS \
	((MITEE_MSG_DATA_SIZE - sizeof(struct optee_msg_arg)) / \
	 sizeof(struct optee_msg_param))

struct optee_wait_queue {
	/* Serializes access to this struct */
	struct mutex mu;
	struct list_head db;
};

struct optee_supp {
	/* Serializes access to this struct */
	struct mutex mutex;
	struct tee_context *ctx;

	int req_id;
	struct list_head reqs;
	struct idr idr;
	struct completion reqs_c;
};

/*
 * Handlers for OPTEE_RPC_CMD_MITEE_CALLBACK requests, looked up by the
 * (client, command) pair carried in the first value parameter.
 */
typedef int (*mitee_rpc_callback_t)(u64 *value, void *buf, size_t size,
				    u32 *out_size);

struct mitee_rpc_callback_queue {
	/* Serializes access to this struct */
	struct mutex mutex;
	struct list_head callbacks;
};

/**
 * struct mitee_ring - single producer, single consumer message ring
 * @lock:	serializes access to the ring from this side
 * @hdr:	ring header, shared with the VM
 * @data:	ring slots, shared with the VM
 */
struct mitee_ring {
	struct mutex lock;
	struct mitee_ring_hdr *hdr;
	void *data;
};

/**
 * struct mitee_msg_queue - memory holding the request and response rings
 * @pa:		physical address of the queue memory
 * @size:	size of the queue memory
 * @va:		kernel address of the queue memory
 * @tx:		requests from Linux to the TEE
 * @rx:		responses and RPC requests from the TEE to Linux
 */
struct mitee_msg_queue {
	phys_addr_t pa;
	size_t size;
	void *va;
	struct mitee_ring tx;
	struct mitee_ring rx;
};

/**
 * struct mitee_task_list - calls waiting to be passed to the TEE
 * @lock:	serializes access to this struct
 * @idr:	tasks by id, the id is passed to the TEE with the call
 * @pending:	tasks not yet picked up by the worker
 */
struct mitee_task_list {
	struct mutex lock;
	struct idr idr;
	struct list_head pending;
};

/**
 * struct mitee - main service struct
 * @supp_teedev:	supplicant device
 * @teedev:		client device
 * @pool:		shared memory pool, NULL if the TEE has none
 * @pool_va:		kernel mapping of the shared memory pool
 * @wait_queue:		OPTEE_RPC_CMD_WAIT_QUEUE handling
 * @callbacks:		OPTEE_RPC_CMD_MITEE_CALLBACK handlers
 * @supp:		supplicant synchronization struct for RPC to supplicant
 * @queue:		request/response rings shared with the VM
 * @tasks:		calls waiting for the worker
 * @call_sem:		serializes calls into the TEE
 * @worker:		thread passing calls and RPCs between Linux and the TEE
 * @worker_wq:		wakes up @worker when a call is queued
 * @call_notifier:	called each time the TEE returns to Linux
 */
struct mitee {
	struct tee_device *supp_teedev;
	struct tee_device *teedev;
	struct tee_shm_pool *pool;
	void *pool_va;
	struct optee_wait_queue wait_queue;
	struct mitee_rpc_callback_queue callbacks;
	struct optee_supp supp;
	struct mitee_msg_queue queue;
	struct mitee_task_list tasks;
	struct semaphore call_sem;
	struct task_struct *worker;
	wait_queue_head_t worker_wq;
	struct atomic_notifier_head call_notifier;
};

struct optee_session {
	struct list_head list_node;
	u32 session_id;
};

struct optee_context_data {
	/* Serializes access to this struct */
	struct mutex mutex;
	struct list_head sess_list;
};

extern struct mitee *mitee_svc;

/* mitee_core.c */
int optee_to_msg_param(struct mitee *mitee, struct optee_msg_param *msg_params,
		       size_t num_params, const struct tee_param *params);
int optee_from_msg_param(struct mitee *mitee, struct tee_param *params,
			 size_t num_params,
			 const struct optee_msg_param *msg_params);

/* optee_call.c */
int optee_do_call_with_arg(struct tee_context *ctx, struct optee_msg_arg *arg);
int optee_open_session(struct tee_context *ctx,
		       struct tee_ioctl_open_session_arg *arg,
		       struct tee_param *param);
int optee_close_session_helper(struct tee_context *ctx, u32 session);
int optee_close_session(struct tee_context *ctx, u32 session);
int optee_invoke_func(struct tee_context *ctx, struct tee_ioctl_invoke_arg *arg,
		      struct tee_param *param);
int optee_cancel_req(struct tee_context *ctx, u32 cancel_id, u32 session);
int optee_check_mem_type(unsigned long start, size_t num_pages);

/* optee_rpc.c */
void optee_wait_queue_init(struct optee_wait_queue *wq);
void optee_wait_queue_exit(struct optee_wait_queue *wq);
void mitee_rpc_callback_queue_init(struct mitee_rpc_callback_queue *queue);
void mitee_rpc_callback_queue_exit(struct mitee_rpc_callback_queue *queue);
int mitee_rpc_register_callback(u32 client, u32 cmd,
				mitee_rpc_callback_t callback);
struct tee_shm *optee_rpc_cmd_alloc_suppl(struct tee_context *ctx, size_t sz);
void optee_rpc_cmd_free_suppl(struct tee_context *ctx, struct tee_shm *shm);
void optee_rpc_cmd(struct tee_context *ctx, struct mitee *mitee,
		   struct optee_msg_arg *arg);

/* optee_supp.c */
u32 optee_supp_thrd_req(struct tee_context *ctx, u32 func, size_t num_params,
			struct tee_param *param);
void optee_supp_init(struct optee_supp *supp);
void optee_supp_uninit(struct optee_supp *supp);
void optee_supp_release(struct optee_supp *supp);
int optee_supp_recv(struct tee_context *ctx, u32 *func, u32 *num_params,
		    struct tee_param *param);
int optee_supp_send(struct tee_context *ctx, u32 ret, u32 num_params,
		    struct tee_param *param);

/* mitee_task.c */
int mitee_msg_queue_init(struct mitee_msg_queue *queue);
void mitee_msg_queue_exit(struct mitee_msg_queue *queue);
int mitee_msg_queue_register(struct mitee *mitee);
void mitee_task_list_init(struct mitee_task_list *tasks);
void mitee_task_list_exit(struct mitee_task_list *tasks);
int mitee_task_call(struct tee_context *ctx, struct optee_msg_arg *arg);
int mitee_worker(void *data);

/* mitee_memlog.c */
int mitee_memlog_init(struct mitee *mitee);
void mitee_memlog_exit(struct mitee *mitee);

static inline void optee_from_msg_param_value(struct tee_param *p, u32 attr,
					      const struct optee_msg_param *mp)
{
	p->attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT +
		  attr - OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
	p->u.value.a = mp->u.value.a;
	p->u.value.b = mp->u.value.b;
	p->u.value.c = mp->u.value.c;
}

static inline void optee_to_msg_param_value(struct optee_msg_param *mp,
					    const struct tee_param *p)
{
	mp->attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT + p->attr -
		   TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	mp->u.value.a = p->u.value.a;
	mp->u.value.b = p->u.value.b;
	mp->u.value.c = p->u.value.c;
}

#endif /* MITEE_PRIVATE_H */
