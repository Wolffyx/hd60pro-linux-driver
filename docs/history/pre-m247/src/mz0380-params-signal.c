// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 HDMI receiver and capture-format module parameters.
 */

#include "mz0380-internal.h"

bool mz0380_probe_windows;
module_param_named(probe_windows, mz0380_probe_windows, bool, 0444);
MODULE_PARM_DESC(probe_windows,
		 "M32 probe: also program outbound windows 1-3 (op 0x04/0x05/0x03) pointing at stream bufs 1/2/3, to find where the encoder writes its bitstream (def:0)");

/*
 * Host-only H.264 probe. Unlike the historical probe_windows knob, this owns
 * four independent window-1 buffers and therefore cannot let the encoded and
 * raw-preview DMA engines overwrite one another. It also selects the complete
 * encoder payload observed in the Windows trace; normal capture is unchanged.
 */
/*
 * M217: deliver the card's RAW frames to V4L2 instead of its encoded ones.
 *
 * This rides the encoded path rather than replacing it, because M210b showed
 * the card-side producer only runs with the encoder tail AND op 0x06 - there is
 * no raw-only mode that produces anything. So the encoder keeps running and its
 * output is simply not forwarded; the raw banks are delivered in its place.
 *
 * Requires post_mask bit 0 to be set, or the card writes a 16-byte stub per
 * frame and no whole frame ever arrives (M214/M215). post_skip sets the
 * fraction of frames that arrive whole: the bitmap step is post_skip+1, and
 * post_skip=0 asks for every frame, which at 1080p60 is ~186 MB/s on a link
 * this card negotiates as PCIe x1 Gen1.
 *
 * Implies the raw bank allocation that raw_bank_observe performs, so it does
 * not have to be set as well. Like raw_bank_observe it is read at insmod - the
 * banks come from mz0380_dma_setup() at PCI probe.
 */
/*
 * M240: DEFAULT 1. Raw is the format a camera should offer first.
 *
 * ENUM_FMT returns this format at index 0, and most camera consumers -
 * browsers, PipeWire, video-conferencing apps - take the first entry or ignore
 * H.264 entirely. With H.264 first the node reads as a capture-card oddity
 * rather than a webcam, which is also how two hardware runs were spent testing
 * the encoded path by accident while trying to reproduce a raw defect in OBS.
 *
 * The encoded path is unchanged and one S_FMT away. What flips is only what an
 * application gets when it does not ask.
 */
bool mz0380_raw_deliver = true;
module_param_named(raw_deliver, mz0380_raw_deliver, bool, 0444);

/*
 * M218: allocate the raw banks even when raw is not the startup format, so an
 * application can select I420 through S_FMT without the module being reloaded.
 *
 * The cost is the allocation itself - two four-buffer banks of 0x466000 - which
 * is why it is a knob rather than unconditional. Turning it off restores the
 * pre-M218 behaviour where raw is reachable only by loading with
 * raw_deliver=1.
 */
bool mz0380_raw_capable = true;
module_param_named(raw_capable, mz0380_raw_capable, bool, 0444);
MODULE_PARM_DESC(raw_capable,
		 "M218: allocate the raw banks so I420 can be selected at runtime through S_FMT (def:1). 0 saves the allocation and makes raw reachable only via raw_deliver=1 at load");
MODULE_PARM_DESC(raw_deliver,
		 "M217/M218/M240: make I420 raw the STARTUP delivery format and the first ENUM_FMT entry (def:1 since M240 - most camera applications take the first format or cannot use H.264 at all). 0 restores H.264 first. Either way both formats are enumerated and an application can pick with S_FMT. post_mask bit 0 is forced on whenever raw is live, so no other parameter is needed");

bool mz0380_h264_probe = true;	/* M216: this is the capture path */
module_param_named(h264_probe, mz0380_h264_probe, bool, 0444);
MODULE_PARM_DESC(h264_probe,
		 "allocate/register a dedicated Windows-style H.264 window-1 ring and deliver from its completion events. DEFAULT 1 since M216 - this stopped being a diagnostic at M177 and is now the working capture path");

/*
 * M36. The fake frame's payload is a constant 0x11 fill followed by ZERO
 * padding, and the old buffer sampling only looked for non-zero bytes - so
 * neither repeated frames nor the true end of the card's write burst were
 * visible. Poisoning the buffers with 0xAA before START makes every
 * card-written byte (including zeros) detectable; a kthread in mz0380-dma.c
 * then tracks the write extent per buffer over time (crawl vs stall, exact
 * stop offset - M35 measured the visible stop at 0x30a000 = 1556 x 0x800
 * DMAC chunks, but zeros beyond it were invisible).
 */
/*
 * EDID command framing is confirmed by both sides of the shipped stack:
 * Windows sends opcode 0x1f with nine parameters and waits for STATUS; the
 * card handler interprets byte 5 as the EEPROM offset and writes the payload
 * in retrying <=8-byte I2C chunks.  Keep the knobs visible for diagnostics,
 * but use that known-good transaction by default.
 */
/*
 * M51b. Data-bit polarity of GPIO_DIR (op 0x17): 0 = data bit 1 means
 * OUTPUT (our reading), 1 = inverted. Unverified against the card, and a
 * wrong guess makes every bit-banged line look stuck low, so it is a
 * runtime switch rather than a constant.
 */
/*
 * M53. Candidate registers per bank for the indirect-port hunt. Cost is
 * quadratic (~20 ms per ordered pair over the mailbox), so 48 candidates is
 * ~45 s per bank; raise it if the hunt comes back empty and the writable
 * count printed per bank was clipped.
 */
