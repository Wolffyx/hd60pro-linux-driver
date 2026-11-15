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
#include "mz0380-edid.h"

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
						dev->mst_gpio_dir_invert);
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
	/* M109: pin 8, the companion strap, low as Windows drives it. */
	ret = mz0380_gpio_set(dev, MZ0380_GPIO_RX_STRAP, 0);
	if (ret)
		return ret;
	msleep(50);
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
	/* M101: reads back 0x20 - bit 2 does not stick; Windows sets it too. */
	MST3367_TRY(mst_set(dev, 0xae, 0x04));
	MST3367_TRY(mst_wr(dev, 0xad, 0x05)); /* enable low-pass filter */
	/* Exact HD60 Pro FUN_14024dc28 values (the sibling uses e0/08). */
	MST3367_TRY(mst_wr(dev, 0xb1, 0xc0));
	MST3367_TRY(mst_wr(dev, 0xb2, 0x00));
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
	MST3367_TRY(mst_wr(dev, 0xb5, 0x0c));

	/* CSC */
	MST3367_TRY(mst_wr(dev, 0x90, 0x15));
	MST3367_TRY(mst_wr(dev, 0x91, 0x15));
	/*
	 * csctbl[0] lands on 0x92, the CSC control byte (M130).
	 * mz0380_mst3367_apply_csc_mode() re-applies it at stream start, once
	 * the receiver has locked and the input colour space is known.
	 */
	for (i = 0; i < ARRAY_SIZE(csctbl); i++)
		MST3367_TRY(mst_wr(dev, 0x92 + i, csctbl[i]));

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
				       dev->mst_gpio_dir_invert);
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

		ret = mz0380_send_command(dev, MZ0380_CMD_I2C_WRITE_S, params,
					  ARRAY_SIZE(params), NULL, 100);
		if (ret) {
			dev_dbg(&dev->pci->dev,
				"EDID write failed at offset %u (%d)\n", off, ret);
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
	/*
	 * On every HD60 Pro measured so far nothing holds an EDID at 0xa0 and
	 * this reads back empty (M43/M51); the source serves its own. Kept as
	 * the one-line answer should a board ever differ.
	 */
	dev_dbg(&dev->pci->dev, "EDID read-back %s (ret=%d, %*ph)\n",
		!ret && verify[1] == 0xff && verify[2] == 0xff &&
		verify[6] == 0xff ? "holds our EDID" : "is empty",
		ret, 8, verify);

	/* hand the store back to the connector side, as Windows does */
	mz0380_gpio_set(dev, MZ0380_GPIO_EDID_MUX, 0);

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
	int ret;

	ret = mst_bank(dev, MST3367_BANK0);
	if (ret)
		return ret;
	ret = mst_wr(dev, MST3367_B0_HPD,
		     on ? MST3367_B0_HPD_ON : MST3367_B0_HPD_OFF);
	if (ret)
		return ret;

	/*
	 * Drive the board pin - and make sure it IS driven: a GPIO_SET on a pin
	 * left as an input silently does nothing (the M51 probe did exactly
	 * that to pin 1).
	 */
	ret = mz0380_gpio_force_output(dev, MZ0380_GPIO_HPD,
				       dev->mst_gpio_dir_invert);
	if (ret)
		return ret;
	/*
	 * M69: HPD is ACTIVE-LOW on this board (win64 FUN_14024eeb8:
	 * pin1 = ~(arg>>4)&1 - HPD_ON drives the pin LOW).
	 *
	 * M61: what proves the edge is the source reacting to it; a GPIO read
	 * returns the input register, which need not follow a driven output,
	 * so the read-backs that used to follow were dropped (M247).
	 */
	return mz0380_gpio_set(dev, MZ0380_GPIO_HPD, !on);
}

/* M48: pulse HPD only, nothing else - /proc/mz0380-hdmi "hpd". */
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

/*
 * M248: values this driver's bring-up leaves in BANK0 and nothing rewrites
 * after it, each one read back on hardware (0x51/0xb7 in M43, 0xad/0xb4 in
 * M101). 0xb7 = HPD asserted is the bring-up's last step.
 */
static const struct {
	u8 reg;
	u8 val;
} mst3367_bringup_marks[] = {
	{ 0xad, 0x05 },
	{ 0xb4, 0x54 },
	{ 0x51, 0x89 },
	{ MST3367_B0_HPD, MST3367_B0_HPD_ON },
};

/*
 * Does the receiver still hold a completed bring-up? Only a module reload
 * within one power cycle leaves it so: the card powers up with the receiver
 * in reset (every read 0x00), and unload touches neither the receiver nor the
 * GPIOs. Returns 1 if so, 0 if not, or a mailbox errno.
 */
