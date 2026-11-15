#!/bin/bash
# Load the driver and LEAVE IT LOADED, so OBS (or any V4L2 app) can open the
# node and you can watch what the card does.
#
#   sudo ./mz0380-live.sh load      # build, smoke-test, insmod, print node
#   sudo ./mz0380-live.sh status    # what is the card doing right now
#   sudo ./mz0380-live.sh watch     # follow the driver's log until Ctrl-C
#   sudo ./mz0380-live.sh unload    # kill users of the node, rmmod, tidy up
#
# Optional knobs (passed only when set, so the module defaults govern):
#   RAWDELIVER=0|1   startup format (default raw YU12)
#   PERSIST=0|1      keep the encoder running between captures (default 1)
#   H264DIVISOR=N    H.264 frame schedule (0 = every frame)
#   POSTSKIP=N       raw decimation (deliver one in N+1 frames)
#   KO=path          load this module instead of building the tree
#   TRACE=1          log every mailbox command (dynamic debug "mbx ...")
#   EXTRA="p=v ..."  any other module parameter
#
# The card wedges somewhere in 8-18 encoder spawns per power cycle; only a
# power cycle AT MAINS clears it. `status` shows the count.
set -u
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

ACTION=${1:-load}

find_node() {
	local name_file node
	for name_file in /sys/class/video4linux/video*/name; do
		[ -r "$name_file" ] || continue
		# M240 renamed the node; accept the old name so this script works
		# against a module built before or after that change.
		case "$(cat "$name_file")" in
		"HD60 Pro HDMI capture"|"mz0380 H.264") ;;
		*) continue ;;
		esac
		node=${name_file%/name}
		echo "/dev/${node##*/}"
		return 0
	done
	return 1
}

