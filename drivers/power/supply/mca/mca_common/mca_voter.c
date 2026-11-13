// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2017, 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2025 The LineageOS Project
 */

#define MCA_LOG_TAG "mca_voter"

#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <mca/common/mca_voter.h>
#include <mca/common/mca_log.h>

#define NUM_MAX_CLIENTS		32
#define DEBUG_FORCE_CLIENT	"DEBUG_FORCE_CLIENT"

/*
 * How much of an overriding client's name is kept.  The name belongs to the
 * caller, so it is copied rather than pointed at.
 */
#define MAX_OVERRIDE_CLIENT_LEN	50

static DEFINE_SPINLOCK(votable_list_slock);
static LIST_HEAD(votable_list);

static struct dentry *debug_root;

struct client_vote {
	bool	enabled;
	int	value;
};

struct mca_votable {
	const char		*name;
	char			override_client[MAX_OVERRIDE_CLIENT_LEN];
	struct list_head	list;
	struct client_vote	votes[NUM_MAX_CLIENTS];
	int			num_clients;
	int			type;
	int			effective_client_id;
	int			effective_result;
	int			override_result;
	struct mutex		vote_lock;
	void			*data;
	int			(*callback)(struct mca_votable *votable,
					    void *data,
					    int effective_result,
					    const char *effective_client);
	char			*client_strs[NUM_MAX_CLIENTS];
	bool			voted_on;
	struct dentry		*root;
	struct dentry		*status_ent;
	u32			force_val;
	bool			force_active;
	struct dentry		*force_active_ent;
	int			default_val;
};

/**
 * vote_min() - the smallest value among the enabled clients
 * @votable:	votable object
 * @client_id:	client number of the latest voter
 * @eff_res:	the smallest enabled value
 * @eff_id:	the client that voted it, or -EINVAL if no client is enabled
 *
 * Context: Must be called with the votable->lock held
 */
static void vote_min(struct mca_votable *votable, int client_id,
		     int *eff_res, int *eff_id)
{
	int i;

	*eff_res = INT_MAX;
	*eff_id = -EINVAL;
	for (i = 0; i < votable->num_clients && votable->client_strs[i]; i++) {
		if (votable->votes[i].enabled &&
		    *eff_res > votable->votes[i].value) {
			*eff_res = votable->votes[i].value;
			*eff_id = i;
		}
	}
}

/**
 * vote_max() - the largest value among the enabled clients
 * @votable:	votable object
 * @client_id:	client number of the latest voter
 * @eff_res:	the largest enabled value
 * @eff_id:	the client that voted it, or -EINVAL if no client is enabled
 *
 * Context: Must be called with the votable->lock held
 */
static void vote_max(struct mca_votable *votable, int client_id,
		     int *eff_res, int *eff_id)
{
	int i;

	*eff_res = INT_MIN;
	*eff_id = -EINVAL;
	for (i = 0; i < votable->num_clients && votable->client_strs[i]; i++) {
		if (votable->votes[i].enabled &&
		    *eff_res < votable->votes[i].value) {
			*eff_res = votable->votes[i].value;
			*eff_id = i;
		}
	}
}

/**
 * vote_set_or() - the first non-zero value among the enabled clients
 * @votable:	votable object
 * @client_id:	client number of the latest voter
 * @eff_res:	the first non-zero enabled value, or 0
 * @eff_id:	@client_id if any client is enabled, -EINVAL otherwise
 *
 * Context: Must be called with the votable->lock held
 */
static void vote_set_or(struct mca_votable *votable, int client_id,
			int *eff_res, int *eff_id)
{
	int i;

	*eff_res = 0;
	*eff_id = -EINVAL;
	for (i = 0; i < votable->num_clients && votable->client_strs[i]; i++) {
		if (!votable->votes[i].enabled)
			continue;

		*eff_id = client_id;
		if (votable->votes[i].value) {
			*eff_res = votable->votes[i].value;
			return;
		}
	}
}

/**
 * vote_set_and() - 0 if any enabled client voted 0
 * @votable:	votable object
 * @client_id:	client number of the latest voter
 * @eff_res:	0 if an enabled client voted 0, otherwise the value of the
 *		last enabled client
 * @eff_id:	@client_id if any client is enabled, -EINVAL otherwise
 *
 * Context: Must be called with the votable->lock held
 */
