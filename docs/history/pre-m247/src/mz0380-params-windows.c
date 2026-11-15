// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 Windows-sequence, IRQ, and ring module parameters.
 */

#include "mz0380-internal.h"

/* ---- M82: the Windows capture-start sequence ---------------------------- */

/*
 * The retail driver reconfigures with
 *   0x07(all channels) -> ~1.9 s -> 0x29 -> 0x2a -> 0x2d(main) -> 0x2d(sub)
 *   -> 0x31
 * and never sends START_STREAMING (0x06) on the capture path: 0x2d and 0x31
 * are themselves bare sysfs_notify("epint") doorbells in ep.ko, which is the
 * same wake 0x06 performs. Buffers are registered before the reconfiguration,
 * when the capture pin opens.
 *
 * 1 selects that order. 0 is the pre-M82 order
 * (0x29 -> 0x2d -> SET_BUF -> 0x2a -> 0x06), which is what every run up to and
 * including M81 used.
 *
 * DEFAULT IS 0, deliberately. M90 showed on hardware that win_seq=1 renders
 * nothing at all - not even the card's NO SIGNAL splash - while win_seq=0 with
 * fw=5 reliably renders it (760/1024 pages, M88). Both variables are necessary
 * and they interact. A default that is known not to reach the splash is a trap:
 * it silently confounds every experiment run "with defaults", which is exactly
 * how the vic_b0=0x20 test was wasted. Defaults track the best known-working
 * configuration; set win_seq=1 explicitly to work on the Windows ordering.
 */
bool mz0380_win_seq = true;	/* M216: part of the validated fw=7 load */
module_param_named(win_seq, mz0380_win_seq, bool, 0644);
MODULE_PARM_DESC(win_seq,
		 "M82: use the Windows capture-start order and opcode set. DEFAULT 1 since M216 - this is the validated 60 fps fw=7 configuration. The old 'win_seq=1 does not reach the splash' note (M90) predates M128-M137, which found the splash thread was masking a working capture path");

/*
 * Belt and braces: send 0x06 as well at the end of the Windows sequence. Off
 * because Windows does not, and an extra epint notify could re-enter the
 * encoder's read loop out of turn.
 */
bool mz0380_win_start_op6 = true;	/* M216: the producer needs it (M210b) */
module_param_named(win_start_op6, mz0380_win_start_op6, bool, 0644);
MODULE_PARM_DESC(win_start_op6,
		 "M82: also fire START_STREAMING(0x06) after the Windows sequence. DEFAULT 1 since M216: M210b showed the card-side producer starts only with the encoder tail AND op 0x06 - neither alone");

/*
 * The measured gap between "[FIRMWARE RESET]" and the [CH00] that follows it
 * was 1.84-1.91 s in every one of the six observed reconfigurations. Only a
 * few hundred ms of that is accounted for by sleeps inside the config
 * function, so treat the rest as the card settling and do not shorten it
 * without evidence.
 */
unsigned int mz0380_stop_settle_ms = 1900;
module_param_named(stop_settle_ms, mz0380_stop_settle_ms, uint, 0644);
MODULE_PARM_DESC(stop_settle_ms,
		 "M82: delay between the pre-STOP and SET_VIC (def:1900, Windows measures 1840-1910)");

/*
 * M84: SET_BUF placement, split out so it can be bisected without touching
 * win_seq. Windows registers its buffers when the capture pin opens, i.e.
 * before the reconfiguration, which is what 1 does. But M23 established on
 * hardware that op6 is what makes vpl_dmac latch channels[] into the outbound
 * iATU, and put SET_BUF *after* SET_VIC so our addresses are the ones latched.
 * Those two orderings only agree if op6 is absent; set 0 to keep the M23
 * placement while the rest of the Windows sequence stays.
 */
bool mz0380_win_bufs_first = true;
module_param_named(win_bufs_first, mz0380_win_bufs_first, bool, 0644);
MODULE_PARM_DESC(win_bufs_first,
		 "M84: with win_seq, program SET_BUF before SET_VIC as Windows does (def:1; 0 = M23 placement, after SET_VIC)");

