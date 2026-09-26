// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 frame inference, event draining, and polling delivery.
 */

#include "mz0380-dma-internal.h"

int mz0380_infer_frame_length(struct mz0380_dev *dev, u32 idx,
				     size_t *length)
{
	const u64 *qwords;
	const u32 *dwords;
	u64 poison_q;
	u32 poison_w;
	size_t i;

	*length = 0;
	if (idx >= MZ0380_STREAM_NR_BUFS || !dev->stream_bufs[idx].va ||
	    !smp_load_acquire(&dev->frame_poison_active))
		return -EINVAL;

	qwords = dev->stream_bufs[idx].va;
	dwords = dev->stream_bufs[idx].va;
	poison_q = mz0380_frame_poison_q(dev);
	poison_w = mz0380_frame_poison_w(dev);
	dma_rmb();

	for (i = MZ0380_STREAM_BUF_SIZE / sizeof(*qwords); i; i--) {
		size_t dword = (i - 1) * 2;

		if (READ_ONCE(qwords[i - 1]) == poison_q)
			continue;

		if (READ_ONCE(dwords[dword + 1]) != poison_w)
			*length = (dword + 2) * sizeof(*dwords);
		else
			*length = (dword + 1) * sizeof(*dwords);

		/* No untouched suffix means truncation, not a proven frame end. */
		if (*length == MZ0380_STREAM_BUF_SIZE)
			return -ENOSPC;
		return 0;
	}

	return -ENODATA;
}

static bool mz0380_frame_event_pop(struct mz0380_dev *dev,
				   struct mz0380_frame_event *snapshot)
{
	unsigned long flags;
	bool have_event = false;

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	if (dev->frame_event_tail != dev->frame_event_head) {
		*snapshot = dev->frame_events[dev->frame_event_tail];
		dev->frame_event_tail = (dev->frame_event_tail + 1) %
					MZ0380_FRAME_EVENT_FIFO_SIZE;
		have_event = true;
	}
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);

	return have_event;
}

struct mz0380_h264_au_class {
	bool has_vcl;
	bool has_idr;
	bool has_parameter_set;
};

static size_t mz0380_h264_find_start_code(const u8 *payload, size_t len,
					   size_t from, size_t *prefix_len)
{
	size_t i;

	for (i = from; i + 3 <= len; i++) {
		if (payload[i] || payload[i + 1])
			continue;
		if (payload[i + 2] == 1) {
			*prefix_len = 3;
			return i;
		}
		if (i + 4 <= len && !payload[i + 2] && payload[i + 3] == 1) {
			*prefix_len = 4;
			return i;
		}
	}
	return len;
}

static void mz0380_h264_cache_parameter_sets(struct mz0380_dev *dev,
					      const u8 *payload, size_t len)
{
	size_t prefix_len;
	size_t start = mz0380_h264_find_start_code(payload, len, 0,
						       &prefix_len);
	bool refreshed = false;

	if (!dev->h264_parameter_sets)
		return;

	while (start < len) {
		size_t nal = start + prefix_len;
		size_t next_prefix_len;
		size_t next;
		size_t bytes;
		u8 type;

		if (nal >= len)
			break;
		next = mz0380_h264_find_start_code(payload, len, nal + 1,
						    &next_prefix_len);
		type = payload[nal] & 0x1f;
		bytes = next - start;

		if (type == 7) {
			dev->h264_parameter_sets_len = 0;
			refreshed = true;
		}
		if ((type == 7 || type == 8) &&
		    (refreshed || !dev->h264_parameter_sets_len) &&
		    bytes <= MZ0380_H264_PARAMETER_SETS_MAX -
			     dev->h264_parameter_sets_len) {
			memcpy(dev->h264_parameter_sets +
			       dev->h264_parameter_sets_len,
			       payload + start, bytes);
			dev->h264_parameter_sets_len += bytes;
		}

		start = next;
		if (next < len)
			prefix_len = next_prefix_len;
	}
}

static struct mz0380_h264_au_class
mz0380_h264_classify_au(const u8 *payload, size_t len)
{
	struct mz0380_h264_au_class c = { 0 };
	size_t i = 0;

	while (i + 3 < len) {
		size_t nal;
		u8 type;

		if (payload[i] || payload[i + 1]) {
			i++;
			continue;
		}
		if (payload[i + 2] == 1)
			nal = i + 3;
		else if (i + 4 < len && !payload[i + 2] &&
			 payload[i + 3] == 1)
			nal = i + 4;
		else {
			i++;
			continue;
		}

		type = payload[nal] & 0x1f;
		if (type >= 1 && type <= 5)
			c.has_vcl = true;
		if (type == 5)
			c.has_idr = true;
		if (type == 7 || type == 8)
			c.has_parameter_set = true;
		i = nal + 1;
	}

	return c;
}

/*
 * A receiver reconnect can resume the encoder in the middle of its old GOP.
 * Feeding those dependent pictures to OBS produces exactly the smeared first
 * frames and persistent frozen decoders seen on hardware.  Parameter-set-only
 * access units are still passed through, but dependent VCL pictures are held
 * until an IDR establishes a new random-access point.
 */
static bool mz0380_h264_recovery_accept(struct mz0380_dev *dev,
					const u8 *payload, size_t len,
					bool *is_idr,
					bool *prepend_parameter_sets,
					bool *has_parameter_sets)
{
	struct mz0380_h264_au_class c = mz0380_h264_classify_au(payload, len);
	unsigned long now = jiffies;
	unsigned long last = READ_ONCE(dev->last_h264_frame_stamp);
	unsigned long stall = msecs_to_jiffies(
		max_t(unsigned int, mz0380_hotplug_stall_ms, 1));

	*is_idr = c.has_idr;
	*prepend_parameter_sets = false;
	*has_parameter_sets = c.has_parameter_set;
	if (READ_ONCE(dev->no_signal_active)) {
		/*
		 * Do not let stale card pictures or standalone live SPS/PPS toggle
		 * the decoder away from the placeholder. The switch is atomic at a
		 * receiver-proven live IDR; a parameter-set-free IDR gets the cache
		 * prepended below.
		 */
		if (!READ_ONCE(dev->signal_locked) ||
		    READ_ONCE(dev->signal_recovering) || !c.has_idr) {
			if (c.has_vcl)
				WRITE_ONCE(dev->last_h264_frame_stamp, now);
			return false;
		}
		if (READ_ONCE(dev->pipeline_reconfigure_pending) ||
		    READ_ONCE(dev->pipeline_start_failed)) {
			if (c.has_vcl)
				WRITE_ONCE(dev->last_h264_frame_stamp, now);
			return false;
		}
	}
	if (!c.has_vcl)
		return true;

	if (mz0380_hotplug_recovery && last &&
	    time_after_eq(now, last + stall)) {
		WRITE_ONCE(dev->h264_waiting_for_idr, true);
		dev_warn(&dev->pci->dev,
			 "H.264 resumed after a %ums gap; discarding dependent pictures until IDR\n",
			 jiffies_to_msecs(now - last));
	}
	WRITE_ONCE(dev->last_h264_frame_stamp, now);

	if (!READ_ONCE(dev->h264_waiting_for_idr))
		return true;
	if (!c.has_idr) {
		pr_info_ratelimited("%s: post-start/reconnect H.264 picture dropped while waiting for IDR%s\n",
				    dev->name,
				    c.has_parameter_set ? " (access unit also contained SPS/PPS)" : "");
		return false;
	}

	*prepend_parameter_sets = !c.has_parameter_set &&
				  dev->h264_parameter_sets_len;
	return true;
}

