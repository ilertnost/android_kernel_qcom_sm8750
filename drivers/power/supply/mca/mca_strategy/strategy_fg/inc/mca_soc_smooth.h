/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __XM_SOC_SMOOTH_H
#define __XM_SOC_SMOOTH_H

/* A raw state of charge of 100.00%, in hundredths. */
#define FG_RAW_SOC_FULL 10000
/* How far the raw reading may drop before a held 100% is let go. */
#define FG_HOLD_UISOC_100_GAP 120

int fg_ui_soc_smooth(struct strategy_fg *fg, bool prohibit_jump);
int strategy_soc_smooth_parse_dt(struct strategy_fg *fg);

#endif /* __XM_SOC_SMOOTH_H */
