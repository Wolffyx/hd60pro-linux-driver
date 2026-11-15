#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# M256: does a running capture follow a source that changes frame rate?
#
# Before M256 a source that came back in another mode left the NO SIGNAL
# picture up until the application restarted the capture. Now the driver
# restarts the encoder itself when the new mode has the same frame size, has
# held for a few seconds, and the per-load allowance (mode_follow) is not used
# up. This test changes the mode under one long capture and watches for that.
#
# Needs the GPU's HDMI output looped into the card (KDE output HDMI-A-1), as
# the M252 range test does; kscreen-doctor switches its refresh rate.
#
#   sudo scripts/mz0380-m256-mode-follow-test.sh
#   sudo MODE_B=1920x1080@50 scripts/mz0380-m256-mode-follow-test.sh
#
# Budget: THREE encoder spawns - the capture start and two followed changes.
# It refuses to start when that would take the power cycle's tally past 8
# (FORCE=1 overrides).
#
# Step 1  capture MODE_A, then switch the source to MODE_B. The encoder must
#   be restarted for it without the capture ending: counter 0 -> 1, frames
#   flowing again, one more spawn.
# Step 2  switch back to MODE_A: followed again, counter 1 -> 2.
# Step 3  the allowance. With mode_follow set to what is already used, a switch
#   to MODE_B must NOT restart the encoder: NO SIGNAL holds and no spawn is
#   spent. Switching back to MODE_A must then cancel the pending replacement
#   and resume frames, still with no spawn.
#
# Pass: no "!!" lines. OBS closed.
set -u
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "run as root (sudo, from your desktop session)"; exit 1; }
U=${SUDO_USER:-}
[ -n "$U" ] || { echo "run it with sudo from your desktop session"; exit 1; }
UID_=$(id -u "$U")
OUTPUT=${OUTPUT:-HDMI-A-1}
MODE_A=${MODE_A:-1920x1080@60}
MODE_B=${MODE_B:-1920x1080@30}
for tool in v4l2-ctl ffmpeg kscreen-doctor; do
	command -v "$tool" >/dev/null || { echo "$tool not found"; exit 1; }
done

OUT=$(mktemp -d /tmp/mz0380-m256.XXXXXX)
chmod 755 "$OUT"
FAILS=0
FFPID=
bad() { echo "!! $*"; FAILS=$((FAILS + 1)); }
ok() { echo "ok $*"; }

as_user() {
	sudo -u "$U" env XDG_RUNTIME_DIR="/run/user/$UID_" \
		DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$UID_/bus" \
		WAYLAND_DISPLAY="${WAYLAND:-wayland-0}" "$@"
}
state() { cat /proc/mz0380-state 2>/dev/null; }
spawns() { state | sed -n 's/.*enc spawns : \([0-9]*\).*/\1/p'; }
follows() { state | sed -n 's/.*mode follow: \([0-9]*\) of.*/\1/p'; }
raw_frames() { state | sed -n 's/.*raw frames : \([0-9]*\) delivered.*/\1/p'; }
source_now() { state | sed -n 's/.*source     : //p'; }
node() {
	grep -l 'HD60 Pro HDMI capture' /sys/class/video4linux/video*/name 2>/dev/null |
		sed 's|/sys/class/video4linux/|/dev/|; s|/name||' | head -1
}
set_mode() {	# set_mode <WxH@R>
	as_user kscreen-doctor "output.$OUTPUT.mode.$1" > /dev/null 2>&1 ||
		bad "kscreen-doctor could not set $OUTPUT to $1"
}
# Seconds until the raw frame counter has grown by 30 from now, or failure.
frames_resume() {	# frames_resume <max seconds>
	local start now i
	start=$(raw_frames)
	for i in $(seq "$1"); do
		sleep 1
		now=$(raw_frames)
		[ $((now - start)) -ge 30 ] && { echo "$i"; return 0; }
	done
	return 1
}
# Wait for the follow counter to reach a value.
wait_follows() {	# wait_follows <value> <max seconds>
	local i
	for i in $(seq "$2"); do
		[ "$(follows)" = "$1" ] && { echo "$i"; return 0; }
		sleep 1
	done
	return 1
}
cleanup() {
	[ -n "$FFPID" ] && kill "$FFPID" 2>/dev/null && wait "$FFPID" 2>/dev/null
	set_mode "$MODE_A"
	[ -w /sys/module/mz0380/parameters/mode_follow ] &&
		echo "${ALLOW:-4}" > /sys/module/mz0380/parameters/mode_follow
}
trap cleanup EXIT

as_user kscreen-doctor -o 2>/dev/null | grep -q " $OUTPUT " ||
	{ echo "no KDE output $OUTPUT - loop the GPU's HDMI into the card first"; exit 1; }
pgrep -x obs > /dev/null && { echo "OBS is running - close it first"; exit 1; }

