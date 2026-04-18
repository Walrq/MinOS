// runtime/rootfs.c
// Sets up an isolated rootfs for each container using:
//   1. OverlayFS  — read-only lower (squashfs image) + writable upper (scratch dir)
//   2. pivot_root — makes the overlay the new / (fully replacing host rootfs)
//   3. Unmounts   — old root is unmounted so host FS is invisible
//
// Directory layout created per container (under /containers/<id>/):
//   lower/   → bind-mount of the OS squashfs rootfs (read-only base image)
//   upper/   → writable scratch layer (container's writes go here)
//   work/    → overlayfs internal workdir (must be on same fs as upper)
//   merged/  → the combined overlayfs mount point → becomes new /
//   oldroot/ → temporary mount point to hold host / during pivot_root

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>   // SYS_pivot_root

#include "runtime.h"

// Wrapper — pivot_root has no glibc wrapper in musl/glibc-static builds
static int pivot_root(const char *new_root, const char *put_old) {
    return (int)syscall(SYS_pivot_root, new_root, put_old);
}

// mkdir -p equivalent: create a directory and all parents (mode 0755)
static int mkdirp(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);   // ignore EEXIST
            *p = '/';
        }
    }
    return mkdir(path, 0755);
}

// Base path where all container working directories live.
// IMPORTANT: upper + work for each container's overlayfs MUST be on a real
// filesystem (tmpfs), NOT on the OS overlayfs itself. Linux refuses to use
// a directory that sits on overlayfs as the upper dir of another overlay.
// We mount a per-container tmpfs, then create upper/ and work/ inside it.
#define CONTAINER_BASE  "/run/containers"
// Path to the OS squashfs rootfs (mounted read-only as the lower layer).
// Stage-1 initramfs mounts /dev/vda2 (squashfs) at /squash before switch_root.
// After switch_root the squashfs stays accessible at /squash in the new root.
#define SQUASHFS_LOWER  "/squash"

