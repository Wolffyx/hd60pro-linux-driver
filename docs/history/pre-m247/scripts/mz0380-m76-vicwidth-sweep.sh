#!/bin/bash
# M76: is the VIC rejecting the line because we declared the wrong sample count?
#
# vpl_vic.ko's ISR validates every incoming line against the VIC's width
# register and reports "(CCIR or width(%lu) chck fail)" on a mismatch; that is
# one of the three hardware status bits that surface to userspace as
# "[VIDEOCAP][ERROR]: No signal !!" (the others being FIFO-full and a genuine
# loss of sync). See RE_FINDINGS.md M76.
#
# The only host input that reaches that register is SET_VIC bytes 24..27.
# video_capture_mgr (FUN_0000a290) patches them into the generated cfg as
# "input frame width"/"input frame height", independently of the capture
# geometry, and libvideocap writes the value to VIC channel register +0x68.
#
# Hypothesis under test: the MST3367 is emitting 8-bit double-rate samples
# rather than true 16-bit BT1120, so the VIC counts 3840 samples per line
# where we have always declared 1920. Byte 7 (the cfg's "input format" enum)
# is swept alongside it, because 3=CCIR656p is the 8-bit sibling of
# 6=BT1120p and is the config the card would need for that bus width.
#
# Four passes, one encoder spawn each - well inside the ~8-18 spawn wedge.
#
# Decision: any pass reporting NONZERO "pages touched" (or a nonzero
# "captured N bytes") is the answer. All four zero eliminates the VIC width
# check as the cause and leaves the serial console as the next step.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

# label:input_frame_width:input_frame_height:input_format(0=auto 6/7)
CASES=${CASES:-"baseline-1920-bt1120p:0:0:0 \
doublerate-3840-bt1120p:3840:0:0 \
doublerate-3840-ccir656p:3840:0:3 \
ccir656p-1920:0:0:3"}

for C in $CASES; do
	IFS=: read -r LABEL W H FMT <<<"$C"
	echo
	echo "############################################################"
	echo "###  M76 $LABEL   (vic_in_w=$W vic_in_h=$H vic_in_fmt=$FMT)"
	echo "############################################################"
	VICINW="$W" VICINH="$H" VICINFMT="$FMT" WATCH=0 \
		scripts/mz0380-m55-real-capture.sh 2>&1 |
		grep -E "captured [0-9]+ bytes|pages touched|SET_VIC\(|MST3367 signal|CARD WEDGED|No coherent"
	echo "--- (power-cycle the source again before the next pass) ---"
	sleep 3
done

echo
echo "=== sweep done. ==="
echo "=== NONZERO 'pages touched' in any pass => the VIC width/format     ==="
echo "===   declaration was the blocker; that pass names the right value. ==="
echo "=== all zero => the width check is eliminated. The remaining        ==="
echo "===   discriminator is the card's serial console (which of the      ==="
echo "===   status bits 0x02/0x04/0x10 is set).                           ==="
