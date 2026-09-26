/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Driver for MZ0380 based capture cards.
 *
 *  Copyright (c) 2026
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef _MZ0380_H_
#define _MZ0380_H_

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <linux/version.h>

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dv-timings.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>
#include "mz0380-reg.h"

#define MZ0380_VERSION_CODE KERNEL_VERSION(0, 1, 0)

#define MZ0380_MAXBOARDS 8
#define MZ0380_MAX_MAPS 2
#define UNSET (-1U)

#define MZ0380_BOARD_NOAUTO             UNSET
#define MZ0380_BOARD_UNKNOWN            0
#define MZ0380_BOARD_ELGATO_HD60_PRO    1
#define MZ0380_BOARD_ELGATO_HD60_PRO_REV3 2

#define MZ0380_INPUT_COUNT              5
#define MZ0380_INPUT_SELECT_PROPERTY    201
#define MZ0380_INPUT_SELECT_HW_REG      0x0040
#define MZ0380_INPUT_SELECT_HW_MASK     0x7
#define MZ0380_INPUT_SELECT_HW_SHIFT    0
#define MZ0380_BITRATE_PROPERTY         403
#define MZ0380_BITRATE_HW_REG           0x005c
#define MZ0380_BITRATE_HW_MASK          0xffffffff
#define MZ0380_BITRATE_HW_SHIFT         0
#define MZ0380_QUALITY_PROPERTY         404
#define MZ0380_QUALITY_HW_REG           0x0060
#define MZ0380_QUALITY_HW_MASK          0xffffffff
#define MZ0380_QUALITY_HW_SHIFT         0
#define MZ0380_GOP_PROPERTY             405
#define MZ0380_GOP_HW_REG               0x0080
#define MZ0380_GOP_HW_MASK              0xffffffff
#define MZ0380_GOP_HW_SHIFT             0
#define MZ0380_B_FRAMES_PROPERTY        411
#define MZ0380_B_FRAMES_HW_REG          0x0088
#define MZ0380_B_FRAMES_HW_MASK         0xffffffff
#define MZ0380_B_FRAMES_HW_SHIFT        0
#define MZ0380_DEFAULT_B_FRAMES         0
#define MZ0380_MAX_B_FRAMES             2
#define MZ0380_QP_STEP_PROPERTY         408
#define MZ0380_QP_STEP_HW_REG           0x0084
#define MZ0380_QP_STEP_HW_MASK          0xffffffff
#define MZ0380_QP_STEP_HW_SHIFT         0
#define MZ0380_DEFAULT_BITRATE          (12 * 1024 * 1024)
#define MZ0380_MIN_BITRATE              (256 * 1024)
#define MZ0380_MAX_BITRATE              (12 * 1024 * 1024)
/*
 * M171: the GOP range lived in mz0380-video.c, so mz0380-core.c - which does
 * the hardware readbacks that feed this control - could not see it and did not
 * range-check against it. That is exactly how a 0 got into a control declared
 * min=1.
 */
#define MZ0380_MIN_GOP                  1
#define MZ0380_DEFAULT_GOP              30
#define MZ0380_MAX_GOP                  300
/*
 * M245: the V4L2 encoder controls. Defaults are the main-stream values the
 * Windows 1.1.195.0 driver sends and the H.264 path was validated with, so an
 * application that never touches the controls gets exactly what it got before.
 * SET_ENC_PARAMS carries the GOP in one byte.
 */
#define MZ0380_ENC_DEFAULT_BITRATE      4194304
#define MZ0380_ENC_DEFAULT_GOP          32
#define MZ0380_ENC_MAX_GOP              255
#define MZ0380_MIN_QUALITY              0
#define MZ0380_DEFAULT_QUALITY          80
#define MZ0380_MAX_QUALITY              100
#define MZ0380_DEFAULT_QP_STEP          0
#define MZ0380_RECORD_MODE_COUNT        3
#define MZ0380_RECORD_MODE_PROPERTY     407
#define MZ0380_RECORD_MODE_HW_REG       0x0058
#define MZ0380_RECORD_MODE_HW_MASK      0x3
#define MZ0380_RECORD_MODE_HW_SHIFT     0
#define MZ0380_CFG_TRACE_START          0x0000
/*
 * M0 observability widen: extend the before/after CFG trace past the old
 * 0x00bc ceiling to cover the hypothesised command/status/param mailbox
 * (0x00c0..0x00e4), so /proc/mz0380-experiment diffs can see whether a
 * mailbox write actually lands. BAR5 is always mapped, so this stays a
 * read-only, cannot-fault extension.
 */
#define MZ0380_CFG_TRACE_END            0x00e4
#define MZ0380_CFG_TRACE_COUNT \
	(((MZ0380_CFG_TRACE_END - MZ0380_CFG_TRACE_START) / sizeof(u32)) + 1)

struct snd_card; /* mz0380-audio.c */
struct mz0380_dev;

enum mz0380_property_experiment_result {
	MZ0380_PROPERTY_EXPERIMENT_NONE = 0,
	MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_DISABLED,
	MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_UNCONFIGURED,
	MZ0380_PROPERTY_EXPERIMENT_INTENT_ONLY_INVALID_CONFIG,
	MZ0380_PROPERTY_EXPERIMENT_CACHED_ONLY,
	MZ0380_PROPERTY_EXPERIMENT_WRITE_APPLIED,
	MZ0380_PROPERTY_EXPERIMENT_WRITE_READBACK_MISMATCH,
};

struct mz0380_board {
	const char *name;
	const char *windows_driver;
};

/*
 * Card bring-up state machine.
 *
 * The card boots its onboard ARM Linux from its OWN FLASH - the host never
 * sends an image. REQUESTED means the handshake is in flight. Bus mastering
 * must stay disabled until READY, otherwise an AMD-Vi IO_PAGE_FAULT was
 * observed at 0x90000000.
 */
