// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 completion drain: H.264 and raw delivery into vb2 buffers.
 */

#include "mz0380-dma-internal.h"

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

static struct mz0380_vb_buffer *mz0380_take_vbuf(struct mz0380_dev *dev)
{
	struct mz0380_vb_buffer *vbuf;
	unsigned long flags;

	spin_lock_irqsave(&dev->buf_lock, flags);
	vbuf = list_first_entry_or_null(&dev->buf_list,
					struct mz0380_vb_buffer, list);
	if (vbuf)
		list_del(&vbuf->list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
	return vbuf;
}

static void mz0380_return_vbuf(struct mz0380_dev *dev,
			       struct mz0380_vb_buffer *vbuf)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->buf_lock, flags);
	list_add(&vbuf->list, &dev->buf_list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
}

static void mz0380_fail_vbuf(struct mz0380_vb_buffer *vbuf)
{
	vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, 0);
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
}

/* The placeholder may end once a live frame arrives on a validated source. */
static bool mz0380_live_source_confirmed(struct mz0380_dev *dev)
{
	return READ_ONCE(dev->no_signal_active) &&
	       READ_ONCE(dev->signal_locked) &&
	       !READ_ONCE(dev->signal_recovering) &&
	       !READ_ONCE(dev->pipeline_reconfigure_pending);
}

/* --- H.264 --------------------------------------------------------------- */

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

/* Keep the latest SPS and the PPS that follow it, Annex-B prefixes included. */
static void mz0380_h264_cache_parameter_set(struct mz0380_dev *dev,
					    const u8 *nal, size_t bytes,
					    u8 type, bool *refreshed)
{
	if (type == 7) {
		dev->h264_parameter_sets_len = 0;
		*refreshed = true;
	}
	if ((*refreshed || !dev->h264_parameter_sets_len) &&
	    bytes <= MZ0380_H264_PARAMETER_SETS_MAX -
		     dev->h264_parameter_sets_len) {
		memcpy(dev->h264_parameter_sets + dev->h264_parameter_sets_len,
		       nal, bytes);
		dev->h264_parameter_sets_len += bytes;
	}
}

/*
 * One pass over an access unit: cache its SPS/PPS and classify it. The walk
 * stops at the first slice - every NAL a decoder needs ahead of the picture
 * precedes it, and the slice data is the bulk of the access unit. (Two full
 * scans per frame used to do this.)
 */
static struct mz0380_h264_au_class
mz0380_h264_scan_au(struct mz0380_dev *dev, const u8 *payload, size_t len)
{
	struct mz0380_h264_au_class c = { 0 };
	bool refreshed = false;
	size_t prefix_len;
	size_t start = mz0380_h264_find_start_code(payload, len, 0,
						   &prefix_len);

	while (start < len) {
		size_t nal = start + prefix_len;
		size_t next_prefix_len = 0;
		size_t next;
		u8 type;

		if (nal >= len)
			break;
		type = payload[nal] & 0x1f;
		if (type >= 1 && type <= 5) {
			c.has_vcl = true;
			c.has_idr = type == 5;
			break;
		}
		next = mz0380_h264_find_start_code(payload, len, nal + 1,
						   &next_prefix_len);
		if (type == 7 || type == 8) {
			c.has_parameter_set = true;
			if (dev->h264_parameter_sets)
				mz0380_h264_cache_parameter_set(
					dev, payload + start, next - start,
					type, &refreshed);
		}
		start = next;
		prefix_len = next_prefix_len;
	}
	return c;
}

/*
 * A receiver reconnect can resume the encoder in the middle of its old GOP.
 * Feeding those dependent pictures to a decoder produces smeared first frames
 * and frozen decoders, so parameter-set-only access units still pass but
 * dependent pictures are held until an IDR establishes a random-access point.
 */
