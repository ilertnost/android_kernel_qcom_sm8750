// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Talking to the ADSP about charging.  See
 * include/mca/common/mca_adsp_glink.h.
 */

#define MCA_LOG_TAG "mca_adsp_glink"

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <mca/common/mca_adsp_glink.h>
#include <mca/common/mca_log.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/qcom/qti_pmic_glink.h>
#include <linux/string.h>
#include <linux/workqueue.h>

/* Who the ADSP thinks it is talking to. */
#define MCA_GLINK_MSG_OWNER		0x800A
#define MCA_GLINK_QBG_MSG_OWNER		0x8009

#define MCA_GLINK_MSG_TYPE_REQ_RESP	1
#define MCA_GLINK_MSG_TYPE_NOTIFY	2

#define MCA_GLINK_OPCODE_READ		1
#define MCA_GLINK_OPCODE_WRITE		2

/* How long to wait for the ADSP to answer. */
#define MCA_GLINK_TIMEOUT_MS		1000

/**
 * struct mca_glink_req_msg - a request for one property
 * @hdr:         who it is for and what kind of message it is
 * @property_id: which property
 * @seq_num:     matched against the response
 * @data:        the value, when writing
 */
struct mca_glink_req_msg {
	struct pmic_glink_hdr	hdr;
	u32			property_id;
	u32			seq_num;
	u8			data[MCA_GLINK_DATA_MAX];
};

/**
 * struct mca_glink_resp_msg - the answer
 * @hdr:         who it is from
 * @property_id: which property it answers about
 * @ret_code:    what the ADSP made of the request
 * @seq_num:     which request it answers
 * @data:        the value
 */
struct mca_glink_resp_msg {
	struct pmic_glink_hdr	hdr;
	u32			property_id;
	u32			ret_code;
	u32			seq_num;
	u8			data[MCA_GLINK_DATA_MAX];
};

/**
 * struct mca_glink_nty_msg - something the ADSP volunteered
 * @hdr:          who it is from
 * @notification: what happened
 * @data:         what goes with it
 */
struct mca_glink_nty_msg {
	struct pmic_glink_hdr	hdr;
	u32			notification;
	u8			data[MCA_GLINK_DATA_MAX];
};

/**
 * struct mca_adsp_glink_ops_list_node - one registered listener
 * @node: links it into the listener list
 * @data: handed back to its callbacks
 * @cb:   what it wants to hear about
 */
struct mca_adsp_glink_ops_list_node {
	struct list_head		node;
	void				*data;
	struct mca_adsp_call_back	*cb;
};

/**
 * struct mca_adsp_glink_data - the channels to the ADSP
 * @dev:            this device
 * @client:         the charging channel, which carries every request
 * @qbg_client:     the gauge channel, which receives its owner's messages
 * @rw_lock:        one exchange at a time, so a response matches the request
 * @ack:            completed when the response arrives
 * @state:          the pmic_glink state, PMIC_GLINK_STATE_UP while usable
 * @adsp_sync_work: tells the listeners to send their state again
 * @pending_prop:   the property being waited on, -1 when idle
 * @ret_code:       what the ADSP made of the last request
 * @seq_num:        incremented per request
 * @data:           where the last read response's value was copied
 */
struct mca_adsp_glink_data {
	struct device			*dev;
	struct pmic_glink_client	*client;
	struct pmic_glink_client	*qbg_client;
	struct mutex			rw_lock;
	struct completion		ack;
	int				state;
	struct work_struct		adsp_sync_work;
	u32				pending_prop;
	int				ret_code;
	u32				seq_num;
	u8				data[MCA_GLINK_DATA_MAX];
};

static struct mca_adsp_glink_data *g_mca_adsp_glink;

/*
 * Listeners register while their drivers probe and are never removed, so the
 * lists are only ever appended to.
 */
static LIST_HEAD(g_mca_adsp_glink_ops_head);
static LIST_HEAD(g_mca_adsp_glink_qbg_ops_head);

