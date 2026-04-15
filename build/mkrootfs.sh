#!/bin/bash
# build/mkrootfs.sh
# Compile all MinOS binaries and assemble the rootfs/ directory.
# rootfs/ is the source for os-root.squashfs (the immutable OS layer).
set -e

MINOS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ROOTFS="$MINOS_DIR/rootfs"

echo "=== Building rootfs/ ==="

# ── Always start clean — stale files from old builds cause bad squashfs ─────
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"/{sbin,bin,services,services.d,dev,proc,sys,tmp,run,containers,mnt}

# ── Compile everything ─────────────────────────────────────────────────────────

echo "[1/5] Compiling init..."
cd "$MINOS_DIR/init"
gcc -static -o init main.c mount.c signal.c reaper.c
cp init "$ROOTFS/sbin/init"
chmod +x "$ROOTFS/sbin/init"

echo "[2/5] Compiling supervisor..."
cd "$MINOS_DIR/supervisor"
# supervisor.c excluded — it has a duplicate main(); main.c is the entry point
gcc -static -o supervisor main.c parser.c spawn.c reaper.c service.c
cp supervisor "$ROOTFS/supervisor"
chmod +x "$ROOTFS/supervisor"

echo "[3/5] Compiling cgmgr..."
cd "$MINOS_DIR/cgroup"
gcc -static -o cgmgr cgmgr.c limits.c
cp cgmgr "$ROOTFS/services/cgmgr"
chmod +x "$ROOTFS/services/cgmgr"

echo "[4/5] Compiling netd..."
cd "$MINOS_DIR/netd"
gcc -static -o netd netd.c bridge.c veth.c nat.c
cp netd "$ROOTFS/services/netd"
chmod +x "$ROOTFS/services/netd"

echo "[5/5] Compiling minc..."
cd "$MINOS_DIR/runtime"
gcc -static -o minc minc.c spawn.c
cp minc "$ROOTFS/bin/minc"
chmod +x "$ROOTFS/bin/minc"

# ── BusyBox for shell + tools ──────────────────────────────────────────────────
BUSYBOX=$(which busybox 2>/dev/null || true)
if [ -n "$BUSYBOX" ]; then
    echo "[+] Installing BusyBox..."
    cp "$BUSYBOX" "$ROOTFS/bin/busybox"
    chmod +x "$ROOTFS/bin/busybox"
    # Create symlinks for common tools
    for tool in sh ash ls cat echo ps kill grep sed awk ip \
                mount umount mkdir rm cp mv ln chmod chown \
                head tail wc cut sort uniq find \
                reboot halt poweroff sync \
                free df dmesg mknod; do
        ln -sf busybox "$ROOTFS/bin/$tool"
    done
else
    echo "WARNING: busybox not found — no shell tools in rootfs!"
fi

# ── Config files ───────────────────────────────────────────────────────────────
echo "[+] Copying configs..."

# cgmgr reads /slices.conf (at filesystem root)
cp "$MINOS_DIR/cgroup/slices.conf" "$ROOTFS/slices.conf"

# netd reads /etc/net.conf if it exists
mkdir -p "$ROOTFS/etc"
[ -f "$MINOS_DIR/netd/net.conf" ] && cp "$MINOS_DIR/netd/net.conf" "$ROOTFS/etc/net.conf"

# Service configs — only copy ones whose binaries exist in rootfs
cp "$MINOS_DIR/services.d/cgmgr.conf"  "$ROOTFS/services.d/cgmgr.conf"
cp "$MINOS_DIR/services.d/netd.conf"   "$ROOTFS/services.d/netd.conf"

# Console service — launches an interactive shell on the serial console
# (inherits init's stdin/stdout which is ttyS0)
cat > "$ROOTFS/services.d/console.conf" <<'EOF'
name=console
bin=/bin/sh
restart=always
EOF

# ── Summary ────────────────────────────────────────────────────────────────────
echo ""
echo "rootfs/ layout:"
find "$ROOTFS" -not -type d | sort | sed "s|$ROOTFS||"
echo ""
echo "Total size: $(du -sh "$ROOTFS" | cut -f1)"
