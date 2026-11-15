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
# the M252 range test does. Each change is made the way a device swap looks to
# the card: kscreen-doctor turns the output off for three seconds, then on in
# the other mode. Switching the refresh rate with the link up does NOT work as
# a test: the receiver keeps its lock, frames keep flowing through the old
# encoder at the new rate, and the driver never measures the source again -
# the first M256 run changed 60 -> 30 Hz that way and nothing happened.
#
#   sudo scripts/mz0380-m256-mode-follow-test.sh
#   sudo MODE_B=1920x1080@50 scripts/mz0380-m256-mode-follow-test.sh
#   sudo REUSE=1 scripts/mz0380-m256-mode-follow-test.sh   # keep the loaded module
#
# Budget: THREE encoder spawns - the capture start and two followed changes
# (two with REUSE=1 when the loaded module's encoder already runs MODE_A raw).
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
# The session's Wayland socket is not always wayland-0 (it was wayland-1 on the
# day M256 was written), and kscreen-doctor fails silently on the wrong one -
# which reads as "no such output". Take the socket that exists.
WAYLAND=${WAYLAND:-$(basename "$(ls "/run/user/$UID_"/wayland-[0-9]* 2>/dev/null | grep -v '\.lock$' | head -1)")}
[ -n "$WAYLAND" ] || { echo "no Wayland socket in /run/user/$UID_ - run this from your desktop session"; exit 1; }
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
		WAYLAND_DISPLAY="$WAYLAND" "$@"
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
set_mode() {	# set_mode <WxH@R> - output on, in that mode
	as_user kscreen-doctor "output.$OUTPUT.enable" "output.$OUTPUT.mode.$1" > /dev/null 2>&1 ||
		bad "kscreen-doctor could not set $OUTPUT to $1"
}
swap_to() {	# swap_to <WxH@R> - what unplugging one device and plugging in another looks like
	as_user kscreen-doctor "output.$OUTPUT.disable" > /dev/null 2>&1 ||
		bad "kscreen-doctor could not turn $OUTPUT off"
	sleep 3
	set_mode "$1"
}
# The driver has to see the change before it can follow it.
wait_noticed() {	# wait_noticed <max seconds>
	local i
	for i in $(seq "$1"); do
		dmesg | grep -q "HDMI source changed to" && return 0
		sleep 1
	done
	return 1
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
if [ "${REUSE:-0}" = 1 ] && [ -d /sys/module/mz0380 ]; then
	echo "REUSE=1: keeping the loaded module (srcversion $(cat /sys/module/mz0380/srcversion))"
else
	SKIPSMOKE=1 ./mz0380-live.sh load > "$OUT/load.log" 2>&1 ||
		{ echo "load failed - see $OUT/load.log"; tail -5 "$OUT/load.log"; exit 1; }
fi
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
# Everything below is counted from here, so a reused module works too.
S0=$(spawns)
F0=$(follows)
echo "   baseline: $S0 spawn(s), $F0 automatic restart(s) used on this load"
[ $((ALLOW - F0)) -ge 2 ] ||
	{ bad "only $((ALLOW - F0)) automatic restart(s) left on this load - reload the module (drop REUSE=1)"; exit 1; }

follow_step() {	# follow_step <label> <mode> <n-th followed change>
	local want_f=$((F0 + $3)) want_s=$((S0 + $3))
	echo
	echo "=== $1: swap the source to $2 ==="
	dmesg -C
	swap_to "$2"
	wait_noticed 20 || {
		bad "$1: the driver never saw the source change in 20 s - nothing to follow ($(source_now))"
		return
	}
	t=$(wait_follows "$want_f" 30) &&
		ok "$1: encoder restarted by the driver ${t}s after it saw the change (mode follow $want_f of $ALLOW)" ||
		{ bad "$1: no automatic restart in 30 s (mode follow $(follows)); log:"
		  dmesg | grep -E "HDMI|encoder" | sed 's/^.*0000:[0-9a-f:.]* /     /' | tail -5; return; }
	t=$(frames_resume 20) && ok "$1: frames flowing again ${t}s later ($(source_now))" ||
		bad "$1: the encoder restarted but no frames followed in 20 s"
	[ "$(spawns)" = "$want_s" ] && ok "$1: one spawn spent ($want_s on this load)" ||
		bad "$1: spawns=$(spawns), expected $want_s"
	kill -0 "$FFPID" 2>/dev/null && ok "$1: the capture never ended" ||
		bad "$1: the capture ended: $(head -1 "$OUT/ffmpeg.log")"
	dmesg | grep -q "restarting the encoder for it (automatic restart $want_f of" ||
		bad "$1: the restart is not in the log"
}

follow_step "step 1" "$MODE_B" 1
follow_step "step 2" "$MODE_A" 2

echo
echo "=== step 3: allowance used up - source -> $MODE_B must hold NO SIGNAL ==="
echo "$(follows)" > /sys/module/mz0380/parameters/mode_follow
dmesg -C
before=$(spawns)
swap_to "$MODE_B"
wait_noticed 20 || bad "step 3: the driver never saw the source change"
sleep 12
[ "$(spawns)" = "$before" ] && ok "step 3: no spawn spent in 15 s" ||
	bad "step 3: spawns $before -> $(spawns) with the allowance used up"
dmesg | grep -q "automatic restarts of this load are used up" &&
	ok "step 3: the log says the allowance is used up" ||
	bad "step 3: no 'used up' line in the log"
state | grep -q "placeholder: SHOWING" && ok "step 3: NO SIGNAL is showing" ||
	bad "step 3: NO SIGNAL is not showing: $(state | sed -n 's/.*placeholder: //p' | cut -c1-40)"
swap_to "$MODE_A"
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
