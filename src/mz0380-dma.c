// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Driver for MZ0380 based capture cards.
 *
 *  DMA windows, completion interrupt and event FIFO (RE_FINDINGS.md M17).
 *
 *  The card DMAs into host buffers through PCIe iATU outbound windows that it
 *  programs from addresses handed over with the SET_BUF opcodes:
 *
 *    window 0 (op 0x02 + op 0x08)  raw I420 frames, 2 banks x 4 slots (M211)
 *    window 1 (op 0x04)            H.264 access units, 4 slots      (M177)
 *    window 3 (op 0x03)            PCM audio, 4 slots               (M246)
 *
 *  The card only latches the HIGH 32 bits of a target (M25), so every slot is
 *  mapped at its own 4 GiB-aligned IOVA with an explicit iommu_map() (M26).
 *  A finished frame raises EVENT (BAR0+0x30) bit 0; the payload words at
 *  0x40..0x4c say which slot, and are only valid until the EVENT is ACKed.
 */

#include "mz0380-dma-internal.h"

/* Header plus payload prefix: all the H.264 drain inspects of an empty slot. */
#define MZ0380_H264_POISON_SPAN		(2 * PAGE_SIZE)

/* --- IOVA placement ------------------------------------------------------- */

static dma_addr_t mz0380_iova(u64 index)
{
	return mz0380_dma_iova_base + (index << 32);
}

/*
 * The card needs a translating IOMMU domain: with iommu=off or iommu=pt there
 * is no way to place a buffer where a high-32-bits-only target can reach it.
 */
static struct iommu_domain *mz0380_iova_domain(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);

	if (mz0380_dma_iova_base & 0xffffffffULL) {
		dev_err(&dev->pci->dev,
			"dma_iova_base 0x%llx is not 4 GiB-aligned - the card would DMA to (base >> 32) << 32, missing the buffers\n",
			mz0380_dma_iova_base);
		return NULL;
	}
	/*
	 * M245: and above the first 4 GiB, which is where the DMA-API allocator
	 * is confined to (see the mask in mz0380_irq_request()).
	 */
	if (!upper_32_bits(mz0380_dma_iova_base)) {
		dev_err(&dev->pci->dev,
			"dma_iova_base 0x%llx is below 4 GiB - that range belongs to the kernel's DMA allocator for this device\n",
			mz0380_dma_iova_base);
		return NULL;
	}
	if (!domain) {
		dev_err(&dev->pci->dev,
			"no IOMMU domain for the device. This card only latches the high 32 bits of a DMA target, so it needs IOMMU translation: enable VT-d/AMD-Vi and boot without iommu=off or iommu=pt\n");
		return NULL;
	}
	if (domain->type != IOMMU_DOMAIN_DMA &&
	    domain->type != IOMMU_DOMAIN_DMA_FQ &&
	    domain->type != IOMMU_DOMAIN_UNMANAGED) {
		dev_err(&dev->pci->dev,
			"IOMMU domain type %u is pass-through/identity - no translation to place buffers with (boot without iommu=pt)\n",
			domain->type);
		return NULL;
	}
	return domain;
}

/*
 * A block the page allocator cannot supply in one piece: fragmented host
 * pages behind the same contiguous card-visible IOVA, mapped one page at a
 * time. b is published only after every page is mapped; on failure the pages
 * already mapped are unmapped before any backing memory is released. This
 * fallback is deliberately x86-only.
 *
 * From pull request #3 (elsell), where it served the 4 MiB stream buffers and
 * the H.264 slots; adapted here to mz0380_map_block().
 */
