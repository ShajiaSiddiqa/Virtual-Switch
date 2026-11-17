#include "shared_frame.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static void *consume(void *arg)
{
    SharedFrame *frame = arg;

    assert(frame->header.length == 1);
    assert(frame->payload[0] == 42);
    shared_frame_release(frame);
    return NULL;
}

int main(void)
{
    FrameHeader header = {.length = 1, .vlan = 1, .priority = 1};
    uint8_t payload = 42;
    SharedFrame *frame = shared_frame_create(&header, &payload);
    assert(frame != NULL);

    payload = 0;  /* Both workers should still see the copied value, 42. */

    pthread_t workers[2];
    for (int i = 0; i < 2; i++) {
        shared_frame_retain(frame);
        assert(pthread_create(&workers[i], NULL, consume, frame) == 0);
    }

    shared_frame_release(frame);

    for (int i = 0; i < 2; i++) {
        assert(pthread_join(workers[i], NULL) == 0);
    }

    header.length = 0;
    frame = shared_frame_create(&header, NULL);
    assert(frame != NULL);
    shared_frame_release(frame);

    puts("PASS: shared frame ownership, concurrent releases, and empty payload");
    return 0;
}
