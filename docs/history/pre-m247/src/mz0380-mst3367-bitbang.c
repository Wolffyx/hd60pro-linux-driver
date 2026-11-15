// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MST3367-side GPIO bit-banged I2C and EDID discovery diagnostics.
 */

#include "mz0380-mst3367-internal.h"

/* --- M51: bit-banged I2C on the card's spare GPIO pins -------------------- */

/*
 * The GPL hdcapm driver (same MST3367, sibling Elgato product) revealed the
 * EDID architecture these cards use: a plain I2C EEPROM wired to the HDMI
 * connector's DDC pins, sitting on a SECOND I2C bus ("bus#1, it has the
 * eeprom on it", dev 0xa2/0xa0) - NOT on the receiver's bus. The source
 * reads the EDID straight out of that chip; no software EDID serving at all.
 *
 * Our mailbox I2C proxy (op 0x1a/0x1b) only reaches the receiver bus (M43:
 * 0x9c + 0x98, nothing else), which would explain every negative EDID sweep:
 * wrong bus. But the card also has host-controllable GPIO (op 0x14/0x15/0x17
 * = read/set/direction), and NEXT_SESSION notes GPIO12/13 as the internal
 * I2C pair. So: bit-bang I2C on those pins from the host, one mailbox
 * command per line transition. Slow (~2 ms per command) but an ACK scan is
 * seconds and a full 256-byte EDID burn is minutes - and the EEPROM keeps
 * the data across power cycles, so the burn is one-time.
 *
 * Open-drain emulation: a line is released high by switching the pin to
 * INPUT (external pull-ups float it up) and driven low by data=0 + OUTPUT.
 * GPIO_DIR (op 0x17) is assumed {mask, data} with data bit 1 = output -
 * unverified on hardware; if a scan finds EVERY address ACKing or the bus
 * reads stuck low, the polarity or pin numbers are wrong, and the scan says
 * so instead of reporting garbage.
 */

/*
 * M51b. op 0x17's data polarity is NOT verified: ep.ko has both
 * gpio_direction_input and _output, but which data bit selects which is a
 * guess. Getting it backwards swaps release/drive, and the bus then looks
 * permanently stuck low - exactly the first hardware symptom. Runtime
 * switch so both readings can be tried without a rebuild.
 */
static int mz0380_bb_dir(struct mz0380_dev *dev, u8 pin, bool output)
{
	bool bit = mz0380_gpio_dir_invert ? !output : output;
	u32 params[2] = { 1u << pin, bit ? (1u << pin) : 0 };

	return mz0380_send_command(dev, MZ0380_CMD_GPIO_DIR, params, 2,
				   NULL, 500);
}

/*
 * M51b. Which pins even exist, and which float high? An I2C pair sits on
 * pull-ups, so it reads 1 when nobody drives it; a pin that is absent or
 * grounded reads 0 forever. One masked read of the whole port, then a
 * per-pin sweep (the firmware may only honour single-pin masks), gives the
 * candidate list before any bit-banging is attempted.
 */
int mz0380_gpio_dump(struct mz0380_dev *dev)
{
	u32 params[1] = { 0xffffffffu };
	u32 reply[4] = { 0 };
	u32 bitmap = 0;
	unsigned int pin;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_info("%s: gpiodump: firmware not READY (state %s)\n",
			dev->name, mz0380_fw_state_name(dev->fw_state));
		return -ENODEV;
	}

	ret = mz0380_send_command_reply(dev, MZ0380_CMD_GPIO_READ, params, 1,
					 NULL, 500, reply, ARRAY_SIZE(reply));
	pr_info("%s: gpiodump full-mask read ret=%d PARAM0=%08x PARAM1=%08x PARAM2=%08x PARAM3=%08x\n",
		dev->name, ret, reply[0], reply[1], reply[2], reply[3]);

	for (pin = 0; pin < 32; pin++) {
		u32 one[1] = { 1u << pin };
		u32 one_reply[3] = { 0 };

		if (mz0380_send_command_reply(dev, MZ0380_CMD_GPIO_READ,
					       one, 1, NULL, 500, one_reply,
					       ARRAY_SIZE(one_reply)))
			continue;
		if (one_reply[2] & (1u << pin))
			bitmap |= 1u << pin;
	}

	pr_info("%s: gpiodump per-pin idle bitmap = 0x%08x (HIGH pins are pull-up candidates for an I2C pair)\n",
		dev->name, bitmap);
	for (pin = 0; pin < 32; pin++)
		if (bitmap & (1u << pin))
			pr_info("%s: gpiodump   pin %2u reads HIGH\n",
				dev->name, pin);
	return 0;
}

