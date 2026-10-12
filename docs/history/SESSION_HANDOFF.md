# MZ0380 Linux driver — session handoff (2026-07-05, evening)

## Project
Clean-room Linux V4L2 driver for the **Elgato Game Capture HD60 Pro** (PCIe,
`12ab:0380`, subsys `1cfa:0006`). Own hardware, RE for interoperability. Repo
`/home/wolffyx/Projects/sc0710`. Active driver = `mz0380-*.c`; the `sc0710-*.c`
files are untracked upstream reference (sibling 4k60 mk.2) — read, don't build.
Card = Yuan MZ0380/SC3C0 board, SoC **Vatics Mozart 395s** (NOT MStar SL6010 —
teardown-corroborated), HDMI receiver **MST3367CMK-LF-170**, HDMI front-end ITE
IT6621FN, firmware 1.11.1.11.

## The one-paragraph story — SOLVED (M15)
Card boots, mailbox works. HDMI was dead because the MST3367 was **held in reset
by GPIO pin9** (active-low), which the Linux side never released. Releasing it
(op 0x15: pin3=1 RX enable, pin9 pulse 1->0->1) makes the I2C bus live, and the
hdcapm init then locks the receiver to the source. **Confirmed on hardware
2026-07-05:** after --gpio-reset the MST3367 locked to a real 1080i signal
(reg 0x55=0x7b, Htotal 2200, Hactive 1920, interlaced) read straight over the
mailbox I2C proxy. First working HDMI signal detection on Linux for this card.
All downstream pieces (init values, EDID, detect math, timing table) are in hand
from `docs/re-2026-07-05/`. Remaining work is DRIVER INTEGRATION, not RE.

## What is SETTLED (don't re-derive; RE_FINDINGS M11+M12)
- I2C proxy ABI verified both sides: op 0x1a rd / 0x1b wr, dev **8-bit 0x9C**
  @BAR0+0x08 (card `>>1`), reg @0x0c, val/result @0x10. Banked: wr reg0=bank.
- Result semantics: sentinel-survives = handler didn't run; **0x00 = NAK**
  (fw forces it); nonzero = ACK. M11's NAKs were genuine, not framing bugs.
- No MCU ("mcu version = 0") — op0x20-passthrough theory dead. Pin-map-wrong
  theory dead (same fw works under Windows).
- hdcapm GPL driver = full MST3367 logic (init_setup, HPD chip-side B0/0xB7
  bit1, HDMI reset B2/0x07 f4→04, HDCP reset B0/0xb8 10→00, detect B0/0x55 &
  0x3c + timing regs + standards table). Local copy:
  `docs/re-2026-07-05/mst3367-reference-from-gpl-driver.md`.
- 256-byte EDID: `docs/re-2026-07-05/elgato-hd60pro-EDID.bin`.
- Bring-up order: reset → EDID → HPD → detect. Source sends nothing until
  EDID+HPD; 0x55 no-signal ≠ NAK.
- Disasm conflict (minor): 0x1e/0x1f bus1 (our RE) vs bus0 (Windows-side RE).
  MST3367 is bus0 via 0x1a/0x1b either way.

## DONE (M16): live signal detect in the driver
`mz0380-mst3367.c` implements `mz0380_mst3367_bringup()` (pin3=1, pin9 1->0->1,
pin8, hdcapm init) + `mz0380_mst3367_read_signal()` (poll 0x55, decode timing,
map to a CEA preset). Wired into `mz0380_query_signal`; primed at card_setup.
VERIFIED: `v4l2-ctl --query-dv-timings` returns 1920x1080 total 2200x1125,
74.25 MHz, CTA-861 VIC 34 (1080p30) from a live source. `mz0380_periph_read`
now uses a low-byte sentinel poll (RE_FINDINGS M16 read-race gotcha).

## STREAMING (M17-M19): infra done, frames NOT yet flowing
`mz0380-dma.c` rewritten to the real protocol (M17): 4x512KiB host buffers,
physaddrs to card via op 0x02, arm via SET_VIC (0x29), stop 0x2a. ISR reads the
real cause (EVENT BAR0+0x30), token BAR0+0x40 (idx&7), acks via
mz0380_mb_ack_event. Command completion now always polls STATUS (M18 hang fix -
enabling MSI had made every command time out). Builds + loads clean.

TESTED (M18/M19): insmod instant, SET_VIC completes, but NO frames - IRQ count
stays 3, zero frame tokens, capture blocks. video_capture_mgr RE (M19): SET_VIC
IS the start (it system()-spawns tinyvenc); no separate kick. So the gap is one
of: SET_VIC struct fields mis-packed (bail branches -> tinyvenc mis/never
launched), the hready host gate not asserted, or buffer physaddr not landing at
BAR0+0x08. Frame LENGTH lives in tinyvenc (not ep.ko).

