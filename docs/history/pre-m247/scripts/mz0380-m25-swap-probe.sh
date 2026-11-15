#!/bin/bash
# M25 probe: does channels[] slot word1 (-> ELBI 0x54, the intended host LOW
# target) reach the hardware at all?
#
# Runs the fake-frame path (stream_nosg=1) twice: once with the default slot
# packing {word0=high32, word1=low32} and once swapped {word0=low32,
# word1=high32}. Compare the AMD-Vi fault addresses against the SET_BUF lines.
#
#   swapped run faults at (low32 << 32)  -> only word0 lands; ELBI 0x54 ignored
#   swapped run faults at a sane address -> both words land
set -u
cd "$(dirname "$0")/.."

run_one() {
	local swap=$1
	echo "================ buf_pair_swap=$swap ================"
	fuser -k /dev/video0 2>/dev/null; sleep 1
	rmmod mz0380 2>/dev/null
	if lsmod | grep -q '^mz0380'; then
		echo "FATAL: mz0380 still loaded, aborting"; exit 1
	fi
	# mz0380.ko links against these; without them insmod dies with
	# "Unknown symbol in module".
	modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings \
		snd-pcm || { echo "modprobe deps failed"; exit 1; }
	dmesg -C
	insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
		enable_video=1 procfs_verbosity=2 stream_nosg=1 \
		buf_pair_swap=$swap || { echo "insmod failed"; exit 1; }
	sleep 3
	timeout 20 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=150 \
		--stream-to=/tmp/cap-swap$swap.h264
	echo "--- SET_BUF slots ---"
	dmesg | grep -E 'SET_BUF\['
	echo "--- IOMMU faults (unique addresses) ---"
	dmesg | grep -oE 'address=0x[0-9a-f]+' | sort -u | head -20
	echo "--- fault count ---"
	dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
	echo "--- driver tail ---"
	dmesg | grep -E 'mz0380|frame token' | tail -20
	echo "--- irq / capture size ---"
	grep '164:' /proc/interrupts
	ls -l /tmp/cap-swap$swap.h264 2>/dev/null
}

run_one 1
run_one 0
echo "================ done ================"
