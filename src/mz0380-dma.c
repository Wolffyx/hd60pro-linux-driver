// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Driver for MZ0380 based capture cards.
 *
 *  Frame delivery + MSI interrupt handling (RE_FINDINGS.md M17).
 *
 *  The card DMAs encoded H.264 frames into host-allocated buffers via a
 *  PCIe iATU outbound window it programs from the physical addresses we
 *  hand it with the buffer-setter mailbox opcodes. SET_VIC configures the
 *  encoder, START_STREAMING arms it, and a finished frame signals an EVENT
 *  (BAR0+0x30) whose token
 *  (BAR0+0x40, low 3 bits) names the completed buffer.
 *
 *  The endpoint does not mirror its card-side encoded-byte count to a verified
 *  host register: BAR0 payload words are packed slot counters, not lengths.
 *  Delivery therefore snapshots each one-shot event before ACK and infers a
 *  bounded, aligned extent from a per-buffer poison suffix before copying.
 */

#include "mz0380-dma-internal.h"

/* SET_VIC below configures video channel zero; channel_done sets its EVENT bit. */
#define MZ0380_VIDEO_EVENT_BIT BIT(MZ0380_STREAM_VIDEO_CHANNEL)

int mz0380_dma_ring_alloc(struct mz0380_dev *dev, struct mz0380_ring *r,
			  u32 nr_entries, u32 entry_size)
{
	size_t total;

	if (!nr_entries || !entry_size)
		return -EINVAL;

	total = (size_t)nr_entries * entry_size;
	r->buf = dma_alloc_coherent(&dev->pci->dev, total, &r->dma,
				    GFP_KERNEL);
	if (!r->buf)
		return -ENOMEM;

	r->total_size = total;
	r->entry_size = entry_size;
	r->nr_entries = nr_entries;
	r->head = 0;
	r->tail_seen = 0;
	return 0;
}

void mz0380_dma_ring_free(struct mz0380_dev *dev, struct mz0380_ring *r)
{
	if (r->buf) {
		dma_free_coherent(&dev->pci->dev, r->total_size,
				  r->buf, r->dma);
		r->buf = NULL;
	}
	r->total_size = 0;
	r->nr_entries = 0;
}

/* --- streaming buffer set (video channel 0) ------------------------------ */

void mz0380_stream_bufs_free(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int i;

	mz0380_extent_watch_stop(dev);	/* M36: never scan freed buffers */

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		struct mz0380_stream_buf *b = &dev->stream_bufs[i];

		if (!b->va)
			continue;

		if (b->pages) {
			/* M26 path: our own pages behind an explicit mapping */
			if (b->dma && domain)
				iommu_unmap(domain, b->dma,
					    MZ0380_STREAM_BUF_SIZE);
			__free_pages(b->pages,
				     get_order(MZ0380_STREAM_BUF_SIZE));
			b->pages = NULL;
		} else {
			dma_free_coherent(&dev->pci->dev, MZ0380_STREAM_BUF_SIZE,
					  b->va, b->dma);
		}
		b->va = NULL;
		b->dma = 0;
	}
}

void mz0380_h264_bufs_free(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int i;

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		struct mz0380_stream_buf *b = &dev->h264_bufs[i];

		if (!b->va)
			continue;
		if (b->pages) {
			if (b->dma && domain)
				iommu_unmap(domain, b->dma, MZ0380_H264_BUF_SIZE);
			__free_pages(b->pages, get_order(MZ0380_H264_BUF_SIZE));
			b->pages = NULL;
		} else {
			dma_free_coherent(&dev->pci->dev, MZ0380_H264_BUF_SIZE,
					  b->va, b->dma);
		}
		b->va = NULL;
		b->dma = 0;
		b->delivered = 0;
	}
}

void mz0380_raw_probe_bufs_free(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int i;

	for (i = 0; i < MZ0380_RAW_PROBE_NR_BUFS; i++) {
		struct mz0380_raw_probe_buf *b = &dev->raw_probe_bufs[i];
		unsigned int p;

		if (b->va) {
			vunmap(b->va);
			b->va = NULL;
		}
		if (b->mapped && b->dma && domain)
			iommu_unmap(domain, b->dma, b->mapped);
		b->mapped = 0;
		if (b->pages) {
			for (p = 0; p < b->nr_pages; p++)
				if (b->pages[p])
					__free_page(b->pages[p]);
			kfree(b->pages);
			b->pages = NULL;
		}
		b->nr_pages = 0;
		b->dma = 0;
	}
}

/*
 * The Windows raw-bank allocation is not a power of two.  Mapping individual
 * pages avoids eight fragile order-11 allocations (and wasting almost 29 MiB
 * in rounded tails) while still presenting each bank slot as one contiguous
 * card-visible IOVA and one contiguous diagnostic VA.
 */
static int mz0380_raw_probe_bufs_alloc_iova(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	const u32 nr_pages = MZ0380_RAW_PROBE_BUF_SIZE / PAGE_SIZE;
	unsigned int i;
	int ret = 0;

	if (!mz0380_dma_iova_remap || !domain)
		return -ENODEV;
	BUILD_BUG_ON(MZ0380_RAW_PROBE_BUF_SIZE % PAGE_SIZE);

	for (i = 0; i < MZ0380_RAW_PROBE_NR_BUFS; i++) {
		struct mz0380_raw_probe_buf *b = &dev->raw_probe_bufs[i];
		dma_addr_t iova = mz0380_dma_iova_base +
			((u64)(2 * MZ0380_STREAM_NR_BUFS + i) << 32) +
			mz0380_card_frame_offset + mz0380_dma_iova_offset;
		unsigned int p;

		b->pages = kcalloc(nr_pages, sizeof(*b->pages), GFP_KERNEL);
		if (!b->pages) {
			ret = -ENOMEM;
			goto err;
		}
		b->nr_pages = nr_pages;
		b->dma = iova;

		for (p = 0; p < nr_pages; p++) {
			phys_addr_t phys;

			b->pages[p] = alloc_page(GFP_KERNEL | __GFP_ZERO);
			if (!b->pages[p]) {
				ret = -ENOMEM;
				goto err;
			}
			phys = page_to_phys(b->pages[p]);
			if (iommu_iova_to_phys(domain, iova + (u64)p * PAGE_SIZE)) {
				pr_err("%s: raw-bank probe IOVA 0x%llx is already mapped\n",
				       dev->name,
				       (unsigned long long)(iova + (u64)p * PAGE_SIZE));
				ret = -EBUSY;
				goto err;
			}
			ret = iommu_map(domain, iova + (u64)p * PAGE_SIZE, phys,
					PAGE_SIZE, IOMMU_READ | IOMMU_WRITE,
					GFP_KERNEL);
			if (ret) {
				pr_err("%s: raw-bank iommu_map(buf=%u page=%u) failed (%d)\n",
				       dev->name, i, p, ret);
				goto err;
			}
			b->mapped += PAGE_SIZE;
		}

		b->va = vmap(b->pages, nr_pages, VM_MAP, PAGE_KERNEL);
		if (!b->va) {
			ret = -ENOMEM;
			goto err;
		}
		memset(b->va,
		       i < MZ0380_STREAM_NR_BUFS ?
			MZ0380_RAW_PROBE_BANK0_POISON :
			MZ0380_RAW_PROBE_BANK1_POISON,
		       MZ0380_RAW_PROBE_BUF_SIZE);
		pr_info("%s: raw-bank probe bank%u buf[%u] mapped at IOVA 0x%llx (%u pages)\n",
			dev->name, i / MZ0380_STREAM_NR_BUFS,
			i % MZ0380_STREAM_NR_BUFS,
			(unsigned long long)iova, nr_pages);
	}
	dma_wmb();
	return 0;

err:
	mz0380_raw_probe_bufs_free(dev);
	return ret;
}

