// projects/sysmon/sysmon.c — MinOS Live Container Dashboard
//
// Shows a live terminal UI with:
//   - Container hostname, cgroup path, uptime
//   - Memory usage bar (green→yellow→red)
//   - PID count bar
//   - Isolated process list (PID namespace only)
//
// Every 20 seconds auto-forks /bin/memhog to demo OOM kill.
//
// Usage:  minc run -t -m 10 /bin/sysmon

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

// ── ANSI codes ────────────────────────────────────────────────────────────────
#define R      "\033[0m"
#define BOLD   "\033[1m"
#define DIM    "\033[2m"
#define RED    "\033[31m"
#define GREEN  "\033[32m"
#define YEL    "\033[33m"
#define MAG    "\033[35m"
#define CYAN   "\033[36m"
#define BGREEN "\033[92m"
#define BRED   "\033[91m"
#define BYEL   "\033[93m"
#define BMAG   "\033[95m"

// Box-drawing strings (UTF-8, passed as const char* — NOT char literals)
#define TL "┌"
#define TR "┐"
#define BL_C "└"
#define BR_C "┘"
#define ML "├"
#define MR "┤"
#define HL "─"
#define VL "│"
#define FULL_BLK "█"
#define LIGHT_BLK "░"

#define BOX_W 63   // dashes per horizontal line
#define BAR_W 28   // progress bar fill width

// ── Global state ──────────────────────────────────────────────────────────────
static char cg_rel[256]  = "";
static char g_host[64]   = "unknown";
static time_t g_start;
static pid_t  stress_pid  = -1;
static int    stress_cycle = 0;
static int    oom_ticks    = 0;

// ── Cgroup helpers ────────────────────────────────────────────────────────────
static long read_cg(const char *file) {
    char path[512];
    snprintf(path, sizeof(path), "/sys/fs/cgroup%s/%s", cg_rel, file);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    fscanf(f, "%ld", &v);
    fclose(f);
    return v;
}

static void detect_cgroup(void) {
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "0::", 3) == 0) {
            char *p = line + 3;
            p[strcspn(p, "\n")] = 0;
            strncpy(cg_rel, p, sizeof(cg_rel) - 1);
            break;
        }
    }
    fclose(f);
}

// ── Box drawing ───────────────────────────────────────────────────────────────
// l/m/r are UTF-8 strings like "┌", "─", "┐"
static void hline(const char *l, const char *r) {
    printf(BOLD MAG "%s", l);
    for (int i = 0; i < BOX_W; i++) printf(HL);
    printf("%s\n" R, r);
}

// Prefix for a content row: │ (space)
#define ROW_L  MAG VL R " "
// Suffix for a content row: (space) │
#define ROW_R  " " MAG VL R

// ── Progress bar ──────────────────────────────────────────────────────────────
static void bar(long cur, long max) {
    if (max <= 0 || max > 0x7FFFFFFFL) {
        printf(DIM "[────────── unlimited ───────────]" R);
        return;
    }
    int pct    = (int)((long long)cur * 100 / max);
    int filled = (int)((long long)cur * BAR_W / max);
    if (filled > BAR_W) filled = BAR_W;

    const char *col = BGREEN;
    if      (pct >= 90) col = BRED;
    else if (pct >= 70) col = BYEL;
    else if (pct >= 50) col = YEL;

    printf("[%s", col);
    for (int i = 0; i < filled; i++)        printf(FULL_BLK);
    printf(R DIM);
    for (int i = filled; i < BAR_W; i++)    printf(LIGHT_BLK);
    printf(R "]");
}

// ── Process list ──────────────────────────────────────────────────────────────
static void list_procs(void) {
    DIR *d = opendir("/proc");
    if (!d) return;

    int pids[128], np = 0;
    struct dirent *e;
    while ((e = readdir(d)) && np < 128) {
        int ok = 1;
        for (char *p = e->d_name; *p; p++)
            if (*p < '0' || *p > '9') { ok = 0; break; }
        if (ok && e->d_name[0] != '0')
            pids[np++] = atoi(e->d_name);
    }
    closedir(d);

    // Sort ascending (small N, bubble is fine)
    for (int i = 0; i < np-1; i++)
        for (int j = 0; j < np-i-1; j++)
            if (pids[j] > pids[j+1]) { int t=pids[j]; pids[j]=pids[j+1]; pids[j+1]=t; }

    printf(ROW_L BOLD CYAN "%-6s  %-22s  %-10s" R ROW_R "\n",
           "PID", "COMMAND", "STATE");
    printf(ROW_L DIM "%-6s  %-22s  %-10s" R ROW_R "\n",
           "──────", "──────────────────────", "──────────");

    for (int i = 0; i < np; i++) {
        char sp[64];
        snprintf(sp, sizeof(sp), "/proc/%d/status", pids[i]);
        FILE *f = fopen(sp, "r");
        if (!f) continue;

        char comm[32] = "?";
        char state_out[64] = "?";
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Name:", 5) == 0)
                sscanf(line+5, "%31s", comm);
            if (strncmp(line, "State:", 6) == 0) {
                char code[4] = "?";
                sscanf(line+6, "%3s", code);
                switch (code[0]) {
                    case 'R': snprintf(state_out, sizeof(state_out), BGREEN "● running"  R); break;
                    case 'S': snprintf(state_out, sizeof(state_out), CYAN   "◌ sleeping" R); break;
                    case 'Z': snprintf(state_out, sizeof(state_out), BRED   "✗ zombie"   R); break;
                    case 'D': snprintf(state_out, sizeof(state_out), BYEL   "⚡ waiting" R); break;
                    default:  snprintf(state_out, sizeof(state_out), "%s", code);            break;
                }
            }
        }
        fclose(f);
        printf(ROW_L CYAN "%-6d" R "  %-22s  %s\n" ROW_R "\n",
               pids[i], comm, state_out);
    }
}

