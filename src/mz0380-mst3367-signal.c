// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MST3367 mode measurement, matching, and digital-output commit.
 */

#include "mz0380-mst3367-internal.h"

/* --- signal detect -------------------------------------------------------- */

/*
 * Map measured geometry to a standard v4l2_dv_timings. We know active width,
 * interlace, and an approximate frame rate; that uniquely picks a CEA preset
 * for the modes this HDMI capture card supports. The preset carries the exact
 * blanking/pixelclock the raw detect registers do not give us directly.
 *
 * vperiod is vertical-update rate times ten: about 600 for either 60p frames
 * or 60i fields. Interlace therefore comes from R5F bit3, not from halving
 * vperiod, and every counter is matched against an explicit tolerance range.
 */
struct mst3367_mode {
	struct v4l2_dv_timings timings;
	u16 htotal_min, htotal_max;
	u16 vtotal_min, vtotal_max;
	u16 hperiod_min, hperiod_max;
	u16 vperiod_min, vperiod_max;
	bool interlaced;
};


/*
 * M56: the progressive rows below are hdcapm's table verbatim (GPL, same
 * MST3367); the 1080i rows extend it from the corresponding CEA line/field
 * rates. Matching is on four measured ranges plus interlace - NOT hactive.
 * Note htotal here is the receiver's own counter, not always the video
 * horizontal total: 720p60 appears at ~2475 or ~1650 depending on the TMDS
 * clock domain, so both are listed.
 */
static const struct mst3367_mode mst3367_modes[] = {
	/*                          htot_min htot_max vtot_min vtot_max
	 *                          hper_min hper_max vper_min vper_max il  */
	{ V4L2_DV_BT_CEA_720X480P59_94,  845,  865,  520,  525,
					 310,  320,  595,  605, false },
	{ V4L2_DV_BT_CEA_1280X720P30,   2300, 2500,  745,  755,
					 215,  235,  290,  310, false },
	{ V4L2_DV_BT_CEA_1280X720P50,   2965, 2985,  745,  755,
					 360,  380,  480,  520, false },
	{ V4L2_DV_BT_CEA_1280X720P60,   2470, 2480,  745,  755,
					 445,  455,  595,  605, false },
	/* same 720p60 seen in the other clock domain (hdcapm: "Tivo") */
	{ V4L2_DV_BT_CEA_1280X720P60,   1645, 1655,  745,  755,
					 445,  455,  595,  605, false },
	{ V4L2_DV_BT_CEA_1920X1080P24,  4080, 4105, 1120, 1130,
					 260,  280,  230,  250, false },
	{ V4L2_DV_BT_CEA_1920X1080P25,  3950, 3970, 1120, 1130,
					 270,  290,  240,  254, false },
	{ V4L2_DV_BT_CEA_1920X1080P30,  2295, 3305, 1120, 1130,
					 330,  345,  290,  310, false },
	/* Interlaced totals count fields; I60 also represents 59.94 Hz. */
	{ V4L2_DV_BT_CEA_1920X1080I50,  3950, 3970,  555,  570,
					 270,  290,  490,  510, true  },
	{ V4L2_DV_BT_CEA_1920X1080I60,  3290, 3310,  555,  570,
					 330,  345,  590,  610, true  },
	{ V4L2_DV_BT_CEA_1920X1080P50,  3950, 3970, 1120, 1130,
					 550,  570,  480,  520, false },
	{ V4L2_DV_BT_CEA_1920X1080P60,  3290, 3310, 1120, 1130,
					 665,  685,  595,  605, false },
};

static bool mst3367_in_range(const struct mst3367_mode *e, u16 htotal,
			     u16 vtotal, u16 hperiod, u16 vperiod, bool il)
{
	return htotal  >= e->htotal_min  && htotal  <= e->htotal_max &&
	       vtotal  >= e->vtotal_min  && vtotal  <= e->vtotal_max &&
	       hperiod >= e->hperiod_min && hperiod <= e->hperiod_max &&
	       vperiod >= e->vperiod_min && vperiod <= e->vperiod_max &&
	       il == e->interlaced;
}

