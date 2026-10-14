// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 raw-frame slot helpers: completeness probes and the copy out.
 *
 * The card writes I420 frames into the four window-0 bank-0 slots and raises
 * no completion of its own for them (M215); the raw drain scans the slots on
 * every encoded completion instead. Everything here answers one of two
 * questions about a slot - does it hold a whole, finished frame, and how does
 * that frame get into a vb2 buffer in the layout the application asked for.
 */

#include "mz0380-dma-internal.h"

u8 mz0380_raw_poison_byte(u32 idx)
{
	return idx < MZ0380_STREAM_NR_BUFS ? MZ0380_RAW_BANK0_POISON :
					     MZ0380_RAW_BANK1_POISON;
}

static u32 mz0380_raw_poison_dword(u32 idx)
{
	return 0x01010101u * mz0380_raw_poison_byte(idx);
}

/*
 * M221: the raw payload length is ALIGN16(W) * H * 3/2 (M213, read out of
 * tinyvenc7), not the 1080p constant: the W/H come from our own SET_VIC.
 */
size_t mz0380_raw_frame_bytes_for(u32 width, u32 height)
{
	if (!width || !height)
		return MZ0380_RAW_FRAME_SIZE_1080P;
	return (size_t)ALIGN(width, 16) * height * 3 / 2;
}

size_t mz0380_raw_frame_bytes(struct mz0380_dev *dev)
{
	return mz0380_raw_frame_bytes_for(dev->capture.width,
					  dev->capture.height);
}

/* The slot and frame a probe may look at, or NULL if there is none. */
static const u8 *mz0380_raw_slot(struct mz0380_dev *dev, u32 idx,
				 size_t *frame)
{
	const struct mz0380_raw_buf *b;

	if (idx >= MZ0380_RAW_NR_BUFS)
		return NULL;
	b = &dev->raw_bufs[idx];
	*frame = mz0380_raw_frame_bytes(dev);
	if (!b->va || *frame < 4096 || *frame > MZ0380_RAW_BUF_SIZE)
		return NULL;
	return b->va;
}

/*
 * Sentinels, as offsets into the frame rather than fixed byte positions.
 *
 * [0] is the last dword and is what actually proves completion, because an
 * ascending DMA writes it last. The rest are spread back through the frame and
 * exist only because that ascending-order assumption is inferred rather than
 * proven: if the card ever writes planes or blocks out of order, requiring the
 * others too still rejects a partial frame. They cost a false negative only
 * when real pixel data happens to equal the poison dword at one of these
 * offsets, which costs one frame and cannot persist.
 */
static void mz0380_raw_sentinel_offsets(size_t frame, size_t out[4])
{
	out[0] = frame - 4;
	out[1] = frame - frame / 16;
	out[2] = frame - frame / 4;
	out[3] = frame / 2;
}

/*
 * M217/M220: has the card written over EVERY sentinel? A transfer still in
 * flight has written the early sentinels and not yet the late ones, so "any"
 * would pass a half-written frame. This only proves the poison is gone - see
 * mz0380_raw_frame_filled() for why that is not the same as a finished frame.
 */
bool mz0380_raw_frame_landed(struct mz0380_dev *dev, u32 idx)
{
	u32 poison = mz0380_raw_poison_dword(idx);
	size_t frame, off[4];
	const u8 *va = mz0380_raw_slot(dev, idx, &frame);
	unsigned int i;

	if (!va)
		return false;
	mz0380_raw_sentinel_offsets(frame, off);
	dma_rmb();
	for (i = 0; i < 4; i++)
		if (READ_ONCE(*(const u32 *)(va + off[i])) == poison)
			return false;
	return true;
}

/*
 * M222/M247: did the card write over ANY sentinel since they were re-armed?
 * Used after the copy: the sentinels were poisoned just before it, so any one
 * of them changing means the card was writing this slot while it was read.
 * (M222 asked for all four, which needs the card to rewrite the bottom half
 * of the frame inside one copy - it effectively never fired.)
 */
