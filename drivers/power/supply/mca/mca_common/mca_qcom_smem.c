// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Battery state the bootloader leaves behind.  See
 * include/mca/common/mca_smem.h.
 */

#define MCA_LOG_TAG "mca_smem"

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/soc/qcom/smem.h>
#include <linux/types.h>

#include <mca/common/mca_log.h>
#include <mca/common/mca_smem.h>

/* The item UEFI writes the battery state into, and how large it is. */
#define MCA_SMEM_BATTERY_ITEM	81
#define MCA_SMEM_BATTERY_SIZE	40

/* Where each value sits within it. */
#define MCA_SMEM_START_MODE	0x20	/* u32 */
#define MCA_SMEM_VERIFY		0x24	/* u8, see the bits below */
#define MCA_SMEM_CELL_SN	0x25	/* u8 */

/* Bits of the byte at MCA_SMEM_VERIFY. */
#define MCA_SMEM_VERIFY_CHIP_OK	BIT(4)
#define MCA_SMEM_VERIFIED	BIT(5)

/*
 * The item is normally created by UEFI, so allocating it here is only for the
 * case where it was not; finding it already there is the expected outcome.
 *
 * Return: the item, or NULL.
 */
static void *mca_smem_battery_item(void)
{
	void *item;
	int ret;

	ret = qcom_smem_alloc(QCOM_SMEM_HOST_ANY, MCA_SMEM_BATTERY_ITEM,
			      MCA_SMEM_BATTERY_SIZE);
	if (ret < 0 && ret != -EEXIST) {
		mca_log_err("unable to allocate shared state entry\n");
		return NULL;
	}

	item = qcom_smem_get(QCOM_SMEM_HOST_ANY, MCA_SMEM_BATTERY_ITEM, NULL);
	if (IS_ERR_OR_NULL(item)) {
		mca_log_err("Unable to acquire shared state entry\n");
		return NULL;
	}

	return item;
}

/*
 * The readers below leave their output untouched when the item cannot be
 * read and still return 0: the callers start from a default and keep it.
 */
int get_smem_battery_info(u32 *mode)
{
	void *item;

	if (!mode)
		return -EINVAL;

	item = mca_smem_battery_item();
	if (!item)
		return 0;

	*mode = *(u32 *)(item + MCA_SMEM_START_MODE);
	mca_log_err("zero_speed_start_mode: %d\n", *mode);

	return 0;
}
EXPORT_SYMBOL(get_smem_battery_info);

int get_smem_battery_verify_result(u8 *verified, u8 *chip_ok)
{
	void *item;
	u8 val;

	if (!verified || !chip_ok)
		return -EINVAL;

	item = mca_smem_battery_item();
	if (!item)
		return 0;

	val = *(u8 *)(item + MCA_SMEM_VERIFY);
	*verified = !!(val & MCA_SMEM_VERIFIED);
	*chip_ok = !!(val & MCA_SMEM_VERIFY_CHIP_OK);
	mca_log_err("uefi_batt_verfitied: %d, uefi_batt_verfitied_chip_ok: %d\n",
		    *verified, *chip_ok);

	return 0;
}
EXPORT_SYMBOL(get_smem_battery_verify_result);

int get_smem_battery_cell_sn(u32 *cell_sn)
{
	void *item;

	if (!cell_sn)
		return -EINVAL;

	item = mca_smem_battery_item();
	if (!item)
		return 0;

	*cell_sn = *(u8 *)(item + MCA_SMEM_CELL_SN);
	mca_log_err("uefi_battery_cell_sn: %d\n", *cell_sn);

	return 0;
}
EXPORT_SYMBOL(get_smem_battery_cell_sn);

MODULE_DESCRIPTION("mca get smem info");
MODULE_AUTHOR("liuliang10@xiaomi.com");
MODULE_LICENSE("GPL v2");
