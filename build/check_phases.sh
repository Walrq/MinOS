#!/bin/bash
# =============================================================================
# MinOS Phase Verification Script
# Checks all 10 phases of the MinOS build roadmap.
# Run from: /home/anura/OS-final/MinOS
# Usage:    bash build/check_phases.sh [--verbose] [--phase N]
# =============================================================================

set -euo pipefail

# ── Colours ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

# ── Globals ───────────────────────────────────────────────────────────────────
VERBOSE=0
ONLY_PHASE=""
PASS=0
FAIL=0
WARN=0
RESULTS=()

ROOT="$(cd "$(dirname "$0")/.." && pwd)"   # MinOS root

# ── Argument parsing ──────────────────────────────────────────────────────────
for arg in "$@"; do
    case "$arg" in
        --verbose|-v) VERBOSE=1 ;;
        --phase=*) ONLY_PHASE="${arg#--phase=}" ;;
    esac
done

# ── Helpers ───────────────────────────────────────────────────────────────────
ok()   { echo -e "  ${GREEN}✓${RESET} $*"; ((PASS++));  RESULTS+=("PASS: $*"); }
fail() { echo -e "  ${RED}✗${RESET} $*"; ((FAIL++));  RESULTS+=("FAIL: $*"); }
warn() { echo -e "  ${YELLOW}⚠${RESET} $*"; ((WARN++));  RESULTS+=("WARN: $*"); }
info() { [[ $VERBOSE -eq 1 ]] && echo -e "  ${CYAN}»${RESET} $*" || true; }

check_file()  { [[ -f "$ROOT/$1" ]] && ok "File exists: $1" || fail "Missing file: $1"; }
check_dir()   { [[ -d "$ROOT/$1" ]] && ok "Dir exists: $1"  || fail "Missing dir: $1"; }
check_bin()   { [[ -x "$ROOT/$1" ]] && ok "Executable: $1"  || fail "Not executable / missing: $1"; }
check_cmd()   { command -v "$1" &>/dev/null && ok "Tool installed: $1" || fail "Tool not found: $1"; }
check_nonempty() {
    if [[ -f "$ROOT/$1" && -s "$ROOT/$1" ]]; then
        ok "Non-empty: $1"
    else
        fail "Missing or empty: $1"
    fi
}
check_static() {
    local f="$ROOT/$1"
    if [[ -f "$f" ]]; then
        if file "$f" | grep -q "statically linked"; then
            ok "Static binary: $1"
        else
            warn "Not statically linked: $1 ($(file "$f" | cut -d, -f2))"
        fi
    else
        fail "Binary missing (static check): $1"
    fi
}
check_elf() {
    local f="$ROOT/$1"
    if [[ -f "$f" ]]; then
        if file "$f" | grep -qE "ELF.*executable|ELF.*shared"; then
            ok "Valid ELF binary: $1"
        else
            warn "Not an ELF binary: $1 ($(file "$f" | cut -d: -f2))"
        fi
    else
        fail "Binary missing (ELF check): $1"
    fi
}
section() {
    local phase="$1"; shift
    echo ""
    echo -e "${BOLD}${BLUE}━━━ $phase ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
    echo -e "${BOLD}$*${RESET}"
}
skip_if_filtered() {
    local phase="$1"
    [[ -z "$ONLY_PHASE" ]] && return 0
    [[ "$ONLY_PHASE" == "$phase" ]] && return 0
    return 1
}

# =============================================================================
echo ""
echo -e "${BOLD}${CYAN}╔══════════════════════════════════════════════════════════════════╗${RESET}"
echo -e "${BOLD}${CYAN}║          MinOS Phase Verification Script                        ║${RESET}"
echo -e "${BOLD}${CYAN}║          Root: $ROOT${RESET}"
echo -e "${BOLD}${CYAN}╚══════════════════════════════════════════════════════════════════╝${RESET}"

