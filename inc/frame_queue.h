#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include "frame.h"
#include <pthread.h>

#define FRAME_QUEUE_CAPACITY 8

typedef struct {
    FrameHeader header;
    uint8_t payload[FRAME_MAX_PAYLOAD];
} QueuedFrame;

typedef struct {
    QueuedFrame slots[FRAME_QUEUE_CAPACITY];
    size_t head;
    size_t count;
    int closed;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
} FrameQueue;

void frame_queue_init(FrameQueue *queue);

/* Returns 1 if queued, or 0 if full or closed. */
int frame_queue_try_push(FrameQueue *queue, const QueuedFrame *frame);

/* Waits for a frame. Returns 1 on success, or 0 when closed and empty. */
int frame_queue_pop(FrameQueue *queue, QueuedFrame *frame);

void frame_queue_close(FrameQueue *queue);

/* Call after all threads using this queue have finished. */
void frame_queue_destroy(FrameQueue *queue);

#endif