unsigned int mz0380_edidhunt_max_regs = 48;
module_param_named(edidhunt_max_regs, mz0380_edidhunt_max_regs, uint, 0644);
MODULE_PARM_DESC(edidhunt_max_regs,
		 "M53: max writable registers per bank used as indirect address/data candidates (def:48; cost is quadratic)");

/*
 * M54. A real source walks in and out of lock while it settles (observed:
 * detect 0x83 -> 0xa3 locked -> 0x83 -> 0x03 inside a second), so a
 * single-shot detect read calls a transmitting source "no signal". How
 * long to sample before giving up.
 */
unsigned int mz0380_signal_poll_ms = 2000;
module_param_named(signal_poll_ms, mz0380_signal_poll_ms, uint, 0644);
MODULE_PARM_DESC(signal_poll_ms,
		 "M54: how long to sample the MST3367 detect register for a lock before reporting no signal (def:2000)");

/*
 * HDMI presence must be monitored independently of H.264 activity. Firmware
 * may be silent after unplug, or a future revision may continuously encode a
 * fallback picture; neither behavior tells us when a real source returns.
 * The monitor reads only the MST3367 lock byte while steady, and performs the
 * expensive coherent timing measurement only across a lock transition.
 */
bool mz0380_hotplug_recovery = true;
module_param_named(hotplug_recovery, mz0380_hotplug_recovery, bool, 0644);
MODULE_PARM_DESC(hotplug_recovery,
		 "re-arm HDMI acquisition after encoded-frame silence (def:1)");

unsigned int mz0380_hotplug_stall_ms = 1500;
module_param_named(hotplug_stall_ms, mz0380_hotplug_stall_ms, uint, 0644);
MODULE_PARM_DESC(hotplug_stall_ms,
		 "encoded-frame silence before HDMI reconnect recovery starts (def:1500)");

unsigned int mz0380_hotplug_retry_ms = 500;
module_param_named(hotplug_retry_ms, mz0380_hotplug_retry_ms, uint, 0644);
MODULE_PARM_DESC(hotplug_retry_ms,
		 "delay between HDMI reconnect detection attempts (def:500)");

unsigned int mz0380_signal_monitor_ms = 500;
module_param_named(signal_monitor_ms, mz0380_signal_monitor_ms, uint, 0644);
MODULE_PARM_DESC(signal_monitor_ms,
		 "MST3367 retry interval while placeholder-only, producer-stalled, or validating reattachment (def:500)");

unsigned int mz0380_no_signal_fps = 2;
module_param_named(no_signal_fps, mz0380_no_signal_fps, uint, 0644);
MODULE_PARM_DESC(no_signal_fps,
		 "host H.264 NO SIGNAL placeholder cadence while VB2 is attached (def:2, range clamped to 1..10)");

/*
 * Cold-card flash boot time varies with the board and its power source. Use a
 * deadline instead of a fixed retry count so an already-running card returns
 * immediately while a genuinely slow card still gets time to become ready.
 */
unsigned int mz0380_card_ready_timeout_ms = 15000;
module_param_named(card_ready_timeout_ms, mz0380_card_ready_timeout_ms,
		   uint, 0644);
MODULE_PARM_DESC(card_ready_timeout_ms,
		 "maximum ms to wait for flash firmware to answer CMD_INIT (def:15000; exits immediately when ready)");

/*
 * M54. Stream a locked-but-unmatched signal as 1080p60 when the geometry
 * says 1080p (htotal 2200). The first source that ever locked reports a
 * saturated vperiod counter, so the frame rate cannot be derived and no
 * table entry matches - without this the whole real-signal path stays
 * blocked on one unreadable register.
 */
/*
 * M114 proved that this firmware writes no card frame at all without receiver
 * lock. Keep STREAMON useful anyway by attaching VB2 to a host-owned H.264
 * placeholder and postponing SET_VIC until a coherent HDMI mode appears.
 * This is enabled by default on the persistent H.264 path; disabling it
 * restores the old -ENOLCK STREAMON failure.
 */
bool mz0380_stream_without_signal = true;
module_param_named(stream_without_signal, mz0380_stream_without_signal,
		   bool, 0644);
MODULE_PARM_DESC(stream_without_signal,
		 "allow H.264 STREAMON without HDMI lock: replay a host placeholder and defer the first encoder spawn until stable lock (def:1)");

bool mz0380_force_timings;
module_param_named(force_timings, mz0380_force_timings, bool, 0644);
MODULE_PARM_DESC(force_timings,
		 "M54: on a locked but unmatched signal with htotal=2200, stream as 1920x1080p60 (def:0)");

bool mz0380_gpio_dir_invert;
module_param_named(gpio_dir_invert, mz0380_gpio_dir_invert, bool, 0644);
MODULE_PARM_DESC(gpio_dir_invert,
		 "M51: invert the GPIO_DIR (op 0x17) data-bit sense used by the bit-banged I2C (def:0 = 1 means output)");

unsigned int mz0380_edid_opcode = MZ0380_CMD_I2C_WRITE_S;
module_param_named(edid_opcode, mz0380_edid_opcode, uint, 0644);
MODULE_PARM_DESC(edid_opcode,
		 "mailbox opcode used to push EDID (def: 0x1f, confirmed Windows/card bulk-write handler)");

unsigned int mz0380_edid_timeout_ms = 100;
module_param_named(edid_timeout_ms, mz0380_edid_timeout_ms, uint, 0644);
MODULE_PARM_DESC(edid_timeout_ms,
		 "ms to wait for each synchronous EDID chunk (def:100; 0 is unsafe fire-and-forget)");