static bool mz0380_h264_recovery_accept(struct mz0380_dev *dev,
					const struct mz0380_h264_au_class *c,
					bool *prepend_parameter_sets)
{
	unsigned long now = jiffies;
	unsigned long last = READ_ONCE(dev->last_h264_frame_stamp);

	*prepend_parameter_sets = false;
	if (READ_ONCE(dev->no_signal_active)) {
		/*
		 * Stale card pictures or standalone live SPS/PPS must not
		 * toggle the decoder away from the placeholder. The switch is
		 * atomic at a receiver-proven live IDR.
		 */
		if (!READ_ONCE(dev->signal_locked) ||
		    READ_ONCE(dev->signal_recovering) || !c->has_idr ||
		    READ_ONCE(dev->pipeline_reconfigure_pending) ||
		    READ_ONCE(dev->pipeline_start_failed)) {
			if (c->has_vcl)
				WRITE_ONCE(dev->last_h264_frame_stamp, now);
			return false;
		}
	}
	if (!c->has_vcl)
		return true;

	if (last && time_after_eq(now, last +
				  msecs_to_jiffies(MZ0380_HOTPLUG_STALL_MS))) {
		WRITE_ONCE(dev->h264_waiting_for_idr, true);
		dev_dbg(&dev->pci->dev,
			"H.264 resumed after a %ums gap; discarding dependent pictures until IDR\n",
			jiffies_to_msecs(now - last));
	}
	WRITE_ONCE(dev->last_h264_frame_stamp, now);

	if (!READ_ONCE(dev->h264_waiting_for_idr))
		return true;
	if (!c->has_idr)
		return false;

	*prepend_parameter_sets = !c->has_parameter_set &&
				  dev->h264_parameter_sets_len;
	return true;
}

/*
 * The card's window-1 slot header, validated before any byte reaches
 * userspace. tinyvenc7 reports the slot in BAR0+0x44's low nibble (zero-based)
 * and each slot starts with a 4 KiB header:
 *
 *   +0x00 encoded byte count   +0x0c one-based slot identity
 *   +0x10 encoded byte count again   +0x1000 Annex-B access unit
 *
 * Returns the access unit length, 0 for a slot the card has not written yet
 * (header and prefix still poison - expected, retry on the next completion),
 * or -EINVAL for a malformed one.
 */
static int mz0380_h264_slot_length(struct mz0380_dev *dev, u32 idx,
				   const u8 **payload)
{
	const u32 *header = dev->h264_bufs[idx].va;
	const u32 poison = 0x01010101u * MZ0380_H264_POISON_BYTE;
	const u8 *p = (const u8 *)header + 4096;
	u32 len, len_copy, slot;

	dma_rmb();
	len = READ_ONCE(header[0]);
	slot = READ_ONCE(header[3]);
	len_copy = READ_ONCE(header[4]);
	*payload = p;

	if (len == poison && len_copy == len && slot == len &&
	    get_unaligned((const u32 *)p) == poison)
		return 0;

	if (!len || len != len_copy || slot != idx + 1 ||
	    len > MZ0380_H264_SET_BUF_SIZE - 4096 ||
	    !((p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) ||
	      (p[0] == 0 && p[1] == 0 && p[2] == 1))) {
		pr_info_ratelimited("%s: H.264 slot %u not complete: len=%u copy=%u identity=%u prefix=%02x %02x %02x %02x\n",
				    dev->name, idx, len, len_copy, slot,
				    p[0], p[1], p[2], p[3]);
		return -EINVAL;
	}
	return len;
}

