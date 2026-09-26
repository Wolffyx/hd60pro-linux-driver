// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Probe-safe V4L2 scaffolding for MZ0380 based capture cards.
 */

#include <linux/limits.h>
#include <linux/string.h>

#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>

#include "mz0380-internal.h"

/* M171: buffers vb2's read() fileio path uses; reported by G/S_PARM. */
#define MZ0380_READ_BUFFERS         2
#define MZ0380_SIZEIMAGE_MIN        (256 * 1024)
#define MZ0380_SIZEIMAGE_MAX        (4 * 1024 * 1024)

enum mz0380_record_mode {
	MZ0380_RECORD_MODE_VBR = 0,
	MZ0380_RECORD_MODE_CBR = 1,
	MZ0380_RECORD_MODE_HBR = 2,
};

static const struct mz0380_mode mz0380_modes[] = {
	{ 1920, 1080 },
	{ 1280,  720 },
	{  720,  480 },
	{  720,  576 },
};

static const struct v4l2_fract mz0380_ntsc_frame_intervals[] = {
	{ .numerator = 1, .denominator = 60 },
	{ .numerator = 1, .denominator = 30 },
	{ .numerator = 1, .denominator = 15 },
	{ .numerator = 2, .denominator = 15 },
	{ .numerator = 4, .denominator = 15 },
};

static const struct v4l2_fract mz0380_pal_frame_intervals[] = {
	{ .numerator = 1, .denominator = 50 },
	{ .numerator = 1, .denominator = 25 },
	{ .numerator = 2, .denominator = 25 },
	{ .numerator = 4, .denominator = 25 },
	{ .numerator = 8, .denominator = 25 },
};

static const char * const mz0380_input_names[MZ0380_INPUT_COUNT] = {
	"HDMI",
	"DVI-D",
	"COMPONENTS (YCBCR)",
	"DVI-A (RGB)",
	"SDI",
};

/* Names for /proc; the property itself is a BAR5 experiment, not a control. */
static const char * const mz0380_record_mode_qmenu[] = {
	"VBR",
	"CBR",
	"HBR",
};

static const struct video_device mz0380_video_template = {
	.name = "mz0380-h264",
};

const char *mz0380_record_mode_name(u32 mode)
{
	if (mode >= ARRAY_SIZE(mz0380_record_mode_qmenu))
		return "unknown";

	return mz0380_record_mode_qmenu[mode];
}

const char *mz0380_input_name(u32 input)
{
	if (input >= ARRAY_SIZE(mz0380_input_names))
		return "unknown";

	return mz0380_input_names[input];
}

static const struct v4l2_fract *
mz0380_interval_table(const struct mz0380_capture_state *capture,
		      unsigned int *count)
{
	if (capture->width == 720 && capture->height == 576) {
		*count = ARRAY_SIZE(mz0380_pal_frame_intervals);
		return mz0380_pal_frame_intervals;
	}

	if (capture->width == 720 && capture->height == 480) {
		*count = ARRAY_SIZE(mz0380_ntsc_frame_intervals);
		return mz0380_ntsc_frame_intervals;
	}

	*count = ARRAY_SIZE(mz0380_ntsc_frame_intervals) +
		 ARRAY_SIZE(mz0380_pal_frame_intervals);
	return NULL;
}

static const struct v4l2_fract *
mz0380_interval_by_index(const struct mz0380_capture_state *capture,
			 unsigned int index)
{
	unsigned int count;
	const struct v4l2_fract *table;

	table = mz0380_interval_table(capture, &count);
	if (table)
		return index < count ? &table[index] : NULL;

	if (index < ARRAY_SIZE(mz0380_ntsc_frame_intervals))
		return &mz0380_ntsc_frame_intervals[index];

	index -= ARRAY_SIZE(mz0380_ntsc_frame_intervals);
	if (index < ARRAY_SIZE(mz0380_pal_frame_intervals))
		return &mz0380_pal_frame_intervals[index];

	return NULL;
}

static bool mz0380_interval_equal(const struct v4l2_fract *a,
				  const struct v4l2_fract *b)
{
	return a->numerator == b->numerator &&
	       a->denominator == b->denominator;
}

static const struct v4l2_fract *
mz0380_default_interval_for_size(u32 width, u32 height)
{
	if (width == 720 && height == 576)
		return &mz0380_pal_frame_intervals[0];

	return &mz0380_ntsc_frame_intervals[0];
}

/*
 * Non-zero tinyvenc7 skip values emit one access unit per input-frame
 * divisor. M204's zero mode selects an all-frame schedule bitmap instead,
 * and M205 hardware-validates that mode at the source cadence.
 * Keep capture.timeperframe describing the source (SET_VIC still needs that),
 * but report the selected encoded cadence to V4L2. A non-reduced fraction
 * such as 2/60 is valid and preserves odd source rates.
 */
static struct v4l2_fract mz0380_reported_interval(struct mz0380_dev *dev)
{
	struct v4l2_fract interval = dev->capture.timeperframe;
	u32 divisor = mz0380_h264_frame_divisor;
	u32 source_fps = dev->capture.source_fps;

