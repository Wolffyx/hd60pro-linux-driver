#!/bin/bash
# M26 test: 4GiB-aligned IOVA placement of the stream buffers.
#
# M25 proved the card latches only the HIGH 32 bits of the host DMA target
# (host_addr == (slot word0 << 32) + aperture_offset; ELBI 0x54, the low half,
# is dropped by the hardware). So each buffer is now iommu_map()'d at
# dma_iova_base + (i << 32), making its low 32 bits zero.
#
# PASS looks like:
#   - "stream buf[i] phys=... mapped at IOVA 0x1_0000_0000 (high32=0x00000001...)"
#   - "SET_BUF[i] dma=0x100000000 -> slot {00000001, 00000000}"
#   - ZERO AMD-Vi IO_PAGE_FAULTs
#   - /tmp/cap-m26.h264 non-zero, IRQ count climbing past 4
# FAIL modes to read for:
#   - "no IOMMU domain" / "pass-through" -> boot without iommu=pt
#   - "IOVA ... already mapped"          -> raise dma_iova_base
#   - faults at 0x100000000 + small offsets -> mapping is right, buffer wrong
set -u
cd "$(dirname "$0")/.."

make || { echo "build failed"; exit 1; }

fuser -k /dev/video0 2>/dev/null; sleep 1
rmmod mz0380 2>/dev/null
if lsmod | grep -q '^mz0380'; then echo "FATAL: mz0380 still loaded"; exit 1; fi

modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm \
	|| { echo "modprobe deps failed"; exit 1; }

echo "--- IOMMU state ---"
grep -o 'iommu=[^ ]*' /proc/cmdline || echo "(no iommu= on cmdline)"
ls -d /sys/bus/pci/devices/0000:04:00.0/iommu_group 2>/dev/null \
	&& cat /sys/bus/pci/devices/0000:04:00.0/iommu_group/type 2>/dev/null

dmesg -C
insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
	enable_video=1 procfs_verbosity=2 stream_nosg=1 \
	dma_iova_remap=1 || { echo "insmod failed"; exit 1; }
sleep 3

timeout 20 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=150 \
	--stream-to=/tmp/cap-m26.h264

echo "--- IOVA mapping ---"
dmesg | grep -E 'stream buf\[|IOVA|iommu_map|pass-through|IOMMU domain'
echo "--- SET_BUF slots ---"
dmesg | grep -E 'SET_BUF\['
echo "--- IOMMU faults (unique addresses) ---"
dmesg | grep -oE 'address=0x[0-9a-f]+' | sort -u | head -20
echo "--- fault count ---"
dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
echo "--- driver tail ---"
dmesg | grep -E 'mz0380|frame token' | tail -25
echo "--- irq / capture size ---"
grep '164:' /proc/interrupts
ls -l /tmp/cap-m26.h264
echo "--- first bytes of capture (expect H.264 start codes 00 00 00 01) ---"
head -c 64 /tmp/cap-m26.h264 | od -An -tx1
