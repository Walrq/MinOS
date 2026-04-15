#!/bin/sh
# build/qemu.sh — Boot MinOS from the disk image
# Supports KVM acceleration when available.
#
# Usage:
#   bash build/qemu.sh          — interactive boot  (serial console)
#   make test                   — boot + tee to build/boot.log

MINOS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$MINOS_DIR/myos.img"

# Validate disk image
if [ ! -f "$IMG" ]; then
    echo "ERROR: $IMG not found."
    echo "Build first with:  make  OR  bash build/build-all.sh"
    exit 1
fi

# KVM acceleration — much faster on native Linux, skip on WSL2 if unavailable
KVM=""
if [ -e /dev/kvm ] && [ -r /dev/kvm ]; then
    KVM="-enable-kvm -cpu host"
    echo "[qemu] KVM acceleration enabled"
else
    echo "[qemu] KVM not available — software emulation"
fi

echo "[qemu] Booting $IMG  (Ctrl+A  X  to exit)"
echo ""

exec qemu-system-x86_64 \
    -drive file="$IMG",format=raw,if=virtio \
    -m 512M \
    -nographic \
    -serial mon:stdio \
    $KVM
