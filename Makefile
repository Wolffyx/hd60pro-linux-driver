#
# Linux driver for Elgato Game Capture HD60 Pro (YUAN MZ0380).
#
# Builds mz0380.ko. This driver is written for the HD60 Pro (MZ0380) and
# shares no source with any other capture driver; see README's Provenance
# section for what was consulted during reverse engineering and what was not.
#

mz0380-objs := \
	src/mz0380-pci.o \
	src/mz0380-params.o \
	src/mz0380-cards.o \
	src/mz0380-fw.o \
	src/mz0380-mailbox.o \
	src/mz0380-proc.o \
	src/mz0380-video.o \
	src/mz0380-vb2.o \
	src/mz0380-signal.o \
	src/mz0380-no-signal.o \
	src/mz0380-mst3367.o \
	src/mz0380-mst3367-signal.o \
	src/mz0380-mst3367-debug.o \
	src/mz0380-dma.o \
	src/mz0380-dma-stream.o \
	src/mz0380-dma-drain.o \
	src/mz0380-dma-raw.o \
	src/mz0380-audio.o


ccflags-y += -I$(src)/src

obj-m += mz0380.o

#
# Kernel feature probes (kbuild pass only - $(srctree) is the kernel tree).
#
# Probe the headers instead of testing LINUX_VERSION_CODE. The exact release
# that dropped a symbol is rarely worth guessing, and a wrong guess produces a
# module that builds but misbehaves, which is far more expensive to diagnose
# than a build error.
#
ifneq ($(KERNELRELEASE),)

# vb2_ops->wait_prepare/wait_finish and the vb2_ops_wait_* helpers exist up to
# 6.x. vb2 core took the q->lock handling over and they were removed; present
# in 6.18, gone in 7.2.
MZ0380_HAVE_VB2_WAIT_OPS := $(shell grep -sqw vb2_ops_wait_prepare \
	$(srctree)/include/media/videobuf2-v4l2.h && echo 1)
ifeq ($(MZ0380_HAVE_VB2_WAIT_OPS),1)
ccflags-y += -DMZ0380_HAVE_VB2_WAIT_OPS
endif

# Linux 7.2 split the old system_wq into explicit percpu/default-unbound
# queues and warns whenever new work is queued on the compatibility alias.
# Probe the declaration so the same source keeps building on older kernels.
MZ0380_HAVE_SYSTEM_DFL_WQ := $(shell grep -sqw system_dfl_wq \
	$(srctree)/include/linux/workqueue.h && echo 1)
ifeq ($(MZ0380_HAVE_SYSTEM_DFL_WQ),1)
ccflags-y += -DMZ0380_HAVE_SYSTEM_DFL_WQ
endif

endif

.PHONY: all all-kernels kernels kcheck clean objclean distclean fw-install \
        load unload tarball list install uninstall dkms-install dkms-uninstall

TARFILES = Makefile dkms.conf mz0380.modprobe.conf COPYING *.md *.sh src scripts

#
# Which kernel to build against.
#
# Defaults to the running kernel, which is the only one whose module can
# actually be insmod'd, so the scripts' plain `make` stays correct. Override
# to cross-build for another installed kernel:
#
#     make KVER=6.18.42-1-cachyos-lts
#     make all-kernels          # every installed kernel that has headers
#
KVER     ?= $(shell uname -r)
KVERSION  = $(KVER)
KBUILD    = /lib/modules/$(KVER)/build

# Installed kernels that can actually be built against, newest first.
KVERS_BUILDABLE = $(shell for d in /lib/modules/*/; do \
	k=$${d%/}; k=$${k##*/}; \
	[ -e "$$d/build/Makefile" ] && echo "$$k"; \
	done | sort -rV)

#
# Toolchain: build with the compiler that built the TARGET kernel.
#
# A module has to be compiled by the same compiler family as its kernel - gcc
# rejects a clang kernel's flags (-mretpoline-external-thunk and friends), and
# clang fails on a gcc kernel's. Picking clang whenever it was installed broke
# the gcc-built LTS kernel on a machine whose default kernel is clang-built, so
# ask the kernel instead: its auto.conf records CONFIG_CC_IS_CLANG. Pass
# KBUILD_FLAGS on the command line to override.
#
KCONFIG_AUTO = $(KBUILD)/include/config/auto.conf
KCC_CLANG   := $(shell grep -qsx 'CONFIG_CC_IS_CLANG=y' $(KCONFIG_AUTO) && echo y)
ifeq ($(KCC_CLANG),y)
KBUILD_FLAGS ?= CC=clang LD=ld.lld
else
KBUILD_FLAGS ?=
endif

#
# M=$(CURDIR), never M=$(PWD). PWD is an environment variable, and sudo's
# env_reset drops it - so `sudo make install` passed kbuild an empty M=, which
# it reads as "build the kernel itself" and answers with a syncconfig failure
# about arch/arm/crypto/Kconfig. The same command works unprivileged, because
# there the shell had exported PWD. CURDIR is set by make itself and survives
# both.
#
all: kcheck
	$(MAKE) -C $(KBUILD) M=$(CURDIR) $(KBUILD_FLAGS) modules