// setup_rootfs() — called inside the container child BEFORE exec.
// Must be called from within a CLONE_NEWNS mount namespace.
//
// On success: the process is now fully inside a new rootfs.
// On failure: writes error to stderr and calls _exit(1).
void setup_rootfs(const char *id) {
    char lower[512], upper[512], work[512], merged[512], oldroot[512];
    char opt[1024];

    // ── Build per-container directory paths ───────────────────────────────────
    snprintf(lower,   sizeof(lower),   "%s/%s/lower",   CONTAINER_BASE, id);
    snprintf(upper,   sizeof(upper),   "%s/%s/upper",   CONTAINER_BASE, id);
    snprintf(work,    sizeof(work),    "%s/%s/work",    CONTAINER_BASE, id);
    snprintf(merged,  sizeof(merged),  "%s/%s/merged",  CONTAINER_BASE, id);
    snprintf(oldroot, sizeof(oldroot), "%s/%s/merged/oldroot", CONTAINER_BASE, id);

    // ── 1. Create base dir + mount tmpfs for upper/work ─────────────────────
    // The OS root is an overlayfs. Linux does not allow a directory on an
    // overlayfs to be used as the upper dir of another overlayfs.
    // Solution: mount a per-container tmpfs first; upper/ and work/ live
    // there (tmpfs is a real fs → kernel permits it as overlay upper).
    char tmpbase[512];
    snprintf(tmpbase, sizeof(tmpbase), "%s/%s", CONTAINER_BASE, id);

    if (mkdirp(tmpbase) < 0 && errno != EEXIST) goto fail_mkdir;

    // Mount tmpfs for this container's scratch space (size capped at 256 MB)
    if (mount("tmpfs", tmpbase, "tmpfs", 0, "size=256m,mode=0755") < 0) {
        perror("rootfs: mount tmpfs for container scratch");
        goto fail;
    }

    // Now create upper/ work/ merged/ INSIDE the tmpfs — these are on a real fs
    if (mkdirp(lower)  < 0 && errno != EEXIST) goto fail_mkdir;
    if (mkdirp(upper)  < 0 && errno != EEXIST) goto fail_mkdir;
    if (mkdirp(work)   < 0 && errno != EEXIST) goto fail_mkdir;
    if (mkdirp(merged) < 0 && errno != EEXIST) goto fail_mkdir;

    // ── 2. Bind-mount the read-only squashfs rootfs as the lower layer ────────
    // SQUASHFS_LOWER is already mounted read-only by initramfs-stage1 / init.
    // We bind it so overlayfs can use it as the read-only base image.
    if (mount(SQUASHFS_LOWER, lower, NULL, MS_BIND | MS_RDONLY, NULL) < 0) {
        // If squashfs isn't available (dev/test mode), bind-mount host / as fallback
        if (mount("/", lower, NULL, MS_BIND | MS_RDONLY, NULL) < 0) {
            perror("rootfs: bind lower");
            goto fail;
        }
    }

    // ── 3. Mount OverlayFS ────────────────────────────────────────────────────
    // lowerdir = read-only base (squashfs or host /)
    // upperdir = writable scratch (container's private writes)
    // workdir  = overlayfs internal state (must be on same fs as upperdir)
    // merged   = the combined view — this becomes the new /
    snprintf(opt, sizeof(opt),
             "lowerdir=%s,upperdir=%s,workdir=%s", lower, upper, work);

    if (mount("overlay", merged, "overlay", MS_NOSUID | MS_NODEV, opt) < 0) {
        perror("rootfs: mount overlay");
        goto fail;
    }

    // ── 4. Prepare oldroot inside merged (pivot_root will put old / here) ─────
    if (mkdirp(oldroot) < 0 && errno != EEXIST) {
        perror("rootfs: mkdir oldroot");
        goto fail;
    }

    // ── 5. pivot_root: swap merged → / and put old / → oldroot ───────────────
    // After this call: merged is /, old host / is accessible at /oldroot.
    if (pivot_root(merged, oldroot) < 0) {
        perror("rootfs: pivot_root");
        goto fail;
    }

    // ── 6. chdir to the new / ─────────────────────────────────────────────────
    if (chdir("/") < 0) {
        perror("rootfs: chdir /");
        goto fail;
    }

    // ── 7. Unmount old root (host filesystem is now invisible) ────────────────
    // MNT_DETACH: lazy unmount — detach immediately, clean up when last user leaves.
    // This ensures the container cannot see or access any host filesystem paths.
    if (umount2("/oldroot", MNT_DETACH) < 0) {
        // Non-fatal: in some environments (nested virt, missing perms) this fails.
        // The container still runs inside the overlay; host paths just remain visible.
        // Log the warning but don't abort.
        const char *msg = "rootfs: warning: could not unmount oldroot (host fs still visible)\n";
        write(2, msg, strlen(msg));
    }

    // ── 8. Remount proc for our PID namespace ────────────────────────────────
    // Detach inherited /proc then mount a fresh one scoped to our PID ns.
    umount2("/proc", MNT_DETACH);  // ignore error — might not be mounted yet
    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) {
        const char *msg = "rootfs: warning: /proc remount failed (PIDs may show as host)\n";
        write(2, msg, strlen(msg));
    }

    // ── 9. Mount /dev inside container ───────────────────────────────────
    // Mount a fresh devtmpfs so the container has /dev/null, /dev/zero etc.
    // Also mount /dev/pts for proper tty/pty support (fixes 'can't access tty').
    mount("devtmpfs", "/dev", "devtmpfs",
          MS_NOSUID | MS_STRICTATIME, "mode=0755");
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts",
          MS_NOSUID | MS_NOEXEC, "mode=0620,ptmxmode=0666");

    return;   // Success — caller proceeds to exec()

fail_mkdir:
    perror("rootfs: mkdirp");
fail:
    _exit(1);
}
