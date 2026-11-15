#!/bin/bash
# M55: real HDMI capture. The normal path requires a coherent receiver lock
# and uses BT1120 -> SSM -> H.264. NOSG=1 remains available only as an
# explicit card-side fake-frame/DMA diagnostic.
#
# M54 found one: a digital microscope drives the wire and the MST3367
# reached lock (detect 0x55=0xa3 LOCKED, htot=0x898=2200 = the horizontal
# total of 1080p). The DSLR still stays dark, which is the EDID blocker and
# is not what this script tests.
#
# Lock can be intermittent while the source settles, so timings are queried
# throughout the source power-cycle window. Unmatched/torn counters are never
# coerced to a guessed video mode.
#
# Decision table:
#   - "MST3367 signal:" followed by valid H.264 NAL units with NOSG=0 -> real
#     capture works end-to-end.
#   - query-dv-timings keeps returning ENOLCK -> the source dropped lock;
#     power-cycle it, or raise signal_poll_ms.
#   - timings fine but 0 bytes captured -> the encoder armed but no frame
#     completed: compare with the nosg path (m50), which does deliver, and
#     look for 'stream start' + enc_stat lines in dmesg.
#   - frames arrive but decode to garbage -> the per-mode receiver config
#     (M10 step 2, still unrecovered) is wrong for this source's mode.
set -u
cd "$(dirname "$0")/.."

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

