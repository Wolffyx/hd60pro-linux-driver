// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 V4L2 capture node: formats, inputs, controls and registration.
 */

#include <linux/string.h>

#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>

#include "mz0380.h"

/* M171: buffers vb2's read() fileio path uses; reported by G/S_PARM. */
#define MZ0380_READ_BUFFERS	2

static const struct mz0380_mode mz0380_modes[] = {
	{ 1920, 1080 },
	{ 1280,  720 },
	{  720,  480 },
	{  720,  576 },
};

/*
 * M240/M241: the three raw layouts first - the card writes I420, and YV12 and
 * NV12 are free re-orderings of it - then H.264. With raw_deliver=0 H.264 is
 * listed first, which is what an application taking index 0 gets.
 */
static const u32 mz0380_raw_formats[] = {
	V4L2_PIX_FMT_YUV420,
	V4L2_PIX_FMT_NV12,
	V4L2_PIX_FMT_YVU420,
};

const struct mz0380_mode *mz0380_find_mode(u32 width, u32 height)
{
	const struct mz0380_mode *best = &mz0380_modes[0];
	u64 best_delta = U64_MAX;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mz0380_modes); i++) {
		u64 delta = abs((int)mz0380_modes[i].width - (int)width) +
			    abs((int)mz0380_modes[i].height - (int)height);

		if (delta < best_delta) {
			best = &mz0380_modes[i];
			best_delta = delta;
		}
	}
	return best;
}

bool mz0380_is_raw_fourcc(u32 fourcc)
{
	return fourcc == V4L2_PIX_FMT_YUV420 ||
	       fourcc == V4L2_PIX_FMT_YVU420 ||
	       fourcc == V4L2_PIX_FMT_NV12;
}

static bool mz0380_is_supported_fourcc(u32 fourcc)
{
	return mz0380_is_raw_fourcc(fourcc) || fourcc == V4L2_PIX_FMT_H264;
}

/*
 * The frame rate actually delivered: the source rate, divided by the H.264
 * schedule's divisor when there is one (M204/M205). A non-reduced fraction
 * such as 2/60 is valid and preserves odd source rates.
 */
static struct v4l2_fract mz0380_reported_interval(struct mz0380_dev *dev)
{
	u32 divisor = mz0380_h264_frame_divisor;
	struct v4l2_fract interval = {
		.denominator = dev->capture.source_fps ?: 60,
	};

	if (divisor == 1 || divisor > U8_MAX)
		divisor = 2;
	interval.numerator = divisor ?: 1;
	return interval;
}

/*
 * M245: everything below answers "what would the node deliver for THIS
 * request" from its arguments, never by writing the request into the device:
 * the drain reads those fields while it copies, so TRY_FMT must change nothing.
 */
static u32 mz0380_sizeimage_for(bool raw, u32 width, u32 height)
{
	if (raw)
		return mz0380_raw_frame_bytes_for(width, height);
	/* An access unit, plus SPS/PPS prepended to a parameter-set-free IDR. */
	return MZ0380_H264_SET_BUF_SIZE - 4096 + MZ0380_H264_PARAMETER_SETS_MAX;
}

u32 mz0380_current_sizeimage(struct mz0380_dev *dev)
{
	return mz0380_sizeimage_for(dev->deliver_raw, dev->capture.width,
				    dev->capture.height);
}

static u32 mz0380_pixelformat_for(bool raw, u32 raw_fourcc)
{
	if (!raw)
		return V4L2_PIX_FMT_H264;
	return raw_fourcc ?: V4L2_PIX_FMT_YUV420;
}

u32 mz0380_current_pixelformat(struct mz0380_dev *dev)
{
	return mz0380_pixelformat_for(dev->deliver_raw, dev->raw_fourcc);
}

static void mz0380_fill_pix_format_for(struct mz0380_dev *dev,
				       struct v4l2_pix_format *pix, bool raw,
				       u32 raw_fourcc, u32 width, u32 height)
{
	memset(pix, 0, sizeof(*pix));
	pix->pixelformat = mz0380_pixelformat_for(raw, raw_fourcc);
	pix->width = width;
	pix->height = height;
	pix->bytesperline = raw ? width : 0;
	pix->field = mz0380_current_field(dev);
	pix->sizeimage = mz0380_sizeimage_for(raw, width, height);
	/* M245: SD modes are BT.601, not BT.709. */
	if (height <= 576) {
		pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	} else {
		pix->colorspace = V4L2_COLORSPACE_REC709;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
	}
	/* The H.264 bitstream carries its own VUI; only raw is relabelled. */
	pix->quantization = raw && mz0380_raw_full_range ?
			    V4L2_QUANTIZATION_FULL_RANGE :
			    V4L2_QUANTIZATION_LIM_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_709;
}

