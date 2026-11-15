#!/bin/bash
# M83: does this board actually carry the two I2C slaves the Windows driver
# talks to and we never have?
#
# The 2026-08-19 Windows collection disassembled every I2C transaction in
# e60MZ0380.X64.SYS v1.1.0.195 and found FOUR 8-bit addresses, not two:
#
#   0x9C  MST3367 HDMI receiver          454 transactions   (we drive this)
#   0x98  "colour-space / video path"    233 transactions   (we have NEVER
#                                                            touched it)
#   0x90  "alternate CSC path"           247 transactions   (likewise)
#   0xA0  EDID EEPROM                      2 bulk writes
#
# That trace is STATIC - it covers every board the driver supports, so the mere
# presence of a call site does not prove the chip is fitted on the HD60 Pro.
# This settles it empirically, and it matters: the open blocker is that the
# SoC's VIC reports no signal on BT1120 while the receiver holds a clean lock.
# A colour-space/video-path device sitting between the receiver's output and
# the SoC's input, left in whatever state power-on gave it, is exactly the kind
# of thing that produces that symptom - and it is the first new suspect since
# the host-side configuration space was declared closed.
#
# Costs: ZERO encoder spawns, no STREAMON, no SET_VIC, no writes of any kind.
# Needs no HDMI source - a bus probe is independent of whether anything is
# plugged in.
#
# Reading the result. REG_READ (op 0x1a) goes out over the card's own I2C
# master and the firmware forces the result byte to 0x00 on a NAK (M15), so:
#
#   varied, non-zero bytes across the range  -> the device ACKs: it is fitted
#   every register 0x00                      -> NAK on every address: not fitted
#                                               (or held in reset, like the
#                                               MST3367 was before M14)
#   every register identical and non-zero    -> suspicious; the bus may be
#                                               floating or mirroring 0x9C
#
# 0x9C is scanned first as a positive control. With no source connected its
# reg 0x55 should read 0x03 (no signal) and the rest of bank 0 should be a
# mixture - if 0x9C itself comes back all-zero the probe is broken, not the
# board, and nothing below means anything.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

COUNT=${1:-0x40}
# M126: the window matters. 0x98's interesting registers - the ones Windows
# writes from the per-resolution table at 0x1402D3B90, and the two constants
# 0xb1=0x0c / 0xb2=0xea - all sit ABOVE 0x3f, so the default 0x00..0x3f scan
# cannot see them. START=0x90 COUNT=0x40 covers 0x90..0xcf.
START=${START:-0x00}

cleanup() { rmmod mz0380 2>/dev/null && echo "(module unloaded)"; }
trap cleanup EXIT INT TERM

make >/dev/null || { echo "build failed"; exit 1; }
rmmod mz0380 2>/dev/null
sleep 1
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
dmesg -C
insmod ./mz0380.ko dma_handshake=1 enable_video=1 \
	periph_count="$COUNT" periph_start="$START" \
	|| { echo "insmod failed"; exit 1; }

echo "waiting for the card to finish booting its own flash image..."
sleep 25

echo "=== interrupt binding (M82: Windows uses INTx) ==="
dmesg | grep -E "interrupt:|irq" | tail -5

echo
echo "=== card state ==="
dmesg | grep -E "firmware|fw version|READY" | tail -5

for chip in 0x9c 0x98 0x90; do
	case "$chip" in
	0x9c) note="POSITIVE CONTROL - MST3367 receiver, known good" ;;
	0x98) note="Windows CSC / video path - 233 transactions, never driven by us" ;;
	0x90) note="Windows alternate CSC - 247 transactions, never driven by us" ;;
	esac

	echo
	echo "================================================================"
	echo "=== chip $chip   ($note)"
	echo "================================================================"
	echo "$chip" > /sys/module/mz0380/parameters/periph_chip 2>/dev/null ||
		printf '%d\n' "$chip" > /sys/module/mz0380/parameters/periph_chip
	cat /proc/mz0380-periph-scan

	nz=$(cat /proc/mz0380-periph-scan | grep -c ' = 00000000$')
	tot=$(cat /proc/mz0380-periph-scan | grep -c 'periph\[')
	echo "--- $chip: $nz of $tot registers read back zero ---"
done

echo
echo "=== verdict ==="
echo "A chip whose whole range reads 0x00 did not ACK. A chip with a varied"
echo "non-zero range is fitted and is currently running on power-on defaults,"
echo "because this driver has never written it."
echo
echo "=== mailbox errors during the scan, if any ==="
dmesg | grep -E "ret=-110|timed out|REG_READ" | tail -10
