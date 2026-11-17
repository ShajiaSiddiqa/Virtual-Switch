#include "frame_queue.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    FrameQueue queue;
    QueuedFrame input = {.header = {.length = 1, .vlan = 1, .priority = 1}};
    QueuedFrame output;

    frame_queue_init(&queue);

    for (int i = 0; i < FRAME_QUEUE_CAPACITY; i++) {
        input.payload[0] = (uint8_t)i;
        assert(frame_queue_try_push(&queue, &input) == 1);
    }
    assert(frame_queue_try_push(&queue, &input) == 0);

    assert(frame_queue_pop(&queue, &output) == 1);
    assert(output.payload[0] == 0);

    input.payload[0] = FRAME_QUEUE_CAPACITY;
    assert(frame_queue_try_push(&queue, &input) == 1);

    frame_queue_close(&queue);
    assert(frame_queue_try_push(&queue, &input) == 0);

    for (int i = 1; i <= FRAME_QUEUE_CAPACITY; i++) {
        assert(frame_queue_pop(&queue, &output) == 1);
        assert(output.payload[0] == i);
    }
    assert(frame_queue_pop(&queue, &output) == 0);

    frame_queue_destroy(&queue);
    puts("PASS: capacity, FIFO wraparound, and close");
    return 0;
}
