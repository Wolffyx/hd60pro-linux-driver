#!/bin/bash
# M29: do frames land now?
#
# The "aperture offset" chased through M26-M28 was a buffer OVERRUN - an IOMMU
# only faults on unmapped addresses, so the first fault marks where the card
# LEAVES the buffer, not where it starts writing. Final model:
#     host_addr = ((word0 << 32) | word1) + aperture_offset(from 0)
# Fix: advertise the buffer base as-is (card_frame_offset=0) and give the card
# 4 MiB per buffer instead of 512 KiB.
#
# PASS:
#   - "SET_BUF[0] buf=0x100000000 target=0x0000000100000000 -> slot {00000001, 00000000}"
#   - fault count 0
#   - "frame token=... head=00 00 00 01 ..."   <- H.264 start code in our buffer
#   - /tmp/cap-m29.h264 non-zero, IRQs well past 4
# PARTIAL, and what it means:
#   - faults at 0x100400000 (the new 4 MiB end) -> still overrunning; the card
#     wants a bigger buffer. Raise MZ0380_STREAM_BUF_SIZE again.
#   - zero faults, frame token lines with head=00 00 00 01, but 0-byte capture
#     -> DMA is solved; the bug is downstream in the token/length/vb2 handoff.
#   - zero faults, head=00 00 00 00 -> card DMAs somewhere else entirely, or
#     writes nothing; check the encoder status words in the same log line.
set -u
cd "$(dirname "$0")/.."

make || { echo "build failed"; exit 1; }

fuser -k /dev/video0 2>/dev/null; sleep 1
rmmod mz0380 2>/dev/null
if lsmod | grep -q '^mz0380'; then echo "FATAL: still loaded"; exit 1; fi
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm \
	|| { echo "modprobe deps failed"; exit 1; }

dmesg -C
insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
	enable_video=1 procfs_verbosity=2 stream_nosg=1 dma_iova_remap=1 \
	|| { echo "insmod failed"; exit 1; }
sleep 3

timeout 20 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=150 \
	--stream-to=/tmp/cap-m29.h264

echo "--- mapping / SET_BUF ---"
dmesg | grep -E 'stream buf\[|SET_BUF\[|wraps 32 bits'
echo "--- IOMMU faults (unique) ---"
dmesg | grep -oE 'address=0x[0-9a-f]+' | sort -u | head -20
echo "--- fault count ---"
dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
echo "--- FRAME TOKENS (the payoff: head= bytes are our buffer's contents) ---"
dmesg | grep -E 'frame token' | head -15
echo "--- driver tail ---"
dmesg | grep -E 'mz0380' | tail -15
echo "--- irq / capture size ---"
grep '164:' /proc/interrupts
ls -l /tmp/cap-m29.h264
echo "--- first bytes (expect 00 00 00 01) ---"
head -c 64 /tmp/cap-m29.h264 | od -An -tx1
