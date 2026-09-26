// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 encoder configuration and stream start/stop sequencing.
 */

#include "mz0380-dma-internal.h"

static int mz0380_stream_configure_encoder(struct mz0380_dev *dev, u32 fps,
					   u32 main_or_sub)
{
	u32 enc[10] = { 0 };
	/*
	 * M245: the main stream's GOP and bitrate come from the V4L2 controls,
	 * whose defaults are the Windows main-stream values - so an application
	 * that never sets them sends exactly what it always did. On the
	 * tinyvenc7 path the sub stream keeps Windows' own numbers (nothing
	 * reads it back); the legacy path mirrors main into sub as before.
	 */
	bool windows_sub = main_or_sub &&
			   (mz0380_h264_probe || mz0380_raw_probe_enc_tail);
	u32 gop = windows_sub ? 30 : dev->capture.enc_gop;
	u32 bitrate = windows_sub ? 4000000 : dev->capture.enc_bitrate;
	u32 quality = 0, frame_skip = 0, frame_avg = 0;
	u32 frame_divisor = 0;
	bool fallback = false;
	int ret;

	if (!fps || fps > U8_MAX) {
		fps = MZ0380_ENC_SAFE_FPS;
		fallback = true;
	}
	if (!gop || gop > U8_MAX) {
		gop = MZ0380_ENC_SAFE_GOP;
		fallback = true;
	}
	if (bitrate < MZ0380_MIN_BITRATE || bitrate > MZ0380_MAX_BITRATE) {
		bitrate = MZ0380_ENC_SAFE_BITRATE;
		fallback = true;
	}

	/*
	 * M82: the sub stream is a second, independent encoder. Windows always
	 * configures both, back to back, and only the second carries
	 * main_or_sub = 1 (its dword build ORs a literal 0x100 into card+0x08
	 * at 0x14028c643 - the byte-9 position, which is exactly where our
	 * tinyvenc5-derived map already put the field). Its own numbers in the
	 * traces were gop = 30 and bitrate = 4 Mbit against the main stream's
	 * gop = 32 / 4 MiB; nothing suggests the card cares, so the sub stream
	 * just mirrors the main configuration here.
	 */
	if (mz0380_h264_probe || mz0380_raw_probe_enc_tail) {
		/*
		 * Mask and quality are the exact values logged by the Windows
		 * 1.1.195.0 capture driver. GOP and bitrate were too (32 and
		 * 4194304 for main, 30 and 4000000 for sub) and still are by
		 * default - see the top of this function.
		 */
		enc[0] = 0x3fff;
		quality = 24;
		/*
		 * M204 corrects the old QP interpretation: tinyvenc7's own
		 * SET_ENC printf names payload bytes 20 and 21 `skip` and `avg`.
		 * A non-zero skip byte is used directly by the vcap modulo gate;
		 * zero selects the 128-bit schedule which h264_param_processing()
		 * fills locally. Its default tiny_calculate_skip_fps(fps, 0)
		 * result has every input-frame bit set. M205 validates that zero
		 * mode at approximately 60 encoded fps from 60-Hz input, making it
		 * the normal default. Divisor/skip 2 remains the proven 30-fps
		 * fallback. One remains invalid because counter % 1 never has
		 * remainder one.
		 */
		frame_divisor = mz0380_h264_frame_divisor;
		if (frame_divisor == 1 || frame_divisor > U8_MAX) {
			pr_warn("%s: h264_frame_divisor=%u is invalid for tinyvenc7; using 2\n",
				dev->name, frame_divisor);
			frame_divisor = 2;
		}
		frame_skip = frame_divisor;
	} else {
		enc[0] = mz0380_enc_mask ?: (MZ0380_ENC_VALID_FPS |
					     MZ0380_ENC_VALID_GOP |
					     MZ0380_ENC_VALID_BITRATE);
	}
	enc[1] = (gop << 24) | (fps << 16) |
		 ((main_or_sub & 0xff) << 8);	/* channel zero */
	/* quality, profile, entropy, mode */
	enc[2] = quality;
	enc[3] = bitrate;
	/* tinyvenc7 payload bytes 20/21 are skip/avg; later fields stay zero. */
	enc[4] = frame_skip | (frame_avg << 8);

	/* Match Windows' EVENT-wait path; timeout 0 would permit mailbox reuse. */
	ret = mz0380_send_command(dev, MZ0380_CMD_SET_ENC_PARAMS, enc,
				  ARRAY_SIZE(enc), NULL, 5000);
	pr_info("%s: stream start: SET_ENC_PARAMS(op 0x2d, mask=0x%04x, %s ch0, fps=%u, gop=%u, quality=%u, bitrate=%u, skip=%u, avg=%u, tinyvenc7 schedule=%s%s) ret=%d\n",
		dev->name, enc[0], main_or_sub ? "sub" : "main", fps, gop,
		quality, bitrate, frame_skip, frame_avg,
		frame_divisor ? "modulo" : "all-frame bitmap",
		fallback ? ", conservative fallback applied" : "", ret);
	return ret;
}

/*
 * STOP_STREAMING as Windows sends it: word[2] = 0xFFFFFFFF ("all channels"),
 * count 3. We have always sent a bare opcode, i.e. channel 0 implicitly and
 * count 2. M82.
 */
static int mz0380_stream_stop_all(struct mz0380_dev *dev, unsigned int timeout_ms)
{
	u32 stop_all = 0xffffffffu;

	return mz0380_send_command(dev, MZ0380_CMD_STOP_STREAMING,
				   &stop_all, 1, NULL, timeout_ms);
}

