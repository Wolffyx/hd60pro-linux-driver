// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MZ0380 PCI, control, and experiment diagnostic formatting.
 */

#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>

#include "mz0380-internal.h"

void mz0380_dump_pci_config(struct seq_file *m, struct mz0380_dev *dev)
{
	u32 reg;
	u32 val = 0;

	seq_puts(m, "  pci cfg dump:\n");
	for (reg = 0; reg < 0x100; reg += 0x10) {
		pci_read_config_dword(dev->pci, reg + 0x0, &val);
		seq_printf(m, "    %02x: %08x", reg + 0x0, val);
		pci_read_config_dword(dev->pci, reg + 0x4, &val);
		seq_printf(m, " %08x", val);
		pci_read_config_dword(dev->pci, reg + 0x8, &val);
		seq_printf(m, " %08x", val);
		pci_read_config_dword(dev->pci, reg + 0xc, &val);
		seq_printf(m, " %08x\n", val);
		}
}

void mz0380_dump_pci_caps(struct seq_file *m, struct mz0380_dev *dev)
{
	int pm_cap;
	int msi_cap;
	int pcie_cap;

	pm_cap = pci_find_capability(dev->pci, PCI_CAP_ID_PM);
	msi_cap = pci_find_capability(dev->pci, PCI_CAP_ID_MSI);
	pcie_cap = pci_find_capability(dev->pci, PCI_CAP_ID_EXP);

	seq_printf(m, "  caps       : PM=%#x MSI=%#x PCIe=%#x\n",
		   pm_cap, msi_cap, pcie_cap);

	if (pm_cap) {
		u16 pmcsr = 0;

		pci_read_config_word(dev->pci, pm_cap + PCI_PM_CTRL, &pmcsr);
		seq_printf(m, "  pm csr     : %04x\n", pmcsr);
	}

	if (msi_cap) {
		u16 msi_flags = 0;

		pci_read_config_word(dev->pci, msi_cap + PCI_MSI_FLAGS, &msi_flags);
		seq_printf(m,
			   "  msi flags  : %04x (enable=%u 64bit=%u multimsg_cap=%u multimsg_en=%u)\n",
			   msi_flags,
			   !!(msi_flags & PCI_MSI_FLAGS_ENABLE),
			   !!(msi_flags & PCI_MSI_FLAGS_64BIT),
			   (msi_flags & PCI_MSI_FLAGS_QMASK) >> 1,
			   (msi_flags & PCI_MSI_FLAGS_QSIZE) >> 4);
	}

	if (pcie_cap) {
		u16 pcie_flags = 0;
		u16 devctl = 0;
		u16 devsta = 0;
		u32 lnkcap = 0;
		u16 lnksta = 0;

		pcie_capability_read_word(dev->pci, PCI_EXP_FLAGS, &pcie_flags);
		pcie_capability_read_word(dev->pci, PCI_EXP_DEVCTL, &devctl);
		pcie_capability_read_word(dev->pci, PCI_EXP_DEVSTA, &devsta);
		pcie_capability_read_dword(dev->pci, PCI_EXP_LNKCAP, &lnkcap);
		pcie_capability_read_word(dev->pci, PCI_EXP_LNKSTA, &lnksta);

		seq_printf(m, "  pcie flags : %04x (ver=%u type=%u)\n",
			   pcie_flags,
			   pcie_flags & PCI_EXP_FLAGS_VERS,
			   (pcie_flags & PCI_EXP_FLAGS_TYPE) >> 4);
		seq_printf(m, "  pcie dev   : ctl=%04x sta=%04x\n",
			   devctl, devsta);
		seq_printf(m,
			   "  pcie link  : cap=%08x speed_cap=%u width_cap=x%u status=%04x speed=%u width=x%u\n",
			   lnkcap,
			   lnkcap & PCI_EXP_LNKCAP_SLS,
			   (lnkcap & PCI_EXP_LNKCAP_MLW) >> 4,
			   lnksta,
			   lnksta & PCI_EXP_LNKSTA_CLS,
			   (lnksta & PCI_EXP_LNKSTA_NLW) >> 4);
	}
}

