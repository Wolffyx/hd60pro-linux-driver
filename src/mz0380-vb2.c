// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 videobuf2 queue ownership and stream lifecycle.
 *
 * With persistent_h264 (the default) the card-side encoder pipeline outlives
 * the VB2 queue: the first STREAMON starts it (one encoder spawn), STREAMOFF
 * only detaches, and a later STREAMON attaches to the pipeline that is still
 * running. A source that changed, or a format the running pipeline was not
 * started for, replaces it once - at the next STREAMON, never behind the
 * application's back.
 */

#include "mz0380.h"

static int mz0380_queue_setup(struct vb2_queue *vq,
			      unsigned int *nbuffers, unsigned int *nplanes,
			      unsigned int sizes[], struct device *alloc_devs[])
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vq);
	unsigned int size = mz0380_current_sizeimage(dev);

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = size;
	*nbuffers = max(*nbuffers, 3u);
	return 0;
}

static int mz0380_buf_prepare(struct vb2_buffer *vb)
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	unsigned int size = mz0380_current_sizeimage(dev);

	if (vb2_plane_size(vb, 0) < size) {
		dev_err(&dev->pci->dev, "buffer too small (%lu < %u)\n",
			vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}
	vb2_set_plane_payload(vb, 0, 0);
	return 0;
}

static void mz0380_buf_queue(struct vb2_buffer *vb)
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mz0380_vb_buffer *buf =
		container_of(vbuf, struct mz0380_vb_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&dev->buf_lock, flags);
	list_add_tail(&buf->list, &dev->buf_list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
}

static void mz0380_return_buffers(struct mz0380_dev *dev,
				  enum vb2_buffer_state state)
{
	struct mz0380_vb_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&dev->buf_lock, flags);
	list_for_each_entry_safe(buf, tmp, &dev->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&dev->buf_lock, flags);
}

/*
 * STREAMON with no HDMI source: serve the NO SIGNAL picture and spend no
 * encoder spawn. The receiver monitor starts the pipeline once a stable source
 * appears.
 */
static int mz0380_start_placeholder_only(struct mz0380_dev *dev, int lock_ret)
{
	if (!mz0380_stream_without_signal || !mz0380_persistent_h264)
		return lock_ret ?: -ENOLCK;
	/* The H.264 placeholder is one fixed 1080p access unit. */
	if (!dev->deliver_raw &&
	    (dev->capture.width != 1920 || dev->capture.height != 1080)) {
		dev_err(&dev->pci->dev,
			"no HDMI signal, and the H.264 NO SIGNAL picture is 1920x1080 while the capture is set up for %ux%u\n",
			dev->capture.width, dev->capture.height);
		return -EPIPE;
	}

	mz0380_signal_clear(dev);
	dev->video_sequence = 0;
	WRITE_ONCE(dev->pipeline_reconfigure_pending, false);
	WRITE_ONCE(dev->pipeline_start_failed, false);
	WRITE_ONCE(dev->raw_last_live, 0);
	WRITE_ONCE(dev->streaming, true);
	mz0380_no_signal_activate(dev, "capture started without a source");
	mz0380_signal_recovery_start(dev);
	dev_info(&dev->pci->dev,
		 "capture started with no HDMI signal; showing NO SIGNAL until a source appears\n");
	return 0;
}

/*
 * Attach VB2 to the persistent pipeline that is already producing. Sending
 * even SET_ENC/SET_BUF/START here would be wrong - tinyvenc is no longer in
 * its initial command state - and SET_VIC would fork another encoder.
 */
