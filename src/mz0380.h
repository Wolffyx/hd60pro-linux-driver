/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Driver for MZ0380 based capture cards (Elgato Game Capture HD60 Pro).
 *
 *  Copyright (c) 2026
 */

#ifndef _MZ0380_H_
#define _MZ0380_H_

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/videodev2.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dv-timings.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "mz0380-reg.h"

#define MZ0380_VERSION			"0.2.0"

#define MZ0380_MAXBOARDS		8
#define MZ0380_MAX_MAPS			2
#define UNSET				(-1U)

#define MZ0380_BOARD_UNKNOWN		0
#define MZ0380_BOARD_ELGATO_HD60_PRO	1
#define MZ0380_BOARD_ELGATO_HD60_PRO_REV3 2

/* V4L2 encoder controls. Defaults are the Windows main-stream values. */
#define MZ0380_MIN_BITRATE		(256 * 1024)
#define MZ0380_MAX_BITRATE		(12 * 1024 * 1024)
#define MZ0380_ENC_DEFAULT_BITRATE	4194304
#define MZ0380_MIN_GOP			1
#define MZ0380_ENC_DEFAULT_GOP		32
#define MZ0380_ENC_MAX_GOP		255	/* SET_ENC_PARAMS carries one byte */

/*
 * Receiver and recovery timing. These were module parameters during bring-up;
 * the values are the ones every hardware run was validated with.
 */
#define MZ0380_SIGNAL_POLL_MS		2000	/* longest wait for a timing lock  */
#define MZ0380_SIGNAL_QUERY_CACHE_MS	1000	/* collapse ioctl query bursts     */
#define MZ0380_SIGNAL_CACHE_MS		30000	/* M65: arm with a recent lock     */
#define MZ0380_SIGNAL_MONITOR_MS	500	/* receiver poll while streaming   */
#define MZ0380_HOTPLUG_STALL_MS		1500	/* producer silence = signal lost  */
#define MZ0380_HOTPLUG_RETRY_MS		500	/* receiver poll while recovering  */
#define MZ0380_UNSUPPORTED_RETRY_MS	3000	/* a locked mode we cannot capture */
#define MZ0380_PENDING_RECHECK_MS	2000	/* replacement pending, still live */
#define MZ0380_CARD_READY_TIMEOUT_MS	15000	/* CMD_INIT retries after boot     */
#define MZ0380_NO_SIGNAL_FPS		2
/* A raw frame this recent means the source is live; no placeholder needed. */
#define MZ0380_RAW_LIVE_MS		1000

/*
 * How the fill probes sample a raw slot: 32 dwords spread over the last 64 luma
 * rows (M229), and 16 per chroma plane (M238).
 */
#define MZ0380_RAW_FILL_SAMPLES		32
#define MZ0380_RAW_FILL_ROWS		64
/* M251: <.25, ~.5, ~1, ~1.5, ~2 and >2.25 frame periods */
#define MZ0380_CADENCE_BUCKETS		6

#define MZ0380_FRAME_EVENT_FIFO_SIZE	64
#define MZ0380_H264_PARAMETER_SETS_MAX	4096

#ifdef MZ0380_HAVE_SYSTEM_DFL_WQ
#define MZ0380_SYSTEM_WQ system_dfl_wq
#else
#define MZ0380_SYSTEM_WQ system_wq
#endif

struct snd_card;
struct mz0380_dev;

struct mz0380_board {
	const char *name;
};

struct mz0380_subid {
	u16 subvendor;
	u16 subdevice;
	u32 card;
};

/*
 * Card handshake state. The card boots its onboard ARM Linux from its OWN
 * FLASH - the host never sends an image. REQUESTED means the handshake is in
 * flight.
 */
enum mz0380_fw_state {
	MZ0380_FW_STATE_NONE = 0,
	MZ0380_FW_STATE_REQUESTED,
	MZ0380_FW_STATE_READY,
	MZ0380_FW_STATE_FAILED,
};

