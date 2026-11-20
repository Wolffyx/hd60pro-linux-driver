#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# M246: does the card deliver HDMI audio into the op 0x03 window?
#
# Loads with enable_audio=1, which allocates four 4 KiB audio slots, registers
# them with op 0x03 alongside the encoded window, and captures the EVENT bits
# 16..23 + BAR0 0x4c token that ep.ko's store_channel_done raises for audio.
# Then streams video (the audio side only runs while the card pipeline does)
# and records from the ALSA device at the same time.
#
# COST: one encoder spawn. Needs a source that sends audio over HDMI.
#
# Reading the result:
#   audio events 0                 -> no audio completion ever reached the host.
#                                     The window theory is incomplete; check the
#                                     IOMMU fault lines below for where the card
#                                     tried to write instead.
#   events > 0, bytes 0, empty > 0 -> completions arrive but nothing lands in
#                                     the slots: the target is off (offset).
#   events > 0, bytes > 0          -> PCM arrives. max extent should read 4096;
#                                     the WAV analysis says whether it is sound.
#   overrun > 0                    -> the card writes more than 4 KiB per slot.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
for t in v4l2-ctl arecord python3; do
	command -v $t >/dev/null || { echo "$t is required"; exit 1; }
done

SECS=${SECS:-8}
WAV=/tmp/mz0380-m246.wav

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

# Output goes to a log, and is shown when a step fails - a silent exit 1 is
# what the second run of this test produced.
if lsmod | grep -q '^mz0380'; then
	./mz0380-live.sh unload > /tmp/mz0380-m246-unload.log 2>&1 || {
		echo "unload failed:"; cat /tmp/mz0380-m246-unload.log; exit 1; }
fi
dmesg -C
TRACE=1 EXTRA="enable_audio=1 ${EXTRA:-}" ./mz0380-live.sh load > /tmp/mz0380-m246-load.log 2>&1 || {
	echo "load failed:"; tail -20 /tmp/mz0380-m246-load.log; exit 1; }
sleep 3
NODE=$(node) || { echo "no video node"; exit 1; }

echo "=== setup ==="
dmesg | grep -E "audio window|ALSA card|audio register" | sed 's/^/  /'
if ! arecord -l 2>/dev/null | grep -q "HD60 Pro HDMI"; then
	echo "  no ALSA capture device - the audio window was not allocated"; exit 1
fi
arecord -l 2>/dev/null | grep "HD60 Pro HDMI" | sed 's/^/  /'
scripts/mz0380-node-holders.sh "$NODE"

echo "=== capture: video on $NODE + ${SECS}s of audio (1 spawn) ==="
v4l2-ctl -d "$NODE" --set-fmt-video=width=1920,height=1080,pixelformat=YU12 >/dev/null
timeout $((SECS + 60)) v4l2-ctl -d "$NODE" --stream-mmap \
	--stream-count=$(( (SECS + 30) * 50 )) --stream-to=/dev/null \
	> /tmp/mz0380-m246-video.log 2>&1 &
VPID=$!
# A fresh load spends many seconds in STREAMON (spawn, settle, SET_AIC) and the
# card only starts its capture app at SET_AIC. ALSA gives up after 10 s without
# a period, so recording on a fixed delay timed out before audio existed - the
# third run of this test. Wait for the first audio slot instead.
for i in $(seq 1 60); do
	ev=$(sed -n 's/^  audio      : \([0-9]*\) events.*/\1/p' /proc/mz0380-state)
	[ "${ev:-0}" -gt 0 ] && break
	sleep 0.5
done
echo "  audio flowing after ~$((i / 2)) s (events=${ev:-0})"
timeout $((SECS + 10)) arecord -D hw:CARD=mz0380,DEV=0 -f S16_LE -c 2 -r 48000 \
	-d "$SECS" "$WAV" 2>&1 | sed 's/^/  arecord: /'
wait "$VPID"

echo "=== what was sent ==="
dmesg | grep -E "mbx op=0x(2a|03) " | sed 's/^/  /'

echo "=== driver counters ==="
grep -E "^  audio      :|^  audio alsa :|^  audio pcm  :|^  pixelformat|^  raw frames" /proc/mz0380-state | sed 's/^/ /'

echo "=== IOMMU faults during the run (where the card tried to write) ==="
dmesg | grep -iE "IO_PAGE_FAULT|AMD-Vi|DMAR|iommu.*fault" | tail -8 | sed 's/^/  /' || true
dmesg | grep -iqE "IO_PAGE_FAULT|DMAR.*fault" || echo "  none"

echo "=== WAV analysis ==="
python3 - "$WAV" <<'EOF'
import sys, wave, array, math
try:
    w = wave.open(sys.argv[1])
except Exception as e:
    print("  no WAV:", e); sys.exit(0)
n = w.getnframes(); ch = w.getnchannels()
a = array.array('h', w.readframes(n))
print(f"  {n} frames, {ch} ch, {w.getframerate()} Hz, {n / max(w.getframerate(),1):.2f} s")
if not a:
    print("  VERDICT: no samples at all - nothing was pushed to ALSA"); sys.exit(0)
zeros = sum(1 for x in a if x == 0) / len(a)
poison = sum(1 for x in a if x == -15421) / len(a)   # 0xc3c3, the slot poison
rms = math.sqrt(sum(x * x for x in a) / len(a))
peak = max(abs(x) for x in a)
print(f"  rms {rms:.1f}  peak {peak}  zero samples {zeros:.1%}  poison samples {poison:.1%}")
if poison > 0.01:
    print("  VERDICT: slot poison reached ALSA - the extent measurement or the slot choice is wrong")
elif zeros > 0.99:
    print("  VERDICT: digital silence - PCM path works end to end only if the source is silent; play sound and retry")
elif rms > 20:
    print("  VERDICT: real signal. Listen to it: " + sys.argv[1])
else:
    print("  VERDICT: near-silent but not zero - listen before concluding")
if rms <= 20 or ch != 2:
    sys.exit(0)

# Byte order: real audio is dominated by low frequencies, so neighbouring
# samples are close (step/level well under 1). Byte-swapped samples look like
# white noise (~1.4). Whichever reading is smoother is the true byte order.
def roughness(s):
    lvl = math.sqrt(sum(x * x for x in s) / len(s)) or 1
    return math.sqrt(sum((s[i] - s[i - 1]) ** 2 for i in range(1, len(s))) / len(s)) / lvl
left, right = a[0::2], a[1::2]
sw = array.array('h', a); sw.byteswap()
loud = 0 if sum(x * x for x in left) >= sum(x * x for x in right) else 1
r_as, r_sw = roughness(a[loud::2]), roughness(sw[loud::2])
print(f"  byte order: roughness as delivered {r_as:.2f}, byte-swapped {r_sw:.2f}")
print("  VERDICT: byte order " + ("OK (little-endian as labelled)" if r_as < r_sw
      else "WRONG - the card delivers big-endian; swap in mz0380_audio_push()"))

# Channel order: per-second level of each channel. Play a left/right test
# (left only, then right only) and the active column must follow the source.
def level(s):
    return math.sqrt(sum(x * x for x in s) / max(len(s), 1))
rate = w.getframerate()
print("  second   ch0(L)    ch1(R)")
for t in range(n // rate):
    print(f"  {t:4d}  {level(left[t*rate:(t+1)*rate]):8.0f}  {level(right[t*rate:(t+1)*rate]):8.0f}")
EOF

scripts/mz0380-spawns.sh commit || true
chmod 644 "$WAV" 2>/dev/null || true
