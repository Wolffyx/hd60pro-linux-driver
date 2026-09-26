// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 PCI probe/remove lifecycle and device resource ownership.
 */

#include "mz0380.h"

bool allow_bus_master;
module_param(allow_bus_master, bool, 0444);
MODULE_PARM_DESC(allow_bus_master,
		 "allow PCI bus mastering during probe; unsafe until DMA is understood");

static unsigned int card[] = { [0 ... (MZ0380_MAXBOARDS - 1)] = UNSET };
module_param_array(card, uint, NULL, 0444);
MODULE_PARM_DESC(card, "card type");

/*
 * M245: card numbers are allocated, not counted. A plain counter was never
 * decremented on remove, so every unbind/bind cycle advanced it and the ninth
 * probe read card[] past its end.
 */
static DEFINE_IDA(mz0380_ida);
DEFINE_MUTEX(devlist);

static void mz0380_resume_work_fn(struct work_struct *work);
LIST_HEAD(mz0380_devlist);

static int mz0380_request_bar(struct mz0380_dev *dev, int map, int bar)
{
	resource_size_t start = pci_resource_start(dev->pci, bar);
	resource_size_t len = pci_resource_len(dev->pci, bar);

	if (request_mem_region(start, len, dev->name) == 0) {
		pr_err("%s: can't get bar[%d] memory @ 0x%llx\n",
		       dev->name, bar, (unsigned long long)start);
		return -EBUSY;
	}

	dev->bar_nr[map] = bar;
	dev->bar_start[map] = start;
	dev->bar_len[map] = len;

	dev->lmmio[map] = ioremap(start, len);
	if (!dev->lmmio[map]) {
		release_mem_region(start, len);
		dev->bar_nr[map] = -1;
		dev->bar_start[map] = 0;
		dev->bar_len[map] = 0;
		pr_err("%s: failed to ioremap bar[%d]\n",
		       dev->name, bar);
		return -ENOMEM;
	}

	dev->bmmio[map] = (u8 __iomem *)dev->lmmio[map];
	return 0;
}

static void mz0380_release_bar(struct mz0380_dev *dev, int map)
{
	if (dev->lmmio[map]) {
		iounmap(dev->lmmio[map]);
		dev->lmmio[map] = NULL;
		dev->bmmio[map] = NULL;
	}

	if (dev->bar_nr[map] >= 0) {
		release_mem_region(dev->bar_start[map], dev->bar_len[map]);
		dev->bar_nr[map] = -1;
		dev->bar_start[map] = 0;
		dev->bar_len[map] = 0;
	}
}