static int mz0380_attach_pipeline(struct mz0380_dev *dev)
{
	if (dev->capture.width != dev->pipeline_width ||
	    dev->capture.height != dev->pipeline_height) {
		dev_err(&dev->pci->dev,
			"cannot attach a %ux%u capture to the running %ux%u encoder\n",
			dev->capture.width, dev->capture.height,
			dev->pipeline_width, dev->pipeline_height);
		return -EBUSY;
	}

	dev->video_sequence = 0;
	WRITE_ONCE(dev->h264_waiting_for_idr, true);
	WRITE_ONCE(dev->last_h264_frame_stamp, jiffies);
	/*
	 * Raw frames keep flowing through a reattachment: give them
	 * MZ0380_RAW_LIVE_MS to arrive before a raw NO SIGNAL picture may be
	 * shown, or every reopen would flash one.
	 */
	WRITE_ONCE(dev->raw_last_live, jiffies);
	WRITE_ONCE(dev->streaming, true);
	dev->pipeline_attach_count++;
	/*
	 * The source may have changed while nothing was attached and the
	 * monitor was stopped: hold the placeholder until one fresh receiver
	 * measurement proves this pipeline still matches.
	 */
	WRITE_ONCE(dev->signal_recovering, true);
	mz0380_no_signal_activate(dev, "validating HDMI at reattachment");
	dev_dbg(&dev->pci->dev,
		"attached to the running encoder (attachment %u, spawns %u)\n",
		dev->pipeline_attach_count, dev->encoder_spawns);
	mz0380_signal_recovery_start(dev);
	return 0;
}

/*
 * M65: no live lock, but this source's mode was detected moments ago. A
 * source that transmits in bursts can miss the instant STREAMON runs, and
 * SET_VIC only needs the geometry. Only for "no lock" and "not stable yet": a
 * different mode (-ERANGE) or a mailbox error is not a lock to reuse.
 */
static bool mz0380_use_recent_lock(struct mz0380_dev *dev, int ret)
{
	struct v4l2_dv_timings *t = &dev->last_good_timings;

	if ((ret != -ENOLCK && ret != -EAGAIN) || !dev->have_last_good ||
	    !time_before(jiffies, dev->last_good_stamp +
			 msecs_to_jiffies(MZ0380_SIGNAL_CACHE_MS)))
		return false;

	dev->signal_locked = true;
	dev->detected_timings = *t;
	dev->capture.source_width = t->bt.width;
	dev->capture.source_height = t->bt.height;
	dev->capture.source_fps = mz0380_source_fps(t);
	dev->capture.source_interlaced = t->bt.interlaced;
	dev_info(&dev->pci->dev,
		 "no live HDMI lock (%d); starting with the %ux%u%c detected %ums ago\n",
		 ret, t->bt.width, t->bt.height, t->bt.interlaced ? 'i' : 'p',
		 jiffies_to_msecs(jiffies - dev->last_good_stamp));
	return true;
}

/*
 * M174/M222: there is no scaler, so the buffers and the card must agree on the
 * frame size. Refuse and tell the application through SOURCE_CHANGE instead of
 * delivering a frame mislabelled as a different geometry.
 */
static int mz0380_check_geometry(struct mz0380_dev *dev)
{
	u32 sw = dev->capture.source_width, sh = dev->capture.source_height;

	if (!dev->signal_locked || !sw || !sh ||
	    (sw == dev->capture.width && sh == dev->capture.height))
		return 0;

	dev_err(&dev->pci->dev,
		"the HDMI source is %ux%u but the capture was set up for %ux%u; set the format to %ux%u (or follow the SOURCE_CHANGE event) and start again\n",
		sw, sh, dev->capture.width, dev->capture.height, sw, sh);
	mz0380_signal_event(dev);
	return -EPIPE;
}

/* No pipeline yet: measure the source and start one (one encoder spawn). */
static int mz0380_start_fresh(struct mz0380_dev *dev)
{
	struct v4l2_dv_timings live;
	bool locked;
	int ret;

	/*
	 * One lock-byte read is enough to prove the empty-input case; the
	 * monitor performs the full measurement once lock appears.
	 */
	if (mz0380_stream_without_signal && mz0380_persistent_h264) {
		ret = mz0380_mst3367_read_lock(dev, &locked, false);
		if (!ret && !locked)
			return mz0380_start_placeholder_only(dev, -ENOLCK);
	}

	ret = mz0380_query_signal(dev, &live);
	if (ret && !mz0380_use_recent_lock(dev, ret)) {
		if (mz0380_stream_without_signal)
			return mz0380_start_placeholder_only(dev, ret);
		dev_warn(&dev->pci->dev,
			 "no HDMI signal locked (%d) - check the source\n",
			 ret);
		return ret;
	}

	ret = mz0380_check_geometry(dev);
	if (ret)
		return ret;

	dev->video_sequence = 0;
	WRITE_ONCE(dev->h264_waiting_for_idr, true);
	WRITE_ONCE(dev->last_h264_frame_stamp, jiffies);
	WRITE_ONCE(dev->raw_last_live, jiffies);
	WRITE_ONCE(dev->streaming, true);
	ret = mz0380_dma_start(dev);
	if (ret) {
		WRITE_ONCE(dev->streaming, false);
		dev_err(&dev->pci->dev, "encoder start failed (%d)\n", ret);
		return ret;
	}
	mz0380_signal_recovery_start(dev);
	return 0;
}