static void vote_set_and(struct mca_votable *votable, int client_id,
			 int *eff_res, int *eff_id)
{
	int i;

	*eff_res = 0;
	*eff_id = -EINVAL;
	for (i = 0; i < votable->num_clients && votable->client_strs[i]; i++) {
		if (!votable->votes[i].enabled)
			continue;

		*eff_id = client_id;
		*eff_res = votable->votes[i].value;
		if (!*eff_res)
			return;
	}
}

/*
 * The id of @client_str, which becomes a client of @votable the first time
 * it is named here.
 */
static int get_client_id(struct mca_votable *votable, const char *client_str)
{
	int i;

	for (i = 0; i < votable->num_clients; i++) {
		if (votable->client_strs[i] &&
		    !strcmp(votable->client_strs[i], client_str))
			return i;
	}

	for (i = 0; i < votable->num_clients; i++) {
		if (!votable->client_strs[i]) {
			votable->client_strs[i] = kstrdup(client_str,
							  GFP_KERNEL);
			if (!votable->client_strs[i])
				return -ENOMEM;
			return i;
		}
	}

	return -EINVAL;
}

static char *get_client_str(struct mca_votable *votable, int client_id)
{
	if (!votable || client_id == -EINVAL)
		return NULL;

	return votable->client_strs[client_id];
}

void mca_lock_votable(struct mca_votable *votable)
{
	mutex_lock(&votable->vote_lock);
}
EXPORT_SYMBOL(mca_lock_votable);

void mca_unlock_votable(struct mca_votable *votable)
{
	mutex_unlock(&votable->vote_lock);
}
EXPORT_SYMBOL(mca_unlock_votable);

/**
 * mca_is_override_vote_enabled() -
 * mca_is_override_vote_enabled_locked() -
 *		The unlocked and locked variants of getting whether an override
 *		vote is in force.
 * @votable:	the votable object
 *
 * Returns:
 *	True if an override vote is in force; false otherwise.
 */
bool mca_is_override_vote_enabled_locked(struct mca_votable *votable)
{
	if (!votable)
		return false;

	return votable->override_result != -EINVAL;
}
EXPORT_SYMBOL(mca_is_override_vote_enabled_locked);

bool mca_is_override_vote_enabled(struct mca_votable *votable)
{
	bool enable;

	if (!votable)
		return false;

	mca_lock_votable(votable);
	enable = mca_is_override_vote_enabled_locked(votable);
	mca_unlock_votable(votable);

	return enable;
}
EXPORT_SYMBOL(mca_is_override_vote_enabled);

/**
 * mca_is_client_vote_enabled() -
 * mca_is_client_vote_enabled_locked() -
 *		The unlocked and locked variants of getting whether a client's
 *		vote is enabled.
 * @votable:	the votable object
 * @client_str: client of interest
 *
 * Returns:
 *	True if the client's vote is enabled; false otherwise.
 */
bool mca_is_client_vote_enabled_locked(struct mca_votable *votable,
				       const char *client_str)
{
	int client_id;

	if (!votable || !client_str)
		return false;

	client_id = get_client_id(votable, client_str);
	if (client_id < 0)
		return false;

	return votable->votes[client_id].enabled;
}
EXPORT_SYMBOL(mca_is_client_vote_enabled_locked);

bool mca_is_client_vote_enabled(struct mca_votable *votable,
				const char *client_str)
{
	bool enabled;

	if (!votable || !client_str)
		return false;

	mca_lock_votable(votable);
	enabled = mca_is_client_vote_enabled_locked(votable, client_str);
	mca_unlock_votable(votable);

	return enabled;
}
EXPORT_SYMBOL(mca_is_client_vote_enabled);

/**
 * mca_get_client_vote() -
 * mca_get_client_vote_locked() -
 *		The unlocked and locked variants of getting a client's voted
 *		value.
 * @votable:	the votable object
 * @client_str: client of interest
 *
 * Returns:
 *	The value the client voted for.  -EINVAL is returned if the client
 *	is not found, or is not enabled on anything but an MCA_VOTE_OR
 *	votable.
 */
