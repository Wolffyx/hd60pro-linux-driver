> **SUPERSEDED — historical bring-up plan, 2026-08-24.**
>
> This file describes the driver as it was imagined at bring-up and several of
> its central claims are now false. It is kept for the reasoning, not as a
> reference. Current state lives in `README.md`, `NEXT_SESSION_START.md` and
> `RE_FINDINGS.md`.
>
> Specifically, do not act on the following, which appear below:
>
> - **"Firmware loader ... chunked BAR5 scratch-window upload state machine"**
>   and the `firmware_upload` gate. **The driver does not upload firmware and
>   the code to do so has been deleted from the tree.** The card boots its own
>   flash image; a previous attempt at a host-side upload broke the card's
>   userspace. This is a standing rule, not a preference.
> - **"All default OFF so probe stays safe"** — `enable_video`, `enable_dma` and
>   `dma_handshake` have defaulted ON since M166, because the old defaults
>   produced a device that could not capture.
> - **"exposes the encoded result as a V4L2 H.264 capture device"** — the
>   delivered payload is raw planar I420, not an H.264 bitstream (M129/M146).
> - The BAR5 mailbox description — the command mailbox is in **BAR0**, not BAR5.

# MZ0380 / HD60 Pro Driver — Implementation Status

## Current state (post-build)

Full driver scaffolding for the Elgato HD60 Pro family is in place:

- PCI probe binds `12ab:0380` (+ all known Elgato subsys variants)
  and `12ab:0381` (Rev. 3).
- BAR0 (1 MiB MMIO) and BAR5 (4 KiB control) mapped.
- Firmware loader (`mz0380-fw.c`) — `request_firmware()` plus a chunked
  BAR5 scratch-window upload state machine.
- DMA + MSI (`mz0380-dma.c`) — coherent ring alloc, ring-base
  programming, MSI ISR, work-queue drain.
- V4L2 streaming (`mz0380-video.c`) — vb2 queue, `REQBUFS`/`QBUF`/
  `DQBUF`/`STREAMON`/`STREAMOFF`, plus HDMI signal detect via
  `VIDIOC_QUERY_DV_TIMINGS` and `V4L2_EVENT_SOURCE_CHANGE`.
- ALSA HDMI audio (`mz0380-audio.c`) — single capture substream,
  S16_LE 32/44.1/48 kHz, period-elapsed driven by the audio ring.
- Module gates: `firmware_upload`, `enable_dma`, `enable_audio`. All
  default OFF so probe stays safe.
- Build: `make` produces `mz0380.ko` only. The legacy `sc0710` source
  set is excluded by default (`MZ0380_LEGACY_SC0710=1` to attempt it).

## Confirmed control-plane facts (preserved from earlier sessions)

- Subsystem: `1cfa:0006` ("Rev. 1 + Ryzen fix") locally.
- PCIe: Gen1 x1, MSI capable, IRQ 40 on this host.
- BAR5 property param window (verified bounded-write):

  | prop | reg     | mask         | name        |
  |------|---------|--------------|-------------|
  | 201  | `0x0040`| `0x7`        | input       |
  | 407  | `0x0058`| `0x3`        | record mode |
  | 403  | `0x005c`| `0xffffffff` | bitrate     |
  | 404  | `0x0060`| `0xffffffff` | quality     |
  | 405  | `0x0080`| `0xffffffff` | GOP         |
  | 408  | `0x0084`| `0xffffffff` | QP step     |
  | 411  | `0x0088`| `0xffffffff` | B-frames    |

- Enabling PCI bus mastering before DMA setup triggered an
  `AMD-Vi IO_PAGE_FAULT` at `0x90000000`. Driver therefore enables bus
  mastering only inside `mz0380_dma_setup()`, after ring base addrs
  are programmed.

## Windows reconnaissance findings

Driver: `e60MZ0380.X64.SYS` 3.7 MB, WDM/KMDF. Notable symbols from
strings:

- `MZ0380_DownloadBaseFirmware`, `MZ0380_DownloadFirmware`,
  `MZ0380_GetFirmwareVersion`, `MZ0380_SEND_COMMAND`
- `MZ0380_HwInitialize`, `[FIRMWARE RESET]`
- `Interrupt_Handler`, `MmAllocateContiguousMemorySpecifyCache`,
  `IoConnectInterrupt`
