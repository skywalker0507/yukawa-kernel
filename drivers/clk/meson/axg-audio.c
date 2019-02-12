// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Copyright (c) 2018 BayLibre, SAS.
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/init.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include "axg-audio.h"
#include "clk-input.h"
#include "clk-regmap.h"
#include "clk-phase.h"
#include "sclk-div.h"

#define AXG_MST_IN_COUNT	8
#define AXG_SLV_SCLK_COUNT	10
#define AXG_SLV_LRCLK_COUNT	10

#define AXG_AUD_GATE(_name, _reg, _bit, _pname, _iflags)		\
struct clk_regmap _name = {					\
	.data = &(struct clk_regmap_gate_data){				\
		.offset = (_reg),					\
		.bit_idx = (_bit),					\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = ""#_name,					\
		.ops = &clk_regmap_gate_ops,				\
		.parent_names = (const char *[]){ _pname },		\
		.num_parents = 1,					\
		.flags = CLK_DUTY_CYCLE_PARENT | (_iflags),		\
	},								\
}

#define AXG_AUD_MUX(_name, _reg, _mask, _shift, _dflags, _pnames, _iflags) \
struct clk_regmap _name = {					\
	.data = &(struct clk_regmap_mux_data){				\
		.offset = (_reg),					\
		.mask = (_mask),					\
		.shift = (_shift),					\
		.flags = (_dflags),					\
	},								\
	.hw.init = &(struct clk_init_data){				\
		.name = ""#_name,					\
		.ops = &clk_regmap_mux_ops,				\
		.parent_names = (_pnames),				\
		.num_parents = ARRAY_SIZE(_pnames),			\
		.flags = CLK_DUTY_CYCLE_PARENT | (_iflags),		\
	},								\
}

#define AXG_AUD_DIV(_name, _reg, _shift, _width, _dflags, _pname, _iflags) \
struct clk_regmap _name = {					\
	.data = &(struct clk_regmap_div_data){				\
		.offset = (_reg),					\
		.shift = (_shift),					\
		.width = (_width),					\
		.flags = (_dflags),					\
	},								\
	.hw.init = &(struct clk_init_data){				\
		.name = ""#_name,					\
		.ops = &clk_regmap_divider_ops,				\
		.parent_names = (const char *[]) { _pname },		\
		.num_parents = 1,					\
		.flags = (_iflags),					\
	},								\
}

#define AXG_PCLK_GATE(_name, _bit)				\
	AXG_AUD_GATE(_name, AUDIO_CLK_GATE_EN, _bit, "audio_pclk", 0)

/* Audio peripheral clocks */
static AXG_PCLK_GATE(ddr_arb,	   0);
static AXG_PCLK_GATE(pdm,	   1);
static AXG_PCLK_GATE(tdmin_a,	   2);
static AXG_PCLK_GATE(tdmin_b,	   3);
static AXG_PCLK_GATE(tdmin_c,	   4);
static AXG_PCLK_GATE(tdmin_lb,	   5);
static AXG_PCLK_GATE(tdmout_a,	   6);
static AXG_PCLK_GATE(tdmout_b,	   7);
static AXG_PCLK_GATE(tdmout_c,	   8);
static AXG_PCLK_GATE(frddr_a,	   9);
static AXG_PCLK_GATE(frddr_b,	   10);
static AXG_PCLK_GATE(frddr_c,	   11);
static AXG_PCLK_GATE(toddr_a,	   12);
static AXG_PCLK_GATE(toddr_b,	   13);
static AXG_PCLK_GATE(toddr_c,	   14);
static AXG_PCLK_GATE(loopback,	   15);
static AXG_PCLK_GATE(spdifin,	   16);
static AXG_PCLK_GATE(spdifout,	   17);
static AXG_PCLK_GATE(resample,	   18);
static AXG_PCLK_GATE(power_detect, 19);
static AXG_PCLK_GATE(spdifout_b,   21);

/* Audio Master Clocks */
static const char * const mst_mux_parent_names[] = {
	"mst_in0", "mst_in1", "mst_in2", "mst_in3",
	"mst_in4", "mst_in5", "mst_in6", "mst_in7",
};

#define AXG_MST_MUX(_name, _reg, _flag)				\
	AXG_AUD_MUX(_name##_sel, _reg, 0x7, 24, _flag,		\
		    mst_mux_parent_names, CLK_SET_RATE_PARENT)

#define AXG_MST_MCLK_MUX(_name, _reg)				\
	AXG_MST_MUX(_name, _reg, CLK_MUX_ROUND_CLOSEST)

#define AXG_MST_SYS_MUX(_name, _reg)				\
	AXG_MST_MUX(_name, _reg, 0)

static AXG_MST_MCLK_MUX(mst_a_mclk,   AUDIO_MCLK_A_CTRL);
static AXG_MST_MCLK_MUX(mst_b_mclk,   AUDIO_MCLK_B_CTRL);
static AXG_MST_MCLK_MUX(mst_c_mclk,   AUDIO_MCLK_C_CTRL);
static AXG_MST_MCLK_MUX(mst_d_mclk,   AUDIO_MCLK_D_CTRL);
static AXG_MST_MCLK_MUX(mst_e_mclk,   AUDIO_MCLK_E_CTRL);
static AXG_MST_MCLK_MUX(mst_f_mclk,   AUDIO_MCLK_F_CTRL);
static AXG_MST_MCLK_MUX(spdifout_clk, AUDIO_CLK_SPDIFOUT_CTRL);
static AXG_MST_MCLK_MUX(spdifout_b_clk, AUDIO_CLK_SPDIFOUT_B_CTRL);
static AXG_MST_MCLK_MUX(pdm_dclk,     AUDIO_CLK_PDMIN_CTRL0);
static AXG_MST_SYS_MUX(spdifin_clk,   AUDIO_CLK_SPDIFIN_CTRL);
static AXG_MST_SYS_MUX(pdm_sysclk,    AUDIO_CLK_PDMIN_CTRL1);

