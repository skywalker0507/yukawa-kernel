// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019 BayLibre, SAS
 * Author: Maxime Jourdan <mjourdan@baylibre.com>
 */

#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "vdec_helpers.h"
#include "dos_regs.h"
#include "h264_dpb.h"

#define SIZE_WORKSPACE		0x200000
#define DCAC_READ_MARGIN	(64 * 1024)
#define DEF_BUF_START_ADDR	0x1000000
#define SIZE_LMEM		PAGE_SIZE
#define SIZE_RPM		0x400
#define SIZE_AUX		16 * SZ_1K

#define MAX_VF_BUF_NUM		27
#define MAX_SIZE_4K		(4096 * 2304)

#define SIZE_EXT_FW		(36 * SZ_1K)
#define MC_OFFSET_MAIN		0x5000

/* h264_multi register mapping */
#define INIT_FLAG_REG		AV_SCRATCH_2
#define HEAD_PADING_REG		AV_SCRATCH_3
#define UCODE_WATCHDOG_REG	AV_SCRATCH_7
#define NAL_SEARCH_CTL		AV_SCRATCH_9
#define LMEM_DUMP_ADR		AV_SCRATCH_L
#define DEBUG_REG1		AV_SCRATCH_M
#define DEBUG_REG2		AV_SCRATCH_N
#define FRAME_COUNTER_REG	AV_SCRATCH_I
#define RPM_CMD_REG		AV_SCRATCH_A
#define H264_DECODE_SIZE	AV_SCRATCH_E
#define H264_DECODE_MODE	AV_SCRATCH_4
#define H264_DECODE_SEQINFO	AV_SCRATCH_5
#define H264_AUX_ADR		AV_SCRATCH_C
#define H264_AUX_DATA_SIZE	AV_SCRATCH_H
#define DPB_STATUS_REG		AV_SCRATCH_J

#define H264_DECODE_INFO	M4_CONTROL_REG
#define H264_BUFFER_INFO_INDEX	PMV3_X
#define H264_BUFFER_INFO_DATA	PMV2_X
#define H264_CURRENT_POC_IDX_RESET	LAST_SLICE_MV_ADDR
#define H264_CURRENT_POC	LAST_MVY
#define H264_CO_MB_WR_ADDR	VLD_C38
#define H264_CO_MB_RD_ADDR	VLD_C39
#define H264_CO_MB_RW_CTL	VLD_C3D

#define DECODE_MODE_MULTI_FRAMEBASE	0x1

enum state {
	STATE_IDLE = 0,
	STATE_PROCESSING,
};

struct frame_data {
	unsigned int info0;
	unsigned int info1;
	unsigned int info2;
};

struct codec_h264_multi {
	/* H.264 decoder requires an extended firmware */
	void      *ext_fw_vaddr;
	dma_addr_t ext_fw_paddr;

	/* Buffer for the H.264 Workspace */
	void      *workspace_vaddr;
	dma_addr_t workspace_paddr;

	/* Buffer for the RPM dump */
	void      *lmem_vaddr;
	dma_addr_t lmem_paddr;

	/* Buffer for the H.264 auxiliary data */
	void      *aux_vaddr;
	dma_addr_t aux_paddr;

	/* Buffer for the collocated MVs */
	void 	  *collocate_cma_vaddr;
	dma_addr_t collocate_cma_paddr;
	u32	   collocate_cma_size;

	struct h264_dpb_stru dpb;
	struct frame_data frame_data[32];
	u32 seq_info;
	u32 seq_info2;
	u32 crop_infor;
	u32 reg_val;
	u32 mb_total;
	u32 mb_width;
	u32 mb_height;
	u32 frame_width;
	u32 frame_height;
	u32 max_reference_size;
	u32 no_poc_reorder_flag;

	u32 data_flag;
	u32 dec_flag;
	enum state state;
};