enum mz0380_fw_state {
	MZ0380_FW_STATE_NONE = 0,
	MZ0380_FW_STATE_REQUESTED,
	MZ0380_FW_STATE_READY,
	MZ0380_FW_STATE_FAILED,
};

/*
 * Per-ring DMA bookkeeping. Video and audio each get one of these.
 *
 * The card writes encoded H.264 NALs (video) or PCM samples (audio)
 * into the page-aligned `buf` allocation, structured as `nr_entries`
 * fixed-size slots of `entry_size` bytes each. Card advances its tail;
 * host advances head as it consumes.
 */
struct mz0380_ring {
	void *buf;              /* kernel virt addr                       */
	dma_addr_t dma;         /* DMA / bus addr                         */
	size_t total_size;
	u32 entry_size;
	u32 nr_entries;
	u32 head;               /* host consumer index                    */
	u32 tail_seen;          /* last tail value drained from card      */
	u32 base_reg_lo;        /* BAR5 offset for ring-base-low          */
	u32 base_reg_hi;
	u32 size_reg;
	u32 entries_reg;
	u32 head_reg;
	u32 tail_reg;
};

/*
 * vb2 buffer wrapper. The encoded payload from one or more ring slots
 * is copied / scatter-gathered into the vb2_buffer-backed memory and
 * handed to userspace via DQBUF.
 */
struct mz0380_vb_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct mz0380_subid {
	u16 subvendor;
	u16 subdevice;
	u32 card;
};

/* Shared only by PCI lifecycle and procfs device enumeration. */
extern bool allow_bus_master;
extern struct mutex devlist;
extern struct list_head mz0380_devlist;
int mz0380_proc_create(void);
void mz0380_proc_remove(void);
void mz0380_event_watch_stop(struct mz0380_dev *dev);

struct mz0380_capture_state {
	/*
	 * M168: no cached pixelformat. poll_drain_ms and stream_nosg are both
	 * 0644 module params, so the advertised fourcc can change under a
	 * running driver; a copy here would go stale the moment it did.
	 * mz0380_current_pixelformat() is the only answer.
	 */
	u32 width;
	u32 height;
	struct v4l2_fract timeperframe;
	/* Receiver timing, kept separate from the encoder's output geometry. */
	u32 source_width;
	u32 source_height;
	u32 source_fps;
	bool source_interlaced;
	u32 input;
	u32 record_mode;
	/*
	 * BAR5 property-experiment mirrors (RE instruments driven from
	 * /proc/mz0380-control). The encoder never reads these.
	 */
	u32 bitrate;
	u32 quality;
	u32 gop_size;
	u32 qp_step;
	u32 b_frames;
	/*
	 * M245: what SET_ENC_PARAMS actually sends for the main stream, set by
	 * the V4L2 bitrate/GOP controls. Kept apart from the BAR5 mirrors above
	 * so a probe-time readback of an unverified register cannot reach the
	 * encoder.
	 */
	u32 enc_bitrate;
	u32 enc_gop;
};

struct mz0380_mode {
	u32 width;
	u32 height;
};

struct mz0380_property_experiment {
	bool valid;
	bool hardware_write;
	u32 property_id;
	u32 previous_value;
	u32 requested_value;
	u32 reg;
	u32 mask;
	u32 shift;
	u32 before_word;
	u32 programmed_word;
	u32 readback_word;
	enum mz0380_property_experiment_result result;
	char source[16];
	u32 before_cfg_trace[MZ0380_CFG_TRACE_COUNT];
	u32 after_cfg_trace[MZ0380_CFG_TRACE_COUNT];
};

/*
 * Live card-event watcher (diagnostic). A kthread samples the BAR0 EVENT word
 * at high rate and records each edge - with the payload words the card writes
 * at BAR0+0x40..0x4c - into a ring exposed at /proc/mz0380-events. This is how
 * signal-change / no-signal notifications are caught, since the card pushes
 * them as edge events rather than exposing a pollable status register.
 */
struct mz0380_event_rec {
	u64 t_ns;		/* local_clock() timestamp                */
	u32 event;		/* BAR0 EVENT word (0x30)                 */
	u32 payload[4];		/* BAR0 0x40..0x4c payload/DPC-arg words  */
	u32 status;		/* BAR0 STATUS (0x2c)                     */
	u32 intflag;		/* BAR5 CFG interrupt flag                */
};

#define MZ0380_EVENT_RING_SIZE 256

/*
 * Frame-completion mailboxes are one-shot: EVENT acknowledgement lets the
 * endpoint overwrite TOKEN/PAYLOAD immediately.  Keep the complete pre-ACK
 * image in a bounded IRQ-safe FIFO and let process context consume it.
 */
struct mz0380_frame_event {
	u64 timestamp_ns;
	u32 event;
	u32 token;
	u32 payload[3];
	u32 enc_status;
};

#define MZ0380_FRAME_EVENT_FIFO_SIZE 64
#define MZ0380_H264_PARAMETER_SETS_MAX 4096

#ifdef MZ0380_HAVE_SYSTEM_DFL_WQ
#define MZ0380_SYSTEM_WQ system_dfl_wq
#else
#define MZ0380_SYSTEM_WQ system_wq
#endif

/*
 * How the fill probes sample a slot: 32 dwords spread over the last 64 luma
 * rows (M229) or over the whole chroma region (M238). In the header because
 * /proc prints the sample count next to the M244 counters, so the number the
 * operator reads and the number the test uses are the same symbol.
 */
#define MZ0380_RAW_FILL_SAMPLES  32
#define MZ0380_RAW_FILL_ROWS     64