static void mz0380_fill_pix_format(struct mz0380_dev *dev,
				   struct v4l2_pix_format *pix)
{
	mz0380_fill_pix_format_for(dev, pix, dev->deliver_raw, dev->raw_fourcc,
				   dev->capture.width, dev->capture.height);
}

static int mz0380_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct mz0380_dev *dev = video_drvdata(file);

	strscpy(cap->driver, "mz0380", sizeof(cap->driver));
	strscpy(cap->card, mz0380_boards[dev->board].name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s",
		 pci_name(dev->pci));
	return 0;
}

static int mz0380_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	unsigned int raw_count = ARRAY_SIZE(mz0380_raw_formats);
	unsigned int h264_index = mz0380_raw_deliver ? raw_count : 0;
	unsigned int raw_index;

	if (f->index > raw_count)
		return -EINVAL;

	/* The core fills in the description and the COMPRESSED flag. */
	if (f->index == h264_index) {
		f->pixelformat = V4L2_PIX_FMT_H264;
		return 0;
	}
	raw_index = mz0380_raw_deliver ? f->index : f->index - 1;
	f->pixelformat = mz0380_raw_formats[raw_index];
	return 0;
}

static int mz0380_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct mz0380_dev *dev = video_drvdata(file);

	mz0380_fill_pix_format(dev, &f->fmt.pix);
	return 0;
}

/*
 * Resolve a TRY_FMT/S_FMT request to what the node would deliver, without
 * touching the device.
 *
 * M223: there is no scaler, so the source geometry wins whenever it is known.
 * M248: except over timings the application set itself - S_DV_TIMINGS sizes
 * the format (M172), and a TRY/S_FMT that snapped back to the source made
 * TRY_FMT(G_FMT) != G_FMT. A mismatch with the source is refused at STREAMON.
 *
 * M247: a pixelformat the node does not offer keeps the current format. It
 * used to mean "H.264" - an application asking for YUYV silently switched the
 * node to the encoded stream and marked the encoder for replacement, which
 * spent an encoder spawn on the next capture.
 */
static const struct mz0380_mode *
mz0380_resolve_fmt(struct mz0380_dev *dev, const struct v4l2_pix_format *pix,
		   bool *raw, u32 *raw_fourcc)
{
	u32 width = pix->width, height = pix->height;

	if (dev->timings_set_by_user) {
		width = dev->set_timings.bt.width;
		height = dev->set_timings.bt.height;
	} else if (dev->capture.source_width && dev->capture.source_height) {
		width = dev->capture.source_width;
		height = dev->capture.source_height;
	}

	*raw = dev->deliver_raw;
	*raw_fourcc = dev->raw_fourcc;
	if (mz0380_is_supported_fourcc(pix->pixelformat)) {
		*raw = mz0380_is_raw_fourcc(pix->pixelformat);
		if (*raw)
			*raw_fourcc = pix->pixelformat;
	}

	return mz0380_find_mode(width, height);
}

static int mz0380_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct mz0380_dev *dev = video_drvdata(file);
	const struct mz0380_mode *mode;
	u32 raw_fourcc;
	bool raw;

	mode = mz0380_resolve_fmt(dev, &f->fmt.pix, &raw, &raw_fourcc);
	mz0380_fill_pix_format_for(dev, &f->fmt.pix, raw, raw_fourcc,
				   mode->width, mode->height);
	return 0;
}

static int mz0380_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct mz0380_dev *dev = video_drvdata(file);
	const struct mz0380_mode *mode;
	u32 raw_fourcc;
	bool raw, changed;

	mode = mz0380_resolve_fmt(dev, &f->fmt.pix, &raw, &raw_fourcc);

	/*
	 * M245: no format change of any kind while buffers exist - a second
	 * process could otherwise re-order or resize frames under buffers the
	 * owner negotiated differently.
	 */
	changed = raw != dev->deliver_raw ||
		  (raw && raw_fourcc != dev->raw_fourcc) ||
		  mode->width != dev->capture.width ||
		  mode->height != dev->capture.height;
	if (changed && vb2_is_busy(&dev->vb_queue))
		return -EBUSY;

	/*
	 * A raw <-> H.264 switch needs the card told (preview mask bit 0, M221),
	 * which only a pipeline start does; STREAMON compares against what the
	 * running pipeline was started with (mz0380_pipeline_config_stale), so
	 * switching there and back again costs nothing.
	 */
	WRITE_ONCE(dev->deliver_raw, raw);
	if (raw)
		WRITE_ONCE(dev->raw_fourcc, raw_fourcc);
	dev->capture.width = mode->width;
	dev->capture.height = mode->height;
	mz0380_fill_pix_format(dev, &f->fmt.pix);
	return 0;
}

