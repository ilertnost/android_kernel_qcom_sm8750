/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Reading the charging stack's device tree properties.
 *
 * The charging drivers each read dozens of properties, and a board that
 * leaves one out should not turn into a driver that quietly charges wrong.
 * Every helper here names the property it could not read, and the scalar ones
 * leave the caller's variable holding a stated default rather than whatever it
 * happened to contain.
 *
 * The counting helpers describe a property as a table: so many rows of so many
 * values each.  A property whose length is not a whole number of rows, or that
 * holds more rows than the caller can take, is refused rather than parsed
 * half way.
 */

#ifndef __MCA_PARSE_DTS_H
#define __MCA_PARSE_DTS_H

#include <linux/types.h>

struct device_node;

int mca_parse_dts_u8(struct device_node *np, const char *prop, u8 *val,
		     u8 def_val);
int mca_parse_dts_u32(struct device_node *np, const char *prop, u32 *val,
		      u32 def_val);
int mca_parse_dts_u8_array(struct device_node *np, const char *prop, u8 *val,
			   u16 count);
int mca_parse_dts_u32_array(struct device_node *np, const char *prop, u32 *val,
			    u32 count);
int mca_parse_dts_u8_count(struct device_node *np, const char *prop,
			   u32 max_rows, u32 row_size);
int mca_parse_dts_u32_count(struct device_node *np, const char *prop,
			    u32 max_rows, u32 row_size);
int mca_parse_dts_u32_index(struct device_node *np, const char *prop, int index,
			    u32 *val);
int mca_parse_dts_string(struct device_node *np, const char *prop,
			 const char **str);
int mca_parse_dts_string_index(struct device_node *np, const char *prop,
			       int index, const char **str);
int mca_parse_dts_count_strings(struct device_node *np, const char *prop,
				u32 max_rows, u32 row_size);
int mca_parse_dts_string_array(struct device_node *np, const char *prop,
			       int *val, u32 max_rows, u32 row_size);

#endif /* __MCA_PARSE_DTS_H */
