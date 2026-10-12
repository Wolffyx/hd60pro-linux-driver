// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 encoder pipeline start/stop.
 *
 * The start sequence is the one the retail Windows driver sends (M82, M216):
 *
 *   STOP(all) . 1.9 s . SET_BUF 02/08/04(/03) . SET_VIC . SET_BUF 04(/03)
 *   . SET_AIC . SET_ENC main . SET_ENC sub . SET_PREVIEW . START
 *
 * Every value below was a module parameter while it was being found; the
 * constants are the values every hardware run since M216 used. SET_VIC forks
 * a fresh encoder process on the card, and the card survives only 8-18 of
 * those per power cycle, so nothing here is ever retried.
 */

#include "mz0380-dma-internal.h"

/* SET_VIC_PARAMS (op 0x29) fields - RE_FINDINGS.md M23, M71, M82, M104. */
#define MZ0380_VIC_FW_TINYVENC7		7	/* streams continuously (M158) */
#define MZ0380_VIC_IN_BT1120P		6
#define MZ0380_VIC_IN_BT1120I		7
#define MZ0380_VIC_COLOR_INFO		0x02010101	/* Windows, always */
#define MZ0380_VIC_BITSTREAMS		1	/* byte 28, must be >= 1 */
#define MZ0380_VIC_NOSG_COLOUR		0x80800000	/* neutral grey */

/* SET_ENC_PARAMS (op 0x2d): the Windows 1.1.195.0 values. */
#define MZ0380_ENC_MASK			0x3fff
#define MZ0380_ENC_QUALITY		24
#define MZ0380_ENC_SUB_GOP		30
#define MZ0380_ENC_SUB_BITRATE		4000000
#define MZ0380_ENC_SAFE_FPS		60
#define MZ0380_ENC_SAFE_GOP		60
#define MZ0380_ENC_SAFE_BITRATE		(4 * 1024 * 1024)

/* SET_PREVIEW_PARAMS (op 0x31). */
#define MZ0380_PREVIEW_MASK_BITMAP	0x01	/* M214: whole raw frames */
#define MZ0380_PREVIEW_DIE_EN		1	/* Windows sends 1, always */
#define MZ0380_PREVIEW_FAKE_FRAME_OFF	1	/* M128: no card NOSG splash */

/* M82: Windows waits ~1.9 s between its pre-STOP and the next SET_VIC. */
#define MZ0380_STOP_SETTLE_MS		1900
/*
 * M139: seeded into BAR0 0x40..0x4c before START. Load-bearing since: the 0x5
 * nibble makes an early BAR0+0x44 read name no valid window-1 slot.
 */
#define MZ0380_TOKEN_SEED		0xa5a5a5a5

#define MZ0380_SPAWN_WARN		8

/* A frame divisor of 1 can never fire (counter % 1 is never 1). */
static u32 mz0380_frame_divisor(void)
{
	u32 divisor = mz0380_h264_frame_divisor;

	return (divisor == 1 || divisor > U8_MAX) ? 2 : divisor;
}

/*
 * One encoder stream. Windows always configures both, main then sub (M82);
 * only main carries the V4L2 bitrate/GOP, sub keeps Windows' own numbers.
 */
static int mz0380_stream_configure_encoder(struct mz0380_dev *dev, u32 fps,
					   bool sub)
{
	u32 enc[10] = { 0 };
	u32 gop = sub ? MZ0380_ENC_SUB_GOP : dev->capture.enc_gop;
	u32 bitrate = sub ? MZ0380_ENC_SUB_BITRATE : dev->capture.enc_bitrate;
	/*
	 * M204: payload byte 20 is tinyvenc7's `skip`. Zero selects the
	 * all-frame schedule bitmap (M205: ~60 fps from 60 Hz input).
	 */
	u32 skip = mz0380_frame_divisor();

	if (!fps || fps > U8_MAX)
		fps = MZ0380_ENC_SAFE_FPS;
	if (!gop || gop > U8_MAX)
		gop = MZ0380_ENC_SAFE_GOP;
	if (bitrate < MZ0380_MIN_BITRATE || bitrate > MZ0380_MAX_BITRATE)
		bitrate = MZ0380_ENC_SAFE_BITRATE;

	enc[0] = MZ0380_ENC_MASK;
	enc[1] = (gop << 24) | (fps << 16) | ((sub ? 1u : 0u) << 8);
	enc[2] = MZ0380_ENC_QUALITY;
	enc[3] = bitrate;
	enc[4] = skip;		/* avg (byte 21) stays 0 */

	/* Match Windows' EVENT-wait path; timeout 0 would permit mailbox reuse. */
	return mz0380_send_command(dev, MZ0380_CMD_SET_ENC_PARAMS, enc,
				   ARRAY_SIZE(enc), NULL, 5000);
}

