// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Reading the charging stack's device tree properties.  See
 * include/mca/common/mca_parse_dts.h.
 */

#define MCA_LOG_TAG "mca_dts"

#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_parse_dts.h>
#include <linux/module.h>
#include <linux/of.h>

int mca_parse_dts_u8(struct device_node *np, const char *prop, u8 *val,
		     u8 def_val)
{
	if (!np || !prop || !val) {
		mca_log_err("np or prop or data is null\n");
		return -EINVAL;
	}

	if (of_property_read_variable_u8_array(np, prop, val, 1, 0) < 0) {
		*val = def_val;
		mca_log_err("prop %s read fail, set default %u\n", prop,
			    def_val);
		return -EINVAL;
	}

	mca_log_debug("prop %s=%u\n", prop, *val);

	return 0;
}
EXPORT_SYMBOL(mca_parse_dts_u8);

int mca_parse_dts_u32(struct device_node *np, const char *prop, u32 *val,
		      u32 def_val)
{
	if (!np || !prop || !val) {
		mca_log_err("np or prop or data is null\n");
		return -EINVAL;
	}

	if (of_property_read_variable_u32_array(np, prop, val, 1, 0) < 0) {
		*val = def_val;
		mca_log_err("prop %s read fail, set default %u\n", prop,
			    def_val);
		return -EINVAL;
	}

	mca_log_debug("prop %s=%u\n", prop, *val);

	return 0;
}
EXPORT_SYMBOL(mca_parse_dts_u32);

int mca_parse_dts_u8_array(struct device_node *np, const char *prop, u8 *val,
			   u16 count)
{
	if (!np || !prop || !val) {
		mca_log_err("np or prop or data is null\n");
		return -EINVAL;
	}

	if (of_property_read_variable_u8_array(np, prop, val, count, 0) < 0) {
		mca_log_err("prop %s read fail, array len %d\n", prop, count);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(mca_parse_dts_u8_array);

int mca_parse_dts_u32_array(struct device_node *np, const char *prop, u32 *val,
			    u32 count)
{
	if (!np || !prop || !val) {
		mca_log_err("np or prop or data is null\n");
		return -EINVAL;
	}

	if (of_property_read_variable_u32_array(np, prop, val, count, 0) < 0) {
		mca_log_err("prop %s read fail, array len %d\n", prop, count);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(mca_parse_dts_u32_array);

/*
 * A table property is @max_rows rows of @row_size values.  A length that is
 * not a whole number of rows, or more rows than the caller can take, is
 * refused.
 */
static bool mca_parse_dts_count_valid(int count, u32 max_rows, u32 row_size)
{
	return count > 0 && (u32)count <= max_rows * row_size &&
	       row_size && !((u32)count % row_size);
}

int mca_parse_dts_u8_count(struct device_node *np, const char *prop,
			   u32 max_rows, u32 row_size)
{
	int count;

	if (!np || !prop) {
		mca_log_err("np or prop is null\n");
		return -EINVAL;
	}

	count = of_property_count_elems_of_size(np, prop, sizeof(u8));
	if (!mca_parse_dts_count_valid(count, max_rows, row_size)) {
		mca_log_err("prop %s length read fail\n", prop);
		return -EINVAL;
	}

	mca_log_debug("prop %s length=%d\n", prop, count);

	return count;
}
EXPORT_SYMBOL(mca_parse_dts_u8_count);

int mca_parse_dts_u32_count(struct device_node *np, const char *prop,
			    u32 max_rows, u32 row_size)
{
	int count;

	if (!np || !prop) {
		mca_log_err("np or prop is null\n");
		return -EINVAL;
	}

	count = of_property_count_elems_of_size(np, prop, sizeof(u32));
	if (!mca_parse_dts_count_valid(count, max_rows, row_size)) {
		mca_log_err("prop %s length read fail\n", prop);
		return -EINVAL;
	}

	mca_log_debug("prop %s length=%d\n", prop, count);

	return count;
}
EXPORT_SYMBOL(mca_parse_dts_u32_count);

int mca_parse_dts_u32_index(struct device_node *np, const char *prop, int index,
			    u32 *val)
{
	if (!np || !prop || !val) {
		mca_log_err("np or prop or data is null\n");
		return -EINVAL;
	}

	if (of_property_read_u32_index(np, prop, index, val)) {
		mca_log_err("prop %s[%d] read fail\n", prop, index);
		return -EINVAL;
	}

	mca_log_debug("prop %s[%d]=%u\n", prop, index, *val);

	return 0;
}
EXPORT_SYMBOL(mca_parse_dts_u32_index);

int mca_parse_dts_string(struct device_node *np, const char *prop,
			 const char **str)
{
	if (!np || !prop || !str) {
		mca_log_err(" np or prop or out is null\n");
		return -EINVAL;
	}

	if (of_property_read_string(np, prop, str)) {
		mca_log_err("prop %s read fail\n", prop);
		return -EINVAL;
	}

	mca_log_debug("prop %s=%s\n", prop, *str);

	return 0;
}
EXPORT_SYMBOL(mca_parse_dts_string);

int mca_parse_dts_string_index(struct device_node *np, const char *prop,
			       int index, const char **str)
{
	if (!np || !prop || !str) {
		mca_log_err("np or prop or out is null\n");
		return -EINVAL;
	}

	if (of_property_read_string_helper(np, prop, str, 1, index) < 0) {
		mca_log_err("prop %s[%d] read fail\n", prop, index);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(mca_parse_dts_string_index);

int mca_parse_dts_count_strings(struct device_node *np, const char *prop,
				u32 max_rows, u32 row_size)
{
	int count;

	if (!np || !prop) {
		mca_log_err("np or prop is null\n");
		return -EINVAL;
	}

	count = of_property_read_string_helper(np, prop, NULL, 0, 0);
	if (!mca_parse_dts_count_valid(count, max_rows, row_size)) {
		mca_log_err("prop %s length read fail\n", prop);
		return -EINVAL;
	}

	mca_log_debug("prop %s length=%d\n", prop, count);

	return count;
}
EXPORT_SYMBOL(mca_parse_dts_count_strings);

/*
 * A table of numbers written as strings, so that each value can be given in
 * whichever base reads clearly.  Each string is converted as it is read.
 *
 * Return: how many values were read, or -EINVAL.
 */
int mca_parse_dts_string_array(struct device_node *np, const char *prop,
			       int *val, u32 max_rows, u32 row_size)
{
	const char *str = NULL;
	int count, i;

	count = mca_parse_dts_count_strings(np, prop, max_rows, row_size);
	if (count < 0)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		if (mca_parse_dts_string_index(np, prop, i, &str))
			return -EINVAL;

		if (kstrtoint(str, 0, &val[i]))
			return -EINVAL;
	}

	return count;
}
EXPORT_SYMBOL(mca_parse_dts_string_array);

MODULE_DESCRIPTION("mca parse dts");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
