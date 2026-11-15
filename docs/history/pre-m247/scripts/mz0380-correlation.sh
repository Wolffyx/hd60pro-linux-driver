#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: scripts/mz0380-correlation.sh [options]

Probe-safe MZ0380 BAR correlation helper.

Options:
  -n, --no-build      skip rebuilding mz0380.ko before loading it
  -p, --profile N     snapshot_profile to use (default: 4)
  -i, --input N       issue SDK property-201 style input-select request N
      --input-seq CSV issue a comma-separated sequence such as 0,1,2,3,4
      --input-api API issue input requests via `procfs` or `v4l2` (default: procfs)
      --input-reg R   BAR5 candidate register offset for the input experiment
      --input-mask M  BAR5 bit mask for the input field (default: 0x7)
      --input-shift S BAR5 bit shift for the input field (default: 0)
  -b, --bitrate N     issue SDK property-403 style bitrate request N
      --bitrate-seq CSV
                     issue a comma-separated bitrate sequence such as
                     6291456,8388608,12582912
      --bitrate-reg R BAR5 candidate register offset for the bitrate experiment
      --bitrate-mask M
                     BAR5 bit mask for the bitrate field (default: 0xffffffff)
      --bitrate-shift S
                     BAR5 bit shift for the bitrate field (default: 0)
  -q, --quality N     issue SDK property-404 style quality request N
      --quality-seq CSV
                     issue a comma-separated quality sequence such as
                     0,50,80,100
      --quality-reg R BAR5 candidate register offset for the quality experiment
      --quality-mask M
                     BAR5 bit mask for the quality field (default: 0xffffffff)
      --quality-shift S
                     BAR5 bit shift for the quality field (default: 0)
  -g, --gop N         issue SDK property-405 style GOP request N
      --gop-seq CSV
                     issue a comma-separated GOP sequence such as
                     30,60,120
      --gop-reg R     BAR5 candidate register offset for the GOP experiment
      --gop-mask M
                     BAR5 bit mask for the GOP field (default: 0xffffffff)
      --gop-shift S
                     BAR5 bit shift for the GOP field (default: 0)
      --b-frames N    issue SDK property-411 style B-frame request N
      --b-frames-seq CSV
                     issue a comma-separated B-frame sequence such as
                     0,1,2
      --b-frames-reg R
                     BAR5 candidate register offset for the B-frame experiment
                     override the fixed BAR5 field for bounded experiments
      --b-frames-mask M
                     BAR5 bit mask for the B-frame field (default: 0xffffffff)
      --b-frames-shift S
                     BAR5 bit shift for the B-frame field (default: 0)
      --qp-step N     issue SDK property-408 style QP-step request N
      --qp-step-seq CSV
                     issue a comma-separated QP-step sequence such as
                     0,4,8
      --qp-step-reg R BAR5 candidate register offset for the QP-step experiment
                     override the fixed BAR5 field for bounded experiments
      --qp-step-mask M
                     BAR5 bit mask for the QP-step field (default: 0xffffffff)
      --qp-step-shift S
                     BAR5 bit shift for the QP-step field (default: 0)
  -r, --record-mode N issue SDK property-407 style record-mode request N
      --record-mode-reg R
                     BAR5 candidate register offset for the record-mode experiment
      --record-mode-mask M
                     BAR5 bit mask for the record-mode field (default: 0x3)
      --record-mode-shift S
                     BAR5 bit shift for the record-mode field (default: 0)
      --enable-video  load the probe-safe V4L2 node (required for --input-api v4l2)
      --enable-dma    alloc rings, request MSI, set bus master  (Phase 2)
                     implies --allow-bus-master
      --enable-audio  register ALSA HDMI audio capture          (Phase 5)
      --allow-bus-master
                     pass allow_bus_master=1 to the module (legacy escape hatch)
      --video-node N  use /dev/videoN for V4L2 input requests; auto-detect by default
  -s, --sleep SEC     sleep for SEC after the "before" capture instead of waiting
                      for Enter
  -o, --out-dir DIR   output directory for captured artifacts (default: /tmp)
  -k, --keep-loaded   leave the module loaded at the end
  -h, --help          show this help

Examples:
  scripts/mz0380-correlation.sh
  scripts/mz0380-correlation.sh --no-build
  scripts/mz0380-correlation.sh --profile 5 --sleep 20
  scripts/mz0380-correlation.sh --profile 5 --input 1
  scripts/mz0380-correlation.sh --profile 5 --input 1 --input-api v4l2 --enable-video
  scripts/mz0380-correlation.sh --profile 5 --input-seq 0,1,2,3,4 --input-reg 0x0040
  scripts/mz0380-correlation.sh --profile 5 --bitrate 12582912 --bitrate-reg 0x005c
  scripts/mz0380-correlation.sh --profile 5 --quality 80 --quality-reg 0x0060
  scripts/mz0380-correlation.sh --profile 5 --quality-seq 0,50,80,100 --quality-reg 0x0060
  scripts/mz0380-correlation.sh --profile 5 --gop 30 --gop-reg 0x0080
  scripts/mz0380-correlation.sh --profile 5 --gop-seq 30,60,120 --gop-reg 0x0080
  scripts/mz0380-correlation.sh --profile 5 --b-frames-seq 0,1,2 --b-frames-reg 0x0088
  scripts/mz0380-correlation.sh --profile 5 --qp-step-seq 0,4,8
  scripts/mz0380-correlation.sh --profile 5 --qp-step-seq 0,4,8 --qp-step-reg 0x0084
  scripts/mz0380-correlation.sh --profile 5 --record-mode 1 --record-mode-reg 0x0058
  scripts/mz0380-correlation.sh --out-dir /tmp/mz0380-run

Bring-up correlation (Phase 1-5):
  scripts/mz0380-correlation.sh --profile 5 --enable-video
  scripts/mz0380-correlation.sh --profile 5 --enable-dma --enable-video
  scripts/mz0380-correlation.sh --profile 5 --enable-dma --enable-audio --enable-video

For the full Phase 1..6 bring-up gate runner see mz0380-bringup.sh.
EOF
}

build_module=1
profile=4
sleep_seconds="20"
out_dir=/tmp
keep_loaded=0
experiment_input=""
experiment_sequence=""
experiment_bitrate=""
experiment_bitrate_sequence=""
experiment_quality=""
experiment_quality_sequence=""
experiment_gop=""
experiment_gop_sequence=""
experiment_b_frames=""
experiment_b_frames_sequence=""
experiment_qp_step=""
experiment_qp_step_sequence=""
experiment_record_mode=""
input_api="procfs"
input_reg=""
input_mask="0x7"
input_shift="0"
bitrate_reg=""
bitrate_mask="0xffffffff"
bitrate_shift="0"
quality_reg=""
quality_mask="0xffffffff"
quality_shift="0"
gop_reg=""
gop_mask="0xffffffff"
gop_shift="0"
b_frames_reg=""
b_frames_mask="0xffffffff"
b_frames_shift="0"
qp_step_reg=""
qp_step_mask="0xffffffff"
qp_step_shift="0"
record_mode_reg=""
record_mode_mask="0x3"
record_mode_shift="0"
enable_video=0
video_node=""
capture_v4l2=0

enable_dma=0
enable_audio=0
allow_bus_master_arg=0
have_v4l2_ctl=0
last_sequence_input=""