struct mz0380_vb_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct mz0380_capture_state {
	u32 width;
	u32 height;
	/* Receiver timing, kept separate from the negotiated geometry. */
	u32 source_width;
	u32 source_height;
	u32 source_fps;
	bool source_interlaced;
	/* What SET_ENC_PARAMS sends for the main stream (V4L2 controls). */
	u32 enc_bitrate;
	u32 enc_gop;
};

/*
 * Frame-completion mailboxes are one-shot: EVENT acknowledgement lets the
 * endpoint overwrite TOKEN/PAYLOAD immediately. Keep what delivery needs from
 * the pre-ACK image in a bounded IRQ-safe FIFO for process context.
 */
struct mz0380_frame_event {
	u64 timestamp_ns;
	u32 event;
	u32 token;
	u32 payload0;	/* BAR0+0x44: tinyvenc7's encoded window-1 slot */
};

/* One window-1 (H.264) or window-3 (audio) slot at its own 4 GiB IOVA. */
struct mz0380_dma_buf {
	void *va;
	dma_addr_t dma;
	struct page *pages;
	u32 delivered;
};

/*
 * One window-0 raw slot. op 0x02 registers bank 0 (slots 0..3), op 0x08 bank 1
 * (4..7), exactly as Windows does; only bank 0 is ever written with frames.
 * Order-0 pages behind one contiguous IOVA and one vmap.
 */
struct mz0380_raw_buf {
	void *va;
	dma_addr_t dma;
	struct page **pages;
	u32 nr_pages;
	size_t mapped;
	u32 delivered;
	/* M219: the head of the frame this slot last delivered. */
	u8 last_head[16];
	bool last_head_valid;
	/* M247: when its chroma first read as the clear value (0 = not). */
	u64 neutral_since_ns;
};

struct mz0380_dev {
	struct list_head devlist;
	struct pci_dev *pci;
	struct mutex lock;		/* video node, vb2 queue, capture state */
	struct mutex ctrl_lock;
	char name[32];
	unsigned int nr;
	unsigned int board;
	void __iomem *lmmio[MZ0380_MAX_MAPS];
	u8 __iomem *bmmio[MZ0380_MAX_MAPS];
	int bar_nr[MZ0380_MAX_MAPS];
	resource_size_t bar_start[MZ0380_MAX_MAPS];
	resource_size_t bar_len[MZ0380_MAX_MAPS];

	struct v4l2_device v4l2_dev;
	struct v4l2_ctrl_handler ctrl_handler;
	struct video_device vdev;
	struct v4l2_ctrl *bitrate_ctrl;
	struct v4l2_ctrl *gop_ctrl;
	struct mz0380_capture_state capture;
	bool v4l2_registered;
	bool ctrl_handler_initialized;
	bool video_registered;

	/* Card handshake */
	enum mz0380_fw_state fw_state;
	struct work_struct resume_work;	/* M245: post-resume handshake */
	struct mutex fw_lock;
	u16 fw_version_major;
	u16 fw_version_minor;

	/* IRQ */
	int irq;
	bool irq_requested;
	atomic_t irq_count;
	atomic_t irq_video_count;

	/* Command mailbox */
	struct mutex cmd_lock;
	spinlock_t event_lock;
	wait_queue_head_t cmd_wait;
	u32 cmd_last_status;
	u32 cmd_last_param[MZ0380_REG_PARAM_MAX];
	bool cmd_complete;

	/* DMA windows (RE_FINDINGS.md M17, M26, M211) */
	bool dma_armed;
	struct mz0380_dma_buf h264_bufs[MZ0380_STREAM_NR_BUFS];
	struct mz0380_raw_buf raw_bufs[MZ0380_RAW_NR_BUFS];
	struct mz0380_dma_buf audio_bufs[MZ0380_AUDIO_NR_SLOTS];

	/* What the node delivers: raw (one of three layouts) or H.264 (M218). */
	bool deliver_raw;
	u32 raw_fourcc;

