#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# M247/M248 hardware check: the cleanup refactor, the receiver kept across a
# reload, and v4l2-compliance with and without a source. Cold boot first.
#
# Budget: 3 encoder spawns - one per traced load, one for the NO SIGNAL
# capture. H264=1 adds 2 more (raw -> H.264 -> raw, each a real replacement).
#
#   sudo scripts/mz0380-m247-verify.sh            # needs a 1080p HDMI source
#   sudo PAUSE=10 scripts/mz0380-m247-verify.sh   # no prompts, fixed waits
#   sudo scripts/mz0380-m247-verify.sh fourcc     # YUYV check only, module loaded
#   sudo scripts/mz0380-m247-verify.sh coldtrace  # cold bring-up trace, no power cycle, 0 spawns
#
# Phase 1  the card is told the same thing.
#   The SAME capture scenario runs on this tree and on the baseline module
#   (HEAD + a mailbox trace, ko/baseline/mz0380.ko), each logging every
#   mailbox command. This tree goes FIRST so that both loads bring the
#   receiver up from reset (the baseline always does; this tree keeps a
#   receiver a previous load set up - M248). The encoder opcodes must match.
# Phase 2  this tree again, so the receiver is kept: no hotplug edge at load,
#   the source locks at once, v4l2-compliance clean with the source present,
#   unsupported pixelformat handling, NO SIGNAL across an unplug, and
#   v4l2-compliance clean again with no source.
#
# Pass: no "!!" lines.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

OUT=${OUT:-/tmp/mz0380-m247}
BASELINE=${BASELINE:-ko/baseline/mz0380.ko}
PAUSE=${PAUSE:-}
FRAMES=${FRAMES:-150}
mkdir -p "$OUT"
FOLLOW=
trap '[ -n "$FOLLOW" ] && kill $FOLLOW 2>/dev/null' EXIT

announce() { printf '%s\n' "$*" > /dev/tty 2>/dev/null || printf '%s\n' "$*" >&2; }
wait_user() {
	announce "# $*"
	if [ -n "$PAUSE" ]; then
		sleep "$PAUSE"
		return
	fi
	announce "# press Enter to continue"
	read -r _ < /dev/tty 2>/dev/null || read -r _ || true
}
node() {
	local n
	for n in /sys/class/video4linux/video*/name; do
		[ "$(cat "$n" 2>/dev/null)" = "HD60 Pro HDMI capture" ] || continue
		n=${n%/name}
		echo "/dev/${n##*/}"
		return 0
	done
	return 1
}
state() { grep -E "^  ($1)" /proc/mz0380-state 2>/dev/null; }
counter() { state "$1" | grep -oE "[0-9]+ $2" | grep -oE '^[0-9]+'; }
capture() {	# capture <fourcc> <file>
	timeout 120 v4l2-ctl -d "$(node)" --set-fmt-video=width=1920,height=1080,pixelformat="$1" \
		--stream-mmap=4 --stream-count="$FRAMES" --stream-to="$2" 2>&1 | tail -1
}
# Seconds (wall clock) until QUERY_DV_TIMINGS reports a source. A query can
# itself block for the receiver's poll window, so iterations are no clock.
wait_lock() {	# wait_lock <max seconds>
	local n t0 ms
	n=$(node) || return 1
	t0=$(date +%s%3N)
	while :; do
		ms=$(( $(date +%s%3N) - t0 ))
		if timeout 10 v4l2-ctl -d "$n" --query-dv-timings 2>/dev/null | grep -q "Active width"; then
			printf '%d.%d\n' $((ms / 1000)) $((ms % 1000 / 100))
			return 0
		fi
		[ "$ms" -lt $(($1 * 1000)) ] || return 1
		sleep 0.5
	done
}
compliance() {	# compliance <log>
	timeout 300 v4l2-compliance -d "$(node)" > "$1" 2>&1
	grep -E "^Total|fail:" "$1" | sed 's/^/  /'
	grep -qE "^Total.*Failed: 0," "$1" || echo "  !! v4l2-compliance failures - see $1"
}