/*
 * Keep window 1 on high dwords 5..8 when the normal raw ring occupies 1..4.
 * The card-side iATU path has the same high-32-bit target limitation for every
 * SET_BUF opcode, so each encoded slot needs its own 4 GiB-spaced IOVA too.
 */
static int mz0380_h264_bufs_alloc_iova(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int order = get_order(MZ0380_H264_BUF_SIZE);
	unsigned int i;
	int ret;

	if (!mz0380_dma_iova_remap || !domain)
		return -ENODEV;

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		struct mz0380_stream_buf *b = &dev->h264_bufs[i];
		dma_addr_t iova = mz0380_dma_iova_base +
			((u64)(MZ0380_STREAM_NR_BUFS + i) << 32);
		phys_addr_t phys;

		b->pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
		if (!b->pages) {
			ret = -ENOMEM;
			goto err;
		}
		b->va = page_address(b->pages);
		phys = page_to_phys(b->pages);

		if (iommu_iova_to_phys(domain, iova)) {
			pr_err("%s: H.264 probe IOVA 0x%llx is already mapped\n",
			       dev->name, (unsigned long long)iova);
			ret = -EBUSY;
			goto err;
		}
		ret = iommu_map(domain, iova, phys, MZ0380_H264_BUF_SIZE,
				IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
		if (ret) {
			pr_err("%s: H.264 iommu_map(0x%llx -> %pa, %u) failed (%d)\n",
			       dev->name, (unsigned long long)iova, &phys,
			       MZ0380_H264_BUF_SIZE, ret);
			goto err;
		}
		b->dma = iova;
		memset(b->va, MZ0380_H264_POISON_BYTE, MZ0380_H264_BUF_SIZE);
		pr_info("%s: H.264 probe buf[%u] phys=%pa mapped at IOVA 0x%llx\n",
			dev->name, i, &phys, (unsigned long long)iova);
	}
	wmb();
	return 0;

err:
	mz0380_h264_bufs_free(dev);
	return ret;
}

/*
 * M246: the audio window's four slots, each at its own 4 GiB-aligned IOVA after
 * every video window, with MZ0380_AUDIO_SLOT_MAP bytes mapped and poisoned so
 * the extent the card writes is measured rather than assumed.
 */
void mz0380_audio_bufs_free(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int i;

	for (i = 0; i < MZ0380_AUDIO_NR_SLOTS; i++) {
		struct mz0380_audio_buf *b = &dev->audio_bufs[i];

		if (!b->pages)
			continue;
		if (b->dma && domain)
			iommu_unmap(domain, b->dma, MZ0380_AUDIO_SLOT_MAP);
		__free_pages(b->pages, get_order(MZ0380_AUDIO_SLOT_MAP));
		b->pages = NULL;
		b->va = NULL;
		b->dma = 0;
	}
	dev->audio_capable = false;
}

static int mz0380_audio_bufs_alloc_iova(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int order = get_order(MZ0380_AUDIO_SLOT_MAP);
	unsigned int i;
	int ret;

	if (!mz0380_dma_iova_remap || !domain)
		return -ENODEV;

	for (i = 0; i < MZ0380_AUDIO_NR_SLOTS; i++) {
		struct mz0380_audio_buf *b = &dev->audio_bufs[i];
		dma_addr_t iova = mz0380_dma_iova_base +
			((u64)(MZ0380_AUDIO_IOVA_INDEX + i) << 32);
		phys_addr_t phys;

		b->pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
		if (!b->pages) {
			ret = -ENOMEM;
			goto err;
		}
		b->va = page_address(b->pages);
		phys = page_to_phys(b->pages);

		if (iommu_iova_to_phys(domain, iova)) {
			pr_err("%s: audio IOVA 0x%llx is already mapped\n",
			       dev->name, (unsigned long long)iova);
			ret = -EBUSY;
			goto err;
		}
		ret = iommu_map(domain, iova, phys, MZ0380_AUDIO_SLOT_MAP,
				IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
		if (ret) {
			pr_err("%s: audio iommu_map(0x%llx) failed (%d)\n",
			       dev->name, (unsigned long long)iova, ret);
			goto err;
		}
		b->dma = iova;
		memset(b->va, MZ0380_AUDIO_POISON_BYTE, MZ0380_AUDIO_SLOT_MAP);
	}
	wmb();	/* poison visible before the card can be told the address */
	dev->audio_capable = true;
	return 0;

err:
	mz0380_audio_bufs_free(dev);
	return ret;
}

/*
 * M246: op 0x03, exactly as Windows builds it - channel, the 4 KiB slot size,
 * then four {hi, lo} targets. Sent wherever the encoded window is (re)registered
 * so the two can never disagree about a spawn boundary.
 */
static int mz0380_audio_program_bufs(struct mz0380_dev *dev)
{
	u32 params[2 + 2 * MZ0380_AUDIO_NR_SLOTS] = { 0 };
	unsigned int i;
	int ret;

	params[0] = MZ0380_STREAM_VIDEO_CHANNEL;
	params[1] = MZ0380_AUDIO_SLOT_SIZE;
	for (i = 0; i < MZ0380_AUDIO_NR_SLOTS; i++) {
		struct mz0380_audio_buf *b = &dev->audio_bufs[i];

		if (!b->va || !b->dma)
			return -ENODEV;
		memset(b->va, MZ0380_AUDIO_POISON_BYTE, MZ0380_AUDIO_SLOT_MAP);
		params[2 + 2 * i] = upper_32_bits(b->dma);
		params[2 + 2 * i + 1] = lower_32_bits(b->dma);
	}
	wmb();	/* re-poisoned slots visible before op 0x03 hands them over */

	ret = mz0380_send_command(dev, MZ0380_CMD_SET_BUF_3, params,
				  ARRAY_SIZE(params), NULL, 2000);
	pr_info("%s: audio window registered (op 0x03, 4 x 0x%x at IOVA 0x%llx..) ret=%d\n",
		dev->name, MZ0380_AUDIO_SLOT_SIZE,
		(unsigned long long)dev->audio_bufs[0].dma, ret);
	return ret;
}

/*
 * M26. Put buffer i at IOVA (dma_iova_base + (i << 32)) so its low 32 bits are
 * zero, because that is the only half of the host target the card's outbound
 * window actually latches (M25, proven on hw: host_addr == (word0 << 32) +
 * aperture_offset). dma_alloc_coherent cannot be steered to a specific IOVA, so
 * we allocate raw pages and map them into the device's IOMMU domain ourselves.
 */
static int mz0380_stream_bufs_alloc_iova(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int order = get_order(MZ0380_STREAM_BUF_SIZE);
	unsigned int i;
	int ret;

	if (mz0380_dma_iova_base & 0xffffffffULL) {
		pr_err("%s: dma_iova_base 0x%llx is not 4GiB-aligned - the card would DMA to (base>>32)<<32, missing the buffer\n",
		       dev->name, mz0380_dma_iova_base);
		return -EINVAL;
	}

	/*
	 * M245: and above the first 4 GiB, which is where the DMA-API allocator
	 * is confined to (see the mask in mz0380_irq_request()).
	 */
	if (!upper_32_bits(mz0380_dma_iova_base)) {
		pr_err("%s: dma_iova_base 0x%llx is below 4GiB - that range belongs to the kernel's DMA allocator for this device\n",
		       dev->name, mz0380_dma_iova_base);
		return -EINVAL;
	}

	if (!domain) {
		pr_err("%s: no IOMMU domain for the device - cannot place buffers at a 4GiB-aligned IOVA. This card only latches the high 32 bits of a DMA target, so it needs IOMMU translation: boot without iommu=off/iommu=pt\n",
		       dev->name);
		return -ENODEV;
	}
	if (domain->type != IOMMU_DOMAIN_DMA &&
	    domain->type != IOMMU_DOMAIN_DMA_FQ &&
	    domain->type != IOMMU_DOMAIN_UNMANAGED) {
		pr_err("%s: IOMMU domain type %u is pass-through/identity - no address translation to exploit (boot without iommu=pt)\n",
		       dev->name, domain->type);
		return -ENODEV;
	}

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		struct mz0380_stream_buf *b = &dev->stream_bufs[i];
		/*
		 * M28: map card_frame_offset ABOVE the address we will
		 * advertise, so the card's constant +offset lands on the
		 * buffer base and word1 can stay 0 (no 32-bit wrap).
		 */
		dma_addr_t iova = mz0380_dma_iova_base + ((u64)i << 32) +
				  mz0380_card_frame_offset +
				  mz0380_dma_iova_offset;
		phys_addr_t phys;

		b->pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
		if (!b->pages) {
			ret = -ENOMEM;
			goto err;
		}
		b->va = page_address(b->pages);
		phys = page_to_phys(b->pages);

		if (iommu_iova_to_phys(domain, iova)) {
			pr_err("%s: IOVA 0x%llx is already mapped - pick another dma_iova_base\n",
			       dev->name, (unsigned long long)iova);
			ret = -EBUSY;
			goto err;
		}

		ret = iommu_map(domain, iova, phys, MZ0380_STREAM_BUF_SIZE,
				IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
		if (ret) {
			pr_err("%s: iommu_map(0x%llx -> %pa, %u) failed (%d)\n",
			       dev->name, (unsigned long long)iova, &phys,
			       MZ0380_STREAM_BUF_SIZE, ret);
			goto err;
		}
		b->dma = iova;

		pr_info("%s: stream buf[%u] phys=%pa mapped at IOVA 0x%llx (high32=0x%08x, low32=0)\n",
			dev->name, i, &phys, (unsigned long long)iova,
			upper_32_bits(iova));
	}

	dev->stream_head = 0;
	return 0;

err:
	mz0380_stream_bufs_free(dev);
	return ret;
}

static int mz0380_stream_bufs_alloc(struct mz0380_dev *dev)
{
	unsigned int i;
	int ret;

	if (mz0380_dma_iova_remap) {
		ret = mz0380_stream_bufs_alloc_iova(dev);
		if (!ret)
			return 0;
		pr_err("%s: 4GiB-aligned IOVA setup failed (%d); refusing to arm unreachable stream buffers\n",
			dev->name, ret);
		return ret;
	}

	/* Explicit diagnostic opt-out; normal hardware requires the IOVA path. */
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		dev->stream_bufs[i].va =
			dma_alloc_coherent(&dev->pci->dev,
					   MZ0380_STREAM_BUF_SIZE,
					   &dev->stream_bufs[i].dma, GFP_KERNEL);
		if (!dev->stream_bufs[i].va) {
			mz0380_stream_bufs_free(dev);
			return -ENOMEM;
		}
	}
	dev->stream_head = 0;
	return 0;
}

