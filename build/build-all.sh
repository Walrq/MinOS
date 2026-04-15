#!/bin/bash
# build/build-all.sh
# Full MinOS build pipeline — one command to build everything.
#
# Stages:
#   1. Compile all binaries + assemble rootfs/
#   2. Pack stage-1 initramfs (bootstrap)
#   3. Create squashfs OS root image
#   4. Assemble bootable disk image with syslinux
#
# Usage:
#   bash build/build-all.sh           -- full build
#   bash build/build-all.sh --no-disk -- skip disk (just squashfs)
set -e

MINOS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
NO_DISK=0
[ "$1" = "--no-disk" ] && NO_DISK=1

cd "$MINOS_DIR"

echo "╔══════════════════════════════════════╗"
echo "║      MinOS Full Build Pipeline       ║"
echo "╚══════════════════════════════════════╝"
echo ""

# Check required host tools
MISSING=""
for cmd in gcc mksquashfs cpio; do
    command -v "$cmd" >/dev/null || MISSING="$MISSING $cmd"
done
if [ $NO_DISK -eq 0 ]; then
    for cmd in extlinux mkfs.ext4 losetup fdisk; do
        command -v "$cmd" >/dev/null || MISSING="$MISSING $cmd"
    done
fi
if [ -n "$MISSING" ]; then
    echo "ERROR: Missing tools:$MISSING"
    echo "Install with: sudo apt install build-essential squashfs-tools syslinux syslinux-common"
    exit 1
fi

# Step 1: Compile and populate rootfs/
echo "━━━ Step 1/4: Building rootfs/ ━━━━━━━━━━━━━━━"
bash "$MINOS_DIR/build/mkrootfs.sh"

# Step 2: Build stage-1 initramfs
echo ""
echo "━━━ Step 2/4: Building stage-1 initramfs ━━━━━"
bash "$MINOS_DIR/build/mkstage1.sh"

# Step 3: Create squashfs
echo ""
echo "━━━ Step 3/4: Building squashfs OS image ━━━━━"
bash "$MINOS_DIR/build/mksquashfs.sh"

if [ $NO_DISK -eq 1 ]; then
    echo ""
    echo "━━━ Step 4/4: Disk image skipped (--no-disk) ━━"
    echo ""
    echo "✓ Build complete (no disk image)"
    exit 0
fi

# Step 4: Assemble disk image
echo ""
echo "━━━ Step 4/4: Assembling disk image ━━━━━━━━━━"
bash "$MINOS_DIR/build/mkdisk.sh"

echo ""
echo "╔══════════════════════════════════════╗"
echo "║         Build Complete! ✓            ║"
echo "╚══════════════════════════════════════╝"
echo ""
echo "  Disk image : myos.img"
echo "  Boot command: bash build/qemu-disk.sh"
echo ""
echo "  Inside MinOS:"
echo "    minc run -t /bin/sh   ← run a container"
echo "    ip addr show osBr0    ← check bridge"