- `MST3367_HDMI_MODE_DETECT( R0055 )` — HDMI signal detect chip
- `TP2834_SET_VIDEO_MODE` — H.264 encoder
- `GetHDMIDotClock => audio sample freq = %d` — audio clock derivation

Firmware: `MZ0380.HD.HEX` is gzipped tar of `yuan_demo_sdi/` — full
embedded Linux for the card's ARMv5 SoC, including `tinyvenc7`
encoder, `video_capture_mgr`, `audio_capture_mgr`, and the device-side
`drivers/ep.ko` which advertises:

- sysfs nodes: `command`, `status`, `param0..N`, `fw_store`
- ISR `pciep_isr` + `msi_enable`
- ring control: `[init]bar 0..1`, `Inbound mem0 start/Limit`
- multi-channel status: `enc_stat0..15`, `hready/ency_ready/aency_ready`
- commands: `BEGIN_FIRMWARE_DOWNLOAD`, `BEGIN_BASE_FIRMWARE_DOWNLOAD`,
  `SET_VIC_PARAMS`, `STOP_STREAMING`, `GET_FIRMWARE_VERSION`,
  `SET_AIC`

This evidence is what `mz0380-reg.h` is built around. The exact BAR5
offsets for command/status/param/firmware-buffer/IRQ-status are
marked **`CHECKME`** until verified against the live device.

## What needs verification before flipping the gates

1. **Command/status register offsets** (`MZ0380_REG_COMMAND`,
   `MZ0380_REG_STATUS`, `MZ0380_REG_PARAM(i)`).
   - Method: load probe-safe, write a known opcode like
     `GET_FW_VERSION` at the hypothesised offset and watch
     `/proc/mz0380-experiment` snapshot diffs.
   - Alternative: do real Ghidra/radare2 disasm of
     `e60MZ0380.X64.SYS` around the `MZ0380_SEND_COMMAND` string xref.

2. **Firmware upload window** (`MZ0380_REG_FW_BUFFER`,
   `MZ0380_REG_FW_CHUNK_SEQ`, `MZ0380_REG_FW_CHUNK_ACK`).
   - Method: with `firmware_upload=1`, watch dmesg for the
     "firmware chunk %u not acked" timeout; if it stays at 0, the
     SEQ register is wrong; if ACK stays at the previous value,
     the buffer offset is wrong.

3. **IRQ status/mask/ack offsets** (`MZ0380_REG_IRQ_*`).
   - Method: load with `enable_dma=1`, watch `/proc/interrupts` for
     IRQ 40 hits. Zero hits == wrong mask reg.

4. **Ring base/head/tail registers** + descriptor layout.
   - Method: after firmware boots, hexdump the first 1KiB of the
     coherent video ring; H.264 NAL prefix `00 00 00 01` should
     appear in the payload area of the first slot once a source
     is connected.

## Verification gates

| Gate                       | Pass criterion                            |
|---                         |---                                        |
| P0 register precision      | `cat /proc/mz0380-state` shows the new fw/IRQ/ring fields without errors |
| P1 firmware ready          | `dmesg` shows `firmware version 1.11` after `firmware_upload=1` |
| P2 IRQ flowing             | `cat /proc/interrupts | grep mz0380` non-zero after HDMI plug-in |
| P3 first H.264 NAL         | `ffmpeg -f v4l2 -pixel_format h264 -i /dev/video0 -t 5 out.h264` and `ffprobe out.h264` reports `h264 1920x1080` |
| P4 signal detect           | `v4l2-ctl --query-dv-timings` reports active timings; unplug HDMI -> reports no signal |
| P5 audio capture           | `arecord -D hw:CARD=mz0380,DEV=0 -d 30 out.wav` no underruns |
| P6 mainline-ready          | `make` clean on fresh checkout, `checkpatch.pl` mostly clean |

## Risks still open

- Subsystem `1cfa:0006` is Rev.1+Ryzen — same firmware as Rev.1 per
  INF — but never live-tested as such. If P1 hangs, retry with
  `MZ0381.HD.HEX` rename or read the FW.TXT inside the blob.
- IOMMU still a real concern. Ring base programming MUST happen
  before `pci_set_master`; the driver enforces this but only if the
  CHECKME ring-base offsets are correct.
- Firmware redistribution: shipping `MZ0380.HD.HEX` in this repo
  needs YUAN/Elgato permission. Until then, README instructs users
  to copy it from a Windows install.

## sc0710 cross-reference findings (added after Phase 7)

