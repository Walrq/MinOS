#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

int main(void) {
    int fd = open("/dev/ttyS0", O_RDWR);
    if (fd < 0) { write(2, "open ttyS0 failed\n", 18); return 1; }

    setsid();
    ioctl(fd, TIOCSCTTY, 0);

    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) close(fd);

    char *argv[] = { "/bin/sh", NULL };
    execv("/bin/sh", argv);
    return 1;
}