	if (!mz0380_h264_probe)
		return interval;

	if (divisor == 1 || divisor > U8_MAX)
		divisor = 2;
	if (!source_fps)
		source_fps = 60;

	interval.numerator = divisor ?: 1;
	interval.denominator = source_fps;
	return interval;
}

const struct mz0380_mode *
mz0380_find_mode(u32 width, u32 height)
{
	unsigned int i;
	const struct mz0380_mode *best = &mz0380_modes[0];
	u64 best_delta = U64_MAX;

	for (i = 0; i < ARRAY_SIZE(mz0380_modes); i++) {
		u64 dw = abs((int)mz0380_modes[i].width - (int)width);
		u64 dh = abs((int)mz0380_modes[i].height - (int)height);
		u64 delta = dw + dh;

		if (delta < best_delta) {
			best = &mz0380_modes[i];
			best_delta = delta;
		}
	}

	return best;
}

const struct v4l2_fract *
mz0380_find_interval(u32 width, u32 height, const struct v4l2_fract *wanted)
{
	unsigned int i;
	unsigned int count = 0;
	const struct v4l2_fract *table;

	if (!wanted || !wanted->numerator || !wanted->denominator)
		return mz0380_default_interval_for_size(width, height);

	table = mz0380_interval_table(&(struct mz0380_capture_state) {
		.width = width,
		.height = height,
	}, &count);
	if (table) {
		for (i = 0; i < count; i++) {
			if (mz0380_interval_equal(wanted, &table[i]))
				return &table[i];
		}
		return mz0380_default_interval_for_size(width, height);
	}

	for (i = 0; i < ARRAY_SIZE(mz0380_ntsc_frame_intervals); i++) {
		if (mz0380_interval_equal(wanted, &mz0380_ntsc_frame_intervals[i]))
			return &mz0380_ntsc_frame_intervals[i];
	}

	for (i = 0; i < ARRAY_SIZE(mz0380_pal_frame_intervals); i++) {
		if (mz0380_interval_equal(wanted, &mz0380_pal_frame_intervals[i]))
			return &mz0380_pal_frame_intervals[i];
	}

	return mz0380_default_interval_for_size(width, height);
}

/*
 * Everything below answers "what format would the node deliver for THIS
 * request", taking the request as arguments rather than reading it back out of
 * the device.
 *
 * M245: TRY_FMT used to answer by writing the request INTO the device -
 * deliver_raw, raw_fourcc and capture - calling the helpers, and restoring the
 * old values afterwards. The drain worker reads those same fields with no lock
 * while it copies a frame, so a TRY_FMT from any process during a capture could
 * make one frame take the wrong chroma layout (correct luma, green/magenta
 * cast) or the H.264 branch. The window is microseconds, which is exactly what
 * makes it look like an occasional hardware fault. V4L2 is explicit that
 * TRY_FMT changes nothing; now it cannot.
 */
u32 mz0380_sizeimage_for(struct mz0380_dev *dev, bool raw, u32 width,
			 u32 height)
{
	u32 size;

	if (mz0380_raw_bank_probe)
		return MZ0380_RAW_PROBE_FRAME_SIZE;
	if (raw)		/* M217/M218, before h264_probe */
		return mz0380_raw_frame_bytes_for(width, height); /* M221 */
	if (mz0380_h264_probe)
		return MZ0380_H264_SET_BUF_SIZE - 4096;
	if (mz0380_stream_nosg)
		return MZ0380_NOSG_NV12_SIZEIMAGE;

	size = clamp_t(u32, DIV_ROUND_UP(dev->capture.enc_bitrate, 8),
		       MZ0380_SIZEIMAGE_MIN, MZ0380_SIZEIMAGE_MAX);
	/*
	 * M111: the poll-drain delivers RAW frames - buf0 receives exactly
	 * width*height*3/2 of 4:2:0, the card's own splash, and the encoder
	 * never produces a bitstream for it - so the plane must hold one.
	 */
	if (mz0380_poll_drain_ms)
		size = max(width * height * 3 / 2, size);

	/*
	 * M175: a declared frame expectation is also a plane requirement.
	 *
	 * expect_frame_bytes exists because the card does not always write
	 * source-sized frames - tinyvenc8 writes 960x540, and fw=6 switches the
	 * card's output format to YUY2, which at 1080p is 4147200 bytes against
	 * this plane's 3110400. Without this the drain would measure a complete
	 * frame and then reject it as "does not fit vb2 plane". Bounded by the
	 * DMA buffer, since nothing larger can arrive anyway.
	 */
	if (mz0380_expect_frame_bytes)
		size = max_t(u32, size,
			     min_t(u32, mz0380_expect_frame_bytes,
				   MZ0380_STREAM_BUF_SIZE));

	return size;
}

u32 mz0380_current_sizeimage(struct mz0380_dev *dev)
{
	return mz0380_sizeimage_for(dev, dev->deliver_raw, dev->capture.width,
				    dev->capture.height);
}