/*
 * tinyvenc7's encoded-output completion record is the second report nibble:
 * BAR0+0x44, captured as snapshot->payload[0]. The low nibble is a zero-based
 * window-1 slot. Each slot begins with a 4 KiB card header:
 *
 *   +0x00 encoded byte count
 *   +0x08 encoder metadata (meaning not needed for delivery)
 *   +0x0c one-based PCIe slot identity
 *   +0x10 encoded byte count again
 *   +0x1000 Annex-B H.264 access unit
 *
 * Both length copies, the slot identity, and the Annex-B prefix were observed
 * on all four buffers in the first successful window-1 hardware run. Validate
 * all of them before exposing an access unit to userspace.
 */
static void
mz0380_drain_h264_snapshot(struct mz0380_dev *dev,
			    const struct mz0380_frame_event *snapshot)
{
	struct mz0380_vb_buffer *vbuf;
	const u32 *header;
	const u8 *payload;
	unsigned long flags;
	u32 idx = snapshot->payload[0] & 0xf;
	u32 len, len_copy, slot;
	size_t plane;
	bool is_idr;
	bool prepend_parameter_sets;
	bool has_parameter_sets;
	bool placeholder_active;
	size_t prefix_bytes = 0;
	void *dst;

	if (idx >= MZ0380_STREAM_NR_BUFS || !dev->h264_bufs[idx].va) {
		pr_info_ratelimited("%s: H.264 completion has no valid window1 slot yet (BAR0+0x44=%08x)\n",
				    dev->name, snapshot->payload[0]);
		return;
	}
	/* Raw-preview events may repeat the last encoded token. */
	if (dev->h264_last_token_valid && dev->h264_last_token == idx)
		return;

	header = dev->h264_bufs[idx].va;
	dma_rmb();
	len = READ_ONCE(header[0]);
	slot = READ_ONCE(header[3]);
	len_copy = READ_ONCE(header[4]);
	payload = (const u8 *)header + 4096;
	/*
	 * Raw-preview events can name an H.264 slot before its DMA has touched
	 * anything.  All three header words and the payload prefix are then still
	 * the 0xcc poison (decimal 3435973836), which is an expected empty slot,
	 * not a malformed access unit worth alarming the operator about.  Leave
	 * the token unconsumed so the later encoded completion can retry it.
	 */
	if (len == 0x01010101u * MZ0380_H264_POISON_BYTE &&
	    len_copy == len && slot == len &&
	    payload[0] == MZ0380_H264_POISON_BYTE &&
	    payload[1] == MZ0380_H264_POISON_BYTE &&
	    payload[2] == MZ0380_H264_POISON_BYTE &&
	    payload[3] == MZ0380_H264_POISON_BYTE)
		return;

	if (!len || len != len_copy || slot != idx + 1 ||
	    len > MZ0380_H264_SET_BUF_SIZE - 4096 ||
	    !((payload[0] == 0 && payload[1] == 0 && payload[2] == 0 &&
	       payload[3] == 1) ||
	      (payload[0] == 0 && payload[1] == 0 && payload[2] == 1))) {
		pr_info_ratelimited("%s: H.264 slot %u not complete: len=%u copy=%u identity=%u prefix=%02x %02x %02x %02x\n",
				    dev->name, idx, len, len_copy, slot,
				    payload[0], payload[1], payload[2], payload[3]);
		return;
	}

	/* The token is now consumed even if userspace supplied no free buffer. */
	dev->h264_last_token = idx;
	dev->h264_last_token_valid = true;
	mz0380_h264_cache_parameter_sets(dev, payload, len);

	if (!READ_ONCE(dev->streaming)) {
		struct mz0380_h264_au_class c =
			mz0380_h264_classify_au(payload, len);

		if (c.has_vcl)
			WRITE_ONCE(dev->last_h264_frame_stamp, jiffies);
		dev->h264_frames_discarded++;
		return;
	}

	mutex_lock(&dev->h264_delivery_lock);
	if (!READ_ONCE(dev->streaming)) {
		dev->h264_frames_discarded++;
		mutex_unlock(&dev->h264_delivery_lock);
		return;
	}
	placeholder_active = READ_ONCE(dev->no_signal_active);
	if (!mz0380_h264_recovery_accept(dev, payload, len, &is_idr,
					   &prepend_parameter_sets,
					   &has_parameter_sets)) {
		if (placeholder_active)
			dev->h264_frames_suppressed++;
		else
			dev->h264_frames_dropped++;
		mutex_unlock(&dev->h264_delivery_lock);
		return;
	}
	prefix_bytes = prepend_parameter_sets ?
		       dev->h264_parameter_sets_len : 0;

	/* STREAMOFF may have raced the validation above; never take a VB2 buffer
	 * after detachment has been published. */
	if (!READ_ONCE(dev->streaming)) {
		dev->h264_frames_discarded++;
		mutex_unlock(&dev->h264_delivery_lock);
		return;
	}

	spin_lock_irqsave(&dev->buf_lock, flags);
	vbuf = list_first_entry_or_null(&dev->buf_list,
					struct mz0380_vb_buffer, list);
	if (vbuf)
		list_del(&vbuf->list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
	if (!vbuf) {
		dev->h264_frames_dropped++;
		pr_info_ratelimited("%s: H.264 slot %u length=%u has no queued vb2 buffer; access unit dropped\n",
				    dev->name, idx, len);
		mutex_unlock(&dev->h264_delivery_lock);
		return;
	}

	dst = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
	plane = vb2_plane_size(&vbuf->vb.vb2_buf, 0);
	if (!dst || len + prefix_bytes > plane) {
		dev->h264_frames_dropped++;
		pr_warn_ratelimited("%s: H.264 access unit %u bytes + %zu cached SPS/PPS bytes does not fit vb2 plane %zu\n",
				    dev->name, len, prefix_bytes, plane);
		vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, 0);
		vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		mutex_unlock(&dev->h264_delivery_lock);
		return;
	}

	dma_rmb();
	if (prefix_bytes)
		memcpy(dst, dev->h264_parameter_sets, prefix_bytes);
	memcpy((u8 *)dst + prefix_bytes, payload, len);
	vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, len + prefix_bytes);
	vbuf->vb.vb2_buf.timestamp = snapshot->timestamp_ns;
	vbuf->vb.field = mz0380_current_field(dev);
	vbuf->vb.sequence = dev->video_sequence++;
	vbuf->vb.flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
			   V4L2_BUF_FLAG_BFRAME);
	vbuf->vb.flags |= is_idr ? V4L2_BUF_FLAG_KEYFRAME :
				     V4L2_BUF_FLAG_PFRAME;
	if (is_idr && READ_ONCE(dev->h264_waiting_for_idr)) {
		WRITE_ONCE(dev->h264_waiting_for_idr, false);
		dev_info(&dev->pci->dev,
			 "clean H.264 IDR received; encoded delivery is synchronized%s\n",
			 has_parameter_sets ? " (access unit includes SPS/PPS)" :
			 (prepend_parameter_sets ?
			  " (cached SPS/PPS prepended)" :
			  " (no SPS/PPS cache available)"));
	}
	if (is_idr && READ_ONCE(dev->no_signal_active) &&
	    READ_ONCE(dev->signal_locked) &&
	    !READ_ONCE(dev->signal_recovering) &&
	    !READ_ONCE(dev->pipeline_reconfigure_pending))
		mz0380_no_signal_deactivate(dev);
	dev->h264_bufs[idx].delivered++;
	dev->h264_frames_delivered++;
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	mutex_unlock(&dev->h264_delivery_lock);
}

