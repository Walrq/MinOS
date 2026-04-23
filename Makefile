# MinOS Build System — Incremental Makefile
# Targets: all  kernel  init  daemons  image  test  verify  clean
#
# Incremental: only recompiles what changed.
#   - Recompile a daemon  → only when its .c sources change
#   - Repack squashfs     → only when rootfs/ contents change
#   - Reassemble disk     → only when squashfs/kernel/initramfs change
#   - Stage-1 initramfs   → only when initramfs-stage1/ scripts change

MINOS_DIR := $(shell pwd)
ROOTFS    := $(MINOS_DIR)/rootfs

CC     := gcc
CFLAGS := -static -O2

# ── Source files ──────────────────────────────────────────────────────────────
INIT_SRCS  := $(wildcard init/*.c)
SUP_SRCS   := $(filter-out supervisor/supervisor.c, $(wildcard supervisor/*.c))
CGMGR_SRCS := $(wildcard cgroup/*.c)
NETD_SRCS  := $(wildcard netd/*.c)
MINC_SRCS  := $(wildcard runtime/*.c)

# ── Output binaries (inside rootfs/) ─────────────────────────────────────────
INIT_BIN      := $(ROOTFS)/sbin/init
SUP_BIN       := $(ROOTFS)/supervisor
CGMGR_BIN     := $(ROOTFS)/services/cgmgr
NETD_BIN      := $(ROOTFS)/services/netd
MINC_BIN      := $(ROOTFS)/bin/minc
CGINSPECT_BIN := $(ROOTFS)/bin/cginspect
NSLIST_BIN    := $(ROOTFS)/bin/nslist
MEMHOG_BIN    := $(ROOTFS)/bin/memhog
DEMO_BIN      := $(ROOTFS)/bin/demo
SYSMON_BIN    := $(ROOTFS)/bin/sysmon
PYRUN_BIN     := $(ROOTFS)/bin/pyrun

# ── Config dependencies ───────────────────────────────────────────────────────
CONFIGS := $(wildcard services.d/cgmgr.conf services.d/netd.conf) \
           $(wildcard netd/net.conf cgroup/slices.conf)
BUSYBOX := $(shell which busybox 2>/dev/null)

# ─────────────────────────────────────────────────────────────────────────────
.PHONY: all kernel init daemons tools image test verify clean info

all: init daemons tools image

# ── Kernel ────────────────────────────────────────────────────────────────────
kernel: kernel/bzImage

kernel/bzImage:
	@echo "ERROR: kernel/bzImage not found."
	@echo "Build the kernel first: cd kernel && make bzImage -j$$(nproc)"
	@exit 1

# ── Rootfs skeleton: dirs + busybox + static configs ─────────────────────────
# Stamp file .stamps/rootfs-setup — only reruns when busybox or configs change.
# NOTE: target is hardcoded (no variable expansion) to avoid empty-var issues.
.stamps/rootfs-setup: $(BUSYBOX) $(CONFIGS)
	@mkdir -p .stamps
	@echo "━━━ [setup] rootfs skeleton + busybox + configs ━━━"
	@mkdir -p $(ROOTFS)/sbin $(ROOTFS)/bin $(ROOTFS)/services \
	           $(ROOTFS)/services.d $(ROOTFS)/etc $(ROOTFS)/dev \
	           $(ROOTFS)/proc $(ROOTFS)/sys $(ROOTFS)/tmp \
	           $(ROOTFS)/run $(ROOTFS)/containers $(ROOTFS)/mnt
	@if [ -n "$(BUSYBOX)" ]; then \
	    cp "$(BUSYBOX)" $(ROOTFS)/bin/busybox && chmod +x $(ROOTFS)/bin/busybox; \
	    for t in sh ash ls cat echo ps kill grep sed awk ip \
	              mount umount mkdir rm cp mv ln chmod chown \
	              head tail wc cut sort uniq find \
	              reboot halt poweroff sync free df dmesg mknod; do \
	        ln -sf busybox $(ROOTFS)/bin/$$t; \
	    done; \
	    echo "  [+] busybox installed"; \
	else \
	    echo "  [!] WARNING: busybox not found"; \
	fi
	@[ -f services.d/cgmgr.conf ] && cp services.d/cgmgr.conf $(ROOTFS)/services.d/ || true
	@[ -f services.d/netd.conf  ] && cp services.d/netd.conf  $(ROOTFS)/services.d/ || true
	@printf 'name=console\nbin=/bin/sh\nrestart=always\n' \
	    > $(ROOTFS)/services.d/console.conf
	@[ -f netd/net.conf      ] && cp netd/net.conf      $(ROOTFS)/etc/net.conf  || true
	@[ -f cgroup/slices.conf ] && cp cgroup/slices.conf $(ROOTFS)/slices.conf   || true
	@touch .stamps/rootfs-setup
	@echo "  [+] configs installed"

# ── Compile: init ─────────────────────────────────────────────────────────────
$(INIT_BIN): $(INIT_SRCS) | .stamps/rootfs-setup
	@echo "━━━ [CC] init ━━━"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^
	@chmod +x $@ && echo "  → $@"

# ── Compile: supervisor ───────────────────────────────────────────────────────
$(SUP_BIN): $(SUP_SRCS) | .stamps/rootfs-setup
	@echo "━━━ [CC] supervisor ━━━"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^
	@chmod +x $@ && echo "  → $@"

# ── Compile: cgmgr ───────────────────────────────────────────────────────────
$(CGMGR_BIN): $(CGMGR_SRCS) | .stamps/rootfs-setup
	@echo "━━━ [CC] cgmgr ━━━"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^
	@chmod +x $@ && echo "  → $@"

# ── Compile: netd ─────────────────────────────────────────────────────────────
$(NETD_BIN): $(NETD_SRCS) | .stamps/rootfs-setup
	@echo "━━━ [CC] netd ━━━"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^
	@chmod +x $@ && echo "  → $@"

# ── Compile: minc (container runtime) ────────────────────────────────────────
$(MINC_BIN): $(MINC_SRCS) | .stamps/rootfs-setup
	@echo "━━━ [CC] minc ━━━"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^
	@chmod +x $@ && echo "  → $@"

# ── Compile: cginspect (cgroup inspector) ─────────────────────────────────────
$(CGINSPECT_BIN): tools/cginspect.c | .stamps/rootfs-setup
	@echo "━━━ [CC] cginspect ━━━"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $<
	@chmod +x $@ && echo "  → $@"

# ── Compile: nslist (namespace inspector) ─────────────────────────────────────
$(NSLIST_BIN): tools/nslist.c | .stamps/rootfs-setup
	@echo "━━━ [CC] nslist ━━━"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $<
	@chmod +x $@ && echo "  → $@"

# ── Compile: memhog (memory eater) ─────────────────────────────────────
$(MEMHOG_BIN): tools/memhog.c | .stamps/rootfs-setup
	@echo "━━━ [CC] memhog ━━━"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $<
	@chmod +x $@ && echo "  → $@"

# ── Copy: demo.sh ─────────────────────────────────────────────────────────────
$(DEMO_BIN): tools/demo.sh | .stamps/rootfs-setup
	@echo "━━━ [CP] demo.sh ━━━"
	@mkdir -p $(@D)
	cp $< $@
	@chmod +x $@ && echo "  → $@"

# ── Compile: sysmon (live container dashboard) ─────────────────────────────────
$(SYSMON_BIN): projects/sysmon/sysmon.c | .stamps/rootfs-setup
	@echo "━━━ [CC] sysmon ━━━"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $<
	@chmod +x $@ && echo "  → $@"

# ── Compile: pyrun (container-gated Python runner) ──────────────────────────
$(PYRUN_BIN): projects/pyrun/pyrun.c | .stamps/rootfs-setup
	@echo "━━━ [CC] pyrun ━━━"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $<
	@chmod +x $@ && echo "  → $@"

# ── Named targets for compilation groups ─────────────────────────────────────
init:    $(INIT_BIN)
daemons: $(SUP_BIN) $(CGMGR_BIN) $(NETD_BIN) $(MINC_BIN)
tools:   $(CGINSPECT_BIN) $(NSLIST_BIN) $(MEMHOG_BIN) $(DEMO_BIN) $(SYSMON_BIN) $(PYRUN_BIN)

# ── Stage-1 initramfs ─────────────────────────────────────────────────────────
initramfs-stage1.cpio: $(wildcard initramfs-stage1/*) $(BUSYBOX)
	@echo "━━━ [stage1] packing initramfs ━━━"
	bash build/mkstage1.sh

# ── Squashfs OS root ──────────────────────────────────────────────────────────
os-root.squashfs: $(INIT_BIN) $(SUP_BIN) $(CGMGR_BIN) $(NETD_BIN) $(MINC_BIN) \
                   $(CGINSPECT_BIN) $(NSLIST_BIN) $(MEMHOG_BIN) \
                   $(DEMO_BIN) $(SYSMON_BIN) $(PYRUN_BIN) \
                   .stamps/rootfs-setup
	@echo "━━━ [squashfs] packing OS root ━━━"
	bash build/mksquashfs.sh
	@echo "  → os-root.squashfs ($$(du -sh os-root.squashfs | cut -f1))"

# ── Disk image ────────────────────────────────────────────────────────────────
myos.img: os-root.squashfs kernel/bzImage initramfs-stage1.cpio
	@echo "━━━ [disk] assembling myos.img ━━━"
	bash build/mkdisk.sh
	@echo "  → myos.img ($$(du -sh myos.img | cut -f1))"

image: myos.img

# ── Test: boot in QEMU (interactive) ────────────────────────────────────────
test: myos.img
	@echo "━━━ [test] booting MinOS ━━━"
	@echo "  Ctrl+] to detach console  |  killall qemu-system-x86_64 to quit"
	bash build/qemu.sh

# ── Boot-log: boot headless and capture serial output to build/boot.log ──────
# Runs QEMU with -serial stdio (no socat) and saves output. Non-interactive.
boot-log: myos.img
	@echo "━━━ [boot-log] booting headless → build/boot.log ━━━"
	qemu-system-x86_64 \
	    -drive file=myos.img,format=raw,if=virtio,cache=unsafe \
	    -m 256M -smp 2 \
	    $(shell [ -e /dev/kvm ] && echo '-enable-kvm -cpu host' || echo '-cpu qemu64') \
	    -rtc clock=host -nographic \
	    -serial stdio -no-reboot \
	    2>&1 | tee build/boot.log

# ── Verify: run the verification suite ───────────────────────────────────────
verify: myos.img
	bash build/verify.sh

# ── Clean: remove all generated files ────────────────────────────────────────
clean:
	@echo "Cleaning..."
	rm -rf $(ROOTFS) os-root.squashfs myos.img initramfs-stage1.cpio \
	       build/boot.log .stamps
	find . \( -name '*.o' -o -name '*.d' \) -delete
	@echo "Done."

# ── Show build info ───────────────────────────────────────────────────────────
info:
	@echo "MinOS Build System"
	@echo "  CC        = $(CC)  CFLAGS = $(CFLAGS)"
	@echo "  ROOTFS    = $(ROOTFS)"
	@echo "  BUSYBOX   = $(BUSYBOX)"
	@echo ""
	@echo "  Source files:"
	@echo "    init    : $(words $(INIT_SRCS))  files: $(INIT_SRCS)"
	@echo "    sup     : $(words $(SUP_SRCS))  files: $(SUP_SRCS)"
	@echo "    cgmgr   : $(words $(CGMGR_SRCS))  files: $(CGMGR_SRCS)"
	@echo "    netd    : $(words $(NETD_SRCS))  files: $(NETD_SRCS)"
	@echo "    minc    : $(words $(MINC_SRCS))  files: $(MINC_SRCS)"
	@echo ""
	@echo "  Targets: all  kernel  init  daemons  image  test  verify  clean"
