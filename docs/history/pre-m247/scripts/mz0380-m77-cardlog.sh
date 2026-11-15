#!/bin/bash
# M77: prove the LOAD_FILES (op 0x6e) transport before anything touches the
# card's boot image.
#
# video_capture_mgr's op-0x6e handler is an arbitrary-offset 16-byte read of
# /mnt/flash/PIC_ENC, and it echoes the serviced command back to the host, so
# the bytes come home in PARAM1..PARAM4. /proc/mz0380-cardlog pages the file
# out through it (RE_FINDINGS.md M76).
#
# This run costs ZERO encoder spawns - no STREAMON, no SET_VIC - so it does not
# touch the ~8-18 spawn wedge budget, and it writes nothing to the card.
#
# Decision:
#   - plausible file bytes (or a clean all-zero/short read) -> transport works,
#     and redirecting the card's stdout into that file gives us its console.
#   - every chunk "<read failed>" -> op 0x6e does not answer for us and the
#     console-over-PCIe plan is dead; fall back to the board's UART.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

BYTES=${1:-256}

cleanup() { rmmod mz0380 2>/dev/null && echo "(module unloaded)"; }
trap cleanup EXIT INT TERM

make >/dev/null || { echo "build failed"; exit 1; }
rmmod mz0380 2>/dev/null
sleep 1
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
dmesg -C
insmod ./mz0380.ko dma_handshake=1 enable_video=1 \
	procfs_verbosity=2 cardlog_bytes="$BYTES" \
	|| { echo "insmod failed"; exit 1; }

echo "waiting for the card to finish booting its own flash image..."
sleep 25

echo "=== card state ==="
dmesg | grep -E "firmware|boot|fw version|READY" | tail -8

echo
echo "=== /mnt/flash/PIC_ENC via LOAD_FILES(0x6e), $BYTES bytes ==="
cat /proc/mz0380-cardlog

echo
echo "=== mailbox errors during the paging, if any ==="
dmesg | grep -E "ret=-110|timed out|LOAD_FILES" | tail -10
