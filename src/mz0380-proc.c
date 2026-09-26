// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 procfs state, file operations, card log, and registration.
 */

#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>

#include "mz0380-internal.h"

static int mz0380_proc_state_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;
	u16 pci_command = 0;
	u16 pci_status = 0;
	u8 pci_irq_line = 0;
	u8 pci_irq_pin = 0;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		pci_read_config_word(dev->pci, PCI_COMMAND, &pci_command);
		pci_read_config_word(dev->pci, PCI_STATUS, &pci_status);
		pci_read_config_byte(dev->pci, PCI_INTERRUPT_LINE, &pci_irq_line);
		pci_read_config_byte(dev->pci, PCI_INTERRUPT_PIN, &pci_irq_pin);

		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  rev        : %u\n", dev->pci_rev);
		seq_printf(m, "  latency    : %u\n", dev->pci_lat);
		seq_printf(m, "  irq        : %u\n", dev->pci->irq);
		seq_printf(m, "  pci cmd    : %04x\n", pci_command);
		seq_printf(m, "  pci status : %04x\n", pci_status);
		seq_printf(m, "  bus master : %s\n",
			   pci_command & PCI_COMMAND_MASTER ? "enabled" : "disabled");
		seq_printf(m, "  probe mode : %s\n",
			   allow_bus_master ? "bus-master enabled" : "probe-safe");
		seq_printf(m, "  irq line   : %02x\n", pci_irq_line);
		seq_printf(m, "  irq pin    : %02x\n", pci_irq_pin);
		mz0380_dump_pci_caps(m, dev);
		seq_printf(m, "  mmio[0x0000] = %08x\n",
			   mz_read(dev, MZ0380_MAP_BAR_MMIO, 0x0000));
		seq_printf(m, "  mmio[0x0004] = %08x\n",
			   mz_read(dev, MZ0380_MAP_BAR_MMIO, 0x0004));
		seq_printf(m, "  cfg[0x0000]  = %08x\n",
			   mz_read(dev, MZ0380_MAP_BAR_CFG, 0x0000));
		seq_printf(m, "  cfg[0x0004]  = %08x\n",
			   mz_read(dev, MZ0380_MAP_BAR_CFG, 0x0004));
		mz0380_fw_info_dump(m, dev);
		mz0380_video_state_dump(m, dev);
		if (procfs_verbosity > 1)
			mz0380_dump_pci_config(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

static ssize_t mz0380_proc_experiment_write(struct file *file,
					    const char __user *buffer,
					    size_t count, loff_t *ppos)
{
	char *cmd;
	u32 reg;
	u32 mask;
	u32 shift;
	unsigned int input;
	unsigned int quality;
	unsigned int gop;
	unsigned int b_frames;
	unsigned int qp_step;
	unsigned int bitrate;
	unsigned int mode;
	ssize_t ret = -EINVAL;
	struct mz0380_dev *dev = NULL;

	if (!count)
		return 0;

	if (*ppos != 0)
		return -EINVAL;

	if (count >= MZ0380_PROC_CMD_MAX)
		return -E2BIG;

	cmd = memdup_user_nul(buffer, count);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);

	strim(cmd);
	if (!strcmp(cmd, "writes on")) {
		allow_experimental_writes = true;
		pr_info("mz0380: experimental BAR5 writes enabled via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "writes off")) {
		allow_experimental_writes = false;
		pr_info("mz0380: experimental BAR5 writes disabled via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "candidate clear")) {
		mz0380_input_candidate_clear();
		pr_info("mz0380: property 201 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "recordmode-candidate clear")) {
		mz0380_record_mode_candidate_clear();
		pr_info("mz0380: property 407 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "bitrate-candidate clear")) {
		mz0380_bitrate_candidate_clear();
		pr_info("mz0380: property 403 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "quality-candidate clear")) {
		mz0380_quality_candidate_clear();
		pr_info("mz0380: property 404 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "gop-candidate clear")) {
		mz0380_gop_candidate_clear();
		pr_info("mz0380: property 405 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "bframes-candidate clear")) {
		mz0380_b_frames_candidate_clear();
		pr_info("mz0380: property 411 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (!strcmp(cmd, "qpstep-candidate clear")) {
		mz0380_qp_step_candidate_clear();
		pr_info("mz0380: property 408 candidate register cleared via procfs\n");
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_input_candidate_set(reg, mask, shift);
		pr_info("mz0380: property 201 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
			input_select_reg, input_select_mask,
			input_select_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "bitrate-candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_bitrate_candidate_set(reg, mask, shift);
		pr_info("mz0380: property 403 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
			bitrate_reg, bitrate_mask, bitrate_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "quality-candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_quality_candidate_set(reg, mask, shift);
		pr_info("mz0380: property 404 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
			quality_reg, quality_mask, quality_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "gop-candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_gop_candidate_set(reg, mask, shift);
		pr_info("mz0380: property 405 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
			gop_reg, gop_mask, gop_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "bframes-candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_b_frames_candidate_set(reg, mask, shift);
		pr_info("mz0380: property 411 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
			b_frames_reg, b_frames_mask, b_frames_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "qpstep-candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_qp_step_candidate_set(reg, mask, shift);
		pr_info("mz0380: property 408 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
			qp_step_reg, qp_step_mask, qp_step_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "recordmode-candidate %x %x %u",
		   &reg, &mask, &shift) == 3) {
		mz0380_record_mode_candidate_set(reg, mask, shift);
		pr_info("mz0380: property 407 candidate set via procfs reg=0x%04x mask=0x%08x shift=%u\n",
			record_mode_reg, record_mode_mask,
			record_mode_shift);
		*ppos += count;
		ret = count;
		goto out;
	}

	if (sscanf(cmd, "input %u", &input) != 1)
		input = UINT_MAX;
	if (sscanf(cmd, "quality %u", &quality) != 1)
		quality = UINT_MAX;
	if (sscanf(cmd, "gop %u", &gop) != 1)
		gop = UINT_MAX;
	if (sscanf(cmd, "bframes %u", &b_frames) != 1)
		b_frames = UINT_MAX;
	if (sscanf(cmd, "qpstep %u", &qp_step) != 1)
		qp_step = UINT_MAX;
	if (sscanf(cmd, "bitrate %u", &bitrate) != 1)
		bitrate = UINT_MAX;
	if (sscanf(cmd, "recordmode %u", &mode) != 1)
		mode = UINT_MAX;
	if (input == UINT_MAX && quality == UINT_MAX && gop == UINT_MAX &&
	    b_frames == UINT_MAX &&
	    qp_step == UINT_MAX &&
	    bitrate == UINT_MAX && mode == UINT_MAX)
		goto out;

	mutex_lock(&devlist);
	if (!list_empty(&mz0380_devlist))
		dev = list_first_entry(&mz0380_devlist, struct mz0380_dev,
				       devlist);
	if (!dev) {
		ret = -ENODEV;
		mutex_unlock(&devlist);
		goto out;
	}

	if (input != UINT_MAX) {
		ret = mz0380_request_input_select(dev, input, "procfs");
	} else if (quality != UINT_MAX) {
		ret = mz0380_request_quality(dev, quality, "procfs");
	} else if (gop != UINT_MAX) {
		ret = mz0380_request_gop(dev, gop, "procfs");
	} else if (b_frames != UINT_MAX) {
		ret = mz0380_request_b_frames(dev, b_frames, "procfs");
	} else if (qp_step != UINT_MAX) {
		ret = mz0380_request_qp_step(dev, qp_step, "procfs");
	} else if (bitrate != UINT_MAX) {
		ret = mz0380_request_bitrate(dev, bitrate, "procfs");
	} else {
		ret = mz0380_request_record_mode(dev, mode, "procfs");
	}
	mutex_unlock(&devlist);
	if (!ret) {
		*ppos += count;
		ret = count;
	}

out:
	kfree(cmd);
	return ret;
}

static int mz0380_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_show, NULL);
}

static int mz0380_proc_state_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_state_show, NULL);
}

static int mz0380_proc_snapshot_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_snapshot_show, NULL);
}

static int mz0380_proc_control_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_control_show, NULL);
}

static int mz0380_proc_experiment_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_experiment_show, NULL);
}

static int mz0380_proc_scan_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_scan_show, NULL);
}

static int mz0380_proc_periph_scan_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_periph_scan_show, NULL);
}

