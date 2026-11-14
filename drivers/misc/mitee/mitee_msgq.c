// SPDX-License-Identifier: GPL-2.0-only
/*
 * Gunyah message queue transport between Linux and the MiTEE VM.
 *
 * The VM is booted by tee-supplicant. Once the resource manager reports it
 * ready, the static shared memory (shared memory pool and TEE log) is shared
 * with it. The VM then connects over the message queue, asks for the shared
 * memory and the request/response rings, and reports when OP-TEE is up.
 *
 * Calls are passed as register sets: fast calls (bit 31 of a0 clear) are
 * answered one at a time, yielding calls get their replies through a small
 * FIFO. Memory registered by TEE clients and memory lent to the TEE on its
 * request are handed over as Gunyah memory parcels.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/gfp.h>
#include <linux/gunyah/gh_msgq.h>
#include <linux/gunyah/gh_rm_drv.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/rhashtable.h>
#include <linux/scatterlist.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sizes.h>
#include <soc/qcom/secure_buffer.h>

#include "mitee_msgq.h"
#include "mitee_private.h"
#include "optee_rpc_cmd.h"

#define MITEE_MSGQ_MAGIC		0x5147534d4f414958ULL	/* "XIAOMSGQ" */
#define MITEE_MSGQ_ID_HLOS		0x8000
#define MITEE_MSGQ_ID_REPLY		0x8000

/* Message ids */
#define MITEE_MSGQ_CONTROL		0
#define MITEE_MSGQ_DATA			1
#define MITEE_MSGQ_FASTCALL		2
#define MITEE_MSGQ_YIELD_CALL		3
#define MITEE_MSGQ_YIELD_REPLY		4
#define MITEE_MSGQ_FASTCALL_REPLY	5

/* Control commands from the VM */
#define MITEE_MSGQ_CTRL_CONNECT		0
#define MITEE_MSGQ_CTRL_GET_SHM		1
#define MITEE_MSGQ_CTRL_TEE_READY	2

/* Shared memory description sent with MITEE_MSGQ_CTRL_GET_SHM */
#define MITEE_MSGQ_SHM_STATIC		5

#define MITEE_DEFAULT_SHM_SIZE		SZ_4M
#define MITEE_SHM_POOL_SIZE		SZ_2M
#define MITEE_SHM_TYPE_CACHED		1

#define MITEE_REPLY_SLOTS		32

/* Client 0 of OPTEE_RPC_CMD_MITEE_CALLBACK: memory lent to the TEE */
#define MITEE_RPC_CLIENT_SECMEM		0
#define MITEE_RPC_SECMEM_LEND		0
#define MITEE_RPC_SECMEM_RECLAIM	1

#define MITEE_MAX_ALLOC_ORDER		9

struct mitee_msgq_msg {
	__le64 magic;
	u32 msg_id;
	u32 size;		/* size of the payload */
	u16 client_id;
	u16 server_id;
	u32 cmd;
	u64 reserved[2];
	u64 regs[5];		/* payload */
} __packed;

#define MITEE_MSGQ_HDR_SIZE		offsetof(struct mitee_msgq_msg, regs)
#define MITEE_MSGQ_PAYLOAD_SIZE		sizeof_field(struct mitee_msgq_msg, regs)

struct mitee_reply_fifo {
	spinlock_t lock;
	wait_queue_head_t wq;
	unsigned int head;
	unsigned int tail;
	struct mitee_regs slots[MITEE_REPLY_SLOTS];
};

struct mitee_fastcall {
	struct mutex mutex;	/* one fast call at a time */
	spinlock_t lock;	/* protects @valid and @regs */
	struct completion done;
	bool valid;
	struct mitee_regs regs;
};

struct mitee_shm_entry {
	struct rhash_head linkage;
	u64 handle;
	struct tee_shm *shm;
	struct sg_table *sgt;
};

struct mitee_secmem {
	struct list_head link;
	struct sg_table *sgt;
	gh_memparcel_handle_t handle;
	size_t size;
};

struct mitee_msgq_dev {
	struct device *dev;

	/* static shared memory: shared memory pool followed by the TEE log */
	struct resource res;
	void *dma_va;
	gh_label_t label;
	gh_memparcel_handle_t shm_handle;

	/* request/response rings, see mitee_task.c */
	phys_addr_t queue_pa;
	size_t queue_size;
	gh_memparcel_handle_t queue_handle;
	bool queue_shared;

	u32 peer_name;
	gh_vmid_t self_vmid;
	gh_vmid_t peer_vmid;
	int vm_state;
	struct notifier_block rm_nb;

	void *msgq;
	struct task_struct *recv_task;
	struct mitee_reply_fifo replies;
	struct mitee_fastcall fastcall;

	struct mutex shm_mutex;		/* protects @shm_table */
	struct rhashtable shm_table;

	struct mutex secmem_mutex;	/* protects @secmem_list */
	struct list_head secmem_list;
};

static struct mitee_msgq_dev *mitee_qdev;

static const struct rhashtable_params mitee_shm_rhash_params = {
	.head_offset = offsetof(struct mitee_shm_entry, linkage),
	.key_len = sizeof_field(struct mitee_shm_entry, handle),
	.key_offset = offsetof(struct mitee_shm_entry, handle),
	.automatic_shrinking = true,
};

static int mitee_assign_mem(phys_addr_t addr, size_t size,
			    const gh_vmid_t *src, int nr_src,
			    const gh_vmid_t *dst, int perms, int nr_dst)
{
	struct qcom_scm_vmperm newvm[2];
	u64 srcvm = 0;
	int i;

	for (i = 0; i < nr_src; i++)
		srcvm |= BIT_ULL(src[i]);

	for (i = 0; i < nr_dst; i++) {
		newvm[i].vmid = dst[i];
		newvm[i].perm = perms;
	}

	return qcom_scm_assign_mem(addr, size, &srcvm, newvm, nr_dst);
}

