#!/bin/bash
# =============================================================================
# fix_kernel_config.sh — Enable all missing kernel options for MinOS
#
# Adds: CONFIG_SQUASHFS, CONFIG_OVERLAY_FS, cgroup controllers, namespaces
# Then rebuilds bzImage and copies it back to MinOS.
#
# Usage:
#   cd /home/anura/OS-final/MinOS
#   bash build/fix_kernel_config.sh
# =============================================================================
set -euo pipefail

# ── Paths ─────────────────────────────────────────────────────────────────────
MINOS="/home/anura/OS-final/MinOS"      # MinOS project root
KSRC="/home/anura/kernel-dev/linux-6.19.12"   # kernel source tree
KCFG="$MINOS/kernel/config"             # MinOS's kernel config (the one we track)

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
RESET='\033[0m'

echo ""
echo -e "${BOLD}MinOS Kernel Config Fixer${RESET}"
echo "  MinOS  : $MINOS"
echo "  KSrc   : $KSRC"
echo "  Config : $KCFG"
echo ""

# Validate paths
if [[ ! -f "$KSRC/Makefile" ]]; then
    echo -e "${RED}ERROR: Kernel source not found at $KSRC${RESET}"
    echo "  Edit KSRC= in this script to point to your kernel source directory."
    exit 1
fi
if [[ ! -f "$KCFG" ]]; then
    echo -e "${RED}ERROR: MinOS kernel config not found at $KCFG${RESET}"
    exit 1
fi

# ── Step 1: Patch config ──────────────────────────────────────────────────────
echo -e "${BOLD}[1] Patching $KCFG ...${RESET}"

patch_config() {
    local key="$1" value="$2" desc="$3"
    local cfg="$KCFG"

    if grep -q "^${key}=${value}" "$cfg" 2>/dev/null; then
        echo -e "  ${GREEN}✓${RESET} Already set: ${key}=${value}  ($desc)"
        return
    fi
    sed -i "/^${key}=/d" "$cfg"
    sed -i "/^# ${key} is not set/d" "$cfg"
    echo "${key}=${value}" >> "$cfg"
    echo -e "  ${GREEN}+${RESET} Set: ${key}=${value}  ($desc)"
}

echo "  [Filesystem — CRITICAL for boot]"
patch_config CONFIG_SQUASHFS              y  "squashfs read-only rootfs"
patch_config CONFIG_SQUASHFS_ZLIB         y  "zlib decompression"
patch_config CONFIG_SQUASHFS_LZ4          y  "lz4 decompression"
patch_config CONFIG_SQUASHFS_LZO          y  "lzo decompression"
patch_config CONFIG_SQUASHFS_XZ           y  "xz decompression"
patch_config CONFIG_SQUASHFS_ZSTD         y  "zstd decompression"
patch_config CONFIG_OVERLAY_FS            y  "overlayfs (container rootfs)"

echo "  [Cgroup v2 controllers]"
patch_config CONFIG_MEMCG                 y  "memory cgroup controller"
patch_config CONFIG_CGROUP_SCHED          y  "CPU scheduling cgroup"
patch_config CONFIG_CGROUP_PIDS           y  "PIDs controller"
patch_config CONFIG_FAIR_GROUP_SCHED      y  "CFS group scheduling (cpu.weight)"
patch_config CONFIG_CGROUP_FREEZER        y  "cgroup freezer"
patch_config CONFIG_BLK_CGROUP            y  "block I/O cgroup"

echo "  [Namespaces]"
patch_config CONFIG_USER_NS               y  "user namespaces (uid/gid mapping)"

echo "  [Networking]"
patch_config CONFIG_BRIDGE                y  "bridge support (osBr0)"
patch_config CONFIG_VETH                  y  "virtual ethernet pairs (containers)"
patch_config CONFIG_NETFILTER             y  "netfilter (for NAT)"
patch_config CONFIG_NF_NAT               y  "NAT/masquerade"
patch_config CONFIG_IP_NF_IPTABLES        y  "iptables"

echo ""

