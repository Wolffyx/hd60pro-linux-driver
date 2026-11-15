// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 HDMI signal detection, the receiver monitor, and the V4L2 DV-timings
 * operations.
 *
 * The card never pushes the input format to the host (RE_FINDINGS.md M6); the
 * host reads it off the MST3367 receiver through the mailbox I2C proxy (M15).
 * Every receiver read is a mailbox round-trip of a few milliseconds, and
 * receiver traffic during a capture can perturb it (M74), so the rules here
 * are about reading as little as possible:
 *
 *   - while a capture runs, frame arrival is the liveness signal, and the
 *     receiver is only polled once the producer goes quiet;
 *   - an idle query reads the lock byte first and measures only a locked
 *     receiver.
 */

#include <linux/v4l2-dv-timings.h>
#include <media/v4l2-event.h>

#include "mz0380.h"

const struct v4l2_dv_timings mz0380_no_signal = {
	.type = V4L2_DV_BT_656_1120,
};

/*
 * What G_DV_TIMINGS answers before any source was seen or set. It has to be a
 * timing S_DV_TIMINGS accepts: v4l2-compliance restores whatever G returned,
 * and the empty mz0380_no_signal failed that restore whenever the test ran
 * without a source (M248). 1080p60 is the capture's startup geometry.
 */
const struct v4l2_dv_timings mz0380_default_timings =
	V4L2_DV_BT_CEA_1920X1080P60;

static const struct v4l2_dv_timings_cap mz0380_timings_cap = {
	.type = V4L2_DV_BT_656_1120,
	.bt = {
		.min_width = 720,
		.max_width = 1920,
		.min_height = 480,
		.max_height = 1080,
		.min_pixelclock = 27000000,
		.max_pixelclock = 148500000,
		.standards = V4L2_DV_BT_STD_CEA861,
		.capabilities = V4L2_DV_BT_CAP_INTERLACED |
				V4L2_DV_BT_CAP_PROGRESSIVE,
	},
};

/*
 * M247: the timings the receiver can actually recognise (the MST3367 mode
 * table), not every CEA-861 timing inside the capability box - those included
 * 640x480 and 576p, which detection can never report.
 */
static const struct v4l2_dv_timings mz0380_supported_timings[] = {
	V4L2_DV_BT_CEA_1920X1080P60,
	V4L2_DV_BT_CEA_1920X1080P50,
	V4L2_DV_BT_CEA_1920X1080P30,
	V4L2_DV_BT_CEA_1920X1080P25,
	V4L2_DV_BT_CEA_1920X1080P24,
	V4L2_DV_BT_CEA_1920X1080I60,
	V4L2_DV_BT_CEA_1920X1080I50,
	V4L2_DV_BT_CEA_1280X720P60,
	V4L2_DV_BT_CEA_1280X720P50,
	V4L2_DV_BT_CEA_1280X720P30,
	V4L2_DV_BT_CEA_720X480P59_94,
};

u32 mz0380_source_fps(const struct v4l2_dv_timings *timings)
{
	const struct v4l2_bt_timings *bt = &timings->bt;
	u64 total = (u64)V4L2_DV_BT_FRAME_WIDTH(bt) *
		    V4L2_DV_BT_FRAME_HEIGHT(bt);

	if (!total || !bt->pixelclock)
		return 0;

	return min_t(u64, 255,
		     div_u64(bt->pixelclock + total / 2, total));
}

void mz0380_signal_event(struct mz0380_dev *dev)
{
	static const struct v4l2_event ev = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	if (dev->video_registered)
		v4l2_event_queue(&dev->vdev, &ev);
}

/* The receiver is locked to nothing. */
void mz0380_signal_clear(struct mz0380_dev *dev)
{
	dev->signal_locked = false;
	dev->detected_timings = mz0380_no_signal;
	dev->capture.source_width = 0;
	dev->capture.source_height = 0;
	dev->capture.source_fps = 0;
	dev->capture.source_interlaced = false;
}