/*
 * M160: the M115 completeness check, on the completion-event path.
 *
 * M115 added it to the poll-drain only, because `frame_events` was 0 in every
 * run this project had ever done - the event path was dead code and nobody
 * could tell. `fw=7` (M159) makes real completion events fire at ~60 Hz, and
 * the event path delivered a 16-byte torn prefix and then re-poisoned the
 * buffer *while the DMA was still writing it*, which is precisely the failure
 * M115's own comment describes.
 *
 * With this set, an event that arrives before the card has written
 * width*height*3/2 bytes is left completely alone - not delivered, and
 * crucially NOT re-poisoned - so the transfer can finish and a later pass
 * picks it up whole.
 *
 * Set to 0 to reproduce the pre-M160 behaviour, which is the only way to see
 * the torn prefixes again if they turn out to be the real transfer size.
 */
bool mz0380_event_require_complete = true;
module_param_named(event_require_complete, mz0380_event_require_complete, bool, 0644);
MODULE_PARM_DESC(event_require_complete,
		 "M160: on a completion event, deliver only a complete w*h*3/2 frame; short ones are left un-poisoned so the DMA can finish (def:1)");

bool mz0380_buf_poison = true;
module_param_named(buf_poison, mz0380_buf_poison, bool, 0644);
MODULE_PARM_DESC(buf_poison,
		 "M36: fill stream buffers with a poison byte before START and track the card's write extent over time (def:1)");

/*
 * M37 follow-up: the hole map found EXACTLY one 4-byte hole at 0x6d384,
 * reproducible at the same offset across runs. A deterministic single-dword
 * hole is at least as likely to be the frame DATA containing the poison
 * word (collision) as a lost write. Running twice with two different poison
 * bytes discriminates: a real un-written hole stays put; a collision moves
 * or disappears.
 */
/*
 * M229: the byte the CARD writes when it clears a slot before filling it.
 *
 * M226 measured it: the unfilled tail of a partially delivered frame reads
 * 0x01 to the last row, and 0x01 is none of the poison values this driver
 * uses (0xa5 and 0x5a for the banks, 0xaa for the frame buffers). It is luma
 * black, written by the card. A parameter rather than a constant so a capture
 * that shows a different clear value can be handled without a rebuild - and so
 * setting it to something the card never writes disables the test, which is
 * how to check whether the test itself is what changed the result.
 */
/*
 * M234: what quantization the RAW node advertises.
 *
 * The node reported V4L2_QUANTIZATION_LIM_RANGE for every format, and a
 * 600-frame capture measured luma reaching 254 with 4.2% of sampled pixels
 * above 235 - which limited-range content cannot do. A full-range payload
 * labelled limited makes every consumer expand it a second time, which is the
 * brighter, harsher picture the operator sees against Windows.
 *
 * RETRACTED 2026-08-31, default now 0. That 4.2% is not reproducible. A later
 * 600-frame capture of the same scene, taken after the picture had stopped
 * drifting bright, measured 0.001% above 235 - fifteen samples out of 1.25
 * million - with a maximum of 237. The original reading was an artifact of
 * capturing while the brightness was still elevated, so the evidence for
 * calling this payload full range is gone.
 *
 * The parameter stays because the question is not settled, only unproven: 1
 * advertises full range, 0 is the limited-range default that matches the rest
 * of the driver. What would actually settle it is a capture with something
 * genuinely black in frame - a full-range source reads near 0 there, a limited
 * one near 16 - which no capture so far has contained.
 *
 * Affects the raw path only; the H.264 bitstream carries its own VUI.
 */
bool mz0380_raw_full_range;	/* M234 RETRACTED - see below. Default limited. */
module_param_named(raw_full_range, mz0380_raw_full_range, bool, 0644);
MODULE_PARM_DESC(raw_full_range,
	"M234 (RETRACTED): advertise the raw I420 node as full range (def:0). The 4.2%-above-235 measurement behind the original default was not reproducible - a later capture showed 0.001% - so limited range is the default again. Set 1 only with fresh evidence. Does not affect H.264, which carries its own VUI");

/*
 * M238: the byte the card clears the CHROMA planes to.
 *
 * M226 measured 0x80 there while luma cleared to 0x01, which is what neutral
 * black looks like in I420. Separate from raw_clear_byte because the two
 * planes clear to different values and the completeness test has to check
 * both - see mz0380_raw_probe_frame_filled.
 */
unsigned int mz0380_raw_clear_chroma = 0x80;
module_param_named(raw_clear_chroma, mz0380_raw_clear_chroma, uint, 0644);
MODULE_PARM_DESC(raw_clear_chroma,
	"M238: byte the card writes when clearing the chroma planes of a raw slot (def:0x80). A slot whose chroma tail is entirely this value has not finished filling, and delivering it gives correct luma with wrong colour");

unsigned int mz0380_raw_clear_byte = 0x01;
module_param_named(raw_clear_byte, mz0380_raw_clear_byte, uint, 0644);
MODULE_PARM_DESC(raw_clear_byte,
	"M229: byte the card writes when clearing a raw slot before filling it (def:1). A slot whose last luma rows are entirely this value has not finished filling and is not delivered");

unsigned int mz0380_poison_byte = 0xaa;
module_param_named(poison_byte, mz0380_poison_byte, uint, 0644);
MODULE_PARM_DESC(poison_byte,
		 "M37: poison byte value for buf_poison (def 0xaa; re-run with e.g. 0x55 to tell a real write hole from a data-equals-poison collision)");

/*
 * M33. The card's encoder will not complete a frame until audio is declared
 * ready. tinyvenc5's is_nosg path waits on /sys/audio_status/audio_ready, and
 * the only writer of that file is video_capture_mgr's SET_AIC_PARAMS (op 0x2a)
 * handler, which runs "echo '1' > /sys/audio_status/audio_ready" when the
 * command's on byte is set. So we send SET_AIC(on=1) just before START.
 * The rest of the fields are ordinary PCM geometry; they are parameters rather
 * than constants because nothing yet tells us the card validates them, and a
 * mismatch with the real I2S setup is a plausible future suspect.
 */
