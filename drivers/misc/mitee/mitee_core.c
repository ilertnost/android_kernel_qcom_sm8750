// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2021, Linaro Limited
 *
 * MiTEE driver core: TEE device registration and OP-TEE message parameter
 * handling for the OP-TEE OS running in the "oemvm" Gunyah VM.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": %s: " fmt, __func__

#include <linux/err.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "mitee_msgq.h"
#include "mitee_private.h"

struct mitee *mitee_svc;

static int from_msg_param_tmp_mem(struct tee_param *p, u32 attr,
				  const struct optee_msg_param *mp)
{
	struct tee_shm *shm;
	phys_addr_t pa;
	int rc;

	p->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT +
		  attr - OPTEE_MSG_ATTR_TYPE_TMEM_INPUT;
	p->u.memref.size = mp->u.tmem.size;
	shm = (struct tee_shm *)(unsigned long)mp->u.tmem.shm_ref;
	if (!shm) {
		p->u.memref.shm_offs = 0;
		p->u.memref.shm = NULL;
		return 0;
	}

	rc = tee_shm_get_pa(shm, 0, &pa);
	if (rc)
		return rc;

	p->u.memref.shm_offs = mp->u.tmem.buf_ptr - pa;
	p->u.memref.shm = shm;

	/* Check that the memref is covered by the shm object */
	if (p->u.memref.size) {
		size_t o = p->u.memref.shm_offs + p->u.memref.size - 1;

		rc = tee_shm_get_pa(shm, o, NULL);
		if (rc)
			return rc;
	}

	return 0;
}

/**
 * optee_from_msg_param() - convert from OPTEE_MSG parameters to
 *			    struct tee_param
 * @mitee:	main service struct
 * @params:	subsystem internal parameter representation
 * @num_params:	number of elements in the parameter arrays
 * @msg_params:	OPTEE_MSG parameters
 *
 * Returns 0 on success or <0 on failure
 */
int optee_from_msg_param(struct mitee *mitee, struct tee_param *params,
			 size_t num_params,
			 const struct optee_msg_param *msg_params)
{
	int rc;
	size_t n;

	for (n = 0; n < num_params; n++) {
		struct tee_param *p = params + n;
		const struct optee_msg_param *mp = msg_params + n;
		u32 attr = mp->attr & OPTEE_MSG_ATTR_TYPE_MASK;

		switch (attr) {
		case OPTEE_MSG_ATTR_TYPE_NONE:
			p->attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
			memset(&p->u, 0, sizeof(p->u));
			break;
		case OPTEE_MSG_ATTR_TYPE_VALUE_INPUT:
		case OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_VALUE_INOUT:
			optee_from_msg_param_value(p, attr, mp);
			break;
		case OPTEE_MSG_ATTR_TYPE_FMEM_INPUT:
		case OPTEE_MSG_ATTR_TYPE_FMEM_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_FMEM_INOUT:
			mitee_msgq_from_msg_param(p, attr, mp);
			break;
		case OPTEE_MSG_ATTR_TYPE_TMEM_INPUT:
		case OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_TMEM_INOUT:
			rc = from_msg_param_tmp_mem(p, attr, mp);
			if (rc) {
				pr_err("tmp mem err: %d!\n", rc);
				return rc;
			}
			break;
		default:
			pr_err("unsupported mem type: %d!\n", attr);
			return -EINVAL;
		}
	}

	return 0;
}

static int to_msg_param_tmp_mem(struct optee_msg_param *mp,
				const struct tee_param *p)
{
	int rc;
	phys_addr_t pa;

	mp->attr = OPTEE_MSG_ATTR_TYPE_TMEM_INPUT + p->attr -
		   TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;

	mp->u.tmem.shm_ref = (unsigned long)p->u.memref.shm;
	mp->u.tmem.size = p->u.memref.size;

	if (!p->u.memref.shm) {
		mp->u.tmem.buf_ptr = 0;
		return 0;
	}

	rc = tee_shm_get_pa(p->u.memref.shm, p->u.memref.shm_offs, &pa);
	if (rc)
		return rc;

	mp->u.tmem.buf_ptr = pa;

	return 0;
}

