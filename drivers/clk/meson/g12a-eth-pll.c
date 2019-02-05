// SPDX-License-Identifier: GPL-2.0
/*
 * Amlogic Meson-G12A Ethernet PLL clock
 *
 * Copyright (c) 2019 Baylibre SAS.
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/init.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "clk-regmap.h"
#include "clk-pll.h"

#define ETH_PLL_STS	0x00
#define ETH_PLL_CTL0	0x04
#define ETH_PLL_CTL1	0x08
#define ETH_PLL_CTL2	0x0c
#define ETH_PLL_CTL3	0x10
#define ETH_PLL_CTL4	0x14
#define ETH_PLL_CTL5	0x18
#define ETH_PLL_CTL6	0x1c
#define ETH_PLL_CTL7	0x20

static const struct pll_params_table g12a_eth_pll_params_table[] = {
	PLL_PARAMS(10, 1),
};

static const struct reg_sequence g12a_eth_pll_init_regs[] = {
	{ .reg = ETH_PLL_CTL0,	.def = 0x09c0040a },
	{ .reg = ETH_PLL_CTL1,	.def = 0x927e0000 },
	{ .reg = ETH_PLL_CTL2,	.def = 0xac5f49e5 },
	{ .reg = ETH_PLL_CTL3,	.def = 0x00000000 },
	{ .reg = ETH_PLL_CTL4,	.def = 0x00000000 },
	{ .reg = ETH_PLL_CTL5,	.def = 0x20200000 },
	{ .reg = ETH_PLL_CTL6,	.def = 0x0000c002 },
	{ .reg = ETH_PLL_CTL7,	.def = 0x00000023 },
};

static const struct meson_clk_pll_data g12a_eth_pll_data = {
	.en = {
		.reg_off = ETH_PLL_CTL0,
		.shift   = 28,
		.width   = 1,
	},
	.m = {
		.reg_off = ETH_PLL_CTL0,
		.shift   = 0,
		.width   = 9,
	},
	.n = {
		.reg_off = ETH_PLL_CTL0,
		.shift   = 10,
		.width   = 5,
	},
	.frac = {
		.reg_off = ETH_PLL_CTL1,
		.shift   = 0,
		.width   = 12,
	},
	.l = {
		.reg_off = ETH_PLL_CTL0,
		.shift   = 31,
		.width   = 1,
	},
	.rst = {
		.reg_off = ETH_PLL_CTL0,
		.shift   = 29,
		.width   = 1,
	},
	.table = g12a_eth_pll_params_table,
	.init_regs = g12a_eth_pll_init_regs,
	.init_count = ARRAY_SIZE(g12a_eth_pll_init_regs),
	.flags = CLK_MESON_PLL_ROUND_CLOSEST,
};

static const struct clk_regmap_mux_data g12a_eth_pll_mux_data = {
	.offset = ETH_PLL_CTL0,
	.mask = 0x1,
	.shift = 23,
};

#define G12A_ETH_PLL_NUM_PARENT 2

static struct clk_hw *g12a_eth_pll_register_clk(struct device *dev,
						struct regmap *map,
						const char *name,
						const void *data,
						const struct clk_ops *ops,
						const char **parent_names,
						unsigned int num_parents,
						unsigned int flags)
{
	struct clk_regmap *clk;
	struct clk_init_data init;
	char *clk_name;
	int ret;

	clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return ERR_PTR(-ENOMEM);

	clk_name = kasprintf(GFP_KERNEL, "%s#%s", dev_name(dev), name);
	if (!clk_name)
		return ERR_PTR(-ENOMEM);

	init.name = clk_name;
	init.ops = ops;
	init.flags = flags;
	init.parent_names = parent_names;
	init.num_parents = num_parents;

	clk->data = (void *)data;
	clk->hw.init = &init;
	clk->map = map;

	ret = devm_clk_hw_register(dev, &clk->hw);
	if (ret && ret != -EPROBE_DEFER)
		dev_err(dev, "failed to register %s", init.name);

	kfree(clk_name);

	return &clk->hw;
}

static const struct regmap_config g12a_eth_pll_regmap_cfg = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= ETH_PLL_CTL3,
};

static int g12a_eth_pll_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *map;
	struct resource *res;
	void __iomem *regs;
	const char *parent_names[G12A_ETH_PLL_NUM_PARENT];
	struct clk_hw *hw;
	int i;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	map = devm_regmap_init_mmio(dev, regs, &g12a_eth_pll_regmap_cfg);
	if (IS_ERR(map)) {
		dev_err(dev, "failed to init regmap: %ld\n", PTR_ERR(map));
		return PTR_ERR(map);
	}

	/* Gather input names */
	for (i = 0; i < G12A_ETH_PLL_NUM_PARENT; i++) {
		struct clk *clk;
		char input[8];

		snprintf(input, sizeof(input), "clkin%d", i);
		clk = devm_clk_get(dev, input);
		if (IS_ERR(clk)) {
			if (clk != ERR_PTR(-EPROBE_DEFER))
				dev_err(dev, "Missing clock %s\n", input);
			return PTR_ERR(clk);
		}

		parent_names[i] = __clk_get_name(clk);
	}

	/* Register input mux */
	hw = g12a_eth_pll_register_clk(dev, map, "mux",
				       &g12a_eth_pll_mux_data,
				       &clk_regmap_mux_ro_ops,
				       parent_names,
				       G12A_ETH_PLL_NUM_PARENT, 0);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	/* Register the pll dco */
	parent_names[0] = clk_hw_get_name(hw);
	hw = g12a_eth_pll_register_clk(dev, map, "dco",
				       &g12a_eth_pll_data,
				       &meson_clk_pll_ops,
				       parent_names, 1, 0);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, hw);
}

static const struct of_device_id clkc_match_table[] = {
	{ .compatible = "amlogic,g12a-eth-pll" },
	{}
};
MODULE_DEVICE_TABLE(of, clkc_match_table);

static struct platform_driver g12a_eth_pll_driver = {
	.probe		= g12a_eth_pll_probe,
	.driver		= {
		.name	= "g12a-eth-pll",
		.of_match_table = clkc_match_table,
	},
};
module_platform_driver(g12a_eth_pll_driver);

MODULE_DESCRIPTION("Amlogic G12a Ethernet PLL driver");
MODULE_AUTHOR("Jerome Brunet <jbrunet@baylibre.com>");
MODULE_LICENSE("GPL v2");