/*
 * M58 (hardware). Only htotal is in a different domain than hdcapm's table.
 * The clean full-lock sample reads
 *
 *   htot=2200 vtot=1125 hper=674 vper=599
 *
 * where vtotal (1125) and vperiod (599 == 60.0Hz, from 1250000/raw) already
 * sit inside the 1080p60 row, confirming hdcapm's vertical constants on this
 * silicon; hperiod likewise (raw 2372 -> 160MHz reference -> 67.4kHz). But
 * htotal is the TRUE video total, 2200, against the table's 3290-3310. That
 * split is not new: hdcapm's own table carries 720p60 twice, at 1650 (true)
 * and 2475 (x1.5), because the chip reports either depending on the clock
 * domain it locks in.
 *
 * So the table stays verbatim and only htotal is normalised. Raw units are
 * tried first, since the chip does report the x1.5 domain for some sources.
 *
 * M79 (hardware). A third domain exists, and it is HDMI DEEP COLOUR. The
 * chip counts htotal in TMDS character clocks, which deep colour scales:
 * 24-bit = x1, 30-bit = x1.25, 36-bit = x1.5. That is exactly why hdcapm
 * carries 720p60 twice (1650 true, 2475 = x1.5): the second row is a 36-bit
 * source, not a different mode.
 *
 * A source arriving here read htot=2750 vtot=1125 hact=1920 hper=674
 * vper=599 - self-consistent 1080p60 in every field except htotal, and
 * 2750 == 2200 x 1.25, i.e. 30-bit deep colour. Only the TMDS-domain counter
 * moves: hperiod and vperiod run off a fixed reference and read unchanged
 * (67.4 kHz / 59.9 Hz), and hactive is a video-domain counter and reads a
 * true 1920. The table sits in the x1.5 domain, so the correction from a
 * 30-bit measurement is x1.5/x1.25 = x6/5: 2750 * 6 / 5 == 3300, dead centre
 * of the 1080p60 row. Without it a perfectly good 1080p60 source is rejected
 * as "coherent but unsupported" and never reaches STREAMON at all.
 *
 * (M57 read x1.25/x0.8 into the vertical fields too. That was fitted to one
 * torn sample - htot=2200 vtot=899 vper=750 is a self-consistent 75Hz, the
 * right TMDS clock with vsync still settling, not a unit mismatch. Retracted.)
 */
const struct v4l2_dv_timings *
mst3367_match_mode(const struct mst3367_measured *m, bool *scaled)
{
	unsigned int i;

	if (scaled)
		*scaled = false;

	for (i = 0; i < ARRAY_SIZE(mst3367_modes); i++)
		if (mst3367_in_range(&mst3367_modes[i], m->htotal, m->vtotal,
				     m->hperiod, m->vperiod, m->interlaced))
			return &mst3367_modes[i].timings;

	for (i = 0; i < ARRAY_SIZE(mst3367_modes); i++)
		if (mst3367_in_range(&mst3367_modes[i], m->htotal * 3 / 2,
				     m->vtotal, m->hperiod,
				     m->vperiod, m->interlaced)) {
			if (scaled)
				*scaled = true;
			return &mst3367_modes[i].timings;
		}

	/* M79: 30-bit deep colour reports x1.25; the table is x1.5. */
	for (i = 0; i < ARRAY_SIZE(mst3367_modes); i++)
		if (mst3367_in_range(&mst3367_modes[i], m->htotal * 6 / 5,
				     m->vtotal, m->hperiod,
				     m->vperiod, m->interlaced)) {
			if (scaled)
				*scaled = true;
			return &mst3367_modes[i].timings;
		}

	return NULL;
}

int mst3367_set_auto_position(struct mz0380_dev *dev, bool enable)
{
	int ret;

	lockdep_assert_held(&mst3367_lock);

	/*
	 * M74: never re-arm acquisition while the encoder is capturing.
	 *
	 * AUTO_POSITION restarts the receiver's position/phase hunt. Doing that
	 * to a receiver that is actively feeding BT1120 to the SoC's VIC can
	 * only disturb the very frames we are trying to capture - and the
	 * diagnostic watch runs concurrently with capture by design, writing
	 * this register on every lock/no-lock transition of a source that
	 * flaps. Reads during streaming stay allowed; writes do not.
	 */
	if (READ_ONCE(dev->pipeline_running) && enable &&
	    !READ_ONCE(dev->signal_recovering)) {
		pr_info_ratelimited("%s: skipping auto-position re-arm while streaming\n",
				    dev->name);
		return 0;
	}
	ret = mst_bank(dev, MST3367_BANK0);
	if (ret)
		return ret;
	return mst_wr(dev, MST3367_B0_AUTO_POSITION,
		      enable ? MST3367_B0_AUTO_POSITION_ON :
			       MST3367_B0_AUTO_POSITION_OFF);
}

