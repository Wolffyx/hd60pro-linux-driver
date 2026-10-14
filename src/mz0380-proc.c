// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 procfs: device list, capture state, and the HDMI receiver commands.
 *
 *   /proc/mz0380        (0444) cards, PCI identity and video node
 *   /proc/mz0380-state  (0444) pipeline, signal and delivery counters. Reading
 *                              it costs no card traffic, so it is safe to poll
 *                              while capturing (scripts/mz0380-m230-*.sh do).
 *   /proc/mz0380-hdmi   (0600) receiver commands; reading it shows the usage.
 */

#include <linux/uaccess.h>

#include "mz0380.h"

#define MZ0380_PROC_CMD_MAX	64

static int mz0380_proc_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&mz0380_devlist_lock);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  device     : %s\n", pci_name(dev->pci));
		seq_printf(m, "  pci id     : %04x:%04x\n",
			   dev->pci->vendor, dev->pci->device);
		seq_printf(m, "  subsystem  : %04x:%04x\n",
			   dev->pci->subsystem_vendor,
			   dev->pci->subsystem_device);
		seq_printf(m, "  board      : %s\n",
			   mz0380_boards[dev->board].name);
		seq_printf(m, "  firmware   : %u.%u (the card's own flash; the host never uploads)\n",
			   dev->fw_version_major, dev->fw_version_minor);
		if (dev->video_registered)
			seq_printf(m, "  video node : /dev/%s\n",
				   video_device_node_name(&dev->vdev));
		else
			seq_puts(m, "  video node : none\n");
	}
	mutex_unlock(&mz0380_devlist_lock);
	return 0;
}

static void mz0380_state_pipeline(struct seq_file *m, struct mz0380_dev *dev)
{
	u32 fourcc = mz0380_current_pixelformat(dev);

	seq_printf(m, "  pixelformat: %4.4s\n", (char *)&fourcc);
	seq_printf(m, "  frame size : %ux%u\n",
		   dev->capture.width, dev->capture.height);
	if (dev->capture.source_width && dev->capture.source_height)
		seq_printf(m, "  source     : %ux%u%c @ %u fps\n",
			   dev->capture.source_width,
			   dev->capture.source_height,
			   dev->capture.source_interlaced ? 'i' : 'p',
			   dev->capture.source_fps);
	else
		seq_puts(m, "  source     : unlocked\n");
	/*
	 * M169: the number that decides whether the next hardware run is safe.
	 * Per load - scripts/mz0380-spawns.sh accumulates it per power cycle.
	 */
	seq_printf(m, "  enc spawns : %u since insmod (card wedges somewhere in 8-18 per POWER CYCLE)\n",
		   dev->encoder_spawns);
	seq_printf(m, "  pipeline   : %s, VB2 %s, %u attachment(s), %u cached SPS/PPS bytes%s%s\n",
		   READ_ONCE(dev->pipeline_running) ? "running" : "stopped",
		   READ_ONCE(dev->streaming) ? "attached" : "detached",
		   dev->pipeline_attach_count, dev->h264_parameter_sets_len,
		   READ_ONCE(dev->pipeline_reconfigure_pending) ||
		   (READ_ONCE(dev->pipeline_running) &&
		    mz0380_pipeline_config_stale(dev)) ?
		   ", replacement pending" : "",
		   READ_ONCE(dev->pipeline_start_failed) ?
		   ", deferred start failed" : "");
	seq_printf(m, "  encoder    : bitrate=%u gop=%u, H.264 frame divisor %u\n",
		   dev->capture.enc_bitrate, dev->capture.enc_gop,
		   mz0380_h264_frame_divisor);
	seq_printf(m, "  irq        : %d total, %d frame events, %llu dropped before drain\n",
		   atomic_read(&dev->irq_count),
		   atomic_read(&dev->irq_video_count),
		   (unsigned long long)READ_ONCE(dev->frame_event_drops));
}