static int mz0380_enum_framesizes(struct file *file, void *priv,
				  struct v4l2_frmsizeenum *fsize)
{
	struct mz0380_dev *dev = video_drvdata(file);

	/*
	 * M222: there is no scaler, so the only size that can work is the one
	 * the node is set up for, which follows the source (M223).
	 */
	if (!mz0380_is_supported_fourcc(fsize->pixel_format) ||
	    fsize->index != 0)
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = dev->capture.width;
	fsize->discrete.height = dev->capture.height;
	return 0;
}

static int mz0380_enum_frameintervals(struct file *file, void *priv,
				      struct v4l2_frmivalenum *fival)
{
	struct mz0380_dev *dev = video_drvdata(file);
	const struct mz0380_mode *mode;

	if (!mz0380_is_supported_fourcc(fival->pixel_format) ||
	    fival->index != 0)
		return -EINVAL;
	mode = mz0380_find_mode(fival->width, fival->height);
	if (mode->width != fival->width || mode->height != fival->height)
		return -EINVAL;
	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete = mz0380_reported_interval(dev);
	return 0;
}

/*
 * M247: a persistent pipeline outlives its application, and with nothing
 * streaming the receiver monitor is stopped - so the cached lock went stale
 * the moment the cable was pulled. The detached drain still stamps every
 * frame the card produces, which says whether the source is there at no
 * receiver cost.
 */
bool mz0380_detached_source_lost(struct mz0380_dev *dev)
{
	unsigned long last = READ_ONCE(dev->last_h264_frame_stamp);

	return !READ_ONCE(dev->streaming) && READ_ONCE(dev->pipeline_running) &&
	       (!last || time_after_eq(jiffies, last +
				msecs_to_jiffies(MZ0380_HOTPLUG_STALL_MS)));
}

/*
 * Is a source there? While a capture or pipeline runs the cached state is
 * used (receiver traffic during a capture can perturb it, M74); otherwise one
 * lock-byte read answers it.
 */
static bool mz0380_source_present(struct mz0380_dev *dev)
{
	bool locked;

	if (READ_ONCE(dev->streaming) || READ_ONCE(dev->pipeline_running))
		return READ_ONCE(dev->signal_locked) &&
		       !mz0380_detached_source_lost(dev);
	if (mz0380_signal_idle_lock(dev, &locked))
		return READ_ONCE(dev->signal_locked);
	return locked;
}

static int mz0380_enum_input(struct file *file, void *priv,
			     struct v4l2_input *inp)
{
	struct mz0380_dev *dev = video_drvdata(file);

	/* M245: one input - the HD60 Pro has a single HDMI connector. */
	if (inp->index != 0)
		return -EINVAL;

