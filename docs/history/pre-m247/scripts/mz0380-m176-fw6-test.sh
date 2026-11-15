#!/bin/bash
# M176: fw=6 - the value Windows actually sends, never tested as a single
# variable.
#
# The question this answers: how does the Windows driver get continuous capture
# out of this card?
#
# M82 read it out of every [CH00] line of every Windows trace: the Windows
# driver sends SET_VIC byte 6 as **6 or 7, picked by frame rate** - 6 for
# 1080p30 and 1080p29.97, 7 for 1080p60. It NEVER sends 5. Our driver has sent
# 5 for its entire history, and 5 is the default to this day.
#
# And fw=6 does NOT select a different binary. Only 7 -> tinyvenc7 and
# 8 -> tinyvenc8; 6 falls through to **tinyvenc5, the same binary we already
# run**. What it changes is the card's capture config: video_capture_mgr writes
# "output format" = 2/YUY2 when fw == 6, and 1/YV12 otherwise (M79).
#
# So Windows drives tinyvenc5 - the binary with the pixel path and the
# livelock - in a mode this project has never once put it in.
#
# The one run that tried fw=6 (RE_FINDINGS ~3486) was explicitly
# retro-invalidated at ~3536: it carried out_fmt=0 and other changes, it
# predates M129 (real video), fake_frame_off, post_mask=0 and the poll-drain,
# and it ran on INTx. RE_FINDINGS' own "what remains genuinely untested" list
# has it at **number 1**.
#
# Why it could have been missed even if it works: YUY2 at 1080p is
# 1920*1080*2 = 4147200 bytes, not the 3110400 of I420. Both of the driver's
# size rules would have discarded such a frame in silence -
#   - the completeness rule waits for source*3/2 and never sees it (M115), and
#   - the vb2 plane is 3110400, so "does not fit vb2 plane; buffer failed".
# That is exactly how M175's first run scored a working tinyvenc8 as "nothing
# delivered". expect_frame_bytes fixes both: it sets what a frame is AND sizes
# the plane to hold it.
#
# Cost: 1 encoder spawn. Check scripts/mz0380-spawns.sh first.
set -u
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

FW=${FW:-6}
# 1080p YUY2. Overridden by whatever the card turns out to write - see the
# "holds N of" advice in the verdict.
EXPECT=${EXPECT:-4147200}
FRAMES=${FRAMES:-60}
WAIT=${WAIT:-25}
CAP=${CAP:-/tmp/cap-m176-fw$FW.raw}

. scripts/mz0380-build.sh
MZKO=$(mz0380_resolve_module) || exit 1

rmmod mz0380 2>/dev/null
sleep 1
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
dmesg -C

echo "=== 1. insmod vic_fw=$FW expect_frame_bytes=$EXPECT ==="
insmod "$MZKO" vic_fw="$FW" expect_frame_bytes="$EXPECT" stall_eos_ms=5000 \
	|| { echo "insmod failed"; exit 1; }
sleep 3
dmesg | grep -a "CMD_INIT\|handshake failed" | tail -2
dmesg | grep -qa "mailbox is deaf" && { echo "CARD WEDGED - power-cycle"; exit 1; }

NODE=
for n in /sys/class/video4linux/video*/name; do
	[ -r "$n" ] || continue
	case "$(cat "$n")" in mz0380*) NODE=/dev/$(basename "$(dirname "$n")");; esac
done
[ -n "$NODE" ] || { echo "no mz0380 video node"; exit 1; }
echo "  plane size: $(v4l2-ctl -d "$NODE" --get-fmt-video | sed -n 's/.*Size Image *: *//p')"

echo
echo "=== 2. $FRAMES frames, ONE spawn, ${WAIT}s ==="
rm -f "$CAP"
START=$(date +%s)
timeout -s INT --foreground "$WAIT" \
	v4l2-ctl -d "$NODE" --stream-mmap --stream-count="$FRAMES" \
	--stream-to="$CAP"
ELAPSED=$(( $(date +%s) - START ))
SZ=$(stat -c %s "$CAP" 2>/dev/null || echo 0)
WHOLE=$(( SZ / EXPECT ))

echo
echo "  ${ELAPSED}s, $SZ bytes = $WHOLE whole ${EXPECT}-byte frame(s)"
[ "$WHOLE" -gt 1 ] && [ "$ELAPSED" -gt 0 ] && echo "  ~$(( WHOLE / ELAPSED )) frames/second"

echo
echo "=== 3. what the card wrote ==="
# THE line to read. A stable N here is the card's real frame size, whatever we
# guessed. This is how M175 run 1 hid a working encoder behind a wrong EXPECT.
dmesg | grep -aoE "holds [0-9]+ of [0-9]+" | sort | uniq -c | sort -rn | head -5
echo "  deliveries : $(dmesg | sed -n 's/.*poll-drain stopped after \([0-9]*\) deliveries.*/\1/p' | tail -1)"
echo "  frame_events: $(dmesg | sed -n 's/.*frame_events=\([0-9]*\).*/\1/p' | tail -1)"
dmesg | grep -a "does not fit vb2 plane" | tail -2

echo
echo "=== VERDICT ==="
if [ "$WHOLE" -ge 2 ]; then
	echo "  CONTINUOUS - $WHOLE frames in ${ELAPSED}s from fw=$FW."
	echo "  This is what Windows sends, and it would be the answer."
	echo "  fw=6 delivers PLANAR 4:2:2 (I422), not packed YUY2 - the cfg"
	echo "  label says YUY2 and M176 measured otherwise:"
	echo "    ffplay -f rawvideo -pixel_format yuv422p -video_size 1920x1080 $CAP"
	echo "  Confirm the frames differ (a live scene must):"
	echo "    split -b $EXPECT $CAP /tmp/m176f- && sha256sum /tmp/m176f-* | awk '{print \$1}' | sort -u | wc -l"
elif [ "$WHOLE" = 1 ]; then
	echo "  ONE frame, then nothing - fw=6 shares the livelock. That would mean"
	echo "  the output format is not what frees tinyvenc5, and the Windows"
	echo "  difference lies elsewhere."
else
	echo "  No whole ${EXPECT}-byte frame."
	echo "  READ THE 'holds N of' LINE ABOVE FIRST. A stable N is the card's"
	echo "  real frame size and this run simply guessed wrong - re-run with"
	echo "  EXPECT=N. Only if N never appears at all did nothing arrive."
fi

echo
echo "=== spawn budget ==="
scripts/mz0380-spawns.sh commit || true
echo
dmesg | grep -aE "poll-drain|frame token|stream start: SET_VIC|stream stop" | tail -10
