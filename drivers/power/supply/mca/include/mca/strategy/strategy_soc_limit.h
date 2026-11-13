/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Capping the state of charge.
 *
 * A phone left on a charger at full for months ages its cell faster than one
 * cycled normally, so a demonstration unit or a phone in a test rack is told
 * to stop at a percentage rather than at full.  The cap is a number the rest
 * of the stack acts on, announced as an event rather than enforced here.
 */

#ifndef __MCA_SOC_LIMIT_H
#define __MCA_SOC_LIMIT_H

#include <linux/types.h>

void soc_limit_process(bool en, int soc_limit_thre);

#endif /* __MCA_SOC_LIMIT_H */