#
# Fail with a usable message instead of kbuild's bare "No such file or
# directory". A running kernel whose package has been upgraded away leaves no
# /lib/modules entry at all, so nothing out-of-tree can be built OR loaded on
# that boot - the only fixes are a reboot into an installed kernel or a
# downgrade, and the error has to say so.
#
kcheck:
	@if [ ! -e "$(KBUILD)/Makefile" ]; then \
		echo "ERROR: no kernel build tree for '$(KVER)'"; \
		echo "       ($(KBUILD) is missing)"; \
		if [ "$(KVER)" = "$$(uname -r)" ]; then \
			echo; \
			echo "  The RUNNING kernel has no /lib/modules entry - its package was"; \
			echo "  upgraded or removed. No out-of-tree module can be built or loaded"; \
			echo "  on this boot. Reboot into one of the kernels below, or reinstall"; \
			echo "  the running kernel's headers."; \
		fi; \
		echo; \
		echo "  buildable kernels: $(KVERS_BUILDABLE)"; \
		echo "  cross-build with:  make KVER=<version>"; \
		exit 1; \
	fi
	@if [ "$(KCC_CLANG)" = y ] && ! { command -v clang && command -v ld.lld; } >/dev/null; then \
		echo "ERROR: kernel '$(KVER)' was built with clang, so this module must be"; \
		echo "       too - install clang and lld (Arch: pacman -S clang lld)."; \
		exit 1; \
	fi

#
# Build for every installed kernel that has headers, keeping one
# $(KO_DIR)/mz0380-<kver>.ko per kernel.
#
# The copies live in a subdirectory, and the loop uses objclean rather than
# `make clean`: kbuild's clean deletes *.ko RECURSIVELY under M=, so both
# keeping the copies alongside and keeping them in $(KO_DIR) lose the previous
# kernel's module to the next kernel's clean.
#
# The running kernel is built LAST so the plain mz0380.ko that the load scripts
# insmod is the one matching this boot.
#
KO_DIR = ko

all-kernels:
	@set -e; \
	mkdir -p $(KO_DIR); \
	running="$$(uname -r)"; \
	others=""; mine=""; \
	for k in $(KVERS_BUILDABLE); do \
		if [ "$$k" = "$$running" ]; then mine="$$k"; else others="$$others $$k"; fi; \
	done; \
	[ -n "$$others$$mine" ] || { echo "no buildable kernel found"; exit 1; }; \
	for k in $$others $$mine; do \
		echo "=== $$k ==="; \
		$(MAKE) --no-print-directory objclean >/dev/null; \
		$(MAKE) --no-print-directory KVER=$$k all; \
		cp mz0380.ko $(KO_DIR)/mz0380-$$k.ko; \
		echo "    -> $(KO_DIR)/mz0380-$$k.ko"; \
	done; \
	if [ -z "$$mine" ]; then \
		rm -f mz0380.ko; \
		echo; \
		echo "NOTE: running kernel '$$running' has no build tree, so mz0380.ko was"; \
		echo "      removed - a module with the wrong vermagic only fails at insmod"; \
		echo "      with 'Invalid module format', which reads like a driver bug."; \
		echo "      Reboot into a built kernel and run make; the per-kernel copies"; \
		echo "      in $(KO_DIR)/ are kept either way."; \
	fi

kernels:
	@echo "running:   $$(uname -r)"
	@echo "buildable: $(KVERS_BUILDABLE)"
	@echo "default:   $(KVER)"