while [[ $# -gt 0 ]]; do
	case "$1" in
	-n|--no-build)
		build_module=0
		shift
		;;
	-p|--profile)
		profile="${2:?missing value for $1}"
		shift 2
		;;
	-i|--input)
		experiment_input="${2:?missing value for $1}"
		shift 2
		;;
	-b|--bitrate)
		experiment_bitrate="${2:?missing value for $1}"
		shift 2
		;;
	-q|--quality)
		experiment_quality="${2:?missing value for $1}"
		shift 2
		;;
	-g|--gop)
		experiment_gop="${2:?missing value for $1}"
		shift 2
		;;
	--b-frames)
		experiment_b_frames="${2:?missing value for $1}"
		shift 2
		;;
	--qp-step)
		experiment_qp_step="${2:?missing value for $1}"
		shift 2
		;;
	--gop-seq)
		experiment_gop_sequence="${2:?missing value for $1}"
		shift 2
		;;
	--b-frames-seq)
		experiment_b_frames_sequence="${2:?missing value for $1}"
		shift 2
		;;
	--qp-step-seq)
		experiment_qp_step_sequence="${2:?missing value for $1}"
		shift 2
		;;
	--quality-seq)
		experiment_quality_sequence="${2:?missing value for $1}"
		shift 2
		;;
	--bitrate-seq)
		experiment_bitrate_sequence="${2:?missing value for $1}"
		shift 2
		;;
	-r|--record-mode)
		experiment_record_mode="${2:?missing value for $1}"
		shift 2
		;;
	--input-seq)
		experiment_sequence="${2:?missing value for $1}"
		shift 2
		;;
	--input-api)
		input_api="${2:?missing value for $1}"
		shift 2
		;;
	--input-reg)
		input_reg="${2:?missing value for $1}"
		shift 2
		;;
	--input-mask)
		input_mask="${2:?missing value for $1}"
		shift 2
		;;
	--input-shift)
		input_shift="${2:?missing value for $1}"
		shift 2
		;;
	--bitrate-reg)
		bitrate_reg="${2:?missing value for $1}"
		shift 2
		;;
	--bitrate-mask)
		bitrate_mask="${2:?missing value for $1}"
		shift 2
		;;
	--bitrate-shift)
		bitrate_shift="${2:?missing value for $1}"
		shift 2
		;;
	--quality-reg)
		quality_reg="${2:?missing value for $1}"
		shift 2
		;;
	--quality-mask)
		quality_mask="${2:?missing value for $1}"
		shift 2
		;;
	--quality-shift)
		quality_shift="${2:?missing value for $1}"
		shift 2
		;;
	--gop-reg)
		gop_reg="${2:?missing value for $1}"
		shift 2
		;;
	--gop-mask)
		gop_mask="${2:?missing value for $1}"
		shift 2
		;;
	--gop-shift)
		gop_shift="${2:?missing value for $1}"
		shift 2
		;;
	--b-frames-reg)
		b_frames_reg="${2:?missing value for $1}"
		shift 2
		;;
	--b-frames-mask)
		b_frames_mask="${2:?missing value for $1}"
		shift 2
		;;
	--b-frames-shift)
		b_frames_shift="${2:?missing value for $1}"
		shift 2
		;;
	--qp-step-reg)
		qp_step_reg="${2:?missing value for $1}"
		shift 2
		;;
	--qp-step-mask)
		qp_step_mask="${2:?missing value for $1}"
		shift 2
		;;
	--qp-step-shift)
		qp_step_shift="${2:?missing value for $1}"
		shift 2
		;;
	--record-mode-reg)
		record_mode_reg="${2:?missing value for $1}"
		shift 2
		;;
	--record-mode-mask)
		record_mode_mask="${2:?missing value for $1}"
		shift 2
		;;
	--record-mode-shift)
		record_mode_shift="${2:?missing value for $1}"
		shift 2
		;;
	--enable-video)
		enable_video=1
		shift
		;;
	--enable-dma)
		enable_dma=1
		allow_bus_master_arg=1
		shift
		;;
	--enable-audio)
		enable_audio=1
		shift
		;;
	--allow-bus-master)
		allow_bus_master_arg=1
		shift
		;;
	--video-node)
		video_node="${2:?missing value for $1}"
		shift 2
		;;
	-s|--sleep)
		sleep_seconds="${2:?missing value for $1}"
		shift 2
		;;
	-o|--out-dir)
		out_dir="${2:?missing value for $1}"
		shift 2
		;;
	-k|--keep-loaded)
		keep_loaded=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		echo "Unknown argument: $1" >&2
		usage >&2
		exit 1
		;;
	esac
done

case "$profile" in
1|2|3|4|5)
	;;
*)
	echo "snapshot profile must be 1..5" >&2
	exit 1
	;;
esac

if [[ -n "$sleep_seconds" && ! "$sleep_seconds" =~ ^[0-9]+$ ]]; then
	echo "--sleep expects an integer number of seconds" >&2
	exit 1
fi

if [[ -n "$experiment_input" && ! "$experiment_input" =~ ^[0-4]$ ]]; then
	echo "--input expects a value from 0 to 4" >&2
	exit 1
fi

if [[ -n "$experiment_record_mode" && ! "$experiment_record_mode" =~ ^[0-2]$ ]]; then
	echo "--record-mode expects a value from 0 to 2" >&2
	exit 1
fi

if [[ -n "$experiment_bitrate" && ! "$experiment_bitrate" =~ ^[0-9]+$ ]]; then
	echo "--bitrate expects a non-negative integer" >&2
	exit 1
fi

if [[ -n "$experiment_quality" && ! "$experiment_quality" =~ ^[0-9]+$ ]]; then
	echo "--quality expects a non-negative integer" >&2
	exit 1
fi

if [[ -n "$experiment_gop" && ! "$experiment_gop" =~ ^[0-9]+$ ]]; then
	echo "--gop expects a non-negative integer" >&2
	exit 1
fi

if [[ -n "$experiment_b_frames" && ! "$experiment_b_frames" =~ ^[0-2]$ ]]; then
	echo "--b-frames expects a value from 0 to 2" >&2
	exit 1
fi

if [[ -n "$experiment_qp_step" && ! "$experiment_qp_step" =~ ^[0-9]+$ ]]; then
	echo "--qp-step expects a non-negative integer" >&2
	exit 1
fi

if [[ -n "$experiment_gop_sequence" &&
      ! "$experiment_gop_sequence" =~ ^[0-9]+(,[0-9]+)*$ ]]; then
	echo "--gop-seq expects a comma-separated list of non-negative integers" >&2
	exit 1
fi

if [[ -n "$experiment_b_frames_sequence" &&
      ! "$experiment_b_frames_sequence" =~ ^[0-2](,[0-2])*$ ]]; then
	echo "--b-frames-seq expects a comma-separated list using values 0..2" >&2
	exit 1
fi

if [[ -n "$experiment_qp_step_sequence" &&
      ! "$experiment_qp_step_sequence" =~ ^[0-9]+(,[0-9]+)*$ ]]; then
	echo "--qp-step-seq expects a comma-separated list of non-negative integers" >&2
	exit 1
fi

if [[ -n "$experiment_quality_sequence" &&
      ! "$experiment_quality_sequence" =~ ^[0-9]+(,[0-9]+)*$ ]]; then
	echo "--quality-seq expects a comma-separated list of non-negative integers" >&2
	exit 1
fi

if [[ -n "$experiment_bitrate_sequence" &&
      ! "$experiment_bitrate_sequence" =~ ^[0-9]+(,[0-9]+)*$ ]]; then
	echo "--bitrate-seq expects a comma-separated list of non-negative integers" >&2
	exit 1
fi

if [[ -n "$experiment_input" && -n "$experiment_sequence" ]]; then
	echo "--input and --input-seq are mutually exclusive" >&2
	exit 1
fi

if [[ -n "$experiment_bitrate" && -n "$experiment_bitrate_sequence" ]]; then
	echo "--bitrate and --bitrate-seq are mutually exclusive" >&2
	exit 1
fi

if [[ -n "$experiment_quality" && -n "$experiment_quality_sequence" ]]; then
	echo "--quality and --quality-seq are mutually exclusive" >&2
	exit 1
fi

if [[ -n "$experiment_gop" && -n "$experiment_gop_sequence" ]]; then
	echo "--gop and --gop-seq are mutually exclusive" >&2
	exit 1
fi

if [[ -n "$experiment_b_frames" && -n "$experiment_b_frames_sequence" ]]; then
	echo "--b-frames and --b-frames-seq are mutually exclusive" >&2
	exit 1
fi

if [[ -n "$experiment_qp_step" && -n "$experiment_qp_step_sequence" ]]; then
	echo "--qp-step and --qp-step-seq are mutually exclusive" >&2
	exit 1
fi

if [[ -n "$experiment_bitrate" &&
      ( -n "$experiment_input" || -n "$experiment_sequence" || -n "$experiment_quality" || -n "$experiment_quality_sequence" || -n "$experiment_gop" || -n "$experiment_gop_sequence" || -n "$experiment_b_frames" || -n "$experiment_b_frames_sequence" || -n "$experiment_qp_step" || -n "$experiment_qp_step_sequence" || -n "$experiment_record_mode" ) ]]; then
	echo "--bitrate is mutually exclusive with --input, --input-seq, --quality, --quality-seq, --gop, --gop-seq, --b-frames, --b-frames-seq, --qp-step, --qp-step-seq, and --record-mode" >&2
	exit 1
fi

if [[ -n "$experiment_bitrate_sequence" &&
      ( -n "$experiment_input" || -n "$experiment_sequence" || -n "$experiment_quality" || -n "$experiment_quality_sequence" || -n "$experiment_gop" || -n "$experiment_gop_sequence" || -n "$experiment_b_frames" || -n "$experiment_b_frames_sequence" || -n "$experiment_qp_step" || -n "$experiment_qp_step_sequence" || -n "$experiment_record_mode" ) ]]; then
	echo "--bitrate-seq is mutually exclusive with --input, --input-seq, --quality, --quality-seq, --gop, --gop-seq, --b-frames, --b-frames-seq, --qp-step, --qp-step-seq, and --record-mode" >&2
	exit 1
fi

