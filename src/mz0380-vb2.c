// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 videobuf2 queue ownership and stream lifecycle.
 */

#include "mz0380-internal.h"

/* ===== vb2 queue ops ================================================= */

static int mz0380_queue_setup(struct vb2_queue *vq,
			      unsigned int *nbuffers, unsigned int *nplanes,
			      unsigned int sizes[], struct device *alloc_devs[])
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vq);
	unsigned int size = mz0380_current_sizeimage(dev);

	if (*nplanes) {
		if (sizes[0] < size)
			return -EINVAL;
		return 0;
	}

	*nplanes = 1;
	sizes[0] = size;
	if (*nbuffers < 3)
		*nbuffers = 3;
	return 0;
}

static int mz0380_buf_prepare(struct vb2_buffer *vb)
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	unsigned int size = mz0380_current_sizeimage(dev);

	if (vb2_plane_size(vb, 0) < size) {
		dev_err(&dev->pci->dev,
			"buffer too small (%lu < %u)\n",
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

static int mz0380_start_placeholder_only(struct mz0380_dev *dev, int lock_ret)
{
	if (!mz0380_stream_without_signal || !mz0380_h264_probe ||
	    !mz0380_persistent_h264)
		return lock_ret ?: -ENOLCK;
	if (dev->capture.width != 1920 || dev->capture.height != 1080) {
		dev_err(&dev->pci->dev,
			"NO SIGNAL placeholder is 1920x1080 but VB2 negotiated %ux%u; cannot return mislabeled H.264\n",
			dev->capture.width, dev->capture.height);
		return -EPIPE;
	}

	dev->detected_timings = mz0380_no_signal;
	dev->signal_locked = false;
	dev->capture.source_width = 0;
	dev->capture.source_height = 0;
	dev->capture.source_fps = 0;
	dev->capture.source_interlaced = false;
	dev->video_sequence = 0;
	WRITE_ONCE(dev->pipeline_reconfigure_pending, false);
	WRITE_ONCE(dev->pipeline_start_failed, false);
	WRITE_ONCE(dev->streaming, true);
	mz0380_no_signal_activate(dev,
		"STREAMON arrived before any stable HDMI lock");
	mz0380_signal_recovery_start(dev);
	dev_info(&dev->pci->dev,
		 "VB2 attached in placeholder-only state: encoder not spawned, SET_VIC count remains %u\n",
		 dev->encoder_spawns);
	return 0;
}

static int mz0380_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vq);
	int ret;

	if (!dev->dma_armed) {
		dev_warn(&dev->pci->dev,
			 "start_streaming called but DMA is not armed (enable_dma=1?)\n");
		ret = -ENODEV;
		goto error;
	}

	/*
	 * M245: every path below talks to the card through the mailbox. After a
	 * resume the handshake runs asynchronously, and a card whose handshake
	 * failed at probe is deaf; either way, say so here instead of letting a
	 * command time out halfway through a pipeline start.
	 */
	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		dev_warn(&dev->pci->dev,
			 "start_streaming: card handshake not complete (state %s)\n",
			 mz0380_fw_state_name(dev->fw_state));
		ret = -EAGAIN;
		goto error;
	}

	/* M245: see mz0380_s_ctrl - held for exactly the streaming session. */
	mz0380_encoder_ctrls_grab(dev, true);

	/*
	 * A persistent H.264 pipeline is already producing into the module-owned
	 * window-1 ring.  Reopening OBS attaches only the VB2 queue: sending even
	 * SET_ENC/SET_BUF/START here is wrong because tinyvenc is no longer in its
	 * initial command state, and sending SET_VIC would fork another process.
	 */
	if (!mz0380_stream_nosg && mz0380_h264_probe &&
	    mz0380_persistent_h264 && READ_ONCE(dev->pipeline_running) &&
	    READ_ONCE(dev->pipeline_reconfigure_pending)) {
		dev_info(&dev->pci->dev,
			 "stable HDMI timing changed; cleanly replacing the persistent encoder at this userspace attachment\n");
		mz0380_dma_stop(dev);
	}

	if (!mz0380_stream_nosg && mz0380_h264_probe &&
	    mz0380_persistent_h264 && READ_ONCE(dev->pipeline_running)) {
		if (dev->capture.width != dev->pipeline_width ||
		    dev->capture.height != dev->pipeline_height) {
			dev_err(&dev->pci->dev,
				"cannot attach %ux%u VB2 session to persistent %ux%u encoder pipeline; stop/reconfigure is required\n",
				dev->capture.width, dev->capture.height,
				dev->pipeline_width, dev->pipeline_height);
			ret = -EBUSY;
			goto error;
		}

		dev->video_sequence = 0;
		WRITE_ONCE(dev->h264_waiting_for_idr, true);
		WRITE_ONCE(dev->last_h264_frame_stamp, jiffies);
		WRITE_ONCE(dev->streaming, true);
		dev->pipeline_attach_count++;
		/*
		 * The HDMI source may have changed while VB2 was detached and the
		 * receiver monitor was intentionally stopped. Present the host IDR
		 * until one fresh lock measurement proves that this pipeline still
		 * matches, then switch only at a clean live IDR.
		 */
		WRITE_ONCE(dev->signal_recovering, true);
		mz0380_no_signal_activate(dev,
			"validating HDMI state at persistent reattachment");
		dev_info(&dev->pci->dev,
			 "VB2 attached to persistent H.264 pipeline (attachment %u, SET_VIC spawns remain %u); no card command sent, validating HDMI then waiting for SPS/PPS + IDR\n",
			 dev->pipeline_attach_count, dev->encoder_spawns);
		mz0380_signal_recovery_start(dev);
		return 0;
	}

	/*
	 * Real capture is driven by whatever the receiver is actually locked
	 * to, so re-detect here: SET_VIC carries the geometry AND the frame
	 * rate down to the card, and a stale timing would arm the encoder for
	 * the wrong cadence. The fake-frame generator ignores the input, so a
	 * lock failure is only fatal for the real path.
	 */
	if (!mz0380_stream_nosg) {
		struct v4l2_dv_timings live;
		bool receiver_locked;

		/*
		 * Do not spend up to signal_poll_ms proving the obvious empty-input
		 * case. One lock-byte read is enough to start the host placeholder;
		 * the independent monitor performs the full coherent measurement when
		 * lock later appears, before it spends the first SET_VIC.
		 */
		if (mz0380_stream_without_signal && mz0380_h264_probe &&
		    mz0380_persistent_h264) {
			ret = mz0380_mst3367_read_lock(dev, &receiver_locked, false);
			if (!ret && !receiver_locked) {
				ret = mz0380_start_placeholder_only(dev, -ENOLCK);
				if (!ret)
					return 0;
				goto error;
			}
		}

		ret = mz0380_query_signal(dev, &live);
		if (ret && dev->have_last_good && mz0380_signal_cache_ms &&
		    time_before(jiffies, dev->last_good_stamp +
				msecs_to_jiffies(mz0380_signal_cache_ms))) {
			/*
			 * M65: no live lock, but we detected this source's mode
			 * moments ago. A source that transmits in ~300ms bursts
			 * around a power-cycle can never be locked at the
			 * instant STREAMON runs, and SET_VIC only needs the
			 * geometry - which has not changed. Arm with what we
			 * measured; the frames are black until the source
			 * transmits again, which is an EDID problem, not a
			 * reason to refuse to stream.
			 */
			live = dev->last_good_timings;
			dev->detected_timings = live;
			dev->signal_locked = true;
			dev->capture.source_width = live.bt.width;
			dev->capture.source_height = live.bt.height;
			dev->capture.source_fps = mz0380_source_fps(&live);
			dev->capture.source_interlaced = live.bt.interlaced;
			if (dev->capture.source_fps) {
				dev->capture.timeperframe.numerator = 1;
				dev->capture.timeperframe.denominator =
					dev->capture.source_fps;
			}
			dev_info(&dev->pci->dev,
				 "no live lock (%d) - arming with the detection from %ums ago (%ux%u%c) [signal_cache_ms=%u]\n",
				 ret,
				 jiffies_to_msecs(jiffies - dev->last_good_stamp),
				 live.bt.width, live.bt.height,
				 live.bt.interlaced ? 'i' : 'p',
				 mz0380_signal_cache_ms);
			ret = 0;
		} else if (ret && mz0380_stream_without_signal) {
			ret = mz0380_start_placeholder_only(dev, ret);
			if (!ret)
				return 0;
			goto error;
		} else if (ret) {
			dev_warn(&dev->pci->dev,
				 "no HDMI signal locked (%d) - check the source, HPD and EDID load\n",
				 ret);
			goto error;
		}

		dev_info(&dev->pci->dev,
			 "live input %ux%u%c@%u -> encoder output %ux%u@%u\n",
			 dev->capture.source_width, dev->capture.source_height,
			 dev->capture.source_interlaced ? 'i' : 'p',
			 dev->capture.source_fps, dev->capture.width,
			 dev->capture.height,
			 dev->capture.timeperframe.denominator /
			 max_t(u32, dev->capture.timeperframe.numerator, 1));

		/*
		 * M174: the buffers and the card must agree on the frame size.
		 *
		 * Two geometries have always existed side by side and nothing
		 * checked they matched. mz0380_queue_setup() sizes the vb2
		 * plane from capture.width/height - what the application
		 * negotiated, 1920x1080 unless it said otherwise - while the
		 * drain measures and delivers
		 * source_width * source_height * 3/2, what the card actually
		 * wrote. On the only source this project has ever tested those
		 * are the same number, so the gap has never shown.
		 *
		 * With a 720p source they differ: the card writes 1382400
		 * bytes, the drain delivers them, and the application is
		 * holding a buffer it was told is 1920x1080 with 1382400 bytes
		 * of payload in it. Nothing errors. It just renders garbage,
		 * which is the worst available outcome and precisely the class
		 * of defect M168 was about.
		 *
		 * Refusing is the honest answer, and it is also the useful one:
		 * the source-change event queued below tells a well-behaved
		 * client to re-negotiate, S_FMT to the detected geometry then
		 * resizes the buffers, and the capture works. Delivering the
		 * mislabelled frame would leave the client no way to find out.
		 *
		 * NOTE: the non-1080p path is CORRECT BY CONSTRUCTION here, not
		 * verified - this project has never had a non-1080p source. All
		 * that is proven is that the old behaviour was wrong.
		 */
		if (mz0380_strict_geometry && dev->signal_locked &&
		    dev->capture.source_width &&
		    dev->capture.source_height &&
		    (dev->capture.source_width != dev->capture.width ||
		     dev->capture.source_height != dev->capture.height)) {
			dev_err(&dev->pci->dev,
				"refusing to stream: the source is %ux%u but the buffers were allocated for %ux%u, and this path has no scaler - the card would write %u bytes into a buffer described as %u. S_FMT to %ux%u (or VIDIOC_SUBSCRIBE_EVENT the source change) and try again\n",
				dev->capture.source_width,
				dev->capture.source_height,
				dev->capture.width, dev->capture.height,
				dev->capture.source_width *
				dev->capture.source_height * 3 / 2,
				dev->capture.width * dev->capture.height * 3 / 2,
				dev->capture.source_width,
				dev->capture.source_height);
			if (dev->video_registered)
				mz0380_signal_event(dev);
			ret = -EPIPE;
			goto error;
		}
	}

	/*
	 * Fake-frame path: no completion IRQ exists (M41), so streaming is a
	 * polling kthread that spawns the encoder itself, once per frame
	 * (M39). The real path arms the encoder here and delivers from the
	 * MSI-driven drain.
	 */
	/* Completion work may run as soon as START is posted. */
	if (!mz0380_stream_nosg && mz0380_h264_probe) {
		WRITE_ONCE(dev->h264_waiting_for_idr, true);
		WRITE_ONCE(dev->last_h264_frame_stamp, jiffies);
	}
	dev->streaming = true;
	if (mz0380_stream_nosg)
		ret = mz0380_nosg_capture_start(dev);
	else
		ret = mz0380_dma_start(dev);
	if (ret) {
		dev->streaming = false;
		dev_err(&dev->pci->dev,
			"%s failed (%d)\n",
			mz0380_stream_nosg ? "nosg_capture_start" : "dma_start",
			ret);
		goto error;
	}
	mz0380_signal_recovery_start(dev);

	return 0;

