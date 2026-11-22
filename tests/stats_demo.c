#include "checked.h"
#include "shm_stats.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    StatsHandle stats;

    if (shm_stats_create(&stats, 2) == -1) {
        perror("shm_stats_create");
        return EXIT_FAILURE;
    }

    int status = EXIT_SUCCESS;
    int input;

    vsw_posix(puts("Type a then Enter to increment Port 1. Type q then Enter to exit."), "puts");

    while ((input = getchar()) != EOF && input != 'q') {
        if (input == 'a' &&
            shm_stats_add(&stats, 0, STATS_FRAMES_IN) == -1) {
            perror("shm_stats_add");
            status = EXIT_FAILURE;
            break;
        }
    }

    if (ferror(stdin)) {
        vsw_posix(fprintf(stderr, "Could not read input\n"), "fprintf");
        status = EXIT_FAILURE;
    }

    if (shm_stats_close(&stats) == -1) {
        perror("shm_stats_close");
        status = EXIT_FAILURE;
    }

    return status;
}