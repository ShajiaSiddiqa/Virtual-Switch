#define _POSIX_C_SOURCE 200809L
#include "frame_queue.h"
#include <assert.h>
#include <stdio.h>

static SharedFrame *make_frame(unsigned int p, unsigned int value)
{
    FrameHeader header = {.length = 1, .vlan = 1, .priority = (uint8_t)p};
    uint8_t payload = (uint8_t)value;
    SharedFrame *frame = shared_frame_create(&header, &payload);
    assert(frame != NULL);
    return frame;
}

static void push(FrameQueue *queue, unsigned int p, unsigned int value)
{
    SharedFrame *frame = make_frame(p, value);
    assert(frame_queue_try_push(queue, frame) == 1);
    assert(atomic_load(&frame->references) == 2);
    shared_frame_release(frame);
}

static void *empty_consumer(void *arg)
{
    SharedFrame *frame;
    assert(frame_queue_pop(arg, &frame) == 0);
    assert(frame == NULL);
    return NULL;
}

int main(void)
{
    FrameQueue queue;
    SharedFrame *frame;
    frame_queue_init(&queue);

    for (unsigned int p = 0; p < 3; p++) {
        for (unsigned int i = 0; i < FRAME_QUEUE_CAPACITY; i++)
            push(&queue, p, i);
        SharedFrame *extra = make_frame(p, 99);
        assert(frame_queue_try_push(&queue, extra) == 0);
        assert(atomic_load(&extra->references) == 1);
        shared_frame_release(extra);
    }
    assert(queue.count == 3 * FRAME_QUEUE_CAPACITY);
    frame_queue_destroy(&queue);
    puts("PASS: independent bounded queues and rejected enqueue ownership");

    frame_queue_init(&queue);
    push(&queue, 0, 0);
    push(&queue, 1, 0);
    push(&queue, 2, 0);
    const unsigned int expected[] = {0,0,0,0,0,1,1,1,2};
    for (unsigned int i = 0; i < 90; i++) {
        assert(frame_queue_pop(&queue, &frame) == 1);
        assert(frame->header.priority == expected[i % 9]);
        unsigned int p = frame->header.priority;
        shared_frame_release(frame);
        push(&queue, p, i);
    }
    frame_queue_destroy(&queue);
    puts("PASS: weighted round robin 5:3:1 with replenished classes");

    frame_queue_init(&queue);
    push(&queue, 0, 1);
    push(&queue, 2, 2);
    /* Deterministically simulate elapsed time without sleeping. */
    assert(pthread_mutex_lock(&queue.mutex) == 0);
    queue.priorities[2].enqueued[queue.priorities[2].head].tv_sec -= 1;
    assert(pthread_mutex_unlock(&queue.mutex) == 0);
    assert(frame_queue_pop(&queue, &frame) == 1);
    assert(frame->header.priority == 2);
    shared_frame_release(frame);
    frame_queue_destroy(&queue);
    puts("PASS: overdue low priority overrides high-priority schedule");

    frame_queue_init(&queue);
    for (unsigned int round = 0; round < 4; round++) {
        for (unsigned int i = 0; i < FRAME_QUEUE_CAPACITY; i++)
            push(&queue, 1, i);
        for (unsigned int i = 0; i < FRAME_QUEUE_CAPACITY; i++) {
            assert(frame_queue_pop(&queue, &frame) == 1);
            assert(frame->payload[0] == i);
            shared_frame_release(frame);
        }
    }
    push(&queue, 1, 42);
    frame_queue_close(&queue);
    SharedFrame *rejected = make_frame(0, 0);
    assert(frame_queue_try_push(&queue, rejected) == 0);
    shared_frame_release(rejected);
    assert(frame_queue_pop(&queue, &frame) == 1);
    assert(frame->payload[0] == 42);
    shared_frame_release(frame);
    assert(frame_queue_pop(&queue, &frame) == 0);
    frame_queue_destroy(&queue);
    puts("PASS: FIFO wraparound, close rejection, and drain");

    frame_queue_init(&queue);
    pthread_t consumer;
    assert(pthread_create(&consumer, NULL, empty_consumer, &queue) == 0);
    frame_queue_close(&queue);
    assert(pthread_join(consumer, NULL) == 0);
    frame_queue_destroy(&queue);
    puts("PASS: closed empty queue wakes/terminates consumer");
    return 0;
}
