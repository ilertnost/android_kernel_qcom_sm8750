// SPDX-License-Identifier: GPL-2.0-only
/*
 * MiTEE trusted UI.
 *
 * The trusted UI application in the TEE asks the normal world for display
 * information, to hand over the display and for its font. Those requests
 * arrive as OPTEE_RPC_CMD_MITEE_CALLBACK RPCs and are forwarded to the
 * vendor.xiaomi.hardware.mitrustedui service, which waits for them on
 * /dev/mitee-tui and answers through ioctls. The service passes the font in
 * a buffer allocated here and mapped into its address space.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": tui: " fmt

#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#include "mitee_notify.h"
#include "mitee_private.h"
#include "mitee_tui.h"
#include "optee_rpc_cmd.h"

/* ioctls of /dev/mitee-tui and /dev/mitee-tui-supp, no size encoded */
#define TUI_IO_NOTIFY		0x7401
#define TUI_IO_WAITCMD		0x7402	/* u32 command */
#define TUI_IO_ACK		0x7403	/* struct tui_ack_arg */
#define TUI_IO_FREE_BUF		0x7404
#define TUI_IO_SIGNAL		0x7405
#define TUI_IO_REGISTER		0x7406
#define TUI_IO_SHM_ALLOC	0x7407	/* struct tui_shm_arg */
#define TUI_IO_SHM_FREE		0x7408	/* struct tui_shm_arg */
#define TUI_IO_TYPE		0x74

/* Commands from the trusted UI application */
#define TUI_CMD_NONE		0
#define TUI_CMD_GET_RESOLUTION	1
#define TUI_CMD_START		2
#define TUI_CMD_STOP		3
#define TUI_CMD_GET_FONT	4

/* Results returned to the trusted UI application */
#define TUI_RET_OK		0
#define TUI_RET_DENIED		1
#define TUI_RET_ERROR		3

#define TUI_RESPONSE_TIMEOUT_MS	5000

/* Notification channel of the trusted UI */
#define TUI_NOTIFY_ID		0x0a0b0c0d
#define TUI_NOTIFY_SIGNAL	0xc

/* Callback id of the trusted UI */
#define TUI_RPC_CLIENT		1

#define TUI_SHM_NAME_LEN	16
#define TUI_SHM_FLAG_MAPPED	BIT(0)
#define TUI_SHM_FLAG_ALLOCATED	BIT(1)

struct tui_ack_arg {
	u64 response;	/* user pointer to struct tui_response */
	u64 size;
};

struct tui_response {
	u32 id;		/* command answered */
	u32 code;	/* 0: ok, 1: error, 2: denied */
	u32 width;
	u32 height;
	u32 reserved[3];
	u32 font_shm_id;
	u64 font_size;
};

struct tui_shm_arg {
	char name[TUI_SHM_NAME_LEN];
	u64 size;
	u32 flags;
	u32 id;
	s32 fd;
	u32 reserved;
};

struct tui_shm {
	struct kref ref;
	char name[TUI_SHM_NAME_LEN];
	int id;
	void *vaddr;
	size_t size;
	u32 flags;
};

struct mitee_tui {
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct device *dev;

	dev_t supp_devt;
	struct cdev supp_cdev;
	struct class *supp_class;
	struct device *supp_dev;

	atomic_t opened;
	int token;

	/* request to the service and its answer */
	u32 user_cmd;
	struct tui_response user_rsp;
	struct completion user_comp;
	bool event_active;
	wait_queue_head_t event_queue;

	/* font being sent to the TEE, holds a reference */
	struct tui_shm *font;
	size_t font_size;

	struct mutex shm_mutex;		/* protects @shm_idr and @font */
	struct idr shm_idr;
};

static struct mitee_tui *mitee_tui;

static const u32 tui_rsp_codes[] = { TUI_RET_OK, TUI_RET_ERROR,
				     TUI_RET_DENIED };

static void tui_shm_release(struct kref *ref)
{
	struct tui_shm *shm = container_of(ref, struct tui_shm, ref);

	vfree(shm->vaddr);
	kfree(shm);
}

static void tui_shm_put(struct tui_shm *shm)
{
	kref_put(&shm->ref, tui_shm_release);
}

