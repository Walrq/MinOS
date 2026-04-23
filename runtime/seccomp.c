// runtime/seccomp.c — BPF seccomp whitelist filter for MinOS containers
//
// This installs a tight syscall whitelist via raw BPF (no libseccomp needed).
// Called from container_main() after clone(), before execv().
//
// All syscalls NOT in the list return EPERM to the container.
// The architecture is validated first — wrong arch → KILL_PROCESS.
//
// To discover which syscalls a binary actually uses:
//   strace -f -e trace=all ./your-binary 2>&1 | grep -oP '^[a-z_0-9]+(?=\()' | sort -u

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

// ── BPF instruction helpers ───────────────────────────────────────────────────

#define ALLOW_ALL  BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
#define DENY_CALL  BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA))

// Validate CPU architecture — wrong arch = instant kill (prevents 32-bit bypass)
#define ARCH_CHECK \
    BPF_STMT(BPF_LD  | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)), \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)

// Load syscall number into accumulator
#define LOAD_NR \
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr))

// Allow if syscall number matches, otherwise fall through to next check
#define ALLOW(nr) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), \
    ALLOW_ALL

// ── Whitelist ─────────────────────────────────────────────────────────────────
// Minimal set needed to run /bin/sh + busybox tools in a container.
// Derived from: strace -f /bin/sh -c "ls /; echo hi; exit" 2>&1

static struct sock_filter seccomp_filter[] = {
    ARCH_CHECK,
    LOAD_NR,

    // ── I/O ───────────────────────────────────────────────────────────────
    ALLOW(__NR_read),
    ALLOW(__NR_write),
    ALLOW(__NR_readv),
    ALLOW(__NR_writev),
    ALLOW(__NR_pread64),
    ALLOW(__NR_pwrite64),

    // ── File operations ───────────────────────────────────────────────────
    ALLOW(__NR_open),
    ALLOW(__NR_openat),
    ALLOW(__NR_close),
    ALLOW(__NR_stat),
    ALLOW(__NR_fstat),
    ALLOW(__NR_lstat),
    ALLOW(__NR_newfstatat),
    ALLOW(__NR_lseek),
    ALLOW(__NR_access),
    ALLOW(__NR_faccessat),
    ALLOW(__NR_getcwd),
    ALLOW(__NR_chdir),
    ALLOW(__NR_fchdir),
    ALLOW(__NR_getdents),
    ALLOW(__NR_getdents64),
    ALLOW(__NR_readlink),
    ALLOW(__NR_readlinkat),
    ALLOW(__NR_mkdir),
    ALLOW(__NR_mkdirat),
    ALLOW(__NR_rmdir),
    ALLOW(__NR_unlink),
    ALLOW(__NR_unlinkat),
    ALLOW(__NR_rename),
    ALLOW(__NR_renameat),
    ALLOW(__NR_renameat2),
    ALLOW(__NR_link),
    ALLOW(__NR_symlink),
    ALLOW(__NR_symlinkat),
    ALLOW(__NR_chmod),
    ALLOW(__NR_fchmod),
    ALLOW(__NR_chown),
    ALLOW(__NR_fchown),
    ALLOW(__NR_lchown),
    ALLOW(__NR_truncate),
    ALLOW(__NR_ftruncate),
    ALLOW(__NR_fsync),
    ALLOW(__NR_fdatasync),
    ALLOW(__NR_sync),
    ALLOW(__NR_umask),
    ALLOW(__NR_statfs),
    ALLOW(__NR_fstatfs),
    ALLOW(__NR_dup),
    ALLOW(__NR_dup2),
    ALLOW(__NR_dup3),
    ALLOW(__NR_fcntl),
    ALLOW(__NR_ioctl),
    ALLOW(__NR_poll),
    ALLOW(__NR_select),
    ALLOW(__NR_pipe),
    ALLOW(__NR_pipe2),

    // ── Process lifecycle ─────────────────────────────────────────────────
    ALLOW(__NR_fork),
    ALLOW(__NR_vfork),
    ALLOW(__NR_clone),
    ALLOW(__NR_execve),
    ALLOW(__NR_execveat),
    ALLOW(__NR_exit),
    ALLOW(__NR_exit_group),
    ALLOW(__NR_wait4),
    ALLOW(__NR_waitid),
    ALLOW(__NR_kill),
    ALLOW(__NR_tkill),
    ALLOW(__NR_tgkill),
    ALLOW(__NR_getpid),
    ALLOW(__NR_getppid),
    ALLOW(__NR_gettid),
    ALLOW(__NR_getuid),
    ALLOW(__NR_getgid),
    ALLOW(__NR_geteuid),
    ALLOW(__NR_getegid),
    ALLOW(__NR_getgroups),
    ALLOW(__NR_getpgrp),
    ALLOW(__NR_getpgid),
    ALLOW(__NR_setpgid),
    ALLOW(__NR_setsid),
    ALLOW(__NR_getsid),

    // ── Memory ────────────────────────────────────────────────────────────
    ALLOW(__NR_mmap),
    ALLOW(__NR_mprotect),
    ALLOW(__NR_munmap),
    ALLOW(__NR_mremap),
    ALLOW(__NR_brk),
    ALLOW(__NR_madvise),