int mca_get_client_vote_locked(struct mca_votable *votable,
			       const char *client_str)
{
	int client_id;

	if (!votable || !client_str)
		return -EINVAL;

	client_id = get_client_id(votable, client_str);
	if (client_id < 0)
		return -EINVAL;

	if (votable->type != MCA_VOTE_OR && !votable->votes[client_id].enabled)
		return -EINVAL;

	return votable->votes[client_id].value;
}
EXPORT_SYMBOL(mca_get_client_vote_locked);

int mca_get_client_vote(struct mca_votable *votable, const char *client_str)
{
	int value;

	if (!votable || !client_str)
		return -EINVAL;

	mca_lock_votable(votable);
	value = mca_get_client_vote_locked(votable, client_str);
	mca_unlock_votable(votable);

	return value;
}
EXPORT_SYMBOL(mca_get_client_vote);

/**
 * mca_get_effective_result() -
 * mca_get_effective_result_locked() -
 *		The unlocked and locked variants of getting the effective value
 *		amongst all the enabled voters.
 * @votable:	the votable object
 *
 * Returns:
 *	The forced value while force_active is set, else the override value
 *	while an override is in force, else the result of the last election.
 *	The default value is returned while there is no result yet.
 */
int mca_get_effective_result_locked(struct mca_votable *votable)
{
	if (!votable)
		return -EINVAL;

	if (votable->force_active)
		return votable->force_val;

	if (votable->override_result != -EINVAL)
		return votable->override_result;

	if (votable->effective_result == -EINVAL)
		return votable->default_val;

	return votable->effective_result;
}
EXPORT_SYMBOL(mca_get_effective_result_locked);

int mca_get_effective_result(struct mca_votable *votable)
{
	int value;

	if (!votable)
		return -EINVAL;

	mca_lock_votable(votable);
	value = mca_get_effective_result_locked(votable);
	mca_unlock_votable(votable);

	return value;
}
EXPORT_SYMBOL(mca_get_effective_result);

/**
 * mca_get_effective_client() -
 * mca_get_effective_client_locked() -
 *		The unlocked and locked variants of getting the effective client
 *		amongst all the enabled voters.
 * @votable:	the votable object
 *
 * Returns:
 *	The effective client, or NULL if no client is enabled.
 */
const char *mca_get_effective_client_locked(struct mca_votable *votable)
{
	if (!votable)
		return NULL;

	if (votable->force_active)
		return DEBUG_FORCE_CLIENT;

	if (votable->override_result != -EINVAL)
		return votable->override_client;

	return get_client_str(votable, votable->effective_client_id);
}
EXPORT_SYMBOL(mca_get_effective_client_locked);

const char *mca_get_effective_client(struct mca_votable *votable)
{
	const char *client_str;

	if (!votable)
		return NULL;

	mca_lock_votable(votable);
	client_str = mca_get_effective_client_locked(votable);
	mca_unlock_votable(votable);

	return client_str;
}
EXPORT_SYMBOL(mca_get_effective_client);

/**
 * mca_vote() - cast or withdraw a client's vote
 * @votable:	the votable object
 * @client_str: the voting client
 * @enabled:	whether the client takes part in the election; its @val is
 *		only counted while it does
 * @val:	the vote value
 *
 * The callback is called when the effective result or the effective client
 * changes, and on the first vote.  A vote identical to the client's previous
 * one is ignored.
 *
 * Returns:
 *	The return from the callback when it was called, or zero.
 */
