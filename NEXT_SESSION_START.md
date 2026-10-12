# NEXT SESSION START

_Last updated 2026-10-12. Full history in **RE_FINDINGS.md** (M1-M248); this
file is the handoff only._

## State (2026-10-12): M247 cleanup + M248 fixes, verified and committed

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

1. **At the next cold boot, `sudo scripts/mz0380-m247-verify.sh`** once more
   for the cold bring-up trace (3 spawns; prompts for unplug/replug twice).
   Pass = no `!!` lines. (`... verify.sh fourcc` alone: the zero-spawn YUYV
   check on a loaded module.) Commit and PR text: no Claude attribution.
   Predictions, written before the run:
   - Phase 1: both traces non-empty (bring-up included - this tree goes first,
     so both loads reset the receiver); identical SET_VIC/SET_ENC/SET_PREVIEW/
     SET_BUF/SET_AIC/START/STOP; the rest of the diff is receiver traffic.
   - Phase 2: `receiver kept at 2 of 2 loads`; the source locks in under 2 s
     (it took 3-7 s when each load reset the receiver); the camera visibly
     stays in HDMI output through the load.
   - v4l2-compliance `Failed: 0` both with the source and with it unplugged.
   - YUYV TRY_FMT answers YU12; unplug during raw capture raises
     `raw NO SIGNAL frames`, replug ends it with no new spawn.
   If a trace differs, the diff names the command.
2. **M247 chroma rule, one more measurement:** show a black screen from the
   source during a raw capture. Before M247 raw froze on the last frame before
   the fade (prediction); now `colourless frames passed on a second look`
   should climb and the picture should follow the source.
3. **M246 audio with a source that has sound** (`sudo
   scripts/mz0380-m246-audio-test.sh`): channel order and byte order are the
   only open questions.
4. **Suspend/resume and `.shutdown`** (M245, still untested). M247 fixed one
   resume defect statically: SET_AIC is re-sent after a card cold boot.
5. **M232** (encoder goes silent) - not reproduced; suspect two consumers.
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
- `scripts/mz0380-m226-blackframe-scan.py` - reads delivered bytes: blank,
  partial, torn, duplicate frames, luma/chroma ranges. Touches no mailbox.
- `scripts/mz0380-m230-flicker-watch.sh` - counter watch, prints on change.
  `/proc/mz0380-state` costs no card traffic at any rate now.
- `scripts/mz0380-m238-chroma-test.sh`, `scripts/mz0380-m243-brightness-ab.sh`.
- `scripts/mz0380-compliance.sh` - v4l2-compliance, zero spawns.
- `scripts/mz0380-source-check.sh` - what is the source sending, zero spawns.
- `scripts/mz0380-m85-unload-smoke.sh` - load/unload with no node, zero spawns.
- `/proc/mz0380-hdmi` (root): `watch`, `csc`, `hpd`, `edid`.
