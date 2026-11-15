#!/bin/bash
# M32: which outbound window does the encoder write its bitstream to?
#
# Where we are: a raw ~3.1 MiB frame lands in window0 buf0 (0x11 fill = the
# card's fake-frame test pattern), zero IOMMU faults, but the card never
# signals channel_done, so no frame-done MSI and nothing reaches vb2.
#
# ep.ko is NOT the gate. msi.constprop.1 (command-done) and store_channel_done
# (frame-done) share one re-arm token: both bail if it is 0, delivery sets it to
# 0, and pciep_isr_clrint sets it back to 1. Our command-done MSIs work, so that
# token cycles fine - if store_channel_done had run we would have seen an MSI.
# The card's userspace simply is not declaring a finished frame.
#
# Hypothesis: op2 programs window0 only, which is where the RAW frame goes. The
# encoder's bitstream destination is a different outbound window that we have
# never programmed, so tinyvenc has nowhere to write and never completes.
#
# This run also programs windows 1-3 (op 0x04/0x05/0x03) pointed at bufs 1/2/3.
#
# READ THE "stop buf[i]" LINES:
#   buf[1|2|3] non-zero, head 00 00 00 01 -> THAT window is the bitstream
#       output. Wire it up properly and the encoder should start completing.
#   buf[1|2|3] non-zero but not a start code -> that window gets something else
#       (statistics? a second plane?); dump more before concluding.
#   only buf[0] non-zero, still no frame token -> the missing piece is not a
#       buffer target. Next suspects: the encoder needs a bitstream-buffer
#       command we have not found (op 0x50, still undecoded, is the only
#       host->card channel left), or it is stalled waiting on something else.
#   op 0x04/0x05/0x03 returning an error -> those opcodes want a different
#       payload shape than op2; read the ret= values.
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
	probe_windows=1 || { echo "insmod failed"; exit 1; }
sleep 3

timeout 20 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=150 \
	--stream-to=/tmp/cap-m32.h264

echo "--- SET_BUF + window probes ---"
dmesg | grep -E 'SET_BUF\[|probe window'
echo "--- *** BUFFER CONTENTS AT STOP (which window is live?) *** ---"
dmesg | grep -E 'stop buf\['
echo "--- card status at stop ---"
dmesg | grep -E 'stream stop:'
echo "--- frame tokens ---"
dmesg | grep -E 'frame token' | head -10
echo "--- fault count ---"
dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
echo "--- unique fault addresses (a new window writing somewhere unmapped) ---"
dmesg | grep -oE 'address=0x[0-9a-f]+' | sort -u | head -20
echo "--- irq / capture size ---"
grep '164:' /proc/interrupts
ls -l /tmp/cap-m32.h264