static int codec_h264_start(struct amvdec_session *sess)
{
	struct codec_h264_multi *h264 = sess->priv;
	struct amvdec_core *core = sess->core;

	h264->workspace_vaddr =
		dma_alloc_coherent(core->dev, SIZE_WORKSPACE,
				   &h264->workspace_paddr, GFP_KERNEL);
	if (!h264->workspace_vaddr)
		return -ENOMEM;

	h264->lmem_vaddr =
		dma_alloc_coherent(core->dev, SIZE_LMEM,
				   &h264->lmem_paddr, GFP_KERNEL);
	if (!h264->lmem_vaddr)
		return -ENOMEM;

	h264->aux_vaddr =
		dma_alloc_coherent(core->dev, SIZE_AUX,
				   &h264->aux_paddr, GFP_KERNEL);
	if (!h264->aux_vaddr)
		return -ENOMEM;

	amvdec_write_dos_bits(core, POWER_CTL_VLD, BIT(9) | BIT(6));
	amvdec_write_dos(core, MDEC_PIC_DC_THRESH, 0x404038aa);
	amvdec_write_dos(core, PSCALE_CTRL, 0);
	amvdec_write_dos(core, NAL_SEARCH_CTL, 0);
	amvdec_write_dos(core, AV_SCRATCH_0, 0);
	amvdec_write_dos(core, AV_SCRATCH_8, h264->workspace_paddr -
					     DEF_BUF_START_ADDR +
					     DCAC_READ_MARGIN);
	amvdec_write_dos(core, AV_SCRATCH_G, h264->ext_fw_paddr);
	amvdec_write_dos(core, LMEM_DUMP_ADR, (u32)h264->lmem_paddr);
	amvdec_write_dos(core, H264_AUX_ADR, h264->aux_paddr);
	amvdec_write_dos(core, H264_AUX_DATA_SIZE, (SIZE_AUX >> 4) << 16);
	amvdec_write_dos(core, H264_DECODE_MODE, DECODE_MODE_MULTI_FRAMEBASE);
	amvdec_write_dos(core, INIT_FLAG_REG, 1);

	return 0;
}

/* Process a new input packet buffer */
static void codec_h264_notify(struct amvdec_session *sess, int size)
{
	struct amvdec_core *core = sess->core;
	struct codec_h264_multi *h264 = sess->priv;

	if (h264->state != STATE_IDLE) {
		dev_err(core->dev, "codec_h264_notify called when not idle\n");
		return;
	}

	h264->state = STATE_PROCESSING;
	amvdec_write_dos(core, H264_DECODE_INFO, BIT(13));
	amvdec_write_dos(core, H264_DECODE_SIZE, size);
	amvdec_write_dos(core, VIFF_BIT_CNT, size * 8);
	amvdec_write_dos(core, DPB_STATUS_REG, H264_ACTION_SEARCH_HEAD);
}

static int codec_h264_stop(struct amvdec_session *sess)
{
	struct codec_h264_multi *h264 = sess->priv;
	struct amvdec_core *core = sess->core;

	if (h264->ext_fw_vaddr)
		dma_free_coherent(core->dev, SIZE_EXT_FW,
				  h264->ext_fw_vaddr, h264->ext_fw_paddr);

	if (h264->workspace_vaddr)
		dma_free_coherent(core->dev, SIZE_WORKSPACE,
				  h264->workspace_vaddr, h264->workspace_paddr);

	if (h264->lmem_vaddr)
		dma_free_coherent(core->dev, SIZE_LMEM,
				  h264->lmem_vaddr, h264->lmem_paddr);

	if (h264->collocate_cma_vaddr)
		dma_free_coherent(core->dev, h264->collocate_cma_size,
				  h264->collocate_cma_vaddr, h264->collocate_cma_paddr);

	return 0;
}

static int codec_h264_load_extended_firmware(struct amvdec_session *sess,
					     const u8 *data, u32 len)
{
	struct codec_h264_multi *h264;
	struct amvdec_core *core = sess->core;

	if (len < SIZE_EXT_FW)
		return -EINVAL;

	h264 = kzalloc(sizeof(*h264), GFP_KERNEL);
	if (!h264)
		return -ENOMEM;

	h264->ext_fw_vaddr = dma_alloc_coherent(core->dev, SIZE_EXT_FW,
					       &h264->ext_fw_paddr, GFP_KERNEL);
	if (!h264->ext_fw_vaddr) {
		kfree(h264);
		return -ENOMEM;
	}

	memcpy(h264->ext_fw_vaddr, data + 0x4000, SIZE_EXT_FW - 0x4000);
	memcpy(h264->ext_fw_vaddr + MC_OFFSET_MAIN, data, 0x2000);
	memcpy(h264->ext_fw_vaddr + MC_OFFSET_MAIN + 0x2000, data + 0x5000, 0x1000);
	memcpy(h264->ext_fw_vaddr + MC_OFFSET_MAIN + 0x3000, data + 0x7000, 0x1000);
	sess->priv = h264;

	return 0;
}

