#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# M238 verification: is the chroma-completeness check REACHABLE, and what does
# it do to a source that is genuinely colourless?
#
# M238 added mz0380_raw_probe_chroma_filled because a slot can hold a finished
# luma plane with its chroma planes still at the clear value, which delivers
# correct detail with a magenta or green cast.  Both of its counters read 0 in
# the only session that tested it, so the check is neither confirmed nor
# refuted: a counter that never moves cannot tell "wired and never needed"
# apart from "never evaluated".
#
# The falsifier is in the check's own documented false negative.  The chroma
# clear byte is 0x80, and 0x80 is ALSO neutral chroma in real content - black,
# white and every grey.  So a full-screen BLACK source must make all 32 chroma
# samples read exactly the clear value on every single frame:
#
#   chroma counter climbs on black  -> the check runs and its false negative is
#                                      PERSISTENT, not the one frame M238
#                                      claimed.
#   counter 0, probes 0             -> the check never evaluates. Dead code,
#                                      and the tinted frames need another
#                                      suspect.
#   counter 0, probes climbing      -> live test, never had cause to fire.
#                                      Read "best k/32": that is how close a
#                                      real frame came to looking cleared.
#
# The first run of this test could not tell those last two apart, because a
# rejection counter reading 0 says nothing about whether the code ran. M244
# added the probe count and the best-match high-water mark for exactly that,
# and the first run also showed why the black phase alone is not decisive: the
# operator\'s black measured Y=51 with chroma noise of plus or minus one, so
# only 11 of the 32 sample dwords were exactly 0x80 and the all-or-nothing test
# was right not to fire.
#
# Phase 2 repeats the capture on normal content for contrast, so one run
# separates "the check is off" from "black is special".
#
# COST: one encoder spawn per capture phase, so 2 by default.  Check
# scripts/mz0380-spawns.sh first.  The counter watch is started BEFORE the
# first consumer opens - that is also the M232 evidence the two existing
# traces lack, both of which started after the silence.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
command -v v4l2-ctl >/dev/null || { echo "v4l2-ctl is required"; exit 1; }

FRAMES=${FRAMES:-120}
PHASE2=${PHASE2:-1}
KEEPLOADED=${KEEPLOADED:-0}
WATCH_LOG=/tmp/mz0380-m238-watch.log
DMESG_OUT=/tmp/mz0380-m238-dmesg.txt
RAW1=/tmp/mz0380-m238-black.i420
RAW2=/tmp/mz0380-m238-content.i420
WATCH_PID=""
STATE=/proc/mz0380-state
# The phase prompts go to the TERMINAL, not to stdout.  Piping this script into
# tail (which is the natural thing to do with it) buffers stdout until the
# script exits, so an interactive prompt on stdout is invisible and the run
# looks frozen while it waits for an Enter the operator cannot see.
# PAUSE=<seconds> replaces the prompt with a fixed wait for an unattended run.
PAUSE=${PAUSE:-}

announce() { printf '%s\n' "$*" > /dev/tty 2>/dev/null || printf '%s\n' "$*" >&2; }

phase_gate() {
	local msg=$1
	announce ""
	announce "############################################################"
	announce "# $msg"
	if [ -n "$PAUSE" ]; then
		announce "# (PAUSE=$PAUSE - continuing in ${PAUSE}s)"
		announce "############################################################"
		sleep "$PAUSE"
		return 0
	fi
	announce "# Then press Enter here."
	announce "############################################################"
	if [ -r /dev/tty ]; then
		read -r _ < /dev/tty || true
	else
		read -r _ || true
	fi
}

cleanup() {
	[ -n "$WATCH_PID" ] && kill "$WATCH_PID" 2>/dev/null
	if [ "$KEEPLOADED" != 1 ]; then
		scripts/mz0380-spawns.sh commit >/dev/null 2>&1 || true
		timeout 30 ./mz0380-live.sh unload >/dev/null 2>&1 || \
			echo "WARNING: unload did not complete within 30s" >&2
	fi
	dmesg > "$DMESG_OUT" 2>/dev/null || true
	chmod 644 "$DMESG_OUT" "$WATCH_LOG" "$RAW1" "$RAW2" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

node() {
	local n
	for n in /sys/class/video4linux/video*/name; do
		[ -r "$n" ] || continue
		case "$(cat "$n")" in
		mz0380*|"HD60 Pro HDMI capture"*)
			echo "/dev/$(basename "$(dirname "$n")")"; return 0;;
		esac
	done
	return 1
}

fills() { sed -n 's/^  raw fills  : \([0-9]*\) [^,]*, \([0-9]*\) [^,]*, chroma test ran \([0-9]*\) times, best \([0-9]*\)\/\([0-9]*\).*/luma_clear=\1 chroma_clear=\2 chroma_probes=\3 best=\4\/\5/p' "$STATE"; }
delivered() { sed -n 's/^  raw frames : \([0-9]*\) delivered.*/\1/p' "$STATE"; }

capture() {
	local out=$1 label=$2 dev before_c after_c before_d after_d
	echo
	echo "=== phase: $label -> $out ==="
	dev=$(node) || { echo "no video node"; return 1; }
	# M244: a foreign consumer (WirePlumber probing the new node, a portal,
	# a leftover OBS) makes REQBUFS return EBUSY and the phase fail for a
	# reason that has nothing to do with what is being tested. Name it.
	scripts/mz0380-node-holders.sh "$dev"
	v4l2-ctl -d "$dev" --set-fmt-video=width=1920,height=1080,pixelformat=YU12 >/dev/null
	before_c=$(fills); before_d=$(delivered)
	echo "before: ${before_c:-<no raw counters yet>} delivered=${before_d:-0}"
	# One spawn.  --stream-count frames, mmap, straight to a file.
	timeout 60 v4l2-ctl -d "$dev" --stream-mmap --stream-count="$FRAMES" \
		--stream-to="$out" 2>&1 | tail -3
	after_c=$(fills); after_d=$(delivered)
	echo "after : ${after_c:-<no raw counters>} delivered=${after_d:-0}"
	echo "bytes : $(stat -c %s "$out" 2>/dev/null || echo 0) (expect $((FRAMES * 3110400)) for $FRAMES whole frames)"
	grep -E '^  pixelformat|^  raw ' "$STATE" || true
}

dmesg -C 2>/dev/null || true
./mz0380-live.sh load || exit 1
sleep 3

# BEFORE any consumer opens - M232 wants exactly this window in the trace.
scripts/mz0380-m230-flicker-watch.sh > "$WATCH_LOG" 2>&1 &
WATCH_PID=$!
sleep 3

phase_gate "Put the source on a FULL-SCREEN BLACK image."
capture "$RAW1" "black source (chroma == clear == 0x80)"

if [ "$PHASE2" = 1 ]; then
	phase_gate "Now put the source back on NORMAL COLOURED content."
	capture "$RAW2" "normal content (contrast)"
fi

sleep 2
kill "$WATCH_PID" 2>/dev/null; WATCH_PID=""

echo
echo "=== spawn tally ==="
scripts/mz0380-spawns.sh commit || true
echo
echo "=== counter watch (only lines where something changed) ==="
cat "$WATCH_LOG"
echo
echo "evidence: $WATCH_LOG $DMESG_OUT $RAW1 $RAW2"
