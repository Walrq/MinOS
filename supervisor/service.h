// supervisor/service.h
#pragma once
#include <sys/types.h>

// Restart policies
#define RESTART_ALWAYS      0   // always restart, no matter what
#define RESTART_ON_FAILURE  1   // restart only if exit code != 0
#define RESTART_NEVER       2   // don't restart, let it stay dead

typedef struct {
    char    name[64];       // human readable: "netd", "cgmgr"
    char    bin[256];       // full path: "/services/netd"
    char   *argv[32];       // argument list, NULL terminated
    int     restart;        // RESTART_* policy
    pid_t   pid;            // current running PID, -1 if stopped
    int     exit_code;      // last known exit code
    int     restart_count;  // how many times has this crashed
} Service;
