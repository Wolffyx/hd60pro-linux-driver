#!/bin/bash
# M28: do the frames finally land?
#
# Model after M27 (hw-measured):
#     host_addr = ((word0 << 32) | word1) + 0x80000     (low half adds mod 2^32)
# So each buffer is mapped 0x80000 ABOVE the address advertised in SET_BUF:
#     advertise base + (i<<32)  ->  slot {i+1, 0}
#     map at    base + (i<<32) + 0x80000
# and the card's +0x80000 lands on the buffer base.
#
# PASS:
#   - "SET_BUF[0] buf=0x100080000 target=0x0000000100000000 -> slot {00000001, 00000000}"
#   - fault count 0
#   - /tmp/cap-m28.h264 non-zero, starting 00 00 00 01
#   - IRQ count climbing well past 4
# FAIL, and what it means:
#   - faults at 0x100100000  -> offset is not constant; it tracks the advertised
#                               address twice. Re-measure with card_frame_offset=0.
#   - faults at 0x100080000  -> offset applies to the MAPPING, not the target;
#                               drop back to dma_iova_offset and advertise raw.
#   - zero faults but 0-byte capture -> DMA lands, completion path is the problem
#                               (frame token / drain), not the address.
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
	--stream-to=/tmp/cap-m28.h264

echo "--- mapping / SET_BUF ---"
dmesg | grep -E 'stream buf\[|SET_BUF\[|wraps 32 bits'
echo "--- IOMMU faults (unique) ---"
dmesg | grep -oE 'address=0x[0-9a-f]+' | sort -u | head -20
echo "--- fault count ---"
dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
echo "--- driver tail ---"
dmesg | grep -E 'mz0380|frame token' | tail -25
echo "--- irq / capture size ---"
grep '164:' /proc/interrupts
ls -l /tmp/cap-m28.h264
echo "--- first bytes (expect 00 00 00 01) ---"
head -c 64 /tmp/cap-m28.h264 | od -An -tx1