/*
 * M168: ONE place decides the advertised fourcc, because three places used to
 * and they had drifted apart.
 *
 * The poll-drain payload is planar I420 (M130, confirmed visually AND by
 * correlation), so V4L2_PIX_FMT_YUV420. The nosg fake-frame path keeps NV12:
 * its layout was never confirmed either way, so correcting it would be a guess
 * rather than a measurement. It is a diagnostic path and defaults off.
 *
 * M217/M218: raw delivery runs WITH h264_probe - the encoder has to keep
 * running for the card to produce raw at all (M210b) - so it is tested first.
 */
static u32 mz0380_pixelformat_for(bool raw, u32 raw_fourcc)
{
	if (mz0380_raw_bank_probe)
		return V4L2_PIX_FMT_YUV420;
	if (raw)
		return raw_fourcc ? raw_fourcc : V4L2_PIX_FMT_YUV420;
	if (mz0380_h264_probe)
		return V4L2_PIX_FMT_H264;
	if (mz0380_stream_nosg)
		return V4L2_PIX_FMT_NV12;
	if (mz0380_poll_drain_ms)
		return V4L2_PIX_FMT_YUV420;
	return V4L2_PIX_FMT_H264;
}

u32 mz0380_current_pixelformat(struct mz0380_dev *dev)
{
	if (!dev)
		return mz0380_pixelformat_for(false, 0);
	return mz0380_pixelformat_for(dev->deliver_raw, dev->raw_fourcc);
}

static void mz0380_fill_pix_format_for(struct mz0380_dev *dev,
				       struct v4l2_pix_format *pix, bool raw,
				       u32 raw_fourcc, u32 width, u32 height)
{
	memset(pix, 0, sizeof(*pix));

	pix->pixelformat = mz0380_pixelformat_for(raw, raw_fourcc);

	if (mz0380_stream_nosg) {
		pix->width = MZ0380_NOSG_NV12_WIDTH;
		pix->height = MZ0380_NOSG_NV12_HEIGHT;
		pix->bytesperline = MZ0380_NOSG_NV12_WIDTH;
	} else if (mz0380_raw_bank_probe || raw || mz0380_poll_drain_ms) {
		/* M111: real geometry, raw payload. */
		pix->width = width;
		pix->height = height;
		pix->bytesperline = width;
	} else {
		pix->width = width;
		pix->height = height;
		pix->bytesperline = 0;
	}

	pix->field = mz0380_current_field(dev);
	pix->sizeimage = mz0380_sizeimage_for(dev, raw, width, height);
	/*
	 * M245: SD modes are BT.601, not BT.709. The mode table offers 720x480
	 * and 720x576, and HDMI sources send those with 601 colorimetry.
	 */
	if (pix->height && pix->height <= 576) {
		pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	} else {
		pix->colorspace = V4L2_COLORSPACE_REC709;
		pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
	}
	/*
	 * M234 (RETRACTED as a default, kept as an operator switch): the
	 * H.264 bitstream carries its own VUI, so only raw is relabelled. All
	 * three raw layouts carry the same bytes, so all three get the same
	 * label - M245 found NV12 and YV12 still claiming limited range.
	 */
	if (mz0380_raw_full_range && mz0380_is_raw_fourcc(pix->pixelformat))
		pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	else
		pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
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
	/* must equal vdev.device_caps exactly or the v4l2 core WARNs */
	cap->device_caps = dev->vdev.device_caps;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

/*
 * M218: enumerate every format this load can actually deliver.
 *
 * Until now this returned exactly one entry, chosen by module parameters, and
 * S_FMT was a no-op - so selecting raw meant reloading the driver with
 * raw_deliver=1. An application cannot discover that, which is the practical
 * difference between "a driver that can capture raw" and "a camera".
 *
 * Both are offered whenever the raw banks exist. The raw banks are still a
 * probe-time allocation (mz0380_dma_setup), so raw_capable is what decides
 * whether the second entry appears; the raw_deliver parameter now only decides
 * which of the two is listed FIRST, i.e. which one an application that takes
 * index 0 without looking will get.
 */
/*
 * M223: there is no scaler, so a requested geometry is a request, not a choice.
 *
 * S_FMT used to accept whatever mz0380_find_mode() matched, including sizes the
 * card cannot produce. STREAMON then refused - correctly, since the card would
 * write a 1080p frame into a 720p buffer - and the node stayed refusing,
 * because nothing ever put the geometry back. An application that asked for
 * 1280x720 once was stuck, and switching pixelformat did not help because the
 * pixelformat was never what was wrong.
 *
 * V4L2 requires TRY_FMT and S_FMT to return the format that will actually be
 * used. For a device whose input geometry is fixed by whatever the source is
 * sending, that means answering with the source geometry instead of accepting a
 * size that can only fail at STREAMON.
 */
static void mz0380_clamp_to_source(struct mz0380_dev *dev, u32 *w, u32 *h)
{
	if (!dev->capture.source_width || !dev->capture.source_height)
		return;
	*w = dev->capture.source_width;
	*h = dev->capture.source_height;
}

static int mz0380_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct mz0380_dev *dev = video_drvdata(file);
	bool raw_first;

	if (mz0380_stream_nosg || mz0380_raw_bank_probe ||
	    mz0380_poll_drain_ms) {
		/* Diagnostic paths deliver one fixed format and nothing else. */
		if (f->index != 0)
			return -EINVAL;
		f->pixelformat = mz0380_current_pixelformat(dev);
		f->flags = 0;
		if (mz0380_stream_nosg)
			strscpy(f->description, "NV12 raw (fake-frame path)",
				sizeof(f->description));
		else if (mz0380_raw_bank_probe)
			strscpy(f->description,
				"I420 raw (M209 op02/op08 discriminator)",
				sizeof(f->description));
		else
			strscpy(f->description, "I420 raw (poll-drain path)",
				sizeof(f->description));
		return 0;
	}

	if (!dev->raw_capable) {
		if (f->index != 0)
			return -EINVAL;
		f->pixelformat = V4L2_PIX_FMT_H264;
		f->flags = V4L2_FMT_FLAG_COMPRESSED;
		strscpy(f->description, "H.264 bytestream",
			sizeof(f->description));
		return 0;
	}

	/*
	 * M241: three raw layouts, then H.264.
	 *
	 * The card writes I420. YV12 is the same data with the chroma planes
	 * exchanged and NV12 with them interleaved, so both come free of any
	 * pixel conversion - and both are formats the Windows driver offers
	 * (M227) and that many applications expect. The re-ordering happens in
	 * mz0380_raw_copy_frame during the copy that already had to occur.
	 */
	if (f->index > 3)
		return -EINVAL;

	raw_first = mz0380_raw_deliver;
	if (f->index == (raw_first ? 3 : 0)) {
		f->pixelformat = V4L2_PIX_FMT_H264;
		f->flags = V4L2_FMT_FLAG_COMPRESSED;
		strscpy(f->description, "H.264 bytestream",
			sizeof(f->description));
		return 0;
	}

	switch (raw_first ? f->index : f->index - 1) {
	case 0:
		f->pixelformat = V4L2_PIX_FMT_YUV420;
		strscpy(f->description, "I420 raw (uncompressed)",
			sizeof(f->description));
		break;
	case 1:
		f->pixelformat = V4L2_PIX_FMT_NV12;
		strscpy(f->description, "NV12 raw (uncompressed)",
			sizeof(f->description));
		break;
	default:
		f->pixelformat = V4L2_PIX_FMT_YVU420;
		strscpy(f->description, "YV12 raw (uncompressed)",
			sizeof(f->description));
		break;
	}
	f->flags = 0;
	return 0;
}