void mz0380_dump_snapshot(struct seq_file *m, struct mz0380_dev *dev)
{
	unsigned int i;
	unsigned int profile = mz0380_snapshot_profile();
	const char *section = NULL;

	seq_puts(m, "  snapshot   : targeted read-only register snapshot\n");
	seq_puts(m, "  scope      : explicitly curated, known-safe offsets only\n");
	seq_puts(m, "  use        : compare repeated reads across HDMI cable or mode changes\n");
	seq_printf(m, "  profile    : %u\n", profile);
	if (profile >= 2)
		seq_puts(m, "  warning    : profile 2 adds experimental BAR5 low-offset reads beyond the vetted baseline\n");
	if (profile >= 3)
		seq_puts(m, "  warning    : profile 3 also adds SC0710-inspired BAR0 signal probes that are unvetted on MZ0380\n");
	if (profile >= 4)
		seq_puts(m, "  warning    : profile 4 also adds an extended BAR5 window and should be treated as exploratory only\n");
	if (profile >= 5)
		seq_puts(m, "  warning    : profile 5 extends BAR5 further and should be used only for deliberate correlation tests\n");

	for (i = 0; i < mz0380_snapshot_regs_count; i++) {
		const struct mz0380_snapshot_reg *snap = &mz0380_snapshot_regs[i];

		if (profile < snap->min_profile)
			continue;

		if (!section || strcmp(section, snap->section)) {
			section = snap->section;
			seq_printf(m, "  %s\n", section);
		}

		mz0380_dump_snapshot_reg(m, dev, snap);
	}
}

void mz0380_dump_control_path(struct seq_file *m, struct mz0380_dev *dev)
{
	unsigned int profile = mz0380_snapshot_profile();

	seq_puts(m, "  control    : SDK-guided BAR5 correlation view\n");
	seq_printf(m, "  profile    : %u\n", profile);
	seq_puts(m, "  handles    : raw=\"MZ0380 PCI\"\n");
	seq_puts(m, "               enc=\"MZ0380 PCI, Analog Encoder\"\n");
	seq_puts(m, "               aud=\"MZ0380 PCI, Analog WaveIn\"\n");
	seq_puts(m, "  sequence   : query 0/8 -> query or set 201 -> raw format -> encoder format and properties -> optional audio -> RUN\n");
	seq_puts(m, "  priorities : keep property 201, 407, 403, 404, 405, 408, and 411 grounded, then move to the next syntax-family target\n");
	seq_puts(m, "  groups     : identity/capability = 0, 8\n");
	seq_puts(m, "               input/receiver = 201, 232, 234, 235\n");
	seq_puts(m, "               encoder rate = 403, 404, 407, 409, 410\n");
	seq_puts(m, "               encoder syntax = 405, 406, 408, 411, 412, 413, 414, 415, 422, 424\n");
	seq_puts(m, "  generic map: 0->405, 1->404, 3->407, 4->403, 5->408,\n");
	seq_puts(m, "               6->409, 7->410, 0x0a->411, 0x0d->422\n");
	mz0380_dump_snapshot_range(m, dev, profile,
				   "BAR5 identity/capability candidates [0x0000..0x0018]",
				   MZ0380_MAP_BAR_CFG, 0x0000, 0x0018);
	mz0380_dump_snapshot_range(m, dev, profile,
				   "BAR5 BAR0-pointer or doorbell hints [0x0030..0x003c]",
				   MZ0380_MAP_BAR_CFG, 0x0030, 0x003c);
	mz0380_dump_snapshot_range(m, dev, profile,
				   "BAR5 working input/receiver window [0x0040..0x0054]",
				   MZ0380_MAP_BAR_CFG, 0x0040, 0x0054);
	mz0380_dump_snapshot_range(m, dev, profile,
				   "BAR5 working encoder-rate window [0x0058..0x007c]",
				   MZ0380_MAP_BAR_CFG, 0x0058, 0x007c);
	mz0380_dump_snapshot_range(m, dev, profile,
				   "BAR5 working encoder-syntax window [0x0080..0x00bc]",
				   MZ0380_MAP_BAR_CFG, 0x0080, 0x00bc);
	seq_puts(m, "  experiment : write 'input <0..4>', 'bitrate <value>', 'quality <value>', 'gop <value>', 'bframes <0..2>', 'qpstep <value>', or 'recordmode <0..2>' to /proc/mz0380-experiment for property-201/403/404/405/407/408/411 runs\n");
	seq_puts(m, "  guardrail  : confirmed properties 201/403/404/405/407/408/411 use fixed BAR5 mappings in the normal driver path; allow_experimental_writes=1 is only required for candidate overrides\n");
}

