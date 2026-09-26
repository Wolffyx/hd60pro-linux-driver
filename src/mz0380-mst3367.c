// SPDX-License-Identifier: GPL-2.0
/*
 * mz0380-mst3367.c - MStar MST3367 HDMI receiver bring-up + signal detect.
 *
 * The card firmware is a dumb I2C/GPIO proxy: the host drives the MST3367
 * entirely over the mailbox (op 0x1a/0x1b register R/W to dev 0x9c, op 0x15
 * GPIO set). RE_FINDINGS.md M11-M15 documents the whole path; the register
 * values here come from the GPL hdcapm driver (docs/re-2026-07-05/), which
 * RE'd the same Windows driver for a sibling board.
 *
 * The key finding (M14/M15): the MST3367 powers up held in reset by GPIO pin9
 * (active-low), so every I2C access NAKs until the host releases it. This file
 * releases pin9, runs the init sequence, then reads the mode-detect registers
 * and maps the measured geometry to a v4l2_dv_timings.
 */

#include "mz0380-mst3367-internal.h"

/*
 * The MST3367 bank selector is global chip state, while each mailbox command
 * is only serialized individually.  Keep bank-select + register batches
 * atomic with respect to every receiver operation in this file.  This is a
 * global lock because the device structure predates the receiver support;
 * HD60 Pro systems normally contain one such card.
 */
DEFINE_MUTEX(mst3367_lock);

bool mst3367_status_locked(u8 status)
{
	/*
	 * M67: gate on the core bits (0x1c), not the full 0x3c. Bit 0x20 is
	 * not required for the timing block to be valid - see the evidence
	 * table at MST3367_B0_DETECT_LOCK_CORE - and it toggles fast enough to
	 * be set when the poll loop reads R55 and clear ~5ms later when
	 * measure_once() re-reads it, which is exactly how three runs died
	 * with "no full lock at start of pass" while R55 read 0x5f.
	 *
	 * Sample quality is not this predicate's job anyway: the snapshot
	 * re-read, the vtotal delta and the line-count cross-check decide
	 * whether a measurement is trustworthy.
	 */
	return (status & MST3367_B0_DETECT_LOCK_CORE) ==
		MST3367_B0_DETECT_LOCK_CORE;
}

/* --- low-level MST3367 register access over the mailbox proxy ------------- */

int mst_wr(struct mz0380_dev *dev, u8 reg, u8 val)
{
	return mz0380_periph_write(dev, MST3367_I2C_DEV, reg, val);
}

int mst_rd(struct mz0380_dev *dev, u8 reg, u8 *val)
{
	u32 v = 0;
	int ret = mz0380_periph_read(dev, MST3367_I2C_DEV, reg, &v);

	if (ret)
		return ret;
	*val = v & 0xff;
	return 0;
}

/* Bank select is a normal write to reg 0x00; the driver caches nothing, so
 * callers must select the bank before each banked access batch. */
int mst_bank(struct mz0380_dev *dev, u8 bank)
{
	return mst_wr(dev, MST3367_REG_BANK_SELECT, bank);
}

/*
 * Is the receiver answering at all? The firmware turns an I2C NAK into a
 * 0x00 result byte, so "every register reads zero" is indistinguishable
 * from a chip that is powered down, held in reset, or absent - and quite
 * distinguishable from a live one, which always has some non-zero config.
 * Sampled across bank0 so one zeroed register cannot decide it.
 */
static int mst_bus_alive(struct mz0380_dev *dev)
{
	unsigned int reg;
	int ret;

	ret = mst_bank(dev, MST3367_BANK0);
	if (ret)
		return ret;
	for (reg = 0x01; reg <= 0xf1; reg += 0x10) {
		u8 v = 0;

		ret = mst_rd(dev, reg, &v);
		if (ret)
			return ret;
		if (v)
			return 1;
	}
	return 0;
}

/* --- GPIO (op 0x15, single-pin mask+data; see reg.h GPIO pin map) --------- */

static int mz0380_gpio_set(struct mz0380_dev *dev, unsigned int pin, int level)
{
	u32 params[2] = { 1u << pin, (u32)(!!level) << pin };

	return mz0380_send_command(dev, MZ0380_CMD_GPIO_SET, params, 2,
				   NULL, 500);
}

/*
 * Force a pin back to OUTPUT (op 0x17). The card powers up with these pins
 * already outputs, so bring-up historically only ever wrote DATA - but the
 * M51 bit-bang probe deliberately switched pins to input to emulate
 * open-drain, and left them that way. A GPIO_SET on an input pin changes
 * nothing, so the reset line (pin9) could no longer be driven and the
 * receiver stayed in reset with a dead I2C bus, surviving every rmmod.
 *
 * @invert selects the data-bit sense, which is still unproven; bring-up
 * tries both and keeps whichever revives the bus.
 */
static int mz0380_gpio_force_output(struct mz0380_dev *dev, unsigned int pin,
				    bool invert)
{
	u32 params[2] = { 1u << pin, invert ? 0 : (1u << pin) };

	return mz0380_send_command(dev, MZ0380_CMD_GPIO_DIR, params, 2,
				   NULL, 500);
}