/*
 * M212: the control M128a asked for and never got.
 *
 * Every run that reaches the raw ring writes exactly sixteen bytes and stops -
 * 0x11 in the splash era, 0x53 in M128a, 0x5a..0x5e tonight.  Whether those are
 * source pixels or a broken transfer of whatever sat in the buffer has never
 * been settled, and it decides which bug this is: a capture that works and a
 * transfer that dies after one burst, or no capture at all.
 *
 * In observation mode nothing routes completions to the raw ring, so the only
 * view of those bytes was the read-back at stop - one sample, after the fact.
 * Sampling them alongside each encoded completion turns a bounded run over a
 * changing scene into the answer: bytes that track the source are pixels.
 *
 * Sixteen bytes per encoded frame across four slots is nothing next to the DMA
 * this path already does, and it only ever reads.
 */
static void mz0380_raw_observe_sample(struct mz0380_dev *dev)
{
	unsigned int i;

	if (!mz0380_raw_bank_observe)
		return;

	dma_rmb();
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		struct mz0380_raw_probe_buf *b = &dev->raw_probe_bufs[i];
		u8 head[sizeof(b->observed_head)];

		if (!b->va)
			continue;
		memcpy(head, b->va, sizeof(head));
		if (b->observed_valid &&
		    !memcmp(head, b->observed_head, sizeof(head)))
			continue;

		memcpy(b->observed_head, head, sizeof(head));
		b->observed_valid = true;
		b->observed_changes++;
		pr_info_ratelimited("%s: M212 raw slot %u head changed (#%llu): %16phN\n",
				    dev->name, i,
				    (unsigned long long)b->observed_changes,
				    b->observed_head);
	}
}

static void
mz0380_drain_raw_probe_snapshot(struct mz0380_dev *dev,
				const struct mz0380_frame_event *snapshot)
{
	struct mz0380_raw_probe_buf *raw;
	struct mz0380_vb_buffer *vbuf;
	unsigned long flags;
	u32 idx = snapshot->token & 7;
	size_t len = 0;
	int ret;

	raw = &dev->raw_probe_bufs[idx];
	dev->raw_probe_events++;
	dev->raw_probe_slots_seen |= BIT(idx);
	raw->completions++;

	ret = mz0380_raw_probe_infer_length(dev, idx, &len);
	raw->last_extent = len;
	if (ret || len != MZ0380_RAW_PROBE_FRAME_SIZE) {
		dev->raw_probe_bad_extents++;
		dev->raw_probe_consecutive_full = 0;
		pr_info_ratelimited("%s: M209 raw completion #%llu token=%08x -> bank%u/op0x%02x slot%u extent=0x%zx (expected exactly 0x%x, ret=%d); slot left un-poisoned for an in-flight DMA\n",
				    dev->name,
				    (unsigned long long)dev->raw_probe_events,
				    snapshot->token,
				    idx / MZ0380_STREAM_NR_BUFS,
				    idx < MZ0380_STREAM_NR_BUFS ?
					MZ0380_CMD_SET_BUF_2 : MZ0380_CMD_SET_BUF_8,
				    idx % MZ0380_STREAM_NR_BUFS, len,
				    MZ0380_RAW_PROBE_FRAME_SIZE, ret);
		goto report;
	}

	dev->raw_probe_full_frames++;
	dev->raw_probe_consecutive_full++;
	pr_info("%s: M209 raw completion #%llu FULL: token=%08x -> bank%u/op0x%02x slot%u wrote exactly 0x%x bytes (consecutive=%u, slots_seen=0x%02x)\n",
		dev->name, (unsigned long long)dev->raw_probe_events,
		snapshot->token, idx / MZ0380_STREAM_NR_BUFS,
		idx < MZ0380_STREAM_NR_BUFS ?
			MZ0380_CMD_SET_BUF_2 : MZ0380_CMD_SET_BUF_8,
		idx % MZ0380_STREAM_NR_BUFS, MZ0380_RAW_PROBE_FRAME_SIZE,
		dev->raw_probe_consecutive_full, dev->raw_probe_slots_seen);

	if (!READ_ONCE(dev->streaming))
		goto repoison;

	spin_lock_irqsave(&dev->buf_lock, flags);
	vbuf = list_first_entry_or_null(&dev->buf_list,
					struct mz0380_vb_buffer, list);
	if (vbuf)
		list_del(&vbuf->list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
	if (!vbuf) {
		pr_info_ratelimited("%s: M209 full raw slot %u has no queued vb2 buffer; dropped and re-poisoned\n",
				    dev->name, idx);
		goto repoison;
	}

	{
		void *dst = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
		size_t plane = vb2_plane_size(&vbuf->vb.vb2_buf, 0);

		if (!dst || plane < MZ0380_RAW_PROBE_FRAME_SIZE) {
			pr_warn("%s: M209 raw frame does not fit vb2 plane %zu\n",
				dev->name, plane);
			vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, 0);
			vb2_buffer_done(&vbuf->vb.vb2_buf,
					VB2_BUF_STATE_ERROR);
			goto repoison;
		}
		dma_rmb();
		memcpy(dst, raw->va, MZ0380_RAW_PROBE_FRAME_SIZE);
		vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0,
				      MZ0380_RAW_PROBE_FRAME_SIZE);
	}

	vbuf->vb.vb2_buf.timestamp = snapshot->timestamp_ns;
	vbuf->vb.field = mz0380_current_field(dev);
	vbuf->vb.sequence = dev->video_sequence++;
	raw->delivered++;
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);

