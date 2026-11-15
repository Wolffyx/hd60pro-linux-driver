#!/bin/bash
# M211: do the op02/op08 raw banks fill while the ENCODED path is running?
#
# M209 started the raw ring alone and the producer never ran.  M210 added the
# entire Windows encoder tail and it still never ran: SET_VIC, both
# SET_ENC_PARAMS, SET_PREVIEW_PARAMS, and events=0 with all eight slots
# poison-intact.  The one structural difference left is the op 0x04 encoded
# window, which those runs never registered - so the working assumption flips:
# tinyvenc validates its whole output set before starting anything.
#
# This run therefore does what Windows does - registers op02, op08 AND op04 -
# and rides the known-good h264_probe path that already delivers 60 fps.  It is
# passive with respect to that path: V4L2 still negotiates H.264, completion
# routing and delivery are untouched, and the raw banks are only poisoned,
# registered, and read back at stop.
#
# It also samples the first sixteen bytes of each op02 slot on every encoded
# completion (M212) and logs them when they change.  Point the camera at
# something that visibly changes - wave a hand, cover the lens, swing it from a
# lamp to a dark corner - and those samples answer M128a's open question:
# sixteen bytes that track the scene are source pixels, which makes this a DMA
# that dies after one burst rather than a capture that never happens.
#
# Scoring is the poison: if any op02/op08 slot reports a non-zero extent after a
# live encoded capture, the raw surface exists and is a by-product of the
# encoded pipeline.  If all eight are pristine, the ring is not the Linux raw
# source and the search goes back to the Windows binary.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
command -v v4l2-ctl >/dev/null || { echo "v4l2-ctl is required"; exit 1; }

KEEPLOADED=${KEEPLOADED:-0}
SECONDS_LIVE=${SECONDS_LIVE:-3}
FRAMES=${FRAMES:-60}
CAPTURE_LOG=$(mktemp /tmp/mz0380-m211-v4l2.XXXXXX.log) || exit 1
RAW_OUT=$(mktemp /tmp/mz0380-m211.XXXXXX.h264) || exit 1
DMESG_OUT=/tmp/mz0380-m211-dmesg.txt
CAPTURE_PID=""

reap_capture() {
	local sig deadline

	[ -n "$CAPTURE_PID" ] || return 0
	kill -0 "$CAPTURE_PID" 2>/dev/null || { CAPTURE_PID=""; return 0; }

	for sig in INT TERM KILL; do
		kill -"$sig" "$CAPTURE_PID" 2>/dev/null || true
		deadline=$(( $(date +%s%3N) + 1500 ))
		while kill -0 "$CAPTURE_PID" 2>/dev/null; do
			[ "$(date +%s%3N)" -ge "$deadline" ] && break
			sleep 0.05
		done
		kill -0 "$CAPTURE_PID" 2>/dev/null || break
	done

	if kill -0 "$CAPTURE_PID" 2>/dev/null; then
		echo "WARNING: v4l2-ctl pid $CAPTURE_PID survived SIGKILL - it is stuck" \
		     "in the driver (D state); a card power cycle may be required." >&2
	else
		wait "$CAPTURE_PID" 2>/dev/null || true
	fi
	CAPTURE_PID=""
}

save_evidence() {
	dmesg > "$DMESG_OUT" 2>/dev/null || true
	chmod 644 "$DMESG_OUT" "$CAPTURE_LOG" "$RAW_OUT" 2>/dev/null || true
}

cleanup() {
	reap_capture
	# The raw-bank read-back happens in the stop path, so the module must be
	# unloaded before the log is scored.
	if [ "$KEEPLOADED" != 1 ]; then
		timeout 30 ./mz0380-live.sh unload >/dev/null 2>&1 || \
			echo "WARNING: unload did not complete within 30s" >&2
	fi
	save_evidence
}
trap cleanup EXIT INT TERM

# This must be the operator's known-good 60 fps OBS configuration plus RAWOBS,
# and nothing else.  Left to live.sh defaults it would run vic_fw=5 (tinyvenc5,
# one frame per process), win_seq=0 and poll_drain_ms=20 - a configuration that
# has never delivered continuous encoded video, which would make a negative
# result meaningless.  POSTMASK/FASTKILL/H264DIVISOR are the driver defaults
# already; they are named here so the whole sequence is readable in one place.
echo "Loading the M211 topology (op02/op08 raw banks + live op04 encoded window)..."
VICFW=7 H264PROBE=1 POLLDRAIN=0 WINSEQ=1 OP6=1 POSTMASK=0 FASTKILL=0 \
	H264DIVISOR=0 PERSIST=1 RAWOBS=1 NOSG=0 ./mz0380-live.sh load || exit 1