/*
 * Release the MST3367 from reset.  The HD60 Pro Windows routine
 * FUN_14024dc28 drives pin9 1->0->1 with 50 ms at every phase, then drives
 * pin8 low and waits another 50 ms.  The old sibling-board sequence used
 * 2/5/10 ms delays and put pin8 back high; that is not the sequence shipped
 * for this PCI ID and can leave the receiver's acquisition state marginal.
 * Keep pin3 enabled around the device-specific reset because it is the board
 * RX/mux gate discovered during the original hardware bring-up.
 */
static int mz0380_mst3367_reset(struct mz0380_dev *dev)
{
	static const u8 driven[] = {
		MZ0380_GPIO_HPD, MZ0380_GPIO_RX_ENABLE,
		MZ0380_GPIO_RX_STRAP, MZ0380_GPIO_RX_RESET,
	};
	unsigned int i;
	int ret;

	/* every pin we are about to drive must actually be an output */
	for (i = 0; i < ARRAY_SIZE(driven); i++) {
		ret = mz0380_gpio_force_output(dev, driven[i],
						mz0380_gpio_dir_invert);
		if (ret)
			return ret;
	}

	ret = mz0380_gpio_set(dev, MZ0380_GPIO_RX_ENABLE, 1);
	if (ret)
		return ret;
	ret = mz0380_gpio_set(dev, MZ0380_GPIO_RX_RESET, 1);
	if (ret)
		return ret;
	msleep(50);
	ret = mz0380_gpio_set(dev, MZ0380_GPIO_RX_RESET, 0);
	if (ret)
		return ret;
	msleep(50);
	ret = mz0380_gpio_set(dev, MZ0380_GPIO_RX_RESET, 1);
	if (ret)
		return ret;
	msleep(50);
	/*
	 * M109: pin 8 is the companion reset/power strap and this line has
	 * driven it LOW since the original bring-up, never varied. Pin 9 next
	 * door is active-low and we release it high; if pin 8 shares that
	 * convention, LOW holds the companion in reset all session - which
	 * would explain the dead HDMI passthrough output.
	 */
	if (mz0380_rx_strap != MZ0380_RX_STRAP_LEAVE) {
		ret = mz0380_gpio_set(dev, MZ0380_GPIO_RX_STRAP,
				      mz0380_rx_strap ? 1 : 0);
		if (ret)
			return ret;
		msleep(50);
	}
	return 0;
}

/*
 * MST3367 init_setup (hdcapm mst3367_init_setup, cross-validated by our disasm).
 * Runs after the reset release; configures HDMI RX path, HDCP receive, YUV422
 * 8-bit output, then a HDMI + HDCP block reset.
 */
/* read-modify-write helpers, matching hdcapm's mst3367_set / mst3367_clr */
int mst_set(struct mz0380_dev *dev, u8 reg, u8 mask)
{
	u8 v = 0;
	int ret;

	ret = mst_rd(dev, reg, &v);
	if (ret)
		return ret;
	return mst_wr(dev, reg, v | mask);
}

int mst_clr(struct mz0380_dev *dev, u8 reg, u8 mask)
{
	u8 v = 0;
	int ret;

	ret = mst_rd(dev, reg, &v);
	if (ret)
		return ret;
	return mst_wr(dev, reg, v & ~mask);
}

static int mst3367_hdcp_reset(struct mz0380_dev *dev)
{
	int ret;

	ret = mst_bank(dev, MST3367_BANK0);
	if (ret)
		return ret;
	ret = mst_wr(dev, 0xb8, 0x10);
	if (ret)
		return ret;
	ret = mst_wr(dev, 0xb8, 0x00);
	if (ret)
		return ret;
	msleep(20);
	return 0;
}

static int mst3367_hdmi_reset(struct mz0380_dev *dev)
{
	int ret;

	ret = mst_bank(dev, MST3367_BANK2);
	if (ret)
		return ret;
	ret = mst_wr(dev, 0x07, 0xf4);
	if (ret)
		return ret;
	ret = mst_wr(dev, 0x07, 0x04);
	if (ret)
		return ret;
	msleep(20);
	return 0;
}

/*
 * MST3367 init - a faithful port of hdcapm's mst3367_init_setup() plus the
 * RxHdmiInit block it ends with (GPL, same receiver, RE'd from the Windows
 * driver for a sibling board).
 *
 * M56: our previous version was a small subset of this and, on hardware,
 * the receiver would see a source's clock (detect bit 0x80, horizontal
 * period measured) but never reach frame lock (0x55 & 0x3c stayed clear,
 * the vertical-period counter sat saturated at 0x1fff). The blocks missing
 * here are exactly the ones that would explain that: RxTmdsInit programs
 * the TMDS equaliser/PLL and RxVideoInit the filter and sync handling.
 * Ordering is hdcapm's: HPD stays off across the whole sequence and only
 * rises at the end, after the HDCP and HDMI blocks are reset. Auto-position
 * remains enabled for acquisition and is disabled only after mode recognition.
 */
