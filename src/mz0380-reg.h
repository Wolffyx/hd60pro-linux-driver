// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Driver for MZ0380 based capture cards (Elgato HD60 Pro family).
 *
 *  Register map, derived from our own reverse engineering of this board: the
 *  card's firmware image (yuan_demo_sdi: ep.ko, video_capture_mgr, tinyvenc5/7)
 *  and the Windows driver e60MZ0380.X64.SYS. See RE_FINDINGS.md.
 *
 *  Hardware overview:
 *    The MZ0380 board has two layers:
 *      1. A Xilinx FPGA bridge in front of an ARMv5 Mozart SoC (Vatics Mozart
 *         395s). The SoC boots its own embedded Linux from the card's OWN
 *         FLASH. The host never uploads firmware - the upload path was removed
 *         from this driver entirely.
 *      2. On the host side, the Xilinx fabric exposes two PCIe BARs:
 *           BAR0 : 1 MiB MMIO  - command mailbox and firmware buffer
 *           BAR5 : 4 KiB CFG   - SDK property/control window
 *
 *    What BAR0 does NOT contain, all established on hardware:
 *      - no host-visible Xilinx XDMA descriptor controller
 *      - no host-driven AXI IIC: I2C to the MST3367 goes over the mailbox
 *        (op 0x1a/0x1b), and the receiver is held in reset by GPIO pin 9
 *      - no HDMI signal-status window: the card never pushes the input format
 *        to the host over PCIe at all. Host-side detect is done by reading the
 *        MST3367 through the mailbox I2C proxy.
 *
 *    Earlier revisions of this file carried Sections B/C/D modelling all three
 *    of those on the sc0710 driver for a different card (Elgato 4K60 Pro
 *    Mk.2). Every define in them went unused and the model was disproved; they
 *    were deleted in 2026-08. Nothing in this driver derives from sc0710.
 *
 *  Cross-references:
 *    - yuan_demo_sdi/drivers/ep.ko  device-side BAR layout strings
 *    - e60MZ0380.X64.SYS strings    MZ0380_SEND_COMMAND, command opcodes
 *    - re-dump/tinyvenc7            preview/raw DMA writer (M213/M214)
 */

#ifndef _MZ0380_REG_H_
#define _MZ0380_REG_H_

/* ---- BAR identification --------------------------------------------- */

#define MZ0380_BAR_MMIO        0   /* PCI BAR index                     */
#define MZ0380_BAR_CFG         5

#define MZ0380_MAP_BAR_MMIO    0   /* index into mz0380_dev->lmmio[]    */
#define MZ0380_MAP_BAR_CFG     1

#define MZ0380_BAR_MMIO_SIZE   0x100000
#define MZ0380_BAR_CFG_SIZE    0x1000

/* ====================================================================
 *  Section A : BAR5 mailbox window
 * ====================================================================
 */

/* ---- Confirmed SDK-property param fields (correlation-verified) ---- */

#define MZ0380_REG_PROP_INPUT          0x0040  /* mask 0x7    */
#define MZ0380_REG_PROP_RECORD_MODE    0x0058  /* mask 0x3    */
#define MZ0380_REG_PROP_BITRATE        0x005c  /* full 32 bit */
#define MZ0380_REG_PROP_QUALITY        0x0060  /* full 32 bit */
#define MZ0380_REG_PROP_GOP            0x0080  /* full 32 bit */
#define MZ0380_REG_PROP_QP_STEP        0x0084  /* full 32 bit */
#define MZ0380_REG_PROP_B_FRAMES       0x0088  /* full 32 bit */

/* ==================================================================== *
 *  IMPORTANT: the command mailbox and firmware download buffer are NOT
 *  in BAR5 - they live in BAR0. This was confirmed by decompiling the
 *  Windows driver e60MZ0380.X64.SYS (see RE_FINDINGS.md). The device
 *  context maps ctx+0x108 -> BAR0 (mailbox + fw buffer) and
 *  ctx+0x110 -> BAR5 (DMA-pointer window + property cache). The former
 *  CHECKME BAR5 offsets (0x00c0.. and 0x0300/0x0400) were wrong; the
 *  mailbox definitions below are BAR0-relative and RE-confirmed.
 * ==================================================================== */

/* ---- Command mailbox - BAR0 (RE-confirmed) ------------------------- */

