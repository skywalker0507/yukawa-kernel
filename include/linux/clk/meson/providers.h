/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 BayLibre, SAS.
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 */

#ifndef __MESON_CLK_PROVIDERS_H
#define __MESON_CLK_PROVIDERS_H

#include <linux/platform_device.h>
#include <linux/clk-provider.h>
#include <linux/regmap.h>

struct meson_clk_provider_ops {
	struct clk *(*clk_get)(struct platform_device *pdev, const char *name);
};

static inline struct meson_clk_provider_ops *
meson_clk_provider_get_ops(struct platform_device *pdev)
{
	return platform_get_drvdata(pdev);
}

struct meson_clk_provider_data {
	struct regmap *regmap;
	const char * const *input_names;
	unsigned input_count;
};

#endif /* __MESON_CLK_PROVIDERS_H */
