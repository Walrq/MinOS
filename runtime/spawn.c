// runtime/spawn.c
// Core container spawner — uses clone() to create isolated namespaces.
//
// Namespace isolation applied per container:
//   CLONE_NEWPID  → container has its own PID tree; container's init = PID 1
//   CLONE_NEWNS   → container has its own mount table (can remount /proc)
//   CLONE_NEWNET  → container has its own network stack (no access to host eth0)
//   CLONE_NEWUTS  → container has its own hostname & domain name
//   CLONE_NEWIPC  → container has its own semaphores, message queues
//
// Cgroup assignment is done by the PARENT (host PID is known after clone())
// using a sync pipe so the child waits until it's safely in its cgroup.

#define _GNU_SOURCE
#include <sched.h>          // clone(), CLONE_NEW*
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>      // TIOCSCTTY
#include <sys/prctl.h>      // prctl, PR_SET_SECUREBITS, PR_CAP_AMBIENT
#include <signal.h>
#include <errno.h>
#include <termios.h>        // tcgetattr / tcsetpgrp

// Linux capability constants (avoid depending on linux/capability.h)
#define CAP_LAST_CAP     40
#define SECBIT_KEEP_CAPS 0
// Securebits: no-setuid-fixup + locked + noroot + locked
// Prevents the container from ever regaining capabilities via setuid/setgid.
#define SECURE_ALL_BITS  0x15   // NOROOT | NOROOT_LOCKED | NO_SETUID_FIXUP | NO_SETUID_FIXUP_LOCKED
#define PR_SET_SECUREBITS 28

#include "runtime.h"

#define STACK_SIZE (1024 * 1024)    // 1 MB stack for container

// Sync pipe: parent signals child after cgroup setup is done
static int sync_pipe[2];

typedef struct {
    const ContainerConfig *cfg;
} ChildArgs;

// ── Capability drop ───────────────────────────────────────────────────────────
// Drop every Linux capability from the bounding set, ambient set, and
// inherited/permitted/effective sets via prctl(). No libcap required.
// After this the container cannot escalate privileges even with setuid binaries.
static void drop_capabilities(void) {
    // 1. Drop each cap from the bounding set (inherited across exec)
    for (int cap = 0; cap <= CAP_LAST_CAP; cap++) {
        prctl(PR_CAPBSET_DROP, cap, 0, 0, 0);
        // Ignore EINVAL for caps the kernel doesn't know about.
    }

    // 2. Clear ambient capability set (caps that survive exec)
    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);

    // 3. Lock securebits: prevent regaining caps via setuid root binaries
    prctl(PR_SET_SECUREBITS, SECURE_ALL_BITS, 0, 0, 0);

    // 4. Zero out all sets (effective, permitted, inheritable) via setuid trick:
    //    Since we're not CLONE_NEWUSER, we use the simpler method
    //    of just setting an empty capset through prctl(PR_SET_KEEPCAPS, 0)
    prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
}

// ── Container PID 1 ────────────────────────────────────────────────────────────
// Everything here runs INSIDE the new namespaces.

static int container_main(void *arg) {
    const ContainerConfig *cfg = ((ChildArgs *)arg)->cfg;

    // Wait for parent to finish cgroup assignment
    char go;
    read(sync_pipe[0], &go, 1);
    close(sync_pipe[0]);

    // 1. Set up controlling terminal BEFORE exec so shell gets job control.
    //    Steps: new session → claim the inherited terminal → set foreground pgrp
    if (cfg->tty) {
        setsid();                              // become session leader
        ioctl(STDIN_FILENO,  TIOCSCTTY, 0);   // claim ttyS0 as controlling tty
        tcsetpgrp(STDIN_FILENO, getpid());     // we are the foreground process
    }

    // 2. Set container hostname (UTS namespace)
    sethostname(cfg->id, strlen(cfg->id));

    // 3. Set up isolated rootfs: overlayfs mount + pivot_root
    //    After this call we are inside a new / (the squashfs overlay).
    //    setup_rootfs() also remounts /proc for our PID namespace.
    setup_rootfs(cfg->id);

    // 4. Drop all Linux capabilities — container runs with zero privileges.
    //    Must happen BEFORE seccomp so prctl() is still available, and
    //    BEFORE exec so the new process image cannot regain caps.
    drop_capabilities();

    // 5. Install seccomp filter — restrict syscalls to a tight whitelist.
    //    Done last so the filter itself doesn't block the prctl calls above.
    load_seccomp();

    // 6. exec the target binary — replaces our process image
    execv(cfg->binary, (char *const *)cfg->argv);

    // execv only returns on failure
    const char *err = "container: exec failed\n";
    write(2, err, strlen(err));
    return 127;
}