After auditing the in-tree sc0710 driver (4K60 Pro Mk.2, same vendor)
and the AMESDK 1.1.0.202.0 sample profiles, the following higher-
confidence hypotheses replace earlier blind CHECKME values:

1. **BAR0 stays asleep until firmware boots.** Pre-boot BAR0 reads
   return `0xffffffff` (confirmed locally). This is consistent with a
   Xilinx FPGA fabric that gates its register file behind a "fabric
   reset" lifted only after the onboard ARM downloads the bitstream
   logic from the firmware blob. Therefore firmware MUST upload via
   BAR5 BEFORE any BAR0 register access.

2. **Xilinx XDMA DMA controller layout** (from sc0710-dma-channel.c
   lines 451-469):
   ```
   base+0x04  ctrl
   base+0x08  ctrl_w1s
   base+0x0c  ctrl_w1c
   base+0x40  status1
   base+0x44  status2
   base+0x48  completed-descriptor-count
   base+0x88  poll_wba_l
   base+0x8c  poll_wba_h
   sg_base = base+0x4000
   sg_base+0x80  sg_start_l
   sg_base+0x84  sg_start_h
   sg_base+0x88  sg_adj
   sg_base+0x8c  sg_credits
   ```
   8-DWORD descriptor: control, lengthBytes, src_l, src_h, dst_l,
   dst_h, next_l, next_h. The driver now has `mz0380_dma_start_xdma()`
   as a Plan B alternative to the BAR5 mailbox ring path.

3. **HDMI status window at BAR0 0x00a8..0x00e4** (sc0710-reg.h):
   - `0x00a8` source width in upper 16 bits
   - `0x00c4` `0x00f0000` idle indicator
   - `0x00c8` source height
   - `0x00d0` `0x4100` idle / `0x4101` streaming, with mode-bits
   - `0x00d4` source format hash
   - `0x00e4` bit0 = streaming flag

   The new `mz0380_signal_from_bar0()` reads these as a fallback when
   the mailbox `QUERY_SIGNAL` opcode is wrong or times out.

4. **Xilinx AXI IIC at BAR0 0x3100..0x310c** (sc0710-i2c.c). Used by
   sc0710 to read the HDMI source EDID. Same chip family likely
   exposes the same IP. EDID reader for mz0380 not yet implemented;
   reg.h has the offsets pre-named for Phase 8.

5. **SDK property semantics** (MZ0380.txt profile + SC5C0 sample):
   - Inputs: HDMI / DVI-D / COMPONENTS / DVI-A / SDI
   - Record modes: VBR / CBR / HBR
   - Bitrate menu: 256 KB .. 12,288 KB (matches our 256K..12M range)
   - Quality menu: 0 .. 10,000 (UI scale; driver uses raw 0..100)
   - B-frames: 0 / 1 / 2
   - NTSC framerates: 60 / 30 / 15 / 7.5 / 3.75
   - PAL framerates: 50 / 25 / 12.5 / 6.25 / 3.125

   All seven property offsets in BAR5 0x0040..0x0088 stay correct.

## File map

| File                        | Phase | Notes                                  |
|---                          |---    |---                                     |
| `mz0380-reg.h`              | P0    | All register offsets, CHECKME marked   |
| `mz0380.h`                  | P0    | Extended dev struct, fw/dma/audio bits |
| `mz0380-cards.c`            | P0    | All known subsys IDs + Rev.3           |
| `mz0380-core.c`             | P1-2  | Probe/teardown, command channel        |
| `mz0380-fw.c`               | P1    | request_firmware + chunked upload      |
| `mz0380-dma.c`              | P2    | Ring alloc + MSI ISR + drain workq     |
| `mz0380-video.c`            | P3-4  | vb2 queue + DV_TIMINGS                 |
| `mz0380-audio.c`            | P5    | ALSA snd_card + PCM substream          |
| `Makefile`                  | P6    | Default builds only mz0380             |
| `README.md`                 | P6    | Install/load/capture instructions      |

## Historical bring-up notes

Earlier session diaries (one entry per property correlation pass)
were consolidated above. The procfs/correlation tooling from those
sessions still exists and remains the right way to refine CHECKME
offsets:

- `mz0380-correlation.sh` — script that loads probe-safe, snapshots
  before/after, diffs, captures `v4l2-ctl --all`. Use with
  `--input-reg`, `--bitrate-reg`, etc. to confirm new candidates.
- `/proc/mz0380-experiment` — runtime BAR5 read/write surface for
  bounded experiments without rebuilding.