/*
 * op 0x31, the command that closes every Windows capture-start sequence. We
 * have always called it POST_PROC; M128's static decode of tinyvenc5 names it
 * SET_PREVIEW_PARAMS and gives the whole 20-byte payload:
 *
 *   [4..7]=mask  [8]=ch  [9]=fps  [0x0a]=skip  [0x0b]=avg  [0x0c]=die_en
 *   [0x0d]=preview_off  [0x0e]=fake_frame_off  [0x0f]=preview_no_osd
 *   [0x10]=hw_d  [0x11]=is_mirror  [0x12]=is_flip
 *
 * M214: the last three are given here in tinyvenc7's order, which is what this
 * card runs. M128's tinyvenc5 decode had them as mirror/flip/hw_d. We send
 * zeros in all three, so the correction is not observable today.
 *
 * so post_di is die_en. On this board every field except fps and die_en is
 * zero in the Windows traces, and die_en is 1 even for a progressive source.
 * fake_frame_off is ours, not Windows': see mz0380_fake_frame_off in core.c.
 */
static int mz0380_stream_post_proc(struct mz0380_dev *dev, u32 fps)
{
	u32 post[5] = { 0 };
	int ret;

	post[0] = mz0380_post_mask;
	/*
	 * M218: raw delivery is useless without mask bit 0.
	 *
	 * Bit 0 is what makes the card populate its preview frame-selection
	 * bitmap; without it every frame takes the not-selected arm and writes
	 * a 16-byte stub, so a node that had been switched to I420 by S_FMT
	 * would sit there delivering nothing at all (M214/M215). Since the
	 * format is now an application's choice rather than an operator's, the
	 * operator is no longer there to set the matching parameter - so tie
	 * the bit to the decision that needs it.
	 */
	if (dev->deliver_raw)
		post[0] |= 0x01;
	post[1] = (fps & 0xff) << 8 |			/* [8]=ch [9]=fps  */
		  (mz0380_post_skip & 0xff) << 16 |	/* [10]=skip       */
		  (mz0380_post_avg & 0xff) << 24;	/* [11]=avg        */
	post[2] = (mz0380_post_di & 0xff) |		/* [12]=die_en     */
		  ((mz0380_fake_frame_off ? 1u : 0u) << 16); /* [14] */

	ret = mz0380_send_command(dev, mz0380_post_proc_opcode, post,
				  ARRAY_SIZE(post), NULL, 5000);
	pr_info("%s: stream start: SET_PREVIEW_PARAMS(op 0x%02x, mask=0x%02x%s, fps=%u, skip=%u, avg=%u, die_en=%u, fake_frame_off=%u) ret=%d\n",
		dev->name, mz0380_post_proc_opcode, post[0],
		dev->deliver_raw ? " (bit0 forced for raw delivery)" : "", fps,
		mz0380_post_skip, mz0380_post_avg,
		mz0380_post_di, mz0380_fake_frame_off, ret);
	return ret;
}

/*
 * Start streaming: program the buffer physaddrs into the card, then arm the
 * encoder with SET_VIC_PARAMS (the same op 0x29 path input-select uses). Per
 * M17 the card's own userspace flips its internal enables in response, so no
 * further host kick should be needed - a live run will confirm.
 */
