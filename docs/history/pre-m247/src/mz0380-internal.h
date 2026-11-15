/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _MZ0380_INTERNAL_H_
#define _MZ0380_INTERNAL_H_

#include "mz0380.h"

#define MZ0380_PROC_CMD_MAX 64

struct mz0380_snapshot_reg {
	const char *section;
	const char *name;
	int map;
	u32 reg;
	unsigned int min_profile;
};

extern const struct mz0380_snapshot_reg mz0380_snapshot_regs[];
extern const unsigned int mz0380_snapshot_regs_count;

/* Diagnostics/procfs parameters consumed outside their definition unit. */
extern unsigned int procfs_verbosity;
extern unsigned int debug;
extern unsigned int snapshot_profile;
extern unsigned int scan_bar;
extern unsigned int scan_start;
extern unsigned int scan_len;
extern bool scan_unsafe;
extern unsigned int periph_chip;
extern unsigned int periph_start;
extern unsigned int periph_count;
extern bool periph_probe;
extern unsigned int event_sample_us;
extern bool event_auto_ack;
extern unsigned int credit_kick_ms;

/* Experimental control-field module parameters shared with procfs. */
extern bool allow_experimental_writes;
extern unsigned int input_select_reg;
extern unsigned int input_select_mask;
extern unsigned int input_select_shift;
extern unsigned int bitrate_reg;
extern unsigned int bitrate_mask;
extern unsigned int bitrate_shift;
extern unsigned int quality_reg;
extern unsigned int quality_mask;
extern unsigned int quality_shift;
extern unsigned int gop_reg;
extern unsigned int gop_mask;
extern unsigned int gop_shift;
extern unsigned int b_frames_reg;
extern unsigned int b_frames_mask;
extern unsigned int b_frames_shift;
extern unsigned int qp_step_reg;
extern unsigned int qp_step_mask;
extern unsigned int qp_step_shift;
extern unsigned int record_mode_reg;
extern unsigned int record_mode_mask;
extern unsigned int record_mode_shift;

u32 mz0380_cfg_trace_reg(unsigned int index);
u32 mz0380_extract_field(u32 word, u32 mask, u32 shift);
bool mz0380_candidate_config_valid(struct mz0380_dev *dev, u32 reg,
				    u32 mask, u32 shift);
bool mz0380_input_select_config_valid(struct mz0380_dev *dev);
bool mz0380_source_uses_candidate_override(const char *source);
const char *mz0380_property_experiment_result_name(
	enum mz0380_property_experiment_result result);
int mz0380_request_scalar_property_locked(
	struct mz0380_dev *dev, struct mz0380_property_experiment *exp,
	u32 property_id, u32 *cached_value, u32 requested_value,
	u32 value_count, u32 reg, u32 mask, u32 shift, const char *source,
	bool require_write_enable);

void mz0380_input_candidate_clear(void);
void mz0380_input_candidate_set(u32 reg, u32 mask, u32 shift);
void mz0380_bitrate_candidate_clear(void);
void mz0380_bitrate_candidate_set(u32 reg, u32 mask, u32 shift);
void mz0380_quality_candidate_clear(void);
void mz0380_quality_candidate_set(u32 reg, u32 mask, u32 shift);
void mz0380_gop_candidate_clear(void);
void mz0380_gop_candidate_set(u32 reg, u32 mask, u32 shift);
void mz0380_b_frames_candidate_clear(void);
void mz0380_b_frames_candidate_set(u32 reg, u32 mask, u32 shift);
void mz0380_qp_step_candidate_clear(void);
void mz0380_qp_step_candidate_set(u32 reg, u32 mask, u32 shift);
void mz0380_record_mode_candidate_clear(void);
void mz0380_record_mode_candidate_set(u32 reg, u32 mask, u32 shift);

void mz0380_dump_snapshot_range(struct seq_file *m, struct mz0380_dev *dev,
				unsigned int profile, const char *title,
				int map, u32 start, u32 end);
unsigned int mz0380_snapshot_profile(void);
void mz0380_dump_snapshot_reg(struct seq_file *m, struct mz0380_dev *dev,
			      const struct mz0380_snapshot_reg *snap);
void mz0380_dump_pci_config(struct seq_file *m, struct mz0380_dev *dev);
void mz0380_dump_pci_caps(struct seq_file *m, struct mz0380_dev *dev);
void mz0380_dump_snapshot(struct seq_file *m, struct mz0380_dev *dev);
void mz0380_dump_control_path(struct seq_file *m, struct mz0380_dev *dev);
void mz0380_dump_input_experiment(struct seq_file *m,
				  struct mz0380_dev *dev);
void mz0380_dump_bitrate_experiment(struct seq_file *m,
				    struct mz0380_dev *dev);
void mz0380_dump_quality_experiment(struct seq_file *m,
				    struct mz0380_dev *dev);
void mz0380_dump_gop_experiment(struct seq_file *m,
				struct mz0380_dev *dev);
void mz0380_dump_b_frames_experiment(struct seq_file *m,
				     struct mz0380_dev *dev);
void mz0380_dump_qp_step_experiment(struct seq_file *m,
				    struct mz0380_dev *dev);
void mz0380_dump_record_mode_experiment(struct seq_file *m,
					struct mz0380_dev *dev);

int mz0380_proc_show(struct seq_file *m, void *v);
int mz0380_proc_snapshot_show(struct seq_file *m, void *v);
int mz0380_proc_control_show(struct seq_file *m, void *v);
int mz0380_proc_experiment_show(struct seq_file *m, void *v);
int mz0380_proc_scan_show(struct seq_file *m, void *v);
int mz0380_proc_periph_scan_show(struct seq_file *m, void *v);
int mz0380_proc_events_show(struct seq_file *m, void *v);
int mz0380_proc_hdmi_show(struct seq_file *m, void *v);
int mz0380_proc_cmd_show(struct seq_file *m, void *v);
ssize_t mz0380_proc_events_write(struct file *file,
				 const char __user *buffer,
				 size_t count, loff_t *ppos);
ssize_t mz0380_proc_hdmi_write(struct file *file,
			       const char __user *buffer,
			       size_t count, loff_t *ppos);
ssize_t mz0380_proc_cmd_write(struct file *file,
			     const char __user *buffer,
			     size_t count, loff_t *ppos);

u32 mz0380_current_sizeimage(struct mz0380_dev *dev);

u32 mz0380_current_pixelformat(struct mz0380_dev *dev);
bool mz0380_is_raw_fourcc(u32 fourcc);
extern const struct vb2_ops mz0380_qops;

#endif
