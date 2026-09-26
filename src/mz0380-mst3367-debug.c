// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MST3367 receiver diagnostics, watches, and output-state inspection.
 */

#include "mz0380-mst3367-internal.h"

/* --- sink-chain diagnostics ----------------------------------------------- */

/*
 * M43. "EDID loaded" only means the mailbox accepted the command: the card's
 * firmware forces the I2C result byte to 0 on a NAK, so a write that never
 * reached a chip is indistinguishable from a good one at the command layer.
 * Everything in the sink chain therefore has to be read BACK.
 *
 * The EDID read-back is the sharpest probe available: bytes 1..6 of a valid
 * EDID are 0xff, a value the firmware cannot manufacture on failure (it
 * forces 0x00). So "header reads ff" proves the DDC EEPROM really holds our
 * blob; "all zero" proves the writes went nowhere.
 */
int mz0380_gpio_get(struct mz0380_dev *dev, unsigned int pin, u32 *out)
{
	u32 params[1] = { 1u << pin };
	u32 reply[3] = { 0 };
	int ret = mz0380_send_command_reply(dev, MZ0380_CMD_GPIO_READ,
					     params, 1, NULL, 500,
					     reply, ARRAY_SIZE(reply));

	if (ret)
		return ret;
	/* op 0x14 returns the sampled bitmap in PARAM2 (BAR0+0x0c) */
	*out = !!(reply[2] & (1u << pin));
	return 0;
}

/*
 * M44. Is BANK3 writable RAM (i.e. the EDID store) or just an empty register
 * shadow? The bank sweep showed banks 0/1/2 full of live config while bank 3
 * reads all zeros except reg 0x00, which is the bank-select echo - exactly
 * what an EDID RAM that nobody has loaded would look like.
 *
 * Write a walking pattern into a few bank-3 registers, read it back, then put
 * the zeros back. Registers that return the pattern are RAM; registers that
 * read back 0 are unimplemented. Reg 0x00 is never touched - it is the bank
 * select on every bank.
 */
int mz0380_mst3367_ramtest(struct mz0380_dev *dev)
{
	static const u8 pattern[] = { 0xa5, 0x5a, 0x00, 0xff, 0x12, 0x34,
				      0x56, 0x78 };
	unsigned int i;
	unsigned int hits = 0;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;

	mutex_lock(&mst3367_lock);
	ret = mst_bank(dev, MST3367_BANK3);
	if (ret)
		goto out_unlock;

	for (i = 0; i < ARRAY_SIZE(pattern); i++)
		mst_wr(dev, 0x10 + i, pattern[i]);

	pr_info("%s: BANK3 RAM test, wrote a5 5a 00 ff 12 34 56 78 at 0x10:\n",
		dev->name);
	for (i = 0; i < ARRAY_SIZE(pattern); i++) {
		u8 v = 0;

		if (mst_rd(dev, 0x10 + i, &v)) {
			pr_info("%s:   reg 0x%02x: read failed\n",
				dev->name, 0x10 + i);
			continue;
		}
		pr_info("%s:   reg 0x%02x: wrote %02x read %02x%s\n",
			dev->name, 0x10 + i, pattern[i], v,
			v == pattern[i] ? "  <- RAM" : "");
		if (v == pattern[i] && pattern[i])
			hits++;
	}

	/* restore */
	for (i = 0; i < ARRAY_SIZE(pattern); i++)
		mst_wr(dev, 0x10 + i, 0x00);
	mst_bank(dev, MST3367_BANK0);

	pr_info("%s: BANK3 RAM test: %u/%u registers held their value - %s\n",
		dev->name, hits, (unsigned int)ARRAY_SIZE(pattern) - 1,
		hits ? "writable, this is a RAM window (EDID candidate)"
		     : "not writable, bank3 is not the EDID store");
	ret = hits ? 0 : -ENODEV;
out_unlock:
	mutex_unlock(&mst3367_lock);
	return ret;
}