# ── Step 2: Copy config into kernel source and resolve deps ───────────────────
echo -e "${BOLD}[2] Resolving config dependencies (make olddefconfig) ...${RESET}"
cp "$KCFG" "$KSRC/.config"
echo -e "  Copied $KCFG → $KSRC/.config"

cd "$KSRC"
make olddefconfig 2>&1 | grep -v "^$" | tail -10

# Save resolved config back to MinOS
cp "$KSRC/.config" "$KCFG"
echo -e "  ${GREEN}✓${RESET} Resolved config saved → $KCFG"

# ── Step 3: Verify critical options survived olddefconfig ─────────────────────
echo ""
echo -e "${BOLD}[3] Verifying critical options ...${RESET}"
verify_config() {
    local key="$1" desc="$2"
    if grep -q "^${key}=y" "$KCFG" 2>/dev/null || grep -q "^${key}=m" "$KCFG" 2>/dev/null; then
        echo -e "  ${GREEN}✓${RESET} $key — $desc"
    else
        echo -e "  ${RED}✗${RESET} $key MISSING after olddefconfig — check Kconfig deps"
        echo "    Fix: cd $KSRC && make menuconfig  → enable $desc manually"
    fi
}
verify_config CONFIG_SQUASHFS   "squashfs (CRITICAL — OS won't boot without this)"
verify_config CONFIG_OVERLAY_FS "overlayfs (CRITICAL — containers need this)"
verify_config CONFIG_MEMCG      "memory cgroup controller"
verify_config CONFIG_CGROUP_PIDS "PIDs cgroup controller"
verify_config CONFIG_USER_NS    "user namespaces"
echo ""

# ── Step 4: Rebuild bzImage ───────────────────────────────────────────────────
echo -e "${BOLD}[4] Building bzImage (-j$(nproc)) ...${RESET}"
echo "    This will take a few minutes..."
echo ""

NPROC=$(nproc)
make bzImage -j"$NPROC" 2>&1 | grep -E "^(  (CC|LD|AR|OBJCOPY)|Kernel:|arch/|ERROR|error)" | tail -20

# ── Step 5: Copy bzImage back to MinOS ───────────────────────────────────────
echo ""
BZIMAGE="$KSRC/arch/x86/boot/bzImage"
if [[ ! -f "$BZIMAGE" ]]; then
    echo -e "${RED}ERROR: bzImage not found at $BZIMAGE — build may have failed${RESET}"
    echo "  Check: cd $KSRC && make bzImage -j$(nproc)"
    exit 1
fi

cp "$BZIMAGE" "$MINOS/kernel/bzImage"
NEW_SZ=$(stat -c%s "$MINOS/kernel/bzImage")
echo -e "  ${GREEN}✓${RESET} bzImage → $MINOS/kernel/bzImage  ($(( NEW_SZ / 1024 / 1024 )) MB)"

# ── Step 6: Rebuild disk image with new kernel ────────────────────────────────
echo ""
echo -e "${BOLD}[5] Rebuilding MinOS disk image with new kernel ...${RESET}"
cd "$MINOS"
bash build/mkstage1.sh   2>/dev/null && echo -e "  ${GREEN}✓${RESET} stage1 initramfs rebuilt" || echo -e "  ${YELLOW}⚠${RESET}  mkstage1.sh skipped (run manually if needed)"
bash build/mksquashfs.sh 2>/dev/null && echo -e "  ${GREEN}✓${RESET} squashfs rebuilt"         || echo -e "  ${YELLOW}⚠${RESET}  mksquashfs.sh skipped (run manually if needed)"
bash build/mkdisk.sh     2>/dev/null && echo -e "  ${GREEN}✓${RESET} disk image rebuilt"       || echo -e "  ${YELLOW}⚠${RESET}  mkdisk.sh skipped (run manually if needed)"

echo ""
echo -e "${GREEN}${BOLD}━━━ Done! ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
echo ""
echo -e "  Next steps:"
echo -e "    ${BOLD}make test${RESET}   — boot the new image in QEMU"
echo -e "    ${BOLD}bash build/check_phases.sh --phase=2${RESET}   — verify kernel config"
echo ""
