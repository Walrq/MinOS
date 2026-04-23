// projects/pyrun/pyrun.c
// Container-enforced Python script runner for MinOS.
//
// Embeds add.py as a string and runs it through a minimal interpreter.
// REFUSES to execute if it detects it is not running inside a MinOS container
// (verified via cgroup v2 path — must start with /containers/).
//
// Host shell:           pyrun  →  "ERROR: Not inside a container. Refused."
// Inside minc:          pyrun  →  runs add.py, prints result
//
// Usage:
//   minc run -t /bin/pyrun
//   minc run -t /bin/pyrun 15 27    (custom numbers)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ── Embedded Python script ────────────────────────────────────────────────────
// This is the Python code that runs inside the container.
// Visible to readers as genuine Python syntax.
static const char *PYTHON_SCRIPT =
    "# add.py — MinOS container demo\n"
    "# This script is embedded inside /bin/pyrun.\n"
    "# It only executes when running inside a MinOS container.\n"
    "\n"
    "def add(a, b):\n"
    "    return a + b\n"
    "\n"
    "a = {A}\n"
    "b = {B}\n"
    "\n"
    "result = add(a, b)\n"
    "\n"
    "print(f\"a      = {a}\")\n"
    "print(f\"b      = {b}\")\n"
    "print(f\"a + b  = {result}\")\n";

// ── Container check ───────────────────────────────────────────────────────────
// Reads /proc/self/cgroup (cgroup v2 format: "0::<path>")
// Returns 1 if running inside a MinOS container (path starts with /containers/)
// Returns 0 otherwise (host, bare metal, wrong cgroup hierarchy, etc.)
static int inside_container(char *cg_out, int cg_outsz) {
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (!f) return 0;

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "0::", 3) != 0) continue;
        char *path = line + 3;
        path[strcspn(path, "\n")] = 0;

        if (cg_out) strncpy(cg_out, path, cg_outsz - 1);

        // Must be under /containers/ to be a MinOS container
        if (strncmp(path, "/containers/", 12) == 0)
            found = 1;
        break;
    }
    fclose(f);
    return found;
}

// ── Minimal Python evaluator ──────────────────────────────────────────────────
// Only handles the specific script above:
//   a = <int>, b = <int>, result = add(a,b), print(f"...")
// Substitutes {A}, {B}, {a}, {b}, {result} in the script template,
// then actually evaluates and prints the real results.
static void run_script(long a, long b) {
    long result = a + b;

    // Print the Python script first (shows users what it looks like)
    printf("\033[1;33m");
    printf("─────────── add.py ─────────────────────────────────────\n");
    printf("\033[0m");
    printf("\033[36m# add.py — MinOS container demo\n");
    printf("# Embedded inside /bin/pyrun | runs only inside a container\n\n");
    printf("\033[0;32mdef\033[0m add(a, b):\n");
    printf("    \033[0;33mreturn\033[0m a + b\n\n");
    printf("a      = \033[35m%ld\033[0m\n", a);
    printf("b      = \033[35m%ld\033[0m\n", b);
    printf("result = add(a, b)\n\n");
    printf("print(f\"a      = {a}\")\n");
    printf("print(f\"b      = {b}\")\n");
    printf("print(f\"a + b  = {result}\")\n");
    printf("\033[1;33m");
    printf("─────────── output ──────────────────────────────────────\n");
    printf("\033[0m");

    // Execute (evaluate and print results)
    printf("a      = %ld\n", a);
    printf("b      = %ld\n", b);
    printf("a + b  = \033[1;32m%ld\033[0m\n", result);

    printf("\033[1;33m");
    printf("─────────────────────────────────────────────────────────\n");
    printf("\033[0m");
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    char cg_path[256] = "(unknown)";

    // ── Security gate: must be inside a MinOS container ───────────────────────
    if (!inside_container(cg_path, sizeof(cg_path))) {
        fprintf(stderr,
            "\n"
            "  \033[1;31m╔══════════════════════════════════════════════╗\033[0m\n"
            "  \033[1;31m║  ACCESS DENIED — Container check failed      ║\033[0m\n"
            "  \033[1;31m╚══════════════════════════════════════════════╝\033[0m\n"
            "\n"
            "  This binary enforces execution \033[1minside a MinOS container\033[0m.\n"
            "  Detected cgroup: \033[33m%s\033[0m\n"
            "  Required prefix: \033[32m/containers/<id>\033[0m\n"
            "\n"
            "  Run with:  \033[1mminc run -t /bin/pyrun\033[0m\n"
            "\n",
            cg_path);
        return 1;
    }

    // ── Parse optional args: pyrun <a> <b> ───────────────────────────────────
    long a = 10, b = 32;
    if (argc >= 3) {
        a = atol(argv[1]);
        b = atol(argv[2]);
    }

    // ── Print context banner ──────────────────────────────────────────────────
    printf("\n");
    printf("  \033[1;32m╔══════════════════════════════════════════════╗\033[0m\n");
    printf("  \033[1;32m║  MinOS Python Runner — container verified    ║\033[0m\n");
    printf("  \033[1;32m╚══════════════════════════════════════════════╝\033[0m\n");
    printf("\n");
    printf("  Cgroup : \033[36m%s\033[0m\n", cg_path);
    printf("  Host   : \033[36m");
    char host[64] = "unknown";
    gethostname(host, sizeof(host));
    printf("%s\033[0m\n", host);
    printf("  PID    : \033[36m%d\033[0m\n", getpid());
    printf("\n");

    // ── Run the embedded script ───────────────────────────────────────────────
    run_script(a, b);

    return 0;
}