/*
 * op 0x31, which closes every Windows capture start. M128 decoded its payload:
 * [4..7]=mask [8]=ch [9]=fps [10]=skip [11]=avg [12]=die_en [14]=fake_frame_off.
 *
 * M218: mask bit 0 is what makes the card write whole raw frames instead of
 * 16-byte stubs (M214), so it follows the format the application chose.
 */
static int mz0380_stream_set_preview(struct mz0380_dev *dev, u32 fps)
{
	u32 post[5] = { 0 };

	post[0] = dev->deliver_raw ? MZ0380_PREVIEW_MASK_BITMAP : 0;
	post[1] = (fps & 0xff) << 8 | (mz0380_post_skip & 0xff) << 16;
	post[2] = MZ0380_PREVIEW_DIE_EN | MZ0380_PREVIEW_FAKE_FRAME_OFF << 16;

	return mz0380_send_command(dev, MZ0380_CMD_POST_PROC, post,
				   ARRAY_SIZE(post), NULL, 5000);
}

/*
 * STOP_STREAMING as Windows sends it: word[2] = 0xFFFFFFFF ("all channels"),
 * count 3 (M82).
 */
static int mz0380_stream_stop_all(struct mz0380_dev *dev,
				  unsigned int timeout_ms)
{
	u32 stop_all = 0xffffffffu;

	return mz0380_send_command(dev, MZ0380_CMD_STOP_STREAMING,
				   &stop_all, 1, NULL, timeout_ms);
}

/*
 * M33: SET_AIC_PARAMS on=1 is what releases the card's audio gate, and the
 * encoder will not complete video frames without it - it is part of the video
 * start, not only of audio.
 *
 * M246: with audio enabled, cmd+4 is the audio index and cmd+5 the channel
 * count (Windows: index | chs << 8 | 16 << 16), which makes the card launch its
 * capture program. Each SET_AIC starts another one, so a video-only load keeps
 * the layout every video run was validated with.
 */
static int mz0380_stream_set_aic(struct mz0380_dev *dev)
{
	u32 word0 = dev->audio_capable ?
		((mz0380_aic_channels & 0xff) << 8 |
		 (u32)(mz0380_aic_bits & 0xffff) << 16) :
		((mz0380_aic_channels & 0xff) |
		 (u32)(mz0380_aic_channels == 1 ? 1 : 0) << 8 |
		 (u32)(mz0380_aic_bits & 0xffff) << 16);
	u32 aic[4] = {
		word0,
		mz0380_aic_freq,
		(mz0380_aic_period_frames & 0xffff) |
		(u32)(mz0380_aic_periods & 0xffff) << 16,
		1u | (u32)(mz0380_aic_int_mode & 0xff) << 8,	/* on=1 */
	};

	return mz0380_send_command(dev, MZ0380_CMD_SET_AIC_PARAMS, aic,
				   ARRAY_SIZE(aic), NULL, 2000);
}

static void mz0380_stream_aic_off(struct mz0380_dev *dev)
{
	u32 aic_off[4] = { 0 };
	int ret;

	ret = mz0380_send_command(dev, MZ0380_CMD_SET_AIC_PARAMS, aic_off,
				  ARRAY_SIZE(aic_off), NULL, 2000);
	dev_warn(&dev->pci->dev, "failed-start unwind: SET_AIC(on=0) ret=%d\n",
		 ret);
	dev->aic_armed = false;
}

/*
 * SET_VIC_PARAMS. The mailbox puts the opcode at struct[0..3] and params[i] at
 * struct[4 + 4i]; field map verified against video_capture_mgr (M23, M71):
 *   [4]=ch [5]=fps [6]=fw [7]=input format (6 BT1120p, 7 BT1120i)
 *   [8..11]=output w/h [12]=output format [16..19]=color_info
 *   [24..27]=VIC input w/h [28]=bitstream_num [31]=is_nosg [33]=fast_kill
 *   [34]=frame interrupt enable [36..39]=no-signal colour
 */
