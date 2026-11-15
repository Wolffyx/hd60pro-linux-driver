#!/bin/bash
# M27: the card's aperture offset.
#
# M26 landed the high32: with buffers mapped at IOVA (i<<32) the writes went to
# 0x100080000 = (1 << 32) + 0x80000, i.e. into buf0's window slot but one
# SET_BUF stride past its base. Hypothesis: the card places bufindex n at
# aperture offset n*stride, starting at bufindex 2 -> 1 * 0x80000.
#
# Run A: stride = 0 in SET_BUF (cmd[0x8]). If the offset is n*stride, the
#        writes drop to offset 0 and land on the buffer base -> zero faults.
# Run B: stride left alone, mappings shifted up by 0x80000 instead, so the
#        buffer sits where the card already writes -> zero faults.
# Exactly one should pass; that one becomes the default.
set -u
cd "$(dirname "$0")/.."

make || { echo "build failed"; exit 1; }

run_one() {
	local tag=$1; shift
	echo "================ $tag ================"
	echo "params: $*"
	fuser -k /dev/video0 2>/dev/null; sleep 1
	rmmod mz0380 2>/dev/null
	if lsmod | grep -q '^mz0380'; then echo "FATAL: still loaded"; exit 1; fi
	modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings \
		snd-pcm || { echo "modprobe deps failed"; exit 1; }
	dmesg -C
	insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
		enable_video=1 procfs_verbosity=2 stream_nosg=1 \
		dma_iova_remap=1 "$@" || { echo "insmod failed"; exit 1; }
	sleep 3
	timeout 20 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=150 \
		--stream-to=/tmp/cap-$tag.h264
	echo "--- IOVA mapping / SET_BUF ---"
	dmesg | grep -E 'stream buf\[|SET_BUF\['
	echo "--- IOMMU faults (unique) ---"
	dmesg | grep -oE 'address=0x[0-9a-f]+' | sort -u | head -20
	echo "--- fault count ---"
	dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
	echo "--- irq ---"
	grep '164:' /proc/interrupts
	ls -l /tmp/cap-$tag.h264
	echo "--- first bytes (expect 00 00 00 01) ---"
	head -c 64 /tmp/cap-$tag.h264 | od -An -tx1
}

run_one A-stride0 set_buf_stride=0
run_one B-shift   dma_iova_offset=0x80000
echo "================ done ================"