#define MZ0380_MB_DOORBELL             0x00    /* write MZ0380_MB_FIRE to trigger */
#define MZ0380_MB_OPCODE               0x04    /* PARAM0 slot = opcode            */
#define MZ0380_MB_RESULT               0x08    /* PARAM1 slot = result (0 == ok)  */
#define MZ0380_MB_PARAM(i)             (0x04 + ((i) * 4)) /* i=0 -> opcode slot   */
#define MZ0380_MB_MAX_ARGS             10U
#define MZ0380_MB_COMMAND_WORDS        (MZ0380_MB_MAX_ARGS + 1U)
#define MZ0380_MB_STATUS               0x2c    /* bit0 = command done             */
#define MZ0380_MB_EVENT                0x30    /* interrupt/event status word      */
/*
 * M138: these four are written by ep.ko's store_channel_done() - the sysfs
 * attribute the CARD's encoder thread writes when it finishes a frame - and
 * they are written UNCONDITIONALLY, before the completion-credit test that
 * decides whether an EVENT is raised at all.  Each holds one nibble per
 * channel taken from the encoder's 24-byte report:
 *
 *   0x40 <- report[1] - 1     0x44 <- report[2] - 1
 *   0x48 <- report[3] - 1     0x4c <- report[4] - 1  (audio channels)
 *
 * So they tick on every card-side frame completion whether or not the host
 * ever hears about it, which makes them the only host-visible proof that the
 * card's producer is still running.  The poll-drain watches 0x40/0x44/0x48
 * for exactly that reason.
 */
#define MZ0380_MB_EVT_PAYLOAD0         0x40    /* event payload words (DPC args)   */
#define MZ0380_MB_EVT_PAYLOAD1         0x44
#define MZ0380_MB_EVT_PAYLOAD2         0x48
#define MZ0380_MB_EVT_PAYLOAD3         0x4c    /* payload for EVENT[23:16] events  */
#define MZ0380_MB_FIRE                 0x800   /* doorbell value to fire a command */
#define MZ0380_MB_INT_ACK              0x400   /* doorbell value acking an event   */
#define MZ0380_MB_RESET                MZ0380_MB_INT_ACK /* old name, same value   */
#define MZ0380_MB_STATUS_DONE          BIT(0)  /* MZ0380_MB_STATUS bit0            */
#define MZ0380_MB_STATUS_OK_STAMP      0xaaaaaaaa /* fw stamps STATUS on success   */
#define MZ0380_MB_STATUS_BOOT_STAMP    0xdddddddd /* fw stamps STATUS after boot   */
#define MZ0380_MB_EVENT_CMD_DONE       BIT(11) /* EVENT bit11 = command complete   */
#define MZ0380_MB_POLL_ITERS           50      /* SEND_COMMAND polls 0x32 times    */

/* BAR5 notify pointers the host must program before CMD_INIT
 * (FUN_140278bb0): physical BAR0 addresses of the mailbox slots. */
#define MZ0380_CFG_NOTIFY_PTR0         0x30    /* <- bar0 phys + 0x04              */
#define MZ0380_CFG_NOTIFY_PTR1         0x38    /* <- bar0 phys + 0x5f              */

/*
 * BAR5 side of the interrupt handshake (Windows event thread
 * FUN_140284380): card sets BAR5[0xdc]=1 on event; host ack sequence is
 *   BAR5[0xdc]=2; BAR0[0x30]=0; BAR0[0x00]=0x400;
 */
#define MZ0380_CFG_INT_FLAG            0xdc
#define MZ0380_CFG_INT_ACK_VAL         2

/* opcode plus ten arguments; the next word is EVENT and is never payload */
#define MZ0380_REG_PARAM_MAX           11

/* Opcodes (RE-confirmed from DownloadFirmware/DownloadBaseFirmware) */
/*
 * Opcodes 0x0b/0x0c/0x0e/0x0f are the card's firmware-download path. They are
 * deliberately NOT defined here. The card boots its own flash image and this
 * driver has no upload path at all - see the header of mz0380-fw.c. Do not
 * re-add them.
 */

/*
 * Peripheral register file access (RE-confirmed, FUN_1402777e4 /
 * FUN_1402851cc): cmd {opcode, chip, reg, value}; read result lands in
 * PARAM3 (BAR0+0x10).
 */
#define MZ0380_CMD_I2C_READ_S           0x1e  /* M69: block read twin of 0x1f;
					       * on NAK leaves the payload
					       * UNTOUCHED (clean NAK detector,
					       * unlike 0x1a which forces 0)   */
#define MZ0380_CMD_REG_READ             0x1a
#define MZ0380_CMD_REG_WRITE            0x1b

/* chip ids seen in the Windows driver */
#define MZ0380_CHIP_BRIDGE              0x90  /* FPGA/bridge register file        */
#define MZ0380_CHIP_ENCODER             0x50  /* CHECKME - poked in fw-boot retry */
#define MZ0380_CHIP_TVP5160             0xb8  /* analog front-end                 */