tally=$(scripts/mz0380-spawns.sh 2>/dev/null | sed -n 's/.*power cycle: \([0-9]*\).*/\1/p')
echo "spawns this power cycle: ${tally:-unknown}; this test spends 3"
if [ "${FORCE:-0}" != 1 ] && [ "${tally:-0}" -gt 5 ]; then
	echo "that would pass 8, where the card has wedged before. Power-cycle first, or FORCE=1."
	exit 1
fi

set_mode "$MODE_A"
SKIPSMOKE=1 ./mz0380-live.sh load > "$OUT/load.log" 2>&1 ||
	{ echo "load failed - see $OUT/load.log"; tail -5 "$OUT/load.log"; exit 1; }
NODE=$(node)
[ -n "$NODE" ] || { echo "no video node"; exit 1; }
ALLOW=$(cat /sys/module/mz0380/parameters/mode_follow)
for i in $(seq 20); do
	timeout 10 v4l2-ctl -d "$NODE" --query-dv-timings 2>/dev/null | grep -q "Active width" && break
	sleep 1
done
echo "source: $(source_now)   allowance: $ALLOW automatic restart(s) per load"

# One capture for the whole test: it must survive every change below.
ffmpeg -nostdin -hide_banner -loglevel error -f v4l2 -input_format yuv420p \
	-video_size 1920x1080 -i "$NODE" -t 180 -f null - < /dev/null > "$OUT/ffmpeg.log" 2>&1 &
FFPID=$!
t=$(frames_resume 20) && ok "capture running on $MODE_A after ${t}s ($(source_now))" ||
	{ bad "no frames from $MODE_A after 20 s - nothing to follow from"; exit 1; }
[ "$(spawns)" = 1 ] || bad "spawns=$(spawns) after the capture start, expected 1"

follow_step() {	# follow_step <label> <mode> <expected counter> <expected spawns>
	echo
	echo "=== $1: source -> $2 ==="
	dmesg -C
	set_mode "$2"
	t=$(wait_follows "$3" 30) &&
		ok "$1: encoder restarted by the driver after ${t}s (mode follow $3 of $ALLOW)" ||
		{ bad "$1: no automatic restart in 30 s (mode follow $(follows)); log:"
		  dmesg | grep -E "HDMI|encoder" | sed 's/^.*0000:[0-9a-f:.]* /     /' | tail -5; return; }
	t=$(frames_resume 20) && ok "$1: frames flowing again ${t}s later ($(source_now))" ||
		bad "$1: the encoder restarted but no frames followed in 20 s"
	[ "$(spawns)" = "$4" ] && ok "$1: $4 spawns" || bad "$1: spawns=$(spawns), expected $4"
	kill -0 "$FFPID" 2>/dev/null && ok "$1: the capture never ended" ||
		bad "$1: the capture ended: $(head -1 "$OUT/ffmpeg.log")"
	dmesg | grep -q "restarting the encoder for it (automatic restart $3 of" ||
		bad "$1: the restart is not in the log"
}

follow_step "step 1" "$MODE_B" 1 2
follow_step "step 2" "$MODE_A" 2 3

echo
echo "=== step 3: allowance used up - source -> $MODE_B must hold NO SIGNAL ==="
echo "$(follows)" > /sys/module/mz0380/parameters/mode_follow
dmesg -C
before=$(spawns)
set_mode "$MODE_B"
sleep 15
[ "$(spawns)" = "$before" ] && ok "step 3: no spawn spent in 15 s" ||
	bad "step 3: spawns $before -> $(spawns) with the allowance used up"
dmesg | grep -q "automatic restarts of this load are used up" &&
	ok "step 3: the log says the allowance is used up" ||
	bad "step 3: no 'used up' line in the log"
state | grep -q "placeholder: SHOWING" && ok "step 3: NO SIGNAL is showing" ||
	bad "step 3: NO SIGNAL is not showing: $(state | sed -n 's/.*placeholder: //p' | cut -c1-40)"
set_mode "$MODE_A"
t=$(frames_resume 30) && ok "step 3: back on $MODE_A, frames resumed after ${t}s" ||
	bad "step 3: frames did not resume on $MODE_A"
[ "$(spawns)" = "$before" ] && ok "step 3: still $before spawns - the pending replacement was cancelled, not carried out" ||
	bad "step 3: spawns $before -> $(spawns)"
kill -0 "$FFPID" 2>/dev/null && ok "step 3: the capture never ended" ||
	bad "step 3: the capture ended: $(head -1 "$OUT/ffmpeg.log")"

echo
dmesg > "$OUT/dmesg.log"
grep -E "WARNING:|BUG:|Oops" "$OUT/dmesg.log" > "$OUT/kernel.bad" &&
	{ bad "kernel complaints:"; sed 's/^/     /' "$OUT/kernel.bad"; }
state | grep -E "enc spawns|mode follow|raw frames|placeholder" | sed 's/^ */   /'
chmod -R a+rX "$OUT"
if [ "$FAILS" = 0 ]; then
	echo "PASS - no '!!' lines. Logs: $OUT"
else
	echo "FAIL - $FAILS '!!' line(s). Logs: $OUT"
fi
exit "$FAILS"
