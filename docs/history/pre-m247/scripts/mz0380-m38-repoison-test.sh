#!/bin/bash
# M38: is the card writing frames CONTINUOUSLY?
#
# M37's poison-byte discriminator (0xaa vs 0x55) proved there are NO holes -
# every "hole" was frame data colliding with the poison. The card writes ONE
# CONTIGUOUS 0..0x30a5bc burst, and 0x30a5c0 = 1557 chunks (last partial
# 0x5c0) = 1920 x 1107 x 1.5 EXACTLY: the transfer COMPLETES at its intended
# size. The "parked DMA engine" theory is dead.
#
# Which leaves the possibility every metric so far was blind to: the card is
# writing IDENTICAL frames to buf0 over and over - same bytes, extent frozen
# after the first frame, tokens silent (channel_done is something else's job).
#
# Discriminator: re-poison the buffers MID-STREAM. If the extent grows again
# after each repoison, the frame loop is alive and completing DMA per frame -
# and the real blocker is tinyvenc5's completion semantics: the channel_done
# pwrite at 0x168c8 sits AFTER the frame LOOP (counter vs limit at 0x16bfc),
# not inside it, so nosg mode may simply never reach it. Next RE target in
# that case: the loop bound/exit conditions in fake_frame_process and the
# alternate channel_done at 0x170d8/0x170e8.
#
# READ THE RESULT:
#   after each REPOISON line, "extent buf[0]=..." reappears within ~a frame
#   time -> CARD IS STREAMING CONTINUOUSLY (DMA fully healthy end-to-end).
#   Repoison also times the frame cadence: extent-line timestamps ~= fps.
#   after REPOISON nothing grows -> exactly one frame was ever written; park
#   confirmed after all; back to the DMAC wait RE.
set -u
cd "$(dirname "$0")/.."

POISON=${1:-0xaa}
echo "=== poison_byte=$POISON ==="

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

timeout 30 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=150 \
	--stream-to=/tmp/cap-m38.h264 &
V4L2=$!

# let the first frame land, then repoison three times
for t in 8 14 20; do
	sleep $((t == 8 ? 8 : 6))
	echo repoison > /proc/mz0380-events
done

wait $V4L2

echo "--- stream start sequence ---"
dmesg | grep -E 'stream start:' | tail -4
echo "--- *** REPOISON vs EXTENT timeline (the M38 measurement) *** ---"
dmesg | grep -E 'extent buf|REPOISON|extent final|holemap'
echo "--- buffer dump at stop ---"
dmesg | grep -E 'stop buf\[|stream stop:'
echo "--- fault count ---"
dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
echo "--- irq ---"
grep '164:' /proc/interrupts