static void mz0380_state_delivery(struct seq_file *m, struct mz0380_dev *dev)
{
	seq_printf(m, "  h264 frames: %llu delivered, %llu dropped, %llu suppressed behind placeholder, %llu discarded while detached (pipeline lifetime)\n",
		   (unsigned long long)dev->h264_frames_delivered,
		   (unsigned long long)dev->h264_frames_dropped,
		   (unsigned long long)dev->h264_frames_suppressed,
		   (unsigned long long)dev->h264_frames_discarded);
	if (!dev->deliver_raw)
		return;
	seq_printf(m, "  raw frames : %llu delivered, %llu dropped, %llu stub completions skipped (post_skip=%u, %zu bytes per frame)\n",
		   (unsigned long long)dev->raw_frames_delivered,
		   (unsigned long long)dev->raw_frames_dropped,
		   (unsigned long long)dev->raw_stub_completions,
		   mz0380_post_skip, mz0380_raw_frame_bytes(dev));
	seq_printf(m, "  raw fills  : %llu slots skipped with luma still clear (M229), %llu with luma done but CHROMA still clear (M238), chroma test ran %llu times, best %u/%u samples at the clear value (M244), %llu colourless frames passed on a second look (M247)\n",
		   (unsigned long long)dev->raw_incomplete_tail,
		   (unsigned long long)dev->raw_incomplete_chroma,
		   (unsigned long long)dev->raw_chroma_probes,
		   dev->raw_chroma_max_clear, MZ0380_RAW_FILL_SAMPLES,
		   (unsigned long long)dev->raw_chroma_neutral);
	seq_printf(m, "  raw repeats: %llu identical-head observations, %llu torn frames discarded mid-copy, %llu completions with >1 slot ready (M222/M223)\n",
		   (unsigned long long)dev->raw_dup_content,
		   (unsigned long long)dev->raw_frames_torn,
		   (unsigned long long)dev->raw_multi_landed);
	seq_printf(m, "  raw rhythm : spacing in frame periods <.25/.5/1/1.5/2/>2.25 - card events %u/%u/%u/%u/%u/%u, frames taken %u/%u/%u/%u/%u/%u; slots ready per scan 0-4 %u/%u/%u/%u/%u; events per drain 1/2/3+ %u/%u/%u (M251)\n",
		   dev->cadence_event[0], dev->cadence_event[1],
		   dev->cadence_event[2], dev->cadence_event[3],
		   dev->cadence_event[4], dev->cadence_event[5],
		   dev->cadence_take[0], dev->cadence_take[1],
		   dev->cadence_take[2], dev->cadence_take[3],
		   dev->cadence_take[4], dev->cadence_take[5],
		   dev->cadence_ready[0], dev->cadence_ready[1],
		   dev->cadence_ready[2], dev->cadence_ready[3],
		   dev->cadence_ready[4], dev->cadence_batch[0],
		   dev->cadence_batch[1], dev->cadence_batch[2]);
}

/* M246: the audio window. */
static void mz0380_state_audio(struct seq_file *m, struct mz0380_dev *dev)
{
	if (!dev->audio_capable)
		return;
	seq_printf(m, "  audio      : %llu events, %llu slots, %llu bytes, %llu empty, %llu overrun, %llu bad token, %llu fifo drops, max extent %u, slots seen 0x%x, last event %08x token %08x (M246)\n",
		   (unsigned long long)dev->audio_events,
		   (unsigned long long)dev->audio_slots,
		   (unsigned long long)dev->audio_bytes,
		   (unsigned long long)dev->audio_empty,
		   (unsigned long long)dev->audio_overrun,
		   (unsigned long long)dev->audio_bad_token,
		   (unsigned long long)dev->audio_fifo_drops,
		   dev->audio_max_extent, dev->audio_slots_seen,
		   dev->audio_last_event, dev->audio_last_token);
	seq_printf(m, "  audio alsa : %llu bytes delivered, %llu periods, %llu pushes refused (why 0x%x: 1 no ring, 2 not open, 4 not running, 8 no sizes, 16 no buffer); audio for %llu ms (M246)\n",
		   (unsigned long long)dev->audio_alsa_bytes,
		   (unsigned long long)dev->audio_alsa_periods,
		   (unsigned long long)dev->audio_alsa_bails,
		   dev->audio_alsa_bail_why,
		   (unsigned long long)div_u64(dev->audio_last_ns -
					       dev->audio_first_ns,
					       NSEC_PER_MSEC));
	seq_printf(m, "  audio pcm  : %u opens, %u prepares, %u starts, %u stops, %u closes; refused %llu while closed, %llu while open but not started (M246)\n",
		   dev->audio_alsa_opens, dev->audio_alsa_prepares,
		   dev->audio_alsa_starts, dev->audio_alsa_stops,
		   dev->audio_alsa_closes,
		   (unsigned long long)dev->audio_alsa_bail_closed,
		   (unsigned long long)dev->audio_alsa_bail_idle);
}