static int mitee_assign_sgt(struct sg_table *sgt, const gh_vmid_t *src,
			    int nr_src, const gh_vmid_t *dst, int perms,
			    int nr_dst)
{
	u32 src_vms[2];
	int dst_vms[2];
	int dst_perms[2];
	int i;

	for (i = 0; i < nr_src; i++)
		src_vms[i] = src[i];

	for (i = 0; i < nr_dst; i++) {
		dst_vms[i] = dst[i];
		dst_perms[i] = perms;
	}

	return hyp_assign_table(sgt, src_vms, nr_src, dst_vms, dst_perms,
				nr_dst);
}

static struct gh_acl_desc *mitee_acl_desc(const gh_vmid_t *vmids, int nr,
					  u8 perms)
{
	struct gh_acl_desc *acl;
	int i;

	acl = kzalloc(struct_size(acl, acl_entries, nr), GFP_KERNEL);
	if (!acl)
		return NULL;

	acl->n_acl_entries = nr;
	for (i = 0; i < nr; i++) {
		acl->acl_entries[i].vmid = vmids[i];
		acl->acl_entries[i].perms = perms;
	}

	return acl;
}

static struct gh_sgl_desc *mitee_sgt_to_sgl_desc(struct sg_table *sgt)
{
	struct gh_sgl_desc *sgl;
	struct scatterlist *sg;
	int i;

	sgl = kzalloc(struct_size(sgl, sgl_entries, sgt->nents), GFP_KERNEL);
	if (!sgl) {
		pr_err("mitee msgq: generate sgl_desc failed, no memory\n");
		return NULL;
	}

	sgl->n_sgl_entries = sgt->nents;
	for_each_sgtable_sg(sgt, sg, i) {
		sgl->sgl_entries[i].ipa_base = page_to_phys(sg_page(sg));
		sgl->sgl_entries[i].size = sg->length;
	}

	return sgl;
}

/* Share physically contiguous memory with the VM, both sides read/write */
static int mitee_msgq_share_mem_contiguous(struct mitee_msgq_dev *qdev,
					   phys_addr_t addr, size_t size,
					   gh_label_t label,
					   gh_memparcel_handle_t *handle)
{
	const gh_vmid_t self[] = { qdev->self_vmid };
	const gh_vmid_t both[] = { qdev->self_vmid, qdev->peer_vmid };
	struct gh_acl_desc *acl;
	struct gh_sgl_desc *sgl;
	int rc;

	rc = mitee_assign_mem(addr, size, self, 1, both, PERM_READ | PERM_WRITE,
			      2);
	if (rc) {
		pr_err("%s: mitee msgq qcom_scm_assign_mem failed addr=%pa size=%zx err=%d\n",
		       __func__, &addr, size, rc);
		return rc;
	}

	acl = mitee_acl_desc(both, 2, GH_RM_ACL_R | GH_RM_ACL_W);
	sgl = kzalloc(struct_size(sgl, sgl_entries, 1), GFP_KERNEL);
	if (!acl || !sgl) {
		rc = -ENOMEM;
		goto err_unassign;
	}

	sgl->n_sgl_entries = 1;
	sgl->sgl_entries[0].ipa_base = addr;
	sgl->sgl_entries[0].size = size;

	rc = ghd_rm_mem_share(GH_RM_MEM_TYPE_NORMAL, 0, label, acl, sgl, NULL,
			      handle);
	if (rc) {
		pr_err("%s: mitee msgq gh_rm_mem_share failed addr=%pa size=%zu err=%d\n",
		       __func__, &addr, size, rc);
		goto err_unassign;
	}

	kfree(acl);
	kfree(sgl);
	return 0;

err_unassign:
	kfree(acl);
	kfree(sgl);
	mitee_assign_mem(addr, size, both, 2, self, PERM_READ | PERM_WRITE |
			 PERM_EXEC, 1);
	return rc;
}

static void mitee_msgq_unshare_mem_contiguous(struct mitee_msgq_dev *qdev,
					      phys_addr_t addr, size_t size,
					      gh_memparcel_handle_t handle)
{
	const gh_vmid_t self[] = { qdev->self_vmid };
	const gh_vmid_t both[] = { qdev->self_vmid, qdev->peer_vmid };
	int rc;

	rc = ghd_rm_mem_reclaim(handle, 0);
	if (rc)
		pr_err("%s: mitee msgq Gunyah reclaim failed\n", __func__);

	rc = mitee_assign_mem(addr, size, both, 2, self, PERM_READ |
			      PERM_WRITE | PERM_EXEC, 1);
	if (rc)
		pr_err("%s: mitee msgq qcom_scm_assign_mem failed addr=%pa size=%zx err=%d\n",
		       __func__, &addr, size, rc);
}

static void mitee_msgq_send(struct mitee_msgq_dev *qdev,
			    struct mitee_msgq_msg *msg, size_t size)
{
	int rc;

	rc = gh_msgq_send(qdev->msgq, msg, size, GH_MSGQ_TX_PUSH);
	if (rc)
		dev_err(qdev->dev, "mitee msgq: send failed: %d\n", rc);
}

static void mitee_msgq_handle_control(struct mitee_msgq_dev *qdev,
				      struct mitee_msgq_msg *msg)
{
	if (msg->server_id != MITEE_MSGQ_ID_HLOS) {
		pr_err("mitee msgq: unknown server id\n");
		return;
	}

