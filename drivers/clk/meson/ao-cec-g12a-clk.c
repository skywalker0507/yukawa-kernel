// SPDX-License-Identifier: GPL-2.0+
/*
 * Amlogic Meson-G12A AO CEC Clock Divider Driver
 *
 * Copyright (c) 2019 Baylibre SAS.
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 */
#include <linux/module.h>
#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <linux/mfd/syscon.h>
#include <linux/clk/meson/providers.h>
#include <linux/clk-provider.h>

#include "clk-regmap.h"
#include "clk-dualdiv.h"
#include "clk-input.h"

#define IN_PREFIX "ao-cec-b-in-"

#define CECB_CLK_CNTL_REG0		0x00
#define CECB_CLK_CNTL_REG1		0x04

static const struct meson_clk_dualdiv_param meson_ao_cec_g12a_div_table[] = {
	{
		.dual	= 1,
		.n1	= 733,
		.m1	= 8,
		.n2	= 732,
		.m2	= 11,
	}, {}
};

static struct clk_regmap meson_ao_cec_g12a_clk_cec_pre = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CECB_CLK_CNTL_REG0,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "ao_cec_b_pre",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ IN_PREFIX "cts_oscin" },
		.num_parents = 1,
	},
};

static struct clk_regmap meson_ao_cec_g12a_clk_cec_div = {
	.data = &(struct meson_clk_dualdiv_data){
		.n1 = {
			.reg_off = CECB_CLK_CNTL_REG0,
			.shift   = 0,
			.width   = 12,
		},
		.n2 = {
			.reg_off = CECB_CLK_CNTL_REG0,
			.shift   = 12,
			.width   = 12,
		},
		.m1 = {
			.reg_off = CECB_CLK_CNTL_REG1,
			.shift   = 0,
			.width   = 12,
		},
		.m2 = {
			.reg_off = CECB_CLK_CNTL_REG1,
			.shift   = 12,
			.width   = 12,
		},
		.dual = {
			.reg_off = CECB_CLK_CNTL_REG0,
			.shift   = 28,
			.width   = 1,
		},
		.table = meson_ao_cec_g12a_div_table,
	},
	.hw.init = &(struct clk_init_data){
		.name = "ao_cec_b_div",
		.ops = &meson_clk_dualdiv_ops,
		.parent_names = (const char *[]){ "ao_cec_b_pre" },
		.num_parents = 1,
	},
};

static struct clk_regmap meson_ao_cec_g12a_clk_cec_sel = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CECB_CLK_CNTL_REG1,
		.mask = 0x1,
		.shift = 24,
		.flags = CLK_MUX_ROUND_CLOSEST,
	},
	.hw.init = &(struct clk_init_data){
		.name = "ao_cec_b_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_names = (const char *[]){ "ao_cec_b_div",
						  "ao_cec_b_pre" },
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap meson_ao_cec_g12a_clk_cec = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CECB_CLK_CNTL_REG0,
		.bit_idx = 30,
	},
	.hw.init = &(struct clk_init_data){
		.name = "ao_cec_b",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "ao_cec_b_sel" },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap *meson_ao_cec_g12a_clk_regmap[] = {
	&meson_ao_cec_g12a_clk_cec_pre,
	&meson_ao_cec_g12a_clk_cec_div,
	&meson_ao_cec_g12a_clk_cec_sel,
	&meson_ao_cec_g12a_clk_cec,
};

struct meson_ao_cec_g12a_clk {
	struct meson_clk_provider_ops ops;
	struct clk *cec_clk;
	struct clk_hw hw;
};

struct clk *meson_ao_cec_g12a_clk_get(struct platform_device *pdev,
				      const char *name)
{
	struct meson_clk_provider_ops *ops;
	struct meson_ao_cec_g12a_clk *priv;

	ops = meson_clk_provider_get_ops(pdev);
	if (!ops)
		return ERR_PTR(-ENODEV);

	priv = container_of(ops, struct meson_ao_cec_g12a_clk, ops);

	return priv->cec_clk;
}

static int meson_ao_cec_g12a_clk_probe(struct platform_device *pdev)
{
	const struct meson_clk_provider_data *data;
	struct meson_ao_cec_g12a_clk *priv;
	struct device *dev = &pdev->dev;
	struct clk_hw *input;
	char *parent_name;
	struct clk *clk;
	int i, ret;

	data = dev_get_platdata(&pdev->dev);
	if (!data)
		return -ENXIO;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->ops.clk_get = meson_ao_cec_g12a_clk_get;

	parent_name = kasprintf(GFP_KERNEL, "%s", IN_PREFIX "cts_oscin");
	if (!parent_name)
		return -ENOMEM;

	/* Register input clock */
	input = meson_clk_hw_register_input(dev->parent, data->input_names[0],
					    parent_name, 0);
	if (IS_ERR(input))
		return PTR_ERR(input);

	/* Populate regmap */
	for (i = 0; i < ARRAY_SIZE(meson_ao_cec_g12a_clk_regmap); i++)
		meson_ao_cec_g12a_clk_regmap[i]->map = data->regmap;

	/* Register all clks */
	for (i = 0; i < ARRAY_SIZE(meson_ao_cec_g12a_clk_regmap); i++) {
		ret = devm_clk_hw_register(dev,
					&meson_ao_cec_g12a_clk_regmap[i]->hw);
		if (ret) {
			dev_err(dev, "Clock registration failed\n");
			return ret;
		}
	}

	clk = __clk_lookup(clk_hw_get_name(&meson_ao_cec_g12a_clk_cec.hw));
	if (!clk)
		return -EINVAL;

	priv->cec_clk = clk;

	platform_set_drvdata(pdev, priv);

	return 0;
}

static struct platform_driver meson_ao_cec_g12a_clk_driver = {
	.probe		= meson_ao_cec_g12a_clk_probe,
	.driver		= {
		.name	= "meson-ao-cec-g12a-clk",
	},
};
module_platform_driver(meson_ao_cec_g12a_clk_driver);

MODULE_AUTHOR("Neil Armstrong <narmstrong@baylibre.com>");
MODULE_DESCRIPTION("Meson G12A AO CEC Clock Divider driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS(PLATFORM_MODULE_PREFIX "meson-ao-cec-g12a-clk");
