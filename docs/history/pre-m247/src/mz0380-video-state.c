// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 human-readable V4L2 state diagnostics.
 */

#include "mz0380-internal.h"

void mz0380_video_state_dump(struct seq_file *m, struct mz0380_dev *dev)
{
	unsigned int fps_milli;
	unsigned int encoded_fps_milli = 0;
	u32 encoded_source_fps;
	u32 frame_divisor;
	u32 hw_input;
	u32 hw_word;
	u32 hw_mode;
	u32 hw_mode_word;
	u32 hw_mode_reg;
	u32 hw_mode_mask;
	u32 hw_mode_shift;
	u32 hw_quality;
	u32 hw_quality_word;
	u32 hw_quality_reg;
	u32 hw_quality_mask;
	u32 hw_quality_shift;
	u32 hw_gop;
	u32 hw_gop_word;
	u32 hw_gop_reg;
	u32 hw_gop_mask;
	u32 hw_gop_shift;
	u32 candidate_qp_step;
	u32 candidate_qp_step_word;
	u32 candidate_qp_step_reg;
	u32 candidate_qp_step_mask;
	u32 candidate_qp_step_shift;
	u32 hw_qp_step;
	u32 hw_qp_step_word;
	u32 hw_qp_step_reg;
	u32 hw_qp_step_mask;
	u32 hw_qp_step_shift;
	u32 candidate_b_frames;
	u32 candidate_b_frames_word;
	u32 candidate_b_frames_reg;
	u32 candidate_b_frames_mask;
	u32 candidate_b_frames_shift;
	u32 hw_b_frames;
	u32 hw_b_frames_word;
	u32 hw_b_frames_reg;
	u32 hw_b_frames_mask;
	u32 hw_b_frames_shift;
	u32 hw_bitrate;
	u32 hw_bitrate_word;
	u32 hw_bitrate_reg;
	u32 hw_bitrate_mask;
	u32 hw_bitrate_shift;
	u32 fourcc = mz0380_current_pixelformat(dev);

	fps_milli = DIV_ROUND_CLOSEST(dev->capture.timeperframe.denominator * 1000,
				      max_t(u32, dev->capture.timeperframe.numerator, 1));
	encoded_source_fps = READ_ONCE(dev->pipeline_source_fps) ?:
			     READ_ONCE(dev->capture.source_fps);
	frame_divisor = mz0380_h264_frame_divisor;
	if (frame_divisor == 1 || frame_divisor > U8_MAX)
		frame_divisor = 2;
	if (mz0380_h264_probe && encoded_source_fps)
		encoded_fps_milli = DIV_ROUND_CLOSEST(encoded_source_fps * 1000,
						       frame_divisor ?: 1);

	if (!mz0380_enable_video) {
		seq_puts(m,
			 "  capture    : cached encoded-first control model (V4L2 node disabled; load with enable_video=1 to register /dev/video*)\n");
	} else {
		seq_puts(m, "  capture    : encoded-first V4L2 scaffolding\n");
	}

	if (dev->video_registered)
		seq_printf(m, "  video node : /dev/%s\n",
			   video_device_node_name(&dev->vdev));
	seq_printf(m, "  pixelformat: %4.4s\n", (char *)&fourcc);
	/*
	 * M169: the spawn budget, which is the number that decides whether the
	 * next hardware run is safe. Per insmod here - the driver cannot see
	 * across a module reload - so ./mz0380-spawns.sh accumulates it per
	 * boot, which is the granularity the card's wedge actually has.
	 */
	seq_printf(m, "  enc spawns : %u since insmod (card wedges somewhere in 8-18 per POWER CYCLE)\n",
		   dev->encoder_spawns);
	if (mz0380_h264_probe) {
		seq_printf(m, "  pipeline   : %s, VB2 %s, %u attachment(s), %u cached SPS/PPS bytes%s%s\n",
			   READ_ONCE(dev->pipeline_running) ? "running" : "stopped",
			   READ_ONCE(dev->streaming) ? "attached" : "detached",
			   dev->pipeline_attach_count,
			   dev->h264_parameter_sets_len,
			   READ_ONCE(dev->pipeline_reconfigure_pending) ?
			   ", replacement pending" : "",
			   READ_ONCE(dev->pipeline_start_failed) ?
			   ", deferred start failed" : "");
		seq_printf(m, "  h264 frames: %llu delivered, %llu dropped, %llu suppressed behind placeholder, %llu discarded while detached (pipeline lifetime)\n",
			   (unsigned long long)dev->h264_frames_delivered,
			   (unsigned long long)dev->h264_frames_dropped,
			   (unsigned long long)dev->h264_frames_suppressed,
			   (unsigned long long)dev->h264_frames_discarded);
		if (dev->deliver_raw) {
			seq_printf(m, "  raw frames : %llu delivered, %llu dropped, %llu stub completions skipped (post_skip=%u -> every %uth frame whole, %zu bytes)\n",
				   (unsigned long long)dev->raw_frames_delivered,
				   (unsigned long long)dev->raw_frames_dropped,
				   (unsigned long long)dev->raw_probe_stub_frames,
				   mz0380_post_skip, mz0380_post_skip + 1,
				   mz0380_raw_frame_bytes(dev));
			/*
			 * M244: the chroma test's own reachability, printed
			 * next to its rejection count. Zero rejections out of
			 * zero probes is dead code; zero out of thousands,
			 * with the best match short of the sample count, is a
			 * live test that never had cause to fire.
			 */
			seq_printf(m, "  raw fills  : %llu slots skipped with luma still clear (M229), %llu with luma done but CHROMA still clear (M238), chroma test ran %llu times, best %u/%u samples at the clear value (M244)\n",
				   (unsigned long long)dev->raw_incomplete_tail,
				   (unsigned long long)dev->raw_incomplete_chroma,
				   (unsigned long long)dev->raw_chroma_probes,
				   dev->raw_chroma_max_clear,
				   MZ0380_RAW_FILL_SAMPLES);
			seq_printf(m, "  raw repeats: %llu identical-head observations, %llu torn frames discarded mid-copy, %llu completions with >1 slot ready (M222/M223)\n",
				   (unsigned long long)dev->raw_dup_content,
				   (unsigned long long)dev->raw_frames_torn,
				   (unsigned long long)dev->raw_multi_landed);
		}
		/*
		 * M246: the audio window. events counts EVENT words with any
		 * audio bit; slots/bytes what was consumed; empty a completion
		 * whose slot was still poison; overrun a write past 4 KiB; the
		 * extent is the most any one completion wrote.
		 */
		if (dev->audio_capable) {
			seq_printf(m, "  audio      : %llu events, %llu slots, %llu bytes, %llu empty, %llu overrun, %llu bad token, %llu fifo drops, max extent %u, slots seen 0x%x, last event %08x token %08x (M246)\n",
				   (unsigned long long)dev->audio_events,
				   (unsigned long long)dev->audio_slots,
				   (unsigned long long)dev->audio_bytes,
				   (unsigned long long)dev->audio_empty,
				   (unsigned long long)dev->audio_overrun,
				   (unsigned long long)dev->audio_bad_token,
				   (unsigned long long)dev->audio_fifo_drops,
				   dev->audio_max_extent, dev->audio_slots_seen,
				   dev->audio_last_event, dev->audio_last_token);
			seq_printf(m, "  audio alsa : %llu bytes delivered, %llu periods, %llu pushes refused (why 0x%x: 1 no ring, 2 not open, 4 not running, 8 no sizes, 16 no buffer); audio for %llu ms (M246)\n",
				   (unsigned long long)dev->audio_alsa_bytes,
				   (unsigned long long)dev->audio_alsa_periods,
				   (unsigned long long)dev->audio_alsa_bails,
				   dev->audio_alsa_bail_why,
				   (unsigned long long)div_u64(dev->audio_last_ns -
						dev->audio_first_ns, NSEC_PER_MSEC));
			seq_printf(m, "  audio pcm  : %u opens, %u prepares, %u starts, %u stops, %u closes; refused %llu while closed, %llu while open but not started (M246)\n",
				   dev->audio_alsa_opens, dev->audio_alsa_prepares,
				   dev->audio_alsa_starts, dev->audio_alsa_stops,
				   dev->audio_alsa_closes,
				   (unsigned long long)dev->audio_alsa_bail_closed,
				   (unsigned long long)dev->audio_alsa_bail_idle);
		}
		/*
		 * Labelled for what it is. "no signal : inactive" was read by
		 * the operator as the driver reporting no HDMI source, when it
		 * means the opposite - the NO SIGNAL placeholder is not being
		 * shown, which is the healthy state.
		 */
		/*
		 * M237: both values are already in memory, so this costs no
		 * mailbox traffic - which after M236 is the deciding factor for
		 * anything printed on a path that gets polled.
		 */
		/*
		 * M237: both values are already in memory, so this costs no
		 * mailbox traffic - which after M236 is the deciding factor for
		 * anything printed on a path that gets polled.
		 *
		 * It is here to answer one question: does the source's colour
		 * space change when the camera switches between 50 and 60 Hz?
		 * The CSC choice is made from 0x48 once at stream start and
		 * never revisited, so if 0x48 differs between the two modes the
		 * wrong matrix survives the switch - and switching back does
		 * not restore it, which is what the operator reports.
		 */
		if (dev->mst_b2_48_valid) {
			static const char * const cs_names[] = {
				"RGB", "YUV422", "YUV444", "undefined"
			};

			seq_printf(m, "  colourspace: source %s (0x48=%02x cached)",
				   cs_names[(dev->mst_b2_48 & 0x60) >> 5],
				   dev->mst_b2_48);
		} else {
			seq_puts(m, "  colourspace: not yet read");
		}
		if (dev->mst_csc_applied_valid)
			seq_printf(m, ", CSC 0x92=%02x (%s) (M237)\n",
				   dev->mst_csc_applied,
				   dev->mst_csc_applied ? "hdcapm RGB matrix" :
							  "bypass");
		else
			seq_puts(m, ", CSC never written (M237)\n");
		seq_printf(m, "  rearms     : %llu MST3367 AUTO_POSITION re-arms (only on real lock loss since M233)\n",
			   (unsigned long long)dev->mst_rearms);
		seq_printf(m, "  placeholder: %s, %llu NO SIGNAL IDRs delivered, %llu cadence misses, %llu withheld from a raw node (M225)\n",
			   READ_ONCE(dev->no_signal_active) ?
			   "SHOWING (no usable source)" : "not shown (source is live)",
			   (unsigned long long)dev->no_signal_frames_delivered,
			   (unsigned long long)dev->no_signal_frames_missed,
			   (unsigned long long)dev->no_signal_frames_suppressed);
		if (READ_ONCE(dev->last_h264_frame_stamp))
			seq_printf(m, "  recovery  : %s, last card H.264 access unit %ums ago\n",
				   READ_ONCE(dev->signal_recovering) ?
				   "active" : "idle",
				   jiffies_to_msecs(jiffies -
					READ_ONCE(dev->last_h264_frame_stamp)));
	}
	seq_printf(m, "  frame size : %ux%u\n",
		   dev->capture.width, dev->capture.height);
	seq_printf(m, "  frame rate : %u.%03u fps\n",
		   fps_milli / 1000, fps_milli % 1000);
	if (encoded_fps_milli) {
		if (frame_divisor)
			seq_printf(m, "  encoded rate: ~%u.%03u fps (%u fps input / tinyvenc7 divisor %u)\n",
				   encoded_fps_milli / 1000,
				   encoded_fps_milli % 1000,
				   encoded_source_fps, frame_divisor);
		else
			seq_printf(m, "  encoded rate: ~%u.%03u fps (%u fps input / tinyvenc7 all-frame bitmap)\n",
				   encoded_fps_milli / 1000,
				   encoded_fps_milli % 1000,
				   encoded_source_fps);
	}
	if (dev->capture.source_width && dev->capture.source_height)
		seq_printf(m, "  source     : %ux%u%c @ %u fps (SET_VIC input geometry)\n",
			   dev->capture.source_width,
			   dev->capture.source_height,
			   dev->capture.source_interlaced ? 'i' : 'p',
			   dev->capture.source_fps);
	else
		seq_puts(m, "  source     : unlocked\n");
	seq_printf(m, "  input      : %s\n",
		   mz0380_input_name(dev->capture.input));
	if (mz0380_read_hw_input_select(dev, &hw_input, &hw_word)) {
		if (hw_input < MZ0380_INPUT_COUNT)
			seq_printf(m,
				   "  input hw   : %s (%u) raw=%08x via BAR5[0x%04x][2:0]\n",
				   mz0380_input_name(hw_input), hw_input,
				   hw_word, MZ0380_INPUT_SELECT_HW_REG);
		else
			seq_printf(m,
				   "  input hw   : invalid (%u) raw=%08x via BAR5[0x%04x][2:0]\n",
				   hw_input, hw_word,
				   MZ0380_INPUT_SELECT_HW_REG);
	}
	seq_printf(m, "  recordmode : %s\n",
		   mz0380_record_mode_name(dev->capture.record_mode));
	seq_printf(m, "               property=%u value=%u\n",
		   MZ0380_RECORD_MODE_PROPERTY, dev->capture.record_mode);
	if (mz0380_read_hw_record_mode(dev, &hw_mode, &hw_mode_word,
				       &hw_mode_reg, &hw_mode_mask,
				       &hw_mode_shift)) {
		seq_printf(m,
			   "  record hw  : %s (%u) raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   mz0380_record_mode_name(hw_mode), hw_mode,
			   hw_mode_word, hw_mode_reg, hw_mode_mask,
			   hw_mode_shift);
	}
	seq_printf(m, "  quality    : %u\n", dev->capture.quality);
	if (mz0380_read_hw_quality(dev, &hw_quality, &hw_quality_word,
				   &hw_quality_reg, &hw_quality_mask,
				   &hw_quality_shift)) {
		seq_printf(m,
			   "  quality hw : %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   hw_quality, hw_quality_word,
			   hw_quality_reg, hw_quality_mask,
			   hw_quality_shift);
	}
	/*
	 * M245: the encoder's own numbers, from the V4L2 controls, as sent in
	 * SET_ENC_PARAMS. The "bitrate"/"gop" lines below them are the BAR5
	 * property experiment, which the encoder has never read.
	 */
	seq_printf(m, "  encoder    : bitrate=%u gop=%u (V4L2 controls, sent in SET_ENC_PARAMS)\n",
		   dev->capture.enc_bitrate, dev->capture.enc_gop);
	seq_printf(m, "  bitrate    : %u\n", dev->capture.bitrate);
	if (mz0380_read_hw_bitrate(dev, &hw_bitrate, &hw_bitrate_word,
				   &hw_bitrate_reg, &hw_bitrate_mask,
				   &hw_bitrate_shift)) {
		seq_printf(m,
			   "  bitrate hw : %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   hw_bitrate, hw_bitrate_word,
			   hw_bitrate_reg, hw_bitrate_mask,
			   hw_bitrate_shift);
	}
	seq_printf(m, "  gop        : %u\n", dev->capture.gop_size);
	if (mz0380_read_hw_gop(dev, &hw_gop, &hw_gop_word,
			       &hw_gop_reg, &hw_gop_mask,
			       &hw_gop_shift)) {
		seq_printf(m,
			   "  gop hw     : %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   hw_gop, hw_gop_word,
			   hw_gop_reg, hw_gop_mask,
			   hw_gop_shift);
	}
	seq_printf(m, "  qpstep     : %u\n", dev->capture.qp_step);
	if (mz0380_read_hw_qp_step(dev, &hw_qp_step, &hw_qp_step_word,
				   &hw_qp_step_reg, &hw_qp_step_mask,
				   &hw_qp_step_shift)) {
		seq_printf(m,
			   "  qpstep hw  : %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   hw_qp_step, hw_qp_step_word,
			   hw_qp_step_reg, hw_qp_step_mask,
			   hw_qp_step_shift);
	}
	if (mz0380_read_candidate_qp_step(dev, &candidate_qp_step,
					  &candidate_qp_step_word,
					  &candidate_qp_step_reg,
					  &candidate_qp_step_mask,
					  &candidate_qp_step_shift)) {
		seq_printf(m,
			   "  qpstep cand: %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   candidate_qp_step, candidate_qp_step_word,
			   candidate_qp_step_reg, candidate_qp_step_mask,
			   candidate_qp_step_shift);
	}
	seq_printf(m, "  b-frames   : %u\n", dev->capture.b_frames);
	if (mz0380_read_hw_b_frames(dev, &hw_b_frames, &hw_b_frames_word,
				    &hw_b_frames_reg, &hw_b_frames_mask,
				    &hw_b_frames_shift)) {
		seq_printf(m,
			   "  bframes hw : %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   hw_b_frames, hw_b_frames_word,
			   hw_b_frames_reg, hw_b_frames_mask,
			   hw_b_frames_shift);
	}
	if (mz0380_read_candidate_b_frames(dev, &candidate_b_frames,
					   &candidate_b_frames_word,
					   &candidate_b_frames_reg,
					   &candidate_b_frames_mask,
					   &candidate_b_frames_shift)) {
		seq_printf(m,
			   "  bframes cand: %u raw=%08x via BAR5[0x%04x] mask=0x%x shift=%u\n",
			   candidate_b_frames, candidate_b_frames_word,
			   candidate_b_frames_reg, candidate_b_frames_mask,
			   candidate_b_frames_shift);
	}
}
