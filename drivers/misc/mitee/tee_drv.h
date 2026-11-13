/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015-2016, Linaro Limited
 *
 * Private copy of the TEE subsystem driver interface used by MiTEE. The
 * generic TEE subsystem is not part of the GKI kernel, so MiTEE carries its
 * own instance of it, derived from Linux 5.16 drivers/tee. Nothing here is
 * exported to other modules.
 */

#ifndef MITEE_TEE_DRV_H
#define MITEE_TEE_DRV_H

#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/tee.h>
#include <linux/types.h>
#include <linux/uuid.h>

/*
 * The file describes the API provided by the generic TEE driver to the
 * specific TEE driver.
 */

#define TEE_SHM_MAPPED		BIT(0)	/* Memory mapped by the kernel */
#define TEE_SHM_DMA_BUF		BIT(1)	/* Memory with dma-buf handle */
#define TEE_SHM_EXT_DMA_BUF	BIT(2)	/* Memory with dma-buf handle */
#define TEE_SHM_REGISTER	BIT(3)	/* Memory registered in secure world */
#define TEE_SHM_USER_MAPPED	BIT(4)	/* Memory mapped in user space */
#define TEE_SHM_POOL		BIT(5)	/* Memory allocated from pool */
#define TEE_SHM_KERNEL_MAPPED	BIT(6)	/* Memory mapped in kernel space */
#define TEE_SHM_PRIV		BIT(7)	/* Memory private to TEE driver */
#define TEE_SHM_VMALLOC		BIT(8)	/* Kernel buffer owned by the shm */

struct tee_device;
struct tee_shm;
struct tee_shm_pool;

/**
 * struct tee_context - driver specific context on file pointer data
 * @teedev:	pointer to this drivers struct tee_device
 * @data:	driver specific context data, managed by the driver
 * @refcount:	reference counter for this structure
 * @releasing:	flag that indicates if context is being released right now.
 *		It is needed to break circular dependency on context during
 *		shared memory release.
 * @supp_nowait: flag that indicates that requests in this context should not
 *		wait for tee-supplicant daemon to be started if not present
 *		and just return with an error code.
 * @cap_memref_null: flag indicating if the TEE Client support shared
 *		memory buffer with a NULL pointer.
 */
struct tee_context {
	struct tee_device *teedev;
	void *data;
	struct kref refcount;
	bool releasing;
	bool supp_nowait;
	bool cap_memref_null;
};

struct tee_param_memref {
	size_t shm_offs;
	size_t size;
	struct tee_shm *shm;
};

struct tee_param_value {
	u64 a;
	u64 b;
	u64 c;
};

struct tee_param {
	u64 attr;
	union {
		struct tee_param_memref memref;
		struct tee_param_value value;
	} u;
};

/**
 * struct tee_driver_ops - driver operations vtable
 * @get_version:	returns version of driver
 * @open:		called when the device file is opened
 * @release:		release this open file
 * @open_session:	open a new session
 * @close_session:	close a session
 * @invoke_func:	invoke a trusted function
 * @cancel_req:		request cancel of an ongoing invoke or open
 * @supp_recv:		called for supplicant to get a command
 * @supp_send:		called for supplicant to send a response
 * @shm_register:	register shared memory buffer in TEE
 * @shm_unregister:	unregister shared memory buffer in TEE
 */
struct tee_driver_ops {
	void (*get_version)(struct tee_device *teedev,
			    struct tee_ioctl_version_data *vers);
	int (*open)(struct tee_context *ctx);
	void (*release)(struct tee_context *ctx);
	int (*open_session)(struct tee_context *ctx,
			    struct tee_ioctl_open_session_arg *arg,
			    struct tee_param *param);
	int (*close_session)(struct tee_context *ctx, u32 session);
	int (*invoke_func)(struct tee_context *ctx,
			   struct tee_ioctl_invoke_arg *arg,
			   struct tee_param *param);
	int (*cancel_req)(struct tee_context *ctx, u32 cancel_id, u32 session);
	int (*supp_recv)(struct tee_context *ctx, u32 *func, u32 *num_params,
			 struct tee_param *param);
	int (*supp_send)(struct tee_context *ctx, u32 ret, u32 num_params,
			 struct tee_param *param);
	int (*shm_register)(struct tee_context *ctx, struct tee_shm *shm,
			    struct page **pages, size_t num_pages,
			    unsigned long start);
	int (*shm_unregister)(struct tee_context *ctx, struct tee_shm *shm);
};

/**
 * struct tee_desc - Describes the TEE driver to the subsystem
 * @name:	name of driver
 * @ops:	driver operations vtable
 * @owner:	module providing the driver
 * @flags:	Extra properties of driver, defined by TEE_DESC_* below
 */
#define TEE_DESC_PRIVILEGED	0x1
struct tee_desc {
	const char *name;
	const struct tee_driver_ops *ops;
	struct module *owner;
	u32 flags;
};

/**
 * struct tee_shm - shared memory object
 * @ctx:	context using the object
 * @paddr:	physical address of the shared memory
 * @kaddr:	virtual address of the shared memory
 * @size:	size of shared memory
 * @offset:	offset of buffer in user space
 * @pages:	locked pages from userspace
 * @num_pages:	number of locked pages
 * @refcount:	reference counter
 * @flags:	defined by TEE_SHM_* in tee_drv.h
 * @id:		unique id of a shared memory object on this device, shared
 *		with user space
 * @sec_world_id:
 *		secure world assigned id of this shared memory object, not
 *		used by all drivers
 */
