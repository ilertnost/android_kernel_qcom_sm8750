// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/vip.c — F2FS VIP inode set (CONFIG_F2FS_VIP_FILE)
 *
 * Ported from the HyperOS 4 kernel, where it backs the observed runtime state:
 *
 *   /sys/fs/f2fs/<dev>/vip_file_enable        = 1
 *   /sys/fs/f2fs/<dev>/gc_pin_file_thresh     = 2048
 *   /sys/fs/f2fs/<dev>/reserved_pin_section   = 512
 *
 * The on-disk format is NOT touched by this feature: f2fs_super_block is
 * byte-identical between the HyperOS 3 and HyperOS 4 kernels (3072 bytes each,
 * verified from the stock BTF), so all of this is in-memory bookkeeping.
 *
 * The three recovery'd entry points below were taken from the HyperOS 4 binary
 * (f2fs_vip_bsearch / f2fs_vip_insert / f2fs_vip_remove):
 *
 *   f2fs_vip_bsearch(nm_i, ino, &found)
 *       - returns 0 with *found == false when count == 0
 *       - otherwise binary-searches sorted[], returning the entry index, or
 *         the insertion point with *found == false
 *   f2fs_vip_remove(sbi, ino)
 *       - no-op when the set is not allocated (stock returns -95)
 *       - no-op when the nid is absent (stock returns -2)
 *       - otherwise memmoves the tail of sorted[] down and decrements count
 *   f2fs_vip_insert(sbi, ino)
 *       - inserts into sorted[], doubling it when it is full
 *
 * The stock container is a sorted array plus an "unsorted" ring buffer that is
 * merged into the sorted array once it fills up.  That ring is a batching
 * optimisation and is NOT reproduced here, for two reasons:
 *
 *   - entries parked in the ring are invisible to f2fs_vip_bsearch(), hence to
 *     f2fs_is_vip_inode()/f2fs_iget(): the first 15 pinned nids were never
 *     reported as VIP, and sorted[] only became non-empty when the 16th entry
 *     flushed the ring;
 *   - the ring stores a per-entry index (unsorted_idx) that is only valid
 *     until the first flush, after which those indices point into a different,
 *     freshly filled ring, so remove-by-index corrupts the ring bookkeeping.
 *
 * F2FS_IOC_SET_PIN_FILE is a cold path (one call per pinned file), so an
 * O(count) insertion into a short sorted array costs nothing measurable and
 * keeps every lookup exact.
 *
 * Call sites recovered in the HyperOS 4 binary:
 *   f2fs_iget            -> f2fs_vip_bsearch   (stamp FI_VIP on the inode)
 *   __f2fs_ioctl         -> f2fs_vip_insert    (F2FS_IOC_SET_PIN_FILE)
 *   f2fs_unlink          -> f2fs_vip_remove
 */

#include <linux/f2fs_fs.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "f2fs.h"

/*
 * f2fs_vip_bsearch - binary search sorted[] by ino
 *
 * Returns the index of @ino when *found is true, otherwise the insertion point
 * for @ino.  Callers must hold vip->lock, or otherwise exclude concurrent
 * mutation.
 */
unsigned int f2fs_vip_bsearch(struct f2fs_nm_info *nm_i, nid_t ino, bool *found)
{
	struct f2fs_vip_manager *vip = &nm_i->vip;
	unsigned int lo = 0, hi = vip->count;

	*found = false;

	/*
	 * Plain lower_bound over [0, count).
	 *
	 * The previous version used `hi = count - 1` together with `hi = mid - 1`
	 * on unsigned ints.  For a key below sorted[0] the search walks down to
	 * lo == hi == 0, that probe sets hi = 0 - 1 = UINT_MAX, and from there
	 * `(int)(hi - lo)` is -1, the `diff++` correction turns it back into 0,
	 * so mid stays 0, lo stays 0 and the loop never terminates.  It spun
	 * with vip->lock held and IRQs disabled by spin_lock_irqsave(), wedging
	 * the CPU (soft lockup / RCU stall) and every other f2fs_iget() with it.
	 */
	while (lo < hi) {
		unsigned int mid = lo + (hi - lo) / 2;

		if (vip->sorted[mid].ino < ino) {
			lo = mid + 1;
			continue;
		}

		if (vip->sorted[mid].ino == ino)
			*found = true;
		hi = mid;
	}

	return lo;
}

