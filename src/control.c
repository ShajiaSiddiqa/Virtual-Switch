#define _POSIX_C_SOURCE 200809L

#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int wait_ready(ControlServer *server, int fd, short events, int timeout_ms)
{
    struct pollfd watched[2] = {
        {.fd = server->wake_pipe[0], .events = POLLIN},
        {.fd = fd, .events = events}
    };

    int result;
    do {
        result = poll(watched, 2, timeout_ms);
    } while (result == -1 && errno == EINTR);

    if (result == -1) 
        return -1;

    if (result == 0) {
        errno = ETIMEDOUT;
        return -1;
    }

    if (watched[0].revents != 0) 
        return 0;

    if ((watched[1].revents & POLLNVAL) != 0) {
        errno = EBADF;
        return -1;
    }

    return 1;
}

static int read_command(ControlServer *server, int fd, char *command)
{
    size_t used = 0;

    while (used < CONTROL_COMMAND_SIZE - 1) {
        if (wait_ready(server, fd, POLLIN, 2000) != 1) 
            return -1;

        char byte;
        ssize_t result = recv(fd, &byte, 1, MSG_DONTWAIT);

        if (result == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) 
                continue;
            return -1;
        }

        if (result == 0 || byte == '\0') {
            errno = EPROTO;
            return -1;
        }

        if (byte == '\n') {
            command[used] = '\0';
            return 0;
        }

        command[used++] = byte;
    }

    errno = EMSGSIZE;
    return -1;
}

static int send_text(ControlServer *server, int fd, const char *text)
{
    size_t sent = 0;
    size_t length = strlen(text);

    while (sent < length) {
        if (wait_ready(server, fd, POLLOUT, 2000) != 1) {
            return -1;
        }

        ssize_t result = send(fd, text + sent, length - sent, MSG_DONTWAIT | MSG_NOSIGNAL);

        if (result == -1) {
            if (errno == EINTR || errno == EAGAIN ||
                errno == EWOULDBLOCK) {
                continue;
            }
            return -1;
        }

        if (result == 0) {
            errno = EPIPE;
            return -1;
        }

        sent += (size_t)result;
    }

    return 0;
}

static void handle_client(ControlServer *server, int client)
{
    char command[CONTROL_COMMAND_SIZE];
    char response[CONTROL_RESPONSE_SIZE] = {0};

    if (read_command(server, client, command) == -1) 
        return;

    int result = server->handler(command, response, sizeof(response), server->context);

    if (memchr(response, '\0', sizeof(response)) == NULL) {
        if (send_text(server, client,
                      "ERROR\nResponse too large\n") == -1) {
            perror("control send");
        }
        return;
    }

    const char *status = result == 0 ? "OK\n" : "ERROR\n";

    if (send_text(server, client, status) == -1 || send_text(server, client, response) == -1) 
        perror("control send");
}

static void *control_worker(void *arg)
{
    ControlServer *server = arg;

    for (;;) {
        int ready = wait_ready(server, server->listener, POLLIN, -1);

        if (ready == 0) 
            return NULL;

        if (ready == -1) {
            perror("control poll");
            return NULL;
        }

        int client = accept(server->listener, NULL, NULL);
        if (client == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) 
                continue;

            perror("control accept");
            return NULL;
        }

        handle_client(server, client);

        if (close(client) == -1) 
            perror("control close client");
    }
}

static int release_resources(ControlServer *server, int owns_path)
{
    int saved = 0;

    if (owns_path && unlink(CONTROL_SOCKET_PATH) == -1) {
        saved = errno;
    }

    int *descriptors[] = {
        &server->listener,
        &server->wake_pipe[0],
        &server->wake_pipe[1]
    };

    for (size_t i = 0;
         i < sizeof(descriptors) / sizeof(descriptors[0]); i++) {
        int fd = *descriptors[i];

        if (fd != -1 && close(fd) == -1 && saved == 0) {
            saved = errno;
        }

        *descriptors[i] = -1;
    }

    server->started = 0;

    if (saved != 0) {
        errno = saved;
        return -1;
    }

    return 0;
}

int control_start(ControlServer *server, ControlHandler handler, void *context)
{
    if (server == NULL || handler == NULL) {
        errno = EINVAL;
        return -1;
    }

    *server = (ControlServer){
        .listener = -1,
        .wake_pipe = {-1, -1},
        .handler = handler,
        .context = context
    };

    int owns_path = 0;

    if (pipe(server->wake_pipe) == -1) 
        goto fail;

    server->listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server->listener == -1) 
        goto fail;

    if (fcntl(server->listener, F_SETFL, O_NONBLOCK) == -1) 
        goto fail;

    struct sockaddr_un address = {.sun_family = AF_UNIX};

    if (sizeof(CONTROL_SOCKET_PATH) > sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        goto fail;
    }

    memcpy(address.sun_path, CONTROL_SOCKET_PATH, sizeof(CONTROL_SOCKET_PATH));

    if (bind(server->listener, (struct sockaddr *)&address, sizeof(address)) == -1) 
        goto fail;

    owns_path = 1;

    if (chmod(CONTROL_SOCKET_PATH, 0600) == -1 || listen(server->listener, 8) == -1) 
        goto fail;

    int error = pthread_create(&server->thread, NULL, control_worker, server);
    if (error != 0) {
        errno = error;
        goto fail;
    }

    server->started = 1;
    return 0;

fail:
    {
        int saved = errno;
        int cleanup_result = release_resources(server, owns_path);
        (void)cleanup_result;
        errno = saved;
        return -1;
    }
}

int control_stop(ControlServer *server)
{
    if (server == NULL || !server->started) {
        errno = EINVAL;
        return -1;
    }

    ssize_t result;
    do {
        result = write(server->wake_pipe[1], "x", 1);
    } while (result == -1 && errno == EINTR);

    if (result != 1) 
        return -1;

    int error = pthread_join(server->thread, NULL);
    if (error != 0) {
        errno = error;
        return -1;
    }

    return release_resources(server, 1);
}