bool mz0380_aic_on = true;
module_param_named(aic_on, mz0380_aic_on, bool, 0644);
MODULE_PARM_DESC(aic_on,
		 "M33: send SET_AIC_PARAMS(on=1) before START, releasing the encoder's audio_ready gate (def:1)");

/*
 * M60: the nosg path costs one encoder spawn per frame (M39: the card's
 * encoder parks after exactly one frame, and only a fresh spawn unparks it),
 * and the card wedges for good after roughly 8-18 of them - a wedge no PCIe
 * reset clears (M52), only removing slot power. Every spawn also re-sent
 * SET_AIC(on=1). audio_ready is a sysfs flag on the card, so re-arming it per
 * frame is most likely pure waste; sending it once per streaming session may
 * stretch the budget. Set aic_every_frame=1 to go back if frames stop landing.
 */
/*
 * M62: require a second agreeing measurement pass before trusting a mode.
 *
 * Off by default, because it costs more time than the source gives us. One
 * pass is ~17 mailbox round-trips (~80ms) and demanding two agreeing passes
 * 10ms apart needs ~170ms of unbroken lock, while the only source available
 * transmits in bursts of roughly 300ms starting at an arbitrary moment - so
 * the pair rarely fits and the run reports "no coherent timing snapshot
 * survived" despite a real full lock.
 *
 * A single pass is not an unchecked one: it re-reads detect and vtotal_lo
 * after the block and rejects the sample if either moved, then cross-checks
 * vperiod against hperiod/(vtotal+1) within 0.5Hz. Set this when a source
 * holds lock continuously and the extra redundancy is free.
 */
/*
 * M65: how long a successful HDMI detection may be reused to arm the encoder
 * after the source has stopped transmitting. The only source available bursts
 * for roughly 300ms per power-cycle, so requiring a live lock at STREAMON
 * makes capture unreachable even though the mode is known. 0 disables the
 * fallback and restores "live lock or nothing".
 */
unsigned int mz0380_signal_cache_ms = 30000;
module_param_named(signal_cache_ms, mz0380_signal_cache_ms, uint, 0644);
MODULE_PARM_DESC(signal_cache_ms,
		 "M65: reuse the last good HDMI detection for this long when arming the stream (def:30000, 0=require a live lock)");

/*
 * V4L2 clients commonly issue several identical DV-timings queries during one
 * negotiation. A receiver measurement costs serialized mailbox traffic, so
 * briefly reuse only a currently locked result. Signal loss is delayed by no
 * more than this interval; zero restores an uncached receiver read every time.
 */
unsigned int mz0380_signal_query_cache_ms = 1000;
module_param_named(signal_query_cache_ms, mz0380_signal_query_cache_ms,
		   uint, 0644);
MODULE_PARM_DESC(signal_query_cache_ms,
		 "reuse a successful live DV-timings query for this many ms (def:1000; 0=always read receiver)");

/*
 * M71: SET_VIC byte6 = "fw", the card's ENCODER SELECTOR, not a pixel format.
 * video_capture_mgr compares it to 7 and 8 to launch ./tinyvenc7 / ./tinyvenc8
 * and otherwise falls through to ./tinyvenc5; the value is also handed to the
 * encoder in its own argv. We had been sending 2 or 3 here (a misread of the
 * field as "2=progressive, 3=interlaced"), which reached tinyvenc5 only by
 * fallthrough. 5 is the H.264 encoder this card uses. Exposed as a parameter
 * so the alternative can be bisected on hardware without a rebuild.
 *
 * M82 supersedes the value, not the meaning. The Windows driver's own
 * [CH00] log prints byte6 as "fw" and byte7 as "vi", and across all four
 * live traces it sends fw = 6 for 1080p30 / 1080p29.97 and fw = 7 for
 * 1080p60. It never sends 5. Since vcm derives the capture cfg's "output
 * format" from this byte (2/YUY2 when fw == 6, else 1/YV12) and spawns
 * ./tinyvenc7 when it is 7, 5 and 6 differ in the pixel format the capture
 * stage is told to produce - and 5 is a value the retail driver has never
 * exercised.
 *
 * M88 (hardware): and yet **only fw = 5 works on this card.** Bisected against
 * the splash oracle with everything else held at its M82 value - the Windows
 * ordering, the new color_info/fast_kill/nosg/aic_int_mode, INTx - flipping
 * this one byte is the whole difference:
 *
 *     fw = 5  -> tinyvenc5, cfg output format 1/YV12  -> splash renders
 *     fw = 6  -> tinyvenc5, cfg output format 2/YUY2  -> nothing written
 *     fw = 7  -> tinyvenc7, cfg output format 1/YV12  -> nothing written
 *
 * 5 and 6 differ ONLY in the cfg's output format, so YUY2 by itself stops the
 * capture loop before it writes anything; 7 fails separately, through a
 * different encoder binary. Windows sends 6 or 7 and captures, so something
 * about the card's state differs from ours and this byte is where it surfaces
 * - but a value that demonstrably kills the capture loop is not one to ship
 * for the sake of parity. 0 selects the Windows rule (6 at <=30 fps, 7 above)
 * for anyone re-testing that contradiction.
 *
 * M159/M161 OVERTURN the "fw = 7 -> nothing written" line above. That verdict
 * came from the pre-M129 regime - splash oracle, post_mask=0x1f truncating the
 * DMA, fake_frame_off unset - which M129 says invalidates every negative of
 * that era, and this was one. Re-measured with the M129 recipe, fw=7 is the
 * ONLY value that streams: 1621 completion interrupts in 57 s, fifo_drops=0,
 * the frame-token register cycling 0->1->2->3 1558 times. fw=5 raises
 * frame_events=0 - always has - and delivers one frame per encoder process
 * because tinyvenc5's build never writes its mma_already_start latch (M158).
 *
 * The default stays 5 only because fw=7 currently transfers 16 bytes per frame
 * instead of 3110400 (M161, card-side, unresolved). When that is fixed, 7 is
 * the value to ship - it is the one the completion path was designed around.
 */
