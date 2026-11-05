#!/bin/bash
# Configure only an isolated, unbound HD60 Pro's default IOMMU domain.
set -euo pipefail

if [[ $EUID != 0 || $# != 1 ]]; then
    echo "Usage: sudo bash $0 <PCI-address>" >&2
    exit 1
fi
bdf=$1
[[ $bdf =~ ^[[:xdigit:]]{4}:[[:xdigit:]]{2}:[[:xdigit:]]{2}\.[0-7]$ ]] || exit 1
dev=/sys/bus/pci/devices/$bdf
[[ -d $dev ]] || exit 1
[[ $(<"$dev/vendor") == 0x12ab ]] || exit 1
case $(<"$dev/device") in 0x0380|0x0381) ;; *) exit 1 ;; esac
[[ ! -L $dev/driver ]] || { echo 'Device is bound; refusing.' >&2; exit 1; }
case $(<"$dev/power_state") in
    D0|unknown) ;;
    *) echo 'Device is not active; refusing a power transition.' >&2; exit 1 ;;
esac
# Linux marks its cached state unknown after driver removal. Read hardware;
# this setpci invocation has no assignment and cannot change power state.
pmcsr=$(setpci -s "$bdf" CAP_PM+4.w)
[[ $pmcsr =~ ^[[:xdigit:]]{4}$ ]] || exit 1
(( (16#$pmcsr & 3) == 0 )) || { echo 'Hardware is not D0.' >&2; exit 1; }
command_word=$(setpci -s "$bdf" COMMAND.w)
[[ $command_word =~ ^[[:xdigit:]]{4}$ ]] || exit 1
(( (16#$command_word & 4) == 0 )) || { echo 'Bus mastering is active; refusing.' >&2; exit 1; }
group=$(readlink -f "$dev/iommu_group")
[[ $group == /sys/kernel/iommu_groups/* ]] || exit 1
members=("$group"/devices/*)
[[ ${#members[@]} == 1 && ${members[0]##*/} == "$bdf" ]] || {
    echo 'Device must be alone in its IOMMU group.' >&2; exit 1;
}
case $(<"$group/type") in
    identity) printf 'DMA\n' > "$group/type" ;;
    DMA|DMA-FQ) ;;
    *) echo 'Unexpected domain type; refusing.' >&2; exit 1 ;;
esac
case $(<"$group/type") in DMA|DMA-FQ) ;; *) exit 1 ;; esac
echo 'Translated domain ready. No driver was loaded or PCI reset requested.'
