#!/bin/bash
# build/verify.sh — Verify MinOS build integrity before shipping
set -euo pipefail

MINOS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PASS=0; FAIL=0

green() { echo -e "\e[32m✓ $*\e[0m"; PASS=$((PASS+1)); }
red()   { echo -e "\e[31m✗ $*\e[0m"; FAIL=$((FAIL+1)); }
hdr()   { echo -e "\n\e[1;34m━━━ $* ━━━\e[0m"; }

# ── 1. Required files exist ────────────────────────────────────────────────────
hdr "1. Required build artifacts"
for f in \
    kernel/bzImage \
    initramfs-stage1.cpio \
    os-root.squashfs \
    myos.img; do
    if [ -f "$MINOS_DIR/$f" ]; then
        SIZE=$(du -sh "$MINOS_DIR/$f" | cut -f1)
        green "$f  ($SIZE)"
    else
        red "$f  MISSING"
    fi
done

# ── 2. Squashfs contents ───────────────────────────────────────────────────────
hdr "2. Squashfs OS root contents"

SQ="$MINOS_DIR/os-root.squashfs"
if [ -f "$SQ" ]; then
    # Required binaries
    for path in \
        squashfs-root/sbin/init \
        squashfs-root/supervisor \
        squashfs-root/services/cgmgr \
        squashfs-root/services/netd \
        squashfs-root/bin/busybox \
        squashfs-root/bin/sh \
        squashfs-root/bin/reboot \
        squashfs-root/services.d/cgmgr.conf \
        squashfs-root/services.d/netd.conf \
        squashfs-root/services.d/console.conf; do
        if unsquashfs -ls "$SQ" 2>/dev/null | grep -qF "$path"; then
            green "squashfs: $path"
        else
            red "squashfs: $path  NOT FOUND"
        fi
    done

    # Bad configs must NOT be present
    for bad in hello.conf runtime.conf; do
        if unsquashfs -ls "$SQ" 2>/dev/null | grep -qF "$bad"; then
            red "squashfs: $bad  PRESENT (should be removed!)"
        else
            green "squashfs: $bad  absent (correct)"
        fi
    done

    # Verify squashfs is valid
    if unsquashfs -s "$SQ" >/dev/null 2>&1; then
        green "squashfs: file integrity OK"
    else
        red "squashfs: file integrity FAILED"
    fi
else
    red "os-root.squashfs not found — run mksquashfs.sh"
fi

# ── 3. Disk image partitions ───────────────────────────────────────────────────
hdr "3. Disk image layout"

IMG="$MINOS_DIR/myos.img"
if [ -f "$IMG" ]; then
    IMG_SIZE=$(du -sh "$IMG" | cut -f1)
    green "myos.img exists ($IMG_SIZE)"

    # Check partition table
    PARTS=$(fdisk -l "$IMG" 2>/dev/null | grep "^$IMG" | wc -l)
    if [ "$PARTS" -ge 2 ]; then
        green "disk: $PARTS partitions found (need 2)"
    else
        red "disk: only $PARTS partition(s) found (need 2)"
    fi

    # Mount vda1 and check boot files
    LOOP=$(sudo losetup --find --show --partscan "$IMG" 2>/dev/null)
    if [ -n "$LOOP" ]; then
        MNT=$(mktemp -d)
        sudo mount "${LOOP}p1" "$MNT" 2>/dev/null && {
            for f in bzImage initramfs.cpio extlinux.conf; do
                if [ -f "$MNT/$f" ]; then
                    green "vda1: /$f present"
                else
                    red "vda1: /$f MISSING"
                fi
            done
            # Check overlay dirs exist
            if [ -d "$MNT/overlay/upper" ] && [ -d "$MNT/overlay/work" ]; then
                green "vda1: overlay/{upper,work} directories present"
            else
                red "vda1: overlay directories MISSING"
            fi
            sudo umount "$MNT"
        }

        # Check vda2 starts with squashfs magic (sqsh = 0x73717368, stored LE as 68 73 71 73)
        MAGIC=$(sudo dd if="${LOOP}p2" bs=4 count=1 2>/dev/null | od -A n -t x1 | tr -d ' \n')
        if echo "$MAGIC" | grep -qi "68737173"; then  # LE 'sqsh' magic on x86
            green "vda2: squashfs magic bytes verified (sqsh)"
        else
            red "vda2: squashfs magic not found (bytes=$MAGIC) — squashfs not written to vda2"
        fi

        rm -rf "$MNT"
        sudo losetup -d "$LOOP" 2>/dev/null
    else
        red "disk: could not attach loop device"
    fi
else
    red "myos.img not found — run mkdisk.sh"
fi

# ── 4. Stage-1 initramfs ───────────────────────────────────────────────────────
hdr "4. Stage-1 initramfs"
CPIO="$MINOS_DIR/initramfs-stage1.cpio"
if [ -f "$CPIO" ]; then
    if cpio -t < "$CPIO" 2>/dev/null | grep -q "^init$"; then
        green "initramfs: /init present"
    else
        red "initramfs: /init NOT FOUND"
    fi
    if cpio -t < "$CPIO" 2>/dev/null | grep -q "busybox"; then
        green "initramfs: busybox present"
    else
        red "initramfs: busybox NOT FOUND"
    fi
    # Check init has no /dev/vda2 regression (should look for vda2)
    if cpio -i --quiet --to-stdout init < "$CPIO" 2>/dev/null | grep -q "vda2"; then
        green "initramfs: init references vda2 (correct 2-partition layout)"
    else
        red "initramfs: init does NOT reference vda2 — may use old 1-partition layout"
    fi
else
    red "initramfs-stage1.cpio not found — run mkstage1.sh"
fi

# ── 5. Kernel ─────────────────────────────────────────────────────────────────
hdr "5. Kernel"
BZ="$MINOS_DIR/kernel/bzImage"
if [ -f "$BZ" ]; then
    BZ_SIZE=$(du -sh "$BZ" | cut -f1)
    green "bzImage present ($BZ_SIZE)"
    # Check kernel build date (should be recent)
    if file "$BZ" | grep -qi "bzImage"; then
        green "bzImage: valid kernel image format"
    else
        red "bzImage: not a valid bzImage"
    fi
fi

# ── 6. Summary ────────────────────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════"
TOTAL=$((PASS + FAIL))
if [ $FAIL -eq 0 ]; then
    echo -e "\e[1;32m  ALL CHECKS PASSED ($PASS/$TOTAL)\e[0m"
    echo ""
    echo "  Ready to boot:"
    echo "    bash build/qemu-disk.sh"
else
    echo -e "\e[1;31m  $FAIL CHECKS FAILED ($PASS/$TOTAL passed)\e[0m"
    echo ""
    echo "  Fix failures then run: bash build/build-all.sh"
fi
echo "═══════════════════════════════════"