unsigned int mz0380_vic_fw = 7;	/* M216: the binary that streams */
module_param_named(vic_fw, mz0380_vic_fw, uint, 0644);
MODULE_PARM_DESC(vic_fw,
		 "SET_VIC byte6 'fw' - picks which encoder BINARY the card spawns (M158): 7=tinyvenc7 (streams continuously; DEFAULT since M216), anything else=tinyvenc5 (one frame per process, then frozen); 0=Windows rule (6 at <=30fps, 7 above)");

/*
 * M72: SET_VIC byte12 ("m" in the card's log) is VideoCap's OUTPUT FORMAT.
 * The SDK's own capture config, re-dump/fw/yuan_demo_sdi/nullsensor_1920x1080.cfg,
 * documents the enum inline: "1:YUV420, 2:YUV422" - and 0, which this driver
 * has always sent, is not a legal value. H.264 encodes from YUV420.
 *
 * M79 then found that byte12 never reaches the cfg at all - vcm passes it on
 * the tinyvenc argv instead, and the cfg's output format comes from byte6.
 * M82 settles the value: Windows sends m = 0 for both 1080p30 and 1080p60 and
 * m = 1 only for the 29.97 DSLR, so it looks like a fractional-rate flag, and
 * 0 is what this card is given for an ordinary integer-rate source.
 */
unsigned int mz0380_vic_out_format;
module_param_named(vic_out_format, mz0380_vic_out_format, uint, 0644);
MODULE_PARM_DESC(vic_out_format,
		 "SET_VIC byte12 'm' - Windows sends 0 at integer frame rates, 1 at 29.97 (def:0)");

/*
 * M72: SET_VIC bytes 16..19 ("color_info") line up with the same config file's
 * brightness / contrast / saturation / field-invert block, whose documented
 * neutral values are 0 / 0 / 128 / 0 - "saturation adjustment (0~255, 128:off,
 * 0:mono)". We have always sent 0,0,0,0, i.e. saturation pinned to mono.
 *
 * M82 retires that reading: the four bytes are color_info[0..3] and Windows
 * sends a fixed 1,1,1,2 on every SET_VIC, never a picture control. vic_saturation
 * now only overrides byte18 of vic_color_info, and does nothing unless set.
 */
unsigned int mz0380_vic_saturation = MZ0380_VIC_SATURATION_UNSET;
module_param_named(vic_saturation, mz0380_vic_saturation, uint, 0644);
MODULE_PARM_DESC(vic_saturation,
		 "override SET_VIC byte18 only; unset leaves vic_color_info alone (def:unset)");

/*
 * M82: SET_VIC bytes 16..19, exactly as the Windows driver builds them at
 * 0x14028bcb9 - [16]=1 [17]=1 [18]=1 [19]=2, i.e. 0x02010101 little-endian.
 * Identical in every trace and for every source.
 */
unsigned int mz0380_vic_color_info = 0x02010101;
module_param_named(vic_color_info, mz0380_vic_color_info, uint, 0644);
MODULE_PARM_DESC(vic_color_info,
		 "M82: SET_VIC bytes 16..19 color_info (def:0x02010101, what Windows always sends)");

/*
 * M82: SET_VIC byte33 fast_kill. Windows logs fk=1 unconditionally.
 *
 * M157: we default to 0 anyway, and this is the one place we deliberately do
 * not match Windows. The byte selects how video_capture_mgr disposes of the
 * previous tinyvenc5 before it spawns the next one (vcm 0x9554):
 *
 *   fk == 1  ->  "SIG   KILL"  kill(pid, 9)      - no cleanup at all
 *   fk != 1  ->  "SIG    INT"  kill(pid, 2), then poll kill(pid,0) every
 *                10 ms up to 200 times (2 s) for a clean exit, and only
 *                then fall back to killall -9
 *
 * tinyvenc5 handles SIGINT/SIGTERM/SIGSEGV with sig_kill() (0x187ec), which
 * runs EncodingGroup::~EncodingGroup() and exit(0) - so the SIGINT path also
 * gets the atexit chain, i.e. SSM_Recycle -> shm_unlink and
 * MemBroker_FreeMemory. Under SIGKILL none of that runs, which is the best
 * candidate for the 8-18 spawn wedge (RE_FINDINGS M157).
 *
 * Neutral on capture: verified on hardware, full 3110400-byte frame, NOT
 * SPLASH, CHROMA OK. Costs at most the card's own teardown wait, which hides
 * inside our existing SET_VIC settle.
 */
unsigned int mz0380_vic_fast_kill;
module_param_named(vic_fast_kill, mz0380_vic_fast_kill, uint, 0644);
MODULE_PARM_DESC(vic_fast_kill,
		 "SET_VIC byte33 fast_kill: 0 = card SIGINTs the old encoder and waits for a clean exit (def, M157), 1 = SIGKILL, what Windows sends");

/*
 * M82: SET_VIC bytes 36..39 - no-signal fill colour, back.Y.U.V. Windows sends
 * 0.00.80.80, i.e. neutral grey; we sent all zeros, which is green.
 */
