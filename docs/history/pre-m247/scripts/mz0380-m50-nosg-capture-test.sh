#!/bin/bash
# M50: pragmatic capture - does the nosg polling path deliver NV12 frames
# to userspace end-to-end?
#
# New machinery under test (mz0380-dma.c mz0380_nosg_thread): with
# stream_nosg=1 the V4L2 node switches to NV12 720x1080 and streamon runs a
# kthread that per frame: poisons buf0, spawns the encoder (SET_VIC ->
# SET_BUF -> op6), polls the tail of the 0x30a5c0-byte raw burst, copies the
# leading 0x11cc40 NV12 bytes into the queued vb2 buffer, sends op7, and
# respawns (M39: one frame per fresh spawn). Expected cadence ~1 frame per
# (start_delay_ms + 450 ms + stop); 500 ms delay verified good on hw.
#
# First hw run (2026-08-14): 4/4 frames both at delay 2000 and 500, 0 faults
# during capture (one boot-time 0x90000000 fault before streaming = the
# known pre-READY artifact, mz0380.h). The frame decoded to the card's OWN
# "NO SIGNAL" splash (spinner + text) - the fake-frame renderer draws it at
# 720 wide with neutral 0x80 chroma regardless of SET_VIC width, hence the
# NV12 720x1080 payload (M50 layout note in mz0380-reg.h).
#
# Decision table:
#   - capture file size == N * 1166400 and 'nosg polling capture stopped
#     after N frames' with N >= 2   -> WORKING repeated capture. Inspect:
#       ffplay -f rawvideo -pixel_format nv12 -video_size 720x1080 /tmp/cap-m50.nv12
#     (expect the grayscale NO SIGNAL splash: spinner + text on black)
#   - exactly 1 frame then 'did not land' warnings -> respawn regression:
#     compare against mz0380-m39-respawn-test.sh (which proved 2 spawns work)
#   - 0 frames, v4l2-ctl times out -> tail-detect never fired: check dmesg
#     for 'nosg spawn failed' (mailbox error) vs silence (frame truly absent;
#     re-verify with buf_poison=1 extent lines)
#   - 0 frames + repeated 'nosg spawn failed (-110)' -> the card's mailbox
#     stopped answering SET_VIC: suspected card-side exhaustion from
#     accumulated parked tinyvenc5 processes (each spawn leaks one, M41;
#     rmmod/insmod + fw re-upload does NOT reboot the card). Remedy: FULL
#     cold boot (mains off; a warm reboot may keep PCIe aux power). If a
#     cold boot fixes it, the per-boot capture budget is finite until a
#     real card reset opcode is found (op 0xFF untested candidate).
#   - file size a multiple of something OTHER than 1166400 -> vb2 plane /
#     payload mismatch, check queue_setup sizes in dmesg
set -u
cd "$(dirname "$0")/.."

FRAMES=${1:-4}
START_DELAY=${2:-2000}   # ms; the spawn gap dominates cadence - sweep down
                         # (500? 200?) once the path works at the safe 2000
make || { echo "build failed"; exit 1; }

fuser -k /dev/video0 2>/dev/null; sleep 1
rmmod mz0380 2>/dev/null
if lsmod | grep -q '^mz0380'; then echo "FATAL: still loaded"; exit 1; fi
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm \
	|| { echo "modprobe deps failed"; exit 1; }

dmesg -C
insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
	enable_video=1 procfs_verbosity=2 stream_nosg=1 dma_iova_remap=1 \
	aic_on=1 buf_poison=0 start_delay_ms="$START_DELAY" \
	|| { echo "insmod failed"; exit 1; }
sleep 3

echo "=== format the node advertises (must be NV12 720x1080) ==="
v4l2-ctl -d /dev/video0 --get-fmt-video

# generous timeout: FRAMES * (start_delay + land + stop) + firmware slack
TMO=$(( (START_DELAY / 1000 + 3) * FRAMES + 15 ))
echo "=== capturing $FRAMES frames (timeout ${TMO}s) ==="
rm -f /tmp/cap-m50.nv12
timeout "$TMO" v4l2-ctl -d /dev/video0 --stream-mmap --stream-count="$FRAMES" \
	--stream-to=/tmp/cap-m50.nv12
sleep 1

SZ=$(stat -c %s /tmp/cap-m50.nv12 2>/dev/null || echo 0)
echo "--- capture file: $SZ bytes = $((SZ / 3110400)) NV12 frames (remainder $((SZ % 3110400))) ---"
echo "--- nosg lifecycle ---"
dmesg | grep -E 'nosg polling capture|nosg frame|nosg spawn'
echo "--- per-spawn markers (one SET_VIC per frame expected) ---"
dmesg | grep -cE 'stream start: SET_VIC'
echo "--- fault count (must be 0) ---"
dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
echo "--- unique bytes in frame 1 (fake fill => tiny set) ---"
head -c 3110400 /tmp/cap-m50.nv12 | od -An -tx1 -v | tr ' ' '\n' | sort -u | grep -v '^$' | head -8