	memset(inp, 0, sizeof(*inp));
	strscpy(inp->name, "HDMI", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->capabilities = V4L2_IN_CAP_DV_TIMINGS;
	if (!mz0380_source_present(dev))
		inp->status = V4L2_IN_ST_NO_SIGNAL;
	return 0;
}

static int mz0380_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

/* The HDMI route is selected once at probe (mz0380_card_setup). */
static int mz0380_s_input(struct file *file, void *priv, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static int mz0380_parm(struct file *file, void *priv,
		       struct v4l2_streamparm *a)
{
	struct mz0380_dev *dev = video_drvdata(file);

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	/* The rate is the source's; S_PARM can only be told what it is. */
	memset(&a->parm, 0, sizeof(a->parm));
	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.timeperframe = mz0380_reported_interval(dev);
	/* M171: read() is supported, so say how many buffers it uses. */
	a->parm.capture.readbuffers = MZ0380_READ_BUFFERS;
	return 0;
}

/* M170: SOURCE_CHANGE is how an application learns to renegotiate. */
static int mz0380_subscribe_event(struct v4l2_fh *fh,
				  const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subscribe_event(fh, sub);
	default:
		return -EINVAL;
	}
}

/* `v4l2-ctl --log-status`: what to paste into a bug report. No card traffic. */
static int mz0380_log_status(struct file *file, void *priv)
{
	struct mz0380_dev *dev = video_drvdata(file);
	u32 fourcc = mz0380_current_pixelformat(dev);
	struct v4l2_device *v4l2 = &dev->v4l2_dev;

	v4l2_info(v4l2, "card handshake: %s, firmware %u.%u\n",
		  mz0380_fw_state_name(dev->fw_state), dev->fw_version_major,
		  dev->fw_version_minor);
	if (dev->signal_locked && !mz0380_detached_source_lost(dev))
		v4l2_print_dv_timings(dev->name, "HDMI source: ",
				      &dev->detected_timings, true);
	else
		v4l2_info(v4l2, "HDMI source: no signal\n");
	v4l2_info(v4l2, "format: %p4cc %ux%u, %s\n", &fourcc,
		  dev->capture.width, dev->capture.height,
		  READ_ONCE(dev->streaming) ? "streaming" : "idle");
	v4l2_info(v4l2, "encoder: %s%s, %u attachment(s), %u spawn(s) since load (the card survives 8-18 per power cycle)\n",
		  READ_ONCE(dev->pipeline_running) ? "running" : "stopped",
		  READ_ONCE(dev->no_signal_active) ? ", NO SIGNAL shown" : "",
		  dev->pipeline_attach_count, dev->encoder_spawns);
	v4l2_info(v4l2, "frames: %llu raw, %llu H.264 delivered; %llu raw / %llu H.264 dropped for lack of a buffer\n",
		  (unsigned long long)dev->raw_frames_delivered,
		  (unsigned long long)dev->h264_frames_delivered,
		  (unsigned long long)dev->raw_frames_dropped,
		  (unsigned long long)dev->h264_frames_dropped);
	v4l2_ctrl_handler_log_status(&dev->ctrl_handler, dev->name);
	return 0;
}

static int mz0380_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mz0380_dev *dev =
		container_of(ctrl->handler, struct mz0380_dev, ctrl_handler);

	if (ctrl->id != V4L2_CID_DV_RX_POWER_PRESENT)
		return -EINVAL;
	/*
	 * M245: the receiver's lock, as the closest measurable stand-in - no
	 * +5V or cable-detect bit is known on the MST3367.
	 */
	ctrl->val = mz0380_source_present(dev) ? 1 : 0;
	return 0;
}

/*
 * M245: bitrate and GOP reach the encoder, in SET_ENC_PARAMS at pipeline
 * start. They are grabbed while streaming (the running encoder cannot re-read
 * them); a change between sessions is picked up at the next STREAMON.
 */
