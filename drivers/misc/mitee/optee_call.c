// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2021, Linaro Limited
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": %s: " fmt, __func__

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "mitee_msgq.h"
#include "mitee_private.h"

/*
 * Only executables from these directories may open sessions with trusted
 * applications.
 */
static const char * const mitee_ca_paths[] = {
	"/vendor/bin",
	"/odm/bin",
	"/apex/com.google.android.widevine/bin/hw",
};

static bool mitee_path_has_prefix(const char *path, const char *prefix)
{
	size_t len = strlen(prefix);

	return !strncmp(path, prefix, len) &&
	       (path[len] == '\0' || path[len] == '/');
}

/* get_task_exe_file() is not exported to modules */
static struct file *mitee_current_exe_file(void)
{
	struct mm_struct *mm = current->mm;
	struct file *exe_file;

	if (!mm || (current->flags & PF_KTHREAD))
		return NULL;

	rcu_read_lock();
	exe_file = rcu_dereference(mm->exe_file);
	if (exe_file && !get_file_rcu(exe_file))
		exe_file = NULL;
	rcu_read_unlock();

	return exe_file;
}

static int mitee_auth_ca(void)
{
	struct file *exe_file;
	char *buf, *path;
	int rc = -EINVAL;
	size_t i;

	exe_file = mitee_current_exe_file();
	if (!exe_file)
		return -EINVAL;

	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buf) {
		fput(exe_file);
		return -ENOMEM;
	}

	path = d_path(&exe_file->f_path, buf, PATH_MAX);
	if (IS_ERR(path)) {
		rc = PTR_ERR(path);
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(mitee_ca_paths); i++) {
		if (mitee_path_has_prefix(path, mitee_ca_paths[i])) {
			rc = 0;
			goto out;
		}
	}

	pr_err("fail to auth ca path: %s\n", path);
out:
	kfree(buf);
	fput(exe_file);
	return rc;
}

/* Requires the filpstate mutex to be held */
static struct optee_session *find_session(struct optee_context_data *ctxdata,
					  u32 session_id)
{
	struct optee_session *sess;

	list_for_each_entry(sess, &ctxdata->sess_list, list_node)
		if (sess->session_id == session_id)
			return sess;

	return NULL;
}

static struct optee_msg_arg *optee_alloc_msg_arg(size_t num_params)
{
	struct optee_msg_arg *ma;

	if (num_params > MITEE_MSG_MAX_PARAMS)
		return ERR_PTR(-EINVAL);

	ma = kzalloc(OPTEE_MSG_GET_ARG_SIZE(num_params), GFP_KERNEL);
	if (!ma)
		return ERR_PTR(-ENOMEM);

	ma->num_params = num_params;
	return ma;
}

/**
 * optee_do_call_with_arg() - Do a call into the TEE
 * @ctx:	calling context
 * @arg:	message argument, updated with the result of the call
 *
 * Calls are queued for the worker thread, which passes them to the TEE and
 * serves its RPC requests until the call is done.
 *
 * Returns 0 if the call reached the TEE, the result is then in @arg->ret.
 */
int optee_do_call_with_arg(struct tee_context *ctx, struct optee_msg_arg *arg)
{
	if (mitee_msgq_vm_state() != MITEE_VM_STATE_RUNNING) {
		pr_err("tee didn't bootup done, try later\n");
		return -EAGAIN;
	}

	return mitee_task_call(ctx, arg);
}

int optee_open_session(struct tee_context *ctx,
		       struct tee_ioctl_open_session_arg *arg,
		       struct tee_param *param)
{
	struct mitee *mitee = tee_get_drvdata(ctx->teedev);
	struct optee_context_data *ctxdata = ctx->data;
	struct optee_msg_arg *msg_arg;
	struct optee_session *sess = NULL;
	uuid_t client_uuid;
	int rc;

	rc = mitee_auth_ca();
	if (rc) {
		pr_err("auth ca name error\n");
		return rc;
	}

	/* +2 for the meta parameters added below */
	msg_arg = optee_alloc_msg_arg(arg->num_params + 2);
	if (IS_ERR(msg_arg))
		return PTR_ERR(msg_arg);

	msg_arg->cmd = OPTEE_MSG_CMD_OPEN_SESSION;
	msg_arg->cancel_id = arg->cancel_id;

	/*
	 * Initialize and add the meta parameters needed when opening a
	 * session.
	 */
	msg_arg->params[0].attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT |
				  OPTEE_MSG_ATTR_META;
	msg_arg->params[1].attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT |
				  OPTEE_MSG_ATTR_META;
	memcpy(&msg_arg->params[0].u.value, arg->uuid, sizeof(arg->uuid));
	msg_arg->params[1].u.value.c = arg->clnt_login;

	rc = tee_session_calc_client_uuid(&client_uuid, arg->clnt_login,
					  arg->clnt_uuid);
	if (rc)
		goto out;
	export_uuid(msg_arg->params[1].u.octets, &client_uuid);

	rc = optee_to_msg_param(mitee, msg_arg->params + 2, arg->num_params,
				param);
	if (rc)
		goto out;

	sess = kzalloc(sizeof(*sess), GFP_KERNEL);
	if (!sess) {
		rc = -ENOMEM;
		goto out;
	}

