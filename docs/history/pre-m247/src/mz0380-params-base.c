// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 base, diagnostics, DMA, and stream module parameters.
 *
 * M245: permissions say when a parameter takes effect. 0444 means load time
 * only: anything that decides what is allocated at probe, which IOVAs the card
 * is told to write to, the pipeline's lifetime, or which format the node
 * advertises. Those were writable through sysfs, where changing one after the
 * buffers existed made the card target addresses computed with a different
 * offset than the one they were mapped at. 0644 remains for values that are
 * re-read at the next stream start or are purely diagnostic.
 */

#include "mz0380-internal.h"

unsigned int procfs_verbosity = 1;
module_param(procfs_verbosity, int, 0644);
MODULE_PARM_DESC(procfs_verbosity,
	"/proc/mz0380-state detail (def:1). 2 adds the PCI config dump. 3 adds the chip-0x90 bridge probe, which costs FIVE mailbox commands per read - do not poll /proc at that level while capturing (M236). Wide BAR scans are disabled as unsafe");

unsigned int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "enable debug messages");


unsigned int snapshot_profile = 1;
module_param(snapshot_profile, int, 0644);
MODULE_PARM_DESC(snapshot_profile,
		 "targeted register snapshot profile: 1=known-safe baseline, 2=baseline plus experimental BAR5 low-offset window, 3=profiles 1-2 plus experimental BAR0 signal window, 4=profiles 1-3 plus extended BAR5 window, 5=profiles 1-4 plus second extended BAR5 window, 6=profiles 1-5 plus M0 mailbox/ring/firmware BAR5 windows (0x00c0..0x00e4, 0x0200..0x0234, 0x0300..0x0410)");

unsigned int scan_bar = MZ0380_MAP_BAR_MMIO;
module_param(scan_bar, uint, 0644);
MODULE_PARM_DESC(scan_bar,
		 "/proc/mz0380-scan target BAR: 0=BAR0/MMIO (default, HDMI signal window), 1=BAR5/CFG");

unsigned int scan_start;
module_param(scan_start, uint, 0644);
MODULE_PARM_DESC(scan_start,
		 "/proc/mz0380-scan first byte offset (4-byte aligned); default 0x0000");

unsigned int scan_len = 0x48;
module_param(scan_len, uint, 0644);
MODULE_PARM_DESC(scan_len,
		 "/proc/mz0380-scan window length in bytes; default 0x48 covers the sc0710-style HDMI status regs (0x00a8..0x00e4)");

bool scan_unsafe;
module_param(scan_unsafe, bool, 0644);
MODULE_PARM_DESC(scan_unsafe,
		 "/proc/mz0380-scan: read ALL offsets in the window, not just proven-safe BAR0 ranges. DANGER: an un-backed post-boot BAR0 offset stalls the CPU on readl until the PCIe completion timeout (looks like a hard hang). Each read is logged to dmesg first so a stall's culprit offset is recoverable. Default false");

unsigned int periph_chip = MZ0380_CHIP_BRIDGE;
module_param(periph_chip, uint, 0644);
MODULE_PARM_DESC(periph_chip,
		 "/proc/mz0380-periph-scan target chip id: 0x90=bridge/FPGA reg file (default, HDMI front-end), 0xb8=TVP5160 analog");

unsigned int periph_start;
module_param(periph_start, uint, 0644);
MODULE_PARM_DESC(periph_start,
		 "/proc/mz0380-periph-scan first peripheral register index; default 0x00");

unsigned int periph_count = 0x40;
module_param(periph_count, uint, 0644);
MODULE_PARM_DESC(periph_count,
		 "/proc/mz0380-periph-scan number of registers to read via REG_READ (0x1a); default 0x40");

bool periph_probe;
module_param(periph_probe, bool, 0644);
MODULE_PARM_DESC(periph_probe,
		 "/proc/mz0380-periph-scan diagnostic: for the first few registers, issue REG_READ and dump EVERY mailbox return slot (STATUS/EVENT/PARAM/payload) so the slot that actually carries the read-back value can be located. Default false");

unsigned int event_sample_us = 200;
module_param(event_sample_us, uint, 0644);
MODULE_PARM_DESC(event_sample_us,
		 "/proc/mz0380-events watcher: EVENT-word poll interval in microseconds; default 200");

bool event_auto_ack = true;
module_param(event_auto_ack, bool, 0644);
MODULE_PARM_DESC(event_auto_ack,
		 "/proc/mz0380-events watcher: ack each recorded event (rearm the card for the next edge). Turn off to capture a single sticky event without acking. Default true");

/*
 * M35. store_channel_done writes the token words (BAR0+0x40/44/48/4c, and the
 * encoder status at 0x50) UNGATED, while EVENT[0x30]+MSI sit behind the
 * one-shot msi_enable credit that only our ack doorbell re-arms. So the token
 * words are the honest witness of card-side frame completion: the watcher
 * thread logs every change it sees in them (only meaningful while the watcher
 * runs - "echo start > /proc/mz0380-events").
 *
 * credit_kick_ms > 0 additionally fires the full ack sequence (BAR5[0xdc]=2,
 * EVENT=0, doorbell 0x400 -> card pciep_isr_clrint -> msi_enable=1) on that
 * period, re-arming the credit even when no MSI ever arrives. If frame tokens
 * only start moving (or MSIs appear) once this is on, the blocker was a dead
 * credit, not a parked encoder.
 */
unsigned int credit_kick_ms;
module_param(credit_kick_ms, uint, 0644);
MODULE_PARM_DESC(credit_kick_ms,
		 "M35 watcher: period in ms for unconditional ack/credit-re-arm kicks (BAR5[0xdc]=2, EVENT=0, doorbell 0x400); 0 = off (default). Live-tunable via /sys/module/mz0380/parameters/");

bool allow_experimental_writes;
module_param(allow_experimental_writes, bool, 0644);
MODULE_PARM_DESC(allow_experimental_writes,
		 "allow tightly scoped BAR5 control experiments; disabled by default");

unsigned int input_select_reg = ~0U;
module_param(input_select_reg, uint, 0644);
MODULE_PARM_DESC(input_select_reg,
		 "BAR5 candidate register offset for SDK property 201 input-select experiments; 0xffffffff disables hardware writes");