static void codec_h264_resume(struct amvdec_session *sess, int changed)
{
	struct amvdec_core *core = sess->core;
	struct codec_h264_multi *h264 = sess->priv;

	if (changed) {
	  	h264->collocate_cma_size = h264->dpb.colocated_buf_size *
	  				   h264->max_reference_size;
	  	h264->collocate_cma_vaddr =
	  		dma_alloc_coherent(core->dev,
	  				   h264->collocate_cma_size,
	  				   &h264->collocate_cma_paddr,
	  				   GFP_KERNEL);
		if (!h264->collocate_cma_vaddr) {
			amvdec_abort(sess);
			return;
		}
		h264->dpb.colocated_mv_addr_start = h264->collocate_cma_paddr;
	  	h264->dpb.colocated_mv_addr_end  =
	  		h264->dpb.colocated_mv_addr_start +
	  		h264->collocate_cma_size;

		amvdec_set_canvases(sess, (u32[]){ ANC0_CANVAS_ADDR, 0 },
					  (u32[]){ 24, 0 });
	}

	amvdec_write_dos(core, DPB_STATUS_REG, H264_ACTION_CONFIG_DONE);
	amvdec_write_dos(core, AV_SCRATCH_0,
			 (h264->max_reference_size << 24) |
			 (h264->dpb.mDPB.size << 16) |
			 (h264->dpb.mDPB.size << 8));
}

static void h264_load_rpm(struct amvdec_session *sess)
{
	struct codec_h264_multi *h264 = sess->priv;
	unsigned short *p = (unsigned short *)h264->lmem_vaddr;
	int i;

	for (i = 0; i < SIZE_RPM; i += 4) {
		int j;
		for (j = 0; j < 4; j++) {
			h264->dpb.dpb_param.l.data[i+j] =
				p[i+3-j];
		}
	}

	h264->dpb.bitstream_restriction_flag =
		(h264->dpb.dpb_param.l.data[SPS_FLAGS2] >> 3) & 0x1;
	h264->dpb.num_reorder_frames =
		h264->dpb.dpb_param.l.data[NUM_REORDER_FRAMES];
	h264->dpb.max_dec_frame_buffering =
		h264->dpb.dpb_param.l.data[MAX_BUFFER_FRAME];
}

static int is_oversize(int w, int h)
{
	int max = MAX_SIZE_4K;

	if (w < 0 || h < 0)
		return true;

	if (h != 0 && (w > max / h))
		return true;

	return false;
}

static int get_max_dec_frame_buf_size(int level_idc,
		int max_reference_frame_num, int mb_width,
		int mb_height)
{
	int pic_size = mb_width * mb_height * 384;

	int size = 0;

	switch (level_idc) {
	case 9:
		size = 152064;
		break;
	case 10:
		size = 152064;
		break;
	case 11:
		size = 345600;
		break;
	case 12:
		size = 912384;
		break;
	case 13:
		size = 912384;
		break;
	case 20:
		size = 912384;
		break;
	case 21:
		size = 1824768;
		break;
	case 22:
		size = 3110400;
		break;
	case 30:
		size = 3110400;
		break;
	case 31:
		size = 6912000;
		break;
	case 32:
		size = 7864320;
		break;
	case 40:
		size = 12582912;
		break;
	case 41:
		size = 12582912;
		break;
	case 42:
		size = 13369344;
		break;
	case 50:
		size = 42393600;
		break;
	case 51:
	case 52:
	default:
		size = 70778880;
		break;
	}

	size /= pic_size;
	size = size + 1;

	if (max_reference_frame_num > size)
		size = max_reference_frame_num;

	return size;
}

/* Called by DPB when a buffer is ready for display */
static void h264_frame_done(struct amvdec_session *sess, struct FrameStore *fs)
{
	printk("Frame DONE: %u\n", fs->frame->vbuf->vb2_buf.index);
	amvdec_dst_buf_done(sess, fs->frame->vbuf, V4L2_FIELD_NONE);
}

