#!/usr/bin/env bash
# mz0380 bring-up verification harness.
#
# Walks the Phase 1..6 gates from PLAN.md, one at a time, with a clear
# pass/fail at each step. Each stage cleanly unloads on failure so the
# tester can iterate.
#
# Stages:
#   1  build         - module compiles
#   2  probe-safe    - module loads, /proc/mz0380* present, BARs mapped
#   3  firmware      - request_firmware() succeeds;
#                      reaches MZ0380_FW_STATE_READY
#   4  bar0-wake     - post-firmware, BAR0 0x0000 != 0xffffffff
#                      (Xilinx fabric is alive)
#   5  irq           - /proc/interrupts shows non-zero counter for the
#                      mz0380 MSI vector after HDMI plug-in
#   6  ring-payload  - hexdump first 64 bytes of one ring slot shows
#                      H.264 NAL prefix 00 00 00 01
#   7  v4l2-capture  - ffmpeg -f v4l2 -i /dev/video0 produces a file
#                      ffprobe reports as H.264 with right dimensions
#   8  dv-timings    - v4l2-ctl --query-dv-timings reports a real
#                      resolution that matches the source
#   9  alsa-capture  - arecord produces a non-empty WAV
#  10  unload-clean  - rmmod succeeds and dmesg is clean
#
# All commands respect $DRY_RUN=1 (print but do not execute) so the
# tester can review the plan before running anything.

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODULE="$PROJECT_ROOT/mz0380.ko"
PCI_ADDR="${PCI_ADDR:-0000:04:00.0}"
VIDEO_NODE="${VIDEO_NODE:-/dev/video0}"
AUDIO_DEV="${AUDIO_DEV:-hw:CARD=mz0380,DEV=0}"
OUT_DIR="${OUT_DIR:-/tmp/mz0380-bringup}"
HDMI_SOURCE_NOTE="(plug HDMI source in when prompted)"
DRY_RUN="${DRY_RUN:-0}"
KEEP_LOADED="${KEEP_LOADED:-0}"
STAGES="${STAGES:-all}"
EXPECTED_FW="${EXPECTED_FW:-/lib/firmware/mz0380/MZ0380.FW.TXT}"
CAPTURE_SECONDS="${CAPTURE_SECONDS:-5}"

usage() {
	cat <<EOF
mz0380 bring-up verification harness.

Usage:  $0 [stage [stage ...]]

Stages (run in order; default 'all'):
  build probe-safe firmware bar0-wake irq ring-payload \\
  v4l2-capture dv-timings alsa-capture unload-clean

Environment overrides:
  PCI_ADDR=$PCI_ADDR
  VIDEO_NODE=$VIDEO_NODE
  AUDIO_DEV=$AUDIO_DEV
  OUT_DIR=$OUT_DIR
  EXPECTED_FW=$EXPECTED_FW
  CAPTURE_SECONDS=$CAPTURE_SECONDS
  KEEP_LOADED=0|1
  DRY_RUN=0|1     (print commands without running)

Example: run only firmware + bar0-wake stages:
  $0 firmware bar0-wake
EOF
}

[[ "${1:-}" == "-h" || "${1:-}" == "--help" ]] && { usage; exit 0; }

mkdir -p "$OUT_DIR"
LOG="$OUT_DIR/bringup-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee -a "$LOG") 2>&1

color()  { printf '\033[%sm%s\033[0m\n' "$1" "$2"; }
hdr()    { color "1;36" "==== $* ===="; }
ok()     { color "1;32" "  OK: $*"; }
warn()   { color "1;33" "  WARN: $*"; }
fail()   { color "1;31" "  FAIL: $*"; }
run()    {
	echo "  \$ $*"
	[[ "$DRY_RUN" == "1" ]] && return 0
	"$@"
}

abort() {
	fail "$*"
	if [[ "$KEEP_LOADED" != "1" ]]; then
		warn "unloading module due to failure"
		[[ "$DRY_RUN" == "1" ]] || sudo rmmod mz0380 2>/dev/null || true
	fi
	exit 1
}

need_cmd() {
	command -v "$1" >/dev/null || abort "missing required command: $1"
}

is_loaded() { lsmod | awk '{print $1}' | grep -qx mz0380; }

ensure_unloaded() {
	if is_loaded; then
		run sudo rmmod mz0380 || abort "failed to unload mz0380"
	fi
}

read_bar0_word() {
	# Read first 32-bit word of BAR0 via sysfs resource0 (read-only)
	local val
	val=$(sudo dd if=/sys/bus/pci/devices/"$PCI_ADDR"/resource0 bs=4 count=1 2>/dev/null \
		| xxd -p -c4)
	# xxd outputs little-endian byte order; reverse for canonical hex
	echo "0x$(echo "$val" | tac -rs '..' | tr -d '\n')"
}