static int mz0380_map_block_fragmented(struct mz0380_dev *dev,
				       struct iommu_domain *domain,
				       struct mz0380_dma_buf *b,
				       dma_addr_t iova, size_t size)
{
	size_t off;
	void *va;
	int ret = -ENOMEM;

	if (!IS_ENABLED(CONFIG_X86_64))
		return -EOPNOTSUPP;
	va = vzalloc(size);
	if (!va)
		return -ENOMEM;
	for (off = 0; off < size; off += PAGE_SIZE) {
		if (mz0380_fault_inject == MZ0380_INJECT_FRAGMENTED_FAIL &&
		    off >= size / 2) {
			ret = -ENOMEM;
			goto fail;
		}
		if (iommu_iova_to_phys(domain, iova + off)) {
			ret = -EBUSY;
			goto fail;
		}
		ret = iommu_map(domain, iova + off,
				page_to_phys(vmalloc_to_page((char *)va + off)),
				PAGE_SIZE, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
		if (ret)
			goto fail;
	}
	b->va = va;
	b->dma = iova;
	pr_info("%s: fragmented buffer mapped at IOVA 0x%llx (%zu bytes)\n",
		dev->name, (unsigned long long)iova, size);
	return 0;
fail:
	dev_warn(&dev->pci->dev,
		 "fragmented mapping at IOVA 0x%llx failed %zu bytes in (%d); unmapping those and freeing the block\n",
		 (unsigned long long)iova, off, ret);
	if (off)
		iommu_unmap(domain, iova, off);
	vfree(va);
	return ret;
}

/* How many of a window's slots are mapped each way, for /proc/mz0380-state. */
void mz0380_dma_blocks_count(const struct mz0380_dma_buf *bufs, unsigned int n,
			     unsigned int *contiguous, unsigned int *fragmented)
{
	unsigned int i;

	*contiguous = 0;
	*fragmented = 0;
	for (i = 0; i < n; i++) {
		if (!bufs[i].va)
			continue;
		if (is_vmalloc_addr(bufs[i].va))
			(*fragmented)++;
		else
			(*contiguous)++;
	}
}

/*
 * One block at one IOVA (window 1 and 3 slots): physically contiguous when
 * the allocator can supply it, fragmented pages otherwise.
 */
static int mz0380_map_block(struct mz0380_dev *dev,
			    struct iommu_domain *domain,
			    struct mz0380_dma_buf *b, u64 index, size_t size)
{
	dma_addr_t iova = mz0380_iova(index);
	phys_addr_t phys;
	int ret;

	if (iommu_iova_to_phys(domain, iova)) {
		dev_err(&dev->pci->dev,
			"IOVA 0x%llx is already mapped - pick another dma_iova_base\n",
			(unsigned long long)iova);
		return -EBUSY;
	}
	if (mz0380_fault_inject != MZ0380_INJECT_FRAGMENTED &&
	    mz0380_fault_inject != MZ0380_INJECT_FRAGMENTED_FAIL)
		b->pages = alloc_pages(GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN |
				       __GFP_NORETRY, get_order(size));
	if (!b->pages)
		return mz0380_map_block_fragmented(dev, domain, b, iova, size);
	phys = page_to_phys(b->pages);
	ret = iommu_map(domain, iova, phys, size, IOMMU_READ | IOMMU_WRITE,
			GFP_KERNEL);
	if (ret) {
		dev_err(&dev->pci->dev, "iommu_map(0x%llx, %zu) failed (%d)\n",
			(unsigned long long)iova, size, ret);
		__free_pages(b->pages, get_order(size));
		b->pages = NULL;
		return ret;
	}
	b->va = page_address(b->pages);
	b->dma = iova;
	return 0;
}

static void mz0380_unmap_block(struct iommu_domain *domain,
			       struct mz0380_dma_buf *b, size_t size)
{
	if (!b->va)
		return;
	if (b->dma && domain)
		iommu_unmap(domain, b->dma, size);
	if (is_vmalloc_addr(b->va))
		vfree(b->va);
	else
		__free_pages(b->pages, get_order(size));
	b->pages = NULL;
	b->va = NULL;
	b->dma = 0;
	b->delivered = 0;
}

/* --- window 1: H.264 ------------------------------------------------------ */

static void mz0380_h264_bufs_free(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int i;

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++)
		mz0380_unmap_block(domain, &dev->h264_bufs[i],
				   MZ0380_H264_BUF_SIZE);
}

static int mz0380_h264_bufs_alloc(struct mz0380_dev *dev,
				  struct iommu_domain *domain)
{
	unsigned int i;
	int ret;

	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		ret = mz0380_map_block(dev, domain, &dev->h264_bufs[i],
				       MZ0380_H264_IOVA_INDEX + i,
				       MZ0380_H264_BUF_SIZE);
		if (ret) {
			mz0380_h264_bufs_free(dev);
			return ret;
		}
	}
	return 0;
}

/* --- window 3: audio (M246) ----------------------------------------------- */

static void mz0380_audio_bufs_free(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int i;

	for (i = 0; i < MZ0380_AUDIO_NR_SLOTS; i++)
		mz0380_unmap_block(domain, &dev->audio_bufs[i],
				   MZ0380_AUDIO_SLOT_MAP);
	dev->audio_capable = false;
}

