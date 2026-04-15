// supervisor/main.c
#include <unistd.h>
#include <stdio.h>
#include "service.h"

extern int   load_services(const char*, Service*, int);
extern pid_t spawn_service(Service*);
extern void  check_services(Service*, int);

#define MAX_SERVICES 32

int main(int argc, char *argv[]) {
    const char *services_dir = argc > 1 ? argv[1] : "/services.d";

    Service services[MAX_SERVICES];
    int count = load_services(services_dir, services, MAX_SERVICES);

    if (count == 0) {
        write(2, "no services found\n", 18);
        _exit(1);
    }

    // Start all services
    for (int i = 0; i < count; i++) {
        spawn_service(&services[i]);
    }

    // Main supervision loop
    while (1) {
        check_services(services, count);
        usleep(50000);   // check every 50ms — low overhead
    }

    return 0;
}