	/* Raw delivery (M217-M247) */
	u32 raw_next_slot;
	unsigned long raw_last_live;	/* jiffies of the last whole raw frame */
	u64 raw_frames_delivered;
	u64 raw_frames_dropped;
	u64 raw_stub_completions;
	u64 raw_dup_content;
	u64 raw_frames_torn;
	u64 raw_multi_landed;
	u64 raw_incomplete_tail;
	u64 raw_incomplete_chroma;
	u64 raw_chroma_probes;
	u64 raw_chroma_neutral;
	u32 raw_chroma_max_clear;
	/*
	 * M251: the delivery rhythm, as histograms of spacing in frame periods
	 * (mz0380_cadence_note): the card's video events, the raw frames taken,
	 * slots ready per scan, and events handled per drain run.
	 */
	u64 cadence_event_prev_ns;
	u64 cadence_take_prev_ns;
	u32 cadence_event[MZ0380_CADENCE_BUCKETS];
	u32 cadence_take[MZ0380_CADENCE_BUCKETS];
	u32 cadence_ready[MZ0380_STREAM_NR_BUFS + 1];
	u32 cadence_batch[3];

	/* H.264 delivery */
	u8 h264_last_token;
	bool h264_last_token_valid;
	u64 h264_frames_delivered;
	u64 h264_frames_dropped;
	u64 h264_frames_discarded;
	u64 h264_frames_suppressed;
	u8 *h264_parameter_sets;
	u32 h264_parameter_sets_len;
	struct mutex delivery_lock;	/* one producer of vb2 buffers at a time */

	/* Host-owned NO SIGNAL picture while the receiver is unlocked. */
	struct delayed_work no_signal_work;
	bool no_signal_active;
	u64 no_signal_frames_delivered;
	u64 no_signal_frames_missed;
	u64 no_signal_raw_frames;

	/* Receiver monitor and hotplug recovery */
	struct delayed_work signal_recovery_work;
	unsigned long last_h264_frame_stamp;
	unsigned long silent_stamp;	/* producer silence already handled */
	unsigned long pending_measure_stamp;
	u32 signal_recovery_attempts;
	bool signal_recovering;
	bool signal_lost_reported;
	bool h264_waiting_for_idr;

	/* vb2 */
	struct vb2_queue vb_queue;
	struct list_head buf_list;
	spinlock_t buf_lock;
	u32 video_sequence;
	bool streaming;

	/* Completion events */
	struct work_struct drain_work;
	struct workqueue_struct *drain_wq;	/* M245 */
	spinlock_t frame_event_lock;
	struct mz0380_frame_event frame_events[MZ0380_FRAME_EVENT_FIFO_SIZE];
	u16 frame_event_head;
	u16 frame_event_tail;
	bool frame_event_drain_scheduled;
	bool frame_events_accepting;
	bool frame_event_ack_deferred;
	u64 frame_event_drops;

	/*
	 * Card-side encoder pipeline, which outlives a VB2 consumer under
	 * persistent_h264. Every SET_VIC forks a fresh tinyvenc on the card, and
	 * the card wedges somewhere in 8-18 of those per POWER CYCLE (M169).
	 */
	u32 encoder_spawns;
	bool pipeline_running;
	bool pipeline_reconfigure_pending;	/* HDMI timing changed */
	bool pipeline_start_failed;
	bool aic_armed;
	u32 pipeline_attach_count;
	u32 pipeline_width;
	u32 pipeline_height;
	u32 pipeline_source_width;
	u32 pipeline_source_height;
	u32 pipeline_source_fps;
	bool pipeline_source_interlaced;
	/* What the running pipeline was configured with (M247). */
	bool pipeline_raw;
	u32 pipeline_enc_bitrate;
	u32 pipeline_enc_gop;

