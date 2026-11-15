#!/bin/bash
# M51: is there a DDC EDID EEPROM on a second I2C bus, reachable by
# bit-banging the card's spare GPIOs?
#
# Motivation (from the GPL hdcapm driver, same MST3367 + sibling Elgato
# product): these cards serve EDID from a plain I2C EEPROM (0xa0/0xa2) wired
# to the HDMI connector's DDC pins on a SEPARATE bus - not from the receiver
# and not from software. Our mailbox I2C proxy only reaches the receiver bus
# (M43: 0x9c + 0x98 only), which would explain all eight negative EDID
# sweeps: wrong bus. GPIO12/13 are the suspected internal I2C pair.
#
# The driver bit-bangs I2C via GPIO opcodes (op 0x14 read / 0x15 set /
# 0x17 direction), one mailbox command per line transition (~2 ms each).
#
# Decision table:
#   - "ACK at 0x50" (and/or 0x51..0x57)  -> EDID EEPROM FOUND. Burn it:
#         (edidburn was removed in M245 - it wrote the card's EEPROM)
#     A VERIFIED burn is permanent (non-volatile) - then pulse HPD
#     (echo "hpd 3 4000" > /proc/mz0380-hdmi) and watch the source: if it
#     starts transmitting, the EDID blocker is DEAD without a Windows trace.
#   - "SDA reads LOW when released - aborting" on both pin orders ->
#     either wrong pins (try other spares), wrong GPIO_DIR polarity (flip
#     the data sense in mz0380_bb_dir), or the pins have no pull-ups
#     (then this bus needs the logic analyzer after all).
#   - scan runs but 0 ACKs on both orders -> pins toggle but nothing
#     listens: likely not the I2C pair; try other pin numbers below.
#   - ">16 ACKs - stuck bus" -> SCL not actually toggling (wrong pin or
#     polarity); same remedies as above.
#
# NOTE: needs a mailbox that answers (cold boot first if the card is in the
# wedged -110 state).
set -u
cd "$(dirname "$0")/.."

SDA=${1:-13}
SCL=${2:-12}
make || { echo "build failed"; exit 1; }

if ! lsmod | grep -q '^mz0380'; then
	modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
	insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
		enable_video=1 procfs_verbosity=2 dma_iova_remap=1 \
		|| { echo "insmod failed"; exit 1; }
	sleep 3
fi

dmesg -C
echo "=== scan sda=$SDA scl=$SCL ==="
echo "i2cscan $SDA $SCL" > /proc/mz0380-hdmi
sleep 12
echo "=== scan swapped sda=$SCL scl=$SDA ==="
echo "i2cscan $SCL $SDA" > /proc/mz0380-hdmi
sleep 12
dmesg | grep -E "i2cbb"
