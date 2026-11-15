#!/bin/bash
# M43: verify the sink chain by READ-BACK, one link at a time.
#
# M42 loaded the EDID and asserted HPD, and the receiver still reported no
# lock. But "EDID loaded" only means the mailbox accepted the command: the
# card firmware forces the I2C result byte to 0 on a NAK, so a write that
# never reached a chip looks identical to a good one. Nothing in that run was
# actually verified. This reads every link back.
#
# HOW TO READ /proc/mz0380-hdmi (printed below):
#
#   EDID read-back "00 ff ff ff ff ff ff 00"
#       -> the DDC EEPROM really holds our blob. 0xff cannot be manufactured
#          by a NAK (the firmware forces 0x00), so this is proof. The sink
#          side is genuinely real and the problem is upstream: the source, the
#          cable, or the receiver's HDMI config values (M10 step 2, the one
#          part we never recovered).
#   EDID read-back "00 00 00 00 00 00 00 00"
#       -> the writes went NOWHERE. Either op 0x1f is not implemented in our
#          fw 1.11 ep.ko, or the DDC EEPROM is not on the bus op 0x1f drives
#          (RE_FINDINGS M11/M13 flag exactly this bus ambiguity). Next: try
#          the per-byte path (op 0x1b to dev 0xA0) and the op 0x20 combo.
#
#   MST3367 BANK0 row all "=00"
#       -> the receiver is not answering AT ALL. Then EDID/HPD are irrelevant
#          and the problem is reset/power (pin9) - re-check the GPIO row.
#   BANK0 row with mixed values, 0x55 not 0x3c
#       -> receiver alive, no signal locked. Sink side is fine; the source is
#          not sending (check EDID above, then cable/source).
#   0x55 & 0x3c == 0x3c
#       -> LOCKED. Re-run mz0380-m42-real-signal-test.sh.
#
#   GPIO hpd pin1 = 1 expected after bring-up; rx_reset pin9 = 1 (released).
#       If pin9 reads 0 the receiver is held in reset and nothing else matters.
#       If the read itself fails, op 0x14 is unimplemented - fall back to
#       trusting the writes for GPIO only.
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
	enable_video=1 procfs_verbosity=2 stream_nosg=0 dma_iova_remap=1 \
	|| { echo "insmod failed"; exit 1; }
sleep 3

# force the bring-up (it also runs lazily on the first detect)
v4l2-ctl -d /dev/video0 --query-dv-timings >/dev/null 2>&1
sleep 1

echo "=============== SINK CHAIN READ-BACK ==============="
cat /proc/mz0380-hdmi

# M44: is BANK3 writable RAM (the EDID store) or a dead shadow?
echo "=============== BANK3 RAM TEST ==============="
echo ramtest > /proc/mz0380-hdmi
dmesg | grep -E 'BANK3'

echo "=============== bring-up log ==============="
dmesg | grep -E 'MST3367|EDID|HPD|periph|REG_READ' | tail -20