static int vh264_set_params(struct amvdec_session *sess,
	u32 param1, u32 param2, u32 param3, u32 param4)
{
	struct amvdec_core *core = sess->core;
	struct codec_h264_multi *h264 = sess->priv;
	int mb_width, mb_total;
	int max_reference_size, level_idc;
	int mb_height = 0;
	u32 seq_info2;
	int ret = 0;
	int active_buffer_spec_num;
	unsigned int frame_mbs_only_flag;
	unsigned int chroma_format_idc, chroma444;
	unsigned int crop_infor, crop_bottom, crop_right;
	unsigned int used_reorder_dpb_size_margin = 6;
	u32 reg_val;

	seq_info2 = param1;
	h264->seq_info = param2;

	mb_width = seq_info2 & 0xff;
	mb_total = (seq_info2 >> 8) & 0xffff;
	if (!mb_width && mb_total) /*for 4k2k*/
		mb_width = 256;
	if (mb_width)
		mb_height = mb_total/mb_width;
	if (mb_width <= 0 || mb_height <= 0 ||
		is_oversize(mb_width << 4, mb_height << 4)) {
		amvdec_write_dos(core, AV_SCRATCH_0, (h264->max_reference_size<<24) |
			(h264->dpb.mDPB.size<<16) |
			(h264->dpb.mDPB.size<<8));
		return 0;
	}

	if (!h264->seq_info2 || (seq_info2 && h264->seq_info2 != (seq_info2 & (~0x80000000)))) {
		h264->seq_info2 = seq_info2 & (~0x80000000);

		dpb_init_global(&h264->dpb,
			0, 0, 0);
		h264->dpb.sess = sess;
		h264->dpb.frame_done_cb = h264_frame_done;

		frame_mbs_only_flag = (h264->seq_info >> 15) & 0x01;
		chroma_format_idc = (h264->seq_info >> 13) & 0x03;
		chroma444 = (chroma_format_idc == 3) ? 1 : 0;
		crop_infor = param3;
		crop_bottom = (crop_infor & 0xff) >> (2 - frame_mbs_only_flag);
		crop_right = ((crop_infor >> 16) & 0xff)
			>> (2 - frame_mbs_only_flag);

		h264->dpb.mSPS.frame_mbs_only_flag = frame_mbs_only_flag;
		h264->frame_width = mb_width << 4;
		h264->frame_height = mb_height << 4;
		if (frame_mbs_only_flag) {
			h264->frame_height =
				h264->frame_height - (2 >> chroma444) *
				min(crop_bottom,
					(unsigned int)((8 << chroma444) - 1));
			h264->frame_width =
				h264->frame_width -
					(2 >> chroma444) * min(crop_right,
						(unsigned
						 int)((8 << chroma444) - 1));
		} else {
			h264->frame_height =
				h264->frame_height - (4 >> chroma444) *
				min(crop_bottom,
					(unsigned int)((8 << chroma444)
							  - 1));
			h264->frame_width =
				h264->frame_width -
				(4 >> chroma444) * min(crop_right,
				(unsigned int)((8 << chroma444) - 1));
		}

		if (h264->frame_height == 1088)
			h264->frame_height = 1080;

		mb_width = (mb_width+3) & 0xfffffffc;
		mb_height = (mb_height+3) & 0xfffffffc;
		mb_total = mb_width * mb_height;

		reg_val = param4;
		level_idc = reg_val & 0xff;
		max_reference_size = (reg_val >> 8) & 0xff;

		h264->dpb.colocated_buf_size = mb_total * 96;
		h264->mb_total = mb_total;
		h264->mb_width = mb_width;
		h264->mb_height = mb_height;

		h264->dpb.reorder_pic_num =
			get_max_dec_frame_buf_size(level_idc,
			max_reference_size, mb_width, mb_height);

		if ((h264->dpb.bitstream_restriction_flag) &&
			(h264->dpb.max_dec_frame_buffering <
			h264->dpb.reorder_pic_num)) {
			h264->dpb.reorder_pic_num = h264->dpb.max_dec_frame_buffering;
		}

		active_buffer_spec_num =
			h264->dpb.reorder_pic_num
			+ 16;
		h264->max_reference_size =
			max_reference_size + 14;

		if (active_buffer_spec_num > MAX_VF_BUF_NUM) {
			active_buffer_spec_num = MAX_VF_BUF_NUM;
			h264->dpb.reorder_pic_num = active_buffer_spec_num
				- 16;
		}
		h264->dpb.mDPB.size = active_buffer_spec_num;
		if (h264->max_reference_size > MAX_VF_BUF_NUM)
			h264->max_reference_size = MAX_VF_BUF_NUM;
		h264->dpb.max_reference_size = h264->max_reference_size;

		if (h264->no_poc_reorder_flag)
			h264->dpb.reorder_pic_num = 1;
	}

	printk("frame: %ux%u, max_reference_size: %u, h264->dpb.mDPB.size: %u\n", h264->frame_width, h264->frame_height,
			  h264->max_reference_size, h264->dpb.mDPB.size);
	amvdec_src_change(sess, h264->frame_width, h264->frame_height,
			  h264->dpb.mDPB.size);

	return ret;
}