unsigned int input_select_mask = 0x7;
module_param(input_select_mask, uint, 0644);
MODULE_PARM_DESC(input_select_mask,
		 "bit mask for the property-201 input-select candidate field");

unsigned int input_select_shift;
module_param(input_select_shift, uint, 0644);
MODULE_PARM_DESC(input_select_shift,
		 "bit shift for the property-201 input-select candidate field");

unsigned int bitrate_reg = ~0U;
module_param(bitrate_reg, uint, 0644);
MODULE_PARM_DESC(bitrate_reg,
		 "BAR5 candidate register offset for SDK property 403 bitrate experiments; 0xffffffff disables hardware writes");

unsigned int bitrate_mask = ~0U;
module_param(bitrate_mask, uint, 0644);
MODULE_PARM_DESC(bitrate_mask,
		 "bit mask for the property-403 bitrate candidate field");

unsigned int bitrate_shift;
module_param(bitrate_shift, uint, 0644);
MODULE_PARM_DESC(bitrate_shift,
		 "bit shift for the property-403 bitrate candidate field");

unsigned int quality_reg = ~0U;
module_param(quality_reg, uint, 0644);
MODULE_PARM_DESC(quality_reg,
		 "BAR5 candidate register offset for SDK property 404 quality experiments; 0xffffffff disables hardware writes");

unsigned int quality_mask = ~0U;
module_param(quality_mask, uint, 0644);
MODULE_PARM_DESC(quality_mask,
		 "bit mask for the property-404 quality candidate field");

unsigned int quality_shift;
module_param(quality_shift, uint, 0644);
MODULE_PARM_DESC(quality_shift,
		 "bit shift for the property-404 quality candidate field");

unsigned int gop_reg = ~0U;
module_param(gop_reg, uint, 0644);
MODULE_PARM_DESC(gop_reg,
		 "BAR5 candidate register offset for SDK property 405 GOP experiments; 0xffffffff disables hardware writes");

unsigned int gop_mask = ~0U;
module_param(gop_mask, uint, 0644);
MODULE_PARM_DESC(gop_mask,
		 "bit mask for the property-405 GOP candidate field");

unsigned int gop_shift;
module_param(gop_shift, uint, 0644);
MODULE_PARM_DESC(gop_shift,
		 "bit shift for the property-405 GOP candidate field");

unsigned int b_frames_reg = ~0U;
module_param(b_frames_reg, uint, 0644);
MODULE_PARM_DESC(b_frames_reg,
		 "BAR5 candidate register offset for SDK property 411 B-frame experiments; 0xffffffff disables candidate writes");

unsigned int b_frames_mask = ~0U;
module_param(b_frames_mask, uint, 0644);
MODULE_PARM_DESC(b_frames_mask,
		 "bit mask for the property-411 B-frame candidate field");

unsigned int b_frames_shift;
module_param(b_frames_shift, uint, 0644);
MODULE_PARM_DESC(b_frames_shift,
		 "bit shift for the property-411 B-frame candidate field");

unsigned int qp_step_reg = ~0U;
module_param(qp_step_reg, uint, 0644);
MODULE_PARM_DESC(qp_step_reg,
		 "BAR5 candidate register offset for SDK property 408 QP-step experiments; 0xffffffff disables candidate writes");

unsigned int qp_step_mask = ~0U;
module_param(qp_step_mask, uint, 0644);
MODULE_PARM_DESC(qp_step_mask,
		 "bit mask for the property-408 QP-step candidate field");

unsigned int qp_step_shift;
module_param(qp_step_shift, uint, 0644);
MODULE_PARM_DESC(qp_step_shift,
		 "bit shift for the property-408 QP-step candidate field");

unsigned int record_mode_reg = ~0U;
module_param(record_mode_reg, uint, 0644);
MODULE_PARM_DESC(record_mode_reg,
		 "BAR5 candidate register offset for SDK property 407 record-mode experiments; 0xffffffff disables hardware writes");

unsigned int record_mode_mask = 0x3;
module_param(record_mode_mask, uint, 0644);
MODULE_PARM_DESC(record_mode_mask,
		 "bit mask for the property-407 record-mode candidate field");

unsigned int record_mode_shift;
module_param(record_mode_shift, uint, 0644);
MODULE_PARM_DESC(record_mode_shift,
		 "bit shift for the property-407 record-mode candidate field");

/*
 * M166: the shipping default is now the known-good capture configuration.
 *
 * This was `bool mz0380_enable_video;` - off - since bring-up, and every successful
 * capture in this project's history passed it explicitly. Left off, `insmod
 * ./mz0380.ko` produces a device that cannot capture, and the user sees either
 * no /dev/video* at all or a node that never delivers. That is not a safe
 * default, it is a broken one.
 *
 * Pass enable_video=0 to get the old bring-up behaviour back.
 */
bool mz0380_enable_video = true;
module_param_named(enable_video, mz0380_enable_video, bool, 0444);
MODULE_PARM_DESC(enable_video,
		 "register the V4L2 node (def:1 since M166 - the shipping default is the known-good capture configuration; 0 restores the bring-up behaviour, no /dev/video*)");


/*
 * Bring-up gates. All default OFF so existing probe-safe behaviour
 * survives. Flip them on incrementally as each phase is verified.
 *
 *   enable_dma=1        alloc rings, enable bus master, request MSI
 *   enable_audio=1      register an ALSA snd_card alongside V4L2
 *
 * There is no firmware_upload parameter and there must never be one again:
 * the card boots its own flash image and mz0380-fw.c only handshakes with it.
 * The old inert compatibility no-op was removed along with the scripts that
 * passed it.
 */
/*
 * GPIO pin 8, the companion reset/power strap. mz0380_mst3367_reset() has
 * always finished by driving it LOW and leaving it there.
 *
 * M109: pin 9 next door is the MST3367 reset and is documented ACTIVE-LOW - we
 * release it to 1. If pin 8 follows the same convention then driving it 0 holds
 * the companion device in reset for the entire session, which is a complete
 * explanation for the card's HDMI passthrough output being dead - no picture at
 * all on a monitor plugged into the card, not even a no-signal message.
 *
 * Nobody chose 0 deliberately; it came from the original bring-up sequence and
 * has never been varied. 0 and 1 are both real levels, so MZ0380_RX_STRAP_LEAVE
 * is the third value meaning "do not drive pin 8 at all".
 */
