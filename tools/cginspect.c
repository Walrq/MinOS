// tools/cginspect.c — walk /sys/fs/cgroup/, print per-cgroup resource stats
//
// Usage (inside MinOS): cginspect
// Shows memory, PID count, and CPU time for every active cgroup.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define CGROOT   "/sys/fs/cgroup"
#define COL_NAME  30
#define COL_MEM   14
#define COL_PIDS   6

// ── Helpers ───────────────────────────────────────────────────────────────────

static void read_first_line(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(buf, size, "-"); return; }
    if (!fgets(buf, size, f)) snprintf(buf, size, "-");
    size_t n = strlen(buf);
    if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
    fclose(f);
}

static void read_stat_field(const char *path, const char *field,
                             char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(buf, size, "-"); return; }
    char line[256];
    size_t flen = strlen(field);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, field, flen) == 0) {
            const char *val = line + flen;
            while (*val == ' ' || *val == '\t') val++;
            snprintf(buf, size, "%s", val);
            size_t n = strlen(buf);
            if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
            fclose(f);
            return;
        }
    }
    fclose(f);
    snprintf(buf, size, "-");
}

static void fmt_bytes(const char *raw, char *out, size_t size) {
    if (strcmp(raw, "-") == 0 || strcmp(raw, "max") == 0) {
        snprintf(out, size, "%s", raw); return;
    }
    char *end;
    long long v = strtoll(raw, &end, 10);
    if (*end != '\0') { snprintf(out, size, "%s", raw); return; }
    if      (v >= 1024LL*1024*1024) snprintf(out, size, "%.1fG", (double)v/(1024.0*1024*1024));
    else if (v >= 1024*1024)        snprintf(out, size, "%.1fM", (double)v/(1024.0*1024));
    else if (v >= 1024)             snprintf(out, size, "%.1fK", (double)v/1024.0);
    else                            snprintf(out, size, "%lldB", v);
}

static void fmt_usec(const char *raw, char *out, size_t size) {
    if (strcmp(raw, "-") == 0) { snprintf(out, size, "-"); return; }
    char *end;
    long long v = strtoll(raw, &end, 10);
    if (*end != '\0') { snprintf(out, size, "%s", raw); return; }
    if      (v >= 1000000LL) snprintf(out, size, "%.2fs",   (double)v/1000000.0);
    else if (v >= 1000)      snprintf(out, size, "%.1fms",  (double)v/1000.0);
    else                     snprintf(out, size, "%lldus", v);
}

// ── Per-cgroup print ──────────────────────────────────────────────────────────

static void print_cgroup(const char *path, int depth) {
    char fpath[512], mem_raw[64], pids_raw[32], cpu_raw[64];

    snprintf(fpath, sizeof(fpath), "%s/memory.current", path);
    read_first_line(fpath, mem_raw, sizeof(mem_raw));

    snprintf(fpath, sizeof(fpath), "%s/pids.current", path);
    read_first_line(fpath, pids_raw, sizeof(pids_raw));

    snprintf(fpath, sizeof(fpath), "%s/cpu.stat", path);
    read_stat_field(fpath, "usage_usec", cpu_raw, sizeof(cpu_raw));

    char mem_fmt[32], cpu_fmt[32];
    fmt_bytes(mem_raw, mem_fmt, sizeof(mem_fmt));
    fmt_usec(cpu_raw, cpu_fmt, sizeof(cpu_fmt));

    const char *rel = path + strlen(CGROOT);
    if (*rel == '/') rel++;
    if (*rel == '\0') rel = "(root)";

    int pad = depth * 2;
    printf("  %*s%-*s  %-*s  %-*s  %s\n",
           pad, "", COL_NAME - pad, rel,
           COL_MEM, mem_fmt, COL_PIDS, pids_raw, cpu_fmt);
}

// ── Recursive walker ──────────────────────────────────────────────────────────

static int dirent_alpha(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

static void walk(const char *dirpath, int depth) {
    print_cgroup(dirpath, depth);

    struct dirent **entries;
    int n = scandir(dirpath, &entries, NULL, dirent_alpha);
    if (n < 0) return;

    for (int i = 0; i < n; i++) {
        struct dirent *e = entries[i];
        if (e->d_name[0] != '.' && e->d_type == DT_DIR) {
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", dirpath, e->d_name);
            walk(child, depth + 1);
        }
        free(e);
    }
    free(entries);
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(void) {
    printf("\n  cginspect — MinOS cgroup resource view\n");
    printf("  %s\n", "══════════════════════════════════════════════════════════════");
    printf("  %-*s  %-*s  %-*s  %s\n",
           COL_NAME, "CGROUP", COL_MEM, "MEMORY", COL_PIDS, "PIDS", "CPU TIME");
    printf("  %-*s  %-*s  %-*s  %s\n",
           COL_NAME, "──────────────────────────────",
           COL_MEM,  "──────────────",
           COL_PIDS, "──────", "────────────────");

    walk(CGROOT, 0);

    printf("  %s\n\n", "══════════════════════════════════════════════════════════════");
    return 0;
}