static int mz0380_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mz0380_dev *dev =
		container_of(ctrl->handler, struct mz0380_dev, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		dev->capture.enc_bitrate = ctrl->val;
		return 0;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		dev->capture.enc_gop = ctrl->val;
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops mz0380_ctrl_ops = {
	.g_volatile_ctrl = mz0380_g_volatile_ctrl,
	.s_ctrl = mz0380_s_ctrl,
};

void mz0380_encoder_ctrls_grab(struct mz0380_dev *dev, bool grabbed)
{
	if (!dev->ctrl_handler_initialized)
		return;
	if (dev->bitrate_ctrl)
		v4l2_ctrl_grab(dev->bitrate_ctrl, grabbed);
	if (dev->gop_ctrl)
		v4l2_ctrl_grab(dev->gop_ctrl, grabbed);
}

/*
 * Does the running pipeline need replacing for this attachment? A raw/H.264
 * switch does (the card's preview mask follows the format), and so does a
 * bitrate or GOP change when H.264 is what gets delivered. Comparing against
 * what the pipeline was started with means toggling a setting and back again
 * costs no encoder spawn, and neither does an encoder setting nobody sees.
 */
bool mz0380_pipeline_config_stale(struct mz0380_dev *dev)
{
	if (dev->deliver_raw != dev->pipeline_raw)
		return true;
	if (dev->deliver_raw)
		return false;
	return dev->capture.enc_bitrate != dev->pipeline_enc_bitrate ||
	       dev->capture.enc_gop != dev->pipeline_enc_gop;
}

/*
 * M247: an S_DV_TIMINGS holds until the last file handle closes. It used to
 * hold for the module's lifetime, so after v4l2-compliance set 1080i60 a
 * progressive source was labelled interlaced until the next reload.
 */
static int mz0380_release(struct file *file)
{
	struct mz0380_dev *dev = video_drvdata(file);

	if (v4l2_fh_is_singular_file(file)) {
		mutex_lock(&dev->lock);
		if (dev->timings_set_by_user) {
			dev->timings_set_by_user = false;
			dev->set_timings = dev->signal_locked ?
					   dev->detected_timings :
					   mz0380_default_timings;
			/* S_DV_TIMINGS resized the format too; put it back. */
			if (!vb2_is_busy(&dev->vb_queue)) {
				const struct mz0380_mode *mode =
					mz0380_find_mode(dev->set_timings.bt.width,
							 dev->set_timings.bt.height);

				dev->capture.width = mode->width;
				dev->capture.height = mode->height;
			}
		}
		mutex_unlock(&dev->lock);
	}
	return vb2_fop_release(file);
}

static const struct v4l2_ioctl_ops mz0380_video_ioctl_ops = {
	.vidioc_querycap = mz0380_querycap,
	.vidioc_enum_fmt_vid_cap = mz0380_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = mz0380_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = mz0380_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = mz0380_s_fmt_vid_cap,
	.vidioc_enum_framesizes = mz0380_enum_framesizes,
	.vidioc_enum_frameintervals = mz0380_enum_frameintervals,
	.vidioc_enum_input = mz0380_enum_input,
	.vidioc_g_input = mz0380_g_input,
	.vidioc_s_input = mz0380_s_input,
	.vidioc_g_parm = mz0380_parm,
	.vidioc_s_parm = mz0380_parm,
	.vidioc_log_status = mz0380_log_status,
	.vidioc_subscribe_event = mz0380_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,

	.vidioc_query_dv_timings = mz0380_query_dv_timings,
	.vidioc_g_dv_timings = mz0380_g_dv_timings,
	.vidioc_s_dv_timings = mz0380_s_dv_timings,
	.vidioc_enum_dv_timings = mz0380_enum_dv_timings,
	.vidioc_dv_timings_cap = mz0380_dv_timings_cap,
};

static const struct v4l2_file_operations mz0380_video_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = mz0380_release,
	.unlocked_ioctl = video_ioctl2,
	.read = vb2_fop_read,
	.mmap = vb2_fop_mmap,
	.poll = vb2_fop_poll,
};

static int mz0380_ctrls_init(struct mz0380_dev *dev)
{
	struct v4l2_ctrl *power;

	v4l2_ctrl_handler_init(&dev->ctrl_handler, 3);
	dev->ctrl_handler.lock = &dev->ctrl_lock;
	dev->ctrl_handler_initialized = true;

	dev->bitrate_ctrl =
		v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_BITRATE,
				  MZ0380_MIN_BITRATE, MZ0380_MAX_BITRATE,
				  256 * 1024, MZ0380_ENC_DEFAULT_BITRATE);
	dev->gop_ctrl =
		v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_GOP_SIZE,
				  MZ0380_MIN_GOP, MZ0380_ENC_MAX_GOP, 1,
				  MZ0380_ENC_DEFAULT_GOP);
	/* One input, so the bitmask has one bit. */
	power = v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
				  V4L2_CID_DV_RX_POWER_PRESENT, 0, 1, 0, 0);
	if (power)
		power->flags |= V4L2_CTRL_FLAG_VOLATILE;

	if (dev->ctrl_handler.error) {
		int err = dev->ctrl_handler.error;

		v4l2_ctrl_handler_free(&dev->ctrl_handler);
		dev->bitrate_ctrl = NULL;
		dev->gop_ctrl = NULL;
		dev->ctrl_handler_initialized = false;
		return err;
	}
	return 0;
}

void mz0380_capture_state_init(struct mz0380_dev *dev)
{
	/*
	 * M171: G_DV_TIMINGS needs a valid answer before anything has been
	 * detected or set. M248: and one S_DV_TIMINGS takes back - QUERY is
	 * what says "nothing here", with ENOLINK.
	 */
	dev->set_timings = mz0380_default_timings;
	dev->detected_timings = mz0380_no_signal;
	dev->capture.width = 1920;
	dev->capture.height = 1080;
	dev->capture.source_width = 1920;
	dev->capture.source_height = 1080;
	dev->capture.source_fps = 60;
	dev->capture.source_interlaced = false;
	dev->capture.enc_bitrate = MZ0380_ENC_DEFAULT_BITRATE;
	dev->capture.enc_gop = MZ0380_ENC_DEFAULT_GOP;
	dev->deliver_raw = mz0380_raw_deliver;
}