static int mz0380_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vq);
	u16 command;
	int ret;

	/* Diagnostic shutdown may clear bus mastering outside the driver.
	 * Never spawn an encoder based only on the cached dma_armed flag.
	 */
	ret = pci_read_config_word(dev->pci, PCI_COMMAND, &command);
	if (ret || command == 0xffff || !(command & PCI_COMMAND_MASTER)) {
		dev_warn(&dev->pci->dev,
			 "capture refused: PCI bus mastering is disabled or inaccessible\n");
		ret = -EIO;
		goto error;
	}

	if (!dev->dma_armed) {
		dev_warn(&dev->pci->dev,
			 "cannot stream: DMA setup failed at load (see the earlier messages)\n");
		ret = -ENODEV;
		goto error;
	}
	/*
	 * M245: every path below talks to the card. After a resume the
	 * handshake runs asynchronously, and a card whose handshake failed at
	 * probe is deaf; say so instead of timing out mid-start.
	 */
	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		dev_warn(&dev->pci->dev,
			 "cannot stream: card handshake %s\n",
			 mz0380_fw_state_name(dev->fw_state));
		ret = -EIO;
		goto error;
	}

	/* M245: see mz0380_s_ctrl - held for exactly the streaming session. */
	mz0380_encoder_ctrls_grab(dev, true);

	if (mz0380_persistent_h264 && READ_ONCE(dev->pipeline_running) &&
	    (READ_ONCE(dev->pipeline_reconfigure_pending) ||
	     mz0380_pipeline_config_stale(dev))) {
		dev_info(&dev->pci->dev,
			 "source or format changed; restarting the encoder for this capture (one encoder spawn)\n");
		mz0380_dma_stop(dev);
	}

	if (mz0380_persistent_h264 && READ_ONCE(dev->pipeline_running))
		ret = mz0380_attach_pipeline(dev);
	else
		ret = mz0380_start_fresh(dev);
	if (!ret)
		return 0;

	mz0380_encoder_ctrls_grab(dev, false);
error:
	mz0380_return_buffers(dev, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void mz0380_stop_streaming(struct vb2_queue *vq)
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vq);

	WRITE_ONCE(dev->streaming, false);
	mz0380_encoder_ctrls_grab(dev, false);
	mz0380_signal_recovery_stop(dev);
	if (mz0380_persistent_h264 && READ_ONCE(dev->pipeline_running)) {
		/*
		 * Publish detachment before synchronising with a drain already
		 * in progress. Later drains keep consuming card slots and
		 * ACKing enc_stat, but cannot take a VB2 buffer.
		 */
		smp_mb();
		flush_work(&dev->drain_work);
		dev_dbg(&dev->pci->dev,
			"detached; the encoder keeps running for the next capture\n");
	} else {
		mz0380_dma_stop(dev);
	}
	mz0380_return_buffers(dev, VB2_BUF_STATE_ERROR);
}

const struct vb2_ops mz0380_qops = {
	.queue_setup     = mz0380_queue_setup,
	.buf_prepare     = mz0380_buf_prepare,
	.buf_queue       = mz0380_buf_queue,
	.start_streaming = mz0380_start_streaming,
	.stop_streaming  = mz0380_stop_streaming,
#ifdef MZ0380_HAVE_VB2_WAIT_OPS
	/*
	 * Kernels up to 6.x require the driver to drop q->lock around a
	 * blocking DQBUF itself; newer vb2 core does it and removed the ops.
	 * The Makefile probes videobuf2-v4l2.h for them.
	 */
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
#endif
};
