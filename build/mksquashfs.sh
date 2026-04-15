#!/bin/bash
# build/mksquashfs.sh
# Pack rootfs/ into a compressed squashfs image (the immutable OS root layer).
set -e

MINOS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ROOTFS="$MINOS_DIR/rootfs"
OUTPUT="$MINOS_DIR/os-root.squashfs"

if [ ! -f "$ROOTFS/sbin/init" ]; then
    echo "ERROR: rootfs not built. Run: bash build/mkrootfs.sh first"
    exit 1
fi

command -v mksquashfs >/dev/null || {
    echo "ERROR: mksquashfs not found. Install with:"
    echo "  sudo apt install squashfs-tools"
    exit 1
}

echo "=== Building squashfs OS image ==="

# Exclude empty/runtime-only directories from the squashfs
# (they'll be created at boot — squashfs is read-only so we can't write to them)
mksquashfs "$ROOTFS" "$OUTPUT" \
    -comp xz \
    -noappend \
    -no-progress \
    -no-exports \
    -e "$ROOTFS/dev" \
    -e "$ROOTFS/proc" \
    -e "$ROOTFS/sys" \
    -e "$ROOTFS/tmp" \
    -e "$ROOTFS/run" \
    -e "$ROOTFS/containers" \
    -e "$ROOTFS/mnt"

echo ""
echo "Created: $OUTPUT"
ls -lh "$OUTPUT"
echo ""
echo "Squashfs contents:"
unsquashfs -ls "$OUTPUT" | head -30
