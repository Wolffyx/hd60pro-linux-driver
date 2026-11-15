#!/bin/bash
# M45: watch the receiver's detect block LIVE while the source reacts.
#
# The key observation this is built around: running the bring-up visibly stops
# the connected camera's live preview. That is what a camera does when it
# detects an HDMI sink and switches its output to HDMI - so the source IS
# responding to our HPD, and the sink side is not as dead as "no lock"
# suggested. This correlates what the camera does with what the chip sees.
#
# Also settled by M44: MST3367 BANK3 is NOT writable (0/7 pattern bytes held),
# so it is not the EDID store - that lead is closed.
#
# WHAT TO DO WHILE IT RUNS (it watches for 40 s):
#   1. leave it alone for ~10 s (baseline),
#   2. UNPLUG the HDMI cable from the card,
#   3. PLUG it back in,
#   4. if the camera has an "HDMI output" / "record" mode toggle, cycle it.
#
# HOW TO READ THE "detect" LINES:
#   55=xx changes when you unplug/replug
#       -> the low bits of 0x55 track cable/5V presence: the receiver really
#          is wired to the connector and sees the source. Then "no-lock" with
#          idle counters means the source is attached but NOT transmitting
#          TMDS -> it is refusing because it cannot read a valid EDID, and
#          the EDID store is the thing still to find.
#   hper/vper/htot counters become non-zero (idle is vper=1fff, htot=0000)
#       -> the receiver IS seeing a TMDS clock: the source transmits, and the
#          fault is the receiver's per-mode configuration (M10 step 2 values,
#          which we never recovered) rather than the EDID.
#   LOCKED appears
#       -> re-run mz0380-m42-real-signal-test.sh immediately.
#   nothing changes at all, even on unplug
#       -> 0x55's low bits are static and the receiver is not wired to this
#          connector's hotplug/5V; suspect the input select (op41 input code)
#          or that the card routes a different physical input.
set -u
cd "$(dirname "$0")/.."

WATCH_SECS=${1:-40}

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

# M46: force the bring-up NOW (it is otherwise lazy, on first detect) so the
# input-select -> reset -> init -> EDID -> HPD sequence has already run before
# we start watching. The M45 run watched a receiver whose input had never been
# selected, which is very likely why nothing ever moved.
v4l2-ctl -d /dev/video0 --query-dv-timings >/dev/null 2>&1
sleep 1
echo "--- bring-up (input select must appear FIRST) ---"
dmesg | grep -E 'input select|MST3367|EDID|HPD'

echo "=========================================================="
echo " WATCHING FOR ${WATCH_SECS}s - now:"
echo "   1) wait ~10s, 2) UNPLUG the HDMI cable, 3) PLUG it back,"
echo "   4) cycle the camera's HDMI-output/record mode if it has one"
echo "=========================================================="
echo "watch $WATCH_SECS" > /proc/mz0380-hdmi

echo "--- detect transitions (each line = something changed) ---"
dmesg | grep -E 'detect 55=|watching MST3367|watch done'
echo "--- did we ever lock? ---"
dmesg | grep -c 'LOCKED'
