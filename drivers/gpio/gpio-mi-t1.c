// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Control GPIOs of the Xiaomi T1 RF companion chip. The lines route the
 * antennas between the cellular modem, WLAN and satellite paths; they are
 * driven to their defaults at probe and the telephony stack switches them
 * through /proc/modem_t1_gpio_ctrl:
 *
 *  - reading lists the TLMM pin number and level of every line,
 *  - writing a string of '0' and '1' characters sets the lines in order,
 *    any other character leaves the corresponding line untouched.
 */

#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#define MI_T1_PROC_NAME		"modem_t1_gpio_ctrl"
#define MI_T1_MAX_GPIOS		19

struct mi_t1_gpio {
	struct gpio_desc *desc;
	/* TLMM pin number, as reported to userspace */
	u32 pin;
};

struct mi_t1_data {
	struct device *dev;
	struct mutex lock;
	unsigned int count;
	struct mi_t1_gpio gpios[MI_T1_MAX_GPIOS];
};

/* All lines idle low, except the twelfth one */
static int mi_t1_default_value(unsigned int index)
{
	return index == 11;
}

static int mi_t1_proc_show(struct seq_file *m, void *v)
{
	struct mi_t1_data *data = m->private;
	unsigned int i;

	seq_printf(m, "GPIO cout=%u : \n", data->count);

	mutex_lock(&data->lock);
	for (i = 0; i < data->count; i++)
		seq_printf(m, "GPIO-%u state=%d \n", data->gpios[i].pin,
			   gpiod_get_raw_value_cansleep(data->gpios[i].desc));
	mutex_unlock(&data->lock);

	return 0;
}

static int mi_t1_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, mi_t1_proc_show, pde_data(inode));
}

static ssize_t mi_t1_proc_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct mi_t1_data *data = pde_data(file_inode(file));
	char config[MI_T1_MAX_GPIOS];
	size_t len = min_t(size_t, count, data->count);
	unsigned int i;

	if (!count)
		return -EINVAL;

	if (copy_from_user(config, buf, len))
		return -EFAULT;

	mutex_lock(&data->lock);
	for (i = 0; i < len; i++) {
		if (config[i] != '0' && config[i] != '1')
			continue;

		gpiod_direction_output_raw(data->gpios[i].desc,
					   config[i] == '1');
	}
	mutex_unlock(&data->lock);

	return count;
}

static const struct proc_ops mi_t1_proc_ops = {
	.proc_open	= mi_t1_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= mi_t1_proc_write,
};

static void mi_t1_remove_proc(void *unused)
{
	remove_proc_entry(MI_T1_PROC_NAME, NULL);
}

static int mi_t1_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mi_t1_data *data;
	char name[sizeof("mi-t1-gpio-") + 2];
	u32 first_pin = 0;
	int base = 0;
	unsigned int i;
	int gpio, ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	mutex_init(&data->lock);

	ret = of_property_read_u32(dev->of_node, "gpio-count", &data->count);
	if (ret || !data->count || data->count > MI_T1_MAX_GPIOS)
		return dev_err_probe(dev, -EINVAL, "invalid gpio-count\n");

	/* TLMM pin of the first line, to number the others accordingly */
	of_property_read_u32(dev->of_node, "first-gpio", &first_pin);

	for (i = 0; i < data->count; i++) {
		struct mi_t1_gpio *t1_gpio = &data->gpios[i];

		/* The lines are named mi-t1-gpio-N, without a -gpios suffix */
		snprintf(name, sizeof(name), "mi-t1-gpio-%u", i + 1);
		gpio = of_get_named_gpio(dev->of_node, name, 0);
		if (gpio < 0)
			return dev_err_probe(dev, gpio, "failed to get %s\n",
					     name);

		ret = devm_gpio_request_one(dev, gpio, mi_t1_default_value(i) ?
					    GPIOF_OUT_INIT_HIGH :
					    GPIOF_OUT_INIT_LOW, name);
		if (ret)
			return dev_err_probe(dev, ret, "failed to request %s\n",
					     name);

		t1_gpio->desc = gpio_to_desc(gpio);
		if (!i)
			base = gpio - first_pin;
		t1_gpio->pin = gpio - base;
	}

	if (!proc_create_data(MI_T1_PROC_NAME, 0664, NULL, &mi_t1_proc_ops,
			      data))
		return -ENOMEM;

	return devm_add_action_or_reset(dev, mi_t1_remove_proc, NULL);
}

static const struct of_device_id mi_t1_of_match[] = {
	{ .compatible = "mdm,mi_t1_chip" },
	{ }
};
MODULE_DEVICE_TABLE(of, mi_t1_of_match);

static struct platform_driver mi_t1_driver = {
	.driver = {
		.name = "gpio-mi-t1",
		.of_match_table = mi_t1_of_match,
	},
	.probe = mi_t1_probe,
};
module_platform_driver(mi_t1_driver);

MODULE_DESCRIPTION("Driver for mi_t1 GPIO control");
MODULE_LICENSE("GPL v2");
