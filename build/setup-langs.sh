#!/bin/bash
# build/setup-langs.sh
# Downloads static Python 3.12 (musl) and builds static TCC,
# then stages both into rootfs/ for inclusion in the squashfs image.
#
# Run once:  bash build/setup-langs.sh
# Then:      make image && make test
#
# Inside MinOS container:
#   python3 /home/hello.py
#   tcc -run /home/hello.c
set -euo pipefail

MINOS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ROOTFS="$MINOS_DIR/rootfs"
WORK="$MINOS_DIR/.lang-build"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; BOLD='\033[1m'; NC='\033[0m'
ok()   { echo -e "${GREEN}✓${NC} $*"; }
info() { echo -e "${YELLOW}→${NC} $*"; }
err()  { echo -e "${RED}✗ ERROR:${NC} $*" >&2; exit 1; }

mkdir -p "$WORK" "$ROOTFS/usr/bin" "$ROOTFS/usr/lib" "$ROOTFS/home"

echo ""
echo -e "${BOLD}MinOS Language Runtime Setup${NC}"
echo "  Rootfs : $ROOTFS"
echo "  Work   : $WORK"
echo ""

# ═══════════════════════════════════════════════════════════════════════════════
# PART 1 — Python 3.12 (musl, statically linked via python-build-standalone)
# ═══════════════════════════════════════════════════════════════════════════════
echo -e "${BOLD}[1/3] Python 3.12 (musl standalone)${NC}"

PY_VERSION="3.12.4"
PY_TAG="20240726"
PY_TARBALL="cpython-${PY_VERSION}+${PY_TAG}-x86_64-unknown-linux-musl-install_only.tar.gz"
PY_URL="https://github.com/indygreg/python-build-standalone/releases/download/${PY_TAG}/${PY_TARBALL}"
PY_DIR="$WORK/python-standalone"

if [ -f "$ROOTFS/usr/bin/python3" ]; then
    ok "Python already staged — skipping download."
else
    info "Downloading $PY_TARBALL (~30 MB)..."
    wget -q --show-progress -O "$WORK/$PY_TARBALL" "$PY_URL" || \
        err "Download failed. Check your internet connection."

    info "Extracting..."
    rm -rf "$PY_DIR"
    mkdir -p "$PY_DIR"
    tar -xf "$WORK/$PY_TARBALL" -C "$PY_DIR"
    ok "Extracted to $PY_DIR"

    # python-build-standalone extracts to python/ subdirectory
    PY_ROOT="$PY_DIR/python"
    [ -d "$PY_ROOT" ] || err "Expected $PY_ROOT — tarball structure changed?"

    # ── Stage the binary ──────────────────────────────────────────────────────
    # The binary is linked with /install as prefix. We override at runtime
    # with a PYTHONHOME wrapper script.
    info "Staging binary..."
    mkdir -p "$ROOTFS/usr/lib/python-bin"
    cp "$PY_ROOT/bin/python3.12" "$ROOTFS/usr/lib/python-bin/python3.12"
    chmod +x "$ROOTFS/usr/lib/python-bin/python3.12"

    # ── Stage the stdlib ──────────────────────────────────────────────────────
    info "Staging stdlib (this takes a moment)..."
    mkdir -p "$ROOTFS/usr/lib/python3.12"

    # Copy stdlib .py files — skip test suite, pydoc_data, idlelib, tkinter
    # to keep the squashfs size reasonable (~10 MB instead of ~80 MB)
    rsync -a --exclude='test/' \
              --exclude='tests/' \
              --exclude='__pycache__/' \
              --exclude='idlelib/' \
              --exclude='tkinter/' \
              --exclude='turtle*' \
              --exclude='pydoc_data/' \
              --exclude='*.pyc' \
        "$PY_ROOT/lib/python3.12/" \
        "$ROOTFS/usr/lib/python3.12/" 2>/dev/null || \
    cp -r "$PY_ROOT/lib/python3.12/." "$ROOTFS/usr/lib/python3.12/"

    # ── Create PYTHONHOME wrapper ─────────────────────────────────────────────
    # The standalone binary has /install baked in as prefix.
    # Setting PYTHONHOME=/usr tells Python where to find its stdlib at runtime.
    cat > "$ROOTFS/usr/bin/python3" << 'WRAPPER'