/*
 * M45. The connected camera visibly reacts to running the bring-up (its live
 * preview stops), which is what a camera does when it detects an HDMI sink
 * and switches its output over. So the source IS responding to our HPD - the
 * question is whether it then transmits, and if it does, why the receiver
 * never reports lock.
 *
 * Watch the detect block live so a plug/unplug or camera power-cycle can be
 * correlated with what the chip sees. reg 0x55 is the status byte: bit2..5
 * (mask 0x3c) are the lock bits the GPL driver gates on.
 *
 * M173 settled what the other bits are, by watching R55 across a live
 * unplug/replug. This comment used to say bits 0..1 were "most plausibly
 * 5V/clock presence" and that guess stood unexamined for the life of the
 * project. It is WRONG: bits 0..1 read 3 with the cable in AND with it out,
 * while the lock bits went 0x3c -> 0x00 -> 0x3c. They are stuck high and carry
 * no status. This card has no +5V detect, which is why
 * V4L2_CID_DV_RX_POWER_PRESENT is not implemented.
 *
 * What does track the cable is mask 0x7c - so bit 6 moves with the signal
 * exactly as the 0x3c lock bits do, and neither we nor hdcapm gate on it.
 * Leave the mask alone: 0x3c has reported LOCKED correctly in every capture
 * this project has made.
 */
int mz0380_mst3367_watch(struct mz0380_dev *dev, unsigned int secs)
{
	unsigned long end;
	struct mst3367_measured last_m = { 0 };
	u8 last_detect = 0;
	int last_result = 0;
	bool last_matched = false;
	bool have_last_m = false;
	bool have_last_result = false;
	bool auto_position = false;
	bool have_auto_position = false;
	bool first = true;
	int ret = 0;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;
	if (!dev->mst3367_ready) {
		ret = mz0380_mst3367_bringup(dev);
		if (ret)
			return ret;
	}

	pr_info("%s: watching MST3367 detect for %us - plug/unplug or power-cycle the source now\n",
		dev->name, secs);
	have_auto_position = false;
	end = jiffies + secs * HZ;

	while (time_before(jiffies, end)) {
		const struct v4l2_dv_timings *t = NULL;
		struct mst3367_measured m = { 0 };
		bool scaled = false;
		bool locked;
		bool changed;
		u8 detect;
		int sample_ret = 0;

		mutex_lock(&mst3367_lock);
		ret = mst_bank(dev, MST3367_BANK0);
		if (!ret)
			ret = mst_rd(dev, MST3367_B0_DETECT, &detect);
		if (ret) {
			mutex_unlock(&mst3367_lock);
			break;
		}

		locked = mst3367_status_locked(detect);
		if (locked) {
			sample_ret = mst3367_measure(dev, &m);
			if (!sample_ret)
				t = mst3367_match_mode(&m, &scaled);
			else if (sample_ret != -ENOLCK && sample_ret != -EAGAIN) {
				ret = sample_ret;
				mutex_unlock(&mst3367_lock);
				break;
			}
		}

		/*
		 * Any non-mode state goes back to acquisition - but only on a
		 * CHANGE. M63: this used to write AUTO_POSITION unconditionally
		 * every 250 ms, and that register restarts position/phase
		 * acquisition, so the watch was re-perturbing the very counters
		 * it then read. A measurement that "moved mid-pass" can be the
		 * driver's own doing.
		 */
		{
			bool want_acq = !locked || sample_ret || !t;

			if (!have_auto_position || auto_position != want_acq) {
				ret = mst3367_set_auto_position(dev, want_acq);
				if (!ret) {
					auto_position = want_acq;
					have_auto_position = true;
				}
			} else {
				ret = 0;
			}
		}
		mutex_unlock(&mst3367_lock);
		if (ret)
			break;

		if (!locked) {
			changed = first || !have_last_result || detect != last_detect ||
				  last_result != -ENOLCK;
			if (changed)
				pr_info("%s: detect 55=%02x %s (auto-position on; timing not sampled)\n",
					dev->name, detect,
					(detect & MST3367_B0_DETECT_LOCK_MASK) ?
					"settling" : "no-lock");
			last_result = -ENOLCK;
			last_detect = detect;
			last_matched = false;
			have_last_result = true;
			have_last_m = false;
		} else if (!t) {
			if (!sample_ret) {
				changed = first || !have_last_m || last_matched ||
					  !mst3367_measurements_agree(&last_m, &m);
				if (changed)
					pr_info("%s: detect 55=%02x LOCKED coherent htot=%u vtot=%u hact=%u hper=%u vper=%u lines=%u 5f=%02x %s => UNSUPPORTED (auto-position on)\n",
						dev->name, m.detect, m.htotal, m.vtotal,
						m.hactive, m.hperiod, m.vperiod,
						m.lines, m.r5f,
						m.interlaced ? "i" : "p");
				last_m = m;
				last_detect = m.detect;
				last_result = -ERANGE;
				last_matched = false;
				have_last_m = true;
				have_last_result = true;
			} else {
				changed = first || !have_last_result ||
					  last_result != -EAGAIN ||
					  detect != last_detect;
				if (changed)
					pr_info("%s: detect 55=%02x full lock but no coherent timing yet: %s (htot=%u vtot=%u hact=%u hper=%u vper=%u 5f=%02x) (auto-position on)\n",
						dev->name, detect,
						m.reject ? m.reject : "no reason recorded",
						m.htotal, m.vtotal, m.hactive,
						m.hperiod, m.vperiod, m.r5f);
				last_result = -EAGAIN;
				last_detect = detect;
				last_matched = false;
				have_last_result = true;
				have_last_m = false;
			}
		} else {
			changed = first || !have_last_m || !last_matched ||
				  !mst3367_measurements_agree(&last_m, &m);
			if (changed)
				pr_info("%s: detect 55=%02x LOCKED coherent htot=%u vtot=%u hact=%u hper=%u vper=%u lines=%u 5f=%02x %s => MATCHED%s (auto-position off)\n",
					dev->name, m.detect, m.htotal, m.vtotal,
					m.hactive, m.hperiod, m.vperiod, m.lines,
					m.r5f, m.interlaced ? "i" : "p",
					scaled ? " (host units)" : "");
			last_m = m;
			last_detect = m.detect;
			last_result = 0;
			last_matched = true;
			have_last_m = true;
			have_last_result = true;
		}
		first = false;
		msleep(250);
	}

	pr_info("%s: watch done%s\n", dev->name, ret ? " (I/O error)" : "");
	return ret;
}

