#!/bin/bash
# M30: the DMA no longer faults - but is the card writing?
#
# M29 got the fault count to 0 for the first time. That is necessarily
# ambiguous: an IOMMU reports only the writes that MISS, so "no faults" means
# either "every write landed in our buffer" or "no writes happened". Neither
# the frame-token log nor the IRQ count moved (still 4, all command-done), so
# the completion path told us nothing either.
#
# This run dumps the buffer contents at streamoff, plus the card's status
# words, which separates the two cases.
#
# READ THE "stop buf[i]" LINES:
#   head=00 00 00 01 ... , pages non-zero > 0
#       -> THE CARD IS WRITING H.264 INTO OUR MEMORY. DMA is done; what is
#          broken is the completion signal (no frame-done MSI / token), so the
#          next hunt is the EVENT/token path, not the address path.
#   head all 00, 0 pages non-zero
#       -> nothing landed. The card stopped writing when the faults stopped,
#          which would mean the target it now has is one it declines to use.
#          Compare EVENT/enc words against the M29 run.
#   head non-zero but not 00 00 00 01
#       -> something lands but it is not an H.264 access unit; dump more of it
#          before trusting the length/token fields.
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
	--stream-to=/tmp/cap-m30.h264

echo "--- mapping / SET_BUF ---"
dmesg | grep -E 'stream buf\[|SET_BUF\['
echo "--- *** BUFFER CONTENTS AT STOP (the answer) *** ---"
dmesg | grep -E 'stop buf\['
echo "--- card status at stop ---"
dmesg | grep -E 'stream stop:'
echo "--- frame tokens ---"
dmesg | grep -E 'frame token' | head -10
echo "--- fault count ---"
dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
echo "--- irq / capture size ---"
grep '164:' /proc/interrupts
ls -l /tmp/cap-m30.h264