/*
 * M111: deliver frames the card has already written, without waiting for a
 * completion event that has never once fired.
 *
 * The real capture path delivers only from the MSI-driven drain. Every run this
 * project has ever made reports frame_events=0 and EVENT[0x30]=0 - the card
 * DMAs a complete 1920x1080 4:2:0 frame into buf0 (760 contiguous pages ending
 * at 0x2f7000 == 1920*1080*3/2) and then nothing tells the host it is there, so
 * vb2 is never fed and userspace reads 0 bytes. The nosg path was written as a
 * poller for exactly this reason (M41); the real path never got the same
 * treatment.
 *
 * Nonzero starts a kthread that scans the stream buffers on this interval and
 * hands any completed frame to the ORIGINAL drain path
 * (mz0380_drain_frame_snapshot), which infers the length from the poison
 * suffix, copies into vb2 and re-poisons - so a frame is delivered once and
 * only once, and the event-driven path stays byte-for-byte unchanged.
 *
 * Default 0 (off) so it bisects cleanly against every earlier result.
 */
/*
 * M118: fire the card's credit re-arm after a poll-drained frame.
 *
 * The completion path's ISR ends with BAR5[0xdc]=2, EVENT=0, doorbell 0x400 -
 * which drives the card's pciep_isr_clrint and sets msi_enable=1 again. That
 * one-shot credit is what lets the card raise the NEXT completion. It has never
 * run in this project, because it lives in an interrupt handler for an event
 * that never fires.
 *
 * The poll-drain acks enc_stat (M117) but that changed nothing, so the slot
 * handshake is not the gate. This is the other half of what a real completion
 * would have done, and it is the last piece of the ISR the poll path does not
 * reproduce.
 */
/*
 * M119: re-notify the on-card encoder, once per frame.
 *
 * ep.ko's op 0x06 handler (@0x1854, M22) does ONE thing: sysfs_notify() on
 * /sys/vpl_pciep/epint. tinyvenc5 blocks on that node, wakes, DMAs a frame, and
 * blocks again. So one op6 buys exactly one frame - which is precisely the
 * cadence measured all session, and why the nosg path respawns the encoder per
 * frame (M39) and burns the card's spawn budget doing it.
 *
 * op6 is fire-and-forget (no mailbox completion, M22) and does NOT fork an
 * encoder - SET_VIC does that. Re-sending it is therefore cheap and costs no
 * spawn budget. Every host-side ack has now been eliminated as the cadence gate
 * (op8/wency_ready M93+M116, enc_stat M117, completion credit M118); this is the
 * card-side wake-up those acks were standing in for.
 */
unsigned int mz0380_op6_kick_ms;
module_param_named(op6_kick_ms, mz0380_op6_kick_ms, uint, 0644);
MODULE_PARM_DESC(op6_kick_ms,
		 "M120: after each poll-drained frame, re-fire START_STREAMING (op 0x06) to re-notify /sys/vpl_pciep/epint and ask for the next one - tinyvenc5 delivers one frame per notify. Value is the MINIMUM spacing in ms, not a period; kicks never fire before the first frame (M119 fired free-running from stream start and got zero frames plus a lost lock) (def:0 = off; try 16)");

/*
 * M126: WHICH opcode the post-frame kick sends. M125 decoded ep.ko's
 * pciep_isr dispatcher:
 *
 *   op 0x06        notifies audio_ctrl, THEN epint / epint_1080p
 *   op 0x09, 0x2f  notify the same epint node, WITHOUT the audio_ctrl notify
 *
 * tinyvenc5 blocks on /sys/vpl_pciep/epint and delivers one frame per notify.
 * op 0x06 is "start", and re-sending start per frame drags the audio control
 * path along with it; 0x2f is the bare wake-up and is the natural per-frame
 * kick. The plumbing already exists - op6_kick_ms fires after each delivered
 * frame (M120) - so only this opcode changes.
 *
 * Default stays 0x06 so op6_kick_ms means exactly what it did before.
 */
unsigned int mz0380_kick_opcode = 0x06;
module_param_named(kick_opcode, mz0380_kick_opcode, uint, 0644);
MODULE_PARM_DESC(kick_opcode,
		 "M126: opcode fired by op6_kick_ms after each delivered frame - 0x06 (def, START, also notifies audio_ctrl) or 0x2f / 0x09 (bare epint wake-up)");

/*
 * M139: sentinel written into BAR0 0x40/0x44/0x48/0x4c just before
 * START_STREAMING so the producer watch can tell "the card reported one frame
 * from buffer 1" (nibble goes to 0, upper bits survive) from "the card never
 * reported at all" (sentinel intact).  Nothing card-side reads these
 * registers; store_channel_done() only read-modify-writes one nibble per
 * channel into them.  Set 0 to leave them alone.
 */
/*
 * M139: SET_VIC bytes 8..9 / 10..11 overrides.
 *
 * These are the bytes the card's cfg patcher (video_capture_mgr 0xa290)
 * rewrites every 1920- and 1080-valued cfg line with, which is where
 * EncodingGroup::Start reads m_vic_width from - and m_vic_width is the ONE
 * value libtkmf_video_source's img_handler compares the VIC's own measured
 * width against before it will publish a frame (M138).  A mismatch drops
 * every frame silently, which is exactly the state M139 measured.
 *
 * They have never been settable independently of the v4l2 capture geometry.
 * The point of splitting them out is the 8-bit double-rate reading: BANK0
 * 0xb0 = 0x21 is embedded-sync 8-bit BT1120, so a 1920-wide picture crosses
 * the bus as 3840 8-bit samples, and the receiver's own detect flaps between
 * hper=674 and hper=337 - a factor of two.  If the VIC measures 3840, 1920
 * can never match.
 *
 * Pair 3840 with height 540 to keep w*h*3/2 at 3110400 bytes: the cfg patcher
 * rewrites both, so the frame stays exactly the size of the existing 4 MiB
 * buffer and mz0380_infer_frame_length()'s hardcoded want. Changing width
 * alone would make the card write 6220800 bytes into a 4 MiB buffer, which is
 * the overrun that caused every IOMMU fault from M26 to M29.
 */