static void h264_config_request(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;
	u32 param1 = amvdec_read_dos(core, AV_SCRATCH_1);
	u32 param2 = amvdec_read_dos(core, AV_SCRATCH_2);
	u32 param3 = amvdec_read_dos(core, AV_SCRATCH_6);
	u32 param4 = amvdec_read_dos(core, AV_SCRATCH_B);

	printk("h264_config_request VIFF_BIT_CNT:%u\n", amvdec_read_dos(core, VIFF_BIT_CNT)/8);
	h264_load_rpm(sess);
	vh264_set_params(sess, param1, param2, param3, param4);
}

static int config_decode_buf(struct amvdec_session *sess, struct StorablePicture *pic)
{
	struct amvdec_core *core = sess->core;
	struct codec_h264_multi *h264 = sess->priv;
	struct h264_dpb_stru *p_H264_Dpb = &h264->dpb;
	struct Slice *pSlice = &(p_H264_Dpb->mSlice);
	int ret = 0;
	unsigned int colocate_adr_offset;
	unsigned int val;
	unsigned long canvas_adr;
	unsigned int ref_reg_val;
	unsigned int one_ref_cfg = 0;
	int h264_buffer_info_data_write_count;
	int i, j;
	unsigned int colocate_wr_adr;
	unsigned int colocate_rd_adr;
	unsigned char use_direct_8x8;
	int canvas_pos;

	canvas_pos = sess->vb2_idx_to_fw_idx[pic->buf_spec_num];
	amvdec_write_dos(core, H264_CURRENT_POC_IDX_RESET, 0);
	amvdec_write_dos(core, H264_CURRENT_POC, pic->frame_poc);
	amvdec_write_dos(core, H264_CURRENT_POC, pic->top_poc);
	amvdec_write_dos(core, H264_CURRENT_POC, pic->bottom_poc);

	amvdec_write_dos(core, CURR_CANVAS_CTRL, canvas_pos << 24);
	canvas_adr = amvdec_read_dos(core, CURR_CANVAS_CTRL) & 0xffffff;

	amvdec_write_dos(core, REC_CANVAS_ADDR, canvas_adr);
	amvdec_write_dos(core, DBKR_CANVAS_ADDR, canvas_adr);
	amvdec_write_dos(core, DBKW_CANVAS_ADDR, canvas_adr);

	if (pic->mb_aff_frame_flag)
		h264->frame_data[pic->buf_spec_num].info0 = 0xf4c0;
	else if (pic->structure == TOP_FIELD)
		h264->frame_data[pic->buf_spec_num].info0 = 0xf400;
	else if (pic->structure == BOTTOM_FIELD)
		h264->frame_data[pic->buf_spec_num].info0 = 0xf440;
	else
		h264->frame_data[pic->buf_spec_num].info0 = 0xf480;

	if (pic->bottom_poc < pic->top_poc)
		h264->frame_data[pic->buf_spec_num].info0 |= 0x100;

	h264->frame_data[pic->buf_spec_num].info1 = pic->top_poc;
	h264->frame_data[pic->buf_spec_num].info2 = pic->bottom_poc;
	amvdec_write_dos(core, H264_BUFFER_INFO_INDEX, 16);

	for (j = 0; j < h264->dpb.mDPB.size; j++) {
		int long_term_flag;
		i = sess->fw_idx_to_vb2_idx[j];
		if (i < 0)
			break;
		long_term_flag =
			get_long_term_flag_by_buf_spec_num(p_H264_Dpb, i);
		if (long_term_flag > 0) {
			if (long_term_flag & 0x1)
				h264->frame_data[i].info0 |= (1 << 4);
			else
				h264->frame_data[i].info0 &= ~(1 << 4);

			if (long_term_flag & 0x2)
				h264->frame_data[i].info0 |= (1 << 5);
			else
				h264->frame_data[i].info0 &= ~(1 << 5);
		}

		if (i == pic->buf_spec_num)
			amvdec_write_dos(core, H264_BUFFER_INFO_DATA,
				h264->frame_data[i].info0 | 0xf);
		else
			amvdec_write_dos(core, H264_BUFFER_INFO_DATA,
				h264->frame_data[i].info0);
		amvdec_write_dos(core, H264_BUFFER_INFO_DATA, h264->frame_data[i].info1);
		amvdec_write_dos(core, H264_BUFFER_INFO_DATA, h264->frame_data[i].info2);
	}

	amvdec_write_dos(core, H264_BUFFER_INFO_INDEX, 0);
	ref_reg_val = 0;
	j = 0;
	h264_buffer_info_data_write_count = 0;

	for (i = 0; i < (unsigned int)(pSlice->listXsize[0]); i++) {
		struct StorablePicture *ref = pSlice->listX[0][i];
		unsigned int cfg;
		canvas_pos = sess->vb2_idx_to_fw_idx[ref->buf_spec_num];

		if (ref->structure == TOP_FIELD)
			cfg = 0x1;
		else if (ref->structure == BOTTOM_FIELD)
			cfg = 0x2;
		else
			cfg = 0x3;

		one_ref_cfg = (canvas_pos & 0x1f) | (cfg << 5);
		ref_reg_val <<= 8;
		ref_reg_val |= one_ref_cfg;
		j++;

		if (j == 4) {
			amvdec_write_dos(core, H264_BUFFER_INFO_DATA, ref_reg_val);
			h264_buffer_info_data_write_count++;
			j = 0;
		}
	}
	if (j != 0) {
		while (j != 4) {
			ref_reg_val <<= 8;
			ref_reg_val |= one_ref_cfg;
			j++;
		}
		amvdec_write_dos(core, H264_BUFFER_INFO_DATA, ref_reg_val);
		h264_buffer_info_data_write_count++;
	}
	ref_reg_val = (one_ref_cfg << 24) | (one_ref_cfg<<16) |
				(one_ref_cfg << 8) | one_ref_cfg;
	for (i = h264_buffer_info_data_write_count; i < 8; i++)
		amvdec_write_dos(core, H264_BUFFER_INFO_DATA, ref_reg_val);

	amvdec_write_dos(core, H264_BUFFER_INFO_INDEX, 8);
	ref_reg_val = 0;
	j = 0;

	for (i = 0; i < (unsigned int)(pSlice->listXsize[1]); i++) {
		struct StorablePicture *ref = pSlice->listX[1][i];
		unsigned int cfg;

		canvas_pos = sess->vb2_idx_to_fw_idx[ref->buf_spec_num];
		if (ref->structure == TOP_FIELD)
			cfg = 0x1;
		else if (ref->structure == BOTTOM_FIELD)
			cfg = 0x2;
		else
			cfg = 0x3;

		one_ref_cfg = (canvas_pos & 0x1f) | (cfg << 5);
		ref_reg_val <<= 8;
		ref_reg_val |= one_ref_cfg;
		j++;

		if (j == 4) {
			amvdec_write_dos(core, H264_BUFFER_INFO_DATA, ref_reg_val);
			j = 0;
		}
	}
	if (j != 0) {
		while (j != 4) {
			ref_reg_val <<= 8;
			ref_reg_val |= one_ref_cfg;
			j++;
		}
		amvdec_write_dos(core, H264_BUFFER_INFO_DATA, ref_reg_val);
	}

	while ((amvdec_read_dos(core, H264_CO_MB_RW_CTL) >> 11) & 0x1)
		;
	if ((pSlice->mode_8x8_flags & 0x4) &&
		(pSlice->mode_8x8_flags & 0x2))
		use_direct_8x8 = 1;
	else
		use_direct_8x8 = 0;

	colocate_adr_offset =
		((pic->structure == FRAME && pic->mb_aff_frame_flag == 0)
		 ? 1 : 2) * 96;
	if (use_direct_8x8)
		colocate_adr_offset >>= 2;

	colocate_adr_offset *= pSlice->first_mb_in_slice;

	if ((pic->colocated_buf_index >= 0) &&
		(pic->colocated_buf_index < p_H264_Dpb->colocated_buf_count)) {
		colocate_wr_adr = p_H264_Dpb->colocated_mv_addr_start +
			((p_H264_Dpb->colocated_buf_size *
			pic->colocated_buf_index)
			>> (use_direct_8x8 ? 2 : 0));
		if ((colocate_wr_adr + p_H264_Dpb->colocated_buf_size) >
			p_H264_Dpb->colocated_mv_addr_end) {
			ret = -3;
		}
		val = colocate_wr_adr + colocate_adr_offset;
		amvdec_write_dos(core, H264_CO_MB_WR_ADDR, val);
	} else {
		amvdec_write_dos(core, H264_CO_MB_WR_ADDR, 0xffffffff);
	}

	if (pSlice->listXsize[1] > 0) {
		struct StorablePicture *colocate_pic = pSlice->listX[1][0];
		int l10_structure;
		int cur_colocate_ref_type;
		unsigned int val;

		if (colocate_pic->mb_aff_frame_flag)
			l10_structure = 3;
		else {
			if (colocate_pic->coded_frame)
				l10_structure = 2;
			else
				l10_structure =	(colocate_pic->structure ==
					BOTTOM_FIELD) ?	1 : 0;
		}

		if (pic->structure == FRAME  || pic->mb_aff_frame_flag) {
			cur_colocate_ref_type =
				(abs(pic->poc - colocate_pic->top_poc)
				< abs(pic->poc -
				colocate_pic->bottom_poc)) ? 0 : 1;
		} else
			cur_colocate_ref_type =
				(colocate_pic->structure
					== BOTTOM_FIELD) ? 1 : 0;

		if ((colocate_pic->colocated_buf_index >= 0) &&
			(colocate_pic->colocated_buf_index <
				p_H264_Dpb->colocated_buf_count)) {
			colocate_rd_adr = p_H264_Dpb->colocated_mv_addr_start +
				((p_H264_Dpb->colocated_buf_size *
				colocate_pic->colocated_buf_index)
				>> (use_direct_8x8 ? 2 : 0));
			if ((colocate_rd_adr + p_H264_Dpb->colocated_buf_size) >
				p_H264_Dpb->colocated_mv_addr_end) {
				ret = -6;
			}

			val = ((colocate_rd_adr+colocate_adr_offset) >> 3) |
				(l10_structure << 30) |
				(cur_colocate_ref_type << 29);
			amvdec_write_dos(core, H264_CO_MB_RD_ADDR, val);
		} else {
			ret = -7;
		}
	}
	return ret;
}

