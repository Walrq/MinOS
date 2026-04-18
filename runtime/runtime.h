#pragma once
#include <sys/types.h>

// Per-container resource and identity configuration.
typedef struct {
    const char  *id;          // container ID e.g. "c0", "c1"
    const char  *binary;      // binary to exec e.g. "/bin/sh"
    const char **argv;        // argv array for execv (argv[0] = binary)
    int          cpu_weight;  // cgroup cpu.weight (1–10000, default 100)
    int          pids_max;    // cgroup pids.max   (max processes in container)
    long         mem_max;     // cgroup memory.max (-1 = unlimited)
    int          tty;         // 1 = allocate controlling terminal (job control)
} ContainerConfig;

// Spawn a container with full namespace isolation.
// Blocks until the container exits. Returns the container's exit status.
int container_spawn(const ContainerConfig *cfg);

// Write uid_map and gid_map for a user namespace.
// Called by the parent after clone(CLONE_NEWUSER).
// Maps container uid 0 → host uid 100000.
void write_uid_map(pid_t pid);

// Install a tight BPF seccomp syscall whitelist.
// Called from inside the container before execv().
void load_seccomp(void);

// In-container namespace setup (called after clone, before exec).
void setup_namespaces(void);

// Set up isolated container rootfs using OverlayFS + pivot_root.
// Called inside the container child (CLONE_NEWNS context) before exec().
// Creates per-container overlay dirs under /containers/<id>/ and pivots
// the process into the new root so the host filesystem is fully hidden.
void setup_rootfs(const char *id);