static int mz0380_dev_setup(struct mz0380_dev *dev)
{
	int i;
	int ret;

	mutex_init(&dev->lock);
	mutex_init(&dev->ctrl_lock);
	mutex_init(&dev->fw_lock);
	mutex_init(&dev->cmd_lock);
	mutex_init(&dev->h264_delivery_lock);
	init_waitqueue_head(&dev->fw_wait);
	init_waitqueue_head(&dev->cmd_wait);
	spin_lock_init(&dev->buf_lock);
	spin_lock_init(&dev->event_lock);
	spin_lock_init(&dev->frame_event_lock);
	INIT_LIST_HEAD(&dev->buf_list);
	mz0380_signal_recovery_init(dev);
	mz0380_no_signal_init(dev);
	INIT_WORK(&dev->resume_work, mz0380_resume_work_fn);
	dev->fw_state = MZ0380_FW_STATE_NONE;
	atomic_set(&dev->irq_count, 0);
	atomic_set(&dev->irq_video_count, 0);
	atomic_set(&dev->irq_audio_count, 0);
	atomic_set(&dev->irq_signal_count, 0);

	ret = ida_alloc(&mz0380_ida, GFP_KERNEL);
	if (ret < 0)
		return ret;
	dev->nr = ret;
	snprintf(dev->name, sizeof(dev->name), "mz0380[%u]", dev->nr);

	for (i = 0; i < MZ0380_MAX_MAPS; i++)
		dev->bar_nr[i] = -1;

	dev->board = UNSET;
	if (dev->nr < MZ0380_MAXBOARDS && card[dev->nr] < mz0380_bcount)
		dev->board = card[dev->nr];
	for (i = 0; dev->board == UNSET && i < mz0380_idcount; i++) {
		if (dev->pci->subsystem_vendor == mz0380_subids[i].subvendor &&
		    dev->pci->subsystem_device == mz0380_subids[i].subdevice)
			dev->board = mz0380_subids[i].card;
	}
	if (dev->board == UNSET) {
		dev->board = MZ0380_BOARD_UNKNOWN;
		mz0380_card_list(dev);
	}

	ret = mz0380_request_bar(dev, MZ0380_MAP_BAR_MMIO, MZ0380_BAR_MMIO);
	if (ret < 0)
		goto fail;

	ret = mz0380_request_bar(dev, MZ0380_MAP_BAR_CFG, MZ0380_BAR_CFG);
	if (ret < 0)
		goto fail;

	if (dev->bar_len[MZ0380_MAP_BAR_MMIO] < MZ0380_BAR_MMIO_SIZE ||
	    dev->bar_len[MZ0380_MAP_BAR_CFG] < MZ0380_BAR_CFG_SIZE) {
		pr_err("%s: unexpected BAR sizes mmio=0x%llx cfg=0x%llx\n",
		       dev->name,
		       (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_MMIO],
		       (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_CFG]);
		ret = -ENODEV;
		goto fail;
	}

	pr_info("%s: subsystem: %04x:%04x, board: %s [card=%d,%s]\n",
		dev->name,
		dev->pci->subsystem_vendor,
		dev->pci->subsystem_device,
		mz0380_boards[dev->board].name,
		dev->board,
		dev->nr < MZ0380_MAXBOARDS && card[dev->nr] == dev->board ?
			"insmod option" : "autodetected");
	pr_info("%s: bar%d @ 0x%llx [0x%llx bytes], bar%d @ 0x%llx [0x%llx bytes]\n",
		dev->name,
		dev->bar_nr[MZ0380_MAP_BAR_MMIO],
		(unsigned long long)dev->bar_start[MZ0380_MAP_BAR_MMIO],
		(unsigned long long)dev->bar_len[MZ0380_MAP_BAR_MMIO],
		dev->bar_nr[MZ0380_MAP_BAR_CFG],
		(unsigned long long)dev->bar_start[MZ0380_MAP_BAR_CFG],
		(unsigned long long)dev->bar_len[MZ0380_MAP_BAR_CFG]);

	return 0;

fail:
	mz0380_release_bar(dev, MZ0380_MAP_BAR_CFG);
	mz0380_release_bar(dev, MZ0380_MAP_BAR_MMIO);
	ida_free(&mz0380_ida, dev->nr);
	return ret;
}

static void mz0380_dev_unregister(struct mz0380_dev *dev)
{
	mz0380_card_cleanup(dev);
	mz0380_release_bar(dev, MZ0380_MAP_BAR_CFG);
	mz0380_release_bar(dev, MZ0380_MAP_BAR_MMIO);
}

/*
 * M245: the card after a system resume.
 *
 * Across S3 the slot loses power and the card cold-boots from its flash; across
 * s2idle it may not. The handshake is the same either way - it is exactly what
 * every insmod does against a card that is already running - but it can take
 * seconds while the card boots, so it runs here rather than holding up resume.
 * The receiver is brought back up lazily by the next signal query.
 */
static void mz0380_resume_work_fn(struct work_struct *work)
{
	struct mz0380_dev *dev = container_of(work, struct mz0380_dev,
					      resume_work);
	int ret;

	ret = mz0380_firmware_load(dev);
	if (ret)
		dev_warn(&dev->pci->dev,
			 "resume: card handshake failed (%d); capture is unavailable until the module is reloaded\n",
			 ret);
	else
		dev_info(&dev->pci->dev, "resume: card handshake complete\n");
}