/* bridge (chip 0x90) registers used by the Windows Interrupt_Handler */
#define MZ0380_BRIDGE_IRQ_MAIN          0x10  /* read-to-clear main status        */
#define MZ0380_BRIDGE_SIGNAL            0x12  /* bit0 = input signal present      */
#define MZ0380_BRIDGE_IRQ_SRC0          0x13  /* per-source status (read-to-clear)*/
#define MZ0380_BRIDGE_IRQ_SRC1          0x14
#define MZ0380_BRIDGE_IRQ_SRC2          0x15
#define MZ0380_BRIDGE_CTL18             0x18  /* bit7 toggled as a reset strobe   */
#define MZ0380_BRIDGE_CTL19             0x19  /* event ack/mask (bits 0x30 kept)  */

/* Post-boot handshake (RE-confirmed, FUN_140278bb0) */
#define MZ0380_CMD_INIT                 0x01  /* hello/init, no params, retried    */
#define MZ0380_CMD_GET_BOARD_VERSION    0x0a  /* STATUS=0xaaaaaaaa on success,
					       * running fw version -> PARAM 0x08/0x0c */

/*
 * Opcodes RE-confirmed from ep.ko disassembly (see RE_FINDINGS.md M6/M7).
 * SET_VIC_PARAMS is the host->card "select input + declare video standard"
 * command: field layout (byte offsets in the firmware command buffer, which
 * the mailbox packs as opcode->0x04, params->0x08..):
 *   cmd[5]=fps  cmd[6]=input-code  cmd[8:9]=width(u16)  cmd[10:11]=height(u16)
 *   cmd[0x22]=int_reduce flag. width==0||height==0 -> firmware sets no_signal.
 */
#define MZ0380_CMD_SET_VIC_PARAMS       0x29  /* 41: select input + WxH/fps    */
#define MZ0380_CMD_SET_ENC_PARAMS       0x2d  /* 45: masked H.264 encoder config */
/*
 * M82 (Windows collect-2026-08-19, func 0x14028a248): POST_PROC. Built and
 * sent immediately after the two SET_ENC_PARAMS calls, with count = 7 (opcode
 * + 5 payload words) and flag = 1 (no ACK poll). Payload, byte-exact from the
 * store sequence at 0x14028c8ba and confirmed against the driver's own printf
 * "mask = %08X, i = %d, fps = %d, skip = %d, avg = %d, di = %d, osd = %d,
 * mirror = %d, flip = %d":
 *     [4..7]   u32 mask = 0x1F   (all five fields valid)
 *     [8]  i   [9]  fps   [10] skip   [11] avg
 *     [12] di  [13] board_flag_a     [14] board_flag_b   [15] osd
 *     [16] flip  [17] mirror  [18] board_flag_c  [19] 0
 *     [20..23] 0
 * Both board flags and the [18] byte are zero on the 0x1C/0xFA (Elgato) board:
 * every branch that sets them tests a board id this card does not have, or a
 * context flag that read zero in all four traces. So the packet the HD60 Pro
 * actually receives is { 0x1F, fps<<8, 0x01, 0, 0 } - di = 1.
 *
 * ep.ko routes op 49 (0x31) to "notify epint" and nothing else, i.e. it is a
 * pure doorbell that wakes the encoder. Windows never sends START_STREAMING
 * (0x06) on the capture path at all; 0x2d and 0x31 are what kick tinyvenc.
 *
 * M128: the card's own name for 0x31 is SET_PREVIEW_PARAMS, and tinyvenc5's
 * printf at 0xf570 gives every field, which supersedes the guessed labels
 * above where the two disagree:
 *     [4..7] mask   [8] ch   [9] fps   [0x0a] skip   [0x0b] avg
 *     [0x0c] die_en ("di")   [0x0d] preview_off
 *     [0x0e] fake_frame_off  ("board_flag_b" above - it is not a board flag)
 *     [0x0f] preview_no_osd ("osd")
 *     [0x10] mirror   [0x11] flip   [0x12] hw_d
 * (the Windows read had mirror/flip the other way round; the card's printf
 * wins.) ep.ko's payload length for 0x31 is 20, so [20..23] never arrive.
 *
 * fake_frame_off is the gate on EncodingGroup::fake_frame_process, the thread
 * that draws NOSG_LOGO_Y. Windows sends 0 there, i.e. retail leaves the
 * standby splash armed too. See mz0380_fake_frame_off in mz0380-core.c.
 */
#define MZ0380_CMD_POST_PROC            0x31  /* 49: SET_PREVIEW_PARAMS        */
#define MZ0380_POST_PROC_MASK           0x1f
#define MZ0380_CMD_GPIO_READ            0x14  /* 20: read GPIO bitmap          */
#define MZ0380_CMD_GPIO_SET             0x15  /* 21: set GPIO data (prop 941)  */
#define MZ0380_CMD_GPIO_DIR             0x17  /* 23: set GPIO direction (940)  */