error:
	mz0380_encoder_ctrls_grab(dev, false);
	{
		struct mz0380_vb_buffer *buf, *tmp;
		unsigned long flags;
		spin_lock_irqsave(&dev->buf_lock, flags);
		list_for_each_entry_safe(buf, tmp, &dev->buf_list, list) {
			list_del(&buf->list);
			vb2_buffer_done(&buf->vb.vb2_buf,
					VB2_BUF_STATE_QUEUED);
		}
		spin_unlock_irqrestore(&dev->buf_lock, flags);
	}
	return ret;
}

static void mz0380_stop_streaming(struct vb2_queue *vq)
{
	struct mz0380_dev *dev = vb2_get_drv_priv(vq);
	struct mz0380_vb_buffer *buf, *tmp;
	unsigned long flags;

	/*
	 * nosg: the capture thread owns the start/stop cycle; its last
	 * iteration already sent STOP, so only the thread needs joining. The
	 * verbose dma_stop still runs for its end-of-stream diagnostics dump.
	 */
	dev->streaming = false;
	mz0380_encoder_ctrls_grab(dev, false);
	mz0380_signal_recovery_stop(dev);
	mz0380_nosg_capture_stop(dev);
	if (!mz0380_stream_nosg && mz0380_h264_probe &&
	    mz0380_persistent_h264 && READ_ONCE(dev->pipeline_running)) {
		/*
		 * Publish detachment before synchronizing with a drain already in
		 * progress.  Future drains continue consuming card slots and ACKing
		 * enc_stat, but cannot take a VB2 buffer.
		 */
		smp_mb();
		flush_work(&dev->drain_work);
		dev_info(&dev->pci->dev,
			 "VB2 detached; persistent H.264 pipeline, DMA mappings and completion ACK path remain active (no STOP, no SET_VIC budget spent)\n");
	} else {
		mz0380_dma_stop(dev);
	}

	spin_lock_irqsave(&dev->buf_lock, flags);
	list_for_each_entry_safe(buf, tmp, &dev->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&dev->buf_lock, flags);
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
	 * blocking DQBUF itself. The ops and the vb2_ops_wait_* helpers were
	 * removed once vb2 core took that over, so newer kernels must not set
	 * them. The Makefile probes videobuf2-v4l2.h rather than testing
	 * LINUX_VERSION_CODE - the removal release is not worth guessing.
	 */
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
#endif
};