static int mz0380_mst3367_init_regs(struct mz0380_dev *dev)
{
	/* CSC coefficients, written as a block at BANK0 0x92.. (hdcapm) */
	static const u8 csctbl[] = {
		0x40,
		0x08, 0x02, 0x03, 0x65, 0x7E, 0x28, /* M11, M12, M13 */
		0x78, 0xB9, 0x0B, 0x65, 0x79, 0xD6, /* M21, M22, M23 */
		0x7F, 0x45, 0x01, 0x27, 0x08, 0x02, /* M31, M32, M33 */
		0x20, 0x00, 0x02, 0x81, 0x20, 0x01, /*  A1,  A2,  A3 */
		0x15, 0x95, 0x05, 0x20, 0xC0, 0x08
	};
	unsigned int i;
	int ret;

	/* HPD off for the whole configuration */
	MST3367_TRY(mst_bank(dev, MST3367_BANK0));
	MST3367_TRY(mst_wr(dev, MST3367_B0_HPD, MST3367_B0_HPD_OFF));

	/*
	 * HD60 Pro FUN_14024dc28 preamble.  These fixed writes precede the
	 * receiver's TMDS setup in the shipping driver; the older sibling-board
	 * sequence omitted them even though 0x64..0x67 sit in the clock/sync
	 * acquisition block.
	 */
	MST3367_TRY(mst_wr(dev, 0x13, 0x08));
	/*
	 * FUN_14024eeb8(0): park HPD DEASSERTED during configuration. M69: the
	 * pin is active-low (pin1 = ~(arg>>4)&1 in the Windows driver), so
	 * deasserted = pin HIGH. We wrote 0 here for the entire project, which
	 * ASSERTED hot-plug while the receiver was half-configured and while
	 * the EDID write was in flight - inviting the source to master the
	 * DDC bus under our EEPROM writes.
	 */
	MST3367_TRY(mz0380_gpio_set(dev, MZ0380_GPIO_HPD, 1));

	/* RxGeneralInit */
	MST3367_TRY(mst_wr(dev, 0x41, 0x6f));
	MST3367_TRY(mst_wr(dev, 0xb8, 0x00));
	MST3367_TRY(mst_wr(dev, 0x64, 0x02));
	MST3367_TRY(mst_wr(dev, 0x65, 0xff));
	MST3367_TRY(mst_wr(dev, 0x66, 0x00));
	MST3367_TRY(mst_wr(dev, 0x67, 0x02));

	/* RxTmdsInit - equaliser / PLL; absent from our old sequence */
	MST3367_TRY(mst_bank(dev, MST3367_BANK1));
	MST3367_TRY(mst_wr(dev, 0x0f, 0x02));
	MST3367_TRY(mst_wr(dev, 0x16, 0x30));
	MST3367_TRY(mst_wr(dev, 0x17, 0x00));
	MST3367_TRY(mst_wr(dev, 0x18, 0x00));
	MST3367_TRY(mst_wr(dev, 0x19, 0x00));
	MST3367_TRY(mst_wr(dev, 0x1a, 0x50));
	MST3367_TRY(mst_clr(dev, 0x2a, 0x07));
	MST3367_TRY(mst_set(dev, 0x2a, 0x07));
	MST3367_TRY(mst_bank(dev, MST3367_BANK2));
	MST3367_TRY(mst_wr(dev, 0x08, 0x03));

	/* RxHdcpInit - receive HDCP */
	MST3367_TRY(mst_bank(dev, MST3367_BANK1));
	MST3367_TRY(mst_wr(dev, 0x24, 0x40));
	MST3367_TRY(mst_wr(dev, 0x30, 0x80));
	MST3367_TRY(mst_wr(dev, 0x31, 0x00));
	MST3367_TRY(mst_wr(dev, 0x32, 0x00));

	/* RxVideoInit */
	MST3367_TRY(mst_bank(dev, MST3367_BANK0));
	MST3367_TRY(mst_wr(dev, 0xb0, 0x14));
	/*
	 * M101: this set does NOT stick. Every output diag since the readback
	 * was added reads 0xae = 0x20, bit2 clear, on runs where this line has
	 * executed - while 0xad and 0xb4, written by the same helper two lines
	 * below, both read back exactly as written. So the bus is fine and
	 * something specific to 0xae bit2 is going on: either it is write-only
	 * or self-clearing (a strobe), or the card's own firmware rewrites the
	 * register. Read it straight back here to tell those apart - if it
	 * reads 0x24 now and 0x20 later, something clears it; if it reads 0x20
	 * now, the write is simply rejected.
	 *
	 * Consequence either way: M99 scored "0xae |= 0x04 is neutral" on a
	 * write that was never in the register. That register is UNTESTED, not
	 * neutral.
	 */
	MST3367_TRY(mst_set(dev, 0xae, 0x04));
	{
		u8 ae_rb = 0;

		if (!mst_rd(dev, 0xae, &ae_rb))
			pr_info("%s: RxVideoInit: 0xae |= 0x04 -> reads back %02x%s\n",
				dev->name, ae_rb,
				(ae_rb & 0x04) ? "" : " (BIT2 DID NOT STICK)");
	}
	MST3367_TRY(mst_wr(dev, 0xad, 0x05)); /* enable low-pass filter */
	/* Exact HD60 Pro FUN_14024dc28 values (the sibling uses e0/08). */
	MST3367_TRY(mst_wr(dev, 0xb1, mz0380_mst_b1 & 0xff));
	MST3367_TRY(mst_wr(dev, 0xb2, mz0380_mst_b2 & 0xff));
	MST3367_TRY(mst_wr(dev, 0xb3, 0x00));
	MST3367_TRY(mst_wr(dev, 0xb4, 0x55));

	/* RxAudioInit */
	MST3367_TRY(mst_clr(dev, 0xb4, 0x03));
	MST3367_TRY(mst_bank(dev, MST3367_BANK2));
	MST3367_TRY(mst_wr(dev, 0x01, 0x61));
	MST3367_TRY(mst_wr(dev, 0x02, 0xf5));
	MST3367_TRY(mst_set(dev, 0x03, 0x02));
	MST3367_TRY(mst_wr(dev, 0x04, 0x01));
	MST3367_TRY(mst_wr(dev, 0x05, 0x00));
	MST3367_TRY(mst_wr(dev, 0x06, 0x08));
	MST3367_TRY(mst_wr(dev, 0x1c, 0x1a));
	MST3367_TRY(mst_wr(dev, 0x1d, 0x00));
	MST3367_TRY(mst_wr(dev, 0x1e, 0x00));
	MST3367_TRY(mst_wr(dev, 0x1f, 0x00));
	MST3367_TRY(mst_clr(dev, 0x25, 0xa2));
	MST3367_TRY(mst_set(dev, 0x25, 0xa2));

	MST3367_TRY(mst_set(dev, 0x02, 0x80));
	MST3367_TRY(mst_set(dev, 0x07, 0x04));
	MST3367_TRY(mst_wr(dev, 0x17, 0xc0));
	MST3367_TRY(mst_wr(dev, 0x19, 0xff));
	MST3367_TRY(mst_wr(dev, 0x1a, 0xff));
	MST3367_TRY(mst_wr(dev, 0x1b, 0xfc));
	MST3367_TRY(mst_wr(dev, 0x20, 0x00));
	MST3367_TRY(mst_clr(dev, 0x21, 0x03));
	MST3367_TRY(mst_wr(dev, 0x22, 0x26));
	MST3367_TRY(mst_wr(dev, 0x27, 0x00));
	MST3367_TRY(mst_set(dev, 0x2e, 0xa1));

	/* colour range */
	MST3367_TRY(mst_bank(dev, MST3367_BANK0));
	MST3367_TRY(mst_wr(dev, 0xab, 0x15));
	MST3367_TRY(mst_clr(dev, 0xac, 0x3f));
	MST3367_TRY(mst_set(dev, 0xac, 0x15));

	/* RxSwitchSource - HDMI */
	MST3367_TRY(mst_wr(dev, MST3367_B0_HPD, MST3367_B0_HPD_OFF));
	MST3367_TRY(mst3367_hdcp_reset(dev));
	MST3367_TRY(mst3367_hdmi_reset(dev));
	MST3367_TRY(mst_bank(dev, MST3367_BANK0));
	MST3367_TRY(mst_wr(dev, 0x51, 0x89));
	MST3367_TRY(mst_wr(dev, MST3367_B0_HPD, MST3367_B0_HPD_ON));

	/* Keep auto-position enabled until a coherent mode has been recognized. */
	MST3367_TRY(mst_wr(dev, MST3367_B0_AUTO_POSITION,
			       MST3367_B0_AUTO_POSITION_ON));
	MST3367_TRY(mst_wr(dev, 0x1e, 0x11));
	MST3367_TRY(mst_wr(dev, 0x1f, 0x01));
	MST3367_TRY(mst_wr(dev, 0x73, 0x90));
	MST3367_TRY(mst_wr(dev, 0xb5, mz0380_mst_b5 & 0xff));

	/* CSC */
	MST3367_TRY(mst_wr(dev, 0x90, 0x15));
	MST3367_TRY(mst_wr(dev, 0x91, 0x15));
	for (i = 0; i < ARRAY_SIZE(csctbl); i++) {
		/*
		 * M130: csctbl[0] lands on 0x92, the CSC control byte - the
		 * only entry that is not a coefficient. Overridable so a
		 * YUV444 source can try bypassing the conversion entirely.
		 * See mz0380_mst_csc_ctl in mz0380-core.c.
		 */
		u8 val = csctbl[i];

		if (!i && mz0380_mst_csc_ctl != MZ0380_MST_CSC_CTL_AUTO)
			val = mz0380_mst_csc_ctl & 0xff;

		MST3367_TRY(mst_wr(dev, 0x92 + i, val));
	}
	/*
	 * AUTO leaves hdcapm's byte here on purpose: this runs before HPD, so
	 * nothing is transmitting and the input colour space is not yet
	 * knowable. mz0380_mst3367_apply_csc_mode() re-applies it at stream
	 * start, once the receiver has locked.
	 */
	if (mz0380_mst_csc_ctl != MZ0380_MST_CSC_CTL_AUTO &&
	    (mz0380_mst_csc_ctl & 0xff) != csctbl[0])
		pr_info("%s: MST3367 CSC control 0x92 forced to 0x%02x (hdcapm default 0x%02x)\n",
			dev->name, mz0380_mst_csc_ctl & 0xff, csctbl[0]);

	/* YUV422, 8-bit, external sync */
	MST3367_TRY(mst_wr(dev, 0xb0, 0x20));

	/* RxHdmiInit */
	MST3367_TRY(mst_bank(dev, MST3367_BANK2));
	MST3367_TRY(mst_clr(dev, 0x01, 0xf0));
	MST3367_TRY(mst_set(dev, 0x01, 0x40 | 0x20));
	MST3367_TRY(mst_set(dev, 0x04, 0x01));
	MST3367_TRY(mst_wr(dev, 0x06, 0x08));
	MST3367_TRY(mst_set(dev, 0x09, 0x20));
	MST3367_TRY(mst_bank(dev, MST3367_BANK0));
	MST3367_TRY(mst_clr(dev, 0x54, 0x10));
	MST3367_TRY(mst_set(dev, 0xac, 0x80));
	MST3367_TRY(mst_set(dev, 0x00, 0x80));
	MST3367_TRY(mst_set(dev, 0xce, 0x80));
	MST3367_TRY(mst_clr(dev, 0xcf, 0x07));
	MST3367_TRY(mst_set(dev, 0xcf, 0x02));
	MST3367_TRY(mst_clr(dev, 0x00, 0x80));

	/*
	 * Complete FUN_14024d2ec's HD60 Pro output-pin mapping.  Registry
	 * defaults are output-map index 1 and automatic mapping enabled, which
	 * select table value zero: BANK80:D0[1:0]=0 and CF[7]=0.
	 */
	MST3367_TRY(mst_bank(dev, 0x80));
	MST3367_TRY(mst_clr(dev, 0xd0, 0x03));
	MST3367_TRY(mst_clr(dev, 0xcf, 0x80));
	MST3367_TRY(mst_bank(dev, MST3367_BANK0));

	msleep(20);
	return 0;
}