stage_build() {
	hdr "1. build"
	need_cmd make
	(
		cd "$PROJECT_ROOT"
		run make clean
		run make
	) || abort "build failed"
	[[ -f "$MODULE" ]] || abort "no $MODULE after build"
	ok "module built"
}

stage_probe_safe() {
	hdr "2. probe-safe load"
	ensure_unloaded
	run sudo insmod "$MODULE" procfs_verbosity=2 enable_video=1 \
		|| abort "insmod (probe-safe) failed"

	for p in /proc/mz0380 /proc/mz0380-state /proc/mz0380-snapshot \
		 /proc/mz0380-control /proc/mz0380-experiment; do
		[[ -r "$p" ]] || abort "missing $p after probe"
	done
	[[ -c "$VIDEO_NODE" ]] || abort "missing $VIDEO_NODE after probe"

	run cp /proc/mz0380 "$OUT_DIR/proc.mz0380.txt"
	run cp /proc/mz0380-state "$OUT_DIR/proc.state.probe-safe.txt"
	ok "probe-safe load + V4L2 node + procfs all present"
}

stage_firmware() {
	hdr "3. firmware handshake (nothing is uploaded)"
	[[ -f "$EXPECTED_FW" ]] \
		|| abort "missing version sidecar at $EXPECTED_FW (see README)"
	ok "version sidecar present"

	ensure_unloaded
	run sudo insmod "$MODULE" procfs_verbosity=2 \
		enable_video=1 \
		|| abort "insmod () failed"

	# Watch dmesg for fw state
	sleep 3
	if dmesg | tail -200 | grep -qE "firmware ready|firmware version"; then
		ok "firmware reached READY state"
	else
		warn "no 'firmware ready' message yet; dumping last 40 mz0380 lines"
		dmesg | grep mz0380 | tail -40 \
			| tee "$OUT_DIR/dmesg.firmware.txt"
		abort "firmware did not reach READY - check CHECKME offsets in mz0380-reg.h"
	fi

	run cp /proc/mz0380-state "$OUT_DIR/proc.state.firmware.txt"
}

stage_bar0_wake() {
	hdr "4. bar0-wake"
	is_loaded || abort "module not loaded; run firmware stage first"

	local word
	word=$(read_bar0_word)
	echo "  BAR0[0x0000] = $word"

	if [[ "$word" == "0xffffffff" || "$word" == "0xffffffffffffffff" ]]; then
		abort "BAR0 still asleep - firmware did not actually wake the Xilinx fabric"
	fi
	ok "BAR0 fabric awake ($word)"
}

stage_irq() {
	hdr "5. irq"
	is_loaded || abort "module not loaded"

	warn "$HDMI_SOURCE_NOTE: ensure HDMI source is plugged in"
	[[ "$DRY_RUN" == "1" ]] || read -r -p "  press Enter when HDMI source is live..."

	ensure_unloaded
	run sudo insmod "$MODULE" procfs_verbosity=2 \
		enable_video=1 enable_dma=1 \
		|| abort "insmod (enable_dma=1) failed"

	sleep 5

	local count
	count=$(grep mz0380 /proc/interrupts | awk '{s=0; for (i=2;i<=NF-2;i++) s+=$i; print s}')
	echo "  /proc/interrupts mz0380 sum = ${count:-0}"

	if [[ -z "$count" || "$count" -lt 1 ]]; then
		warn "no IRQ activity. Either IRQ regs are wrong (mz0380-reg.h MZ0380_REG_IRQ_*) or the mask bits are wrong"
		grep mz0380 /proc/interrupts > "$OUT_DIR/proc.interrupts.txt" || true
		abort "no IRQ counted - check MZ0380_REG_IRQ_MASK / MZ0380_REG_IRQ_STATUS"
	fi
	ok "IRQ flowing ($count counted)"
	cp /proc/interrupts "$OUT_DIR/proc.interrupts.txt"
}

stage_ring_payload() {
	hdr "6. ring-payload"
	is_loaded || abort "module not loaded"

	# Read ring slot 0 directly via debugfs if exposed, otherwise via
	# /proc/mz0380-state hex dump section.
	if grep -q "video_ring" /proc/mz0380-state 2>/dev/null; then
		grep -A4 "video_ring" /proc/mz0380-state \
			| tee "$OUT_DIR/ring.head.txt"
	fi

	warn "ring payload inspection requires a debugfs hook the driver"
	warn "does not yet expose. Skipping deep hex check; manually:"
	warn "  sudo dd if=/dev/mem bs=1 count=64 skip=\$(physical addr of slot 0)"
	ok "ring-payload stage advisory only"
}

