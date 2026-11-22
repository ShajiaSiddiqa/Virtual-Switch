#define _POSIX_C_SOURCE 200809L
#include "checked.h"
#include "frame_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned int schedule[] = {0, 0, 0, 0, 0, 1, 1, 1, 2};

static void check_thread(int error)
{
    if (error != 0) {
        vsw_posix(fprintf(stderr, "Frame queue: %s\n", strerror(error)), "fprintf");
        abort();
    }
}

static struct timespec monotonic_now(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        perror("queue clock_gettime");
        abort();
    }
    return now;
}

static int earlier(struct timespec a, struct timespec b)
{
    return a.tv_sec < b.tv_sec ||
           (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec);
}

/* Caller holds queue->mutex; queue is nonempty. */
static unsigned int choose_priority(FrameQueue *queue)
{
    struct timespec now = monotonic_now();
    int oldest = -1;

    for (unsigned int p = 0; p < FRAME_QUEUE_PRIORITIES; p++) {
        PriorityQueue *priority = &queue->priorities[p];
        if (priority->count == 0)
            continue;

        struct timespec timestamp = priority->enqueued[priority->head];
        struct timespec due = timestamp;
        due.tv_sec += FRAME_QUEUE_AGING_MS / 1000;
        due.tv_nsec += (FRAME_QUEUE_AGING_MS % 1000) * 1000000L;
        if (due.tv_nsec >= 1000000000L) {
            due.tv_sec++;
            due.tv_nsec -= 1000000000L;
        }

        if (!earlier(now, due) &&
            (oldest < 0 || earlier(timestamp,
                queue->priorities[oldest].enqueued[
                    queue->priorities[oldest].head]))) {
            oldest = (int)p;
        }
    }

    /* Overdue frames override weights; select the oldest class head. */
    if (oldest >= 0)
        return (unsigned int)oldest;

    for (size_t i = 0; i < sizeof(schedule) / sizeof(schedule[0]); i++) {
        unsigned int p = schedule[queue->schedule_next];
        queue->schedule_next = (queue->schedule_next + 1) %
                               (sizeof(schedule) / sizeof(schedule[0]));
        if (queue->priorities[p].count != 0)
            return p;
    }

    abort(); /* Nonempty queue must have a nonempty priority. */
}

void frame_queue_init(FrameQueue *queue)
{
    memset(queue, 0, sizeof(*queue));
    check_thread(pthread_mutex_init(&queue->mutex, NULL));
    check_thread(pthread_cond_init(&queue->ready, NULL));
}

int frame_queue_try_push(FrameQueue *queue, SharedFrame *frame)
{
    if (frame == NULL || frame->header.priority >= FRAME_QUEUE_PRIORITIES)
        return 0;

    check_thread(pthread_mutex_lock(&queue->mutex));
    PriorityQueue *priority = &queue->priorities[frame->header.priority];
    if (queue->closed || priority->count == FRAME_QUEUE_CAPACITY) {
        check_thread(pthread_mutex_unlock(&queue->mutex));
        return 0;
    }

    size_t tail = (priority->head + priority->count) % FRAME_QUEUE_CAPACITY;
    priority->enqueued[tail] = monotonic_now();
    shared_frame_retain(frame);
    priority->slots[tail] = frame;
    priority->count++;
    queue->count++;
    check_thread(pthread_cond_signal(&queue->ready));
    check_thread(pthread_mutex_unlock(&queue->mutex));
    return 1;
}

int frame_queue_pop(FrameQueue *queue, SharedFrame **frame)
{
    check_thread(pthread_mutex_lock(&queue->mutex));
    while (queue->count == 0 && !queue->closed)
        check_thread(pthread_cond_wait(&queue->ready, &queue->mutex));

    if (queue->count == 0) {
        *frame = NULL;
        check_thread(pthread_mutex_unlock(&queue->mutex));
        return 0;
    }

    PriorityQueue *priority = &queue->priorities[choose_priority(queue)];
    *frame = priority->slots[priority->head];
    priority->slots[priority->head] = NULL;
    priority->head = (priority->head + 1) % FRAME_QUEUE_CAPACITY;
    priority->count--;
    queue->count--;
    check_thread(pthread_mutex_unlock(&queue->mutex));
    return 1;
}

void frame_queue_close(FrameQueue *queue)
{
    check_thread(pthread_mutex_lock(&queue->mutex));
    queue->closed = 1;
    check_thread(pthread_cond_broadcast(&queue->ready));
    check_thread(pthread_mutex_unlock(&queue->mutex));
}

void frame_queue_destroy(FrameQueue *queue)
{
    for (unsigned int p = 0; p < FRAME_QUEUE_PRIORITIES; p++) {
        PriorityQueue *priority = &queue->priorities[p];
        while (priority->count > 0) {
            shared_frame_release(priority->slots[priority->head]);
            priority->slots[priority->head] = NULL;
            priority->head = (priority->head + 1) % FRAME_QUEUE_CAPACITY;
            priority->count--;
        }
    }
    queue->count = 0;
    check_thread(pthread_cond_destroy(&queue->ready));
    check_thread(pthread_mutex_destroy(&queue->mutex));
}

int frame_queue_wait(FrameQueue *queue)
{
    check_thread(pthread_mutex_lock(&queue->mutex));
    while (queue->count == 0 && !queue->closed)
        check_thread(pthread_cond_wait(&queue->ready, &queue->mutex));
    int available = queue->count != 0;
    check_thread(pthread_mutex_unlock(&queue->mutex));
    return available;
}

int frame_queue_try_pop(FrameQueue *queue, SharedFrame **frame)
{
    check_thread(pthread_mutex_lock(&queue->mutex));
    if (queue->count == 0) {
        *frame = NULL;
        check_thread(pthread_mutex_unlock(&queue->mutex));
        return 0;
    }
    PriorityQueue *priority = &queue->priorities[choose_priority(queue)];
    *frame = priority->slots[priority->head];
    priority->slots[priority->head] = NULL;
    priority->head = (priority->head + 1) % FRAME_QUEUE_CAPACITY;
    priority->count--;
    queue->count--;
    check_thread(pthread_mutex_unlock(&queue->mutex));
    return 1;
}

size_t frame_queue_clear(FrameQueue *queue)
{
    check_thread(pthread_mutex_lock(&queue->mutex));
    size_t removed = queue->count;
    for (unsigned int p = 0; p < FRAME_QUEUE_PRIORITIES; p++) {
        PriorityQueue *priority = &queue->priorities[p];
        while (priority->count != 0) {
            shared_frame_release(priority->slots[priority->head]);
            priority->slots[priority->head] = NULL;
            priority->head = (priority->head + 1) % FRAME_QUEUE_CAPACITY;
            priority->count--;
        }
    }
    queue->count = 0;
    queue->schedule_next = 0;
    check_thread(pthread_mutex_unlock(&queue->mutex));
    return removed;
}