/* M241: the three layouts the raw path can emit from one I420 source. */
bool mz0380_is_raw_fourcc(u32 fourcc)
{
	return fourcc == V4L2_PIX_FMT_YUV420 ||
	       fourcc == V4L2_PIX_FMT_YVU420 ||
	       fourcc == V4L2_PIX_FMT_NV12;
}

static int mz0380_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct mz0380_dev *dev = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	mz0380_fill_pix_format(dev, &f->fmt.pix);
	return 0;
}

/*
 * Resolve a TRY_FMT/S_FMT request to the format the node would deliver,
 * without touching the device: the source geometry wins (M223, there is no
 * scaler), and the pixelformat decides raw versus H.264 when the raw banks
 * exist (M218).
 */
static const struct mz0380_mode *
mz0380_resolve_fmt(struct mz0380_dev *dev, const struct v4l2_pix_format *pix,
		   bool *raw, u32 *raw_fourcc)
{
	u32 width = pix->width, height = pix->height;

	mz0380_clamp_to_source(dev, &width, &height);

	*raw = dev->deliver_raw;
	*raw_fourcc = dev->raw_fourcc;
	if (dev->raw_capable) {
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

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (mz0380_stream_nosg) {
		/* fake-frame path: one fixed raw mode */
		mz0380_fill_pix_format(dev, &f->fmt.pix);
		return 0;
	}

	/* M245: computed, never written into dev - see mz0380_sizeimage_for. */
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
	const struct v4l2_fract *interval;
	u32 raw_fourcc;
	bool raw, changed;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (mz0380_stream_nosg) {
		/* fake-frame path: one fixed raw mode */
		mz0380_fill_pix_format(dev, &f->fmt.pix);
		return 0;
	}

	mode = mz0380_resolve_fmt(dev, &f->fmt.pix, &raw, &raw_fourcc);

	/*
	 * M245: no format change of any kind while buffers exist.
	 *
	 * Only the raw/H.264 switch used to be refused. A switch between raw
	 * layouts was committed on purpose mid-stream (M241), and a geometry
	 * change was never checked at all - so a SECOND process could S_FMT a
	 * node another application was streaming from. The layout switch then
	 * re-ordered every later frame into buffers the owner had negotiated as
	 * a different layout, and a geometry change shrank the frame size the
	 * drain copies (mz0380_raw_frame_bytes reads capture.width) under
	 * buffers sized for the old one. V4L2 says -EBUSY, and applications
	 * already handle it.
	 */
	changed = raw != dev->deliver_raw ||
		  (raw && raw_fourcc != dev->raw_fourcc) ||
		  mode->width != dev->capture.width ||
		  mode->height != dev->capture.height;
	if (changed && vb2_is_busy(&dev->vb_queue))
		return -EBUSY;

	if (raw != dev->deliver_raw) {
		dev->deliver_raw = raw;

		/*
		 * M221: the card has to be told, and a persistent pipeline will
		 * not tell it.
		 *
		 * post_mask bit 0 - what makes the card write whole frames
		 * instead of 16-byte stubs - is sent by mz0380_stream_post_proc()
		 * during stream start. Under persistent_h264 a second STREAMON on
		 * a running pipeline attaches VB2 and nothing else, so switching
		 * H.264 -> I420 after streaming once would leave the card in stub
		 * mode. Flag the pipeline for replacement so the next attachment
		 * restarts it and re-sends the command.
		 */
		if (READ_ONCE(dev->pipeline_running))
			WRITE_ONCE(dev->pipeline_reconfigure_pending, true);
		dev_info(&dev->pci->dev, "delivery format set to %s%s\n",
			 raw ? "raw (uncompressed)" : "H.264",
			 READ_ONCE(dev->pipeline_running) ?
				"; encoder pipeline will be replaced at the next attachment" :
				"");
	}
	/*
	 * M241: a switch BETWEEN raw layouts needs no pipeline work - the card
	 * keeps writing I420 and only the copy re-orders.
	 */
	if (raw)
		dev->raw_fourcc = raw_fourcc;

	interval = mz0380_find_interval(mode->width, mode->height,
					&dev->capture.timeperframe);
	dev->capture.width = mode->width;
	dev->capture.height = mode->height;
	dev->capture.timeperframe = *interval;
	mz0380_fill_pix_format(dev, &f->fmt.pix);

	return 0;
}

static int mz0380_enum_framesizes(struct file *file, void *priv,
				  struct v4l2_frmsizeenum *fsize)
{
	struct mz0380_dev *dev = video_drvdata(file);

	/*
	 * M244: accept every format ENUM_FMT offers, which since M241 is three
	 * raw layouts and not one.
	 *
	 * NV12 and YV12 were added to the format list and not here, so the
	 * node advertised two formats whose frame sizes could not be
	 * enumerated. v4l2-compliance failed it five times over as a scaling
	 * defect, which is what a format with no frame sizes looks like from
	 * the outside. mz0380_is_raw_fourcc is the same predicate the rest of
	 * the raw path uses, so the two lists cannot drift apart again.
	 */
	if (fsize->pixel_format != mz0380_current_pixelformat(dev) &&
	    !(dev->raw_capable &&
	      (mz0380_is_raw_fourcc(fsize->pixel_format) ||
	       fsize->pixel_format == V4L2_PIX_FMT_H264)))
		return -EINVAL;

	if (mz0380_stream_nosg) {
		if (fsize->index != 0)
			return -EINVAL;
		fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fsize->discrete.width = MZ0380_NOSG_NV12_WIDTH;
		fsize->discrete.height = MZ0380_NOSG_NV12_HEIGHT;
		return 0;
	}

	/*
	 * M222: there is no scaler, so the only size that can ever work is the
	 * one the source is sending.
	 *
	 * This used to enumerate the whole mode table, which reads as a menu of
	 * supported resolutions and is not one. Selecting any entry other than
	 * the live source geometry gets as far as STREAMON and is then refused
	 * by mz0380_vb2_start_streaming() - "the card would write 3110400 bytes
	 * into a buffer described as 1382400" - which in an application looks
	 * like the driver falling over rather than like a mode it should never
	 * have been offered.
	 *
	 * When the source geometry is not known yet, fall back to the table so
	 * that probing an idle node still describes what the card can do.
	 */
	if (dev->capture.width && dev->capture.height) {
		if (fsize->index != 0)
			return -EINVAL;
		fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fsize->discrete.width = dev->capture.width;
		fsize->discrete.height = dev->capture.height;
		return 0;
	}

	if (fsize->index >= ARRAY_SIZE(mz0380_modes))
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = mz0380_modes[fsize->index].width;
	fsize->discrete.height = mz0380_modes[fsize->index].height;

	return 0;
}

static int mz0380_enum_frameintervals(struct file *file, void *priv,
				      struct v4l2_frmivalenum *fival)
{
	struct mz0380_dev *dev = video_drvdata(file);
	const struct mz0380_mode *mode;
	const struct v4l2_fract *interval;
	struct mz0380_capture_state capture = { 0 };

	/* M244: the same three raw layouts ENUM_FMT and ENUM_FRAMESIZES list. */
	if (fival->pixel_format != mz0380_current_pixelformat(dev) &&
	    !(dev->raw_capable &&
	      (mz0380_is_raw_fourcc(fival->pixel_format) ||
	       fival->pixel_format == V4L2_PIX_FMT_H264)))
		return -EINVAL;

	mode = mz0380_find_mode(fival->width, fival->height);
	if (mode->width != fival->width || mode->height != fival->height)
		return -EINVAL;
	if (mz0380_h264_probe) {
		if (fival->index != 0)
			return -EINVAL;
		fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
		fival->discrete = mz0380_reported_interval(dev);
		return 0;
	}

	capture.width = fival->width;
	capture.height = fival->height;
	interval = mz0380_interval_by_index(&capture, fival->index);
	if (!interval)
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete = *interval;

	return 0;
}

/*
 * M170: an HDMI capture input that could not say whether anything was plugged
 * into it.
 *
 * `capabilities` was left at 0 even though this driver implements the whole
 * DV-timings ioctl set. V4L2_IN_CAP_DV_TIMINGS is how an application learns
 * that QUERY_DV_TIMINGS is worth calling at all, so without it a well-behaved
 * client never asks - and asking is the only way to get the geometry off this
 * card, since the card never pushes format to the host (M6).
 *
 * `status` was left at 0, which in V4L2 means "no problems". So the node
 * asserted a healthy source unconditionally, including with the cable out. The
 * standard bit for this is V4L2_IN_ST_NO_SIGNAL and it is precisely the thing
 * this project has spent whole sessions determining by hand.
 *
 * The live query is skipped while streaming: it reads the MST3367 over the
 * mailbox I2C proxy, and M74 is the reminder that a watch running alongside a
 * capture can perturb the capture. Streaming already knows what it locked, so
 * the cached flag is both cheaper and correct there.
 *
 * Only the selected input can be measured - the receiver serves one at a time -
 * so the others report NO_SIGNAL rather than claiming a clean bill of health
 * for a path nothing has looked at.
 */
static int mz0380_enum_input(struct file *file, void *priv,
			     struct v4l2_input *inp)
{
	struct mz0380_dev *dev = video_drvdata(file);
	unsigned int index = inp->index;
	bool locked = false;

	/*
	 * M245: one input. The HD60 Pro has a single HDMI connector, and the
	 * other four names are the SDK's generic front ends. Listing them put
	 * an input menu in every application whose other entries route the VIC
	 * to a front end this board does not have (see mz0380_card_setup).
	 */
	if (index != 0)
		return -EINVAL;

	if (index == dev->capture.input) {
		if (READ_ONCE(dev->streaming) ||
		    READ_ONCE(dev->pipeline_running)) {
			locked = dev->signal_locked;
		} else {
			struct v4l2_dv_timings live;

			locked = !mz0380_query_signal(dev, &live);
		}
	}

	memset(inp, 0, sizeof(*inp));
	inp->index = index;
	strscpy(inp->name, mz0380_input_names[index], sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->capabilities = V4L2_IN_CAP_DV_TIMINGS;
	if (!locked)
		inp->status = V4L2_IN_ST_NO_SIGNAL;

	return 0;
}

static int mz0380_g_input(struct file *file, void *priv, unsigned int *i)
{
	struct mz0380_dev *dev = video_drvdata(file);

	*i = dev->capture.input;
	return 0;
}

static int mz0380_s_input(struct file *file, void *priv, unsigned int i)
{
	struct mz0380_dev *dev = video_drvdata(file);

	if (i != 0)
		return -EINVAL;
	return mz0380_request_input_select(dev, i, "v4l2");
}

static int mz0380_g_parm(struct file *file, void *priv,
			 struct v4l2_streamparm *a)
{
	struct mz0380_dev *dev = video_drvdata(file);

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memset(&a->parm, 0, sizeof(a->parm));
	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.timeperframe = mz0380_reported_interval(dev);
	/*
	 * M171: the node advertises V4L2_CAP_READWRITE, so read() is a
	 * supported I/O method and V4L2 requires this field to say how many
	 * buffers it uses. The memset left it 0, which v4l2-compliance reports
	 * as "!cap->readbuffers" - a device claiming read() support while
	 * declaring it has no buffers to read into.
	 */
	a->parm.capture.readbuffers = MZ0380_READ_BUFFERS;

	return 0;
}

static int mz0380_s_parm(struct file *file, void *priv,
			 struct v4l2_streamparm *a)
{
	struct mz0380_dev *dev = video_drvdata(file);
	const struct v4l2_fract *interval;

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (!mz0380_h264_probe) {
		interval = mz0380_find_interval(dev->capture.width,
						dev->capture.height,
						&a->parm.capture.timeperframe);
		dev->capture.timeperframe = *interval;
	}

	memset(&a->parm, 0, sizeof(a->parm));
	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.timeperframe = mz0380_reported_interval(dev);
	a->parm.capture.readbuffers = MZ0380_READ_BUFFERS;

	return 0;
}

/*
 * M170: V4L2_EVENT_SOURCE_CHANGE was unreachable from both ends.
 *
 * mz0380_signal_event() builds and queues one, and the ops table pointed
 * vidioc_subscribe_event straight at v4l2_ctrl_subscribe_event, which accepts
 * V4L2_EVENT_CTRL and nothing else. So SUBSCRIBE_EVENT(SOURCE_CHANGE) returned
 * -EINVAL to every application, and the event - had anything emitted one - had
 * no subscriber it could reach. The feature was listed as implemented in
 * PLAN.md and was not reachable by any client.
 */
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

static int mz0380_log_status(struct file *file, void *priv)
{
	struct mz0380_dev *dev = video_drvdata(file);

	v4l2_ctrl_handler_log_status(&dev->ctrl_handler, dev->name);
	return 0;
}

static int mz0380_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mz0380_dev *dev =
		container_of(ctrl->handler, struct mz0380_dev, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_DV_RX_POWER_PRESENT: {
		bool locked;

		/*
		 * M245: the receiver's lock, as the closest measurable stand-in.
		 * No +5V or cable-detect bit is known on the MST3367 - R55 reads
		 * 0x83/0x03 with nothing plugged in and with a source that is
		 * powered but not transmitting alike - so this reads 1 when the
		 * receiver is locked to a source and 0 otherwise.
		 *
		 * Live when nothing is capturing: the cached flag is only as new
		 * as the last query, and after a load or rebind it read 0 with a
		 * locked source until something else asked. One R55 read is
		 * cheap. While a stream or pipeline runs the cached state is
		 * used instead, the same rule ENUM_INPUT follows (M74: receiver
		 * traffic during a capture can perturb it).
		 */
		if (READ_ONCE(dev->streaming) || READ_ONCE(dev->pipeline_running) ||
		    mz0380_mst3367_read_lock(dev, &locked, false))
			locked = READ_ONCE(dev->signal_locked);
		ctrl->val = locked ? 1 : 0;
		return 0;
	}
	default:
		return -EINVAL;
	}
}

/*
 * M245: the controls that exist are the ones that do something.
 *
 * Bitrate, quality, GOP, B-frames and record mode used to be written straight
 * into BAR5 registers whose meaning on this card was never established (the
 * property-experiment path), and SET_ENC_PARAMS then ignored them in favour of
 * the Windows constants. Brightness, contrast, hue, saturation, sharpness,
 * profile, level and peak bitrate were stored and never read. v4l2-compliance
 * exercises every control, so each run wrote arbitrary values into BAR5.
 *
 * What is left is bitrate and GOP, which now reach the encoder: they are sent
 * in SET_ENC_PARAMS for the main stream when a pipeline starts. They are
 * grabbed while streaming, because the running encoder cannot re-read them.
 * Between sessions a persistent pipeline is still running with the old values,
 * so a change there schedules its replacement at the next attachment - the same
 * mechanism a raw/H.264 switch uses, and at the same cost of one encoder spawn.
 */
static int mz0380_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mz0380_dev *dev =
		container_of(ctrl->handler, struct mz0380_dev, ctrl_handler);
	u32 *field;

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		field = &dev->capture.enc_bitrate;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		field = &dev->capture.enc_gop;
		break;
	default:
		return -EINVAL;
	}

	if (*field != (u32)ctrl->val && READ_ONCE(dev->pipeline_running) &&
	    !READ_ONCE(dev->pipeline_reconfigure_pending)) {
		WRITE_ONCE(dev->pipeline_reconfigure_pending, true);
		dev_info(&dev->pci->dev,
			 "%s changed; the encoder pipeline will be replaced at the next attachment (one encoder spawn)\n",
			 ctrl->name);
	}
	*field = ctrl->val;
	return 0;
}

