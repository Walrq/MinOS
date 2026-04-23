#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>

// Forward declarations (you'll implement these)
void mount_filesystems(void);
void setup_signals(void);
void reap_zombies(void);


// init/main.c — replace spawn_shell() with this

void start_supervisor(void) {
    pid_t pid = fork();

    if (pid == 0) {
        // reset signal mask for child
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);

        execl("/supervisor", "supervisor", "/services.d", NULL);

        write(2, "failed to start supervisor\n", 27);
        _exit(1);
    }
    // init falls back to its reaper loop
    // if supervisor ever dies, init will see it via waitpid
}
int main(void) {

    // Ensure we are PID 1
    if (getpid() != 1) {
        write(2, "fatal: not PID 1\n", 17);
        _exit(1);
    }

    mount_filesystems();   // Step 2
    setup_signals();       // Step 3

    start_supervisor();         // NEW (Step 3.5)

    // Main loop — reap zombies forever
    while (1) {
        reap_zombies();
        sleep(1);
    }

    return 0;
}