/*
 * M73: dump the receiver's OUTPUT stage.
 *
 * Everything upstream is now proven: the receiver locks, measures a clean
 * 1080p60 and holds it for tens of seconds. Everything downstream is armed:
 * SET_VIC carries legal values, the encoder spawns, START fires. Yet the card
 * writes nothing at all - the buffer poison scan reads 0/1024 pages touched -
 * and tinyvenc5 opens the VIC itself ("Can't create video capture -> exit"),
 * so a VIC that never sees a BT1120 clock is the remaining explanation.
 *
 * We configure that output stage in commit_digital_output() and have never
 * once read it back. These are the registers that gate it:
 *   BANK0 0xab bit7 - freeze/hold, set around the 0xb0 update and cleared
 *   BANK0 0xb0      - output format/clock select (hdcapm writes 0x20 for
 *                     "YUV422 / 8-bit output"; we write 0x21 for non-720p30)
 *   BANK0 0xb1/0xb2 - output config / special-mode select
 *   BANK0 0xb3      - written 0xff by the Windows indirect-port path
 *   BANK2 0x07      - the timing latch (bit4 pulsed on first acquisition)
 * plus 0x55 so the lock state at the same instant is on the same line.
 */
void mz0380_mst3367_output_diag(struct mz0380_dev *dev, const char *tag)
{
	u8 r55 = 0, ab = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0, b7 = 0, r51 = 0;
	u8 ad = 0, ae = 0, b4 = 0, b5 = 0;
	u8 b2_01 = 0, b2_02 = 0, b2_07 = 0;
	u8 b1_01 = 0, b1_34 = 0, b2_0b = 0, b2_0c = 0, b2_0e = 0, b2_48 = 0;
	bool link_read = false;

	if (dev->fw_state != MZ0380_FW_STATE_READY || !dev->mst3367_ready)
		return;

	mutex_lock(&mst3367_lock);
	if (mst_bank(dev, MST3367_BANK0))
		goto out;
	mst_rd(dev, MST3367_B0_DETECT, &r55);
	mst_rd(dev, 0xab, &ab);
	/*
	 * M99: the three registers the Windows output-stage block writes and
	 * that we never read back. Write ops return result 0x00 whatever the
	 * bus does - the firmware forces it - so ret=0 on mst_wr() is NOT
	 * evidence a write landed. Only a read-back is. M99 scored MSTOUT=1 as
	 * neutral without them; that verdict is only sound if these three read
	 * ad=00 ae|=04 b4=54 on a MSTOUT=1 run.
	 */
	mst_rd(dev, 0xad, &ad);
	mst_rd(dev, 0xae, &ae);
	mst_rd(dev, 0xb4, &b4);
	/* M126: 0xb5 is resolution-dependent in gchd and a fixed constant here. */
	mst_rd(dev, 0xb5, &b5);
	mst_rd(dev, 0xb0, &b0);
	mst_rd(dev, 0xb1, &b1);
	mst_rd(dev, 0xb2, &b2);
	mst_rd(dev, 0xb3, &b3);
	mst_rd(dev, 0xb7, &b7);
	mst_rd(dev, 0x51, &r51);
	if (!mst_bank(dev, MST3367_BANK2)) {
		mst_rd(dev, 0x01, &b2_01);
		mst_rd(dev, 0x02, &b2_02);
		mst_rd(dev, 0x07, &b2_07);
		/* M78: HDMI packet reception (hdcapm MST3367_HdmiGetPacketStatus). */
		mst_rd(dev, 0x0b, &b2_0b);
		mst_rd(dev, 0x0c, &b2_0c);
		mst_rd(dev, 0x0e, &b2_0e);
		mst_rd(dev, 0x48, &b2_48);
		mst_bank(dev, MST3367_BANK0);
		/*
		 * M133: cache it for mz0380_mst3367_apply_csc_mode(). This read
		 * path is the one proven to work on hardware; the standalone one
		 * returned 0x00 while this returned 0xd2 156 ms later.
		 */
		dev->mst_b2_48 = b2_48;
		dev->mst_b2_48_valid = true;
	}
	/*
	 * M78: the LINK layer, which we have never read. R55 lock only says the
	 * timing front end recovered a clock; whether the receiver forwards
	 * pixels to BT1120 depends on whether the source came up in HDMI mode
	 * and on HDCP. hdcapm's RxTmdsGetType: BANK1 0x01 bit2 = HDMI (else
	 * DVI), bit0 = HDCP present, BANK1 0x34 bit7 = HDCP active. These are
	 * the same two registers the Windows driver reads right after its
	 * output-stage commit.
	 */
	if (!mst_bank(dev, MST3367_BANK1)) {
		mst_rd(dev, 0x01, &b1_01);
		mst_rd(dev, 0x34, &b1_34);
		link_read = true;
		mst_bank(dev, MST3367_BANK0);
	}

	pr_info("%s: output stage [%s]: R55=%02x %s | B0: ab=%02x ad=%02x ae=%02x b0=%02x b1=%02x b2=%02x b3=%02x b4=%02x b5=%02x b7=%02x 51=%02x | B2: 01=%02x 02=%02x 07=%02x\n",
		dev->name, tag, r55,
		mst3367_status_locked(r55) ? "LOCKED" : "no-lock",
		ab, ad, ae, b0, b1, b2, b3, b4, b5, b7, r51,
		b2_01, b2_02, b2_07);
	if (link_read)
		pr_info("%s: link [%s]: B1 01=%02x 34=%02x -> %s, HDCP %s%s | B2 0b=%02x 0c=%02x 0e=%02x 48=%02x, input colorspace %s\n",
			dev->name, tag, b1_01, b1_34,
			(b1_01 & 0x04) ? "HDMI" : "DVI (source fell back - EDID?)",
			(b1_01 & 0x01) ? "present" : "absent",
			(b1_34 & 0x80) ? ", ACTIVE (encrypted)" : "",
			b2_0b, b2_0c, b2_0e, b2_48,
			/* hdcapm MST3367_HdmiGetPacketColor: B2 0x48 bits 6:5 */
			(b2_48 & 0x60) == 0x00 ? "RGB" :
			(b2_48 & 0x60) == 0x20 ? "YUV422" :
			(b2_48 & 0x60) == 0x40 ? "YUV444" : "undefined");
	if (ab & 0x80)
		pr_warn("%s: output stage [%s]: 0xab bit7 is STILL SET - the output is frozen\n",
			dev->name, tag);
out:
	mutex_unlock(&mst3367_lock);
}