/*
 * Load the EDID into the receiver's DDC EEPROM (I2C 0xA0) using the exact
 * Windows write_s packet: PARAM1=(len<<16)|(offset<<8)|dev8 followed by 32
 * data bytes in PARAM2..PARAM9.  This deliberately leaves PARAM10/STATUS free
 * so each chunk can complete synchronously before the mailbox is reused.
 */
/*
 * M69: read the EDID store back through opcode 0x1e - the block-read twin of
 * the 0x1f write. Unlike 0x1a, whose result the firmware forces to 0 on a
 * NAK, 0x1e leaves the host's payload bytes UNTOUCHED on failure - so poison
 * the buffer first and a NAK is unambiguous. Returns 0 with 32 bytes in *buf.
 */
static int mz0380_mst3367_read_edid_chunk(struct mz0380_dev *dev,
					  unsigned int off, u8 *buf)
{
	u32 params[1 + MZ0380_EDID_CHUNK / sizeof(u32)];
	unsigned int i;
	int ret;

	memset(params, 0xa5, sizeof(params));    /* poison: NAK leaves it */
	params[0] = MZ0380_EDID_I2C_DEV |
		    ((u32)off << 8) |
		    ((u32)MZ0380_EDID_CHUNK << 16);

	ret = mz0380_send_command(dev, MZ0380_CMD_I2C_READ_S, params,
				  ARRAY_SIZE(params), NULL, 500);
	if (ret)
		return ret;
	/* the reply payload lands back in the PARAM slots */
	for (i = 0; i < MZ0380_EDID_CHUNK / sizeof(u32); i++) {
		u32 v = mz_mmio_read(dev, MZ0380_MB_PARAM(1 + i));

		memcpy(buf + i * sizeof(u32), &v, sizeof(v));
	}
	return 0;
}