static void mz0380_h264_deliver(struct mz0380_dev *dev,
				const struct mz0380_frame_event *snapshot,
				u32 idx, const u8 *payload, size_t len,
				const struct mz0380_h264_au_class *c)
{
	struct mz0380_vb_buffer *vbuf;
	bool prepend_parameter_sets;
	size_t prefix_bytes, plane;
	void *dst;

	if (!mz0380_h264_recovery_accept(dev, c, &prepend_parameter_sets)) {
		if (READ_ONCE(dev->no_signal_active))
			dev->h264_frames_suppressed++;
		else
			dev->h264_frames_dropped++;
		return;
	}
	prefix_bytes = prepend_parameter_sets ?
		       dev->h264_parameter_sets_len : 0;

	/* Never take a VB2 buffer once STREAMOFF has published detachment. */
	if (!READ_ONCE(dev->streaming)) {
		dev->h264_frames_discarded++;
		return;
	}
	vbuf = mz0380_take_vbuf(dev);
	if (!vbuf) {
		dev->h264_frames_dropped++;
		return;
	}

	dst = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
	plane = vb2_plane_size(&vbuf->vb.vb2_buf, 0);
	if (!dst || len + prefix_bytes > plane) {
		dev->h264_frames_dropped++;
		pr_warn_ratelimited("%s: H.264 access unit %zu bytes + %zu cached SPS/PPS bytes does not fit vb2 plane %zu\n",
				    dev->name, len, prefix_bytes, plane);
		mz0380_fail_vbuf(vbuf);
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
	vbuf->vb.flags |= c->has_idr ? V4L2_BUF_FLAG_KEYFRAME :
				       V4L2_BUF_FLAG_PFRAME;
	if (c->has_idr && READ_ONCE(dev->h264_waiting_for_idr)) {
		WRITE_ONCE(dev->h264_waiting_for_idr, false);
		dev_dbg(&dev->pci->dev,
			"clean H.264 IDR received; encoded delivery is synchronized\n");
	}
	if (c->has_idr && mz0380_live_source_confirmed(dev))
		mz0380_no_signal_deactivate(dev);
	dev->h264_bufs[idx].delivered++;
	dev->h264_frames_delivered++;
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

static void
mz0380_drain_h264_snapshot(struct mz0380_dev *dev,
			   const struct mz0380_frame_event *snapshot)
{
	struct mz0380_h264_au_class c;
	const u8 *payload;
	u32 idx = snapshot->payload0 & 0xf;
	int len;

	if (idx >= MZ0380_STREAM_NR_BUFS || !dev->h264_bufs[idx].va)
		return;		/* the token seed, before the first report */
	/* Raw-preview events may repeat the last encoded token. */
	if (dev->h264_last_token_valid && dev->h264_last_token == idx)
		return;

	len = mz0380_h264_slot_length(dev, idx, &payload);
	if (len <= 0)
		return;

	/* The token is now consumed even if userspace supplied no buffer. */
	dev->h264_last_token = idx;
	dev->h264_last_token_valid = true;
	c = mz0380_h264_scan_au(dev, payload, len);

	mutex_lock(&dev->delivery_lock);
	if (READ_ONCE(dev->streaming)) {
		mz0380_h264_deliver(dev, snapshot, idx, payload, len, &c);
	} else {
		/* Detached persistent pipeline: keep the liveness stamp. */
		if (c.has_vcl)
			WRITE_ONCE(dev->last_h264_frame_stamp, jiffies);
		dev->h264_frames_discarded++;
	}
	mutex_unlock(&dev->delivery_lock);
}

/* --- raw ------------------------------------------------------------------ */

/*
 * M222: hand one raw frame from slot @idx to V4L2. Called with delivery_lock
 * held, so it cannot interleave with a NO SIGNAL picture.
 *
 * It does not trust the completeness test to survive the copy: the sentinels
 * are re-poisoned BEFORE the copy and re-read after it, and if the card wrote
 * any of them meanwhile the frame is part old and part new and is discarded.
 */
static bool mz0380_raw_deliver_slot(struct mz0380_dev *dev, u32 idx)
{
	struct mz0380_raw_buf *raw = &dev->raw_bufs[idx];
	bool neutral = raw->neutral_since_ns != 0;
	struct mz0380_vb_buffer *vbuf;
	size_t frame = mz0380_raw_frame_bytes(dev);
	u32 sentinels[4];
	u64 timestamp;
	void *dst;

	if (raw->last_head_valid &&
	    !memcmp(raw->last_head, raw->va, sizeof(raw->last_head)))
		dev->raw_dup_content++;	/* M242: a source-rate indicator */

	if (!READ_ONCE(dev->streaming)) {
		mz0380_raw_sentinel_repoison(dev, idx);
		return false;
	}

	vbuf = mz0380_take_vbuf(dev);
	if (!vbuf) {
		dev->raw_frames_dropped++;
		mz0380_raw_sentinel_repoison(dev, idx);
		return false;
	}

	dst = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
	if (!dst || vb2_plane_size(&vbuf->vb.vb2_buf, 0) < frame) {
		dev->raw_frames_dropped++;
		pr_warn_ratelimited("%s: raw frame %zu bytes does not fit the vb2 plane\n",
				    dev->name, frame);
		mz0380_fail_vbuf(vbuf);
		mz0380_raw_sentinel_repoison(dev, idx);
		return false;
	}

	/*
	 * M224: stamp when the frame was taken - and before the 3 MB copy, so
	 * the copy's own duration and cache behaviour stay out of the timeline.
	 */
	timestamp = ktime_get_ns();
	mz0380_raw_sentinel_save(dev, idx, sentinels);
	mz0380_raw_sentinel_repoison(dev, idx);
	dma_rmb();
	mz0380_raw_copy_frame(dev, dst, raw->va, frame);

	if (mz0380_raw_sentinel_rewritten(dev, idx)) {
		/* The card was writing this slot while we read it. */
		dev->raw_frames_torn++;
		mz0380_return_vbuf(dev, vbuf);
		return false;
	}

	/* M239: put back the pixels the sentinels were sitting on. */
	mz0380_raw_sentinel_restore(dev, dst, sentinels);

	vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, frame);
	vbuf->vb.vb2_buf.timestamp = timestamp;
	vbuf->vb.field = mz0380_current_field(dev);
	vbuf->vb.sequence = dev->video_sequence++;
	vbuf->vb.flags &= ~(V4L2_BUF_FLAG_PFRAME | V4L2_BUF_FLAG_BFRAME);
	vbuf->vb.flags |= V4L2_BUF_FLAG_KEYFRAME;

	if (mz0380_live_source_confirmed(dev))
		mz0380_no_signal_deactivate(dev);

	raw->delivered++;
	memcpy(raw->last_head, dst, sizeof(raw->last_head));
	raw->last_head_valid = true;
	dev->raw_frames_delivered++;
	if (neutral)
		dev->raw_chroma_neutral++;
	mz0380_cadence_note(dev, dev->cadence_take,
			    &dev->cadence_take_prev_ns, timestamp);
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	return true;
}

/* Is bank-0 slot @idx holding a whole, finished frame (M217, M229, M238)? */
static bool mz0380_raw_slot_ready(struct mz0380_dev *dev, u32 idx)
{
	if (!mz0380_raw_frame_landed(dev, idx)) {
		dev->raw_bufs[idx].neutral_since_ns = 0;
		return false;
	}
	if (!mz0380_raw_frame_filled(dev, idx)) {
		/* Cleared to black, not yet filled. Leave it to finish. */
		dev->raw_bufs[idx].neutral_since_ns = 0;
		dev->raw_incomplete_tail++;
		return false;
	}
	if (!mz0380_raw_chroma_filled(dev, idx)) {
		dev->raw_incomplete_chroma++;
		return false;
	}
	return true;
}

/*
 * M223: one frame per completion, taken round-robin.
 *
 * The raw window raises no completion of its own (M215), so this runs on the
 * ENCODED completion and scans the slots instead of trusting a token (M222).
 * At most one frame is taken per completion, which matches the frame clock,
 * and each scan starts after the slot taken last, so the four slots are
 * consumed in the order the card fills them without having to know it.
 */
static void mz0380_drain_raw_deliver(struct mz0380_dev *dev)
{
	unsigned int i, slot;
	u8 ready = 0;

	/*
	 * The source changed mode under the running encoder: what the card
	 * writes now is not what the buffers were negotiated for. Deliver
	 * nothing, so the NO SIGNAL picture shows until the application
	 * restarts the capture (or the change proves transient).
	 */
	if (READ_ONCE(dev->pipeline_reconfigure_pending))
		return;

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++)
		if (mz0380_raw_slot_ready(dev, i))
			ready |= BIT(i);
	dev->cadence_ready[hweight8(ready)]++;

	if (!ready) {
		dev->raw_stub_completions++;
		return;
	}
	/*
	 * The source is live whether or not the application had a buffer
	 * queued for this frame; the NO SIGNAL picture keys off this.
	 */
	WRITE_ONCE(dev->raw_last_live, jiffies);
	if (hweight8(ready) > 1)
		dev->raw_multi_landed++;

	mutex_lock(&dev->delivery_lock);
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		slot = (dev->raw_next_slot + i) % MZ0380_STREAM_NR_BUFS;
		if (!(ready & BIT(slot)))
			continue;
		if (mz0380_raw_deliver_slot(dev, slot))
			dev->raw_next_slot = (slot + 1) % MZ0380_STREAM_NR_BUFS;
		break;
	}
	mutex_unlock(&dev->delivery_lock);
}