/* release = input, pull-up floats the line high */
static int mz0380_bb_release(struct mz0380_dev *dev, u8 pin)
{
	return mz0380_bb_dir(dev, pin, false);
}

static int mz0380_bb_drive_low(struct mz0380_dev *dev, u8 pin)
{
	u32 params[2] = { 1u << pin, 0 };
	int ret = mz0380_send_command(dev, MZ0380_CMD_GPIO_SET, params, 2,
				      NULL, 500);

	if (ret)
		return ret;
	return mz0380_bb_dir(dev, pin, true);
}

static int mz0380_bb_read(struct mz0380_dev *dev, u8 pin, u8 *val)
{
	u32 params[1] = { 1u << pin };
	u32 reply[3] = { 0 };
	int ret = mz0380_send_command_reply(dev, MZ0380_CMD_GPIO_READ,
					     params, 1, NULL, 500,
					     reply, ARRAY_SIZE(reply));

	if (ret)
		return ret;
	*val = !!(reply[2] & (1u << pin));
	return 0;
}

struct mz0380_bb_bus {
	struct mz0380_dev *dev;
	u8 sda;
	u8 scl;
};

static int mz0380_bb_start(struct mz0380_bb_bus *b)
{
	int ret;

	/* both released -> SDA low -> SCL low */
	ret = mz0380_bb_release(b->dev, b->sda);
	ret = ret ?: mz0380_bb_release(b->dev, b->scl);
	ret = ret ?: mz0380_bb_drive_low(b->dev, b->sda);
	ret = ret ?: mz0380_bb_drive_low(b->dev, b->scl);
	return ret;
}

static int mz0380_bb_stop(struct mz0380_bb_bus *b)
{
	int ret;

	ret = mz0380_bb_drive_low(b->dev, b->sda);
	ret = ret ?: mz0380_bb_release(b->dev, b->scl);
	ret = ret ?: mz0380_bb_release(b->dev, b->sda);
	return ret;
}

static int mz0380_bb_write_bit(struct mz0380_bb_bus *b, bool bit)
{
	int ret;

	ret = bit ? mz0380_bb_release(b->dev, b->sda)
		  : mz0380_bb_drive_low(b->dev, b->sda);
	ret = ret ?: mz0380_bb_release(b->dev, b->scl);   /* clock high */
	ret = ret ?: mz0380_bb_drive_low(b->dev, b->scl); /* clock low  */
	return ret;
}

static int mz0380_bb_read_bit(struct mz0380_bb_bus *b, u8 *bit)
{
	int ret;

	ret = mz0380_bb_release(b->dev, b->sda);
	ret = ret ?: mz0380_bb_release(b->dev, b->scl);
	ret = ret ?: mz0380_bb_read(b->dev, b->sda, bit);
	ret = ret ?: mz0380_bb_drive_low(b->dev, b->scl);
	return ret;
}

/* returns 0 on ACK, 1 on NAK, negative on mailbox error */
static int mz0380_bb_write_byte(struct mz0380_bb_bus *b, u8 byte)
{
	u8 ack;
	int i, ret;

	for (i = 7; i >= 0; i--) {
		ret = mz0380_bb_write_bit(b, (byte >> i) & 1);
		if (ret)
			return ret;
	}
	ret = mz0380_bb_read_bit(b, &ack);
	if (ret)
		return ret;
	return ack;   /* SDA low during 9th clock = ACK(0) */
}