/**
 * optee_to_msg_param() - convert from struct tee_params to OPTEE_MSG
 *			  parameters
 * @mitee:	main service struct
 * @msg_params:	OPTEE_MSG parameters
 * @num_params:	number of elements in the parameter arrays
 * @params:	subsystem itnernal parameter representation
 * Returns 0 on success or <0 on failure
 *
 * Memory from the static shared memory pool is passed by physical address,
 * registered memory by the handle of the Gunyah memory parcel.
 */
int optee_to_msg_param(struct mitee *mitee, struct optee_msg_param *msg_params,
		       size_t num_params, const struct tee_param *params)
{
	int rc;
	size_t n;

	for (n = 0; n < num_params; n++) {
		const struct tee_param *p = params + n;
		struct optee_msg_param *mp = msg_params + n;

		switch (p->attr) {
		case TEE_IOCTL_PARAM_ATTR_TYPE_NONE:
			mp->attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
			memset(&mp->u, 0, sizeof(mp->u));
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT:
			optee_to_msg_param_value(mp, p);
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT:
			if (tee_shm_is_registered(p->u.memref.shm))
				rc = mitee_msgq_to_msg_param(mp, p);
			else
				rc = to_msg_param_tmp_mem(mp, p);
			if (rc) {
				pr_err("%s attr: %#llx failed: %d!\n",
				       __func__, p->attr, rc);
				return rc;
			}
			break;
		default:
			pr_err("%s unsupport attr: %#llx!\n", __func__, p->attr);
			return -EINVAL;
		}
	}

	return 0;
}

static void mitee_get_version(struct tee_device *teedev,
			      struct tee_ioctl_version_data *vers)
{
	struct mitee *mitee = tee_get_drvdata(teedev);
	struct tee_ioctl_version_data v = {
		.impl_id = TEE_IMPL_ID_OPTEE,
		.impl_caps = TEE_OPTEE_CAP_TZ,
		.gen_caps = TEE_GEN_CAP_GP,
	};

	/* Only clients may register their own memory with the TEE */
	if (teedev != mitee->supp_teedev)
		v.gen_caps |= TEE_GEN_CAP_REG_MEM;
	*vers = v;
}

static int mitee_open(struct tee_context *ctx)
{
	struct optee_context_data *ctxdata;
	struct tee_device *teedev = ctx->teedev;
	struct mitee *mitee = tee_get_drvdata(teedev);

	ctxdata = kzalloc(sizeof(*ctxdata), GFP_KERNEL);
	if (!ctxdata)
		return -ENOMEM;

	if (teedev == mitee->supp_teedev) {
		bool busy = true;

		mutex_lock(&mitee->supp.mutex);
		if (!mitee->supp.ctx) {
			busy = false;
			mitee->supp.ctx = ctx;
		}
		mutex_unlock(&mitee->supp.mutex);
		if (busy) {
			kfree(ctxdata);
			return -EBUSY;
		}
	}
	mutex_init(&ctxdata->mutex);
	INIT_LIST_HEAD(&ctxdata->sess_list);

	ctx->cap_memref_null = true;
	ctx->data = ctxdata;
	return 0;
}

static void mitee_release_helper(struct tee_context *ctx)
{
	struct optee_context_data *ctxdata = ctx->data;
	struct optee_session *sess;
	struct optee_session *sess_tmp;

	if (!ctxdata)
		return;

	list_for_each_entry_safe(sess, sess_tmp, &ctxdata->sess_list,
				 list_node) {
		list_del(&sess->list_node);
		optee_close_session_helper(ctx, sess->session_id);
		kfree(sess);
	}
	kfree(ctxdata);
	ctx->data = NULL;
}

static void mitee_release(struct tee_context *ctx)
{
	mitee_release_helper(ctx);
}

static void mitee_release_supp(struct tee_context *ctx)
{
	struct mitee *mitee = tee_get_drvdata(ctx->teedev);

	mitee_release_helper(ctx);
	optee_supp_release(&mitee->supp);
}

static const struct tee_driver_ops mitee_clnt_ops = {
	.get_version = mitee_get_version,
	.open = mitee_open,
	.release = mitee_release,
	.open_session = optee_open_session,
	.close_session = optee_close_session,
	.invoke_func = optee_invoke_func,
	.cancel_req = optee_cancel_req,
	.shm_register = mitee_msgq_shm_register,
	.shm_unregister = mitee_msgq_shm_unregister,
};

static const struct tee_desc mitee_clnt_desc = {
	.name = "mitee-clnt",
	.ops = &mitee_clnt_ops,
	.owner = THIS_MODULE,
};

