#define _POSIX_C_SOURCE 200809L

#include "signal_control.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    int notify_fd;
    unsigned int reloads;
    unsigned int dumps;
} TestContext;

static int request_shutdown(void *arg)
{
    TestContext *context = arg;
    const char byte = 'S';
    ssize_t result;

    do {
        result = write(context->notify_fd, &byte, 1);
    } while (result == -1 && errno == EINTR);

    return result == 1 ? 0 : -1;
}

static int request_reload(void *arg)
{
    TestContext *context = arg;
    context->reloads++;

    return puts("PASS: SIGHUP callback") == EOF ||
           fflush(stdout) == EOF ? -1 : 0;
}

static int request_dump(void *arg)
{
    TestContext *context = arg;
    context->dumps++;

    return puts("PASS: SIGUSR1 callback") == EOF ||
           fflush(stdout) == EOF ? -1 : 0;
}

int main(void)
{
    sigset_t signals;

    if (signal_control_block(&signals) == -1) {
        perror("signal_control_block");
        return EXIT_FAILURE;
    }

    int notification[2];

    if (pipe(notification) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    TestContext context = {
        .notify_fd = notification[1]
    };

    SignalControl control;

    if (signal_control_start(
            &control, &signals,
            request_shutdown, request_reload, request_dump,
            &context) == -1) {
        perror("signal_control_start");
        close(notification[0]);
        close(notification[1]);
        return EXIT_FAILURE;
    }

    printf("PID: %ld\n", (long)getpid());
    puts("Send SIGHUP, then SIGUSR1, then SIGINT.");
    fflush(stdout);

    char byte;
    ssize_t received;

    do {
        received = read(notification[0], &byte, 1);
    } while (received == -1 && errno == EINTR);

    int status = EXIT_SUCCESS;

    if (received != 1 || byte != 'S') {
        fprintf(stderr, "FAIL: shutdown notification\n");
        status = EXIT_FAILURE;
    }

    if (signal_control_stop(&control) == -1) {
        perror("signal_control_stop");
        status = EXIT_FAILURE;
    } else if (context.reloads != 1 || context.dumps != 1) {
        fprintf(stderr, "FAIL: expected one reload and one dump\n");
        status = EXIT_FAILURE;
    } else if (status == EXIT_SUCCESS) {
        puts("PASS: SIGINT notification and clean signal-thread shutdown");
    }

    close(notification[0]);
    close(notification[1]);

    return status;
}