unsigned int mz0380_vic_out_w;
module_param_named(vic_out_w, mz0380_vic_out_w, uint, 0644);
MODULE_PARM_DESC(vic_out_w,
		 "M139: SET_VIC bytes 8..9 override - the width img_handler's frame gate compares the VIC's measurement against. 0 = use the v4l2 capture width (def:0). Pair 3840 with vic_out_h=540 to keep the frame 3110400 bytes.");

unsigned int mz0380_vic_out_h;
module_param_named(vic_out_h, mz0380_vic_out_h, uint, 0644);
MODULE_PARM_DESC(vic_out_h,
		 "M139: SET_VIC bytes 10..11 override. 0 = use the v4l2 capture height (def:0)");

/*
 * M148: SET_VIC byte 28, bitstream_num. Hardcoded to 1 since M22 and never
 * varied - the only thing ever established about it is that the firmware
 * rejects 0 ("MUST be >=1"), so 1 was chosen and frozen.
 *
 * tinyvenc5's EncodingGroup::encode_handler tests a per-channel field at
 * [chan+0x34] immediately after its first successful SSM_ReleaseAndReceive
 * (0x12f4c: cmp #1 / bls 0x13d04) and takes a different path when the value is
 * <= 1. Our streams report "bitstreams=1", the encoder has never produced a
 * bitstream (enc[0x50]=0, channel_done never written), and the one frame that
 * does reach the host arrives raw over the MMA path rather than as H.264 -
 * which is what that alternate path would look like.
 *
 * The identification of [chan+0x34] as this field is INFERENCE, not proof.
 * Default stays 1 so nothing changes unless the knob is set.
 */
/*
 * M151: whether to clear the card's enc_stat word (BAR0+0x50) after each
 * delivered frame.
 *
 * M40 added this on the theory that the card's encoder "produces exactly one
 * bitstream and then skips every subsequent frame" without the ack, and it has
 * been unconditional ever since - the one write the driver still makes to the
 * card on every delivered frame.
 *
 * M150 undermines the theory it rests on. The completion record the ack is
 * supposed to be answering is written by a pwrite in tinyvenc5's
 * encode_handler that sits inside a block guarded by
 * EncodingGroup::mma_already_start, a flag with nine reads and zero writes in
 * the whole binary. That path cannot execute, so the card never reports a
 * completed frame to anybody - and an ack for a report that never happens is
 * at best inert and at worst a mid-stream write to a register whose meaning we
 * inferred from a protocol that does not run.
 *
 * Default is 1, i.e. the behaviour every prior measurement was taken with.
 * Set 0 to remove the write and leave the card alone after a frame.
 */
/*
 * M155: send SET_VIC only on the FIRST stream cycle after insmod.
 *
 * M154 got two distinct frames from one module load by running two v4l2
 * captures - but the dmesg shows every STREAMON re-sending SET_VIC, and
 * SET_VIC is what makes video_capture_mgr fork a fresh tinyvenc5. So a cycle
 * costs an encoder spawn out of the 8-18 budget plus a ~2 s settle, and
 * "cycle for cadence" is not viable.
 *
 * The question this answers: does the frame come from the RESPAWN, or would a
 * cycle without SET_VIC also produce one? M153 says the one-frame bound is a
 * per-process latch (mma_already_start, never set, so
 * TK_MMA_StartOneFrame runs once and is never awaited), which predicts that a
 * cycle without a respawn yields nothing - the parked tinyvenc5 has already
 * spent its single push.
 *
 * A confirmed negative closes "cycle cheaply for cadence" for good. A positive
 * would mean the frame does not depend on the respawn, and cycling becomes a
 * real - if slow - video path.
 *
 * Default 0 = every stream start sends SET_VIC, which is what every prior
 * result used.
 */
/*
 * M156: whether streamoff sends STOP_STREAMING (op 0x07) to the card.
 *
 * M155 found that with SET_VIC suppressed, every later cycle's op 0x2d times
 * out (ret=-110) - nothing on the card answers the mailbox any more. The
 * obvious suspect is our own STOP: video_capture_mgr contains two
 * system("killall -9 tinyvenc5") sites (0x8d1c and 0x95c4), and if either sits
 * in the 0x07 handler then our streamoff is what removes the encoder.
 *
 * Proving which handler owns them means more hand-tracing of vcm, which has
 * already produced one wrong result today (M150/M152). Testing is cheaper and
 * decisive: with stop_on_streamoff=0 and setvic_once=1, a second cycle whose
 * 0x2d returns 0 proves tinyvenc5 survived, i.e. STOP was killing it.
 *
 * Safety: skipping STOP leaves the card free to keep writing the stream
 * buffers after streamoff. They stay allocated for the life of the module, and
 * mz0380_dma_teardown() clears bus mastering BEFORE freeing them, so DMA is
 * disarmed at the PCI level on unload either way.
 *
 * Default 1 = send STOP, the behaviour every prior result used.
 */
bool mz0380_stop_on_streamoff = true;
module_param_named(stop_on_streamoff, mz0380_stop_on_streamoff, bool, 0644);
MODULE_PARM_DESC(stop_on_streamoff,
		 "M156: send STOP_STREAMING (op 0x07) at streamoff (def:1 = every prior result; 0 = leave the card streaming so a later cycle can find tinyvenc5 alive)");

bool mz0380_setvic_once;
module_param_named(setvic_once, mz0380_setvic_once, bool, 0644);
MODULE_PARM_DESC(setvic_once,
		 "M155: send SET_VIC only on the first stream cycle after insmod, so later cycles do not respawn tinyvenc5 (def:0 = send every cycle, the behaviour all prior results used)");

/*
 * The real no-power-cycle lifecycle.  SET_VIC owns a scarce card-side process,
 * whereas STREAMOFF only means the current userspace consumer went away. Keep
 * the process, DMA mappings, completion FIFO and enc_stat ACK path alive until
 * module removal; a later STREAMON attaches without sending any card command.
 */
bool mz0380_persistent_h264 = true;
module_param_named(persistent_h264, mz0380_persistent_h264, bool, 0444);
MODULE_PARM_DESC(persistent_h264,
		 "keep one H.264 VIC/encoder pipeline alive across V4L2 STREAMOFF/STREAMON (def:1; H264PROBE path only)");