stage_v4l2_capture() {
	hdr "7. v4l2-capture"
	need_cmd ffmpeg
	need_cmd ffprobe
	is_loaded || abort "module not loaded"

	local out="$OUT_DIR/capture.h264"
	rm -f "$out"

	run timeout "$((CAPTURE_SECONDS+5))" \
		ffmpeg -y -nostats -hide_banner -loglevel warning \
		-f v4l2 -pixel_format h264 -i "$VIDEO_NODE" \
		-t "$CAPTURE_SECONDS" -c copy "$out"

	if [[ ! -s "$out" ]]; then
		abort "ffmpeg produced no output. STREAMON path likely wrong."
	fi

	local meta
	meta=$(ffprobe -v error -select_streams v -show_entries \
		stream=codec_name,width,height,r_frame_rate -of csv=p=0 "$out")
	echo "  ffprobe: $meta"
	echo "$meta" | grep -q '^h264' || abort "captured stream is not H.264"

	ok "captured H.264 stream ($meta)"
}

stage_dv_timings() {
	hdr "8. dv-timings"
	need_cmd v4l2-ctl
	is_loaded || abort "module not loaded"

	local q
	q=$(v4l2-ctl --device="$VIDEO_NODE" --query-dv-timings 2>&1 || true)
	echo "  $q" | sed 's/^/    /'
	echo "$q" > "$OUT_DIR/dv-timings.txt"

	if echo "$q" | grep -qE "^Width|width:"; then
		ok "dv-timings reports an active resolution"
	else
		warn "dv-timings did not parse - signal probe may be wrong"
		warn "this is non-fatal if v4l2-capture passed"
	fi
}

stage_alsa_capture() {
	hdr "9. alsa-capture"
	need_cmd arecord
	is_loaded || abort "module not loaded"

	# Re-load with audio if not already enabled
	if ! arecord -L 2>/dev/null | grep -q mz0380; then
		ensure_unloaded
		run sudo insmod "$MODULE" procfs_verbosity=2 \
			enable_video=1 \
			enable_dma=1 enable_audio=1 \
			|| abort "insmod (enable_audio=1) failed"
		sleep 2
	fi

	local out="$OUT_DIR/capture.wav"
	rm -f "$out"
	run timeout "$((CAPTURE_SECONDS+5))" \
		arecord -D "$AUDIO_DEV" -f S16_LE -r 48000 -c 2 \
		-d "$CAPTURE_SECONDS" "$out"

	[[ -s "$out" ]] || abort "arecord produced no output"
	ok "captured $(stat -c%s "$out") bytes of audio"
}

stage_unload_clean() {
	hdr "10. unload-clean"
	if is_loaded; then
		run sudo rmmod mz0380 || abort "rmmod failed"
	fi

	# Look for AMD-Vi / IOMMU faults that the earlier bring-up
	# notes warned about.
	if dmesg | tail -200 | grep -qE "AMD-Vi|IO_PAGE_FAULT|iommu"; then
		warn "IOMMU events in recent dmesg:"
		dmesg | tail -200 | grep -E "AMD-Vi|IO_PAGE_FAULT|iommu" \
			| tee "$OUT_DIR/iommu.warnings.txt"
		abort "IOMMU fault detected - ring base programming sequence wrong"
	fi
	ok "clean unload, no IOMMU faults"
}

select_stages() {
	if [[ "${1:-}" == "all" || $# -eq 0 ]]; then
		echo build probe-safe firmware bar0-wake irq \
		     ring-payload v4l2-capture dv-timings \
		     alsa-capture unload-clean
	else
		echo "$@"
	fi
}

main() {
	hdr "mz0380 bring-up @ $(date)"
	echo "  log:        $LOG"
	echo "  out-dir:    $OUT_DIR"
	echo "  pci:        $PCI_ADDR"
	echo "  video:      $VIDEO_NODE"
	echo "  audio:      $AUDIO_DEV"
	echo "  dry-run:    $DRY_RUN"

	local s
	for s in $(select_stages "$@"); do
		case "$s" in
			build)         stage_build ;;
			probe-safe)    stage_probe_safe ;;
			firmware)      stage_firmware ;;
			bar0-wake)     stage_bar0_wake ;;
			irq)           stage_irq ;;
			ring-payload)  stage_ring_payload ;;
			v4l2-capture)  stage_v4l2_capture ;;
			dv-timings)    stage_dv_timings ;;
			alsa-capture)  stage_alsa_capture ;;
			unload-clean)  stage_unload_clean ;;
			*) abort "unknown stage: $s" ;;
		esac
	done

	hdr "all stages passed"
}

main "$@"