/*
 * M130: choose the CSC mode from the colour space the source is ACTUALLY
 * sending, and apply it.
 *
 * hdcapm reads the input colour space (BANK2 0x48 bits 6:5) into
 * regb2r48_cached and then never uses it: its CSC table goes out
 * unconditionally. That is harmless on its board, whose EDID makes sources
 * send RGB, so its fixed RGB->YCbCr matrix is always right. We push no EDID
 * (M127) and this source picks YUV444, so the same matrix converts YCbCr as
 * though it were RGB - measured on hardware as chroma 96% ANTI-correlated
 * with luma (M129/M130), because HDMI puts Cb on blue, Y on green and Cr on
 * red and an RGB matrix therefore computes Cb_out = -0.291*Y + ...
 *
 * With a YCbCr input and a YCbCr 4:2:2 output over BT1120 there is nothing to
 * convert, and clearing 0x92 bypasses the conversion. Verified on hardware:
 * corr(chroma, luma) went -0.966/-0.749 to -0.319/+0.349 and the picture came
 * out in correct, natural colour.
 *
 * This cannot live in init_regs(): that runs at bring-up, before HPD is even
 * asserted, so nothing is transmitting and 0x48 is meaningless. It has to be
 * re-applied once the receiver has locked, which is why the stream-start path
 * calls it.
 */
