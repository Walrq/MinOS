#!/bin/bash
# build/qemu-disk.sh — boot MinOS from the real disk image (no -kernel/-initrd)
MINOS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$MINOS_DIR/myos.img"

[ -f "$IMG" ] || { echo "ERROR: $IMG not found. Run: bash build/build-all.sh"; exit 1; }

# Use KVM if available (much faster), fall back to software emulation
KVM=""
[ -e /dev/kvm ] && KVM="-enable-kvm"

exec qemu-system-x86_64 \
    -drive file="$IMG",format=raw,if=virtio \
    -m 512M \
    -nographic \
    $KVM
