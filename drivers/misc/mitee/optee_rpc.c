// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2021, Linaro Limited
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": %s: " fmt, __func__

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include "mitee_private.h"
#include "optee_rpc_cmd.h"

struct wq_entry {
	struct list_head link;
	struct completion c;
	u32 key;
};

void optee_wait_queue_init(struct optee_wait_queue *priv)
{
	mutex_init(&priv->mu);
	INIT_LIST_HEAD(&priv->db);
}

void optee_wait_queue_exit(struct optee_wait_queue *priv)
{
	mutex_destroy(&priv->mu);
}

static void handle_rpc_func_cmd_get_time(struct optee_msg_arg *arg)
{
	struct timespec64 ts;

	if (arg->num_params != 1)
		goto bad;
	if ((arg->params[0].attr & OPTEE_MSG_ATTR_TYPE_MASK) !=
			OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT)
		goto bad;

	ktime_get_real_ts64(&ts);
	arg->params[0].u.value.a = ts.tv_sec;
	arg->params[0].u.value.b = ts.tv_nsec;

	arg->ret = TEEC_SUCCESS;
	return;
bad:
	arg->ret = TEEC_ERROR_BAD_PARAMETERS;
}

#if IS_REACHABLE(CONFIG_I2C)
static void handle_rpc_func_cmd_i2c_transfer(struct tee_context *ctx,
					     struct optee_msg_arg *arg)
{
	struct mitee *mitee = tee_get_drvdata(ctx->teedev);
	struct tee_param *params;
	struct i2c_adapter *adapter;
	struct i2c_msg msg = { };
	size_t i;
	int ret = -EOPNOTSUPP;
	u8 attr[] = {
		TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT,
		TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT,
		TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT,
		TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT,
	};

	if (arg->num_params != ARRAY_SIZE(attr)) {
		arg->ret = TEEC_ERROR_BAD_PARAMETERS;
		return;
	}

	params = kmalloc_array(arg->num_params, sizeof(struct tee_param),
			       GFP_KERNEL);
	if (!params) {
		arg->ret = TEEC_ERROR_OUT_OF_MEMORY;
		return;
	}

	if (optee_from_msg_param(mitee, params, arg->num_params,
				       arg->params))
		goto bad;

	for (i = 0; i < arg->num_params; i++) {
		if (params[i].attr != attr[i])
			goto bad;
	}

	adapter = i2c_get_adapter(params[0].u.value.b);
	if (!adapter)
		goto bad;

	if (params[1].u.value.a & OPTEE_RPC_I2C_FLAGS_TEN_BIT) {
		if (!i2c_check_functionality(adapter,
					     I2C_FUNC_10BIT_ADDR)) {
			i2c_put_adapter(adapter);
			goto bad;
		}

		msg.flags = I2C_M_TEN;
	}

	msg.addr = params[0].u.value.c;
	msg.buf  = params[2].u.memref.shm->kaddr;
	msg.len  = params[2].u.memref.size;

	switch (params[0].u.value.a) {
	case OPTEE_RPC_I2C_TRANSFER_RD:
		msg.flags |= I2C_M_RD;
		break;
	case OPTEE_RPC_I2C_TRANSFER_WR:
		break;
	default:
		i2c_put_adapter(adapter);
		goto bad;
	}

	ret = i2c_transfer(adapter, &msg, 1);

	if (ret < 0) {
		arg->ret = TEEC_ERROR_COMMUNICATION;
	} else {
		params[3].u.value.a = msg.len;
		if (optee_to_msg_param(mitee, arg->params,
					     arg->num_params, params))
			arg->ret = TEEC_ERROR_BAD_PARAMETERS;
		else
			arg->ret = TEEC_SUCCESS;
	}

	i2c_put_adapter(adapter);
	kfree(params);
	return;
bad:
	kfree(params);
	arg->ret = TEEC_ERROR_BAD_PARAMETERS;
}
#else
static void handle_rpc_func_cmd_i2c_transfer(struct tee_context *ctx,
					     struct optee_msg_arg *arg)
{
	arg->ret = TEEC_ERROR_NOT_SUPPORTED;
}
#endif

static struct wq_entry *wq_entry_get(struct optee_wait_queue *wq, u32 key)
{
	struct wq_entry *w;

	mutex_lock(&wq->mu);

	list_for_each_entry(w, &wq->db, link)
		if (w->key == key)
			goto out;

