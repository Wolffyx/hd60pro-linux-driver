// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 validated control write requests.
 */

#include "mz0380-internal.h"

int mz0380_request_input_select(struct mz0380_dev *dev, u32 input,
			       const char *source)
{
	struct mz0380_property_experiment *exp = &dev->input_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_input_select_config_valid(dev);
	int ret;

	if (input >= MZ0380_INPUT_COUNT)
		return -EINVAL;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_INPUT_SELECT_PROPERTY,
						    &dev->capture.input, input,
						    MZ0380_INPUT_COUNT,
						    use_candidate ?
						    input_select_reg :
						    MZ0380_INPUT_SELECT_HW_REG,
						    use_candidate ?
						    input_select_mask :
						    MZ0380_INPUT_SELECT_HW_MASK,
						    use_candidate ?
						    input_select_shift :
						    MZ0380_INPUT_SELECT_HW_SHIFT,
						    source, use_candidate);
	mutex_unlock(&dev->ctrl_lock);

	pr_info("%s: property %u input-select request %u (%s) via %s -> %s\n",
		dev->name, exp->property_id, exp->requested_value,
		mz0380_input_name(exp->requested_value), exp->source,
		mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		pr_info("%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
			dev->name, exp->property_id, exp->reg, exp->mask,
			exp->shift, exp->before_word, exp->programmed_word,
			exp->readback_word);

	return ret;
}
int mz0380_request_bitrate_locked(struct mz0380_dev *dev, u32 bitrate,
				  const char *source)
{
	struct mz0380_property_experiment *exp = &dev->bitrate_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_candidate_config_valid(dev, bitrate_reg,
					      bitrate_mask,
					      bitrate_shift);
	int ret;

	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_BITRATE_PROPERTY,
						    &dev->capture.bitrate,
						    bitrate, 0,
						    use_candidate ?
						    bitrate_reg :
						    MZ0380_BITRATE_HW_REG,
						    use_candidate ?
						    bitrate_mask :
						    MZ0380_BITRATE_HW_MASK,
						    use_candidate ?
						    bitrate_shift :
						    MZ0380_BITRATE_HW_SHIFT,
						    source, use_candidate);

	pr_info("%s: property %u bitrate request %u via %s -> %s\n",
		dev->name, exp->property_id, exp->requested_value,
		exp->source,
		mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		pr_info("%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
			dev->name, exp->property_id, exp->reg, exp->mask,
			exp->shift, exp->before_word, exp->programmed_word,
			exp->readback_word);

	return ret;
}

int mz0380_request_bitrate(struct mz0380_dev *dev, u32 bitrate,
			   const char *source)
{
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_bitrate_locked(dev, bitrate, source);
	mutex_unlock(&dev->ctrl_lock);

	return ret;
}

int mz0380_request_quality_locked(struct mz0380_dev *dev, u32 quality,
				  const char *source)
{
	struct mz0380_property_experiment *exp = &dev->quality_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_candidate_config_valid(dev, quality_reg,
					      quality_mask,
					      quality_shift);
	int ret;

	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_QUALITY_PROPERTY,
						    &dev->capture.quality,
						    quality, 0,
						    use_candidate ?
						    quality_reg :
						    MZ0380_QUALITY_HW_REG,
						    use_candidate ?
						    quality_mask :
						    MZ0380_QUALITY_HW_MASK,
						    use_candidate ?
						    quality_shift :
						    MZ0380_QUALITY_HW_SHIFT,
						    source, use_candidate);

	pr_info("%s: property %u quality request %u via %s -> %s\n",
		dev->name, exp->property_id, exp->requested_value,
		exp->source,
		mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		pr_info("%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
			dev->name, exp->property_id, exp->reg, exp->mask,
			exp->shift, exp->before_word, exp->programmed_word,
			exp->readback_word);

	return ret;
}

int mz0380_request_quality(struct mz0380_dev *dev, u32 quality,
			   const char *source)
{
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_quality_locked(dev, quality, source);
	mutex_unlock(&dev->ctrl_lock);

	return ret;
}

int mz0380_request_gop_locked(struct mz0380_dev *dev, u32 gop,
			      const char *source)
{
	struct mz0380_property_experiment *exp = &dev->gop_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_candidate_config_valid(dev, gop_reg,
					      gop_mask,
					      gop_shift);
	int ret;

	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_GOP_PROPERTY,
						    &dev->capture.gop_size,
						    gop, 0,
						    use_candidate ?
						    gop_reg :
						    MZ0380_GOP_HW_REG,
						    use_candidate ?
						    gop_mask :
						    MZ0380_GOP_HW_MASK,
						    use_candidate ?
						    gop_shift :
						    MZ0380_GOP_HW_SHIFT,
						    source, use_candidate);

	pr_info("%s: property %u GOP request %u via %s -> %s\n",
		dev->name, exp->property_id, exp->requested_value,
		exp->source,
		mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		pr_info("%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
			dev->name, exp->property_id, exp->reg, exp->mask,
			exp->shift, exp->before_word, exp->programmed_word,
			exp->readback_word);

	return ret;
}