/*
 * Returns a new buffer holding two references: one owned by the id table
 * and one for the caller, to be dropped with tui_shm_put().
 */
static struct tui_shm *tui_shm_alloc(struct mitee_tui *tui, size_t size)
{
	struct tui_shm *shm;

	shm = kzalloc(sizeof(*shm), GFP_KERNEL);
	if (!shm)
		return ERR_PTR(-ENOMEM);

	kref_init(&shm->ref);
	shm->flags = TUI_SHM_FLAG_MAPPED | TUI_SHM_FLAG_ALLOCATED;
	shm->size = PAGE_ALIGN(size);
	shm->vaddr = vmalloc_user(shm->size);
	if (!shm->vaddr) {
		pr_err("fail to vmalloc_user page, size:%zu\n", shm->size);
		kfree(shm);
		return ERR_PTR(-ENOMEM);
	}

	kref_get(&shm->ref);
	mutex_lock(&tui->shm_mutex);
	shm->id = idr_alloc(&tui->shm_idr, shm, 1, 0, GFP_KERNEL);
	mutex_unlock(&tui->shm_mutex);
	if (shm->id < 0) {
		pr_err("fail to idr_alloc shm id\n");
		vfree(shm->vaddr);
		kfree(shm);
		return ERR_PTR(-EINVAL);
	}

	return shm;
}

/*
 * Drop the buffer from the id table along with the table's reference;
 * open file descriptors and a font transfer keep it alive. Only the
 * caller that actually removed the id drops that reference.
 */
static int tui_shm_free_id(struct mitee_tui *tui, int id)
{
	struct tui_shm *shm;

	mutex_lock(&tui->shm_mutex);
	shm = idr_remove(&tui->shm_idr, id);
	mutex_unlock(&tui->shm_mutex);
	if (!shm)
		return -EINVAL;

	pr_info("free shm, name=%s\n", shm->name);
	tui_shm_put(shm);
	return 0;
}

/* Drop the font of a finished or aborted transfer, @shm_mutex held */
static void tui_font_drop_locked(struct mitee_tui *tui)
{
	if (!tui->font)
		return;

	tui_shm_put(tui->font);
	tui->font = NULL;
	tui->font_size = 0;
}

static int tui_shm_fop_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct tui_shm *shm = filp->private_data;

	if (vma->vm_pgoff + vma_pages(vma) > shm->size >> PAGE_SHIFT) {
		pr_err("vma size is over shm_size, error\n");
		return -EINVAL;
	}

	if (remap_vmalloc_range(vma, shm->vaddr, vma->vm_pgoff)) {
		pr_err("mmap: remap failed with error\n");
		return -ENOMEM;
	}

	return 0;
}

static int tui_shm_fop_release(struct inode *inode, struct file *filp)
{
	struct tui_shm *shm = filp->private_data;

	tui_shm_put(shm);
	return 0;
}

static const struct file_operations tui_shm_fops = {
	.owner = THIS_MODULE,
	.mmap = tui_shm_fop_mmap,
	.release = tui_shm_fop_release,
};

static int tui_shm_create_fd(struct tui_shm *shm, const char *name)
{
	int fd;

	if (!name[0])
		return -EINVAL;

	strscpy(shm->name, name, sizeof(shm->name));
	kref_get(&shm->ref);
	fd = anon_inode_getfd(shm->name, &tui_shm_fops, shm, O_RDWR);
	if (fd < 0) {
		pr_err("failed to create anon fd, err=%d\n", fd);
		tui_shm_put(shm);
		return -ENOMEM;
	}

	return fd;
}

static int tui_ioctl_shm_alloc(struct mitee_tui *tui, void __user *uarg)
{
	struct tui_shm_arg arg;
	struct tui_shm *shm;

	if (copy_from_user(&arg, uarg, sizeof(arg))) {
		pr_err("failed to copy_from_user\n");
		return -EFAULT;
	}

	if (arg.flags) {
		pr_err("shm flags is invalid\n");
		return -EINVAL;
	}
	arg.name[TUI_SHM_NAME_LEN - 1] = '\0';

	shm = tui_shm_alloc(tui, arg.size);
	if (IS_ERR(shm)) {
		pr_err("an error occured in tui_shm_alloc\n");
		return -ENOMEM;
	}

	arg.flags = TUI_SHM_FLAG_MAPPED;
	arg.id = shm->id;
	arg.fd = tui_shm_create_fd(shm, arg.name);
	tui_shm_put(shm);

	if (copy_to_user(uarg, &arg, sizeof(arg))) {
		pr_err("failed to copy_to_user\n");
		return -EFAULT;
	}

	return 0;
}