/* The receiver is locked to @t; @t also becomes the M65 fallback. */
void mz0380_signal_adopt(struct mz0380_dev *dev,
			 const struct v4l2_dv_timings *t)
{
	dev->signal_locked = true;
	dev->detected_timings = *t;
	/*
	 * M171: G tracks reality absent an S. M245: and ONLY absent an S, or a
	 * QUERY after S_DV_TIMINGS would make G return something never set.
	 */
	if (!dev->timings_set_by_user)
		dev->set_timings = *t;
	dev->last_good_timings = *t;
	dev->last_good_stamp = jiffies;
	dev->have_last_good = true;
	dev->capture.source_width = t->bt.width;
	dev->capture.source_height = t->bt.height;
	dev->capture.source_fps = mz0380_source_fps(t);
	dev->capture.source_interlaced = t->bt.interlaced;
}

/*
 * Measure the receiver and update the cached signal state. Returns 0 with a
 * timing, -ENOLCK (locked to nothing), -EAGAIN (lock seen, timing not stable
 * yet), -ERANGE (a stable mode the card cannot capture) or an I/O error.
 */
static int mz0380_query_signal_internal(struct mz0380_dev *dev,
					struct v4l2_dv_timings *out,
					bool allow_cache, bool notify)
{
	bool was_locked = dev->signal_locked;
	struct v4l2_dv_timings prev = dev->detected_timings;
	int ret;

	/* Collapse the burst of identical queries made during negotiation. */
	if (allow_cache && was_locked && dev->have_last_good &&
	    time_before(jiffies, dev->last_good_stamp +
			msecs_to_jiffies(MZ0380_SIGNAL_QUERY_CACHE_MS))) {
		*out = dev->detected_timings;
		return 0;
	}

	ret = mz0380_mst3367_read_signal(dev, out);
	if (ret) {
		mz0380_signal_clear(dev);
		*out = mz0380_no_signal;
		if (notify && was_locked)
			mz0380_signal_event(dev);
		return ret == -ENODEV ? -ENOLCK : ret;
	}

	/* M170: a change is reported as one. */
	if (notify &&
	    (!was_locked || !v4l2_match_dv_timings(&prev, out, 250000, false)))
		mz0380_signal_event(dev);
	mz0380_signal_adopt(dev, out);
	return 0;
}

int mz0380_query_signal(struct mz0380_dev *dev, struct v4l2_dv_timings *out)
{
	return mz0380_query_signal_internal(dev, out, true, true);
}

static bool mz0380_pipeline_matches_signal(struct mz0380_dev *dev,
					   const struct v4l2_dv_timings *live)
{
	return dev->pipeline_source_width == live->bt.width &&
	       dev->pipeline_source_height == live->bt.height &&
	       dev->pipeline_source_fps == mz0380_source_fps(live) &&
	       dev->pipeline_source_interlaced == live->bt.interlaced;
}

static void mz0380_signal_report_lost(struct mz0380_dev *dev)
{
	if (dev->signal_lost_reported)
		return;
	dev->signal_lost_reported = true;
	dev_info(&dev->pci->dev,
		 "HDMI signal lost; showing NO SIGNAL until it returns\n");
}

static void mz0380_signal_report_back(struct mz0380_dev *dev,
				      const struct v4l2_dv_timings *live)
{
	if (!dev->signal_lost_reported)
		return;
	dev->signal_lost_reported = false;
	dev_info(&dev->pci->dev, "HDMI signal back: %ux%u%c@%u\n",
		 live->bt.width, live->bt.height,
		 live->bt.interlaced ? 'i' : 'p', mz0380_source_fps(live));
}

/*
 * No pipeline yet (STREAMON arrived before a stable source): a stable lock
 * that fits the negotiated buffers starts the first one - one SET_VIC.
 */
