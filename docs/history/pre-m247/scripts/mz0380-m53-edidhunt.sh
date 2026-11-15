#!/bin/bash
# M53: is the EDID RAM behind an INDIRECT address/data port?
#
# M49b closed the direct search (no EDID-sized writable window in any bank),
# and M51/M51b closed the bit-bang search (see the decision notes there: the
# host-visible GPIOs have no pull-ups and nothing ACKs on any pair). What no
# scan so far could see is an indirect port - one register holding an
# address, a second reading/writing the byte there, with the RAM invisible
# to a register sweep. Two ordinary-looking config registers.
#
# The driver separates them by behaviour: address-indexed storage remembers
# a different byte per address, a plain register only the last byte written.
# Every register touched is restored.
#
# Decision table:
#   - "edidhunt HIT bank<N>: addr=0xAA data=0xDD" -> a window onto indexed
#     storage. Next: write the 256-byte EDID through it, re-pulse HPD
#     (echo "hpd 3 4000" | sudo tee /proc/mz0380-hdmi) and watch the camera.
#     If it switches to HDMI out, the blocker is dead.
#   - "NO indirect port found" -> the receiver holds no host-writable EDID
#     storage at all. Combined with M43 (no EEPROM on the receiver bus) and
#     M51b (no host-reachable second bus), every host-side EDID route is now
#     eliminated and the live Windows trace is the only remaining option.
#   - runs but the per-bank "writable candidates" line shows exactly the cap
#     -> raise it: insmod ... edidhunt_max_regs=96 (cost is quadratic).
set -u
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
MAXREGS=${1:-48}
make >/dev/null || { echo "build failed"; exit 1; }

# ALWAYS reload: a module left over from an earlier run is an older build
# that does not know this command, and the proc handler would have had to
# guess what we meant. (It now rejects unknown commands and says so.)
fuser -k /dev/video0 2>/dev/null
rmmod mz0380 2>/dev/null
sleep 1
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
	enable_video=1 procfs_verbosity=2 dma_iova_remap=1 \
	edidhunt_max_regs="$MAXREGS" || { echo "insmod failed"; exit 1; }
echo "waiting for the card to finish booting its own flash image..."
sleep 25

dmesg -C
echo "=== edidhunt (this takes a few minutes; 4 banks x quadratic pairs) ==="
echo "edidhunt" > /proc/mz0380-hdmi || { echo "proc write rejected"; dmesg | tail -3; exit 1; }
dmesg | grep -E "edidhunt" || { echo "no edidhunt output - see dmesg"; dmesg | tail -5; exit 1; }