# =============================================================================
# PHASE 1 — Dev Environment Setup
# =============================================================================
skip_if_filtered 1 && {
section "Phase 1" "🟢 Dev Environment Setup"

echo "  [Tools]"
check_cmd gcc
check_cmd g++
check_cmd make
check_cmd flex
check_cmd bison
check_cmd qemu-system-x86_64
check_cmd mksquashfs
check_cmd dd
check_cmd file

echo "  [Static toolchain / musl]"
# Write to a real .c file so gcc can detect the language (stdin has no extension)
echo 'int main(){return 0;}' > /tmp/_minos_test.c
if gcc -static -o /tmp/_minos_test_static /tmp/_minos_test.c 2>/dev/null; then
    if file /tmp/_minos_test_static | grep -q "statically linked"; then
        ok "gcc can produce static binaries"
    else
        warn "gcc -static didn't produce a statically linked binary"
    fi
    rm -f /tmp/_minos_test_static
else
    fail "gcc static compilation failed (install: sudo apt install libc6-dev)"
fi
rm -f /tmp/_minos_test.c

if command -v musl-gcc &>/dev/null; then
    ok "musl-gcc available: $(musl-gcc --version 2>&1 | head -1)"
else
    warn "musl-gcc not found (install: sudo apt install musl-tools)"
fi

echo "  [QEMU version]"
qemu_ver=$(qemu-system-x86_64 --version 2>/dev/null | head -1)
info "$qemu_ver"
[[ -n "$qemu_ver" ]] && ok "QEMU version detected: $qemu_ver" || fail "QEMU version check failed"

echo "  [Project structure]"
check_dir "."
check_file "Makefile"
check_dir "kernel"
check_dir "init"
check_dir "supervisor"
check_dir "runtime"
check_dir "netd"
check_dir "cgroup"
check_dir "tools"
check_dir "build"
check_dir "rootfs"
check_dir "docs"
}

# =============================================================================
# PHASE 2 — Kernel Configuration & Build
# =============================================================================
skip_if_filtered 2 && {
section "Phase 2" "🔵 Kernel Configuration & Build"

check_file "kernel/bzImage"
check_nonempty "kernel/bzImage"
check_file "kernel/config"
check_nonempty "kernel/config"
check_file "kernel/build.sh"

echo "  [bzImage size check (must be > 1 MB)]"
if [[ -f "$ROOT/kernel/bzImage" ]]; then
    sz=$(stat -c%s "$ROOT/kernel/bzImage" 2>/dev/null || echo 0)
    if [[ $sz -gt 1048576 ]]; then
        ok "bzImage size: $(( sz / 1024 / 1024 )) MB"
    else
        fail "bzImage is suspiciously small: ${sz} bytes"
    fi
fi

echo "  [Kernel config feature checks]"
KCFG="$ROOT/kernel/config"
check_kconfig() {
    local key="$1" desc="$2"
    if grep -q "^${key}=y" "$KCFG" 2>/dev/null || grep -q "^${key}=m" "$KCFG" 2>/dev/null; then
        ok "Kernel feature enabled: $desc ($key)"
    elif grep -q "^# ${key} is not set" "$KCFG" 2>/dev/null; then
        fail "Kernel feature DISABLED: $desc ($key)"
    else
        warn "Kernel feature not found in config: $desc ($key)"
    fi
}

check_kconfig CONFIG_CGROUPS             "cgroups"
check_kconfig CONFIG_CGROUP_V2           "cgroup v2 (unified hierarchy)"  || \
check_kconfig CONFIG_CGROUP_BPF          "cgroup v2 (BPF alt check)"
check_kconfig CONFIG_NAMESPACES          "namespaces"
check_kconfig CONFIG_PID_NS              "PID namespaces"
check_kconfig CONFIG_NET_NS              "network namespaces"
check_kconfig CONFIG_UTS_NS              "UTS namespaces"
check_kconfig CONFIG_OVERLAY_FS          "overlayfs"
check_kconfig CONFIG_SECCOMP             "seccomp"
check_kconfig CONFIG_SECCOMP_FILTER      "seccomp BPF filter"
check_kconfig CONFIG_VIRTIO              "virtio"
check_kconfig CONFIG_VIRTIO_NET          "virtio networking"
check_kconfig CONFIG_VIRTIO_BLK          "virtio block device"
}

