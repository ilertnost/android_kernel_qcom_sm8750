// SPDX-License-Identifier: GPL-2.0-only
/*
 * MiTEE call path.
 *
 * Calls into the TEE are queued as tasks and handed to a single worker
 * thread. The worker writes the OP-TEE message argument into the request
 * ring, which lives in memory shared with the VM, and kicks the VM with a
 * yielding call over the message queue. When the VM returns, the response
 * ring holds either the result of the call or an RPC request, which is
 * served before resuming the call.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": %s: " fmt, __func__

#include <linux/err.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/wait.h>

#include "mitee_msgq.h"
#include "mitee_private.h"

#define MITEE_QUEUE_SIZE		(3 * PAGE_SIZE)
#define MITEE_RING_SLOTS		16

/* Layout of the queue memory */
#define MITEE_QUEUE_RX_HDR		0x0
#define MITEE_QUEUE_TX_HDR		0x800
#define MITEE_QUEUE_RX_DATA		0x1000
#define MITEE_QUEUE_TX_DATA		0x2000

#define MITEE_MSG_MAGIC			0x4d49	/* "MI" */
#define MITEE_MSG_VERSION		1

/* Message types from Linux to the TEE */
#define MITEE_MSG_CALL			0x1
#define MITEE_MSG_RPC_RETURN		0x2

/* Message types from the TEE to Linux */
#define MITEE_MSG_CALL_DONE		0x80000001
#define MITEE_MSG_RPC			0x80000002

/* Notifier event sent each time the TEE returns to Linux */
#define MITEE_CALL_RETURNED		1

struct mitee_ring_hdr {
	u64 head;	/* next slot written by the producer */
	u64 tail;	/* next slot read by the consumer */
	u64 size;	/* number of slots */
};

struct mitee_msg {
	u32 magic;
	u32 version;
	u32 reserved;
	u32 task_id;
	u32 type;
	u8 data[MITEE_MSG_DATA_SIZE];
} __packed;

static_assert(sizeof(struct mitee_msg) == 0x100);

struct mitee_task {
	struct list_head link;
	struct tee_context *ctx;
	struct completion done;
	struct optee_msg_arg *arg;
	int id;
	int rc;
};

int mitee_msg_queue_init(struct mitee_msg_queue *queue)
{
	void *va;

	va = alloc_pages_exact(MITEE_QUEUE_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!va) {
		pr_err("message queue alloc fail\n");
		return -ENOMEM;
	}

	queue->va = va;
	queue->pa = virt_to_phys(va);
	queue->size = MITEE_QUEUE_SIZE;

	mutex_init(&queue->rx.lock);
	queue->rx.hdr = va + MITEE_QUEUE_RX_HDR;
	queue->rx.data = va + MITEE_QUEUE_RX_DATA;
	queue->rx.hdr->size = MITEE_RING_SLOTS;

	mutex_init(&queue->tx.lock);
	queue->tx.hdr = va + MITEE_QUEUE_TX_HDR;
	queue->tx.data = va + MITEE_QUEUE_TX_DATA;
	queue->tx.hdr->size = MITEE_RING_SLOTS;

	pr_info("mitee allocated task queue pa: %pa va: %p\n", &queue->pa, va);
	return 0;
}

void mitee_msg_queue_exit(struct mitee_msg_queue *queue)
{
	free_pages_exact(queue->va, queue->size);
	mutex_destroy(&queue->rx.lock);
	mutex_destroy(&queue->tx.lock);
	pr_info("mitee message queue destroyed\n");
}

/* The queue memory is lent to the VM once it is up */
int mitee_msg_queue_register(struct mitee *mitee)
{
	struct mitee_regs regs = {
		.a0 = MITEE_CALL_SET_QUEUE,
		.a1 = mitee->queue.pa,
		.a2 = mitee->queue.size,
	};
	int rc;

	rc = mitee_msgq_call(&regs);
	if (rc)
		pr_err("register msg buf failed:%d\n", rc);

	return rc;
}

