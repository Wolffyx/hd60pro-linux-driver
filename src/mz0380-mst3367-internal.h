/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _MZ0380_MST3367_INTERNAL_H_
#define _MZ0380_MST3367_INTERNAL_H_

#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/v4l2-dv-timings.h>
#include <media/v4l2-dv-timings.h>

#include "mz0380.h"
#include "mz0380-reg.h"

/* BANK0 0xb0: YUV422, 8-bit, embedded sync - what the VIC wants (M100). */
#define MST3367_B0_OUTPUT	0x21
/* BANK0 0x92: hdcapm's CSC control byte, the RGB -> YCbCr matrix (M130). */
#define MST3367_CSC_CTL_HDCAPM	0x40

#define MST3367_TRY(_op) do { \
		ret = (_op); \
		if (ret) \
			return ret; \
	} while (0)

struct mst3367_measured {
	u16 htotal, vtotal;
	u16 hperiod, vperiod;
	u16 hperiod_raw, vperiod_raw;
	u16 lines;
	u8 detect;
	const char *reject;
	bool interlace_from_geometry;
	bool lock_ended_during_pass;
	u8 r5f;
	bool interlaced;
};

extern struct mutex mst3367_lock;
bool mst3367_status_locked(u8 status);
int mst_wr(struct mz0380_dev *dev, u8 reg, u8 val);
int mst_rd(struct mz0380_dev *dev, u8 reg, u8 *val);
int mst_bank(struct mz0380_dev *dev, u8 bank);
int mst_set(struct mz0380_dev *dev, u8 reg, u8 mask);
int mst_clr(struct mz0380_dev *dev, u8 reg, u8 mask);

int mst3367_measure(struct mz0380_dev *dev, struct mst3367_measured *out);
bool mst3367_measurements_agree(const struct mst3367_measured *a,
				const struct mst3367_measured *b);
int mst3367_set_auto_position(struct mz0380_dev *dev, bool enable);
const struct v4l2_dv_timings *
mst3367_match_mode(const struct mst3367_measured *m, bool *scaled);

#endif