bool mz0380_raw_sentinel_rewritten(struct mz0380_dev *dev, u32 idx)
{
	u32 poison = mz0380_raw_poison_dword(idx);
	size_t frame, off[4];
	const u8 *va = mz0380_raw_slot(dev, idx, &frame);
	unsigned int i;

	if (!va)
		return false;
	mz0380_raw_sentinel_offsets(frame, off);
	dma_rmb();
	for (i = 0; i < 4; i++)
		if (READ_ONCE(*(const u32 *)(va + off[i])) != poison)
			return true;
	return false;
}

/*
 * M229: has the card FINISHED writing this slot?
 *
 * M226 established on hardware that the card CLEARS a slot to black before
 * filling it - luma 0x01, chroma 0x80 - so the clear removes every sentinel at
 * once and "landed" goes true long before the picture arrives. The clear does
 * leave a signature: if the last luma rows still read entirely as the clear
 * byte, the fill has not reached the end of the picture.
 *
 * Sampled, not scanned. Any sample differing from the clear value proves the
 * region was written. The false negative is a frame whose last rows are flat
 * 0x01 - below video black (16), so not real content.
 */
bool mz0380_raw_frame_filled(struct mz0380_dev *dev, u32 idx)
{
	const u32 clear = 0x01010101u * MZ0380_RAW_CLEAR_LUMA;
	size_t frame, luma, region, base, step;
	const u8 *va = mz0380_raw_slot(dev, idx, &frame);
	unsigned int i;

	if (!va)
		return false;

	luma = frame / 3 * 2;	/* I420: luma is the first two thirds */
	region = (size_t)MZ0380_RAW_FILL_ROWS * dev->capture.width;
	if (!region || region > luma)
		region = luma;
	step = round_down(region / MZ0380_RAW_FILL_SAMPLES, 4);
	if (step < 4)
		return true;
	base = round_down(luma - region, 4);

	dma_rmb();
	for (i = 0; i < MZ0380_RAW_FILL_SAMPLES; i++)
		if (READ_ONCE(*(const u32 *)(va + base + i * step)) != clear)
			return true;
	return false;
}

/* How many of n samples spread over [base, base + len) read as @clear. */
static unsigned int mz0380_raw_count_clear(const u8 *va, size_t base,
					   size_t len, u32 clear)
{
	const unsigned int n = MZ0380_RAW_FILL_SAMPLES / 2;
	size_t step = round_down(len / n, 4);
	unsigned int i, matched = 0;

	if (step < 4)
		return 0;
	base = round_down(base, 4);
	for (i = 0; i < n; i++)
		if (READ_ONCE(*(const u32 *)(va + base + i * step)) == clear)
			matched++;
	return matched;
}

/* One source frame: the card fills a slot within it, or it could not keep up. */
static u64 mz0380_raw_frame_period_ns(struct mz0380_dev *dev)
{
	return div_u64(NSEC_PER_SEC, dev->capture.source_fps ?: 60);
}

/*
 * M251: count the spacing since the previous moment in @hist, in frame
 * periods: <.25, ~.5, ~1, ~1.5, ~2, >2.25. One frame in eight arrives right
 * behind its predecessor and the next two periods later (M250); whether the
 * card's events already do that, or only the delivery does, is the question.
 */
void mz0380_cadence_note(struct mz0380_dev *dev, u32 *hist, u64 *prev,
			 u64 now)
{
	u64 quarters;

	if (*prev && now > *prev) {
		quarters = div64_u64((now - *prev) * 4,
				     mz0380_raw_frame_period_ns(dev));
		hist[min_t(u64, (quarters + 1) / 2,
			   MZ0380_CADENCE_BUCKETS - 1)]++;
	}
	*prev = now;
}