# =============================================================================
# PHASE 3 — Init (PID 1)
# =============================================================================
skip_if_filtered 3 && {
section "Phase 3" "🟣 Init (PID 1)"

echo "  [Source files]"
check_file "init/main.c"
check_file "init/mount.c"
check_file "init/signal.c"
check_file "init/reaper.c"

echo "  [Source content checks]"
grep_src() {
    local file="$ROOT/$1" pattern="$2" desc="$3"
    if grep -q "$pattern" "$file" 2>/dev/null; then
        ok "Found in $1: $desc"
    else
        fail "Missing in $1: $desc"
    fi
}
grep_src "init/main.c"   "mount_filesystems\|mount_fs"     "mount_filesystems() call"
grep_src "init/main.c"   "setup_signals\|signal"           "signal setup"
grep_src "init/main.c"   "waitpid\|reaper"                 "zombie reaper"
grep_src "init/mount.c"  "/proc"                            "mounts /proc"
grep_src "init/mount.c"  "/sys"                             "mounts /sys"
grep_src "init/mount.c"  "/dev"                             "mounts /dev"
grep_src "init/mount.c"  "cgroup\|cgroup2"                  "mounts cgroup2"
grep_src "init/signal.c" "SIGCHLD\|signal"                  "SIGCHLD handler"
grep_src "init/reaper.c" "waitpid"                          "waitpid() in reaper"

echo "  [Compiled binaries]"
check_bin "initramfs/init"
check_static "initramfs/init"
check_file "rootfs/sbin/init"
check_static "rootfs/sbin/init"

echo "  [Stage-1 initramfs]"
check_file "initramfs-stage1/init"
grep_src  "initramfs-stage1/init" "switch_root\|pivot_root" "switch_root in stage-1"
grep_src  "initramfs-stage1/init" "overlay\|squash"         "overlay mount in stage-1"

echo "  [Initramfs archive]"
check_nonempty "initramfs-stage1.cpio"
}

# =============================================================================
# PHASE 4 — Process Supervisor
# =============================================================================
skip_if_filtered 4 && {
section "Phase 4" "🟣 Process Supervisor"

echo "  [Source files]"
check_file "supervisor/main.c"
check_file "supervisor/service.h"
check_file "supervisor/parser.c"
check_file "supervisor/reaper.c"
check_file "supervisor/spawn.c"

echo "  [Source content checks]"
grep_src "supervisor/main.c"   "services.d\|services_d"   "reads services.d directory"
grep_src "supervisor/main.c"   "waitpid\|reaper"          "monitors processes"
grep_src "supervisor/parser.c" "restart\|policy"          "restart policy parsing"
grep_src "supervisor/reaper.c" "waitpid"                  "waitpid in supervisor reaper"
grep_src "supervisor/spawn.c"  "fork\|exec"               "fork+exec in spawn"
grep_src "supervisor/service.h" "restart\|RESTART"        "restart policy in header"

echo "  [Service config]"
check_file "rootfs/services.d/console.conf"
grep_src "rootfs/services.d/console.conf" "restart"       "restart policy in console.conf"
grep_src "rootfs/services.d/console.conf" "/bin/sh"       "spawns /bin/sh"

echo "  [Compiled binary]"
check_bin "initramfs/supervisor"
check_static "initramfs/supervisor"
check_file "rootfs/supervisor"
}