static void h264_slice_head_done(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;
	struct codec_h264_multi *h264 = sess->priv;
	u32 slice_header_process_status;
	int ret;

	h264_load_rpm(sess);
	slice_header_process_status = h264_slice_header_process(&h264->dpb);
	if (!h264->dpb.mVideo.dec_picture) {
		dev_err(core->dev, "H264 decoding picture is NULL\n");
		amvdec_abort(sess);
		return;
	}

	ret = config_decode_buf(sess, h264->dpb.mVideo.dec_picture);
	if (ret) {
		dev_err(core->dev, "H264 config_decode_buf failed %d", ret);
		amvdec_abort(sess);
		return;
	}

	if (slice_header_process_status == 1) {
		amvdec_write_dos(core, DPB_STATUS_REG,
				 H264_ACTION_DECODE_NEWPIC);
		h264->data_flag = 0;
		if (h264->dpb.dpb_param.l.data[SLICE_TYPE] == I_Slice)
			h264->data_flag |= I_FLAG;
		if ((h264->dpb.dpb_param.dpb.NAL_info_mmco & 0x1f) == 5)
			h264->data_flag |= IDR_FLAG;
	 } else {
		amvdec_write_dos(core, DPB_STATUS_REG,
				 H264_ACTION_DECODE_SLICE);
	 }
}