static int mz0380_stream_set_vic(struct mz0380_dev *dev, u32 fps,
				 u32 in_w, u32 in_h)
{
	bool interlaced = dev->capture.source_interlaced;
	u32 params[9] = { 0 };

	params[0] = fps << 8 | MZ0380_VIC_FW_TINYVENC7 << 16 |
		    (interlaced ? MZ0380_VIC_IN_BT1120I :
				  MZ0380_VIC_IN_BT1120P) << 24;
	params[1] = (dev->capture.height & 0xffff) << 16 |
		    (dev->capture.width & 0xffff);
	params[3] = MZ0380_VIC_COLOR_INFO;
	params[5] = (in_h & 0xffff) << 16 | (in_w & 0xffff);
	params[6] = MZ0380_VIC_BITSTREAMS;
	params[8] = MZ0380_VIC_NOSG_COLOUR;

	/*
	 * M169: counted on the fire, not the result - a SET_VIC that times out
	 * may still have forked an encoder, and a budget that only counted
	 * successes would under-report exactly when the card is in trouble.
	 */
	dev->encoder_spawns++;
	if (dev->encoder_spawns == MZ0380_SPAWN_WARN)
		dev_warn(&dev->pci->dev,
			 "%u encoder spawns since load - the card has wedged in the 8-18 range before (recovery is a mains-off cold boot). The budget is per POWER CYCLE: scripts/mz0380-spawns.sh has the running total\n",
			 dev->encoder_spawns);
	return mz0380_send_command(dev, MZ0380_CMD_SET_VIC_PARAMS, params,
				   ARRAY_SIZE(params), NULL, 2000);
}

/* SET_ENC main + sub, then SET_PREVIEW: the Windows tail, which is the kick. */
static int mz0380_stream_encoder_tail(struct mz0380_dev *dev, u32 fps)
{
	int ret;

	ret = mz0380_stream_configure_encoder(dev, fps, false);
	if (!ret)
		ret = mz0380_stream_configure_encoder(dev, fps, true);
	if (!ret)
		ret = mz0380_stream_set_preview(dev, fps);
	return ret;
}

/*
 * M139: seed the frame-token registers before the card can touch them; its
 * store_channel_done() rewrites one nibble per channel, so the seed makes a
 * report visible and keeps an early window-1 read from naming a valid slot.
 */
static void mz0380_stream_seed_tokens(struct mz0380_dev *dev)
{
	mz_mmio_write(dev, MZ0380_MB_EVT_PAYLOAD0, MZ0380_TOKEN_SEED);
	mz_mmio_write(dev, MZ0380_MB_EVT_PAYLOAD1, MZ0380_TOKEN_SEED);
	mz_mmio_write(dev, MZ0380_MB_EVT_PAYLOAD2, MZ0380_TOKEN_SEED);
	mz_mmio_write(dev, MZ0380_MB_EVT_PAYLOAD3, MZ0380_TOKEN_SEED);
	wmb();
}

static void mz0380_pipeline_record(struct mz0380_dev *dev, u32 fps,
				   u32 in_w, u32 in_h)
{
	WRITE_ONCE(dev->pipeline_running, true);
	WRITE_ONCE(dev->pipeline_reconfigure_pending, false);
	WRITE_ONCE(dev->pipeline_start_failed, false);
	dev->pipeline_width = dev->capture.width;
	dev->pipeline_height = dev->capture.height;
	dev->pipeline_source_width = in_w;
	dev->pipeline_source_height = in_h;
	dev->pipeline_source_fps = fps;
	dev->pipeline_source_interlaced = dev->capture.source_interlaced;
	dev->pipeline_raw = dev->deliver_raw;
	dev->pipeline_enc_bitrate = dev->capture.enc_bitrate;
	dev->pipeline_enc_gop = dev->capture.enc_gop;
	dev->pipeline_attach_count++;
}

