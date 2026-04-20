# MinOS

A minimal, from-scratch Linux-based operating system with an integrated container runtime, built as an OS course project.

MinOS boots a custom kernel into a custom C init, runs a service supervisor, and provides `minc` — a lightweight container runtime with full namespace isolation, OverlayFS rootfs, cgroup v2 resource limits, capability dropping, and a BPF seccomp whitelist.

---

## Features

- **Custom kernel** — minimal Linux config with cgroup v2, namespaces, SquashFS, OverlayFS, virtio, and seccomp
- **Custom init (PID 1)** — written in C; mounts `/proc /sys /dev`, sets up cgroup v2, launches the supervisor
- **Service supervisor** — reads `services.d/`, spawns services, monitors with `waitpid`, auto-restarts on failure
- **cgroup v2 resource manager (`cgmgr`)** — creates and wires per-container cgroup slices with `cpu.weight`, `memory.max`, `pids.max`
- **Network daemon (`netd`)** — creates `osBr0` bridge, assigns `10.0.0.1/24`, enables NAT masquerade
- **Container runtime (`minc`)** — runs isolated containers with:
  - `CLONE_NEWPID` · `CLONE_NEWNS` · `CLONE_NEWUTS` · `CLONE_NEWNET` · `CLONE_NEWIPC`
  - OverlayFS rootfs (SquashFS lower + tmpfs upper/work + `pivot_root`)
  - Capability drop (all 41 bounding set caps cleared, ambient cleared, securebits locked)
  - BPF seccomp syscall whitelist
  - cgroup v2 assignment for CPU/memory/PIDs limits
- **Diagnostic tools** — `cginspect` (cgroup tree viewer), `nslist` (namespace inspector)
- **Two-stage initramfs** — stage-1 shell script mounts `/dev/vda1` (ext4) + `/dev/vda2` (SquashFS), builds an OverlayFS root, then `switch_root`s into the OS
- **Bootable disk image** — 512 MB raw image with syslinux bootloader, built entirely from the Makefile

---

## Architecture

```
myos.img (512 MB raw disk)
├── /dev/vda1  ext4          ← boot partition: syslinux + bzImage + stage-1 initramfs
└── /dev/vda2  SquashFS      ← read-only OS root (rootfs/)

Boot sequence:
  BIOS → syslinux → bzImage
    → initramfs-stage1/init (shell)
        mounts vda1 (ext4) + vda2 (squashfs)
        overlayfs: lower=/squash  upper=/mnt/overlay/upper (ext4)
        switch_root /newroot /sbin/init
    → init/main.c  (PID 1)
        mount /proc /sys /dev /tmp
        enable cgroup v2 controllers
        exec supervisor /services.d
    → supervisor/main.c
        spawn: cgmgr  netd  console
    → /bin/sh  (interactive console)
```

```
Container lifecycle (minc run /bin/sh):
  clone(CLONE_NEWPID|NS|UTS|NET|IPC)
    wait for cgroup assignment (sync pipe)
    setup_rootfs():
      mount tmpfs → /run/containers/c0/
      bind /squash → lower/
      overlayfs lower+upper+work → merged/
      pivot_root merged/ → /   (host fs now invisible)
      mount proc, devtmpfs, devpts
    drop_capabilities()    ← bounding + ambient + securebits
    load_seccomp()         ← BPF whitelist
    execv(/bin/sh)
```

---

## Directory Structure

```
MinOS/
├── init/               PID 1 — mount, signal, reaper
├── supervisor/         Service supervisor — parser, spawn, reaper
├── cgroup/             cgmgr — cgroup v2 slice manager
├── netd/               Network daemon — bridge, NAT, DHCP
├── runtime/            minc container runtime
│   ├── minc.c          CLI + container ID management
│   ├── spawn.c         clone() + cgroup assignment + capability drop
│   ├── rootfs.c        OverlayFS setup + pivot_root
│   ├── namespace.c     UID/GID map helpers
│   ├── seccomp.c       BPF syscall whitelist
│   └── runtime.h       Shared types and declarations
├── tools/              cginspect, nslist
├── initramfs-stage1/   Stage-1 init shell script
├── kernel/             bzImage + .config (tracked, not the binary)
├── build/              All build and test scripts
│   ├── check_phases.sh     Phase-by-phase verification (211 checks)
│   ├── fix_kernel_config.sh Kernel config patcher + rebuilder
│   ├── mkdisk.sh           Disk image assembler
│   ├── mksquashfs.sh       SquashFS builder
│   ├── mkstage1.sh         Stage-1 initramfs builder
│   ├── qemu.sh             Boot in QEMU (disk-image mode)
│   └── qemu-disk.sh        Alternate QEMU boot script
├── syslinux/           Bootloader binaries
├── docs/               Design notes
├── Makefile            Top-level build system
└── README.md
```

---

