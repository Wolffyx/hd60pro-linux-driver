// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Driver for MZ0380 based capture cards.
 *
 *  ALSA capture of the HDMI audio the card extracts (M246).
 *
 *  The card DMAs PCM into the audio window - four 4 KiB slots registered with
 *  op 0x03 - and reports each filled slot with its own EVENT bit and a token in
 *  BAR0+0x4c (see MZ0380_AUDIO_NR_SLOTS in mz0380-reg.h). mz0380-dma.c measures
 *  how much of the slot was written and hands those bytes here; this file is
 *  only a ring that copies them into the ALSA buffer and reports periods.
 *
 *  The format is whatever SET_AIC asked for at stream start - aic_channels,
 *  aic_bits and aic_freq, 2 x 16-bit at 48 kHz by default - so that is the one
 *  format offered. Audio only flows while the card's pipeline is running, i.e.
 *  while something is (or, with the persistent pipeline, has been) capturing
 *  video: a capture opened on its own waits for the first slot.
 *
 *  Opt-in with enable_audio=1 until verified on hardware.
 */

#include "mz0380.h"

#if IS_ENABLED(CONFIG_SND)

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

struct mz0380_pcm {
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct mz0380_dev *dev;
	/* Protects everything below against the push from the drain worker. */
	spinlock_t lock;
	struct snd_pcm_substream *substream;
	bool running;
	unsigned int period_bytes;
	unsigned int buffer_bytes;
	unsigned int hw_ptr;		/* next byte ALSA will be given    */
	unsigned int period_pos;	/* bytes since the last period      */
};

static int mz0380_pcm_open(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;
	struct snd_pcm_runtime *rt = ss->runtime;
	unsigned int rate = mz0380_aic_freq;
	unsigned int chans = mz0380_aic_channels;
	unsigned long flags;

	/*
	 * One format: the one the card was told to produce. Anything the
	 * rest of this file cannot label honestly is refused rather than
	 * delivered as something it is not.
	 */
	if (mz0380_aic_bits != 16 || chans < 1 || chans > 2 ||
	    rate < 8000 || rate > 192000) {
		dev_warn(&dev->pci->dev,
			 "audio: SET_AIC is %u ch x %u bit at %u Hz; only 1-2 ch x 16 bit is offered\n",
			 chans, mz0380_aic_bits, rate);
		return -EINVAL;
	}

	rt->hw.info = SNDRV_PCM_INFO_INTERLEAVED |
		      SNDRV_PCM_INFO_BLOCK_TRANSFER |
		      SNDRV_PCM_INFO_MMAP |
		      SNDRV_PCM_INFO_MMAP_VALID;
	rt->hw.formats = SNDRV_PCM_FMTBIT_S16_LE;
	rt->hw.rates = snd_pcm_rate_to_rate_bit(rate);
	if (rt->hw.rates == SNDRV_PCM_RATE_KNOT)
		rt->hw.rates = SNDRV_PCM_RATE_CONTINUOUS;
	rt->hw.rate_min = rate;
	rt->hw.rate_max = rate;
	rt->hw.channels_min = chans;
	rt->hw.channels_max = chans;
	rt->hw.buffer_bytes_max = 256 * 1024;
	rt->hw.period_bytes_min = 1024;
	rt->hw.period_bytes_max = 64 * 1024;
	rt->hw.periods_min = 2;
	rt->hw.periods_max = 64;

	spin_lock_irqsave(&pcm->lock, flags);
	pcm->substream = ss;
	pcm->running = false;
	spin_unlock_irqrestore(&pcm->lock, flags);
	dev->audio_alsa_opens++;
	return 0;
}

static int mz0380_pcm_close(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;
	unsigned long flags;

	spin_lock_irqsave(&pcm->lock, flags);
	pcm->running = false;
	pcm->substream = NULL;
	spin_unlock_irqrestore(&pcm->lock, flags);
	dev->audio_alsa_closes++;
	return 0;
}

static int mz0380_pcm_hw_params(struct snd_pcm_substream *ss,
				struct snd_pcm_hw_params *p)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;
	unsigned long flags;

	spin_lock_irqsave(&pcm->lock, flags);
	pcm->period_bytes = params_period_bytes(p);
	pcm->buffer_bytes = params_buffer_bytes(p);
	spin_unlock_irqrestore(&pcm->lock, flags);
	return 0;
}

static int mz0380_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;
	unsigned long flags;

	spin_lock_irqsave(&pcm->lock, flags);
	pcm->hw_ptr = 0;
	pcm->period_pos = 0;
	spin_unlock_irqrestore(&pcm->lock, flags);
	dev->audio_alsa_prepares++;
	return 0;
}

static int mz0380_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	/* Called with the stream lock held, so no irqsave needed on top. */
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		spin_lock(&pcm->lock);
		pcm->running = true;
		spin_unlock(&pcm->lock);
		dev->audio_alsa_starts++;
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		spin_lock(&pcm->lock);
		pcm->running = false;
		spin_unlock(&pcm->lock);
		dev->audio_alsa_stops++;
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t mz0380_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct mz0380_dev *dev = snd_pcm_substream_chip(ss);
	struct mz0380_pcm *pcm = dev->snd_pcm;

	return bytes_to_frames(ss->runtime, READ_ONCE(pcm->hw_ptr));
}

