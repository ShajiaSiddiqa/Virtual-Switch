#include "frame_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_thread(int error)
{
    if (error != 0) {
        fprintf(stderr, "Frame queue: %s\n", strerror(error));
        abort();
    }
}

void frame_queue_init(FrameQueue *queue)
{
    queue->head = 0;
    queue->count = 0;
    queue->closed = 0;
    check_thread(pthread_mutex_init(&queue->mutex, NULL));
    check_thread(pthread_cond_init(&queue->ready, NULL));
}

// int frame_queue_try_push(FrameQueue *queue, const QueuedFrame *frame)
// {
//     check_thread(pthread_mutex_lock(&queue->mutex));

//     if (queue->closed || queue->count == FRAME_QUEUE_CAPACITY) {
//         check_thread(pthread_mutex_unlock(&queue->mutex));
//         return 0;
//     }

//     size_t tail = (queue->head + queue->count) % FRAME_QUEUE_CAPACITY;
//     queue->slots[tail] = *frame;
//     queue->count++;
//     check_thread(pthread_cond_signal(&queue->ready));

//     check_thread(pthread_mutex_unlock(&queue->mutex));
//     return 1;
// }

// int frame_queue_pop(FrameQueue *queue, QueuedFrame *frame)
// {
//     check_thread(pthread_mutex_lock(&queue->mutex));

//     while (queue->count == 0 && !queue->closed) {
//         check_thread(pthread_cond_wait(&queue->ready, &queue->mutex));
//     }

//     if (queue->count == 0) {
//         check_thread(pthread_mutex_unlock(&queue->mutex));
//         return 0;
//     }

//     *frame = queue->slots[queue->head];
//     queue->head = (queue->head + 1) % FRAME_QUEUE_CAPACITY;
//     queue->count--;

//     check_thread(pthread_mutex_unlock(&queue->mutex));
//     return 1;
// }

int frame_queue_try_push(FrameQueue *queue, SharedFrame *frame)
{
    check_thread(pthread_mutex_lock(&queue->mutex));

    if (queue->closed || queue->count == FRAME_QUEUE_CAPACITY) {
        check_thread(pthread_mutex_unlock(&queue->mutex));
        return 0;
    }

    size_t tail = (queue->head + queue->count) % FRAME_QUEUE_CAPACITY;
    shared_frame_retain(frame);
    queue->slots[tail] = frame;
    queue->count++;
    check_thread(pthread_cond_signal(&queue->ready));

    check_thread(pthread_mutex_unlock(&queue->mutex));
    return 1;
}

int frame_queue_pop(FrameQueue *queue, SharedFrame **frame)
{
    check_thread(pthread_mutex_lock(&queue->mutex));

    while (queue->count == 0 && !queue->closed) {
        check_thread(pthread_cond_wait(&queue->ready, &queue->mutex));
    }

    if (queue->count == 0) {
        *frame = NULL;
        check_thread(pthread_mutex_unlock(&queue->mutex));
        return 0;
    }

    *frame = queue->slots[queue->head];
    queue->slots[queue->head] = NULL;
    queue->head = (queue->head + 1) % FRAME_QUEUE_CAPACITY;
    queue->count--;

    check_thread(pthread_mutex_unlock(&queue->mutex));
    return 1;
}

void frame_queue_destroy(FrameQueue *queue)
{
    while (queue->count > 0) {
        shared_frame_release(queue->slots[queue->head]);
        queue->slots[queue->head] = NULL;
        queue->head = (queue->head + 1) % FRAME_QUEUE_CAPACITY;
        queue->count--;
    }

    check_thread(pthread_cond_destroy(&queue->ready));
    check_thread(pthread_mutex_destroy(&queue->mutex));
}

void frame_queue_close(FrameQueue *queue)
{
    check_thread(pthread_mutex_lock(&queue->mutex));
    queue->closed = 1;
    check_thread(pthread_cond_broadcast(&queue->ready));
    check_thread(pthread_mutex_unlock(&queue->mutex));
}

// void frame_queue_destroy(FrameQueue *queue)
// {
//     check_thread(pthread_cond_destroy(&queue->ready));
//     check_thread(pthread_mutex_destroy(&queue->mutex));
// }