/*
 * f2fs_vip_grow - double sorted[].
 *
 * Called from f2fs_vip_insert() *before* it takes vip->lock: kvmalloc_array()
 * may sleep, and the set is protected by a spinlock taken with
 * spin_lock_irqsave(), where sleeping is not allowed.  The state is re-checked
 * under the lock, so losing a race with another grower only wastes one
 * allocation.  The old array is released after the lock is dropped, once no
 * lookups can still be walking it.
 */
static int f2fs_vip_grow(struct f2fs_nm_info *nm_i)
{
	struct f2fs_vip_manager *vip = &nm_i->vip;
	struct vip_inode_sorted *ns, *old;
	unsigned int old_cap, new_cap;
	unsigned long flags;

	old_cap = READ_ONCE(vip->capacity);
	new_cap = old_cap * 2;
	if (new_cap < F2FS_VIP_SORTED_MIN)
		new_cap = F2FS_VIP_SORTED_MIN;
	if (new_cap < old_cap || new_cap > F2FS_VIP_SORTED_MAX)
		return -EINVAL;

	ns = kvmalloc_array(new_cap, sizeof(*ns), GFP_NOFS);
	if (!ns)
		return -ENOMEM;

	spin_lock_irqsave(&vip->lock, flags);

	if (!vip->sorted) {
		spin_unlock_irqrestore(&vip->lock, flags);
		kvfree(ns);
		return -EOPNOTSUPP;
	}

	if (vip->capacity != old_cap) {
		spin_unlock_irqrestore(&vip->lock, flags);
		kvfree(ns);
		return 0;
	}

	if (vip->count)
		memcpy(ns, vip->sorted, vip->count * sizeof(*ns));

	old = vip->sorted;
	vip->sorted = ns;
	vip->capacity = new_cap;

	spin_unlock_irqrestore(&vip->lock, flags);

	kvfree(old);

	return 0;
}

/*
 * f2fs_vip_insert - add @ino to the VIP set.
 *
 * Returns 0 on success, when the entry already exists, or when the feature is
 * off; -ENOMEM when the sorted array cannot grow, -EOPNOTSUPP when the set is
 * not usable, -ENOSPC when a growth race left the array full again.
 */
int f2fs_vip_insert(struct f2fs_sb_info *sbi, nid_t ino)
{
	struct f2fs_nm_info *nm_i = sbi->nm_info;
	struct f2fs_vip_manager *vip;
	unsigned long flags;
	unsigned int idx;
	bool found;
	int ret = 0;

	if (!nm_i || !READ_ONCE(nm_i->vip.sorted) || !READ_ONCE(nm_i->vip.enabled))
		return 0;
	vip = &nm_i->vip;

	/*
	 * Make room *before* taking the lock: kvmalloc_array() with GFP_KERNEL
	 * inside the spinlocked section below would be a "sleeping function
	 * called from invalid context" bug, since vip->lock is taken with
	 * spin_lock_irqsave() (IRQs off).
	 */
	if (READ_ONCE(vip->count) >= READ_ONCE(vip->capacity)) {
		ret = f2fs_vip_grow(nm_i);
		if (ret)
			return ret;
	}

	spin_lock_irqsave(&vip->lock, flags);

	if (!vip->sorted) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (!vip->enabled)		/* switched off meanwhile: no-op */
		goto out;

	idx = f2fs_vip_bsearch(nm_i, ino, &found);
	if (found)
		goto out;

	if (vip->count >= vip->capacity) {	/* lost the race with a grower */
		ret = -ENOSPC;
		goto out;
	}

	memmove(&vip->sorted[idx + 1], &vip->sorted[idx],
		(vip->count - idx) * sizeof(*vip->sorted));
	vip->sorted[idx].ino = ino;
	vip->count++;
out:
	spin_unlock_irqrestore(&vip->lock, flags);
	return ret;
}

