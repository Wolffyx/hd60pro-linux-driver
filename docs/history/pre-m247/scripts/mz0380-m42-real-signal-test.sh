#!/bin/bash
# M42: real-signal bring-up. THE pivot away from the dead stream_nosg path.
#
# M41 closed the fake path: its DMA park is card-internal (missing DMAC
# completion IRQ) and unreachable from the host. The real path uses the same
# DMA machinery but runs in the card's encode_handler, paced by vpl_vic
# capture interrupts - and it is the product goal anyway.
#
# New in the driver for this run:
#   - EDID (the recovered 256-byte HD60 Pro blob) is loaded into the
#     receiver's DDC EEPROM at bring-up, then HPD is asserted. Without a
#     readable EDID an HDMI source keeps its transmitter OFF, which is why
#     every previous nosg=0 attempt saw a dead input.
#   - SET_VIC now carries the real fps (was always 0 -> card forced 60).
#   - streamon re-detects the live timing and refuses to start without lock.
#   - enc_stat handshake byte is acked (BAR0+0x50), required for frame 2+.
#
# PLUG A LIVE HDMI SOURCE INTO THE CARD'S INPUT BEFORE RUNNING THIS.
#
# READ THE RESULT:
#   "EDID loaded" + "HPD asserted"        -> the sink side is now real. If the
#       source still shows nothing, check the source's own output/EDID cache
#       (some need a physical re-plug after the first HPD edge).
#   "MST3367 signal: WxH..."              -> RECEIVER LOCKED. This alone is a
#       first: every prior run had no signal at all.
#   "MST3367 locked but unmatched"        -> lock is real, mode table needs the
#       reported geometry adding (paste the line into RE_FINDINGS).
#   "frame token=..." / irq second column -> FRAMES FLOWING. The DMAC IRQ is
#       alive on the real path and the fake-path park was a fake-path problem.
#   lock but no frames                    -> the DMAC completion IRQ really is
#       dead on this unit; next stop is the VIC/BT1120 routing into the SSM
#       ring (op 0x50 and the vpl_vic config).
set -u
cd "$(dirname "$0")/.."

make || { echo "build failed"; exit 1; }

fuser -k /dev/video0 2>/dev/null; sleep 1
rmmod mz0380 2>/dev/null
if lsmod | grep -q '^mz0380'; then echo "FATAL: still loaded"; exit 1; fi
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm \
	|| { echo "modprobe deps failed"; exit 1; }

dmesg -C
# stream_nosg=0 -> REAL capture path (encode_handler, not fake_frame_process)
insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
	enable_video=1 procfs_verbosity=2 stream_nosg=0 dma_iova_remap=1 \
	aic_on=1 buf_poison=1 || { echo "insmod failed"; exit 1; }
sleep 3

echo "--- receiver bring-up (reset -> init -> EDID -> HPD) ---"
dmesg | grep -E 'MST3367|EDID|HPD'

echo "--- signal detect (give the source time to wake after the HPD edge) ---"
for i in 1 2 3 4 5; do
	v4l2-ctl -d /dev/video0 --query-dv-timings 2>&1 | head -12
	if dmesg | grep -q 'MST3367 signal:'; then break; fi
	echo "  (no lock yet, retry $i/5)"
	sleep 3
done

echo "--- what the receiver reports ---"
dmesg | grep -E 'MST3367 signal|locked but unmatched'

echo "--- capture attempt ---"
timeout 20 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=150 \
	--stream-to=/tmp/cap-m42.h264

echo "--- stream sequence ---"
dmesg | grep -E 'stream start:|SET_BUF\[' | tail -8
echo "--- *** FRAME TOKENS (frames actually completing) *** ---"
dmesg | grep -E 'frame token' | head -10
echo "--- write extent (did anything land at all?) ---"
dmesg | grep -E 'extent buf|extent final|holemap'
echo "--- card status at stop ---"
dmesg | grep -E 'stream stop:|stop buf\['
echo "--- fault count ---"
dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
echo "--- irq (second column = video IRQs) / capture size ---"
grep '164:' /proc/interrupts
ls -l /tmp/cap-m42.h264
echo "--- first bytes (expect 00 00 00 01 for H.264) ---"
head -c 64 /tmp/cap-m42.h264 | od -An -tx1