int mz0380_dma_start(struct mz0380_dev *dev)
{
	u32 params[9] = { 0 };
	u32 out_w = dev->capture.width, out_h = dev->capture.height;
	u32 in_w = dev->capture.source_width ?: dev->detected_timings.bt.width;
	u32 in_h = dev->capture.source_height ?: dev->detected_timings.bt.height;
	bool interlaced = dev->capture.source_interlaced;
	u32 fps = dev->capture.source_fps ?:
		  mz0380_timings_fps(&dev->detected_timings);
	bool fps_ok;
	/*
	 * byte6 "fw": encoder selector (M71) AND cfg output-format selector
	 * (M79: vcm writes "output format" = 2/YUY2 when fw == 6, else 1/YV12;
	 * fw == 7 spawns ./tinyvenc7, fw == 8 ./tinyvenc8, anything else
	 * ./tinyvenc5). M82: the Windows driver only ever sends 6 or 7, picked
	 * by frame rate - 6 for 1080p30 and 1080p29.97, 7 for 1080p60, in
	 * every [CH00] line of every trace. It never sends 5, which is what we
	 * have always sent. 0 here means "use the Windows rule".
	 */
	u32 fw = mz0380_vic_fw ?: (fps > 30 ? 7u : 6u);   /* 0 = Windows rule; see M88 */
	/*
	 * byte7: VideoCap INPUT FORMAT, per the SDK capture config
	 * (re-dump/fw/yuan_demo_sdi/nullsensor_1920x1080.cfg): "input format
	 * (1:8-bits Raw, 2:CCIR656i, 3:CCIR656p, 4:Bayer, 5:16-bits Raw,
	 * 6:BT1120p, 7:BT1120i)". The card's own printf calls the field
	 * "interlace", which is why M71 read it as a boolean and M103 briefly
	 * reclassified it back - but the label is loose, not wrong: 6 vs 7 is
	 * BT1120p vs BT1120i.
	 *
	 * M104 settled it ON HARDWARE and the enum reading won. 3, 6 and 7 all
	 * reach the card's NOSG splash and are indistinguishable; 0 - which is
	 * not in the enum - produces nothing at all, i.e. VideoCap never opens
	 * and tinyvenc exits before it can draw. That also PROVES the byte is
	 * consumed, which the three indistinguishable values could not.
	 *
	 * MZ0380_VIC_IN_FMT_AUTO exists because the old `vic_in_fmt ?: derived`
	 * idiom could not express 0 at all, so the field's own sweep knob could
	 * not reach part of its range. The derived default is unchanged.
	 */
	u32 in_fmt = (mz0380_vic_in_fmt == MZ0380_VIC_IN_FMT_AUTO)
			? (interlaced ? 7u : 6u) : mz0380_vic_in_fmt;
	u32 out_fmt = mz0380_vic_out_format;    /* byte12 "m", see M72 */
	u32 vic_in_w, vic_in_h;
	bool aic_newly_armed = false;
	bool vic_fired = false;
	int ret;

	if (!dev->dma_armed)
		return -ENODEV;
	if (!in_w)
		in_w = out_w;
	if (!in_h)
		in_h = out_h;

	/*
	 * SET_VIC_PARAMS 44-byte struct - byte offsets VERIFIED by disassembling
	 * video_capture_mgr's op-41 handler (RE_FINDINGS.md M23). The mailbox puts
	 * the opcode at struct[0..3]; our params[i] lands at struct[4+4i..7+4i]
	 * (params[0]=struct[4..7]). Authoritative field map:
	 *   [4]=ch  [5]=fps  [6]=fw  [7]=input format (6=BT1120p, 7=BT1120i)
	 *   [8..9]=width  [10..11]=height  [12]=m  [13]=flip  [14]=mirror
	 *   [16..19]=color_info  [20..21]=x_start  [22..23]=y_start
	 *   [24..25]=input_frame_width  [26..27]=input_frame_height
	 *   [28]=bitstream_num(MUST be >=1)  [29]=osd_en  [30]=osd_size
	 *   [31]=is_nosg  [32]=vanc_lines  [33]=fast_kill
 *   [34]=frame-completion interrupt enable (M136 - was guessed as "mix")
 *   [35]=is_slave
	 *   [36..39]=nosg back/y/u/v
	 *
	 * M71: bytes 6 and 7 corrected against the card's OWN printf. The
	 * format string at video_capture_mgr .rodata 0xb734 is
	 *   "[Video_MGR][ch%d] SET_VIC fw(%d), fps(%d), resolution(%dx%d)
	 *    interlace(%d), m(%d), color_info ..."
	 * and decoding its call at vcm 0x8f60..0x8fbc under AAPCS gives
	 * r1=[cmd+4]=ch, r2=[cmd+6]=fw, r3=[cmd+5]=fps, then stack args
	 * [cmd+8]=W, [cmd+10]=H, [cmd+7]=interlace, [cmd+12]=m, ... So byte6
	 * is the ENCODER SELECTOR (vcm 0x9250 compares it to 7 -> ./tinyvenc7,
	 * and 8 -> ./tinyvenc8, else ./tinyvenc5).
	 *
	 * M127/M128 CORRECTION to the rest of that paragraph: byte7 is NOT an
	 * interlace boolean. video_capture_mgr only *labels* it "interlace(%d)"
	 * in the printf above; the value is sprintf'd RAW into the cfg line
	 * matching "input format", whose enum is 6 = BT1120p and 7 = BT1120i.
	 * The code below (in_fmt) is right and always was - it was this comment
	 * that was wrong. Historical note: the pre-M71 packing had byte6 = 2|3,
	 * which spawned tinyvenc5 only by fallthrough.
	 * params[8] carries the no-signal colour bytes (M82: 0.00.80.80); the
	 * following struct word is PARAM10/STATUS and is never SET_VIC payload.
	 * The old M20/M22 packing put input_w/input_h/bitstream_num two bytes early,
	 * so the firmware read bitstream_num=0 (byte28 unset) and garbage input dims
	 * -> tinyvenc5 spawned but silent. This is the corrected layout.
	 * is_nosg=1 spawns the card's fake_frame_process (black-frame generator,
	 * no capture/SSM dependency): a stream_nosg test lever that bisects the
	 * encode+DMA path from the upstream BT1120 capture path.
	 */
	/*
	 * [5] = fps. We had always left this 0, which the card patches up with
	 * "Error!!! vic_fps cannot be 0, set fps to 60" - harmless while the
	 * fake-frame generator paced itself, but the real capture path derives
	 * its frame cadence from it, so send the rate we actually detected.
	 */
	params[0] = (fps << 8) | (fw << 16) | (in_fmt << 24);
	/*
	 * M72: byte12 = output format, bytes 16..19 = brightness, contrast,
	 * saturation, field-invert. Both blocks were left at zero for this
	 * driver's whole life; zero is not a legal output format and zero
	 * saturation is the config's documented "mono".
	 */
	params[2] = out_fmt & 0xff;
	/*
	 * M82: bytes 16..19 are color_info[0..3], and the Windows driver sends
	 * a fixed 1,1,1,2 on every single SET_VIC in all four traces - never a
	 * per-picture value. Decoded from the dword build at 0x14028bcb9,
	 *   eax = c3<<24 | c1<<16 | c2<<8 | c1
	 * cross-checked against its printf "color = %d.%d.%d.%d" which prints
	 * the same three stack slots in the order c3,c1,c2,c1 and read
	 * "color = 2.1.1.1" in every trace. So c1=1, c2=1, c3=2 and the bytes
	 * on the wire are [16]=1 [17]=1 [18]=1 [19]=2.
	 *
	 * This supersedes the M72 reading of the same four bytes as
	 * brightness/contrast/saturation/field-invert: that guess put 128 in
	 * byte 18 and zero everywhere else, which is not a value the retail
	 * driver ever sends. vic_saturation is kept only as an override.
	 */
	params[3] = mz0380_vic_color_info;
	if (mz0380_vic_saturation != MZ0380_VIC_SATURATION_UNSET)
		params[3] = (params[3] & ~0x00ff0000u) |
			    ((mz0380_vic_saturation & 0xff) << 16);
	/*
	 * M139: bytes 8..9 / 10..11.  The cfg patcher turns these into the
	 * card's capture geometry AND into m_vic_width, which is the value
	 * img_handler's frame gate tests.  Overridable so the gate can be
	 * moved without moving the v4l2 format.
	 */
	if (mz0380_vic_out_w)
		out_w = mz0380_vic_out_w;
	if (mz0380_vic_out_h)
		out_h = mz0380_vic_out_h;
	params[1] = ((out_h & 0xffff) << 16) | (out_w & 0xffff);
	/*
	 * M76: bytes 24..27 are the VIC's own width/height register, patched
	 * into the card's cfg as "input frame width/height" independently of
	 * the capture geometry. Overridable to test the 8-bit double-rate
	 * hypothesis (3840) against the VIC's width check.
	 */
	vic_in_w = mz0380_vic_in_w ?: in_w;
	vic_in_h = mz0380_vic_in_h ?: in_h;
	params[5] = ((vic_in_h & 0xffff) << 16) | (vic_in_w & 0xffff);
	/* [28]=bitstream_num (M148 knob, def 1), [31]=is_nosg */
	params[6] = (mz0380_bitstream_num & 0xffu) |
		    ((mz0380_stream_nosg ? 1u : 0u) << 24);
	/*
	 * M82: byte 33 is fast_kill and Windows sends 1, always ("fk=1" in
	 * every [CH00] line of every trace). Bytes 32 (vanc_lines) and 34..35
	 * (mix, is_slave) are zero there too, which is what we already send.
	 *
	 * M157: byte 33 is the only field where we deliberately diverge from
	 * Windows - it picks SIGKILL (1) versus SIGINT-and-wait (0) for the
	 * card's teardown of the previous tinyvenc5, and only the second runs
	 * the encoder's destructor and atexit cleanup. Default 0. See the
	 * mz0380_vic_fast_kill block in mz0380-core.c.
	 */
	params[7] = ((mz0380_vic_fast_kill & 0xff) << 8) |	/* [33] */
		    ((mz0380_vic_int_mode & 0xff) << 16);	/* [34] M136 */
	/*
	 * M82: bytes 36..39 are the no-signal fill colour, and Windows sends
	 * back=0, Y=0x00, U=0x80, V=0x80 - neutral grey, not the all-zero
	 * (green) we have been sending. Only consumed when the card falls back
	 * to its NOSG generator, but it costs nothing to match.
	 */
	params[8] = mz0380_vic_nosg;

	/*
	 * M209 is intentionally one narrow experiment.  The 0x2f7600 oracle is
	 * valid only for the retail 1080p60 tinyvenc7 geometry with no VBI.  Refuse
	 * a different source instead of turning a different byte count into a
	 * false op02/op08 result.
	 */
	fps_ok = fps == 60 || (mz0380_raw_probe_allow_30 && fps == 30);
	if (mz0380_raw_bank_probe &&
	    (mz0380_stream_nosg || fw != 7 || !fps_ok || interlaced ||
	     in_w != 1920 || in_h != 1080 || out_w != 1920 || out_h != 1080 ||
	     vic_in_w != 1920 || vic_in_h != 1080)) {
		pr_err("%s: M209 raw_bank_probe requires live progressive 1920x1080@%s, fw=7, 1920x1080 VIC/output geometry and VBI=0 (got input=%ux%u%s@%u fw=%u vic=%ux%u output=%ux%u nosg=%u)\n",
		       dev->name,
		       mz0380_raw_probe_allow_30 ? "60 or @30" : "60",
		       in_w, in_h, interlaced ? "i" : "p", fps, fw,
		       vic_in_w, vic_in_h, out_w, out_h, mz0380_stream_nosg);
		return -EINVAL;
	}
	if (mz0380_raw_bank_probe && fps == 30)
		pr_warn("%s: raw discriminator is running on a 1080p30 source, a deviation from the Windows-confirmed 1080p60; the 0x%x extent oracle is geometry-only and remains valid, but score this run as 30 Hz\n",
			dev->name, MZ0380_RAW_PROBE_FRAME_SIZE);

	/*
	 * M82: Windows precedes EVERY reconfiguration with a stop, and its
	 * "[FIRMWARE RESET]" log line is exactly that - opcode 0x07 with
	 * word[2] = 0xFFFFFFFF ("all channels"), count 3, flag 1
	 * (fire-and-forget), sent from 0x14028cf38 with the reconfiguration
	 * function called on the very next instruction. We have always sent
	 * STOP with an empty payload, and only on the unwind path.
	 *
	 * The traces show a consistent 1.84-1.91 s between that stop and the
	 * SET_VIC that follows it. Part of it is eight msleep() calls inside
	 * the config function; the rest is unattributed, so the safe reading
	 * is that the card is not ready for 0x29 immediately.
	 */
	if (mz0380_win_seq) {
		int stop_ret = mz0380_stream_stop_all(dev, 0);

		pr_info("%s: stream start: pre-STOP(op 0x07, all channels) ret=%d, settling %u ms\n",
			dev->name, stop_ret, mz0380_stop_settle_ms);
		msleep(mz0380_stop_settle_ms);

		/*
		 * Windows registers its capture buffers when the pin opens,
		 * which is before the reconfiguration - the opposite of the
		 * M23 placement below, which was chosen so that op6's iATU
		 * latch would see our addresses. The two only agree when there
		 * is no op6; win_bufs_first=0 keeps the M23 placement.
		 */
		if (mz0380_win_bufs_first) {
			ret = mz0380_stream_program_bufs(dev);
			if (ret) {
				pr_warn("%s: SET_BUF failed (%d) - frames will not flow\n",
					dev->name, ret);
				goto err_events;
			}
		}
	}

	/*
	 * M155: optionally skip SET_VIC on every cycle after the first, so the
	 * cycle does not respawn tinyvenc5. See the setvic_once parameter.
	 */
	if (mz0380_setvic_once && dev->stream_cycles > 0) {
		pr_info("%s: stream start: SET_VIC SKIPPED (setvic_once=1, cycle %u) - no respawn; if a frame still arrives it did not come from the spawn\n",
			dev->name, dev->stream_cycles);
		ret = 0;
		goto vic_done;
	}

	/* Even a timed-out transaction may already have spawned tinyvenc5. */
	vic_fired = true;
	/*
	 * M169: count it here, on the fire rather than on the result, for
	 * exactly the reason the line above gives - a SET_VIC that times out
	 * may still have forked an encoder, and a spawn budget that only counts
	 * successes would under-report precisely when the card is in trouble.
	 *
	 * This is the single SET_VIC site in the driver, so this counter sees
	 * every spawn including the nosg loop's per-frame respawns, which reach
	 * it through mz0380_dma_start().
	 */
	dev->encoder_spawns++;
	if (dev->encoder_spawns == 8)
		pr_warn("%s: 8 encoder spawns since insmod - entering the 8-18 range where the card has wedged before (recovery is a mains-off cold boot, or mz0380-m52-card-recovery.sh). The budget is per POWER CYCLE, not per insmod: ./mz0380-spawns.sh has the running total\n",
			dev->name);
	ret = mz0380_send_command(dev, MZ0380_CMD_SET_VIC_PARAMS, params,
				  ARRAY_SIZE(params), NULL, 2000);
	if (mz0380_stream_nosg)
		pr_info("%s: stream start: SET_VIC(synthetic no-signal source %ux%u@%u %s, fw=%u; host output is polled NV12, not live HDMI/H.264) ret=%d\n",
			dev->name, in_w, in_h, fps,
			interlaced ? "i" : "p", fw, ret);
	else
		/*
		 * M137: fk and int_mode added. Byte 34 (int_mode) was set for
		 * the first time in M136 and this line did not print it, so the
		 * negative result could not be distinguished from "the knob
		 * never landed" - method rule 9, in a line that has been short
		 * of these two bytes all along.
		 */
		pr_info("%s: stream start: SET_VIC(input=%ux%u%s@%u fw=%u in_fmt=%u out_fmt=%u vic_in=%ux%u fk=%u int_mode=%u -> %s output=%ux%u, bitstreams=%u) ret=%d\n",
			dev->name, in_w, in_h, interlaced ? "i" : "p",
			fps, fw, in_fmt, out_fmt, vic_in_w, vic_in_h,
			mz0380_vic_fast_kill & 0xff,
			mz0380_vic_int_mode & 0xff,
			mz0380_raw_bank_probe ? "M209 raw-only" : "H.264",
			out_w, out_h,
			mz0380_bitstream_num & 0xffu, ret);
vic_done:
	dev->stream_cycles++;
	if (ret)
		goto err_events;

	/*
	 * SET_VIC only spawns the encoder (via video_capture_mgr) and clears
	 * no_signal; tinyvenc5 then blocks on /sys/vpl_pciep/epint waiting for a
	 * separate START_STREAMING (op 0x06) before it DMAs any frame (M22).
	 * Give the freshly system()-forked tinyvenc5 time to exec, open epint and
	 * consume the SET_VIC(0x29) it reads first, so our op6 lands as the next
	 * distinct command rather than racing its start-up read. The delay is a
	 * heuristic for the on-card process spawn; tune against hardware via the
	 * start_delay_ms module param if the first frame is missed (symptom: no
	 * rising IRQ/token count after op6, IRQ 164 stuck at the idle value 3).
	 */
	/* M82: Windows inserts no settle at all between 0x29 and 0x2a. */
	if (!mz0380_win_seq)
		msleep(mz0380_start_delay_ms);

	/*
	 * Windows registers window 1 before SET_VIC, but SET_VIC creates a fresh
	 * tinyvenc process and the existing raw path has shown that card-side
	 * channel state may be rebuilt at that boundary. Re-register only the
	 * owned encoded bank after the spawn as well. This command does not wake
	 * the encoder and therefore cannot race an encoded DMA.
	 */
	if ((mz0380_h264_probe || mz0380_raw_bank_probe) &&
	    mz0380_win_seq && mz0380_win_bufs_first) {
		ret = (mz0380_raw_bank_probe || mz0380_raw_bank_observe) ?
			mz0380_stream_program_bufs(dev) :
			mz0380_h264_program_bufs(dev);
		if (ret)
			goto err_events;
		pr_info("%s: %s re-registered after SET_VIC spawn\n",
			dev->name, mz0380_raw_bank_probe ?
			"raw-only op02/op08 banks (no op04)" :
			mz0380_raw_bank_observe ?
			"op02/op08 raw banks and the op04 encoded window" :
			"H.264 window1");
	}

	/*
	 * SET_VIC launches tinyvenc5, but opcode 0x2d is the host-owned encoder
	 * configuration transaction.  Send it only after tinyvenc5 has consumed
	 * its mandatory first SET_VIC read.  Its non-zero timeout serializes the
	 * full-width mailbox packet through the command EVENT before SET_BUF can
	 * overwrite the shared words.  The NOSG diagnostic emits raw synthetic
	 * NV12 and deliberately skips H.264 configuration/spawn work.
	 *
	 * M82: in the Windows order the encoder commands come AFTER SET_AIC,
	 * so this runs further down instead.
	 */
	if (!mz0380_win_seq && !mz0380_stream_nosg &&
	    !mz0380_raw_bank_probe) {
		ret = mz0380_stream_configure_encoder(dev, fps, 0);
		if (ret)
			goto err_events;
		/* The Windows H.264 path always configures both encoder streams. */
		if (mz0380_h264_probe && mz0380_enc_sub) {
			ret = mz0380_stream_configure_encoder(dev, fps, 1);
			if (ret)
				goto err_events;
		}
	}

	/*
	 * NOW program the buffer physaddrs into channels[] - AFTER the SET_VIC spawn
	 * settled and BEFORE op6. START(op6) makes vpl_dmac latch channels[] into
	 * the outbound iATU; doing SET_BUF here (not before SET_VIC) guarantees our
	 * addresses are the ones latched, so the iATU low target = our buffer, not 0
	 * (RE_FINDINGS.md M23). op2 does not sysfs_notify, so it won't disturb the
	 * tinyvenc5 that is blocked waiting for op6.
	 */
	if (!mz0380_win_seq || !mz0380_win_bufs_first) {
		ret = mz0380_stream_program_bufs(dev);
		if (ret) {
			pr_warn("%s: SET_BUF failed (%d) - frames will not flow\n",
				dev->name, ret);
			goto err_events;
		}
	}
	if (!mz0380_stream_nosg) {
		if (mz0380_raw_bank_probe)
			pr_info("%s: M209 SET_BUF topology provides eight independent raw buffers: token%%8 0..3 -> op02 bank, 4..7 -> op08 bank; op04 is disabled\n",
				dev->name);
		else
			pr_info("%s: SET_BUF(op 0x%02x) provides four collision-free buffers (tokens 0..3); 3-bit tokens 4..7 are rejected and acknowledged until a second four-buffer allocation is wired to SET_BUF_8\n",
				dev->name, mz0380_set_buf_opcode);
	}

	/*
	 * Real H.264 needs an owned poison suffix for bounded length inference.
	 * NOSG keeps the older live extent diagnostic; its fixed raw size does not
	 * use the completion FIFO or inferred-length path.
	 */
	if (!mz0380_stream_nosg) {
		if (!mz0380_raw_bank_probe)
			mz0380_frame_buffers_poison_start(dev);
		/* Accept completions only after every token has a poison baseline. */
		mz0380_frame_events_start(dev);
	} else if (mz0380_buf_poison) {
		mz0380_extent_watch_start(dev);
	}

	/*
	 * M40: declare every bitstream slot free before the encoder starts.
	 * enc_stat<idx> (BAR0+0x50+idx) is the per-frame "may I encode into
	 * the host buffer" handshake; the card only ever sets it to 1 (after
	 * a bitstream DMA) and never clears it, so a stale 1 left by a
	 * previous stream would make the encoder retry 10x and then skip
	 * every frame.
	 *
	 * M151: this one is unconditional on purpose - the enc_stat_ack knob
	 * covers only the per-frame ack. Clearing once at start establishes a
	 * known value to read back at stop, and every result in the file was
	 * taken with it.
	 */
	mz_mmio_write(dev, MZ0380_MB_ENC_STATUS, MZ0380_MB_ENC_STAT_FREE);
	wmb();

	/*
	 * M33: release the audio gate BEFORE START. tinyvenc5's is_nosg path
	 * blocks on /sys/audio_status/audio_ready ("[tiny5]is_nosg wait audio
	 * timeout, i2s_num=%d, audio ready[%d]") before it will ACK
	 * START_STREAMING, and the only thing that sets that file is
	 * video_capture_mgr's SET_AIC_PARAMS handler when on=1. Without it the
	 * card DMAs a raw frame but never writes channel_done, which is exactly
	 * the state M30-M32 left us in: data in the buffer, no completion.
	 */
	if (mz0380_aic_on && (mz0380_aic_every_frame || !dev->aic_armed)) {
		bool was_armed = dev->aic_armed;
		u32 aic[4] = {
			/* cmd+4 channel_num | cmd+5 mono<<8 | cmd+6 bits<<16 */
			(mz0380_aic_channels & 0xff) |
			((u32)(mz0380_aic_channels == 1 ? 1 : 0) << 8) |
			((u32)(mz0380_aic_bits & 0xffff) << 16),
			mz0380_aic_freq,			/* cmd+8  freq  */
			(mz0380_aic_period_frames & 0xffff) |	/* cmd+12       */
			((u32)(mz0380_aic_periods & 0xffff) << 16), /* cmd+14   */
			1u |					/* cmd+16 on=1  */
			((u32)(mz0380_aic_int_mode & 0xff) << 8),/* cmd+17     */
		};

		ret = mz0380_send_command(dev, MZ0380_CMD_SET_AIC_PARAMS, aic,
					  ARRAY_SIZE(aic), NULL, 2000);
		pr_info("%s: stream start: SET_AIC(on=1, %u ch, %u bit, %u Hz, %u frames x %u periods) ret=%d\n",
			dev->name, mz0380_aic_channels, mz0380_aic_bits,
			mz0380_aic_freq, mz0380_aic_period_frames,
			mz0380_aic_periods, ret);
		if (!ret) {
			dev->aic_armed = true;
			aic_newly_armed = !was_armed;
		} else {
			/* Without audio_ready, tinyvenc5 never completes video frames. */
			goto err_events;
		}
	}

	/*
	 * M73: photograph the receiver's output stage on both sides of the
	 * encoder kick. If the VIC never sees a clock, the evidence is here and
	 * nowhere the host can otherwise reach - the card's own log is on a
	 * serial port we do not have.
	 */
	/*
	 * M130/M133: the source is locked by now, so 0x48 finally means
	 * something - pick the CSC mode from the colour space it is actually
	 * sending. The diag runs FIRST because its read of 0x48 is the one that
	 * works, and it caches the value we then use; M133 had these the other
	 * way round and the standalone read returned 0x00, selecting the RGB
	 * matrix for a YUV444 source.
	 */
	if (!mz0380_stream_nosg) {
		mz0380_mst3367_output_diag(dev, "before START");
		mz0380_mst3367_apply_csc_mode(dev);
	}

	/*
	 * M82: the Windows tail. Both encoder streams, then POST_PROC. ep.ko
	 * routes 0x2d (45) and 0x31 (49) to a bare sysfs_notify("epint"), the
	 * same wake op 0x06 performs, so this sequence is itself the kick and
	 * Windows never sends 0x06 on the capture path.
	 */
	if (mz0380_win_seq && !mz0380_stream_nosg &&
	    (!mz0380_raw_bank_probe || mz0380_raw_probe_enc_tail)) {
		if (mz0380_raw_bank_probe)
			pr_info("%s: M210: sending the Windows encoder tail on the raw-only sink topology; M209 proved op 0x06 alone leaves the producer dead\n",
				dev->name);
		ret = mz0380_stream_configure_encoder(dev, fps, 0);
		if (ret)
			goto err_events;
		if (mz0380_enc_sub) {
			ret = mz0380_stream_configure_encoder(dev, fps, 1);
			if (ret)
				goto err_events;
		}
		ret = mz0380_stream_post_proc(dev, fps);
		if (ret)
			goto err_events;
	}

	/*
	 * M128: the baseline (win_seq=0) never sends 0x31 at all, so the only
	 * way to reach fake_frame_off on the path that actually renders is to
	 * send it here. It has to precede op 0x06: the standby-thread guard runs
	 * inside 0x06's handler (new EncodingGroup -> on_start_thread -> Start ->
	 * init_func), and it reads preview_params_settings[ch].byte[0x0a] once,
	 * at pthread_create time. Nothing is sent unless the knob is set, so the
	 * default baseline is byte-identical to before.
	 */
	if (!mz0380_win_seq && !mz0380_stream_nosg &&
	    !mz0380_raw_bank_probe &&
	    (mz0380_fake_frame_off || mz0380_post_proc)) {
		ret = mz0380_stream_post_proc(dev, fps);
		if (ret)
			goto err_events;
		/* M128b: see mz0380_post_proc_gap_ms. 0x06 otherwise lands on
		 * the doorbell 9 us later, while the card is still inside the
		 * 0x31 handler. */
		if (mz0380_post_proc_gap_ms) {
			pr_info("%s: stream start: waiting %u ms before op 0x06 (M128b cadence test)\n",
				dev->name, mz0380_post_proc_gap_ms);
			msleep(mz0380_post_proc_gap_ms);
		}
	}

	/*
	 * M139: seed the frame-token registers with a sentinel before the card
	 * can touch them.
	 *
	 * M138's watch reported "0 changes, final 40/44/48 = 0", which is
	 * ambiguous: the card's store_channel_done() writes report[N] - 1 into
	 * one nibble per channel, so a single report carrying buffer index 1
	 * writes zero over a register that was already zero and looks identical
	 * to never having run at all.
	 *
	 * The write is a read-modify-write of one nibble
	 * (reg = (reg & ~(0xf << ch*4)) | (val << ch*4)), so a sentinel makes
	 * it unmistakable: the upper bits survive and only channel 0's nibble
	 * changes.  a5a5a5a5 -> a5a5a5a0 means the encoder reported exactly one
	 * frame from buffer 1; a5a5a5a5 unchanged means it never reported at
	 * all.  Nothing card-side ever reads these, so seeding them is inert.
	 *
	 * Self-validating: if BAR0 0x40 is not host-writable the producer
	 * watch's baseline line reads back something other than the sentinel
	 * and the whole test is void.
	 */
	if (mz0380_token_seed) {
		mz_mmio_write(dev, MZ0380_MB_EVT_PAYLOAD0, mz0380_token_seed);
		mz_mmio_write(dev, MZ0380_MB_EVT_PAYLOAD1, mz0380_token_seed);
		mz_mmio_write(dev, MZ0380_MB_EVT_PAYLOAD2, mz0380_token_seed);
		mz_mmio_write(dev, MZ0380_MB_EVT_PAYLOAD3, mz0380_token_seed);
		wmb();
		pr_info("%s: stream start: frame-token sentinel %08x seeded into BAR0 40/44/48/4c; readback %08x/%08x/%08x/%08x\n",
			dev->name, mz0380_token_seed,
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD0),
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1),
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2),
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3));
	}

	/*
	 * op 0x06 is FIRE-AND-FORGET. Unlike INIT/SET_VIC, its ep.ko handler
	 * (@0x1854, M22) only does sysfs_notify(epint) to wake tinyvenc5 - it
	 * posts NO mailbox completion (no STATUS bit0, no EVENT bit11). Waiting
	 * for one always burns the full timeout and returns a bogus -ETIMEDOUT
	 * (proven on hw: SET_VIC->op6 gap == msleep + full 2000ms, zero EVENT
	 * lines logged). So send with timeout_ms=0: fire the doorbell and return.
	 * Frame arrival is confirmed downstream by the MSI/outbound-ATU path, not
	 * by a command ack.
	 */
	if ((mz0380_raw_bank_probe && !mz0380_raw_probe_enc_tail) ||
	    !mz0380_win_seq || mz0380_win_start_op6) {
		ret = mz0380_send_command(dev, MZ0380_CMD_START_STREAMING,
					  NULL, 0, NULL, 0);
		pr_info("%s: stream start: START_STREAMING(op 0x06) fired (async, ret=%d)\n",
			dev->name, ret);
	} else {
		ret = 0;
	}
	if (ret)
		goto err_events;
	dev->stream_head = 0;

	/*
	 * Do not wait for the encoder after START. This function runs inside
	 * vb2's STREAMON callback, so userspace cannot dequeue and requeue its
	 * completed buffers until we return. The old 500 ms wait exhausted the
	 * initial vb2 queue at 30 fps and dropped a run of access units from the
	 * first H.264 GOP, leaving the decoder with broken references. The
	 * receiver/CSC diagnostic that affects setup already ran before START;
	 * any post-start observation must be asynchronous.
	 */

	/* M111: last, so it only runs once the stream is genuinely started. */
	mz0380_poll_drain_start(dev);
	WRITE_ONCE(dev->pipeline_running, true);
	WRITE_ONCE(dev->pipeline_reconfigure_pending, false);
	dev->pipeline_width = out_w;
	dev->pipeline_height = out_h;
	dev->pipeline_source_width = in_w;
	dev->pipeline_source_height = in_h;
	dev->pipeline_source_fps = fps;
	dev->pipeline_source_interlaced = interlaced;
	WRITE_ONCE(dev->pipeline_start_failed, false);
	dev->pipeline_attach_count++;
	return 0;

