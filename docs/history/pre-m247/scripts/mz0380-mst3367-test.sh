#!/bin/sh
# mz0380-mst3367-test.sh - MST3367 I2C-over-mailbox bring-up test (RE_FINDINGS M12)
#
# Protocol (verified both-sides disasm, docs/re-2026-07-05/):
#   read = op 0x1a, write = op 0x1b, dev = 8-BIT 0x9C (card does >>1 -> 0x4e),
#   banked regs: write reg 0x00 = bank first. Mode detect = BANK0 0x55 & 0x3c.
#
# Result semantics (M11, 0xEE-sentinel proven): on I2C failure the firmware
# FORCES the result slot (BAR0+0x10) to 0x00. So:
#     result == sentinel  -> handler never ran
#     result == 0x00      -> NAK (real I2C failure)
#     result != 0         -> ACK - first real byte from the MST3367
#
# M11 showed genuine NAKs at 0x9C; same card+fw works under Windows, so the
# receiver I2C domain is gated by something Windows does first. This script
# probes the gating candidates in order:
#   P1  baseline read (re-confirm NAK signature)
#   P2  GPIO bitmap read (op 0x14) - state snapshot before any pokes
#   P3  --with-vic: SET_VIC_PARAMS (input select) then re-probe
#   P4  GPL hdcapm init/reset sequence then re-probe + detect block
#
# Run as root with the module loaded:
#   modprobe videodev videobuf2-v4l2 videobuf2-dma-sg v4l2-dv-timings snd-pcm
#   insmod ./mz0380.ko dma_handshake=1 procfs_verbosity=2

set -u
CMD=/proc/mz0380-cmd
SCAN=/proc/mz0380-scan
PARAM=/sys/module/mz0380/parameters
SENT=0xee            # sentinel preloaded into the result slot

[ -w "$CMD" ] || { echo "ERROR: $CMD not writable (root? module loaded?)"; exit 1; }

# --- arg parse -------------------------------------------------------------
# --with-vic            fire SET_VIC (input select) before the I2C tests
# --with-vic            fire SET_VIC (input select) before the I2C tests
# --gpio-reset          run the Windows-exact receiver ungate (pin3 enable +
#                       pin8/9 reset pulse) before the I2C tests, then re-probe.
#                       This is the primary experiment now.
WITH_VIC=0
GPIO_RESET=0
for a in "$@"; do
	case "$a" in
	--with-vic)        WITH_VIC=1 ;;
	--gpio-reset)      GPIO_RESET=1 ;;
	esac
done

# point the raw scanner at the mailbox slots BAR0 0x00..0x5c
echo 0    > "$PARAM/scan_start"
echo 0x60 > "$PARAM/scan_len"

say()  { echo; echo "=== $* ==="; }
mb()   { echo "$*" > "$CMD"; sleep 0.05; }
last() { dmesg | grep "raw cmd" | tail -n 1; }
slot10() { grep "0x0010\]" "$SCAN" | head -n 1 | sed 's/^ *//'; }
# gpio_set <pin> <level>: op 0x15, Windows-exact single-pin semantics
#   word2 = mask = (1<<pin), word3 = data = (level<<pin).
# This touches ONLY <pin>; other pins (HPD=1, I2C=4-7) are untouched.
gpio_set() {
	mask=$((1 << $1)); data=$(($2 << $1))
	mb "$(printf '0x15 0x%x 0x%x' "$mask" "$data")"
	echo "    gpio pin $1 = $2 -> $(last)"
}

# receiver_reset: the Windows MST3367 ungate sequence (from e60MZ0380 RE).
#   pin3 = RX/mux enable (drive high, once)
#   pin9 = receiver reset, ACTIVE-LOW: pulse high->low->high (release = high)
#   pin8 = companion strap, driven low with pin9 low, high on release
# Direction is NOT set (Windows never issues op 0x17; pins pre-config'd output).
receiver_reset() {
	echo "  Windows ungate: pin3=RXenable, pin8/9 reset pulse (pin9 active-low)"
	gpio_set 3 1          # RX / mux enable
	gpio_set 9 1          # ensure released baseline
	sleep 0.02
	gpio_set 8 0          # companion strap low
	gpio_set 9 0          # ASSERT reset (low)
	sleep 0.05
	gpio_set 9 1          # RELEASE reset (high)
	gpio_set 8 1          # companion strap high
	sleep 0.10            # let the receiver's PLL/I2C come up
}