// ── Stress test ───────────────────────────────────────────────────────────────
static void tick_stress(long elapsed) {
    // Reap if done
    if (stress_pid > 0) {
        int st;
        if (waitpid(stress_pid, &st, WNOHANG) == stress_pid) {
            if (WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL)
                oom_ticks = 6;
            stress_pid = -1;
        }
    }
    if (oom_ticks > 0) oom_ticks--;

    // Launch memhog at second 8 of every 20-second cycle
    int phase = (int)(elapsed % 20);
    if (phase == 8 && stress_pid < 0) {
        stress_cycle++;
        stress_pid = fork();
        if (stress_pid == 0) {
            char *args[] = {"/bin/memhog", "30", NULL};
            execv("/bin/memhog", args);
            _exit(127);
        }
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(void) {
    detect_cgroup();
    gethostname(g_host, sizeof(g_host));
    g_start = time(NULL);

    printf("\033[?25l");   // hide cursor
    signal(SIGCHLD, SIG_DFL);

    while (1) {
        long elapsed = (long)(time(NULL) - g_start);
        int hrs  = elapsed / 3600;
        int mins = (elapsed % 3600) / 60;
        int secs = elapsed % 60;

        long mem_cur = read_cg("memory.current");
        long mem_max = read_cg("memory.max");
        long pid_cur = read_cg("pids.current");
        long pid_max = read_cg("pids.max");

        printf("\033[H");   // cursor home — avoids full clear flicker

        // ── Header ───────────────────────────────────────────────────────────
        hline(TL, TR);

        printf(ROW_L BOLD BMAG
               "MinOS Container Monitor" R
               "   host: " CYAN BOLD "%-8s" R
               "  uptime: " YEL "%02d:%02d:%02d" R
               "     " ROW_R "\n",
               g_host, hrs, mins, secs);

        if (cg_rel[0])
            printf(ROW_L DIM "cgroup: " R CYAN "%-54s" R ROW_R "\n", cg_rel);

        hline(ML, MR);

        // ── Memory bar ───────────────────────────────────────────────────────
        if (mem_cur >= 0) {
            int pct = (mem_max > 0) ? (int)((long long)mem_cur*100/mem_max) : 0;
            printf(ROW_L BOLD "MEM " R);
            bar(mem_cur, mem_max);
            printf(CYAN " %5.1fMB" R "/" MAG "%-5.1fMB" R " %3d%%" ROW_R "\n",
                   mem_cur/1048576.0, mem_max/1048576.0, pct);
        } else {
            printf(ROW_L BOLD "MEM " R DIM
                   "  memory cgroup not active                              "
                   R ROW_R "\n");
        }

        // ── PID bar ──────────────────────────────────────────────────────────
        if (pid_cur >= 0) {
            printf(ROW_L BOLD "PID " R);
            bar(pid_cur, pid_max);
            printf(CYAN " %3ld" R "/" MAG "%-3ld" R
                   " pids                  " ROW_R "\n", pid_cur, pid_max);
        } else {
            printf(ROW_L BOLD "PID " R DIM
                   "  pids cgroup not active                                "
                   R ROW_R "\n");
        }

        // ── Stress status ────────────────────────────────────────────────────
        printf(ROW_L);
        int phase = (int)(elapsed % 20);
        if (oom_ticks > 0)
            printf(BRED BOLD "  OOM-KILL #%d! Kernel reclaimed memory!         " R,
                   stress_cycle);
        else if (stress_pid > 0)
            printf(BYEL "  memhog running — watch the MEM bar fill up!    " R);
        else
            printf(CYAN "  Next stress test in %2ds ...                    " R,
                   (phase < 8) ? 8 - phase : 20 + 8 - phase);
        printf(ROW_R "\n");

        hline(ML, MR);

        // ── Process list ─────────────────────────────────────────────────────
        printf(ROW_L BOLD "PROCESSES" R
               DIM " — host procs invisible (PID namespace)        " R
               ROW_R "\n");
        list_procs();

        // Clear any leftover rows from previous render
        for (int i = 0; i < 4; i++)
            printf(ROW_L "                                                               "
                   ROW_R "\n");

        hline(BL_C, BR_C);
        printf(DIM "  Ctrl+C to exit  |  refreshes every 1s\n" R);
        fflush(stdout);

        tick_stress(elapsed);
        sleep(1);
    }
}