/*
 * Commit the receiver's digital output after a mode has stabilized.  On the
 * first accepted mode after init or lock loss, the HD60 Pro driver pulses
 * BANK2:07 bit4 to latch the new timing.  It then gates output with AB[7],
 * preserves B0's clock/polarity bits and selects 0x21 for ordinary modes.
 * The one special timing in our advertised table is 1280x720p30: it writes
 * BANK0:B2=3 and selects 0x20.  Treating every non-1080 mode as special was a
 * sibling-board shortcut and misclocked 720p50/60 and SD input.
 */
static int
mst3367_commit_digital_output(struct mz0380_dev *dev,
			      const struct v4l2_dv_timings *timings)
{
	const struct v4l2_bt_timings *bt = &timings->bt;
	u64 total = (u64)V4L2_DV_BT_FRAME_WIDTH(bt) *
		    V4L2_DV_BT_FRAME_HEIGHT(bt);
	u32 fps = total && bt->pixelclock ?
		  div_u64(bt->pixelclock + total / 2, total) : 0;
	bool special_720p30 = bt->width == 1280 && bt->height == 720 &&
			      !bt->interlaced && fps == 30;
	bool initial_acquisition = !dev->signal_locked;
	u8 reg07;
	u8 b0;
	int clear_ret;
	int ret;

	lockdep_assert_held(&mst3367_lock);
	/*
	 * M247: re-committing an unchanged mode freezes and retimes a receiver
	 * that may be feeding a running encoder - at every reattachment and
	 * every idle query. Only a new mode, or a fresh acquisition, needs it.
	 */
	if (!initial_acquisition && dev->mst_committed_valid &&
	    v4l2_match_dv_timings(&dev->mst_committed, timings, 0, false))
		return 0;
	if (initial_acquisition) {
		ret = mst_bank(dev, MST3367_BANK2);
		if (ret)
			return ret;
		ret = mst_rd(dev, 0x07, &reg07);
		if (ret)
			return ret;
		ret = mst_wr(dev, 0x07, reg07 | 0x10);
		if (ret)
			return ret;
		ret = mst_wr(dev, 0x07, reg07 & ~0x10);
		if (ret)
			return ret;
	}

	ret = mst_bank(dev, MST3367_BANK0);
	if (ret)
		return ret;

	/*
	 * M100: BANK0 0xb0 decoded from hdcapm's own commented-out
	 * alternatives: bit0 = embedded (1) vs external (0) sync, bit2 = 10-bit
	 * vs 8-bit. 0x21 - YUV422, 8-bit, EMBEDDED sync - is the only value
	 * that gets the SoC's VIC to initialise; it wants CCIR timing codes
	 * in-stream. (M80/M94 measured Windows' 0x14 block as worse.)
	 */
	ret = mst_wr(dev, 0xb2, special_720p30 ? 0x03 : 0x00);
	if (ret)
		return ret;
	ret = mst_set(dev, 0xab, 0x80);		/* freeze while retiming */
	if (ret)
		return ret;
	ret = mst_rd(dev, 0xb0, &b0);
	if (!ret)
		ret = mst_wr(dev, 0xb0, (b0 & 0xc2) |
			     (special_720p30 ? 0x20 : (MST3367_B0_OUTPUT & 0x3d)));
	clear_ret = mst_clr(dev, 0xab, 0x80);
	if (!ret && !clear_ret) {
		dev->mst_committed = *timings;
		dev->mst_committed_valid = true;
	}
	return ret ? ret : clear_ret;
}

/*
 * Read one complete timing snapshot.  The HD60 Pro Windows driver accepts a
 * sample only with all four R55 lock bits, re-reads R5C, and verifies that the
 * measured vertical rate agrees with hfreq / vtotal.  Those checks reject the
 * partial-lock rows whose counters changed while mailbox reads were in flight.
 * mst3367_lock must cover the whole banked transaction.
 */