repoison:
	mz0380_raw_probe_buffer_repoison(dev, idx);
report:
	if (dev->raw_probe_events == MZ0380_RAW_PROBE_NR_BUFS)
		pr_info("%s: M209 raw discriminator first-eight summary: events=%llu exact_frames=%llu bad_extents=%llu consecutive_exact=%u slots_seen=0x%02x\n",
			dev->name,
			(unsigned long long)dev->raw_probe_events,
			(unsigned long long)dev->raw_probe_full_frames,
			(unsigned long long)dev->raw_probe_bad_extents,
			dev->raw_probe_consecutive_full,
			dev->raw_probe_slots_seen);
	if (!dev->raw_probe_success_reported &&
	    dev->raw_probe_consecutive_full >= MZ0380_RAW_PROBE_NR_BUFS &&
	    dev->raw_probe_slots_seen == GENMASK(MZ0380_RAW_PROBE_NR_BUFS - 1, 0)) {
		dev->raw_probe_success_reported = true;
		pr_info("%s: M209 raw discriminator SUCCESS: eight consecutive exact 0x%x writes covered all op02/op08 slots (slots_seen=0x%02x)\n",
			dev->name, MZ0380_RAW_PROBE_FRAME_SIZE,
			dev->raw_probe_slots_seen);
	}
}

/*
 * M217: hand one raw I420 frame to V4L2.
 *
 * Called on an ENCODED completion, which is the only completion this path gets:
 * the raw window never raises one of its own (M215 saw completions=0 while the
 * card was demonstrably writing whole frames). That is workable because the two
 * are driven by the same frame clock - the card writes the raw slot for frame N
 * and encodes frame N - so the encoded event is a usable proxy for "a raw slot
 * just changed", and the token tells us which.
 *
 * The slot may hold a whole frame or the 16-byte stub the card writes for every
 * frame its preview scheduler did not select (M214), so every completion is
 * tested and the stubs are simply skipped. With post_skip=29 that means 29 of
 * every 30 completions do nothing here.
 *
 * TEARING, and the assumption that guards it: an encoded completion is not a
 * statement that the raw DMA has finished, so in principle this could copy a
 * frame that is still being written. The sentinel test is what stops it - the
 * first sentinel sits at the LAST dword of the frame, so it can only read as
 * written once the transfer has reached the end. That holds if the card writes
 * the frame in ascending address order, which is what a linear DMA does and
 * what the extent measurements in M209/M215 are consistent with, but it has not
 * been proven directly. If torn frames ever show up, this is the assumption to
 * doubt first.
 */
/*
 * M222: hand one raw I420 frame to V4L2.
 *
 * Called on an ENCODED completion, which is the only completion this path gets:
 * the raw window raises none of its own (M215 measured completions=0 while the
 * card was demonstrably writing whole frames).
 *
 * TWO THINGS THIS DELIBERATELY DOES NOT DO, both learned the hard way.
 *
 * It does not use the completion's token to pick a slot. M217 did, on the
 * assumption that `token & 7` names the raw slot the way it names the encoded
 * one. The M212 head sampler says otherwise: over one run the four slots
 * changed 2, 219, 217 and 230 times, which is not four slots taking turns. So
 * a token-selected slot was frequently one the card had not just written -
 * stale, giving byte-identical duplicates, or mid-write, giving tears. All
 * slots are scanned instead, and any that carries a complete frame is taken.
 *
 * And it does not trust the completeness test to survive the copy. Passing the
 * sentinel test proves a whole frame was there when it was read; it says
 * nothing about whether the card starts overwriting that slot during the two
 * milliseconds the memcpy takes. So the sentinels are re-poisoned BEFORE the
 * copy and re-read after it: if the card wrote into the slot meanwhile it will
 * have put its own bytes back over them, and the frame is discarded rather than
 * delivered torn. That is what the flicker was - M220 fixed a partial frame
 * passing the test, and the flicker survived because it was never the only way
 * a torn frame could reach userspace.
 */