struct mz0380_dev {
	struct list_head devlist;
	struct pci_dev *pci;
	struct mutex lock;
	struct mutex ctrl_lock;
	char name[32];
	unsigned int nr;
	unsigned int board;
	void __iomem *lmmio[MZ0380_MAX_MAPS];
	u8 __iomem *bmmio[MZ0380_MAX_MAPS];
	int bar_nr[MZ0380_MAX_MAPS];
	resource_size_t bar_start[MZ0380_MAX_MAPS];
	resource_size_t bar_len[MZ0380_MAX_MAPS];
	u8 pci_rev;
	u8 pci_lat;
	struct v4l2_device v4l2_dev;
	struct v4l2_ctrl_handler ctrl_handler;
	struct video_device vdev;
	struct v4l2_ctrl *bitrate_ctrl;
	struct v4l2_ctrl *gop_ctrl;
	struct mz0380_capture_state capture;

	/* Firmware */
	enum mz0380_fw_state fw_state;
	/* M245: post-resume handshake, run off the resume path. */
	struct work_struct resume_work;
	struct mutex fw_lock;
	u16 fw_version_major;
	u16 fw_version_minor;
	wait_queue_head_t fw_wait;

	/* IRQ */
	int irq;
	bool irq_requested;
	bool msi_enabled;
	atomic_t irq_count;
	atomic_t irq_video_count;
	atomic_t irq_audio_count;
	atomic_t irq_signal_count;

	/* Command channel serialisation */
	struct mutex cmd_lock;
	wait_queue_head_t cmd_wait;
	u32 cmd_last_status;
	u32 cmd_last_param[MZ0380_REG_PARAM_MAX];
	bool cmd_complete;

	/* DMA rings */
	struct mz0380_ring video_ring;
	struct mz0380_ring audio_ring;
	bool dma_armed;

	// pattern-check: skip preloaded DMA buffer array, plain state, no abstraction
	/*
	 * Streaming buffer set (RE_FINDINGS.md M17). The card DMAs encoded
	 * frames into these host buffers via its iATU outbound window; we hand
	 * their physaddrs over with the buffer-setter mailbox opcodes. The
	 * completion EVENT carries the finished buffer index in (token & 7).
	 *
	 * M26: the card can only be told the HIGH 32 bits of that address
	 * (ELBI 0x54, the low half, is ignored by the hardware - proven on hw),
	 * so each buffer is placed at its own 4 GiB-aligned IOVA by an explicit
	 * iommu_map. @pages is the backing allocation for that path (NULL when
	 * the buffer came from dma_alloc_coherent instead).
	 */
	struct mz0380_stream_buf {
		void *va;
		dma_addr_t dma;
		struct page *pages;
		/*
		 * M168: frames delivered out of this buffer this stream. The
		 * drain re-poisons immediately after copying, so the extent
		 * report at stream stop scans a buffer that is poison again and
		 * says "0/1024 sampled pages touched" - identical to the card
		 * having written nothing. That reading is the whole diagnosis
		 * in several places in RE_FINDINGS, so the successful case must
		 * not be able to imitate the empty one.
		 */
		u32 delivered;
	} stream_bufs[MZ0380_STREAM_NR_BUFS];
	u32 stream_head;	/* next buffer index we expect from the card */

	/*
	 * Opt-in H.264 diagnostic bank. tinyvenc7 sends raw preview DMA through
	 * outbound window 0 (stream_bufs) and encoded output through window 1.
	 * These must not alias: the two producers rotate independently.
	 */
	struct mz0380_stream_buf h264_bufs[MZ0380_STREAM_NR_BUFS];
	/* Opt-in Windows-parity window-0 banks: op 0x02 then independent op 0x08. */
	struct mz0380_raw_probe_buf {
		void *va;
		dma_addr_t dma;
		struct page **pages;
		u32 nr_pages;
		size_t mapped;
		size_t last_extent;
		u32 completions;
		u32 delivered;
		/* M212: last sampled head, for the "are these pixels" control. */
		u8 observed_head[16];
		bool observed_valid;
		u64 observed_changes;
		/*
		 * M219: which frame this slot last handed to userspace. The
		 * sentinel test says a whole frame is PRESENT, not that it is
		 * NEW, and encoded completions outrun the raw DMA at full rate.
		 */
		u32 last_token;
		bool last_token_valid;
		u8 last_head[16];
	} raw_probe_bufs[MZ0380_RAW_PROBE_NR_BUFS];
	u64 raw_probe_events;
	/*
	 * M218: which format the node is currently delivering, and whether raw
	 * is available at all. raw_capable is set when the raw banks were
	 * allocated (a probe-time decision, since they come from
	 * mz0380_dma_setup); deliver_raw is the runtime choice S_FMT makes
	 * between the two, seeded from the raw_deliver module parameter.
	 */
	bool raw_capable;
	bool deliver_raw;
	/* M241: which raw layout S_FMT selected - I420, YV12 or NV12. */
	u32 raw_fourcc;
	/* M217: raw delivery to V4L2. */
	u64 raw_probe_stub_frames;
	u64 raw_dup_token;
	u64 raw_dup_content;
	u64 raw_frames_torn;
	u64 raw_multi_landed;
	u32 raw_next_slot;
	u64 raw_frames_delivered;
	u64 raw_frames_dropped;
	u64 raw_probe_full_frames;
	u64 raw_probe_bad_extents;
	u32 raw_probe_consecutive_full;
	u8 raw_probe_slots_seen;
	bool raw_probe_success_reported;
	u8 h264_last_token;
	bool h264_last_token_valid;
	u64 h264_frames_delivered;
	u64 h264_frames_dropped;
	u64 h264_frames_discarded;
	u64 h264_frames_suppressed;
	u8 *h264_parameter_sets;
	u32 h264_parameter_sets_len;
	/* Host-owned H.264 placeholder used while the HDMI receiver is unlocked. */
	struct delayed_work no_signal_work;
	struct mutex h264_delivery_lock;
	u64 no_signal_frames_delivered;
	u64 no_signal_frames_missed;
	/* M225: placeholder ticks withheld because the node is delivering raw. */
	u64 no_signal_frames_suppressed;
	/* M229: slots whose last luma rows were still the card's clear value. */
	u64 raw_incomplete_tail;
	/* M238: slots whose luma was complete but whose CHROMA was not. */
	u64 raw_incomplete_chroma;
	/*
	 * M244: what raw_incomplete_chroma == 0 actually means.
	 *
	 * A rejection counter that never moves cannot tell "wired and never
	 * needed" apart from "never evaluated", which is exactly where M238
	 * sat after its first hardware session. These two say which: probes
	 * counts every evaluation of the chroma test, and max_clear records
	 * the most sample dwords that ever matched the clear value in one
	 * slot. Probes climbing with max_clear short of MZ0380_RAW_FILL_SAMPLES
	 * is a live test that simply never had cause to fire.
	 */
	u64 raw_chroma_probes;
	u32 raw_chroma_max_clear;
	/* M233: AUTO_POSITION re-arms issued, i.e. real acquisition losses. */
	u64 mst_rearms;
	/* M237: last CSC byte written to BANK0 0x92, and whether one ever was. */
	u8 mst_csc_applied;
	bool mst_csc_applied_valid;
	bool no_signal_active;
	/*
	 * The receiver monitor is independent of encoder activity. This matters
	 * when firmware keeps producing a fallback picture: frame activity alone
	 * cannot say whether a real HDMI source is present.
	 */
	struct delayed_work signal_recovery_work;
	unsigned long last_h264_frame_stamp;
	u32 signal_recovery_attempts;
	bool signal_recovering;
	bool h264_waiting_for_idr;