int mz0380_h264_program_bufs(struct mz0380_dev *dev)
{
	u32 params[2 + 2 * MZ0380_STREAM_NR_BUFS] = { 0 };
	unsigned int i;
	int ret;

	params[0] = MZ0380_STREAM_VIDEO_CHANNEL;
	params[1] = MZ0380_H264_SET_BUF_SIZE;
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		struct mz0380_stream_buf *b = &dev->h264_bufs[i];
		u64 target = b->dma;

		if (!b->va || !target)
			return -ENODEV;
		memset(b->va, MZ0380_H264_POISON_BYTE, MZ0380_H264_BUF_SIZE);
		b->delivered = 0;
		params[2 + 2 * i] = upper_32_bits(target);
		params[2 + 2 * i + 1] = lower_32_bits(target);
		pr_info("%s: H.264 SET_BUF_4[%u] target=0x%016llx -> slot {%08x, %08x}\n",
			dev->name, i, (unsigned long long)target,
			params[2 + 2 * i],
			params[2 + 2 * i + 1]);
	}
	wmb();

	ret = mz0380_send_command(dev, MZ0380_CMD_SET_BUF_4, params,
				  ARRAY_SIZE(params), NULL, 2000);
	pr_info("%s: H.264 probe registered dedicated window1 ring (op 0x04, size=0x%x) ret=%d\n",
		dev->name, MZ0380_H264_SET_BUF_SIZE, ret);
	/* M246: audio rides along; its failure must not cost the video. */
	if (!ret && dev->audio_capable) {
		int aret = mz0380_audio_program_bufs(dev);

		if (aret)
			pr_warn("%s: audio window registration failed (%d); video is unaffected\n",
				dev->name, aret);
	}
	return ret;
}

