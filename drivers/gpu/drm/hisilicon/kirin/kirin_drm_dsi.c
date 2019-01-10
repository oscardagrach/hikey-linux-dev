// SPDX-License-Identifier: GPL-2.0-only
/*
 * DesignWare MIPI DSI Host Controller v1.02 driver
 *
 * Copyright (c) 2016 Linaro Limited.
 * Copyright (c) 2014-2016 Hisilicon Limited.
 *
 * Author:
 *	<shizongxuan@huawei.com>
 *	<zhangxiubin@huawei.com>
 *	<lvda3@hisilicon.com>
 */
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder_slave.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_sysfs.h>

#include "kirin_drm_dsi.h"
#include "dw_dsi_reg.h"

static struct kirin_dsi_ops *hisi_dsi_ops;

void dsi_set_output_client(struct drm_device *dev)
{
	enum dsi_output_client client;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	struct drm_connector_list_iter conn_iter;
	struct dw_dsi *dsi;

	mutex_lock(&dev->mode_config.mutex);

	/* find dsi encoder */
	drm_for_each_encoder(encoder, dev)
		if (encoder->encoder_type == DRM_MODE_ENCODER_DSI)
			break;
	dsi = encoder_to_dsi(encoder);

	/* find HDMI connector */
	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter)
		if (connector->connector_type == DRM_MODE_CONNECTOR_HDMIA)
			break;
	drm_connector_list_iter_end(&conn_iter);

	/*
	 * set the proper dsi output client
	 */
	client = connector->status == connector_status_connected ? OUT_HDMI :
								   OUT_PANEL;
	if (client != dsi->cur_client) {
		/*
		 * set the switch ic to select the HDMI or MIPI_DSI
		 */
		if (hisi_dsi_ops->version == KIRIN960_DSI)
			gpiod_set_value_cansleep(dsi->gpio_mux, client);

		dsi->cur_client = client;
		/* let the userspace know panel connector status has changed */
		drm_sysfs_hotplug_event(dev);
		DRM_INFO("client change to %s\n",
			 client == OUT_HDMI ? "HDMI" : "panel");
	}

	mutex_unlock(&dev->mode_config.mutex);
}
EXPORT_SYMBOL_GPL(dsi_set_output_client);

/***************************for the encoder_helper_funcs****************************************/
static const struct drm_encoder_funcs dw_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int dsi_encoder_atomic_check(struct drm_encoder *encoder,
				    struct drm_crtc_state *crtc_state,
				    struct drm_connector_state *conn_state)
{
	/* do nothing */
	return 0;
}

static enum drm_mode_status
dsi_encoder_mode_valid(struct drm_encoder *encoder,
		       const struct drm_display_mode *mode)

{
	return hisi_dsi_ops->encoder_valid(encoder, mode);
}

static void dsi_encoder_mode_set(struct drm_encoder *encoder,
				 struct drm_display_mode *mode,
				 struct drm_display_mode *adj_mode)
{
	struct dw_dsi *dsi = encoder_to_dsi(encoder);

	drm_mode_copy(&dsi->cur_mode, adj_mode);
}

static void dsi_encoder_enable(struct drm_encoder *encoder)
{
	struct dw_dsi *dsi = encoder_to_dsi(encoder);

	if (dsi->enable)
		return;

	hisi_dsi_ops->encoder_enable(encoder);

	dsi->enable = true;
}

static void dw_dsi_set_mode(struct dw_dsi *dsi, enum dsi_work_mode mode)
{
	struct dsi_hw_ctx *ctx = dsi->ctx;
	void __iomem *base = ctx->base;

	writel(RESET, base + PWR_UP);
	writel(mode, base + MODE_CFG);
	writel(POWERUP, base + PWR_UP);
}

static void dsi_encoder_disable(struct drm_encoder *encoder)
{
	struct dw_dsi *dsi = encoder_to_dsi(encoder);
	struct dsi_hw_ctx *ctx = dsi->ctx;

	if (!dsi->enable)
		return;

	dw_dsi_set_mode(dsi, DSI_COMMAND_MODE);

	dsi->enable = false;
}

static const struct drm_encoder_helper_funcs dw_encoder_helper_funcs = {
	.atomic_check = dsi_encoder_atomic_check,
	.mode_valid = dsi_encoder_mode_valid,
	.mode_set = dsi_encoder_mode_set,
	.enable = dsi_encoder_enable,
	.disable = dsi_encoder_disable
};