/* pattern-check: skip register/pin #define constants, no types or behavior */

/*
 * GPIO pin map (RE_FINDINGS.md M14, from e60MZ0380.X64.SYS disasm). op 0x15
 * (GPIO_SET) is single-pin: PARAM1 = mask = (1<<pin), PARAM2 = data =
 * (level<<pin). Direction is never set over the mailbox (pins are pre-config'd
 * outputs), so there is no GPIO_DIR step. pin9 held low at power-up is what
 * kept the MST3367 in reset and the I2C bus dead (M11-M15).
 */
#define MZ0380_GPIO_HPD                 1     /* HDMI hot-plug detect. M69:
					       * ACTIVE-LOW on the Elgato board:
					       * win64 FUN_14024eeb8 computes
					       * pin1 = ~(arg>>4)&1, so HPD_ON
					       * drives the pin LOW. We had it
					       * inverted for the whole project. */
#define MZ0380_GPIO_EDID_MUX            2     /* M69: DDC path mux, only ever
					       * touched by the Windows EDID
					       * handler: 1 = local EDID store
					       * on the SoC's i2c-0, 0 = the
					       * passthrough/monitor side.     */
#define MZ0380_GPIO_RX_ENABLE           3     /* receiver / mux enable (=1)    */
#define MZ0380_GPIO_RX_STRAP            8     /* companion reset/power strap   */
#define MZ0380_GPIO_RX_RESET            9     /* MST3367 reset, ACTIVE-LOW     */

/*
 * MST3367 HDMI receiver over the mailbox I2C proxy (op 0x1a read / 0x1b write).
 * 8-bit device address; the card firmware shifts it >>1 to 7-bit 0x4e. Register
 * space is banked: write reg 0x00 = bank before touching a banked register.
 * Register roles + detect math: RE_FINDINGS.md M15 + docs/re-2026-07-05/.
 */
/*
 * EDID bulk write, confirmed in both e60MZ0380.X64.SYS and yuan_ioctrl.
 * Opcode 0x1f consumes this packed command:
 *   cmd[4] = dev8, cmd[5] = EEPROM offset, cmd[6..7] = payload length,
 *   cmd[8..] = payload bytes.
 * The Windows driver writes 32-byte chunks and waits synchronously for the
 * shared STATUS completion before reusing the mailbox.  Opcode 0x20 is a
 * different generic combo transaction whose byte 5 is a direction flag.
 */
#define MZ0380_CMD_I2C_WRITE_S          0x1f
#define MZ0380_EDID_I2C_DEV             0xa0
#define MZ0380_EDID_CHUNK               32
#define MZ0380_EDID_SIZE                256

#define MST3367_I2C_DEV                 0x9c
#define MST3367_REG_BANK_SELECT         0x00
#define MST3367_BANK0                   0x00
#define MST3367_BANK1                   0x01
#define MST3367_BANK2                   0x02
/*
 * BANK3 exists per the GPL driver ("256-byte register shadow per bank",
 * BANK0..BANK3) but neither that driver nor our RE ever writes it. On
 * hardware it reads all zeros apart from the bank-select echo at reg 0x00,
 * while banks 0-2 are full of live config - the shape of an EDID RAM that
 * has never been loaded (M44).
 */
#define MST3367_BANK3                   0x03
/*
 * BANK0 0xb7 is the receiver's HPD/link enable (hdcapm: bit1 clear = HPD on).
 * The init sequence parks it at 0x02 (off) while configuring; the driver
 * raises HPD only after the EDID is in place.
 */
#define MST3367_B0_HPD                  0xb7
#define MST3367_B0_HPD_ON               0x00
#define MST3367_B0_HPD_OFF              0x02
/* BANK0 mode-detect block */
#define MST3367_B0_DETECT               0x55  /* usable lock iff mask == 0x3c   */
#define MST3367_B0_DETECT_LOCK_MASK     0x3c
/*
 * M67: bit 0x20 flaps independently of a usable lock, so the bits that must
 * ALL be set for the timing block to be readable are 0x04|0x08|0x10. Evidence
 * from every R55 this project has logged:
 *
 *   0x3f 0x5f 0x7f          -> timing block good (0x5f carried a textbook
 *                              1080p60, and 0x5f fails a 0x3c test)
 *   0xa3 0x23 0x27 0xc7 0x2b -> torn counters (vtot=5, htot=656, ...)
 *   0x83 0x03               -> no signal
 *
 * 0x1c classifies all ten correctly; 0x3c rejects good data whenever 0x20
 * happens to be low, which it was on three consecutive runs - and it can drop
 * BETWEEN the poll's read and the measurement's own re-read.
 */