int mca_vote(struct mca_votable *votable, const char *client_str,
	     bool enabled, int val)
{
	int effective_id = -EINVAL;
	int effective_result;
	const char *pad;
	int client_id;
	int rc = 0;
	int len, i;

	if (!votable || !client_str)
		return -EINVAL;

	mca_lock_votable(votable);

	client_id = get_client_id(votable, client_str);
	if (client_id < 0) {
		mca_log_err("can not find client id %s\n", client_str);
		rc = client_id;
		goto out;
	}

	if (votable->votes[client_id].enabled == enabled &&
	    votable->votes[client_id].value == val) {
		mca_log_debug("%s: %s,%d same vote %s of val=%d\n",
			      votable->name, client_str, client_id,
			      enabled ? "on" : "off", val);
		votable->votes[client_id].enabled = enabled;
		votable->votes[client_id].value = val;

		if (votable->voted_on) {
			mca_log_debug("%s: %s,%d Ignoring similar vote %s of val=%d\n",
				      votable->name, client_str, client_id,
				      enabled ? "on" : "off", val);
			goto out;
		}
	} else {
		votable->votes[client_id].enabled = enabled;
		votable->votes[client_id].value = val;
	}

	mca_log_info("%s: %s,%d voting %s of val=%d\n", votable->name,
		     client_str, client_id, enabled ? "on" : "off", val);

	switch (votable->type) {
	case MCA_VOTE_MIN:
		vote_min(votable, client_id, &effective_result, &effective_id);
		break;
	case MCA_VOTE_MAX:
		vote_max(votable, client_id, &effective_result, &effective_id);
		break;
	case MCA_VOTE_OR:
		vote_set_or(votable, client_id, &effective_result,
			    &effective_id);
		break;
	case MCA_VOTE_AND:
		vote_set_and(votable, client_id, &effective_result,
			     &effective_id);
		break;
	default:
		rc = -EINVAL;
		goto out;
	}

	/* With no enabled client the election falls back to its default. */
	if (effective_id == -EINVAL)
		effective_result = votable->default_val;

	if (!votable->voted_on ||
	    effective_result != votable->effective_result ||
	    effective_id != votable->effective_client_id) {
		votable->effective_client_id = effective_id;
		votable->effective_result = effective_result;
		mca_log_info("%s: effective vote is now %d voted by %s,%d\n",
			     votable->name, effective_result,
			     get_client_str(votable, effective_id),
			     effective_id);
		if (votable->callback && !votable->force_active &&
		    votable->override_result == -EINVAL) {
			rc = votable->callback(votable, votable->data,
					       effective_result,
					       get_client_str(votable,
							      effective_id));
			if (rc)
				mca_log_err("callback failed with rc=%d\n", rc);
		}
	}

	/* Every client and its vote, printed as a table. */
	mca_log_info("%s VOTER:\n", votable->name);
	mca_log_info("\t\t\t\t\tclient\t\t\t\t\t\tenable\t\tvalue");

	for (i = 0; i < votable->num_clients; i++) {
		if (!votable->client_strs[i])
			break;

		/*
		 * The name follows its index and a dot, and the next column
		 * starts on a tab stop, so the padding is counted from the
		 * width of both together.
		 */
		len = strlen(votable->client_strs[i]) + (i > 9 ? 1 : 0);
		if (len < 2)
			pad = "\t\t\t\t\t\t";
		else if (len < 6)
			pad = "\t\t\t\t\t";
		else if (len < 10)
			pad = "\t\t\t\t";
		else if (len < 14)
			pad = "\t\t\t";
		else if (len < 18)
			pad = "\t\t";
		else if (len < 22)
			pad = "\t";
		else
			pad = "";

		mca_log_info("\t\t\t\t\t%d.%s%s\t%d\t\t\t%d\n", i,
			     votable->client_strs[i], pad,
			     votable->votes[i].enabled,
			     votable->votes[i].value);
	}

	votable->voted_on = true;
out:
	mca_unlock_votable(votable);

	return rc;
}
EXPORT_SYMBOL(mca_vote);

/**
 * mca_vote_override() - force a value past the election
 * @votable:		The votable object
 * @override_client:	The client forcing the value
 * @enabled:		Whether the override is put in force or withdrawn
 * @val:		The value forced while @enabled
 *
 * The callback is called with @val, and the override only takes effect if
 * it succeeds.  Withdrawing the override calls the callback with the result
 * of the last election.  While force_active is set the override is only
 * recorded.
 *
 * Returns:
 *	The return from the callback, or zero.
 */
int mca_vote_override(struct mca_votable *votable, const char *override_client,
		      bool enabled, int val)
{
	int rc = 0;

	if (!votable || !override_client)
		return -EINVAL;

	mca_lock_votable(votable);

	if (votable->force_active) {
		votable->override_result = enabled ? val : -EINVAL;
		goto out;
	}

	if (enabled) {
		if (votable->callback)
			rc = votable->callback(votable, votable->data, val,
					       override_client);
		if (rc)
			goto out;

		strscpy(votable->override_client, override_client,
			sizeof(votable->override_client));
		votable->override_result = val;
		mca_log_err("ovrried vote %d", val);
	} else {
		if (votable->callback)
			rc = votable->callback(votable, votable->data,
					       votable->effective_result,
					       get_client_str(votable,
						votable->effective_client_id));
		votable->override_result = -EINVAL;
		mca_log_err("cleared ovrried vote %d", -EINVAL);
	}

out:
	mca_unlock_votable(votable);

	return rc;
}
EXPORT_SYMBOL(mca_vote_override);

