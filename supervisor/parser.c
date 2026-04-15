// supervisor/parser.c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include "service.h"

int load_services(const char *dir, Service *services, int max) {
    DIR *d = opendir(dir);
    if (!d) return 0;

    struct dirent *entry;
    int count = 0;

    while ((entry = readdir(d)) && count < max) {
        // skip . and ..
        if (entry->d_name[0] == '.') continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        Service *s = &services[count];
        memset(s, 0, sizeof(Service));  // zero everything, including argv
        s->pid = -1;
        s->restart = RESTART_ALWAYS;
        s->restart_count = 0;

        char line[256];
        while (fgets(line, sizeof(line), f)) {
            // strip newline and carriage return
            line[strcspn(line, "\r\n")] = 0;

            if (strncmp(line, "name=", 5) == 0) {
                strncpy(s->name, line + 5, sizeof(s->name) - 1);

            } else if (strncmp(line, "bin=", 4) == 0) {
                strncpy(s->bin, line + 4, sizeof(s->bin) - 1);

            } else if (strncmp(line, "args=", 5) == 0) {
                char argbuf[256];
                strncpy(argbuf, line + 5, sizeof(argbuf) - 1);
                char *token = strtok(argbuf, " ");
                int i = 1;
                while (token && i < 31) {
                    s->argv[i++] = strdup(token);
                    token = strtok(NULL, " ");
                }
                s->argv[i] = NULL;

            } else if (strncmp(line, "restart=", 8) == 0) {
                if (strcmp(line + 8, "always") == 0)
                    s->restart = RESTART_ALWAYS;
                else if (strcmp(line + 8, "on-failure") == 0)
                    s->restart = RESTART_ON_FAILURE;
                else if (strcmp(line + 8, "never") == 0)
                    s->restart = RESTART_NEVER;
            }
        }

        fclose(f);

        // argv[0] must always be the binary path
        s->argv[0] = strdup(s->bin);

        // safety: ensure argv is NULL terminated
        // (already guaranteed by memset, but be explicit)
        s->argv[31] = NULL;

        count++;
    }

    closedir(d);
    return count;
}