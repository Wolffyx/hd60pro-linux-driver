// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 control-field discovery, readback, and synchronization.
 */

#include "mz0380-internal.h"

u32 mz_read(struct mz0380_dev *dev, int map, u32 reg)
{
	return readl(dev->lmmio[map] + reg);
}

void mz_write(struct mz0380_dev *dev, int map, u32 reg, u32 value)
{
	writel(value, dev->lmmio[map] + reg);
}

u32 mz0380_cfg_trace_reg(unsigned int index)
{
	return MZ0380_CFG_TRACE_START + (index * sizeof(u32));
}

static void mz0380_capture_cfg_trace(struct mz0380_dev *dev, u32 *trace)
{
	unsigned int i;

	for (i = 0; i < MZ0380_CFG_TRACE_COUNT; i++)
		trace[i] = mz_read(dev, MZ0380_MAP_BAR_CFG,
				   mz0380_cfg_trace_reg(i));
}

bool mz0380_candidate_config_valid(struct mz0380_dev *dev, u32 reg,
					  u32 mask, u32 shift)
{
	u64 field_mask;

	if (reg == ~0U || (reg & 0x3))
		return false;

	if ((resource_size_t)reg + sizeof(u32) >
	    dev->bar_len[MZ0380_MAP_BAR_CFG])
		return false;

	if (!mask || shift >= 32)
		return false;

	field_mask = (u64)mask << shift;
	if (field_mask > U32_MAX)
		return false;

	return true;
}

bool mz0380_input_select_config_valid(struct mz0380_dev *dev)
{
	return mz0380_candidate_config_valid(dev, input_select_reg,
					     input_select_mask,
					     input_select_shift);
}

u32 mz0380_extract_field(u32 word, u32 mask, u32 shift)
{
	return (word >> shift) & mask;
}

static bool mz0380_has_hw_cfg_field(struct mz0380_dev *dev, u32 reg,
				    u32 mask, u32 shift)
{
	return dev->board == MZ0380_BOARD_ELGATO_HD60_PRO &&
	       mz0380_candidate_config_valid(dev, reg, mask, shift);
}

