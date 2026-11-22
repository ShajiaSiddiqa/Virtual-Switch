#ifndef CONTROL_H
#define CONTROL_H

#include <stddef.h>
#include <stdlib.h>
#include <sys/un.h>
#include <pthread.h>

#define CONTROL_SOCKET_PATH   "/tmp/vswitch.sock"
#define CONTROL_COMMAND_SIZE  128
#define CONTROL_RESPONSE_SIZE 65536


/* Set before starting threads. swctl uses the same environment override. */
static inline const char *control_socket_path(void)
{
    const char *path = getenv("VSWITCH_SOCKET");
    return path != NULL ? path : CONTROL_SOCKET_PATH;
}

typedef int (*ControlHandler)(const char *command, char *response, size_t capacity, void *context);

typedef void (*ControlErrorLogger)(void *context, const char *operation, int error);

typedef struct {
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int listener;
    int wake_pipe[2];
    pthread_t thread;
    int started;
    ControlHandler handler;
    void *context;
    ControlErrorLogger error_logger;
    void *error_context;
} ControlServer;

int control_start(ControlServer *server, ControlHandler handler, void *context);

int control_start_logged(ControlServer *server, ControlHandler handler, void *context,
                         ControlErrorLogger logger, void *log_context);
int control_stop(ControlServer *server);

#endif