/* M82: Windows configures a second (sub) encoder stream on every start. */
bool mz0380_enc_sub = true;
module_param_named(enc_sub, mz0380_enc_sub, bool, 0644);
MODULE_PARM_DESC(enc_sub,
		 "M82: also send SET_ENC_PARAMS for the sub stream (main_or_sub=1) (def:1)");

/*
 * M204: tinyvenc7's own SET_ENC_PARAMS printf identifies payload byte 20 as
 * `skip`, not QP-min. Non-zero values drive (input_counter % skip) == 1.
 * Zero selects a 128-bit schedule that h264_param_processing() itself fills
 * via tiny_calculate_skip_fps(input_fps, 0); that bitmap selects every input
 * frame. M205 hardware-validates value 0 at approximately 60 encoded fps from
 * a 60-Hz input, so it is now the default. Value 2 remains the proven 30-fps
 * fallback; value 1 can never leave remainder one.
 */
unsigned int mz0380_h264_frame_divisor;
module_param_named(h264_frame_divisor, mz0380_h264_frame_divisor, uint, 0644);
MODULE_PARM_DESC(h264_frame_divisor,
		 "tinyvenc7 H.264 frame schedule: 0=all-frame bitmap (def, hardware-proven 60 fps at 60 Hz), 2..255=modulo skip; 1 invalid");

/*
 * SET_ENC_PARAMS validity mask. We send bits 0/1/6 (fps, gop, bitrate) and
 * leave the rest masked out because their enums were never verified; Windows
 * sends 0x3FFF with every field populated. 0 keeps the conservative mask.
 */
unsigned int mz0380_enc_mask;
module_param_named(enc_mask, mz0380_enc_mask, uint, 0644);
MODULE_PARM_DESC(enc_mask,
		 "SET_ENC_PARAMS validity mask - 0=conservative fps/gop/bitrate, 0x3fff=Windows (def:0)");

/* M82: POST_PROC (0x31). Windows sends mask 0x1F with di=1, everything else 0. */
/*
 * M128d/M131: DEFAULT CHANGED 0x1f -> 0. The mask gates three stores in
 * tinyvenc5's SET_PREVIEW_PARAMS handler - bit 0 skip, bit 1 avg, bit 4 die_en
 * - and with 0x1f one of them TRUNCATES THE DMA to a single 16-byte burst.
 * Proven on hardware: same command, mask 0x1f -> 16 bytes, mask 0 -> a whole
 * 3110400-byte frame. It is also the entire reason win_seq=1 "renders nothing"
 * (M90/M91), since that path sends 0x31 with this mask.
 *
 * M214 corrects that reading. 16 bytes is not a truncation - it is the stub
 * tinyvenc7's vcap_handler writes on every frame the preview scheduler does
 * NOT select (`mov r3, #16` @ 0x129b0), on the same handle and buffer as the
 * full ALIGN16(W)*H*3/2 transfer. Frame selection reads a 128-bit bitmap that
 * only tiny_calculate_avg_fps / tiny_calculate_skip_fps ever write, and both
 * call sites sit behind mask bit 0. With post_mask=0 the bitmap stays zero, so
 * no frame is ever selected and every frame gets the stub.
 *
 * Bit 0 is therefore the one that matters, and it is an enable, not a
 * truncation: setting it makes the card start sending whole frames.
 */
unsigned int mz0380_post_mask;
module_param_named(post_mask, mz0380_post_mask, uint, 0644);
MODULE_PARM_DESC(post_mask,
		 "SET_PREVIEW_PARAMS(0x31) validity mask (def:0. M214: bit0 enables the preview frame-selection bitmap - without it the card writes a 16-byte stub per frame and never a whole one. bit1 applies post_avg, bit4 applies die_en)");

