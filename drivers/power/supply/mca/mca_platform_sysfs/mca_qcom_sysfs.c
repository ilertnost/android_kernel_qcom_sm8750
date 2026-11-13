// SPDX-License-Identifier: GPL-2.0-only
/*
 * mca_qcom_sysfs.c
 *
 * Charging attributes under /sys/class/qcom-battery, read and written by
 * Xiaomi's userspace (adapter type, wireless state, reverse charging, ...).
 * Every attribute is a thin wrapper around a call into the charging stack.
 *
 * Copyright (c) 2023-2023 Xiaomi Technologies Co., Ltd.
 */

#define MCA_LOG_TAG "mca_qcom_sysfs"

#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <mca/common/mca_log.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/platform/platform_cp_class.h>
#include <mca/platform/platform_wireless_class.h>
#include <mca/protocol/protocol_class.h>
#include <mca/protocol/protocol_pd_class.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include <mca/strategy/strategy_wireless_class.h>
#include "../mca_strategy/strategy_charger/inc/mca_charger_thermal.h"
#include "../mca_strategy/strategy_wireless/inc/mca_wireless_revchg.h"

/**
 * struct mca_qcom_sysfs - driver state
 * @dev:                 this device
 * @mca_qcom_class:      the "qcom-battery" class the attributes belong to
 * @support_multi_typec: "mi,support_multi_typec" is present in the DT
 */
struct mca_qcom_sysfs {
	struct device *dev;
	struct class mca_qcom_class;
	bool support_multi_typec;
};

/* Indexed by the BC1.2 adapter type (XM_CHARGER_TYPE_*). */
static const char * const power_supply_usb_type_text[] = {
	"Unknown", "SDP", "CDP", "DCP", "USB_FLOAT", "HVDCP", "HVDCP_3",
	"HVDCP_3_B", "HVDCP_3P5", "C", "PD", "PD_PPS", "PD_PPS", "Unknown",
	"ACA", "DCP",
};

/* Indexed by the wireless strategy's pad type. */
static const char * const power_supply_wls_type_text[] = {
	"Unknown", "BPP", "EPP", "HPP",
};

static const char *usb_type_name(u32 type)
{
	if (type < ARRAY_SIZE(power_supply_usb_type_text))
		return power_supply_usb_type_text[type];

	return "Unknown";
}

static const char *wls_type_name(u32 type)
{
	if (type < ARRAY_SIZE(power_supply_wls_type_text))
		return power_supply_wls_type_text[type];

	return "Unknown";
}

static ssize_t real_type_show(const struct class *c,
			      const struct class_attribute *attr, char *buf)
{
	u32 type = 0;
	int ret;

	ret = protocol_class_get_adapter_type(ADAPTER_PROTOCOL_BC12, &type);
	if (ret < 0)
		return ret;

	mca_log_info("real type = %s\n", usb_type_name(type));

	return snprintf(buf, PAGE_SIZE, "%s\n", usb_type_name(type));
}
static CLASS_ATTR_RO(real_type);

static ssize_t wireless_type_show(const struct class *c,
				  const struct class_attribute *attr,
				  char *buf)
{
	int type = 0;
	int ret;

	ret = strategy_class_wireless_ops_get_wls_type(&type);
	if (ret < 0)
		return ret;

	mca_log_info("wireless type = %s\n", wls_type_name(type));

	return snprintf(buf, PAGE_SIZE, "%s\n", wls_type_name(type));
}
static CLASS_ATTR_RO(wireless_type);

static ssize_t authentic_show(const struct class *c,
			      const struct class_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 1);
}

static ssize_t authentic_store(const struct class *c,
			       const struct class_attribute *attr,
			       const char *buf, size_t count)
{
	return count;
}
static CLASS_ATTR_RW(authentic);

static ssize_t slave_authentic_show(const struct class *c,
				    const struct class_attribute *attr,
				    char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 1);
}

