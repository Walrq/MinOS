// tools/mintab.c — MinOS session multiplexer
// mintab daemon          : start background daemon (auto-started by supervisor)
// mintab new <cmd...>    : launch a new container session
// mintab list            : show all sessions
// mintab attach <id>     : attach stdin/stdout (Ctrl+A D to detach)
// mintab kill <id>       : terminate a session

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <pty.h>
#if defined(__GLIBC__)
#include <utmp.h>  // required by forkpty on some glibc versions
#endif
#include <termios.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>

#define SOCK_PATH    "/run/mintab.sock"
#define MAX_SESSIONS 16
#define MAX_PENDING  8

// ── Data types ────────────────────────────────────────────────────────────────

typedef struct {
    int   id;
    int   pty_master;
    pid_t pid;
    char  cmd[256];
    int   alive;
    int   client_fd;   // attached client socket, or -1
} Session;

typedef struct {
    int  fd;
    char buf[256];
    int  len;
} Pending;

static Session S[MAX_SESSIONS];
static int     NS = 0;
static Pending P[MAX_PENDING];
static int     NP = 0;

// ── Utility ───────────────────────────────────────────────────────────────────

static void set_nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static int find_session(int id) {
    for (int i = 0; i < NS; i++)
        if (S[i].id == id && S[i].alive) return i;
    return -1;
}

static void drop_pending(int i) {
    close(P[i].fd);
    P[i] = P[--NP];
}

static void end_session(int i) {
    S[i].alive = 0;
    close(S[i].pty_master);
    if (S[i].client_fd >= 0) {
        write(S[i].client_fd, "\r\n[session ended]\r\n", 19);
        close(S[i].client_fd);
        S[i].client_fd = -1;
    }
}

// ── Daemon: process one complete command ──────────────────────────────────────

static void handle_cmd(char *cmd, int cfd) {
    cmd[strcspn(cmd, "\r\n")] = 0;

    // ── NEW <cmd...> ──────────────────────────────────────────────────────────
    if (strncmp(cmd, "NEW ", 4) == 0) {
        if (NS >= MAX_SESSIONS) {
            write(cfd, "ERR too many sessions\n", 22); close(cfd); return;
        }
        char *arg = cmd + 4;
        char copy[256]; strncpy(copy, arg, sizeof(copy) - 1);
        char *argv[64]; int argc = 0;
        for (char *t = strtok(copy, " "); t && argc < 63; t = strtok(NULL, " "))
            argv[argc++] = t;
        argv[argc] = NULL;

        int master;
        pid_t pid = forkpty(&master, NULL, NULL, NULL);
        if (pid < 0) { write(cfd, "ERR forkpty\n", 12); close(cfd); return; }
        if (pid == 0) {
            // Close ALL file descriptors inherited from the daemon (fd 3+).
            // forkpty() already redirected stdin/stdout/stderr (0,1,2) to the
            // PTY slave. Without this, execvp'd minc would hold the daemon's
            // listen socket and all client sockets open — preventing EOF on
            // those sockets and causing `mintab new` to hang forever.
            for (int i = 3; i < 1024; i++) close(i);
            execvp(argv[0], argv);
            _exit(127);
        }

        set_nonblock(master);
        Session *s = &S[NS];
        s->id = NS; s->pty_master = master; s->pid = pid;
        s->alive = 1; s->client_fd = -1;
        strncpy(s->cmd, arg, sizeof(s->cmd) - 1);
        NS++;

        char resp[32]; snprintf(resp, sizeof(resp), "OK %d\n", s->id);
        write(cfd, resp, strlen(resp));
        close(cfd);
        return;
    }

    // ── LIST ──────────────────────────────────────────────────────────────────
    if (strcmp(cmd, "LIST") == 0) {
        char out[2048] = ""; int any = 0;
        for (int i = 0; i < NS; i++) {
            if (!S[i].alive) continue;
            char line[256];
            snprintf(line, sizeof(line), "[%d]  pid=%-6d  %-30s %s\n",
                     S[i].id, (int)S[i].pid, S[i].cmd,
                     S[i].client_fd >= 0 ? "[attached]" : "");
            strncat(out, line, sizeof(out) - strlen(out) - 1);
            any = 1;
        }
        if (!any) strcpy(out, "(no sessions)\n");
        write(cfd, out, strlen(out));
        close(cfd);
        return;
    }

    // ── ATTACH <id> ───────────────────────────────────────────────────────────
    if (strncmp(cmd, "ATTACH ", 7) == 0) {
        int id  = atoi(cmd + 7);
        int idx = find_session(id);
        if (idx < 0) { write(cfd, "ERR no such session\n", 20); close(cfd); return; }
        if (S[idx].client_fd >= 0) close(S[idx].client_fd); // kick old client
        write(cfd, "OK\n", 3);
        S[idx].client_fd = cfd;   // daemon now owns this fd (blocking is fine — select guards it)
        return;
    }

    // ── KILL <id> ─────────────────────────────────────────────────────────────
    if (strncmp(cmd, "KILL ", 5) == 0) {
        int id  = atoi(cmd + 5);
        int idx = find_session(id);
        if (idx < 0) { write(cfd, "ERR no such session\n", 20); close(cfd); return; }
        kill(S[idx].pid, SIGKILL);
        end_session(idx);
        write(cfd, "OK\n", 3);
        close(cfd);
        return;
    }

    write(cfd, "ERR unknown command\n", 20);
    close(cfd);
}