	// pattern-check: skip plain diagnostic fields on an existing struct
	/*
	 * M36 write-extent watch. The buffers are poisoned with 0xAA at stream
	 * start so that card-written ZEROS become visible (the fake frame's
	 * trailing padding is zeros, which the old non-zero sampling could not
	 * see). A kthread then tracks, per buffer, the byte offset up to which
	 * the poison has been overwritten - giving the true transfer extent
	 * and its progress over time (crawl vs stall, exact stop offset).
	 */
	struct task_struct *extent_task;
	size_t extent_last[MZ0380_STREAM_NR_BUFS];

	/* vb2 video streaming */
	struct vb2_queue vb_queue;
	struct list_head buf_list;
	spinlock_t buf_lock;
	struct work_struct drain_work;
	struct workqueue_struct *drain_wq;	/* M245 */
	spinlock_t frame_event_lock;
	struct mz0380_frame_event
		frame_events[MZ0380_FRAME_EVENT_FIFO_SIZE];
	u16 frame_event_head;
	u16 frame_event_tail;
	bool frame_event_drain_scheduled;
	bool frame_events_accepting;
	bool frame_event_ack_deferred;
	u8 frame_event_drop_tokens;
	u64 frame_event_drops;
	u32 video_sequence;
	/* M155: stream cycles since insmod, for stream_setvic_once. */
	u32 stream_cycles;

	/*
	 * M169: encoder spawns since insmod - every SET_VIC this driver has
	 * fired, which is the ONLY thing that forks a fresh tinyvenc5.
	 *
	 * Distinct from stream_cycles, which counts cycles whether or not
	 * SET_VIC was actually sent (setvic_once skips it) and so is not a
	 * spawn count. The card wedges for good somewhere in the 8-18 spawn
	 * range and only removing slot power brings it back, so this is the
	 * number that says how close the card is to the cliff.
	 */
	u32 encoder_spawns;
	u8 frame_poison_byte;
	bool frame_poison_active;
	/* Card-side VIC/tinyvenc lifetime, independent of a VB2 consumer. */
	bool pipeline_running;
	bool pipeline_reconfigure_pending;
	u32 pipeline_width;
	u32 pipeline_height;
	u32 pipeline_source_width;
	u32 pipeline_source_height;
	u32 pipeline_source_fps;
	bool pipeline_source_interlaced;
	u32 pipeline_attach_count;
	bool pipeline_start_failed;
	bool streaming;

	// pattern-check: skip two plain fields on the existing device struct
	/*
	 * Fake-frame (stream_nosg) polling capture. No completion IRQ ever
	 * arrives on this path (M41: card-internal), so a kthread poisons buf0,
	 * spawns the encoder, polls for the frame's contiguous burst to land,
	 * delivers it as NV12 and respawns (M39: one frame per fresh spawn).
	 */
	struct task_struct *nosg_task;
	struct task_struct *poll_task;
	u32 nosg_sequence;
	u32 nosg_spawns;            /* encoder spawns this session (wedge budget) */
	bool aic_armed;             /* SET_AIC(on=1) already sent this session    */