/*
 * The audio window's four slots, each at its own 4 GiB-aligned IOVA after
 * every video window, with MZ0380_AUDIO_SLOT_MAP bytes mapped and poisoned so
 * the extent the card writes is measured rather than assumed.
 */
static int mz0380_audio_bufs_alloc(struct mz0380_dev *dev,
				   struct iommu_domain *domain)
{
	unsigned int i;
	int ret;

	for (i = 0; i < MZ0380_AUDIO_NR_SLOTS; i++) {
		ret = mz0380_map_block(dev, domain, &dev->audio_bufs[i],
				       MZ0380_AUDIO_IOVA_INDEX + i,
				       MZ0380_AUDIO_SLOT_MAP);
		if (ret) {
			mz0380_audio_bufs_free(dev);
			return ret;
		}
		memset(dev->audio_bufs[i].va, MZ0380_AUDIO_POISON_BYTE,
		       MZ0380_AUDIO_SLOT_MAP);
	}
	wmb();	/* poison visible before the card can be told the address */
	dev->audio_capable = true;
	return 0;
}

/*
 * op 0x03, exactly as Windows builds it - channel, the 4 KiB slot size, then
 * four {hi, lo} targets. Sent wherever the encoded window is (re)registered so
 * the two can never disagree about a spawn boundary.
 */
static int mz0380_audio_program_bufs(struct mz0380_dev *dev)
{
	u32 params[2 + 2 * MZ0380_AUDIO_NR_SLOTS] = { 0 };
	unsigned int i;
	int ret;

	params[0] = MZ0380_STREAM_VIDEO_CHANNEL;
	params[1] = MZ0380_AUDIO_SLOT_SIZE;
	for (i = 0; i < MZ0380_AUDIO_NR_SLOTS; i++) {
		struct mz0380_dma_buf *b = &dev->audio_bufs[i];

		if (!b->va || !b->dma)
			return -ENODEV;
		memset(b->va, MZ0380_AUDIO_POISON_BYTE, MZ0380_AUDIO_SLOT_MAP);
		params[2 + 2 * i] = upper_32_bits(b->dma);
		params[2 + 2 * i + 1] = lower_32_bits(b->dma);
	}
	wmb();	/* re-poisoned slots visible before op 0x03 hands them over */

	ret = mz0380_send_command(dev, MZ0380_CMD_SET_BUF_3, params,
				  ARRAY_SIZE(params), NULL, 2000);
	dev_dbg(&dev->pci->dev,
		"audio window registered (op 0x03, 4 x 0x%x at IOVA 0x%llx..) ret=%d\n",
		MZ0380_AUDIO_SLOT_SIZE,
		(unsigned long long)dev->audio_bufs[0].dma, ret);
	return ret;
}

/* Register window 1 with op 0x04, and the audio window behind it. */
int mz0380_h264_program_bufs(struct mz0380_dev *dev)
{
	u32 params[2 + 2 * MZ0380_STREAM_NR_BUFS] = { 0 };
	unsigned int i;
	int ret;

	params[0] = MZ0380_STREAM_VIDEO_CHANNEL;
	params[1] = MZ0380_H264_SET_BUF_SIZE;
	for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
		struct mz0380_dma_buf *b = &dev->h264_bufs[i];

		if (!b->va || !b->dma)
			return -ENODEV;
		/*
		 * The drain recognises a slot the card has not written yet by
		 * its header and payload prefix still reading as poison; those
		 * live in the first two pages.
		 */
		memset(b->va, MZ0380_H264_POISON_BYTE, MZ0380_H264_POISON_SPAN);
		b->delivered = 0;
		params[2 + 2 * i] = upper_32_bits(b->dma);
		params[2 + 2 * i + 1] = lower_32_bits(b->dma);
	}
	wmb();

	ret = mz0380_send_command(dev, MZ0380_CMD_SET_BUF_4, params,
				  ARRAY_SIZE(params), NULL, 2000);
	if (ret)
		return ret;
	/* M246: audio rides along; its failure must not cost the video. */
	if (dev->audio_capable) {
		int aret = mz0380_audio_program_bufs(dev);

		if (aret)
			dev_warn(&dev->pci->dev,
				 "audio window registration failed (%d); video is unaffected\n",
				 aret);
	}
	return 0;
}

