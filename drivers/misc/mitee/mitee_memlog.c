// SPDX-License-Identifier: GPL-2.0-only
/*
 * MiTEE log.
 *
 * OP-TEE writes its log into two rings at the end of the static shared
 * memory: one holding the boot log and one for everything after it. The log
 * is exported line by line through /proc/mitee_log, read by miteelog. The
 * TEE also leaves a key for the log in the header, readable through
 * /proc/mitee_log_key.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": %s: " fmt, __func__

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/notifier.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <soc/qcom/minidump.h>

#include "mitee_msgq.h"
#include "mitee_private.h"

#define MITEE_LOG_KEY_OFFSET	0x171
#define MITEE_LOG_KEY_SIZE	0x159
#define MITEE_LOG_DATA_OFFSET	0x3cb
#define MITEE_LOG_LINE_MAX	255

struct mitee_log_ring_hdr {
	u32 write_pos;	/* bytes reserved by writers */
	u32 commit_pos;	/* bytes completely written */
	u32 size;	/* ring size, a power of 2 */
};

/* Header at the start of the log memory, written by the TEE */
struct mitee_log_hdr {
	struct mitee_log_ring_hdr main;
	struct mitee_log_ring_hdr boot;
};

/**
 * struct mitee_memlog - log reader state
 * @hdr:		log memory
 * @pa:			physical address of the log memory
 * @size:		size of the log memory
 * @log_entry:		/proc/mitee_log
 * @key_entry:		/proc/mitee_log_key
 * @main_pos:		read position in the main ring
 * @boot_pos:		read position in the boot ring
 * @boot_done:		the boot ring has been read completely
 * @wq:			readers waiting for new log
 * @calls:		number of returns from the TEE, the log may have
 *			changed on each of them
 * @calls_seen:		value of @calls at the last read
 * @readable:		no read is in progress
 * @call_nb:		notifier called on each return from the TEE
 * @line:		line being copied
 */
struct mitee_memlog {
	struct mitee_log_hdr *hdr;
	phys_addr_t pa;
	size_t size;
	struct proc_dir_entry *log_entry;
	struct proc_dir_entry *key_entry;
	u32 main_pos;
	u32 boot_pos;
	bool boot_done;
	wait_queue_head_t wq;
	atomic_t calls;
	int calls_seen;
	atomic_t readable;
	struct notifier_block call_nb;
	struct md_region md;
	char line[MITEE_LOG_LINE_MAX + 1];
};

static struct mitee_memlog *mitee_memlog;

static int mitee_memlog_call_notify(struct notifier_block *nb,
				    unsigned long action, void *data)
{
	struct mitee_memlog *s = container_of(nb, struct mitee_memlog,
					      call_nb);

	if (action != 1)
		return NOTIFY_DONE;

	atomic_inc(&s->calls);
	wake_up_interruptible(&s->wq);
	return NOTIFY_OK;
}

static int mitee_memlog_open(struct inode *inode, struct file *file)
{
	struct mitee_memlog *s = pde_data(inode);
	int rc;

	rc = nonseekable_open(inode, file);
	if (!rc)
		s->calls_seen = atomic_read(&s->calls);

	return rc;
}

/*
 * Copy whole lines from @ring, starting at *@pos, until @count bytes are
 * used or the ring is empty.
 */
static ssize_t mitee_memlog_read_ring(struct mitee_memlog *s,
				      const struct mitee_log_ring_hdr *ring,
				      const u8 *data, u32 *pos,
				      char __user *buf, size_t count)
{
	u32 rpos = *pos, end, size, avail, len, i;
	size_t copied = 0, max;
	char *kbuf;
	char c;

	size = READ_ONCE(ring->size);
	if (!is_power_of_2(size))
		return -EIO;

	end = READ_ONCE(ring->commit_pos);
	/* Read the data only after seeing the position covering it */
	rmb();
	if (READ_ONCE(ring->write_pos) - rpos > size) {
		pr_notice("mitee: log overflow, lose some msg.");
		rpos = READ_ONCE(ring->write_pos) - size;
	}

	if (rpos > end)
		return -EFAULT;

	avail = end - rpos;
	max = min_t(size_t, count, avail);
	kbuf = kmalloc(round_down(max, 256) + 256, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	while (rpos != end) {
		len = min_t(u32, end - rpos, MITEE_LOG_LINE_MAX);
		i = 0;
		do {
			c = data[(rpos + i) & (size - 1)];
			s->line[i++] = c;
		} while (i < len && c != '\n');
		s->line[i] = '\0';

		if (copied + i > max)
			break;

		memcpy(kbuf + copied, s->line, i);
		copied += i;
		rpos += i;
	}

	if (copy_to_user(buf, kbuf, copied)) {
		kfree(kbuf);
		return -EFAULT;
	}

	kfree(kbuf);
	*pos = rpos;
	return copied;
}

static ssize_t mitee_memlog_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct mitee_memlog *s = pde_data(file_inode(file));
	struct mitee_log_hdr *hdr = s->hdr;
	u8 *data = (u8 *)hdr + MITEE_LOG_DATA_OFFSET;
	ssize_t rc;

	/* One reader at a time, concurrent readers get nothing */
	if (!atomic_xchg(&s->readable, 0))
		return 0;

	if (!s->boot_done) {
		if (s->boot_pos == READ_ONCE(hdr->boot.commit_pos)) {
			/* Boot log done, the next read continues with the rest */
			s->boot_done = true;
			rc = 0;
		} else {
			rc = mitee_memlog_read_ring(s, &hdr->boot,
						    data + READ_ONCE(hdr->main.size),
						    &s->boot_pos, buf, count);
		}
	} else if (s->main_pos == READ_ONCE(hdr->main.commit_pos)) {
		rc = 0;
	} else {
		rc = mitee_memlog_read_ring(s, &hdr->main, data, &s->main_pos,
					    buf, count);
	}

	s->calls_seen = atomic_read(&s->calls);
	atomic_set(&s->readable, 1);
	return rc;
}