static int mz0380_mst3367_load_edid(struct mz0380_dev *dev)
{
	u8 verify[MZ0380_EDID_CHUNK];
	unsigned int off;
	int ret;

	/*
	 * M69: GPIO pin 2 is the DDC path mux, driven ONLY by the Windows
	 * EDID handler: 1 = the local EDID store is on the SoC's i2c-0
	 * (where our opcode 0x1f writes go), 0 = the passthrough side. Every
	 * previous EDID attempt ran with pin 2 in its power-on state - if
	 * that state is 0, our writes went to a bus with nothing on it,
	 * which is exactly the observed NAK-and-read-back-zero.
	 */
	ret = mz0380_gpio_force_output(dev, MZ0380_GPIO_EDID_MUX,
				       mz0380_gpio_dir_invert);
	if (!ret)
		ret = mz0380_gpio_set(dev, MZ0380_GPIO_EDID_MUX, 1);
	if (ret)
		pr_warn("%s: EDID mux (pin2) drive failed (%d) - writing with it as-is\n",
			dev->name, ret);

	for (off = 0; off < MZ0380_EDID_SIZE; off += MZ0380_EDID_CHUNK) {
		u32 params[1 + MZ0380_EDID_CHUNK / sizeof(u32)] = { 0 };

		params[0] = MZ0380_EDID_I2C_DEV |
			    ((u32)off << 8) |
			    ((u32)MZ0380_EDID_CHUNK << 16);
		memcpy(&params[1], &mz0380_edid_default[off],
		       MZ0380_EDID_CHUNK);

		if (!mz0380_edid_timeout_ms) {
			pr_warn("%s: refusing unsafe asynchronous EDID write at offset %u\n",
				dev->name, off);
			return -EINVAL;
		}
		ret = mz0380_send_command(dev, mz0380_edid_opcode, params,
					  ARRAY_SIZE(params), NULL,
					  mz0380_edid_timeout_ms);
		if (ret) {
			pr_warn("%s: EDID write failed at offset %u (%d)\n",
				dev->name, off, ret);
			/* leave the mux on the connector side on the way out */
			mz0380_gpio_set(dev, MZ0380_GPIO_EDID_MUX, 0);
			return ret;
		}
		msleep(20);   /* conservative EEPROM page-write cycle */
	}

	/*
	 * M69: verify with the clean NAK detector while the mux still points
	 * at the local store. Header bytes 1..6 of a valid EDID are 0xff - a
	 * value the poison (0xa5) cannot fake.
	 */
	memset(verify, 0, sizeof(verify));
	ret = mz0380_mst3367_read_edid_chunk(dev, 0, verify);
	if (!ret && verify[1] == 0xff && verify[2] == 0xff && verify[6] == 0xff)
		pr_info("%s: EDID READ-BACK OK (%02x %02x %02x %02x %02x %02x %02x %02x) - a real store holds our EDID\n",
			dev->name, verify[0], verify[1], verify[2], verify[3],
			verify[4], verify[5], verify[6], verify[7]);
	else
		pr_warn("%s: EDID read-back failed (ret=%d, first bytes %02x %02x %02x %02x) - nothing is holding the EDID at 0xa0\n",
			dev->name, ret, verify[0], verify[1], verify[2],
			verify[3]);

	/* hand the store back to the connector side, as Windows does */
	mz0380_gpio_set(dev, MZ0380_GPIO_EDID_MUX, 0);

	pr_info("%s: EDID pushed (%u bytes, opcode 0x%02x, wait %ums) - the source is the oracle: a camera switches to HDMI out once it can read one\n",
		dev->name, MZ0380_EDID_SIZE, mz0380_edid_opcode,
		mz0380_edid_timeout_ms);
	return 0;
}

