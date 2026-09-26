// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 procfs debug, scan, event, HDMI, and raw-command handlers.
 */

#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>

#include "mz0380-internal.h"

int mz0380_proc_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
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
		seq_printf(m, "  win driver : %s\n",
			   mz0380_boards[dev->board].windows_driver);
		if (dev->video_registered)
			seq_printf(m, "  video node : /dev/%s\n",
				   video_device_node_name(&dev->vdev));
		else if (!mz0380_enable_video)
			seq_puts(m,
				 "  video node : disabled (load with enable_video=1 to register /dev/video*)\n");
		seq_puts(m,
			 "  firmware   : card's own flash image (host never uploads)\n");
		seq_printf(m, "  bar%d start: 0x%llx\n",
			   dev->bar_nr[MZ0380_MAP_BAR_MMIO],
			   (unsigned long long)dev->bar_start[MZ0380_MAP_BAR_MMIO]);
		seq_printf(m, "  bar%d len  : 0x%llx\n",
			   dev->bar_nr[MZ0380_MAP_BAR_MMIO],
			   (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_MMIO]);
		seq_printf(m, "  bar%d start: 0x%llx\n",
			   dev->bar_nr[MZ0380_MAP_BAR_CFG],
			   (unsigned long long)dev->bar_start[MZ0380_MAP_BAR_CFG]);
		seq_printf(m, "  bar%d len  : 0x%llx\n",
			   dev->bar_nr[MZ0380_MAP_BAR_CFG],
			   (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_CFG]);

		if (procfs_verbosity > 1)
			seq_puts(m, "  BAR scan disabled: wide MMIO reads proved unsafe on this hardware\n");
	}
	mutex_unlock(&devlist);

	return 0;
}

