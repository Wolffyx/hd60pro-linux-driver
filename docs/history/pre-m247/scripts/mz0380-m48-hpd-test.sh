#!/bin/bash
# M48: HPD, and ONLY HPD.
#
# Every test so far moved several things at once (input select, receiver
# init, EDID push, HPD), and the source reacted exactly ONCE across all of
# them - which is as easily explained by a camera idle-timeout as by our
# hotplug edge. Five EDID opcodes (0x20/0x1f/0x1e/0x1b/0x1d) produced no
# reaction at all, so the EDID route via the mailbox looks closed.
#
# This changes ONE variable. It pulses the hotplug line 5 times with 4 s
# between edges and does nothing else. A source that sees a sink appear and
# disappear should react EVERY time, reproducibly.
#
# WATCH THE CAMERA SCREEN FOR THE WHOLE RUN (~40 s) and note whether it
# reacts on each "assert" line, once, or never.
#
# WHAT IT MEANS:
#   reacts on EVERY pulse
#       -> we really do own the board's hotplug line. Then the EDID is being
#          served by something we do not drive (the factory-programmed EDID
#          MCU the RE docs describe), and the remaining fault is the
#          receiver's own per-mode configuration - the values from M10 step 2
#          that were never recovered. A live Windows I2C trace is then the
#          shortest path, and it is the SAME trace that would settle EDID.
#   reacts NEVER (and the first time was a coincidence)
#       -> our HPD is not reaching the connector. GPIO pin1 reads back as 1,
#          so either pin1 is not the hotplug line on this board revision, or
#          the pin drives something behind a buffer that also needs the EDID
#          MCU alive. Next: sweep the other GPIO pins one at a time and watch
#          the camera, which is the only oracle we have.
#   reacts only the first time
#       -> the source latches sink-presence and will not re-read without a
#          longer HPD-low period; re-run with a longer gap:
#             sudo bash mz0380-m48-hpd-test.sh 5 8000
set -u
cd "$(dirname "$0")/.."

COUNT=${1:-5}
GAP=${2:-4000}

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

# bring the receiver up once (input select -> reset -> init -> HPD)
v4l2-ctl -d /dev/video0 --query-dv-timings >/dev/null 2>&1
sleep 1

echo "=========================================================="
echo " PULSING HPD $COUNT TIMES, ${GAP}ms PER EDGE - WATCH THE CAMERA"
echo " Note whether it reacts EVERY time, ONCE, or NEVER."
echo "=========================================================="
echo "hpd $COUNT $GAP" > /proc/mz0380-hdmi

echo "--- pulse log ---"
dmesg | grep -E 'HPD pulse|HPD asserted|HPD deasserted'
echo "--- did the receiver ever see anything? ---"
echo "watch 5" > /proc/mz0380-hdmi
dmesg | grep -E 'detect 55=' | tail -3