struct tee_shm {
	struct tee_context *ctx;
	phys_addr_t paddr;
	void *kaddr;
	size_t size;
	unsigned int offset;
	struct page **pages;
	size_t num_pages;
	refcount_t refcount;
	u32 flags;
	int id;
	u64 sec_world_id;
};

/**
 * struct tee_shm_pool_mgr - shared memory manager
 * @ops:		operations
 * @private_data:	private data for the shared memory manager
 */
struct tee_shm_pool_mgr {
	const struct tee_shm_pool_mgr_ops *ops;
	void *private_data;
};

/**
 * struct tee_shm_pool_mgr_ops - shared memory pool manager operations
 * @alloc:		called when allocating shared memory
 * @free:		called when freeing shared memory
 * @destroy_poolmgr:	called when destroying the pool manager
 */
struct tee_shm_pool_mgr_ops {
	int (*alloc)(struct tee_shm_pool_mgr *poolmgr, struct tee_shm *shm,
		     size_t size);
	void (*free)(struct tee_shm_pool_mgr *poolmgr, struct tee_shm *shm);
	void (*destroy_poolmgr)(struct tee_shm_pool_mgr *poolmgr);
};

/**
 * struct tee_shm_pool - shared memory pool
 * @private_mgr:	pool manager for shared memory only between kernel
 *			and secure world
 * @dma_buf_mgr:	pool manager for shared memory exported to user space
 */
struct tee_shm_pool {
	struct tee_shm_pool_mgr *private_mgr;
	struct tee_shm_pool_mgr *dma_buf_mgr;
};

#define TEE_DEVICE_FLAG_REGISTERED	0x1
#define TEE_MAX_DEV_NAME_LEN		32

/**
 * struct tee_device - TEE Device representation
 * @name:	name of device
 * @desc:	description of device
 * @id:		unique id of device
 * @flags:	represented by TEE_DEVICE_FLAG_REGISTERED above
 * @dev:	embedded basic device structure
 * @cdev:	embedded cdev
 * @num_users:	number of active users of this device
 * @c_no_users:	completion used when unregistering the device
 * @mutex:	mutex protecting @num_users and @idr
 * @idr:	register of user space shared memory objects allocated or
 *		registered on this device
 * @pool:	shared memory pool
 */
struct tee_device {
	char name[TEE_MAX_DEV_NAME_LEN];
	const struct tee_desc *desc;
	int id;
	unsigned int flags;

	struct device dev;
	struct cdev cdev;

	size_t num_users;
	struct completion c_no_users;
	struct mutex mutex;	/* protects num_users and idr */

	struct idr idr;
	struct tee_shm_pool *pool;
};

/* tee_core.c */
int tee_core_init(void);
void tee_core_exit(void);

struct tee_device *tee_device_alloc(const struct tee_desc *teedesc,
				    struct device *dev,
				    struct tee_shm_pool *pool,
				    void *driver_data);
int tee_device_register(struct tee_device *teedev);
void tee_device_unregister(struct tee_device *teedev);
void *tee_get_drvdata(struct tee_device *teedev);

bool tee_device_get(struct tee_device *teedev);
void tee_device_put(struct tee_device *teedev);
void teedev_ctx_get(struct tee_context *ctx);
void teedev_ctx_put(struct tee_context *ctx);

int tee_session_calc_client_uuid(uuid_t *uuid, u32 connection_method,
				 const u8 connection_data[TEE_IOCTL_UUID_LEN]);

/* tee_shm_pool.c */
struct tee_shm_pool *tee_shm_pool_alloc(struct tee_shm_pool_mgr *priv_mgr,
					struct tee_shm_pool_mgr *dmabuf_mgr);
struct tee_shm_pool_mgr *tee_shm_pool_mgr_alloc_res_mem(unsigned long vaddr,
							phys_addr_t paddr,
							size_t size,
							int min_alloc_order);
void tee_shm_pool_free(struct tee_shm_pool *pool);

static inline void tee_shm_pool_mgr_destroy(struct tee_shm_pool_mgr *poolm)
{
	poolm->ops->destroy_poolmgr(poolm);
}

/* tee_shm.c */
struct tee_shm *tee_shm_alloc(struct tee_context *ctx, size_t size, u32 flags);
struct tee_shm *tee_shm_register(struct tee_context *ctx, unsigned long addr,
				 size_t length, u32 flags);
int tee_shm_get_fd(struct tee_shm *shm);
void tee_shm_free(struct tee_shm *shm);
void tee_shm_put(struct tee_shm *shm);
int tee_shm_va2pa(struct tee_shm *shm, void *va, phys_addr_t *pa);
int tee_shm_pa2va(struct tee_shm *shm, phys_addr_t pa, void **va);
void *tee_shm_get_va(struct tee_shm *shm, size_t offs);
int tee_shm_get_pa(struct tee_shm *shm, size_t offs, phys_addr_t *pa);
struct tee_shm *tee_shm_get_from_id(struct tee_context *ctx, int id);

static inline bool tee_shm_is_registered(struct tee_shm *shm)
{
	return shm && (shm->flags & TEE_SHM_REGISTER);
}

static inline bool tee_param_is_memref(struct tee_param *param)
{
	switch (param->attr & TEE_IOCTL_PARAM_ATTR_TYPE_MASK) {
	case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT:
	case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT:
	case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT:
		return true;
	default:
		return false;
	}
}

#endif /* MITEE_TEE_DRV_H */