#!/bin/sh
export PYTHONHOME=/usr
export PYTHONPATH=/usr/lib/python3.12
# TERM=dumb: disables readline escape sequences that conflict with the
# QEMU serial console/socat raw mode, preventing dropped/mangled keystrokes.
export TERM=dumb
export PYTHONIOENCODING=utf-8
exec /usr/lib/python-bin/python3.12 "$@"
WRAPPER
    chmod +x "$ROOTFS/usr/bin/python3"

    # Symlinks
    ln -sf python3 "$ROOTFS/usr/bin/python"

    ok "Python 3.12 staged → /usr/bin/python3"
fi

# ═══════════════════════════════════════════════════════════════════════════════
# PART 2 — TCC (Tiny C Compiler, built as static binary)
# ═══════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}[2/3] TCC — Tiny C Compiler${NC}"

TCC_DIR="$WORK/tinycc"

if [ -f "$ROOTFS/usr/bin/tcc" ]; then
    ok "TCC already staged — skipping build."
else
    if ! command -v git &>/dev/null; then
        err "git not found — install with: sudo apt install git"
    fi

    info "Cloning TCC source (~5 MB)..."
    if [ -d "$TCC_DIR/.git" ]; then
        info "Repo already cloned, updating..."
        git -C "$TCC_DIR" pull --quiet
    else
        git clone --depth=1 https://repo.or.cz/tinycc.git "$TCC_DIR" --quiet
    fi

    info "Configuring TCC..."
    cd "$TCC_DIR"

    # Do NOT set --sysincludepaths here — TCC uses itself (bootstrap) to compile
    # libtcc1.a, and if sysincludepaths is wrong the bootstrap can't find stdio.h.
    # We handle include paths at RUNTIME via a wrapper script (-B flag).
    ./configure --prefix=/usr 2>&1 | tail -5

    make -j"$(nproc)" LDFLAGS="-static" 2>&1 | tail -5
    ok "TCC built."

    # Stage TCC binary to a private location
    mkdir -p "$ROOTFS/usr/lib/tcc-bin"
    cp "$TCC_DIR/tcc" "$ROOTFS/usr/lib/tcc-bin/tcc"
    chmod +x "$ROOTFS/usr/lib/tcc-bin/tcc"

    # Stage TCC's own minimal headers (stddef.h, stdarg.h, etc.)
    mkdir -p "$ROOTFS/usr/lib/tcc/include"
    cp -r "$TCC_DIR/include/." "$ROOTFS/usr/lib/tcc/include/"

    # Stage libtcc1.a (needed for tcc -run to link programs)
    cp "$TCC_DIR/libtcc1.a" "$ROOTFS/usr/lib/tcc/" 2>/dev/null || true

    # Copy C standard headers (musl preferred, system fallback)
    MUSL_INC=""
    for d in /usr/include/x86_64-linux-musl \
              /usr/lib/x86_64-linux-gnu/musl/include \
              /usr/include/musl; do
        [ -f "$d/stdio.h" ] && { MUSL_INC="$d"; break; }
    done

    if [ -n "$MUSL_INC" ]; then
        cp -rn "$MUSL_INC/." "$ROOTFS/usr/lib/tcc/include/" 2>/dev/null || true
        ok "Musl headers staged from $MUSL_INC"
    else
        info "Musl headers not found — install musl-dev: sudo apt install musl-dev"
        info "Copying minimal system headers as fallback..."
        for h in stdio.h stdlib.h string.h unistd.h stdint.h stddef.h errno.h \
                  fcntl.h sys/types.h sys/stat.h; do
            SRC="/usr/include/$h"
            DST="$ROOTFS/usr/lib/tcc/include/$h"
            [ -f "$SRC" ] && { mkdir -p "$(dirname "$DST")"; cp "$SRC" "$DST"; }
        done
    fi

    # Runtime wrapper: -B/usr/lib/tcc tells TCC to look in {B}/include for
    # headers and {B}/libtcc1.a for the support library — both staged above.
    cat > "$ROOTFS/usr/bin/tcc" << 'WRAPPER'
#!/bin/sh
exec /usr/lib/tcc-bin/tcc -B/usr/lib/tcc "$@"
WRAPPER
    chmod +x "$ROOTFS/usr/bin/tcc"
    ok "TCC staged → /usr/bin/tcc"