	/* HDMI receiver */
	struct v4l2_dv_timings detected_timings;
	/* M171: what S_DV_TIMINGS was told; G returns it, QUERY detects. */
	struct v4l2_dv_timings set_timings;
	bool timings_set_by_user;
	/* M65: the last good detection, kept across later failures. */
	struct v4l2_dv_timings last_good_timings;
	unsigned long last_good_stamp;
	bool have_last_good;
	bool signal_locked;
	bool mst3367_ready;	/* reset released + init applied */
	bool mst_gpio_dir_invert;
	/* M133: last BANK2 0x48 (input colour space), read by the output diag. */
	u8 mst_b2_48;
	/* M252: and 0x4a, AVI PB3 - bits 3:2 the declared RGB quantization */
	u8 mst_b2_4a;
	bool mst_b2_48_valid;
	u8 mst_csc_applied;
	bool mst_csc_applied_valid;
	u64 mst_rearms;	/* M233: AUTO_POSITION re-arms on real lock loss */
	/* M247: the output stage was committed for this mode already. */
	struct v4l2_dv_timings mst_committed;
	bool mst_committed_valid;
	/* M247: log a measured mode or an unsupported one once, not per poll. */
	struct v4l2_dv_timings mst_logged;
	u16 mst_unsupported_htotal;
	u16 mst_unsupported_vtotal;

	/* ALSA (M246) */
	struct snd_card *snd_card;
	void *snd_pcm;
	bool audio_registered;
	bool audio_capable;
	struct work_struct audio_work;
	struct {
		u32 event;
		u32 token;
	} audio_fifo[MZ0380_AUDIO_EVENT_FIFO_SIZE];
	u16 audio_fifo_head;
	u16 audio_fifo_tail;
	u32 audio_last_slot;
	u64 audio_events;
	u64 audio_slots;
	u64 audio_bytes;
	u64 audio_empty;
	u64 audio_overrun;
	u64 audio_bad_token;
	u64 audio_fifo_drops;
	u32 audio_max_extent;
	u32 audio_last_event;
	u32 audio_last_token;
	u8 audio_slots_seen;
	u64 audio_first_ns;
	u64 audio_last_ns;
	u64 audio_alsa_bytes;
	u64 audio_alsa_periods;
	u64 audio_alsa_bails;
	u32 audio_alsa_bail_why;
	u32 audio_alsa_opens;
	u32 audio_alsa_prepares;
	u32 audio_alsa_starts;
	u32 audio_alsa_stops;
	u32 audio_alsa_closes;
	u64 audio_alsa_bail_closed;
	u64 audio_alsa_bail_idle;
};

/* ---- module parameters (mz0380-params.c) ------------------------------- */

extern bool mz0380_enable_video;
extern bool mz0380_enable_audio;
extern bool mz0380_raw_deliver;
extern bool mz0380_persistent_h264;
extern bool mz0380_stream_without_signal;
extern bool mz0380_raw_full_range;
extern unsigned int mz0380_h264_frame_divisor;
extern unsigned int mz0380_post_skip;
extern unsigned long long mz0380_dma_iova_base;
extern bool mz0380_aic_every_frame;
extern unsigned int mz0380_aic_channels;
extern unsigned int mz0380_aic_bits;
extern unsigned int mz0380_aic_freq;
extern unsigned int mz0380_aic_period_frames;
extern unsigned int mz0380_aic_periods;
extern unsigned int mz0380_aic_int_mode;

/* ---- boards and PCI lifecycle (mz0380-cards.c, mz0380-pci.c) ----------- */

extern const struct mz0380_board mz0380_boards[];
extern const unsigned int mz0380_bcount;
extern const struct mz0380_subid mz0380_subids[];
extern const unsigned int mz0380_idcount;
extern struct mutex mz0380_devlist_lock;
extern struct list_head mz0380_devlist;

void mz0380_card_list(struct mz0380_dev *dev);
int mz0380_card_setup(struct mz0380_dev *dev);
void mz0380_card_cleanup(struct mz0380_dev *dev);

/* ---- procfs (mz0380-proc.c) -------------------------------------------- */

int mz0380_proc_create(void);
void mz0380_proc_remove(void);

/* ---- register access ---------------------------------------------------- */

static inline u32 mz_read(struct mz0380_dev *dev, int map, u32 reg)
{
	return readl(dev->lmmio[map] + reg);
}

static inline void mz_write(struct mz0380_dev *dev, int map, u32 reg, u32 value)
{
	writel(value, dev->lmmio[map] + reg);
}

