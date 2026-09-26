#!/bin/bash
# M171/M172: reload the module and run v4l2-compliance.
#
# ZERO encoder spawns - this never streams, so it is free to re-run as often as
# you like. That matters: conformance is the one test in this tree that costs
# nothing against a spawn budget of 8-18 per power cycle.
#
# Baseline: 148 tests. M170 was 132/16. M171 fixed the control-range,
# readbuffers and DV-timings-contract defects and took it to 142/6. M172 fixes
# the last six.
#
# M245 added V4L2_CID_DV_RX_POWER_PRESENT (it reports the receiver's lock) and
# cut the input list to the one HDMI connector, so the five POWER_PRESENT
# warnings - one per advertised input - should be gone. It also removed the
# controls that did nothing, so the test count may differ from 148.
set -u
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

. scripts/mz0380-build.sh
MZKO=$(mz0380_resolve_module) || exit 1

rmmod mz0380 2>/dev/null
sleep 1
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
insmod "$MZKO" || { echo "insmod failed"; exit 1; }
sleep 3

NODE=
for n in /sys/class/video4linux/video*/name; do
	[ -r "$n" ] || continue
	# M240 renamed the node to "HD60 Pro HDMI capture"; older builds
	# called it mz0380. Match either, or this finds nothing.
	case "$(cat "$n")" in
	mz0380*|"HD60 Pro HDMI capture"*) NODE=/dev/$(basename "$(dirname "$n")");;
	esac
done
[ -n "$NODE" ] || { echo "no mz0380 video node"; exit 1; }

OUT=$(mktemp /tmp/mz0380-compliance.XXXXXX)
v4l2-compliance -d "$NODE" > "$OUT" 2>&1
# Written by root, read by whoever debugs the failures afterwards.
chmod 644 "$OUT"

echo "=== failures ==="
grep -E "fail:|FAIL" "$OUT" | sed 's/^[[:space:]]*//' | sort | uniq -c | sort -rn
echo
echo "=== warnings ==="
grep -E "warn:" "$OUT" | sed 's/^[[:space:]]*//' | sed 's/for input [0-9]*//' | sort | uniq -c
echo
grep "^Total" "$OUT"

# A failure line on its own does not say which ioctl call produced it. Print
# the test block each failure sits in, which does.
if grep -q "fail:" "$OUT"; then
	echo
	echo "=== context around each failure ==="
	grep -n -B8 "fail:" "$OUT" | sed 's/^/  /'
fi
echo "(full output: $OUT)"
