#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# M252: where do RGB black and white land, for each RGB range the source can
# declare? And, from the same first capture, the M251 delivery rhythm.
#
# The MST3367 table the driver inherited from hdcapm is a full-swing BT.709
# RGB->YCbCr matrix with a +10 luma offset (decoded in M252): RGB 0 -> Y 10,
# RGB 255 -> 264, clipped to 255. The node is labelled limited range, where
# black is 16 and white 235. What the right table is depends on what the
# source sends and says it sends (AVI InfoFrame Q1:Q0, BANK2 0x4a).
#
# Needs the GPU's HDMI output looped into the card (KDE output HDMI-A-1). The
# script runs everything itself, as you, in your desktop session: a full-screen
# mpv pattern - left half black, right half white - and kscreen-doctor to set
# that output's RGB range to automatic, full, limited, then back to automatic.
#
# Predictions from the M252 decode (current table):
#   full    : black Y ~10 (11 if the source dithers to 1), white Y 255 (clipped)
#   limited : black Y ~26, white Y ~244
#   chroma  : 128 on both halves, every phase
#   0x4a    : bits 3:2 = 10 for full, 01 for limited if the GPU signals it
#
# Cost: ONE encoder spawn (the first capture; the next two reattach).
#
#   sudo scripts/mz0380-m252-range-test.sh
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root (sudo, from your desktop session)"; exit 1; }
U=${SUDO_USER:-}
[ -n "$U" ] || { echo "run it with sudo from your desktop session"; exit 1; }
UID_=$(id -u "$U")
# The session's Wayland socket is not always wayland-0 (it was wayland-1 on the
# day M256 was written), and kscreen-doctor fails silently on the wrong one -
# which reads as "no such output". Take the socket that exists.
WAYLAND=${WAYLAND:-$(basename "$(ls "/run/user/$UID_"/wayland-[0-9]* 2>/dev/null | grep -v '\.lock$' | head -1)")}
[ -n "$WAYLAND" ] || { echo "no Wayland socket in /run/user/$UID_ - run this from your desktop session"; exit 1; }

OUT=${OUT:-/tmp/mz0380-m252-range}
OUTPUT=${OUTPUT:-HDMI-A-1}
FRAMES=${FRAMES:-120}
RHYTHM_FRAMES=${RHYTHM_FRAMES:-900}
mkdir -p "$OUT"
MPV=

as_user() {
	sudo -u "$U" env XDG_RUNTIME_DIR="/run/user/$UID_" \
		DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$UID_/bus" \
		WAYLAND_DISPLAY="$WAYLAND" "$@"
}
node() {
	local n
	for n in /sys/class/video4linux/video*/name; do
		[ "$(cat "$n" 2>/dev/null)" = "HD60 Pro HDMI capture" ] || continue
		n=${n%/name}
		echo "/dev/${n##*/}"
		return 0
	done
	return 1
}
state() { grep -E "^  ($1)" /proc/mz0380-state 2>/dev/null; }
locked() {
	local i
	for i in $(seq 30); do
		timeout 10 v4l2-ctl -d "$N" --query-dv-timings 2>/dev/null |
			grep -q "Active width" && return 0
		sleep 0.5
	done
	return 1
}
cleanup() {
	[ -n "$MPV" ] && kill "$MPV" 2>/dev/null
	as_user kscreen-doctor "output.$OUTPUT.rgbrange.automatic" > /dev/null 2>&1
	./mz0380-live.sh unload > /dev/null 2>&1
}

as_user kscreen-doctor -o 2>/dev/null | grep -q " $OUTPUT " ||
	{ echo "no KDE output $OUTPUT - loop the GPU's HDMI into the card first"; exit 1; }
pgrep -x obs > /dev/null && { echo "OBS is running - close it first"; exit 1; }
scripts/mz0380-spawns.sh 2>/dev/null | tail -2
trap cleanup EXIT

./mz0380-live.sh load > "$OUT/load.log" 2>&1 || { echo "load failed - see $OUT/load.log"; exit 1; }
if dmesg | grep -q -E "card handshake failed|CMD_INIT got no answer"; then
	echo "!! card handshake failed - the card is wedged; remove its power (PSU switch, 30 s)"
	exit 1
fi
N=$(node) || { echo "no node"; exit 1; }

cat > "$OUT/scan.py" <<'EOF'
import sys
import numpy as np
W, H = 1920, 1080
L = W * H; C = L // 4; F = L + 2 * C
buf = bytearray(F); mv = memoryview(buf)
out = open(sys.argv[1], "w")
while True:
    got = 0
    while got < F:
        r = sys.stdin.buffer.readinto(mv[got:])
        if not r:
            break
        got += r
    if got < F:
        break
    a = np.frombuffer(buf, np.uint8)
    y = a[:L].reshape(H, W); u = a[L:L + C].reshape(H // 2, W // 2); v = a[L + C:].reshape(H // 2, W // 2)
    row = []
    for x0, x1 in ((200, 760), (1160, 1720)):      # left = black, right = white
        row += [y[200:880, x0:x1].mean(), u[100:440, x0 // 2:x1 // 2].mean(), v[100:440, x0 // 2:x1 // 2].mean()]
    out.write(" ".join(f"{x:.2f}" for x in row) + "\n")
EOF

as_user mpv --fs --fs-screen-name="$OUTPUT" --osc=no --osd-level=0 --cursor-autohide=always \
	'av://lavfi:color=c=black:s=1920x1080:r=60,drawbox=x=960:y=0:w=960:h=1080:color=white:t=fill' \
	> "$OUT/mpv.log" 2>&1 &
MPV=$!
sleep 3

phase() {	# phase <rgbrange> <frames>
	local range=$1 frames=$2
	echo
	echo "=== RGB range: $range ==="
	as_user kscreen-doctor "output.$OUTPUT.rgbrange.$range" > /dev/null 2>&1 ||
		echo "  !! kscreen-doctor could not set rgbrange.$range"
	sleep 4
	locked || { echo "  !! no lock after the range change"; return; }
	echo csc > /proc/mz0380-hdmi
	state colourspace | sed 's/^/ /'
	timeout 120 v4l2-ctl -d "$N" --set-fmt-video=width=1920,height=1080,pixelformat=YU12 \
		--stream-mmap=4 --stream-count="$frames" --stream-to=- 2> "$OUT/$range-v4l2-ctl.log" |
		python3 -I "$OUT/scan.py" "$OUT/$range.txt"
	python3 -I - "$OUT/$range.txt" <<'EOF'
import sys
rows = [list(map(float, l.split())) for l in open(sys.argv[1])][10:]
if not rows:
    print("  !! no frames"); sys.exit(0)
m = [sum(c) / len(rows) for c in zip(*rows)]
print(f"  {len(rows)} frames - black: Y {m[0]:6.2f} Cb {m[1]:6.2f} Cr {m[2]:6.2f}   "
      f"white: Y {m[3]:6.2f} Cb {m[4]:6.2f} Cr {m[5]:6.2f}")
EOF
}

phase automatic "$RHYTHM_FRAMES"
state 'raw frames|raw repeats|raw rhythm' | sed 's/^/ /'
phase full "$FRAMES"
phase limited "$FRAMES"
echo
state 'enc spawns' | sed 's/^/ /'
echo "  predicted (current table): full -> black ~10-11, white 255; limited -> black ~26, white ~244; chroma 128"
echo "artefacts in $OUT"
