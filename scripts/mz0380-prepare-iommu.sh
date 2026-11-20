#!/usr/bin/env bash
# Give one HD60 Pro a translating IOMMU domain on a system booted with
# iommu=pt, without changing the system-wide default.
#
# Why this exists
# ---------------
# The card latches only the high 32 bits of a DMA target, so the driver places
# every buffer at its own 4 GiB boundary through the IOMMU and refuses to
# stream in a pass-through (identity) domain. Linux lets one IOMMU group be
# switched to translated DMA while its device has no driver bound, which is
# enough: nothing else on the machine has to give up iommu=pt.
#
#   sudo modprobe -r mz0380                       # the card must be unbound
#   sudo scripts/mz0380-prepare-iommu.sh 0000:04:00.0
#   sudo modprobe mz0380
#
# Find the address with `lspci -D -d 12ab:`.
#
# What it does: checks, then writes "DMA" to the group's `type` attribute if it
# reads "identity". What it never does: load or unload a driver, reset the
# device, change its power state (the setpci calls below only read), or persist
# anything - the setting is gone after a reboot, so run it before each first
# load.
#
# It refuses a device that is bound, shares its IOMMU group, has bus mastering
# on, or is not in D0. The checks are not a lock: do not bind a driver or start
# a passthrough operation on the device while it runs.
set -euo pipefail

die() { echo "$*" >&2; exit 1; }

if [[ $EUID != 0 || $# != 1 ]]; then
    echo "Usage: sudo bash $0 <PCI-address>   (e.g. 0000:04:00.0)" >&2
    exit 1
fi
bdf=$1
[[ $bdf =~ ^[[:xdigit:]]{4}:[[:xdigit:]]{2}:[[:xdigit:]]{2}\.[0-7]$ ]] ||
    die "'$bdf' is not a full PCI address (domain:bus:device.function)."
dev=/sys/bus/pci/devices/$bdf
[[ -d $dev ]] || die "No PCI device at $bdf."
[[ $(<"$dev/vendor") == 0x12ab ]] || die "$bdf is not an HD60 Pro (vendor is not 12ab)."
case $(<"$dev/device") in
    0x0380|0x0381) ;;
    *) die "$bdf is not an HD60 Pro (device is not 0380 or 0381)." ;;
esac
[[ ! -L $dev/driver ]] || die 'Device is bound; refusing. Unload the driver first: modprobe -r mz0380'
case $(<"$dev/power_state") in
    D0|unknown) ;;
    *) die 'Device is not active; refusing a power transition.' ;;
esac
# Linux marks its cached state unknown after driver removal. Read hardware;
# this setpci invocation has no assignment and cannot change power state.
command -v setpci >/dev/null || die 'setpci not found; install pciutils.'
pmcsr=$(setpci -s "$bdf" CAP_PM+4.w)
[[ $pmcsr =~ ^[[:xdigit:]]{4}$ ]] || die "Could not read the power-management register (got '$pmcsr')."
(( (16#$pmcsr & 3) == 0 )) || die 'Hardware is not D0.'
command_word=$(setpci -s "$bdf" COMMAND.w)
[[ $command_word =~ ^[[:xdigit:]]{4}$ ]] || die "Could not read the PCI command register (got '$command_word')."
(( (16#$command_word & 4) == 0 )) || die 'Bus mastering is active; refusing.'
[[ -e $dev/iommu_group ]] ||
    die 'The device has no IOMMU group: enable VT-d/AMD-Vi in the firmware setup and boot without iommu=off.'
group=$(readlink -f "$dev/iommu_group")
[[ $group == /sys/kernel/iommu_groups/* ]] || die "Unexpected IOMMU group path '$group'."
members=("$group"/devices/*)
[[ ${#members[@]} == 1 && ${members[0]##*/} == "$bdf" ]] ||
    die 'Device must be alone in its IOMMU group.'
case $(<"$group/type") in
    identity) printf 'DMA\n' > "$group/type" ;;
    DMA|DMA-FQ) ;;
    *) die "Unexpected domain type '$(<"$group/type")'; refusing." ;;
esac
case $(<"$group/type") in
    DMA|DMA-FQ) ;;
    *) die "The kernel did not accept the change; the domain type is still '$(<"$group/type")'." ;;
esac
echo 'Translated domain ready. No driver was loaded or PCI reset requested.'