/*
 * Assert hot-plug detect. Two levers, both needed: the board-level HPD pin
 * (GPIO pin1) that the source physically senses, and the receiver's own HPD
 * enable (BANK0 0xb7 bit1, cleared to 0 by the init sequence above). Order
 * matters - HPD must rise only AFTER the EDID is readable, or the source will
 * read a garbage/absent EDID once and not retry.
 */
static int mz0380_mst3367_hpd(struct mz0380_dev *dev, bool on)
{
	u32 level = 0;
	u8 b7 = 0;
	bool pin_ok, receiver_ok;
	int ret;

	ret = mst_bank(dev, MST3367_BANK0);
	if (ret)
		return ret;
	ret = mst_wr(dev, MST3367_B0_HPD,
		     on ? MST3367_B0_HPD_ON : MST3367_B0_HPD_OFF);
	if (ret)
		return ret;

	/*
	 * Drive the board pin - and make sure it IS driven. A GPIO_SET on a
	 * pin left as an input silently does nothing (the M51 probe did
	 * exactly that to pin1), so the driver would claim HPD while the
	 * source saw no edge at all. Force the direction, then read the pin
	 * and the receiver's own HPD bit back and report what actually stuck.
	 */
	ret = mz0380_gpio_force_output(dev, MZ0380_GPIO_HPD,
				       mz0380_gpio_dir_invert);
	if (ret)
		return ret;
	/*
	 * M69: HPD is ACTIVE-LOW on this board (win64 FUN_14024eeb8:
	 * pin1 = ~(arg>>4)&1 - HPD_ON drives the pin LOW). Every earlier run
	 * asserted when it meant to release and vice versa, which is exactly
	 * the recorded behaviour: the camera reacted to our edge, then fell
	 * back to its LCD because our "asserted" steady state was, physically,
	 * no sink present.
	 */
	ret = mz0380_gpio_set(dev, MZ0380_GPIO_HPD, !on);
	if (ret)
		return ret;

	ret = mz0380_gpio_get(dev, MZ0380_GPIO_HPD, &level);
	if (ret)
		return ret;
	ret = mst_rd(dev, MST3367_B0_HPD, &b7);
	if (ret)
		return ret;

	pin_ok = !!level == !on;    /* active-low: asserted reads 0 */
	receiver_ok = (!(b7 & MST3367_B0_HPD_OFF)) == on;
	pr_info("%s: HPD %s -> pin%u reads %u, BANK0[0x%02x]=0x%02x%s%s\n",
		dev->name, on ? "asserted" : "deasserted", MZ0380_GPIO_HPD,
		level, MST3367_B0_HPD, b7,
		pin_ok ? "" : "  <- board HPD pin did not follow",
		receiver_ok ? "" : "  <- receiver HPD bit did not follow");

	/*
	 * M61: this read-back is DIAGNOSTIC, not a gate. Every command above
	 * already returned its own error; what is left is a sampled pin, and
	 * on this SoC a GPIO read returns the input register, which need not
	 * follow a pin we are driving as an output. Asserting HPD failed the
	 * check while deasserting passed it - a one-sided result that fits a
	 * read-back artefact, not a dead line. What actually proved HPD works
	 * is the source reacting to the edge (M48/M49), and failing bring-up
	 * here throws away the detect path over a sample we cannot trust.
	 */
	if (!pin_ok || !receiver_ok)
		pr_warn("%s: HPD read-back did not follow (pin_ok=%u receiver_ok=%u) - continuing; the edge is verified by the source's reaction, not by this sample\n",
			dev->name, pin_ok, receiver_ok);
	return 0;
}