	/* HDMI signal */
	// pattern-check: skip adding one bool state flag to existing struct
	struct v4l2_dv_timings detected_timings;
	/*
	 * M171: what S_DV_TIMINGS was last told, which V4L2 keeps distinct from
	 * what is actually on the wire. G_DV_TIMINGS returns this;
	 * QUERY_DV_TIMINGS returns detected_timings. Conflating them failed
	 * v4l2-compliance - it sets a timing from ENUM_DV_TIMINGS and requires
	 * G to return it, and G was answering with the live detection instead.
	 */
	struct v4l2_dv_timings set_timings;
	/* M245: an application called S_DV_TIMINGS; detection must not clobber it. */
	bool timings_set_by_user;
	/*
	 * M65: the last detection that actually succeeded, kept across later
	 * failures. A source that transmits in short bursts cannot be locked
	 * at STREAMON time, but the geometry it showed us is still the right
	 * thing to arm the encoder with.
	 */
	struct v4l2_dv_timings last_good_timings;
	unsigned long last_good_stamp;
	bool have_last_good;
	bool signal_locked;
	bool mst3367_ready;	/* receiver reset released + init applied */
	/*
	 * M133: last BANK2 0x48 the output diag read successfully. The CSC mode
	 * is chosen from bits 6:5 of this, and a standalone read of it is not
	 * reliable - see mz0380_mst3367_apply_csc_mode(). hdcapm caches the same
	 * register (regb2r48_cached) and now we know why.
	 */
	u8 mst_b2_48;
	bool mst_b2_48_valid;

	/* ALSA */
	struct snd_card *snd_card;
	void *snd_pcm; /* opaque to avoid pulling snd headers in this header */
	bool audio_registered;
	struct mz0380_property_experiment input_experiment;
	struct mz0380_property_experiment bitrate_experiment;
	struct mz0380_property_experiment quality_experiment;
	struct mz0380_property_experiment gop_experiment;
	struct mz0380_property_experiment b_frames_experiment;
	struct mz0380_property_experiment qp_step_experiment;
	struct mz0380_property_experiment record_mode_experiment;
	bool v4l2_registered;
	bool ctrl_handler_initialized;
	bool video_registered;

	/* Live event watcher */
	struct mz0380_event_rec event_ring[MZ0380_EVENT_RING_SIZE];
	unsigned int event_head;
	unsigned int event_count;
	u64 event_seen;
	spinlock_t event_lock;
	struct task_struct *event_kthread;
	bool event_watching;
};

extern const struct mz0380_board mz0380_boards[];
extern const unsigned int mz0380_bcount;
extern const struct mz0380_subid mz0380_subids[];
extern const unsigned int mz0380_idcount;
extern bool mz0380_enable_video;

void mz0380_card_list(struct mz0380_dev *dev);
int mz0380_card_setup(struct mz0380_dev *dev);
void mz0380_card_cleanup(struct mz0380_dev *dev);

int mz0380_video_register(struct mz0380_dev *dev);
void mz0380_video_unregister(struct mz0380_dev *dev);
void mz0380_dev_put(struct mz0380_dev *dev);
void mz0380_encoder_ctrls_grab(struct mz0380_dev *dev, bool grabbed);
u32 mz0380_sizeimage_for(struct mz0380_dev *dev, bool raw, u32 width,
			 u32 height);
void mz0380_video_state_dump(struct seq_file *m, struct mz0380_dev *dev);
void mz0380_capture_state_init(struct mz0380_dev *dev);
const char *mz0380_input_name(u32 input);
const char *mz0380_record_mode_name(u32 mode);
bool mz0380_read_hw_input_select(struct mz0380_dev *dev, u32 *input,
				 u32 *raw_word);
bool mz0380_sync_hw_input_select(struct mz0380_dev *dev, const char *reason);
int mz0380_request_input_select(struct mz0380_dev *dev, u32 input,
			       const char *source);
bool mz0380_read_hw_bitrate(struct mz0380_dev *dev, u32 *bitrate,
			    u32 *raw_word, u32 *reg,
			    u32 *mask, u32 *shift);
bool mz0380_sync_hw_bitrate(struct mz0380_dev *dev, const char *reason);
bool mz0380_read_candidate_bitrate(struct mz0380_dev *dev, u32 *bitrate,
				   u32 *raw_word, u32 *reg,
				   u32 *mask, u32 *shift);
bool mz0380_sync_candidate_bitrate(struct mz0380_dev *dev,
				   const char *reason);
int mz0380_request_bitrate_locked(struct mz0380_dev *dev, u32 bitrate,
				  const char *source);
int mz0380_request_bitrate(struct mz0380_dev *dev, u32 bitrate,
			   const char *source);
bool mz0380_read_hw_quality(struct mz0380_dev *dev, u32 *quality,
			    u32 *raw_word, u32 *reg,
			    u32 *mask, u32 *shift);
bool mz0380_sync_hw_quality(struct mz0380_dev *dev, const char *reason);
bool mz0380_read_candidate_quality(struct mz0380_dev *dev, u32 *quality,
				   u32 *raw_word, u32 *reg,
				   u32 *mask, u32 *shift);
bool mz0380_sync_candidate_quality(struct mz0380_dev *dev,
				   const char *reason);
int mz0380_request_quality_locked(struct mz0380_dev *dev, u32 quality,
				  const char *source);
int mz0380_request_quality(struct mz0380_dev *dev, u32 quality,
			   const char *source);
bool mz0380_read_candidate_gop(struct mz0380_dev *dev, u32 *gop,
			       u32 *raw_word, u32 *reg,
			       u32 *mask, u32 *shift);
bool mz0380_read_hw_gop(struct mz0380_dev *dev, u32 *gop,
			u32 *raw_word, u32 *reg,
			u32 *mask, u32 *shift);
bool mz0380_sync_hw_gop(struct mz0380_dev *dev, const char *reason);
bool mz0380_sync_candidate_gop(struct mz0380_dev *dev,
			       const char *reason);
int mz0380_request_gop_locked(struct mz0380_dev *dev, u32 gop,
			      const char *source);
int mz0380_request_gop(struct mz0380_dev *dev, u32 gop,
		       const char *source);
bool mz0380_read_hw_b_frames(struct mz0380_dev *dev, u32 *b_frames,
			     u32 *raw_word, u32 *reg,
			     u32 *mask, u32 *shift);
bool mz0380_read_candidate_b_frames(struct mz0380_dev *dev, u32 *b_frames,
				    u32 *raw_word, u32 *reg,
				    u32 *mask, u32 *shift);