# TRY_FMT and S_FMT with a fourcc the node does not offer must leave the format
# alone (M247). Raw ioctls: v4l2-ctl checks the fourcc against ENUM_FMT itself
# and refuses before asking the driver ("The pixelformat 'YUYV' is invalid"),
# so the first two runs of this check never reached it. It starts from YU12,
# because the old bug answered H.264 - an H.264 start (OBS sets it) proves
# nothing - and puts the original format back. Zero spawns: nothing streams.
fourcc_check() {	# fourcc_check <node> <fourcc>
	python3 -I - "$1" "$2" <<'EOF'
import fcntl, struct, sys
# struct v4l2_format on 64-bit: type, pad to the 8-aligned union, the twelve
# u32 of v4l2_pix_format (width, height, pixelformat first), rest of the union
FMT = struct.Struct("I4x12I152x")
ioc = lambda nr: (3 << 30) | (FMT.size << 16) | (ord("V") << 8) | nr
G_FMT, S_FMT, TRY_FMT = ioc(4), ioc(5), ioc(64)
cc = lambda v: v.to_bytes(4, "little").decode("ascii", "replace")

def call(fd, req, pix):
    buf = bytearray(FMT.pack(1, *pix))       # V4L2_BUF_TYPE_VIDEO_CAPTURE
    fcntl.ioctl(fd, req, buf)
    return list(FMT.unpack(buf)[1:])

asked = int.from_bytes(sys.argv[2].encode(), "little")
yu12 = int.from_bytes(b"YU12", "little")
with open(sys.argv[1], "rb", buffering=0) as f:
    original = call(f.fileno(), G_FMT, [0] * 12)
    before = call(f.fileno(), S_FMT, original[:2] + [yu12] + original[3:])
    want = before[:2] + [asked] + before[3:]
    tried = call(f.fileno(), TRY_FMT, want)
    set_to = call(f.fileno(), S_FMT, want)
    after = call(f.fileno(), G_FMT, [0] * 12)
    call(f.fileno(), S_FMT, original)
print(f"  {cc(asked)}: TRY_FMT -> {cc(tried[2])}, S_FMT -> {cc(set_to[2])}; format "
      f"{cc(before[2])} {before[0]}x{before[1]} before, {cc(after[2])} {after[0]}x{after[1]} after")
if before[2] != yu12 or tried[2] != yu12 or set_to[2] != yu12 or after[:3] != before[:3]:
    print(f"  !! {cc(asked)} changed the format")
EOF
}

# The card answered its handshake, or the run is over: a deaf mailbox (M149,
# the wedge's second form - CMD_INIT -110 on the first load) refuses every
# capture, and nothing after this point would mean anything.
card_alive() {	# both this tree and the baseline log it; live.sh cleared the log at load
	dmesg | grep -q -E "card handshake failed|CMD_INIT got no answer" || return 0
	dmesg | grep -E "CMD_INIT got no answer" | tail -1 | sed 's/^/  /'
	echo "  !! card handshake failed (CMD_INIT -110): the card's mailbox is deaf."
	echo "     Wedged, or never power-cycled: switch the PSU off at the back (or"
	echo "     unplug it), wait 30 s, boot, and run this again. Nothing else recovers it."
	./mz0380-live.sh unload > /dev/null 2>&1
	exit 1
}

# A traced load: the log is followed live, because a mailbox trace overruns
# the 128 KiB ring before unload. trace_stop unloads and extracts the trace.
trace_load() {	# trace_load <label> <module>
	scripts/mz0380-node-holders.sh 2>/dev/null | grep -v "^no mz0380 node" | tail -2
	dmesg -C
	dmesg -w > "$OUT/$1-dmesg.log" 2>/dev/null &
	FOLLOW=$!
	KO="$2" TRACE=1 SKIPSMOKE=1 ./mz0380-live.sh load > "$OUT/$1-load.log" 2>&1 ||
		{ echo "load failed - see $OUT/$1-load.log"; exit 1; }
	card_alive
}
trace_stop() {	# trace_stop <label>
	./mz0380-live.sh unload > /dev/null 2>&1
	sleep 1
	kill $FOLLOW 2>/dev/null; wait $FOLLOW 2>/dev/null; FOLLOW=
	grep -o 'mbx op=.*' "$OUT/$1-dmesg.log" > "$OUT/$1.trace"
	# ret/status vary run to run (and with them nothing the card was told).
	sed -E 's/ ret=-?[0-9]+ status=[0-9a-f]+//' "$OUT/$1.trace" > "$OUT/$1.norm"
	echo "  $(wc -l < "$OUT/$1.trace") mailbox commands traced"
	[ -s "$OUT/$1.trace" ] || echo "  !! nothing traced - see $OUT/$1-dmesg.log"
}
trace_diff() {	# trace_diff <baseline label> <m247 label>
	diff "$OUT/$1.norm" "$OUT/$2.norm" > "$OUT/$2.diff"
	echo "  $(grep -c '^<' "$OUT/$2.diff") commands only in baseline, $(grep -c '^>' "$OUT/$2.diff") only in m247 (full diff: $OUT/$2.diff)"
	echo "  opcodes only in baseline: $(grep '^<' "$OUT/$2.diff" | grep -o 'op=0x..' | sort | uniq -c | tr -s ' \n' ' ')"
	echo "  opcodes only in m247    : $(grep '^>' "$OUT/$2.diff" | grep -o 'op=0x..' | sort | uniq -c | tr -s ' \n' ' ')"
}

