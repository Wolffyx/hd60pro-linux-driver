// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host-side NO SIGNAL picture.
 *
 * Hardware M114 established that the card produces no frame at all while the
 * MST3367 has no lock, so a consumer would otherwise sit on its last picture
 * (or time out, as browser and conferencing apps do) whenever the source is
 * unplugged or has not started yet. The driver fills that gap itself, at
 * MZ0380_NO_SIGNAL_FPS, in whatever format the node is delivering:
 *
 *   H.264  a self-contained 1920x1080 IDR replayed from host memory; its
 *          centred 320x240 picture is the card's own NOSG_LOGO_Y, extracted
 *          from tinyvenc5 and identity-checked by SHA-256.
 *   raw    a black frame at the negotiated geometry with NO SIGNAL written
 *          across it (M247; before that a raw node simply stalled).
 *
 * Neither costs a mailbox command, a DMA ownership change or an encoder spawn.
 */

#include "mz0380.h"
#include "mz0380-no-signal-data.h"

#define MZ0380_GLYPH_W		5
#define MZ0380_GLYPH_H		7
#define MZ0380_TEXT_Y		200	/* luma of the lettering */
#define MZ0380_BLACK_Y		16
#define MZ0380_NEUTRAL_UV	128

/* "NO SIGNAL" in a 5x7 font, one byte per row, bit 4 = leftmost column. */
static const u8 mz0380_no_signal_text[][MZ0380_GLYPH_H] = {
	{ 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },	/* N */
	{ 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e },	/* O */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },	/*   */
	{ 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e },	/* S */
	{ 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e },	/* I */
	{ 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f },	/* G */
	{ 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },	/* N */
	{ 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 },	/* A */
	{ 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f },	/* L */
};

static unsigned long mz0380_no_signal_interval(void)
{
	return max_t(unsigned long, 1,
		     msecs_to_jiffies(1000 / MZ0380_NO_SIGNAL_FPS));
}

/*
 * Black limited-range I420/YV12/NV12 - neutral chroma is 128 in every layout,
 * so only the luma plane differs - with the lettering scaled to about a third
 * of the picture width and centred.
 */
static void mz0380_no_signal_render_raw(void *dst, u32 width, u32 height)
{
	const unsigned int chars = ARRAY_SIZE(mz0380_no_signal_text);
	const unsigned int cols = chars * (MZ0380_GLYPH_W + 1) - 1;
	size_t frame = mz0380_raw_frame_bytes_for(width, height);
	size_t luma = frame / 3 * 2;
	u32 stride = luma / height;
	unsigned int scale = max(1u, width / 3 / cols);
	unsigned int x0 = (width - min(width, cols * scale)) / 2;
	unsigned int y0 = (height - min(height, MZ0380_GLYPH_H * scale)) / 2;
	unsigned int c, row, bit, line;
	u8 *y = dst;

	memset(y, MZ0380_BLACK_Y, luma);
	memset(y + luma, MZ0380_NEUTRAL_UV, frame - luma);

	for (c = 0; c < chars; c++) {
		for (row = 0; row < MZ0380_GLYPH_H; row++) {
			for (bit = 0; bit < MZ0380_GLYPH_W; bit++) {
				unsigned int x = x0 + (c * (MZ0380_GLYPH_W + 1) +
						       bit) * scale;

				if (!(mz0380_no_signal_text[c][row] &
				      BIT(MZ0380_GLYPH_W - 1 - bit)))
					continue;
				if (x + scale > width)
					continue;
				for (line = 0; line < scale; line++) {
					u32 py = y0 + row * scale + line;

					if (py < height)
						memset(y + (size_t)py * stride + x,
						       MZ0380_TEXT_Y, scale);
				}
			}
		}
	}
}

/*
 * M225: a raw node shows the placeholder only once live raw frames have
 * stopped. The placeholder is also activated while a reattachment is being
 * validated, with live frames still flowing, and interleaving the two is a
 * visible flicker.
 */
static bool mz0380_raw_source_live(struct mz0380_dev *dev)
{
	unsigned long last = READ_ONCE(dev->raw_last_live);

	return last && time_before(jiffies,
				   last + msecs_to_jiffies(MZ0380_RAW_LIVE_MS));
}