static int mitee_msg_enqueue(struct mitee_ring *ring, u32 task_id, u32 type,
			     const void *data, size_t size)
{
	struct mitee_ring_hdr *hdr = ring->hdr;
	struct mitee_msg *msg;
	u64 head;

	if (size > MITEE_MSG_DATA_SIZE) {
		pr_err("invalid param\n");
		return -EINVAL;
	}

	mutex_lock(&ring->lock);
	head = READ_ONCE(hdr->head);
	if ((head + 1) % hdr->size == READ_ONCE(hdr->tail)) {
		mutex_unlock(&ring->lock);
		pr_err("tx buf full\n");
		return -EBUSY;
	}

	msg = ring->data + head * sizeof(*msg);
	memset(msg, 0, sizeof(*msg));
	msg->magic = MITEE_MSG_MAGIC;
	msg->version = MITEE_MSG_VERSION;
	msg->task_id = task_id;
	msg->type = type;
	memcpy(msg->data, data, size);

	/* Publish the slot before moving the head */
	smp_wmb();
	WRITE_ONCE(hdr->head, (head + 1) % hdr->size);
	mutex_unlock(&ring->lock);

	return 0;
}

static int mitee_msg_dequeue(struct mitee_ring *ring, struct mitee_msg *out)
{
	struct mitee_ring_hdr *hdr = ring->hdr;
	struct mitee_msg *msg;
	u64 tail;

	mutex_lock(&ring->lock);
	tail = READ_ONCE(hdr->tail);
	if (tail == READ_ONCE(hdr->head) || tail >= hdr->size) {
		mutex_unlock(&ring->lock);
		pr_err("rx buf empty\n");
		return -EBUSY;
	}

	/* Read the slot only after seeing the head that covers it */
	smp_rmb();
	msg = ring->data + tail * sizeof(*msg);
	memcpy(out, msg, sizeof(*out));
	memset(msg, 0, sizeof(*msg));
	WRITE_ONCE(hdr->tail, (tail + 1) % hdr->size);
	mutex_unlock(&ring->lock);

	if (out->magic != MITEE_MSG_MAGIC) {
		pr_err("invalid magic\n");
		return -EINVAL;
	}

	if (out->version != MITEE_MSG_VERSION) {
		pr_err("invalid version\n");
		return -EINVAL;
	}

	return 0;
}

void mitee_task_list_init(struct mitee_task_list *tasks)
{
	mutex_init(&tasks->lock);
	idr_init(&tasks->idr);
	INIT_LIST_HEAD(&tasks->pending);
}

void mitee_task_list_exit(struct mitee_task_list *tasks)
{
	idr_destroy(&tasks->idr);
	mutex_destroy(&tasks->lock);
}

/**
 * mitee_task_call() - pass a call to the worker and wait for its result
 * @ctx:	calling context
 * @arg:	message argument, updated with the result of the call
 *
 * Calls are serialized: the TEE serves one call at a time.
 */
int mitee_task_call(struct tee_context *ctx, struct optee_msg_arg *arg)
{
	struct mitee *mitee = tee_get_drvdata(ctx->teedev);
	struct mitee_task_list *tasks = &mitee->tasks;
	struct mitee_task task = {
		.ctx = ctx,
		.arg = arg,
	};
	int rc;

	init_completion(&task.done);

	down(&mitee->call_sem);

	mutex_lock(&tasks->lock);
	task.id = idr_alloc(&tasks->idr, &task, 1, INT_MAX, GFP_KERNEL);
	if (task.id > 0)
		list_add_tail(&task.link, &tasks->pending);
	mutex_unlock(&tasks->lock);

	if (task.id < 0) {
		pr_err("%s:mitee task alloc failed\n", __func__);
		rc = task.id;
		goto out;
	}

	wake_up_interruptible(&mitee->worker_wq);
	wait_for_completion(&task.done);
	rc = task.rc;

	mutex_lock(&tasks->lock);
	idr_remove(&tasks->idr, task.id);
	mutex_unlock(&tasks->lock);
out:
	up(&mitee->call_sem);
	return rc;
}

