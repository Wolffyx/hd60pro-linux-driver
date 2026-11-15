#!/bin/bash
# Zero-spawn source check: is the HDMI source actually 1920x1080p60 right now?
#
# Answer "what is the source sending" before spending a capture on it. Safe to
# run as often as you like: QUERY_DV_TIMINGS measures the receiver live while
# no pipeline runs, and no SET_VIC is sent, so the spawn budget is untouched.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
command -v v4l2-ctl >/dev/null || { echo "v4l2-ctl is required"; exit 1; }

WANT_W=${WANT_W:-1920}
WANT_H=${WANT_H:-1080}
WANT_FPS=${WANT_FPS:-60}
KEEPLOADED=${KEEPLOADED:-0}
LOADED=0

cleanup() {
	[ "$LOADED" = 1 ] && [ "$KEEPLOADED" != 1 ] && \
		timeout 30 ./mz0380-live.sh unload >/dev/null 2>&1
	return 0
}
trap cleanup EXIT INT TERM

if [ -e /dev/video0 ] && grep -q '^mz0380 ' /proc/modules 2>/dev/null; then
	echo "using the already-loaded module"
else
	# Plain load: no probe knobs, nothing armed, nothing started.
	./mz0380-live.sh load >/dev/null 2>&1 || { echo "load failed"; exit 1; }
	LOADED=1
fi

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
[ -n "$NODE" ] || { echo "video node not found"; exit 1; }

echo "--- QUERY_DV_TIMINGS on $NODE (live receiver read, no SET_VIC) ---"
Q=$(v4l2-ctl -d "$NODE" --query-dv-timings 2>&1)
printf '%s\n' "$Q" | sed 's/^/  /'

W=$(printf '%s\n' "$Q" | sed -n 's/^[[:space:]]*Active width: \([0-9]*\).*/\1/p')
H=$(printf '%s\n' "$Q" | sed -n 's/^[[:space:]]*Active height: \([0-9]*\).*/\1/p')
FPS=$(printf '%s\n' "$Q" | sed -n 's/.*(\([0-9]*\)\.[0-9]* frames per second).*/\1/p')
IL=$(printf '%s\n' "$Q" | grep -qi 'interlaced' && echo i || echo p)

echo
if [ -z "$W" ] || [ -z "$FPS" ]; then
	echo "NO MEASUREMENT: the receiver reported no lock at all."
	echo "Check the cable, the source's power state, and that the source is"
	echo "not asleep."
	exit 1
fi

echo "measured: ${W}x${H}${IL} at ${FPS} fps"
if [ "$W" = "$WANT_W" ] && [ "$H" = "$WANT_H" ] && [ "$IL" = p ] && [ "$FPS" = "$WANT_FPS" ]; then
	echo "PASS: the source is ${WANT_W}x${WANT_H}p${WANT_FPS}."
	exit 0
fi

echo "FAIL: expected ${WANT_W}x${WANT_H}p${WANT_FPS}. Change the source's output"
echo "mode (or set WANT_W/WANT_H/WANT_FPS) and re-run."
exit 1
