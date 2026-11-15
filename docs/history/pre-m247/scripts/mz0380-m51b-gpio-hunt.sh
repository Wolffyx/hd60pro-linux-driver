#!/bin/bash
# M51b: which GPIO pins are actually there, and which float HIGH?
#
# The first bit-bang attempt (M51) aborted on both pin orders with "SDA
# reads LOW when released" for pins 12/13. Two candidate causes, both
# cheap to separate:
#   (a) pins 12/13 do not exist / are not the I2C pair - an older probe saw
#       only bits 3 and 6 high at idle, so the port may be narrow;
#   (b) the GPIO_DIR (op 0x17) data-bit polarity is inverted from our
#       guess, which swaps release/drive and makes ANY line look stuck low.
#
# This script dumps the idle level of all 32 pins (I2C pairs sit on
# pull-ups => read HIGH when idle), then retries the ACK scan on every
# HIGH pin pair, under BOTH direction polarities.
#
# Decision table:
#   - two or more pins read HIGH, and a scan on some pair reports
#     "ACK at 0x50" -> the DDC EEPROM is found; burn with
#       (edidburn was removed in M245 - it wrote the card's EEPROM)
#   - HIGH pins exist but every pair scans with 0 ACKs -> those pins are
#     not the DDC pair (they may be status inputs); the EEPROM is likely
#     only reachable from the card's own /dev/i2c-1, which the mailbox
#     does not expose -> back to the Windows-trace plan.
#   - NO pin reads HIGH under either polarity -> op 0x14 does not sample
#     real pin state for this port (it may return the output latch), and
#     the bit-bang route is blind; abandon it.
set -u
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
make >/dev/null || { echo "build failed"; exit 1; }

load() {
	local inv=$1
	fuser -k /dev/video0 2>/dev/null
	rmmod mz0380 2>/dev/null
	sleep 1
	modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
	insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
		enable_video=1 procfs_verbosity=2 dma_iova_remap=1 \
		gpio_dir_invert=$inv || { echo "insmod failed"; exit 1; }
	sleep 25   # card boots its own flash image
}

for INV in 0 1; do
	echo "############ gpio_dir_invert=$INV ############"
	load $INV
	dmesg -C
	echo "gpiodump" > /proc/mz0380-hdmi
	sleep 5
	dmesg | grep -E "gpiodump"

	HIGH=$(dmesg | sed -n 's/.*gpiodump   pin *\([0-9]*\) reads HIGH.*/\1/p' | tr '\n' ' ')
	echo "--- pins HIGH at idle: ${HIGH:-none} ---"
	[ -z "$HIGH" ] && { echo "no candidates under this polarity"; continue; }

	for A in $HIGH; do
		for B in $HIGH; do
			[ "$A" = "$B" ] && continue
			echo "--- i2cscan sda=$A scl=$B (invert=$INV) ---"
			dmesg -C
			echo "i2cscan $A $B" > /proc/mz0380-hdmi
			sleep 12
			dmesg | grep -E "i2cbb" | head -5
			if dmesg | grep -q "i2cbb ACK"; then
				echo "*** DEVICES FOUND: sda=$A scl=$B invert=$INV ***"
				dmesg | grep "i2cbb ACK"
				exit 0
			fi
		done
	done
done

echo "no I2C devices found on any HIGH pin pair under either polarity"
exit 1