if [[ -n "$experiment_quality" &&
      ( -n "$experiment_input" || -n "$experiment_sequence" || -n "$experiment_gop" || -n "$experiment_gop_sequence" || -n "$experiment_b_frames" || -n "$experiment_b_frames_sequence" || -n "$experiment_qp_step" || -n "$experiment_qp_step_sequence" || -n "$experiment_record_mode" ) ]]; then
	echo "--quality is mutually exclusive with --input, --input-seq, --gop, --gop-seq, --b-frames, --b-frames-seq, --qp-step, --qp-step-seq, and --record-mode" >&2
	exit 1
fi

if [[ -n "$experiment_quality_sequence" &&
      ( -n "$experiment_input" || -n "$experiment_sequence" || -n "$experiment_gop" || -n "$experiment_gop_sequence" || -n "$experiment_b_frames" || -n "$experiment_b_frames_sequence" || -n "$experiment_qp_step" || -n "$experiment_qp_step_sequence" || -n "$experiment_record_mode" ) ]]; then
	echo "--quality-seq is mutually exclusive with --input, --input-seq, --gop, --gop-seq, --b-frames, --b-frames-seq, --qp-step, --qp-step-seq, and --record-mode" >&2
	exit 1
fi

if [[ -n "$experiment_gop" &&
      ( -n "$experiment_input" || -n "$experiment_sequence" || -n "$experiment_b_frames" || -n "$experiment_b_frames_sequence" || -n "$experiment_qp_step" || -n "$experiment_qp_step_sequence" || -n "$experiment_record_mode" ) ]]; then
	echo "--gop is mutually exclusive with --input, --input-seq, --b-frames, --b-frames-seq, --qp-step, --qp-step-seq, and --record-mode" >&2
	exit 1
fi

if [[ -n "$experiment_gop_sequence" &&
      ( -n "$experiment_input" || -n "$experiment_sequence" || -n "$experiment_b_frames" || -n "$experiment_b_frames_sequence" || -n "$experiment_qp_step" || -n "$experiment_qp_step_sequence" || -n "$experiment_record_mode" ) ]]; then
	echo "--gop-seq is mutually exclusive with --input, --input-seq, --b-frames, --b-frames-seq, --qp-step, --qp-step-seq, and --record-mode" >&2
	exit 1
fi

if [[ -n "$experiment_b_frames" &&
      ( -n "$experiment_input" || -n "$experiment_sequence" || -n "$experiment_qp_step" || -n "$experiment_qp_step_sequence" || -n "$experiment_record_mode" ) ]]; then
	echo "--b-frames is mutually exclusive with --input, --input-seq, --qp-step, --qp-step-seq, and --record-mode" >&2
	exit 1
fi

if [[ -n "$experiment_b_frames_sequence" &&
      ( -n "$experiment_input" || -n "$experiment_sequence" || -n "$experiment_qp_step" || -n "$experiment_qp_step_sequence" || -n "$experiment_record_mode" ) ]]; then
	echo "--b-frames-seq is mutually exclusive with --input, --input-seq, --qp-step, --qp-step-seq, and --record-mode" >&2
	exit 1
fi

if [[ -n "$experiment_qp_step" &&
      ( -n "$experiment_input" || -n "$experiment_sequence" || -n "$experiment_record_mode" ) ]]; then
	echo "--qp-step is mutually exclusive with --input, --input-seq, and --record-mode" >&2
	exit 1
fi

if [[ -n "$experiment_qp_step_sequence" &&
      ( -n "$experiment_input" || -n "$experiment_sequence" || -n "$experiment_record_mode" ) ]]; then
	echo "--qp-step-seq is mutually exclusive with --input, --input-seq, and --record-mode" >&2
	exit 1
fi

if [[ -n "$experiment_record_mode" &&
      ( -n "$experiment_input" || -n "$experiment_sequence" ) ]]; then
	echo "--record-mode is mutually exclusive with --input and --input-seq" >&2
	exit 1
fi

if [[ -n "$experiment_sequence" && ! "$experiment_sequence" =~ ^[0-4](,[0-4])*$ ]]; then
	echo "--input-seq expects a comma-separated list using values 0..4" >&2
	exit 1
fi

case "$input_api" in
procfs|v4l2)
	;;
*)
	echo "--input-api expects procfs or v4l2" >&2
	exit 1
	;;
esac

