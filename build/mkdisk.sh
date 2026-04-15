#!/bin/bash
# build/mkdisk.sh
# Assemble a bootable MinOS disk image (myos.img):
#   MBR   → extlinux bootloader
#   vda1  → ext4  (bzImage + initramfs.cpio + extlinux.cfg + overlay upper/work)
#   vda2  → squashfs written RAW (mounted directly as block device — no loop needed!)
#
# Requires: extlinux, mkfs.ext4, losetup, fdisk
# Install: sudo apt install syslinux-common extlinux

set -e

MINOS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$MINOS_DIR/myos.img"
IMG_SIZE_MB=512

# ── Preflight checks ───────────────────────────────────────────────────────────
echo "=== MinOS Disk Image Builder ==="

for f in \
    "$MINOS_DIR/kernel/bzImage" \
    "$MINOS_DIR/initramfs-stage1.cpio" \
    "$MINOS_DIR/os-root.squashfs"; do
    [ -f "$f" ] || { echo "ERROR: missing $f — run build-all.sh"; exit 1; }
done

for cmd in extlinux mkfs.ext4 losetup fdisk; do
    command -v "$cmd" >/dev/null || {
        echo "ERROR: $cmd not found. Install with: sudo apt install syslinux-common extlinux"
        exit 1
    }
done

# Find syslinux MBR binary (path varies by distro)
MBR_BIN=""
for p in \
    /usr/lib/syslinux/mbr/mbr.bin \
    /usr/lib/syslinux/bios/mbr.bin \
    /usr/share/syslinux/mbr.bin \
    /usr/lib/syslinux/mbr.bin; do
    [ -f "$p" ] && { MBR_BIN="$p"; break; }
done
[ -n "$MBR_BIN" ] || { echo "ERROR: syslinux MBR binary not found"; exit 1; }
echo "Using MBR: $MBR_BIN"

SQ_SIZE=$(stat -c%s "$MINOS_DIR/os-root.squashfs")
SQ_MB=$(( (SQ_SIZE + 1048575) / 1048576 + 4 ))   # squashfs size + 4MB padding
echo "Squashfs size: ${SQ_MB}MB"

# ── Create blank disk image ────────────────────────────────────────────────────
echo "Creating ${IMG_SIZE_MB}MB disk image..."
dd if=/dev/zero of="$IMG" bs=1M count="$IMG_SIZE_MB" status=progress

# ── Partition table: 2 partitions ─────────────────────────────────────────────
# p1: ~490MB ext4 boot+overlay, p2: squashfs (raw, just enough space)
echo "Partitioning (2 partitions)..."
PART2_START=2048            # in sectors (512B each), start of p1
PART1_END=$(( IMG_SIZE_MB * 1024 * 2 - SQ_MB * 1024 * 2 - 1 ))  # leave room for p2
PART2_END=$(( IMG_SIZE_MB * 1024 * 2 - 1 ))

fdisk "$IMG" <<EOF
n
p
1
${PART2_START}
${PART1_END}
a
1
n
p
2
$(( PART1_END + 1 ))
${PART2_END}
t
2
83
w
EOF

# ── Set up loop device ─────────────────────────────────────────────────────────
echo "Setting up loop device..."
LOOP=$(sudo losetup --find --show --partscan "$IMG")
echo "Loop device: $LOOP  (p1=${LOOP}p1  p2=${LOOP}p2)"

# ── Format partition 1 as ext4 (boot + overlay) ───────────────────────────────
echo "Formatting vda1 as ext4..."
sudo mkfs.ext4 -L "MinOS-boot" "${LOOP}p1"

# ── Write squashfs RAW to partition 2 (no filesystem — squashfs IS the fs) ────
echo "Writing squashfs to vda2 (raw)..."
sudo dd if="$MINOS_DIR/os-root.squashfs" of="${LOOP}p2" bs=1M status=progress

# ── Mount p1 and copy boot files ──────────────────────────────────────────────
MNTDIR=$(mktemp -d)
echo "Mounting ${LOOP}p1 → $MNTDIR ..."
sudo mount "${LOOP}p1" "$MNTDIR"

echo "Copying boot files..."
sudo cp "$MINOS_DIR/kernel/bzImage"          "$MNTDIR/bzImage"
sudo cp "$MINOS_DIR/initramfs-stage1.cpio"   "$MNTDIR/initramfs.cpio"

# extlinux config (no squashfs file needed on this partition anymore)
sudo cp "$MINOS_DIR/syslinux/syslinux.cfg"   "$MNTDIR/extlinux.conf"

# Persistent overlay dirs (writes here survive reboots)
sudo mkdir -p "$MNTDIR/overlay/upper" "$MNTDIR/overlay/work"

echo "Boot partition contents:"
ls -lh "$MNTDIR/"

# Install extlinux bootloader
echo "Installing extlinux..."
sudo extlinux --install "$MNTDIR"

sudo umount "$MNTDIR"
rm -rf "$MNTDIR"

# ── Install MBR ───────────────────────────────────────────────────────────────
echo "Installing MBR ($MBR_BIN) to $LOOP..."
sudo dd if="$MBR_BIN" of="$LOOP" bs=440 count=1 conv=notrunc 2>/dev/null

# ── Cleanup ───────────────────────────────────────────────────────────────────
sudo losetup -d "$LOOP"

echo ""
echo "=== Done! ==="
echo "Disk layout:"
echo "  vda1 → ext4  (boot + overlay)"
echo "  vda2 → squashfs raw (OS root, no loop device needed)"
echo ""
echo "Disk image: $IMG  ($(du -sh "$IMG" | cut -f1))"
echo "Boot with: bash build/qemu-disk.sh"