int mz0380_i2cbb_scan(struct mz0380_dev *dev, u8 sda, u8 scl)
{
	struct mz0380_bb_bus b = { .dev = dev, .sda = sda, .scl = scl };
	unsigned int addr, acks = 0;
	u8 v;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_info("%s: i2cbb scan: firmware not READY (state %s) - mailbox dead, cold boot needed?\n",
			dev->name, mz0380_fw_state_name(dev->fw_state));
		return -ENODEV;
	}

	/* idle-state sanity: both lines must float high or there is no bus */
	ret = mz0380_bb_release(dev, sda);
	ret = ret ?: mz0380_bb_release(dev, scl);
	ret = ret ?: mz0380_bb_read(dev, sda, &v);
	if (ret) {
		pr_info("%s: i2cbb scan sda=%u scl=%u: mailbox error %d\n",
			dev->name, sda, scl, ret);
		return ret;
	}
	if (!v) {
		pr_info("%s: i2cbb scan sda=%u scl=%u (dir_invert=%u): SDA reads LOW when released - wrong pin, wrong DIR polarity, or no pull-up; aborting. Run 'gpiodump' for the pins that float HIGH, and try gpio_dir_invert=1\n",
			dev->name, sda, scl, mz0380_gpio_dir_invert);
		return -EIO;
	}

	pr_info("%s: i2cbb scan start (sda=%u scl=%u)\n", dev->name, sda, scl);
	for (addr = 0x08; addr < 0x78; addr++) {
		ret = mz0380_bb_start(&b);
		ret = ret < 0 ? ret : mz0380_bb_write_byte(&b, addr << 1);
		if (ret >= 0)
			mz0380_bb_stop(&b);
		if (ret < 0) {
			pr_info("%s: i2cbb scan aborted at 0x%02x (%d)\n",
				dev->name, addr, ret);
			return ret;
		}
		if (ret == 0) {
			acks++;
			pr_info("%s: i2cbb ACK at 0x%02x (8-bit 0x%02x)\n",
				dev->name, addr, addr << 1);
		}
	}

	/*
	 * Hand the pins back as OUTPUTS. Leaving them as inputs is not
	 * cosmetic: pin9 is the receiver's reset and pin1 is HPD, so a probe
	 * that walks away mid-emulation leaves the receiver held in reset with
	 * a dead I2C bus, and no rmmod/insmod can undo it (only a cold boot).
	 */
	mz0380_bb_dir(dev, sda, true);
	mz0380_bb_dir(dev, scl, true);
	dev->mst3367_ready = false;   /* force a fresh bring-up after this */

	if (acks > 16)
		pr_info("%s: i2cbb scan: %u ACKs - that is a stuck bus, not real devices (check pins/polarity)\n",
			dev->name, acks);
	else
		pr_info("%s: i2cbb scan done: %u device(s)\n", dev->name, acks);
	return 0;
}

/*
 * M245: the EEPROM burn that lived here is gone, for the same reason the
 * firmware upload path was deleted: it wrote non-volatile storage on the card
 * - by GPIO bit-bang, to any 7-bit address, from a /proc write - and a wrong
 * pin pair or address could overwrite a part the card needs to boot. The
 * scan above only reads, and stays.
 */

/* --- M53: hunt an INDIRECT address/data port into the EDID RAM ----------- */

/*
 * M49b closed the DIRECT search: no bank holds an EDID-sized writable window
 * (longest contiguous run anywhere is 33 bytes). But that scan could never
 * have found the other way receivers expose EDID RAM - an INDIRECT port: one
 * register holds an address, a second reads/writes the byte at that address,
 * and the RAM behind them is invisible to a register sweep. Two ordinary
 * config registers, indistinguishable from the rest.
 *
 * They are separable by BEHAVIOUR, though, and cheaply. Storage indexed by an
 * address register remembers a different byte per address; a plain register
 * remembers only the last byte written to it:
 *
 *     A=0x10, D=0xa5 ; A=0x20, D=0x5a ; then A=0x10 -> D reads 0xa5?
 *                                            A=0x20 -> D reads 0x5a?
 *
 * A plain register returns 0x5a both times and is rejected. Both values
 * surviving means the pair (A,D) is a window onto address-indexed storage -
 * the EDID RAM, if it exists at all. Every register touched is restored.
 */
static bool mst_indirect_pair_is_ram(struct mz0380_dev *dev, u8 areg, u8 dreg)
{
	u8 v1 = 0, v2 = 0;

	if (mst_wr(dev, areg, 0x10) || mst_wr(dev, dreg, 0xa5) ||
	    mst_wr(dev, areg, 0x20) || mst_wr(dev, dreg, 0x5a) ||
	    mst_wr(dev, areg, 0x10) || mst_rd(dev, dreg, &v1) ||
	    mst_wr(dev, areg, 0x20) || mst_rd(dev, dreg, &v2))
		return false;

	return v1 == 0xa5 && v2 == 0x5a;
}