/*
 * M47: re-push the EDID and re-pulse HPD without reloading the module, so a
 * sweep over edid_opcode / edid_timeout_ms can be watched against the source
 * live. HPD has to fall and rise again: a source only re-reads the EDID on a
 * hotplug edge.
 */
/*
 * M48. Isolate HPD. Every test so far changed several things at once (input
 * select, receiver init, EDID push, HPD), and the source reacted exactly once
 * - which is as consistent with a coincidence (camera idle-timeout) as with
 * our hotplug edge. A source's response to HPD is the one thing we can test
 * with a single variable: pulse it, change nothing else, and see whether the
 * camera reacts every time.
 *
 * If it does, the board-level hotplug line really is ours to drive and the
 * EDID is being served by something we do not control (the factory-programmed
 * EDID MCU the RE docs mention) - which would make the remaining fault the
 * receiver's own configuration. If it never reacts, our HPD is not reaching
 * the connector at all and no amount of EDID work will ever matter.
 */
/*
 * M49. HPD is now PROVEN to reach the connector: the camera reacts to the
 * hotplug edge, then falls back to its LCD instead of staying in HDMI output
 * - which is exactly what a source does when it sees a sink appear, tries to
 * read the EDID over DDC, and gets nothing. So the EDID is the single
 * remaining fault, and it must be served out of the receiver (nothing else
 * exists on the bus to serve it).
 *
 * Find the EDID RAM by writability. A config register mostly has reserved or
 * hardwired bits and will not return an arbitrary byte; a RAM cell returns
 * exactly what was written. Sweep every bank, read the original, write a test
 * value, read back, and put the original back immediately. A contiguous run
 * of registers that hold arbitrary values is the EDID window.
 *
 * Original values are restored register-by-register, so the chip is left as
 * it was found; the receiver is re-initialised on every bring-up anyway.
 */
int mz0380_mst3367_wscan(struct mz0380_dev *dev)
{
	static const u8 test1 = 0x5a, test2 = 0xa5;
	unsigned int bank;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;

	pr_info("%s: writability scan (finding RAM windows; '#' = holds an arbitrary byte)\n",
		dev->name);

	mutex_lock(&mst3367_lock);
	for (bank = 0; bank <= 3; bank++) {
		bool ram[256] = { false };
		unsigned int reg, run_start = 0;
		unsigned int hits = 0, best = 0, best_start = 0;

		if (mst_bank(dev, bank))
			continue;

		/*
		 * Scan the WHOLE file, 0x01..0xff. The first pass stopped at
		 * 0x80 and bank3's writable run was still going at the edge -
		 * if it continues to 0xff that is a 128-byte window, exactly
		 * one EDID block. Reg 0x00 is the bank select: never written.
		 */
		for (reg = 1; reg <= 0xff; reg++) {
			u8 orig = 0, rb1 = 0, rb2 = 0;

			if (mst_rd(dev, reg, &orig))
				continue;
			if (mst_wr(dev, reg, test1) ||
			    mst_rd(dev, reg, &rb1) ||
			    mst_wr(dev, reg, test2) ||
			    mst_rd(dev, reg, &rb2)) {
				mst_wr(dev, reg, orig);
				continue;
			}
			mst_wr(dev, reg, orig);      /* restore immediately */

			if (rb1 == test1 && rb2 == test2) {
				ram[reg] = true;
				hits++;
			}
		}

		/* report contiguous runs - a RAM window is one long run */
		for (reg = 1; reg <= 0x100; reg++) {
			bool is_ram = (reg <= 0xff) && ram[reg];

			if (is_ram && !run_start) {
				run_start = reg;
			} else if (!is_ram && run_start) {
				unsigned int len = reg - run_start;

				if (len >= 8)
					pr_info("%s: bank%u run 0x%02x-0x%02x (%u bytes)\n",
						dev->name, bank, run_start,
						reg - 1, len);
				if (len > best) {
					best = len;
					best_start = run_start;
				}
				run_start = 0;
			}
		}
		pr_info("%s: bank%u: %u writable, longest run %u bytes at 0x%02x%s\n",
			dev->name, bank, hits, best, best_start,
			best >= 128 ? "  <- EDID-SIZED WINDOW" : "");
	}

	mst_bank(dev, MST3367_BANK0);
	mutex_unlock(&mst3367_lock);
	pr_info("%s: writability scan done - a long run of '#' is the EDID RAM candidate\n",
		dev->name);
	return 0;
}