/* --- window 0: raw frames ------------------------------------------------- */

static void mz0380_raw_bufs_free(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pci->dev);
	unsigned int i, p;

	for (i = 0; i < MZ0380_RAW_NR_BUFS; i++) {
		struct mz0380_raw_buf *b = &dev->raw_bufs[i];

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
 * The Windows raw-bank allocation is not a power of two. Mapping individual
 * pages avoids eight fragile order-11 allocations (and almost 29 MiB of rounded
 * tails) while still presenting each slot as one contiguous card-visible IOVA
 * and one contiguous kernel VA.
 */
static int mz0380_raw_buf_alloc(struct mz0380_dev *dev,
				struct iommu_domain *domain, u32 idx)
{
	struct mz0380_raw_buf *b = &dev->raw_bufs[idx];
	const u32 nr_pages = MZ0380_RAW_BUF_SIZE / PAGE_SIZE;
	dma_addr_t iova = mz0380_iova(MZ0380_RAW_IOVA_INDEX + idx);
	unsigned int p;
	int ret;

	BUILD_BUG_ON(MZ0380_RAW_BUF_SIZE % PAGE_SIZE);
	b->pages = kcalloc(nr_pages, sizeof(*b->pages), GFP_KERNEL);
	if (!b->pages)
		return -ENOMEM;
	b->nr_pages = nr_pages;
	b->dma = iova;

	for (p = 0; p < nr_pages; p++) {
		dma_addr_t at = iova + (u64)p * PAGE_SIZE;

		b->pages[p] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!b->pages[p])
			return -ENOMEM;
		if (iommu_iova_to_phys(domain, at)) {
			dev_err(&dev->pci->dev,
				"raw IOVA 0x%llx is already mapped - pick another dma_iova_base\n",
				(unsigned long long)at);
			return -EBUSY;
		}
		ret = iommu_map(domain, at, page_to_phys(b->pages[p]),
				PAGE_SIZE, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
		if (ret)
			return ret;
		b->mapped += PAGE_SIZE;
	}

	b->va = vmap(b->pages, nr_pages, VM_MAP, PAGE_KERNEL);
	return b->va ? 0 : -ENOMEM;
}

static int mz0380_raw_bufs_alloc(struct mz0380_dev *dev,
				 struct iommu_domain *domain)
{
	unsigned int i;
	int ret;

	for (i = 0; i < MZ0380_RAW_NR_BUFS; i++) {
		ret = mz0380_raw_buf_alloc(dev, domain, i);
		if (ret) {
			dev_err(&dev->pci->dev,
				"raw slot %u allocation failed (%d)\n", i, ret);
			mz0380_raw_bufs_free(dev);
			return ret;
		}
	}
	return 0;
}

/*
 * op 0x02 (bank 0) then op 0x08 (bank 1), what Windows registers for window 0
 * (M211). Only the sentinels have to be armed: the raw drain tells a written
 * slot from an untouched one by them alone (M217), and bank 1 is never written.
 */
static int mz0380_raw_program_bufs(struct mz0380_dev *dev)
{
	static const u8 opcodes[MZ0380_RAW_BANKS] = {
		MZ0380_CMD_SET_BUF_2,
		MZ0380_CMD_SET_BUF_8,
	};
	u32 params[2 + 2 * MZ0380_STREAM_NR_BUFS] = { 0 };
	unsigned int bank, i;
	int ret;

	params[0] = MZ0380_STREAM_VIDEO_CHANNEL;
	params[1] = MZ0380_RAW_BUF_SIZE;
	for (bank = 0; bank < MZ0380_RAW_BANKS; bank++) {
		for (i = 0; i < MZ0380_STREAM_NR_BUFS; i++) {
			u32 idx = bank * MZ0380_STREAM_NR_BUFS + i;
			struct mz0380_raw_buf *b = &dev->raw_bufs[idx];

			if (!b->va || !b->dma)
				return -ENODEV;
			mz0380_raw_sentinel_repoison(dev, idx);
			params[2 + 2 * i] = upper_32_bits(b->dma);
			params[2 + 2 * i + 1] = lower_32_bits(b->dma);
		}
		dma_wmb();
		ret = mz0380_send_command(dev, opcodes[bank], params,
					  ARRAY_SIZE(params), NULL, 2000);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Every window the card writes, in the Windows order: the raw banks, then the
 * encoded window (and audio). The raw banks are registered even for an H.264
 * session; that is the topology every H.264 capture since M211 ran on.
 */
int mz0380_dma_program_bufs(struct mz0380_dev *dev)
{
	int ret = mz0380_raw_program_bufs(dev);

	if (ret)
		return ret;
	return mz0380_h264_program_bufs(dev);
}

int mz0380_dma_setup(struct mz0380_dev *dev)
{
	struct iommu_domain *domain = mz0380_iova_domain(dev);
	int ret;

	if (!domain)
		return -ENODEV;

	ret = mz0380_h264_bufs_alloc(dev, domain);
	if (ret)
		return ret;

	dev->h264_parameter_sets = kzalloc(MZ0380_H264_PARAMETER_SETS_MAX,
					   GFP_KERNEL);
	if (!dev->h264_parameter_sets) {
		ret = -ENOMEM;
		goto err_h264;
	}

	ret = mz0380_raw_bufs_alloc(dev, domain);
	if (ret)
		goto err_ps;

	/* Optional like the ALSA device it feeds: failing costs audio only. */
	if (mz0380_enable_audio) {
		ret = mz0380_audio_bufs_alloc(dev, domain);
		if (ret)
			dev_warn(&dev->pci->dev,
				 "audio window unavailable (%d); ALSA capture will not be offered\n",
				 ret);
	}

	dev->dma_armed = true;
	return 0;

err_ps:
	kfree(dev->h264_parameter_sets);
	dev->h264_parameter_sets = NULL;
err_h264:
	mz0380_h264_bufs_free(dev);
	return ret;
}

void mz0380_dma_teardown(struct mz0380_dev *dev)
{
	dev->dma_armed = false;
	kfree(dev->h264_parameter_sets);
	dev->h264_parameter_sets = NULL;
	dev->h264_parameter_sets_len = 0;
	mz0380_raw_bufs_free(dev);
	mz0380_h264_bufs_free(dev);
	mz0380_audio_bufs_free(dev);
}

/* --- interrupt ------------------------------------------------------------ */

/*
 * The interrupt cause is the EVENT word (BAR0+0x30). The central ACK primitive
 * snapshots command replies and frame payloads together under its event lock
 * before clearing this one-shot mailbox, and the command poller can consume the
 * same event without entering this handler - so frame handling lives in
 * mz0380_handle_event_snapshot(), which every ACK path calls.
 */
static irqreturn_t mz0380_isr(int irq, void *data)
{
	struct mz0380_dev *dev = data;
	u32 event;

	/*
	 * M84: a shared INTx handler is called for every other device on the
	 * line, including during our own teardown, so it must not depend on
	 * remove-path ordering to keep the BAR mapped.
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
	 * M245: keep the kernel's own DMA mappings below 4 GiB. The windows are
	 * placed with iommu_map() straight into the device's DMA-API domain at
	 * IOVAs its allocator does not know are taken; a 32-bit mask confines
	 * every allocator-chosen IOVA to the low 4 GiB, and dma_iova_base is
	 * refused below 4 GiB, so the two ranges cannot meet.
	 */
	ret = dma_set_mask_and_coherent(&dev->pci->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&dev->pci->dev, "no usable DMA mask\n");
		return ret;
	}

	/*
	 * M82: the endpoint advertises MSI, but the retail Windows driver never
	 * uses it - it runs on a level-sensitive shared line. Do the same.
	 */
	nvec = pci_alloc_irq_vectors(dev->pci, 1, 1, PCI_IRQ_INTX);
	if (nvec < 1) {
		dev_err(&dev->pci->dev, "pci_alloc_irq_vectors failed (%d)\n",
			nvec);
		return nvec;
	}
	dev->irq = pci_irq_vector(dev->pci, 0);

	INIT_WORK(&dev->drain_work, mz0380_drain_work_fn);
	INIT_WORK(&dev->audio_work, mz0380_audio_work_fn);
	/*
	 * M245: the drain copies up to 3 MB per frame. A dedicated high-priority
	 * queue gives that copy its own worker instead of competing on the
	 * system workqueue; max_active 1 keeps drains strictly serial.
	 */
	dev->drain_wq = alloc_workqueue("mz0380-drain/%u",
					WQ_HIGHPRI | WQ_UNBOUND, 1, dev->nr);
	if (!dev->drain_wq) {
		pci_free_irq_vectors(dev->pci);
		return -ENOMEM;
	}

	ret = request_irq(dev->irq, mz0380_isr, IRQF_SHARED, dev->name, dev);
	if (ret) {
		dev_err(&dev->pci->dev, "request_irq(%d) failed (%d)\n",
			dev->irq, ret);
		destroy_workqueue(dev->drain_wq);
		dev->drain_wq = NULL;
		pci_free_irq_vectors(dev->pci);
		return ret;
	}

	/*
	 * Probe masked the line. Unmask it only now that a handler exists: a
	 * level-triggered CMD_DONE on a shared line with nobody to claim it gets
	 * the whole line disabled.
	 */
	pci_intx(dev->pci, 1);
	dev->irq_requested = true;
	return 0;
}

void mz0380_irq_release(struct mz0380_dev *dev)
{
	if (!dev->irq_requested)
		return;

	mz0380_dma_flush_events(dev);
	pci_intx(dev->pci, 0);
	free_irq(dev->irq, dev);
	pci_free_irq_vectors(dev->pci);
	dev->irq_requested = false;
	if (dev->drain_wq) {
		/* flush_events cancelled the work; nothing can requeue it now. */
		destroy_workqueue(dev->drain_wq);
		dev->drain_wq = NULL;
	}
}

/*
 * M40. Acknowledge consumed bitstreams: write 0 to the card's per-stream
 * enc_stat bytes (BAR0 + 0x50 + idx). See MZ0380_MB_ENC_STATUS in
 * mz0380-reg.h for the protocol - without this ack the card's encoder
 * produces exactly one bitstream and then skips every subsequent frame.
 */
void mz0380_enc_stat_ack(struct mz0380_dev *dev)
{
	mz_mmio_write(dev, MZ0380_MB_ENC_STATUS, MZ0380_MB_ENC_STAT_FREE);
	wmb();
}

/* --- audio events (M246) -------------------------------------------------- */

/*
 * The snapshot is taken under the same pre-ACK rule as a video frame - once
 * EVENT is cleared the card may overwrite 0x4c - and the slot is consumed in
 * process context on the drain workqueue.
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
 * poison - hand at most one slot of it to ALSA, and re-poison what was written.
 */
static void mz0380_audio_consume_slot(struct mz0380_dev *dev, u32 slot)
{
	struct mz0380_dma_buf *b = &dev->audio_bufs[slot];
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
		 * audio_last_slot, and reading it inside the loop walked round
		 * to the slot just consumed and consumed it again.
		 */
		start = dev->audio_last_slot;
		for (k = 1; k <= MZ0380_AUDIO_NR_SLOTS; k++) {
			slot = (start + k) % MZ0380_AUDIO_NR_SLOTS;
			if (mask & BIT(slot))
				mz0380_audio_consume_slot(dev, slot);
		}
	}
}

/* --- video events --------------------------------------------------------- */

/*
 * Snapshot the frame mailbox while it still belongs to @event. The endpoint
 * event channel is a one-shot ping-pong: ACK re-arms it and TOKEN/PAYLOAD may
 * then be overwritten immediately. This hook is called centrally by
 * mz0380_mb_ack_event(), not just by the ISR, because command polling can win
 * the race and ACK a combined CMD_DONE|frame event itself.
 *
 * IRQ-safe and non-sleeping. Outside a stream frame_events_accepting is false,
 * so early ACKs are a cheap no-op here.
 */
void mz0380_handle_event_snapshot(struct mz0380_dev *dev, u32 event)
{
	struct mz0380_frame_event snapshot;
	unsigned long flags;
	bool dropped = false;
	bool ack_now = false;
	u16 next;

	if ((event & MZ0380_AUDIO_EVENT_MASK) && dev->audio_capable &&
	    READ_ONCE(dev->frame_events_accepting))
		mz0380_audio_event_snapshot(dev, event);

	if (!(event & MZ0380_VIDEO_EVENT_BIT) ||
	    !READ_ONCE(dev->frame_events_accepting))
		return;

	snapshot.timestamp_ns = ktime_get_ns();
	snapshot.event = event;
	snapshot.token = mz_mmio_read(dev, MZ0380_MB_FRAME_TOKEN);
	snapshot.payload0 = mz_mmio_read(dev, MZ0380_MB_EVT_PAYLOAD1);
	/* Pair the endpoint's completion notification with its DMA. */
	dma_rmb();

	atomic_inc(&dev->irq_video_count);

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	next = (dev->frame_event_head + 1) % MZ0380_FRAME_EVENT_FIFO_SIZE;
	if (!dev->frame_events_accepting || next == dev->frame_event_tail) {
		dev->frame_event_drops++;
		dropped = true;
		/*
		 * Do not free the encoder while an older snapshot is queued or
		 * being copied: it could immediately reuse and overwrite that
		 * slot. The drain batch (or streamoff flush) ACKs after every
		 * older snapshot has been consumed.
		 */
		ack_now = !dev->frame_event_drain_scheduled &&
			  dev->frame_event_head == dev->frame_event_tail;
		if (!ack_now)
			dev->frame_event_ack_deferred = true;
	} else {
		dev->frame_events[dev->frame_event_head] = snapshot;
		dev->frame_event_head = next;
		mz0380_cadence_note(dev, dev->cadence_event,
				    &dev->cadence_event_prev_ns,
				    snapshot.timestamp_ns);
		if (!dev->frame_event_drain_scheduled) {
			dev->frame_event_drain_scheduled = true;
			/*
			 * Publish and queue atomically with respect to
			 * streamoff: cancel_work_sync() must not miss a
			 * not-yet-queued kick.
			 */
			queue_work(dev->drain_wq, &dev->drain_work);
		}
	}
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);

	if (!dropped)
		return;
	if (ack_now)
		mz0380_enc_stat_ack(dev);
	pr_warn_ratelimited("%s: frame event dropped before drain (event=%08x token=%08x payload=%08x); enc_stat ACK %s\n",
			    dev->name, snapshot.event, snapshot.token,
			    snapshot.payload0,
			    ack_now ? "sent" : "deferred until older snapshots drain");
}