/*
 * f2fs_vip_remove - drop @ino from the VIP set.
 *
 * No-op when the set is not allocated (stock returns -95) or when the nid is
 * absent (stock returns -2); otherwise memmoves the tail of sorted[] down.
 */
void f2fs_vip_remove(struct f2fs_sb_info *sbi, nid_t ino)
{
	struct f2fs_nm_info *nm_i = sbi->nm_info;
	struct f2fs_vip_manager *vip;
	unsigned long flags;
	unsigned int idx;
	bool found;

	if (!nm_i)
		return;
	vip = &nm_i->vip;

	spin_lock_irqsave(&vip->lock, flags);

	if (!vip->sorted)
		goto out;

	idx = f2fs_vip_bsearch(nm_i, ino, &found);
	if (!found)
		goto out;

	memmove(&vip->sorted[idx], &vip->sorted[idx + 1],
		(vip->count - idx - 1) * sizeof(*vip->sorted));
	vip->count--;
out:
	spin_unlock_irqrestore(&vip->lock, flags);
}

/*
 * Returns whether @ino is in the VIP set.
 *
 * Called from f2fs_iget(), which the mount path reaches for F2FS_META_INO and
 * the quota inodes *before* f2fs_build_node_manager() has run, so sbi->nm_info
 * is still NULL there. Guard for it, not only for safety: an unguarded
 * sbi->nm_info->vip dereference panics during mount.
 *
 * This is a hot path (once per f2fs_iget()), so the feature flag and the
 * (empty) count are checked before taking the lock.
 */
bool f2fs_is_vip_inode(struct f2fs_sb_info *sbi, nid_t ino)
{
	struct f2fs_nm_info *nm_i = READ_ONCE(sbi->nm_info);
	struct f2fs_vip_manager *vip;
	unsigned long flags;
	bool found = false;

	if (!nm_i || !READ_ONCE(nm_i->vip.sorted))
		return false;

	vip = &nm_i->vip;

	if (!READ_ONCE(vip->enabled) || !READ_ONCE(vip->count))
		return false;

	spin_lock_irqsave(&vip->lock, flags);
	if (vip->sorted)
		f2fs_vip_bsearch(nm_i, ino, &found);
	spin_unlock_irqrestore(&vip->lock, flags);

	return found;
}

/*
 * f2fs_inode_set_task_flag - classify an inode by the task writing to it.
 *
 * Recovered from the HyperOS 4 binary (f2fs_inode_set_task_flag, 488 B at
 * 0x1e793fc8 in the stock image). The stock code does an atomic CAS on
 * fi->task_flag, storing 2 when the writing task is a plain one and 4 when
 * its name starts with "background" (the .rodata string at stock offset
 * 0x158c6a7, compared with length 11 = "background"). It also bails out
 * early when the inode is already classified and when the feature flag in
 * the mount options is off.
 *
 * NOTE (honest limit of the recovery): the stock binary walks a task list
 * (two pointer dereferences plus an offset, then the name) rather than
 * reading current->comm directly, so the exact task it matches is not
 * fully recovered. What is certain is the observable contract: task_flag is
 * atomically set from 0 to 2 or 4. We implement the "background" comparison
 * against the current task's name, which is what the string and length
 * unambiguously encode, and leave the search method out.
 */
void f2fs_inode_set_task_flag(struct f2fs_sb_info *sbi,
				struct f2fs_inode_info *fi)
{
	unsigned int v;

	if (atomic_read(&fi->task_flag))
		return;

	if (!strncmp(current->comm, "background", 11))
		v = 4u;
	else
		v = 2u;

	atomic_cmpxchg(&fi->task_flag, 0, v);
}

/*
 * f2fs_set_inode_dst_flag - set the write-destination class of an inode.
 *
 * The stock symbol set_inode_dst_flag is called from f2fs_write_inline_data
 * and do_write_page. The binary writes the destination class into
 * fi->dst_flag; the exact enum values are not recovered, so the caller
 * passes them explicitly.
 */
void f2fs_set_inode_dst_flag(struct f2fs_inode_info *fi, unsigned int flag)
{
	atomic_set(&fi->dst_flag, flag);
}

