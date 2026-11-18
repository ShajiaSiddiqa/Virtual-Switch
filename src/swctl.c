#define _POSIX_C_SOURCE 200809L

#include "shm_stats.h"
#include "control.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
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

    if (shm_stats_snapshot(handle, ports, SHM_STATS_MAX_PORTS,  &num_ports) == -1) {
        perror("shm_stats_snapshot");
        return -1;
    }

    if (puts("PORT  IN  OUT  DROPPED  MIRRORED") == EOF) 
        return -1;


    for (size_t i = 0; i < num_ports; i++) {
        if (printf("%zu  %" PRIu64 "  %" PRIu64 "  %" PRIu64 "  %" PRIu64 "\n",  i + 1, ports[i].frames_in, 
            ports[i].frames_out,  ports[i].frames_dropped, ports[i].frames_mirrored) < 0) 
            return -1;
    }

    return fflush(stdout) == EOF ? -1 : 0;
}

static int run_control(const char *command)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int status = EXIT_FAILURE;
    const struct timeval timeout = {.tv_sec = 3};

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 || 
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1) {
        perror("control timeout");
        goto cleanup;
    }

    struct sockaddr_un address = {.sun_family = AF_UNIX};

    if (sizeof(CONTROL_SOCKET_PATH) > sizeof(address.sun_path)) {
        fprintf(stderr, "Control socket path is too long\n");
        goto cleanup;
    }

    memcpy(address.sun_path, CONTROL_SOCKET_PATH, sizeof(CONTROL_SOCKET_PATH));

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("connect control");
        goto cleanup;
    }

    size_t length = strlen(command);
    size_t sent = 0;

    while (sent < length) {
        ssize_t result = send(fd, command + sent, length - sent, MSG_NOSIGNAL);

        if (result == -1 && errno == EINTR) 
            continue;

        if (result <= 0) {
            if (result == 0) 
                errno = EPIPE;

            perror("send command");
            goto cleanup;
        }

        sent += (size_t)result;
    }

    char response[CONTROL_RESPONSE_SIZE + 8];
    size_t used = 0;

    for (;;) {
        if (used == sizeof(response)) {
            fprintf(stderr, "Control response is too large\n");
            goto cleanup;
        }

        ssize_t result = recv(fd, response + used, sizeof(response) - used, 0);

        if (result == -1 && errno == EINTR) 
            continue;

        if (result == -1) {
            perror("receive response");
            goto cleanup;
        }

        if (result == 0) 
            break;

        used += (size_t)result;
    }

    size_t prefix;
    FILE *output;

    if (used >= 3 && memcmp(response, "OK\n", 3) == 0) {
        prefix = 3;
        output = stdout;
        status = EXIT_SUCCESS;
    } else if (used >= 6 && memcmp(response, "ERROR\n", 6) == 0) {
        prefix = 6;
        output = stderr;
    } else {
        fprintf(stderr, "Invalid control response\n");
        goto cleanup;
    }

    size_t body_length = used - prefix;

    if (fwrite(response + prefix, 1, body_length, output) != body_length ||
        fflush(output) == EOF) {
        fprintf(stderr, "Could not display control response\n");
        status = EXIT_FAILURE;
    }

cleanup:
    if (close(fd) == -1) {
        perror("close control");
        status = EXIT_FAILURE;
    }

    return status;
}
int main(int argc, char *argv[])
{
    if (argc == 3 && strcmp(argv[1], "show") == 0 && strcmp(argv[2], "mac") == 0) 
        return run_control("show mac\n");

    if (argc == 3 && strcmp(argv[1], "show") == 0 && strcmp(argv[2], "ports") == 0) 
        return run_control("show ports\n");

    if (argc == 3 && strcmp(argv[1], "clear") == 0 && strcmp(argv[2], "mac") == 0) 
        return run_control("clear mac\n");

    if ((argc != 2 && argc != 3) || strcmp(argv[1], "stats") != 0 || (argc == 3 && strcmp(argv[2], "--watch") != 0)) {
        fprintf(stderr,
                "Usage:\n"
                "  %s stats [--watch]\n"
                "  %s show mac\n"
                "  %s show ports\n"
                "  %s clear mac\n",
                argv[0], argv[0], argv[0], argv[0]);
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