int mz0380_proc_snapshot_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  device     : %s\n", pci_name(dev->pci));
		seq_printf(m, "  board      : %s\n",
			   mz0380_boards[dev->board].name);
		mz0380_dump_snapshot(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

int mz0380_proc_control_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  device     : %s\n", pci_name(dev->pci));
		seq_printf(m, "  board      : %s\n",
			   mz0380_boards[dev->board].name);
		mz0380_dump_control_path(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

int mz0380_proc_experiment_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  device     : %s\n", pci_name(dev->pci));
		seq_printf(m, "  board      : %s\n",
			   mz0380_boards[dev->board].name);
		mz0380_dump_input_experiment(m, dev);
		mz0380_dump_quality_experiment(m, dev);
		mz0380_dump_gop_experiment(m, dev);
		mz0380_dump_b_frames_experiment(m, dev);
		mz0380_dump_qp_step_experiment(m, dev);
		mz0380_dump_bitrate_experiment(m, dev);
		mz0380_dump_record_mode_experiment(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

/*
 * BAR0 offsets proven to complete a readl without stalling the PCIe link:
 * these are already read live by mz0380_mailbox_scan() (0x00..0x60) and
 * mz0380_signal_from_bar0() (0xa8..0xe4). A blind read of an un-backed BAR0
 * offset post-boot does NOT return 0xffffffff - it stalls the CPU on the readl
 * until the host completion timeout, which presents as a hard hang. So the scan
 * only reads inside these ranges unless the operator opts in with scan_unsafe=1.
 */
struct mz0380_safe_range {
	u32 start;	/* inclusive */
	u32 end;	/* exclusive */
};

static const struct mz0380_safe_range mz0380_scan_safe_mmio[] = {
	{ 0x0000, 0x0060 },	/* bootloader / mailbox aperture */
	{ 0x00a0, 0x00e8 },	/* sc0710-style HDMI status block  */
};

static bool mz0380_scan_offset_safe(unsigned int map, u32 reg)
{
	unsigned int i;

	/* BAR5/CFG is a small fully-backed 4K window; sweeping it is safe. */
	if (map != MZ0380_MAP_BAR_MMIO)
		return true;

	for (i = 0; i < ARRAY_SIZE(mz0380_scan_safe_mmio); i++)
		if (reg >= mz0380_scan_safe_mmio[i].start &&
		    reg < mz0380_scan_safe_mmio[i].end)
			return true;

	return false;
}

/*
 * Raw contiguous register-window dump for signal-source discovery.
 *
 * Emits one 4-byte register per line in a diff-stable format:
 *     bar0[0x00a8] = 12345678
 * so two captures (HDMI source plugged vs unplugged) can be compared with
 * plain diff(1) to see exactly which register(s) track cable/lock state.
 * The window (BAR, start, length) is retunable live via the scan_bar /
 * scan_start / scan_len module params under /sys/module/mz0380/parameters/.
 * Read-only and bounded to the mapped BAR length - never touches the card.
 */
static void mz0380_dump_scan_window(struct seq_file *m, struct mz0380_dev *dev)
{
	unsigned int map = scan_bar;
	u32 start = scan_start & ~0x3u;
	resource_size_t bar_len;
	u64 end;
	u32 reg;

	if (map >= MZ0380_MAX_MAPS || !dev->lmmio[map]) {
		seq_printf(m, "  scan       : bar index %u unmapped\n", map);
		return;
	}

	bar_len = dev->bar_len[map];
	end = (u64)start + (scan_len ? scan_len : 0x48);
	if (end > bar_len)
		end = bar_len;

	seq_printf(m,
		   "  scan bar%d 0x%04x..0x%04llx (len 0x%x, bar_len 0x%llx, mode %s)\n",
		   dev->bar_nr[map], start, end, scan_len,
		   (unsigned long long)bar_len,
		   scan_unsafe ? "UNSAFE-all-offsets" : "safe-ranges-only");

	if ((u64)start >= end) {
		seq_puts(m, "  scan       : empty window (start past bar_len)\n");
		return;
	}

	for (reg = start; reg < end; reg += 4) {
		if (!scan_unsafe && !mz0380_scan_offset_safe(map, reg)) {
			seq_printf(m,
				   "  bar%d[0x%04x] = ........ (skipped: outside safe range; scan_unsafe=1 to read)\n",
				   dev->bar_nr[map], reg);
			continue;
		}

		/*
		 * Log the target before the readl so that if an un-backed
		 * offset stalls the link, the last line in dmesg after the
		 * (possibly forced) recovery pinpoints the culprit.
		 */
		if (scan_unsafe)
			pr_info("%s: scan probing bar%d[0x%04x]\n",
				dev->name, dev->bar_nr[map], reg);

		seq_printf(m, "  bar%d[0x%04x] = %08x\n",
			   dev->bar_nr[map], reg, mz_read(dev, map, reg));
	}
}

int mz0380_proc_scan_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		mz0380_dump_scan_window(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

/*
 * Peripheral register-file scan for signal-source discovery.
 *
 * Unlike /proc/mz0380-scan (raw BAR0 reads, most of which are un-backed
 * post-boot), this walks a chip's registers through the proven mailbox
 * REG_READ (0x1a) command path - the same route mz0380_periph_read() uses to
 * reach the HDMI bridge (chip 0x90) and the TVP5160 (0xb8). Each register is
 * emitted diff-stably as:
 *     periph[0x90][0x12] = 00000001
 * so a plugged-vs-unplugged diff reveals which bridge register actually tracks
 * HDMI signal lock (the old 0x12 "bit0 = signal present" guess is unverified).
 * A short per-read timeout bounds the cost of a non-responding register.
 */
static void mz0380_dump_periph_scan(struct seq_file *m, struct mz0380_dev *dev)
{
	u8 chip = periph_chip & 0xff;
	unsigned int reg;
	unsigned int start = periph_start & 0xff;
	unsigned int end = start + periph_count;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		seq_puts(m, "  periph     : card handshake not complete\n");
		return;
	}
	if (end > 0x100)
		end = 0x100;

	/*
	 * Diagnostic: the read-back result slot for REG_READ is a guess
	 * (periph_read() reads PARAM3). If a bridge scan returns all-zero, the
	 * card may be writing the value into a different slot. Issue REG_READ
	 * for the first few registers and dump every candidate return word so
	 * the real result slot is visible. The transaction helper snapshots the
	 * opcode plus the ten real command words and copies them while cmd_lock is
	 * still held. EVENT/payload are re-read separately (all within the safe
	 * mailbox aperture).
	 */
	if (periph_probe) {
		unsigned int n = min(periph_count, 4u);
		unsigned int i;

		seq_printf(m,
			   "  periph PROBE: REG_READ(0x1a) chip 0x%02x, full slot dump for %u reg(s)\n",
			   chip, n ? n : 1);
		for (reg = start; reg < start + (n ? n : 1); reg++) {
			u32 params[3] = { chip, reg, 0 };
			u32 reply[MZ0380_MB_COMMAND_WORDS] = { 0 };
			u32 status = 0;
			int ret = mz0380_send_command_reply(
				dev, MZ0380_CMD_REG_READ, params, 3,
				&status, 500, reply, ARRAY_SIZE(reply));

			seq_printf(m, "  --- reg 0x%02x: send ret=%d STATUS[0x2c]=%08x ---\n",
				   reg, ret, status);
			for (i = 0; i < MZ0380_MB_COMMAND_WORDS; i++)
				seq_printf(m, "    PARAM(%2u)[bar0+0x%02x] = %08x%s\n",
					   i, MZ0380_MB_PARAM(i), reply[i],
					   i == 0 ? "  <- opcode echo" :
					   i == 1 ? "  <- RESULT slot"  :
					   i == 3 ? "  <- periph_read() reads here" : "");
			seq_printf(m, "    EVENT[0x30]      = %08x\n",
				   mz_mmio_read(dev, MZ0380_MB_EVENT));
			seq_printf(m, "    PAYLOAD0[0x40]   = %08x\n",
				   mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD0));
			seq_printf(m, "    PAYLOAD1[0x44]   = %08x\n",
				   mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1));
			seq_printf(m, "    PAYLOAD2[0x48]   = %08x\n",
				   mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2));
			seq_printf(m, "    PAYLOAD3[0x4c]   = %08x\n",
				   mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3));
		}
		return;
	}

	seq_printf(m,
		   "  periph chip 0x%02x regs 0x%02x..0x%02x via REG_READ(0x1a)%s\n",
		   chip, start, end ? end - 1 : start,
		   dev->dma_armed ? "" :
		   " [dma not armed - reads may fail; load dma_handshake=1]");

	for (reg = start; reg < end; reg++) {
		u32 value = 0;
		int ret = mz0380_periph_read(dev, chip, reg, &value);

		if (ret)
			seq_printf(m,
				   "  periph[0x%02x][0x%02x] = ........ (err %d)\n",
				   chip, reg, ret);
		else
			seq_printf(m, "  periph[0x%02x][0x%02x] = %08x\n",
				   chip, reg, value);
	}
}

