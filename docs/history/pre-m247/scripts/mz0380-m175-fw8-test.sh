#!/bin/bash
# M175: the third encoder binary. Nobody has ever run it.
#
# NEXT_SESSION_START says continuous streaming is closed because both routes
# are blocked:
#   fw=5  tinyvenc5 - has the pixels, livelocks after one frame (M153/M166)
#   fw=7  tinyvenc7 - has the cadence, sends 16 bytes/frame, and the interval
#                     knob is opcode 0x32 which ep.ko does not forward (M165)
#
# There is a third. video_capture_mgr spawns ./tinyvenc8 when SET_VIC byte 6 is
# 8, the driver's vic_fw parameter passes 8 through unclamped, and
# re-dump/fw/yuan_demo_sdi/tinyvenc8 is 447 KB of ARM that this project has
# never disassembled or run. It is mentioned once in RE_FINDINGS, in passing,
# as part of a sentence about byte 6.
#
# Static comparison against tinyvenc5 says it is worth a spawn:
#
#   EncodingGroup::mma_already_start   tinyvenc5: 1 byte   tinyvenc8: 8 bytes
#
# That is the exact variable M153 named as the livelock: read and never written
# in tinyvenc5, so TK_MMA_WaitOneFrameComplete is unreachable, so the DMAC
# profile is never returned and the next push spins forever. A different SIZE
# means a different type - one flag versus eight - which is what per-profile
# state would look like.
#
# And tinyvenc8 calls the MMA functions ONLY from encode_handler. tinyvenc5
# calls them from fake_frame_process too - the standby thread that M129 caught
# painting NOSG_LOGO_Y over working captures.
#
# None of that proves it streams. One spawn answers it, and the answer is one
# of four:
#
#   continuous + whole frames  -> continuous streaming, on this firmware, today
#   continuous + 16 bytes      -> the fw=7 story again
#   one frame + freeze         -> the fw=5 story again
#   nothing at all             -> tinyvenc8 does not run or wants a different ABI
#
# Cost: 1 encoder spawn. Check scripts/mz0380-spawns.sh first - the card wedges
# somewhere in the 8-18 range per power cycle.
set -u
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

FW=${FW:-8}
FRAMES=${FRAMES:-60}
WAIT=${WAIT:-25}
# M175 run 1: tinyvenc8 writes 777600 bytes = a complete 960x540 I420 frame,
# exactly a quarter of the source. The completeness rule expected
# source*3/2 = 3110400 and so waited forever on frames that were already whole.
# EXPECT tells it what a frame is; 0 restores the derive-from-source rule.
EXPECT=${EXPECT:-777600}
WANT=${WANT:-$EXPECT}
CAP=${CAP:-/tmp/cap-m175-fw$FW.raw}

. scripts/mz0380-build.sh
MZKO=$(mz0380_resolve_module) || exit 1

rmmod mz0380 2>/dev/null
sleep 1
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
dmesg -C

echo "=== 1. insmod with vic_fw=$FW ==="
# stall_eos_ms is raised, not disabled: a genuinely stalled stream still ends
# cleanly instead of being SIGTERMed mid-write (the M116 truncation), but a
# slow-but-continuous stream is not cut short and mistaken for a stall.
insmod "$MZKO" vic_fw="$FW" stall_eos_ms=5000 \
	expect_frame_bytes="$EXPECT" || { echo "insmod failed"; exit 1; }
sleep 3
dmesg | grep -a "CMD_INIT\|handshake failed" | tail -2
if dmesg | grep -qa "mailbox is deaf"; then
	echo "CARD IS WEDGED - power-cycle. Nothing below is meaningful."
	exit 1
fi

NODE=
for n in /sys/class/video4linux/video*/name; do
	[ -r "$n" ] || continue
	case "$(cat "$n")" in mz0380*) NODE=/dev/$(basename "$(dirname "$n")");; esac
done
[ -n "$NODE" ] || { echo "no mz0380 video node"; exit 1; }

echo
echo "=== 2. ask for $FRAMES frames, ONE spawn, ${WAIT}s ==="
rm -f "$CAP"
START=$(date +%s)
timeout -s INT --foreground "$WAIT" \
	v4l2-ctl -d "$NODE" --stream-mmap --stream-count="$FRAMES" \
	--stream-to="$CAP"
ELAPSED=$(( $(date +%s) - START ))
SZ=$(stat -c %s "$CAP" 2>/dev/null || echo 0)
WHOLE=$(( SZ / WANT ))
REM=$(( SZ % WANT ))

echo
echo "  ${ELAPSED}s, $SZ bytes = $WHOLE whole ${WANT}-byte frame(s) + $REM"
if [ "$WHOLE" -gt 1 ] && [ "$ELAPSED" -gt 0 ]; then
	echo "  ~$(( WHOLE / ELAPSED )) frames/second"
fi

echo
echo "=== 3. what did the card actually do? ==="
DELIV=$(dmesg | sed -n 's/.*poll-drain stopped after \([0-9]*\) deliveries.*/\1/p' | tail -1)
EVENTS=$(dmesg | sed -n 's/.*frame_events=\([0-9]*\).*/\1/p' | tail -1)
echo "  poll-drain deliveries : ${DELIV:-?}"
echo "  frame_events (IRQ)    : ${EVENTS:-?}"
dmesg | grep -aE "spawn|SET_VIC\(input" | tail -3

echo
echo "=== VERDICT ==="
if [ "$WHOLE" -ge 2 ]; then
	echo "  CONTINUOUS STREAMING with whole frames - $WHOLE frames in ${ELAPSED}s."
	echo "  This is the result the project has been told is unreachable."
	echo
	echo "  The frames are 960x540, NOT the 1920x1080 the node advertises, so"
	echo "  view and score them at their real geometry:"
	echo "    ffplay -f rawvideo -pixel_format yuv420p -video_size 960x540 $CAP"
	echo "    scripts/mz0380-m127-splash.py $CAP 960 540"
	echo "  Check first that they are not all the SAME image - a live scene"
	echo "  should differ frame to frame:"
	echo "    split -b $WANT $CAP /tmp/m175f- && sha256sum /tmp/m175f-* | awk '{print \$1}' | sort -u | wc -l"
elif [ "$WHOLE" = 1 ]; then
	echo "  ONE whole frame, then nothing - tinyvenc8 shares fw=5's bound."
elif [ "$SZ" -gt 0 ]; then
	echo "  $SZ bytes and not one whole ${WANT}-byte frame."
else
	echo "  NOTHING delivered at ${WANT} bytes per frame."
	echo "  Before concluding: grep the dmesg below for 'holds N of' - if N is"
	echo "  a stable number, THAT is the card's real frame size and this run"
	echo "  simply had the wrong EXPECT. Re-run with EXPECT=N."
	echo "  (That is exactly how run 1 read as 'nothing' while the card was"
	echo "   writing complete 960x540 frames.)"
fi

echo
echo "=== spawn budget ==="
scripts/mz0380-spawns.sh commit || true
echo
dmesg | grep -aE "poll-drain|frame token|stream start|stream stop" | tail -12
