#include <sys/wait.h>
#include <stdlib.h>

void reap_zombies(void) {

    // waitpid(-1) = wait for ANY child
    // WNOHANG   = don't block if no children have died yet
    // Loop until no more dead children to collect
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}