static bool mz0380_raw_deliver_slot(struct mz0380_dev *dev,
				    const struct mz0380_frame_event *snapshot,
				    u32 idx)
{
	struct mz0380_raw_probe_buf *raw = &dev->raw_probe_bufs[idx];
	struct mz0380_vb_buffer *vbuf;
	unsigned long flags;
	size_t frame = mz0380_raw_frame_bytes(dev);
	u32 sentinels[4];
	void *dst;
	size_t plane;

	if (!raw->va || !mz0380_raw_probe_frame_landed(dev, idx))
		return false;

	if (raw->last_token_valid &&
	    !memcmp(raw->last_head, raw->va, sizeof(raw->last_head)))
		dev->raw_dup_content++;

	raw->completions++;
	dev->raw_probe_events++;
	dev->raw_probe_slots_seen |= BIT(idx);
	dev->raw_probe_full_frames++;

	if (!READ_ONCE(dev->streaming)) {
		mz0380_raw_probe_sentinel_repoison(dev, idx);
		return false;
	}

	spin_lock_irqsave(&dev->buf_lock, flags);
	vbuf = list_first_entry_or_null(&dev->buf_list,
					struct mz0380_vb_buffer, list);
	if (vbuf)
		list_del(&vbuf->list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
	if (!vbuf) {
		dev->raw_frames_dropped++;
		mz0380_raw_probe_sentinel_repoison(dev, idx);
		return false;
	}

	dst = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
	plane = vb2_plane_size(&vbuf->vb.vb2_buf, 0);
	if (!dst || plane < frame) {
		dev->raw_frames_dropped++;
		pr_warn_ratelimited("%s: raw frame %zu bytes does not fit vb2 plane %zu\n",
				    dev->name, frame, plane);
		vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, 0);
		vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		mz0380_raw_probe_sentinel_repoison(dev, idx);
		return false;
	}

	/*
	 * M239: save the real pixels under the sentinels before arming.
	 *
	 * Poisoning the source and then copying the whole slot put four dwords
	 * of 0xa5/0x5a into EVERY delivered frame - measured at 600 of 600,
	 * all four offsets. Three of them land in the chroma planes, so each
	 * painted a small coloured blob at a fixed pixel; (960,674) is the one
	 * the operator saw "close to middle".
	 *
	 * The detector genuinely needs the poison present in the source while
	 * the copy runs, so it is not reordered. The real values are put back
	 * into the destination after the copy is known not to be torn.
	 */
	mz0380_raw_probe_sentinel_save(dev, idx, sentinels);
	mz0380_raw_probe_sentinel_repoison(dev, idx);
	dma_rmb();
	mz0380_raw_copy_frame(dev, dst, raw->va, frame);

	if (mz0380_raw_probe_frame_landed(dev, idx)) {
		/*
		 * The card overwrote the sentinels we had just poisoned, so it
		 * was writing this slot while we read it. What we copied is
		 * part old frame and part new - exactly the flicker. Drop it
		 * and leave the slot alone; the write in progress will finish
		 * and the next pass will find it complete.
		 */
		dev->raw_frames_torn++;
		spin_lock_irqsave(&dev->buf_lock, flags);
		list_add(&vbuf->list, &dev->buf_list);
		spin_unlock_irqrestore(&dev->buf_lock, flags);
		return false;
	}

	/* The copy is good; put back the pixels the sentinels were sitting on. */
	mz0380_raw_probe_sentinel_restore(dev, dst, sentinels);

	vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, frame);
	/*
	 * M224: stamp when this frame was taken, not when some other frame
	 * completed.
	 *
	 * This used to copy snapshot->timestamp_ns, which was defensible while
	 * M217 selected the slot FROM that completion's token. M222 stopped
	 * doing that - slots are now scanned and taken in rotation - so the
	 * completion that triggers a delivery is no longer the completion for
	 * the frame being delivered, and the timestamp belonged to a different
	 * frame entirely.
	 *
	 * The frames themselves are provably fine: a 89-frame capture had no
	 * poison anywhere, no tears, stable luma and correct colour, arriving
	 * in slot rotation. What a renderer had to work with was good pictures
	 * carrying other frames' timestamps, at ~50 fps against the 60 the node
	 * advertises. That presents as stutter, which is what "flicker" on
	 * every raw format but H.264 describes - H.264 delivers on the
	 * completion whose timestamp it stamps, so it never had the problem.
	 */
	vbuf->vb.vb2_buf.timestamp = ktime_get_ns();
	vbuf->vb.field = mz0380_current_field(dev);
	vbuf->vb.sequence = dev->video_sequence++;
	vbuf->vb.flags &= ~(V4L2_BUF_FLAG_PFRAME | V4L2_BUF_FLAG_BFRAME);
	vbuf->vb.flags |= V4L2_BUF_FLAG_KEYFRAME;

	if (READ_ONCE(dev->no_signal_active) &&
	    READ_ONCE(dev->signal_locked) &&
	    !READ_ONCE(dev->signal_recovering) &&
	    !READ_ONCE(dev->pipeline_reconfigure_pending))
		mz0380_no_signal_deactivate(dev);

	raw->delivered++;
	dev->raw_frames_delivered++;
	raw->last_token = snapshot->token;
	raw->last_token_valid = true;
	memcpy(raw->last_head, dst, sizeof(raw->last_head));
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	return true;
}

/*
 * M223: one frame per completion, taken round-robin, and say what was seen.
 *
 * M222 scanned slots 0..3 and delivered every one that carried a complete
 * frame. That is wrong in a way that looks exactly like the symptom being
 * chased: if two slots hold frames captured at different moments, delivering
 * both in SLOT order emits them out of TIME order, and out-of-order frames
 * flicker just like torn ones do. Nothing here knows which of two ready slots
 * is the newer.
 *
 * So take at most one per completion, which matches the frame clock the
 * completions arrive on, and start each scan after the slot taken last, so the
 * four are consumed in rotation rather than always preferring slot 0. If the
 * card fills them in rotation - which is what a four-slot ring does - that
 * consumes them in the order they were produced without needing to know the
 * order.
 *
 * The trace is here because this has now been diagnosed wrong three times from
 * inspection alone. `landed` is the bitmap of slots holding a complete frame
 * when the scan ran; more than one bit set means frames are queueing up and
 * ordering matters, exactly one means the pacing is right, and zero means the
 * completion arrived before the DMA finished.
 */
static void
mz0380_drain_raw_deliver(struct mz0380_dev *dev,
			 const struct mz0380_frame_event *snapshot)
{
	unsigned int i, slot;
	u8 landed = 0;

	/*
	 * M229: a slot must be FILLED, not merely touched.
	 *
	 * mz0380_raw_probe_frame_landed only asks whether the poison is gone,
	 * and M226 showed on hardware that the card clears a slot to black
	 * before filling it. The clear removes every sentinel at once, so
	 * "landed" goes true the moment the clear lands and the copy took
	 * whatever had been written so far - an all-black frame when it ran
	 * early, picture over black when it ran mid-fill.
	 *
	 * M228 tried to solve this with time instead, requiring a slot to be
	 * seen landed on two consecutive scans. Its own counter falsified it:
	 * one deferral in 1200 frames, because a slot the card cleared stays
	 * landed indefinitely - only the DELIVERED slot is re-poisoned - so the
	 * second observation was always already satisfied. The same run
	 * recorded 1947 completions with more than one slot ready against zero
	 * under M223, which is that fact seen from the other side. It is gone.
	 */
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		if (!dev->raw_probe_bufs[i].va ||
		    !mz0380_raw_probe_frame_landed(dev, i))
			continue;
		if (!mz0380_raw_probe_frame_filled(dev, i)) {
			/* Cleared, not yet filled. Leave it to finish. */
			dev->raw_incomplete_tail++;
			continue;
		}
		/*
		 * M238: luma finishing does not mean chroma has. I420 is
		 * planar and the card fills ascending, so a slot can hold a
		 * complete picture with its colour planes still cleared -
		 * which arrives as an occasional frame with correct detail
		 * and a magenta or green cast.
		 */
		if (!mz0380_raw_probe_chroma_filled(dev, i)) {
			dev->raw_incomplete_chroma++;
			continue;
		}
		landed |= BIT(i);
	}

	if (!landed) {
		dev->raw_probe_stub_frames++;
		return;
	}
	if (hweight8(landed) > 1)
		dev->raw_multi_landed++;

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		slot = (dev->raw_next_slot + i) % MZ0380_STREAM_NR_BUFS;
		if (!(landed & BIT(slot)))
			continue;
		if (mz0380_raw_deliver_slot(dev, snapshot, slot)) {
			dev->raw_next_slot = (slot + 1) %
					     MZ0380_STREAM_NR_BUFS;
			dev_dbg_ratelimited(&dev->pci->dev, "raw scan landed=0x%x took slot %u (multi=%llu torn=%llu unfilled=%llu)\n",
					    landed, slot,
					    (unsigned long long)dev->raw_multi_landed,
					    (unsigned long long)dev->raw_frames_torn,
					    (unsigned long long)dev->raw_incomplete_tail);
		}
		return;
	}
}

