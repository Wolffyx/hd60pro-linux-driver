# Next Session Summary

## Current Driver State
- The project focus is now explicitly:
  - implement a working Linux driver for the card
  - use correlation, V4L2, and procfs experiments only when they directly help driver bring-up
  - welcome any test that materially advances driver implementation
- The probe-safe `mz0380` path is working for:
  - PCI probe
  - BAR0 and BAR5 mapping
  - `/proc/mz0380`
  - `/proc/mz0380-state`
  - `/proc/mz0380-snapshot`
  - `/proc/mz0380-control`
- The driver now also:
  - seeds the cached input state from persisted BAR5 `0x0040[2:0]` during probe
  - seeds cached record mode from fixed BAR5 `0x0058[1:0]` during probe
  - seeds cached bitrate from fixed BAR5 `0x005c` during probe
  - seeds cached quality from fixed BAR5 `0x0060` during probe
  - seeds cached GOP from fixed BAR5 `0x0080` during probe
  - seeds cached B-frames from fixed BAR5 `0x0088` during probe
  - seeds cached QP step from fixed BAR5 `0x0084` during probe
  - exposes fixed hardware-backed property `411` writes through procfs
  - keeps B-frames visible in `/proc/mz0380-state`, `/proc/mz0380-experiment`, and the probe-safe V4L2 node
  - exposes fixed hardware-backed property `411` through the probe-safe V4L2 node
  - shows both cached input and hardware-derived input in `/proc/mz0380-state`
  - exposes fixed hardware-backed property `201/403/404/405/407/408/411` writes through procfs
  - exposes fixed hardware-backed property `404`, `405`, and `411` through the probe-safe V4L2 node
- The reusable probe script is:
  - [mz0380-correlation.sh](/home/wolffyx/Projects/sc0710/mz0380-correlation.sh)
- The script now:
  - builds `mz0380.ko`
  - loads declared dependencies first via `modprobe`
  - loads `mz0380.ko` in probe-safe mode
  - captures before and after snapshots
  - prints diffs
  - prints recent kernel log lines

## Confirmed Control Mappings
- Property `201` input select is sufficiently confirmed for driver work:
  - BAR5 `0x0040[2:0]`
  - `0=HDMI`
  - `1=DVI-D`
  - `2=COMPONENTS (YCbCr)`
  - `3=DVI-A (RGB)`
  - `4=SDI`
  - full `0,1,2,3,4` sweep tracked exactly
  - only BAR5 `0x0040` changed during the tested runs
  - probe-time sync from persisted hardware state works
  - procfs and V4L2 now agree on the active input selection
- Property `407` record mode is sufficiently confirmed for driver work:
  - BAR5 `0x0058[1:0]`
  - `0=VBR`
  - `1=CBR`
  - `2=HBR`
  - only BAR5 `0x0058` changed during the tested runs
  - readback matched every programmed value
  - reload persistence was observed
- Property `403` bitrate is sufficiently confirmed for driver work:
  - BAR5 `0x005c` with `mask=0xffffffff shift=0`
  - tested values:
    - `6291456 -> 0x00600000`
    - `8388608 -> 0x00800000`
    - `12582912 -> 0x00c00000`
  - only BAR5 `0x005c` changed during the tested runs
  - readback matched every programmed value
  - reload parity confirmed probe-time cached bitrate sync from hardware
- Property `404` quality is sufficiently confirmed for driver work:
  - BAR5 `0x0060` with `mask=0xffffffff shift=0`
  - tested values:
    - `0 -> 0x00000000`
    - `50 -> 0x00000032`
    - `80 -> 0x00000050`
    - `100 -> 0x00000064`
  - only BAR5 `0x0060` changed during the tested runs
  - readback matched every programmed value
  - reload parity confirmed probe-time cached quality sync from hardware
- Property `405` GOP is now sufficiently confirmed for driver work:
  - BAR5 `0x0080` with `mask=0xffffffff shift=0`
  - tested values:
    - `30 -> 0x0000001e`
    - `60 -> 0x0000003c`
    - `120 -> 0x00000078`
  - only BAR5 `0x0080` changed during the tested runs
  - readback matched every programmed value
  - reload parity confirmed probe-time cached GOP sync from hardware
  - procfs and `/proc/mz0380-state` tracked the same value across the tested sweep
- Property `408` QP step is now sufficiently confirmed for driver work:
  - BAR5 `0x0084` with `mask=0xffffffff shift=0`
  - tested values:
    - `0 -> 0x00000000`
    - `4 -> 0x00000004`
    - `8 -> 0x00000008`
  - only BAR5 `0x0084` changed during the tested runs
  - readback matched every programmed value
  - reload parity confirmed probe-time cached QP-step sync from hardware
  - procfs and `/proc/mz0380-state` tracked the same value across the tested sweep
