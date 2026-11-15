#!/bin/bash
# ARCHIVED - DO NOT RUN. Added when this file was archived (see ../README.md).
# The first stage of this script (pm reset) hung the X570 chipset and took the
# whole host down (RE_FINDINGS M249). A wedged card is recovered by removing
# power only. Everything below is kept unchanged as a record of the experiment.
echo "archived: this script PCI-resets the card, which crashed the host (M249). Not running." >&2
exit 1

# M52: can a wedged card be recovered WITHOUT a mains-off cold boot?
#
# The wedge (hit after ~8-18 encoder spawns): the card's mailbox stops ACKing
# everything - CMD_INIT times out (-110) and the driver reports "the mailbox is
# deaf". rmmod/insmod does not help. M157 named the likely cause (SIGKILLed
# encoders never run their cleanup) but that is a way to stop reaching the
# wedge, not a way out of one. This script walks the escalating PCI-level
# resets the kernel offers for the device and tests the mailbox after each:
#
#   1. pm  reset  - D3hot -> D0 power-state bounce
#   2. bus reset  - secondary-bus (hot) reset on the upstream bridge;
#                   if the slot's PERST# is routed into the SoC's reset
#                   tree this reboots the card's ARM Linux
#   3. remove + rescan - full re-enumeration on top of a bus reset
#
# Verdict per stage comes from reloading the module and looking for the
# mailbox answering (CMD_INIT ok / fw READY / "already runs firmware").
#
# Decision table:
#   - "RECOVERED after <stage>"  -> cold boot NOT needed; use this stage
#     as the standard recovery (and consider automating it in the driver).
#   - all three fail             -> the SoC really does keep running through
#     PCIe resets; mains-off cold boot is the only recovery.
#
# Only meaningful ON A WEDGED CARD. Run the 2 s check first (M149) - if
# CMD_INIT answers, there is nothing here to recover.
set -u
cd "$(dirname "$0")/.."

. scripts/mz0380-build.sh
KO=$(mz0380_resolve_module) || exit 1

PCI=${PCI_ADDR:-0000:04:00.0}
SYS=/sys/bus/pci/devices/$PCI

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
[ -e "$SYS" ] || { echo "no device at $PCI"; exit 1; }

# Without these, insmod fails with "Unknown symbol in module" - a missing
# dependency, which says nothing about the card. M149.
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm

unload() {
	fuser -k /dev/video0 2>/dev/null
	rmmod mz0380 2>/dev/null
	sleep 1
}

# Load the module and report whether the card's mailbox answered.
#
# The markers are the ones mz0380_card_init() actually prints (M149):
#   healthy: CMD_INIT answered on attempt 1 (status=0xdddddddd)
#   wedged:  CMD_INIT got no answer (-110) ... the mailbox is deaf
# The handshake resolves in ~2 s either way - the card boots its own flash and
# we never upload anything, so there is no 25 s upload to wait out.
mailbox_alive() {
	dmesg -C
	insmod "$KO" dma_handshake=1 enable_dma=1 \
		enable_video=1 procfs_verbosity=2 dma_iova_remap=1 || return 1
	for i in $(seq 15); do
		if dmesg | grep -qaE "CMD_INIT answered on attempt"; then
			return 0
		fi
		if dmesg | grep -qaE "CMD_INIT got no answer|mailbox is deaf|card handshake failed"; then
			return 1
		fi
		sleep 1
	done
	# no explicit marker either way - fall back to the state file
	grep -qi "state *: *ready" /proc/mz0380-state 2>/dev/null
}

try_stage() {
	local name="$1"; shift
	echo "=== stage: $name ==="
	unload
	"$@" || { echo "  ($name action failed)"; return 1; }
	sleep 2
	if mailbox_alive; then
		echo "RECOVERED after $name - card mailbox answers again"
		dmesg | grep -E "mz0380" | tail -5
		exit 0
	fi
	echo "  $name: still wedged"
	return 1
}

echo "reset methods offered: $(cat "$SYS"/reset_method 2>/dev/null)"

try_stage "pm-reset"  sh -c "echo pm  > '$SYS/reset_method' && echo 1 > '$SYS/reset'"
try_stage "bus-reset" sh -c "echo bus > '$SYS/reset_method' && echo 1 > '$SYS/reset'"
try_stage "remove+rescan" sh -c "echo 1 > '$SYS/remove'; sleep 2; echo 1 > /sys/bus/pci/rescan; sleep 3"

echo "ALL PCI RESETS FAILED - the SoC survives PCIe resets; mains-off cold boot is the only recovery"
exit 1