fi

# ═══════════════════════════════════════════════════════════════════════════════
# PART 3 — Demo scripts
# ═══════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}[3/3] Staging demo scripts → rootfs/home/${NC}"

mkdir -p "$ROOTFS/home"

# Python demo
cat > "$ROOTFS/home/hello.py" << 'PYEOF'
# hello.py — MinOS Python demo
# Run with:  minc run /usr/bin/python3 /home/hello.py
# Or:        minc run -m 32 /usr/bin/python3 /home/hello.py

import os
import sys

def add(a, b):
    return a + b

print("=" * 40)
print("  MinOS Python 3 — Container Demo")
print("=" * 40)
print(f"  Python  : {sys.version.split()[0]}")
print(f"  Hostname: {os.uname().nodename}")
print(f"  PID     : {os.getpid()}")
print()

a = 17
b = 25
result = add(a, b)
print(f"  add({a}, {b}) = {result}")
print()

print("  PID namespace (only container processes):")
try:
    procs = [p for p in os.listdir('/proc') if p.isdigit()]
    for pid in sorted(procs, key=int):
        try:
            with open(f'/proc/{pid}/comm') as f:
                comm = f.read().strip()
            print(f"    PID {pid:>4}: {comm}")
        except:
            pass
except Exception as e:
    print(f"    (could not list: {e})")

print()
print("=" * 40)
PYEOF
ok "hello.py → rootfs/home/hello.py"

# Advanced Python demo
cat > "$ROOTFS/home/add.py" << 'PYEOF'
# add.py — simple addition demo
# Run with:  minc run /usr/bin/python3 /home/add.py 10 32
import sys

a = int(sys.argv[1]) if len(sys.argv) > 1 else 10
b = int(sys.argv[2]) if len(sys.argv) > 2 else 32

def add(a, b):
    return a + b

result = add(a, b)
print(f"a      = {a}")
print(f"b      = {b}")
print(f"a + b  = {result}")
PYEOF
ok "add.py → rootfs/home/add.py"

# C demo
cat > "$ROOTFS/home/hello.c" << 'CEOF'
/* hello.c — MinOS TCC demo
 * Compile + run with:  minc run /usr/bin/tcc -run /home/hello.c
 */
#include <stdio.h>
#include <unistd.h>

int add(int a, int b) { return a + b; }

int main(void) {
    char hostname[64] = "unknown";
    gethostname(hostname, sizeof(hostname));

    printf("========================================\n");
    printf("  MinOS TCC C Compiler — Container Demo\n");
    printf("========================================\n");
    printf("  Hostname: %s\n", hostname);
    printf("  PID     : %d\n", getpid());
    printf("\n");
    printf("  add(17, 25) = %d\n", add(17, 25));
    printf("\n");
    printf("  Compiled and executed inside the container\n");
    printf("  using TCC (Tiny C Compiler) — no host tools!\n");
    printf("========================================\n");
    return 0;
}
CEOF
ok "hello.c → rootfs/home/hello.c"

# Copy the full todo project
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TODO_SRC="$SCRIPT_DIR/../rootfs/home/todo.py"
if [ -f "$TODO_SRC" ]; then
    ok "todo.py already in rootfs/home/ (tracked with rootfs)"
else
    info "todo.py not found — create it at rootfs/home/todo.py"
fi

echo ""
echo -e "${GREEN}${BOLD}━━━ Language runtime setup complete! ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo "  Now rebuild the image:"
echo -e "    ${BOLD}make image${NC}"
echo ""
echo "  Boot and run:"
echo -e "    ${BOLD}make test${NC}"
echo ""
echo "  Inside MinOS (~ #):"
echo -e "    ${BOLD}minc run /usr/bin/python3 /home/hello.py${NC}"
echo -e "    ${BOLD}minc run /usr/bin/python3 /home/add.py 10 32${NC}"
echo -e "    ${BOLD}minc run -t /usr/bin/python3${NC}              # REPL"
echo -e "    ${BOLD}minc run /usr/bin/tcc -run /home/hello.c${NC}"
echo -e "    ${BOLD}minc run -m 32 /usr/bin/python3 /home/hello.py${NC}"
echo ""