static int mz0380_raw_probe_program_bufs(struct mz0380_dev *dev)
{
	u32 params[2 + 2 * MZ0380_STREAM_NR_BUFS] = { 0 };
	static const u8 opcodes[MZ0380_RAW_PROBE_BANKS] = {
		MZ0380_CMD_SET_BUF_2,
		MZ0380_CMD_SET_BUF_8,
	};
	unsigned int bank, i;

	params[0] = MZ0380_STREAM_VIDEO_CHANNEL;
	params[1] = MZ0380_RAW_PROBE_BUF_SIZE;
	for (bank = 0; bank < MZ0380_RAW_PROBE_BANKS; bank++) {
		u8 poison = bank ? MZ0380_RAW_PROBE_BANK1_POISON :
				   MZ0380_RAW_PROBE_BANK0_POISON;
		int ret;

		for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
			struct mz0380_raw_probe_buf *b =
				&dev->raw_probe_bufs[bank * MZ0380_STREAM_NR_BUFS + i];
			u64 target;

			/*
			 * The banks are allocated in mz0380_dma_setup(),
			 * which runs at PCI probe - so raw_bank_probe /
			 * raw_bank_observe are read at INSMOD, not at stream
			 * start. Setting either through sysfs on an
			 * already-loaded module reaches this path with no
			 * banks behind it, and the bare -ENODEV surfaces as
			 * "SET_BUF failed (-19)" with nothing to say why.
			 */
			if (!b->va || !b->dma) {
				pr_err("%s: raw bank%u buf[%u] is not allocated - raw_bank_%s is a LOAD-TIME parameter (the banks come from mz0380_dma_setup at probe). Reload with RAWOBS=1 (or RAWPROBE=1) instead of setting it through sysfs\n",
				       dev->name, bank, i,
				       mz0380_raw_bank_observe ? "observe" :
								 "probe");
				return -ENODEV;
			}
			memset(b->va, poison, MZ0380_RAW_PROBE_BUF_SIZE);
			target = (u64)b->dma - mz0380_card_frame_offset;
			params[2 + 2 * i] = upper_32_bits(target);
			params[2 + 2 * i + 1] = lower_32_bits(target);
			pr_info("%s: raw-bank probe op 0x%02x bank%u buf[%u] target=0x%016llx -> slot {%08x, %08x}\n",
				dev->name, opcodes[bank], bank, i,
				(unsigned long long)target,
				params[2 + 2 * i], params[2 + 2 * i + 1]);
		}
		dma_wmb();
		ret = mz0380_send_command(dev, opcodes[bank], params,
					   ARRAY_SIZE(params), NULL, 2000);
		pr_info("%s: raw-bank probe registered bank%u with op 0x%02x, four independent 0x%x-byte buffers, poison=0x%02x ret=%d\n",
			dev->name, bank, opcodes[bank],
			MZ0380_RAW_PROBE_BUF_SIZE, poison, ret);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * Hand the card the buffer physical addresses (M17): one 12-word command,
 * params = { channel, stride, (phys_hi,phys_lo) x NR_BUFS }. The card copies
 * these into its channels[] array and programs the iATU outbound window from
 * them, so its encoder DMA lands in our buffers.
 */
int mz0380_stream_program_bufs(struct mz0380_dev *dev)
{
	u32 params[2 + 2 * MZ0380_STREAM_NR_BUFS];
	unsigned int i;
	int ret;

	if (mz0380_raw_bank_probe) {
		/* M209 is raw-only: op 0x04 must never be part of this test. */
		return mz0380_raw_probe_program_bufs(dev);
	}

	/*
	 * M211 registers what Windows registers: the raw banks through op02/op08
	 * AND the encoded window through op04. The raw banks take the op02 slots,
	 * so the legacy stream-buffer registration below is skipped rather than
	 * layered underneath them.
	 */
	if (mz0380_raw_bank_observe || mz0380_raw_deliver ||
	    (mz0380_raw_capable && mz0380_h264_probe)) {
		ret = mz0380_raw_probe_program_bufs(dev);
		if (ret)
			return ret;
		return mz0380_h264_program_bufs(dev);
	}

	params[0] = MZ0380_STREAM_VIDEO_CHANNEL;
	/*
	 * M92: word[3] is a buffer SIZE IN BYTES, not a stride. The Windows
	 * driver builds every buffer-registration command (0x02, 0x03, 0x04,
	 * 0x05, 0x08) with the identical shape - word[2] = channel, word[3] =
	 * size, word[4..11] = four {hi,lo} address pairs, count = 12 - and the
	 * sizes it sends decode exactly:
	 *
	 *   0x466000 = 2048 x 1125 x 2   + 4096   (YUV422, stride 2048, vtotal)
	 *   0x34BD00 = 2048 x 1125 x 1.5 + 256    (YUV420, same geometry)
	 *   0x10F000 = 1024 x  540 x 2   + 4096   (preview, YUV422)
	 *   0x0CA900 = 1024 x  540 x 1.5 + 256    (preview, YUV420)
	 *
	 * i.e. frame bytes plus a small header, at a power-of-two stride. That
	 * also identifies the "third region" in the DriverEntry log line
	 * [MEMORY] [00466000] [0034BD00] [0034BD00], which the Windows
	 * collection had left unexplained.
	 *
	 * Our value is the size of the buffer we actually allocate, which is
	 * the same thing Windows sends, so the number was right even though
	 * M27 named it "stride". Nothing changes here but the meaning.
	 */
	params[1] = mz0380_set_buf_stride;	/* cmd[0x8] = size, see above */
	/*
	 * ep.ko op2 copies cmd[0xc+8i]->channels[ch]+0 (iATU UPPER target reg 0x58 =
	 * host addr HIGH) and cmd[0x10+8i]->+4 (iATU LOWER reg 0x54 = host addr LOW).
	 * Pair order is {high32, low32} (RE_FINDINGS.md M23; polarity proven on hw -
	 * the cmd[0xc] value surfaced in the fault's high dword 0xfff8_0000_00000000).
	 * cmd[8] stride is ignored (M27: sending 0 changed nothing); the ATU limit is
	 * a hardcoded 32 MB aperture. The card
	 * cycles bufindex 1..N over 8-byte slots, so load all NR_BUFS. CRITICAL: this
	 * op2 must reach channels[] AFTER SET_VIC spawns the encoder and BEFORE op6 -
	 * START latches channels[] into the iATU (vpl_dmac StartTail); an op2 sent
	 * before the spawn is clobbered by the encoder's channel init -> low latches
	 * as reset 0 -> DMA to host ~0 + IOMMU fault.
	 */
	/*
	 * M25 probe (mz0380_buf_pair_swap): default pair order is
	 * {word0=high32, word1=low32}; swapped it is {word0=low32, word1=high32}.
	 * word0 reaches ELBI 0x58, word1 reaches ELBI 0x54. See the param's
	 * comment in mz0380-core.c for how to read the resulting fault address.
	 */
	/*
	 * M28: the card writes at (advertised target) + card_frame_offset, and
	 * the low half adds WITHOUT carrying into the high half. So advertise
	 * the buffer address minus that offset - with the M26/M28 mapping the
	 * buffers already sit card_frame_offset above a 4GiB boundary, which
	 * makes the advertised low word exactly 0 and the wrap unreachable.
	 */
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		u64 target = (u64)dev->stream_bufs[i].dma -
			     mz0380_card_frame_offset;
		u32 hi = upper_32_bits(target);
		u32 lo = lower_32_bits(target);

		params[2 + 2 * i]     = mz0380_buf_pair_swap ? lo : hi; /* -> ELBI 0x58 */
		params[2 + 2 * i + 1] = mz0380_buf_pair_swap ? hi : lo; /* -> ELBI 0x54 */

		pr_info("%s: SET_BUF[%u] buf=%pad target=0x%016llx -> slot {%08x, %08x}%s\n",
			dev->name, i, &dev->stream_bufs[i].dma, target,
			params[2 + 2 * i], params[2 + 2 * i + 1],
			mz0380_buf_pair_swap ? " (pair swapped)" : "");

		if (lo > U32_MAX - mz0380_card_frame_offset)
			pr_warn("%s: SET_BUF[%u] low word 0x%08x + offset 0x%x wraps 32 bits - the card will DMA to the wrong place\n",
				dev->name, i, lo, mz0380_card_frame_offset);
	}

	ret = mz0380_send_command(dev, mz0380_set_buf_opcode, params,
				  ARRAY_SIZE(params), NULL, 2000);
	if (ret)
		return ret;

	/*
	 * M92: Windows sends 0x08 immediately after 0x02, with the same channel,
	 * the same size word and four more address pairs - the pair is issued
	 * back to back at 0x14027b62d / 0x14027b752 and never one without the
	 * other. ep.ko treats them asymmetrically: op2 fills window0 slots 1..4
	 * and CLEARS host_ready (G[0]), while op8 fills slots 5..8 and SETS
	 * wency_ready = 8. We have only ever sent the one that clears a ready
	 * flag, and never the one that sets the other.
	 *
	 * Same four buffers: the point is the slots and the wency_ready side
	 * effect, not extra memory. Off by default - the M88 baseline renders
	 * the splash without it, and after M84 a change that has not been
	 * measured does not get to be a default.
	 */
	if (mz0380_set_buf_op8) {
		int r8 = mz0380_send_command(dev, MZ0380_CMD_SET_BUF_8, params,
					     ARRAY_SIZE(params), NULL, 2000);

		pr_info("%s: SET_BUF_8(op 0x08, window0 slots 5..8, sets wency_ready) ret=%d\n",
			dev->name, r8);
	}

	if (mz0380_h264_probe) {
		ret = mz0380_h264_program_bufs(dev);
		if (ret)
			return ret;
		/* Never let the legacy alias probe overwrite our owned window 1. */
		return 0;
	}

	if (!mz0380_probe_windows)
		return ret;

	/*
	 * M32 probe. op2 fills window0 only. The card DMAs a raw ~3.1 MiB frame
	 * into window0 buf0 and then never signals channel_done - and ep.ko's
	 * event path is demonstrably armed (our command-done MSIs work and use
	 * the same one-shot token), so the card's userspace is simply not
	 * declaring a finished frame. The likely reason is that the ENCODER's
	 * bitstream destination is a different outbound window, one we have
	 * never programmed, so tinyvenc has nowhere to put its output.
	 *
	 * Point the other windows at the three buffers window0 is not using
	 * (the card only ever writes buf0), so no extra memory is needed. All
	 * four slots of a window get the same address: we only need to learn
	 * WHICH window comes alive, not where inside it the card writes.
	 */
	{
		static const struct {
			u8 opcode;
			u8 buf;
			const char *what;
		} windows[] = {
			{ MZ0380_CMD_SET_BUF_4, 1, "window1" },
			{ MZ0380_CMD_SET_BUF_5, 2, "window2" },
			{ MZ0380_CMD_SET_BUF_3, 3, "window3" },
		};
		unsigned int w;

		for (w = 0; w < ARRAY_SIZE(windows); w++) {
			u64 target = (u64)dev->stream_bufs[windows[w].buf].dma -
				     mz0380_card_frame_offset;
			int r;

			for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
				params[2 + 2 * i]     = upper_32_bits(target);
				params[2 + 2 * i + 1] = lower_32_bits(target);
			}

			r = mz0380_send_command(dev, windows[w].opcode, params,
						ARRAY_SIZE(params), NULL, 2000);
			pr_info("%s: probe %s (op 0x%02x) -> buf[%u] 0x%016llx ret=%d\n",
				dev->name, windows[w].what, windows[w].opcode,
				windows[w].buf, target, r);
		}
	}

	return 0;
}