static int mst3367_measure_once(struct mz0380_dev *dev,
				struct mst3367_measured *out)
{
	u8 detect, detect_after, hi, lo, vtotal_hi_after, vtotal_lo_after;
	u16 vtotal_after;
	u8 r57, r58, r59, r5a, r5f;
	u16 raw;
	u32 converted, calculated_vperiod;
	int ret;

	memset(out, 0, sizeof(*out));
	lockdep_assert_held(&mst3367_lock);

	MST3367_TRY(mst_bank(dev, MST3367_BANK0));
	MST3367_TRY(mst_rd(dev, MST3367_B0_DETECT, &detect));
	out->detect = detect;   /* M63: so a REJECTED sample still reports R55 */
	if (!mst3367_status_locked(detect)) {
		out->reject = "no full lock at start of pass";
		return -ENOLCK;
	}

	MST3367_TRY(mst_rd(dev, MST3367_B0_HTOTAL_HI, &hi));
	MST3367_TRY(mst_rd(dev, MST3367_B0_HTOTAL_LO, &lo));
	out->htotal = (((u16)hi << 8) | lo) & 0xfff;

	MST3367_TRY(mst_rd(dev, MST3367_B0_VTOTAL_HI, &hi));
	MST3367_TRY(mst_rd(dev, MST3367_B0_VTOTAL_LO, &lo));
	out->vtotal = (((u16)hi << 8) | lo) & 0x7ff;

	MST3367_TRY(mst_rd(dev, MST3367_B0_HPERIOD_HI, &r57));
	MST3367_TRY(mst_rd(dev, MST3367_B0_HPERIOD_LO, &r58));
	MST3367_TRY(mst_rd(dev, MST3367_B0_VPERIOD_HI, &r59));
	MST3367_TRY(mst_rd(dev, MST3367_B0_VPERIOD_LO, &r5a));
	MST3367_TRY(mst_rd(dev, MST3367_B0_INTERLACE, &r5f));

	raw = ((u16)(r57 & 0x3f) << 8) | r58;
	out->hperiod_raw = raw;
	converted = raw ? 1600000u / raw : 0;
	if (converted > U16_MAX) {
		out->reject = "hperiod counter too small (rate overflowed u16)";
		return -EAGAIN;
	}
	out->hperiod = converted;

	raw = ((u16)(r59 & 0x3f) << 8) | r5a;
	out->vperiod_raw = raw;
	converted = raw ? 1250000u / raw : 0;
	if (converted > U16_MAX) {
		out->reject = "vperiod counter too small (rate overflowed u16)";
		return -EAGAIN;
	}
	out->vperiod = converted;

	/*
	 * The actual HD60 Pro binary uses R5F bit3.  This also explains the
	 * known progressive R5F=0x17 sample: old hdcapm bit1 is set, bit3 is not.
	 *
	 * M64 (hardware): but bit3 alone is not trustworthy either. A sample
	 * measuring htot=2200 vtot=1120 hper=674 vper=602 - a textbook 1080p60,
	 * and progressive beyond doubt - carried R5F=0x48, bit3 set. Note 0x40
	 * is set in every torn sample we have ever logged, so R5F most likely
	 * carries a validity flag and its other bits mean nothing while it is
	 * raised.
	 *
	 * The geometry does not lie: lines = hfreq/vfreq is the number of lines
	 * per VERTICAL period, which equals vtotal for a progressive source and
	 * half of it when the vertical counter is timing fields. Decide on that
	 * and keep the register bit only for the case where the geometry is too
	 * degenerate to call.
	 */
	out->r5f = r5f;
	out->lines = out->vperiod ?
		(u16)((u32)out->hperiod * 1000u / out->vperiod) : 0;

	if (out->lines && out->vtotal) {
		int d_prog = abs((int)out->lines - (int)out->vtotal);
		int d_int  = abs((int)out->lines - (int)out->vtotal / 2);

		out->interlaced = d_int < d_prog;
		out->interlace_from_geometry = true;
	} else {
		out->interlaced = !!(r5f & MST3367_B0_INTERLACE_BIT);
		out->interlace_from_geometry = false;
	}