/**
 * mca_rerun_election() - call the callback again with the effective result
 * @votable:	the votable object
 *
 * Returns:
 *	The return from the callback, or zero if there is none.
 */
int mca_rerun_election(struct mca_votable *votable)
{
	int effective_result;
	int rc = 0;

	if (!votable)
		return -EINVAL;

	mca_lock_votable(votable);
	effective_result = mca_get_effective_result_locked(votable);
	if (votable->callback)
		rc = votable->callback(votable, votable->data, effective_result,
				       get_client_str(votable,
						votable->effective_client_id));
	mca_unlock_votable(votable);

	return rc;
}
EXPORT_SYMBOL(mca_rerun_election);

struct mca_votable *mca_find_votable(const char *name)
{
	struct mca_votable *v, *found = NULL;
	unsigned long flags;

	if (!name)
		return NULL;

	spin_lock_irqsave(&votable_list_slock, flags);
	list_for_each_entry(v, &votable_list, list) {
		if (!strcmp(v->name, name)) {
			found = v;
			break;
		}
	}
	spin_unlock_irqrestore(&votable_list_slock, flags);

	return found;
}
EXPORT_SYMBOL(mca_find_votable);

static int mca_force_active_get(void *data, u64 *val)
{
	struct mca_votable *votable = data;

	*val = votable->force_active;

	return 0;
}

static int mca_force_active_set(void *data, u64 val)
{
	struct mca_votable *votable = data;
	int effective_result;
	const char *client;
	int rc = 0;

	mca_lock_votable(votable);
	votable->force_active = !!val;

	if (!votable->callback)
		goto out;

	if (votable->force_active) {
		rc = votable->callback(votable, votable->data,
				       votable->force_val, DEBUG_FORCE_CLIENT);
	} else {
		if (votable->override_result != -EINVAL) {
			effective_result = votable->override_result;
			client = votable->override_client;
		} else {
			effective_result = votable->effective_result;
			client = get_client_str(votable,
						votable->effective_client_id);
		}
		rc = votable->callback(votable, votable->data, effective_result,
				       client);
	}
out:
	mca_unlock_votable(votable);

	return rc;
}
DEFINE_DEBUGFS_ATTRIBUTE(mca_votable_force_ops, mca_force_active_get,
			 mca_force_active_set, "%lld\n");

static int mca_show_votable_clients(struct seq_file *m, void *data)
{
	struct mca_votable *votable = m->private;
	const char *effective_client_str;
	const char *type_str = "Unkonwn";
	int i;

	mca_lock_votable(votable);

	for (i = 0; i < votable->num_clients; i++) {
		if (votable->client_strs[i]) {
			seq_printf(m, "%s: %s:\t\t\ten=%d v=%d\n",
				   votable->name,
				   votable->client_strs[i],
				   votable->votes[i].enabled,
				   votable->votes[i].value);
		}
	}

	switch (votable->type) {
	case MCA_VOTE_MIN:
		type_str = "Min";
		break;
	case MCA_VOTE_MAX:
		type_str = "Max";
		break;
	case MCA_VOTE_OR:
		type_str = "Set_Or";
		break;
	case MCA_VOTE_AND:
		type_str = "Set_And";
		break;
	}

	effective_client_str = mca_get_effective_client_locked(votable);
	seq_printf(m, "%s: effective=%s type=%s v=%d\n",
		   votable->name,
		   effective_client_str ? effective_client_str : "none",
		   type_str,
		   mca_get_effective_result_locked(votable));

	mca_unlock_votable(votable);

	return 0;
}

static int mca_votable_status_open(struct inode *inode, struct file *file)
{
	struct mca_votable *votable = inode->i_private;

	return single_open(file, mca_show_votable_clients, votable);
}