#define MST3367_B0_DETECT_LOCK_CORE     0x1c
#define MST3367_B0_HPERIOD_HI           0x57  /* hperiod = 1600000/(hi<<8|lo)  */
#define MST3367_B0_HPERIOD_LO           0x58
#define MST3367_B0_VPERIOD_HI           0x59  /* vperiod = 1250000/(hi<<8|lo)  */
#define MST3367_B0_VPERIOD_LO           0x5a
#define MST3367_B0_VTOTAL_HI            0x5b
#define MST3367_B0_VTOTAL_LO            0x5c
#define MST3367_B0_INTERLACE            0x5f  /* HD60 Pro: bit3 = interlaced   */
#define MST3367_B0_INTERLACE_BIT        0x08
#define MST3367_B0_HTOTAL_HI            0x6a
#define MST3367_B0_HTOTAL_LO            0x6b
#define MST3367_B0_AUTO_POSITION        0xe2
#define MST3367_B0_AUTO_POSITION_ON     0x80
#define MST3367_B0_AUTO_POSITION_OFF    0x00
/* BANK2 active-pixel counter */
#define MST3367_B2_HACTIVE_LO           0x28  /* hactive = (0x29<<8)|0x28      */
#define MST3367_B2_HACTIVE_HI           0x29

/* Input codes for SET_VIC_PARAMS cmd[6] (QCAP qcap.h). */
#define MZ0380_INPUT_CODE_COMPOSITE     0
#define MZ0380_INPUT_CODE_HDMI          2
#define MZ0380_INPUT_CODE_DVI_D         3
#define MZ0380_INPUT_CODE_COMPONENT     4
#define MZ0380_INPUT_CODE_SDI           6
#define MZ0380_INPUT_CODE_AUTO          7

/*
 * START_STREAMING = mailbox opcode 0x06 (RE_FINDINGS M22, on-card RE).
 * SET_VIC_PARAMS (0x29) alone does NOT start frames: it makes video_capture_mgr
 * spawn tinyvenc5 (with the argv derived from the 44-byte SET_VIC command) and
 * clears no_signal, but tinyvenc5 then blocks reading /sys/vpl_pciep/epint for a
 * *separate* START_STREAMING command before it begins the per-frame host DMA
 * (channel_done pwrite -> ep.ko MSI + outbound ATU). tinyvenc5's command
 * dispatch (jump table indexed by cmd-6) maps command 6 to that start block;
 * ep.ko's op6 ISR handler (@0x1854) sysfs_notify()s epint whenever no_signal==0,
 * which is exactly the wake tinyvenc5 waits for. Op6 carries no payload.
 * (M6's "op6 = enc-status+notify" saw the ISR side but not the tinyvenc5
 * consumer, and so wrongly concluded no start opcode existed.)
 */
#define MZ0380_CMD_START_STREAMING      0x06  /* tinyvenc5 START (epint cmd 6) */

/* pattern-check: skip streaming register/opcode #define constants (M17) */
/*
 * Streaming/DMA protocol (RE_FINDINGS.md M17, both-sides RE). The card DMAs
 * encoded frames into host buffers via a PCIe iATU outbound window it programs
 * from host-supplied physical addresses. The host hands those addresses over
 * with the config-setter opcodes below (12-word command: opcode, channel,
 * stride, then up to 4 {phys_hi, phys_lo} pairs), arms with SET_VIC_PARAMS
 * (0x29), and stops with 0x07. Frame completion arrives as an EVENT (BAR0+0x30)
 * with the buffer token in BAR0+0x40.
 */
#define MZ0380_CMD_SET_BUF_2            0x02  /* buffer phys addrs (primary)   */
#define MZ0380_CMD_SET_BUF_3            0x03
#define MZ0380_CMD_SET_BUF_4            0x04
#define MZ0380_CMD_SET_BUF_5            0x05
#define MZ0380_CMD_SET_BUF_8            0x08
/*
 * M33, from video_capture_mgr's own dispatch (it preads the 44-byte command and
 * switches on word 0):
 *     cmp #7  -> STOP_STREAMING     cmp #41 (0x29) -> SET_VIC
 *     cmp #42 -> SET_AIC_PARAMS
 * So STOP is op 7 - right next to START (op 6) - and 0x2a, which M17 guessed
 * was STOP, is actually the audio-parameter command. We had been sending
 * SET_AIC with an all-zero payload (i.e. "audio off") at every streamoff, and
 * never issuing a stop at all.
 */
#define MZ0380_CMD_STOP_STREAMING       0x07  /* was 0x2a (wrong) - M33        */