# =============================================================================
# PHASE 5 — Cgroup v2 Manager
# =============================================================================
skip_if_filtered 5 && {
section "Phase 5" "🟢 Cgroup v2 Manager"

echo "  [Source files]"
check_file "cgroup/cgmgr.c"
check_file "cgroup/limits.c"
check_file "cgroup/limits.h"
check_file "cgroup/slices.conf"

echo "  [Source content checks]"
grep_src "cgroup/cgmgr.c"   "cpu\|memory\|pids"     "controls CPU/memory/pids"
grep_src "cgroup/cgmgr.c"   "slices.conf"            "reads slices.conf"
grep_src "cgroup/limits.c"  "memory.max\|cpu.weight" "writes cgroup files"
grep_src "init/mount.c"     "cgroup2\|cgroup"        "init mounts cgroup2"
grep_src "init/mount.c"     "cpu\|memory"            "init enables controllers"

echo "  [Config]"
check_nonempty "cgroup/slices.conf"
grep_src "cgroup/slices.conf" "system\|containers"   "slice definitions"
grep_src "cgroup/slices.conf" "cpu_weight\|memory\|mem" "resource limits defined"

echo "  [Compiled binary]"
check_bin "initramfs/services/cgmgr"
check_static "initramfs/services/cgmgr"
check_file "rootfs/services/cgmgr"

echo "  [Runtime cgroup check (if running in Linux)]"
if [[ -d /sys/fs/cgroup ]]; then
    if stat -f -c %T /sys/fs/cgroup 2>/dev/null | grep -q cgroup2; then
        ok "Host cgroup v2 filesystem mounted (unified hierarchy)"
    else
        warn "Host is not using pure cgroup v2 (may be hybrid)"
    fi
    [[ -f /sys/fs/cgroup/cgroup.controllers ]] && \
        ok "cgroup.controllers exists: $(cat /sys/fs/cgroup/cgroup.controllers)" || \
        warn "cgroup.controllers not found on host"
else
    warn "cgroup filesystem not accessible on this host (expected inside VM/container)"
fi
}

# =============================================================================
# PHASE 6 — Network Daemon
# =============================================================================
skip_if_filtered 6 && {
section "Phase 6" "🟠 Network Daemon"

echo "  [Source files]"
check_file "netd/netd.c"
check_file "netd/netd.h"
check_file "netd/bridge.c"
check_file "netd/veth.c"
check_file "netd/nat.c"
check_file "netd/net.conf"

echo "  [Source content checks]"
grep_src "netd/bridge.c" "osBr0\|bridge"          "creates osBr0 bridge"
grep_src "netd/bridge.c" "netlink\|NETLINK\|RTM_"  "uses netlink API"
grep_src "netd/veth.c"   "veth\|VETH"              "creates veth pairs"
grep_src "netd/nat.c"    "nftables\|iptables\|nat\|masquerade" "NAT setup"
grep_src "netd/netd.c"   "ip_forward\|forwarding"  "enables IP forwarding"

echo "  [Config]"
check_nonempty "netd/net.conf"
grep_src "netd/net.conf" "10\.0\.0\|osBr0\|bridge" "bridge IP/name config"

echo "  [Compiled binary]"
check_bin "initramfs/services/netd"
check_static "initramfs/services/netd"
check_file "rootfs/services/netd"

echo "  [Container veth wiring check (runtime/spawn.c)]"
if grep -q "veth\|CLONE_NEWNET" "$ROOT/runtime/spawn.c" 2>/dev/null; then
    ok "runtime/spawn.c references veth or CLONE_NEWNET"
else
    warn "runtime/spawn.c may not wire veth into container netns (check manually)"
fi
}