- Property `411` B-frames is now sufficiently confirmed for driver work:
  - BAR5 `0x0088` with `mask=0xffffffff shift=0`
  - tested values:
    - `0 -> 0x00000000`
    - `1 -> 0x00000001`
    - `2 -> 0x00000002`
  - only BAR5 `0x0088` changed during the tested runs
  - readback matched every programmed value
  - reload parity confirmed probe-time cached B-frame sync from hardware
  - procfs, `/proc/mz0380-state`, and `v4l2-ctl --all` tracked the same value across the tested sweep

## Current Experiment Interface
- New proc entry:
  - `/proc/mz0380-experiment`
- Supported runtime commands:
  - `echo 'input <0..4>' > /proc/mz0380-experiment`
  - `echo 'candidate <reg> <mask> <shift>' > /proc/mz0380-experiment`
  - `echo 'candidate clear' > /proc/mz0380-experiment`
  - `echo 'recordmode <0..2>' > /proc/mz0380-experiment`
  - `echo 'recordmode-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment`
  - `echo 'recordmode-candidate clear' > /proc/mz0380-experiment`
  - `echo 'bitrate <value>' > /proc/mz0380-experiment`
  - `echo 'bitrate-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment`
  - `echo 'bitrate-candidate clear' > /proc/mz0380-experiment`
  - `echo 'quality <value>' > /proc/mz0380-experiment`
  - `echo 'quality-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment`
  - `echo 'quality-candidate clear' > /proc/mz0380-experiment`
  - `echo 'gop <value>' > /proc/mz0380-experiment`
  - `echo 'gop-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment`
  - `echo 'gop-candidate clear' > /proc/mz0380-experiment`
  - `echo 'bframes <0..2>' > /proc/mz0380-experiment`
  - `echo 'bframes-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment`
  - `echo 'bframes-candidate clear' > /proc/mz0380-experiment`
  - `echo 'qpstep <value>' > /proc/mz0380-experiment`
  - `echo 'qpstep-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment`
  - `echo 'qpstep-candidate clear' > /proc/mz0380-experiment`
  - `echo 'writes on' > /proc/mz0380-experiment`
  - `echo 'writes off' > /proc/mz0380-experiment`
- The correlation script now uses that runtime path when `--input-reg` is provided, so the
  before-capture artifacts include the configured candidate field and write-enable state.
- The correlation script also supports:
  - `--input-seq 0,1,2,3,4`
  - `--record-mode 0|1|2`
  - `--record-mode-reg <reg>`
  - `--bitrate <value>`
  - `--bitrate-seq v1,v2,...`
  - `--bitrate-reg <reg>`
  - `--bitrate-mask <mask>`
  - `--bitrate-shift <shift>`
  - `--quality <value>`
  - `--quality-seq v1,v2,...`
  - `--quality-reg <reg>`
  - `--quality-mask <mask>`
  - `--quality-shift <shift>`
  - `--gop <value>`
  - `--gop-seq v1,v2,...`
  - `--gop-reg <reg>`
  - `--gop-mask <mask>`
  - `--gop-shift <shift>`
  - `--b-frames <value>`
  - `--b-frames-seq v1,v2,...`
  - `--b-frames-reg <reg>`
  - `--b-frames-mask <mask>`
  - `--b-frames-shift <shift>`
  - `--qp-step <value>`
  - `--qp-step-seq v1,v2,...`
  - `--qp-step-reg <reg>`
  - `--qp-step-mask <mask>`
  - `--qp-step-shift <shift>`
  - `--input-api procfs|v4l2`
  - `--enable-video`
  - `--video-node /dev/videoN`
  so one candidate field can be exercised across multiple values in one run.
- The script now also prints a `sequence summary` section and saves per-step:
  - `control.seq*.txt`
  - `experiment.seq*.txt`
  - `snapshot.seq*.txt`
  - `state.seq*.txt`
- When the probe-safe V4L2 node is enabled and `v4l2-ctl` is available, the script also saves:
  - `v4l2.all.*.txt`
  - `v4l2.input.*.txt`
  - `v4l2.inputs.*.txt`
- `/proc/mz0380-state` now initializes and keeps showing the cached encoded-first control
  model even when `enable_video=0`, so software-side input changes appear there correctly.
- That state path now also reports the direct BAR5-derived hardware input so reload tests can
  distinguish persisted device state from the cached linux-side model.
- When `--input-api v4l2` is used, the script auto-loads `enable_video=1`, resolves the
  registered `/dev/videoN` node, and issues the same input selection through `v4l2-ctl`.
- Even when `--input-api procfs` is used, the script now captures V4L2-visible state too if
  `enable_video=1` and a video node is present, so procfs and V4L2 views can be compared in
  the same run.