/*
 * LOAD_FILES. M76: video_capture_mgr's op-0x6e handler (vcm FUN_0000aa3c) is
 * an arbitrary-offset 16-byte read/write of ONE fixed path on the card,
 * /mnt/flash/PIC_ENC:
 *
 *   struct[4..5]  = is_write   (0 = read the card -> host, 1 = write)
 *   struct[6..7]  = byte offset (fseek, 16-bit)
 *   struct[8..23] = the 16 data bytes; on a read vcm pwrite()s the whole
 *                   44-byte command back, so they come home in PARAM1..PARAM4
 *
 * The card boots the rootfs WE upload (mz0380-fw.c ships the whole
 * yuan_demo_sdi/ tar), so redirecting the card's stdout into that file turns
 * this opcode into the card's console over PCIe - no UART, no board access.
 * This define is the read half; nothing in-tree writes the file.
 */
#define MZ0380_CMD_LOAD_FILES           0x6e  /* 110: PIC_ENC 16B r/w - M76    */
#define MZ0380_LOAD_FILES_CHUNK         16

/*
 * SET_AIC_PARAMS. Field offsets recovered from video_capture_mgr's printf
 * ("Set AIC PARAMS-> bits, channel_num, mono, freq, frame_num_of_period,
 * period_num_of_buffer, on") by following the ARM vararg registers/stack, and
 * cross-checked against the Windows driver's "ai=%d, chs=%d, bits=%d, freq=%d,
 * period=%d.%d, is_aic_on=%d, aic_int_mode=%d". Offsets are into the card's
 * command buffer, where cmd+N == BAR0 + 4 + N == our PARAM((N/4)):
 *     cmd+4  u8  channel_num       cmd+5  u8  mono
 *     cmd+6  u16 bits              cmd+8  u32 freq
 *     cmd+12 u16 frame_num_of_period
 *     cmd+14 u16 period_num_of_buffer
 *     cmd+16 u8  on                cmd+17 u8  aic_int_mode (ep.ko G[0x63c])
 * on=1 makes video_capture_mgr run "echo '1' > /sys/audio_status/audio_ready",
 * which is what tinyvenc5's is_nosg path blocks on before it will ACK
 * START_STREAMING and start completing frames (M33).
 */
#define MZ0380_CMD_SET_AIC_PARAMS       0x2a  /* 42: audio params + on flag    */

/*
 * M128: tinyvenc5's complete dispatch table, read straight off the switch at
 * main+0x804 (jump table base 0xe6e0, index = cmd - 6, default 0xe654 = the
 * top of the poll loop). Names are the card's own, from the format strings
 * each handler prints; lengths are ep.ko's rodata[0xa0 + cmd] copy size.
 *
 *   cmd   handler   len  name
 *   0x06  0xe954     8   START_STREAMING - news the EncodingGroups, then one
 *                        pthread(on_start_thread) per channel -> Start()
 *   0x09  0xe93c     8   bare ACK: pwrite(epint, payload, 44). No side effect
 *                        at all, which is exactly why it works as a wake-up
 *   0x29  (pre-loop) 40  SET_VIC_PARAMS - read once BEFORE the loop; the
 *                        in-loop table maps 0x29 to the default, so a second
 *                        one is silently ignored
 *   0x2a  0xed48    20   SET_AIC_PARAMS (audio: i2s/audio counts). Video-inert
 *   0x2d  0xebdc    44   SET_ENC_PARAMS
 *   0x2f  0xebdc    44   SET_ENC_PARAMS_POST - SAME handler as 0x2d; the only
 *                        difference is which of the two banner strings it
 *                        prints and a "sub"/"main" tag. H.264 knobs only
 *                        (gop/qp/profile/bitrate/crop/resize): it cannot
 *                        affect capture, which retires M125's blind flood
 *   0x31  0xef24    20   SET_PREVIEW_PARAMS (see MZ0380_CMD_POST_PROC)
 *   0x50  0xeb8c    44   SET_OSD - on-screen text
 *   0x51  0xea78    20   SET_BAR - a colour-bar overlay rect, per (ch, line):
 *                        [4]=ch [5]=line [6..7]=is_show [8..9]=x [10..11]=y
 *                        [12..13]=w [14..15]=h [0x10]=update [0x11..0x13]=y,u,v
 *                        clamped against preview_settings[ch] w/h. Overlay only
 *   0x52  0xea34     7   SET_VIDEO_INVISIBLE - [4]=ch [5]=insert [6]=load
 *   0x62  0xe854    12   SET_LOGO - [4]=ch [5]=is_show [6]=reload
 *                        [7]=pic_order [8..9]=x [10..11]=y. Overlay only
 *
 * Anything else falls through to the default and is dropped without a word.
 * Of the four the M127 handoff listed as worth decoding, three (0x2f, 0x51,
 * 0x62) turn out to be encoder-side or cosmetic and are now closed. The
 * fourth, 0x31, is the one that matters.
 */