static void
mz0380_drain_frame_snapshot(struct mz0380_dev *dev,
			    const struct mz0380_frame_event *snapshot)
{
	struct mz0380_vb_buffer *vbuf;
	unsigned long flags;
	u32 idx = snapshot->token & 7;
	u8 *payload;
	size_t len;
	int ret;

	if (mz0380_raw_bank_probe) {
		mz0380_drain_raw_probe_snapshot(dev, snapshot);
		return;
	}

	if (mz0380_h264_probe) {
		/*
		 * M217: raw_deliver forwards the raw banks instead of the
		 * encoded output. The encoder still runs - it is what makes the
		 * card produce raw at all - its bitstream is just not delivered.
		 */
		if (dev->deliver_raw) {
			/*
			 * M231: this completion IS the proof the producer is
			 * alive, so record it as such.
			 *
			 * The receiver monitor decides a configured pipeline
			 * has gone silent by comparing last_h264_frame_stamp
			 * against hotplug_stall_ms, and every one of the
			 * writes to that stamp lives in the encoded delivery
			 * path - which this branch skips. In a raw session the
			 * stamp therefore froze at STREAMON and went stale
			 * 1500 ms later, every time, on a perfectly live
			 * source.
			 *
			 * What follows from that is not cosmetic. The monitor
			 * responds by declaring NO SIGNAL, entering recovery,
			 * and RE-ARMING HDMI acquisition - a write to the
			 * MST3367 - in the middle of the capture, roughly
			 * every 1.5 seconds for as long as raw is streaming. A
			 * counter watch caught it flapping between "SHOWING
			 * (no usable source)" and "not shown" for an entire
			 * 50-second raw session while h264deliv stayed at 0,
			 * where two H.264 sessions either side of it held
			 * recovery idle throughout. M225 keeps the placeholder
			 * frames off a raw node, so what reached the screen was
			 * not the placeholder but the receiver being disturbed
			 * mid-capture.
			 */
			WRITE_ONCE(dev->last_h264_frame_stamp, jiffies);
			mz0380_drain_raw_deliver(dev, snapshot);
			return;
		}
		mz0380_drain_h264_snapshot(dev, snapshot);
		mz0380_raw_observe_sample(dev);
		return;
	}

	if (idx >= MZ0380_STREAM_NR_BUFS) {
		pr_warn_ratelimited("%s: frame dropped: 3-bit token=%08x selects unprogrammed slot %u (only 0..%u are backed; SET_BUF_8 second set not allocated), event=%08x payload=%08x/%08x/%08x enc=%08x\n",
				    dev->name, snapshot->token, idx,
				    MZ0380_STREAM_NR_BUFS - 1, snapshot->event,
				    snapshot->payload[0], snapshot->payload[1],
				    snapshot->payload[2], snapshot->enc_status);
		return;
	}

	payload = dev->stream_bufs[idx].va;
	if (!payload) {
		pr_warn_ratelimited("%s: frame dropped: token %u has no DMA buffer; enc_stat ACK follows drain batch\n",
				    dev->name, idx);
		return;
	}
	if (!READ_ONCE(dev->streaming)) {
		pr_info_ratelimited("%s: frame token %u arrived during streamoff; dropped, re-poisoned and acknowledged\n",
				    dev->name, idx);
		goto repoison;
	}

	ret = mz0380_infer_frame_length(dev, idx, &len);
	if (ret || !len || len >= MZ0380_STREAM_BUF_SIZE) {
		pr_warn_ratelimited("%s: frame dropped: no bounded poison-suffix length (ret=%d event=%08x token=%08x payload counters=%08x/%08x/%08x enc=%08x head=%02x %02x %02x %02x); mailbox counters are not byte lengths\n",
				    dev->name, ret, snapshot->event, snapshot->token,
				    snapshot->payload[0], snapshot->payload[1],
				    snapshot->payload[2], snapshot->enc_status,
				    payload[0], payload[1], payload[2], payload[3]);
		goto repoison;
	}

	/*
	 * M160: the same completeness rule the poll-drain has had since M115,
	 * on the path that actually fires now.
	 *
	 * The poison boundary marks how far the DMA has GOT, not that it has
	 * finished. M115 learned that on the poll path and guarded it there;
	 * this path was left unguarded because `frame_events` was 0 in every
	 * run until fw=7 (M159), so it had never once executed against a live
	 * producer. It then did the exact thing M115 warns about: delivered a
	 * torn 16-byte prefix at ~60 Hz and re-poisoned the buffer underneath
	 * an in-flight transfer, which also destroys the rest of the frame.
	 *
	 * `return` rather than `goto repoison` is the whole point. Leave the
	 * partial buffer exactly as it is and the card finishes writing it;
	 * a later event or the poll-drain then sees a complete frame.
	 */
	if (mz0380_event_require_complete) {
		size_t want = mz0380_expect_frame_bytes ?:
			      (size_t)dev->capture.source_width *
			      dev->capture.source_height * 3 / 2;

		if (want && len < want) {
			pr_info_ratelimited("%s: frame token %u holds %zu of %zu bytes on the completion event - DMA still in flight, left un-poisoned (M160; event_require_complete=0 to deliver torn prefixes)\n",
					    dev->name, idx, len, want);
			return;
		}
	}

	/*
	 * M168: "H.264 length" was wrong on the path that actually runs. The
	 * poison-boundary inference is format-agnostic and what it measured
	 * here, on every capture this project has, is a raw I420 frame.
	 */
	pr_info_ratelimited("%s: frame token %u inferred payload length=%zu from the 4-byte poison boundary (tail collision risk 2^-32; payload counters %08x/%08x/%08x were not used)\n",
				    dev->name, idx, len, snapshot->payload[0],
				    snapshot->payload[1], snapshot->payload[2]);

	spin_lock_irqsave(&dev->buf_lock, flags);
	vbuf = list_first_entry_or_null(&dev->buf_list,
					struct mz0380_vb_buffer, list);
	if (vbuf)
		list_del(&vbuf->list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
	if (!vbuf) {
		pr_info_ratelimited("%s: frame token %u length=%zu has no queued vb2 buffer; dropped and re-poisoned\n",
				    dev->name, idx, len);
		goto repoison;
	}

	{
		void *dst = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
		size_t plane = vb2_plane_size(&vbuf->vb.vb2_buf, 0);

		if (!dst || len > plane) {
			pr_warn_ratelimited("%s: inferred frame %zu bytes does not fit vb2 plane %zu; buffer failed\n",
					    dev->name, len, plane);
			vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, 0);
			vb2_buffer_done(&vbuf->vb.vb2_buf,
					VB2_BUF_STATE_ERROR);
			goto repoison;
		}
		dma_rmb();
		memcpy(dst, payload, len);
		vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, len);
	}

	vbuf->vb.vb2_buf.timestamp = snapshot->timestamp_ns;
	vbuf->vb.field = mz0380_current_field(dev);	/* M172 */
	vbuf->vb.sequence = dev->video_sequence++;
	dev->stream_bufs[idx].delivered++;	/* M168: see the extent report */
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);

