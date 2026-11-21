#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include "shared_frame.h"
#include <pthread.h>
#include <time.h>

#define FRAME_QUEUE_PRIORITIES 3
/* Capacity per priority; each port holds at most 24 queued frames. */
#define FRAME_QUEUE_CAPACITY 8
#define FRAME_QUEUE_AGING_MS 100

typedef struct {
    SharedFrame *slots[FRAME_QUEUE_CAPACITY];
    struct timespec enqueued[FRAME_QUEUE_CAPACITY];
    size_t head;
    size_t count;
} PriorityQueue;

typedef struct {
    PriorityQueue priorities[FRAME_QUEUE_PRIORITIES];
    size_t count;
    size_t schedule_next;
    int closed;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
} FrameQueue;

void frame_queue_init(FrameQueue *queue);
/* Returns 1 and retains a reference, or 0 without retaining on rejection. */
int frame_queue_try_push(FrameQueue *queue, SharedFrame *frame);
/* Transfers one reference to the caller; closed and empty returns 0. */
int frame_queue_pop(FrameQueue *queue, SharedFrame **frame);
/* Reject new deliveries and wake consumers; queued frames remain drainable. */
void frame_queue_close(FrameQueue *queue);
/* After all users stop, release queued references and destroy synchronization. */
void frame_queue_destroy(FrameQueue *queue);
#endif
