/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Charging around what the user is doing.
 *
 * A phone charging while it is being used has to trade charging speed against
 * everything else the user notices: how warm the phone gets in the hand, how
 * long the battery lasts over years, whether a game stutters because the
 * charger is heating the board.  Userspace knows what is on screen and what
 * the user has asked for; this is where it says so, and where those requests
 * are turned into the limits the charging strategies work to.
 *
 * The requests are votes rather than settings: several of them can be in
 * force at once, and the most conservative wins.
 */

#ifndef __MCA_SMART_CHARGE_H
#define __MCA_SMART_CHARGE_H

#include <linux/types.h>

/* The modes userspace can ask for, each a bit in the control word. */
enum SMART_CHG_MODE {
	SMART_CHG_NIGHT,
	SMART_CHG_NAVIGATION,
	SMART_CHG_OUTDOOR,
	SMART_CHG_LOWFAST,
	SMART_CHG_ENDURANCE_PRO,
	SMART_CHG_WLS_SUPER,
	SMART_CHG_SENSE_CHG,
	SMART_CHG_WLS_QUIET,
	SMART_CHG_EXTREME_COLD,
	SMART_CHG_TRAVELWAIT,
	SMART_CHG_BYPASS,
	SMART_CHG_MAX = 15,
};

/*
 * What the phone is being used for.  The numbers are not contiguous: they are
 * what userspace sends, and the gaps are scenes that were retired.  A scene
 * above %SMART_CHG_SCENE_REDIR_START is the same scene reported by a second
 * source, which the phone treats identically.
 */
enum smart_chg_scene_type {
	SMART_CHG_SCENE_NORMAL		= 0,
	SMART_CHG_SCENE_HUANJI		= 1,
	SMART_CHG_SCENE_PHONE		= 5,
	SMART_CHG_SCENE_NOLIMIT		= 6,
	SMART_CHG_SCENE_CLASS0		= 7,
	SMART_CHG_SCENE_YOUTUBE		= 8,
	SMART_CHG_SCENE_NAVIGATION	= 10,
	SMART_CHG_SCENE_VIDEO		= 11,
	SMART_CHG_SCENE_VIDEOCHAT	= 14,
	SMART_CHG_SCENE_CAMERA		= 15,
	SMART_CHG_SCENE_TGAME		= 18,
	SMART_CHG_SCENE_MGAME		= 19,
	SMART_CHG_SCENE_YUANSHEN	= 20,
	SMART_CHG_SCENE_XINGTIE		= 25,
	SMART_CHG_SCENE_DANMU		= 28,
	SMART_CHG_SCENE_PER_NORMAL	= 50,
	SMART_CHG_SCENE_PER_CLASS0	= 57,
	SMART_CHG_SCENE_PER_YOUTUBE	= 58,
	SMART_CHG_SCENE_PER_VIDEO	= 61,
	SMART_CHG_SCENE_PER_XINGTIE	= 75,
	SMART_CHG_SCENE_PER_DANMU	= 78,
	SMART_CHG_SCENE_HP_GAME		= 501,
	SMART_CHG_SCENE_CGAME		= 700,
	SMART_CHG_SCENE_CGAME_2		= 702,
	SMART_CHG_SCENE_INDEX_MAX	= 703,
	SMART_CHG_SCENE_REDIR_START	= 1000,
	SMART_CHG_SCENE_REDIR_INDEX_MAX	= 1703,
};

/* How the phone is resting, which changes how well it sheds heat. */
enum smart_chg_posture_stat {
	SMART_CHG_POSTURE_UNKNOW = 0,
	SMART_CHG_POSTURE_DESKTOP,
	SMART_CHG_POSTURE_HOLDER,
	SMART_CHG_POSTURE_ONEHAND,
	SMART_CHG_POSTURE_TWOHAND_H,
	SMART_CHG_POSTURE_TWOHAND_V,
	SMART_CHG_POSTURE_ANS_CALL,
	SMART_CHG_POSTURE_INDEX_MAX,

	/* What a test writes to stand in for the real posture. */
	SMART_CHG_POSTURE_FAKE_CLEAR = 100,
	SMART_CHG_POSTURE_FAKE_DESKTOP,
	SMART_CHG_POSTURE_FAKE_HOLDER,
	SMART_CHG_POSTURE_FAKE_ONEHAND,
	SMART_CHG_POSTURE_FAKE_TWOHAND_H,
	SMART_CHG_POSTURE_FAKE_TWOHAND_V,
};

/* Which of the two boosts a mode is asking for. */
enum smart_chg_pwr_boost_type {
	SMART_CHG_OUTDOOR_PWR_BOOST,
	SMART_CHG_TRAVELWAIT_PWR_BOOST,
	SMART_CHG_PWR_BOOST_MAX_INDEX,
};

/**
 * union SMART_CHG_HEADER - one request from userspace
 * @AsUINT32:   the whole word
 * @enable:     the mode is being turned on rather than off
 * @func_type:  which mode is being set: bit (n - 1) for &enum SMART_CHG_MODE n,
 *              exactly one of them
 * @func_value: what it is being set to
 * @soc_limit:  for the modes that cap the battery level, the cap, in percent
 *
 * The mode bits below sit at bit n for &enum SMART_CHG_MODE n, so the word
 * read back through sysfs (ret_code in bit 0, then one bit per mode in force)
 * uses the same positions.
 */
