#!/bin/bash
# M36: measure the card's TRUE write extent and its progress over time.
#
# M35 proved channel_done never fires (tokens ungated, 0 changes in 24 s) and
# that the park is inside the per-chunk StartDMAC/WaitDMAC loop (2048-byte
# DMAC chunks; the visible stop 0x30a000 = 1556 x 0x800 exactly, so >1500
# chunk completions DID fire before the stall - the DMAC IRQ path works).
#
# But "visible" was one byte per 4 KiB page tested for non-zero: the fake
# frame is 0x11 fill + ZERO padding, so card-written zeros were invisible and
# the true extent/stop point is unknown. This run poisons all four buffers
# with 0xAA before START (driver param buf_poison, default on) and a driver
# kthread logs the write extent per buffer every 20 ms.
#
# READ THE RESULT:
#   "extent buf[0]=0x... (+0x...)" lines: timestamps give the progress curve.
#   - grows the whole run at ~constant rate  -> CRAWLING, not stalled: the
#     outbound path is slow (flow control / back-pressure); compute B/s.
#   - jumps quickly then freezes at X        -> hard stall at exact offset X.
#     X == true transfer end; compare against 0x30a000, frame size 0x2F7600,
#     chunk grid 0x800, and check whether X repeats across runs.
#   - reaches 0x400000 (full 4 MiB buffer)   -> the card wrote everything it
#     was asked and STILL no channel_done -> re-aim at the second Start/Wait
#     pair in ProcessOneFrame (metadata DMA) or the frame-loop exit paths.
#   - buf1-3 extents non-zero                -> the card does advance the
#     ring; slot-reuse assumption wrong.
# Also dumps PCIe MPS/MRRS + AER status: a payload-size mismatch or logged
# PCIe error would explain a deterministic outbound stall.
set -u
cd "$(dirname "$0")/.."

# M37: optional poison byte override (default 0xaa). Run once with 0x55 to
# tell a real un-written hole from frame data that merely equals the poison.
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

echo start > /proc/mz0380-events

timeout 30 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=150 \
	--stream-to=/tmp/cap-m36.h264

echo stop > /proc/mz0380-events

echo "--- stream start sequence ---"
dmesg | grep -E 'stream start:|SET_BUF\[' | tail -8
echo "--- *** WRITE EXTENT progression (the M36 measurement) *** ---"
dmesg | grep -E 'extent buf|extent final'
echo "--- *** HOLE MAP (M37: data<->poison transitions per buffer) *** ---"
dmesg | grep -E 'holemap'
echo "--- token watch (should stay 0 per M35) ---"
dmesg | grep -E 'live token|token watch summary'
echo "--- buffer dump at stop (now poison-aware: 'touched' includes zeros) ---"
dmesg | grep -E 'stop buf\[|stream stop:'
echo "--- fault count ---"
dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
echo "--- PCIe MPS/MRRS + AER (card, then upstream bridge) ---"
for d in 0000:04:00.0 $(basename "$(dirname "$(readlink /sys/bus/pci/devices/0000:04:00.0)")"); do
	echo "== $d =="
	lspci -vv -s "${d#0000:}" 2>/dev/null | grep -E 'DevCtl:|DevSta:|MaxPayload|MaxReadReq|CESta:|UESta:|LnkSta:'
done
echo "--- irq / capture size ---"
grep '164:' /proc/interrupts
ls -l /tmp/cap-m36.h264
