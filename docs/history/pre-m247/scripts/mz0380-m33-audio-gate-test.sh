#!/bin/bash
# M33: release the encoder's audio gate, and stop with the right opcode.
#
# Two findings from video_capture_mgr + tinyvenc5, both from the card's own
# binaries:
#
# 1. tinyvenc5's is_nosg path waits on /sys/audio_status/audio_ready before it
#    will ACK START_STREAMING:
#      [tiny5]is_nosg wait audio timeout, i2s_num=%d, audio ready[%d]
#      [tiny5][ch %d] START_STREAMING -------------> is_nosg ACK
#    The only writer of that file is video_capture_mgr's SET_AIC_PARAMS
#    handler: "echo '1' > /sys/audio_status/audio_ready" when the command's
#    on byte is set. We had never sent it - which fits M30-M32 exactly: a raw
#    frame lands in our buffer, but channel_done is never written.
#
# 2. video_capture_mgr's dispatch is: op 7 -> STOP_STREAMING, op 41 -> SET_VIC,
#    op 42 -> SET_AIC_PARAMS. So 0x2a is NOT stop (M17 guessed wrong): we were
#    sending SET_AIC with a zeroed payload - "audio off" - at every streamoff,
#    and never sending a stop at all. Both are corrected here.
#
# PASS:
#   - "SET_AIC(on=1, 2 ch, 16 bit, 48000 Hz, ...) ret=0"
#   - "frame token=0x... idx=... head=..." lines appear  <- THE COMPLETION PATH
#   - irqs climbing past the command count, second number (video IRQs) > 0
#   - /tmp/cap-m33.h264 non-zero
# PARTIAL:
#   - SET_AIC ret != 0 -> the payload shape is wrong; the field offsets are in
#     mz0380-reg.h, re-check them against re-dump/vcm.txt around 0x9358.
#   - still no frame token, buf0 still filling -> audio_ready was not the gate
#     (or not the only one). Next: op 0x50, the last undecoded channel.
#   - buf0 now EMPTY too -> the AIC command disturbed the video path; try
#     aic_on=0 to confirm it is the cause.
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
	aic_on=1 || { echo "insmod failed"; exit 1; }
sleep 3

timeout 20 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=150 \
	--stream-to=/tmp/cap-m33.h264

echo "--- stream start sequence (SET_VIC -> SET_BUF -> SET_AIC -> START) ---"
dmesg | grep -E 'stream start:|SET_BUF\['
echo "--- *** FRAME TOKENS (completion path - the thing we are testing) *** ---"
dmesg | grep -E 'frame token' | head -15
echo "--- buffer contents at stop ---"
dmesg | grep -E 'stop buf\['
echo "--- card status at stop ---"
dmesg | grep -E 'stream stop:'
echo "--- fault count ---"
dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
echo "--- irq (second number = video IRQs) / capture size ---"
grep '164:' /proc/interrupts
ls -l /tmp/cap-m33.h264
echo "--- first bytes (expect 00 00 00 01 for H.264) ---"
head -c 64 /tmp/cap-m33.h264 | od -An -tx1