int mz0380_proc_periph_scan_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		mz0380_dump_periph_scan(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

/* ===== live card-event watcher ==========================================
 *
 * The card notifies signal/format changes as edge events on the BAR0 EVENT
 * word (0x30), carrying data in the payload words at 0x40..0x4c, rather than
 * exposing a pollable status register. A kthread samples EVENT at high rate and
 * records each edge into a per-device ring; /proc/mz0380-events dumps the ring
 * (read) and starts/stops/clears the watcher (write). Snapshot the payloads
 * before acking, since the ack clears the event.
 */
static void mz0380_event_record(struct mz0380_dev *dev, u32 event)
{
	struct mz0380_event_rec snap;
	unsigned long flags;

	snap.t_ns       = local_clock();
	snap.event      = event;
	snap.payload[0] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD0);
	snap.payload[1] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1);
	snap.payload[2] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2);
	snap.payload[3] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3);
	snap.status     = mz_mmio_read(dev, MZ0380_MB_STATUS);
	snap.intflag    = mz_cfg_read(dev, MZ0380_CFG_INT_FLAG);

	spin_lock_irqsave(&dev->event_lock, flags);
	dev->event_ring[dev->event_head] = snap;
	dev->event_head = (dev->event_head + 1) % MZ0380_EVENT_RING_SIZE;
	if (dev->event_count < MZ0380_EVENT_RING_SIZE)
		dev->event_count++;
	dev->event_seen++;
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static int mz0380_event_thread(void *data)
{
	static const u16 tok_reg[5] = { 0x40, 0x44, 0x48, 0x4c, 0x50 };
	struct mz0380_dev *dev = data;
	u64 tok_changes[5] = { 0 };
	u32 tok_last[5];
	bool tok_valid = false;
	unsigned long next_print = jiffies;
	unsigned long next_kick = jiffies;
	unsigned int kicks = 0;
	u32 last = 0;
	unsigned int i;

	while (!kthread_should_stop()) {
		u32 tok[5];
		bool tok_diff = false;
		u32 event = mz_mmio_read(dev, MZ0380_MB_EVENT);

		/* M35: token words are written ungated by store_channel_done */
		for (i = 0; i < ARRAY_SIZE(tok_reg); i++) {
			tok[i] = mz_mmio_read(dev, tok_reg[i]);
			if (tok_valid && tok[i] != tok_last[i]) {
				tok_diff = true;
				tok_changes[i]++;
			}
			tok_last[i] = tok[i];
		}
		if (tok_diff && time_after_eq(jiffies, next_print)) {
			pr_info("%s: live token EVENT=%08x tok40=%08x 44=%08x 48=%08x 4c=%08x enc50=%08x\n",
				dev->name, event, tok[0], tok[1], tok[2],
				tok[3], tok[4]);
			next_print = jiffies + HZ / 20;
		}
		tok_valid = true;

		if (credit_kick_ms &&
		    time_after_eq(jiffies, next_kick)) {
			mutex_lock(&dev->cmd_lock);
			mz0380_mb_ack_event(dev);
			mutex_unlock(&dev->cmd_lock);
			kicks++;
			pr_info("%s: credit kick #%u (ack sequence fired)\n",
				dev->name, kicks);
			next_kick = jiffies + msecs_to_jiffies(credit_kick_ms);
		}

		if (event && event != last) {
			/*
			 * Serialise the ack (which rings the 0x400 doorbell)
			 * with the command channel so an in-flight command is
			 * never aborted mid-flight.
			 */
			mutex_lock(&dev->cmd_lock);
			event = mz_mmio_read(dev, MZ0380_MB_EVENT);
			if (event) {
				mz0380_event_record(dev, event);
				if (event_auto_ack)
					mz0380_mb_ack_event(dev);
			}
			mutex_unlock(&dev->cmd_lock);
			last = event_auto_ack ? 0 : event;
		} else if (!event) {
			last = 0;
		}
		usleep_range(event_sample_us, event_sample_us + 50);
	}
	pr_info("%s: token watch summary: changes tok40=%llu 44=%llu 48=%llu 4c=%llu enc50=%llu, kicks=%u\n",
		dev->name, tok_changes[0], tok_changes[1], tok_changes[2],
		tok_changes[3], tok_changes[4], kicks);
	return 0;
}