static inline u32 mz_cfg_read(struct mz0380_dev *dev, u32 reg)
{
	return mz_read(dev, MZ0380_MAP_BAR_CFG, reg);
}

static inline void mz_cfg_write(struct mz0380_dev *dev, u32 reg, u32 v)
{
	mz_write(dev, MZ0380_MAP_BAR_CFG, reg, v);
}

static inline u32 mz_mmio_read(struct mz0380_dev *dev, u32 reg)
{
	return mz_read(dev, MZ0380_MAP_BAR_MMIO, reg);
}

static inline void mz_mmio_write(struct mz0380_dev *dev, u32 reg, u32 v)
{
	mz_write(dev, MZ0380_MAP_BAR_MMIO, reg, v);
}

/* ---- command mailbox (mz0380-mailbox.c) --------------------------------- */

int mz0380_send_command(struct mz0380_dev *dev, u32 opcode,
			const u32 *params, unsigned int nparams,
			u32 *status_out, unsigned int timeout_ms);
/* Copy PARAM0..reply_words-1 before cmd_lock permits another transaction. */
int mz0380_send_command_reply(struct mz0380_dev *dev, u32 opcode,
			      const u32 *params, unsigned int nparams,
			      u32 *status_out, unsigned int timeout_ms,
			      u32 *reply, unsigned int reply_words);
void mz0380_mb_ack_event(struct mz0380_dev *dev);
int mz0380_periph_read(struct mz0380_dev *dev, u8 chip, u8 reg, u32 *val);
int mz0380_periph_write(struct mz0380_dev *dev, u8 chip, u8 reg, u32 val);
int mz0380_card_init(struct mz0380_dev *dev);

/* ---- card handshake (mz0380-fw.c) --------------------------------------- */

int mz0380_firmware_load(struct mz0380_dev *dev);
void mz0380_firmware_release(struct mz0380_dev *dev);
const char *mz0380_fw_state_name(enum mz0380_fw_state s);

/* ---- DMA, IRQ and streaming (mz0380-dma*.c) ------------------------------ */

int mz0380_irq_request(struct mz0380_dev *dev);
void mz0380_irq_release(struct mz0380_dev *dev);
int mz0380_dma_setup(struct mz0380_dev *dev);
void mz0380_dma_teardown(struct mz0380_dev *dev);
int mz0380_dma_start(struct mz0380_dev *dev);
void mz0380_dma_stop(struct mz0380_dev *dev);
void mz0380_dma_quiesce(struct mz0380_dev *dev);
/*
 * Non-sleeping pre-ACK hook. mz0380_mb_ack_event() calls it with the live
 * EVENT value before clearing EVENT, so TOKEN/PAYLOAD cannot be lost when the
 * command poller (rather than the ISR) consumes a combined event.
 */
void mz0380_handle_event_snapshot(struct mz0380_dev *dev, u32 event);
void mz0380_dma_flush_events(struct mz0380_dev *dev);
size_t mz0380_raw_frame_bytes(struct mz0380_dev *dev);
size_t mz0380_raw_frame_bytes_for(u32 width, u32 height);

/* ---- V4L2 (mz0380-video.c, mz0380-vb2.c) -------------------------------- */

extern const struct vb2_ops mz0380_qops;
int mz0380_video_register(struct mz0380_dev *dev);
void mz0380_video_unregister(struct mz0380_dev *dev);
void mz0380_dev_put(struct mz0380_dev *dev);
void mz0380_capture_state_init(struct mz0380_dev *dev);
void mz0380_encoder_ctrls_grab(struct mz0380_dev *dev, bool grabbed);
bool mz0380_is_raw_fourcc(u32 fourcc);
u32 mz0380_current_pixelformat(struct mz0380_dev *dev);
u32 mz0380_current_sizeimage(struct mz0380_dev *dev);
bool mz0380_pipeline_config_stale(struct mz0380_dev *dev);
bool mz0380_detached_source_lost(struct mz0380_dev *dev);

/* ---- HDMI signal and DV timings (mz0380-signal.c) ------------------------ */