static ssize_t slave_authentic_store(const struct class *c,
				     const struct class_attribute *attr,
				     const char *buf, size_t count)
{
	return count;
}
static CLASS_ATTR_RW(slave_authentic);

static ssize_t pd_verifed_show(const struct class *c,
			       const struct class_attribute *attr, char *buf)
{
	int verified = 0;
	int ret;

	ret = protocol_class_get_adapter_verified(ADAPTER_PROTOCOL_PD,
						  &verified);
	if (ret < 0)
		return ret;

	mca_log_info("show pd_verifed = %d\n", verified);

	return snprintf(buf, PAGE_SIZE, "%d\n", verified);
}

static ssize_t pd_verifed_store(const struct class *c,
				const struct class_attribute *attr,
				const char *buf, size_t count)
{
	int val = 0;
	int ret;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	mca_log_info("store pd_verifed = %d\n", val);
	ret = protocol_class_set_adapter_verified(ADAPTER_PROTOCOL_PD, val);
	if (ret < 0)
		return ret;

	return count;
}
static CLASS_ATTR_RW(pd_verifed);

/*
 * The quick-charge strategy answers first, the buck strategy if it has no
 * type, and the wireless strategy overrides both while a pad is online.
 */
static ssize_t quick_charge_type_show(const struct class *c,
				      const struct class_attribute *attr,
				      char *buf)
{
	int online = 0;
	int type = 0;

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
				     STRATEGY_STATUS_TYPE_QC_TYPE, &type);
	if (!type)
		mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
					     STRATEGY_STATUS_TYPE_QC_TYPE,
					     &type);

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
				     STRATEGY_STATUS_TYPE_ONLINE, &online);
	if (online)
		mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					     STRATEGY_STATUS_TYPE_QC_TYPE,
					     &type);

	return snprintf(buf, PAGE_SIZE, "%d\n", type);
}
static CLASS_ATTR_RO(quick_charge_type);

static ssize_t power_max_show(const struct class *c,
			      const struct class_attribute *attr, char *buf)
{
	int online = 0;
	int power = 0;

	mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
				     STRATEGY_STATUS_TYPE_ONLINE, &online);
	if (online)
		mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BASIC_WIRELESS,
					     STRATEGY_STATUS_TYPE_POWER_MAX,
					     &power);

	return snprintf(buf, PAGE_SIZE, "%d\n", power);
}
static CLASS_ATTR_RO(power_max);

static ssize_t soc_decimal_show(const struct class *c,
				const struct class_attribute *attr, char *buf)
{
	int soc_decimal = 0;
	int rate = 0;

	strategy_class_fg_ops_get_soc_decimal(&soc_decimal, &rate);

	return snprintf(buf, PAGE_SIZE, "%d\n", soc_decimal);
}
static CLASS_ATTR_RO(soc_decimal);

static ssize_t soc_decimal_rate_show(const struct class *c,
				     const struct class_attribute *attr,
				     char *buf)
{
	int soc_decimal = 0;
	int rate = 0;

	strategy_class_fg_ops_get_soc_decimal(&soc_decimal, &rate);

	return snprintf(buf, PAGE_SIZE, "%d\n", rate);
}
static CLASS_ATTR_RO(soc_decimal_rate);

static ssize_t usb_real_type_show(const struct class *c,
				  const struct class_attribute *attr,
				  char *buf)
{
	u32 type = 0;
	int ret;

	ret = protocol_class_get_adapter_type(ADAPTER_PROTOCOL_BC12, &type);
	if (ret < 0)
		return ret;

	mca_log_info("usb real type = %s\n", usb_type_name(type));

	return snprintf(buf, PAGE_SIZE, "%s\n", usb_type_name(type));
}
static CLASS_ATTR_RO(usb_real_type);