/*
 * MSI ISR. The interrupt cause is the EVENT word (BAR0+0x30).  The central ACK
 * primitive snapshots command replies and frame payloads together under its
 * event lock before clearing this one-shot mailbox.
 *
 * Frame completion is deliberately not an else branch here.  EVENT may carry
 * command and frame bits together, and the command-poll path can consume the
 * same one-shot mailbox without entering this ISR.  mz0380_mb_ack_event()
 * invokes mz0380_handle_event_snapshot() before every ACK, which is the single
 * place that snapshots frame TOKEN/PAYLOAD and queues drain work.
 */
static irqreturn_t mz0380_isr(int irq, void *data)
{
	struct mz0380_dev *dev = data;
	u32 event;

	/*
	 * M84: a SHARED INTx handler is called for every other device on the
	 * line, including during our own teardown. If the BAR has already been
	 * unmapped the very first MMIO read is a NULL dereference - which is
	 * exactly how the first INTx run died (oops in mz_read, CR2 = 0x30 =
	 * MZ0380_MB_EVENT, "rmmod exited with irqs disabled"). The ordering bug
	 * that opened that window is fixed in mz0380_finidev(), but a shared
	 * handler must not depend on ordering alone.
	 */
	if (!dev || !dev->bmmio[MZ0380_MAP_BAR_MMIO])
		return IRQ_NONE;

	event = mz_mmio_read(dev, MZ0380_MB_EVENT);
	if (!event || event == 0xffffffff)
		return IRQ_NONE;

	atomic_inc(&dev->irq_count);

	mz0380_mb_ack_event(dev);
	return IRQ_HANDLED;
}

int mz0380_irq_request(struct mz0380_dev *dev)
{
	int nvec, ret;

	if (dev->irq_requested)
		return 0;

	/*
	 * M245: keep the kernel's own DMA mappings below 4 GiB.
	 *
	 * The M26 buffers are placed with iommu_map() straight into the
	 * device's DMA-API domain, at dma_iova_base + (i << 32) - IOVAs the
	 * domain's allocator does not know are taken, so a later DMA-API
	 * mapping for this device could be handed the same address. A 32-bit
	 * mask confines every allocator-chosen IOVA for this device to the low
	 * 4 GiB, and mz0380_stream_bufs_alloc_iova() refuses a base below
	 * 4 GiB, so the two ranges cannot meet. The mask only steers the
	 * allocator: the card still reaches the high windows, because the
	 * IOMMU translates whatever is mapped.
	 *
	 * The full fix is a private IOMMU domain owned by this driver, which
	 * needs the card alone in its IOMMU group - not true on every board -
	 * so it stays a follow-up.
	 */
	ret = dma_set_mask_and_coherent(&dev->pci->dev,
					mz0380_dma_iova_remap ?
					DMA_BIT_MASK(32) : DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&dev->pci->dev,
						DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&dev->pci->dev, "no usable DMA mask\n");
		return ret;
	}

	/*
	 * M82. The endpoint ADVERTISES MSI - Windows derives
	 * DEVPKEY_PciDevice_InterruptSupport = 3 (line + message) and
	 * InterruptMessageMaximum = 1 straight from its config space - but the
	 * retail driver never uses it. On the Windows box the card's
	 * Interrupt Management\MessageSignaledInterruptProperties key is absent
	 * (14 other PCI devices on the same board have it), the INF carries no
	 * MSISupported directive, AllocConfig assigns a level-sensitive shared
	 * line with CM_RESOURCE_INTERRUPT_MESSAGE clear, and the trace logs
	 * "INTERRUPT = 00000000" on IRQ 29.
	 *
	 * So PCI_IRQ_MSI | PCI_IRQ_INTX picks the one interrupt path the
	 * shipping stack has never exercised. If the card's MSI path is unwired
	 * in firmware the symptom is precisely ours: commands complete, the
	 * encoder starts, no completion ever arrives. Default to what Windows
	 * does and treat MSI as the experiment.
	 */
	nvec = pci_alloc_irq_vectors(dev->pci, 1, 1,
				     mz0380_irq_intx ? PCI_IRQ_INTX
						     : (PCI_IRQ_MSI |
							PCI_IRQ_INTX));
	if (nvec < 1) {
		pr_err("%s: pci_alloc_irq_vectors failed (%d)\n",
		       dev->name, nvec);
		return nvec;
	}

	dev->msi_enabled = (nvec >= 1) && dev->pci->msi_enabled;
	/* Legacy delivery needs the line unmasked; probe masks it by default. */
	if (!dev->msi_enabled)
		pci_intx(dev->pci, 1);
	pr_info("%s: interrupt: %s, irq %u (Windows uses INTx)\n",
		dev->name, dev->msi_enabled ? "MSI" : "legacy INTx",
		pci_irq_vector(dev->pci, 0));
	dev->irq = pci_irq_vector(dev->pci, 0);

	dev->frame_event_head = 0;
	dev->frame_event_tail = 0;
	dev->frame_event_drain_scheduled = false;
	dev->frame_events_accepting = false;
	dev->frame_event_ack_deferred = false;
	dev->frame_event_drop_tokens = 0;
	dev->frame_event_drops = 0;
	/* Deferred first start follows placeholder buffers in the same VB2 run. */
	if (!READ_ONCE(dev->no_signal_active))
		dev->video_sequence = 0;
	dev->frame_poison_active = false;
	INIT_WORK(&dev->drain_work, mz0380_drain_work_fn);
	INIT_WORK(&dev->audio_work, mz0380_audio_work_fn);
	/*
	 * M245: the drain copies up to 3 MB per frame. On the shared system
	 * workqueue that competes with - and delays - unrelated work, and
	 * inherits its latency. A dedicated high-priority queue gives the frame
	 * copy its own worker; max_active 1 keeps drains strictly serial, which
	 * the single-work-item design already assumed.
	 */
	dev->drain_wq = alloc_workqueue("mz0380-drain/%u",
					WQ_HIGHPRI | WQ_UNBOUND, 1, dev->nr);
	if (!dev->drain_wq) {
		pci_free_irq_vectors(dev->pci);
		return -ENOMEM;
	}

	ret = request_irq(dev->irq, mz0380_isr,
			  dev->msi_enabled ? 0 : IRQF_SHARED,
			  dev->name, dev);
	if (ret) {
		pr_err("%s: request_irq(%d) failed (%d)\n",
		       dev->name, dev->irq, ret);
		destroy_workqueue(dev->drain_wq);
		dev->drain_wq = NULL;
		pci_free_irq_vectors(dev->pci);
		return ret;
	}

	dev->irq_requested = true;
	pr_info("%s: IRQ %d ready (%s)\n",
		dev->name, dev->irq,
		dev->msi_enabled ? "MSI" : "INTx");
	return 0;
}