#define MZ0380_CMD_SET_ENC_PARAMS_POST  0x2f  /* 47: same handler as 0x2d      */
#define MZ0380_CMD_SET_BAR              0x51  /* 81: colour-bar overlay rect   */
#define MZ0380_CMD_SET_VIDEO_INVISIBLE  0x52  /* 82: insert/load               */
#define MZ0380_CMD_SET_LOGO             0x62  /* 98: logo overlay              */

/* BAR0 frame-completion status window (M17). EVENT is MZ0380_MB_EVENT (0x30). */
#define MZ0380_MB_FRAME_TOKEN           0x40  /* (token & 7) = buffer index    */
#define MZ0380_MB_ENC_STATUS            0x50  /* +N = per-channel status byte  */
/*
 * M40: that byte is the card's /sys/vpl_pciep/enc_stat<idx> (idx = ch*2 +
 * stream), and it is a HANDSHAKE, not a status report. Per frame the encoder
 * preads it: 0 = "host has consumed the last bitstream, encode another",
 * 1 = busy (retries 10x then skips the frame), 2 = skip. After DMAing a
 * bitstream the CARD writes 1 - and nothing card-side ever writes it back to
 * 0. Only the host can (ep.ko's encode_status_storeN backs this same byte),
 * so the host must clear it to acknowledge each consumed frame or the encoder
 * stops producing after one.
 */
#define MZ0380_MB_ENC_STAT_IDX(ch, stream)  ((ch) * 2 + (stream))
#define MZ0380_MB_ENC_STAT_FREE         0     /* host ack: buffer consumed     */

/* First-cut streaming geometry (video channel 0). Tunable once frames flow. */
#define MZ0380_STREAM_VIDEO_CHANNEL     0
#define MZ0380_STREAM_NR_BUFS           4     /* 4 phys pairs per SET_BUF cmd  */
/*
 * M29: 512 KiB was too small - the card streamed straight past the end of the
 * buffer and the overrun was what produced every IOMMU fault from M26 on (the
 * writes INSIDE the buffer had been succeeding silently all along). 4 MiB is
 * one alloc_pages order-10 block, and comfortably over an uncompressed
 * 1920x1080 NV12 frame (~3.1 MiB) as well as any sane H.264 access unit. The
 * card's outbound aperture is 32 MiB total, so 4 x 4 MiB still fits.
 */
#define MZ0380_STREAM_BUF_SIZE          0x400000 /* 4 MiB per frame buffer     */
#define MZ0380_STREAM_BUF_STRIDE        MZ0380_STREAM_BUF_SIZE

/*
 * Windows gives opcode 0x04 (outbound window 1) a separate four-buffer bank
 * for the H.264 encoder.  Its command advertises 0x97f00 bytes per slot; use
 * a 1 MiB backing allocation so the mapping is page/order aligned and leaves
 * a guard margin around the observed Windows size.
 */
#define MZ0380_H264_BUF_SIZE            0x100000 /* 1 MiB backing per slot     */
#define MZ0380_H264_SET_BUF_SIZE        0x097f00 /* Windows command size word  */
#define MZ0380_H264_POISON_BYTE         0xcc

/*
 * Windows' HD60 Pro branch gives outbound window 0 two independent four-slot
 * banks.  Both op 0x02 and op 0x08 advertise this exact YUV422 allocation:
 * 2048-byte stride * 1125 lines * 2 bytes + a 4096-byte header.
 */
#define MZ0380_RAW_PROBE_BANKS          2
#define MZ0380_RAW_PROBE_NR_BUFS        \
	(MZ0380_RAW_PROBE_BANKS * MZ0380_STREAM_NR_BUFS)
#define MZ0380_RAW_PROBE_BUF_SIZE       0x466000
#define MZ0380_RAW_PROBE_FRAME_SIZE     0x2f7600
#define MZ0380_RAW_PROBE_BANK0_POISON   0xa5
#define MZ0380_RAW_PROBE_BANK1_POISON   0x5a

/*
 * M36/M37 (proven on hw): the fake-frame path lands ONE fully contiguous raw
 * burst of exactly 1920 x 1107 x 1.5 bytes (0x30a5c0) in buf0 within 450 ms
 * of START, then the card's encoder loop parks (M39: a fresh spawn yields
 * exactly one more frame).
 *
 * M50 (decoded from a delivered capture): the burst SIZE is 1920x1107-based,
 * but the PICTURE inside is NV12 720x1080 - the fake-frame renderer draws
 * its "NO SIGNAL" splash (spinner + text, 0x11 background) at 720 wide no
 * matter what width SET_VIC carried. Layout: Y 720x1080 @0, UV 720x540
 * @0x11cc40*2/3 (=0xbdd80, all 0x80 = neutral chroma), 0xff junk fill to
 * the end of the burst. So the deliverable payload is the leading 0x11cc40
 * bytes.
 */
