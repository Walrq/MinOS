#include <sys/mount.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

void mount_filesystems(void) {

    // /proc — exposes kernel data as files
    // e.g. /proc/1234/status = info about PID 1234
    mount("proc", "/proc", "proc",
          MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);

    // /sys — exposes device/driver info + cgroup v2 lives here
    mount("sys", "/sys", "sysfs",
          MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);

    // /dev — device files (keyboard, disk, tty, null, etc.)
    // MUST come before anything that opens a device
    mount("dev", "/dev", "devtmpfs",
          MS_NOSUID | MS_NOEXEC, NULL);

    // /tmp — in-memory scratch space
    mount("tmp", "/tmp", "tmpfs",
          MS_NOSUID | MS_NODEV, NULL);

    // /sys/fs/cgroup — cgroup v2 unified hierarchy
    // MUST be mounted after /sys is up
    mount("cgroup2", "/sys/fs/cgroup", "cgroup2",
          MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);

    // Enable controllers at root — write one at a time so a missing
    // controller (e.g. memory if CONFIG_MEMCG=n) doesn't block the rest
    const char *subtree = "/sys/fs/cgroup/cgroup.subtree_control";
    int fd;

    fd = open(subtree, O_WRONLY);
    if (fd >= 0) { write(fd, "+cpu",  4); close(fd); }

    fd = open(subtree, O_WRONLY);
    if (fd >= 0) { write(fd, "+pids", 5); close(fd); }

    // memory is optional — only present if CONFIG_MEMCG=y in kernel
    fd = open(subtree, O_WRONLY);
    if (fd >= 0) { write(fd, "+memory", 7); close(fd); }

    // Create OS-level cgroup slices
    // system/     — for OS daemons (supervisor, cgmgr, netd)
    // containers/ — parent for all per-container cgroups
    mkdir("/sys/fs/cgroup/system",     0755);
    mkdir("/sys/fs/cgroup/containers", 0755);
}