static int mca_adsp_glink_register(struct list_head *list,
				   struct mca_adsp_call_back *cb, void *data)
{
	struct mca_adsp_glink_ops_list_node *node;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	node->data = data;
	node->cb = cb;
	list_add_tail(&node->node, list);

	return 0;
}

int mca_adsp_glink_resister_ops(struct mca_adsp_call_back *cb, void *data)
{
	return mca_adsp_glink_register(&g_mca_adsp_glink_ops_head, cb, data);
}
EXPORT_SYMBOL(mca_adsp_glink_resister_ops);

int mca_adsp_glink_qbg_resister_ops(struct mca_adsp_call_back *cb, void *data)
{
	return mca_adsp_glink_register(&g_mca_adsp_glink_qbg_ops_head, cb, data);
}
EXPORT_SYMBOL(mca_adsp_glink_qbg_resister_ops);

/*
 * One exchange at a time.  The response carries the sequence number of the
 * request it answers, so a late answer to a request that already timed out is
 * not taken for the answer to the current one.
 *
 * Every request goes out on the charging channel; @owner says which of the
 * two the ADSP is to answer on.
 *
 * Return: 0, the ADSP's own non-zero return code, or a negative error.
 */
static __always_inline int mca_adsp_glink_write(u32 owner, u32 opcode,
						u32 property_id, void *data,
						int len)
{
	struct mca_adsp_glink_data *gd = g_mca_adsp_glink;
	struct mca_glink_req_msg msg = { 0 };
	int ret;

	if (!data || (unsigned int)len >= MCA_GLINK_DATA_MAX || !gd)
		return -1;

	mutex_lock(&gd->rw_lock);

	msg.hdr.owner = owner;
	msg.hdr.type = MCA_GLINK_MSG_TYPE_REQ_RESP;
	msg.hdr.opcode = opcode;
	msg.property_id = property_id;
	msg.seq_num = ++gd->seq_num;

	if (opcode == MCA_GLINK_OPCODE_WRITE)
		memcpy(msg.data, data, len);

	if (!gd->state) {
		mca_log_err("glink state is down\n");
		ret = -ENOTCONN;
		goto out;
	}

	reinit_completion(&gd->ack);
	gd->pending_prop = property_id;

	ret = pmic_glink_write(gd->client, &msg, sizeof(msg));
	if (ret)
		goto out;

	if (!wait_for_completion_timeout(&gd->ack,
					 msecs_to_jiffies(MCA_GLINK_TIMEOUT_MS))) {
		mca_log_err("timed out sending message, prop_id: %d\n",
			    gd->pending_prop);
		ret = -ETIMEDOUT;
	} else {
		ret = gd->ret_code;
	}

	gd->pending_prop = -1;
	gd->ret_code = 0;

	if (!ret && opcode == MCA_GLINK_OPCODE_READ) {
		memcpy(data, gd->data, len);
		memset(gd->data, 0, sizeof(gd->data));
	}

out:
	mutex_unlock(&gd->rw_lock);

	return ret;
}

int mca_adsp_glink_read_prop(u32 property_id, void *data, int len)
{
	return mca_adsp_glink_write(MCA_GLINK_MSG_OWNER, MCA_GLINK_OPCODE_READ,
				    property_id, data, len);
}
EXPORT_SYMBOL(mca_adsp_glink_read_prop);

int mca_adsp_glink_write_prop(u32 property_id, void *data, int len)
{
	return mca_adsp_glink_write(MCA_GLINK_MSG_OWNER, MCA_GLINK_OPCODE_WRITE,
				    property_id, data, len);
}
EXPORT_SYMBOL(mca_adsp_glink_write_prop);

int mca_adsp_glink_qbg_read_prop(u32 property_id, void *data, int len)
{
	return mca_adsp_glink_write(MCA_GLINK_QBG_MSG_OWNER,
				    MCA_GLINK_OPCODE_READ, property_id, data,
				    len);
}
EXPORT_SYMBOL(mca_adsp_glink_qbg_read_prop);

