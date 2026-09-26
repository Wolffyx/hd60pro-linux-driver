#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# M243: is the brightness change the CARD or the CAMERA?
#
# The operator sees the picture step brighter or darker, and the steps land on
# scene changes rather than ratcheting, which is what a camera's own auto
# exposure does.  Three driver-side theories have already been retracted for
# resting on numbers measured while another defect was active (M234 full-range
# labelling, M237 CSC staleness), so this does not add a fourth.  It measures
# the same camera through a SECOND, unrelated capture path.
#
#   both paths step the same way  -> the camera is doing it.  Nothing in this
#                                    driver is involved and the item closes.
#   only the PCIe path steps      -> it is ours, and the next suspect is
#                                    mst3367_commit_digital_output, the other
#                                    thing a timing change runs.
#
# Same scan, same statistics, on both:
#   sudo scripts/mz0380-m243-brightness-ab.sh pcie      # 1 encoder spawn
#   (move the HDMI cable to the USB capture card)
#   scripts/mz0380-m243-brightness-ab.sh usb /dev/videoN  # 0 spawns
#
# Point the camera at something whose scene actually changes - cover it, swing
# it to a lamp and back - because a still scene cannot show a step.  Capture at
# 2 fps: a slow ratchet is better seen over five minutes than over thirty
# seconds, and 6 MB/s does not stress anything.
set -u
cd "$(dirname "$0")/.."
command -v ffmpeg >/dev/null || { echo "ffmpeg is required"; exit 1; }

MODE=${1:-}
DEV=${2:-}
SECS=${SECS:-300}
FPS=${FPS:-2}
OUT=/tmp/mz0380-m243-$MODE.log

node() {
	local n
	for n in /sys/class/video4linux/video*/name; do
		[ -r "$n" ] || continue
		case "$(cat "$n")" in
		mz0380*|"HD60 Pro HDMI capture"*)
			echo "/dev/$(basename "$(dirname "$n")")"; return 0;;
		esac
	done
	return 1
}

case "$MODE" in
pcie)
	[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
	lsmod | grep -q '^mz0380' || ./mz0380-live.sh load || exit 1
	sleep 3
	DEV=$(node) || { echo "no mz0380 node"; exit 1; }
	;;
usb)
	[ -n "$DEV" ] || { echo "usage: $0 usb /dev/videoN"; exit 1; }
	# "videoN" is the placeholder in this script's own usage line, and it
	# was once passed through verbatim: ffmpeg failed, the scan read no
	# frames, and the run looked like a capture that produced nothing
	# rather than a device that was never named.
	if [ ! -c "$DEV" ]; then
		echo "$DEV is not a character device."
		echo "Plug the USB capture card in and pick its node from:"
		v4l2-ctl --list-devices 2>/dev/null || ls /dev/video* 2>/dev/null
		exit 1
	fi
	# Refuse to measure our own node and call it an independent path.
	if [ "$DEV" = "$(node 2>/dev/null || true)" ]; then
		echo "$DEV IS the mz0380 node - that is the PCIe path, not a second one"; exit 1
	fi
	;;
*)
	echo "usage: $0 pcie | usb /dev/videoN"; exit 1;;
esac

FRAMES=$((SECS * FPS))
scripts/mz0380-node-holders.sh "$DEV"

# M244: cumulative counters cannot say WHEN a chroma rejection happened, and
# that is the whole question - rejections during a colourless stretch are the
# check's documented false negative, rejections during a lit, coloured scene
# are genuine mid-fill catches. The scan prints one line per 20 frames, which
# at 2 fps is one line per 10 seconds; this watch prints one line per second
# with the same counters. Lining the two up dates every rejection against the
# luma of the scene that produced it.
WATCH_PID=""
if [ "$MODE" = pcie ] && [ -r /proc/mz0380-state ]; then
	scripts/mz0380-m230-flicker-watch.sh > /tmp/mz0380-m243-watch.log 2>&1 &
	WATCH_PID=$!
	trap '[ -n "$WATCH_PID" ] && kill "$WATCH_PID" 2>/dev/null' EXIT INT TERM
fi
echo "=== $MODE: $DEV, ${SECS}s at ${FPS} fps = $FRAMES frames -> $OUT ==="
case "${SCENE:-any}" in
bright)
	# The M238 discriminator needs a scene that is never colourless: a
	# genuinely neutral frame trips the chroma test's documented false
	# negative, and then the rejections prove nothing.
	echo "SCENE=bright: keep it lit and COLOURFUL throughout. Do NOT cover the"
	echo "lens and do not let the picture go dark - that is what this run tests."
	;;
*)
	echo "Change the scene several times while this runs (cover the lens, swing to a lamp)."
	;;
esac

# The USB leg once ran at the device's own default geometry while the scan
# sliced the stream as 1920x1080: 360 frames of 1280x720 divide exactly into
# 160 frames of 1080p, so it reported 160 frames, three "partial fills" and a
# range verdict, all of them artefacts of reading one resolution as another.
# Pin the geometry on the way in, and scale as a backstop for a device that
# cannot deliver it, so both paths reach the scan as the same 1920x1080 I420
# the scan assumes.
ffmpeg -hide_banner -loglevel error -f v4l2 -video_size 1920x1080 -i "$DEV" \
	-vf "fps=$FPS,scale=1920:1080" -pix_fmt yuv420p -f rawvideo \
	-frames:v "$FRAMES" - 2>/tmp/mz0380-m243-$MODE.ffmpeg.log |
	python3 scripts/mz0380-m226-blackframe-scan.py --interval $((FPS * 10)) \
		--min-frames "$FRAMES" --fps "$FPS" | tee "$OUT"

if [ "$MODE" = pcie ]; then
	echo
	grep -E '^  pixelformat|^  raw ' /proc/mz0380-state 2>/dev/null || true
	scripts/mz0380-spawns.sh commit || true
	if [ -n "$WATCH_PID" ]; then
		kill "$WATCH_PID" 2>/dev/null; WATCH_PID=""
		echo
		echo "=== when the chroma rejections happened (rawnochroma over time) ==="
		# Only the lines where the M238 counter itself moved, with the
		# elapsed second, so they can be read against the scan windows
		# above (one window = 10 seconds at 2 fps).
		awk '/rawnochroma=/ {
			match($0, /rawnochroma=[0-9]+/); v = substr($0, RSTART+12, RLENGTH-12)
			if (v != prev) { print; prev = v }
		}' /tmp/mz0380-m243-watch.log | tail -40
		chmod 644 /tmp/mz0380-m243-watch.log 2>/dev/null || true
	fi
fi
echo
echo "log: $OUT (ffmpeg stderr: /tmp/mz0380-m243-$MODE.ffmpeg.log)"