/*
 * M136/M138: SET_VIC byte 34 - NOT the frame-completion interrupt enable.
 *
 * Found in ep.ko, not guessed. pciep_isr's cmd-41 arm (0x171c) ends with:
 *
 *   1744: ldrb r1, [r4, #0x22]     @ SET_VIC payload byte 34
 *   1748: adds r3, r1, #0
 *   174c: movne r3, #1
 *   1754: strb r3, [r5, #0x630]    @ state[0x630] = (byte34 != 0)
 *
 * and state[0x630] IS read by store_channel_done() - the sysfs attribute the
 * CARD's userspace writes when a channel finishes a frame:
 *
 *   e88: ldrb r3, [r1, #0x630]
 *   e8c: cmp  r3, #0
 *   e94: beq  0xefc                @ int_mode == 0: the OTHER branch
 *
 * M138 CORRECTION - this byte is NOT the EVENT gate. Both branches of that
 * test raise the EVENT the same way, and both are gated on the same thing: the
 * one-shot completion credit at ep.ko's .data[0].
 *
 *   e98: ldr r0, [credit] ; beq 0xed8   @ int_mode != 0 branch: no credit ->
 *   f08: ldr r8, [credit] ; beq 0xf34   @ int_mode == 0 branch:  accumulate
 *   f54: ldr r0, [credit] ; beq 0xfb4   @ ditto, aic_int_mode != 1
 *
 * The only difference between the branches is the shift used for the AUDIO
 * channel bit (r0+15 vs r4+16). For video the code is identical. So byte 34
 * cannot change frame reporting, which is exactly what M137 measured.
 *
 * The credit itself is fine: it starts at 1, every raised event consumes it,
 * and card IRQ 42 (pciep_isr_clrint) restores it - reached from the host by
 * BAR0[0x00] = MZ0380_MB_INT_ACK (0x400, bit 10; 0x800/bit 11 is IRQ 43, the
 * command dispatcher). M133's 1224 completed commands prove that path re-arms
 * it 1000+ times per run. See mz0380_credit_rearm().
 *
 * Kept as a knob only because it is free and the AIC twin (SET_AIC byte 17 ->
 * state[0x63c]) is set to 1; nothing depends on it.
 */
unsigned int mz0380_vic_int_mode;
module_param_named(vic_int_mode, mz0380_vic_int_mode, uint, 0644);
MODULE_PARM_DESC(vic_int_mode,
		 "SET_VIC byte 34 -> ep.ko state[0x630]. M138: NOT the frame-completion EVENT gate (the one-shot credit is, and it is re-armed correctly); byte 34 only shifts the audio bit. Tried in M137, changes nothing (def:0)");

unsigned int mz0380_vic_nosg = 0x80800000;
module_param_named(vic_nosg, mz0380_vic_nosg, uint, 0644);
MODULE_PARM_DESC(vic_nosg,
		 "M82: SET_VIC bytes 36..39 nosg back/Y/U/V (def:0x80800000 = 0.00.80.80)");

/*
 * M73: BANK0 0xb0 output format/clock select for ordinary (non-720p30) modes.
 * We write 0x21; the GPL hdcapm driver ends its init with 0xb0 = 0x20,
 * commented "YUV422 / 8-bit output". Bit0 is the difference and it is
 * unidentified. Tunable so the two can be compared on hardware while the
 * receiver's output stage is under suspicion for producing no BT1120 clock.
 */
unsigned int mz0380_vic_b0 = 0x21;
module_param_named(vic_b0, mz0380_vic_b0, uint, 0644);
MODULE_PARM_DESC(vic_b0,
		 "M73: MST3367 BANK0 0xb0 output select for ordinary modes (def:0x21, hdcapm uses 0x20)");

/*
 * M126: BANK0 0xb1 / 0xb2 / 0xb5 - the receiver output-stage registers that
 * every sibling driver disagrees with us about, and that have never been
 * tunable.
 *
 *            ours    hdcapm   gchd (1080p)   Windows 0x9c trace
 *   0xb1     0xc0    0xe0     0x0c           0xc0  (confirmed)
 *   0xb2     0x00    0x08     0xc4           value never resolved
 *   0xb5     0x0c    -        0xd0           not seen
 *
 * The Windows trace pins 0xb1 and leaves 0xb2 as "?", so 0xb2 is the one
 * register with no reference value at all - and it is the one both siblings
 * write non-zero while we write zero. gchd is the more interesting witness
 * despite writing different values: it selects BOTH 0xb2 and 0xb5 from the
 * INPUT RESOLUTION (configure_hdmi.cpp: 0xb2 = c4/cc/cf and 0xb5 = d0/cc/cc
 * for 1080 / 720 / SD), where ours are two fixed constants. Structure
 * transfers even when values do not.
 *
 * Defaults are the current hard-coded values, so leaving these unset changes
 * nothing. Sweep ONE at a time (method rule 2), with fw=5, win_seq=0 and
 * vic_b0=0x21 - the only combination known to reach the splash.
 */
unsigned int mz0380_mst_b1 = 0xc0;
module_param_named(mst_b1, mz0380_mst_b1, uint, 0644);
MODULE_PARM_DESC(mst_b1,
		 "M126: MST3367 BANK0 0xb1 output config (def:0xc0 = ours+Windows; hdcapm 0xe0, gchd 0x0c)");

unsigned int mz0380_mst_b2;
module_param_named(mst_b2, mz0380_mst_b2, uint, 0644);
MODULE_PARM_DESC(mst_b2,
		 "M126: MST3367 BANK0 0xb2 output config (def:0x00 = ours; hdcapm 0x08, gchd 0xc4 for 1080p)");

