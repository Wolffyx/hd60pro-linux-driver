#!/bin/bash
# M170: is this a V4L2 device other people's software can use?
#
# Every capture in this project's history was driven by a script in this tree
# that already knew the answer - the geometry, the format, the frame count. That
# is why M168 found a node advertising the wrong fourcc and refusing to
# enumerate its own frame sizes: nothing had ever asked it anything.
#
# This runs the two tests that ask.
#
#   1. v4l2-compliance, the standard conformance suite. The non-streaming pass
#      exercises the whole ioctl surface and costs ZERO encoder spawns.
#   2. Repeat capture. The card is a single-shot grabber, so the product is a
#      still grabber, and the first thing anyone will do is take a SECOND still.
#      Nothing has ever verified that STREAMOFF/STREAMON yields another good
#      frame - M39 says a fresh spawn delivers one, but no run has taken three
#      in a row and compared them. Costs one spawn per still.
#
# Spawn cost: STILLS (default 3). The card wedges somewhere in the 8-18 range
# per power cycle, so check the budget first:  scripts/mz0380-spawns.sh
set -u
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

STILLS=${STILLS:-3}
WANT=${WANT:-3110400}
OUT=${OUT:-/tmp/m170}
COMPLIANCE=${COMPLIANCE:-1}

. scripts/mz0380-build.sh
MZKO=$(mz0380_resolve_module) || exit 1

rmmod mz0380 2>/dev/null
sleep 1
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
dmesg -C

echo "=== 1. plain insmod ==="
insmod "$MZKO" || { echo "insmod failed"; exit 1; }
sleep 3
dmesg | grep -a "CMD_INIT\|handshake failed" | tail -2
if dmesg | grep -qa "mailbox is deaf"; then
	echo "CARD IS WEDGED - power-cycle. Nothing below is meaningful."
	exit 1
fi

NODE=
for n in /sys/class/video4linux/video*/name; do
	[ -r "$n" ] || continue
	case "$(cat "$n")" in
	mz0380*) NODE=/dev/$(basename "$(dirname "$n")");;
	esac
done
[ -n "$NODE" ] || { echo "no mz0380 video node"; exit 1; }
echo "using $NODE"

echo
echo "=== 2. does the input report whether a source is connected? ==="
# M170: capabilities used to be 0 (so no client would ever ask for DV timings)
# and status used to be 0, which in V4L2 means "no problems" - asserted with the
# cable out.
v4l2-ctl -d "$NODE" --list-inputs | sed -n '1,20p'

echo
echo "=== 3. can SOURCE_CHANGE be subscribed? ==="
# M170: the driver has always had an emitter for this and the ops table pointed
# subscribe_event at the ctrl-only helper, so every client got -EINVAL.
if v4l2-ctl -d "$NODE" --help-all >/dev/null 2>&1; then
	timeout 3 v4l2-ctl -d "$NODE" --wait-for-event=source_change=0 >/dev/null 2>&1
	rc=$?
	# 124 = the timeout fired, which means the SUBSCRIBE succeeded and it sat
	# waiting. A prompt non-zero exit means SUBSCRIBE was refused.
	if [ "$rc" = 124 ]; then
		echo "PASS  SUBSCRIBE_EVENT(SOURCE_CHANGE) accepted (waited, then timed out)"
	else
		echo "FAIL  SUBSCRIBE_EVENT(SOURCE_CHANGE) rejected (rc=$rc)"
	fi
fi

if [ "$COMPLIANCE" = 1 ]; then
	echo
	echo "=== 4. v4l2-compliance, no streaming - ZERO spawns ==="
	if command -v v4l2-compliance >/dev/null; then
		v4l2-compliance -d "$NODE" 2>&1 | tail -40
	else
		echo "(v4l2-compliance not installed - skipping)"
	fi
fi

echo
echo "=== 5. repeat capture: $STILLS stills, one spawn each ==="
rm -f "$OUT"-*.i420
PASSES=0
for i in $(seq "$STILLS"); do
	F="$OUT-$i.i420"
	timeout -s INT --foreground 60 \
		v4l2-ctl -d "$NODE" --stream-mmap --stream-count=1 \
		--stream-to="$F" >/dev/null 2>&1
	SZ=$(stat -c %s "$F" 2>/dev/null || echo 0)
	if [ "$SZ" = "$WANT" ]; then
		SUM=$(sha256sum "$F" | cut -c1-16)
		echo "  still $i: $SZ bytes  sha=$SUM  OK"
		PASSES=$((PASSES + 1))
	else
		echo "  still $i: $SZ bytes  (wanted $WANT)  FAILED"
	fi
	sleep 1
done

echo
echo "  $PASSES of $STILLS stills were whole frames"
if [ "$PASSES" = "$STILLS" ]; then
	echo "  PASS  repeat capture works - the still grabber is usable as one"
elif [ "$PASSES" -gt 0 ]; then
	echo "  PARTIAL  the first still(s) worked and a later one did not."
	echo "  That is the signature to care about: check dmesg for SET_VIC ret=-110"
	echo "  (the card wedging) versus a lost HDMI lock (the source)."
else
	echo "  FAIL  no whole frames at all - see dmesg below"
fi

# Distinct frames? Identical hashes across stills would mean we are re-reading
# one buffer rather than capturing again.
DISTINCT=$(sha256sum "$OUT"-*.i420 2>/dev/null | awk '{print $1}' | sort -u | wc -l)
echo "  $DISTINCT distinct frame(s) among them (1 = the same image every time,"
echo "  which for a live scene would mean the second capture did not happen)"

echo
echo "=== 6. spawn budget ==="
scripts/mz0380-spawns.sh commit || true

echo
echo "=== 7. dmesg ==="
dmesg | grep -aE "SET_VIC|poll-drain|end of stream|frame token|deaf" | tail -20