static int tui_ioctl_shm_free(struct mitee_tui *tui, void __user *uarg)
{
	struct tui_shm_arg arg;

	if (copy_from_user(&arg, uarg, sizeof(arg))) {
		pr_err("failed to copy_from_user\n");
		return -EINVAL;
	}

	if (arg.flags) {
		pr_err("shm flags is invalid\n");
		return -EINVAL;
	}

	/* Freeing the font buffer aborts a transfer in progress */
	mutex_lock(&tui->shm_mutex);
	if (tui->font && tui->font->id == arg.id)
		tui_font_drop_locked(tui);
	mutex_unlock(&tui->shm_mutex);

	if (tui_shm_free_id(tui, arg.id)) {
		pr_err("failed to get data by anonid, err=%u\n", arg.id);
		return -EINVAL;
	}

	return 0;
}

/* Pass a command to the service and wait for its answer */
static u32 send_cmd_to_user(struct mitee_tui *tui, u32 cmd)
{
	u32 ret = TUI_RET_ERROR;

	tui->user_cmd = cmd;

	if (!atomic_read(&tui->opened)) {
		pr_err("waiting for userland answer for err!!\n");
		goto out;
	}

	/* Never take a stale answer to an earlier request for this one */
	memset(&tui->user_rsp, 0, sizeof(tui->user_rsp));
	reinit_completion(&tui->user_comp);
	tui->event_active = true;
	wake_up_interruptible(&tui->event_queue);

	if (!wait_for_completion_timeout(&tui->user_comp,
			msecs_to_jiffies(TUI_RESPONSE_TIMEOUT_MS))) {
		/* Stock reports this as the missing answer: TUI_RET_ERROR */
		pr_info("No acknowledge from client, timeout!\n");
		goto out;
	}

	if (tui->user_rsp.id != cmd) {
		pr_err("Wrong response id(%u,%u)\n", tui->user_rsp.id, cmd);
		goto out;
	}

	ret = TUI_RET_OK;
	if (tui->user_rsp.code < ARRAY_SIZE(tui_rsp_codes))
		ret = tui_rsp_codes[tui->user_rsp.code];
out:
	tui->user_cmd = TUI_CMD_NONE;
	return ret;
}

static int tui_send_font(struct mitee_tui *tui, u64 *value, void *buf,
			 size_t size, u32 *out_size)
{
	size_t len = value[1];
	size_t offs = value[2];
	struct tui_shm *shm;
	u32 ret;

	if (!offs) {
		ret = send_cmd_to_user(tui, TUI_CMD_GET_FONT);
		if (ret) {
			pr_err("send_cmd_to_user() failed: %u\n", ret);
			return ret;
		}

		mutex_lock(&tui->shm_mutex);
		tui_font_drop_locked(tui);
		shm = idr_find(&tui->shm_idr, tui->user_rsp.font_shm_id);
		if (shm) {
			kref_get(&shm->ref);
			tui->font = shm;
			tui->font_size = min_t(size_t, tui->user_rsp.font_size,
					       shm->size);
		}
		mutex_unlock(&tui->shm_mutex);
		if (!shm) {
			pr_err("failed to get data by anonid, err=%u\n",
			       tui->user_rsp.font_shm_id);
			return TEEC_ERROR_BAD_PARAMETERS;
		}
	}

	mutex_lock(&tui->shm_mutex);
	if (!tui->font || offs > tui->font_size || !buf) {
		mutex_unlock(&tui->shm_mutex);
		return TEEC_ERROR_BAD_PARAMETERS;
	}

	len = min3(len, size, tui->font_size - offs);
	memcpy(buf, tui->font->vaddr + offs, len);

	value[1] = offs ? len : tui->font_size;
	*out_size = len;

	/* The whole font was sent: free its buffer, as stock does */
	if (offs + len >= tui->font_size) {
		shm = tui->font;
		if (idr_find(&tui->shm_idr, shm->id) == shm) {
			idr_remove(&tui->shm_idr, shm->id);
			tui_shm_put(shm);
		}
		tui_font_drop_locked(tui);
	}
	mutex_unlock(&tui->shm_mutex);

	return TUI_RET_OK;
}