	switch (msg->cmd) {
	case MITEE_MSGQ_CTRL_CONNECT:
		if (qdev->vm_state > MITEE_VM_STATE_READY)
			dev_err(qdev->dev, "mitee msgq: msgq state error\n");
		if (msg->size) {
			pr_err("mitee msgq: incorrect msg length\n");
			return;
		}
		msg->client_id |= MITEE_MSGQ_ID_REPLY;
		mitee_msgq_send(qdev, msg, MITEE_MSGQ_HDR_SIZE);
		qdev->vm_state = MITEE_VM_STATE_CONNECTED;
		break;
	case MITEE_MSGQ_CTRL_GET_SHM:
		if (msg->size)
			pr_err("mitee msgq: incorrect msg length\n");

		msg->client_id |= MITEE_MSGQ_ID_REPLY;
		msg->msg_id = MITEE_MSGQ_FASTCALL;
		msg->size = MITEE_MSGQ_PAYLOAD_SIZE;
		msg->regs[0] = MITEE_MSGQ_SHM_STATIC;
		msg->regs[1] = qdev->res.start;
		msg->regs[2] = MITEE_SHM_POOL_SIZE;
		msg->regs[3] = qdev->shm_handle;
		msg->regs[4] = qdev->label;
		mitee_msgq_send(qdev, msg, sizeof(*msg));

		/* Give the VM time to accept the static memory first */
		msleep(20);

		if (!qdev->queue_shared) {
			if (mitee_msgq_share_mem_contiguous(qdev,
							    qdev->queue_pa,
							    qdev->queue_size,
							    0,
							    &qdev->queue_handle))
				return;
			qdev->queue_shared = true;
		}

		msg->regs[0] = MITEE_CALL_SET_QUEUE;
		msg->regs[1] = qdev->queue_pa;
		msg->regs[2] = qdev->queue_size;
		msg->regs[3] = qdev->queue_handle;
		msg->regs[4] = 0;
		mitee_msgq_send(qdev, msg, sizeof(*msg));
		break;
	case MITEE_MSGQ_CTRL_TEE_READY:
		if (msg->size)
			pr_err("mitee msgq: incorrect msg length\n");
		qdev->vm_state = MITEE_VM_STATE_RUNNING;
		break;
	default:
		pr_info("mitee msgq: unknown msg_id 0x%x\n", msg->cmd);
		break;
	}
}

static void mitee_msgq_handle_yield_reply(struct mitee_msgq_dev *qdev,
					  struct mitee_msgq_msg *msg)
{
	struct mitee_reply_fifo *fifo = &qdev->replies;
	unsigned int next;

	if (qdev->vm_state != MITEE_VM_STATE_RUNNING)
		pr_err("mitee msgq process reply state error\n");

	if (msg->size != MITEE_MSGQ_PAYLOAD_SIZE) {
		pr_err("mitee msgq: incorrect data size for reply\n");
		return;
	}

	spin_lock(&fifo->lock);
	next = (fifo->head + 1) % MITEE_REPLY_SLOTS;
	if (next == fifo->tail) {
		spin_unlock(&fifo->lock);
		pr_err("mitee msgq: recv_queue is full\n");
		return;
	}
	memcpy(&fifo->slots[fifo->head], msg->regs, sizeof(msg->regs));
	fifo->head = next;
	spin_unlock(&fifo->lock);

	wake_up(&fifo->wq);
}

static void mitee_msgq_handle_fastcall_reply(struct mitee_msgq_dev *qdev,
					     struct mitee_msgq_msg *msg)
{
	struct mitee_fastcall *fc = &qdev->fastcall;

	if (qdev->vm_state < MITEE_VM_STATE_CONNECTED)
		pr_err("mitee msgq process fastcall state error\n");

	if (msg->size != MITEE_MSGQ_PAYLOAD_SIZE) {
		pr_err("mitee msgq: incorrect data size for fastcall reply\n");
		return;
	}

	spin_lock(&fc->lock);
	if (fc->valid) {
		spin_unlock(&fc->lock);
		pr_err("mitee msgq: recv_queue is full\n");
		return;
	}
	memcpy(&fc->regs, msg->regs, sizeof(msg->regs));
	fc->valid = true;
	spin_unlock(&fc->lock);

	complete(&fc->done);
}

static int mitee_msgq_recv_task(void *data)
{
	struct mitee_msgq_dev *qdev = data;
	struct mitee_msgq_msg *msg;
	size_t size;
	int rc;

	pr_info("mitee msgq: recv task start\n");

	msg = kmalloc(GH_MSGQ_MAX_MSG_SIZE_BYTES, GFP_KERNEL);
	if (!msg) {
		dev_err(qdev->dev, "mitee msgq: without enough memory");
		return -ENOMEM;
	}

	/*
	 * Let mitee_msgq_stop_recv_task() interrupt the blocking receive.
	 * Only kernel-sent signals are let through, so user space can not
	 * kill the thread and cut off every TEE reply.
	 */
	allow_kernel_signal(SIGKILL);

	while (!kthread_should_stop()) {
		memset(msg, 0, GH_MSGQ_MAX_MSG_SIZE_BYTES);
		size = 0;
		rc = gh_msgq_recv(qdev->msgq, msg, GH_MSGQ_MAX_MSG_SIZE_BYTES,
				  &size, 0);
		if (rc == -ERESTARTSYS || signal_pending(current))
			break;
		if (rc < 0 || size > GH_MSGQ_MAX_MSG_SIZE_BYTES ||
		    size < MITEE_MSGQ_HDR_SIZE) {
			dev_err(qdev->dev,
				"mitee msgq: Failed to receive the msg with status %d",
				rc);
			continue;
		}

		if (le64_to_cpu(msg->magic) != MITEE_MSGQ_MAGIC) {
			pr_err("mitee msgq: msgq data magic num is wrong!\n");
			continue;
		}

		if (msg->size > size - MITEE_MSGQ_HDR_SIZE) {
			pr_err("mitee msgq: msgq data length is too long!\n");
			continue;
		}

		switch (msg->msg_id) {
		case MITEE_MSGQ_CONTROL:
			mitee_msgq_handle_control(qdev, msg);
			break;
		case MITEE_MSGQ_DATA:
			if (qdev->vm_state < MITEE_VM_STATE_CONNECTED)
				pr_err("mitee msgq process data state error\n");
			pr_err("mitee msgq: unexpected data message\n");
			break;
		case MITEE_MSGQ_YIELD_REPLY:
			mitee_msgq_handle_yield_reply(qdev, msg);
			break;
		case MITEE_MSGQ_FASTCALL_REPLY:
			mitee_msgq_handle_fastcall_reply(qdev, msg);
			break;
		default:
			pr_err("unknown msg type from mitee %x\n", msg->msg_id);
			break;
		}
	}

	kfree(msg);
	return 0;
}