	/*
	 * M64: prove the snapshot before spending anything else. (hactive is
	 * not read at all since M247: nothing matches on it, and it cost a
	 * bank switch and three more round-trips per sample for a log line.)
	 */
	MST3367_TRY(mst_rd(dev, MST3367_B0_VTOTAL_HI, &vtotal_hi_after));
	MST3367_TRY(mst_rd(dev, MST3367_B0_VTOTAL_LO, &vtotal_lo_after));
	vtotal_after = (((u16)vtotal_hi_after << 8) | vtotal_lo_after) & 0x7ff;
	MST3367_TRY(mst_rd(dev, MST3367_B0_DETECT, &detect_after));
	/*
	 * M64: losing lock on this LAST read is not by itself a reason to throw
	 * the sample away, so the verdict is deferred until the sample has been
	 * checked on its own merits below. A bursty source (the microscope
	 * transmits for ~300ms after a power-cycle) routinely stops during the
	 * trailing round-trips, after the whole timing block has already been
	 * read - and a block that re-reads identical and passes its internal
	 * cross-check is good data no matter what the source did afterwards.
	 */
	/*
	 * M63: compare the WHOLE vtotal as a number, with a tolerance.
	 *
	 * The previous gate was (vtotal_lo ^ vtotal_lo_after) & 0xfe on the low
	 * byte alone, which is not a tolerance at all - it is a bit pattern:
	 *
	 *   1125 -> 1126 : 0x65 ^ 0x66 = 0x03, & 0xfe = 0x02 -> REJECTED
	 *   1124 -> 1125 : 0x64 ^ 0x65 = 0x01, & 0xfe = 0x00 -> accepted
	 *   1125 -> 1381 : low bytes identical                -> accepted (!)
	 *
	 * So a one-line wobble was rejected or accepted purely on the parity of
	 * the low byte, while a 256-line jump - the exact corruption an
	 * unlatched HI/LO pair produces - sailed through. Our own 1080p60
	 * sample sits at vtotal=1125, low byte 0x65, on the rejecting side of
	 * that coin: a settling source that moves one line is thrown away.
	 */
	if (abs((int)vtotal_after - (int)out->vtotal) > 2) {
		out->reject = "vtotal moved during the pass";
		return -EAGAIN;
	}

	/* Windows accepts 20.0--150.0 Hz and at most 0.5 Hz disagreement. */
	if (!out->htotal || !out->hperiod ||
	    out->vtotal < 150 || out->vperiod < 200 || out->vperiod > 1500) {
		out->reject =
			!out->htotal   ? "htotal is zero" :
			!out->hperiod  ? "hperiod is zero" :
			out->vtotal < 150 ? "vtotal below 150 lines" :
			out->vperiod < 200 ? "vperiod below 20.0 Hz" :
					 "vperiod above 150.0 Hz";
		return -EAGAIN;
	}
	/*
	 * M64: check the geometry, which covers both field orders.
	 *
	 * The old form was calculated_vperiod = hperiod*1000/(vtotal+1) against
	 * vperiod within 5 (0.5Hz). That silently assumed progressive: for
	 * 1080i60 the receiver reports vtotal as FRAME lines (1125) while
	 * vperiod times fields (600), so it computed 299 against 600 and threw
	 * every interlaced source away.
	 *
	 * lines (= hfreq/vfreq) is already the lines per vertical period, so
	 * the honest test is that it lands near vtotal (progressive) or near
	 * vtotal/2 (interlaced) - the same comparison that picked the field
	 * order above. Tolerance is in lines: both inputs are truncated
	 * integer divisions, so a couple of lines of slop is arithmetic, not
	 * signal instability.
	 */
	calculated_vperiod = out->interlaced ? out->vtotal / 2u : out->vtotal;
	if (abs((int)out->lines - (int)calculated_vperiod) > 8) {
		out->reject = "line count disagrees with vtotal";
		return -EAGAIN;
	}

	/*
	 * M64: everything above passed - the block re-read identical and the
	 * rate counters agree with the line total. Now, and only now, does the
	 * trailing lock state matter, and it is worth no more than a note.
	 */
	if (!mst3367_status_locked(detect_after)) {
		out->lock_ended_during_pass = true;
		dev_dbg(&dev->pci->dev,
			"MST3367 source stopped during the trailing reads (R55 %02x -> %02x) but the sample is self-consistent - keeping it\n",
			detect, detect_after);
	} else {
		out->detect = detect_after;
	}