static void mz0380_event_watch_start(struct mz0380_dev *dev)
{
	if (dev->event_watching)
		return;
	dev->event_kthread = kthread_run(mz0380_event_thread, dev,
					 "mz0380-events/%u", dev->nr);
	if (IS_ERR(dev->event_kthread)) {
		dev->event_kthread = NULL;
		return;
	}
	dev->event_watching = true;
}

void mz0380_event_watch_stop(struct mz0380_dev *dev)
{
	if (dev->event_kthread) {
		kthread_stop(dev->event_kthread);
		dev->event_kthread = NULL;
	}
	dev->event_watching = false;
}

static void mz0380_event_ring_clear(struct mz0380_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	dev->event_head = 0;
	dev->event_count = 0;
	dev->event_seen = 0;
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static void mz0380_dump_events(struct seq_file *m, struct mz0380_dev *dev)
{
	struct mz0380_event_rec *snap;
	unsigned int count, head, i, idx;
	u64 seen, t0;
	unsigned long flags;

	seq_printf(m, "  watcher    : %s (sample %u us, auto_ack %d)\n",
		   dev->event_watching ? "running" : "stopped",
		   event_sample_us, event_auto_ack);
	seq_printf(m,
		   "  live       : EVENT[0x30]=%08x STATUS[0x2c]=%08x intflag=%08x\n",
		   mz_mmio_read(dev, MZ0380_MB_EVENT),
		   mz_mmio_read(dev, MZ0380_MB_STATUS),
		   mz_cfg_read(dev, MZ0380_CFG_INT_FLAG));

	snap = kmalloc_array(MZ0380_EVENT_RING_SIZE, sizeof(*snap), GFP_KERNEL);
	if (!snap) {
		seq_puts(m, "  (out of memory)\n");
		return;
	}

	spin_lock_irqsave(&dev->event_lock, flags);
	count = dev->event_count;
	head = dev->event_head;
	seen = dev->event_seen;
	for (i = 0; i < count; i++) {
		idx = (head - count + i + MZ0380_EVENT_RING_SIZE) %
		      MZ0380_EVENT_RING_SIZE;
		snap[i] = dev->event_ring[idx];
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);

	seq_printf(m, "  recorded   : %u shown of %llu total edges%s\n",
		   count, seen,
		   seen > count ? " (ring wrapped, oldest lost)" : "");
	if (!count) {
		seq_puts(m,
			 "  (none; 'echo start > /proc/mz0380-events', toggle the HDMI source, then re-read)\n");
		kfree(snap);
		return;
	}

	t0 = snap[0].t_ns;
	for (i = 0; i < count; i++)
		seq_printf(m,
			   "  +%9llu us  EVENT=%08x  p=%08x %08x %08x %08x  STATUS=%08x intflag=%08x\n",
			   (unsigned long long)(snap[i].t_ns - t0) / 1000,
			   snap[i].event,
			   snap[i].payload[0], snap[i].payload[1],
			   snap[i].payload[2], snap[i].payload[3],
			   snap[i].status, snap[i].intflag);

	kfree(snap);
}

int mz0380_proc_events_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		mz0380_dump_events(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

ssize_t mz0380_proc_events_write(struct file *file,
					const char __user *buffer,
					size_t count, loff_t *ppos)
{
	struct mz0380_dev *dev;
	char *cmd;
	ssize_t ret = -EINVAL;

	if (!count || *ppos != 0)
		return count ? -EINVAL : 0;
	if (count >= MZ0380_PROC_CMD_MAX)
		return -E2BIG;

	cmd = memdup_user_nul(buffer, count);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);
	strim(cmd);

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		if (!strcmp(cmd, "start")) {
			mz0380_event_watch_start(dev);
			ret = count;
		} else if (!strcmp(cmd, "stop")) {
			mz0380_event_watch_stop(dev);
			ret = count;
		} else if (!strcmp(cmd, "clear")) {
			mz0380_event_ring_clear(dev);
			ret = count;
		} else if (!strcmp(cmd, "repoison")) {
			/* M38: re-fill the stream bufs with poison mid-stream */
			mz0380_extent_repoison(dev);
			ret = count;
		}
	}
	mutex_unlock(&devlist);

	if (ret > 0)
		*ppos += count;
	kfree(cmd);
	return ret;
}