static bool mz0380_read_hw_cfg_field(struct mz0380_dev *dev, u32 reg,
				     u32 mask, u32 shift, u32 *value,
				     u32 *raw_word, u32 *out_reg,
				     u32 *out_mask, u32 *out_shift)
{
	u32 word;

	if (!mz0380_has_hw_cfg_field(dev, reg, mask, shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, reg);
	if (raw_word)
		*raw_word = word;
	if (value)
		*value = mz0380_extract_field(word, mask, shift);
	if (out_reg)
		*out_reg = reg;
	if (out_mask)
		*out_mask = mask;
	if (out_shift)
		*out_shift = shift;

	return true;
}

bool mz0380_source_uses_candidate_override(const char *source)
{
	return source && !strcmp(source, "procfs");
}

const char *
mz0380_property_experiment_result_name(enum mz0380_property_experiment_result result)
{
	switch (result) {
	case MZ0380_PROPERTY_EXPERIMENT_NONE:
		return "none";
	case MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_DISABLED:
		return "intent-only (writes disabled)";
	case MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_UNCONFIGURED:
		return "intent-only (candidate register not configured)";
	case MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_INVALID_CONFIG:
		return "intent-only (candidate register config invalid)";
	case MZ0380_PROPERTY_EXPERIMENT_CACHED_ONLY:
		return "cached-only (no hardware mapping confirmed)";
	case MZ0380_PROPERTY_EXPERIMENT_WRITE_APPLIED:
		return "hardware write applied";
	case MZ0380_PROPERTY_EXPERIMENT_WRITE_READBACK_MISMATCH:
		return "hardware write readback mismatch";
	}

	return "unknown";
}

void mz0380_input_candidate_clear(void)
{
	input_select_reg = ~0U;
	input_select_mask = 0x7;
	input_select_shift = 0;
}

void mz0380_input_candidate_set(u32 reg, u32 mask, u32 shift)
{
	input_select_reg = reg;
	input_select_mask = mask;
	input_select_shift = shift;
}

void mz0380_bitrate_candidate_clear(void)
{
	bitrate_reg = ~0U;
	bitrate_mask = ~0U;
	bitrate_shift = 0;
}

void mz0380_bitrate_candidate_set(u32 reg, u32 mask, u32 shift)
{
	bitrate_reg = reg;
	bitrate_mask = mask;
	bitrate_shift = shift;
}

void mz0380_quality_candidate_clear(void)
{
	quality_reg = ~0U;
	quality_mask = ~0U;
	quality_shift = 0;
}

void mz0380_quality_candidate_set(u32 reg, u32 mask, u32 shift)
{
	quality_reg = reg;
	quality_mask = mask;
	quality_shift = shift;
}

void mz0380_gop_candidate_clear(void)
{
	gop_reg = ~0U;
	gop_mask = ~0U;
	gop_shift = 0;
}

void mz0380_gop_candidate_set(u32 reg, u32 mask, u32 shift)
{
	gop_reg = reg;
	gop_mask = mask;
	gop_shift = shift;
}

void mz0380_b_frames_candidate_clear(void)
{
	b_frames_reg = ~0U;
	b_frames_mask = ~0U;
	b_frames_shift = 0;
}

void mz0380_b_frames_candidate_set(u32 reg, u32 mask, u32 shift)
{
	b_frames_reg = reg;
	b_frames_mask = mask;
	b_frames_shift = shift;
}

void mz0380_qp_step_candidate_clear(void)
{
	qp_step_reg = ~0U;
	qp_step_mask = ~0U;
	qp_step_shift = 0;
}

void mz0380_qp_step_candidate_set(u32 reg, u32 mask, u32 shift)
{
	qp_step_reg = reg;
	qp_step_mask = mask;
	qp_step_shift = shift;
}

void mz0380_record_mode_candidate_clear(void)
{
	record_mode_reg = ~0U;
	record_mode_mask = 0x3;
	record_mode_shift = 0;
}

void mz0380_record_mode_candidate_set(u32 reg, u32 mask, u32 shift)
{
	record_mode_reg = reg;
	record_mode_mask = mask;
	record_mode_shift = shift;
}

int
mz0380_request_scalar_property_locked(struct mz0380_dev *dev,
				      struct mz0380_property_experiment *exp,
				      u32 property_id, u32 *cached_value,
				      u32 requested_value, u32 value_count,
				      u32 reg, u32 mask, u32 shift,
				      const char *source,
				      bool require_write_enable)
{
	u32 field_mask;
	int ret = 0;

	if (value_count && requested_value >= value_count)
		return -EINVAL;

	memset(exp, 0, sizeof(*exp));
	exp->valid = true;
	exp->property_id = property_id;
	exp->previous_value = *cached_value;
	exp->requested_value = requested_value;
	exp->reg = reg;
	exp->mask = mask;
	exp->shift = shift;
	strscpy(exp->source, source ?: "unknown", sizeof(exp->source));

	mz0380_capture_cfg_trace(dev, exp->before_cfg_trace);
	*cached_value = requested_value;

	if (require_write_enable && !allow_experimental_writes) {
		exp->result = MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_DISABLED;
		goto out_trace;
	}

	if (reg == ~0U) {
		exp->result = MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_UNCONFIGURED;
		ret = require_write_enable ? 0 : -EINVAL;
		goto out_trace;
	}

	if (!mz0380_candidate_config_valid(dev, reg, mask, shift)) {
		exp->result = MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_INVALID_CONFIG;
		ret = require_write_enable ? 0 : -EINVAL;
		goto out_trace;
	}

	field_mask = mask << shift;
	exp->before_word = mz_read(dev, MZ0380_MAP_BAR_CFG, reg);
	exp->programmed_word =
		(exp->before_word & ~field_mask) |
		((requested_value & mask) << shift);
	mz_write(dev, MZ0380_MAP_BAR_CFG, reg, exp->programmed_word);
	exp->readback_word = mz_read(dev, MZ0380_MAP_BAR_CFG, reg);
	exp->hardware_write = true;
	if (exp->readback_word != exp->programmed_word)
		exp->result =
			MZ0380_PROPERTY_EXPERIMENT_WRITE_READBACK_MISMATCH;
	else
		exp->result = MZ0380_PROPERTY_EXPERIMENT_WRITE_APPLIED;
	ret = exp->result ==
	      MZ0380_PROPERTY_EXPERIMENT_WRITE_READBACK_MISMATCH ?
		-EIO : 0;

out_trace:
	mz0380_capture_cfg_trace(dev, exp->after_cfg_trace);
	return ret;
}

bool mz0380_read_hw_input_select(struct mz0380_dev *dev, u32 *input, u32 *raw_word)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_INPUT_SELECT_HW_REG,
					MZ0380_INPUT_SELECT_HW_MASK,
					MZ0380_INPUT_SELECT_HW_SHIFT,
					input, raw_word, NULL, NULL, NULL);
}