static ssize_t otg_ui_support_show(const struct class *c,
				   const struct class_attribute *attr,
				   char *buf)
{
	bool support = false;

	platform_class_buckchg_ops_is_support_cid(MAIN_BUCK_CHARGER, &support);

	return snprintf(buf, PAGE_SIZE, "%d\n", support);
}
static CLASS_ATTR_RO(otg_ui_support);

static ssize_t cid_status_show(const struct class *c,
			       const struct class_attribute *attr, char *buf)
{
	bool status = false;
	int ret;

	ret = protocol_class_pd_get_cid_status(TYPEC_PORT_0, &status);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", status);
}
static CLASS_ATTR_RO(cid_status);

static ssize_t cc_toggle_show(const struct class *c,
			      const struct class_attribute *attr, char *buf)
{
	bool toggle = false;
	int ret;

	ret = protocol_class_pd_get_cc_toggle(TYPEC_PORT_0, &toggle);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", toggle);
}

static ssize_t cc_toggle_store(const struct class *c,
			       const struct class_attribute *attr,
			       const char *buf, size_t count)
{
	int val = 0;
	int ret;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	ret = protocol_class_pd_set_cc_toggle(TYPEC_PORT_0, val != 0);
	if (ret < 0)
		return ret;

	return count;
}
static CLASS_ATTR_RW(cc_toggle);

static ssize_t has_dp_show(const struct class *c,
			   const struct class_attribute *attr, char *buf)
{
	bool has_dp = false;
	int ret;

	ret = protocol_class_pd_get_has_dp(TYPEC_PORT_0, &has_dp);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", has_dp);
}
static CLASS_ATTR_RO(has_dp);

static ssize_t dam_ovpgate_show(const struct class *c,
				const struct class_attribute *attr, char *buf)
{
	bool status = false;
	int ret;

	ret = platform_class_cp_get_ovpgate_status(CP_ROLE_MASTER, &status);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", status);
}

static ssize_t dam_ovpgate_store(const struct class *c,
				 const struct class_attribute *attr,
				 const char *buf, size_t count)
{
	bool val = false;
	int ret;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	ret = platform_class_cp_enable_ovpgate(CP_ROLE_MASTER, val);
	if (ret < 0)
		return ret;

	return count;
}
static CLASS_ATTR_RW(dam_ovpgate);

static ssize_t pmic_ibat_show(const struct class *c,
			      const struct class_attribute *attr, char *buf)
{
	int ibat = 0;
	int ret;

	ret = platform_class_buckchg_ops_get_pack_ibat(MAIN_BUCK_CHARGER, &ibat);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", ibat);
}
static CLASS_ATTR_RO(pmic_ibat);

static ssize_t wireless_chip_fw_show(const struct class *c,
				     const struct class_attribute *attr,
				     char *buf)
{
	char fw_version[10] = { 0 };
	int ret;

	ret = platform_class_wireless_get_fw_version(WIRELESS_ROLE_MASTER,
						     fw_version);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%s\n", fw_version);
}

static ssize_t wireless_chip_fw_store(const struct class *c,
				      const struct class_attribute *attr,
				      const char *buf, size_t count)
{
	int val = 0;
	int ret;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	ret = mca_wireless_rev_update_fw_version(val);
	if (ret < 0)
		return ret;

	return count;
}
static CLASS_ATTR_RW(wireless_chip_fw);

static ssize_t reverse_chg_mode_show(const struct class *c,
				     const struct class_attribute *attr,
				     char *buf)
{
	bool enabled = false;
	int ret;

	ret = mca_wireless_rev_get_reverse_chg(&enabled);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", enabled);
}

/*
 * The enable result is not checked: the user's request is recorded either
 * way so that reverse charging can resume once the condition that refused
 * it has cleared.
 */
static ssize_t reverse_chg_mode_store(const struct class *c,
				      const struct class_attribute *attr,
				      const char *buf, size_t count)
{
	int val = 0;
	int ret;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	mca_wireless_rev_enable_reverse_charge(val != 0);
	mca_log_err("zxy store reverse_chg_mode = %d\n", val);
	ret = mca_wireless_rev_set_user_reverse_chg(val != 0);
	if (ret < 0)
		return ret;

	return count;
}
static CLASS_ATTR_RW(reverse_chg_mode);