/*
 * M214: cmd bytes 0x0a (skip) and 0x0b (avg) were never populated - the
 * payload builder only ever set ch and fps - so the card has always taken
 * tiny_calculate_skip_fps(fps, skip=0, ...), whose step is skip+1. Step 1
 * selects EVERY frame, which at 1080p60 is 3110400 * 60 = ~186 MB/s on a link
 * this card negotiates as PCIe x1 Gen1 (~250 MB/s theoretical). post_skip
 * exists so the first bit-0 run can ask for a sane fraction of that instead:
 * skip=N selects every (N+1)th frame, so skip=29 is roughly 2 fps of full
 * frames alongside the H.264 stream.
 */
unsigned int mz0380_post_skip;
module_param_named(post_skip, mz0380_post_skip, uint, 0644);
MODULE_PARM_DESC(post_skip,
		 "SET_PREVIEW_PARAMS(0x31) skip byte 0x0a (def:0). Preview bitmap step is skip+1, so 0 selects every frame; needs post_mask bit0 to take effect");

unsigned int mz0380_post_avg;
module_param_named(post_avg, mz0380_post_avg, uint, 0644);
MODULE_PARM_DESC(post_avg,
		 "SET_PREVIEW_PARAMS(0x31) avg byte 0x0b (def:0). Non-zero with post_mask bit1 routes the card to tiny_calculate_avg_fps instead of tiny_calculate_skip_fps");

unsigned int mz0380_post_di = 1;
module_param_named(post_di, mz0380_post_di, uint, 0644);
MODULE_PARM_DESC(post_di, "M82: POST_PROC(0x31) deinterlace flag (def:1, what Windows sends)");

/*
 * M128: op 0x31 is not "POST_PROC", it is SET_PREVIEW_PARAMS, and its payload
 * byte 0x0e is `fake_frame_off`. Decoded statically from tinyvenc5's dispatch
 * table (main+0x804, index = cmd - 6) and its verbose printf at 0xf570:
 *
 *   [4..7]=mask(u32, sticky-OR'd)  [8]=ch  [9]=fps  [0x0a]=skip  [0x0b]=avg
 *   [0x0c]=die_en  [0x0d]=preview_off  [0x0e]=fake_frame_off
 *   [0x0f]=preview_no_osd  [0x10]=mirror  [0x11]=flip  [0x12]=hw_d
 *
 * The handler at 0xef24 stores byte 0x0e to
 * EncodingGroup::preview_params_settings[ch].byte[0x0a] UNCONDITIONALLY - the
 * validity mask gates only byte 0x0c (die_en, mask bit 4). That is the exact
 * byte M127 identified as the standby-splash gate: EncodingGroup::init_func
 * (0x10ac0) does
 *
 *   if (preview_params_settings[ch].byte[0x0a] == 0)
 *           pthread_create(&t, NULL, EncodingGroup::fake_frame_process, this);
 *
 * and main zeroes it at startup (0xe400). So on our baseline the standby
 * thread that draws NOSG_LOGO_Y is created because we have never told the card
 * otherwise. The second spawn site, in EncodingGroup::Start (0x10f30), is
 * gated by is_nosg instead, which we already send as 0.
 *
 * The guard runs inside the op-0x06 handler (which news the EncodingGroups,
 * then pthread_create's on_start_thread -> Start -> init_func), so 0x31 only
 * has an effect if it lands BEFORE START_STREAMING.
 *
 * Why it is worth a spawn: the "real frames are rejected in
 * libtkmf_video_source.so.0" mechanism is static RE only - the card's console
 * is not reachable, so we have never observed the drop. With the standby
 * thread suppressed there is nothing left to emit a frame except the real
 * encode path, which splits the two remaining stories cleanly:
 *   NO FRAME   -> the rejection is real; nothing ever reaches the encoder.
 *   real frame -> the standby thread was winning the race and masking it.
 * M131: DEFAULT CHANGED 0 -> 1. With it at 0 the card draws NOSG_LOGO_Y over a
 * perfectly good capture, which is what hid real video from this project for
 * its entire life (M129). Set it to 0 only to reproduce a pre-M129 result.
 */