static void mz0380_state_receiver(struct seq_file *m, struct mz0380_dev *dev)
{
	static const char * const cs_names[] = {
		"RGB", "YUV422", "YUV444", "undefined"
	};
	/* M252: AVI InfoFrame Q1:Q0 - what the source says its RGB range is */
	static const char * const q_names[] = {
		"default", "limited", "full", "reserved"
	};
	unsigned long last = READ_ONCE(dev->last_h264_frame_stamp);

	if (dev->mst_b2_48_valid)
		seq_printf(m, "  colourspace: source %s, RGB range %s (0x48=%02x 0x4a=%02x cached)",
			   cs_names[(dev->mst_b2_48 & 0x60) >> 5],
			   q_names[(dev->mst_b2_4a >> 2) & 3],
			   dev->mst_b2_48, dev->mst_b2_4a);
	else
		seq_puts(m, "  colourspace: not yet read");
	if (dev->mst_csc_applied_valid)
		seq_printf(m, ", CSC 0x92=%02x (%s)\n", dev->mst_csc_applied,
			   dev->mst_csc_applied ? "hdcapm RGB matrix" :
						  "bypass");
	else
		seq_puts(m, ", CSC never written\n");
	seq_printf(m, "  rearms     : %llu MST3367 AUTO_POSITION re-arms (only on real lock loss since M233)\n",
		   (unsigned long long)dev->mst_rearms);
	seq_printf(m, "  placeholder: %s, %llu NO SIGNAL IDRs delivered, %llu cadence misses, %llu raw NO SIGNAL frames delivered\n",
		   READ_ONCE(dev->no_signal_active) ?
		   "SHOWING (no usable source)" : "not shown (source is live)",
		   (unsigned long long)dev->no_signal_frames_delivered,
		   (unsigned long long)dev->no_signal_frames_missed,
		   (unsigned long long)dev->no_signal_raw_frames);
	if (last)
		seq_printf(m, "  recovery  : %s, last card frame %ums ago\n",
			   READ_ONCE(dev->signal_recovering) ? "active" : "idle",
			   jiffies_to_msecs(jiffies - last));
}

static int mz0380_proc_state_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&mz0380_devlist_lock);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  card       : handshake %s, firmware %u.%u, streaming %s\n",
			   mz0380_fw_state_name(dev->fw_state),
			   dev->fw_version_major, dev->fw_version_minor,
			   dev->dma_armed ? "available" : "UNAVAILABLE");
		if (dev->video_registered)
			seq_printf(m, "  video node : /dev/%s\n",
				   video_device_node_name(&dev->vdev));
		mz0380_state_pipeline(m, dev);
		mz0380_state_delivery(m, dev);
		mz0380_state_audio(m, dev);
		mz0380_state_receiver(m, dev);
	}
	mutex_unlock(&mz0380_devlist_lock);
	return 0;
}

/* ---- /proc/mz0380-hdmi --------------------------------------------------- */

static int mz0380_hdmi_watch(struct mz0380_dev *dev, const char *args)
{
	unsigned int secs = 20;

	if (*args && kstrtouint(args, 10, &secs))
		return -EINVAL;
	return mz0380_mst3367_watch(dev, clamp(secs, 1u, 120u));
}

static int mz0380_hdmi_hpd(struct mz0380_dev *dev, const char *args)
{
	unsigned int n = 5, gap = 4000;

	if (*args && sscanf(args, "%u %u", &n, &gap) < 1)
		return -EINVAL;
	return mz0380_mst3367_hpd_pulse(dev, clamp(n, 1u, 20u),
					clamp(gap, 200u, 10000u));
}

static int mz0380_hdmi_edid(struct mz0380_dev *dev, const char *args)
{
	return mz0380_mst3367_reload_edid(dev);
}

