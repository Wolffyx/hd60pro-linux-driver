// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 mailbox transport, card-ready handshake, and peripheral proxy.
 */

#include "mz0380.h"


/*
 * Mailbox command send.
 *
 * Protocol confirmed against both the Windows transport and ep.ko:
 *   1. clear the shared PARAM10/STATUS word before installing payload
 *   2. write opcode to PARAM0 and up to ten arguments to PARAM1..PARAM10
 *   3. ring BAR0+0x00 with 0x800
 *   4. unless explicitly asynchronous, wait for STATUS or EVENT completion;
 *      a full-width command can wait only for EVENT because STATUS is payload
 *
 * The public helper serialises transactions with dev->cmd_lock. Peripheral
 * reads use the internal locked form so the lock also covers their late result.
 */
/*
 * Ack/rearm one card event, exact write order of the Windows event
 * thread (FUN_140284380): BAR5 flag first, then the BAR0 event word,
 * then the 0x400 doorbell. Also deasserts INTx.
 */
/*
 * M4 diagnostic: empirically locate the post-boot mailbox layout.
 *
 * ep.ko (card side) allocates a fresh dma_coherent(0x60) command buffer on boot
 * and maps host BAR0 to it via the inbound window based at dma_handle+4, so the
 * post-boot opcode/STATUS offsets differ from the bootloader's (which we use).
 * Rather than guess the offset, sweep candidate (opcode_off, doorbell_off)
 * layouts: for each, issue GET_BOARD_VERSION and see whether any word in the
 * 0x60 mailbox region changes away from the 0xdddddddd poison / our own writes.
 * Read-modify only within the 0x60 mailbox aperture - safe.
 */
void mz0380_mailbox_scan(struct mz0380_dev *dev)
{
	static const u32 op_off[]   = { 0x00, 0x04 };
	static const u32 bell_off[] = { 0x00, 0x04 };
	unsigned int oi, bi, w;

	pr_info("%s: mailbox scan: baseline region dump:\n", dev->name);
	for (w = 0; w < 0x60; w += 4)
		pr_info("%s:   base[0x%02x] = %08x\n", dev->name, w,
			mz_mmio_read(dev, w));

	for (oi = 0; oi < ARRAY_SIZE(op_off); oi++) {
		for (bi = 0; bi < ARRAY_SIZE(bell_off); bi++) {
			u32 o = op_off[oi], b = bell_off[bi];
			bool changed = false;

			/* clear the region we own, without touching the poison
			 * word until after, so we can spot a real card write */
			for (w = 0; w < 0x60; w += 4)
				if (w != MZ0380_MB_STATUS)
					mz_mmio_write(dev, w, 0);
			/* opcode GET_BOARD_VERSION at candidate offset */
			mz_mmio_write(dev, o, MZ0380_CMD_GET_BOARD_VERSION);
			wmb();
			mz_mmio_write(dev, b, MZ0380_MB_FIRE);
			msleep(50);

			for (w = 0; w < 0x60; w += 4) {
				u32 v = mz_mmio_read(dev, w);

				if (v != 0 && v != 0xdddddddd &&
				    v != MZ0380_CMD_GET_BOARD_VERSION) {
					pr_info("%s: scan op@0x%02x bell@0x%02x: base[0x%02x]=%08x (RESPONSE?)\n",
						dev->name, o, b, w, v);
					changed = true;
				}
			}
			if (!changed)
				pr_info("%s: scan op@0x%02x bell@0x%02x: no change\n",
					dev->name, o, b);
			mz0380_mb_ack_event(dev);
		}
	}
	pr_info("%s: mailbox scan done\n", dev->name);
}

static void mz0380_mb_snapshot_reply(struct mz0380_dev *dev)
{
	unsigned int i;

	BUILD_BUG_ON(MZ0380_MB_COMMAND_WORDS > MZ0380_REG_PARAM_MAX);
	memset(dev->cmd_last_param, 0, sizeof(dev->cmd_last_param));
	for (i = 0; i < MZ0380_MB_COMMAND_WORDS; i++)
		dev->cmd_last_param[i] =
			mz_mmio_read(dev, MZ0380_MB_PARAM(i));
}

