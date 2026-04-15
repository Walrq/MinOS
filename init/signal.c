#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/reboot.h>

/* busybox reboot sends SIGTERM to PID 1 → actual restart */
static void handle_reboot(int sig) {
    (void)sig;
    write(2, "init: rebooting...\n", 19);
    sync();
    reboot(RB_AUTOBOOT);
}

/* busybox halt / Ctrl+Alt+Del → halt system */
static void handle_halt(int sig) {
    (void)sig;
    write(2, "init: halting...\n", 17);
    sync();
    reboot(RB_HALT_SYSTEM);
}

/* busybox poweroff → sends SIGUSR2 → power off */
static void handle_poweroff(int sig) {
    (void)sig;
    write(2, "init: powering off...\n", 22);
    sync();
    reboot(RB_POWER_OFF);
}

void setup_signals(void) {
    sigset_t all;

    // Block EVERY signal first — PID 1 must not die from stray signals
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, NULL);

    // Unblock: SIGCHLD, SIGTERM (reboot), SIGINT (halt), SIGUSR2 (poweroff)
    sigset_t unblocked;
    sigemptyset(&unblocked);
    sigaddset(&unblocked, SIGCHLD);
    sigaddset(&unblocked, SIGTERM);  // busybox reboot sends SIGTERM to PID 1
    sigaddset(&unblocked, SIGINT);   // halt / Ctrl+Alt+Del
    sigaddset(&unblocked, SIGUSR2);  // busybox poweroff
    sigprocmask(SIG_UNBLOCK, &unblocked, NULL);

    // Install handlers
    struct sigaction sa_reboot   = { .sa_handler = handle_reboot,   .sa_flags = SA_RESTART };
    struct sigaction sa_halt     = { .sa_handler = handle_halt,     .sa_flags = SA_RESTART };
    struct sigaction sa_poweroff = { .sa_handler = handle_poweroff, .sa_flags = SA_RESTART };
    sigemptyset(&sa_reboot.sa_mask);
    sigemptyset(&sa_halt.sa_mask);
    sigemptyset(&sa_poweroff.sa_mask);
    sigaction(SIGTERM, &sa_reboot,   NULL);  // reboot
    sigaction(SIGINT,  &sa_halt,     NULL);  // halt
    sigaction(SIGUSR2, &sa_poweroff, NULL);  // poweroff
}