/****************************************************************************/
static int dsi_bridge_init(struct drm_device *dev, struct dw_dsi *dsi)
{
	struct drm_encoder *encoder = &dsi->encoder;
	struct drm_bridge *bridge;
	struct device_node *np = dsi->dev->of_node;
	int ret;

	/*
	 * Get the endpoint node. In our case, dsi has one output port1
	 * to which the external HDMI bridge is connected.
	 */
	ret = drm_of_find_panel_or_bridge(np, 1, 0, NULL, &bridge);
	if (ret)
		return ret;

	/* associate the bridge to dsi encoder */
	return drm_bridge_attach(encoder, bridge, NULL, 0);
}

static int dw_drm_encoder_init(struct device *dev, struct drm_device *drm_dev,
			       struct drm_encoder *encoder)
{
	int ret;
	u32 crtc_mask = drm_of_find_possible_crtcs(drm_dev, dev->of_node);

	if (!crtc_mask) {
		DRM_ERROR("failed to find crtc mask\n");
		return -EINVAL;
	}

	encoder->possible_crtcs = crtc_mask;
	ret = drm_encoder_init(drm_dev, encoder, &dw_encoder_funcs,
			       DRM_MODE_ENCODER_DSI, NULL);
	if (ret) {
		DRM_ERROR("failed to init dsi encoder\n");
		return ret;
	}

	drm_encoder_helper_add(encoder, &dw_encoder_helper_funcs);

	return 0;
}

static int dsi_bind(struct device *dev, struct device *master, void *data)
{
	struct dsi_data *ddata = dev_get_drvdata(dev);
	struct dw_dsi *dsi = &ddata->dsi;
	struct drm_device *drm_dev = data;
	int ret;

	ret = dw_drm_encoder_init(dev, drm_dev, &dsi->encoder);
	if (ret)
		return ret;

	ret = dsi_bridge_init(drm_dev, dsi);
	if (ret)
		return ret;

	return 0;
}

static void dsi_unbind(struct device *dev, struct device *master, void *data)
{
	/* do nothing */
}

const struct component_ops kdsi_ops = {
	.bind = dsi_bind,
	.unbind = dsi_unbind,
};

static int dsi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dsi_data *data;
	struct dw_dsi *dsi;
	struct dsi_hw_ctx *ctx;
	int ret;

	hisi_dsi_ops = (struct kirin_dsi_ops *)of_device_get_match_data(dev);

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		DRM_ERROR("failed to allocate dsi data.\n");
		return -ENOMEM;
	}
	dsi = &data->dsi;
	ctx = &data->ctx;
	dsi->ctx = ctx;
	dsi->dev = &pdev->dev;

	if (hisi_dsi_ops == NULL)
		DRM_ERROR("hisi_dsi_ops is not bind\n");
	ret = hisi_dsi_ops->host_init(dev, dsi);
	if (ret)
		return ret;

	ret = hisi_dsi_ops->parse_dt(pdev, dsi);
	if (ret)
		goto err_host_unregister;

	platform_set_drvdata(pdev, data);

	return 0;

err_host_unregister:
	mipi_dsi_host_unregister(&dsi->host);
	return ret;
}

static int dsi_remove(struct platform_device *pdev)
{
	struct dsi_data *data = platform_get_drvdata(pdev);
	struct dw_dsi *dsi = &data->dsi;

	mipi_dsi_host_unregister(&dsi->host);

	return 0;
}

static const struct of_device_id dsi_of_match[] = {
#ifdef CONFIG_DRM_HISI_KIRIN960
	{
		.compatible = "hisilicon,hi3660-dsi",
		.data = &kirin_dsi_960,
	},
#endif
#ifdef CONFIG_DRM_HISI_KIRIN620
	{
		.compatible = "hisilicon,hi6220-dsi",
		.data = &kirin_dsi_620,
	},
#endif
	{ /* end node */ }
};
MODULE_DEVICE_TABLE(of, dsi_of_match);

static struct platform_driver dsi_driver = {
	.probe = dsi_probe,
	.remove = dsi_remove,
	.driver = {
		.name = "dw-dsi",
		.of_match_table = dsi_of_match,
	},
};

module_platform_driver(dsi_driver);

MODULE_DESCRIPTION("DesignWare MIPI DSI Host Controller v1.02 driver");
MODULE_LICENSE("GPL v2");
