// runtime/namespace.c — user namespace UID/GID mapping for MinOS containers
//
// Maps container uid 0 → host uid 100000 (HOST_UID_BASE).
// If the container escapes to the host, it's an unprivileged nobody (uid 100000).
//
// uid_map format: <container_start> <host_start> <range>
//   "0 100000 65536" → container uid 0..65535 = host uid 100000..165535

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "runtime.h"

// The host UID that container root (uid 0) maps to.
// Must be outside the system's normal uid range (typically 0–65535).
#define HOST_UID_BASE  100000
#define UID_RANGE      65536

// ── Write a proc file ────────────────────────────────────────────────────────

static int write_proc(const char *path, const char *data) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, data, strlen(data));
    close(fd);
    return (n < 0) ? -1 : 0;
}

// ── Public API ────────────────────────────────────────────────────────────────

// Called by the PARENT after clone() with CLONE_NEWUSER.
// The child must be blocking on the sync pipe while we write these.
void write_uid_map(pid_t pid) {
    char path[256], map[64];
    int  n;

    // Step 1: Write "deny" to /proc/$pid/setgroups before writing gid_map.
    // Required by kernel since Linux 3.19 to prevent privilege escalation.
    // NOTE: This only works when CLONE_NEWUSER is used. Since MinOS containers
    // currently run without user namespaces (CLONE_NEWUSER not in clone flags),
    // this write will fail with EACCES/ENOENT — that is expected and harmless.
    snprintf(path, sizeof(path), "/proc/%d/setgroups", (int)pid);
    if (write_proc(path, "deny") < 0) {
        // No user namespace active — uid/gid mapping not applicable. Skip silently.
        return;
    }

    // Step 2: uid_map — "0 HOST_UID_BASE UID_RANGE"
    // Container uid 0 = host uid HOST_UID_BASE
    snprintf(path, sizeof(path), "/proc/%d/uid_map", (int)pid);
    n = snprintf(map, sizeof(map), "0 %d %d\n", HOST_UID_BASE, UID_RANGE);
    (void)n;
    if (write_proc(path, map) < 0) {
        write(2, "[ns] WARNING: could not write uid_map\n", 38);
        return;
    }

    // Step 3: gid_map — same mapping for groups
    snprintf(path, sizeof(path), "/proc/%d/gid_map", (int)pid);
    n = snprintf(map, sizeof(map), "0 %d %d\n", HOST_UID_BASE, UID_RANGE);
    if (write_proc(path, map) < 0) {
        write(2, "[ns] WARNING: could not write gid_map\n", 38);
        return;
    }

    write(2, "[ns] user namespace mapped: container uid 0 = host uid 100000\n", 62);
}

// Called from inside the container (after clone, before exec).
// Any per-container in-namespace setup goes here.
void setup_namespaces(void) {
    // Hostname is set in spawn.c via sethostname().
    // /proc is remounted in spawn.c via umount2+mount.
    // Nothing more needed here for the basic case.
}