/*
 * M238/M247: have the CHROMA planes been written too?
 *
 * I420 is Y, then U, then V, and the card fills ascending, so luma finishing
 * says nothing about chroma - and the clear value 0x80 is also neutral chroma
 * in real content, which is what makes this hard.
 *
 * M238 sampled U and V as one region and passed the slot as soon as any one
 * sample differed from the clear value, so a frame with U written and V still
 * clear passed (16/32 samples clear - exactly the high-water mark M244 kept
 * reporting), and that is the frame with a green or magenta cast. V is written
 * last, so V decides: any colour in V means the slot is done.
 *
 * V reading entirely clear is either a fill in progress or content whose Cr is
 * neutral - a black screen, a grey desktop, a navy loading screen - and one
 * look cannot tell them apart. Holding such frames back forever freezes the
 * picture on whatever came before the fade. Time can tell: the card writes a
 * whole frame within one frame period, so a slot whose V still reads clear a
 * frame period after it was first seen that way is content, and is delivered.
 * (Counting scans instead of time is wrong: a drain that is behind runs
 * several scans within microseconds.)
 */
bool mz0380_raw_chroma_filled(struct mz0380_dev *dev, u32 idx)
{
	const u32 clear = 0x01010101u * MZ0380_RAW_CLEAR_CHROMA;
	struct mz0380_raw_buf *b;
	size_t frame, luma, csize;
	const u8 *va = mz0380_raw_slot(dev, idx, &frame);
	unsigned int u_clear, v_clear;
	u64 now;

	if (!va)
		return false;
	b = &dev->raw_bufs[idx];

	luma = frame / 3 * 2;
	csize = (frame - luma) / 2;
	dma_rmb();
	u_clear = mz0380_raw_count_clear(va, luma, csize, clear);
	v_clear = mz0380_raw_count_clear(va, luma + csize, csize, clear);

	/* M244: prove the test runs, and how close real content comes. */
	dev->raw_chroma_probes++;
	dev->raw_chroma_max_clear = max(dev->raw_chroma_max_clear,
					u_clear + v_clear);

	if (v_clear < MZ0380_RAW_FILL_SAMPLES / 2) {
		b->neutral_since_ns = 0;
		return true;
	}
	now = ktime_get_ns();
	if (!b->neutral_since_ns) {
		b->neutral_since_ns = now;
		return false;
	}
	return now - b->neutral_since_ns >= mz0380_raw_frame_period_ns(dev);
}

/*
 * M239: the tear detector needs the poison in the SOURCE while the copy runs,
 * and the copy then carries it into the picture. Save the four real dwords
 * first and repair them in the DESTINATION once the copy is known good.
 */
void mz0380_raw_sentinel_save(struct mz0380_dev *dev, u32 idx, u32 out[4])
{
	size_t frame, off[4];
	const u8 *va = mz0380_raw_slot(dev, idx, &frame);
	unsigned int i;

	memset(out, 0, 4 * sizeof(*out));
	if (!va)
		return;
	mz0380_raw_sentinel_offsets(frame, off);
	dma_rmb();
	for (i = 0; i < 4; i++)
		out[i] = READ_ONCE(*(const u32 *)(va + off[i]));
}

/*
 * M241: put a saved byte back where the CHOSEN LAYOUT keeps it. The sentinel
 * offsets are positions in the card's I420 buffer; YV12 swaps the chroma
 * planes and NV12 interleaves them, so each byte is mapped individually.
 */
static void mz0380_raw_restore_byte(void *dst, u32 fourcc, size_t frame,
				    size_t off, u8 val)
{
	size_t luma = frame / 3 * 2;
	size_t csize = (frame - luma) / 2;
	size_t idx;

	if (off < luma || fourcc == V4L2_PIX_FMT_YUV420) {
		((u8 *)dst)[off] = val;		/* luma, or no re-ordering */
		return;
	}

	if (off < luma + csize) {
		idx = off - luma;		/* a U sample */
		if (fourcc == V4L2_PIX_FMT_YVU420)
			((u8 *)dst)[luma + csize + idx] = val;
		else				/* NV12: U is the even byte */
			((u8 *)dst)[luma + 2 * idx] = val;
		return;
	}

	idx = off - luma - csize;		/* a V sample */
	if (fourcc == V4L2_PIX_FMT_YVU420)
		((u8 *)dst)[luma + idx] = val;
	else					/* NV12: V is the odd byte */
		((u8 *)dst)[luma + 2 * idx + 1] = val;
}

