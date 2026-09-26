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

#define MZ0380_DEFAULT_COLOR        128
#define MZ0380_MAX_COLOR            255
/* M171: buffers vb2's read() fileio path uses; reported by G/S_PARM. */
#define MZ0380_READ_BUFFERS         2
#define MZ0380_SIZEIMAGE_MIN        (256 * 1024)
#define MZ0380_SIZEIMAGE_MAX        (4 * 1024 * 1024)
#define MZ0380_CID_SC540_RECORD_MODE (V4L2_CID_USER_BASE + 0x10f0)

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

static const char * const mz0380_record_mode_qmenu[] = {
	"VBR",
	"CBR",
	"HBR",
};

static const struct v4l2_ctrl_ops mz0380_ctrl_ops;

static const struct v4l2_ctrl_config mz0380_ctrl_record_mode = {
	.ops = &mz0380_ctrl_ops,
	.id = MZ0380_CID_SC540_RECORD_MODE,
	.name = "SC540 Record Mode",
	.type = V4L2_CTRL_TYPE_MENU,
	.min = MZ0380_RECORD_MODE_VBR,
	.max = MZ0380_RECORD_MODE_HBR,
	.def = MZ0380_RECORD_MODE_CBR,
	.qmenu = mz0380_record_mode_qmenu,
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

static u32 mz0380_sizeimage(const struct mz0380_capture_state *capture)
{
	u32 bitrate = max(capture->bitrate, capture->bitrate_peak);
	u32 sizeimage = DIV_ROUND_UP(bitrate, 8);

	sizeimage = clamp(sizeimage, (u32)MZ0380_SIZEIMAGE_MIN,
			  (u32)MZ0380_SIZEIMAGE_MAX);

	return sizeimage;
}

/*
 * With stream_nosg the card's fake-frame generator delivers raw NV12 (M36:
 * one 1920x1107x1.5 contiguous burst; the leading 1920x1080 is the picture),
 * captured by the polling loop in mz0380-dma.c - not H.264. The node
 * advertises whichever format the current mode actually produces.
 */
/*
 * M111: the real path delivers RAW frames too, not H.264, whenever the
 * poll-drain is doing the delivering. buf0 receives exactly
 * 1920*1080*3/2 = 3110400 bytes of 4:2:0 - the card's own splash - and the
 * encoder never produces a bitstream for it. Advertising H.264 there would
 * hand userspace 3 MB of planar YUV labelled as a bytestream, and sizing the
 * plane from the bitrate (12 Mbit/8 = 1.5 MB) would make the drain reject
 * every frame as "does not fit vb2 plane" before it got that far.
 *
 * So both the format and the plane size follow the same switch.
 */
static u32 mz0380_raw_frame_size(const struct mz0380_capture_state *capture)
{
	return capture->width * capture->height * 3 / 2;
}

u32 mz0380_current_sizeimage(struct mz0380_dev *dev)
{
	u32 size;

	if (mz0380_raw_bank_probe)
		return MZ0380_RAW_PROBE_FRAME_SIZE;
	if (dev->deliver_raw)		/* M217/M218, before h264_probe */
		return mz0380_raw_frame_bytes(dev);  /* M221: not always 1080p */
	if (mz0380_h264_probe)
		return MZ0380_H264_SET_BUF_SIZE - 4096;
	if (mz0380_stream_nosg)
		return MZ0380_NOSG_NV12_SIZEIMAGE;
	if (mz0380_poll_drain_ms)
		size = max(mz0380_raw_frame_size(&dev->capture),
			   mz0380_sizeimage(&dev->capture));
	else
		size = mz0380_sizeimage(&dev->capture);

	/*
	 * M175: a declared frame expectation is also a plane requirement.
	 *
	 * expect_frame_bytes exists because the card does not always write
	 * source-sized frames - tinyvenc8 writes 960x540, and fw=6 is
	 * documented to switch the card's output format to YUY2, which at
	 * 1080p is 4147200 bytes against this plane's 3110400. Without this
	 * the drain would measure a complete frame and then reject it with
	 * "does not fit vb2 plane", which is a different way to lose the same
	 * frame - and a harder one to read, because it looks like a driver bug
	 * rather than an expectation mismatch.
	 *
	 * Bounded by the DMA buffer, since nothing larger can arrive anyway.
	 */
	if (mz0380_expect_frame_bytes)
		size = max_t(u32, size,
			     min_t(u32, mz0380_expect_frame_bytes,
				   MZ0380_STREAM_BUF_SIZE));

	return size;
}

/*
 * M168: ONE place decides the advertised fourcc, because three places used to
 * and they had drifted apart - ENUM_FMT said NV12 while ENUM_FRAMESIZES and
 * ENUM_FRAMEINTERVALS still tested for H.264 and so returned -EINVAL for the
 * very format the node had just enumerated. Every caller now asks here.
 *
 * The poll-drain payload is planar I420 - Y, then a 960x540 U plane, then a
 * 960x540 V plane (M130, confirmed visually AND by correlation: the U/V-swapped
 * rendering gives the textbook red/blue swap, plain yuv420p does not). It was
 * advertised as NV12, which is the same byte count with the chroma
 * INTERLEAVED, so every V4L2 application - ffmpeg, GStreamer, OBS - rendered
 * the magenta/green interleave banding RE_FINDINGS describes. The frame was
 * always right; the label was wrong. V4L2_PIX_FMT_YUV420 is I420.
 *
 * The nosg fake-frame path keeps NV12: its layout was never confirmed either
 * way (different producer, different 1920x1107 geometry, flat logo content
 * where interleave banding would not show), so correcting it would be a guess
 * rather than a measurement. It is a diagnostic path and defaults off.
 */
u32 mz0380_current_pixelformat(struct mz0380_dev *dev)
{
	if (mz0380_raw_bank_probe)
		return V4L2_PIX_FMT_YUV420;
	/*
	 * M217/M218: raw delivery runs WITH h264_probe - the encoder has to
	 * keep running for the card to produce raw at all (M210b) - so it has
	 * to be tested first, or the node would advertise H.264 while
	 * delivering I420. Which one is live is now a runtime choice (S_FMT),
	 * not a module parameter; the parameter only seeds it.
	 */
	if (dev && dev->deliver_raw)
		return dev->raw_fourcc ? dev->raw_fourcc : V4L2_PIX_FMT_YUV420;
	if (mz0380_h264_probe)
		return V4L2_PIX_FMT_H264;
	if (mz0380_stream_nosg)
		return V4L2_PIX_FMT_NV12;
	if (mz0380_poll_drain_ms)
		return V4L2_PIX_FMT_YUV420;
	return V4L2_PIX_FMT_H264;
}

static void mz0380_fill_pix_format(struct mz0380_dev *dev,
				   struct v4l2_pix_format *pix)
{
	memset(pix, 0, sizeof(*pix));

	pix->pixelformat = mz0380_current_pixelformat(dev);

	if (mz0380_stream_nosg) {
		pix->width = MZ0380_NOSG_NV12_WIDTH;
		pix->height = MZ0380_NOSG_NV12_HEIGHT;
		pix->bytesperline = MZ0380_NOSG_NV12_WIDTH;
	} else if (mz0380_raw_bank_probe || dev->deliver_raw ||
		   mz0380_poll_drain_ms) {
		/* M111: real geometry, raw payload. */
		pix->width = dev->capture.width;
		pix->height = dev->capture.height;
		pix->bytesperline = dev->capture.width;
	} else {
		pix->width = dev->capture.width;
		pix->height = dev->capture.height;
		pix->bytesperline = 0;
	}

	pix->field = mz0380_current_field(dev);
	pix->sizeimage = mz0380_current_sizeimage(dev);
	pix->colorspace = V4L2_COLORSPACE_REC709;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
	/*
	 * M234: report the range the payload actually uses.
	 *
	 * This said LIM_RANGE for every format. A 600-frame raw capture
	 * measured luma up to 254 with 4.2% of sampled pixels above 235, which
	 * limited-range content cannot contain, and the card writes its own
	 * black as 0-1 rather than 16. Declaring full-range data as limited
	 * makes every consumer expand 16..235 to 0..255 a second time, which
	 * shows as a brighter and harsher picture than the same card produces
	 * on Windows.
	 *
	 * Raw only. The H.264 bitstream carries its own VUI, so what this node
	 * claims for it is not what a decoder obeys, and changing it could
	 * mislead a consumer that is currently correct.
	 */
	if (mz0380_raw_full_range &&
	    pix->pixelformat == V4L2_PIX_FMT_YUV420)
		pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	else
		pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_709;
}

static void mz0380_apply_try_fmt(struct mz0380_dev *dev, struct v4l2_format *f)
{
	const struct mz0380_mode *mode;
	const struct v4l2_fract *interval;

	mode = mz0380_find_mode(f->fmt.pix.width, f->fmt.pix.height);
	interval = mz0380_find_interval(mode->width, mode->height,
					&dev->capture.timeperframe);

	dev->capture.width = mode->width;
	dev->capture.height = mode->height;
	dev->capture.timeperframe = *interval;
	mz0380_fill_pix_format(dev, &f->fmt.pix);
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

static int mz0380_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct mz0380_dev *dev = video_drvdata(file);
	struct mz0380_capture_state saved = dev->capture;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (mz0380_stream_nosg) {
		/* fake-frame path: one fixed raw mode */
		mz0380_fill_pix_format(dev, &f->fmt.pix);
		return 0;
	}

	/*
	 * M218: the pixelformat IS negotiable now, when the raw banks exist.
	 * Anything else still falls back to what the driver would deliver -
	 * V4L2 requires TRY_FMT to return a workable format, never an error.
	 */
	mz0380_clamp_to_source(dev, &f->fmt.pix.width, &f->fmt.pix.height);

	if (dev->raw_capable) {
		bool want_raw = mz0380_is_raw_fourcc(f->fmt.pix.pixelformat);
		bool saved_raw = dev->deliver_raw;
		u32 saved_fourcc = dev->raw_fourcc;

		dev->deliver_raw = want_raw;
		if (want_raw)
			dev->raw_fourcc = f->fmt.pix.pixelformat;
		mz0380_apply_try_fmt(dev, f);
		dev->deliver_raw = saved_raw;
		dev->raw_fourcc = saved_fourcc;
		dev->capture = saved;
		return 0;
	}

	mz0380_apply_try_fmt(dev, f);
	dev->capture = saved;

	return 0;
}

static int mz0380_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct mz0380_dev *dev = video_drvdata(file);
	const struct mz0380_mode *mode;
	const struct v4l2_fract *interval;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (mz0380_stream_nosg) {
		/* fake-frame path: one fixed raw mode */
		mz0380_fill_pix_format(dev, &f->fmt.pix);
		return 0;
	}

	/*
	 * M218: commit the requested delivery format. Changing it while buffers
	 * are queued would hand an application a plane sized for the other
	 * format, so refuse rather than surprise it - this is what every V4L2
	 * driver does and what applications already expect.
	 */
	mz0380_clamp_to_source(dev, &f->fmt.pix.width, &f->fmt.pix.height);

	if (dev->raw_capable) {
		bool want_raw = mz0380_is_raw_fourcc(f->fmt.pix.pixelformat);

		/*
		 * M241: a switch BETWEEN raw layouts needs none of the
		 * pipeline work below - the card keeps writing I420 either
		 * way and only the copy re-orders - so record it and fall
		 * through without disturbing a running stream.
		 */
		if (want_raw)
			dev->raw_fourcc = f->fmt.pix.pixelformat;

		if (want_raw != dev->deliver_raw) {
			if (vb2_is_busy(&dev->vb_queue))
				return -EBUSY;
			dev->deliver_raw = want_raw;

			/*
			 * M221: the card has to be told, and a persistent
			 * pipeline will not tell it.
			 *
			 * post_mask bit 0 - what makes the card write whole
			 * frames instead of 16-byte stubs - is sent by
			 * mz0380_stream_post_proc() during stream start. Under
			 * persistent_h264 a second STREAMON on a running
			 * pipeline attaches VB2 and nothing else, so switching
			 * H.264 -> I420 after streaming once would leave the
			 * card in stub mode and deliver nothing at all. Flag
			 * the pipeline for replacement so the next attachment
			 * restarts it and re-sends the command.
			 */
			if (READ_ONCE(dev->pipeline_running))
				WRITE_ONCE(dev->pipeline_reconfigure_pending,
					   true);
			pr_info("%s: delivery format set to %s%s\n", dev->name,
				want_raw ? "raw (uncompressed)" : "H.264",
				READ_ONCE(dev->pipeline_running) ?
					"; encoder pipeline will be replaced at the next attachment" :
					"");
		}
	}

	mode = mz0380_find_mode(f->fmt.pix.width, f->fmt.pix.height);
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

	if (index >= ARRAY_SIZE(mz0380_input_names))
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
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		ctrl->val = dev->capture.bitrate;
		return 0;
	case V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY:
		ctrl->val = dev->capture.quality;
		return 0;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		ctrl->val = dev->capture.gop_size;
		return 0;
	case V4L2_CID_MPEG_VIDEO_B_FRAMES:
		ctrl->val = dev->capture.b_frames;
		return 0;
	case MZ0380_CID_SC540_RECORD_MODE:
		ctrl->val = dev->capture.record_mode;
		return 0;
	default:
		return -EINVAL;
	}
}

