// service definition: name, binary path, restart policy, cgroup assignment
#include <stdio.h>

struct service {
    char *name;
    char *path;
};
