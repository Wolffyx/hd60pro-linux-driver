#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# M247 chroma rule on hardware: does raw capture follow the source through a
# black screen, instead of freezing on the last frame before it?
#
# The driver holds back a raw slot whose V plane still reads the clear value
# 0x80 - the card clears a slot before filling it - but 0x80 is also neutral
# chroma: black, grey, white. M238 held such frames until V changed, so a black
# screen froze the picture. M247 delivers a slot whose V has read clear for a
# whole frame period, and counts it as `colourless frames passed on a second
# look`.
#
# One continuous raw capture: normal content, then black, then normal again.
# Frames are piped into a scanner (no 3 GB file) that records, per frame, mean
# luma and how many of the DRIVER'S OWN 16 V-plane sample dwords read exactly
# 0x80808080 - 16/16 is what takes the neutral path - and when it arrived, so
# a freeze is a gap.
#
#   M247 holds:   frames keep coming through the black at the source rate, and
#                 every 16/16 frame is counted as colourless
#   M238 regime:  16/16 frames never arrive - a gap as long as the black
#   inconclusive: no frame reached 16/16 - the black was not exactly neutral
#                 (a lens cap gives sensor noise of +-1 in chroma, M244). Use a
#                 digital black: a laptop/phone showing a full-screen black
#                 image, or a camera playing back a black picture.
#
# Cost: ONE encoder spawn (the capture). Keep OBS closed.
#
#   sudo scripts/mz0380-m247-black-test.sh
#   sudo SECS=40 BLACK_AT=10 BLACK_FOR=15 scripts/mz0380-m247-black-test.sh
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

OUT=${OUT:-/tmp/mz0380-m247-black}
SECS=${SECS:-30}
BLACK_AT=${BLACK_AT:-8}
BLACK_FOR=${BLACK_FOR:-12}
mkdir -p "$OUT"
CAP=

announce() { printf '%s\n' "$*" > /dev/tty 2>/dev/null || printf '%s\n' "$*" >&2; }
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
# raw fills, milestone tags dropped: luma-clear skips, chroma-clear skips,
# chroma probes, best, of, colourless passed
fills() { state 'raw fills' | sed 's/(M[0-9]*)//g' | grep -oE '[0-9]+' | tr '\n' ' '; }
cleanup() {
	[ -n "$CAP" ] && kill "$CAP" 2>/dev/null
	./mz0380-live.sh unload > /dev/null 2>&1
}
trap cleanup EXIT

scripts/mz0380-spawns.sh 2>/dev/null | tail -2
pgrep -x obs > /dev/null && { echo "OBS is running - close it first (it takes the node and spends spawns)"; exit 1; }

./mz0380-live.sh load > "$OUT/load.log" 2>&1 || { echo "load failed - see $OUT/load.log"; exit 1; }
if dmesg | grep -q -E "card handshake failed|CMD_INIT got no answer"; then
	echo "!! card handshake failed - the card is wedged; remove its power (PSU switch, 30 s)"
	exit 1
fi
N=$(node) || { echo "no node"; exit 1; }

