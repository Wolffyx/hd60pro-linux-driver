#!/bin/bash
# Bounded M209 op02/op08 raw-bank discriminator.
#
# This is intentionally a one-shot hardware experiment.  It loads the driver
# in raw-only mode, queues eight V4L2 I420 buffers, and stops at the earlier of
# eight exact frames or two seconds after START_STREAMING.  It never enables
# h264_probe, and it treats any op04 registration or second SET_VIC as failure.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
command -v v4l2-ctl >/dev/null || { echo "v4l2-ctl is required"; exit 1; }

KEEPLOADED=${KEEPLOADED:-0}
CAPTURE_LOG=$(mktemp /tmp/mz0380-m209-v4l2.XXXXXX.log) || exit 1
RAW_OUT=$(mktemp /tmp/mz0380-m209.XXXXXX.i420) || exit 1
DMESG_OUT=/tmp/mz0380-m209-dmesg.txt
CAPTURE_PID=""

# The first M209 run wedged the harness itself: v4l2-ctl sat in an
# uninterruptible driver wait, `kill -INT` did nothing, and the plain `wait`
# blocked the script forever.  A one-shot hardware experiment must never lose
# its evidence to that, so reaping is bounded and escalates INT -> TERM -> KILL.
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
		     "in the driver (D state).  The module cannot be unloaded until it" \
		     "exits; a card power cycle may be required." >&2
	else
		wait "$CAPTURE_PID" 2>/dev/null || true
	fi
	CAPTURE_PID=""
}

# Evidence first, teardown second: a stuck capture must not cost the log.
save_evidence() {
	dmesg > "$DMESG_OUT" 2>/dev/null || true
	chmod 644 "$DMESG_OUT" "$CAPTURE_LOG" "$RAW_OUT" 2>/dev/null || true
}

cleanup() {
	reap_capture
	save_evidence
	if [ "$KEEPLOADED" != 1 ]; then
		timeout 30 ./mz0380-live.sh unload >/dev/null 2>&1 || \
			echo "WARNING: unload did not complete within 30s" >&2
	fi
}
trap cleanup EXIT INT TERM

echo "Loading the isolated M209 topology (no stream is started by load)..."
H264PROBE=0 RAWBANKS=1 POLLDRAIN=0 VICFW=7 WINSEQ=1 WINBUFS=1 \
	OP6=0 PERSIST=0 NOSG=0 ./mz0380-live.sh load || exit 1

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
[ -n "$NODE" ] || { echo "M209 video node not found"; exit 1; }

echo "Capturing from $NODE; deadline starts when the sole op06 is logged."
v4l2-ctl -d "$NODE" --set-fmt-video=width=1920,height=1080,pixelformat=YU12 \
	--stream-mmap=8 --stream-count=8 --stream-to="$RAW_OUT" \
	>"$CAPTURE_LOG" 2>&1 &
CAPTURE_PID=$!

# STREAMON includes the Windows-order 1.9 s settle.  Wait for the actual kick,
# then enforce the requested two-second live window rather than charging that
# setup time against the discriminator.
START_DEADLINE=$(( $(date +%s%3N) + 7000 ))
while ! dmesg | grep -q 'stream start: START_STREAMING(op 0x06) fired'; do
	if ! kill -0 "$CAPTURE_PID" 2>/dev/null; then
		break
	fi
	if [ "$(date +%s%3N)" -ge "$START_DEADLINE" ]; then
		break
	fi
	sleep 0.02
done

if dmesg | grep -q 'stream start: START_STREAMING(op 0x06) fired'; then
	LIVE_DEADLINE=$(( $(date +%s%3N) + 2000 ))
	while kill -0 "$CAPTURE_PID" 2>/dev/null; do
		dmesg | grep -q 'M209 raw discriminator SUCCESS' && break
		[ "$(date +%s%3N)" -ge "$LIVE_DEADLINE" ] && break
		sleep 0.02
	done
fi

reap_capture
save_evidence

SETVIC_COUNT=$(dmesg | grep -c 'stream start: SET_VIC(')
OP04_COUNT=$(dmesg | grep -c 'H.264 probe registered dedicated window1 ring')
SUCCESS=0
if dmesg | grep -q 'M209 raw discriminator SUCCESS' &&
   [ "$SETVIC_COUNT" -eq 1 ] && [ "$OP04_COUNT" -eq 0 ]; then
	SUCCESS=1
fi

echo
echo "M209 result:"
dmesg | grep -E 'M209 raw completion|M209 raw discriminator|M209 raw V4L2 totals' | tail -20
echo "  SET_VIC count : $SETVIC_COUNT (required: 1)"
echo "  op04 count    : $OP04_COUNT (required: 0)"
echo "  raw capture   : $RAW_OUT ($(stat -c %s "$RAW_OUT") bytes)"
echo "  v4l2-ctl log  : $CAPTURE_LOG"
echo "  kernel log    : $DMESG_OUT"
echo "  v4l2-ctl says : $(tr '\n' ' ' < "$CAPTURE_LOG")"

if [ "$SUCCESS" -eq 1 ]; then
	echo "PASS: eight consecutive 0x2f7600 writes covered all op02/op08 slots."
	exit 0
fi

echo "INCONCLUSIVE/FAIL: the bounded success oracle was not met; inspect the lines above."
exit 1