static bool mitee_reply_fifo_pop(struct mitee_reply_fifo *fifo,
				 struct mitee_regs *regs)
{
	bool popped = false;

	spin_lock(&fifo->lock);
	if (fifo->head != fifo->tail) {
		memcpy(regs, &fifo->slots[fifo->tail], sizeof(*regs));
		fifo->tail = (fifo->tail + 1) % MITEE_REPLY_SLOTS;
		popped = true;
	}
	spin_unlock(&fifo->lock);

	return popped;
}

static int mitee_msgq_yield_call(struct mitee_msgq_dev *qdev,
				 struct mitee_regs *regs)
{
	struct mitee_reply_fifo *fifo = &qdev->replies;
	struct mitee_msgq_msg msg = {
		.magic = cpu_to_le64(MITEE_MSGQ_MAGIC),
		.msg_id = MITEE_MSGQ_YIELD_CALL,
		.size = MITEE_MSGQ_PAYLOAD_SIZE,
		.client_id = MITEE_MSGQ_ID_HLOS,
	};

	memcpy(msg.regs, regs, sizeof(msg.regs));
	mitee_msgq_send(qdev, &msg, sizeof(msg));

	wait_event(fifo->wq, mitee_reply_fifo_pop(fifo, regs));

	return 0;
}

static int mitee_msgq_fast_call(struct mitee_msgq_dev *qdev,
				struct mitee_regs *regs)
{
	struct mitee_fastcall *fc = &qdev->fastcall;
	struct mitee_msgq_msg msg = {
		.magic = cpu_to_le64(MITEE_MSGQ_MAGIC),
		.msg_id = MITEE_MSGQ_FASTCALL,
		.size = MITEE_MSGQ_PAYLOAD_SIZE,
		.client_id = MITEE_MSGQ_ID_HLOS,
	};
	int rc = 0;

	memcpy(msg.regs, regs, sizeof(msg.regs));

	mutex_lock(&fc->mutex);
	mitee_msgq_send(qdev, &msg, sizeof(msg));
	wait_for_completion(&fc->done);

	spin_lock(&fc->lock);
	if (fc->valid) {
		memcpy(regs, &fc->regs, sizeof(*regs));
		memset(&fc->regs, 0, sizeof(fc->regs));
		fc->valid = false;
	} else {
		pr_err("mitee msgq: failed to get fastcall reply\n");
		rc = -EIO;
	}
	spin_unlock(&fc->lock);
	mutex_unlock(&fc->mutex);

	if (rc)
		pr_err("mitee msgq: read msg from mitee vm failed with 0x%x", rc);

	return rc;
}

/**
 * mitee_msgq_call() - call into the TEE VM
 * @regs:	arguments, replaced by the results
 *
 * Returns 0 if the VM answered the call.
 */
int mitee_msgq_call(struct mitee_regs *regs)
{
	struct mitee_msgq_dev *qdev = mitee_qdev;

	if (!qdev) {
		pr_err("mitee msgq: driver didn't initialized, try later\n");
		return -ENODEV;
	}

	switch (regs->a0) {
	case MITEE_CALL_GET_SHM_POOL:
		regs->a0 = qdev->res.start;
		regs->a1 = MITEE_SHM_POOL_SIZE;
		regs->a2 = MITEE_SHM_TYPE_CACHED;
		return 0;
	case MITEE_CALL_GET_LOG_BUF:
		regs->a0 = qdev->res.start + MITEE_SHM_POOL_SIZE;
		regs->a1 = resource_size(&qdev->res) - MITEE_SHM_POOL_SIZE;
		return 0;
	case MITEE_CALL_SET_QUEUE:
		qdev->queue_pa = regs->a1;
		qdev->queue_size = regs->a2;
		return 0;
	}

	if (qdev->vm_state < MITEE_VM_STATE_CONNECTED) {
		pr_err("mitee msgq: msgq didn't set up, try later\n");
		return -EAGAIN;
	}

	if (regs->a0 & BIT(31))
		return mitee_msgq_yield_call(qdev, regs);

	return mitee_msgq_fast_call(qdev, regs);
}

int mitee_msgq_vm_state(void)
{
	return mitee_qdev ? mitee_qdev->vm_state : MITEE_VM_STATE_DOWN;
}

static struct tee_shm *mitee_msgq_shm_from_handle(struct mitee_msgq_dev *qdev,
						  u64 handle,
						  struct sg_table **sgt)
{
	struct mitee_shm_entry *entry;
	struct tee_shm *shm = NULL;

	mutex_lock(&qdev->shm_mutex);
	entry = rhashtable_lookup_fast(&qdev->shm_table, &handle,
				       mitee_shm_rhash_params);
	if (entry) {
		shm = entry->shm;
		if (sgt)
			*sgt = entry->sgt;
	}
	mutex_unlock(&qdev->shm_mutex);

	return shm;
}

static int mitee_msgq_add_shm_handle(struct mitee_msgq_dev *qdev, u64 handle,
				     struct tee_shm *shm, struct sg_table *sgt)
{
	struct mitee_shm_entry *entry;
	int rc;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->handle = handle;
	entry->shm = shm;
	entry->sgt = sgt;

	mutex_lock(&qdev->shm_mutex);
	rc = rhashtable_lookup_insert_fast(&qdev->shm_table, &entry->linkage,
					   mitee_shm_rhash_params);
	mutex_unlock(&qdev->shm_mutex);
	if (rc)
		kfree(entry);

	return rc;
}

static void mitee_shm_rem_mem_handle(struct mitee_msgq_dev *qdev, u64 handle)
{
	struct mitee_shm_entry *entry;

	mutex_lock(&qdev->shm_mutex);
	entry = rhashtable_lookup_fast(&qdev->shm_table, &handle,
				       mitee_shm_rhash_params);
	if (entry)
		rhashtable_remove_fast(&qdev->shm_table, &entry->linkage,
				       mitee_shm_rhash_params);
	mutex_unlock(&qdev->shm_mutex);

	kfree(entry);
}