static int mz0380_proc_events_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_events_show, NULL);
}

static int mz0380_proc_hdmi_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_hdmi_show, NULL);
}

static int mz0380_proc_cmd_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_cmd_show, NULL);
}

static struct proc_ops mz0380_proc_fops = {
	.proc_open = mz0380_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_state_fops = {
	.proc_open = mz0380_proc_state_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_snapshot_fops = {
	.proc_open = mz0380_proc_snapshot_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_control_fops = {
	.proc_open = mz0380_proc_control_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_experiment_fops = {
	.proc_open = mz0380_proc_experiment_open,
	.proc_read = seq_read,
	.proc_write = mz0380_proc_experiment_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_scan_fops = {
	.proc_open = mz0380_proc_scan_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_periph_scan_fops = {
	.proc_open = mz0380_proc_periph_scan_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_events_fops = {
	.proc_open = mz0380_proc_events_open,
	.proc_read = seq_read,
	.proc_write = mz0380_proc_events_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_hdmi_fops = {
	.proc_open = mz0380_proc_hdmi_open,
	.proc_read = seq_read,
	.proc_write = mz0380_proc_hdmi_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_cmd_fops = {
	.proc_open = mz0380_proc_cmd_open,
	.proc_read = seq_read,
	.proc_write = mz0380_proc_cmd_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/*
 * M76: read 16 bytes from the card's /mnt/flash/PIC_ENC via LOAD_FILES.
 *
 * The card echoes the whole 44-byte command back after servicing it, so the
 * data lands in PARAM1..PARAM4. Costs one mailbox round-trip per chunk.
 */
static int mz0380_load_files_read(struct mz0380_dev *dev, u16 offset,
				  u8 out[MZ0380_LOAD_FILES_CHUNK])
{
	u32 reply[MZ0380_MB_COMMAND_WORDS] = { 0 };
	u32 params[5];
	unsigned int i;
	int ret;

	params[0] = (u32)offset << 16;	/* is_write = 0 in the low half */
	/*
	 * Sentinel, not zero-fill. If the card cannot open PIC_ENC it prints
	 * "[LOGO_LOAD] cannot open %s" and returns WITHOUT touching the
	 * payload, so the command still completes and whatever we sent comes
	 * straight back. A zero payload therefore cannot distinguish "the file
	 * is 256 zero bytes" from "the file does not exist". 0xA5 can.
	 */
	for (i = 1; i < ARRAY_SIZE(params); i++)
		params[i] = 0xa5a5a5a5;

	ret = mz0380_send_command_reply(dev, MZ0380_CMD_LOAD_FILES,
					params, ARRAY_SIZE(params), NULL, 2000,
					reply, ARRAY_SIZE(reply));
	if (ret)
		return ret;

	/*
	 * reply[0] is the PARAM0 slot, which is the OPCODE word (reg.h:
	 * MZ0380_MB_OPCODE == MZ0380_MB_PARAM(0) == 0x04). Our params[0]
	 * therefore comes back in reply[1], and the 16 data bytes - card
	 * struct[8..23] - in reply[2..5].
	 */
	for (i = 0; i < MZ0380_LOAD_FILES_CHUNK; i++)
		out[i] = reply[2 + (i / 4)] >> ((i % 4) * 8);

	return 0;
}

/*
 * M76: write 16 bytes to the card's /mnt/flash/PIC_ENC via LOAD_FILES.
 *
 * vcm opens "wb+" when the offset is 0 (create/truncate) and "ab" otherwise,
 * so offset 0 is the only one that reliably lands where we asked. Used solely
 * by the cardlog_probe self-test: an untouched read sentinel cannot tell
 * "the file is missing" from "vcm never serviced the command", and a
 * write-then-read-back can.
 *
 * This is the only thing in-tree that writes to the card's flash. PIC_ENC is
 * the encoder's logo scratch file, not boot-critical, and the probe truncates
 * it to the 16 bytes it writes.
 */
static int mz0380_load_files_write(struct mz0380_dev *dev, u16 offset,
				   const u8 in[MZ0380_LOAD_FILES_CHUNK])
{
	u32 params[5] = { 0 };
	unsigned int i;

	params[0] = 1u | ((u32)offset << 16);	/* is_write = 1 */
	for (i = 0; i < MZ0380_LOAD_FILES_CHUNK; i++)
		params[1 + (i / 4)] |= (u32)in[i] << ((i % 4) * 8);

	return mz0380_send_command(dev, MZ0380_CMD_LOAD_FILES, params,
				   ARRAY_SIZE(params), NULL, 2000);
}

static void mz0380_cardlog_probe(struct seq_file *m, struct mz0380_dev *dev)
{
	static const u8 pattern[MZ0380_LOAD_FILES_CHUNK] = {
		'M', 'Z', '0', '3', '8', '0', '-', 'M',
		'7', '7', '-', 'P', 'R', 'O', 'B', 'E',
	};
	u8 back[MZ0380_LOAD_FILES_CHUNK];
	unsigned int i;
	int ret;

	seq_puts(m, "probe: writing a 16-byte pattern to PIC_ENC offset 0, then reading it back\n");

	ret = mz0380_load_files_write(dev, 0, pattern);
	if (ret) {
		seq_printf(m, "probe: write failed: %d\n", ret);
		return;
	}

	ret = mz0380_load_files_read(dev, 0, back);
	if (ret) {
		seq_printf(m, "probe: read-back failed: %d\n", ret);
		return;
	}

	seq_puts(m, "probe: read back |");
	for (i = 0; i < MZ0380_LOAD_FILES_CHUNK; i++)
		seq_putc(m, (back[i] >= 0x20 && back[i] < 0x7f) ? back[i] : '.');
	seq_puts(m, "|\n");

	if (!memcmp(back, pattern, sizeof(pattern)))
		seq_puts(m, "probe: PASS - vcm services op 0x6e and PIC_ENC is readable and writable\n");
	else if (back[0] == 0xa5 && !memchr_inv(back, 0xa5, sizeof(back)))
		seq_puts(m, "probe: FAIL - sentinel untouched, vcm never serviced the command\n");
	else
		seq_puts(m, "probe: MISMATCH - the command was serviced but the bytes differ\n");
}

static int mz0380_proc_cardlog_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		unsigned int off;

		seq_printf(m, "%s: /mnt/flash/PIC_ENC via LOAD_FILES(0x6e), %u bytes\n",
			   dev->name, mz0380_cardlog_bytes);

		if (mz0380_cardlog_probe_enabled)
			mz0380_cardlog_probe(m, dev);

		for (off = 0; off < mz0380_cardlog_bytes;
		     off += MZ0380_LOAD_FILES_CHUNK) {
			u8 buf[MZ0380_LOAD_FILES_CHUNK];
			unsigned int i;
			int ret;

			if (off > U16_MAX)
				break;

			ret = mz0380_load_files_read(dev, off, buf);
			if (ret) {
				seq_printf(m, "%04x: <read failed: %d>\n",
					   off, ret);
				break;
			}

			if (buf[0] == 0xa5 && !memchr_inv(buf, 0xa5,
							  sizeof(buf))) {
				seq_printf(m, "%04x: <untouched sentinel - the card did not read the file>\n",
					   off);
				break;
			}

			seq_printf(m, "%04x:", off);
			for (i = 0; i < MZ0380_LOAD_FILES_CHUNK; i++)
				seq_printf(m, " %02x", buf[i]);
			seq_puts(m, "  |");
			for (i = 0; i < MZ0380_LOAD_FILES_CHUNK; i++)
				seq_putc(m, (buf[i] >= 0x20 && buf[i] < 0x7f) ?
					 buf[i] : '.');
			seq_puts(m, "|\n");
		}
	}
	mutex_unlock(&devlist);
	return 0;
}

static int mz0380_proc_cardlog_open(struct inode *inode, struct file *file)
{
	return single_open(file, mz0380_proc_cardlog_show, NULL);
}

static struct proc_ops mz0380_proc_cardlog_fops = {
	.proc_open = mz0380_proc_cardlog_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/*
 * M76: raw dump of stream buffer 0.
 *
 * The poison scan proved the card writes ~3.1 MB into buf0 on the REAL
 * (is_nosg=0) path while the receiver holds lock - head bytes 0x11, i.e. the
 * card's own NO-SIGNAL splash, not H.264. Identifying that content offline is
 * the difference between "the encoder produced nothing" and "the encoder
 * produced the wrong picture", so expose the buffer verbatim.
 */
static ssize_t mz0380_proc_buf0_read(struct file *file, char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct mz0380_dev *dev;
	ssize_t ret = 0;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		if (!dev->stream_bufs[0].va)
			continue;
		ret = simple_read_from_buffer(buf, count, ppos,
					      dev->stream_bufs[0].va,
					      MZ0380_STREAM_BUF_SIZE);
		break;
	}
	mutex_unlock(&devlist);
	return ret;
}

static struct proc_ops mz0380_proc_buf0_fops = {
	.proc_read = mz0380_proc_buf0_read,
	.proc_lseek = default_llseek,
};

void mz0380_proc_remove(void)
{
	remove_proc_entry("mz0380-cardlog", NULL);
	remove_proc_entry("mz0380-buf0", NULL);
	remove_proc_entry("mz0380-cmd", NULL);
	remove_proc_entry("mz0380-hdmi", NULL);
	remove_proc_entry("mz0380-events", NULL);
	remove_proc_entry("mz0380-periph-scan", NULL);
	remove_proc_entry("mz0380-scan", NULL);
	remove_proc_entry("mz0380-experiment", NULL);
	remove_proc_entry("mz0380-control", NULL);
	remove_proc_entry("mz0380-snapshot", NULL);
	remove_proc_entry("mz0380-state", NULL);
	remove_proc_entry("mz0380", NULL);
}

/*
 * M245: who may read what.
 *
 * Anything whose READ drives the card - raw BAR windows (scan), mailbox I2C
 * (periph-scan: up to 256 commands, each up to a second under cmd_lock; hdmi:
 * the receiver diag) - is root-only. They were world-readable, so any user
 * could stall a capture with cat. state, snapshot, control and events stay
 * readable: the unprivileged counter watch reads state, and the one part of it
 * that talks to the card is gated on CAP_SYS_ADMIN in mz0380_fw_info_dump().
 */
int mz0380_proc_create(void)
{
	struct proc_dir_entry *pe;

	pe = proc_create("mz0380", 0444, NULL, &mz0380_proc_fops);
	if (!pe)
		return -ENOMEM;

	pe = proc_create("mz0380-state", 0444, NULL, &mz0380_proc_state_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-snapshot", 0444, NULL,
			 &mz0380_proc_snapshot_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-control", 0444, NULL,
			 &mz0380_proc_control_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-experiment", 0644, NULL,
			 &mz0380_proc_experiment_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-cardlog", 0400, NULL,
			 &mz0380_proc_cardlog_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-buf0", 0400, NULL, &mz0380_proc_buf0_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-scan", 0400, NULL, &mz0380_proc_scan_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-periph-scan", 0400, NULL,
			 &mz0380_proc_periph_scan_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-events", 0644, NULL, &mz0380_proc_events_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-hdmi", 0600, NULL, &mz0380_proc_hdmi_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	pe = proc_create("mz0380-cmd", 0600, NULL, &mz0380_proc_cmd_fops);
	if (!pe) {
		mz0380_proc_remove();
		return -ENOMEM;
	}

	return 0;
}