	w = kmalloc(sizeof(*w), GFP_KERNEL);
	if (w) {
		init_completion(&w->c);
		w->key = key;
		list_add_tail(&w->link, &wq->db);
	}
out:
	mutex_unlock(&wq->mu);
	return w;
}

static void wq_sleep(struct optee_wait_queue *wq, u32 key)
{
	struct wq_entry *w = wq_entry_get(wq, key);

	if (w) {
		wait_for_completion(&w->c);
		mutex_lock(&wq->mu);
		list_del(&w->link);
		mutex_unlock(&wq->mu);
		kfree(w);
	}
}

static void wq_wakeup(struct optee_wait_queue *wq, u32 key)
{
	struct wq_entry *w = wq_entry_get(wq, key);

	if (w)
		complete(&w->c);
}

static void handle_rpc_func_cmd_wq(struct mitee *mitee,
				   struct optee_msg_arg *arg)
{
	if (arg->num_params != 1)
		goto bad;

	if ((arg->params[0].attr & OPTEE_MSG_ATTR_TYPE_MASK) !=
			OPTEE_MSG_ATTR_TYPE_VALUE_INPUT)
		goto bad;

	switch (arg->params[0].u.value.a) {
	case OPTEE_RPC_WAIT_QUEUE_SLEEP:
		wq_sleep(&mitee->wait_queue, arg->params[0].u.value.b);
		break;
	case OPTEE_RPC_WAIT_QUEUE_WAKEUP:
		wq_wakeup(&mitee->wait_queue, arg->params[0].u.value.b);
		break;
	default:
		goto bad;
	}

	arg->ret = TEEC_SUCCESS;
	return;
bad:
	arg->ret = TEEC_ERROR_BAD_PARAMETERS;
}

static void handle_rpc_func_cmd_wait(struct optee_msg_arg *arg)
{
	u32 msec_to_wait;

	if (arg->num_params != 1)
		goto bad;

	if ((arg->params[0].attr & OPTEE_MSG_ATTR_TYPE_MASK) !=
			OPTEE_MSG_ATTR_TYPE_VALUE_INPUT)
		goto bad;

	msec_to_wait = arg->params[0].u.value.a;

	/* Go to interruptible sleep */
	msleep_interruptible(msec_to_wait);

	arg->ret = TEEC_SUCCESS;
	return;
bad:
	arg->ret = TEEC_ERROR_BAD_PARAMETERS;
}

static void handle_rpc_supp_cmd(struct tee_context *ctx, struct mitee *mitee,
				struct optee_msg_arg *arg)
{
	struct tee_param *params;

	arg->ret_origin = TEEC_ORIGIN_COMMS;

	params = kmalloc_array(arg->num_params, sizeof(struct tee_param),
			       GFP_KERNEL);
	if (!params) {
		arg->ret = TEEC_ERROR_OUT_OF_MEMORY;
		return;
	}

	if (optee_from_msg_param(mitee, params, arg->num_params,
				       arg->params)) {
		arg->ret = TEEC_ERROR_BAD_PARAMETERS;
		goto out;
	}

	arg->ret = optee_supp_thrd_req(ctx, arg->cmd, arg->num_params, params);

	if (optee_to_msg_param(mitee, arg->params, arg->num_params,
				     params))
		arg->ret = TEEC_ERROR_BAD_PARAMETERS;
out:
	kfree(params);
}

struct tee_shm *optee_rpc_cmd_alloc_suppl(struct tee_context *ctx, size_t sz)
{
	u32 ret;
	struct tee_param param;
	struct mitee *mitee = tee_get_drvdata(ctx->teedev);
	struct tee_shm *shm;

	param.attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT;
	param.u.value.a = OPTEE_RPC_SHM_TYPE_APPL;
	param.u.value.b = sz;
	param.u.value.c = 0;

	ret = optee_supp_thrd_req(ctx, OPTEE_RPC_CMD_SHM_ALLOC, 1, &param);
	if (ret)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&mitee->supp.mutex);
	/* Increases count as secure world doesn't have a reference */
	shm = tee_shm_get_from_id(mitee->supp.ctx, param.u.value.c);
	mutex_unlock(&mitee->supp.mutex);
	return shm;
}