/*
 * M130: MST3367 BANK0 0x92 - the CSC control byte.
 *
 * The 31-byte table we inherit from hdcapm spans 0x92..0xB0, and only the
 * first byte is not a coefficient:
 *
 *   0x92        0x40                 <- this byte: control
 *   0x93..0x98  M11 M12 M13          (2 bytes each, big-endian)
 *   0x99..0x9E  M21 M22 M23
 *   0x9F..0xA4  M31 M32 M33
 *   0xA5..0xAA  A1  A2  A3           (offsets)
 *   0xAB..0xB0  15 95 05 20 C0 08    (colour range + output stage; 0xB0 is
 *                                     rewritten straight afterwards)
 *
 * Why it matters (M129/M130): the captured frame's luma is perfect and its
 * chroma is 96% ANTI-correlated with luma - the signature of a matrix
 * subtracting luma from channels that are already luma-free. HDMI YCbCr 4:4:4
 * puts Cb on the blue channel, Y on green and Cr on red, so feeding it to an
 * RGB->YCbCr matrix gives Cb_out = -0.291*Y + ..., exactly what we measure.
 * Inverting that model on the captured frame turns the picture from
 * magenta/green mush into a coherent image, which confirms the mechanism.
 *
 * hdcapm writes this table unconditionally. Its MST3367_HdmiGetPacketColor()
 * reads the input colour space from BANK2 0x48 bits 6:5 and caches it in
 * regb2r48_cached - and then never uses it to pick a matrix. That is fine on
 * hdcapm's board, whose EDID makes sources send RGB. We push no EDID at all
 * (M127) and this source picked YUV444, which our own link log reports on
 * every run: "48=d2 ... input colorspace YUV444".
 *
 * With YCbCr in and YCbCr 4:2:2 out over BT1120, no conversion is wanted. If
 * 0x40 is the CSC enable, 0 should bypass it and the chroma should come out
 * right. That is one spawn and it is the cheapest thing that could work; the
 * fixed-point format of the coefficients has NOT been cracked, so do not try
 * to hand-write a matrix yet.
 */
unsigned int mz0380_mst_csc_ctl = MZ0380_MST_CSC_CTL_AUTO;
module_param_named(mst_csc_ctl, mz0380_mst_csc_ctl, uint, 0644);
MODULE_PARM_DESC(mst_csc_ctl,
		 "M130: MST3367 BANK0 0x92, the CSC control byte. Default AUTO (0xffffffff) = pick from the detected input colorspace: 0x00 (no conversion) for YUV422/YUV444, 0x40 (hdcapm's RGB->YCbCr) for RGB. Any other value forces that byte");

unsigned int mz0380_mst_b5 = 0x0c;
module_param_named(mst_b5, mz0380_mst_b5, uint, 0644);
MODULE_PARM_DESC(mst_b5,
		 "M126: MST3367 BANK0 0xb5 (def:0x0c = ours; gchd sends 0xd0 for 1080p, 0xcc otherwise)");

/*
 * M126: gchd commits 0xb0 in TWO writes - the configuration value with bit0
 * (embedded sync) CLEAR, then the same value with bit0 SET as the very last
 * register write of the whole HDMI bring-up (0xe8 then 0xe9). We write 0xb0
 * once, with bit0 already set, inside the 0xab bit7 freeze bracket.
 *
 * This is NOT the same experiment as M96, which measured 0x20 as a terminal
 * value and got nothing written. If bit0 is an output enable rather than a
 * mode select, the config must land before it is asserted, and a one-shot
 * write with bit0 already high never gives the retimer a clean edge.
 */
bool mz0380_mst_b0_late;
module_param_named(mst_b0_late, mz0380_mst_b0_late, bool, 0644);
MODULE_PARM_DESC(mst_b0_late,
		 "M126: commit 0xb0 as (value & ~1) inside the freeze, then set bit0 after unfreezing - gchd's order (def:0)");

/*
 * M75: which opcode programs the encoder's DMA destination buffers.
 *
 * We have always sent 0x02. The Windows driver builds the SAME 12-word command
 * (doorbell, opcode, channel, stride, then {hi,lo} address pairs) with opcodes
 * 0x04 and 0x05 - win64.txt 0x140279219 and 0x140279663, both with r8d=0xc
 * words and the payload pulled from an address array in dword pairs.
 *
 * 0x02 demonstrably works for the card's synthetic raw-NV12 path, so it does
 * reach a channel; but the H.264 bitstream may DMA from a different channel
 * whose addresses 0x02 never programs, which would leave the encoder running
 * with no destination and the host buffers untouched - exactly what the poison
 * scan reports (0/1024 pages) while the receiver holds a clean lock.
 */
unsigned int mz0380_set_buf_opcode = 0x02;
module_param_named(set_buf_opcode, mz0380_set_buf_opcode, uint, 0444);
MODULE_PARM_DESC(set_buf_opcode,
		 "M75: opcode that programs encoder DMA buffers - 2 (def), or 4/5/8 as the Windows driver uses");

/*
 * M76: SET_VIC bytes 24..27 ("input_frame_width/height") are the ONLY host
 * input that reaches the VIC's own width register. video_capture_mgr patches
 * them straight into the cfg lines "input frame width"/"input frame height"
 * (vcm FUN_0000a290, args 9 and 10), separately from the capture width, and
 * libvideocap writes that value to VIC channel register +0x68. vpl_vic's ISR
 * compares the incoming line against it and reports
 * "(CCIR or width(%lu) chck fail)" on a mismatch - one of the three status
 * bits behind "No signal !!".
 *
 * If the MST3367 is emitting 8-bit double-rate samples rather than true
 * 16-bit BT1120, the VIC counts 3840 samples per line where we declared 1920.
 * This is the only way to test that from the host without a scope. 0 keeps
 * the detected geometry.
 */