err_events:
	/*
	 * SET_VIC forks an encoder process before it replies.  Every failure after
	 * the doorbell therefore owes the card a STOP, including SET_VIC timeout
	 * itself; otherwise retries permanently consume the small spawn budget.
	 * Keep NOSG's normal successful stop/respawn loop unchanged--this is only
	 * the failed-start unwind.
	 */
	if (vic_fired) {
		int stop_ret = mz0380_stream_stop_all(dev, 2000);

		pr_warn("%s: stream start failed (%d) after SET_VIC; best-effort STOP_STREAMING ret=%d\n",
			dev->name, ret, stop_ret);
	}
	if (aic_newly_armed) {
		u32 aic_off[4] = { 0 };
		int aic_ret;

		aic_ret = mz0380_send_command(dev, MZ0380_CMD_SET_AIC_PARAMS,
					       aic_off, ARRAY_SIZE(aic_off),
					       NULL, 2000);
		pr_warn("%s: failed-start unwind: SET_AIC(on=0) ret=%d\n",
			dev->name, aic_ret);
		dev->aic_armed = false;
	}
	if (!mz0380_stream_nosg) {
		mz0380_dma_flush_events(dev);
		WRITE_ONCE(dev->frame_poison_active, false);
	}
	return ret;
}

void __mz0380_dma_stop(struct mz0380_dev *dev, bool verbose)
{
	int stop_ret = 0;

	/*
	 * M111: stop the poll-drain before anything touches buffer ownership -
	 * it copies out of the stream buffers and re-poisons them, exactly the
	 * race the extent watcher is frozen for below.
	 */
	mz0380_poll_drain_stop(dev);

	if (verbose)
		/* Freeze the diagnostic reader before changing buffer ownership. */
		mz0380_extent_watch_stop(dev);

	if (dev->dma_armed && READ_ONCE(dev->pipeline_running)) {
		if (mz0380_stop_on_streamoff || verbose) {
			stop_ret = mz0380_stream_stop_all(dev, 2000);
			pr_info("%s: final pipeline stop: STOP_STREAMING(all channels) ret=%d after %u userspace attachment(s) and %u SET_VIC spawn(s)\n",
				dev->name, stop_ret, dev->pipeline_attach_count,
				dev->encoder_spawns);
			WRITE_ONCE(dev->pipeline_running, false);
		} else {
			/*
			 * M156: leave the card streaming. Buffers stay mapped
			 * until unload, and teardown clears bus mastering
			 * before freeing them.
			 */
			pr_info("%s: stream stop: STOP_STREAMING SKIPPED (stop_on_streamoff=0) - the card is left streaming on purpose\n",
				dev->name);
		}
	}

	if (verbose) {
		/*
		 * STOP first, then cancel/drain completion work before inspecting
		 * memory.  This makes streamoff safe against a worker copying or
		 * re-poisoning while the diagnostic dump walks the same buffer.
		 */
		mz0380_dma_flush_events(dev);
		pr_info("%s: stream stop: EVENT[0x30]=%08x token[0x40]=%08x 0x44=%08x 0x48=%08x 0x4c=%08x enc[0x50]=%08x irq_total=%d frame_events=%d fifo_drops=%llu\n",
			dev->name,
			mz_mmio_read(dev, MZ0380_MB_EVENT),
			mz_mmio_read(dev, MZ0380_MB_FRAME_TOKEN),
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1),
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2),
			mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3),
			mz_mmio_read(dev, MZ0380_MB_ENC_STATUS),
			atomic_read(&dev->irq_count),
			atomic_read(&dev->irq_video_count),
			(unsigned long long)READ_ONCE(dev->frame_event_drops));
		mz0380_mst3367_output_diag(dev, "at stop");
		mz0380_stream_bufs_dump(dev, "stop");
		mz0380_raw_probe_bufs_dump(dev, "stop");
		mz0380_h264_bufs_dump(dev, "stop");
		if (mz0380_h264_probe)
			pr_info("%s: H.264 V4L2 totals: %llu delivered, %llu dropped for lack of a userspace buffer\n",
				dev->name,
				(unsigned long long)dev->h264_frames_delivered,
				(unsigned long long)dev->h264_frames_dropped);
		if (mz0380_raw_bank_probe)
			pr_info("%s: M209 raw V4L2 totals: events=%llu exact_frames=%llu bad_extents=%llu consecutive_exact=%u slots_seen=0x%02x\n",
				dev->name,
				(unsigned long long)dev->raw_probe_events,
				(unsigned long long)dev->raw_probe_full_frames,
				(unsigned long long)dev->raw_probe_bad_extents,
				dev->raw_probe_consecutive_full,
				dev->raw_probe_slots_seen);
		WRITE_ONCE(dev->frame_poison_active, false);
	}
}