static unsigned long mz0380_signal_start_pipeline(struct mz0380_dev *dev,
						  const struct v4l2_dv_timings *live)
{
	int ret;

	WRITE_ONCE(dev->signal_recovering, false);
	if (live->bt.width != dev->capture.width ||
	    live->bt.height != dev->capture.height) {
		WRITE_ONCE(dev->pipeline_reconfigure_pending, true);
		mz0380_signal_event(dev);
		dev_warn(&dev->pci->dev,
			 "HDMI source is %ux%u but the capture was set up for %ux%u; keeping NO SIGNAL until the application renegotiates\n",
			 live->bt.width, live->bt.height, dev->capture.width,
			 dev->capture.height);
		return MZ0380_SIGNAL_MONITOR_MS;
	}
	if (READ_ONCE(dev->pipeline_start_failed))
		return MZ0380_SIGNAL_MONITOR_MS;

	WRITE_ONCE(dev->h264_waiting_for_idr, true);
	ret = mz0380_dma_start(dev);
	/* START takes ~2.8 s; the producer's silence clock starts after it. */
	WRITE_ONCE(dev->last_h264_frame_stamp, jiffies);
	if (ret) {
		WRITE_ONCE(dev->pipeline_start_failed, true);
		dev_err(&dev->pci->dev,
			"HDMI lock found but the deferred encoder start failed (%d); NO SIGNAL stays up and no automatic retry is made (each retry would spend an encoder spawn)\n",
			ret);
		return MZ0380_SIGNAL_MONITOR_MS;
	}
	mz0380_signal_report_back(dev, live);
	return MZ0380_SIGNAL_MONITOR_MS;
}

/* Size, rate and scan type: what an encoder is started for. */
static bool mz0380_same_mode(const struct v4l2_dv_timings *a,
			     const struct v4l2_dv_timings *b)
{
	return a->bt.width == b->bt.width && a->bt.height == b->bt.height &&
	       a->bt.interlaced == b->bt.interlaced &&
	       mz0380_source_fps(a) == mz0380_source_fps(b);
}

/*
 * M256: may the driver replace the encoder for @live by itself?
 *
 * Until M256 a source that came back in another mode left the NO SIGNAL
 * picture up until the application restarted the capture - and OBS never
 * does, so swapping a 60 Hz device for a 30 Hz one looked like a dead card.
 *
 * Only a change the application cannot see in its buffers qualifies: the same
 * frame size and scan type at another rate. A new size still needs the
 * application to renegotiate. And only mode_follow times per load, because
 * every replacement is an encoder spawn, and a source that keeps changing its
 * mind would otherwise walk the card into its wedge.
 */
static bool mz0380_mode_fits_capture(struct mz0380_dev *dev,
				     const struct v4l2_dv_timings *live)
{
	return live->bt.width == dev->capture.width &&
	       live->bt.height == dev->capture.height &&
	       live->bt.interlaced == dev->pipeline_source_interlaced;
}

static bool mz0380_mode_followable(struct mz0380_dev *dev,
				   const struct v4l2_dv_timings *live)
{
	return mz0380_mode_fits_capture(dev, live) &&
	       dev->mode_follow_restarts < mz0380_mode_follow;
}

/*
 * M256: has the new mode held long enough? A reconnecting source can report a
 * mode it is only passing through (a run measured 1080p60 between two stable
 * 1080p30 reads), so the same mode has to be measured again and again for
 * MZ0380_MODE_FOLLOW_SETTLE_MS - three measurements at the pending re-check
 * rate - before a spawn is spent on it.
 */
static bool mz0380_mode_follow_settled(struct mz0380_dev *dev,
				       const struct v4l2_dv_timings *live)
{
	if (!dev->follow_timing ||
	    !mz0380_same_mode(&dev->follow_timings, live)) {
		dev->follow_timings = *live;
		dev->follow_since = jiffies;
		dev->follow_timing = true;
		return false;
	}
	return time_after_eq(jiffies, dev->follow_since +
			     msecs_to_jiffies(MZ0380_MODE_FOLLOW_SETTLE_MS));
}

/* What a mode change leads to, for the one log line that announces it. */
static const char *mz0380_mode_change_outcome(struct mz0380_dev *dev,
					      const struct v4l2_dv_timings *live)
{
	if (!mz0380_mode_fits_capture(dev, live) || !mz0380_mode_follow)
		return "NO SIGNAL until the application restarts the capture";
	if (dev->mode_follow_restarts >= mz0380_mode_follow)
		return "the automatic restarts of this load are used up; NO SIGNAL until the application restarts the capture";
	return "NO SIGNAL while it settles, then the encoder restarts for it";
}

