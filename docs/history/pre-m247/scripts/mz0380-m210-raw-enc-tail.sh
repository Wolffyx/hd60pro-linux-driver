#!/bin/bash
# Bounded M210 discriminator: the M209 sink topology WITH the Windows encoder
# tail.
#
# M209 ran the whole raw-only sequence and the card never wrote a frame token:
# irq_total=8, frame_events=0, the BAR0 sentinel intact, all eight slots still
# poisoned.  op 0x06 alone does not start this firmware's producer.  Raw-only
# differed from the working 60 fps H.264 start in two ways - no op04 sink and
# no encoder tail - and only the tail can explain a dead producer.
#
# This run changes exactly that one variable: identical eight-slot op02/op08
# topology, still no op04, plus SET_ENC_PARAMS x2 and SET_PREVIEW_PARAMS sent
# byte for byte as the H.264 path sends them.  Windows does not send op 0x06
# when the tail is present, so neither does this; the deadline therefore starts
# at SET_PREVIEW_PARAMS instead.
#
# One-shot: it stops at the earlier of eight exact frames or two seconds after
# the tail completes, never enables h264_probe, and treats any op04
# registration or second SET_VIC as failure.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
command -v v4l2-ctl >/dev/null || { echo "v4l2-ctl is required"; exit 1; }

# OP6=1 additionally fires START_STREAMING(0x06) after the tail.  Windows does
# not, but the operator's known-good 60 fps configuration does, and the first
# M210 run omitted it - so OP6=1 is what makes this run "the working sequence
# minus op04" rather than "minus op04 and op06".
#
# FPS30=1 permits the run on a progressive 1080p30 source.  The 0x2f7600
# extent oracle is geometry-only and stays valid there, but it is a deviation
# from the Windows-confirmed 1080p60 and the driver logs a warning for it, so
# the run stays readable as a 30 Hz run afterwards.
KEEPLOADED=${KEEPLOADED:-0}
CAPTURE_LOG=$(mktemp /tmp/mz0380-m210-v4l2.XXXXXX.log) || exit 1
RAW_OUT=$(mktemp /tmp/mz0380-m210.XXXXXX.i420) || exit 1
DMESG_OUT=/tmp/mz0380-m210-dmesg.txt
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

if [ "${FPS30:-0}" = 1 ]; then
	echo "NOTE: FPS30=1 - this run will accept a 1080p30 source, which is a"
	echo "      deviation from the Windows-confirmed 1080p60.  Score it as 30 Hz."
fi
echo "Loading the M210 topology (raw sinks + encoder tail; load starts nothing)..."
# Every knob below is the operator's known-good 60 fps OBS configuration,
# minus h264_probe and op04.  Anything left to a live.sh default would silently
# differ from it: vic_fw defaults to 5 (tinyvenc5, one frame per process),
# win_seq to 0, and poll_drain_ms to 20.
H264PROBE=0 RAWBANKS=1 RAWTAIL=1 RAW30="${FPS30:-0}" \
	VICFW=7 WINSEQ=1 WINBUFS=1 POLLDRAIN=0 POSTMASK=0 FASTKILL=0 \
	OP6="${OP6:-0}" PERSIST=0 NOSG=0 ./mz0380-live.sh load || exit 1

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
[ -n "$NODE" ] || { echo "M210 video node not found"; exit 1; }

echo "Capturing from $NODE; deadline starts when SET_PREVIEW_PARAMS is logged."
v4l2-ctl -d "$NODE" --set-fmt-video=width=1920,height=1080,pixelformat=YU12 \
	--stream-mmap=8 --stream-count=8 --stream-to="$RAW_OUT" \
	>"$CAPTURE_LOG" 2>&1 &
CAPTURE_PID=$!

# STREAMON includes the Windows-order 1.9 s settle.  Wait for the tail, which
# is the kick on this path, then enforce the requested two-second live window
# rather than charging that setup time against the discriminator.
START_MARK='stream start: SET_PREVIEW_PARAMS'
START_DEADLINE=$(( $(date +%s%3N) + 7000 ))
while ! dmesg | grep -q "$START_MARK"; do
	if ! kill -0 "$CAPTURE_PID" 2>/dev/null; then
		break
	fi
	if [ "$(date +%s%3N)" -ge "$START_DEADLINE" ]; then
		break
	fi
	sleep 0.02
done

if dmesg | grep -q "$START_MARK"; then
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
ENC_COUNT=$(dmesg | grep -c 'stream start: SET_ENC_PARAMS(')
POST_COUNT=$(dmesg | grep -c 'stream start: SET_PREVIEW_PARAMS(')
OP06_COUNT=$(dmesg | grep -c 'stream start: START_STREAMING(op 0x06) fired')
# OP6=1 reproduces the operator's working sequence, which sends op06 AFTER the
# tail even though Windows does not.  M210's first run suppressed it, so that
# run differed from the working path by op06 as well as op04 and cannot isolate
# op04 on its own.
EXPECT_OP06=${OP6:-0}
SUCCESS=0
if dmesg | grep -q 'M209 raw discriminator SUCCESS' &&
   [ "$SETVIC_COUNT" -eq 1 ] && [ "$OP04_COUNT" -eq 0 ] &&
   [ "$ENC_COUNT" -eq 2 ] && [ "$POST_COUNT" -eq 1 ] &&
   [ "$OP06_COUNT" -eq "$EXPECT_OP06" ]; then
	SUCCESS=1
fi

echo
echo "M210 result:"
dmesg | grep -E 'M210:|M209 raw completion|M209 raw discriminator|M209 raw V4L2 totals' | tail -24
echo "  SET_VIC count : $SETVIC_COUNT (required: 1)"
echo "  op04 count    : $OP04_COUNT (required: 0)"
echo "  SET_ENC count : $ENC_COUNT (required: 2)"
echo "  POST_PROC cnt : $POST_COUNT (required: 1)"
echo "  op06 count    : $OP06_COUNT (required: $EXPECT_OP06)"
echo "  source rate   : $(dmesg | sed -n 's/.*live input \(1920x1080[pi]@[0-9]*\).*/\1/p' | tail -1)"
echo "  raw capture   : $RAW_OUT ($(stat -c %s "$RAW_OUT") bytes)"
echo "  v4l2-ctl log  : $CAPTURE_LOG"
echo "  kernel log    : $DMESG_OUT"
echo "  v4l2-ctl says : $(tr '\n' ' ' < "$CAPTURE_LOG")"

if [ "$SUCCESS" -eq 1 ]; then
	echo "PASS: with the encoder tail present, eight consecutive 0x2f7600 writes"
	echo "covered all op02/op08 slots.  The raw surface is a product of the"
	echo "configured encoder pipeline; native V4L2 negotiation can be built on it."
	exit 0
fi

echo "INCONCLUSIVE/FAIL: the bounded success oracle was not met; inspect the lines above."
echo "If the tail was sent (SET_ENC=2, POST_PROC=1) and the banks are still"
echo "poisoned with frame_events=0, the producer needs the op04 sink to exist at"
echo "all - see the M210 section in RE_FINDINGS.md for the run after this one."
exit 1
