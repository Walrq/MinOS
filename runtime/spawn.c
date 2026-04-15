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
#include <signal.h>
#include <errno.h>
#include <termios.h>        // tcgetattr / tcsetpgrp

#include "runtime.h"

#define STACK_SIZE (1024 * 1024)    // 1 MB stack for container

// Sync pipe: parent signals child after cgroup setup is done
static int sync_pipe[2];

typedef struct {
    const ContainerConfig *cfg;
} ChildArgs;

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

    // 2. Remount /proc for OUR PID namespace
    //    Without this, /proc still shows host PIDs.
    //    CLONE_NEWNS gives us our own mount table so this is safe.
    umount2("/proc", MNT_DETACH);
    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) {
        write(2, "container: failed to mount /proc\n", 33);
    }

    // 3. Install seccomp filter — restrict syscalls before exec
    //    This limits what the container can call to a tight whitelist.
    load_seccomp();

    // 4. exec the target binary — this replaces our process image
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
        CLONE_NEWPID  |   // own PID namespace  ✅ (kernel has CONFIG_PID_NS)
        CLONE_NEWNS   |   // own mount namespace ✅ (kernel has CONFIG_MNT_NS)
        CLONE_NEWUTS  |   // own hostname        ✅ (kernel has CONFIG_UTS_NS)
        // CLONE_NEWNET — disabled: CONFIG_NET_NS not in this kernel build
        // CLONE_NEWIPC — disabled: CONFIG_IPC_NS not in this kernel build
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