/*
 * M237: the CSC matrix is chosen from BANK2 0x48 at stream start and never
 * revisited, and a persistent pipeline survives a source mode change - so a
 * source that switches colour space keeps the old matrix. This re-reads and
 * re-applies it by hand.
 */
static int mz0380_hdmi_csc(struct mz0380_dev *dev, const char *args)
{
	int ret = mz0380_mst3367_refresh_colourspace(dev);

	if (!ret)
		mz0380_mst3367_apply_csc_mode(dev);
	return ret;
}

static const struct {
	const char *name;
	int (*fn)(struct mz0380_dev *dev, const char *args);
	const char *help;
} mz0380_hdmi_cmds[] = {
	{ "watch", mz0380_hdmi_watch,
	  "watch [secs]     log the receiver's detect state to dmesg (default 20 s)" },
	{ "csc", mz0380_hdmi_csc,
	  "csc              re-read the source colour space and re-apply the CSC" },
	{ "hpd", mz0380_hdmi_hpd,
	  "hpd [n] [gap_ms] pulse hot-plug n times (default 5 x 4000 ms)" },
	{ "edid", mz0380_hdmi_edid,
	  "edid             re-push the EDID and re-pulse hot-plug" },
};

static int mz0380_proc_hdmi_show(struct seq_file *m, void *v)
{
	unsigned int i;

	seq_puts(m, "usage: echo '<command>' > /proc/mz0380-hdmi   (root; results go to dmesg)\n");
	for (i = 0; i < ARRAY_SIZE(mz0380_hdmi_cmds); i++)
		seq_printf(m, "  %s\n", mz0380_hdmi_cmds[i].help);
	seq_puts(m, "Each command drives the receiver over the card's mailbox; avoid them while capturing.\n");
	return 0;
}

static ssize_t mz0380_proc_hdmi_write(struct file *file,
				      const char __user *buffer,
				      size_t count, loff_t *ppos)
{
	struct mz0380_dev *dev;
	char *buf, *cmd, *args;
	unsigned int i;
	int ret = -EINVAL;

	if (!count)
		return 0;
	if (*ppos || count >= MZ0380_PROC_CMD_MAX)
		return -EINVAL;

	buf = memdup_user_nul(buffer, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	args = strim(buf);
	cmd = strsep(&args, " \t");
	args = args ? skip_spaces(args) : "";

	for (i = 0; i < ARRAY_SIZE(mz0380_hdmi_cmds); i++) {
		if (strcmp(cmd, mz0380_hdmi_cmds[i].name))
			continue;
		ret = 0;
		mutex_lock(&mz0380_devlist_lock);
		list_for_each_entry(dev, &mz0380_devlist, devlist) {
			int r = mz0380_hdmi_cmds[i].fn(dev, args);

			if (r)
				ret = r;
		}
		mutex_unlock(&mz0380_devlist_lock);
		break;
	}
	kfree(buf);
	if (ret)
		return ret;
	*ppos += count;
	return count;
}

static int mz0380_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, mz0380_proc_show, NULL);
}

static int mz0380_proc_state_open(struct inode *inode, struct file *file)
{
	return single_open(file, mz0380_proc_state_show, NULL);
}

static int mz0380_proc_hdmi_open(struct inode *inode, struct file *file)
{
	return single_open(file, mz0380_proc_hdmi_show, NULL);
}

static const struct proc_ops mz0380_proc_fops = {
	.proc_open = mz0380_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops mz0380_proc_state_fops = {
	.proc_open = mz0380_proc_state_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops mz0380_proc_hdmi_fops = {
	.proc_open = mz0380_proc_hdmi_open,
	.proc_read = seq_read,
	.proc_write = mz0380_proc_hdmi_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

void mz0380_proc_remove(void)
{
	remove_proc_entry("mz0380-hdmi", NULL);
	remove_proc_entry("mz0380-state", NULL);
	remove_proc_entry("mz0380", NULL);
}

int mz0380_proc_create(void)
{
	if (!proc_create("mz0380", 0444, NULL, &mz0380_proc_fops) ||
	    !proc_create("mz0380-state", 0444, NULL, &mz0380_proc_state_fops) ||
	    !proc_create("mz0380-hdmi", 0600, NULL, &mz0380_proc_hdmi_fops)) {
		mz0380_proc_remove();
		return -ENOMEM;
	}
	return 0;
}
