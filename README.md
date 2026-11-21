# mz0380 — Linux driver for the Elgato Game Capture HD60 Pro

A V4L2 driver for the Elgato **HD60 Pro** PCIe capture card (YUAN MZ0380,
PCI `12ab:0380` / `12ab:0381`). The card shows up as an ordinary camera,
`HD60 Pro HDMI capture`, that OBS, ffmpeg/ffplay, GStreamer and browsers can
open directly.

It is reverse-engineered, out-of-tree, and not affiliated with Elgato. It works
well on the hardware it was developed on; read [the encoder spawn
budget](#the-encoder-spawn-budget) before you start experimenting.

## What you get

| | |
|---|---|
| Video | HDMI in, 1920x1080 at the source rate (50/60 fps tested; 1080i, 720p and 480p are detected but untested) |
| Formats | `YU12` (I420), `NV12`, `YV12` uncompressed, and `H264` from the card's own encoder |
| No source | a NO SIGNAL picture at 2 fps instead of a frozen frame or a timeout, in every format |
| Hot-plug | unplug and replug the source mid-capture, or swap it for one at another frame rate; the stream carries on |
| Audio | HDMI audio as an ALSA capture device (48 kHz stereo), on by default |
| Conformance | `v4l2-compliance` clean |

Applications choose the format. `YU12` is the default; OBS should be set to
**NV12** (it does not offer `YU12`).

## Requirements

- A supported HD60 Pro in a PCIe slot (see [Supported cards](#supported-cards)).
- **A translating IOMMU** - VT-d or AMD-Vi enabled in firmware, and Linux booted
  *without* `iommu=off` or `iommu=pt`. On Intel, most kernels also need
  `intel_iommu=on` on the command line: VT-d stays off unless the kernel was
  built with `CONFIG_INTEL_IOMMU_DEFAULT_ON`. The card can only address the top 32
  bits of a DMA target, so the driver places every buffer at its own 4 GiB
  boundary through the IOMMU. Without one, the node appears but streaming is
  refused with a message saying why. If the machine has to keep `iommu=pt`,
  see [Keeping `iommu=pt`](#keeping-iommupt).
- Kernel headers for the running kernel (`/lib/modules/$(uname -r)/build`).
- The compiler that built your kernel. The Makefile reads that from the
  kernel's own config, so a clang-built kernel gets clang and a gcc-built one
  gets gcc - install `clang` and `lld` if your kernel is clang-built.
- Optional: `v4l-utils` (`v4l2-ctl`, `v4l2-compliance`), `ffmpeg`, OBS Studio,
  `dkms`.

If Secure Boot enforces module signatures, sign the module with your
distribution's usual mechanism (an unsigned one fails with `Key was rejected
by service`).

## Install

```bash
make
sudo make dkms-install
sudo modprobe mz0380
```

DKMS rebuilds the module on every kernel upgrade. Without DKMS, `sudo make
install` installs the current build for the running kernel only. Either way
the module then loads by itself whenever the card is present, with no options.
Remove it with `sudo make dkms-uninstall` (or `sudo make uninstall`).

### NixOS

NixOS has no `/lib/modules/.../build` and no DKMS, so use the flake instead of
the Makefile targets. It builds the module for whatever kernel the system
runs (`boot.kernelPackages`) and rebuilds it with every kernel change:

```nix
# flake.nix
{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  inputs.mz0380.url = "github:Wolffyx/hd60pro-linux-driver";  # or .../v0.3.0 to pin a release
  inputs.mz0380.inputs.nixpkgs.follows = "nixpkgs";

  outputs = { nixpkgs, mz0380, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      modules = [
        ./configuration.nix
        mz0380.nixosModules.default
        { hardware.mz0380.enable = true; }
      ];
    };
  };
}
```

After `nixos-rebuild switch` the module loads by itself when the card is
present (or now, with `sudo modprobe mz0380`).

Module parameters are NixOS options too, instead of `/etc/modprobe.d`:

```nix
hardware.mz0380 = {
  enable = true;
  enableAudio = false;                        # video only (audio is on by default)
  moduleOptions = { raw_deliver = false; };   # any other parameter
};
```

They take effect the next time the module loads: reboot, or
`sudo modprobe -r mz0380 && sudo modprobe mz0380`. The card's audio is a
separate ALSA capture device, so in OBS add it as its own audio input.

Without flakes, add the package directly from a checkout of this repository:

```nix
boot.extraModulePackages = [
  (config.boot.kernelPackages.callPackage /path/to/hd60pro-linux-driver/nix/package.nix { })
];
```

The IOMMU requirement is the same as everywhere else. The NixOS module adds
`intel_iommu=on` to `boot.kernelParams` (needed on Intel, ignored on AMD), so
reboot after the first `nixos-rebuild switch`; with the non-flake snippet, add
it yourself. The module also warns at build time if `boot.kernelParams`
contains `iommu=pt` or `iommu=off`. The
flake has been build-tested against nixpkgs' default kernel but not yet run
on a NixOS machine with the card.

### Keeping `iommu=pt`

A system booted with `iommu=pt` (common where devices are passed through to
virtual machines) can keep it. Linux lets a single IOMMU group be switched to
translated DMA while its device has no driver bound, and
`scripts/mz0380-prepare-iommu.sh` does that for the card alone:

```bash
lspci -D -d 12ab:                                  # the card's address
sudo modprobe -r mz0380
sudo scripts/mz0380-prepare-iommu.sh 0000:04:00.0
sudo modprobe mz0380
```

The card must be alone in its IOMMU group. The script refuses if it is not,
if a driver is bound, or if the card is not idle and powered; it loads no
driver and resets nothing. The setting does not survive a reboot, and the
module loads by itself at boot, so the three `sudo` steps are needed once
after each boot. `iommu=off` is different: there is no IOMMU to switch, and
the script says so.

Check that it came up:

```bash
v4l2-ctl --list-devices
sudo dmesg | grep mz0380
```

A healthy load prints:

```text
mz0380[0]: CMD_INIT answered on attempt 1 after ...ms (status=...)
mz0380 0000:04:00.0: registered video0
mz0380 0000:04:00.0: Elgato Game Capture HD60 Pro (subsystem 1cfa:0006), firmware 1.11
```

and the first time an application looks at the source, `HDMI signal:
1920x1080p@60`.

There is no firmware to install: the card boots its own flash image, and the
driver never uploads anything to it.

## Use it

The node number depends on what else is plugged in. This finds it by name:

```bash
NODE=$(grep -l 'HD60 Pro HDMI capture' /sys/class/video4linux/video*/name | sed 's|/sys/class/video4linux/|/dev/|; s|/name||')
```

**Watch it** with ffplay:

```bash
ffplay -f v4l2 -input_format yuv420p -video_size 1920x1080 "$NODE"
```

**OBS**: add a *Video Capture Device (V4L2)* source, pick *HD60 Pro HDMI
capture*, and set *Video Format* to **NV12**. Turn off *Deactivate when not
showing* - see the next section for why. If the preview lags behind the
source, turn off *Use Buffering* in the same dialog.

**Record** with ffmpeg, raw in and H.264 out through your GPU or CPU:

```bash
ffmpeg -f v4l2 -input_format yuv420p -video_size 1920x1080 -i "$NODE" -c:v libx264 -preset veryfast out.mkv
```

or take the card's own H.264 stream without re-encoding:

```bash
ffmpeg -f v4l2 -input_format h264 -i "$NODE" -c copy out.mkv
```

**Browsers and conferencing apps** (Chrome, Firefox, Discord, ...) should list
the card as a camera and use the uncompressed formats (not yet tested here).

**Status** at any time, at no cost to a running capture:

```bash
v4l2-ctl -d "$NODE" --log-status && sudo dmesg | tail -12
cat /proc/mz0380-state
```

## The encoder spawn budget

**Read this before experimenting.** Every capture start that has to (re)start
the card's encoder forks a new encoder process on the card, and the card's
firmware stops answering after somewhere between **8 and 18** of those per
power cycle. A wedged card shows no picture and its commands time out; only
removing power fully - the PSU switch or the mains plug, not a reboot -
brings it back.

The driver is built around this:

- The encoder keeps running between captures (`persistent_h264=1`). Closing and
  reopening an application, or OBS hiding and showing a source, costs nothing.
- A capture started with no HDMI source shows NO SIGNAL and spends nothing; the
  encoder starts once, when a source appears.
- An unplug/replug of the same source mode costs nothing.
- What does cost one spawn: the first capture after loading the module,
  switching between an uncompressed format and H.264, changing the H.264
  bitrate/GOP before an H.264 capture, and a source that comes back in a
  different mode. Switching between `YU12`, `NV12` and `YV12` is free.

`/proc/mz0380-state` (`enc spawns`) and `v4l2-ctl --log-status` show the count
since the module was loaded; `scripts/mz0380-spawns.sh` keeps a running total
per power cycle. Applications that retry a failing capture in a loop can burn
through the budget in seconds - which is why OBS should not *deactivate* the
source when it is hidden.

## Formats and frame rates

| Format | Bytes per 1080p frame | Notes |
|---|---|---|
| `YU12` | 3,110,400 | what the card writes; default |
| `NV12` | 3,110,400 | for OBS and most hardware encoders |
| `YV12` | 3,110,400 | |
| `H264` | up to ~610 KiB | the card's encoder, High profile; key frames every 32 frames |

There is no scaler: the frame size is the source's. A source that changes
resolution mid-capture raises a V4L2 `SOURCE_CHANGE` event, shows NO SIGNAL,
and the application restarts the capture at the new size.

A source that changes only its frame rate - a 60 Hz device swapped for a 30 Hz
one - is followed without the application doing anything: once the new mode
has held for about four seconds the driver restarts the card's encoder for it,
and the picture returns a few seconds later. Each such restart is one encoder
spawn, so the driver does it at most `mode_follow` times per load (4 by
default). After that, or with `mode_follow=0`, NO SIGNAL stays up until the
application restarts the capture - in OBS, re-select the device in the
source's properties.

The frame rate is the source's. A 30 fps game over a 60 Hz HDMI link arrives
as 60 frames per second with every frame doubled - that is the source, not the
driver.

The H.264 stream has two controls, `video_bitrate` (256 kbit/s - 12 Mbit/s,
default 4 Mbit/s) and `video_gop_size` (1-255, default 32), set with `v4l2-ctl
-c` between captures.

## Module parameters

None are needed. `modinfo mz0380` lists them all; the ones worth knowing:

| Parameter | Default | |
|---|---|---|
| `raw_deliver` | 1 | startup format for applications that never choose one: 1 = YU12, 0 = H.264 |
| `persistent_h264` | 1 | keep the encoder running between captures - leave on |
| `stream_without_signal` | 1 | allow a capture to start with no source (shows NO SIGNAL) |
| `mode_follow` | 4 | automatic encoder restarts per load when the source changes frame rate; 0 = leave it to the application |
| `h264_frame_divisor` | 0 | 0 = encode every frame; 2 = every second frame |
| `post_skip` | 0 | uncompressed decimation: deliver one in `post_skip+1` frames |
| `raw_full_range` | 0 | label uncompressed formats full-range (0-255) |
| `dma_iova_base` | 0x100000000 | where the 4 GiB-aligned DMA windows go, if that range is taken |
| `enable_audio` | 1 | register the HDMI audio capture device; 0 = video only |
| `enable_video` | 1 | 0 brings the card up without a video node |

Set them in `/etc/modprobe.d/mz0380.conf` (installed with commented examples).

## Troubleshooting

**No `/dev/video*` node** - `sudo dmesg | grep mz0380` and `lspci -nnk -d 12ab:`.
Usual causes: a module built for another kernel (`Invalid module format`;
compare `modinfo -F vermagic mz0380.ko` with `uname -r`), a module signature
rejection, or an unknown subsystem ID (the log lists the `card=` override).

**`streaming unavailable` / `no IOMMU domain`** - enable VT-d/AMD-Vi in the
firmware setup and remove `iommu=off` / `iommu=pt` from the kernel command
line. On Intel, also add `intel_iommu=on`: without it most kernels leave VT-d
off even when the firmware enables it (`sudo dmesg | grep -i dmar` then shows
only `DMAR-IR` lines and no `DMAR: IOMMU enabled`). Or keep `iommu=pt` and switch the card alone - see
[Keeping `iommu=pt`](#keeping-iommupt).

**`card handshake failed` or commands time out (`-110`)** - the card's
firmware is not answering: it is wedged (see the spawn budget) or still
booting. Remove power fully, then try again.

**Picture shows NO SIGNAL with a source connected** - check
`v4l2-ctl -d "$NODE" --query-dv-timings`. If it reports no link, the source is
not transmitting: wake it, or toggle its HDMI output. If the log says the
source mode is not supported, set the source to 1080p, 1080i, 720p or 480p.

**A camera leaves HDMI mode when the module loads** - the first load after a
power-up has to reset the HDMI receiver, and a camera reads that as an unplug;
most come back within a few seconds. Reloading the module keeps the receiver
as it is, so the source sees nothing. If a source does not come back,
`echo hpd 1 | sudo tee /proc/mz0380-hdmi` sends it one hot-plug pulse.

**NO SIGNAL stays up after swapping devices** - the new device sends another
resolution, or the automatic restarts are used up (`mode follow` in
`/proc/mz0380-state`, and the log says which). Restart the capture in the
application; a module reload is not needed.

**Wrong colours** (green/magenta cast) after the source switched modes -
`echo csc | sudo tee /proc/mz0380-hdmi` re-reads the source colour space.

**The module will not unload** - something has the node open:
`scripts/mz0380-node-holders.sh` names it (desktop session daemons probe every
new camera). `sudo ./mz0380-live.sh unload` closes the holders first.

When reporting a problem, include `v4l2-ctl -d "$NODE" --log-status` output
(it lands in `dmesg`), `cat /proc/mz0380-state`, and the `mz0380` lines from
`dmesg`.

## Diagnostics and development

| | |
|---|---|
| `/proc/mz0380` | cards, PCI identity, firmware version, node |
| `/proc/mz0380-state` | pipeline, signal and per-format delivery counters; free to read while capturing |
| `/proc/mz0380-hdmi` | receiver commands (root): `watch`, `csc`, `hpd`, `edid` - see `cat /proc/mz0380-hdmi` |
| `dyndbg` | `insmod mz0380.ko dyndbg='format ^mbx +p'` logs every command sent to the card; `dyndbg=+p` logs everything |

For development, `./mz0380-live.sh load|status|watch|unload` builds, smoke
tests, loads and inspects the module from the tree, and keeps the spawn tally.
`make all-kernels` builds for every installed kernel into `ko/`.

| File | |
|---|---|
| `NEXT_SESSION_START.md` | the current state of the work and what is next |
| `RE_FINDINGS.md` | the reverse-engineering record, milestone by milestone (M1-M247); code comments cite it as `Mnnn` |
| `docs/history/` | superseded plans and handoffs, kept for their reasoning |
| `docs/history/pre-m247/` | the diagnostics code and experiment scripts M247 removed from the driver - a record, never built |
| `scripts/` | hardware tests (`mz0380-m*-*.sh`), the spawn tally, frame analysis, RE tooling |

```
src/mz0380-pci.c          probe/remove, suspend/resume
src/mz0380-cards.c        board table, probe-time setup
src/mz0380-mailbox.c      the card's command mailbox
src/mz0380-fw.c           handshake with the firmware the card booted
src/mz0380-mst3367*.c     the MST3367 HDMI receiver (bring-up, mode detection)
src/mz0380-signal.c       signal state, hot-plug recovery, DV timings ioctls
src/mz0380-video.c        V4L2 node: formats, inputs, controls
src/mz0380-vb2.c          stream start/stop, persistent encoder attach
src/mz0380-dma.c          DMA windows, IRQ, completion events
src/mz0380-dma-stream.c   the card's encoder start/stop command sequence
src/mz0380-dma-drain.c    H.264 and raw frame delivery
src/mz0380-dma-raw.c      raw slot completeness checks and the copy out
src/mz0380-no-signal.c    the NO SIGNAL picture
src/mz0380-audio.c        ALSA capture
src/mz0380-proc.c         /proc files
```

## Supported cards

The PCI function must be vendor/device `12ab:0380` or `12ab:0381` with one of:

| Subsystem | Variant |
|---|---|
| `1cfa:0003` | HD60 Pro Rev. 1 |
| `1cfa:0005` | HD60 Pro Rev. 2 (unreleased) |
| `1cfa:0006` | HD60 Pro Rev. 1 + Ryzen fix (the development card) |
| `1cfa:0010` | HD60 Pro Rev. 3 (`DEV_0381`, untested) |
| `12ab:05cf` | HD60 Pro prototype |

```bash
lspci -nn -d 12ab:
```

The Elgato 4K60 Pro and HD60 S are different hardware and are not supported.

## Releases and branches

`main` holds released code only; every tagged release (`v0.3.0`, ...) is on
the [releases page](https://github.com/Wolffyx/hd60pro-linux-driver/releases).
Work happens on `dev`:

1. Branch from `dev`, open the pull request against `dev`.
2. When `dev` is ready to ship, run the hardware checks on it with OBS
   closed - `sudo scripts/mz0380-m85-unload-smoke.sh` (zero spawns, includes
   removing the card under a running audio capture) and `sudo
   BASELINE=none scripts/mz0380-m247-verify.sh` - then bump the version on `dev`
   (`scripts/mz0380-version.sh X.Y.Z`, which sets `src/mz0380.h` and
   `dkms.conf`) and open a pull request from `dev` into `main`.
3. Merging it publishes `vX.Y.Z` with release notes
   (`.github/workflows/release.yml`). The pull request already fails if the
   module does not build or that version was released before.

## License

**GPL-2.0-or-later**, and for a Linux kernel module that is not really a
choice. Recorded as an SPDX tag in every source file, with the full text in
`COPYING`.

GPLv3 is not available here, for three independent reasons:

1. **The kernel is GPLv2-only.** Linux does not carry the "or later" clause, and
   GPLv2 and GPLv3 are mutually incompatible. A loadable module is a derivative
   work of the kernel it links against, so a GPLv3 module could not be
   distributed for use with Linux.
2. **`MODULE_LICENSE()` has no GPLv3 ident.** The accepted strings in
   `include/linux/module.h` are all GPLv2-based, plus `"Proprietary"`. A
   GPLv3-only module would have to declare itself proprietary, which taints the
   kernel and makes the loader refuse to bind `EXPORT_SYMBOL_GPL` symbols - and
   videobuf2, which this driver is built on, exports GPL-only. The module would
   not load.
`MODULE_LICENSE("GPL")` already means "GPLv2 or later", so the built module is
consistent with the SPDX tags with no change needed.

Note that reasons 1 and 2 are properties of building a Linux kernel module and
hold no matter whose code is in the tree. Removing third-party code, as the
next section describes, does not make GPLv3 available.

## Provenance

This driver was written for the HD60 Pro (MZ0380). It is not a fork.

**sc0710 (Elgato 4K60 Pro Mk.2, Steven Toth) - not used.** The repository
directory is named `sc0710` for historical reasons and early revisions carried
that driver's sources locally as reading material, but they were never tracked
in git, never built by default, and never distributed. In 2026-08 they were
moved out of `src/` to a gitignored `reference/` directory and the `legacy`
build target was deleted. Three register-map sections (`BAR0` XDMA, `BAR0`
Xilinx AXI IIC, `BAR0` HDMI signal status) had been modelled on it; every
define in all three was unused, hardware disproved the model on all three
counts, and they were removed. **No code in `src/` derives from sc0710.**

**hdcapm (Startech USB2HDCAPM, Steven Toth / Michael Grzeschik, GPL-2) - used,
and credited.** `src/mz0380-mst3367.c` follows the structure of that driver's
`mst3367_init_setup()`. This is a genuine and deliberate dependency, and it is
kept because the alternative is worse, not because it could not be avoided:

- hdcapm reverse-engineered **the same Windows driver** this project does, for
  a board built on the same Vatics Mozart 395s + MST3367 combination.
- Every register value in the sequence was independently re-derived from our
  own disassembly of `e60MZ0380.X64.SYS` and agrees with theirs. See
  `docs/re-2026-07-05/mst3367-reference-from-gpl-driver.md`, which marks each
  confirmation inline.
- The values are therefore facts about how the MST3367 is brought up, not
  authored expression, and we hold an independent derivation of them.

Since the project is GPL-2.0-or-later regardless, the honest thing is to keep
the citation rather than paraphrase the code to obscure it.

**Everything else** comes from this project's own reverse engineering of the
card's firmware image (`ep.ko`, `video_capture_mgr`, `tinyvenc5`/`tinyvenc7`)
and the Windows driver, documented milestone by milestone in `RE_FINDINGS.md`.