/*
 * M245: leave the card idle for a reboot, kexec or suspend.
 *
 * With stop_on_streamoff=0 a persistent pipeline deliberately outlives every
 * STREAMOFF (M156) and is only stopped at unload. Nothing stopped it at
 * shutdown, so a warm reboot handed the next boot a card still DMAing into
 * IOVAs nothing had mapped yet. This is the unload path's STOP without its
 * diagnostics dump, and it is safe to call on a card that is not streaming.
 */
void mz0380_dma_quiesce(struct mz0380_dev *dev)
{
	WRITE_ONCE(dev->streaming, false);
	mz0380_signal_recovery_stop(dev);
	mz0380_poll_drain_stop(dev);
	if (dev->dma_armed && READ_ONCE(dev->pipeline_running)) {
		int ret = mz0380_stream_stop_all(dev, 2000);

		dev_info(&dev->pci->dev,
			 "quiesce: STOP_STREAMING(all channels) ret=%d\n", ret);
		WRITE_ONCE(dev->pipeline_running, false);
	}
	mz0380_dma_flush_events(dev);
}

void mz0380_dma_stop(struct mz0380_dev *dev)
{
	WRITE_ONCE(dev->streaming, false);
	mz0380_signal_recovery_stop(dev);
	__mz0380_dma_stop(dev, true);
}

/*
 * The true encoded-byte count exists in a card-side enc_stat structure, but
 * ep.ko discards it and exposes only packed slot counters at 0x40/0x44/0x48.
 * Infer a bounded length from buffer ownership instead: every slot is filled
 * with a repeated poison dword before the endpoint may use it, and completion
 * holds enc_stat until this worker copies/drops and re-poisons it.  Scan from
 * the end for the last changed dword.  A compressed final dword colliding with
 * the poison can undercount by four bytes (probability 2^-32); unlike the old
 * 4 MiB fallback, the result is bounded by an observed DMA write boundary.
 */
