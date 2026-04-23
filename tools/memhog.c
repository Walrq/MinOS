#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    long megabytes = 50;
    if (argc > 1) megabytes = atoi(argv[1]);

    printf("[memhog] Preparing to allocate %ld MB in 1MB chunks...\n", megabytes);
    
    char **chunks = malloc(megabytes * sizeof(char*));
    if (!chunks) {
        printf("[memhog] Failed to allocate array\n");
        return 1;
    }

    for (int i = 0; i < megabytes; i++) {
        chunks[i] = malloc(1024 * 1024);
        if (!chunks[i]) {
            printf("[memhog] malloc failed at %d MB\n", i);
            break;
        }
        // Touch the memory so the OS allocates physical pages
        memset(chunks[i], 0x42, 1024 * 1024);
        printf("[memhog] Allocated %d MB\n", i + 1);
        usleep(100000); // Wait 100ms
    }

    printf("[memhog] Done allocating. Sleeping to hold memory.\n");
    sleep(60);
    return 0;
}