static int mz0380_initdev(struct pci_dev *pci_dev,
			  const struct pci_device_id *pci_id)
{
	struct mz0380_dev *dev;
	int err;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pci = pci_dev;

	err = pci_enable_device(pci_dev);
	if (err) {
		kfree(dev);
		return err;
	}

	pci_clear_master(pci_dev);
	/*
	 * Mask INTx for the duration of probe: nothing is registered to service
	 * the line yet, and a pending assert on a shared line would spin the
	 * kernel in unclaimed-interrupt handling between event and ack.
	 * mz0380_irq_request() unmasks it again once an ISR exists, which on
	 * the default INTx path (M82) is required for any interrupt at all.
	 */
	pci_intx(pci_dev, 0);
	pci_read_config_byte(pci_dev, PCI_CLASS_REVISION, &dev->pci_rev);
	pci_read_config_byte(pci_dev, PCI_LATENCY_TIMER, &dev->pci_lat);

	/*
	 * M4 diagnostic: bus mastering must be live BEFORE the firmware
	 * handshake (which happens inside mz0380_firmware_load below), not
	 * after. Enable it here so the CMD_INIT doorbell is issued with bus
	 * mastering on. No ring addresses are programmed (still unverified),
	 * so the card has no host DMA target - safe.
	 */
	if (mz0380_dma_handshake) {
		pci_set_master(pci_dev);
		pr_info("%s: dma_handshake: bus mastering enabled pre-firmware\n",
			pci_name(pci_dev));
	}

	pr_info("mz0380 device found at %s, rev: %u, irq: %u, latency: %u\n",
		pci_name(pci_dev), dev->pci_rev, pci_dev->irq, dev->pci_lat);
	pr_info("%s: probe mode: bus master initially disabled\n",
		pci_name(pci_dev));

	err = mz0380_dev_setup(dev);
	if (err < 0)
		goto fail_disable;

	/*
	 * Phase 1: card handshake. Nothing is uploaded - the card boots
	 * its own flash image; we only shake hands and read its version.
	 */
	err = mz0380_firmware_load(dev);
	if (err) {
		pr_warn("%s: firmware load skipped/failed (%d) - continuing without it\n",
			dev->name, err);
		/* not fatal during bring-up */
	}

	/*
	 * Phase 2: request IRQ + alloc DMA rings, gated by enable_dma.
	 * Bus mastering is enabled only inside mz0380_dma_setup, AFTER
	 * the ring base addresses have been programmed.
	 */
	if (mz0380_enable_dma) {
		err = mz0380_irq_request(dev);
		if (err) {
			pr_warn("%s: IRQ request failed (%d) - continuing without DMA\n",
				dev->name, err);
		} else {
			err = mz0380_dma_setup(dev);
			if (err) {
				pr_warn("%s: DMA setup failed (%d) - continuing without streaming\n",
					dev->name, err);
				mz0380_irq_release(dev);
			}
		}
	} else if (allow_bus_master) {
		/* legacy escape hatch: bus-master without DMA setup */
		pci_set_master(pci_dev);
	}

	err = mz0380_card_setup(dev);
	if (err < 0)
		goto fail_dma;

	pci_set_drvdata(pci_dev, dev);

	mutex_lock(&devlist);
	list_add_tail(&dev->devlist, &mz0380_devlist);
	mutex_unlock(&devlist);

	dev_dbg(&pci_dev->dev, "probe complete\n");
	return 0;

fail_dma:
	mz0380_irq_release(dev);
	mz0380_dma_teardown(dev);
	mz0380_firmware_release(dev);
	mz0380_dev_unregister(dev);
	ida_free(&mz0380_ida, dev->nr);
fail_disable:
	pci_disable_device(pci_dev);
	kfree(dev);
	return err;
}

static void mz0380_finidev(struct pci_dev *pci_dev)
{
	struct mz0380_dev *dev = pci_get_drvdata(pci_dev);

	if (!dev)
		return;

	mutex_lock(&devlist);
	list_del(&dev->devlist);
	mutex_unlock(&devlist);

	cancel_work_sync(&dev->resume_work);

	/*
	 * M245: unregister the node FIRST. vb2_video_unregister_device() stops
	 * a running stream through the normal stop_streaming path while every
	 * mapping it touches is still live, and releases the queue so a handle
	 * kept open past this point has nothing left to stop when it closes.
	 */
	mz0380_card_cleanup(dev);

	/* stop the event watcher before any MMIO mapping is torn down */
	mz0380_event_watch_stop(dev);

	mz0380_nosg_capture_stop(dev);	/* join before the bufs it polls die */
	mz0380_dma_stop(dev);
	/*
	 * M84: free the IRQ BEFORE any MMIO mapping goes away. On the shared
	 * INTx line every other device on the line enters our handler, and the
	 * first one to do so after the unmap dereferenced a NULL bmmio and
	 * oopsed inside rmmod.
	 */
	mz0380_irq_release(dev);
	mz0380_dev_unregister(dev);
	mz0380_dma_teardown(dev);
	mz0380_firmware_release(dev);
	pci_clear_master(pci_dev);
	pci_disable_device(pci_dev);
	ida_free(&mz0380_ida, dev->nr);
	/* Frees now, or when the last open handle on the node closes. */
	mz0380_dev_put(dev);
}

/*
 * M245: a reboot or kexec must not inherit a streaming card. See
 * mz0380_dma_quiesce() for why one could be left running.
 */