void optee_rpc_cmd_free_suppl(struct tee_context *ctx, struct tee_shm *shm)
{
	struct tee_param param;

	param.attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT;
	param.u.value.a = OPTEE_RPC_SHM_TYPE_APPL;
	param.u.value.b = shm->id;
	param.u.value.c = 0;

	/*
	 * Match the tee_shm_get_from_id() in cmd_alloc_suppl() as secure
	 * world has released its reference.
	 *
	 * It's better to do this before sending the request to supplicant
	 * as we'd like to let the process doing the initial allocation to
	 * do release the last reference too in order to avoid stacking
	 * many pending fput() on the client process. This could otherwise
	 * happen if secure world does many allocate and free in a single
	 * invoke.
	 */
	tee_shm_put(shm);

	optee_supp_thrd_req(ctx, OPTEE_RPC_CMD_SHM_FREE, 1, &param);
}

static void handle_rpc_func_cmd_shm_alloc(struct tee_context *ctx,
					  struct optee_msg_arg *arg)
{
	struct tee_shm *shm;

	arg->ret_origin = TEEC_ORIGIN_COMMS;

	if (arg->num_params != 1 ||
	    arg->params[0].attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT) {
		arg->ret = TEEC_ERROR_BAD_PARAMETERS;
		return;
	}

	switch (arg->params[0].u.value.a) {
	case OPTEE_RPC_SHM_TYPE_APPL:
		shm = optee_rpc_cmd_alloc_suppl(ctx, arg->params[0].u.value.b);
		break;
	case OPTEE_RPC_SHM_TYPE_KERNEL:
		shm = tee_shm_alloc(ctx, arg->params[0].u.value.b,
				    TEE_SHM_MAPPED | TEE_SHM_PRIV);
		break;
	case OPTEE_RPC_SHM_TYPE_GLOBAL:
		shm = tee_shm_alloc(ctx, arg->params[0].u.value.b,
				    TEE_SHM_MAPPED | TEE_SHM_DMA_BUF |
				    TEE_SHM_PRIV);
		break;
	default:
		arg->ret = TEEC_ERROR_BAD_PARAMETERS;
		return;
	}

	if (IS_ERR(shm)) {
		arg->ret = TEEC_ERROR_OUT_OF_MEMORY;
		return;
	}

	/* The shared memory pool is physically contiguous */
	arg->params[0].attr = OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT;
	arg->params[0].u.tmem.buf_ptr = shm->paddr;
	arg->params[0].u.tmem.size = shm->size;
	arg->params[0].u.tmem.shm_ref = (unsigned long)shm;
	arg->ret = TEEC_SUCCESS;
}

static void handle_rpc_func_cmd_shm_free(struct tee_context *ctx,
					 struct optee_msg_arg *arg)
{
	struct tee_shm *shm;

	arg->ret_origin = TEEC_ORIGIN_COMMS;

	if (arg->num_params != 1 ||
	    arg->params[0].attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT)
		goto bad;

	shm = (struct tee_shm *)(unsigned long)arg->params[0].u.value.b;
	if (!shm)
		goto bad;

	switch (arg->params[0].u.value.a) {
	case OPTEE_RPC_SHM_TYPE_APPL:
		optee_rpc_cmd_free_suppl(ctx, shm);
		break;
	case OPTEE_RPC_SHM_TYPE_KERNEL:
	case OPTEE_RPC_SHM_TYPE_GLOBAL:
		tee_shm_free(shm);
		break;
	default:
		goto bad;
	}

	arg->ret = TEEC_SUCCESS;
	return;
bad:
	arg->ret = TEEC_ERROR_BAD_PARAMETERS;
}

struct mitee_rpc_callback {
	struct list_head link;
	mitee_rpc_callback_t callback;
	u32 client;
	u32 cmd;
};

void mitee_rpc_callback_queue_init(struct mitee_rpc_callback_queue *queue)
{
	mutex_init(&queue->mutex);
	INIT_LIST_HEAD(&queue->callbacks);
}

void mitee_rpc_callback_queue_exit(struct mitee_rpc_callback_queue *queue)
{
	struct mitee_rpc_callback *cb, *tmp;

	list_for_each_entry_safe(cb, tmp, &queue->callbacks, link) {
		list_del(&cb->link);
		kfree(cb);
	}
	mutex_destroy(&queue->mutex);
}

/**
 * mitee_rpc_register_callback() - serve OPTEE_RPC_CMD_MITEE_CALLBACK requests
 * @client:	service id
 * @cmd:	command of the service
 * @callback:	handler, replaces an earlier handler of the same command
 */
