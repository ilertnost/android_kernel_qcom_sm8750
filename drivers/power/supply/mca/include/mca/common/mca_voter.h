/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015-2017, 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2025 The LineageOS Project
 */

#ifndef __MCA_COMMON_H
#define __MCA_COMMON_H

#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>

struct mca_votable;

/**
 * enum mca_votable_type - how an election combines its clients' votes
 * @MCA_VOTE_MIN: the smallest value among the enabled clients
 * @MCA_VOTE_MAX: the largest value among the enabled clients
 * @MCA_VOTE_OR:  the first non-zero value among the enabled clients, in the
 *                order the clients first voted; 0 if every enabled client
 *                voted 0
 * @MCA_VOTE_AND: 0 if any enabled client voted 0, otherwise the value of
 *                the last enabled client
 *
 * For every type, an election with no enabled client results in the
 * votable's default value.  The set types use the voted value as it is: a
 * client votes 1 to set the flag, not merely by being enabled.
 */
enum mca_votable_type {
	MCA_VOTE_MIN,
	MCA_VOTE_MAX,
	MCA_VOTE_OR,
	MCA_VOTE_AND,
	NUM_VOTABLE_TYPES,
};

bool mca_is_client_vote_enabled(struct mca_votable *votable, const char *client_str);
bool mca_is_client_vote_enabled_locked(struct mca_votable *votable,
				       const char *client_str);
bool mca_is_override_vote_enabled(struct mca_votable *votable);
bool mca_is_override_vote_enabled_locked(struct mca_votable *votable);
int mca_get_client_vote(struct mca_votable *votable, const char *client_str);
int mca_get_client_vote_locked(struct mca_votable *votable, const char *client_str);
int mca_get_effective_result(struct mca_votable *votable);
int mca_get_effective_result_locked(struct mca_votable *votable);
const char *mca_get_effective_client(struct mca_votable *votable);
const char *mca_get_effective_client_locked(struct mca_votable *votable);
int mca_vote(struct mca_votable *votable, const char *client_str, bool state,
	     int val);
int mca_vote_override(struct mca_votable *votable, const char *override_client,
		      bool state, int val);
int mca_rerun_election(struct mca_votable *votable);
struct mca_votable *mca_find_votable(const char *name);
struct mca_votable *mca_create_votable(const char *name, int votable_type,
				   int (*callback)(struct mca_votable *votable,
						   void *data,
						   int effective_result,
						   const char *effective_client),
				   int default_val, void *data);
void mca_destroy_votable(struct mca_votable *votable);
void mca_lock_votable(struct mca_votable *votable);
void mca_unlock_votable(struct mca_votable *votable);

#endif /* __MCA_COMMON_H */
