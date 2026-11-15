// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 safe register snapshot definitions and helpers.
 */

#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>

#include "mz0380-internal.h"


#define MZ0380_PROC_CMD_MAX 64

const struct mz0380_snapshot_reg mz0380_snapshot_regs[] = {
	{
		.section = "BAR0/MMIO",
		.name = "mmio_probe_0000",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x0000,
		.min_profile = 1,
	},
	{
		.section = "BAR0/MMIO",
		.name = "mmio_probe_0004",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x0004,
		.min_profile = 1,
	},
	{
		.section = "BAR5/CFG",
		.name = "cfg_probe_0000",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0000,
		.min_profile = 1,
	},
	{
		.section = "BAR5/CFG",
		.name = "cfg_probe_0004",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0004,
		.min_profile = 1,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0008",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0008,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_000c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x000c,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0010",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0010,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0014",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0014,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0018",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0018,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_001c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x001c,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0020",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0020,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0024",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0024,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0028",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0028,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_002c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x002c,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0030",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0030,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0034",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0034,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_0038",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0038,
		.min_profile = 2,
	},
	{
		.section = "BAR5/CFG experimental",
		.name = "cfg_exp_003c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x003c,
		.min_profile = 2,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00a8",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00a8,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00ac",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00ac,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00c4",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00c4,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00c8",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00c8,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00d0",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00d0,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00d4",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00d4,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00d8",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00d8,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00dc",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00dc,
		.min_profile = 3,
	},
	{
		.section = "BAR0/MMIO experimental",
		.name = "mmio_exp_00e4",
		.map = MZ0380_MAP_BAR_MMIO,
		.reg = 0x00e4,
		.min_profile = 3,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0040",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0040,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0044",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0044,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0048",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0048,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_004c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x004c,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0050",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0050,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0054",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0054,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0058",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0058,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_005c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x005c,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0060",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0060,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0064",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0064,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0068",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0068,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_006c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x006c,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0070",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0070,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0074",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0074,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_0078",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0078,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG extended experimental",
		.name = "cfg_ext_007c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x007c,
		.min_profile = 4,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_0080",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0080,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_0084",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0084,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_0088",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0088,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_008c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x008c,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_0090",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0090,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_0094",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0094,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_0098",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x0098,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_009c",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x009c,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00a0",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00a0,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00a4",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00a4,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00a8",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00a8,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00ac",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00ac,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00b0",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00b0,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00b4",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00b4,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00b8",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00b8,
		.min_profile = 5,
	},
	{
		.section = "BAR5/CFG second extended experimental",
		.name = "cfg_ext2_00bc",
		.map = MZ0380_MAP_BAR_CFG,
		.reg = 0x00bc,
		.min_profile = 5,
	},
	/*
	 * Profile 6 (M0 observability widen): mailbox command/status/param
	 * window (0x00c0..0x00e4), DMA ring window (0x0200..0x0234), and the
	 * firmware upload seq/ack/buffer window (0x0300..0x0310, 0x0400..0x0410).
	 * All BAR5, always mapped, read-only. Lets snapshot diffs reveal whether
	 * the CHECKME offsets in mz0380-reg.h respond to activity.
	 */
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00c0", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00c0, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00c4", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00c4, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00c8", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00c8, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00cc", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00cc, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00d0", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00d0, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00d4", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00d4, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00d8", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00d8, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00dc", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00dc, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00e0", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00e0, .min_profile = 6, },
	{ .section = "BAR5/CFG mailbox (M0)",  .name = "cfg_m6_00e4", .map = MZ0380_MAP_BAR_CFG, .reg = 0x00e4, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0200", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0200, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0204", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0204, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0208", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0208, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_020c", .map = MZ0380_MAP_BAR_CFG, .reg = 0x020c, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0210", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0210, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0214", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0214, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0220", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0220, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0224", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0224, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0228", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0228, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_022c", .map = MZ0380_MAP_BAR_CFG, .reg = 0x022c, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0230", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0230, .min_profile = 6, },
	{ .section = "BAR5/CFG ring (M0)",     .name = "cfg_m6_0234", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0234, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0300", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0300, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0304", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0304, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0308", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0308, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_030c", .map = MZ0380_MAP_BAR_CFG, .reg = 0x030c, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0310", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0310, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0400", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0400, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0404", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0404, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0408", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0408, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_040c", .map = MZ0380_MAP_BAR_CFG, .reg = 0x040c, .min_profile = 6, },
	{ .section = "BAR5/CFG firmware (M0)", .name = "cfg_m6_0410", .map = MZ0380_MAP_BAR_CFG, .reg = 0x0410, .min_profile = 6, },
};

const unsigned int mz0380_snapshot_regs_count =
	ARRAY_SIZE(mz0380_snapshot_regs);

unsigned int mz0380_snapshot_profile(void)
{
	if (snapshot_profile < 2)
		return 1;

	if (snapshot_profile < 3)
		return 2;

	if (snapshot_profile < 4)
		return 3;

	if (snapshot_profile < 5)
		return 4;

	if (snapshot_profile < 6)
		return 5;

	return 6;
}

static void mz0380_snapshot_note(struct mz0380_dev *dev, u32 value,
				 char *note, size_t note_len)
{
	int map;

	note[0] = '\0';

	for (map = 0; map < MZ0380_MAX_MAPS; map++) {
		resource_size_t start = dev->bar_start[map];
		resource_size_t end = start + dev->bar_len[map];
		unsigned long long offset;

		if (!dev->lmmio[map] || value < start || value >= end)
			continue;

		offset = (unsigned long long)(value - start);
		if (offset & 0x3)
			snprintf(note, note_len,
				 " -> bar%d+0x%llx (unaligned)",
				 dev->bar_nr[map], offset);
		else
			snprintf(note, note_len, " -> bar%d+0x%llx",
				 dev->bar_nr[map], offset);
		break;
	}
}

void mz0380_dump_snapshot_reg(struct seq_file *m, struct mz0380_dev *dev,
				     const struct mz0380_snapshot_reg *snap)
{
	char note[48];
	u32 value;

	value = mz_read(dev, snap->map, snap->reg);
	mz0380_snapshot_note(dev, value, note, sizeof(note));

	seq_printf(m, "    %-16s [0x%04x] = %08x%s\n",
		   snap->name, snap->reg, value, note);
}

void mz0380_dump_snapshot_range(struct seq_file *m, struct mz0380_dev *dev,
				       unsigned int profile, const char *title,
				       int map, u32 start, u32 end)
{
	unsigned int i;
	bool found = false;

	seq_printf(m, "  %s\n", title);

	for (i = 0; i < ARRAY_SIZE(mz0380_snapshot_regs); i++) {
		const struct mz0380_snapshot_reg *snap = &mz0380_snapshot_regs[i];

		if (profile < snap->min_profile)
			continue;
		if (snap->map != map)
			continue;
		if (snap->reg < start || snap->reg > end)
			continue;

		mz0380_dump_snapshot_reg(m, dev, snap);
		found = true;
	}

	if (!found)
		seq_printf(m,
			   "    unavailable at snapshot_profile=%u\n",
			   profile);
}