static int mz0380_vb2_init(struct mz0380_dev *dev)
{
	struct vb2_queue *q = &dev->vb_queue;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_READ | VB2_DMABUF | VB2_USERPTR;
	q->drv_priv = dev;
	q->buf_struct_size = sizeof(struct mz0380_vb_buffer);
	q->ops = &mz0380_qops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	/*
	 * M245: one mutex for the queue and every other ioctl, so S_FMT's
	 * vb2_is_busy() test and REQBUFS on another handle cannot interleave.
	 */
	q->lock = &dev->lock;
	q->gfp_flags = GFP_KERNEL;
	q->min_queued_buffers = 2;
	q->dev = &dev->pci->dev;

	return vb2_queue_init(q);
}

/*
 * M245: the device struct lives until the last file handle closes. PCI
 * unbind, hot removal and AER can remove the device under an open handle, so
 * the struct is freed from the v4l2_device release callback instead.
 */
static void mz0380_v4l2_release(struct v4l2_device *v4l2_dev)
{
	struct mz0380_dev *dev =
		container_of(v4l2_dev, struct mz0380_dev, v4l2_dev);

	if (dev->ctrl_handler_initialized)
		v4l2_ctrl_handler_free(&dev->ctrl_handler);
	kfree(dev);
}

/* Drop remove's reference: frees now, or at the last close of the node. */
void mz0380_dev_put(struct mz0380_dev *dev)
{
	if (dev->v4l2_dev.release)
		v4l2_device_put(&dev->v4l2_dev);
	else
		kfree(dev);
}

static void mz0380_ctrls_free(struct mz0380_dev *dev)
{
	if (!dev->ctrl_handler_initialized)
		return;
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	dev->bitrate_ctrl = NULL;
	dev->gop_ctrl = NULL;
	dev->ctrl_handler_initialized = false;
}

int mz0380_video_register(struct mz0380_dev *dev)
{
	struct video_device *vdev = &dev->vdev;
	int err;

	err = v4l2_device_register(&dev->pci->dev, &dev->v4l2_dev);
	if (err)
		return err;
	dev->v4l2_registered = true;

	err = mz0380_ctrls_init(dev);
	if (err)
		goto fail_v4l2;
	dev->v4l2_dev.ctrl_handler = &dev->ctrl_handler;

	err = mz0380_vb2_init(dev);
	if (err)
		goto fail_ctrls;

	/* M240: the node is named after the device, not one of its formats. */
	strscpy(vdev->name, "HD60 Pro HDMI capture", sizeof(vdev->name));
	vdev->fops = &mz0380_video_fops;
	vdev->ioctl_ops = &mz0380_video_ioctl_ops;
	vdev->lock = &dev->lock;
	vdev->queue = &dev->vb_queue;
	vdev->release = video_device_release_empty;
	vdev->v4l2_dev = &dev->v4l2_dev;
	vdev->ctrl_handler = &dev->ctrl_handler;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			    V4L2_CAP_READWRITE;
	vdev->dev_parent = &dev->pci->dev;
	video_set_drvdata(vdev, dev);

	err = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (err)
		goto fail_ctrls;

	dev->video_registered = true;
	/* From here the struct is refcounted - see mz0380_v4l2_release. */
	dev->v4l2_dev.release = mz0380_v4l2_release;

	dev_info(&dev->pci->dev, "registered %s\n",
		 video_device_node_name(vdev));
	return 0;

fail_ctrls:
	mz0380_ctrls_free(dev);
fail_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);
	dev->v4l2_registered = false;
	return err;
}

void mz0380_video_unregister(struct mz0380_dev *dev)
{
	if (dev->video_registered) {
		/*
		 * M245: stops a running stream and releases the queue NOW,
		 * while the hardware is still mapped, so a handle that stays
		 * open past remove finds no queue left to stop on close.
		 */
		vb2_video_unregister_device(&dev->vdev);
		dev->video_registered = false;
	}

	if (dev->v4l2_registered) {
		v4l2_device_unregister(&dev->v4l2_dev);
		dev->v4l2_registered = false;
	}

	/*
	 * Open handles can still take control events, so a refcounted device
	 * frees its handler in mz0380_v4l2_release instead.
	 */
	if (!dev->v4l2_dev.release)
		mz0380_ctrls_free(dev);
}