#define AXG_MST_DIV(_name, _reg, _flag)				\
	AXG_AUD_DIV(_name##_div, _reg, 0, 16, _flag,		\
		    ""#_name"_sel", CLK_SET_RATE_PARENT)	\

#define AXG_MST_MCLK_DIV(_name, _reg)				\
	AXG_MST_DIV(_name, _reg, CLK_DIVIDER_ROUND_CLOSEST)

#define AXG_MST_SYS_DIV(_name, _reg)				\
	AXG_MST_DIV(_name, _reg, 0)

static AXG_MST_MCLK_DIV(mst_a_mclk,   AUDIO_MCLK_A_CTRL);
static AXG_MST_MCLK_DIV(mst_b_mclk,   AUDIO_MCLK_B_CTRL);
static AXG_MST_MCLK_DIV(mst_c_mclk,   AUDIO_MCLK_C_CTRL);
static AXG_MST_MCLK_DIV(mst_d_mclk,   AUDIO_MCLK_D_CTRL);
static AXG_MST_MCLK_DIV(mst_e_mclk,   AUDIO_MCLK_E_CTRL);
static AXG_MST_MCLK_DIV(mst_f_mclk,   AUDIO_MCLK_F_CTRL);
static AXG_MST_MCLK_DIV(spdifout_clk, AUDIO_CLK_SPDIFOUT_CTRL);
static AXG_MST_MCLK_DIV(spdifout_b_clk, AUDIO_CLK_SPDIFOUT_B_CTRL);
static AXG_MST_MCLK_DIV(pdm_dclk,     AUDIO_CLK_PDMIN_CTRL0);
static AXG_MST_SYS_DIV(spdifin_clk,   AUDIO_CLK_SPDIFIN_CTRL);
static AXG_MST_SYS_DIV(pdm_sysclk,    AUDIO_CLK_PDMIN_CTRL1);

#define AXG_MST_MCLK_GATE(_name, _reg)				\
	AXG_AUD_GATE(_name, _reg, 31,  ""#_name"_div",	\
		     CLK_SET_RATE_PARENT)

static AXG_MST_MCLK_GATE(mst_a_mclk,   AUDIO_MCLK_A_CTRL);
static AXG_MST_MCLK_GATE(mst_b_mclk,   AUDIO_MCLK_B_CTRL);
static AXG_MST_MCLK_GATE(mst_c_mclk,   AUDIO_MCLK_C_CTRL);
static AXG_MST_MCLK_GATE(mst_d_mclk,   AUDIO_MCLK_D_CTRL);
static AXG_MST_MCLK_GATE(mst_e_mclk,   AUDIO_MCLK_E_CTRL);
static AXG_MST_MCLK_GATE(mst_f_mclk,   AUDIO_MCLK_F_CTRL);
static AXG_MST_MCLK_GATE(spdifout_clk, AUDIO_CLK_SPDIFOUT_CTRL);
static AXG_MST_MCLK_GATE(spdifout_b_clk, AUDIO_CLK_SPDIFOUT_B_CTRL);
static AXG_MST_MCLK_GATE(spdifin_clk,  AUDIO_CLK_SPDIFIN_CTRL);
static AXG_MST_MCLK_GATE(pdm_dclk,     AUDIO_CLK_PDMIN_CTRL0);
static AXG_MST_MCLK_GATE(pdm_sysclk,   AUDIO_CLK_PDMIN_CTRL1);

/* Sample Clocks */
#define AXG_MST_SCLK_PRE_EN(_name, _reg)			\
	AXG_AUD_GATE(mst_##_name##_sclk_pre_en, _reg, 31,	\
		     "mst_"#_name"_mclk", 0)

static AXG_MST_SCLK_PRE_EN(a, AUDIO_MST_A_SCLK_CTRL0);
static AXG_MST_SCLK_PRE_EN(b, AUDIO_MST_B_SCLK_CTRL0);
static AXG_MST_SCLK_PRE_EN(c, AUDIO_MST_C_SCLK_CTRL0);
static AXG_MST_SCLK_PRE_EN(d, AUDIO_MST_D_SCLK_CTRL0);
static AXG_MST_SCLK_PRE_EN(e, AUDIO_MST_E_SCLK_CTRL0);
static AXG_MST_SCLK_PRE_EN(f, AUDIO_MST_F_SCLK_CTRL0);

#define AXG_AUD_SCLK_DIV(_name, _reg, _div_shift, _div_width,		\
			 _hi_shift, _hi_width, _pname, _iflags)		\
struct clk_regmap _name = {					\
	.data = &(struct meson_sclk_div_data) {				\
		.div = {						\
			.reg_off = (_reg),				\
			.shift   = (_div_shift),			\
			.width   = (_div_width),			\
		},							\
		.hi = {							\
			.reg_off = (_reg),				\
			.shift   = (_hi_shift),				\
			.width   = (_hi_width),				\
		},							\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = ""#_name,					\
		.ops = &meson_sclk_div_ops,				\
		.parent_names = (const char *[]) { _pname },		\
		.num_parents = 1,					\
		.flags = (_iflags),					\
	},								\
}

#define AXG_MST_SCLK_DIV(_name, _reg)					\
	AXG_AUD_SCLK_DIV(mst_##_name##_sclk_div, _reg, 20, 10, 0, 0,	\
			 "mst_"#_name"_sclk_pre_en",		\
			 CLK_SET_RATE_PARENT)

static AXG_MST_SCLK_DIV(a, AUDIO_MST_A_SCLK_CTRL0);
static AXG_MST_SCLK_DIV(b, AUDIO_MST_B_SCLK_CTRL0);
static AXG_MST_SCLK_DIV(c, AUDIO_MST_C_SCLK_CTRL0);
static AXG_MST_SCLK_DIV(d, AUDIO_MST_D_SCLK_CTRL0);
static AXG_MST_SCLK_DIV(e, AUDIO_MST_E_SCLK_CTRL0);
static AXG_MST_SCLK_DIV(f, AUDIO_MST_F_SCLK_CTRL0);

#define AXG_MST_SCLK_POST_EN(_name, _reg)				\
	AXG_AUD_GATE(mst_##_name##_sclk_post_en, _reg, 30,		\
		     "mst_"#_name"_sclk_div", CLK_SET_RATE_PARENT)

static AXG_MST_SCLK_POST_EN(a, AUDIO_MST_A_SCLK_CTRL0);
static AXG_MST_SCLK_POST_EN(b, AUDIO_MST_B_SCLK_CTRL0);
static AXG_MST_SCLK_POST_EN(c, AUDIO_MST_C_SCLK_CTRL0);
static AXG_MST_SCLK_POST_EN(d, AUDIO_MST_D_SCLK_CTRL0);
static AXG_MST_SCLK_POST_EN(e, AUDIO_MST_E_SCLK_CTRL0);
static AXG_MST_SCLK_POST_EN(f, AUDIO_MST_F_SCLK_CTRL0);

#define AXG_AUD_TRIPHASE(_name, _reg, _width, _shift0, _shift1, _shift2, \
			 _pname, _iflags)				\
struct clk_regmap _name = {					\
	.data = &(struct meson_clk_triphase_data) {			\
		.ph0 = {						\
			.reg_off = (_reg),				\
			.shift   = (_shift0),				\
			.width   = (_width),				\
		},							\
		.ph1 = {						\
			.reg_off = (_reg),				\
			.shift   = (_shift1),				\
			.width   = (_width),				\
		},							\
		.ph2 = {						\
			.reg_off = (_reg),				\
			.shift   = (_shift2),				\
			.width   = (_width),				\
		},							\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = ""#_name,					\
		.ops = &meson_clk_triphase_ops,				\
		.parent_names = (const char *[]) { _pname },		\
		.num_parents = 1,					\
		.flags = CLK_DUTY_CYCLE_PARENT | (_iflags),		\
	},								\
}

#define AXG_MST_SCLK(_name, _reg)					\
	AXG_AUD_TRIPHASE(mst_##_name##_sclk, _reg, 1, 0, 2, 4,		\
			 "mst_"#_name"_sclk_post_en", CLK_SET_RATE_PARENT)

static AXG_MST_SCLK(a, AUDIO_MST_A_SCLK_CTRL1);
static AXG_MST_SCLK(b, AUDIO_MST_B_SCLK_CTRL1);
static AXG_MST_SCLK(c, AUDIO_MST_C_SCLK_CTRL1);
static AXG_MST_SCLK(d, AUDIO_MST_D_SCLK_CTRL1);
static AXG_MST_SCLK(e, AUDIO_MST_E_SCLK_CTRL1);
static AXG_MST_SCLK(f, AUDIO_MST_F_SCLK_CTRL1);

#define AXG_MST_LRCLK_DIV(_name, _reg)					\
	AXG_AUD_SCLK_DIV(mst_##_name##_lrclk_div, _reg, 0, 10, 10, 10,	\
		    "mst_"#_name"_sclk_post_en", 0)			\

static AXG_MST_LRCLK_DIV(a, AUDIO_MST_A_SCLK_CTRL0);
static AXG_MST_LRCLK_DIV(b, AUDIO_MST_B_SCLK_CTRL0);
static AXG_MST_LRCLK_DIV(c, AUDIO_MST_C_SCLK_CTRL0);
static AXG_MST_LRCLK_DIV(d, AUDIO_MST_D_SCLK_CTRL0);
static AXG_MST_LRCLK_DIV(e, AUDIO_MST_E_SCLK_CTRL0);
static AXG_MST_LRCLK_DIV(f, AUDIO_MST_F_SCLK_CTRL0);

#define AXG_MST_LRCLK(_name, _reg)					\
	AXG_AUD_TRIPHASE(mst_##_name##_lrclk, _reg, 1, 1, 3, 5,		\
			 "mst_"#_name"_lrclk_div", CLK_SET_RATE_PARENT)

static AXG_MST_LRCLK(a, AUDIO_MST_A_SCLK_CTRL1);
static AXG_MST_LRCLK(b, AUDIO_MST_B_SCLK_CTRL1);
static AXG_MST_LRCLK(c, AUDIO_MST_C_SCLK_CTRL1);
static AXG_MST_LRCLK(d, AUDIO_MST_D_SCLK_CTRL1);
static AXG_MST_LRCLK(e, AUDIO_MST_E_SCLK_CTRL1);
static AXG_MST_LRCLK(f, AUDIO_MST_F_SCLK_CTRL1);

static const char * const tdm_sclk_parent_names[] = {
	"mst_a_sclk", "mst_b_sclk", "mst_c_sclk",
	"mst_d_sclk", "mst_e_sclk", "mst_f_sclk",
	"slv_sclk0", "slv_sclk1", "slv_sclk2",
	"slv_sclk3", "slv_sclk4", "slv_sclk5",
	"slv_sclk6", "slv_sclk7", "slv_sclk8",
	"slv_sclk9"
};

#define AXG_TDM_SCLK_MUX(_name, _reg)				\
	AXG_AUD_MUX(tdm##_name##_sclk_sel, _reg, 0xf, 24,	\
		    CLK_MUX_ROUND_CLOSEST,			\
		    tdm_sclk_parent_names, 0)

static AXG_TDM_SCLK_MUX(in_a,  AUDIO_CLK_TDMIN_A_CTRL);
static AXG_TDM_SCLK_MUX(in_b,  AUDIO_CLK_TDMIN_B_CTRL);
static AXG_TDM_SCLK_MUX(in_c,  AUDIO_CLK_TDMIN_C_CTRL);
static AXG_TDM_SCLK_MUX(in_lb, AUDIO_CLK_TDMIN_LB_CTRL);
static AXG_TDM_SCLK_MUX(out_a, AUDIO_CLK_TDMOUT_A_CTRL);
static AXG_TDM_SCLK_MUX(out_b, AUDIO_CLK_TDMOUT_B_CTRL);
static AXG_TDM_SCLK_MUX(out_c, AUDIO_CLK_TDMOUT_C_CTRL);

#define AXG_TDM_SCLK_PRE_EN(_name, _reg)				\
	AXG_AUD_GATE(tdm##_name##_sclk_pre_en, _reg, 31,		\
		     "tdm"#_name"_sclk_sel", CLK_SET_RATE_PARENT)

static AXG_TDM_SCLK_PRE_EN(in_a,  AUDIO_CLK_TDMIN_A_CTRL);
static AXG_TDM_SCLK_PRE_EN(in_b,  AUDIO_CLK_TDMIN_B_CTRL);
static AXG_TDM_SCLK_PRE_EN(in_c,  AUDIO_CLK_TDMIN_C_CTRL);
static AXG_TDM_SCLK_PRE_EN(in_lb, AUDIO_CLK_TDMIN_LB_CTRL);
static AXG_TDM_SCLK_PRE_EN(out_a, AUDIO_CLK_TDMOUT_A_CTRL);
static AXG_TDM_SCLK_PRE_EN(out_b, AUDIO_CLK_TDMOUT_B_CTRL);
static AXG_TDM_SCLK_PRE_EN(out_c, AUDIO_CLK_TDMOUT_C_CTRL);

#define AXG_TDM_SCLK_POST_EN(_name, _reg)				\
	AXG_AUD_GATE(tdm##_name##_sclk_post_en, _reg, 30,		\
		     "tdm"#_name"_sclk_pre_en", CLK_SET_RATE_PARENT)

static AXG_TDM_SCLK_POST_EN(in_a,  AUDIO_CLK_TDMIN_A_CTRL);
static AXG_TDM_SCLK_POST_EN(in_b,  AUDIO_CLK_TDMIN_B_CTRL);
static AXG_TDM_SCLK_POST_EN(in_c,  AUDIO_CLK_TDMIN_C_CTRL);
static AXG_TDM_SCLK_POST_EN(in_lb, AUDIO_CLK_TDMIN_LB_CTRL);
static AXG_TDM_SCLK_POST_EN(out_a, AUDIO_CLK_TDMOUT_A_CTRL);
static AXG_TDM_SCLK_POST_EN(out_b, AUDIO_CLK_TDMOUT_B_CTRL);
static AXG_TDM_SCLK_POST_EN(out_c, AUDIO_CLK_TDMOUT_C_CTRL);

#define AXG_TDM_SCLK(_name, _reg)					\
	struct clk_regmap tdm##_name##_sclk = {			\
	.data = &(struct meson_clk_phase_data) {			\
		.ph = {							\
			.reg_off = (_reg),				\
			.shift   = 29,					\
			.width   = 1,					\
		},							\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = "tdm"#_name"_sclk",				\
		.ops = &meson_clk_phase_ops,				\
		.parent_names = (const char *[])			\
		{ "tdm"#_name"_sclk_post_en" },			\
		.num_parents = 1,					\
		.flags = CLK_DUTY_CYCLE_PARENT | CLK_SET_RATE_PARENT,	\
	},								\
}

static AXG_TDM_SCLK(in_a,  AUDIO_CLK_TDMIN_A_CTRL);
static AXG_TDM_SCLK(in_b,  AUDIO_CLK_TDMIN_B_CTRL);
static AXG_TDM_SCLK(in_c,  AUDIO_CLK_TDMIN_C_CTRL);
static AXG_TDM_SCLK(in_lb, AUDIO_CLK_TDMIN_LB_CTRL);
static AXG_TDM_SCLK(out_a, AUDIO_CLK_TDMOUT_A_CTRL);
static AXG_TDM_SCLK(out_b, AUDIO_CLK_TDMOUT_B_CTRL);
static AXG_TDM_SCLK(out_c, AUDIO_CLK_TDMOUT_C_CTRL);

static const char * const tdm_lrclk_parent_names[] = {
	"mst_a_lrclk", "mst_b_lrclk", "mst_c_lrclk",
	"mst_d_lrclk", "mst_e_lrclk", "mst_f_lrclk",
	"slv_lrclk0", "slv_lrclk1", "slv_lrclk2",
	"slv_lrclk3", "slv_lrclk4", "slv_lrclk5",
	"slv_lrclk6", "slv_lrclk7", "slv_lrclk8",
	"slv_lrclk9"
};

#define AXG_TDM_LRLCK(_name, _reg)		       \
	AXG_AUD_MUX(tdm##_name##_lrclk, _reg, 0xf, 20, \
		    CLK_MUX_ROUND_CLOSEST,	       \
		    tdm_lrclk_parent_names, 0)

static AXG_TDM_LRLCK(in_a,  AUDIO_CLK_TDMIN_A_CTRL);
static AXG_TDM_LRLCK(in_b,  AUDIO_CLK_TDMIN_B_CTRL);
static AXG_TDM_LRLCK(in_c,  AUDIO_CLK_TDMIN_C_CTRL);
static AXG_TDM_LRLCK(in_lb, AUDIO_CLK_TDMIN_LB_CTRL);
static AXG_TDM_LRLCK(out_a, AUDIO_CLK_TDMOUT_A_CTRL);
static AXG_TDM_LRLCK(out_b, AUDIO_CLK_TDMOUT_B_CTRL);
static AXG_TDM_LRLCK(out_c, AUDIO_CLK_TDMOUT_C_CTRL);

/*
 * Array of all AXG clocks provided by this provider
 * The input clocks of the controller will be populated at runtime
 */
static struct clk_hw_onecell_data axg_audio_hw_onecell_data = {
	.hws = {
		[AUD_CLKID_DDR_ARB]		= &ddr_arb.hw,
		[AUD_CLKID_PDM]			= &pdm.hw,
		[AUD_CLKID_TDMIN_A]		= &tdmin_a.hw,
		[AUD_CLKID_TDMIN_B]		= &tdmin_b.hw,
		[AUD_CLKID_TDMIN_C]		= &tdmin_c.hw,
		[AUD_CLKID_TDMIN_LB]		= &tdmin_lb.hw,
		[AUD_CLKID_TDMOUT_A]		= &tdmout_a.hw,
		[AUD_CLKID_TDMOUT_B]		= &tdmout_b.hw,
		[AUD_CLKID_TDMOUT_C]		= &tdmout_c.hw,
		[AUD_CLKID_FRDDR_A]		= &frddr_a.hw,
		[AUD_CLKID_FRDDR_B]		= &frddr_b.hw,
		[AUD_CLKID_FRDDR_C]		= &frddr_c.hw,
		[AUD_CLKID_TODDR_A]		= &toddr_a.hw,
		[AUD_CLKID_TODDR_B]		= &toddr_b.hw,
		[AUD_CLKID_TODDR_C]		= &toddr_c.hw,
		[AUD_CLKID_LOOPBACK]		= &loopback.hw,
		[AUD_CLKID_SPDIFIN]		= &spdifin.hw,
		[AUD_CLKID_SPDIFOUT]		= &spdifout.hw,
		[AUD_CLKID_RESAMPLE]		= &resample.hw,
		[AUD_CLKID_POWER_DETECT]	= &power_detect.hw,
		[AUD_CLKID_MST_A_MCLK_SEL]	= &mst_a_mclk_sel.hw,
		[AUD_CLKID_MST_B_MCLK_SEL]	= &mst_b_mclk_sel.hw,
		[AUD_CLKID_MST_C_MCLK_SEL]	= &mst_c_mclk_sel.hw,
		[AUD_CLKID_MST_D_MCLK_SEL]	= &mst_d_mclk_sel.hw,
		[AUD_CLKID_MST_E_MCLK_SEL]	= &mst_e_mclk_sel.hw,
		[AUD_CLKID_MST_F_MCLK_SEL]	= &mst_f_mclk_sel.hw,
		[AUD_CLKID_MST_A_MCLK_DIV]	= &mst_a_mclk_div.hw,
		[AUD_CLKID_MST_B_MCLK_DIV]	= &mst_b_mclk_div.hw,
		[AUD_CLKID_MST_C_MCLK_DIV]	= &mst_c_mclk_div.hw,
		[AUD_CLKID_MST_D_MCLK_DIV]	= &mst_d_mclk_div.hw,
		[AUD_CLKID_MST_E_MCLK_DIV]	= &mst_e_mclk_div.hw,
		[AUD_CLKID_MST_F_MCLK_DIV]	= &mst_f_mclk_div.hw,
		[AUD_CLKID_MST_A_MCLK]		= &mst_a_mclk.hw,
		[AUD_CLKID_MST_B_MCLK]		= &mst_b_mclk.hw,
		[AUD_CLKID_MST_C_MCLK]		= &mst_c_mclk.hw,
		[AUD_CLKID_MST_D_MCLK]		= &mst_d_mclk.hw,
		[AUD_CLKID_MST_E_MCLK]		= &mst_e_mclk.hw,
		[AUD_CLKID_MST_F_MCLK]		= &mst_f_mclk.hw,
		[AUD_CLKID_SPDIFOUT_CLK_SEL]	= &spdifout_clk_sel.hw,
		[AUD_CLKID_SPDIFOUT_CLK_DIV]	= &spdifout_clk_div.hw,
		[AUD_CLKID_SPDIFOUT_CLK]	= &spdifout_clk.hw,
		[AUD_CLKID_SPDIFIN_CLK_SEL]	= &spdifin_clk_sel.hw,
		[AUD_CLKID_SPDIFIN_CLK_DIV]	= &spdifin_clk_div.hw,
		[AUD_CLKID_SPDIFIN_CLK]		= &spdifin_clk.hw,
		[AUD_CLKID_PDM_DCLK_SEL]	= &pdm_dclk_sel.hw,
		[AUD_CLKID_PDM_DCLK_DIV]	= &pdm_dclk_div.hw,
		[AUD_CLKID_PDM_DCLK]		= &pdm_dclk.hw,
		[AUD_CLKID_PDM_SYSCLK_SEL]	= &pdm_sysclk_sel.hw,
		[AUD_CLKID_PDM_SYSCLK_DIV]	= &pdm_sysclk_div.hw,
		[AUD_CLKID_PDM_SYSCLK]		= &pdm_sysclk.hw,
		[AUD_CLKID_MST_A_SCLK_PRE_EN]	= &mst_a_sclk_pre_en.hw,
		[AUD_CLKID_MST_B_SCLK_PRE_EN]	= &mst_b_sclk_pre_en.hw,
		[AUD_CLKID_MST_C_SCLK_PRE_EN]	= &mst_c_sclk_pre_en.hw,
		[AUD_CLKID_MST_D_SCLK_PRE_EN]	= &mst_d_sclk_pre_en.hw,
		[AUD_CLKID_MST_E_SCLK_PRE_EN]	= &mst_e_sclk_pre_en.hw,
		[AUD_CLKID_MST_F_SCLK_PRE_EN]	= &mst_f_sclk_pre_en.hw,
		[AUD_CLKID_MST_A_SCLK_DIV]	= &mst_a_sclk_div.hw,
		[AUD_CLKID_MST_B_SCLK_DIV]	= &mst_b_sclk_div.hw,
		[AUD_CLKID_MST_C_SCLK_DIV]	= &mst_c_sclk_div.hw,
		[AUD_CLKID_MST_D_SCLK_DIV]	= &mst_d_sclk_div.hw,
		[AUD_CLKID_MST_E_SCLK_DIV]	= &mst_e_sclk_div.hw,
		[AUD_CLKID_MST_F_SCLK_DIV]	= &mst_f_sclk_div.hw,
		[AUD_CLKID_MST_A_SCLK_POST_EN]	= &mst_a_sclk_post_en.hw,
		[AUD_CLKID_MST_B_SCLK_POST_EN]	= &mst_b_sclk_post_en.hw,
		[AUD_CLKID_MST_C_SCLK_POST_EN]	= &mst_c_sclk_post_en.hw,
		[AUD_CLKID_MST_D_SCLK_POST_EN]	= &mst_d_sclk_post_en.hw,
		[AUD_CLKID_MST_E_SCLK_POST_EN]	= &mst_e_sclk_post_en.hw,
		[AUD_CLKID_MST_F_SCLK_POST_EN]	= &mst_f_sclk_post_en.hw,
		[AUD_CLKID_MST_A_SCLK]		= &mst_a_sclk.hw,
		[AUD_CLKID_MST_B_SCLK]		= &mst_b_sclk.hw,
		[AUD_CLKID_MST_C_SCLK]		= &mst_c_sclk.hw,
		[AUD_CLKID_MST_D_SCLK]		= &mst_d_sclk.hw,
		[AUD_CLKID_MST_E_SCLK]		= &mst_e_sclk.hw,
		[AUD_CLKID_MST_F_SCLK]		= &mst_f_sclk.hw,
		[AUD_CLKID_MST_A_LRCLK_DIV]	= &mst_a_lrclk_div.hw,
		[AUD_CLKID_MST_B_LRCLK_DIV]	= &mst_b_lrclk_div.hw,
		[AUD_CLKID_MST_C_LRCLK_DIV]	= &mst_c_lrclk_div.hw,
		[AUD_CLKID_MST_D_LRCLK_DIV]	= &mst_d_lrclk_div.hw,
		[AUD_CLKID_MST_E_LRCLK_DIV]	= &mst_e_lrclk_div.hw,
		[AUD_CLKID_MST_F_LRCLK_DIV]	= &mst_f_lrclk_div.hw,
		[AUD_CLKID_MST_A_LRCLK]		= &mst_a_lrclk.hw,
		[AUD_CLKID_MST_B_LRCLK]		= &mst_b_lrclk.hw,
		[AUD_CLKID_MST_C_LRCLK]		= &mst_c_lrclk.hw,
		[AUD_CLKID_MST_D_LRCLK]		= &mst_d_lrclk.hw,
		[AUD_CLKID_MST_E_LRCLK]		= &mst_e_lrclk.hw,
		[AUD_CLKID_MST_F_LRCLK]		= &mst_f_lrclk.hw,
		[AUD_CLKID_TDMIN_A_SCLK_SEL]	= &tdmin_a_sclk_sel.hw,
		[AUD_CLKID_TDMIN_B_SCLK_SEL]	= &tdmin_b_sclk_sel.hw,
		[AUD_CLKID_TDMIN_C_SCLK_SEL]	= &tdmin_c_sclk_sel.hw,
		[AUD_CLKID_TDMIN_LB_SCLK_SEL]	= &tdmin_lb_sclk_sel.hw,
		[AUD_CLKID_TDMOUT_A_SCLK_SEL]	= &tdmout_a_sclk_sel.hw,
		[AUD_CLKID_TDMOUT_B_SCLK_SEL]	= &tdmout_b_sclk_sel.hw,
		[AUD_CLKID_TDMOUT_C_SCLK_SEL]	= &tdmout_c_sclk_sel.hw,
		[AUD_CLKID_TDMIN_A_SCLK_PRE_EN]	= &tdmin_a_sclk_pre_en.hw,
		[AUD_CLKID_TDMIN_B_SCLK_PRE_EN]	= &tdmin_b_sclk_pre_en.hw,
		[AUD_CLKID_TDMIN_C_SCLK_PRE_EN]	= &tdmin_c_sclk_pre_en.hw,
		[AUD_CLKID_TDMIN_LB_SCLK_PRE_EN] = &tdmin_lb_sclk_pre_en.hw,
		[AUD_CLKID_TDMOUT_A_SCLK_PRE_EN] = &tdmout_a_sclk_pre_en.hw,
		[AUD_CLKID_TDMOUT_B_SCLK_PRE_EN] = &tdmout_b_sclk_pre_en.hw,
		[AUD_CLKID_TDMOUT_C_SCLK_PRE_EN] = &tdmout_c_sclk_pre_en.hw,
		[AUD_CLKID_TDMIN_A_SCLK_POST_EN] = &tdmin_a_sclk_post_en.hw,
		[AUD_CLKID_TDMIN_B_SCLK_POST_EN] = &tdmin_b_sclk_post_en.hw,
		[AUD_CLKID_TDMIN_C_SCLK_POST_EN] = &tdmin_c_sclk_post_en.hw,
		[AUD_CLKID_TDMIN_LB_SCLK_POST_EN] = &tdmin_lb_sclk_post_en.hw,
		[AUD_CLKID_TDMOUT_A_SCLK_POST_EN] = &tdmout_a_sclk_post_en.hw,
		[AUD_CLKID_TDMOUT_B_SCLK_POST_EN] = &tdmout_b_sclk_post_en.hw,
		[AUD_CLKID_TDMOUT_C_SCLK_POST_EN] = &tdmout_c_sclk_post_en.hw,
		[AUD_CLKID_TDMIN_A_SCLK]	= &tdmin_a_sclk.hw,
		[AUD_CLKID_TDMIN_B_SCLK]	= &tdmin_b_sclk.hw,
		[AUD_CLKID_TDMIN_C_SCLK]	= &tdmin_c_sclk.hw,
		[AUD_CLKID_TDMIN_LB_SCLK]	= &tdmin_lb_sclk.hw,
		[AUD_CLKID_TDMOUT_A_SCLK]	= &tdmout_a_sclk.hw,
		[AUD_CLKID_TDMOUT_B_SCLK]	= &tdmout_b_sclk.hw,
		[AUD_CLKID_TDMOUT_C_SCLK]	= &tdmout_c_sclk.hw,
		[AUD_CLKID_TDMIN_A_LRCLK]	= &tdmin_a_lrclk.hw,
		[AUD_CLKID_TDMIN_B_LRCLK]	= &tdmin_b_lrclk.hw,
		[AUD_CLKID_TDMIN_C_LRCLK]	= &tdmin_c_lrclk.hw,
		[AUD_CLKID_TDMIN_LB_LRCLK]	= &tdmin_lb_lrclk.hw,
		[AUD_CLKID_TDMOUT_A_LRCLK]	= &tdmout_a_lrclk.hw,
		[AUD_CLKID_TDMOUT_B_LRCLK]	= &tdmout_b_lrclk.hw,
		[AUD_CLKID_TDMOUT_C_LRCLK]	= &tdmout_c_lrclk.hw,
		[NR_CLKS] = NULL,
	},
	.num = NR_CLKS,
};

/*
 * Array of all G12A clocks provided by this provider
 * The input clocks of the controller will be populated at runtime
 */
static struct clk_hw_onecell_data g12a_audio_hw_onecell_data = {
	.hws = {
		[AUD_CLKID_DDR_ARB]		= &ddr_arb.hw,
		[AUD_CLKID_PDM]			= &pdm.hw,
		[AUD_CLKID_TDMIN_A]		= &tdmin_a.hw,
		[AUD_CLKID_TDMIN_B]		= &tdmin_b.hw,
		[AUD_CLKID_TDMIN_C]		= &tdmin_c.hw,
		[AUD_CLKID_TDMIN_LB]		= &tdmin_lb.hw,
		[AUD_CLKID_TDMOUT_A]		= &tdmout_a.hw,
		[AUD_CLKID_TDMOUT_B]		= &tdmout_b.hw,
		[AUD_CLKID_TDMOUT_C]		= &tdmout_c.hw,
		[AUD_CLKID_FRDDR_A]		= &frddr_a.hw,
		[AUD_CLKID_FRDDR_B]		= &frddr_b.hw,
		[AUD_CLKID_FRDDR_C]		= &frddr_c.hw,
		[AUD_CLKID_TODDR_A]		= &toddr_a.hw,
		[AUD_CLKID_TODDR_B]		= &toddr_b.hw,
		[AUD_CLKID_TODDR_C]		= &toddr_c.hw,
		[AUD_CLKID_LOOPBACK]		= &loopback.hw,
		[AUD_CLKID_SPDIFIN]		= &spdifin.hw,
		[AUD_CLKID_SPDIFOUT]		= &spdifout.hw,
		[AUD_CLKID_RESAMPLE]		= &resample.hw,
		[AUD_CLKID_POWER_DETECT]	= &power_detect.hw,
		[AUD_CLKID_SPDIFOUT_B]		= &spdifout_b.hw,
		[AUD_CLKID_MST_A_MCLK_SEL]	= &mst_a_mclk_sel.hw,
		[AUD_CLKID_MST_B_MCLK_SEL]	= &mst_b_mclk_sel.hw,
		[AUD_CLKID_MST_C_MCLK_SEL]	= &mst_c_mclk_sel.hw,
		[AUD_CLKID_MST_D_MCLK_SEL]	= &mst_d_mclk_sel.hw,
		[AUD_CLKID_MST_E_MCLK_SEL]	= &mst_e_mclk_sel.hw,
		[AUD_CLKID_MST_F_MCLK_SEL]	= &mst_f_mclk_sel.hw,
		[AUD_CLKID_MST_A_MCLK_DIV]	= &mst_a_mclk_div.hw,
		[AUD_CLKID_MST_B_MCLK_DIV]	= &mst_b_mclk_div.hw,
		[AUD_CLKID_MST_C_MCLK_DIV]	= &mst_c_mclk_div.hw,
		[AUD_CLKID_MST_D_MCLK_DIV]	= &mst_d_mclk_div.hw,
		[AUD_CLKID_MST_E_MCLK_DIV]	= &mst_e_mclk_div.hw,
		[AUD_CLKID_MST_F_MCLK_DIV]	= &mst_f_mclk_div.hw,
		[AUD_CLKID_MST_A_MCLK]		= &mst_a_mclk.hw,
		[AUD_CLKID_MST_B_MCLK]		= &mst_b_mclk.hw,
		[AUD_CLKID_MST_C_MCLK]		= &mst_c_mclk.hw,
		[AUD_CLKID_MST_D_MCLK]		= &mst_d_mclk.hw,
		[AUD_CLKID_MST_E_MCLK]		= &mst_e_mclk.hw,
		[AUD_CLKID_MST_F_MCLK]		= &mst_f_mclk.hw,
		[AUD_CLKID_SPDIFOUT_CLK_SEL]	= &spdifout_clk_sel.hw,
		[AUD_CLKID_SPDIFOUT_CLK_DIV]	= &spdifout_clk_div.hw,
		[AUD_CLKID_SPDIFOUT_CLK]	= &spdifout_clk.hw,
		[AUD_CLKID_SPDIFOUT_B_CLK_SEL]	= &spdifout_b_clk_sel.hw,
		[AUD_CLKID_SPDIFOUT_B_CLK_DIV]	= &spdifout_b_clk_div.hw,
		[AUD_CLKID_SPDIFOUT_B_CLK]	= &spdifout_b_clk.hw,
		[AUD_CLKID_SPDIFIN_CLK_SEL]	= &spdifin_clk_sel.hw,
		[AUD_CLKID_SPDIFIN_CLK_DIV]	= &spdifin_clk_div.hw,
		[AUD_CLKID_SPDIFIN_CLK]		= &spdifin_clk.hw,
		[AUD_CLKID_PDM_DCLK_SEL]	= &pdm_dclk_sel.hw,
		[AUD_CLKID_PDM_DCLK_DIV]	= &pdm_dclk_div.hw,
		[AUD_CLKID_PDM_DCLK]		= &pdm_dclk.hw,
		[AUD_CLKID_PDM_SYSCLK_SEL]	= &pdm_sysclk_sel.hw,
		[AUD_CLKID_PDM_SYSCLK_DIV]	= &pdm_sysclk_div.hw,
		[AUD_CLKID_PDM_SYSCLK]		= &pdm_sysclk.hw,
		[AUD_CLKID_MST_A_SCLK_PRE_EN]	= &mst_a_sclk_pre_en.hw,
		[AUD_CLKID_MST_B_SCLK_PRE_EN]	= &mst_b_sclk_pre_en.hw,
		[AUD_CLKID_MST_C_SCLK_PRE_EN]	= &mst_c_sclk_pre_en.hw,
		[AUD_CLKID_MST_D_SCLK_PRE_EN]	= &mst_d_sclk_pre_en.hw,
		[AUD_CLKID_MST_E_SCLK_PRE_EN]	= &mst_e_sclk_pre_en.hw,
		[AUD_CLKID_MST_F_SCLK_PRE_EN]	= &mst_f_sclk_pre_en.hw,
		[AUD_CLKID_MST_A_SCLK_DIV]	= &mst_a_sclk_div.hw,
		[AUD_CLKID_MST_B_SCLK_DIV]	= &mst_b_sclk_div.hw,
		[AUD_CLKID_MST_C_SCLK_DIV]	= &mst_c_sclk_div.hw,
		[AUD_CLKID_MST_D_SCLK_DIV]	= &mst_d_sclk_div.hw,
		[AUD_CLKID_MST_E_SCLK_DIV]	= &mst_e_sclk_div.hw,
		[AUD_CLKID_MST_F_SCLK_DIV]	= &mst_f_sclk_div.hw,
		[AUD_CLKID_MST_A_SCLK_POST_EN]	= &mst_a_sclk_post_en.hw,
		[AUD_CLKID_MST_B_SCLK_POST_EN]	= &mst_b_sclk_post_en.hw,
		[AUD_CLKID_MST_C_SCLK_POST_EN]	= &mst_c_sclk_post_en.hw,
		[AUD_CLKID_MST_D_SCLK_POST_EN]	= &mst_d_sclk_post_en.hw,
		[AUD_CLKID_MST_E_SCLK_POST_EN]	= &mst_e_sclk_post_en.hw,
		[AUD_CLKID_MST_F_SCLK_POST_EN]	= &mst_f_sclk_post_en.hw,
		[AUD_CLKID_MST_A_SCLK]		= &mst_a_sclk.hw,
		[AUD_CLKID_MST_B_SCLK]		= &mst_b_sclk.hw,
		[AUD_CLKID_MST_C_SCLK]		= &mst_c_sclk.hw,
		[AUD_CLKID_MST_D_SCLK]		= &mst_d_sclk.hw,
		[AUD_CLKID_MST_E_SCLK]		= &mst_e_sclk.hw,
		[AUD_CLKID_MST_F_SCLK]		= &mst_f_sclk.hw,
		[AUD_CLKID_MST_A_LRCLK_DIV]	= &mst_a_lrclk_div.hw,
		[AUD_CLKID_MST_B_LRCLK_DIV]	= &mst_b_lrclk_div.hw,
		[AUD_CLKID_MST_C_LRCLK_DIV]	= &mst_c_lrclk_div.hw,
		[AUD_CLKID_MST_D_LRCLK_DIV]	= &mst_d_lrclk_div.hw,
		[AUD_CLKID_MST_E_LRCLK_DIV]	= &mst_e_lrclk_div.hw,
		[AUD_CLKID_MST_F_LRCLK_DIV]	= &mst_f_lrclk_div.hw,
		[AUD_CLKID_MST_A_LRCLK]		= &mst_a_lrclk.hw,
		[AUD_CLKID_MST_B_LRCLK]		= &mst_b_lrclk.hw,
		[AUD_CLKID_MST_C_LRCLK]		= &mst_c_lrclk.hw,
		[AUD_CLKID_MST_D_LRCLK]		= &mst_d_lrclk.hw,
		[AUD_CLKID_MST_E_LRCLK]		= &mst_e_lrclk.hw,
		[AUD_CLKID_MST_F_LRCLK]		= &mst_f_lrclk.hw,
		[AUD_CLKID_TDMIN_A_SCLK_SEL]	= &tdmin_a_sclk_sel.hw,
		[AUD_CLKID_TDMIN_B_SCLK_SEL]	= &tdmin_b_sclk_sel.hw,
		[AUD_CLKID_TDMIN_C_SCLK_SEL]	= &tdmin_c_sclk_sel.hw,
		[AUD_CLKID_TDMIN_LB_SCLK_SEL]	= &tdmin_lb_sclk_sel.hw,
		[AUD_CLKID_TDMOUT_A_SCLK_SEL]	= &tdmout_a_sclk_sel.hw,
		[AUD_CLKID_TDMOUT_B_SCLK_SEL]	= &tdmout_b_sclk_sel.hw,
		[AUD_CLKID_TDMOUT_C_SCLK_SEL]	= &tdmout_c_sclk_sel.hw,
		[AUD_CLKID_TDMIN_A_SCLK_PRE_EN]	= &tdmin_a_sclk_pre_en.hw,
		[AUD_CLKID_TDMIN_B_SCLK_PRE_EN]	= &tdmin_b_sclk_pre_en.hw,
		[AUD_CLKID_TDMIN_C_SCLK_PRE_EN]	= &tdmin_c_sclk_pre_en.hw,
		[AUD_CLKID_TDMIN_LB_SCLK_PRE_EN] = &tdmin_lb_sclk_pre_en.hw,
		[AUD_CLKID_TDMOUT_A_SCLK_PRE_EN] = &tdmout_a_sclk_pre_en.hw,
		[AUD_CLKID_TDMOUT_B_SCLK_PRE_EN] = &tdmout_b_sclk_pre_en.hw,
		[AUD_CLKID_TDMOUT_C_SCLK_PRE_EN] = &tdmout_c_sclk_pre_en.hw,
		[AUD_CLKID_TDMIN_A_SCLK_POST_EN] = &tdmin_a_sclk_post_en.hw,
		[AUD_CLKID_TDMIN_B_SCLK_POST_EN] = &tdmin_b_sclk_post_en.hw,
		[AUD_CLKID_TDMIN_C_SCLK_POST_EN] = &tdmin_c_sclk_post_en.hw,
		[AUD_CLKID_TDMIN_LB_SCLK_POST_EN] = &tdmin_lb_sclk_post_en.hw,
		[AUD_CLKID_TDMOUT_A_SCLK_POST_EN] = &tdmout_a_sclk_post_en.hw,
		[AUD_CLKID_TDMOUT_B_SCLK_POST_EN] = &tdmout_b_sclk_post_en.hw,
		[AUD_CLKID_TDMOUT_C_SCLK_POST_EN] = &tdmout_c_sclk_post_en.hw,
		[AUD_CLKID_TDMIN_A_SCLK]	= &tdmin_a_sclk.hw,
		[AUD_CLKID_TDMIN_B_SCLK]	= &tdmin_b_sclk.hw,
		[AUD_CLKID_TDMIN_C_SCLK]	= &tdmin_c_sclk.hw,
		[AUD_CLKID_TDMIN_LB_SCLK]	= &tdmin_lb_sclk.hw,
		[AUD_CLKID_TDMOUT_A_SCLK]	= &tdmout_a_sclk.hw,
		[AUD_CLKID_TDMOUT_B_SCLK]	= &tdmout_b_sclk.hw,
		[AUD_CLKID_TDMOUT_C_SCLK]	= &tdmout_c_sclk.hw,
		[AUD_CLKID_TDMIN_A_LRCLK]	= &tdmin_a_lrclk.hw,
		[AUD_CLKID_TDMIN_B_LRCLK]	= &tdmin_b_lrclk.hw,
		[AUD_CLKID_TDMIN_C_LRCLK]	= &tdmin_c_lrclk.hw,
		[AUD_CLKID_TDMIN_LB_LRCLK]	= &tdmin_lb_lrclk.hw,
		[AUD_CLKID_TDMOUT_A_LRCLK]	= &tdmout_a_lrclk.hw,
		[AUD_CLKID_TDMOUT_B_LRCLK]	= &tdmout_b_lrclk.hw,
		[AUD_CLKID_TDMOUT_C_LRCLK]	= &tdmout_c_lrclk.hw,
		[NR_CLKS] = NULL,
	},
	.num = NR_CLKS,
};

/*
 * Convenience table to populate regmap in .probe()
 * Note that this table is shared between both AXG and G12A,
 * with spdifout_b clocks being exclusive to G12A. Since those
 * clocks are not declared within the AXG onecell table, we do not
 * feel the need to have separate AXG/G12A regmap tables.
 */
static struct clk_regmap *const audio_clk_regmaps[] = {
	&ddr_arb,
	&pdm,
	&tdmin_a,
	&tdmin_b,
	&tdmin_c,
	&tdmin_lb,
	&tdmout_a,
	&tdmout_b,
	&tdmout_c,
	&frddr_a,
	&frddr_b,
	&frddr_c,
	&toddr_a,
	&toddr_b,
	&toddr_c,
	&loopback,
	&spdifin,
	&spdifout,
	&resample,
	&power_detect,
	&spdifout_b,
	&mst_a_mclk_sel,
	&mst_b_mclk_sel,
	&mst_c_mclk_sel,
	&mst_d_mclk_sel,
	&mst_e_mclk_sel,
	&mst_f_mclk_sel,
	&mst_a_mclk_div,
	&mst_b_mclk_div,
	&mst_c_mclk_div,
	&mst_d_mclk_div,
	&mst_e_mclk_div,
	&mst_f_mclk_div,
	&mst_a_mclk,
	&mst_b_mclk,
	&mst_c_mclk,
	&mst_d_mclk,
	&mst_e_mclk,
	&mst_f_mclk,
	&spdifout_clk_sel,
	&spdifout_clk_div,
	&spdifout_clk,
	&spdifout_b_clk_sel,
	&spdifout_b_clk_div,
	&spdifout_b_clk,
	&spdifin_clk_sel,
	&spdifin_clk_div,
	&spdifin_clk,
	&pdm_dclk_sel,
	&pdm_dclk_div,
	&pdm_dclk,
	&pdm_sysclk_sel,
	&pdm_sysclk_div,
	&pdm_sysclk,
	&mst_a_sclk_pre_en,
	&mst_b_sclk_pre_en,
	&mst_c_sclk_pre_en,
	&mst_d_sclk_pre_en,
	&mst_e_sclk_pre_en,
	&mst_f_sclk_pre_en,
	&mst_a_sclk_div,
	&mst_b_sclk_div,
	&mst_c_sclk_div,
	&mst_d_sclk_div,
	&mst_e_sclk_div,
	&mst_f_sclk_div,
	&mst_a_sclk_post_en,
	&mst_b_sclk_post_en,
	&mst_c_sclk_post_en,
	&mst_d_sclk_post_en,
	&mst_e_sclk_post_en,
	&mst_f_sclk_post_en,
	&mst_a_sclk,
	&mst_b_sclk,
	&mst_c_sclk,
	&mst_d_sclk,
	&mst_e_sclk,
	&mst_f_sclk,
	&mst_a_lrclk_div,
	&mst_b_lrclk_div,
	&mst_c_lrclk_div,
	&mst_d_lrclk_div,
	&mst_e_lrclk_div,
	&mst_f_lrclk_div,
	&mst_a_lrclk,
	&mst_b_lrclk,
	&mst_c_lrclk,
	&mst_d_lrclk,
	&mst_e_lrclk,
	&mst_f_lrclk,
	&tdmin_a_sclk_sel,
	&tdmin_b_sclk_sel,
	&tdmin_c_sclk_sel,
	&tdmin_lb_sclk_sel,
	&tdmout_a_sclk_sel,
	&tdmout_b_sclk_sel,
	&tdmout_c_sclk_sel,
	&tdmin_a_sclk_pre_en,
	&tdmin_b_sclk_pre_en,
	&tdmin_c_sclk_pre_en,
	&tdmin_lb_sclk_pre_en,
	&tdmout_a_sclk_pre_en,
	&tdmout_b_sclk_pre_en,
	&tdmout_c_sclk_pre_en,
	&tdmin_a_sclk_post_en,
	&tdmin_b_sclk_post_en,
	&tdmin_c_sclk_post_en,
	&tdmin_lb_sclk_post_en,
	&tdmout_a_sclk_post_en,
	&tdmout_b_sclk_post_en,
	&tdmout_c_sclk_post_en,
	&tdmin_a_sclk,
	&tdmin_b_sclk,
	&tdmin_c_sclk,
	&tdmin_lb_sclk,
	&tdmout_a_sclk,
	&tdmout_b_sclk,
	&tdmout_c_sclk,
	&tdmin_a_lrclk,
	&tdmin_b_lrclk,
	&tdmin_c_lrclk,
	&tdmin_lb_lrclk,
	&tdmout_a_lrclk,
	&tdmout_b_lrclk,
	&tdmout_c_lrclk,
};

static int devm_clk_get_enable(struct device *dev, char *id)
{
	struct clk *clk;
	int ret;

	clk = devm_clk_get(dev, id);
	if (IS_ERR(clk)) {
		ret = PTR_ERR(clk);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get %s", id);
		return ret;
	}

	ret = clk_prepare_enable(clk);
	if (ret) {
		dev_err(dev, "failed to enable %s", id);
		return ret;
	}

	ret = devm_add_action_or_reset(dev,
				       (void(*)(void *))clk_disable_unprepare,
				       clk);
	if (ret) {
		dev_err(dev, "failed to add reset action on %s", id);
		return ret;
	}

	return 0;
}

static int
axg_register_clk_hw_input(struct device *dev,
			  struct clk_hw_onecell_data *audio_hw_onecell_data,
			  const char *name,
			  unsigned int clkid)
{
	char *clk_name;
	struct clk_hw *hw;
	int err = 0;

	clk_name = kasprintf(GFP_KERNEL, "%s", name);
	if (!clk_name)
		return -ENOMEM;

	hw = meson_clk_hw_register_input(dev, name, clk_name, 0);
	if (IS_ERR(hw)) {
		/* It is ok if an input clock is missing */
		if (PTR_ERR(hw) == -ENOENT) {
			dev_dbg(dev, "%s not provided", name);
		} else {
			err = PTR_ERR(hw);
			if (err != -EPROBE_DEFER)
				dev_err(dev, "failed to get %s clock", name);
		}
	} else {
		audio_hw_onecell_data->hws[clkid] = hw;
	}

	kfree(clk_name);
	return err;
}

static int
axg_register_clk_hw_inputs(struct device *dev,
			   struct clk_hw_onecell_data *audio_hw_onecell_data,
			   const char *basename,
			   unsigned int count,
			   unsigned int clkid)
{
	char *name;
	int i, ret;

	for (i = 0; i < count; i++) {
		name = kasprintf(GFP_KERNEL, "%s%d", basename, i);
		if (!name)
			return -ENOMEM;

		ret = axg_register_clk_hw_input(dev, audio_hw_onecell_data,
						name, clkid + i);
		kfree(name);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct regmap_config audio_regmap_cfg = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= AUDIO_CLK_PDMIN_CTRL1,
};

struct audioclk_data {
	struct clk_hw_onecell_data *hw_onecell_data;
};

static int axg_audio_clkc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct audioclk_data *data;
	struct regmap *map;
	struct resource *res;
	void __iomem *regs;
	struct clk_hw *hw;
	int ret, i;

	data = of_device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	map = devm_regmap_init_mmio(dev, regs, &audio_regmap_cfg);
	if (IS_ERR(map)) {
		dev_err(dev, "failed to init regmap: %ld\n", PTR_ERR(map));
		return PTR_ERR(map);
	}

	/* Get the mandatory peripheral clock */
	ret = devm_clk_get_enable(dev, "pclk");
	if (ret)
		return ret;

	ret = device_reset(dev);
	if (ret) {
		dev_err(dev, "failed to reset device\n");
		return ret;
	}

	/* Register the peripheral input clock */
	hw = meson_clk_hw_register_input(dev, "pclk", "audio_pclk", 0);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	data->hw_onecell_data->hws[AUD_CLKID_PCLK] = hw;

	/* Register optional input master clocks */
	ret = axg_register_clk_hw_inputs(dev, data->hw_onecell_data,
					 "mst_in",
					 AXG_MST_IN_COUNT,
					 AUD_CLKID_MST0);
	if (ret)
		return ret;

	/* Register optional input slave sclks */
	ret = axg_register_clk_hw_inputs(dev, data->hw_onecell_data,
					 "slv_sclk",
					 AXG_SLV_SCLK_COUNT,
					 AUD_CLKID_SLV_SCLK0);
	if (ret)
		return ret;

	/* Register optional input slave lrclks */
	ret = axg_register_clk_hw_inputs(dev, data->hw_onecell_data,
					 "slv_lrclk",
					 AXG_SLV_LRCLK_COUNT,
					 AUD_CLKID_SLV_LRCLK0);
	if (ret)
		return ret;

	/* Populate regmap for the regmap backed clocks */
	for (i = 0; i < ARRAY_SIZE(audio_clk_regmaps); i++)
		audio_clk_regmaps[i]->map = map;

	/* Take care to skip the registered input clocks */
	for (i = AUD_CLKID_DDR_ARB; i < data->hw_onecell_data->num; i++) {
		hw = data->hw_onecell_data->hws[i];
		/* array might be sparse */
		if (!hw)
			continue;

		ret = devm_clk_hw_register(dev, hw);
		if (ret) {
			dev_err(dev, "failed to register clock %s\n",
				hw->init->name);
			return ret;
		}
	}

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					   data->hw_onecell_data);
}

static const struct audioclk_data axg_audioclk_data = {
	.hw_onecell_data = &axg_audio_hw_onecell_data,
};

static const struct audioclk_data g12a_audioclk_data = {
	.hw_onecell_data = &g12a_audio_hw_onecell_data,
};

static const struct of_device_id clkc_match_table[] = {
	{ .compatible = "amlogic,axg-audio-clkc",
	  .data = &axg_audioclk_data },
	{ .compatible = "amlogic,g12a-audio-clkc",
	  .data = &g12a_audioclk_data },
	{}
};
MODULE_DEVICE_TABLE(of, clkc_match_table);

static struct platform_driver axg_audio_driver = {
	.probe		= axg_audio_clkc_probe,
	.driver		= {
		.name	= "axg-audio-clkc",
		.of_match_table = clkc_match_table,
	},
};
module_platform_driver(axg_audio_driver);

MODULE_DESCRIPTION("Amlogic A113x/G12A Audio Clock driver");
MODULE_AUTHOR("Jerome Brunet <jbrunet@baylibre.com>");
MODULE_LICENSE("GPL v2");
