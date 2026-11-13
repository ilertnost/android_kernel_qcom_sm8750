// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPI IR LED character device
 *
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 * Copyright (C) 2026 The LineageOS Project
 *
 * Xiaomi drives the IR LED from a SPI MOSI line and lets userspace render
 * the carrier itself: the consumer IR HAL sets the length of a burst with
 * LIRC_SET_SEND_MODE on /dev/ir_spi and writes the burst as a bitmap that is
 * shifted out at 1.92 MHz, 32 bits per word.
 */

#include <linux/fs.h>
#include <linux/lirc.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>

#define IR_SPI_DRIVER_NAME		"ir-spi"
#define IR_SPI_DEFAULT_FREQUENCY	1920000
#define IR_SPI_BITS_PER_WORD		32
/* Large enough for the longest consumer IR pattern */
#define IR_SPI_DEFAULT_BUFFER_SIZE	150000

struct ir_spi_data {
	struct miscdevice misc;
	struct spi_device *spi;
	struct spi_transfer xfer;
	struct mutex lock;
	unsigned int users;
	u8 *buffer;
	size_t buffer_size;
};

static int ir_spi_reserve_buffer(struct ir_spi_data *idata, size_t length)
{
	u8 *buffer;

	if (length <= idata->buffer_size)
		return 0;

	buffer = kmalloc(length, GFP_KERNEL | GFP_DMA);
	if (!buffer)
		return -ENOMEM;

	kfree(idata->buffer);
	idata->buffer = buffer;
	idata->buffer_size = length;

	return 0;
}

static ssize_t ir_spi_write(struct file *file, const char __user *buf,
			    size_t length, loff_t *offset)
{
	struct ir_spi_data *idata = container_of(file->private_data,
						 struct ir_spi_data, misc);
	bool burst_length_set;
	int ret;

	mutex_lock(&idata->lock);

	if (idata->xfer.len && idata->xfer.len != length) {
		ret = -EINVAL;
		goto out;
	}

	ret = ir_spi_reserve_buffer(idata, length);
	if (ret)
		goto out;

	if (copy_from_user(idata->buffer, buf, length)) {
		ret = -EFAULT;
		goto out;
	}

	/* Without a burst length set, the write itself defines it */
	burst_length_set = idata->xfer.len;
	idata->xfer.tx_buf = idata->buffer;
	idata->xfer.len = length;
	ret = spi_sync_transfer(idata->spi, &idata->xfer, 1);
	if (ret)
		dev_err(&idata->spi->dev, "unable to deliver the signal\n");
	if (!burst_length_set)
		idata->xfer.len = 0;

out:
	mutex_unlock(&idata->lock);

	return ret ? ret : length;
}

static long ir_spi_ioctl(struct file *file, unsigned int cmd,
			 unsigned long arg)
{
	struct ir_spi_data *idata = container_of(file->private_data,
						 struct ir_spi_data, misc);
	u32 length;
	int ret;

	if (cmd != LIRC_SET_SEND_MODE)
		return -EINVAL;

	if (get_user(length, (u32 __user *)arg))
		return -EFAULT;

	mutex_lock(&idata->lock);

	if (idata->xfer.len == length) {
		ret = 0;
	} else if (idata->users > 1) {
		/* All users have to agree on the burst length */
		ret = -EPERM;
	} else {
		ret = ir_spi_reserve_buffer(idata, length);
		if (!ret)
			idata->xfer.len = length;
	}

	mutex_unlock(&idata->lock);

	return ret;
}

static int ir_spi_open(struct inode *inode, struct file *file)
{
	struct ir_spi_data *idata = container_of(file->private_data,
						 struct ir_spi_data, misc);

	mutex_lock(&idata->lock);
	idata->users++;
	mutex_unlock(&idata->lock);

	return 0;
}

static int ir_spi_release(struct inode *inode, struct file *file)
{
	struct ir_spi_data *idata = container_of(file->private_data,
						 struct ir_spi_data, misc);

	mutex_lock(&idata->lock);
	/* The burst length is forgotten when the last user goes away */
	if (!--idata->users) {
		idata->xfer.len = 0;
		idata->xfer.speed_hz = IR_SPI_DEFAULT_FREQUENCY;
	}
	mutex_unlock(&idata->lock);

	return 0;
}

static const struct file_operations ir_spi_fops = {
	.owner		= THIS_MODULE,
	.open		= ir_spi_open,
	.release	= ir_spi_release,
	.write		= ir_spi_write,
	.unlocked_ioctl	= ir_spi_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static int ir_spi_probe(struct spi_device *spi)
{
	struct ir_spi_data *idata;
	int ret;

	idata = devm_kzalloc(&spi->dev, sizeof(*idata), GFP_KERNEL);
	if (!idata)
		return -ENOMEM;

	mutex_init(&idata->lock);
	idata->spi = spi;
	idata->xfer.bits_per_word = IR_SPI_BITS_PER_WORD;
	idata->xfer.speed_hz = IR_SPI_DEFAULT_FREQUENCY;

	/* Allocate the burst buffer up front, it needs physically contiguous memory */
	ret = ir_spi_reserve_buffer(idata, IR_SPI_DEFAULT_BUFFER_SIZE);
	if (ret)
		return ret;

	idata->misc.minor = MISC_DYNAMIC_MINOR;
	idata->misc.name = "ir_spi";
	idata->misc.fops = &ir_spi_fops;
	idata->misc.parent = &spi->dev;

	ret = misc_register(&idata->misc);
	if (ret) {
		kfree(idata->buffer);
		return ret;
	}

	spi_set_drvdata(spi, idata);

	return 0;
}

static void ir_spi_remove(struct spi_device *spi)
{
	struct ir_spi_data *idata = spi_get_drvdata(spi);

	misc_deregister(&idata->misc);
	kfree(idata->buffer);
}

static const struct of_device_id ir_spi_of_match[] = {
	{ .compatible = "ir-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, ir_spi_of_match);

static struct spi_driver ir_spi_driver = {
	.probe = ir_spi_probe,
	.remove = ir_spi_remove,
	.driver = {
		.name = IR_SPI_DRIVER_NAME,
		.of_match_table = ir_spi_of_match,
	},
};
module_spi_driver(ir_spi_driver);

MODULE_DESCRIPTION("SPI IR LED");
MODULE_LICENSE("GPL v2");