bool mz0380_fake_frame_off = true;
module_param_named(fake_frame_off, mz0380_fake_frame_off, bool, 0644);
/*
 * M128b: the control for the above. The first fake_frame_off run changed TWO
 * things at once relative to the baseline - it sent op 0x31, which win_seq=0
 * never does, AND it set byte 0x0e in it - and it came back with M91's exact
 * 16-byte DMA stall. M91 saw that stall under win_seq=1, whose sequence also
 * contains 0x31, so "0x31 itself truncates the transfer" is a live and
 * parsimonious explanation that the run cannot separate from "the standby
 * thread is gone and the real path stalls".
 *
 * post_proc=1 sends 0x31 on the baseline with fake_frame_off left at 0, i.e.
 * one variable from that run. Method rule 1: every other variable is at a
 * value already known to permit a full frame, so it is informative either way.
 *   full splash (3110400 B) -> 0x31 is harmless; the stall belongs to
 *                              fake_frame_off, i.e. to the real path
 *   16 bytes of 0x11        -> 0x31 truncates; that is also M91's answer
 */
bool mz0380_post_proc = true;
module_param_named(post_proc, mz0380_post_proc, bool, 0644);
/*
 * M128b: how long to wait between op 0x31 and op 0x06.
 *
 * The control run pinned the truncation on 0x31 but NOT on anything 0x31
 * means. The handler is inert on this card: for ch 0 it falls through to
 * fopen("/tmp/PIC_INSERT","rb"), which cannot exist, so it sets
 * g_insert_pic = 0 and bare-ACKs (tinyvenc5 0xf068 -> 0xffac). The MemBroker
 * allocation that would have followed is never reached. Nothing it writes
 * touches capture, DMA or the VIC.
 *
 * What DID change is the mailbox cadence in front of START_STREAMING. ep.ko
 * serves one command at a time out of the mailbox and pokes it at the card as
 * a bare sysfs_notify("epint"); tinyvenc5 wakes, reads 44 bytes, and services
 * it. Measured on the two runs:
 *
 *   baseline   SET_AIC -> [156 ms, spent in the output-stage diag] -> 0x06
 *   with 0x31  SET_AIC -> [156 ms diag] -> 0x31 -> [9 us] -> 0x06
 *
 * Nine microseconds. 0x06 is fire-and-forget (timeout_ms = 0), so it does not
 * wait for anything, and it lands on the doorbell while tinyvenc5 is still
 * inside the 0x31 handler. Every other command in the sequence is tens to
 * hundreds of ms apart. That is the one thing 0x31 changes that could plausibly
 * truncate a transfer, and it also explains M91 without needing win_bufs_first.
 *
 * A gap is one variable from the run that produced the stall. Nonzero and the
 * frame comes back whole => cadence, and 0x31 is usable, which is what
 * fake_frame_off needs. Still 16 bytes at, say, 200 ms => the race is not it
 * and the 0x31 handler needs another read.
 */
unsigned int mz0380_post_proc_gap_ms;
module_param_named(post_proc_gap_ms, mz0380_post_proc_gap_ms, uint, 0644);
/*
 * M128c: which opcode goes into the pre-START slot. Default 0x31, i.e. no
 * change. The point of making it a variable is the control that partitions the
 * remaining space: op 0x09 is provably inert on BOTH sides - ep.ko routes it to
 * the same 0x1824 arm of pciep_isr that 0x2f/0x50/0x51/0x52/0x62 use, and
 * tinyvenc5's handler (0xe93c) is a bare pwrite(epint, payload, 44) with no
 * side effect whatsoever.
 *
 *   0x09 also truncates -> the fault is "an extra epint command immediately
 *                          before START", not anything 0x31 means. Structural,
 *                          and it would apply to the whole win_seq ordering.
 *   0x09 is clean        -> 0x31's own writes into tinyvenc5 are the cause,
 *                          and post_mask / post_di split them further.
 */
