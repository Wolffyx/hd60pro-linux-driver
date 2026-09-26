# NEXT SESSION START

_Last updated 2026-09-01. Full history in **RE_FINDINGS.md**. This file is the
handoff only. Everything below was verified on hardware unless it says
otherwise._

## State (2026-09-01): raw capture works and presents as a camera

1920x1080 uncompressed through V4L2, ~50-60 fps, no module parameters needed.
The node enumerates `YU12`, `NV12`, `YV12`, `H264` in that order and is named
`HD60 Pro HDMI capture`.

### Closed this session, confirmed on hardware

| | defect | how it was found |
|---|---|---|
| M229 | card CLEARS a slot before filling it, so the sentinel test fired on the clear | scanning delivered bytes - the tail read 0x01, a value no poison here uses |
| M231 | raw sessions starved the liveness stamp, so the monitor re-armed HDMI mid-capture every 1.5s | counter watch with a working H.264 capture in the same trace for contrast |
| M239 | the tear detector's poison was copied into **every** delivered frame | predicting the pixel (960,674) from the sentinel offsets, then measuring 600/600 |
| M240 | H.264 enumerated first, node named after a codec | reading ENUM_FMT against what camera apps actually do |
| M241 | only I420 offered | NV12 and YV12 are free re-orderings of it; NV12 confirmed rendering |
| M242 | 45% "duplicate frames" | whole-frame fingerprints at two rates - **not a defect**, the source is 30 fps on a 60 Hz link |
| M243 | the stepping brightness | the same camera through a USB capture card steps identically - **it is the camera's auto exposure**, not this driver |
| M244 | `v4l2-compliance` had regressed to 142/6 | M240/M241 added NV12/YV12 to `ENUM_FMT` and not to `ENUM_FRAMESIZES` - back to **148/148** |
| M245 | whole-driver review: 12 defects + cleanup | TRY_FMT mutating live state, unguarded S_FMT, unbind use-after-free, world-readable hardware /proc files, controls that wrote BAR5 and did nothing - compliance 48/48 with 0 warnings, unbind/controls/capture checks all PASS; suspend/resume and `.shutdown` untested |
| M246 | HDMI audio: op 0x03 window, EVENT[19:16] + token 0x4c, SET_AIC bytes 4/5 corrected | transport proven on hardware (~47 x 4 KiB/s, ALSA delivers 48 kHz stereo); content silent because the source has no audio. Opt-in: `EXTRA="enable_audio=1"` |

### Retracted or refuted, and why - read before re-proposing any of them

- **M228** deferral: its own counter said 1 in 1200. A slot the card cleared
  stays "landed" forever, so the second observation was always already true.
- **M234** full-range labelling: rested on 4.2% of luma above 235, measured
  while the picture was drifting bright. A later capture read 0.001%.
- **M237** CSC staleness: real asymmetry, but 0x48 reads `d2` (YUV444) at both
  50 and 60 Hz, so it cannot explain the brightness.

The pattern in all three: a number measured while another defect was active, or
a fix shipped before its own falsifier was read. **Put a counter in every fix
and read it before believing the fix.**

## Open, in the order I would take them

0. **M246 audio - verify with a source that has audio.** Cold boot first
   (every audio-enabled load leaves another capture app running on the card,
   and STOP does not end it), then `sudo scripts/mz0380-m246-audio-test.sh`
   with a PC or console output playing sound. Transport is proven; only the
   sample content (channel order, endianness) is unverified. Also worth
   adding: silence while the card is not producing, so an application that
   opens the ALSA device before any video capture does not hit ALSA's 10 s
   timeout.
0b. **M245 leftovers.** Compliance, unbind, controls and the second-process
   capture all passed on hardware. Not yet run: suspend/resume (untested code
   on the resume path) and `.shutdown` - a warm reboot after a capture is the
   cheap test, and a clean next boot with no IOMMU faults is the pass. The
   power_present fix made after the first run is verified.
1. **M238, one measurement short of closed.** The chroma test is live and does
   fire - M244 added a probe count and a best-match high-water mark, and across
   five captures it ran up to 660k times and rejected 60 to 420 slots. Every
   rejection is an exact 32/32 samples at the clear value, while real content
   never got past 16/32 (a monitor showing black reached 14/32), so these look
   like genuine mid-fill catches rather than the documented false negative.
   What is missing is timing: the counters are cumulative and every long run so
   far contained a colourless stretch. `mz0380-m243-brightness-ab.sh pcie` now
   prints the seconds at which `rawnochroma` moved, which reads against the
   scan's per-window luma. One lit, never-dark run settles it.
2. **M232, with a new suspect.** Still not reproduced deliberately, but M244
   found that something on the desktop - WirePlumber probes every new V4L2 node
   on udev add - opens the node the moment it appears, and in one run streamed
   2591 H.264 frames to a consumer this project never started. Two consumers
   contending is exactly what "pipeline running, VB2 detached, 20 attachments"
   looks like from inside the driver. `scripts/mz0380-node-holders.sh` names
   the holders and both hardware scripts call it before every capture phase.
   Try reproducing M232 by opening a second consumer on purpose.
3. **The unexplained `S_DV_TIMINGS` failure.** Compliance failed
   `v4l2-test-io-config.cpp(239)` once and passed on the next run. Nothing in
   the M244 change touches DV timings; the failing run had a foreign consumer
   on the node and the clean one did not. Not diagnosed, not claimed fixed.
4. ~~**Duplicate frames.**~~ **CLOSED (M242): not a defect.** Whole-frame
   fingerprints at two sampling rates both imply ~30 fps of real content on a
   60 Hz link. `raw_dup_content` is a source-rate indicator, not an error
   counter.
5. ~~**Brightness.**~~ **CLOSED (M243): the camera.** The same camera through a
   UGREEN 25173 USB capture card steps identically - +51 luma vs +57, largest
   step 43 vs 52, median move 0.07 on both. Do not read the two RANGE VERDICTs
   as a difference between the cards: the USB leg arrives through ffmpeg's
   yuvj420p conversion, which clamps to 16..235 whatever the camera sends.
   M234 stays retracted.
6. ~~**v4l2-compliance.**~~ **CLEAN (M244): 148/148, 5 documented warnings.**
   Re-run it after any ABI change - it costs no spawns and it is the only thing
   in this tree that notices when an ENUM_FMT addition is not carried through
   to the rest of the enumeration.

## Running it

`modprobe` needs the module installed for the running kernel. `mz0380-live.sh`
builds and insmods from the tree instead, and keeps the spawn tally, which is
why it is worth using:

```bash
sudo env VICFW=7 H264PROBE=1 POLLDRAIN=0 WINSEQ=1 OP6=1 POSTMASK=0 FASTKILL=0 H264DIVISOR=0 PERSIST=1 ./mz0380-live.sh load
```

Every knob there equals the driver default (M216), so it is verbose rather than
wrong. `sudo ./mz0380-live.sh unload` when done - that is what clears the
root-owned objects that otherwise break the next plain `make`.

**The node number is not stable** - a USB camera can take video0. Resolve it by
name:

```bash
ffplay -f v4l2 -input_format yuv420p -video_size 1920x1080 "$(grep -l 'HD60 Pro HDMI capture' /sys/class/video4linux/video*/name | sed 's|/sys/class/video4linux/|/dev/|; s|/name||')"
```

**OBS does not offer `YU12` in its Video Format dropdown**, so an OBS session
can silently test the encoded path; two hardware runs were spent that way.
Since M240 raw is the default, but confirm with `pixelformat` in
`/proc/mz0380-state` before trusting any raw result - the `raw frames`,
`raw fills` and `raw repeats` lines only print when raw is live.

## Budget - READ BEFORE ANY HARDWARE RUN

The card wedges somewhere in **8-18 encoder spawns per power cycle**, and a
wedge costs a mains-off cold boot. `scripts/mz0380-spawns.sh` keeps the tally
in `/run`, so a reboot resets it automatically.

One power cycle reached **18 spawns with no wedge** under the current
`vic_fast_kill=0` default, which answers M157: the 8-18 band was measured on a
configuration that is no longer the default. Treat 18 as observed-safe-once,
not a new ceiling.

Every OBS reopen is a spawn. `ffplay` is the cheaper consumer for testing, and
an OBS retry loop can burn several spawns in seconds.

## Deliberately NOT done

**Zero-copy delivery.** The 3.1 MB memcpy per frame is 186 MB/s and the
largest cost in the driver, but the four raw slots are DMA targets programmed
once by `SET_BUF` at probe. Eliminating the copy means re-pointing the card at
each vb2 buffer per frame - a mailbox command in the delivery path, on a card
that wedges within 8-18 encoder spawns. The copy is the safe half of that
trade. Scope it properly if it is ever wanted; do not slip it in beside other
changes.

**Pruning the 146 module parameters.** Most are RE-era probes, but which ones
are still reached for is the operator's knowledge, not the code's. Mark
deprecated rather than delete.

## Tools worth keeping

- `scripts/mz0380-m226-blackframe-scan.py` - reads delivered bytes. Blank and
  partial frames, fill boundary, luma/chroma range, brightness drift vs steps,
  sentinel poison, whole-frame duplicates. Touches no mailbox.
- `scripts/mz0380-m230-flicker-watch.sh` - counter watch, prints only on
  change. Polls at 1s since M236; do NOT raise `procfs_verbosity` to 3 while
  capturing, that costs five mailbox commands per read and froze the desktop.