bool mz0380_enc_stat_ack_on = true;
module_param_named(enc_stat_ack, mz0380_enc_stat_ack_on, bool, 0644);
MODULE_PARM_DESC(enc_stat_ack,
		 "M151: clear enc_stat (BAR0+0x50) after each delivered frame (def:1 = the M40 behaviour all prior results used; 0 = do not write the card after a frame)");

unsigned int mz0380_bitstream_num = 1;
module_param_named(bitstream_num, mz0380_bitstream_num, uint, 0644);
MODULE_PARM_DESC(bitstream_num,
		 "M148: SET_VIC byte 28. Firmware requires >=1; hardcoded to 1 since M22 and never swept (def:1)");

unsigned int mz0380_token_seed = 0xa5a5a5a5;
module_param_named(token_seed, mz0380_token_seed, uint, 0644);
MODULE_PARM_DESC(token_seed,
		 "M139: sentinel seeded into the BAR0 frame-token registers before START so the producer watch can distinguish 'reported buffer 1' from 'never reported' (0 = do not seed, def:0xa5a5a5a5)");

/*
 * M126: with the kick confined to the post-delivery branch, a one-frame stream
 * fires exactly one kick - which is what the first 0x2f run measured, and it
 * cannot distinguish "the card ignored the wake-up" from "we only asked once".
 * With this set, the kick repeats every op6_kick_ms once at least one frame has
 * been delivered. Still never before the first frame: that is the M119/M120
 * race that cost a lock and a run.
 */
bool mz0380_kick_repeat;
module_param_named(kick_repeat, mz0380_kick_repeat, bool, 0644);
MODULE_PARM_DESC(kick_repeat,
		 "M126: after the FIRST delivered frame, repeat the kick every op6_kick_ms even with no new delivery (def:0)");

bool mz0380_poll_drain_credit;
module_param_named(poll_drain_credit, mz0380_poll_drain_credit, bool, 0644);
MODULE_PARM_DESC(poll_drain_credit,
		 "M118: after a poll-drained frame, fire the credit re-arm the completion ISR would have (BAR5[0xdc]=2, EVENT=0, doorbell 0x400) (def:0)");

/*
 * M166: on by default, because with the shipping `fw=5` it is the ONLY
 * delivery path there is.
 *
 * Under fw=5 the card never raises a frame-completion interrupt - `frame_events`
 * has been 0 in every run in this project's history, and M166 explains why: the
 * push that carries the pixels (tinyvenc5 0x14728, a3 = w*h*3/2 = 3110400) has
 * no reachable completion handler behind it. So the card writes a whole frame
 * into the buffer and never tells us.
 *
 * With this at 0 a plain `insmod` therefore delivers **nothing at all** - no
 * events, no polling, no frames - which is indistinguishable from broken
 * hardware to anyone opening /dev/video0. Every real frame this project has
 * captured came from a script passing POLLDRAIN=20 by hand.
 *
 * 20 ms is what every successful capture since M111 has used.
 */
unsigned int mz0380_poll_drain_ms;	/* M216: 0, the fw=7 default */
module_param_named(poll_drain_ms, mz0380_poll_drain_ms, uint, 0444);
MODULE_PARM_DESC(poll_drain_ms,
		 "M111: poll the stream buffers every N ms and deliver any frame the card has already written, instead of waiting for a completion event that never arrives. DEFAULT 0 since M216: the fw=7 encoded path uses the window-1 completion ring and does not need polling. Set 20 only for the legacy fw=5 raw poll-drain path, which delivers nothing without it (M166)");

/*
 * M168: end the stream instead of hanging on it.
 *
 * Under the shipping fw=5 the card is a single-shot grabber and that is a
 * proven bound, not a missing trick (M166): tinyvenc5 pushes one frame from
 * TK_MMA_StartOneFrame, then MassMemAccess_StartDMAC spins forever in an
 * unbounded sched_yield loop because the only call that would return the
 * profile - TK_MMA_WaitOneFrameComplete - is unreachable in this build (M153).
 * No host-side write reaches any link in that chain.
 *
 * Until now the driver did nothing about it, so an application that kept
 * reading - ffmpeg, OBS, anything that streams rather than grabs - blocked in
 * DQBUF forever after receiving its one correct frame. A hang is the worst way
 * to report a bound: it looks like broken hardware or a broken driver, and it
 * is neither.
 *
 * So once a frame HAS been delivered and the requested interval has passed with
 * nothing following it, mark the queue errored. DQBUF then returns -EIO and
 * poll() reports EPOLLERR, which is how V4L2 says "this source has stopped" -
 * applications finish their file and exit with the frame they got. The gate is
 * deliberately after the first delivery: the first frame lands ~1.2 s after
 * stream start behind start_delay_ms, and erroring before it would break every
 * normal capture.
 *
 * Nothing here respawns the encoder. A respawn does yield one more frame (M39),
 * but each one costs part of the 8-18 spawn budget, so faking a video stream
 * that way would wedge the card within seconds of an OBS session.
 *
 * 0 restores the old behaviour: block in DQBUF indefinitely.
 */
unsigned int mz0380_stall_eos_ms = 2000;
module_param_named(stall_eos_ms, mz0380_stall_eos_ms, uint, 0644);
MODULE_PARM_DESC(stall_eos_ms,
		 "M168: after a frame has been delivered, if this many ms pass with no further frame, error the vb2 queue so DQBUF returns -EIO instead of blocking forever - fw=5 is a single-shot grabber (def:2000; 0 = block forever, the pre-M168 behaviour)");

/*
 * M174: refuse to stream when the detected source geometry and the negotiated
 * buffer geometry disagree.
 *
 * They have always been two separate numbers - mz0380_queue_setup() sizes the
 * vb2 plane from capture.width/height, the drain measures and delivers
 * source_width*source_height*3/2 - and nothing checked they matched. On a 1080p
 * source they are the same value, which is every source this project has had.
 * On a 720p source the card writes 1382400 bytes into a buffer the application
 * was told is 1920x1080, nothing errors, and it renders garbage.
 *
 * With this on, that becomes -EPIPE plus a log line naming both geometries and
 * the S_FMT that fixes it, and a source-change event so a client that
 * subscribed can re-negotiate by itself.
 *
 * Set to 0 if a detection wobble ever refuses a capture that would have
 * worked - the old behaviour is to stream anyway and let the frame be
 * mislabelled.
 */