/*
 * M256: replace the running encoder with one for @live - one encoder spawn.
 * The session carries on: the application keeps its buffers and its queue,
 * and sees the NO SIGNAL picture until the new encoder delivers.
 */
static unsigned long mz0380_signal_follow_mode(struct mz0380_dev *dev,
					       const struct v4l2_dv_timings *live)
{
	dev->follow_timing = false;
	dev->mode_follow_restarts++;
	dev_info(&dev->pci->dev,
		 "HDMI source held %ux%u%c@%u for %ums; restarting the encoder for it (automatic restart %u of %u, one encoder spawn)\n",
		 live->bt.width, live->bt.height,
		 live->bt.interlaced ? 'i' : 'p', mz0380_source_fps(live),
		 jiffies_to_msecs(jiffies - dev->follow_since),
		 dev->mode_follow_restarts, mz0380_mode_follow);
	mz0380_dma_stop_encoder(dev);
	return mz0380_signal_start_pipeline(dev, live);
}

/* A pipeline exists and the receiver just produced a stable measurement. */
static unsigned long mz0380_signal_check_pipeline(struct mz0380_dev *dev,
						  const struct v4l2_dv_timings *live)
{
	bool pending = READ_ONCE(dev->pipeline_reconfigure_pending);

	WRITE_ONCE(dev->signal_recovering, false);
	if (!mz0380_pipeline_matches_signal(dev, live)) {
		WRITE_ONCE(dev->pipeline_reconfigure_pending, true);
		if (!pending) {
			mz0380_signal_event(dev);
			dev_info(&dev->pci->dev,
				 "HDMI source changed to %ux%u%c@%u (encoder runs %ux%u%c@%u); %s\n",
				 live->bt.width, live->bt.height,
				 live->bt.interlaced ? 'i' : 'p',
				 mz0380_source_fps(live),
				 dev->pipeline_source_width,
				 dev->pipeline_source_height,
				 dev->pipeline_source_interlaced ? 'i' : 'p',
				 dev->pipeline_source_fps,
				 mz0380_mode_change_outcome(dev, live));
		}
		if (mz0380_mode_followable(dev, live) &&
		    mz0380_mode_follow_settled(dev, live))
			return mz0380_signal_follow_mode(dev, live);
		return MZ0380_SIGNAL_MONITOR_MS;
	}
	/* Back on the pipeline's mode: whatever was being timed is moot. */
	dev->follow_timing = false;

	/*
	 * A reconnect can expose a transient timing while the source negotiates
	 * (a run measured 1080p60 between two stable 1080p30 reads). If the
	 * receiver is back on the pipeline's mode, cancel the stale request -
	 * otherwise it suppresses every live IDR on a perfectly live card.
	 */
	if (pending) {
		WRITE_ONCE(dev->pipeline_reconfigure_pending, false);
		dev_info(&dev->pci->dev,
			 "HDMI timing returned to %ux%u%c@%u; pending encoder replacement cancelled\n",
			 dev->pipeline_source_width,
			 dev->pipeline_source_height,
			 dev->pipeline_source_interlaced ? 'i' : 'p',
			 dev->pipeline_source_fps);
	}
	mz0380_signal_report_back(dev, live);
	WRITE_ONCE(dev->h264_waiting_for_idr, true);
	return MZ0380_SIGNAL_MONITOR_MS;
}

