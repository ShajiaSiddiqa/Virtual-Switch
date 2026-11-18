#define _POSIX_C_SOURCE 200809L

#include "shm_stats.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int print_stats(StatsHandle *handle)
{
    PortStats ports[SHM_STATS_MAX_PORTS];
    size_t num_ports;

    if (shm_stats_snapshot(handle, ports, SHM_STATS_MAX_PORTS,
                           &num_ports) == -1) {
        perror("shm_stats_snapshot");
        return -1;
    }

    if (puts("PORT  IN  OUT  DROPPED  MIRRORED") == EOF) {
        return -1;
    }

    for (size_t i = 0; i < num_ports; i++) {
        if (printf("%zu  %" PRIu64 "  %" PRIu64 "  %" PRIu64 "  %" PRIu64 "\n",
                   i + 1, ports[i].frames_in, ports[i].frames_out,
                   ports[i].frames_dropped, ports[i].frames_mirrored) < 0) {
            return -1;
        }
    }

    return fflush(stdout) == EOF ? -1 : 0;
}

int main(int argc, char *argv[])
{
    if ((argc != 2 && argc != 3) || strcmp(argv[1], "stats") != 0 ||
        (argc == 3 && strcmp(argv[2], "--watch") != 0)) {
        fprintf(stderr, "Usage: %s stats [--watch]\n", argv[0]);
        return EXIT_FAILURE;
    }

    int watch = argc == 3;
    sigset_t stop_signals;

    if (sigemptyset(&stop_signals) == -1 ||
        sigaddset(&stop_signals, SIGINT) == -1 ||
        sigaddset(&stop_signals, SIGTERM) == -1) {
        perror("configure signals");
        return EXIT_FAILURE;
    }

    if (watch) {
        int error = pthread_sigmask(SIG_BLOCK, &stop_signals, NULL);
        if (error != 0) {
            fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
            return EXIT_FAILURE;
        }
    }

    StatsHandle stats;
    if (shm_stats_attach(&stats) == -1) {
        perror("shm_stats_attach");
        return EXIT_FAILURE;
    }

    int status = EXIT_SUCCESS;

    for (;;) {
        if (print_stats(&stats) == -1) {
            fprintf(stderr, "Could not display statistics\n");
            status = EXIT_FAILURE;
            break;
        }

        if (!watch) {
            break;
        }

        const struct timespec interval = {.tv_sec = 1};
        int received = sigtimedwait(&stop_signals, NULL, &interval);

        if (received == SIGINT || received == SIGTERM) {
            break;
        }

        if (received == -1 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        }

        perror("sigtimedwait");
        status = EXIT_FAILURE;
        break;
    }

    if (shm_stats_close(&stats) == -1) {
        perror("shm_stats_close");
        status = EXIT_FAILURE;
    }

    return status;
}