- `scripts/mz0380-m238-chroma-test.sh` - loads, starts the counter watch
  BEFORE any consumer opens (the window M232's two existing traces both lack),
  then captures a phase per scene. Prompts go to the terminal, so it can be
  piped without looking frozen; `PAUSE=<seconds>` makes it unattended.
- `scripts/mz0380-m243-brightness-ab.sh` - the same scan on two capture paths.
  `pcie` costs one spawn and runs the counter watch alongside; `usb /dev/videoN`
  costs none. Pins the capture geometry, because the first USB leg took the
  device's 720p default, was sliced as 1080p, and produced a full set of
  confident and entirely false verdicts.
- `scripts/mz0380-node-holders.sh` - who else has the node open. Root to see
  other users' descriptors.

Both were what closed M229, M231 and M239. Reading the code produced five wrong
theories; measuring what arrives produced every right one. M243 extends that:
the theories that survived were the ones checked against a SECOND capture path,
and the ones that died were measured on ours alone.

## What this session built

- **M213/M214** (static, tinyvenc7): the `op 0x02` transfer length is
  `ALIGN16(W)*H*3/2`; the famous 16 bytes were the stub the card writes for
  every frame its preview scheduler did not select, and `post_mask` bit 0 is
  what populates that selection bitmap. Nothing was ever broken.
- **M215** (hardware): all four op02 slots came back with full `0x2f7600`
  frames. `op 0x08` is never written.
- **M216**: the validated configuration is the driver's default -
  `sudo modprobe mz0380` works with no parameters. `make dkms-install`,
  `make install`, `/etc/modprobe.d/mz0380.conf`, `MODULE_VERSION`.
- **M217/M218**: raw delivered to V4L2; both `H264` and `YU12` enumerated, the
  application chooses with `S_FMT`, and `post_mask` bit 0 is forced on whenever
  raw is live so no parameter is needed.
- **M219**: full-rate raw measured at ~50 fps, link-limited.
- **M223**: `S_FMT` coerces geometry to the live source - the card has no
  scaler, and accepting 1280x720 previously wedged the node into refusing every
  STREAMON with no way back.
- Repo reorganised into `src/` and `scripts/` with `mz0380-live.sh` at the root;
  GPL-2.0-or-later with `COPYING` and SPDX tags; sc0710 removed from the build
  and from `src/`.
- `scripts/mz0380-build-check.sh` - builds in an empty tree and reports **make's
  exit status**. It exists because a check that looked for `mz0380.ko` passed on
  a stale file and reported two compile failures as successes.

### What the three bounded runs established (2026-08-27)

- The producer needs the encoder tail **and** `op 0x06`. Tail alone: nothing.
  op06 alone: nothing. Both: 209 completions. Logs
  `docs/m209-raw-only-2026-08-27.log`, `docs/m210-enc-tail-2026-08-27.log`,
  `docs/m210b-tail-plus-op6-2026-08-27.log`.
- Only the `op 0x02` bank rotates - tokens `a5a5a5a0..a3`, `slots_seen` capped
  at `0x0f`, all four `op 0x08` slots poison-intact in every run. The M209
  static claim of an eight-slot `token % 8` ring is not what this firmware does
  on the Linux sequence.
- `op 0x04` is irrelevant to the raw ring: M211 ran the operator's working
  configuration with the encoded path fully alive (60 frames, 891,529 bytes,
  clean IDR) and the raw banks behaved identically.

### Known-good load, for reference

```
sudo env VICFW=7 H264PROBE=1 POLLDRAIN=0 WINSEQ=1 OP6=1 POSTMASK=0 \
         FASTKILL=0 H264DIVISOR=0 PERSIST=1 ./mz0380-live.sh load
```

`mz0380-live.sh` defaults are NOT this configuration - `vic_fw` defaults to 5,
`win_seq` to 0, `poll_drain_ms` to 20 - so every harness names the full knob set
explicitly. An unnamed knob in this tree is a wrong knob.

One measurement taken but not concluded: 209 raw completions at 16.3 ms apart
(~61/s) against a receiver reading of 30 fps and 60 encoded frames in roughly
3 s. Those do not agree, and M105 records a `hper=337 vper=299` artifact during
re-lock that is exactly what this camera reads. The possibility that the source
is 60 Hz and the `vperiod` decode is halved is open; measure it deliberately.

The product architecture remains unchanged: installed PCI modalias autoload,
generic V4L2 registration at probe, application-owned format negotiation, and
raw or H.264 setup only at STREAMON. There is no mandatory loopback and no
kernel H.264 decoder. Windows files remain reverse-engineering references, not
Linux firmware or runtime dependencies. Do not advertise YUYV/NV12/YV12 until a
bounded run shows repeated full frames.

## Current development build: hybrid NO SIGNAL recovery (host-only follow-up)

The persistent card lifecycle is now hardware-validated. One fresh load used
three OBS attachments, two same-mode HDMI recoveries, and only **one SET_VIC**.
Detached access units continued to rotate and ACK, every reattachment/reconnect
returned at a clean IDR, and there were zero mailbox timeouts. Module removal
then logged exactly one successful final boundary:

```
final pipeline stop: STOP_STREAMING(all channels) ret=0 after 3 userspace
attachment(s) and 1 SET_VIC spawn(s)
```

The next source implements the missing user-visible NO SIGNAL state. M114 had
already proved that an unlocked card produces no frame, so the old plan to
cache a card-generated splash was impossible. The new path embeds a
self-contained 1920x1080 High Profile level-4.2 Annex-B SPS/PPS/IDR with a NO
SIGNAL picture and replays it from host memory at two fps. The first synthetic
picture was rejected on hardware as visibly different from the original card
splash. It has now been replaced with the exact firmware `NOSG_LOGO_Y` asset:
320x240, centred at (800,420), on the original Y=0x11/U=V=0x80 canvas. The
generator extracts it from the user's local tinyvenc5 ELF and requires the
hardware-recorded SHA-256 before encoding; the vendor bitmap is not duplicated
inside the generator.

The first M193 hardware run exposed one more discriminator. The MST3367
reported a coherent 1920x1080p30 timing and R55=0x7f, so STREAMON treated the
input as live and sent one SET_VIC. The card then produced zero H.264 access
units: status remained `no signal: inactive`, all H.264 counters stayed zero,
and OBS was black. A receiver lock byte—even backed by coherent timing
registers—is therefore not sufficient proof that the encoded producer is
alive.

The current follow-up combines both observations without polling the firmware
mailbox during healthy capture. MST3367 register access is itself proxied by
mailbox commands, and the first M193 run scheduled those commands every 500 ms
starting immediately after START_STREAMING. Earlier hardware work already
showed that receiver mailbox access during capture perturbs delivery; the full
log then rolled out of the kernel ring because of the repeated command traffic.

An explicit receiver unlock still enters the zero-spawn placeholder path
immediately. Once a configured pipeline produces no H.264 access unit for
`hotplug_stall_ms` (default 1500 ms), the worker begins receiver polling, the
host placeholder starts, and AUTO_POSITION is forced back into acquisition
even when R55 remains asserted. Healthy producer activity schedules only a
timer and performs no receiver transaction. Placeholder-only capture and
persistent reattachment still poll/validate because no live stream is being
exposed. Recovery sends no additional SET_VIC and cannot create an encoder
respawn loop.

With HDMI absent, STREAMON now attaches VB2 immediately to that placeholder
and sends no card command: the pipeline remains stopped and `SET_VIC` remains
zero. A 500 ms MST3367 lock-byte retry runs while no card pipeline exists; it
re-arms acquisition and, when full lock appears, performs one coherent timing
measurement and starts the card pipeline once. The placeholder remains visible
until a live IDR; no standalone live SPS/PPS or dependent picture can toggle
the decoder early.

After HDMI loss, the existing card pipeline remains alive and its DMA
completions continue to be drained/ACKed, while the placeholder is presented.
Same-mode lock switches back at a clean live IDR with no new command. A changed
mode leaves the placeholder active, queues one source-change event, and defers
the already-controlled single replacement to the next attachment. Reattachment
also validates receiver state, covering a cable/source change made while OBS
was closed.

`stream_without_signal=1` is now the default for persistent H.264. New knobs are
`signal_monitor_ms=500` and `no_signal_fps=2`; `SIGMON` and `NOSIGFPS` expose
them through `mz0380-live.sh`. The former is now the retry interval only while
placeholder-only, producer-stalled, or validating reattachment. Status reports placeholder activity, delivered
placeholder IDRs, cadence misses, and deferred-start/replacement state. The
Linux 7.2 deprecated `system_wq` warning is removed through a feature-probed
`system_dfl_wq` selection.

The M193 integrated source passed clang/lld `W=1` on Linux 7.2 and 6.18 LTS,
`git diff --check`, shell syntax, module-parameter inspection, and byte-exact
extraction plus repeated decode of the embedded H.264 IDR. The runnable module
was rebuilt for 7.2 after the cross-build. The hybrid receiver-lock/producer-
silence follow-up also passes `W=1` on both 6.18 LTS and 7.2 after gating
healthy-stream mailbox access; the final module targets 7.2. Hardware
validation is pending.

The exact-vendor-image replacement is host-validated: its source asset hashes
to `dfce4efd5139298f544d23473f85a42fb7115a3c5e4ba65b71c070d59883a30b`,
the generated access unit contains AUD/SPS/PPS/IDR and decodes as 1920x1080
High Profile level 4.2. The perceived lag is explained by the 30 fps HDMI
input: tinyvenc7 divisor 2 emits at most about 15 encoded fps. A 60 fps source
is required for the already validated approximately 30 encoded fps.

The live loader's cleanup regression is fixed: its internal pre-load unload
preserves the freshly built `mz0380.ko`; only an explicit `unload` removes
sudo-owned generated outputs. The runnable module was rebuilt for Linux 7.2
after this correction. The failed ENOENT load did not insert the module or
spend a SET_VIC spawn.

Hardware subsequently validated that corrected loader. After two manual HPD
pulses, the pipeline remained attached and live with one SET_VIC across three
userspace attachments, no drops, and no recovery activity. Delivery advanced
by 66 access units in four seconds, consistent with the predicted ~15 fps for
the connected 1080p30 source. The pulses did not destabilize the encoder, but
were not issued during an actual connected-but-unlocked outage.

The subsequent cable round-trip completes the primary validation. OBS showed
the exact original spinner/`NO SIGNAL` artwork throughout the unplugged
interval. The driver delivered 56 placeholder IDRs with zero cadence misses,
recognized the same 1080p30 mode on reconnect, held the splash until a clean
SPS/PPS-bearing live IDR, and then resumed continuous live delivery. The
pipeline stayed running with exactly one SET_VIC and no dropped frames.

### Remaining hardware validation

The primary OBS lifecycle is complete: repeated attachment, unplug, exact
vendor placeholder, same-mode reconnect, clean-IDR handoff, and persistent
one-spawn delivery are all proven. Remaining discriminators are narrower:

1. On a fresh load begun with HDMI already absent, confirm placeholder-only
   STREAMON keeps the card pipeline stopped and spends zero SET_VIC.
2. Return with a genuinely changed HDMI mode and validate the controlled single
   replacement at the next attachment.
3. Test one HPD pulse only during a real connected-but-unlocked outage before
   considering any automated HPD recovery.
4. At the end of the current run, close OBS, unload once, and retain the single
   final pipeline STOP log.

True 60-fps H.264 delivery is now validated. M205 ran the M204
`H264DIVISOR=0` schedule at a confirmed 1920x1080p60 input. Both SET_ENC calls
landed with `skip=0, avg=0`; status advanced from 909 to 1663 delivered access
units while the pipeline stayed on one SET_VIC, and the final counters reached
4,339 frame events over approximately 73 seconds. That is about 59--60 fps.
There were zero FIFO drops, zero command timeouts, and a clean final STOP.

The nine H.264 drops were short `no queued vb2 buffer` intervals and did not
limit the producer. Value 0 is now the module and recommended-profile default;
value 2 remains the proven 30-fps fallback.

Windows presents this device as a generally usable camera and the captured
DirectShow enumeration proves both pins offer YUY2/YV12/NV12 as well as H.264;
YUY2 is the current/default format. Linux must match this with native V4L2
`ENUM_FMT`/`S_FMT` negotiation. Module load should register the hardware, not
select H.264 through `h264_probe`. Configure the selected raw or encoded DMA
path only at STREAMON. A loopback decoder is optional, not the target design,
and an H.264 decoder must not be added to the kernel module.

The M200 independent raw-bank lead is still closed under the required
condition. SET_VIC started at confirmed 1920x1080p60; opcode-0x02 received only
16-byte records and opcode-0x08 remained untouched. Do not repeat that
H.264-selected RAWBANKS sequence; M209's raw-only test is a different start.

The normal persistent H.264 load is now:

```bash
sudo env \
  VICFW=7 H264PROBE=1 POLLDRAIN=0 \
  WINSEQ=1 OP6=1 POSTMASK=0 FASTKILL=0 \
  H264DIVISOR=0 PERSIST=1 \
  ./mz0380-live.sh load
```

Use exactly one OBS V4L2 source, keep buffering disabled, and avoid applying
Properties unnecessarily because a restart can spend another encoder spawn.

The first RAWBANKS run started its encoder at 1080p30, so it was not the needed
60-Hz discriminator. It found exactly 16 bytes in each opcode-0x02 buffer and
zero writes in every independent opcode-0x08 buffer. During reconnect the
receiver briefly measured 1080p60 and then returned to the pipeline's original
1080p30. That exposed a host state bug: the transient changed-mode replacement
remained latched and suppressed thousands of healthy H.264 units behind NO
SIGNAL even after the old mode returned. The card itself stayed responsive,
continued producing, reported no command timeouts, and stopped cleanly.

The current module cancels a pending replacement when later coherent timing
again matches the running pipeline, then resumes only at a clean IDR. First
validate that correction with the normal path (`RAWBANKS` omitted), reproducing
one unplug/reconnect without reopening OBS. If a transient mode appears, expect
`cancelled transient replacement`, followed by `NO SIGNAL presentation ended
at clean live IDR`, with SET_VIC still one. Only after that should another raw-
bank experiment be considered, and only with SET_VIC itself confirmed at
1080p60 from the start.

That normal-path validation now passes. SET_VIC began at 1080p60; reconnect
briefly measured 1080p30 twice, then returned to persistent 1080p60. The driver
cancelled the transient replacement and ended the placeholder 274 ms later at
a clean SPS/PPS-bearing IDR. Live state afterward was 1995 delivered, zero
dropped, placeholder inactive, recovery idle, and still one SET_VIC. That run
used the older divisor-2 profile. M205 later validated the all-frame profile at
approximately 60 fps from the same 60-Hz input class.

The reconnect log now proves the placeholder-to-live boundary itself works:
1080p30 was validated at timestamp 4988.568, a clean IDR arrived at 4990.893,
and the placeholder ended. About 153 live access units were then delivered in
11.8 seconds before R55 became unlocked again, consistent with the firmware's
divisor-2 limit on a 30 fps input and with the user's lag report. Divisor 1 is
not a remedy: its modulo predicate is unreachable and produces no H.264. Use a
60 fps HDMI mode to obtain the hardware-validated ~30 encoded fps.

The cable remained physically connected when R55 became unlocked at 5002.691,
so the second loss is a real source/receiver link flap. It eventually recovered
without a new SET_VIC at 5533.670 and switched on a clean IDR at 5536.199. Two
manual one-second HPD pulses were issued only after that recovery; they proved
HPD read-back and did not break live capture, but they did not prove HPD can
shorten an outage. Do not automate HPD until one pulse is tested during an
actual connected-but-unlocked interval.

Do not open/apply OBS Properties during this discriminator. An already-deaf
card remains a distinct PCI reset question; do not manufacture the 8--18 spawn
wedge to test it.

## Previous development build: HDMI hotplug recovery (host-only)

The latest source adds recovery for the reported HDMI cable round-trip failure.
The existing module has no live signal-change IRQ handling: the
`MZ0380_IRQ_SIGNAL_CHANGE` definition is unused, receiver timing queries are
correctly cached while video flows, and `AUTO_POSITION` was unconditionally
refused while `dev->streaming` remained true. Consequently an unplug could
stop H.264 forever while OBS held the last decoded picture.

The new delayed worker is driven by H.264 frame silence. It performs no
receiver I2C during healthy video. After `hotplug_stall_ms=1500` without an
encoded VCL access unit it enters recovery, permits MST3367 acquisition to be
re-armed, and retries stable lock detection every `hotplug_retry_ms=500`.
Transient loss and a same-mode return do not emit a V4L2 source-change event,
so OBS is not encouraged to STREAMOFF/STREAMON and consume another encoder
spawn. A genuinely changed returning timing emits the event and is handled by
the persistent pipeline's controlled next-attachment replacement above.

H.264 delivery now begins at a clean decoder boundary on both initial STREAMON
and reconnect. Parameter-set-only access units can pass, dependent pictures are
dropped, and the first IDR resumes delivery with `V4L2_BUF_FLAG_KEYFRAME`.
This is the second half of the initial black/smeared-picture fix: M188 removed
the post-START VB2 starvation window, while this change refuses to expose an
incomplete inter-frame reference chain.

The module builds cleanly against Linux 7.2.0-1-cachyos with clang/lld and
`W=1`; `git diff --check` and the shell syntax checks pass. It has not been
loaded on hardware. The observed power-cycle tally was already around nine
after OBS's cable/source retries, so power the slot fully off before testing
this build rather than spend another spawn in the known 8--18 wedge range.

On the next fresh-power test, open OBS once, confirm the initial
`clean H.264 IDR received` line, unplug HDMI for at least two seconds, and plug
the same camera back into the Elgato without closing/reopening the OBS source.
Expected logs are `entering HDMI reconnect recovery`, one or more recovery
attempts, a stable reconnect lock, then a clean IDR. `SET_VIC sent` must remain
exactly one across the cable round trip.

## Earlier development build: adaptive readiness/lock cache and core split

The first M188 hardware run validates the startup fix and the clean encoder
replacement path together. Loaded with `FASTKILL=0`, OBS used one `SET_VIC`
and reached **1482 H.264 access units delivered, zero dropped, zero command
timeouts** before the first status snapshot. The old regular startup drop
burst was absent. The input remained locked at 1920x1080p60 YUV444.

A later group of five drops was not startup loss. It followed a live receiver
mailbox read (`drained stale EVENT ... before command 0x1a`) and occupied five
regular encoder periods. OBS can issue `VIDIOC_QUERY_DV_TIMINGS` while its
capture is active; that ioctl was the remaining path which still measured the
MST3367 over serialized mailbox I2C while streaming. `ENUM_INPUT` already
returned cached lock state during capture for this reason. The development
source now applies the same rule to `QUERY_DV_TIMINGS`: while streaming it
returns the timings used to configure the current encoder and does not touch
the mailbox. This follow-up is host-build-checked but has not yet been loaded.

The newest host-only change removes the synchronous 500 ms wait and receiver
diagnostic after `START_STREAMING`. That wait ran inside the VB2 `STREAMON`
callback while the encoder was already producing at about 30 fps, preventing
OBS from recycling its completed buffers. In the latest hardware log, ten
access units were consequently dropped at regular approximately 33 ms spacing
through the exact timestamp of the `output stage [after START]` diagnostic.
The resulting hole in the first H.264 GOP explains the briefly black, smeared,
or blurry startup pictures. The pre-START diagnostic and CSC programming are
unchanged. This fix is build-checked but has not yet been loaded on hardware.

On the next single OBS open, expect no regular startup `has no queued vb2
buffer` burst and no initial corrupt pictures. Record status after several
seconds. A later isolated drop is separate from this deterministic startup
problem and should be preserved if it recurs.

Hardware validation passed on a fresh power cycle. The complete live-load
command took 13 seconds including the build and load/unload smoke test; the old
workflow took roughly 40 seconds because it always added a 25-second sleep.
CMD_INIT answered on attempt 1 after **6 ms**, firmware 1.11/version discovery
finished about 110 ms after probe began, and INTx 40 came up normally.

OBS opened one V4L2 user and spent exactly one `SET_VIC`. Only one
`MST3367 signal:` measurement was logged during negotiation, confirming that
the one-second successful-query cache removed the earlier duplicate burst. The
input was locked 1920x1080p60 YUV444. After the short userspace buffer-queueing
gap, status reached **615 H.264 access units delivered, 18 dropped, zero
timeouts**; the drop count had essentially stopped increasing while delivery
continued at the expected approximately 30 fps. This validates the readiness,
lock-cache, and first refactor build on hardware.

The next hardware run should use a fresh slot-power cycle. The development
build now removes the unconditional 25-second delay from `mz0380-live.sh`:
`insmod` already waits synchronously for PCI probe, and the script polls only
for the short sysfs/device-node publication race. `LOADWAIT=5` is the bounded
post-insmod node wait and normally exits immediately.

Card firmware readiness is now deadline-based. `card_ready_timeout_ms=15000`
retries CMD_INIT until the cold card answers, but exits on the first success;
the log reports attempts and elapsed milliseconds. This replaces the old fixed
ten-attempt/~6-second window, which could abandon a slower-flash-booting card.

Repeated successful DV-timings queries are cached for
`signal_query_cache_ms=1000`. OBS normally asks for the same timing several
times during one negotiation, and each MST3367 measurement costs serialized
mailbox I/O. The cache applies only while the receiver is currently marked
locked; setting it to zero restores a hardware read on every query. The
tradeoff is that a newly lost source can be reported up to one second late.

Both cleanup phases are complete and build-checked. `mz0380-core.c` is only 13
lines. MST3367 is divided into initialization (938 lines), diagnostics (653),
signal/output handling (744), and GPIO/bit-banged EDID tools (513). DMA is
divided into allocation/IRQ/event setup (836), stream sequencing (773),
draining/poll delivery (616), extent diagnostics/teardown (355), and no-SG
fallback capture (189). The largest normal implementation file left is
`mz0380-video.c` at 1043 lines, close to the target; every other driver `.c`
unit is 942 lines or less. The second-phase split is host-build-verified but the
currently loaded module is still the already hardware-validated first-phase
binary.

For the next run, record the new CMD_INIT timing line, elapsed load command
time, time from opening OBS to the first frame, and how many `MST3367 signal:`
lines appear during initial negotiation. Do not spend the run changing OBS
Properties.

## Latest cold-boot confirmation and 60 fps lead (2026-08-25)

The first run after removing mains power was healthy. It used the documented
`VICFW=7 H264PROBE=1 ... H264DIVISOR=2` command, found the card already running
firmware 1.11, and brought up legacy INTx normally. With OBS holding the only
V4L2 file descriptor, status showed a locked 1920x1080p60 input, **2915 H.264
access units delivered, 10 dropped, and zero command timeouts**. The ten drops
were one regular 33 ms-spaced no-vb2-buffer burst, not an encoder or HDMI
stall.

`SET_VIC sent: 3` means OBS opened/restarted V4L2 three times during this test.
It does not mean the card spontaneously respawned the encoder. Avoid reopening
or applying source Properties while measuring; each STREAMON is still one
encoder spawn.

The subsequent unload/reload test also behaved correctly. Unload committed the
first load's 3 spawns; the next load used 2 more, and the final unload committed
**5 total for the same physical power cycle**. Its current stream delivered 301
H.264 access units with zero drops and zero command timeouts. The new source was
actually **1920x1080p30 YUV422** (`hper=336`, `vper=299`, `B2:48=b0`), whereas
the earlier source was 1920x1080p60 YUV444 (`hper~=674`, `vper~=599`,
`B2:48=d2`). The encoder was also configured for 30 fps, so this run confirms
clean capture of the replacement 30 Hz source; it is not a 60 fps result.

That status snapshot already showed `SET_VIC sent: 2`. Because no before-gap
count was captured, it still cannot distinguish same-process relock from an OBS
STREAMOFF/STREAMON during the source move. Repeat only after resetting slot
power if that distinction is needed, recording status before disconnect,
during the gap, and after reconnect without opening Properties.

During a source change, the Linux preview froze on the last decoded frame while
no HDMI source was available, then changed and resumed once the replacement
device was connected. Windows instead displays its active **NO SIGNAL** image
during the gap. Treat these as two separate facts: Linux is missing Windows'
no-signal presentation, but the receiver/live path appears able to relock and
resume after a source swap. Confirm whether that recovery used the same encoder
process by checking that `SET_VIC sent` did not increase before unloading.

The saved Windows 1080p60 trace contains `fw=7`, `fps=60`, and two
`SET_ENC_PARAMS` calls. M204 corrects the earlier field name: tinyvenc7 calls
payload bytes 20/21 `skip`/`avg`; they are not the final QP bounds printed
later in the command. Opcode `0x32` is absent but irrelevant because the
already-forwarded SET_ENC handler generates its own schedule bitmap.

The concrete Windows/Linux difference is instead in the capture-buffer banks.
For this exact HD60 Pro board branch, Windows registers:

- opcode `0x02`: four distinct `0x466000`-byte buffers;
- opcode `0x08`: four *additional*, distinct `0x466000`-byte buffers;
- opcode `0x04`: four `0x34bd00`-byte buffers;
- opcode `0x05`: four `0x34bd00`-byte buffers.

Linux's opt-in M200 diagnostic reproduced the independent `0x02`/`0x08`
topology and tested it at a confirmed 1080p60 SET_VIC. M203 closed this lead:
opcode `0x02` wrote only 16 bytes per buffer and `0x08` wrote nothing.

### Historical 60 fps plan (completed by M205/M206)

Do not retry `H264DIVISOR=1`, opcode `0x32`, or the H.264-selected M203
RAWBANKS sequence. The bounded test at this checkpoint was `H264DIVISOR=0` on
the ordinary H.264 path. It cleared SET_ENC `skip` and `avg`, and M205/M206
subsequently validated the resulting all-frame schedule in hardware.

If unloading/reloading first, close OBS before `sudo ./mz0380-live.sh unload`.
That command commits the current load's spawn count before `rmmod`; a module
reload does **not** reset the card's physical spawn budget. Only removing slot
power does. Reopening OBS after the reload will spend at least the next spawn.

## Historical checkpoint: continuous H.264 works (2026-08-24 23:50; superseded by M205 for cadence)

Continuous HDMI H.264 capture now works. The older sections saying every route
is closed predate M177/M178 in `RE_FINDINGS.md`.

- tinyvenc7 writes encoded access units to Windows outbound window 1 (opcode
  `0x04`), not to the raw-preview buffer whose 16-byte cadence dominated the
  earlier investigation.
- Four dedicated 1 MiB buffers at IOVAs `0x500000000..0x800000000` receive a
  4 KiB transport header followed by Annex-B H.264. The driver parses it and
  delivers V4L2 H.264 continuously.
- A bounded test decoded 77 live 1920x1080 High Profile frames. OBS later
  received 910 frames with only 2 startup drops and displayed the live image.
- The measured 10-12 fps came from tinyvenc7's SET_ENC `skip=5` byte and its
  `(input counter % N) == 1` gate, hence 60/5 = 12. The driver defaults to the
  proven `skip=2` result (30 fps). M204 shows that zero selects a firmware-
  generated all-frame bitmap; only value one is intrinsically invalid.
- Hardware testing proved the new divisor produces 30 Hz (1975 OBS frames in
  69.3 seconds including startup).
- The corrected-cadence build is now hardware-verified. `v4l2-ctl
  --list-formats-ext` enumerated H.264 at exactly 30.000 fps for every size,
  and the OBS log negotiated `Framerate: 30.00 fps` with a 166666 us select
  timeout.
- OBS Linux V4L2 buffering defaults on. Unchecking **Use Buffering** made the
  preview visibly smoother/more responsive. In this actual run, applying the
  property stopped and restarted capture once, so it consumed a second
  SET_VIC spawn even though OBS itself was not restarted.
- Latest status after that change: signal locked at 1920x1080p60, 1111 H.264
  frames delivered, 10 dropped in the short restart gap, zero command
  timeouts, two SET_VIC spawns. The access-unit cadence remained 30 Hz.
- The machine/card is being fully powered off after this checkpoint. The next
  session therefore begins with a reset spawn budget; do not inherit a tally
  from this run.

Use:

```bash
sudo env VICFW=7 H264PROBE=1 POLLDRAIN=0 WINSEQ=1 OP6=1 \
  POSTMASK=0 FASTKILL=0 H264DIVISOR=2 ./mz0380-live.sh load
```

Keep exactly one OBS V4L2 source. A previous black preview was a duplicate
hidden source owning `/dev/video0` while the visible source failed with EBUSY.

### First test after powering back on

1. Run the load command above once.
2. Do not reopen the OBS source Properties if **Use Buffering** remains saved
   as off. OBS should start one 30 fps stream automatically.
3. Judge two things separately: motion fluidity (the current mode is 30 fps)
   and control-to-preview latency. If it is still delayed, preserve the stream
   and collect `sudo ./mz0380-live.sh status`; do not reconnect repeatedly.
4. When finished, close OBS and run `sudo ./mz0380-live.sh unload`.

### User requirement for the next session: restore 60 fps

Do **not** treat 30 fps as the card's final ceiling. What is proved is narrower:
tinyvenc7's non-zero **divisor mode** tops out at 30 fps because divisor 2 is
the fastest valid value and divisor 1 emits none. M204 now identifies zero as
the distinct all-frame bitmap mode and implements it as an opt-in test. The
next session should validate that exact mode once, not resume the older
opcode/raw-bank search.

Continuous capture, the older 30-fps fallback, and the low-latency OBS setting
were proved at this checkpoint. M205 later closed true 60 fps; residual
latency optimization remains separate.

## Where the driver is

**It works.** A plain `insmod ./mz0380.ko` with no arguments captures a real,
correctly-coloured 1080p frame, and `v4l2-compliance` passes 148/148.

| | |
|---|---|
| plain `insmod` captures | yes - 1920x1080 planar **I420**, 3110400 bytes |
| repeat capture | yes - 3 stills in a row, whole and all different (M171) |
| the one-frame bound | reported as end-of-stream, not a hang (M168) |
| `v4l2-compliance` | **148/148**, 5 warnings with a documented reason (M173) |
| non-1080p | never tested - correct by construction only (M174) |
| audio | not implemented |
| continuous video | see below |

```bash
sudo modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
sudo insmod ./mz0380.ko
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=1 --stream-to=/tmp/f.i420
ffplay -f rawvideo -pixel_format yuv420p -video_size 1920x1080 /tmp/f.i420
```

Expect **one frame, then a clean end-of-stream** (`DQBUF` returns `-EIO`).
STREAMOFF + STREAMON gets the next one. Do not respawn per frame to fake video:
every stream start forks an encoder and the card wedges somewhere in the
**8-18 spawn range per power cycle**. `scripts/mz0380-spawns.sh` has the running
total and needs no root.

## THE ONE THING TO RUN FIRST

```bash
sudo scripts/mz0380-m176-fw6-test.sh
```

One spawn. This is the open question and it is the answer to "how does Windows
get continuous capture out of this card".

**Windows never sends `fw=5`. This driver always has, and still defaults to it.**
M82 read it out of every `[CH00]` line of every Windows trace: the Windows
driver sends SET_VIC byte 6 as **6 or 7, picked by frame rate** - `6` at
1080p30/29.97, `7` at 1080p60.

`fw=6` does **not** select a different binary. Only `7 -> tinyvenc7` and
`8 -> tinyvenc8`; `6` falls through to **tinyvenc5, the binary we already run**.
What it changes is the card's capture config - `video_capture_mgr` writes
*output format* `2/YUY2` instead of `1/YV12` (M79). So Windows drives
tinyvenc5, the binary with the pixel path *and* the livelock, in a mode this
project has never once put it in.

The single run that tried it was **explicitly retro-invalidated** (RE_FINDINGS
~3536): it carried `out_fmt=0` and other changes, predates M129 (real video),
`fake_frame_off`, `post_mask=0` and the poll-drain, and ran on INTx.
RE_FINDINGS' own "genuinely untested" list has it at **number 1**.

**No prediction is attached.** There is no traced mechanism connecting an output
format to the `mma_already_start` livelock and the prior is low. It is the last
untested difference between our configuration and Windows' on the same binary,
and it costs one spawn.

If it delivers, the frame is **4147200 bytes of packed YUY2** (4:2:2), not
3110400 of planar I420 - view it as `yuyv422`. Both of the driver's size rules
would have thrown such a frame away in silence until M175 added
`expect_frame_bytes`, which the test passes for you.

## The one-frame bound, and an important correction

For `fw=5` and `fw=8` the bound is real and fully traced (M153/M158/M166):

1. the DMAC start ioctl `0xDE00` clears `free[profile]` on success;
2. the only thing that sets it back is the `0xDE01` wait, issued solely by
   `TK_MMA_WaitOneFrameComplete`;
3. that call is **unreachable** - `mma_already_start` is read nine times and
   never written in this build (M153, CFG-proven);
4. so the next push gets `-1` and **spins forever** in
   `MassMemAccess_StartDMAC`'s unbounded `sched_yield` loop, at `SCHED_FIFO`;
5. only process death (`Close`) returns the profile - which is why a respawn
   yields exactly one more frame.

That chain explains the one frame, the OBS freeze, `frame_events=0`, and why a
respawn was the only thing that ever helped.

**This file used to end that paragraph with "Stop looking for a knob. There
isn't one." That sentence was wrong twice in one session and it is worth
knowing why**, because it cost several turns before anyone checked:

- it was written when **two** encoder binaries were known. There are three.
  `tinyvenc8` was named once in RE_FINDINGS in passing, was spawnable by the
  existing `vic_fw=8` with no upload, and had **never been run**. M175 ran it.
- it made `fw=6` - the value Windows actually sends - look settled, when the
  only run that tried it had been retro-invalidated in this same file.

The bound is real. The closure was inherited rather than measured, and a
closure inherited from a document is not a measurement.

### The other TWO routes, also closed - M175 measured the third binary

**There were three encoder binaries, not two.** `tinyvenc8` sat in the card
image, named once in RE_FINDINGS in passing, spawnable by the existing `vic_fw=8`
with no upload - and had never been run. It has now been:

| | pixels | cadence | `frame_events` |
|---|---|---|---|
| `fw=5` tinyvenc5 | 1920x1080 whole frames | one per process, then livelock | 0 |
| `fw=7` tinyvenc7 | **16 bytes** | continuous 60 Hz, real IRQs | 1621 in 57 s |
| `fw=8` tinyvenc8 | **960x540** whole frames | one per process, then livelock | 0 |

tinyvenc8 produces real quarter-resolution video (NOT SPLASH, 213 distinct Y
values) and shares tinyvenc5's exact bound. So continuous streaming is closed on
this firmware **by measurement of every binary the card ships**, rather than by
having tested two of three - which is what this file used to claim.

Two of the three have pixels and no cadence; one has cadence and no pixels.
Do not re-derive this from the prose below - `sudo scripts/mz0380-m175-fw8-test.sh`
re-checks it for one spawn.

**What this does NOT close is `fw=6`**, which selects none of these three
paths' behaviour but tinyvenc5 in YUY2 mode - see the top of this file.

### The fw=7 route, in detail

`fw` selects which binary the card spawns - `7` runs `./tinyvenc7`, anything
else `./tinyvenc5` (M158). tinyvenc7 does **not** have the livelock and streams
beautifully:

| | `fw=5` | `fw=7` |
|---|---|---|
| cadence | 1 frame per process | continuous, 60 Hz |
| `frame_events` | **0**, always | **1621** in 57 s, `fifo_drops=0` |
| delivery | poll-drain scan | real completion interrupts |
| bytes/frame | **3110400** | 16 |

But tinyvenc7 sends a full frame only every Nth (a modulo, or a 128-bit schedule
bitmap), and both N and the bitmap are set **only** by mailbox opcode `0x32`.
This card's `ep.ko` forwards `rodata[0xa0+cmd]` bytes per opcode, and
`rodata[0xd2] = 0` - **opcode 0x32 is not forwarded at all** (M165, validated
against M127's twelve known lengths). N stays 0, the bitmap stays zero, and
every frame takes the 16-byte path. Nothing host-side changes that.

So: `fw=5` has the pixels and no cadence; `fw=7` has the cadence and no pixels.
Fixing either needs a different `ep.ko` or a different `tinyvenc5`, and the
standing rule forbids putting either on the card.

### What the driver does about it

`poll_drain_ms` now **defaults to 20** (M166). Under `fw=5` the card raises no
completion interrupt, so the poll-drain is the only delivery path there is -
with it off, a plain `insmod` delivered nothing at all and looked like dead
hardware. Every real frame in this project came from a script passing
`POLLDRAIN=20` by hand.

Expect from a plain load: **one correct 1080p I420 frame, then a clean
end-of-stream.** The single frame is the hardware's actual behaviour. Do not
"fix" it by respawning per frame - each respawn costs one of the 8-18 spawn
budget (`## Card state`), so an OBS session would wedge the card within seconds.

**M168 stopped it being a hang.** The frame was always correct; the node
describing it was not. It advertised NV12 for a payload that is planar I420
(M130), so every application that trusted the driver rendered interleave
banding; `ENUM_FRAMESIZES` and `ENUM_FRAMEINTERVALS` still tested for H.264 and
returned `-EINVAL` for the format `ENUM_FMT` had just handed out; and after the
one frame `DQBUF` blocked forever, which is indistinguishable from dead
hardware. All three are fixed - one `mz0380_current_pixelformat()` decides the
fourcc, and `stall_eos_ms` (def 2000) errors the queue once a frame has been
delivered and nothing follows, so `DQBUF` returns `-EIO` and readers exit with
what they got.

**Verified on hardware 2026-08-24**: `YU12` advertised, four frame sizes
enumerated where the node used to return `-EINVAL`, and `v4l2-ctl` asked for
three frames on a one-frame card and **exited by itself in 7 s with exactly
3110400 bytes, rc=0**. Re-run it any time with

```bash
sudo scripts/mz0380-m168-v4l2-abi.sh
```

One spawn. The enumeration half of it costs none.

**M170 did the same sweep over the rest of the application-facing surface.**
`V4L2_EVENT_SOURCE_CHANGE` was dead at both ends - an emitter nothing called,
and a `subscribe_event` that returned `-EINVAL` for it; the HDMI input reported
`capabilities = 0` and `status = 0`, i.e. "no DV timings here" and "no problems"
with the cable out; `make load-streaming` aborted demanding the deleted
firmware-upload blob; and the README still told users to pass
`firmware_upload=1`, a parameter `insmod` rejects. All fixed, README rewritten,
`PLAN.md` marked SUPERSEDED with its false claims named. **Not yet run:**

```bash
sudo scripts/mz0380-m170-readiness.sh
```

`v4l2-compliance` (zero spawns) plus the first repeat-capture test this project
has run - three stills in a row, checked for whole frames and for being
different images. Costs 3 spawns; check `scripts/mz0380-spawns.sh` first.

**M171 ran it. Repeat capture PASSES** - three stills, all 3110400 bytes, all
real pictures, all three hashes different, so each STREAMOFF/STREAMON really did
capture again. The still grabber is usable as one.

**v4l2-compliance: 148 tests, 132 passed, 16 failed** - three causes, all fixed
in source, all needing a re-run to confirm:

1. GOP/bitrate controls reported values outside their own declared ranges,
   because the BAR5 readback adopts a register the H.264 path never wrote. Zero
   means unset. Two of the six sync helpers already rejected out-of-range
   readbacks; the rest now agree with them.
2. `readbuffers` was 0 while the node advertises `V4L2_CAP_READWRITE`.
3. `G_DV_TIMINGS` answered with the live detection instead of what
   `S_DV_TIMINGS` was told, and S validated nothing.

```bash
sudo rmmod mz0380; sudo insmod ./mz0380.ko && sleep 3 && v4l2-compliance -d /dev/video0 2>&1 | tail -25
```

Zero spawns, and `scripts/mz0380-compliance.sh` makes it one word.

**Progress: 132/16 -> 142/6 -> 147/1.** Note the middle step: **five of those
six were introduced by the fix for the first sixteen** (a bare `vb2_is_busy()`
in `S_DV_TIMINGS`, which broke compliance's "you may set the timings you already
have"). Conformance is not a checklist you satisfy once - it is the only thing
in this tree that notices when a correction overshoots, and it costs nothing
against the spawn budget. Re-run it after any ABI change.

**148 tests, 148 succeeded, 0 failed.** v4l2-compliance is clean.

The last failure was `field == V4L2_FIELD_NONE`: the driver enumerates 1080i50
and 1080i60 but reported a progressive format for them, because `field` was a
literal. It now follows the negotiated timings, and the delivery path uses the
same answer. **Fixed in source, awaiting the confirming run.**

`V4L2_CID_DV_RX_POWER_PRESENT not found` (5 warnings) stays: the driver has the
information and does not export it, which is an addition rather than a
correction. **M173 answered it and the answer is no.** R55 bits
0-1 were "most plausibly 5V/clock presence" (M45). They are not: across a live
unplug/replug they held the value 3 throughout, while the lock bits went
`0x3c -> 0x00 -> 0x3c` and proved the read was live. **This card has no +5V
detect**, so the control stays unimplemented and the five warnings stay. Do not
substitute the lock bits - a powered-but-idle source reads lock 0, which is the
very distinction the control exists to make.

Same trace, recorded properly this time: bit 6 tracks the cable exactly as the
0x3c bits do and nothing gates on it (leave the mask alone - 0x3c has been
correct in every capture), and bit 7 was set only in transition and clear in
both settled states, which is 5 samples and therefore an observation, not a
finding.

### If you want to keep going, the honest options

0. **Get a non-1080p source and test with it.** M174 found by reading that the
   vb2 plane is sized from the negotiated geometry while the drain delivers the
   detected one, and nothing compared them - so a 720p source would have handed
   applications a 720p frame in a buffer labelled 1920x1080, silently. The guard
   (`strict_geometry`, def 1) now refuses that instead, but **the non-1080p path
   is correct by construction, not verified**. Any source that can output 720p
   settles it. The 1080p regression check is `sudo scripts/mz0380-m168-v4l2-abi.sh` -
   the guard sits on the working path.

1. **Widen the spawn budget** so single-shot capture is at least repeatable.
   M157's `vic_fast_kill=0` is already the default and is the only candidate;
   it needs ordinary runs to accumulate evidence (18+ spawns on one power cycle
   without a wedge is the answer). **M169 built the instrument for this** - the
   count did not exist before, and `dmesg` could not supply it because every
   script here opens with `dmesg -C`. Now:

   ```bash
   scripts/mz0380-spawns.sh
   ```

   No root needed to read. `mz0380-m55-real-capture.sh` banks each run's spawns
   automatically from its `cleanup()`, so ordinary runs accumulate the evidence
   by themselves. The tally lives in `/run` and so resets when the host boots,
   which is the same event that resets the card.
2. **Make recovery cheap** - `mz0380-m52-card-recovery.sh` is fixed and has
   never been run. Do it *while wedged*.
3. Accept the bound and present the device as a still grabber.

**Everything below this point predates M159.** The cadence, respawn-per-frame
and "host-side lever" sections describe the search that M166 ended; read them as
history.

---

## HARD CONSTRAINT: do not upload firmware to the card

Use only what is already on the card. Standing instruction, not a preference.

**As of 2026-08-20 the upload path is GONE FROM THE TREE ENTIRELY** - not
neutered, not inert, gone. There is nothing left to accidentally re-enable:

- `mz0380-fw.c` is handshake-only: read the expected version from the
  `MZ0380.FW.TXT` sidecar, `mz0380_card_init()` (CMD_INIT + GET_BOARD_VERSION),
  warn on mismatch, mark READY.
- **The `firmware_upload` module parameter no longer exists.** It survived for a
  while as an inert no-op so old scripts would still insmod; both halves were
  removed together. `modinfo` confirms it is absent.
- **All 32 scripts** that passed `firmware_upload=1` were stripped, plus
  `mz0380-correlation.sh`'s `--firmware-upload` CLI flag and its dead
  `insmod_args+=()` branch.
- **The opcodes are deleted from `mz0380-reg.h`**: `MZ0380_CMD_BEGIN_FW_DL`,
  `MZ0380_CMD_COMMIT_FW`, `MZ0380_CMD_BEGIN_BASE_FW_DL`,
  `MZ0380_CMD_COMMIT_BASE_FW`, `MZ0380_MB_FW_BUFFER` and the prose block
  describing the aperture protocol. A comment marks 0x0b/0x0c/0x0e/0x0f as
  deliberately undefined.
- `struct mz0380_board.firmware_name` / `.firmware_base_name` and both
  `MODULE_FIRMWARE("...HD.HEX")` declarations are gone. The only file the
  driver ever asks the firmware loader for is `mz0380/MZ0380.FW.TXT`, read for
  a version comparison it only ever warns about.
- `MZ0380_FW_STATE_UPLOADING` removed from the state enum.
- **`mz0380-m81-build-cardlog-fw.sh` is DELETED.** It built a patched card
  rootfs to redirect the card's console over PCIe. The previous session's
  attempt at exactly that is what broke the card's userspace.
- **`mz0380-m0m2-test.sh` lost its `m2` stage** - that stage *was* the upload
  attempt. `m0` (read-only observability) and `m4` (bus master + MSI handshake)
  remain.
- `mz0380-bringup.sh`'s presence check now points at the `FW.TXT` sidecar
  instead of the `.HEX` blob.
- `mz0380-re-dump.sh` is untouched and stays: it unpacks the blob **read-only**
  on the host for RE analysis (it is where tinyvenc5 is read from). Reading the
  image is not uploading it.

Keep `/lib/firmware/mz0380/MZ0380.FW.TXT` at `01.11` (matches the card). Never
restore the `01.13` file. The `.HEX` files under `/lib/firmware/mz0380/` are now
unreferenced by the driver; deleting them is optional and needs root - leave
them unless `re-dump.sh` no longer needs the unpack.

Consequence: the VIC hardware status word cannot be read by patching the card.
Plan around that rather than circling back to it.


## Card state

**A soft PC shutdown is enough to reset the card** - M127 ran five streams on a
card that had only been soft powered off, with no wedge. The older "power-cycle
at mains first" advice is over-cautious; do it only if a run shows the wedge.

The card still wedges somewhere in the **8-18 stream** range. M127 ended at
**5 streams**; the 2026-08-21 session spent **12** (M128a-d, M129, M130a,
M132-M135) on top of whatever the current power cycle had already used. None
wedged - but 12 is inside the historical 8-18 wedge range, so the next few runs
are in the risky zone. tinyvenc5 is spawned per stream by `video_capture_mgr`
(`system("./tinyvenc5 -D ...")` plus `killall -9 tinyvenc5`), and
`EncodingGroup::Start`'s failure path calls `exit()` - so "a wall of
`SET_VIC ret=-110`" is consistent with tinyvenc5 finally failing to start and
nothing being left to ACK.

**M157 named a mechanism and changed a default because of it.** SET_VIC byte 33
`fast_kill` picks how vcm disposes of the previous encoder: `1` (what Windows
sends, and our old default) is a bare `kill(pid, 9)`; `0` is `kill(pid, 2)`
followed by a 2 s poll for a clean exit. tinyvenc5 handles SIGINT with a
handler that runs `EncodingGroup::~EncodingGroup()` and `exit(0)`, so only the
second branch ever reaches the destructor, the atexit chain, `SSM_Recycle` ->
`shm_unlink` and `MemBroker_FreeMemory`. SIGKILL skips all of it, and the
resources it skips (`[SSM] over %d handles`, a 32 MB `/tmp`, the DRAM
carve-out) are the finite cross-process kind that only a power cycle otherwise
reclaims.

**`vic_fast_kill` now defaults to 0.** Verified neutral on capture: a full
3110400-byte frame, NOT SPLASH, CHROMA OK, `ret=0`. It is **not yet proven to
move the wedge** - proving that directly costs ~30 spawns - so the plan is to
let ordinary runs accumulate the evidence. Every dmesg records which regime it
belonged to via `fk=` in the SET_VIC banner. **If a session passes 18 spawns on
one power cycle without wedging, that is the answer.** If it wedges anyway, the
next move is `mz0380-m52-card-recovery.sh`. Its `mailbox_alive` greps were
refreshed to `CMD_INIT answered on attempt` in e93285b - this file said they
still needed it, and that was already out of date. The script has still **never
been run**; a working bus-reset stage would turn the wedge from a mains-off cold
boot into five seconds.

**The wedge has TWO signatures, and the second one was only named on
2026-08-23.** The one this file has always described is a wall of
`SET_VIC ret=-110` mid-run. The other kills the card before anything starts:

    CMD_INIT got no answer (-110), STATUS=00000000 EVENT=00000000
        RESULT=00000000 bar5[dc]=00000000
    card handshake failed (-110) - the mailbox is deaf
    MST3367 bring-up skipped - firmware not ready

PCIe is **fine** in this state - the device enumerates, an IRQ is assigned, and
BAR reads return sane values (`bar5[30]=fc200004`, `bar5[38]=fc20005f`). What is
dead is the card's own mailbox service. Because the handshake never completes,
the driver never brings up the MST3367, so the visible symptom is
**"No coherent HDMI timing was locked during the 45s window"** on every run -
which reads exactly like a dead HDMI source and is not one. The receiver was
never touched.

Two traps this cost a session:

- `mz0380-m55-real-capture.sh` prints `waiting for the card to finish booting
  its own flash image...` and then `using /dev/video0` **whether or not the
  handshake succeeded**. Neither line is evidence the mailbox is alive.
- A run that aborts before stream start leaves `token[0x40]=00000000`,
  `irq_total=0` and **unpoisoned (all-zero) buffers**, because the sentinel seed
  and the poison fill both happen *at* stream start. That is not a wedge
  signature and not a capture - it is an abort. Only compare those fields
  between runs that actually reached `stream start:`.

Confirm it in **2 seconds**, zero spawns, no HDMI source needed - this is the
cheapest test in the tree and it should be the first thing run whenever a
session starts or a run reports no lock:

```bash
sudo modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm; sudo insmod ./mz0380.ko; sudo dmesg | grep -a "CMD_INIT\|handshake" | tail -3; sudo rmmod mz0380
```

- healthy: `CMD_INIT answered on attempt 1 (status=0xdddddddd)`
- wedged: `CMD_INIT got no answer (-110)` + `the mailbox is deaf`

The `modprobe` line is required - without it `insmod` fails with
`Unknown symbol in module`, which is a missing dependency and says nothing about
the card. (`Invalid module format` is different again: that is a kernel/vermagic
mismatch. See the build section.)

The longer I2C probe says the same thing and also shows the bus, if wanted:

```bash
sudo START=0x50 scripts/mz0380-m83-i2c-devscan.sh 16
```

`periph: card handshake not complete` and `0 of 0 registers read` means the
mailbox is deaf, not that the receiver is missing. Power-cycle; do not chase the
source or the receiver.

**The control capture is the health check:**

```bash
sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 1
scripts/mz0380-m127-splash.py /tmp/cap-m55.nv12       # expect: SPLASH
```

`sudo` is required for `dmesg` on this kernel - without it it fails and silently
prints 0.

---

## Building - the driver now targets two kernel series

`make` still does the right thing; read this only when it refuses to.

**Every hardware result in this file was measured on `6.18.42-1-cachyos-lts`,
and that is where runs should stay.** A kernel change is a new variable under
whatever is being measured (method rule 1). 7.x is supported so the tree keeps
building as the distro moves, not so results can be taken there.

| command | what |
|---|---|
| `make` | running kernel, as before |
| `make kernels` | running kernel, which kernels are buildable, which one `make` will use |
| `make KVER=<ver>` | cross-build for another installed kernel |
| `make all-kernels` | build every buildable kernel into `ko/mz0380-<ver>.ko` |
| `make objclean` | top-level object wipe; what `all-kernels` uses between kernels |
| `make distclean` | `clean` plus `ko/` |

**The only 6.x -> 7.x drift was `vb2_ops->wait_prepare`/`wait_finish` and the
`vb2_ops_wait_*` helpers** (`mz0380-video.c`'s `mz0380_qops`). vb2 core took
over the `q->lock` handling and both the ops and the helpers were removed:
present in 6.18, gone in 7.2. The Makefile **greps `videobuf2-v4l2.h`** and
defines `MZ0380_HAVE_VB2_WAIT_OPS`; it does not test `LINUX_VERSION_CODE`,
because the removal release is between 6.18 and 7.2 and a wrong guess yields a
module that builds and then mishandles a blocking DQBUF. Verified both ways:
the 6.18 module still imports both symbols, the 7.2 one imports neither.

### The failure that cost the 2026-08-23 session its first run

A rolling distro can upgrade the kernel package **out from under the running
kernel**, leaving `/lib/modules/$(uname -r)` gone entirely. Nothing
out-of-tree can be built *or loaded* on such a boot, and kbuild says only
`No such file or directory`. `make` now names the state, lists the buildable
kernels, and the hardware scripts fall back to a prebuilt
`ko/mz0380-$(uname -r).ko` (loudly - it may predate your edits) via
`mz0380-build.sh`. If neither exists, **reboot into an installed kernel** -
prefer the LTS.

---

## STATE after M129 - THE CARD CAPTURES REAL VIDEO

**2026-08-21: real 1080p video came out of the card.** A photograph of the scene
in front of the camera, confirmed visually and by measurement. The raw frame is
preserved in the repo as `m129-first-real-frame.raw`
(sha256 `76e7050e...cc8704`).

    sudo POLLDRAIN=20 EXTRA="post_proc=1 post_mask=0 fake_frame_off=1" scripts/mz0380-m55-real-capture.sh 1

    captured 3110400 bytes = 1 whole frames + 0 bytes
    VERDICT: NOT SPLASH  (Y 170 distinct values, UV 144)

    row-to-row corr 0.9665 | col-to-col 0.9703 | pixel-shuffled control -0.0008
    22.02% of pixels have |dY/dx| > 8 ; 0 of 1080 rows are video black

Luma is **perfect** - a sharp, artefact-free, correctly-framed 1080p greyscale
image with no tearing or banding. Chroma is wrong (see below).

### Three ingredients, all required

1. **`fake_frame_off=1`** - op `0x31` byte `0x0e`. Stops
   `EncodingGroup::fake_frame_process` ever being created.
2. **`post_mask=0`** - the default `0x1f` enables a mask-gated store that
   truncates the DMA to one 16-byte burst (M128d). With `0x1f` the frame never
   completes, so #1 alone shows nothing.
3. **`post_proc=1`** - the `win_seq=0` baseline never sent `0x31` at all.

M128a had #1 and #3 but not #2. Its "16 bytes of 0x53" was the real frame,
truncated.

### What this overturns - re-read old results with this in mind

**"The VIC reports no signal" was wrong.** The standby thread was painting
NOSG_LOGO_Y over a working capture path, and every previous run measured the
thread, not the capture.

- `fake_frame_process` is not a fallback for a dead input. It runs
  unconditionally beside `encode_handler` because
  `preview_params_settings[ch].byte[0x0a]` is zeroed at startup and nobody ever
  told the card otherwise - **Windows included** (M82's `board_flag_b` = 0).
- The `(Drop this frame) ... bNoSignal` path in `libtkmf_video_source.so.0` that
  M127 named as "the target" is **not** what was happening. It was never
  observed - the card's console is unreachable - and it was wrong.
- M127's corollary now has its explanation: every sweep scored as
  splash/not-splash was reading a thread that ignores every knob those sweeps
  turned. The negatives stand, and they were never about capture.

### Score with `mz0380-m127-splash.py`

Exit 1 = SPLASH, 0 = NOT SPLASH, 2 = NO FRAME. **With `fake_frame_off=1` the
oracle inverts in usefulness: 0 / NOT SPLASH is now the PASS.** It also prints
the distinct-value counts, which is the quick real-vs-flat check.

### Chroma - FIXED (M130a)

    corr(A, luma)  -0.9664 -> -0.3194        share of A that is luma  0.93 -> 0.10
    corr(B, luma)  -0.7488 -> +0.3492        share of B that is luma  0.56 -> 0.12

Correct, natural-colour 1080p. The residual is ordinary scene correlation, not a
defect.

**Cause.** HDMI YCbCr 4:4:4 puts Cb on blue, Y on green, Cr on red. hdcapm's CSC
table converts RGB->YCbCr and is written unconditionally - its
`MST3367_HdmiGetPacketColor()` reads the input colour space from BANK2 0x48 bits
6:5, caches it, and never uses it. Fine on its board, whose EDID makes sources
send RGB. We push no EDID, this source sends YUV444, so the matrix computed
`Cb_out = -0.291*Y + ...`: perfect luma, chroma that was mostly inverted luma.

**Fix.** `mst_csc_ctl` defaults to **AUTO** and picks the mode from the detected
colour space - 0x00 (no conversion) for YUV422/YUV444, 0x40 (hdcapm's matrix)
for RGB. It is applied by `mz0380_mst3367_apply_csc_mode()` at **stream start**,
not in `init_regs()`, because init runs before HPD and 0x48 is meaningless
there. An explicit value still forces the byte.

**The frame is planar I420, NOT NV12.** Y, then two 960x540 planes. The
`.nv12` filename is historical and cost two viewing mistakes in one session;
`mz0380-m55-real-capture.sh` now prints the right hint.

```bash
ffplay -f rawvideo -pixel_format yuv420p -video_size 1920x1080 m130-colour-correct-frame.raw
```

Before/after pair kept in-tree: `m129-first-real-frame.raw` (broken chroma),
`m130-colour-correct-frame.raw` (correct).

---

## What is CLOSED - do not re-open

| avenue | how |
|---|---|
| **I2C `0x98`** | It is an **IT66121 HDMI transmitter** (vendor `0x4954` "ITE" at regs 0x00/0x01, device `0x612` rev 1 at 0x02/0x03, ignores bank select). The board's output passthrough. **Not on the capture path.** M127c. |
| **I2C `0x90` / `0xa0`** | Not fitted. The bus is exactly two devices: `0x9c` receiver, `0x98` transmitter. |
| **MST3367 `0xb0`** | Fully decoded (M100): bit0 = embedded(1)/external(0) sync, bit2 = 10(1)/8(0) bit. `0x21`,`0x25` (embedded) -> splash. `0x20`,`0x14` (external) -> **nothing renders at all**. Embedded sync is required for VIC init. |
| **MST3367 `0xb1`/`0xb2`/`0xb5`** | These are **CSC-matrix cells**, not an output stage - hdcapm writes a 36-byte CSC table at `0x92..0xb5` that overlaps them, then fixes up `0xb0` last. We write the same table. Sweeping one byte of a matrix was never a hypothesis. M127d. |
| **Receiver init generally** | Ours is a **superset of hdcapm's** - every register it writes, we write, `0xe2` included (we drive auto-position dynamically). M127h. |
| **SET_VIC `in_fmt`** | 3, 6, 7 all splash. 0 is illegal and stops the card rendering (M104). |
| **SET_VIC `vic_in_w`** | 1920 / 2048 / 2200 / **3840** all splash. M76's double-rate width hypothesis is now validly dead - M127g re-measured 3840 at a working `b0` with a working delivery path and a real oracle. |
| **Host wake-ups / kicks** | op `0x2f` x1171 and op `0x06` x1224 (M133): accepted every time, **one frame**, and the receiver **loses lock** under either. Kicks do not gate the cadence. Do not flood these. |
| **Opcode `0x50`** | `SET_OSD` - on-screen-display text. |
| **the Windows ordering (`win_seq`)** | **Neutral, M142.** pre-STOP + 1900 ms settle + SET_BUF-first reproduce the baseline exactly, once op `0x06` is present. Do not spend another spawn on it. |
| **op `0x06`** | **Required, M142.** It arms `vpl_dmac`'s outbound push - the transfer that carries the card's one frame to the host. Without it: 0 frames, 0/1024 pages touched. Not a "kick" (those are the `0x2f`/`0x06` floods of M133) - it is the arming step. |
| **"the VIC sees no signal"** | **WRONG - M129.** The card captures real 1080p video. The standby thread was painting over it. Every splash verdict in this file measured that thread. |
| **frame rejection in `libtkmf_video_source.so.0`** | Not what was happening. M127 named it as the target from static RE alone; it was never observed and it was wrong. M129. **M138 gives the mechanical reason:** `bNoSignal`/`bCCIRErr`/`bFifoFull` are `printf` arguments in the drop banner and are never tested. The only real gate is a width equality test. |
| **the host completion handshake** | **Fully cleared, M138.** `ep.ko` cannot block (no wait/wake/sleep relocation exists in it). The EVENT credit is a real one-shot but `BAR0[0x00]=0x400` re-arms it via card IRQ 42, and M133's 1224 completed commands prove that happens 1000+ times a run. Stop spending spawns here. |
| **`state[0x630]` / SET_VIC byte 34** | M136 read it as gating EVENT-vs-accumulate. **Wrong, M138:** both branches are credit-gated identically and byte 34 only shifts the *audio* bit. M137's null result was expected. |
| **`skip` / `fps` / `avg` as preview pacers** | **Dead, M138.** `m_fps` and `m_avg` are stored by `TKMF_VideoSrc_Setup` and never read. `m_skip` drops one frame in `(skip+1)` - so `skip=0` is the *no-drop* setting, the opposite of M134's theory. |
| **EDID / HDCP** | Dead ends. Windows captures with no EDID pushed. |
| **op `0x2f`** | `SET_ENC_PARAMS_POST` - the SAME handler as `0x2d` (0xebdc), differing only in which banner it prints. H.264 knobs; cannot touch capture. M128. |
| **op `0x51`** | `SET_BAR` - a colour-bar overlay rect per (ch, line). Overlay only. M128. |
| **op `0x62`** | `SET_LOGO` - logo overlay, `/tmp/PIC_LOGO_%d`, max 320x240. Overlay only. M128. |
| **op `0x29` after start** | Ignored. The in-loop dispatch maps `0x29` to the default; tinyvenc5 reads SET_VIC exactly once, before the loop. M128. |
| **"an extra command before START truncates"** | No. The `0x09` control delivers a full frame. Nor is it cadence: a 200 ms gap changed nothing. M128c/d. |
| **M91's "something else in `win_seq`"** | It is `0x31` carrying `post_mask=0x1f`. With mask 0 the same command delivers a whole frame. M128d. |

The receiver holds a clean `R55=0x7f` 1080p60 HDMI lock in **every one** of
these. The VIC reports no signal in **every one** of these.

---

## Verified ABI (M127) - trust this over older field maps

`ep.ko`'s `epint_show` memcpy's `rodata[0xa0 + cmd]` bytes of the mailbox to the
card's userspace. Per-opcode payload length:

| cmd | 0x06 | 0x07 | 0x09 | 0x29 | 0x2a | 0x2d | 0x2f | 0x31 | 0x50 | 0x51 | 0x52 | 0x62 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| len | 8 | 8 | 8 | **40** | 20 | 44 | 44 | 20 | 44 | 20 | 7 | 12 |

SET_VIC (0x29), 40 bytes, verified from both `ep.ko` and `video_capture_mgr`:

```
 0..3  cmd = 0x29        20..21 x_start            32     vanc_lines
 4     ch                22..23 y_start            33     fast_kill
 5     fps               24..25 input_frame_width   35     is_slave
 6     fw                26..27 input_frame_height  36..39 nosg back/y/u/v
 7     input format      28     bitstream_num
 8..9  width             29     osd_enabled
10..11 height            30     osd_size
12     m                 31     is_nosg
13     flip  14 mirror   16..19 color_info[4]
```

- **Byte 7 is the cfg `input format` enum, not an interlace bool.**
  `video_capture_mgr` *labels* it `interlace(%d)` in its printf, but it is
  `sprintf`'d **raw** into the cfg line matching `"input format"`. `6 = BT1120p`,
  `7 = BT1120i`. The code is right. (`mz0380-dma.c`'s M71 comment block said
  otherwise; **fixed in M128**.)
- Bytes 40..41 exist in video_capture_mgr's per-channel struct but are past the
  40-byte payload, so they are not host-settable through SET_VIC.

**The cfg is patched by value as well as by comment.** M126's "tinyvenc5 patches
exactly nine keys" is wrong; the patcher is in `video_capture_mgr` (0xa290) and
it `atoi()`s every line, rewriting **any line valued 1080** with SET_VIC
`height` (bytes 10..11) and any line valued 1920 with SET_VIC `width`
(bytes 8..9). So captured geometry IS host-driven - through bytes 8..11, not
through `input frame width/height` (bytes 24..27, the `vic_in_w`/`vic_in_h`
knobs).

**ep.ko has a sticky `no_signal` latch.** SET_VIC with `width == 0 || height == 0`
sets it, does **not** `sysfs_notify`, and then ops
`0x06 0x2a 0x2f 0x31 0x50 0x51 0x52 0x60 0x61 0x62` all early-out until a good
SET_VIC clears it.

**tinyvenc5's complete command set:** `0x06 0x09 0x2a 0x2d 0x2f 0x31 0x50 0x51
0x52 0x62`, plus `0x29` handled before the loop. Everything else is ignored.

**M128 named every one of them** - full table in `mz0380-reg.h` next to
`MZ0380_CMD_SET_AIC_PARAMS`, derivation in RE_FINDINGS M128. The one that
matters:

`0x31` is **SET_PREVIEW_PARAMS**, not "POST_PROC", 20 bytes:

```
[4..7] mask (u32)  [8] ch   [9] fps   [0x0a] skip   [0x0b] avg
[0x0c] die_en      [0x0d] preview_off [0x0e] fake_frame_off
[0x0f] preview_no_osd  [0x10] mirror  [0x11] flip   [0x12] hw_d
```

The mask gates **only** byte `0x0c`; `0x0d`..`0x12` are stored unconditionally.
Byte `0x0e` lands in `preview_params_settings[ch].byte[0x0a]`, which is the
`pthread_create` guard on `EncodingGroup::fake_frame_process` in
`init_func` - i.e. the splash. It is read **once**, inside the `0x06` handler,
so `0x31` only bites if it arrives **before** START_STREAMING. Windows sends 0
there (M82 called that byte `board_flag_b`), so retail leaves the splash armed
too. M82 also had mirror/flip swapped; the card's own printf wins.

---

## Where to go next

Capture works. The remaining defect is **one frame per stream**, and M138
relocated it decisively: the stall is **on the card, upstream of tinyvenc5**.

### What M138 settled (static, zero spawns) - do not redo any of it

`EncodingGroup::encode_handler` is read. It is at **0x12b00** (0x12c60 is the
`open()` of `channel_done` inside it). Its loop head is **0x12f04** and the
first call of every iteration is `SSM_ReleaseAndReceive` (0x12f24).
`libsyncsharedmemory.so.0` has **no timed condvar wait**: when reader index ==
writer index the thread parks in `pthread_cond_wait` forever. So the encoder is
**starved, not stuck** - and none of M137's five PLT candidates is reached,
because all of them sit downstream of that first call.

The whole producer chain is now mapped:

    vpl_vic.ko ISR -> chan->frame_ready = 1 + __wake_up
      -> ioctl(/dev/vpl_vic, 0xe301) = wait_event_interruptible_timeout(...)
      -> libvideocap VideoCap_WaitVIC / VideoCap_Sleep
      -> libtk_video_capture process() loop
      -> libtkmf_video_source img_handler  (the callback)
      -> SSM_DeliverAndAllocate -> wr_idx++ -> broadcast
      -> tinyvenc5 encode_handler -> pwrite channel_done -> ep.ko -> host

`process()` **never exits on a stalled VIC** - it retries `VideoCap_Sleep` every
1 ms forever. A dead VIC therefore looks exactly like what we have: one frame,
no error, no log, no host-visible symptom.

Also closed by M138:

| claim | status |
|---|---|
| `store_channel_done` blocks waiting for a host ack | **dead.** `ep.ko` has no wait/wake/sleep relocation at all; the handler runs with IRQs masked and is straight-line register writes. |
| the EVENT credit is the gate | **dead, and the driver is correct.** `.data[0]` starts at 1, is consumed by every event and re-armed only by card **IRQ 42** = `pciep_isr_clrint`, reached by `BAR0[0x00] = 0x400` (bit 10; `0x800`/bit 11 -> IRQ 43 = command dispatcher). M133's 1224 successful commands prove it is re-armed 1000+ times per run. |
| `state[0x630]` / SET_VIC byte 34 gates EVENT vs accumulate | **wrong (M136 corrected).** Both branches are credit-gated identically; byte 34 only shifts the **audio** bit by 15 vs 16. M137's null result was the expected one. |
| `bNoSignal` / `bCCIRErr` / `bFifoFull` gate frame delivery | **dead.** They are `printf` arguments in `img_handler`'s drop banner and are never tested anywhere in the library. |
| `skip` paces the preview (M134's theory) | **backwards.** `img_handler` drops when `Count % (m_skip+1) == 1`, i.e. it drops **one in (skip+1)**. `m_skip = 0` is the *no-drop* setting. |
| `m_fps` / `m_avg` reach the video source | **dead.** `TKMF_VideoSrc_Setup` stores five halfwords; `img_handler` reads only width (offset 0) and skip (offset 6). fps and avg are written and never read. |

### The one remaining host-side gate in the source layer

`img_handler`'s only real early-out is a **geometry equality test**:

    VIC_Get.width (capture descriptor +0x1c)  ==  Tiny_Set.width (.bss 0xaa54)

`Tiny_Set` comes from a **single** `TKMF_VideoSrc_Setup` call in
`EncodingGroup::Start` (tinyvenc5 **0x11014**), filled from the per-channel
config record: width `cfg[ch]+0x0c`, height `+0x0e`, fps `+0x09`, skip `+0x38`.
One call, before the loop - so the gate cannot change mid-stream, and it cannot
be what kills frame 2 while letting frame 1 through. But `cfg[ch]+0x0c/0x0e` are
now known to be the **only** host-settable values the source layer reads at all,
which is what makes SET_VIC geometry worth a look.

### M139/M140 settled it further - read this before planning anything

**`store_channel_done()` has never run.** A sentinel seeded into BAR0
0x40/0x44/0x48/0x4c survived 56 s of streaming untouched, with the readback line
proving the registers are host-writable. No video channel and no audio channel
has ever reported a frame. `enc[0x50]=0` says no bitstream was ever DMA'd
either. M138's "writes once then blocks" is **wrong**: `encode_handler` never
writes at all.

`encode_handler` is nonetheless alive - `EncodingGroup::init_func` creates it
unconditionally at **0x10ab0**, with its run flag set one instruction earlier at
0x10a9c. (The gated `pthread_create` at 0x10c08 is `fake_frame_process`, the
splash.) So it is parked on its first `SSM_ReleaseAndReceive`, having received
zero frames.

**The single frame that reaches the host is not from the SDK's frame path at
all.** It arrives with no `channel_done`, no `enc_stat` and no EVENT, over
`vpl_dmac`'s PCIe outbound transfer (`ep.ko` exports `pcie_set_outbound`;
`vpl_dmac.ko` is its only importer, `VPL_DMAC_StartTail` 0xc94 /
`VPL_DMAC_ISRTail` 0xf70).

**One model explains every observation, and nothing else needs to be true: the
card's VIC captures a single frame and stops.** `img_handler`'s *first* call is
the init path - `SSM_Writer`, a prime `SSM_DeliverAndAllocate` with a NULL
descriptor that does not advance `wr_idx`, `TK_ImgProc_Init`, return - so it
publishes nothing. With no second call the ring stays empty forever, the encoder
parks, `channel_done` never fires, and the one captured frame is pushed to the
host by the DMAC. Buffers 1-3 are never touched because there was never a second
capture.

### HARD METHOD RULE, new and expensive: SET_VIC geometry is never one variable

`re-dump/fw/yuan_demo_sdi/nullsensor_1920x1080.cfg` - the card's own capture
config, in the tree - has **twelve lines valued 1920 and twelve valued 1080**:
maximum frame w/h (the ISP allocation), captured frame w/h, input frame w/h, and
nine AE HardwareCtl window w/h pairs. `video_capture_mgr`'s patcher rewrites
every one of them blindly.

So `vic_out_w=3840 vic_out_h=540` (M140) did not move `img_handler`'s gate
input; it moved the ISP allocation and all nine auto-exposure windows too, and
the capture path did not come up at all - **zero** frames, one worse than
baseline. That result says nothing about the gate, and the same objection
applies retroactively to M76 and every other width sweep in `RE_FINDINGS.md`.

The cfg also documents the enums inline: output format `1:YUV420 2:YUV422`,
input format `6:BT1120p 7:BT1120i`. `in_fmt=6` is confirmed right.

**M143 decoded the patcher exactly** (vcm 0xa290; loop counter `#163` = the
cfg's 163 lines, so the on-card template is the one in the tree). Two
mechanisms, in this order per line:

| test | rewritten with |
|---|---|
| `strstr "input format"` / `"output format"` | args 4 / 5 |
| `strstr "start x position"` / `"start y position"` | r9 / r10 |
| `strstr "input frame width"` / `"input frame height"` | r11 / struct halfword |
| `atoi(line) == 1920` **and** `strstr "maximum frame width"` | struct halfword (ISP allocation) |
| `atoi(line) == 1920` otherwise | SET_VIC width |
| `atoi(line) == 1080` | SET_VIC height |
| `strstr "flip video"` / `"mirror video"` | struct halfwords |
| anything else | copied through unchanged |

So `maximum frame **width**` is special-cased and does *not* take the swept
width; `maximum frame **height**` has no such case and does move with height.
The nine AE window pairs still move with both, so the method rule stands.

**Line 4 is `60 // captured frame count`, and nothing can reach it.** Not a
patcher key, and 60 is neither 1920 nor 1080, so it passes through untouched.
No binary in the image contains that string at all - the readers parse the cfg
positionally - so every stream this card runs is configured for a **60-frame**
capture. Not the one-frame cause (60 is not 1), but a ceiling that will bite the
moment the cadence is fixed. Do not read "60 frames then stops" as a new defect.

### M141-M142: the Windows ordering was run. It is NEUTRAL. Do not run it again.

**Run on 2026-08-23, and the kernel was cleared as a variable** - both runs on
`7.2.0-1-cachyos`, same power cycle:

| run | sequence | frames | token[0x40] |
|---|---|---|---|
| M141c | baseline | 1, real video, NOT SPLASH, CHROMA OK | `a5a5a5a5` |
| M141 | `win_seq=1` | **0**, 0/1024 pages touched | `a5a5a5a5` |
| M142 | `win_seq=1 win_start_op6=1` | **1**, real video | `a5a5a5a5` |

All three on `7.2.0-1-cachyos`, one power cycle, `R55=0x7f` locked throughout,
every command `ret=0`.

**Op `0x06` arms the `vpl_dmac` outbound push.** That is the whole of M141's
zero: `0x06` is the only difference between M141 and M142, and adding it back
restores the frame exactly. M140's reading - that `0x2d`/`0x31` do the same bare
`sysfs_notify("epint")` - is true of the *card's* dispatch and says nothing
about what arms the DMAC on the way out.

**With `0x06` present the Windows ordering reproduces the baseline precisely.**
pre-STOP, 1900 ms settle, SET_BUF-first: all neutral. Sentinel intact,
`enc[0x50]=0`, `EVENT=0`, 0 producer-watch changes.

So **the host-side sequence is exhausted.** Ordering, opcode set, kicks, credit,
completion handshake, geometry, preview params, CSC - all measured, all neutral
or worse. Nothing the host sends changes how many times the VIC captures.

**The frame count is not a VIC-capture counter.** The one host-visible frame
comes from the DMAC one-shot, not the SDK path, which has never run (M139). No
host counter can tell one VIC capture from several that were never published.

<details>
<summary>Original M140 write-up of why this run was worth making (kept for the field derivations)</summary>

### The Windows ordering - one spawn, three distinguishable answers.

```bash
sudo POLLDRAIN=20 EXTRA="win_seq=1" scripts/mz0380-m55-real-capture.sh 5
```

**Why this and not more RE.** Windows streams continuously off this exact card
with the exact same on-card firmware - the card boots its own flash, so nothing
about it differs between hosts. Whatever makes the VIC capture more than once is
therefore in the host->card sequence, and that sequence is already decoded.
`win_seq=1` assembles it, and every field inside it is byte-matched from
M82/M127/M128:

1. pre-**STOP** (op 0x07, all channels). Windows precedes *every*
   reconfiguration with this; we have only ever sent STOP on the unwind path.
2. **1900 ms settle** (`stop_settle_ms`, default 1900 - Windows measures
   1.84-1.91 s between that stop and the SET_VIC that follows).
3. **SET_BUF first** (`win_bufs_first`, default true). Windows registers its
   capture buffers when the pin opens, i.e. before the reconfiguration.
4. SET_VIC -> SET_AIC -> 0x2d -> 0x31.
5. **No op 0x06.** Windows never sends START_STREAMING on the capture path;
   ep.ko routes 0x2d and 0x31 to the same bare `sysfs_notify("epint")` that
   0x06 performs, so the tail *is* the kick. (`win_start_op6=1` adds it back.)

**It has never been run.** Until M135a the `0x31` in this path carried
`post_mask=0x1f`, which truncates the DMA to 16 bytes and made the whole
ordering unscoreable - that is the entire content of M90/M91's "win_seq renders
nothing". Mask is 0 by default since M131.

**SUPERSEDED - the sentinel is blind, so read the FRAME COUNT. Proven in M153
after two wrong turns; the chain is worth knowing because the question is not
eyeball-able.**

M150 said the `channel_done` `pwrite` (0x13538) is unreachable, arguing from a
`beq` that skips the range containing it. M152 withdrew that: the argument is
invalid, because three unconditional branches from outside (0x13e9c, 0x142b0,
0x1582c) jump into 0x134a0, inside the range and ahead of the write. M153 then
settled it mechanically with `mz0380-cfg.py`, and the answer is that M150's
*conclusion* was right after all - those three branches are themselves
reachable only *through* the flag-guarded fall-through, so removing that one
edge kills them too.

With `mma_already_start` pinned to 0 (nine reads, zero stores, no pool word for
0x7eda0), the CFG reports **unreachable** for the `channel_done` write, the
second `pwrite`, all three `TK_MMA_ProcessOneFrame` sites and
`TK_MMA_WaitOneFrameComplete`. Exactly one push survives:
**`TK_MMA_StartOneFrame` at 0x1430c, asynchronous, never awaited.**

So `store_channel_done` never runs, `token[0x40]` cannot move, and every
"moves off `a5a5a5a5`" row below is unsatisfiable. **Frames delivered is the
only working oracle** - the poll-drain measures those against the poison
boundary, independently, so the frame counts in the run history are all valid.

Do not re-derive this by reading branches. Run:

```bash
python3 mz0380-cfg.py re-dump/tinyvenc5.txt 0x12b00 0x15c60 0x12f04 <addr>...
```

Original text, kept because the run history refers to it:

**Read the result off `token[0x40]`, not off the frame count.** The M139
sentinel turned the card's own encoder into a host-visible oracle that is
independent of the DMA, the notification path and the poll-drain:

| `token[0x40]` at stop | meaning |
|---|---|
| moves off `a5a5a5a5` | **The encoder ran.** The Windows ordering is the fix. Chase cadence from there with a live pipeline instead of a dead one. |
| `a5a5a5a5`, 0 frames | Ordering is not it, and the no-0x06 variant is eliminated with it. Retry once with `win_seq=1 win_start_op6=1` before abandoning. |
| `a5a5a5a5`, 1 frame | Same one-shot as baseline; ordering is neutral. Host-side is then genuinely exhausted - go to step 2. |

</details>

### START HERE (2026-08-23): the cadence question is answered. Read this first.

**One frame per tinyvenc5 process, and only a new SET_VIC makes a new process.**
That single sentence covers every measurement in this file - why OBS shows one
image and freezes, why every m55 run reports `1 deliveries`, and why the NOSG
respawn-per-frame loop was the only thing that ever produced a sequence.

The card is **not** broken. M154 captured two frames 13 s apart in one module
load, 83% of pixels changed, so the producer, receiver, encoder and DMA path
all work and re-capture fresh content on every cycle.

**There is no host-side lever left.** Three were removed this session and none
moved the cadence:

| lever removed | milestone | result |
|---|---|---|
| the per-frame `enc_stat` ack - the last mid-stream write to the card | M151 | no change |
| the respawn (`setvic_once=1`) | M155 | no frame at all; `0x2d` times out |
| STOP at streamoff (`stop_on_streamoff=0`) | M156 | no change; STOP exonerated |

That is on top of ordering (M141/M142), kicks (M146), geometry (M140) and
`bitstream_num` (M148).

**The mechanism, from M153's CFG:** `EncodingGroup::mma_already_start` is a
per-process static with nine reads and zero writes, so with it pinned at 0 the
only reachable MMA push is `TK_MMA_StartOneFrame` (0x1430c) - asynchronous -
and `TK_MMA_WaitOneFrameComplete` is unreachable. One push per process is
exactly what the code shape predicts.

**The one live thread.** After its single push the encoder stops answering op
`0x2d` (`ret=-110` on every later cycle), yet it does not exit: `encode_handler`
has no `exit`/`abort` call, has one epilogue, and parks on
`SSM_ReleaseAndReceive` as M138 described. Nor is it a mutex deadlock - cutting
the unlock at 0x13730 makes both the loop head and the wait unreachable from the
re-lock, so every path back releases the lock. But `encode_handler` is only a
*thread*; tinyvenc5's **main** loop services `0x2d`, and nothing yet explains
why that stops.

**Read tinyvenc5's main command loop.** Last unpulled thread, static, zero
spawns. Use `mz0380-cfg.py`, not eyeballed branches - that mistake cost three
milestones today (M150 -> M152 -> M153).

<details>
<summary>Superseded framing: why does `encode_handler` not iterate twice? (M147-M150)</summary>

### why does `encode_handler` not iterate twice? (static, zero spawns)

M147 relocated the defect. The chain of inference, each link checkable:

1. The delivered frame is **raw I420**, not H.264 (M146) - `enc[0x50]=0` and the
   sentinel say no bitstream was ever produced, yet the image is valid.
2. Only `vpl_dmac` can write host memory (`ep.ko` exports `pcie_set_outbound`,
   `vpl_dmac` is its only importer). It is reached only via `/dev/vpl_dmac` ->
   `libmassmemaccess.so.9` -> `libtk_mass_mem_access.so.0`, and of the video
   binaries only **tinyvenc5** links those.
3. Every `TK_MMA_*` call in tinyvenc5 is inside **`EncodingGroup::encode_handler`**,
   with the push sites (0x1349c, 0x13e64, 0x142ac, 0x1430c) in the loop body,
   after the first `SSM_ReleaseAndReceive` (0x12f24).
4. `img_handler` publishes at **0x1290**, reachable only when `[r4+0xc] != 0`;
   the first call goes to 0x129c instead - the init path with `SSM_Writer`
   (0x14f4) and the priming deliver (0x1544).

So: a frame arrived -> the ring was non-empty -> `img_handler` was called **at
least twice** -> **the VIC captured at least two frames**, and `encode_handler`
ran at least one complete iteration.

M150's warm-up counter pushes that further: if the first two receives are
discarded, the delivered frame is the **third**, so `img_handler` published
three times and was called at least four. That is stacked inference and flagged
as such - but every closer look has moved the captured-frame count **up**, never
down. The card's capture is not the broken part.

**Read the loop body, 0x12f24 to the branch back to 0x12f04**, in
`re-dump/tinyvenc5.txt`. The loop has four back-edges (0x12fa8, 0x13734,
0x13d28, 0x13dd0) and one **exit** on an SSM error: a return of -1 or -2 leaves
via 0x1512c, which sets a flag and jumps to the cleanup at 0x12fc0. So "parked
forever in `pthread_cond_wait`" (M138) is not the only way this thread stops -
it can also leave the loop entirely.

**Already read, do not redo (M150):**

- The first post-receive test at 0x12f4c (`[chan+0x34] <= 1 -> 0x13d04`) is a
  **self-incrementing warm-up counter**: the branch it takes increments that
  same address (0x13d20) and loops. The first two receives are discarded; the
  main body runs from the third. It is not `bitstream_num`, which is why M148's
  hardware sweep was flat.
- **`channel_done` cannot be written in this image** (M150, withdrawn by M152,
  restored by M153's CFG). With `mma_already_start` at 0, the write at 0x13538,
  the second `pwrite`, all three `TK_MMA_ProcessOneFrame` sites and
  `TK_MMA_WaitOneFrameComplete` are all unreachable. **The one surviving push is
  `TK_MMA_StartOneFrame` at 0x1430c - asynchronous, and never awaited.**
  That unifies the whole run history: `mma_already_start` is per-process,
  tinyvenc5 is spawned per stream, so **one frame per spawn** is exactly what
  the code shape predicts.
- What IS established about `mma_already_start`: nine reads, zero writes, no
  literal-pool reference to 0x7eda0. It is 0 for the process lifetime, so every
  branch that needs it set is not taken - including the synchronous
  `TK_MMA_WaitOneFrameComplete` at 0x14358, which therefore never waits. Known already: `EncodingGroup::mma_already_start`
(.bss 0x7eda0, `[base-0xfa8]`) has **nine reads and zero writes** - it is
permanently 0, so every branch requiring it is dead, including the synchronous
`TK_MMA_WaitOneFrameComplete` at 0x14358. Map those dead branches before
trusting any control flow in this function.

**Corrections this forces** - do not reason from the superseded versions:

| claim | status |
|---|---|
| `store_channel_done` has never run (M139) | **stands** - measured |
| `encode_handler` received zero frames (M139) | **too strong** - it ran a full iteration |
| the VIC captures exactly one frame (M140/M144) | **at least two** - never measured; the host counts DMA pushes, not captures |
| the one frame is not from the SDK path (M139) | **wrong** - it is, just not the H.264 bitstream path |

<details>
<summary>Superseded: the VIC re-arm investigation (M144/M145)</summary>

### who re-arms the VIC? (static, zero spawns)

M144 disassembled `vpl_vic.ko` (kept as `re-dump/vpl_vic.txt` +
`re-dump/vpl_vic.sym`) and found the mechanism, though not yet its cause.

`0x60` - bits 5:6 of the register-block words at offsets **8** and **0xc** -
appears exactly four times in the module and nowhere else:

| site | bits SET | bits CLEAR |
|---|---|---|
| `Ioctl` 0x3a3c/0x3a48 | skip - do not disturb an in-flight capture | program the next capture target (`[dev+0x1c]`, `[dev+0x20]` from `[chan+0x40]`/`[chan+0x44]`) |
| `ISR` 0x17d0/0x17dc | keep bit 10 - capture stays enabled | **0x1928: clear bit 0 of `[block+4]` - HALT** |

A one-shot machine: the VIC runs while something keeps arming it, and the ISR
shuts it down the instant it finds nothing armed. The ISR makes exactly **one**
`__wake_up` (0x17a8), so `frame_ready` has a single source and the cadence
question is entirely "how often does the ISR reach it".

**M145 answered that question: nobody re-arms per frame.** The steady-state
loop is `VideoCap_GetBuf` -> consumer -> `VideoCap_ReleaseBuf`, and `GetBuf`
calls **only** `VideoCap_GetBufVIC` (`0x8078e303`, a 120-byte read - exactly the
per-buffer struct size). The `VideoCap_StartVIC` call sits past `GetBuf`'s
literal pool, in a helper `VideoCap_Start` uses. The target-programming block at
`vpl_vic` 0x3a50 belongs to the **setup** ioctl `0x4028e302` (0x37c0..0x3a8c)
and runs once.

So: setup programs the target, `StartVIC` (`0xe313`) sets bit 10 plus `0xe8` in
the channel word at `[regs + chan*4 + 0x10]`, and the hardware **free-runs**.
The ISR only keeps it enabled - or halts it. **The card halting after one frame
is a hardware-state condition, not a missing call**, and
`VideoCap_ReleaseBufVIC` writing no VIC register is consistent rather than
suspicious.

The VIC ioctl map, for reference:

| wrapper | ioctl | vpl_vic dispatch |
|---|---|---|
| `VideoCap_WaitVIC` (and `VideoCap_Sleep`, a tail-jump to it) | `0xe301` | - |
| setup | `0x4028e302` | 0x37c0 |
| `VideoCap_GetBufVIC` | `0x8078e303` | 0x2b00 |
| `VideoCap_ReleaseBufVIC` | `0x4004e304` | 0x29d4 (built inline from the pooled `0x4020e305`) |
| `VideoCap_StartVIC` | `0xe313` | 0x3c54 |

### The one lead left in the ISR

At 0x16xx-0x1728 the ISR copies `[chan+0x200..0x20c]` into
`[block+0x278..0x284]`, then two extras gated on **software flags**:

    1728  [chan+0x210] == 1 -> [chan+0x214] into [block+0x2c8]
    173c  [chan+0x218] == 1 -> [block+0x204] bits 1:0 from [chan+0x21c]

Those flags are filled by the setup ioctl from the SDK's options - ultimately
from the cfg. It is the only remaining place where something host-influenced
changes what the ISR does per frame. Read that next; it is static and costs zero
spawns.

Still true and still worth knowing, closed by M144:

- **the release ioctl is not the re-arm.** `VideoCap_ReleaseBufVIC`
  (`0x4004e304`, dispatch 0x29d4) only compacts the file's queued list
  (`[file+0x10]`, count `[file+0x18]`), decrements `[buf+0x64]`, and branches to
  the force-release path at 0x4754 when `[file+0x1c]==1`. Per-buffer struct is
  **120 bytes**, array at `[dev+0x90]`. It writes **no** VIC register.
- **`capture_app_infinite` is an AUDIO app** - `libasound`, `TK_MMA_*`,
  `-R 48000 -F 256 -B 4`. There is no continuous-video reference app in the
  image.

</details>

</details>

<details>
<summary>M140's original framing of this step (superseded by M144)</summary>

1. **`vpl_vic.ko`'s buffer lifecycle.**
   The release path is `VideoCap_ReleaseBufVIC` = `ioctl(fd, 0x4004e304, idx)`,
   dispatched at **vpl_vic 0x29d4** (the tree builds that constant inline from
   the pooled `0x4020e305`, which is why grepping for `4004e304` finds nothing).
   It dequeues from the file's list and decrements a per-buffer refcount at
   `[buf+0x64]`; the `[file+0x1c]==1` branch at 0x4754 is the *force-release*
   path (it matches libvideocap's "[yuan][Sleep] Force Release Done"), not the
   normal one. The ISR's ready gate is `[chan+0x1c]`, set to **0xffff** by the
   `0x4028e302` setup ioctl at 0x3394.
   **Warning from experience:** `+0x1c` is a generic offset reused across a
   dozen structs in this module and there is no VIC register datasheet in the
   tree. This search did not converge in one sitting. Time-box it.

</details>

3. **`out_fmt` / `fw`.** The card's own cfg documents the enum inline -
   `output format (1:YUV420, 2:YUV422)` - but M79 found SET_VIC byte 12 never
   reaches the cfg (vcm passes it on tinyvenc's argv) and M82 settled it as a
   fractional-rate flag Windows sends 0 for. The cfg's output format comes from
   byte 6 (`fw`), where M88 established 5 is the only value that works. Both
   already worked; listed so nobody re-derives them.

4. **Possible mirror.** The "Boss" logo at top-left of
   `m130-colour-correct-frame.raw` looks mirrored. Check against the physical
   scene before touching `mirror`/`flip` - the camera may simply be pointed
   that way.

**Do NOT** sweep SET_VIC geometry again without first solving the ISP-allocation
problem (see the method rule above), and do NOT spend another spawn on any
completion or reporting handshake - credit, `enc_stat`, kicks, `vic_int_mode`,
EVENT. All of them are downstream of a capture that only ever happens once, and
M138/M139 proved the reporting path correct end to end and never reached.

## Tools

| thing | what it does |
|---|---|
| `mz0380-m55-real-capture.sh` | the real-path run. **The control capture is also the health check.** Always unloads on exit. |
| `mz0380-m130-chroma.py` | **the chroma oracle (M130).** `corr(chroma, luma)` per plane; 0 = CHROMA OK, 1 = CONTAMINATED/PARTIAL, 2 = no frame. Validated against `m129-first-real-frame.raw`. |
| `m129-first-real-frame.raw` | the first real captured frame, kept in-tree. planar 4:2:0, sha256 `76e7050e...cc8704`. |
| `mz0380-m127-splash.py` | **the oracle.** SHA-256s the 320x240 crop at (800,420) against `NOSG_LOGO_Y`. 1 = SPLASH, 0 = NOT SPLASH, 2 = NO FRAME. |
| `mz0380-live.sh` | `load` / `status` / `watch` / `unload`. Leaves the module loaded so OBS can open the node. Defaults `POLLDRAIN=20`. |
| `mz0380-m85-unload-smoke.sh` | run after every build. Load/unload, ~5 s, zero spawns. |
| `mz0380-build.sh` | sourced by the three loading scripts: build for the running kernel, else fall back to a prebuilt `ko/mz0380-$(uname -r).ko`, else show the real build error. |
| `mz0380-m83-i2c-devscan.sh` | zero-spawn I2C probe. `START=<reg>` picks the window (default `0x00`), `$1` the count. |
| `/proc/mz0380-periph-scan` | arbitrary I2C register dump; `periph_chip` takes any 8-bit address |
| `/proc/mz0380-buf0` | raw stream buffer 0 |
| `mz0380-m126-score.py` | **superseded** by m127-splash.py. |

New knob (M151): `enc_stat_ack` - clear enc_stat after each delivered frame,
default 1 (the M40 behaviour every prior result used). Setting 0 removes the
**last** write the driver makes to the card after START; measured, and the
cadence is unchanged, so no host-side write after START causes the stall.

New knob (M148): `bitstream_num` - SET_VIC byte 28, default 1. Hardcoded since
M22, swept once and negative; kept because the field is now settable for free.

m55/live env knobs: `VICFW VICM VICB0 VICINW VICINH VICINFMT WINSEQ INTX ENCSUB
OP6 WINBUFS OP8 PROBEWIN MSTOUT MSTAD SETBUF RXSTRAP POLLDRAIN POLLCREDIT
OP6KICK NOSRC MSTB1 MSTB2 MSTB5 B0LATE KICKOP KICKREP`, plus
`EXTRA="param=value ..."` and `SKIPSMOKE=1`.

### Re-unpacking the blob for static RE (read-only, NOT an upload)

```bash
tar xzf /usr/lib/firmware/mz0380/MZ0380.HD.HEX.stock -C <scratch>
llvm-objdump -d --triple=armv5te-linux-gnueabi <scratch>/yuan_demo_sdi/drivers/vpl_vic.ko
```

Everything is unstripped: `ep.ko`, `vpl_vic.ko`, `tinyvenc5`,
`video_capture_mgr`, `libvideocap.so.13`, `libtkmf_video_source.so.0`.
`libvideocap` is PIC and addresses strings as `GOT_base(0x18e4c) + literal`;
tinyvenc5 and video_capture_mgr are EXEC with plain absolute literals.

## Watching in OBS

```bash
sudo ./mz0380-live.sh load     # prints the node, leaves the module loaded
sudo ./mz0380-live.sh status   # signal, spawn count, last log lines
sudo ./mz0380-live.sh unload
```

Expect **one frame and then a frozen preview** - still true, but since M129 that
one frame can be real video rather than the splash. Load with
`fake_frame_off=1 post_mask=0 post_proc=1` to get it. Continuous streaming is
item 3 in "Where to go next".
Every OBS start/stop is one stream out of ~8-18 - turn off "Deactivate when not
showing" and do not leave it looping.

---

## Method rules - these keep being paid for

1. **A bisect step is only informative if every OTHER variable sits at a value
   already known to permit the outcome you are measuring.** Casualties so far:
   M76's "width hypothesis DEAD" (scored `captured 0 bytes`, structurally zero
   for *every* setting before M112 - re-measured and confirmed dead in M127g),
   the `win_seq` results, and the `fw=6` run.
2. **One structural change at a time.**
3. **A caveat in a source comment describes the state when it was written - it
   is not a statement about the current result set.** M127 burned two spawns
   re-running M96 because core.c:1008 says "that is exactly how the
   `vic_b0=0x20` test was wasted" (written about the *M73-era* attempt) and it
   was read as "`0x20` has never been validly measured". **Grep RE_FINDINGS for
   the actual result table before calling anything untested.**
4. **Byte-for-byte parity with the retail driver is a hypothesis generator, not
   a rule.** Same firmware, same opcode, same struct - and the value Windows
   sends breaks us (`fw=7`).
5. **Never let a test harness carry its own copy of a driver default.** Use
   `add_opt`, which passes a knob only when the caller sets it.
6. **grep can lie on these files.** The Windows I2C summary is extended-ASCII;
   plain `grep` prints nothing. Use `grep -a`.
7. **`sudo` resets the environment.** `START=0x90 sudo ./script` silently runs
   with the default; the variable must go AFTER `sudo`. Read the header line
   that echoes the setting actually used.
8. **`dmesg -C` runs at the top of m55 and m83.** A `dmesg | grep` issued after
   a later run has already lost the earlier one's evidence. Grep in the same
   command as the run, or not at all.
9. **Confirm the knob landed before reading the result.** Every m55 run echoes
   `b0=..` in three `output stage` lines and `vic_in=..x..`/`in_fmt=..` in the
   SET_VIC line. If they do not show what you set, the run measured nothing.

## Gotchas that still cost time

- This shell is **fish**, not bash. Put loops and parameter expansion in a
  `bash -c` or a script.
- System `objdump` has **no ARM support**. Use `llvm-objdump` or Ghidra headless
  (`-processor ARM:LE:32:v5`).
- `m55` runs `make` as root, leaving root-owned `*.o` that break the next
  non-root build. `mz0380-live.sh unload` cleans them up; otherwise `rm -f *.o`.
- Always reload the module - a stale one silently lacks new params.
- The sweep scripts grep-filter m55's output; read a full unfiltered run before
  concluding anything.
- The card console route is **dead**: redirecting it needs
  `yuan_start_process.sh` modified, i.e. an upload. See the hard constraint.
- The steady-state card boot path is `etc/rc.local` (which starts
  `video_capture_mgr -D -P 5`), **not** `yuan_start_process.sh` (the first-boot
  / restore path, which is the one that starts tinyvenc5 directly).