/*
 * Select the real HDMI front end without spawning the encoder.  The old proc
 * command packed an unverified raw input code and geometry into SET_VIC; that
 * opcode configures/spawns tinyvenc and belongs only in stream start.  Property
 * 201 uses the V4L2 input index instead, where HDMI is confirmed as index 0.
 * Refreshing the MST3367 sink then drives HPD low, reloads EDID, and raises HPD
 * so the source sees a coherent sink transition.
 */
static int mz0380_activate_hdmi_sink(struct mz0380_dev *dev)
{
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_warn("%s: HDMI sink refresh skipped - card handshake not complete (load dma_handshake=1)\n",
			dev->name);
		return -ENODEV;
	}

	ret = mz0380_request_input_select(dev, 0, "hdmi-proc");
	if (ret) {
		pr_warn("%s: HDMI property-%u index-0 select failed (%d)\n",
			dev->name, MZ0380_INPUT_SELECT_PROPERTY, ret);
		return ret;
	}

	ret = mz0380_mst3367_reload_edid(dev);
	pr_info("%s: HDMI property-%u index 0 selected; MST3367 EDID/HPD refresh ret=%d (SET_VIC deferred to stream start)\n",
		dev->name, MZ0380_INPUT_SELECT_PROPERTY, ret);
	return ret;
}