static struct mitee_task *mitee_task_next(struct mitee_task_list *tasks)
{
	struct mitee_task *task;

	mutex_lock(&tasks->lock);
	task = list_first_entry_or_null(&tasks->pending, struct mitee_task,
					link);
	if (task)
		list_del_init(&task->link);
	mutex_unlock(&tasks->lock);

	return task;
}

/* Run the request ring until the TEE is done with it */
static int mitee_run_queue(struct mitee *mitee)
{
	struct mitee_regs regs = { .a0 = MITEE_CALL_PROCESS_QUEUE };
	int rc;

	while (true) {
		rc = mitee_msgq_call(&regs);
		if (rc)
			return rc;

		atomic_notifier_call_chain(&mitee->call_notifier,
					   MITEE_CALL_RETURNED, NULL);

		if (regs.a0 != MITEE_CALL_RET_YIELD)
			return 0;

		regs = (struct mitee_regs){ .a0 = MITEE_CALL_RESUME };
	}
}

static int mitee_yielding_call(struct mitee *mitee, struct mitee_task *task)
{
	union {
		struct optee_msg_arg arg;
		u8 buf[MITEE_MSG_DATA_SIZE];
	} rpc;
	struct mitee_msg *msg;
	struct mitee_task *done;
	size_t size;
	u32 task_id = task->id;
	u32 type = MITEE_MSG_CALL;
	const void *data = task->arg;
	int rc;

	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	size = OPTEE_MSG_GET_ARG_SIZE(task->arg->num_params);

	while (true) {
		rc = mitee_msg_enqueue(&mitee->queue.tx, task_id, type, data,
				       size);
		if (rc) {
			pr_err("msg enqueue err: %d\n", rc);
			break;
		}

		rc = mitee_run_queue(mitee);
		if (rc)
			break;

		rc = mitee_msg_dequeue(&mitee->queue.rx, msg);
		if (rc) {
			pr_err("msg recv dequeue err: %d\n", rc);
			break;
		}

		if (msg->type == MITEE_MSG_RPC) {
			memcpy(rpc.buf, msg->data, sizeof(rpc.buf));
			rpc.arg.ret_origin = TEEC_ORIGIN_COMMS;
			if (rpc.arg.num_params > MITEE_MSG_MAX_PARAMS) {
				rpc.arg.num_params = 0;
				rpc.arg.ret = TEEC_ERROR_BAD_PARAMETERS;
			} else {
				optee_rpc_cmd(task->ctx, mitee, &rpc.arg);
			}

			task_id = msg->task_id;
			type = MITEE_MSG_RPC_RETURN;
			data = &rpc.arg;
			size = OPTEE_MSG_GET_ARG_SIZE(rpc.arg.num_params);
			continue;
		}

		if (msg->type != MITEE_MSG_CALL_DONE) {
			pr_err("invalid msg type: %#x\n", msg->type);
			rc = -EINVAL;
			break;
		}

		mutex_lock(&mitee->tasks.lock);
		done = idr_find(&mitee->tasks.idr, msg->task_id);
		mutex_unlock(&mitee->tasks.lock);
		if (done != task) {
			pr_warn("task id invalid, maybe task aborted\n");
			rc = -EINVAL;
			break;
		}

		/* The result never grows the argument */
		memcpy(task->arg, msg->data,
		       min_t(size_t, OPTEE_MSG_GET_ARG_SIZE(task->arg->num_params),
			     MITEE_MSG_DATA_SIZE));
		break;
	}

	kfree(msg);
	return rc;
}

int mitee_worker(void *data)
{
	struct mitee *mitee = data;
	struct mitee_task *task;

	while (!kthread_should_stop()) {
		if (wait_event_interruptible(mitee->worker_wq,
					     !list_empty(&mitee->tasks.pending) ||
					     kthread_should_stop()))
			continue;

		task = mitee_task_next(&mitee->tasks);
		if (!task)
			continue;

		task->rc = mitee_yielding_call(mitee, task);
		complete(&task->done);
	}

	return 0;
}