static const struct file_operations mca_votable_status_ops = {
	.owner		= THIS_MODULE,
	.open		= mca_votable_status_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void mca_votable_unlist(struct mca_votable *votable)
{
	unsigned long flags;

	spin_lock_irqsave(&votable_list_slock, flags);
	list_del(&votable->list);
	spin_unlock_irqrestore(&votable_list_slock, flags);
}

/**
 * mca_create_votable() - create a votable, or find the one of that name
 * @name:		the votable's name, by which mca_find_votable() finds it
 * @votable_type:	one of enum mca_votable_type
 * @callback:		called with each new effective result, may be NULL
 * @default_val:	the result while no client is enabled
 * @data:		passed back to @callback
 *
 * A votable that already exists under @name is returned as it is, so that
 * drivers sharing a votable can each create it.
 *
 * Returns:
 *	The votable, or an ERR_PTR().
 */
struct mca_votable *mca_create_votable(const char *name, int votable_type,
				       int (*callback)(struct mca_votable *votable,
						       void *data,
						       int effective_result,
						       const char *effective_client),
				       int default_val, void *data)
{
	struct mca_votable *votable;
	unsigned long flags;

	if (!name)
		return ERR_PTR(-EINVAL);

	votable = mca_find_votable(name);
	if (votable)
		return votable;

	if (!debug_root) {
		debug_root = debugfs_create_dir("business-votable", NULL);
		if (!debug_root) {
			mca_log_err("Couldn't create debug dir\n");
			return ERR_PTR(-ENOMEM);
		}
	}

	if (votable_type >= NUM_VOTABLE_TYPES) {
		mca_log_err("Invalid votable_type specified for voter\n");
		return ERR_PTR(-EINVAL);
	}

	votable = kzalloc(sizeof(*votable), GFP_KERNEL);
	if (!votable)
		return ERR_PTR(-ENOMEM);

	votable->name = kstrdup(name, GFP_KERNEL);
	if (!votable->name) {
		kfree(votable);
		return ERR_PTR(-ENOMEM);
	}

	votable->num_clients = NUM_MAX_CLIENTS;
	votable->callback = callback;
	votable->type = votable_type;
	votable->data = data;
	votable->override_result = -EINVAL;
	mutex_init(&votable->vote_lock);

	/*
	 * Before the first vote there is no effective client, and no result
	 * except on an MCA_VOTE_OR votable, which starts out clear.
	 */
	votable->effective_client_id = -EINVAL;
	votable->effective_result = votable->type == MCA_VOTE_OR ? 0 : -EINVAL;

	spin_lock_irqsave(&votable_list_slock, flags);
	list_add(&votable->list, &votable_list);
	spin_unlock_irqrestore(&votable_list_slock, flags);

	votable->root = debugfs_create_dir(name, debug_root);
	if (!votable->root) {
		mca_log_err("Couldn't create debug dir %s\n", name);
		mca_votable_unlist(votable);
		kfree(votable->name);
		kfree(votable);
		return ERR_PTR(-ENOMEM);
	}

	votable->status_ent = debugfs_create_file("status", S_IFREG | 0444,
						  votable->root, votable,
						  &mca_votable_status_ops);
	if (!votable->status_ent) {
		mca_log_err("Couldn't create status dbg file for %s\n", name);
		goto err_debugfs;
	}

	debugfs_create_u32("force_val", S_IFREG | 0644, votable->root,
			   &votable->force_val);

	votable->force_active_ent = debugfs_create_file("force_active",
							S_IFREG | 0444,
							votable->root, votable,
							&mca_votable_force_ops);
	if (!votable->force_active_ent) {
		mca_log_err("Couldn't create force_active dbg file for %s\n",
			    name);
		goto err_debugfs;
	}

	votable->default_val = default_val;

	return votable;

err_debugfs:
	mca_votable_unlist(votable);
	debugfs_remove(votable->root);
	kfree(votable->name);
	kfree(votable);

	return ERR_PTR(-EEXIST);
}
EXPORT_SYMBOL(mca_create_votable);

void mca_destroy_votable(struct mca_votable *votable)
{
	int i;

	if (!votable)
		return;

	mca_votable_unlist(votable);

	debugfs_remove(votable->root);

	for (i = 0; i < votable->num_clients && votable->client_strs[i]; i++)
		kfree(votable->client_strs[i]);

	kfree(votable->name);
	kfree(votable);
}
EXPORT_SYMBOL(mca_destroy_votable);

MODULE_DESCRIPTION("mca voter");
MODULE_AUTHOR("getian@xiaomi.com");
MODULE_LICENSE("GPL v2");