void mz0380_irq_release(struct mz0380_dev *dev)
{
	if (!dev->irq_requested)
		return;

	mz0380_dma_flush_events(dev);
	free_irq(dev->irq, dev);
	pci_free_irq_vectors(dev->pci);
	dev->irq_requested = false;
	dev->msi_enabled = false;
	if (dev->drain_wq) {
		/* flush_events cancelled the work; nothing can requeue it now. */
		destroy_workqueue(dev->drain_wq);
		dev->drain_wq = NULL;
	}
}

int mz0380_dma_setup(struct mz0380_dev *dev)
{
	int ret;

	if (mz0380_raw_bank_probe &&
	    (mz0380_h264_probe || !mz0380_dma_iova_remap)) {
		pr_err("%s: raw_bank_probe is raw-only and requires h264_probe=0 plus dma_iova_remap=1\n",
		       dev->name);
		return -EINVAL;
	}

	/*
	 * M210 only means anything on top of the M209 sink topology. Setting it
	 * alone would silently change nothing, and a bounded hardware run must
	 * never be scored against a configuration that was not the one intended.
	 */
	if (mz0380_raw_probe_enc_tail && !mz0380_raw_bank_probe) {
		pr_err("%s: raw_probe_enc_tail (M210) requires raw_bank_probe=1; the H.264 path already sends the encoder tail\n",
		       dev->name);
		return -EINVAL;
	}

	/*
	 * M211 rides the working encoded path instead of replacing it, so it
	 * needs the opposite preconditions to M209: op04 present, raw-only
	 * absent.
	 */
	if ((mz0380_raw_bank_observe || mz0380_raw_deliver) &&
	    (mz0380_raw_bank_probe || !mz0380_h264_probe ||
	     !mz0380_dma_iova_remap)) {
		pr_err("%s: raw_bank_observe (M211) rides a live H.264 capture and requires h264_probe=1 plus dma_iova_remap=1, with raw_bank_probe=0\n",
		       dev->name);
		return -EINVAL;
	}

	ret = mz0380_stream_bufs_alloc(dev);
	if (ret) {
		pr_err("%s: stream buffer alloc failed (%d)\n", dev->name, ret);
		return ret;
	}
	if (mz0380_h264_probe) {
		ret = mz0380_h264_bufs_alloc_iova(dev);
		if (ret) {
			pr_err("%s: dedicated H.264 buffer alloc failed (%d)\n",
			       dev->name, ret);
			mz0380_stream_bufs_free(dev);
			return ret;
		}
		pr_info("%s: H.264 diagnostic enabled: %u window1 buffers x %u KiB\n",
			dev->name, MZ0380_STREAM_NR_BUFS,
			MZ0380_H264_BUF_SIZE >> 10);
		dev->h264_parameter_sets =
			kzalloc(MZ0380_H264_PARAMETER_SETS_MAX, GFP_KERNEL);
		if (!dev->h264_parameter_sets) {
			pr_err("%s: H.264 parameter-set cache alloc failed\n",
			       dev->name);
			mz0380_h264_bufs_free(dev);
			mz0380_stream_bufs_free(dev);
			return -ENOMEM;
		}
	}
	if (mz0380_raw_bank_probe || mz0380_raw_bank_observe ||
	    mz0380_raw_deliver ||
	    (mz0380_raw_capable && mz0380_h264_probe)) {
		bool raw_requested = mz0380_raw_bank_probe ||
				     mz0380_raw_bank_observe ||
				     mz0380_raw_deliver;

		ret = mz0380_raw_probe_bufs_alloc_iova(dev);
		if (ret && !raw_requested) {
			/*
			 * M218: the banks were wanted only so that raw COULD be
			 * selected later. Failing the whole probe over that
			 * would turn "this machine cannot spare 8 x 0x466000"
			 * into "this card does not work", which is a far worse
			 * outcome than losing an optional format. Carry on with
			 * H.264 and simply do not advertise I420.
			 */
			pr_warn("%s: raw banks unavailable (%d); I420 will not be offered. H.264 capture is unaffected - set raw_capable=0 to skip this attempt\n",
				dev->name, ret);
			ret = 0;
		} else if (ret) {
			pr_err("%s: Windows-parity raw-bank alloc failed (%d)\n",
			       dev->name, ret);
			kfree(dev->h264_parameter_sets);
			dev->h264_parameter_sets = NULL;
			mz0380_h264_bufs_free(dev);
			mz0380_stream_bufs_free(dev);
			return ret;
		} else {
		/*
		 * M218: with the banks in place the node can offer raw as a
		 * format an application selects, rather than one the operator
		 * has to reload the module to reach.
		 */
			dev->raw_capable = true;
			dev->deliver_raw = mz0380_raw_deliver;
			pr_info("%s: raw banks ready: 2 x 4 x 0x%x bytes (op 0x02 + op 0x08); I420 is selectable, startup format is %s\n",
				dev->name, MZ0380_RAW_PROBE_BUF_SIZE,
				dev->deliver_raw ? "I420 raw" : "H.264");
		}
	}

	/*
	 * M246: the audio window, when asked for. Optional in the same way the
	 * raw banks are: failing to get it costs audio, never the video node.
	 */
	if (mz0380_enable_audio && mz0380_h264_probe) {
		ret = mz0380_audio_bufs_alloc_iova(dev);
		if (ret)
			pr_warn("%s: audio window unavailable (%d); ALSA capture will not be offered\n",
				dev->name, ret);
		else
			pr_info("%s: audio window ready: %u x 0x%x slots from IOVA 0x%llx\n",
				dev->name, MZ0380_AUDIO_NR_SLOTS,
				MZ0380_AUDIO_SLOT_SIZE,
				(unsigned long long)dev->audio_bufs[0].dma);
		ret = 0;
	}

	pr_info("%s: %u stream buffers x %u KiB (buf0 @ %pad)\n",
		dev->name, MZ0380_STREAM_NR_BUFS,
		MZ0380_STREAM_BUF_SIZE >> 10, &dev->stream_bufs[0].dma);

	pci_set_master(dev->pci);
	dev->dma_armed = true;
	return 0;
}