repoison:
	mz0380_frame_buffer_repoison(dev, idx);
}

/*
 * M111: the poll-drain fallback.
 *
 * The card writes a whole frame and then never tells us. mz0380_drain_frame_snapshot()
 * already knows how to turn "buffer idx holds a frame" into a vb2 delivery - it
 * infers the length from the poison suffix, copies, and re-poisons. The only
 * thing missing on the real path is something to call it, because the
 * completion event that normally supplies the token has never fired.
 *
 * So synthesise the snapshot. token = the buffer index, timestamp = now,
 * everything else zero (the drain only reads those fields for diagnostics).
 * Re-poisoning inside the drain is what makes this idempotent: a frame is
 * picked up on the first pass that sees it and cannot be delivered twice.
 *
 * mz0380_infer_frame_length() returns -ENODATA for a fully-poisoned buffer, so
 * it doubles as the "is there anything here" test and no separate dirty check
 * is needed.
 */
static int mz0380_poll_drain_thread(void *data)
{
	struct mz0380_dev *dev = data;
	unsigned long next_kick = jiffies;
	unsigned long last_delivery = 0;
	bool stall_reported = false;
	unsigned int delivered = 0;
	unsigned int kicks = 0;
	u32 tok[3] = {};
	bool tok_seeded = false;
	unsigned int tok_changes = 0;

	while (!kthread_should_stop()) {
		bool handled = false;
		bool kick_due = false;
		unsigned int idx;

		/*
		 * M138: the producer watch.  BAR0 0x40/0x44/0x48 are updated by
		 * the card's store_channel_done() before its credit test, so
		 * they advance on every card-side frame completion even when no
		 * EVENT is raised.  Until now they were only sampled from
		 * mz0380_handle_event_snapshot(), which never runs after the
		 * first frame - so nobody has ever watched them over time.
		 *
		 *   stays put  -> the encoder wrote channel_done ONCE and is
		 *                 parked in SSM_ReleaseAndReceive; the card's
		 *                 producer is stalled, upstream of tinyvenc5.
		 *   ticks      -> the producer is fine and the loss is ours.
		 *
		 * Three MMIO reads per poll interval, no mailbox traffic, no
		 * I2C: this cannot perturb capture the way M74's watch did.
		 */
		if (READ_ONCE(dev->streaming)) {
			u32 now[3];

			now[0] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD0);
			now[1] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1);
			now[2] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2);

			if (!tok_seeded) {
				tok_seeded = true;
				pr_info("%s: producer watch: baseline 40=%08x 44=%08x 48=%08x\n",
					dev->name, now[0], now[1], now[2]);
			} else if (memcmp(now, tok, sizeof(tok))) {
				tok_changes++;
				pr_info("%s: producer watch: change #%u 40=%08x 44=%08x 48=%08x (was %08x/%08x/%08x)\n",
					dev->name, tok_changes, now[0], now[1],
					now[2], tok[0], tok[1], tok[2]);
			}
			memcpy(tok, now, sizeof(tok));
		}


		for (idx = 0; idx < MZ0380_STREAM_NR_BUFS; idx++) {
			struct mz0380_frame_event snapshot = {};
			size_t want;
			size_t len;

			if (!READ_ONCE(dev->streaming))
				break;
			if (mz0380_infer_frame_length(dev, idx, &len) || !len)
				continue;

			/*
			 * M115: only deliver a COMPLETE frame. The poison
			 * boundary marks how far the DMA has got, not that it
			 * has finished, so polling a burst in flight yields a
			 * torn prefix - the first run delivered 794368 bytes
			 * and then the real 3110400, and ffplay rejected the
			 * fragment. The nosg path has always had this check
			 * (mz0380_nosg_frame_landed); the real path needs it
			 * for the same reason.
			 *
			 * The expected size is exact and known: the card
			 * writes width*height*3/2 of 4:2:0. Anything short is
			 * a burst still in progress - leave it alone and it
			 * will be complete on a later pass.
			 */
			want = mz0380_expect_frame_bytes ?:
			       (size_t)dev->capture.source_width *
			       dev->capture.source_height * 3 / 2;
			if (want && len < want) {
				pr_info_ratelimited("%s: poll-drain: buf %u holds %zu of %zu bytes - DMA still in flight, waiting\n",
						    dev->name, idx, len, want);
				continue;
			}

			snapshot.token = idx;
			snapshot.timestamp_ns = ktime_get_ns();
			pr_info_ratelimited("%s: poll-drain: buf %u holds %zu bytes with no completion event; delivering\n",
					    dev->name, idx, len);
			mz0380_drain_frame_snapshot(dev, &snapshot);
			delivered++;
			last_delivery = jiffies;
			handled = true;
		}

		/*
		 * M117: hand the slot back. mz0380_enc_stat_ack()'s own comment
		 * says it outright - "without this ack the card's encoder
		 * produces exactly one bitstream and then skips every
		 * subsequent frame" - which is precisely the cadence we
		 * measured: one frame per stream, buffers 1-3 never touched.
		 *
		 * mz0380_dma_drain_video() acks after its drain batch, but the
		 * poll path calls mz0380_drain_frame_snapshot() directly and so
		 * skipped it. The card sets enc_stat and never clears it
		 * itself; the driver clears it once at stream start (M40) and,
		 * until now, never again on this path.
		 */
		if (handled) {
			mz0380_enc_stat_ack(dev);
			/*
			 * M118: and the other half of what a real completion
			 * would have done. enc_stat alone (M117) did not move
			 * the cadence off one frame, so the slot handshake is
			 * not the gate. This re-arms the card's one-shot
			 * completion credit, which nothing has ever restored on
			 * this path because the ISR that normally does it runs
			 * only for an event that never fires.
			 */
			if (mz0380_poll_drain_credit)
				mz0380_credit_rearm(dev);

			/*
			 * M120: ask for the NEXT frame, once per frame we just
			 * consumed. tinyvenc5 delivers one frame per
			 * sysfs_notify on /sys/vpl_pciep/epint and op6's ep.ko
			 * handler does nothing but that notify (M22).
			 *
			 * M119 fired this free-running from stream start and it
			 * was WORSE than not firing at all - zero frames, and
			 * the receiver dropped its lock. Our own comment at the
			 * op6 send site says why: op6 must land after the
			 * freshly forked tinyvenc5 has exec'd and consumed the
			 * SET_VIC it reads first, "rather than racing its
			 * start-up read". A timer starting at t=0 races exactly
			 * that, 62 times a second.
			 *
			 * Kicking only after a delivered frame makes the race
			 * impossible: the first kick cannot happen until the
			 * first frame has already arrived. op6_kick_ms is now a
			 * minimum spacing, not a period.
			 */
			kick_due = true;
		}

		/*
		 * M126: keep kicking after the FIRST delivery, not only after
		 * each one. A kick that fires only inside the handled branch
		 * gives exactly one kick per frame, so a one-frame stream is a
		 * one-kick experiment - which cannot tell "the card ignored the
		 * wake-up" apart from "we only ever asked once". The M119/M120
		 * hazard was kicking BEFORE the first frame, racing tinyvenc5's
		 * start-up read of SET_VIC; gating on delivered > 0 keeps that
		 * impossible while letting the wake-up repeat.
		 */
		if (mz0380_kick_repeat && delivered)
			kick_due = true;

		if (kick_due && mz0380_op6_kick_ms &&
		    time_after_eq(jiffies, next_kick)) {
			/*
			 * M126: 0x06 by default; 0x2f / 0x09 are the same
			 * epint notify without the audio_ctrl side effect
			 * (M125).
			 */
			int kick_ret = mz0380_send_command(dev,
							   mz0380_kick_opcode & 0xff,
							   NULL, 0, NULL, 0);

			if (!kicks)
				pr_info("%s: poll-drain: first kick op 0x%02x ret=%d\n",
					dev->name, mz0380_kick_opcode & 0xff,
					kick_ret);
			kicks++;
			next_kick = jiffies +
				msecs_to_jiffies(mz0380_op6_kick_ms);
		}

		/*
		 * M168: the stream is over, so say so rather than blocking the
		 * reader forever. See the mz0380_stall_eos_ms comment in
		 * mz0380-core.c for why one frame is the hardware's bound.
		 */
		if (delivered && !stall_reported && mz0380_stall_eos_ms &&
		    READ_ONCE(dev->streaming) &&
		    time_after(jiffies,
			       last_delivery +
			       msecs_to_jiffies(mz0380_stall_eos_ms))) {
			stall_reported = true;
			pr_info("%s: poll-drain: %u frame(s) delivered and nothing for %u ms - signalling end of stream (DQBUF will return -EIO). fw=%u is a single-shot grabber (M166); stop and restart the stream for another frame, or set stall_eos_ms=0 to block instead\n",
				dev->name, delivered, mz0380_stall_eos_ms,
				mz0380_vic_fw);
			vb2_queue_error(&dev->vb_queue);
		}

		msleep_interruptible(max_t(unsigned int, 1,
					   mz0380_poll_drain_ms));
	}

	pr_info("%s: poll-drain stopped after %u deliveries, %u kicks (op 0x%02x); producer watch saw %u change(s), final 40=%08x 44=%08x 48=%08x\n",
		dev->name, delivered, kicks, mz0380_kick_opcode & 0xff,
		tok_changes, tok[0], tok[1], tok[2]);
	return 0;
}

