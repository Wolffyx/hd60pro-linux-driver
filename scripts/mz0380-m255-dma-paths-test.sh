#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# M255: do the two DMA paths from pull request #3 work when they are taken?
#
# Neither runs on a healthy machine. The fragmented-page fallback only runs
# when a contiguous allocation fails, and the STREAMON refusal only when PCI
# bus mastering has been cleared behind the driver's back. The fault_inject
# module parameter makes each reachable on demand, without fragmenting host
# memory and without writing the card's PCI command register.
#
#   sudo scripts/mz0380-m255-dma-paths-test.sh           # phases 1-3, ZERO spawns
#   sudo scripts/mz0380-m255-dma-paths-test.sh capture   # + phase 4, TWO spawns
#
# Phase 1  the unwind. fault_inject=2 fails each fragmented mapping half-way.
#   The load must survive, report streaming unavailable, and refuse STREAMON.
# Phase 2  the fallback. fault_inject=1 maps the H.264 slots from fragmented
#   pages. This load reuses phase 1's IOVAs, so a mapping phase 1 leaked shows
#   up here as "already mapped". v4l2-compliance must stay clean.
# Phase 3  the bus-master refusal. fault_inject=3 makes STREAMON read bus
#   mastering as off: it must fail with EIO, count the refusal, spend no spawn
#   and leave the queue usable (v4l2-compliance again).
# Phase 4  (capture) real frames through the fragmented slots: 300 raw frames
#   scanned for blanks, then 300 H.264 frames that must decode. Raw is the
#   first capture after a load (one spawn), H.264 a format switch (one more).
#   Needs a steady 1080p source: it waits for a lock before spending anything,
#   and says so if the source dropped out while it was capturing.
#
# The script ends by reloading the module with fault_inject=0.
# Pass: no "!!" lines. OBS closed, as for every hardware run.
set -u
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
MODE=${1:-}
case "$MODE" in ''|capture) ;; *) echo "usage: $0 [capture]"; exit 1;; esac
for tool in v4l2-ctl v4l2-compliance ffmpeg; do
	command -v "$tool" >/dev/null || { echo "$tool not found (v4l-utils, ffmpeg)"; exit 1; }
done
if [ "$MODE" = capture ]; then
	for tool in ffprobe python3; do
		command -v "$tool" >/dev/null || { echo "$tool not found"; exit 1; }
	done
fi

# Every ffmpeg here runs with -nostdin and stdin closed. Under `timeout` it is
# in a background process group, and one that touches the terminal is stopped
# by SIGTTOU before it opens the device - it then sits there until the timeout
# kills it, having captured nothing (the first M255 runs lost phase 4 to this).
FF="ffmpeg -nostdin -hide_banner -loglevel error"

OUT=$(mktemp -d /tmp/mz0380-m255.XXXXXX)
chmod 755 "$OUT"
FAILS=0
bad() { echo "!! $*"; FAILS=$((FAILS + 1)); }
ok() { echo "ok $*"; }

state() { cat /proc/mz0380-state 2>/dev/null; }
spawns() { state | sed -n 's/.*enc spawns : \([0-9]*\).*/\1/p'; }
refusals() { state | sed -n 's/.*; \([0-9]*\) capture start(s) refused.*/\1/p'; }
node() {
	grep -l 'HD60 Pro HDMI capture' /sys/class/video4linux/video*/name 2>/dev/null |
		sed 's|/sys/class/video4linux/|/dev/|; s|/name||' | head -1
}

load() {	# load <label> <fault_inject>
	EXTRA="fault_inject=$2" SKIPSMOKE=1 ./mz0380-live.sh load > "$OUT/$1-load.log" 2>&1 ||
		{ bad "$1: load failed - see $OUT/$1-load.log"; tail -5 "$OUT/$1-load.log"; return 1; }
	sleep 1
	NODE=$(node)
	[ -n "$NODE" ] || { bad "$1: no video node"; return 1; }
}

# Anything the kernel itself complains about. The one IO_PAGE_FAULT at
# 0x90000000 is the card's pre-ready boot artifact (RE_FINDINGS M26).
kernel_clean() {	# kernel_clean <label>
	dmesg > "$OUT/$1-dmesg.log"
	if grep -E "WARNING:|BUG:|Oops|general protection|already mapped" "$OUT/$1-dmesg.log" |
	   grep -v "^$" > "$OUT/$1-kernel.bad"; then
		bad "$1: kernel complaints:"; sed 's/^/     /' "$OUT/$1-kernel.bad"
		return
	fi
	if grep "IO_PAGE_FAULT" "$OUT/$1-dmesg.log" | grep -v "address=0x90000000" > "$OUT/$1-fault.bad"; then
		bad "$1: IOMMU faults:"; sed 's/^/     /' "$OUT/$1-fault.bad"
		return
	fi
	ok "$1: no kernel warnings, no unexpected IOMMU faults"
}