bool mz0380_read_hw_bitrate(struct mz0380_dev *dev, u32 *bitrate,
			    u32 *raw_word, u32 *reg,
			    u32 *mask, u32 *shift)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_BITRATE_HW_REG,
					MZ0380_BITRATE_HW_MASK,
					MZ0380_BITRATE_HW_SHIFT,
					bitrate, raw_word, reg, mask, shift);
}

bool mz0380_read_hw_quality(struct mz0380_dev *dev, u32 *quality,
			    u32 *raw_word, u32 *reg,
			    u32 *mask, u32 *shift)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_QUALITY_HW_REG,
					MZ0380_QUALITY_HW_MASK,
					MZ0380_QUALITY_HW_SHIFT,
					quality, raw_word, reg, mask, shift);
}

bool mz0380_read_hw_record_mode(struct mz0380_dev *dev, u32 *mode,
				u32 *raw_word, u32 *reg,
				u32 *mask, u32 *shift)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_RECORD_MODE_HW_REG,
					MZ0380_RECORD_MODE_HW_MASK,
					MZ0380_RECORD_MODE_HW_SHIFT,
					mode, raw_word, reg, mask, shift);
}

bool mz0380_read_hw_gop(struct mz0380_dev *dev, u32 *gop,
			u32 *raw_word, u32 *reg,
			u32 *mask, u32 *shift)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_GOP_HW_REG,
					MZ0380_GOP_HW_MASK,
					MZ0380_GOP_HW_SHIFT,
					gop, raw_word, reg, mask, shift);
}

bool mz0380_read_hw_b_frames(struct mz0380_dev *dev, u32 *b_frames,
			     u32 *raw_word, u32 *reg,
			     u32 *mask, u32 *shift)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_B_FRAMES_HW_REG,
					MZ0380_B_FRAMES_HW_MASK,
					MZ0380_B_FRAMES_HW_SHIFT,
					b_frames, raw_word, reg, mask, shift);
}

bool mz0380_read_hw_qp_step(struct mz0380_dev *dev, u32 *qp_step,
			    u32 *raw_word, u32 *reg,
			    u32 *mask, u32 *shift)
{
	return mz0380_read_hw_cfg_field(dev, MZ0380_QP_STEP_HW_REG,
					MZ0380_QP_STEP_HW_MASK,
					MZ0380_QP_STEP_HW_SHIFT,
					qp_step, raw_word, reg, mask, shift);
}

