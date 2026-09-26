#!/bin/bash
# Load the driver and LEAVE IT LOADED, so OBS (or any V4L2 app) can open the
# node and you can watch what the card does. Companion to the m55 harness,
# which always unloads on exit.
#
#   sudo ./mz0380-live.sh load      # build, smoke-test, insmod, print node
#   sudo ./mz0380-live.sh status    # what is the card doing right now
#   sudo ./mz0380-live.sh watch     # follow the driver's log until Ctrl-C
#   sudo ./mz0380-live.sh unload    # kill users of the node, rmmod, tidy up
#
# Takes the same env knobs as m55 (POLLDRAIN VICFW VICB0 MSTB1 MSTB2 MSTB5
# POSTMASK POSTSKIP POSTAVG RAWDELIVER
# B0LATE KICKOP KICKREP OP6KICK WINSEQ INTX SETBUF ... plus EXTRA="p=v ...").
# POLLDRAIN remains available for the legacy raw-preview path. H264PROBE=1 uses
# the dedicated window-1 completion ring and does not need polling.
#
# READ THIS BEFORE YOU WATCH OBS
#
#   * With H264PROBE=1, persistent_h264 defaults on: the first STREAMON creates
#     one encoder and later OBS STREAMOFF/STREAMON cycles only detach/attach VB2.
#     A true HDMI timing change cleanly replaces the encoder once at the next
#     attachment. `status` prints both pipeline state and the SET_VIC count.
#   * With no HDMI source, the H.264 path now returns a host-owned NO SIGNAL
#     IDR at low cadence and spends no SET_VIC. Stable lock starts the card
#     pipeline once; unplug keeps that pipeline draining while the placeholder
#     is shown, and reconnect switches back only at a clean live IDR.
#   * When the card wedges, every command returns -110 and only a power cycle
#     AT MAINS clears it (slot standby survives a soft power-off).
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
	echo "--- receiver / signal (last few) ---"
	dmesg | grep -E 'MST3367 signal|detect 55=' | tail -5
	echo
	echo "--- encoder spawns this session (budget is ~8-18 per power cycle) ---"
	SET_VIC_COUNT=$(sed -n 's/^  enc spawns : \([0-9][0-9]*\).*/\1/p' \
		/proc/mz0380-state 2>/dev/null | head -1)
	if [ -z "$SET_VIC_COUNT" ]; then
		SET_VIC_COUNT=$(dmesg | grep -c 'stream start: SET_VIC')
	fi
	printf '  SET_VIC sent    : %s\n' "$SET_VIC_COUNT"
	if grep -q '^  h264 frames:' /proc/mz0380-state 2>/dev/null; then
		sed -n 's/^  pipeline[[:space:]]*:/  pipeline      :/p' /proc/mz0380-state
		sed -n 's/^  h264 frames:/  h264 frames   :/p' /proc/mz0380-state
		sed -n 's/^  no signal :/  no signal     :/p' /proc/mz0380-state
		sed -n 's/^  recovery  :/  recovery      :/p' /proc/mz0380-state
		sed -n 's/^  encoded rate:/  encoded rate  :/p' /proc/mz0380-state
	else
		printf '  frames delivered: %s\n' "$(dmesg | grep -c 'inferred payload length=')"
	fi
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
	# governs otherwise. Hardcoding "${VAR:-<literal>}" here silently overrides
	# the module default the moment the two drift apart.
	OPTARGS=""
	add_opt() { [ -z "$2" ] || OPTARGS="$OPTARGS $1=$2"; }
	add_opt vic_fw          "${VICFW:-}"
	add_opt vic_out_format  "${VICM:-}"
	add_opt vic_fast_kill   "${FASTKILL:-}"
	add_opt win_seq         "${WINSEQ:-}"
	add_opt irq_intx        "${INTX:-}"
	add_opt enc_sub         "${ENCSUB:-}"
	add_opt h264_frame_divisor "${H264DIVISOR:-}"
	add_opt win_start_op6   "${OP6:-}"
	add_opt win_bufs_first  "${WINBUFS:-}"
	add_opt set_buf_op8     "${OP8:-}"
	add_opt probe_windows   "${PROBEWIN:-}"
	add_opt h264_probe      "${H264PROBE:-}"
	add_opt persistent_h264 "${PERSIST:-}"
	add_opt raw_bank_probe  "${RAWBANKS:-}"
	add_opt raw_probe_enc_tail "${RAWTAIL:-}"
	add_opt raw_probe_allow_30 "${RAW30:-}"
	add_opt raw_bank_observe "${RAWOBS:-}"
	add_opt raw_deliver     "${RAWDELIVER:-}"
	add_opt post_mask       "${POSTMASK:-}"
	add_opt post_skip       "${POSTSKIP:-}"
	add_opt post_avg        "${POSTAVG:-}"
	add_opt mst_win_output  "${MSTOUT:-}"
	add_opt mst_ad          "${MSTAD:-}"
	add_opt vic_in_w        "${VICINW:-}"
	add_opt vic_in_h        "${VICINH:-}"
	add_opt vic_in_fmt      "${VICINFMT:-}"
	add_opt vic_b0          "${VICB0:-}"
	add_opt mst_b1          "${MSTB1:-}"
	add_opt mst_b2          "${MSTB2:-}"
	add_opt mst_b5          "${MSTB5:-}"
	add_opt mst_b0_late     "${B0LATE:-}"
	add_opt set_buf_opcode  "${SETBUF:-}"
	add_opt rx_strap        "${RXSTRAP:-}"
	add_opt poll_drain_credit "${POLLCREDIT:-}"
	add_opt op6_kick_ms     "${OP6KICK:-}"
	add_opt kick_opcode     "${KICKOP:-}"
	add_opt kick_repeat     "${KICKREP:-}"
	add_opt stream_without_signal "${NOSRC:-}"
	add_opt signal_monitor_ms "${SIGMON:-}"
	add_opt no_signal_fps "${NOSIGFPS:-}"
	# M216: no longer defaulted here. poll_drain_ms is 0 in the driver now,
	# which is right for the fw=7 completion-ring path; hardcoding 20 here
	# would silently override the module default, which is exactly what the
	# add_opt comment above warns against. POLLDRAIN=20 still selects the
	# legacy fw=5 poll-drain path.
	add_opt poll_drain_ms   "${POLLDRAIN:-}"

	dmesg -C
	insmod "$MZKO" dma_handshake=1 enable_dma=1 \
		enable_video=1 procfs_verbosity=2 dma_iova_remap=1 aic_on=1 \
		stream_nosg="${NOSG:-0}" force_timings=0 signal_poll_ms=4000 \
		$OPTARGS ${EXTRA:-} \
		|| { echo "insmod failed"; exit 1; }
	echo "insmod:$OPTARGS ${EXTRA:-}"

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
	dmesg | grep -E 'MST3367 signal|firmware|IRQ' | tail -6
	echo
	echo "=================================================================="
	echo " node: $NODE   (module stays loaded until you unload it)"
	echo "=================================================================="
	echo
	echo " In OBS: Video Capture Device (V4L2)  ->  device $NODE"
	echo "   - turn OFF 'Deactivate when not showing'"
	echo "   - turn OFF 'Use Buffering' for a low-latency preview"
	echo "   - if the preview stalls, do NOT let it retry in a loop:"
	echo "     every retry is one encoder spawn out of a budget of ~8-18."
	echo
	if [ "${H264PROBE:-0}" = 1 ]; then
		echo " H.264 window-1 capture is enabled. It delivers valid 1920x1080"
		if [ "${H264DIVISOR:-0}" = 0 ]; then
			echo " High Profile H.264. Validated all-frame bitmap mode is selected:"
			echo " a 60 fps HDMI input encodes at approximately 60 fps."
		else
			echo " High Profile H.264. Non-zero skip/divisor mode is selected;"
			echo " divisor 2 produces approximately 30 fps from a 60 Hz input."
		fi
	else
		echo " Expect ONE frame, then a frozen preview, showing the card's own"
		echo " monochrome NO SIGNAL splash. That is the default raw path."
	fi
	echo
	echo "   sudo ./mz0380-live.sh status    # signal, spawns, last log lines"
	echo "   sudo ./mz0380-live.sh watch     # follow the log live"
	echo "   sudo ./mz0380-live.sh unload    # when you are done"
	;;

*)
	echo "usage: $0 {load|status|watch|unload}"
	exit 1
	;;
esac