if [[ -n "$input_reg" && ! "$input_reg" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--input-reg expects a decimal or hex register offset" >&2
	exit 1
fi

if [[ -n "$input_mask" && ! "$input_mask" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--input-mask expects a decimal or hex mask" >&2
	exit 1
fi

if [[ -n "$input_shift" && ! "$input_shift" =~ ^[0-9]+$ ]]; then
	echo "--input-shift expects an integer bit shift" >&2
	exit 1
fi

if [[ -n "$bitrate_reg" && ! "$bitrate_reg" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--bitrate-reg expects a decimal or hex register offset" >&2
	exit 1
fi

if [[ -n "$bitrate_mask" && ! "$bitrate_mask" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--bitrate-mask expects a decimal or hex mask" >&2
	exit 1
fi

if [[ -n "$bitrate_shift" && ! "$bitrate_shift" =~ ^[0-9]+$ ]]; then
	echo "--bitrate-shift expects an integer bit shift" >&2
	exit 1
fi

if [[ -n "$quality_reg" && ! "$quality_reg" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--quality-reg expects a decimal or hex register offset" >&2
	exit 1
fi

if [[ -n "$quality_mask" && ! "$quality_mask" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--quality-mask expects a decimal or hex mask" >&2
	exit 1
fi

if [[ -n "$quality_shift" && ! "$quality_shift" =~ ^[0-9]+$ ]]; then
	echo "--quality-shift expects an integer bit shift" >&2
	exit 1
fi

if [[ -n "$gop_reg" && ! "$gop_reg" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--gop-reg expects a decimal or hex register offset" >&2
	exit 1
fi

if [[ -n "$gop_mask" && ! "$gop_mask" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--gop-mask expects a decimal or hex mask" >&2
	exit 1
fi

if [[ -n "$gop_shift" && ! "$gop_shift" =~ ^[0-9]+$ ]]; then
	echo "--gop-shift expects an integer bit shift" >&2
	exit 1
fi

if [[ -n "$b_frames_reg" && ! "$b_frames_reg" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--b-frames-reg expects a decimal or hex register offset" >&2
	exit 1
fi

if [[ -n "$b_frames_mask" && ! "$b_frames_mask" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--b-frames-mask expects a decimal or hex mask" >&2
	exit 1
fi

if [[ -n "$b_frames_shift" && ! "$b_frames_shift" =~ ^[0-9]+$ ]]; then
	echo "--b-frames-shift expects an integer bit shift" >&2
	exit 1
fi

if [[ -n "$qp_step_reg" && ! "$qp_step_reg" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--qp-step-reg expects a decimal or hex register offset" >&2
	exit 1
fi

if [[ -n "$qp_step_mask" && ! "$qp_step_mask" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--qp-step-mask expects a decimal or hex mask" >&2
	exit 1
fi

if [[ -n "$qp_step_shift" && ! "$qp_step_shift" =~ ^[0-9]+$ ]]; then
	echo "--qp-step-shift expects an integer bit shift" >&2
	exit 1
fi

if [[ -n "$record_mode_reg" && ! "$record_mode_reg" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--record-mode-reg expects a decimal or hex register offset" >&2
	exit 1
fi

if [[ -n "$record_mode_mask" && ! "$record_mode_mask" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]]; then
	echo "--record-mode-mask expects a decimal or hex mask" >&2
	exit 1
fi

if [[ -n "$record_mode_shift" && ! "$record_mode_shift" =~ ^[0-9]+$ ]]; then
	echo "--record-mode-shift expects an integer bit shift" >&2
	exit 1
fi

if [[ -n "$video_node" && ! "$video_node" =~ ^/dev/video[0-9]+$ ]]; then
	echo "--video-node expects a path like /dev/video0" >&2
	exit 1
fi

if [[ "$input_api" == "v4l2" ]]; then
	enable_video=1
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
module_path="${script_dir}/mz0380.ko"

mkdir -p "$out_dir"

prefix="${out_dir}/mz0380.p${profile}"

capture_phase() {
	local phase=$1

	cat /proc/mz0380-control > "${prefix}.control.${phase}.txt"
	cat /proc/mz0380-experiment > "${prefix}.experiment.${phase}.txt"
	cat /proc/mz0380-snapshot > "${prefix}.snapshot.${phase}.txt"
	cat /proc/mz0380-state > "${prefix}.state.${phase}.txt"
}

capture_v4l2_phase() {
	local phase=$1

	if [[ $capture_v4l2 -eq 0 ]]; then
		return
	fi

	v4l2-ctl --device="$video_node" --all \
		> "${prefix}.v4l2.all.${phase}.txt"
	v4l2-ctl --device="$video_node" --get-input \
		> "${prefix}.v4l2.input.${phase}.txt"
	v4l2-ctl --device="$video_node" --list-inputs \
		> "${prefix}.v4l2.inputs.${phase}.txt"
}

show_diff() {
	local label=$1
	local before=$2
	local after=$3
	local status=0

	echo
	echo "=== ${label} diff ==="
	if ! diff -u "$before" "$after"; then
		status=$?
		if [[ $status -gt 1 ]]; then
			echo "diff failed for ${label}" >&2
			exit $status
		fi
	fi
}

show_optional_diff() {
	local label=$1
	local before=$2
	local after=$3

	if [[ -f "$before" && -f "$after" ]]; then
		show_diff "$label" "$before" "$after"
	fi
}

capture_step() {
	local suffix=$1

	cat /proc/mz0380-control > "${prefix}.control.${suffix}.txt"
	cat /proc/mz0380-experiment > "${prefix}.experiment.${suffix}.txt"
	cat /proc/mz0380-snapshot > "${prefix}.snapshot.${suffix}.txt"
	cat /proc/mz0380-state > "${prefix}.state.${suffix}.txt"
	capture_v4l2_phase "$suffix"
}

configure_experiment() {
	local writes_enabled=0

	if [[ -z "$input_reg" ]]; then
		:
	else
		echo "Configuring property-201 candidate BAR5 field: reg=${input_reg} mask=${input_mask} shift=${input_shift}"
		printf 'candidate %s %s %s\n' \
			"$input_reg" "$input_mask" "$input_shift" |
			sudo tee /proc/mz0380-experiment >/dev/null
		writes_enabled=1
	fi

	if [[ -n "$record_mode_reg" ]]; then
		echo "Configuring property-407 candidate BAR5 field: reg=${record_mode_reg} mask=${record_mode_mask} shift=${record_mode_shift}"
		printf 'recordmode-candidate %s %s %s\n' \
			"$record_mode_reg" "$record_mode_mask" "$record_mode_shift" |
			sudo tee /proc/mz0380-experiment >/dev/null
		writes_enabled=1
	fi

	if [[ -n "$bitrate_reg" ]]; then
		echo "Configuring property-403 candidate BAR5 field: reg=${bitrate_reg} mask=${bitrate_mask} shift=${bitrate_shift}"
		printf 'bitrate-candidate %s %s %s\n' \
			"$bitrate_reg" "$bitrate_mask" "$bitrate_shift" |
			sudo tee /proc/mz0380-experiment >/dev/null
		writes_enabled=1
	fi

	if [[ -n "$quality_reg" ]]; then
		echo "Configuring property-404 candidate BAR5 field: reg=${quality_reg} mask=${quality_mask} shift=${quality_shift}"
		printf 'quality-candidate %s %s %s\n' \
			"$quality_reg" "$quality_mask" "$quality_shift" |
			sudo tee /proc/mz0380-experiment >/dev/null
		writes_enabled=1
	fi

	if [[ -n "$gop_reg" ]]; then
		echo "Configuring property-405 candidate BAR5 field: reg=${gop_reg} mask=${gop_mask} shift=${gop_shift}"
		printf 'gop-candidate %s %s %s\n' \
			"$gop_reg" "$gop_mask" "$gop_shift" |
			sudo tee /proc/mz0380-experiment >/dev/null
		writes_enabled=1
	fi

	if [[ -n "$b_frames_reg" ]]; then
		echo "Configuring property-411 candidate BAR5 field: reg=${b_frames_reg} mask=${b_frames_mask} shift=${b_frames_shift}"
		printf 'bframes-candidate %s %s %s\n' \
			"$b_frames_reg" "$b_frames_mask" "$b_frames_shift" |
			sudo tee /proc/mz0380-experiment >/dev/null
		writes_enabled=1
	fi

	if [[ -n "$qp_step_reg" ]]; then
		echo "Configuring property-408 candidate BAR5 field: reg=${qp_step_reg} mask=${qp_step_mask} shift=${qp_step_shift}"
		printf 'qpstep-candidate %s %s %s\n' \
			"$qp_step_reg" "$qp_step_mask" "$qp_step_shift" |
			sudo tee /proc/mz0380-experiment >/dev/null
		writes_enabled=1
	fi

	if [[ $writes_enabled -eq 1 ]]; then
		echo "Enabling experimental BAR5 writes"
		printf 'writes on\n' | sudo tee /proc/mz0380-experiment >/dev/null
	fi
}

resolve_video_node() {
	local state_file=$1
	local detected_node
	local node

	if [[ -n "$video_node" ]]; then
		return
	fi

	detected_node=$(extract_first_match '^  video node : \(/dev/video[0-9]\+\)$' "$state_file")
	if [[ -n "$detected_node" ]]; then
		video_node="$detected_node"
		return
	fi

	detected_node=$(extract_first_match '^  video node : \(/dev/video[0-9]\+\)$' "${prefix}.control.before.txt")
	if [[ -n "$detected_node" ]]; then
		video_node="$detected_node"
		return
	fi

	detected_node=$(extract_first_match '^  video node : \(/dev/video[0-9]\+\)$' /proc/mz0380)
	if [[ -n "$detected_node" ]]; then
		video_node="$detected_node"
		return
	fi

	for node in /dev/video*; do
		if [[ ! -e "$node" ]]; then
			continue
		fi

		if v4l2-ctl --device="$node" --all 2>/dev/null |
		   grep -q 'Driver name[[:space:]]*:[[:space:]]*mz0380'; then
			video_node="$node"
			return
		fi
	done
}

validate_v4l2_path() {
	if command -v v4l2-ctl >/dev/null 2>&1; then
		have_v4l2_ctl=1
	else
		have_v4l2_ctl=0
	fi

	if [[ "$input_api" == "v4l2" && $have_v4l2_ctl -eq 0 ]]; then
		echo "v4l2-ctl is required for --input-api v4l2" >&2
		exit 1
	fi

	if [[ $enable_video -eq 0 || $have_v4l2_ctl -eq 0 ]]; then
		return
	fi

	resolve_video_node "${prefix}.state.before.txt"
	if [[ -z "$video_node" ]]; then
		if [[ "$input_api" == "v4l2" ]]; then
			echo "Could not auto-detect a /dev/video node; use --video-node /dev/videoN" >&2
			exit 1
		fi
		return
	fi

	if [[ ! -e "$video_node" ]]; then
		if [[ "$input_api" == "v4l2" ]]; then
			echo "Configured video node does not exist: $video_node" >&2
			exit 1
		fi
		return
	fi

	capture_v4l2=1
}

run_input_request() {
	local input_value=$1

	echo "Issuing property-201 style input-select request: ${input_value} via ${input_api}"
	case "$input_api" in
	procfs)
		if [[ -n "$input_reg" ]]; then
			echo "Configured candidate BAR5 field: reg=${input_reg} mask=${input_mask} shift=${input_shift}"
		else
			echo "Using fixed BAR5 field: reg=0x0040 mask=0x7 shift=0"
		fi
		printf 'input %s\n' "$input_value" | sudo tee /proc/mz0380-experiment >/dev/null
		;;
	v4l2)
		echo "Using V4L2 node: ${video_node}"
		v4l2-ctl --device="$video_node" --set-input="$input_value"
		;;
	esac
}

run_input_sequence() {
	local value
	local step=0
	local -a values

	IFS=',' read -r -a values <<< "$experiment_sequence"
	for value in "${values[@]}"; do
		run_input_request "$value"
		capture_step "seq${step}.input${value}"
		step=$((step + 1))
	done

	last_sequence_input="${values[${#values[@]} - 1]}"
}

run_record_mode_request() {
	local mode_value=$1

	echo "Issuing property-407 style record-mode request: ${mode_value} via procfs"
	if [[ -n "$record_mode_reg" ]]; then
		echo "Configured candidate BAR5 field: reg=${record_mode_reg} mask=${record_mode_mask} shift=${record_mode_shift}"
	else
		echo "Using fixed BAR5 field: reg=0x0058 mask=0x3 shift=0"
	fi
	printf 'recordmode %s\n' "$mode_value" | sudo tee /proc/mz0380-experiment >/dev/null
}

run_bitrate_request() {
	local bitrate_value=$1

	echo "Issuing property-403 style bitrate request: ${bitrate_value} via procfs"
	if [[ -n "$bitrate_reg" ]]; then
		echo "Configured candidate BAR5 field: reg=${bitrate_reg} mask=${bitrate_mask} shift=${bitrate_shift}"
	else
		echo "Using fixed BAR5 field: reg=0x005c mask=0xffffffff shift=0"
	fi
	printf 'bitrate %s\n' "$bitrate_value" | sudo tee /proc/mz0380-experiment >/dev/null
}

run_quality_request() {
	local quality_value=$1

	echo "Issuing property-404 style quality request: ${quality_value} via procfs"
	if [[ -n "$quality_reg" ]]; then
		echo "Configured candidate BAR5 field: reg=${quality_reg} mask=${quality_mask} shift=${quality_shift}"
	else
		echo "Using fixed BAR5 field: reg=0x0060 mask=0xffffffff shift=0"
	fi
	printf 'quality %s\n' "$quality_value" | sudo tee /proc/mz0380-experiment >/dev/null
}

run_gop_request() {
	local gop_value=$1

	echo "Issuing property-405 style GOP request: ${gop_value} via procfs"
	if [[ -n "$gop_reg" ]]; then
		echo "Configured candidate BAR5 field: reg=${gop_reg} mask=${gop_mask} shift=${gop_shift}"
	else
		echo "Using fixed BAR5 field: reg=0x0080 mask=0xffffffff shift=0"
	fi
	printf 'gop %s\n' "$gop_value" | sudo tee /proc/mz0380-experiment >/dev/null
}

run_b_frames_request() {
	local b_frames_value=$1

	echo "Issuing property-411 style B-frame request: ${b_frames_value} via procfs"
	if [[ -n "$b_frames_reg" ]]; then
		echo "Configured candidate BAR5 field: reg=${b_frames_reg} mask=${b_frames_mask} shift=${b_frames_shift}"
	else
		echo "Using fixed BAR5 field: reg=0x0088 mask=0xffffffff shift=0"
	fi
	printf 'bframes %s\n' "$b_frames_value" | sudo tee /proc/mz0380-experiment >/dev/null
}

run_qp_step_request() {
	local qp_step_value=$1

	echo "Issuing property-408 style QP-step request: ${qp_step_value} via procfs"
	if [[ -n "$qp_step_reg" ]]; then
		echo "Configured candidate BAR5 field: reg=${qp_step_reg} mask=${qp_step_mask} shift=${qp_step_shift}"
	else
		echo "Using fixed BAR5 field: reg=0x0084 mask=0xffffffff shift=0"
	fi
	printf 'qpstep %s\n' "$qp_step_value" | sudo tee /proc/mz0380-experiment >/dev/null
}

run_quality_sequence() {
	local value
	local step=0
	local -a values

	IFS=',' read -r -a values <<< "$experiment_quality_sequence"
	for value in "${values[@]}"; do
		run_quality_request "$value"
		capture_step "seq${step}.quality${value}"
		step=$((step + 1))
	done
}

run_bitrate_sequence() {
	local value
	local step=0
	local -a values

	IFS=',' read -r -a values <<< "$experiment_bitrate_sequence"
	for value in "${values[@]}"; do
		run_bitrate_request "$value"
		capture_step "seq${step}.bitrate${value}"
		step=$((step + 1))
	done
}

run_gop_sequence() {
	local value
	local step=0
	local -a values

	IFS=',' read -r -a values <<< "$experiment_gop_sequence"
	for value in "${values[@]}"; do
		run_gop_request "$value"
		capture_step "seq${step}.gop${value}"
		step=$((step + 1))
	done
}

run_b_frames_sequence() {
	local value
	local step=0
	local -a values

	IFS=',' read -r -a values <<< "$experiment_b_frames_sequence"
	for value in "${values[@]}"; do
		run_b_frames_request "$value"
		capture_step "seq${step}.bframes${value}"
		step=$((step + 1))
	done
}

run_qp_step_sequence() {
	local value
	local step=0
	local -a values

	IFS=',' read -r -a values <<< "$experiment_qp_step_sequence"
	for value in "${values[@]}"; do
		run_qp_step_request "$value"
		capture_step "seq${step}.qpstep${value}"
		step=$((step + 1))
	done
}

extract_first_match() {
	local pattern=$1
	local file=$2

	sed -n "s@${pattern}@\\1@p" "$file" | head -n 1
}

extract_snapshot_word() {
	local reg=$1
	local file=$2

	sed -n "s@^    [^ ]* *\\[0x$(printf '%04x' "$((reg))")\\] = \\([0-9a-f]\\{8\\}\\)\$@\\1@p" "$file" | head -n 1
}

show_sequence_summary() {
	local value
	local step=0
	local -a values
	local experiment_file
	local snapshot_file
	local state_file
	local requested
	local result
	local write_triplet
	local cfg_0040
	local state_input
	local state_input_hw
	local v4l2_input

	if [[ -z "$experiment_sequence" ]]; then
		return
	fi

	IFS=',' read -r -a values <<< "$experiment_sequence"

	echo
	echo "=== sequence summary ==="
	for value in "${values[@]}"; do
		experiment_file="${prefix}.experiment.seq${step}.input${value}.txt"
		snapshot_file="${prefix}.snapshot.seq${step}.input${value}.txt"
		state_file="${prefix}.state.seq${step}.input${value}.txt"

		requested=$(extract_first_match '^  requested  : .* (\([0-4]\))$' "$experiment_file")
		result=$(extract_first_match '^  result     : \(.*\)$' "$experiment_file")
		write_triplet=$(extract_first_match '^               before=\([0-9a-f]\{8\} programmed=[0-9a-f]\{8\} readback=[0-9a-f]\{8\}\)$' "$experiment_file")
		cfg_0040=$(extract_first_match '^    cfg_ext_0040     \[0x0040\] = \([0-9a-f]\{8\}\)$' "$snapshot_file")
		state_input=$(extract_first_match '^  input      : \(.*\)$' "$state_file")
		state_input_hw=$(extract_first_match '^  input hw   : \(.*\)$' "$state_file")
		v4l2_input=$(extract_first_match '^Video input : \(.*\)$' "${prefix}.v4l2.input.seq${step}.input${value}.txt")

		echo "step ${step}: input=${value} requested=${requested:-unknown} cfg0040=${cfg_0040:-unknown} state=${state_input:-unknown}"
		if [[ -n "$state_input_hw" ]]; then
			echo "  hw=${state_input_hw}"
		fi
		if [[ -n "$v4l2_input" ]]; then
			echo "  v4l2=${v4l2_input}"
		fi
		if [[ -n "$result" ]]; then
			echo "  result=${result}"
		fi
		if [[ -n "$write_triplet" ]]; then
			echo "  write=${write_triplet}"
		fi

		step=$((step + 1))
	done
}

show_bitrate_sequence_summary() {
	local value
	local step=0
	local -a values
	local experiment_file
	local snapshot_file
	local state_file
	local requested
	local result
	local write_triplet
	local cfg_bitrate
	local state_bitrate
	local state_bitrate_hw
	local summary_bitrate_reg=${bitrate_reg:-0x005c}

	if [[ -z "$experiment_bitrate_sequence" ]]; then
		return
	fi

	IFS=',' read -r -a values <<< "$experiment_bitrate_sequence"

	echo
	echo "=== bitrate sequence summary ==="
	for value in "${values[@]}"; do
		experiment_file="${prefix}.experiment.seq${step}.bitrate${value}.txt"
		snapshot_file="${prefix}.snapshot.seq${step}.bitrate${value}.txt"
		state_file="${prefix}.state.seq${step}.bitrate${value}.txt"

		requested=$(extract_first_match '^  requested  : \([0-9]\+\)$' "$experiment_file")
		result=$(extract_first_match '^  result     : \(.*\)$' "$experiment_file")
		write_triplet=$(extract_first_match '^               before=\([0-9a-f]\{8\} programmed=[0-9a-f]\{8\} readback=[0-9a-f]\{8\}\)$' "$experiment_file")
		cfg_bitrate=$(extract_snapshot_word "$summary_bitrate_reg" "$snapshot_file")
		state_bitrate=$(extract_first_match '^  bitrate    : \(.*\)$' "$state_file")
		state_bitrate_hw=$(extract_first_match '^  bitrate hw : \(.*\)$' "$state_file")

		echo "step ${step}: bitrate=${value} requested=${requested:-unknown} cfg=${cfg_bitrate:-unknown} state=${state_bitrate:-unknown}"
		if [[ -n "$state_bitrate_hw" ]]; then
			echo "  hw=${state_bitrate_hw}"
		fi
		if [[ -n "$result" ]]; then
			echo "  result=${result}"
		fi
		if [[ -n "$write_triplet" ]]; then
			echo "  write=${write_triplet}"
		fi

		step=$((step + 1))
	done
}

show_quality_sequence_summary() {
	local value
	local step=0
	local -a values
	local experiment_file
	local snapshot_file
	local state_file
	local requested
	local result
	local write_triplet
	local cfg_quality
	local state_quality
	local state_quality_hw
	local summary_quality_reg=${quality_reg:-0x0060}

	if [[ -z "$experiment_quality_sequence" ]]; then
		return
	fi

	IFS=',' read -r -a values <<< "$experiment_quality_sequence"

	echo
	echo "=== quality sequence summary ==="
	for value in "${values[@]}"; do
		experiment_file="${prefix}.experiment.seq${step}.quality${value}.txt"
		snapshot_file="${prefix}.snapshot.seq${step}.quality${value}.txt"
		state_file="${prefix}.state.seq${step}.quality${value}.txt"

		requested=$(extract_first_match '^  requested  : \([0-9]\+\)$' "$experiment_file")
		result=$(extract_first_match '^  result     : \(.*\)$' "$experiment_file")
		write_triplet=$(extract_first_match '^               before=\([0-9a-f]\{8\} programmed=[0-9a-f]\{8\} readback=[0-9a-f]\{8\}\)$' "$experiment_file")
		cfg_quality=$(extract_snapshot_word "$summary_quality_reg" "$snapshot_file")
		state_quality=$(extract_first_match '^  quality    : \(.*\)$' "$state_file")
		state_quality_hw=$(extract_first_match '^  quality hw : \(.*\)$' "$state_file")

		echo "step ${step}: quality=${value} requested=${requested:-unknown} cfg=${cfg_quality:-unknown} state=${state_quality:-unknown}"
		if [[ -n "$state_quality_hw" ]]; then
			echo "  hw=${state_quality_hw}"
		fi
		if [[ -n "$result" ]]; then
			echo "  result=${result}"
		fi
		if [[ -n "$write_triplet" ]]; then
			echo "  write=${write_triplet}"
		fi

		step=$((step + 1))
	done
}

show_gop_sequence_summary() {
	local value
	local step=0
	local -a values
	local experiment_file
	local snapshot_file
	local state_file
	local requested
	local result
	local write_triplet
	local cfg_gop
	local state_gop
	local state_gop_hw
	local summary_gop_reg=${gop_reg:-0x0080}

	if [[ -z "$experiment_gop_sequence" ]]; then
		return
	fi

	IFS=',' read -r -a values <<< "$experiment_gop_sequence"

	echo
	echo "=== gop sequence summary ==="
	for value in "${values[@]}"; do
		experiment_file="${prefix}.experiment.seq${step}.gop${value}.txt"
		snapshot_file="${prefix}.snapshot.seq${step}.gop${value}.txt"
		state_file="${prefix}.state.seq${step}.gop${value}.txt"

		requested=$(extract_first_match '^  requested  : \([0-9]\+\)$' "$experiment_file")
		result=$(extract_first_match '^  result     : \(.*\)$' "$experiment_file")
		write_triplet=$(extract_first_match '^               before=\([0-9a-f]\{8\} programmed=[0-9a-f]\{8\} readback=[0-9a-f]\{8\}\)$' "$experiment_file")
		cfg_gop=$(extract_snapshot_word "$summary_gop_reg" "$snapshot_file")
		state_gop=$(extract_first_match '^  gop        : \(.*\)$' "$state_file")
		state_gop_hw=$(extract_first_match '^  gop hw     : \(.*\)$' "$state_file")

		echo "step ${step}: gop=${value} requested=${requested:-unknown} cfg=${cfg_gop:-unknown} state=${state_gop:-unknown}"
		if [[ -n "$state_gop_hw" ]]; then
			echo "  hw=${state_gop_hw}"
		fi
		if [[ -n "$result" ]]; then
			echo "  result=${result}"
		fi
		if [[ -n "$write_triplet" ]]; then
			echo "  write=${write_triplet}"
		fi

		step=$((step + 1))
	done
}

show_b_frames_sequence_summary() {
	local value
	local step=0
	local -a values
	local experiment_file
	local snapshot_file
	local state_file
	local requested
	local result
	local write_triplet
	local cfg_b_frames
	local state_b_frames
	local state_b_frames_candidate
	local summary_b_frames_reg=${b_frames_reg:-0x0088}

	if [[ -z "$experiment_b_frames_sequence" ]]; then
		return
	fi

	IFS=',' read -r -a values <<< "$experiment_b_frames_sequence"

	echo
	echo "=== b-frames sequence summary ==="
	for value in "${values[@]}"; do
		experiment_file="${prefix}.experiment.seq${step}.bframes${value}.txt"
		snapshot_file="${prefix}.snapshot.seq${step}.bframes${value}.txt"
		state_file="${prefix}.state.seq${step}.bframes${value}.txt"

		requested=$(extract_first_match '^  requested  : \([0-9]\+\)$' "$experiment_file")
		result=$(extract_first_match '^  result     : \(.*\)$' "$experiment_file")
		write_triplet=$(extract_first_match '^               before=\([0-9a-f]\{8\} programmed=[0-9a-f]\{8\} readback=[0-9a-f]\{8\}\)$' "$experiment_file")
		cfg_b_frames=$(extract_snapshot_word "$summary_b_frames_reg" "$snapshot_file")
		state_b_frames=$(extract_first_match '^  b-frames   : \(.*\)$' "$state_file")
		state_b_frames_candidate=$(extract_first_match '^  bframes cand: \(.*\)$' "$state_file")

		echo "step ${step}: bframes=${value} requested=${requested:-unknown} cfg=${cfg_b_frames:-unknown} state=${state_b_frames:-unknown}"
		if [[ -n "$state_b_frames_candidate" ]]; then
			echo "  candidate=${state_b_frames_candidate}"
		fi
		if [[ -n "$result" ]]; then
			echo "  result=${result}"
		fi
		if [[ -n "$write_triplet" ]]; then
			echo "  write=${write_triplet}"
		fi

		step=$((step + 1))
	done
}

show_qp_step_sequence_summary() {
	local value
	local step=0
	local -a values
	local experiment_file
	local snapshot_file
	local state_file
	local requested
	local result
	local write_triplet
	local cfg_qp_step
	local state_qp_step
	local state_qp_step_candidate
	local summary_qp_step_reg=${qp_step_reg:-0x0084}

	if [[ -z "$experiment_qp_step_sequence" ]]; then
		return
	fi

	IFS=',' read -r -a values <<< "$experiment_qp_step_sequence"

	echo
	echo "=== qp-step sequence summary ==="
	for value in "${values[@]}"; do
		experiment_file="${prefix}.experiment.seq${step}.qpstep${value}.txt"
		snapshot_file="${prefix}.snapshot.seq${step}.qpstep${value}.txt"
		state_file="${prefix}.state.seq${step}.qpstep${value}.txt"

		requested=$(extract_first_match '^  requested  : \([0-9]\+\)$' "$experiment_file")
		result=$(extract_first_match '^  result     : \(.*\)$' "$experiment_file")
		write_triplet=$(extract_first_match '^               before=\([0-9a-f]\{8\} programmed=[0-9a-f]\{8\} readback=[0-9a-f]\{8\}\)$' "$experiment_file")
		cfg_qp_step=$(extract_snapshot_word "$summary_qp_step_reg" "$snapshot_file")
		state_qp_step=$(extract_first_match '^  qpstep     : \(.*\)$' "$state_file")
		state_qp_step_candidate=$(extract_first_match '^  qpstep cand: \(.*\)$' "$state_file")

		echo "step ${step}: qpstep=${value} requested=${requested:-unknown} cfg=${cfg_qp_step:-unknown} state=${state_qp_step:-unknown}"
		if [[ -n "$state_qp_step_candidate" ]]; then
			echo "  candidate=${state_qp_step_candidate}"
		fi
		if [[ -n "$result" ]]; then
			echo "  result=${result}"
		fi
		if [[ -n "$write_triplet" ]]; then
			echo "  write=${write_triplet}"
		fi

		step=$((step + 1))
	done
}

show_starting_candidate_summary() {
	local before_snapshot="${prefix}.snapshot.before.txt"
	local before_state="${prefix}.state.before.txt"
	local cfg_0040
	local cfg_bitrate
	local cfg_quality
	local cfg_gop
	local cfg_b_frames
	local cfg_qp_step
	local cfg_record
	local state_input
	local state_input_hw
	local state_bitrate
	local state_bitrate_hw
	local state_quality
	local state_quality_hw
	local state_gop
	local state_gop_hw
	local state_b_frames
	local state_b_frames_candidate
	local state_qp_step
	local state_qp_step_candidate
	local state_record
	local state_record_hw
	local before_v4l2_input

	if [[ -z "$input_reg" && -z "$bitrate_reg" && -z "$quality_reg" &&
	      -z "$gop_reg" && -z "$b_frames_reg" && -z "$qp_step_reg" &&
	      -z "$record_mode_reg" ]]; then
		return
	fi

	echo
	echo "=== starting candidate state ==="
	if [[ -n "$input_reg" ]]; then
		cfg_0040=$(extract_first_match '^    cfg_ext_0040     \[0x0040\] = \([0-9a-f]\{8\}\)$' "$before_snapshot")
		state_input=$(extract_first_match '^  input      : \(.*\)$' "$before_state")
		state_input_hw=$(extract_first_match '^  input hw   : \(.*\)$' "$before_state")
		before_v4l2_input=$(extract_first_match '^Video input : \(.*\)$' "${prefix}.v4l2.input.before.txt")
		echo "input field   : reg=${input_reg} mask=${input_mask} shift=${input_shift}"
		echo "before cfg0040: ${cfg_0040:-unknown}"
		echo "before input  : ${state_input:-unknown}"
		if [[ -n "$state_input_hw" ]]; then
			echo "before hw     : ${state_input_hw}"
		fi
		if [[ -n "$before_v4l2_input" ]]; then
			echo "before v4l2   : ${before_v4l2_input}"
		fi
	fi

	if [[ -n "$record_mode_reg" ]]; then
		cfg_record=$(extract_snapshot_word "$record_mode_reg" "$before_snapshot")
		state_record=$(extract_first_match '^  recordmode : \(.*\)$' "$before_state")
		state_record_hw=$(extract_first_match '^  record hw  : \(.*\)$' "$before_state")
		echo "record field  : reg=${record_mode_reg} mask=${record_mode_mask} shift=${record_mode_shift}"
		echo "before cfg    : ${cfg_record:-unknown}"
		echo "before record : ${state_record:-unknown}"
		if [[ -n "$state_record_hw" ]]; then
			echo "before hw     : ${state_record_hw}"
		fi
	fi

	if [[ -n "$bitrate_reg" ]]; then
		cfg_bitrate=$(extract_snapshot_word "$bitrate_reg" "$before_snapshot")
		state_bitrate=$(extract_first_match '^  bitrate    : \(.*\)$' "$before_state")
		state_bitrate_hw=$(extract_first_match '^  bitrate hw : \(.*\)$' "$before_state")
		echo "bitrate field : reg=${bitrate_reg} mask=${bitrate_mask} shift=${bitrate_shift}"
		echo "before cfg    : ${cfg_bitrate:-unknown}"
		echo "before bitrate: ${state_bitrate:-unknown}"
		if [[ -n "$state_bitrate_hw" ]]; then
			echo "before hw     : ${state_bitrate_hw}"
		fi
	fi

	if [[ -n "$quality_reg" ]]; then
		cfg_quality=$(extract_snapshot_word "$quality_reg" "$before_snapshot")
		state_quality=$(extract_first_match '^  quality    : \(.*\)$' "$before_state")
		state_quality_hw=$(extract_first_match '^  quality hw : \(.*\)$' "$before_state")
		echo "quality field : reg=${quality_reg} mask=${quality_mask} shift=${quality_shift}"
		echo "before cfg    : ${cfg_quality:-unknown}"
		echo "before quality: ${state_quality:-unknown}"
		if [[ -n "$state_quality_hw" ]]; then
			echo "before hw     : ${state_quality_hw}"
		fi
	fi

	if [[ -n "$gop_reg" ]]; then
		cfg_gop=$(extract_snapshot_word "$gop_reg" "$before_snapshot")
		state_gop=$(extract_first_match '^  gop        : \(.*\)$' "$before_state")
		state_gop_hw=$(extract_first_match '^  gop hw     : \(.*\)$' "$before_state")
		echo "gop field     : reg=${gop_reg} mask=${gop_mask} shift=${gop_shift}"
		echo "before cfg    : ${cfg_gop:-unknown}"
		echo "before gop    : ${state_gop:-unknown}"
		if [[ -n "$state_gop_hw" ]]; then
			echo "before hw     : ${state_gop_hw}"
		fi
	fi

	if [[ -n "$b_frames_reg" ]]; then
		cfg_b_frames=$(extract_snapshot_word "$b_frames_reg" "$before_snapshot")
		state_b_frames=$(extract_first_match '^  b-frames   : \(.*\)$' "$before_state")
		state_b_frames_candidate=$(extract_first_match '^  bframes cand: \(.*\)$' "$before_state")
		echo "bframes field : reg=${b_frames_reg} mask=${b_frames_mask} shift=${b_frames_shift}"
		echo "before cfg    : ${cfg_b_frames:-unknown}"
		echo "before bframes: ${state_b_frames:-unknown}"
		if [[ -n "$state_b_frames_candidate" ]]; then
			echo "before cand   : ${state_b_frames_candidate}"
		fi
	fi

	if [[ -n "$qp_step_reg" ]]; then
		cfg_qp_step=$(extract_snapshot_word "$qp_step_reg" "$before_snapshot")
		state_qp_step=$(extract_first_match '^  qpstep     : \(.*\)$' "$before_state")
		state_qp_step_candidate=$(extract_first_match '^  qpstep cand: \(.*\)$' "$before_state")
		echo "qpstep field  : reg=${qp_step_reg} mask=${qp_step_mask} shift=${qp_step_shift}"
		echo "before cfg    : ${cfg_qp_step:-unknown}"
		echo "before qpstep : ${state_qp_step:-unknown}"
		if [[ -n "$state_qp_step_candidate" ]]; then
			echo "before cand   : ${state_qp_step_candidate}"
		fi
	fi
}

load_dependencies() {
	local depends dep

	depends=$(modinfo -F depends "$module_path" 2>/dev/null || true)
	if [[ -z "$depends" ]]; then
		return
	fi

	IFS=',' read -r -a dep_list <<< "$depends"
	for dep in "${dep_list[@]}"; do
		if [[ -z "$dep" ]]; then
			continue
		fi

		echo "Loading dependency: $dep"
		sudo modprobe "$dep"
	done
}

build_step() {
	if [[ $build_module -eq 0 ]]; then
		echo "Skipping build step"
		return
	fi

	echo "Building mz0380.ko"
	make -C "/lib/modules/$(uname -r)/build" \
		M="$script_dir" \
		CC=clang \
		LD=ld.lld \
		mz0380.ko
}

echo "Output prefix: ${prefix}"
build_step

if [[ ! -f "$module_path" ]]; then
	echo "Module not found after build: $module_path" >&2
	exit 1
fi

echo "Loading mz0380 with snapshot_profile=${profile}"

sudo rmmod mz0380 2>/dev/null || true
load_dependencies
insmod_args=(
	procfs_verbosity=2
	"allow_bus_master=${allow_bus_master_arg}"
	"snapshot_profile=${profile}"
)

if [[ -n "$input_reg" ]]; then
	insmod_args+=(
		"input_select_reg=${input_reg}"
		"input_select_mask=${input_mask}"
		"input_select_shift=${input_shift}"
	)
fi

if [[ -n "$bitrate_reg" ]]; then
	insmod_args+=(
		"bitrate_reg=${bitrate_reg}"
		"bitrate_mask=${bitrate_mask}"
		"bitrate_shift=${bitrate_shift}"
	)
fi

if [[ -n "$quality_reg" ]]; then
	insmod_args+=(
		"quality_reg=${quality_reg}"
		"quality_mask=${quality_mask}"
		"quality_shift=${quality_shift}"
	)
fi

if [[ -n "$gop_reg" ]]; then
	insmod_args+=(
		"gop_reg=${gop_reg}"
		"gop_mask=${gop_mask}"
		"gop_shift=${gop_shift}"
	)
fi

if [[ -n "$b_frames_reg" ]]; then
	insmod_args+=(
		"b_frames_reg=${b_frames_reg}"
		"b_frames_mask=${b_frames_mask}"
		"b_frames_shift=${b_frames_shift}"
	)
fi

if [[ -n "$qp_step_reg" ]]; then
	insmod_args+=(
		"qp_step_reg=${qp_step_reg}"
		"qp_step_mask=${qp_step_mask}"
		"qp_step_shift=${qp_step_shift}"
	)
fi

if [[ -n "$record_mode_reg" ]]; then
	insmod_args+=(
		"record_mode_reg=${record_mode_reg}"
		"record_mode_mask=${record_mode_mask}"
		"record_mode_shift=${record_mode_shift}"
	)
fi

if [[ $enable_video -eq 1 ]]; then
	insmod_args+=(enable_video=1)
fi

if [[ $enable_dma -eq 1 ]]; then
	insmod_args+=(enable_dma=1)
fi

if [[ $enable_audio -eq 1 ]]; then
	insmod_args+=(enable_audio=1)
fi

if ! sudo insmod "$module_path" "${insmod_args[@]}"; then
	echo "insmod failed; recent kernel log follows:" >&2
	sudo journalctl -k --since "2 minutes ago" | grep -E 'mz0380|AMD-Vi|IOMMU|videodev|v4l2' >&2 || true
	exit 1
fi

configure_experiment
capture_phase before
validate_v4l2_path
capture_v4l2_phase before

echo
echo "Before capture complete."
echo "Artifacts:"
echo "  ${prefix}.control.before.txt"
echo "  ${prefix}.experiment.before.txt"
echo "  ${prefix}.snapshot.before.txt"
echo "  ${prefix}.state.before.txt"
if [[ $capture_v4l2 -eq 1 ]]; then
	echo "  V4L2 node: ${video_node}"
	echo "  ${prefix}.v4l2.all.before.txt"
	echo "  ${prefix}.v4l2.input.before.txt"
	echo "  ${prefix}.v4l2.inputs.before.txt"
fi
show_starting_candidate_summary

if [[ -n "$experiment_sequence" ]]; then
	echo
	echo "Issuing property-201 style input-select sequence: ${experiment_sequence}"
	run_input_sequence
	if [[ -n "$sleep_seconds" ]]; then
		echo "Waiting ${sleep_seconds} seconds after the final input request..."
		sleep "$sleep_seconds"
	fi
elif [[ -n "$experiment_bitrate_sequence" ]]; then
	echo
	echo "Issuing property-403 style bitrate sequence: ${experiment_bitrate_sequence}"
	run_bitrate_sequence
	if [[ -n "$sleep_seconds" ]]; then
		echo "Waiting ${sleep_seconds} seconds after the final bitrate request..."
		sleep "$sleep_seconds"
	fi
elif [[ -n "$experiment_quality_sequence" ]]; then
	echo
	echo "Issuing property-404 style quality sequence: ${experiment_quality_sequence}"
	run_quality_sequence
	if [[ -n "$sleep_seconds" ]]; then
		echo "Waiting ${sleep_seconds} seconds after the final quality request..."
		sleep "$sleep_seconds"
	fi
elif [[ -n "$experiment_gop_sequence" ]]; then
	echo
	echo "Issuing property-405 style GOP sequence: ${experiment_gop_sequence}"
	run_gop_sequence
	if [[ -n "$sleep_seconds" ]]; then
		echo "Waiting ${sleep_seconds} seconds after the final GOP request..."
		sleep "$sleep_seconds"
	fi
elif [[ -n "$experiment_b_frames_sequence" ]]; then
	echo
	echo "Issuing property-411 style B-frame sequence: ${experiment_b_frames_sequence}"
	run_b_frames_sequence
	if [[ -n "$sleep_seconds" ]]; then
		echo "Waiting ${sleep_seconds} seconds after the final B-frame request..."
		sleep "$sleep_seconds"
	fi
elif [[ -n "$experiment_qp_step_sequence" ]]; then
	echo
	echo "Issuing property-408 style QP-step sequence: ${experiment_qp_step_sequence}"
	run_qp_step_sequence
	if [[ -n "$sleep_seconds" ]]; then
		echo "Waiting ${sleep_seconds} seconds after the final QP-step request..."
		sleep "$sleep_seconds"
	fi
elif [[ -n "$experiment_input" ]]; then
	echo
	run_input_request "$experiment_input"
	if [[ -n "$sleep_seconds" ]]; then
		echo "Waiting ${sleep_seconds} seconds after the input request..."
		sleep "$sleep_seconds"
	fi
elif [[ -n "$experiment_record_mode" ]]; then
	echo
	run_record_mode_request "$experiment_record_mode"
	if [[ -n "$sleep_seconds" ]]; then
		echo "Waiting ${sleep_seconds} seconds after the record-mode request..."
		sleep "$sleep_seconds"
	fi
elif [[ -n "$experiment_gop" ]]; then
	echo
	run_gop_request "$experiment_gop"
	if [[ -n "$sleep_seconds" ]]; then
		echo "Waiting ${sleep_seconds} seconds after the GOP request..."
		sleep "$sleep_seconds"
	fi
elif [[ -n "$experiment_b_frames" ]]; then
	echo
	run_b_frames_request "$experiment_b_frames"
	if [[ -n "$sleep_seconds" ]]; then
		echo "Waiting ${sleep_seconds} seconds after the B-frame request..."
		sleep "$sleep_seconds"
	fi
elif [[ -n "$experiment_qp_step" ]]; then
	echo
	run_qp_step_request "$experiment_qp_step"
	if [[ -n "$sleep_seconds" ]]; then
		echo "Waiting ${sleep_seconds} seconds after the QP-step request..."
		sleep "$sleep_seconds"
	fi
elif [[ -n "$experiment_quality" ]]; then
	echo
	run_quality_request "$experiment_quality"
	if [[ -n "$sleep_seconds" ]]; then
		echo "Waiting ${sleep_seconds} seconds after the quality request..."
		sleep "$sleep_seconds"
	fi
elif [[ -n "$experiment_bitrate" ]]; then
	echo
	run_bitrate_request "$experiment_bitrate"
	if [[ -n "$sleep_seconds" ]]; then
		echo "Waiting ${sleep_seconds} seconds after the bitrate request..."
		sleep "$sleep_seconds"
	fi
elif [[ -n "$sleep_seconds" ]]; then
	echo
	echo "Change the hardware state now."
	echo "Examples: unplug/replug HDMI, stop/start source, or switch 1080p60 <-> 720p60."
	echo "Waiting ${sleep_seconds} seconds..."
	sleep "$sleep_seconds"
else
	echo
	echo "Change the hardware state now."
	echo "Examples: unplug/replug HDMI, stop/start source, or switch 1080p60 <-> 720p60."
	printf 'Press Enter to capture the "after" state... '
	read -r _
fi

capture_phase after
capture_v4l2_phase after

echo
echo "After capture complete."

show_diff "control" \
	"${prefix}.control.before.txt" \
	"${prefix}.control.after.txt"
show_optional_diff "experimsent" \
	"${prefix}.experiment.before.txt" \
	"${prefix}.experiment.after.txt"
show_diff "snapshot" \
	"${prefix}.snapshot.before.txt" \
	"${prefix}.snapshot.after.txt"
show_diff "state" \
	"${prefix}.state.before.txt" \
	"${prefix}.state.after.txt"
show_optional_diff "v4l2 all" \
	"${prefix}.v4l2.all.before.txt" \
	"${prefix}.v4l2.all.after.txt"
show_optional_diff "v4l2 input" \
	"${prefix}.v4l2.input.before.txt" \
	"${prefix}.v4l2.input.after.txt"
show_optional_diff "v4l2 inputs" \
	"${prefix}.v4l2.inputs.before.txt" \
	"${prefix}.v4l2.inputs.after.txt"
show_sequence_summary
show_bitrate_sequence_summary
show_quality_sequence_summary
show_gop_sequence_summary
show_b_frames_sequence_summary
show_qp_step_sequence_summary

echo
echo "=== kernel log ==="
sudo journalctl -k --since "5 minutes ago" | grep -E 'mz0380|AMD-Vi|IOMMU' || true

if [[ $keep_loaded -eq 0 ]]; then
	echo
	echo "Unloading mz0380"
	sudo rmmod mz0380
fi

echo
echo "Done."
echo "Saved artifacts:"
echo "  ${prefix}.control.before.txt"
echo "  ${prefix}.control.after.txt"
echo "  ${prefix}.experiment.before.txt"
echo "  ${prefix}.experiment.after.txt"
echo "  ${prefix}.snapshot.before.txt"
echo "  ${prefix}.snapshot.after.txt"
echo "  ${prefix}.state.before.txt"
echo "  ${prefix}.state.after.txt"
if [[ $capture_v4l2 -eq 1 ]]; then
	echo "  ${prefix}.v4l2.all.before.txt"
	echo "  ${prefix}.v4l2.all.after.txt"
	echo "  ${prefix}.v4l2.input.before.txt"
	echo "  ${prefix}.v4l2.input.after.txt"
	echo "  ${prefix}.v4l2.inputs.before.txt"
	echo "  ${prefix}.v4l2.inputs.after.txt"
fi
if [[ -n "$experiment_sequence" ]]; then
	echo "  ${prefix}.control.seq*.txt"
	echo "  ${prefix}.experiment.seq*.txt"
	echo "  ${prefix}.snapshot.seq*.txt"
	echo "  ${prefix}.state.seq*.txt"
	if [[ $capture_v4l2 -eq 1 ]]; then
		echo "  ${prefix}.v4l2.all.seq*.txt"
		echo "  ${prefix}.v4l2.input.seq*.txt"
		echo "  ${prefix}.v4l2.inputs.seq*.txt"
	fi
fi
if [[ -n "$experiment_bitrate_sequence" ]]; then
	echo "  ${prefix}.control.seq*.txt"
	echo "  ${prefix}.experiment.seq*.txt"
	echo "  ${prefix}.snapshot.seq*.txt"
	echo "  ${prefix}.state.seq*.txt"
fi
if [[ -n "$experiment_quality_sequence" ]]; then
	echo "  ${prefix}.control.seq*.txt"
	echo "  ${prefix}.experiment.seq*.txt"
	echo "  ${prefix}.snapshot.seq*.txt"
	echo "  ${prefix}.state.seq*.txt"
fi
if [[ -n "$experiment_qp_step_sequence" ]]; then
	echo "  ${prefix}.control.seq*.txt"
	echo "  ${prefix}.experiment.seq*.txt"
	echo "  ${prefix}.snapshot.seq*.txt"
	echo "  ${prefix}.state.seq*.txt"
fi
