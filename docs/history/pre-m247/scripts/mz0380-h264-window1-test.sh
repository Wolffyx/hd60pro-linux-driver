#!/bin/bash
# One-spawn, host-only capture test for tinyvenc7's encoded-output path.
#
# The driver validates the card's duplicated length and slot fields, strips the
# 4 KiB transport header, and gives V4L2 one Annex-B access unit per buffer.
# STREAMOFF also dumps the dedicated window-1 buffers for diagnosis.
set -u
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
[ ! -d /sys/module/mz0380 ] || {
	echo "mz0380 is already loaded; unload it before this one-spawn test"
	exit 1
}

WAIT=${WAIT:-10}
LOG=${LOG:-/tmp/mz0380-h264-window1-op6.log}
HOLD=${HOLD:-/tmp/mz0380-h264-window1.h264}
LOADED=0

cleanup() {
	if [ "$LOADED" = 1 ] && [ -d /sys/module/mz0380 ]; then
		scripts/mz0380-spawns.sh commit || true
		if rmmod mz0380; then
			scripts/mz0380-spawns.sh unloaded || true
		else
			echo "WARNING: rmmod failed; run mz0380-live.sh status"
		fi
	fi
}
trap cleanup EXIT INT TERM

. scripts/mz0380-build.sh
MZKO=$(mz0380_resolve_module) || exit 1

echo "=== spawn budget before test ==="
# The module is known to be absent above; make that load boundary explicit so
# a new counter value equal to the previous module's value is still counted.
scripts/mz0380-spawns.sh unloaded >/dev/null || true
scripts/mz0380-spawns.sh
echo

modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
dmesg -C

insmod "$MZKO" dma_handshake=1 enable_dma=1 enable_video=1 \
	procfs_verbosity=2 dma_iova_remap=1 aic_on=1 stream_nosg=0 \
	force_timings=0 signal_poll_ms=4000 vic_fw=7 poll_drain_ms=0 \
	h264_probe=1 win_seq=1 win_start_op6=1 post_mask=0 vic_fast_kill=0 \
	h264_frame_divisor=2 \
	|| { echo "insmod failed"; exit 1; }
LOADED=1
sleep 5

NODE=
for name_file in /sys/class/video4linux/video*/name; do
	[ -r "$name_file" ] || continue
	case "$(cat "$name_file")" in
	mz0380*)
		node_dir=${name_file%/name}
		NODE=/dev/${node_dir##*/}
		break
		;;
	esac
done
[ -n "$NODE" ] || { echo "no mz0380 video node"; exit 1; }

echo "=== holding $NODE in STREAMON for ${WAIT}s (one encoder spawn) ==="
# timeout status 124 is acceptable; its SIGINT closes the node and triggers
# the window-1 dump after preserving every access unit delivered so far.
timeout -s INT --foreground "$WAIT" v4l2-ctl -d "$NODE" \
	--stream-mmap=8 --stream-count=600 --stream-to="$HOLD" || true
sleep 1

dmesg >"$LOG"
echo
echo "=== H.264 window-1 evidence ==="
dmesg | grep -aE "H\.264|SET_ENC_PARAMS|stream stop: EVENT|IOMMU|DMAR" | tail -80
echo
SIZE=$(stat -c %s "$HOLD" 2>/dev/null || echo 0)
echo "V4L2 H.264 file: $HOLD ($SIZE bytes)"
if [ "$SIZE" -gt 0 ]; then
	echo "RESULT: V4L2 delivered an H.264 bytestream."
	if command -v ffprobe >/dev/null 2>&1; then
		ffprobe -v error -select_streams v:0 \
			-show_entries stream=codec_name,width,height,avg_frame_rate \
			-of default=noprint_wrappers=1 "$HOLD" || true
	fi
elif dmesg | grep -aqE "H\.264 window1 buf\[[0-3]\].*extent=0x[1-9a-fA-F]"; then
	echo "RESULT: window 1 received data, but V4L2 delivered no access units."
else
	echo "RESULT: window 1 remained untouched. Keep $LOG; do not repeat yet."
fi
echo "Full kernel log: $LOG"