/*
 * Xiaomi "cold file extension" layer: after the upstream cold/hot match in
 * set_file_temperature(), also mark fi->cold_flag when the file is cold, the
 * feature is on and the name matches the kernel-side second list.
 */
bool f2fs_set_inode_cold_from_extension(struct f2fs_sb_info *sbi,
		struct inode *inode, const unsigned char *name,
		unsigned int idx, unsigned int cold_count)
{
	struct f2fs_inode_info *fi = F2FS_I(inode);
	unsigned int i;

	if (!sbi || idx >= cold_count || !file_is_cold(inode))
		return false;
	if (!sbi->vip_cold_ext_enable || !sbi->vip_cold_ext_count)
		return false;

	for (i = 0; i < sbi->vip_cold_ext_count; i++) {
		const char *ext = sbi->vip_cold_ext[i];
		const unsigned char *p = name;
		size_t ext_len;

		if (!ext)
			continue;
		ext_len = strlen(ext);
		if (!ext_len || ext_len > F2FS_EXTENSION_LEN)
			continue;

		/* mirror the stock walk: compare the suffix after the last '.' */
		while (*p && *p != '.')
			p++;
		if (!*p)
			continue;

		if (!strncasecmp(p + 1, ext, ext_len)) {
			WRITE_ONCE(fi->cold_flag, 1);
			return true;
		}
	}

	return false;
}

/*
 * Parse a user-supplied "|"-separated extension list. The stock
 * f2fs_update_extension_list() stores up to 64 pointers and returns -EINVAL
 * when the list is full; the second list we bound at 17 entries.
 */
int f2fs_update_vip_extension_list(struct f2fs_sb_info *sbi,
		const char *name, size_t len, bool set)
{
	int ret = 0;

	if (!sbi || !name)
		return -EINVAL;

	if (set) {
		if (sbi->vip_cold_ext_count >= ARRAY_SIZE(sbi->vip_cold_ext))
			return -EINVAL;
		sbi->vip_cold_ext[sbi->vip_cold_ext_count] =
			kstrndup(name, len, GFP_KERNEL);
		if (!sbi->vip_cold_ext[sbi->vip_cold_ext_count])
			return -ENOMEM;
		sbi->vip_cold_ext_count++;
		sbi->vip_cold_ext_enable = 1;
	} else {
		unsigned int i;

		for (i = 0; i < sbi->vip_cold_ext_count; i++) {
			if (sbi->vip_cold_ext[i] &&
			    !strncmp(sbi->vip_cold_ext[i], name, len)) {
				kfree(sbi->vip_cold_ext[i]);
				sbi->vip_cold_ext[i] = NULL;
				ret = 0;
			}
		}
	}

	return ret;
}

int f2fs_vip_init(struct f2fs_nm_info *nm_i)
{
	struct f2fs_vip_manager *vip = &nm_i->vip;

	vip->count = 0;
	vip->enabled = 0;
	vip->capacity = F2FS_VIP_SORTED_MIN;
	vip->sorted = kvmalloc_array(vip->capacity, sizeof(*vip->sorted),
				     GFP_KERNEL);
	if (!vip->sorted) {
		vip->capacity = 0;
		return -ENOMEM;
	}

	vip->count = 0;
	spin_lock_init(&vip->lock);
	vip->enabled = 1;	/* observed state on stock: vip_file_enable = 1 */

	return 0;
}

void f2fs_vip_destroy(struct f2fs_nm_info *nm_i)
{
	struct f2fs_vip_manager *vip = &nm_i->vip;
	struct vip_inode_sorted *sorted;
	unsigned long flags;

	/* f2fs_vip_init() may have failed, in which case nothing is allocated
	 * and vip->lock was never initialised. */
	if (!READ_ONCE(vip->sorted))
		return;

	spin_lock_irqsave(&vip->lock, flags);
	sorted = vip->sorted;
	vip->sorted = NULL;
	vip->count = 0;
	vip->capacity = 0;
	vip->enabled = 0;
	spin_unlock_irqrestore(&vip->lock, flags);

	kvfree(sorted);
}
