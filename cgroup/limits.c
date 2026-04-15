// cgroup/limits.c
// Low-level helpers: write to cgroup control files, parse memory strings.

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "limits.h"

// Write a value string to a cgroup control file.
// e.g. cg_write("/sys/fs/cgroup/containers/app/memory.max", "67108864");
void cg_write(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    write(fd, value, strlen(value));
    close(fd);
}

// Parse "512M" -> 536870912, "2G" -> 2147483648, "max" -> -1
long parse_mem(const char *s) {
    if (!s || s[0] == 'm') return -1;   // "max"

    long val = atol(s);
    // Walk past the digits to find the unit suffix
    const char *p = s;
    while (*p == '-' || (*p >= '0' && *p <= '9')) p++;

    if      (*p == 'K' || *p == 'k') val *= 1024L;
    else if (*p == 'M' || *p == 'm') val *= 1024L * 1024;
    else if (*p == 'G' || *p == 'g') val *= 1024L * 1024 * 1024;

    return val;
}
