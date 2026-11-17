#include "shared_frame.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

SharedFrame *shared_frame_create(const FrameHeader *header,
                                 const uint8_t *payload)
{
    if (header == NULL || header->length > FRAME_MAX_PAYLOAD ||
        (header->length != 0 && payload == NULL)) {
        errno = EINVAL;
        return NULL;
    }

    SharedFrame *frame = malloc(sizeof(*frame));
    if (frame == NULL) {
        return NULL;
    }

    atomic_init(&frame->references, 1);
    frame->header = *header;

    if (header->length != 0) {
        memcpy(frame->payload, payload, header->length);
    }

    return frame;
}

void shared_frame_retain(SharedFrame *frame)
{
    atomic_fetch_add_explicit(&frame->references, 1, memory_order_relaxed);
}

void shared_frame_release(SharedFrame *frame)
{
    if (atomic_fetch_sub_explicit(&frame->references, 1,
                                  memory_order_acq_rel) == 1) {
        free(frame);
    }
}