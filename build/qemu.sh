#!/bin/sh
# build/qemu.sh — Boot MinOS from the disk image
#
# Serial console is served on a Unix socket.
# Connect to it (in a second terminal) with:
#   socat -,raw,echo=0 UNIX-CONNECT:/tmp/minos-console.sock
# Or just run:
#   bash build/console.sh
#
# Usage:
#   bash build/qemu.sh          — start VM (no console window)
#   bash build/console.sh       — attach console in current terminal
#   make test                   — boot + auto-attach console

MINOS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IMG="$MINOS_DIR/myos.img"
CONSOLE_SOCK="/tmp/minos-console.sock"
MONITOR_SOCK="/tmp/minos-monitor.sock"

if [ ! -f "$IMG" ]; then
    echo "ERROR: $IMG not found."
    echo "Build first with:  make  OR  bash build/build-all.sh"
    exit 1
fi

# ── KVM ───────────────────────────────────────────────────────────────────────
KVM=""
CPU="-cpu qemu64"
if [ -e /dev/kvm ] && [ -r /dev/kvm ]; then
    KVM="-enable-kvm"
    CPU="-cpu host"
    echo "[qemu] KVM enabled"
else
    echo "[qemu] KVM not available — software emulation"
fi

# Clean up old sockets
rm -f "$CONSOLE_SOCK" "$MONITOR_SOCK"

echo "[qemu] Booting $IMG"
echo "[qemu] Attach console: bash build/console.sh"
echo "[qemu] Or:             socat -,raw,echo=0 UNIX-CONNECT:$CONSOLE_SOCK"
echo ""

# ── Launch QEMU (background) ──────────────────────────────────────────────────
# Serial console on a Unix socket — bypasses WSL stdio buffering entirely.
# socat connects to it with proper raw terminal mode → zero input lag.
qemu-system-x86_64 \
    -drive file="$IMG",format=raw,if=virtio,cache=unsafe \
    -m 256M \
    -smp 2 \
    $CPU \
    $KVM \
    -rtc clock=host \
    -nographic \
    -serial unix:"$CONSOLE_SOCK",server,nowait \
    -monitor unix:"$MONITOR_SOCK",server,nowait \
    -no-reboot &

QEMU_PID=$!
echo "[qemu] PID $QEMU_PID — waiting for socket..."

# Wait for the console socket to appear
for i in $(seq 1 20); do
    [ -S "$CONSOLE_SOCK" ] && break
    sleep 0.2
done

if [ ! -S "$CONSOLE_SOCK" ]; then
    echo "[qemu] ERROR: console socket never appeared — QEMU may have crashed"
    exit 1
fi

echo "[qemu] Console ready — attaching (Ctrl+] to detach)"
echo ""

# ── Attach console via socat ──────────────────────────────────────────────────
# raw,echo=0  — proper raw terminal mode, no WSL buffering
# UNIX-CONNECT — direct socket connection to VM serial port
socat -,raw,echo=0 "UNIX-CONNECT:$CONSOLE_SOCK"

# When socat exits, kill QEMU
echo ""
echo "[qemu] Console detached — stopping VM"
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null