// ── Daemon main event loop ────────────────────────────────────────────────────

static void run_daemon(void) {
    for (int i = 0; i < MAX_SESSIONS; i++) S[i].alive = 0;
    unlink(SOCK_PATH);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    bind(lfd, (struct sockaddr *)&addr, sizeof(addr));
    listen(lfd, 8);
    signal(SIGCHLD, SIG_DFL);

    while (1) {
        // Reap zombies
        pid_t wp; int ws;
        while ((wp = waitpid(-1, &ws, WNOHANG)) > 0)
            for (int i = 0; i < NS; i++)
                if (S[i].pid == wp && S[i].alive) end_session(i);

        // Build fd_set
        fd_set rfds; FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        int mfd = lfd;

        for (int i = 0; i < NP; i++) {
            FD_SET(P[i].fd, &rfds);
            if (P[i].fd > mfd) mfd = P[i].fd;
        }
        for (int i = 0; i < NS; i++) {
            if (!S[i].alive) continue;
            FD_SET(S[i].pty_master, &rfds);
            if (S[i].pty_master > mfd) mfd = S[i].pty_master;
            if (S[i].client_fd >= 0) {
                FD_SET(S[i].client_fd, &rfds);
                if (S[i].client_fd > mfd) mfd = S[i].client_fd;
            }
        }

        struct timeval tv = { 1, 0 };
        if (select(mfd + 1, &rfds, NULL, NULL, &tv) < 0) continue;

        // New connection
        if (FD_ISSET(lfd, &rfds)) {
            int cfd = accept(lfd, NULL, NULL);
            if (cfd >= 0 && NP < MAX_PENDING) {
                set_nonblock(cfd);
                P[NP].fd = cfd; P[NP].len = 0;
                NP++;
            } else if (cfd >= 0) {
                write(cfd, "ERR busy\n", 9); close(cfd);
            }
        }

        // Read commands from pending clients
        char buf[1024];
        for (int i = NP - 1; i >= 0; i--) {
            if (!FD_ISSET(P[i].fd, &rfds)) continue;
            int n = read(P[i].fd, buf, sizeof(buf) - 1);
            if (n <= 0) { drop_pending(i); continue; }
            int space = (int)sizeof(P[i].buf) - P[i].len - 1;
            if (n > space) n = space;
            memcpy(P[i].buf + P[i].len, buf, n);
            P[i].len += n;
            P[i].buf[P[i].len] = 0;
            if (strchr(P[i].buf, '\n') || P[i].len >= (int)sizeof(P[i].buf) - 1) {
                // Save fd and buf locally before removing from array
                int  saved_fd  = P[i].fd;
                char saved_buf[256];
                memcpy(saved_buf, P[i].buf, sizeof(saved_buf));
                P[i] = P[--NP];   // remove slot
                handle_cmd(saved_buf, saved_fd);
            }
        }

        // Forward PTY ↔ client
        for (int i = 0; i < NS; i++) {
            if (!S[i].alive) continue;

            // PTY → client
            if (FD_ISSET(S[i].pty_master, &rfds)) {
                int n = read(S[i].pty_master, buf, sizeof(buf));
                if (n > 0) {
                    if (S[i].client_fd >= 0) write(S[i].client_fd, buf, n);
                } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
                    end_session(i);
                }
            }

            // Client → PTY
            if (S[i].client_fd >= 0 && FD_ISSET(S[i].client_fd, &rfds)) {
                int n = read(S[i].client_fd, buf, sizeof(buf));
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    close(S[i].client_fd);
                    S[i].client_fd = -1;
                } else if (n > 0) {
                    write(S[i].pty_master, buf, n);
                }
            }
        }
    }
}

