// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Driver for MZ0380 based capture cards.
 *
 *  Board ID table + setup. Subsystem variants known from the Windows
 *  GameCaptureHD60Pro.inf:
 *
 *    PCI\VEN_12AB&DEV_0380&SUBSYS_00031CFA  Rev. 1
 *    PCI\VEN_12AB&DEV_0380&SUBSYS_00051CFA  Rev. 2 (never released)
 *    PCI\VEN_12AB&DEV_0380&SUBSYS_00061CFA  Rev. 1 + Ryzen fix
 *    PCI\VEN_12AB&DEV_0381&SUBSYS_00101CFA  Rev. 3 (uses MZ0381 firmware)
 *    PCI\VEN_12AB&DEV_0380&SUBSYS_12AB05CF  Prototype
 */

#include "mz0380.h"

const struct mz0380_board mz0380_boards[] = {
	[MZ0380_BOARD_UNKNOWN] = {
		.name = "UNKNOWN/GENERIC",
		.windows_driver = "unknown",
	},
	[MZ0380_BOARD_ELGATO_HD60_PRO] = {
		.name = "Elgato Game Capture HD60 Pro",
		.windows_driver = "e60MZ0380.X64.SYS",
	},
	[MZ0380_BOARD_ELGATO_HD60_PRO_REV3] = {
		.name = "Elgato Game Capture HD60 Pro (Rev.3)",
		.windows_driver = "e60MZ0380.X64.SYS",
	},
};
const unsigned int mz0380_bcount = ARRAY_SIZE(mz0380_boards);

const struct mz0380_subid mz0380_subids[] = {
	/* Rev. 1 (original) */
	{ .subvendor = 0x1cfa, .subdevice = 0x0003,
	  .card = MZ0380_BOARD_ELGATO_HD60_PRO },
	/* Rev. 2 (never released) - treat as Rev. 1 */
	{ .subvendor = 0x1cfa, .subdevice = 0x0005,
	  .card = MZ0380_BOARD_ELGATO_HD60_PRO },
	/* Rev. 1 + Ryzen fix - confirmed in this test rig */
	{ .subvendor = 0x1cfa, .subdevice = 0x0006,
	  .card = MZ0380_BOARD_ELGATO_HD60_PRO },
	/* Rev. 3 - device id 0x0381, different firmware blob */
	{ .subvendor = 0x1cfa, .subdevice = 0x0010,
	  .card = MZ0380_BOARD_ELGATO_HD60_PRO_REV3 },
	/* Prototype */
	{ .subvendor = 0x12ab, .subdevice = 0x05cf,
	  .card = MZ0380_BOARD_ELGATO_HD60_PRO },
};
const unsigned int mz0380_idcount = ARRAY_SIZE(mz0380_subids);

void mz0380_card_list(struct mz0380_dev *dev)
{
	int i;

	if (dev->pci->subsystem_vendor == 0 &&
	    dev->pci->subsystem_device == 0) {
		pr_info("%s: Board has no valid PCIe subsystem ID.\n"
			"%s: Pass card=<n> as an insmod option to override autodetect.\n",
			dev->name, dev->name);
	} else {
		pr_info("%s: Board %04x:%04x is not known to the driver.\n"
			"%s: Pass card=<n> as an insmod option to override autodetect.\n",
			dev->name,
			dev->pci->subsystem_vendor,
			dev->pci->subsystem_device,
			dev->name);
	}

	pr_info("%s: Valid card=<n> values are:\n", dev->name);
	for (i = 0; i < mz0380_bcount; i++)
		pr_info("%s:    card=%d -> %s\n",
			dev->name, i, mz0380_boards[i].name);
}

int mz0380_card_setup(struct mz0380_dev *dev)
{
	int ret;

	mz0380_capture_state_init(dev);
	mz0380_sync_hw_input_select(dev, "probe");
	mz0380_sync_hw_quality(dev, "probe");
	mz0380_sync_hw_gop(dev, "probe");
	mz0380_sync_hw_bitrate(dev, "probe");
	mz0380_sync_hw_b_frames(dev, "probe");
	mz0380_sync_hw_qp_step(dev, "probe");
	mz0380_sync_hw_record_mode(dev, "probe");

	switch (dev->board) {
	case MZ0380_BOARD_ELGATO_HD60_PRO:
	case MZ0380_BOARD_ELGATO_HD60_PRO_REV3:
		break;
	default:
		return 0;
	}

	/*
	 * These boards have one physical capture connector: HDMI is V4L2 input
	 * zero.  BAR5 property 201 survives warm reloads, so merely inheriting its
	 * previous value can leave the VIC routed to an unrelated front end while
	 * the MST3367 is correctly receiving HDMI.  Select the route explicitly
	 * before releasing the receiver and raising HPD.
	 */
	ret = mz0380_request_input_select(dev, 0, "HD60 Pro bring-up");
	if (ret)
		pr_warn("%s: could not select the HDMI VIC route (%d)\n",
			dev->name, ret);

	/*
	 * Release the MST3367 from reset and apply its init now (non-fatal if the
	 * firmware is not ready yet). This wakes the HDMI input at probe; a later
	 * VIDIOC_QUERY_DV_TIMINGS re-runs it lazily if it was skipped here.
	 */
	mz0380_mst3367_bringup(dev);

	if (!mz0380_enable_video) {
		pr_info("%s: probe-safe V4L2 node disabled; load with enable_video=1 when you want /dev/video*\n",
			dev->name);
		return 0;
	}

	ret = mz0380_video_register(dev);
	if (ret)
		return ret;

	if (mz0380_enable_audio) {
		ret = mz0380_audio_register(dev);
		if (ret) {
			pr_warn("%s: audio register failed (%d) - continuing without ALSA\n",
				dev->name, ret);
		}
	}

	return 0;
}

void mz0380_card_cleanup(struct mz0380_dev *dev)
{
	mz0380_audio_unregister(dev);
	mz0380_video_unregister(dev);
}