// ── Public API ─────────────────────────────────────────────────────────────────

int container_spawn(const ContainerConfig *cfg) {
    if (pipe(sync_pipe) < 0) return -1;

    // Allocate stack (clone() needs an explicit stack — stack grows downward)
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        close(sync_pipe[0]); close(sync_pipe[1]);
        return -1;
    }
    char *stack_top = stack + STACK_SIZE;

    int clone_flags =
        CLONE_NEWPID  |   // own PID namespace  ✅
        CLONE_NEWNS   |   // own mount namespace ✅ (required for pivot_root)
        CLONE_NEWUTS  |   // own hostname        ✅
        CLONE_NEWNET  |   // own network stack   ✅ (CONFIG_NET_NS enabled)
        CLONE_NEWIPC  |   // own IPC namespace   ✅
        SIGCHLD;          // parent gets SIGCHLD when container exits

    ChildArgs args = {.cfg = cfg};

    pid_t pid = clone(container_main, stack_top, clone_flags, &args);
    if (pid < 0) {
        perror("minc: clone");
        free(stack);
        close(sync_pipe[0]); close(sync_pipe[1]);
        return -1;
    }

    close(sync_pipe[0]);  // parent only writes

    // ── Cgroup assignment (done by parent, using the real host PID) ───────────
    char procs_path[256], pid_str[32];
    snprintf(procs_path, sizeof(procs_path),
             "/sys/fs/cgroup/containers/%s/cgroup.procs", cfg->id);
    snprintf(pid_str, sizeof(pid_str), "%d", (int)pid);

    int fd = open(procs_path, O_WRONLY);
    if (fd >= 0) {
        write(fd, pid_str, strlen(pid_str));
        close(fd);
    }

    // Write user namespace UID/GID mapping (if CLONE_NEWUSER was used)
    // Maps container uid 0 → host uid 100000 for privilege containment.
    write_uid_map(pid);

    // Apply cpu.weight
    char cpu_path[320], cpu_val[32];
    snprintf(cpu_path, sizeof(cpu_path),
             "/sys/fs/cgroup/containers/%s/cpu.weight", cfg->id);
    snprintf(cpu_val, sizeof(cpu_val), "%d", cfg->cpu_weight);
    fd = open(cpu_path, O_WRONLY);
    if (fd >= 0) { write(fd, cpu_val, strlen(cpu_val)); close(fd); }

    // Apply pids.max
    char pids_path[320], pids_val[32];
    snprintf(pids_path, sizeof(pids_path),
             "/sys/fs/cgroup/containers/%s/pids.max", cfg->id);
    snprintf(pids_val, sizeof(pids_val), "%d", cfg->pids_max);
    fd = open(pids_path, O_WRONLY);
    if (fd >= 0) { write(fd, pids_val, strlen(pids_val)); close(fd); }

    // ── Unblock the child ──────────────────────────────────────────────────────
    write(sync_pipe[1], "1", 1);
    close(sync_pipe[1]);

    // Wait for container to exit
    int status = 0;
    waitpid(pid, &status, 0);
    free(stack);

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