cat > "$OUT/scan.py" <<'EOF'
import sys, time, zlib
W, H = 1920, 1080
L = W * H; C = L // 4; F = L + 2 * C
VBASE = L + C                       # I420: Y, U, V
STEP = (C // 16) & ~3               # mz0380_raw_count_clear(): 16 samples
CLEAR = b"\x80\x80\x80\x80"
buf = bytearray(F); mv = memoryview(buf)
out = open(sys.argv[1], "w"); prev = None; n = 0
while True:
    got = 0
    while got < F:
        r = sys.stdin.buffer.readinto(mv[got:])
        if not r:
            break
        got += r
    if got < F:
        break
    y = buf[0:L:4099]; u = buf[L:VBASE:2003]; v = buf[VBASE:F:2003]
    vclear = sum(buf[VBASE + i * STEP:VBASE + i * STEP + 4] == CLEAR for i in range(16))
    sig = zlib.crc32(buf[0:L:997])
    out.write(f"{n} {time.monotonic():.6f} {sum(y)/len(y):.1f} {sum(u)/len(u):.1f} {sum(v)/len(v):.1f} {vclear} {int(sig == prev)}\n")
    prev = sig; n += 1
EOF

echo "=== locking ==="
for _ in $(seq 20); do
	timeout 10 v4l2-ctl -d "$N" --query-dv-timings 2>/dev/null | grep -q "Active width" && break
	sleep 0.5
done
FPS=$(timeout 10 v4l2-ctl -d "$N" --query-dv-timings 2>/dev/null |
	grep -oE '\([0-9.]+ frames per second' | grep -oE '[0-9.]+' | cut -d. -f1)
[ -n "$FPS" ] || { echo "!! no source locked - check the cable and the source"; exit 1; }
echo "  source at $FPS fps"
B=$(fills); D0=$(state 'raw frames' | grep -oE '^  raw frames : [0-9]+ delivered, [0-9]+' | grep -oE '[0-9]+' | tr '\n' ' ')

announce ""
announce "# capture starts now ($SECS s). Keep NORMAL content on the source."
announce "# At ${BLACK_AT}s you will be told to switch to BLACK, ${BLACK_FOR}s later back."
timeout $((SECS + 30)) v4l2-ctl -d "$N" \
	--set-fmt-video=width=1920,height=1080,pixelformat=YU12 \
	--stream-mmap=4 --stream-count=$((SECS * FPS)) --stream-to=- \
	2> "$OUT/v4l2-ctl.log" | python3 -I "$OUT/scan.py" "$OUT/frames.txt" &
CAP=$!
sleep "$BLACK_AT"
announce "# >>> switch the source to BLACK now <<<"
sleep "$BLACK_FOR"
announce "# >>> back to NORMAL content now <<<"
wait "$CAP"
CAP=

A=$(fills); D1=$(state 'raw frames' | grep -oE '^  raw frames : [0-9]+ delivered, [0-9]+' | grep -oE '[0-9]+' | tr '\n' ' ')
state 'enc spawns|raw frames|raw fills|raw repeats|raw rhythm' | sed 's/^/ /'

echo
echo "=== timeline (1 s buckets by frame arrival) ==="
python3 -I - "$OUT/frames.txt" "$B" "$A" "$D0" "$D1" <<'EOF'
import sys
# frames.txt: index arrival meanY meanU meanV vclear repeat
fr = [l.split() for l in open(sys.argv[1])]
dq = [float(f[1]) for f in fr]
fr = [[f[0]] + f[2:] for f in fr]
n = len(fr)
if not n:
    print("  !! no frames captured"); sys.exit(0)
t0 = dq[0]
print("  sec  frames  meanY   meanV  16/16-neutral  repeats  max gap ms")
gaps = [0.0] + [(dq[i] - dq[i - 1]) * 1000 for i in range(1, n)]
for s in range(int(dq[n - 1] - t0) + 1):
    idx = [i for i in range(n) if int(dq[i] - t0) == s]
    if not idx:
        print(f"  {s:3d}       0   (nothing delivered)"); continue
    y = sum(float(fr[i][1]) for i in idx) / len(idx)
    v = sum(float(fr[i][3]) for i in idx) / len(idx)
    neu = sum(fr[i][4] == "16" for i in idx); rep = sum(fr[i][5] == "1" for i in idx)
    print(f"  {s:3d}  {len(idx):6d}  {y:5.1f}  {v:6.1f}  {neu:13d}  {rep:7d}  {max(gaps[i] for i in idx):10.1f}")
b = [int(x) for x in sys.argv[2].split()]; a = [int(x) for x in sys.argv[3].split()]
d0 = [int(x) for x in sys.argv[4].split()]; d1 = [int(x) for x in sys.argv[5].split()]
neutral = sum(fr[i][4] == "16" for i in range(n))
colourless = a[5] - b[5] if len(a) > 5 and len(b) > 5 else -1
maxgap = max(gaps)
print(f"\n  {n} frames, max gap {maxgap:.1f} ms; {neutral} delivered with all 16 V samples exactly 0x80")
print(f"  driver: +{d1[0]-d0[0]} delivered, +{d1[1]-d0[1]} dropped for lack of a buffer, "
      f"+{a[1]-b[1]} chroma-clear deferrals, +{colourless} colourless passed (M247), best {a[3]}/{a[4]}")
if maxgap > 250:
    print(f"  !! delivery stopped for {maxgap:.0f} ms - a freeze"
          + (" (no neutral frame got through: the M238 regime)" if neutral == 0 else ""))
elif neutral == 0:
    print("  INCONCLUSIVE: no frame was exactly neutral - the black had chroma noise; use a digital black")
if neutral and colourless < neutral:
    print(f"  !! {neutral} exactly-neutral frames delivered but only {colourless} counted as colourless")
elif neutral and maxgap <= 250:
    print("  M247 holds: exactly-neutral frames were delivered at the source rate")
EOF
echo
echo "artefacts in $OUT: frames.txt v4l2-ctl.log"