# =============================================================================
# PHASE 7 — Container Runtime
# =============================================================================
skip_if_filtered 7 && {
section "Phase 7" "🟣 Container Runtime (minc)"

echo "  [Source files]"
check_file "runtime/minc.c"
check_file "runtime/spawn.c"
check_file "runtime/namespace.c"
check_file "runtime/rootfs.c"
check_file "runtime/seccomp.c"
check_file "runtime/runtime.h"

echo "  [Namespace checks (spawn.c)]"
grep_src "runtime/spawn.c" "CLONE_NEWPID"  "uses CLONE_NEWPID"
grep_src "runtime/spawn.c" "CLONE_NEWNS"   "uses CLONE_NEWNS (mount namespace)"
grep_src "runtime/spawn.c" "CLONE_NEWUTS"  "uses CLONE_NEWUTS"
grep_src "runtime/spawn.c" "clone("        "uses clone() syscall"
grep_src "runtime/spawn.c" "cgroup"        "assigns to cgroup"

echo "  [Rootfs isolation (rootfs.c)]"
grep_src "runtime/rootfs.c" "pivot_root\|chroot"    "uses pivot_root or chroot"
grep_src "runtime/rootfs.c" "overlay\|overlayfs"    "mounts overlayfs"
if grep -q "pivot_root" "$ROOT/runtime/rootfs.c" 2>/dev/null; then
    ok "pivot_root() found — full rootfs isolation"
elif grep -q "chroot" "$ROOT/runtime/rootfs.c" 2>/dev/null; then
    warn "chroot found in rootfs.c (weaker than pivot_root)"
else
    fail "No pivot_root or chroot in rootfs.c — rootfs isolation incomplete"
fi

echo "  [Seccomp (seccomp.c)]"
grep_src "runtime/seccomp.c" "SECCOMP_SET_MODE_FILTER\|seccomp("  "installs seccomp filter"
grep_src "runtime/seccomp.c" "BPF_STMT\|sock_filter"              "raw BPF filter"
grep_src "runtime/seccomp.c" "AUDIT_ARCH\|arch"                    "architecture check"

echo "  [Capabilities (spawn.c or minc.c)]"
if grep -q "cap_set\|CAP_DROP\|PR_SET_SECCOMP\|prctl" "$ROOT/runtime/spawn.c" 2>/dev/null \
   || grep -q "cap_set\|CAP_DROP\|prctl" "$ROOT/runtime/minc.c" 2>/dev/null; then
    ok "Capability drop / prctl found"
else
    warn "No explicit capability drop found in runtime — capabilities may not be dropped"
fi

echo "  [Compiled binary]"
check_bin "rootfs/bin/minc"
check_static "rootfs/bin/minc"

echo "  [Functional test: minc help/usage]"
if "$ROOT/rootfs/bin/minc" --help 2>&1 | grep -qi "usage\|run\|minc"; then
    ok "minc binary responds to --help"
elif "$ROOT/rootfs/bin/minc" 2>&1 | grep -qi "usage\|run\|minc"; then
    ok "minc binary shows usage on no args"
else
    warn "minc binary did not show usage (may require root / kernel namespaces)"
fi
}

# =============================================================================
# PHASE 8 — Root Filesystem & Disk Image
# =============================================================================
skip_if_filtered 8 && {
section "Phase 8" "🟢 Root Filesystem & Disk Image"

echo "  [Disk image]"
check_nonempty "myos.img"
if [[ -f "$ROOT/myos.img" ]]; then
    img_mb=$(( $(stat -c%s "$ROOT/myos.img" 2>/dev/null || echo 0) / 1024 / 1024 ))
    if [[ $img_mb -gt 64 ]]; then
        ok "myos.img size: ${img_mb} MB"
    else
        fail "myos.img too small: ${img_mb} MB (expect 512 MB)"
    fi
fi

echo "  [Squashfs]"
check_nonempty "os-root.squashfs"
if command -v unsquashfs &>/dev/null; then
    if unsquashfs -s "$ROOT/os-root.squashfs" 2>/dev/null | grep -q "Filesystem"; then
        ok "os-root.squashfs is a valid squashfs image"
    else
        fail "os-root.squashfs may be corrupt"
    fi
else
    warn "unsquashfs not available — cannot validate squashfs"
fi

echo "  [Rootfs tree]"
check_dir "rootfs"
check_file "rootfs/sbin/init"
check_file "rootfs/supervisor"
check_dir  "rootfs/bin"
check_file "rootfs/bin/minc"
check_dir  "rootfs/services"
check_dir  "rootfs/services.d"
check_file "rootfs/slices.conf"
check_file "rootfs/etc/net.conf"

echo "  [Bootloader]"
check_file "syslinux/syslinux.cfg"
grep_src "syslinux/syslinux.cfg" "bzImage\|vmlinuz" "references kernel image"
grep_src "syslinux/syslinux.cfg" "initrd\|initramfs" "references initramfs"

echo "  [Build scripts]"
check_file "build/mkdisk.sh"
check_file "build/mkrootfs.sh"
check_file "build/mksquashfs.sh"
check_file "build/mkstage1.sh"
}

