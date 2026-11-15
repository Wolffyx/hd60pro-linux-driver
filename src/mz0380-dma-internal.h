/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _MZ0380_DMA_INTERNAL_H_
#define _MZ0380_DMA_INTERNAL_H_

#include <linux/iommu.h>
#include <linux/vmalloc.h>

#include "mz0380.h"

/* SET_VIC configures video channel zero; channel_done sets its EVENT bit. */
#define MZ0380_VIDEO_EVENT_BIT		BIT(MZ0380_STREAM_VIDEO_CHANNEL)

/* mz0380-dma.c */
void mz0380_frame_events_start(struct mz0380_dev *dev);
void mz0380_enc_stat_ack(struct mz0380_dev *dev);
int mz0380_dma_program_bufs(struct mz0380_dev *dev);
int mz0380_h264_program_bufs(struct mz0380_dev *dev);
void mz0380_audio_work_fn(struct work_struct *work);

/* mz0380-dma-drain.c */
void mz0380_drain_work_fn(struct work_struct *w);

/* mz0380-dma-raw.c */
u8 mz0380_raw_poison_byte(u32 idx);
bool mz0380_raw_frame_landed(struct mz0380_dev *dev, u32 idx);
bool mz0380_raw_frame_filled(struct mz0380_dev *dev, u32 idx);
bool mz0380_raw_chroma_filled(struct mz0380_dev *dev, u32 idx);
void mz0380_cadence_note(struct mz0380_dev *dev, u32 *hist, u64 *prev,
			 u64 now);
bool mz0380_raw_sentinel_rewritten(struct mz0380_dev *dev, u32 idx);
void mz0380_raw_sentinel_save(struct mz0380_dev *dev, u32 idx, u32 out[4]);
void mz0380_raw_sentinel_restore(struct mz0380_dev *dev, void *dst,
				 const u32 in[4]);
void mz0380_raw_sentinel_repoison(struct mz0380_dev *dev, u32 idx);
void mz0380_raw_copy_frame(struct mz0380_dev *dev, void *dst, const void *src,
			   size_t frame);

#endif