union SMART_CHG_HEADER {
	u32 AsUINT32;
	struct {
		u32 enable:1;
		u32 func_type:15;
		u32 func_value:16;
	};
	struct {
		u32 :1;
		u32 navigation:1;
		u32 outdoor:1;
		u32 lowfast:1;
		u32 endurance_pro:1;
		u32 wls_super_chg:1;
		u32 sense_chg:1;
		u32 wls_quiet:1;
		u32 extreme_cold:1;
		u32 travel_wait:1;
		u32 smart_bypass:1;
		u32 :5;
		u32 soc_limit:8;
		u32 :8;
	};
};

/**
 * struct SMART_CHG_DATA - what one mode was asked for
 * @enable:         the mode is on
 * @func_value:     what it was set to
 * @use_fake_value: a test is overriding it
 * @fake_value:     what the test set it to
 *
 * Which mode this is comes from the entry's place in the array, so it is not
 * repeated here.
 */
struct SMART_CHG_DATA {
	u16	enable;
	u16	func_value;
	bool	use_fake_value;
	int	fake_value;
};

/**
 * struct SMART_CHG_INFO - every mode, as userspace last left it
 * @ret_code:      what the last request came to
 * @smart_chgcfg:  one entry per mode
 */
struct SMART_CHG_INFO {
	u32			ret_code;
	struct SMART_CHG_DATA	smart_chgcfg[SMART_CHG_MAX];
};

/**
 * struct smart_batt_temp_range - which band, and what it covers
 * @idx: the band's place in the charging table
 * @min: it starts here, in degrees
 * @max: and ends below here
 */
struct smart_batt_temp_range {
	int	idx;
	int	min;
	int	max;
};

/**
 * struct smart_batt_jeita_term_para - what one band should finish at
 * @t_range: the band this is for
 * @vterm:   the float voltage it should charge to, in millivolts
 * @iterm:   the current it should finish at, in milliamps
 *
 * A cell that has aged will not take the voltage it took when new, and one
 * that has aged little should not be held to a limit set for the worst case.
 * Measuring the cell and redrawing where each band finishes is what this
 * carries; where the bands themselves lie is the chemistry's and does not
 * move.
 */
struct smart_batt_jeita_term_para {
	struct smart_batt_temp_range	t_range;
	int				vterm;
	int				iterm;
};

/*
 * A cell's charging behaviour changes as it ages, so userspace can hand the
 * kernel a replacement for the tables the device tree shipped with.  What
 * follows is the shape of that hand-off: a header, then one description per
 * temperature band, then the curve each band follows.  In order: the
 * header, the jeita terms (ffc ones first), the wired specs, then the
 * wireless specs, each spec immediately followed by its curve points.
 */
/* One point on a band's curve: the current allowed between two levels. */
struct smart_batt_spec_curve {
	int	mv;
	int	ma_h;
	int	ma_l;
};

/*
 * How many curve points one band may carry.  The consumers stage the points
 * in an array of this size before taking them, so it is a limit of the
 * hand-off format itself and not of any one consumer: smart_charge refuses a
 * blob that exceeds it rather than letting a consumer overrun its staging
 * buffer.
 */
#define SMART_BATT_SPEC_MAX_STEPS	8

struct smart_batt_spec {
	u32				type;
	u32				ffc;
	struct smart_batt_temp_range	t_range;
	u32				step_size;
	struct smart_batt_spec_curve	*steps;
};

/*
 * @checksum is the byte sum of everything from @type up to @total_len bytes
 * into the blob; @total_len counts from the start of the header.
 */
struct smart_basp_header {
	u32	checksum;
	u32	type;
	u32	total_len;
	u32	jeita_ffc_term_size;
	u32	jeita_normal_term_size;
	u32	wired_ffc_size;
	u32	wired_normal_size;
	u32	wls_ffc_size;
	u32	wls_normal_size;
};

/* Which part of the charging path a client of the smart-charge interface is. */
enum mca_smartchg_if_charger_type {
	MCA_SMARTCHG_IF_CHG_TYPE_BUCK = 0,
	MCA_SMARTCHG_IF_CHG_TYPE_WL_BUCK,
	MCA_SMARTCHG_IF_CHG_TYPE_QC,
	MCA_SMARTCHG_IF_CHG_TYPE_WL_QC,
	MCA_SMARTCHG_IF_CHG_TYPE_JEITA,
	MCA_SMARTCHG_IF_CHG_TYPE_THERMAL,
	MCA_SMARTCHG_IF_CHG_TYPE_END,
};

/**
 * struct mca_smartchg_if_ops - what the charging stack does with the requests
 * @type: which charger this implementation drives
 * @data: handed back to every call
 *
 * The limits are handed on rather than applied here: whether a lower float
 * voltage means charging more slowly or stopping sooner is the strategy's
 * decision, not this module's.
 */
struct mca_smartchg_if_ops {
	int	type;
	void	*data;

	int (*set_delta_fv)(void *data, int delta_fv);
	int (*set_delta_ichg)(void *data, int delta_ichg);
	int (*set_fcc)(void *data, int fcc);
	int (*set_soc_limit_sts)(void *data, int soc_limit_sts);
	int (*set_pwr_boost_sts)(void *data, int pwr_boost_sts);
	int (*set_wls_super_sts)(void *data, int wls_super_sts);
	int (*set_wls_quiet_sts)(void *data, int wls_quiet_sts);
	int (*update_baa_para)(void *data, char *para, int ffc_size,
			       int normal_size);
};

int mca_smartchg_if_ops_register(struct mca_smartchg_if_ops *ops);

int mca_smartchg_get_scene(void);
int mca_smartchg_set_scene(int scene);
int mca_smartchg_get_board_temp(void);
int mca_smartchg_set_board_temp(int board_temp);
int mca_smartchg_get_limit_soc(void);
int mca_smartchg_is_extreme_cold_enabled(void);

#endif /* __MCA_SMART_CHARGE_H */
