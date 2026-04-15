// supervisor/reaper.c
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>
#include "service.h"
#include "spawn.h"

// Called by the main loop — checks if any service died
void check_services(Service *services, int count) {
    int status;
    pid_t dead;

    // Non-blocking — collect ALL dead children in one pass
    while ((dead = waitpid(-1, &status, WNOHANG)) > 0) {

        // Find which service this PID belongs to
        for (int i = 0; i < count; i++) {
            if (services[i].pid != dead) continue;

            Service *s = &services[i];
            s->pid = -1;
            s->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            s->restart_count++;

            // Apply restart policy
            int should_restart = 0;

            if (s->restart == RESTART_ALWAYS)
                should_restart = 1;

            else if (s->restart == RESTART_ON_FAILURE && s->exit_code != 0)
                should_restart = 1;

            // RESTART_NEVER falls through — should_restart stays 0

            if (should_restart) {
                // Backoff: 2^n seconds (1s,2s,4s,8s,16s, capped 30s)
                unsigned int d = 1u << (s->restart_count < 5 ? s->restart_count : 5);
                if (d > 30) d = 30;
                sleep(d);
                spawn_service(s);
            }

            break;
        }
    }
}