void mz0380_mb_ack_event(struct mz0380_dev *dev)
{
	unsigned long flags;
	u32 event;

	/*
	 * ACK re-arms the endpoint and permits it to overwrite TOKEN/PAYLOAD.
	 * Capture their frame-bearing state first, regardless of whether the ACK
	 * came from the MSI ISR, command poller, boot waiter, or stale-event drain.
	 * The poller and ISR can observe the same one-shot EVENT concurrently, so
	 * serialize the complete snapshot/rearm sequence.  Otherwise one context
	 * could clear and rearm the mailbox while the other is still reading the
	 * old event's payload.
	 */
	spin_lock_irqsave(&dev->event_lock, flags);
	event = mz_mmio_read(dev, MZ0380_MB_EVENT);
	if (event && event != U32_MAX) {
		/*
		 * Keep the command reply paired with the same EVENT as the frame
		 * payload.  Reading these in the ISR before taking event_lock lets a
		 * polling CPU ACK/rearm between EVENT and the reply snapshot.
		 */
		if (event & MZ0380_MB_EVENT_CMD_DONE) {
			dev->cmd_last_status =
				mz_mmio_read(dev, MZ0380_MB_STATUS);
			mz0380_mb_snapshot_reply(dev);
			smp_store_release(&dev->cmd_complete, true);
			wake_up_all(&dev->cmd_wait);
		}
		mz0380_handle_event_snapshot(dev, event);
	}
	mz_cfg_write(dev, MZ0380_CFG_INT_FLAG, MZ0380_CFG_INT_ACK_VAL);
	mz_mmio_write(dev, MZ0380_MB_EVENT, 0);
	wmb();
	mz_mmio_write(dev, MZ0380_MB_DOORBELL, MZ0380_MB_INT_ACK);
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

/*
 * M118: the credit re-arm, lifted out of the ISR so the poll-drain can fire the
 * same sequence. The card's completion channel is a one-shot: msi_enable is
 * consumed when it posts an event and only this doorbell restores it. On the
 * poll path no event ever posts, so nothing has ever re-armed it.
 */
void mz0380_credit_rearm(struct mz0380_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	mz_cfg_write(dev, MZ0380_CFG_INT_FLAG, MZ0380_CFG_INT_ACK_VAL);
	mz_mmio_write(dev, MZ0380_MB_EVENT, 0);
	wmb();
	mz_mmio_write(dev, MZ0380_MB_DOORBELL, MZ0380_MB_INT_ACK);
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

/* dev->cmd_lock must remain held until any opcode-specific late reply is read. */
static int mz0380_send_command_locked(struct mz0380_dev *dev, u32 opcode,
				       const u32 *params,
				       unsigned int nparams,
				       u32 *status_out,
				       unsigned int timeout_ms)
{
	unsigned int i;
	u32 status = 0;
	bool ack_slot_is_payload;
	int ret = 0;

	/*
	 * RE-confirmed mailbox model (BAR0). SEND_COMMAND writes the opcode to
	 * the PARAM0 slot (BAR0+0x04), the arguments to the following slots,
	 * then fires the doorbell (BAR0+0x00 = 0x800). BAR0+0x2c is both PARAM10
	 * and STATUS; Windows sends commands which use all ten arguments through
	 * its EVENT-wait path, which skips only the overlapping STATUS poll.
	 * PARAM11 would be EVENT and must never be written as payload.
	 */
	BUILD_BUG_ON(MZ0380_MB_PARAM(MZ0380_MB_MAX_ARGS) !=
		     MZ0380_MB_STATUS);
	BUILD_BUG_ON(MZ0380_MB_PARAM(MZ0380_MB_MAX_ARGS + 1) !=
		     MZ0380_MB_EVENT);

	lockdep_assert_held(&dev->cmd_lock);
	if (nparams > MZ0380_MB_MAX_ARGS || (nparams && !params))
		return -EINVAL;
	ack_slot_is_payload = nparams == MZ0380_MB_MAX_ARGS;
	if (status_out)
		*status_out = 0;

	/*
	 * Drain a stale unacked event (e.g. from a previous module life) before
	 * assigning a new command to cmd_complete. mz0380_mb_ack_event() takes
	 * the frame/token snapshot before it clears EVENT, so this cannot discard
	 * a late frame completion.
	 */
	{
		u32 stale = mz_mmio_read(dev, MZ0380_MB_EVENT);

		WRITE_ONCE(dev->cmd_complete, false);
		if ((stale && stale != U32_MAX) ||
		    mz_cfg_read(dev, MZ0380_CFG_INT_FLAG) == 1) {
			mz0380_mb_ack_event(dev);
			dev_dbg(&dev->pci->dev,
				"drained stale EVENT=0x%08x before command 0x%x\n",
				stale, opcode);
		}
		/* Retire an ISR which observed the old EVENT before the drain. */
		if (dev->irq_requested)
			synchronize_irq(dev->irq);
	}

	/*
	 * Clear the completion latch before any command words are installed.
	 * In particular, do not clear it after PARAM10: for a full-width command
	 * that same write would destroy its final payload word. The firmware's
	 * 0xaaaaaaaa/0xdddddddd values are replies/initial state, not immutable
	 * stamps; retaining an old one would falsely complete the next command.
	 */
	mz_mmio_write(dev, MZ0380_MB_STATUS, 0);
	WRITE_ONCE(dev->cmd_complete, false);
	smp_wmb();

	/* opcode -> PARAM0 (BAR0+0x04), args -> PARAM1..PARAM10 */
	mz_mmio_write(dev, MZ0380_MB_OPCODE, opcode);
	for (i = 0; i < nparams; i++)
		mz_mmio_write(dev, MZ0380_MB_PARAM(i + 1), params[i]);

	wmb();
	mz_mmio_write(dev, MZ0380_MB_DOORBELL, MZ0380_MB_FIRE);

	/*
	 * timeout_ms == 0 is the only fire-and-forget form. A command occupying
	 * PARAM10 cannot poll that shared word, but a non-zero timeout still waits
	 * for EVENT CMD_DONE, matching Windows' semaphore path for SET_BUF.
	 */
	if (!timeout_ms)
		return 0;

	{
		/*
		 * Short commands poll STATUS even when MSI is enabled: they can finish
		 * through bit0 without an interrupt. Full-width commands skip STATUS
		 * because it is PARAM10 and wait exclusively for EVENT CMD_DONE.
		 * dev->cmd_complete covers an event consumed first by the ISR; this
		 * loop also consumes live events so it works without IRQ delivery.
		 * Ack only a real event—the old ack-every-tick scheme could abort a
		 * STATUS-completing command.
		 */
		unsigned int waited = 0;
		unsigned int max_wait = max(timeout_ms,
					    (unsigned int)MZ0380_MB_POLL_ITERS);
		bool done = false;
		u32 event;

		do {
			if (smp_load_acquire(&dev->cmd_complete)) {
				if (!ack_slot_is_payload)
					status = dev->cmd_last_status;
				done = true;
				break;
			}
			if (!ack_slot_is_payload)
				status = mz_mmio_read(dev, MZ0380_MB_STATUS);
			/*
			 * Completion = bit0, or the firmware's 0xaaaaaaaa
			 * success stamp (GET_BOARD_VERSION/INIT). The
			 * 0xdddddddd boot stamp also has bit0 set but is NOT a
			 * completion.
			 */
			if (!ack_slot_is_payload &&
			    (status == MZ0380_MB_STATUS_OK_STAMP ||
			     ((status & MZ0380_MB_STATUS_DONE) &&
			      status != MZ0380_MB_STATUS_BOOT_STAMP))) {
				done = true;
				break;
			}
			event = mz_mmio_read(dev, MZ0380_MB_EVENT);
			if (event && event != U32_MAX) {
				bool cmd_done = event & MZ0380_MB_EVENT_CMD_DONE;

				/* Snapshot frame-bearing lanes before the ACK clears EVENT. */
				mz0380_mb_ack_event(dev);
				/* M245: every command posts one; dynamic debug. */
				dev_dbg(&dev->pci->dev,
					"EVENT=0x%08x during command 0x%x (%s), acked\n",
					event, opcode,
					cmd_done ? "cmd-done" : "other");
				if (cmd_done) {
					done = true;
					break;
				}
			}
			usleep_range(900, 1100);
			waited += 1;
		} while (waited < max_wait);

		if (!done)
			ret = -ETIMEDOUT;

		/*
		 * A command that completed via STATUS bit0 may still post a
		 * trailing completion event a moment later. Give it a few ms and
		 * snapshot every frame-bearing event before ACKing; a frame-only
		 * event does not end this short trailing-command-event window.
		 */
		if (done) {
			for (waited = 0; waited < 10; waited++) {
				bool cmd_done;

				event = mz_mmio_read(dev, MZ0380_MB_EVENT);
				cmd_done = event && event != U32_MAX &&
					   (event & MZ0380_MB_EVENT_CMD_DONE);
				if (event && event != U32_MAX) {
					mz0380_mb_ack_event(dev);
					if (cmd_done)
						break;
				} else if (mz_cfg_read(dev,
						       MZ0380_CFG_INT_FLAG) == 1) {
					/* No EVENT payload is live; only rearm INTx. */
					mz0380_mb_ack_event(dev);
					break;
				}
				usleep_range(300, 500);
			}
		}

		/*
		 * Preserve a reply already paired with CMD_DONE by
		 * mz0380_mb_ack_event().  Once that ACK rearms the endpoint, a live
		 * reread is no longer tied to the event we just consumed.  STATUS-only
		 * completions have no such snapshot, so take one while excluding the
		 * ISR's EVENT snapshot/ACK sequence.
		 */
		{
			unsigned long flags;

			spin_lock_irqsave(&dev->event_lock, flags);
			if (!smp_load_acquire(&dev->cmd_complete))
				mz0380_mb_snapshot_reply(dev);
			spin_unlock_irqrestore(&dev->event_lock, flags);
		}
	}

	if (status_out)
		*status_out = status;
	return ret;
}

/* Copy shared reply storage before cmd_lock permits another transaction. */
int mz0380_send_command_reply(struct mz0380_dev *dev, u32 opcode,
			       const u32 *params, unsigned int nparams,
			       u32 *status_out, unsigned int timeout_ms,
			       u32 *reply, unsigned int reply_words)
{
	int ret;

	if (reply_words > MZ0380_MB_COMMAND_WORDS ||
	    (reply_words && !reply))
		return -EINVAL;
	if (status_out)
		*status_out = 0;
	if (reply_words)
		memset(reply, 0, reply_words * sizeof(*reply));
	if (nparams > MZ0380_MB_MAX_ARGS || (nparams && !params))
		return -EINVAL;

	mutex_lock(&dev->cmd_lock);
	ret = mz0380_send_command_locked(dev, opcode, params, nparams,
					 status_out, timeout_ms);
	if (reply_words) {
		if (timeout_ms && nparams != MZ0380_MB_MAX_ARGS)
			memcpy(reply, dev->cmd_last_param,
			       reply_words * sizeof(*reply));
	}
	mutex_unlock(&dev->cmd_lock);
	return ret;
}

int mz0380_send_command(struct mz0380_dev *dev, u32 opcode,
			const u32 *params, unsigned int nparams,
			u32 *status_out, unsigned int timeout_ms)
{
	int ret;

	if (nparams > MZ0380_MB_MAX_ARGS || (nparams && !params))
		return -EINVAL;

	mutex_lock(&dev->cmd_lock);
	ret = mz0380_send_command_locked(dev, opcode, params, nparams,
					 status_out, timeout_ms);
	mutex_unlock(&dev->cmd_lock);
	return ret;
}

/*
 * Post-boot handshake (Windows FUN_140278bb0): program the BAR5 notify
 * pointers with the physical BAR0 mailbox addresses, ack, then send
 * CMD_INIT until the firmware answers. Follow up with GET_BOARD_VERSION
 * (success stamp 0xaaaaaaaa) whose result in PARAM 0x08/0x0c is the
 * RUNNING firmware version. The mailbox ignores most opcodes until this
 * dance is done.
 */
int mz0380_card_init(struct mz0380_dev *dev)
{
	/*
	 * M245: the card is told where BAR0 is as the PCIe BUS sees it, which
	 * is what the card DMAs to - not the CPU physical address, which differs
	 * behind a translating host bridge.
	 */
	pci_bus_addr_t bar0 = pci_bus_address(dev->pci, 0);
	unsigned int attempt;
	unsigned long started = jiffies;
	unsigned long deadline = started +
		msecs_to_jiffies(mz0380_card_ready_timeout_ms);
	u32 init_reply[2] = { 0 };
	u32 status = 0;
	int ret = -ETIMEDOUT;

	/*
	 * The notify pointers are 32 bits wide. A firmware that places BAR0
	 * above 4 GiB (Above-4G decoding, resizable BAR) leaves the card no way
	 * to reach its own mailbox, and the handshake below then times out with
	 * nothing to say why - so say why.
	 */
	if (upper_32_bits(bar0))
		dev_err(&dev->pci->dev,
			"BAR0 is at bus address 0x%llx, above 4 GiB; the card's notify pointers are 32-bit, so the handshake cannot work. Disable Above-4G decoding / resizable BAR for this slot, or move the card\n",
			(unsigned long long)bar0);

	mz_cfg_write(dev, MZ0380_CFG_NOTIFY_PTR0, lower_32_bits(bar0) + 0x04);
	mz_cfg_write(dev, MZ0380_CFG_NOTIFY_PTR1, lower_32_bits(bar0) + 0x5f);
	wmb();
	mz0380_mb_ack_event(dev);

	for (attempt = 0;; attempt++) {
		ret = mz0380_send_command_reply(
			dev, MZ0380_CMD_INIT, NULL, 0, &status, 600,
			init_reply, ARRAY_SIZE(init_reply));
		if (!ret || !mz0380_card_ready_timeout_ms ||
		    time_after_eq(jiffies, deadline))
			break;
		msleep(100);
	}
	if (ret) {
		pr_warn("%s: CMD_INIT got no answer after %u attempts/%ums (%d), STATUS=%08x EVENT=%08x RESULT=%08x bar5[dc]=%08x bar5[30]=%08x bar5[38]=%08x\n",
			dev->name, attempt + 1,
			jiffies_to_msecs(jiffies - started), ret,
			status,
			mz_mmio_read(dev, MZ0380_MB_EVENT),
			init_reply[1],
			mz_cfg_read(dev, MZ0380_CFG_INT_FLAG),
			mz_cfg_read(dev, MZ0380_CFG_NOTIFY_PTR0),
			mz_cfg_read(dev, MZ0380_CFG_NOTIFY_PTR1));
		if (mz0380_dma_handshake)
			mz0380_mailbox_scan(dev);
		return ret;
	}
	pr_info("%s: CMD_INIT answered on attempt %u after %ums (status=0x%08x)\n",
		dev->name, attempt + 1,
		jiffies_to_msecs(jiffies - started), status);

	{
		u32 params[2] = { 0, 0 };

		/*
		 * The version words can settle after command completion. Keep the
		 * mailbox transaction locked through that delay and extraction so a
		 * concurrent GPIO/I2C command cannot replace PARAM1/PARAM2.
		 */
		mutex_lock(&dev->cmd_lock);
		ret = -ETIMEDOUT;
		for (attempt = 0; attempt < 10 && ret; attempt++)
			ret = mz0380_send_command_locked(
				dev, MZ0380_CMD_GET_BOARD_VERSION, params, 2,
				&status, 5000);
		if (!ret) {
			msleep(100);
			dev->fw_version_major =
				mz_mmio_read(dev, MZ0380_MB_PARAM(1));
			dev->fw_version_minor =
				mz_mmio_read(dev, MZ0380_MB_PARAM(2));
			dev->cmd_last_param[1] = dev->fw_version_major;
			dev->cmd_last_param[2] = dev->fw_version_minor;
		}
		mutex_unlock(&dev->cmd_lock);
	}
	if (ret) {
		pr_warn("%s: GET_BOARD_VERSION got no answer (%d)\n",
			dev->name, ret);
		return ret;
	}

	pr_info("%s: board reports running firmware %u.%u (status=0x%08x)\n",
		dev->name, dev->fw_version_major, dev->fw_version_minor,
		status);
	return 0;
}

/*
 * Peripheral register file access via mailbox opcodes 0x1a/0x1b
 * (Windows FUN_1402777e4 / FUN_1402851cc). Read results land in the
 * PARAM3 slot (BAR0+0x10). Requires booted firmware.
 */
/*
 * The card's userspace I2C proxy (yuan_ioctrl) writes the read result into the
 * PARAM3 slot (BAR0+0x10) *after* it posts command completion, so the snapshot
 * send_command() takes at the completion edge can still hold the previous
 * command's result (off-by-one on back-to-back reads). The firmware writes only
 * the low BYTE of that slot, so we pre-load a distinctive sentinel byte and
 * poll the low byte until the firmware replaces it (NAK -> 0x00, ACK -> value).
 * A real register may itself equal one sentinel.  Retry once with its inverse;
 * no byte can equal both values, so a legitimate 0xa5/0x5a reply is not
 * misreported as a timeout.
 */
#define MZ0380_PERIPH_READ_SENTINEL0	0xa5u
#define MZ0380_PERIPH_READ_SENTINEL1	0x5au
#define MZ0380_PERIPH_READ_POLL_US	500
#define MZ0380_PERIPH_READ_POLL_ITERS	60	/* ~30 ms budget for the result */

int mz0380_periph_read(struct mz0380_dev *dev, u8 chip, u8 reg, u32 *val)
{
	static const u8 sentinels[] = {
		MZ0380_PERIPH_READ_SENTINEL0,
		MZ0380_PERIPH_READ_SENTINEL1,
	};
	u32 params[3] = { chip, reg, 0 };
	u32 result = 0;
	unsigned int attempt, i;
	int ret = -ETIMEDOUT;

	/*
	 * Keep cmd_lock across both command completion and the proxy's late
	 * low-byte store. Otherwise a second mailbox command may replace PARAM3
	 * between send_command() unlocking and this result poll.
	 */
	mutex_lock(&dev->cmd_lock);
	for (attempt = 0; attempt < ARRAY_SIZE(sentinels); attempt++) {
		params[2] = sentinels[attempt];
		ret = mz0380_send_command_locked(dev, MZ0380_CMD_REG_READ,
						 params, ARRAY_SIZE(params),
						 NULL, 1000);
		if (ret) {
			pr_info("%s: REG_READ chip=0x%02x reg=0x%02x failed (%d), STATUS=%08x EVENT=%08x RESULT=%08x P3=%08x\n",
				dev->name, chip, reg, ret,
				mz_mmio_read(dev, MZ0380_MB_STATUS),
				mz_mmio_read(dev, MZ0380_MB_EVENT),
				mz_mmio_read(dev, MZ0380_MB_RESULT),
				mz_mmio_read(dev, MZ0380_MB_PARAM(3)));
			goto out_unlock;
		}

		for (i = 0; i < MZ0380_PERIPH_READ_POLL_ITERS; i++) {
			result = mz_mmio_read(dev, MZ0380_MB_PARAM(3));
			if ((result & 0xff) != sentinels[attempt]) {
				ret = 0;
				goto have_result;
			}
			usleep_range(MZ0380_PERIPH_READ_POLL_US,
				     MZ0380_PERIPH_READ_POLL_US * 2);
		}
	}

	ret = -ETIMEDOUT;
	pr_warn("%s: REG_READ chip=0x%02x reg=0x%02x result timed out with both sentinels after %u polls each (P3=%08x STATUS=%08x EVENT=%08x)\n",
		dev->name, chip, reg, MZ0380_PERIPH_READ_POLL_ITERS,
		result, mz_mmio_read(dev, MZ0380_MB_STATUS),
		mz_mmio_read(dev, MZ0380_MB_EVENT));
	goto out_unlock;

have_result:
	dev->cmd_last_param[3] = result;
	if (val)
		*val = result & 0xff;

out_unlock:
	mutex_unlock(&dev->cmd_lock);
	return ret;
}

int mz0380_periph_write(struct mz0380_dev *dev, u8 chip, u8 reg, u32 val)
{
	u32 params[3] = { chip, reg, val };

	return mz0380_send_command(dev, MZ0380_CMD_REG_WRITE, params, 3,
				   NULL, 1000);
}
