# Windows: what to collect to make the Linux driver work

_Written 2026-08-19. Self-contained — you should not need the Linux machine or
the chat history to follow this._

Only tasks whose output feeds the Linux driver are listed. Each says what it
unblocks. Nothing here modifies the card.

---

## Prerequisite: mains-off cold boot

Shut down, switch the PSU off **at the mains**, wait ~10 s, power up.

Slot standby power keeps the card's SoC alive through a normal soft power-off,
so a plain reboot does not reset it. The card is currently in a state where its
kernel side answers but its userspace video process does not, and only a
mains-off cycle is known to clear that. Booting Windows also re-provisions the
card's firmware from Elgato's own image, so this doubles as recovery.

---

## 1. EDID: what does a working card present?  ← biggest single gap

Our driver has **never successfully delivered an EDID**. The receiver's EDID
shadow (MST3367 BANK3) reads all zeros, no EEPROM exists on the card's I2C bus,
and five candidate mailbox opcodes were tried and rejected. Sources still
transmit, but each picks its own fallback format — which may be the whole
reason nothing downstream works.

EDID is readable from the **source** side: plug a second computer into the
capture card's HDMI **input**, and to that computer the card is just a display.

Do it while Windows is driving the card **and capture is working**:

*Source computer running Windows* — NirSoft **DumpEDID**, or:

    Get-WmiObject -Namespace root\wmi -Class WmiMonitorDescriptorMethods |
        ForEach-Object { $_.WmiGetMonitorRawEEdidV1Block(0).BlockContent } |
        ForEach-Object { "{0:X2}" -f $_ } | Out-String

*Source computer running Linux:*

    cp /sys/class/drm/card*/card*-HDMI-A-*/edid ~/card-edid.bin
    edid-decode < ~/card-edid.bin

Save the **raw binary**, not just the decoded text, and note which port on the
source is the card.

**Unblocks:** the bytes our driver must present. Right now we are guessing at
both the content and the delivery mechanism; this gives us the content exactly.

> Also worth repeating the same dump on Linux with our driver loaded. If the
> source sees nothing, or something different, that delta is the bug. Needs no
> Windows.

## 2. HDCP: is it blocking capture, or normal?

On Linux every source reported the link as HDMI with HDCP active/encrypted
(MST3367 BANK1 `0x01 = 0x8d`, `0x34 = 0x90`) — three different sources,
byte-identical. We could not tell whether that is why capture produces nothing,
or whether it is normal operation Windows also sees.

Test the **same sources** and record what Elgato's software shows, verbatim:

| source | picture? | error text |
|---|---|---|
| digital microscope / camera | | |
| phone via USB-C→HDMI dongle | | |
| the source that read htotal=2750 | | |

**Unblocks:** if Windows shows "HDCP protected content" and no picture, HDCP is
real, the Linux driver is not at fault for those sources, and we need a
non-HDCP source to finish. If Windows shows a picture, HDCP is a red herring
and the fault is in the receiver→VIC handoff. Either result removes a live
hypothesis we are otherwise paying for.

## 3. The driver binary

    C:\Windows\System32\drivers\e60MZ0380.X64.SYS