	return 0;
}

bool mst3367_measurements_agree(const struct mst3367_measured *a,
				       const struct mst3367_measured *b)
{
	return abs((int)a->htotal - (int)b->htotal) <= 2 &&
	       abs((int)a->vtotal - (int)b->vtotal) <= 2 &&
	       abs((int)a->hperiod - (int)b->hperiod) <= 2 &&
	       abs((int)a->vperiod - (int)b->vperiod) <= 2 &&
	       a->interlaced == b->interlaced;
}

/*
 * M62: one pass. measure_once already proves its snapshot did not move and
 * that the fields are mutually consistent; a second confirming pass needs
 * ~170 ms of unbroken lock, which a bursty source does not hold. On failure
 * the rejected sample is handed back too - its numbers are the only evidence
 * of WHY a lock that really happened produced no timing.
 */
int mst3367_measure(struct mz0380_dev *dev, struct mst3367_measured *out)
{
	lockdep_assert_held(&mst3367_lock);
	return mst3367_measure_once(dev, out);
}

/*
 * Cheap steady-state presence probe: one bank select and one lock-byte read.
 * It deliberately does not update dev->signal_locked; the lifecycle worker
 * owns that state transition and follows a newly asserted lock with the full,
 * self-consistent timing measurement below.
 */
int mz0380_mst3367_read_lock(struct mz0380_dev *dev, bool *locked,
			     bool rearm_acquisition)
{
	u8 detect;
	int ret;

	*locked = false;
	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;
	if (!dev->mst3367_ready) {
		ret = mz0380_mst3367_bringup(dev);
		if (ret)
			return ret;
	}

	mutex_lock(&mst3367_lock);
	ret = mst_bank(dev, MST3367_BANK0);
	if (!ret)
		ret = mst_rd(dev, MST3367_B0_DETECT, &detect);
	if (!ret) {
		*locked = mst3367_status_locked(detect);
		/*
		 * M233: never re-arm a receiver that is already locked.
		 *
		 * AUTO_POSITION starts a continuous position/phase hunt, and
		 * this path only ever turns it ON. mz0380_mst3367_read_signal
		 * pairs its enable with the disable at its `matched:` label, so
		 * there the hunt is bounded; here nothing ever cleared it, and
		 * the receiver was left auto-adjusting for the rest of the
		 * capture. The operator sees that as the picture growing
		 * steadily brighter on a 60 Hz source, which Windows does not
		 * do - it does not re-arm mid-capture at all.
		 *
		 * The M74 guard in mst3367_set_auto_position was meant to stop
		 * exactly this, but it exempts signal_recovering, and the
		 * recovery worker sets that flag immediately before calling
		 * here. So the one caller the guard needed to catch was the one
		 * it let through.
		 *
		 * A locked receiver has nothing to re-acquire. Re-arm only on
		 * an actual loss, which is what the flag was for.
		 */
		if (rearm_acquisition && !*locked) {
			dev->mst_rearms++;
			ret = mst3367_set_auto_position(dev, true);
		}
	}
	mutex_unlock(&mst3367_lock);
	return ret;
}

/* Log a recognised mode once, not on every measurement of it. */
static void mst3367_log_mode(struct mz0380_dev *dev,
			     const struct v4l2_dv_timings *t,
			     const struct mst3367_measured *m, bool scaled)
{
	if (dev->mst_logged.bt.width &&
	    v4l2_match_dv_timings(&dev->mst_logged, t, 0, false))
		return;
	dev->mst_logged = *t;
	dev_info(&dev->pci->dev,
		 "HDMI signal: %ux%u%c@%u\n", t->bt.width, t->bt.height,
		 t->bt.interlaced ? 'i' : 'p', mz0380_source_fps(t));
	dev_dbg(&dev->pci->dev,
		"MST3367 htot=%u%s vtot=%u hper=%u vper=%u R55=0x%02x%s\n",
		m->htotal, scaled ? " [host units]" : "", m->vtotal,
		m->hperiod, m->vperiod, m->detect,
		m->interlace_from_geometry ? "" : " [interlace from R5F bit]");
}

