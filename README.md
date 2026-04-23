# MinOS

A minimal, from-scratch Linux-based operating system with an integrated container runtime, built as an OS course project.

MinOS boots a custom kernel into a custom C `init`, runs a service supervisor, and provides `minc` — a lightweight container runtime with full namespace isolation, OverlayFS rootfs, cgroup v2 resource limits, capability dropping, and a BPF seccomp whitelist.

---

## Features

- **Custom kernel** — minimal Linux config with cgroup v2, namespaces, SquashFS, OverlayFS, virtio, and seccomp
- **Custom init (PID 1)** — written in C; mounts `/proc /sys /dev`, sets up cgroup v2, launches the supervisor
- **Service supervisor** — reads `services.d/`, spawns services, monitors with `waitpid`, auto-restarts on failure
- **cgroup v2 resource manager (`cgmgr`)** — one-shot setup: creates `system/` and `containers/` slices with cpu/memory/pids limits
- **Network daemon (`netd`)** — creates `osBr0` bridge at `10.0.0.1/24`, enables IP forwarding and NAT masquerade
- **Container runtime (`minc`)** — runs isolated containers with:
  - `CLONE_NEWPID` · `CLONE_NEWNS` · `CLONE_NEWUTS` · `CLONE_NEWNET` · `CLONE_NEWIPC`
  - OverlayFS rootfs (SquashFS lower + tmpfs upper/work + `pivot_root`)
  - Capability drop (all 41 bounding set caps cleared, ambient cleared, securebits locked)
  - BPF seccomp syscall whitelist
  - cgroup v2 assignment for CPU / memory / PIDs limits (`-m <mb>` flag)