static ssize_t reverse_chg_state_show(const struct class *c,
				      const struct class_attribute *attr,
				      char *buf)
{
	int state = 0;
	int ret;

	ret = mca_wireless_rev_get_reverse_chg_state(&state);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", state);
}
static CLASS_ATTR_RO(reverse_chg_state);

static ssize_t magnetic_case_flag_show(const struct class *c,
				       const struct class_attribute *attr,
				       char *buf)
{
	bool status = false;
	int ret;

	ret = platform_class_wireless_get_hall_gpio_status(WIRELESS_ROLE_MASTER,
							   &status);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", status);
}
static CLASS_ATTR_RO(magnetic_case_flag);

static ssize_t tx_adapter_show(const struct class *c,
			       const struct class_attribute *attr, char *buf)
{
	int val = 0;
	int ret;

	ret = platform_class_wireless_get_tx_adapter(WIRELESS_ROLE_MASTER, &val);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}
static CLASS_ATTR_RO(tx_adapter);

static ssize_t rx_vout_show(const struct class *c,
			    const struct class_attribute *attr, char *buf)
{
	int val = 0;
	int ret;

	ret = platform_class_wireless_get_vout(WIRELESS_ROLE_MASTER, &val);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}
static CLASS_ATTR_RO(rx_vout);

static ssize_t rx_vrect_show(const struct class *c,
			     const struct class_attribute *attr, char *buf)
{
	int val = 0;
	int ret;

	ret = platform_class_wireless_get_vrect(WIRELESS_ROLE_MASTER, &val);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}
static CLASS_ATTR_RO(rx_vrect);

static ssize_t rx_iout_show(const struct class *c,
			    const struct class_attribute *attr, char *buf)
{
	int val = 0;
	int ret;

	ret = platform_class_wireless_get_iout(WIRELESS_ROLE_MASTER, &val);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}
static CLASS_ATTR_RO(rx_iout);

static ssize_t rx_ss_show(const struct class *c,
			  const struct class_attribute *attr, char *buf)
{
	int val = 0;
	int ret;

	ret = platform_class_wireless_get_ss_voltage(WIRELESS_ROLE_MASTER, &val);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}
static CLASS_ATTR_RO(rx_ss);

static ssize_t shipmode_count_reset_show(const struct class *c,
					 const struct class_attribute *attr,
					 char *buf)
{
	bool ship_mode = false;
	int ret;

	ret = platform_class_buckchg_ops_get_ship_mode(MAIN_BUCK_CHARGER,
						       &ship_mode);
	if (ret < 0)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%u\n", ship_mode);
}

static ssize_t shipmode_count_reset_store(const struct class *c,
					  const struct class_attribute *attr,
					  const char *buf, size_t count)
{
	int val = 0;
	int ret;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	ret = platform_class_buckchg_ops_set_ship_mode(MAIN_BUCK_CHARGER,
						       val != 0);
	if (ret < 0)
		return ret;

	return count;
}
static CLASS_ATTR_RW(shipmode_count_reset);

static ssize_t wls_thermal_remove_show(const struct class *c,
				       const struct class_attribute *attr,
				       char *buf)
{
	bool remove = false;
	int ret;

	ret = mca_get_wls_charger_thermal_remove(&remove);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "%d\n", remove);
}

static ssize_t wls_thermal_remove_store(const struct class *c,
					const struct class_attribute *attr,
					const char *buf, size_t count)
{
	int val = 0;
	int ret;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	ret = mca_set_wls_charger_thermal_remove(val != 0);
	mca_log_err("zxy store wls_thermal_remove = %d\n", val);
	if (ret < 0)
		return ret;

	return count;
}
static CLASS_ATTR_RW(wls_thermal_remove);

