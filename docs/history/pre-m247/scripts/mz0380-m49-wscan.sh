#!/bin/bash
# M49: find the EDID RAM by writability.
#
# WHAT M48 PROVED: the camera reacted to the hotplug edge and then FELL BACK
# to its LCD instead of staying in HDMI output. That is exactly what a source
# does when it sees a sink appear, tries to read the EDID over DDC, and gets
# nothing back. So:
#     HPD WORKS - our hotplug line really does reach the connector.
#     THE EDID IS THE ONE REMAINING FAULT.
# Three unknowns became one.
#
# Nothing but the receiver exists on the bus (only 0x9c and 0x98 answer), so
# the EDID has to be served out of the receiver's own RAM. This finds that RAM
# the only way left without a datasheet: by writability. A config register has
# reserved/hardwired bits and will not return an arbitrary byte; a RAM cell
# returns exactly what you wrote. The scan writes 0x5a then 0xa5 to every
# register of every bank and restores the original value immediately.
#
# READ THE MAP ('#' = held both test values, '.' = did not):
#   a long contiguous run of '#'
#       -> that is the EDID RAM. Tell me the bank and offset range and the
#          EDID writer gets retargeted at it; the camera should then stay in
#          HDMI output mode and the receiver should lock.
#   only scattered '#'
#       -> those are ordinary read/write config registers, not a RAM window.
#          The EDID RAM is then gated behind an enable bit we do not know, and
#          the live Windows I2C trace is the remaining path.
#   no '#' at all
#       -> the whole register file is read-only through this path, which would
#          contradict the init writes that demonstrably stick; treat as a bug
#          in the scan and tell me.
#
# Safe: every register is restored to the value it had, and the receiver is
# re-initialised from scratch on every module load anyway.
set -u
cd "$(dirname "$0")/.."

make || { echo "build failed"; exit 1; }

fuser -k /dev/video0 2>/dev/null; sleep 1
rmmod mz0380 2>/dev/null
if lsmod | grep -q '^mz0380'; then echo "FATAL: still loaded"; exit 1; fi
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm \
	|| { echo "modprobe deps failed"; exit 1; }

dmesg -C
insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
	enable_video=1 procfs_verbosity=2 stream_nosg=0 dma_iova_remap=1 \
	|| { echo "insmod failed"; exit 1; }
sleep 3

v4l2-ctl -d /dev/video0 --query-dv-timings >/dev/null 2>&1
sleep 1

echo "=============== WRITABILITY SCAN (takes ~1 min) ==============="
echo wscan > /proc/mz0380-hdmi

echo "--- contiguous writable runs (a 128-byte run = one EDID block) ---"
dmesg | grep -E 'bank[0-3] run|bank[0-3]:|writability scan'
