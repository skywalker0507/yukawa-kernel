// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 BayLibre, SAS.
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/phy.h>
#include <linux/mdio-mux.h>
#include <linux/clk.h>

#define ETH_PHY_CNTL0	0x0
#define ETH_PHY_CNTL1	0x4
#define ETH_PHY_CNTL2	0x8

#define MESON_G12A_MDIO_EXTERNAL_ID 0
#define MESON_G12A_MDIO_INTERNAL_ID 1

struct mdio_mux_g12a {
	void *mux_handle;
	void __iomem *regs;
	struct clk *pll;
	bool pll_is_enabled;
};

static int mdio_mux_g12a_enable_internal(struct device *dev)
{
	struct mdio_mux_g12a *priv = dev_get_drvdata(dev);
	int ret;

	if (IS_ERR(priv->pll)) {
		dev_err(dev, "failed to enable phy - missing pll\n");
		return PTR_ERR(priv->pll);
	}

	ret = clk_prepare_enable(priv->pll);
	if (ret) {
		dev_err(dev, "failed to enable pll\n");
		return ret;
	}

	priv->pll_is_enabled = true;
	writel_relaxed(0x33000180, priv->regs + ETH_PHY_CNTL0);
	writel_relaxed(0x00074043, priv->regs + ETH_PHY_CNTL1);
	writel_relaxed(0x00000260, priv->regs + ETH_PHY_CNTL2);

	return 0;
}

static int mdio_mux_g12a_enable_external(struct device *dev)
{
	struct mdio_mux_g12a *priv = dev_get_drvdata(dev);

	if (!IS_ERR(priv->pll) && priv->pll_is_enabled) {
		clk_disable_unprepare(priv->pll);
		priv->pll_is_enabled = false;
	}

	writel_relaxed(0x0, priv->regs + ETH_PHY_CNTL2);

	return 0;
}


static int mdio_mux_g12a_switch_fn(int current_child, int desired_child,
				   void *data)
{
	struct device *dev = data;

	if (current_child == desired_child)
		return 0;

	switch (desired_child) {
	case MESON_G12A_MDIO_EXTERNAL_ID:
		return mdio_mux_g12a_enable_external(dev);
	case MESON_G12A_MDIO_INTERNAL_ID:
		return mdio_mux_g12a_enable_internal(dev);
	default:
		return -EINVAL;
	}
}

static const struct of_device_id mdio_mux_g12a_match[] = {
	{ .compatible = "amlogic,g12a-mdio-mux", },
	{},
};
MODULE_DEVICE_TABLE(of, mdio_mux_g12a_match);

static int mdio_mux_g12a_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct mdio_mux_g12a *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cntl");
	priv->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->regs))
		return PTR_ERR(priv->regs);

	priv->pll = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->pll)) {
		/* We may not need the PLL, so wait before complaining */
		if (PTR_ERR(priv->pll) == -EPROBE_DEFER)
			return PTR_ERR(priv->pll);
	}

	return mdio_mux_init(dev, dev->of_node, mdio_mux_g12a_switch_fn,
			     &priv->mux_handle, dev, NULL);
}

static int mdio_mux_g12a_remove(struct platform_device *pdev)
{
	struct mdio_mux_g12a *priv = platform_get_drvdata(pdev);

	mdio_mux_uninit(priv->mux_handle);

	if (!IS_ERR(priv->pll) && priv->pll_is_enabled)
		clk_disable_unprepare(priv->pll);

	return 0;
}

static struct platform_driver mdio_mux_g12a_driver = {
	.probe		= mdio_mux_g12a_probe,
	.remove		= mdio_mux_g12a_remove,
	.driver		= {
		.name	= "mdio-mux-g12a",
		.of_match_table = mdio_mux_g12a_match,
	},
};
module_platform_driver(mdio_mux_g12a_driver);

MODULE_DESCRIPTION("Amlogic G12a mdio mux");
MODULE_AUTHOR("Jerome Brunet <jbrunet@baylibre.com>");
MODULE_LICENSE("GPL v2");