# One STREAMON, bounded, output kept. ffmpeg rather than v4l2-ctl: v4l2-ctl
# queries the DV timings first and, with no source locked at that instant,
# waits for one without ever calling STREAMON - the first M255 run failed
# phase 3 that way, with an empty log and no refusal counted.
streamon() {	# streamon <label>
	timeout 15 $FF -f v4l2 -input_format yuv420p -video_size 1920x1080 \
		-i "$NODE" -frames:v 1 -f null - < /dev/null > "$OUT/$1-streamon.log" 2>&1
}

# Phase 4 judges frames, so it needs a source that stays put. Wait for a lock,
# and afterwards refuse to blame the driver for a source that dropped out.
wait_source() {	# wait_source <max seconds> - prints the mode once locked
	local i
	for i in $(seq "$1"); do
		if timeout 10 v4l2-ctl -d "$NODE" --query-dv-timings 2>/dev/null | grep -q "Active width"; then
			state | sed -n 's/.*source     : //p'
			return 0
		fi
		sleep 1
	done
	return 1
}
source_dropped() {	# true if the log since the last dmesg -C shows a loss
	dmesg | grep -qE "HDMI signal lost|HDMI source: no signal|started with no HDMI signal|no frames for"
}

compliance() {	# compliance <label>
	v4l2-compliance -d "$NODE" > "$OUT/$1-compliance.log" 2>&1
	if grep -q "Failed: 0, Warnings: 0" "$OUT/$1-compliance.log"; then
		ok "$1: v4l2-compliance $(grep '^Total' "$OUT/$1-compliance.log" | sed 's/.*video[0-9]*: //')"
	else
		bad "$1: v4l2-compliance: $(grep '^Total' "$OUT/$1-compliance.log")"
	fi
}

echo "=== phase 1: unwind of a mapping that fails half-way (fault_inject=2) ==="
if load p1 2; then
	state | grep -q "streaming UNAVAILABLE" &&
		ok "p1: load survived, streaming reported unavailable" ||
		bad "p1: streaming is not reported unavailable: $(state | grep 'card ')"
	n=$(dmesg | grep -c "fragmented mapping at IOVA .* failed")
	[ "$n" -ge 1 ] && ok "p1: $n half-way failure(s) logged and unwound" ||
		bad "p1: the injected failure was never logged"
	streamon p1
	grep -q "No such device" "$OUT/p1-streamon.log" &&
		ok "p1: STREAMON refused with ENODEV" ||
		bad "p1: STREAMON was not refused with ENODEV: $(tail -1 "$OUT/p1-streamon.log")"
	[ "$(spawns)" = 0 ] && ok "p1: 0 spawns" || bad "p1: spawns=$(spawns), expected 0"
	kernel_clean p1
fi

echo
echo "=== phase 2: fragmented slots, same IOVAs (fault_inject=1) ==="
if load p2 1; then
	state | grep "dma slots" | sed 's/^ */   /'
	state | grep -q "streaming available" && ok "p2: streaming available" ||
		bad "p2: streaming unavailable - a phase 1 mapping may have leaked: $(dmesg | grep -m1 'already mapped')"
	state | grep -q "H.264 0 contiguous + 4 fragmented" &&
		ok "p2: all four H.264 slots are fragmented" ||
		bad "p2: H.264 slots are not all fragmented"
	compliance p2
	[ "$(spawns)" = 0 ] && ok "p2: 0 spawns" || bad "p2: spawns=$(spawns), expected 0"
	kernel_clean p2

	echo
	echo "=== phase 3: STREAMON with bus mastering read as off (fault_inject=3) ==="
	before=$(refusals)
	echo 3 > /sys/module/mz0380/parameters/fault_inject
	dmesg -C
	streamon p3
	echo 1 > /sys/module/mz0380/parameters/fault_inject
	grep -q "Input/output error" "$OUT/p3-streamon.log" &&
		ok "p3: STREAMON refused with EIO" ||
		bad "p3: STREAMON was not refused with EIO: $(tail -1 "$OUT/p3-streamon.log")"
	dmesg | grep -q "capture refused: PCI bus mastering" &&
		ok "p3: the refusal is logged" || bad "p3: the refusal is not in the log"
	[ "$(refusals)" = $((before + 1)) ] && ok "p3: refusal counter $before -> $(refusals)" ||
		bad "p3: refusal counter $before -> $(refusals), expected +1"
	[ "$(spawns)" = 0 ] && ok "p3: 0 spawns" || bad "p3: spawns=$(spawns), expected 0"
	compliance p3-after
	kernel_clean p3