/*
 * The bytes a NO SIGNAL picture takes in the current format, or 0 if there is
 * none to give: the H.264 one is a fixed 1080p access unit, and a decoder
 * handed it in a 720p stream would be handed a different stream.
 */
static size_t mz0380_no_signal_len(struct mz0380_dev *dev)
{
	if (READ_ONCE(dev->deliver_raw))
		return mz0380_raw_frame_bytes(dev);
	if (dev->capture.width != 1920 || dev->capture.height != 1080)
		return 0;
	return mz0380_no_signal_h264_len;
}

static void mz0380_no_signal_deliver(struct mz0380_dev *dev)
{
	size_t len = mz0380_no_signal_len(dev);
	struct mz0380_vb_buffer *vbuf;
	unsigned long flags;
	void *dst;

	if (!len)
		return;

	spin_lock_irqsave(&dev->buf_lock, flags);
	vbuf = list_first_entry_or_null(&dev->buf_list,
					struct mz0380_vb_buffer, list);
	if (vbuf)
		list_del(&vbuf->list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
	if (!vbuf) {
		dev->no_signal_frames_missed++;
		return;
	}

	dst = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
	if (!dst || vb2_plane_size(&vbuf->vb.vb2_buf, 0) < len) {
		dev->no_signal_frames_missed++;
		vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, 0);
		vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		return;
	}

	if (READ_ONCE(dev->deliver_raw)) {
		mz0380_no_signal_render_raw(dst, dev->capture.width,
					    dev->capture.height);
		dev->no_signal_raw_frames++;
	} else {
		memcpy(dst, mz0380_no_signal_h264, len);
		dev->no_signal_frames_delivered++;
	}
	vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, len);
	vbuf->vb.vb2_buf.timestamp = ktime_get_ns();
	vbuf->vb.field = mz0380_current_field(dev);
	vbuf->vb.sequence = dev->video_sequence++;
	vbuf->vb.flags &= ~(V4L2_BUF_FLAG_PFRAME | V4L2_BUF_FLAG_BFRAME);
	vbuf->vb.flags |= V4L2_BUF_FLAG_KEYFRAME;
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

static void mz0380_no_signal_work_fn(struct work_struct *work)
{
	struct mz0380_dev *dev = container_of(to_delayed_work(work),
					      struct mz0380_dev, no_signal_work);

	mutex_lock(&dev->delivery_lock);
	if (READ_ONCE(dev->streaming) && READ_ONCE(dev->no_signal_active) &&
	    !(READ_ONCE(dev->deliver_raw) && mz0380_raw_source_live(dev)))
		mz0380_no_signal_deliver(dev);
	mutex_unlock(&dev->delivery_lock);

	if (READ_ONCE(dev->streaming) && READ_ONCE(dev->no_signal_active))
		mod_delayed_work(MZ0380_SYSTEM_WQ, &dev->no_signal_work,
				 mz0380_no_signal_interval());
}

void mz0380_no_signal_init(struct mz0380_dev *dev)
{
	INIT_DELAYED_WORK(&dev->no_signal_work, mz0380_no_signal_work_fn);
}

void mz0380_no_signal_activate(struct mz0380_dev *dev, const char *reason)
{
	bool was_active = READ_ONCE(dev->no_signal_active);

	WRITE_ONCE(dev->no_signal_active, true);
	WRITE_ONCE(dev->h264_waiting_for_idr, true);
	if (was_active)
		return;
	dev_dbg(&dev->pci->dev, "NO SIGNAL picture armed (%s)\n", reason);
	/*
	 * Recovery may confirm the same unlocked state every retry interval;
	 * only the first activation schedules, so the cadence stays fixed.
	 */
	if (READ_ONCE(dev->streaming))
		mod_delayed_work(MZ0380_SYSTEM_WQ, &dev->no_signal_work, 0);
}

void mz0380_no_signal_deactivate(struct mz0380_dev *dev)
{
	if (!READ_ONCE(dev->no_signal_active))
		return;
	WRITE_ONCE(dev->no_signal_active, false);
	cancel_delayed_work(&dev->no_signal_work);
	dev_dbg(&dev->pci->dev, "NO SIGNAL picture ended at a live frame\n");
}

void mz0380_no_signal_stop(struct mz0380_dev *dev)
{
	WRITE_ONCE(dev->no_signal_active, false);
	cancel_delayed_work_sync(&dev->no_signal_work);
}