/* OPTEE_RPC_CMD_MITEE_CALLBACK handler of the trusted UI application */
static int tui_process_tuisvc_callback(u64 *value, void *buf, size_t size,
				       u32 *out_size)
{
	struct mitee_tui *tui = mitee_tui;
	u64 *res = buf;
	u32 cmd;
	int ret;

	if (!value)
		return TEEC_ERROR_BAD_PARAMETERS;

	cmd = value[0];
	switch (cmd) {
	case TUI_CMD_NONE:
		pr_err("Notified without command\n");
		return -1;
	case TUI_CMD_GET_RESOLUTION:
		if (!res || size < 2 * sizeof(*res))
			return TEEC_ERROR_BAD_PARAMETERS;
		ret = send_cmd_to_user(tui, cmd);
		if (ret)
			break;
		res[0] = tui->user_rsp.width;
		res[1] = tui->user_rsp.height;
		*out_size = 0x20;
		break;
	case TUI_CMD_START:
		ret = send_cmd_to_user(tui, cmd);
		break;
	case TUI_CMD_STOP:
		ret = send_cmd_to_user(tui, cmd);
		if (!ret)
			value[0] = 0;
		break;
	case TUI_CMD_GET_FONT:
		ret = tui_send_font(tui, value, buf, size, out_size);
		break;
	default:
		pr_err("Unknown command\n");
		ret = 1;
		break;
	}

	pr_info("return 0x%08x to cmd 0x%08x\n", ret, cmd);
	return ret;
}

static int tui_send_response(struct mitee_tui *tui, void __user *uarg)
{
	struct tui_ack_arg arg;

	if (copy_from_user(&arg, uarg, sizeof(arg)) ||
	    arg.size > sizeof(tui->user_rsp) ||
	    copy_from_user(&tui->user_rsp, u64_to_user_ptr(arg.response),
			   arg.size)) {
		pr_err("tui_send_response copy_from_user fail\n");
		return -EFAULT;
	}

	complete(&tui->user_comp);
	return 0;
}

static long tui_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mitee_tui *tui = mitee_tui;
	void __user *uarg = (void __user *)arg;
	int ret = 0;

	if (_IOC_TYPE(cmd) != TUI_IO_TYPE)
		return -EINVAL;

	switch (cmd) {
	case TUI_IO_NOTIFY:
		break;
	case TUI_IO_WAITCMD:
		if (copy_to_user(uarg, &tui->user_cmd, sizeof(tui->user_cmd))) {
			pr_err("tui copy to user failed!\n");
			ret = -EFAULT;
		}
		break;
	case TUI_IO_ACK:
		ret = tui_send_response(tui, uarg);
		break;
	case TUI_IO_FREE_BUF:
		break;
	case TUI_IO_SIGNAL:
		if (tui->token <= 0)
			tui->token = mitee_smc_notify_connect(0, TUI_NOTIFY_ID);
		pr_info("mitee_tui check connect token: %x\n", tui->token);
		if (mitee_smc_notify_signal(0, tui->token, TUI_NOTIFY_SIGNAL))
			pr_err("notify signal failed!\n");
		break;
	case TUI_IO_REGISTER:
		ret = mitee_rpc_register_callback(TUI_RPC_CLIENT,
						  MITEE_RPC_CALLBACK_CMD_BUFFER,
						  tui_process_tuisvc_callback);
		break;
	case TUI_IO_SHM_ALLOC:
		ret = tui_ioctl_shm_alloc(tui, uarg);
		break;
	case TUI_IO_SHM_FREE:
		ret = tui_ioctl_shm_free(tui, uarg);
		break;
	default:
		pr_err("Unknown ioctl (%u)!\n", cmd);
		return -ENOTTY;
	}

	return ret;
}