# One load, two captures (the second is a reattachment), unload.
trace_run() {	# trace_run <label> <module>
	local t
	echo "=== phase 1 [$1]: $2 ==="
	trace_load "$1" "$2"
	if t=$(wait_lock 30); then
		echo "  source locked ${t}s after load"
	else
		echo "  !! no source within 30s of load"
	fi
	echo "  capture 1: $(capture YU12 "$OUT/$1-1.yuv")"
	echo "  capture 2: $(capture YU12 "$OUT/$1-2.yuv")"
	state 'enc spawns|pipeline|raw frames|raw fills|raw repeats|raw rhythm' | sed 's/^/ /'
	trace_stop "$1"
}

# `fourcc`: just the unsupported-pixelformat check on the loaded module.
if [ "${1:-}" = fourcc ]; then
	N=$(node) || { echo "no HD60 Pro node - load the module first"; exit 1; }
	fourcc_check "$N" YUYV
	exit 0
fi

# `coldtrace`: this tree's cold bring-up next to the baseline's, without a
# power cycle and with zero spawns (loads only, nothing streams). A reload
# keeps the receiver (M248), so the baseline's raw mailbox passthrough breaks
# the bring-up signature first: BANK0 0xad 0x05 -> 0x04 (the low-pass filter,
# which the cold path writes back). The source sees two hot-plug drops.
if [ "${1:-}" = coldtrace ]; then
	[ -f "$BASELINE" ] || { echo "no baseline module at $BASELINE"; exit 1; }
	make >/dev/null || { echo "build failed"; exit 1; }
	./mz0380-live.sh unload > /dev/null 2>&1
	echo "=== baseline cold bring-up: $BASELINE ==="
	trace_load cold-baseline "$BASELINE"
	sleep 2
	echo "0x1b 0x9c 0x00 0x00" > /proc/mz0380-cmd	# BANK0
	echo "0x1b 0x9c 0xad 0x04" > /proc/mz0380-cmd	# signature broken
	trace_stop cold-baseline
	echo "=== this tree, forced cold: ./mz0380.ko ==="
	trace_load cold-m247 ./mz0380.ko
	sleep 2
	trace_stop cold-m247
	if grep -q "HDMI receiver kept" "$OUT/cold-m247-dmesg.log"; then
		echo "  !! the receiver was kept - the signature write did not land"
	fi
	grep -q "mbx op=0x15" "$OUT/cold-m247.trace" ||
		echo "  !! no GPIO writes in this tree's trace - it did not reset the receiver"
	echo
	echo "=== cold bring-up diff (baseline -> m247) ==="
	# Leave our two writes out of the baseline's side. 0xad=0x04 is unique -
	# bring-up writes 0x05 - and the bank select is the line before it.
	L=$(grep -n 'op=0x1b .*args\[3\]=9c 00 00 00 ad 00 00 00 04 00 00 00' \
		"$OUT/cold-baseline.norm" | head -1 | cut -d: -f1)
	if [ -n "$L" ] && [ "$L" -gt 1 ]; then
		sed -i "$((L - 1)),${L}d" "$OUT/cold-baseline.norm"
		echo "  (the 2 signature-breaking writes cut from the baseline trace)"
	else
		echo "  (the signature-breaking writes are not in the baseline trace - nothing cut)"
	fi
	trace_diff cold-baseline cold-m247
	echo "  expected: baseline-only = its dir_invert=0 reset pass and read-backs;"
	echo "            m247-only = one bank select + the 0xad signature read"
	echo "artefacts in $OUT - cold-*.trace, cold-m247.diff"
	exit 0
fi

[ -f "$BASELINE" ] || { echo "no baseline module at $BASELINE"; exit 1; }
make >/dev/null || { echo "build failed"; exit 1; }
SPAWNS=$(scripts/mz0380-spawns.sh 2>/dev/null | grep -oE 'cycle: [0-9]+' | grep -oE '[0-9]+')
scripts/mz0380-spawns.sh 2>/dev/null | tail -3
[ "${SPAWNS:-0}" = 0 ] ||
	echo "  NOTE: not a fresh power cycle - the m247 load may keep the receiver, so its trace has no bring-up to compare"

trace_run m247 ./mz0380.ko
trace_run baseline "$BASELINE"