void mz0380_raw_sentinel_restore(struct mz0380_dev *dev, void *dst,
				 const u32 in[4])
{
	size_t frame = mz0380_raw_frame_bytes(dev);
	size_t off[4];
	unsigned int i, b;
	u32 fourcc = dev->raw_fourcc ?: V4L2_PIX_FMT_YUV420;

	if (!dst || frame < 4096 || frame > MZ0380_RAW_BUF_SIZE)
		return;
	mz0380_raw_sentinel_offsets(frame, off);
	for (i = 0; i < 4; i++)
		for (b = 0; b < 4; b++)
			mz0380_raw_restore_byte(dst, fourcc, frame, off[i] + b,
						(in[i] >> (8 * b)) & 0xff);
}

void mz0380_raw_sentinel_repoison(struct mz0380_dev *dev, u32 idx)
{
	u32 poison = mz0380_raw_poison_dword(idx);
	size_t frame, off[4];
	u8 *va = (u8 *)mz0380_raw_slot(dev, idx, &frame);
	unsigned int i;

	if (!va)
		return;
	mz0380_raw_sentinel_offsets(frame, off);
	for (i = 0; i < 4; i++)
		WRITE_ONCE(*(u32 *)(va + off[i]), poison);
	dma_wmb();
	/* Re-armed: whatever the card writes next is a new frame. */
	dev->raw_bufs[idx].neutral_since_ns = 0;
}

/* Spread four bytes to the even bytes of a u64: abcd -> 0a0b0c0d. */
static inline u64 mz0380_spread_bytes(u32 x)
{
	u64 v = x;

	v = (v | (v << 16)) & 0x0000ffff0000ffffULL;
	return (v | (v << 8)) & 0x00ff00ff00ff00ffULL;
}

/*
 * NV12's interleaved chroma plane from I420's separate U and V planes, eight
 * source bytes of each per step. The kernel is built without SIMD, so the byte
 * loop this replaces ran one store per output byte over a megabyte per frame.
 */
static void mz0380_interleave_uv(u8 *out, const u8 *u, const u8 *v,
				 size_t n)
{
	size_t i = 0;

	for (; i + 8 <= n; i += 8, out += 16) {
		u64 uu = get_unaligned_le64(u + i);
		u64 vv = get_unaligned_le64(v + i);

		put_unaligned_le64(mz0380_spread_bytes(lower_32_bits(uu)) |
				   mz0380_spread_bytes(lower_32_bits(vv)) << 8,
				   out);
		put_unaligned_le64(mz0380_spread_bytes(upper_32_bits(uu)) |
				   mz0380_spread_bytes(upper_32_bits(vv)) << 8,
				   out + 8);
	}
	for (; i < n; i++) {
		*out++ = u[i];
		*out++ = v[i];
	}
}

/*
 * M241: copy one raw frame out, re-ordering chroma into the chosen layout.
 * The card writes I420; YV12 is the same planes with U and V exchanged and NV12
 * interleaves them, so neither needs any pixel arithmetic.
 */
void mz0380_raw_copy_frame(struct mz0380_dev *dev, void *dst, const void *src,
			   size_t frame)
{
	size_t luma = frame / 3 * 2;
	size_t csize = (frame - luma) / 2;
	const u8 *u = (const u8 *)src + luma;
	const u8 *v = u + csize;

	switch (dev->raw_fourcc) {
	case V4L2_PIX_FMT_YVU420:
		memcpy(dst, src, luma);
		memcpy((u8 *)dst + luma, v, csize);
		memcpy((u8 *)dst + luma + csize, u, csize);
		return;
	case V4L2_PIX_FMT_NV12:
		memcpy(dst, src, luma);
		mz0380_interleave_uv((u8 *)dst + luma, u, v, csize);
		return;
	default:
		memcpy(dst, src, frame);
		return;
	}
}