## Prerequisites

```bash
# Ubuntu / WSL2
sudo apt update
sudo apt install -y \
  gcc make flex bison bc libssl-dev libelf-dev \
  musl-tools libc6-dev \
  qemu-system-x86_64 \
  squashfs-tools \
  syslinux syslinux-utils \
  dosfstools e2fsprogs
```

> ⚠️ **Build must be run inside WSL2 (Ubuntu)** — the build scripts use Linux-specific tools and paths.

---

## Quick Start

```bash
git clone https://github.com/<you>/MinOS.git
cd MinOS

# 1. Build everything (kernel must already be compiled — see Kernel section)
make

# 2. Boot in QEMU
make test
# → Drops into MinOS shell at  ~ #

# 3. Run a container
minc run /bin/echo "hello from container"

# 4. Interactive container shell
minc run -t /bin/sh
```

---

## Running Containers

All commands run from the MinOS shell (`~ #`) inside QEMU:

```sh
# One-shot command
minc run /bin/echo "hello from container"

# Interactive shell with job control
minc run -t /bin/sh

# Run 4 containers back-to-back
minc run /bin/echo "Container 0"
minc run /bin/echo "Container 1"
minc run /bin/echo "Container 2"
minc run /bin/echo "Container 3"

# Run 4 containers simultaneously (background)
minc run /bin/sh -c "sleep 30 && echo c0 done" &
minc run /bin/sh -c "sleep 25 && echo c1 done" &
minc run /bin/sh -c "sleep 20 && echo c2 done" &
minc run /bin/sh -c "sleep 15 && echo c3 done" &

# Inspect cgroup resource usage across containers
ls /sys/fs/cgroup/containers/
```

Each container gets an auto-incrementing ID (`c0`, `c1`, `c2`, ...) and is fully isolated:

```sh
# Inside a container:
hostname          # → c1  (UTS namespace)
ls /              # → MinOS rootfs (OverlayFS, not host files)
cat /proc/self/status | grep Pid   # → PID 1 in own namespace
```

---

## Build Targets

```bash
make              # build everything: init, daemons, tools, image
make init         # compile init (PID 1)
make daemons      # compile supervisor, cgmgr, netd, minc
make tools        # compile cginspect, nslist
make image        # build squashfs + disk image
make test         # boot myos.img in QEMU
make clean        # remove all build artifacts
make verify       # run static checks on the build
```

---

## Kernel

The kernel binary (`kernel/bzImage`) is not tracked in git (14 MB). Build it from Linux source:

```bash
# 1. Download Linux source
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.1.tar.xz
tar -xf linux-6.1.tar.xz

# 2. Apply MinOS config
cp kernel/config linux-6.1/.config

# 3. Fix any missing options (squashfs, overlayfs, etc.)
bash build/fix_kernel_config.sh

# 4. Or build manually
cd linux-6.1
make olddefconfig
make bzImage -j$(nproc)
cp arch/x86/boot/bzImage ../MinOS/kernel/bzImage
```

**Critical kernel options** (all set in `kernel/config`):

| Config | Purpose |
|--------|---------|
| `CONFIG_SQUASHFS=y` | Read-only OS rootfs (REQUIRED — OS won't boot without it) |
| `CONFIG_OVERLAY_FS=y` | OverlayFS for OS root + container rootfs |
| `CONFIG_CGROUPS=y` | cgroup v2 support |
| `CONFIG_MEMCG=y` | Memory controller |
| `CONFIG_CGROUP_PIDS=y` | PIDs controller |
| `CONFIG_NAMESPACES=y` | All container namespaces |
| `CONFIG_SECCOMP_FILTER=y` | BPF syscall filtering |
| `CONFIG_VIRTIO_NET + BLK=y` | QEMU virtio devices |

---

## Verification

A 211-check phase verification script is included:

```bash
# Verify all 10 phases
bash build/check_phases.sh

# Verify a specific phase
bash build/check_phases.sh --phase=7

# Expected output:
#   PASS: 211
#   WARN: 0
#   FAIL: 0
#   Overall health: 100% (211/211 checks passed)
#   🎉 ALL CHECKS PASSED — MinOS is fully built and ready!
```

---

## Phase Roadmap

| Phase | Component | Status |
|-------|-----------|--------|
| 1 | Dev environment (gcc, musl, QEMU) | ✅ |
| 2 | Kernel config + bzImage | ✅ |
| 3 | Custom init (PID 1) | ✅ |
| 4 | Process supervisor | ✅ |
| 5 | cgroup v2 manager | ✅ |
| 6 | Network daemon (bridge + NAT) | ✅ |
| 7 | Container runtime (minc) | ✅ |
| 8 | Bootable disk image | ✅ |
| 9 | Build system automation | ✅ |
| 10 | Hardening + diagnostic tools | ✅ |

---

## License

MIT
