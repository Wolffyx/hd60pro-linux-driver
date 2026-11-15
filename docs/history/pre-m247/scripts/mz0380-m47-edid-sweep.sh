#!/bin/bash
# M47: find the opcode that actually gets an EDID into the card.
#
# State of play:
#   - The receiver is alive and configured (register read-back proves it), the
#     input is now selected before bring-up, HPD is asserted - and the source
#     still never transmits.
#   - The EDID is the one link we have NEVER verified. op 0x1f returned
#     success but stored nothing; op 0x20 returned -110, our own timeout,
#     while the very next command worked fine - i.e. the card posts no
#     completion for it, exactly like START_STREAMING (op 6, M22).
#   - We cannot verify by read-back: nothing answers at the DDC address on
#     this bus. THE SOURCE IS THE ORACLE - a camera switches to HDMI output
#     mode only once it can read a valid EDID.
#
# So sweep the plausible opcodes fire-and-forget and WATCH THE CAMERA.
#
# WHAT TO DO: keep the camera powered, HDMI cable in, its screen visible.
# For each variant the script pushes the EDID and re-pulses HPD, then waits.
# WATCH THE CAMERA'S SCREEN each time.
#
# THE ANSWER:
#   camera flips to HDMI-output mode on variant X  -> X is the working EDID
#       opcode. Tell me which one; it becomes the default and the receiver
#       should then lock (the script prints the detect row after each push).
#   camera never reacts, detect stays 55=03 hper=0000 vper=1fff
#       -> no host opcode in fw 1.11 can store an EDID. Remaining options:
#          (a) capture a live Windows I2C trace of UpdateEDID (also yields the
#              per-mode config values from M10 step 2 that we still lack),
#          (b) check whether the card's own userspace (yuan_ioctrl) serves the
#              EDID from flash and simply needs a different trigger.
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
	edid_opcode=0x20 edid_timeout_ms=0 || { echo "insmod failed"; exit 1; }
sleep 3

# force the bring-up (input select -> reset -> init -> EDID -> HPD)
v4l2-ctl -d /dev/video0 --query-dv-timings >/dev/null 2>&1
sleep 1

for op in 0x20 0x1f 0x1e 0x1b 0x1d; do
	echo "=================================================="
	echo " VARIANT opcode=$op  -- WATCH THE CAMERA SCREEN NOW"
	echo "=================================================="
	echo "$op" > /sys/module/mz0380/parameters/edid_opcode
	echo edid > /proc/mz0380-hdmi     # re-push EDID + re-pulse HPD
	sleep 6
	echo "--- detect after opcode $op ---"
	echo "watch 4" > /proc/mz0380-hdmi
	dmesg | grep -E 'detect 55=' | tail -2
	if dmesg | grep -q 'LOCKED'; then
		echo "*** LOCKED with opcode $op *** - stop here and report it"
		break
	fi
done

echo "=============== summary ==============="
dmesg | grep -E 'EDID pushed|EDID write failed|detect 55=|LOCKED' | tail -20
echo "--- did we ever lock? (0 = no) ---"
dmesg | grep -c 'LOCKED'