find_mz_video_node() {
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

VIDEO_NODE=

# M60: leave the machine as we found it. An insmod'd module left behind kept
# the card armed between runs and made the next run's "reload" a no-op against
# stale code.
cleanup() {
	local node=${VIDEO_NODE:-}

	[ -n "$node" ] || node=$(find_mz_video_node 2>/dev/null || true)
	[ -z "$node" ] || fuser -k "$node" 2>/dev/null
	# M169: bank this run's encoder spawns BEFORE the module goes away and
	# takes the count with it. In cleanup rather than at the end of the
	# script so that an early exit or a Ctrl-C still counts the spawns it
	# already spent - those are the runs where knowing the budget matters
	# most. /proc/mz0380-state resets at every insmod and this tree
	# reinsmods per run, so nothing else accumulates it.
	scripts/mz0380-spawns.sh commit 2>/dev/null || true
	rmmod mz0380 2>/dev/null && echo "(module unloaded)"
}
trap cleanup EXIT INT TERM
# M60: the NOSG diagnostic respawns the encoder for every frame and the card
# wedges after roughly 8-18 spawns. Real capture keeps one encoder alive for
# the whole stream, but retain the conservative default for comparable tests.
FRAMES=${1:-6}
LOCKWAIT=${2:-45}
# Real capture is the default. NOSG=1 deliberately bypasses HDMI/BT1120 and
# must never be used to decide whether source pixels are reaching the card.
NOSG=${NOSG:-0}
. scripts/mz0380-build.sh
MZKO=$(mz0380_resolve_module) || exit 1

# M85: never spend a hardware run on a module that cannot unload cleanly.
# An oops in the exit path wedges the module in MODULE_STATE_GOING, which
# no rmmod (not even -f) can clear - it costs a full power-cycle. The check
# is a few seconds and zero encoder spawns. SKIPSMOKE=1 to bypass.
if [ "${SKIPSMOKE:-0}" != 1 ]; then
	state=$(cat /sys/module/mz0380/initstate 2>/dev/null || true)
	if [ "$state" = going ]; then
		echo "mz0380 is wedged in MODULE_STATE_GOING - power-cycle required"; exit 1
	fi
	scripts/mz0380-m85-unload-smoke.sh >/tmp/mz0380-smoke.log 2>&1 || {
		echo "load/unload smoke test FAILED - see /tmp/mz0380-smoke.log"
		tail -20 /tmp/mz0380-smoke.log
		exit 1
	}
	echo "(load/unload smoke test clean)"
fi

VIDEO_NODE=$(find_mz_video_node 2>/dev/null || true)
[ -z "$VIDEO_NODE" ] || fuser -k "$VIDEO_NODE" 2>/dev/null
rmmod mz0380 2>/dev/null
VIDEO_NODE=
sleep 1
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
# M89: pass a knob ONLY when the caller actually set it, so the DRIVER's own
# default always governs otherwise. Hardcoding "${VAR:-<literal>}" here silently
# overrides the module default the moment the two drift apart - which is exactly
# what happened after M88: vic_fw's default was corrected to 5 in the driver
# while this script still forced vic_fw=0, and 0 means "use the Windows rule",
# so the run went out at fw=7 and burned a hardware pass proving nothing.
OPTARGS=""
add_opt() {   # add_opt <module-param> <env-value>
	[ -z "$2" ] || OPTARGS="$OPTARGS $1=$2"
}
add_opt vic_fw          "${VICFW:-}"
add_opt vic_out_format  "${VICM:-}"
add_opt win_seq         "${WINSEQ:-}"
add_opt irq_intx        "${INTX:-}"
add_opt enc_sub         "${ENCSUB:-}"
add_opt win_start_op6   "${OP6:-}"
add_opt win_bufs_first  "${WINBUFS:-}"
add_opt set_buf_op8     "${OP8:-}"
add_opt probe_windows   "${PROBEWIN:-}"
add_opt mst_win_output  "${MSTOUT:-}"
add_opt mst_ad          "${MSTAD:-}"
# M96: these five used to be hardcoded in the insmod line below with literal
# defaults (0/0/0/0x21/2). They all happened to match the driver, but that is
# exactly the drift trap method rule 4 names - route them through add_opt too.
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
add_opt poll_drain_ms   "${POLLDRAIN:-}"
add_opt poll_drain_credit "${POLLCREDIT:-}"
add_opt op6_kick_ms     "${OP6KICK:-}"
add_opt kick_opcode     "${KICKOP:-}"
add_opt kick_repeat     "${KICKREP:-}"
add_opt stream_without_signal "${NOSRC:-}"

dmesg -C
insmod "$MZKO" dma_handshake=1 enable_dma=1 \
	enable_video=1 procfs_verbosity=2 dma_iova_remap=1 aic_on=1 \
	stream_nosg="$NOSG" force_timings=0 signal_poll_ms=4000 \
	$OPTARGS ${EXTRA:-} \
	|| { echo "insmod failed"; exit 1; }
echo "waiting for the card to finish booting its own flash image..."
sleep 25

VIDEO_NODE=$(find_mz_video_node) || {
	echo "mz0380 video node did not register"
	exit 1
}
echo "using $VIDEO_NODE"

if ! dmesg | grep -q '4 stream buffers x'; then
	echo "DMA stream buffers were not armed; capture cannot work."
	dmesg | grep -E 'IOVA|IOMMU|stream buffer|DMA setup' | tail -20
	exit 1
fi

# Property 201 is persistent card state. Do not let a previous Windows/Linux
# session leave the VIC routed away from the only physical HDMI connector.
v4l2-ctl -d "$VIDEO_NODE" --set-input=0 || {
	echo "could not select the HDMI input route"
	exit 1
}

# The source transmits around a plug/power event. QUERY_DV_TIMINGS itself must
# run during that window; the old script blocked in procfs watch and queried
# only after the transient lock had already disappeared.
if [ "$NOSG" = 1 ]; then
	echo "=== 1. NOSG=1: skipping receiver lock (card-side diagnostic only) ==="
elif [ "${NOSRC:-0}" = 1 ]; then
	# M113: deliberately no source connected. There is nothing to lock, so
	# skip the gate entirely - the card still runs its capture path and
	# falls back to its own no-signal splash, which is what Windows shows
	# in OBS in exactly this situation.
	echo "=== 1. NOSRC=1: no source connected, skipping the receiver lock ==="
else
	echo "=== 1. HPD edge, then poll timings while YOU power-cycle the source ==="
	dmesg -C
	echo "hpd 2 2000" > /proc/mz0380-hdmi
	echo
	echo "   >>> POWER-CYCLE (or re-plug) THE SOURCE NOW - you have ${LOCKWAIT}s <<<"
	echo
	TIMINGS=/tmp/mz0380-m55-timings.txt
	TIMING_ERR=/tmp/mz0380-m55-timings.err
	rm -f "$TIMINGS" "$TIMING_ERR"
	LOCKED=0
	DEADLINE=$((SECONDS + LOCKWAIT))
	while [ "$SECONDS" -lt "$DEADLINE" ]; do
		if v4l2-ctl -d "$VIDEO_NODE" --query-dv-timings >"$TIMINGS" 2>"$TIMING_ERR"; then
			LOCKED=1
			break
		fi
		sleep 0.2
	done

	if [ "$LOCKED" -ne 1 ]; then
		echo "No coherent HDMI timing was locked during the ${LOCKWAIT}s window."
		cat "$TIMING_ERR"
		dmesg | grep -E "MST3367|detect|lock|timing|unmatched" | tail -40
		exit 2
	fi

	echo "=== 1b. coherent mode detected ==="
	cat "$TIMINGS"
	dmesg | grep -E "MST3367 signal|coherent but unsupported|timing" | tail -20
	# M70: show the EDID push + read-back verdict from bring-up before any
	# later dmesg -C erases it. READ-BACK OK = a real store holds our EDID.
	echo "--- EDID delivery verdict ---"
	dmesg | grep -iE "EDID push|EDID READ-BACK|EDID read-back|EDID write failed|EDID mux" | tail -5
fi

echo
if [ "${NOSRC:-0}" = 1 ]; then
	echo "=== 2. capture $FRAMES frames - NO SOURCE, expecting the card's own splash ==="
else
	echo "=== 2. capture $FRAMES frames - POWER-CYCLE THE SOURCE ONCE STREAMING STARTS ==="
fi
# M130: the payload is planar I420 (Y, then two 960x540 planes), NOT NV12.
# The .nv12 name is historical and has cost two viewing mistakes; the ffplay
# hints below say yuv420p. Decoding it as nv12 gives magenta/green interleave
# banding on an otherwise perfect picture.
# M58: the nosg path delivers raw 4:2:0, not H.264 - name the file for what it
# actually holds so the check below is not nonsense (the M57 run "failed"
# ffprobe purely because raw 4:2:0 was written to a .h264 name).
# M111: with POLLDRAIN set the real path also delivers raw 4:2:0, not H.264 -
# the card writes a 1920x1080 4:2:0 frame and no bitstream. Name the file for
# what it holds, or the ffprobe check below is nonsense (same trap as M57).
if [ "$NOSG" = 1 ] || [ -n "${POLLDRAIN:-}" ] || [ "${NOSRC:-0}" = 1 ]; then
	CAP=/tmp/cap-m55.nv12
else
	CAP=/tmp/cap-m55.h264
fi
rm -f "$CAP"
dmesg -C

# M66: the detection burst is long gone by the time the encoder is armed.
# Arming costs ~4s (SET_VIC 2s + SET_ENC_PARAMS 2s), and the source only
# transmits for roughly 300ms after a power-cycle, so a capture started
# "while lock is live" is guaranteed to record silence. The burst has to
# arrive AFTER the encoder is running - hence the prompt below, and a window
# long enough to cycle the source two or three times.
#
# The concurrent watch is read-only and correlates what the receiver saw with
# what the encoder produced: without it, "0 bytes" cannot distinguish "the
# source never transmitted" from "it did and the card dropped the frames".
# M113: with no source there is nothing to wait for - no burst to catch, no
# power-cycle to prompt for. A 60s window would just be 60s of nothing, which
# is most of why this script feels slow.
if [ "${NOSRC:-0}" = 1 ]; then
	CAPWAIT=${CAPWAIT:-10}
	WATCH=${WATCH:-0}
else
	CAPWAIT=${CAPWAIT:-60}
fi

# M68: give the source a REASON to transmit while the encoder is armed.
#
# The previous run answered the open question: the watch reported 55=03
# no-lock, unchanged, for the whole 60s window - so the card was not dropping
# frames, the source was simply silent. HPD is raised once during bring-up and
# then never moves, so a source that has already given up has no reason to
# retry. M48/M49 proved this source reacts to a hotplug EDGE, so pulse HPD
# repeatedly across the capture window and watch between the pulses.
#
# The writes are serialised on purpose: /proc/mz0380-hdmi runs "watch" inline,
# so a concurrent hpd write would just block behind it. Alternating in one
# background shell gives edges at roughly t+7s, t+21s and t+35s.
# M74: WATCH=0 runs the capture with NO concurrent receiver I2C at all.
# The watch samples the MST3367 every 250ms and, until M74, re-armed
# auto-position on every lock transition - i.e. the diagnostic was poking
# acquisition registers on a receiver that was mid-capture. Writes are now
# suppressed while streaming, but a clean control run with zero I2C traffic
# is still the only way to prove the observer is not the problem.
WATCH=${WATCH:-1}
WATCH_PID=
if [ "$WATCH" = 1 ]; then
	( sleep 6
	  for _ in 1 2 3; do
		echo "hpd 2 1000"  > /proc/mz0380-hdmi 2>/dev/null
		echo "watch 12"    > /proc/mz0380-hdmi 2>/dev/null
	  done ) &
	WATCH_PID=$!
else
	echo "   (WATCH=0: no HPD pulses, no receiver polling during capture)"
fi

echo
echo "   >>> The driver now pulses HPD 3x during this window by itself.    <<<"
echo "   >>> ALSO power-cycle the source a couple of times over ${CAPWAIT}s   <<<"
echo "   >>> - whichever makes it transmit, the encoder is already armed.  <<<"
echo

# M116: v4l2-ctl blocks until --stream-count frames arrive. While the card
# delivers only ONE frame per stream (the open cadence problem), asking for more
# guarantees that `timeout` SIGTERMs it mid-write and the last frame loses its
# unflushed stdio tail - the first OP8 run captured 3108864 of 3110400 bytes,
# short by exactly the 1536-byte remainder, and ffplay rejected the whole file.
# SIGINT first gives v4l2-ctl a chance to close the file cleanly.
timeout -s INT --foreground "$CAPWAIT" \
	v4l2-ctl -d "$VIDEO_NODE" --stream-mmap --stream-count="$FRAMES" \
	--stream-to="$CAP"
SZ=$(stat -c %s "$CAP" 2>/dev/null || echo 0)
[ -z "$WATCH_PID" ] || wait "$WATCH_PID" 2>/dev/null
echo "--- captured $SZ bytes ---"

echo "--- did the receiver see the source while the encoder was running? ---"
dmesg | grep -E "detect 55=|MST3367 signal" | tail -12

echo "=== 3. what did the card do? ==="
# M132: "MST3367 CSC" added - mz0380_mst3367_apply_csc_mode() logs which CSC
# mode it picked and why, and the old pattern filtered it out, so the one line
# that confirms the colour knob landed was invisible. Method rule 9.
# M139: "producer watch" added. The watch samples BAR0 40/44/48 every poll
# interval and its baseline/change lines are the only host-visible proof that
# the card's producer is still running - filtering them out would repeat
# exactly the mistake method rule 9 records.
# M159: this was ONE grep ending in `head -28`, and under fw=7 the card
# produced a per-frame line every ~16 ms. The flood filled all 28 slots, so the
# `stream stop:` summary - frame_events, irq_total, the final token values - was
# cut from the output of the most important run in the project. Structural lines
# and per-frame lines are now capped separately: the summary can never again be
# pushed out by the thing it is summarising.
dmesg | grep -aE "stream start|stream stop|enc|no HDMI signal|poll-drain|MST3367 CSC|producer watch" | head -20
# The per-frame flood, capped. With a continuous producer the COUNT is the
# result, not the individual lines - so print it either way.
FRAME_TOKENS=$(dmesg | grep -ac "frame token")
dmesg | grep -a "frame token" | head -8
if [ "$FRAME_TOKENS" -gt 8 ]; then
	echo "  ... $FRAME_TOKENS 'frame token' lines in total (capped at 8)."
	echo "  A repeating token sequence IS a cadence - the one-frame bound is"
	echo "  1 delivery per stream, so >1 here is the result worth reading."
fi
# M70: the per-buffer poison scan is the "did H.264 bytes land without a
# completion" measurement - it has been printed at every stop and filtered
# out by the grep above this whole time.
echo "--- MST3367 output stage (is the receiver clocking BT1120 out?) ---"
dmesg | grep -E "output stage" | tail -6
# M78: the link layer. R55 lock is only the timing front end; BANK1 0x01 bit2
# says whether the source actually came up in HDMI mode or fell back to DVI.
echo "--- link layer (HDMI vs DVI, HDCP) ---"
dmesg | grep -E "link \[" | tail -6
echo "--- buffer poison scan (pages touched = card wrote data) ---"
dmesg | grep -E "stop buf\[" | tail -8
dmesg | grep -E "encoder spawns|entering the range|SET_AIC\(on=0\)" | tail -3

# M76: the card writes ~3.1 MB into buf0 on the real path (head 0x11 = its own
# NO-SIGNAL splash). Dump it before the module is unloaded so the content can
# be identified offline instead of guessed from 16 head bytes.
BUF0=${BUF0:-/tmp/mz0380-buf0.bin}
if [ -r /proc/mz0380-buf0 ]; then
	dd if=/proc/mz0380-buf0 of="$BUF0" bs=1M status=none 2>/dev/null
	echo "--- buf0 dumped to $BUF0 ($(stat -c %s "$BUF0" 2>/dev/null || echo 0) bytes) ---"
	echo "    first 32 bytes:"
	head -c 32 "$BUF0" | od -An -tx1
	echo "    bytes at 0xbdd80 (UV plane if this is the 720x1080 splash):"
	dd if="$BUF0" bs=1 skip=$((0xbdd80)) count=16 status=none | od -An -tx1
fi

# M59: a wall of SET_VIC ret=-110 is the known wedge, not a capture bug. The
# mailbox stops answering after enough encoder spawns and ONLY a mains-off
# cold boot clears it - rmmod/insmod and firmware re-upload do not. Say so
# here, because every later result in this run is meaningless once it hits.
if [ "$(dmesg | grep -c 'ret=-110')" -gt 2 ]; then
	echo
	echo "!!! CARD WEDGED: SET_VIC is timing out (-110). The mailbox is dead."
	echo "!!! Shut down, switch the PSU off at the mains, then retry."
	echo "!!! uptime: $(uptime -p) - a cold boot resets this counter."
fi
if [ "$SZ" -gt 0 ] && [ "$NOSG" = 1 ]; then
	echo "=== 4. NOSG diagnostic frame (card-generated; not HDMI pixels) ==="
	echo "The raw payload proves only the fake-frame encoder/DMA path."
	head -c 64 "$CAP" | od -An -tx1
elif [ "$SZ" -gt 0 ]; then
	if [ -n "${POLLDRAIN:-}" ] || [ "$NOSG" = 1 ]; then
		# M115: this path delivers RAW planar 4:2:0, so the H.264 NAL check and
		# ffprobe are nonsense on it - the first run reported the file as
		# "ADPCM Nintendo Gamecube DTK", which is ffprobe guessing at
		# planar YUV. Check what actually matters instead: whole frames.
		SZ4=$(stat -c %s "$CAP")
		FRAMESZ=$((1920 * 1080 * 3 / 2))
		echo "=== 4. raw planar I420 sanity ($FRAMESZ bytes per frame) ==="
		head -c 32 "$CAP" | od -An -tx1
		WHOLE=$((SZ4 / FRAMESZ))
		REM=$((SZ4 % FRAMESZ))
		echo "  captured $SZ4 bytes = $WHOLE whole frames + $REM bytes"
		if [ "$REM" -ne 0 ] && [ "$WHOLE" -ge 1 ]; then
			# M116: drop the partial tail so the file is playable. The
			# tail is either a torn DMA or v4l2-ctl killed mid-write;
			# either way it is not a frame and ffplay rejects the file
			# because of it.
			truncate -s $((WHOLE * FRAMESZ)) "$CAP"
			echo "  trimmed the $REM-byte partial tail -> $WHOLE playable frame(s)"
		elif [ "$REM" -ne 0 ]; then
			echo "  WARNING: $REM bytes and NOT ONE whole frame."
			echo "  The driver log above shows what it delivered - if it says"
			echo "  length=$FRAMESZ then v4l2-ctl was killed mid-write, so ask"
			echo "  for a count the card can actually deliver:"
			echo "      sudo POLLDRAIN=20 $0 1"
		fi
		echo "  view it:  ffplay -f rawvideo -pixel_format yuv420p -video_size 1920x1080 $CAP"
		echo "  score it: scripts/mz0380-m127-splash.py $CAP   # splash, or a real frame"
		echo "            scripts/mz0380-m130-chroma.py $CAP   # is the colour right"
	else
		echo "=== 4. does it look like H.264? (expect 00 00 00 01 NAL starts) ==="
		head -c 32 "$CAP" | od -An -tx1
		command -v ffprobe >/dev/null && ffprobe -v error -show_streams "$CAP" 2>&1 | head -20
	fi
fi