# =============================================================================
# PHASE 9 — Build System & Automation
# =============================================================================
skip_if_filtered 9 && {
section "Phase 9" "⚪ Build System & Automation"

check_file "Makefile"
check_file "build/Makefile"
check_file "build/build-all.sh"
check_file "build/qemu.sh"
check_file "build/qemu-disk.sh"
check_file "build/verify.sh"

echo "  [Makefile target checks]"
make_target() {
    local t="$1"
    if make -n -C "$ROOT" "$t" &>/dev/null; then
        ok "Makefile target exists: $t"
    else
        warn "Makefile target missing or broken: $t"
    fi
}
make_target all
make_target init
make_target daemons
make_target tools
make_target image
make_target clean

echo "  [QEMU run script checks]"
grep_src "build/qemu.sh"      "qemu-system-x86_64"  "qemu.sh uses qemu-system-x86_64"
grep_src "build/qemu.sh"      "bzImage\|kernel"      "qemu.sh references kernel"
grep_src "build/qemu-disk.sh" "myos.img"             "qemu-disk.sh boots disk image"

echo "  [Boot log from last run]"
if [[ -f "$ROOT/build/boot.log" && -s "$ROOT/build/boot.log" ]]; then
    ok "boot.log exists (last boot recorded)"
    info "Last 5 lines of boot.log:"
    tail -5 "$ROOT/build/boot.log" | while IFS= read -r line; do info "  $line"; done
else
    warn "build/boot.log is empty or missing (run: make test)"
fi
}

# =============================================================================
# PHASE 10 — Debugging & Hardening
# =============================================================================
skip_if_filtered 10 && {
section "Phase 10" "🔴 Debugging & Hardening"

echo "  [Diagnostic tools]"
check_file "tools/cginspect.c"
check_file "tools/nslist.c"
check_file "tools/netstat.c"
check_bin  "rootfs/bin/cginspect"
check_bin  "rootfs/bin/nslist"
check_static "rootfs/bin/cginspect"
check_static "rootfs/bin/nslist"

echo "  [cginspect source check]"
grep_src "tools/cginspect.c" "cgroup\|/sys/fs/cgroup" "reads cgroup v2 files"

echo "  [nslist source check]"
grep_src "tools/nslist.c" "/proc\|ns/"                 "reads /proc namespace links"

echo "  [Seccomp hardening]"
grep_src "runtime/seccomp.c" "AUDIT_ARCH"              "arch check prevents 32-bit bypass"
# Count whitelisted syscalls
if [[ -f "$ROOT/runtime/seccomp.c" ]]; then
    sc_count=$(grep -c "__NR_\|SYS_" "$ROOT/runtime/seccomp.c" 2>/dev/null || echo 0)
    if [[ $sc_count -gt 20 ]]; then
        ok "Seccomp whitelist has ~$sc_count syscall references"
    else
        warn "Seccomp whitelist seems thin: only $sc_count syscall references"
    fi
fi

echo "  [User namespace support]"
grep_src "runtime/namespace.c" "uid_map\|gid_map"     "writes uid_map/gid_map"

echo "  [Read-only rootfs (squashfs)]"
if [[ -f "$ROOT/os-root.squashfs" ]]; then
    ok "SquashFS root exists — rootfs is inherently read-only"
else
    warn "os-root.squashfs missing — cannot verify read-only rootfs"
fi

echo "  [dm-verity (optional)]"
if grep -rq "dm-verity\|verity" "$ROOT/" 2>/dev/null; then
    ok "dm-verity references found (optional feature implemented)"
else
    warn "dm-verity not implemented (listed as optional — acceptable)"
fi

echo "  [Verify script]"
check_file "build/verify.sh"
if [[ -x "$ROOT/build/verify.sh" ]]; then
    ok "build/verify.sh is executable"
else
    warn "build/verify.sh is not executable (run: chmod +x build/verify.sh)"
fi
}

