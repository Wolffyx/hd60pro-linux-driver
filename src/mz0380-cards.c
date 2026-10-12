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
	},
	[MZ0380_BOARD_ELGATO_HD60_PRO] = {
		.name = "Elgato Game Capture HD60 Pro",
	},
	[MZ0380_BOARD_ELGATO_HD60_PRO_REV3] = {
		.name = "Elgato Game Capture HD60 Pro (Rev.3)",
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
	unsigned int i;

	dev_warn(&dev->pci->dev,
		 "board %04x:%04x is not known to the driver; pass card=<n> to override autodetect:\n",
		 dev->pci->subsystem_vendor, dev->pci->subsystem_device);
	for (i = 0; i < mz0380_bcount; i++)
		dev_warn(&dev->pci->dev, "    card=%u -> %s\n",
			 i, mz0380_boards[i].name);
}

/*
 * These boards have one physical capture connector, and HDMI is VIC route 0:
 * BAR5 property 201 (reg 0x40, bits 2:0). The property survives warm reloads,
 * so inheriting its previous value can leave the VIC routed to a front end
 * this board does not have while the MST3367 is receiving HDMI correctly.
 * Select the route explicitly before the receiver is released and HPD rises.
 */
static void mz0380_select_hdmi_route(struct mz0380_dev *dev)
{
	u32 before = mz_cfg_read(dev, MZ0380_REG_PROP_INPUT);
	u32 want = before & ~MZ0380_PROP_INPUT_MASK;
	u32 readback;

	mz_cfg_write(dev, MZ0380_REG_PROP_INPUT, want);
	readback = mz_cfg_read(dev, MZ0380_REG_PROP_INPUT);
	if (readback != want)
		dev_warn(&dev->pci->dev,
			 "HDMI route select did not stick (BAR5[0x%02x] %08x -> %08x, read back %08x)\n",
			 MZ0380_REG_PROP_INPUT, before, want, readback);
}

int mz0380_card_setup(struct mz0380_dev *dev)
{
	int ret;

	mz0380_capture_state_init(dev);

	switch (dev->board) {
	case MZ0380_BOARD_ELGATO_HD60_PRO:
	case MZ0380_BOARD_ELGATO_HD60_PRO_REV3:
		break;
	default:
		return 0;
	}

	mz0380_select_hdmi_route(dev);

	/*
	 * Release the MST3367 from reset and apply its init now (non-fatal if
	 * the card is not ready yet: the first signal query retries it). This
	 * wakes the HDMI input at probe, so a source sees a sink straight away.
	 */
	mz0380_mst3367_bringup(dev);

	if (!mz0380_enable_video) {
		dev_info(&dev->pci->dev,
			 "V4L2 node disabled (enable_video=0)\n");
		return 0;
	}

	ret = mz0380_video_register(dev);
	if (ret)
		return ret;

	if (mz0380_enable_audio) {
		ret = mz0380_audio_register(dev);
		if (ret)
			dev_warn(&dev->pci->dev,
				 "audio register failed (%d) - continuing without ALSA\n",
				 ret);
	}

	return 0;
}

void mz0380_card_cleanup(struct mz0380_dev *dev)
{
	mz0380_audio_unregister(dev);
	mz0380_video_unregister(dev);
}