static const struct snd_pcm_ops mz0380_pcm_ops = {
	.open      = mz0380_pcm_open,
	.close     = mz0380_pcm_close,
	.hw_params = mz0380_pcm_hw_params,
	.prepare   = mz0380_pcm_prepare,
	.trigger   = mz0380_pcm_trigger,
	.pointer   = mz0380_pcm_pointer,
};

/*
 * M246: one slot's worth of PCM from the drain worker. Copied into the ALSA
 * buffer at the hardware pointer, wrapping, with a period reported each time
 * enough bytes have arrived - the slot size (4 KiB) and the period size the
 * application chose need not match.
 */
void mz0380_audio_push(struct mz0380_dev *dev, const void *data, size_t len)
{
	struct mz0380_pcm *pcm = READ_ONCE(dev->snd_pcm);
	struct snd_pcm_substream *ss;
	unsigned long flags;
	bool elapsed = false;
	const u8 *src = data;
	u8 *dst;

	if (!pcm) {
		dev->audio_alsa_bails++;
		dev->audio_alsa_bail_why |= 1;
		return;
	}

	spin_lock_irqsave(&pcm->lock, flags);
	ss = pcm->substream;
	if (!ss || !pcm->running || !pcm->buffer_bytes || !pcm->period_bytes ||
	    !ss->runtime || !ss->runtime->dma_area) {
		dev->audio_alsa_bails++;
		if (!ss)
			dev->audio_alsa_bail_closed++;
		else if (!pcm->running)
			dev->audio_alsa_bail_idle++;
		dev->audio_alsa_bail_why |= (!ss ? 2 : 0) |
			(!pcm->running ? 4 : 0) |
			(!pcm->buffer_bytes || !pcm->period_bytes ? 8 : 0) |
			(ss && (!ss->runtime || !ss->runtime->dma_area) ? 16 : 0);
		spin_unlock_irqrestore(&pcm->lock, flags);
		return;
	}
	dev->audio_alsa_bytes += len;
	dst = ss->runtime->dma_area;
	while (len) {
		size_t chunk = min_t(size_t, len,
				     pcm->buffer_bytes - pcm->hw_ptr);

		memcpy(dst + pcm->hw_ptr, src, chunk);
		src += chunk;
		len -= chunk;
		pcm->hw_ptr = (pcm->hw_ptr + chunk) % pcm->buffer_bytes;
		pcm->period_pos += chunk;
	}
	if (pcm->period_pos >= pcm->period_bytes) {
		pcm->period_pos %= pcm->period_bytes;
		elapsed = true;
	}
	spin_unlock_irqrestore(&pcm->lock, flags);

	if (elapsed) {
		dev->audio_alsa_periods++;
		snd_pcm_period_elapsed(ss);
	}
}

int mz0380_audio_register(struct mz0380_dev *dev)
{
	struct snd_card *card;
	struct snd_pcm *pcm_dev;
	struct mz0380_pcm *pcm;
	int ret;

	if (dev->audio_registered)
		return 0;
	/* No audio window, nothing to capture from: do not offer a device. */
	if (!dev->audio_capable)
		return -ENODEV;

	ret = snd_card_new(&dev->pci->dev, SNDRV_DEFAULT_IDX1, "mz0380",
			   THIS_MODULE, sizeof(*pcm), &card);
	if (ret)
		return ret;

	pcm = card->private_data;
	pcm->card = card;
	pcm->dev = dev;
	spin_lock_init(&pcm->lock);

	strscpy(card->driver, "mz0380", sizeof(card->driver));
	strscpy(card->shortname, "HD60 Pro HDMI", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "%s on %s", mz0380_boards[dev->board].name,
		 pci_name(dev->pci));

	ret = snd_pcm_new(card, "HD60 Pro HDMI", 0, 0, 1, &pcm_dev);
	if (ret)
		goto fail_card;
	pcm->pcm = pcm_dev;
	pcm_dev->private_data = dev;
	pcm_dev->info_flags = 0;
	strscpy(pcm_dev->name, "HD60 Pro HDMI", sizeof(pcm_dev->name));
	snd_pcm_set_ops(pcm_dev, SNDRV_PCM_STREAM_CAPTURE, &mz0380_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm_dev, SNDRV_DMA_TYPE_VMALLOC, NULL,
				       0, 256 * 1024);

	/* Published before registration so the first open finds it. */
	dev->snd_pcm = pcm;
	ret = snd_card_register(card);
	if (ret)
		goto fail_card;

	dev->snd_card = card;
	dev->audio_registered = true;
	pr_info("%s: ALSA card '%s' registered\n", dev->name, card->shortname);
	return 0;

fail_card:
	dev->snd_pcm = NULL;
	snd_card_free(card);
	return ret;
}

void mz0380_audio_unregister(struct mz0380_dev *dev)
{
	if (!dev->audio_registered)
		return;

	/*
	 * The card's private data - the ring the drain worker pushes into - is
	 * freed with the card. Remove unregisters ALSA before it stops DMA, so
	 * unpublish the ring and wait out any push already in flight first.
	 */
	WRITE_ONCE(dev->snd_pcm, NULL);
	if (dev->irq_requested)
		flush_work(&dev->audio_work);

	/* Waits for every open handle to close before it returns. */
	if (dev->snd_card) {
		snd_card_free(dev->snd_card);
		dev->snd_card = NULL;
	}
	dev->audio_registered = false;
}

#endif /* CONFIG_SND */