#define MZ0380_STREAM_RAW_WIDTH         1920
#define MZ0380_STREAM_RAW_HEIGHT        1107
#define MZ0380_STREAM_RAW_FRAME_SIZE \
	(MZ0380_STREAM_RAW_WIDTH * MZ0380_STREAM_RAW_HEIGHT * 3 / 2)
#define MZ0380_NOSG_NV12_WIDTH          720
#define MZ0380_NOSG_NV12_HEIGHT         1080
#define MZ0380_NOSG_NV12_SIZEIMAGE \
	(MZ0380_NOSG_NV12_WIDTH * MZ0380_NOSG_NV12_HEIGHT * 3 / 2)

/*
 * Disproven guesses (absent from the ep.ko dispatcher): SET_AIC=0x11,
 * QUERY_SIGNAL=0x20. There is no card->host signal query.
 */
#define MZ0380_CMD_RESET                0xFF  /* CHECKME */

/* ====================================================================
 *  Interrupt status/mask (CHECKME)
 *
 *  ep.ko references "pciep_isr_clrint" and "msi_enable" but the host-
 *  visible interrupt register layout is not yet known. Hypothesised
 *  block lives in BAR0 above the HDMI status window.
 * ====================================================================
 */

#define MZ0380_REG_IRQ_STATUS           0x0100  /* CHECKME (BAR0) */
#define MZ0380_REG_IRQ_MASK             0x0104  /* CHECKME (BAR0) */
#define MZ0380_REG_IRQ_ACK              0x0108  /* CHECKME (BAR0) */

#define MZ0380_IRQ_CMD_COMPLETE         BIT(0)
#define MZ0380_IRQ_VIDEO_RING_READY     BIT(1)
#define MZ0380_IRQ_AUDIO_RING_READY     BIT(2)
#define MZ0380_IRQ_SIGNAL_CHANGE        BIT(3)
#define MZ0380_IRQ_FW_READY             BIT(4)
#define MZ0380_IRQ_ERROR                BIT(5)

/* ====================================================================
 *  Ring base/size/head/tail (legacy mailbox - kept for now)
 *
 *  Two conflicting models:
 *    Model A (mailbox):  ring base programmed via BAR5 0x0200..0x0234.
 *                        Card pushes 4 KiB-aligned descriptors of the
 *                        custom flags+len+pts format below.
 *    Model B (XDMA):     an sc0710-style Xilinx XDMA descriptor table.
 *                        DISPROVEN and its definitions deleted - this card
 *                        has no host-visible XDMA controller.
 *
 *  Model A is what the bring-up scaffolding in mz0380-dma.c uses. The real
 *  streaming path is neither: it is the mailbox plus the outbound windows.
 * ====================================================================
 */

#define MZ0380_REG_VIDEO_RING_BASE_LO   0x0200  /* CHECKME (BAR5) */
#define MZ0380_REG_VIDEO_RING_BASE_HI   0x0204
#define MZ0380_REG_VIDEO_RING_SIZE      0x0208
#define MZ0380_REG_VIDEO_RING_ENTRIES   0x020c
#define MZ0380_REG_VIDEO_RING_HEAD      0x0210
#define MZ0380_REG_VIDEO_RING_TAIL      0x0214

#define MZ0380_REG_AUDIO_RING_BASE_LO   0x0220  /* CHECKME (BAR5) */
#define MZ0380_REG_AUDIO_RING_BASE_HI   0x0224
#define MZ0380_REG_AUDIO_RING_SIZE      0x0228
#define MZ0380_REG_AUDIO_RING_ENTRIES   0x022c
#define MZ0380_REG_AUDIO_RING_HEAD      0x0230
#define MZ0380_REG_AUDIO_RING_TAIL      0x0234

/* Per-entry descriptor in our Model A ring (CHECKME) */
#define MZ0380_DESC_FLAGS_OFFSET        0x00
#define MZ0380_DESC_BYTECOUNT_OFFSET    0x04
#define MZ0380_DESC_PTS_LO_OFFSET       0x08
#define MZ0380_DESC_PTS_HI_OFFSET       0x0c
#define MZ0380_DESC_PAYLOAD_OFFSET      0x14

#define MZ0380_DESC_FLAG_KEY_FRAME      BIT(0)
#define MZ0380_DESC_FLAG_END_OF_STREAM  BIT(1)
#define MZ0380_DESC_FLAG_ERROR          BIT(2)

#endif /* _MZ0380_REG_H_ */
