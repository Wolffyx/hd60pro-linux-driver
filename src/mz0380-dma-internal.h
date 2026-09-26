/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _MZ0380_DMA_INTERNAL_H_
#define _MZ0380_DMA_INTERNAL_H_

#include <linux/delay.h>
#include <linux/iommu.h>
#include <linux/kthread.h>
#include <linux/vmalloc.h>

#include "mz0380.h"

#define MZ0380_ENC_VALID_FPS	BIT(0)
#define MZ0380_ENC_VALID_GOP	BIT(1)
#define MZ0380_ENC_VALID_BITRATE	BIT(6)
#define MZ0380_ENC_SAFE_FPS	60
#define MZ0380_ENC_SAFE_GOP	60
#define MZ0380_ENC_SAFE_BITRATE	(4 * 1024 * 1024)

void mz0380_drain_work_fn(struct work_struct *w);
void mz0380_audio_work_fn(struct work_struct *work);
void mz0380_audio_bufs_free(struct mz0380_dev *dev);
void mz0380_enc_stat_ack(struct mz0380_dev *dev);
void mz0380_frame_buffer_repoison(struct mz0380_dev *dev, u32 idx);
void mz0380_raw_probe_buffer_repoison(struct mz0380_dev *dev, u32 idx);
void mz0380_poll_drain_start(struct mz0380_dev *dev);
void mz0380_poll_drain_stop(struct mz0380_dev *dev);
void mz0380_extent_watch_stop(struct mz0380_dev *dev);
void mz0380_extent_watch_start(struct mz0380_dev *dev);
void mz0380_stream_bufs_free(struct mz0380_dev *dev);
void mz0380_h264_bufs_free(struct mz0380_dev *dev);
void mz0380_raw_probe_bufs_free(struct mz0380_dev *dev);
void mz0380_stream_bufs_dump(struct mz0380_dev *dev, const char *tag);
void mz0380_h264_bufs_dump(struct mz0380_dev *dev, const char *tag);
void mz0380_raw_probe_bufs_dump(struct mz0380_dev *dev, const char *tag);
void mz0380_frame_events_start(struct mz0380_dev *dev);
int mz0380_h264_program_bufs(struct mz0380_dev *dev);
int mz0380_stream_program_bufs(struct mz0380_dev *dev);
void mz0380_frame_buffers_poison_start(struct mz0380_dev *dev);
u8 mz0380_timings_fps(const struct v4l2_dv_timings *timings);
u8 mz0380_poison_b(void);
u32 mz0380_poison_w(void);
u32 mz0380_frame_poison_w(const struct mz0380_dev *dev);
u64 mz0380_frame_poison_q(const struct mz0380_dev *dev);
void __mz0380_dma_stop(struct mz0380_dev *dev, bool verbose);
int mz0380_infer_frame_length(struct mz0380_dev *dev, u32 idx,
			      size_t *length);
bool mz0380_raw_probe_frame_landed(struct mz0380_dev *dev, u32 idx);
bool mz0380_raw_probe_frame_filled(struct mz0380_dev *dev, u32 idx);
bool mz0380_raw_probe_chroma_filled(struct mz0380_dev *dev, u32 idx);
void mz0380_raw_probe_sentinel_repoison(struct mz0380_dev *dev, u32 idx);
void mz0380_raw_probe_sentinel_save(struct mz0380_dev *dev, u32 idx, u32 out[4]);
void mz0380_raw_probe_sentinel_restore(struct mz0380_dev *dev, void *dst,
				       const u32 in[4]);
void mz0380_raw_copy_frame(struct mz0380_dev *dev, void *dst, const void *src,
			   size_t frame);
int mz0380_raw_probe_infer_length(struct mz0380_dev *dev, u32 idx,
				  size_t *length);

#endif