int mitee_msgq_to_msg_param(struct optee_msg_param *mp,
			    const struct tee_param *p)
{
	struct tee_shm *shm = p->u.memref.shm;

	mp->attr = OPTEE_MSG_ATTR_TYPE_FMEM_INPUT + p->attr -
		   TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;

	if (shm) {
		u64 shm_offs = p->u.memref.shm_offs;

		mp->u.fmem.internal_offs = shm->offset;

		mp->u.fmem.offs_low = shm_offs;
		mp->u.fmem.offs_high = shm_offs >> 32;
		/* Check that the entire offset could be stored. */
		if (mp->u.fmem.offs_high != shm_offs >> 32)
			return -EINVAL;

		mp->u.fmem.global_id = shm->sec_world_id;
	} else {
		memset(&mp->u, 0, sizeof(mp->u));
		mp->u.fmem.global_id = OPTEE_MSG_FMEM_INVALID_GLOBAL_ID;
	}
	mp->u.fmem.size = p->u.memref.size;

	return 0;
}

void mitee_msgq_from_msg_param(struct tee_param *p, u32 attr,
			       const struct optee_msg_param *mp)
{
	struct tee_shm *shm = NULL;
	u64 offs_high = 0;
	u64 offs_low = 0;

	p->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT +
		  attr - OPTEE_MSG_ATTR_TYPE_FMEM_INPUT;
	p->u.memref.size = mp->u.fmem.size;

	if (mp->u.fmem.global_id != OPTEE_MSG_FMEM_INVALID_GLOBAL_ID &&
	    mitee_qdev)
		shm = mitee_msgq_shm_from_handle(mitee_qdev,
						 mp->u.fmem.global_id, NULL);
	p->u.memref.shm = shm;

	if (shm) {
		offs_low = mp->u.fmem.offs_low;
		offs_high = mp->u.fmem.offs_high;
	}
	p->u.memref.shm_offs = offs_high << 32 | offs_low;
}

static void mitee_msgq_unassign_sgt(struct mitee_msgq_dev *qdev,
				    struct sg_table *sgt)
{
	const gh_vmid_t self[] = { qdev->self_vmid };
	const gh_vmid_t both[] = { qdev->self_vmid, qdev->peer_vmid };
	int rc;

	rc = mitee_assign_sgt(sgt, both, 2, self, PERM_READ | PERM_WRITE |
			      PERM_EXEC, 1);
	if (rc)
		pr_err("mitee msgq %s: hyp_assign_table mem failed with err=%d\n",
		       "assign_sgt_perm", rc);
}

int mitee_msgq_shm_register(struct tee_context *ctx, struct tee_shm *shm,
			    struct page **pages, size_t num_pages,
			    unsigned long start)
{
	struct mitee *mitee = tee_get_drvdata(ctx->teedev);
	struct mitee_msgq_dev *qdev = mitee_qdev;
	gh_vmid_t self[1], both[2];
	gh_memparcel_handle_t handle = 0;
	struct gh_acl_desc *acl;
	struct gh_sgl_desc *sgl;
	struct sg_table *sgt;
	int rc;

	if (ctx->teedev == mitee->supp_teedev) {
		pr_err("%s mitee msgq: do not support supp !\n", __func__);
		return -EACCES;
	}

	if (!qdev || qdev->vm_state < MITEE_VM_STATE_READY)
		return -EAGAIN;

	rc = optee_check_mem_type(start, num_pages);
	if (rc)
		return rc;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		pr_err("%s mitee msgq: without enough memory!\n", __func__);
		return -ENOMEM;
	}

	rc = sg_alloc_table_from_pages(sgt, pages, num_pages, 0,
				       num_pages * PAGE_SIZE, GFP_KERNEL);
	if (rc)
		goto err_free_sgt;

	self[0] = qdev->self_vmid;
	both[0] = qdev->self_vmid;
	both[1] = qdev->peer_vmid;

	rc = mitee_assign_sgt(sgt, self, 1, both, PERM_READ | PERM_WRITE, 2);
	if (rc) {
		pr_err("mitee msgq %s: hyp_assign_table mem failed with err=%d\n",
		       "assign_sgt_perm", rc);
		goto err_free_table;
	}

	acl = mitee_acl_desc(both, 2, GH_RM_ACL_R | GH_RM_ACL_W);
	sgl = mitee_sgt_to_sgl_desc(sgt);
	if (!acl || !sgl) {
		kfree(acl);
		kfree(sgl);
		rc = -ENOMEM;
		goto err_unassign;
	}

	rc = ghd_rm_mem_share(GH_RM_MEM_TYPE_NORMAL, 0, 0, acl, sgl, NULL,
			      &handle);
	kfree(acl);
	kfree(sgl);
	if (rc) {
		pr_err("mitee msgq shared memory failed with rc (%d)\n", rc);
		goto err_unassign;
	}

	rc = mitee_msgq_add_shm_handle(qdev, handle, shm, sgt);
	if (rc) {
		pr_err("%s mitee msgq: add handle failed: %d!\n", __func__, rc);
		goto err_reclaim;
	}

	shm->sec_world_id = handle;
	return 0;

err_reclaim:
	ghd_rm_mem_reclaim(handle, 0);
err_unassign:
	mitee_msgq_unassign_sgt(qdev, sgt);
err_free_table:
	sg_free_table(sgt);
err_free_sgt:
	kfree(sgt);
	return rc;
}

static int mitee_msgq_release_shm(struct mitee_msgq_dev *qdev, u64 handle,
				  struct sg_table *sgt)
{
	int rc;

	rc = ghd_rm_mem_reclaim(handle, 0);
	if (rc) {
		pr_err("mitee msgq mem_reclaim: failed with 0x%llx %d",
		       handle, rc);
		/* The memory may still be mapped by the VM, leak it */
		return rc;
	}

