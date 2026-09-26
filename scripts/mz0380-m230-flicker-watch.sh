#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# M230: watch the counters that separate the possible causes of the SECOND
# flicker - the 0.5-to-1-second black the operator still sees in OBS on the
# non-YU12 formats after M229 closed the single-frame blanks.
#
# That duration is 30 to 60 consecutive frames at 60 fps, so it is not the
# defect M226 measured, and a 1200-frame capture found no blanks at all. The
# candidates make DIFFERENT counters move, which is the whole point of watching
# rather than guessing:
#
#   pixelformat flips to H264   -> OBS is not on the raw path for that format.
#                                  BGR3/YV12 come from libv4l2 emulation, which
#                                  may fall back to H264 instead of converting
#                                  from YU12. Then this is an H.264 defect and
#                                  nothing about raw applies.
#   raw dropped climbing        -> buffer starvation; the node had no vb2 buffer
#                                  to deliver into.
#   raw fills climbing hard     -> M229 is rejecting nearly every slot, so the
#                                  card stopped finishing fills.
#   placeholder IDRs delivered  -> the NO SIGNAL replay is on screen. At
#                                  no_signal_fps=2 that blacks the picture for
#                                  exactly this kind of duration.
#   h264 dropped / 0 SPS+PPS    -> the encoded path is broken, which is the
#                                  known post-raw-session defect.
#   nothing moves at all        -> the driver delivered normally throughout and
#                                  the black is downstream of it, in libv4l2 or
#                                  in OBS.
#
# Prints a line only when something CHANGES, so a quiet stream stays quiet and
# the moment of a flicker is obvious in the log.
#
#   ./scripts/mz0380-m230-flicker-watch.sh          # until Ctrl-C
#   ./scripts/mz0380-m230-flicker-watch.sh | tee /tmp/m230.log
#
# Needs no root and touches no hardware - it reads /proc and nothing else, so
# it costs no encoder spawn.
set -u
STATE=/proc/mz0380-state
# M236: one second, not 0.2. Reading /proc/mz0380-state is not free - at
# procfs_verbosity=3 it issues five mailbox commands, and even below that it
# walks the whole device state. Polling five times a second against a running
# capture froze the machine for about a second at a time. Override with
# INTERVAL= if a finer trace is genuinely needed, and keep procfs_verbosity
# below 3 while capturing.
INTERVAL=${INTERVAL:-1}

[ -r "$STATE" ] || { echo "$STATE not readable - is the driver loaded?"; exit 1; }

# One number per line, in a fixed order, so a plain string compare detects any
# change without parsing each field twice.
sample() {
	sed -n \
		-e 's/^  pixelformat: \(.*\)$/fmt=\1/p' \
		-e 's/^  raw frames : \([0-9]*\) delivered, \([0-9]*\) dropped.*/rawdeliv=\1 rawdrop=\2/p' \
		-e 's/^  raw fills  : \([0-9]*\) [^,]*, \([0-9]*\) .*/rawunfilled=\1 rawnochroma=\2/p' \
		-e 's/^  raw repeats: \([0-9]*\) identical-head[^,]*, \([0-9]*\) torn.*/rawdup=\1 rawtorn=\2/p' \
		-e 's/^  rearms     : \([0-9]*\) .*/rearms=\1/p' \
		-e 's/^  placeholder: \([^,]*\), \([0-9]*\) NO SIGNAL IDRs delivered, \([0-9]*\) cadence misses, \([0-9]*\).*/ph=\1 phdeliv=\2 phmiss=\3 phheld=\4/p' \
		-e 's/^  h264 frames: \([0-9]*\) delivered, \([0-9]*\) dropped.*/h264deliv=\1 h264drop=\2/p' \
		-e 's/^  pipeline   : \([^,]*\), \([^,]*\), [^,]*, \([0-9]*\) cached SPS\/PPS.*/pipe=\1 vb2=\2 sps=\3/p' \
		-e 's/^  recovery  : \([^,]*\),.*/recov=\1/p' \
		"$STATE" | tr '\n' ' '
}

echo "watching $STATE every ${INTERVAL}s - printing only on change. Ctrl-C to stop."
echo "Reproduce the flicker in OBS, then read the lines around the moment it went black."
echo

prev=""
start=$SECONDS
while :; do
	cur=$(sample)
	if [ "$cur" != "$prev" ]; then
		printf '[%6ss] %s\n' "$((SECONDS - start))" "$cur"
		prev=$cur
	fi
	sleep "$INTERVAL"
done
