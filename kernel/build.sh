#!/bin/bash
# =============================================================================
# kernel/build.sh — Build the MinOS kernel (bzImage)
#
# Usage:
#   bash kernel/build.sh              # auto-detects kernel source
#   KSRC=/path/to/linux bash kernel/build.sh   # explicit source path
#
# What it does:
#   1. Locates the Linux kernel source tree
#   2. Copies kernel/config → <src>/.config
#   3. Runs make olddefconfig     (resolves any new/missing symbols)
#   4. Builds bzImage with all CPUs
#   5. Copies arch/x86/boot/bzImage → kernel/bzImage
#   6. Saves the resolved .config back to kernel/config
# =============================================================================
set -euo pipefail

# ── Paths ─────────────────────────────────────────────────────────────────────
MINOS="$(cd "$(dirname "$0")/.." && pwd)"   # MinOS project root
KCFG="$MINOS/kernel/config"                 # tracked kernel config
OUT="$MINOS/kernel/bzImage"                 # where to copy the result

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

echo ""
echo -e "${BOLD}${CYAN}MinOS Kernel Builder${RESET}"
echo -e "  Project : $MINOS"
echo ""

# ── Locate kernel source ──────────────────────────────────────────────────────
# Priority: $KSRC env var → common candidate paths
if [[ -z "${KSRC:-}" ]]; then
    for candidate in \
        /home/anura/kernel-dev/linux-6.19.12 \
        /home/anura/kernel-dev/linux-6.1* \
        /home/anura/linux-6* \
        /home/anura/linux \
        /usr/src/linux \
        $(ls -d /usr/src/linux-* 2>/dev/null | sort -V | tail -1) \
        $(ls -d /home/*/kernel-dev/linux-* 2>/dev/null | sort -V | tail -1); do
        # Expand globs
        for dir in $candidate; do
            if [[ -f "$dir/Makefile" && -f "$dir/scripts/config" ]]; then
                KSRC="$dir"
                break 2
            fi
        done
    done
fi

if [[ -z "${KSRC:-}" ]]; then
    echo -e "${RED}ERROR: Kernel source not found.${RESET}"
    echo ""
    echo "  Options:"
    echo "    1. Set KSRC:  KSRC=/path/to/linux bash kernel/build.sh"
    echo "    2. Download:"
    echo "       wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.1.tar.xz"
    echo "       tar -xf linux-6.1.tar.xz -C /home/anura/kernel-dev/"
    echo "       KSRC=/home/anura/kernel-dev/linux-6.1 bash kernel/build.sh"
    exit 1
fi

echo -e "  Kernel source : ${BOLD}$KSRC${RESET}"
echo -e "  Config        : $KCFG"
echo -e "  Output        : $OUT"
echo ""

# Validate
[[ -f "$KCFG" ]] || { echo -e "${RED}ERROR: $KCFG not found${RESET}"; exit 1; }
[[ -f "$KSRC/Makefile" ]] || { echo -e "${RED}ERROR: $KSRC doesn't look like a kernel source tree${RESET}"; exit 1; }

# ── Step 1: Copy config ───────────────────────────────────────────────────────
echo -e "${BOLD}[1/4] Copying config → $KSRC/.config${RESET}"
cp "$KCFG" "$KSRC/.config"
echo -e "  ${GREEN}✓${RESET} Done"
echo ""

# ── Step 2: Resolve new/missing symbols ──────────────────────────────────────
echo -e "${BOLD}[2/4] Running make olddefconfig (resolves new Kconfig symbols) ...${RESET}"
cd "$KSRC"
make olddefconfig 2>&1 | grep -v "^$" | grep -v "^#" | head -20
echo -e "  ${GREEN}✓${RESET} Config resolved"
echo ""

# ── Step 3: Build ─────────────────────────────────────────────────────────────
NPROC=$(nproc)
echo -e "${BOLD}[3/4] Building bzImage (-j${NPROC}) — this takes a few minutes ...${RESET}"
echo ""
START=$(date +%s)

make bzImage -j"$NPROC" 2>&1 \
    | grep -E "^(  (CC|LD|AR|AS|HOSTCC|OBJCOPY|SHIPPED)|Kernel:|arch/|setup/|ERROR|error:)" \
    | tail -30

END=$(date +%s)
ELAPSED=$(( END - START ))
echo ""

# Verify the image was produced
BZIMAGE="$KSRC/arch/x86/boot/bzImage"
if [[ ! -f "$BZIMAGE" ]]; then
    echo -e "${RED}ERROR: bzImage not found at $BZIMAGE — build failed.${RESET}"
    echo "  Run manually: cd $KSRC && make bzImage -j$NPROC"
    exit 1
fi

SZ=$(stat -c%s "$BZIMAGE")
echo -e "  ${GREEN}✓${RESET} Build complete in ${ELAPSED}s — size: $(( SZ / 1024 / 1024 )) MB"
echo ""

# ── Step 4: Copy results back to MinOS ───────────────────────────────────────
echo -e "${BOLD}[4/4] Copying artifacts to MinOS ...${RESET}"

cp "$BZIMAGE" "$OUT"
echo -e "  ${GREEN}✓${RESET} bzImage → $OUT"

# Save the resolved config (olddefconfig may have added new symbols)
cp "$KSRC/.config" "$KCFG"
echo -e "  ${GREEN}✓${RESET} Resolved config → $KCFG"

echo ""
echo -e "${BOLD}${GREEN}━━━ Kernel build complete ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${RESET}"
echo ""
echo "  Next steps:"
echo "    make image    — rebuild squashfs + disk image with new kernel"
echo "    make test     — boot in QEMU"
echo ""