int mz0380_request_gop(struct mz0380_dev *dev, u32 gop,
		       const char *source)
{
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_gop_locked(dev, gop, source);
	mutex_unlock(&dev->ctrl_lock);

	return ret;
}

int mz0380_request_b_frames_locked(struct mz0380_dev *dev, u32 b_frames,
				   const char *source)
{
	struct mz0380_property_experiment *exp = &dev->b_frames_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_candidate_config_valid(dev, b_frames_reg,
					      b_frames_mask,
					      b_frames_shift);
	int ret;

	if (b_frames > MZ0380_MAX_B_FRAMES)
		return -EINVAL;

	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_B_FRAMES_PROPERTY,
						    &dev->capture.b_frames,
						    b_frames,
						    MZ0380_MAX_B_FRAMES + 1,
						    use_candidate ?
						    b_frames_reg :
						    MZ0380_B_FRAMES_HW_REG,
						    use_candidate ?
						    b_frames_mask :
						    MZ0380_B_FRAMES_HW_MASK,
						    use_candidate ?
						    b_frames_shift :
						    MZ0380_B_FRAMES_HW_SHIFT,
						    source, use_candidate);

	pr_info("%s: property %u B-frames request %u via %s -> %s\n",
		dev->name, exp->property_id, exp->requested_value,
		exp->source,
		mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		pr_info("%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
			dev->name, exp->property_id, exp->reg, exp->mask,
			exp->shift, exp->before_word, exp->programmed_word,
			exp->readback_word);

	return ret;
}

int mz0380_request_b_frames(struct mz0380_dev *dev, u32 b_frames,
			    const char *source)
{
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_b_frames_locked(dev, b_frames, source);
	mutex_unlock(&dev->ctrl_lock);

	return ret;
}

int mz0380_request_qp_step_locked(struct mz0380_dev *dev, u32 qp_step,
				  const char *source)
{
	struct mz0380_property_experiment *exp = &dev->qp_step_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_candidate_config_valid(dev, qp_step_reg,
					      qp_step_mask,
					      qp_step_shift);
	int ret;

	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_QP_STEP_PROPERTY,
						    &dev->capture.qp_step,
						    qp_step, 0,
						    use_candidate ?
						    qp_step_reg :
						    MZ0380_QP_STEP_HW_REG,
						    use_candidate ?
						    qp_step_mask :
						    MZ0380_QP_STEP_HW_MASK,
						    use_candidate ?
						    qp_step_shift :
						    MZ0380_QP_STEP_HW_SHIFT,
						    source, use_candidate);

	pr_info("%s: property %u QP-step request %u via %s -> %s\n",
		dev->name, exp->property_id, exp->requested_value,
		exp->source,
		mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		pr_info("%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
			dev->name, exp->property_id, exp->reg, exp->mask,
			exp->shift, exp->before_word, exp->programmed_word,
			exp->readback_word);

	return ret;
}

int mz0380_request_qp_step(struct mz0380_dev *dev, u32 qp_step,
			   const char *source)
{
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_qp_step_locked(dev, qp_step, source);
	mutex_unlock(&dev->ctrl_lock);

	return ret;
}

int mz0380_request_record_mode_locked(struct mz0380_dev *dev, u32 mode,
				      const char *source)
{
	struct mz0380_property_experiment *exp = &dev->record_mode_experiment;
	bool use_candidate = mz0380_source_uses_candidate_override(source) &&
		mz0380_candidate_config_valid(dev, record_mode_reg,
					      record_mode_mask,
					      record_mode_shift);
	int ret;

	if (mode >= MZ0380_RECORD_MODE_COUNT)
		return -EINVAL;

	ret = mz0380_request_scalar_property_locked(dev, exp,
						    MZ0380_RECORD_MODE_PROPERTY,
						    &dev->capture.record_mode,
						    mode,
						    MZ0380_RECORD_MODE_COUNT,
						    use_candidate ?
						    record_mode_reg :
						    MZ0380_RECORD_MODE_HW_REG,
						    use_candidate ?
						    record_mode_mask :
						    MZ0380_RECORD_MODE_HW_MASK,
						    use_candidate ?
						    record_mode_shift :
						    MZ0380_RECORD_MODE_HW_SHIFT,
						    source, use_candidate);

	pr_info("%s: property %u record-mode request %u (%s) via %s -> %s\n",
		dev->name, exp->property_id, exp->requested_value,
		mz0380_record_mode_name(exp->requested_value), exp->source,
		mz0380_property_experiment_result_name(exp->result));
	if (exp->hardware_write)
		pr_info("%s: property %u BAR5 write reg=0x%04x mask=0x%08x shift=%u before=%08x programmed=%08x readback=%08x\n",
			dev->name, exp->property_id, exp->reg, exp->mask,
			exp->shift, exp->before_word, exp->programmed_word,
			exp->readback_word);

	return ret;
}

int mz0380_request_record_mode(struct mz0380_dev *dev, u32 mode,
			       const char *source)
{
	int ret;

	mutex_lock(&dev->ctrl_lock);
	ret = mz0380_request_record_mode_locked(dev, mode, source);
	mutex_unlock(&dev->ctrl_lock);

	return ret;
}