bool mz0380_strict_geometry = true;
module_param_named(strict_geometry, mz0380_strict_geometry, bool, 0644);
MODULE_PARM_DESC(strict_geometry,
		 "M174: refuse STREAMON when the detected source geometry differs from the negotiated format, instead of delivering a frame that does not match the format the application was given (def:1; 0 = the pre-M174 behaviour)");

/*
 * M175: what a COMPLETE frame is, when the card is not writing source-sized
 * frames.
 *
 * The completeness rule (M115, and M160 on the event path) is "a frame is done
 * when the buffer holds source_width * source_height * 3/2 bytes". That is
 * right for tinyvenc5, which writes a 1080p frame for a 1080p source, and it
 * was written when tinyvenc5 was the only producer anyone had run.
 *
 * tinyvenc8 writes 777600 bytes - a complete 960x540 I420 frame, exactly a
 * quarter of the source. The rule cannot tell that from a 1080p frame that is
 * three-quarters written, so it waited forever and the first fw=8 run scored
 * as "nothing delivered" while the driver's own log said, sixty times a second,
 * that buf 0 held 777600 of 3110400 bytes.
 *
 * This says what to expect instead. 0 keeps deriving it from the source, which
 * is every result before M175.
 */
unsigned int mz0380_expect_frame_bytes;
module_param_named(expect_frame_bytes, mz0380_expect_frame_bytes, uint, 0644);
MODULE_PARM_DESC(expect_frame_bytes,
		 "M175: bytes that constitute a COMPLETE frame, when the card writes a geometry other than the source's (tinyvenc8 writes 777600 = 960x540 I420). 0 = derive it from the detected source, which is correct for fw=5 (def:0)");

unsigned int mz0380_rx_strap;
module_param_named(rx_strap, mz0380_rx_strap, uint, 0644);
MODULE_PARM_DESC(rx_strap,
		 "M109: GPIO8 companion reset/power strap level after the receiver reset (def:0, the level this driver has always driven; 1 = release; 0xffffffff = do not drive it at all)");

/*
 * M166: the shipping default is now the known-good capture configuration.
 *
 * This was `bool mz0380_enable_dma;` - off - since bring-up, and every successful
 * capture in this project's history passed it explicitly. Left off, `insmod
 * ./mz0380.ko` produces a device that cannot capture, and the user sees either
 * no /dev/video* at all or a node that never delivers. That is not a safe
 * default, it is a broken one.
 *
 * Pass enable_dma=0 to get the old bring-up behaviour back.
 */
bool mz0380_enable_dma = true;
module_param_named(enable_dma, mz0380_enable_dma, bool, 0444);
MODULE_PARM_DESC(enable_dma,
		 "allocate ring buffers, request MSI, enable bus mastering (def:1 since M166; 0 restores the bring-up behaviour, which cannot capture)");

/*
 * Gap between SET_VIC(0x29) and START_STREAMING(op 0x06). SET_VIC makes the
 * card's video_capture_mgr system()-fork tinyvenc5; op6 only kicks that
 * encoder once it has exec'd, opened /sys/vpl_pciep/epint and consumed the
 * SET_VIC command first (M22). Fire op6 too early and it either misses
 * tinyvenc5's poll or clobbers the SET_VIC it expects on its first read - the
 * encoder then sits idle (symptom: IRQ 164 stuck at 3, 0-byte capture).
 * Writable at runtime so the spawn window can be swept without a reload.
 */
unsigned int mz0380_start_delay_ms = 2000;
module_param_named(start_delay_ms, mz0380_start_delay_ms, uint, 0644);
MODULE_PARM_DESC(start_delay_ms,
		 "ms to wait after SET_VIC before firing START_STREAMING(op 0x06) so the card can fork+exec tinyvenc5 and read SET_VIC first (def:2000; too short = encoder idle at 3 IRQs)");

/*
 * Diagnostic bisection lever. SET_VIC byte[31]=is_nosg: when set, the card's
 * tinyvenc encoder spawns fake_frame_process, a black/color test-pattern
 * generator that pwrites channel_done on a timer with NO capture/SSM/BT1120
 * dependency (RE_FINDINGS.md M23). stream_nosg=1 => frames flowing proves the
 * whole START -> channel_done -> MSI -> outbound-ATU -> host-DMA path; frames
 * still absent then points downstream (channels[] host target unset). Off by
 * default so real capture is used. Writable at runtime.
 */
bool mz0380_stream_nosg;
module_param_named(stream_nosg, mz0380_stream_nosg, bool, 0444);
MODULE_PARM_DESC(stream_nosg,
		 "SET_VIC is_nosg flag: 1 = force the card's fake-frame (test-pattern) generator, bypassing real BT1120 capture; diagnostic bisection lever (def:0)");

/*
 * How long the stream_nosg polling capture waits for the raw frame's
 * contiguous burst to finish landing in buf0 before it gives up on this
 * spawn and respawns. Hardware lands the whole 0x30a5c0-byte frame within
 * 450 ms of START (M36), so 3 s is generous slack, not a tuned value.
 */
unsigned int mz0380_nosg_frame_timeout_ms = 3000;
module_param_named(nosg_frame_timeout_ms, mz0380_nosg_frame_timeout_ms,
		   uint, 0644);
MODULE_PARM_DESC(nosg_frame_timeout_ms,
		 "stream_nosg: ms to wait for the raw fake frame to land in buf0 before respawning the encoder (def:3000; hw lands in ~450)");

/*
 * M25 probe. op2 hands the card 8-byte channels[] slots; pcie_set_outbound
 * (ep.ko 0x5a8) then does ELBI[0x58] = slot word0, ELBI[0x54] = slot word1.
 * Default packing is {word0=high32, word1=low32}, which is what the firmware
 * intends (0x58=high, 0x54=low). M24 showed the resulting DMA target is
 * (word0 << 32) + aperture_offset with the low half always 0, i.e. word1 never
 * reaches the hardware. Setting this swaps the pair to {word0=low32,
 * word1=high32} so the fault address tells us which reading is true:
 *   fault at (low32 << 32)  -> only word0 lands; ELBI 0x54 is ignored by the
 *                              HW (the real outbound low target is elsewhere,
 *                              likely 0x84) -> take the 4GB-aligned-IOVA route.
 *   fault at the correct 64-bit address, or frames land -> both words land and
 *                              the earlier low32 loss was ours.
 * Diagnostic only; leave off for normal operation. Writable at runtime.
 */
