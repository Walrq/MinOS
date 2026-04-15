// tools/nslist.c — list active namespaces, group PIDs by shared namespace inode
//
// Usage (inside MinOS): nslist
// Reads /proc/*/ns/* symlinks and groups processes by shared namespaces.
// Lets you verify containers are isolated from the host.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

#define MAX_PIDS 512

static const char *NS_TYPES[] = { "pid", "net", "mnt", "uts", "ipc", "user", "cgroup" };
#define NS_COUNT  (int)(sizeof(NS_TYPES)/sizeof(NS_TYPES[0]))

typedef struct {
    int            pid;
    char           comm[32];
    unsigned long  ns[7];    // inode for each namespace type
} Proc;

// ── Helpers ───────────────────────────────────────────────────────────────────

static unsigned long ns_inode(int pid, const char *type) {
    char path[256], link[256];
    snprintf(path, sizeof(path), "/proc/%d/ns/%s", pid, type);
    ssize_t n = readlink(path, link, sizeof(link)-1);
    if (n < 0) return 0;
    link[n] = '\0';
    // format: "pid:[4026531836]"
    char *b = strchr(link, '[');
    return b ? strtoul(b+1, NULL, 10) : 0;
}

static void read_comm(int pid, char *buf, size_t size) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(buf, size, "?"); return; }
    if (fgets(buf, size, f)) {
        size_t n = strlen(buf);
        if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
    }
    fclose(f);
}

static int cmp_pid(const void *a, const void *b) {
    return ((Proc*)a)->pid - ((Proc*)b)->pid;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(void) {
    Proc procs[MAX_PIDS];
    int  count = 0;

    // Collect all numeric /proc entries
    DIR *d = opendir("/proc");
    if (!d) { perror("opendir /proc"); return 1; }

    struct dirent *e;
    while ((e = readdir(d)) && count < MAX_PIDS) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end != '\0' || pid <= 0) continue;

        procs[count].pid = (int)pid;
        read_comm((int)pid, procs[count].comm, sizeof(procs[count].comm));
        for (int i = 0; i < NS_COUNT; i++)
            procs[count].ns[i] = ns_inode((int)pid, NS_TYPES[i]);
        count++;
    }
    closedir(d);

    qsort(procs, count, sizeof(Proc), cmp_pid);

    // Get host namespaces from PID 1
    unsigned long host_ns[7] = {0};
    for (int p = 0; p < count; p++) {
        if (procs[p].pid == 1) {
            for (int i = 0; i < NS_COUNT; i++)
                host_ns[i] = procs[p].ns[i];
            break;
        }
    }

    // ── Full namespace table ──────────────────────────────────────────────────
    printf("\n  nslist — MinOS namespace view\n");
    printf("  %s\n", "══════════════════════════════════════════════════════════════════════");
    printf("  %-6s  %-14s  %-12s  %-12s  %-12s  %-12s\n",
           "PID", "COMM", "pid_ns", "mnt_ns", "net_ns", "uts_ns");
    printf("  %-6s  %-14s  %-12s  %-12s  %-12s  %-12s\n",
           "──────", "──────────────", "────────────",
           "────────────", "────────────", "────────────");

    for (int p = 0; p < count; p++) {
        // ns[0]=pid ns[2]=mnt ns[1]=net ns[3]=uts
        printf("  %-6d  %-14s  %-12lu  %-12lu  %-12lu  %-12lu\n",
               procs[p].pid, procs[p].comm,
               procs[p].ns[0], procs[p].ns[2],
               procs[p].ns[1], procs[p].ns[3]);
    }

    // ── Container detection ───────────────────────────────────────────────────
    printf("\n  Isolation check (diff from host PID 1):\n");
    printf("  %s\n", "──────────────────────────────────────────────");

    int isolated = 0;
    for (int p = 0; p < count; p++) {
        if (procs[p].pid == 1) continue;

        // Check if ANY namespace differs from host
        int diff = 0;
        for (int i = 0; i < NS_COUNT; i++) {
            if (procs[p].ns[i] && procs[p].ns[i] != host_ns[i]) { diff = 1; break; }
        }
        if (!diff) continue;

        isolated++;
        printf("  PID %-5d  %-14s  ", procs[p].pid, procs[p].comm);
        for (int i = 0; i < NS_COUNT; i++) {
            if (procs[p].ns[i] && procs[p].ns[i] != host_ns[i])
                printf("[%s isolated] ", NS_TYPES[i]);
        }
        printf("\n");
    }

    if (!isolated)
        printf("  (no isolated processes — no containers running)\n");

    printf("  %s\n\n", "══════════════════════════════════════════════════════════════════════");
    return 0;
}