	if (optee_do_call_with_arg(ctx, msg_arg)) {
		msg_arg->ret = TEEC_ERROR_COMMUNICATION;
		msg_arg->ret_origin = TEEC_ORIGIN_COMMS;
	}

	if (msg_arg->ret == TEEC_SUCCESS) {
		/* A new session has been created, add it to the list. */
		sess->session_id = msg_arg->session;
		mutex_lock(&ctxdata->mutex);
		list_add(&sess->list_node, &ctxdata->sess_list);
		mutex_unlock(&ctxdata->mutex);
	} else {
		pr_warn("%s failed, msg_arg->ret: %#x\n", __func__,
			msg_arg->ret);
		kfree(sess);
	}

	if (optee_from_msg_param(mitee, param, arg->num_params,
				 msg_arg->params + 2)) {
		arg->ret = TEEC_ERROR_COMMUNICATION;
		arg->ret_origin = TEEC_ORIGIN_COMMS;
		/* Close session again to avoid leakage */
		optee_close_session(ctx, msg_arg->session);
	} else {
		arg->session = msg_arg->session;
		arg->ret = msg_arg->ret;
		arg->ret_origin = msg_arg->ret_origin;
	}
out:
	kfree(msg_arg);

	return rc;
}

int optee_close_session_helper(struct tee_context *ctx, u32 session)
{
	struct optee_msg_arg *msg_arg;

	msg_arg = optee_alloc_msg_arg(0);
	if (IS_ERR(msg_arg))
		return PTR_ERR(msg_arg);

	msg_arg->cmd = OPTEE_MSG_CMD_CLOSE_SESSION;
	msg_arg->session = session;
	optee_do_call_with_arg(ctx, msg_arg);

	kfree(msg_arg);

	return 0;
}

int optee_close_session(struct tee_context *ctx, u32 session)
{
	struct optee_context_data *ctxdata = ctx->data;
	struct optee_session *sess;

	/* Check that the session is valid and remove it from the list */
	mutex_lock(&ctxdata->mutex);
	sess = find_session(ctxdata, session);
	if (sess)
		list_del(&sess->list_node);
	mutex_unlock(&ctxdata->mutex);
	if (!sess)
		return -EINVAL;
	kfree(sess);

	return optee_close_session_helper(ctx, session);
}

int optee_invoke_func(struct tee_context *ctx, struct tee_ioctl_invoke_arg *arg,
		      struct tee_param *param)
{
	struct mitee *mitee = tee_get_drvdata(ctx->teedev);
	struct optee_context_data *ctxdata = ctx->data;
	struct optee_msg_arg *msg_arg;
	struct optee_session *sess;
	int rc;

	/* Check that the session is valid */
	mutex_lock(&ctxdata->mutex);
	sess = find_session(ctxdata, arg->session);
	mutex_unlock(&ctxdata->mutex);
	if (!sess)
		return -EINVAL;

	msg_arg = optee_alloc_msg_arg(arg->num_params);
	if (IS_ERR(msg_arg))
		return PTR_ERR(msg_arg);
	msg_arg->cmd = OPTEE_MSG_CMD_INVOKE_COMMAND;
	msg_arg->func = arg->func;
	msg_arg->session = arg->session;
	msg_arg->cancel_id = arg->cancel_id;

	rc = optee_to_msg_param(mitee, msg_arg->params, arg->num_params, param);
	if (rc)
		goto out;

	if (optee_do_call_with_arg(ctx, msg_arg)) {
		msg_arg->ret = TEEC_ERROR_COMMUNICATION;
		msg_arg->ret_origin = TEEC_ORIGIN_COMMS;
	}

	if (optee_from_msg_param(mitee, param, arg->num_params,
				 msg_arg->params)) {
		msg_arg->ret = TEEC_ERROR_COMMUNICATION;
		msg_arg->ret_origin = TEEC_ORIGIN_COMMS;
	}

	arg->ret = msg_arg->ret;
	arg->ret_origin = msg_arg->ret_origin;
out:
	kfree(msg_arg);
	return rc;
}

/* Calls are served one at a time, there is nothing to cancel */
int optee_cancel_req(struct tee_context *ctx, u32 cancel_id, u32 session)
{
	pr_err("%s cancel is not support!\n", __func__);
	return -EACCES;
}

static bool is_normal_memory(pgprot_t p)
{
	return (pgprot_val(p) & PTE_ATTRINDX_MASK) == PTE_ATTRINDX(MT_NORMAL);
}

static int __check_mem_type(struct mm_struct *mm, unsigned long start,
			    unsigned long end)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, start);

	for_each_vma_range(vmi, vma, end) {
		if (!is_normal_memory(vma->vm_page_prot))
			return -EINVAL;
	}

	return 0;
}

int optee_check_mem_type(unsigned long start, size_t num_pages)
{
	struct mm_struct *mm = current->mm;
	int rc;

	/*
	 * Allow kernel address to register with OP-TEE as kernel
	 * pages are configured as normal memory only.
	 */
	if (virt_addr_valid((void *)start) || is_vmalloc_addr((void *)start))
		return 0;

	mmap_read_lock(mm);
	rc = __check_mem_type(mm, start, start + num_pages * PAGE_SIZE);
	mmap_read_unlock(mm);

	return rc;
}