bool mz0380_sync_hw_b_frames(struct mz0380_dev *dev,
			     const char *reason);
bool mz0380_sync_candidate_b_frames(struct mz0380_dev *dev,
				    const char *reason);
int mz0380_request_b_frames_locked(struct mz0380_dev *dev, u32 b_frames,
				   const char *source);
int mz0380_request_b_frames(struct mz0380_dev *dev, u32 b_frames,
			    const char *source);
bool mz0380_read_hw_qp_step(struct mz0380_dev *dev, u32 *qp_step,
			    u32 *raw_word, u32 *reg,
			    u32 *mask, u32 *shift);
bool mz0380_read_candidate_qp_step(struct mz0380_dev *dev, u32 *qp_step,
				   u32 *raw_word, u32 *reg,
				   u32 *mask, u32 *shift);
bool mz0380_sync_hw_qp_step(struct mz0380_dev *dev, const char *reason);
bool mz0380_sync_candidate_qp_step(struct mz0380_dev *dev,
				   const char *reason);
int mz0380_request_qp_step_locked(struct mz0380_dev *dev, u32 qp_step,
				  const char *source);
int mz0380_request_qp_step(struct mz0380_dev *dev, u32 qp_step,
			   const char *source);
bool mz0380_read_hw_record_mode(struct mz0380_dev *dev, u32 *mode,
				u32 *raw_word, u32 *reg,
				u32 *mask, u32 *shift);
bool mz0380_sync_hw_record_mode(struct mz0380_dev *dev,
				const char *reason);
bool mz0380_read_candidate_record_mode(struct mz0380_dev *dev, u32 *mode,
				       u32 *raw_word, u32 *reg,
				       u32 *mask, u32 *shift);
bool mz0380_sync_candidate_record_mode(struct mz0380_dev *dev,
				       const char *reason);
int mz0380_request_record_mode_locked(struct mz0380_dev *dev, u32 mode,
				      const char *source);
int mz0380_request_record_mode(struct mz0380_dev *dev, u32 mode,
			       const char *source);

u32 mz_read(struct mz0380_dev *dev, int map, u32 reg);
void mz_write(struct mz0380_dev *dev, int map, u32 reg, u32 value);

/* Convenience BAR5 accessors */
static inline u32 mz_cfg_read(struct mz0380_dev *dev, u32 reg)
{
	return mz_read(dev, MZ0380_MAP_BAR_CFG, reg);
}
static inline void mz_cfg_write(struct mz0380_dev *dev, u32 reg, u32 v)
{
	mz_write(dev, MZ0380_MAP_BAR_CFG, reg, v);
}

/* Convenience BAR0 accessors (post-firmware) */
static inline u32 mz_mmio_read(struct mz0380_dev *dev, u32 reg)
{
	return mz_read(dev, MZ0380_MAP_BAR_MMIO, reg);
}
static inline void mz_mmio_write(struct mz0380_dev *dev, u32 reg, u32 v)
{
	mz_write(dev, MZ0380_MAP_BAR_MMIO, reg, v);
}
static inline void mz_mmio_set(struct mz0380_dev *dev, u32 reg, u32 bits)
{
	mz_mmio_write(dev, reg, mz_mmio_read(dev, reg) | bits);
}
static inline void mz_mmio_clr(struct mz0380_dev *dev, u32 reg, u32 bits)
{
	mz_mmio_write(dev, reg, mz_mmio_read(dev, reg) & ~bits);
}

/* Command/mailbox helpers (mz0380-core.c) */
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

/* Firmware (mz0380-fw.c) */
int mz0380_firmware_load(struct mz0380_dev *dev);
void mz0380_firmware_release(struct mz0380_dev *dev);
int mz0380_card_init(struct mz0380_dev *dev);
void mz0380_mailbox_scan(struct mz0380_dev *dev);
const char *mz0380_fw_state_name(enum mz0380_fw_state s);
void mz0380_fw_info_dump(struct seq_file *m, struct mz0380_dev *dev);

/* DMA + IRQ (mz0380-dma.c) */
int mz0380_irq_request(struct mz0380_dev *dev);
void mz0380_irq_release(struct mz0380_dev *dev);
int mz0380_dma_setup(struct mz0380_dev *dev);
void mz0380_dma_teardown(struct mz0380_dev *dev);
int mz0380_dma_start(struct mz0380_dev *dev);
void mz0380_dma_stop(struct mz0380_dev *dev);
void mz0380_dma_quiesce(struct mz0380_dev *dev);
/*
 * Non-sleeping pre-ACK hook.  mz0380_mb_ack_event() must call this with the
 * live EVENT value before clearing EVENT, so TOKEN/PAYLOAD cannot be lost when
 * the command poller (rather than the MSI ISR) consumes a combined event.
 */
void mz0380_handle_event_snapshot(struct mz0380_dev *dev, u32 event);
void mz0380_dma_flush_events(struct mz0380_dev *dev);
int mz0380_nosg_capture_start(struct mz0380_dev *dev);
void mz0380_nosg_capture_stop(struct mz0380_dev *dev);
void mz0380_extent_repoison(struct mz0380_dev *dev);	/* M38 */
int mz0380_dma_ring_alloc(struct mz0380_dev *dev, struct mz0380_ring *r,
			  u32 nr_entries, u32 entry_size);
void mz0380_dma_ring_free(struct mz0380_dev *dev, struct mz0380_ring *r);
void mz0380_dma_drain_video(struct mz0380_dev *dev);

/* HDMI signal detect and DV timings (mz0380-signal.c) */
extern const struct v4l2_dv_timings mz0380_no_signal;
const struct mz0380_mode *mz0380_find_mode(u32 width, u32 height);
const struct v4l2_fract *mz0380_find_interval(
	u32 width, u32 height, const struct v4l2_fract *wanted);