	mitee_msgq_unassign_sgt(qdev, sgt);
	sg_free_table(sgt);
	kfree(sgt);
	return 0;
}

int mitee_msgq_shm_unregister(struct tee_context *ctx, struct tee_shm *shm)
{
	struct mitee_msgq_dev *qdev = mitee_qdev;
	u64 handle = shm->sec_world_id;
	struct optee_msg_arg *msg_arg;
	struct sg_table *sgt = NULL;
	int rc;

	if (!mitee_msgq_shm_from_handle(qdev, handle, &sgt))
		return -EINVAL;

	mitee_shm_rem_mem_handle(qdev, handle);
	shm->sec_world_id = 0;

	msg_arg = kzalloc(OPTEE_MSG_GET_ARG_SIZE(1), GFP_KERNEL);
	if (!msg_arg)
		return -ENOMEM;

	msg_arg->cmd = OPTEE_MSG_CMD_UNREGISTER_SHM;
	msg_arg->num_params = 1;
	msg_arg->params[0].attr = OPTEE_MSG_ATTR_TYPE_FMEM_INPUT;
	msg_arg->params[0].u.fmem.global_id = handle;

	rc = optee_do_call_with_arg(ctx, msg_arg);
	if (rc)
		pr_err("mitee msgq: Unregister SHM id 0x%llx rc %d\n", handle,
		       rc);
	kfree(msg_arg);

	return mitee_msgq_release_shm(qdev, handle, sgt);
}

int mitee_msgq_shm_unregister_supp(struct tee_context *ctx,
				   struct tee_shm *shm)
{
	struct mitee_msgq_dev *qdev = mitee_qdev;
	u64 handle = shm->sec_world_id;
	struct sg_table *sgt = NULL;

	if (!mitee_msgq_shm_from_handle(qdev, handle, &sgt))
		return -EINVAL;

	mitee_shm_rem_mem_handle(qdev, handle);
	return mitee_msgq_release_shm(qdev, handle, sgt);
}

static void mitee_free_memory_sgt(struct sg_table *sgt)
{
	struct scatterlist *sg;
	int i;

	for_each_sgtable_sg(sgt, sg, i)
		__free_pages(sg_page(sg), get_order(sg->length));

	sg_free_table(sgt);
	kfree(sgt);
}

/* Allocate @size bytes as few, large, physically contiguous chunks */
static struct sg_table *mitee_alloc_memory_sgt(size_t size)
{
	struct sg_table *sgt;
	struct scatterlist *sg;
	struct page **chunks;
	unsigned int *orders;
	unsigned int nr = 0;
	int order, rc;
	size_t left = size;

	chunks = kcalloc(size >> PAGE_SHIFT, sizeof(*chunks), GFP_KERNEL);
	orders = kcalloc(size >> PAGE_SHIFT, sizeof(*orders), GFP_KERNEL);
	if (!chunks || !orders)
		goto err_free;

	for (order = MITEE_MAX_ALLOC_ORDER; order >= 0 && left; order--) {
		while (left >= (PAGE_SIZE << order)) {
			struct page *page = alloc_pages(GFP_KERNEL | __GFP_NOWARN |
							(order ? __GFP_NORETRY : 0),
							order);

			if (!page)
				break;
			chunks[nr] = page;
			orders[nr] = order;
			nr++;
			left -= PAGE_SIZE << order;
		}
	}

	if (left) {
		pr_err("mitee dynamic mem: system without enough memory, still need 0x%zx\n",
		       left);
		goto err_free;
	}

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		goto err_free;

	rc = sg_alloc_table(sgt, nr, GFP_KERNEL);
	if (rc) {
		pr_err("mitee dynamic mem: failed to alloc sg_table with %d\n",
		       rc);
		kfree(sgt);
		goto err_free;
	}

	for_each_sgtable_sg(sgt, sg, order)
		sg_set_page(sg, chunks[order], PAGE_SIZE << orders[order], 0);

	kfree(chunks);
	kfree(orders);
	return sgt;

err_free:
	while (nr--)
		__free_pages(chunks[nr], orders[nr]);
	kfree(chunks);
	kfree(orders);
	return NULL;
}

/* Lend memory to the VM, which then has exclusive access to it */
static int mitee_msgq_secmem_lend(struct mitee_msgq_dev *qdev, size_t size,
				  gh_memparcel_handle_t *handle)
{
	const gh_vmid_t self[] = { qdev->self_vmid };
	const gh_vmid_t peer[] = { qdev->peer_vmid };
	struct mitee_secmem *secmem;
	struct gh_acl_desc *acl;
	struct gh_sgl_desc *sgl;
	struct sg_table *sgt;
	int rc;

	secmem = kzalloc(sizeof(*secmem), GFP_KERNEL);
	if (!secmem)
		return -ENOMEM;

	sgt = mitee_alloc_memory_sgt(size);
	if (!sgt) {
		rc = -ENOMEM;
		goto err_free_secmem;
	}

	rc = mitee_assign_sgt(sgt, self, 1, peer, PERM_READ | PERM_WRITE |
			      PERM_EXEC, 1);
	if (rc) {
		pr_err("mitee msgq %s: hyp_assign_table mem failed with err=%d\n",
		       "assign_sgt_perm_lend", rc);
		goto err_free_sgt;
	}

	acl = mitee_acl_desc(peer, 1, GH_RM_ACL_R | GH_RM_ACL_W | GH_RM_ACL_X);
	sgl = mitee_sgt_to_sgl_desc(sgt);
	if (!acl || !sgl) {
		kfree(acl);
		kfree(sgl);
		rc = -ENOMEM;
		goto err_unassign;
	}

	rc = ghd_rm_mem_lend(GH_RM_MEM_TYPE_NORMAL, 0, 0, acl, sgl, NULL,
			     handle);
	kfree(acl);
	kfree(sgl);
	if (rc) {
		pr_err("mitee msgq lend memory failed with rc (%d)\n", rc);
		goto err_unassign;
	}

	secmem->sgt = sgt;
	secmem->handle = *handle;
	secmem->size = size;

	mutex_lock(&qdev->secmem_mutex);
	list_add_tail(&secmem->link, &qdev->secmem_list);
	mutex_unlock(&qdev->secmem_mutex);

	return 0;

err_unassign:
	rc = mitee_assign_sgt(sgt, peer, 1, self, PERM_READ | PERM_WRITE |
			      PERM_EXEC, 1);
	if (rc) {
		pr_err("mitee msgq %s: hyp_assign_table mem failed with err=%d\n",
		       "assign_sgt_perm_lend", rc);
		/* Not ours anymore, leak it */
		kfree(secmem);
		return -EIO;
	}
err_free_sgt:
	mitee_free_memory_sgt(sgt);
err_free_secmem:
	kfree(secmem);
	return rc ? rc : -EIO;
}