bool mz0380_buf_pair_swap;
module_param_named(buf_pair_swap, mz0380_buf_pair_swap, bool, 0444);
MODULE_PARM_DESC(buf_pair_swap,
		 "M25 probe: swap the op2 channels[] slot word order to {low32, high32} (def:0 = {high32, low32})");

/*
 * M26. The M25 probe proved the card's outbound window takes only the HIGH
 * 32 bits of the host target: host_addr == (slot word0 << 32) + aperture
 * offset, with the low half (ELBI 0x54) dropped on the floor by the hardware.
 * The only way to make that address land in one of our buffers is to put the
 * buffer at an IOVA whose low 32 bits are zero, i.e. 4 GiB-aligned. So we
 * allocate the stream buffers as plain pages and iommu_map() each one at
 * dma_iova_base + (i << 32), then hand the card word0 = IOVA >> 32, word1 = 0.
 *
 * Requires a translating IOMMU domain (AMD-Vi/Intel VT-d in DMA mode - not
 * iommu=pt / iommu=off). Base must be 4 GiB-aligned; it is chosen low and far
 * from where the DMA-API's IOVA allocator hands out addresses (near the top of
 * the domain aperture), and every mapping is checked for a collision first.
 */
bool mz0380_dma_iova_remap = true;
module_param_named(dma_iova_remap, mz0380_dma_iova_remap, bool, 0444);
MODULE_PARM_DESC(dma_iova_remap,
		 "M26: place each stream buffer at its own 4GiB-aligned IOVA via iommu_map, so the card's high32-only outbound target lands in it (def:1)");

unsigned long long mz0380_dma_iova_base = 0x100000000ULL;
module_param_named(dma_iova_base, mz0380_dma_iova_base, ullong, 0444);
MODULE_PARM_DESC(dma_iova_base,
		 "M26: base IOVA for the remapped stream buffers, must be 4GiB-aligned; buffer i lands at base + (i << 32) (def:0x100000000)");

/*
 * M27. With the 4GiB-aligned IOVA in place the card's writes landed at
 * (word0 << 32) + 0x80000 - one SET_BUF stride past the window base, i.e.
 * host_addr = (word0 << 32) + bufindex * stride, with the card starting at
 * bufindex 2. Two runtime knobs to pin that down without a rebuild:
 *   set_buf_stride  - the stride word in SET_BUF (cmd[0x8]). If the offset is
 *                     bufindex*stride, 0 here should drag the writes down onto
 *                     the buffer base.
 *   dma_iova_offset - shifts every mapping up by this much, so the buffer sits
 *                     exactly where the card writes instead. Set it to the
 *                     observed offset (0x80000) if the stride knob does not
 *                     move the writes.
 * Exactly one of the two should be needed. Both are diagnostics until the hw
 * says which; the winner becomes the default.
 */
unsigned long long mz0380_dma_iova_offset;
module_param_named(dma_iova_offset, mz0380_dma_iova_offset, ullong, 0444);
MODULE_PARM_DESC(dma_iova_offset,
		 "M27: shift each remapped stream buffer up by this many bytes within its 4GiB slot, to meet the card's aperture offset (def:0)");

unsigned int mz0380_set_buf_stride = MZ0380_STREAM_BUF_STRIDE;
module_param_named(set_buf_stride, mz0380_set_buf_stride, uint, 0444);
MODULE_PARM_DESC(set_buf_stride,
		 "M27: the stride word sent in SET_BUF cmd[0x8]; the card appears to place buffer n at aperture offset n*stride (def:0x80000)");

/*
 * M29. Final model, and every hw data point fits it:
 *
 *     host_addr = ((word0 << 32) | word1) + aperture_offset
 *
 * where aperture_offset starts at 0 and the low half adds MOD 2^32 (no carry
 * into word0). The "0x80000 offset" chased in M27/M28 was not an offset at
 * all: it was the card running off the END of a 512 KiB buffer. Writes inside
 * the mapping succeed silently, so the first fault appears one buffer-size in,
 * which looked exactly like a constant offset. M28 advertised base while
 * mapping base+0x80000, left the advertised address unmapped, and faulted at
 * offset 0 - which is what exposed the mistake.
 *
 * So: advertise the buffer base as-is (offset 0), and make the buffers big
 * enough for whatever the card streams. This knob stays as a runtime escape
 * hatch, defaulting to 0.
 */
unsigned int mz0380_card_frame_offset;
module_param_named(card_frame_offset, mz0380_card_frame_offset, uint, 0444);
MODULE_PARM_DESC(card_frame_offset,
		 "M29: bytes the card adds to the advertised target before writing a frame; buffers are mapped this far above the address sent in SET_BUF. Measured 0 on hw (def:0)");

/*
 * M32. A raw frame lands in window0 buf0, but the card never signals
 * channel_done. ep.ko's delivery path is not the gate: command-done and
 * frame-done share one re-arm token (msi.constprop.1 / store_channel_done both
 * bail when it is 0, and pciep_isr_clrint sets it), and our command MSIs prove
 * that token cycles correctly. So the card's userspace is not declaring a
 * finished frame - most likely the encoder's bitstream destination is one of
 * the outbound windows we have never programmed (op2 only fills window0).
 * This sends the other windows' buffer-setter opcodes too, pointed at the three
 * buffers the card is currently ignoring, to find which window wakes up.
 */
/*
 * M92: Windows never sends 0x02 without 0x08 right behind it (0x14027b62d
 * then 0x14027b752, same channel, same size, four more address pairs).
 * ep.ko: op2 fills window0 slots 1..4 and clears host_ready; op8 fills
 * slots 5..8 and sets wency_ready = 8.
 */
/*
 * M94: write the receiver's output stage the way the Windows driver does -
 * 0xb0/0xae/0xad/0xb1/0xb2/0xb3/0xb4 as one block, not 0xb0 alone. See
 * mst3367_commit_digital_output(). Windows sends 0xb0 = 0x14, so pair this
 * with vic_b0=0x14.
 */
