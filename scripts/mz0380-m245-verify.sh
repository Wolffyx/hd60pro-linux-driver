#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# M245 hardware checks for the review fixes. Each phase tests one fix
# deterministically; none of them is a "looks fine" check.
#
#   unbind  (0 spawns)  PCI unbind with a file handle held open on the node,
#                       then close it. Before M245 remove kfree'd the device
#                       while the handle's close still had to run against it.
#                       PASS = no Oops/BUG/general protection/KASAN in dmesg
#                       and the node comes back on rebind. Without KASAN a
#                       use-after-free need not crash, so this can catch the
#                       old defect but cannot prove its absence on its own.
#   ctrls   (0 spawns)  the control set: bitrate, GOP, and a readable
#                       power_present; none of the removed ones.
#   capture (1 spawn)   a raw capture while a SECOND process tries to change
#                       the format. S_FMT must fail with EBUSY (it used to
#                       succeed and re-lay-out the owner's frames) and a burst
#                       of TRY_FMTs must leave the owner's format untouched.
#
#   sudo scripts/mz0380-m245-verify.sh            # all three
#   sudo PHASES="unbind ctrls" scripts/mz0380-m245-verify.sh   # no spawns
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
command -v v4l2-ctl >/dev/null || { echo "v4l2-ctl is required"; exit 1; }

PHASES=${PHASES:-"unbind ctrls capture"}
FAIL=0

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

pass() { echo "  PASS: $*"; }
fail() { echo "  FAIL: $*"; FAIL=1; }

lsmod | grep -q '^mz0380' || ./mz0380-live.sh load >/dev/null || exit 1
sleep 3
NODE=$(node) || { echo "no mz0380 node"; exit 1; }
BDF=$(basename "$(readlink -f "/sys/class/video4linux/$(basename "$NODE")/device")")
echo "node $NODE on $BDF"

for phase in $PHASES; do
case "$phase" in
unbind)
	echo "=== unbind with an open handle (0 spawns) ==="
	dmesg -C
	exec 3<"$NODE" || { fail "could not open $NODE"; continue; }
	echo "$BDF" > /sys/bus/pci/drivers/mz0380/unbind
	sleep 1
	[ -e "$NODE" ] && fail "$NODE still exists after unbind" || pass "node gone while a handle is still open"
	exec 3<&-		# the close that used to run against freed memory
	sleep 1
	if dmesg | grep -qE "Oops|BUG:|general protection|KASAN|use-after-free|WARNING: CPU"; then
		fail "kernel reported a fault:"; dmesg | grep -E -A3 "Oops|BUG:|general protection|KASAN|use-after-free|WARNING: CPU" | head -20
	else
		pass "handle closed after unbind with no fault reported"
	fi
	echo "$BDF" > /sys/bus/pci/drivers/mz0380/bind
	sleep 4
	NODE=$(node) && pass "rebound as $NODE ($(dmesg | grep -o 'mz0380\[[0-9]*\]' | tail -1))" || fail "no node after rebind"
	;;
ctrls)
	echo "=== control set (0 spawns) ==="
	CTRLS=$(v4l2-ctl -d "$NODE" --list-ctrls 2>&1)
	echo "$CTRLS" | sed 's/^/    /'
	echo "$CTRLS" | grep -q "video_bitrate" && pass "bitrate present" || fail "bitrate missing"
	echo "$CTRLS" | grep -q "video_gop_size" && pass "GOP present" || fail "GOP missing"
	echo "$CTRLS" | grep -q "power_present" && pass "power_present present" || fail "power_present missing"
	echo "$CTRLS" | grep -qiE "brightness|contrast|saturation|hue|sharpness|record_mode|constant_quality|h264_profile|h264_level|b_frames|peak" \
		&& fail "a removed control is still listed" || pass "no dead controls"
	INPUTS=$(v4l2-ctl -d "$NODE" --list-inputs 2>&1 | grep -c "Input *:")
	[ "$INPUTS" = 1 ] && pass "one input" || fail "$INPUTS inputs listed"
	# power_present must agree with ENUM_INPUT's own live status - the first
	# M245 run read 0 with a locked source because the control used a flag
	# nothing had refreshed since the rebind.
	PWR=$(v4l2-ctl -d "$NODE" --get-ctrl=power_present 2>&1 | awk '{print $2}')
	if v4l2-ctl -d "$NODE" --list-inputs 2>&1 | grep -q "no signal"; then
		WANT=0x00000000
	else
		WANT=0x00000001
	fi
	[ "$PWR" = "$WANT" ] && pass "power_present $PWR agrees with the input status" \
		|| fail "power_present $PWR but the input status implies $WANT"
	;;
capture)
	echo "=== capture with a second process changing formats (1 spawn) ==="
	scripts/mz0380-node-holders.sh "$NODE"
	v4l2-ctl -d "$NODE" --set-fmt-video=width=1920,height=1080,pixelformat=YU12 >/dev/null
	OUT=/tmp/mz0380-m245-capture.i420
	timeout 60 v4l2-ctl -d "$NODE" --stream-mmap --stream-count=180 \
		--stream-to="$OUT" > /tmp/mz0380-m245-capture.log 2>&1 &
	CAP=$!
	sleep 2
	ERR=$(v4l2-ctl -d "$NODE" --set-fmt-video=pixelformat=NV12 2>&1)
	echo "$ERR" | grep -qi "busy" && pass "S_FMT from a second process refused: busy" \
		|| fail "S_FMT from a second process was not refused: $ERR"
	for i in $(seq 1 200); do
		v4l2-ctl -d "$NODE" --try-fmt-video=pixelformat=NV12 >/dev/null 2>&1
		v4l2-ctl -d "$NODE" --try-fmt-video=pixelformat=H264 >/dev/null 2>&1
	done
	FMT=$(v4l2-ctl -d "$NODE" --get-fmt-video 2>&1 | grep -o "'YU12'\|'NV12'\|'H264'\|'YV12'")
	[ "$FMT" = "'YU12'" ] && pass "owner's format still YU12 after 400 TRY_FMTs" || fail "format is now $FMT"
	wait "$CAP"
	BYTES=$(stat -c %s "$OUT" 2>/dev/null || echo 0)
	[ "$BYTES" = $((180 * 3110400)) ] && pass "180 whole frames captured" || fail "captured $BYTES bytes"
	python3 scripts/mz0380-m226-blackframe-scan.py --min-frames 180 < "$OUT" | grep -E "VERDICT|poison|partial" | sed 's/^/    /'
	scripts/mz0380-spawns.sh commit || true
	;;
esac
done

echo
[ "$FAIL" = 0 ] && echo "ALL PASS" || echo "SOME CHECKS FAILED"
exit $FAIL