static void
mz0380_drain_frame_snapshot(struct mz0380_dev *dev,
			    const struct mz0380_frame_event *snapshot)
{
	if (!READ_ONCE(dev->deliver_raw)) {
		mz0380_drain_h264_snapshot(dev, snapshot);
		return;
	}
	/*
	 * M231: an encoded completion IS the proof the producer is alive. The
	 * receiver monitor reads silence off this stamp; in a raw session it
	 * would otherwise freeze at STREAMON and the monitor would re-arm HDMI
	 * acquisition mid-capture every 1.5 s.
	 */
	WRITE_ONCE(dev->last_h264_frame_stamp, jiffies);
	mz0380_drain_raw_deliver(dev);
}

static void mz0380_dma_drain_video(struct mz0380_dev *dev)
{
	struct mz0380_frame_event snapshot;
	unsigned long event_flags, flags;
	unsigned int batch = 0;
	bool handled = false;

	/*
	 * Keep drain_scheduled asserted until the queue-empty observation and
	 * the ownership ACK are one transaction. Clearing it in event_pop()
	 * would leave a window where the ACK frees a slot the pre-ACK hook has
	 * just snapshotted but this worker has not copied yet.
	 */
	for (;;) {
		/*
		 * Drain the complete batch before the one ownership ACK, so an
		 * overflowed newest event cannot release the endpoint early and
		 * overwrite an older queued slot before its copy.
		 */
		while (mz0380_frame_event_pop(dev, &snapshot)) {
			handled = true;
			batch++;
			mz0380_drain_frame_snapshot(dev, &snapshot);
		}

		spin_lock_irqsave(&dev->frame_event_lock, flags);
		handled |= dev->frame_event_ack_deferred;
		dev->frame_event_ack_deferred = false;
		spin_unlock_irqrestore(&dev->frame_event_lock, flags);

		/*
		 * The snapshot hook runs under event_lock and then takes
		 * frame_event_lock; take them in the same order so no snapshot
		 * can sit between the final empty check and the enc_stat ACK.
		 * If one arrived while we copied, loop without clearing
		 * drain_scheduled: this invocation owns the retry.
		 */
		spin_lock_irqsave(&dev->event_lock, event_flags);
		spin_lock(&dev->frame_event_lock);
		if (dev->frame_event_tail != dev->frame_event_head ||
		    dev->frame_event_ack_deferred) {
			spin_unlock(&dev->frame_event_lock);
			spin_unlock_irqrestore(&dev->event_lock, event_flags);
			continue;
		}
		dev->frame_event_drain_scheduled = false;
		if (handled)
			mz0380_enc_stat_ack(dev);
		if (batch)
			dev->cadence_batch[min(batch, 3u) - 1]++;
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