static int mitee_msgq_secmem_reclaim(struct mitee_msgq_dev *qdev,
				     gh_memparcel_handle_t handle)
{
	const gh_vmid_t self[] = { qdev->self_vmid };
	const gh_vmid_t peer[] = { qdev->peer_vmid };
	struct mitee_secmem *secmem, *tmp;
	bool found = false;
	int rc;

	mutex_lock(&qdev->secmem_mutex);
	list_for_each_entry_safe(secmem, tmp, &qdev->secmem_list, link) {
		if (secmem->handle == handle) {
			list_del(&secmem->link);
			found = true;
			break;
		}
	}
	mutex_unlock(&qdev->secmem_mutex);

	if (!found)
		return -EINVAL;

	rc = ghd_rm_mem_reclaim(handle, 0);
	if (rc) {
		pr_err("mitee msgq mem_reclaim: failed with 0x%x %d", handle,
		       rc);
		goto leak;
	}

	rc = mitee_assign_sgt(secmem->sgt, peer, 1, self, PERM_READ |
			      PERM_WRITE | PERM_EXEC, 1);
	if (rc) {
		pr_err("mitee msgq %s: hyp_assign_table mem failed with err=%d\n",
		       "assign_sgt_perm_lend", rc);
		goto leak;
	}

	mitee_free_memory_sgt(secmem->sgt);
	kfree(secmem);
	return 0;

leak:
	/* The pages may still belong to the VM, never hand them out again */
	kfree(secmem);
	return rc;
}

/*
 * OPTEE_RPC_CMD_MITEE_CALLBACK, client MITEE_RPC_CLIENT_SECMEM:
 * value[0].a = command, value[0].c = size (lend) or handle (reclaim).
 * Lending returns the handle in value[0].a and the size in value[0].b.
 */
static int mitee_msgq_rpc_callback(u64 *value, void *buf, size_t size,
				   u32 *out_size)
{
	struct mitee_msgq_dev *qdev = mitee_qdev;
	gh_memparcel_handle_t handle;
	u32 len;

	switch (value[0]) {
	case MITEE_RPC_SECMEM_LEND:
		len = value[2];
		if (!len)
			break;
		if (mitee_msgq_secmem_lend(qdev, PAGE_ALIGN(len), &handle))
			break;
		value[0] = handle;
		value[1] = len;
		return TEEC_SUCCESS;
	case MITEE_RPC_SECMEM_RECLAIM:
		if (mitee_msgq_secmem_reclaim(qdev, value[2]))
			break;
		return TEEC_SUCCESS;
	default:
		pr_err("mitee msgq: unknown cmd\n");
		break;
	}

	return TEEC_ERROR_BAD_PARAMETERS;
}

static int mitee_msgq_share_static_mem(struct mitee_msgq_dev *qdev)
{
	return mitee_msgq_share_mem_contiguous(qdev, qdev->res.start,
					       resource_size(&qdev->res),
					       qdev->label, &qdev->shm_handle);
}

static void mitee_msgq_unshare_static_mem(struct mitee_msgq_dev *qdev)
{
	mitee_msgq_unshare_mem_contiguous(qdev, qdev->res.start,
					  resource_size(&qdev->res),
					  qdev->shm_handle);

	if (qdev->queue_shared) {
		mitee_msgq_unshare_mem_contiguous(qdev, qdev->queue_pa,
						  qdev->queue_size,
						  qdev->queue_handle);
		qdev->queue_shared = false;
	}
}

static int mitee_msgq_rm_cb(struct notifier_block *nb, unsigned long cmd,
			    void *data)
{
	struct mitee_msgq_dev *qdev = container_of(nb, struct mitee_msgq_dev,
						   rm_nb);
	struct gh_rm_notif_vm_status_payload *payload = data;
	gh_vmid_t peer_vmid, self_vmid;

	if (cmd != GH_RM_NOTIF_VM_STATUS)
		return NOTIFY_DONE;

	if (payload->vm_status != GH_RM_VM_STATUS_READY &&
	    payload->vm_status != GH_RM_VM_STATUS_RESET)
		return NOTIFY_DONE;

	if (ghd_rm_get_vmid(qdev->peer_name, &peer_vmid))
		return NOTIFY_DONE;
	if (ghd_rm_get_vmid(GH_PRIMARY_VM, &self_vmid))
		return NOTIFY_DONE;
	if (peer_vmid != payload->vmid)
		return NOTIFY_DONE;

	if (payload->vm_status == GH_RM_VM_STATUS_READY) {
		qdev->vm_state = MITEE_VM_STATE_READY;
		qdev->self_vmid = self_vmid;
		qdev->peer_vmid = peer_vmid;
		if (mitee_msgq_share_static_mem(qdev)) {
			pr_err("%s: mitee msgq failed to share memory\n",
			       __func__);
			return NOTIFY_DONE;
		}

		mitee_rpc_register_callback(MITEE_RPC_CLIENT_SECMEM,
					    MITEE_RPC_SECMEM_LEND,
					    mitee_msgq_rpc_callback);
		mitee_rpc_register_callback(MITEE_RPC_CLIENT_SECMEM,
					    MITEE_RPC_SECMEM_RECLAIM,
					    mitee_msgq_rpc_callback);
	} else {
		qdev->vm_state = MITEE_VM_STATE_DOWN;
		mitee_msgq_unshare_static_mem(qdev);
	}

	return NOTIFY_DONE;
}