    // ── Signals ───────────────────────────────────────────────────────────
    ALLOW(__NR_rt_sigaction),
    ALLOW(__NR_rt_sigprocmask),
    ALLOW(__NR_rt_sigreturn),
    ALLOW(__NR_rt_sigsuspend),
    ALLOW(__NR_rt_sigpending),
    ALLOW(__NR_sigaltstack),

    // ── Time ──────────────────────────────────────────────────────────────
    ALLOW(__NR_nanosleep),
    ALLOW(__NR_clock_nanosleep),
    ALLOW(__NR_clock_gettime),
    ALLOW(__NR_gettimeofday),
    ALLOW(__NR_time),

    // ── Networking (basic) ────────────────────────────────────────────────
    ALLOW(__NR_socket),
    ALLOW(__NR_connect),
    ALLOW(__NR_bind),
    ALLOW(__NR_listen),
    ALLOW(__NR_accept),
    ALLOW(__NR_accept4),
    ALLOW(__NR_sendto),
    ALLOW(__NR_recvfrom),
    ALLOW(__NR_sendmsg),
    ALLOW(__NR_recvmsg),
    ALLOW(__NR_getsockname),
    ALLOW(__NR_getpeername),
    ALLOW(__NR_setsockopt),
    ALLOW(__NR_getsockopt),
    ALLOW(__NR_shutdown),

    // ── Event / async ─────────────────────────────────────────────────────
    ALLOW(__NR_epoll_create),
    ALLOW(__NR_epoll_create1),
    ALLOW(__NR_epoll_ctl),
    ALLOW(__NR_epoll_wait),
    ALLOW(__NR_epoll_pwait),
    ALLOW(__NR_eventfd),
    ALLOW(__NR_eventfd2),
    ALLOW(__NR_futex),

    // ── Misc ──────────────────────────────────────────────────────────────
    ALLOW(__NR_prctl),
    ALLOW(__NR_arch_prctl),
    ALLOW(__NR_getrlimit),
    ALLOW(__NR_setrlimit),
    ALLOW(__NR_prlimit64),
    ALLOW(__NR_sched_getaffinity),
    ALLOW(__NR_sched_setaffinity),
    ALLOW(__NR_sched_getparam),
    ALLOW(__NR_sched_getscheduler),
    ALLOW(__NR_sched_yield),
    ALLOW(__NR_getrandom),
    ALLOW(__NR_set_tid_address),
    ALLOW(__NR_set_robust_list),
    ALLOW(__NR_get_robust_list),
    ALLOW(__NR_uname),
    ALLOW(__NR_umount2),
    ALLOW(__NR_mount),
    ALLOW(__NR_capget),
    ALLOW(__NR_capset),

    // ── Python 3.10+ / TCC specific ───────────────────────────────────────
    // statx    — Python uses statx() for os.stat() on kernel >= 4.11
    // clone3   — newer glibc threading uses clone3() instead of clone()
    // timerfd  — asyncio event loop
    // utimensat / fchownat / fchmodat — os module file operations
    // memfd_create — Python internal memory operations
    // sendfile — os.sendfile()
    // copy_file_range — shutil optimizations
    ALLOW(__NR_statx),
    ALLOW(__NR_clone3),
    ALLOW(__NR_timerfd_create),
    ALLOW(__NR_timerfd_settime),
    ALLOW(__NR_timerfd_gettime),
    ALLOW(__NR_utimensat),
    ALLOW(__NR_fchownat),
    ALLOW(__NR_fchmodat),
    ALLOW(__NR_memfd_create),
    ALLOW(__NR_sendfile),
    ALLOW(__NR_copy_file_range),
    ALLOW(__NR_inotify_init1),
    ALLOW(__NR_inotify_add_watch),
    ALLOW(__NR_inotify_rm_watch),
    ALLOW(__NR_signalfd4),
    ALLOW(__NR_setuid),
    ALLOW(__NR_setgid),
    ALLOW(__NR_setgroups),
    ALLOW(__NR_mlock),
    ALLOW(__NR_munlock),
    ALLOW(__NR_mlockall),
    ALLOW(__NR_munlockall),
    ALLOW(__NR_getresuid),
    ALLOW(__NR_getresgid),

    // ── Default: deny everything else ─────────────────────────────────────
    DENY_CALL,
};

// ── Public API ────────────────────────────────────────────────────────────────

void load_seccomp(void) {
    // PR_SET_NO_NEW_PRIVS: prevent setuid/setcap escalation inside container.
    // Required before installing a strict seccomp filter when not running as root.
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        write(2, "[seccomp] WARNING: PR_SET_NO_NEW_PRIVS failed\n", 46);
        return;
    }

    struct sock_fprog prog = {
        .len    = (unsigned short)(sizeof(seccomp_filter) / sizeof(seccomp_filter[0])),
        .filter = seccomp_filter,
    };

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0) {
        // Not fatal — kernel may lack CONFIG_SECCOMP_FILTER.
        // Container still runs with namespace + cgroup isolation.
        write(2, "[seccomp] WARNING: filter not installed (CONFIG_SECCOMP_FILTER needed)\n", 72);
        return;
    }

    write(2, "[seccomp] whitelist filter installed\n", 37);
}