bool mz0380_mst_win_output;
module_param_named(mst_win_output, mz0380_mst_win_output, bool, 0644);
MODULE_PARM_DESC(mst_win_output,
		 "M94: write the full Windows MST3367 output-stage block (def:0; pair with vic_b0=0x14)");

/*
 * M94: BANK0 0xad. Windows computes it as (context_flag > 0) & 5, i.e. 0 or 1,
 * from a field we cannot identify from the host. Exposed so both can be tried.
 */
unsigned int mz0380_mst_ad;
module_param_named(mst_ad, mz0380_mst_ad, uint, 0644);
MODULE_PARM_DESC(mst_ad, "M94: MST3367 BANK0 0xad value - Windows sends 0 or 1 (def:0)");

bool mz0380_set_buf_op8;
module_param_named(set_buf_op8, mz0380_set_buf_op8, bool, 0444);
MODULE_PARM_DESC(set_buf_op8,
		 "M92: also send SET_BUF op 0x08 after op 0x02, as Windows always does (def:0)");

/*
 * M209: the old op8 knob aliases the first four addresses.  Windows instead
 * owns eight buffers: op2 slots 1..4 and four independent op8 slots 5..8,
 * each advertised as 0x466000 bytes.  This is now an isolated raw-only V4L2
 * discriminator: H.264 window 1/op04 is deliberately forbidden.
 */
bool mz0380_raw_bank_probe;
module_param_named(raw_bank_probe, mz0380_raw_bank_probe, bool, 0444);
MODULE_PARM_DESC(raw_bank_probe,
		 "M209: raw-only Windows-exact independent op02/op08 banks (requires h264_probe=0 + IOVA remap; forbids op04; def:0)");

/*
 * M210: the M209 hardware run answered its own question and produced nothing.
 * The full raw-only sequence ran - one SET_VIC, both banks registered, op06
 * fired - and the card never wrote a frame token: irq_total=8, frame_events=0,
 * the seeded BAR0 0x40..0x4c sentinel intact, all eight slots still poisoned.
 * The producer never started, so op06 alone does not start it.
 *
 * Raw-only differs from the working 60 fps H.264 start in exactly two ways: no
 * op04 sink, and no encoder tail.  A missing sink cannot suppress generation
 * into the sinks that do exist, which leaves the tail.  M82 showed the card
 * routes 0x2d/0x31 to the same epint wake as 0x06, so the wake is not the
 * point - SET_ENC_PARAMS/SET_PREVIEW_PARAMS configure the pipeline that
 * produces frames at all, and the raw planes are plausibly its by-product.
 *
 * This knob restores that tail on top of the M209 sink topology, byte for byte
 * as the H.264 path sends it: the Windows-exact encoder values, both streams,
 * SET_PREVIEW_PARAMS, and no op06 (Windows does not send it when the tail is
 * present).  It changes exactly one variable against M209.
 */
bool mz0380_raw_probe_enc_tail;
module_param_named(raw_probe_enc_tail, mz0380_raw_probe_enc_tail, bool, 0444);
MODULE_PARM_DESC(raw_probe_enc_tail,
		 "M210: with raw_bank_probe, send the Windows encoder tail (SET_ENC_PARAMS x2 + SET_PREVIEW_PARAMS) and suppress op06 (def:0)");

/*
 * The bounded raw discriminators pin themselves to the retail 1080p60
 * configuration because the 0x2f7600 oracle must not be scored against a
 * different byte count.  A 1080p30 source does not change that byte count:
 * the frame extent is pure geometry (1920 * 1080 * 3 / 2), and `fw` selects
 * the chroma layout rather than the refresh - M209 established that six means
 * planar 4:2:2 (S/2 per plane) and every other value, seven included, means
 * planar 4:2:0 (S/4 per plane).  Only cadence differs, and the run is bounded
 * by completions rather than by wall-clock frames.
 *
 * So 1080p30 is a legitimate host for the same experiment, but it is still a
 * deviation from the Windows-confirmed configuration and must never be entered
 * silently: a run scored at 30 Hz has to be readable as such afterwards.  This
 * knob is that opt-in, and taking it logs a warning into the same dmesg the
 * run is scored from.
 */
bool mz0380_raw_probe_allow_30;
module_param_named(raw_probe_allow_30, mz0380_raw_probe_allow_30, bool, 0444);
MODULE_PARM_DESC(raw_probe_allow_30,
		 "Permit the bounded raw discriminators to run on a progressive 1080p30 source; a documented deviation from the Windows-confirmed 1080p60 (def:0)");

/*
 * M211: the last structural difference between the raw-only start and the one
 * that works.
 *
 * M209 showed op 0x06 alone leaves the producer dead; M210 restored the whole
 * Windows encoder tail and it stayed dead - SET_VIC, both SET_ENC_PARAMS,
 * SET_PREVIEW_PARAMS, and still events=0 with all eight slots poison-intact.
 * The only thing those runs still lacked was the op 0x04 encoded window.
 *
 * The earlier reasoning that a missing *sink* cannot stop generation is
 * therefore wrong for this firmware: tinyvenc appears to validate its complete
 * output set before it starts anything. So the next question is not "does the
 * raw ring work on its own" but "does the raw ring fill while the encoded path
 * is running", which is also exactly what Windows does - it registers op02,
 * op08 AND op04 on every start.
 *
 * This mode is deliberately passive. It rides the known-good h264_probe path,
 * which delivers 60 fps today, and changes nothing about V4L2 negotiation,
 * completion routing or delivery: the eight raw buffers are registered through
 * op02/op08, poisoned, and read back at stop. If they show writes, the raw
 * surface exists and is a by-product of the encoded pipeline. If they are
 * still pristine after a live capture, the op02/op08 ring is not the Linux
 * raw source at all and the search moves back to the Windows binary.
 */
bool mz0380_raw_bank_observe;
module_param_named(raw_bank_observe, mz0380_raw_bank_observe, bool, 0444);
MODULE_PARM_DESC(raw_bank_observe,
		 "M211: register and poison the op02/op08 raw banks alongside a live H.264 capture and report their extents at stop (requires h264_probe=1 + IOVA remap; def:0) LOAD-TIME ONLY: the banks are allocated in mz0380_dma_setup() at probe, so setting this through sysfs on a loaded module makes stream start fail with -ENODEV");