## BAR Snapshot State
- Stable structured BAR5 words still observed:
  - `0x0000 = 11000001`
  - `0x0004 = 00010001`
  - `0x0008 = 03aac004`
  - `0x0018 = c7000030`
  - `0x0030 = fc200004 -> BAR0+0x4`
  - `0x0038 = fc20005f -> BAR0+0x5f (unaligned)`
  - `0x0050 = 00400208`
- The SC0710-inspired BAR0 experimental window remained all `ffffffff`.
- Current interpretation:
  - BAR5 low and mid ranges look more like static control or mailbox structure than live HDMI-presence bits.
  - BAR0 still looks data-plane or DMA-adjacent.

## Best Next Step
- Stop spending more time on property `201` as a primary target.
- Keep BAR5 `0x0040[2:0]` as implemented property-201 evidence inside the driver.
- Keep BAR5 `0x0058[1:0]` as the current property-407 field.
- Keep BAR5 `0x005c` as the current property-403 field.
- Keep BAR5 `0x0060` as the current property-404 field.
- Keep BAR5 `0x0080` as the current property-405 field.
- Keep BAR5 `0x0088` as the current property-411 field.
- Keep BAR5 `0x0084` as the current property-408 field.
- The main plan is now driver implementation for the card, not wider blind correlation for its own sake.
- Tests are still welcome, but only when they directly support implementation decisions, verification, or bring-up safety.
- `411` is now grounded enough to treat as the next implemented syntax-family control after `201`, `407`, `403`, `404`, `405`, and `408`.
- The next sensible move is to inspect the next post-`411` syntax-family target rather than re-proving `411`.

## Next Session Implementation Target
- Start validating one new control family beyond the already-confirmed `201`, `407`, `403`, `404`, `405`, `408`, and `411` paths.
- The concrete next-session goals should be:
  - keep the now-confirmed property-`411` path stable in procfs, state, and V4L2
  - inspect the next `411`-adjacent syntax-family control in the same BAR5 `0x0080..0x00bc` window
  - capture before/programmed/readback values and BAR5 deltas for the next bounded target
  - keep the V4L2 node probe-safe while preparing the next real hardware-backed syntax control
- Do not spend the next session on wider blind BAR walks unless a specific implementation hypothesis requires it.

## Most Useful Confirmed Commands
For record mode:

```bash
cd /home/wolffyx/Projects/sc0710
scripts/mz0380-correlation.sh --profile 5 --record-mode 2 --record-mode-reg 0x0058 --keep-loaded
```

For bitrate:

```bash
cd /home/wolffyx/Projects/sc0710
scripts/mz0380-correlation.sh --profile 5 --bitrate-seq 6291456,8388608,12582912 --bitrate-reg 0x005c --keep-loaded
```

For quality:

```bash
cd /home/wolffyx/Projects/sc0710
scripts/mz0380-correlation.sh --profile 5 --quality-seq 0,50,80,100 --quality-reg 0x0060 --keep-loaded
```

For GOP:

```bash
cd /home/wolffyx/Projects/sc0710
scripts/mz0380-correlation.sh --profile 5 --gop-seq 30,60,120 --enable-video --keep-loaded
```

For QP step:

```bash
cd /home/wolffyx/Projects/sc0710
scripts/mz0380-correlation.sh --profile 5 --qp-step-seq 0,4,8 --enable-video --keep-loaded
```

## Recommended Resume Commands
```bash
cd /home/wolffyx/Projects/sc0710
sed -n '1,220p' PLAN.md
sed -n '1,260p' NEXT_SESSION_SUMMARY.md
```

Then inspect the now-confirmed `405/408` paths and the next syntax-family candidates before editing more code:

```bash
cd /home/wolffyx/Projects/sc0710
rg -n "gop|qpstep|405|408|411" mz0380-core.c mz0380-video.c mz0380-cards.c mz0380.h mz0380-correlation.sh
```

The next quick sanity-check command for the confirmed QP-step path should look like:

```bash
cd /home/wolffyx/Projects/sc0710
sudo rmmod mz0380
scripts/mz0380-correlation.sh --no-build --profile 5 --qp-step-seq 0,4,8 --enable-video --keep-loaded
```

`0x0080`, `0x0084`, and `0x0088` are now confirmed as the BAR5 property-405, property-408, and property-411 fields. The next syntax-family search should stay in the same BAR5 `0x0080..0x00bc` window before considering any wider search.

The current quick-start command for re-checking the grounded property-411 path should be:

```bash
cd /home/wolffyx/Projects/sc0710
sudo rmmod mz0380
scripts/mz0380-correlation.sh --no-build --profile 5 --b-frames-seq 0,1,2 --enable-video --keep-loaded
```