static void mz0380_dump_cfg_trace_changes(struct seq_file *m,
					  const struct mz0380_property_experiment *exp)
{
	unsigned int i;
	bool found = false;

	for (i = 0; i < MZ0380_CFG_TRACE_COUNT; i++) {
		u32 reg = mz0380_cfg_trace_reg(i);
		u32 before = exp->before_cfg_trace[i];
		u32 after = exp->after_cfg_trace[i];

		if (before == after)
			continue;

		seq_printf(m, "    [0x%04x] %08x -> %08x\n", reg, before, after);
		found = true;
	}

	if (!found)
		seq_puts(m, "    no BAR5 delta across [0x0000..0x00bc]\n");
}

void mz0380_dump_input_experiment(struct seq_file *m,
					 struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->input_experiment;

	seq_puts(m, "  experiment : SDK property-201 input-select helper\n");
	seq_puts(m, "  command    : echo 'input <0..4>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'candidate clear' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'writes on|off' > /proc/mz0380-experiment\n");
	seq_printf(m, "  writes     : %s\n",
		   allow_experimental_writes ? "enabled" : "disabled");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_INPUT_SELECT_HW_REG, MZ0380_INPUT_SELECT_HW_MASK,
		   MZ0380_INPUT_SELECT_HW_SHIFT);

	if (input_select_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   input_select_reg, input_select_mask,
			   input_select_shift,
			   mz0380_input_select_config_valid(dev) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %s (%u)\n",
		   mz0380_input_name(exp->previous_value),
		   exp->previous_value);
	seq_printf(m, "  requested  : %s (%u)\n",
		   mz0380_input_name(exp->requested_value),
		   exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u -> %u -> %u\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}

