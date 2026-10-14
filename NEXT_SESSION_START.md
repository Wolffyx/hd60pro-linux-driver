# NEXT SESSION START

_Last updated 2026-10-14. Full history in **RE_FINDINGS.md** (M1-M250); this
file is the handoff only._

## State (2026-10-14): M247 + M248 fully verified on hardware, kernel 7.2.9-2

**NEVER PCI-reset the card** (`reset`, `reset_method`, D3hot, remove/rescan):
it sits behind the X570 chipset, and a PM reset of the wedged card took the
whole chipset - storage, NICs, USB - and the host down (M249). Recovery from a
wedge is removing power only (PSU switch or mains, 30 s). A power-down that
leaves standby power on does not clear it: the card was still deaf after one.

Verify run 4 (fresh power cycle, now 3 spawns) passed with no `!!`: this
tree's COLD bring-up traced against HEAD's - every difference an intended M247
removal or source-timing-dependent polling; the receiver kept on reload;
compliance 48/48 with and without a source; YUYV keeps YU12; NO SIGNAL across
an unplug with no spawn. The verify script now stops at once on a deaf card.

## Before that (2026-10-12): M247 cleanup + M248 fixes

Video works (1920x1080 raw `YU12`/`NV12`/`YV12` and `H264`, 50-60 fps,
v4l2-compliance clean as of M245/M244). HDMI audio transport proven (M246),
content unverified. `sudo modprobe mz0380` needs no parameters.

M247 cut the driver from 21k to 10k lines
and the module parameters from 148 to 17, removing every path the default
configuration never reaches. Two read-only audits found the card's command
sequence on the default path identical to HEAD `4a5c270` apart from intended
removals (see RE_FINDINGS M247 for the list), and the correctness review's
findings were fixed. It builds clean with `W=1` on 6.18 LTS (gcc) and 7.2
(clang).

UX changes a user will notice: a NO SIGNAL picture on raw nodes too, YUYV
requests no longer flip the node to H.264, idle ioctls answer at once with no
source, a handful of dmesg lines instead of ~90 per load/start, `v4l2-ctl
--log-status`, and a README written for users.

M248 came out of the first verify run (RE_FINDINGS M248):
the mailbox trace was empty (dyndbg quoting, fixed); v4l2-compliance failed 3
tests because it ran with no source and G_DV_TIMINGS returned empty timings
(fixed: 1080p60 default; set timings now size the format); and every load
reset the receiver, which makes a camera leave HDMI mode (fixed: a reload keeps
a receiver that is already up - only the first load after power-up resets it).

Run 2 (no cold boot) passed everything it could test: receiver kept at every
load, lock 0.2 s after load, encoder commands identical to HEAD, compliance
48/48 with and without a source, NO SIGNAL across an unplug. Run 3 put the
YUYV check on the driver for the first time (v4l2-ctl refuses unknown fourccs
itself; now raw ioctls): YUYV and MJPG keep YU12. One gap left: this tree's
COLD bring-up is untraced (the receiver was kept at every load).
**Power cycle total is 7 spawns** (OBS spent one) - cold boot before the
next capture.

## Next, in order

Any hardware run: OBS closed (it grabs the node at load and spends spawns).
`sudo scripts/mz0380-m247-verify.sh` is the regression check (3 spawns;
`fourcc` and `coldtrace` modes are zero-spawn). No Claude attribution in
commit or PR text.

1. **M247 chroma rule, needs a DIGITAL black** (M250): the DSLR's black has
   chroma noise and never reaches the neutral path (best 9/32 exact samples),
   so run `sudo scripts/mz0380-m247-black-test.sh` (1 spawn) with a laptop or
   phone over HDMI showing a full-screen black image, or a camera playing
   back a black photo. Pass: 16/16-neutral frames delivered at the source
   rate and all counted as `colourless passed`. (Lens-cap black: no freeze,
   30 fps throughout - already measured.)
   **New lead, same script's data:** one frame in eight arrives ~0 ms after
   its predecessor and the next ~66 ms later (average exactly 30 fps). Judder
   candidate; measure with the driver's vb2 timestamps.