static void mz0380_shutdown(struct pci_dev *pci_dev)
{
	struct mz0380_dev *dev = pci_get_drvdata(pci_dev);

	if (!dev)
		return;

	cancel_work_sync(&dev->resume_work);
	mz0380_event_watch_stop(dev);
	mz0380_nosg_capture_stop(dev);
	mz0380_dma_quiesce(dev);
	pci_clear_master(pci_dev);
}

/*
 * M245: system sleep.
 *
 * A capture cannot be carried across: the card may be power-cycled, and a
 * persistent encoder does not survive that. So the queue is failed - a
 * streaming application sees DQBUF return an error and restarts the stream
 * after resume, which is the standard contract - and the card is quiesced
 * exactly as for shutdown.
 */
static int mz0380_suspend(struct device *d)
{
	struct pci_dev *pci_dev = to_pci_dev(d);
	struct mz0380_dev *dev = pci_get_drvdata(pci_dev);

	if (!dev)
		return 0;

	cancel_work_sync(&dev->resume_work);
	if (dev->video_registered && vb2_is_streaming(&dev->vb_queue))
		vb2_queue_error(&dev->vb_queue);
	mz0380_event_watch_stop(dev);
	mz0380_nosg_capture_stop(dev);
	mz0380_dma_quiesce(dev);
	pci_clear_master(pci_dev);
	return 0;
}

static int mz0380_resume(struct device *d)
{
	struct pci_dev *pci_dev = to_pci_dev(d);
	struct mz0380_dev *dev = pci_get_drvdata(pci_dev);

	if (!dev)
		return 0;

	if (dev->dma_armed || allow_bus_master || mz0380_dma_handshake)
		pci_set_master(pci_dev);
	/* The card may have cold-booted: redo the handshake, off this path. */
	mz0380_firmware_release(dev);
	dev->mst3367_ready = false;
	queue_work(MZ0380_SYSTEM_WQ, &dev->resume_work);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mz0380_pm_ops, mz0380_suspend, mz0380_resume);

static const struct pci_device_id mz0380_pci_tbl[] = {
	/* Rev. 1 (original)                       1cfa:0003 */
	{ .vendor = 0x12ab, .device = 0x0380,
	  .subvendor = 0x1cfa, .subdevice = 0x0003 },
	/* Rev. 2 (never released)                 1cfa:0005 */
	{ .vendor = 0x12ab, .device = 0x0380,
	  .subvendor = 0x1cfa, .subdevice = 0x0005 },
	/* Rev. 1 + Ryzen fix                      1cfa:0006 */
	{ .vendor = 0x12ab, .device = 0x0380,
	  .subvendor = 0x1cfa, .subdevice = 0x0006 },
	/* Rev. 3 (device id 0x0381)               1cfa:0010 */
	{ .vendor = 0x12ab, .device = 0x0381,
	  .subvendor = 0x1cfa, .subdevice = 0x0010 },
	/* Prototype                               12ab:05cf */
	{ .vendor = 0x12ab, .device = 0x0380,
	  .subvendor = 0x12ab, .subdevice = 0x05cf },
	{ }
};
MODULE_DEVICE_TABLE(pci, mz0380_pci_tbl);
/*
 * The only file this driver ever asks the firmware loader for: the 6-byte
 * ASCII version sidecar, read so a version mismatch can be reported. No image
 * is ever sent to the card.
 */
MODULE_FIRMWARE("mz0380/MZ0380.FW.TXT");

static struct pci_driver mz0380_pci_driver = {
	.name = "mz0380",
	.id_table = mz0380_pci_tbl,
	.probe = mz0380_initdev,
	.remove = mz0380_finidev,
	.shutdown = mz0380_shutdown,
	.driver.pm = pm_sleep_ptr(&mz0380_pm_ops),
};

static int __init mz0380_init(void)
{
	int ret;

	pr_info("mz0380 driver version %d.%d.%d loaded\n",
		(MZ0380_VERSION_CODE >> 16) & 0xff,
		(MZ0380_VERSION_CODE >> 8) & 0xff,
		MZ0380_VERSION_CODE & 0xff);

	ret = mz0380_proc_create();
	if (ret < 0)
		return ret;

	ret = pci_register_driver(&mz0380_pci_driver);
	if (ret < 0) {
		mz0380_proc_remove();
		return ret;
	}

	return 0;
}

static void __exit mz0380_fini(void)
{
	mz0380_proc_remove();
	pci_unregister_driver(&mz0380_pci_driver);
	pr_info("mz0380 driver unloaded\n");
}

module_init(mz0380_init);
module_exit(mz0380_fini);
