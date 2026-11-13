// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 The LineageOS Project
 */

#define pr_fmt(fmt) "hwid: " fmt

#include <linux/bitfield.h>
#include <linux/hwid.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <soc/qcom/socinfo.h>

/*
 * hwid_value layout:
 *   [31:20] sales region
 *   [19:16] build stage
 *   [31:16] major hardware version
 *   [15:0]  minor hardware version
 */
#define HW_COUNTRY_VERSION_SHIFT	20
#define HW_MAJOR_VERSION_SHIFT		16
#define HW_MINOR_VERSION_MASK		GENMASK(15, 0)
#define HW_BUILD_VERSION_MASK		GENMASK(19, 16)

static uint hwid_value;
module_param(hwid_value, uint, 0444);
MODULE_PARM_DESC(hwid_value, "xiaomi hwid value correspondingly different build");

static uint project;
module_param(project, uint, 0444);
MODULE_PARM_DESC(project, "xiaomi project serial num predefine");

static uint build_adc;
module_param(build_adc, uint, 0444);
MODULE_PARM_DESC(build_adc, "xiaomi adc value of build resistance");

static uint project_adc;
module_param(project_adc, uint, 0444);
MODULE_PARM_DESC(project_adc, "xiaomi adc value of project resistance");

struct hwid_soc_projects {
	uint32_t soc_id;
	const char * const *names;
	unsigned int count;
};

/* Indexed by hwid.project - 1 */
static const char * const waipio_projects[] = {
	"zeus", "cupid", "katyusha", "ingres", "zizhan", "thor",
	"loki", "unicorn", "ziyi", "diting", "caiwei", "mayfly",
};

static const char * const kalama_projects[] = {
	"nuwa", "fuxi", "wangshu", "socrates", "ishtar", "babylon",
};

static const char * const pineapple_projects[] = {
	"shennong", "houji", "manet", "aurora", "suiren", "ruyi", "goku",
};

static const char * const cliffs_projects[] = {
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, "peridot", "chenfeng",
};

static const char * const sun_projects[] = {
	"haotian", "dada", "miro", "xuanyuan", "bixi", "nirvana",
	"onyx", "luming", "piano", "yupei", "annibale",
};

#define SOC_PROJECTS(id, table) { id, table, ARRAY_SIZE(table) }

static const struct hwid_soc_projects soc_projects[] = {
	SOC_PROJECTS(457, waipio_projects),
	SOC_PROJECTS(519, kalama_projects),
	SOC_PROJECTS(557, pineapple_projects),
	SOC_PROJECTS(614, cliffs_projects),
	SOC_PROJECTS(618, sun_projects),
};

const char *product_name_get(void)
{
	uint32_t soc_id = socinfo_get_id();
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(soc_projects); i++) {
		const struct hwid_soc_projects *p = &soc_projects[i];

		if (p->soc_id != soc_id)
			continue;

		if (project < 1 || project > p->count || !p->names[project - 1])
			break;

		return p->names[project - 1];
	}

	return "unknown";
}
EXPORT_SYMBOL(product_name_get);

uint32_t get_hw_version_platform(void)
{
	return project;
}
EXPORT_SYMBOL(get_hw_version_platform);

uint32_t get_hw_id_value(void)
{
	return hwid_value;
}
EXPORT_SYMBOL(get_hw_id_value);

uint32_t get_hw_country_version(void)
{
	return hwid_value >> HW_COUNTRY_VERSION_SHIFT;
}
EXPORT_SYMBOL(get_hw_country_version);

uint32_t get_hw_version_major(void)
{
	return hwid_value >> HW_MAJOR_VERSION_SHIFT;
}
EXPORT_SYMBOL(get_hw_version_major);

uint32_t get_hw_version_minor(void)
{
	return hwid_value & HW_MINOR_VERSION_MASK;
}
EXPORT_SYMBOL(get_hw_version_minor);

uint32_t get_hw_version_build(void)
{
	return FIELD_GET(HW_BUILD_VERSION_MASK, hwid_value);
}
EXPORT_SYMBOL(get_hw_version_build);

uint32_t get_hw_project_adc(void)
{
	return project_adc;
}
EXPORT_SYMBOL(get_hw_project_adc);

uint32_t get_hw_build_adc(void)
{
	return build_adc;
}
EXPORT_SYMBOL(get_hw_build_adc);

static int __init hwid_init(void)
{
	pr_info("project %u (%s), hwid 0x%x, project adc %u, build adc %u\n",
		project, product_name_get(), hwid_value, project_adc,
		build_adc);

	return 0;
}
module_init(hwid_init);

MODULE_DESCRIPTION("Hwid Module Driver for Xiaomi Corporation");
MODULE_LICENSE("GPL v2");