(name may differ — search `C:\Windows\System32\drivers\` for `*MZ0380*`, or
Device Manager → the capture device → Driver → Driver Details)

**Unblocks two things.** We only ever had a disassembly of its `.text`; the
data sections were never captured.

- The driver bulk-writes **18 bytes to I2C device 0x98** at sub-address 0x76,
  plus one byte at 0x73 (payloads at `0x14033e938` / `0x14033e968` /
  `0x140340b5c`). 0x98 is one of only two devices on the card's I2C bus and we
  have **never written it**. Those bytes are in this file.
- If the card has no EDID EEPROM — and we found none — the EDID has to come
  from the host, so **the EDID block is probably in this binary too**. It is
  trivially findable: every EDID starts `00 FF FF FF FF FF FF 00`. Nothing in
  the card's own firmware image matched that signature.

Copy the file before any driver reinstall or cleanup.

## 4. Live register dumps while capture works

Everything our driver does goes through a mailbox in BAR0, and we have only
ever seen it in the **failing** state — `EVENT` at `+0x30`, frame token at
`+0x40`, encoder status at `+0x50` read zero on every run.

Get **RW-Everything** (rweverything.com, free). Run as Administrator.
**Read only — do not write.**

1. `Access → PCI`, find the card (vendor `12AB`, device `0380`; bus/dev/fn from
   Device Manager → Details → Location information).
2. **Dump the full PCI config space** (0x00–0xFF, and extended to 0x1000 if
   offered). Save it.
3. `Access → Memory` at the BAR0 base (config offset `0x10`), dump **0x00–0x80**
   in each state:

   | state |
   |---|
   | a. no HDMI cable |
   | b. cable in, source off |
   | c. source on, signal present, not capturing |
   | d. **Elgato app actively capturing** ← the one that matters |
   | e. capture stopped |

4. Same for BAR5 (config offset `0x24`), **0x00–0x100**, if you have patience.

**Unblocks:** state (d) shows what a completed frame looks like at the host
interface — never once observed. The config space confirms BAR sizes, whether
**MSI** is enabled and with how many vectors (our driver assumes MSI and has
never verified it against Windows), and the link width/speed. It also carries
the **subsystem vendor/device ID at offset 0x2C** — our driver has board-
specific branches keyed off that (the Elgato branch, board id 0x1c/0xfa), taken
from reverse engineering and never confirmed against the real device.

If you do only one dump, make it state (d) of BAR0.

## 5. Hot-plug sequence trace

We know the mechanism on paper — MST3367 receiver, an HPD GPIO, an EDID the
sink should present — but not the *sequence* a working driver runs, or what it
does the moment a cable is plugged.

Enable kernel `DbgPrint` output (filtered off by default):

    HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter
    DEFAULT   (REG_DWORD)  = 0xFFFFFFFF

then reboot. Get **DebugView** (Sysinternals), run as Administrator, and under
Capture tick **Capture Kernel**, **Enable Verbose Kernel Output**, **Capture
Events**.

With it capturing, do these ~10 s apart and note the wall-clock time of each:

1. start with no HDMI cable
2. plug the cable in, source **powered off**
3. power the source **on**
4. wait for a picture in the Elgato app
5. start a capture, run 30 s
6. stop the capture
7. power the source off, cable still in
8. unplug the cable

Save the whole log plus your timings. Tags known to exist in this driver:
`[MEMORY]`, `[HARDWARE]`, `[TECHPOINT]`, `[MCU]`, `[CPLD]`,
`[KSEVENT_DEVICE_LOST]`.

**Before you start, check for a verbosity knob.** Many vendor drivers gate
their own logging behind a registry value, and turning it up costs nothing:

    HKLM\SYSTEM\CurrentControlSet\Services\<driver service name>\
    HKLM\SYSTEM\CurrentControlSet\Services\<driver service name>\Parameters

Look for anything like `DebugLevel`, `DebugMask`, `TraceLevel`, `LogLevel`,
`Verbose`. If one exists, set it high (`0xFFFFFFFF` or the largest plausible
value), reboot, and redo the trace. The service name is on the Device Manager
→ Driver → Driver Details page. A more verbose log here is worth more than
most of the rest of this document.

**Unblocks:** each transition should produce driver activity — HPD assertion,
EDID delivery, receiver re-init, mode detect, encoder arm. Our driver
reproduces some of this and we do not know which parts are missing or
mis-ordered. Step 2 of the list (cable in, no signal) is where EDID would be
delivered.

Also note from the app: how long after step 3 the picture appears, any
resolution/format readout verbatim, and what unplugging does.

## 6. Firmware version and image

Record the firmware version the card reports under Windows (DebugView will
likely print it), and copy Elgato's firmware files — search the install
directory and driver store for `MZ0380*` (expect `MZ0380.HD.HEX`,
`MZ0380.FW.TXT`, `MZ0380.BASE.TXT`):

    C:\Program Files\Elgato\...
    C:\Windows\System32\DriverStore\FileRepository\<...mz0380 or e60...>\

**Unblocks:** ours is `01.11` from 2020. If Elgato ships newer, our driver will
see the mismatch and **silently re-upload 01.11 over it on every insmod**,
downgrading the card each boot. If the versions differ we should ship Elgato's
image, and the card-side binaries we reverse-engineered may be stale.

## 7. HDMI passthrough output

Does the board have an HDMI **output** as well as an input? If so:

- is anything connected to it in normal use?
- does capture behave differently with a display connected versus nothing?

**Unblocks:** cards with passthrough very often **clone the downstream
display's EDID** upstream. If this one does, then with nothing plugged into the
output it may legitimately present a null EDID — which would mean the all-zero
BANK3 we have been treating as a bug in our delivery code was never our bug.
Also testable on Linux in minutes.

## 8. Software reset for a wedged card  _(optional, saves time later)_

After roughly 8–18 encoder starts the card stops answering, and **only a
mains-off power cycle** brings it back — not a warm reboot, not rmmod/insmod,
not re-uploading firmware. This cost real time during development.

If you hit the wedge on Windows, note whether Windows recovers it **without**
mains-off, and how (driver reload? warm reboot? something in the app?). Also
note whether Elgato ships a firmware-update or device-recovery tool.

**Unblocks:** if Windows resets this card in software there is a sequence we
are missing — an FLR, a config write, a mailbox opcode — and finding it turns a
5-minute physical power cycle into a one-liner.

---

## Priority

If time is short, these four are worth more than the rest combined:

1. **#1 EDID dump** — the biggest gap, and needs only a second machine and a cable
2. **#2 HDCP oracle** — free, settles a live question
3. **#4 state (d)** — one BAR0 dump while capturing
4. **#3** — copy the `.sys`

#1 and #7 can also be done on Linux with our driver; comparing the two answers
is the point.

---

## Linux-side state, for when you return

- Working tree uncommitted; `RE_FINDINGS.md` documents milestones **M76–M81**.
- **M79 is a real fix**, verified on hardware: the driver rejected any 30-bit
  deep-colour source, because the MST3367 counts htotal in TMDS character
  clocks (×1.25 for 30-bit, ×1.5 for 36-bit). That source now matches 1080p60.
- The capture blocker is unresolved: receiver locks, mode matches, encoder
  spawns, and the SoC's VIC still reports no signal. Host-side configuration
  space is exhausted — see M76–M80 for what has been eliminated.
- `/lib/firmware/mz0380/MZ0380.FW.TXT` is `01.13` while the stock image carries
  `01.11`, forcing a re-upload on every insmod. Once the card is confirmed
  healthy, restore it:

      sudo cp /lib/firmware/mz0380/MZ0380.FW.TXT.stock \
              /lib/firmware/mz0380/MZ0380.FW.TXT

- `mz0380-m81-build-cardlog-fw.sh` builds a modified card rootfs that redirects
  the card's console to a host-readable file. **The first attempt broke the
  card's userspace** — it changed three things at once, including an unverified
  `-L` flag that probably made `video_capture_mgr` exit at boot. Rebuilt as a
  single one-line change but **not** reinstalled. `--restore` puts stock back.
