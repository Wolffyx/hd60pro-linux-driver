// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Driver for MZ0380 based capture cards.
 *
 *  Card bring-up handshake. NO FIRMWARE IS EVER UPLOADED.
 *
 *  The Elgato HD60 Pro is a "smart" capture card: its YUAN MZ0380 PCIe
 *  endpoint boots its own embedded Linux from ONBOARD FLASH, on an ARMv5
 *  SoC, without any help from the host. That image carries the ARM binaries
 *  (tinyvenc, video_capture_mgr) plus the on-card kernel module ep.ko which
 *  serves the PCIe mailbox.
 *
 *  This driver deliberately does NOT push an image to the card. It is a
 *  standing project constraint, and it is also what the retail Windows
 *  driver does in practice - its DriverEntry logs
 *      TOTAL DOWNLOAD TIMES = 00000000 (6 BYTES) (SUCCESS) (1.11)
 *      BOARD VERSION: 1.11 / 1.11
 *  i.e. only the 6-byte version file is read, the versions match, and no
 *  image moves. The MZ0380.HD.HEX that used to ship with this tree was
 *  byte-identical to Elgato's shipping image (sha256 be0d5e19...), so there
 *  was never anything to gain by sending it - only ~21 s per insmod and a way
 *  to brick the card.
 *
 *  What this file does now:
 *    - read the expected version from the MZ0380.FW.TXT sidecar (there is
 *      no version-query opcode; this is where Windows gets it too),
 *    - handshake with the firmware already running on the card
 *      (mz0380_card_init: CMD_INIT + GET_BOARD_VERSION),
 *    - report any mismatch loudly and carry on with what is running.
 *
 *  Nothing here uploads, and no part of the upload protocol survives anywhere
 *  in the tree - not the opcodes, not the BAR0 aperture define, not the module
 *  parameter, not the .HEX blob names. Do not re-add any of it.
 */

#include <linux/ctype.h>
#include <linux/firmware.h>

#include "mz0380.h"

const char *mz0380_fw_state_name(enum mz0380_fw_state s)
{
	switch (s) {
	case MZ0380_FW_STATE_NONE:      return "none";
	case MZ0380_FW_STATE_REQUESTED: return "requested";
	case MZ0380_FW_STATE_READY:     return "ready";
	case MZ0380_FW_STATE_FAILED:    return "failed";
	}
	return "unknown";
}

/*
 * Parse the "MM.mm" ASCII sidecar version file (MZ0380.FW.TXT). This is where
 * the Windows driver gets the EXPECTED firmware version from - there is no
 * version-query opcode. It is the only file this driver reads, and it is read
 * for comparison only.
 */
static bool mz0380_fw_parse_version(const u8 *d, size_t len,
				    u32 *major, u32 *minor)
{
	if (len < 5 || !isdigit(d[0]) || !isdigit(d[1]) || d[2] != '.' ||
	    !isdigit(d[3]) || !isdigit(d[4]))
		return false;
	*major = (d[0] - '0') * 10 + (d[1] - '0');
	*minor = (d[3] - '0') * 10 + (d[4] - '0');
	return true;
}

/*
 * Bring the card up. NOT an upload - see the file header. The card is already
 * running its own flash image by the time we probe; all we do is shake hands
 * with it, learn which version that is, and say so.
 */
int mz0380_firmware_load(struct mz0380_dev *dev)
{
	u32 want_major = 0, want_minor = 0;
	const struct firmware *vf;
	bool have_want = false;
	int ret;

	mutex_lock(&dev->fw_lock);

	if (dev->fw_state == MZ0380_FW_STATE_READY) {
		mutex_unlock(&dev->fw_lock);
		return 0;
	}

	dev->fw_state = MZ0380_FW_STATE_REQUESTED;

	/*
	 * Expected version, from the ASCII sidecar. Optional: its only job is
	 * to make a mismatch visible. There is no version-query opcode, so
	 * this file is also where the Windows driver gets the number it prints
	 * as "FIRMWARE VERSION".
	 */
	if (!firmware_request_nowarn(&vf, "mz0380/MZ0380.FW.TXT",
				     &dev->pci->dev)) {
		have_want = mz0380_fw_parse_version(vf->data, vf->size,
						    &want_major, &want_minor);
		release_firmware(vf);
	}

	/* Handshake with whatever the card booted: CMD_INIT + board version. */
	ret = mz0380_card_init(dev);
	if (ret) {
		pr_err("%s: card handshake failed (%d) - the mailbox is deaf. The card boots from its own flash; nothing here uploads firmware, so this is a card or link problem, not a missing image.\n",
		       dev->name, ret);
		dev->fw_state = MZ0380_FW_STATE_FAILED;
		mutex_unlock(&dev->fw_lock);
		return ret;
	}

	if (have_want && (dev->fw_version_major != want_major ||
			  dev->fw_version_minor != want_minor))
		pr_warn("%s: card runs firmware %u.%u but %s expects %u.%u - continuing with what the card has (this driver never uploads)\n",
			dev->name, dev->fw_version_major,
			dev->fw_version_minor, "MZ0380.FW.TXT",
			want_major, want_minor);

	dev->fw_state = MZ0380_FW_STATE_READY;
	mutex_unlock(&dev->fw_lock);
	return 0;
}

void mz0380_firmware_release(struct mz0380_dev *dev)
{
	mutex_lock(&dev->fw_lock);
	dev->fw_state = MZ0380_FW_STATE_NONE;
	mutex_unlock(&dev->fw_lock);
}