fi

phase4() {
	local mode frames errs
	echo
	echo "=== phase 4: real frames through the fragmented slots (2 spawns) ==="
	mode=$(wait_source 20) || {
		bad "p4: no HDMI source locked after 20 s - not spending spawns; connect a steady 1080p source and rerun"
		return
	}
	echo "   source: $mode"
	dmesg -C

	$FF -f v4l2 -input_format yuv420p -video_size 1920x1080 -i "$NODE" \
		-frames:v 300 -f rawvideo - < /dev/null 2>"$OUT/p4-raw.err" |
		python3 scripts/mz0380-m226-blackframe-scan.py > "$OUT/p4-raw-scan.log" 2>&1
	grep -q "^frames read *: 300" "$OUT/p4-raw-scan.log" &&
		ok "p4: 300 raw frames read" ||
		bad "p4: raw capture short: $(grep '^frames read' "$OUT/p4-raw-scan.log") $(head -1 "$OUT/p4-raw.err")"
	grep -q "^blank frames *: 0 " "$OUT/p4-raw-scan.log" &&
		grep -q "^partial fills *: 0" "$OUT/p4-raw-scan.log" &&
		ok "p4: no blank or partial raw frame" ||
		bad "p4: $(grep -E '^(blank frames|partial fills)' "$OUT/p4-raw-scan.log" | tr '\n' ' ')"

	timeout 40 $FF -y -f v4l2 -input_format h264 -i "$NODE" -frames:v 300 \
		-c copy "$OUT/p4.mkv" < /dev/null 2>"$OUT/p4-h264.err"
	frames=$(ffprobe -v error -count_frames -select_streams v:0 \
		-show_entries stream=nb_read_frames -of csv=p=0 "$OUT/p4.mkv" 2>/dev/null)
	[ "${frames:-0}" -ge 290 ] && ok "p4: $frames H.264 frames recorded" ||
		bad "p4: only ${frames:-0} H.264 frames recorded: $(head -1 "$OUT/p4-h264.err")"
	errs=$($FF -i "$OUT/p4.mkv" -f null - < /dev/null 2>&1 | wc -l)
	[ "$errs" = 0 ] && ok "p4: the H.264 stream decodes with no error" ||
		bad "p4: $errs decoder error line(s) in the H.264 stream"
	state | grep -E "h264 frames|raw frames|dma slots|enc spawns" | sed 's/^ */   /'
	[ "$(spawns)" = 2 ] && ok "p4: 2 spawns, as budgeted" ||
		bad "p4: spawns=$(spawns), expected 2 - something else opened the node? (scripts/mz0380-node-holders.sh)"
	if source_dropped; then
		bad "p4: the HDMI source dropped out during the captures - the frame results above are not the driver's; rerun with a steady source:"
		dmesg | grep -E "HDMI signal|HDMI source|no frames for" | sed 's/^.*0000:[0-9a-f:.]* /     /' | tail -6
	fi
	kernel_clean p4
}

if [ "$MODE" = capture ] && [ -n "${NODE:-}" ] && [ "$FAILS" = 0 ]; then
	phase4
elif [ "$MODE" = capture ]; then
	echo
	echo "phase 4 skipped: an earlier phase failed, not spending spawns on it"
fi

echo
echo "=== restore: reload with fault_inject=0 ==="
if load restore 0; then
	state | grep "dma slots" | sed 's/^ */   /'
	state | grep -q "H.264 4 contiguous + 0 fragmented" && state | grep -q "fault_inject=0" &&
		ok "restore: contiguous slots, fault_inject=0" ||
		bad "restore: the module is not back to its default mapping"
fi

echo
chmod -R a+rX "$OUT"
if [ "$FAILS" = 0 ]; then
	echo "PASS - no '!!' lines. Logs: $OUT"
else
	echo "FAIL - $FAILS '!!' line(s). Logs: $OUT"
fi
exit "$FAILS"
