#!/bin/bash
# M39: does EVERY tinyvenc5 spawn yield exactly one frame?
#
# M38 proved: one contiguous full-size frame lands <450 ms after START, then
# the card NEVER writes again (3 mid-stream repoisons, zero regrowth). The
# frame pacing in fake_frame_process is TIME-based (clock deltas + usleep),
# so a live loop would free-run - the thread most plausibly ERRORS OUT or
# exits after frame 1 (TK_MMA_Release + return path at 0x16a80).
#
# Cheap discriminator, no reload: streamon/streamoff TWICE on one module
# load. SET_VIC spawns a fresh tinyvenc5 each time (M19), so:
#   - frame lands BOTH times  -> "one frame per spawn": thread dies/exits
#     after its first frame; the per-frame path itself is fine.
#   - frame lands only on the FIRST streamon -> the card is left in a state
#     that even a fresh spawn can't stream from (engine/ring wedged) - the
#     blocker is in vpl_dmac/hardware state, not the thread.
set -u
cd "$(dirname "$0")/.."

POISON=${1:-0xaa}
make || { echo "build failed"; exit 1; }

fuser -k /dev/video0 2>/dev/null; sleep 1
rmmod mz0380 2>/dev/null
if lsmod | grep -q '^mz0380'; then echo "FATAL: still loaded"; exit 1; fi
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm \
	|| { echo "modprobe deps failed"; exit 1; }

dmesg -C
insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
	enable_video=1 procfs_verbosity=2 stream_nosg=1 dma_iova_remap=1 \
	aic_on=1 buf_poison=1 poison_byte="$POISON" \
	|| { echo "insmod failed"; exit 1; }
sleep 3

for round in 1 2; do
	echo "=== streamon round $round ==="
	timeout 12 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=150 \
		--stream-to=/tmp/cap-m39-$round.h264
	sleep 2
done

echo "--- extent per round (one 'extent buf[0]' line per round = one frame per spawn) ---"
dmesg | grep -E 'extent buf|extent final|REPOISON'
echo "--- stream start/stop markers ---"
dmesg | grep -E 'stream start: SET_VIC|stream start: START|stream stop:'
echo "--- fault count ---"
dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