static void h264_pic_data_done(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;
	struct codec_h264_multi *h264 = sess->priv;
	struct StorablePicture *pic =
		h264->dpb.mVideo.dec_picture;
	u32 offset = pic->offset_delimiter_lo |
		(pic->offset_delimiter_hi << 16);
	int ret;

	ret = store_picture_in_dpb(&h264->dpb,
		h264->dpb.mVideo.dec_picture,
		h264->data_flag | h264->dec_flag |
		h264->dpb.mVideo.dec_picture->data_flag);
	if (ret) {
		dev_err(core->dev,
			"H264 store_picture_in_dpb failed: %d\n", ret);
		amvdec_abort(sess);
		return;
	}
	bufmgr_post(&h264->dpb);
	h264->dpb.mVideo.dec_picture = NULL;
	h264->dpb.decode_pic_count++;

	/* Check whether there is unprocessed bitstream left in the input */
	if (amvdec_read_dos(core, VIFF_BIT_CNT) > 0) {
		printk("%u bytes remain\n", amvdec_read_dos(core, VIFF_BIT_CNT)/8);
		amvdec_write_dos(core, DPB_STATUS_REG, H264_ACTION_SEARCH_HEAD);
	} else {
		h264->state = STATE_IDLE;
		schedule_work(&sess->esparser_queue_work);
	}
}

