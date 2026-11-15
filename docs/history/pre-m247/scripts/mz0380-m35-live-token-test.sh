#!/bin/bash
# M35: LIVE completion-path discriminator. Settles the M34 ambiguity.
#
# Why M30-M33 could not distinguish the two remaining hypotheses:
#   - fake_frame fill is a CONSTANT 0x11 pattern, so frame N overwriting buf0
#     with identical bytes is invisible in the buffer dump;
#   - store_channel_done writes token nibbles as (value-1), so a card that
#     reuses slot 1 every frame writes (1-1)=0 to BAR0+0x40 forever - reading
#     0 at STOP proves nothing;
#   - all prior status reads happened AT STOP, never during streaming.
#
# New RE this session (see RE_FINDINGS.md M35):
#   - store_channel_done: tokens 0x40/44/48 (video) / 0x4c (audio) are written
#     UNCONDITIONALLY; EVENT[0x30] + MSI are gated on the one-shot msi_enable
#     credit (.data[0]), re-armed ONLY by pciep_isr_clrint (= our ack doorbell
#     0x400), and CLEARED on every delivered interrupt.
#   - MassMemAccess_WaitDMAC = while(ioctl(0xde01)) retry-forever; kernel side
#     sleeps un-timed until the DMAC ISR sets a per-slot flag. A card-side DMAC
#     park is unrecoverable without that IRQ.
#
# Userspace mmap of /sys/.../resource0 is blocked (CONFIG_IO_STRICT_DEVMEM:
# the driver's request_mem_region makes the BAR "busy" => sysfs mmap EINVAL),
# so the poller lives in the driver's event-watcher kthread (M35 additions):
#   - it now logs every change of BAR0 0x40/44/48/4c/0x50 ("live token" lines,
#     rate-capped at 20/s, exact change counts printed when the watcher stops);
#   - new param credit_kick_ms: period for unconditional ack kicks
#     (BAR5[0xdc]=2, EVENT=0, doorbell 0x400 -> clrint -> msi_enable=1).
# The watcher only runs after "echo start > /proc/mz0380-events".
#
# Phases: A (12 s) observe only; B (12 s) + 1 Hz credit kicks (live param).
#
# VERDICTS:
#   "live token" lines in phase A       -> channel_done IS firing; blocker is
#       MSI/EVENT delivery. (EVENT edges also land in /proc/mz0380-events.)
#   A quiet, tokens/"frame token" in B  -> completions were being swallowed by
#       a dead credit; fix = periodic/streamon re-arm in the driver.
#   A and B both quiet (all counts 0)   -> M34 CONFIRMED: no channel_done ever;
#       card DMAC never completes. Next: vpl_dmac ISRHead/IntrEnable RE + DMAC
#       IRQ plumbing.
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
	aic_on=1 credit_kick_ms=0 || { echo "insmod failed"; exit 1; }
sleep 3

echo start > /proc/mz0380-events || { echo "watcher start failed"; exit 1; }

timeout 30 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=150 \
	--stream-to=/tmp/cap-m35.h264 &
V4L2=$!

sleep 12
echo "=== phase B: enabling 1 Hz credit kicks ==="
echo 1000 > /sys/module/mz0380/parameters/credit_kick_ms

wait $V4L2
echo 0 > /sys/module/mz0380/parameters/credit_kick_ms
echo stop > /proc/mz0380-events   # prints the token-change summary

echo "--- stream start sequence ---"
dmesg | grep -E 'stream start:|SET_BUF\[' | tail -8
echo "--- *** LIVE TOKEN lines (phase A = pre-kick, phase B = post-kick) *** ---"
dmesg | grep -E 'live token|credit kick|token watch summary'
echo "--- *** FRAME TOKENS (MSI-delivered completions) *** ---"
dmesg | grep -E 'frame token' | head -15
echo "--- event ring (EVENT edges the watcher recorded) ---"
cat /proc/mz0380-events | head -40
echo "--- buffer contents / card status at stop ---"
dmesg | grep -E 'stop buf\[|stream stop:'
echo "--- fault count ---"
dmesg | grep -c 'AMD-Vi.*IO_PAGE_FAULT'
echo "--- irq / capture size ---"
grep '164:' /proc/interrupts
ls -l /tmp/cap-m35.h264