// ── Client ────────────────────────────────────────────────────────────────────

static int dial(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "mintab: daemon not running — start with: mintab daemon &\n");
        close(fd); return -1;
    }
    return fd;
}

static int simple_cmd(const char *msg) {
    int fd = dial(); if (fd < 0) return 1;
    write(fd, msg, strlen(msg));
    shutdown(fd, SHUT_WR);
    char buf[4096]; int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, n);
    close(fd); return 0;
}

static int do_attach(int id) {
    int fd = dial(); if (fd < 0) return 1;
    char msg[32]; snprintf(msg, sizeof(msg), "ATTACH %d\n", id);
    write(fd, msg, strlen(msg));

    char ack[16] = {0};
    read(fd, ack, sizeof(ack) - 1);
    if (strncmp(ack, "OK", 2) != 0) {
        fprintf(stderr, "mintab: %s", ack); close(fd); return 1;
    }

    fprintf(stderr, "[attached to session %d — Ctrl+A then D to detach]\r\n", id);

    struct termios orig, raw;
    tcgetattr(STDIN_FILENO, &orig);
    raw = orig; cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    int ctrl_a = 0;
    char buf[256];
    while (1) {
        fd_set rfds; FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds); FD_SET(fd, &rfds);
        int mx = fd + 1;
        if (select(mx, &rfds, NULL, NULL, NULL) < 0) break;

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            // detect Ctrl+A then 'd'
            for (int i = 0; i < n; i++) {
                if (ctrl_a && (buf[i] == 'd' || buf[i] == 'D')) {
                    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
                    printf("\r\n[detached from session %d]\r\n", id);
                    close(fd); return 0;
                }
                ctrl_a = (buf[i] == '\x01');
            }
            write(fd, buf, n);
        }
        if (FD_ISSET(fd, &rfds)) {
            int n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            write(STDOUT_FILENO, buf, n);
        }
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    printf("\r\n[session closed]\r\n");
    close(fd); return 0;
}

// ── Entry point ───────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "mintab — MinOS session multiplexer\n"
            "  mintab daemon          start the daemon\n"
            "  mintab new <cmd...>    launch new session\n"
            "  mintab list            list sessions\n"
            "  mintab attach <id>     attach (Ctrl+A D to detach)\n"
            "  mintab kill <id>       kill a session\n");
        return 1;
    }

    if (!strcmp(argv[1], "daemon")) { run_daemon(); return 0; }

    if (!strcmp(argv[1], "new") && argc >= 3) {
        char msg[512] = "NEW ";
        for (int i = 2; i < argc; i++) {
            strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 2);
            if (i < argc - 1) strncat(msg, " ", 2);
        }
        strncat(msg, "\n", 2);
        return simple_cmd(msg);
    }

    if (!strcmp(argv[1], "list"))   return simple_cmd("LIST\n");

    if (!strcmp(argv[1], "attach") && argc == 3)
        return do_attach(atoi(argv[2]));

    if (!strcmp(argv[1], "kill") && argc == 3) {
        char msg[32]; snprintf(msg, sizeof(msg), "KILL %s\n", argv[2]);
        return simple_cmd(msg);
    }

    fprintf(stderr, "mintab: unknown subcommand '%s'\n", argv[1]);
    return 1;
}
