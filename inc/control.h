#ifndef CONTROL_H
#define CONTROL_H

#include <stddef.h>
#include <pthread.h>

#define CONTROL_SOCKET_PATH   "/tmp/vswitch.sock"
#define CONTROL_COMMAND_SIZE  128
#define CONTROL_RESPONSE_SIZE 65536


typedef int (*ControlHandler)(const char *command, char *response, size_t capacity, void *context);

typedef struct {
    int listener;
    int wake_pipe[2];
    pthread_t thread;
    int started;
    ControlHandler handler;
    void *context;
} ControlServer;

int control_start(ControlServer *server, ControlHandler handler, void *context);

int control_stop(ControlServer *server);

#endif