#ifndef FRAME_IO_H
#define FRAME_IO_H

#include "frame.h"

/* The caller supplies a payload buffer of at least FRAME_MAX_PAYLOAD bytes.
   Returns 1 for a complete frame, 0 for EOF between frames, or -1 on error. */
int frame_read(int fd, FrameHeader *header, uint8_t payload[FRAME_MAX_PAYLOAD]);

/* payload must contain at least header->length readable bytes.
   It may be NULL when length is zero. Returns 0 on success or -1 on error. */
int frame_write(int fd, const FrameHeader *header, const uint8_t *payload);

#endif