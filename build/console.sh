#!/bin/sh
# build/console.sh — Attach to a running MinOS QEMU console
#
# Use this to connect to the VM in a second terminal after running:
#   bash build/qemu.sh &
# Or just run:
#   make test    (qemu.sh auto-attaches)

CONSOLE_SOCK="/tmp/minos-console.sock"

if [ ! -S "$CONSOLE_SOCK" ]; then
    echo "ERROR: Console socket not found at $CONSOLE_SOCK"
    echo "Start MinOS first: bash build/qemu.sh"
    exit 1
fi

echo "Attaching to MinOS console (Ctrl+] to detach)"
echo ""
exec socat -,raw,echo=0 "UNIX-CONNECT:$CONSOLE_SOCK"