static int mz0380_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mz0380_dev *dev =
		container_of(ctrl->handler, struct mz0380_dev, ctrl_handler);

	switch (ctrl->id) {
	case MZ0380_CID_SC540_RECORD_MODE:
		return mz0380_request_record_mode_locked(dev, ctrl->val,
							 "v4l2-ctrl");
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		return mz0380_request_bitrate_locked(dev, ctrl->val,
						     "v4l2-ctrl");
	case V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY:
		return mz0380_request_quality_locked(dev, ctrl->val,
						     "v4l2-ctrl");
	case V4L2_CID_MPEG_VIDEO_BITRATE_PEAK:
		dev->capture.bitrate_peak = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		return mz0380_request_gop_locked(dev, ctrl->val,
						 "v4l2-ctrl");
	case V4L2_CID_MPEG_VIDEO_B_FRAMES:
		return mz0380_request_b_frames_locked(dev, ctrl->val,
						      "v4l2-ctrl");
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		dev->capture.h264_profile = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		dev->capture.h264_level = ctrl->val;
		break;
	case V4L2_CID_BRIGHTNESS:
		dev->capture.brightness = ctrl->val;
		break;
	case V4L2_CID_CONTRAST:
		dev->capture.contrast = ctrl->val;
		break;
	case V4L2_CID_HUE:
		dev->capture.hue = ctrl->val;
		break;
	case V4L2_CID_SATURATION:
		dev->capture.saturation = ctrl->val;
		break;
	case V4L2_CID_SHARPNESS:
		dev->capture.sharpness = ctrl->val;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops mz0380_ctrl_ops = {
	.g_volatile_ctrl = mz0380_g_volatile_ctrl,
	.s_ctrl = mz0380_s_ctrl,
};

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
	v4l2_ctrl_handler_init(&dev->ctrl_handler, 14);
	dev->ctrl_handler.lock = &dev->ctrl_lock;
	dev->ctrl_handler_initialized = true;

	dev->record_mode_ctrl =
		v4l2_ctrl_new_custom(&dev->ctrl_handler,
				     &mz0380_ctrl_record_mode, NULL);
	if (dev->record_mode_ctrl)
		dev->record_mode_ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	dev->bitrate_ctrl =
		v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_BITRATE,
				  MZ0380_MIN_BITRATE, MZ0380_MAX_BITRATE,
				  256 * 1024, MZ0380_DEFAULT_BITRATE);
	if (dev->bitrate_ctrl)
		dev->bitrate_ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	dev->quality_ctrl =
		v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY,
				  MZ0380_MIN_QUALITY, MZ0380_MAX_QUALITY,
				  1, MZ0380_DEFAULT_QUALITY);
	if (dev->quality_ctrl)
		dev->quality_ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_BITRATE_PEAK,
			  MZ0380_MIN_BITRATE, MZ0380_MAX_BITRATE,
			  256 * 1024, MZ0380_DEFAULT_BITRATE);
	dev->gop_ctrl =
		v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_GOP_SIZE,
				  MZ0380_MIN_GOP, MZ0380_MAX_GOP, 1,
				  MZ0380_DEFAULT_GOP);
	if (dev->gop_ctrl)
		dev->gop_ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	dev->b_frames_ctrl =
		v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_B_FRAMES,
				  0, MZ0380_MAX_B_FRAMES, 1,
				  MZ0380_DEFAULT_B_FRAMES);
	if (dev->b_frames_ctrl)
		dev->b_frames_ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE;
	v4l2_ctrl_new_std_menu(&dev->ctrl_handler, &mz0380_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_H264_PROFILE,
			       V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, 0,
			       V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
	v4l2_ctrl_new_std_menu(&dev->ctrl_handler, &mz0380_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_H264_LEVEL,
			       V4L2_MPEG_VIDEO_H264_LEVEL_4_2, 0,
			       V4L2_MPEG_VIDEO_H264_LEVEL_4_2);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
			  V4L2_CID_BRIGHTNESS, 0, MZ0380_MAX_COLOR, 1,
			  MZ0380_DEFAULT_COLOR);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
			  V4L2_CID_CONTRAST, 0, MZ0380_MAX_COLOR, 1,
			  MZ0380_DEFAULT_COLOR);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
			  V4L2_CID_HUE, 0, MZ0380_MAX_COLOR, 1,
			  MZ0380_DEFAULT_COLOR);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
			  V4L2_CID_SATURATION, 0, MZ0380_MAX_COLOR, 1,
			  MZ0380_DEFAULT_COLOR);
	v4l2_ctrl_new_std(&dev->ctrl_handler, &mz0380_ctrl_ops,
			  V4L2_CID_SHARPNESS, 0, MZ0380_MAX_COLOR, 1,
			  MZ0380_DEFAULT_COLOR);

	if (dev->ctrl_handler.error) {
		int err = dev->ctrl_handler.error;

		v4l2_ctrl_handler_free(&dev->ctrl_handler);
		dev->bitrate_ctrl = NULL;
		dev->quality_ctrl = NULL;
		dev->gop_ctrl = NULL;
		dev->b_frames_ctrl = NULL;
		dev->record_mode_ctrl = NULL;
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
	dev->capture.bitrate_peak = MZ0380_DEFAULT_BITRATE;
	dev->capture.gop_size = MZ0380_DEFAULT_GOP;
	dev->capture.qp_step = MZ0380_DEFAULT_QP_STEP;
	dev->capture.b_frames = MZ0380_DEFAULT_B_FRAMES;
	dev->capture.h264_profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
	dev->capture.h264_level = V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
	dev->capture.brightness = MZ0380_DEFAULT_COLOR;
	dev->capture.contrast = MZ0380_DEFAULT_COLOR;
	dev->capture.hue = MZ0380_DEFAULT_COLOR;
	dev->capture.saturation = MZ0380_DEFAULT_COLOR;
	dev->capture.sharpness = MZ0380_DEFAULT_COLOR;
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
	q->lock = &dev->queue_lock;
	q->gfp_flags = GFP_KERNEL;
	q->min_queued_buffers = 2;
	q->dev = &dev->pci->dev;

	return vb2_queue_init(q);
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
	 * device, not one of the formats it can produce. "mz0380 H.264" made a
	 * card that captures uncompressed video look like a codec node.
	 */
	strscpy(dev->vdev.name, "HD60 Pro HDMI capture", sizeof(dev->vdev.name));
	video_set_drvdata(&dev->vdev, dev);

	err = video_register_device(&dev->vdev,
				    VFL_TYPE_VIDEO, -1);
	if (err)
		goto fail_ctrls;

	dev->video_registered = true;

	printk(KERN_INFO
	       "%s: registered %s (H.264 capture, streaming=%s)\n",
	       dev->name, video_device_node_name(&dev->vdev),
	       dev->dma_armed ? "armed" : "disarmed");

	return 0;

fail_ctrls:
	if (dev->ctrl_handler_initialized) {
		v4l2_ctrl_handler_free(&dev->ctrl_handler);
		dev->bitrate_ctrl = NULL;
		dev->quality_ctrl = NULL;
		dev->gop_ctrl = NULL;
		dev->b_frames_ctrl = NULL;
		dev->record_mode_ctrl = NULL;
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
		video_unregister_device(&dev->vdev);
		dev->video_registered = false;
	}

	if (dev->ctrl_handler_initialized) {
		v4l2_ctrl_handler_free(&dev->ctrl_handler);
		dev->bitrate_ctrl = NULL;
		dev->quality_ctrl = NULL;
		dev->gop_ctrl = NULL;
		dev->b_frames_ctrl = NULL;
		dev->record_mode_ctrl = NULL;
		dev->ctrl_handler_initialized = false;
	}

	if (dev->v4l2_registered) {
		v4l2_device_unregister(&dev->v4l2_dev);
		dev->v4l2_registered = false;
	}
}