static __poll_t mitee_memlog_poll(struct file *file, poll_table *wait)
{
	struct mitee_memlog *s = pde_data(file_inode(file));

	if (s->main_pos != READ_ONCE(s->hdr->main.commit_pos))
		return EPOLLIN | EPOLLRDNORM;

	poll_wait(file, &s->wq, wait);

	if (s->calls_seen != atomic_read(&s->calls))
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}

static int mitee_memlog_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct proc_ops mitee_memlog_fops = {
	.proc_open = mitee_memlog_open,
	.proc_read = mitee_memlog_read,
	.proc_release = mitee_memlog_release,
	.proc_poll = mitee_memlog_poll,
};

static int mitee_memlog_key_open(struct inode *inode, struct file *file)
{
	return nonseekable_open(inode, file);
}

/* The key never changes, each read returns all of it */
static ssize_t mitee_memlog_key_read(struct file *file, char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct mitee_memlog *s = pde_data(file_inode(file));
	size_t len = min_t(size_t, count, MITEE_LOG_KEY_SIZE);
	char *kbuf;

	kbuf = kmalloc(MITEE_LOG_KEY_SIZE, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	memcpy(kbuf, (u8 *)s->hdr + MITEE_LOG_KEY_OFFSET, MITEE_LOG_KEY_SIZE);
	if (copy_to_user(buf, kbuf, len)) {
		kfree(kbuf);
		pr_err("error miteelog mitee_memlog_key_read\n");
		return -EFAULT;
	}

	kfree(kbuf);
	return len;
}

static int mitee_memlog_key_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct proc_ops mitee_memlog_key_fops = {
	.proc_open = mitee_memlog_key_open,
	.proc_read = mitee_memlog_key_read,
	.proc_release = mitee_memlog_key_release,
};

int mitee_memlog_init(struct mitee *mitee)
{
	struct mitee_regs regs = { .a0 = MITEE_CALL_GET_LOG_BUF };
	struct mitee_memlog *s;
	phys_addr_t begin, end;
	int rc;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	rc = mitee_msgq_call(&regs);
	if (rc) {
		pr_err("miteelog service not available %d\n", rc);
		rc = -EIO;
		goto err_free;
	}

	begin = roundup(regs.a0, PAGE_SIZE);
	end = rounddown(regs.a0 + regs.a1, PAGE_SIZE);
	s->pa = begin;
	s->size = end - begin;
	pr_err("miteelog paddr=%pa, size=%zu\n", &s->pa, s->size);

	s->hdr = memremap(s->pa, s->size, MEMREMAP_WB);
	if (!s->hdr) {
		pr_err("miteelog ioremap failed\n");
		rc = -EFAULT;
		goto err_free;
	}
	memset(s->hdr, 0, s->size);

	init_waitqueue_head(&s->wq);
	atomic_set(&s->readable, 1);

	s->call_nb.notifier_call = mitee_memlog_call_notify;
	rc = atomic_notifier_chain_register(&mitee->call_notifier,
					    &s->call_nb);
	if (rc) {
		pr_err("failed to register mitee call notifier\n");
		goto err_unmap;
	}

	s->log_entry = proc_create_data("mitee_log", 0444, NULL,
					&mitee_memlog_fops, s);
	if (!s->log_entry) {
		pr_info("mitee_log proc_create failed!\n");
		rc = -ENOMEM;
		goto err_unregister;
	}

	s->key_entry = proc_create_data("mitee_log_key", 0444, NULL,
					&mitee_memlog_key_fops, s);
	if (!s->key_entry) {
		pr_info("mitee_log proc_key_create failed!\n");
		rc = -ENOMEM;
		goto err_remove_log;
	}

	/* Keep the TEE log in RAM dumps */
	strscpy(s->md.name, "MITEELOG", sizeof(s->md.name));
	s->md.virt_addr = (uintptr_t)s->hdr;
	s->md.phys_addr = s->pa;
	s->md.size = s->size;
	if (msm_minidump_add_region(&s->md) < 0)
		pr_err("%s: Failed to add %s data in Minidump, add region failed!\n",
		       "add_log_memory_to_dump_region", "MITEELOG");

	mitee_memlog = s;
	return 0;

err_remove_log:
	proc_remove(s->log_entry);
err_unregister:
	atomic_notifier_chain_unregister(&mitee->call_notifier, &s->call_nb);
err_unmap:
	memunmap(s->hdr);
err_free:
	kfree(s);
	return rc;
}

void mitee_memlog_exit(struct mitee *mitee)
{
	struct mitee_memlog *s = mitee_memlog;

	if (!s)
		return;

	msm_minidump_remove_region(&s->md);
	proc_remove(s->key_entry);
	proc_remove(s->log_entry);
	atomic_notifier_chain_unregister(&mitee->call_notifier, &s->call_nb);
	memunmap(s->hdr);
	kfree(s);
	mitee_memlog = NULL;
}