/*
 * M237: re-read BANK2 0x48 and update the cache.
 *
 * The cache is written only by mz0380_mst3367_output_diag, which runs at
 * stream start and stream stop. Nothing invalidates it in between, so after a
 * source mode change - the camera switching between 50 and 60 Hz, say - the
 * colour space reported in /proc/mz0380-state is whatever was true at the last
 * stream start.
 *
 * That made the obvious experiment useless: an unchanged value cannot
 * distinguish "the source sends the same colour space in both modes" from "the
 * cache was never re-read". This gives a fresh read on demand, which costs a
 * few mailbox commands and so is deliberately NOT on any polled path.
 */
int mz0380_mst3367_refresh_colourspace(struct mz0380_dev *dev)
{
	u8 b2_48;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY || !dev->mst3367_ready)
		return -ENODEV;

	mutex_lock(&mst3367_lock);
	ret = mst_bank(dev, MST3367_BANK2);
	if (!ret)
		ret = mst_rd(dev, 0x48, &b2_48);
	if (!ret) {
		dev->mst_b2_48 = b2_48;
		dev->mst_b2_48_valid = true;
	}
	mst_bank(dev, MST3367_BANK0);
	mutex_unlock(&mst3367_lock);

	if (ret) {
		pr_info("%s: MST3367 colourspace re-read failed (%d)\n",
			dev->name, ret);
		return ret;
	}
	pr_info("%s: MST3367 colourspace re-read: 0x48=%02x\n",
		dev->name, b2_48);
	return 0;
}

void mz0380_mst3367_apply_csc_mode(struct mz0380_dev *dev)
{
	static const char * const names[] = {
		"RGB", "YUV422", "YUV444", "undefined"
	};
	u8 b2_48 = 0;
	unsigned int cs;
	bool cached;
	u8 want;

	if (dev->fw_state != MZ0380_FW_STATE_READY || !dev->mst3367_ready)
		return;

	/*
	 * M133: prefer the value the output diag last read. A standalone
	 * BANK2 0x48 read from here is NOT reliable - on the M133 run it
	 * returned 0x00 (which decodes as RGB, the wrong branch) while the
	 * diag read 0xd2 (YUV444) from the same register 156 ms later, and the
	 * receiver was locked throughout. Rather than add a second read path
	 * and hope, reuse the one that demonstrably works. This is also why
	 * hdcapm caches the register instead of re-reading it.
	 *
	 * The stream-start path calls the diag immediately before us, so the
	 * cache is fresh. Falling back to our own read keeps this correct for
	 * any caller that has not.
	 */
	cached = dev->mst_b2_48_valid;
	if (cached) {
		b2_48 = dev->mst_b2_48;
		mutex_lock(&mst3367_lock);
	} else {
		mutex_lock(&mst3367_lock);
		if (mst_bank(dev, MST3367_BANK2))
			goto out;
		if (mst_rd(dev, 0x48, &b2_48))
			goto out;
		if (mst_bank(dev, MST3367_BANK0))
			goto out;
	}

	cs = (b2_48 & 0x60) >> 5;
	if (mz0380_mst_csc_ctl == MZ0380_MST_CSC_CTL_AUTO) {
		/*
		 * Convert only for an RGB source. "undefined" keeps hdcapm's
		 * value: it is what every board that works today ships with,
		 * so it is the safer thing to fall back to when 0x48 has not
		 * settled.
		 */
		want = (cs == 1 || cs == 2) ? 0x00 : MZ0380_MST_CSC_CTL_HDCAPM;
	} else {
		want = mz0380_mst_csc_ctl & 0xff;
	}

	if (mst_wr(dev, 0x92, want))
		goto out;
	dev->mst_csc_applied = want;
	dev->mst_csc_applied_valid = true;
	pr_info("%s: MST3367 CSC 0x92 = 0x%02x (%s, input colorspace %s from 0x48=%02x, %s)\n",
		dev->name, want,
		mz0380_mst_csc_ctl == MZ0380_MST_CSC_CTL_AUTO ? "auto" : "forced",
		names[cs], b2_48, cached ? "cached" : "read here");
out:
	mutex_unlock(&mst3367_lock);
}