static irqreturn_t codec_h264_threaded_isr(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;
	struct codec_h264_multi *h264 = sess->priv;
	u32 dec_dpb_status = amvdec_read_dos(core, DPB_STATUS_REG);

	switch (dec_dpb_status) {
	case H264_CONFIG_REQUEST:
		h264_config_request(sess);
		break;
	case H264_DATA_REQUEST:
		h264->state = STATE_IDLE;
		schedule_work(&sess->esparser_queue_work);
		break;
	case H264_SLICE_HEAD_DONE:
		h264_slice_head_done(sess);
		break;
	case H264_PIC_DATA_DONE:
		h264_pic_data_done(sess);
		break;
	case H264_DECODE_BUFEMPTY:
	case H264_SEARCH_BUFEMPTY: /* fallthrough */
		h264->state = STATE_IDLE;
		schedule_work(&sess->esparser_queue_work);
		break;
	default:
		dev_err(core->dev,
			"Unhandled H264 MULTI ISR code %08X", dec_dpb_status);
		amvdec_abort(sess);
		return IRQ_HANDLED;
	}

	if (amvdec_read_dos(core, AV_SCRATCH_G) == 1)
		amvdec_write_dos(core, AV_SCRATCH_G, 0);

	return IRQ_HANDLED;
}

static irqreturn_t codec_h264_isr(struct amvdec_session *sess)
{
	struct amvdec_core *core = sess->core;

	amvdec_write_dos(core, ASSIST_MBOX1_CLR_REG, 0);

	return IRQ_WAKE_THREAD;
}

static u32 codec_h264_input_ready(struct amvdec_session *sess)
{
	struct codec_h264_multi *h264 = sess->priv;

	return h264->state == STATE_IDLE;
}

static int codec_h264_can_recycle(struct amvdec_core *core)
{
	return 1;
}

static void codec_h264_recycle(struct amvdec_core *core, u32 index)
{
	struct codec_h264_multi *h264 = core->cur_sess->priv;

	printk("Recycling %d\n", index);
	set_frame_output_flag(&h264->dpb, index);
}

struct amvdec_codec_ops codec_h264_multi_ops = {
	.start = codec_h264_start,
	.stop = codec_h264_stop,
	.load_extended_firmware = codec_h264_load_extended_firmware,
	.isr = codec_h264_isr,
	.threaded_isr = codec_h264_threaded_isr,
	.resume = codec_h264_resume,
	.notify = codec_h264_notify,
	.input_ready = codec_h264_input_ready,
	.can_recycle = codec_h264_can_recycle,
	.recycle = codec_h264_recycle,
};
