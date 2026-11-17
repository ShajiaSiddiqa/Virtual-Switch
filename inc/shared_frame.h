#ifndef SHARED_FRAME_H
#define SHARED_FRAME_H

#include "frame.h"
#include <stdatomic.h>

typedef struct {
    atomic_uint references;
    FrameHeader header;
    uint8_t payload[FRAME_MAX_PAYLOAD];
} SharedFrame;

/* On success, the caller owns one reference. */
SharedFrame *shared_frame_create(const FrameHeader *header,
                                 const uint8_t *payload);

/* A reference may be retained only while the caller owns a live reference. */
void shared_frame_retain(SharedFrame *frame);

/* Give up one owned reference. The last release frees the frame. */
void shared_frame_release(SharedFrame *frame);

#endif