void mz0380_poll_drain_start(struct mz0380_dev *dev)
{
	struct task_struct *task;

	if (!mz0380_poll_drain_ms || dev->poll_task || mz0380_stream_nosg ||
	    mz0380_h264_probe || mz0380_raw_bank_probe)
		return;

	task = kthread_run(mz0380_poll_drain_thread, dev,
			   "mz0380-polldrain/%u", dev->nr);
	if (IS_ERR(task)) {
		pr_warn("%s: poll-drain thread failed to start (%ld)\n",
			dev->name, PTR_ERR(task));
		return;
	}
	dev->poll_task = task;
	pr_info("%s: poll-drain armed every %u ms - frames the card has already written will be delivered without a completion event (M111)\n",
		dev->name, mz0380_poll_drain_ms);
}

void mz0380_poll_drain_stop(struct mz0380_dev *dev)
{
	if (!dev->poll_task)
		return;
	kthread_stop(dev->poll_task);
	dev->poll_task = NULL;
}

void mz0380_dma_drain_video(struct mz0380_dev *dev)
{
	struct mz0380_frame_event snapshot;
	unsigned long event_flags, flags;
	unsigned int i;
	bool handled = false;

	/*
	 * Keep drain_scheduled asserted until the queue-empty observation and the
	 * ownership ACK are one transaction.  Merely clearing it in event_pop()
	 * leaves a window where the ACK can free a token that the pre-ACK hook has
	 * just snapshotted but this worker has not copied yet.
	 */
	for (;;) {
		bool ack_deferred;
		u8 drop_tokens;

		/*
		 * Drain/copy/re-poison the complete batch before the one ownership
		 * ACK.  An overflowed newest event therefore cannot release the
		 * endpoint early and overwrite an older queued token before its copy.
		 */
		while (mz0380_frame_event_pop(dev, &snapshot)) {
			handled = true;
			mz0380_drain_frame_snapshot(dev, &snapshot);
		}

		spin_lock_irqsave(&dev->frame_event_lock, flags);
		ack_deferred = dev->frame_event_ack_deferred;
		dev->frame_event_ack_deferred = false;
		drop_tokens = dev->frame_event_drop_tokens;
		dev->frame_event_drop_tokens = 0;
		spin_unlock_irqrestore(&dev->frame_event_lock, flags);

		for (i = 0; i < (mz0380_raw_bank_probe ?
					MZ0380_RAW_PROBE_NR_BUFS :
					MZ0380_STREAM_NR_BUFS); i++) {
			if (drop_tokens & BIT(i)) {
				if (mz0380_raw_bank_probe)
					mz0380_raw_probe_buffer_repoison(dev, i);
				else
					mz0380_frame_buffer_repoison(dev, i);
			}
		}
		handled |= ack_deferred || drop_tokens;

		/*
		 * The core calls our snapshot hook while holding event_lock, and the
		 * hook then takes frame_event_lock.  Take the same lock order here so
		 * no snapshot can sit between the final empty check and the whole-word
		 * enc_stat ACK.  If an event arrived while we copied/re-poisoned, loop
		 * without clearing drain_scheduled; schedule_work() coalescing is then
		 * harmless because this invocation owns the retry.
		 */
		spin_lock_irqsave(&dev->event_lock, event_flags);
		spin_lock(&dev->frame_event_lock);
		if (dev->frame_event_tail != dev->frame_event_head ||
		    dev->frame_event_ack_deferred ||
		    dev->frame_event_drop_tokens) {
			spin_unlock(&dev->frame_event_lock);
			spin_unlock_irqrestore(&dev->event_lock, event_flags);
			continue;
		}
		dev->frame_event_drain_scheduled = false;
		if (handled)
			mz0380_enc_stat_ack(dev);
		spin_unlock(&dev->frame_event_lock);
		spin_unlock_irqrestore(&dev->event_lock, event_flags);
		return;
	}
}

void mz0380_drain_work_fn(struct work_struct *w)
{
	struct mz0380_dev *dev = container_of(w, struct mz0380_dev, drain_work);

	mz0380_dma_drain_video(dev);
}
