#!/bin/bash
# M173: do R55 bits 0-1 track the HDMI cable?
#
# M45 asked this and never answered it. mz0380-mst3367.c has carried the guess
# ever since - "we consistently read 0x03 - bits the driver does not use, most
# plausibly 5V/clock presence" - and M45's own note said "the natural reading is
# 5V/cable presence and/or clock detect".
#
# It matters because V4L2_CID_DV_RX_POWER_PRESENT is the last thing between this
# driver and a clean v4l2-compliance warning column, and that control is the
# standard way an application tells "nothing plugged in" from "plugged in but
# not transmitting". Backing it with a hypothesis would export a guess to every
# client that reads it, in the one place a client is entitled to trust.
#
# Two single reads are not enough: they are indistinguishable unless you already
# know which one had the cable in, and that is precisely the ambiguity that
# makes a wrong conclusion easy. So this watches continuously and prints every
# CHANGE, and you do the unplugging while it runs.
#
# Costs ZERO encoder spawns and needs NO root - /proc/mz0380-hdmi is
# world-readable and reading it does a live receiver read. The module must
# already be loaded.
set -u
cd "$(dirname "$0")/.."

SECS=${1:-40}
[ -r /proc/mz0380-hdmi ] || {
	echo "/proc/mz0380-hdmi not readable - is the module loaded?"
	exit 1
}

r55() {
	sed -n 's/.*[^0-9a-f]55=\([0-9a-f][0-9a-f]\).*/\1/p' /proc/mz0380-hdmi | head -1
}

echo "Watching R55 for ${SECS}s. While this runs:"
echo "  1. UNPLUG the HDMI cable from the card"
echo "  2. wait ~5s"
echo "  3. PLUG IT BACK IN"
echo
echo "What decides it: bits 0-1 (mask 0x03) of R55."
echo "  they drop on unplug and return on replug -> 5V/cable presence CONFIRMED"
echo "  they stay put                            -> the guess is WRONG; the"
echo "                                              warning stays and M45's"
echo "                                              reading is finally dead"
echo

END=$(( $(date +%s) + SECS ))
LAST=
CHANGES=0
SEEN=""
while [ "$(date +%s)" -lt "$END" ]; do
	V=$(r55)
	if [ -n "$V" ] && [ "$V" != "$LAST" ]; then
		LOW=$(( 0x$V & 0x03 ))
		LOCK=$(( 0x$V & 0x3c ))
		printf '  %s  R55=0x%s   bits0-1=%d   lock(0x3c)=0x%02x\n' \
			"$(date +%H:%M:%S)" "$V" "$LOW" "$LOCK"
		[ -z "$LAST" ] || CHANGES=$((CHANGES + 1))
		case " $SEEN " in *" $LOW "*) ;; *) SEEN="$SEEN $LOW";; esac
		LAST=$V
	fi
	sleep 0.25
done

echo
echo "R55 changed $CHANGES time(s); bits0-1 took the value(s):$SEEN"
NVALS=$(echo $SEEN | wc -w)
if [ "$NVALS" -gt 1 ]; then
	echo "VERDICT: bits 0-1 MOVED. If they moved with the cable, M45's reading"
	echo "         is confirmed and V4L2_CID_DV_RX_POWER_PRESENT can be backed"
	echo "         by R55 bit 0."
elif [ "$CHANGES" = 0 ]; then
	echo "VERDICT: NOTHING changed at all - not even the lock bits. Either the"
	echo "         cable was never actually out, or this read is not live."
	echo "         Check that the lock bits move first; if they do not, the"
	echo "         low bits proving nothing is not evidence about the low bits."
else
	echo "VERDICT: the lock bits moved but bits 0-1 did NOT. They are not cable"
	echo "         presence. M45's guess is dead - do not wire"
	echo "         V4L2_CID_DV_RX_POWER_PRESENT to them."
fi
