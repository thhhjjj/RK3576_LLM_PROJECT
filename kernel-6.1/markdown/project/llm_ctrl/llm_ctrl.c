// SPDX-License-Identifier: GPL-2.0
/*
 * llm_ctrl — PHASE1 NPU thermal helper (platform + sysfs + genetlink)
 *
 * Reads existing thermal zone type "npu-thermal" (milli-Celsius).
 * Exposes Celsius via sysfs; multicast genetlink on over-threshold.
 * Does not map TSADC or register a second sensor.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <net/genetlink.h>

#include "llm_ctrl.h"

#define LLM_CTRL_POLL_MS		1000
#define LLM_CTRL_HYST_C			2
#define LLM_CTRL_ZONE_TYPE		"npu-thermal"

static int threshold_c = LLM_CTRL_DEFAULT_THRESHOLD_C;
module_param(threshold_c, int, 0644);
MODULE_PARM_DESC(threshold_c, "Over-temp warn threshold in Celsius (default 85)");

struct llm_ctrl {
	struct device			*dev;
	struct thermal_zone_device	*tz;
	struct delayed_work		poll_work;
	struct mutex			lock;
	int				temp_c;
	int				threshold_c;
	bool				alarmed;
};

static struct llm_ctrl *g_ctrl;
static struct platform_device *g_pdev;

static const struct genl_multicast_group llm_ctrl_mcgrps[] = {
	{ .name = LLM_CTRL_MCGRP_EVENTS, },
};

static struct genl_family llm_ctrl_gnl_family;

static int llm_ctrl_read_temp_c(struct llm_ctrl *ctrl, int *out_c)
{
	int temp_mc;
	int ret;

	ret = thermal_zone_get_temp(ctrl->tz, &temp_mc);
	if (ret)
		return ret;

	*out_c = temp_mc / 1000;
	return 0;
}

static int llm_ctrl_send_overtemp(struct llm_ctrl *ctrl, int temp_c)
{
	struct sk_buff *skb;
	void *hdr;
	int ret;

	skb = genlmsg_new(GENLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	hdr = genlmsg_put(skb, 0, 0, &llm_ctrl_gnl_family, 0,
			  LLM_CTRL_CMD_OVERTEMP);
	if (!hdr) {
		nlmsg_free(skb);
		return -EMSGSIZE;
	}

	if (nla_put_s32(skb, LLM_CTRL_A_TEMP_C, temp_c) ||
	    nla_put_s32(skb, LLM_CTRL_A_THRESHOLD_C, ctrl->threshold_c)) {
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return -EMSGSIZE;
	}

	genlmsg_end(skb, hdr);
	ret = genlmsg_multicast(&llm_ctrl_gnl_family, skb, 0, 0, GFP_KERNEL);
	/* No listeners is fine during bring-up. */
	if (ret == -ESRCH)
		ret = 0;
	return ret;
}

static void llm_ctrl_poll(struct work_struct *work)
{
	struct llm_ctrl *ctrl = container_of(to_delayed_work(work),
					     struct llm_ctrl, poll_work);
	int temp_c;
	int thr;
	int ret;

	ret = llm_ctrl_read_temp_c(ctrl, &temp_c);
	if (ret) {
		dev_warn_ratelimited(ctrl->dev, "read temp failed: %d\n", ret);
		goto resched;
	}

	mutex_lock(&ctrl->lock);
	ctrl->temp_c = temp_c;
	thr = ctrl->threshold_c;

	if (!ctrl->alarmed && temp_c >= thr) {
		ctrl->alarmed = true;
		mutex_unlock(&ctrl->lock);
		dev_warn(ctrl->dev, "NPU overtemp: %d C (threshold %d C)\n",
			 temp_c, thr);
		llm_ctrl_send_overtemp(ctrl, temp_c);
	} else if (ctrl->alarmed && temp_c < thr - LLM_CTRL_HYST_C) {
		ctrl->alarmed = false;
		mutex_unlock(&ctrl->lock);
		dev_info(ctrl->dev, "NPU temp recovered: %d C\n", temp_c);
	} else {
		mutex_unlock(&ctrl->lock);
	}

resched:
	schedule_delayed_work(&ctrl->poll_work,
			      msecs_to_jiffies(LLM_CTRL_POLL_MS));
}

static ssize_t temp_c_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct llm_ctrl *ctrl = dev_get_drvdata(dev);
	int temp_c;
	int ret;

	ret = llm_ctrl_read_temp_c(ctrl, &temp_c);
	if (ret)
		return ret;

	mutex_lock(&ctrl->lock);
	ctrl->temp_c = temp_c;
	mutex_unlock(&ctrl->lock);

	return sysfs_emit(buf, "%d\n", temp_c);
}
static DEVICE_ATTR_RO(temp_c);