do_unload() {
	# The load path calls this after building and smoke-testing.  Preserve those
	# freshly generated files there; only an explicit user unload should remove
	# root-owned outputs that can obstruct the next non-root build.
	local cleanup_generated=${1:-1}
	local node
	node=$(find_node 2>/dev/null || true)
	if [ -n "$node" ]; then
		fuser -k "$node" 2>/dev/null && echo "killed the process holding $node"
		sleep 1
	fi
	if [ -d /sys/module/mz0380 ]; then
		scripts/mz0380-spawns.sh commit 2>/dev/null || true
	fi
	# M246: with enable_audio=1 the desktop audio server (WirePlumber) opens
	# the new ALSA card's control device the moment it appears, and an open
	# ALSA file pins the module - rmmod then refuses with "in use" for as
	# long as the session runs. Unbinding first takes the driver's normal
	# removal path, which disconnects the card; the server sees that, closes
	# its handle, and the reference is gone before rmmod asks.
	if [ "$(cat /sys/module/mz0380/refcnt 2>/dev/null || echo 0)" -gt 0 ]; then
		local dev
		for dev in /sys/bus/pci/drivers/mz0380/0000:*; do
			[ -e "$dev" ] || continue
			echo "unbinding $(basename "$dev") to release open handles (refcnt $(cat /sys/module/mz0380/refcnt))"
			echo "$(basename "$dev")" > /sys/bus/pci/drivers/mz0380/unbind
		done
		sleep 1
	fi
	if rmmod mz0380 2>/dev/null; then
		echo "module unloaded"
		scripts/mz0380-spawns.sh unloaded 2>/dev/null || true
	elif [ -d /sys/module/mz0380 ]; then
		echo "rmmod FAILED - state=$(cat /sys/module/mz0380/initstate 2>/dev/null)"
		echo "if that says 'going' the module is wedged; power-cycle at mains."
		return 1
	else
		echo "module was not loaded"
	fi
	if [ "$cleanup_generated" = 1 ]; then
		# Hardware harnesses run make as root and can leave generated outputs that
		# break the next non-root build with "Operation not permitted".  The
		# ordinary glob misses .module-common.o, so name it and the final ko.
		# src/ holds the objects since the 2026-08 reorg; the root-owned ones
		# there are just as fatal to the next non-root build as the top-level
		# ones, and a bare ./*.o no longer reaches them.
		rm -f ./*.o ./.module-common.o ./mz0380.ko \
		      ./src/*.o ./src/.*.o.cmd
	fi
	return 0
}

case "$ACTION" in
unload)
	do_unload 1
	exit $?
	;;

status)
	if [ ! -d /sys/module/mz0380 ]; then echo "not loaded"; exit 1; fi
	node=$(find_node 2>/dev/null || echo "(no node)")
	echo "node          : $node"
	echo "initstate     : $(cat /sys/module/mz0380/initstate 2>/dev/null)"
	echo "users         : $(cat /sys/module/mz0380/refcnt 2>/dev/null)"
	echo
	grep -E '^  (card|source|pixelformat|enc spawns|pipeline|placeholder|h264 frames|raw frames|recovery)' \
		/proc/mz0380-state 2>/dev/null
	printf '  command timeouts: %s\n' "$(dmesg | grep -c 'ret=-110')"
	echo
	echo "--- last driver lines ---"
	dmesg | grep mz0380 | tail -12
	;;

watch)
	echo "following the driver log - Ctrl-C to stop (module stays loaded)"
	dmesg -w | grep --line-buffered mz0380
	;;

load)
	. scripts/mz0380-build.sh
	MZKO=$(mz0380_resolve_module) || exit 1

	# M85: never leave a module loaded that cannot unload cleanly. An oops in
	# the exit path wedges it in MODULE_STATE_GOING, which no rmmod clears -
	# and this script deliberately leaves the module in place, so the check
	# matters more here than in m55. SKIPSMOKE=1 to bypass.
	if [ "${SKIPSMOKE:-0}" != 1 ]; then
		state=$(cat /sys/module/mz0380/initstate 2>/dev/null || true)
		if [ "$state" = going ]; then
			echo "mz0380 is wedged in MODULE_STATE_GOING - power-cycle at mains"
			exit 1
		fi
		scripts/mz0380-m85-unload-smoke.sh >/tmp/mz0380-smoke.log 2>&1 || {
			echo "load/unload smoke test FAILED - see /tmp/mz0380-smoke.log"
			tail -20 /tmp/mz0380-smoke.log
			exit 1
		}
		echo "(load/unload smoke test clean)"
	fi

	# Stop any previous instance, but retain MZKO resolved above: deleting the
	# just-built module here makes the following insmod fail with ENOENT.
	do_unload 0 >/dev/null 2>&1
	sleep 1
	modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm

	# M89: pass a knob ONLY when the caller set it, so the driver's own default
	# governs otherwise.
	OPTARGS=""
	add_opt() { [ -z "$2" ] || OPTARGS="$OPTARGS $1=$2"; }
	add_opt raw_deliver        "${RAWDELIVER:-}"
	add_opt persistent_h264    "${PERSIST:-}"
	add_opt h264_frame_divisor "${H264DIVISOR:-}"
	add_opt post_skip          "${POSTSKIP:-}"
	[ -n "${KO:-}" ] && MZKO=$KO

	dmesg -C
	# No quotes inside the dyndbg query: insmod wraps a value containing
	# spaces in "...", and a nested "mbx " splits it ("unclosed quote: mbx",
	# and nothing traced). '^' anchors the match to the start of the format.
	if [ "${TRACE:-0}" = 1 ]; then
		insmod "$MZKO" $OPTARGS ${EXTRA:-} dyndbg='format ^mbx +p' \
			|| { echo "insmod failed"; exit 1; }
	else
		insmod "$MZKO" $OPTARGS ${EXTRA:-} || { echo "insmod failed"; exit 1; }
	fi
	echo "insmod:$OPTARGS ${EXTRA:-}${TRACE:+ (mailbox trace on)}"

	# insmod is synchronous: it returns only after the PCI probe, firmware
	# handshake, DMA setup, and V4L2 registration have completed.  The old
	# unconditional 25-second sleep therefore delayed every healthy load even
	# when the card answered in 100 ms.  Keep a short bounded poll for sysfs/dev
	# publication races, but return as soon as the node exists.
	LOAD_WAIT_SECS=${LOADWAIT:-5}
	case "$LOAD_WAIT_SECS" in
		''|*[!0-9]*) echo "LOADWAIT must be a non-negative integer"; exit 1 ;;
	esac
	echo "waiting up to ${LOAD_WAIT_SECS}s for the video node..."
	deadline=$((SECONDS + LOAD_WAIT_SECS))
	NODE=""
	while [ -z "$NODE" ]; do
		NODE=$(find_node 2>/dev/null || true)
		[ -n "$NODE" ] && break
		[ "$SECONDS" -ge "$deadline" ] && break
		sleep 0.1
	done
	if [ -z "$NODE" ]; then
		echo "no video node appeared - driver log:"
		dmesg | grep mz0380 | tail -20
		exit 1
	fi

	echo
	dmesg | grep mz0380 | tail -6
	echo
	echo "=================================================================="
	echo " node: $NODE   (module stays loaded until you unload it)"
	echo "=================================================================="
	echo
	echo " ffplay: ffplay -f v4l2 -input_format yuv420p -video_size 1920x1080 $NODE"
	echo " OBS   : Video Capture Device (V4L2) -> $NODE, Video Format NV12"
	echo "         turn OFF 'Deactivate when not showing'; avoid retry loops -"
	echo "         every encoder restart spends one of ~8-18 spawns per power cycle."
	echo
	echo "   sudo ./mz0380-live.sh status    # signal, spawns, counters"
	echo "   sudo ./mz0380-live.sh watch     # follow the log live"
	echo "   sudo ./mz0380-live.sh unload    # when you are done"
	;;

*)
	echo "usage: $0 {load|status|watch|unload}"
	exit 1
	;;
esac