int mz0380_mst3367_hpd_pulse(struct mz0380_dev *dev, unsigned int count,
			     unsigned int gap_ms)
{
	unsigned int i;
	int ret = 0;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;

	mutex_lock(&mst3367_lock);
	for (i = 0; i < count; i++) {
		pr_info("%s: HPD pulse %u/%u: deassert\n",
			dev->name, i + 1, count);
		ret = mz0380_mst3367_hpd(dev, false);
		if (ret)
			break;
		msleep(gap_ms);
		pr_info("%s: HPD pulse %u/%u: assert - watch the source now\n",
			dev->name, i + 1, count);
		ret = mz0380_mst3367_hpd(dev, true);
		if (ret)
			break;
		msleep(gap_ms);
	}
	mutex_unlock(&mst3367_lock);
	return ret;
}

int mz0380_mst3367_reload_edid(struct mz0380_dev *dev)
{
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;
	if (!dev->mst3367_ready) {
		ret = mz0380_mst3367_bringup(dev);
		if (ret)
			return ret;
	}

	mutex_lock(&mst3367_lock);
	ret = mz0380_mst3367_hpd(dev, false);
	if (ret)
		goto out;
	msleep(200);
	ret = mz0380_mst3367_load_edid(dev);
	if (ret)
		goto out;
	msleep(100);
	ret = mz0380_mst3367_hpd(dev, true);
out:
	mutex_unlock(&mst3367_lock);
	return ret;
}

int mz0380_mst3367_bringup(struct mz0380_dev *dev)
{
	int alive;
	int ret = 0;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_warn("%s: MST3367 bring-up skipped - firmware not ready\n",
			dev->name);
		return -ENODEV;
	}

	mutex_lock(&mst3367_lock);
	if (dev->mst3367_ready)
		goto out;
	dev->mst3367_ready = false;
	/* A physical reset invalidates the cached mode and requires the latch pulse. */
	dev->signal_locked = false;

	ret = mz0380_mst3367_reset(dev);
	if (ret)
		goto failed;

	/*
	 * The reset above forces the driven pins to outputs, but the DIR data
	 * polarity is a guess. If it was the wrong one we have just made them
	 * inputs, reset stays asserted and the bus is dead - which is exactly
	 * the state the M51 bit-bang probe left the card in. Detect that and
	 * retry with the opposite sense, then remember which one worked.
	 */
	alive = mst_bus_alive(dev);
	if (alive < 0) {
		ret = alive;
		goto failed;
	}
	if (!alive) {
		pr_info("%s: MST3367 silent after reset (dir_invert=%u) - retrying with the opposite GPIO_DIR sense\n",
			dev->name, mz0380_gpio_dir_invert);
		mz0380_gpio_dir_invert = !mz0380_gpio_dir_invert;
		ret = mz0380_mst3367_reset(dev);
		if (ret)
			goto failed;

		alive = mst_bus_alive(dev);
		if (alive < 0) {
			ret = alive;
			goto failed;
		}
		if (alive) {
			pr_info("%s: MST3367 answering with dir_invert=%u - GPIO direction polarity now known\n",
				dev->name, mz0380_gpio_dir_invert);
		} else {
			mz0380_gpio_dir_invert = !mz0380_gpio_dir_invert;
			pr_warn("%s: MST3367 still silent under both GPIO_DIR senses - receiver held in reset or unpowered; a mains-off cold boot restores the card's own pin config\n",
				dev->name);
			ret = -ENODEV;
			goto failed;
		}
	}

	ret = mz0380_mst3367_init_regs(dev);
	if (ret)
		goto failed;

	/*
	 * Present the card as a sink: EDID first, then HPD. Toggle HPD low
	 * across the load so a source that was already attached re-reads the
	 * EDID instead of keeping whatever it saw before.
	 */
	ret = mz0380_mst3367_hpd(dev, false);
	if (ret)
		goto failed;
	/*
	 * M61: an EDID failure must NOT abort the bring-up. Nothing on this
	 * board has ever ACKed an EDID write - there is no EEPROM on the
	 * receiver's bus (M43/M51) - so with a synchronous edid_timeout_ms
	 * this returns -110 every single time. Gating on it left the receiver
	 * un-reset, un-initialised and HPD-less, which is why a run could
	 * produce no detect samples at all rather than merely no lock.
	 *
	 * The receiver locks perfectly well without an EDID: every lock this
	 * project has ever recorded happened with the EDID push failing. EDID
	 * only decides whether the SOURCE keeps transmitting.
	 */
	ret = mz0380_mst3367_load_edid(dev);
	if (ret)
		pr_warn("%s: EDID push failed (%d) - continuing; the receiver still detects, but the source will not hold its output\n",
			dev->name, ret);
	msleep(100);            /* let the source settle before the edge */
	ret = mz0380_mst3367_hpd(dev, true);
	if (ret)
		goto failed;

	dev->mst3367_ready = true;
	pr_info("%s: MST3367 receiver brought up (reset released, init applied)\n",
		dev->name);
	goto out;

failed:
	dev->mst3367_ready = false;
	pr_warn("%s: MST3367 bring-up failed (%d); receiver remains not ready\n",
		dev->name, ret);
out:
	mutex_unlock(&mst3367_lock);
	return ret;
}