static const struct v4l2_ctrl_ops mz0380_ctrl_ops = {
	.g_volatile_ctrl = mz0380_g_volatile_ctrl,
	.s_ctrl = mz0380_s_ctrl,
};

/*
 * Hold the encoder controls for a streaming session. Safe to call from any
 * sleeping context; does nothing before the node is registered.
 */
void mz0380_encoder_ctrls_grab(struct mz0380_dev *dev, bool grabbed)
{
	if (!dev->ctrl_handler_initialized)
		return;
	if (dev->bitrate_ctrl)
		v4l2_ctrl_grab(dev->bitrate_ctrl, grabbed);
	if (dev->gop_ctrl)
		v4l2_ctrl_grab(dev->gop_ctrl, grabbed);
}

/* ===== ioctl table & file ops ======================================== */

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
	.vidioc_g_parm = mz0380_g_parm,
	.vidioc_s_parm = mz0380_s_parm,
	.vidioc_log_status = mz0380_log_status,
	.vidioc_subscribe_event = mz0380_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	/* streaming */
	.vidioc_reqbufs       = vb2_ioctl_reqbufs,
	.vidioc_create_bufs   = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf   = vb2_ioctl_prepare_buf,
	.vidioc_querybuf      = vb2_ioctl_querybuf,
	.vidioc_qbuf          = vb2_ioctl_qbuf,
	.vidioc_dqbuf         = vb2_ioctl_dqbuf,
	.vidioc_expbuf        = vb2_ioctl_expbuf,
	.vidioc_streamon      = vb2_ioctl_streamon,
	.vidioc_streamoff     = vb2_ioctl_streamoff,

	/* HDMI signal */
	.vidioc_query_dv_timings  = mz0380_query_dv_timings,
	.vidioc_g_dv_timings      = mz0380_g_dv_timings,
	.vidioc_s_dv_timings      = mz0380_s_dv_timings,
	.vidioc_enum_dv_timings   = mz0380_enum_dv_timings,
	.vidioc_dv_timings_cap    = mz0380_dv_timings_cap,
};

