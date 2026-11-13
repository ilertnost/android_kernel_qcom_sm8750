// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Chinese Xiaomi phones carry a GPIO that tells the WLAN stack which SAR
 * (transmit power) limits apply. Its level is published over the "wifi_sar"
 * generic netlink family: as a reply to SAR_CMD_WPAS_READY once the
 * supplicant is up, and on the "sar_event" multicast group whenever the
 * debounced level changes.
 */

#include <linux/gpio.h>
#include <linux/hwid.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/pm_wakeup.h>
#include <linux/workqueue.h>
#include <net/genetlink.h>

#define SAR_GENL_NAME			"wifi_sar"
#define SAR_DEFAULT_DEBOUNCE_MS		50

enum {
	SAR_ATTR_UNSPEC,
	SAR_ATTR_STATUS,
	__SAR_ATTR_AFTER_LAST,
	SAR_ATTR_MAX = __SAR_ATTR_AFTER_LAST - 1
};

enum {
	SAR_CMD_UNSPEC,
	SAR_CMD_WPAS_READY,
	SAR_CMD_GPIO_STATUS,
};

enum {
	SAR_MCGRP_STATUS,
};

struct wifi_gpio_data {
	struct device *dev;
	int gpio;
	int irq;
	u32 debounce_ms;
	struct delayed_work debounce_work;
};

/* There is a single SAR GPIO, the netlink handler needs to find it */
static struct wifi_gpio_data *wifi_gpio;

static struct genl_family sar_family;

static int sar_send_status(u32 status)
{
	struct sk_buff *msg;
	void *hdr;
	int ret;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_put(msg, 0, 0, &sar_family, 0, SAR_CMD_GPIO_STATUS);
	if (!hdr) {
		nlmsg_free(msg);
		return -EMSGSIZE;
	}

	if (nla_put_u32(msg, SAR_ATTR_STATUS, status)) {
		genlmsg_cancel(msg, hdr);
		nlmsg_free(msg);
		return -EMSGSIZE;
	}

	genlmsg_end(msg, hdr);

	ret = genlmsg_multicast(&sar_family, msg, 0, SAR_MCGRP_STATUS,
				GFP_KERNEL);
	/* Nobody listening yet is fine */
	if (ret && ret != -ESRCH) {
		pr_err("sar send status failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int sar_wpas_ready(struct sk_buff *skb, struct genl_info *info)
{
	if (!wifi_gpio)
		return -ENODEV;

	return sar_send_status(gpio_get_value(wifi_gpio->gpio));
}

static const struct nla_policy sar_policy[SAR_ATTR_MAX + 1] = {
	[SAR_ATTR_STATUS] = { .type = NLA_U32 },
};

static const struct genl_small_ops sar_ops[] = {
	{
		.cmd = SAR_CMD_WPAS_READY,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = sar_wpas_ready,
		.flags = GENL_ADMIN_PERM,
	},
};

static const struct genl_multicast_group sar_mcgrps[] = {
	[SAR_MCGRP_STATUS] = { .name = "sar_event" },
};

static struct genl_family sar_family __ro_after_init = {
	.name = SAR_GENL_NAME,
	.version = 1,
	.maxattr = SAR_ATTR_MAX,
	.policy = sar_policy,
	.module = THIS_MODULE,
	.small_ops = sar_ops,
	.n_small_ops = ARRAY_SIZE(sar_ops),
	.resv_start_op = SAR_CMD_GPIO_STATUS + 1,
	.mcgrps = sar_mcgrps,
	.n_mcgrps = ARRAY_SIZE(sar_mcgrps),
};

static void wifi_gpio_debounce_work(struct work_struct *work)
{
	struct wifi_gpio_data *data = container_of(to_delayed_work(work),
						   struct wifi_gpio_data,
						   debounce_work);
	int status = gpio_get_value(data->gpio);

	sar_send_status(status);
	dev_info(data->dev, "gpio status: %d\n", status);
	pm_relax(data->dev);
}

static irqreturn_t wifi_gpio_irq(int irq, void *dev_id)
{
	struct wifi_gpio_data *data = dev_id;

	/* Stay awake until the debounced level has been published */
	pm_stay_awake(data->dev);
	mod_delayed_work(system_wq, &data->debounce_work,
			 msecs_to_jiffies(data->debounce_ms));

	return IRQ_HANDLED;
}

static void wifi_gpio_cleanup(void *ptr)
{
	struct wifi_gpio_data *data = ptr;

	wifi_gpio = NULL;
	dev_pm_clear_wake_irq(data->dev);
	cancel_delayed_work_sync(&data->debounce_work);
	pm_relax(data->dev);
}

static int wifi_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wifi_gpio_data *data;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	data->debounce_ms = SAR_DEFAULT_DEBOUNCE_MS;
	of_property_read_u32(dev->of_node, "debounce-time", &data->debounce_ms);
	INIT_DELAYED_WORK(&data->debounce_work, wifi_gpio_debounce_work);

	data->gpio = of_get_named_gpio(dev->of_node, "gpio", 0);
	if (data->gpio < 0)
		return dev_err_probe(dev, data->gpio, "failed to get gpio\n");

	ret = devm_gpio_request_one(dev, data->gpio, GPIOF_IN, "gpio");
	if (ret)
		return dev_err_probe(dev, ret, "failed to request gpio\n");

	data->irq = gpio_to_irq(data->gpio);
	if (data->irq < 0)
		return data->irq;

	device_init_wakeup(dev, true);

	ret = devm_add_action_or_reset(dev, wifi_gpio_cleanup, data);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, data->irq, NULL, wifi_gpio_irq,
					IRQF_ONESHOT | IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING,
					"xiaomi_wifi_gpio", data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	ret = dev_pm_set_wake_irq(dev, data->irq);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set wake irq\n");

	wifi_gpio = data;

	return 0;
}

static const struct of_device_id wifi_gpio_of_match[] = {
	{ .compatible = "xiaomi,xiaomi-wifi" },
	{ }
};
MODULE_DEVICE_TABLE(of, wifi_gpio_of_match);

static struct platform_driver wifi_gpio_driver = {
	.driver = {
		.name = "xiaomi_wifi_gpio",
		.of_match_table = wifi_gpio_of_match,
	},
	.probe = wifi_gpio_probe,
};

static int __init wifi_gpio_init(void)
{
	int ret;

	/* The SAR GPIO is only fitted on boards sold in China */
	if (get_hw_country_version() != CountryCN) {
		pr_info("%s not cn devices, not init\n", __func__);
		return 0;
	}

	ret = genl_register_family(&sar_family);
	if (ret)
		return ret;

	ret = platform_driver_register(&wifi_gpio_driver);
	if (ret)
		genl_unregister_family(&sar_family);

	return ret;
}
module_init(wifi_gpio_init);

static void __exit wifi_gpio_exit(void)
{
	if (get_hw_country_version() != CountryCN)
		return;

	platform_driver_unregister(&wifi_gpio_driver);
	genl_unregister_family(&sar_family);
}
module_exit(wifi_gpio_exit);

MODULE_DESCRIPTION("Wifi Gpio Testing Driver for Xiaomi Corporation");
MODULE_LICENSE("GPL v2");