int mz0380_mst3367_edidhunt(struct mz0380_dev *dev)
{
	unsigned int bank, hits = 0, probe;
	bool answered = false;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_info("%s: edidhunt: firmware not READY (state %s)\n",
			dev->name, mz0380_fw_state_name(dev->fw_state));
		return -ENODEV;
	}

	/*
	 * The receiver powers up held in reset (pin9, active-low) and NAKs
	 * every access until released - and the firmware turns a NAK into a
	 * 0x00 result byte, so an un-brought-up bus looks exactly like a chip
	 * whose registers are all read-only. Bring it up first.
	 */
	if (!dev->mst3367_ready) {
		ret = mz0380_mst3367_bringup(dev);
		if (ret)
			return ret;
	}

	mutex_lock(&mst3367_lock);
	/*
	 * Then prove the bus is alive before believing any negative: read a
	 * spread of bank0 registers and require at least one non-zero. All
	 * zeros means nobody is answering, which is a broken probe, not the
	 * absence of an EDID window.
	 */
	mst_bank(dev, MST3367_BANK0);
	for (probe = 0x01; probe <= 0xf1; probe += 0x10) {
		u8 v = 0;

		if (!mst_rd(dev, probe, &v) && v)
			answered = true;
	}
	if (!answered) {
		pr_info("%s: edidhunt ABORTED: every register reads 0x00 - the receiver is not answering I2C (NAK), so this run would prove nothing. Check the bring-up (reset pin9) first\n",
			dev->name);
		ret = -ENXIO;
		goto out_unlock;
	}

	pr_info("%s: edidhunt: receiver answering; looking for an indirect address/data port (up to %u candidates per bank)\n",
		dev->name, mz0380_edidhunt_max_regs);

	for (bank = 0; bank <= 3; bank++) {
		u8 cand[256];
		u8 orig[256];
		unsigned int n = 0, i, j, tried = 0;
		unsigned int reg;

		if (mst_bank(dev, bank))
			continue;

		/* candidates = registers that hold an arbitrary byte */
		for (reg = 1; reg <= 0xff && n < mz0380_edidhunt_max_regs; reg++) {
			u8 was = 0, rb = 0;

			if (mst_rd(dev, reg, &was))
				continue;
			if (mst_wr(dev, reg, 0x5a) || mst_rd(dev, reg, &rb)) {
				mst_wr(dev, reg, was);
				continue;
			}
			mst_wr(dev, reg, was);
			if (rb == 0x5a) {
				orig[n] = was;
				cand[n++] = reg;
			}
			cond_resched();
		}

		pr_info("%s: edidhunt bank%u: %u writable candidates -> %u ordered pairs\n",
			dev->name, bank, n, n * (n ? n - 1 : 0));

		if (!n)
			pr_info("%s: edidhunt bank%u: no register accepted a write - M49 found writable runs here, so treat this as a dead bus, not a result\n",
				dev->name, bank);

		for (i = 0; i < n; i++) {
			for (j = 0; j < n; j++) {
				if (i == j)
					continue;
				tried++;
				if (mst_indirect_pair_is_ram(dev, cand[i],
							     cand[j])) {
					pr_info("%s: edidhunt HIT bank%u: addr=0x%02x data=0x%02x behaves as address-indexed RAM - EDID window candidate\n",
						dev->name, bank, cand[i],
						cand[j]);
					hits++;
				}
				/* put both registers back as we go */
				mst_wr(dev, cand[i], orig[i]);
				mst_wr(dev, cand[j], orig[j]);
				cond_resched();
				if (!(tried % 500))
					pr_info("%s: edidhunt bank%u: %u pairs tested\n",
						dev->name, bank, tried);
			}
		}
	}

	if (hits)
		pr_info("%s: edidhunt done: %u candidate port(s) - write the EDID through one and re-pulse HPD\n",
			dev->name, hits);
	else
		pr_info("%s: edidhunt done: NO indirect port found. The receiver holds no host-writable EDID storage, so the EDID must come from elsewhere on the DDC lines (card-side bus) - static RE is exhausted, a live Windows trace is the remaining route\n",
			dev->name);
	ret = 0;
out_unlock:
	mutex_unlock(&mst3367_lock);
	return ret;
}