u32 mz0380_source_fps(const struct v4l2_dv_timings *timings);
int mz0380_query_signal(struct mz0380_dev *dev,
			struct v4l2_dv_timings *timings);
void mz0380_signal_event(struct mz0380_dev *dev);
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

// pattern-check: skip two function prototypes, procedural module, no abstraction
/* MST3367 receiver bring-up + signal detect (mz0380-mst3367.c) */
int mz0380_mst3367_bringup(struct mz0380_dev *dev);
void mz0380_mst3367_diag(struct mz0380_dev *dev, struct seq_file *m);
void mz0380_mst3367_output_diag(struct mz0380_dev *dev, const char *tag);
void mz0380_mst3367_apply_csc_mode(struct mz0380_dev *dev);
int mz0380_mst3367_refresh_colourspace(struct mz0380_dev *dev);
int mz0380_mst3367_ramtest(struct mz0380_dev *dev);	/* M44 */
int mz0380_mst3367_watch(struct mz0380_dev *dev, unsigned int secs);	/* M45 */
int mz0380_mst3367_reload_edid(struct mz0380_dev *dev);	/* M47 */
int mz0380_mst3367_wscan(struct mz0380_dev *dev);	/* M49 */
int mz0380_mst3367_hpd_pulse(struct mz0380_dev *dev, unsigned int count,
			     unsigned int gap_ms);	/* M48 */
int mz0380_mst3367_read_signal(struct mz0380_dev *dev,
			       struct v4l2_dv_timings *out);
int mz0380_mst3367_read_lock(struct mz0380_dev *dev, bool *locked,
			     bool rearm_acquisition);
void mz0380_signal_recovery_init(struct mz0380_dev *dev);
void mz0380_signal_recovery_start(struct mz0380_dev *dev);
void mz0380_signal_recovery_stop(struct mz0380_dev *dev);
void mz0380_no_signal_init(struct mz0380_dev *dev);
void mz0380_no_signal_activate(struct mz0380_dev *dev, const char *reason);
void mz0380_no_signal_deactivate(struct mz0380_dev *dev);
void mz0380_no_signal_stop(struct mz0380_dev *dev);
int mz0380_gpio_dump(struct mz0380_dev *dev);			/* M51b */
int mz0380_mst3367_edidhunt(struct mz0380_dev *dev);		/* M53  */
int mz0380_i2cbb_scan(struct mz0380_dev *dev, u8 sda, u8 scl);	/* M51 */

/* ALSA audio (mz0380-audio.c) */
#if IS_ENABLED(CONFIG_SND)
int mz0380_audio_register(struct mz0380_dev *dev);
void mz0380_audio_unregister(struct mz0380_dev *dev);
#else
static inline int mz0380_audio_register(struct mz0380_dev *dev) { return 0; }
static inline void mz0380_audio_unregister(struct mz0380_dev *dev) {}
#endif

/*
 * "derive byte7 from the detected scan" sentinel for vic_in_fmt. Needed so
 * vic_in_fmt=0 is expressible - see the comment on the parameter in
 * mz0380-core.c and M103.
 */
#define MZ0380_VIC_IN_FMT_AUTO		(~0u)

/*
 * "do not touch pin 8 at all" sentinel for rx_strap. 0 and 1 are both real
 * levels, so the leave-alone case needs a third value - see M109.
 */
#define MZ0380_RX_STRAP_LEAVE		(~0u)

/* Module-param gates */
extern bool mz0380_stream_without_signal;
extern unsigned int mz0380_op6_kick_ms;
extern unsigned int mz0380_kick_opcode;
extern bool mz0380_kick_repeat;
extern bool mz0380_poll_drain_credit;
extern unsigned int mz0380_poll_drain_ms;
extern unsigned int mz0380_stall_eos_ms;
extern bool mz0380_strict_geometry;
extern unsigned int mz0380_expect_frame_bytes;
void mz0380_credit_rearm(struct mz0380_dev *dev);
extern unsigned int mz0380_rx_strap;
extern bool mz0380_enable_dma;
extern unsigned int mz0380_start_delay_ms;
extern bool mz0380_stream_nosg;
extern unsigned int mz0380_nosg_frame_timeout_ms;
extern bool mz0380_buf_pair_swap;
extern bool mz0380_dma_iova_remap;
extern unsigned long long mz0380_dma_iova_base;
extern unsigned long long mz0380_dma_iova_offset;
extern unsigned int mz0380_set_buf_stride;
extern unsigned int mz0380_card_frame_offset;
extern bool mz0380_probe_windows;
extern bool mz0380_h264_probe;
extern bool mz0380_gpio_dir_invert;
extern unsigned int mz0380_signal_poll_ms;
extern unsigned int mz0380_card_ready_timeout_ms;
extern bool mz0380_force_timings;
extern unsigned int mz0380_edidhunt_max_regs;
extern unsigned int mz0380_edid_opcode;
extern unsigned int mz0380_edid_timeout_ms;
extern bool mz0380_buf_poison;
extern bool mz0380_event_require_complete;
/* Also declared in mz0380-internal.h; needed here for mz0380-fw.c (M236). */
extern unsigned int procfs_verbosity;
extern unsigned int mz0380_poison_byte;
extern unsigned int mz0380_raw_clear_byte;
extern unsigned int mz0380_raw_clear_chroma;
extern bool mz0380_raw_full_range;
extern bool mz0380_aic_on;
extern bool mz0380_aic_every_frame;
extern bool mz0380_signal_confirm;
extern unsigned int mz0380_vic_fw;
extern unsigned int mz0380_vic_out_format;
extern unsigned int mz0380_cardlog_bytes;
extern bool mz0380_cardlog_probe_enabled;
extern unsigned int mz0380_vic_in_w;
extern unsigned int mz0380_vic_in_h;
extern unsigned int mz0380_vic_in_fmt;
extern unsigned int mz0380_vic_saturation;
extern unsigned int mz0380_vic_b0;
extern unsigned int mz0380_mst_b1;
extern unsigned int mz0380_mst_b2;
/* M130: mz0380_mst_csc_ctl - AUTO picks the mode from BANK2 0x48. */
#define MZ0380_MST_CSC_CTL_AUTO		0xffffffffu
#define MZ0380_MST_CSC_CTL_HDCAPM	0x40
extern unsigned int mz0380_mst_csc_ctl;
extern unsigned int mz0380_mst_b5;
extern bool mz0380_mst_b0_late;
extern unsigned int mz0380_set_buf_opcode;
extern unsigned int mz0380_signal_cache_ms;
extern unsigned int mz0380_signal_query_cache_ms;
extern bool mz0380_hotplug_recovery;
extern unsigned int mz0380_hotplug_stall_ms;
extern unsigned int mz0380_hotplug_retry_ms;
extern unsigned int mz0380_signal_monitor_ms;
extern unsigned int mz0380_no_signal_fps;
extern unsigned int mz0380_aic_channels;
extern unsigned int mz0380_aic_bits;
extern unsigned int mz0380_aic_freq;
extern unsigned int mz0380_aic_period_frames;
extern unsigned int mz0380_aic_periods;
extern unsigned int mz0380_aic_int_mode;
/*
 * M82: no legal saturation value can exceed 255, so a sentinel above the byte
 * range means "leave vic_color_info untouched".
 */