# Drop just enough build state to force a full rebuild against a different
# kernel, without kbuild's recursive walk. Top level only, and $(KO_DIR) is
# untouched by construction.
objclean:
	@rm -f *.o .*.o *.mod *.mod.c .*.cmd mz0380.ko \
		src/*.o src/.*.o src/.*.cmd \
		Module.symvers modules.order modules.builtin*
	@rm -rf .tmp_versions

# clean must work even when the running kernel has no build tree, otherwise
# the one command that unsticks a broken tree is itself unavailable.
clean:
	@if [ -e "$(KBUILD)/Makefile" ]; then \
		$(MAKE) -C $(KBUILD) M=$(CURDIR) $(KBUILD_FLAGS) clean; \
	else \
		echo "no build tree for '$(KVER)' - removing build products directly"; \
		rm -f *.o *.ko *.mod *.mod.c .*.cmd src/*.o src/.*.cmd \
			Module.symvers modules.order; \
		rm -rf .tmp_versions; \
	fi

# clean deliberately leaves $(KO_DIR) alone - all-kernels runs clean between
# kernels, so anything it removed there would not survive the next pass.
distclean: clean
	rm -rf $(KO_DIR)


#
# The card boots its own flash image; the driver never uploads firmware. The
# only file it ever asks for is MZ0380.FW.TXT, a 6-byte version sidecar read to
# compare versions - optional, and the comparison is skipped without it.
#
FW_DIR ?= /lib/firmware/mz0380
fw-install:
	@if [ ! -f $(FW_DIR)/MZ0380.FW.TXT ]; then \
		echo "note: $(FW_DIR)/MZ0380.FW.TXT is absent - not needed to run."; \
		echo "      To enable the firmware version check:"; \
		echo "          echo 01.11 | sudo tee $(FW_DIR)/MZ0380.FW.TXT"; \
	else \
		echo "firmware version sidecar: $(FW_DIR)/MZ0380.FW.TXT = $$(cat $(FW_DIR)/MZ0380.FW.TXT)"; \
	fi

#
# Development load/unload straight from the tree. For anything more,
# ./mz0380-live.sh does the same with a smoke test and a spawn tally.
#
load: all
	sudo modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc \
		v4l2-dv-timings snd-pcm
	sudo insmod ./mz0380.ko

unload:
	sudo rmmod mz0380 || true

tarball:
	tar zcf ../mz0380-$(VERSION)-$(shell date +%Y%m%d).tgz $(TARFILES)

list:
	v4l2-ctl --list-devices

#
# Installation.
#
# `make dkms-install` is the one to use: the module is rebuilt automatically on
# every kernel upgrade. `make install` drops the current build into one
# kernel's module tree, and the next kernel upgrade leaves it behind.
#
# Neither builds: run `make` as your user first, so nothing in the tree ends up
# owned by root.
#
# One source for the version: MZ0380_VERSION in src/mz0380.h
# (scripts/mz0380-version.sh sets it, and dkms.conf with it). Not in the
# kbuild pass: that runs from the kernel tree, which has its own VERSION.
ifeq ($(KERNELRELEASE),)
VERSION      = $(shell sed -n 's/^\#define MZ0380_VERSION[[:space:]]*"\(.*\)"$$/\1/p' src/mz0380.h)
endif
MODDIR       = /lib/modules/$(KVER)/kernel/drivers/media/pci/mz0380
MODPROBE_DIR = /etc/modprobe.d
DKMS_SRC     = /usr/src/mz0380-$(VERSION)

install:
	@[ -f mz0380.ko ] || { echo "build first, as your user: make"; exit 1; }
	@[ "$$(modinfo -F vermagic mz0380.ko | cut -d' ' -f1)" = "$(KVER)" ] || \
		{ echo "mz0380.ko was built for another kernel; run: make KVER=$(KVER)"; exit 1; }
	install -d $(DESTDIR)$(MODDIR)
	install -m 644 mz0380.ko $(DESTDIR)$(MODDIR)/
	install -d $(DESTDIR)$(MODPROBE_DIR)
	install -m 644 mz0380.modprobe.conf $(DESTDIR)$(MODPROBE_DIR)/mz0380.conf
	@[ -n "$(DESTDIR)" ] || depmod -a $(KVER)
	@echo
	@echo "installed for kernel $(KVER). It loads by itself when the card is"
	@echo "present (after a reboot), or now with:  sudo modprobe mz0380"
	@echo "This binds to ONE kernel; 'make dkms-install' survives upgrades."

uninstall:
	rm -f $(DESTDIR)$(MODDIR)/mz0380.ko
	rm -f $(DESTDIR)$(MODPROBE_DIR)/mz0380.conf
	-rmdir $(DESTDIR)$(MODDIR) 2>/dev/null
	@[ -n "$(DESTDIR)" ] || depmod -a $(KVER)
	@echo "uninstalled from kernel $(KVER)."

#
# DKMS. Copies the sources - and only the sources - to /usr/src, so the module
# is rebuilt on every kernel upgrade. Re-running it replaces an earlier install
# of the same version.
#
dkms-install: dkms.conf
	@command -v dkms >/dev/null || { echo "dkms is not installed"; exit 1; }
	-dkms remove -m mz0380 -v $(VERSION) --all >/dev/null 2>&1
	rm -rf $(DKMS_SRC)
	install -d $(DKMS_SRC)/src
	install -m 644 Makefile dkms.conf mz0380.modprobe.conf COPYING $(DKMS_SRC)/
	install -m 644 src/*.c src/*.h $(DKMS_SRC)/src/
	dkms add     -m mz0380 -v $(VERSION)
	dkms build   -m mz0380 -v $(VERSION)
	dkms install -m mz0380 -v $(VERSION)
	install -d $(MODPROBE_DIR)
	install -m 644 mz0380.modprobe.conf $(MODPROBE_DIR)/mz0380.conf
	@echo
	@echo "DKMS install complete; the module rebuilds on kernel upgrades."
	@echo "Load it now with:  sudo modprobe mz0380"

dkms-uninstall:
	-dkms remove -m mz0380 -v $(VERSION) --all
	rm -rf $(DKMS_SRC)
	rm -f $(MODPROBE_DIR)/mz0380.conf
	@echo "DKMS package removed."
