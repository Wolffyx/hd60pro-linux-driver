// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 module parameters.
 *
 * The defaults are the validated capture configuration: `modprobe mz0380` with
 * no options is what every hardware run since M216 used. The ~130 reverse-
 * engineering knobs that used to live here were retired in M247; the values
 * they held are now constants at their single point of use, and git history
 * (and RE_FINDINGS.md) keeps what each of them was for.
 */

#include "mz0380.h"

bool mz0380_enable_video = true;
module_param_named(enable_video, mz0380_enable_video, bool, 0444);
MODULE_PARM_DESC(enable_video,
		 "register the V4L2 capture node (default 1). 0 brings the card up without a node, for a zero-spawn load/unload smoke test");

bool mz0380_raw_deliver = true;
module_param_named(raw_deliver, mz0380_raw_deliver, bool, 0444);
MODULE_PARM_DESC(raw_deliver,
		 "startup format: 1 = uncompressed YU12 (default), 0 = H.264. Applications choose freely with VIDIOC_S_FMT either way; this only decides what one that never asks gets");

bool mz0380_persistent_h264 = true;
module_param_named(persistent_h264, mz0380_persistent_h264, bool, 0444);
MODULE_PARM_DESC(persistent_h264,
		 "keep the card's encoder running between captures (default 1). Each restart spends one of the 8-18 encoder spawns the card survives per power cycle; leave this on");

bool mz0380_stream_without_signal = true;
module_param_named(stream_without_signal, mz0380_stream_without_signal, bool, 0644);
MODULE_PARM_DESC(stream_without_signal,
		 "let capture start with no HDMI source and show a NO SIGNAL picture until one appears (default 1). 0 makes STREAMON fail instead");

unsigned int mz0380_h264_frame_divisor;
module_param_named(h264_frame_divisor, mz0380_h264_frame_divisor, uint, 0444);
MODULE_PARM_DESC(h264_frame_divisor,
		 "H.264 frame schedule: 0 = every input frame (default), N >= 2 = one in N (2 halves 60 to 30 fps). 1 is invalid and treated as 2");

unsigned int mz0380_post_skip;
module_param_named(post_skip, mz0380_post_skip, uint, 0444);
MODULE_PARM_DESC(post_skip,
		 "raw capture decimation: deliver one in post_skip+1 source frames (default 0 = every frame). Lowers PCIe bandwidth on a slow slot");

bool mz0380_raw_full_range;
module_param_named(raw_full_range, mz0380_raw_full_range, bool, 0644);
MODULE_PARM_DESC(raw_full_range,
		 "label raw formats as full-range (0-255) instead of limited (16-235, default). Changes only the colorimetry reported to applications");

unsigned long long mz0380_dma_iova_base = 0x100000000ULL;
module_param_named(dma_iova_base, mz0380_dma_iova_base, ullong, 0444);
MODULE_PARM_DESC(dma_iova_base,
		 "4 GiB-aligned IOVA where the DMA windows start (default 0x100000000). Change only if the driver reports the range is already mapped");

unsigned int mz0380_mode_follow = 4;
module_param_named(mode_follow, mz0380_mode_follow, uint, 0644);
MODULE_PARM_DESC(mode_follow,
		 "how many times per load the driver may restart the encoder by itself when the source comes back in another frame rate at the same size (default 4; each costs one encoder spawn). 0 = never: NO SIGNAL stays up until the application restarts the capture");

unsigned int mz0380_fault_inject;
module_param_named(fault_inject, mz0380_fault_inject, uint, 0644);
MODULE_PARM_DESC(fault_inject,
		 "TEST ONLY (default 0). 1 = map the H.264 and audio slots from fragmented pages as if the contiguous allocation had failed; 2 = as 1, and fail that mapping half-way to exercise its unwinding (streaming is then unavailable); 3 = STREAMON behaves as if PCI bus mastering were off. 1 and 2 are read at load");

bool mz0380_enable_audio;
module_param_named(enable_audio, mz0380_enable_audio, bool, 0444);
MODULE_PARM_DESC(enable_audio,
		 "register an ALSA capture device for HDMI audio (experimental, default 0)");

/* SET_AIC_PARAMS (op 0x2a). Audio is the only user of the non-default values. */
bool mz0380_aic_every_frame;
module_param_named(aic_every_frame, mz0380_aic_every_frame, bool, 0644);
MODULE_PARM_DESC(aic_every_frame,
		 "send SET_AIC at every pipeline start, not only the first (default 0)");

unsigned int mz0380_aic_channels = 2;
module_param_named(aic_channels, mz0380_aic_channels, uint, 0644);
MODULE_PARM_DESC(aic_channels, "SET_AIC channel count (default 2)");

unsigned int mz0380_aic_bits = 16;
module_param_named(aic_bits, mz0380_aic_bits, uint, 0644);
MODULE_PARM_DESC(aic_bits, "SET_AIC sample width in bits (default 16)");

unsigned int mz0380_aic_freq = 48000;
module_param_named(aic_freq, mz0380_aic_freq, uint, 0644);
MODULE_PARM_DESC(aic_freq, "SET_AIC sample rate in Hz (default 48000)");

unsigned int mz0380_aic_period_frames = 256;
module_param_named(aic_period_frames, mz0380_aic_period_frames, uint, 0644);
MODULE_PARM_DESC(aic_period_frames,
		 "SET_AIC frames per period (default 256, the card's own default)");

unsigned int mz0380_aic_periods = 4;
module_param_named(aic_periods, mz0380_aic_periods, uint, 0644);
MODULE_PARM_DESC(aic_periods, "SET_AIC period count (default 4)");

unsigned int mz0380_aic_int_mode = 1;
module_param_named(aic_int_mode, mz0380_aic_int_mode, uint, 0644);
MODULE_PARM_DESC(aic_int_mode,
		 "SET_AIC interrupt mode: 1 = one EVENT bit per audio slot, as Windows sends (default)");