#define MZ0380_VIC_SATURATION_UNSET	0xffffffffu
extern unsigned int mz0380_vic_color_info;
extern unsigned int mz0380_vic_fast_kill;
extern unsigned int mz0380_vic_int_mode;
extern unsigned int mz0380_token_seed;
extern unsigned int mz0380_vic_out_w;
extern unsigned int mz0380_vic_out_h;
extern unsigned int mz0380_vic_nosg;
extern bool mz0380_win_seq;
extern bool mz0380_win_start_op6;
extern bool mz0380_win_bufs_first;
extern unsigned int mz0380_stop_settle_ms;
extern bool mz0380_enc_sub;
extern unsigned int mz0380_h264_frame_divisor;
extern unsigned int mz0380_enc_mask;
extern unsigned int mz0380_post_mask;
extern unsigned int mz0380_bitstream_num;
extern bool mz0380_enc_stat_ack_on;
extern bool mz0380_setvic_once;
extern bool mz0380_stop_on_streamoff;
extern bool mz0380_persistent_h264;
extern unsigned int mz0380_post_di;
/*
 * M221: the raw payload length, ALIGN16(w) * h * 3/2 - not the 1080p constant.
 *
 * Declared in mz0380.h because both sides need it and this is the only header
 * they share: the video side reaches it through mz0380-internal.h for
 * sizeimage, the DMA side through mz0380-dma-internal.h for the sentinels and
 * the copy. Putting it in either of those built clean on one side and failed on
 * the other, twice.
 */
size_t mz0380_raw_frame_bytes(struct mz0380_dev *dev);
size_t mz0380_raw_frame_bytes_for(u32 width, u32 height);

extern bool mz0380_raw_deliver;
extern bool mz0380_raw_capable;
extern unsigned int mz0380_post_skip;
extern unsigned int mz0380_post_avg;
extern bool mz0380_fake_frame_off;
extern bool mz0380_post_proc;
extern unsigned int mz0380_post_proc_gap_ms;
extern unsigned int mz0380_post_proc_opcode;
extern bool mz0380_irq_intx;
extern bool mz0380_set_buf_op8;
extern bool mz0380_raw_bank_probe;
extern bool mz0380_raw_probe_enc_tail;
extern bool mz0380_raw_probe_allow_30;
extern bool mz0380_raw_bank_observe;
extern bool mz0380_mst_win_output;
extern unsigned int mz0380_mst_ad;
extern bool mz0380_dma_handshake;
extern bool mz0380_enable_audio;
extern unsigned int mz0380_video_ring_entries;
extern unsigned int mz0380_video_ring_entry_size;
extern unsigned int mz0380_audio_ring_entries;
extern unsigned int mz0380_audio_ring_entry_size;

/*
 * M172: the reported field follows the negotiated timings.
 *
 * v4l2-compliance's last failure was "field == V4L2_FIELD_NONE" in the
 * DV-timings test: it sets one of the two interlaced modes this driver
 * enumerates (1080i50 and 1080i60), asks for the format, and is told
 * V4L2_FIELD_NONE - a progressive format for an interlaced signal. The field
 * was a hardcoded constant in mz0380_fill_pix_format().
 *
 * V4L2_FIELD_INTERLACED rather than ALTERNATE because this path delivers whole
 * frames in one buffer, and for interlaced BT timings bt.height is already the
 * frame height (1080 for 1080i), so the geometry stays consistent.
 *
 * The buffer metadata uses the same answer, so a delivered frame never claims a
 * different field from the format that was negotiated for it.
 */
static inline u32 mz0380_current_field(const struct mz0380_dev *dev)
{
	/*
	 * The nosg fake-frame generator produces a fixed progressive pattern
	 * whatever timings were set, and its delivery path says so, so the
	 * format must agree rather than inheriting an interlaced answer.
	 */
	if (mz0380_stream_nosg)
		return V4L2_FIELD_NONE;
	return dev->set_timings.bt.interlaced ? V4L2_FIELD_INTERLACED
					      : V4L2_FIELD_NONE;
}

#endif