void mz0380_mst3367_diag(struct mz0380_dev *dev, struct seq_file *m)
{
	static const struct { const char *name; unsigned int pin; } pins[] = {
		{ "hpd      ", MZ0380_GPIO_HPD },
		{ "rx_enable", MZ0380_GPIO_RX_ENABLE },
		{ "rx_strap ", MZ0380_GPIO_RX_STRAP },
		{ "rx_reset ", MZ0380_GPIO_RX_RESET },
	};
	unsigned int i;
	u8 v;

	seq_printf(m, "%s: fw %s, receiver %s\n", dev->name,
		   mz0380_fw_state_name(dev->fw_state),
		   dev->mst3367_ready ? "brought up" : "NOT brought up");

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		seq_puts(m, "  (firmware not ready - nothing else can be probed)\n");
		return;
	}

	mutex_lock(&mst3367_lock);
	seq_puts(m, "  GPIO read-back (op 0x14):\n");
	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		u32 level = 0;
		int ret = mz0380_gpio_get(dev, pins[i].pin, &level);

		if (ret)
			seq_printf(m, "    %s pin%-2u : read failed (%d)\n",
				   pins[i].name, pins[i].pin, ret);
		else
			seq_printf(m, "    %s pin%-2u : %u\n",
				   pins[i].name, pins[i].pin, level);
	}

	seq_puts(m, "  EDID read-back from DDC 0xA0 (bytes 1..6 must be ff):\n    ");
	for (i = 0; i < 8; i++) {
		u32 b = 0;

		if (mz0380_periph_read(dev, MZ0380_EDID_I2C_DEV, i, &b))
			seq_puts(m, "?? ");
		else
			seq_printf(m, "%02x ", b & 0xff);
	}
	seq_puts(m, "\n");

	/*
	 * M43: the receiver answers at 0x9c but 0xA0 read back all zeros, so
	 * either nothing lives at the EDID address on this bus or the EEPROM
	 * is gated behind the receiver's DDC. Probe the addresses the Windows
	 * driver knows about: a non-zero byte proves a device is there,
	 * because the firmware forces 0x00 when nobody ACKs.
	 */
	/*
	 * Reading a single register is not enough to call an address dead:
	 * reg 0x00 on the main bank is the bank-select and legitimately reads
	 * back 0. Sample a few registers per address and treat "any non-zero"
	 * as present (the firmware forces 0x00 when nobody ACKs).
	 */
	seq_puts(m, "  I2C presence scan (regs 00/01/02/7f; any non-zero = present):\n");
	{
		/*
		 * 0xaa: the board also carries a small Nuvoton "EDID MCU" -
		 * the doc calls it "the EEPROM the HDMI source reads" - and
		 * our own RE noted an MCU passthrough slave at 0xaa. If the
		 * EDID lives there, that is what has to be programmed.
		 */
		static const u8 addrs[] = { 0x9c, 0xa0, 0xa2, 0xa4, 0xa6,
					    0xa8, 0xaa, 0x88, 0x98, 0x60,
					    0x90, 0x94, 0x74, 0xb0 };
		static const u8 probe[] = { 0x00, 0x01, 0x02, 0x7f };

		for (i = 0; i < ARRAY_SIZE(addrs); i++) {
			unsigned int j;
			bool any = false;

			seq_printf(m, "    %02x:", addrs[i]);
			for (j = 0; j < ARRAY_SIZE(probe); j++) {
				u32 b = 0;

				if (mz0380_periph_read(dev, addrs[i], probe[j],
						       &b)) {
					seq_puts(m, " ??");
					continue;
				}
				seq_printf(m, " %02x", b & 0xff);
				any |= !!(b & 0xff);
			}
			seq_puts(m, any ? "   <- present\n" : "\n");
		}
	}

	/*
	 * Hunt for the EDID. It is not at the DDC EEPROM address on this bus,
	 * so it is most likely served out of the receiver's internal EDID RAM.
	 * Dump the first 8 bytes from every address that answered: whichever
	 * window shows the EDID signature 00 ff ff ff ff ff ff 00 is the one
	 * to write the blob into.
	 */
	seq_puts(m, "  EDID signature hunt (looking for 00 ff ff ff ff ff ff 00):\n");
	{
		static const u8 addrs[] = { 0x98, 0xaa, 0xa8, 0xa2, 0x88,
					    0x90, 0x94, 0x60 };
		unsigned int j;

		for (i = 0; i < ARRAY_SIZE(addrs); i++) {
			seq_printf(m, "    %02x:", addrs[i]);
			for (j = 0; j < 8; j++) {
				u32 b = 0;

				if (mz0380_periph_read(dev, addrs[i], j, &b))
					seq_puts(m, " ??");
				else
					seq_printf(m, " %02x", b & 0xff);
			}
			seq_puts(m, "\n");
		}
	}

	/*
	 * The bus holds only 0x9c and 0x98 - no EEPROM, no MCU - so the EDID
	 * must live inside the receiver. The GPL reference says the chip has
	 * FOUR banks (0..3) with a 256-byte shadow each, and we have only ever
	 * used 0/1/2: bank 3 is unexplored and is the natural home for EDID
	 * RAM. Dump the head of every bank on both slaves and look for the
	 * signature.
	 */
	seq_puts(m, "  bank sweep, first 16 bytes of each bank (EDID RAM hunt):\n");
	{
		static const u8 slaves[] = { MST3367_I2C_DEV, 0x98 };
		unsigned int s, bank, j;

		for (s = 0; s < ARRAY_SIZE(slaves); s++) {
			for (bank = 0; bank <= 3; bank++) {
				if (mz0380_periph_write(dev, slaves[s],
							MST3367_REG_BANK_SELECT,
							bank))
					continue;
				seq_printf(m, "    %02x bank%u:", slaves[s],
					   bank);
				for (j = 0; j < 16; j++) {
					u32 b = 0;

					if (mz0380_periph_read(dev, slaves[s],
							       j, &b))
						seq_puts(m, " ??");
					else
						seq_printf(m, " %02x",
							   b & 0xff);
				}
				seq_puts(m, "\n");
			}
		}
		mst_bank(dev, MST3367_BANK0);   /* leave the chip on bank 0 */
	}

	seq_puts(m, "  MST3367 BANK0 regs (all-zero column = bus NAK, not real values):\n");
	if (mst_bank(dev, MST3367_BANK0)) {
		seq_puts(m, "    bank select failed - receiver I2C is dead\n");
		goto out_unlock;
	}
	{
		static const u8 regs[] = { 0x55, 0x5f, 0x6a, 0x6b, 0x59, 0x5a,
					   0xb0, 0xb7, 0xb8, 0x51 };

		seq_puts(m, "    ");
		for (i = 0; i < ARRAY_SIZE(regs); i++) {
			if (mst_rd(dev, regs[i], &v))
				seq_printf(m, "%02x=?? ", regs[i]);
			else
				seq_printf(m, "%02x=%02x ", regs[i], v);
		}
		seq_puts(m, "\n");
	}
	seq_puts(m, "  reg 0x55 & 0x3c == 0x3c means locked; 0x00 across the row means\n"
		    "  the receiver is not answering at all (check reset/power, not EDID).\n");
out_unlock:
	mutex_unlock(&mst3367_lock);
}