/* The receiver reads unlocked. Returns the next poll interval. */
static unsigned long mz0380_signal_unlocked(struct mz0380_dev *dev)
{
	bool first_loss = !READ_ONCE(dev->signal_recovering) ||
			  !dev->signal_recovery_attempts;
	bool locked;
	int ret;

	mz0380_signal_clear(dev);
	mz0380_no_signal_activate(dev, "receiver unlocked");
	/* M256: a mode has to hold without a drop-out to be followed. */
	dev->follow_timing = false;
	if (first_loss) {
		WRITE_ONCE(dev->signal_recovering, true);
		dev->signal_recovery_attempts = 0;
		/* The one write this path makes: re-arm acquisition (M233). */
		ret = mz0380_mst3367_read_lock(dev, &locked, true);
		if (ret)
			pr_info_ratelimited("%s: HDMI acquisition re-arm failed (%d)\n",
					    dev->name, ret);
		if (READ_ONCE(dev->pipeline_running) ||
		    dev->pipeline_attach_count)
			mz0380_signal_report_lost(dev);
	}
	dev->signal_recovery_attempts++;
	return MZ0380_HOTPLUG_RETRY_MS;
}

/*
 * The receiver reports lock but a running encoder has gone quiet (R55 can stay
 * asserted with stale timing registers). Handled once per silence: the next
 * step re-measures the receiver, and if frames still do not come after that,
 * the encoder itself is gone - polling the receiver harder cannot fix it.
 * (Re-arming acquisition here was a no-op since M233: the receiver is locked.)
 */
static unsigned long mz0380_signal_silent(struct mz0380_dev *dev,
					  unsigned long last)
{
	if (dev->silent_stamp == last)
		return MZ0380_SIGNAL_MONITOR_MS;
	dev->silent_stamp = last;
	mz0380_no_signal_activate(dev, "producer silent despite receiver lock");
	WRITE_ONCE(dev->signal_recovering, true);
	WRITE_ONCE(dev->h264_waiting_for_idr, true);
	dev->signal_recovery_attempts = 0;
	dev_warn(&dev->pci->dev,
		 "no frames for %ums although the receiver reports lock; showing NO SIGNAL\n",
		 jiffies_to_msecs(jiffies - last));
	return MZ0380_HOTPLUG_RETRY_MS;
}

/*
 * One receiver monitor step. Returns the delay to the next one, or 0 when
 * streaming ended underneath it.
 */
static unsigned long mz0380_signal_monitor_step(struct mz0380_dev *dev)
{
	unsigned long last = READ_ONCE(dev->last_h264_frame_stamp);
	unsigned long stall = msecs_to_jiffies(MZ0380_HOTPLUG_STALL_MS);
	bool running = READ_ONCE(dev->pipeline_running);
	bool recovering = READ_ONCE(dev->signal_recovering);
	bool pending = READ_ONCE(dev->pipeline_reconfigure_pending);
	struct v4l2_dv_timings live;
	bool locked;
	int ret;

	/*
	 * A live frame is the cheapest and strongest evidence there is: never
	 * let the monitor touch the receiver while one arrived recently.
	 * Reattachment and active recovery bypass this so one fresh receiver
	 * measurement still gates the switch away from the placeholder.
	 */
	if (running && last && !recovering && !pending &&
	    time_before(jiffies, last + stall))
		return MZ0380_SIGNAL_MONITOR_MS;

	ret = mz0380_mst3367_read_lock(dev, &locked, false);
	if (!READ_ONCE(dev->streaming))
		return 0;
	if (ret) {
		pr_info_ratelimited("%s: HDMI lock monitor read failed (%d)\n",
				    dev->name, ret);
		return MZ0380_HOTPLUG_RETRY_MS;
	}
	if (!locked)
		return mz0380_signal_unlocked(dev);

	/*
	 * M256: not while a replacement is pending. An encoder set up for
	 * another mode is expected to go quiet, and the silence handler acts
	 * once per silence and then returns early for good - which would stop
	 * the re-measurements that time the new mode (and that notice the old
	 * one coming back).
	 */
	if (running && last && !recovering && !pending &&
	    time_after_eq(jiffies, last + stall))
		return mz0380_signal_silent(dev, last);

	/* A healthy configured pipeline needs no full multi-register read. */
	if (!recovering && running && !pending)
		return MZ0380_SIGNAL_MONITOR_MS;
	if (!recovering && !running &&
	    (READ_ONCE(dev->pipeline_start_failed) || pending))
		return MZ0380_SIGNAL_MONITOR_MS;
	/*
	 * M247: a replacement is pending while the old encoder still delivers.
	 * Keep re-measuring so the transient-cancel above stays reachable, but
	 * at a walk: every measurement is ~30 mailbox commands and a log line.
	 */
	if (!recovering && pending &&
	    time_before(jiffies, dev->pending_measure_stamp +
			msecs_to_jiffies(MZ0380_PENDING_RECHECK_MS)))
		return MZ0380_SIGNAL_MONITOR_MS;
	dev->pending_measure_stamp = jiffies;

	dev->signal_recovery_attempts++;
	ret = mz0380_query_signal_internal(dev, &live, false, false);
	if (!READ_ONCE(dev->streaming))
		return 0;
	if (ret == -ERANGE)
		return MZ0380_UNSUPPORTED_RETRY_MS;
	if (ret)
		return MZ0380_HOTPLUG_RETRY_MS;

	if (!running)
		return mz0380_signal_start_pipeline(dev, &live);
	return mz0380_signal_check_pipeline(dev, &live);
}