i2c_wr() { # i2c_wr <reg> <val>
	mb "0x1b 0x9c $1 $2"
	echo "  wr reg $1 = $2 -> $(last)"
}

i2c_rd() { # i2c_rd <reg> ; sentinel in the result slot, late re-read for safety
	mb "0x1a 0x9c $1 $SENT"
	echo "  rd reg $1 -> $(last)"
	sleep 0.2
	echo "    late $(slot10)   [${SENT}=handler-didnt-run, 00=NAK, else=ACK!]"
}

bank() { i2c_wr 0x00 "$1"; }

say "P0 sanity: GET_BOARD_VERSION (0x0a)"
mb "0x0a"
last

say "P0c FULL GPIO bitmap (op 0x14 mask 0xffffffff) - result at BAR0+0x0c"
mb "0x14 0xffffffff"
last

say "P0d BUS-LIVENESS: write reg0=0x02 then read reg0 back (0x02=bus alive, 00=dead)"
i2c_wr 0x00 0x02
i2c_rd 0x00
bank 0x00

say "P1 baseline: bank0 select + read 0x55 (expect NAK=00 per M11; nonzero = breakthrough)"
bank 0x00
i2c_rd 0x55

say "P2 GPIO bitmap read (op 0x14) - snapshot, no pokes"
mb "0x14 $SENT"
last
sleep 0.2
echo "  late $(slot10)"

if [ "$WITH_VIC" = "1" ]; then
	say "P3 input select: SET_VIC_PARAMS HDMI 1920x1080@60, then re-probe 0x9C"
	echo "2 1920 1080 60" > /proc/mz0380-hdmi
	sleep 1
	bank 0x00
	i2c_rd 0x55
	say "P3b GPIO bitmap after SET_VIC (diff vs P2 = pins the card itself moved)"
	mb "0x14 $SENT"
	last
fi

if [ "$GPIO_RESET" = "1" ]; then
	say "P3g RECEIVER RESET (Windows pin3/8/9 sequence) then re-probe 0x9C"
	receiver_reset
	bank 0x00
	echo "  re-probe 0x55 after receiver reset:"
	i2c_rd 0x55
	echo "  bus-liveness after reset (write reg0=0x02, read back):"
	i2c_wr 0x00 0x02
	i2c_rd 0x00
	bank 0x00
fi

say "P4 GPL init_setup (hdcapm) - writes NAK harmlessly if still gated"
bank 0x00
i2c_wr 0xb7 0x02        # HPD off (blind write, bit1 set)
i2c_wr 0x41 0x6f
i2c_wr 0xb8 0x00
bank 0x01
i2c_wr 0x0f 0x02
i2c_wr 0x16 0x30
i2c_wr 0x24 0x40        # HDCP receive
bank 0x00
i2c_wr 0xb0 0x14
i2c_wr 0xb1 0xe0
bank 0x02
i2c_wr 0x01 0x61
i2c_wr 0x02 0xf5
bank 0x00
i2c_wr 0x51 0x89
i2c_wr 0xb7 0x00        # HPD + link on
sleep 0.05
i2c_wr 0xb0 0x20        # YUV422 8-bit output

say "P4b HDMI reset (BANK2 0x07 f4->04) + HDCP reset (BANK0 0xb8 10->00)"
bank 0x02
i2c_wr 0x07 0xf4
i2c_wr 0x07 0x04
sleep 0.05
bank 0x00
i2c_wr 0xb8 0x10
i2c_wr 0xb8 0x00
sleep 0.05

say "P5 mode-detect block (BANK0; signal present if 0x55 & 0x3c)"
bank 0x00
for r in 0x55 0x6a 0x6b 0x5b 0x5c 0x57 0x58 0x59 0x5a 0x5f 0xb7; do
	i2c_rd $r
done
bank 0x02
i2c_rd 0x28
i2c_rd 0x29

say "done - dmesg tail"
dmesg | tail -n 60