static __poll_t tui_poll(struct file *file, poll_table *wait)
{
	struct mitee_tui *tui = mitee_tui;

	poll_wait(file, &tui->event_queue, wait);

	if (tui->event_active) {
		tui->event_active = false;
		return EPOLLIN | EPOLLRDNORM;
	}

	return 0;
}

static int tui_open(struct inode *inode, struct file *file)
{
	atomic_inc(&mitee_tui->opened);
	return 0;
}

static int tui_release(struct inode *inode, struct file *file)
{
	atomic_dec(&mitee_tui->opened);
	return 0;
}

static const struct file_operations tui_fops = {
	.owner = THIS_MODULE,
	.open = tui_open,
	.release = tui_release,
	.poll = tui_poll,
	.unlocked_ioctl = tui_ioctl,
	.compat_ioctl = tui_ioctl,
};

static int tui_add_device(const char *name, const char *class_name,
			  dev_t *devt, struct cdev *cdev, struct class **class,
			  struct device **dev)
{
	int rc;

	rc = alloc_chrdev_region(devt, 0, 1, name);
	if (rc)
		return rc;

	cdev_init(cdev, &tui_fops);
	cdev->owner = THIS_MODULE;
	rc = cdev_add(cdev, *devt, 1);
	if (rc)
		goto err_unregister;

	*class = class_create(class_name);
	if (IS_ERR(*class)) {
		rc = PTR_ERR(*class);
		goto err_cdev_del;
	}

	*dev = device_create(*class, NULL, *devt, NULL, name);
	if (IS_ERR(*dev)) {
		rc = PTR_ERR(*dev);
		goto err_class_destroy;
	}

	return 0;

err_class_destroy:
	class_destroy(*class);
err_cdev_del:
	cdev_del(cdev);
err_unregister:
	unregister_chrdev_region(*devt, 1);
	return rc;
}

static void tui_remove_device(dev_t devt, struct cdev *cdev,
			      struct class *class)
{
	device_destroy(class, devt);
	class_destroy(class);
	cdev_del(cdev);
	unregister_chrdev_region(devt, 1);
}

int mitee_tui_init(void)
{
	struct mitee_tui *tui;
	int rc;

	pr_info("Loading mitee-tui module.\n");

	tui = kzalloc(sizeof(*tui), GFP_KERNEL);
	if (!tui)
		return -ENOMEM;

	atomic_set(&tui->opened, 0);
	init_completion(&tui->user_comp);
	init_waitqueue_head(&tui->event_queue);
	mutex_init(&tui->shm_mutex);
	idr_init(&tui->shm_idr);
	mitee_tui = tui;

	rc = tui_add_device("mitee-tui", "tui_cls", &tui->devt, &tui->cdev,
			    &tui->class, &tui->dev);
	if (rc) {
		pr_err("Unable to add Trusted UI char device: %d\n", rc);
		goto err_free;
	}

	rc = tui_add_device("mitee-tui-supp", "tui_supp_cls", &tui->supp_devt,
			    &tui->supp_cdev, &tui->supp_class, &tui->supp_dev);
	if (rc) {
		pr_err("Unable to add Trusted UI supp char device: %d\n", rc);
		goto err_remove_tui;
	}

	/* Fails until the TEE is up, TUI_IO_SIGNAL then connects again */
	tui->token = mitee_smc_notify_connect(0, TUI_NOTIFY_ID);
	pr_info("get mitee_tui connect token: %x\n", tui->token);

	return 0;

err_remove_tui:
	tui_remove_device(tui->devt, &tui->cdev, tui->class);
err_free:
	mitee_tui = NULL;
	idr_destroy(&tui->shm_idr);
	kfree(tui);
	return rc;
}

void mitee_tui_exit(void)
{
	struct mitee_tui *tui = mitee_tui;
	struct tui_shm *shm;
	int id;

	if (!tui)
		return;

	tui_remove_device(tui->supp_devt, &tui->supp_cdev, tui->supp_class);
	tui_remove_device(tui->devt, &tui->cdev, tui->class);

	tui_font_drop_locked(tui);
	idr_for_each_entry(&tui->shm_idr, shm, id)
		tui_shm_put(shm);
	idr_destroy(&tui->shm_idr);
	mitee_tui = NULL;
	kfree(tui);
}
