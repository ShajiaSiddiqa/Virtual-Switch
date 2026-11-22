#ifndef PIPE_LINK_H
#define PIPE_LINK_H
#include "frame.h"
#include <stdatomic.h>

typedef struct {
    int read_fd;
    int write_fd;
    int wake[2];
    atomic_int stopped;
} PipeLink;

/* Create endpoints before fork; close the unused endpoint in each process. */
int pipe_link_pair(PipeLink endpoints[2]);
/* Adopt distinct FIFO read/write descriptors; ownership transfers on success. */
int pipe_link_adopt(PipeLink *link, int read_fd, int write_fd);
/* One receiver and one sender per endpoint; no concurrent senders/receivers. */
int pipe_link_receive(PipeLink *link, FrameHeader *header,
                      uint8_t payload[FRAME_MAX_PAYLOAD]); /* 1 frame, 0 EOF, -1 error */
int pipe_link_send(PipeLink *link, const FrameHeader *header,
                   const uint8_t *payload); /* 0 success, -1 error */
/* Wake this process's blocked reader/writer; stopped endpoint cannot restart. */
int pipe_link_stop(PipeLink *link);
/* Only after all users join. Also use to close inherited unused endpoints. */
int pipe_link_destroy(PipeLink *link);
#endif