/*
 * M30. With zero IOMMU faults we can no longer see the card's writes: an IOMMU
 * only reports the ones that MISS. So look in the buffers themselves. Reports,
 * per buffer, the first 16 bytes and how far into it anything non-zero reaches
 * - which separates "the card is writing and only the completion signal is
 * missing" from "the card stopped writing".
 */
/*
 * M40. Acknowledge consumed bitstreams: write 0 to the card's per-stream
 * enc_stat bytes (BAR0 + 0x50 + idx). See MZ0380_MB_ENC_STATUS in
 * mz0380-reg.h for the protocol - without this ack the card's encoder
 * produces exactly one bitstream and then skips every subsequent frame.
 * The whole word is cleared: we drive a single channel with one bitstream,
 * so idx 0 is ours and the rest are unused.
 */
void mz0380_enc_stat_ack(struct mz0380_dev *dev)
{
	/*
	 * M151: the knob covers the per-frame ack only. The single clear at
	 * stream start (mz0380_dma_start_stream) is deliberately left in - it
	 * establishes a known value to read back, and every result in the file
	 * was taken with it.
	 */
	if (!mz0380_enc_stat_ack_on)
		return;

	mz_mmio_write(dev, MZ0380_MB_ENC_STATUS, MZ0380_MB_ENC_STAT_FREE);
	wmb();
}

/*
 * M246: audio. The snapshot is taken under the same pre-ACK rule as a video
 * frame - once EVENT is cleared the card may overwrite 0x4c - and the slot is
 * consumed in process context on the drain workqueue.
 */
static void mz0380_audio_event_snapshot(struct mz0380_dev *dev, u32 event)
{
	u32 token = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3);
	unsigned long flags;
	u16 next;

	dma_rmb();
	spin_lock_irqsave(&dev->frame_event_lock, flags);
	if (!dev->audio_events)
		dev->audio_first_ns = ktime_get_ns();
	dev->audio_last_ns = ktime_get_ns();
	dev->audio_events++;
	next = (dev->audio_fifo_head + 1) % MZ0380_AUDIO_EVENT_FIFO_SIZE;
	if (next == dev->audio_fifo_tail) {
		dev->audio_fifo_drops++;
	} else {
		dev->audio_fifo[dev->audio_fifo_head].event = event;
		dev->audio_fifo[dev->audio_fifo_head].token = token;
		dev->audio_fifo_head = next;
	}
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);
	queue_work(dev->drain_wq, &dev->audio_work);
}

/*
 * Measure how far into the slot the card wrote - from the end, against the
 * poison, like the video extent - hand at most one slot of it to ALSA, and
 * re-poison what was written. The measurement is the point on first contact:
 * nothing but the op 0x03 size word says a completion carries 4096 bytes.
 */
static void mz0380_audio_consume_slot(struct mz0380_dev *dev, u32 slot)
{
	struct mz0380_audio_buf *b = &dev->audio_bufs[slot];
	const u32 *w = b->va;
	const u32 poison = 0x01010101u * MZ0380_AUDIO_POISON_BYTE;
	size_t i, extent = 0, len;

	dev->audio_slots++;
	dev->audio_slots_seen |= BIT(slot);
	dev->audio_last_slot = slot;

	dma_rmb();
	for (i = MZ0380_AUDIO_SLOT_MAP / sizeof(*w); i; i--) {
		if (READ_ONCE(w[i - 1]) != poison) {
			extent = i * sizeof(*w);
			break;
		}
	}
	if (!extent) {
		dev->audio_empty++;
		return;
	}
	if (extent > dev->audio_max_extent)
		dev->audio_max_extent = extent;
	if (extent > MZ0380_AUDIO_SLOT_SIZE)
		dev->audio_overrun++;

	len = min_t(size_t, extent, MZ0380_AUDIO_SLOT_SIZE);
	mz0380_audio_push(dev, b->va, len);
	dev->audio_bytes += len;

	memset(b->va, MZ0380_AUDIO_POISON_BYTE, extent);
	dma_wmb();
}

void mz0380_audio_work_fn(struct work_struct *work)
{
	struct mz0380_dev *dev = container_of(work, struct mz0380_dev,
					      audio_work);

	for (;;) {
		unsigned long flags;
		u32 event, token, mask, slot, start, k;

		spin_lock_irqsave(&dev->frame_event_lock, flags);
		if (dev->audio_fifo_tail == dev->audio_fifo_head) {
			spin_unlock_irqrestore(&dev->frame_event_lock, flags);
			return;
		}
		event = dev->audio_fifo[dev->audio_fifo_tail].event;
		token = dev->audio_fifo[dev->audio_fifo_tail].token;
		dev->audio_fifo_tail = (dev->audio_fifo_tail + 1) %
				       MZ0380_AUDIO_EVENT_FIFO_SIZE;
		spin_unlock_irqrestore(&dev->frame_event_lock, flags);

		dev->audio_last_event = event;
		dev->audio_last_token = token;

		/*
		 * With aic_int_mode=1 each completed slot has its own bit,
		 * BIT(16 + slot), and bits accumulated while the card had no
		 * credit arrive together. Otherwise the one bit is BIT(16 + ch)
		 * and the slot is the token's channel-0 nibble.
		 */
		mask = mz0380_aic_int_mode == 1 ?
		       (event >> 16) & (BIT(MZ0380_AUDIO_NR_SLOTS) - 1) : 0;
		if (!mask) {
			slot = token & 0xf;
			if (slot >= MZ0380_AUDIO_NR_SLOTS) {
				dev->audio_bad_token++;
				continue;
			}
			mask = BIT(slot);
		}

		/*
		 * Oldest first: the card fills the slots in rotation. The
		 * starting point is taken once - consume_slot() moves
		 * audio_last_slot, and reading it inside the loop walked
		 * round to the slot just consumed and consumed it again
		 * (the first hardware run: 1168 slots for 584 events, every
		 * second one empty).
		 */
		start = dev->audio_last_slot;
		for (k = 1; k <= MZ0380_AUDIO_NR_SLOTS; k++) {
			slot = (start + k) % MZ0380_AUDIO_NR_SLOTS;
			if (mask & BIT(slot))
				mz0380_audio_consume_slot(dev, slot);
		}
	}
}

/*
 * Snapshot the frame mailbox while it still belongs to @event.  The endpoint
 * event channel is a one-shot ping-pong: ACK re-arms it and TOKEN/PAYLOAD may
 * then be overwritten immediately.  This hook is called centrally by
 * mz0380_mb_ack_event(), not just by the ISR, because command polling can win
 * the race and ACK a combined CMD_DONE|frame event itself.
 *
 * The helper is IRQ-safe and does not sleep.  Before IRQ/work setup and outside
 * a real stream frame_events_accepting is false, so early firmware ACKs are a
 * cheap no-op here.
 */
