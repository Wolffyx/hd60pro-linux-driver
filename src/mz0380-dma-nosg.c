// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 no-scatter-gather polling capture fallback.
 */

#include "mz0380-dma-internal.h"

/* --- stream_nosg polling capture ----------------------------------------- */

/*
 * The fake-frame path has no host-visible completion: the card DMAs one
 * contiguous 0x30a5c0-byte burst into buf0 and then its encoder loop parks
 * forever, IRQ-less, for a card-internal reason no host action can fix (M41).
 * But the burst itself is proven contiguous front-to-back (M37), so "the
 * final dwords are no longer poison" == "the whole frame has landed". And a
 * fresh encoder spawn reliably yields exactly one more frame (M39). Those
 * two facts make a polling capture loop: poison buf0, spawn, poll the tail,
 * deliver as NV12, stop, respawn.
 */

static bool mz0380_nosg_frame_landed(struct mz0380_dev *dev)
{
	const u32 *p = dev->stream_bufs[0].va;
	size_t end = MZ0380_STREAM_RAW_FRAME_SIZE / 4;

	/*
	 * Two independent tail dwords must have been overwritten: one could
	 * collide with frame data that happens to equal the poison word (the
	 * M37 poison_byte experiment saw exactly such collisions mid-frame),
	 * two adjacent collisions at the fixed frame tail are not credible.
	 */
	return p[end - 1] != mz0380_poison_w() &&
	       p[end - 2] != mz0380_poison_w();
}

static void mz0380_nosg_deliver(struct mz0380_dev *dev)
{
	struct mz0380_vb_buffer *vbuf;
	unsigned long flags;

	if (!READ_ONCE(dev->streaming))
		return;

	spin_lock_irqsave(&dev->buf_lock, flags);
	vbuf = list_first_entry_or_null(&dev->buf_list,
					struct mz0380_vb_buffer, list);
	if (vbuf)
		list_del(&vbuf->list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);

	if (!vbuf) {
		pr_info_ratelimited("%s: nosg frame %u landed but no buffer queued - dropped\n",
				    dev->name, dev->nosg_sequence);
		return;
	}

	{
		void *dst = vb2_plane_vaddr(&vbuf->vb.vb2_buf, 0);
		size_t plane = vb2_plane_size(&vbuf->vb.vb2_buf, 0);
		size_t cpy = min_t(size_t, MZ0380_NOSG_NV12_SIZEIMAGE, plane);

		if (dst)
			memcpy(dst, dev->stream_bufs[0].va, cpy);
		vb2_set_plane_payload(&vbuf->vb.vb2_buf, 0, cpy);
	}

	vbuf->vb.vb2_buf.timestamp = ktime_get_ns();
	vbuf->vb.field = V4L2_FIELD_NONE;
	vbuf->vb.sequence = dev->nosg_sequence++;
	vb2_buffer_done(&vbuf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

static int mz0380_nosg_thread(void *data)
{
	struct mz0380_dev *dev = data;

	while (!kthread_should_stop()) {
		unsigned long deadline;
		bool landed = false;
		int ret;

		/*
		 * Poison the landing zone ourselves so frame arrival is
		 * detectable regardless of the buf_poison diagnostic knob
		 * (dma_start re-poisons the whole set when that is on; a
		 * second memset is harmless).
		 */
		memset(dev->stream_bufs[0].va, mz0380_poison_b(),
		       MZ0380_STREAM_RAW_FRAME_SIZE);
		wmb();

		/*
		 * M60: count the spawns. The card wedges for good after
		 * roughly 8-18 of them and nothing short of removing slot
		 * power brings it back (M52), so the count is the single
		 * most useful number in the log - and a warning before the
		 * cliff is worth more than a post-mortem after it.
		 */
		dev->nosg_spawns++;
		if (dev->nosg_spawns == 8)
			pr_warn("%s: nosg: 8 encoder spawns this session - entering the range where the card has wedged before (needs a mains-off cold boot)\n",
				dev->name);

		ret = mz0380_dma_start(dev);
		if (ret) {
			pr_warn("%s: nosg spawn failed (%d) - retrying in 500 ms\n",
				dev->name, ret);
			if (!kthread_should_stop())
				msleep(500);
			continue;
		}

		deadline = jiffies +
			   msecs_to_jiffies(mz0380_nosg_frame_timeout_ms);
		while (!kthread_should_stop() &&
		       time_before(jiffies, deadline)) {
			if (mz0380_nosg_frame_landed(dev)) {
				landed = true;
				break;
			}
			msleep(10);
		}

		if (landed)
			mz0380_nosg_deliver(dev);
		else if (!kthread_should_stop())
			pr_warn("%s: nosg frame did not land within %u ms - respawning\n",
				dev->name, mz0380_nosg_frame_timeout_ms);

		/*
		 * Quiet stop: op7 ends this spawn so the next SET_VIC forks a
		 * fresh tinyvenc5 whose VPL_DMAC_Open clears the parked state
		 * (M39/M41) - that reset is what makes the next frame possible.
		 * The verbose stop's dumps would spam dmesg once per frame.
		 */
		__mz0380_dma_stop(dev, false);
	}
	return 0;
}

int mz0380_nosg_capture_start(struct mz0380_dev *dev)
{
	struct task_struct *task;

	if (!dev->dma_armed || !dev->stream_bufs[0].va)
		return -ENODEV;
	if (dev->nosg_task)
		return 0;

	dev->nosg_sequence = 0;
	dev->nosg_spawns = 0;
	dev->aic_armed = false;
	task = kthread_run(mz0380_nosg_thread, dev, "mz0380-nosg/%u", dev->nr);
	if (IS_ERR(task))
		return PTR_ERR(task);
	dev->nosg_task = task;

	pr_info("%s: nosg synthetic/no-signal polling started (NV12 %ux%u generated on-card; not HDMI input; diagnostic path spawns one encoder process per frame and is not real-time)\n",
		dev->name, MZ0380_NOSG_NV12_WIDTH,
		MZ0380_NOSG_NV12_HEIGHT);
	return 0;
}

void mz0380_nosg_capture_stop(struct mz0380_dev *dev)
{
	if (!dev->nosg_task)
		return;
	kthread_stop(dev->nosg_task);
	dev->nosg_task = NULL;

	/*
	 * M60: hand the card's audio side back. Every session so far armed
	 * audio_ready and never cleared it, once per frame, which is exactly
	 * the kind of one-way resource churn that fits a card that dies after
	 * a fixed number of spawns.
	 */
	if (dev->aic_armed) {
		u32 aic[4] = { 0, 0, 0, 0 };   /* cmd+16 on=0 */
		int ret = mz0380_send_command(dev, MZ0380_CMD_SET_AIC_PARAMS,
					      aic, ARRAY_SIZE(aic), NULL, 2000);

		pr_info("%s: nosg: SET_AIC(on=0) ret=%d\n", dev->name, ret);
		dev->aic_armed = false;
	}

	pr_info("%s: nosg polling capture stopped after %u frames, %u encoder spawns\n",
		dev->name, dev->nosg_sequence, dev->nosg_spawns);
}
