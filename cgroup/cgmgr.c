// cgroup/cgmgr.c
// Cgroup Manager — runs once at boot as a supervisor service.
//
// Responsibilities:
//   1. Enable subtree_control on the containers/ slice so per-container
//      cgroups inherit the memory/cpu/pids controllers.
//   2. Parse /slices.conf and apply cpu.weight + memory.max to each slice.
//
// After setup is done, the process exits cleanly (restart=on-failure
// in cgmgr.conf means supervisor won't restart a clean exit).

#include "limits.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CGROUP_ROOT "/sys/fs/cgroup"
#define SLICES_CONF "/slices.conf"

// Enable cgroup controllers at a given path, one at a time.
// Writing all in one shot fails atomically if any controller is missing
// (e.g. +memory when CONFIG_MEMCG=n). One-at-a-time is safe.
static void enable_controllers(const char *cgroup_path) {
  char ctrl[320];
  snprintf(ctrl, sizeof(ctrl), "%s/cgroup.subtree_control", cgroup_path);
  cg_write(ctrl, "+cpu");    // CPU weight scheduling
  cg_write(ctrl, "+pids");   // process count limits
  cg_write(ctrl, "+memory"); // optional — needs CONFIG_MEMCG=y
}

// Apply limits for a single slice from a parsed slices.conf line.
// slice_name: "system" or "containers"
// cpu_weight:  integer (1-10000, kernel default is 100)
// mem_max_s:   "512M", "2G", or "max"
static void apply_slice(const char *slice_name, int cpu_weight,
                        const char *mem_max_s) {

  char path[256];
  snprintf(path, sizeof(path), "%s/%s", CGROUP_ROOT, slice_name);

  // --- cpu.weight ---
  char cpu_file[320], cpu_val[32];
  snprintf(cpu_file, sizeof(cpu_file), "%s/cpu.weight", path);
  snprintf(cpu_val, sizeof(cpu_val), "%d", cpu_weight);
  cg_write(cpu_file, cpu_val);

  // --- memory.max ---
  char mem_file[320];
  snprintf(mem_file, sizeof(mem_file), "%s/memory.max", path);

  if (strcmp(mem_max_s, "max") == 0) {
    cg_write(mem_file, "max");
  } else {
    long bytes = parse_mem(mem_max_s);
    char mem_val[32];
    snprintf(mem_val, sizeof(mem_val), "%ld", bytes);
    cg_write(mem_file, mem_val);
  }
}

int main(void) {
  // Ensure slice dirs exist (init creates them, but be safe)
  mkdir("/sys/fs/cgroup/system", 0755);
  mkdir("/sys/fs/cgroup/containers", 0755);

  // Step 1: enable controllers at the ROOT cgroup
  // This makes cpu/pids files appear in system/ and containers/
  enable_controllers(CGROUP_ROOT);

  // Step 2: delegate the same controllers into containers/
  // so per-container cgroups (containers/app/) can inherit them
  enable_controllers(CGROUP_ROOT "/containers");

  // Step 3: parse slices.conf and apply limits to each slice
  FILE *f = fopen(SLICES_CONF, "r");
  if (!f) {
    write(2, "cgmgr: cannot open " SLICES_CONF "\n",
          sizeof("cgmgr: cannot open " SLICES_CONF "\n") - 1);
    return 1;
  }

  char line[256];
  while (fgets(line, sizeof(line), f)) {

    // skip comments and blank lines
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
      continue;

    char name[64] = {0};
    int cpu_weight = 100;     // kernel default
    char mem_max[32] = "max"; // unlimited by default

    // Format: <name>  cpu_weight=<N>  mem_max=<NM|NG|max>
    char *tok = strtok(line, " \t\n\r");
    if (!tok)
      continue;
    strncpy(name, tok, sizeof(name) - 1);

    while ((tok = strtok(NULL, " \t\n\r")) != NULL) {
      if (strncmp(tok, "cpu_weight=", 11) == 0)
        cpu_weight = atoi(tok + 11);
      else if (strncmp(tok, "mem_max=", 8) == 0)
        strncpy(mem_max, tok + 8, sizeof(mem_max) - 1);
    }

    apply_slice(name, cpu_weight, mem_max);
  }

  fclose(f);

  // Done — exit 0 so supervisor (restart=on-failure) doesn't restart us
  return 0;
}