- **Diagnostic tools** — `cginspect` (cgroup tree viewer), `nslist` (namespace inspector)
- **Demo suite** — `demo`, `sysmon`, `pyrun`, `memhog` — see [Demo](#demo)
- **Two-stage initramfs** — stage-1 shell script mounts `/dev/vda1` (ext4) + `/dev/vda2` (SquashFS), builds an OverlayFS root, then `switch_root`s into the OS
- **Bootable disk image** — 512 MB raw image with extlinux bootloader, fully built from the Makefile

---

## Architecture

```
myos.img (512 MB raw disk)
├── /dev/vda1  ext4          ← boot partition: extlinux + bzImage + stage-1 initramfs
└── /dev/vda2  SquashFS      ← read-only OS root (rootfs/)

Boot sequence:
  BIOS → extlinux → bzImage
    → initramfs-stage1/init (shell)
        mounts vda1 (ext4) + vda2 (squashfs)
        overlayfs: lower=/squash  upper=/mnt/overlay/upper (ext4)
        switch_root /newroot /sbin/init
    → init/main.c  (PID 1)
        mount /proc /sys /dev /tmp
        enable cgroup v2 controllers
        exec supervisor
    → supervisor/main.c
        spawn: cgmgr (one-shot)  netd (one-shot)  console (/bin/sh)
    → /bin/sh  (interactive console)
```

```
Container lifecycle (minc run -t -m 10 /bin/sh):
  clone(CLONE_NEWPID|NS|UTS|NET|IPC)
    wait for cgroup assignment (sync pipe)
    write pid → /sys/fs/cgroup/containers/<id>/cgroup.procs
    write memory.max, pids.max, cpu.weight
    setup_rootfs():
      mount tmpfs → /run/containers/<id>/
      bind /squash → lower/
      overlayfs lower+upper+work → merged/
      pivot_root merged/ → /   (host fs now invisible)
      mount proc, devtmpfs, devpts
    drop_capabilities()    ← bounding + ambient + securebits
    load_seccomp()         ← BPF whitelist
    execv(binary)
```

---

## Directory Structure

```
MinOS/
├── init/               PID 1 — mount, signal, reaper
├── supervisor/         Service supervisor — parser, spawn, reaper
├── cgroup/             cgmgr — cgroup v2 slice manager
├── netd/               Network daemon — bridge + NAT
├── runtime/            minc container runtime
│   ├── minc.c          CLI + flag parsing (-t, -m)
│   ├── spawn.c         clone() + cgroup assignment + capability drop
│   ├── rootfs.c        OverlayFS setup + pivot_root
│   ├── namespace.c     UID/GID map helpers
│   ├── seccomp.c       BPF syscall whitelist
│   └── runtime.h       Shared types and declarations
├── tools/
│   ├── cginspect.c     cgroup hierarchy inspector
│   ├── nslist.c        namespace inspector
│   ├── memhog.c        Memory allocator for OOM demo
│   └── demo.sh         7-test capability demo script
├── projects/
│   ├── sysmon/         Live container dashboard (auto OOM stress test)
│   └── pyrun/          Container-gated Python runner demo
├── services.d/         Service configs for supervisor
│   ├── cgmgr.conf
│   ├── netd.conf
│   └── console.conf
├── initramfs-stage1/   Stage-1 init shell script
├── kernel/             kernel/config tracked; bzImage gitignored
├── build/              All build and test scripts
│   ├── build-all.sh        Full pipeline script
│   ├── mkdisk.sh           Disk image assembler
│   ├── mksquashfs.sh       SquashFS builder
│   ├── mkstage1.sh         Stage-1 initramfs builder
│   ├── mkrootfs.sh         Rootfs compiler
│   ├── qemu.sh             Boot VM (socat console, no lag)
│   ├── console.sh          Re-attach console to running VM
│   ├── verify.sh           Build artifact verification
│   └── fix_kernel_config.sh Kernel config patcher + rebuilder
├── syslinux/           Bootloader config
├── rootfs/             Compiled binaries staging area (gitignored)
├── Makefile            Top-level build system
└── README.md
```

---

## Prerequisites

```bash
# Ubuntu / WSL2
sudo apt update && sudo apt install -y \
  gcc make flex bison bc libssl-dev libelf-dev \
  qemu-system-x86_64 socat \
  squashfs-tools \
  syslinux syslinux-common extlinux \
  dosfstools e2fsprogs
```

> ⚠️ **Build must be run inside WSL2 (Ubuntu)**. The scripts use Linux-specific tools (`losetup`, `mount`, `mksquashfs`, etc.).

> ⚠️ For KVM acceleration (strongly recommended), enable nested virtualization in `%USERPROFILE%\.wslconfig`:
> ```ini
> [wsl2]
> nestedVirtualization=true
> ```
> Then run `wsl --shutdown` and reopen WSL.

---

## Quick Start

```bash
git clone https://github.com/<you>/MinOS.git
cd MinOS

# 1. Build kernel first (see Kernel section below), then:
make

# 2. Boot in QEMU (uses socat for zero-lag console)
make test
# → MinOS boots to  ~ #

# 3. Run a container
minc run /bin/echo "hello from container"

# 4. Interactive container shell
minc run -t /bin/sh

# 5. Memory-limited container
minc run -t -m 10 /bin/sh
```

---

## Running Containers

All commands run from the MinOS shell (`~ #`) inside QEMU.

```sh
# Basic
minc run /bin/echo "hello from container"
minc run -t /bin/sh                        # interactive shell

# With memory limit (MB)
minc run -t -m 10 /bin/sh                 # 10 MB hard limit
minc run -m 10 /bin/memhog 30             # OOM kill demo

# Inspect cgroup hierarchy
cginspect
ls /sys/fs/cgroup/containers/

# Inspect namespaces
nslist
```

Each container gets an auto-incrementing ID (`c0`, `c1`, …) and full isolation:

```sh
hostname          # → c1   (UTS namespace)
ps                # → only sees its own PIDs (PID namespace)
ls /              # → container OverlayFS root (not host files)
```

---

## Demo

### Full capability demo (7 isolation tests)
```sh
minc run -t -m 10 /bin/demo
```
Tests: UTS · PID · Mount/OverlayFS · Network · Capability drop · PIDs cgroup · Memory OOM kill

### Live container dashboard
```sh
minc run -t -m 10 /bin/sysmon
```
Live terminal UI: memory bar, PID bar, isolated process list. Auto-stresses memory every 20 seconds — watch the bar go red and the kernel OOM-kill the hog.

### Container-enforced Python runner
```sh
# Blocked on host:
/bin/pyrun
# → ACCESS DENIED — Container check failed

# Works inside a container:
minc run -t /bin/pyrun 10 32
# → shows add.py source + computes 10 + 32 = 42
minc run -t /bin/pyrun 7 93
```
`pyrun` reads `/proc/self/cgroup` and refuses to execute unless the cgroup path starts with `/containers/`.

### OOM kill demo
```sh
minc run -m 10 /bin/memhog 30
# → allocates 1MB at a time, watch it get SIGKILL'd at ~10MB
# → exit code 137 = 128 + SIGKILL
```

---

## Build Targets

```bash
make              # build everything: init, supervisor, daemons, tools, projects, image
make init         # compile init (PID 1)
make daemons      # compile supervisor, cgmgr, netd, minc
make tools        # compile cginspect, nslist, memhog, demo, sysmon, pyrun
make image        # build squashfs + disk image (forces rebuild)
make test         # boot myos.img in QEMU (interactive, socat console)
make boot-log     # headless boot, output → build/boot.log
make verify       # run artifact verification checks
make clean        # remove all build artifacts
```

---

## Kernel

The kernel binary (`kernel/bzImage`, ~14 MB) is gitignored. Build from Linux source:

```bash
# 1. Download Linux source
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.1.tar.xz
tar -xf linux-6.1.tar.xz

# 2. Apply MinOS config + resolve dependencies
cp kernel/config linux-6.1/.config
cd linux-6.1 && make olddefconfig

# 3. Build
make bzImage -j$(nproc)
cp arch/x86/boot/bzImage ../MinOS/kernel/bzImage

# Or use the automated script (also rebuilds the disk image):
bash build/fix_kernel_config.sh
```

**Critical kernel options** (all set in `kernel/config`):

| Config | Purpose |
|--------|---------|
| `CONFIG_SQUASHFS=y` | Read-only OS rootfs (**REQUIRED** — OS won't boot without it) |
| `CONFIG_OVERLAY_FS=y` | OverlayFS for OS root + container rootfs |
| `CONFIG_CGROUPS=y` | cgroup v2 support |
| `CONFIG_MEMCG=y` | Memory controller (enables OOM kill) |
| `CONFIG_CGROUP_PIDS=y` | PIDs controller |
| `CONFIG_NAMESPACES=y` | All container namespaces |
| `CONFIG_SECCOMP_FILTER=y` | BPF syscall filtering |
| `CONFIG_VIRTIO_NET=y` + `CONFIG_VIRTIO_BLK=y` | QEMU virtio devices |
| `CONFIG_USER_NS=y` | User namespace (UID/GID mapping) |

---

## Verification

```bash
# Quick artifact check
bash build/verify.sh

# Phase-by-phase deep verification (211 checks)
bash build/check_phases.sh

# Expected:
#   PASS: 211   WARN: 0   FAIL: 0
#   Overall health: 100%
```

---

## Phase Roadmap

| Phase | Component | Status |
|-------|-----------|--------|
| 1 | Dev environment (gcc, QEMU, tools) | ✅ |
| 2 | Kernel config + bzImage | ✅ |
| 3 | Custom init (PID 1) | ✅ |
| 4 | Process supervisor | ✅ |
| 5 | cgroup v2 manager | ✅ |
| 6 | Network daemon (bridge + NAT) | ✅ |
| 7 | Container runtime (minc) | ✅ |
| 8 | Bootable disk image | ✅ |
| 9 | Build system automation | ✅ |
| 10 | Hardening + diagnostic tools | ✅ |
| 11 | Demo suite (sysmon, pyrun, memhog) | ✅ |

---

## License

MIT
