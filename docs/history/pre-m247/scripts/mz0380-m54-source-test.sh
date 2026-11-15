#!/bin/bash
# M54: is the card sending the trigger, and does THIS source answer it?
#
# The trigger a source waits for is two things: HPD asserted on the
# connector, then a readable EDID over DDC. HPD is ours to drive (GPIO pin1
# + receiver BANK0 0xb7); the EDID is the unsolved blocker. This script
# separates "we are not triggering" from "we trigger and the source refuses
# for lack of EDID", and then watches whether the source transmits anyway.
#
# Why a different source matters: many devices transmit with no readable
# EDID at all, and those would light up the whole capture path today.
#   GOOD candidates: Raspberry Pi with hdmi_force_hotplug=1 (+ hdmi_group/
#     hdmi_mode set) - it drives HDMI without reading an EDID; cheap media
#     players / set-top boxes / DVD players; some action cams.
#   BAD candidates: PCs and laptops (they refuse without EDID, same as the
#     DSLR), and anything that insists on HDCP.
#
# Decision table:
#   1. "MST3367 receiver brought up" and NOT "still silent" -> receiver is
#      alive; the GPIO-direction damage from the M51 probe is repaired.
#   2. "HPD asserted -> pin1 reads 1" -> the card IS driving the trigger.
#      A "PIN DID NOT FOLLOW" line means it is not, and nothing downstream
#      matters until that is fixed.
#   3. In the watch output: any change in the detect block after the source
#      is plugged/powered = the source started transmitting -> real capture
#      is now testable (stream_nosg=0). A frozen block = the source is
#      staying dark, i.e. it wants the EDID.
#
# Usage: run it, and when it says so, power-cycle / re-plug the source.
set -u
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
WATCH=${1:-40}
make >/dev/null || { echo "build failed"; exit 1; }

fuser -k /dev/video0 2>/dev/null
rmmod mz0380 2>/dev/null
sleep 1
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
insmod ./mz0380.ko dma_handshake=1 enable_dma=1 \
	enable_video=1 procfs_verbosity=2 dma_iova_remap=1 \
	|| { echo "insmod failed"; exit 1; }
echo "waiting for the card to finish booting its own flash image..."
sleep 25

echo "=== 1. receiver alive + HPD actually driven? ==="
dmesg -C
echo "edid" > /proc/mz0380-hdmi     # bring-up + EDID push + HPD cycle
sleep 3
dmesg | grep -E "MST3367|HPD|EDID|silent|answering"

echo
echo "=== 2. HPD pulses (watch the source now) ==="
dmesg -C
echo "hpd 3 4000" > /proc/mz0380-hdmi
sleep 14
dmesg | grep -E "HPD"

echo
echo "=== 3. is the source transmitting? (${WATCH}s) ==="
echo "    PLUG IN / POWER ON the source now if it is not already."
dmesg -C
echo "watch $WATCH" > /proc/mz0380-hdmi
dmesg | grep -E "MST3367|detect|locked|signal" | tail -40