unsigned int mz0380_post_proc_opcode = MZ0380_CMD_POST_PROC;
module_param_named(post_proc_opcode, mz0380_post_proc_opcode, uint, 0644);
MODULE_PARM_DESC(post_proc_opcode,
		 "M128c: opcode for the pre-START slot (def:0x31). 0x09 is the inert control - same ep.ko arm, and tinyvenc5's handler is a bare ACK");
MODULE_PARM_DESC(post_proc_gap_ms,
		 "M128b: ms to wait between SET_PREVIEW_PARAMS(0x31) and START_STREAMING(0x06). 0 reproduces the 16-byte stall exactly; try 200 (def:0)");
MODULE_PARM_DESC(post_proc,
		 "send SET_PREVIEW_PARAMS(0x31) on the win_seq=0 baseline (def:1 since M131 - it is what carries fake_frame_off). 0 restores the pre-M129 sequence");
MODULE_PARM_DESC(fake_frame_off,
		 "M128: SET_PREVIEW_PARAMS(0x31) byte 0x0e - 1 suppresses the card's standby NOSG_LOGO_Y thread, so a delivered frame is a real one (def:1 since M131; 0 restores the splash)");

/*
 * M4 diagnostic: enable bus mastering + MSI/ISR BEFORE firmware load, but do
 * NOT program any (still-unverified) ring addresses. Safe because the card has
 * no host DMA target to write to; tests whether the post-boot mailbox doorbell
 * only reaches the card once bus mastering is on.
 */
/*
 * M82: run the card on legacy INTx, which is the only interrupt path the
 * Windows driver has ever used on this device. See mz0380_irq_request().
 */
bool mz0380_irq_intx = true;
module_param_named(irq_intx, mz0380_irq_intx, bool, 0444);
MODULE_PARM_DESC(irq_intx,
		 "M82: force legacy INTx instead of MSI, as the Windows driver does (def:1)");

/*
 * M166: the shipping default is now the known-good capture configuration.
 *
 * This was `bool mz0380_dma_handshake;` - off - since bring-up, and every successful
 * capture in this project's history passed it explicitly. Left off, `insmod
 * ./mz0380.ko` produces a device that cannot capture, and the user sees either
 * no /dev/video* at all or a node that never delivers. That is not a safe
 * default, it is a broken one.
 *
 * Pass dma_handshake=0 to get the old bring-up behaviour back.
 */
bool mz0380_dma_handshake = true;
module_param_named(dma_handshake, mz0380_dma_handshake, bool, 0444);
MODULE_PARM_DESC(dma_handshake,
		 "enable bus master + MSI (no ring programming) before the firmware handshake; M4 diagnostic (def:1 since M166)");

bool mz0380_enable_audio;
module_param_named(enable_audio, mz0380_enable_audio, bool, 0444);
MODULE_PARM_DESC(enable_audio,
		 "register the experimental ALSA scaffold (PCM DMA is not implemented; def:0)");

unsigned int mz0380_video_ring_entries = 16;
module_param_named(video_ring_entries, mz0380_video_ring_entries,
		   uint, 0444);
MODULE_PARM_DESC(video_ring_entries,
		 "reserved legacy ring knob; real video uses four SET_BUF slots (default 16)");

unsigned int mz0380_video_ring_entry_size = (512 * 1024);
module_param_named(video_ring_entry_size, mz0380_video_ring_entry_size,
		   uint, 0444);
MODULE_PARM_DESC(video_ring_entry_size,
		 "reserved legacy ring knob, not used by SET_BUF video (default 512 KiB)");

unsigned int mz0380_audio_ring_entries = 8;
module_param_named(audio_ring_entries, mz0380_audio_ring_entries,
		   uint, 0444);
MODULE_PARM_DESC(audio_ring_entries,
		 "reserved until the audio DMA ABI is implemented (default 8)");

unsigned int mz0380_audio_ring_entry_size = 32768;
module_param_named(audio_ring_entry_size, mz0380_audio_ring_entry_size,
		   uint, 0444);
MODULE_PARM_DESC(audio_ring_entry_size,
		 "reserved until the audio DMA ABI is implemented (default 32 KiB)");
