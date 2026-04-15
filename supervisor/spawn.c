// supervisor/spawn.c
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "service.h"
#include<signal.h>


pid_t spawn_service(Service *s) {
    pid_t pid = fork();

    if (pid < 0) {
        // fork failed — system is in trouble
        write(2, "fork failed\n", 12);
        return -1;
    }

if (pid == 0) {
    // Reset signal mask
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);

    // Connect to terminal  ← ADD THIS
    int fd = open("/dev/ttyS0", O_RDWR);
    if (fd >= 0) {
        setsid();
        ioctl(fd, TIOCSCTTY, 1);
        dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        if (fd > 2) close(fd);
    }

    execv(s->bin, s->argv);
    /* execv failed — print which binary and why */
    const char *p = s->bin;
    write(2, "execv failed [", 14);
    write(2, p, strlen(p));
    write(2, "] errno=", 8);
    /* print errno as digit(s) */
    char ebuf[8]; int e = errno, i = 0;
    if (e == 0) { ebuf[i++] = '0'; }
    else { int tmp = e; while (tmp) { ebuf[i++] = '0' + (tmp % 10); tmp /= 10; } }
    /* reverse */
    for (int a = 0, b = i-1; a < b; a++, b--) { char t=ebuf[a]; ebuf[a]=ebuf[b]; ebuf[b]=t; }
    write(2, ebuf, i);
    write(2, "\n", 1);
    _exit(1);
}

    // ── PARENT (supervisor) ──
    s->pid = pid;
    return pid;
}