// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 The LineageOS Project
 *
 * Panel state for the charging drivers.
 *
 * Charging behaviour depends on whether the screen is on and whether it is in
 * high brightness mode.  This listens to the panel and announces both on the
 * MCA_EVENT_TYPE_PANEL chain, passing the new state as an int.
 *
 * The panel is found through the "panel" phandles of the charge-screen node.
 * Only one of them is the panel actually fitted, and it is not registered with
 * DRM until the display driver has probed, so the search is retried a few
 * times.
 */

#define MCA_LOG_TAG "mca_panel"

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/panel_event_notifier.h>
#include <linux/workqueue.h>

#include <drm/drm_panel.h>

#include <mca/common/mca_event.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_panel.h>

/* The display driver may probe well after this one. */
#define MCA_PANEL_REGISTER_DELAY_MS	5000
#define MCA_PANEL_RETRY_MAX		3

struct mca_panel {
	struct device		*dev;
	struct delayed_work	register_work;
	void			*cookie;
	int			screen_state;
	int			hbm_state;
};

/*
 * The state is announced to the listeners rather than answered here; these
 * report nothing for drivers that ask.
 */
int mca_panel_get_screen_state(void)
{
	return 0;
}
EXPORT_SYMBOL(mca_panel_get_screen_state);

int mca_panel_get_hbm_state(void)
{
	return 0;
}
EXPORT_SYMBOL(mca_panel_get_hbm_state);

static void mca_panel_event_notifier_callback(enum panel_event_notifier_tag tag,
					      struct panel_event_notification *notification,
					      void *pvt_data)
{
	struct mca_panel *mp = pvt_data;
	unsigned long event;
	int *state;

	if (!notification) {
		mca_log_debug("Invalid panel notification\n");
		return;
	}

	mca_log_info("panel event received, type: %d\n",
		     notification->notif_type);

	switch (notification->notif_type) {
	case DRM_PANEL_EVENT_HBM_ON:
	case DRM_PANEL_EVENT_HBM_OFF:
		mp->hbm_state =
			notification->notif_type == DRM_PANEL_EVENT_HBM_ON;
		state = &mp->hbm_state;
		event = MCA_EVENT_PANEL_HBM_STATE_CHANGE;
		break;
	case DRM_PANEL_EVENT_BLANK:
	case DRM_PANEL_EVENT_UNBLANK:
		mp->screen_state =
			notification->notif_type != DRM_PANEL_EVENT_BLANK;
		state = &mp->screen_state;
		event = MCA_EVENT_PANEL_SCREEN_STATE_CHANGE;
		break;
	default:
		mca_log_debug("Ignore panel event: %d\n",
			      notification->notif_type);
		return;
	}

	mca_event_block_notify(MCA_EVENT_TYPE_PANEL, event, state);
}

static void mca_panel_register_panel_notifier(struct mca_panel *mp)
{
	struct of_phandle_args args;
	struct device_node *node;
	struct drm_panel *panel = NULL;
	void *cookie;
	int count, i;

	node = of_find_node_by_name(NULL, "charge-screen");
	if (!node) {
		mca_log_err("ERROR: Cannot find node with panel!");
		return;
	}

	count = of_count_phandle_with_args(node, "panel", NULL);
	if (count < 1)
		goto out;

	for (i = 0; i < count; i++) {
		memset(&args, 0, sizeof(args));
		if (of_parse_phandle_with_args(node, "panel", NULL, i, &args) ||
		    !args.np)
			goto out;

		panel = of_drm_find_panel(args.np);
		of_node_put(args.np);
		if (!IS_ERR(panel))
			break;
	}

	if (i == count) {
		if (PTR_ERR(panel) != -EPROBE_DEFER)
			mca_log_err("Failed to find active panel, rc=%d\n",
				    (int)PTR_ERR(panel));
		goto out;
	}

	if (!panel) {
		mca_log_err("Failed to find active panel, rc=%d\n", 0);
		goto out;
	}

	cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,
					       PANEL_EVENT_NOTIFIER_CLIENT_BATTERY_CHARGER,
					       panel,
					       mca_panel_event_notifier_callback,
					       mp);
	if (IS_ERR(cookie)) {
		mca_log_err("Failed to register panel event notifier, rc=%d\n",
			    (int)PTR_ERR(cookie));
		goto out;
	}

	mca_log_err("register panel notifier successful\n");
	mp->cookie = cookie;

out:
	of_node_put(node);
}

static void mca_panel_register_panel_notifier_work(struct work_struct *work)
{
	struct mca_panel *mp = container_of(to_delayed_work(work),
					    struct mca_panel, register_work);
	static int retry_count = MCA_PANEL_RETRY_MAX;

	mca_panel_register_panel_notifier(mp);

	if (retry_count-- && !mp->cookie) {
		mca_log_err("retry register panel notifier, retry_count = %d\n",
			    retry_count);
		queue_delayed_work(system_wq, &mp->register_work,
				   msecs_to_jiffies(MCA_PANEL_REGISTER_DELAY_MS));
	}
}

static int mca_panel_probe(struct platform_device *pdev)
{
	static int probe_cnt;
	struct mca_panel *mp;

	mca_log_info("probe_cnt = %d\n", ++probe_cnt);

	mp = devm_kzalloc(&pdev->dev, sizeof(*mp), GFP_KERNEL);
	if (!mp) {
		mca_log_err("out of memory\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, mp);
	mp->dev = &pdev->dev;
	INIT_DELAYED_WORK(&mp->register_work,
			  mca_panel_register_panel_notifier_work);

	/* DRM registers the panel only once the display driver has probed. */
	queue_delayed_work(system_wq, &mp->register_work,
			   msecs_to_jiffies(MCA_PANEL_REGISTER_DELAY_MS));

	mca_log_err("probe OK");

	return 0;
}

static int mca_panel_remove(struct platform_device *pdev)
{
	struct mca_panel *mp = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&mp->register_work);

	if (mp->cookie)
		panel_event_notifier_unregister(mp->cookie);

	devm_kfree(&pdev->dev, mp);

	return 0;
}

static void mca_panel_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,mca_panel" },
	{ }
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver mca_panel_driver = {
	.driver = {
		.name = "mca_panel",
		.of_match_table = match_table,
	},
	.probe = mca_panel_probe,
	.remove = mca_panel_remove,
	.shutdown = mca_panel_shutdown,
};
module_platform_driver(mca_panel_driver);

MODULE_DESCRIPTION("mca get panel event");
MODULE_AUTHOR("muxinyi1@xiaomi.com");
MODULE_LICENSE("GPL v2");