NODE=""
for name_file in /sys/class/video4linux/video*/name; do
	[ -r "$name_file" ] || continue
	# M240 renamed the node; accept the old name so this script works
	# against a module built before or after that change.
	case "$(cat "$name_file")" in
	"HD60 Pro HDMI capture"|"mz0380 H.264") ;;
	*) continue ;;
	esac
	node=${name_file%/name}
	NODE="/dev/${node##*/}"
	break
done
[ -n "$NODE" ] || { echo "M211 video node not found"; exit 1; }

echo "Capturing H.264 from $NODE for up to ${SECONDS_LIVE}s / ${FRAMES} frames..."
v4l2-ctl -d "$NODE" --stream-mmap=4 --stream-count="$FRAMES" \
	--stream-to="$RAW_OUT" >"$CAPTURE_LOG" 2>&1 &
CAPTURE_PID=$!

DEADLINE=$(( $(date +%s%3N) + SECONDS_LIVE * 1000 ))
while kill -0 "$CAPTURE_PID" 2>/dev/null; do
	[ "$(date +%s%3N)" -ge "$DEADLINE" ] && break
	sleep 0.05
done
reap_capture

# Unload now: the poison read-back runs in the stop path.
if [ "$KEEPLOADED" != 1 ]; then
	timeout 30 ./mz0380-live.sh unload >/dev/null 2>&1 || true
fi
save_evidence

echo
echo "M211 result:"
grep -E 'stop raw bank' "$DMESG_OUT" | sed 's/^.*mz0380\[0\]: /  /' | \
	sed 's/, head=.*//'

# M212: did those sixteen bytes move while the scene did?  Distinct head values
# across the run are what separates "source pixels" from "buffer residue".
HEAD_CHANGES=$(grep -c 'M212 raw slot' "$DMESG_OUT")
DISTINCT_HEADS=$(grep 'M212 raw slot' "$DMESG_OUT" | sed 's/.*: //' | sort -u | wc -l)
if [ "$HEAD_CHANGES" -gt 0 ]; then
	echo
	echo "  --- M212 head samples (first/last few) ---"
	grep 'M212 raw slot' "$DMESG_OUT" | sed 's/^.*mz0380\[0\]: /  /' | head -4
	grep 'M212 raw slot' "$DMESG_OUT" | sed 's/^.*mz0380\[0\]: /  /' | tail -4
fi

SETVIC_COUNT=$(grep -c 'stream start: SET_VIC(' "$DMESG_OUT")
H264_FRAMES=$(sed -n 's/.*H.264 V4L2 totals: \([0-9]\+\) delivered.*/\1/p' \
	"$DMESG_OUT" | tail -1)
: "${H264_FRAMES:=0}"
TOUCHED=$(grep -c 'stop raw bank.* extent=0x[1-9a-f]' "$DMESG_OUT")

echo "  SET_VIC count    : $SETVIC_COUNT (expected: 1)"
echo "  H.264 delivered  : $H264_FRAMES (the encoded path must actually run)"
echo "  raw slots written: $TOUCHED of 8"
echo "  head changes     : $HEAD_CHANGES ($DISTINCT_HEADS distinct values)"
echo "  encoded capture  : $RAW_OUT ($(stat -c %s "$RAW_OUT") bytes)"
echo "  kernel log       : $DMESG_OUT"

if [ "$H264_FRAMES" -eq 0 ]; then
	echo "INCONCLUSIVE: the encoded path delivered nothing, so the raw banks were"
	echo "never given a chance.  Fix the encoded capture first - this is the same"
	echo "configuration that delivers 60 fps in OBS."
	exit 1
fi

if [ "$TOUCHED" -gt 0 ]; then
	echo "POSITIVE: the op02/op08 raw banks receive DMA while the encoded pipeline"
	echo "runs.  Record the extents and tokens; the raw surface is a by-product of"
	echo "the configured encoder, not an independent capture path."
	if [ "$HEAD_CHANGES" -gt 0 ]; then
		echo
		echo "The sixteen bytes MOVED during the run ($DISTINCT_HEADS distinct"
		echo "values).  If the scene was changing, they are source pixels and the"
		echo "bug is a DMA that dies after one burst - compare them against"
		echo "$RAW_OUT, which is what the camera saw."
	else
		echo
		echo "The sixteen bytes never changed.  Either the scene was static or"
		echo "they are not pixels; re-run over a deliberately changing scene"
		echo "before concluding either way."
	fi
	exit 0
fi

echo "NEGATIVE: $H264_FRAMES encoded frames were delivered and all eight raw slots"
echo "are still pristine.  The op02/op08 ring is not the Linux raw source; the"
echo "next step is static, in the Windows binary, not another start."
exit 1
