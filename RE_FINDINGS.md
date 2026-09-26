# MZ0380 register map — reverse-engineered from `e60MZ0380.X64.SYS`

Ground truth extracted via Ghidra headless decompilation of the Windows driver
(`e60MZ0380.X64.SYS`, 3.7 MB, from the Elgato HD60 Pro Windows install). These
replace the earlier `CHECKME` guesses in `mz0380-reg.h`, which were structurally
wrong (they assumed a BAR5 mailbox + chunked firmware protocol).

## BAR identity (CONFIRMED)

The device context holds two mapped MMIO pointers:
- `ctx + 0x108` → **BAR0** (1 MiB) — command mailbox + firmware download buffer.
- `ctx + 0x110` → **BAR5** (4 KiB) — DMA-address pointer window + SDK property cache.

Proof: init routine `FUN_140278bb0` writes `*(ctx+0x110) + 0x30 = BAR0phys + 4`
and `*(ctx+0x110) + 0x38 = BAR0phys + 0x5f`. On this host `lspci` shows BAR0 at
`fc200000`, and PLAN.md's live BAR5 snapshot observed `0x30 = fc200004` and
`0x38 = fc20005f` — an exact match. So the window written at ctx+0x110 IS BAR5,
and its 0x30/0x38 registers are programmed with BAR0-relative physical addresses.

## Command mailbox — BAR0 (from `MZ0380_SEND_COMMAND` = `FUN_140285074`)

Layout, relative to BAR0 base:
| offset | role |
|--------|------|
| `0x00` | doorbell / command trigger. Write `0x800` to fire a command; `0x400` seen in reset/init |
| `0x04` | PARAM0 = opcode (`param_2[1]`) |
| `0x08` | PARAM1 / result readback (`== 0` means success after firmware commit) |
| `0x0c` | PARAM2 |
| `0x10…` | further params (PARAM3…) |
| `0x2c` | STATUS. **bit0 = command done/ready**. Cleared to 0 before issuing |
| `0x30` | written 0 during init (misc control) |
| `0x60` | firmware download buffer aperture (blob written here word-by-word, ascending) |

`SEND_COMMAND(ctx, params[], count, [wait])`:
1. If `count >= 2`: for i in 1..count-1, write `params[i]` to `BAR0 + i*4`
   (so opcode `params[1]` → `BAR0+0x04`, next → `+0x08`, …).
2. Write `0x800` to `BAR0 + 0x00` (doorbell).
3. If `wait` arg set: block on a semaphore (async completion via ISR).
   Else poll `BAR0 + 0x2c` up to 50× (`0x32`), success when `& 1`.

## Firmware download (from `MZ0380_DownloadFirmware` = `FUN_1402762d4`)

Main (HD) firmware:
1. `SEND_COMMAND(op=0x0b, size)` — BEGIN_FIRMWARE_DOWNLOAD, param = blob byte count.
2. Copy the **entire** blob into `BAR0 + 0x60`, ascending 32-bit writes
   (`(size+3)>>2` words). Not chunked, no per-chunk ack.
3. `SEND_COMMAND(op=0x0c)` — commit / execute (card reboots into new firmware).
4. Delay, then success iff `*(BAR0 + 0x08) == 0`.

Base firmware (`MZ0380_DownloadBaseFirmware` = `FUN_140275f64`) is identical with
opcodes `0x0e` (begin) / `0x0f` (commit) instead of `0x0b` / `0x0c`.

## Confirmed opcodes
| op | meaning |
|----|---------|
| `0x0b` | BEGIN_FIRMWARE_DOWNLOAD (param = size) |
| `0x0c` | COMMIT/EXECUTE firmware |
| `0x0e` | BEGIN_BASE_FIRMWARE_DOWNLOAD (param = size) |
| `0x0f` | COMMIT/EXECUTE base firmware |

## BAR5 (ctx+0x110) — confirmed uses
- `0x30` / `0x38`: BAR0-relative physical pointers programmed at init (0x30 = base+4,
  0x38 = base+0x5f) — the card's notify/mailbox address handshake.
- `0x40`…`0x88`: SDK property cache (7 correlation-confirmed props — unchanged).
- `0xdc`: written `2` during init (purpose TBD).

## Still to resolve (next RE pass)
- IRQ status/mask/ack registers + bit assignments (decompile `Interrupt_Handler`
  = `FUN_14024ba60`).
- DMA ring / XDMA descriptor programming (the `0x30/0x38` BAR5 pointers plus BAR0
  channel regs — decompile the streaming setup around the `[FIRMWARE RESET]` path
  `FUN_1402829a0`).
- Raw-vs-encoded output format selection (SET_VIC / SET_AIC opcodes).

Source logs: `scratchpad/re/ghidra.log` (string xrefs), `scratchpad/re/decomp.log`
(decompiled functions).

## Interrupt/event protocol (RE pass 2 — FUN_140284380, the Windows "event thread")

Windows does NOT service the card from a real ISR for the mailbox: a priority-31
kernel thread loops every ~1 ms doing:

```c
event = BAR0[0x30];              // interrupt/event status word
BAR5[0xdc] = 2;                  // ack/rearm the card-side flag (card sets it to 1)
BAR0[0x30] = 0;                  // clear the event word
BAR0[0x00] = 0x400;              // doorbell ack (0x400 = event ack, not "reset")
if (event & BIT(11))             // bit11 = COMMAND COMPLETE
    KeReleaseSemaphore(cmd_sem); // wakes MZ0380_SEND_COMMAND's IRQ-wait path
// event[7:0]  -> video/stream DPCs, payload in BAR0[0x40/0x44/0x48]
// event[23:16]-> other DPC class,  payload in BAR0[0x4c]
```

Confirmed against hardware: after our un-acked BEGIN_FW_DL, BAR5[0xdc] read 1 and
PCI INTx stayed pending until this exact 3-write ack sequence.

## MZ0380_SEND_COMMAND (FUN_140285074) — fully decoded

- cmd struct words[1..n] are written to BAR0+0x04,0x08,0x0c,0x10 (word1 = opcode).
- doorbell = write 0x800 to BAR0+0x00.
- Poll path: clear BAR0+0x2c, poll bit0, 50 x 1 ms (matches our driver).
- IRQ path (long commands, incl. firmware download): wait on semaphore released by
  the event thread on EVENT bit11 — STATUS bit0 alone is NOT sufficient for these.
- After >=100 consecutive timeouts the driver stops clearing STATUS (failure latch).

## Peripheral register file = mailbox opcodes 0x1a/0x1b

- `FUN_1402777e4(dev, 0, chip, reg)`  = mailbox cmd {0x800, 0x1a, chip, reg, 0},
  result read back from BAR0+0x10. (register READ)
- `FUN_1402851cc(dev, 0, chip, reg, val)` = mailbox cmd {0x800, 0x1b, chip, reg, val}.
  (register WRITE)
- chip ids seen: 0x90 (bridge/FPGA register file — the ISR-status regs 0x10..0x19,
  0x8b live here), 0xb8 (TVP5160 analog front-end), 0x50 (encoder?).
- So the "Interrupt_Handler" (FUN_14024ba60, actually a DPC) reads its per-source
  status registers THROUGH the mailbox, indices 0x10/0x13/0x14/0x15/0x8b of chip
  0x90; final re-reads of 0x13/0x14/0x15/0x10 act as read-to-clear.

## Firmware upload — VERIFIED ON HARDWARE (M2 PASS)

Working sequence (mz0380-fw.c):
1. BEGIN_FW_DL (0x0b, param = byte count) — completes via EVENT bit11 (~1 ms).
2. Whole blob word-wise to BAR0+0x60.
3. COMMIT_FW (0x0c) fire-and-forget — this opcode never posts a completion.
4. Boot wait ~21 s with 1 ms unconditional ack heartbeat (mirror the Windows
   event thread); boot-done arrives as EVENT=0x800 and RESULT (BAR0+0x08)
   transitions size-echo -> 0.
5. Post-boot: STATUS (BAR0+0x2c) reads 0xdddddddd, GET_FW_VERSION (0x01)
   answers (fw 0.1), mailbox remains functional.

Every event MUST be acked (BAR5[0xdc]=2, BAR0[0x30]=0, doorbell 0x400) or the
card wedges with INTx asserted and later commands time out.

## DMA buffer allocation (RE pass 3 — FUN_14028d254, generic YUAN SDK init)

One SDK function sizes and allocates all DMA memory. MZ0380 matches via
`(devid - 0x370) & 0xffee == 0` (0x370/0x371/0x380/0x381) at LAB_14028dcfb:

- ctx[0x18] = 0        -> allocation mode 0: MmAllocateContiguousMemorySpecifyCache,
                          retry loop walking 128 MB windows upward below 4 GB
- ctx[0x1c] = common buffer size. HD60 Pro (subsys 1cfa:0006 takes the
  "else" branch) = 0x400000 (4 MB); other MZ038x flavors 0x200000.
- stream buffer sizes for our flavor: 0x655000 (or 0x466000/0x34bd00 variant),
  DbgPrint tag "[MEMORY] [%08X] [%08X] [%08X]".
- allocated VA stored at ctx[0x28], physical address (MmGetPhysicalAddress)
  at ctx[0x27]. WHO writes ctx[0x27] into a BAR register = still open —
  next RE target (consumers of ctx offset 0x138/0x140 region).

Note: BAR5[0x30]=bar0+0x4 and BAR5[0x38]=bar0+0x5f exist pre-boot already,
so those two are card-programmed defaults, not host DMA pointers.

## THE ORACLE: firmware blob is a full Linux SDK; card-side ep.ko serves the mailbox

MZ0380.HD.HEX = gzip -> GNU tar "yuan_demo_sdi/" = complete ARM Linux userland
for the card SoC (project SC5C0). rc.local loads drivers then runs
video_capture_mgr / audio_capture_mgr / yuan_ioctrl. The host-facing PCIe
mailbox is implemented by drivers/ep.ko (ARM ELF, not stripped) - this is the
authoritative post-boot command spec, no more guessing from the Windows side.

Extracted: scratchpad/fw/yuan_demo_sdi/. Key funcs in ep.ko:
  pciep_isr @ 0x11234       - host command dispatcher (the real ISR)
  pciep_isr_clrint @ 0x10d68
  command_store/show        - sysfs command attr

### ep.ko command model (RE-confirmed)
- Command buffer base = inbound_mem0_start + 4 (== host BAR0 phys + 4), recomputed
  every doorbell from *(ep_regs + 0x10). Matches BAR5[0x30]=bar0+4, BAR5[0x38]=bar0+0x5f.
- Layout at that base: word0 = opcode, word1.. = params, word10 (= bar0+0x2c,
  our STATUS) = completion slot.
- Completion is per-opcode, TWO mechanisms:
  * opcode 0x01 (INIT), 0x0a (GET_VERSION), 0x02/0x03/0x04 (SET banks): write
    STATUS word10. GET_VERSION writes version to word1/word2 (bar0+8/0x0c) and
    STATUS = 0xaaaaaaaa (DAT_00011978) when ready, else DAT_0001197c.
  * opcode 0x0b/0x0e (FW download), 0x2a/0x29 (signal): sysfs_notify -> raises the
    host EVENT 0x800 instead of writing STATUS. (Explains why fw upload completes
    via EVENT, not STATUS.)
- Unknown opcode -> prints "$$$command: 0x%08x" + 10 params and returns WITHOUT
  writing STATUS (host sees a timeout).

### Why the live command path (INIT/version/signal) is still deaf post-boot
Pre-boot the bootloader answers 0x0b with a fixed inbound mapping. Post-boot ep.ko
brings up the full inbound/DMA window; until the host establishes that window
(bus-master DMA, currently disabled by enable_dma=0), the doorbell IRQ fetches the
command from the wrong card-RAM address and STATUS stays at ep.ko's 0xdddddddd
poison value. => the post-boot control path is gated on DMA/bus-master (M4), not on
opcode correctness. mz0380_card_init() is Windows-accurate and should light up once
the inbound window is programmed.

## M4 CONTROL PATH WORKING (verified on hardware)

Post-boot mailbox is fully operational. Sequence (mz0380_card_init):
1. BAR5[0x30]=bar0_phys+4, BAR5[0x38]=bar0_phys+0x5f (already latched at boot).
2. CMD_INIT (0x01): opcode -> BAR0+0x04, doorbell BAR0+0x00 = 0x800.
   Completion = STATUS(BAR0+0x2c) reads 0xaaaaaaaa (NOT bit0; the fw success
   stamp). Answered on attempt 1.
3. GET_BOARD_VERSION (0x0a): running fw version in PARAM1/PARAM2 (BAR0+0x08/0x0c).
   Reports 1.11, matching MZ0380.FW.TXT "01.11".
4. Version-skip: if the card already runs the shipped version, the ~21 s
   upload+boot is skipped entirely.
5. Peripheral read (mz0380_periph_read, opcode 0x1a) of bridge chip 0x90 reg
   0x12 returns live HDMI signal presence (bit0). Confirmed responding.

THE decisive bug (found via mz0380_mailbox_scan): the poll loop was firing the
0x400 ack doorbell every 1 ms unconditionally, which ABORTS a STATUS-completing
command (0x01/0x0a) before the card finishes. Fix: poll silently, ack only when
EVENT(0x30) != 0. Offsets (op@0x04, bell@0x00) were correct throughout.

Bus mastering (dma_handshake=1) was enabled for the successful runs; whether the
quiet poll alone suffices without it is untested.

Remaining for full capture (M4 second half): program the frame DMA ring (real
ring-register offsets still a guess in mz0380-dma.c) and drive SET_VIC /
START_STREAMING over the now-working command channel.

## M4b SIGNAL-DETECT: BAR0 dead end, pivot to bridge-chip command path (2026-07-04)

Hardware plug/unplug diff via the new `/proc/mz0380-scan` (BAR0 window,
per-read `pr_info` trace):

- BAR0/MMIO is only backed at **0x04..0x5c** (the command-mailbox aperture).
  0x00 reads 0xffffffff. Every offset >= 0x60 eats a ~200 ms PCIe completion
  timeout then returns 0xffffffff -- NOT a hard hang, just ~0.2 s/read, so a
  blind wide BAR0 sweep looks frozen (that was the earlier "freeze").
- The sc0710-ported HDMI window (0xa8..0xe4 in `mz0380_signal_from_bar0()`)
  sits in that dead zone => reads never change on plug/unplug.
  **The sc0710 register model does not apply here; HDMI signal is NOT a BAR0
  register.** Those guesses should be dropped from `mz0380_signal_from_bar0()`.

Correct surface = the HDMI **bridge chip (0x90)** via the proven mailbox
`REG_READ (0x1a)` path (`mz0380_periph_read`). New `/proc/mz0380-periph-scan`
walks a chip's register file diff-stably. The old `MZ0380_BRIDGE_SIGNAL 0x12
bit0` label is unverified -- the hunt (`PERIPH=1 scripts/mz0380-signal-hunt.sh`)
finds which bridge register really tracks lock. Needs `dma_handshake=1`.

Tooling: `/proc/mz0380-scan` (raw BAR, safe-range gated), `/proc/mz0380-periph-scan`
(bridge/TVP5160 via 0x1a), `mz0380-signal-hunt.sh` (capture + diff analyzer,
LOCK-BIT vs ACTIVITY).

## M4c EP.KO COMMAND ORACLE — signal is NOT a register, and 0x1a is bogus (2026-07-04)

Re-extracted the card firmware (`tar xzf /lib/firmware/mz0380/MZ0380.HD.HEX`,
gzip+tar rooted at `yuan_demo_sdi/`) and disassembled the card's PCIe-endpoint
mailbox server `drivers/ep.ko` (ARM, not stripped) with llvm-objdump. The
command dispatcher is `pciep_isr`: it reads the opcode with `ldr r6,[r4]`
(r4 = inbound command buffer; [r4+0]=host BAR0+0x04 opcode, [r4+4]=host 0x08
RESULT, [r4+8]=host 0x0c, ...) and branches on a fixed opcode set.

Opcodes actually dispatched by fw 1.11:
  1,2,4,6,7,8,9,10,11,14,15,20,21,23,34,41,42,45,47,49,80,82,96,97,98,100,110,123,124
Identified so far:
  - 10 (0x0a) GET_BOARD_VERSION  -> writes version to [r4+4]/[r4+8] (host PARAM1/2). MATCHES.
  -  1 INIT, 11 fw-download BEGIN, 14 fw related (debug-printed in tail).
  -  2 / 8  copy a block of host words into card config state (SET_* setters).
  - 41 (0x29) SET_VIC_PARAMS: reads width/height FROM the host command, prints
       "SET_VIC_PARAMS(fw %d) size(%dx%d) fps(%d)"; if width==0||height==0 it
       sets the card `no_signal` global (data sym `no_signal` @0x71c) and prints
       "cmd(%d) => no signal". So the HOST supplies resolution; the card derives
       no-signal from a zero size.

KEY CORRECTIONS (previous guesses were wrong):
  - 0x1a REG_READ / 0x1b REG_WRITE are NOT card commands. Opcode 26 (0x1a)
    dispatches to a sysfs_notify stub, not a peripheral read. The earlier
    "bridge chip 0x90 reg read confirmed" was the mailbox returning a constant
    0x48 (the RESULT slot), never real register data. `mz0380_periph_read()` and
    the whole bridge-register model are built on a bogus opcode.
  - Guessed opcodes SET_VIC_PARAMS=0x10, QUERY_SIGNAL=0x20, START_STREAMING=0x12
    are absent from the dispatcher; SET_VIC is really 0x29 (41).
  - There is NO mailbox getter that returns input width/height/signal. HDMI
    signal presence is therefore NOT pollable via a command in this firmware.

SIGNAL MODEL (reconciles the QCAP SDK): QCAP exposes signal via CALLBACKS
(QCAP_REGISTER_NO_SIGNAL_DETECTED_CALLBACK / SIGNAL_REMOVED) and the result code
NO_SIGNAL_DETECTED=0x9, not a poll. The card detects format itself (video_capture_mgr
+ libvideocap + the front-end receiver) into `g_stream_info` (44B @0x640) and
raises a host EVENT. reg.h already reserves EVT_PAYLOAD0..3 at BAR0+0x40..0x4c
("DPC args"). => the real signal/format path is the card->host EVENT with the
format carried in the 0x40..0x4c payload words, decoded on the interrupt.

NEXT (correct milestone B): capture EVENT(0x30)+payloads(0x40..0x4c) LIVE across
a plug/unplug (events are edge-triggered, so sample continuously without reload
and without acking), and RE which pciep_isr path / card event writes those
payloads. The two-static-snapshot register diff cannot see an edge event.

================================================================================
M5  CROSS-REF: sibling 4k60 Pro mk.2 (sc0710) + USB gchd give the EXACT signal
    blob layout. (Reviewed ~/Downloads/test/elgato-gchd-master + untracked
    sc0710-i2c.c / sc0710-video.c in this repo.) 2026-07-04.
================================================================================

The status blob our ARM MCU serves is now known byte-for-byte, from the sibling
card that reads the SAME ARM MCU over a different transport.

sc0710 (4k60 Pro mk.2) reads HDMI status via an AXI-IIC master at BAR0 0x3100:
  - ARM MCU is an I2C slave: I2C_DEV__ARM_MCU = (0x32<<1). Host does write-read.
  - AXI-IIC regs: 0x3100 ctrl (0x2=TX reset,0x1=enable), 0x3104 status
    (0x44=addr ack, 0xc4=subaddr ack, 0xc8=read done), 0x3108 TX_FIFO
    (bit8=start, bit9=stop), 0x310C RX_FIFO.
  - THREE status pages by subaddress: 0x00 -> 26 bytes (HDMI status),
    0x1a -> 16 bytes (status2), 0x2a -> 16 bytes (status3).

HDMI status blob (subaddr 0x00, 26 bytes) decode (sc0710_i2c_read_hdmi_status):
    rbuf[0x08]            != 0  => LOCKED (signal present)     <-- the flag
    rbuf[0x0a..0x0b] LE16      => width            (e.g. 1920)
    rbuf[0x08..0x09] LE16      => height/field     (e.g. 540; x2 if interlaced)
    rbuf[0x04..0x05] LE16      => pixelLineV/V-total (e.g. 562)
    rbuf[0x06..0x07] LE16      => pixelLineH/H-total (e.g. 2200)
    rbuf[0x0d] & 0x01          => interlaced
    (rbuf[0x0d] & 0x30) >> 4   => colorimetry 1=BT709 2=BT601 3=BT2020
    rbuf[0x0f]                 => colorspace 0=YUV422/420 1=YUV444 2=RGB444
  Captured example (1080i59.94): 1920 / Htot 2200 / Vtot 562 / interlaced.
  (H,V) -> format via sc0710_format_find_by_timing() table (sc0710-video.c:185+):
    {1650, 750,1280, 720,0,...720p}, {2200,562,1920,540,1,...1080i}, etc.
  NO-signal branch just zeroes everything and sets locked=0. There is no
  distinct "no signal" register - locked byte == 0 IS the no-signal state.
  (Matches gchd's USB path: it derives the same by SAMPLING a line-period
  counter and pattern-matching magic sums, and treats a sentinel read
  0xad4d as "no signal". Colorspace autodetect unreliable there too.)

WHY THIS NAILS MZ0380 (HD60 Pro): same ARM MCU, different transport. sc0710's
AXI-IIC block (BAR0 0x3100..0x3120) lands in OUR dead zone (BAR0 backed only
0x04..0x5c) -> HD60 Pro has no AXI-IIC path; the host<->ARM-MCU channel here IS
the BAR0 mailbox. So the firmware serves this same locked/width/height/H/V blob
(our g_stream_info 44B@0x640) over the mailbox/EVENT instead of I2C. The sibling
polls it (sc0710-core.c:230,450) - so on MZ0380 a poll-able form may also exist.

REVISED NEXT (supersedes the blind diff above):
  1. Run the live event watcher across a cable toggle and decode payload words
     0x40..0x4c with the template above (hunt for the locked byte + LE16
     1920/1080/2200/1125 - not "which word flips").
  2. New probe: read the firmware buffer at BAR0+0x60 for a 26/44-byte blob =
     the mailbox mirror of sc0710's "I2C read subaddr 0" (g_stream_info@0x640).
  3. Wire mz0380_signal_from_bar0() to those exact offsets + reuse the
     sc0710-video.c timing table for fps/scan-mode. Delete remaining guesses.

M5 EMPIRICAL RESULT (2026-07-04, fw 1.11): the idle card reports NO signal to
the host, two independent ways:
  a. Live event watcher across a real HDMI plug/unplug: 0 edges. EVENT(0x30)
     stayed 0, STATUS(0x2c) steady 0x00000001. The card raised no host edge.
  b. Static two-state diff of the backed mailbox region BAR0 0x00..0x5c
     (6 unplugged + 6 plugged samples): no word changed with plug state.
  => In idle (front-end not armed, no input selected, not streaming), there is
     neither a pollable signal word NOR a pushed event in the host-visible BAR0
     region. This reconciles everything: fw 1.11 follows the host-forces-format
     model (SET_VIC_PARAMS 0x29 takes WxH FROM the host); the card does not
     autodetect-and-report to the host while idle. Matches gchd/elgato ("no
     continuous autodetect") and the QCAP SET_VIC path.
  => Signal/format reporting, IF it exists at all, is gated behind arming the
     pipeline (input-select + start-streaming = milestone C). The sibling
     sc0710 can read a status blob because its ARM MCU front-end is always
     powered over AXI-IIC; on HD60 Pro that path is absent (dead zone) and the
     front-end appears powered down until capture starts.
  NOTE: mz0380-reg.h still carries STALE sc0710-derived guesses disproven by RE
     - CMD opcodes 0x10/0x11/0x12/0x13/0x20 (real SET_VIC=0x29; 0x12/0x20 absent
     from dispatcher), AXI_IIC block @0x3100 (in BAR0 dead zone, no HW), HDMI
     regs 0xa8..0xe4 (un-backed). Clean these when wiring the real path.
  OPEN (dispatched to firmware disasm): does a get-stream-info/get-status opcode
     exist among the real set {6,7,9,11,14,15,20,21,23,34,42,45,47,49,80,82,96,
     97,98,100,110,123,124}, and which are the real START_STREAMING + INPUT_SELECT?

================================================================================
M6  DISASM CLOSURE (2026-07-04): ep.ko command set has NO card->host format
    path. Milestone B is unsupported by fw 1.11 firmware. Evidence-locked.
================================================================================

Disassembled ep.ko (yuan_demo_sdi variant, ARM EABI5, not stripped; dispatcher
pciep_isr @.text 0x1210). Full opcode->handler map recovered. Conclusions:

Q1 GET-STATUS OPCODE: NONE (HIGH). The ISR (0x1210..0x1944) never references
   g_stream_info (.bss+0x640). No handler copies video-format/resolution/fps/
   signal into the host reply words. The richest reply is GET_VERSION (op10):
   2 words (fw_version[0],[1]) at reply [4],[8]. GPIO ops 20/21/23 return a bit
   map at [2]/[8]. That is the entire card->host data surface.

Q3 WHO FILLS g_stream_info: ONLY the card's local userspace, via the on-card
   char-device ioctl livectrl_ioctl (LIVECTRL_IOCTL_GET_INFO 0x40047005 /
   SET_INFO 0xC0047005, 44B, copy_to/from_user of .bss+0x640). ep.ko has NO
   HDMI-detect routine; it never measures format. The front-end (Gennum GV7601
   SDI/HDMI receiver via spi_gv7601, + vpl_vic / gv7601_audio) lives in sibling
   .ko modules and is NOT reachable over the PCIe mailbox.

Q4 EVENT EMIT: sysfs_notify + the msi finalizer (0x115c) which sets EVENT bit11
   (orr #2048) at DBI+0x30 and rings a doorbell = the sole host-notify path.
   It carries ONLY the event bit; NO format payload. Several opcodes
   (6,9,15,42,45,47,49,80,96,97,98,110) gate their notify on no_signal
   (.bss+0x71c) but write no data. BAR0 0x40..0x4c is populated only by
   config-bank echoes (op2/4/6/8) and op10/21/23 replies - never format.

=> DEFINITIVE MODEL: format is strictly HOST->CARD via SET_VIC_PARAMS (op41,
   0x29). Correct field layout of the command buffer (cmd[] = mailbox param
   slots, byte offsets from opcode buffer base):
     cmd[6]     = VIC / input code (special value 7)
     cmd[5]     = fps
     cmd[8..9]  = Width  (u16)
     cmd[10..11]= Height (u16)
     cmd[0x22]  = int_reduce flag -> g_interrupt_reduce_enable (.bss+0x630)
   Width==0 || Height==0  => sets no_signal=1 (.bss+0x71c), prints
   "cmd(%d) => no signal". Else no_signal=0, stores input code to .data+0x04
   (windows_select_fw), sysfs_notify. There is NO reverse (card->host) format
   command. Any host "query signal" design must be abandoned. The host is the
   SOURCE of format (assert via op41), and gets only a no_signal-change nudge
   (EVENT bit11) back - exactly the gchd/elgato "no autodetect" model.

REAL OPCODE MAP (replaces the stale CHECKME guesses in mz0380-reg.h):
   1 INIT | 2 set-cfg-bankA | 4 set-cfg-bankB | 6 enc-status-bank+notify |
   8 set-enc-params(via host ptr) | 10 GET_VERSION | 11 BEGIN_FW_DL |
   14 BEGIN_BASE_FW_DL | 20 GPIO-read | 21 GPIO-set | 23 GPIO-dir |
   34 STOP_STREAMING-area notify | 41 SET_VIC_PARAMS | 45 SET_AIC_INT_MODE |
   100 PREVIEW_BUF_EX | 123/124 fw-dl variants | 7/9/15/42/47/49/80/82/96/97/98/
   110 = signal-gated sysfs notifies (no data). START_STREAMING is NOT an ep.ko
   opcode - the encoder/XDMA lives in the vpl_*/vma_* modules; "start capture"
   from the host = program cfg banks (op2/4/8) + assert VIC (op41) + arm XDMA
   host-side. Stale/bogus: 0x10/0x11/0x12/0x13/0x20 (not in dispatch), AXI_IIC
   @0x3100 (BAR0 dead zone), HDMI regs 0xa8..0xe4 (un-backed).

MILESTONE B: CLOSED - not achievable with this firmware. Proceed to the
host-forces-format design (V4L2 dv_timings SET by app/EDID, asserted via op41;
no S_DV_TIMINGS query-from-card, no VIDIOC_QUERY_DV_TIMINGS backed by hardware).

================================================================================
M7  HDMI ACTIVATION (2026-07-04): the source stays asleep because the card
    never presents itself as a sink (HPD/EDID). Fix = run the SDK bring-up.
================================================================================

USER SYMPTOM: HDMI source (camera) plugged into the card's IN port does NOT
enter HDMI mode / outputs nothing (OUT passthrough dark). Same camera on a real
monitor DOES wake. => classic HDMI sink-handshake gap: a sink must assert HPD
+ serve a valid EDID over DDC for the source to output. The idle card asserts
neither, so the source never starts. This is WHY every idle signal test this
session saw nothing - there was never a signal on the wire to detect.

REFERENCE (Yuan QCAP Linux SDK, ~/Downloads/test/SDK 1.1.0.202.0/.../QCAP/LINUX/
qcap_linux_sdk_1_88_0). The samples open our exact device by name:
  QCAP_CREATE("MZ0380 PCI", 0, win, &dev, TRUE, TRUE);
  // header: "MZ0380 PCI" IS FOR SC350, SC3C0, SC550, SC560, SC5C0
  // header: SC3C0N4/N8/N16 (MZ0380 PCI) DON'T SUPPORT AUTO STANDARD DETECTION
Card identity: Yuan SC3C0-family, SoC = SL6010 (per SC280/SC380 guide). The
"no auto standard detection" line officially confirms M6: host must supply the
video standard; only signal lock (present/absent) is reported back.

MINIMAL BRING-UP that wakes the source (sc5c0n1.c initialize_capture):
  QCAP_CREATE(...); register NO_SIGNAL/SIGNAL_REMOVED/FORMAT_CHANGED callbacks;
  QCAP_SET_VIDEO_INPUT(dev, QCAP_INPUT_TYPE_HDMI /* =2 */);
  QCAP_SET_AUDIO_INPUT(dev, EMBEDDED_AUDIO /* =0 */);
  QCAP_RUN(dev);
No explicit EDID or HPD call - the front-end asserts HPD + serves EDID as a
side effect of input-select + RUN. Our driver boots the firmware but never
selects the input or runs the pipeline, so the front-end stays powered down
and HPD is never asserted. THAT is the gap to close.

INPUT CODES (qcap.h): COMPOSITE0 SVIDEO1 HDMI2 DVI_D3 COMPONENT4 RGB5 SDI6
  AUTO7 DP8. HDMI = 2. This is the value for op41 SET_VIC_PARAMS cmd[6].

GPIO PATH (SC280/SC380 guide, "access SL6010's GPIO"): property 940 =
  direction (1=output), 941 = data (1=high). These map to ep.ko GPIO opcodes
  23 (direction) and 21 (set). HPD to the source is plausibly one of these
  pins ("GPIO controlled by the first chipset in one board"). Doc example even
  shows SET(940,0xFFFF);SET(941,0xFFFF) = all 16 pins output-high.
SIGNAL LOCK IS READABLE after all (AMESDK guide): AMESDK_GET_LOCK(dev[ch],
  &status) returns a 4-bit value, bit N = channel N lock. So lock (present/
  absent) is host-visible in the official stack - reconcile with M6: it is the
  STANDARD (WxH/fps) that is not auto-detected, not the lock bit.

CURRENT DRIVER GAP: input_select is wired as a BAR register poke
  (input_select_reg module param) - wrong model. Input-select is the op41
  mailbox command (cmd[6]=input code), not a register write. Rewire to op41.

NEXT (activation, in cheap->involved order):
  1. Issue op41 SET_VIC_PARAMS(input=HDMI=2, W=1920,H=1080,fps=60) over the
     mailbox after firmware-ready, then check: does the source wake (OUT lights
     up / monitor on OUT shows it) and does a no_signal/lock event fire?
  2. If not, assert the HPD GPIO: op23 set pin(s) to output, op21 drive high
     (start from the documented all-high, then narrow). Re-check source wake.
  3. Full RUN = program cfg banks (op2/4/8) + op41 + arm host XDMA. Milestone C.

EXPERIMENT RESULTS (2026-07-05, driver builds; new /proc/mz0380-hdmi op41 +
/proc/mz0380-cmd generic mailbox sender). Command path CONFIRMED WORKING:
  - op41 SET_VIC_PARAMS(input=2, various WxH/fps): ret=0, card echoes 0x29 in
    the result slot (opcode ack). First send after load times out (-110) as a
    warmup/stale-event drain; subsequent sends complete. => op41 completes but
    does NOT wake the source and does NOT light OUT. Consistent with the disasm
    (op41 only stores input + sets no_signal + sysfs_notify; no front-end power).
  - GPIO fully controllable: op20 GPIO_READ returns 0x48 (bits 3,6) at idle;
    op23 dir=0xffff then op21 data=0xffff -> re-read op20 returns 0xffff (write
    verified stuck). Driving ALL GPIO high did NOT wake the source. => GPIO is
    r/w-able but is NOT the HPD lever (or HPD alone is insufficient w/o EDID).
  CONCLUSION: neither op41 (declare format) nor GPIO (all-high) makes the card
  present itself as an HDMI sink. The blocker is the deeper front-end bring-up
  (receiver power + EDID load + HPD), which lives in the ARM-side modules/app,
  not ep.ko. Dispatched to a focused firmware RE (front-end chip, boot-vs-
  on-demand power, HPD mechanism, default-EDID presence, host trigger chain).
  NB: op41 result slot convention - cmd_last_param[0] = opcode echo, not data.
  NB: after all-high poke the card GPIO is left 0xffff; reload to restore 0x48.

================================================================================
M8  ROOT CAUSE (2026-07-05, firmware RE #2): the flashed firmware is the SDI
    demo build; it has NO HDMI receiver driver. Front-end mismatch.
================================================================================

Disassembled the whole firmware image (ep.ko + yuan_ioctrl app + front-end .ko's).
/lib/firmware/mz0380/MZ0380.HD.HEX (914KB gzip, a 2020 Yuan build) extracts to a
SINGLE tree: yuan_demo_sdi/. Front-end modules shipped: spi_gv7601.ko (Gennum
GV7601 = 3G-SDI receiver, SPI, /dev/gv7601), NULLSensor.ko, vpl_vic.ko, i2c-gpio.
The HDMI receiver (ITE IT6604) driver is NOT in the image - IT6604 appears only
as an uncompiled build option (drivers/Kconfig "HDMI audio (for IT6604)",
Kbuild IPCam/ + IT6604_Audio.ko under CONFIG_IT6604). => this is the SDI variant
firmware; nothing in it powers/initialises an HDMI receiver, asserts HDMI HPD, or
serves EDID. Byte-scan for an EDID header (00 FF FF FF FF FF FF 00) across the
whole tree = zero. No "hpd"/"edid"/"ddc" strings anywhere.

ARCHITECTURE (front-end agnostic SoC): the external receiver (GV7601 for SDI,
IT6604 for HDMI) converts the input to BT.1120 16-bit parallel video and feeds
the SL6010 SoC VIC (vpl_vic.ko, /dev/vpl_vic0; libvideocap.so.13 = the "No
signal" source). The PCIe/encoder path (ep.ko + vma_* encoders) is identical
regardless of front-end. So on an HDMI card running THIS firmware, spi_gv7601
tries SPI to a GV7601 that isn't present, while the card's actual IT6604 (I2C) is
never initialised -> no HPD/EDID -> the source stays dark. This is the root cause
of every "source won't wake" result above.

FIRMWARE IS A REGISTER PROXY: yuan_ioctrl (the on-card app) hardcodes NO receiver
register values. It boots, opens /dev/gv7601 + /dev/i2c-0,1 + /sys/vpl_pciep/
command, clears the "dency" gate, and blocks in poll() waiting for host commands.
The HOST supplies every receiver register write over the mailbox:
  op24 (0x18) GV7601 SPI read      op25 (0x19) GV7601 SPI write
  op33 (0x21) SPI group read       op34 (0x22) SPI group write
  op26 (0x1a) I2C read             op27 (0x1b) I2C write
  (ep.ko forwards opcode<=34,!=41 via sysfs_notify "command" to yuan_ioctrl,
   which reads a 44B packet, packet[0]=opcode, and dispatches - main switch
   @0x97c4. GV7601 SPI = 16-bit addr/16-bit data; ioctl(fd,1)=write ioctl(fd,125)
   =read.) So the receiver init register list lives in the VENDOR HOST driver
   (QCAP/libvideocap/Windows), NOT the firmware - that is the missing piece.

CONFIRMS op41 is software-only (pciep_isr@0x171c): packet[5]=input, [8]=W u16,
[0xa]=H u16, [0x22]=int_reduce; W==0||H==0 -> no_signal flag; else set VIC code +
notify encoder. Touches no receiver reg, no GPIO. Also: i2cdetect/i2cget binaries
ship in the tarball (they run on the card ARM) documenting the I2C proxy ABI.

IMPLICATION / FORK:
  A. This is the wrong-front-end firmware. If a retail Elgato (HDMI) firmware or a
     Yuan HDMI build (with the IT6604 driver + init) exists, flashing THAT brings
     HDMI up natively. Provenance of MZ0380.HD.HEX (a 2020 Yuan SDI demo) is
     suspect - it is almost certainly not the retail HD60 Pro firmware.
  B. Stay on this firmware and host-drive the receiver via the I2C proxy (op26/27):
     probe /dev/i2c-0,1 for the IT6604, then replay its full power+EDID+HPD init
     register sequence from the host. Hard - needs the IT6604 register script,
     which is not in this blob.
  NEXT (cheap, decisive): probe the front-end. op24 SPI-read a GV7601 chip-ID
  reg (is an SDI rx even there?) and op26 I2C-scan i2c-0/1 for the IT6604 - see
  what ACKs. That confirms the physical front-end + whether the proxy can reach
  it, before committing to A or B. (Need the op24/op26 packet param layout first.)

================================================================================
M9  FRONT-END IDENTIFIED + FIRMWARE IS CORRECT (2026-07-05, retail Windows
    driver RE). HDMI receiver = MStar MST3367, host-driven over I2C.
================================================================================

CORRECTION to M8's "wrong firmware": the flashed /lib/firmware/mz0380/
MZ0380.HD.HEX is BYTE-IDENTICAL (md5 616643fb..) to the retail Elgato
MZ0380.HD.HEX, version 01.11 = exactly what the card reports. So the firmware is
CORRECT. Retail set (Elgato HD60 Pro install): MZ0380.HD/SD.HEX + MZ0381.HD/SD
.HEX + FW.TXT(01.11). "HD"/"SD" != HDMI/SDI (the .HD blob ships the GV7601 *SDI*
driver, .SD ships neither) - they are board/version variants, not front-ends.

FRONT-END (from the Windows kernel driver e60MZ0380.X64.SYS, at the dual-boot
mount /run/media/.../Program Files/Elgato/Game Capture HD60 Pro/): the HDMI
input receiver is a **MStar MST3367**, driven ENTIRELY by the host over I2C via
the firmware's MCU I2C proxy (op26/27; "mcu_i2c_access", "i2c_write_bytes"). The
firmware's spi_gv7601 is for the SDI variant and is vestigial on the HDMI board.
The .SYS also supports SA7160 / TW2968 (analog SD) and GV7601 (SDI) front-ends -
Elgato picks MST3367 for HD60 Pro HDMI. There is NO IT6604 here (M8's Kconfig
IT6604 was a red herring).

The .SYS carries the complete HDMI bring-up (debug strings as anchors):
  - MST3367 init/power: "MST3367 DELAY", "MST3367_ADC_AUTO_PHASE",
    "MST3367_HDMI_MODE_DETECT( 0x55 = 0x%x )" (mode/timing read from reg 0x55).
  - EDID: host BUILDS + LOADS an EDID into the receiver - "CDevice::Enter
    UpdateEDID", "send EDID data", "update EDID checksum", "[UPDATE.EDID] ELGATO
    BOARD SC5C0N1 1080P". No raw EDID header (00 FF..00) in the binary, so it is
    constructed in code or written straight into MST3367 EDID RAM via I2C. For
    waking the source, ANY valid 1080p CEA EDID should suffice.
  - HPD: "[HOTPLUG %d]" - an HPD assert path exists (MST3367 register or GPIO).
  - Mode detect reg 0x55 => this is ALSO a potential real signal/format read
    (contradicts nothing in M6: that was ep.ko; here the host reads MST3367 regs
    over I2C directly). GetHDMIDotClock derives pixel clock + audio sample rate.

=> ROOT CAUSE (final): our driver boots the (correct) firmware but never runs the
MST3367 HDMI-receiver bring-up that the Windows driver does over I2C. No MST3367
init + no EDID + no HPD => the source sees a dead sink => stays dark. op41/GPIO
were always irrelevant to this.

NEXT: RE e60MZ0380.X64.SYS to extract (a) the MST3367 power-up/init I2C register
sequence, (b) EDID load sequence (or use a generic 1080p EDID), (c) HPD/[HOTPLUG]
assert, (d) MST3367 I2C slave address + which bus, (e) the op26/27 I2C mailbox
packet framing - then replay it from the Linux driver via /proc/mz0380-cmd.
Dispatched to a focused Windows-driver RE agent.

================================================================================
M10 MST3367 BRING-UP RECOVERED (2026-07-05, e60MZ0380.X64.SYS RE). The exact
    replayable HDMI-receiver init/EDID/HPD/detect sequence. THE unblocker.
================================================================================
Full detail in memories [[mz0380-mst3367-i2c-abi]] + [[mz0380-mst3367-edid-and-detect]].
Windows x64 driver, image base 0x140000000; working copy + disasm in the session
scratchpad. All verified against call sites.

MAILBOX TRANSPORT (host side, confirms our BAR0 model): single BAR writer
sub_140285074. Frame = dwords, DOORBELL LAST:
  +0x00 = 0x800 (doorbell/go, written last)   +0x04 = op-number (verbatim, no shift)
  +0x08 = word2   +0x0c = word3 (read result returned here for some ops)
  +0x10 = word4 (I2C write value / I2C read result)   +0x2c = completion, poll bit0
Ops: 0x1a=I2C read, 0x1b=I2C write, 0x1f=bulk (EDID), 0x15=GPIO-set, 0x14=GPIO-read,
0x20=MCU raw-I2C passthrough. (These match /proc/mz0380-cmd: opcode->+0x04,
params->+0x08.. . NB our fw 1.11 ep.ko may stub some; the Windows-paired
yuan_ioctrl implements them - verify each empirically.)

MST3367 I2C ADDRESSES (8-bit): PRIMARY bank = 0x9C (HDMI core/timing/detect,
this is the one to use); others 0x88/0x98/0x60/0x90/0x94 (aux banks);
EDID DDC EEPROM = 0xA0. Paged: to touch a banked reg, first write the page# to
reg 0x00 (op 0x1b) - pages 0,1,2. Helpers: reg-write sub_14028658c (op0x1b,
args dev=0x9C,page,reg,val); reg-read sub_140277884 (op0x1a). MCU passthrough
slave 0x55->0xAA via op 0x20 sub-tag 0x66 (sub_140275804) - separate from banks.

INIT (CAVEAT): NO static {reg,val} table exists. Two runtime megafuncs compute
values from the detected/forced timing: config sub_14024efc8 (~106 writes, gated
on input-type @this+0x73a8) + timing/PLL sub_14024dc28 (~69 writes). ADDRESSES/
ORDER/PAGES/commit-reg are recovered; per-mode VALUES are dynamic. Skeleton
(dev 0x9C p0): FIRST write reg 0x0F=0x20 (soft-reset/power); then clears 0x54=0,
0x0E=0, input-select 0x04/0x01, window 0x05/0x06/0x07/0x1F, PLL 0xB2, analog/EQ
0xE2-0xE4/0xAB, output BT.1120 window 0x18-0x1A/0x2D-0x2F/0x3A/0x3B/0x39/0x2C,
output timing gen 0x80-0x86; const from sub_14024dc28: 0xB1=0xC0, 0xB4=0x55 then
RMW &0xFC, page2 0x61. COMMIT/strobe = reg 0x51 (write 0x00 then 0x21 to latch).
=> the HDMI-1080p data VALUES need a LIVE I2C TRACE of a working Windows bring-up
(or an MST3367 datasheet). This is the one remaining unknown.

EDID: fully recovered, 256 bytes, checksums valid -> saved verbatim to
mz0380-edid-hd60pro.txt (from .data @0x140350a20). Write = 8x 32-byte bulk (op
0x1f) to I2C 0xA0; frame word2 = (len<<16)|(block<<8)|0xA0 then 32 bytes; loop
0..0x100 step 0x20. Orchestrator CDevice::UpdateEDID sub_14024678c validates
header + checksum first. Any valid 1080p CEA EDID substitutes.

HPD = a GPIO write (NOT an MST3367 reg): op 0x15, word2=(1<<pin) mask, word3=
(state<<pin). HPD pin = 1 (mask 0x02, value 0x02). Asserted AFTER EDID load
(UpdateEDID loads EDID -> logs "[HOTPLUG %d]" -> raises pin, sub_140287d00).
Plug read-back: op 0x14 GPIO-read, word2=(1<<pin), result bit at +0x0c.

MODE/FORMAT DETECT (real signal read! resurrects milestone B): all op0x1a dev
0x9C. reg 0x55 = signal status, LOCKED/HDMI when (val & 0x3C)==0x3C (gates
detect). Timing from regs 0x40-0x45 (dot-clock + H/V total counters, (hi<<8)|lo),
0x47 (color-depth/pixel-repeat: low nibble 5/6/7 => /5,/3,/2), 0x57&0x3F/0x58/
0x59&0x3F/0x5A/0x5B/0x5C/0x5F (Htot/Vtot/Hact/Vact/sync), page2 0x28/0x29 (counter
hi bits), page1 0x01 bit2 (lock/interlace), 0x4C p2 (audio rate). Dot-clock =
divide 1600000/1250000 by counters; funcs sub_14024d5e0 (detect) + sub_14024a174
(GetHDMIDotClock).

REPLAYABLE CHECKLIST:
  1. reset/power: write dev0x9C p0 reg 0x0F=0x20, then 0x0E/0x54 clears.
  2. run config seq (addresses/order above); HDMI-1080p VALUES from live trace.
  3. commit: reg 0x51 = 0x00 then 0x21.
  4. load EDID: 8x32B bulk (op0x1f) to 0xA0 (bytes in mz0380-edid-hd60pro.txt).
  5. assert HPD: op0x15 pin1 (mask 0x02, val 0x02) - AFTER step 4.
  6. poll reg 0x55 until (v&0x3C)==0x3C; read timing regs, decode per above.
ONLY remaining unknown = the per-mode MST3367 register VALUES (step 2). Get via
a live I2C capture on Windows, or by an op0x1a dump of 0x9C regs after a working
Windows bring-up, or from the MST3367 datasheet.

================================================================================
M11 OP26/27 ARE LIVE (yuan_ioctrl, 2026-07-05 am) — but every I2C read NAKed
================================================================================

Disassembled the on-card `yuan_ioctrl` (ARM ELF from the firmware tar). It
services the I2C-proxy opcodes ep.ko merely sysfs-notifies about — so M4c's
"0x1a is a stub" was wrong at the SYSTEM level: ep.ko's sysfs_notify hands the
command buffer to the userspace daemon, which does the real work. Dispatch is
jump-table on (opcode - 24) @0x97d8; idx8 combo handler @0x9d98.

Mailbox slot map for op 0x1a/0x1b (host BAR0 offsets): dev8 @0x08 (card does
addr7 = dev8 >> 1), reg @0x0c, val/result @0x10. Doorbell/framing identical to
the proven SEND_COMMAND path.

EMPIRICAL RESULT (sentinel in the result slot): every read attempt at every
address on "both buses" left the sentinel untouched — read as NAK-everywhere,
"not even the EDID EEPROM answers". See M12 for why that conclusion was partly
an artifact.

================================================================================
M12 WINDOWS-SIDE RE COMPLETE (2026-07-05 pm) — authoritative I2C protocol,
    GPL MST3367 driver found, corrections to M10/M11. Docs vendored at
    docs/re-2026-07-05/ (HD60-PRO-LINUX-DRIVER.md is the reference).
================================================================================

A Windows-side session disassembled BOTH the retail e60MZ0380.X64.SYS
(v1.1.0.194; loaded v195 byte-compatible) AND the on-card firmware, and found
that an existing GPL-v2 driver (stoth68000/hdcapm, Startech USB2HDCAPM — same
Vatics Mozart 395s + MST3367) already contains the complete MST3367 register
logic RE'd from the same Windows driver. Cross-validated against our disasm.

VERIFIED PROTOCOL (both sides agree; supersedes M11's bus map):
  - READ = op 0x1a, WRITE = op 0x1b, dev = 8-BIT 0x9C in the command word;
    the card computes addr7 = addr8 >> 1 (0x9C -> 0x4E). We already sent 0x9C.
  - 0x1e/0x1f = multi-byte read_s/write_s on BUS0 (NOT bus1 — M11's bus map
    was wrong). 0x1c/0x1d = HDCP-encrypted ops, the ONLY bus1 users.
    0x20 = combo (addr8 @byte0, rw @byte1, len @bytes2-3 of word +0x08;
    payload from +0x0c; card chunks 16B internally).
  - Registers are BANKED: write reg 0x00 = bank (0..3) first; Windows caches
    the current bank (ctx+0x2090). Reg 0x55 etc. are BANK0.
  - No separate MCU: "mcu version = 0" / "NO MCU" — the SoC is the I2C master.
    mcu_i2c_access passthrough theory is dead.
  - Card boots itself from flash at slot power; fw 1.11.1.11 confirmed live.

HARDWARE CONFIRMED (teardown-corroborated): MST3367CMK-LF-170 receiver,
Vatics Mozart 395s encoder SoC (NOT MStar SL6010), ITE IT6621FN HDMI front-end.

WHY M11 SAW "NAK EVERYWHERE" — reconciliation with the 0xEE-sentinel data:
  - The bus0 NAK was REAL, not an artifact: fw forces result=0x00 on I2C
    failure, and every bus0 probe came back 0x00 (sentinel 0xEE overwritten).
    yuan_ioctrl ran, the kernel I2C_RDWR failed. Result-race excluded for bus0.
  - But the pin-map theory ("demo firmware's gpio-i2c doesn't reach the
    MST3367") is now DEAD: the same card, same flashed fw 1.11.1.11, works
    under Windows on this machine. Bus0 GPIO12/13 does reach the receiver.
  - => The receiver I2C domain is GATED until something Windows does first.
    Candidates, in test order: (1) input select / SET_VIC (AUTO.INPUT runs in
    Windows D0 init before receiver access); (2) MZ0380_HwInitialize-era
    config setters (op 0x02/0x08 blocks) / windows_select_fw; (3) a GPIO
    power/reset pin via op 0x15 (HPD is already known = pin1; another pin may
    be receiver reset — read the bitmap with op 0x14 before guessing);
    (4) SA7160_HARDWARE_I2C_RESET-equivalent bus recovery.
  - DISASM CONFLICT to resolve when it matters: our yuan_ioctrl RE put
    0x1e/0x1f (read_s/write_s) on BUS1; the Windows-side RE of the same binary
    puts them on BUS0 (only HDCP 0x1c/0x1d on bus1). Either way the MST3367 is
    bus0 via 0x1a/0x1b; the EDID 0xA0 target of the bulk op 0x1f follows
    whichever bus 0x1f really uses. Bus1 probes were inconclusive (op 0x1e
    does NOT force 0 on NAK; sentinel survived = no info).

GPL DRIVER GIVES THE MISSING VALUES (M10's "one remaining unknown" CLOSED):
hdcapm mst3367-drv.c has init_setup, HPD (BANK0 0xB7 bit1), HDMI reset (BANK2
0x07 f4->04), HDCP reset (BANK0 0xb8 10->00), mode detect (BANK0 0x55 & 0x3c;
htotal 0x6a/0x6b, vtotal 0x5b/0x5c, hperiod 1600000/(0x57<<8|0x58), vperiod
1250000/(0x59<<8|0x5a), interlaced 0x5f bit1, hactive BANK2 0x29<<8|0x28) and
the full video-standards timing table. Local copy:
docs/re-2026-07-05/mst3367-reference-from-gpl-driver.md.
Architecture: register an i2c_adapter whose master_xfer tunnels through the
mailbox, then reuse mst3367-drv.c as a V4L2 i2c subdev nearly unmodified.

256-byte EDID recovered (valid checksums, "SC530-N1", 1080p60 preferred):
docs/re-2026-07-05/elgato-hd60pro-EDID.bin — push verbatim.

BRING-UP ORDER (Windows): reset -> write EDID -> assert HPD -> mode detect.
A source outputs NOTHING until EDID+HPD are presented; reg 0x55 reads
no-signal even with I2C working — do not conflate with NAK.

NEXT: run scripts/mz0380-mst3367-test.sh (root, module loaded) — baseline 0x9C read
(nonzero result = first-ever ACK), GPIO bitmap read, then the gating
candidates: --with-vic (SET_VIC first), GPL init/reset sequence. First real
ACK unblocks the whole hdcapm reuse path.

================================================================================
M13 HDCAPM BRING-UP REPLAYED ON HW — DECISIVE: addressing correct, mailbox
    result path proven, MST3367 STILL NAKs after input-select + full init.
    Gate = a host-driven receiver reset/power GPIO. (2026-07-05 evening)
================================================================================

Ran mz0380-mst3367-test.sh --with-vic on the card (fw 1.11.1.11 live). Results:

ADDRESSING CONFIRMED CORRECT (no double-shift): we send dev 0x9c; the card
echoes 0x4e back in the dev slot (0x9c >> 1 = 0x4e = 7-bit MST3367). So the
8-bit-address convention is right and the card shifts exactly once.

MAILBOX RESULT PATH PROVEN WORKING: GPIO read (op 0x14, mask 0xee) returned a
REAL value 0x20 in the result slot (BAR0+0x0c) — nonzero data comes back when a
handler produces it. So a 0x00 read result is a true NAK, not a transport bug.

SET_VIC (input select) COMPLETED: ret was -110 only because our wait polls
STATUS; the card actually signalled completion via EVENT bit11 (=0x800, drained
before the next command). So input=HDMI(2) 1920x1080@60 WAS selected.

MST3367 STILL DEAD: after --with-vic + the full GPL hdcapm init_setup + HDMI
reset (B2 0x07 f4/04) + HDCP reset (B0 0xb8 10/00), EVERY read (0x55, 0x6a/0x6b,
0x5b/0x5c, 0x57-0x5a, 0x5f, 0xb7 in BANK0; 0x28/0x29 in BANK2) returns 0x00 =
NAK. Since a write to a NAKing device fails silently (status still "done"), the
init WRITES almost certainly never landed either → the bus is DEAD/GATED, not
merely unconfigured.

WHY (firmware role clarified): the card firmware is a DUMB I2C PROXY. Disasm of
the on-card yuan_ioctrl shows NO MST3367 / reset / GPIO / HDMI logic at all —
only i2c_read/i2c_write/spi. ALL receiver bring-up (SA7160_HARDWARE_I2C_RESET,
mode detect, etc.) lives in the Windows host driver and is replayed over the
mailbox. ep.ko owns the GPIO (gpio_direction_output / gpio_set_value, symbol
gpio_dir_settings), reachable from the host via op 0x14 (read) / 0x15 (set) /
0x17 (dir). So the missing step is a HOST-driven receiver power/reset GPIO that
Windows asserts (its SA7160_HARDWARE_I2C_RESET) BEFORE any receiver I2C. HPD is
GPIO pin1; the receiver reset/power pin is a different, still-unidentified pin.

drivers.sh note (Mozart-SDK generic): bus map `i2c-gpio bus_num=2 scl0=12
sda0=13 scl1=4 sda1=5`; a comment calls gpio12/13 the SSM2603 audio-codec bus
and gpio6/7 the "VIC control pins ... from sensor or video chip" (commented-out
alt). Windows proves /dev/i2c-0 does reach the MST3367 on this same firmware, so
the pin pair is fine — the blocker is receiver POWER/RESET state, not routing.

NEXT (M14): identify the receiver-reset GPIO and pulse it before I2C.
  1. Read the FULL GPIO bitmap (op 0x14 mask 0xffffffff) as a baseline.
  2. Bus-liveness proof: write reg0=0x02 then read reg0 back; 0x02 = bus alive,
     0x00 = confirmed dead. (Distinguishes gate-from-reset vs wrong-result-slot.)
  3. Get the exact reset pin from ep.ko's gpio_dir_settings init (disasm in
     progress) and/or the Windows SA7160_HARDWARE_I2C_RESET's 3 GPIO words
     (dir/data/mask). Then op 0x17 dir-out + op 0x15 pulse low->high on that
     pin, re-probe 0x55. CAUTION: some Mozart GPIOs gate PCIe/DMA — use the
     identified pin, not a blind sweep; reload the module after.

================================================================================
M14 RECEIVER-RESET GPIO IDENTIFIED (Windows e60MZ0380 disasm, 2026-07-05 eve).
    The dead I2C bus is because pin9 (MST3367 reset, active-low) is asserted.
================================================================================

Disassembled the Windows driver's GPIO paths. Two protocol facts + the pin map:

op 0x15 (GPIO SET) is SINGLE-PIN mask+data, NOT a full-bitmap write (corrects an
ep.ko-disasm guess): the only builder (Win helper 0x140287d00, args dl=pin,
r8b=level) emits word2 = mask = (1<<pin), word3 = data = (level<<pin). So it
drives ONLY the masked pin. There is NO op 0x17 (GPIO DIR) builder anywhere —
Windows never sets direction; pins are pre-configured as outputs by the card
firmware. op 0x14 (GPIO READ) builder = 0x140277f98.

SA7160_HARDWARE_I2C_RESET (fn 0x140293408) drives NO GPIO — it is pure MMIO to
the SoC's on-chip I2C-master block (base [ctx+0x108], regs 0xb008 cmd / 0xb00c
status / 0xbfd8..0xbfe8 clock dividers). It resets the I2C CONTROLLER, not the
receiver. (So that string was a red herring for the reset pin.)

GPIO PIN MAP (op 0x15, pin = bit index, mask = 1<<pin):
  pin 1  (0x02)  = HPD (confirmed). assert/deassert around EDID re-read.
  pin 3  (0x08)  = RX / mux enable. driven =1 once at bring-up (0x14028574f).
  pin 8  (0x100) = companion reset/power strap, driven low alongside pin9 low.
  pin 9  (0x200) = *** MST3367 RECEIVER RESET, ACTIVE-LOW ***. Canonical pulse
                   1->0->1 in 5+ routines (0x14027755c etc). RELEASE = drive
                   HIGH (1); assert = drive LOW (0).
  pin 10 (0x400) / pin 11 (0x800) = alt-board-variant resets (guarded branches).
MST3367 bring-up orchestrator = 0x14028548c: interleaves pin9 reset pulses with
I2C writes to slaves 0x9a/0x88/0x9c/0xb8 (0x9c = MST3367; the others = front-end
/ HDCP / companion chips).

=> ROOT CAUSE of the dead bus (M11-M13): the card powers up with pin9 LOW =
MST3367 held in reset, and nothing on the Linux side ever releases it. Every
I2C txn NAKs because the chip is in reset. Windows releases pin9 (+ pin3 enable)
during D0 bring-up before touching the receiver.

FIX (mailbox commands, no direction needed):
  RX enable:      {0x800, 0x15, 0x08,  0x08}        (pin3 = 1)
  reset pulse:    {..,0x15,0x200,0x200} high ->
                  {..,0x15,0x200,0x000} low  (delay) ->
                  {..,0x15,0x200,0x200} high (release)   (pin9 1->0->1)
  companion:      pin8 low during the low phase, high on release.
Then run the hdcapm init + probe reg 0x55.

TEST: scripts/mz0380-mst3367-test.sh --gpio-reset   (P3g runs exactly this sequence,
then re-probes 0x55 + a write/read-back bus-liveness check). First nonzero read
= MST3367 out of reset = whole bring-up unblocked.

================================================================================
M15  *** SUCCESS *** MST3367 OUT OF RESET, LOCKED TO 1080i. First working HDMI
     signal detection on Linux. (2026-07-05 evening, ran --gpio-reset)
================================================================================

Releasing GPIO pin9 (receiver reset) was the whole blocker. Ran
mz0380-mst3367-test.sh --gpio-reset. Sequence pin3=1 (RX enable), pin9 1->0->1
(release reset), pin8 companion. Results:

- Immediately after the pin9 pulse (before any register init): reg 0x55 read
  0x03 (was 0x00), and a write reg0=0x02 read back 0x02 => the I2C bus is LIVE,
  writes AND reads now ACK. The dead bus was purely pin9 held low.
- After the hdcapm init_setup + HDMI/HDCP resets, the mode-detect block reads a
  real locked signal:
    0x55 = 0x7b   -> & 0x3c = 0x38 (nonzero) = LOCKED / signal present
    0x6a,0x6b = 08,98 -> Htotal = 0x0898 = 2200
    0x5b,0x5c = 04,38 -> (vtotal-ish) 0x0438 = 1080
    0x57,0x58 = 12,8f -> hperiod_raw 0x128f=4751 -> 1600000/4751 ~= 337
    0x59,0x5a = 10,4f -> vperiod_raw 0x104f=4175 -> 1250000/4175 ~= 300 (=>~30fps)
    0x5f = 47 -> bit1 set = INTERLACED
    BANK2 0x28,0x29 = 80,07 -> Hactive = 0x0780 = 1920
  => 1920 active, Htotal 2200, interlaced, ~30 frame/s = 1080i59.94. Matches the
  sibling sc0710's known {htot 2200, 1920, interlaced} 1080i signature exactly.
- User observed: the source (camera) entered HDMI mode and STAYED in it after we
  ran this — the bring-up genuinely activates the input.

PROVEN END-TO-END BRING-UP RECIPE (host, over the mailbox):
  1. RX enable:      op 0x15 pin3 = 1        {0x800,0x15,0x08,0x08}
  2. reset pulse:    op 0x15 pin9 1 -> 0 -> 1 (delay each) ; pin8 low then high
  3. hdcapm init_setup (BANK writes, docs/re-2026-07-05/mst3367-reference-*.md)
  4. HDMI reset B2 0x07 f4->04 ; HDCP reset B0 0xb8 10->00
  5. poll BANK0 0x55; locked when (0x55 & 0x3c) != 0
  6. read timing: Htot 0x6a/0x6b, Vtot 0x5b/0x5c, hperiod 0x57/0x58,
     vperiod 0x59/0x5a, interlaced 0x5f&2, Hact BANK2 0x29/0x28; match table.
  (EDID push + HPD pin1 assert still to be added for sources that gate on EDID;
   this source locked without it.)

NEXT: fold steps 1-6 into the driver as mz0380_mst3367_bringup() +
query_signal, called from card_setup / on an input-select. Then the endgame:
an i2c_adapter tunneling the mailbox so hdcapm mst3367-drv.c runs as a V4L2
i2c subdev. Milestone B (real signal/format to host) is now OPEN and WORKING.

================================================================================
M16 DRIVER INTEGRATION — live VIDIOC_QUERY_DV_TIMINGS working. (2026-07-05 eve)
================================================================================

Folded the bring-up + detect into the driver (mz0380-mst3367.c):
  mz0380_mst3367_bringup()     - pin3=1, pin9 1->0->1, pin8 strap, hdcapm init.
  mz0380_mst3367_read_signal() - poll BANK0 0x55; if locked, read the timing
                                 regs and map to v4l2_dv_timings via a CEA preset
                                 table. Wired into mz0380_query_signal() and
                                 called once at card_setup (dev->mst3367_ready).

RESULT: v4l2-ctl --query-dv-timings returns a full valid timing, e.g.
  1920x1080, total 2200x1125, pixelclock 74.25 MHz, CTA-861 VIC 34 (1080p30).
The interlace bit / fps now decode correctly with stable reads; the M15
"interlaced" read was a racy artifact (see the read-race gotcha below).

*** CRITICAL GOTCHA - mailbox read result race (root of two integration bugs) ***
The card's userspace proxy (yuan_ioctrl) writes the read result into the PARAM3
slot (BAR0+0x10) *after* ep.ko posts command completion, and it writes only the
LOW BYTE of that slot. Two consequences the driver must handle:
  1. Reading dev->cmd_last_param[3] at the send_command() completion edge returns
     the PREVIOUS command's byte on back-to-back reads (off-by-one). The manual
     probe script hid this with a 200 ms "late re-read"; the driver did not.
  2. A full-word sentinel (0xffffffff) never clears in the high bytes (firmware
     touches only byte0), so a "high bytes == 0" settle check times out forever.
FIX (mz0380_periph_read): preload a distinctive LOW-BYTE sentinel (0xa5) into
the result slot, fire, then poll (slot & 0xff) until it != sentinel (~30 ms
budget); return the low byte. NAK -> 0x00, ACK -> value, both != 0xa5.

Milestone B (host reads HDMI format off the receiver) is DONE and in the driver.
Remaining: EDID push + HPD for sources that gate on EDID; then the DMA/stream
path (milestone C) and the i2c_adapter-tunnel refactor to reuse hdcapm verbatim.

================================================================================
M17 STREAMING/DMA PROTOCOL — both-sides RE (ep.ko + e60MZ0380.X64.SYS).
    The current mz0380-dma.c ring model is FICTIONAL; real path below.
    (2026-07-05 evening; milestone C)
================================================================================

Two independent disassembly passes (on-card ep.ko + Windows host driver) plus
our own empirical opcode knowledge converge on this. The existing DMA code
(BAR5 ring base/size/head/tail regs, START/STOP 0x12/0x13, IRQ regs at BAR0
0x100/0x104/0x108) is entirely invented and must be replaced.

REAL MODEL = card-programmed iATU outbound window + BAR0 status window, NOT a
host-programmed ring:
- Host allocates contiguous host buffers and DMAs frame data NOWHERE itself;
  the card's encoder DMAs frames INTO those host buffers via a PCIe iATU
  outbound window (15 MiB / 0xF00000 per channel) that the card programs from a
  host-supplied physaddr.
- Host hands the card the buffer physical addresses via mailbox CONFIG opcodes
  op 0x02/0x03/0x04/0x05/0x08 (host-side proven). Each is a 12-word command:
    word0=0x800, word1=opcode, word2=channel, word3=stride(0x2000),
    word4..=4x (phys_hi, phys_lo) pairs.   (loop channel 0..7)
  Card-side ep.ko confirms: op2/4/8 ISR handlers bulk-copy host cmd words into
  the channels[] array; pcie_set_outbound(idx) then copies the {lo,hi} pair into
  the iATU (elbi_base+0x58/+0x54), region ctrl +0x74, size +0xd4=0xF00000.
- ARM = mailbox op 0x29 SET_VIC_PARAMS (width/height/fps/interlace/color...).
  We already send this and it COMPLETES (M13). size==0 -> card sets no_signal;
  size!=0 -> encoder armed. The card's own video_capture_mgr reacts by setting
  its internal sysfs flags (hready/epint/dency..wency) - these are CARD-SIDE
  plumbing, NOT host-facing, so the host does pure mailbox.
- STOP = mailbox op 0x2a STOP_STREAMING(fw). (Old 0x12/0x13 are bogus.)

FRAME COMPLETION (host-side proven; matches our existing event-ack):
- IRQ cause = EVENT word BAR0+0x30. bit11 = command-done; byte-lanes bits0..23
  = per-channel frame-done. 0xEEEEEEEE=error, 0xAAAAAAAA=ready sentinel.
- Frame token = BAR0+0x40: (token & 7) = completed buffer/descriptor index,
  (token & ~7) = frame pointer/token. Host xchgs it into its own producer ring,
  then clears BAR0+0x50 = 0.
- Per-channel encode status byte = BAR0+0x50+N (== enc_statN sysfs). Packed
  done-count nibbles at BAR0+0x40/0x44/0x48/0x4c (card-side).
- ACK (both sides agree, == our working event ack): BAR5(ctx+0x110)+0xDC = 2 ->
  BAR0+0x30 = 0 -> doorbell BAR0+0x00 = 0x400.
- Buffer geometry: 8 channels x 0x20 (32) descriptors = 256 slots; per-buffer
  stride 0x2000; per-descriptor written-length flags in a host array
  (Windows ctx+0x2590). One full buffer per descriptor index.

OPCODE-NUMBERING CAVEAT (host-side agent error, corrected by our empirics):
- The Windows-side pass claimed "no op 0x29" and "op 0x15/0x17 = stream enable".
  WRONG per hardware: op 0x29 SET_VIC completes on our card (M13); op 0x15 is
  GPIO set (pin9 reset woke the receiver, M15). So op 0x15/0x17 are GPIO, and
  SET_VIC(0x29) is the real arm. Trust the CARD-SIDE ep.ko opcode map + our
  empirics; use the host-side pass only for the buffer-address command SHAPE and
  the completion/token/ack mechanism (which it proved cleanly).

OPEN ITEMS to resolve on hardware (can't be settled statically):
1. Whether SET_VIC alone starts frames once buffers are set, or a separate
   enable/kick mailbox op is needed. (Card userspace does epint internally;
   host may need nothing beyond SET_VIC.)
2. Exact frame BYTE-LENGTH source: a BAR0 status reg (card-side suggests
   +0x28..+0x44 window / +0x50+N byte) vs a card-written host-memory field.
   Read candidates on the completion IRQ and see which tracks frame size.
3. Which of op2/3/4/5/8 maps to which stream (H.264 vs preview vs audio); start
   with op2 = the encoded-video buffer set.

DRIVER REWRITE PLAN (mz0380-dma.c), keeping the vb2/IRQ scaffolding:
  setup:  alloc N contiguous buffers (start 1 channel, 4-8 descriptors of
          0x2000+ each; H.264 frames are small); send op2 (+3/4/5/8) with their
          phys pairs; keep the existing MSI request.
  arm:    on vb2 start_streaming: send SET_VIC_PARAMS(0x29) with detected WxH/fps
          (reuse mz0380_activate_hdmi_locked packing).
  isr:    read EVENT BAR0+0x30; if a frame-done lane, read token BAR0+0x40,
          idx=token&7, copy that buffer to the head vb2 buffer (length from the
          candidate length reg), vb2_buffer_done; ack (0xDC=2,0x30=0,doorbell
          0x400) via the existing mz0380_mb_ack_event.
  stop:   op 0x2a STOP_STREAMING.
  Replace all CHECKME ring/IRQ reg offsets in reg.h with the BAR0 status-window
  offsets above; delete the base/size/head/tail ring regs and the xdma plan-B.

Primary evidence: ep.ko pcie_set_outbound@0x5a8, store_channel_done@0xdc8,
pciep_isr@0x1210, msi.constprop@0x115c, epint_store@0x19b8; Windows arm routine
0x140278ce0, buffer-addr ops 0x14027957b.., ISR 0x14028ec70 (EVENT@0x30,
token@0x40), ack 0x14028eca4.

================================================================================
M18 STREAMING FIRST-CUT TEST — hang fixed; SET_VIC alone does NOT start frames.
    (2026-07-05 late)
================================================================================

Ran the first-cut streaming path (enable_dma=1 enable_video=1). Findings:

BUG FIXED (regression from enabling the MSI ISR): mz0380_send_command took the
MSI-wait branch whenever msi_enabled, but this card completes short commands via
STATUS bit0 with NO interrupt (only ~3 MSI ever fire). So every command
(GPIO 0x15 in bring-up, 0x1b register writes) timed out 500-1000 ms each -> the
MST3367 bring-up alone stalled insmod ~30 s ("stuck"). Fix: send_command now
ALWAYS polls STATUS for completion and additionally honours dev->cmd_complete
(set by the ISR if it catches an EVENT-bit11 command-done). insmod is instant
again, no timeouts.

EMPIRICAL (open item #1 resolved): with buffers programmed via op 0x02 + arm via
SET_VIC_PARAMS(0x29, ret=0), NO frames flow: IRQ count stays 3, zero frame-token
EVENTs, v4l2 capture blocks, /tmp/o.h264 = 0 bytes. => SET_VIC is necessary but
NOT sufficient; a separate START KICK is required, and/or op 0x02 is not the
right buffer-address opcode.

STILL UNKNOWN (both RE passes were fuzzy here): the exact HOST mailbox opcode
that makes the card's video_capture_mgr start a channel. Card-side said the kick
is an internal `epint` sysfs (set by video_capture_mgr, not host-facing);
host-side said op 0x17/0x15 "enable" - but op 0x15 is GPIO on our card (M14/M15),
so that mapping is suspect. Need: RE video_capture_mgr's mailbox-command
handling to find which host opcode -> channel start (and confirm the
buffer-address opcode), rather than guessing on hardware. Frame length reg
(item #2) and op->stream map (#3) remain open behind this.

================================================================================
M19 STREAMING START = SET_VIC spawns tinyvenc (video_capture_mgr RE). Remaining
    gaps need tinyvenc RE or a live Windows trace. (2026-07-05 late)
================================================================================

RE'd video_capture_mgr (ARM, stripped; resolved via PLT + literal pool). It is a
command DISPATCHER daemon, not the encoder:
- opens /sys/vpl_pciep/epint O_RDWR, poll()s it, on event pread()s the 44-byte
  mailbox command struct, dispatches on cmdbuf[0]=opcode (@0x8df4+).
- op 41 (SET_VIC, @0x8ea8): populates a per-channel channels[] config from the
  command bytes, sprintf's a `./tinyvenc5 -D ... -w %d` (or venc7) command line,
  and **system()-spawns tinyvenc @0x9348**. THIS is the start - there is NO
  separate epint/ency/hready mailbox kick the host must send. SET_VIC with valid
  fields IS the "go".
- op 42 (STOP): killall -9 the encoders, then writes hready=0.
- BAIL branches in the op-41 handler (why tinyvenc may never spawn):
    @0x8ec4: if cmdbuf[5] == 0  -> jumps to the no-signal/no-bitstream path.
    @0x8eb4: if table[cmdbuf[5]] == 8 -> reject.
  cmdbuf[5] = mailbox byte 0x09 = params[0] bits[15:8]. Our current packing puts
  fps there (nonzero), so we likely pass 0x8ec4 - but the exact field semantics
  (channel/format/codec at cmdbuf+4/+5/+6, W/H at +8/+10) are not fully pinned,
  and a wrong codec/format byte still mis-launches or picks the wrong tinyvenc.

- The *ency sysfs (dency/qency/hency/wency) are written by yuan_ioctrl = the SDI
  (GV7601) path, NOT the HDMI/MST3367 path. Irrelevant here.

BUFFER-ADDRESS OPCODE (ep.ko pciep_isr): op2 copies host cmd words -> BAR0
+0x08..+0x24; op4 -> +0x48; op8 (PREVIEW_BUF_EX) -> +0x28. None call
pcie_set_outbound directly; the exported pcie_set_outbound(idx,lo,hi) is called
by TINYVENC, reading the address the host left at BAR0+0x08 (op2). So op2 is the
best candidate for the encoded-video buffer physaddr - confirm the words land at
BAR0+0x08.

FRAME LENGTH: NOT in ep.ko. store_channel_done writes only 4-bit per-channel
status nibbles (BAR0 +0x40/44/48/4c) + the done token (+0x30) + MSI. The encoded
byte length is written by tinyvenc into its own output descriptor. => need to RE
tinyvenc5/7 (now extracted to scratchpad fw/yuan_demo_sdi/) for the length field
+ the exact channels[] struct it consumes.

WHY NO FRAMES (hypotheses, unresolved): (1) SET_VIC struct fields mis-packed so
tinyvenc launches wrong / not at all; (2) hready gate not asserted by the host
(host mechanism unknown - card sysfs, host must trigger via a mailbox op/BAR
write; op1/INIT may or may not set it); (3) buffer physaddr not reaching BAR0+0x08.

RECOMMENDED NEXT (most efficient): a LIVE Windows mailbox trace - breakpoint
SEND_COMMAND @0x140285074 (or the arm routine @0x140278ce0) in WinDbg during a
working 1080p capture and dump every {opcode, args} in order. That resolves ALL
remaining unknowns at once: the exact buffer opcode+physaddr args, the exact
SET_VIC 44-byte struct, whether/how hready is asserted, and the start ordering.
Static alternative: RE tinyvenc5 for the channels[] struct + length descriptor.

================================================================================
M20 STREAMING CONTRACT NAILED (tinyvenc5 + video_capture_mgr RE). SET_VIC bug
    found + fixed; frame-length located. (2026-07-05 late)
================================================================================

SET_VIC (op 0x29) 44-byte struct - EXACT field map (video_capture_mgr op-41,
@0x8ea8; channels[] base 0x141d4 stride 60; tinyvenc argv reads this record):
  struct byte (=param word: params[0]=struct[4..7], params[i]=struct[4+4i..]):
    [4]     channel (0)
    [5]     fw/index
    [6]     FORMAT/CODEC: {2,3}=tinyvenc5 H.264 (2 prog, 3 interlaced - inferred),
            7=tinyvenc7. MUST be in {2,3} for venc5.
    [8..9]  width  (LE, MUST be >127)
    [10..11]height (LE, MUST be >127)
    [22..23]input_frame_width
    [24..25]input_frame_height
    [26..27]bitstream_num (MUST be >=1)
    [30]    is_nosg
The op-41 bail branches (0x8eb4 cmp #8; 0x8ec0) are argv/boot-fixed
(product_type, debug flag), NOT host-controllable, and BOTH still spawn tinyvenc.
So SET_VIC always launches the encoder; the host-side musts are: width>127,
height>127, byte6 in {2,3}, bitstream_num>=1.

THE BUG (why the first cut spawned tinyvenc but produced nothing): our SET_VIC
sent bitstream_num=0 and input_frame_w/h=0 (params[2..7] were 0). FIXED in
mz0380_dma_start:
    params[0] = fmt << 16;                    // struct[6] format 2/3
    params[1] = (h<<16) | w;                  // width, height
    params[4] = w << 16;                       // struct[22..23] input_w
    params[5] = (1<<16) | h;                   // struct[26..27]=1, [24..25]=input_h
(1080p example struct: 29 00 00 00 00 00 02 00 80 07 38 04 00*10 80 07 38 04
 01 00 ...; 0x780=1920, 0x438=1080.)

FRAME DELIVERY (tinyvenc5, not stripped):
- Per frame: DMA via the SoC MMA engine (TK_MMA_SetOptions/ProcessOneFrame,
  PCIEtOptions @0x7ed8c) from a physically-contiguous MemBroker buffer; ep.ko's
  ATU outbound window (programmed from the host addr at BAR0+0x08 via
  pcie_set_outbound) relays it into host DRAM. tinyvenc never reads BAR0 itself.
- Notify: pwrite(/sys/class/vpl_pciep/channel_done, record, 24) once per frame.
  record = 6xu32; word0 = channel. ep.ko store_channel_done turns this into the
  host MSI + the BAR0+0x40/44/48/4c status nibbles + BAR0+0x30 EVENT.
- FRAME BYTE LENGTH = binary u32 at **enc_stat struct +0x08** (from PB_GetEncBytes
  delta), also mirrored in the channel_done record. keyframe flag at result+0x94.

RISK / OPEN: tinyvenc's EncodingGroup::enable_dma gate (@0x7ed88) is set 1 only
when input-type [cfg+0x02] == 4 or 9 in this yuan_demo_sdi build; our HDMI format
is 2/3. The retail Windows driver clearly DMAs HDMI frames with this same fw, so
either [cfg+0x02] is a different (input-select) value than byte6, or the gate is
bypassed on the HDMI path. Watch for it: if the SET_VIC fix still yields no host
IRQ, this gate (input-type) is the next thing to resolve.

Host buffer address: goes to the card via op 0x02 (lands at BAR0+0x08, feeds
pcie_set_outbound). hready is NOT a hard gate; op2-before-SET_VIC is the real
requirement.

================================================================================
M21 hready is NOT the blocker (ep.ko RE). Confirms enable_dma is the sole gate.
    (2026-07-05 late)
================================================================================

RE of ep.ko host_ready/channel_done path:
- /sys/class/vpl_pciep/channel_done: .show=show_host_ready (returns host_ready
  @.bss+0), .store=store_channel_done. Separate writable node `hready`
  (hready_store -> kstrtoint -> writes .bss+0 = host_ready).
- host_ready is a STATUS word, NOT a hard interlock: store_channel_done and the
  MSI path (msi.constprop @0x18a0) branch on pirq_base != 0, not on host_ready.
  No `if(host_ready) deliver` test in the frame path.
- NO host mailbox opcode sets host_ready (op1/INIT included - no such store).
  It's set by on-card userspace via the hready sysfs node.
- Re-confirmed op2 (@0x1458): copies mailbox [ep_command+0xc..+0x28] ->
  ep_regs+0x08..+0x24 = the DMA-target address staging. Matches our op2.

=> hready is a RED HERRING for "no frames". The sole remaining blocker is
tinyvenc's enable_dma input-type gate (M20: [cfg+2] must be 4/9; HDMI=2/3).
Next session: don't re-chase hready. Resolve enable_dma via the live Windows
trace (what the retail driver sends to turn on host-DMA for HDMI) or by fully
tracing tinyvenc5 main @0xded0 ([r5+2] source, literals @0xef04/0xedbc).

================================================================================
M22 THE REAL BLOCKER: missing START_STREAMING (mailbox op 0x06). enable_dma gate
    was a MISREAD. Fix implemented. (2026-07-06, full on-card static RE)
================================================================================

Path B (static RE of the on-card yuan_demo_sdi userspace) resolved the gate
definitively. M20's "enable_dma input-type 4/9 gate" was WRONG - it conflated
three different globals that happen to be tested near each other in tinyvenc5.

GROUND TRUTH (symbols from the not-stripped tinyvenc5 ELF; all addrs its .text):
  0x7ed88 = EncodingGroup::enable_dma        (byte)
  0x7dd3c = EncodingGroup::total_channel_num (word)  <- "%d channels"
  0x7dd40 = EncodingGroup::preview_settings  (struct; +2 = input-format byte)
- enable_dma is WRITE-ONLY in tinyvenc5: set 0 @0xe350 (STOP path), set 1
  UNCONDITIONALLY @0xe97c inside the START_STREAMING handler. Nothing in .text
  reads it to gate frames. So it never blocked host DMA. (It is a status byte
  other code/ep.ko may observe; not a host-controllable gate.)
- The `==4 || ==9` compare @0xe968 (that M20 read as the enable_dma gate) really
  sets total_channel_num=4 (SDI multi-channel override). total_channel_num is
  otherwise loaded from argv via atoi @0xe2e0 in the getopt loop, so the
  video_capture_mgr-spawned path already gets a valid channel count (>=1) from
  the SET_VIC bitstream_num field. Not a blocker either.

ACTUAL START PROTOCOL (tinyvenc5 + video_capture_mgr + ep.ko, all cross-checked):
1. Both tinyvenc5 and video_capture_mgr read 44-byte commands from
   /sys/vpl_pciep/epint (O_RDWR). ep.ko's epint_show copies the raw host mailbox
   command (incl. opcode word) to readers; epint_show only passes the command
   set {6,7,9,40,41,42,45,47,49,80,81,82,96,97,98,110}.
2. Host SET_VIC_PARAMS (op 41 = 0x29): video_capture_mgr dispatches it (@0x8ea8),
   builds the long argv (-a..-w = fps/res/interlace/input_w/h/bitstream_num/...)
   and system()-spawns `./tinyvenc5 -D -a.. -w..`. ep.ko op41 also sets
   no_signal=0 when WxH!=0. video_capture_mgr does NOT handle cmd 6 and does NOT
   self-issue any start - it only spawns.
3. tinyvenc5 boots, opens epint, its FIRST read expects SET_VIC(41) (acks with a
   44-byte pwrite @0xf264), then enters a poll/pread loop.
4. Host START_STREAMING (op 6): tinyvenc5's dispatch is a jump table indexed by
   (cmd-6); table[0] (cmd 6) = the start block @0xe954, which spawns the encoder
   channels and begins the per-frame path (TK_MMA + pwrite
   /sys/class/vpl_pciep/channel_done -> ep.ko MSI + outbound ATU into host DRAM).
   ep.ko op6 ISR handler (@0x1854): if no_signal==0, sysfs_notify(epint) -> wakes
   tinyvenc5's poll. Op6 carries NO payload.
=> Frames need op 0x29 THEN op 0x06. Our driver only ever sent 0x29 (+ SET_BUF
   0x02). That is why IRQ count stayed at 3 with a 0-byte file, through every
   SET_VIC field fix - the encoder was spawned and idle, never told to start.

M6 RECONCILED: M6 read op6 from the ep.ko ISR only and labelled it
"enc-status-bank+notify", concluding "START_STREAMING is NOT an ep.ko opcode."
The ISR view was right (op6 = a gated sysfs_notify) but missed the tinyvenc5
consumer for which that notify IS the start kick. No contradiction; M6 was
half the picture.

FIX (implemented this session):
- mz0380-reg.h: MZ0380_CMD_START_STREAMING 0x12(bogus) -> 0x06.
- mz0380-dma.c mz0380_dma_start(): after SET_VIC succeeds, msleep(500) then
  send op 0x06 (no params). The delay lets the freshly system()-forked tinyvenc5
  exec + consume its first-read SET_VIC before our op6 lands as the next command
  (avoids racing tinyvenc5 start-up; tune on hw). Builds clean.
- STOP path unchanged: op 0x2a = 42 is handled by video_capture_mgr (@0x9358,
  killall tinyvenc) - correct as-is.

NEXT (hardware test): load, start capture, expect two acks (SET_VIC then
START_STREAMING) then a rising IRQ/token count + nonzero-length frames. If op6
acks but tokens stay flat: (a) increase the msleep (tinyvenc5 not up yet), or
(b) confirm no_signal==0 at op6 time (needs live locked source + real WxH in
SET_VIC). Frame LENGTH still at enc_stat+0x08 (M20) - wire once frames flow.

================================================================================
M23 THE OFFSET BUG + is_nosg BISECTION. Encoder was spawned-but-silent from a
    +2-byte SET_VIC packing error (M20/M22 were wrong); downstream DMA targets
    host 0 because SET_BUF(op2) never lands our buffer addr in channels[]. Full
    HD60 Pro fw-1.11 on-card RE via llvm-objdump. (2026-07-07)
================================================================================

FIRMWARE SOURCE: the HD60 Pro's own blob /lib/firmware .../MZ0380.HD.HEX
(gzip + GNU tar, ~2.5 MB) extracts to yuan_demo_sdi/ - the SAME package M6/M20/
M22 RE'd. So tinyvenc5 / video_capture_mgr / ep.ko ARE this card's fw-1.11
runtime, not a foreign SDI SDK. Boot rc.local: yuan_ioctrl + audio_capture_mgr
+ video_capture_mgr (latter spawns tinyvenc on SET_VIC). Un-stripped (tinyvenc5
~1162 syms). Disasm: llvm-objdump --triple=armv7-linux-gnueabi (system objdump
has no ARM target). Tree: scratchpad/fw/yuan_demo_sdi/.

no_signal (ep.ko, RE-CONFIRMED - kills the M20 "SoC signal-detect" idea):
  written ONLY in the op41 handler, purely from W/H:
    no_signal = (width==0 || height==0) ? 1 : 0   // width=ep_command[8],H=[0xA]
  No hardware/BT1120 read touches it; host fully controls it via op41 W/H. Gate:
  many opcodes incl. op6 hit `if(no_signal){printk("$$$ cmd(%d) => no signal");
  MSI; return;}`. Our W/H@8/10 were always correct => gate was OPEN the whole
  time; no_signal was never the blocker.

op6 (START) is FIRE-AND-FORGET (ep.ko handler .text 0x1854): if no_signal==0 it
  sysfs_notify(epint) [epint_1080p iff op41 byte[6]==7]. It posts NO mailbox
  cmd-done -> host MUST send op6 with timeout_ms=0. Waiting for a completion =
  the false -ETIMEDOUT(-110) we hit. epint_show returns the LIVE ep_command
  mailbox (len = ep_cmds_size[opcode]); op6 relies on its command still sitting
  in the mailbox, doesn't copy.

tinyvenc5: opens /sys/vpl_pciep/epint O_RDWR, poll()s. FIRST blocking read must
  be SET_VIC(0x29) (acks by pwrite'ing 44B back); later cmds dispatch via jump
  table index (opcode-6). op6 -> on_start_thread -> EncodingGroup::Start ->
  init_func: TKMF_VideoSrc_Init (fail => "Can't create video capture ->exit!!!")
  + spawn encode_handler (always) + fake_frame_process (iff is_nosg!=0). NO
  signal-lock check in tinyvenc: it pulls frames from an SSM shared-mem ring the
  CAPTURE side fills from BT1120; empty ring => usleep(1000) loop, silent.

SET_VIC 44-byte struct - byte offsets VERIFIED 3 ways in video_capture_mgr's
op41 handler (THE FIX):
  [4]ch [5]fps [6]fw/format(2 prog|3 int, ==7 alt) [7]interlace
  [8:9]width [10:11]height [12]m [13]flip [14]mirror [16:19]color_info
  [20:21]x_start [22:23]y_start [24:25]input_frame_width [26:27]input_frame_height
  [28]bitstream_num(MUST>=1) [29]osd_en [30]osd_size [31]is_nosg
  [35]is_slave [36:39]nosg back/y/u/v.  Spawn GATE: width<=127||height<=127 =>
  "Wrong resolustion", no spawn (our W/H pass).
  BUG: M20/M22 put input_w@22,input_h@24,bitstream_num@26 - all +2 short. FW
  then read bitstream_num=0 (byte28 unset) + junk input dims => tinyvenc spawned
  but emitted nothing. This (NOT timing, NOT no_signal) was the silence. Proof
  it wasn't timing: start_delay sweep 2000/4000ms = flat 3 IRQ.
  CORRECTED packing (params[i]->struct[4+4i]): params[0]=fmt<<16;
  params[1]=params[5]=(h<<16)|w; params[6]=1|((is_nosg?1:0)<<24).

is_nosg BISECTION (on-hw, decisive): stream_nosg=1 sets byte[31] -> card spawns
  fake_frame_process (black-frame gen, memset buf, timer channel_done, NO
  capture/SSM dep). Result: IRQ 164 3->7 AND a burst of IOMMU IO_PAGE_FAULTs at
  host 0x0,0x80,0x100..0x480 (128B stride). => the encode -> channel_done -> MSI
  -> outbound-ATU -> host-DMA path is ALIVE, but the ATU host target is ~0, not
  our buf 0xfff80000. Per ep.ko pcie_set_outbound the target comes from
  channels[] (window*24+field; ATU low@0x58/high@0x54), populated by op2/4/8
  copying ep_command[0xc..0x28]. So SET_BUF(op2) isn't landing our physaddr
  (likely hi/lo order or channel-entry offset; our buf hi=0 read as target-low
  => DMA to host 0). ATU aperture base 0x90000000 is hardcoded card-side (=the
  probe-time fault).

TWO REMAINING BUGS, cleanly separated by is_nosg:
  (1) DOWNSTREAM: SET_BUF(op2) channels[] host-addr layout wrong -> ATU hits
      host 0. Fix = correct op2 command layout (exact offsets being RE'd).
  (2) UPSTREAM: real BT1120->SSM capture not delivering -> real encoder idle
      (TEST1 nosg=0 stayed at 3 IRQ, no faults). Needs VIC / MST3367 BT1120
      output routing on the host-init side.

HOST FIXES THIS SESSION (built, hw-tested): op6 fire-and-forget (send_command
  timeout_ms=0); SET_VIC offsets corrected (bitstream_num@28=1, input dims@24/
  26); module params start_delay_ms (timing - now known irrelevant) and
  stream_nosg (is_nosg bisection lever, 0644). NEXT: fix SET_BUF layout, re-test
  nosg=1 (expect frames into our buffer, no fault), then upstream BT1120.

================================================================================
M24 DMA TARGET: engine works, but host target low32 is stuck 0 - frames land at
    (cmd[0xc] << 32) + aperture_offset. SET_BUF/channels[]/ELBI fully traced.
    Open blocker (paused). (2026-07-08)
================================================================================

is_nosg=1 (fake_frame_process) proved the encode->channel_done->MSI->outbound-
ATU->host-DMA chain is ALIVE: the card actively DMAs frames. The only defect is
the DESTINATION address. Empirically nailed (three hw runs):
  cmd[0xc]=0          -> IOMMU faults at host 0x0,0x80,0x100..0x480 (128B stride)
  cmd[0xc]=0xfff80000 -> faults at 0xfff8000000000000,..80,..100
  => host_target = (cmd[0xc] << 32) + aperture_offset;  LOW32 == 0 ALWAYS,
     independent of cmd[0x10]. The 0x80 stride is the frame walking the aperture.

CARD-SIDE TRACE (ep.ko + vpl_dmac.ko, llvm-objdump + capstone):
- channels[] entry = 192 bytes = 24 x 8-byte {hi,lo} slots per channel (stride
  192 = 0xC0 * channel). SIX buffer-load opcodes each memcpy ep_command[0xc..0x28]
  (8 words) into a DIFFERENT window region, each skipping slot0:
    handler@0x1458 -> entry+0x08 (also zeros entry+0), @0x14b8 -> +0x28 (sets
    [0x61c]=opcode, the SET_BUF_EX kick), @0x1588 -> +0x48, @0x12b0 -> +0x68,
    @0x1408 -> +0x88, @0x1518 -> +0xa8 (sets [0x620]=1). (The earlier "op2->+0x00"
    was WRONG; our op2 buf0 hi/lo land at entry+0x08/+0x0c, i.e. slot1.)
- pcie_set_outbound(window,ch,bufidx) (ep.ko @0x5a8) is the SOLE writer of the
  ELBI outbound-target regs. entry = channels + 8*(24*ch + (bufidx-1) + wfield).
  window0 wfield=0. It writes: ELBI[0x74]=0x90000000 (aperture base), [0x7c]=
  0x91FFFFFF (limit, 32MB), [0x54]=entry+4 (intended LOW), [0x58]=entry+0
  (HIGH, empirically effective), [0xd4]=0xf00000 (15MB). No ldrd; plain ldr/str.
- ELBI is a Vatics-PROPRIETARY glue block (ioremap phys 0x82040000, size 256),
  NOT DesignWare iATU (that's dbi_base 0x82000000). Init programs the INBOUND
  window on the same block: base@0x30, limit@0x38, target-LOW@0x40, target-HIGH@
  0x48 (spacing +8 from base+0x10). Init also ZEROES ELBI 0x54..0xCC at probe.
- INFERENCE (unproven): by inbound's field spacing, the OUTBOUND effective low-
  target is likely ELBI 0x84 (base 0x74 + 0x10), and pcie_set_outbound writes the
  low to 0x54 - a register the HW ignores (init-zeroed, never re-read). That is
  exactly "HIGH tracks channels+0, LOW always 0". So no op2/op4/op8 payload can
  set low32 via this path (only pcie_set_outbound touches ELBI, and it targets
  the wrong reg). vpl_dmac VPL_DMAC_StartTail/ISRTail copy profile[0x8..0x34] ->
  DMA-engine MMR (the real per-frame dst descriptor); profile[0x38/39/3a/3b] =
  window/channel/bufindex/outbound-enable, set at runtime by the card DMAC mgr.

CAVEAT (why the "unreachable" read is probably incomplete): the HD60 Pro works
normally with Elgato's driver on <4GB-RAM PCs, so low32 MUST be reachable by some
host path. We are likely mis-driving the buffer setup (wrong opcode, or the DMAC
manager builds profile[dst] from a channels[] slot we populate wrong), NOT hitting
a true hw dead-end. Unresolved.

PAUSED here (user choice). Two ways forward next session:
  (1) RIGHT WAY: RE how the WORKING path gets the host address into the DMA
      descriptor - trace vpl_dmac.ko profile[dst] source + the Windows driver's
      frame-buffer setup (our op2 came from M17, may be the wrong command).
  (2) WORKAROUND (guaranteed by the confirmed model): put the DMA buffer at a
      4GB-aligned IOVA (low32=0) via an explicit iommu_map at IOVA 0x1_0000_0000,
      set cmd[0xc]=high32(=1), cmd[0x10]=0 -> frames land in-buffer. Needs IOMMU
      remap or a reserved-mem boot param; a hack, but works if (1) stalls.
Fault flags: 0x20 (read-ish) vs 0x30 seen across runs. Disasm cached:
scratchpad/disasm.txt (ep.ko), scratchpad/dmac.txt (vpl_dmac.ko); fw tree at
scratchpad/fw/yuan_demo_sdi/.

================================================================================
M25 DMA TARGET, ROUND 2: host->card command aliasing PROVEN CORRECT (no off-by-N
    in op2); opcode->window/slot map completed; ELBI outbound target is the sole
    host-address input in the entire card firmware. (2026-08-07)
================================================================================

Re-extracted MZ0380.HD.HEX (gzip+tar -> yuan_demo_sdi/), re-disassembled ep.ko +
vpl_dmac.ko, and disassembled the WINDOWS driver (MZ0380.X64.SYS, PE32+ x86-64,
extracted from Game_Capture_HD60_Pro_1.1.0.177 via 7z).

1) MAILBOX ALIASING - our op2 packing is CORRECT (this was the top suspect after
   the SET_VIC +2 bug; it is now ruled out).
   ep.ko init (mozart_module_init):
     g[0x4]  = dma_alloc_coherent(0x60 bytes), bus addr -> g[0x724]
     g[0x624]= that buffer + 4                          <- "ep_command" base (r4)
     ELBI[0x08] = g[0x724] + 4                          <- card target of window0
     ELBI[0x30] = DBI[0x10] + 4    (host BAR0 base + 4) <- host base  of window0
     ELBI[0x38] = DBI[0x10] + 0x5f (host BAR0 + 95)     <- host limit of window0
   pciep_isr reads the opcode at [r4] == host BAR0+0x04. Therefore
        ep_command[i]  ==  host BAR0 offset (4 + i).
   So ep_command[0xc] == BAR0+0x10 == our PARAM(3), ep_command[0x10] == BAR0+0x14.
   Our mz0380_stream_program_bufs writes {channel, stride, hi, lo, ...} to
   PARAM(1..) i.e. BAR0 0x08,0x0c,0x10,0x14 -> lands exactly on ep_command
   [0x4]=channel, [0x8]=stride, [0xc]=hi, [0x10]=lo. VERIFIED CORRECT.
   (DBI = ioremap(0x82000000,4096) = DesignWare dbi; DBI[0x10]/[0x14] = the BAR0/
   BAR1 addresses the HOST BIOS assigned - that is how the card learns them.)

2) COMPLETE buffer-load opcode -> channels[] map (pciep_isr dispatch @0x1258):
     op 0x02 -> channels[ch] + 0x08   (also zeroes +0x00), window0 slots 1..4
     op 0x08 -> +0x28                 window0 slots 5..8
     op 0x04 -> +0x48                 window1 slots 1..4
     op 0x05 -> +0x68                 window2 slots 1..4
     op 0x03 -> +0x88                 window3 slots 1..4
     op 0x64 -> +0xa8                 window4 slots 1..4
   Each copies ep_command[0xc..0x28] (8 words = 4 x 8-byte slots), channel index
   from ep_command[0x4], stride 192 (0xC0) per channel. pcie_set_outbound's
   window->wfield add is {w0:+0, w1:+8, w2:+12, w3:+16, w4:+20} slots, which is
   exactly the region each opcode fills, starting at bufidx 2 (slot0 unused).
   => op2 IS the right command for window0. Not the M17 guess it was feared to be.

3) vpl_dmac.ko VPL_DMAC_StartTail (0xb5c) - definitive:
     if (profile[0x3b]) pcie_set_outbound(profile[0x38], profile[0x39],
                                          profile[0x3a]);   /* win, ch, bufidx */
     then copies profile[0x08..0x34] -> DMA MMR, MMR[0x8] |= 6 (go).
   profile[0x08..0x34] are CARD-LOCAL addresses (the 0x90000000 outbound
   aperture). No host address exists anywhere in the DMA descriptor.
   => the ELBI outbound target written by pcie_set_outbound is the ONLY host
   address input in the whole card firmware. There is no second path to RE.
   pcie_set_outbound is EXPORT_SYMBOL_GPL'd and vpl_dmac.ko is its only caller.

4) pcie_set_outbound (ep.ko 0x5a8), exact:
     entry = channels + 8*(24*ch + (bufidx-1) + wfield)
     ELBI[0x50] = 1              ELBI[0x74] = 0x90000000   ELBI[0x7c] = 0x91FFFFFF
     ELBI[0x54] = *(entry+4)     ELBI[0x58] = *(entry+0)   ELBI[0xd4] = 0xF00000
   Our slot layout {word0=hi, word1=lo} therefore feeds 0x58=hi, 0x54=lo - which
   is what the firmware intends. Hardware honours 0x58 and drops 0x54 (M24).

5) ELBI window-field spacing, now with two independent samples (fw_store and
   logo_store @0xc98/0xd00 program a SECOND inbound window at runtime):
     window0: host base @0x30, host limit @0x38, card target @0x08
     window1: host base @0x40, host limit @0x48, card target @0x10
     (init also writes ELBI[0x18]=0xc7000030 -> implies a window2 whose base/limit
      would be @0x50/@0x58 - exactly the registers pcie_set_outbound scribbles.)
   Two readings remain, and they are mutually exclusive:
     (a) 0x50=enable, 0x54/0x58 = outbound target lo/hi, 0x74/0x7c = card-side
         base/limit. Firmware is right, HW should honour 0x54 -> then our low32
         is being lost for some other reason.
     (b) 0x50/0x58 are inbound-window2 base/limit and the real OUTBOUND target
         low/high live elsewhere (0x84/0x8c by the +0x10 base spacing). Firmware
         writes the low to a register the HW ignores -> M24's "low always 0".
   The hw evidence (host_target == (word0 << 32) + aperture_offset, exactly)
   fits (b). Nothing in the extracted firmware writes ELBI 0x84.

6) WINDOWS DRIVER (MZ0380.X64.SYS) - what it confirms and what it does not:
   - Mailbox is identical: ctx[0x108] = BAR0 MMIO; opcode -> +0x04, params ->
     +0x08.., doorbell = write 0x800 to +0x00, completion = poll +0x2c bit0,
     500 x 1ms, then "MZ0380 COMMAND TIMEOUT". Matches mz0380-reg.h exactly.
   - Frame buffers: default path (ctx[0xc0]==0) is
     DmaOperations->AllocateCommonBuffer -> logical addr in ctx[0xc8], VA in
     ctx[0xd0]; fallback path is MmAllocateContiguousMemorySpecifyCache with
     HighestAcceptableAddress walked 0x07FFFFFF -> 0xFFFFFFFF in 0x8000000 steps.
     Both are <4GB, i.e. the Windows path always has high32 == 0 and a NON-ZERO
     low32 - the exact case our hardware says is impossible. So either the HW does
     honour a low-target register (and reading (a) is right), or the Windows
     driver reaches it by a command we have not yet located.
   - Opcodes >= 0x50 are NOT handled by ep.ko (dispatch sends 80/81/82 to the
     bare notify path @0x1824); they are consumed by card userspace
     (video_capture_mgr / tinyvenc5) reading the same command buffer. The
     Windows driver does send op 0x50 with 32-byte payloads. Not yet decoded.

NEXT (in order, cheapest decisive first):
  A. HW PROBE, ~20 min, settles (a) vs (b) for good: send op2 with the pair
     order SWAPPED (word0=low32, word1=high32) and with a deliberately
     recognisable low (e.g. buffer at a known IOVA). Watch the AMD-Vi fault
     addresses.
       fault at (low32 << 32)          -> word0 is the only word that reaches the
                                          HW -> reading (b) -> use workaround #2.
       fault at correct 64-bit address -> reading (a); the earlier low32 loss was
                                          a driver-side artifact, and we are done.
  B. If (b): 4GB-aligned IOVA workaround (M24 item 2) - iommu_map at
     0x1_0000_0000, cmd[0xc]=1, cmd[0x10]=0.
  C. Decode op 0x50 in the Windows driver + video_capture_mgr; it is the only
     remaining host->card channel we have not read.
Artifacts this session: scratchpad/fw/yuan_demo_sdi/ (re-extracted),
scratchpad/disasm.txt (ep.ko), scratchpad/dmac.txt (vpl_dmac.ko),
scratchpad/win/ (Windows driver package), scratchpad/win64.txt (MZ0380.X64.SYS).

================================================================================
M26 SWAP PROBE: CLOSED. The card latches ONLY the high 32 bits of the host DMA
    target; ELBI 0x54 is ignored by the hardware. Fix = 4GiB-aligned IOVA.
    (2026-08-07, hardware-verified)
================================================================================

PROBE (mz0380-m25-swap-probe.sh, buf_pair_swap module param, stream_nosg=1).
Same buffers both runs, dma=0xfff80000 / 0xfff00000 / 0xffe80000 / 0xffe00000
(all high32 == 0):

  buf_pair_swap=1  slot {word0=fff80000, word1=00000000}
                   -> IO_PAGE_FAULT at 0xfff8000000000000, +0x80, +0x100 .. +0x480
  buf_pair_swap=0  slot {word0=00000000, word1=fff80000}   (default packing)
                   -> IO_PAGE_FAULT at 0x0, 0x80, 0x100 .. 0x480

The target followed word0 in BOTH runs and word1 in NEITHER. With word1 =
0xfff80000 the low half of the target was still exactly 0. Therefore:

    host_target = (slot word0 << 32) + aperture_offset      [CONFIRMED]
    slot word1 -> ELBI[0x54] -> DISCARDED BY THE HARDWARE   [CONFIRMED]

This settles M24/M25 reading (b): pcie_set_outbound writes the host LOW target
to a register the Vatics glue block does not implement (init zeroes 0x54..0xCC;
by the inbound field spacing the live one is probably 0x84, which no code in the
extracted firmware ever writes). No mailbox opcode can reach it - op2/3/4/5/8/
0x64 only fill channels[], and channels[] is consumed solely by
pcie_set_outbound. Option A is dead.

Also observed on the swap=1 run: one fault at address=0x90000000, the card's own
outbound aperture base leaking through as a host address - an access made before
the window target was programmed. Harmless, but it confirms the aperture base.

Both runs: 4 MSIs, /tmp/cap-swap{0,1}.h264 = 0 bytes. Encoder alive, frames lost.

FIX IMPLEMENTED (M26): give the card a target whose low 32 bits are zero, i.e.
put each stream buffer at its own 4 GiB-aligned IOVA.
  - dma_alloc_coherent cannot be steered to a chosen IOVA, so the buffers are
    now alloc_pages() + an explicit iommu_map() into the device's IOMMU domain
    at dma_iova_base + (i << 32) (mz0380_stream_bufs_alloc_iova, mz0380-dma.c).
  - SET_BUF then sends word0 = IOVA >> 32 (1,2,3,4 with the default base) and
    word1 = 0, so host_target == the buffer base exactly, and the card's
    offset walk (0x0, 0x80, ...) writes straight into it.
  - New params: dma_iova_remap (bool, def 1), dma_iova_base (ullong, def
    0x100000000, must be 4GiB-aligned). buf_pair_swap must stay 0.
  - Requires a translating IOMMU domain: DMA / DMA_FQ / UNMANAGED. Identity or
    pass-through (iommu=pt, iommu=off) is rejected with a clear message; the
    code then falls back to dma_alloc_coherent and warns that frames will not
    land. Collisions with the DMA-API's own IOVA allocator are checked for with
    iommu_iova_to_phys() before every map (that allocator works down from the
    top of the aperture, so a low base is safe in practice).
RESULT (hardware, mz0380-m26-iova-test.sh): the high32 half now works exactly as
the model predicts. Domain type DMA_FQ, all four buffers mapped:
  buf0 phys 0x97b900000 -> IOVA 0x100000000 (high32=1)   SET_BUF slot {1, 0}
  buf1 phys 0x96ba00000 -> IOVA 0x200000000 (high32=2)   SET_BUF slot {2, 0}
  buf2 phys 0x97cc80000 -> IOVA 0x300000000 (high32=3)   SET_BUF slot {3, 0}
  buf3 phys 0x97b880000 -> IOVA 0x400000000 (high32=4)   SET_BUF slot {4, 0}
Faults are now at 0x100080000, +0x80, +0x100 .. +0x480 - i.e. inside buf0's
4GiB window (high32 == 1, as commanded), but at aperture offset 0x80000, one
MZ0380_STREAM_BUF_STRIDE past the base. Our 512 KiB mapping covers
0x100000000..0x10007FFFF, so the writes miss it by exactly one stride.

    host_target = (word0 << 32) + bufindex * stride     [refined, M27]

with the card starting at bufindex 2 (slot0 unused, M25 item 2) -> 1 * 0x80000.
That also explains the earlier runs: every buffer had high32 == 0 then, so all
four windows aliased onto 0 and the offset was masked by the noise.

STATUS: M26 high32 fix CONFIRMED on hardware. Aperture offset open -> M27.

================================================================================
M27 APERTURE OFFSET: two runtime knobs to pin down where inside the window the
    card writes. (2026-08-07, written, hw run pending)
================================================================================

New params (mz0380-core.c), both diagnostic until the hw picks a winner:
  set_buf_stride  (uint, def 0x80000) - the stride word in SET_BUF cmd[0x8].
        If the offset really is bufindex*stride, 0 drags the writes down onto
        the buffer base.
  dma_iova_offset (ullong, def 0)     - shifts every mapping up inside its 4GiB
        slot, so the buffer sits where the card already writes instead. Low 32
        bits of the IOVA no longer have to be zero: the card supplies them from
        the aperture offset, we only have to agree on the value.
Test: mz0380-m27-offset-test.sh - run A sends stride=0, run B shifts the
mappings by 0x80000. Exactly one should give zero faults; that becomes default.

RESULT (hardware): NEITHER passed, and the pair of results is more informative
than a pass would have been.
  run A  set_buf_stride=0      buffer base 0x100000000, slot {1, 00000000}
                               -> faults at 0x100080000 + 0x80 ..
  run B  dma_iova_offset=0x80000
                               buffer base 0x100080000, slot {1, 00080000}
                               -> faults at 0x100100000 + 0x80 ..
Both are exactly (buffer base + 0x80000). Two conclusions:
  - stride is NOT the source of the offset: sending 0 moved nothing. 0x80000 is
    a card-side constant (the frame area starts that far into the aperture).
  - run B advertised word1 = 0x80000 and the write moved up by 0x80000, so
    **word1 DOES reach the hardware**. M25/M26's "ELBI 0x54 is discarded" is
    WRONG and is retracted here.

CORRECTED MODEL (supersedes M24/M25/M26):

    host_addr = ((word0 << 32) | word1) + 0x80000,  low half adds mod 2^32

The 32-bit truncation is what produced the original M25 evidence: in the
buf_pair_swap=0 run word1 was 0xfff80000, and 0xfff80000 + 0x80000 = 0x1_0000_0000
truncates to 0 - hence faults at 0x0, which looked exactly like "the low word is
ignored". It was a wrap, not a discard.
STILL UNEXPLAINED: the buf_pair_swap=1 run (word0=0xfff80000, word1=0) faulted at
0xfff8000000000000, not 0xfff8000000080000 as the +0x80000 constant predicts.
Noted, not papered over; it does not block the fix.

================================================================================
M28 FIX: map each buffer card_frame_offset ABOVE the address advertised in
    SET_BUF. (2026-08-07, written, hw run pending)
================================================================================

  advertise: base + (i << 32)                     -> slot {i+1, 00000000}
  map at:    base + (i << 32) + card_frame_offset
  card writes advertised + 0x80000 == the buffer base.
word1 stays 0, so the mod-2^32 wrap is unreachable by construction; a wrap that
could still happen (non-default params) is warned about in the log.
New param card_frame_offset (uint, def 0x80000). set_buf_stride keeps its
default and is now known to be inert. Test: mz0380-m28-landing-test.sh.

RESULT (hardware): faults moved to 0x100000000 - the ADVERTISED address, offset
0 - which disproves the +0x80000 constant and exposes what was really going on.

================================================================================
M29 THE OFFSET WAS A BUFFER OVERRUN. Final model; every data point from M24 on
    fits it. (2026-08-07, hardware)
================================================================================

    host_addr = ((word0 << 32) | word1) + aperture_offset
    aperture_offset starts at 0; the low half adds MOD 2^32 (no carry to word0)

An IOMMU only faults on UNMAPPED addresses. Writes that land inside our mapping
are silent. So the first fault address is not where the card starts writing - it
is where the card leaves the buffer. Every "offset" we measured was a buffer
end, not an offset:
  M26     map 0x100000000 +512K, advertise 0x100000000
          -> faults at 0x100080000 == buffer END. Data WAS landing.
  M27 A   identical mapping, stride=0 -> same end -> same faults. Stride inert.
  M27 B   map 0x100080000 +512K, advertise 0x100080000
          -> faults at 0x100100000 == that buffer's END. (This is the run that
             "proved" word1 works - it does, but the +0x80000 was the overrun.)
  M28     map 0x100080000, advertise 0x100000000 -> the advertised address is
          UNMAPPED, so the very first write faults -> 0x100000000, offset 0.
And the two older ones, which now also fit:
  M25 swap=1  target 0xfff80000_00000000, nothing mapped there -> faults from
              offset 0 -> 0xfff8000000000000. Fits.
  M25 swap=0  target 0x00000000fff80000, which IS where dma_alloc_coherent put
              the buffer -> writes landed until the overrun at 0xfff80000 +
              0x80000 = 0x1_00000000, which wraps mod 2^32 -> faults at 0x0.
              Fits, and explains the "low word is discarded" reading exactly.
M24/M25/M26's "ELBI 0x54 is ignored by the hardware" is RETRACTED in full. The
low word always worked. The blocker was never the address protocol: it was a
512 KiB buffer the card streams straight past, plus one 32-bit wrap.

FIX (M29):
  - card_frame_offset default 0: advertise the buffer base as-is.
  - MZ0380_STREAM_BUF_SIZE 0x80000 -> 0x400000 (4 MiB, one order-10 block, over
    an uncompressed 1080p NV12 frame; 4 x 4 MiB fits the 32 MiB aperture).
  - The 4GiB-aligned IOVA remap (M26) is KEPT: it is no longer strictly
    required, but it pins word1 to 0, which puts the mod-2^32 wrap out of reach
    by construction. dma_iova_remap=0 falls back to plain dma_alloc_coherent.
Test: mz0380-m29-frames-test.sh. If faults now stop at a 4 MiB boundary the
card wants an even bigger buffer; if they stop entirely, read the "frame token"
lines for the payload head bytes.

RESULT (hardware): FAULT COUNT 0 - the first clean run in the whole M24..M29
sequence. All four buffers mapped and advertised cleanly:
  buf[i] phys 0xa26c00000 / 0xa21c00000 / 0xa28c00000 / 0xa31000000
         -> IOVA 0x100000000 / 0x200000000 / 0x300000000 / 0x400000000
  SET_BUF[i] target == buf == slot {i+1, 00000000}
The host->card DMA address protocol is SOLVED.

Not solved: no "frame token" lines, IRQ 164 still at 4 (all command-done),
/tmp/cap-m29.h264 still 0 bytes.

================================================================================
M30 IS THE CARD WRITING? Zero faults is ambiguous by construction. (2026-08-07,
    written, hw run pending)
================================================================================

An IOMMU reports only the writes that MISS. "No faults" therefore means either
"every write landed in our buffers" or "no writes happened", and the completion
path is silent either way, so it cannot break the tie. The buffer contents can.
Added mz0380_stream_bufs_dump() (mz0380-dma.c), called from mz0380_dma_stop()
before STOP_STREAMING, which logs per buffer: the first 16 bytes, how many
4 KiB-sampled pages are non-zero, and the highest non-zero offset - plus the
card's EVENT/token/payload/enc-status words and the IRQ counters.
  head 00 00 00 01 + non-zero pages -> card IS writing H.264 into our memory;
        the remaining bug is the frame-done MSI / token path, NOT the address.
  all zero                          -> the card writes nothing now; the target
        it holds is one it declines to use. Compare status words against M29.
Test: mz0380-m30-landed-test.sh.

RESULT (hardware): **THE CARD IS WRITING INTO HOST MEMORY.** Milestone.
  stop buf[0] @0x100000000 head=11 11 11 11 11 11 10 11 11 11 11 11 11 11 11 11
              | 779/1024 sampled pages non-zero, last @0x30a000
  stop buf[1] @0x200000000 all zero        (same for buf[2], buf[3])
  stream stop: EVENT=0 token=0 0x44=0 0x48=0 0x4c=0 enc=0 irqs=3/0
  fault count 0
Readings:
- ~3.19 MiB written (last non-zero sample at 0x30a000). A 1920x1080 4:2:0 frame
  is 3110400 == 0x2F7600. So what lands is an UNCOMPRESSED raw frame, NOT an
  H.264 bitstream - there is no 00 00 00 01 start code, the fill is a constant
  0x11 (one 0x10 in the head), which is what the card's fake_frame_process test
  pattern generator emits under stream_nosg=1.
- Only buffer 0 is written. The card is not rotating bufindex over our four
  slots, at least not in the fake-frame path.
- This also confirms the M29 diagnosis exactly: a 3.19 MiB write into a 512 KiB
  buffer overruns at 0x80000, which is precisely where every fault from M26
  onward began.
So the host<-card DATA path is now proven end to end. What is NOT working is the
COMPLETION path: no frame-done MSI (irq_video_count == 0), EVENT/token/enc all
zero, so mz0380_dma_drain_video never runs and nothing reaches vb2.

================================================================================
M31 OPEN: frame-done completion. The data arrives; nothing announces it.
================================================================================
Two independent questions, in priority order:
  1. Why no frame-done MSI? ep.ko has store_channel_done + encode_status0..15
     sysfs attrs, and the card's userspace pwrites channel_done when a frame is
     finished. Our MSI fires 3 times (all command-done, EVENT bit11) and never
     for a frame. Either the card needs an explicit enable we have not sent, or
     the fake-frame path skips the channel_done write that raises it.
  2. Raw vs encoded. What lands is a raw frame in the buffer set we programmed
     with op2 (window0). The H.264 bitstream, if it is produced at all, must go
     to one of the other windows (op3/4/5/8/0x64), whose targets we have never
     programmed - they would be 0, yet there are no faults, which suggests the
     encoder is not running or not DMAing in this path. Worth re-testing with
     stream_nosg=0 now that the address path works.
Note that a raw 1080p frame path is itself a usable capture driver (V4L2 NV12),
so option 2 is a fork in the road, not just a diagnosis.

CONTROL RUN (M25 swap probe re-run on the M30 build) - the landing is causal,
not a fluke:
  buf_pair_swap=1 (target deliberately wrong, {0,i+1}) -> 10 faults, ALL FOUR
                  buffers all-zero. Nothing lands.
  buf_pair_swap=0 (correct, {i+1,0})                   -> 0 faults, buf[0] 0x11
                  fill, 779/1024 pages, last @0x30a000. Reproduces M30 exactly.

================================================================================
M32 THE EVENT TOKEN IS NOT THE GATE. ep.ko delivery RE'd; the card's userspace
    is not declaring a finished frame. (2026-08-07)
================================================================================

ep.ko RE (from windowsDriver/hd60-trace/artifacts/oncard-binaries/ep.ko, which
the Windows session already extracted - disasm in ep-disasm.txt):

store_channel_done (0xdc8) - the sysfs attr the card's userspace writes when a
frame is finished. It takes 6 words (channel, then per-plane/per-stage values),
packs (val-1) into a 4-bit field per channel in the mailbox payload words at
0x40 / 0x44 / 0x48 - which is exactly our MZ0380_MB_FRAME_TOKEN and its
neighbours, and confirms the (token & 7) buffer-index reading - and then:
    if (G[0x630])   ... path A     (G[0x630] = SET_VIC cmd[0x22] != 0, the
    else            ... path B      "int_reduce" flag; path B also consults
                                     G[0x63c], set from another command's [0x11])
  BOTH paths end in the same delivery sequence, and both require *flag != 0:
    mailbox[0x30] = G[0x634] | bit ;  G[0x634] = 0 ;  *flag = 0 ;  ELBI[0xdc] = 1
  and when *flag == 0 they only OR the bit into G[0x634] and return - the event
  is accumulated, never delivered.

msi.constprop.1 (0x115c) - the COMMAND-done path - is gated on the SAME flag,
with the same consume-and-clear:
    r0 = G[0x634] | 0x800 ; if (*flag == 0) return ; mailbox[0x30] = r0 ;
    G[0x634] = 0 ; *flag = 0 ; ELBI[0xdc] = 1
pciep_isr_clrint (0xd44) sets *flag = 1 (and clears mailbox[0x30]).

So the card->host event channel is a strict one-shot ping-pong: one event is
delivered, the token is consumed, and the host's ack re-arms it. CRUCIALLY this
means the token CANNOT be our blocker: our command-done MSIs work and cycle that
exact token (3 of them per run), so it was armed while frames were flowing. Had
store_channel_done run at all, we would have gotten an MSI.
=> The card's userspace never writes channel_done for these frames.

HYPOTHESIS (M32 probe): op2 programs window0, which is where the RAW frame goes.
The ENCODER's bitstream destination is a different outbound window, one we have
never programmed - so tinyvenc has nowhere to write its output and never
completes a frame. Probe sends op 0x04 / 0x05 / 0x03 (windows 1/2/3) pointed at
stream bufs 1/2/3, which the card currently ignores, so it costs no memory.
New param probe_windows (def 0). Test: mz0380-m32-windows-test.sh.
If no window wakes up, the remaining unread host->card channel is opcode 0x50,
which ep.ko forwards to card userspace and which the Windows driver does send.

RESULT (hardware): HYPOTHESIS REJECTED, cleanly.
  probe window1 (op 0x04) -> buf[1] 0x200000000 ret=0
  probe window2 (op 0x05) -> buf[2] 0x300000000 ret=0
  probe window3 (op 0x03) -> buf[3] 0x400000000 ret=0
  stop buf[0] 0x11 fill, 779/1024 pages (unchanged)
  stop buf[1], buf[2], buf[3] ALL ZERO
  irqs 6/0 (3 commands + 3 probes, all acked), 0 faults, no frame token
All three opcodes were accepted, so the card took the addresses; nothing was
ever written to them, and no new fault addresses appeared either (so the
encoder is not writing to some OTHER unmapped target). The encoder is not
producing a bitstream at all, to any window. An unprogrammed destination was
not the blocker.

REMAINING LEADS, in order:
  1. Opcode 0x50 - the only host->card channel never decoded. ep.ko does not
     handle it (dispatch sends 80/81/82 to a bare notify); it is consumed by
     card USERSPACE, and the Windows driver sends it with 32-byte payloads.
     This is where a bitstream-buffer / encoder-config handoff would live.
     Needs: video_capture_mgr + tinyvenc5 disasm (they read the same command
     buffer), and the matching Windows-side call site.
  2. What makes video_capture_mgr call channel_done at all - RE it directly
     rather than inferring from the outside.
  3. stream_nosg=0 (real BT1120 capture) now that the address path works - the
     fake-frame generator may simply not drive the encoder end of the pipe.
  4. Fallback that needs no further RE: the raw ~3.1 MiB frame in buf0 is
     already a usable V4L2 NV12 capture path if driven by polling instead of
     the missing completion interrupt.

================================================================================
M33 THE ENCODER IS WAITING FOR AUDIO. Also: STOP_STREAMING is op 7, not 0x2a.
    (2026-08-07, from the card's own binaries; hw run pending)
================================================================================

Who writes channel_done: tinyvenc5, to /sys/class/vpl_pciep/channel_done. Not
video_capture_mgr. And tinyvenc5's is_nosg path blocks on AUDIO first:
    [tiny5]is_nosg cannot open /sys/audio_status/audio_ready
    [tiny5]is_nosg wait audio timeout, i2s_num=%d, audio ready[%d]
    [tiny5]is_nosg %d audio channels (wait %d times pass)
    [tiny5][ch %d] START_STREAMING -------------> is_nosg ACK
The only writer of that file is video_capture_mgr's SET_AIC_PARAMS handler:
    echo '1' > /sys/audio_status/audio_ready      (when the command's on byte
    echo '0' > /sys/audio_status/audio_ready       is set / clear)
We have never sent any audio command. That fits M30-M32 exactly: a raw frame
lands in our buffer, and channel_done is never written.

video_capture_mgr's dispatch, which also corrects an M17 guess:
    pread(fd, cmd, 44, 0) ; switch (cmd[0]):
       7  -> STOP_STREAMING        41 (0x29) -> SET_VIC
       42 (0x2a) -> SET_AIC_PARAMS    96/97/110 -> others
STOP is op 7, adjacent to START (op 6). 0x2a is the AUDIO command, so our
"STOP_STREAMING" at every streamoff was really SET_AIC with an all-zero payload
- i.e. "audio off" - and no stop was ever sent. Both fixed in mz0380-reg.h.

SET_AIC_PARAMS payload, recovered from video_capture_mgr's printf vararg order
(handler at vcm.txt 0x9358, format at 0xb57c) and cross-checked against the
Windows driver string "ai=%d, chs=%d, bits=%d, freq=%d, period=%d.%d,
is_aic_on=%d, aic_int_mode=%d". cmd+N == BAR0+4+N:
    cmd+4  u8  channel_num          cmd+5  u8  mono
    cmd+6  u16 bits                 cmd+8  u32 freq
    cmd+12 u16 frame_num_of_period  cmd+14 u16 period_num_of_buffer
    cmd+16 u8  on                   cmd+17 u8  aic_int_mode -> ep.ko G[0x63c]
(ep.ko's own op-0x2a handler reads cmd[0x11] into G[0x63c], the second gate in
store_channel_done - so both sides of this command are now accounted for.)

FIX: send SET_AIC(on=1) between SET_BUF and START. New params aic_on (def 1),
aic_channels/bits/freq/period_frames/periods. Test: mz0380-m33-audio-gate-test.sh.
Watch for "frame token" lines - that is the completion path we have never once
seen fire.

RESULT (hardware): SET_AIC(on=1, 2 ch, 16 bit, 48000 Hz, 1024 x 4) ret=0 -
accepted - and NOTHING ELSE CHANGED. No frame token, irqs 3/0, buf0 still gets
the raw 0x11 frame, bufs 1-3 empty, 0 faults. Audio was not the gate, or not the
only one. Note the timing: SET_VIC at t=7418.58 spawns tinyvenc5, SET_AIC lands
at t=7420.65 - two seconds later, because of start_delay_ms - so if tinyvenc5
samples audio_ready early and times out, we set it too late to matter. Worth one
retry with the AIC sent BEFORE SET_VIC before writing the idea off entirely.

WHAT THIS RUN DOES ESTABLISH: the 0x11 fill is EncodingGroup::fake_frame_process,
which is a tinyvenc5 symbol. So tinyvenc5 IS running, IS in its frame loop, and
IS reaching the DMA - it simply never completes a frame or writes channel_done.
The remaining unknown is inside tinyvenc5's per-frame path, between generating
the frame and the channel_done write.

HYPOTHESES TESTED AND REJECTED SO FAR (each one narrowed the search):
  M25/M26  "the hardware discards the low half of the DMA target"  -> retracted
           in M29; it was a buffer overrun plus a mod-2^32 wrap.
  M27/M28  "there is a constant aperture offset"                   -> no; same
           overrun seen from the other side.
  M32      "the encoder's bitstream needs another outbound window" -> no; all
           three extra windows accepted the addresses, none was ever written.
  M33      "the encoder is blocked on audio_ready"                 -> no visible
           change (but see the timing caveat above).

NEXT, in order of expected value:
  1. RE tinyvenc5's channel_done write site directly (re-dump/tinyvenc5.txt,
     string /sys/class/vpl_pciep/channel_done) and walk backwards: what must be
     true for it to be reached? This is the only place left that can answer it,
     and it stops the guess-and-check loop.
  2. Opcode 0x50 - still the only host->card channel never decoded, and the
     Windows driver does send it.
  3. AIC before SET_VIC (cheap retry of M33 with the ordering fixed).
  4. stream_nosg=0 real capture, now that the address path works.
  5. PRAGMATIC: the raw ~3.1 MiB 1080p frame in buf0 is a complete picture. A
     polling drain plus V4L2 NV12 would give working capture without decoding
     the encoder's completion path at all. This does not need any further RE.

================================================================================
M34 THE COMPLETION WRITE SITE, AND WHY IT IS NEVER REACHED. (2026-08-07)
================================================================================

tinyvenc5 opens /sys/class/vpl_pciep/channel_done in BOTH
EncodingGroup::encode_handler (0x12c60) and EncodingGroup::fake_frame_process
(0x15d58) - the latter is the one running in our stream_nosg=1 path, and the fd
is kept at sp+0x84. The write, at 0x168c8:

    16838: bl TK_MMA_ProcessOneFrame       <- the MassMemAccess DMA to the host
    1683c: r2 = *[0x16bfc] ; r4++          <- frame counter vs a limit
    1684c: cmp r2, r4 ; bgt 0x167cc        <- loop while more frames remain
    1686c: r2 = [r3+0xe98] ; bne 0x170d8   <- alternate exit, also a
    16878: r3 = [r3+0xe9c] ; bne 0x170d8      channel_done pwrite (0x170e8)
    168c8: r0 = fd ; r1 = r4-88 ; r2 = 24 ; r3 = 0
    168d8: bl pwrite                       <- CHANNEL_DONE, 24 bytes

24 bytes == the 6 words store_channel_done unpacks (ldm {r4,r8}, +8, +0xc,
+0x10, +0x14). Both sides of the completion protocol now reconcile exactly.

THE KEY ORDERING: channel_done is written only AFTER TK_MMA_ProcessOneFrame
RETURNS. Our data did land in the host buffer, so that call was entered and did
transfer. The state that fits every observation is that it NEVER RETURNS -
MassMemAccess_WaitDMAC blocking on a DMA-completion that never arrives:
  - exactly ONE frame's worth of data in buf0 (last non-zero at 0x30a000,
    against a 1080p 4:2:0 frame of 0x2F7600) and never more, across many runs;
  - tinyvenc5 demonstrably alive (its fake_frame_process produced that frame);
  - no channel_done, therefore no EVENT, no MSI, no token - all consistent with
    the thread being parked inside one call rather than looping.
So the question is no longer "why does the card not tell us" but "why does the
card's own DMA engine not report its transfer complete". That is a card-internal
signal (vpl_dmac's ISR / VPL_DMAC_ISRTail), and it is the next thing to read.

NEXT: VPL_DMAC_ISRTail + MassMemAccess_WaitDMAC (re-dump/vpl_dmac.txt, and
libmassmemaccess.so.9 which is in re-dump/fw/yuan_demo_sdi/). Specifically: what
does the DMAC ISR require before it marks a transfer done, and does the outbound
path need something from the host (a credit, a read pointer, an ack) that we are
not providing. Note ep.ko exposes hready/dency sysfs attrs that card userspace
reads - vcm does "echo '0' > /sys/vpl_pciep/hready" on stop, and tinyvenc5 opens
/sys/vpl_pciep/dency - so a host-readiness handshake exists and we have never
driven it. That is the strongest remaining suspect.

================================================================================
M35 THE WHOLE NOTIFY/CREDIT ARCHITECTURE, AND WHY "EXACTLY ONE FRAME" WAS NEVER
    PROVEN. (2026-08-07, static RE; hw discriminator pending)
================================================================================

ep.ko re-disassembled WITH relocations (llvm-objdump -dr) - every literal that
was an opaque `.word 0x0` in ep.txt now resolves. Regenerate via `-dr`; the old
dump hid all of the following.

1. THE CARD IS SYSFS-NOTIFY DRIVEN. pciep_isr does almost no work itself; it
   parks data in globals and calls sysfs_notify(pciep_kobj, NULL, <attr>).
   Card userspace poll()s those attrs. Full dispatch (r6 = opcode):
     1            -> print FIRMWARE Ready, set flag
     2 / 8        -> window0 slots 1-4 / 5-8 into channels[ch] (+ op2 CLEARS
                     host_ready=G[0], op8 sets wency_ready=8)
     3 / 4 / 5    -> windows 3 / 1 / 2 slot writes (per-channel struct 0xc0 apart)
     100 (0x64)   -> window4 slots + pre_uv_ready=1
     10           -> query: writes G[0x66c],G[0x670] back into mailbox cmd+4/+8
     20 / 21 / 23 -> GPIO (dir clear / single pin / 20-bit mask loop)
     41 SET_VIC   -> no_signal=G[0x71c] from (W==0||H==0); G[0x630]=int_reduce
                     from cmd[0x22]; windows_select_fw(.data+4) = cmd[6]==7 ? 7:5
                     (fw 7 = tinyvenc7/full-HD path, else 5 = tinyvenc5);
                     notify "epint"
     42 SET_AIC   -> G[0x63c]=cmd[0x11] (aic_int_mode); notify "epint" (or
                     "epint_1080p" if fw==7), then notify "audio_ctrl"
     6 START      -> notify "audio_ctrl", then "epint"/"epint_1080p". No MSI,
                     no mailbox touch - confirms M22 fire-and-forget.
     7 STOP       -> print STOP_STREAMING(fw), notify "epint"
     9,45,47,49,80-82(0x50-0x52),96,97,98,110,123,124
                  -> NOTHING but notify "epint"/"epint_1080p": pure doorbells.
                     These opcodes are handled ENTIRELY by card userspace, which
                     wakes and pread()s the 44-byte mailbox itself. This is how
                     op 0x50 (the "undecoded channel") works - decode it in
                     vcm/tinyvenc, not ep.ko.
   Simple ops (2,3,4,5,8,10,20,21,23,100) end with msi.constprop.1 = the
   cmd-done MSI. Userspace-routed ops get their cmd-done later, from the app.

2. THE INTERRUPT CREDIT (confirms + sharpens M32's note). store_channel_done:
     - token words are written UNCONDITIONALLY and UNGATED:
         w5==0: BAR0[0x40]=(w1-1), [0x44]=(w2-1), [0x48]=(w3-1)  (nibble<<ch*4)
         w5!=0: BAR0[0x4c]=(w4-1)
     - EVENT[0x30] + MSI are gated on msi_enable (.data[0]): if 0, the
       completion bit is parked in pending G[0x634] and NOTHING reaches the
       host. Every delivered interrupt CLEARS msi_enable (one-shot credit).
     - pciep_isr_clrint (our ack: BAR5[0xdc]=2 + doorbell 0x400) = BAR0[0x30]=0
       and msi_enable=1. The ONLY re-arm.
     - int_reduce (G[0x630], from SET_VIC cmd[0x22]) and aic_int_mode (G[0x63c])
       only select which pending-accumulate flavour runs; both flavours still
       need the credit to deliver.

3. "EXACTLY ONE FRAME" (M30/M34) WAS NEVER PROVABLE from our evidence:
     - fake_frame fill is constant 0x11 -> frame N overwriting buf0 with the
       same bytes is invisible;
     - if the card reuses slot 1 every frame (host never advances the ring),
       every channel_done writes token nibble (1-1)=0 -> BAR0[0x40] reads 0
       forever, INCLUDING at stop;
     - we only ever sampled at STOP.
   So "channel_done never fires" (M34) vs "channel_done fires repeatedly into
   slot 1 but EVENT/MSI delivery is dead" are BOTH consistent with every run so
   far. M34's WaitDMAC-park inference stands as the favourite (the credit
   provably cycles during the command phase, and START consumes none), but it
   is not proven.

4. WaitDMAC MECHANICS (libmassmemaccess.so.9 + vpl_dmac.ko):
     - MassMemAccess_WaitDMAC = `while (ioctl(fd, 0xde01)) ;` - an INFINITE
       retry loop; the ioctl only errors on signal_pending.
     - vpl_dmac Ioctl 0xde01: sleeps (prepare_to_wait/schedule, NO timeout) on
       flag[slot] at dev+((slot+130)<<2), set only by the DMAC hw ISR, which
       also wake_up()s dev+(slot+1)*8 and calls ISRTail.
     - VPL_DMAC_ISRTail = ring consumer: advances tail (mod 64), programs the
       next queued descriptor into the hw regs and kicks it (reg[8]|=6); when
       ring empty clears busy (reg2[0]&=~0x400, dev[0x138]=0).
     - So a missing DMAC completion IRQ parks tinyvenc5 forever, exactly M34's
       shape. It also STALLS THE WHOLE DESCRIPTOR RING - nothing else the card
       queues on that engine will run either.
     - pcie_set_outbound (ep.ko, now readable): ELBI[0x74]=?, [0x7c]=0x91FFFFFF
       (window limit -> 0x90000000..0x91FFFFFF = 32 MiB aperture), [0x54]=low32,
       [0x58]=high32, [0xd4]=0xF00000. 32 MiB >> one frame; window size is not
       the stall.

5. tinyvenc5 EncodingGroup::Start (0x10ebc..): spawns the frame thread
   (pthread_create -> 0x15c60 wrapper -> fake_frame_process), then for the LAST
   channel pread()s the 44-byte mailbox and, if cmd[0]==6 (START), polls
   /sys/vpl_pciep/dency at 1 ms up to 100 tries until atoi(dency) == a
   per-group expected value, then pwrite()s a 44-byte ACK. dency_ready lives in
   ep.ko .bss (init 0); its ONLY writers are the sysfs store (yuan_ioctrl's
   debug `echo '0' > dency` - "Disable ENCY") - nothing in vcm sets it. If the
   expected value is 0 the gate is open by default; RE of the expected-value
   field still open. hready/host_ready: G[0], cleared by op2 SET_BUF, read back
   only by hready_show; vcm reads the attr and writes '0' on stop. It is a
   vcm-side state flag, NOT an ep.ko DMA gate - the M34 "hready handshake"
   suspicion is DOWNGRADED (nothing in ep.ko or vpl_dmac consumes it).

DISCRIMINATOR (mz0380-m35-live-token-test.sh): watch BAR0 0x40-0x50 DURING the
stream (phase A, 12 s), then add a 1 Hz ack-kick (BAR5[0xdc]=2, BAR0[0x30]=0,
doorbell 0x400 - each kick re-arms the credit) for 12 s more:
   - edges in phase A            -> channel_done fires; blocker is MSI delivery
   - quiet A, "frame token" in B -> dead credit; fix ack/re-arm in driver
   - both quiet                  -> M34 confirmed; go RE vpl_dmac ISRHead/
                                    IntrEnable + the DMAC IRQ plumbing.
Implementation note: userspace mmap of /sys/.../resource0 fails EINVAL - the
driver's request_mem_region marks the BAR busy and CONFIG_IO_STRICT_DEVMEM
makes busy regions exclusive - so the poller lives in the driver's event-watch
kthread (mz0380-core.c): "live token" change lines (20/s cap) + exact change
counters on watcher stop, and a live-tunable credit_kick_ms param for the
kicks. Watcher armed via "echo start > /proc/mz0380-events".

RESULT (hardware, 2026-08-07): BOTH PHASES QUIET. Token change counters
tok40=44=48=4c=enc50=0 over the whole 24 s stream, 19 credit kicks changed
nothing, event ring recorded 0 edges, frame still lands in buf0 (779 pages),
0 faults, irqs 3/0.
  ==> M34 CONFIRMED, now by direct ungated evidence: store_channel_done is
  NEVER CALLED. Not a credit problem, not an MSI-delivery problem. tinyvenc5
  is parked inside TK_MMA_ProcessOneFrame -> MassMemAccess_WaitDMAC: the
  card's own DMAC completion IRQ never fires, even though the full ~3.19 MiB
  did reach host RAM. The dead/ambiguous branches (delivery, credit, ring
  reuse) are all closed; the only open question is inside vpl_dmac:
  Start/Wait slot protocol, ISRHead slot attribution, IntrEnable, and why a
  transfer whose data demonstrably arrived never reports completion.
  (Side observation: even the SET_VIC/SET_AIC command completions produced no
  EVENT edges while the watcher ran - cmd-done during streamon is evidently
  seen via the STATUS-bit0 poll path, another hint the card-side interrupt
  raise machinery is not doing what M17 assumed.)

================================================================================
M36 THE TRANSFER IS CHUNKED - AND OUR EXTENT EVIDENCE WAS HALF-BLIND.
    (2026-08-07, static RE + instrumentation; hw run pending)
================================================================================

MassMemAccess RE (libmassmemaccess.so.9, scratchpad mma.txt):
- StartDMAC builds a 60-byte descriptor (ctrl@+8 with mode bits x100/x200/x300,
  src@+0x10, dst@+0x14, 2D geometry @+0x18..0x34), MemMgr_CacheFlush, then
  `while (ioctl(fd, 0xde00)) sched_yield()` to claim a ring slot + submit.
  vpl_dmac's 0xde00 handler claims a free slot (dev[slot+0x308] flag),
  programs/queues via StartHead/StartTail; ISRTail advances the 64-deep ring
  and kicks the next queued descriptor.
- ProcessOneFrame's chunked path (req[0x30]!=0): r9 = ceil(req[0x1c]/0x800)
  chunks; per chunk: src/dst += n*0x800, size = 0x800 (remainder on the last),
  then StartDMAC + **WaitDMAC PER CHUNK**, strictly sequential.
- THE ARITHMETIC HIT: the observed stop 0x30a000 = 1556 * 0x800 EXACTLY. So
  on our runs the card completed ~1556 chunk transfers - 1556 DMAC completion
  IRQs fired fine - and then one chunk's completion never arrived. "The DMAC
  IRQ plumbing is broken" is dead; something stalls MID-STREAM, and the same
  place every run (M25/M30/M33/M35 all end at 0x30a000... as far as we could
  SEE).

...as far as we could see: the stop-dump samples ONE byte per 4 KiB page and
tests non-zero. The fake frame is 0x11 fill + ZERO padding - card-written
zeros are invisible, so the true extent may be far past 0x30a000 and the
apparent determinism an artifact of the metric.

INSTRUMENTATION (driver, this session):
- buf_poison param (default 1): all four stream buffers memset to 0xAA after
  SET_BUF, before SET_AIC/START.
- mz0380-extent/%u kthread (mz0380-dma.c): forward-incremental scan per
  buffer every 20 ms for the first dword still equal to the 0xAAAAAAAA
  poison; logs "extent buf[i]=0x... (+0x...)" on change (20/s cap), final
  extents at stream stop. Gives the true write extent INCLUDING zeros, and
  its progress curve (crawl vs hard stall, exact stop offset, per-buffer).
- stop-dump "sampled pages" is now poison-aware ("touched" = != 0xAA).

Test: mz0380-m36-extent-test.sh (also dumps PCIe MPS/MRRS/AER for the card
and its upstream bridge - a payload/flow-control anomaly is a live suspect
for a deterministic mid-stream outbound stall). Readouts in the script
header: constant growth = crawling (compute B/s); freeze at X = hard stall
at true offset X (compare 0x2F7600 frame, 0x800 grid, run-to-run repeat);
full 4 MiB = card wrote everything and the blocker moves to the SECOND
Start/Wait pair in ProcessOneFrame (metadata DMA) or the loop exits;
buf1-3 nonzero = ring does advance.

RESULT (hardware, 2026-08-07): THE WRITES HAVE A HOLE. One extent line at
t=+0.45 s after START: buf[0]=0x6d384 - then frozen for the remaining 27 s -
while the page sampling still saw touches out to 0x30a000. So the write
pattern is: contiguous data 0..0x6d383, at least one still-poisoned dword AT
0x6d384 (= 233 x 1920-byte lines + 4, mid-chunk: chunk 218 offset 0x384),
then more data beyond, ending around 0x30a000. A chunk was partially
written / a write went missing MID-STREAM - with 0 IOMMU faults, 0 AER
errors (CESta/UESta clean on card AND bridge), MPS 128 matched both ends,
MRRS 512, link 2.5GT/s x1. buf1-3 untouched (pure poison). Everything up to
the first burst (~447 KiB in <450 ms) moved at full speed.

The single-frozen-extent metric cannot see past the first hole, so M37 adds
a full hole map at stream stop (mz0380_extent_hole_map): every data<->poison
transition per buffer (first 8 holes with offset+length, total data bytes,
last-data offset, interior hole count). This distinguishes:
  - ONE small hole            -> a lost write burst (silent PCIe/AXI drop);
  - periodic stride holes     -> the transfer is 2D with dst stride > row,
                                 i.e. STRUCTURAL, and the "hole" is by design
                                 (then the stall story changes completely);
  - many scattered holes      -> systematic write loss along the stream.
Re-run mz0380-m36-extent-test.sh (now prints the hole map).

HOLE MAP RESULT (hardware, 2026-08-07): EXACTLY ONE 4-byte hole, at 0x6d384,
identical offset in both runs. Total data 0x30a5bc bytes, last data at
0x30a5bc, i.e. the stream is CONTIGUOUS 0..0x30a5bb except that single dword,
and stops mid-chunk (0x30a5c0 = 1556*0x800 + 0x5c0). Notable: 0x30a5c0 ==
1920*1107*1.5 exactly. Still 0 faults, 0 AER, buf1-3 pure poison.

A deterministic SINGLE-DWORD hole at the same offset is at least as likely
to be a COLLISION - the frame data itself containing 0xAAAAAAAA at 0x6d384
(the fill is not perfectly uniform; the head already shows a 0x10 byte) - as
a genuinely lost write. New param poison_byte (def 0xaa): re-run once with
poison_byte=0x55; a real un-written hole stays at 0x6d384, a collision moves
or disappears (and then the card's writes are FULLY contiguous and simply
stop at 0x30a5c0, mid-chunk 1556, deterministically).

POISON-BYTE RESULT (hardware, 2026-08-07): COLLISION CONFIRMED - THERE ARE
NO HOLES. With poison 0x55 the 0x6d384 hole vanished and 31 different
"holes" appeared (@0x130cc0 0xc, @0x130d98 0x10, @0x1747f4 0x10, ... - all
clustered 1.2-1.5 MiB into the frame, i.e. mid-picture: frame data that
happens to contain 0x55555555 dwords, likely an OSD/pattern region).
data=0x30a49c vs 0x30a5bc with 0xaa - the difference is exactly the
respective collisions. CONCLUSIONS, now solid:
  1. The card writes ONE FULLY CONTIGUOUS burst 0..0x30a5bc, fast (<450 ms
     from START), deterministically, zero loss, zero faults, zero AER.
  2. 0x30a5c0 = 1557 chunks of 0x800 with a partial last chunk of 0x5c0
     = 1920 x 1107 x 1.5 EXACTLY: the transfer ENDS AT ITS INTENDED SIZE.
     THE TRANSFER COMPLETES. The "parked DMAC / missing completion IRQ"
     theory (M34) is DEAD - the engine finished every chunk including the
     partial last one.
  3. Therefore the park is AFTER the data DMA: between ProcessOneFrame's
     return and the channel_done pwrite - OR there is no park at all and
     the card is writing IDENTICAL frames to buf0 continuously (invisible:
     same bytes, extent frozen after frame 1, tokens silent). M34's reading
     of fake_frame_process supports the latter: the 0x168c8 channel_done
     pwrite sits AFTER the frame LOOP (counter vs limit at [0x16bfc]), not
     inside it - nosg mode may simply never exit the loop and so never
     signal, BY DESIGN of the fake path.

M38 DISCRIMINATOR (mz0380-m38-repoison-test.sh): "echo repoison >
/proc/mz0380-events" re-fills the buffers with poison MID-STREAM (driver
cmd + mz0380_extent_repoison). Three repoisons at t=8/14/20 s:
  - extent regrows after each  -> the card streams continuously; DMA path
    100% healthy end-to-end; blocker = tinyvenc5 frame-loop/completion
    semantics (RE the loop bound [0x16bfc], the alternate exits 0x1686c/
    0x16878 -> 0x170d8 channel_done, and how the REAL encode_handler path
    signals per-frame instead). Repoison cadence also measures fps.
  - nothing regrows            -> exactly one frame ever; the park is real,
    between the last chunk's WaitDMAC return and channel_done.

RESULT (hardware, 2026-08-07): NOTHING REGROWS. One frame at t=+0.45 s, then
three repoisons all stayed pure poison; final extents 0, stop dump 100%
poison (0/1024 pages). EXACTLY ONE FRAME PER STREAMON, then the card never
writes again. Combined with M37 (that one frame is complete, contiguous,
full intended size): the DMA machinery works perfectly ONCE, and whatever
should produce frame 2 never runs.

The frame pacing in fake_frame_process is TIME-based (64-bit clock deltas vs
a period + usleep at 0x16a94-0x16acc, loop-back to 0x161bc) - not gated on
any hardware tick - so a live loop would free-run. Most plausible: the
thread ERRORS OUT/EXITS after frame 1 (TK_MMA_Release + return at
0x16a80-0x16a90), or blocks in one of: the mailbox pread at 0x16cc8 (it
polls the 44-byte command mailbox mid-loop - stale opcode semantics?), the
per-bitstream 4 KiB info-block ProcessOneFrame at 0x16838 (iteration 0
branches to 0x170f0 instead - big frame vs info block), or a flag wait on
[r3+0xe98]/[r3+0xe9c] (writers unknown). Deep RE of the whole loop underway
(subagent); M39 (mz0380-m39-respawn-test.sh) additionally answers whether a
SECOND streamon on the same module load produces its own one frame (thread
dies per spawn - per-frame path fine) or nothing (card-side engine/ring
wedged after the first run).

================================================================================
M43 M42 RAN: NO LOCK. And nothing in that run was actually verified.
    (2026-08-07)
================================================================================

Hardware result of M42: "EDID loaded (256 bytes)" + "HPD asserted" + receiver
brought up, then five detect attempts all -ENOLCK, streamon refused, 0 IRQs.

The important caveat, which invalidates the optimistic reading: **a successful
mailbox return proves nothing about I2C.** The card firmware forces the result
byte to 0x00 on a NAK (M13), so an EDID write that never reached a chip is
indistinguishable at the command layer from one that did. We have no evidence
the EDID is in the EEPROM, that op 0x1f is even implemented in our fw 1.11
ep.ko, or that op 0x1f drives the bus the DDC EEPROM sits on - RE_FINDINGS
M11/M13 already flag that exact bus ambiguity (our yuan_ioctrl RE put 0x1e/
0x1f on BUS1, the Windows-side RE puts them on BUS0).

DIAGNOSTIC ADDED (mz0380_mst3367_diag, /proc/mz0380-hdmi; script
mz0380-m43-sink-diag.sh). Reads every link back instead of trusting writes:
  - GPIO read-back (op 0x14) for hpd/rx_enable/rx_strap/rx_reset;
  - EDID read-back from DDC 0xA0. THIS IS THE SHARP PROBE: bytes 1..6 of a
    valid EDID are 0xff, which the firmware cannot manufacture on failure
    (it forces 0x00). "ff" = the blob is really there; "00 00 .." = the
    writes went nowhere and the op/bus is wrong;
  - MST3367 BANK0 register row (0x55 detect, 0x5f, htotal, vperiod, 0xb0,
    0xb7 HPD, 0xb8, 0x51). An all-zero row means the receiver is not
    answering at all, in which case EDID/HPD are moot and the fault is
    reset/power (pin9).
This splits "sink is fake" from "sink is real but the source is silent" from
"receiver is dead", which the M42 run could not.

RESULT (hardware, 2026-08-07) - two clean facts:
  1. THE RECEIVER IS ALIVE AND OUR I2C WRITES LAND. BANK0 read-back:
     55=03 5f=40 6a=00 6b=00 59=1f 5a=ff b0=20 b7=00 b8=00 51=89.
     b0=0x20, b7=0x00 and 51=0x89 are EXACTLY the values init_regs wrote, read
     back through a separate command - so op 0x1a/0x1b to dev 0x9c works
     end-to-end and the MST3367 is powered, out of reset and configured.
     GPIO read-back: hpd=1, rx_enable=1, rx_strap=1, rx_reset=1 (released).
     0x55=0x03, so (0x55 & 0x3c)==0 -> NO SIGNAL LOCKED. 59/5a = 0x1fff-ish
     is the idle/no-signal counter value.
  2. THE EDID NEVER REACHED ANYTHING: read-back from 0xA0 was
     00 00 00 00 00 00 00 00. Since the firmware forces 0x00 on a NAK and
     bytes 1..6 of any valid EDID are 0xff, this is proof the blob is not
     stored - the M42 "EDID loaded" message was meaningless.

CAUSE FOUND (docs/re-2026-07-05/HD60-PRO-LINUX-DRIVER.md sect 3d item 3 +
windows-findings.md sect 1): **multi-byte/EDID uses the COMBO opcode 0x20
(i2c_combo_cmd_x), not 0x1f.** 0x1f is read_s/write_s, the opcode whose bus
assignment our two RE sources disagree about. The correct frame is
  cmd[4]=dev8  cmd[5]=rw  cmd[6..7]=len  cmd[8..]=payload
i.e. PARAM1 = dev8 | (rw<<8) | (len<<16). Our M42 code also mispacked the
second byte as a block number where the rw flag belongs.

FIX (this session): EDID write switched to op 0x20 with that layout, and the
payload now leads with the EEPROM byte offset (an I2C EEPROM write carries
its target address as the first data byte), so each chunk is a 33-byte
transfer; 5 ms between chunks for the page-write cycle.

ALSO ADDED: an I2C presence scan in /proc/mz0380-hdmi over the addresses the
Windows driver knows (0x9c, 0xa0, 0xa2, 0x88, 0x98, 0x60, 0x90, 0x94). A
non-zero byte proves a device answered. This settles whether anything lives
at the EDID address on this bus at all - if 0xa0 stays silent while 0x9c
answers, the EEPROM is either on the other bus or gated behind the receiver's
DDC, and the EDID would have to go into the MST3367's internal EDID RAM
instead.

SECOND RUN (op 0x20 combo, hardware): EDID read-back STILL all zeros, and the
presence scan (reg 0x00 only) gave:
    9c:00 a0:00 a2:00 88:00 98:54 60:00 90:00 94:00
  - 0x98 ANSWERED with 0x54 - a real device (the firmware cannot fabricate a
    non-zero byte). Per M10's address list 0x98 is one of the MST3367's aux
    banks.
  - 0x9c reading 0x00 is NOT evidence of absence: reg 0x00 on the main bank
    is the bank-select, and bank 0 legitimately reads back 0. The BANK0 row
    already proved 0x9c is alive.
  - 0xa0 silent again => there is no DDC EEPROM at that address on this bus.

WHERE THE EDID REALLY LIVES - two candidates from the RE docs, which disagree
with each other and now have to be settled empirically:
  (a) HD60-PRO-LINUX-DRIVER.md sect 5c step 3: "Driver writes EDID into the
      MST3367" - i.e. internal EDID RAM behind one of the aux slaves.
  (b) same doc sect 5b: the Jump2LDROM/Run APROM/VerifyLDROM strings are a
      "Nuvoton MCU ISP flash dance for the small EDID MCU (the EEPROM the
      HDMI *source* reads)" - i.e. a SEPARATE little MCU holds the EDID, and
      our own earlier RE noted an MCU passthrough slave at 0xaa.
If (b) is right, no amount of writing to the MST3367 will ever make the source
see an EDID, and the blob has to be pushed to that MCU instead.

PROBE WIDENED (this session) to decide it: the scan now samples regs
00/01/02/7f per address (one register is not enough - reg 0 is bank-select on
the main bank) across 0x9c/0xa0/0xa2/0xa4/0xa6/0xa8/0xaa/0x88/0x98/0x60/
0x90/0x94/0x74/0xb0, flagging any address with a non-zero byte as present;
and a second pass dumps the first 8 bytes from every plausible window looking
for the EDID signature 00 ff ff ff ff ff ff 00. Whichever window shows that
signature is where the card's EDID is stored - and therefore where ours must
be written.

WIDENED-SCAN RESULT (hardware): THE BUS HOLDS EXACTLY TWO DEVICES.
    9c: 00 69 d0 00   <- present (the MST3367 main slave)
    98: 54 49 12 0c   <- present
    a0/a2/a4/a6/a8/aa/88/60/90/94/74/b0: all 00 = nobody home
So claim (b) is DEAD: there is no Nuvoton EDID MCU at 0xaa and no EEPROM at
0xa0/0xa2 on this bus. Nothing outside the receiver can hold the EDID, so
claim (a) stands by elimination - the EDID must go into the MST3367 itself.
The signature hunt found no EDID at 0x98's register head either
(54 49 12 16 1c 60 00 00), so it is not a flat EDID window.

NEXT PROBE (built, unrun): the GPL reference states the chip has FOUR banks
(BANK0..BANK3, "256-byte register shadow per bank") - and we have only ever
touched banks 0/1/2. Bank 3 is completely unexplored and is the natural home
for EDID RAM. /proc/mz0380-hdmi now sweeps banks 0..3 on BOTH slaves (0x9c
and 0x98) and dumps the first 16 bytes of each, hunting the signature. If a
bank shows 00 ff ff ff ff ff ff 00, that is the EDID store and the writer
just has to be retargeted at it (bank-select, then sequential registers).

BANK SWEEP RESULT (hardware):
    9c bank0: 00 69 d0 48 00 10 08 20 80 80 80 80 80 40 08
    9c bank1: 01 80 00 00 00 00 00 fd 00 00 00 00 11 00 00 02
    9c bank2: 02 61 f5 00 00 00 00 04 00 08 00 00 00 15 00 00
    9c bank3: 03 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    98 bank0..3: identical rows (54 49 12 16 1c 60 00 00 00 ff ff ff 00 00 6c 08)
Readings:
  - Banking on 0x9c is REAL and our writes are live: reg 0x00 echoes the
    selected bank on every bank, and bank2's 0x01=0x61 / 0x02=0xf5 are exactly
    what init_regs wrote.
  - 0x98 ignores bank-select entirely (four identical rows), so it is a
    separate device with a flat register map, not a second bank window - and
    it holds no EDID.
  - **BANK3 IS EMPTY**: every byte 0x01..0x0f reads 0 while banks 0-2 are full
    of live config. An all-zero 256-byte shadow is exactly what an EDID RAM
    that has never been loaded looks like - and "the source sees an invalid
    (all-zero) EDID" would explain the no-signal state perfectly.
  - No EDID signature anywhere, which is consistent with that: there is
    nothing to find because nothing was ever written.

================================================================================
M45 THE SOURCE IS REACTING TO US. (2026-08-07)
================================================================================

M44 RESULT (hardware): BANK3 IS NOT WRITABLE - 0/7 pattern bytes held their
value. Bank 3 is an unimplemented shadow, not the EDID store. Lead closed.

THE OBSERVATION THAT REFRAMES EVERYTHING (user report): running the bring-up
script visibly STOPS THE LIVE PREVIEW OF THE CONNECTED CAMERA. That is
exactly what a camera does when it detects an HDMI sink and switches its
output over to HDMI. So:
  - a real source IS attached to the card's input (worth stating plainly -
    every "no signal" result until now was ambiguous about this);
  - the source RESPONDS to what we do, i.e. our HPD toggle is reaching it
    physically. The sink side is not dead.
Which makes the open question sharper: does the source then TRANSMIT, and if
it does, why does the receiver never report lock?

Supporting detail: reg 0x55 reads 0x03 consistently - bits 0 and 1, which are
OUTSIDE the 0x3c lock mask the GPL driver gates on, and which it never uses.
The natural reading is 5V/cable presence and/or clock detect. If those bits
track the cable, the receiver is genuinely wired to the connector.

M45 TOOL (built): "echo watch [secs] > /proc/mz0380-hdmi" samples the whole
detect block (0x55, 0x5f, hperiod, vperiod, htotal) every 250 ms and logs
every change, so a plug/unplug or camera mode-cycle can be correlated with
what the chip sees. Script mz0380-m45-watch-test.sh drives it.
Decision table:
  - 0x55 changes on unplug/replug -> receiver is wired to the connector and
    sees the source; "no-lock + idle counters" then means the source is
    attached but NOT transmitting, i.e. it is refusing for lack of a valid
    EDID -> finding the EDID store remains the blocker.
  - timing counters go non-zero (idle is vper=1fff, htot=0000) -> the
    receiver IS seeing TMDS: the source transmits and the fault is the
    receiver's per-mode config (M10 step 2 values, never recovered), NOT the
    EDID.
  - nothing ever changes -> 0x55's low bits are static and the receiver may
    not be wired to this connector at all; suspect input select (op41 input
    code) or a different physical input.

M45 RESULT (hardware): NOTHING MOVED. One line at t=0
(55=03 no-lock, 5f=40, hper=0000, vper=1fff, htot=0000) and then no change at
all for 40 s across unplug/replug. So 0x55's low bits do NOT track the cable
and the detect block is frozen at its idle values.

CAUSE (user's own observation - "the script does not trigger the hdmi node"):
that watch ran against a receiver whose INPUT HAD NEVER BEEN SELECTED. The
M42 path reached SET_VIC only through streamon; mz0380_mst3367_bringup went
straight to reset/init/EDID/HPD and never told the card which front-end to
route. The Windows order is AUTO.INPUT (input select) -> EDID -> HPD ->
detect, and RE_FINDINGS M13 already named input-select as the prime suspect
for "the receiver domain is gated until Windows does something first". A
receiver that answers I2C perfectly, has a source physically reacting to its
HPD, and yet shows a detect block frozen at idle is exactly what an unrouted
front-end looks like.

FIX (M46, this session): mz0380_mst3367_bringup now sends
SET_VIC(input=HDMI, 1920x1080@60) as its FIRST action, before the receiver
reset. The watch script also forces the bring-up before watching (it was
lazy, running on first detect) and prints the sequence so "input select"
can be confirmed to appear first.

================================================================================
M47 THE EDID IS THE LAST UNVERIFIED LINK - AND op 0x20 IS FIRE-AND-FORGET.
    (2026-08-07)
================================================================================

M46 RESULT (hardware): input select now runs first
("input select: SET_VIC(input=HDMI 1920x1080@60) ret=0"), and the detect block
STILL never moves (55=03, 5f=40, hper=0000, vper=1fff, htot=0000 for 40 s,
unplug/replug included). So input-select was not the gate either.

But the same log finally shows what happens to the EDID:
    "EDID combo write failed at offset 0 (-110)"
-110 is OUR timeout, and every subsequent command (the whole receiver init)
worked - the mailbox was not wedged. That is the START_STREAMING signature
from M22: the card executes the opcode but posts NO completion, so waiting
for one always fails. The earlier op 0x1f attempt returned 0 instantly and
stored nothing, which is what an unimplemented opcode looks like.

So the EDID has never once been delivered, by any route, and that is exactly
the link no read-back can check (nothing answers at the DDC address on this
bus). THE SOURCE IS THE ORACLE: the user reports the camera switches to HDMI
mode when it detects a real sink, i.e. when it can read a valid EDID.

FIX/HARNESS (M47): EDID chunks are now sent fire-and-forget by default
(edid_timeout_ms=0), the opcode is a live module param (edid_opcode, def
0x20), and "echo edid > /proc/mz0380-hdmi" re-pushes the blob and re-pulses
HPD without a reload (a source only re-reads on a hotplug edge).
mz0380-m47-edid-sweep.sh walks 0x20/0x1f/0x1e/0x1b/0x1d and pauses after each
so the camera's screen can be watched.
  - camera flips to HDMI-out on some variant -> that is the EDID opcode.
  - no variant works -> no host opcode in fw 1.11 can store an EDID; next is
    a live Windows I2C trace of UpdateEDID (which also yields the per-mode
    config values from M10 step 2), or checking whether the card's own
    userspace serves the EDID from flash and needs a different trigger.

M47 RESULT (hardware): ALL FIVE OPCODES NEGATIVE. 0x20/0x1f/0x1e/0x1b/0x1d
each pushed the full 256 bytes fire-and-forget with an HPD re-pulse; the
detect block never moved off 55=03 / hper=0000 / vper=1fff / htot=0000 and
the camera never switched to HDMI output. The user also notes the source
reacted only ONCE, on an early run, and never again - which is as consistent
with a camera idle-timeout as with our hotplug edge, so that single reaction
must not be treated as evidence that HPD works.

WHERE THIS LEAVES THE SINK CHAIN - what is proven vs still assumed:
  PROVEN: receiver powered, out of reset, configured, answering I2C with
    read-back of our own values; input select runs first; GPIO pins read back
    at their intended levels; only 0x9c and 0x98 exist on the bus.
  NEVER VERIFIED: (1) that an EDID is stored anywhere, (2) that our HPD write
    physically reaches the connector, (3) the receiver's per-mode config
    values (M10 step 2 - the one part of the Windows bring-up we could not
    recover, ~175 register writes computed at runtime).
Any one of those three can produce exactly what we see.

M48 (built): isolate HPD as a SINGLE variable - "echo hpd [count] [gap_ms] >
/proc/mz0380-hdmi" pulses the hotplug line N times and does nothing else, so
the camera can be watched for a reproducible reaction.
  - reacts every pulse -> we own the hotplug line; the EDID must be served by
    the factory-programmed EDID MCU (not by us), and the remaining fault is
    the receiver config (M10 step 2).
  - never reacts -> HPD is not reaching the connector despite pin1 reading
    back 1; sweep the other GPIO pins one at a time against the camera.
  - reacts once only -> the source latches sink-presence; retry with a longer
    HPD-low period.

HONEST STATUS: three separate unknowns remain and each needs the same thing to
resolve efficiently - a LIVE WINDOWS I2C/mailbox TRACE of the working driver
doing UpdateEDID + receiver config. That single capture would deliver the EDID
mechanism, the HPD sequence and the ~175 per-mode register values at once.
Continuing to guess opcode/bus/bank combinations from static RE has now
produced five consecutive negatives and is the wrong tool for what is left.

================================================================================
M48 RESULT: **HPD WORKS.** THE EDID IS THE ONLY REMAINING FAULT. (2026-08-07)
================================================================================

Five clean HPD pulses, nothing else changed. User observation, and it is
decisive: the camera "reacted to the signal but did not stay in HDMI output
mode" - it responded to the hotplug edge and then fell back to its LCD.

That is the textbook sequence for: source sees HPD assert -> attempts to read
the EDID over DDC -> gets nothing -> concludes there is no valid sink ->
reverts. So:
  - OUR HPD PHYSICALLY REACHES THE CONNECTOR. GPIO pin1 (plus BANK0 0xb7) is
    the real hotplug line and the source honours it. Unknown (2) from M47 is
    CLOSED, and the earlier single reaction was not a coincidence after all -
    it was the same thing, just not repeated because the source only re-probes
    on an edge.
  - The receiver never locking is therefore NOT explained by the per-mode
    config values (unknown 3): the source is not transmitting at all, so
    there is nothing for the receiver to be misconfigured about yet.
  - EVERYTHING now hangs on the EDID: nothing we have done has ever put one
    where the source can read it.
Three unknowns collapsed to one. (Only reacting on some pulses is expected -
a source that has just failed an EDID read may back off before re-probing.)

M49 (built): find the EDID RAM BY WRITABILITY - the last systematic host-side
option before a live trace. Only 0x9c/0x98 exist on the bus, so the EDID must
come out of the receiver's own RAM. A config register has reserved/hardwired
bits and will not return an arbitrary byte; a RAM cell returns exactly what
was written. "echo wscan > /proc/mz0380-hdmi" writes 0x5a then 0xa5 to every
register 0x01-0x80 of every bank, restoring each original immediately, and
prints a per-bank map ('#' = held both values).
  - a long contiguous run of '#' = the EDID window -> retarget the writer.
  - only scattered '#' = ordinary r/w config regs; the EDID RAM is gated
    behind an unknown enable bit -> live Windows trace.
Script: mz0380-m49-wscan.sh. Reg 0x00 is never written (bank select).

M49 FIRST PASS (hardware, regs 0x01-0x80 only):
    bank0: 95 writable, long runs (expected - this is the main config bank)
    bank1: 29 writable, scattered
    bank2: 32 writable, one run at the start then scattered
    bank3: 48 writable, '.' across the whole low half and then a LONG
           CONTIGUOUS RUN that was still going when the scan hit its 0x80
           ceiling
Bank3 is the interesting one: it holds no config at all (every byte reads 0,
M43) yet the top of its register file accepts arbitrary values. That also
explains why the M44 ramtest failed - it probed 0x10..0x17, which is inside
bank3's dead low half.

SCAN WIDENED (M49b): the sweep now covers the FULL file 0x01..0xff and
reports contiguous runs explicitly ("bank3 run 0x60-0xff (160 bytes)") plus
the longest run per bank, flagging anything >= 128 bytes as an EDID-sized
window. If bank3's run continues to 0xff it is a 128-byte block - exactly one
EDID block - and that is where the blob goes.

M49b RESULT (hardware, full 0x01-0xff sweep): NO EDID-SIZED WINDOW EXISTS.
    bank0: 156 writable, longest run 33 bytes @0x6e
           (runs: 0x02-0x13, 0x17-0x24, 0x26-0x31, 0x36-0x3f, 0x41-0x4a,
            0x60-0x67, 0x6e-0x8e, 0x90-0xac, 0xb0-0xb8)
    bank1:  71 writable, longest run 19 bytes @0x81 (0x81-0x93, 0xe0-0xef)
    bank2:  34 writable, longest run  9 bytes @0x01
    bank3:  48 writable, longest run 30 bytes @0x40 (0x40-0x5d, 0x62-0x6b)
The longest run anywhere is 33 bytes - a quarter of one EDID block. The
writable registers are ordinary r/w config scattered through the file, not a
RAM window. bank3's earlier "long run at the ceiling" was the 0x40-0x5d run
ending at the old 0x80 cut-off, not a continuation.

=> THE EDID RAM IS NOT REACHABLE AS A FLAT REGISTER WINDOW ON THIS PATH. It is
either gated behind an enable bit we do not know, addressed through an
indirect port (address register + auto-incrementing data register - which
would appear in this scan as just two writable registers, indistinguishable
from config), or served by hardware we cannot see from the host bus.

================================================================================
STRATEGIC CONCLUSION (2026-08-07): STATIC RE IS EXHAUSTED FOR THE SINK SIDE.
================================================================================

What is now PROVEN on hardware, and should not be re-litigated:
  1. The whole host-side DMA/streaming stack works: addressing, 4GiB-aligned
     IOVA, outbound aperture, per-chunk completion, credits, MSI, event ring.
     One complete 1920x1107x1.5 frame lands within 450 ms of every START,
     contiguous, zero loss, zero IOMMU faults, zero AER errors (M35-M39).
  2. The fake-frame path's stall is card-internal and unreachable from the
     host (M41): a missing DMAC completion IRQ parks tinyvenc5 forever;
     VPL_DMAC_Open clears the busy/head/tail state, which is why every fresh
     spawn yields exactly one more frame.
  3. The MST3367 is powered, out of reset, configured and answering I2C with
     our own written values read back (M43). Only 0x9c and 0x98 exist on the
     bus - no EEPROM, no EDID MCU.
  4. Input select (SET_VIC) runs before receiver bring-up (M46) - not the gate.
  5. **HPD WORKS** (M48): the source reacts to our hotplug edge and then
     reverts, i.e. it sees the sink appear, fails to read an EDID, and gives
     up. This is the single most useful datum of the session.
  6. The EDID has NEVER been delivered by any route: 5 opcodes
     (0x20/0x1f/0x1e/0x1b/0x1d) x fire-and-forget x HPD re-pulse, all
     negative (M47); no EEPROM on the bus (M43); no EDID-sized RAM window in
     any bank (M49b).

The ONE remaining blocker is therefore: HOW DOES THE HOST GET AN EDID INTO
THIS CARD? Everything else is either working or provably not the current
fault.

THE ANSWER REQUIRES A LIVE TRACE, not more static RE. A capture of the
Windows driver performing UpdateEDID would show, in one go: the exact opcode,
the exact frame layout, the target address, and any enable/unlock sequence
that precedes it. The same capture also yields the ~175 per-mode receiver
config writes (M10 step 2) that we will need immediately afterwards. Static
analysis has now produced eight consecutive negatives on this question and has
no remaining unexplored surface: the opcode space is swept, the bus is
enumerated, every bank is dumped and writability-mapped.

HOW TO GET THE TRACE (in preference order):
  a. Boot Windows with the Elgato driver, run its capture app so a source
     locks, and capture the driver's mailbox traffic - a DbgView/WPP capture
     of e60MZ0380.X64.SYS (it logs "[UPDATE.EDID]", "send EDID data",
     "[HOTPLUG %d]"), or an ETW/kernel trace of its BAR0 writes.
  b. Physically probe the card's internal I2C (GPIO12/13) with a logic
     analyser while Windows brings the card up - this reads the wire directly
     and needs no software cooperation.
  c. If neither is possible: accept the sink side as blocked and pivot the
     project to the SDI/other input paths, or to consuming the one raw frame
     the fake path does deliver.

M44 TEST (built, unrun): "echo ramtest > /proc/mz0380-hdmi" writes a walking
pattern (a5 5a 00 ff 12 34 56 78) into BANK3 regs 0x10..0x17, reads it back,
then restores zeros. Reg 0x00 is never touched (bank select on every bank).
  - registers hold the pattern -> BANK3 is writable RAM, i.e. the EDID store:
    retarget the EDID writer at it (bank 3 + sequential register writes,
    skipping/working around reg 0x00) and re-run M42.
  - registers read back 0 -> bank3 is not the EDID store; the RAM is behind an
    indirect address/data port, and the next step is a live Windows I2C trace
    of UpdateEDID (which also yields the per-mode config values, M10 step 2).

================================================================================
M42 DRIVER: SINK-SIDE BRING-UP IMPLEMENTED (EDID + HPD + fps). (2026-08-07)
================================================================================

Acting on M41 (fake path is a host-side dead end), the driver now implements
the real-signal path's missing prerequisites. All compile-verified; hardware
run pending a live HDMI source (mz0380-m42-real-signal-test.sh).

1. EDID LOAD - the gap that made every previous nosg=0 attempt hopeless. An
   HDMI source keeps its transmitter OFF until it can read an EDID from the
   sink, so "no signal" was guaranteed regardless of receiver config. The
   256-byte HD60 Pro blob recovered in M10 (mz0380-edid-hd60pro.txt) is now
   compiled in as mz0380-edid.h (generated with header+checksum validation)
   and written to the DDC EEPROM (I2C 8-bit 0xA0) with the bulk opcode 0x1f,
   8 x 32-byte chunks, PARAM1 = (len<<16)|(block<<8)|dev8, bytes packed LE.
2. HPD - asserted only AFTER the EDID is in place, and deliberately toggled
   low across the load so an already-attached source re-reads it instead of
   keeping a stale/absent copy. Two levers, both driven: board GPIO pin1 and
   the receiver's own enable (BANK0 0xb7, 0x02 = off / 0x00 = on).
3. SET_VIC fps ([5]) was ALWAYS 0 - the card patched it to 60 with its own
   "Error!!! vic_fps cannot be 0" message. Harmless while the fake generator
   paced itself; the real capture path derives its cadence from it. Now sent
   from the detected timing (frame rate, so 1080i60 -> 30, matching the
   card's own convention).
4. streamon re-detects the live timing when stream_nosg=0 and refuses to
   start without lock, so SET_VIC can never arm the encoder for a stale mode.
5. enc_stat handshake (M40) acked at stream start and after every delivered
   frame - required for frame 2 onwards on the real path.

STILL UNKNOWN for the real path: the per-mode MST3367 register VALUES for the
full config sequence (M10 step 2) are not in our dump; hdcapm's init_setup is
what we run, which is enough for lock+detect but may not be enough for clean
BT.1120 output routing into the SSM ring. If the receiver locks but no frames
flow, that (plus vpl_vic config and op 0x50) is the next target - and it also
finally answers whether the DMAC completion IRQ is dead in general or only in
fake_frame_process.

================================================================================
M41 CASE CLOSED (static): THE CARD'S DMAC COMPLETION IRQ NEVER FIRES.
    (2026-08-07. Second subagent + own verification of vpl_dmac state handling)
================================================================================

The submit/wait accounting that M40 left open is now decided, and it is NOT a
ring-slot leak: fake_frame_process issues EXACTLY ONE submit per iteration
(0x17590 and 0x1724c are mutually exclusive variants - 0x175b0 jumps past
0x1723c) and exactly one wait (0x17298 or 0x1839c, keyed on slot[0x53]).
MassMemAccess_StartOneFrame calls StartDMAC once (mma 0x1a9c, every branch
funnels there). Submits == waits, 1:1.

THE MECHANISM (vpl_dmac.ko, verified directly, not just from the report):
  - submit ioctl 0xde00 claims slot dev[0x140] (StartHead 0xb54), sets
    filp[0xc]=n, calls StartTail 0xb5c which kicks the HARDWARE DIRECTLY but
    ONLY IF dev[0x138]==0 (0xb7c/0xb88), setting dev[0x138]=1 at 0xba4.
  - wait ioctl 0xde01 sleeps TASK_INTERRUPTIBLE (prepare_to_wait state=1 at
    0x6b4, schedule at 0x6a0) until dev[0x208 + filp[0xc]*4] == 1, then frees
    the slot (0x6f8) and sets filp[0xc]=-1.
  - ONLY the DMAC hardware ISR sets that done flag (ISR 0x30: done[tail]=1,
    __wake_up 0x44) and ONLY ISRTail (0xe5c) advances the tail (0xe70), starts
    the next queued descriptor, and clears busy dev[0x138]=0 (0xf90).
  - MassMemAccess_WaitDMAC (mma 0x1ee4) retries the wait ioctl FOREVER
    (0x1efc), so even a signal cannot break out.
So: the first transfer is kicked SYNCHRONOUSLY from the submit path. The
preview frame landing therefore proves the DMA ENGINE works and says NOTHING
about the completion interrupt. If that IRQ never arrives: done[n] is never
set (thread sleeps forever at 0x6a0), dev[0x138] stays 1 (every later submit
silently enqueues and is never kicked), tail never advances. Symptom:
EXACTLY ONE DMA, ever, then total silence, with the frame itself complete.
That is precisely M35-M38.

AND IT EXPLAINS M39, which nothing else did: VPL_DMAC_Open (0xd28) clears
busy(0x138), tail(0x13c) and head(0x140), and the driver's Open() re-runs
that full init (plus request_irq/IntrClear/IntrEnable) when
module_refcount()==0 - i.e. once the previous tinyvenc5 has been killed at
streamoff. So each new streamon gets a clean DMAC and produces exactly one
more frame. Frame-per-spawn, forever. Every observation from M24 on now fits
one model with no leftovers.

CONSEQUENCE FOR THE HOST: dev[0x138] and done[n] are written ONLY by the
card's own ISR. No mailbox opcode, BAR write, doorbell, credit re-arm or
buffer-setup change can set them. **From the host side the stream_nosg fake
path is a dead end** - once parked, only a re-spawn recovers it.
(Runner-up hypothesis, if slot[0x53]==0 so the wait comes after the encode:
the thread is instead in vma_h4ee's 0xdb01 wait, schedule() at file 0x660,
TASK_UNINTERRUPTIBLE with no timeout. Ranked second: that encoder is
memory-to-memory with depends= empty, no capture clock needed. Distinguishing
symptom if a card console ever becomes available: thread in D state = encoder;
S state = DMAC wait; R at 100% = the sched_yield submit spin at mma 0x1df0.)

WHY THE IRQ MIGHT BE MISSING is now the only open question, and it is
card-internal: the DMAC's completion interrupt line/mask (fLib_SetIntTrig at
vpl_dmac 0x324, IntrEnable 0xc9c setting bit1 of reg[8]) is set up in Open(),
which our runs do reach. A plausible remaining reading is that the retail
firmware's real path never depends on this wait completing under our exact
conditions, i.e. that the fake path is simply not exercised on shipping units.

WHAT THIS DOES NOT BLOCK: the real-signal path uses the SAME vpl_dmac pair,
but its frame cadence is driven by vpl_vic capture interrupts inside
encode_handler (~0x12b00, TK_MMA calls at 0x151a0/0x14220) rather than by
this software loop. Driving a real HDMI signal in is now both the product
goal and the only way to learn whether the DMAC IRQ is genuinely dead or
whether fake_frame_process alone is parked.

================================================================================
M40 FULL fake_frame_process RE (subagent report, 2026-08-07). Corrected model.
================================================================================

Complete loop decode (report highlights; addresses in tinyvenc5.txt):
- ONE exit only (0x16a48-0x16a90: close channel_done fd, TK_MMA_Release x2,
  pop), guarded solely by this->0x71 which is cleared by EncodingGroup::Stop
  or two setup-failure paths. NO error path exits the loop; every error print
  loops back. The thread cannot "die quietly" mid-loop.
- Iteration order: pacing (time-based) -> OSD/clock render -> SetOptions ->
  [if g_enable_dma] TK_MMA_StartOneFrame(ctxA raw preview Y) @0x17590 +
  optional second StartOneFrame @0x1724c + TK_MMA_WaitOneFrameComplete
  @0x17298 -> per-stream enc_stat poll + TK_H264Enc_ProcessOneFrame ->
  [if g_enable_dma] r4 bitstream-DMA loop (armed: big encoded transfer +
  enc_stat write-back pwrite(1) @0x17eec; idle: 4KiB dummy) -> channel_done
  pwrite EVERY iteration (site A @0x168d8 "re-announce previous" when no
  bitstream armed, site B @0x170e8 when armed) -> unlock, loop.
- THE enc_stat HANDSHAKE (host-facing ABI, new): /sys/vpl_pciep/enc_stat<idx>
  backs onto the SAME host-visible struct as the tokens - BAR0+0x50+idx,
  idx = ch*2+stream. Card preads it each frame: byte 0 -> arm bitstream
  (g.0xe98=1); 1 -> busy, retry x10 then skip; 2 -> skip. After DMAing a
  bitstream the card writes 1 there itself. NOTHING card-side clears it:
  **the HOST must write 0 to BAR0+0x50+idx to ack frame consumption** or
  encoding stops after one frame. (encode_status_store0 in ep.ko is the
  only writer that can return it to 0.) => DRIVER TODO regardless of
  anything else: clear enc_stat byte on every frame-done.
- The mailbox is NOT read by this thread (the 0x16cc8 pread is enc_stat, not
  the command mailbox); stale opcodes are irrelevant to it.
- g_enable_dma (card global 0x7ed88): set to 1 by tinyvenc main's
  START_STREAMING handler; if 0, ALL DMA and BOTH channel_done sites are
  skipped (0x1677c -> 0x168dc).

CROSS-CHECK AGAINST HARDWARE (M35-M39) - the model narrows hard: if the loop
were completing iterations we would see channel_done pwrites (every
iteration, both sites host-visible via BAR0 tokens) and the enc_stat
write-back (BAR0+0x50 byte flip). We see NEITHER, ever, and no idle 4KiB
transfers after repoison. Therefore the thread parks INSIDE iteration 1,
after the raw preview DMA (data landed) and before the r4 loop. Two
candidates remain:
  P1 TK_MMA_WaitOneFrameComplete @0x17298 (preview-DMA completion wait;
     = MassMemAccess_WaitDMAC infinite ioctl retry; kernel frees ring slots
     only in the wait, filp tracks only the LAST submitted slot - a
     2-submit/1-wait imbalance would leak+park deterministically);
  P2 TK_H264Enc_ProcessOneFrame @0x17f08 (hardware H.264 encoder wait; may
     depend on an engine/clock that is dead in nosg mode).
Second subagent dispatched to split P1/P2 (submit/wait balance @0x17580-
0x172a8, encoder blocking semantics via libtk_h264_encoder + vma_h4ee.ko)
and name the host-side unblock if any.

M39 RESULT (hardware, 2026-08-07): FRAME LANDS BOTH ROUNDS. Two streamons on
one module load, extent buf[0]=0x6d384 after each SET_VIC/START (irqs 3/0 ->
7/0, still no video IRQs, 0 faults). The card is NOT wedged: every fresh
tinyvenc5 spawn produces exactly one complete frame and then its loop stops.
The defect is therefore in the thread's own continuation/exit logic after
frame 1 - not in the DMA engine, ring, aperture, host addressing, IRQ
delivery, credits, or any host-visible machinery. All of those are now
proven good end-to-end.

================================================================================
M50: nosg POLLING CAPTURE - FIRST WORKING V4L2 DELIVERY, AND THE FAKE FRAME
     IS THE CARD'S OWN "NO SIGNAL" SPLASH AT 720x1080
     (2026-08-14, driver work + hardware VERIFIED)
================================================================================

Exploited three proven facts (no new RE needed): the raw burst is fully
contiguous (M37), so "tail dwords != poison" == frame complete; each fresh
spawn yields exactly one frame (M39); STOP=op7 ends a spawn cleanly. New
mz0380_nosg_thread (mz0380-dma.c): per frame - poison buf0, dma_start
(spawn), poll burst tail, copy NV12 payload to vb2, quiet op7, respawn.
stream_nosg=1 switches the V4L2 node to NV12.

HW RESULT: 4/4 frames delivered to userspace via v4l2-ctl --stream-mmap,
exact payload sizes, one SET_VIC per frame, 0 faults during capture (the
single logged IO_PAGE_FAULT at 0x90000000 predates streaming = the known
pre-READY boot artifact). start_delay_ms=500 works: ~1.9 s/frame. This is
the first end-to-end Linux capture from this card.

FRAME CONTENT (decoded offline from the delivered capture): the fake frame
is NOT a flat fill - it is the card's own rendered "NO SIGNAL" splash
(spinner icon + "NO SIGNAL" text). Layout inside the 0x30a5c0 burst:
  - Y plane   720x1080 @0x0      (0x11 background, splash graphics)
  - UV plane  720x540  @0xbdd80  (all 0x80 = neutral chroma, grayscale)
  - 0xff junk fill (uninitialised card memory, sparse noise) to burst end
So the renderer draws at 720 WIDE regardless of SET_VIC width=1920, while
the DMA length stays 1920x1107x1.5. First extraction assumed NV12 1920x1080
=> junk read as chroma (all-pink frame, 720-stride rows wrapped 8/3 times).
Driver now delivers NV12 720x1080 (0x11cc40 bytes).

Notable implications:
  1. The card HAS an on-board splash/OSD renderer the fake path exercises -
     the encode pipeline input is a real rendered picture, not a stub.
  2. The 0x11 "fill" of M30-M38 was this splash's background all along.
  3. Open question (cosmetic): whether the 720-wide render is fake_frame's
     hardcoded canvas or tracks some other config (OSD size? input dims?).

--------------------------------------------------------------------------

 M76 THE CARD-SIDE CAPTURE CHAIN, FULLY DECOMPILED. (2026-08-19)

Tooling note: system objdump has no ARM support here; use `llvm-objdump` or
Ghidra headless (`/opt/ghidra/support/analyzeHeadless <proj> <name> -import
<file> -processor ARM:LE:32:v5 -postScript DecompAll.java <out.c>`). All four
card-side binaries were decompiled this pass: `vpl_vic.ko`, `ep.ko`,
`video_capture_mgr`, `tinyvenc5`, plus `libvideocap.so.13`.

### 1. What "No signal !!" actually is (the blocker, precisely located)

`VideoCap_GetBufVIC` is ONE ioctl and nothing else:

    ioctl(fd, 0x8078e303, &buf)      /* _IOR(0xe3, 3, 120) on /dev/vpl_vicN */

It returns the ioctl's own return value. In `vpl_vic.ko`'s `Ioctl` the GETBUF
arm returns **-1** when the channel has no completed frame queued
(`chan->done_list == NULL`), which is what `VideoCap_GetBuf` turns into the
console line. The three error messages are selected from a status word the
driver copies out of the VIC's hardware MMR (per-channel byte in MMR+0x30):

    (stat & 0x12) == 0x12   ->  "[VIDEOCAP][ERROR]: FIFO full (error frame) !!"
    (stat & 0x14) == 0x14   ->  "[VIDEOCAP][ERROR]: No signal !!"
    (stat & 0x17) == 0x10   ->  vpl_vic ISR printk
                                "(CCIR or width(%lu) chck fail)"

**All three are hardware verdicts about the BT1120 bus**, produced by the SoC's
video input controller, not by any software policy we can reach from the host.
"No signal" means the VIC saw no valid embedded-sync stream on its input pins
while the MST3367 held lock. Bit 0x10 is the shared error flag; bit 0x02 is
FIFO overrun; bit 0x04 is no-signal. They are only distinguishable on the
card's console.

### 2. SET_VIC -> capture config: complete, byte-exact, and OUR PACKET IS RIGHT

`video_capture_mgr` op 0x29 fills a per-channel struct, then `FUN_0000a290`
(vcm 0xa290) rewrites the 163-line template `nullsensor_1920x1080.cfg` into
`/tmp/nullsensor_yuan%d.cfg`, which the freshly spawned tinyvenc reads. Only
these cfg lines are patched - everything else is copied verbatim:

  cfg line                    <- source
  "input format"              <- SET_VIC byte 7   (6=BT1120p, 7=BT1120i)
  "output format"             <- **derived from byte 6 (fw)**: 2 (YUY2) if
                                 fw == 6, else 1 (YV12). NOT byte 12.
  "start x position"          <- bytes 20..21
  "start y position"          <- bytes 22..23
  "input frame width"         <- bytes 24..25 (falls back to bytes 8..9 if 0)
  "input frame height"        <- bytes 26..27 (falls back to bytes 10..11 if 0)
  "flip video"                <- byte 13
  "mirror video"              <- byte 14
  "maximum frame width"       <- (capture width + 15) & ~15
  every other line valued 1920 <- capture width  (bytes 8..9)
  every line valued 1080       <- capture height (bytes 10..11)

Consequences worth recording:
  - Byte 12 ("m") never reaches the cfg at all; it is passed on the tinyvenc
    argv instead. Sweeping `vic_out_format` therefore cannot change the
    capture output format - only `fw == 6` does that.
  - fw in {2,3} makes vcm OVERRIDE the width to 1968 (1936 when height ==
    576). Those are the SDI paths. fw >= 4 rounds width up to a multiple of 16.
  - fw == 7 spawns ./tinyvenc7, fw == 8 spawns ./tinyvenc8, anything else
    (including 6) spawns ./tinyvenc5.
  - Hard guard: if capture width < 128 OR capture height < 128, vcm does
    NOTHING - no cfg, no spawn.
  - `tinyvenc5` has VideoCap **14.0.0.0** statically linked; the on-disk
    `libvideocap.so.13` (13.0.0.4) is not what runs. Same logic, different
    build - hence `VideoCap_CheckVIC`'s version-only "Invalid vpl_vic driver
    version" string.

Verdict: the driver's 44-byte SET_VIC packet and the cfg it produces are
correct. The cfg is eliminated as a suspect.

### 3. NEW: ep.ko has a NO-SIGNAL LATCH that silently swallows commands

In `pciep_isr`, op 0x29 does:

    if (width != 0 && height != 0) {
        no_signal_latch = 0;
        windows_select_fw = (fw == 7) ? 7 : 5;
        sysfs_notify("command");            /* wakes video_capture_mgr */
    } else {
        printk("$$$ cmd(%d) => no signal");
        no_signal_latch = 1;
    }

While the latch is set, ops **6, 7, 9, 0x2a, 0x2f, 0x31, 0x60, 0x61, 0x62**
skip their sysfs_notify entirely - the card never sees them - but ep.ko still
runs its MSI path, so **the host observes a clean completion for a command
that was discarded**. Only a SET_VIC with non-zero width AND height clears it.
This is the mechanism behind the whole class of "returns 0, nothing happens".
Our driver has exactly one SET_VIC call site and never sends zero dimensions,
so this is not our current bug - but it is a real trap.

### 4. NEW: SET_VIC byte 34 = int_reduce_en

Previously unnamed in our field map. ep.ko stores it at its private +0x630 and
`store_channel_done` branches on it to choose between coalescing frame-done
events into the shadow EVENT word and raising an MSI per event. We send 0
(the more-interrupts option). Worth diffing against what Windows sends.

### 5. CONFIRMED: the one-shot MSI credit, and what re-arms it

    msi():                 EVENT |= 0x800
                           if (msi_enable) { BAR0[0x30] = EVENT; EVENT = 0;
                                             msi_enable = 0; ring doorbell; }
    pciep_isr_clrint():    BAR0[0x30] = 0;  msi_enable = 1

So the credit is consumed by every MSI and re-armed by the host's
interrupt-clear write - i.e. by our existing ack. The "dead credit"
hypothesis from M35 is closed: the credit cannot stay dead as long as we ack.

### 6. Loose ends surfaced, not chased

  - `tinyvenc5`'s `Start()` reads `/sys/vpl_pciep/dency`; value 3 turns on
    "split_check" for the channel. `dency` is one of ep.ko's sysfs knobs
    (hready/hency/aency/dency/qency/wency/pre_uv) - none is written by the
    host command path we use.
  - The unexplored I2C device at 0x98 remains unexplored.

### Where this leaves the search

Every host-controllable software input to the capture chain is now verified
correct against the card's own code, and the failing verdict is produced by
the SoC's VIC hardware about the BT1120 bus itself. The next discriminating
evidence is physical: the card's serial console (which of bits 0x02/0x04/0x10
is set), or a probe on the BT1120 lines.

One cheap software experiment does remain, because the cfg exposes the VIC's
width register independently of the capture width: send SET_VIC with
bytes 24..25 (input frame width) = 3840 while leaving capture width at 1920.
If the MST3367 is emitting 8-bit double-rate samples rather than 16-bit
BT1120, the VIC's width check would fail exactly as observed, and this is the
only host-side way to test that without a scope. The paired variant is
in_fmt = 3 (CCIR656p) with the same 3840.

### M76 SWEEP RESULT (hardware, 2026-08-19): width hypothesis DEAD, and the
### card is writing its NO-SIGNAL SPLASH on the REAL path

`mz0380-m76-vicwidth-sweep.sh`, four passes against a locked 1080p60 source
(R55=0x7f, htot=2200, vtot=1125, hact=1920 every pass):

    vic_in=1920x1080 in_fmt=6   ret=0   captured 0 bytes
    vic_in=3840x1080 in_fmt=6   ret=0   captured 0 bytes
    vic_in=3840x1080 in_fmt=3   ret=0   captured 0 bytes
    vic_in=1920x1080 in_fmt=3   ret=0   captured 0 bytes

Byte-identical outcomes. **The VIC width/format declaration is eliminated** -
the 8-bit double-rate hypothesis is dead, and so is CCIR656p as an input
format. `vic_in_w`/`vic_in_h`/`vic_in_fmt` stay in the driver as inert
(0-default) levers.

The sweep's own grep hid the important part. Every pass reported:

    stop buf[0] @0x100000000 head=11 11 11 11 11 10 11 11 ... |
        760/1024 sampled pages touched, last @0x2f7000
    stop buf[1..3]                                            0/1024

Buffers are freshly poisoned with 0xAA at every arm (bufs 1-3 still read 0xAA,
so the scan is sound). So on the **real** path, `is_nosg=0`, with the receiver
locked for the whole 60 s window, the card DMAs ~3.1 MB into buf0 -
deterministically, same extent and same head bytes in all four passes.
0x2f7000 = 3,108,864, i.e. the 0x30a5c0 (3,188,160) fake-frame burst extent of
M30-M38, and 0x11 is that splash's documented background fill.

This overturns two things the previous handoff asserted:

  1. "the card's VideoCap never delivers a frame / host buffers stay
     untouched (0/1024 pages)" - it *does* deliver, and the destination
     programming, opcode 0x02, IOVA remap and outbound window all work on the
     real path, not just the synthetic one.
  2. The failure is not "nothing happens". tinyvenc5 is running and producing
     its **no-signal fallback picture** - which is the card-side expression of
     the `VideoCap_GetBuf -> "[VIDEOCAP][ERROR]: No signal !!"` path decoded
     above. The card's own userspace has decided there is no input while the
     MST3367 holds lock.

`captured 0 bytes` is therefore a separate, downstream fact: raw splash bytes
land in buf0 but no completion (enc_stat / frame token) is ever signalled, so
the V4L2 node has nothing to hand out. Whether that is because the splash is a
one-shot write outside the encode path, or because completion is only raised
for encoded output, is the next question - and the m55 run that produced this
was grep-filtered, so the `stream start|frame token|enc` lines were never
seen.

Instrumentation added for the next pass: `/proc/mz0380-buf0` dumps stream
buffer 0 verbatim (0400), and m55 now writes it to `/tmp/mz0380-buf0.bin`
before unloading the module, so the content can be identified offline rather
than inferred from 16 head bytes.

### M76 buf0 DECODED, and the console is reachable over PCIe after all

`/proc/mz0380-buf0` dump of the M55 run, region map:

    [0x000000, 0x1FA400)  2,073,600 = 1920x1080 Y, fill 0x11 + ~13.6 KB of glyphs
    [0x1FA400, 0x2F7600)  1,036,800 = 1920x540  UV, all 0x80 (neutral = grayscale)
    [0x2F7600, 0x400000)  0xAA poison, untouched

Exactly 1920*1080*1.5. Rendered, the Y plane is the card's **"NO SIGNAL"**
splash (spinner + text, centred) - at OUR SET_VIC geometry, not the 720-wide
canvas of M50. So the card accepted our width, ran tinyvenc5, and drew its
no-signal picture while R55 held 0x7f LOCKED for 60 s. `irq_total=4` (the
command acks), `frame_events=0`, `EVENT=0`, `enc=0`: raw splash bytes land in
buf0 with no completion, which is why V4L2 sees 0 bytes.

Output-stage diff vs stoth68000/hdcapm `mst3367-drv.c` (an independent Windows
I2C trace of the same receiver family):

    reg        ours (live)   hdcapm      note
    BANK0 b0   0x21          0x14        ours = init 0x14 -> 0x20 -> per-mode
                                         (b0 & 0xc2) | (vic_b0 & 0x3d)
    BANK0 b1   0xc0          0xe0        ours is deliberate: HD60 Pro's own
    BANK0 b2   0x00          0x08          FUN_14024dc28, "sibling uses e0/08"
    BANK0 b4   0x55 -> 0x54  same
    ad/ae/b5/73/1e/1f/90/91/CSC           identical

So b1/b2 are board-specific and sourced from this card's Windows driver, not a
bug; and the `vic_b0` parameter's premise ("hdcapm uses 0x20") is wrong - the
only 0xb0 write in hdcapm is 0x14. The receiver config is not obviously at
fault, and static comparison has run out of road.

**The card's console does not need a UART.** mz0380-fw.c uploads
`/lib/firmware/mz0380/MZ0380.HD.HEX` - a gzip+tar of the entire
`yuan_demo_sdi/` rootfs, including `yuan_start_process.sh` - on every insmod,
and the card reboots into it. We therefore own the card's userspace. And
`video_capture_mgr` op **0x6e (LOAD_FILES)** is an arbitrary-offset 16-byte
read/write of a fixed path, `/mnt/flash/PIC_ENC` (vcm FUN_0000aa3c):

    param1 = is_write (0 = read)   param2 = fseek offset
    payload = 16 bytes at cmd[12..27], echoed back to the host by pwrite()

Redirect tinyvenc5/video_capture_mgr stdout+stderr to `/mnt/flash/PIC_ENC` in
`yuan_start_process.sh`, then page it back 16 bytes at a time with op 0x6e.
That yields the exact line the card prints - which of "FIFO full",
"No signal !!" or "(CCIR or width chck fail)" - over PCIe, with no board
access. Recovery path if an image is bad: the card keeps
`/mnt/flash/yuan_demo_sdi_bak` and `yuan_update_only.sh`.

================================================================================

 M78 THE SOURCE IS ENCRYPTING THE LINK. (2026-08-19)

Read the LINK layer for the first time. R55 lock is the timing front end only;
whether the receiver forwards pixels to BT1120 is a different layer, and we had
never looked at it. hdcapm's RxTmdsGetType names the registers, and they are
the same two the Windows driver reads right after its output-stage commit:

    BANK1 0x01  bit2 = HDMI (clear = DVI), bit1 = HDCP_OP_STS, bit0 = HDCP_MD
    BANK1 0x34  bit7 = HDCP in use

Hardware, three sample points in one run (before START, after START, +60 s),
identical every time, receiver holding R55=0x7f the whole window:

    link: B1 01=8d 34=90 -> HDMI, HDCP present, ACTIVE (encrypted)
          B2 0b=38 0c=11 0e=df 48=80

0x8d bits [2:0] = 101, which the MST3367 table reads as **HDMI EESS + HDCP,
without advance cipher**. The source is encrypting.

This dissolves the contradiction the project has been stuck on since M45. TMDS
clock recovery and timing measurement work on an encrypted stream - the clock
and syncs are not encrypted, the pixels are. So "receiver LOCKED, coherent
1080p60" and "the SoC's VIC sees no signal" were never in conflict; we were
reading one layer and drawing conclusions about another. Every downstream
elimination this session (cfg, SET_VIC field map, VIC width/format, DMA
destination opcode, MSI credit, output-stage register diff) stands, and none
of them could ever have produced a frame while the pixels arriving at the
receiver were undecryptable.

It also retires the console-over-PCIe plan as the next step: the card's log
would have printed "No signal !!" and explained nothing.

NOT re-litigated, and worth keeping straight: the 1920-wide "NO SIGNAL" splash
of M76 is OUR geometry echoed back (host -> SET_VIC -> generated cfg ->
tinyvenc render canvas). It proves the config path and the render/DMA/host
path, and nothing whatsoever about what the SoC sees. No number originating
from the VIC's view of its input has ever reached the host.

NEXT TEST (no code): attach a source that does not assert HDCP - a PC desktop
output, a Pi, a console with HDCP disabled - and run m55. The link line then
reads "HDCP absent", and either frames flow (diagnosis confirmed, driver done)
or they do not (HDCP was a red herring, and the receiver -> VIC path is back
in scope along with the console plan).

The test source used for every run on record is a digital microscope/camera.
Cameras and camcorders commonly assert HDCP on their HDMI output
unconditionally, which fits the entire symptom history.

Instrumentation from this pass, all read-only and kept:
  - mz0380_mst3367_output_diag() now also reads BANK1 0x01/0x34 and BANK2
    0x0b/0x0c/0x0e/0x48 and decodes HDMI-vs-DVI and HDCP, at all three
    existing sample points. m55 surfaces it under "link layer".
  - /proc/mz0380-buf0 (M76) and /proc/mz0380-cardlog + LOAD_FILES (M76/M77)
    remain available; the cardlog_probe self-test is built but unrun.

================================================================================

 M79 DEEP COLOUR: A VALID 1080p60 SOURCE WAS BEING REJECTED OUTRIGHT.
 (2026-08-19, fixed and verified on hardware)

A third test source read, stably and repeatedly:

    htot=2750 vtot=1125 hact=1920 hper=674 vper=599 lines=1125 p [R55=0x7f]
    => "MST3367 coherent but unsupported - please report"

Detection failed, so the run never reached STREAMON at all. But every field
except htotal is a textbook 1080p60, and 2750 == 2200 x 1.25.

The MST3367 counts htotal in TMDS CHARACTER CLOCKS, which HDMI deep colour
scales: 24-bit x1, 30-bit x1.25, 36-bit x1.5. hperiod and vperiod run off a
fixed reference and are unaffected (67.4 kHz / 59.9 Hz, both correct), and
hactive is a video-domain counter and reads a true 1920. Only the TMDS-domain
number moves. The source is sending 30-bit deep colour.

The driver already half-knew this. mst3367_match_mode() tries raw and x3/2,
and hdcapm's table carries 720p60 twice (1650 and 2475). That duplicate row is
not "the same mode on a Tivo" - it is a 36-bit deep-colour source. We handled
x1 and x1.5 and had a hole at x1.25.

The table sits in the x1.5 domain, so a 30-bit measurement needs x1.5/x1.25 =
x6/5: 2750 * 6 / 5 == 3300, dead centre of the 1080p60 row (3290-3310). Third
scaling pass added to mst3367_match_mode().

HARDWARE RESULT: same source now reports "=> MATCHED", yields the correct
1920x1080p60 V4L2 preset with a real 148.5 MHz pixel clock, and arms STREAMON.
Independent of the capture blocker, and a bug any deep-colour source would hit.

--------------------------------------------------------------------------

 M79b HDCP DOWNGRADED AS A SUSPECT

Three sources now - a microscope/camera, a phone over a USB-C HDMI dongle, and
the deep-colour source above - all read B1 01=8d 34=90, byte-identical.

M78 read that as the blocker. On reflection that is too strong: a source only
encrypts AFTER a successful HDCP handshake, so "active/encrypted" means the
MST3367's HDCP engine authenticated and is decrypting - normal operation, and
what Windows would see too. The Elgato "refuse to capture protected content"
behaviour is HOST-driven (the Windows driver selects /tmp/PIC_HDCP via the
logo opcode); the card does not refuse on its own, it reports no signal.

HDCP stays recorded as a fact about the links we have tested. It is no longer
the leading explanation. A non-HDCP source would still be a clean control if
one turns up, but it is not worth hunting for.

--------------------------------------------------------------------------

 M80 THE REAL SUSPECT NOW: EMBEDDED SYNC vs EXTERNAL SYNC

All three sources also report input colorspace RGB (B2 0x48 & 0x60 == 0x00).
More to the point, this driver's own receiver init ends with

    /* YUV422, 8-bit, external sync */
    mst_wr(dev, 0xb0, 0x20);

EXTERNAL sync - separate HS/VS/DE. And we tell the SoC's VIC in_fmt=6 =
BT1120p, which carries sync EMBEDDED in the data as SAV/EAV codes. A VIC
scanning for embedded timing codes in a stream that has none reports exactly
what we observe: no valid sync, while the timing front end stays locked.

The cfg enum is 1:8-bits Raw, 2:CCIR656i, 3:CCIR656p, 4:Bayer, 5:16-bits Raw,
6:BT1120p, 7:BT1120i. 2/3/6/7 are all embedded-sync. 5 is the 16-bit
external-sync sibling of 6 and has NEVER been tried - M76 swept only 6 and 3.

mz0380-m80-syncmode-sweep.sh sweeps in_fmt x vic_b0 (we write 0x21; hdcapm's
only 0xb0 write is 0x14). Read the buffers, not the capture: buf0 at 760/1024
is the NO SIGNAL splash and means the pass failed. buf[1..3] touched, or a
nonzero capture, is the answer.

### M80 RESULT (hardware): sync-mode pairing is NOT the blocker - but the
### splash turns out to be a free progress oracle

Five passes, deep-colour source, mode MATCHED, SET_VIC ret=0 every time:

    in_fmt  vic_b0   buf0            capture
    5       0x21     0/1024 poison   0 bytes
    5       0x14     0/1024 poison   0 bytes
    6       0x14     0/1024 poison   0 bytes
    1       0x21     0/1024 poison   0 bytes
    2       0x21     760/1024 SPLASH 0 bytes

No pass captured anything, and no buffer other than buf0 was ever touched. The
embedded-vs-external sync hypothesis is dead: both families fail, and so do
both 0xb0 values.

The unplanned finding is the buf0 column. Combined with M76 (in_fmt 6 and 3,
b0=0x21, both splash):

    b0=0x21 + an EMBEDDED-sync in_fmt (2, 3, 6)  -> card renders NO SIGNAL splash
    a "Raw" in_fmt (1, 5), or b0=0x14            -> card writes nothing at all

So SET_VIC byte 7 and the receiver's 0xb0 both demonstrably change how far the
card gets, which independently confirms the control surface reaches the card's
VIC init. More useful: **the splash is a free progress oracle.** "Splash" means
VideoCap initialised and the capture loop ran and failed on no-signal;
"nothing" means it failed earlier, at init. Any future config change can be
scored on that axis with no console at all.

Not a wedge: the last pass still rendered, so the card was healthy throughout
and the 0/1024 passes are real results, not exhaustion.

Where that leaves the search. The host-side configuration space is now
genuinely exhausted: cfg contents, every SET_VIC field, VIC width/height,
input format across both sync families, output format, DMA destination
opcode, MSI credit, receiver output-stage registers against two independent
Windows traces, and the link layer. The one instrument not yet used is the
card's own log, and M76/M77 already built and proved the transport for it
(LOAD_FILES op 0x6e round-trips cleanly; /mnt/flash/PIC_ENC simply does not
exist yet). What remains is the one-line change to yuan_start_process.sh that
creates it.

---

### M82 (Windows collect-2026-08-19): the capture-start sequence was wrong, and
### so was the interrupt model

Source: `/run/media/wolffyx/Work/hd60-trace/collect-2026-08-19/`, a full
re-collection on the working Windows 11 machine - four live DebugView kernel
traces plus a capstone re-analysis of `e60MZ0380.X64.SYS` v1.1.0.195. It
answers the `WINDOWS_SESSION.md` list and overturns several things this project
had settled.

#### What it kills

**EDID is not the blocker, and was never on the path.** All four traces,
including one complete `DriverEntry`, contain zero `[UPDATE.EDID]`, `[HOTPLUG]`
or `VSTATE_*` lines. The code at `0x140248110` explains it: the push is gated on
a dirty flag at `EDID_buffer+0x100` that `DriverEntry` never sets, so the driver
delivers an EDID only when an application hands it one - and no Elgato app was
installed. Sources locked and frames reached OBS anyway. Host-side EDID
delivery is **not required for this card to capture**. (The blob itself is a
static 256 bytes in `.data`, monitor name `SC530-N1`, saved in the collection.)

**HDCP is not the blocker.** The DSLR - one of the sources this driver cannot
capture - captures fine on Windows: `R0055 = 7F`, 2200x1125, dotclock 74175,
all four pin slots open, full command sequence, ring advancing, and no HDCP
path anywhere in any trace.

**Firmware 01.11 is the shipping version.** Our `MZ0380.HD.HEX` is byte-identical
to Elgato's (sha256 `be0d5e19...`). There is nothing newer to prefer, so
restoring `MZ0380.FW.TXT` to `01.11` is correct and stops the 21 s re-upload.

**`R0055 = 0xFF` is a valid lock**, interchangeable with `0x7F`; only `0x03`
means no signal. Our `mst3367_status_locked()` gates on `& 0x1c` and already
accepts it, so no change - but a stricter predicate would have rejected a good
signal.

#### 1. The interrupt path we use is one Windows has never exercised

`DEVPKEY_PciDevice_InterruptSupport = 3` and `InterruptMessageMaximum = 1`:
the endpoint **does** advertise MSI, one message. Windows declines it.
`MessageSignaledInterruptProperties` is absent for this device (14 other PCI
devices on the same board have it), the INF has no `MSISupported`, `AllocConfig`
assigns a level-sensitive shared line with `CM_RESOURCE_INTERRUPT_MESSAGE`
clear, and the trace logs `INTERRUPT = 00000000` on IRQ 29.

We called `pci_alloc_irq_vectors(..., PCI_IRQ_MSI | PCI_IRQ_INTX)`, which
prefers MSI because the hardware says it is available. On top of that,
`mz0380_initdev()` called `pci_intx(pci_dev, 0)` and never undid it, so the
legacy line was masked even if MSI had failed to bind. If the card's MSI path
is unwired in firmware - which the vendor refusing to use it hints at - the
symptom is exactly ours. Now defaults to INTx (`irq_intx=1`), and the line is
unmasked once an ISR exists.

#### 2. `0x29` IS sent by Windows, first, and `CARD-ADDRESS-MAP.md` was wrong

That document states in two places that `SET_VIC_PARAMS` is never sent by the
Windows driver. It is, from `0x14028bc4c`. The earlier sweep missed it because
the store is a plain `mov dword ptr [rbp+0xc4], 0x29` inside an 11 KB function.
The full sequence, from one function at `0x14028a248`, on every mode change:

    0x07  STOP_STREAMING   word[2]=0xFFFFFFFF, count 3, flag 1
       ~1.9 s              (1.84-1.91 s in all six observed reconfigurations)
    0x29  SET_VIC_PARAMS   count 0x0B, flag 1
    0x2A  SET_AIC          count 6,    flag 1
    0x2D  encoder, main    count 0x0C, flag 1, mask 0x3FFF, main_or_sub = 0
    0x2D  encoder, sub     count 0x0C, flag 1, mask 0x3FFF, main_or_sub = 1
    0x31  POST_PROC        count 7,    flag 1, mask 0x1F, di = 1

Ours was `0x29 -> 0x2d(main) -> SET_BUF -> 0x2a -> 0x06`. Differences that
matter: no stop first, no settle, the encoder before the audio, only one
encoder stream, and **`0x31` never sent at all**.

`[FIRMWARE RESET]` is resolved and is not a reset: the log site only sets a
"reconfiguration pending" flag; the work is `0x14028cf38` (the `0x07` above)
followed on the very next instruction by the reconfigure. `count = 3` and
`word[2] = 0xFFFFFFFF` are both required - Windows' SEND_COMMAND rejects
`count <= 1`.

**Windows never sends START_STREAMING (`0x06`) on the capture path.** It does
not need to: ep.ko routes `0x2d` (45) and `0x31` (49) to a bare
`sysfs_notify("epint")`, the same wake `0x06` performs. The tail of the
sequence is itself the kick.

#### 3. SET_VIC, byte-exact - five fields were wrong

Decoded from the nine dword stores at `0x14028bc56..0x14028bd49` and confirmed
against the driver's own printf, which proves the byte assignment independently:
`vi` is read from the slot that lands at **byte 7** and `fw` from **byte 6**.

| byte | field | Windows sends | we sent |
|---|---|---|---|
| 6 | `fw` | **6** at <=30 fps, **7** at 60 | 5 |
| 7 | `vi` / input format | 6 (BT1120p) | 6/7 by interlace - matches |
| 12 | `m` | 0 at integer rates, 1 at 29.97 | 1 |
| 16..19 | `color_info` | **0x02010101** (1,1,1,2) | 0x00800000 |
| 33 | `fast_kill` | **1** | 0 |
| 36..39 | `nosg` back/Y/U/V | **0.00.80.80** | 0 |

`fw` is the sharpest of these. M79 established that vcm derives the capture
cfg's *output format* from it - 2/YUY2 when `fw == 6`, else 1/YV12 - and spawns
`./tinyvenc7` when it is 7. We have always sent 5, which is not a value the
retail driver has ever used; it reaches tinyvenc5 by fallthrough but selects
the other pixel format.

The M72 reading of bytes 16..19 as brightness/contrast/saturation/field-invert
is retired: they are `color_info[0..3]` and Windows sends the same four bytes
for every source in every trace.

#### 4. SET_AIC byte 17

`aic_int_mode = 1` in every trace (ep.ko stores it as `G[0x63c]`); we sent 0.
Also settled: `frame_num_of_period = 4`, and the sample rate follows the client
rather than being fixed at 48 kHz.

#### 5. What is now the leading suspect: two I2C slaves we have never driven

The static I2C trace finds **four** 8-bit addresses, not the two this project
assumed:

    0x9C  MST3367 receiver         454 transactions   (we drive this)
    0x98  colour-space / video path 233               (never touched)
    0x90  alternate CSC path        247               (never touched)
    0xA0  EDID EEPROM                 2 bulk writes

The 18 bytes to `0x98` sub-address `0x76` that `WINDOWS_SESSION.md` §3 asked
about are YCbCr->RGB conversion matrices: little-endian 14-bit two's-complement
Q11, and the BT.709 variant decodes to the exact ITU-R constants (+1.5396,
-0.4595/-0.1831, +1.8159), which confirms the reading. One of two matrices is
chosen by a colorimetry bit out of a video-format table at `0x1402d393c`.

That trace is static, so it covers every board the driver supports and does not
by itself prove either chip is fitted on the HD60 Pro. **But we already proved
0x98 is** - and then threw the result away. The M43-M49b bus enumeration (op
0x1a probe, non-zero = present) recorded: "ONLY 0x9c (main) and 0x98 (flat map,
ignores bank-select, not EDID) answer". At the time everything was being judged
by whether it could hold an EDID, so a device that clearly was not an EEPROM
was noted and dropped. It is a fitted, ACKing slave that the Windows driver
writes 181 times and this driver has never written once. (The same enumeration
found nothing at 0xa0, which is consistent: Windows only ever *writes* that
address, and only on the EDID path that never runs.)

The open blocker is
that the SoC's VIC reports no signal on BT1120 while the receiver holds a clean
lock, and a colour-space/video-path device between the receiver's output and
the SoC's input - sitting on power-on defaults because nothing has ever written
it - is the first new candidate for that since the host-side configuration
space was declared closed. `mz0380-m83-i2c-devscan.sh` settles whether they
ACK; it costs zero encoder spawns and needs no source.

#### 6. Frame geometry hypothesis

`DriverEntry` logs `[MEMORY] [00466000] [0034BD00] [0034BD00]`. `0x34BD00` =
3,456,256, and `2048 x 1125 x 1.5 = 3,456,000` - 256 bytes short. That is a
YUV420 frame at a **2048-byte stride** over **1125 lines** (the *vtotal*, not
the 1080 active lines) plus a 256-byte header, allocated twice. One sample, so
a hypothesis and not a layout - but it is a concrete alternative to assuming a
tightly packed 1920x1080 frame. `0x466000` is a third region, unexplained.

#### Changes made

`irq_intx` (def 1), `win_seq` (def 1), `stop_settle_ms` (def 1900),
`vic_color_info` (def 0x02010101), `vic_fast_kill` (def 1), `vic_nosg`
(def 0x80800000), `aic_int_mode` (def 1), `enc_sub` (def 1), `enc_mask`
(def 0 = keep the conservative fps/gop/bitrate mask), `post_mask` (def 0x1f),
`post_di` (def 1), `win_start_op6` (def 0). `vic_fw` default 5 -> 0 (auto),
`vic_out_format` default 1 -> 0, `vic_saturation` now unset by default and only
overrides byte 18. All STOPs now carry the all-channels word.

---

### M83 RESULT (hardware, 2026-08-20): 0x98 IS fitted; 0x90 is not

Zero-spawn bus probe, `mz0380-m83-i2c-devscan.sh`, REG_READ (0x1a) over the
mailbox proxy, regs 0x00..0x3f, no source needed.

| chip | zero regs | verdict |
|---|---|---|
| `0x9c` MST3367 (positive control) | 38 / 64 | present, plausible bank-0 values |
| **`0x98`** | **10 / 64** | **PRESENT.** Dense, varied, high-entropy - `54 49 12 16 1c 60 ... 96 5e 9a 5f 0d fb 82 d9 ...` |
| `0x90` | 64 / 64 | absent - NAKs every address on this board |

So of the two slaves the Windows driver drives and we never have, **one is
really on this board**: `0x98`, which the retail driver touches 233 times (181
of them writes) and which has been sitting on power-on defaults for the entire
life of this project. `0x90` is another board's path and can be ignored.

This also retro-confirms the M43-M49b enumeration, which had already seen 0x98
answer and filed it as "flat map, ignores bank-select, not EDID" - correct, and
irrelevant to what it actually is.

### M84 RESULT (hardware, 2026-08-20): the Windows sequence runs clean and makes
### things WORSE - the splash oracle went dark

First run of the M82 change set against a live 1080p60 source (not the DSLR).
Every command was accepted:

    pre-STOP(op 0x07, all channels) ret=0, settling 1900 ms
    SET_VIC(1920x1080p@60 fw=7 in_fmt=6 out_fmt=0 ...) ret=0
    SET_AIC(on=1, 2ch, 16bit, 48000) ret=0
    SET_ENC_PARAMS(0x2d, mask=0x0043, main ch0, fps=60, gop=60) ret=0
    SET_ENC_PARAMS(0x2d, mask=0x0043, sub  ch0, fps=60, gop=60) ret=0
    POST_PROC(0x31, mask=0x1f, fps=60, di=1) ret=0

Receiver held `R55=0x7f` for the whole 60 s window and re-MATCHED 1080p60
repeatedly across source power-cycles. And:

    stop buf[0..3]: 0/1024 sampled pages touched, head still aa aa aa ...
    EVENT=0 token=0 enc_stat=0 frame_events=0 irq_total=8

**All four buffers untouched.** That is a regression, not a null result: before
this change set the same path wrote ~3.1 MB of the card's NO SIGNAL splash into
buf0. Scored on the M80 oracle, we moved from "VideoCap initialised, capture
loop ran, failed on no-signal" to "failed earlier, at VIC init".

Six things changed at once, which is exactly the mistake the M81 post-mortem
warned about. Ranked suspects:

1. **`fw` 5 -> 7.** Much the largest blast radius. `fw == 7` makes vcm spawn a
   **different binary** (`./tinyvenc7`, which does exist in the image - checked)
   and makes ep.ko route every subsequent notify to **`epint_1080p`** instead of
   `epint`. Every host-side ABI we hold - notably the enc_stat handshake at
   BAR0+0x50+idx - was derived from tinyvenc5. Note Windows only uses 7 at
   60 fps; at 30 fps it uses 6, which still falls through to tinyvenc5 and only
   changes the cfg's output format to 2/YUY2. **So `fw=6` is the Windows value
   that keeps our known-good encoder binary, and is the right first bisect.**
2. **No op 0x06.** M22 proved on hardware that tinyvenc5 blocks on epint waiting
   for START before it DMAs anything. Windows substitutes the 0x2d/0x31
   notifies - but our 0x2d carries mask 0x0043, not Windows' 0x3FFF.
   `win_start_op6=1` restores it.
3. **SET_BUF moved before SET_VIC.** M23 put it after, so that op6's iATU latch
   would see our addresses. Now split out as `win_bufs_first` (M84) so it can be
   bisected without disturbing `win_seq`.
4. The value fixes (color_info, fast_kill, nosg, m, aic_int_mode) - low blast
   radius, but `fast_kill=1` is not a name to trust blindly.

**Unrelated but worth keeping: this source is HDCP-free and YUV444.**
`B1 01=8c 34=40` -> HDMI, **HDCP absent**; `B2 48=d2` -> **input colorspace
YUV444**. Every source before this one read `01=8d 34=90` (HDCP active) and
`48=80` (RGB). So the capture failure reproduces with no HDCP anywhere in the
link - which confirms the Windows oracle's verdict on our own hardware, not just
theirs. It also means the receiver's output stage is being asked to convert
YUV444 -> YUV422 for BT1120 with `b0=0x21`; hdcapm ends its init with
`0xb0 = 0x20`, and M80 swept 0x21 against 0x14 but never against 0x20.

`irq_total=8` on legacy INTx (irq 40) - the line does deliver, so the M82
interrupt change is at least not inert. Those are command completions, not frame
events (`frame_events=0`).

#### M84 fallout: the INTx switch exposed a teardown-order bug (fixed)

The run after M84 could not load at all - `insmod: Device or resource busy`,
with `lsmod` showing `mz0380 303104 -1`: **refcnt -1**, i.e. the module stuck in
`MODULE_STATE_GOING`. A module in that state can neither be removed nor
reloaded; only a reboot clears it.

Cause, from the journal:

    RIP: 0010:mz_read+0x16/0x20 [mz0380]
    CR2: 0000000000000030        <- MZ0380_MB_EVENT
    RAX: 0000000000000000        <- dev->bmmio[0], already NULL
    RSI: 0000000000000000        <- map index 0 = MZ0380_MAP_BAR_MMIO
    note: rmmod[26730] exited with irqs disabled

`mz0380_finidev()` called `mz0380_dev_unregister()` (which `iounmap`s both BARs
and NULLs `bmmio`) **before** `mz0380_irq_release()` (which does the
`free_irq`). Under MSI that window is harmless: an MSI source stops signalling
once the device is quiesced, so the handler is never entered after the unmap.
Under the shared INTx line the Windows driver uses, **every other device on
IRQ 40 enters our handler** - and the first one to do so inside that window
dereferenced a NULL `bmmio` in the ISR's very first `mz_mmio_read`, oopsing
inside `rmmod` with IRQs disabled.

So this is not a reason to back away from INTx; it is a latent ordering bug that
only a shared line can reach. Two fixes:

1. `mz0380_finidev()` now frees the IRQ before unmapping anything.
2. `mz0380_isr()` returns `IRQ_NONE` if `dev->bmmio[MZ0380_MAP_BAR_MMIO]` is
   NULL. A shared handler must not depend on teardown ordering alone.

Lesson for the scripts: `mz0380-m55-real-capture.sh`'s cleanup trap runs
`fuser -k` on the video node and then `rmmod`. When the `rmmod` itself oopses,
the trap reports a confusing `line 48: <pid> Killed` and leaves the machine
needing a reboot - the "Killed" is the dying `rmmod`, not the capture.

**Prevention, not just a fix.** `mz0380-m85-unload-smoke.sh` loads and unloads
the module with `enable_dma=1` (required - `mz0380_irq_request()` is gated on
it, and with no `request_irq` there is no `free_irq` and so no DEBUG_SHIRQ
callback to catch anything), `enable_video=0`, `firmware_upload=0`. Five
seconds, zero encoder spawns, and it fails loudly on any oops/BUG/WARN or a
module still listed after `rmmod`. Run it for **both** interrupt paths - a pass
under MSI proves nothing about INTx, because MSI cannot reach the shared-IRQ
teardown window at all. `mz0380-m55-real-capture.sh` now runs it as a preflight
and refuses to start if it fails or if the module is already stuck in
`MODULE_STATE_GOING` (`SKIPSMOKE=1` bypasses).

The general rule this cost us: **moving a driver to a shared interrupt is a
contract change, not a one-line parameter change.** The handler becomes
callable at instants it previously never was - including from inside
`free_irq()` - so it must be audited against every teardown path and must be
safe when the resources it reads are already gone.

### M86 CONTROL (hardware, 2026-08-20): the oracle holds, so M84 really is a regression

`fw=6` did not restore the splash either (0/1024 on all four buffers), which
clears tinyvenc7 - but it also does not clear the change set, because `fw=6`
is not the old configuration: per M79, `fw==6` makes vcm write "output format"
= 2/YUY2 into the cfg where our long-standing `fw=5` wrote 1/YV12. Neither M84
nor the fw bisect ever reproduced the pre-M82 state.

Worse, the "splash rendered" baseline had only ever been observed with the OLD
sources (RGB, HDCP-active). The current source is YUV444 and HDCP-free, so
"we regressed it" was, at that point, an unsupported inference. Run the control
before bisecting against a baseline you have not verified on the hardware in
front of you.

Control = the exact pre-M82 configuration, current source:

    WINSEQ=0 VICFW=5 VICM=1 INTX=0
    EXTRA="vic_color_info=0x00800000 vic_fast_kill=0 vic_nosg=0 aic_int_mode=0"

Result:

    stop buf[0] head=11 11 11 11 11 10 11 11 ... | 760/1024 pages touched, last @0x2f7000
    stop buf[1..3] untouched

**The splash is back.** So the oracle is valid for this source and the M82
change set is what took it away. Note the UV offset: 0xbdd80 reads 0x11, not
0x80, i.e. this is the 1920-wide layout, not M50's 720-wide canvas - the card
renders at our SET_VIC geometry, as M80 recorded.

`irq_total` was 4 under MSI here against 8 under INTx in M84/M85 - both are
command completions, `frame_events=0` in every run, so the interrupt path is
not implicated either way.

The change set splits cleanly in two, and one run separates them: **ordering**
(`win_seq`: pre-STOP + settle, SET_BUF before SET_VIC, AIC before the encoder,
sub-stream, POST_PROC, no op6) versus **values** (fw, out_fmt, color_info,
fast_kill, nosg, aic_int_mode) plus the interrupt path. `WINSEQ=0` with every
other M82 default is the split.

### M87 BISECT (hardware, 2026-08-20): the regression is in the VALUES, not the order

`WINSEQ=0` with every other M82 default (fw=auto=7, out_fmt=0, new color_info /
fast_kill / nosg / aic_int_mode, INTx): **0/1024, no splash.**

Old order + old values (M86)  -> splash
Old order + new values (M87)  -> nothing

So the whole ordering half of M82 is cleared: the pre-STOP with 0xFFFFFFFF, the
1.9 s settle, SET_BUF before SET_VIC, AIC before the encoder, the sub-stream
SET_ENC_PARAMS and POST_PROC(0x31) are all innocent. The card accepts the
Windows sequence exactly as the Windows driver issues it.

This also retro-invalidates the `fw=6` run as evidence about `fw`: it carried
`out_fmt=0` and the new cosmetics too, so its failure said nothing about the
encoder selector.

    run          order   fw   m   cosmetics   irq    buf0
    M84          new     7    0   new         INTx   nothing
    fw bisect    new     6    0   new         INTx   nothing
    M86 control  old     5    1   old         MSI    SPLASH
    M87          old     7    0   new         INTx   nothing

Next single variable from M87: `WINSEQ=0 VICFW=5` (only fw changes). If that
restores the splash then only fw=5 works, while Windows sends 6 or 7 - which
would be a real contradiction worth chasing rather than a settled answer. If it
does not, fw is cleared and the suspects are byte 12 `m` (M79: it never reaches
the cfg, it goes on the tinyvenc argv, where 0 may simply be rejected),
color_info, fast_kill, nosg, aic_int_mode, or INTx.

### M88 (hardware, 2026-08-20): the regression is ONE BYTE - SET_VIC byte 6 (fw)

`WINSEQ=0 VICFW=5`, i.e. M87 with `fw` changed from 7 to 5 and **nothing else**
- `out_fmt=0`, the new `color_info`/`fast_kill`/`nosg`/`aic_int_mode`, INTx all
still in place:

    stop buf[0] head=11 11 11 11 11 10 11 11 ... | 760/1024 pages touched, last @0x2f7000

**Splash back.** Bisect complete.

    fw = 5  -> tinyvenc5, cfg output format 1/YV12  -> splash renders
    fw = 6  -> tinyvenc5, cfg output format 2/YUY2  -> nothing written
    fw = 7  -> tinyvenc7, cfg output format 1/YV12  -> nothing written

5 and 6 spawn the SAME binary and differ only in the cfg's "output format"
line, so **YUY2 alone stops the capture loop before it writes a byte**. 7 fails
independently, via a different encoder. Everything else in M82 is cleared and
stays: the full Windows ordering (pre-STOP with 0xFFFFFFFF, 1.9 s settle,
SET_BUF first, AIC before the encoder, the sub-stream, POST_PROC 0x31), all
four value fixes, and legacy INTx.

`vic_fw` default reverted to 5. `vic_fw=0` still selects the Windows rule for
anyone re-testing the contradiction.

**And it is a contradiction worth keeping in view.** Windows sends 6 at 30 fps
and 7 at 60 and captures fine, on a card running a byte-identical firmware
image (sha256 be0d5e19...; both hosts skip the upload because the version
matches, so both boot the card's own flash). The same rootfs, the same opcode,
the same 44-byte struct - and the value the retail driver uses kills our
capture loop before it renders anything.

So the difference is card STATE, not card CODE, and something in the state
Windows establishes and we do not is what makes 6/7 viable there. The obvious
candidate is the one M83 just found: **I2C 0x98, fitted, ACKing, written 181
times by the retail driver and never once by us.** A colour-space converter is
exactly the kind of thing that would make the difference between a YV12 and a
YUY2 capture path working, and it sits in the video path where the VIC's
no-signal verdict is produced.

**Method note.** Byte-for-byte parity with the retail driver is a hypothesis
generator, not a rule. Six changes went in together on the strength of "this is
what Windows does"; five were right and one was actively harmful, and the run
that found it was the CONTROL - re-establishing the known-good baseline on the
hardware actually in front of us - not any of the bisect steps.

**M89 (tooling, cost one hardware run).** The first attempt at the M88 baseline
went out at `fw=7` despite the driver default having just been corrected to 5,
because `mz0380-m55-real-capture.sh` hardcoded `vic_fw="${VICFW:-0}"` - and
after M88, `0` means "use the Windows rule", i.e. 7 at 60 fps. The script
silently overrode the default it was supposed to be testing.

The script now builds its optional arguments with `add_opt`, which appends a
knob **only when the caller actually set the environment variable**, so the
driver's own default governs in every other case. Any `"${VAR:-<literal>}"`
default in a test harness is a second source of truth for a value that already
has one, and it goes stale the moment the driver changes.

(The run itself was not misleading, just redundant: full Windows sequence +
`fw=7` -> nothing written, consistent with M88.)

### M90 CORRECTION (hardware, 2026-08-20): M87 and M88 both overclaimed - ordering
### matters too, and the two interact

Full Windows sequence with `fw=5` (verified `fw=5` on the wire this time, after
the M89 harness fix): **0/1024, no splash.**

    run          order   fw   m   cosmetics   irq    buf0
    M84          new     7    0   new         INTx   nothing
    fw bisect    new     6    0   new         INTx   nothing
    M86 control  old     5    1   old         MSI    SPLASH
    M87          old     7    0   new         INTx   nothing
    M88          old     5    0   new         INTx   SPLASH
    M90          new     5    0   new         INTx   nothing

M88 and M90 differ in `win_seq` alone. M87 and M88 differ in `fw` alone. So
**both are necessary**: the splash appears only with `fw=5` AND the pre-M82
ordering. Neither variable alone explains anything.

**Where the reasoning went wrong.** M87 concluded "the regression is in the
VALUES, not the order" from a single run that reverted the ordering while
`fw=7` was still set - and `fw=7` independently stops the capture loop. A
variable cannot be cleared by a run in which another variable is already fatal.
M88 then inherited the error and stated that the entire ordering half of M82
was innocent. Both claims are withdrawn. What survives from M88 is only the
narrower fact that, holding the old ordering fixed, fw=5 works and fw=7 does
not.

This is the same confounding mistake as the original six-at-once change, in a
subtler form: a bisect step is only informative if every OTHER variable is at a
value already known to permit the outcome being measured.

**Remaining bisect, now on the ordering, with fw=5 held at its working value.**
`win_seq` bundles six changes:

  1. pre-STOP (0x07 with 0xFFFFFFFF) plus the 1.9 s settle
  2. SET_BUF before SET_VIC          (`win_bufs_first`)
  3. SET_AIC before the encoder      (vs the encoder first)
  4. the sub-stream SET_ENC_PARAMS   (`enc_sub`)
  5. POST_PROC (0x31)
  6. no START_STREAMING (0x06)       (`win_start_op6`)

6 and 2 are the two with hardware-proven prior evidence against them - M22
showed tinyvenc blocks on epint waiting for op6, and M23 placed SET_BUF after
SET_VIC so that op6's iATU latch would see our addresses. Test 6 first
(`OP6=1`), then 2 (`WINBUFS=0`).

### M91 (hardware, 2026-08-20): op6 is necessary but NOT sufficient - and the
### failure mode is a 16-byte stall, not silence

`OP6=1`, `win_seq=1`, `fw=5` (default). New outcome, distinct from both previous
ones:

    stop buf[0] head=11 11 11 11 10 10 11 11 11 11 11 11 11 11 11 11 | 1/1024 pages touched
    buf0 dump  +0x00: 11 11 11 11 10 10 11 11 11 11 11 11 11 11 11 11
               +0x10: aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa

**Sixteen bytes of splash pixel data, then untouched poison.** The transfer
started and died immediately. For comparison, M88's full splash was 760/1024
pages, ~3.1 MB, same 0x11 background.

    run                       order  fw  op6  bufs   buf0
    M88                       old    5   yes  after  SPLASH (760/1024)
    M90                       new    5   no   first  nothing (0/1024)
    M91                       new    5   yes  first  16 BYTES (1/1024)

So op6 is required - Windows genuinely does not need it, we do - but restoring
it alone does not restore the stream. Something else in `win_seq` truncates the
DMA after one 16-byte burst.

M23 predicted exactly this shape: START (op6) is what makes vpl_dmac latch
channels[] into the outbound iATU, and SET_BUF was deliberately placed AFTER
SET_VIC so the addresses latched would be ours. Under `win_bufs_first=1` we
program the buffers before SET_VIC, so the spawn sits between SET_BUF and the
latch. A first burst landing correctly and everything after it going elsewhere
is what a stale or half-updated iATU target looks like.

Next: `OP6=1 WINBUFS=0` - one variable from M91, and every other variable is at
a value already known to permit writing, so the step is informative either way.

### M92 (full read of collect-2026-08-19, 2026-08-20)

Everything in the Windows collection that bears on the capture path has now been
read rather than skimmed. Three results, one of which retires a lead of my own.

#### 1. The buffer-registration command shape, byte-exact

`funcinfo` puts every one of them inside a single 6987-byte function,
`0x14027aaa8..0x14027c5f3`. Extracting each doorbell-to-SEND_COMMAND block:

    0x14027af84  op=r15d  ch=ebx   size=0xCA900   count=0xC
    0x14027b080  op=3     ch=r12d  size=r15d      count=0xC
    0x14027b111  op=4     ch=ebx   size=r15d      count=0xC
    0x14027b1f2  op=5     ch=ebx   size=edi       count=0xC
    0x14027b2e8  op=r15d  ch=ebx   size=0x10F000  count=0xC
    0x14027b3e9  op=3     ch=r12d  size=r15d      count=0xC
    0x14027b47a  op=4     ch=ebx   size=r15d      count=0xC
    0x14027b55b  op=5     ch=ebx   size=edi       count=0xC
    0x14027b62d  op=2     ch=0     size=0x466000  count=0xC
    0x14027b752  op=8     ch=0     size=same      count=0xC
    0x14027b787  op=3     ch=r15d  size=0x2000    count=0xC

Identical shape throughout: `word[2]` = channel, **`word[3]` = buffer SIZE IN
BYTES**, `word[4..11]` = four {hi,lo} pairs, count 12. High dword first, which
is what we already send.

**`word[3]` is a size, not a stride.** M27 named it "stride" and concluded the
card ignored it. The sizes decode exactly:

    0x466000 = 2048 x 1125 x 2   + 4096
    0x34BD00 = 2048 x 1125 x 1.5 + 256
    0x10F000 = 1024 x  540 x 2   + 4096
    0x0CA900 = 1024 x  540 x 1.5 + 256

frame bytes plus a small header, at a power-of-two stride, height = vtotal not
active lines. That also identifies the third region in
`[MEMORY] [00466000] [0034BD00] [0034BD00]`, which the collection had left
unexplained: it is this command's size word. Our value (the real size of the
buffer we allocate) was right all along; only the name was wrong.

#### 2. op 0x02 and op 0x08 are a PAIR, and we send half of it

Windows issues `0x08` immediately after `0x02` - back to back at
`0x14027b62d` / `0x14027b752`, same channel, same size, four more address
pairs - and never one without the other. ep.ko treats them asymmetrically:
op2 fills window0 slots 1..4 and **clears** `host_ready` (G[0]); op8 fills
slots 5..8 and **sets** `wency_ready = 8`. For the whole life of this driver we
have sent only the one that clears a ready flag.

Added as `set_buf_op8`, default 0 - the M88 baseline renders the splash without
it, and after M84 an unmeasured change does not get to be a default.

Note `probe_windows` (M32) already covers ops 0x04/0x05/0x03 but never included
0x08, so this specific pairing had not been tested.

Also confirmed from trace 1: `[CAPTURE PIN]` + `START [0] [0]` at 19:33:24.887,
`[CH00]` at 19:33:25.607 - buffers really are registered ~0.7 s BEFORE SET_VIC,
which is what `win_bufs_first=1` does.

#### 3. RETRACTION: the 0x98 lead is much weaker than I claimed

`dbgprint-log-strings.md` shows this one binary serves many boards - it is full
of **NVP6134** (Nextchip analog AHD/CVBS decoder, 36 distinct error strings),
**TECHPOINT** (TP28xx analog), and **SA7160** (analog tuner) paths. The I2C
transaction trace is STATIC, so it spans every one of those boards, and the
"233 transactions on 0x98" figure counts call sites that are gated on board IDs
this card does not have.

On top of that, `csc-matrices-and-format-table.md` shows the two matrices the
driver actually sends to 0x98 (E4, E6) are **YCbCr->RGB**. A chip being handed
YCbCr->RGB coefficients sits on a path that ends at a DISPLAY - the card's HDMI
passthrough output - not between the MST3367 and the SoC's BT1120 input, which
carries YCbCr and needs no such conversion.

M83 remains true: something IS fitted at 0x98 on this board and answers with
dense varied data (54/64 registers non-zero), and we have never written it. But
"0x98 is where the capture blocker lives" was my inference from a transaction
count, made before reading what the transactions were, and it does not survive
the reading. Recorded as a retraction, not a refinement.

#### What the read did NOT turn up

No missing opcode in the capture path, no missing SET_VIC field, no host-side
step between detection and streaming that we skip. The `0x02`/`0x08` pairing is
the only concrete host-side gap the whole collection revealed.

### M93 RESULT (hardware, 2026-08-20): op 0x08 is neutral

`WINSEQ=0 OP8=1`. The command is accepted (`SET_BUF_8 ... ret=0`) and the result
is byte-identical to the M88 baseline: buf0 760/1024 pages, head
`11 11 11 11 11 10 11 11`, last @0x2f7000; bufs 1-3 untouched; EVENT, token and
enc_stat all zero; `frame_events=0`.

So the `host_ready` / `wency_ready` asymmetry between op2 and op8 is **not** a
gate on the frame path. The only concrete host-side gap the entire Windows
collection revealed is now tested and negative. `set_buf_op8` stays at default 0.

**Where that leaves the host side.** Everything the Windows collection could
give has been extracted and applied or eliminated:

  - capture-start sequence, byte-exact        -> applied, and it needs op6 added
                                                 back plus fw=5 to work at all
  - SET_VIC field values                      -> applied (4 of 5); fw is ours
  - SET_AIC aic_int_mode                      -> applied
  - POST_PROC 0x31, sub-stream 0x2d           -> applied, neutral
  - legacy INTx                               -> applied, neutral (and exposed
                                                 a real teardown bug, M84)
  - 64-bit DMA mask                           -> already correct
  - EDID delivery                             -> DEAD, Windows never pushes one
  - HDCP                                      -> DEAD, DSLR captures on Windows
  - firmware version/image                    -> identical to Elgato's
  - buffer-registration shape and size word   -> already correct (M92)
  - op 0x02 / 0x08 pairing                    -> tested, neutral (this entry)
  - I2C 0x98                                  -> retracted as a lead (M92)

And the blocker is unchanged: the receiver holds a clean lock, every command is
accepted, the encoder spawns and its capture loop runs - and the SoC's VIC
reports no signal on BT1120. That verdict is produced by VIC hardware
(MMR+0x30) and no host-side setting reaches it, which is what M76-M80 already
established and what this whole session's host-side work has now re-confirmed
from a second, independent direction.

**The one unexplained divergence worth carrying forward** is fw. Windows sends
6 (=> cfg output format 2/YUY2) or 7 (=> tinyvenc7); both make our VIC init fail
before it writes a byte, while 5 (=> YV12, tinyvenc5) works. YUY2 is packed
4:2:2, the natural format for a BT1120 bus, and YV12 requires a chroma
downsample - so the card that cannot do the EASY one is telling us something
about its VIC configuration. Nothing host-side can read that back.

**Next instrument, not next parameter.** The remaining question is *why* the VIC
says no signal, and the only thing that can answer it is the card's own console:
`video_capture_mgr` op 0x6e (LOAD_FILES) is an arbitrary-offset read of
/mnt/flash/PIC_ENC, M77 proved the transport works from the host, and M81 built
a one-line firmware change that redirects the card's stdout into that file. It
is built and NOT installed. That is the next move, and it is the same conclusion
M80 reached before the Windows detour - which was itself worth taking, since it
closed EDID, HDCP, the sequence and the interrupt model for good.

### M94 RESULT (hardware, 2026-08-20): the Windows output-stage block does not
### rescue b0=0x14 - but the negative locates the failure

`WINSEQ=0 MSTOUT=1 VICB0=0x14`, with `MSTAD=0` and `MSTAD=1`. Both runs:
0/1024 on every buffer, no splash. The block landed (`Windows block applied
(b0=14 ae|=04 ad=00 b1=c0 b2=00 b3=00 b4=54) ret=0`) and read back correctly
(`b0=14 b1=c0 b2=00 b3=00`), so this is a real negative, not a failed write.

M80's isolated result stands: 0x14 is worse than 0x21, and the three registers
we were missing (0xad, 0xae, 0xb4) do not change that. `mst_win_output` stays
default 0.

**What the negative proves, which is more than the positive would have.**
`b0` decides how far the card gets:

    b0 = 0x21  -> splash rendered      => VIC initialised, capture loop ran
    b0 = 0x14  -> nothing written      => failed at VIC init

A RECEIVER OUTPUT register deciding whether the SoC's VIC initialises means the
VIC is looking at the BT1120 clock. If the receiver were not driving that bus,
0x21 would fail exactly the way 0x14 does. **So at b0=0x21 the MST3367 IS
clocking pixels onto BT1120, and the VIC is rejecting what arrives.**

That reclassifies the blocker. The three VIC verdicts are:

    (s & 0x14) == 0x14  "No signal !!"                  <- what we assumed
    (s & 0x17) == 0x10  "(CCIR or width(%lu) chck fail)" <- what the evidence fits
    (s & 0x12) == 0x12  "FIFO full (error frame)"

We have been reasoning about the first for the whole project. The evidence -
clock present, capture loop running, zero valid frames - fits the second:
embedded sync present but the CCIR structure or the line width is not what the
VIC was told to expect. Note the message names WIDTH specifically, and that the
VIC's own width register comes from SET_VIC bytes 24..27, swept in M76 without
success (including 3840 for the 8-bit double-rate theory).

The card's splash is a NOSG fallback: tinyvenc5's `is_nosg` /
`NOSG_LOGO_YUV422` / `/tmp/PIC_NOSG` path renders it precisely BECAUSE the
capture found nothing usable. So "capture mode never triggers" is not what is
happening - it triggers, initialises, finds nothing it will accept, and falls
back. The user's other reading, that the card never gets usable data from the
capture chip, is exactly right.

**Consequence for the next step.** Which of the three verdicts fires is the
single fact that would direct everything after it, and it exists only in the
VIC's MMR+0x30 on the card, reachable only from the card's own console. That is
now the only remaining instrument, and M77 already proved its transport.

### M95 (2026-08-20): the vic_b0=0x20 test was confounded by a bad DEFAULT

`sudo VICB0=0x20 scripts/mz0380-m55-real-capture.sh 4` returned 0/1024, no splash -
and the result is worthless, because `win_seq` still defaulted to 1. The log
shows it plainly: pre-STOP -> SET_VIC -> SET_AIC -> 2x SET_ENC -> POST_PROC, no
op6. M90 had ALREADY established that `win_seq=1` renders nothing whatever else
is set. The register did land (`b0=20` reads back in all three output-stage
diagnostics); the run simply could not have produced a splash for any value of
b0.

Third instance of the same error in one session: the six-at-once M82 change,
M87 "clearing" the ordering with `fw=7` still set, and now this. The first two
were reasoning mistakes. **This one was a packaging mistake, and worse for it:**
the driver's own defaults were a configuration known not to reach the splash,
so any test run "with defaults" was silently confounded - including a test whose
whole purpose was to be a clean single variable.

**Fix: `win_seq` now defaults to 0.** Defaults track the best known-working
configuration. `win_seq=1` remains available for work on the Windows ordering,
but it must now be asked for. A default that cannot reach the measurement
oracle is a trap, not a preference.

The `vic_b0 = 0x20` question is therefore still **OPEN and untested**. Rerun on
the corrected defaults:

    sudo VICB0=0x20 scripts/mz0380-m55-real-capture.sh 4

with the control immediately before or after it:

    sudo scripts/mz0380-m55-real-capture.sh 4        # must give 760/1024

---

### M96 RESULT (hardware, 2026-08-20): vic_b0=0x20 is NEGATIVE - and bit0 is
### finally identified as an output clock-rate / bus-width select

Run, on a card freshly cold-booted at mains (spawn budget reset), with a
verified control immediately before it. Every other knob confirmed at its
known-good value *from the source*, not from memory: `vic_fw=5`, `win_seq=0`,
`win_bufs_first=1`, `enc_sub=1`, `mst_win_output=0`, `irq_intx=1`.

    sudo scripts/mz0380-m55-real-capture.sh 4               # control
    sudo VICB0=0x20 scripts/mz0380-m55-real-capture.sh 4    # the test

**Control reproduced the baseline exactly**: `760/1024 sampled pages touched,
last @0x2f7000`, head `11 11 11 11 11 10 11 11`, bufs 1-3 untouched, `R55=0x7f`
held for the whole 60 s, every command `ret=0`, `b0=21` read back at all three
diag points.

**The test: `b0=20` read back at all three diag points (the knob landed), and
the splash STOPPED.** `0/1024` on every one of the four buffers, buf0 still
solid `aa` poison end to end. `0x20` behaves exactly like `0x14`: it fails
*before* the VIC ever initialises, whereas `0x21` at least reaches VIC init and
lets tinyvenc5 render its NOSG fallback.

So the hdcapm lead is closed. `0x21` remains the only value of the three that
gets anywhere, and the table is now complete:

| value | source | result |
|---|---|---|
| `0x21` | ours | reaches splash - the ONLY value that does |
| `0x14` | Windows driver | nothing written (M80, M94) |
| `0x20` | hdcapm, "YUV422 / 8-bit output" | nothing written (M96) |

#### The real yield: what bit0 actually does

Our own source comment has said for months that `0x20` and `0x21` "differ in
bit0 alone and bit0 is unidentified". This run identifies it, from a side
effect nobody was watching - the receiver's own timing counters.

Control, all three polls:

    hper=674 vper=599 lines=1125

`b0=0x20`, first poll (still 674) and then from the second poll onward:

    hper=337 vper=299 lines=1127

`674 -> 337` and `599 -> 299` are both exactly half. Those two fields are not
raw registers: `mst3367_measure_once()` derives them from raw counters against
an internal reference (`vperiod = 1250000/raw`, `hperiod` off a 160 MHz
reference - see the comment at mz0380-mst3367.c:1523). A displayed value that
halves means the **raw counter doubled**, i.e. the reference the counters run
on doubled, while the source in front of the card did not change (`htot=2200`,
`hact=1920`, and the v4l2 modeline all identical across both runs).

That is consistent with exactly one reading, and it is hdcapm's own comment:

* `b0` bit0 = 1 -> 16-bit output, one sample per clock, 1x pixel clock.
* `b0` bit0 = 0 -> **8-bit multiplexed** output, two bytes per sample, **2x**
  clock. Hence hdcapm calling `0x20` "YUV422 / **8-bit** output".

The change appearing at the *second* poll, after `START_STREAMING`, not at the
first, fits: the retimed output clock only comes up once the output stage is
actually running.

**Consequence for the width hypothesis.** In 8-bit mode the VIC would see 3840
bytes per active line where it was told 1920 - the precise shape of the
`(CCIR or width(%lu) chck fail)` message. But the failure at `0x20` is the
*harder* one (`0x14`-like, nothing written at all), not the splash-with-no-frame
one. So `0x21` is already the correct bus width for our VIC config, and the
width half of that ISR message is very likely NOT our failure. **That shifts
the weight of the M94/M95 hypothesis onto the CCIR half** - the embedded SAV/EAV
timing-reference codes.

#### The next test this points at, and why M94 did not already run it

Our driver has **no SAV/EAV or embedded-sync handling anywhere** (grep for
sav/eav/656/ccir in mz0380-mst3367.c returns only prose). If the receiver emits
separate H/V sync rather than embedded CCIR timing codes, the VIC sees a clock
and rejects the structure - splash, no frame. That is our exact symptom.

The Windows output-stage block writes three registers we never touch:
`0xae |= 0x04`, `0xad`, `0xb4 = 0x55 & ~0x03 = 0x54`. Any of those is a
candidate for the sync-structure select.

M94 tested that block **only paired with `vic_b0=0x14`**, because that is what
Windows sends. But `0x14` independently kills the splash - so M94 scored a new
block against a knob already known to sit at a value that cannot produce the
outcome being measured. That is method rule 1, violated a fourth time, in the
opposite direction: not a bad default, a bad deliberate pairing.

The block has never been tried at the only `b0` that works:

    sudo MSTOUT=1 scripts/mz0380-m55-real-capture.sh 4     # vic_b0 stays at its 0x21 default

`mst_win_output=1` writes `b0` as a plain write of `vic_b0` (0x21), where the
default path writes `(b0 & 0xc2) | (vic_b0 & 0x3d)`. Hardware `b0 & 0xc2` reads
0 on this card in both runs above, so both paths put the same 0x21 on the wire:
the **only** deltas against the known-good baseline are `0xae |= 0x04`,
`0xad = 0`, `0xb4 = 0x54`. A clean single-block bisect.

`MSTAD=1` is the follow-up if `MSTAD=0` (the default) is neutral.

#### Harness fix made in the same session (method rule 4, again)

`mz0380-m55-real-capture.sh` still hardcoded five knobs directly in its insmod
line: `vic_in_w=0 vic_in_h=0 vic_in_fmt=0 vic_b0=0x21 set_buf_opcode=2`. All
five happened to equal the driver default, so nothing was confounded - but this
is the identical trap that burned M89, sitting in the identical file, one knob
over. All five now go through `add_opt`, so the driver default governs unless
the caller sets the env var. `bash -n` clean; no behaviour change today.

---

### M97 (2026-08-20): the firmware-upload path is deleted from the tree, not
### merely disabled

Standing project constraint, now enforced by absence rather than by discipline.
Previously the upload code was removed but its *interface* survived: an inert
`firmware_upload` module parameter kept for script compatibility, the four
download opcodes and the BAR0 blob aperture still defined in `mz0380-reg.h` as
"RE documentation", the `.HEX` blob names still in the board table and in
`MODULE_FIRMWARE`, and a neutered `m81` rootfs-repack script. All of that is
gone:

* `mz0380-core.c` - `firmware_upload` param and its `MODULE_PARM_DESC` deleted
  (`modinfo` confirms absent); both `MODULE_FIRMWARE("mz0380/MZ038x.HD.HEX")`
  replaced with the single `mz0380/MZ0380.FW.TXT` the driver actually reads;
  the `/proc` state line now says "card's own flash image (host never uploads)"
  instead of printing a blob filename.
* `mz0380.h` - `extern bool mz0380_firmware_upload_enabled`,
  `struct mz0380_board.firmware_name`, `.firmware_base_name`, and
  `MZ0380_FW_STATE_UPLOADING` removed; the state-machine comment corrected (it
  still claimed the card boots "from the uploaded blob").
* `mz0380-reg.h` - `MZ0380_MB_FW_BUFFER`, `MZ0380_CMD_BEGIN_FW_DL`,
  `MZ0380_CMD_COMMIT_FW`, `MZ0380_CMD_BEGIN_BASE_FW_DL`,
  `MZ0380_CMD_COMMIT_BASE_FW` and the aperture-protocol prose all deleted, with
  a comment left in their place marking 0x0b/0x0c/0x0e/0x0f as deliberately
  undefined.
* `mz0380-cards.c` - blob filenames dropped from both board entries.
* `mz0380-fw.c` - header no longer points at reg.h for the protocol; the stale
  "watched during a firmware-upload attempt" comments reworded.
* Scripts - `firmware_upload=1` stripped from **32** files;
  `mz0380-correlation.sh` lost its `--firmware-upload` flag, its usage lines and
  a dead `insmod_args+=()` branch; `mz0380-m0m2-test.sh` lost its `m2` stage,
  which *was* the upload attempt; `mz0380-bringup.sh`'s presence check moved
  from the `.HEX` blob to the `FW.TXT` sidecar; the misleading
  "waiting for firmware upload + boot..." banners now say the card is booting
  its own flash image; `mz0380-m81-build-cardlog-fw.sh` deleted outright.

`mz0380-re-dump.sh` is deliberately untouched: it unpacks the blob read-only on
the host to read tinyvenc5 for RE. Reading the image is not sending it.

Builds clean, 99 module params remain, `bash -n` passes on every script.

### M98 (2026-08-20, observation): OBS sees the device and shows a black canvas

Reported while an `m55` run was in flight: OBS lists the Elgato/PCIe device but
renders nothing. Consistent with everything else - `captured 0 bytes`, `0/1024`
or splash-only pages - and it does add one small thing: two independent V4L2
consumers (m55's capture command and OBS) both get zero frames, so the failure
is not something specific to how `m55` opens or negotiates the node.

**But do not run OBS concurrently with `m55` again.** It is a second opener on
`/dev/video0` competing for streaming state and buffer ownership, i.e. exactly
the kind of uncontrolled variable method rule 1 is about. If OBS is wanted as a
consumer, run it *instead of* the script's capture step, never alongside it.

---

### M99 RESULT (hardware, 2026-08-20): the Windows output-stage block is NEUTRAL
### at b0=0x21 - and it accidentally bisected 0xad as well

    sudo MSTOUT=1 scripts/mz0380-m55-real-capture.sh 4

Block applied and logged: `b0=21 ae|=04 ad=00 b1=c0 b2=00 b3=00 b4=54 ret=0`.
Result **bit-identical to the baseline**: `760/1024 sampled pages touched, last
@0x2f7000`, head `11 11 11 11 11 10 11 11`, bufs 1-3 untouched, `R55=0x7f` held,
every command `ret=0`. So `0xae |= 0x04`, `0xad` and `0xb4 = 0x54` are all
neutral to the splash oracle. That closes the M94/M95 lead: pairing the block
with a working `b0` does not rescue it, so the block was never the problem and
`0x14` really is fatal on its own.

The run also settled `0xad` for free, in both directions, by accident. Our
`RxHdmiInit` already writes `0xad = 0x05` ("enable low-pass filter",
mz0380-mst3367.c:333); the `mst_win_output` path then **overwrites** it with
`mz0380_mst_ad`, which defaults to 0. So the MSTOUT=1 run was the low-pass
filter OFF and the baseline is it ON, and the two are indistinguishable. Since
`0xad` was the best candidate for the "find the register that forces
4:4:4 -> 4:2:2 on the output" follow-up, that follow-up is now weak.

Caveat recorded and fixed: `mst_wr()` returning 0 is NOT evidence a write
landed - the card's firmware forces write ops to result 0x00, so a NAK is
invisible. `0xad`/`0xae`/`0xb4` were never read back. `mz0380_mst3367_output_diag()`
now reads all three, so every future run confirms them at no cost. On the next
`MSTOUT=1` run they must read `ad=00 ae=<bit2 set> b4=54`; if they do not, this
verdict needs re-scoring.

Also confirmed in the same pair of runs: the driver loads and streams normally
with the `firmware_upload` parameter deleted (M97).

### M100 (2026-08-20): BANK0 0xb0 is FULLY DECODED, and M96's reading of bit0
### was wrong

Pulled `stoth68000/hdcapm` (`mst3367-drv.c`, 1154 lines - same receiver, values
taken off a vendor I2C trace). At the end of its init function, three
commented-out alternatives sit above the live write:

    //mst3367_set(sd, BANK0, 0xB0, 0x25 ); /* RX_OUTPUT_YUV422 / 10.BITS / EXTERNAL SYNC */
    //mst3367_set(sd, BANK0, 0xB0, 0x21 ); /* RX_OUTPUT_YUV422 / 08.BITS / EMBEDDED SYNC */
    //mst3367_set(sd, BANK0, 0xB0, 0x24 ); /* RX_OUTPUT_YUV422 / 10.BITS / EXTERNAL SYNC */
    mst3367_wr(sd, BANK0, 0xB0, 0x20 );    /* RX_OUTPUT_YUV422 / 08.BITS / EXTERNAL SYNC */

Reading the four against each other:

| bit | meaning |
|---|---|
| bit0 (0x01) | **EMBEDDED sync (1) vs EXTERNAL sync (0)** |
| bit2 (0x04) | **10-bit (1) vs 8-bit (0)** |

(0x24 and 0x25 carry the same comment text; by the bit rule 0x25 is the
EMBEDDED one and hdcapm's comment is a copy-paste slip.)

**This corrects M96.** M96 inferred from the receiver's period counters halving
that bit0 was a bus-width / clock-rate select. The register-level decode from a
vendor-derived source beats that inference: bit0 is the **sync mode**. The
halving is a side effect of switching to external sync, not the definition of
the bit. The rest of M96 stands - `0x20` is negative, the knob landed, the
splash stopped.

**And the decode explains every b0 result we have.** Both values that write
nothing at all - Windows' `0x14` and hdcapm's `0x20` - have bit0 = 0, EXTERNAL
sync. The single value that gets the VIC to initialise, `0x21`, is the
EMBEDDED-sync one. The SoC's VIC wants CCIR timing codes in-stream. So on the
`(CCIR or width chck fail)` message we are on the right side of the CCIR half,
and the **width** half is back in play - the opposite of what M96 concluded.

#### The next test: `vic_b0 = 0x25`

`0x25` is `0x21` **plus bit2**: one bit off the only value known to reach VIC
init, and the bit hdcapm labels 10-BITS. BT.1120 is natively a **20-bit**
interface (10-bit Y + 10-bit C); 8-bit is the reduced BT.656-style variant.
Feeding a 10-bit VIC an 8-bit stream is precisely a width/structure mismatch.

    sudo scripts/mz0380-m55-real-capture.sh 4               # control
    sudo VICB0=0x25 scripts/mz0380-m55-real-capture.sh 4

`0x25 & 0x3d = 0x25`, so the mask in `commit_digital_output()` passes it intact;
confirm `b0=25` in the output-stage diag before reading anything else. Note that
Windows' `0x14` already has bit2 set - it is 10-bit/external - so 10-bit is not
an exotic choice on this board, only the sync mode was ever wrong about it.

Follow-ups, in order: `0x24` (10-bit external, weaker - external sync has failed
twice); then `b1/b2 = 0xe0/0x08`, the sibling-board values hdcapm uses where we
write `0xc0/0x00` from the HD60 Pro's own `FUN_14024dc28`.

---

### M101 RESULT (hardware, 2026-08-20): vic_b0=0x25 is NEUTRAL - 10-bit changes
### nothing - but the new readback catches a write that never landed

    sudo VICB0=0x25 scripts/mz0380-m55-real-capture.sh 4

`b0=25` read back at all three diag points. Output **bit-identical to the
baseline**: `760/1024 sampled pages touched, last @0x2f7000`, head
`11 11 11 11 11 10 11 11`, bufs 1-3 untouched. So bit2 (10-bit vs 8-bit) is
neutral to the splash oracle, and the `0xb0` table is now:

| value | = | result |
|---|---|---|
| `0x21` | 8-bit, embedded | splash (baseline) |
| `0x25` | 10-bit, embedded | splash, bit-identical (M101) |
| `0x20` | 8-bit, external | nothing written (M96) |
| `0x14` | 10-bit, external + bit4 | nothing written (M80, M94) |

Clean separation on bit0 alone: **embedded sync reaches VIC init, external sync
does not, and the bit-depth bit does not matter either way.**

#### The readback earned its keep on its first run

The diag line now reads `ab=15 ad=05 ae=20 b0=25 b1=c0 b2=00 b3=00 b4=54`.

`0xae = 0x20` - **bit2 clear** - on a run where `RxVideoInit` executes
`mst_set(dev, 0xae, 0x04)`. Meanwhile `0xad = 0x05` and `0xb4 = 0x54`, written
by the same helper two lines below in the same bank, both read back exactly as
written. The bus is fine; something specific to `0xae` bit2 is going on.

**So M99's "0xae |= 0x04 is neutral" is withdrawn.** That register was never in
the state we thought we were testing. It is UNTESTED, not neutral. Added an
immediate read-back-and-log right after the init write to separate the two
explanations: write-only/self-clearing strobe (would read 0x24 immediately, 0x20
later) versus rejected write (reads 0x20 immediately).

Also corrected: the M94 comment at mz0380-mst3367.c claiming `0xae/0xad/0xb4`
sit "still at power-on" outside the Windows block. They do not - `RxVideoInit`
writes all three, which the readback proves for two of them.

### M102 (2026-08-20): the VIC is being told BT1120p while the receiver emits a
### CCIR656-shaped stream

Every sweep for months has been on the **receiver** side of the BT1120 link.
The host side of the same interface is one byte of SET_VIC and has never been
touched.

`mz0380-dma.c:1116`: `in_fmt = mz0380_vic_in_fmt ?: (interlaced ? 7 : 6)`, and
the enum is quoted in our own source from the card's SDK capture config
(`re-dump/fw/yuan_demo_sdi/nullsensor_1920x1080.cfg`):

    input format (1:8-bits Raw, 2:CCIR656i, 3:CCIR656p, 4:Bayer,
                  5:16-bits Raw, 6:BT1120p, 7:BT1120i)

We send **6 = BT1120p**. But `b0 = 0x21` is, per hdcapm's own comment,
`RX_OUTPUT_YUV422 / 08.BITS / EMBEDDED SYNC`. An 8-bit multiplexed YCbCr stream
with embedded timing codes is **CCIR656**, not BT1120 - BT1120 is the wide
(16/20-bit) HD interface. And the VIC ISR's complaint is, verbatim,
`(CCIR or width chck fail)`.

So the two ends of the link are configured for different structures, which is
the exact shape of the failure, and it is a one-knob test:

    sudo scripts/mz0380-m55-real-capture.sh 4                 # control
    sudo VICINFMT=3 scripts/mz0380-m55-real-capture.sh 4      # CCIR656p

`vic_b0` stays at its `0x21` default, so the pairing is coherent: receiver says
8-bit embedded sync, VIC is told CCIR656p. Confirm `in_fmt=3` in the SET_VIC
line before reading anything else.

This also revises M100's closing claim. M100 argued that because we sit on the
embedded-sync side, the CCIR half of the message was satisfied and the width
half was the open question. Both halves are in play: 656-vs-1120 is a
*structure* difference that would trip either check.

Follow-ups if `in_fmt=3` is neutral:

1. `VICINFMT=3` with `VICB0=0x25` - 10-bit 656.
2. `VICINFMT=2` (656i) as a control on the enum itself - it should behave
   *differently* from 3 on a progressive source; if 2 and 3 are
   indistinguishable, the field is not reaching the VIC at all and that is the
   finding.
3. `b1`/`b2` = `0xe0`/`0x08`, hdcapm's sibling-board values against our
   `0xc0`/`0x00`.

---

### M103 (2026-08-20): SET_VIC byte7 is the INTERLACE FLAG, we have sent 6 for a
### progressive source for the whole project, and 0 was UNREACHABLE

`VICINFMT=3` ran and landed (`in_fmt=3` in the SET_VIC line) and was
**bit-identical to the baseline** - `760/1024`, `last @0x2f7000`, same head. So
CCIR656p is neutral, exactly like BT1120p.

That null result is the clue. Reading our own source for why, there is a
contradiction sitting inside a single comment block in `mz0380-dma.c`:

* The authoritative field map, from **M23/M71 - disassembling
  video_capture_mgr's op-41 handler and decoding the card's own printf**
  (`"[Video_MGR][ch%d] SET_VIC fw(%d), fps(%d), resolution(%dx%d)
  interlace(%d), m(%d), ..."`) - says `[4]=ch [5]=fps [6]=fw [7]=interlace`,
  and states that byte7 "is the INTERLACE FLAG, which is passed straight into
  the encoder's argv".
* **M72/M76 then overrode that** with the enum from
  `re-dump/fw/yuan_demo_sdi/nullsensor_1920x1080.cfg` - "input format
  (1:8-bits Raw, 2:CCIR656i, 3:CCIR656p, 4:Bayer, 5:16-bits Raw, 6:BT1120p,
  7:BT1120i)" - and sent 6/7 derived from the detected scan.

**The config file describes a different struct.** It is the SDK demo
application's capture config, not the SET_VIC mailbox command whose handler M71
actually disassembled. A disassembly of the real handler beats a text file that
happens to contain a plausible-looking enum. M72's own comment even concedes
"the card's printf calls it interlace" and then argues the label is "merely
loose" - that is the tell.

Consequences:

1. For a progressive 1080p60 source the correct byte7 is **0**. We have sent
   **6** since the driver was written.
2. Every value ever swept here - 3, 6, 7 - is **nonzero**, i.e. "interlaced" to
   the encoder. That is precisely why M102's `in_fmt=3` came back
   bit-identical to `in_fmt=6`, and why byte7 has looked inert all along.
3. **0 was not expressible.** The code was
   `in_fmt = mz0380_vic_in_fmt ?: (interlaced ? 7 : 6)`, so `VICINFMT=0`
   selected the derived default. The one value that matters was unreachable
   through the knob built to sweep this field. The M76 sweep could not have
   found it.

Fixed: `MZ0380_VIC_IN_FMT_AUTO` (`~0u`) is now the "derive it" sentinel and the
parameter defaults to it, so the derived behaviour is byte-for-byte unchanged
while `vic_in_fmt=0` is finally reachable.

    sudo scripts/mz0380-m55-real-capture.sh 4              # control
    sudo VICINFMT=0 scripts/mz0380-m55-real-capture.sh 4   # interlace=0, progressive

Confirm `in_fmt=0` in the SET_VIC line. An encoder told to expect interlaced
fields from a progressive source is a completely sufficient explanation for
"capture initialises, runs, and accepts nothing", which is the symptom we have
had for months.

**Method note.** This is the `?:`-as-default idiom hiding a legal value, which
is the same class of bug as method rule 4 (a harness carrying its own copy of a
default) - a knob that silently cannot reach part of its own range. Worth
grepping for elsewhere: `mz0380_vic_fw ?: (...)` has the identical shape, and
`vic_fw=0` likewise means "auto" rather than 0.

---

### M104 RESULT (hardware, 2026-08-20): byte7 is the FORMAT ENUM after all -
### M103 RETRACTED - but byte7 is now PROVEN CONSUMED

    sudo VICINFMT=0 scripts/mz0380-m55-real-capture.sh 4

`in_fmt=0` in the SET_VIC line, and the **splash STOPPED**: `0/1024` on all four
buffers, buf0 solid `aa`. The first non-neutral result ever obtained from the
host side of the SET_VIC struct.

| byte7 | reading | result |
|---|---|---|
| 3 | CCIR656p | splash |
| 6 | BT1120p (our default) | splash |
| 7 | BT1120i | splash |
| **0** | not in the enum | **nothing written** |

**M103's reclassification is withdrawn.** It argued byte7 is the interlace flag,
so 0 (progressive) should be correct. Under that reading 0 ought to work at
least as well as the 6 we have always sent; instead it is strictly worse, and
worse in the specific way that means tinyvenc never got far enough to render its
NOSG fallback. Under the M72/M76 reading - byte7 is the capture INPUT FORMAT
enum, valid range 1..7 - 0 is not a legal value, `VideoCap` fails to open, the
encoder exits before it can draw anything. That fits exactly. M72/M76 were
right and the SDK config's enum does describe this field.

**What the run does establish, and it is new:** byte7 is *consumed*. Until now
every value tried (3, 6, 7) was indistinguishable, which was equally consistent
with "the field is ignored". 0 behaves differently, so the byte reaches the card
and is acted on. That closes a question that has been open by default.

Kept: `MZ0380_VIC_IN_FMT_AUTO`. The `?:`-as-default idiom genuinely could not
express 0, and making it expressible is what produced this result. The derived
default is byte-for-byte unchanged.

### M105 CORRECTION (2026-08-20): the halved period counters in M96 were a
### SOURCE RE-LOCK ARTIFACT, not a b0 effect

The M104 run - `vic_b0` at its `0x21` default throughout, read back `b0=21` at
all three diag points - produced this:

    detect 55=a3 settling (auto-position on; timing not sampled)
    detect 55=7f LOCKED ... hper=674 vper=599 lines=1125
    detect 55=7f LOCKED ... hper=337 vper=299 lines=1127     <-- halved, at b0=0x21
    detect 55=d6 settling (auto-position on; timing not sampled)
    detect 55=7f LOCKED ... hper=674 vper=599 lines=1125

The exact `hper=337 vper=299 lines=1127` sample that M96 attributed to
`b0 = 0x20`, reproduced at `b0 = 0x21`, bracketed by partial-lock (`55=a3`,
`55=d6`) rows - i.e. during a source re-lock. The m55 script explicitly asks the
operator to power-cycle the source during exactly this window.

So the halving is a transient measurement artifact of re-acquisition, and it has
nothing to do with `0xb0`. M96 built a bus-width/clock-rate theory for bit0 on
that single sample. M100 had already superseded the conclusion via hdcapm's
decode (bit0 = embedded vs external sync); this retires the *evidence* as well.

**Method note.** The sample was taken inside a window the harness itself
destabilises. A measurement made while the operator is being told to unplug the
source is not a controlled measurement, and one sample is not a trend - two
separate failures of method rule 1 in the same observation.

### M106 (static RE, 2026-08-20): the VIC failure test, read out of vpl_vic.ko

Unpacked the card rootfs read-only on the host (`MZ0380.HD.HEX` is a gzip'd tar)
and disassembled `yuan_demo_sdi/drivers/vpl_vic.ko` - ELF32 ARM, **not
stripped**, symbols intact: `VIC_SetSizeToVIC`, `ISR`, `Open`, `Ioctl`,
`VIC_DetectStd`, `VIC_AutoDetectStdTasklet`.

The failure message lives in `ISR` (`.text 0x0fd8`, 4472 bytes) and the test in
front of it is, verbatim:

    1358: and  r0, r6, #23        @ r6 = dwVICMmrStat, 23 = 0x17
    137c: cmp  r0, #16            @ 0x10
    1384: beq  0x1814             @ -> the "(CCIR or width(%lu) chck fail)" printk

confirming `(stat & 0x17) == 0x10` exactly as the handoff has claimed. The
printk's five arguments decode as `ch = r9`, `Index = [[r8+0x68]+0x68]`,
`dwVICMmrStat = r6`, `dwVICMmrCtrl = r4`, and **`width` = a driver local at
`fp-0x4c`** - not a value the host supplies, so it cannot be steered directly
from SET_VIC. The fail path also forces `[r8+0xc0] = 0x30` where the normal path
stores `stat & 0x1f`, and `[r8+0x234]` is a printk rate-limit counter.

Useful consequences:

* The status word is genuine VIC hardware state. There is no host-reachable
  register that changes which branch is taken - only the data actually arriving
  on the bus does.
* `VIC_SetSizeToVIC` (`.text 0x0000`, 340 bytes) is where the expected geometry
  is programmed into the MMR, and it is the right target for the next static
  pass: it will name which MMR fields the width check compares, and therefore
  which SET_VIC bytes reach them.
* The SDK cfg's field order was checked against video_capture_mgr's own SET_VIC
  printf. The printf lists `flip`/`mirror` and has **no "field mode" argument**,
  so the cfg's "field mode (0:two single fields, 1:one interleaved field)" is
  not a byte of this struct. That hypothesis is dead before costing a run.

Everything here was static: **zero encoder spawns, zero hardware runs.**

---

### M107 RESULT (hardware, 2026-08-20): WINBUFS=0 does not move the 16-byte
### stall - but the 16 bytes are NOT a truncated splash

    sudo WINSEQ=1 OP6=1 WINBUFS=0 scripts/mz0380-m55-real-capture.sh 4

`1/1024 sampled pages touched, last @0x0`, i.e. the same "exactly 16 bytes
written" stall M91 found with `WINSEQ=1 OP6=1` alone. Putting SET_BUF back after
SET_VIC (the M23 placement, so the addresses op6 latches into the outbound iATU
are ours) changes nothing. `win_bufs_first` is neutral on this branch.

The 16 bytes themselves are new information:

    splash (760/1024):  11 11 11 11 11 10 11 11 11 11 11 11 11 11 11 11
    this run (16 bytes): 11 11 11 11 10 10 11 11 11 11 11 11 11 11 11 11
                         then aa (poison) from byte 16 on

**Not a prefix of the splash** - byte 4 differs. So the 16 bytes are not "the
splash, truncated at one burst"; they are different data. Whatever the card
emits on this path, it is not the NOSG logo.

Also visible only on this path: `POST_PROC(op 0x31, mask=0x1f, fps=60, di=1)`.
`di=1` is deinterlace-on for a progressive source; it comes from the recovered
Windows sequence, so it is presumably what Windows sends, but it has never been
varied.

The M105 artifact reproduced again here (`hper=337 vper=299 lines=1127` plus a
"vtotal moved during the pass" row, at `b0=21`, during re-lock).

### M108 (static RE, 2026-08-20): the VIC's width test is a 2560 THRESHOLD on a
### number the HOST supplies

`VIC_SetSizeToVIC` (`vpl_vic.ko` `.text 0x0000`, 340 bytes), decoded:

    a8: ldrh r2, [r12, #28]     @ per-channel MMR + 0x1c, LOW halfword
    b0: cmp  r2, #2560          @ 0xA00
    b4: ldr  r2, [r4]
    b8: ldr  r1, [r2, #0x204]
    bc: bhi  0x134              @ width > 2560
    c0: tst  r1, #3             @ width <= 2560:
    c4: ldreq r1, [r2, #0x204]  @   if (MMR[0x204] & 3) == 0
    c8: orreq r1, r1, #1        @     MMR[0x204] |= 1
    ...
    134: and r1, r1, #3         @ width > 2560:
    138: cmp r1, #1             @   if (MMR[0x204] & 3) == 1
    140: biceq r1, r1, #3       @     MMR[0x204] &= ~3

so **`MMR[0x204]` bits[1:0] are a wide-mode select driven purely by the
programmed width, with a threshold at 2560**, and nothing about the arriving
data participates in the decision.

The per-channel block is `base + 0x40 + ch*0x38`; `MMR+0x1c` is built at +0x5c
as `(height + y_start[9:0]) << 16 | (width + x_start[10:0]) & 0xffff`, from the
function's own `r0` (width) and `r1` (height) arguments. Both call sites, in
`Ioctl` at `0x40ac` and `0x4968`, pass `r0 = r10` and `r1 = r7` - a width and a
height the ioctl was handed, i.e. **host-supplied**.

#### Why this re-opens M76

At 1920 the VIC takes the narrow branch and sets `MMR[0x204] |= 1`. If the
receiver's 8-bit 4:2:2 output presents **two bytes per pixel**, a 1920-pixel
line is 3840 samples - over the threshold - and the VIC has been configured for
the wrong mode by a number we chose.

M76 swept `vic_in_w` (SET_VIC bytes 24..27) including 3840 and recorded it
negative. That verdict should not be trusted:

* M76 **predates `win_seq`** (introduced M82) and **predates the `fw=5`
  correction** (M88). The baseline it was scored against was never verified to
  reach the splash - the same defect M95 identified in the first `vic_b0=0x20`
  attempt.
* M76 had no mechanism. It was "try 3840 because 8-bit might double the rate".
  There is now a specific threshold, a specific register, and a specific
  consequence.

    sudo scripts/mz0380-m55-real-capture.sh 4                 # control
    sudo VICINW=3840 scripts/mz0380-m55-real-capture.sh 4     # cross the 2560 threshold

Everything else stays at today's verified defaults (`fw=5`, `win_seq=0`,
`b0=0x21`, `in_fmt=6`). Confirm `vic_in=3840x1080` in the SET_VIC line.

Open question the disassembly has not answered: **which** SET_VIC field becomes
`r0`. `vic_in_w` (bytes 24..27) is the best candidate and the cheap one; the
capture width (bytes 8..9) is the alternative, but it doubles as the H.264
output width, so changing it is not a clean single-variable test. Tracing where
`r10` and `r7` are loaded in `Ioctl` before `0x40ac` would settle it statically
and costs no spawns.

---

### M109 RESULT (hardware, 2026-08-20): vic_in_w=3840 is NEUTRAL under a
### verified baseline - M76's negative now stands

    sudo VICINW=3840 scripts/mz0380-m55-real-capture.sh 4

`vic_in=3840x1080` in the SET_VIC line, and the result is **bit-identical to the
baseline**: `760/1024`, `last @0x2f7000`, head `11 11 11 11 11 10 11 11`.

So SET_VIC bytes 24..27 are **not** the width that reaches
`VIC_SetSizeToVIC`'s `r0`, or the 2560 threshold is not our failure. Either way
M108's proposed test is answered in the negative, and - unlike M76, which ran
before `win_seq` existed and before the `fw=5` correction - this one was scored
against a baseline verified minutes earlier on the same source. **M76's negative
is now trustworthy.** Do not sweep `vic_in_w` again.

The remaining candidate for `r0` is the capture width (bytes 8..9), which
doubles as the H.264 output width, so it is not a clean single-variable test.
Settling it statically - tracing where `r10`/`r7` are loaded in `Ioctl` before
the call at `0x40ac` - is still the zero-spawn option.

### M110 (2026-08-20): the card's HDMI PASSTHROUGH output is dead, and we may be
### holding the companion device in reset

New observation from the operator, and it is not something the buffer oracle
would ever have shown: **a monitor plugged into the card's HDMI OUT displays
nothing at all while the driver runs - not even a "no signal" message.**

This is not "the card does not boot". The card demonstrably boots: the mailbox
answers, it reports firmware 1.11, MST3367 I2C works, the receiver locks and
holds, the encoder spawns, and 760 of 1024 sampled pages of splash arrive by
DMA. The passthrough is a **separate subsystem this driver has never enabled**.

The suspect is `mz0380_mst3367_reset()`. Its last act, since the original
bring-up and never varied since:

    mz0380_gpio_set(dev, MZ0380_GPIO_RX_STRAP, 0);   /* pin 8 */

`MZ0380_GPIO_RX_STRAP` is pin 8, documented in `mz0380-reg.h` as the "companion
reset/power strap". Pin 9 immediately next to it is the MST3367 reset and is
documented **ACTIVE-LOW**; the same function releases pin 9 to 1. If pin 8
follows that convention, driving it 0 holds the companion device in reset for
the whole session, which is a complete explanation for a dead passthrough.

Caveat, and it is a real one: M83 found a chip at I2C 0x98 that **ACKs** and has
54/64 registers non-zero, which is not obviously the behaviour of a part held in
reset. So either pin 8 is not that chip's reset, or I2C survives the reset while
the video path does not. The two are distinguishable.

#### The free test first

Causality before code. With no module loaded at all, does the monitor on the
card's HDMI OUT show the source?

    sudo rmmod mz0380      # then look at the monitor

* Source appears with no driver, dies when the driver loads -> **we break it**,
  and pin 8 is the first suspect.
* Dead both ways -> the passthrough needs enabling and never has been; the
  companion at 0x98 (never written by us) is where to look.

#### The knob, now that it exists

`rx_strap` was added because nobody ever chose 0 deliberately - it is an
artefact of the original bring-up. 0 and 1 are both real levels, so
`MZ0380_RX_STRAP_LEAVE` (`~0u`) is a third value meaning "do not drive pin 8 at
all". Default is 0, i.e. **behaviour is unchanged** unless asked.

    sudo RXSTRAP=1 scripts/mz0380-m55-real-capture.sh 4            # release the strap
    sudo RXSTRAP=0xffffffff scripts/mz0380-m55-real-capture.sh 4   # never drive it

Score BOTH oracles on these runs: the buffer scan as usual, **and the monitor on
the card's HDMI OUT**. A passthrough that comes back is a real result even if
the capture oracle does not move - it would prove the pin controls a live video
path and that we have been disabling board hardware for the entire project.

**Method note.** This is the first genuinely new *observable* in a long time.
Every experiment for months has been scored on one number - pages touched in
buf0 - and a whole subsystem was sitting there unmeasured. Worth asking what
else on this board has state nobody has looked at.

---

### M111 (2026-08-20): THE OTHER HALF OF THE PROBLEM - one COMPLETE frame lands
### and the host never delivers it. "captured 0 bytes" was never the card's fault

Two results and one operator observation converged on a reframing that the
buffer oracle has been hiding for months.

**1. `RXSTRAP=1` is neutral** (bit-identical baseline) and the passthrough is
dead **with no module loaded at all**. So we do not break it; it was simply never
enabled. M110 closed in both directions. Pin 8 is not the companion's reset, or
not the passthrough's gate.

**2. `VICINW=3840` is neutral** (M109), which retires the 2560-threshold test.

**3. The operator reports that on Windows, with NO SOURCE CONNECTED, OBS shows
the card's "no signal" splash continuously.** Same card, same firmware. Windows
streams that splash as video; we get nothing on screen.

#### The arithmetic nobody did

    last touched page @0x2f7000, 760 pages contiguous from page 0
    1920 x 1080 x 1.5 = 3110400 = 0x2F7C00  ->  pages 0..759 = 760 pages

`760/1024 sampled pages touched, last @0x2f7000` is **exactly one 1080p 4:2:0
frame**, contiguous from offset 0. Not a partial write, not a scribble, not a
logo fragment. The card renders a complete frame and DMAs all of it into our
buffer.

Every session has read that number as "the card fell back to its splash, so
capture failed". It does mean the *content* is the splash rather than source
pixels. It does **not** mean the frame path failed - a whole frame arrived.

#### Why it never reaches userspace

`mz0380-video.c` says it outright:

    Fake-frame path: no completion IRQ exists (M41), so streaming is a
    polling kthread that spawns the encoder itself, once per frame (M39).
    The real path arms the encoder here and delivers from the MSI-driven
    drain.

The real path delivers **only** on a frame-completion event. `frame_events=0`,
`EVENT[0x30]=0` in every run ever recorded. That event has never fired once in
this project - which is precisely why the nosg path was written as a poller in
the first place. The real path never got the same treatment, so a complete frame
sits in buf0 and nothing hands it to vb2.

That is `captured 0 bytes`. That is OBS listing the device and showing black.

#### This splits the blocker in two

| | problem | evidence | status |
|---|---|---|---|
| **content** | splash instead of source pixels | VIC rejects BT1120 structure | everything M76-M109 swept |
| **delivery** | one frame lands, host never delivers it | `frame_events=0`, complete frame in buf0 | **never investigated** |

The delivery half is host-side, costs no encoder spawns to work on, and has a
perfect target: **match what Windows does with no source at all** - the splash,
continuously, in OBS. That is a smaller, checkable goal than "capture the
source", and reaching it validates SET_BUF, the outbound ATU, the DMA target,
vb2 and the V4L2 format negotiation in one shot. It also turns the content
problem into something visible in OBS at 60fps instead of one number per
105-second m55 run.

#### On the instrument

The operator's own summary - "we stay stuck to this m55 part" - is correct and
this finding is why. m55 is a 105-second, source-dependent ritual that reports a
single end-of-run page count. It cannot distinguish "no frame" from "one frame,
undelivered", and that distinction turns out to be the whole thing. The next
instrument should need **no source** (Windows proves none is required to get
pixels), run in seconds, and sample buffer state and the EVENT/token words on a
timeline rather than once at the end.

#### Next

1. Give the real path a polling delivery fallback, as the nosg path already has:
   if a frame's worth of buffer is dirty and no completion event arrived, hand it
   to vb2. Param-gated so it can be bisected. Expected result: the splash appears
   in OBS, matching Windows.
2. Only then return to the content problem, with a live picture as the oracle.

---

### M112 (2026-08-20): poll-drain implemented - deliver the frame the card has
### already written, and stop lying about its format

Host-side only, no encoder spawns to develop. Three coupled defects, all found
by following M111 into the delivery path:

**1. Nothing ever calls the delivery code.** `mz0380_drain_frame_snapshot()`
already knows how to turn "buffer idx holds a frame" into a vb2 delivery - infer
the length from the poison suffix, copy, re-poison. The real path only ever
reaches it from the MSI drain, which needs a completion event that has never
fired. `mz0380_poll_drain_thread()` now synthesises the snapshot (token = buffer
index, timestamp = now) and calls the SAME function, so the event-driven path is
byte-for-byte unchanged and a frame is delivered exactly once - the re-poison
inside the drain is what makes it idempotent. `mz0380_infer_frame_length()`
returns `-ENODATA` on a fully-poisoned buffer, so it doubles as the "is there
anything here" test.

**2. The plane is too small for the frame.** `sizeimage = bitrate/8`, clamped -
12 Mbit gives 1.5 MB, and m55's 4 Mbit gives 512 KB. The frame is **3110400
bytes**. The drain's `len > plane` guard would have rejected every single
delivery with "does not fit vb2 plane". With `poll_drain_ms` set, sizeimage is
now `max(width*height*3/2, bitrate/8)`.

**3. The node advertises H.264 for a raw payload.** buf0 holds planar 4:2:0, not
a bytestream - 3.1 MB of `0x11` is not H.264, and a compressed splash would be a
few KB. The nosg path already advertises NV12 for exactly this reason; the real
path never did. `VIDIOC_ENUM_FMT`, `TRY_FMT`, `S_FMT` and `G_FMT` now report
NV12 at the detected geometry when `poll_drain_ms` is set.

`mz0380-m55-real-capture.sh` writes to `cap-m55.nv12` instead of `.h264` when
`POLLDRAIN` is set - the same naming trap that made M57 "fail" ffprobe.

Everything is gated on `poll_drain_ms != 0`, default 0, so every earlier result
still reproduces exactly.

#### The test, and it needs NO SOURCE

Windows shows the no-signal splash in OBS with nothing plugged in, so the source
is not required to get pixels - and leaving it out removes the last uncontrolled
variable from the run.

    sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 4

Expect in dmesg:

    poll-drain armed every 20 ms ...
    poll-drain: buf 0 holds 3110400 bytes with no completion event; delivering

and a non-zero `--- captured N bytes ---`. Then look at it:

    ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 /tmp/cap-m55.nv12

A visible splash is the first picture this project has ever produced, and it
would validate SET_BUF, the outbound ATU, the DMA target, vb2 and format
negotiation in one shot.

**What it does NOT prove.** The card may still write only one frame - the
delivery fix cannot manufacture frames 2..N. If exactly one frame arrives and
then the count stops, the remaining problem is the card's frame cadence
(credits/`enc_stat`/`host_ready`), which is a different investigation from the
content problem and from this one. Three separate questions, now separable.

---

### M113 (2026-08-20): the no-source run was impossible - both the script and the
### driver refused before the encoder was ever armed

The first `POLLDRAIN=20` attempt never reached the capture step:

    No coherent HDMI timing was locked during the 45s window.
    (module unloaded)

Two independent gates, and the instruction to disconnect the source ran straight
into both:

1. **m55 step 1** polls `--query-dv-timings` for `LOCKWAIT` seconds and
   `exit 2`s if nothing locks. With nothing plugged in, nothing ever will.
2. **`mz0380_video_start_streaming()`** does the same thing in the driver: no
   lock, no `signal_cache_ms` hit, `goto error`. `mz0380_force_timings` does not
   help - it only rescues a signal that IS locked but matches no table entry.

Both fixed, both opt-in:

* `stream_without_signal=1` (new, default 0) arms the real path at 1920x1080p60
  with no lock at all, logging plainly that the card's splash is what to expect
  rather than source pixels.
* `NOSRC=1` in m55 skips the lock gate, drops `CAPWAIT` to 10s, defaults
  `WATCH=0` (no HPD pulses, no receiver I2C - there is no source to provoke),
  writes `cap-m55.nv12`, and stops printing power-cycle prompts.

The timing change matters beyond convenience. A no-source run is ~15s instead of
~105s, needs no operator action, and has **no source power-cycling mid-window** -
which is what produced the M105 measurement artefact. It is a strictly better
instrument for everything except source-content questions.

    sudo NOSRC=1 POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 4
    ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 /tmp/cap-m55.nv12

Expect: `arming anyway at 1920x1080p60 [stream_without_signal=1]`, then
`poll-drain: buf 0 holds 3110400 bytes with no completion event; delivering`,
then a non-zero byte count.

---

### M114 RESULT (hardware, 2026-08-20): with NO source the card writes NOTHING -
### the splash needs a lock, and the Windows "no signal" was misread

    sudo NOSRC=1 POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 4

Everything armed exactly as intended - `arming anyway at 1920x1080p60
[stream_without_signal=1]`, SET_VIC/SET_ENC_PARAMS/SET_AIC/START all `ret=0` -
and the card wrote **nothing at all**: `0/1024` on all four buffers, buf0 solid
poison, `captured 0 bytes`, and no `poll-drain: buf N holds ...` line because
there was never anything to deliver.

**So the NOSG splash is not a no-source behaviour.** It appears when the
receiver holds a lock and the capture path then finds nothing it will accept.
With no source at all the encoder produces no frame whatsoever. That is a real
new fact about the card, and it narrows what the splash means: it is evidence
the capture path RAN, which requires a lock.

Two supporting details from the same run, both consistent:

* `b0=20` at all three diag points, not our `0x21` - with no lock,
  `commit_digital_output()` never runs, so the receiver sits at its own default.
* `B1 01=80` (DVI), `48=00` (RGB), `0b=00`, `0c=00` - the link layer reading
  nothing, as expected with an empty connector.

#### Correction: the premise of the no-source test was mine and it was wrong

M111 built on the operator's report that Windows shows "no signal" in OBS with
nothing connected, and treated it as "the card streams its splash with no
source". This run disproves that for our stack, and the likeliest explanation is
that the Elgato **software** draws that overlay host-side - it is a different
driver stack, and an application-drawn placeholder is the ordinary way that is
done. An anecdote about another stack's UI was given the weight of a measurement
about card behaviour, and it aimed the test at the wrong scenario.

`stream_without_signal` and `NOSRC` are kept - they are correct as features, the
run did what it was asked, and "the card writes nothing without a lock" is worth
having established. But a no-source run cannot test frame DELIVERY, because
there is no frame.

#### What is still untested: M112, with the source connected

M111's finding is untouched by this. With a source locked, the card DMAs
**exactly one complete 1920x1080 4:2:0 frame** (760 contiguous pages ending at
0x2f7000 = 3110400 bytes) and `frame_events=0` - the completion event never
fires, so `mz0380_drain_frame_snapshot()` is never called and userspace gets
nothing. That is what the poll-drain exists to fix, and it has not yet been run
in the situation it was written for:

    sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 4     # SOURCE CONNECTED, no NOSRC

Expect `poll-drain: buf 0 holds 3110400 bytes with no completion event;
delivering` and a non-zero byte count, then:

    ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 /tmp/cap-m55.nv12

(An empty file makes ffplay hang on the rawvideo demuxer waiting for a frame -
that is what "stuck" looked like here, not a decode problem.)

---

### M115 RESULT (hardware, 2026-08-20): **FIRST PIXELS.** The poll-drain works -
### 3903488 bytes captured, and the card's splash renders

    sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 4

    --- captured 3903488 bytes ---
    frame token 0 inferred length=794368  from 4-byte poison boundary
    frame token 0 inferred length=3110400 from 4-byte poison boundary

**The first bytes this project has ever pulled off the card**, and the operator
confirms the card's own "no signal" screen renders in ffplay. `frame_events=0`
and `EVENT[0x30]=0` as always - the completion event still never fires - and it
no longer matters, because the poll-drain delivers without it.

M111 is confirmed end-to-end: the card had been writing a complete frame all
along and the host was throwing it away. `captured 0 bytes` was never the card's
fault. Everything downstream of the DMA is now proven working - SET_BUF, the
outbound ATU, the DMA target, the poison-boundary length inference, vb2, and
V4L2 format negotiation.

#### The one defect in the first run: a torn frame

3903488 = **793088 + 3110400**, exactly. Two deliveries, and the first was a
fragment:

    [rawvideo] Invalid buffer size, packet size 793088 < expected frame_size 3110400

The poison boundary marks how far the DMA has **got**, not that it has
**finished**, so a poll that lands mid-burst returns a torn prefix. The nosg path
has always guarded against this (`mz0380_nosg_frame_landed()` requires two
untouched tail dwords at the known frame size); the real path shipped without
the equivalent.

Fixed: the poll-drain now requires `len >= source_width * source_height * 3/2`
before delivering, and logs `holds N of M bytes - DMA still in flight, waiting`
otherwise. The expected size is exact and known, so this needs no heuristic.

`m55` step 4 also stopped running an H.264 NAL check and `ffprobe` over raw NV12 -
that is what produced the run's "ADPCM Nintendo Gamecube DTK" line, which is
ffprobe guessing at planar YUV. It now reports whole-frame counts and warns on a
partial tail.

To view the good frame from the existing capture without another run:

    tail -c 3110400 /tmp/cap-m55.nv12 > /tmp/frame.nv12
    ffplay -f rawvideo -pixel_format nv12 -video_size 1920x1080 /tmp/frame.nv12

#### Where the three problems now stand

| | question | status |
|---|---|---|
| **delivery** | frame lands, host never hands it over | **SOLVED** (M112/M115) |
| **cadence** | does the card write frames 2..N? | **NO** - one frame in a 60s window |
| **content** | splash instead of source pixels | still open, unchanged |

The cadence problem is now cleanly isolated for the first time. One complete
frame arrived in sixty seconds, and buffers 1-3 were never touched, so the card
is not rotating through the registered buffers. That points at the credit /
ownership handshake - `enc_stat`, `host_ready`/`wency_ready`, op2/op8 - which was
examined in M92/M93 and called neutral **on an oracle that could not see it**:
"one frame lands" looks identical whether or not credits are returned. That
verdict should be re-tested now that a second frame is a visible outcome.

---

### M116 RESULT (hardware, 2026-08-20): op 0x08 is neutral for CADENCE too -
### re-tested on an oracle that can see it

    sudo OP8=1 POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 4

    SET_BUF_8(op 0x08, window0 slots 5..8, sets wency_ready) ret=0
    frame token 0 inferred length=3110400 ...
    --- captured 3108864 bytes ---

**Exactly one frame**, same as without op8, and bufs 1-3 never went non-poison.
M93 called op8 neutral using the pages-touched oracle, which could not
distinguish "one frame" from "one frame and no credits". This run scored it on
frame COUNT, the oracle that can, and the answer is the same: **op 0x08 does not
affect cadence.** The `host_ready` / `wency_ready` asymmetry is now eliminated
twice, on two different oracles. Stop testing it.

Also note the poll-drain's completeness gate (M115) worked: exactly one
`length=3110400` delivery this run, no torn 794368-byte prefix.

#### A harness bug that ate the frame: 1536 bytes short

The driver delivered a **full 3110400** bytes - the log says so - but the file
held **3108864**, short by exactly **1536**. ffplay rejected the whole capture,
and `tail -c 3110400` could not rescue it because the missing bytes were never
written.

`v4l2-ctl --stream-mmap --stream-count=4` blocks until four frames arrive. The
card delivers one. So `timeout` SIGTERMs v4l2-ctl at CAPWAIT, **mid-write**, and
the last frame loses its unflushed stdio tail. Nothing to do with the card, the
DMA or the poll-drain - the measurement destroyed its own result.

This is the same class as method rule 4 (a harness carrying its own copy of a
default): the harness asked for an outcome the system could not produce and then
mangled the outcome it did produce. Fixed three ways:

* `timeout -s INT --foreground` so v4l2-ctl gets a chance to close the file.
* Step 4 trims a partial tail when at least one whole frame survived, so the
  file is playable.
* When NOT ONE whole frame survived, it now says so explicitly and prints the
  fix - ask for a count the card can deliver:

      sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 1

**While cadence is one-frame-per-stream, request ONE frame.** Asking for four
guarantees the kill-mid-write every time.

#### Cadence: what is left to test

Eliminated: op 0x08 / `wency_ready` (M93, M116). Still untried:

1. `credit_kick_ms` - already a parameter, never used in anger. It fires the
   full ack sequence (BAR5[0xdc]=2, BAR0[0x30]=0, doorbell 0x400) on a timer.
   If the card is waiting for an ownership ack that only the completion path
   sends, this is exactly the missing kick - and the completion path has never
   run once.
2. `enc_stat` (BAR0+0x50+idx): the per-frame "may I encode into the host buffer"
   handshake. M40 notes the card only ever SETS it and never clears it, so a
   stale 1 makes the encoder retry 10x then skip every frame. The driver clears
   it at stream start - but nothing clears it **after each delivered frame**,
   which is what a running stream would need.

(2) is the strongest untested hypothesis on the board: it is a per-frame
handshake, the card never clears it itself, and we only ever clear it once at
start. That is a precise fit for "exactly one frame, then silence".

### M117 (2026-08-20): the poll-drain never acked enc_stat - the exact
### documented cause of "one frame then nothing"

Found by reading our own code after M116 measured one-frame cadence with op8
eliminated. `mz0380_enc_stat_ack()` has carried this comment since M40:

    M40. Acknowledge consumed bitstreams: write 0 to the card's per-stream
    enc_stat bytes (BAR0 + 0x50 + idx). ... without this ack the card's
    encoder produces exactly one bitstream and then skips every subsequent
    frame.

That is the measured symptom, word for word: one frame per stream, buffers 1-3
never touched.

`mz0380_dma_drain_video()` acks after its drain batch. The poll-drain calls
`mz0380_drain_frame_snapshot()` **directly**, bypassing the batch wrapper, so it
never acked. The card sets `enc_stat` and never clears it itself; the driver
clears it once at stream start (mz0380-dma.c:1365) and, on this path, never
again.

So M112's delivery fix was half a fix: it took the frame out of the buffer but
never told the card the slot was free. Fixed - the poll thread now acks once per
pass in which it delivered anything, mirroring the event path's batch ack.

**This is a prediction, not a result.** If it is right, the next run gives more
than one frame and buffers 1-3 start seeing traffic. If cadence stays at one
frame, `enc_stat` is not the gate and `credit_kick_ms` is next.

    sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 4

Ask for 4 frames deliberately this time: if the fix works, v4l2-ctl gets its
four and exits cleanly, and the M116 kill-mid-write problem disappears on its
own. If only one arrives it will be trimmed and reported as before.

---

### M118 RESULT (hardware, 2026-08-20): the enc_stat ack does NOT fix cadence -
### M117 refuted. And the 1536-byte shortfall is fully explained

    sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 4

One `frame token 0 inferred length=3110400` delivery. **Still exactly one
frame**, bufs 1-3 still untouched. M117's prediction was wrong: acking
`enc_stat` after each poll-drained frame changes nothing.

The ack was still a real bug and the fix stays - the poll path bypassed the
batch wrapper that the event path acks from, and the card genuinely does need
that slot handed back. It simply is not what is holding cadence at one frame.

#### The 1536 bytes: arithmetic, not mystery

    3108864 = 759 x 4096   (the frame is 759.375 stdio blocks)

v4l2-ctl buffers to a stdio `FILE*`. When `timeout` kills it, whole 4 KiB blocks
have been flushed and the final **partial** block - 1536 bytes - has not. Same
value twice, exactly, because it is deterministic. `timeout -s INT` did not help:
v4l2-ctl installs no handler, so SIGINT terminates it just as abruptly.

**Consequence for method: the captured file size is a bad oracle while cadence is
broken.** The honest count is the driver's own delivery lines:

    dmesg | grep -c 'inferred H.264 length='

That is independent of how v4l2-ctl dies. Score cadence on that from now on.

To get a clean playable file, ask for a count the card can deliver, so v4l2-ctl
exits normally and flushes:

    sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 1

#### What is left, and why credit_kick_ms could not have been tested as-is

`credit_kick_ms` fires only inside `mz0380_event_thread()`, which runs only
after `echo start > /proc/mz0380-events`. m55 never starts it, so setting the
parameter alone would have done nothing - a trap worth recording before someone
"tests credit_kick_ms" and reads a false negative.

So the sequence moved into the path that actually runs. `mz0380_credit_rearm()`
is now lifted out of the ISR (it was inline in the interrupt handler) and the
poll-drain fires it after a delivered batch under `poll_drain_credit`:

    BAR5[0xdc] = 2 ; BAR0[0x30] = 0 ; doorbell 0x400

That drives the card's `pciep_isr_clrint` and restores `msi_enable = 1`. The
completion channel is a **one-shot**: posting an event consumes the credit and
only this doorbell restores it. On our path no event ever posts, so nothing has
ever restored it - the card may be sitting on a spent credit, unable to signal
the next frame, which fits "exactly one frame" as well as enc_stat did and is
the last part of the ISR the poll path does not reproduce.

    sudo POLLDRAIN=20 POLLCREDIT=1 scripts/mz0380-m55-real-capture.sh 4

Score with `dmesg | grep -c 'inferred H.264 length='`, and watch bufs 1-3.

### M119 (2026-08-20): every host-side ack is eliminated - the cadence gate is
### the per-frame WAKE-UP, and op 0x06 is it

    sudo POLLDRAIN=20 POLLCREDIT=1 scripts/mz0380-m55-real-capture.sh 4
    sudo dmesg | grep -c 'inferred H.264 length='   ->  1

The completion credit re-arm (BAR5[0xdc]=2, EVENT=0, doorbell 0x400) fired after
every delivered frame and cadence stayed at **exactly one**. Combined with M116
and M117 that closes the whole host-side ack surface:

| candidate | verdict |
|---|---|
| op 0x08 / `wency_ready` | neutral (M93 on pages, M116 on frame count) |
| `enc_stat` slot ack | neutral (M117) |
| completion credit re-arm | neutral (M118/M119) |

None of them is the gate. Which fits what M41 already recorded and this session
kept reading past: *"the card DMAs one contiguous burst into buf0 and then its
encoder loop parks forever, IRQ-less, for a card-internal reason no host action
can fix"*, and M39: *"a fresh encoder spawn reliably yields exactly one more
frame."* **One frame per wake-up is the card's actual behaviour.** The nosg path
has always worked around it by respawning the encoder per frame, which is why
that path eats the spawn budget.

#### The cheap wake-up nobody tried

From M22, and sitting in our own comment at mz0380-dma.c:1294:

    tinyvenc5 then blocks on /sys/vpl_pciep/epint waiting for a separate
    START_STREAMING (op 0x06) before it DMAs any frame

and ep.ko's op6 handler (@0x1854) does **nothing but** `sysfs_notify(epint)`.
So: one notify, one frame. We have always sent exactly one op6, at stream start,
and we have always received exactly one frame. The correlation has been sitting
in plain sight the whole time.

op6 is fire-and-forget - it posts no mailbox completion - and crucially it does
**not** fork an encoder; SET_VIC does that. So re-sending it is cheap and costs
**no spawn budget**, unlike the nosg respawn workaround.

`op6_kick_ms` re-fires it on a timer from inside the poll-drain thread (the only
thread that reliably runs for the whole stream), and the stop line now reports
the kick count alongside deliveries.

    sudo POLLDRAIN=20 OP6KICK=16 scripts/mz0380-m55-real-capture.sh 4

16 ms is one frame period at 60fps. Score with:

    sudo dmesg | grep -c 'inferred H.264 length='

More than one is the result. If frames now flow, cadence is solved and the
remaining problem is content alone. If it is still one, the wake-up is not a
notify either and the next step is static: read tinyvenc5's frame loop in the
unpacked blob to find what it actually blocks on after frame one.

**Note:** `dmesg` needs `sudo` on this kernel (`read kernel buffer failed:
Operation not permitted` otherwise, which silently reports 0).

### M120 RESULT (hardware, 2026-08-20): a free-running op6 kick is WORSE than
### none - zero frames and a lost receiver lock

    sudo POLLDRAIN=20 OP6KICK=16 scripts/mz0380-m55-real-capture.sh 4
    sudo dmesg | grep -c 'inferred H.264 length='   ->  0

A regression from the reliable one frame, and the receiver came apart with it:

    detect 55=83 no-lock  (t+3.5s)
    detect 55=03 no-lock  (and stayed there for the rest of the run)
    output stage [at stop]: R55=03 no-lock ... b7=02
    link [at stop]: B1 01=80 -> DVI (source fell back)

The cause is written in our own source at the op6 send site
(mz0380-dma.c:1296): op6 must land **after** the freshly `system()`-forked
tinyvenc5 has exec'd, opened epint and consumed the `SET_VIC(0x29)` it reads
first, *"rather than racing its start-up read"*. A timer that starts at stream
start races exactly that, 62 times a second, and `start_delay_ms` exists
precisely because the timing of the FIRST op6 is delicate.

So the hypothesis is not refuted - the *shape* of the test was wrong. M119
turned a one-shot handshake into a flood.

Whether the lock loss is causal or coincidental is not established. The script
pulses HPD three times during the capture window by design (`WATCH=1`) and the
source has recovered from that in every previous run; this time it did not. A
mailbox saturated at 62 commands/second contending with the detect and HPD work
is a plausible mechanism, but a flaky source is not excluded. `b7=02` at stop is
new and unexplained.

#### The right shape: one kick per consumed frame

`op6_kick_ms` is now a **minimum spacing**, not a period, and kicks fire only
**after a delivered frame**. That makes the start-up race impossible - the first
kick cannot occur until the first frame has already arrived - and it matches what
the handshake actually is: consume a frame, ask for the next.

    sudo POLLDRAIN=20 OP6KICK=16 scripts/mz0380-m55-real-capture.sh 4
    sudo dmesg | grep -c 'inferred H.264 length='

Expect at least the usual one delivery (the kick cannot make the first frame
worse now). More than one means the notify is the cadence gate. Exactly one
means the wake-up is something else, and the next step is static: read
tinyvenc5's frame loop for what it blocks on after frame one.

**Cold-boot at mains before this run.** Roughly 14 encoder spawns since the last
power cycle, the wedge band is 8-18, and the card just had a bad run.

### M121 (2026-08-20): two zero-frame runs in a row, no cold boot between them -
### the card is in the wedge band and the last two results are VOID

    sudo POLLDRAIN=20 OP6KICK=16 scripts/mz0380-m55-real-capture.sh 4   (reshaped kick)
    sudo dmesg | grep -c 'inferred H.264 length='   ->  0

**No op6 kick could have fired.** M120's reshaped kick only fires after a
delivered frame, and nothing was delivered, so `op6_kick_ms` was inert for the
whole run. The reshaped kick is therefore **still untested**, and the zero-frame
result cannot be attributed to it.

What it can be attributed to: the card. Uptime is continuous across M119 and
M120 - the recommended mains-off cold boot did not happen - putting this at
roughly **15 encoder spawns since the last power cycle**, inside the documented
8-18 wedge band. The signature fits:

* two consecutive zero-frame runs after a long, reliable run of exactly-one-frame
  results;
* the receiver lock flapping through `55=df`, `55=d7`, `55=c7` and repeated
  `hper=337 vper=299 lines=1127` re-lock transients (the M105 artefact) where
  earlier runs held `55=7f` steady for 60 s.

**Both M119's and M120's runs are void as cadence evidence.** M119 additionally
had a real confound (the free-running flood raced tinyvenc5's start-up read), but
its zero-frame result and this one share a simpler explanation that has to be
excluded first.

#### Method note

The rule this session keeps paying for, in a new costume: *a bisect step is only
informative if every other variable sits at a value already known to permit the
outcome you are measuring.* Card health is one of those variables, and it is the
one nobody lists. Two runs were spent measuring a degraded card.

Also fixed: m55's step-3 grep filtered out the poll-drain's own summary line, so
`poll-drain stopped after N deliveries, M op6 kicks` - the thing that would have
shown the kicks never fired - was invisible in the run output. It is in the grep
now.

#### Sequence to resume

1. **Mains-off cold boot.** Slot standby survives a soft power-off.
2. `sudo scripts/mz0380-m77-cardlog.sh` - zero-spawn health check. Healthy = op 0x6e
   answers with the untouched sentinel; `-110` means still wedged.
3. Control, and ask for one frame so v4l2-ctl exits cleanly:
   `sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 1`
   Expect exactly one `inferred H.264 length=3110400` and a clean 3110400-byte
   file. **If this does not give one frame, stop - the card is still unhealthy
   and nothing measured after it means anything.**
4. Only then: `sudo POLLDRAIN=20 OP6KICK=16 scripts/mz0380-m55-real-capture.sh 4`
   and check `poll-drain stopped after N deliveries, M op6 kicks` in the output.
   M > 0 is the proof the kick actually ran.

### M122 (2026-08-20): m77 gives a FALSE all-clear, and the reference projects
### do not have our problem

**The control failed.** After m77 reported the card healthy (op 0x6e answered
with the untouched sentinel), the control run delivered **0 frames**:

    poll-drain stopped after 0 deliveries, 0 op6 kicks

Uptime is continuous - the mains-off cold boot still has not happened - putting
this at ~17 spawns since the last power cycle, at the very top of the 8-18 wedge
band.

**So m77 healthy != the card can still spawn an encoder.** m77 exercises the
mailbox transport only; the spawn budget is a separate resource it cannot see.
Recording this because the handoff has called m77 "the health check" and it just
issued a false all-clear that would have licensed interpreting two more void
runs. A real health check has to be the control capture itself.

The M120 reshaped op6 kick is **still untested**: `0 op6 kicks` confirms it never
fired, because it fires only after a delivered frame and nothing was delivered.

#### The reference projects, assessed (zero hardware cost)

`elgato-gchd` (plus the `_2` and `_3` forks) - **wrong silicon.** Fujitsu
MB86H57 / MB86M01 over USB, host-uploaded firmware. `GCHD::stream()` is a bare
`libusb_bulk_transfer`; it is continuous because USB bulk is continuous, so there
is no per-frame handshake to learn from. The forks differ only in
`settings.cpp`, `psi_pat/psi_pmt` and `utility.cpp` - nothing touching the
stream path.

`sc0710` (already in this repo - Elgato 4K60 Pro mk.2, Yuan, PCIe) - closer, but
its answer is architectural rather than portable. It **never asks the card for
frames**: an XDMA descriptor chain writes continuously and
`sc0710_thread_dma_function` services it every 2 ms, while
`sc0710_thread_hdmi_function` polls I2C status every 200 ms and does nothing
else. That is the "keep it listening" model - two poll threads and a hardware
ring - but the MZ0380 has no such ring: an ARM SoC runs Linux and `tinyvenc5`
decides when a frame happens.

Neither project has an on-card process blocking on a sysfs notify, because
neither has an on-card process. **The reference that can answer this is
tinyvenc5 itself**, which is already unpacked:

    tar xzf /usr/lib/firmware/mz0380/MZ0380.HD.HEX -C <scratch>
    llvm-objdump -d --triple=armv5te-linux-gnueabi <scratch>/yuan_demo_sdi/tinyvenc5

Read its frame loop for what it blocks on after frame one - whether that is the
epint notify (making M120's kick right in principle) or something else entirely.
Static, no spawns, and it can be done while the card is powered off.

### M123 (2026-08-20): elgato-gchd DOES drive an MST3367-family receiver -
### M122's dismissal was wrong. But its values need decoding, not copying

The operator pushed back on M122's "wrong silicon" verdict, correctly. M122
judged gchd by its encoder and USB transport and never looked at its HDMI
front-end, which is a separate chip.

gchd's `mailWrite(0x4e, {reg, val})` port drives a receiver with the **MST3367
register set**: `0x00` as bank select, plus `0xab`, `0xad`, `0xae`, `0xb0`,
`0xb1`, `0xb2`, `0xb3`, `0xb4` - the exact output-stage registers this project
has been sweeping all session. `readHdmiSignalInformation()` reads a signal
block and an RGB bit, `configureHDMI()` walks the same bring-up shape.

So it is a genuine reference for the CONTENT problem. Two caveats found while
checking, both of which matter:

**1. RETRACTED, mid-analysis: "we clobber the bank register's upper bits."**
gchd writes bank selects as `0xcc` / `0xcd` / `0xce` where we write bare
`0x00` / `0x01` / `0x02`, and the low bits line up as banks 0/1/2 - which looked
like proof that we zero four control bits on every bank switch. It is not.
`hdcapm`, which is unambiguously MST3367 and matches our whole register map
(`0x55` detect, `0xb0` output, banks 0-3), writes **bare bank numbers**:

    static void mst3367_switch_bank(...)  { u8 buf[] = { 0x00, bank }; ... }

Our encoding is corroborated by the reference that is definitely the same part.
The claim was made on a coincidence and withdrawn within the same pass.

**2. gchd's register VALUES do not transfer.** For the same registers:

| reg | hdcapm | gchd |
|---|---|---|
| `0xb1` | `0xe0` | `0x0c` |
| `0xb2` | `0x08` | `0xcc` |
| `0xb3` | `0x00` | `0xcc` |
| `0xb4` | `0x55` | `0x99` |

and `0xcc` recurs across unrelated registers (`0x25`, `0x26`, `0x27`, `0x31`,
`0x32`, ...) far too often to be register content. That points at padding or an
encoding inside gchd's mail protocol, not raw values. Copying them over would be
method rule 3 exactly - byte-for-byte parity treated as a rule rather than a
hypothesis generator.

**What gchd is actually worth, then:** its *structure* - the order of the
bring-up, which registers are touched for HDMI versus component, what it reads
back and when, and `readHdmiSignalInformation()`'s notion of a valid signal.
Decoding `mailWrite`'s wire format first is a prerequisite for anything
value-level, and that is a self-contained task that costs no hardware runs.

`sc0710` (in this repo) remains a structural reference only: two poll threads
(HDMI status at 200 ms, DMA at 2 ms) and an XDMA descriptor chain that writes
continuously. No per-frame handshake, because it has no on-card process.

---

### M124 (static, 2026-08-20): systematic hunt for missing pieces across all
### three reference projects - one big finding, one clean negative

Operator asked for a broad sweep of the reference projects for anything this
driver lacks. Entirely static: **zero encoder spawns, zero hardware runs.**

#### 1. CLEAN NEGATIVE: the MST3367 init is COMPLETE

Diffed every `(bank, register)` hdcapm writes against every one we write, with
symbolic names resolved and hdcapm's `#if 0` blocks stripped:

    hdcapm (live code only): 59 pairs      ours: 66 pairs
    written by hdcapm, never by us:        NONE

Our receiver init is a **superset** of hdcapm's. Value diff over the shared
registers found only ordering/duplication artefacts plus two deliberate
differences already documented in our source (`0xb1` = `0xc0` vs hdcapm's
`0xe0`, `0xb2` = `0x00` vs `0x08`) - the HD60 Pro's own Windows values from
`FUN_14024dc28`, versus hdcapm's sibling board.

**"We are missing an MST3367 register" is closed.** After a session of sweeping
that chip, this is worth having: the receiver side is done, and effort should go
elsewhere.

#### 2. gchd drives a DIFFERENT receiver - do not copy its values

`mailWrite(port, {reg, val})` writes raw bytes, so gchd's values are real
register writes, not an encoding (M123 left this open). But they disagree with
hdcapm on the same register numbers (`0xb1`: `0x0c` vs `0xe0`; `0xb2`: `0xcc` vs
`0x08`; `0xb4`: `0x99` vs `0x55`) and its bank selects carry a `0xcc` base where
both hdcapm and we write bare bank numbers. Game Capture HD is older hardware
with an overlapping register map. **Its structure is worth reading; its values
are not transferable.**

#### 3. sc0710 - architectural reference only

Two poll threads (HDMI status 200 ms, DMA 2 ms) over an XDMA descriptor chain
that writes continuously. No per-frame handshake, because it has no on-card
process. Confirms polling is a legitimate shape; offers nothing to copy.

#### 4. THE FINDING: hdcapm acks every buffer with a FIRMWARE COMMAND

`hdcapm` is the true architectural sibling - PCIe capture card, on-board encoder,
host-pull model with no completion interrupt. Its loop polls a status block
(`REG_06B0`), and when a buffer is ready it reads address and length from that
block. Then, after every transfer:

    /* Acknowledge the buffer back to the firmware. */
    hdcapm_read32(dev, 0x800, &val);
    hdcapm_write32(dev, 0x800, val);              /* clear latched status   */
    hdcapm_write32(dev, REG_FW_CMD_ARG(0), 0x83);
    hdcapm_write32(dev, REG_FW_CMD_ARG(1), arr[4]);   /* dwords consumed    */
    hdcapm_write32(dev, REG_FW_CMD_ARG(2), 0x2aaaaaaa);
    hdcapm_write32(dev, REG_FW_CMD_BUSY, 1);
    hdcapm_write32(dev, REG_FW_CMD_EXECUTE, 0x30);
    hdcapm_write32(dev, 0x6c8, 0);                /* clear buffer-ready flag */

A **per-buffer acknowledge sent as a mailbox COMMAND, carrying the consumed
length.** We have never done anything of that shape. Our acks are raw register
pokes - `enc_stat` (M117) and the credit doorbell (M118) - and both were
measured neutral. A command-level "I consumed buffer N, length L" is a different
mechanism entirely, and it is exactly the kind of thing a card would wait on
before producing frame two.

#### 5. Twelve opcodes the card accepts that we have NEVER sent

Disassembled `ep.ko`'s `pciep_isr` (the real host-command dispatcher, `0x1210`)
and enumerated its opcode compares:

    0x01 0x02 0x04 0x06 0x08 0x09 0x0a 0x0b 0x0f 0x14 0x15 0x17 0x22
    0x29 0x2a 0x2d 0x2f 0x31 0x50 0x52 0x60 0x61 0x62 0x64 0x6e 0x7b

Never sent by this driver:

    0x09  0x0f  0x22  0x2f  0x50  0x52  0x60  0x61  0x62  0x64  0x7b
    (0x0b is firmware download - stays untouched, permanently)

**`0x09` and `0x2f` are the standout pair.** They share a handler at `0x1824`
which gates on the same flag byte `[r5+0x71c]` as the op 0x06 handler at
`0x1854` and then falls into the path at `0x1830` that op 0x31 (POST_PROC) also
enters - i.e. the same *class* of operation as START_STREAMING, with a different
command code. If the card has a "frame consumed, send the next" command, it is
in this group, and it would be the MZ0380 analogue of hdcapm's `0x83` ack.

**Caveat on the extraction:** the opcode scan covered `pciep_isr+0x00..0x250`
only, and it lists `0x03`, `0x05`, `0x07`, `0x1a`, `0x1b`, `0x1e`, `0x1f` as
"not in the dispatcher" even though I2C demonstrably works over `0x1a`/`0x1b`.
So the compare list is INCOMPLETE - there are more handlers further in, and the
"never sent" list is a lower bound, not a closed set. Re-run over the whole
function before treating it as exhaustive.

#### Next, in order, all static until the last step

1. Decode the `0x09` / `0x2f` handler at `0x1824` fully - what it reads from the
   command block and what it notifies. This is the best candidate for a
   per-frame ack and costs nothing.
2. Complete the opcode scan over all of `pciep_isr`.
3. Check `video_capture_mgr` / `tinyvenc5` for which of `0x09`/`0x2f`/`0x22`
   they expect between frames.
4. Only then test on hardware - and only after a mains-off cold boot, with the
   one-frame control passing first.

### M125 (static, 2026-08-20): op 0x09 / 0x2f are a SECOND WAKE-UP, and there
### are TWO epint nodes

Completed the opcode scan over all of `pciep_isr` (M124's was truncated - `0x07`
is present after all) and decoded the wake-up handlers.

Full dispatcher opcode set:

    0x01 0x02 0x04 0x06 0x07 0x08 0x09 0x0a 0x0b 0x0f 0x14 0x15 0x17
    0x22 0x29 0x2a 0x2d 0x2f 0x31 0x50 0x52 0x60 0x61 0x62 0x64 0x6e 0x7b 0x7c

Never sent by this driver: `0x09 0x0f 0x22 0x2f 0x50 0x52 0x60 0x61 0x62 0x64
0x7b 0x7c` (`0x0b` is firmware download and stays untouched).

#### The wake-up handlers, decoded

Every one has the shape `sysfs_notify(kobj=[r5+0x678], NULL, <name>)`. Resolving
the literal pool through the ELF relocations gives the names:

| pool slot | string |
|---|---|
| `0x1990` | `epint` |
| `0x1998` | `epint_1080p` |
| `0x199c` | `audio_ctrl` |
| `0x1988` | `"$$$ cmd(%d) => no signal \n"` |

* **op 0x06** (`0x1854`): notifies `audio_ctrl`, **then** `epint` or
  `epint_1080p`, selected by a word at `.data+4` compared against 7.
* **op 0x09 / 0x2f** (`0x1824`): the same `epint` / `epint_1080p` notify,
  **without** the `audio_ctrl` notify.
* **op 0x2a** (SET_AIC, `0x17cc`): logs `$$$ SET_AIC INT MODE(%d)`, then the
  same epint notify pair.

So **0x09 and 0x2f are a second, cheaper wake-up** - op6 minus the audio side
effect. If the card has a "produce the next frame" command distinct from
"start", this is it, and it is the natural thing to fire per frame where M119
and M120 fired op 0x06.

#### Two epint nodes, selected by a mode word

`epint` versus `epint_1080p` is chosen by `[.data+4] == 7`. Our SET_VIC byte6
(`fw`) is the encoder selector - 7 spawns `./tinyvenc7`, else `./tinyvenc5`
(M71) - so the `== 7` test is very likely that same value, meaning **tinyvenc5
listens on `epint` and tinyvenc7 on `epint_1080p`.** That is a clean, testable
account of M88's otherwise unexplained result that `fw=7` produces nothing:
wrong encoder binary AND a notify aimed at the node it is not blocked on.

#### The no-signal gate

All three handlers first read a flag byte at `[r5+0x71c]` and, if it is nonzero,
jump to `printk("$$$ cmd(%d) => no signal")` and do nothing else. **The card
refuses START and both wake-ups outright while it believes there is no signal.**
Worth knowing before firing any kick: a wake-up sent while that flag is set is
silently discarded, which would look exactly like "the kick did not help".

#### Next (static first, hardware last)

1. Find what sets/clears `[r5+0x71c]` and whether the host can read it. If it can,
   it is a far better streaming oracle than the buffer poison scan.
2. Confirm `[.data+4]` is SET_VIC byte6 by finding its writer in the 0x29 handler.
3. Then, on hardware after a cold boot: send op `0x2f` (or `0x09`) after each
   poll-drained frame instead of op `0x06`. The plumbing already exists -
   `op6_kick_ms` fires post-delivery (M120); only the opcode changes.


---

## M126 - the splash frame measured, and the receiver output stage reopened

Sources cross-referenced this milestone: the Windows collection
(`/run/media/wolffyx/Work/hd60-trace/collect-2026-08-19/`) and
`elgato-gchd` (`src/gchd/configure_hdmi.cpp`).

### The delivered frame is a splash, and we now know its shape

Decoded `/tmp/cap-m55.nv12` (3110400 bytes, the M112 baseline capture) instead
of eyeballing it in ffplay:

| plane | content |
|---|---|
| Y | `0x11` everywhere except one band |
| UV | `0x80` everywhere, **one unique value in the whole plane** |
| ink | rows **437-641**, cols **5-1100**, 180 rows, a few hundred pixels each |

So: video black, strictly monochrome (UV never deviates), with an
anti-aliased text band vertically centred on line 539. The card composited a
full 1920x1080 surface and drew its own splash into it. Its compositor and its
DMA are both healthy; the VIC simply never wrote live pixels into that surface.

**This also corrects the M125 ordering.** The `[r5+0x71c]` no-signal flag was
demonstrably **CLEAR** during this run - if it were set, op 0x06 would have
been discarded too and no frame would exist at all. The flag is not what
produces the splash. Cadence (op 0x2f) and content (VIC input) are two
independent gates, and the 0x2f lead is unaffected by the splash.

### gchd is at the same I2C address, so its structure IS comparable

`configure_hdmi.cpp` drives 7-bit **0x4e** = our 8-bit **0x9C**, banked through
register `0x00` with a `0xcc` base (`0xcc`/`0xcd`/`0xce` = bank 0/1/2). Bank-0
register numbers therefore line up with ours directly. M123's "values are not
transferable" stands; the structure was never checked.

What the structure says (`configure_hdmi.cpp:170-186`, `:205-211`):

```
0x00 = 0xcc                                  # bank 0
0xb2 = 0xc4 (1080) / 0xcc (720) / 0xcf (SD)  # RESOLUTION-DEPENDENT
0xb5 = 0xd0 (1080) / 0xcc (720) / 0xcc (SD)  # RESOLUTION-DEPENDENT
...
0xb0 = 0xe8      # config written with bit0 CLEAR
0xb1 = 0x0c
0xad = 0xc9
0xb0 = 0xe9      # bit0 set LAST, as the final write of the bring-up
```

Two things we do differently and have never tested:

1. **0xb2 and 0xb5 are fixed constants in our driver** (`0x00`, `0x0c`) where
   gchd derives both from the input resolution.
2. **We commit 0xb0 in one write with bit0 already set**, inside the `0xab`
   bit7 freeze. gchd treats bit0 as the last thing asserted, after the config
   is in place. That is not the M96 experiment - M96 measured `0x20` as a
   *terminal* value and (correctly) got nothing.

Four-way comparison:

| reg | ours | hdcapm | gchd 1080p | Windows `0x9c` trace |
|---|---|---|---|---|
| `0xb1` | `0xc0` | `0xe0` | `0x0c` | `0xc0` **confirmed** |
| `0xb2` | `0x00` | `0x08` | `0xc4` | value unresolved (`?`) |
| `0xb5` | `0x0c` | - | `0xd0` | not seen |

`0xb2` is the only one of the three with no Windows reference value at all, and
the only one both siblings write non-zero while we write zero.

### Device 0x98 - static verdict: board-gated, but not settled

M124 retracted "0x98 is the blocker" on the grounds that its traffic is CSC
matrices for a display path. That retraction covered `0x1402625f4` /
`0x140262834`. It does **not** cover `0x14024cdc0`, which is a different
function entirely - disassembled this milestone:

* It is **table-driven** from a 24-byte-per-entry table at `0x1402D3B90`,
  keyed by a mode id in byte 0. Byte 20 of each entry is the frame rate
  (`0x3c`/`0x32` = 60/50) and bytes 16-19 are the pixel clock, little-endian:
  entry `0x01` = 25,175,000 (VGA), `0x04` = 74,250,000, **`0x10` =
  148,500,000 = 1080p60**. The decode is self-verifying.
* It writes device `0x98` registers `0x90 0x91 0x95 0x96 0x97 0xa0 0xa1 0xa2
  0xa3 0xa6` from that table, then **`0xb1 = 0x0c` and `0xb2 = 0xea` as hard
  constants**.
* `0xb1 = 0x0c` is byte-identical to gchd's `0xb1` on its receiver, and gchd
  writes the same 0x98-side registers (`0x0c 0x0e 0xa2 0xc0`) that the Windows
  trace touches on `0x98`.

**But it is gated.** Entry does `test r8b, 2; je <return>`, branches on
`byte [ctx+0x12]` (against 0 and against 2), and its only caller
(`0x14024d29c`, itself called from `0x14024c74c`) first reads `0x98` register
`0x09` and compares the result to `0xb2` - a chip-ID probe. `[ctx+0x12]` is a
board/decoder-type selector, and this driver serves NVP6134 / TECHPOINT /
SA7160 boards as well as ours. Static analysis cannot say which arm our board
takes.

**Cheap discriminator, zero card spawns:** read `0x98` registers `0xb1` and
`0xb2` on our board with `/proc/mz0380-periph-scan` (`periph_chip=0x98`,
`mz0380-m83-i2c-devscan.sh`). If they already read `0x0c` / `0xea`, something
already programmed them and the avenue is closed for good. If they read
anything else, the block is unwritten on our card and worth one spawn.

### Implemented (no behaviour change at defaults)

New module parameters, defaults equal to the previously hard-coded values:

| param | default | m55 env |
|---|---|---|
| `mst_b1` | `0xc0` | `MSTB1` |
| `mst_b2` | `0x00` | `MSTB2` |
| `mst_b5` | `0x0c` | `MSTB5` |
| `mst_b0_late` | `0` | `B0LATE` |

`mst_b0_late=1` writes `0xb0` as `value & ~1` inside the `0xab` freeze, then
re-writes it with bit0 set after the unfreeze - gchd's order.
`mz0380_mst3367_output_diag()` now also reads back `0xb5`, so every register in
the sweep appears on the existing output-stage line.


### M126 results (hardware, 4 spawns, one cold session)

Baseline + three single-variable runs, every other knob at its default, scored
with `mz0380-m126-score.py` on the chroma plane rather than on file size.

| run | `0xb2` readback | frame |
|---|---|---|
| control | `00` | splash |
| `MSTB2=0x08` (hdcapm) | `08` | splash |
| `MSTB2=0xc4` (gchd 1080p) | `c4` | splash |
| `B0LATE=1` | `00`, `b0=21` | splash |

The readbacks confirm every write landed - `0xb2` is not being rejected. And
the four captures are not merely "all splash": they are **byte-identical**.
Every run reports the same histogram to the sample - `Y 17 x2059998, 15 x989,
12 x694`, 200 unique luma values, 180 non-flat rows, one unique chroma value.

**`0xb2` is NEUTRAL and is now closed**, across the full spread of values any
sibling driver uses. `B0LATE` likewise produced no change (see the caveat
below).

That the frame is bit-identical under three different receiver output-stage
configurations is the stronger result: the splash is a fixed card-side asset,
and **no register in the receiver's output stage influences what the encoder
consumes.** Taken with M99 (Windows block), M100/M101 (`0xb0`, all four
values), M96 (`0xb0` external sync), M99 (`0xad`, `0xb4`) and M124 (our init
is a superset of hdcapm's), the MST3367 output stage is now empirically
exhausted as a content lever. Stop sweeping it.

**Caveat on `B0LATE`, stated rather than glossed:** the run shows `b0=21` at
all three diag points, which is the expected end state either way, and the
`0xb0 committed in two steps` line is printed at mode-commit time, outside the
sections `m55` greps. The run is therefore consistent with the two-step commit
having happened and having done nothing, but it does not prove the path
executed. `sudo dmesg | grep 'two steps'` after a `B0LATE=1` run settles it.

### M126: what the I2C device scan actually showed

| device | result |
|---|---|
| `0x9c` MST3367 | 38/64 zero, structured values - positive control good |
| `0x98` | 10/64 zero, varied high-entropy values - **fitted and ACKing** |
| `0x90` | **64/64 zero - does not ACK. Not fitted on this board.** |

So the board carries two I2C slaves, not the four the static Windows trace
suggests: `0x9c` and `0x98`. `0x90`, the "alternate CSC path" that M124 already
doubted, is absent - that half of the four-device map is settled.

One useful byte: `0x98` register `0x09` reads **`0xff`**. The Windows caller
chain (`0x14024c74c` -> `0x14024d29c` -> `0x14024cdc0`) *writes* `0x09 = 0xb2`
(at `0x14024c642`) and then *reads it back* and compares against `0xb2` - a
write-and-verify presence test, not a hard-wired chip ID. `0xff` means nothing
has ever written it, on this board, by anything.

**Correction to my own plan:** the discriminator I proposed - read `0x98`
`0xb1`/`0xb2` and see whether they already hold `0x0c`/`0xea` - was NOT
answered by this scan. `mz0380-m83-i2c-devscan.sh` scans `0x00..0x3f` only, and
both registers sit above that window. The scan as run cannot speak to it.
`periph_start` already existed as a module parameter; the script now plumbs it
through as `START`, so `START=0x90 sudo scripts/mz0380-m83-i2c-devscan.sh 0x40`
covers `0x90..0xcf` and actually runs the test. Zero spawns.

### M126: `kick_opcode`

`op6_kick_ms` now fires `kick_opcode` (default `0x06`, unchanged) instead of a
hard-coded `MZ0380_CMD_START_STREAMING`, so M125's lead can be run:

    sudo POLLDRAIN=20 OP6KICK=16 KICKOP=0x2f scripts/mz0380-m55-real-capture.sh 3
    sudo dmesg | grep -c 'inferred H.264 length='   # cadence: want > 1

Score on the delivery count, never the file size (M118). The poll-drain summary
line now names the opcode it fired.


### M126: op 0x2f measured - neutral as a single post-frame kick

    poll-drain stopped after 1 deliveries, 1 kicks (op 0x2f)

One frame delivered at t+0.15 s, one kick fired immediately after it, and the
drain then polled for a further **56 seconds** and saw nothing. The stop poison
scan shows all four buffers untouched, so no second frame landed anywhere -
buf0 reading `aa` at stop is the M38 re-poison after delivery, not a
contradiction.

**But this is a one-kick experiment.** The kick lives inside the `if (handled)`
branch (M120), so a one-frame stream fires exactly one wake-up. That cannot
separate "the card ignored 0x2f" from "we only ever asked once, and tinyvenc5
had already exited". Verdict on 0x2f: **not yet earned.**

Added `kick_repeat` (def 0): once the FIRST frame has been delivered, repeat the
kick every `op6_kick_ms` regardless of whether a new frame arrived. The
M119/M120 hazard was kicking *before* the first frame and racing tinyvenc5's
start-up read of SET_VIC; gating on `delivered > 0` keeps that impossible. The
first kick's return code is now logged, so a rejected command is visible
instead of being swallowed by the async send.

    sudo POLLDRAIN=20 OP6KICK=16 KICKOP=0x2f KICKREP=1 scripts/mz0380-m55-real-capture.sh 3
    sudo dmesg | grep -c 'inferred H.264 length='

### M126 gotcha: sudo resets the environment

`START=0x90 sudo scripts/mz0380-m83-i2c-devscan.sh 0x40` ran, printed a full scan,
and **silently scanned `0x00..0x3f` anyway** - `sudo` does not forward
environment variables from the caller. Every knob has to sit AFTER `sudo`,
which is why `sudo POLLDRAIN=20 ./mz0380-m55-...` has always worked:

    sudo START=0x90 scripts/mz0380-m83-i2c-devscan.sh 0x40

The scan header prints the register window it actually used
(`regs 0x00..0x3f`). Read it before reading the values - this class of failure
looks exactly like a completed experiment.

Related: `sudo dmesg | grep 'two steps'` returning nothing does NOT settle
whether `B0LATE` executed. `m55` and `m83` both run `dmesg -C`, and an m83 scan
ran between the `B0LATE` capture and the grep. The evidence was cleared, not
absent. Grep in the same shell command as the run, or not at all.


### M126: op 0x2f, repeated - accepted, useless, and it DESTROYS THE LOCK

    poll-drain: first kick op 0x2f ret=0
    poll-drain stopped after 1 deliveries, 1171 kicks (op 0x2f)

1171 wake-ups over ~56 s, every one accepted by the mailbox, **one frame**. All
four buffers untouched at stop.

And the input path came apart under it:

| | before START | after START | at stop |
|---|---|---|---|
| `R55` | `7f` LOCKED | `7f` LOCKED | **`03` no-lock** |
| link | HDMI | HDMI | **DVI** (`B1 01=80`) |
| `0xb7` | `00` | `00` | **`02`** |

`R55` went `7f` -> `83` -> `03` about 3.5 s after the kicks began and never
recovered, through the whole operator power-cycle window - in every other run
this session it returned to `7f` within 400 ms. This is the **same signature as
M120's free-running op 0x06**: zero further frames and a lost lock. `0xb7`
moving is new; nothing in this driver writes it.

So the two "cheaper wake-up" opcodes behave like op 0x06, including its damage.
**The host-side wake-up avenue is closed.** Do not flood these opcodes again.

#### What one-frame-then-silence actually means

Put together with the splash: the card emits exactly one frame per stream, its
content is a card-side asset, no host ack / credit / wake-up changes either
fact, and each fresh `m55` run - i.e. each fresh encoder spawn - yields exactly
one more frame.

That is the profile of **tinyvenc5 exiting after its first frame**, not of a
running encoder waiting to be nudged. Its own string is
`"Can't create video capture -> exit"`, and `vpl_vic.ko` fails init with
`(CCIR or width chck fail)` when `stat & 0x17 == 0x10`. A process that fails
VIC init, emits the no-signal splash once and quits explains every
observation - and explains why M117, M118, M119, M120, M124's buffer-ack idea
and now M125's 0x2f are all neutral: there is nothing left alive to wake.

**Cadence and content are one bug, not two.** The M126 opening claim that they
are independent gates was wrong - they are independent *symptoms* of the same
failure. The target is VIC init.

#### Which re-opens the geometry, with a number this time

M76 swept `vic_in_w` over exactly two values, 1920 and 3840, and scored every
pass as `captured 0 bytes`. That scoring is the pre-M112 delivery bug: **no
setting could have produced a non-zero result in that harness**, so M76's
"width hypothesis DEAD" verdict is a rule-1 violation of the kind this file
already documents, not a measurement.

The Windows `DriverEntry` trace supplies a geometry M76 never tried:

    [MEMORY] [00466000] [0034BD00] [0034BD00]
    0x466000 = 2048 x 1125 x 2   + 4096
    0x34BD00 = 2048 x 1125 x 1.5 + 256

**stride 2048, height 1125** - the vtotal, not the 1080 active lines. Windows
sizes its capture surfaces for a 2048x1125 frame while we declare 1920x1080,
and `VIC_SetSizeToVIC` picks its wide/narrow mode from the programmed width at
a 2560 threshold (M106/M108), so width is a value the VIC genuinely acts on.

Now testable properly: `POLLDRAIN` delivers, and `mz0380-m126-score.py` scores
on the chroma plane instead of on a byte count that used to be structurally
zero.

    sudo POLLDRAIN=20 VICINW=2048 VICINH=1125 scripts/mz0380-m55-real-capture.sh 1
    sudo POLLDRAIN=20 VICINW=2200 VICINH=1125 scripts/mz0380-m55-real-capture.sh 1

#### 0x98 above 0x3f - read, and it is untouched

`START=0x90` (after `sudo`) covered `0x90..0xcf`:

    0x9c: b0=20 b1=c0 b2=00 b3=00 b4=54 b5=0c   <- exactly what we wrote, module unloaded
    0x98: b1=6a b2=77                            <- NOT 0x0c / 0xea

The `0x9c` window doubles as a control and passes: it reads back our own init
values with the driver gone. `0x98`'s `0xb1`/`0xb2` are **not** the constants
`0x14024cdc0` writes, so nothing on this board has ever run that block.

The scan is also reproducible - the two `0x00..0x3f` passes are byte-identical -
so `0x98` is a real, stable register file, not bus noise.

**Caveat before anyone writes it:** the readout is high-entropy with almost no
zeros, which is not what a video chip's power-on defaults usually look like,
and this driver has never written this device. It could equally be a
configuration EEPROM. Windows' own probe is a write-and-verify
(`0x09 = 0xb2`, read back, compare), so the safe first move is that single
register, not the `0x14024cdc0` block.


### M126: geometry is neutral too - six byte-identical splashes

    vic_in=2048x1125   ret=0   splash
    vic_in=2200x1125   ret=0   splash

Both echoed back by SET_VIC, both scored on chroma. Every capture this session -
control, `MSTB2` at `00`/`08`/`c4`, `B0LATE=1`, `2048x1125`, `2200x1125` - is
**byte-identical**: `Y 17 x2059998, 15 x989, 12 x694`, 200 unique luma, 180
non-flat rows, one unique chroma. Six configurations, one frame.

So the Windows `[MEMORY]` geometry does not move it either, and M76's verdict -
reached on an oracle that could not have produced any other answer - happens to
have been right. It is right *now*, on a sound measurement.

### M126: opcode 0x50 decoded - it is SET_OSD, and that closes it

M32 listed `0x50` as "the only host->card channel never decoded... this is where
a bitstream-buffer / encoder-config handoff would live". Disassembled
(`0x140289f94`, callers `0x14028caa1` / `0x14028e10e`):

* sends the command **twice**, count 12 each time, 32 payload bytes per send
  from `movups xmm0/xmm1` - i.e. a **64-byte string**, split in two chunks, with
  a chunk bit (`ebx |= 0x10`) on the second;
* `strlen()` of the string is computed inline and packed into word[2] together
  with three small integers; word[3] carries two more;
* the caller loops `edi` from 0 to **0x18 (24)** over a table at **stride 0x41
  (65 bytes)** - 24 strings of up to 64 chars plus NUL - reading four parallel
  dword arrays at stride 0xc00 for the integers.

tinyvenc5 names it outright:

    [tiny5] SET_OSD [%02x] ch[%d], line[%d], psz_length=%d, font_style=%d,
            font_size=%d, position(x,y)=(%d,%d)

String plus length plus font style, font size and an (x,y) - exactly the shape
above, 24 OSD lines. `libtextrender.so.0` is in the card rootfs. **Opcode 0x50
is on-screen-display text, not a capture handoff.** M32's remaining-lead #1 is
closed; do not spend a spawn on it.

### M126: the VIC's config file is IN THE BLOB, and it is nearly all fixed

`tinyvenc5` reads `nullsensor_1920x1080.cfg`, copies it to
`/tmp/nullsensor_yuan.cfg`, and patches exactly nine keys - its own string
table lists them:

    input format / output format / start x position / start y position /
    input frame width / input frame height / maximum frame width /
    flip video / mirror video

The template (readable, `yuan_demo_sdi/nullsensor_1920x1080.cfg`):

    1920   // maximum frame width      <- patched
    1080   // maximum frame height     <- NOT patched
    1920   // captured frame width     <- NOT patched
    1080   // captured frame height    <- NOT patched
    0 / 0  // start x / y position     <- patched
    1      // output format (1:YUV420, 2:YUV422)
    6      // input format (6: BT1120p, 7: BT1120i)
    1920   // input frame width        <- patched
    1080   // input frame height       <- patched
    1      // field mode (1: one interleaved field)

So the capture geometry the VIC actually uses is **hard-coded 1920x1080 in the
template**, and `input frame width/height` - the only width the host can move -
is a separate declaration. That is the concrete form of M106/M108's "the printed
width is a driver local, not host-supplied", and it explains why 1920, 2048,
2200 and 3840 all produce the same result: none of them changes what the VIC
captures.

The library's own constraint strings, for the record:

    The sum of the capture width and the start pixel must be less or equal to input width.
    The max frame width must be larger or equal to capture width.
    [VIDEOCAP][ERROR]: Fail to do vpl_vic device driver ioctl (IO Number %d) !!
    [tiny5] Can't create video capture-------------> exit !!!

**Consequence: the host-reachable VIC configuration surface is now fully
enumerated and fully swept.** Nine keys, all of which we already drive, none of
which changes the outcome. The failure is not in what we tell the VIC.

#### Correction made and withdrawn within this milestone

On first reading `video_capture_mgr`'s SET_VIC printf I took `nosg` to mean "no
signal" and suspected we were asking for the splash. We are not:
`is_nosg` is byte 31 and comes from `stream_nosg`, which defaults to 0
(mz0380-dma.c:1223), and `vic_nosg` only colours that path. The project's field
map was already correct. The name does mean "no signal" rather than
"no scatter-gather", which is worth having straight, but nothing follows from it.

---

## M127 - the card-side pipeline read end to end; three prior verdicts corrected

Static only. Zero hardware, zero encoder spawns, no upload. Everything below is
read out of the stock blob (`MZ0380.HD.HEX.stock`) with `llvm-objdump` plus a
literal-pool/GOT string resolver; every file involved is unstripped.

### 1. What actually runs on the card, and when

`etc/rc.local` is the steady-state boot path. It starts **`video_capture_mgr -D -P 5`**
and does *not* start tinyvenc5. `yuan_start_process.sh` - which does launch
`./tinyvenc5 -D -c nullsensor_1920x1080.cfg` - is the first-boot / restore path
only.

So `video_capture_mgr` is the resident daemon. It polls `/sys/vpl_pciep/epint`,
and on SET_VIC it writes `/tmp/nullsensor_yuan<N>.cfg` and then
`system("./tinyvenc5 -D [-L] -a .. -w ..")`. It also has `killall -9 tinyvenc5`
and `[Video_MGR] timeout ---->break(%d)`. **tinyvenc5 is per-stream, and it is
video_capture_mgr - not tinyvenc5 - that owns the cfg.**

`EncodingGroup::Start` ends its failure path in `exit()` (0x11260). tinyvenc5
dying is therefore consistent with the hard wedge (a wall of `SET_VIC ret=-110`,
nothing left to ACK), but it cannot be what happens on a *normal* stream,
because a normal stream returns a frame.

### 2. SET_VIC (0x29) - the complete 40-byte layout, verified from two sides

`ep.ko`'s `epint_show` memcpy's `rodata[0xa0 + cmd]` bytes of the mailbox to the
card's userspace. That table is the per-opcode payload length:

| cmd | 0x06 | 0x07 | 0x09 | 0x29 | 0x2a | 0x2d | 0x2f | 0x31 | 0x50 | 0x51 | 0x52 | 0x60 | 0x61 | 0x62 | 0x6e |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| len | 8 | 8 | 8 | **40** | 20 | 44 | 44 | 20 | 44 | 20 | 7 | 16 | 8 | 12 | 24 |

`video_capture_mgr`'s own SET_VIC printf marshals those 40 bytes, and the
offsets fall out of the disassembly exactly:

```
 0..3  cmd = 0x29        20..21 x_start            32     vanc_lines
 4     ch                22..23 y_start            33     fast_kill
 5     fps               24..25 input_frame_width   34     -
 6     fw                26..27 input_frame_height  35     is_slave
 7     input format      28     bitstream_num       36     nosg_back_color
 8..9  width             29     osd_enabled         37     nosg_y
10..11 height            30     osd_size            38     nosg_u
12     m                 31     is_nosg             39     nosg_v
13     flip
14     mirror
16..19 color_info[4]
```

This matches the project's existing map. Two notes:

- **Byte 7 is the cfg `input format` enum, not an interlace bool.**
  `video_capture_mgr` labels it `interlace(%d)` in its printf, which invites the
  wrong conclusion. Follow the value, not the label: it is passed as the 5th
  argument to the cfg writer and `sprintf`'d **raw** into the line matching
  `"input format"` (patcher at 0xa3a8, arg resolved at 0xa5f8-frame +0x00 =
  `ldrb [r12,#7]` at 0x91f0). `6 = BT1120p, 7 = BT1120i` stands. Our driver is
  right; do not "fix" this.
- Bytes 40..41 exist in video_capture_mgr's *per-channel* struct but are past
  the 40-byte epint payload, so they are not host-settable through SET_VIC.

### 3. The cfg is patched far more than "nine keys" - M126's claim is wrong

M126 concluded the capture geometry is hard-coded in the template and only nine
keys are patched. That was read off tinyvenc5's string table. The patcher is in
`video_capture_mgr` (0xa290) and it works two ways:

- **by comment**: `input format`, `output format` (a hard `1`), `start x position`,
  `start y position`, `input frame width`, `input frame height`, `flip video`,
  `mirror video`, `maximum frame width`.
- **by value**: it `atoi()`s every line and rewrites **any line whose value is
  1080** with SET_VIC `height` (bytes 10..11), and any line valued 1920 with
  either `maximum frame width`'s source or SET_VIC `width` (bytes 8..9).

So `captured frame width/height` and `maximum frame height` *are* driven from
the host - through SET_VIC bytes 8..11, not through `input frame width/height`
(bytes 24..27, the `vic_in_w`/`vic_in_h` knobs M76 swept). **The width
experiments moved the wrong field.**

### 4. ep.ko's sticky `no_signal` latch

`pciep_isr` op 0x29 (0x171c): if `width == 0 || height == 0` it prints
`SET_VIC (CH %d) NOSG ... size(%dx%d)`, sets a global `no_signal = 1`, MSIs the
host and **does not `sysfs_notify`** - nothing spawns. Once latched, ops
`0x06, 0x2a, 0x2f, 0x31, 0x50, 0x51, 0x52, 0x60, 0x61, 0x62` all early-out with
`cmd(%d) => no signal`. Only a SET_VIC with non-zero w/h clears it.

On the success path it stores `(fw == 7) ? 7 : 5` and notifies `epint_1080p`
when that is 7, otherwise `epint`. That is the whole of what `fw` does in ep.ko.

### 5. tinyvenc5's command surface

`main` opens `/sys/vpl_pciep/epint`, `read()`s 44 bytes, and requires word0 ==
41 (`GOT command 0x%X, why?????? should be SET_VIC_PARAMS`). It ACKs by
`pwrite`ing the same 44 bytes back. Then `poll()` + `pread()` in a loop, with a
jump table for cmd 6..98. Implemented commands, exhaustively:

`0x06 0x09 0x2a 0x2d 0x2f 0x31 0x50 0x51 0x52 0x62` - everything else is ignored.

### 6. Where the splash comes from - and it is not a VIC-init failure

`NOSG_LOGO_Y` / `NOSG_LOGO_YUV422` are **data symbols inside tinyvenc5**, drawn
by `EncodingGroup::fake_frame_process`. `init_func` starts that thread whenever
`preview_params_settings[ch].byte[0x0a] == 0`, which `main` zeroes at startup -
so the fake-frame thread is *always* running alongside `encode_handler`. It is
the standby source, not an error path.

The real path drops frames in `libtkmf_video_source.so.0`:

```
[yuan][tkmf] (Drop this frame) Tiny_Set(%d x %d)!= VIC_Get( %d x %d)(stride %d),
  Count = %d, idx = %d, Time = %d:%03d , dwInWidth: %d,
  bCCIRErr: %d, bNoSignal: %d, bFifoFull: %d
```

and `VideoCap_GetBuf` (libvideocap 0x43f0) reads those flags out of the buffer
descriptor: `+0x28 = bFifoFull`, `+0x2c = bNoSignal`.

`bCCIRErr` originates in `vpl_vic.ko`'s ISR. `dwVICMmrStat = MMR[0x30] >> (ch*8)`
for ch<=3 (`MMR[0x34] >> (6*ch-24)` above that), and the failing test is
`(stat & 0x17) == 0x10` at 0x1358/0x137c - bit 4 set with bits 0..2 clear. The
message is rate-limited by `dwErrPrintPeriod`.

**So the model is: capture initialises fine, real frames are rejected, and the
standby thread emits the built-in no-signal logo.** M126's "the target is the
VIC init failure" does not survive - if `TKMF_VideoSrc_Init` had failed,
tinyvenc5 would have `exit()`ed and there would be no frame at all.

### 7. Host-readable card state (already wired, worth using)

`store_channel_done` (ep.ko 0xdc8, reached from `livectrl_ioctl`) writes the
card's own view into BAR0: per-channel nibble counters at `0x40/0x44/0x48/0x4c`
and the EVENT word at `0x30`. `encode_status_storeN` writes
`/sys/vpl_pciep/enc_stat<N>` to `BAR0 + 0x50 + N`. The driver already reads
both (`MZ0380_MB_FRAME_TOKEN`, `MZ0380_MB_ENC_STATUS`).

### M127b - the splash identified by byte identity, not inference

Run: `sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 1`, card healthy, one frame,
receiver `R55=0x7f` LOCKED 1920x1080p60 HDMI, `htot=2200 vtot=1125`.

The captured Y plane is **`NOSG_LOGO_Y`, the 320x240 data symbol at tinyvenc5
+0x6b10c (0x12c00 bytes)**, blitted into an otherwise `0x11` frame:

- histogram match is exact - the asset has `15 x989, 12 x694`, the capture has
  `15 x989, 12 x694`;
- 179 of the asset's 240 rows are found **verbatim** in the capture (the rest are
  flat rows, which match trivially);
- located, every row lands at `y = 420 + r, x = 800`, destination stride 1920 -
  a perfectly centred `(1920-320)/2, (1080-240)/2` memcpy.

So the frame is `EncodingGroup::fake_frame_process` doing exactly its job. It is
not a failure artefact, not a scaled asset and not an OSD render. **The splash
oracle is now byte identity against a known asset; the chroma heuristic
(`UV unique == 1`) can be retired.** M126's ink measurement of "rows 437-641,
cols 5-1100" was contaminated by a single stray non-flat row near y=0.

Corollary: because the delivered frame is a fixed asset, "frames byte-identical"
carries **no gradient**. Every sweep scored that way was only ever a binary
splash / not-splash detector. Those verdicts still stand as negatives, but no
sweep of that shape can ever rank two settings.

#### The stop-line counters are zero-ambiguous

```
stream stop: EVENT[0x30]=00000000 token[0x40]=00000000 0x44=00000000 0x48=00000000
             0x4c=00000000 enc[0x50]=00000000 irq_total=4 frame_events=0
```

None of that means "the card never wrote". `MZ0380_MB_ENC_STAT_FREE` is itself
**0**, so the driver arms `0x50` with 0 and reads 0 back. And
`store_channel_done` (ep.ko 0xdc8) writes `count - 1` into the per-channel
nibble, so a one-frame channel legitimately writes 0 to `0x40/0x44/0x48`. To
make these readable, arm them with a non-zero sentinel instead.

### M127c - I2C 0x98 identified: it is an ITE HDMI *transmitter*, lead closed

The answer has been sitting in M83's bank sweep since it was run (RE_FINDINGS
~line 2291) and was recorded as a curiosity rather than read as a chip ID:

```
98 bank0..3: identical rows (54 49 12 16 1c 60 00 00 00 ff ff ff 00 00 6c 08)
```

- `0x00/0x01 = 54 49` -> little-endian **`0x4954`**, the ITE Tech vendor ID
  (`"IT"` in ASCII).
- `0x02/0x03 = 12 16` -> device `0x612`, revision 1.
- All four "banks" read identically, so it does not implement MST3367-style
  bank select - it is a flat register file, not a receiver sibling.

That is an **IT66121 / IT6612-class HDMI transmitter**, and 8-bit `0x98`
(7-bit `0x4C`) is its stock address. On this board it drives the HDMI **output**
passthrough.

Everything about the Windows traffic now fits: a per-mode table keyed by pixel
clock (entry `0x10` = 148,500,000 = 1080p60), YCbCr->RGB coefficients, and the
`0xb1 = 0x0c` / `0xb2 = 0xea` constants are transmitter output setup. M92
retracted this lead on the grounds that the traffic was CSC; the retraction was
right, and the chip ID now makes it final.

**`0x98` is not on the capture path. Do not spend another run on it.** It is
also not a configuration EEPROM, so the "write-and-verify `0x09` first" caution
from the M126 handoff is moot - and we had already written its register `0x00`
during the EDID bank hunt with no ill effect.

Residual value, for later and unrelated to the blocker: this is how HDMI
passthrough would be implemented, and we have never initialised it.

**Bus inventory is now complete and every device is identified:**

| 8-bit | 7-bit | device | role |
|---|---|---|---|
| `0x9c` | `0x4e` | MST3367 | HDMI receiver - the capture path |
| `0x98` | `0x4c` | IT66121 | HDMI transmitter - output passthrough |
| `0x90` | `0x48` | - | not fitted (64/64 zero at two windows) |
| `0xa0` | `0x50` | - | not fitted (no EDID EEPROM; EDID is inside the receiver) |

### M127d - hdcapm init diff, and the one register never validly measured

Fetched the GPL `hdcapm` MST3367 driver (stoth68000/hdcapm, `mst3367-drv.c`)
and diffed its `mst3367_init_setup()` against ours. We match it almost
everywhere - `0xab=15 0xad=05 0xb4=54 0xb5=0c 0x51=89`, BANK2 `01=61 02=f5
07=04`, and we do write its 36-byte CSC table at `0x92..0xb5`. Two notes:

- **`0xb0..0xb5` are the tail of the CSC table, not a standalone output stage.**
  hdcapm writes `0xb0..0xb5` in `RxVideoInit`, then clobbers them with the CSC
  block at `0x92..0xb5`, then re-writes `0xb0` last. So our observed
  `b1=c0 b2=00 b5=0c` are CSC bytes and agree with hdcapm's *final* state; the
  `0xb1=0xe0 / 0xb2=0x08` in its `RxVideoInit` are transient and never survive.
  M126's byte-at-a-time `b1/b2/b5` sweeps were sweeping single cells of a
  matrix, which is why they were flat.
- `0xae |= 0x04` does not stick on our board (reads back `0x20`, bit2 clear).
  Already known and documented in `mz0380-mst3367.c`; unchanged here.

The MST3367 has **no 16-bit or 20-bit output mode**. Its only `0xb0` options,
from the driver's own comments:

| `0xb0` | meaning |
|---|---|
| `0x20` | RX_OUTPUT_YUV422 / 08.BITS / EXTERNAL SYNC - **what hdcapm ships** |
| `0x21` | RX_OUTPUT_YUV422 / 08.BITS / EMBEDDED SYNC - **what we ship** |
| `0x24` / `0x25` | RX_OUTPUT_YUV422 / 10.BITS / EXTERNAL SYNC |

`vic_b0` is the single register where we knowingly differ from the reference
driver for the same receiver, on the bit that selects embedded vs external
sync - a first-order property of the pixel bus. **And it has never been validly
measured.** core.c:1008 records why: the `vic_b0=0x20` run was made while
`win_seq=1` was still the default, and M90 proved `win_seq=1` renders nothing at
all. The result was confounded by a second variable known to suppress the
outcome being measured - method rule 1, exactly.

Next run, one spawn, everything else at the known-good baseline:

```bash
sudo POLLDRAIN=20 VICB0=0x20 scripts/mz0380-m55-real-capture.sh 1
```

Confirm `b0=20` in the `output stage` readback lines before believing anything -
that is what makes it a measurement rather than a repeat of the wasted one.

### M127e - a real splash oracle

`mz0380-m127-splash.py` replaces the chroma heuristic. It SHA-256s the 320x240
crop at (800,420) and compares against the known digest of `NOSG_LOGO_Y`
(`dfce4efd...83a30b`). Exit 1 = SPLASH, 0 = NOT SPLASH, 2 = NO FRAME. The asset
is vendor firmware and is deliberately not vendored - only its digest is.

### M127f (hardware, 2026-08-21): TWO SPAWNS WASTED - this replicates M96

    sudo POLLDRAIN=20 VICB0=0x20 scripts/mz0380-m55-real-capture.sh 1
    sudo POLLDRAIN=20 VICB0=0x20 VICINW=3840 scripts/mz0380-m55-real-capture.sh 1

Both landed (`b0=20` at all three diag points, `vic_in=3840x1080` in the SET_VIC
line) and both produced **nothing**: 0 deliveries, `0/1024` on all four buffers,
`buf0` solid `aa`, capture 0 bytes.

**This is M96 re-run.** `vic_b0=0x20` had already been measured validly, and
M100 had already decoded `0xb0` from hdcapm's four commented variants:
bit0 = embedded(1)/external(0) sync, bit2 = 10-bit(1)/8-bit(0). The known table
was already complete before this session started:

| `0xb0` | = | result |
|---|---|---|
| `0x21` | 8-bit, embedded | splash (baseline) |
| `0x25` | 10-bit, embedded | splash, bit-identical (M101) |
| `0x20` | 8-bit, external | nothing (M96, re-confirmed here) |
| `0x14` | 10-bit, external - Windows' value | nothing (M80, M94) |

**How the error happened, because the mechanism is reusable.** core.c:1008 says
"that is exactly how the `vic_b0=0x20` test was wasted", written about the
*M73-era* attempt that ran under `win_seq=1`. I read that as "`0x20` has never
been validly measured" and did not grep RE_FINDINGS for the actual result. M96
had re-run it properly after `win_seq` was defaulted to 0. **A caveat in a
source comment describes the state at the time it was written; it is not a
statement about the current result set.** Check the findings file, not the code
comment, before calling anything untested.

I also re-derived M96's "the period counters halved, so bit0 is a clock/width
select" - and the second run refuted it in-place: with `b0=0x20` held constant
the counters went `674 -> 337 -> 674` and `lines 1125 -> 1127 -> 1125` across
source re-locks. It is a settling state, not a `b0` effect. M100 had already
retracted this from the register decode. Nothing new either way.

**Net new information from two spawns: none.** The only thing added is a second
confirmation that `0x20` renders nothing.

#### Where that leaves the actual question

M100's conclusion stands and is the live one: embedded sync is required for the
VIC to initialise at all, bit2 is neutral, so **we are on the right side of the
CCIR half of `(CCIR or width chck fail)` and the width half is what is left**.

The untested cell is `vic_in_w = 3840` **at a `b0` that reaches VIC init**.
M76 ran 3840 pre-M112, when the harness scored `captured 0 bytes` for every
setting and could not have returned a different answer (the M126 handoff already
flags that verdict as needing re-measurement). M126 re-ran 2048 and 2200 at
`b0=0x21` but not 3840. And the two runs above put 3840 against `b0=0x20`, which
never reaches VIC init, so they say nothing about width.

```bash
sudo POLLDRAIN=20 VICINW=3840 scripts/mz0380-m55-real-capture.sh 1
scripts/mz0380-m127-splash.py /tmp/cap-m55.nv12
```

Defaults elsewhere (`b0=0x21`, `fw=5`, `win_seq=0`, `in_fmt=6`). Confirm both
`b0=21` and `vic_in=3840x1080` in the log.

### M127g RESULT (hardware, 2026-08-21): `vic_in_w = 3840` is NEUTRAL - M76 is now validly dead

    sudo POLLDRAIN=20 VICINW=3840 scripts/mz0380-m55-real-capture.sh 1

Both knobs confirmed in the log: `b0=21` at all three diag points,
`vic_in=3840x1080` in the SET_VIC line. One frame, and
`mz0380-m127-splash.py` returns **SPLASH** - SHA-256 `dfce4efd...83a30b`,
byte-identical to `NOSG_LOGO_Y`.

This is the re-measurement the M126 handoff asked for. M76's original 3840 run
was scored `captured 0 bytes`, which was structurally zero for *every* setting
before M112, so it could not have returned a different answer. Run at a `b0`
that reaches VIC init, with a delivery path that works and an oracle that
identifies the picture, **3840 is genuinely neutral.** The 8-bit double-rate
width hypothesis is dead on its merits rather than on a broken harness.

`vic_in_w` is now swept at 1920 / 2048 / 2200 / 3840, all splash.

### M127h - our MST3367 init is a SUPERSET of hdcapm's

Checked every register in `mst3367_init_setup()` against `mz0380-mst3367.c`:

    0x41 0xb8 0x0f 0x16 0x17 0x18 0x19 0x1a 0x2a 0x24 0x30 0x31 0x32
    0x1e 0x1f 0x73 0x90 0x91 0xac 0xb7

All present. The only apparent gap, `0xe2` (hdcapm: "DISABLE AUTO POSITION"),
is not a gap - we drive it dynamically through `mst3367_set_auto_position()`
(auto-position on until a coherent mode is recognised, then off), which the run
logs confirm (`(auto-position off)` on every locked detect). That is a
deliberate improvement on hdcapm, not an omission.

**So there is no missing receiver-init register.** Combined with M127d (the
`0xb1/0xb2/0xb5` "output stage" is CSC-matrix cells, and we write the same table
hdcapm does) and M100/M101 (`0xb0` fully decoded and swept), the receiver-side
configuration surface is exhausted.

### M127 session summary - where the host-side search now stands

Swept and neutral, all with a valid harness and the byte-identity oracle:

| surface | values tried | result |
|---|---|---|
| `0xb0` sync/width | `0x21` `0x25` (embedded) / `0x20` `0x14` (external) | embedded -> splash; external -> nothing renders |
| `0xb1/0xb2/0xb5` | ours / hdcapm / gchd | splash (and they are CSC cells - M127d) |
| receiver init | full hdcapm register set | we are a superset - M127h |
| SET_VIC `in_fmt` | 3, 6, 7 (0 = illegal) | splash |
| SET_VIC `vic_in_w` | 1920, 2048, 2200, 3840 | splash - M127g |
| SET_VIC `is_nosg`, `fast_kill`, `color_info`, `m` | Windows values | splash |
| host wake-ups | op `0x2f` x1171, op `0x06` flood | splash, and the receiver loses lock |
| I2C `0x98` | - | IT66121 transmitter, off the capture path - M127c |

The receiver holds a clean `R55=0x7f` 1080p60 HDMI lock in every one of these.
The VIC reports no signal in every one of these. **No host-reachable
configuration changes that.**

What remains genuinely untested, in descending order of prior:

1. **`fw = 6`** - the value Windows sends. The one run that tried it
   (RE_FINDINGS ~3486) carried other changes and was **explicitly
   retro-invalidated at ~3536**; it has never been run as a single variable.
   Same failure shape as M76's width verdict, which turned out to be worth
   re-measuring. Note `fw=6` still selects tinyvenc5 (only 7 -> tinyvenc7,
   8 -> tinyvenc8) and still notifies `epint`, so it is a small change - it
   alters the cfg's output format to 2/YUY2 (~3406). Low prior for fixing a
   *capture* fault, but it is cheap and it is the last invalidated verdict.
2. **tinyvenc5 opcodes `0x2f`, `0x31`, `0x51`, `0x62`** - still undecoded
   (handlers at 0xebdc, 0xef24, 0xea78, 0xe854). Static, free.
3. **GPIO pins other than 1/3/8/9** via op `0x15`. Expensive to evaluate - each
   candidate needs a stream to score.

The honest position: the remaining discriminating information is
`dwVICMmrStat` inside the SoC, and it is not host-reachable. Reading it needs a
card-side change, which the standing no-upload rule forbids. Plan around that.

---

## M128 - the tinyvenc5 dispatch table decoded; the splash has an off switch

Static RE only. Zero encoder spawns, no hardware run. Blob unpacked read-only
per the recipe in NEXT_SESSION_START.md; everything below is from
`llvm-objdump -d --triple=armv5te-linux-gnueabi tinyvenc5` plus the symbol
table (the binary is unstripped, and every `EncodingGroup` static is a named
`B` symbol, which is what makes the struct offsets readable).

### The dispatch table

`main`'s command loop reads 44 bytes from `/sys/vpl_pciep/epint` and switches on
the first word at `main+0x804`:

```
    e6d0: sub  r3, r2, #6
    e6d4: cmp  r3, #92
    e6d8: ldrls pc, [pc, r3, lsl #2]      @ table base 0xe6e0
    e6dc: b    0xe654                      @ default: back to the top, silently
```

so index = cmd - 6, 93 entries, everything unlisted falls through without a
word. Resolving the table against each handler's own format strings:

| cmd | handler | len | card's own name |
|---|---|---|---|
| `0x06` | 0xe954 | 8 | `START_STREAMING` |
| `0x09` | 0xe93c | 8 | bare ACK - `pwrite(epint, payload, 44)`, nothing else |
| `0x29` | pre-loop | 40 | `SET_VIC_PARAMS` |
| `0x2a` | 0xed48 | 20 | `SET_AIC_PARAMS` |
| `0x2d` | 0xebdc | 44 | `SET_ENC_PARAMS` |
| `0x2f` | **0xebdc** | 44 | `SET_ENC_PARAMS_POST` |
| `0x31` | 0xef24 | 20 | `SET_PREVIEW_PARAMS` |
| `0x50` | 0xeb8c | 44 | `SET_OSD` |
| `0x51` | 0xea78 | 20 | `SET_BAR` |
| `0x52` | 0xea34 | 7 | `SET_VIDEO_INVISIBLE` |
| `0x62` | 0xe854 | 12 | `SET_LOGO` |

Two structural facts fall straight out:

- **`0x29` in the loop is a no-op.** Index 35 maps to the default. tinyvenc5
  reads SET_VIC exactly once, before the loop (0xe60c), and that pre-loop
  handler (0xf264) only `puts` its ACK banner and `pwrite`s the payload back.
  A second SET_VIC to a running tinyvenc5 is discarded in silence.
- **`0x2d` and `0x2f` are the same handler.** The only difference is the banner
  (`cmp r2,#45` at 0xebec picks `"[tiny5] SET_ENC_PARAMS "` vs
  `"[tiny5] SET_ENC_PARAMS_POST "`) and a `main`/`sub` tag. Everything it
  writes lands in `EncodingGroup::main_encode_settings` (144 bytes/ch,
  72 bytes/stream) - gop, qp, profile, bitrate, IDR, skip, avg, entropy,
  aspect, resize, hdr_opts, crop.

### Three of the four targets are closed

`0x2f`, `0x51` and `0x62` are encoder-side or cosmetic. None of them touches
VIC, capture, or the receiver:

- **`0x2f` = SET_ENC_PARAMS_POST.** H.264 knobs. It cannot affect capture, which
  retires M125's flood of 1171 as noise on principle rather than on evidence.
- **`0x51` = SET_BAR.** A colour-bar overlay rect, per (channel, line), stored
  in `EncodingGroup::bar_settings` (16 bytes/entry, index `ch*2 + line`):
  `[4]=ch [5]=line [6..7]=is_show [8..9]=x [10..11]=y [12..13]=w [14..15]=h
  [0x10]=update [0x11..0x13]=y,u,v`. x+w and y+h are clamped against
  `preview_settings[ch]` width/height, printing
  `"[tiny5]---------> wrong1 x=%d, w=%d"` / `wrong2 y=%d, h=%d` and zeroing the
  offending pair. Overlay only.
- **`0x62` = SET_LOGO.** `[4]=ch [5]=is_show [6]=reload [7]=pic_order
  [8..9]=x [10..11]=y` into `EncodingGroup::logo_settings` (8 bytes/ch); loads
  `/tmp/PIC_LOGO_%d`, max 320x240. Overlay only.

### `0x31` is SET_PREVIEW_PARAMS, and byte 0x0e is `fake_frame_off`

The handler at 0xef24 and its verbose printf at 0xf570 give the whole payload:

```
[4..7] mask (u32)   [8] ch   [9] fps   [0x0a] skip   [0x0b] avg
[0x0c] die_en       [0x0d] preview_off [0x0e] fake_frame_off
[0x0f] preview_no_osd  [0x10] mirror   [0x11] flip   [0x12] hw_d
```

The mask is sticky-OR'd into `preview_params_settings[ch].u32[0]` and gates
exactly one field - byte `0x0c` (die_en), mask bit 4, cleared after use.
Bytes `0x0d`..`0x12` are stored **unconditionally**, so `fake_frame_off`
lands whatever the mask says.

Store map (`MLA r3, r5, #40, lr` with `lr = preview_params_settings`):
byte `0x0e` -> `preview_params_settings[ch].byte[0x0a]`.

That is the exact byte M127 named as the standby-splash gate. Both halves now
line up from opposite directions:

```
main   (0xe400)  strb r3, [r6, #0xa]        @ r6 = pps; zeroed at startup
init_func (0x10ac0)
        ldrb r1, [r2, #0x52]                @ r2 = 0x7dd38 + ch*40, +0x52-0x48 = pps[ch][0x0a]
        cmp  r1, #0
        beq  0x10bf0 -> pthread_create(..., EncodingGroup::fake_frame_process, this)
```

There is a **second** spawn site, in `EncodingGroup::Start` (0x10f30), gated by
`preview_settings[0].byte[0x1b]` instead - which is `is_nosg`, and we already
send 0. So on the baseline the splash thread exists for exactly one reason:
nobody has ever told the card to turn it off.

### Why this is worth one spawn

The "real frames are rejected in `libtkmf_video_source.so.0`" mechanism is
static RE only. The card's console is unreachable (no-upload rule), so the drop
has never been *observed* - it is inferred. Suppressing the standby thread
removes the only other frame source, which splits the two remaining stories:

- **NO FRAME** -> the rejection is real; nothing reaches the encoder at all.
- **a real frame** -> the standby thread was winning the race and masking it,
  and the whole "VIC sees no signal" reading has been measuring the wrong thing.

Note the guard is read **once**, at `pthread_create` time, inside the `0x06`
handler (`new EncodingGroup` -> `pthread_create(on_start_thread)` -> `Start` ->
`init_func`). So `0x31` is only effective **before** `START_STREAMING`.

### Corroboration and one correction to the Windows read

M82 decoded this same packet from the Windows driver and labelled byte `[14]`
`board_flag_b`. It is `fake_frame_off`, and Windows sends 0 - i.e. **retail
leaves the standby splash armed too.** M82 also had mirror/flip the other way
round from the card's printf; the card wins. `di` = `die_en` is right.

### Code

- `fake_frame_off` module param (def 0, baseline unchanged). Sets `0x31`
  byte `0x0e`, and on the `win_seq=0` path sends `0x31` before `0x06`, which
  the baseline otherwise never sends at all.
- `MZ0380_CMD_POST_PROC` keeps its name and value; the full field map and the
  dispatch table are now in `mz0380-reg.h`. Added
  `MZ0380_CMD_SET_ENC_PARAMS_POST`/`SET_BAR`/`SET_VIDEO_INVISIBLE`/`SET_LOGO`.
- Fixed the M71 comment block in `mz0380-dma.c`: byte 7 is the cfg
  `input format` enum, not an interlace bool. The code was always right.

Builds clean. **Not yet smoke-tested or run on hardware** - `mz0380-m85-unload-smoke.sh`
needs root and this session had no sudo.

### Next

1. `sudo scripts/mz0380-m85-unload-smoke.sh`, then one spawn:
   `sudo POLLDRAIN=20 EXTRA="fake_frame_off=1" scripts/mz0380-m55-real-capture.sh 1`
   scored with `mz0380-m127-splash.py` (expect 2/NO FRAME or, if the race
   reading is right, 0/NOT SPLASH with real content).
2. `fw = 6`, one spawn - still the last invalidated verdict.
3. GPIO pins beyond 1/3/8/9.

### M128a (hardware, 2026-08-21): fake_frame_off lands, the splash is gone, and the frame is a 16-byte stall

One spawn. `sudo POLLDRAIN=20 EXTRA="fake_frame_off=1" scripts/mz0380-m55-real-capture.sh 1`,
default everything else (`win_seq=0`, `fw=5`, `in_fmt=6`, bufs after SET_VIC).

The knob landed - method rule 9 satisfied before reading anything else:

    stream start: SET_PREVIEW_PARAMS(op 0x31, mask=0x1f, fps=60, die_en=1, fake_frame_off=1) ret=0
    stream start: START_STREAMING(op 0x06) fired (async, ret=0)

Ordering is right: `0x31` at 19046.701388, `0x06` at 19046.701399, 11 us apart.
The receiver held `R55=0x7f` LOCKED coherent 1920x1080p60 throughout, across
three HPD pulses, and the output stage never twitched (`b0=21 b1=c0 ... 51=89`
identical before START, after START and at stop).

Result:

    poll-drain: buf 0 holds 16 of 3110400 bytes - DMA still in flight, waiting   (x17, over 57 s)
    stop buf[0] head=53 53 53 52 52 52 52 52 52 51 53 53 53 54 54 55 | 1/1024 pages touched
    buf0 +0x00: 53 53 53 52 52 52 52 52 52 51 53 53 53 54 54 55
         +0x10: aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa aa
    captured 0 bytes

`mz0380_infer_frame_length()` scans **backwards** from the end of the 4 MiB
buffer for the last non-poison qword, so `len == 16` is exact: bytes 0..15 were
written and every byte after them is untouched `0xaa`. One 128-bit burst.

#### Two things changed, and only one of them is new

**Not new: the 16-byte stall itself.** That is M91's exact signature
(RE_FINDINGS ~3665), and M91 saw it under `win_seq=1` - whose sequence also
contains `0x31`. So "op `0x31` truncates the DMA after one burst" is a live and
parsimonious reading, and it would incidentally answer M91's open question
("something else in `win_seq` truncates the DMA after one 16-byte burst").
M91's own suspect was `win_bufs_first` and the iATU latch, but that does not
transfer here: this run had bufs in the M23 position, which has always
permitted a full 3.1 MB write.

**New: the sixteen bytes are not the splash.** Every previous run that reached
this point wrote `11 11 11 11 11 10 11 11 ...` - the standby thread's 0x11
canvas with its 1-LSB dither. M91's sixteen bytes were `11 11 11 11 10 10 11 11
11 11 11 11 11 11 11 11`. This run wrote a dithered flat field at **0x53**,
about 83, a mid-dark grey. Nothing on the card is known to paint 0x53. So
`fake_frame_off=1` did what the static RE said it would: the standby generator
did not run, and whatever produced those bytes is not it.

#### What this run cannot settle

It changed two variables at once against the baseline - it sent `0x31` at all
(the `win_seq=0` path never does) **and** it set byte `0x0e` inside it. Method
rule 1: the step is not informative until the other one is pinned. Both live
stories fit the evidence:

- **`0x31` truncates.** Then the 16 bytes are a broken transfer of whatever was
  in the frame buffer, and the 0x53 is only "not the splash canvas", not
  "captured pixels".
- **`fake_frame_off` works and the real path is producing.** Then the standby
  thread was the only thing ever completing a frame, the real path manages one
  burst and dies, and 0x53 is source luma.

#### The control, built

`post_proc=1` (new, `mz0380-core.c`) sends `0x31` on the `win_seq=0` baseline
with `fake_frame_off` left at 0 - one variable from this run, every other
variable at a value already known to permit a full frame:

```bash
sudo POLLDRAIN=20 EXTRA="post_proc=1" scripts/mz0380-m55-real-capture.sh 1
```

    full 3110400 B splash  -> 0x31 is harmless. The stall belongs to
                              fake_frame_off, i.e. to the real capture path,
                              and the 0x53 is real luma. Then the blocker moves
                              from "the VIC sees no signal" to "the real path
                              delivers one burst and stops" - a different, and
                              much more tractable, bug.
    16 bytes of 0x11       -> 0x31 truncates. That closes M91 too, and
                              fake_frame_off has to be retested some other way.

A second, cheaper discriminator if a spawn is spare: repeat the
`fake_frame_off=1` run with the source showing something drastically brighter
or darker. If those sixteen bytes track the source, they are pixels and the
control barely matters.

**Do not read the 0x53 as proof of capture yet.** It is the first non-splash,
non-black data the real path has ever produced, which is worth exactly one
control run to confirm and not more than that.

### M128b (hardware, 2026-08-21): the control fires - op `0x31` truncates the DMA, and M91 is closed

One spawn. `sudo POLLDRAIN=20 EXTRA="post_proc=1" scripts/mz0380-m55-real-capture.sh 1`.
`fake_frame_off=0`, everything else exactly as M128a.

    stream start: SET_PREVIEW_PARAMS(op 0x31, mask=0x1f, fps=60, die_en=1, fake_frame_off=0) ret=0
    stream start: START_STREAMING(op 0x06) fired (async, ret=0)
    poll-drain: buf 0 holds 16 of 3110400 bytes   (x17)
    stop buf[0] head=11 11 11 11 10 10 11 11 11 11 11 11 11 11 11 11 | 1/1024 pages touched
    captured 0 bytes

**Sixteen bytes, and they are M91's sixteen bytes byte for byte.** Receiver held
`R55=0x7f` LOCKED throughout; output stage identical at all three sample points.

| run | 0x31 | fake_frame_off | result | head |
|---|---|---|---|---|
| baseline (M88 etc.) | no | - | full 3110400 B | `11 11 11 11 11 10 11 ...` |
| M91 (`win_seq=1 OP6=1`) | yes | 0 | 16 B | `11 11 11 11 10 10 11 ...` |
| M128a | yes | **1** | 16 B | `53 53 53 52 52 52 52 52 ...` |
| M128b (control) | yes | 0 | 16 B | `11 11 11 11 10 10 11 ...` |

Two conclusions, one of them a correction to M128a:

1. **op `0x31` truncates the DMA after one 16-byte burst.** It does so with
   `fake_frame_off` at either value, so the stall belongs to `0x31`, not to the
   byte. M128a's stall is therefore NOT evidence about the real capture path.
2. **M91 is closed.** "Something else in `win_seq` truncates the DMA after one
   16-byte burst" is `0x31`. M91's own suspect - `win_bufs_first` and a stale
   outbound iATU - is wrong: M128b had the buffers in the M23 position, which
   has always permitted a full 3.1 MB write, and still stalled.

**What survives from M128a:** the head content. Same command, same everything,
one byte different, and the sixteen bytes went from the standby thread's 0x11
canvas to a dithered flat field at 0x53. `fake_frame_off=1` does reach the card
and does stop the splash generator painting. That is n=1 against n=2 and it is
not proof the 0x53 is captured luma - it may be uninitialised card memory - but
whatever else it is, it is not the splash.

#### It is not what `0x31` *means* - the handler is inert on this card

Static, free, done before proposing another run. The ch-0 path of the handler
(tinyvenc5 0xf020) falls through to:

    f060: fopen("/tmp/PIC_INSERT", "rb")
    f06c: subs r4, r0, #0 ; beq 0xffac
    ffb8: strb r5, [r6]          @ r6 = g_insert_pic (0x7ed12), r5 = 0
    ffbc: beq 0xe93c             @ -> the bare 44-byte ACK

`/tmp/PIC_INSERT` does not exist, so `fopen` returns NULL, `g_insert_pic` is
cleared and the handler ACKs. The `MemBroker_GetMemory(w*h*2)` allocation at
0xf090 - the one thing in the handler that could plausibly move a DMA target -
is **never reached**. Nothing the handler does touches capture, DMA or the VIC.
The mask bits only steer which of `skip`/`avg` get stored and a
`tiny_calculate_skip_fps` call, all of which rejoin at 0xf020.

#### What `0x31` changes is the cadence in front of START

From the two runs' own timestamps:

    baseline    SET_AIC -> [156 ms, spent in the output-stage diag] -> 0x06
    with 0x31   SET_AIC -> [156 ms diag] -> 0x31 -> [9 us] -> 0x06

**Nine microseconds.** `0x06` is fire-and-forget (`timeout_ms = 0`), so it hits
the doorbell while tinyvenc5 is still inside the `0x31` handler. ep.ko serves
one command at a time out of the mailbox and pokes the card with a bare
`sysfs_notify("epint")`; every other command in the sequence is tens to hundreds
of ms apart. A first burst landing correctly and everything after it going
nowhere is what a start that raced its own configuration looks like - and it
explains M91 without needing `win_bufs_first`.

#### Next, one variable, one spawn

`post_proc_gap_ms` (new) inserts a wait between `0x31` and `0x06`:

```bash
sudo POLLDRAIN=20 EXTRA="post_proc=1 post_proc_gap_ms=200" scripts/mz0380-m55-real-capture.sh 1
```

    full 3110400 B splash -> cadence. 0x31 is usable with a gap, which is what
                             fake_frame_off needs to be testable at all. Then
                             re-run fake_frame_off=1 WITH the gap, and the
                             splash oracle finally answers the real question.
    16 bytes of 0x11      -> the race is not it; the 0x31 handler needs another
                             read, and the M128a 0x53 stays unexplained.

Note this also matters beyond `0x31`: if a 9 us command-to-START gap is enough
to truncate the stream, the same hazard sits in `win_seq=1`, which is why that
path "renders nothing" (M90/M91). Fixing it may unblock the Windows ordering
as a whole.

### M128c (hardware, 2026-08-21): the cadence hypothesis is dead; `0x31` bypasses ep.ko's no_signal latch

One spawn. `sudo POLLDRAIN=20 EXTRA="post_proc=1 post_proc_gap_ms=200" scripts/mz0380-m55-real-capture.sh 1`.
One variable from M128b.

    stream start: SET_PREVIEW_PARAMS(op 0x31, mask=0x1f, fps=60, die_en=1, fake_frame_off=0) ret=0
    stream start: waiting 200 ms before op 0x06 (M128b cadence test)
    stream start: START_STREAMING(op 0x06) fired (async, ret=0)
    poll-drain: buf 0 holds 16 of 3110400 bytes   (x16)
    stop buf[0] head=11 11 11 11 10 10 11 11 11 11 11 11 11 11 11 11 | 1/1024 pages

The gap landed (22570.106832 -> 22570.307842, 201 ms) and changed nothing.
**The 9 us race is not the mechanism.** Receiver LOCKED throughout, and with
the 200 ms gap the detect thread even stopped logging `55=a7 settling` churn -
this was the quietest run of the three, and it still stalled.

That is two mechanism hypotheses dead:

| hypothesis | how it died |
|---|---|
| `0x31` reallocates a buffer under the DMA | static: `fopen("/tmp/PIC_INSERT")` fails on this card, so `MemBroker_GetMemory(w*h*2)` at 0xf090 is never reached (M128b) |
| `0x31` races `0x06` (9 us) | hardware: 200 ms gap, identical 16-byte stall (M128c) |

#### ep.ko's dispatch, and a correction to the M127 ABI note

`pciep_isr` (ep.ko 0x1210) switches on the opcode. Nearly every command lands on
a common arm at 0x1824 which first tests the sticky no-signal latch at
`state[0x71c]` and bails to the "ignored" logger at 0x1894 if it is set:

    1824: ldrb r3, [r5, #0x71c]
    1828: cmp  r3, #0
    182c: bne  0x1894              @ ignored
    1830: ...                      @ sysfs_notify(epint)

`0x2f`, `0x50`, `0x51`, `0x52`, `0x62` and `0x2a` all enter at 0x1824. `0x06`
has its own arm at 0x1854 which tests the same latch and then notifies **two**
attributes (0x199c first, then the common 0x1998) - that is the audio_ctrl
notify the docs mention. But:

    13ac: cmp r6, #49
    13b0: beq 0x1830               @ NOT 0x1824

**`0x31` enters at 0x1830, past the latch test.** It is the one opcode in the
set that is NOT gated by no_signal. M127's ABI note lists `0x31` among the
latched ops; that is wrong. Corrected, but it is not the truncation mechanism
either - the latch is clear in these runs (a good SET_VIC precedes them, and
`0x06` on the gated arm is plainly getting through).

#### What `0x31` actually writes into tinyvenc5, in full

Worth having written down, because the remaining suspects are all in here.
With our payload (mask 0x1f, ch 0, fps 60, die_en 1, everything else 0):

    pps[0].u32[0] |= 0x1f          then bits 4, 0, 1 cleared again as consumed
    pps[0][4] = ch = 0
    pps[0][5] = fps = 60
    pps[0][8] = die_en = 1         gated by mask bit 4
    pps[0][6] = skip               gated by mask bit 0
    pps[0][7] = avg = 0            gated by mask bit 1
    pps[0][9]    = preview_off      = 0   unconditional
    pps[0][0x0a] = fake_frame_off         unconditional
    pps[0][0x0b] = preview_no_osd   = 0   unconditional
    pps[0][0x0c] = mirror           = 0   unconditional
    pps[0][0x0d] = flip             = 0   unconditional
    pps[0][0x0e] = hw_d             = 0   unconditional
    preview_settings[0][0x30] = 0         unconditional
    g_insert_pic = 0                      via the failed fopen

One card-side rewrite we did not ask for: at 0xefac the handler tests
`w * h * fps > 0x041eb000` (68,952,064). 1920x1080x60 = 124,416,000, so we take
it, and at 0xf5d0 - because we send avg = 0 - it runs `if (skip <= 1) skip = 2`
**on the payload buffer**, then stores that 2 into `pps[0][6]`. We ask for
skip 0 and the card records skip 2.

#### The ladder, in order, each one variable

`fake_frame_off` is stored **unconditionally** - it needs no mask bit. So the
minimal `0x31` is also the one that still delivers what we want.

1. **`post_proc=1 post_mask=0`** - the smallest possible `0x31`. Clearing the
   mask drops the die_en store (bit 4), the skip store and the avg store
   (bits 0/1) and takes the short arm at 0xf014. Strictly fewer card-side
   writes than mask 0x1f.
   ```bash
   sudo POLLDRAIN=20 EXTRA="post_proc=1 post_mask=0" scripts/mz0380-m55-real-capture.sh 1
   ```
   full frame -> `0x31` is usable. Immediately follow with
   `post_proc=1 post_mask=0 fake_frame_off=1`, which is the experiment this
   whole thread exists to run.
   16 bytes -> die_en and the forced skip are both exonerated, and the cause is
   in the unconditional block above or in the epint traffic itself.

2. **`post_proc=1 post_proc_opcode=0x09`** - the control for "any extra command
   before START". `0x09` is inert on both sides: same 0x1824 arm in ep.ko, and
   tinyvenc5's handler at 0xe93c is a bare `pwrite(epint, payload, 44)`.
   ```bash
   sudo POLLDRAIN=20 EXTRA="post_proc=1 post_proc_opcode=0x09" scripts/mz0380-m55-real-capture.sh 1
   ```
   16 bytes -> the fault is structural: an extra epint command immediately
   before START truncates the stream whatever it is. That is a far bigger
   finding than `0x31`, and it would explain the whole `win_seq` ordering
   failing (M90/M91).
   full frame -> `0x31`'s own writes are the cause; combine with (1) to
   localise.

3. Only if both are clean and the stall persists: re-read the unconditional
   block, starting with `preview_settings[0][0x30]`, whose reader has not been
   found yet.

**Honest status:** `0x31` truncates, reproducibly, three runs. Why is still
open, and I have been wrong about it twice.

### M128d (hardware, 2026-08-21): both ladder steps clean - the truncator is a mask-gated store, and `0x31` is now usable

Two spawns, run back to back.

**Step 1 - `post_proc=1 post_mask=0`:**

    stream start: SET_PREVIEW_PARAMS(op 0x31, mask=0x00, fps=60, die_en=1, fake_frame_off=0) ret=0
    stream start: START_STREAMING(op 0x06) fired (async, ret=0)
    poll-drain: buf 0 holds 3110400 bytes with no completion event; delivering
    captured 3110400 bytes = 1 whole frames + 0 bytes

**Step 2 - `post_proc=1 post_proc_opcode=0x09`:**

    stream start: SET_PREVIEW_PARAMS(op 0x09, mask=0x1f, ...) ret=0
    poll-drain: buf 0 holds 1475200 of 3110400 bytes - DMA still in flight, waiting
    poll-drain: buf 0 holds 3110400 bytes with no completion event; delivering
    captured 3110400 bytes = 1 whole frames + 0 bytes

Both full frames. Step 2 even caught the transfer mid-flight at 1475200 bytes
and then complete 21 ms later - a healthy DMA, which is exactly what the
16-byte runs were not.

Scored on the host, no spawn:

    scripts/mz0380-m127-splash.py /tmp/cap-m55.nv12
    sha256  dfce4efd5139298f544d23473f85a42fb7115a3c5e4ba65b71c070d59883a30b
    VERDICT: SPLASH - byte-identical to tinyvenc5's NOSG_LOGO_Y.

#### The result

| run | opcode | mask | result |
|---|---|---|---|
| M91 / M128b / M128c | 0x31 | 0x1f | 16 bytes |
| M128a | 0x31 | 0x1f | 16 bytes |
| **M128d step 1** | 0x31 | **0x00** | **full 3110400 B, SPLASH** |
| **M128d step 2** | **0x09** | 0x1f | **full 3110400 B, SPLASH** |

- **An extra epint command immediately before START is harmless.** The `0x09`
  control is clean, so the structural reading is dead - it is not "any command",
  and it is not the cadence (M128c already killed that with a 200 ms gap).
- **`0x31` itself is harmless.** With mask 0 it delivers a whole frame.
- **The truncator is one of the three mask-gated stores.** mask 0x1f enables
  exactly three writes that mask 0 does not:

      bit 0 -> pps[ch][6] = skip     (which the card had forced to 2, M128c)
      bit 1 -> pps[ch][7] = avg = 0
      bit 4 -> pps[ch][8] = die_en = 1

  Everything else in the handler is unconditional and ran identically in both.
- **M90/M91 are explained.** `win_seq=1` sends `0x31` with `post_mask` at its
  default 0x1f. That is why the Windows ordering "renders nothing at all" -
  not the ordering, one field in one command.

Prior on which bit: **die_en**. It is a de-interlace engine being switched on
for a progressive 1080p60 source, and it is set to 1 for exactly one reason -
M82 saw Windows send 1. That is the same byte-parity reasoning that already
cost us `fw=7` (method rule 4: parity is a hypothesis generator, not a rule).
`skip=2` is second: we ask for 0 and the card rewrites it.

#### `0x31` is now usable, so the experiment can finally run

`fake_frame_off` is stored **unconditionally** - it needs no mask bit. So the
minimal `0x31` carries it:

```bash
sudo POLLDRAIN=20 EXTRA="post_proc=1 post_mask=0 fake_frame_off=1" scripts/mz0380-m55-real-capture.sh 1
scripts/mz0380-m127-splash.py /tmp/cap-m55.nv12
```

This is the run this whole thread exists for, and it now has a working delivery
path and a validated oracle under it. Read it as:

    1 / SPLASH     -> fake_frame_off did not take. Check the dmesg line first.
    2 / NO FRAME   -> the standby thread was the ONLY frame source. The real
                      path produces nothing, the rejection in
                      libtkmf_video_source.so.0 is real, and the VIC genuinely
                      sees no signal.
    0 / NOT SPLASH -> a real frame. The standby thread was masking it, and the
                      no-signal reading has been measuring the wrong thing for
                      the entire project.

Then, separately and worth a spawn each because it bears on the whole Windows
ordering: bisect the mask bit with `post_mask=0x10` (die_en alone) against
`post_mask=0x01` (skip alone).

#### Unrelated observation, logged not chased

Both runs show the detect thread reporting `hper=337 vper=299 lines=1127`
alongside the usual `hper=674 vper=599 lines=1125`, at the same `htot=2200
vtot=1125 hact=1920` and still `MATCHED`. Exactly half the usual hper/vper.
Seen only after the frame was delivered in step 1, but throughout step 2. Not
touched here.

---

## M129 (hardware, 2026-08-21): REAL VIDEO. The card was capturing all along.

    sudo POLLDRAIN=20 EXTRA="post_proc=1 post_mask=0 fake_frame_off=1" scripts/mz0380-m55-real-capture.sh 1

    stream start: SET_PREVIEW_PARAMS(op 0x31, mask=0x00, fps=60, die_en=1, fake_frame_off=1) ret=0
    stream start: START_STREAMING(op 0x06) fired (async, ret=0)
    poll-drain: buf 0 holds 3110400 bytes with no completion event; delivering
    captured 3110400 bytes = 1 whole frames + 0 bytes

    scripts/mz0380-m127-splash.py /tmp/cap-m55.nv12
    sha256  eea4e3bf875845d453abd5911f7ef9b63fa72ec24dd4ece524e64e73fb50f5f7
    VERDICT: NOT SPLASH
             Y outside the crop: 170 distinct values
             UV plane:           144 distinct values

**It is a photograph of the scene in front of the camera.** Confirmed visually
by the operator and by ffplay. Raw frame preserved as
`m129-first-real-frame.raw` (sha256
`76e7050e53dfe68b033c3aa6659a68c2013671333e1dbe812bfd4697c7cc8704`).

Objective check before anyone called it, so that "it looks like an image" is not
the evidence:

    Y   min=78 max=247 mean=144.94 std=45.64
    row-to-row corr (Y[r] vs Y[r+1])   = 0.9665
    col-to-col corr (Y[:,c] vs [:,c+1]) = 0.9703
    same, pixel-shuffled control        = -0.0008
    22.02% of pixels have |dY/dx| > 8
    quadrant Y means: 149.7 / 157.4 / 105.0 / 167.7
    rows with mean < 20 (video black): 0 of 1080

0.97 neighbour correlation against -0.0008 shuffled is not uninitialised DRAM
and not a flat field. The luma plane on its own renders as a clean, sharp,
artefact-free 1080p greyscale frame - no tearing, no banding, correct geometry.

### What this overturns

**The "VIC reports no signal" reading was wrong.** The card's standby thread was
drawing NOSG_LOGO_Y over a working capture path, and every previous run measured
the standby thread. Specifically:

- `EncodingGroup::fake_frame_process` was not a fallback for a dead input. It
  ran unconditionally alongside `encode_handler` because
  `preview_params_settings[ch].byte[0x0a]` is zeroed at startup and nobody had
  ever told the card otherwise - Windows included (M82's `board_flag_b` = 0).
- The `(Drop this frame) ... bNoSignal` path in `libtkmf_video_source.so.0`,
  which M127 named as the target, is not what was happening. It was never
  observed - the card's console is unreachable - and it was wrong.
- Every "splash" verdict in this file measured the standby thread, not the
  capture. The M127 corollary ("no sweep of that shape can rank two settings")
  stands and is now the explanation for the entire negative result set: the
  sweeps were fine, the oracle was reading a thread that ignores every knob
  they turned.

### The three ingredients, and why it took all three

1. `fake_frame_off = 1` in op `0x31` byte `0x0e` - stops the standby thread ever
   being created (M128, static).
2. `post_mask = 0` - the default 0x1f enables a mask-gated store that truncates
   the DMA to one 16-byte burst (M128d). With mask 0x1f the frame never
   completes, so #1 alone shows nothing.
3. Sending `0x31` at all on the `win_seq=0` baseline, which never did.

M128a had #1 and #3 but not #2, which is why it produced 16 bytes of 0x53 -
that was the real frame, truncated.

### Remaining defect: chroma

Luma is perfect. Chroma is not, and it is not a plane-order problem:

    chroma region 1036800 bytes = two 960x540 planes
    plane A: mean 124.41 std 27.29   row-corr 0.9376  col-corr 0.9489
    plane B: mean 134.93 std 28.20   row-corr 0.9482  col-corr 0.9504
    corr(A, B)          =  0.6598
    corr(A, Y downsampled) = -0.9645
    corr(B, Y downsampled) = -0.7552

Both planes are image-like, so the layout is planar 4:2:0 (decoding as NV12
gives magenta/green interleave banding; as `yuv420p` the banding vanishes and
the geometry is exact). But **plane A is 96% ANTI-correlated with luma**. That
is not colour difference data, it is inverted luma. Colour-difference planes for
a real scene do not track -Y.

Two candidates, in order:

1. **The MST3367 CSC matrix.** Our driver already decodes and logs
   `input colorspace YUV444` from the link registers, and the receiver's job is
   YUV444 -> BT1120 YCbCr 4:2:2. We program hdcapm's 36-byte CSC table at
   `0x92..0xb5` verbatim (M127d/M127h). If that table is an RGB-input matrix and
   the source is sending YUV444, "luma roughly right, chroma tracking -Y" is
   exactly the artefact. This is the top suspect and it is host-fixable.
2. **`out_fmt`.** SET_VIC byte 12 is the output format and we send **0**, which
   M72 already noted is not a legal value; the card falls back to the cfg's
   `output format` (1/YV12 for `fw=5`, 2/YUY2 for `fw=6` - M79). Worth setting
   deliberately. Note `fw=6`/YUY2 changes the frame to 4:2:2, i.e. 4147200
   bytes, and `mz0380_infer_frame_length`'s `want` is hardcoded `w*h*3/2` - that
   needs fixing before any YUY2 run or poll-drain will never see a complete
   frame.

### Also unresolved

- **Possible mirror.** The "Boss" logo at top-left appears mirrored. Could be
  the physical scene; check against it before touching `mirror`/`flip`
  (SET_VIC bytes 13/14, and the SET_PREVIEW_PARAMS bytes 0x10/0x11).
- **Which mask bit truncates** (M128d): `post_mask=0x10` (die_en) vs `0x01`
  (skip). Not needed for capture any more, but it is the whole explanation for
  `win_seq=1` rendering nothing (M90/M91), so it still gates the Windows
  ordering.
- The `hper=337 vper=299 lines=1127` detect readings, exactly half the usual,
  appearing alongside the normal ones.

### Method note

The thing that produced this was the control run, not the hypothesis. M128a
looked like a result and was two variables; the `post_mask=0` and `0x09` controls
cost one spawn each and turned a wrong conclusion into the right one. Three
mechanism guesses were wrong along the way (buffer realloc, the 9 us race, and
"any extra command"); none of them cost a spawn to kill except the last, because
the first died statically and the second died on a knob that already existed.

## M130: the chroma defect diagnosed - an RGB matrix on a YCbCr source

Static + host-side analysis of the M129 frame. No spawns.

### The measurement

    plane A  mean 124.57  std 27.23   corr(A, luma) = -0.9664
    plane B  mean 135.13  std 28.16   corr(B, luma) = -0.7488

Both chroma planes are image-like in their own right (row-corr 0.94, col-corr
0.95), so the layout is planar 4:2:0 and the geometry is right. But plane A is
**96% anti-correlated with luma**. Colour-difference data does not track -Y.

### The mechanism

HDMI YCbCr 4:4:4 assigns Cb to the blue TMDS channel, Y to green and Cr to red.
Feed that to an RGB->YCbCr matrix (BT.601 shown) and it computes:

    Y_out  =  0.257*Cr + 0.504*Y  + 0.098*Cb   -> dominated by Y, looks fine
    Cb_out = -0.148*Cr - 0.291*Y  + 0.439*Cb   -> dominated by -Y
    Cr_out =  0.439*Cr - 0.368*Y  - 0.071*Cb   -> -Y plus a real Cr term

That is the observation exactly: perfect luma, one chroma plane nearly pure
inverted luma, the other inverted luma mixed with something real. Inverting the
model on the captured frame turns the magenta/green mush into a coherent
picture of the actual object - residual cast, because the exact matrix and
range are not pinned, but unmistakably the right shape.

### Why we have an RGB matrix

hdcapm was fetched and read rather than assumed. Its `mst3367-drv.c`:

```c
static inline u32 MST3367_HdmiGetPacketColor(struct v4l2_subdev *sd)
{
	u8 r48 = mst3367_rd(sd, BANK2, 0x48) & 0x60;
	if (r48 == 0x00) color = 0;      /* RX_INPUT_RGB    */
	else if (r48 == 0x20) color = 1; /* RX_INPUT_YUV422 */
	else if (r48 == 0x40) color = 2; /* RX_INPUT_YUV444 */
```

It detects the input colour space, caches it in `regb2r48_cached` - **and never
uses it to choose a matrix.** The CSC table goes out unconditionally:

```c
for (i = 0; i < sizeof(csctbl); i++)
	mst3367_wr(sd, BANK0, 0x92 + i, csctbl[i]);
```

That is fine on hdcapm's board, whose EDID makes sources send RGB. **We push no
EDID at all** (M127 closed EDID as a dead end for making the source transmit),
and this source chose YUV444 - which our own link line has been reporting on
every single run, unremarked, for months:

    link [before START]: ... 48=d2, input colorspace YUV444

So we inherited a bug that cannot fire on the board it came from.

### The table's layout, which is not all coefficients

31 bytes at 0x92..0xB0:

    0x92        0x40                 <- CONTROL, not a coefficient
    0x93..0x98  M11 M12 M13          2 bytes each, big-endian
    0x99..0x9E  M21 M22 M23
    0x9F..0xA4  M31 M32 M33
    0xA5..0xAA  A1  A2  A3           offsets
    0xAB..0xB0  15 95 05 20 C0 08    colour range + output stage

The trailing bytes land on 0xAB (colour range) and 0xB0 (output format), both of
which the init rewrites immediately afterwards - that is the "fixes up 0xb0
last" M127d noticed.

**The coefficient fixed-point format is NOT cracked.** High nibble 7 marks the
negative entries and 0 the positive ones, but no scale tried reproduces a
recognisable BT.601 or BT.709 matrix in either direction. Do not hand-write a
matrix on the strength of a guess.

### The cheap experiment

`0x92 = 0x40` is a single-bit control byte in front of a colour-space
conversion we do not want: the input is YCbCr and the output over BT1120 is
YCbCr 4:2:2. If 0x40 is the enable, clearing it should bypass the conversion.

New knob `mst_csc_ctl` (default 0x40, i.e. no change):

```bash
sudo POLLDRAIN=20 EXTRA="post_proc=1 post_mask=0 fake_frame_off=1 mst_csc_ctl=0" \
     scripts/mz0380-m55-real-capture.sh 1
scripts/mz0380-m130-chroma.py /tmp/cap-m55.nv12
```

The run echoes `MST3367 CSC control 0x92 = 0x00 (hdcapm default 0x40)` when the
override is active - check it before reading the result (method rule 9).

### New oracle: `mz0380-m130-chroma.py`

Companion to the splash oracle. Reports `corr(chroma, luma)` and the plane
means, and verdicts CHROMA OK / PARTIAL / CONTAMINATED. Validated against the
M129 frame, which it correctly calls CONTAMINATED at |corr| 0.97.

Exit 0 = correct, 1 = contaminated or partial, 2 = no whole frame.

### If `mst_csc_ctl=0` does not work

In descending order:

1. Sweep the other bits of 0x92 - it is one byte, and a full sweep is 8 spawns,
   but 0x00/0x40/0x80/0xc0 covers the plausible enable/mode encodings.
2. **Push an EDID that advertises RGB only.** That makes the source send RGB and
   the inherited matrix becomes correct by construction. EDID was closed as a
   dead end for *making the source transmit*, which is a different question -
   the source transmits fine now, we would only be steering its output format.
3. Crack the fixed-point format properly, which probably means finding an
   MStar/MST3367 CSC register description rather than more numerology.

### M130a (hardware, 2026-08-21): `mst_csc_ctl=0` fixes the colour

One spawn.

    sudo POLLDRAIN=20 EXTRA="post_proc=1 post_mask=0 fake_frame_off=1 mst_csc_ctl=0" \
         scripts/mz0380-m55-real-capture.sh 1

| statistic | M129 (`0x92=0x40`) | M130a (`0x92=0x00`) |
|---|---|---|
| `corr(A, luma)` | **-0.9664** | -0.3194 |
| `corr(B, luma)` | -0.7488 | **+0.3492** (sign flipped) |
| share of A that is luma | 0.93 | **0.10** |
| share of B that is luma | 0.56 | **0.12** |
| plane A std | 27.23 | 12.12 |
| luma std | 45.70 | 66.73 |

Rendered as plain `yuv420p` (I420: Y, U, V) it is a **correct, natural-colour
image** - a red pedal on a neutral dark mat, black PCB with gold pads, yellow
sticker, no cast. The U/V-swapped rendering gives the textbook red/blue swap
(blue pedal, brown mat), which confirms plain I420 is the right order.

So `0x92` **is** the CSC control byte, `0x40` enables the conversion, and with a
YCbCr source the conversion is exactly what we did not want. Diagnosis
(M130) confirmed on hardware.

The residual 0.10/0.12 is ordinary scene correlation - bright things are often
also saturated - not a defect. Luma std rising 45.7 -> 66.7 is the range no
longer being squashed by the matrix.

#### The fix, made properly rather than as a knob

`mst_csc_ctl` now defaults to **AUTO**, which does what hdcapm reads the
register for and then never does:

```c
cs = (b2_48 & 0x60) >> 5;                    /* BANK2 0x48, hdcapm's own decode */
want = (cs == 1 || cs == 2) ? 0x00           /* YUV422 / YUV444: do not convert */
                            : 0x40;          /* RGB: hdcapm's RGB->YCbCr matrix */
```

This **cannot** go in `init_regs()`: that runs at bring-up, before HPD is
asserted, so nothing is transmitting and `0x48` is meaningless. New
`mz0380_mst3367_apply_csc_mode()` is called from the stream-start path just
before the "before START" diag, once the receiver has locked. Any explicit
value for the parameter still forces that byte, at init and at stream start.

"undefined" (`cs == 3`) deliberately falls back to hdcapm's `0x40`: it is what
every working board ships, so it is the safer branch when `0x48` has not
settled.

#### Tooling

- `mz0380-m130-chroma.py` recalibrated. It now reports **corr^2** - the share of
  a chroma plane's variance that luma explains - because correlation magnitude
  alone is a bad test: real scenes do correlate colour with brightness. The two
  measured states are an order of magnitude apart (0.93 vs 0.10), so the
  thresholds (>=0.40 contaminated, <=0.25 clean) are not delicate. Verified
  both ways: exit 0 on the fixed frame, exit 1 on the M129 frame.
- `m130-colour-correct-frame.raw` kept in-tree beside `m129-first-real-frame.raw`
  as the before/after pair.
- **`mz0380-m55-real-capture.sh` no longer says NV12.** The payload is planar
  I420 and always was; the `.nv12` filename is historical. The wrong hint cost
  two viewing mistakes in one session. The script now prints `yuv420p` and the
  two scoring commands.

## M131: the working configuration is now the driver's default

No spawns. Method rule 5 - "never let a test harness carry its own copy of a
driver default" - cuts both ways: once a setting is known correct, the DRIVER
should carry it, not a 60-character `EXTRA=` string that is easy to mistype and
easy to forget.

| parameter | was | now | why |
|---|---|---|---|
| `post_proc` | 0 | **1** | it is what carries `fake_frame_off`; the `win_seq=0` path never sent 0x31 |
| `post_mask` | 0x1f | **0** | 0x1f truncates the DMA to 16 bytes (M128d) |
| `fake_frame_off` | 0 | **1** | 0 paints NOSG_LOGO_Y over a working capture (M129) |
| `mst_csc_ctl` | - | **AUTO** | picks the CSC mode from the detected input colour space (M130a) |

So a plain `insmod` should now produce real, correctly-coloured video, and the
capture command is back to:

```bash
sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 5
```

Each old behaviour is still reachable: `post_proc=0` restores the pre-M129
sequence, `fake_frame_off=0` restores the splash, `post_mask=0x1f` restores the
truncation, an explicit `mst_csc_ctl` forces that byte. Reproducing any result
in this file older than M129 needs the first three.

The harness was checked rather than assumed: `mz0380-m55-real-capture.sh` routes
every knob it owns through `add_opt` and none of these four are among them -
they only ever arrive via `EXTRA=`. No drift.

### Next: sustained capture, and it may not even be broken

**Every real-video run so far asked for exactly one frame.** `m55`'s argument is
the frame count (`FRAMES=${1:-6}` -> `v4l2-ctl --stream-count`), and M129/M130a
both passed `1`. "poll-drain stopped after 1 deliveries" was the request being
satisfied, not a ceiling being hit. Buffers 1-3 reading untouched is equally
consistent with "v4l2-ctl dequeued its one frame and the driver stopped
streaming" as with "the card only ever produced one".

`op6_kick_ms` also defaults to **0**, so no wake-up has ever been fired on a
configuration that produces real frames. The whole M117/M118/M120/M126 kick and
credit machinery is present in `mz0380_poll_drain_thread()` and has only ever
been exercised against the splash.

So the next run is just to ask for more, one variable from M130a:

```bash
sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 5
scripts/mz0380-m130-chroma.py /tmp/cap-m55.nv12
```

`timeout -s INT --foreground "$CAPWAIT"` bounds it, so a stall cannot hang the
run; the script truncates to whole frames and says how many it got.

- **5 whole frames** -> sustained capture already works and the "one frame per
  stream" cadence was an artefact of never asking for a second one.
- **1 frame then timeout** -> the cadence is real. Then, in order:
  `op6_kick_ms=33 kick_repeat=1` (the kick machinery, never yet tried with real
  frames), then `poll_drain_credit`.

Note this run also exercises the `mst_csc_ctl=AUTO` path for the first time -
M130a forced `0` explicitly. The run echoes
`MST3367 CSC 0x92 = 0x00 (auto, input colorspace YUV444 from 0x48=d2)`; if
chroma comes back contaminated, suspect AUTO before the frame count.

Not yet smoke-tested: `sudo scripts/mz0380-m85-unload-smoke.sh` should be run first,
as after every build.

## M132 (hardware, 2026-08-21): the one-frame cadence is REAL

    sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 5

First run on the M131 defaults, i.e. no `EXTRA=` at all. The frame count was the
only variable against M130a.

    29111.695  poll-drain: buf 0 holds 91392 of 3110400 bytes - DMA still in flight
    29111.717  poll-drain: buf 0 holds 3110400 bytes ... delivering
    29111.717  frame token 0 inferred ... length=3110400
    29167.389  poll-drain stopped after 1 deliveries, 0 kicks (op 0x06)

**One frame, then 55.7 seconds of nothing.** v4l2-ctl waited for five, got one,
and `timeout -s INT` killed it mid-write - hence 3108864 bytes, short by exactly
the 1536-byte stdio tail, which is the M116 failure mode the script warns about.
The head bytes (`50 50 50 50 51 51 51 51 52 53 55 56 ...`) are real video, not
the 0x11 splash, so the capture itself was fine.

So the M131 hypothesis is dead: asking for more frames does not produce more
frames. The cadence is a property of the card, not of the request. Buffers 1-3
untouched is now meaningful rather than merely consistent.

Worth noting the DMA is fast: 91392 bytes at the first poll and complete 22 ms
later. The transfer is not the bottleneck; nothing asks for a second frame.

### An observability gap of my own making

`mz0380_mst3367_apply_csc_mode()` logs which CSC mode it chose, and section 3 of
the harness greps
`stream start|stream stop|frame token|enc|no HDMI signal|poll-drain` - which
does not match `MST3367 CSC`. So the single line that confirms the colour knob
landed was **filtered out of the output of the very run that first exercised
it**. Exactly what method rule 9 exists to prevent, in a line I added myself
one milestone earlier. Pattern added to the grep.

The chroma could not be scored either way: the truncated file is 1536 bytes
short of a frame, so `mz0380-m130-chroma.py` correctly refuses it (exit 2). The
AUTO path is therefore **still unverified** - carry that into the next run.

### Next: the kick machinery, never yet tried with real frames

`op6_kick_ms` defaults to 0, so `0 kicks` is not a failure - nothing was asked
to fire. The whole M117/M118/M120/M126 apparatus in
`mz0380_poll_drain_thread()` (enc_stat ack, credit re-arm, op6 kick, repeat) has
only ever run against the splash, where the frame source was a standby thread
that ignored all of it. It has never been exercised on a path where a real
encoder is waiting to be asked.

M120's reasoning still holds and is why the kick is gated on a delivered frame:
firing from t=0 races tinyvenc5's start-up read of SET_VIC (M119 - zero frames
AND the receiver lost lock). Gating on `delivered > 0` makes that race
impossible, and M126 made the kick repeat rather than fire once per frame.

Ask for a frame every ~33 ms once the first has landed:

```bash
sudo POLLDRAIN=20 EXTRA="op6_kick_ms=33 kick_repeat=1" scripts/mz0380-m55-real-capture.sh 5
scripts/mz0380-m130-chroma.py /tmp/cap-m55.nv12
```

    5 whole frames  -> streaming works; the card simply needs asking per frame.
    still 1         -> the kick opcode is wrong for this purpose. Try
                       kick_opcode=0x2f and 0x09 (bare epint notify, no
                       audio_ctrl side effect), then poll_drain_credit=1.
    0 frames / lock lost -> the M119 hazard came back despite the gate; stop and
                       re-read the gating rather than raising the interval.

Check the run echoes a nonzero kick count in `poll-drain stopped after N
deliveries, K kicks` before reading anything else, and check the `MST3367 CSC`
line that is now visible.

## M133 (hardware, 2026-08-21): kicks do not gate the cadence, and they cost the lock

    sudo POLLDRAIN=20 EXTRA="op6_kick_ms=33 kick_repeat=1" scripts/mz0380-m55-real-capture.sh 5

    29677.971  poll-drain: buf 0 holds 3110400 bytes ... delivering
    29677.972  poll-drain: first kick op 0x06 ret=0
    29733.653  poll-drain stopped after 1 deliveries, 1224 kicks (op 0x06)

**1224 kicks. One frame.** Every kick returned ret=0, so the card accepted all
of them and produced nothing.

And it cost the receiver its lock:

    29680.231  detect 55=83 no-lock (auto-position on; timing not sampled)
    29680.499  detect 55=03 no-lock
    ...        output stage [at stop]: R55=ff

Lock was lost ~2.3 s after the flood began, and never came back - `R55=ff` at
stop against `7f` on every other run this session. This is the M119/M125 hazard
("op 0x2f accepted 1171 times -> one frame, and the receiver lost lock under it.
Do not flood these.") reconfirmed with `0x06` at 33 ms. **The closed-table entry
was right and now covers 0x06 too.**

So the wake-up is not the gate. Do not spend more spawns on kick tuning.

### What the stop line says instead

    stream stop: EVENT[0x30]=00000000 token[0x40]=00000000 ... enc[0x50]=00000000
                 irq_total=6 frame_events=0 fifo_drops=0

- `frame_events=0`, and `irq_total=6` is just the mailbox traffic. The card
  never raises a frame-completion event at all - which is why poll-drain exists.
- `token[0x40]=0`. The buffer index (token & 7) never advances off 0, matching
  buffers 1-3 reading untouched in every run.
- `enc[0x50]=0` - already "host has consumed it, encode another" (M40), so the
  enc_stat handshake is not what is being waited on.

The DMA of the one frame is fast and complete (2573440 of 3110400 bytes at the
first poll, whole 21 ms later). The card writes one frame into buffer 0, never
signals, never advances, and no amount of asking changes it.

**Next single variable: `poll_drain_credit=1`, kicks OFF.** That is M118's
`mz0380_credit_rearm()` - the card's one-shot completion credit, which nothing
has ever restored on this path because the ISR that normally does it runs only
for an event that never fires. It is the only remaining piece of the
handshake that has not been tried against real frames.

```bash
sudo POLLDRAIN=20 EXTRA="poll_drain_credit=1" scripts/mz0380-m55-real-capture.sh 5
```

### A bug of mine: the CSC AUTO path read the wrong register value

    29676.556  MST3367 CSC 0x92 = 0x40 (auto, input colorspace RGB from 0x48=00)
    29676.712  link [before START]: ... 48=d2, input colorspace YUV444

`mz0380_mst3367_apply_csc_mode()` read BANK2 0x48 as **0x00** and therefore
picked 0x40, the RGB matrix - the exact wrong branch for this source. The output
diag read the same register as **0xd2** 156 ms later, with the receiver locked
throughout. So M132's AUTO run was silently applying the broken CSC, and the
truncated capture meant the chroma oracle could not catch it.

Why the standalone read fails is not established. It is not worth establishing:
the diag's read path is proven on every run in this file, so **the fix is to
reuse it rather than add a second one**. `output_diag()` now caches 0x48 into
`dev->mst_b2_48`, `apply_csc_mode()` prefers the cached value (and says
`cached` or `read here` in its log line), and the stream-start path calls the
diag FIRST so the cache is fresh. This is also, belatedly, why hdcapm keeps
`regb2r48_cached` instead of re-reading.

**`mst_csc_ctl=AUTO` remains unverified on hardware.** M130a proved
`mst_csc_ctl=0` works; AUTO has never yet chosen correctly.

### Harness note: score chroma on a 1-frame run

Asking for 5 frames guarantees `timeout -s INT` kills v4l2-ctl mid-write and the
single good frame loses its 1536-byte stdio tail, so the file is unscoreable
(the oracle correctly refuses it, exit 2). Use `... .sh 5` to test cadence and
read the driver log; use `... .sh 1` when the frame itself needs scoring.

## M134 (hardware, 2026-08-21): credit is not the gate either; CSC AUTO verified

    sudo POLLDRAIN=20 EXTRA="poll_drain_credit=1" scripts/mz0380-m55-real-capture.sh 5

    41251.274  MST3367 CSC 0x92 = 0x00 (auto, input colorspace YUV444 from 0x48=d2, cached)
    41252.546  poll-drain: buf 0 holds 3110400 bytes ... delivering
    41308.233  poll-drain stopped after 1 deliveries, 0 kicks

**One frame again.** `mz0380_credit_rearm()` (M118) does not move the cadence.
Lock held clean throughout - `55=7f LOCKED` at every sample and at stop - which
also confirms M133's lock loss was the kick flood and nothing else.

### The M133 fix is verified

`MST3367 CSC 0x92 = 0x00 (auto, input colorspace YUV444 from 0x48=d2, cached)`.
AUTO now reads the right value, from the cached diag read, and picks the right
branch. `mst_csc_ctl=AUTO` is **confirmed working** - the last of M130's fix to
be proven on hardware. (The frame itself is still unscoreable: 5-frame runs
always truncate. Colour is established by M130a plus this log line; one
`.sh 1` run would close it formally.)

### Where the handshake hunt stands

Everything the completion path could have wanted has now been tried against
real frames, one at a time:

| mechanism | milestone | result |
|---|---|---|
| enc_stat ack | M117 (in poll-drain unconditionally) | 1 frame |
| op6 / 0x2f kick, repeating | M133, 1224 kicks | 1 frame, **and lock lost** |
| completion credit re-arm | M134 | 1 frame |

And the stop line is the same every time: `frame_events=0`, `token[0x40]=0`,
`enc[0x50]=0`. The card writes one frame into buffer 0, never raises an event,
never advances the buffer index, and nothing the host returns changes it.

### Re-reading the problem: this is the PREVIEW path, not the encoder

The delivered payload is **raw planar 4:2:0**, not an H.264 bitstream - the
"inferred H.264 length" in the log is a misnomer left from when we expected
NALs. The splash arrived the same way. So the DMA target is a raw frame buffer
fed by the card's preview path, and its cadence is governed by
`preview_params_settings` - the struct op 0x31 writes.

Which puts the spotlight on two fields we are deliberately NOT setting:

    post[1] = (fps & 0xff) << 8;    /* [8]=ch=0  [9]=fps=60  [10]=skip=0  [11]=avg=0 */

`skip` and `avg` are gated by mask bits 0 and 1, and since M131 the mask is 0,
so **neither is ever stored**: `pps[6]` and `pps[7]` keep their startup value of
zero. M128c already noticed the card's own handler wants `skip` to be at least 2
- with `avg == 0` it runs `if (skip <= 1) skip = 2` on the payload at 0xf5d0 -
but with mask bit 0 clear that 2 never reaches the struct.

A preview pacer with skip=0 is a plausible reason for exactly one frame.

### Next: the M128d bisect, now on the critical path

Which bit of the old 0x1f mask truncates the DMA was deprioritised as
"interesting but not needed". It is needed now, because it decides whether
`skip` can be set at all.

```bash
sudo POLLDRAIN=20 EXTRA="post_mask=0x01" scripts/mz0380-m55-real-capture.sh 5
```

Bit 0 alone = store `skip`, nothing else.

    full frames  -> bit 0 is safe, so die_en (bit 4) is the truncator - which was
                    the standing prior, and it also unblocks the Windows
                    ordering (M90/M91). AND skip is now applied: watch the
                    delivery count. More than one frame closes the cadence.
    16 bytes     -> `skip` itself is the truncator. Then it can never be set,
                    and the cadence gate is somewhere else entirely.

Two answers from one spawn, whichever way it lands. Follow with
`post_mask=0x10` (die_en alone) to complete the bisect.

## M135 (hardware, 2026-08-21): mask bit 0 is the truncator - and the mechanism is a SKIPPED function call

    sudo POLLDRAIN=20 EXTRA="post_mask=0x01" scripts/mz0380-m55-real-capture.sh 5

Run twice, identical both times:

    stream start: SET_PREVIEW_PARAMS(op 0x31, mask=0x01, fps=60, die_en=1, fake_frame_off=1)
    poll-drain: buf 0 holds 16 of 3110400 bytes - DMA still in flight, waiting  (x16)
    stop buf[0] head=2e 2e 2f 2f 2e 2e 2e 2f 2f 30 31 30 2f 2f 2f 2f   (run 1)
    stop buf[0] head=30 30 31 31 30 30 31 30 30 30 30 30 30 30 30 30   (run 2)

**Bit 0 alone truncates.** My standing prior was die_en (bit 4); it was wrong.
Note the 16 bytes are real video at the current scene's luma (~0x30, a dark
evening scene) - the same shape as M128a's 0x53, which is now explained: those
were real frames, truncated.

### It is not "setting skip breaks it"

The branch at tinyvenc5 0xf338 decides whether `tiny_calculate_skip_fps()` gets
called:

```
f34c: tst  r3, #1          @ sticky mask bit 0
f350: beq  0xf394          @ clear -> fall through to the call
f354: ldrb r1, [r2, #6]    @ pps[6] = skip
f358: cmp  r1, #0
f35c: beq  0xf394
f360: ldrb r2, [r2, #5]    @ pps[5] = fps
f368: beq  0xf394
f378:      clear mask bit 0
f390: b    0xf3f4          @ <-- SKIPS tiny_calculate_skip_fps
f394: tst  r3, #2
f3b4: ...
f3f0: bl   tiny_calculate_skip_fps
```

With mask 0: bit 0 clear -> f394 -> bit 1 clear -> f3b4 -> **calls** it -> whole
frame. With mask 0x01 and the card's own clamp having forced skip to 2 (M128c)
and fps 60: -> f36c -> clears the bit -> **skips the call** -> 16 bytes.

So the correlation is *calling `tiny_calculate_skip_fps`* <=> a complete
transfer, not the value of `skip`. Falsifiable prediction:
**`post_mask=0x10` (die_en alone) should give a FULL frame**, because bits 0 and
1 stay clear and the call happens. That completes the M128d bisect and tests the
model in one spawn. It also settles whether die_en is safe, which is what the
Windows ordering (M90/M91) needs.

### Two cheap dead ends closed statically

- **ops `0x60`/`0x61`.** They are in ep.ko's no_signal list but absent from
  tinyvenc5's dispatch table, so ep.ko must handle them itself - a plausible
  "hand the buffer back" candidate. It is not: `pciep_isr` 0x1888 does the
  latch test and falls to 0x17bc, the same bare `sysfs_notify(epint)` every
  other opcode gets. No buffer semantics at all.
- **The buffer tables.** cmd 2 (ours) and cmd 5 both index a 192-byte per-group
  struct by payload word 0 (`MLA r5, #192, r2, r5`) and each write **8** address
  words - cmd 2 at group offset 0x08..0x24, cmd 5 at 0x68..0x84. Two separate
  8-entry tables. Nothing in ep.ko ever reads either; the card's userspace
  consumes them. This matches the driver's own warning about a "SET_BUF_8 second
  set not allocated" and means we back 4 of 8 slots in one of two tables - but
  the card only ever writes slot 0, so under-provisioning is not the gate.

### Cadence: where it actually stands

Everything host-side has been tried, one variable at a time:

| tried | milestone | result |
|---|---|---|
| enc_stat ack | M117 | 1 frame |
| op6 kick x1224 | M133 | 1 frame, lock lost |
| credit re-arm | M134 | 1 frame |
| asking v4l2 for more | M132 | 1 frame |
| ops 0x60/0x61 as buffer return | M135, static | not buffer ops at all |

The card writes one frame into buffer 0, raises no completion, never advances
the token, and nothing the host sends changes it. **The remaining answer is
card-side**: what in `libvideocap` / `video_capture_mgr` advances the preview
buffer index and re-arms the DMA. That is a static dig, it is free, and it is
the honest next step - not another knob.

### Next, in order

1. **`post_mask=0x10`, one spawn.** Completes the bisect, tests the
   `tiny_calculate_skip_fps` model, and unblocks the Windows ordering question.
   Predicted: full frame.
2. **Static: the preview DMA loop.** `libvideocap.so.13` /
   `video_capture_mgr` - find what writes the host buffer addresses into the
   DMA engine per frame and what advances the index. `libvideocap` is PIC and
   addresses strings as `GOT_base(0x18e4c) + literal`.
3. `out_fmt` (SET_VIC byte 12, still 0) once the cadence is understood.

### Spawn budget warning

This session has spent **12** encoder spawns on top of whatever the power cycle
already had. The card has historically wedged in the **8-18** range and has not
wedged yet, so the next few runs are in the risky zone. A soft PC shutdown
resets it (M127).

### M135a (hardware): `post_mask=0x10` gives a FULL frame - the bisect is complete and the model holds

    stream start: SET_PREVIEW_PARAMS(op 0x31, mask=0x10, fps=60, die_en=1, fake_frame_off=1)
    poll-drain: buf 0 holds 1121024 of 3110400 bytes - DMA still in flight
    poll-drain: buf 0 holds 3110400 bytes ... delivering

Predicted and observed. So:

| mask | stores | tiny_calculate_skip_fps | result |
|---|---|---|---|
| 0x00 | nothing | called | full frame |
| 0x01 | skip | **skipped** | 16 bytes |
| 0x10 | die_en | called | **full frame** |
| 0x1f | skip+avg+die_en | **skipped** | 16 bytes |

**die_en is safe. Bit 0 is the truncator, and the mechanism is the skipped
call.** M128d's prior (die_en) was wrong, and the de-interlacer being on for a
progressive source turns out to be harmless.

**This unblocks the Windows ordering.** `win_seq=1` "renders nothing"
(M90/M91) because it sends `0x31` with mask 0x1f, whose bit 0 skips the call.
`win_seq=1 post_mask=0` (or `0x10`) is now worth a spawn on its own account.

## M136: SET_VIC byte 34 is the FRAME-COMPLETION INTERRUPT ENABLE (static)

Free. Found by reading ep.ko rather than guessing, and it is the best cadence
candidate this session has produced.

`pciep_isr`'s cmd-41 (SET_VIC) arm ends:

```
1744: ldrb r1, [r4, #0x22]     @ SET_VIC payload byte 34
1748: adds r3, r1, #0
174c: movne r3, #1
1754: strb r3, [r5, #0x630]    @ state[0x630] = (byte34 != 0)
```

`state[0x630]` is the gate inside `store_channel_done()` - the sysfs attribute
**the card's own userspace writes when a channel finishes a frame**:

```
dc8: <store_channel_done>
 ...  packs (count - 1) into a nibble at bit ch*4 in BAR0 0x40/0x44/0x48/0x4c
 e88: ldrb r3, [r1, #0x630]
 e8c: cmp  r3, #0
 e94: beq  0xefc               @ ZERO: do not raise - accumulate into [0x634]
 ebc: str  r4, [r3, #0x30]     @ BAR0 0x30: EVENT |= (1 << ch)
 ed0: str  r0, [r3]            @ and poke the interrupt
```

Those four BAR0 registers are exactly the ones our stop line prints, and
`0x40` is `MZ0380_MB_FRAME_TOKEN`.

**With byte 34 at zero the card counts its completed frames and never tells the
host.** Which is precisely what every run reports:

    stream stop: EVENT[0x30]=00000000 token[0x40]=00000000 0x44=... 0x48=... 0x4c=...
                 frame_events=0

Note `store_channel_done` writes `count - 1`, so a single completed frame packs
as nibble 0 - indistinguishable from never-written. `token[0x40]=0` has always
been read as "the index never advanced"; it is equally "one frame, index 0, and
no interrupt".

We have sent 0 in byte 34 for the driver's entire life. M82 read it as "mix"
from the Windows traces, where it is also zero - but "Windows sends 0" has
already cost us `fw=7` and `post_mask=0x1f` (method rule 4). The AIC twin of
this mechanism is already named in our own header: SET_AIC byte 17 =
`aic_int_mode` -> `state[0x63c]`, the other branch of the same gate at 0xefc.
Nothing has ever set the video one.

New knob `vic_int_mode` (SET_VIC byte 34, default 0 = no change):

```bash
sudo POLLDRAIN=20 EXTRA="vic_int_mode=1" scripts/mz0380-m55-real-capture.sh 5
```

    >1 frame                 -> the cadence is solved.
    1 frame but EVENT/frame_events nonzero -> the interrupt now arrives; the
                                remaining gate is what the host does with it
                                (the ISR path, which has never run for real).
    no change                -> byte 34 is not the enable, or the card's
                                userspace never calls channel_done at all.

Read `EVENT[0x30]`, `frame_events` and `token[0x40]` in the stop line before
anything else - those three are the measurement, not the frame count.

## M137 (hardware + static, 2026-08-21): `vic_int_mode=1` changes nothing - and why

    sudo POLLDRAIN=20 EXTRA="vic_int_mode=1" scripts/mz0380-m55-real-capture.sh 5

    stream stop: EVENT[0x30]=00000000 token[0x40]=00000000 ... frame_events=0
    poll-drain stopped after 1 deliveries, 0 kicks

No change. **Caveat, and it is mine:** the SET_VIC log line did not print byte
34, so this run cannot formally distinguish "the flag landed and did nothing"
from "the flag never landed" - method rule 9 again, in a line that has been
short of `fk` and `int_mode` all along. `insmod` would have rejected an unknown
parameter and did not, and `params[7]` demonstrably ORs the byte in, so the
value almost certainly landed; but "almost certainly" is not the standard this
file holds itself to. **Fixed:** the line now prints `fk=` and `int_mode=`.
Re-confirm on the next run that carries the knob.

### Why it may be moot: who actually writes `channel_done`

`/sys/class/vpl_pciep/channel_done` (the attribute `store_channel_done()`
backs - ep.ko creates it with `__class_create` + `class_create_file`, which is
why its path prefix differs from the `/sys/vpl_pciep/` attributes) is
referenced from exactly **two** places in tinyvenc5:

    12c60   EncodingGroup::encode_handler
    15d58   EncodingGroup::fake_frame_process

The second is the standby splash thread, which `fake_frame_off=1` stops from
ever being created (M128/M129). So since M129 the **only** writer is
`encode_handler`, and it evidently writes once and then blocks.

This is not a regression from `fake_frame_off`: every pre-M129 run also
delivered exactly one frame per stream, with the splash thread running. But it
does relocate the question. The cadence gate is **inside `encode_handler`'s
loop** - what it waits on after finishing frame 1 - and `state[0x630]` only
decides whether a completion that never happens gets reported.

That also explains why every host-side handshake failed: enc_stat, credit and
kicks all address the *reporting* path, and the producer is what is stalled.

### Next, and it is static

Disassemble `EncodingGroup::encode_handler` (tinyvenc5 0x12c60 region) and find
what it blocks on after its first `channel_done` write. Candidates visible in
the PLT: `TK_H264Enc_WaitOneFrame`, `TK_MMA_WaitOneFrameComplete`,
`SSM_ReleaseAndReceive`, `PB_GetFullness`, and the `pread` of
`/sys/vpl_pciep/enc_stat%d`. One of those returns and never comes back.

This is free, it is the only remaining thread, and it is where the next session
should start. Do NOT spend more spawns on host-side knobs until it is read -
five have now been spent proving, one at a time, that the reporting path is not
the problem.

## M138 (static, 2026-08-21): the producer chain read end to end - `encode_handler` blocks on an empty ring, and the stall is three layers upstream

M137 ended with a list of five PLT candidates and the instruction to read
`EncodingGroup::encode_handler`. It is read. **None of the five is the answer**,
because none of them is reached: the loop parks at its very first statement.

Everything below is static, from the in-tree blob unpack. Zero spawns.

### `encode_handler`'s loop, exactly

`_ZN13EncodingGroup14encode_handlerEPv` is at **0x12b00**, not 0x12c60 (0x12c60
is the `open()` of `channel_done` inside it), and runs to 0x15c5c. Prologue:

    12c60  ldr  r0, =0x2ae10          "/sys/class/vpl_pciep/channel_done"
    12c64  bl   open@plt              -> fd kept at [sp,#0x78]
    12c94  bl   SSM_Reader@plt        -> reader handle to [r11,#0x6c]

Loop head is **0x12f04**. The body begins:

    12f04  ldrb r8, [r11,#0x38]       run flag
    12f0c  beq  0x12fc0               0 -> clean exit
    12f10  ldr  r0, [r11,#0x6c]       reader handle
    12f18  beq  0x150f8               NULL -> puts + exit
    12f24  bl   SSM_ReleaseAndReceive@plt      <-- FIRST call of every iteration
    12f30  bls  0x1512c               ret < 0 -> set flag, exit

`SSM_ReleaseAndReceive` releases the frame just encoded and fetches the next.
It is the first thing the loop does, so if it does not return, nothing else in
the body runs. `TK_H264Enc_WaitOneFrame` (0x14760, 0x147e0),
`TK_MMA_WaitOneFrameComplete` (0x14358, 0x150dc) and the `pread` of
`enc_stat%d` (0x139e0) all sit **downstream** of it. There is no `PB_GetFullness`
call in the function at all.

The `channel_done` write is at **0x13dfc** - `pwrite(fd=[sp,#0x78],
buf=sp+0x120, 24, 0)` - well after the receive. So the order per frame is
*receive -> encode -> report -> receive again*, and "one frame reported" means
the second receive never returned.

### It blocks, it does not exit

`libsyncsharedmemory.so.0` imports `pthread_cond_wait` and **no timed variant**.
`SSM_ReleaseAndReceive` (0x1d0c):

    1e74  cmp  r3, r2                 r3 = reader rd_idx [r4,#0x10]
                                      r2 = shared wr_idx [r5,#0x9c]
    1e80  beq  0x1e58 -> pthread_cond_wait(cond = shm+0xc0, mutex = shm+0xa4)

Reader index equal to writer index means the ring is empty, and the thread
sleeps on a process-shared condvar with **no timeout**. It is parked, not dead.
That distinction is testable and matters: on the error return (`ret < 0`) the
loop instead falls to **0x12fc0**, which `close()`s the `channel_done` fd,
`TK_MMA_Release`s both handles, `SSM_Release` + `SSM_RecycleHandle`s the reader
and returns 0. A dead encoder therefore *closes* its sysfs fd; a starved one
holds it open.

### The full producer chain

    vpl_vic.ko ISR (0xfd8)
      sets chan->frame_ready = 1 at [r8,#0x4c], then __wake_up (0x17a8)
        |
    ioctl(/dev/vpl_vic, 0xe301)              vpl_vic Ioctl handler at 0x30a8
      = wait_event_interruptible_timeout(frame_ready == 1, T)
      T = global[0x40] * 100 jiffies; on success clears the flag
        |
    libvideocap.so.13  VideoCap_WaitVIC (0x4644) = 2 insns: ioctl(fd, 0xe301)
      VideoCap_Sleep (0x43d4) is a single `b` to it
        |
    libtk_video_capture.so.0  process() (0xd0c)   <-- the frame loop
        |
    callback = libtkmf_video_source.so.0  img_handler (0x1188)
      registered through TK_VideoCap_Init from TKMF_VideoSrc_Init (0x18bc/0x18e4)
        |
    SSM_DeliverAndAllocate -> wr_idx++ -> broadcast
        |
    tinyvenc5 encode_handler wakes

`process()` is the loop, and it **never gives up**:

    d40  loop:  if (!ctx->run) return 0
    d4c         if (ctx->force_exit) { usleep(100); return 0 }
    d58         r = VideoCap_Sleep(ctx)                 // the blocking wait
    d64         if (r == 0) goto da0                    // frame ready
                ctx->timeouts++
                usleep(1000); goto d4c                  // retry, forever
    da0         r = VideoCap_GetBuf(ctx, &buf)
                if (r) { usleep(1000); retry }
    e00         callback(&buf, cbarg)                   // img_handler
    e0c         VideoCap_ReleaseBuf(ctx, &buf)          // ioctl 0x4004e304
    e20         ...option update...  goto d40

A VIC that stops producing therefore yields a permanent one-frame state with a
1 ms poll spinning behind it and **no error anywhere the host can see**. That is
exactly the symptom this project has had since the beginning.

### `img_handler`'s drop gates, both of them, decoded

This is the function the "(Drop this frame) ... bNoSignal" string lives in. Its
two early-outs are:

**1. A pure geometry equality test.**

    11b4  ldr  r3, [r5,#0x1c]         VIC_Get width, from the capture descriptor
    11bc  ldrh r1, [0xaa54]           Tiny_Set width, from .bss
    11c4  bne  0x12e0                 -> printf, then return WITHOUT delivering

The printf is
`[yuan][tkmf] (Drop this frame) Tiny_Set(%d x %d)!= VIC_Get( %d x %d)(stride %d),
Count = %d, idx = %d, Time = %d:%03d, dwInWidth: %d, bCCIRErr: %d, bNoSignal: %d,
bFifoFull: %d`, and its arguments map to
`bss[0]`, `bss[2]`, `frame[0x1c]`, `frame[0x20]`, `frame[0x24]`, `frame[0x4]`,
`frame[0x0]`, `frame[0x8]`, `frame[0xc]`, `frame[0x18]`, `frame[0x30]`,
`frame[0x2c]`, `frame[0x28]`.

**`bNoSignal`, `bCCIRErr` and `bFifoFull` are printf arguments and nothing
else.** They are never tested, anywhere in the library. M127 named this path as
the target and M129 called that wrong; this is *why* it was wrong, and it is now
closed for a mechanical reason rather than an inference.

**2. A modulo frame skip, and only when pixel format == 2.**

    1340  ldrh r1, [0xaa54+6]         m_skip
    1350  beq  -> normal              m_skip == 0: no skipping at all
    1358  r0 = frame[0x4]             Count
    135c  bl   __aeabi_uidivmod       r1 = Count % (m_skip + 1)
    1364  bne  -> normal
    1368  return                      Count % (m_skip+1) == 1 -> drop

Note the direction: it drops **one frame in (m_skip+1)**, it does not deliver
one in (m_skip+1). `m_skip = 0` disables it entirely, `m_skip = 1` costs half
the frames. **This kills M134's "a preview pacer with skip=0 is a plausible
reason for exactly one frame"** - skip=0 is the *no-drop* setting. Chasing
`skip` through the mask (M134/M135) was chasing a divider that was already off.

### `Tiny_Set` is host-driven, once, and three of its five fields are dead

`TKMF_VideoSrc_Setup` (0x168c) is 5 halfword stores into the .bss struct at
**0xaa54**: `{ m_vic_width, m_vic_height, m_fps, m_skip, m_avg }`, plus the
`[TKMF] VideoSrc_Setup m_vic_width(%d), m_vic_height(%d), m_fps(%d), m_skip(%d),
m_avg(%d)` banner.

`img_handler` reads **only offset 0 (width) and offset 6 (skip)**. Offset 2
(height) appears in the drop printf only; **`m_fps` and `m_avg` are written and
never read**. Anything that tried to pace the source through fps/avg was a
no-op.

tinyvenc5 calls it **exactly once**, from `EncodingGroup::Start` at **0x11014**,
filling the struct from the per-channel config record (60 bytes per channel):

    [sp+8]  = ldrh cfg[ch]+0x0c    m_vic_width
    [sp+10] = ldrh cfg[ch]+0x0e    m_vic_height
    [sp+12] = ldrb cfg[ch]+0x09    m_fps
    [sp+14] = ldrb cfg[ch]+0x38    m_skip
    [sp+16] = 0                    m_avg

One call, before the loop, so the geometry gate cannot change mid-stream. If it
matched for frame 1 it matches for frame 2.

### `img_handler`'s first call is an init, not a delivery

    11c8  ldr r2, [r4,#0x0c]   ; if 0 -> 0x129c, the one-time init
    14f4  bl  SSM_Writer(..., 4 buffers)
    1544  bl  SSM_DeliverAndAllocate(writer, zeroed descriptor)   ; prime only
    1580  bl  TK_ImgProc_Init ...
    1600  [r4,#0x0c] = 1 ; return

`SSM_DeliverAndAllocate` (0x19a8) short-circuits at 0x19c4 when the descriptor's
buffer pointer is NULL and does **not** advance `wr_idx`. So the prime is not a
frame. Getting one frame at the host means `img_handler` ran at least twice and
then stopped.

### The host-facing link cleared: `store_channel_done` cannot block

`ep.ko` has **no** relocation to `wait_event`, `wake_up`, `msleep`, `schedule`
or `udelay` anywhere - its only scheduling relocs are two `preempt_schedule`
from `spin_unlock`. `store_channel_done` (0xdc8) runs with interrupts masked
(`mrs`/`orr #0x80`/`msr`) and is a straight-line register write. The `pwrite`
at 0x13dfc therefore always returns immediately, and "encode_handler is blocked
in the sysfs write waiting for a host ack" is **dead**.

### The EVENT credit, and why the existing ack sequence is correct

While in `ep.ko`, the completion path was read out completely. There is one
32-bit word in `.data` at offset 0, **initialised to 1**, that gates every
event the card raises:

    pciep_isr_clrint (0xd44)      card IRQ 42
        bar0[0x30] = 0
        credit     = 1                 <-- the ONLY writer of 1

    msi.constprop.1 (0x1160)      command completion
        pending |= (1 << 11)
        if (credit) { bar0[0x30] = pending; pending = 0; credit = 0; *doorbell = 1 }

    store_channel_done (0xdc8)    frame completion
        pending |= (1 << ch)
        if (credit) { bar0[0x30] = pending; pending = 0; credit = 0; *doorbell = 1 }
        else        { }                       // accumulate silently

`pending` is `state[0x634]`, the host-notify register pointer is
`state[0x638]`, set at module init to `<cfg base> + 0xdc` (which init also
primes to 2) - i.e. exactly the `BAR5[0xdc]` of our ack sequence.

`mozart_module_init` registers **`request_irq(42, pciep_isr_clrint, ...)`** and
**`request_irq(43, pciep_isr, ...)`**. The doorbell values line up with the IRQ
numbers on a base of 32: `MZ0380_MB_INT_ACK` = 0x400 = bit 10 -> IRQ 42 ->
`pciep_isr_clrint`; `MZ0380_MB_FIRE` = 0x800 = bit 11 -> IRQ 43 -> `pciep_isr`,
the command dispatcher. **`BAR0[0x00] = 0x400` is what re-arms the credit**, and
the driver's `mz0380_credit_rearm()` fires precisely that.

Independent confirmation: command completions consume the same one-shot. M133
ran 1224 op-`0x06` and 1171 op-`0x2f` commands and every one completed. That is
impossible unless the credit is being restored 1000+ times per run. **M118's
credit hypothesis was right about the mechanism and M134's negative was right
about the outcome** - the credit works, and it was never the gate.

### Correction to M136: `state[0x630]` does not gate EVENT delivery

M136 read SET_VIC byte 34 -> `state[0x630]` as deciding "whether
`store_channel_done()` raises `EVENT |= (1 << ch)` or merely accumulates". It
does not. Both branches of the 0x630 test are credit-gated identically:

    e88  ldrb r3, [state+0x630]
    e94  beq  0xefc                 int_mode == 0
    e98  ldr  r0, [credit] ; beq 0xed8 -> accumulate     (int_mode != 0 branch)
    ...
    efc  ldrb r7, [state+0x63c]     aic_int_mode
    f08  ldr  r8, [credit] ; beq 0xf34 -> accumulate     (int_mode == 0 branch)
    f54  ldr  r0, [credit] ; beq 0xfb4 -> accumulate

The only real difference between the two branches is the shift used for the
**audio** channel bit (`r0+15` vs `r4+16`). For video the code is the same
either way. So M137's "`vic_int_mode=1` changes nothing" is the correct and
expected result, and the caveat about byte 34 not appearing in the log is moot:
the knob could not have mattered. `state[0x63c]`/SET_AIC byte 17 is in the same
position.

### What this leaves

The stall is **on the card, upstream of tinyvenc5**, in one of:

1. the VIC hardware not raising further frame interrupts,
2. `vpl_vic.ko`'s ISR (0xfd8) not reaching its `frame_ready = 1` + `__wake_up`
   at 0x1788/0x17a8 on later frames,
3. `process()` spinning on `VideoCap_Sleep` timeouts, which is the observable
   consequence of either.

None of that is reachable by patching (the no-upload rule) or by console (the
card's console is unreachable). What *is* reachable is everything the host feeds
the VIC: SET_VIC geometry, the receiver's BT1120 output, and the GPIO/reset
lines. The geometry gate above makes the first of those newly interesting - not
as a drop cause (it would drop frame 1 too) but because `cfg[ch]+0x0c/0x0e` are
the only host-settable values the source layer actually reads.

## M139 (hardware, 2026-08-21): the encoder never reported a single frame - `store_channel_done` has never run

    sudo POLLDRAIN=20 scripts/mz0380-m55-real-capture.sh 5

    stream start: frame-token sentinel a5a5a5a5 seeded into BAR0 40/44/48/4c;
                  readback a5a5a5a5/a5a5a5a5/a5a5a5a5/a5a5a5a5
    producer watch: baseline 40=a5a5a5a5 44=a5a5a5a5 48=a5a5a5a5
    poll-drain: buf 0 holds 3110400 bytes with no completion event; delivering
    poll-drain stopped after 1 deliveries, 0 kicks; producer watch saw 0 change(s),
                  final 40=a5a5a5a5 44=a5a5a5a5 48=a5a5a5a5
    stream stop: EVENT[0x30]=00000000 token[0x40]=a5a5a5a5 0x44=a5a5a5a5
                 0x48=a5a5a5a5 0x4c=a5a5a5a5 enc[0x50]=00000000
                 irq_total=6 frame_events=0 fifo_drops=0

**The sentinel is intact in all four registers after 56 seconds of streaming.**
The readback line proves BAR0 0x40 is host-writable, so the test is valid, and
`store_channel_done()` read-modify-writes one nibble per channel into exactly
these words *before* its credit test - so a single call would have left
`a5a5a5a0`.

### `store_channel_done()` has never run. Not once. Video or audio.

That is stronger than M138's reading and it corrects it. M138 concluded
"`encode_handler` writes `channel_done` once and then blocks". It **never
writes it at all**:

- `token[0x40]` = sentinel: no video channel ever reported.
- `token[0x4c]` = sentinel: no audio channel ever reported either.
- `enc[0x50] = 0`: per M40 the card writes 1 there after DMAing a bitstream.
  It never did. **The H.264 encoder produced nothing.**
- `frame_events = 0` with `irq_total = 6`: the six interrupts are command
  completions, every one of which the host ACKed. The completion credit was
  therefore restored repeatedly and was available; nothing was waiting to use
  it. The accumulator argument closes the last hole - a frame bit parked in
  `state[0x634]` would have ridden out on the next raised EVENT, and none ever
  carried one.

### And yet one full, real frame reached buffer 0

3110400 bytes of correctly-coloured planar I420, landing ~1.2 s after
START_STREAMING, with **no** `channel_done`, **no** `enc_stat`, and **no**
EVENT. Buffers 1, 2 and 3 were never touched (`0/1024 sampled pages`).

So the frame this project has been calling "the capture" since M129 does not
come from the SDK's frame path at all. `channel_done` is that path's only
notification and it never fired. The pixels arrive over `vpl_dmac`'s PCIe
outbound transfer - `pcie_set_outbound` is exported by `ep.ko` and
**`vpl_dmac.ko` is its only importer**, called from `VPL_DMAC_StartTail`
(0xc94) and re-armed from `VPL_DMAC_ISRTail` (0xf70). One transfer completed
and the chain was never re-armed.

### `encode_handler` IS running - it is parked with zero frames received

The thread is not missing. `EncodingGroup::init_func` (0x109cc) creates it
**unconditionally**:

    10a98  mov  r3, #1
    10a9c  strb r3, [r5,#0x38]        <- the run flag encode_handler tests at 0x12f04
    10aa8  ldr  r2, =0x12b00           encode_handler
    10ab0  bl   pthread_create@plt

(`fake_frame_process` is the *other* create, at 0x10c08, and it is the gated
one - that is the splash `fake_frame_off=1` suppresses.)

So the thread exists, its run flag is set, and M138 established that the first
call of every loop iteration is `SSM_ReleaseAndReceive`. Combining that with
the sentinel: **`encode_handler` never got past its first `SSM_ReleaseAndReceive`,
because the SSM ring was never fed.** `img_handler` published **zero** frames -
not one, not one-then-stop.

`img_handler` publishes zero frames in exactly two cases (M138 enumerated its
only two early-outs):

1. it is never called - the VIC delivers nothing to `libtk_video_capture`'s
   `process()` loop, which then spins on `VideoCap_Sleep` timeouts forever; or
2. its **width gate** rejects every frame:
   `VIC_Get.width != Tiny_Set.width` -> drop, silently, with the banner going
   to a console we cannot read.

### Why (2) is now the leading candidate, and why M76/M127g did not test it

`Tiny_Set.width` is `cfg[ch]+0x0c`, read once by `EncodingGroup::Start`
(0x11014) and fed by the card's cfg patcher from **SET_VIC bytes 8..9** - the
`width` field, which the driver has always filled from the v4l2 capture width,
1920. It has never been settable on its own.

M76 and M127g both swept **3840**, but through `vic_in_w` - SET_VIC bytes
24..27 - which is the VIC's own width *register* and **not** the value the gate
compares. The gate's input has never been moved.

Three independent things point at 3840:

- **BANK0 `0xb0 = 0x21`** - M100 decoded bit0 = embedded sync, bit2 = 10/8-bit.
  0x21 is embedded-sync **8-bit** BT1120, so a 1920-pixel line crosses the bus
  as 3840 8-bit samples.
- **The receiver's own detect flaps by exactly a factor of two**: this run
  logged `hper=674 vper=599 lines=1125` and `hper=337 vper=299 lines=1127`
  alternately, both at `55=7f LOCKED coherent`. Two clock domains, 2:1.
- M100 already established that **embedded sync is required** for VIC init
  (`0x20`/`0x14`, external sync, render nothing at all), so the 8-bit
  double-rate framing is not optional - it is the only mode that works.

### The test, and how to run it without an overrun

`vic_out_w` / `vic_out_h` now override SET_VIC bytes 8..9 / 10..11
independently of the v4l2 format (default 0 = use the capture geometry).

**Width alone would overrun.** The cfg patcher rewrites every 1920-valued line,
so the card would capture 3840x1080 = 6220800 bytes into a 4 MiB buffer - the
exact overrun that produced every IOMMU fault from M26 to M29. Halving the
height compensates: 3840 x 540 x 3/2 = **3110400 bytes**, identical to today,
so the buffer, the SET_BUF geometry and `mz0380_infer_frame_length()`'s
hardcoded `w*h*3/2` all stay correct.

```bash
sudo POLLDRAIN=20 EXTRA="vic_out_w=3840 vic_out_h=540" scripts/mz0380-m55-real-capture.sh 5
```

Only the width is compared (`img_handler` reads bss offset 0; offset 2, the
height, appears in the drop banner only), so this moves one gate input and
nothing else the gate can see.

    token[0x40] leaves a5a5a5a5   -> the gate was not the blocker; the VIC is not
                                     calling img_handler at all, and the target is
                                     vpl_vic's ISR / VideoCap_Sleep.
    token[0x40] becomes a5a5a5aN  -> the gate WAS the blocker. The encoder is
                                     running. Then walk the width back toward the
                                     true value and fix the geometry properly.

### Method note

The first producer-watch run reported "0 changes, final 40/44/48 = 0" and that
was **not** evidence of anything: `store_channel_done` writes `report[N] - 1`,
so a single report of buffer 1 writes zero over a register that was already
zero. The sentinel is what made the measurement mean something. Method rule 1
again - a null result is only informative once every other value in it is known
to be distinguishable.

## M140 (hardware, 2026-08-22): 3840 is not the gate - and no width sweep this project has run was ever single-variable

    sudo POLLDRAIN=20 EXTRA="vic_out_w=3840 vic_out_h=540" scripts/mz0380-m55-real-capture.sh 5

    stream start: SET_VIC(... -> H.264 output=3840x540, bitstreams=1) ret=0
    producer watch: baseline 40=a5a5a5a5 44=a5a5a5a5 48=a5a5a5a5
    poll-drain stopped after 0 deliveries, 0 kicks; producer watch saw 0 change(s),
                  final 40=a5a5a5a5 44=a5a5a5a5 48=a5a5a5a5
    stop buf[0..3] ... 0/1024 sampled pages touched

**Zero deliveries** - one worse than the 1920x1080 baseline - and the sentinel is
untouched. `img_handler`'s width gate is not opened by 3840, so the M139
double-rate reading is wrong: the VIC is not measuring 3840.

The receiver was locked and coherent throughout (`55=7f`, `b0=21`, htot=2200,
vtot=1125), so nothing upstream changed. Only the geometry did.

### Why it went to zero, and the method consequence

`re-dump/fw/yuan_demo_sdi/nullsensor_1920x1080.cfg` is the card's own capture
config and it is in the tree. It contains **twelve lines valued 1920 and twelve
valued 1080**:

    maximum frame width / height          <- ISP allocation
    captured frame width / height
    input frame width / height
    AE HardwareCtl Window0..8 x width / y height    (nine pairs)

M127 established that `video_capture_mgr`'s patcher (0xa290) `atoi()`s every
line and rewrites **any** line valued 1920 with SET_VIC bytes 8..9 and **any**
line valued 1080 with bytes 10..11. It is blunt: it does not know which line it
is on.

So `vic_out_w=3840 vic_out_h=540` did not move one gate input. It moved the ISP
allocation size, the capture geometry, the VIC input geometry and all nine
auto-exposure windows, simultaneously. The capture path never came up at all,
which is a perfectly ordinary outcome for a 3840x540 ISP configuration and says
nothing about the gate.

**This retroactively qualifies every width experiment in this file**, M76's
included: a SET_VIC width change is a ~24-line cfg rewrite, and there is no way
to move the gate's input without also moving the ISP's. Method rule 1 applies -
the other variables were never held at values known to permit the outcome.

### The cfg also documents the enums inline

    1  // output format (1:YUV420, 2:YUV422)
    6  // input format (1:8-bits Raw, 2:CCIR656i, 3:CCIR656p, 4: Bayer,
       //               5:16-bits Raw, 6: BT1120p, 7: BT1120i)

`in_fmt = 6` is confirmed correct for a progressive BT1120 source. (SET_VIC byte
12 is *not* this field - M79 found it never reaches the cfg and M82 settled it
as a fractional-rate flag Windows sends 0 for. Unchanged.)

### The unifying model: the VIC captures exactly ONE frame

Everything M139 measured follows from that single fact, with nothing else
needing to be true:

| observation | consequence of one VIC frame |
|---|---|
| `channel_done` never written (sentinel intact) | `img_handler`'s **first** call is the init path - `SSM_Writer`, a prime `SSM_DeliverAndAllocate` with a NULL descriptor that does not advance `wr_idx`, `TK_ImgProc_Init`, then return. It publishes nothing. With no second call the ring stays empty. |
| `encode_handler` produced nothing | It is created unconditionally (M139) and parks on its first `SSM_ReleaseAndReceive`, which has no timeout (M138). |
| `EVENT = 0`, `frame_events = 0`, `enc[0x50] = 0` | Downstream of a `channel_done` that never happens. |
| one real frame in buffer 0 | That single VIC capture, pushed over `vpl_dmac`'s outbound transfer. |
| buffers 1-3 never touched | There was never a second capture to push. |
| 3840x540 gives zero | The ISP could not come up, so not even the one capture happened. |

The defect is now stated in one line: **the card's VIC captures a single frame
and stops.** Everything this project has spent spawns on since M111 - the
completion credit, enc_stat, kicks, `vic_int_mode`, the poll-drain - is
downstream of that and cannot affect it.

---

## M141 (hardware, 2026-08-23): the Windows ordering was finally run - and it is worse than the baseline

The run M140's handoff called `START HERE`. It had never been executed with a
scoreable path: until M135a the `0x31` inside `win_seq` carried
`post_mask=0x1f`, which truncates the DMA to one 16-byte burst, and that is the
entire content of M90/M91's "win_seq renders nothing".

### Environment note - the kernel is NOT a variable, and this was checked

The session opened on a boot whose running kernel (`7.1.8-1-cachyos`) had been
upgraded out from under it: `/lib/modules/7.1.8-1-cachyos` did not exist, so
nothing out-of-tree could be built or loaded. Both runs below were taken on
`7.2.0-1-cachyos`, not the `6.18.42-1-cachyos-lts` every prior result used.

That would normally void the result under method rule 1, so the baseline was
re-measured on the same kernel, same power cycle, immediately after:

| run | sequence | frames | oracle |
|---|---|---|---|
| M141 | `win_seq=1` | **0** | - |
| M141c | baseline (defaults) | **1**, 3110400 bytes | NOT SPLASH (Y 212 / UV 203 distinct); CHROMA OK (luma explains 2% / 0%) |

**7.2.0 reproduces M129/M130 exactly.** The kernel is cleared, and the
comparison stands.

(The driver now builds on both series. The only 6.x -> 7.x drift was
`vb2_ops->wait_prepare`/`wait_finish` and the `vb2_ops_wait_*` helpers, removed
once vb2 core took over `q->lock`. The Makefile greps `videobuf2-v4l2.h` rather
than testing `LINUX_VERSION_CODE`, because the removal release lies between
6.18 and 7.2 and a wrong guess builds cleanly and then mishandles a blocking
DQBUF.)

### What the ordering did

Every command was accepted - this is the first time the sequence has been
delivered end to end:

    pre-STOP (op 0x07, all channels) ret=0
      -> 1900 ms settle
      -> SET_VIC(1920x1080p@60 fw=5 in_fmt=6 vic_in=1920x1080) ret=0
      -> SET_AIC ret=0
      -> 0x2d main ret=0, 0x2d sub ret=0
      -> 0x31 mask=0x00 fake_frame_off=1 die_en=1 ret=0
      -> no op 0x06

The receiver held `R55=0x7f` LOCKED before, during and after; CSC resolved to
0x00 from a YUV444 input; HDCP absent. Nothing about the input degraded.

### The result

    token[0x40]=a5a5a5a5  0x44/0x48/0x4c=a5a5a5a5   enc[0x50]=0   EVENT[0x30]=0
    producer watch: 0 changes over 56 s
    captured 0 bytes
    buf[0..3]: 0/1024 sampled pages touched

The M139 sentinel never moved, so **`store_channel_done` did not run** - the
encoder starved here exactly as it does on the baseline. That was expected.

**What was not expected: zero pages were touched.** The baseline's single frame
does not come from the SDK frame path at all (M139); it is pushed by
`vpl_dmac`'s outbound transfer. `win_seq=1` lost *that* too. The Windows
ordering is the first configuration measured that is strictly worse than the
baseline while the receiver stays locked and every command returns 0.

### Reading it

Per M140's decision table this is the `a5a5a5a5` + 0 frames row: the ordering is
not the fix, and the no-`0x06` variant goes with it. The one structural
difference that can plausibly suppress a DMAC push is the **absence of op
0x06** - `win_seq` relies on `0x2d`/`0x31` performing the same bare
`sysfs_notify("epint")`, which is true for the *card's* dispatch but says
nothing about what arms `vpl_dmac` on the host side. `win_start_op6=1` is the
single-variable retry.


---

## M142 (hardware, 2026-08-23): op 0x06 is what arms the DMAC one-shot - and the Windows ordering is neutral

The single-variable retry M141 called for. Same kernel (`7.2.0-1-cachyos`), same
power cycle, same source, same lock.

| run | sequence | frames | token[0x40] |
|---|---|---|---|
| M141c | baseline | 1 | `a5a5a5a5` |
| M141 | `win_seq=1` | **0** | `a5a5a5a5` |
| M142 | `win_seq=1 win_start_op6=1` | **1** | `a5a5a5a5` |

M142's frame scored NOT SPLASH (Y 212 / UV 214 distinct) and CHROMA OK (luma
explains at most 4%). It is the same real 1080p the baseline gives.

(`v4l2-ctl` was killed mid-write because the run asked for 5 frames and one
arrived, so the file is 3108864 of 3110400 bytes. The driver log shows it
delivered the full frame. The oracles were run on a copy zero-padded by those
1536 tail bytes, which lie in the V plane; the splash crop at (800,420) is in
luma and unaffected.)

### What it settles

**Op `0x06` gates the `vpl_dmac` outbound push.** Adding it back to `win_seq` is
the only change between M141 and M142, and it restores the frame exactly. So
M141's zero was not the ordering breaking capture - it was the missing
START_STREAMING failing to arm the transfer that carries the card's one frame to
the host. M140's reading of `ep.ko` - that `0x2d`/`0x31` perform the same bare
`sysfs_notify("epint")` as `0x06` - is true of the *card's* dispatch and says
nothing about what arms the DMAC.

**The Windows ordering is neutral.** pre-STOP + 1900 ms settle + SET_BUF-first,
with `0x06` present, produces precisely the baseline: one frame, sentinel
intact, `enc[0x50]=0`, `EVENT=0`, 0 producer-watch changes. Every command
returned 0 and `R55=0x7f` held throughout, so this is a clean negative, not a
misfire.

**The host-side sequence is now exhausted.** Ordering, opcode set, kicks,
credit, completion handshake, geometry, preview params, CSC - all measured, all
either neutral or worse. Nothing the host sends changes how many times the VIC
captures.

---

## M143 (static, 2026-08-23): the cfg patcher decoded exactly - and `captured frame count` is a host-unreachable 60

`video_capture_mgr` 0xa290 is the patcher. It reads
`nullsensor_1920x1080.cfg`, rewrites selected lines, and writes
`/tmp/nullsensor_yuan%d.cfg`. The loop counter is `mov r4, #163` - exactly the
163 lines of the cfg in the tree, which confirms the template is unchanged on
the card.

It uses **two** mechanisms, not one. Per line, in order:

| test | line rewritten with |
|---|---|
| `strstr "input format"` | arg 4 (r7) |
| `strstr "output format"` | arg 5 (r8) |
| `strstr "start x position"` | r9 |
| `strstr "start y position"` | r10 |
| `strstr "input frame width"` | r11 |
| `strstr "input frame height"` | struct halfword |
| `atoi(line) == 1920` | **and** `strstr "maximum frame width"` -> struct halfword; otherwise -> arg 2 (width) |
| `atoi(line) == 1080` | arg 3 (height) |
| `strstr "flip video"` | struct halfword |
| `strstr "mirror video"` | struct halfword |
| none of the above | copied through unchanged |

Two corrections to the record:

- **M126 ("tinyvenc5 patches exactly nine keys") and M140 ("the patcher is in
  `video_capture_mgr`") are both half right.** Both binaries carry the key
  strings and both reference `/tmp/nullsensor_yuan%d.cfg`; the cfg is patched
  by vcm and then read (and partly re-patched) by tinyvenc5.
- **`maximum frame width` is special-cased**, so M140's "rewrites every line
  valued 1920 blindly" is not quite right - the ISP allocation width takes a
  different value from the captured width. `maximum frame height` has no such
  case and does take the SET_VIC height. M140's method rule still stands: nine
  AE window pairs and the max-height line still move with a height sweep.

### The finding: line 4

    4    60      // captured frame count

**No binary in the image contains the string `captured frame count`** - not
`libvideocap.so.13`, not `libtk_video_capture.so.0`, not
`libtkmf_video_source.so.0`, not tinyvenc5, not vcm. The comment text exists
only in the cfg, so the readers parse it **positionally**, and field 4 is read
as whatever the file says.

It is not a patcher key, and 60 is neither 1920 nor 1080, so **it passes through
untouched and no host command can reach it.** Every stream this card runs is
configured for a 60-frame capture.

This is **not** the one-frame cause - 60 is not 1, and nothing reduces it. But
it is a hard ceiling that would bite immediately after the cadence is fixed, and
it is worth knowing before anyone reads "60 frames then stops" as a new defect.

### A caution about what the frame count actually measures

The host's frame count is **not** a measurement of VIC captures. M142 shows the
one host-visible frame is delivered by the `vpl_dmac` one-shot that op `0x06`
arms; the SDK frame path (`channel_done` -> `ep.ko`) has never run at all
(M139). "The VIC captures exactly one frame" remains the simplest model that
fits, but it is inference, and no host-side counter can distinguish one VIC
capture from several that were never published.


---

## M144 (static, 2026-08-23): `vpl_vic.ko` disassembled - the ISR's re-arm decision located, and the release path cleared

First disassembly of the VIC driver in this tree. Kept as `re-dump/vpl_vic.txt`
(with relocations, so external calls resolve) and `re-dump/vpl_vic.sym`:

    llvm-objdump -dr --triple=armv5te-linux-gnueabi <unpack>/yuan_demo_sdi/drivers/vpl_vic.ko

| symbol | addr |
|---|---|
| `VIC_SetSizeToVIC` | 0x0000 |
| `VIC_AEWBOneframeWQ` | 0x0154 |
| `VIC_AutoFocusOneframeWQ` | 0x0768 |
| `Close` | 0x0b3c |
| **`ISR`** | **0x0fd8** |
| `Open` | 0x2150 |
| `MMap` | 0x27fc |
| **`Ioctl`** | **0x2850** |
| `VIC_GetDevNum` / `VIC_DetectStd` / `VIC_AutoDetectStdTasklet` | 0x4a18 / 0x4a68 / 0x4bc0 |

### The ISR wakes the waiter exactly once per frame, and it is conditional

The ISR (0xfd8..0x2150) makes **one** `__wake_up` call, at **0x17a8**, reached
only from `beq 0x1798` at **0x1468** on a local flag at `[r11-0x48]`. Its other
external calls are three `schedule_work`s (the AE/AWB/AF one-frame work queues),
two `printk`s, `do_gettimeofday` and a divide. There is no second wake path, so
`frame_ready` is set at exactly one place and the whole cadence question reduces
to how often the ISR reaches it.

### The re-arm decision - first time this project has located one

At **0x17b0** the ISR clears bit **10** (`#1024`) of the channel word at
`[regbase + (r9+4)*4]`, then:

    17bc  ldr  r3, [r1]              ; register block
    17cc  ldr  r5, [r3, #0x8]
    17d0  tst  r5, #0x60             ; bits 5:6
    17d4  bne  17e4                  ; -> re-arm
    17d8  ldr  r3, [r3, #0xc]
    17dc  tst  r3, #0x60
    17e0  beq  1928                  ; -> STOP
    17e4  str  r6, [r9, #0x10]       ; r6 = value | 1024  -> capture stays enabled

The `0x1928` arm does **not** simply skip the re-arm - it clears bit **0** of
`[regblock + 4]`:

    1948  bic  r5, r5, #1
    194c  str  r5, [r3, #0x4]

So the hardware has an explicit **"capture one more frame, or halt"** decision
taken inside the ISR, gated on bits 5:6 of two registers at block offsets 8 and
0xc. This is exactly the shape of the observed defect. **What those bits mean is
not derivable from the image** - there is no VIC datasheet in the tree and the
registers are raw offsets - so this locates the mechanism without yet naming it.

### The release path is NOT the re-arm, and adds no lever

`VideoCap_ReleaseBufVIC` (`ioctl(fd, 0x4004e304, idx)`) dispatches at **0x29d4**,
confirming M140's map, and the handler is purely bookkeeping:

- per-buffer struct is **120 bytes** (`idx*15` then `<<3`), array at `[dev+0x90]`
- it linearly searches the file's queued list at `[file+0x10]`, length
  `[file+0x18]`
- on a hit (0x3dc4) it **compacts the array** and decrements `[file+0x18]`
- `[file+0x1c] == 1` branches to the **force-release** path at 0x4754 (this is
  libvideocap's "[yuan][Sleep] Force Release Done"), not the normal one
- otherwise it decrements the per-buffer refcount at `[buf+0x64]` and returns

**It writes no VIC register at all.** So "the host/consumer releasing a buffer
is what re-arms the VIC" is dead as stated: release touches only the driver's
own list and refcount. Whatever sets bits 5:6 at block offsets 8/0xc, it is not
this ioctl directly.

### `capture_app_infinite` is an AUDIO app - closed

The name is the most inviting thing in the image and it is a false lead. It is
dynamically linked against `libasound.so.2`, `libmembroker.so.0`,
`libtk_mass_mem_access.so.0`, `libmassmemaccess.so.9` and `libmemmgr.so.4`, its
only TK imports are `TK_MMA_Init` / `TK_MMA_SetOptions` /
`TK_MMA_ProcessOneFrame`, and its own usage string is

    "for HD use ./capture_app_infinite -D -d 0 -R 48000 -F 256 -B 4"

- rate 48000, 256 frames, 4 buffers. It touches no VIC and no video capture
library. There is **no continuous-video reference application in the image.**


### The other half: the same bits gate the ARMING write, in `Ioctl`

`0x60` appears exactly **four** times in the whole module - twice in the ISR
above, twice at **0x3a3c / 0x3a48** inside `Ioctl`, and nowhere else. The Ioctl
site tests the identical pair and uses it to skip a write:

    3a38  ldr  r1, [r3, #0x8]
    3a3c  tst  r1, #0x60
    3a40  bne  3a8c              ; busy -> skip
    3a44  ldr  r1, [r3, #0xc]
    3a48  tst  r1, #0x60
    3a4c  bne  3a8c              ; busy -> skip
    3a50..3a84                   ; program the capture target:
                                 ;   [dev+0x1c] = ([chan+0x40] << 2) | (old >> 16 << 16)
                                 ;   [dev+0x20] = (old & 0xffff) | ([chan+0x44] << 18)

So bits 5:6 of block offsets 8 and 0xc read as **"a capture is armed / in
flight"**:

| site | bits set | bits clear |
|---|---|---|
| `Ioctl` 0x3a3c | skip - do not disturb an in-flight capture | program the next capture target |
| `ISR` 0x17d0 | keep bit 10 - capture stays enabled | **0x1928: clear bit 0 of `[block+4]` - halt** |

That is a coherent one-shot machine: the VIC runs while something keeps arming
it, and the ISR shuts it down the moment it finds nothing armed. It matches the
observed defect exactly, and it is the first mechanism in this project that
does.

### The question this leaves, stated precisely

**Who calls the arming ioctl, and is that call gated on a buffer release?**

- If `libtk_video_capture`'s `process()` re-arms unconditionally after
  `VideoCap_WaitVIC` returns, then arming is independent of the encoder, the
  deadlock model below is wrong, and the halt is somewhere else.
- If arming happens only after a consumer releases a buffer, the chain closes
  into a card-side deadlock: `img_handler`'s first call is init-only and
  publishes nothing (M139) -> the SSM ring never fills -> `encode_handler` parks
  -> nothing is released -> nothing re-arms -> the ISR halts the VIC after one
  frame. Everything observed follows, **and no host command can break it** -
  which would redirect the whole project to making frame 1 publish rather than
  to making frame 2 arrive.

Answering it needs the caller chain in `libvideocap.so.13` /
`libtk_video_capture.so.0`, not more `vpl_vic`. It is static and costs zero
spawns.


---

## M145 (static, 2026-08-23): there is no per-frame software re-arm - the buffer-lifecycle theory is dead

M144 left one question: *who calls the arming ioctl, and is it gated on a
buffer release?* Answered, and the answer kills the theory that has been step 2
of the handoff since M140.

### The VIC ioctl map, from `libvideocap.so.13`

| wrapper | ioctl | notes |
|---|---|---|
| `VideoCap_WaitVIC` | `0xe301` | bare tail-jump to `ioctl` |
| `VideoCap_Sleep` | - | **tail-jump to `VideoCap_WaitVIC`.** Confirms M138's chain. |
| setup | `0x4028e302` | `_IOW`, 40 bytes |
| `VideoCap_GetBufVIC` | `0x8078e303` | `_IOR`, **120 bytes** - exactly the per-buffer struct size M144 measured in `vpl_vic` |
| `VideoCap_ReleaseBufVIC` | `0x4004e304` | `_IOW`, 4 bytes (the index) |
| `VideoCap_StartVIC` | `0xe313` | retries in a loop until it returns 0 |

`vpl_vic`'s dispatch confirms the pairing: `0x8078e303` at 0x2af4 -> handler at
0x2b00; `0xe313` at 0x299c -> handler at 0x3c54; `0x4020e305` at 0x29b8, from
which the release cmd `0x4004e304` is built inline (`sub #0x1c0000; sub #1`),
which is why grepping for the literal finds nothing.

### The per-frame loop does not re-arm

`libtk_video_capture.so.0` imports only the **non-VIC** wrappers -
`VideoCap_GetBuf`, `VideoCap_ReleaseBuf`, `VideoCap_Start`, `VideoCap_Stop`,
`VideoCap_Sleep`, `VideoCap_Initial`, `VideoCap_Release`, `VideoCap_SetOptions`.
So the steady-state loop is `GetBuf` -> consumer -> `ReleaseBuf`.

`VideoCap_GetBuf` (0x43f0) calls **`VideoCap_GetBufVIC` and nothing else** on
its normal path; its only other calls are `ReleaseBuf`/`ReleaseBufVIC` on the
reject paths and two `puts` banners. `VideoCap_ReleaseBuf` (0x43d8) is a
one-line forward to `VideoCap_ReleaseBufVIC`.

The `VideoCap_StartVIC` call at 0x45b0 is **not** inside `GetBuf` - it sits past
`GetBuf`'s literal pool (0x4594..0x45a8), in an unnamed local helper that
`VideoCap_Start` uses. `GetBuf` never re-arms.

### And the arming write is in the SETUP ioctl, not a per-frame one

The capture-target programming M144 found at `vpl_vic` 0x3a50 - the block
guarded by the bits-5:6 busy test - lies inside the long
`0x4028e302` **setup** path (0x37c0..0x3a8c), which also fills `[chan+0x14]`
through `[chan+0xc4]` and pushes geometry to `[dev+0x4]`, `[dev+0x8]`. It runs
**once**, at open/configure time.

### Consequence

**No software re-arms the VIC per frame.** The sequence is: setup programs the
target once, `StartVIC` (`0xe313`) sets bit 10 plus `0xe8` in the channel word
at `[regs + chan*4 + 0x10]`, and the hardware then free-runs. The ISR's job is
only to keep it enabled - and to **halt** it (0x1928, clearing bit 0 of
`[block+4]`) the moment bits 5:6 of `[block+8]`/`[block+0xc]` read clear.

So the deadlock model from M144 is **dead**: capture does not stop because
nobody released a buffer, and `VideoCap_ReleaseBufVIC` writing no VIC register
(M144) is consistent, not suspicious. The card halting after one frame is a
**hardware-state** condition, not a missing call.

This closes handoff step 2 ("`vpl_vic.ko`'s buffer lifecycle") as an avenue.

### The one lead it leaves

Earlier in the ISR (0x16xx-0x1728) there is a block that copies `[chan+0x200]`
.. `[chan+0x20c]` into `[block+0x278]` .. `[block+0x284]`, and then two guarded
extras:

    1728  ldr r3, [r8, #0x210] ; cmp #1 ; streq [chan+0x214] -> [block+0x2c8]
    173c  ldr r3, [r8, #0x218] ; cmp #1 ; then a read-modify-write of
                                          [block+0x204] bits 1:0 from [chan+0x21c]

Those are per-frame register updates gated on **software flags in the channel
struct** (`+0x210`, `+0x218`), which the setup ioctl fills from the SDK's
options - i.e. ultimately from the cfg. That is the only remaining place where
something host-influenced changes what the ISR does per frame. It is the next
thing to read, and it is static.


### Addendum: the ISR's two gated extras never execute

`[chan+0x210]` and `[chan+0x218]` - the flags gating the per-frame extras at
0x1728/0x173c - are **never written anywhere in the module**. Every store to
those offsets (0x1c88, 0x1e6c, 0x39bc) targets the *register block* (`r3 = [r5]`),
not the channel struct (`r8`, confirmed as the software struct because 0x1798
passes it straight to `__wake_up`). Zero-initialised and never set, so both
`cmp #1` tests always fail and neither extra runs.

That was the last host-influenced per-frame path in the ISR, and it is dead.
No knob reaches the ISR's behaviour.

### What that leaves: two live models, and they are distinguishable

Everything above assumes the M140 model - the VIC captures once and halts. The
evidence for it is entirely indirect. A second model fits every observation just
as well:

**Model B: the VIC keeps capturing; op `0x06` arms exactly one DMAC push.**
M142 proved `0x06` arms the outbound transfer. If it arms *one* transfer, then
one frame per stream is a property of the *delivery* path, not the capture path,
and the SDK path is broken somewhere in publishing (`img_handler` taking the
init path repeatedly, `TK_ImgProc_Init` failing, etc.) rather than in capture.

The two differ in a directly measurable way. Fire **one** `0x06` several seconds
after the first frame and compare the delivered images:

- **different image** -> the VIC was still capturing. M140's model is wrong, and
  the whole project redirects from "make frame 2 arrive" to "make frame 1
  publish".
- **byte-identical image** -> the DMAC re-pushed a stale buffer. The VIC halted,
  M140's model is confirmed, and it is confirmed *directly* for the first time.
- **no second frame** -> `0x06` arms once per stream and nothing re-arms.

This is not the kick avenue M133 closed. M133 flooded 1224 kicks, lost the
receiver lock, and was asking whether kicks *pace* the cadence. This uses a
single well-separated kick as a **probe of the producer**, which is a different
question and a different regime.


---

## M146 (hardware, 2026-08-23): 12 kicks, one frame - the producer is dead, and the flood was what cost M133 its lock

The probe M145 designed. `op6_kick_ms=5000 kick_repeat=1`, one op `0x06` every
5 s after the first delivery.

    poll-drain: first kick op 0x06 ret=0
    poll-drain stopped after 1 deliveries, 12 kicks (op 0x06);
        producer watch saw 0 change(s), final 40=a5a5a5a5 44=a5a5a5a5 48=a5a5a5a5
    stream stop: EVENT[0x30]=0 token[0x40]=a5a5a5a5 enc[0x50]=0
                 irq_total=6 frame_events=0 fifo_drops=0

**Model B is dead.** If the VIC were still capturing and each `0x06` armed a
DMAC push, twelve kicks over 55 s would have produced roughly twelve frames. It
produced one - the same one the baseline gives, delivered before the first kick.
The poll-drain kept sampling buffer 0 for the remaining 55 s and it never
refilled.

So op `0x06` arms the outbound push **once per stream**, at stream start, and
re-firing it later does nothing. M140's model survives its first direct test:
the card stops producing, and the defect is upstream of delivery, not in it.

### Correction to M133: kicks do not cost the receiver lock. Flooding does.

M133 fired 1171-1224 kicks and reported that "the receiver **loses lock** under
either". At 5 s spacing the lock is untouched - `R55=0x7f` before START, after
START and at stop, and every `detect` line through the whole 55 s reads
`55=7f LOCKED coherent`. The lock loss was an artefact of the flood rate, not a
property of the opcode. The rest of M133 stands.

### The frame that arrives is RAW I420, and that is a clue nobody has followed

`enc[0x50]=0` says no bitstream was ever DMA'd and the sentinel says
`encode_handler` never ran - yet the frame the host receives is a valid planar
I420 image, not H.264. **It cannot have come from the encoder.** The driver's
"inferred H.264 length" wording is legacy naming from before M129.

That narrows the producer: something on the card is pushing a **raw preview
frame** over `vpl_dmac`'s outbound transfer, exactly once, and it is not the SDK
encoder path. The knobs that pace a preview - op `0x31`'s `fps` (0x09),
`skip` (0x0a), `avg` (0x0b), `preview_off` (0x0d), `preview_no_osd` (0x0f),
`hw_d` (0x12) - are host-settable and, apart from `fake_frame_off`, have never
been swept with a working delivery path. M138 established that `m_fps`/`m_avg`
are never read and `m_skip` drops one in `(skip+1)`, but that was about
`img_handler`, i.e. the path *into the SSM ring for the encoder*, which is a
different consumer from whatever is writing to the host.

**Next, and it is static:** find who initiates the outbound transfer.
`ep.ko` exports `pcie_set_outbound`; `vpl_dmac.ko` is its only importer
(`VPL_DMAC_StartTail` 0xc94, `VPL_DMAC_ISRTail` 0xf70). Work out which
process calls into `vpl_dmac` with a raw frame, and the knobs that pace it
become visible. `re-dump/vpl_dmac.txt` is already in the tree.


---

## M147 (static, 2026-08-23): the raw frame proves the SDK pipeline completes one FULL pass - M139's "the encoder never ran" is too strong

M146 established the delivered frame is raw I420, not H.264. Tracing what can
write raw video into host memory forces a revision of the M139/M140 model.

### Only one thing on the card can write to host memory

`ep.ko` exports `pcie_set_outbound`; **`vpl_dmac.ko` is its only importer**
(M139). `vpl_dmac` is reached only through `/dev/vpl_dmac`, and the only
userspace object that names that node is **`libmassmemaccess.so.9`**, wrapped by
`libtk_mass_mem_access.so.0`. Of the video binaries, only **tinyvenc5** links
both (`video_capture_mgr` links just `libmemmgr`; `libtkmf_video_source` links
only pthread/libc). The audio apps link it too, which is what
`capture_app_infinite` is doing (M144).

And every `TK_MMA_*` call in tinyvenc5 - `Init`, `SetOptions`,
`StartOneFrame`, `WaitOneFrameComplete`, `ProcessOneFrame`, `Release` - is
inside **`EncodingGroup::encode_handler`**. `TK_MMA_Init` is in its prologue
(0x12b7c, 0x12ba0, before the loop head at 0x12f04); every push site
(0x1349c, 0x13e64, 0x142ac, 0x1430c) is in the loop body, i.e. **after** the
first `SSM_ReleaseAndReceive` at 0x12f24 (M138).

### Therefore the ring delivered at least once

`img_handler`'s publish is the `SSM_DeliverAndAllocate` at **0x1290**, reached
only when `[r4+0xc] != 0`; when it is zero the call goes to 0x129c, the init
path, which is where `SSM_Writer` (0x14f4) and the priming
`SSM_DeliverAndAllocate` (0x1544) live. So the first call initialises and
publishes nothing - M139 is right about that - and **the publish at 0x1290
requires a second call**.

Chaining it: a raw frame reached the host -> `encode_handler` executed a loop
body -> its first `SSM_ReleaseAndReceive` returned -> the ring was non-empty ->
`img_handler` published at 0x1290 -> `img_handler` was called **at least
twice** -> **the VIC captured at least two frames.**

### What this revises

| claim | status |
|---|---|
| `store_channel_done` has never run (M139, sentinel) | **stands** - measured, not inferred |
| "`encode_handler` is alive but starved, having received zero frames" (M139) | **too strong.** It received at least one and ran at least one full iteration. |
| "the VIC captures exactly one frame and stops" (M140) | **at least two.** The count was never measured; the host's frame count counts DMA pushes, not captures (M143's caution, now concrete). |
| "the one frame does not come from the SDK's frame path" (M139) | **wrong.** It is the SDK path - `img_handler` -> SSM -> `encode_handler` -> MMA -> `vpl_dmac` -> host. What it is *not* is the H.264 bitstream path, which is what `channel_done`/`enc_stat` report. |

So the pipeline is not dead. **It completes at least one full pass and then
stops**, and the failure is narrower than "nothing works": it is whatever
prevents iteration two.

### `EncodingGroup::mma_already_start` is never written

The symbol (`_ZN13EncodingGroup17mma_already_startE`, .bss 0x7eda0, addressed as
`[base-0xfa8]`) has **nine `ldrb` reads and zero writes** anywhere in tinyvenc5.
It is a static bool that stays 0 for the process lifetime, so every branch that
needs it set is unreachable - including the synchronous
`TK_MMA_WaitOneFrameComplete` at 0x14358, which is guarded by
`cmp #0 / beq` at 0x14330 and therefore **never waits for a transfer to
finish**.

That is not obviously the defect - not waiting makes transfers async, which is
permissive rather than blocking - but it is a whole family of dead branches in
the exact function that stopped iterating, and it should be mapped before
anything else in `encode_handler` is trusted.

### Next

The question is no longer "why does the VIC capture once" - it captured at least
twice. It is **"why does `encode_handler` not complete a second iteration"**,
and the answer is somewhere in the loop body between 0x12f24 and the branch back
to the loop head. That is static, it is a bounded region, and the disassembly is
in `re-dump/tinyvenc5.txt`.


---

## M149 (hardware, 2026-08-23): the wedge's second signature - a deaf mailbox that looks exactly like a dead HDMI source

After M146 the card stopped locking. Four consecutive runs, including a plain
control capture with no `EXTRA=`, all ended at
`No coherent HDMI timing was locked during the 45s window`.

It was not the source, and it was not that session's driver changes.

    CMD_INIT got no answer (-110), STATUS=00000000 EVENT=00000000
        RESULT=00000000 bar5[dc]=00000000 bar5[30]=fc200004 bar5[38]=fc20005f
    card handshake failed (-110) - the mailbox is deaf
    MST3367 bring-up skipped - firmware not ready

PCIe is healthy in this state: the device enumerates at 0000:04:00.0, an IRQ is
assigned, the subsystem ID reads correctly and **BAR reads return sane non-zero
values**. Only the card's own mailbox service is gone. Since the handshake never
completes, `mz0380_mst3367_bringup()` is skipped entirely - the receiver is
never touched - so every run reports no lock. The I2C probe agrees and says so
plainly: `periph: card handshake not complete`, `0 of 0 registers read`.

This is a **different presentation** of the 8-18 stream wedge from the
`SET_VIC ret=-110` wall this file has always described. It appeared after
roughly seven streams on that power cycle.

### The driver changes were ruled out by experiment, not by argument

The suspicion was reasonable - the failures began right after a rebuild - so it
was tested rather than reasoned away. `ko/mz0380-7.2.0-1-cachyos.ko`, built
before the `bitstream_num` knob existed, was insmod'd directly and produced the
identical deaf-mailbox handshake failure. The control run that failed had also
carried no `EXTRA=`, so `bitstream_num` defaulted to 1 and its `SET_VIC` was
byte-identical to M146's - and in any case the failure happens before `SET_VIC`
is ever sent.

### Two traps worth not repeating

- `mz0380-m55-real-capture.sh` prints `waiting for the card to finish booting
  its own flash image...` and then `using /dev/video0` **regardless of whether
  the handshake succeeded.** Neither line is evidence the mailbox is alive, and
  reading them as such cost a wrong diagnosis in this session.
- A run that aborts before stream start leaves `token[0x40]=00000000`,
  `irq_total=0` and **all-zero, unpoisoned buffers**, because the sentinel seed
  and the poison fill both happen *at* stream start. Read as a wedge signature
  that is badly misleading: it is simply an abort. Only compare those fields
  across runs that actually reached `stream start:`.


### Recovery confirmed, and the 2-second wedge test

A full power-down cleared it: `CMD_INIT answered on attempt 1
(status=0xdddddddd)`, and the very next control capture delivered a complete
3110400-byte frame with `R55=0x7f` throughout and the sentinel intact.

The check that distinguishes the two states costs **2 seconds and zero spawns**,
against 94 seconds for a capture that cannot succeed anyway:

```bash
sudo modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
sudo insmod ./mz0380.ko; sudo dmesg | grep -a "CMD_INIT\|handshake" | tail -3; sudo rmmod mz0380
```

The `modprobe` line is not optional: without it `insmod` fails with
`Unknown symbol in module`, which is a missing dependency and says nothing about
the card. `Invalid module format` is a third, unrelated failure - vermagic
mismatch against the running kernel.


---

## M148 (hardware, 2026-08-23): `bitstream_num=2` changes nothing - clean negative on a field frozen since M22

M147 left `encode_handler`'s first post-receive test as the live suspect:

    12f24  bl   SSM_ReleaseAndReceive
    12f48  ldr  r2, [r12, #0x34]      ; per-channel field, stride 60
    12f4c  cmp  r2, #1
    12f50  bls  0x13d04               ; <= 1 -> alternate path

Our streams report `bitstreams=1`, and the alternate path is what we observe: a
raw frame over MMA, no H.264 bitstream, `channel_done` never written. SET_VIC
byte 28 (`bitstream_num`) has been **hardcoded to 1 since M22** and never
varied - the only established fact about it was that firmware rejects 0.

A `bitstream_num` module parameter was added (default 1, so the baseline is
unchanged) and the SET_VIC log line now prints the real value instead of a
hardcoded `bitstreams=1`.

    stream start: SET_VIC(... -> H.264 output=1920x1080, bitstreams=2) ret=0
    poll-drain stopped after 1 deliveries, 0 kicks; producer watch saw 0 change(s)
    stream stop: EVENT[0x30]=0 token[0x40]=a5a5a5a5 enc[0x50]=0
                 irq_total=6 frame_events=0 fifo_drops=0

**Identical to the baseline in every field.** The firmware accepts 2 without
complaint - `ret=0`, no `no_signal` latch, lock held at `R55=0x7f` throughout -
and it makes no difference at all.

So either `[chan+0x34]` is not `bitstream_num`, or the `<= 1` branch is not what
starves the encoder. The identification was flagged as inference when the test
was designed; it did not survive. The knob stays in the tree at its default of
1, since it costs nothing and the field is now settable if a future hypothesis
needs it.

Method note: this cost one spawn and answered cleanly because the value was the
**only** thing that changed, on a card whose health had just been confirmed by a
control capture. That is what M140's method rule asks for.


---

## M150 (static, 2026-08-23): `channel_done` is dead code, and `[chan+0x34]` is a warm-up counter

Two findings from the loop body, both of which tighten earlier results.

### 1. The `channel_done` write is guarded by a flag that is never set

`EncodingGroup::encode_handler` writes a 24-byte record at **0x13538**:

    13500..13534   build the record at sp+312
    13538          pwrite(fd=[sp+0x78], sp+312, 24, 0)
    1353c          pthread_mutex_unlock ...

`[sp+0x78]` is the fd from the `open()` at 0x12c64 - the `channel_done` open
M138 identified. And the whole block containing it is jumped over:

    133e4  ldrb r1, [r6, #-0xfa8]      ; EncodingGroup::mma_already_start
    133ec  cmp  r1, #0
    133f0  beq  0x1353c                ; always taken -> skips 0x133f4..0x13538

M147 established that `mma_already_start` has **nine reads and zero writes**
anywhere in the binary; there is also no literal-pool word holding its address
(0x7eda0), so it is only ever reached through the `[base-0xfa8]` form that was
enumerated. It is 0 for the process lifetime.

**So this `pwrite` can never execute**, and with it the `TK_MMA_ProcessOneFrame`
at 0x1349c in the same block. That is the mechanical explanation for M139's
sentinel result: `store_channel_done` has never run because the code that would
call it is unreachable in this firmware image.

The consequence is worth stating plainly: **the completion/reporting path the
host driver waits on cannot fire for anybody**, us or Windows. The poll-drain
(M111) is not a workaround for a driver we got wrong - on this image it is the
only mechanism there is.

A second 24-byte `pwrite` to the same fd exists at **0x13dfc**, on a different
path (reached from the `bne` at 0x134d8/0x134e4), and it jumps to 0x1353c
afterwards. That one is not behind the dead flag, so it is where any surviving
report would come from. It did not fire either.

### 2. `[chan+0x34]` is a self-incrementing warm-up counter, not `bitstream_num`

The first test after the receive turns out to be a counter against itself:

    12f48  ldr  r2, [r12, #0x34]       ; r12 = r10 + ch*60
    12f4c  cmp  r2, #1
    12f50  bls  0x13d04                ; <= 1 -> discard path
    ...
    13d10  rsb  r3, r1, r3             ; r3 = ch*15
    13d14  add  r3, r10, r3, lsl #2    ; = r10 + ch*60, the SAME address
    13d18  add  r2, r2, #1
    13d20  str  r2, [r3, #0x34]        ; counter++
    13d28  b    0x12f04                ; back to the loop head

The branch taken when the value is `<= 1` increments that same value and loops.
So the first two receives are **discarded as warm-up** and the main body does
not run until the third.

This independently confirms M148's negative: the field is a counter, so
`bitstream_num` was never going to move it, which is exactly what the hardware
said.

### What it does to the frame accounting

If the counter means what it appears to, the arithmetic is:

| receive | counter on entry | action |
|---|---|---|
| 1 | 0 | discard, counter -> 1, loop |
| 2 | 1 | discard, counter -> 2, loop |
| 3 | 2 | main body - MMA push, i.e. **our one delivered frame** |

Working backwards through M147's chain: three receives means `img_handler`
published three times, and its first call is the init path that publishes
nothing - so **`img_handler` was called at least four times and the VIC
delivered at least four frames.**

That is stacked inference and is flagged as such. But it moves the same
direction as M147 and away from M140: every time this has been examined more
closely, the number of frames the card actually captured has gone **up**, and
the "captures once and halts" model has looked worse. The defect is not that
the card cannot capture.


---

## M151 (hardware, 2026-08-23): the enc_stat ack is inert - and the frame-token sentinel has been measuring dead code

`enc_stat_ack=0`, removing the last write the driver makes to the card after
START. Result identical to baseline in every field: one delivery, one frame,
`R55=0x7f` held, `EVENT=0`, sentinel untouched.

    poll-drain stopped after 1 deliveries, 0 kicks; producer watch saw 0 change(s)
    stream stop: EVENT[0x30]=0 token[0x40]=a5a5a5a5 enc[0x50]=00000000
                 irq_total=6 frame_events=0 fifo_drops=0

### What it settles

**M40's rationale was wrong, and harmless.** "Without this ack the card's
encoder produces exactly one bitstream and then skips every subsequent frame"
has been in the tree since M40 and shaped M117/M118. Removing the ack changes
nothing, which is what M150 predicted: the completion record it answers is
written by a `pwrite` that cannot execute.

**The driver now provably does not disturb the card mid-stream.** After START it
makes no writes at all in this configuration - no ack, no credit re-arm
(default off), no kicks (default off) - and the cadence is unchanged. Every
host-side write after START is eliminated as a cause of the one-frame stall.

**`enc[0x50]=0` was measured for the first time as a CARD-authored value.**
Every previous run had the driver clearing that register after the frame, so
the 0 in the stop line was partly self-inflicted. With the ack disabled the
card owns it for the whole stream, and it is still 0: the card genuinely never
set enc_stat. The encoder never wrote a bitstream, independent of anything we
did.

### The sentinel is blind, and several decision tables in this file relied on it

M139 seeded `a5a5a5a5` into BAR0 0x40/0x44/0x48/0x4c and called the result "a
host-visible oracle" for whether the card's encoder ran. Those registers are
written by `ep.ko`'s `store_channel_done`, which runs when the card's userspace
`pwrite`s the `channel_done` node.

**M150 showed that `pwrite` is unreachable** - it sits inside the block guarded
by `mma_already_start`, a flag with nine reads and zero writes in the whole
binary. So the sentinel **cannot move, no matter what the card does**. It is
not an oracle for the producer; it is a detector for a code path that does not
exist in this firmware image.

Consequences, and they are not small:

- Every "token moves off `a5a5a5a5` -> the encoder ran" row in this file's
  decision tables (M140's Windows-ordering table, M142, M146, M148, M151) is
  **unsatisfiable**. Those runs were reading a constant.
- The "producer watch saw 0 change(s)" line means nothing about the producer.
  It should be read as "the dead path is still dead".
- The *frame counts* in all those runs remain valid, because they were measured
  by the poll-drain against the poison boundary, which is independent.

**The only working oracle this project has for card-side progress is the number
of complete frames the poll-drain delivers.** Nothing else that has been tried
observes the producer at all. Any future test must be designed against frame
count, not against the sentinel.


---

## M152 (static, 2026-08-23): CORRECTION - M150's "dead code" and M151's "blind sentinel" are both withdrawn

M150 claimed the `channel_done` `pwrite` at tinyvenc5 0x13538 is unreachable
because `encode_handler` jumps over it whenever `mma_already_start == 0`, which
is always. M151 built on that and declared the M139 frame-token sentinel blind,
marking several decision tables in `NEXT_SESSION_START.md` unsatisfiable.

**Both are wrong.** The error: an address *range* was treated as a basic block.
ARM code here is not laid out linearly, and three unconditional branches from
outside the range jump into the middle of it, ahead of the write:

    13e9c  b 0x134a0
    142b0  b 0x134a0
    1582c  b 0x134a0

0x134a0 lies inside 0x133f4..0x13538 and **before** the `pwrite`. Entering
there reaches 0x134b8 -> 0x134d4 -> 0x134e8 -> 0x13500 -> 0x13538 without ever
testing `mma_already_start`. Targets 0x1341c, 0x13438, 0x13458 and 0x134b8 are
likewise branched to from outside.

So:

| claim | status |
|---|---|
| `channel_done`'s `pwrite` is unreachable (M150) | **withdrawn** |
| the completion path cannot fire for anybody, Windows included (M150) | **withdrawn** |
| the M139 sentinel cannot move (M151) | **withdrawn** - it can |
| the decision tables keyed on `token[0x40]` are unsatisfiable (M151) | **withdrawn** - they stand |

### What survives, and it was verified differently

- **`mma_already_start` has nine reads and zero writes**, and no literal-pool
  word holds its address (0x7eda0). That was checked directly, not inferred
  from layout, and it stands. It is 0 for the process lifetime, so the
  fall-through at 0x133f0 always skips, and the synchronous
  `TK_MMA_WaitOneFrameComplete` at 0x14358 - guarded by the same flag at
  0x14330 - **never waits**.
- **`[chan+0x34]` is a self-incrementing warm-up counter.** Verified by address
  arithmetic: 0x12f48 loads from `r10 + ch*60 + 0x34`, and 0x13d10-0x13d20
  recomputes the identical address and stores value+1. Independent of layout,
  and it stands. It also independently explains M148's flat result.
- **M151's hardware measurements stand in full** - they were measurements, not
  inference. `enc_stat_ack=0` changes nothing, the driver makes no writes to
  the card after START in that configuration, and `enc[0x50]=0` is a
  card-authored value.

### Method note, at cost

M150 and M151 were both committed before this was caught. The specific mistake
is worth naming because it is easy to repeat on this codebase: **`beq` over a
range does not make the range dead.** Before calling any code unreachable in
these binaries, enumerate the branches whose *target* lies inside the range,
not just the branch that skips it. `grep -oE "0x1[0-9a-f]{4} <"` over the
function and a numeric filter takes seconds and would have prevented both.


---

## M153 (static, 2026-08-23): the reachability question settled mechanically - M150's conclusion restored, its argument replaced

M150 claimed `channel_done` is unreachable. M152 withdrew that because the
argument was invalid (a branch over a range says nothing about branches into
it). Rather than hand-trace a third time, the analysis was **mechanised**:
`mz0380-cfg.py` builds a basic-block CFG from the objdump text and computes
reachability from `encode_handler`'s loop head.

The CFG deliberately **over-approximates** - `bl` is treated as returning, and
an unrecognised branch form falls through - so anything it reports as
unreachable is unreachable under a strictly more permissive model than reality.

`mma_already_start` is provably 0 for the process lifetime (nine `ldrb`, zero
stores, no literal-pool word holding 0x7eda0 - checked directly, and unaffected
by the M152 error). Both `beq`s guarded by it are therefore always taken, so
their fall-through edges are removed:

    (0x133f0 -> 0x133f4)   and   (0x14334 -> 0x14338)

With just those two edges gone, 2452 of 3110 instructions remain reachable, and:

| site | reachable with the flag at 0? |
|---|---|
| `channel_done` `pwrite` 0x13538 | **no** |
| second `pwrite` 0x13dfc | **no** |
| `TK_MMA_ProcessOneFrame` 0x1349c / 0x13e64 / 0x142ac | **no** (all three) |
| `TK_MMA_WaitOneFrameComplete` 0x14358 | **no** |
| 0x134a0 - the entry M152 found | **no** |
| **`TK_MMA_StartOneFrame` 0x1430c** | **YES** |

The three unconditional branches into 0x134a0 that M152 correctly identified
(0x13e9c, 0x142b0, 0x1582c) are themselves reachable only *through* the
flag-guarded fall-through. Removing that one edge makes them, and 0x134a0,
unreachable. That is what hand-reading missed in both directions.

### Where this leaves the three claims

| claim | status |
|---|---|
| `channel_done` cannot be written in this image (M150) | **restored**, on a mechanical proof rather than a range argument |
| the M139 sentinel cannot move (M151) | **restored** - `store_channel_done` needs that `pwrite` |
| M152's correction of M150's *argument* | **stands** - the argument really was invalid, and the CFG is the reason the right answer is now trustworthy |

Three revisions on one question is expensive. The lesson is in the tool: on this
codebase, reachability claims are not eyeball-able and should not be made
without the CFG.

### The substantive finding

**The only MMA push that can execute is the asynchronous
`TK_MMA_StartOneFrame` at 0x1430c, and its completion is never waited on**,
because `TK_MMA_WaitOneFrameComplete` sits behind the same permanently-false
flag. Every synchronous push path and every host-reporting path in
`encode_handler` is unreachable in this firmware image.

That is consistent with everything measured: the host receives a raw frame (a
DMAC outbound transfer, started but never awaited), no `channel_done`, no
`enc_stat`, no EVENT, and the sentinel never moves. It also supplies a
mechanism for the one-frame stall that requires nothing else to be true - a
transfer that is started and never completed leaves the DMAC profile in use, so
the next `StartOneFrame` fails, takes the error path at 0x1513c, and loops
silently. **That last step is inference about `TK_MMA_*` semantics, not
proof** - the libraries would have to be read to confirm it.

### Consequence for testing

Frame count remains the only working oracle, and this time the reason is
proven rather than asserted: the sentinel's writer is unreachable.


---

## M154 (hardware, 2026-08-23): TWO frames in one module load - the limit is per stream cycle, and the producer is fine

The test M153 asked for. Module loaded once with `mz0380-live.sh load`, then two
independent v4l2 captures without unloading:

    v4l2-ctl --stream-mmap --stream-count=1 --stream-to=/tmp/a.raw
    v4l2-ctl --stream-mmap --stream-count=1 --stream-to=/tmp/b.raw

Both delivered a complete 3110400-byte frame. Both scored **NOT SPLASH**
(Y 211 / UV 225 and Y 213 / UV 208 distinct values). And they are **different
frames**, not the same buffer read twice:

    luma mean |a-b| = 45.90    max 209    83.0% of pixels differ by more than 8
    a: mean 141.6 std 57.0     b: mean 132.5 std 57.4
    a row-to-row correlation 0.9691   (a real image; noise would be ~0)

Thirteen seconds apart, and the scene moved. This is the **first time this
project has captured two distinct frames without reloading the driver.**

### What it overturns

Every run in this file until now ended `poll-drain stopped after 1 deliveries`
and the conclusion drawn, repeatedly, was that the card produces one frame and
stops. **It does not.** It produces one frame per *stream cycle*, and a second
STREAMOFF/STREAMON yields another. The producer is healthy; the encoder,
receiver and DMA path all work; what is bounded is the cycle, not the card.

That also matches M153's code shape exactly. `mma_already_start` is a
per-process static, `TK_MMA_StartOneFrame` is the only reachable push and its
completion is never awaited, so the first transfer of a process succeeds and
nothing arranges a second. A fresh stream cycle gets a fresh start.

### The open question this immediately raises

**Does a stream cycle respawn tinyvenc5?** It matters a great deal:

- If STREAMON re-sends SET_VIC, each cycle costs one encoder spawn, the budget
  is the familiar 8-18, and this is a diagnostic rather than a path to video.
- If the cycle is cheaper than a spawn, then cycling is a **cadence mechanism** -
  crude, but the first one this project has found that yields more than one
  frame.

The answer is in the dmesg of this run: a second `stream start: SET_VIC(...)`
line means respawn. Read it before designing anything on top.

### Method note

This cost two spawns and overturned a model that eight milestones had been
reasoning within. It was worth running earlier than it was. The reason it had
not been: every previous harness unloaded the module between captures, so
"one frame per stream" and "one frame per module load" were never separated.
`mz0380-m55-real-capture.sh` unloads on exit by design (M60), which is correct
for its purpose and hid this for the entire history of the file.


### M154 addendum: a stream cycle IS a spawn, and OBS confirms the user-visible shape

The dmesg for that run settles the open question the wrong way for hopes of a
cadence mechanism. The second capture's stream carries its own full start
sequence:

    4773.13  stream start: SET_VIC(...) ret=0
    4775.31  stream start: START_STREAMING(op 0x06) fired
    4776.56  poll-drain: buf 0 holds 3110400 bytes; delivering
    4776.84  stream stop
    4785.13  stream stop            (module unload)

So every STREAMON re-sends SET_VIC, which is what makes `video_capture_mgr`
spawn a fresh tinyvenc5. **One stream cycle = one encoder spawn.** The 8-18
spawn budget therefore caps the whole approach, and each cycle also pays the
~2 s SET_VIC settle. Cycling is a diagnostic, not a path to video.

**The user-visible behaviour agrees, and is worth recording as the plain-English
statement of the defect:** with the module loaded and OBS opened on
`/dev/video0`, the preview shows **one image and then freezes** - moving the
object in front of the camera changes nothing. OBS holds a single stream open,
so it receives that stream's one frame and never gets another. The two distinct
frames of M154 came from running `v4l2-ctl` twice, i.e. two cycles, not from
one stream producing two frames.

So M154's headline stands in a narrower form: **the producer is healthy and
re-captures fresh content on each cycle** - the two frames were 13 s apart with
83% of pixels changed, which proves the card is not stuck holding one image -
but the bound remains one frame per tinyvenc5 process, exactly as M153's
per-process `mma_already_start` latch predicts. This is the same "one frame per
spawn" the NOSG diagnostic has always shown (M60), now confirmed on the real
capture path.


---

## M155 (hardware, 2026-08-23): a cycle without the respawn yields nothing - one frame per tinyvenc5 process, confirmed

`setvic_once=1`, so only the first stream cycle after insmod sends SET_VIC and
later cycles do not respawn tinyvenc5. Three consecutive v4l2 captures in one
module load:

    c1  3110400 bytes                 (cycle 1 - SET_VIC sent, encoder spawned)
    c2  0 bytes   VIDIOC_STREAMON returned -1 (Connection timed out)
    c3  0 bytes   VIDIOC_STREAMON returned -1 (Connection timed out)

(`e3b0c442...b855` is the SHA-256 of the empty file.)

**M153's prediction holds exactly.** The frame comes from the *respawn*, not
from the cycle. `mma_already_start` is a per-process static that is never
written, so a tinyvenc5 process gets exactly one reachable
`TK_MMA_StartOneFrame` and a cycle that reuses the parked process gets nothing.

### What this closes

**"Cycle cheaply for cadence" is dead.** M154 raised it; this kills it. The
only way to get another frame is another SET_VIC, which forks another
tinyvenc5, which costs one of the 8-18 spawn budget and a ~2 s settle. There is
no cheap cycle.

Combined with M154, the whole cadence question now has a single, consistent
answer that fits every measurement in this file:

**one frame per tinyvenc5 process, and SET_VIC is the only way to get a new
process.**

That is why OBS shows one image and then freezes, why every m55 run reports
`1 deliveries`, and why the NOSG diagnostic's respawn-per-frame loop (M60) was
the only thing that ever produced a sequence.

### A new datum worth chasing: the parked encoder stops answering the mailbox

`VIDIOC_STREAMON` did not merely fail to deliver - it **timed out**. With
SET_VIC skipped, some later command in the start sequence got no answer, which
means the parked tinyvenc5 stops servicing the mailbox commands it normally
handles (`0x2d`, `0x31`, `0x06` are all in its set per M127/M128).

That is a fingerprint of *where* it is parked, and it is free to read: the
dmesg for this run names the opcode that timed out. If `0x2d` times out, the
command loop itself is gone; if only `0x06` does, the loop lives and only the
frame path is stuck. Read it before designing anything else.


---

## M156 (hardware, 2026-08-23): our STOP is not what kills tinyvenc5

M155 left one obvious suspect: `video_capture_mgr` contains two
`system("killall -9 tinyvenc5")` sites (0x8d1c, 0x95c4), and if either sits in
the `0x07` handler then the driver's own streamoff removes the encoder. Tested
with `stop_on_streamoff=0` instead of hand-tracing vcm's dispatch.

The knob demonstrably took effect:

    stream stop: STOP_STREAMING SKIPPED (stop_on_streamoff=0) - the card is left
                 streaming on purpose

And the result is unchanged from M155:

    d1  3110400 bytes
    d2  0 bytes   VIDIOC_STREAMON returned -1 (Connection timed out)
    d3  0 bytes   VIDIOC_STREAMON returned -1 (Connection timed out)

    cycle 1: SET_ENC_PARAMS(op 0x2d ...) ret=-110
    cycle 2: SET_ENC_PARAMS(op 0x2d ...) ret=-110

**STOP is exonerated.** tinyvenc5 stops answering `0x2d` on its own, whether or
not the host tears the stream down.

### What is still answering, and what is not

`CMD_INIT` answers immediately on the next load
(`answered on attempt 1 (status=0xdddddddd)`), so the card's firmware and
`ep.ko`'s command service are fine. What has gone is specifically the process
that services `0x2d` - tinyvenc5. The card is healthy at roughly eight spawns
into this power cycle, with no wedge.

### Where that leaves the one-frame bound

Three independent host-side levers have now been removed and none changed
anything:

| lever | milestone | result |
|---|---|---|
| the per-frame `enc_stat` ack (the last mid-stream write) | M151 | no change |
| skipping the respawn (`setvic_once=1`) | M155 | no frame at all |
| skipping STOP (`stop_on_streamoff=0`) | M156 | no change |

**There is no host-side lever left on the cadence.** One frame per tinyvenc5
process, the process becomes unresponsive by itself afterwards, and only a new
SET_VIC - i.e. a new process - produces another frame.

### The next question, and it is static

Does tinyvenc5 **exit** or **hang** after its single push? The distinction
matters because an exit is a decision the code takes and may be conditional,
while a hang is the `SSM_ReleaseAndReceive` park M138 described.

`mz0380-cfg.py` can answer it: from the `TK_MMA_StartOneFrame` success block at
0x14318, check which is reachable - the loop head at 0x12f04, the loop exit at
0x12fc0, or a function epilogue. That costs nothing and does not need the card.


### M156 addendum: the encoder hangs rather than exits, and it is not a mutex deadlock

Two mechanical checks with `mz0380-cfg.py`, both cheap, both negative in the
useful direction.

**Does tinyvenc5 exit after its single push?** No. `encode_handler` contains no
call to `exit`, `_exit`, `abort` or `pthread_exit` anywhere, it has exactly one
epilogue (0x13020), and from the `TK_MMA_StartOneFrame` success block at
0x14318 the loop head at 0x12f04 and the `SSM_ReleaseAndReceive` at 0x12f24 are
both reachable. So the thread loops back and **parks on the receive**, which is
M138's model, rather than terminating.

**Is it a mutex deadlock?** No. `encode_handler` locks at 0x13040, does an
unlock/re-lock pair at 0x13540/0x13548 (the re-lock is executed, because the
`mma_already_start` skip branches to 0x1353c which is between them), and
unlocks at 0x13730, immediately before the back-edge at 0x13734. Cutting every
edge out of that unlock makes both the loop head and the SSM wait
**unreachable** from the re-lock:

    loop head reachable at all               : True
    loop head reachable WITHOUT the unlock   : False
    SSM wait  reachable WITHOUT the unlock   : False

So every path back to the wait releases the mutex first. The encoder does not
park holding a lock that the command path needs, and the obvious explanation
for `0x2d` timing out is eliminated.

**What is left unexplained:** `encode_handler` is a thread created by
`init_func`; tinyvenc5's *main* loop is what services `0x2d`. That thread
parking should not stop the main loop answering the mailbox, yet `0x2d` returns
-110 on every cycle after the first. Reading tinyvenc5's main command loop is
the next static step, and it is the last thread in this investigation that has
not been pulled.



---

## M157 (static + hardware, 2026-08-24): SET_VIC byte 33 `fast_kill` picks SIGKILL over a clean encoder shutdown - the first named mechanism for the 8-18 spawn wedge

The wedge has been a bare observation since M52: the card stops answering after
somewhere between 8 and 18 encoder spawns, only a power cycle clears it, and
nothing in this file has ever said *what* runs out. It has cost whole sessions
(M121, M122) and it caps every experiment that needs more than a handful of
streams (M154, M155).

**`video_capture_mgr` disposes of the previous tinyvenc5 in one of two ways, and
SET_VIC byte 33 chooses which.** From vcm at 0x9554:

```
9554: r0 = &g_setvic            ; 0x141d4
9558: ldrb r3, [r0, #0x21]      ; byte 33 = fast_kill
955c: cmp  r3, #1
9560: beq  0x97d0               ; fk == 1:
                                ;   puts("[Video_MGR] ----> SIG   KILL")
                                ;   kill(pid, 9)          <- no wait, no cleanup
                                ; fk != 1:
9564:                           ;   puts("[Video_MGR] ----> SIG    INT")
956c:                           ;   kill(pid, 2)
9584: kill(pid, 0)              ;   poll, 200 iterations
95a0: usleep(0x2710)            ;   10 ms apart -> 2 s ceiling
95ac: printf("[Video_MGR] timeout ---->break(%d)")
95c4:                           ;   killall -9 tinyvenc5   <- backstop only
```

**And tinyvenc5 has a real clean-exit path that only the second branch reaches.**
`main` installs one handler for SIGTERM(15), SIGINT(2) and SIGSEGV(11) at
0xe418/0xe424/0xe430; the handler is `sig_kill` at 0x187ec:

```
187ec  sig_kill(sig):
         printf(...); usleep(1000)
         if (sig == 11) puts(...)
         g_stop = 1
         if (g_count > 0 && g_group)
                 EncodingGroup::~EncodingGroup(g_group)   ; 0x10d9c
                 operator delete(g_group)
         exit(0)
```

`exit(0)` also runs the atexit chain, and `libsyncsharedmemory.so.0` registers
one (`reg_release_func`, `SSM_Recycle`). So the SIGINT path gets the encoder
destructor **plus** `SSM_RecycleHandle` -> `shm_unlink` and
`MemBroker_FreeMemory`. Under `kill(pid, 9)` none of it runs.

### Why that is the leading candidate for the wedge

The resources the skipped cleanup owns are exactly the finite, cross-process
kind that a power cycle is the only other way to reclaim:

- `libsyncsharedmemory.so.0` carries `[SSM] over %d handles, name: %s` and
  `[SSM] cannot allocate buffer over %d` - hard caps, whose recycler
  (`SSM_RecycleHandle`) is what SIGKILL skips. Its segments are POSIX
  `shm_open` objects, which survive SIGKILL until an `shm_unlink` that never
  comes.
- `/tmp` is a **32 MB** tmpfs (`mount -o remount,rw,size=32768k /tmp` in
  `etc/rc.local`).
- `MemBroker_*` allocations come from the DRAM carve-out via `/dev/vpl_edmc`.

`EncodingGroup::Start`'s failure path calls `exit()`, which is precisely the
"nothing is left to ACK" shape of both wedge signatures.

### Two drivers checked and cleared as the primary suspect

- **`vpl_edmc.ko` frees properly.** Its `Close` (0x7b8) calls
  `DeleteBlkInfoNode` **and** `DeleteSharedBlkInfoNode` on the dying file, then
  `kfree`s the private struct - and it even handles death-while-locked, by
  `up()`ing the allocator semaphore when `private[0] == 1` before freeing. Both
  the "Regular:" and "Shared:" lists are per-file. So media memory is not leaked
  by SIGKILL at the driver layer.
- **`vpl_dmac.ko` has a real hazard, but not this one.** Its `Close` (0xb8)
  busy-waits for the profile's completion word - `ldr` / `cmp #1` / `bne` at
  0xec, **no timeout, no `schedule()`, no signal check** - before releasing the
  profile (64 of them, state array at MMIO +0x208, freed flag at +0x308; the
  ISR sets state = 1 on completion). A process killed with a profile that never
  completes can never finish dying. Our one transfer does complete, so this is
  not what we are hitting, but it is worth remembering the next time a process
  on the card appears unkillable.

### Hardware control: `vic_fast_kill=0` is neutral on capture

    sudo POLLDRAIN=20 EXTRA="post_proc=1 post_mask=0 fake_frame_off=1 \
        vic_fast_kill=0" scripts/mz0380-m55-real-capture.sh 1

    stream start: SET_VIC(... fk=0 ...) ret=0
    poll-drain: buf 0 holds 3110400 bytes; delivering
    captured 3110400 bytes = 1 whole frames + 0 bytes

    VERDICT: NOT SPLASH   Y 212 distinct values, UV 221
    VERDICT: CHROMA OK    luma explains A 0.01 / B 0.00 of chroma

A complete, real, correctly-coloured frame - indistinguishable from the fk=1
baseline. The card accepts byte 33 = 0 and the SET_VIC still returns 0. The
card's teardown wait is invisible from the host because it hides inside the
SET_VIC settle we already pay (SET_VIC 12542.166 -> START 12544.375, 2.21 s,
against M154's 2.18 s at fk=1).

Also note the packing was already right: `params[7] = (fast_kill & 0xff) << 8`,
and `params[k]` covers payload bytes `4+4k .. 7+4k`, so that is byte 33.

**The default is now 0** (`mz0380-core.c`). This is the one field where we
deliberately do not match Windows, which sends `fk=1` in every `[CH00]` line of
every trace (M82). The justification: byte 33 governs only the disposal of the
*previous* encoder, M156 already showed our STOP is not what kills tinyvenc5,
and the frame is byte-for-byte the same shape either way. Retail opens one
stream and keeps it; our workflow spends one spawn per capture, so we are the
only party for whom the budget is a live constraint.

### What this does NOT do, and how it can be judged

It does not touch the one-frame bound - that is M153/M155, a different defect.

And it is **not yet proven to move the wedge**. Proving it directly costs
reaching the wedge twice, ~30 spawns, which is more than this question is worth
paying up front. The cheap alternative is to leave the default at 0 and let
every ordinary run accumulate evidence for free: if this session and the next
pass 18 spawns on one power cycle without a wedge, that is the answer. The
per-run marker is `fk=0` in the SET_VIC banner, so every dmesg records which
regime it belonged to.

The card is at roughly **1 spawn** into this power cycle (fresh boot), so the
counter starts clean here.

### If it wedges anyway: make the recovery cheap

`mz0380-m52-card-recovery.sh` walks pm-reset -> secondary-bus reset ->
remove+rescan, testing the mailbox after each. It has **never been run**, and
its `mailbox_alive` markers are stale - they grep for `BEGIN_FW_DL` and firmware
upload text from the era before that path was deleted. Refresh it to the current
`CMD_INIT answered on attempt 1` / `card handshake failed` markers before use.
If the slot's PERST# reaches the SoC's reset tree, the bus-reset stage reboots
the card's Linux and a wedge becomes a five-second recovery instead of a
mains-off cold boot. Worth exactly one run, while actually wedged.


---

## M158 (static, 2026-08-24): the one-frame bound is a LIVELOCK on an unreleased DMAC profile - and `fw` selects which encoder binary the card spawns

Two findings, and the second is a live experiment. Both came out of reading
`vpl_dmac.ko`, which M153 named as the missing piece and then could not get:
*"That last step is inference about `TK_MMA_*` semantics, not proof - the
libraries would have to be read to confirm it."* They have now been read, and
the driver is a better authority than the libraries anyway.

### 1. The proof M153 was missing

Four facts, each mechanical:

**a. The start ioctl fails hard when the profile is busy.** `vpl_dmac`'s
`Ioctl` case `0xDE00` (0x5c8):

```
5dc  p = VPL_DMAC_StartHead(base)      ; = devinfo[0x140], the caller's profile
5ec  r3 = free[0x308 + p*4]
5f0  cmp r3, #0
5f4  bne 0x7bc                         ; free -> claim it
5f8  return -1                         ; BUSY -> hard fail
...
7c0  free[0x308 + p*4] = 0             ; claimed
7c4  file->private->profile = p
7d0  VPL_DMAC_StartTail(...)           ; kick the hardware
```

**b. `free[p]` is set back to 1 in exactly two places.** The wait ioctl
`0xDE01` (0x630), which also clears `state[0x208 + p*4]` and forgets the
profile:

```
6f4  state[0x208 + p*4] = 0
6f8  free[0x308 + p*4]  = 1
6fc  file->private->profile = -1
```

and `Close` (0xb8), i.e. **process death** - which first busy-waits for
`state[p] == 1` with no timeout, no `schedule()` and no signal check (0xec).

**c. The wait ioctl is what `TK_MMA_WaitOneFrameComplete` issues.**
`MassMemAccess_WaitOneFrameComplete` -> `MassMemAccess_WaitDMAC` ->
`ioctl(fd, 0xDE01)` (libmassmemaccess.so.9 0x1ee4, constant at 0x1f08).

**d. The start does not fail gracefully - it spins.**
`MassMemAccess_StartDMAC` (0x1df0):

```
1df0  bl  sched_yield
1df4  r0 = fd
1df8  r1 = 0xDE00
1dfc  bl  ioctl
1e00  cmp r0, #0
1e04  bne 0x1df0          ; <<< retry forever, yielding
1e08  return
```

M153 proved `TK_MMA_WaitOneFrameComplete` is unreachable in tinyvenc5 (the
`mma_already_start` guard is permanently 0). So the profile claimed by the
first push is **never** returned inside that process, and therefore:

**A second `TK_MMA_StartOneFrame` in one tinyvenc5 process can never return.
It livelocks in `sched_yield`.**

That is stronger than M153's guess that the second start "fails, takes the
error path at 0x1513c, and loops silently". It never reaches an error path at
all. One frame per tinyvenc5 process is structural, and `Close` releasing the
profile on process death is exactly why the respawn is the only thing that ever
produced another frame (M154, M155).

**It also answers M156's parting question.** M156 ended: *"That thread parking
should not stop the main loop answering the mailbox, yet `0x2d` returns -110 on
every cycle after the first."* It is not parked. It is spinning. tinyvenc5's
`main` calls `sched_setscheduler(0, SCHED_FIFO, ...)` at 0xe40c, and a sibling
thread spinning at real-time priority is a mechanism for the command loop going
unserved that requires nothing else to be true. (The thread's inherited
priority is inference, not read - `pthread_create`'s attr was not checked.)

### 2. `fw` is not "the encoder selector" - it picks the BINARY

`video_capture_mgr` at 0x9244, on the SET_VIC per-channel struct
(base 0x141d4, **stride 60**, and struct offset == payload byte offset):

```
9248  r3 = ch*15
924c  r3 = base + ch*60
9250  r3 = struct[ch].byte[6]        ; fw
9254  cmp r3, #7
9258  beq 0x98f8                     ; -> "call ----> tinyvenc7" -> ./tinyvenc7
                                     ; else -> "call ----> tinyvenc5" -> ./tinyvenc5
```

(Bytes 2/3 and 6 take further paths at 0x9a24 and 0x9184; the 5-versus-7 split
is the one that matters.) The same read incidentally **confirms M157's byte-33
claim independently**: the SET_VIC handler stores payload byte 0x21 to struct
offset 0x21 at 0x91a4.

**So M88's "the regression is ONE BYTE - SET_VIC byte 6" was right about the
byte and blind about the meaning.** `fw=7` did not select a different encoder
mode; it ran a different program. And `fw=5` is not a magic value - it is
"anything that is not 7".

### 3. Why that matters now: tinyvenc7 does not have the bug

The three encoders import identical `TK_MMA_*` symbols, but they are different
architectures. tinyvenc5 is `img_handler` -> SSM ring -> `encode_handler` ->
MMA. **tinyvenc7 has no SSM ring on the frame path at all** - its MMA calls
live in `vcap_handler(const video_cap_state*, void*)`, i.e. straight off the
video-capture callback.

And it drives the latch correctly. tinyvenc7 `vcap_handler` at 0x125b8:

```
125b8  bl  TK_MMA_StartOneFrame
125c0  subs r1, r0, #0
125c4  bne 0x12e1c                   ; start failed -> error path
125c8  r3 = 1
125cc  strb r3, [r8, #-0xef8]        ; *** mma_already_start = 1 ***
125d0  r3 = [r5, #-0xf3d]            ; sync-mode byte
125d8  cmp r3, #1
125dc  bne 0x125f0                   ; async -> skip the wait
125e4  bl  TK_MMA_WaitOneFrameComplete
125ec  strb r3, [r7, #-0xef8]        ; *** mma_already_start = 0 ***
```

That is what the variable is *for*: an in-flight latch, set after a successful
start and cleared after the wait. **tinyvenc5 reads that latch nine times and
never writes it** (M153: nine `ldrb`, zero stores). tinyvenc5 is not
mis-configured - the shipped build has a genuine defect, and tinyvenc7 is the
same vendor's correct version of the same idiom.

### The experiment

`fw=7` is a one-word change (`VICFW=7`), it is the first candidate this project
has had for more than one frame per stream, and the only evidence against it is
**M88, which was measured in the regime M129 invalidated** - splash oracle,
`post_mask=0x1f` truncating the DMA to one 16-byte burst, `fake_frame_off`
unset. Every negative from that era has to be re-read, and this is one.

    sudo POLLDRAIN=20 VICFW=7 \
        EXTRA="post_proc=1 post_mask=0 fake_frame_off=1" \
        scripts/mz0380-m55-real-capture.sh 4

Ask for 4 frames, because the whole point is whether more than one arrives.
**Frame count is the oracle** (M153), so read `poll-drain stopped after N
deliveries`. N > 1 settles it. N == 1 with a real frame means tinyvenc7 runs on
this board but hits its own bound. N == 0 means tinyvenc7 does not drive this
board's VIC, which is a clean negative and costs one spawn.

Risk is one spawn. vcm kills tinyvenc5/7/8 at startup, so the binaries do not
collide.


---

## M159 (hardware, 2026-08-24): `fw=7` BREAKS THE ONE-FRAME BOUND - continuous ~61 Hz production, and the frame-token register moves for the first time

    sudo POLLDRAIN=20 VICFW=7 EXTRA="post_proc=1 post_mask=0 fake_frame_off=1" \
        scripts/mz0380-m55-real-capture.sh 4

    stream start: SET_VIC(... fw=7 ... bitstreams=1) ret=0

    14998.536  frame token 0 ... payload counters a5a5a5a0/ffffffff/a5a5a5a5
    14998.556  frame token 1 ... payload counters a5a5a5a1/ffffffff/a5a5a5a5
    14998.570  frame token 2 ... payload counters a5a5a5a2/ffffffff/a5a5a5a5
    14998.587  frame token 3 ... payload counters a5a5a5a3/ffffffff/a5a5a5a5
    14998.602  frame token 0 ...            (and so on, 14 events in 213 ms)

**This is a cadence.** Fourteen events in 213 ms, mean gap 16.4 ms = **61 Hz**,
cycling all four buffers in order, and - decisively - **re-writing each buffer
after the driver re-poisons it**. The card is producing continuously.

Under `fw=5` the same recipe produces exactly one write and then nothing, ever,
until the process is replaced. **M158's prediction is confirmed: the one-frame
bound is a property of the tinyvenc5 build, not of the board, not of the VIC,
and not of anything host-side.**

### The frame-token register moved - a first

Every run in this file's history has ended `producer watch saw 0 change(s),
final 40=a5a5a5a5`, and M151 concluded the sentinel was blind because its
writer (`store_channel_done`) is unreachable. Under `fw=7` the sentinel
**moves**: the token register reads `a5a5a5a0`, `a5a5a5a1`, `a5a5a5a2`,
`a5a5a5a3` on successive events - our seed `a5a5a5a5` with the low bits
replaced by the buffer index - and `payload[1]` reads `ffffffff` where it was
seeded `a5a5a5a5`. The driver's `idx = token & 7` picks 0,1,2,3 from exactly
those values.

So the reporting path tinyvenc5 has compiled out **is present and running in
tinyvenc7**. M151's "the sentinel is blind" was true of tinyvenc5 and is not a
property of the hardware.

### The new defect: 16 bytes per event, and it is real video

Each event carries **16 bytes**, not 3110400. But the bytes are real:

    2c 2c 2c 2c 2c 2c 2c 2c 2c 2c 2c 2c 2c 2c 2c 2c
    2d 2d 2d 2d 2c 2c 2c 2c 2c 2c 2d 2d 2d 2c 2c 2c

Dark luma with real variation - not poison (`aa`), not the sentinel (`a5`), not
zeros. Four events reached v4l2 (64 bytes total); the rest were dropped with
`no queued vb2 buffer` because 16-byte "frames" exhaust the queue immediately.

### A trap: do NOT read the 16 as tinyvenc5's literal

tinyvenc5's only reachable MMA push passes a constant:

    142fc  mov r1, #0x90000000
    14300  mov r2, r5             ; phys addr
    14304  ldr r0, [sp, #0x60]    ; handle
    14308  mov r3, #16            ; <-- literal
    1430c  bl  TK_MMA_StartOneFrame

and tinyvenc7's passes a variable (`r3 = r9`, 0x125b8). That looks like the
answer and **is not**. Through the wrapper
(`TK_MMA_StartOneFrame` 0xc64: `ctx[0x18] = a3`, then
`MassMemAccess_StartOneFrame(ctx[0], ctx+4)`), that argument lands at
StartDMAC's `[base+0x14]`, which is `orr`'d into the DMAC **control word** at
`<< 10` (0x1d5c/0x1d6c) and written to the mapped MMR at `[block+8]`. It is a
control field, not a byte count. The two 16s are not known to be the same
thing. **Unresolved - do not build on it.**

### What was NOT captured, and why

m55's section-3 grep showed the token flood and **nothing after it**: no
`poll-drain armed`, no `poll-drain stopped after N deliveries`, and **no
`stream stop:` summary**. So `frame_events`, `irq_total` and the final token
values are unknown for this run, and *which path delivered* - the real
frame-event path or the poll-drain - is unknown with it. That distinction
matters: real completion events would be another first.

The flood pushed them out. The numbers are still in the kernel ring buffer and
cost nothing to recover:

```bash
sudo dmesg | grep -aE "poll-drain|stream stop|producer watch"
```

**m55 needs a fix**: its section-3 grep has to keep the summary lines when a
run produces hundreds of per-frame lines. Until then, run the command above
after every `fw=7` capture.

### The next question, and it is one spawn

Is 16 bytes what the card transfers, or what our re-poison leaves behind? The
poll-drain re-poisons the whole buffer after every delivery *and* after every
drop, so a card writing progressively would be reset every 20 ms and would
always present a short prefix. Change **one variable** - give the card 2 s per
look instead of 20 ms:

    sudo POLLDRAIN=2000 VICFW=7 EXTRA="post_proc=1 post_mask=0 fake_frame_off=1" \
        scripts/mz0380-m55-real-capture.sh 4
    sudo dmesg | grep -aE "poll-drain|stream stop|producer watch"

A length that grows means we were truncating it ourselves and the fix is
host-side. A length still exactly 16 means the transfer really is one burst,
and the target moves to the DMAC control word.


---

## M160 (hardware + fix, 2026-08-24): the completion events are REAL, the poll-drain never fired, and the 16 bytes are OUR bug

The recovered summary from M159's run, and the `POLLDRAIN=2000` control:

    (M159, POLLDRAIN=20)
    poll-drain stopped after 0 deliveries, 0 kicks; producer watch saw 0 change(s)
    stream stop: ... irq_total=41 frame_events=34 fifo_drops=0

    (control, POLLDRAIN=2000)
    poll-drain stopped after 0 deliveries, 0 kicks
    stream stop: ... irq_total=30 frame_events=23 fifo_drops=0
    26 'frame token' lines, every one length=16

### `frame_events` is not zero any more

**`frame_events=34` and `frame_events=23`.** That field has been **0 in every
run in this file's history.** The completion path - the interrupt the card is
supposed to raise when a frame lands - has never once fired under `fw=5`, and
under `fw=7` it fires 34 times in a 700 ms stream, with `fifo_drops=0`.

And `poll-drain stopped after **0 deliveries**` in both runs. So every one of
those deliveries came through the **real completion-event path**. Under `fw=5`
it is the exact opposite: `frame_events=0`, and the poll-drain finds the frame
by scanning. The two builds do not merely differ in cadence - they deliver
through different mechanisms, and only tinyvenc7 uses the one the hardware was
designed around.

### The `POLLDRAIN=2000` control was answered, but not by the mechanism intended

The design was "give the card 2 s per look". It changed nothing - still exactly
16 bytes - but **not because 16 is the transfer size**. `POLLDRAIN` only paces
the *poll* fallback, which did 0 deliveries in both runs. The drain fires on the
event, so the poll interval was never in the path being measured. The control
was inert by construction. Recording that as a design error: *check which code
path your variable is actually in before spending a spawn on it.*

### What is really truncating the frame - and it is ours

`mz0380_drain_frame_snapshot()` infers the length from the poison boundary,
delivers whatever it finds, and then **re-poisons the buffer** - unconditionally,
through `goto repoison`.

M115 already knew this was wrong:

> *only deliver a COMPLETE frame. The poison boundary marks how far the DMA has
> got, not that it has finished, so polling a burst in flight yields a torn
> prefix - the first run delivered 794368 bytes and then the real 3110400*

...and added the `want = width*height*3/2` guard **to the poll-drain only**.
The event path never got it, and nobody noticed, because with `frame_events=0`
**that path had never executed against a live producer.** It was untested code
that looked tested.

So on every one of tinyvenc7's ~60 Hz completion events the driver read the
buffer while the DMA was still filling it, delivered the 16 bytes that had
landed, and then **re-poisoned the buffer underneath the transfer** - destroying
the rest of the frame and guaranteeing the next event would find a short prefix
too. A self-sustaining truncation, entirely host-side.

That the length was *always exactly 16* is consistent: the event fires early in
the transfer, and re-poisoning resets the boundary every time, so the driver
samples the same early point of every frame.

### The fix

`mz0380_drain_frame_snapshot()` now applies the M115 rule. A short frame is
**left completely alone** - not delivered, and crucially **not re-poisoned**
(`return`, not `goto repoison`) - so the DMA finishes and a later event or the
poll-drain picks it up whole.

`event_require_complete=0` restores the old behaviour, which is the only way to
see the torn prefixes again if it turns out the card really does send 16 bytes.

### The run

    sudo POLLDRAIN=20 VICFW=7 EXTRA="post_proc=1 post_mask=0 fake_frame_off=1" \
        scripts/mz0380-m55-real-capture.sh 4
    sudo dmesg | grep -aE "poll-drain|stream stop|producer watch|in flight"

- **Whole frames delivered, more than one** - that is streaming video, and the
  remaining work is quality, not cadence.
- **`DMA still in flight` lines and then nothing** - the card starts a frame per
  event but never finishes it. That is a real card-side truncation and the
  target moves to the DMAC descriptor, with the `<< 10` control field the first
  place to look.
- **Whole frames but still only one** - the completeness rule is right and
  something else bounds the cadence.

Do not judge this run on the captured byte count alone: with 4 requested frames
and a 60 Hz producer, `frame_events` and the delivered lengths in dmesg are the
result.


---

## M161 (hardware, 2026-08-24): the 16 bytes are the CARD's, not ours - and the producer is rock solid at 1621 completion interrupts

M160's fix went in and the answer is its second branch. Over a ~57 s stream:

    stream stop: EVENT[0x30]=00000000 token[0x40]=a5a5a5a0 0x44=a5a5a5a0
                 0x48=ffffffff 0x4c=a5a5a5a5 enc[0x50]=00000000
                 irq_total=1628 frame_events=1621 fifo_drops=0

    poll-drain stopped after 0 deliveries; producer watch saw 1558 change(s)

    $ grep -aoE "holds [0-9]+ of" m161-dmesg.log | sort -u
    holds 16 of

**`frame_events=1621`, `fifo_drops=0`, 1558 token transitions.** The card raises
a real completion interrupt per frame, continuously, for a minute, cycling
`0->1->2->3->0`. Under `fw=5` this number has been **0 in every run ever
recorded**.

**And "16" is the only length that ever appears.** This run left every short
buffer **un-poisoned for the entire stream** - the M160 fix means nothing
overwrote them - and no buffer ever grew past 16 bytes. So the truncation is not
our re-poison racing the DMA. The card transfers 16 bytes per frame and stops.

M160 was still a real bug (the event path genuinely lacked M115's guard, and it
had never executed against a live producer), but it was not this bug. Both
paths now agree, which is what makes the measurement trustworthy:

    poll-drain: buf 0 holds 16 of 3110400 bytes - DMA still in flight, waiting
    frame token 0 holds 16 of 3110400 bytes on the completion event - ...

### The lead: tinyvenc5 hardcodes what tinyvenc7 computes

`TK_MMA_StartOneFrame(ctx, 0x90000000, phys, a3)`. In tinyvenc5 (0x14308):

    14308  mov r3, #16                  ; literal

In tinyvenc7's `vcap_handler` (0x12518):

    12518  ldrh r9, [r4, #86]           ; a geometry field at +0x56
    12528  add  r9, r9, r9, lsl #1      ; * 3
    1252c  asr  r9, r9, #1              ; / 2     -> value * 3/2

`x * 3 / 2` is the 4:2:0 size formula. tinyvenc7 **derives** that argument from
frame geometry; tinyvenc5 pins it to a constant. This is the same shape as the
`mma_already_start` defect (M158): the correct idiom in one build, stubbed in
the other.

**What is NOT established:** the units of `a3`, and whether it is the transfer
length at all. Through the wrapper it reaches `MassMemAccess_StartDMAC`'s
`[base+0x14]` and is `orr`'d into the DMAC control word at `<< 10` (0x1d6c),
into a field the preceding `bic #0xFF0` only clears to bit 11 - so a value of 16
would land at bit 14, outside the cleared field. That does not read like an
integer length, and the descriptor's size fields come from
`MassMemAccess_Initial`/`SetOptions` instead (the TK wrapper's `Init` passes a
hardcoded 2560 and 1920 - stride and width - at 0x9c4/0x9c8). **Do not build on
"a3 is the length" until the descriptor layout is read.** Flagged, not resolved.

### Method note, and it is mine

The `POLLDRAIN=2000` run (M160) was designed to test whether our re-poison was
truncating an in-flight frame. It could not have: `POLLDRAIN` paces only the
poll fallback, and the drain fires on the completion event. **The variable was
not in the code path under measurement.** One spawn, no information. The rule
that would have caught it costs nothing: before spending a spawn, name the
function the variable is read in.

### State

Four spawns into this power cycle, `vic_fast_kill=0` throughout, no wedge, and
the card sustained a 57-second stream with 1628 interrupts and zero FIFO drops.


---

## M162 (static, 2026-08-24): the MMA "length" argument is not a length, and neither binary ever asks for a frame-sized transfer

Chasing M161's lead. It does not survive, and what replaces it is more useful.

### `a3` is a control-word field, proven three ways

`TK_MMA_StartOneFrame(ctx, 0x90000000, phys, a3)` - M161 flagged `a3` as
"probably the length, units unresolved". It is not a length:

1. **Where it goes.** The wrapper (0xc64) stores it at `ctx[0x18]`, which is
   `MassMemAccess_StartDMAC`'s `[base+0x14]`, which is `orr`'d into the DMAC
   control word at `<< 10` (0x1d6c/0x1e40) and written to the mapped MMR at
   `[block+8]`. The preceding `bic #0xFF0` only clears to bit 11, so a value of
   16 lands at bit 14 - incoherent as an integer field, ordinary as a flag.
2. **Who else writes that slot.** `MassMemAccess_SetOptions` option **78**
   writes `handle[0x14]` (0x10ec) - the same slot, through a config call.
3. **The measurement.** tinyvenc7's `vcap_handler` pushes with `a3 = 16` at
   0x129a4 and `a3 = height*3/2` at 0x125b4. **Both deliver exactly 16 bytes.**

### No call site in either binary asks for a frame

| | `a3` values | `MemBroker_CacheCopyBack` size |
|---|---|---|
| tinyvenc5, 12 aperture sites | 4096 x4, 16 x2, r9 x1 | 4096 |
| tinyvenc7, 9 aperture sites | 16, height*3/2 | 16 (Start), 4096 (Process) |

There is **no `3110400` constant anywhere in tinyvenc5**, and no literal-pool
`0x90000000` either - every reference is the `mov #imm` form, all twelve of them
MMA calls. Only `tinyvenc5` and `tinyvenc7` reference the outbound aperture at
all; `libvideocap`, `libtk_video_capture`, `libtkmf_video_source`, the IBPE
library, `libmemmgr` and both MMA libraries reference it **zero** times. So the
push to the host lives in the tinyvenc binaries and nowhere else.

### tinyvenc7 uses two handles, and both are small

In `vcap_handler`:

    [r4+0xb4]  -> 3x TK_MMA_StartOneFrame,   CacheCopyBack(buf, 16)
    [r4+0xb8]  -> 2x TK_MMA_ProcessOneFrame, CacheCopyBack(buf, 4096)

`TK_MMA_ProcessOneFrame` is the API M153 proved unreachable in tinyvenc5; in
tinyvenc7 it is on the frame path. But it moves 4096 bytes, not 3110400.

### The chain that pins the transfer length

The cache flush is *maintenance*, not a length - a DMA can move more than was
flushed. But that asymmetry is exactly what makes it decisive here: if the
engine moved a frame while only 16 bytes were flushed back, the host would
receive **3110400 bytes of mostly-stale DRAM**, not 16 bytes. M161 measured
exactly 16 bytes arriving, with the rest of the buffer still poisoned, over a
57-second stream with the buffers left un-poisoned. **So the configured transfer
length really is 16 bytes**, and it is configured, not passed per-call.

### Where the geometry comes from - and the next read

Both binaries configure MMA with **option 80 only** (`mov #80` before every
`TK_MMA_SetOptions` in both; option 80 writes four flag bytes to
`handle[0x60..0x63]`, and `MassMemAccess_WaitDMAC` clears `[0x63]` - another
in-flight latch, in the library this time). **Neither ever calls option 23**,
the branch that does the `MemMgr_GetPhysAddr` and stride arithmetic. So the
transfer geometry is established in `TK_MMA_Init` / `MassMemAccess_Initial`.

`TK_MMA_Init(a, b, c, d)` -> `ctx[0x14]=a`, `ctx[0x10]=b`, `ctx[0x30]=c`,
`ctx[0x0c]=(d!=0)`, and `MassMemAccess_Initial(ctx, {name, 2560, 1920, 0,0,0})`
- 2560x1920 being a max-allocation bound, not the transfer size. The one
structural difference found so far in the init arguments:

    tinyvenc5 (0x12b7c):  a = r6,  b = 2,   c = 32,  d = 1
    tinyvenc7 (0xdbec):   a = r4,  b = r4,  c = 32,  d = 1

**Next read (static, free): map the rest of `TK_MMA_Init` and
`MassMemAccess_Initial` onto the 60-byte descriptor** - `StartDMAC` writes
`[block+0x14/0x18/0x1c/0x20/0x24/0x28/0x2c/0x34/0x38]` from descriptor fields
`[0x5c/0x28/0x30/0x34/0x38/0x3c/0x40/0x44/0x60]`, and one of those is the byte
count. Then compare what each binary puts there.

### The CFG could not help here, and that is worth knowing

`mz0380-cfg.py` on `vcap_handler` (0x122d4..0x13260, 966 instructions) reports
**everything reachable**, including both `ProcessOneFrame` sites. That is not a
result - the tool over-approximates, and it only produced M153's answer because
`mma_already_start` was provably constant and let two edges be cut. tinyvenc7
has no such constant. Reachability questions here need a proven-constant flag;
without one the tool says nothing.


---

## M163 (static, 2026-08-24): CORRECTION - M162's central claim is WRONG. `a3` IS the length, tinyvenc7 HAS a full-frame push, and three gates divert it to a 16-byte one

### The error

M162 said "`a3` is a control-word field, proven three ways". It is not, and the
proof was built on one bad identification:

**`MassMemAccess_StartDMAC`'s first argument is the HANDLE, not the per-call
descriptor.** `MassMemAccess_StartOneFrame(r0=handle, r1=desc)` keeps `r5=handle`
and `r4=desc`, **copies descriptor fields into the handle** (0x1a74-0x1a88), and
then calls `StartDMAC(r0 = r5)` - the handle. I read `r4` inside StartDMAC as the
descriptor, so the field I traced into the control word at `<< 10` was
`handle[0x14]`, which `TK_MMA_Init` sets to a constant **2** via option 78. Not
`a3`, and not variable at all.

The third "proof" was worse: I observed two sites passing different `a3` values
and asserted both delivered 16 bytes. **Nothing established that both sites
execute.** That was an assumption presented as a measurement.

### What is actually true

`a3` travels `desc[0x14]` -> `handle[0x28]` -> **`MMR[0x18]`**, and the
neighbouring writes settle what that register is:

    MMR[0x10] = phys(handle[0x58])  = a2            SOURCE
    MMR[0x14] = handle[0x5c]        = a1 0x90000000 DEST
    MMR[0x18] = handle[0x28]        = a3            LENGTH
    MMR[0x1c] = handle[0x30]
    MMR[0x24] = handle[0x38]

Source, destination, length, in that order. **`a3` is the transfer size.**

And the arithmetic closes exactly. At tinyvenc7 0x12518, `a3 = [r4+0x56] * 3/2`
- for height 1080 that is **1620**, and a 1080p I420 frame is 1080 luma lines
plus 540 lines' worth of chroma = **1620 lines x 1920 bytes = 3110400**, the
precise frame size this project has been receiving since M129. That is not a
coincidence, and it means **tinyvenc7 contains a full-frame push.**

### The three gates

The full-frame push at 0x125b8 is reached only if all three hold (0x124e0):

```
124e0  cmp  r0, #1
124fc  beq  0x12944        ; r0 == 1        -> 16-byte push
12500  cmp  r10, #1
12504  bne  0x12944        ; r10 != 1       -> 16-byte push
12508  ldrb r1, [r7, #-0xf10]
1250c  cmp  r1, r8         ; r8 = 0
12510  beq  0x12944        ; global == 0    -> 16-byte push
12514  ...                 ; full frame, a3 = height*3/2
```

`0x12944` leads to the `a3 = 16` push at 0x129b0 - the one we are getting. So
the card is **choosing** the 16-byte path, every frame, and at least one of
these three conditions is against us.

### Why this is the best lead the project has had

`video_capture_mgr` spawns tinyvenc7 as

    ./tinyvenc7 -D [-L] -a %d -b %d -c %d ... -v %d %d %d %d -w %d

- **26 numeric parameters, built from the per-channel SET_VIC struct.** The
globals in this cluster (`[r7-0xf10]` here, `[r5-0xf3d]` the sync-mode byte at
0x125d0, `[r7-0xef8]` = `mma_already_start`) are the shape of argv-parsed config
flags. If any of the three gates maps to one of those options, **it is
host-settable through SET_VIC**, and this becomes a knob rather than a defect.

### Next, and it is still static

1. Identify `r0`, `r10` and the global at `[r7-0xf10]` by tracing back through
   `vcap_handler` from 0x124e0.
2. Read tinyvenc7's `getopt` loop and map each option letter to its global.
3. Cross it against vcm's `sprintf` of the tinyvenc7 command line (0xbb00 /
   0xbba8) to see which SET_VIC field feeds it.

### Method note

Three revisions on `a3` in two milestones. The pattern that produced the error
is the same one M153 recorded for reachability: **a structural claim about which
argument is which, made by reading one function in isolation.** StartDMAC alone
cannot tell you what its parameter is; only its caller can. The check that would
have caught it is mechanical - before asserting what a field is, read the call
site that supplies it.


---

## M164 (static, 2026-08-24): the 16 bytes are a FRAME-INTERVAL SELECTOR - tinyvenc7 sends a small block every frame and a full frame every Nth, and N makes the full one unreachable

All three gates from M163 are now resolved. Two pass. The third explains
everything.

### Gate 1 - passes, always

    124cc  ldrb r0, [r5, #-0xf3f]     ; global 0x4c0e9
    124e0  cmp  r0, #1
    124fc  beq  0x12944               ; == 1 diverts

The **only** writer in the whole binary is `main` at 0xdf60, `strb r4, ...`
with `r4 = 0` (0xdf44). It is 0 for the process lifetime, so this gate is never
the diverter.

### Gate 3 - passes, always

    12508  ldrb r1, [r7, #-0xf10]     ; global 0x4c118
    1250c  cmp  r1, r8                ; r8 = 0
    12510  beq  0x12944               ; == 0 diverts

`main` initialises it to **1** (0xe430, `strb r2, [r11, #-0xf10]` with `r2=1`),
and the only other writer is the **`-A`** option handler (0x100f4), which sets
it to 0 after printing a banner. `video_capture_mgr` spawns tinyvenc7 as
`./tinyvenc7 -D [-L] -a .. -w ..` and **never passes `-A`**, so this gate is 1
and passes.

### Gate 2 - THIS is the one, and it is a modulo

    124b0  ldrb r1, [r5, #-0xf42]     ; N, global 0x4c1e6
    124b8  cmp  r1, #0
    124bc  beq  0x129d0               ; N == 0 -> a different path entirely
    124c0  ldr  r0, [r4, #0x7c]       ; frame counter, in the config struct
    124c4  bl   __aeabi_uidivmod      ; r1 = counter % N
    124c8  mov  r10, r1
    12500  cmp  r10, #1
    12504  bne  0x12944               ; remainder != 1 -> 16-byte push

**`(frame_counter % N) == 1`.** So tinyvenc7's design is not broken and not a
truncation: it pushes a **small block every frame and a full frame every Nth**
- a preview/keyframe cadence. We are getting the small block 100% of the time
because the full-frame case never comes up:

- **`N == 1`** makes `counter % 1` identically 0, so the remainder can never be
  1 and the full push is **unreachable for the life of the process**.
- **`N == 0`** diverts at 0x124b8 before the modulo is even reached.

Either value produces exactly what M161 measured: a completion interrupt per
frame, 16 bytes every time, forever, with the producer otherwise healthy.

### Where N comes from - and why it is not an argv option

`N` at `0x4c1e6` is written once, in `main` at 0xeb70, and its source is
`[r8-0xe06]` = **0x4c222**, which is **read twice and never written anywhere in
the binary**. So it arrives as part of a bulk-loaded config struct, not from
`getopt`. The second read confirms the shape - it is a per-channel value with a
global fallback:

    f02c  ldrb   r1, [r8, #-0xe06]    ; per-channel N   (0x4c222)
    f030  cmp    r1, #0
    f034  ldrbeq r1, [r8, #-0xf43]    ; fall back to the global default
    f038  strb   r1, [r2, #0xe]

and the neighbouring field `[r8-0xe05]` gets the identical treatment into
`[r2, #0xf]` - adjacent bytes, i.e. a per-channel array.

**This matters because the cfg is host-reachable.** M126/M128 established that
`video_capture_mgr` (0xa290) `atoi()`s every line of
`/tmp/nullsensor_yuan%d.cfg` and rewrites values from the SET_VIC payload. If
the key backing 0x4c222 is in that cfg, **N is host-settable and this becomes a
knob.**

### Why this is the whole game

At N = 2 the card would send a full 1080p frame every other frame - **30 fps at
1920x1080**, on a producer already proven to sustain 1621 completion interrupts
with zero FIFO drops (M161). Nothing else about the pipeline needs to change.

### Next, still static

1. Find which cfg key populates 0x4c222 - read tinyvenc7's cfg parser (it opens
   `/tmp/nullsensor_yuan%d.cfg`) and map key strings to the 0x4c1xx/0x4c2xx
   cluster.
2. Cross that key against vcm's patcher (0xa290) to see whether it is one of the
   lines vcm rewrites, and from which SET_VIC field.
3. Only then spend a spawn.

### Method note

This chain held because every hop was resolved to an absolute address and then
grepped for **all** writers, rather than reading the first plausible one. Gate 3
looked like the obvious suspect (a flag that defaults to "off" would have
explained everything) and it is the opposite - it defaults to 1 and only an
option nobody passes can clear it. Two gates were eliminated by writer-census,
not by argument.


---

## M165 (static, 2026-08-24, **superseded by M204**): the frame-interval knob is opcode 0x32, and **ep.ko does not forward it** - the fw=7 full-frame path is not host-reachable

**Correction:** M165's direct-writer census missed calls to
`tiny_calculate_skip_fps()` and `tiny_calculate_avg_fps()`. Those helpers write
the schedule bitmaps indirectly from the already-forwarded opcodes `0x31` and
`0x2d`; opcode `0x32` is not required for the all-frame H.264 schedule. Keep
the material below as the historical chain, not as the current conclusion.

M164's chain completes, and it ends in a wall. Recording it fully, because the
wall is the result.

### The config cluster is the MAILBOX PAYLOAD BUFFER

`N` (0x4c222) and `M` (0x4c221) looked like cfg values because nothing in
tinyvenc7 writes them with a direct offset. They are not. `main` at 0xe014:

    e014  ldr  r1, =0x4c218
    e018  mov  r2, #44            ; exactly the max mailbox payload (M127)
    e01c  bl   pread              ; pread(fd, 0x4c218, 44, ...)
    e028  ldr  r1, [r8, #-0xe10]  ; = [0x4c218] = the OPCODE
    e02c  sub  r3, r1, #6
    e034  ldrls pc, [pc, r3, lsl #2]   ; jump table, opcodes 6..98

So **0x4c218 is the command buffer**, and the "config bytes" are payload
offsets: `M` = byte **9**, `N` (ch0) = byte **10**, ch1 = byte **11**.

### The command is 0x32

Walking the jump table, the handler containing 0xeb28-0xeb70 - the code that
stores `M` and `N` - is entry **50 = 0x32**. Its handler reads payload offsets
4..18. `0x32` is **not in tinyvenc5's command set** (M128 enumerated it:
`0x06 0x09 0x2a 0x2d 0x2f 0x31 0x50 0x51 0x52 0x62` plus `0x29`), which is why
this project has never seen it.

### And ep.ko will not deliver it

`ep.ko`'s `epint_show` copies `rodata[0xa0 + cmd]` bytes to the card's
userspace (M127). Extracting `.rodata` and checking that table against M127's
twelve independently-verified lengths - **all twelve match exactly**, so the
table base is confirmed:

    0x06:8  0x07:8  0x09:8  0x29:40  0x2a:20  0x2d:44
    0x2f:44 0x31:20 0x50:44 0x51:20  0x52:7   0x62:12

    opcode 0x32 -> rodata[0xd2] = 0

**Zero.** The complete set of opcodes this firmware's `ep.ko` forwards is
`0x06 0x07 0x09 0x29 0x2a 0x2d 0x2f 0x31 0x50 0x51 0x52 0x60 0x61 0x62`.
`0x32` is not among them, so a `0x32` we send is copied as **0 bytes** - the
opcode itself never reaches tinyvenc7's dispatch.

### Therefore, and this is now fully determined

`M` and `N` stay at their `.bss` value of **0** forever. `N == 0` takes the
bitmap path (M164), the 128-bit schedule bitmap at `[-0xf38]/[-0xf30]/[-0xf2c]`
is **read-only in the entire binary** - never written, so all zeros - the
extracted bit is 0, and `r10 != 1` diverts to the 16-byte push. **Every frame,
forever.** That is exactly what M161 measured, with no free parameters left.

**The `fw=7` full-frame push is unreachable on this card's firmware**, and the
only thing that would change it is a different `ep.ko` - which the standing
no-upload rule forbids, and which broke the card once already.

### What survives, and what it cost

`fw=7` is not wasted. It proved, on hardware, things `fw=5` never could:

- the card's **completion-interrupt path works** (`frame_events=1621`,
  `fifo_drops=0`) - it had been 0 in every run in this file's history;
- the frame-token register **does** move (1558 transitions);
- the producer sustains 60 Hz indefinitely, so the one-frame bound was never
  a hardware or VIC limit.

Those are permanent gains. The route to *pixels* through tinyvenc7 is closed.

### The thread this leaves open, and it is the important one

**Under `fw=5` the host receives 3110400 bytes, and no push site examined so far
accounts for it.** tinyvenc5's `a3` values across its twelve aperture sites are
`4096` x4, `16` x2, and **`mov r3, r9`** x1 - one variable site. `a3` is the
length (M163), and a full 1080p I420 frame is `a3 = 1620` at a 1920 stride. So
tinyvenc5 has a frame-sized push and it is the `r9` site, which M153's
reachability work did not cover.

**Next: identify tinyvenc5's `mov r3, r9` push site, establish what `r9` is
there, and determine what makes it fire once and only once.** That is where the
pixels actually come from, it is the path that already delivers whole frames,
and its bound (M158's livelock) is a defect in tinyvenc5 rather than a missing
opcode.


---

## M166 (static, 2026-08-24): the pixel path is tinyvenc5 0x14728, `a3` is a BYTE count, and the one-frame bound is now fully mechanical

M165's open thread, closed.

### The push that delivers the frame

`encode_handler` has **two** reachable `TK_MMA_StartOneFrame` calls, not one.
M153 queried `0x1430c` and found it reachable; **it never queried 0x14728.**
Re-running `mz0380-cfg.py` with M153's two `mma_already_start` edge-cuts
reproduces its exact 2452/3110 reachable count, so the setup is faithful:

    StartOneFrame a3=16   0x1430c : REACHABLE   (M153 knew)
    StartOneFrame a3=r9   0x14728 : REACHABLE   (M153 never asked)
    WaitOneFrameComplete  0x14358 : unreachable
    ProcessOneFrame       0x1349c : unreachable

And `r9` at 0x14728 is computed at 0x146b8:

    146b8  ldrh  r3, [r4, #44]        ; w
    146bc  ldrh  r2, [r4, #14]        ; h
    146c0  mul   r3, r2, r3           ; w * h
    146c4  addne r3, r3, r3, lsl #1   ; NE -> * 3
    146d0  lsleq r9, r3, #1           ; EQ -> w*h*2     (4:2:2 / YUY2)
    146d4  asrne r9, r3, #1           ; NE -> w*h*3/2   (4:2:0 / I420)

For 1920x1080 the NE branch is **3110400** - the exact byte count this project
has received since M129, and the same number `mz0380_infer_frame_length`
reconstructs from the poison boundary.

**So `a3` is a byte count**, confirmed independently of M163's reasoning, and
**0x14728 is where the pixels come from.** The `MemBroker_CacheCopyBack` at
0x146d8 flushes the same buffer immediately before it.

(Note this also corrects a unit assumption in M163: `a3` is bytes, not lines.
The "1620 lines x 1920 stride" reading happened to reach the right total for
the wrong reason. tinyvenc7's `a3 = [r4+0x56] * 3/2` is a **16-bit** field
written by `-g`, so it cannot hold `w*h` and its push really is small - which is
consistent with M164/M165 rather than in tension with them.)

### Why exactly one frame, end to end

Every piece is now proven, and they compose without anything left over:

1. `encode_handler` reaches 0x14728 and pushes the **whole frame**. The host
   receives 3110400 bytes. (M129 onward, every run.)
2. The DMAC start ioctl `0xDE00` clears `free[profile]` on success (M158).
3. `TK_MMA_WaitOneFrameComplete` - the **only** caller of the `0xDE01` wait that
   sets `free[profile]` back to 1 - is **unreachable**, because
   `mma_already_start` is read nine times and never written (M153).
4. So the profile stays claimed for the life of the process.
5. The next `TK_MMA_StartOneFrame` - 0x1430c, or 0x14728 on the next loop
   iteration - gets `-1` from the driver and **spins forever** in
   `MassMemAccess_StartDMAC`'s unbounded `sched_yield` retry loop (M158), at
   `SCHED_FIFO` priority (`main` 0xe40c).
6. Only `Close` - process death - returns the profile (M158), which is why a
   respawn yields exactly one more frame (M154, M155).

That accounts for the one frame, for OBS freezing, for `frame_events=0` under
`fw=5`, for `0x2d` going unanswered afterwards (M156's open question), and for
the respawn being the only thing that ever helped. **No host-side lever exists
at any point in that chain** - the defect is a missing store in tinyvenc5's
build, and the standing rule forbids replacing it.

### What this settles about the project's goal

On this firmware, whole-frame capture is bounded at **one frame per encoder
process**. `fw=7` removes that bound but its full-frame push needs opcode 0x32,
which this card's `ep.ko` does not forward (M165). Both routes are now closed by
proof rather than by exhaustion:

| route | cadence | full frames | blocker |
|---|---|---|---|
| `fw=5` / tinyvenc5 | 1 per process | **yes, 3110400 B** | `mma_already_start` never written -> DMAC profile livelock |
| `fw=7` / tinyvenc7 | continuous, 60 Hz | no, 16 B | interval needs op 0x32; `ep.ko` forwards 0 bytes for it |

The honest summary is that this card, with the firmware it boots, is a
**single-shot 1080p frame grabber**, and the driver should present it as one.
Respawn-per-frame is the only sequence mechanism, it costs ~2 s and one of the
8-18 spawn budget per frame, and M157's `vic_fast_kill=0` is the only thing that
might widen that budget.

### Method note

The error that hid this for thirteen milestones was a **query list**, not an
analysis. M153's CFG work was correct, its tooling was right, and its conclusion
- "the only MMA push that can execute is the asynchronous one" - was drawn from
a table of five addresses that did not include the sixth. The tool would have
answered 0x14728 correctly at any point since M153. *Enumerate the call sites
first, then query all of them.*


---

## M167 (hardware, 2026-08-24): a plain `insmod` captures - the driver finally does out of the box what took a 60-line script

    sudo rmmod mz0380; sudo insmod ./mz0380.ko     # no arguments at all
    v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=1 --stream-to=/tmp/plain.raw

    stream start: SET_VIC(... fw=5 ...) ret=0
    poll-drain armed every 20 ms
    poll-drain: buf 0 holds 3110400 bytes with no completion event; delivering
    poll-drain stopped after 1 deliveries, 0 kicks
    stream stop: ... irq_total=6 frame_events=0 fifo_drops=0

    3110400 bytes = 1 whole frame
    VERDICT: NOT SPLASH   Y 212 distinct values, UV 202
    VERDICT: CHROMA OK    luma explains A 0.05 / B 0.00

A real, correctly-coloured 1080p I420 frame from a bare `insmod`. Every capture
before this one required six parameters set by hand.

### How it was found, which is the point

The M166 verification run was attempted as a "plain load" and produced
**nothing** - no `/dev/video0` at all. The driver had said why, in its own log
line: `probe-safe V4L2 node disabled; load with enable_video=1`. The test
command was wrong, not the driver.

But the reason it was easy to get wrong is the finding: **the shipping defaults
could not capture.** Six parameters were needed - `dma_handshake`, `enable_dma`,
`enable_video`, `dma_iova_remap`, `aic_on`, `poll_drain_ms` - and only two of
them defaulted on. Every script in the tree passed all six explicitly, so the
broken default was invisible for the entire life of the project: nobody ever
loaded the module the way a user would.

Their own help text admitted the reason - "off by default", "keep unloads simple
during bring-up", "M4 diagnostic". All bring-up-era, all long past. Flipped in
M166/M167: `enable_video`, `enable_dma`, `dma_handshake` to true and
`poll_drain_ms` to 20, each with a comment saying what it was and that `=0`
restores the old behaviour. No measurement is affected, because every script
already set them.

### What this confirms about M166

The delivery matched the model with nothing left over: `frame_events=0` (the
`fw=5` push has no reachable completion handler), the frame found by the
poll-drain scan, `1 deliveries` and then nothing more. The card wrote a whole
frame and never said so - exactly as predicted.

### Method note

A default that every caller overrides is not a default, it is dead code with an
opinion. The way to find these is to run the software the way a user would,
once, rather than always through the harness that knows the magic arguments.

---

## M168 (source, 2026-08-24): the node lied about its own format and then refused to describe itself - and it hung instead of admitting the one-frame bound

M167 loaded the driver the way a user would and found six defaults that could
not capture. M168 asks the next question in the same direction: once the node
exists, does it describe **the device it actually is**? Three answers, all no,
all on the shipping defaults, and all invisible to this tree because every
script here drives `v4l2-ctl` with the format already decided and never
negotiates.

### 1. The advertised fourcc was NV12. The payload is I420.

M130 settled the layout on hardware: Y, then a 960x540 **U** plane, then a
960x540 **V** plane - confirmed visually (a red pedal on a neutral mat, no cast)
and by correlation, with the U/V-swapped rendering giving the textbook red/blue
swap. RE_FINDINGS says it in three places, and `mz0380-m55-real-capture.sh`
prints `-pixel_format yuv420p` because getting it wrong "cost two viewing
mistakes in one session".

`mz0380-video.c` advertised `V4L2_PIX_FMT_NV12` anyway - on the poll-drain path,
which is the default path since M166. NV12 is the same 3110400 bytes with the
chroma **interleaved** rather than planar, so nothing fails loudly: ffmpeg,
GStreamer and OBS all accept the buffer and render the magenta/green interleave
banding this document already describes at M130. The frame was always right.
The label was wrong, and the label is the only thing an application has.

Fixed: the poll-drain path now advertises `V4L2_PIX_FMT_YUV420`, which is I420.
`sizeimage` and `bytesperline` are unchanged - `w*h*3/2` and `w` either way - so
no size arithmetic anywhere moves.

The **nosg** fake-frame path keeps NV12 deliberately. Its layout was never
confirmed either way: different producer, different 1920x1107 geometry, and flat
logo content where interleave banding would not show. Correcting it would be a
guess dressed as a measurement, and it is a diagnostic path that defaults off.

### 2. The node contradicted itself within one ioctl sequence

`ENUM_FMT` returned the raw format. `ENUM_FRAMESIZES` and `ENUM_FRAMEINTERVALS`
still tested `pixel_format != V4L2_PIX_FMT_H264` and returned **-EINVAL for the
very format `ENUM_FMT` had just handed out**. A client that enumerates before
opening - the documented order, and what GStreamer's `v4l2src` and
`v4l2-ctl --list-formats-ext` do - was told the device supports no sizes and no
frame rates.

The cause is ordinary drift: three places independently decided the fourcc, and
two of them were never revisited when M111 added the raw path. There is now one
`mz0380_current_pixelformat()` and every call site asks it - `fill_pix_format`,
`ENUM_FMT`, `TRY_FMT`, `S_FMT`, `ENUM_FRAMESIZES`, `ENUM_FRAMEINTERVALS` and the
procfs line. `struct mz0380_capture_state.pixelformat` was **deleted** rather
than kept in sync: `poll_drain_ms` and `stream_nosg` are both `0644`, so the
advertised format can change under a running driver and any cached copy is a
stale answer waiting to be printed. The procfs view had in fact been printing
`H264` unconditionally since bring-up, because the field was assigned once at
init and never again.

### 3. It hung rather than reporting the bound

Under `fw=5` the card is a single-shot grabber and M166 closed that as a proven
bound. The driver did nothing about it, so an application that kept reading -
ffmpeg, OBS, anything that streams rather than grabs - blocked in `DQBUF`
**forever** after receiving its one correct frame. A hang is the worst available
way to report a known limit: it is indistinguishable from broken hardware, a
broken driver, and a wedged card, and this project has spent sessions telling
those apart.

`stall_eos_ms` (default 2000, 0 = old behaviour) now errors the vb2 queue when a
frame **has** been delivered and nothing follows it for that long. `DQBUF` then
returns `-EIO` and `poll()` reports `EPOLLERR`, which is how V4L2 says the source
has stopped; applications finalise their output and exit with the frame they
got. `vb2_queue_error()`'s own documentation confirms the flag is cleared by
`vb2_streamoff()`, so STREAMOFF/STREAMON is a clean way to ask for another
frame.

Three properties of the gate matter:

- It arms **only after the first delivery**. The first frame lands ~1.2 s after
  stream start behind `start_delay_ms`; erroring before it would break every
  capture in the tree.
- It never respawns anything. A respawn does yield one more frame (M39), but
  each costs part of the 8-18 spawn budget, so faking a video stream that way
  would wedge the card within seconds of an OBS session.
- Under `vic_fw=7` it cannot fire: those frames are 16 bytes, fail the M115
  completeness check, are never delivered, and so `delivered` stays 0. fw=7
  keeps the old blocking behaviour, which is the correct answer for a path whose
  producer is genuinely still running.

This also removes the hazard M116 documented in `mz0380-m55-real-capture.sh`:
asking for more frames than the card can deliver used to mean `timeout` SIGTERMed
`v4l2-ctl` mid-write and the last frame lost its unflushed stdio tail. The reader
now exits on its own, with whole frames only.

### Also corrected

`enable_video`, `enable_dma` and `dma_handshake` still described themselves as
"disabled by default", "off by default" and "M4 diagnostic" in their
`MODULE_PARM_DESC` strings. M166 flipped all three to `true` and the help text
was not updated, so `modinfo` contradicted the code - the same defect M167's
method note is about, one layer up.

### Verified on hardware, 2026-08-24

`sudo scripts/mz0380-m168-v4l2-abi.sh`, one spawn, on 7.2.0-1-cachyos:

    [0]: 'YU12' (Planar YUV 4:2:0)
            Size: Discrete 1920x1080
                    Interval: Discrete 0.017s (60.000 fps)
            ... 4 discrete sizes, intervals for each

    <VIDIOC_DQBUF: failed: Input/output error
    v4l2-ctl rc=0 after 7s, captured 3110400 bytes

    poll-drain: buf 0 holds 3110400 bytes with no completion event; delivering
    poll-drain: 1 frame(s) delivered and nothing for 2000 ms - signalling end
      of stream (DQBUF will return -EIO)
    poll-drain stopped after 1 deliveries, 0 kicks

    VERDICT: NOT SPLASH   Y 212 distinct values, UV 219

All three: the fourcc is `YU12` (`V4L2_PIX_FMT_YUV420`), `ENUM_FRAMESIZES` now
answers for the format `ENUM_FMT` hands out - four sizes, each with its
intervals, where it previously returned `-EINVAL` - and a reader that asked for
three frames on a card that has one **exited by itself in 7 s with exactly one
whole frame**, where it would have blocked until something killed it. Note
`rc=0`: the `-EIO` ends the stream without making the tool report failure.

### What the verification run exposed, none of it in the capture path

**The check itself was wrong.** It parsed `--list-formats-ext` for
`Pixel Format :`, which is `--get-fmt-video`'s spelling and does not appear in
that output, so it read the fourcc as empty and printed
`FAIL fourcc = 'none' - expected YU12` **against a driver that was correct** -
the same class of mistake as the bug it was checking for. It now matches the
`[0]: 'YU12'` form that command actually prints.

**`mz0380-m127-splash.py` was still handing out the NV12 viewing hint.** Its
NOT SPLASH verdict printed
`ffplay -f rawvideo -pixel_format nv12 ...`, which is precisely the trap M130
documents, on the line a reader sees at the exact moment they have a real frame
in hand. `mz0380-m126-score.py` had the same line. Both now say `yuv420p`;
`mz0380-m130-chroma.py` already did.

**The driver's own delivery line said `inferred H.264 length=`** for a raw I420
frame - format-agnostic inference, mislabelled, on the only path that runs. Now
`inferred payload length=`.

**The buffer priming line said the same thing**: "H.264 payload size will be
inferred from the bounded changed prefix". Also now `payload size`.

**And the extent report at stream stop can imitate a failure.** The run printed

    stop buf[0] @0x0000000100000000 head=aa aa aa ... | 0/1024 sampled pages
      touched, last @0x0

on the buffer that had **just delivered a correct 1080p frame**. The drain
re-poisons immediately after copying, so by the time the report scans, a
successful buffer is 0xaa again and reads exactly like one the card never wrote
to. "0/1024 pages touched" is load-bearing evidence in several diagnoses in this
file, and until now the successful case could imitate the empty one. Each
`stream_buf` now counts the frames delivered out of it and the line says so:

    | 0/1024 sampled pages touched, last @0x0 | 1 frame(s) delivered from it
      (so it is poison again by design, not untouched)

The pattern across all of it is the M168 pattern one layer out: the payload has
been I420 since M129, the capture path was corrected then, and every *name* and
*diagnostic* around it stayed as it was - H.264, NV12, and a page-touch count
that describes the buffer after the driver has wiped it. None of it changed a
captured byte. All of it changes what the next reader concludes.

### Method note

M167's note says to run the software the way a user would. M168 is the same rule
applied one level out: the V4L2 node is not the thing the driver writes, it is
the thing an application **reads**, and no script in this tree has ever read it.
Every capture here passes `--stream-to` with the geometry known in advance, so
the entire format-negotiation surface - the part a real application depends on
first - was never exercised by anything, ever.

---

## M169 (source, 2026-08-24): the spawn budget was never actually counted, so the experiment this project keeps asking for could not be run

NEXT_SESSION_START's first open question has had the same shape for several
sessions:

> **If a session passes 18 spawns on one power cycle without wedging, that is
> the answer.**

Nothing in this tree can produce that number.

- The driver counted `stream_cycles`, not spawns. It increments on every stream
  cycle **including the ones where SET_VIC was deliberately skipped**
  (`setvic_once`), and it was never exposed anywhere - not in procfs, not in the
  stream-stop line. `nosg_spawns` is a real count but only of the diagnostic
  path's per-frame respawns.
- Whatever count existed reset at every `insmod`, and **every hardware script in
  this tree rmmods and reinsmods per run**. So each run starts from zero.
- `dmesg` would have spanned the reloads, since each spawn prints a
  `stream start: SET_VIC(...)` line - except that the same scripts open with
  `dmesg -C`.

So the budget has been tracked by memory and by re-reading the handoff's prose.
That is how a session ends up recording "12 spawns on top of whatever the
current power cycle had already used", which is not a measurement, and how the
first symptom of the cliff has always been arriving at it.

### What now counts

`dev->encoder_spawns` increments at the driver's **single** SET_VIC send site,
which is definitionally every fork of a fresh `tinyvenc5` - the nosg loop's
per-frame respawns reach that site through `mz0380_dma_start()` and so are
included. It increments on the **fire**, not on the result, for the reason the
comment already sitting above that call gives: a SET_VIC that times out may
still have spawned an encoder, and a budget that counted only successes would
under-report exactly when the card is in trouble.

It warns once at 8 - the bottom of the historical wedge range - and
`/proc/mz0380-state` carries `enc spawns : N since insmod`.

### Counting across reloads, which is the granularity the wedge has

`mz0380-spawns.sh` accumulates that per-insmod count into `/run`, which a tmpfs
clears at every boot. That is the right granularity **by construction** rather
than by bookkeeping: the handoff records that a soft PC shutdown is enough to
reset the card, so a host boot bounds a card power cycle. It is wrong for a warm
reboot, where the card may stay powered while `/run` is cleared, so `reset`
exists for when you know better.

`commit` adds only the delta since the last commit and treats a count that went
**backwards** as a fresh insmod, so it is idempotent within a load and correct
across them. `mz0380-m55-real-capture.sh` calls it from `cleanup()` rather than
at the end of the script, so a run that aborts early or is interrupted still
banks the spawns it already spent - those are the runs where the remaining
budget matters most.

    scripts/mz0380-spawns.sh          # no root needed, reads the tally + live count
    sudo scripts/mz0380-spawns.sh commit
    sudo scripts/mz0380-spawns.sh reset

The verdict line is the experiment: below 8 is the safe range, 8-18 is the
gamble, and past 18 without a wedge is M157's answer.

Working on hardware as of the M168 run:

    === 7. spawn budget ===
    spawns this power cycle: 1  (this load has spawned 1)
      Below 8 - the range where the card has never wedged.

That tally starts from this boot's M168 run; spawns spent before the instrument
existed are not in it.

### Method note

The project had a stated next experiment, a stated success criterion, and no
instrument. The reason is worth naming because it recurs: the number existed in
`dmesg`, and the harness that runs the experiment **clears `dmesg` first**. Every
piece was present and the measurement was still impossible. Before designing the
next experiment, check that something can actually read out its result.

---

## M170 (source, 2026-08-24): the parts of the driver an application talks to, that no application had ever talked to

M168 fixed what the V4L2 node said about its **format**. The same question asked
about everything else the node exposes found three more, plus a build and a
front door that both point at deleted code.

### The source-change event was dead at both ends

`mz0380_signal_event()` builds a `V4L2_EVENT_SOURCE_CHANGE` and queues it. It is
`EXPORT_SYMBOL_GPL`, it has been there since bring-up, and **nothing has ever
called it** - `grep` across the tree returns the definition and no callers.

Had anything called it, nobody could have received it: the ops table set

    .vidioc_subscribe_event = v4l2_ctrl_subscribe_event,

which accepts `V4L2_EVENT_CTRL` and returns `-EINVAL` for everything else. So
`SUBSCRIBE_EVENT(SOURCE_CHANGE)` failed for every client. `PLAN.md` lists the
feature as implemented.

Both halves fixed together, because either alone is still nothing:
`mz0380_subscribe_event()` routes `SOURCE_CHANGE` to
`v4l2_src_change_event_subscribe()`, and `mz0380_query_signal()` now compares
the new detection against what was last reported and emits on a change - lock
gained, lock lost, or different timings.

There is no background poller, so the event fires when something asks
(`QUERY_DV_TIMINGS`, `ENUM_INPUT`, arming a stream). That is useful to a polling
client and honest about what it is. A client that only sleeps on the event needs
a poller, deliberately not added: it would drive receiver I2C on a timer, and
M74 is the record of what a watch running alongside a capture costs.

### The HDMI input could not say whether anything was plugged into it

`mz0380_enum_input()` `memset` the whole struct and filled in three fields. So:

- `capabilities` was **0**, with no `V4L2_IN_CAP_DV_TIMINGS`, even though this
  driver implements the entire DV-timings ioctl set. That flag is how a client
  learns those calls are worth making, and they are the *only* way to get
  geometry off this card, because the card never pushes format to the host (M6).
  A well-behaved application therefore never asked.
- `status` was **0**, which in V4L2 means *no problems*. The node asserted a
  healthy source unconditionally, cable in or out. `V4L2_IN_ST_NO_SIGNAL` is the
  standard bit for exactly the question this project has repeatedly answered by
  hand.

Both are filled in now. The live query is skipped while streaming - it reads the
MST3367 over the mailbox I2C proxy and streaming already knows what it locked -
and only the selected input is measured, since the receiver serves one at a
time. The others report `NO_SIGNAL` rather than claiming a clean bill of health
for a path nothing has looked at.

### `make load-streaming` failed on a correctly-installed system

Its `fw-install` prerequisite tested for `/lib/firmware/mz0380/MZ0380.HD.HEX`
and aborted with instructions to go and extract it from the Windows installer.
**That blob is the firmware-upload path, which was deleted from this tree.**
Nothing reads it. The only file the driver requests is the optional
`MZ0380.FW.TXT` version sidecar. The target now reports the sidecar's presence
and never fails. `load`/`load-streaming` stopped spelling out parameters that
have been defaults since M166, and `capture-h264` - which asked a one-frame card
for six frames of a format it does not produce - became `capture`, asking for
one.

### The README described a different driver

It opened with "The host driver uploads firmware", gave a three-step bring-up
sequence in which two steps pass `firmware_upload=1`, documented that parameter
in its table, and closed by warning readers to be careful before "flipping
`firmware_upload=1` on production hardware". **That parameter does not exist**;
`insmod` rejects it. It also promised "a V4L2 H.264 capture device" for a raw
I420 payload, listed defaults that changed in M166, and had a stray
`sudo chown` line loose in the Status section.

The front door was the last place in the tree still advertising the one
operation the project has a standing rule against. Rewritten around what the
device actually is, leading with the single-frame bound. `PLAN.md` carries a
SUPERSEDED banner naming each of its false claims rather than being quietly
left to be believed.

### Status

Source-verified, both kernels clean. `mz0380-m170-readiness.sh` is the check:
`v4l2-compliance` plus the first repeat-capture test this project has ever run.

### Method note

M168's note said the node is what an application *reads*. M170 is the rest of
that surface, and the pattern is consistent: every part of this driver that a
script in this tree drives has been debugged to death, and every part that only
an outside application would touch - input status, event subscription, the
build's own load target, the README - was never executed by anything and quietly
described a driver that no longer exists.

---

## M171 (hardware, 2026-08-24): v4l2-compliance, run for the first time - 148 tests, 16 failures, three causes

`mz0380-m170-readiness.sh` on the card. The repeat-capture half passed
outright; the conformance half found three defects, each multiplied across the
five inputs.

### Repeat capture works - the still grabber is usable as one

Three consecutive stills, one encoder spawn each:

    /tmp/m170-1.i420  3110400 bytes  sha 0d776cc4b79a61f7  NOT SPLASH, Y 214
    /tmp/m170-2.i420  3110400 bytes  sha 46ea2b605a72caef  NOT SPLASH, Y 212
    /tmp/m170-3.i420  3110400 bytes  sha aee605059e797904  NOT SPLASH, Y 213

Every one a whole frame, every one a real picture, and **all three hashes
different** - so each STREAMOFF/STREAMON genuinely captured the scene again
rather than re-reading one buffer. M39 said a fresh spawn yields one more frame;
this is the first run that took three in a row and checked they were whole and
distinct. Spawn tally ended at 3, still below the 8-18 wedge range.

### 148 tests, 132 succeeded, 16 failed, 5 warnings

**1. `VIDIOC_G/S_CTRL` and `G/S/TRY_EXT_CTRLS`: "returned control value out of
range", "invalid control 009909cb" (10 failures).**

`0x009909cb` is `V4L2_CID_MPEG_VIDEO_GOP_SIZE`, declared `min=1 max=300
default=30`, and reporting **0**. `/proc/mz0380-state` shows where the 0 came
from:

    gop hw     : 0 raw=00000000 via BAR5[0x0080] mask=0xffffffff shift=0
    bitrate hw : 0 raw=00000000 via BAR5[0x005c] mask=0xffffffff shift=0

The `mz0380_sync_*` helpers read those BAR5 fields and adopt whatever they hold.
The fields are written only by the H.264 path; this driver's capture path is raw
I420, so they have never been written and read back as zero. **Zero means
UNSET.** `v4l2-ctl --list-ctrls` was showing `video_bitrate ... min=262144 ...
value=0` for the same reason.

The bitrate sync already *knew*: it printed
`candidate bitrate field is outside current V4L2 range [262144..12582912]` and
then stored the value anyway. Meanwhile `mz0380_sync_candidate_b_frames()` and
`mz0380_sync_candidate_record_mode()` had it right all along - they range-check
and return false. So the fix is not a new policy, it is making the other six
agree with the two that were already correct: `mz0380_sync_in_range()` rejects
an out-of-range readback and leaves the cached value, which is the declared
default until userspace sets something.

The GOP range lived in `mz0380-video.c` while the readback lives in
`mz0380-core.c`, so the check could not have been written where it was needed.
`MZ0380_MIN_GOP`/`DEFAULT`/`MAX` moved to `mz0380.h` beside the other ranges.

**2. `VIDIOC_G/S_PARM`: `!cap->readbuffers` (5 failures).**

The node advertises `V4L2_CAP_READWRITE` and `vb2_fop_read`, so `read()` is a
supported I/O method - and `parm.capture.readbuffers` was left 0 by the memset.
A device claiming `read()` support while declaring it has no buffers to read
into. Now reports `MZ0380_READ_BUFFERS`.

**3. `VIDIOC_ENUM/G/S/QUERY_DV_TIMINGS`: `g_timings.bt.width !=
enumtimings.timings.bt.width` (1 failure).**

`G_DV_TIMINGS` returned `detected_timings` and `S_DV_TIMINGS` was a bare
`return 0` under a comment reading "card auto-detects; setting is a
no-op-but-validate" - which neither noted the value nor validated it. So S
followed by G returned something unrelated to what was set.

V4L2 keeps these distinct: **G returns what was SET, QUERY returns what is
DETECTED.** They are now separate fields. `S_DV_TIMINGS` validates against
`mz0380_timings_cap`, returns `-EBUSY` while streaming, and stores; a successful
detection also updates the set value so G tracks reality for a client that never
calls S. Nothing about capture changes - stream start reads the detection, as it
always did.

**Warning, not fixed:** `V4L2_CID_DV_RX_POWER_PRESENT not found` on every input.
The standard control for "is the source powering the hotplug pin". The driver
has the underlying information; exposing it is a genuine addition rather than a
correction, and it is a warning.

### Re-run: 132/16 -> 142/6

The control-range, `readbuffers` and DV-timings-contract fixes all took. The six
that remained were of two kinds, and five of them were **introduced by the fix
itself**.

**`testCanSetSameTimings` (5).** `S_DV_TIMINGS` gained a bare
`vb2_is_busy()` -> `-EBUSY`. Compliance allocates buffers and then sets the
timings it already has, which must succeed: setting a value to itself
invalidates nothing. `-EBUSY` is for a change that would resize the format under
allocated buffers, so the test is now `busy AND the timings differ`.

**`fmt.fmt.pix.width >= enumtimings.timings.bt.width * 1.5` (1).** Compliance
set a small timing, asked for the format, and was still told 1920x1080 -
`S_DV_TIMINGS` updated its own stored copy and nothing else. For an HDMI
receiver **the capture format is the source geometry**: there is no scaler on
this path and the delivered frame is exactly `width*height*3/2` of I420. So
setting the timings now snaps `capture.width/height` through
`mz0380_find_mode()` and picks the matching interval.

Worth noting what was NOT changed with it: the *detection* path still leaves
`capture.width/height` alone. Making the advertised format follow the detected
source is the coherent design for a capture card - and it is what the
source-change event wired up in M170 exists to announce - but it cannot be
validated without a non-1080p source, and if the card turns out to deliver
1080p-sized frames for a 720p input the drain's "does not fit vb2 plane" check
would start rejecting every frame. It stays on the open list with the rest of
the non-1080p question.

### Second re-run: 142/6 -> 147/1

Both M172 fixes took. The single remaining failure was
`v4l2-test-io-config.cpp(210): field == V4L2_FIELD_NONE`.

`mz0380_fill_pix_format()` set `pix->field = V4L2_FIELD_NONE` as a literal. This
driver's `mz0380_timings_cap` advertises `V4L2_DV_BT_CAP_INTERLACED`, so
`ENUM_DV_TIMINGS` enumerates two interlaced modes - index 11 and 13, 1080i50 and
1080i60. Compliance sets one, asks for the format, and is told a **progressive
format for an interlaced signal**.

The field now follows the negotiated timings. `V4L2_FIELD_INTERLACED` rather
than `ALTERNATE` because this path delivers a whole frame per buffer, and for
interlaced BT timings `bt.height` is already the frame height - both interlaced
entries report `Active height: 1080` - so the geometry stays consistent either
way. The delivery path uses the same helper, so a buffer never claims a
different field from the format negotiated for it.

`mz0380_current_field()` also answers `NONE` unconditionally under
`stream_nosg`: that path is the card's fixed progressive test pattern whatever
timings were set, and its delivery site says `NONE` outright. Without that the
format and the buffer would have disagreed on the one path where the answer is
not in doubt.

### Third re-run: 148 tests, 148 succeeded, 0 failed

    Total for mz0380 device /dev/video0: 148, Succeeded: 148, Failed: 0, Warnings: 5

**v4l2-compliance is clean.** 132/16 -> 142/6 -> 147/1 -> 148/0.

The five remaining warnings are all `V4L2_CID_DV_RX_POWER_PRESENT not found`,
one per input, and that is deliberately still open - see below.

### Method note

The conformance suite has existed for the whole life of this project and had
never been pointed at the driver. It found in one run, costing zero encoder
spawns, three defects that no amount of capturing would have surfaced - because
all three live in the half of the ABI that answers questions rather than moves
pixels, and nothing in this tree asks questions.

The follow-up is the second half of that note: **five of the six remaining
failures were introduced by the fix for the first sixteen.** A conformance suite
is not a checklist you satisfy once; it is the only thing in this tree that
notices when a correction goes too far. `mz0380-compliance.sh` makes re-running
it one word, and it costs nothing against the spawn budget.


---

## M173 (open, 2026-08-24): the last compliance warning needs a fact this project has only ever guessed at

`V4L2_CID_DV_RX_POWER_PRESENT` is the standard control for "is a powered source
attached to this input" - the +5V line, distinct from whether the receiver has
locked. Exporting it would clear the last five warnings and is genuinely useful:
it is how an application distinguishes *nothing plugged in* from *plugged in but
not transmitting*, which is a distinction this project has repeatedly made by
hand and at some cost.

The driver appears to have the bits. `mz0380-mst3367.c` says of R55:

>  we consistently read 0x03 - bits the driver does not use, most plausibly
>  5V/clock presence.

and M45 reasoned the same way: "The natural reading is 5V/cable presence and/or
clock detect." With a source attached, R55 currently reads **0x7f** - bits 0 and
1 set, outside the 0x3c lock mask the GPL driver gates on.

**"Most plausibly" and "the natural reading" are not measurements.** Wiring a
standard V4L2 control to a hypothesis would export a guess to every application
that reads it, and would do it in the one place a client is entitled to trust -
worse than the warning, which at least says "not found" honestly.

M45 wrote the test years of sessions ago and it was never run: does R55 & 0x03
track the cable? It costs **zero encoder spawns** and needs only an unplug:

    cat /proc/mz0380-hdmi | grep '55='        # source connected  -> 55=7f now
    # unplug the HDMI cable
    cat /proc/mz0380-hdmi | grep '55='        # expect the low bits to drop

`/proc/mz0380-hdmi` is world-readable, so this needs no root and no reload.

### ANSWERED, 2026-08-24, on hardware: the guess is WRONG

`mz0380-m173-5v-test.sh`, one unplug/replug, zero encoder spawns:

    18:24:25  R55=0x83   bits0-1=3   lock(0x3c)=0x00     <- cable out
    18:24:27  R55=0x03   bits0-1=3   lock(0x3c)=0x00     <- settled, out
    18:24:38  R55=0x83   bits0-1=3   lock(0x3c)=0x00     <- replugging
    18:24:40  R55=0xff   bits0-1=3   lock(0x3c)=0x3c     <- acquiring
    18:24:42  R55=0x7f   bits0-1=3   lock(0x3c)=0x3c     <- settled, locked

The lock bits went `0x3c -> 0x00 -> 0x3c`, which is what makes this conclusive
rather than null: the read is live and the cable really was out. **Bits 0-1 held
the value 3 through all of it.** They are not 5V presence, not cable presence,
and not clock presence - they do not move at all.

`V4L2_CID_DV_RX_POWER_PRESENT` therefore stays unimplemented and
v4l2-compliance keeps its five warnings. The driver has no +5V detect. "Locked"
is not a substitute: a source that is powered but not transmitting reads
lock = 0, which is the exact distinction the control exists to make, so wiring
it to the lock bits would answer the question wrongly rather than not at all.

### What the same trace did establish

Decomposing the four observed values by what is constant and what moves:

| | mask | |
|---|---|---|
| set in every sample, cable in or out | `0x03` | not status - stuck high |
| set only with the cable in | `0x7c` | tracks the signal |
| the driver's lock mask | `0x3c` | a **subset** of what tracks |

**Bit 6 tracks the cable exactly as the 0x3c lock bits do, and neither this
driver nor hdcapm gates on it.** That is not a reason to change the mask - 0x3c
has correctly reported LOCKED in every capture this project has made, and
widening a working detect for tidiness is how a working detect stops working -
but it is now a recorded fact rather than an unexamined one.

**Bit 7 is not a status bit in the usual sense.** It was set at 18:24:25 (just
after the unplug), 18:24:38 and 18:24:40 (during the replug), and clear in both
*settled* states, 0x03 (out) and 0x7f (locked). That is consistent with a
transition or instability flag. Five samples is not enough to call it, and it is
recorded here as an observation with its sample count - which is the whole
difference between this entry and the one it replaces.

### Method note

M45 wrote "most plausibly 5V/clock presence" and "the natural reading is 5V/cable
presence". Both were reasonable. Both were wrong, and the test that would have
shown it cost **zero encoder spawns, no root, and forty seconds** - it needed a
person to pull a cable, which is the only reason it sat unrun for the life of
the project.

The near-miss is worth recording too. The first run of the test changed nothing
at all, because the cable was still in. Two `cat`s of R55 would have been
indistinguishable from a real negative, and would have "confirmed" the opposite
conclusion just as convincingly. The script prints changes as they happen and
reports "nothing moved - not even the lock bits" as a **failed experiment**
rather than a negative result, and that distinction is what made the second run
mean anything.

---

## M174 (source, 2026-08-24): the buffers and the card have always had separate ideas about the frame size

Found by reading, not by running - no non-1080p source exists in this project,
which is exactly why it survived.

Two geometries have coexisted since the raw path was added:

- `mz0380_queue_setup()` sizes the vb2 plane from `mz0380_current_sizeimage()`,
  which is `capture.width * capture.height * 3/2` - **what the application
  negotiated**, 1920x1080 unless it called S_FMT.
- the drain measures completeness and delivers against
  `capture.source_width * source_height * 3/2` - **what the card actually
  wrote**, taken from the receiver's detection.

Nothing has ever compared them, and nothing makes one follow the other. On a
1080p source they are the same 3110400, which is every source this project has
ever had, so the gap has never once shown.

With a 720p source they differ and the failure is silent. The card writes
1382400 bytes; the drain's `len < want` check passes because `want` is also
1382400; `vb2_set_plane_payload()` records 1382400 into a plane of 3110400; and
the application is handed a buffer it was told is 1920x1080 YU12 with a 720p
frame inside it. No ioctl fails. It renders garbage.

That is the same shape as M168's fourcc: **a correct payload with a wrong label,
which nothing in the stack can detect and every consumer gets wrong.**

### The fix, and what it deliberately does not do

`strict_geometry` (def 1) makes stream start refuse when the detected source and
the negotiated format disagree, with a message naming both geometries, both byte
counts and the `S_FMT` that resolves it - and queues the source-change event
M170 made subscribable, so a client that subscribed can re-negotiate without
being told.

It does **not** make the format follow the detection. That is the tempting fix
and it is a trap here: `mz0380_query_signal()` now runs from `ENUM_INPUT` as
well as `QUERY_DV_TIMINGS`, so a format that follows detection would resize
itself underneath v4l2-compliance's format tests, which enumerate and set
formats without any expectation that reading an input changes one. The M171/M172
work is one re-run away from being undone that way. Refusing keeps
negotiation entirely in the application's hands, where V4L2 puts it.

`strict_geometry=0` restores the old behaviour, for the case where a detection
wobble refuses a capture that would have worked.

### Status

**The non-1080p path is now correct by construction, not verified.** What is
proven is that the previous behaviour was wrong. Verifying it needs a source
this project does not have; the 1080p path must be regression-checked either
way, since the guard sits directly on it.

---

## M175 (hardware, 2026-08-24): there was a THIRD encoder binary, nobody had run it, and it is now measured

`NEXT_SESSION_START` has said for several sessions that continuous streaming is
closed because both routes are blocked. There were three routes.

`re-dump/fw/yuan_demo_sdi/tinyvenc8` is 447 KB of ARM dated March 2020. This
file mentions it **once**, inside a sentence about SET_VIC byte 6. It had never
been disassembled and never been spawned - and `vic_fw=8` reaches it with no
upload and no rule broken, because the driver does
`u32 fw = mz0380_vic_fw ?: ...` with no clamping.

### Why it looked worth a spawn

| | tinyvenc5 | tinyvenc8 |
|---|---|---|
| `EncodingGroup::mma_already_start` | **1 byte** | **8 bytes** |
| `TK_MMA_*` calls in `encode_handler` | 2 start / 2 wait | 2 start / 2 wait |
| `TK_MMA_*` calls in `fake_frame_process` | 2 start / 2 wait | **none** |
| `fake_frame` strings | 4 | 1 |

That first row is the livelock itself. M153 found `mma_already_start` read and
never written in tinyvenc5, which makes `TK_MMA_WaitOneFrameComplete`
unreachable, which is why the DMAC profile is never returned. A different SIZE
is a different type - one flag versus eight - which is what per-profile state
would look like.

### Run 1 read as "nothing delivered". It was not.

    poll-drain: buf 0 holds 777600 of 3110400 bytes - DMA still in flight, waiting

sixty times a second, for twenty-five seconds. **777600 = 960 x 540 x 3/2**, a
complete quarter-resolution I420 frame, exactly. The card was writing whole
frames and the driver was discarding every one of them.

The completeness rule (M115, M160) is "a frame is done at
`source_width * source_height * 3/2`". That is right for tinyvenc5, which writes
a 1080p frame for a 1080p source, and it was written when tinyvenc5 was the only
producer anyone had run. It cannot tell a **whole 960x540 frame** from a
**three-quarters-written 1080p one**, so it waited forever.

`expect_frame_bytes` now says what a frame is; 0 keeps deriving it from the
source, which is every result before this one.

### Run 2, with the driver told what a frame is

    10s, 777600 bytes = 1 whole 777600-byte frame
    poll-drain: buf 0 holds 130816 of 777600 bytes - DMA still in flight, waiting
    poll-drain: buf 0 holds 777600 bytes with no completion event; delivering
    poll-drain: 1 frame(s) delivered and nothing for 5000 ms - end of stream

    VERDICT: NOT SPLASH   Y 213 distinct values, UV 213

**tinyvenc8 works and produces real 960x540 video - one frame per process, then
the same freeze as tinyvenc5.** `frame_events=0`, producer watch saw no change.
The 8-byte `mma_already_start` did not buy a second frame.

### All three binaries, now measured rather than assumed

| | pixels | cadence | `frame_events` |
|---|---|---|---|
| `fw=5` tinyvenc5 | 1920x1080 whole frames | one per process, then livelock | 0 |
| `fw=7` tinyvenc7 | **16 bytes** | continuous 60 Hz, real IRQs | 1621 in 57 s |
| `fw=8` tinyvenc8 | **960x540** whole frames | one per process, then livelock | 0 |

Two of the three have pixels and no cadence; one has cadence and no pixels.
**Continuous streaming is closed on this firmware** - and it is now closed by
measurement of every binary the card ships, rather than by having tested two of
three.

fw=8 is not useless: 960x540 is a quarter of the data for the same single-shot
cost, which is the cheaper option if a smaller still is wanted. It is not a
better one - fw=5 gives full resolution for the same spawn.

### Method note

Two mistakes in one milestone, both mine, both the same shape.

**"Both routes are closed" was in the handoff, and I repeated it for several
turns before checking.** It was written when two binaries were known. The third
was named in the tree, spawnable by an existing parameter, and never tried.
A closure inherited from a document is not a measurement, and the cost of
checking was one spawn.

**Run 1's verdict said "NOTHING delivered" while printing the disproof
underneath it.** The script tested for a fixed expected size and reported the
mismatch as absence. That is M173's failed-experiment-versus-negative-result
trap exactly, one milestone later, in a script written by the same person who
had just written that note. The verdict now says: if `holds N of` shows a stable
N, that N is the card's real frame size - re-run with `EXPECT=N`.

---

## M176 (hardware, 2026-08-24): fw=6 is what Windows sends, it delivers a BETTER frame than fw=5, and it does not free the livelock

The last invalidated verdict, re-run as a single variable. `vic_fw=6`,
`expect_frame_bytes=4147200`, everything else at the shipping defaults.

    plane size: 4147200
    10s, 4147200 bytes = 1 whole 4147200-byte frame
    poll-drain: buf 0 holds 1666176 of 4147200 bytes - DMA still in flight
    poll-drain: buf 0 holds 4147200 bytes with no completion event; delivering
    poll-drain: 1 frame(s) delivered and nothing for 5000 ms - end of stream
    frame_events=0

### The frame arrived, whole, and it is 4:2:2

**4147200 bytes = 1920 x 1080 x 2.** This is the first hardware confirmation of
M79's static reading that `fw == 6` makes `video_capture_mgr` write a different
capture output format. The byte count alone proves the format changed: every
other configuration writes `w*h*3/2`.

**But it is PLANAR 4:2:2, not packed YUY2**, and that is a correction to how
M79's cfg label has been read ever since - including in the script written to
run this test, an hour before the test ran:

| hypothesis | luma row-to-row corr |
|---|---|
| packed YUY2, luma = every 2nd byte | +0.8997 |
| **planar, first 1920x1080 bytes = Y** | **+0.9814** |
| pixel-shuffled control | -0.0044 |

The layout is Y 1920x1080, then U 960x1080, then V 960x1080 - I422. Both chroma
planes are image-like (row corr 0.96) and the chroma is healthy:

    corr(U, luma) +0.2647    corr(V, luma) -0.2798

against M130's broken-CSC signature of -0.96 / -0.75. Ordinary scene
correlation, no cast.

    ffplay -f rawvideo -pixel_format yuv422p -video_size 1920x1080 /tmp/cap-m176-fw6.raw

**This makes fw=6 the better still mode.** Same one spawn, same one frame, but
960x1080 chroma instead of fw=5's 960x540 - double the vertical chroma
resolution. Nothing in the driver offers it yet; the node advertises I420 and
`expect_frame_bytes` is a diagnostic knob, not a format.

### It does not free the livelock

One frame, then nothing, `frame_events=0`, producer watch saw no change - the
tinyvenc5 bound, unchanged. **The capture output format is not what separates
Windows from us.**

### All four configurations, measured

| | binary | frame | cadence |
|---|---|---|---|
| `fw=5` | tinyvenc5 | 1920x1080 **I420** (3110400) | one, then livelock |
| `fw=6` | tinyvenc5 | 1920x1080 **I422** (4147200) | one, then livelock |
| `fw=7` | tinyvenc7 | **16 bytes** | continuous 60 Hz, 1621 real IRQs |
| `fw=8` | tinyvenc8 | 960x540 I420 (777600) | one, then livelock |

Every `tinyvenc5` mode livelocks regardless of output format or resolution, and
`tinyvenc8` livelocks the same way. Only `tinyvenc7` sustains a cadence, and it
pushes 16 bytes.

### So how DOES Windows do it - the next thing to check

Windows sends `fw=7` at 1080p60. Under `fw=7` this card pushes 16 bytes per
frame into all four of our buffers, cycling `0->1->2->3->0`, with real
completion interrupts, indefinitely (M161). Windows gets video out of that.

**Nobody has ever dumped what those 16 bytes contain.** M161 measured the
*length* - "holds 16 of" is the only length that ever appears - and M164
interpreted the surrounding code statically. The driver logs
`stop buf[N] head=<16 bytes>` at every stream stop, so the content has been one
`grep` away from being known for the whole investigation, and no `fw=7` run in
this file quotes it.

If those 16 bytes are a descriptor - an address and a length - then the picture
is somewhere we have never looked, and the card has been telling us where 60
times a second. If they are pixels (as the `fw=5` 16-byte truncations were,
M138: real video at the scene's luma), the frame-interval reading stands.

Related and also never done: **M32's outbound-window probe has only ever run
under `fw=5`**, where the producer emits one frame and stops. Its conclusion -
"all three extra windows accepted the addresses, none was ever written" - was
measured against a producer that was livelocked. `probe_windows=1` under `fw=7`,
where the producer is demonstrably alive and pushing continuously, is a
different experiment with the same command.

Both cost one spawn each. Neither has been tried.

---

## M177 (hardware/source, 2026-08-24): window 1 is the continuous H.264 output, and V4L2/OBS now consume it

M176's closure was wrong because it treated tinyvenc7's 16-byte preview DMA as
the only output. Windows registers a separate opcode-`0x04` four-slot bank for
the encoder. With four 1 MiB backing buffers mapped at IOVAs `0x500000000`
through `0x800000000`, and the Windows-advertised slot size `0x97f00`, all four
buffers receive continuous encoded output.

The decisive start sequence is `fw=7`, `win_seq=1`, `win_start_op6=1`,
`post_mask=0`, `vic_fast_kill=1`. Window 1 must be registered again after
SET_VIC, because SET_VIC spawns tinyvenc7 and resets the card-side state.

Each slot has a 4 KiB transport header:

```
+0x00  encoded byte count
+0x08  encoder metadata
+0x0c  one-based slot identity
+0x10  duplicate encoded byte count
+0x1000 Annex-B H.264 access unit
```

The driver validates both lengths, the identity, and the Annex-B start code,
then strips the transport header and completes one V4L2 H.264 buffer per access
unit. It also ignores repeated BAR0+0x44 tokens from preview-only completion
events.

First bounded capture:

```
77 V4L2 access units, 2 startup drops
11,476,992-byte elementary stream
ffprobe: H.264 High, 1920x1080, yuvj420p, level 4.1, 77 frames
ffmpeg: all 77 frames decoded (one recoverable macroblock error)
```

The first decoded frame was the live Nucleo-board HDMI picture, not the splash.
OBS then ran the same path for 910 delivered access units with only 2 drops for
lack of an initial userspace buffer. Its render 99th percentile was under
0.5 ms. Continuous HDMI capture is therefore open and working; the remaining
problem is encoded cadence, not transport or userspace loss.

OBS initially showed black for a separate reason: a hidden old V4L2 source
owned `/dev/video0`, while the visible duplicate failed with `EBUSY`. Removing
the duplicate sources left one owner and produced the picture.

---

## M178 (static/source, 2026-08-24, **field interpretation superseded by M204**): the 12 fps limiter is tinyvenc7 reusing Windows's QP-min byte as a frame divisor

M204 preserves the measured modulo result but corrects the field name and zero
mode: command byte 20 is `skip`, not QP-min, and zero selects a bitmap which
SET_ENC processing populates locally rather than an inaccessible empty bitmap.

OBS negotiated 60 fps, but the first run converged near 12 fps. The driver
delivered 910 and dropped only 2, and tinyvenc7 continued producing preview
completion events near the 60 Hz input rate, so neither OBS nor the host queue
was the limiter.

tinyvenc7 `vcap_handler` at `0x12668` supplies the explanation:

```
ldrb  r1, [stream + 0x18]
bl    __aeabi_uidivmod       ; remainder = input counter % byte
cmp   remainder, #1
bne   no_h264_this_frame
```

SET_ENC_PARAMS mask bit 7 copies payload byte `+0x14` (the field the firmware
banner calls minimum QP) to `stream+0x18`. Windows sends 5 and runs tinyvenc5,
where this value is a normal QP bound. We copied that Windows value while
running tinyvenc7. On tinyvenc7 it therefore also means encode one frame in
five: `60 / 5 = 12 fps`, exactly the measured cadence.

The fastest usable divisor is 2, giving 30 fps from 60 Hz input. Divisor 1 is
a firmware edge case: `counter % 1` is always zero and can never equal one, so
it produces no H.264. Divisor 0 selects a 128-bit schedule bitmap populated by
opcode `0x32`; the shipping `ep.ko` forwards zero bytes for that opcode, so the
all-frames schedule is not host-reachable without changing card firmware.

The driver now exposes `h264_frame_divisor`, defaults it to 2, rejects values
outside 2..255, and passes it in that nominal QP-min byte. The module builds
cleanly. The 30 fps prediction still needs one post-power-cycle hardware run;
the previous power cycle ended at 6 SET_VIC spawns, and opening OBS properties
caused two capture restarts, so do not spend that run before resetting the
card's spawn tally.

---

## M179 (hardware/source, 2026-08-24): divisor 2 produces 30 fps; OBS latency is a separate buffering problem

The first divisor-2 OBS run used one SET_VIC spawn and delivered 1125 frames
before the live status sample. Consecutive card access units in the kernel log
arrived 32.7-34.1 ms apart. The complete OBS session captured 1975 frames from
23:30:53.630 to 23:32:02.949: 28.5 fps including encoder startup, converging on
the predicted 30 fps. OBS render work remained negligible (99th percentile
0.513 ms). The divisor fix is therefore proven.

The source still felt delayed/unresponsive because two independent timing
facts were wrong for a low-latency preview:

1. The driver advertised the 60 Hz VIC input cadence even though tinyvenc7
   emitted H.264 at 30 Hz. The H.264 V4L2 path now enumerates only its effective
   `divisor/source_fps` interval and G/S_PARM return the same value, while the
   internal source interval remains 60 Hz for SET_VIC.
2. OBS's Linux V4L2 source defaults `buffering=true`; the saved scene did not
   override it. For interactive capture, uncheck **Use Buffering** on the one
   V4L2 source. This changes OBS's async queue mode and does not change the
   card's encoder cadence.

Ten `no queued vb2 buffer` messages occurred in a short burst during the live
status interaction, but 10/1135 is below one percent and cannot explain the
overall responsiveness. The producer itself stayed at a regular 30 Hz.

The corrected-cadence module builds cleanly but needs a new post-power-cycle
OBS run. The last cycle ended at 7 spawns, so no further run is safe on it.

---

## M180 (hardware/OBS, 2026-08-24): 30 fps reporting verified; disabling OBS buffering improves responsiveness

After a full power cycle, the corrected-cadence module was loaded with
`VICFW=7 H264PROBE=1 POLLDRAIN=0 WINSEQ=1 OP6=1 POSTMASK=0 FASTKILL=1
H264DIVISOR=2`. `VIDIOC_ENUM_FRAMEINTERVALS` reported a discrete 0.033 s / 30.000
fps interval for H.264 at all four advertised sizes. OBS independently logged
`Framerate: 30.00 fps` and set its select timeout to 166666 us (five frame
periods), proving the V4L2 reporting fix reached the application.

Turning off the OBS source's **Use Buffering** option made the live preview
visibly smoother/more responsive. Although OBS itself was not restarted,
applying the source property did restart V4L2 capture: the log stopped the first
capture after 447 frames and began a second one about 397 ms later. Corresponding
driver status showed two SET_VIC spawns. Treat opening/applying source Properties
as potentially costing one spawn in this installed OBS build.

The post-change status showed a locked 1920x1080p60 HDMI input, 1111 H.264
frames delivered, 10 dropped, and zero command timeouts. The ten drops form the
expected approximately 333 ms no-vb2-buffer gap around capture restart, not a
producer stall. The firmware continued generating access units every roughly
33.3 ms. The machine/card will be fully powered off before the next session, so
the next run starts with a reset card spawn budget.

### Requirement carried forward after M180

The user states that the card should work at 60 fps. Therefore M178's 30 fps
result is a working checkpoint, not project closure. It proves only that
tinyvenc7's currently selected modulo/divisor mode cannot encode every 60 Hz
input frame (`N=2` gives 30; `N=1` never satisfies remainder 1). The next
session must use the Windows collection at
`/run/media/wolffyx/Work/hd60-trace/collect-2026-08-19` to find the all-frame
mode or missing initialization. Re-examine divisor 0 / the 128-bit schedule,
opcode `0x32` and alternate transports, main versus sub stream setup, and
pre-`SET_ENC_PARAMS` state. Do not generalize the current divisor-mode limit
into a claim that the card or complete firmware cannot do 60 fps.

---

## M181 (hardware/OBS, 2026-08-25): the post-power-cycle run is healthy; OBS spent three spawns

After removing mains power and starting clean, the documented live command was
run unchanged:

```
sudo env VICFW=7 H264PROBE=1 POLLDRAIN=0 \
    WINSEQ=1 OP6=1 POSTMASK=0 FASTKILL=1 \
    H264DIVISOR=2 ./mz0380-live.sh load
```

The board reported its already-running firmware 1.11 and legacy INTx came up
normally. The subsequent status sample, while OBS was the sole `/dev/video0`
owner, reported:

```
signal       : locked 1920x1080p60
SET_VIC sent : 3
H.264        : 2915 delivered, 10 dropped (current stream)
timeouts     : 0
users        : 1
```

The ten drops form one contiguous producer-paced burst from 540.906351 through
541.206733: one access unit every about 33 ms had no queued vb2 buffer. That is
userspace buffer starvation over roughly 333 ms, not an HDMI unlock or encoder
stall. The receiver remained locked and its post-START output/link snapshots
were normal immediately afterward.

This run independently confirms that the corrected 30 fps H.264 path survives
a cold card start and a longer OBS session. It does **not** test 60 fps: the
encoder still ran with `h264_frame_divisor=2`. `SET_VIC sent: 3` proves OBS
caused three V4L2 STREAMON cycles during the attempt. The log alone cannot say
which UI actions caused the two extra cycles, so do not attribute them more
narrowly; the operational rule remains that reopening/applying Properties can
spend another spawn.

---

## M182 (Windows trace/static, 2026-08-25, **H.264 conclusion superseded by M204**): Windows does not select an all-frame H.264 schedule; its raw buffer topology is the real unimplemented difference

The complete saved Windows collection was revisited specifically for the 60 fps
requirement. The 1080p60 transitions in
`logs/hd60-bringup-20260819-223305.log` say:

```
[CH00] vi=6, fw=7, cx=1920, cy=1080, fps=60, ...
SET_ENC main: fps=60, ... qp=5~51
SET_ENC sub : fps=60, ... qp=5~51
```

That is not an all-frame tinyvenc7 H.264 configuration. M178 proved that
tinyvenc7 copies this nominal QP-min byte into the divisor used by
`(frame_counter % N) == 1`; `N=5` selects one encoded frame in five. The
Windows driver's structural mailbox sweep finds 38 opcodes and **does not
contain `0x32`**, so there is no traced Windows host command populating the
128-bit all-frame schedule either.

This corrects the next-work premise after M180. The saved Windows trace does not
hide a different `SET_ENC_PARAMS` value or an opcode-`0x32` send that Linux
missed. Either Windows' requested 60 fps was not a measurement of 60 distinct
delivered frames, or its usable 60 fps capture comes through a different output
path. The latter now has a concrete structural candidate.

For the exact `PCI\VEN_12AB&DEV_0380&SUBSYS_00061CFA` branch, disassembly of the
Windows buffer-registration function at `0x14027b62b..0x14027b925` shows:

| opcode | bank | buffers | advertised bytes | context address fields |
|---|---:|---:|---:|---|
| `0x02` | window 0, first half | 4 | `0x466000` | `+0x190..+0x1c0` |
| `0x08` | window 0, second half | 4 | `0x466000` | `+0x1d0..+0x200` |
| `0x03` | window 3/control | 4 per group | `0x2000` | common small pool |
| `0x04` | window 1 | 4 | `0x34bd00` | `+0x390..+0x3c0` |
| `0x05` | window 2 | 4 | `0x34bd00` | `+0x590..+0x5c0` |

The sizes agree with the Windows startup line `[MEMORY] [00466000]
[0034BD00] [0034BD00]`: `0x466000` is a 2048-stride, 1125-line YUV422-sized
region plus 4096 bytes, while `0x34bd00` is the corresponding YUV420-sized
region plus 256 bytes.

Linux does not reproduce that topology:

- its opcode-`0x02` ring has four 4 MiB allocations, smaller than `0x466000`;
- `set_buf_op8=1` sends the **same four addresses** again rather than the four
  independent addresses Windows uses;
- the working live path assigns opcode `0x04` to a separate four-slot
  `0x097f00` H.264 transport ring.

This difference was never tested by M93/M116: those experiments enabled op
`0x08` with aliased buffers, under the old path, and therefore did not reproduce
Windows' second four-buffer bank.

### Next experiment, after a host-only implementation

Add an opt-in raw-bank diagnostic with eight independently mapped buffers:
four for opcode `0x02` and four for opcode `0x08`, each advertising `0x466000`.
Keep it separate from `h264_probe`, poison both banks distinctly, and inspect
write extents plus the BAR0 completion token during one bounded `fw=7` run.
The discriminator is exact:

- a complete raw frame in either bank identifies the 60 fps data path;
- 16 bytes in both banks closes this buffer-topology lead;
- writes only in the second bank prove that aliasing op `0x08` hid the Windows
  producer.

Do not spend that spawn until the independent mappings and bounds are
build-checked. Do not retry divisor 1 or host opcode `0x32`; both are already
closed statically.

---

## M183 (user observation, 2026-08-25): Linux freezes the last frame across source loss and resumes after reconnect; Windows presents NO SIGNAL

The user supplied the missing user-visible comparison:

- On Windows, when no HDMI source is available, the capture output displays a
  **NO SIGNAL** message rather than holding the previous frame.
- On the current Linux H.264 path, moving to another source/device froze the
  preview on the last decoded frame while the input was absent.
- After the replacement device was connected, the picture changed and live
  capture started working again.

The first point is consistent with the card's already-proven rendered NO SIGNAL
splash, but this observation alone does not establish whether the Windows
driver asks the firmware for that splash or substitutes it elsewhere. Linux's
freeze is expected for an H.264 consumer receiving no new access units: OBS
continues displaying its last decoded frame.

The recovery is the important positive result. The receiver and running live
path can apparently survive a source-loss/relock transition; a driver reload is
not intrinsically required just because the HDMI source changes. However, the
observation alone cannot prove that OBS did not cycle V4L2 internally. Preserve
the `SET_VIC sent` count immediately before and after the next source swap:

- unchanged count = same tinyvenc7 process relocked and resumed;
- incremented count = OBS recovered by STREAMOFF/STREAMON and spent another
  encoder spawn.

Do not implement the Windows-style no-signal display by respawning the card's
firmware encoder on every unlock. That would consume the finite per-power-cycle
spawn budget. It is a presentation/fallback feature separate from the 60 fps
data-path work and should eventually be handled without a SET_VIC cycle.

### Reload accounting

The user plans to unload and load the driver again. `mz0380-live.sh unload`
first commits the current module's encoder-spawn count to the `/run` tally and
then removes the module, so use it rather than a bare `rmmod`. Reloading resets
only the driver's `since insmod` counter; it does not reset the card-side
8--18-spawn budget. Only a slot power removal resets that physical state.

---

## M184 (hardware, 2026-08-25): reload tally is correct; replacement input was 1080p30 YUV422 and captured cleanly

The planned unload/reload was completed without removing card power. The first
unload reported and committed 3 encoder spawns. `mz0380-spawns.sh` then showed
3 committed with the module absent. After the next load, OBS had caused 2 new
`SET_VIC` sends; the final unload therefore committed a power-cycle total of 5:

```
first unload:  spawns this power cycle: 3 (this load has spawned 3)
second status: SET_VIC sent: 2
final unload:  spawns this power cycle: 5 (this load has spawned 2)
```

This validates the persistent accounting across module unload/reload. A module
reload did not incorrectly reset the card-side budget.

The second load was healthy: firmware 1.11 remained running, INTx 40 came up,
the current stream delivered 301 H.264 access units with zero drops, and there
were zero command timeouts. The one early
`len=3435973836 ... prefix=cc cc cc cc` message is the expected untouched
`0xcccccccc` poison sentinel before slot 3's first completion; later delivery
proves it was not a stuck slot.

This source was not the earlier 60 Hz input. Its receiver measurements were:

```
htot=2200 vtot=1125 hper=336 vper=299 hact=1920
input colorspace YUV422 (B2:48=b0)
```

Those counters identify 1920x1080p30. The prior cold-start source measured
`hper~=674 vper~=599` (1920x1080p60) and reported YUV444 (`B2:48=d2`). The
encoder and preview parameters in this reload were both set to 30 fps, so the
301 clean access units demonstrate correct capture of the replacement 30 Hz
source, not delivery of 60 distinct frames per second.

The current status already read `SET_VIC sent: 2`, but there is no preserved
count immediately before the source was disconnected. Consequently this run
still cannot prove whether source-loss recovery kept one encoder instance alive
or OBS performed a STREAMOFF/STREAMON. If that distinction is tested again,
capture `status` before disconnect, while disconnected, and after reconnect,
without opening OBS Properties. Because the power-cycle tally is now 5, remove
slot power before spending additional diagnostic spawns.

---

## M185 (host-only, 2026-08-25): adaptive card readiness, short live-lock cache, and first file-size refactor

The apparent 25-second card-boot delay in the live workflow was not imposed by
the card. `insmod` is synchronous and returns after the PCI driver's probe has
finished, yet `mz0380-live.sh` then slept for 25 seconds unconditionally. The
latest hardware log showed CMD_INIT/version completing in roughly 100 ms, so
that sleep delayed a healthy load without testing anything. It is replaced by
an early-exit device-node poll, bounded by `LOADWAIT=5` seconds.

The actual cold-card CMD_INIT wait is now adaptive too. The old ten attempts at
600 ms allowed only about six seconds before the driver gave up permanently for
that probe. `card_ready_timeout_ms=15000` now supplies a deadline: the first
successful response exits immediately, while a slow flash boot may continue
retrying for up to 15 seconds. The CMD_INIT log includes attempt count and
elapsed milliseconds so this can be measured rather than inferred.

OBS issues bursts of duplicate DV-timings queries. One successful MST3367 mode
measurement takes roughly 80--180 ms of serialized mailbox traffic, so several
queries can add visible negotiation delay and contend with other commands.
`signal_query_cache_ms=1000` now reuses only a successful, currently locked
result for one second. It does not extend the 30-second stale-mode fallback used
to arm a stream; it is a separate short cache. Signal loss may be visible one
second late. Set the new parameter to zero for an exact pre-change comparison.

The first structural split moved unrelated responsibilities out of the former
6739-line `mz0380-core.c`:

- `mz0380-mailbox.c`: mailbox transport, peripheral proxy, card handshake;
- `mz0380-pci.c`: PCI probe/remove and BAR ownership;
- three parameter files grouped by base/stream, receiver, and Windows sequence;
- control read/synchronization and validated write units;
- snapshot, proc diagnostic, proc debug, and proc registration/state units;
- `mz0380-signal.c`, `mz0380-vb2.c`, and `mz0380-video-state.c`.

`mz0380-core.c` is now 13 lines. Every new translation unit is at most 942
lines, and `mz0380-video.c` is down to 1043. The two remaining large files are
`mz0380-mst3367.c` (2865) and `mz0380-dma.c` (2766); they deliberately remain
for a second phase after hardware validates the readiness/cache change.

The full combined module builds successfully against Linux 7.2.0-1-cachyos
with clang/lld. `git diff --check` and `bash -n mz0380-live.sh` also pass. No
hardware command or encoder spawn was used for M185.

---

## M186 (hardware, 2026-08-25): adaptive readiness and lock-query cache validated

The M185 build was loaded after resetting slot power with the standard fw7,
H.264 divisor-2 command. The full shell command completed in 13 seconds,
including compilation and the mandatory load/unload smoke test. This compares
with roughly 40 seconds for the previous workflow; the removed unconditional
25-second sleep accounts for the improvement.

The new readiness diagnostic reported:

```
CMD_INIT answered on attempt 1 after 6ms (status=0xdddddddd)
```

Probe began at 4688.296704, firmware 1.11 was reported at 4688.406237, and INTx
40 was ready at 4688.406274. The real card-side readiness and version handshake
therefore took about 110 ms in this power cycle. The 15-second deadline imposed
no delay on the healthy card and remains available for genuinely slow boots.

OBS negotiated with one V4L2 user and exactly one `SET_VIC`. Only one
`MST3367 signal:` line appeared, versus the earlier repeated query burst. It
identified a locked 1920x1080p60 YUV444 source (`hper=674`, `vper=599`,
`B2:48=d2`). This is the expected effect of the one-second successful-query
cache.

The first status caught the userspace queueing gap at 28 access units delivered
and 17 dropped. Roughly 21 seconds of kernel time after stream startup, status
showed 615 delivered and 18 dropped with zero command timeouts. The producer
continued at the expected approximately 30 fps and the drop count had nearly
stopped; the drops are missing queued vb2 buffers, not encoder, receiver, or
mailbox failures. The refactored module remained live and stable.

---

## M187 (host-only, 2026-08-25): MST3367 and DMA split; driver units now meet the size target

After M186 validated the first refactor and readiness behavior on hardware, the
two remaining oversized translation units were split without changing command
ordering, register values, allocation sizes, or stream policy.

The former 2865-line MST3367 unit is now:

| unit | responsibility | lines |
|---|---|---:|
| `mz0380-mst3367.c` | reset, initialization, EDID/HPD, receiver bring-up | 938 |
| `mz0380-mst3367-signal.c` | measurement, mode matching, output commit | 744 |
| `mz0380-mst3367-debug.c` | diagnostics, watch, CSC/output inspection | 653 |
| `mz0380-mst3367-bitbang.c` | GPIO I2C and EDID discovery tools | 513 |

Shared receiver-private types and low-level helpers are declared only in
`mz0380-mst3367-internal.h`.

The former 2766-line DMA unit is now:

| unit | responsibility | lines |
|---|---|---:|
| `mz0380-dma.c` | allocation, IOVA programming, IRQ/event setup | 836 |
| `mz0380-dma-stream.c` | encoder configuration and start/stop sequence | 773 |
| `mz0380-dma-drain.c` | frame inference, event/poll draining | 616 |
| `mz0380-dma-extent.c` | poison/extent diagnostics and teardown | 355 |
| `mz0380-dma-nosg.c` | no-SG fallback polling capture | 189 |

Shared DMA-private helpers are declared in `mz0380-dma-internal.h`. The largest
remaining ordinary `.c` file is `mz0380-video.c` at 1043 lines; every other
driver implementation unit is 942 lines or less, and `mz0380-core.c` remains
13 lines.

The full module builds and links successfully against Linux 7.2.0-1-cachyos
with clang/lld. `git diff --check` and the live-script shell syntax check pass.
The module currently loaded during M186 is the already-validated pre-M187
binary; these second-phase mechanical splits have not yet been reloaded onto
hardware and spent no additional encoder spawn.

---

## M188 (hardware/source, 2026-08-25): the post-START wait starved VB2 and cut a hole in the first H.264 GOP

The first run of the M187 split build exposed a repeatable user-visible startup
defect: before settling into a clean picture, OBS could show black and the
first few visible pictures could be blurry, smeared, or incomplete. The exact
appearance varied with the connected camera.

The status snapshot was otherwise healthy: one V4L2 user, one `SET_VIC`, a
locked 1920x1080p60 YUV444 input, 465 H.264 access units delivered, 11 dropped,
and zero command timeouts. Ten of those drops formed this contiguous burst:

```
7577.589657  first no-queued-vb2 drop
7577.622176  next drop
...          one access unit about every 33 ms
7577.822966  eighth listed drop
7577.847102  output stage [after START] diagnostic
7577.856314  final drop in the startup burst
```

The source explains the timestamp boundary. `mz0380_start_streaming()` marks
the stream live and calls `mz0380_dma_start()` synchronously from VB2's
`STREAMON` callback. `mz0380_dma_start()` fires opcode `0x06`, allowing the
encoder to produce immediately, but then slept 500 ms and ran the receiver
diagnostic before returning. Userspace cannot finish `VIDIOC_STREAMON`, dequeue
completed buffers, and requeue them during that wait. Once the finite initial
VB2 queue was exhausted, `mz0380_drain_h264_snapshot()` had no destination and
dropped otherwise complete access units. The burst stopped when the diagnostic
finished and STREAMON returned.

This is not torn DMA or an encoder warm-up effect. Every accepted access unit
still passed the duplicated length, slot identity, bounds, and Annex-B prefix
checks. Instead, the deterministic loss cuts a hole in the first inter-frame
reference chain. H.264 decoder concealment then accounts for camera-dependent
black, smeared, or blurry output until a clean random-access point/reference
chain is available.

The synchronous post-START sleep and diagnostic are removed. The earlier
pre-START receiver diagnostic remains because it supplies the cached input
colour-space value used immediately by CSC programming. `STREAMON` now returns
as soon as START succeeds and the drain is armed; any future post-start
observation must run asynchronously. The remaining isolated eleventh drop at
7593.221571 is not part of the startup burst and should be monitored separately.

This change is host-only until the next load. The validation discriminator is
simple: one OBS open should produce no regular approximately 33 ms startup drop
burst, and the first displayed pictures should decode cleanly.

---

## M189 (hardware/source, 2026-08-25): startup loss is gone with clean kill; live timing queries caused a separate five-frame gap

After a real slot-power reset, the M188 build was loaded with the normal fw7
H.264 sequence but `FASTKILL=0`. Probe was healthy, firmware 1.11 answered,
legacy INTx 40 came up, and OBS opened the node exactly once. The first status
snapshot reported:

```
SET_VIC sent   : 1
h264 frames    : 1482 delivered, 0 dropped
command timeouts: 0
signal         : locked 1920x1080p60 YUV444
```

This closes M188's primary discriminator: removing the synchronous 500 ms
post-START wait eliminated the deterministic startup drop burst. It also proves
that tinyvenc7's continuous window-1 H.264 path works with the firmware's clean
encoder-replacement branch (`fast_kill=0`), rather than requiring Windows'
SIGKILL branch. Keep `FASTKILL=0` in the normal recipe.

A later status snapshot found 1629 delivered and five dropped. Those five were
one contiguous approximately 33 ms-spaced group at 2498.947507 through
2499.082578, immediately after:

```
2498.680961 drained stale EVENT=0x00000001 before command 0x1a
```

Opcode `0x1a` is the MST3367 mailbox-I2C register read. The proc status reader
itself performs only cached/MMIO state reads; the live command comes from a
V4L2 signal/timing query. `ENUM_INPUT` already deliberately avoids receiver
I2C while streaming, but `VIDIOC_QUERY_DV_TIMINGS` still called
`mz0380_query_signal()` unconditionally. OBS can issue that ioctl while active,
and its serialized receiver measurement pauses userspace buffer recycling long
enough for the finite VB2 queue to empty.

`mz0380_query_dv_timings()` now returns `dev->detected_timings` while streaming,
or `-ENOLCK` from cached state if the stream has no known lock. This is also the
only semantically consistent result: the running encoder cannot adopt new
geometry until a new stream configuration. Outside streaming, the ioctl keeps
performing a live receiver measurement, so negotiation and source discovery
remain unchanged. The follow-up is host-only until the next module load.

The single `slot 3 not complete` line containing the untouched `0xcccccccc`
poison values is the already-understood pre-first-write observation from M184;
it did not increment the dropped-frame count and is unrelated to both gaps.

---

## M190 (host-only, 2026-08-25): frame-silence HDMI hotplug recovery and clean-IDR delivery

Moving the camera cable from the Elgato to a USB capture device and back could
leave OBS frozen on its last Elgato picture. This exposed a real missing
lifecycle rather than an ordinary decoder colour/range problem.

The driver had every ingredient except a reconnect trigger. Source-change
event support existed, but there was no background signal monitor. The
`MZ0380_IRQ_SIGNAL_CHANGE` bit was defined but never interpreted by the IRQ
handler and `irq_signal_count` was never incremented. Live timing queries had
just been made cache-only to prevent receiver mailbox I2C from starving VB2.
Finally, `mst3367_set_auto_position()` refused to re-arm acquisition whenever
`dev->streaming` was true. HDMI loss stops access units but does not make VB2
call STREAMOFF, so that condition remains true forever.

Recovery is now triggered by absence of valid H.264 VCL access units. Healthy
video merely refreshes a jiffies timestamp and the delayed worker returns
without any receiver access. After 1500 ms of silence the worker marks the
stream recovering, making the sole narrow exception to the active-stream
AUTO_POSITION guard, and performs uncached stable-lock detection. A failed
attempt leaves acquisition armed and retries after 500 ms. A successful
same-mode lock lets the existing VIC/encoder resume. Loss and same-mode return
are intentionally not reported as V4L2 source changes because that can make a
client restart V4L2 and spend another scarce SET_VIC spawn. A stable changed
mode is reported, but no hidden SET_VIC is issued.

The drain also classifies Annex-B NAL units. At initial STREAMON and after a
frame gap, metadata-only access units may pass but dependent VCL pictures are
discarded until an IDR arrives. The IDR is marked with
`V4L2_BUF_FLAG_KEYFRAME`. This prevents a new or resumed OBS decoder from being
fed the middle of an old GOP, which accounts for both persistent freeze and
the earlier camera-dependent smeared first pictures.

This is host-build-verified with clang/lld against Linux 7.2.0-1-cachyos,
including `W=1`. It is not hardware-verified. It handles same-mode cable
round trips without a new encoder spawn; changed-mode in-place card
reconfiguration remains part of the persistent-encoder work.

---

## M191 (host-only, 2026-08-25): persistent card pipeline removes SET_VIC from ordinary OBS restarts

The experimental `stop_on_streamoff=0,setvic_once=1` pair did not constitute
persistence. It suppressed STOP and the next SET_VIC, but the second STREAMON
still replayed SET_ENC, SET_BUF and START into a tinyvenc process that had
already advanced beyond those command states. Hardware had shown those later
commands timing out. Persistence therefore requires a separate attach path,
not a selectively shortened start sequence.

`pipeline_running` now represents the card-side VIC/tinyvenc/DMA lifetime;
`streaming` represents only whether a VB2 userspace consumer is attached. The
first STREAMON follows the proven Windows sequence and records the configured
source/output geometry. On persistent STREAMOFF, the driver publishes VB2
detachment and synchronizes with any drain already copying a buffer, but does
not flush the completion FIFO or send STOP. New completions continue to be
validated and consumed, the window-1 token advances, and the batch drain sends
the normal whole-word `enc_stat` ACK. This prevents the encoder from parking
after its four host buffers fill while OBS is closed.

The detached drain caches exact Annex-B SPS and PPS NAL units in a bounded
4096-byte module-owned buffer and counts complete discarded access units
separately. Reattachment checks output compatibility, resets only the V4L2
sequence/decoder boundary, marks VB2 attached, and sends no card command at
all. Dependent VCL pictures remain held until IDR. A parameter-set-free IDR is
returned as cached SPS/PPS concatenated with the original access unit and is
marked KEYFRAME, allowing a newly constructed OBS decoder to join a pipeline
which may have been running for hours.

Receiver writes and live timing queries now use `pipeline_running`, not VB2
attachment, as the protection boundary: closing OBS must not make diagnostics
think it is safe to retime a still-running receiver. HDMI recovery remains
dormant while detached and starts when a consumer attaches; the continuously
drained H.264 timestamp distinguishes a healthy producer immediately.

A true stable timing change sets `pipeline_reconfigure_pending` and queues one
V4L2 source-change event without restarting in the worker. On the next
attachment, the old encoder gets the clean all-channel STOP and the ordinary
full start creates exactly one correctly configured replacement. This bounds
spawns to actual input-mode changes instead of client retry count.

Normal module removal is the forced final-stop boundary regardless of the old
`stop_on_streamoff` diagnostic knob. STOP precedes event flush and IRQ release;
PCI bus mastering is still cleared before persistent mappings and the SPS/PPS
cache are freed.

The integrated source builds with clang/lld `W=1` against Linux
7.2.0-1-cachyos. `git diff --check`, script syntax checks, and module-parameter
inspection pass. Hardware validation is pending. Success requires repeated OBS
close/open cycles and a same-mode HDMI round trip to leave the SET_VIC count at
one while attachment and detached-discard counters advance.

This prevents the known wedge during normal operation; recovering a card that
is already mailbox-deaf remains contingent on hardware validation of its
advertised PCI `pm` and `bus` reset methods. If both fail to reset the onboard
ARM SoC, software cannot guarantee recovery from that pre-existing state
without a slot power controller.

---

## M192 (hardware, 2026-08-25): persistent pipeline validated through three attachments, HDMI recovery, and final unload

The M191 lifecycle passed its complete hardware discriminator on a fresh card
cycle. The first OBS STREAMON sent one `SET_VIC` for a locked 1920x1080p30
source. Two later VB2 attachments logged `no card command sent`; attachment
count reached three while `SET_VIC sent` remained exactly one. Detached access
units continued rotating: the status snapshot counted 102 complete H.264
access units discarded while detached, proving the completion/ACK path stayed
live without a userspace buffer owner.

Both HDMI cable round trips entered recovery after encoder silence, reacquired
the same 1920x1080p30 timing, and returned at clean IDRs containing SPS/PPS.
The final status showed 926 delivered access units, 35 host-side drops, 102
detached discards, and zero command timeouts. The drops therefore did not
represent a card respawn or mailbox failure.

After OBS closed, module removal produced the exact ownership boundary the
design required:

```
final pipeline stop: STOP_STREAMING(all channels) ret=0 after 3 userspace
attachment(s) and 1 SET_VIC spawn(s)
```

The script committed a power-cycle tally of one. A second unload correctly
reported that the module was not loaded. M191 is no longer host-only: ordinary
OBS close/reopen, detached drain/ACK, same-mode HDMI recovery, clean-IDR
reattachment, and the one final STOP are hardware-proven together.

The run also exposed a Linux 7.2 compatibility warning: delayed recovery work
was queued on the deprecated `system_wq` alias. It did not affect this result,
but the next source feature-probes and selects `system_dfl_wq` where available.

---

## M193 (host-only, 2026-08-25): zero-spawn NO SIGNAL presentation and independent lock monitoring

M114 already proved the essential negative fact: when the MST3367 is entirely
unlocked, firmware writes no raw frame and no H.264 access unit. The proposed
plan to cache a card-generated no-source splash could therefore never work.
The Linux state now uses a host-owned placeholder, matching what the Windows
software stack most likely did rather than attributing its UI overlay to card
DMA.

The module embeds one 3207-byte, self-contained Annex-B access unit: AUD, High
Profile level-4.2 1920x1080 SPS/PPS, and one IDR containing a dark NO SIGNAL
picture. While HDMI is absent it is copied into queued VB2 buffers at a
clamped low cadence (default two fps), with monotonic timestamps and KEYFRAME
metadata. It requires no mailbox transaction, DMA remap, encoder process, or
card completion. `stream_without_signal=1` now defaults on for the persistent
H.264 path.

The first no-source STREAMON takes a cheap receiver lock-byte sample and
returns immediately in placeholder-only state. It deliberately does not send
`SET_VIC`; the scarce spawn count remains zero until a coherent HDMI timing is
measured. A failed deferred start is latched so the monitor cannot create a
respawn loop. Closing/reopening userspace is the explicit retry boundary.

The HDMI worker no longer infers source presence from encoded-frame silence.
Every 500 ms by default it reads only MST3367 bank 0 register 0x55. On loss it
activates the placeholder, permits one AUTO_POSITION re-arm, and leaves an
existing persistent encoder draining and ACKing. On full lock it performs the
existing complete coherent timing measurement. With no pipeline it starts the
first one once; with a matching pipeline it waits for its next clean IDR; with
a changed timing it keeps the placeholder active and defers one controlled
replacement to the next userspace attachment.

Placeholder and live delivery share a mutex. While the placeholder is active,
card metadata, dependent pictures, stale IDRs, and frames seen before receiver
validation are consumed/ACKed but cannot reach VB2. The switch occurs only at
a receiver-proven IDR, with cached card SPS/PPS prepended if needed. Persistent
reattachment also starts in this validation state so a source change performed
while OBS was closed cannot be mistaken for the old mode.

Status now separates placeholder IDRs and cadence misses from card H.264
delivered/dropped/detached counts. `signal_monitor_ms` and `no_signal_fps` are
module parameters exposed as `SIGMON` and `NOSIGFPS` by the live script. The
workqueue selection uses a header probe: `system_dfl_wq` on kernels which
declare it, `system_wq` on older builds.

The integrated module builds cleanly with clang/lld and `W=1` against Linux
7.2.0-1-cachyos and 6.18.42-1-cachyos-lts. The cross-build also found and
removed one stale `static` GPIO forward declaration left by the earlier source
split. The final on-disk module was rebuilt for the running 7.2 kernel.
`git diff --check`, shell syntax, module parameter inspection, byte-exact
header extraction, and repeated decode of five concatenated placeholder IDRs
all pass. Hardware validation is pending and must begin with HDMI absent: OBS
should receive the placeholder with zero SET_VIC, then a connected fixed source
should spend exactly one.

---

## M194 (mixed hardware/host, 2026-08-25): coherent receiver lock is not proof of an H.264 producer

The first M193 hardware attempt did not enter the placeholder-only path. At
STREAMON the MST3367 reported a coherent 1920x1080p30 mode (`htot=2200`,
`vtot=1125`, `hper=337`, `vper=299`) with R55=0x7f. The driver consequently
sent one SET_VIC and left the placeholder inactive. The resulting card
pipeline produced no access unit at all: status showed zero H.264 delivered,
dropped, suppressed, or detached frames, zero placeholder IDRs, and zero
command timeouts. OBS remained completely black.

This result invalidates receiver state as a sufficient sole discriminator.
R55 and the timing block can be fully asserted and internally coherent while
the configured H.264 producer remains silent. It does not yet identify whether
the receiver state was stale or whether another card-side start failure
occurred; the preserved detailed stream-start log is still required for that
distinction.

The unload dump adds one strong negative result: all four dedicated window-1
buffers were still byte-for-byte 0xcc poison. Each had zero written extent,
zero sampled pages touched, and zero delivered access units. The failure was
therefore before VB2 delivery and before H.264 decoding; no encoded producer
ever wrote host memory. The detailed start log could not be preserved because
`mz0380-live.sh load` clears dmesg and the new 500 ms monitor's repeated
mailbox-I2C commands subsequently rolled the earlier lines out of the ring.

The host follow-up restores producer silence as the gate for receiver access
once a card pipeline exists. MST3367 reads are firmware-mailbox commands, and
M74 already established that receiver transactions during capture perturb the
stream. A healthy recent H.264 access unit therefore reschedules the worker
without touching the receiver. Once a running pipeline has produced nothing
for `hotplug_stall_ms`, the worker activates the host NO SIGNAL presentation,
marks delivery as waiting for a validated IDR, and forces MST3367 AUTO_POSITION
acquisition even if R55 still claims lock. It performs no SET_VIC, STOP, or
encoder replacement. With no pipeline and during persistent reattachment,
receiver polling remains enabled because it is needed to discover or validate
the source before exposing live data.

The follow-up builds cleanly against Linux 6.18 LTS and the running 7.2 kernel
with clang/lld and `W=1`; its on-disk module has the correct 7.2 vermagic.
Hardware validation remains pending.

---

## M195 (host, 2026-08-25): placeholder changed from synthetic art to the exact vendor splash

The first placeholder was functionally correct but visibly not the card's
original NO SIGNAL picture. The distinction in the existing evidence matters:
the H.264 window emits no access unit on receiver unlock (M114), but the older
raw standby path did expose the card's fixed splash (M126/M127b). The host must
therefore encode a saved copy; it cannot request the picture from a silent
H.264 producer.

The original is `NOSG_LOGO_Y`, a 320x240 luma asset at tinyvenc5 ELF virtual
address 0x6b10c / file offset 0x5b10c. The local firmware dump produced the
previously recorded identity digest
`dfce4efd5139298f544d23473f85a42fb7115a3c5e4ba65b71c070d59883a30b`.
`mz0380-no-signal-generate.py` now refuses any non-matching binary, reconstructs
the hardware-captured 1920x1080 I420 canvas (Y=0x11, U=V=0x80), blits the asset
at its exact centred coordinates (800,420), and encodes one self-contained
Annex-B access unit. The generated stream contains AUD/SPS/PPS/IDR and probes
as 1920x1080 yuv420p High Profile level 4.2.

The user's simultaneous report that live streaming looked laggy remains a
separate open issue. The claimed unplugged and plugged dmesg captures were not
present in the received message, so there is not yet evidence to distinguish a
placeholder that failed to deactivate, repeated recovery transitions, VB2
drops, or genuinely low live-card cadence.

A read-only snapshot of the still-loaded module resolves part of that
ambiguity. The card count was frozen at 1121 delivered H.264 access units while
the placeholder count continued from 2272 to 2282 during an approximately
two-second sample; the state remained `pipeline: running`, `no signal: active`,
and `source: unlocked`. The perceived current output was therefore placeholder
cadence, not a low-rate live encoder. The card had produced real video earlier
and subsequently stopped; the omitted reconnect log is still needed to say
why.

The sample also found a host cadence defect. `mz0380_no_signal_activate()` was
called on every 500 ms unlocked retry and unconditionally moved the already
scheduled placeholder work to delay zero. That superimposed recovery retries
on the configured two-fps presentation. Activation now schedules immediate
work only on the inactive-to-active transition; the work item alone owns later
cadence. Status also reports whether recovery is active and the age of the last
card H.264 access unit.

---

## M196 (hardware, 2026-08-25, **zero-mode conclusion superseded by M204**): clean placeholder/live boundary; 30 Hz input exposes the divisor-2 cadence

The retained recovery log proves the state transition works. After a long
unplugged interval, recovery attempt 1526 measured a coherent 1920x1080p30
source at 4988.568762. At 4990.893041 the card supplied an IDR containing
SPS/PPS; the driver synchronized delivery and ended the NO SIGNAL presentation
at that exact decoder boundary. There was no SET_VIC respawn.

At 5002.691573 the producer had again been silent long enough to justify a
receiver read, and R55 was actually unlocked. The driver returned to the host
placeholder and re-armed acquisition. The operator confirmed the HDMI cable
remained plugged, so this is a real connected-link flap: either the source
stopped transmitting or the MST3367 lost the link. It is not an OBS or
placeholder pacing verdict.

The live interval also explains the reported motion lag. The delivered-card
counter advanced by approximately 153 access units over the 11.8-second live
window, about 13 fps after transition overhead. The input was 30 fps and
tinyvenc7's fastest valid schedule is `(input_counter % 2) == 1`, so its ceiling
is 15 encoded fps. Divisor 1 can never leave remainder one and produces no
H.264; divisor zero selects an inaccessible schedule bitmap. The earlier
hardware-proven 30 encoded fps result used a 60 fps HDMI input. Diagnostics and
the live script now state both source and predicted encoded cadence explicitly
instead of implying that a 30 fps input remains 30 fps.

The source later recovered on its own at 5533.670436 (attempt 1021), followed
by a clean IDR and placeholder exit at 5536.199792. Two manual one-second HPD
pulses happened only afterward, at 5547.921 and 5553.906, so they cannot be
credited with restoring the link. They did prove the active-low line and
receiver bit both followed deassert/assert correctly, consumed no SET_VIC, and
did not destabilize the recovered stream. A later four-second counter sample
advanced live delivery from 2326 to 2393 with the placeholder inactive and the
driver's persistent spawn counter at one.

`mz0380-live.sh status` incorrectly printed `SET_VIC sent: 0` during that run
because it counted matching dmesg lines after the original start line had
rolled out of the ring. Status now reads the driver's `enc spawns` lifetime
counter from `/proc/mz0380-state`, retaining dmesg only as a legacy fallback.

---

## M197 (host, 2026-08-25): live-loader cleanup regression fixed

An explicit `mz0380-live.sh unload` now removes `mz0380.ko` and hidden
`.module-common.o` as well as ordinary objects, preventing sudo-owned build
outputs from blocking a later non-root link. The first implementation also ran
that cleanup during `load`, after the module had already been built and its
path resolved; the following `insmod` consequently failed with ENOENT.

`do_unload` now distinguishes the internal pre-load stop from the explicit
user unload. The former preserves the just-built artifacts, while the latter
performs the ownership cleanup. The corrected script passes `bash -n` and
`git diff --check`, and the module was rebuilt warning-free with clang/lld and
`W=1` for the running Linux 7.2 kernel. The failed load never reached the real
module insertion or STREAMON and therefore consumed no SET_VIC spawn.

---

## M198 (hardware, 2026-08-25): corrected loader and post-HPD live stream validated

The corrected live loader completed its smoke test, inserted the Linux 7.2
module, and registered `/dev/video2`. Two manual one-second HPD pulses followed.
The retained driver state afterward showed one SET_VIC spawn across three VB2
attachments, a running attached pipeline, zero live-frame drops, an inactive
placeholder, zero placeholder cadence misses, and fresh card H.264 activity.

A direct four-second sample advanced delivered live access units from 744 to
810 (+66) while recovery remained idle and the spawn counter stayed at one.
This is the expected approximately 15-fps output for a 30-fps HDMI input with
tinyvenc7 divisor 2. The pulses therefore neither respawned nor destabilized
the encoder. Because they were issued while the link was already producing,
this run does not establish whether HPD shortens a connected-but-unlocked
outage.

---

## M199 (hardware, 2026-08-25): exact vendor NO SIGNAL and cable round-trip validated

The operator visually confirmed that the host placeholder now matches the
card's original splash: the centred segmented spinner and `NO SIGNAL` artwork
on the original black canvas. This closes the visual mismatch from the first
synthetic placeholder.

The accompanying log proves the complete persistent cable-loss lifecycle. At
6521.516886 the receiver lost lock and the driver activated the host H.264
placeholder without sending a card command. It delivered 56 placeholder IDRs
with zero cadence misses, consistent with approximately 25 seconds at two fps.
At 6544.102026 recovery attempt 44 measured the same 1920x1080p30 mode; the
existing encoder remained unchanged while the placeholder stayed visible. A
clean live IDR containing SPS/PPS arrived at 6546.584270, and only then did the
driver end the NO SIGNAL presentation.

Final state remained `pipeline: running`, VB2 attached, recovery idle, with
zero live drops and exactly one SET_VIC across three userspace attachments.
Live delivery continued afterward (2629 access units at the later snapshot,
with the newest card unit 15 ms old). Thus unplug, continuous original splash,
same-mode reconnect, and decoder-safe live resumption are all hardware-
validated without an encoder respawn.

---

## M200 (host, 2026-08-25): Windows-exact independent raw-bank diagnostic implemented

The successful NO SIGNAL work does not close the user's 60-fps requirement.
The current tinyvenc7 modulo path still produces at most 30 H.264 fps from a
60-Hz input (and this last run negotiated a 30-Hz input, so it produced about
15 fps). The next trace-backed lead remains Windows' unimplemented raw-buffer
topology, not another divisor value.

`raw_bank_probe=1` now allocates eight independent buffers exactly as the saved
Windows HD60 Pro branch does: four for opcode 0x02 and four different buffers
for opcode 0x08, each advertising `0x466000` bytes. Bank 0 is poisoned with
0xa5 and bank 1 with 0x5a. The buffers use individually allocated pages behind
contiguous IOMMU IOVAs, avoiding eight fragile order-11 allocations and their
rounded 8-MiB backing cost. The proven H.264 window-1 ring remains separate and
the default path is unchanged.

The diagnostic registers both raw banks plus the H.264 bank before SET_VIC and
again after the spawn, then reports each raw buffer's final written extent,
sampled touched-page count, poison identity, and first 16 bytes alongside the
existing final EVENT/token snapshot. A bounded 1080p60 run can therefore
distinguish a full raw frame from the known 16-byte preview record or an
untouched bank without changing the V4L2 delivery format.

The source passes clang/lld `W=1` builds on Linux 6.18 LTS and Linux 7.2;
`mz0380.ko` was finally rebuilt for the running 7.2 kernel. `bash -n` and
`git diff --check` also pass. During cross-validation, a direct combined
kbuild `clean modules` invocation temporarily replaced the external-module
Makefile with kbuild's generated wrapper. It was stopped immediately and the
exact pre-command working Makefile (refactored object list plus both feature
probes) was restored and diff-checked before using the project's safe
`objclean` target.

---

## M201 (hardware/host, 2026-08-25): apparent reconnect freeze was a stale transient-mode latch

The first M200 hardware run did not wedge the card. It retained fresh H.264
access units (2--38 ms old in status), zero command timeouts, one SET_VIC, and
a clean final STOP. The reason OBS remained on NO SIGNAL is visible in the
counters: only 120 live units reached userspace while 2186 healthy units were
suppressed behind the placeholder.

The receiver measured 1080p30 at pipeline creation, transiently reported
1080p60 during reconnect, and then repeatedly settled back to 1080p30. The
60-Hz sample correctly set `pipeline_reconfigure_pending`, but the recovery
worker stopped validating timing once healthy H.264 resumed and never cleared
the flag when the original mode returned. Every subsequent clean IDR was
therefore deliberately suppressed forever. The card was producing; the host
state machine made it look frozen.

Recovery now keeps timing validation active while a replacement is pending.
If the live timing matches the existing pipeline again, it cancels the stale
replacement, logs that decision, and switches from the placeholder at the next
clean IDR. A genuinely changed stable mode remains pending for the controlled
next-attachment replacement. `mz0380-live.sh status` also once again prints the
pipeline line (including `replacement pending`) by accepting the proc file's
aligned whitespace.

The raw-bank result itself was negative but bounded for the tested 30-Hz
configuration: all four opcode-0x02 buffers contained exactly 16 changed bytes,
while all four independent opcode-0x08 buffers remained completely poisoned
and untouched. The run began with SET_VIC at 1080p30, so this does not settle
whether a pipeline configured from the outset at 1080p60 exposes another path.

The state-machine fix passes clang/lld `W=1` on Linux 6.18 LTS and Linux 7.2;
the final module targets the running 7.2 kernel. Shell syntax and diff checks
also pass.

---

## M202 (hardware, 2026-08-25): transient-mode cancellation validated on a 60-Hz pipeline

The corrected normal path started once at 1920x1080p60 and delivered at the
expected divisor-2 rate of approximately 30 H.264 fps. Before cable loss it had
delivered 781 access units with zero drops, zero suppression, one attachment,
one SET_VIC, and no command timeouts.

On reconnect the receiver twice measured a transient 1080p30 mode, setting the
controlled replacement pending as intended. The next coherent measurement
returned to the persistent 1080p60 mode. The new path logged `cancelled
transient replacement`, kept the existing encoder, accepted a clean SPS/PPS-
bearing IDR 274 ms later, and ended the NO SIGNAL presentation. The later
snapshot remained healthy: 1995 live access units delivered, zero drops, only
27 units suppressed during the handoff, placeholder inactive, recovery idle,
and one SET_VIC.

This closes the M201 stale-latch regression. The source's 60-to-30-to-60
negotiation is real receiver behavior, but it no longer manufactures a frozen
placeholder or an encoder respawn.

One startup diagnostic printed all three H.264 header fields as decimal
3435973836, which is exactly `0xcccccccc`: the untouched buffer poison. A raw-
preview event can name a window-1 slot before encoded DMA begins. The drain now
silently leaves such a wholly untouched slot unconsumed for its later encoded
completion; genuinely partial or malformed headers retain the existing
ratelimited diagnostic.

---

## M203 (hardware, 2026-08-26): independent raw-bank lead closed at a confirmed 1080p60 SET_VIC

The required bounded M200 discriminator has now run with the encoder configured
at 1920x1080p60 from the outset. The receiver reported coherent 2200x1125
timings and R55=0x7f, and SET_VIC explicitly logged
`input=1920x1080p@60`. The simultaneous divisor-2 H.264 control remained
healthy: the later live snapshot showed 949 delivered access units, zero
drops, an inactive placeholder, fresh producer activity, one attachment, one
SET_VIC, and no command timeouts. Module removal completed one successful final
STOP with `fifo_drops=0`.

Neither Windows-style raw bank carried a frame. All four opcode-0x02 bank-0
buffers changed over exactly `0x10` bytes and retained poison beyond that
16-byte preview/status record. All four independent opcode-0x08 bank-1 buffers
had extent zero, zero sampled pages touched, and their 0x5a poison remained
byte-exact at the head. This reproduces the earlier 30-Hz shape under the
previously missing 60-Hz precondition.

The independent opcode-0x02/opcode-0x08 topology is therefore not a hidden raw
60-fps video path for this firmware configuration. Do not spend further encoder
spawns repeating RAWBANKS. True 60-fps delivery remains unresolved and returns
to static firmware/Windows-driver analysis for a different output path or
all-frame mode. The validated user-facing path remains divisor-2 H.264: about
30 encoded fps from this 60-fps HDMI input.

---

## M204 (static/source, 2026-08-26): H.264's all-frame bitmap is reachable through SET_ENC_PARAMS

M165/M178 made two linked interpretation errors. First, the bitmap writer
census searched for direct stores and missed calls to the two exported helper
functions. Second, SET_ENC_PARAMS payload bytes 20 and 21 were named `qp_min`
and `qp_max` in the host driver even though tinyvenc7's own command printf
names them `skip` and `avg`.

The relevant tinyvenc7 chain is now complete:

1. SET_ENC_PARAMS mask bits 7/8 copy command bytes 20/21 to stream offsets
   `+0x18/+0x19` as skip/average frame-rate controls.
2. When both values are zero, `h264_param_processing()` at `0xd4e8` calls
   `tiny_calculate_skip_fps(input_fps, 0, stream+0x30, stream+0x38, false)`.
3. The helper at `0x13c2c` sets bit positions `0, skip+1, 2*(skip+1), ...` below
   `input_fps`. With skip zero and a 60-Hz input this is bits 0 through 59: one
   selected bit for every input frame.
4. `vcap_handler` at `0x12668` uses the old modulo path only when
   `stream+0x18` is non-zero. With zero it reads the 128-bit bitmap at
   `stream+0x30..+0x3f`; a selected bit reaches
   `TK_H264Enc_ProcessOneFrame`.

No opcode `0x32`, alternate `ep.ko`, Windows installer, or card firmware upload
is needed. The current hardware-proven value `h264_frame_divisor=2` is in fact
`skip=2`, explaining the measured 30 fps. Value one remains invalid because
`counter % 1` can never equal one. Value zero is now admitted as an opt-in
all-frame diagnostic and clears both skip and avg. V4L2 and `/proc` reporting
describe it as source-rate output; the proven default remains two until the
card confirms the prediction.

The next bounded hardware run is therefore one normal H.264 load, with
RAWBANKS omitted and `H264DIVISOR=0`. At a confirmed 1080p60 source, open
exactly one OBS V4L2 source for roughly ten seconds, then close OBS and unload
once. Success is approximately 60 delivered access units per second with no
producer-paced drops or command timeouts. Failure is still informative and
must preserve the final SET_ENC, state, STOP, and H.264 cadence lines.

---

## M205 (hardware/source, 2026-08-26): the all-frame bitmap delivers true 1080p60 H.264

The bounded M204 test succeeded. SET_VIC started one tinyvenc7 process at a
coherent 1920x1080p60 input (`htot=2200`, `vtot=1125`, `hper=674`, `vper=599`,
`R55=0x7f`). Both SET_ENC_PARAMS transactions explicitly logged
`skip=0, avg=0, tinyvenc7 schedule=all-frame bitmap` and completed without a
timeout.

The live snapshots were healthy:

- 909 delivered and 9 dropped at the first sample;
- 1,663 delivered and the same 9 dropped at the second sample;
- one userspace attachment and one SET_VIC throughout;
- placeholder inactive, recovery idle, and fresh H.264 activity 2--5 ms old;
- zero command timeouts.

At final removal the pipeline had run for approximately 73 seconds and
reported 4,339 frame events, about 59--60 events per second. It stopped once
with `fifo_drops=0` and a clean STOP response. The nine H.264 drops occurred in
two short bursts whose logs explicitly say `no queued vb2 buffer`; the producer
continued at frame cadence and the count did not grow afterward. They are
userspace buffer-availability gaps, not an encoder ceiling.

This closes the true-60-fps requirement. No raw-bank route, opcode `0x32`,
alternate `ep.ko`, Elgato Windows installer, or firmware upload is involved.
SET_ENC's zero `skip/avg` values make tinyvenc7 use the schedule it constructs
locally, selecting every 60-Hz input frame. The module parameter default and
recommended loader profile are now `h264_frame_divisor=0`/`H264DIVISOR=0`.
Value 2 remains a validated 30-fps fallback; value 1 remains invalid.

---

## M206 (hardware/product requirement, 2026-08-26): long 60-fps run passes; broad camera compatibility remains open

A second all-frame run independently sustained the result. SET_VIC again
started at confirmed 1920x1080p60, both SET_ENC transactions carried
`skip=0, avg=0`, and final stop reported 16,754 frame events over approximately
279.7 seconds: about 59.9 events per second. The persistent pipeline served two
userspace attachments with one SET_VIC spawn, then stopped cleanly with zero
FIFO drops. This is stronger duration and reattachment validation for M205.

The user records an additional Windows-parity requirement: Elgato's Windows
driver appears as a camera that can be selected broadly across applications.
The Linux module does register a standard V4L2 `/dev/videoN`, but the working
live profile advertises compressed H.264. OBS accepts it directly; webcam-only
applications that require raw YUYV/NV12 are not guaranteed to enumerate or
open it. That is a compatibility gap, not a failure of the validated capture
transport.

The immediate bridge is one OBS V4L2 source feeding OBS Virtual Camera through
Linux `v4l2loopback`. Native parity requires either a safe raw hardware output
path or a maintained userspace H.264-decode/virtual-camera service. H.264
decoding must not be added to the kernel driver.

---

## M207 (product architecture correction, 2026-08-26): Windows parity means native negotiated raw and encoded formats

The user rejects a mandatory OBS/FFmpeg loopback bridge as the product design,
correctly. Re-reading the saved live DirectShow probe makes the target
unambiguous: both Windows output pins advertise YUY2, YV12, NV12, RGB24, RGB32,
main H.264, and 960x540 substream H.264. `GetFormat` reported YUY2 1920x1080 at
30 fps as the current type, and YUY2 capabilities include 50 and 59.94 fps.
The card configuration independently documents hardware output format 1 as
YUV420 and 2 as YUV422. RGB24/32 remain likely host conversions.

Therefore `h264_probe` and the one-format `mz0380_current_pixelformat()` model
are reverse-engineering scaffolding, not the final interface. The production
Linux shape is:

1. PCI modalias loads the installed module at boot; probe initializes the card
   and registers stable V4L2/ALSA interfaces without starting or selecting a
   video codec.
2. `VIDIOC_ENUM_FMT` exposes only hardware paths that have been validated.
3. `VIDIOC_TRY_FMT`/`VIDIOC_S_FMT` stores YUYV/NV12/YV12/H.264 selection while
   the queue is idle.
4. STREAMON programs the corresponding raw or encoded card/DMA path. H.264
   controls are meaningful only for an encoded selection.
5. STREAMOFF stops or persistently detaches according to that selected path;
   module removal owns the final card stop.

Do not advertise YUYV prematurely. M203 proved that the current tinyvenc7
sequence writes only 16-byte records to the candidate raw bank even though
Windows' YUY2 sample is 4,147,200 bytes. Identify the Windows pin-selection and
raw delivery path before deciding whether YUY2 is native DMA or a host
conversion. Once a continuous raw source is known, implement native V4L2
negotiation and retire `h264_probe` from the user-facing workflow. A loopback
bridge remains an optional workaround only; it is not Windows parity.

---

## M208 (Windows `.sys` static, 2026-08-26): format choice is per pin, and Windows performs raw conversion

The retail driver's camera interface has now been followed from its KS filter
descriptor into the live callbacks. The filter descriptor at `0x1402f5890`
contains two `0x88`-byte video pin descriptors. Both point at the same table at
`0x1402f69c0` and each advertises `0x140` (320) data ranges. Their shared
intersection handler is `0x14021d880`; the pin dispatch uses create callback
`0x14021c4a0` and state callback `0x14021d7f0`.

The create callback parses the negotiated width, height, frame interval, bit
depth and compression FourCC from the application's media type. It stores raw
formats in the base stream-slot group. A 24-bit H.264/HEVC selection uses the
base slot plus eight, while X264/X265 uses the base slot plus sixteen. It also
allocates width/height-sized working surfaces for raw selections. Thus the
Windows binary independently confirms the product architecture in M207: codec
and pixel format are chosen when an application opens/configures a pin, not at
driver load.

The base raw delivery routine at `0x140280a98` is a large host-side processing
path. It branches on 12-, 16-, 24- and 32-bit output and contains explicit YV12
(`0x32315659`) and NV12 (`0x3231564e`) handling, scaling, plane copies and packed
output conversion helpers. Therefore the DirectShow default of YUY2 does **not**
establish that PCIe DMA delivers packed YUY2. Windows can advertise YUY2 after
converting a planar native surface inside its kernel driver.

This refines, rather than removes, the raw-path blocker. Linux has already
received one complete 1920x1080 planar I420 frame (`3,110,400` bytes) through
the older tinyvenc5 route, but that producer does not rotate continuously. The
M203 tinyvenc7 diagnostic found 16-byte records in opcode `0x02` and no writes
in opcode `0x08`; it did not establish where Windows obtains the continuously
rotating planar surfaces consumed by its raw handler. The next static target is
the producer/source-buffer selection inside `0x140280a98` and the corresponding
Windows buffer-bank state, not an assumed one-command switch to packed YUY2.

Do not expose a fake YUYV capability yet. First prove a continuous native raw
surface and its actual layout. Then V4L2 can advertise that native format and,
only if needed for broad camera compatibility, add a deliberate conversion
path. H.264 remains a separate negotiated choice and must not be selected by
module insertion.

---

## M209 (Windows `.sys` static, 2026-08-26): raw DMA is an eight-slot planar I420/YUV422 ring in op02/op08

The source selection inside `0x140280a98` is now closed for this board. The
routine does not read the opcode-`0x04` or opcode-`0x05` allocations, nor a
hidden scratch surface. For the `12ab:0380`, subsystem `1cfa:0006` path it
uses the first byte of the incoming completion/sample record as a slot token.
When all eight large buffers exist it computes `token % 8`; otherwise it uses
`token % 4`. Slots zero through seven select these CPU virtual addresses:

| token | CPU VA field | physical-address field | registration |
|---:|---:|---:|---|
| 0--3 | context `+0x1190..+0x11a8` | `+0x190..+0x1c0` | opcode `0x02`, four `0x466000` buffers |
| 4--7 | context `+0x11b0..+0x11c8` | `+0x1d0..+0x200` | opcode `0x08`, four independent `0x466000` buffers |

`0x14028f278` allocates those CPU and physical-address pairs together with
`MmAllocateContiguousMemorySpecifyCache` and `MmGetPhysicalAddress`.
Registration at `0x14027b62b..0x14027b925` forwards the same physical fields
to op02 and op08. The opcode-`0x04` buffers are the next allocation group
(CPU VAs beginning at context `+0x1290`, physical fields `+0x390`) and opcode
`0x05` is the group after that (`+0x1390` / `+0x590`). Both are allocated at
`0x34bd00`, but neither group is a source for this board's base raw callback.

Each selected op02/op08 slot contains all three native planes contiguously.
Let `W` be the shared capture state's width, `H` its active height plus enabled
VBI lines, and `S = align(W, 16) * H`. The routine derives:

```
Y = slot + 0
U = slot + S
V = slot + S + (fw == 6 ? S / 2 : S / 4)
```

The source order is Y/U/V, not Y/V/U. The equal-size YV12 output branch proves
the distinction: it copies the second source chroma pointer (`V`) into YV12's
first chroma plane, then the first source chroma pointer (`U`) into the second.
The NV12 branch passes the same U-then-V pair to its interleaver. When the
shared SET_VIC `fw` field is six, each chroma plane is `S/2` (planar 4:2:2,
total `2*S`). For every other `fw`, including the Windows 1080p60 choice seven,
each chroma plane is `S/4` (planar 4:2:0/I420, total `3*S/2`). Windows therefore
converts native planar DMA to advertised YUY2 in host code; packed YUY2 is not
the PCIe source layout.

The registered `0x466000` is maximum capacity, not the expected active write
extent. With the bounded test fixed at 1920x1080p60, `fw=7`, and VBI disabled,
`S=0x1fa400`: Y is `0x1fa400` bytes, U is `0x7e900`, V is `0x7e900`, and the
frame ends at `0x2f7600` (3,110,400 bytes). This exactly matches the complete
I420 frame Linux previously received. For reference, `fw=6` at the same active
geometry would end at `0x3f4800`. The larger allocations also accommodate
stride/VBI variants; they must not be mistaken for the per-frame byte count.

The next hardware discriminator is consequently distinct from M203. Add one
opt-in, H.264-disabled raw-start diagnostic that activates the base raw stream
selection, registers eight independently poisoned `0x466000` slots through
op02/op08, keeps VBI zero, sends the confirmed 1080p60 `fw=7` SET_VIC, and does
not register or consume the encoded op04 path. Spend one start only and bound
observation to two seconds or eight completions, whichever comes first.
Success requires repeated completion tokens selecting the op02/op08 ring and
at least eight consecutive `0x2f7600` planar writes with the Y/U/V boundaries
above; one full frame is not continuous capture. Repeating the M203 result
(`0x10` only in op02 and no op08 writes) under this raw-only start closes this
ring for the Linux command sequence. Any other extent is retained once with
its token and plane-boundary samples, then the run stops. Do not spend that
start until the raw-only diagnostic is implemented and dual-kernel build
checked.

This resolves the static questions but does not yet authorize advertising a
raw V4L2 format. Native format negotiation remains gated on repeated full
frames from the bounded raw-only run.

---

## M209 hardware run (2026-08-27): the raw-only start has no producer at all

The bounded raw-only discriminator was spent once, on a fresh card power cycle,
against a live 1080p60 source. Full log: `docs/m209-raw-only-2026-08-27.log`.

The topology behaved exactly as designed. One `SET_VIC` was sent
(`fw=7 in_fmt=6 out_fmt=0 -> M209 raw-only output=1920x1080, bitstreams=1`,
`ret=0`), both banks were registered before it and re-registered after the
spawn (`op 0x02` at IOVA `0x9..0xc00000000`, `op 0x08` at `0xd..0x1000000000`,
four independent `0x466000` buffers each, poison `0xa5`/`0x5a`), no `op 0x04`
window was ever created, and `START_STREAMING(op 0x06)` fired `ret=0`.

Nothing followed. Over the 810 s the node stayed open:

```
stream stop: EVENT[0x30]=00000000 token[0x40]=a5a5a5a5 0x44=a5a5a5a5
             0x48=a5a5a5a5 0x4c=a5a5a5a5 enc[0x50]=00000000
             irq_total=8 frame_events=0 fifo_drops=0
M209 raw V4L2 totals: events=0 exact_frames=0 bad_extents=0
                      consecutive_exact=0 slots_seen=0x00
```

Every one of the eight raw slots ended at `extent=0x0`, `0/1126 sampled pages
touched`, poison intact, `completions=0`. The `irq_total=8` accounts for the
setup command acknowledgements only.

This is a stronger negative than M203, and a different one. M203 at least found
16-byte records in `op 0x02`; here the card never wrote a frame token into
BAR0 `0x40..0x4c` - the seeded `a5a5a5a5` sentinel survived untouched - and
`enc[0x50]` stayed zero. The encoder side never ran. The receiver was not the
problem: `R55=0x7f LOCKED`, HDMI, HDCP absent, input colorspace YUV444, both
before START and at stop.

The result therefore does not close the op02/op08 ring. It closes something
else: **`op 0x06` alone does not start this firmware's producer.** The raw-only
path differs from the working H.264 path in exactly two ways, and only one of
them can explain a dead producer:

1. `op 0x04` is not registered - a missing *sink*, which cannot stop the card
   from generating frames into the two sinks that were registered; and
2. the Windows encoder tail is skipped
   (`mz0380-dma-stream.c` guards `SET_ENC_PARAMS` x2 and `POST_PROC` on
   `!mz0380_raw_bank_probe`).

M82 already established that ep.ko routes `0x2d` (45) and `0x31` (49) to the
same bare `sysfs_notify("epint")` that `0x06` performs, and that Windows never
sends `0x06` on the capture path. The natural reading of this run is that the
wake is not the point: the encoder tail *configures the pipeline that produces
frames*, and the raw planes in `op 0x02`/`op 0x08` are a product of that
pipeline rather than an independent capture path that bypasses it. The single
complete I420 frame Linux received through the older tinyvenc5 route arrived on
a sequence that did send the tail.

One caveat about the stop diagnostics, so it is not misread later: the legacy
`stop buf[0..3]` lines report `1024/1024 sampled pages touched` with all-zero
heads. Those buffers are zero-initialised rather than poisoned, so "touched" is
trivially true for them. They are not evidence of writes.

Harness note: the run wedged the *script*, not the card. `v4l2-ctl` sat in
`DQBUF` on a stream that produced nothing, `kill -INT` did not reap it, and the
unbounded `wait` in the cleanup trap blocked for 13.5 minutes until the operator
interrupted the shell. `mz0380-m209-raw-only.sh` now escalates INT -> TERM ->
KILL with a 1.5 s bound per signal, warns explicitly if the process survives
SIGKILL (a D-state wait inside the driver), saves `dmesg` to
`/tmp/mz0380-m209-dmesg.txt` and chmods all three artifacts world-readable
before teardown, and wraps the unload in `timeout 30`. The card itself was
clean throughout: one SET_VIC spawn, zero mailbox timeouts, and a normal
`final pipeline stop: STOP_STREAMING(all channels) ret=0`.

### M210: implemented, build-checked, unspent

Do not re-run M209 as it stands; its question is answered. The next bounded run
keeps the raw-only sink topology and restore the encoder tail: eight
independently poisoned `op 0x02`/`op 0x08` buffers, no `op 0x04` registration,
confirmed 1080p60 `fw=7`, VBI zero, and `SET_ENC_PARAMS` x2 plus `POST_PROC`
sent exactly as the working H.264 path sends them. That isolates the single
remaining variable.

It is now implemented as `raw_probe_enc_tail` (module parameter, default off,
`RAWTAIL=1` through `mz0380-live.sh`) and driven by
`mz0380-m210-raw-enc-tail.sh`. Three gates changed and nothing else:
`mz0380_stream_configure_encoder()` takes the Windows-exact 1.1.195.0 values
for M210 as well as for `h264_probe`; the encoder-tail block in
`mz0380_dma_start()` now runs when `raw_bank_probe` is paired with the new
knob; and `op 0x06` is suppressed in that case, because Windows does not send
it once the tail is present and M82 showed `0x2d`/`0x31` already perform the
same epint wake. `raw_probe_enc_tail` without `raw_bank_probe` is rejected at
`mz0380_dma_setup()` rather than silently doing nothing, so a spent run can
never be scored against a configuration that was not the intended one. The
harness deadline consequently anchors on the `SET_PREVIEW_PARAMS` log line
instead of the op06 line, and its oracle additionally requires
`SET_ENC_PARAMS` x2, one `SET_PREVIEW_PARAMS`, and zero op06. The driver's
success line still reads `M209 raw discriminator SUCCESS` - it is the shared
raw-completion oracle, not a mislabel.

Both kernels build warning-free (`6.18.42-1-cachyos-lts`, `7.2.0-1-cachyos`).
The start is unspent. If raw planes appear in the banks, the raw surface is a
by-product of the configured encoder pipeline and native V4L2 negotiation can
be built on it. If the banks stay poisoned while the encoder tail is present,
the producer requires the `op 0x04` sink to exist at all, and the next step is a
run with all three windows registered and the raw banks observed alongside a
live H.264 stream - which the current mutual-exclusion check in
`mz0380-dma.c` forbids and would have to be relaxed for that experiment only.

Budget: this run spent one SET_VIC spawn of the 8-18 per-power-cycle range.

---

## M210 attempts 1 and 2 (2026-08-27): rejected at the guard, source was 1080p30

Two M210 starts were attempted and neither reached the card. The raw-only
STREAMON guard rejected both before any command was sent:

```
MST3367 signal: 1920x1080p (htot=2200 vtot=1125 hper=337 vper=299 hact=1920 R55=0x7f)
live input 1920x1080p@30 -> encoder output 1920x1080@30
M209 raw_bank_probe requires live progressive 1920x1080@60, fw=7, ...
                              (got input=1920x1080p@30 fw=7 vic=1920x1080 output=1920x1080 nosg=0)
dma_start failed (-22)
```

`SET_VIC` stayed at zero in both runs, so the encoder spawn budget was
untouched (tally 2) and the M210 question remains open.

The measurement is not a torn sample. `hperiod` is the line rate and `vperiod`
is Hz x10, so 1080p60 reads `hper=674 vper=59x` (67.4 kHz, 59.9 Hz) and these
runs read exactly half: 33.7 kHz and 29.9 Hz, against a correct `vtot=1125`.
33.7 kHz / 1125 lines = 29.96 Hz - self-consistent 1080p30. Both runs produced
byte-identical numbers. The source, a camera, had settled at 1080p30.

The pushed EDID is not the cause. `edid-decode` on
`mz0380-edid-hd60pro.txt` reports `DTD 1: 1920x1080 60.000000 Hz 148.5 MHz` as
the preferred timing and VIC 16 as native; VIC 34 (1080p30) is merely also
advertised, as it is on any CEA-861 1080p EDID.

The operational lesson is the second one this session. `mz0380-live.sh status`
cannot answer "is the source 60 Hz right now" - it greps for a past
`MST3367 signal` line, and the receiver is only measured at STREAMON, so on a
fresh load the section is empty and reads like a healthy no-op.
`mz0380-source-check.sh` now answers it directly: it loads plainly, reads the
receiver live through `QUERY_DV_TIMINGS` (`mz0380_query_signal()` serves that
while the pipeline is stopped, so no `SET_VIC` is sent), decodes the vperiod
units, and exits nonzero unless the source is 1920x1080p60. Gate every bounded
run on it.

### Attempt 3 and the 1080p30 opt-in

A third attempt was rejected identically. The source rate was then confirmed
independently of our own receiver arithmetic: `QUERY_DV_TIMINGS` on the node
reports `Pixelclock: 74250000 Hz (30.00 frames per second)` and
`CTA-861 VIC: 34`, and VIC 34 is 1080p30 by definition. The camera is genuinely
at 30 Hz, and the MST3367 host-unit decode (`hper=337 vper=299`) was correct.
`SET_VIC` stayed at zero across all three attempts; the budget is still 2.

Rather than leave the experiment blocked on a source that will not move,
`raw_probe_allow_30` (`FPS30=1` through `mz0380-m210-raw-enc-tail.sh`, `RAW30`
through `mz0380-live.sh`) permits the bounded discriminators to run on a
progressive 1080p30 source. This is sound for this specific oracle and unsound
to generalise from, so both halves are worth stating:

* the `0x2f7600` extent is pure geometry - `1920 * 1080 * 3 / 2` - and does not
  depend on refresh at all;
* `fw` selects the chroma layout, not the rate: M209 established six means
  planar 4:2:2 (`S/2` per plane) and every other value, seven included, means
  planar 4:2:0 (`S/4` per plane), and M79 ties `fw == 7` to the tinyvenc7 spawn;
* the run is bounded by completions and by a wall-clock window, and eight
  completions at 30 Hz still fit the two-second window comfortably.

What it does not establish is Windows parity: Windows was observed at 1080p60,
and a 30 Hz result cannot be reported as reproducing the retail configuration.
The knob therefore defaults off, the driver logs an explicit warning into the
same dmesg the run is scored from, and the harness prints the deviation before
loading and the measured source rate in its result block. If the banks fill at
30 Hz, the follow-up is a 60 Hz confirmation run, not a conclusion.

---

## M210 hardware result (2026-08-27): the encoder tail does not start it either

M210 was spent once, on a 1080p30 source under the documented `FPS30=1`
deviation. Full log: `docs/m210-enc-tail-2026-08-27.log`. Everything the run
required was present:

```
SET_VIC 1, SET_ENC_PARAMS 2, SET_PREVIEW_PARAMS 1, op06 0, source 1920x1080p@30
M209 raw V4L2 totals: events=0 exact_frames=0 bad_extents=0 slots_seen=0x00
stop raw bank0/1 buf[0..3]: extent=0x0, 0/1126 sampled pages touched, completions=0
```

So the Windows encoder tail is not what starts the producer, and neither is
`op 0x06`. Both hypotheses are dead, and the negative is as total as M209's:
not one byte was written into any of the eight slots.

That leaves exactly one structural difference between these runs and the
encoded path that delivers 60 fps today: **`op 0x04` was never registered**.
The reasoning in the M209 entry - that a missing sink cannot suppress
generation into the sinks that do exist - is therefore wrong for this firmware.
The behaviour is consistent with tinyvenc validating its complete output set
before it starts anything, which also matches what Windows does on every start:
it registers `op 0x02`, `op 0x08` and `op 0x04` together.

### M211: implemented, build-checked, unspent

`raw_bank_observe` (`RAWOBS` through `mz0380-live.sh`,
`mz0380-m211-raw-observe.sh` to run it) inverts the experiment. Instead of
replacing the encoded path it rides it: `h264_probe=1` exactly as the working
60 fps configuration, `op 0x04` registered and consumed as usual, and the eight
`0x466000` raw buffers registered through `op 0x02`/`op 0x08`, poisoned, and
read back at stop. The raw banks take the `op 0x02` slots, so the legacy
stream-buffer registration is skipped rather than layered underneath them -
which is Windows' own topology.

It is deliberately passive: V4L2 still negotiates H.264, completion routing and
delivery are untouched, and nothing about the run can perturb the encoded path
it is measuring. `raw_bank_observe` requires `h264_probe=1` and
`dma_iova_remap=1` and refuses to coexist with `raw_bank_probe`, so the two
experiments cannot be confused for one another.

The oracle is the poison. Any `op 0x02`/`op 0x08` slot with a non-zero extent
after a live encoded capture proves the raw surface exists as a by-product of
the configured encoder pipeline. Eight pristine slots after confirmed encoded
delivery proves the opposite - the ring is not the Linux raw source at all -
and the next step after that is static work in the Windows binary rather than
another start.

Both kernels build warning-free. The start is unspent.

### Spawn accounting correction

`mz0380-live.sh` already commits the tally itself: `do_unload()` runs
`scripts/mz0380-spawns.sh commit` before `rmmod` and `unloaded` after it. The manual
`scripts/mz0380-spawns.sh add 1` recommended after the M209 and M210 runs therefore
double-counted them. `encoder_spawns++` sits on the SET_VIC fire, so the three
guard-rejected attempts cost nothing at all. The tally read 7 after the M210
run against roughly 4 real spawns; treat it as a conservative over-count and
reset it after the next mains-off cycle rather than trusting the number.

---

## Correction (2026-08-27): M210 did not isolate op04, and M211 was mis-configured

The operator's known-good 60 fps OBS load is:

```
sudo env VICFW=7 H264PROBE=1 POLLDRAIN=0 WINSEQ=1 OP6=1 POSTMASK=0 \
         FASTKILL=0 H264DIVISOR=0 PERSIST=1 ./mz0380-live.sh load
```

Two things follow, and both invalidate work above rather than extending it.

**`OP6=1`.** The working path fires `START_STREAMING(0x06)` *after* the Windows
tail, even though the retail driver does not. M210 deliberately suppressed op06
on the grounds that Windows omits it once `0x2d`/`0x31` have been sent. So the
M210 run differed from the working sequence by op06 **and** op04, and its
negative cannot be attributed to the missing op04 alone. The conclusion "only
op04 is left" was premature.

The cheap experiment that does isolate op04 is M210 with op06 restored - the
working sequence minus the encoded window only. `mz0380-m210-raw-enc-tail.sh`
now takes `OP6=1` for exactly that, and its oracle expects `EXPECT_OP06`
completions instead of hard-coding zero:

```
sudo FPS30=1 OP6=1 scripts/mz0380-m210-raw-enc-tail.sh
```

**Defaults are not the working configuration.** The first `mz0380-m211-raw-observe.sh`
set only `H264PROBE=1 RAWOBS=1 PERSIST=0` and left the rest to `mz0380-live.sh`,
which defaults `vic_fw` to 5 (tinyvenc5 - one frame per process, then frozen),
`win_seq` to 0 (M90: does not reach the splash) and `poll_drain_ms` to 20. That
is not a configuration that has ever delivered continuous encoded video, so a
"the raw banks stayed pristine" result from it would have been worthless. Both
harnesses now name the full knob set explicitly, and any future one must too:
for this driver, an unnamed knob is a wrong knob.

`POSTMASK=0`, `FASTKILL=0` and `H264DIVISOR=0` are already the driver defaults
(M128d, M157 and the all-frame bitmap schedule respectively), so those three
carry no drift - but they are named in the harnesses anyway so the whole start
sequence is readable in one place.

### Run order after this correction

1. `sudo FPS30=1 OP6=1 scripts/mz0380-m210-raw-enc-tail.sh` - the working sequence
   minus op04. A positive here means the raw ring works and op04 was never
   needed; a negative isolates op04 properly, which M210 as first run did not.
2. `sudo scripts/mz0380-m211-raw-observe.sh` - the working sequence *with* op04, raw
   banks observed passively alongside it.

---

## M210b + M211 (hardware, 2026-08-27): the producer runs, and the blocker is the old 16-byte stall

Two runs, both on the 1080p30 camera. Logs:
`docs/m210b-tail-plus-op6-2026-08-27.log`, `docs/m211-raw-observe-2026-08-27.log`.

### The producer needs the encoder tail AND op 0x06 - neither alone

`sudo FPS30=1 OP6=1 scripts/mz0380-m210-raw-enc-tail.sh` produced **209 completions**
where M209 and M210 produced none. Lining the three up:

| run | encoder tail | op 0x06 | result |
|---|---|---|---|
| M209 | no | yes | `events=0`, all eight slots poison-intact |
| M210 | yes | no | `events=0`, all eight slots poison-intact |
| M210b | yes | yes | 209 completions rotating op02 slots 0..3 |

So `op 0x06` alone does not start the producer and the Windows tail alone does
not either; the pair does. This also retires the conclusion drawn from M210
that "only the missing op04 is left" - op04 was never the discriminator.

### Only the op02 bank rotates - op08 is never written

Tokens arrived as `a5a5a5a0`, `a5a5a5a1`, `a5a5a5a2`, `a5a5a5a3` - our seeded
`a5a5a5a5` sentinel with only the low nibble replaced, cycling 0..3 - and
`slots_seen` never exceeded `0x0f` across 209 events. All four `op 0x08` slots
ended `extent=0x0`, `0/1126 sampled pages touched`, poison intact.

The M209 static reading said this board selects `token % 8` across two banks
when all eight buffers exist. On the Linux command sequence it demonstrably
uses four. Either the eight-slot mode needs something we do not send, or the
`% 8` branch is not the one this firmware takes; the static claim should not be
repeated as if hardware had confirmed it.

### Every write is exactly 16 bytes - the M91/M107/M128a stall

`extent=0x10` on every one of the 209 completions, `1/1126 sampled pages
touched`, `bad_extents=209`, `exact_frames=0`. This is the long-standing
16-byte stall signature, and it is now reproduced on the raw ring itself.

The bytes are not the splash and not a constant:

```
M91   (splash-era):  11 11 11 11 10 10 11 11 11 11 11 11 11 11 11 11
M128a (0x53 field):  a dithered flat field at 0x53
M210b slot0:         5e 5e 5d 5d 5a 5a 5b 5a 5c 5c 5c 5e 5e 5e 5e 5e
M210b slot1:         5d 5d 5d 5d 5c 5a 5a 5c 5d 5c 5c 5c 5e 5d 5d 5e
M211  slot0/slot3:   5d 5d 5b 5a 5a 5a 5b 5d 5c 5c 5d 5e 5d 5d 5d 5c
M211  slot1/slot2:   5c 5c 5c 59 5a 5c 5b 5c 5b 5c 5d 5d 5d 5d 5d 5d
```

Dithered mid-dark grey around 0x5a-0x5e, differing per slot and between runs.
That is what a dark camera scene looks like as luma, and M128a's control
question - "are these pixels or a broken transfer of whatever was in the
buffer" - is still the right question. It has not been answered, and these
values do not answer it either.

### M211: the encoded path does not change any of it

`sudo scripts/mz0380-m211-raw-observe.sh` ran the operator's working configuration
with `op 0x04` live and the raw banks observed passively. The encoded path
worked - **60 H.264 frames delivered, 891,529 bytes, clean IDR** - and the raw
banks behaved exactly as in M210b: four `op 0x02` slots at `extent=0x10`, all
four `op 0x08` slots pristine.

So registering and consuming `op 0x04` neither enables nor disturbs the raw
ring. The raw path's problem is not a missing sink. It is that the transfer
stops after one 16-byte burst - the same failure the splash-era runs hit, now
visible on a path that is otherwise fully alive.

### Cadence, and one thing not to conclude yet

The 209 raw completions arrived every 16.3 ms on average - about 61 per second
- while the encoder was configured for the 30 fps the receiver measured. That
is worth noticing but not worth concluding from: it could mean the source is
really 60 Hz and the `vperiod` decode is halved (M105 records a
`hper=337 vper=299` artifact during re-lock, exactly the numbers this camera
produces), or it could mean the card emits two records per frame. The 60 H.264
frames in the M211 window work out to roughly 20 fps, which matches neither
cleanly. Measure it deliberately before building on it.

### Next: answer M128a's control, which is still open

The question is unchanged since M128a: are those sixteen bytes source pixels,
or a broken transfer of buffer residue? The cheap discriminator M128a proposed
is still the right one - make the source drastically brighter or darker and see
whether the bytes track it - and M211 is now the ideal host for it, because the
encoded stream captured alongside is a visual record of what the camera saw.

`raw_bank_observe` now samples the first sixteen bytes of each `op 0x02` slot
on every encoded completion and logs them whenever they change, so a single
bounded run over a changing scene answers it without another start.

---

## M212 (hardware + offline, 2026-08-27): the sixteen bytes ARE pixels, measured against the co-captured picture

The question RE_FINDINGS has carried since M128a - and named explicitly at
M161/M177, "nobody has ever dumped what those 16 bytes contain" - is answered,
and answered numerically rather than by eye.

`raw_bank_observe` samples the first sixteen bytes of each `op 0x02` slot on
every encoded completion and logs them on change, so one bounded M211 run
produces both the raw samples and, in the same capture window, the H.264 stream
the encoder made from the same frames. Decoding that stream gives an
independent record of what the camera saw. Artifacts:
`docs/m212-head-samples-2026-08-27.log`,
`docs/m212-encoded-reference-2026-08-27.h264`.

Decoded frame 0, first sixteen luma samples:

```
5e 5c 59 57 57 57 58 58 59 59 5a 5b 5c 5c 5c 5c
```

Each raw head against the closest decoded frame:

| slot | head | MAD | max byte diff |
|---|---|---:|---:|
| 0 | `5e5d5a5758585959595a5c5c5d5c5a5d` | 0.88 | 2 |
| 1, 2 | `5f5d5a585858595a5a595d5c5d5b5a5c` | 1.12 | 3 |
| 0, 3 | `5e5c59585858585959595c5b5c5c5a5d` | 0.56 | 2 |
| 1 | `5d5b59585858585858595b5b5c5c5b5c` | 0.50 | 1 |
| 3 | `5e5c5a595656585858595c5c5c5c5c5c` | 0.56 | 2 |

Controls from the same decoded frame: the top-left sixteen bytes against a
mid-frame region give MAD 37.00, and against the `0xa5` poison 75.06. So a
mean absolute difference below 1.2, with no byte off by more than 3, is not a
coincidence of a flat grey scene - it is the same sixteen pixels, differing
only by H.264 quantisation.

**The card captures correctly and transfers sixteen bytes.** Not a descriptor,
not buffer residue, not the standby canvas: the top-left luma of the live
picture, written into whichever `op 0x02` slot the token selects, sixty times a
second. Two slots receiving byte-identical heads in the same run is consistent
with that too - the same picture reaching two slots.

### What this means, and the reframing it forces

The raw capture path is not broken in the sense of "no video". Every stage
works up to and including the DMA, which then stops after one burst. That is a
much narrower bug than anything considered above.

It also connects to what M177 already called this window: **tinyvenc7's 16-byte
preview DMA**. Under `fw=7` the `op 0x02` bank is the preview writer, not the
full-frame raw writer, and M92's own size table says what a preview surface is
sized for on this card:

```
0x10F000 = 1024 x 540 x 2   + 4096   (preview, YUV422)
0x0CA900 = 1024 x 540 x 1.5 + 256    (preview, YUV420)
```

So the target for a continuous `fw=7` raw path may not be `0x2f7600` at all.
The M209 static reading that produced that number described the retail driver's
*base raw* callback; nothing has shown that Linux's `fw=7` `op 0x02` window is
that same surface. A 1024x540 preview at `0xCA900` is an equally consistent
reading of the same evidence, and it is the one M177 already adopted.

`op 0x31` is `SET_PREVIEW_PARAMS` - the preview configuration - and
`post_mask` gates which of its fields the card applies. We currently send
`post_mask=0`, i.e. no fields applied, so the preview writer runs on card
defaults. M128d established that `0x1f` truncates to 16 bytes under `fw=5`;
nothing has mapped the mask under `fw=7`, where the writer is a different
binary.

### Next, in order, and all but the last are free

1. **Static, zero spawns.** Find the preview DMA length in tinyvenc7 - what
   sets the transfer size for the `op 0x02` writer, and whether any
   `SET_PREVIEW_PARAMS` field or geometry register changes it. The sources are
   already in the tree (`re-dump/`, `ep-disasm.txt`). This is the highest-value
   remaining work and it costs nothing.
2. **Re-read the size question.** Decide from the binary whether the `fw=7`
   `op 0x02` surface is 1920x1080 I420 (`0x2f7600`) or a 1024x540 preview
   (`0xCA900`). Every oracle in the harnesses currently assumes the former on
   the strength of a static reading of a different callback.
3. **`probe_windows=1` under `fw=7`** (one spawn, never run). M32's conclusion
   that the three extra outbound windows are never written was measured under
   `fw=5`, against a producer that was livelocked. Under `fw=7` the producer is
   demonstrably alive, so it is a different experiment with the same command.
4. **A `post_mask` bisect under `fw=7`** (one spawn per step). Only after 1 and
   2 say which field to move.

### Budget

The tally reads 11 - inside the historical 8-18 wedge range. Nothing further
should be spent on hardware until a mains-off power cycle and
`scripts/mz0380-spawns.sh reset`.

## M213 (tinyvenc7 static, 2026-08-28): the `op 0x02` transfer length is `ALIGN16(W) * H * 3/2`, and SET_PREVIEW_PARAMS cannot change it

Zero spawns. `re-dump/fw/yuan_demo_sdi/tinyvenc7` had never been
disassembled - only `tinyvenc5.txt` existed in `re-dump/`. The binary is ARM
EABI5, **not stripped**, so the globals that matter carry their real names:
`raw_dma_`, `h264_dma_`, `preview_settings`, `preview_params_settings`,
`is_preview`, `enable_dma`, `PCIEtOptions`. The system `objdump` has no ARM
support; `llvm-objdump --triple=armv7-none-linux-gnueabi` does.

### The length, in the per-frame path

`vcap_handler(video_cap_state const*, void*)` @ `0x122d4` computes the raw
transfer size inline and passes it as the 4th argument:

```
12514: ldrh r0, [r4, #116]      ; preview_settings+0x24
12518: ldrh r9, [r4, #86]       ; preview_settings+0x06   = H
12520: mul  r9, r0, r9
12528: add  r9, r9, r9, lsl #1  ; *3
1252c: asr  r9, r9, #1          ; /2
...
1258c: bl   TK_MMA_SetOptions(h=[r4+0xb4], PCIEtOptions, ...)
125b8: bl   TK_MMA_StartOneFrame(h=[r4+0xb4], 0x90000000, phys, r9)
```

`r4` is the settings base `0x4b030`, so `[r4+0xb4]` is `raw_dma_` @ `0x4b0e4`
and `[r4+0xb8]` is `h264_dma_` @ `0x4b0e8`. **`raw_dma_` is the `op 0x02`
writer.** The other site, @ `0x126cc`, is `h264_dma_`: a fixed **4096-byte**
info block whose word at +8 is `(token & 7) + 1` - the eight-slot ring, in the
binary, exactly as M209 read it out of the `.sys`.

`preview_settings+0x24` is derived once in `main` @ `0xddb0`:

```
ddb0: ldrh r1, [r10, #84]      ; preview_settings+0x04 = W
ddbc: ldrh r2, [r10, #86]      ; preview_settings+0x06 = H
ddc8: add  r12, r1, #15
ddd0: and  r3, r12, #0xfff0    ; ALIGN16(W)
dde0: strh r4,  [r10, #118]    ; +0x76 = W/2
dde4: strh r0,  [r10, #122]    ; +0x7a = ALIGN16(W/2)
dde8: strh r3,  [r10, #116]    ; +0x74 = ALIGN16(W)      <- the multiplicand
ddec: strh r12, [r10, #120]    ; +0x78 = H/2
```

So the `fw=7` `op 0x02` transfer length is

>   **`len = ALIGN16(width) * height * 3 / 2`**  - planar I420, computed at
>   run time, never a constant.

### Which surface: 0x2f7600, decisively

`preview_settings+0x04` and `+0x06` are written **from the command line**, by
`atoi` in the getopt switch @ `0xffe8` / `0xffd8`. They are the `-w` / `-h`
that `video_capture_mgr` sprintf's from SET_VIC geometry. Nothing else writes
them.

And `vcap_handler` refuses any frame whose geometry disagrees, @ `0x12330`:

```
[tiny7] Drop frame(%d), Tiny_Set(%d x %d) != VIC_Get( %ld x %ld )
```

with `Tiny_Set` read from `preview_settings+0x04/+0x06` and `VIC_Get` from the
capture state. A 1024x540 preview surface underneath a 1920x1080 VIC would
therefore not produce short writes - it would produce **dropped frames and no
writes at all**. The two cannot disagree by construction.

For our load, SET_VIC geometry is 1920x1080, `ALIGN16(1920) = 1920`, and

```
1920 * 1080 * 3/2 = 0x2F7600
```

**The harness oracles are already correct.** Item 2 of the previous handoff -
"decide whether the surface is `0x2f7600` or `0xCA900`" - resolves to
`0x2f7600`, and the M92 preview-surface reading does not govern tinyvenc7's
writer. Incidentally the `0xCA900` figure was never right on its own terms
either: `1024 * 540 * 3/2 = 0xCA800`.

### SET_PREVIEW_PARAMS has no geometry field at all

The `op 0x31` handler is in `main` @ `0xea8c`, and its own trace string
enumerates every field it carries (`.rodata` @ `0x2649c`):

```
[tiny7] SET_PREVIEW_PARAMS mask=0x%x, ch[%d], fps=%d, skip=%d, avg=%d,
die_en = %d, preview_off = %d, fake_frame_off = %d, preview_no_osd = %d,
hw_d = %d is_mirror = %d, is_flip = %d
```

Reading the handler, the command bytes land in `preview_params_settings`
(`0x4c0e0`), which is a **different struct** from `preview_settings`
(`0x4b080`) - and the length is computed only from the latter. Field map, from
the 44-byte command buffer `cmd` @ `0x4c218`:

| cmd  | -> pps | field                          | mask gate |
|------|--------|--------------------------------|-----------|
| +04  | +0x00  | mask, OR-accumulated, persists | -         |
| +08  | +0x04  | ch                             | always    |
| +09  | +0x05  | fps (mask-bitmap modulus)      | always    |
| +0a  | +0x06  | skip (frame modulus)           | bit 1     |
| +0c  | +0x08  | avg                            | bit 4, **one-shot** (`bic` after apply) |
| +0d  | +0x09  | die_en / preview_off           | always    |
| +0e  | +0x0a  | fake_frame_off                 | always    |
| +0f  | +0x0b  | preview_no_osd                 | always    |
| +10  | +0x0c  | hw_d                           | always    |
| +11  | +0x0d  | is_mirror                      | always    |
| +12  | +0x0e  | is_flip                        | always    |
| -    | +0x10  | preview bitmap, low 64         | -         |
| -    | +0x18  | preview bitmap, high 64        | -         |

**No entry is a width, a height, a stride, a surface size, or a transfer
length.** So item 4 of the previous handoff - a `post_mask` bisect to move the
transfer size - is answered negative before it costs a spawn: no value of
`post_mask` can change how many bytes `raw_dma_` writes. `post_mask` selects
*which frames* get written and how the preview is post-processed, never *how
much*.

### What `post_mask` does gate: frame selection

`vcap_handler` @ `0x124b0` picks one of two selection modes, and the raw DMA
runs only if the result is exactly 1:

- `pps[+0x06] != 0`: `r10 = frame_count % pps[+0x06]`, fire when `== 1`.
- `pps[+0x06] == 0`: `r10 =` bit `(frame_count % pps[+0x05])` of the **128-bit
  preview bitmap** at `pps+0x10 .. pps+0x1f` - the pair printed by
  `Preview mask = 0x%llx - 0x%llx`. This is the preview-side twin of the
  all-frame H.264 bitmap M177 named.

Both feed the same guard @ `0x124cc`, which requires all three of:

```
pps[+0x09] != 1          ; preview_off / die_en
r10 == 1                 ; the frame-selection result above
enable_dma != 0          ; 0x4c118
```

We send `post_mask=0`, so `skip`, `fps` and the bitmap are all card defaults
and the selection is running on whatever the flash image chose. That is the
only remaining `post_mask`-shaped lever, and it is a *rate* lever.

### A bandwidth clamp that sits exactly on our operating point

Also in the `op 0x31` handler, @ `0xeb78`:

```
eb78: ldrh r12,[r10,#84]      ; W
eb7c: ldrh r4, [r10,#86]      ; H
eb80: ldrb r1, [r10,#0x51]    ; preview fps
eb84: mul  r4, r12, r4
eb8c: mul  r4, r1, r4         ; W*H*fps
eb90: cmp  r4, #0x76a7000
eb94: ble  ok                 ; else halve pps[+0x07]
```

`0x76a7000 = 124416000 = 1920 * 1080 * 60` **exactly**, and the comparison is
`ble`. So 1080p60 passes the cap with precisely zero margin and is not
clamped; anything above it - a higher fps, or a wider surface - is halved. The
constant is itself good evidence that 1920x1080p60 is the intended maximum
preview surface for this binary, which is the same conclusion the length
arithmetic reaches.

### Method note

The disassembly is reproducible and nothing about it touches hardware:

```
llvm-objdump -d -C --triple=armv7-none-linux-gnueabi \
  re-dump/fw/yuan_demo_sdi/tinyvenc7 > tinyvenc7.txt
```

ARM literal pools are not resolved by `llvm-objdump`, and every interesting
reference in this binary is a pool load, so the reading above was done against
an annotated dump that resolves each `ldr rN, [pc, #imm]` to its pool word and
maps that word to a symbol or a C string. Without that step the globals are
invisible. `mz0380-tinyvenc7-annotate.py` in the tree regenerates it.

## M214 (tinyvenc7 static, 2026-08-28): the 16 bytes are the *not-selected* stub, and `post_mask=0` guarantees every frame is not selected

Zero spawns, same annotated dump as M213. This is the mechanism behind every
raw-window observation from M128d to M212, and it is a single branch.

### `vcap_handler` writes the raw channel on EVERY frame - at one of two lengths

The frame-selection guard @ `0x124cc` does not choose *whether* to DMA. It
chooses *how much*. Both arms call `TK_MMA_StartOneFrame` on the same
`raw_dma_` handle with the same buffer:

| arm | site | length passed in `r3` |
|-----|------|-----------------------|
| selected | `0x125b8` | `r9` = `ALIGN16(W) * H * 3/2` = `0x2F7600` at 1080p |
| **not selected** | `0x129b0` | **`#16`** |

```
; 0x12944 - the not-selected arm
12988: mov  r1, #16
1298c: ldr  r0, [r6, #0x38]
12990: bl   MemBroker_CacheCopyBack        ; flush 16 bytes
12994: ldr  r0, [r6, #0x38]
12998: ldr  r8, [r4, #0xb4]                ; raw_dma_
1299c: bl   MemBroker_GetPhysAddr
129a4: mov  r3, #16                        ; <- the transfer length
129b0: bl   TK_MMA_StartOneFrame(raw_dma_, 0x90000000, phys, 16)
```

**The sixteen bytes are the first sixteen bytes of the very same I420 frame
buffer the full transfer would have sent.** That is why M212 found them to be
genuine top-left luma samples matching the co-captured decoded picture to a
mean absolute difference of 0.50-1.12: it is the identical source pointer, with
`3110400` replaced by `16`. Nothing was truncating a large transfer, and nothing
was writing a header - the card was writing the head of the frame because that
is what a 16-byte transfer from a frame base pointer *is*.

### Why we are always on the not-selected arm

`r10` must equal 1 for the full transfer. It comes from one of two modes
(`0x124b0`), and with our payload both evaluate to 0:

- `pps[+0x06]` (`skip`) is `0`, so the modulus mode is not taken and the
  **128-bit preview bitmap** mode is (`0x129d0`).
- The bitmap lives at `pps+0x10 .. pps+0x1f`, in **`.bss`**, and the only code
  that ever writes it is `tiny_calculate_avg_fps` @ `0x13ad4` and
  `tiny_calculate_skip_fps` @ `0x13c2c`, called from the SET_PREVIEW_PARAMS
  handler at `0xf228` / `0xf28c`.
- Both call sites sit behind `ands lr, r3, #1` @ `0xeb54` - **mask bit 0**.

We send `post_mask=0`. Bit 0 is clear, neither helper ever runs, the bitmap
stays all zeros, `and r10, r3, #1` yields 0 for every frame, and every frame
takes the 16-byte arm. The other two gate terms are satisfied and are not the
problem: `enable_dma` is set to 1 in the start path @ `0xe430` (only the
"Disable dma" argv flag clears it), and `preview_off` = `pps[+0x09]` = cmd
byte `0x0d`, which we send as 0.

> **Nothing is broken.** The card is doing exactly what it was configured to
> do. A preview writer that was told to select no frames writes the stub for
> every frame, forever.

This also re-reads M128d. "post_mask=0x1f truncates the DMA to 16 bytes" was
the wrong causal direction: 16 bytes is not a truncation, it is the stub the
not-selected arm always writes. Under `fw=5` with `0x1f`, and under `fw=7` with
`0`, the frame selection came out empty for different reasons and produced the
same stub.

### The experiment this implies - one spawn, sharp pass/fail

Set **mask bit 0** so the bitmap is actually computed. Following the handler
from `0xeb54`:

- bit 0 set, bit 1 clear -> `0xf1f4` -> `r3 = mask & 2` is 0 -> `0xf268` ->
  `tiny_calculate_skip_fps(pps[5], ..., &lo, &hi, 0)` **writes the bitmap**.
- bit 0 and bit 1 set -> `pps+0x07` (`avg`) is taken from cmd byte `0x0b`
  first, and a non-zero `avg` routes to `tiny_calculate_avg_fps` instead.

Either helper populates it, so **`post_mask=0x01` is sufficient** and is the
minimal step. `post_mask=0x03` with a non-zero `avg` byte exercises the
averaging variant.

Note that `mz0380_stream_post_proc()` currently builds only
`post[1] = (fps & 0xff) << 8`, so cmd bytes `0x0a` (`skip`) and `0x0b` (`avg`)
are always zero. Driving the modulus mode instead of the bitmap mode therefore
needs a payload change as well as a mask change: `skip` is cmd byte `0x0a`, and
the fire condition is `frame_count % skip == 1`.

Prediction, and it is falsifiable in one run: with `post_mask=0x01` the raw
window should start receiving `0x2F7600`-byte transfers on the frames the
bitmap selects, while unselected frames keep producing 16-byte writes.

### A field-order discrepancy to fix in the driver comment

`mz0380-dma-stream.c` documents the tail of the `op 0x31` payload, from M128's
**tinyvenc5** decode, as:

```
[0x10]=mirror  [0x11]=flip  [0x12]=hw_d
```

tinyvenc7's own trace string orders the last three the other way:

```
... preview_no_osd = %d, hw_d = %d is_mirror = %d, is_flip = %d
   ->  [0x10]=hw_d  [0x11]=is_mirror  [0x12]=is_flip
```

We send zeros in all three, so nothing observable changes today, but the
comment should not be trusted for `fw=7` if any of them is ever used.

### M214 addendum: `tiny_calculate_skip_fps` decoded, and why bit 0 alone is a big step

`tiny_calculate_skip_fps(fps, skip, &lo, &hi, avg_mode)` @ `0x13c2c` builds the
128-bit selection bitmap directly:

```
13c50: mov  r4, r0        ; fps
13c58: orrs r6, r4, r5    ; fps == 0 -> early out, bitmap untouched
13c60: mov  r0, r1        ; skip
13c64: adds r6, r0, #1    ; step = skip + 1
loop @13c9c:
       set bit r0 in the 128-bit accumulator
13cd8: r0 += step
13ce8: blo loop           ; while (idx < fps)
```

So it sets bits `0, step, 2*step, ...` below `fps`, i.e. **selects every
`(skip+1)`th frame**, and the `avg_mode` flag (false on our path) only
pre-scales `skip` to `2*skip+1`.

The call site passes `skip` from `preview_params_settings[+0x06]`, which comes
from **command byte `0x0a`** - and `mz0380_stream_post_proc()` never populated
that byte. It built only `post[1] = (fps & 0xff) << 8`. So the card has always
been handed `skip = 0`, giving `step = 1`.

**`step = 1` selects every frame.** Turning on mask bit 0 with the payload as it
stood would therefore jump straight from a 16-byte stub per frame to a full
`0x2F7600` frame per frame: `3110400 * 60 = 186 MB/s`, on a link this card
negotiates at PCIe **x1 Gen1** (`pcie link ... speed=1 width=x1`, ~250 MB/s
theoretical). That is ~75% of the link, on top of the H.264 stream, and it is
not the experiment anyone would choose to run first.

Note the card itself will not object: tinyvenc7's own bandwidth clamp is
`W*H*fps <= 0x76a7000`, which is exactly `1920*1080*60`, compared with `ble`.
1080p60 sits precisely on the cap and is not clamped.

Accordingly `post_skip` (byte `0x0a`) and `post_avg` (byte `0x0b`) are now sent,
as module parameters defaulting to 0, so the first bit-0 run can request a
fraction of the frames. `post_skip=29` selects every 30th frame - about 2 full
frames per second - which is enough to prove the mechanism at ~6 MB/s.

## M215 attempt 1 (2026-08-28): null run - `raw_bank_observe` is a load-time parameter

No spawn consumed; the stream never started, so the encoder never spawned and
the tally stayed at 1.

The run was set up correctly in every respect that was checked - `post_mask=1`,
`post_skip=29`, `h264_probe=Y`, `dma_iova_remap=Y`, `raw_bank_probe=N` - and
still produced nothing:

```
mz0380[0]: stream start: pre-STOP(op 0x07, all channels) ret=0, settling 1900 ms
mz0380[0]: SET_BUF failed (-19) - frames will not flow
mz0380 0000:04:00.0: dma_start failed (-19)
```

The harness had enabled `raw_bank_observe` through sysfs on the already-loaded
module, on the assumption that its banks are allocated at stream start. They are
not. The allocation lives in `mz0380_dma_setup()`, and **`mz0380_dma_setup()` is
called from `mz0380_pci.c` at PCI probe** - i.e. at insmod. So
`raw_bank_observe` and `raw_bank_probe` are read once, at load.

Setting either afterwards sends `mz0380_stream_program_bufs()` down the
raw-bank branch with `raw_probe_bufs[].va == NULL`, and
`mz0380_raw_probe_program_bufs()` returns a bare `-ENODEV` that surfaces only as
"SET_BUF failed (-19)" - a message that says nothing about the actual cause.

Three fixes, all made:

1. `mz0380_raw_probe_program_bufs()` now names the cause: that the banks come
   from `mz0380_dma_setup()` at probe, that the parameter is load-time, and to
   reload with `RAWOBS=1` rather than poke sysfs.
2. `MODULE_PARM_DESC(raw_bank_observe)` says LOAD-TIME ONLY.
3. The harness now *checks* `raw_bank_observe=Y` and refuses, instead of
   setting it.

**A fourth fix matters more than the other three.** The harness scored this run
as `M215 NEGATIVE: every slot is still at the 16-byte stub`, because it tested
`FULL == 0` without asking whether any slot had been written at all. Nothing was
written; nothing was even streamed. A run that fails to start is not evidence
about the hypothesis, and reporting it as one would have retired a correct
prediction on the strength of a driver misconfiguration. The scoring now
separates a null run from a negative and exits non-zero, printing the
`SET_BUF`/`dma_start`/alloc failures and v4l2-ctl's own output - which the first
version discarded to `/dev/null`, which is why the cause was not visible in the
run output at all.

M214's prediction remains untested.

## M215 (hardware, 2026-08-28): **mask bit 0 turns the stub into whole raw frames** - M214 proven

One spawn (tally 1 -> 2). Load:

```
VICFW=7 H264PROBE=1 POLLDRAIN=0 WINSEQ=1 OP6=1 POSTMASK=0x01 POSTSKIP=29
RAWOBS=1 FASTKILL=0 H264DIVISOR=0 PERSIST=1
```

900 encoded frames captured at ~60 fps alongside, then a forced pipeline stop
for the extent dump:

```
stop raw bank0 op0x02 buf[0] extent=0x2f7600 (3110400 bytes), 760/1126 pages touched, poison=0xa5
stop raw bank0 op0x02 buf[1] extent=0x2f7600 (3110400 bytes), 757/1126 pages touched, poison=0xa5
stop raw bank0 op0x02 buf[2] extent=0x2f7600 (3110400 bytes), 760/1126 pages touched, poison=0xa5
stop raw bank0 op0x02 buf[3] extent=0x2f7600 (3110400 bytes), 758/1126 pages touched, poison=0xa5
stop raw bank1 op0x08 buf[0..3] extent=0x0 (0 bytes), 0/1126 pages touched, poison=0x5a
```

`0x2f7600` = 3110400 = `ALIGN16(1920) * 1080 * 3/2`, the exact value M213 read
out of `vcap_handler` @ `0x12514`. **The raw capture path works.** The card has
been able to send whole I420 frames the entire time; it was writing the
not-selected stub because `post_mask=0` left the preview selection bitmap
empty, exactly as M214 predicted.

### What the numbers confirm beyond the headline

- **The transfer stops at the frame boundary.** The buffer is `0x466000` =
  1126 pages; a frame is 3110400 bytes = 759.4 pages; 757-760 sampled pages are
  touched. Nothing runs past the frame, so the length really is computed, not a
  fixed window size.
- **`op 0x08` is never written.** Bank1 came back with `extent=0x0`, zero pages
  touched and its `0x5a` poison fully intact. The "two independent banks"
  model that M209 carried over from the Windows `.sys` does not describe this
  path: under `fw=7` raw arrives through **op 0x02 only**. Registering op08
  costs nothing but it receives nothing.
- **Both arms write, which is why head-change counts are high.** The M212 head
  sampler logged max counters of 2, 219, 217 and 230 across the four slots over
  ~10000 frames. `post_skip=29` selects every 30th frame, which would predict
  far fewer - until you remember that unselected frames still write the 16-byte
  stub. Stub and full frame both change the head, so the head sampler counts
  both. The asymmetry between slot 0 and slots 1-3 is sampling timing, not two
  different behaviours.

### The remaining gap is now ours, not the card's

```
completions=0 delivered=0 last_completion_extent=0x0
```

Whole frames are landing in host memory and **the driver's completion path never
counts them**. Nothing is wrong on the card side any more: the pixels are there,
at the right length, in our buffers, at a rate we control. What is missing is
the host-side plumbing that notices a raw completion and hands the buffer to
VB2 - the same job `mz0380_drain_raw_probe_snapshot()` does for the encoded
window.

That is the whole of the remaining work for raw capture, and it is ordinary
driver work rather than reverse engineering.

### Method note, and two harness bugs worth remembering

The extent dump is emitted only by `mz0380_raw_probe_bufs_dump(dev, "stop")` in
the stream-stop path. Under `persistent_h264=1` - which is part of the
known-good load - a V4L2 STREAMOFF only detaches VB2 and the pipeline stays up,
so that path never runs. Attempt 2 of this run streamed 900 frames flawlessly
and reported nothing, because the measurement it existed to take is only
produced by a real stop. The harness now forces the stop itself.

The other bug was worse and is worth stating plainly: the harness scored a run
with zero extent dumps as `M215 NEGATIVE: every slot is still at the 16-byte
stub`, having tested `FULL == 0` without ever asking whether any slot had been
written. Twice in a row it reported a correct hypothesis as refuted by a run
that had not tested it. Scoring now distinguishes three outcomes - a run that
never streamed, a run that streamed but produced no measurement, and a run that
can actually be scored - and the first two exit non-zero saying so.

## M216 (2026-08-28): the validated configuration is now the driver's default, and the driver installs

No hardware, no spawns. Until now the card only worked if the operator supplied
an eleven-variable `env` incantation; the compiled-in defaults described a
configuration that had been superseded by M128-M137 and M177. Anyone who
installed the module and ran `modprobe mz0380` got a driver that did not
capture, and every harness that did not set the knobs explicitly was measuring
the old path.

Defaults changed to match the load that M177/M215 validated:

| parameter | was | now | why |
|-----------|-----|-----|-----|
| `vic_fw` | 5 | **7** | tinyvenc5 delivers one frame per process then freezes (M158/M159) |
| `h264_probe` | 0 | **1** | stopped being a diagnostic at M177; it is the capture path |
| `win_seq` | 0 | **1** | the validated 60 fps order |
| `win_start_op6` | 0 | **1** | M210b: the producer needs the encoder tail AND op 0x06 |
| `poll_drain_ms` | 20 | **0** | fw=7 uses the window-1 completion ring, not polling |

`persistent_h264=1` and `vic_fast_kill=0` were already correct.

`mz0380-live.sh` no longer hardcodes `poll_drain_ms=20`. Its own `add_opt`
comment warns that hardcoding a knob "silently overrides the module default the
moment the two drift apart" - and that is exactly what had happened.

**Consequence worth stating:** 46 of the 51 harnesses in `scripts/` do not set
these knobs, so they now exercise the fw=7 path rather than the fw=5 one. They
are records of concluded experiments, and the standing rule since 72c25d4 is
that a harness must mirror the load it means to test rather than inherit
defaults. Any harness re-run from the m25-m85 era needs its configuration
stated explicitly.

### Packaging

`MODULE_DEVICE_TABLE` already covered all five known subsystem IDs, so an
installed module autoloads on the card - what was missing was a way to install
it at all.

- `make dkms-install` / `dkms-uninstall` - the persistent path. An out-of-tree
  module outside DKMS silently stops loading at the next kernel upgrade, which
  for a capture card presents as "my camera disappeared".
- `make install` / `uninstall` - single-kernel, for a quick test.
- `/etc/modprobe.d/mz0380.conf` - sets **nothing**, deliberately, since the
  defaults are now right. It carries the `softdep` for videobuf2/DV-timings/ALSA
  and documents the raw-capture and troubleshooting knobs commented out.
- `MODULE_VERSION("0.1.0")`, which DKMS needs.

The card now works from `sudo modprobe mz0380` with no parameters.

## M217 (2026-08-28): raw frames delivered to V4L2 - `raw_deliver`

No hardware yet; this is the plumbing M215 said was the only thing left.

`raw_deliver=1` makes the node advertise `V4L2_PIX_FMT_YUV420` at
`sizeimage=0x2f7600` and forward the card's raw banks to VB2 instead of its
encoded output. The encoder keeps running, because M210b showed the card-side
producer only runs with the encoder tail AND `op 0x06` - there is no raw-only
mode that produces anything - so its bitstream is simply not delivered.

### The completion problem, and the proxy used for it

M215 measured `completions=0` on the raw window while it was demonstrably
writing whole frames: the raw path raises no completion of its own. What it does
have is a shared frame clock - the card writes the raw slot for frame N and
encodes frame N - so the **encoded** completion is used as the trigger, and its
token selects the slot exactly as it already does for the encoded ring.

### Testing for a whole frame in O(1)

`mz0380_raw_probe_infer_length()` answers "how many bytes did the card write" by
scanning backwards from the end of a 4.6 MB buffer. For the 16-byte stub that
walks the whole buffer, so at 60 fps it is ~276 MB/s of reads before the
whole-buffer re-poison writes it back. Affordable for a diagnostic; not for a
capture path.

That question no longer needs asking. M213 read the transfer length out of
tinyvenc7 and M215 confirmed it, and M214 showed the card writes only that
length or a 16-byte stub - so the only question is which arrived, and four
sentinels answer it. They sit near the end of the frame, past anything a stub
touches, and are **spread** rather than adjacent: adjacent sentinels share an
image region, and a saturated flat area could carry the poison value across a
few contiguous bytes, whereas four unrelated rows carrying it at once is not a
coincidence real video produces. Only the sentinels are re-armed afterwards.

### The assumption to doubt first if frames ever tear

An encoded completion does not assert that the raw DMA has finished, so this
could in principle copy a frame mid-write. The guard is the first sentinel,
which sits at the **last dword of the frame** and can only read as written once
the transfer reached the end. That holds if the card writes in ascending address
order - what a linear DMA does, and consistent with the M209/M215 extent
measurements - but it has not been proven directly. It is written down in the
code as the first thing to suspect if torn frames appear.

### Wiring

- `raw_deliver` implies the raw bank allocation, so `raw_bank_observe` need not
  also be set. Like it, it is **load-time** (`0444`): the banks come from
  `mz0380_dma_setup()` at PCI probe.
- It outranks `h264_probe` in `mz0380_current_pixelformat()` and
  `mz0380_current_sizeimage()`, since `h264_probe` stays on underneath it.
- A delivered frame under a locked signal retires the NO SIGNAL placeholder;
  raw has no IDR, so the encoded path's clean-IDR trigger does not apply.
- `/proc/mz0380-state` gains a `raw frames` line counting delivered, dropped and
  skipped stub completions.

**Untested on hardware.** It needs `post_mask=0x01`, or every completion is a
stub and nothing is ever delivered.

### M217 hardware confirmation (2026-08-28): raw capture works end to end

One spawn (tally 2 -> 3). Load:

```
sudo env POSTMASK=0x01 POSTSKIP=29 RAWDELIVER=1 ./mz0380-live.sh load
```

Everything else came from the M216 defaults - no other knobs.

```
Format Video Capture:
        Width/Height      : 1920/1080
        Pixel Format      : 'YU12' (Planar YUV 4:2:0)
```

```
raw frames : 32 delivered, 0 dropped, 5565 stub completions skipped
h264 frames: 0 delivered
```

`v4l2-ctl --stream-mmap --stream-to` produced 32 whole frames; the file's
292936-byte tail is the final write being cut off when the tool exited, not a
short payload - the driver reports zero drops and every delivered frame carries
the full `0x2f7600`.

Content, checked rather than assumed:

- **32 of 32 frames are distinct** - no duplicate delivery, so the sentinel
  re-arm is keeping up with the completion rate.
- Y spans 0..255 with a mean near 110; U and V sit at 129-134, i.e. close to
  neutral. That is a real scene, not poison, not a flat pattern.
- Rendered to PNG the frame is a sharp, correctly-exposed 1080p photograph of a
  desk, and **the colours are right** - the blue keypad renders blue. That
  independently confirms the I420 plane order (a U/V swap gives the textbook
  blue/orange inversion, which is exactly how M130 caught the earlier
  mislabelling).
- No tearing in any frame. The sentinel-at-the-last-dword guard is holding, so
  the ascending-address DMA assumption recorded above survives its first test.

Rate matches the prediction: 32 frames over ~18 s is ~1.8 fps against the 2 fps
that `post_skip=29` implies (bits 0 and 30 set in a 60-frame period). The
higher figure `v4l2-ctl` prints is its own inter-arrival estimate, which
excludes the idle gaps.

**The driver now captures uncompressed 1920x1080 I420 through V4L2 with no
encoder in the delivery path.** That was the project's stated goal.

## M218 (2026-08-28): raw is a format applications choose, not a module parameter

No hardware. M217 made raw delivery possible; it was still reachable only by
reloading the driver with `raw_deliver=1`, which an application cannot discover
or request. That is the practical difference between a driver that can capture
raw and a camera.

- **`ENUM_FMT` now enumerates both** `H264` and `YU12` whenever the raw banks
  exist, instead of the single parameter-chosen entry it returned before.
  `raw_deliver` no longer decides *whether* raw is reachable, only which of the
  two is listed first - i.e. what an application that takes index 0 gets.
- **`S_FMT` selects between them at runtime**, storing the choice in
  `dev->deliver_raw`. Switching while buffers are queued returns `-EBUSY`
  rather than handing back a plane sized for the other format.
- **`TRY_FMT`** evaluates the requested format without committing, and still
  returns a workable format for anything unrecognised, as V4L2 requires.
- `mz0380_current_pixelformat()` takes a `dev` now; the drain, the size
  calculation and `/proc` all follow the runtime choice.

### Two traps this opened, and what closes them

**Selecting raw with `post_mask=0` would deliver nothing.** Bit 0 is what makes
the card populate its selection bitmap; without it every frame takes the
not-selected arm and writes a 16-byte stub (M214/M215). While raw was an
operator's decision the operator also set the parameter. Now that it is an
application's decision there is no operator, so `mz0380_stream_post_proc()`
forces bit 0 whenever `dev->deliver_raw` is set. Nothing else has to be
configured to capture raw.

**Allocating the banks by default could break loading.** `raw_capable`
(default 1) allocates them whenever the encoded path is on, so raw is
selectable out of the box - but that is 8 x `0x466000`, and a machine that
cannot spare it would previously have failed the whole probe. Turning "this
machine cannot spare 37 MB" into "this card does not work" is a far worse
outcome than losing an optional format, so a **capability-only** allocation
failure is now non-fatal: it warns, leaves `raw_capable` false, and H.264
capture is unaffected. An explicitly requested one (`raw_deliver`,
`raw_bank_observe`, `raw_bank_probe`) still fails loudly, because there the
caller asked for something specific and silently not doing it would be worse.

### Not done, and deliberately

`post_skip` still defaults to 0, so selecting I420 asks the card for **every**
frame - ~186 MB/s at 1080p60 against a link this card negotiates as PCIe x1
Gen1. That is the honest default for a camera and it is what the next
experiment measures; it has never been run. If full rate does not hold, the
default becomes a throttle rather than a surprise.

### Correction to the M215 follow-up list

That list named "source-change events and a watch thread" as missing work. It
is not missing: `V4L2_EVENT_SOURCE_CHANGE` is emitted by
`mz0380_signal_event()`, subscription was fixed in M170, and
`mz0380_signal_recovery_work_fn()` monitors at `signal_monitor_ms` (500 ms)
during capture. It deliberately does not sample the receiver while the producer
is healthy, because MST3367 transactions during capture perturb delivery - a
hardware constraint that was measured, not an oversight.

### M218 hardware confirmation (2026-08-28): a camera, with no parameters

One spawn (tally 3 -> 4). `sudo ./mz0380-live.sh load` with **no env at all** -
the insmod line printed `insmod:` and nothing else.

```
[0]: 'H264' (H.264, compressed)   1920x1080 / 1280x720 / 720x480 / 720x576 @ 60
[1]: 'YU12' (Planar YUV 4:2:0)    1920x1080 / 1280x720 / 720x480 / 720x576 @ 60
```

`v4l2-ctl --set-fmt-video=pixelformat=YU12 --stream-mmap` then selected raw the
way an application would, and it streamed. No module parameter was involved at
any point: `post_mask` bit 0 was forced on by the format choice, as M218
intended.

## M219 (hardware, 2026-08-28): full-rate raw is ~50 fps, link-limited - and 18% of the frames were duplicates

### The rate

```
raw frames : 118 delivered, 0 dropped, 0 stub completions skipped (post_skip=0 -> every 1th frame whole)
```

`v4l2-ctl` reported **50.05 and 51.00 fps** across the run. Zero stubs means the
card selected every frame, as `post_skip=0` asks; zero drops means no vb2
buffer was ever missing.

`3110400 * 50 = 155 MB/s`, on `pcie link ... speed=1 width=x1` - PCIe x1 Gen1,
250 MB/s theoretical, ~200 MB/s realistic after protocol overhead. So 155 MB/s
is about 78% of usable bandwidth and 60 fps would need 186 MB/s, which is past
what this link can carry. **The 50 fps ceiling is the link, not the card and not
the driver**, and the degradation is graceful: no drops, no errors, just a lower
delivered rate.

That also retires the concern that opened M218 - that defaulting `post_skip=0`
would be a surprise. It is not; it is simply what the hardware can do.

### The defect

118 frames delivered, **97 distinct**. Twenty-one were byte-identical repeats,
and consecutive-frame luma deltas included exact zeroes.

The ratio names the cause. Encoded completions arrive at the source rate, 60/s,
while the raw DMA sustains ~50/s - so about one completion in six finds the slot
still holding the frame already delivered. `10/60` is 17%; `21/118` is 17.8%.

The sentinel test cannot see this. It answers "is a whole frame present in this
slot", which is exactly the question M217 needed, but at full rate the question
that matters is "is it a NEW one". Re-arming the sentinels after delivery does
not help, because the card re-writes the slot with the same frame.

### The fix

The card gives each frame a token, so the token is the frame identity: a
completion whose token matches what that slot last delivered is suppressed and
counted in `raw_dup_token`.

The 16-byte head is compared as well, but **only counted**, never used to
suppress. A repeat under a *new* token would mean the card re-sent a frame
rather than the host re-reading one - a different fault needing a different fix,
and worth being able to tell apart. And content can legitimately repeat: a
camera pointed at a still subject produces identical frames, and dropping those
would stall the stream. `/proc/mz0380-state` reports both counters.

### The fix was wrong, and the run said so immediately

Suppressing on the token collapsed full-rate raw from **50 fps to 1.98 fps**.

`snapshot->token` is not a frame identity. It is slot-indexed: the synthesised
path sets `snapshot.token = idx` outright (`mz0380-dma-drain.c`, "token = the
buffer index"), and the encoded drain compares its own last token against `idx`
rather than a frame counter. For a given slot the token barely changes, so the
guard matched nearly every completion and suppressed nearly every frame.

The assumption was checkable in the tree before the run, and it was not checked.

Both counters are kept, and **neither suppresses now**:

- `raw_dup_token` - how often a slot's token repeated. Now known to be almost
  always, which is what makes it useless as an identity.
- `raw_dup_content` - how often the 16-byte head repeated. This is the one that
  would distinguish a host-side re-read from the card re-sending a frame.

Content cannot become the guard either: a camera pointed at a still subject
produces identical frames, and suppressing those would stall the stream exactly
when nothing is wrong.

So the duplicates are measured and left in. A repeated frame in a raw stream is
cosmetic - it reads as a momentarily lower frame rate - while both suppression
attempts broke capture outright. Fixing it properly needs a real per-frame
identity from the card, and nothing found so far provides one. **50 fps with 18%
repeats is the current honest state of full-rate raw.**

## M220 (2026-08-28): the flicker was a torn-frame bug in M217's sentinel test - OR where it needed AND

Reported from OBS: continuous flicker on `YU12` and on the formats libv4l2
emulates from it (`BGR3`, `YV12`), while `H264` - a different delivery path
entirely - stayed clean. That asymmetry localises it to
`mz0380_drain_raw_deliver()` rather than to anything the card does.

### The bug

`mz0380_raw_probe_frame_landed()` returned `true` as soon as **one** sentinel
differed from poison:

```c
for (i = 0; i < ARRAY_SIZE(mz0380_raw_sentinels); i++)
        if (READ_ONCE(*p) != poison)
                return true;      /* ANY -> landed */
return false;
```

The sentinels are at `FRAME_SIZE-4`, `-4096`, `-65536` and `-1048576`. A
transfer still in flight has written the early offsets and not yet the late
ones, so a half-written frame passed the test. What userspace received was new
content in the leading ~2 MB and the *previous* frame still occupying the
trailing ~1 MB - a torn frame, every time the race was lost, which at full rate
is often. In OBS that is a flicker.

The M217 comment claimed the guard held because "the first sentinel sits at the
LAST dword of the frame, so it can only read as written once the transfer has
reached the end". That was true of the sentinel and false of the loop: the OR
meant sentinel[0] was never required, only reached first.

### The fix

Require **all** sentinels. Since the DMA writes in ascending order,
`FRAME_SIZE-4` is written last, so demanding every sentinel is equivalent to
demanding the transfer reached the end - which is what the guard was always
supposed to mean. It is still checked first, so an in-flight frame is rejected
on the first read.

The trade is a false negative when a real frame's own data happens to equal the
poison dword at one of four fixed offsets. That costs a single frame and cannot
persist, because the next frame's content differs.

### Why it did not show up until now

M215 and M217 were both run at `post_skip=29` - one whole frame every 30, about
2 fps. At that rate each transfer had ~500 ms to finish before the next
completion looked at the slot, so the race was never lost and the 32-frame
capture was clean and sharp. M219 was the first run at `post_skip=0`, where a
slot is revisited every ~4 frames at 50-60 fps and the window is tens of
milliseconds. **The bug was always there; only the full-rate run could expose
it.**

That is also a caution about the M217 measurement: "no tearing in any frame"
was true of what was run, and was read as though it validated the guard in
general. It validated it at 2 fps.

### What this does not explain

The byte-identical duplicates M219 measured are a separate phenomenon - a torn
frame is not byte-identical to its predecessor. `raw_dup_content` still counts
them, and they remain unexplained.

## M221 (2026-08-28): audit of the M217-M220 raw path - two more defects, both introduced this session

Prompted by "are there any other bugs that were possibly missed", after M219
and M220 had each shipped a wrong fix. Reviewing the new code rather than the
card found two more, both real, neither yet observed because the paths that
reach them had not been exercised.

### 1. Raw was silently broken at every resolution except 1080p

`MZ0380_RAW_PROBE_FRAME_SIZE` is the 1080p frame, `0x2f7600`, and the whole raw
path used it as a constant: `sizeimage`, the sentinel offsets, the `memcpy`
length and the vb2 plane check.

M218 then advertised `YU12` at 1280x720, 720x480 and 720x576 as well. At those
geometries the card writes `ALIGN16(W)*H*3/2` - 1382400 bytes at 720p - so
every sentinel sat **past the end of the frame the card actually wrote**, stayed
poison forever, and `mz0380_raw_probe_frame_landed()` could never return true.
Selecting raw at anything other than 1080p would have delivered **nothing at
all**, with no error anywhere. One fixed offset (`FRAME_SIZE - 1048576`) is
outright negative for a 720x480 frame of 518400 bytes.

Fixed by deriving the length from geometry, which is what M213 said it was all
along:

```c
size_t mz0380_raw_frame_bytes(struct mz0380_dev *dev)
{
	return (size_t)ALIGN(dev->capture.width, 16) *
	       dev->capture.height * 3 / 2;
}
```

and by expressing the sentinels as offsets **within** that frame rather than
fixed byte positions. `[0]` is still the last dword - the one that proves
completion under an ascending DMA - and the others are spread back through the
frame, which is what keeps M220's AND meaningful if the write order ever turns
out not to be ascending.

### 2. Changing format in OBS after streaming once would deliver nothing

`post_mask` bit 0 is sent by `mz0380_stream_post_proc()` during stream start.
Under `persistent_h264` a second STREAMON on a running pipeline deliberately
attaches VB2 and sends nothing else - correctly, since tinyvenc is no longer in
its initial command state and a SET_VIC would fork another process.

So H.264 -> stream -> stop -> switch to `YU12` -> stream would reuse the running
pipeline, never re-send `post_mask`, and leave the card writing 16-byte stubs.
Zero frames, no error. The M218 test only passed because the format was chosen
on a fresh load, where the first STREAMON does a full start.

`S_FMT` now sets `pipeline_reconfigure_pending` when the delivery format changes
while a pipeline is running, so the next attachment cleanly replaces the encoder
and re-sends the command - reusing the mechanism already built for HDMI timing
changes.

### Checked and found sound

- The landed test / `memcpy` race: a slot is revisited every ~4 frames (~80 ms
  at 50 fps) while the copy takes on the order of a millisecond.
- `deliver_raw` is only ever set inside the successful bank allocation, so a
  failed or skipped allocation cannot leave the node advertising a format it
  cannot deliver.
- Capability-only allocation failure is non-fatal; an explicitly requested one
  still fails loudly.
- A `seq_printf` pair joined by a comma operator was rewritten with braces. It
  behaved correctly and would not have stayed that way.

### Still open

The byte-identical duplicates from M219 remain unexplained. They are not torn
frames - a torn frame differs from its predecessor - and both suppression
attempts made things worse, so they are counted and left alone.

### M221 addendum: the verification was the defect

The M221 changes were reported as building clean and did not build at all. Two
compile errors reached the operator and cost a load: `mz0380_raw_frame_bytes`
declared in a header the video side does not include, and
`mz0380_raw_poison_dword` deleted along with the sentinel array it sat next to.

Neither is interesting. How they got through is.

The working tree could not be built non-root - the hardware scripts run `make`
as root and leave root-owned objects - so the check was done in a copy under
/tmp. That copy was refreshed by removing `src/` and `Makefile` and copying them
back, and then judged by `ls mz0380.ko`. **A stale `.ko` from an earlier
successful build was still sitting there.** The test passed on a leftover file,
every time, regardless of whether `make` had succeeded.

So the check reported success on a tree that had never compiled, twice in a row,
and looked exactly like a real verification in the transcript.

`scripts/mz0380-build-check.sh` now does the two things that were missing: it
starts from an empty directory, so nothing from a previous run can be mistaken
for this one's output, and it reports **make's exit status** rather than the
presence of a file. It needs no root and touches no hardware, so there is no
longer a reason to skip it.

The declaration itself now lives in `mz0380.h`, the only header both sides
share. `mz0380-internal.h` is reachable from the video side and
`mz0380-dma-internal.h` from the DMA side; putting it in either one built clean
on one side and failed on the other.

## M222 (2026-08-28): the flicker was never one bug - the token does not name the raw slot

Two reports after M220/M221 landed: the flicker was still there at 1080p, and
selecting 1280x720 "crashed the driver".

### The 720p report was not a crash

The driver refused, correctly, and said why:

```
refusing to stream: the source is 1920x1080 but the buffers were allocated for
1280x720, and this path has no scaler - the card would write 3110400 bytes into
a buffer described as 1382400
```

The module stayed loaded. **The card has no scaler**: it produces at the source
geometry and nothing else. So `VIDIOC_ENUM_FRAMESIZES` returning the whole mode
table was itself the fault - it reads as a menu of supported resolutions, and
every entry except the live source one is a trap that fails at STREAMON, which
in an application looks like the driver falling over. It now reports the live
source geometry alone, falling back to the table only when no geometry is known
yet.

This also corrects M221, which called the raw path "broken at every resolution
except 1080p". Deriving the length from geometry was right and remains, but the
resolutions it was fixing were never reachable in the first place.

### The flicker: the token does not name the raw slot

M217 picked the slot with `idx = snapshot->token & 7`, on the assumption that
the encoded completion's token indexes the raw ring the same way it indexes the
encoded one. Nothing ever established that, and the M212 head sampler had
already contradicted it: across one run the four op02 slots changed **2, 219,
217 and 230** times. Four slots taking turns produce four similar counts. These
do not.

So a token-selected slot was frequently not the one the card had just written -
stale, which is the byte-identical duplicates M219 measured, or mid-write, which
is the flicker. M220's OR-to-AND fix was a real bug fixed, but it only closed
one of the ways a torn frame could arrive, which is why the flicker survived it.

`mz0380_drain_raw_deliver()` now ignores the token and scans bank0's four slots,
taking any that carries a complete frame. Bank1 is skipped outright: M215 found
it untouched with its poison intact.

### A completeness test that survives the copy

Passing the sentinel test proves a whole frame was present when the sentinels
were read. It says nothing about the two milliseconds the `memcpy` then takes,
during which the card may begin overwriting that same slot.

So the sentinels are now re-poisoned **before** the copy and re-read after it. If
the card wrote into the slot meanwhile it will have put its own bytes back over
them, the frame is discarded instead of delivered torn, and the vb2 buffer is
returned to the queue rather than consumed. `raw_frames_torn` counts it.

That makes the guard cover the whole window between deciding a frame is complete
and finishing with it, which neither M217 nor M220 did.

### Standing correction

Three separate explanations have now been offered for this flicker - a partial
frame passing an OR test, a wrong frame size, and a wrong slot. Only the last
one accounts for the duplicates as well, and only the tear detector closes the
copy window regardless of which slot the card is writing. The first two were
real defects; neither was the reported symptom's cause.

## M223 (2026-08-28): the stuck node, and admitting the flicker has not been diagnosed

### The 720p report was a wedge, not a crash, and it was mine

```
frame size : 1280x720      <- what S_FMT accepted
source     : 1920x1080     <- what the card actually produces
pipeline   : stopped, replacement pending
```

Switching back to H.264 "did nothing" because the pixelformat was never the
problem - `/proc` shows it was already `H264`. The **geometry** was stuck at
720p, so every STREAMON was refused by the no-scaler check, permanently.

`S_FMT` accepted a size the hardware cannot produce and nothing ever put it
back. V4L2 requires TRY_FMT and S_FMT to return the format that will actually be
used; for a device whose input geometry is whatever the source is sending, that
means answering with the source geometry rather than accepting a request that
can only fail later. `mz0380_clamp_to_source()` now does that in both, using the
receiver's measured `capture.source_width/height`.

That closes the trap at its origin: an application asking for 720p is told
1920x1080 and uses it, instead of being accepted and then refused forever.

### The flicker is still not diagnosed, and the record should say so

Three causes have been proposed and three fixes shipped:

| | proposed cause | outcome |
|---|---|---|
| M220 | partial frame passing an OR sentinel test | real bug, fixed, flicker remained |
| M221 | frame size hardcoded to 1080p | real bug, fixed, flicker remained |
| M222 | token does not name the raw slot; no tear detection | real bugs, fixed, flicker remained |

Each was a genuine defect found by inspection. None was the reported symptom's
cause, and each was presented with more confidence than a fix validated only by
reasoning deserves.

M222 also introduced a fourth candidate while claiming to fix the third: it
delivered **every** landed slot per completion, in slot order. If two slots hold
frames captured at different moments, that emits them out of time order, and
out-of-order frames flicker exactly like torn ones.

### What this change does instead of guessing again

`mz0380_drain_raw_deliver()` now takes **at most one frame per completion**,
which matches the frame clock the completions arrive on, and starts each scan
after the slot taken last so the four are consumed in rotation. If the card
fills a four-slot ring in rotation, consuming them in rotation preserves order
without needing to know it.

And it traces, because inspection has now been wrong three times:

```
raw scan landed=0x6 took slot 1 (multi=... torn=...)
```

- `landed` - which slots held a complete frame when the scan ran. **More than
  one bit set means frames are queueing and ordering genuinely matters; exactly
  one means the pacing is right; zero means the completion beat the DMA.**
- `raw_multi_landed` and `raw_frames_torn` are in `/proc/mz0380-state`.

Whether the flicker survives this is the measurement. If `landed` is usually a
single bit and the picture is still wrong, ordering is not the cause either and
the next suspect is the completion source itself - an encoded completion may
simply not indicate that any raw frame is ready.

## M224 (2026-08-28): the frames were never the problem - the timestamps were

The M223 trace settled what three rounds of inspection could not:

```
raw scan landed=0x8 took slot 3 (multi=0 torn=0)
raw scan landed=0x1 took slot 0 (multi=0 torn=0)
raw scan landed=0x2 took slot 1 (multi=0 torn=0)
raw scan landed=0x4 took slot 2 (multi=0 torn=0)
```

Exactly one slot ready per completion, rotating cleanly 0-1-2-3, `multi=0`,
`torn=0`. So frames are not queueing, delivery order cannot be wrong, and the
tear detector never fires.

An 89-frame capture taken from the running driver confirms it from the other
end: **no poison anywhere** (so no region of any frame went unwritten), no
tears, luma mean flat at 121.8 across the run, chroma at 135/129, frames
arriving in slot rotation.

The delivered pictures are correct. Every content-side hypothesis is dead.

### What was actually wrong

```c
vbuf->vb.vb2_buf.timestamp = snapshot->timestamp_ns;
```

That was defensible under M217, where the slot was selected **from that
completion's token**, so the completion and the frame were the same event. M222
stopped doing that - slots are scanned and consumed in rotation - and the line
was left alone. From that point the timestamp stamped on a raw frame belonged to
a different frame.

Combined with delivering ~50 fps against the 60 the node advertises, what a
renderer received was good pictures carrying other frames' timestamps at an
irregular cadence. That presents as stutter, and stutter is what "flickering on
every format except H.264" describes: the encoded path delivers on the very
completion whose timestamp it stamps, so it never had the fault.

Now stamped with `ktime_get_ns()` at delivery.

### Method note

This is the fourth explanation offered for the same symptom, and the first
reached by measurement rather than inspection. The three before it - an OR
sentinel test, a hardcoded frame size, a token that does not name the slot -
were all real defects and all fixed, and none of them was the reported problem.
The instrumentation added in M223 answered it in one run.

The pattern is worth naming: each earlier fix was found by reading the code,
confirmed by reasoning, and shipped as though reasoning were evidence. The
counters cost one run and ended it.

## M225 (2026-08-29): the raw flicker is an H.264 placeholder delivered to an I420 node

Symptom, as the operator describes it: frames go black or blank intermittently
and the picture is correct whenever it is present. Flashing - not stutter, and
not corruption. That distinction is what retires M220/M221/M222/M224 at once:
each of those was a claim about the content or the timing of the raw frames,
and an 89-frame capture off the running driver had no poison, no tears, flat
luma and correct colour. The black frames were never raw frames.

`mz0380_no_signal_work_fn` (`src/mz0380-no-signal.c`) replays a self-contained
1920x1080 H.264 IDR from host memory into the shared VB2 queue. Its gates were
`streaming`, `no_signal_active` and `mz0380_h264_probe`. None of them mentions
the negotiated pixel format, and raw delivery satisfies all three: the encoder
must keep running for the card to produce raw at all (M210b), so `h264_probe`
is set in exactly the configuration raw needs.

Activation sites reached during a normal raw session:

- `mz0380_start_streaming`, persistent-pipeline reattachment - "validating HDMI
  state at persistent reattachment" (`src/mz0380-vb2.c`).
- the receiver monitor, on unlock and on producer silence
  (`src/mz0380-signal.c`).

Each tick took a buffer off `dev->buf_list` at up to `no_signal_fps` and
returned it holding `mz0380_no_signal_h264_len` bytes of Annex-B with
`bytesused` set to match. An I420 consumer renders that as a black or garbage
frame. `H264` never showed it because there the placeholder is what the node
promised - it is the whole point of the feature.

Fix: `mz0380_no_signal_replay_allowed()` withholds the placeholder whenever
`dev->deliver_raw` is set. The raw queue holds instead, which is the correct
presentation for an unlocked source.

Instrumentation, deliberately kept: the work item is still **scheduled on the
same cadence** in raw mode and counts rather than delivers, so
`no_signal_frames_suppressed` ("withheld from a raw node" in
`/proc/mz0380-state`) reports exactly how many buffers the previous code was
corrupting. One run therefore tests the fix and the diagnosis together - a
non-zero count with the flicker gone confirms both, and a zero count falsifies
M225 before a sixth theory can be built on it. This exists because the flicker
had been diagnosed wrong four times from inspection alone.

**Status: FALSIFIED on hardware, 2026-08-29.** The counter did its job. A
~16 second `ffplay -input_format yuv420p` run on `/dev/video0` reported:

```
pixelformat: YU12
raw frames : 637 delivered, 0 dropped, 10 stub completions skipped
raw repeats: 129 identical-head observations, 0 torn, 0 completions with >1 slot ready
no signal  : inactive, 0 placeholder IDRs delivered, 19 withheld from a raw node (M225)
```

The mechanism was real and live: `no_signal_fps` is 2, so 19 withheld ticks
means the placeholder was armed for roughly 9.5 of those 16 seconds and the old
code would have injected about 19 black frames into the stream. **And the
flashing was unchanged.** So the placeholder was a genuine defect worth fixing -
it cannot be allowed to hand H.264 to an I420 node - but it is not the cause,
which makes it the fifth wrong diagnosis of this symptom.

The fix and the counter stay. What the same run rules out matters more: 0
dropped, 0 torn, 0 multi-slot, 0 placeholders delivered. The black frames reach
userspace through the ordinary delivery path, as ordinary frames.

Note also 129 identical-head observations in 637 delivered - about 20% of
frames repeat the previous frame's first bytes. That is a separate open
question and is judder, not flashing.

## M226 (2026-08-29): the untested possibility - are the delivered frames themselves black?

`mz0380_raw_probe_frame_landed` decides a slot holds a complete frame by
checking that the poison sentinels have been overwritten. A blank frame
overwrites them exactly as a picture does. The test therefore proves the card
wrote *something*, and has never been able to distinguish a picture from a
black rectangle. If the card emits an occasional blank into the raw bank - and
nothing so far has looked - it is delivered as a good frame and presents
precisely as intermittent flashing.

Every content-side claim so far rests on one 89-frame capture described as
having flat luma and correct colour. 89 frames is under two seconds; at the
observed rate of flashing that is a handful of events, and a per-frame scan was
never run.

`scripts/mz0380-m226-blackframe-scan.py` reads I420 frames from stdin and
reports per-frame luma, so nothing hits the disk:

```
ffmpeg -f v4l2 -input_format yuv420p -video_size 1920x1080 \
       -i /dev/video0 -frames:v 600 -f rawvideo - 2>/dev/null \
  | ./scripts/mz0380-m226-blackframe-scan.py
```

It is written to be falsifiable in both directions and says so in its own
output: blanks found means the flashing is content the driver was handed, and
no blanks found means the payload is innocent and the next suspect is pacing or
the consumer - explicitly not a licence to build a fix on the file's silence.

Status: script self-tested against synthetic frames; hardware run outstanding.

## M227 (2026-08-30): what the Windows driver offers that ours does not

Screenshots of the Windows OBS property pages for "Game Capture HD60 Pro" are
in `/run/media/wolffyx/Work/hd60-trace/images/`. The Video Format dropdown is
populated by the Windows driver with:

```
Any, XRGB, NV12, YV12, YUY2, H264
```

Ours enumerates `H264` and `YU12` only. The raw bank payload is confirmed
planar I420 on our side (M215/M217), and `YV12` differs from `YU12` only in
chroma plane order while `NV12` is semi-planar, so these are most likely
DirectShow-side conversions of one hardware format rather than six distinct
card modes. `XRGB` and `YUY2` would need a colour conversion somewhere. None of
this has been tested.

Recorded as a capability gap, not a defect, and explicitly not to be chased
while the raw flicker is open.

Other pages, for the record: Resolution/FPS Type `Custom` -> 1920x1080; FPS
list runs from "Match Output FPS" and "Highest FPS" down to 1; Color Space
offers Default/Rec.709/Rec.601/Rec.2100 PQ/Rec.2100 HLG; Color Range Default;
Buffering Auto-Detect/Enable/Disable; Audio Output Mode "Capture audio only".

A separate "Video Decoder" property sheet shows Video Standard `NTSC_M` with
`Lines detected: 525` and greyed-out "VCR Input"/"Output Enable". That is a
generic DirectShow analogue-decoder dialog and reads as vestigial UI for an
HDMI capture card - worth remembering before anyone reads 525 lines as a
statement about this hardware.

## M228 (2026-08-30): the card CLEARS a slot before filling it, so "landed" fires on the clear

M226 stopped the guessing by measuring. Two 1200-frame captures and one
600-frame capture off `/dev/video0` in `YU12`:

- 5 to 11 fully blank frames per 1200, classified as Y=0/1 with both chroma
  planes at exactly 128.
- alongside them, partially black frames: picture on top, black to the bottom,
  with the boundary at roughly row 700, 811, 902 and 930 in different frames.
- an 18-band luma profile of one partial frame showed real picture down to the
  boundary and then a flat band reading **0x01**, to the last row.

0x01 is the decisive number. Bank poison is 0xa5 and 0x5a, and the frame
buffers use 0xaa; none of them is 0x01. So the tail was written by the CARD,
and 0x01 is luma black. Chroma in the same frames reads 0x80.

The card therefore clears a slot to black - luma 0x01, chroma 0x80 - and then
fills it with the picture, ascending. `mz0380_raw_probe_frame_landed` tests
whether four sentinels still hold poison, and the clear overwrites every byte
of the frame including all four. So the completeness test passes the instant
the CLEAR lands, and the copy takes whatever has been filled so far: an
all-black frame when it runs early, picture-over-black when it runs mid-fill.

That is the flicker, and it explains the whole history. The frames were never
torn in the sense M220/M222 assumed - the tear detector was working - and the
delivered content was exactly what the buffer held. Every earlier theory asked
what was wrong with the frame; none asked whether the transfer had finished,
because the sentinel test was believed to answer that.

Two readings were tried and rejected on the way here, both recorded so neither
is repeated. A constant boundary at row 811 in the first capture suggested a
half-height 1920x540 frame, since 1920*540*3/2 is exactly 1555200 bytes, half
of 3110400 - the band profile refuted it, because the tail would then read 128
(chroma misread as luma) and it reads 1. And the boundary is not constant once
more frames are examined.

**Fix.** No sentinel value or offset can survive a clear that touches every
byte, so the completeness test stops asking the content. A fill takes about
2 ms and these completions arrive about 16 ms apart, so a slot still landed on
the NEXT scan has certainly finished. `mz0380_drain_raw_deliver` now delivers
only slots seen landed on two consecutive scans, costs one completion of
latency, and clears the delivered slot from the record so it must be observed
twice again. The card revisits a given slot every fourth frame, so this cannot
starve.

`raw_deferred_fills` in `/proc/mz0380-state` counts the completions held back.
It is the falsifier: if the flashing survives with that counter at zero, the
deferral never engaged and this diagnosis is wrong; if it is large and the
scanner still reports blanks and partial fills, the clear-then-fill reading is
wrong. `scripts/mz0380-m226-blackframe-scan.py` remains the ground truth.

**Status: FALSIFIED on hardware, 2026-08-30, by its own counter.**

```
raw frames : 1001 delivered, 0 dropped, 0 stub completions skipped
raw fills  : 1 completions deferred one scan for the card's fill to finish
raw repeats: 277 identical-head, 0 torn, 1947 completions with >1 slot ready
```

One deferral in 1200 frames, and the scanner still reported 9 blank frames.
The gate never closed, because the premise was wrong: a slot the card cleared
stays "landed" indefinitely. Only the DELIVERED slot is re-poisoned, so the
other three keep satisfying the test forever and the second observation was
always already true. The same run recorded 1947 completions with more than one
slot ready against zero under M223 - the same fact from the other side, and the
number that should have been read as a warning before shipping the deferral.

The measurement in M226 stands; only the M228 remedy was wrong.

## M229 (2026-08-30): test for the CLEAR pattern, not for absent poison

M228 failed because it reasoned about time. The clear leaves a signature that
can be tested directly instead: if the last luma rows still read entirely as
the card's clear byte, the fill has not reached the end of the picture.

`mz0380_raw_probe_frame_filled()` samples 32 dwords across the final 64 luma
rows and returns false only when every one of them equals the clear value. Any
single differing sample proves the region was written, so the test is
deliberately permissive - it rejects a uniformly-clear tail and nothing else.
Sampled rather than scanned, because reading the whole tail per completion
would cost more bandwidth than the copy it guards.

The clear byte is `raw_clear_byte`, a writable module parameter defaulting to
1, which is what M226 measured. It is a parameter for two reasons: a capture
showing a different clear value needs no rebuild, and setting it to a byte the
card never writes disables the test, which is how to check whether the test
itself is what changed the result.

`raw_incomplete_tail` in `/proc/mz0380-state` ("raw fills") counts slots
skipped this way, and is the falsifier. Zero with the flashing still present
means the test never fired and this is wrong too. A large count with blanks
still reaching userspace means the tail is not where the fill ends.

The false negative is a frame whose last rows are genuinely flat black: it is
held back and the next completion delivers, costing one frame of an already
black picture. The opposite trade - passing a half-filled frame - is the defect
this exists to stop.

**Status: CONFIRMED on hardware, 2026-08-30.** A 1200-frame capture in `YU12`:

```
partial fills : 0
blank frames  : 0            (luma mean range 16.91 .. 118.11)
raw frames : 992 delivered, 0 dropped, 20 stub completions skipped
raw fills  : 80 slots skipped because their last luma rows were still the clear byte
raw repeats: 167 identical-head, 0 torn, 0 completions with >1 slot ready
```

Zero blank frames and zero partial fills, against 9 blanks and a dozen partial
fills in every previous capture. The test fired 80 times, so it engaged rather
than passing by accident. `completions with >1 slot ready` also fell from 1947
to 0, which is the accumulating-landed pathology M228 stumbled over: rejecting
unfilled slots stops them piling up as permanently-ready.

The raw I420 payload is clean. Six theories were needed to get here and five of
them were wrong; what closed it was measuring the delivered bytes
(`scripts/mz0380-m226-blackframe-scan.py`) instead of reading the code.

## M230 (2026-08-30): a SECOND flicker, and it is not the same defect

With M229 in place the operator still reports black flicker in OBS on the
non-`YU12` formats, lasting **0.5 to 1 second** at a time.

That duration is the finding. At 60 fps it is 30 to 60 consecutive black
frames, where M226/M229 were about single dropped-in blanks. A 1200-frame
capture covering 20 seconds found zero blanks, so whatever this is does not
reach the I420 payload the same way.

What it could be, none of it yet tested:

- OBS is not on the raw path at all for those formats. The node enumerates only
  `H264` and `YU12`; `BGR3`/`YV12` come from libv4l2 emulation, which may fall
  back to `H264` rather than converting from `YU12`. The H.264 path has its own
  open defect (see below), and `pixelformat` in `/proc/mz0380-state` says which
  path a session is really using.
- Buffer starvation over a run of completions, which shows as
  `raw frames ... dropped`.
- The NO SIGNAL placeholder, which at `no_signal_fps=2` would black the picture
  for exactly this kind of duration - but M225 withholds it from a raw node, so
  this only applies if the session is not raw.

Also open and probably related to the first bullet: after a raw session the
encoded path came back with `0 cached SPS/PPS bytes`, 274 dropped and 0
delivered, which is why OBS showed black on `H264` at that point.

`scripts/mz0380-m230-flicker-watch.sh` samples the counters that separate these
so one OBS session identifies which, instead of one session per guess.

### A third false pass from the build checker, and what it was

`scripts/mz0380-build-check.sh` reported "BUILD OK" for a tree that could not
compile - `mz0380-dma-drain.c` still referenced two struct members that had
been deleted. It copies `Makefile` and `src` into an empty directory, and
`cp -r src` brought the working tree's `.o` and `.cmd` files with it. Those
objects are newer than the sources, so kbuild skipped compiling, linked them,
and produced a `.ko`.

The copy now has every build product stripped from it before make runs. This is
the third false pass from this script: first a stale `mz0380.ko` satisfying an
`ls` test, then `-v` being forwarded to make so `make -v` exited 0 without
building, and now copied objects. Each time the failure mode was the check
answering a question adjacent to the one being asked.

## M231 (2026-08-30): the second flicker - raw capture starves the liveness stamp and the monitor re-arms HDMI mid-capture

M230's counter watch answered it in one session. Three phases in one log, same
source throughout, a live 1920x1080p60:

```
[35-47s]  fmt=H264  recov=idle    ph never SHOWING          h264deliv climbing
[52-98s]  fmt=YU12  recov=active  ph=SHOWING <-> not shown   h264deliv=0
                    flapping every 1-2s for the whole 50-second session
[102-116s] fmt=H264 recov=idle    ph never SHOWING          h264deliv climbing
```

The receiver monitor declared "no usable source" continuously during raw
capture and never once during encoded capture, on the same unchanged HDMI
input.

The cause is a starved timestamp. `mz0380_signal_monitor` decides a configured
pipeline has gone silent by comparing `last_h264_frame_stamp` against
`hotplug_stall_ms`, which defaults to **1500 ms**. Every write to that stamp
lives in the encoded delivery path, and `mz0380_drain_frame_snapshot` returns
early for `deliver_raw` before reaching any of them. So in a raw session the
stamp freezes at STREAMON and is stale 1.5 seconds later, forever after, while
raw frames arrive normally - `rawdeliv` climbed steadily right through it.

1500 ms is also the observed flap period, and the operator describes the black
as lasting 0.5 to 1 second.

What the monitor then does is the damage. It is not only cosmetic: it declares
NO SIGNAL, sets `signal_recovering`, and calls
`mz0380_mst3367_read_lock(dev, &locked, true)` - the acquisition RE-ARM, which
writes to the receiver - in the middle of an active capture, roughly every 1.5
seconds for as long as raw streams. M225 keeps the placeholder frames off a raw
node (`phdeliv=0`, `phheld` climbing 1 to 66 across the session), so what
reached the screen was never the placeholder. It was the receiver being
disturbed mid-capture.

This is very likely the same mechanism behind the other open item: the encoded
path coming back with `0 cached SPS/PPS bytes` and delivering nothing after a
raw session. Dozens of unnecessary acquisition re-arms is not a state the
encoder was expected to survive.

**Fix.** A raw completion is itself proof the producer is alive, so
`mz0380_drain_frame_snapshot` stamps `last_h264_frame_stamp` on the
`deliver_raw` branch before dispatching. One line, in the one place the
information exists.

**Falsifier**, and it needs no new instrument: re-run
`scripts/mz0380-m230-flicker-watch.sh` over a raw session. `recov` must stay
`idle` and `ph` must stay `not shown` for the whole session. If they still flap,
the stamp was not the gate. If they stop flapping and the black persists, the
black is not the re-arm.

Note the shape of this one against the six that preceded it. Every earlier
theory came from reading the raw delivery path; this came from watching two
counters that had no obvious connection to it, in a session that also contained
a working H.264 capture for contrast. The contrast is what made it visible.

## M231 confirmed (2026-08-30), and what survived it

Re-run of the M230 counter watch with the liveness stamp in place, same source,
same session structure - H.264 capture, raw capture, H.264 capture:

| | recovery events during raw | `phheld` | encoded path after the raw session |
|---|---|---|---|
| before M231 | flapping every 1-2s for the whole 50s | 1 -> 66 | `sps=0`, 274 dropped, 0 delivered |
| after M231 | **2 events in 23s** (t=25s, t=33s) | 1 -> 2 | `sps=39`, delivering, **0 dropped** |

The 1500 ms stall cadence is gone. The encoded path also recovers cleanly after
a raw session now, which was a separate open item and is independent
confirmation of the diagnosis: the spurious acquisition re-arms were what left
the encoder without parameter sets.

**Two recovery events survive**, 8 seconds apart, so they are not the stall
timer. The remaining trigger is the other branch of `mz0380_signal_monitor`:
`mz0380_mst3367_read_lock` returning `locked == false`. `signal_poll_ms` is
4000, so a receiver read that reports unlocked on a live source is enough to
declare NO SIGNAL, enter recovery, and re-arm acquisition. `rawtorn` also went
0 -> 1 at t=33s, alongside one of the two events.

Not yet established whether those reads are genuine momentary unlocks or
misreads of a mailbox-backed I2C register while raw DMA is saturating the link.
A single unlocked read is thin evidence for tearing down a working capture
either way, and a consecutive-reads debounce is the obvious remedy - but the
visible effect of two events in 23 seconds has not been measured yet, so it is
not yet known whether this is worth fixing.

## M157 answered (2026-08-30): the wedge cliff moved

`mz0380-spawns.sh` reported **18 spawns on one power cycle with no wedge**, and
flagged it as M157's open question. With `vic_fast_kill=0` - which is the
driver default - the historical 8-to-18 wedge band did not bite.

This does not license unlimited spawns; it records that the band was measured
under a configuration that is no longer the default, and that the current
default survived the top of it once. Treat 18 as observed-safe-once, not as a
new ceiling, until a second power cycle repeats it.

## The raw flicker is CLOSED (2026-08-30)

Operator confirmation on a `YU12` OBS session with M229 and M231 in place: **no
black at all**. Two defects, closed by two different kinds of evidence.

- **M229** - single black and part-black frames. Closed by measurement: 0 blank
  frames and 0 partial fills in a 1200-frame capture, against 9 and a dozen in
  every capture before it.
- **M231** - the 0.5-to-1-second blacks. Closed by observation, with the
  counters agreeing: recovery events during raw fell from continuous flapping
  to 2 in 23 seconds, and the encoded path stopped coming back broken.

It took seven attempts. Five were wrong (M220, M221, M222, M224, M225) and two
were right (M229, M231), and the two right ones were not found by reading the
delivery path - which is where all five wrong ones came from. M229 came from
scanning the delivered bytes and finding the tail read 0x01, a value no poison
in this driver uses. M231 came from watching two counters with no obvious
connection to raw delivery, in a session that also contained a working H.264
capture for contrast.

The method that worked, for whoever is here next: measure what actually arrives,
put a falsifier in every fix, and get a working case in the same trace as the
broken one.

### What is open after it

- **Duplicate frames.** `rawdup` reached 209 identical-head observations in 1103
  delivered, about 19%. The operator describes it as the image "not refreshing"
  when something new enters the camera's view. This is judder, not flashing, and
  is the next defect in the raw path.
- **Two recovery events per ~23s of raw**, from `mz0380_mst3367_read_lock`
  reporting unlocked on a live source. Now visually harmless; a
  consecutive-reads debounce is the obvious remedy if it ever matters.
- **Picture quality against Windows.** Side-by-side captures of the same scene
  show the Linux image noisier and harsher than the Windows one. Suspects are
  colour range and the sharpness control default (`sharp=128`), neither
  investigated. See [[M227]] for the Windows-side settings that were captured.
- **Format enumeration.** Windows offers XRGB/NV12/YV12/YUY2/H264; the node
  offers H264 and YU12 (M227).

## M232 (2026-08-31): a dead persistent encoder has no escape path

Reported as "the card video does load very hard or not at all", with a black
image in OBS. `/proc/mz0380-state` at the time:

```
pixelformat: YU12
pipeline   : running, VB2 detached, 20 attachment(s), 0 cached SPS/PPS bytes
h264 frames: 0 delivered      raw frames : 0 delivered, 0 stub skipped
raw fills  : 0                placeholder: 188 withheld from a raw node
recovery   : idle, last card H.264 access unit 12863ms ago
```

Zero completions of either kind, so nothing downstream of a completion ran -
M225, M229 and M231 all live there and none of them could have executed. `raw
fills : 0` in particular proves the M229 test never rejected anything.

The kernel log shows the loop, repeated for attachments 15 through 20:

```
VB2 detached; persistent H.264 pipeline ... remain active (no STOP, no SET_VIC budget spent)
NO SIGNAL state entered (validating HDMI state at persistent reattachment): node is
    delivering raw, so no placeholder is sent and the queue holds
VB2 attached to persistent H.264 pipeline (attachment 20, SET_VIC spawns remain 1)
MST3367 signal: 1920x1080p [host units] (htot=2200 vtot=1125 hact=1920 R55=0x7f)
HDMI reconnect lock on recovery attempt 1: 1920x1080p@60; persistent encoder
    unchanged, holding NO SIGNAL until clean live IDR
```

The receiver was locked and reporting perfect timing the whole time. The
producer was silent for 12.8 seconds. The driver confirmed the lock about twenty
times, concluded "persistent encoder unchanged", and never restarted it.
**There is no path that respawns a persistent encoder that has gone silent**,
short of a module reload - which did fix it immediately.

M225 did not cause this and did make it visible. Before it, the placeholder was
pushed into the raw queue, so a consumer received frames - wrong-format garbage,
but frames - and stayed attached. Withholding them correctly means a consumer
gets nothing, times out, and reattaches, and each reattachment re-enters NO
SIGNAL. That is the "loads very hard" behaviour: OBS cycling once a second
against a pipeline that will never produce.

**Not reproduced since.** A clean reload, then a 33-second OBS session in
`YU12`, ran healthy: one attachment, `rawdeliv` 887 -> 2486 at about 64 fps,
`rawdrop` 8, recovery idle apart from three momentary blips, `phheld` 4 against
188 in the failing session. `ffplay` never triggered it at all.

No fix is proposed yet, because the trigger is unknown - what killed the
producer in the first place is not in the captured log, which begins after the
silence started. Two candidates when it recurs: a bounded escape that stops and
restarts the pipeline after a good lock plus sustained silence (costs one
SET_VIC from the wedge budget, so it needs a hard limit), or refusing the
reattachment shortcut when the pipeline has produced nothing since the last
attach. Capture the log from BEFORE the first silent second next time; the
existing `scripts/mz0380-m230-flicker-watch.sh` running from before OBS starts
is enough.

Workaround meanwhile: `sudo ./mz0380-live.sh unload && ... load` clears it, and
`ffplay -f v4l2 -input_format yuv420p -video_size 1920x1080 /dev/video0` is the
known-good raw consumer.

### Duplicate frames confirmed at ~20%

The healthy session also quantified the next defect: `rawdup` climbed 1026 ->
1345 while `rawdeliv` climbed 899 -> 2486, so 319 duplicates in 1587 delivered
frames, about 20%. Consistent with the earlier 209 in 1103. This is what the
operator sees as the image "not refreshing" when a new object enters view.

## M233 (2026-08-31): the picture brightens steadily because AUTO_POSITION is latched on during capture

Operator report: with the camera at 60 Hz the image gets brighter continuously
over a capture. Windows does not do this.

`MST3367_B0_AUTO_POSITION` (BANK0 0xe2, ON = 0x80) starts the receiver's
position/phase hunt. Two callers use it, and only one of them is symmetric:

- `mz0380_mst3367_read_signal` enables it while hunting for a mode and
  **disables it at the `matched:` label** before committing digital output. The
  hunt is bounded.
- `mz0380_mst3367_read_lock(dev, &locked, rearm_acquisition=true)` turns it ON
  and **never turns it off**. Nothing else clears it, so the receiver was left
  auto-adjusting for the remainder of the capture.

That second caller is the recovery re-arm. `mst3367_set_auto_position` carries
the M74 guard that exists to prevent exactly this - "never re-arm acquisition
while the encoder is capturing... can only disturb the very frames we are trying
to capture" - but the guard exempts `signal_recovering`, and
`mz0380_signal_monitor` sets that flag immediately before calling. The one
caller the guard needed to catch is the one it lets through.

Note how this compounds with M231. Before M231 the monitor declared the producer
silent every 1500 ms during raw capture, so this latch was being re-applied
several times a second; M231 cut that to roughly one event per ten seconds,
which is why the symptom reads as a slow steady drift rather than instability.
The two remaining recovery events per 23 seconds recorded there are enough to
keep re-arming a receiver that never needed it.

**Fix.** A locked receiver has nothing to re-acquire, so re-arm only when the
lock byte says the signal is actually lost:

```c
if (rearm_acquisition && !*locked)
```

This keeps re-acquisition on genuine loss, which is what the flag was for, and
stops the write reaching a receiver that is mid-capture and locked.

`mst_rearms` in `/proc/mz0380-state` counts the re-arms that still happen, and
is the falsifier. On a healthy locked capture it must stay at 0. If the
brightening persists with that counter at 0, AUTO_POSITION was not the cause and
the next suspects are the CSC/range path (`mz0380_mst3367_apply_csc_mode`) or an
auto-gain on the card itself - neither investigated.

Untested on hardware.

## M234 (2026-08-31): the raw node reports limited range for a full-range payload

Operator report, separate from the M233 drift: the Linux picture is brighter
than Windows from the moment the driver loads. A constant offset, not a creep.

`mz0380_fill_pix_format` set `V4L2_QUANTIZATION_LIM_RANGE` for every format,
unconditionally. A 600-frame `YU12` capture measured:

```
luma  min/max     : 51 / 254
chroma min/max    : 49 / 169
luma below 16     : 0 of 1248000 sampled (0.000%)
luma above 235    : 52830 of 1248000 sampled (4.233%)
```

Limited-range luma lives in 16..235. Isolated superwhite happens, but 4.2% of
sampled pixels above 235 with a hard maximum of 254 is a full-range payload.
M226 separately measured the card writing its own black as 0-1 rather than 16.

Declaring full-range data as limited makes every consumer expand 16..235 to
0..255 a second time: mid-tones lift, highlights clip, and the picture reads as
brighter and harsher - which is also the "less clear than Windows" impression
recorded in M227.

**What this capture did NOT establish**, and it matters: the black end. The
darkest pixel in frame was 51, so nothing here proves where black sits. The
verdict rests on the high end plus the card's clear value.

**Fix.** Report full range on the raw node. It is a module parameter,
`raw_full_range` (default 1), so the two can be compared against Windows in
seconds without a rebuild - which is the right way to settle a question whose
evidence is one-sided.

Raw only. The H.264 bitstream carries its own VUI, so what this node claims
about an encoded stream is not what a decoder obeys, and relabelling it could
mislead a consumer that is currently correct.

Untested on hardware. The check is whether the picture matches Windows with
`raw_full_range=1` and regains the excess brightness at 0:

```bash
sudo sh -c 'echo 0 > /sys/module/mz0380/parameters/raw_full_range'
```

A better capture would point the camera at something genuinely black and
re-run the M234 range check; a full-range source should then show luma near 0
rather than near 16, which is the half of the evidence still missing.

## M236 (2026-08-31): the watch script was freezing the machine, and perturbing its own measurements

Operator report: `scripts/mz0380-m230-flicker-watch.sh` froze the desktop for
about a second, every one to two seconds.

Every read of `/proc/mz0380-state` ran the chip-0x90 bridge probe in
`mz0380_fw_info_dump` - **five `mz0380_periph_read` calls, each a full mailbox
command**: write params, ring the doorbell, wait on the event bit, ack. The
watcher polled at 5 Hz, so 25 mailbox transactions per second were competing
with a running 60 fps capture for the same mailbox.

The freeze is the visible cost. The methodological cost is worse and is the
reason this is recorded rather than quietly patched: **a diagnostic that
competes for the mailbox is a participant in what it measures.** Recovery
events, lock losses and `rearms` counted while polling at 5 Hz cannot be
attributed cleanly to the driver - the instrument may have provoked some of
them.

What that does and does not invalidate:

- **M231 stands.** Its evidence is a before/after contrast recorded at the same
  polling rate, with a working H.264 capture in the same trace as the broken raw
  one. A constant perturbation cannot explain a difference that tracks the
  format.
- **Absolute counts from those traces are suspect.** "Two recovery events in 23
  seconds" (M231) and the residual lock losses behind M233 were measured under
  this load. They may be lower, or absent, when nobody is polling.
- **M226/M229/M234 are unaffected.** Those come from the frame scanner reading
  delivered bytes, which touches no mailbox.

Fixes: the bridge probe now requires `procfs_verbosity=3`, so it is opt-in for
RE work rather than running on every status read; and the watcher polls at 1
second instead of 0.2, overridable with `INTERVAL=`.

Re-measuring the residual M233 lock losses with the probe gated is worth doing
before anyone treats them as real.

## M234 RETRACTED, and M235: the brightness STEPS, it does not drift (2026-08-31)

A 600-frame capture decimated to 2 fps - five minutes of the same scene, with
the M236 mailbox probe gated off so nothing was competing for the card:

```
frames  10-279 : mean 78.24 .. 78.29      flat to +/-0.03 over two minutes
frames 280-299 : 79.43 -> 80.76           one step, +2.6
frames 300-479 : mean 80.74 .. 81.00      flat again
frames 480-599 : mean 81.05 .. 81.21      small second step, then flat
```

**The picture is not ratcheting.** It holds to within 0.03 luma for minutes and
then changes in discrete jumps. That is something adjusting once per scene
change - which is what auto-exposure does - and it is the behaviour the
operator described as colours shifting when a hand passes over the camera.
Nothing in the driver is known to do this, and the camera is the obvious
candidate, but that has not been proven and would be settled by repeating the
hand-wave on Windows.

The scanner reported "drifting at +7.091 luma per 1000 frames" for that trace,
which is a line fitted to a step function - a confident drift rate for
something that is not drifting. It now measures the largest window-to-window
step against the total spread and says STEPPED or continuous accordingly,
self-tested against both shapes.

### M234 is retracted

Same capture, range check:

| | M234's capture | this capture |
|---|---|---|
| luma above 235 | 52830 of 1248000 (**4.233%**) | 15 of 1248000 (**0.001%**) |
| luma max | 254 | 237 |

**The 4.2% is not reproducible.** It was measured while the picture was still
in its drifted-bright state, so the superwhite that the full-range conclusion
rested on was an artifact of the very brightness problem being investigated.
`below 16` is 0.096% and confined to the first bucket, i.e. a startup frame.

`raw_full_range` therefore defaults to 0 again - limited range, as before M234.
The parameter stays, because the question is unproven rather than settled. What
would settle it is a capture with something genuinely black in frame: a
full-range source reads near 0 there, a limited one near 16. No capture so far
has contained real black, which is the same gap the original M234 commit
flagged and then shipped a default on anyway.

**The lesson, for the next person and for me.** M234 was measured during a
known-unstable condition and shipped on one statistic whose weakness was
recorded in its own commit message. The frame scanner was right both times; the
error was drawing a conclusion from a capture taken while a separate defect was
active.

## M238 (2026-08-31): M229 only checked luma, so chroma could still be cleared

Operator captured an occasional frame with a magenta/green cast over the
picture while the luminance detail was intact - the board and the noisy
background both correctly resolved, only the colour wrong. Two neighbouring
frames from the same session were normal.

Correct luma with wrong chroma is a plane-level fault, and it points straight
at M229. I420 is planar - Y, then U, then V - and the card fills a slot
ascending after clearing it. `mz0380_raw_probe_frame_filled` samples only the
final 64 rows of the LUMA plane, so it answers "has the picture arrived" and
says nothing about the two chroma planes that follow it in memory. A slot
whose luma has landed but whose chroma is still the clear value passes the
test and is delivered with correct detail and the colour of the clear.

That is a defect in the M229 fix, not in the code it replaced: the old
sentinel at `frame - 4` was in the V plane and did cover chroma, and choosing
the luma tail traded that coverage away without noticing.

**Fix.** `mz0380_raw_probe_chroma_filled` applies the same test to the final
sixth of the frame against the chroma clear value, `raw_clear_chroma` (0x80,
measured in M226). A slot is delivered only when both planes are past their
clear.

The chroma test has a false-negative case the luma one does not: neutral
chroma in real content is also 0x80, so a frame whose bottom is genuinely
colourless is held back for one completion. That costs one frame and cannot
persist, and it is the right direction - a deferred frame is invisible, a
wrong-coloured one is not.

`raw fills` in `/proc/mz0380-state` now reports the two rejections separately.
`raw_incomplete_chroma` is the falsifier: if tinted frames persist while it
stays at 0, chroma completeness is not the cause and the next suspect is the
CSC path (M237), which would tint every frame in a session rather than
occasional ones.

Untested on hardware.

## M237 REFUTED (2026-08-31): the source colour space does not change with the mode

With the `csc` command forcing a fresh BANK2 0x48 read either side of a
50/60 Hz change:

```
colourspace: source YUV444 (0x48=d2 cached), CSC 0x92=00 (bypass)
colourspace: source YUV444 (0x48=d2 cached), CSC 0x92=00 (bypass)
```

Identical, and the operator reports no meaningful brightness difference at
60 Hz on this attempt. The stale-cache asymmetry in `apply_csc_mode` is real -
the CSC is chosen once at stream start and `mst_b2_48_valid` is never cleared -
but it is not what changes the brightness, because the value it caches does not
change.

The staleness remains a latent bug worth fixing if a source ever does switch
colour space, and the `csc` command remains the manual escape. Neither is
urgent.

What this leaves for the 50/60 Hz brightness: `mst3367_commit_digital_output`,
which writes different BANK0/AB values per mode and is the other thing a
timing change runs. Not investigated. Note also that the operator's most recent
attempt did not reproduce the symptom strongly, so before spending more runs on
it, confirm it still reproduces at all.

## M239 (2026-09-01): the driver was painting its own poison into every delivered frame

Operator report: isolated red dots in the picture, one near the middle.

`mz0380_raw_deliver_slot` armed the tear detector and then copied:

```c
/* Arm the tear detector, then copy. */
mz0380_raw_probe_sentinel_repoison(dev, idx);
dma_rmb();
memcpy(dst, raw->va, frame);
```

The poison goes into the SOURCE buffer, and the memcpy then copies the whole
slot - poison included - into the vb2 plane. Every delivered raw frame carried
four dwords of 0xa5 or 0x5a at fixed offsets. Measured: **600 frames of 600,
all four offsets**, no exceptions.

Three of the four sentinels sit in the chroma planes, so each paints a small
coloured blob at a deterministic pixel. For 1920x1080:

| offset | plane | luma pixel |
|---|---|---|
| `frame-4` | V | (1912, 1078) |
| `frame-frame/16` | V | **(960, 674)** |
| `frame-frame/4` | U | (0, 540) |
| `frame/2` | Y | (0, 810) |

(960,674) is horizontally centred, which is what "one close to middle" was.
Predicting the pixel before looking is what made this findable in one capture
rather than by argument.

This has been true since the raw path shipped. Every luma statistic in
M226/M229/M234/M235 included these sixteen corrupted bytes - negligible against
3110400, but real, and worth knowing when re-reading those numbers.

**Fix.** The detector genuinely needs the poison present in the source while
the copy runs, so it is not reordered. `mz0380_raw_probe_sentinel_save` records
the four real dwords before arming, and `mz0380_raw_probe_sentinel_restore`
writes them back into the DESTINATION once the copy is known not to be torn.
Tear detection is unchanged; only the delivered pixels are repaired.

`scripts/mz0380-m226-blackframe-scan.py` checks all four offsets in every frame
and is the falsifier: after this, no delivered frame should carry poison. If
dots remain with that count at zero, they are not ours.

**Status: CONFIRMED on hardware, 2026-09-01.** The same 600-frame capture that
read 600 of 600 before the fix reads **0 of 600** after it. Whether the visible
red dots are gone is the operator's call - the counter proves only that the
driver is no longer the source of poison at those offsets.

## M240/M241 CONFIRMED (2026-09-01): the node presents as a camera

```
[0]: 'YU12' (Planar YUV 4:2:0)
[1]: 'NV12' (Y/UV 4:2:0)
[2]: 'YV12' (Planar YVU 4:2:0)
[3]: 'H264' (H.264, compressed)
```

Raw at index 0, H.264 last, node named `HD60 Pro HDMI capture`. NV12 negotiated
and **rendered correctly** - operator confirms it matches YU12, so the
interleave and the byte-wise sentinel restore that had to follow it are both
right.

This matters more than it looks. Most camera consumers take the first
enumerated format or cannot use H.264 at all, and with H.264 first the node
read as a capture-card oddity. It is also how two hardware runs were spent
testing the encoded path by accident while trying to reproduce a raw defect in
OBS - the format order was silently choosing the wrong path for the
experiment.

The three raw layouts cost nothing beyond the copy that already happened: the
card writes I420, YV12 exchanges the chroma planes and NV12 interleaves them.
`YUYV` and `XRGB`, which Windows also offers (M227), would need real pixel
conversion and are not implemented.

## M242 (2026-09-01): the duplicate frames are the SOURCE's frame rate, not a driver fault

`raw_dup_content` had been reading 20% and latterly 45%, and was carried as the
largest remaining defect in the raw path. It is not a defect.

Whole-frame fingerprints at two sampling rates:

| sampled at | distinct | adjacent duplicates | implied unique rate |
|---|---|---|---|
| 60 fps | 345 / 600 | 42.6% | 34.5 fps |
| 34 fps | 288 / 340 | 15.3% | 28.8 fps |

Both point at roughly **30 fps of real content**. In both captures the
duplicates are almost entirely ADJACENT PAIRS - run length 2.00-2.02, spacing 1
- which is what a source sending each frame twice on a 60 Hz link produces.

The two numbers differ (34.5 against 28.8) in the direction the method
predicts: the fingerprint is an exact byte match, so sensor noise makes some
genuine repeats compare as different. The duplicate count is therefore a lower
bound and the distinct count an upper one, and the error is largest at the
higher sampling rate where more true duplicates exist to be missed. Sampling a
30 fps source at 34 fps predicts about 12% duplicates; 15.3% was measured.

**The driver is delivering exactly what the card gives it.** The operator's
"image not refreshing when a new object enters view" is the camera's own frame
rate showing through, not a delivery fault.

This also retires the M209-era puzzle recorded in the handoff - "209 raw
completions at 16.3 ms apart (~61/s) against a receiver reading of 30 fps and
60 encoded frames". The receiver was right: the CONTENT is 30 fps while the
SIGNAL is 60 Hz. Nothing was ever dropping half the frames.

`raw_dup_content` should be read as a source-rate indicator rather than an
error counter. Its head-only comparison also makes it noisier than the
whole-frame fingerprint, so prefer
`scripts/mz0380-m226-blackframe-scan.py --fps <rate>` when the number matters.


## M243 (2026-09-02): the brightness steps are the CAMERA, measured through a second capture card

Three driver-side theories for the stepping brightness have been retracted
(M234 full-range labelling, M237 CSC staleness, and the AUTO_POSITION latch of
M233 as a complete explanation), each for the same reason: a number measured
on the only path available, with no independent path to check it against.

The operator moved the HDMI cable to a UGREEN 25173 USB capture card and ran
the same scan, on the same camera, over the same kind of scene. Neither
capture device knows anything about the other.

| path | first -> last quarter | largest step | median step | verdict |
|---|---|---|---|---|
| USB (UGREEN 25173) | 53.39 -> 104.80 (+51.41) | 43.02 | 0.07 | STEPPED |
| PCIe (this driver) | 54.18 -> 111.30 (+57.13) | 52.05 | 0.07 | STEPPED |

Same shape, same magnitude, same timing: flat at about 52 luma for the first
80 frames, one jump, then a plateau that holds to the end. A defect in this
driver cannot reproduce itself inside an unrelated USB capture card, so the
brightness stepping is the camera's own auto exposure. **Closed.**

The remaining 110 vs 105 plateau gap is not exposure. It is range handling:
the PCIe payload reaches 254 while the USB path arrives through ffmpeg's
yuvj420p -> yuv420p conversion, which clamps to 16..235 by construction. That
also disqualifies the range check as A/B evidence - the USB leg reports
"limited range" no matter what the camera sends, so do NOT read the two
RANGE VERDICTs as a difference between the cards. M234 stays retracted.

### The first USB leg was 720p read as 1080p

Worth recording because every verdict it printed looked plausible. ffmpeg was
given no geometry, took the device's default 1280x720, and the scan sliced the
stream as 1920x1080. 360 source frames x 1382400 bytes / 3110400 = exactly
160, which is what it reported reading - and it invented three "partial fills"
whose band profile repeats the same picture twice per frame, because that is
what one resolution read as another looks like. `scripts/mz0380-m243-brightness-ab.sh`
now pins `-video_size 1920x1080` and scales as a backstop.

The lesson generalises: the scan trusts its caller for geometry, so any capture
into it must state the geometry rather than accept a device default.

## M244 (2026-09-02): the ABI grew two formats and the enumeration did not follow

`v4l2-compliance` was 148/148 at M172 and had not been re-run since M240/M241
added NV12 and YV12 to `ENUM_FMT`. It was **142/6**:

```
5 fail: v4l2-test-formats.cpp(1967): node->can_scale && node->frmsizes_count[...]
1 fail: v4l2-test-io-config.cpp(239): doioctl(node, VIDIOC_S_DV_TIMINGS, &timings)
```

`mz0380_enum_framesizes` and `mz0380_enum_frameintervals` each carried their
own inline list of acceptable fourccs - `V4L2_PIX_FMT_YUV420 || H264` - written
before there were three raw layouts. So the node advertised NV12 and YV12
through `ENUM_FMT` and then answered `-EINVAL` when asked for their frame
sizes. v4l2-compliance reports a format with no enumerable sizes as a scaling
failure, once per input, which is why one defect appeared five times.

Both now call `mz0380_is_raw_fourcc`, the predicate the rest of the raw path
already used, so the lists cannot drift apart again. Confirmed directly rather
than by the suite alone - all four formats enumerate 1920x1080:

```
YU12: Size: Discrete 1920x1080     NV12: Size: Discrete 1920x1080
YV12: Size: Discrete 1920x1080     H264: Size: Discrete 1920x1080
```

**148 tests, 148 succeeded, 0 failed.**

The `S_DV_TIMINGS` failure disappeared in the same re-run and is NOT claimed as
fixed - nothing in this change touches DV timings. The failing run had a
foreign consumer holding the node (below); the clean one did not. Treat it as
unexplained and watch for it.

### Something else on the desktop opens the node the moment it appears

One run of the M238 test failed its first phase with `VIDIOC_REQBUFS` returning
`EBUSY` and `pixelformat` stuck at `H264`, while the counter watch recorded
**2591 H.264 frames delivered** to a consumer this project never started.
WirePlumber probes every new V4L2 node on udev add and the portals sit behind
it.

This is not cosmetic:

- it spends encoder spawns from a budget of 8-18 per power cycle - one such
  run took the tally from 1 to 3 without capturing anything;
- it makes a capture fail for a reason that reads as a driver defect;
- **two consumers contending is a live candidate for M232.** "Pipeline running,
  VB2 detached, 20 attachments" is what contention looks like from inside the
  driver, and M232 has never been reproduced deliberately.

`scripts/mz0380-node-holders.sh` names the holders, and both hardware scripts
call it before every capture phase. It needs root to see other users' file
descriptors and says so rather than reporting a clean node it cannot verify.

### M238 is reachable, and a rejection counter of 0 no longer means nothing

M238's chroma-completeness check had never fired, and its counter reading 0
could not distinguish "wired and never needed" from "never evaluated". Two
counters now answer that: every evaluation is counted, and the high-water mark
of matching samples is kept.

| capture | probes | rejections | best match |
|---|---|---|---|
| 120 frames, black display | 217 | 0 | 14/32 |
| 120 frames, normal content | 1140 | 0 | 16/32 |
| 15264 frames, mixed scene | 15349 | 60 | 32/32 |
| 24457 frames, mixed scene | 646405 | 356 | 32/32 |
| 33447 frames, lit scene | 660323 | 420 | 32/32 |

The check is live and it does fire, at roughly 0.4% to 1.5% of delivered
frames. The evidence that these are genuine mid-fill catches rather than the
documented false negative:

- real content never approached the threshold. A monitor showing black reached
  14 of 32 samples and normal content 16 of 32, because sensor noise of plus or
  minus one breaks the exact-equality test. Every rejection is an exact 32/32,
  which is what a DMA clear looks like and what content does not;
- delivery did not stall during the dark stretches of the long runs. If neutral
  content were tripping the test, rejection would approach total while the
  picture was colourless; instead frames kept flowing at `mean_y` around 52.

Not yet closed: the counters are cumulative, so they cannot date a rejection
against the scene that produced it, and every long run so far contained some
colourless stretch. `scripts/mz0380-m243-brightness-ab.sh` in `pcie` mode now
runs the counter watch alongside the capture and prints the seconds at which
`rawnochroma` moved, which reads directly against the scan's per-window luma.

## M245 (2026-09-26): a whole-driver review, and the fixes it produced

A read of the full lifecycle - probe/remove, V4L2/vb2, IRQ/mailbox, DMA and
IOMMU, delivery, signal recovery, procfs, controls, ALSA. clang W=1 with
`-Wextra` was already clean and checkpatch found only style, so everything
below came from reading the code. Built clean after the changes; **none of it
is hardware-verified yet** - `scripts/mz0380-m245-verify.sh` is the test.

### Defects fixed

1. **TRY_FMT wrote into the live device.** It set `deliver_raw`, `raw_fourcc`
   and `capture` to the request, called the helpers, and restored them. The
   drain reads those fields unlocked while it copies, so a TRY_FMT from any
   process during a capture could copy one frame in the wrong chroma layout -
   correct luma, green/magenta cast - or send it down the H.264 branch. That is
   the M238 symptom class. The window is microseconds, so it is a candidate,
   not a demonstrated cause. The format helpers are now pure functions of their
   arguments (`mz0380_sizeimage_for`, `mz0380_fill_pix_format_for`).
2. **S_FMT was only busy-checked for the raw/H.264 switch.** A second process
   could change the raw layout (deliberately allowed mid-stream by M241) or
   the geometry under another application's buffers. Now any change returns
   `-EBUSY` while buffers exist, which is what V4L2 specifies.
3. **Use-after-free on unbind with a handle open.** `vdev.release` was
   `video_device_release_empty` and remove did `kfree(dev)`. The struct is now
   freed from the `v4l2_device` release callback; remove unregisters the node
   first with `vb2_video_unregister_device()`, which stops a stream while the
   hardware is still mapped.
4. **`card[]` read out of bounds** after 8 unbind/bind cycles: the card counter
   was never decremented. Now an IDA.
5. **Unprivileged hardware access through /proc.** `mz0380-periph-scan` (up to
   256 mailbox I2C commands per read), `mz0380-scan` (raw BAR reads) and
   `mz0380-hdmi` (receiver diag) were world-readable. Now root-only;
   `mz0380-state` stays 0444 for the watch script, with its one mailbox read
   (verbosity 3) gated on `CAP_SYS_ADMIN`.
6. **Controls that lied.** Bitrate/quality/GOP/B-frames/record-mode wrote
   unverified BAR5 registers and SET_ENC_PARAMS then ignored them; brightness,
   contrast, hue, saturation, sharpness, profile, level and peak bitrate were
   never read. Now: bitrate and GOP only, sent in SET_ENC_PARAMS for the main
   stream, defaults = the Windows values (4194304 / 32) so default behaviour is
   byte-identical. Grabbed while streaming; a change between sessions schedules
   the persistent pipeline's replacement at the next attachment (one spawn).
   Plus `V4L2_CID_DV_RX_POWER_PRESENT`, read-only, reporting receiver LOCK -
   no +5V/cable bit is known on the MST3367, so a powered but unlocked source
   reads 0.
7. **Five inputs** on a one-connector card. Now HDMI only.
8. **No `.shutdown`.** A persistent pipeline (stop_on_streamoff=0) survived
   into a warm reboot or kexec. `mz0380_dma_quiesce()` sends STOP and bus
   mastering is cleared. Suspend does the same after failing the vb2 queue;
   resume redoes the handshake off the resume path (`resume_work`) and the
   receiver comes back lazily. **Suspend/resume is untested.**
9. **18 module parameters** that decide allocation, DMA addresses, pipeline
   lifetime or the advertised format were 0644. Now 0444.
10. **IOMMU:** buffers are `iommu_map`ped into the DMA-API domain behind its
    allocator. Mitigated, not redesigned: with `dma_iova_remap` the DMA mask is
    32-bit, so the allocator stays below 4 GiB, and a `dma_iova_base` below
    4 GiB is refused. The full fix - a driver-owned domain - needs the card
    alone in its IOMMU group, which is board-dependent.
11. **EDID burn removed** - it wrote card EEPROM by bit-bang from a /proc write,
    the same risk class as the deleted firmware upload.
12. QUERY_DV_TIMINGS now returns `-ENOLINK` for no signal and `-ENOLCK` for
    unstable (it returned `-ENOLCK` for both); a detection no longer overwrites
    timings an application set with S_DV_TIMINGS; SD modes report BT.601.

### Cleanup

One mutex for vb2 and the other ioctls; drain on its own `WQ_HIGHPRI`
workqueue instead of `system_wq`; 45 internal `EXPORT_SYMBOL_GPL`s removed;
`printk(KERN_*)` -> `pr_*`; the per-command and per-frame info lines that no
script parses moved to dynamic debug; notify pointer uses `pci_bus_address()`
and says why the handshake fails if BAR0 is above 4 GiB; dead pre-4.0 procfs
branch removed; `MODULE_AUTHOR` corrected; board tables `const`.

### Deliberately not done

- **HDMI audio.** The card's PCM transport is not reverse engineered. The ALSA
  stub used to accept an open and never deliver a period, hanging clients; it
  now refuses the open with a message.
- **Operating without an IOMMU.** Not a software gap: the card latches only the
  high 32 bits of a DMA target, so buffers must sit at 4 GiB-aligned bus
  addresses, which needs translation. The error now says so.
- **debugfs migration** of the /proc files - every hardware script and the
  unprivileged watcher use the /proc paths, and debugfs is root-only.
- **Condensing the M-number comments** - they are the index into this file.

### M245 hardware results (2026-09-26)

- **v4l2-compliance: 48 tests, 48 succeeded, 0 failed, 0 warnings.** The drop
  from 148 is the input list: compliance repeats about 25 tests for every input
  the node lists, and going from 5 inputs to 1 removes 4 x 25 = 100. The five
  POWER_PRESENT warnings are gone.
- **Unbind with a handle open: PASS.** The node disappears while the handle
  stays open, closing it afterwards reports no fault, and rebind comes back as
  `mz0380[0]` - the IDA reused the number. No KASAN on this kernel, so this
  shows the old crash path is not taken, not that no stale access remains.
- **Controls: PASS.** GOP 1..255 default 32, bitrate 262144..12582912 default
  4194304, power_present read-only, nothing else; one input.
- **Capture with a second process: PASS.** S_FMT(NV12) from another process
  during a YU12 capture was refused as busy; 400 TRY_FMTs (NV12 and H264) left
  the owner's format at YU12; 180 whole frames, no partial fills, no sentinel
  poison. One spawn.
- **Defect in the M245 change itself:** power_present read 0 with a locked
  source. It reported the cached `signal_locked`, which nothing had refreshed
  since the rebind. It now reads R55 live when no stream or pipeline is
  running, and `m245-verify.sh ctrls` checks it against ENUM_INPUT's status.
  **Re-run on a fresh load: power_present = 0x00000001 with the source
  locked, agreeing with the input status. PASS.** Spawn tally 2.

Still untested: suspend/resume and `.shutdown`.

## M246 (2026-09-26): HDMI audio is window 3 - op 0x03, four 4 KiB slots, EVENT[19:16] + token 0x4c

The data path the Windows traces could not show (collect-2026-08-19,
WHAT-LINUX-STILL-NEEDS item 3), read from the two binaries instead.

**Windows registers it.** e60MZ0380 v195 at `0x14027b05f` builds, for channel
0..7 in a loop:

    {0x800, 0x03, ch, 0x1000, {hi, lo} x 4}      slots = [dev+0xc8] + k * 0x1000

- op `0x03`, a **4 KiB** slot size, four consecutive slots per channel cut from
one common buffer. Every video registration is megabytes; this one is audio.
4096 bytes is exactly 256 frames x 4 periods x 2 ch x 16 bit, which is both what
our SET_AIC already asks for and the card's own `capture_app_infinite` default
(`-F 256 -B 4`). ep.ko maps op 0x03 to `channels[ch] + 0x88` - window 3 (M23's
opcode table). This driver had never sent it; M32 sent it once, pointed at a
video buffer, in a session where the pipeline produced nothing at all, so that
said nothing about audio.

**The card reports it.** ep.ko `store_channel_done` (0xdc8) takes a descriptor
whose word 5 selects the audio branch:

    audio:  bar0[0x4c] nibble[ch] = desc[4] - 1          (slot 0..3)
    EVENT:  vic_int_mode != 0            -> BIT(16 + ch)
            vic_int_mode == 0, aic_int_mode == 1 -> BIT(15 + desc[4])  = bits 16..19

We send vic_int_mode 0 and aic_int_mode 1 (what Windows sends), so each filled
slot should raise its own bit in 16..19. `reg.h` already labelled 0x4c "payload
for EVENT[23:16] events". The ISR ACKed those bits and nothing ever read them.

**Implemented behind `enable_audio=1` (load time, default off):** four slots at
their own 4 GiB-aligned IOVAs after every video window (index 16..19), 64 KiB
mapped and poisoned per slot; op 0x03 sent wherever the encoded window is
(re)registered; EVENT[23:16] + 0x4c snapshotted pre-ACK like a video frame;
each slot's written extent measured against the poison, at most 4 KiB handed to
a real ALSA capture device (S16_LE, the SET_AIC channel count and rate), then
re-poisoned. `/proc/mz0380-state` prints an `audio :` line with every counter.

**Predictions, before any hardware run** (`scripts/mz0380-m246-audio-test.sh`,
one spawn): audio events > 0 at about 48000 / 1024 = 47 per second, max extent
4096, no overrun, no empty slots, and a WAV whose content follows the source.
Events at 0 would mean the window is not the whole story; empty slots with
events would mean the target is right in principle but offset. Unverified.

### M246 first run: zero audio events - and why (SET_AIC bytes 4 and 5 were wrong)

    audio : 0 events, 0 slots, 0 bytes ... ; IOMMU faults: none
    raw frames: 600 delivered; arecord: read error: Input/output error

The window and the event plumbing were not reached at all, because the card
never produced audio. Its `audio_capture_mgr` (on-card, ~1.9 KB of code,
disassembled into `re-dump/acm.ann.txt`) polls `/sys/vpl_pciep/audio_ctrl`,
which ep.ko notifies for op 0x06 AND for op 0x2a (the SET_AIC handler at 0x17cc
notifies epint and then `audio_ctrl` - M125 had only seen the first). On a
SET_AIC it launches

    ./capture_app_infinite -D -P 5 -d 0 -R <freq> -F <frames> -B <periods>

only when it was started with `-P 5` (rc.local does), **cmd+4 == 0**, cmd+5 / 2
>= 1 (it compares cmd+5 against 8 for the 8-channel app) and on == 1. So cmd+4
is an audio index and **cmd+5 is the channel count**. video_capture_mgr's printf
calls them "channel_num" and "mono", which is where M33's layout came from; the
Windows driver agrees with the audio manager, not the label - its dword at
0x14028c10e is `index | chs << 8 | 16 << 16`. We sent cmd+4 = 2, cmd+5 = 0, so the
PCM capture program has never run on this card.

Fixed for the audio build only: with the audio window allocated, SET_AIC word 0
is `0 | chs << 8 | bits << 16`. A video-only load keeps the legacy bytes it was
validated with - the launched app is a daemon, and each SET_AIC starts another
one on the card, which is exactly the kind of change the wedge budget argues
against making silently. Second run pending.

### M246 second run: THE CARD DELIVERS HDMI AUDIO

    SET_AIC(on=1, 2 ch, 16 bit, 48000 Hz, 256 frames x 4 periods,
            audio (index 0, chs in cmd+5) layout word0=00100200) ret=0
    audio : 584 events, 1168 slots, 2392064 bytes, 584 empty, 0 overrun,
            0 bad token, max extent 4096, slots seen 0xf,
            last event 00080000 token a5a5a5a3
    raw frames: 600 delivered; IOMMU faults: none

584 x 4096 bytes = 12.46 s of 48 kHz stereo 16-bit, against a ~12 s video run:
about 47 completions a second, one full 4 KiB slot each, all four slots in use.
The last event, bit 19, is slot 3, and so is the token's nibble - both
completion signals agree with the ep.ko decode. Video was unaffected by the
corrected SET_AIC. (The run before this one never reached the card: WirePlumber
held the new ALSA card's control device, which pins the module, so the reload
failed. `mz0380-live.sh unload` now unbinds first.)

Two defects of mine, both visible in those numbers:

- 1168 slots for 584 events, one empty per event: the consume loop re-read
  `audio_last_slot` while consume_slot() was moving it, walked round and took
  the slot it had just re-poisoned. Fixed (start slot taken once).
- The capture got nothing (`arecord: read error: Input/output error`, an
  empty WAV) although 2.4 MB was consumed during its window. Not understood
  from the code; the push now counts bytes delivered, periods, refusals and
  the reason for them, and the arrival window. Third run pending.

### M246 third run, and two zero-spawn checks: transport PROVEN, content silent

Third run: `589 events, 589 slots, 2412544 bytes, 0 empty` - the double
consume is gone. ALSA still `0 bytes delivered, 589 refused (why 0x6)`: every
push found the capture closed or not started. Not a driver fault: a fresh load
spends ~20 s before SET_AIC (spawn, settle), the card starts its capture app only
then, and arecord - started on a fixed 2 s delay - hit ALSA's 10 s no-period
timeout and exited before the first slot existed. The test now waits for the
first audio event before recording.

With that run's module still loaded, its persistent pipeline kept the audio
flowing (338 s, ~47 slots/s, no video attached), so the ALSA side could be tested
without another spawn:

| check | result |
|---|---|
| arecord alone, 3 s | 577536 bytes delivered, 24 periods - exactly 3 s of 48 kHz stereo |
| arecord while re-attaching video (persistent pipeline, 0 spawns) | 770048 bytes delivered - 4 s - video at 50 fps throughout |
| content of both WAVs | **100% zero samples**, no poison |

So the path is proven end to end: SET_AIC launches the card's capture app,
op 0x03's window receives 4 KiB periods at the right rate, EVENT[19:16] + 0x4c
report them, and ALSA delivers them. What arrives is digital silence. Not yet
separated:

- the source sends no HDMI audio (cameras often embed none, or only when a
  setting is on);
- **several capture apps are running on the card.** Every SET_AIC starts
  another `capture_app_infinite` daemon and STOP does not end it -
  audio_capture_mgr answers op 0x07 with "unknow command" - so runs 2-4 left
  three competing for I2S 0. A mains-off cold boot clears them;
- the receiver's audio path. Our init already carries hdcapm's RxAudioInit
  register for register, so this is the least likely.

Next: cold boot, a source known to embed audio (a PC or console HDMI output),
one run of `scripts/mz0380-m246-audio-test.sh`.

### M246 silence explained: the source has no audio

The operator confirms the HDMI source embeds no audio, so all-zero PCM is the
correct output, not a defect - the card's capture app clocks I2S silence at the
requested rate, which is exactly what arrived. What remains unverified is only
the non-zero case: that a source WITH audio produces the expected samples
(channel order, endianness). Test with a PC or console output when one is
available. The competing-daemons caveat stands for any future audio run: cold
boot first, because each audio-enabled load leaves another capture app running.