/*
 * M76: how many bytes of the card's /mnt/flash/PIC_ENC /proc/mz0380-cardlog
 * pages back. One mailbox round-trip per 16 bytes, so keep it small until the
 * transport is proven.
 */
unsigned int mz0380_cardlog_bytes = 256;
bool mz0380_cardlog_probe_enabled;
module_param_named(cardlog_probe, mz0380_cardlog_probe_enabled, bool, 0644);
MODULE_PARM_DESC(cardlog_probe,
		 "M76: WRITES 16 bytes to the card's /mnt/flash/PIC_ENC and reads them back, to prove vcm services op 0x6e (def:0)");
module_param_named(cardlog_bytes, mz0380_cardlog_bytes, uint, 0644);
MODULE_PARM_DESC(cardlog_bytes,
		 "M76: bytes of the card's PIC_ENC to page via /proc/mz0380-cardlog (def:256, 16 B per mailbox command)");

unsigned int mz0380_vic_in_w;
module_param_named(vic_in_w, mz0380_vic_in_w, uint, 0644);
MODULE_PARM_DESC(vic_in_w,
		 "M76: override SET_VIC bytes 24..25 input_frame_width (0=detected; try 3840 for 8-bit double-rate)");

unsigned int mz0380_vic_in_h;
module_param_named(vic_in_h, mz0380_vic_in_h, uint, 0644);
MODULE_PARM_DESC(vic_in_h,
		 "M76: override SET_VIC bytes 26..27 input_frame_height (0=detected)");

/*
 * SET_VIC byte7: the capture INPUT FORMAT enum (1=8-bits Raw, 2=CCIR656i,
 * 3=CCIR656p, 4=Bayer, 5=16-bits Raw, 6=BT1120p, 7=BT1120i), from the SDK
 * capture config. We derive 6/7 from the detected scan.
 *
 * M103 briefly reclassified this as the interlace flag, on the strength of the
 * card's own printf calling it "interlace" and M71's disassembly of the op-41
 * handler. M104 settled it on hardware and the enum reading won: 3, 6 and 7 all
 * reach the card's NOSG splash and are indistinguishable, while 0 - not in the
 * enum - produces nothing at all, which is VideoCap failing to open and
 * tinyvenc exiting before it can draw. The printf label is loose, not wrong:
 * 6 vs 7 is BT1120p vs BT1120i.
 *
 * The same run PROVED the byte is consumed at all, which three
 * indistinguishable values never could.
 *
 * MZ0380_VIC_IN_FMT_AUTO is the "derive it" sentinel. It exists because the old
 * `vic_in_fmt ?: derived` idiom could not express 0, so this field's own sweep
 * knob could not reach part of its own range - which is why the one informative
 * value went untried for so long. The derived default is unchanged.
 */
unsigned int mz0380_vic_in_fmt = MZ0380_VIC_IN_FMT_AUTO;
module_param_named(vic_in_fmt, mz0380_vic_in_fmt, uint, 0644);
MODULE_PARM_DESC(vic_in_fmt,
		 "SET_VIC byte7 input format (2=CCIR656i, 3=CCIR656p, 6=BT1120p, 7=BT1120i). Unset = derive 6/7 from the scan. M104: 0 is NOT in the enum and stops the card writing anything - it is the proof the byte is consumed, not a usable setting");

bool mz0380_signal_confirm;
module_param_named(signal_confirm, mz0380_signal_confirm, bool, 0644);
MODULE_PARM_DESC(signal_confirm,
		 "M62: require two agreeing measurement passes per mode detect (def:0 - a short-burst source cannot supply the window)");

bool mz0380_aic_every_frame;
module_param_named(aic_every_frame, mz0380_aic_every_frame, bool, 0644);
MODULE_PARM_DESC(aic_every_frame,
		 "M60: re-send SET_AIC(on=1) on every encoder spawn instead of once per session (def:0)");

unsigned int mz0380_aic_channels = 2;
module_param_named(aic_channels, mz0380_aic_channels, uint, 0644);
MODULE_PARM_DESC(aic_channels, "M33: SET_AIC channel_num (def:2)");

unsigned int mz0380_aic_bits = 16;
module_param_named(aic_bits, mz0380_aic_bits, uint, 0644);
MODULE_PARM_DESC(aic_bits, "M33: SET_AIC bits per sample (def:16)");

unsigned int mz0380_aic_freq = 48000;
module_param_named(aic_freq, mz0380_aic_freq, uint, 0644);
MODULE_PARM_DESC(aic_freq, "M33: SET_AIC sample rate (def:48000)");

unsigned int mz0380_aic_period_frames = 256;
module_param_named(aic_period_frames, mz0380_aic_period_frames, uint, 0644);
MODULE_PARM_DESC(aic_period_frames,
		 "SET_AIC frames per period (def:256; Windows/card default, 256x4=1024-frame mover)");

unsigned int mz0380_aic_periods = 4;
module_param_named(aic_periods, mz0380_aic_periods, uint, 0644);
MODULE_PARM_DESC(aic_periods, "M33: SET_AIC period_num_of_buffer (def:4)");

/*
 * M82: SET_AIC byte17. ep.ko stores it as G[0x63c] ("aic_int_mode") and the
 * Windows driver logs aic_int_mode=1 in every trace; we have always sent 0.
 * Its value is board-derived there and resolves to 1 for board id 0xFA.
 */
unsigned int mz0380_aic_int_mode = 1;
module_param_named(aic_int_mode, mz0380_aic_int_mode, uint, 0644);
MODULE_PARM_DESC(aic_int_mode, "M82: SET_AIC byte17 aic_int_mode (def:1, what Windows sends)");
