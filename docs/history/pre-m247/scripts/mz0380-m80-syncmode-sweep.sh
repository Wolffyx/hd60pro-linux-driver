#!/bin/bash
# M80: embedded sync vs external sync - is the VIC scanning for timing codes
# that the receiver never emits?
#
# Everything about the link is now healthy: R55 locked, mode MATCHED (M79
# fixed the deep-colour rejection), SET_VIC accepted, encoder spawned. And the
# SoC's VIC still reports no signal on BT1120.
#
# BT1120 (and CCIR656) carry sync EMBEDDED in the data as SAV/EAV codes. Our
# receiver init ends with a write commented, in this driver's own source,
# "YUV422, 8-bit, external sync" - separate HS/VS/DE, no embedded codes. We
# then tell the VIC in_fmt=6 (BT1120p), which scans for embedded codes. If
# that pairing is wrong the VIC sees no valid sync no matter how clean the
# input is, which is exactly the symptom.
#
# The cfg enum (nullsensor_1920x1080.cfg) is:
#   1:8-bits Raw  2:CCIR656i  3:CCIR656p  4:Bayer  5:16-bits Raw
#   6:BT1120p     7:BT1120i
# 2/3/6/7 are all embedded-sync. 5 is the 16-bit external-sync sibling of 6
# and has never been tried; M76 swept only 6 and 3.
#
# vic_b0 rides along because BANK0 0xb0 is the receiver's output format/sync
# select: we write 0x21, hdcapm's only 0xb0 write is 0x14.
#
# Five passes, one encoder spawn each - inside the ~8-18 spawn wedge budget.
#
# Decision: any pass with NONZERO "pages touched" on a buffer OTHER than buf0,
# or a nonzero capture, is the answer. buf0 alone at 760/1024 is the card's
# NO SIGNAL splash and means that pass failed like all the others.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

# label:vic_in_fmt:vic_b0
CASES=${CASES:-"16bit-raw-extsync:5:0x21 \
16bit-raw-b0-hdcapm:5:0x14 \
bt1120p-b0-hdcapm:6:0x14 \
8bit-raw:1:0x21 \
ccir656i:2:0x21"}

for C in $CASES; do
	IFS=: read -r LABEL FMT B0 <<<"$C"
	echo
	echo "############################################################"
	echo "###  M80 $LABEL   (vic_in_fmt=$FMT vic_b0=$B0)"
	echo "############################################################"
	VICINFMT="$FMT" VICB0="$B0" WATCH=0 \
		scripts/mz0380-m55-real-capture.sh 1 2>&1 |
		grep -E "captured [0-9]+ bytes|pages touched|SET_VIC\(|link \[|output stage \[before|CARD WEDGED|No coherent"
	echo "--- (power-cycle the source again before the next pass) ---"
	sleep 3
done

echo
echo "=== sweep done. ==="
echo "=== buf[1..3] touched, or a nonzero capture => that pass is the answer. ==="
echo "=== buf[0] alone at 760/1024 in every pass => still the NO SIGNAL       ==="
echo "===   splash, and the sync-mode pairing is not the blocker either.      ==="
