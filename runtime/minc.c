// runtime/minc.c
// MinOS Container CLI — the user-facing tool for running containers.
//
// Usage:
//   minc run <binary> [args...]      spawn container (no tty)
//   minc run -t <binary> [args...]   spawn container WITH job control (tty)
//
// -t allocates a controlling terminal so interactive shells work properly
//    (no 'job control turned off' warning from BusyBox ash).
//
// What it does:
//   1. Allocates a container ID (c0, c1, c2, ...)
//   2. Creates /sys/fs/cgroup/containers/<id>/
//   3. Calls container_spawn() → clone() with full namespace isolation
//   4. Cleans up the cgroup when the container exits

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "runtime.h"

// ── Container ID counter ───────────────────────────────────────────────────────
// Persisted in /run/minc.count so IDs increment across invocations.

static int next_id(void) {
    int id = 0;
    mkdir("/run", 0755);    // ensure /run exists

    int fd = open("/run/minc.count", O_RDWR | O_CREAT, 0644);
    if (fd >= 0) {
        char buf[16] = {0};
        if (read(fd, buf, sizeof(buf) - 1) > 0)
            id = atoi(buf) + 1;

        lseek(fd, 0, SEEK_SET);
        char out[16];
        int n = snprintf(out, sizeof(out), "%d", id);
        write(fd, out, n);
        ftruncate(fd, n);
        close(fd);
    }
    return id;
}

// ── Helpers ────────────────────────────────────────────────────────────────────

static void cg_write(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    write(fd, val, strlen(val));
    close(fd);
}

static void print_usage(void) {
    const char *msg =
        "minc — MinOS Container Runtime\n"
        "\n"
        "Usage:\n"
        "  minc run [-t] <binary> [args...]\n"
        "\n"
        "Flags:\n"
        "  -t   allocate controlling terminal (needed for interactive shells)\n"
        "\n"
        "Examples:\n"
        "  minc run -t /bin/sh          interactive shell with job control\n"
        "  minc run /bin/sh             non-interactive shell\n"
        "  minc run /bin/echo hello     run a command\n";
    write(1, msg, strlen(msg));
}

// ── Main ───────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[1], "run") != 0) {
        print_usage();
        return 1;
    }

    // Parse optional -t flag: minc run -t <binary> [args...]
    int tty        = 0;
    int binary_idx = 2;   // index of binary in argv[]

    if (argc > 3 && strcmp(argv[2], "-t") == 0) {
        tty        = 1;
        binary_idx = 3;
    }

    if (binary_idx >= argc) {
        print_usage();
        return 1;
    }

    // Generate container ID
    int num = next_id();
    char id[32];
    snprintf(id, sizeof(id), "c%d", num);

    // Argv for the container starts at binary_idx
    const char **ctr_argv = (const char **)&argv[binary_idx];

    ContainerConfig cfg = {
        .id         = id,
        .binary     = argv[binary_idx],
        .argv       = ctr_argv,
        .cpu_weight = 100,
        .pids_max   = 32,
        .mem_max    = -1,
        .tty        = tty,
    };

    // Create cgroup directory for this container
    char cg_path[256];
    snprintf(cg_path, sizeof(cg_path), "/sys/fs/cgroup/containers/%s", id);
    mkdir(cg_path, 0755);

    // Print startup banner
    char banner[128];
    int n = snprintf(banner, sizeof(banner),
        "[minc] container %s starting: %s\n", id, cfg.binary);
    write(1, banner, n);

    // Launch the container (blocks until it exits)
    int exit_code = container_spawn(&cfg);

    // Cleanup: remove cgroup directory (must be empty — kernel enforces this)
    rmdir(cg_path);

    n = snprintf(banner, sizeof(banner),
        "[minc] container %s exited with code %d\n", id, exit_code);
    write(1, banner, n);

    return exit_code;
}