SET_VIC FIXED (M20): the 44-byte struct is now packed correctly (format byte6,
width/height, input dims, bitstream_num=1). Confirmed on hw: "SET_VIC(1920x1080 p
H.264, bitstreams=1) ret=0". BUT still NO host DMA - MSI flat at 3, 0-byte file.

BLOCKER RESOLVED (M22): the "enable_dma input-type 4/9 gate" (M20) was a MISREAD
- enable_dma (0x7ed88) is write-only in tinyvenc5 and never gates frames; the
==4/9 compare just sets total_channel_num (already fed from argv). The real
blocker: SET_VIC (op 0x29) only SPAWNS tinyvenc5 (via video_capture_mgr) - it
does not start frames. tinyvenc5 then blocks on /sys/vpl_pciep/epint waiting for
a SEPARATE START_STREAMING = mailbox op 0x06 before it DMAs anything. Our driver
never sent op 0x06 (its START_STREAMING #define was a bogus 0x12, unused). Chain
proven both-sides: tinyvenc5 cmd jump-table (cmd 6 -> start @0xe954) +
video_capture_mgr (spawns on 41, ignores 6) + ep.ko op6 ISR @0x1854 (sysfs_notify
epint when no_signal==0). Path A (Windows VM) abandoned - VM freezes the host.

FIX IMPLEMENTED (this session, builds clean, NOT yet hw-tested):
- mz0380-reg.h: MZ0380_CMD_START_STREAMING 0x12 -> 0x06.
- mz0380-dma.c dma_start(): after SET_VIC ok, msleep(500) then send op 0x06
  (no params). Delay lets tinyvenc5 finish forking/exec + eat its first-read
  SET_VIC before op6 lands.

NEXT = HARDWARE TEST (Path A/B both retired). Load driver, start capture on a
live locked 1080p source. EXPECT: two acks in dmesg ("SET_VIC ... ret=0" then
"START_STREAMING(op 0x06) ret=0"), then IRQ/token count RISES and the file is
non-empty. If op6 acks but tokens stay flat: (a) bump the msleep (tinyvenc5 not
up yet - the spawn/exec is slow on the SoC), or (b) verify no_signal==0 at op6
time (needs the source truly locked and real WxH in SET_VIC, both already true
on the M16 path). Full detail: RE_FINDINGS M22.

Full detail: RE_FINDINGS M17-M22. The drain logs token/idx/candidate length
regs/buffer head bytes, so once frames flow the length (enc_stat+0x08) falls out.

## NEXT (after frames flow)
1. Set the correct frame length from the resolved register; fix vb2 format
   (H.264 pixelformat, sizeimage) so the .h264 is playable.
2. Audio DMA path (currently no-op; enable_audio milestone-C follow-up).
3. EDID push (op 0x1f to 0xA0, `docs/re-2026-07-05/elgato-hd60pro-EDID.bin`) +
   HPD (op 0x15 pin1) for sources that gate output on EDID.
4. Refactor: i2c_adapter tunneling the mailbox so hdcapm mst3367-drv.c runs as a
   V4L2 subdev; source-change IRQ so QUERY reflects hot-plug without reload.
Protocol: RE_FINDINGS M17; pins M14; detect M15/M16.

## Gotchas (carried forward)
- Always sentinel the result slot; stale slots lie (the "127 ACKs on bus1"
  artifact). 0x00 result = NAK by design.
- pipefail + `grep -q` breaks the reload script; enable_video → pipewire holds
  video0 → rmmod EBUSY.
- Load: `modprobe videodev videobuf2-v4l2 videobuf2-dma-sg v4l2-dv-timings
  snd-pcm` then `insmod ./mz0380.ko firmware_upload=1 dma_handshake=1
  procfs_verbosity=2`.
- Tools: `/proc/mz0380-cmd` (raw opcode), `/proc/mz0380-hdmi` (SET_VIC),
  `/proc/mz0380-scan` (raw BAR0 window via scan_start/scan_len params).

## Repo state
Uncommitted: RE_FINDINGS.md M11+M12, `mz0380-mst3367-test.sh`,
`docs/re-2026-07-05/` (4 vendored docs), this handoff. Commit as a
checkpoint when convenient. Windows trace kit remains at
`/run/media/wolffyx/Work/hd60-trace/` (firmware tars + disasm scripts there,
not vendored).