void mz0380_frame_events_start(struct mz0380_dev *dev)
{
	unsigned long flags;

	if (!dev->irq_requested)
		return;

	/* Flush also resolves an ACK deferred by a full FIFO. */
	mz0380_dma_flush_events(dev);

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	dev->frame_event_head = 0;
	dev->frame_event_tail = 0;
	dev->frame_event_drain_scheduled = false;
	dev->frame_event_ack_deferred = false;
	dev->frame_event_drops = 0;
	dev->h264_last_token_valid = false;
	dev->h264_frames_delivered = 0;
	dev->h264_frames_dropped = 0;
	dev->h264_frames_discarded = 0;
	dev->h264_frames_suppressed = 0;
	dev->h264_parameter_sets_len = 0;
	/*
	 * M229: per-session, so a count from a previous stream cannot be read
	 * as evidence about this one.
	 */
	dev->raw_incomplete_tail = 0;
	dev->raw_incomplete_chroma = 0;
	dev->raw_chroma_neutral = 0;
	dev->cadence_event_prev_ns = 0;
	dev->cadence_take_prev_ns = 0;
	memset(dev->cadence_event, 0, sizeof(dev->cadence_event));
	memset(dev->cadence_take, 0, sizeof(dev->cadence_take));
	memset(dev->cadence_ready, 0, sizeof(dev->cadence_ready));
	memset(dev->cadence_batch, 0, sizeof(dev->cadence_batch));
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
	dev->frame_events_accepting = true;
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);
}

void mz0380_dma_flush_events(struct mz0380_dev *dev)
{
	unsigned long flags;
	unsigned int pending = 0;
	bool ack_deferred;

	if (!dev->irq_requested)
		return;

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	dev->frame_events_accepting = false;
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);

	/* Synchronize before vb2 buffers or DMA memory go away. */
	cancel_work_sync(&dev->drain_work);
	cancel_work_sync(&dev->audio_work);

	spin_lock_irqsave(&dev->frame_event_lock, flags);
	while (dev->frame_event_tail != dev->frame_event_head) {
		dev->frame_event_tail = (dev->frame_event_tail + 1) %
					MZ0380_FRAME_EVENT_FIFO_SIZE;
		pending++;
	}
	dev->frame_event_head = 0;
	dev->frame_event_tail = 0;
	dev->frame_event_drain_scheduled = false;
	ack_deferred = dev->frame_event_ack_deferred;
	dev->frame_event_ack_deferred = false;
	dev->frame_event_drops += pending;
	spin_unlock_irqrestore(&dev->frame_event_lock, flags);

	if (pending || ack_deferred)
		mz0380_enc_stat_ack(dev);
}