static ssize_t threshold_c_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct llm_ctrl *ctrl = dev_get_drvdata(dev);
	int thr;

	mutex_lock(&ctrl->lock);
	thr = ctrl->threshold_c;
	mutex_unlock(&ctrl->lock);

	return sysfs_emit(buf, "%d\n", thr);
}

static ssize_t threshold_c_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct llm_ctrl *ctrl = dev_get_drvdata(dev);
	int val;
	int ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;
	if (val < 0 || val >= 115)
		return -EINVAL;

	mutex_lock(&ctrl->lock);
	ctrl->threshold_c = val;
	ctrl->alarmed = false;
	mutex_unlock(&ctrl->lock);

	return count;
}
static DEVICE_ATTR_RW(threshold_c);

static struct attribute *llm_ctrl_attrs[] = {
	&dev_attr_temp_c.attr,
	&dev_attr_threshold_c.attr,
	NULL,
};
ATTRIBUTE_GROUPS(llm_ctrl);

static int llm_ctrl_probe(struct platform_device *pdev)
{
	struct llm_ctrl *ctrl;
	struct thermal_zone_device *tz;
	int temp_c;
	int ret;

	tz = thermal_zone_get_zone_by_name(LLM_CTRL_ZONE_TYPE);
	if (IS_ERR(tz)) {
		dev_err(&pdev->dev, "thermal zone '%s' not found: %ld\n",
			LLM_CTRL_ZONE_TYPE, PTR_ERR(tz));
		return PTR_ERR(tz);
	}

	ctrl = devm_kzalloc(&pdev->dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->dev = &pdev->dev;
	ctrl->tz = tz;
	ctrl->threshold_c = threshold_c;
	mutex_init(&ctrl->lock);
	INIT_DELAYED_WORK(&ctrl->poll_work, llm_ctrl_poll);

	ret = llm_ctrl_read_temp_c(ctrl, &temp_c);
	if (ret) {
		dev_err(&pdev->dev, "initial temp read failed: %d\n", ret);
		return ret;
	}
	ctrl->temp_c = temp_c;

	platform_set_drvdata(pdev, ctrl);
	g_ctrl = ctrl;

	ret = sysfs_create_groups(&pdev->dev.kobj, llm_ctrl_groups);
	if (ret)
		return ret;

	schedule_delayed_work(&ctrl->poll_work,
			      msecs_to_jiffies(LLM_CTRL_POLL_MS));

	dev_info(&pdev->dev,
		 "ready: zone=%s temp=%d C threshold=%d C\n",
		 LLM_CTRL_ZONE_TYPE, temp_c, ctrl->threshold_c);
	return 0;
}

static int llm_ctrl_remove(struct platform_device *pdev)
{
	struct llm_ctrl *ctrl = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&ctrl->poll_work);
	sysfs_remove_groups(&pdev->dev.kobj, llm_ctrl_groups);
	g_ctrl = NULL;
	return 0;
}

static struct platform_driver llm_ctrl_driver = {
	.probe = llm_ctrl_probe,
	.remove = llm_ctrl_remove,
	.driver = {
		.name = "llm_ctrl",
	},
};

static struct genl_family llm_ctrl_gnl_family = {
	.name		= LLM_CTRL_GENL_NAME,
	.version	= LLM_CTRL_GENL_VERSION,
	.maxattr	= LLM_CTRL_A_MAX,
	.module		= THIS_MODULE,
	.mcgrps		= llm_ctrl_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(llm_ctrl_mcgrps),
};

static int __init llm_ctrl_init(void)
{
	int ret;

	ret = genl_register_family(&llm_ctrl_gnl_family);
	if (ret)
		return ret;

	ret = platform_driver_register(&llm_ctrl_driver);
	if (ret)
		goto err_genl;

	g_pdev = platform_device_register_simple("llm_ctrl", -1, NULL, 0);
	if (IS_ERR(g_pdev)) {
		ret = PTR_ERR(g_pdev);
		g_pdev = NULL;
		goto err_drv;
	}

	return 0;

err_drv:
	platform_driver_unregister(&llm_ctrl_driver);
err_genl:
	genl_unregister_family(&llm_ctrl_gnl_family);
	return ret;
}

static void __exit llm_ctrl_exit(void)
{
	if (g_pdev)
		platform_device_unregister(g_pdev);
	platform_driver_unregister(&llm_ctrl_driver);
	genl_unregister_family(&llm_ctrl_gnl_family);
}

module_init(llm_ctrl_init);
module_exit(llm_ctrl_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Moments");
MODULE_DESCRIPTION("PHASE1 NPU thermal sysfs + genetlink helper");
MODULE_ALIAS("platform:llm_ctrl");