static const struct v4l2_file_operations mz0380_video_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
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
	 * M171: give G_DV_TIMINGS a valid answer before anything has been
	 * detected or set. A zeroed struct has type 0, which is not a valid
	 * v4l2_dv_timings type; mz0380_no_signal carries the right type with
	 * empty timings, which is the honest "nothing here yet".
	 */
	dev->set_timings = mz0380_no_signal;
	dev->capture.width = 1920;
	dev->capture.height = 1080;
	dev->capture.timeperframe = mz0380_ntsc_frame_intervals[0];
	dev->capture.source_width = 1920;
	dev->capture.source_height = 1080;
	dev->capture.source_fps = 60;
	dev->capture.source_interlaced = false;
	dev->capture.input = 0;
	dev->capture.record_mode = MZ0380_RECORD_MODE_CBR;
	dev->capture.bitrate = MZ0380_DEFAULT_BITRATE;
	dev->capture.quality = MZ0380_DEFAULT_QUALITY;
	dev->capture.gop_size = MZ0380_DEFAULT_GOP;
	dev->capture.qp_step = MZ0380_DEFAULT_QP_STEP;
	dev->capture.b_frames = MZ0380_DEFAULT_B_FRAMES;
	dev->capture.enc_bitrate = MZ0380_ENC_DEFAULT_BITRATE;
	dev->capture.enc_gop = MZ0380_ENC_DEFAULT_GOP;
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
	 * M245: one mutex for the queue and every other ioctl.
	 *
	 * They were separate, so S_FMT (vdev.lock) could pass its
	 * vb2_is_busy() test while REQBUFS on another file handle (q->lock)
	 * allocated buffers at the old size, and STREAMON wrote dev->capture
	 * under a lock S_DV_TIMINGS and S_FMT did not hold. Nothing here needs
	 * them apart: the ioctls that talk to the receiver already refuse to
	 * while streaming, and vb2 drops the lock while DQBUF waits.
	 */
	q->lock = &dev->lock;
	q->gfp_flags = GFP_KERNEL;
	q->min_queued_buffers = 2;
	q->dev = &dev->pci->dev;

	return vb2_queue_init(q);
}