static int mst3367_holds_bringup(struct mz0380_dev *dev)
{
	unsigned int i;
	int ret;

	ret = mst_bank(dev, MST3367_BANK0);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(mst3367_bringup_marks); i++) {
		u8 v = 0;

		ret = mst_rd(dev, mst3367_bringup_marks[i].reg, &v);
		if (ret)
			return ret;
		if (v != mst3367_bringup_marks[i].val)
			return 0;
	}
	return 1;
}

/*
 * Release the receiver from reset and prove the bus answers. The reset forces
 * the driven pins to outputs, but the DIR data polarity is a guess. If it was
 * the wrong one we have just made them inputs, reset stays asserted and the
 * bus is dead - which is exactly the state the M51 bit-bang probe left the
 * card in. Detect that and retry with the opposite sense, then remember which
 * one worked.
 */
static int mst3367_reset_until_alive(struct mz0380_dev *dev)
{
	int alive;
	int ret;

	ret = mz0380_mst3367_reset(dev);
	if (ret)
		return ret;
	alive = mst_bus_alive(dev);
	if (alive)
		return alive < 0 ? alive : 0;

	dev_dbg(&dev->pci->dev,
		"MST3367 silent after reset (dir_invert=%u) - retrying with the opposite GPIO_DIR sense\n",
		dev->mst_gpio_dir_invert);
	dev->mst_gpio_dir_invert = !dev->mst_gpio_dir_invert;
	ret = mz0380_mst3367_reset(dev);
	if (ret)
		return ret;
	alive = mst_bus_alive(dev);
	if (alive < 0)
		return alive;
	if (alive) {
		dev_dbg(&dev->pci->dev,
			"MST3367 answering with dir_invert=%u\n",
			dev->mst_gpio_dir_invert);
		return 0;
	}
	dev->mst_gpio_dir_invert = !dev->mst_gpio_dir_invert;
	pr_warn("%s: MST3367 still silent under both GPIO_DIR senses - receiver held in reset or unpowered; a mains-off cold boot restores the card's own pin config\n",
		dev->name);
	return -ENODEV;
}

/* Reset, init, EDID, hotplug: what a power cycle needs. */
static int mst3367_cold_bringup(struct mz0380_dev *dev)
{
	int ret;

	ret = mst3367_reset_until_alive(dev);
	if (ret)
		return ret;
	ret = mz0380_mst3367_init_regs(dev);
	if (ret)
		return ret;

	/*
	 * Present the card as a sink: EDID first, then HPD. Toggle HPD low
	 * across the load so a source that was already attached re-reads the
	 * EDID instead of keeping whatever it saw before.
	 */
	ret = mz0380_mst3367_hpd(dev, false);
	if (ret)
		return ret;
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
		dev_dbg(&dev->pci->dev,
			"EDID push failed (%d) - expected on this board; the receiver detects without it\n",
			ret);
	msleep(100);            /* let the source settle before the edge */
	ret = mz0380_mst3367_hpd(dev, true);
	if (ret)
		return ret;
	dev_dbg(&dev->pci->dev,
		"MST3367 receiver brought up (reset released, init applied)\n");
	return 0;
}

/*
 * M248: a reload keeps a receiver that is already brought up. The cold path
 * drops hot-plug for ~0.5 s, and a camera reads that as a disconnect: it
 * leaves HDMI mode and takes seconds to come back, if it does - so every
 * module load used to cost the source.
 */
int mz0380_mst3367_bringup(struct mz0380_dev *dev)
{
	int ret = 0;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		dev_dbg(&dev->pci->dev,
			"MST3367 bring-up skipped - card handshake not complete\n");
		return -ENODEV;
	}

	mutex_lock(&mst3367_lock);
	if (dev->mst3367_ready)
		goto out;
	/* Either way the mode is unknown: the first lock commits and latches. */
	dev->signal_locked = false;
	dev->mst_committed_valid = false;

	ret = mst3367_holds_bringup(dev);
	if (ret > 0) {
		dev_info(&dev->pci->dev,
			 "HDMI receiver kept from the previous load (no reset, no hotplug)\n");
		ret = 0;
	} else if (!ret) {
		ret = mst3367_cold_bringup(dev);
	}
	dev->mst3367_ready = !ret;
	if (ret)
		pr_warn("%s: MST3367 bring-up failed (%d); receiver remains not ready\n",
			dev->name, ret);
out:
	mutex_unlock(&mst3367_lock);
	return ret;
}