extern const struct v4l2_dv_timings mz0380_no_signal;
extern const struct v4l2_dv_timings mz0380_default_timings;
struct mz0380_mode {
	u32 width;
	u32 height;
};
const struct mz0380_mode *mz0380_find_mode(u32 width, u32 height);
u32 mz0380_source_fps(const struct v4l2_dv_timings *timings);
int mz0380_query_signal(struct mz0380_dev *dev,
			struct v4l2_dv_timings *timings);
void mz0380_signal_event(struct mz0380_dev *dev);
void mz0380_signal_clear(struct mz0380_dev *dev);
int mz0380_signal_idle_lock(struct mz0380_dev *dev, bool *locked);
void mz0380_signal_adopt(struct mz0380_dev *dev,
			 const struct v4l2_dv_timings *timings);
int mz0380_query_dv_timings(struct file *file, void *fh,
			    struct v4l2_dv_timings *timings);
int mz0380_g_dv_timings(struct file *file, void *fh,
			struct v4l2_dv_timings *timings);
int mz0380_s_dv_timings(struct file *file, void *fh,
			struct v4l2_dv_timings *timings);
int mz0380_enum_dv_timings(struct file *file, void *fh,
			   struct v4l2_enum_dv_timings *timings);
int mz0380_dv_timings_cap(struct file *file, void *fh,
			  struct v4l2_dv_timings_cap *cap);
void mz0380_signal_recovery_init(struct mz0380_dev *dev);
void mz0380_signal_recovery_start(struct mz0380_dev *dev);
void mz0380_signal_recovery_stop(struct mz0380_dev *dev);

/* ---- NO SIGNAL picture (mz0380-no-signal.c) ----------------------------- */

void mz0380_no_signal_init(struct mz0380_dev *dev);
void mz0380_no_signal_activate(struct mz0380_dev *dev, const char *reason);
void mz0380_no_signal_deactivate(struct mz0380_dev *dev);
void mz0380_no_signal_stop(struct mz0380_dev *dev);

/* ---- MST3367 HDMI receiver (mz0380-mst3367*.c) -------------------------- */

int mz0380_mst3367_bringup(struct mz0380_dev *dev);
int mz0380_mst3367_read_signal(struct mz0380_dev *dev,
			       struct v4l2_dv_timings *out);
int mz0380_mst3367_read_lock(struct mz0380_dev *dev, bool *locked,
			     bool rearm_acquisition);
void mz0380_mst3367_output_diag(struct mz0380_dev *dev, const char *tag);
void mz0380_mst3367_apply_csc_mode(struct mz0380_dev *dev);
int mz0380_mst3367_refresh_colourspace(struct mz0380_dev *dev);
int mz0380_mst3367_watch(struct mz0380_dev *dev, unsigned int secs);
int mz0380_mst3367_reload_edid(struct mz0380_dev *dev);
int mz0380_mst3367_hpd_pulse(struct mz0380_dev *dev, unsigned int count,
			     unsigned int gap_ms);

/* ---- ALSA (mz0380-audio.c) ---------------------------------------------- */

#if IS_ENABLED(CONFIG_SND)
int mz0380_audio_register(struct mz0380_dev *dev);
void mz0380_audio_unregister(struct mz0380_dev *dev);
void mz0380_audio_push(struct mz0380_dev *dev, const void *data, size_t len);
#else
static inline int mz0380_audio_register(struct mz0380_dev *dev) { return 0; }
static inline void mz0380_audio_unregister(struct mz0380_dev *dev) {}
static inline void mz0380_audio_push(struct mz0380_dev *dev, const void *data,
				     size_t len) {}
#endif

/*
 * M172: the reported field follows the negotiated timings. Whole frames are
 * delivered in one buffer, and for interlaced BT timings bt.height is already
 * the frame height, so V4L2_FIELD_INTERLACED keeps the geometry consistent.
 */
static inline u32 mz0380_current_field(const struct mz0380_dev *dev)
{
	return dev->set_timings.bt.interlaced ? V4L2_FIELD_INTERLACED
					      : V4L2_FIELD_NONE;
}

#endif
