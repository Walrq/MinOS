#pragma once

// Write `value` string to a cgroup control file at `path`.
// Silently ignores errors (cgroup files are not always writable).
void cg_write(const char *path, const char *value);

// Parse a human memory string like "512M" or "2G" into bytes.
long parse_mem(const char *s);