void mz0380_handle_event_snapshot(struct mz0380_dev *dev, u32 event)
{
	struct mz0380_frame_event snapshot;
	unsigned long flags;
	u16 next;
	bool dropped = false;
	bool ack_now = false;

	/*
	 * M246: audio completions carry their own EVENT bits (16..23) and slot
	 * token (0x4c). Before this nothing looked at either, so every audio
	 * event the card raised was ACKed and discarded.
	 */
	if ((event & MZ0380_AUDIO_EVENT_MASK) && dev->audio_capable &&
	    READ_ONCE(dev->frame_events_accepting))
		mz0380_audio_event_snapshot(dev, event);

	if (!(event & MZ0380_VIDEO_EVENT_BIT) ||
	    !READ_ONCE(dev->frame_events_accepting))
		return;

	snapshot.timestamp_ns = ktime_get_ns();
	snapshot.event = event;
	snapshot.token = mz_mmio_read(dev, MZ0380_MB_FRAME_TOKEN);
	snapshot.payload[0] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1);
	snapshot.payload[1] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD2);
	snapshot.payload[2] = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD3);
	snapshot.enc_status = mz_mmio_read(dev, MZ0380_MB_ENC_STATUS);
	/* Pair the endpoint's completion notification with its coherent DMA. */
	dma_rmb();

	atomic_inc(&dev->irq_video_count);

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	if (!dev->frame_events_accepting) {
		dev->frame_event_drops++;
		dropped = true;
	} else {
		next = (dev->frame_event_head + 1) %
		       MZ0380_FRAME_EVENT_FIFO_SIZE;
		if (next == dev->frame_event_tail) {
			dev->frame_event_drops++;
			dropped = true;
		} else {
			dev->frame_events[dev->frame_event_head] = snapshot;
			dev->frame_event_head = next;
			if (!dev->frame_event_drain_scheduled) {
				dev->frame_event_drain_scheduled = true;
				/*
				 * Publish and queue atomically with respect to streamoff:
				 * cancel_work_sync() must not miss a not-yet-queued kick.
				 */
				queue_work(dev->drain_wq, &dev->drain_work);
			}
		}
	}
	if (dropped) {
		/*
		 * Do not free the encoder while an older snapshot is queued or
		 * being copied: it could immediately reuse and overwrite that
		 * token.  The drain batch (or streamoff flush) ACKs after all
		 * older buffers have been consumed/re-poisoned.
		 */
		ack_now = !dev->frame_event_drain_scheduled &&
			  dev->frame_event_head == dev->frame_event_tail;
		if (!ack_now) {
			dev->frame_event_ack_deferred = true;
			if ((snapshot.token & 7) <
			    (mz0380_raw_bank_probe ? MZ0380_RAW_PROBE_NR_BUFS :
			     MZ0380_STREAM_NR_BUFS))
				dev->frame_event_drop_tokens |=
					BIT(snapshot.token & 7);
		}
	}
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);

	if (dropped) {
		if (ack_now)
			mz0380_enc_stat_ack(dev);
		pr_warn_ratelimited("%s: frame event dropped before drain (event=%08x token=%08x payload=%08x/%08x/%08x enc=%08x); enc_stat ACK %s\n",
				    dev->name, snapshot.event, snapshot.token,
				    snapshot.payload[0], snapshot.payload[1],
				    snapshot.payload[2], snapshot.enc_status,
				    ack_now ? "sent" : "deferred until older snapshots drain");
		return;
	}

}

void mz0380_frame_events_start(struct mz0380_dev *dev)
{
	unsigned long flags;
	unsigned int i;

	if (!dev->irq_requested)
		return;

	/* Flush also resolves an ACK deferred by a full FIFO. */
	mz0380_dma_flush_events(dev);

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	dev->frame_event_head = 0;
	dev->frame_event_tail = 0;
	dev->frame_event_drain_scheduled = false;
	dev->frame_event_ack_deferred = false;
	dev->frame_event_drop_tokens = 0;
	dev->frame_event_drops = 0;
	dev->video_sequence = 0;
	dev->h264_last_token_valid = false;
	dev->h264_frames_delivered = 0;
	dev->h264_frames_dropped = 0;
	dev->h264_frames_discarded = 0;
	dev->h264_frames_suppressed = 0;
	dev->h264_parameter_sets_len = 0;
	dev->raw_probe_events = 0;
	dev->raw_probe_full_frames = 0;
	dev->raw_probe_bad_extents = 0;
	dev->raw_probe_consecutive_full = 0;
	dev->raw_probe_slots_seen = 0;
	dev->raw_probe_success_reported = false;
	/*
	 * M229: per-session, so a count from a previous stream cannot be read
	 * as evidence about this one.
	 */
	dev->raw_incomplete_tail = 0;
	dev->raw_incomplete_chroma = 0;
	/* M246: the audio counters are per session too. */
	dev->audio_fifo_head = 0;
	dev->audio_fifo_tail = 0;
	dev->audio_last_slot = MZ0380_AUDIO_NR_SLOTS - 1;
	dev->audio_events = 0;
	dev->audio_slots = 0;
	dev->audio_bytes = 0;
	dev->audio_empty = 0;
	dev->audio_overrun = 0;
	dev->audio_bad_token = 0;
	dev->audio_fifo_drops = 0;
	dev->audio_max_extent = 0;
	dev->audio_slots_seen = 0;
	dev->audio_first_ns = 0;
	dev->audio_last_ns = 0;
	dev->audio_alsa_bytes = 0;
	dev->audio_alsa_periods = 0;
	dev->audio_alsa_bails = 0;
	dev->audio_alsa_bail_why = 0;
	for (i = 0; i < MZ0380_RAW_PROBE_NR_BUFS; i++) {
		dev->raw_probe_bufs[i].last_extent = 0;
		dev->raw_probe_bufs[i].completions = 0;
		dev->raw_probe_bufs[i].delivered = 0;
	}
	dev->frame_events_accepting = true;
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);
}

void mz0380_dma_flush_events(struct mz0380_dev *dev)
{
	unsigned long flags;
	unsigned int pending = 0;
	unsigned int i;
	bool ack_deferred;
	u8 drop_tokens;

	if (!dev->irq_requested)
		return;

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	dev->frame_events_accepting = false;
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);

	/* Process-context API: synchronize before vb2 buffers or DMA memory go away. */
	cancel_work_sync(&dev->drain_work);
	cancel_work_sync(&dev->audio_work);

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	while (dev->frame_event_tail != dev->frame_event_head) {
		u32 token = dev->frame_events[dev->frame_event_tail].token & 7;

		if (token < (mz0380_raw_bank_probe ?
			     MZ0380_RAW_PROBE_NR_BUFS :
			     MZ0380_STREAM_NR_BUFS))
			dev->frame_event_drop_tokens |= BIT(token);
		dev->frame_event_tail = (dev->frame_event_tail + 1) %
					MZ0380_FRAME_EVENT_FIFO_SIZE;
		pending++;
	}
	dev->frame_event_head = 0;
	dev->frame_event_tail = 0;
	dev->frame_event_drain_scheduled = false;
	ack_deferred = dev->frame_event_ack_deferred;
	dev->frame_event_ack_deferred = false;
	drop_tokens = dev->frame_event_drop_tokens;
	dev->frame_event_drop_tokens = 0;
	dev->frame_event_drops += pending;
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++)
		if (drop_tokens & BIT(i))
			mz0380_frame_buffer_repoison(dev, i);

	if (pending || ack_deferred || drop_tokens)
		mz0380_enc_stat_ack(dev);
}