void mz0380_dump_bitrate_experiment(struct seq_file *m,
					   struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->bitrate_experiment;

	seq_puts(m, "  experiment : SDK property-403 bitrate helper\n");
	seq_puts(m, "  command    : echo 'bitrate <value>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'bitrate-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'bitrate-candidate clear' > /proc/mz0380-experiment\n");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_BITRATE_HW_REG, MZ0380_BITRATE_HW_MASK,
		   MZ0380_BITRATE_HW_SHIFT);

	if (bitrate_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   bitrate_reg, bitrate_mask, bitrate_shift,
			   mz0380_candidate_config_valid(dev, bitrate_reg,
							 bitrate_mask,
							 bitrate_shift) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %u\n", exp->previous_value);
	seq_printf(m, "  requested  : %u\n", exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u -> %u -> %u\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}

void mz0380_dump_quality_experiment(struct seq_file *m,
					   struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->quality_experiment;

	seq_puts(m, "  experiment : SDK property-404 quality helper\n");
	seq_puts(m, "  command    : echo 'quality <value>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'quality-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'quality-candidate clear' > /proc/mz0380-experiment\n");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_QUALITY_HW_REG, MZ0380_QUALITY_HW_MASK,
		   MZ0380_QUALITY_HW_SHIFT);

	if (quality_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   quality_reg, quality_mask, quality_shift,
			   mz0380_candidate_config_valid(dev, quality_reg,
							 quality_mask,
							 quality_shift) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %u\n", exp->previous_value);
	seq_printf(m, "  requested  : %u\n", exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u -> %u -> %u\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}

void mz0380_dump_gop_experiment(struct seq_file *m,
				       struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->gop_experiment;

	seq_puts(m, "  experiment : SDK property-405 GOP helper\n");
	seq_puts(m, "  command    : echo 'gop <value>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'gop-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'gop-candidate clear' > /proc/mz0380-experiment\n");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_GOP_HW_REG, MZ0380_GOP_HW_MASK,
		   MZ0380_GOP_HW_SHIFT);

	if (gop_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   gop_reg, gop_mask, gop_shift,
			   mz0380_candidate_config_valid(dev, gop_reg,
							 gop_mask,
							 gop_shift) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %u\n", exp->previous_value);
	seq_printf(m, "  requested  : %u\n", exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u -> %u -> %u\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}

void mz0380_dump_b_frames_experiment(struct seq_file *m,
					    struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->b_frames_experiment;

	seq_puts(m, "  experiment : SDK property-411 B-frame helper\n");
	seq_puts(m, "  command    : echo 'bframes <0..2>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'bframes-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'bframes-candidate clear' > /proc/mz0380-experiment\n");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_B_FRAMES_HW_REG, MZ0380_B_FRAMES_HW_MASK,
		   MZ0380_B_FRAMES_HW_SHIFT);

	if (b_frames_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   b_frames_reg, b_frames_mask, b_frames_shift,
			   mz0380_candidate_config_valid(dev, b_frames_reg,
							 b_frames_mask,
							 b_frames_shift) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %u\n", exp->previous_value);
	seq_printf(m, "  requested  : %u\n", exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u -> %u -> %u\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}

void mz0380_dump_qp_step_experiment(struct seq_file *m,
					   struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->qp_step_experiment;

	seq_puts(m, "  experiment : SDK property-408 QP-step helper\n");
	seq_puts(m, "  command    : echo 'qpstep <value>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'qpstep-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'qpstep-candidate clear' > /proc/mz0380-experiment\n");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_QP_STEP_HW_REG, MZ0380_QP_STEP_HW_MASK,
		   MZ0380_QP_STEP_HW_SHIFT);

	if (qp_step_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   qp_step_reg, qp_step_mask, qp_step_shift,
			   mz0380_candidate_config_valid(dev, qp_step_reg,
							 qp_step_mask,
							 qp_step_shift) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %u\n", exp->previous_value);
	seq_printf(m, "  requested  : %u\n", exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u -> %u -> %u\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}

void mz0380_dump_record_mode_experiment(struct seq_file *m,
					       struct mz0380_dev *dev)
{
	const struct mz0380_property_experiment *exp = &dev->record_mode_experiment;

	seq_puts(m, "  experiment : SDK property-407 record-mode helper\n");
	seq_puts(m, "  command    : echo 'recordmode <0..2>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'recordmode-candidate <reg> <mask> <shift>' > /proc/mz0380-experiment\n");
	seq_puts(m, "               echo 'recordmode-candidate clear' > /proc/mz0380-experiment\n");
	seq_printf(m,
		   "  backing    : fixed BAR5[0x%04x] mask=0x%08x shift=%u\n",
		   MZ0380_RECORD_MODE_HW_REG, MZ0380_RECORD_MODE_HW_MASK,
		   MZ0380_RECORD_MODE_HW_SHIFT);

	if (record_mode_reg == ~0U)
		seq_puts(m, "  candidate  : none; procfs requests use the fixed hardware mapping\n");
	else
		seq_printf(m,
			   "  candidate  : override reg=0x%04x mask=0x%08x shift=%u (%s)\n",
			   record_mode_reg, record_mode_mask,
			   record_mode_shift,
			   mz0380_candidate_config_valid(dev, record_mode_reg,
							 record_mode_mask,
							 record_mode_shift) ?
			   "config valid" : "invalid config");

	if (!exp->valid) {
		seq_puts(m, "  last       : none\n");
		return;
	}

	seq_printf(m, "  source     : %s\n", exp->source);
	seq_printf(m, "  previous   : %s (%u)\n",
		   mz0380_record_mode_name(exp->previous_value),
		   exp->previous_value);
	seq_printf(m, "  requested  : %s (%u)\n",
		   mz0380_record_mode_name(exp->requested_value),
		   exp->requested_value);
	seq_printf(m, "  result     : %s\n",
		   mz0380_property_experiment_result_name(exp->result));

	if (exp->hardware_write) {
		seq_printf(m,
			   "  write      : reg=0x%04x mask=0x%08x shift=%u\n",
			   exp->reg, exp->mask, exp->shift);
		seq_printf(m,
			   "               before=%08x programmed=%08x readback=%08x\n",
			   exp->before_word, exp->programmed_word,
			   exp->readback_word);
		seq_printf(m,
			   "               fields=%u (%s) -> %u (%s) -> %u (%s)\n",
			   mz0380_extract_field(exp->before_word, exp->mask,
						exp->shift),
			   mz0380_record_mode_name(
				mz0380_extract_field(exp->before_word,
						      exp->mask, exp->shift)),
			   mz0380_extract_field(exp->programmed_word,
						exp->mask, exp->shift),
			   mz0380_record_mode_name(
				mz0380_extract_field(exp->programmed_word,
						      exp->mask, exp->shift)),
			   mz0380_extract_field(exp->readback_word, exp->mask,
						exp->shift),
			   mz0380_record_mode_name(
				mz0380_extract_field(exp->readback_word,
						      exp->mask, exp->shift)));
	}

	seq_puts(m, "  bar5 delta :\n");
	mz0380_dump_cfg_trace_changes(m, exp);
}