static const struct tee_driver_ops mitee_supp_ops = {
	.get_version = mitee_get_version,
	.open = mitee_open,
	.release = mitee_release_supp,
	.supp_recv = optee_supp_recv,
	.supp_send = optee_supp_send,
	.shm_register = mitee_msgq_shm_register,
	.shm_unregister = mitee_msgq_shm_unregister_supp,
};

static const struct tee_desc mitee_supp_desc = {
	.name = "mitee-supp",
	.ops = &mitee_supp_ops,
	.owner = THIS_MODULE,
	.flags = TEE_DESC_PRIVILEGED,
};

/*
 * The first page of the static shared memory is used for driver private
 * allocations, the rest for buffers exported to user space.
 */
static struct tee_shm_pool *mitee_shm_pool_alloc(void **memremaped_shm)
{
	struct mitee_regs regs = { .a0 = MITEE_CALL_GET_SHM_POOL };
	struct tee_shm_pool_mgr *priv_mgr;
	struct tee_shm_pool_mgr *dmabuf_mgr;
	struct tee_shm_pool *pool;
	phys_addr_t begin, end, paddr;
	unsigned long vaddr;
	size_t size;
	void *va;
	int rc;

	rc = mitee_msgq_call(&regs);
	if (rc) {
		pr_err("get shm pool failed: %d\n", rc);
		*memremaped_shm = NULL;
		return NULL;
	}

	if (regs.a2 != 1) {
		pr_err("only normal cached shared memory supported\n");
		return ERR_PTR(-EINVAL);
	}

	pr_info("get shm pool: %llx size: %llx\n", regs.a0, regs.a1);
	begin = roundup(regs.a0, PAGE_SIZE);
	end = rounddown(regs.a0 + regs.a1, PAGE_SIZE);
	paddr = begin;
	size = end - begin;

	if (size < 2 * PAGE_SIZE) {
		pr_err("too small shared memory area\n");
		return ERR_PTR(-EINVAL);
	}

	va = memremap(paddr, size, MEMREMAP_WB);
	if (!va) {
		pr_err("shared memory ioremap failed\n");
		return ERR_PTR(-EINVAL);
	}
	vaddr = (unsigned long)va;

	priv_mgr = tee_shm_pool_mgr_alloc_res_mem(vaddr, paddr, PAGE_SIZE, 3);
	if (IS_ERR(priv_mgr)) {
		pool = ERR_CAST(priv_mgr);
		goto err_memunmap;
	}

	dmabuf_mgr = tee_shm_pool_mgr_alloc_res_mem(vaddr + PAGE_SIZE,
						    paddr + PAGE_SIZE,
						    size - PAGE_SIZE,
						    PAGE_SHIFT);
	if (IS_ERR(dmabuf_mgr)) {
		pool = ERR_CAST(dmabuf_mgr);
		goto err_free_priv_mgr;
	}

	pool = tee_shm_pool_alloc(priv_mgr, dmabuf_mgr);
	if (IS_ERR(pool))
		goto err_free_dmabuf_mgr;

	*memremaped_shm = va;
	return pool;

err_free_dmabuf_mgr:
	tee_shm_pool_mgr_destroy(dmabuf_mgr);
err_free_priv_mgr:
	tee_shm_pool_mgr_destroy(priv_mgr);
err_memunmap:
	memunmap(va);
	return pool;
}

