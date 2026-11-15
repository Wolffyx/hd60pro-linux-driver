#!/bin/bash
# M168: does the V4L2 node describe the device it actually is?
#
# Two defects, both on the SHIPPING defaults, both invisible to every capture
# script in this tree because they all drive the node through v4l2-ctl with the
# format already decided:
#
#  1. THE FOURCC WAS WRONG. The poll-drain payload is planar I420 - Y, then a
#     960x540 U plane, then a 960x540 V plane (M130, confirmed visually and by
#     correlation). The node advertised V4L2_PIX_FMT_NV12, which is the same
#     byte count with the chroma INTERLEAVED. Any application that trusts the
#     driver - ffmpeg, GStreamer, OBS - therefore rendered the magenta/green
#     interleave banding RE_FINDINGS describes at M130. The frame was always
#     right; the label was wrong.
#
#  2. THE NODE CONTRADICTED ITSELF. ENUM_FMT returned the raw format while
#     ENUM_FRAMESIZES and ENUM_FRAMEINTERVALS still tested for H.264 and so
#     returned -EINVAL for the very format ENUM_FMT had just handed out. A
#     client that enumerates before opening - which is the documented order -
#     got "this device supports no sizes".
#
# And one behaviour: under fw=5 the card is a single-shot grabber (M166, a
# proven bound). Until M168 the driver did nothing about it, so a reader that
# kept reading blocked in DQBUF forever after its one correct frame. It now
# errors the queue stall_eos_ms after the last delivery, which is how V4L2 says
# "this source has stopped": DQBUF returns -EIO and the application exits with
# the frame it got, instead of looking like dead hardware.
#
# Cost: the enumeration half is ZERO spawns. The capture half is ONE.
set -u
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

CAP=${CAP:-/tmp/cap-m168.i420}
WANT=${WANT:-3110400}

. scripts/mz0380-build.sh
MZKO=$(mz0380_resolve_module) || exit 1

rmmod mz0380 2>/dev/null
sleep 1
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
dmesg -C

# A plain insmod, no arguments. M167: the shipping defaults are the capture
# configuration, and this script must not quietly repair them.
echo "=== 1. plain insmod, no arguments ==="
insmod "$MZKO" || { echo "insmod failed"; exit 1; }
sleep 3

echo
echo "=== 2. is the card's mailbox alive? (2s, zero spawns) ==="
# The cheapest test in the tree. A deaf mailbox looks exactly like a dead HDMI
# source further down, so rule it out before believing anything else here.
dmesg | grep -a "CMD_INIT\|handshake failed" | tail -3
if dmesg | grep -qa "mailbox is deaf"; then
	echo
	echo "CARD IS WEDGED - the mailbox is deaf. Power-cycle; nothing below is meaningful."
	exit 1
fi

NODE=
for n in /sys/class/video4linux/video*/name; do
	[ -r "$n" ] || continue
	case "$(cat "$n")" in
	mz0380*) NODE=/dev/$(basename "$(dirname "$n")");;
	esac
done
[ -n "$NODE" ] || { echo "no mz0380 video node registered"; exit 1; }
echo "using $NODE"

echo
echo "=== 3. the ABI, zero spawns ==="
echo "--- v4l2-ctl --list-formats-ext ---"
v4l2-ctl -d "$NODE" --list-formats-ext
echo "--- v4l2-ctl --get-fmt-video ---"
v4l2-ctl -d "$NODE" --get-fmt-video

# --list-formats-ext prints the fourcc as "  [0]: 'YU12' (Planar YUV 4:2:0)".
# "Pixel Format :" is --get-fmt-video's spelling and does not appear here; the
# first version of this check looked for that and reported FAIL against a driver
# that was right, which is the same class of mistake M168 is about.
FOURCC=$(v4l2-ctl -d "$NODE" --list-formats-ext 2>/dev/null |
	 sed -n "s/^[[:space:]]*\[[0-9]\+\][[:space:]]*:[[:space:]]*'\([A-Za-z0-9]*\)'.*/\1/p" | head -1)
SIZES=$(v4l2-ctl -d "$NODE" --list-formats-ext 2>/dev/null | grep -c "Size: Discrete")

echo
# YU12 is v4l2-ctl's spelling of V4L2_PIX_FMT_YUV420, i.e. planar I420.
case "$FOURCC" in
YU12) echo "PASS  fourcc = YU12 (V4L2_PIX_FMT_YUV420, planar I420) - matches M130";;
NV12) echo "FAIL  fourcc = NV12 - the M168 fix is not in the loaded module";;
*)    echo "FAIL  fourcc = '${FOURCC:-none}' - expected YU12";;
esac
if [ "$SIZES" -gt 0 ]; then
	echo "PASS  ENUM_FRAMESIZES returned $SIZES discrete size(s) for that format"
else
	echo "FAIL  ENUM_FRAMESIZES returned nothing - the node still contradicts itself"
fi

echo
echo "=== 4. capture, ONE spawn ==="
echo "Asking for 3 frames on a card that can deliver 1. Pre-M168 this blocked"
echo "until the timeout killed v4l2-ctl mid-write; now the driver should say"
echo "end-of-stream after ~2s and v4l2-ctl should exit by itself."
rm -f "$CAP"
START=$(date +%s)
timeout -s INT --foreground 60 \
	v4l2-ctl -d "$NODE" --stream-mmap --stream-count=3 --stream-to="$CAP"
RC=$?
ELAPSED=$(( $(date +%s) - START ))
SZ=$(stat -c %s "$CAP" 2>/dev/null || echo 0)
echo "v4l2-ctl rc=$RC after ${ELAPSED}s, captured $SZ bytes"

echo
if [ "$RC" = 124 ]; then
	echo "FAIL  v4l2-ctl ran into the 60s timeout - DQBUF still blocks forever"
elif [ "$SZ" = "$WANT" ]; then
	echo "PASS  exactly one WHOLE frame ($WANT bytes) and the reader exited on its own"
elif [ "$SZ" = 0 ]; then
	echo "FAIL  no frame at all - check the dmesg below before blaming M168"
else
	echo "PARTIAL  $SZ bytes, wanted a multiple of $WANT - see the dmesg below"
fi

echo
echo "=== 5. did the driver say end-of-stream? ==="
dmesg | grep -a "signalling end of stream" || echo "(no EOS line - the stall path did not fire)"

echo
echo "=== 6. is it a real picture? ==="
if [ -s "$CAP" ]; then
	scripts/mz0380-m127-splash.py "$CAP" || true
	echo
	echo "  view it:  ffplay -f rawvideo -pixel_format yuv420p -video_size 1920x1080 $CAP"
fi

echo
echo "=== 7. spawn budget ==="
# M169: bank this run's spawns while the module is still loaded.
scripts/mz0380-spawns.sh commit || true

echo
echo "=== 8. dmesg ==="
dmesg | grep -a "mz0380" | tail -25