int mitee_rpc_register_callback(u32 client, u32 cmd,
				mitee_rpc_callback_t callback)
{
	struct mitee_rpc_callback_queue *queue = &mitee_svc->callbacks;
	struct mitee_rpc_callback *cb, *tmp;

	cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb)
		return -ENOMEM;

	cb->client = client;
	cb->cmd = cmd;
	cb->callback = callback;

	mutex_lock(&queue->mutex);
	list_for_each_entry(tmp, &queue->callbacks, link) {
		if (tmp->client == client && tmp->cmd == cmd) {
			list_del(&tmp->link);
			kfree(tmp);
			break;
		}
	}
	list_add_tail(&cb->link, &queue->callbacks);
	mutex_unlock(&queue->mutex);

	return 0;
}

static int optee_msg_arg_to_callback_para(struct optee_msg_arg *arg,
					  u64 **value, void **buf,
					  u32 *size)
{
	struct optee_msg_param *mp;
	struct tee_shm *shm;
	phys_addr_t pa;
	void *va;
	int rc;

	if (arg->num_params < 3)
		return -EINVAL;

	*value = NULL;
	*buf = NULL;
	*size = 0;

	if (arg->params[1].attr)
		*value = &arg->params[1].u.value.a;

	mp = &arg->params[2];
	if (!mp->attr)
		return 0;

	shm = (struct tee_shm *)(unsigned long)mp->u.tmem.shm_ref;
	if (!shm) {
		pr_err("mitee: callback invalid shm\n");
		return -EINVAL;
	}

	rc = tee_shm_get_pa(shm, 0, &pa);
	if (rc) {
		pr_err("mitee: callback invalid shm pa, rc %d\n", rc);
		return rc;
	}

	va = tee_shm_get_va(shm, mp->u.tmem.buf_ptr - pa);
	if (IS_ERR(va))
		return PTR_ERR(va);

	*buf = va;
	*size = mp->u.tmem.size;
	return 0;
}

static void handle_rpc_cmd_callback(struct mitee *mitee,
				    struct optee_msg_arg *arg)
{
	struct mitee_rpc_callback_queue *queue = &mitee->callbacks;
	struct mitee_rpc_callback *cb;
	u32 cmd, client, out_size;
	u64 *value;
	void *buf;
	u32 size;
	int rc;

	if (!arg->num_params) {
		arg->ret = TEEC_ERROR_BAD_PARAMETERS;
		return;
	}

	cmd = arg->params[0].u.value.a;
	client = arg->params[0].u.value.b;

	mutex_lock(&queue->mutex);
	list_for_each_entry(cb, &queue->callbacks, link) {
		if (cb->client != client || cb->cmd != cmd)
			continue;

		if (cmd != MITEE_RPC_CALLBACK_CMD_BUFFER) {
			arg->ret = cb->callback(&arg->params[0].u.value.a,
						NULL, 0, NULL);
			break;
		}

		rc = optee_msg_arg_to_callback_para(arg, &value, &buf, &size);
		if (rc) {
			pr_err("mitee callback msg_arg invalid %d\n", rc);
			arg->ret = TEEC_ERROR_BAD_PARAMETERS;
			break;
		}

		out_size = 0;
		arg->ret = cb->callback(value, buf, size, &out_size);
		arg->params[2].u.tmem.size = out_size;
		break;
	}
	mutex_unlock(&queue->mutex);
}

void optee_rpc_cmd(struct tee_context *ctx, struct mitee *mitee,
		   struct optee_msg_arg *arg)
{
	switch (arg->cmd) {
	case OPTEE_RPC_CMD_GET_TIME:
		handle_rpc_func_cmd_get_time(arg);
		break;
	case OPTEE_RPC_CMD_WAIT_QUEUE:
		handle_rpc_func_cmd_wq(mitee, arg);
		break;
	case OPTEE_RPC_CMD_SUSPEND:
		handle_rpc_func_cmd_wait(arg);
		break;
	case OPTEE_RPC_CMD_SHM_ALLOC:
		handle_rpc_func_cmd_shm_alloc(ctx, arg);
		break;
	case OPTEE_RPC_CMD_SHM_FREE:
		handle_rpc_func_cmd_shm_free(ctx, arg);
		break;
	case OPTEE_RPC_CMD_BENCH_REG:
		arg->ret = TEEC_ERROR_NOT_SUPPORTED;
		break;
	case OPTEE_RPC_CMD_I2C_TRANSFER:
		handle_rpc_func_cmd_i2c_transfer(ctx, arg);
		break;
	case OPTEE_RPC_CMD_MITEE_CALLBACK:
		handle_rpc_cmd_callback(mitee, arg);
		break;
	default:
		handle_rpc_supp_cmd(ctx, mitee, arg);
	}
}