int mz0380_proc_hdmi_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	seq_puts(m, "usage: echo hdmi > /proc/mz0380-hdmi\n");
	seq_puts(m, "  selects confirmed V4L2/property-201 HDMI index 0, reloads EDID, and pulses HPD.\n");
	seq_puts(m, "  It does not send SET_VIC or start the encoder; those happen only at stream start.\n");
	seq_puts(m, "  Legacy numeric forms accept property index 0 or raw HDMI code 2 only; geometry is ignored.\n");
	seq_puts(m, "  Raw DVI/component/SDI/auto input codes are rejected. Other commands: csc, edid, hpd, watch, ramtest, wscan, edidhunt, gpiodump, i2cscan.\n");
	seq_puts(m, "\nsink chain read-back (M43: write returns prove nothing - the\n"
		    "firmware forces the I2C result to 0 on a NAK, so verify by reading):\n");
	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist)
		mz0380_mst3367_diag(dev, m);
	mutex_unlock(&devlist);
	return 0;
}

ssize_t mz0380_proc_hdmi_write(struct file *file,
				      const char __user *buffer,
				      size_t count, loff_t *ppos)
{
	struct mz0380_dev *dev;
	u32 input = 0, width = 1920, height = 1080, fps = 60;
	char *cmd;

	if (!count || *ppos != 0)
		return count ? -EINVAL : 0;
	if (count >= MZ0380_PROC_CMD_MAX)
		return -E2BIG;

	cmd = memdup_user_nul(buffer, count);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);
	strim(cmd);
	if (!strcmp(cmd, "hdmi"))
		goto activate_hdmi;

	/*
	 * M237: "csc" re-reads the source colour space and re-applies the CSC.
	 *
	 * The CSC matrix is chosen from BANK2 0x48 at stream start and never
	 * revisited, and with persistent_h264 a source mode change does not
	 * restart the pipeline - so a camera switching between 50 and 60 Hz
	 * keeps whatever matrix was right for the mode before it. This is both
	 * the experiment (does 0x48 differ between modes?) and the manual
	 * workaround if it does.
	 */
	if (!strcmp(cmd, "csc")) {
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist) {
			if (!mz0380_mst3367_refresh_colourspace(dev))
				mz0380_mst3367_apply_csc_mode(dev);
		}
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M44: "ramtest" probes whether MST3367 BANK3 is writable RAM */
	if (!strcmp(cmd, "ramtest")) {
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_mst3367_ramtest(dev);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M49: "wscan" hunts writable RAM windows (EDID store) */
	if (!strcmp(cmd, "wscan")) {
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_mst3367_wscan(dev);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M48: "hpd [count] [gap_ms]" pulses HPD only, nothing else */
	if (!strncmp(cmd, "hpd", 3)) {
		unsigned int n = 5, gap = 4000;

		sscanf(cmd, "hpd %u %u", &n, &gap);
		n = clamp(n, 1u, 20u);
		gap = clamp(gap, 200u, 10000u);
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_mst3367_hpd_pulse(dev, n, gap);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M47: "edid" re-pushes the EDID and re-pulses HPD */
	if (!strcmp(cmd, "edid")) {
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_mst3367_reload_edid(dev);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M53: "edidhunt" - hunt an indirect address/data port into EDID RAM */
	if (!strcmp(cmd, "edidhunt")) {
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_mst3367_edidhunt(dev);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M51b: "gpiodump" - idle level of every GPIO pin (pull-up hunt) */
	if (!strcmp(cmd, "gpiodump")) {
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_gpio_dump(dev);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M51: "i2cscan [sda] [scl]" bit-bang scan of a spare-GPIO I2C bus */
	if (!strncmp(cmd, "i2cscan", 7)) {
		unsigned int sda = 13, scl = 12;

		sscanf(cmd, "i2cscan %u %u", &sda, &scl);
		kfree(cmd);
		if (sda > 31 || scl > 31 || sda == scl)
			return -EINVAL;
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_i2cbb_scan(dev, sda, scl);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/* M45: "watch [secs]" logs the detect block live to dmesg */
	if (!strncmp(cmd, "watch", 5)) {
		unsigned int secs = 20;

		sscanf(cmd, "watch %u", &secs);
		secs = clamp(secs, 1u, 120u);
		kfree(cmd);
		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist)
			mz0380_mst3367_watch(dev, secs);
		mutex_unlock(&devlist);
		*ppos += count;
		return count;
	}

	/*
	 * Anything left must be the deprecated numeric "input width height fps"
	 * form. A word that matched no command above is a typo - or, more usefully,
	 * a
	 * command this module is too old to know, which is what a stale
	 * insmod looks like. Falling through would silently run an HDMI
	 * activation with default geometry and log nothing about the real
	 * request, so reject it and name the vocabulary instead.
	 */
	if (*cmd && !(*cmd >= '0' && *cmd <= '9')) {
		pr_info("mz0380: unknown /proc/mz0380-hdmi command '%s' - known: hdmi, ramtest, wscan, edidhunt, gpiodump, i2cscan, hpd, edid, watch, or legacy '0|2 [w h fps]'\n",
			cmd);
		kfree(cmd);
		return -EINVAL;
	}

	{
		u32 values[4];
		unsigned int n = 0;
		char *p = cmd;
		char *tok;

		while ((tok = strsep(&p, " \t")) != NULL) {
			if (!*tok)
				continue;
			if (n == ARRAY_SIZE(values) ||
			    kstrtou32(tok, 0, &values[n])) {
				pr_warn("mz0380: malformed legacy HDMI request; use 'hdmi'\n");
				kfree(cmd);
				return -EINVAL;
			}
			n++;
		}
		if (!n) {
			kfree(cmd);
			return -EINVAL;
		}

		input = values[0];
		if (n > 1)
			width = values[1];
		if (n > 2)
			height = values[2];
		if (n > 3)
			fps = values[3];
	}

	/* 0 is property-201 HDMI; 2 is accepted only as the old raw HDMI code. */
	if (input != 0 && input != MZ0380_INPUT_CODE_HDMI) {
		pr_warn("mz0380: rejected non-HDMI raw input code %u; use 'hdmi' (property-201 index 0)\n",
			input);
		kfree(cmd);
		return -EINVAL;
	}
	pr_warn("mz0380: legacy HDMI request '%u %u %u %u': geometry is ignored; selecting property-201 index 0 and refreshing EDID/HPD\n",
		input, width, height, fps);

activate_hdmi:
	kfree(cmd);

	{
		ssize_t ret = count;
		bool found = false;

		mutex_lock(&devlist);
		list_for_each_entry(dev, &mz0380_devlist, devlist) {
			int err;

			found = true;
			err = mz0380_activate_hdmi_sink(dev);
			if (err && ret > 0)
				ret = err;
		}
		mutex_unlock(&devlist);
		if (!found)
			return -ENODEV;
		if (ret < 0)
			return ret;
	}

	*ppos += count;
	return count;
}

/*
 * Generic mailbox command passthrough for RE (RE_FINDINGS.md M7). Sends an
 * arbitrary opcode + params and dumps the returned status/param slots, so the
 * front-end/HPD bring-up can be probed without a recompile per opcode - e.g.
 * GPIO direction/data (op 0x17/0x15, SL6010 GPIO props 940/941), config banks
 * (op 0x02/0x04/0x08). Root-only (0644) and gated on firmware-ready.
 */
static int mz0380_raw_command_locked(struct mz0380_dev *dev, u32 opcode,
				     const u32 *params, unsigned int nparams)
{
	u32 reply[4] = { 0 };
	u32 status = 0;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_warn("%s: raw cmd skipped - firmware not ready\n", dev->name);
		return -ENODEV;
	}
	if (nparams > MZ0380_MB_MAX_ARGS)
		return -E2BIG;

	ret = mz0380_send_command_reply(dev, opcode, params, nparams,
					   &status, 500, reply,
					   ARRAY_SIZE(reply));
	pr_info("%s: raw cmd op=0x%02x n=%u ret=%d status=0x%08x out=%08x %08x %08x %08x\n",
		dev->name, opcode, nparams, ret, status,
		reply[0], reply[1], reply[2], reply[3]);
	return ret;
}

int mz0380_proc_cmd_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	seq_puts(m, "usage: echo \"<opcode> [p0 p1 ...]\" > /proc/mz0380-cmd  (hex 0x.. or dec)\n");
	seq_puts(m, "  sends one mailbox command; results go to dmesg (status + first 4 return slots).\n");
	seq_puts(m, "  e.g. GPIO all-out: '0x17 0xffff' then all-high: '0x15 0xffff'; read GPIO: '0x14'.\n");
	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist)
		seq_printf(m, "%s: fw %s\n", dev->name,
			   mz0380_fw_state_name(dev->fw_state));
	mutex_unlock(&devlist);
	return 0;
}

ssize_t mz0380_proc_cmd_write(struct file *file,
				     const char __user *buffer,
				     size_t count, loff_t *ppos)
{
	u32 vals[MZ0380_MB_COMMAND_WORDS] = { 0 };
	struct mz0380_dev *dev;
	unsigned int n = 0;
	char *cmd, *p, *tok;

	if (!count || *ppos != 0)
		return count ? -EINVAL : 0;
	if (count >= MZ0380_PROC_CMD_MAX)
		return -E2BIG;

	cmd = memdup_user_nul(buffer, count);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);
	strim(cmd);

	p = cmd;
	while ((tok = strsep(&p, " \t")) != NULL) {
		if (!*tok)
			continue;
		if (n == ARRAY_SIZE(vals)) {
			kfree(cmd);
			return -E2BIG;
		}
		if (kstrtou32(tok, 0, &vals[n])) {
			kfree(cmd);
			return -EINVAL;
		}
		n++;
	}
	kfree(cmd);
	if (n < 1)
		return -EINVAL;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist)
		mz0380_raw_command_locked(dev, vals[0], &vals[1], n - 1);
	mutex_unlock(&devlist);

	*ppos += count;
	return count;
}