static void mz0380_signal_recovery_work_fn(struct work_struct *work)
{
	struct mz0380_dev *dev = container_of(to_delayed_work(work),
					      struct mz0380_dev,
					      signal_recovery_work);
	unsigned long next_ms;

	if (!READ_ONCE(dev->streaming)) {
		WRITE_ONCE(dev->signal_recovering, false);
		return;
	}

	next_ms = mz0380_signal_monitor_step(dev);
	if (next_ms && READ_ONCE(dev->streaming))
		mod_delayed_work(MZ0380_SYSTEM_WQ, &dev->signal_recovery_work,
				 msecs_to_jiffies(next_ms));
}

void mz0380_signal_recovery_init(struct mz0380_dev *dev)
{
	INIT_DELAYED_WORK(&dev->signal_recovery_work,
			  mz0380_signal_recovery_work_fn);
}

void mz0380_signal_recovery_start(struct mz0380_dev *dev)
{
	dev->signal_recovery_attempts = 0;
	dev->silent_stamp = 0;
	dev->pending_measure_stamp = 0;
	dev->follow_timing = false;
	WRITE_ONCE(dev->h264_waiting_for_idr, true);
	WRITE_ONCE(dev->last_h264_frame_stamp, jiffies);
	mod_delayed_work(MZ0380_SYSTEM_WQ, &dev->signal_recovery_work, 0);
}

void mz0380_signal_recovery_stop(struct mz0380_dev *dev)
{
	cancel_delayed_work_sync(&dev->signal_recovery_work);
	mz0380_no_signal_stop(dev);
	WRITE_ONCE(dev->signal_recovering, false);
	WRITE_ONCE(dev->h264_waiting_for_idr, false);
}

/*
 * Idle (nothing streaming, no pipeline): is the receiver locked? One lock-byte
 * read. An unlock observed here is a real loss: the cached state is cleared,
 * so the next acquisition commits and latches the output stage again, and
 * acquisition is re-armed - once per loss, never per query, because re-arming
 * restarts the receiver's position hunt.
 */
int mz0380_signal_idle_lock(struct mz0380_dev *dev, bool *locked)
{
	bool was_locked = dev->signal_locked;
	int ret;

	ret = mz0380_mst3367_read_lock(dev, locked, was_locked);
	if (ret || *locked)
		return ret;
	mz0380_signal_clear(dev);
	if (was_locked)
		mz0380_signal_event(dev);
	return 0;
}

/*
 * A full measurement costs ~30 mailbox commands, so an idle query reads the
 * lock byte first and an unlocked receiver answers at once instead of being
 * polled for up to MZ0380_SIGNAL_POLL_MS under the device lock.
 */
static int mz0380_query_idle(struct mz0380_dev *dev,
			     struct v4l2_dv_timings *t)
{
	bool locked;
	int ret;

	/* Negotiation fires these in bursts; a fresh lock answers for free. */
	if (dev->signal_locked && dev->have_last_good &&
	    time_before(jiffies, dev->last_good_stamp +
			msecs_to_jiffies(MZ0380_SIGNAL_QUERY_CACHE_MS))) {
		*t = dev->detected_timings;
		return 0;
	}