/*
 * M245: the device struct lives until the last file handle closes.
 *
 * It embeds the video_device, and remove used to kfree() it with the node's
 * release set to video_device_release_empty. rmmod cannot reach that while a
 * handle is open - the module refcount holds it - but PCI unbind, hot removal
 * and AER can: the handle's close then ran vb2_fop_release and the fh teardown
 * against freed memory. The v4l2_device refcount already counts exactly the
 * right users (one for registration, one per registered node), so the struct is
 * freed from its release callback instead.
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

int mz0380_video_register(struct mz0380_dev *dev)
{
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

	memcpy(&dev->vdev, &mz0380_video_template, sizeof(dev->vdev));
	dev->vdev.fops = &mz0380_video_fops;
	dev->vdev.ioctl_ops = &mz0380_video_ioctl_ops;
	dev->vdev.lock = &dev->lock;
	dev->vdev.queue = &dev->vb_queue;
	dev->vdev.release = video_device_release_empty;
	dev->vdev.v4l2_dev = &dev->v4l2_dev;
	dev->vdev.ctrl_handler = &dev->ctrl_handler;
	dev->vdev.vfl_dir = VFL_DIR_RX;
	dev->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE |
				V4L2_CAP_STREAMING |
				V4L2_CAP_READWRITE;
	dev->vdev.dev_parent = &dev->pci->dev;
	/*
	 * M240: the node name is what applications list, so it should name the
	 * device, not one of the formats it can produce.
	 */
	strscpy(dev->vdev.name, "HD60 Pro HDMI capture", sizeof(dev->vdev.name));
	video_set_drvdata(&dev->vdev, dev);

	err = video_register_device(&dev->vdev, VFL_TYPE_VIDEO, -1);
	if (err)
		goto fail_ctrls;

	dev->video_registered = true;
	/* From here the struct is refcounted - see mz0380_v4l2_release. */
	dev->v4l2_dev.release = mz0380_v4l2_release;

	dev_info(&dev->pci->dev, "registered %s (streaming=%s)\n",
		 video_device_node_name(&dev->vdev),
		 dev->dma_armed ? "armed" : "disarmed");

	return 0;

fail_ctrls:
	if (dev->ctrl_handler_initialized) {
		v4l2_ctrl_handler_free(&dev->ctrl_handler);
		dev->bitrate_ctrl = NULL;
		dev->gop_ctrl = NULL;
		dev->ctrl_handler_initialized = false;
	}
fail_v4l2:
	if (dev->v4l2_registered) {
		v4l2_device_unregister(&dev->v4l2_dev);
		dev->v4l2_registered = false;
	}
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
	if (!dev->v4l2_dev.release && dev->ctrl_handler_initialized) {
		v4l2_ctrl_handler_free(&dev->ctrl_handler);
		dev->bitrate_ctrl = NULL;
		dev->gop_ctrl = NULL;
		dev->ctrl_handler_initialized = false;
	}
}