int mca_adsp_glink_qbg_write_prop(u32 property_id, void *data, int len)
{
	return mca_adsp_glink_write(MCA_GLINK_QBG_MSG_OWNER,
				    MCA_GLINK_OPCODE_WRITE, property_id, data,
				    len);
}
EXPORT_SYMBOL(mca_adsp_glink_qbg_write_prop);

static void mca_adsp_glink_handle_message(struct mca_adsp_glink_data *gd,
					  struct mca_glink_resp_msg *msg,
					  size_t len)
{
	u32 ret_code;

	/* A message too short to carry a return code is a failed request. */
	ret_code = len >= offsetof(struct mca_glink_resp_msg, seq_num) ?
		   msg->ret_code : (u32)-EINVAL;

	if (len == sizeof(*msg) && !ret_code) {
		if (msg->seq_num != gd->seq_num) {
			mca_log_err("invalid seq_num: %d != %d\n",
				    gd->seq_num, msg->seq_num);
			return;
		}

		if (msg->hdr.opcode == MCA_GLINK_OPCODE_READ)
			memcpy(gd->data, msg->data, sizeof(gd->data));
	} else {
		mca_log_err("glink retcode error, property_id: %d, retcode: %d, len: %lu %lu\n",
			    len >= offsetof(struct mca_glink_resp_msg, ret_code) ?
			    msg->property_id : 0, ret_code, len,
			    sizeof(*msg));
		gd->ret_code = ret_code;
	}

	complete(&gd->ack);
}

static void mca_adsp_glink_handle_notification(struct mca_glink_nty_msg *msg,
					       size_t len)
{
	struct mca_adsp_glink_ops_list_node *node;
	u8 buf[MCA_GLINK_DATA_MAX] = { 0 };
	struct list_head *list;
	u32 notification = 0;

	list = msg->hdr.owner == MCA_GLINK_QBG_MSG_OWNER ?
	       &g_mca_adsp_glink_qbg_ops_head : &g_mca_adsp_glink_ops_head;

	/*
	 * The listeners are always handed a full buffer; whatever the ADSP
	 * did not send reads as zero.
	 */
	if (len >= offsetof(struct mca_glink_nty_msg, data))
		notification = msg->notification;
	if (len > offsetof(struct mca_glink_nty_msg, data))
		memcpy(buf, msg->data,
		       min_t(size_t, len - offsetof(struct mca_glink_nty_msg,
						    data), sizeof(buf)));

	mca_log_info("mca_adsp receive notification %d, owner %d\n",
		     notification, msg->hdr.owner);

	list_for_each_entry(node, list, node) {
		if (node->cb->notify_cb)
			node->cb->notify_cb(notification, buf, sizeof(buf),
					    node->data);
	}
}

static int mca_adsp_glink_callback(void *priv, void *data, size_t len)
{
	struct pmic_glink_hdr *hdr = data;

	if (hdr->type == MCA_GLINK_MSG_TYPE_NOTIFY)
		mca_adsp_glink_handle_notification(data, len);
	else
		mca_adsp_glink_handle_message(priv, data, len);

	return 0;
}

/* The ADSP is back and has to be told again what it lost. */
static void mca_adsp_glink_sync_work(struct work_struct *work)
{
	struct mca_adsp_glink_ops_list_node *node;

	list_for_each_entry(node, &g_mca_adsp_glink_ops_head, node) {
		if (node->cb->sync_data_cb)
			node->cb->sync_data_cb(node->data);
	}

	list_for_each_entry(node, &g_mca_adsp_glink_qbg_ops_head, node) {
		if (node->cb->sync_data_cb)
			node->cb->sync_data_cb(node->data);
	}
}

/*
 * pmic_glink calls this from process context.  Going down, the listeners are
 * told at once; coming back up, they are asked to resend from a worker, so
 * that they can talk to the ADSP again.
 */