int mz0380_dma_start(struct mz0380_dev *dev)
{
	u32 in_w = dev->capture.source_width ?: dev->detected_timings.bt.width;
	u32 in_h = dev->capture.source_height ?: dev->detected_timings.bt.height;
	u32 fps = dev->capture.source_fps ?:
		  mz0380_source_fps(&dev->detected_timings) ?: 60;
	bool aic_newly_armed = false;
	bool vic_fired = false;
	int ret;

	if (!dev->dma_armed)
		return -ENODEV;
	if (!in_w)
		in_w = dev->capture.width;
	if (!in_h)
		in_h = dev->capture.height;

	/*
	 * M82: Windows precedes every reconfiguration with a fire-and-forget
	 * STOP and ~1.9 s of settling, then registers its buffers before the
	 * SET_VIC (they are registered when its pin opens).
	 */
	mz0380_stream_stop_all(dev, 0);
	msleep(MZ0380_STOP_SETTLE_MS);
	ret = mz0380_dma_program_bufs(dev);
	if (ret)
		goto err_buffers;

	vic_fired = true;
	ret = mz0380_stream_set_vic(dev, fps, in_w, in_h);
	if (ret)
		goto err;

	/*
	 * SET_VIC creates a fresh tinyvenc process and card-side channel state
	 * may be rebuilt at that boundary, so re-register the encoded window
	 * (it does not wake the encoder, so cannot race an encoded DMA).
	 */
	ret = mz0380_h264_program_bufs(dev);
	if (ret)
		goto err;

	/* Accept completions only once every slot has its poison baseline. */
	mz0380_frame_events_start(dev);

	/*
	 * M40: declare every bitstream slot free before the encoder starts; a
	 * stale 1 left by a previous stream makes it skip every frame.
	 */
	mz_mmio_write(dev, MZ0380_MB_ENC_STATUS, MZ0380_MB_ENC_STAT_FREE);
	wmb();

	if (mz0380_aic_every_frame || !dev->aic_armed) {
		bool was_armed = dev->aic_armed;

		ret = mz0380_stream_set_aic(dev);
		if (ret)
			goto err;
		dev->aic_armed = true;
		aic_newly_armed = !was_armed;
	}

	/*
	 * M130/M133: the source is locked by now, so pick the receiver's CSC
	 * mode from the colour space it is actually sending. The output diag
	 * runs first because its read of BANK2 0x48 is the one that works, and
	 * it caches the value the CSC choice then uses.
	 */
	mz0380_mst3367_output_diag(dev, "before START");
	mz0380_mst3367_apply_csc_mode(dev);

	ret = mz0380_stream_encoder_tail(dev, fps);
	if (ret)
		goto err;

	mz0380_stream_seed_tokens(dev);

	/*
	 * op 0x06 is fire-and-forget: its ep.ko handler only wakes tinyvenc and
	 * posts no mailbox completion (M22). Do not wait for the encoder after
	 * it either - this runs inside STREAMON, and userspace cannot recycle
	 * buffers until it returns.
	 */
	mz0380_send_command(dev, MZ0380_CMD_START_STREAMING, NULL, 0, NULL, 0);

	mz0380_pipeline_record(dev, fps, in_w, in_h);
	dev_info(&dev->pci->dev,
		 "encoder started: %ux%u%c@%u -> %s, encoder spawn %u since load\n",
		 in_w, in_h, dev->capture.source_interlaced ? 'i' : 'p', fps,
		 dev->deliver_raw ? "raw" : "H.264", dev->encoder_spawns);
	return 0;

err_buffers:
	dev_warn(&dev->pci->dev, "SET_BUF failed (%d) - frames cannot flow\n",
		 ret);
err:
	/*
	 * SET_VIC forks an encoder process before it replies. Every failure
	 * after the doorbell therefore owes the card a STOP, including a
	 * SET_VIC timeout; otherwise retries consume the small spawn budget.
	 */
	if (vic_fired)
		dev_warn(&dev->pci->dev,
			 "stream start failed (%d) after SET_VIC; best-effort STOP_STREAMING ret=%d\n",
			 ret, mz0380_stream_stop_all(dev, 2000));
	if (aic_newly_armed)
		mz0380_stream_aic_off(dev);
	mz0380_dma_flush_events(dev);
	return ret;
}

/*
 * M245: leave the card idle for a reboot, kexec or suspend. A persistent
 * pipeline outlives every STREAMOFF and is otherwise only stopped at unload,
 * so a warm reboot would hand the next boot a card still DMAing into IOVAs
 * nothing had mapped yet. Safe on a card that is not streaming.
 */
void mz0380_dma_quiesce(struct mz0380_dev *dev)
{
	WRITE_ONCE(dev->streaming, false);
	mz0380_signal_recovery_stop(dev);
	if (dev->dma_armed && READ_ONCE(dev->pipeline_running)) {
		int ret = mz0380_stream_stop_all(dev, 2000);

		dev_info(&dev->pci->dev,
			 "quiesce: STOP_STREAMING(all channels) ret=%d\n", ret);
		WRITE_ONCE(dev->pipeline_running, false);
	}
	mz0380_dma_flush_events(dev);
}

/* The final stop: unload, a pipeline replacement, or a non-persistent STREAMOFF. */
void mz0380_dma_stop(struct mz0380_dev *dev)
{
	WRITE_ONCE(dev->streaming, false);
	mz0380_signal_recovery_stop(dev);
	if (dev->dma_armed && READ_ONCE(dev->pipeline_running)) {
		int ret = mz0380_stream_stop_all(dev, 2000);

		dev_info(&dev->pci->dev,
			 "encoder stopped (ret=%d) after %u attachment(s); %u encoder spawn(s) since load, %llu H.264 / %llu raw frames delivered\n",
			 ret, dev->pipeline_attach_count, dev->encoder_spawns,
			 (unsigned long long)dev->h264_frames_delivered,
			 (unsigned long long)dev->raw_frames_delivered);
		WRITE_ONCE(dev->pipeline_running, false);
	}
	mz0380_dma_flush_events(dev);
}