2. **M246 audio with a source that has sound** (`sudo
   scripts/mz0380-m246-audio-test.sh`): channel order and byte order are the
   only open questions.
3. **Suspend/resume and `.shutdown`** (M245, still untested). M247 fixed one
   resume defect statically: SET_AIC is re-sent after a card cold boot.
4. **M232** (encoder goes silent) - not reproduced; suspect two consumers.
   `scripts/mz0380-node-holders.sh` names them.

## Running it

```bash
sudo ./mz0380-live.sh load            # build, smoke test, insmod, print node
sudo ./mz0380-live.sh status          # signal, spawns, counters
sudo ./mz0380-live.sh unload
sudo TRACE=1 ./mz0380-live.sh load    # also log every mailbox command
sudo KO=ko/baseline/mz0380.ko TRACE=1 ./mz0380-live.sh load   # HEAD 4a5c270
```

The node number is not stable; find it by name (`HD60 Pro HDMI capture`).
OBS: Video Format **NV12** (it does not offer YU12).

## Budget - READ BEFORE ANY HARDWARE RUN

The card wedges somewhere in **8-18 encoder spawns per power cycle**; a wedge
costs a mains-off cold boot. `scripts/mz0380-spawns.sh` keeps the tally in
`/run` (a reboot resets it - only reset it by hand after a real power cut).
What spends one: the first capture after a load, a raw<->H.264 switch, an
H.264 bitrate/GOP change before an H.264 capture, a source that returns in a
different mode. Reattach, unplug/replug of the same mode, and switching between
YU12/NV12/YV12 are free. `ffplay` is the cheap consumer; OBS retry loops are not.

## Retracted or refuted - read before re-proposing any of them

- **M157 PCI-reset recovery** (pm / bus / remove+rescan on a wedged card):
  the first stage hung the X570 chipset and crashed the host (M249). Closed.

- **M228** deferral: a slot the card cleared stays "landed" forever, so a
  second observation by itself proves nothing. (M247's chroma second look is
  timed - one frame period - for that reason.)
- **M234** full-range labelling: measured while the picture drifted bright.
- **M237** CSC staleness: 0x48 reads `d2` at both 50 and 60 Hz.
- **M243** brightness steps: the camera's auto exposure, not the driver.

The pattern: a number measured while another defect was active, or a fix
shipped before its own falsifier was read. **Put a counter in every fix and
read it before believing the fix.**

## Deliberately NOT done

- **Zero-copy raw delivery.** The 3.1 MB copy per frame is the largest cost,
  but removing it means re-pointing the card at each vb2 buffer per frame - a
  mailbox command in the delivery path of a card that wedges.
- **SOURCE_CHANGE on every loss/return.** Only a mode change raises it:
  GStreamer renegotiates on each event, and the NO SIGNAL picture already
  covers a plain unplug.
- **The mailbox's trailing-event wait** (~4 ms per command after completion,
  `mz0380-mailbox.c`). Probably removable, but it may be pacing the card's I2C
  proxy; measure with the trace timestamps before touching it.

## Tools

- `scripts/mz0380-m247-verify.sh` - the M247/M248 check above.
- `scripts/mz0380-m247-black-test.sh` - raw capture through a black screen (M250).
- `scripts/mz0380-m226-blackframe-scan.py` - reads delivered bytes: blank,
  partial, torn, duplicate frames, luma/chroma ranges. Touches no mailbox.
- `scripts/mz0380-m230-flicker-watch.sh` - counter watch, prints on change.
  `/proc/mz0380-state` costs no card traffic at any rate now.
- `scripts/mz0380-m238-chroma-test.sh`, `scripts/mz0380-m243-brightness-ab.sh`.
- `scripts/mz0380-compliance.sh` - v4l2-compliance, zero spawns.
- `scripts/mz0380-source-check.sh` - what is the source sending, zero spawns.
- `scripts/mz0380-m85-unload-smoke.sh` - load/unload with no node, zero spawns.
- `/proc/mz0380-hdmi` (root): `watch`, `csc`, `hpd`, `edid`.
