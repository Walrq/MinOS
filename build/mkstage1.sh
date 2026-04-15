#!/bin/bash
# build/mkstage1.sh
# Build the stage-1 initramfs (minimal bootstrap that mounts squashfs+overlay)
set -e

MINOS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
STAGE1_SRC="$MINOS_DIR/initramfs-stage1"
STAGE1_BUILD="$MINOS_DIR/build/stage1-root"
OUTPUT="$MINOS_DIR/initramfs-stage1.cpio"

echo "=== Building Stage-1 Initramfs ==="

# Find busybox — MUST be statically linked for initramfs use
# Try busybox-static first (Ubuntu: sudo apt install busybox-static)
BUSYBOX=$(command -v busybox-static 2>/dev/null || true)
if [ -z "$BUSYBOX" ]; then
    BUSYBOX=$(command -v busybox 2>/dev/null || true)
    if [ -n "$BUSYBOX" ]; then
        if ! file "$BUSYBOX" | grep -q "statically linked"; then
            echo "ERROR: /usr/bin/busybox is dynamically linked — cannot use in initramfs!"
            echo "Fix with: sudo apt install busybox-static"
            exit 1
        fi
    else
        echo "ERROR: busybox not found. Install: sudo apt install busybox-static"
        exit 1
    fi
fi

echo "Using busybox: $BUSYBOX"

# Build stage-1 directory tree
rm -rf "$STAGE1_BUILD"
mkdir -p "$STAGE1_BUILD"/{bin,dev,proc,sys,mnt,squash,newroot}

# Copy BusyBox and create symlinks for everything stage-1 needs
cp "$BUSYBOX" "$STAGE1_BUILD/bin/busybox"
chmod +x "$STAGE1_BUILD/bin/busybox"

for tool in sh ash mount umount mkdir echo sleep switch_root cat; do
    ln -sf busybox "$STAGE1_BUILD/bin/$tool"
done

# Copy the stage-1 init script
cp "$STAGE1_SRC/init" "$STAGE1_BUILD/init"
chmod +x "$STAGE1_BUILD/init"

# Pack as cpio (newc format, uncompressed — kernel unpacks natively)
echo "Packing stage-1 cpio..."
cd "$STAGE1_BUILD"
find . | cpio -o -H newc > "$OUTPUT" 2>/dev/null

SIZE=$(du -sh "$OUTPUT" | cut -f1)
echo "Created: $OUTPUT ($SIZE)"
echo "Stage-1 contains:"
ls -la "$STAGE1_BUILD/"
ls -la "$STAGE1_BUILD/bin/"