# =============================================================================
# BONUS — Integration / functional checks
# =============================================================================
skip_if_filtered 0 && {
section "BONUS" "🔧 Integration & Functional Checks"

echo "  [Docs]"
check_dir  "docs"
check_file "docs/boot-sequence.md"
check_file "docs/cgroup-layout.md"
check_file "docs/design.md"

echo "  [ELF validation of all compiled binaries]"
for bin in \
    initramfs/init \
    initramfs/supervisor \
    initramfs/services/cgmgr \
    initramfs/services/netd \
    rootfs/sbin/init \
    rootfs/supervisor \
    rootfs/bin/minc \
    rootfs/bin/cginspect \
    rootfs/bin/nslist \
    rootfs/services/cgmgr \
    rootfs/services/netd; do
    [[ -f "$ROOT/$bin" ]] && check_elf "$bin"
done

echo "  [Makefile dry-run (make -n all)]"
if make -n -C "$ROOT" all &>/dev/null; then
    ok "make -n all succeeds (build system consistent)"
else
    warn "make -n all had warnings or errors"
fi
}

# =============================================================================
# Final Report
# =============================================================================
echo ""
echo -e "${BOLD}${CYAN}╔══════════════════════════════════════════════════════════════════╗${RESET}"
echo -e "${BOLD}${CYAN}║                    FINAL REPORT                                 ║${RESET}"
echo -e "${BOLD}${CYAN}╚══════════════════════════════════════════════════════════════════╝${RESET}"
echo ""
echo -e "  ${GREEN}PASS${RESET}: $PASS"
echo -e "  ${YELLOW}WARN${RESET}: $WARN"
echo -e "  ${RED}FAIL${RESET}: $FAIL"
echo ""

TOTAL=$((PASS + WARN + FAIL))
if [[ $TOTAL -gt 0 ]]; then
    PCT=$(( PASS * 100 / TOTAL ))
    echo -e "  Overall health: ${BOLD}${PCT}%${RESET} (${PASS}/${TOTAL} checks passed)"
fi
echo ""

if [[ $FAIL -eq 0 && $WARN -eq 0 ]]; then
    echo -e "${BOLD}${GREEN}  🎉 ALL CHECKS PASSED — MinOS is fully built and ready!${RESET}"
elif [[ $FAIL -eq 0 ]]; then
    echo -e "${BOLD}${YELLOW}  ⚠  Minor warnings — review above. Core system is healthy.${RESET}"
elif [[ $FAIL -le 5 ]]; then
    echo -e "${BOLD}${YELLOW}  🔧 A few failures — check above and address them.${RESET}"
else
    echo -e "${BOLD}${RED}  ✗  Multiple failures — significant gaps in the build.${RESET}"
fi

echo ""
echo -e "  ${CYAN}Tip:${RESET} Run ${BOLD}bash build/check_phases.sh --verbose${RESET} for detailed output."
echo -e "  ${CYAN}Tip:${RESET} Run ${BOLD}bash build/check_phases.sh --phase=7${RESET} to check a single phase."
echo -e "  ${CYAN}Tip:${RESET} Run ${BOLD}make test${RESET} to boot the OS in QEMU and capture a boot log."
echo ""

# Save report
REPORT_FILE="$ROOT/build/phase_check_$(date +%Y%m%d_%H%M%S).log"
{
    echo "MinOS Phase Check — $(date)"
    echo "Root: $ROOT"
    echo "Pass: $PASS  Warn: $WARN  Fail: $FAIL"
    echo ""
    for r in "${RESULTS[@]}"; do echo "$r"; done
} > "$REPORT_FILE" 2>/dev/null && echo -e "  Report saved to: ${BOLD}${REPORT_FILE}${RESET}" || true
echo ""

exit $FAIL
