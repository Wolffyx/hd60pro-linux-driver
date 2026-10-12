#!/bin/bash
# M85: load/unload smoke test. Run this after EVERY build, before any hardware
# run. It costs zero encoder spawns and takes a few seconds.
#
# Why this exists
# ---------------
# M84's INTx switch wedged the machine. Not because INTx is wrong - it is what
# the Windows driver uses - but because the driver had a latent teardown-order
# bug that only a SHARED interrupt handler can reach:
#
#   mz0380_finidev()  unmapped both BARs (mz0380_dev_unregister)
#                     and only THEN called free_irq (mz0380_irq_release)
#
# Under MSI that window is unreachable: an MSI source stops signalling once the
# device is quiesced, so the handler is never entered after the unmap. Under a
# shared line it is reachable, and with CONFIG_DEBUG_SHIRQ=y (set on this
# kernel) it is not merely reachable but GUARANTEED - free_irq() deliberately
# calls the handler one last time, in process context, precisely to catch
# handlers that still touch freed resources. Ours read BAR0+0x30 through a
# NULL bmmio and oopsed inside rmmod with IRQs disabled, which leaves the
# module in MODULE_STATE_GOING (refcnt -1). Nothing removes it after that:
# delete_module() rejects any module whose state != MODULE_STATE_LIVE *before*
# it looks at the force flag, so even `rmmod -f` returns EBUSY. Only a reboot
# clears it - and because finidev aborted, dma_teardown/pci_clear_master/
# pci_disable_device never ran either, so it wants a full power-off.
#
# The whole failure is detectable in about five seconds without touching the
# encoder, which is what this script does.
#
# What it exercises
# -----------------
# A full probe - card handshake, request_irq (so free_irq and its DEBUG_SHIRQ
# callback run on the way out), DMA windows, receiver bring-up - with
# enable_video=0, so no V4L2 node appears for the desktop to open and pin the
# module. Nothing is ever uploaded - the card runs its own flash image. No
# STREAMON, no SET_VIC, no encoder spawn.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

fail() { echo; echo "SMOKE FAILED: $*"; echo "Do NOT run a hardware test until this is clean."; exit 1; }

# A module already stuck in MODULE_STATE_GOING cannot be removed or replaced.
state=$(cat /sys/module/mz0380/initstate 2>/dev/null || true)
if [ "$state" = "going" ]; then
	fail "mz0380 is already wedged in MODULE_STATE_GOING (refcnt $(cat /sys/module/mz0380/refcnt 2>/dev/null)). Power-cycle the machine; rmmod -f will not help."
fi

. scripts/mz0380-build.sh
MZKO=$(mz0380_resolve_module) || fail "build failed"
rmmod mz0380 2>/dev/null
sleep 1
modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc v4l2-dv-timings snd-pcm
dmesg -C

echo "=== load (no V4L2 node; the IRQ is requested, which is the point) ==="
insmod "$MZKO" enable_video=0 || fail "insmod failed"

sleep 2
dmesg | grep -E "mz0380" | tail -3

echo
echo "=== unload (CONFIG_DEBUG_SHIRQ makes free_irq re-enter our ISR here) ==="
rmmod mz0380 || fail "rmmod returned non-zero"
sleep 1

echo
echo "=== checking for oops / BUG / WARN in the cycle ==="
if dmesg | grep -qE "BUG:|Oops|kernel NULL pointer|Call Trace|WARNING:|general protection"; then
	dmesg | grep -E -A 5 "BUG:|Oops|kernel NULL pointer|Call Trace|WARNING:|general protection" | head -40
	fail "kernel complained during load/unload"
fi

# The decisive check: a module that oopsed on the way out is still listed.
if lsmod | grep -qE "^mz0380"; then
	fail "module still present after rmmod: $(lsmod | grep '^mz0380')"
fi

echo "clean: loaded, bound, unloaded, no kernel complaints, module gone."