static ssize_t hall_phone_case_show(const struct class *c,
				    const struct class_attribute *attr,
				    char *buf)
{
	int category = 0;

	platform_class_wireless_get_phone_case_category(WIRELESS_ROLE_MASTER,
							&category);

	return snprintf(buf, PAGE_SIZE, "%d\n", category);
}

static ssize_t hall_phone_case_store(const struct class *c,
				     const struct class_attribute *attr,
				     const char *buf, size_t count)
{
	int val = 0;
	int ret;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	ret = platform_class_wireless_set_phone_case_category(WIRELESS_ROLE_MASTER,
							      val);
	if (ret < 0)
		return ret;

	mca_log_err("store hall_phone_case = %d\n", val);

	return count;
}
static CLASS_ATTR_RW(hall_phone_case);

static ssize_t soh_show(const struct class *c,
			const struct class_attribute *attr, char *buf)
{
	int soh = 0;

	strategy_class_fg_get_soh(&soh);

	return snprintf(buf, PAGE_SIZE, "%d\n", soh);
}
static CLASS_ATTR_RO(soh);

static struct attribute *mca_qcom_sysfs_attrs[] = {
	&class_attr_real_type.attr,
	&class_attr_wireless_type.attr,
	&class_attr_authentic.attr,
	&class_attr_slave_authentic.attr,
	&class_attr_pd_verifed.attr,
	&class_attr_quick_charge_type.attr,
	&class_attr_power_max.attr,
	&class_attr_soc_decimal.attr,
	&class_attr_soc_decimal_rate.attr,
	&class_attr_usb_real_type.attr,
	&class_attr_otg_ui_support.attr,
	&class_attr_cid_status.attr,
	&class_attr_cc_toggle.attr,
	&class_attr_has_dp.attr,
	&class_attr_dam_ovpgate.attr,
	&class_attr_pmic_ibat.attr,
	&class_attr_wireless_chip_fw.attr,
	&class_attr_reverse_chg_mode.attr,
	&class_attr_reverse_chg_state.attr,
	&class_attr_magnetic_case_flag.attr,
	&class_attr_tx_adapter.attr,
	&class_attr_rx_vout.attr,
	&class_attr_rx_vrect.attr,
	&class_attr_rx_iout.attr,
	&class_attr_rx_ss.attr,
	&class_attr_shipmode_count_reset.attr,
	&class_attr_wls_thermal_remove.attr,
	&class_attr_hall_phone_case.attr,
	&class_attr_soh.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mca_qcom_sysfs);

static int mca_qcom_sysfs_probe(struct platform_device *pdev)
{
	struct mca_qcom_sysfs *chip;
	int ret;

	mca_log_info("probe begin\n");

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	chip->support_multi_typec = of_property_present(pdev->dev.of_node,
							"mi,support_multi_typec");
	chip->mca_qcom_class.name = "qcom-battery";
	chip->mca_qcom_class.class_groups = mca_qcom_sysfs_groups;
	platform_set_drvdata(pdev, chip);

	ret = class_register(&chip->mca_qcom_class);
	if (ret < 0) {
		mca_log_err("class reg failed %d\n", ret);
		return ret;
	}

	mca_log_err("probe ok\n");

	return 0;
}

static int mca_qcom_sysfs_remove(struct platform_device *pdev)
{
	struct mca_qcom_sysfs *chip = platform_get_drvdata(pdev);

	class_unregister(&chip->mca_qcom_class);

	return 0;
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,qcom_sysfs" },
	{ }
};

static struct platform_driver mca_qcom_sysfs_driver = {
	.driver = {
		.name = "mca_qcom_sysfs",
		.of_match_table = match_table,
	},
	.probe = mca_qcom_sysfs_probe,
	.remove = mca_qcom_sysfs_remove,
};
module_platform_driver(mca_qcom_sysfs_driver);

MODULE_DESCRIPTION("mca qcom sysfs");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