bool mz0380_read_candidate_bitrate(struct mz0380_dev *dev, u32 *bitrate,
				   u32 *raw_word, u32 *reg,
				   u32 *mask, u32 *shift)
{
	u32 word;

	if (!mz0380_candidate_config_valid(dev, bitrate_reg,
					   bitrate_mask,
					   bitrate_shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, bitrate_reg);
	if (raw_word)
		*raw_word = word;
	if (bitrate)
		*bitrate = mz0380_extract_field(word, bitrate_mask,
						bitrate_shift);
	if (reg)
		*reg = bitrate_reg;
	if (mask)
		*mask = bitrate_mask;
	if (shift)
		*shift = bitrate_shift;

	return true;
}

bool mz0380_read_candidate_quality(struct mz0380_dev *dev, u32 *quality,
				   u32 *raw_word, u32 *reg,
				   u32 *mask, u32 *shift)
{
	u32 word;

	if (!mz0380_candidate_config_valid(dev, quality_reg,
					   quality_mask,
					   quality_shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, quality_reg);
	if (raw_word)
		*raw_word = word;
	if (quality)
		*quality = mz0380_extract_field(word, quality_mask,
						quality_shift);
	if (reg)
		*reg = quality_reg;
	if (mask)
		*mask = quality_mask;
	if (shift)
		*shift = quality_shift;

	return true;
}

bool mz0380_read_candidate_gop(struct mz0380_dev *dev, u32 *gop,
			       u32 *raw_word, u32 *reg,
			       u32 *mask, u32 *shift)
{
	u32 word;

	if (!mz0380_candidate_config_valid(dev, gop_reg,
					   gop_mask,
					   gop_shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, gop_reg);
	if (raw_word)
		*raw_word = word;
	if (gop)
		*gop = mz0380_extract_field(word, gop_mask,
					    gop_shift);
	if (reg)
		*reg = gop_reg;
	if (mask)
		*mask = gop_mask;
	if (shift)
		*shift = gop_shift;

	return true;
}

bool mz0380_read_candidate_b_frames(struct mz0380_dev *dev, u32 *b_frames,
				    u32 *raw_word, u32 *reg,
				    u32 *mask, u32 *shift)
{
	u32 word;

	if (!mz0380_candidate_config_valid(dev, b_frames_reg,
					   b_frames_mask,
					   b_frames_shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, b_frames_reg);
	if (raw_word)
		*raw_word = word;
	if (b_frames)
		*b_frames = mz0380_extract_field(word, b_frames_mask,
						 b_frames_shift);
	if (reg)
		*reg = b_frames_reg;
	if (mask)
		*mask = b_frames_mask;
	if (shift)
		*shift = b_frames_shift;

	return true;
}

bool mz0380_read_candidate_qp_step(struct mz0380_dev *dev, u32 *qp_step,
				   u32 *raw_word, u32 *reg,
				   u32 *mask, u32 *shift)
{
	u32 word;

	if (!mz0380_candidate_config_valid(dev, qp_step_reg,
					   qp_step_mask,
					   qp_step_shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, qp_step_reg);
	if (raw_word)
		*raw_word = word;
	if (qp_step)
		*qp_step = mz0380_extract_field(word, qp_step_mask,
						qp_step_shift);
	if (reg)
		*reg = qp_step_reg;
	if (mask)
		*mask = qp_step_mask;
	if (shift)
		*shift = qp_step_shift;

	return true;
}

/*
 * M171: a readback outside the control's declared range is NOT a value.
 *
 * v4l2-compliance failed VIDIOC_G/S_CTRL and G/S/TRY_EXT_CTRLS with
 * "returned control value out of range" on V4L2_CID_MPEG_VIDEO_GOP_SIZE, which
 * is declared min=1 and was reporting 0. The 0 came from here: these sync
 * helpers read a BAR5 field and adopt whatever it holds. Those registers are
 * only ever written by the H.264 path, this driver's capture path is raw I420,
 * so they have never been written and read back as zero. Zero means UNSET, not
 * "a GOP of zero" - and bitrate 0 (declared min 262144) is the same story.
 *
 * The bitrate sync already noticed: it printed "outside current V4L2 range" and
 * then stored the value anyway. mz0380_sync_candidate_b_frames() and
 * mz0380_sync_candidate_record_mode() got it right from the start - they reject
 * and return false. This makes the other four agree with those two.
 *
 * Rejecting leaves the cached value in place, which is the declared default
 * until userspace sets something, so the control always reports a legal value.
 */
static bool mz0380_sync_in_range(struct mz0380_dev *dev, const char *what,
				 u32 val, u32 min, u32 max, u32 raw_word,
				 const char *reason)
{
	if (val >= min && val <= max)
		return true;

	pr_warn("%s: %s field returned %u (raw=%08x), outside the V4L2 range [%u..%u] - keeping the cached value; the register is unwritten, not zero%s%s\n",
		dev->name, what, val, raw_word, min, max,
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");
	return false;
}

bool mz0380_sync_candidate_bitrate(struct mz0380_dev *dev,
				   const char *reason)
{
	u32 bitrate;
	u32 raw_word;

	if (!mz0380_read_candidate_bitrate(dev, &bitrate, &raw_word, NULL,
					   NULL, NULL))
		return false;

	if (!mz0380_sync_in_range(dev, "candidate bitrate", bitrate, MZ0380_MIN_BITRATE, MZ0380_MAX_BITRATE,
				  raw_word, reason))
		return false;

	dev->capture.bitrate = bitrate;
	pr_info("%s: synced cached bitrate from candidate BAR5 field = %u (raw=%08x)%s%s\n",
		dev->name, bitrate, raw_word,
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");

	if (bitrate < MZ0380_MIN_BITRATE || bitrate > MZ0380_MAX_BITRATE)
		pr_warn("%s: candidate bitrate field is outside current V4L2 range [%u..%u]\n",
			dev->name, MZ0380_MIN_BITRATE, MZ0380_MAX_BITRATE);

	return true;
}

bool mz0380_sync_hw_bitrate(struct mz0380_dev *dev, const char *reason)
{
	u32 bitrate;
	u32 raw_word;

	if (!mz0380_read_hw_bitrate(dev, &bitrate, &raw_word, NULL,
				    NULL, NULL))
		return false;

	if (!mz0380_sync_in_range(dev, "BAR5 bitrate", bitrate, MZ0380_MIN_BITRATE, MZ0380_MAX_BITRATE,
				  raw_word, reason))
		return false;

	dev->capture.bitrate = bitrate;
	pr_info("%s: synced cached bitrate from BAR5[0x%04x] = %u (raw=%08x)%s%s\n",
		dev->name, MZ0380_BITRATE_HW_REG, bitrate, raw_word,
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");

	if (bitrate < MZ0380_MIN_BITRATE || bitrate > MZ0380_MAX_BITRATE)
		pr_warn("%s: BAR5 bitrate field is outside current V4L2 range [%u..%u]\n",
			dev->name, MZ0380_MIN_BITRATE, MZ0380_MAX_BITRATE);

	return true;
}

bool mz0380_sync_candidate_quality(struct mz0380_dev *dev,
				   const char *reason)
{
	u32 quality;
	u32 raw_word;

	if (!mz0380_read_candidate_quality(dev, &quality, &raw_word, NULL,
					   NULL, NULL))
		return false;

	if (!mz0380_sync_in_range(dev, "candidate quality", quality, MZ0380_MIN_QUALITY, MZ0380_MAX_QUALITY,
				  raw_word, reason))
		return false;

	dev->capture.quality = quality;
	pr_info("%s: synced cached quality from candidate BAR5 field = %u (raw=%08x)%s%s\n",
		dev->name, quality, raw_word,
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_hw_quality(struct mz0380_dev *dev, const char *reason)
{
	u32 quality;
	u32 raw_word;

	if (!mz0380_read_hw_quality(dev, &quality, &raw_word, NULL,
				    NULL, NULL))
		return false;

	if (!mz0380_sync_in_range(dev, "BAR5 quality", quality, MZ0380_MIN_QUALITY, MZ0380_MAX_QUALITY,
				  raw_word, reason))
		return false;

	dev->capture.quality = quality;
	pr_info("%s: synced cached quality from BAR5[0x%04x] = %u (raw=%08x)%s%s\n",
		dev->name, MZ0380_QUALITY_HW_REG, quality, raw_word,
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");

	if (quality < MZ0380_MIN_QUALITY || quality > MZ0380_MAX_QUALITY)
		pr_warn("%s: BAR5 quality field is outside current V4L2 range [%u..%u]\n",
			dev->name, MZ0380_MIN_QUALITY, MZ0380_MAX_QUALITY);

	return true;
}

bool mz0380_sync_candidate_gop(struct mz0380_dev *dev,
			       const char *reason)
{
	u32 gop;
	u32 raw_word;

	if (!mz0380_read_candidate_gop(dev, &gop, &raw_word, NULL,
				       NULL, NULL))
		return false;

	if (!mz0380_sync_in_range(dev, "candidate GOP", gop, MZ0380_MIN_GOP, MZ0380_MAX_GOP,
				  raw_word, reason))
		return false;

	dev->capture.gop_size = gop;
	pr_info("%s: synced cached GOP from candidate BAR5 field = %u (raw=%08x)%s%s\n",
		dev->name, gop, raw_word,
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_candidate_b_frames(struct mz0380_dev *dev,
				    const char *reason)
{
	u32 b_frames;
	u32 raw_word;

	if (!mz0380_read_candidate_b_frames(dev, &b_frames, &raw_word, NULL,
					    NULL, NULL))
		return false;

	if (b_frames > MZ0380_MAX_B_FRAMES) {
		pr_warn("%s: candidate B-frame field returned out-of-range value %u (raw=%08x)%s%s\n",
			dev->name, b_frames, raw_word,
			reason && *reason ? " during " : "",
			reason && *reason ? reason : "");
		return false;
	}

	dev->capture.b_frames = b_frames;
	pr_info("%s: synced cached B-frames from candidate BAR5 field = %u (raw=%08x)%s%s\n",
		dev->name, b_frames, raw_word,
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_hw_b_frames(struct mz0380_dev *dev,
			     const char *reason)
{
	u32 b_frames;
	u32 raw_word;

	if (!mz0380_read_hw_b_frames(dev, &b_frames, &raw_word, NULL,
				     NULL, NULL))
		return false;

	if (b_frames > MZ0380_MAX_B_FRAMES) {
		pr_warn("%s: BAR5 B-frame field returned out-of-range value %u (raw=%08x)%s%s\n",
			dev->name, b_frames, raw_word,
			reason && *reason ? " during " : "",
			reason && *reason ? reason : "");
		return false;
	}

	dev->capture.b_frames = b_frames;
	pr_info("%s: synced cached B-frames from BAR5[0x%04x] = %u (raw=%08x)%s%s\n",
		dev->name, MZ0380_B_FRAMES_HW_REG, b_frames, raw_word,
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_candidate_qp_step(struct mz0380_dev *dev,
				   const char *reason)
{
	u32 qp_step;
	u32 raw_word;

	if (!mz0380_read_candidate_qp_step(dev, &qp_step, &raw_word, NULL,
					   NULL, NULL))
		return false;

	dev->capture.qp_step = qp_step;
	pr_info("%s: synced cached QP step from candidate BAR5 field = %u (raw=%08x)%s%s\n",
		dev->name, qp_step, raw_word,
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_hw_qp_step(struct mz0380_dev *dev, const char *reason)
{
	u32 qp_step;
	u32 raw_word;

	if (!mz0380_read_hw_qp_step(dev, &qp_step, &raw_word, NULL,
				    NULL, NULL))
		return false;

	dev->capture.qp_step = qp_step;
	pr_info("%s: synced cached QP step from BAR5[0x%04x] = %u (raw=%08x)%s%s\n",
		dev->name, MZ0380_QP_STEP_HW_REG, qp_step, raw_word,
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_hw_gop(struct mz0380_dev *dev, const char *reason)
{
	u32 gop;
	u32 raw_word;

	if (!mz0380_read_hw_gop(dev, &gop, &raw_word, NULL,
				NULL, NULL))
		return false;

	if (!mz0380_sync_in_range(dev, "BAR5 GOP", gop, MZ0380_MIN_GOP, MZ0380_MAX_GOP,
				  raw_word, reason))
		return false;

	dev->capture.gop_size = gop;
	pr_info("%s: synced cached GOP from BAR5[0x%04x] = %u (raw=%08x)%s%s\n",
		dev->name, MZ0380_GOP_HW_REG, gop, raw_word,
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");

	return true;
}

bool mz0380_read_candidate_record_mode(struct mz0380_dev *dev, u32 *mode,
				       u32 *raw_word, u32 *reg,
				       u32 *mask, u32 *shift)
{
	u32 word;

	if (!mz0380_candidate_config_valid(dev, record_mode_reg,
					   record_mode_mask,
					   record_mode_shift))
		return false;

	word = mz_read(dev, MZ0380_MAP_BAR_CFG, record_mode_reg);
	if (raw_word)
		*raw_word = word;
	if (mode)
		*mode = mz0380_extract_field(word, record_mode_mask,
					       record_mode_shift);
	if (reg)
		*reg = record_mode_reg;
	if (mask)
		*mask = record_mode_mask;
	if (shift)
		*shift = record_mode_shift;

	return true;
}

bool mz0380_sync_candidate_record_mode(struct mz0380_dev *dev,
				       const char *reason)
{
	u32 mode;
	u32 raw_word;

	if (!mz0380_read_candidate_record_mode(dev, &mode, &raw_word, NULL,
					       NULL, NULL))
		return false;

	if (mode >= MZ0380_RECORD_MODE_COUNT) {
		pr_warn("%s: candidate record-mode field returned out-of-range value %u (raw=%08x)%s%s\n",
			dev->name, mode, raw_word,
			reason && *reason ? " during " : "",
			reason && *reason ? reason : "");
		return false;
	}

	dev->capture.record_mode = mode;
	pr_info("%s: synced cached record mode from candidate BAR5 field = %u (%s)%s%s\n",
		dev->name, mode, mz0380_record_mode_name(mode),
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_hw_record_mode(struct mz0380_dev *dev,
				const char *reason)
{
	u32 mode;
	u32 raw_word;

	if (!mz0380_read_hw_record_mode(dev, &mode, &raw_word, NULL,
					NULL, NULL))
		return false;

	if (mode >= MZ0380_RECORD_MODE_COUNT) {
		pr_warn("%s: BAR5 record-mode field returned out-of-range value %u (raw=%08x)%s%s\n",
			dev->name, mode, raw_word,
			reason && *reason ? " during " : "",
			reason && *reason ? reason : "");
		return false;
	}

	dev->capture.record_mode = mode;
	pr_info("%s: synced cached record mode from BAR5[0x%04x] = %u (%s)%s%s\n",
		dev->name, MZ0380_RECORD_MODE_HW_REG, mode,
		mz0380_record_mode_name(mode),
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");

	return true;
}

bool mz0380_sync_hw_input_select(struct mz0380_dev *dev, const char *reason)
{
	u32 input;
	u32 raw_word;

	if (!mz0380_read_hw_input_select(dev, &input, &raw_word))
		return false;

	if (input >= MZ0380_INPUT_COUNT) {
		pr_warn("%s: BAR5[0x%04x][2:0] returned out-of-range input %u (raw=%08x)%s%s\n",
			dev->name, MZ0380_INPUT_SELECT_HW_REG, input, raw_word,
			reason && *reason ? " during " : "",
			reason && *reason ? reason : "");
		return false;
	}

	dev->capture.input = input;
	pr_info("%s: synced cached input from BAR5[0x%04x][2:0] = %u (%s)%s%s\n",
		dev->name, MZ0380_INPUT_SELECT_HW_REG, input,
		mz0380_input_name(input),
		reason && *reason ? " during " : "",
		reason && *reason ? reason : "");

	return true;
}