static void mitee_msgq_stop_recv_task(struct task_struct *task)
{
	/*
	 * The thread returns as soon as the signal interrupts its receive,
	 * and kthreads are reaped on exit: keep the task around until
	 * kthread_stop() is done with it.
	 */
	get_task_struct(task);
	send_sig(SIGKILL, task, 1);
	kthread_stop(task);
	put_task_struct(task);
}

static int mitee_msgq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct mitee_msgq_dev *qdev;
	struct device_node *np;
	int rc;

	qdev = devm_kzalloc(dev, sizeof(*qdev), GFP_KERNEL);
	if (!qdev)
		return -ENOMEM;

	qdev->dev = dev;
	platform_set_drvdata(pdev, qdev);

	spin_lock_init(&qdev->replies.lock);
	init_waitqueue_head(&qdev->replies.wq);
	mutex_init(&qdev->fastcall.mutex);
	spin_lock_init(&qdev->fastcall.lock);
	init_completion(&qdev->fastcall.done);
	mutex_init(&qdev->shm_mutex);
	mutex_init(&qdev->secmem_mutex);
	INIT_LIST_HEAD(&qdev->secmem_list);

	rc = of_property_read_u32(node, "gunyah-shm-label", &qdev->label);
	if (rc) {
		dev_err(dev, "failed to read label info %d\n", rc);
		return rc;
	}

	/* Only the primary VM side of the link is implemented */
	if (!of_property_read_bool(node, "xiaomi,master"))
		return -EINVAL;

	np = of_parse_phandle(node, "shared-buffer", 0);
	if (np) {
		rc = of_address_to_resource(np, 0, &qdev->res);
		of_node_put(np);
		if (rc) {
			dev_err(dev, "of_address_to_resource failed!\n");
			return -EINVAL;
		}
	} else {
		qdev->dma_va = dma_alloc_attrs(dev, MITEE_DEFAULT_SHM_SIZE,
					       &qdev->res.start, GFP_KERNEL,
					       DMA_ATTR_FORCE_CONTIGUOUS);
		if (!qdev->dma_va)
			return -ENOMEM;
		qdev->res.end = qdev->res.start + MITEE_DEFAULT_SHM_SIZE - 1;
	}

	if (resource_size(&qdev->res) <= MITEE_SHM_POOL_SIZE) {
		dev_err(dev, "shared buffer too small\n");
		rc = -EINVAL;
		goto err_free_dma;
	}

	if (of_property_read_u32(node, "peer-name", &qdev->peer_name))
		qdev->peer_name = GH_SELF_VM;

	rc = rhashtable_init(&qdev->shm_table, &mitee_shm_rhash_params);
	if (rc) {
		pr_err("mitee msgq: rhashtable_init error %d\n", rc);
		goto err_free_dma;
	}

	qdev->msgq = gh_msgq_register(GH_MSGQ_LABEL_TEST_OEMVM);
	if (IS_ERR(qdev->msgq)) {
		rc = PTR_ERR(qdev->msgq);
		goto err_destroy_table;
	}

	qdev->recv_task = kthread_run(mitee_msgq_recv_task, qdev,
				      "mitee_msgq_recv_task");
	if (IS_ERR(qdev->recv_task)) {
		rc = PTR_ERR(qdev->recv_task);
		goto err_unregister_msgq;
	}

	qdev->rm_nb.notifier_call = mitee_msgq_rm_cb;
	qdev->rm_nb.priority = INT_MAX;
	rc = gh_rm_register_notifier(&qdev->rm_nb);
	if (rc)
		goto err_stop_task;

	mitee_qdev = qdev;
	return 0;

err_stop_task:
	mitee_msgq_stop_recv_task(qdev->recv_task);
err_unregister_msgq:
	gh_msgq_unregister(qdev->msgq);
err_destroy_table:
	rhashtable_destroy(&qdev->shm_table);
err_free_dma:
	if (qdev->dma_va)
		dma_free_attrs(dev, MITEE_DEFAULT_SHM_SIZE, qdev->dma_va,
			       qdev->res.start, DMA_ATTR_FORCE_CONTIGUOUS);
	return rc;
}

static void mitee_shm_entry_free(void *ptr, void *arg)
{
	kfree(ptr);
}

static int mitee_msgq_remove(struct platform_device *pdev)
{
	struct mitee_msgq_dev *qdev = platform_get_drvdata(pdev);

	gh_rm_unregister_notifier(&qdev->rm_nb);
	mitee_qdev = NULL;

	mitee_msgq_stop_recv_task(qdev->recv_task);
	gh_msgq_unregister(qdev->msgq);

	if (qdev->vm_state != MITEE_VM_STATE_DOWN)
		mitee_msgq_unshare_static_mem(qdev);

	rhashtable_free_and_destroy(&qdev->shm_table, mitee_shm_entry_free,
				    NULL);

	if (qdev->dma_va)
		dma_free_attrs(qdev->dev, MITEE_DEFAULT_SHM_SIZE, qdev->dma_va,
			       qdev->res.start, DMA_ATTR_FORCE_CONTIGUOUS);

	return 0;
}

static const struct of_device_id mitee_msgq_match[] = {
	{ .compatible = "xiaomi,mitee-hlos-ipc" },
	{}
};
MODULE_DEVICE_TABLE(of, mitee_msgq_match);

static struct platform_driver mitee_msgq_driver = {
	.probe = mitee_msgq_probe,
	.remove = mitee_msgq_remove,
	.driver = {
		.name = "mitee_ipc",
		.of_match_table = mitee_msgq_match,
		.suppress_bind_attrs = true,
	},
};

int mitee_msgq_driver_init(void)
{
	return platform_driver_register(&mitee_msgq_driver);
}

void mitee_msgq_driver_exit(void)
{
	platform_driver_unregister(&mitee_msgq_driver);
}