static void mca_adsp_glink_state_cb(void *priv, enum pmic_glink_state state)
{
	struct mca_adsp_glink_data *gd = priv;
	struct mca_adsp_glink_ops_list_node *node;

	gd->state = state;

	if (state == PMIC_GLINK_STATE_UP) {
		schedule_work(&gd->adsp_sync_work);
		return;
	}

	list_for_each_entry(node, &g_mca_adsp_glink_ops_head, node) {
		if (node->cb->glink_down_cb)
			node->cb->glink_down_cb(node->data);
	}

	list_for_each_entry(node, &g_mca_adsp_glink_qbg_ops_head, node) {
		if (node->cb->glink_down_cb)
			node->cb->glink_down_cb(node->data);
	}
}

static int mca_adsp_glink_probe(struct platform_device *pdev)
{
	struct pmic_glink_client_data client_data = { 0 };
	struct pmic_glink_client_data qbg_client_data = { 0 };
	struct mca_adsp_glink_data *gd;
	int ret;

	gd = devm_kzalloc(&pdev->dev, sizeof(*gd), GFP_KERNEL);
	if (!gd)
		return -ENOMEM;

	gd->dev = &pdev->dev;
	gd->pending_prop = -1;
	mutex_init(&gd->rw_lock);
	init_completion(&gd->ack);
	gd->state = PMIC_GLINK_STATE_UP;
	INIT_WORK(&gd->adsp_sync_work, mca_adsp_glink_sync_work);

	client_data.name = "mca_adap_glink";
	client_data.id = MCA_GLINK_MSG_OWNER;
	client_data.priv = gd;
	client_data.msg_cb = mca_adsp_glink_callback;
	client_data.state_cb = mca_adsp_glink_state_cb;

	gd->client = pmic_glink_register_client(gd->dev, &client_data);
	if (IS_ERR(gd->client)) {
		ret = PTR_ERR(gd->client);
		if (ret != -EPROBE_DEFER)
			mca_log_err("Error in registering with pmic_glink %d\n",
				    ret);
		return ret;
	}

	qbg_client_data.name = "mca_adap_qbg_glink";
	qbg_client_data.id = MCA_GLINK_QBG_MSG_OWNER;
	qbg_client_data.priv = gd;
	qbg_client_data.msg_cb = mca_adsp_glink_callback;
	qbg_client_data.state_cb = mca_adsp_glink_state_cb;

	gd->qbg_client = pmic_glink_register_client(gd->dev, &qbg_client_data);
	if (IS_ERR(gd->qbg_client)) {
		ret = PTR_ERR(gd->qbg_client);
		if (ret != -EPROBE_DEFER)
			mca_log_err("Error in registering with qbg_glink %d\n",
				    ret);
		/* The callbacks must not outlive the memory they are given. */
		pmic_glink_unregister_client(gd->client);
		return ret;
	}

	platform_set_drvdata(pdev, gd);
	g_mca_adsp_glink = gd;

	mca_log_err("probe ok\n");

	return 0;
}

static int mca_adsp_glink_remove(struct platform_device *pdev)
{
	struct mca_adsp_glink_data *gd = platform_get_drvdata(pdev);

	g_mca_adsp_glink = NULL;
	pmic_glink_unregister_client(gd->qbg_client);
	pmic_glink_unregister_client(gd->client);
	cancel_work_sync(&gd->adsp_sync_work);

	return 0;
}

static const struct of_device_id mca_adsp_match_table[] = {
	{ .compatible = "mca,adsp_glink" },
	{ }
};
MODULE_DEVICE_TABLE(of, mca_adsp_match_table);

static struct platform_driver mca_adsp_driver = {
	.driver = {
		.name		= "mca_adsp_glink",
		.of_match_table	= mca_adsp_match_table,
	},
	.probe		= mca_adsp_glink_probe,
	.remove		= mca_adsp_glink_remove,
};
module_platform_driver(mca_adsp_driver);

MODULE_DESCRIPTION("mca glink driver");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