/* A stable mode the card cannot capture: say so once per geometry. */
static void mst3367_log_unsupported(struct mz0380_dev *dev,
				    const struct mst3367_measured *m)
{
	if (dev->mst_unsupported_htotal == m->htotal &&
	    dev->mst_unsupported_vtotal == m->vtotal)
		return;
	dev->mst_unsupported_htotal = m->htotal;
	dev->mst_unsupported_vtotal = m->vtotal;
	dev_warn(&dev->pci->dev,
		 "HDMI source mode is not supported (htot=%u vtot=%u hper=%u vper=%u lines=%u %s, R55=0x%02x); set the source to 1080p/1080i/720p/480p - and please report it\n",
		 m->htotal, m->vtotal, m->hperiod, m->vperiod, m->lines,
		 m->interlaced ? "interlaced" : "progressive", m->detect);
}

/*
 * Read the MST3367 mode-detect block and fill *out. Returns 0 with a valid
 * timing, -ENOLCK when no lock was seen, -EAGAIN when lock was seen but no
 * coherent sample came of it, -ERANGE for a coherent unsupported mode, or an
 * I/O errno. Runs bring-up on first use.
 */
int mz0380_mst3367_read_signal(struct mz0380_dev *dev,
			       struct v4l2_dv_timings *out)
{
	const struct v4l2_dv_timings *match = NULL;
	struct mst3367_measured m = { 0 };
	/* m also receives REJECTED samples, so keep the unsupported one apart */
	struct mst3367_measured unsupported = { 0 };
	unsigned int unsupported_agree = 0;
	bool acquisition_enabled = false;
	bool saw_full_lock = false;
	bool scaled = false;
	unsigned long deadline;
	u8 detect = 0;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;
	if (!dev->mst3367_ready) {
		ret = mz0380_mst3367_bringup(dev);
		if (ret)
			return ret;
	}

	/*
	 * The Windows driver gates timing reads on the complete R55 lock mask;
	 * partial masks are acquisition states, not lower-quality locks. Keep
	 * auto-position enabled while acquiring or after any unstable sample,
	 * and disable it once a supported mode is identified.
	 */
	mutex_lock(&mst3367_lock);
	deadline = jiffies + msecs_to_jiffies(MZ0380_SIGNAL_POLL_MS);
	for (;;) {
		ret = mst_bank(dev, MST3367_BANK0);
		if (!ret)
			ret = mst_rd(dev, MST3367_B0_DETECT, &detect);
		if (ret)
			goto out_unlock;

		if (mst3367_status_locked(detect)) {
			saw_full_lock = true;
			ret = mst3367_measure(dev, &m);
			if (!ret) {
				match = mst3367_match_mode(&m, &scaled);
				if (match)
					break;
				/*
				 * M247: two agreeing samples of an unsupported
				 * mode settle it; polling out the full window
				 * only kept the mailbox busy.
				 */
				if (unsupported_agree &&
				    mst3367_measurements_agree(&unsupported, &m))
					unsupported_agree++;
				else
					unsupported_agree = 1;
				unsupported = m;
				if (unsupported_agree >= 2)
					break;
			} else if (ret != -ENOLCK && ret != -EAGAIN) {
				goto out_unlock;
			}
		}

		/* Lost lock, incoherent, or unsupported: keep acquiring. */
		if (!acquisition_enabled) {
			ret = mst3367_set_auto_position(dev, true);
			if (ret)
				goto out_unlock;
			acquisition_enabled = true;
		}
		if (time_after_eq(jiffies, deadline))
			break;
		msleep(20);
	}

	if (match) {
		ret = mst3367_set_auto_position(dev, false);
		if (!ret)
			ret = mst3367_commit_digital_output(dev, match);
		if (!ret) {
			*out = *match;
			mst3367_log_mode(dev, match, &m, scaled);
		}
	} else if (unsupported_agree) {
		mst3367_log_unsupported(dev, &unsupported);
		ret = -ERANGE;
	} else {
		if (saw_full_lock)
			dev_dbg(&dev->pci->dev,
				"MST3367 lock seen but no coherent sample (last: %s; htot=%u vtot=%u hper=%u vper=%u R55=0x%02x)\n",
				m.reject ?: "no reason recorded", m.htotal,
				m.vtotal, m.hperiod, m.vperiod, m.detect);
		ret = saw_full_lock ? -EAGAIN : -ENOLCK;
	}

out_unlock:
	mutex_unlock(&mst3367_lock);
	return ret;
}