echo
echo "=== phase 1 result: mailbox command diff (baseline -> m247) ==="
trace_diff baseline m247
for op in 0x29 0x2d 0x31 0x02 0x08 0x04 0x2a 0x06 0x07; do
	a=$(grep -c "op=$op" "$OUT/baseline.norm"); b=$(grep -c "op=$op" "$OUT/m247.norm")
	[ "$a" = "$b" ] || echo "  !! op $op count differs: baseline $a, m247 $b - check $OUT/m247.diff"
	diff <(grep "op=$op" "$OUT/baseline.norm") <(grep "op=$op" "$OUT/m247.norm") > /dev/null ||
		echo "  !! op $op arguments differ - check $OUT/m247.diff"
done

echo
echo "=== phase 2: this tree, receiver kept; zero spawns unless stated ==="
announce "# watch the camera during this load: it should stay in HDMI output"
dmesg -C
dmesg -w > "$OUT/ux-dmesg.log" 2>/dev/null &
FOLLOW=$!
./mz0380-live.sh load > "$OUT/ux-load.log" 2>&1 || { echo "load failed"; exit 1; }
card_alive
sleep 0.5
LOADS=$(grep -c "CMD_INIT answered" "$OUT/ux-dmesg.log")
KEPT=$(grep -c "HDMI receiver kept from the previous load" "$OUT/ux-dmesg.log")
echo "  receiver kept at $KEPT of $LOADS loads (live.sh smoke test + real)"
[ "$KEPT" -gt 0 ] && [ "$KEPT" = "$LOADS" ] ||
	echo "  !! the receiver was reset at load - see $OUT/ux-dmesg.log"
if t=$(wait_lock 30); then
	echo "  source locked ${t}s after load (it took 3-7s when every load reset the receiver)"
	[ "${t%%.*}" -lt 2 ] || echo "  !! slow lock after a kept receiver - did the camera leave HDMI mode?"
else
	echo "  !! no source within 30s of load"
fi
N=$(node)

echo "--- v4l2-compliance, source present (no streaming) ---"
compliance "$OUT/compliance-signal.log"
echo "--- unsupported pixelformat keeps the format ---"
fourcc_check "$N" YUYV
echo "--- log-status ---"
timeout 10 v4l2-ctl -d "$N" --log-status > /dev/null 2>&1
dmesg | grep -A8 "START STATUS" | tail -9

echo "--- NO SIGNAL on a raw node (one spawn: the first capture starts the encoder) ---"
v4l2-ctl -d "$N" --set-fmt-video=width=1920,height=1080,pixelformat=YU12 \
	--stream-mmap=4 --stream-count=100000 --stream-to=/dev/null > /dev/null 2>&1 &
CAP=$!
sleep 4
SP0=$(counter 'enc spawns' 'since')
wait_user "UNPLUG the HDMI cable now (capture keeps running)"
sleep 5
state 'placeholder|recovery' | sed 's/^/ /'
NS1=$(counter placeholder 'raw NO SIGNAL')
wait_user "PLUG the HDMI cable back in"
sleep 6
state 'placeholder|recovery|raw frames|enc spawns' | sed 's/^/ /'
NS2=$(counter placeholder 'raw NO SIGNAL')
SP1=$(counter 'enc spawns' 'since')
SHOWN=$(state placeholder | grep -c "not shown")
kill $CAP 2>/dev/null; wait $CAP 2>/dev/null
[ "${NS1:-0}" -gt 0 ] || echo "  !! no raw NO SIGNAL frames while unplugged"
[ "$SHOWN" = 1 ] || echo "  !! placeholder still shown after replug"
[ "${SP0:-x}" = "${SP1:-y}" ] || echo "  !! spawns changed across the unplug: $SP0 -> $SP1"
echo "  raw NO SIGNAL frames: $NS1 while unplugged, $NS2 after replug"
dmesg | grep -E "HDMI signal" | tail -4 | sed 's/^/ /'

echo "--- v4l2-compliance, no source ---"
wait_user "UNPLUG the HDMI cable again"
sleep 3
compliance "$OUT/compliance-nosignal.log"
wait_user "PLUG the HDMI cable back in"

if [ "${H264:-0}" = 1 ]; then
	echo "--- H.264 and back (two spawns: each switch replaces the encoder) ---"
	wait_lock 30 > /dev/null || echo "  !! no source within 30s of replug"
	echo "  H.264  : $(capture H264 "$OUT/h264.h264")"
	echo "  raw    : $(capture NV12 "$OUT/nv12.yuv")"
	state 'enc spawns' | sed 's/^/ /'
fi

./mz0380-live.sh unload > /dev/null 2>&1
sleep 1
kill $FOLLOW 2>/dev/null; wait $FOLLOW 2>/dev/null; FOLLOW=
scripts/mz0380-spawns.sh 2>/dev/null | tail -2
echo
echo "artefacts in $OUT - send m247.diff, compliance-*.log and *-dmesg.log"