	ret = mz0380_signal_idle_lock(dev, &locked);
	if (ret)
		return ret;
	if (!locked) {
		*t = mz0380_no_signal;
		return -ENOLCK;
	}
	return mz0380_query_signal(dev, t);
}

int mz0380_query_dv_timings(struct file *file, void *fh,
			    struct v4l2_dv_timings *t)
{
	struct mz0380_dev *dev = video_drvdata(file);
	int ret;

	/*
	 * Do not drive the receiver while video is flowing: OBS issues this
	 * periodically from the thread that recycles buffers, and a live
	 * measurement stalls that recycling. The monitor owns the receiver
	 * then, and a detached pipeline's frame clock says whether the source
	 * is still there.
	 */
	if (READ_ONCE(dev->streaming) || READ_ONCE(dev->pipeline_running)) {
		if (!dev->signal_locked || !dev->have_last_good ||
		    mz0380_detached_source_lost(dev)) {
			*t = mz0380_no_signal;
			return -ENOLINK;
		}
		*t = dev->detected_timings;
		return 0;
	}

	/*
	 * M245: V4L2's error codes for this ioctl, not the driver's own:
	 * internally -ENOLCK means "locked to nothing" and -EAGAIN "not stable
	 * yet"; the ioctl spells those -ENOLINK and -ENOLCK.
	 */
	ret = mz0380_query_idle(dev, t);
	switch (ret) {
	case -ENOLCK:
	case -ENODEV:	/* card handshake or receiver not up (yet) */
		return -ENOLINK;
	case -EAGAIN:
		return -ENOLCK;
	default:
		return ret;	/* 0, -ERANGE, or a real I/O error */
	}
}

/*
 * M171: G returns what was SET, QUERY returns what is DETECTED. The card
 * auto-detects, so the set value does not steer the hardware; a detection
 * updates it too, so G tracks reality for anyone who never calls S.
 */
int mz0380_g_dv_timings(struct file *file, void *fh,
			struct v4l2_dv_timings *t)
{
	struct mz0380_dev *dev = video_drvdata(file);

	*t = dev->set_timings;
	return 0;
}

static bool mz0380_timings_supported(const struct v4l2_dv_timings *t)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mz0380_supported_timings); i++)
		if (v4l2_match_dv_timings(t, &mz0380_supported_timings[i],
					  250000, false))
			return true;
	return false;
}

int mz0380_s_dv_timings(struct file *file, void *fh,
			struct v4l2_dv_timings *t)
{
	struct mz0380_dev *dev = video_drvdata(file);
	const struct mz0380_mode *mode;

	if (!v4l2_valid_dv_timings(t, &mz0380_timings_cap, NULL, NULL) ||
	    !mz0380_timings_supported(t))
		return -EINVAL;

	/*
	 * M172: busy only blocks a CHANGE - setting the timings already in
	 * force must succeed with buffers allocated.
	 */
	if (vb2_is_busy(&dev->vb_queue) &&
	    !v4l2_match_dv_timings(&dev->set_timings, t, 0, false))
		return -EBUSY;

	dev->set_timings = *t;
	dev->timings_set_by_user = true;

	/* M172: and the format follows the timings - there is no scaler. */
	mode = mz0380_find_mode(t->bt.width, t->bt.height);
	dev->capture.width = mode->width;
	dev->capture.height = mode->height;
	return 0;
}

int mz0380_enum_dv_timings(struct file *file, void *fh,
			   struct v4l2_enum_dv_timings *t)
{
	if (t->pad || t->index >= ARRAY_SIZE(mz0380_supported_timings))
		return -EINVAL;
	memset(t->reserved, 0, sizeof(t->reserved));
	t->timings = mz0380_supported_timings[t->index];
	return 0;
}

int mz0380_dv_timings_cap(struct file *file, void *fh,
			  struct v4l2_dv_timings_cap *cap)
{
	*cap = mz0380_timings_cap;
	return 0;
}
