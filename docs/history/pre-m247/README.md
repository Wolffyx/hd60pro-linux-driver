# Pre-M247 diagnostics and experiment scripts

This directory is a record, not part of the driver. Nothing here is built,
installed or run.

M247 cut the driver to its default path: about 21k lines down to 10k, and 148
module parameters down to 17. The code and scripts that reverse-engineered the
card got it to the point where that cut was possible. They are kept here,
unchanged, as they were at commit `4a5c270`, so the work behind
`RE_FINDINGS.md` can be read next to its conclusions.

**Do not copy these back into the build.** They depend on module parameters,
structure fields and `/proc` commands that no longer exist. The scripts are
stored without the executable bit for the same reason.

## `src/` - 14 files, about 7,300 lines

| File | What it was |
|---|---|
| `mz0380-proc-debug.c` | `/proc` debug handlers: register scans, event log, HDMI commands, raw mailbox commands |
| `mz0380-proc-diagnostics.c` | PCI config, control-field and experiment state dumps |
| `mz0380-snapshot.c` | the safe-register snapshot tables and helpers |
| `mz0380-video-state.c` | human-readable V4L2 state dump |
| `mz0380-dma-extent.c` | DMA buffer poisoning and write-extent measurement (M36-M38) |
| `mz0380-dma-nosg.c` | the no-scatter-gather polling capture fallback (M41, M50) |
| `mz0380-mst3367-bitbang.c` | bit-banged I2C on spare GPIO pins, EDID EEPROM hunt (M51) |
| `mz0380-controls-read.c`, `mz0380-controls-write.c` | control-field discovery, readback and validated write experiments |
| `mz0380-params-base.c`, `mz0380-params-signal.c`, `mz0380-params-windows.c` | the experiment module parameters, each tagged with its milestone |
| `mz0380-internal.h`, `mz0380-core.c` | the internal header and module boilerplate those files shared |

## `scripts/` - 48 files

One script per hardware experiment, named after the milestone in
`RE_FINDINGS.md` that it settled (`mz0380-mNN-*.sh`), plus the early bring-up,
signal-hunt and frame-scoring tools. Each script's header says what question it
asked and how to read the answer.

Two warnings:

- **`mz0380-m52-card-recovery.sh` PCI-resets the card.** Its first stage hung
  the X570 chipset and crashed the host (M249). A guard was added at the top
  when it was archived so it cannot run by accident; the rest is unchanged.
  A wedged card is recovered by removing power only.
- The scripts that mention a firmware upload describe a path that was deleted
  long before this snapshot. The driver never uploads anything to the card.