static int __init mitee_core_init(void)
{
	struct device_node *np;
	struct tee_device *teedev;
	struct tee_shm_pool *pool;
	struct mitee *mitee;
	void *pool_va;
	int rc;

	pr_info("initializing driver\n");

	np = of_find_compatible_node(NULL, NULL, "xiaomi,mitee");
	if (!np) {
		pr_err("no such dev: %s\n", "xiaomi,mitee");
		return -ENODEV;
	}

	mitee = kzalloc(sizeof(*mitee), GFP_KERNEL);
	if (!mitee) {
		of_node_put(np);
		return -ENOMEM;
	}
	of_node_put(np);

	optee_wait_queue_init(&mitee->wait_queue);
	mitee_rpc_callback_queue_init(&mitee->callbacks);
	optee_supp_init(&mitee->supp);
	ATOMIC_INIT_NOTIFIER_HEAD(&mitee->call_notifier);
	sema_init(&mitee->call_sem, 1);
	init_waitqueue_head(&mitee->worker_wq);
	mitee_task_list_init(&mitee->tasks);
	mitee_svc = mitee;

	rc = tee_core_init();
	if (rc)
		goto err_uninit;

	rc = mitee_msgq_driver_init();
	if (rc) {
		pr_err("comm protocol init failed\n");
		goto err_tee_core_exit;
	}

	pool = mitee_shm_pool_alloc(&pool_va);
	if (IS_ERR(pool)) {
		rc = PTR_ERR(pool);
		goto err_msgq_exit;
	}
	mitee->pool = pool;
	mitee->pool_va = pool_va;

	teedev = tee_device_alloc(&mitee_clnt_desc, NULL, pool, mitee);
	if (IS_ERR(teedev)) {
		rc = PTR_ERR(teedev);
		goto err_free_pool;
	}
	mitee->teedev = teedev;

	teedev = tee_device_alloc(&mitee_supp_desc, NULL, pool, mitee);
	if (IS_ERR(teedev)) {
		rc = PTR_ERR(teedev);
		goto err_unreg_teedev;
	}
	mitee->supp_teedev = teedev;

	rc = tee_device_register(mitee->teedev);
	if (rc)
		goto err_unreg_supp_teedev;

	rc = tee_device_register(mitee->supp_teedev);
	if (rc)
		goto err_unreg_supp_teedev;

	rc = mitee_memlog_init(mitee);
	if (rc) {
		pr_err("failed to initial mitee_memlog driver (%d)\n", rc);
		goto err_unreg_supp_teedev;
	}

	rc = mitee_msg_queue_init(&mitee->queue);
	if (rc) {
		pr_err("failed to init mitee msg queue (%d)\n", rc);
		goto err_memlog_exit;
	}

	rc = mitee_msg_queue_register(mitee);
	if (rc) {
		pr_err("failed to register mitee msg queue (%d)\n", rc);
		goto err_msg_queue_exit;
	}

	mitee->worker = kthread_run(mitee_worker, mitee, "mitee_worker/%d", 0);
	if (IS_ERR(mitee->worker)) {
		rc = PTR_ERR(mitee->worker);
		pr_err("tee_worker thread creation failed\n");
		goto err_msg_queue_exit;
	}
	set_user_nice(mitee->worker, MIN_NICE);

	pr_info("initialized driver\n");
	return 0;

err_msg_queue_exit:
	mitee_msg_queue_exit(&mitee->queue);
err_memlog_exit:
	mitee_memlog_exit(mitee);
err_unreg_supp_teedev:
	tee_device_unregister(mitee->supp_teedev);
err_unreg_teedev:
	tee_device_unregister(mitee->teedev);
err_free_pool:
	if (pool)
		tee_shm_pool_free(pool);
	if (pool_va)
		memunmap(pool_va);
err_msgq_exit:
	mitee_msgq_driver_exit();
err_tee_core_exit:
	tee_core_exit();
err_uninit:
	mitee_svc = NULL;
	mitee_task_list_exit(&mitee->tasks);
	optee_supp_uninit(&mitee->supp);
	mitee_rpc_callback_queue_exit(&mitee->callbacks);
	optee_wait_queue_exit(&mitee->wait_queue);
	kfree(mitee);
	return rc;
}
module_init(mitee_core_init);

static void __exit mitee_core_exit(void)
{
	struct mitee *mitee = mitee_svc;

	/*
	 * The two devices have to be unregistered before we can free the
	 * other resources.
	 */
	tee_device_unregister(mitee->supp_teedev);
	tee_device_unregister(mitee->teedev);

	kthread_stop(mitee->worker);
	mitee_msgq_driver_exit();
	mitee_msg_queue_exit(&mitee->queue);
	mitee_memlog_exit(mitee);
	mitee_svc = NULL;

	mitee_task_list_exit(&mitee->tasks);
	optee_supp_uninit(&mitee->supp);
	mitee_rpc_callback_queue_exit(&mitee->callbacks);
	optee_wait_queue_exit(&mitee->wait_queue);
	if (mitee->pool)
		tee_shm_pool_free(mitee->pool);
	if (mitee->pool_va)
		memunmap(mitee->pool_va);
	tee_core_exit();
	kfree(mitee);
}
module_exit(mitee_core_exit);

MODULE_AUTHOR("Linaro");
MODULE_DESCRIPTION("OP-TEE driver for the Xiaomi MiTEE virtual machine");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL v2");
