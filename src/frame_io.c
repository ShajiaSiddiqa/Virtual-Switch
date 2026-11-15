#include "frame_io.h"
#include "io.h"

#include <errno.h>

int frame_read(int fd, FrameHeader *header, uint8_t payload[FRAME_MAX_PAYLOAD])
{
    uint8_t wire[FRAME_HEADER_SIZE];

    ssize_t result = read_full(fd, wire, sizeof(wire));
    if (result <= 0) 
        return (int)result;

    if (frame_deserialize(wire, sizeof(wire), header) != 0) 
    {
        errno = EPROTO;
        return -1;
    }

    if (header->length == 0) 
        return 1;

    result = read_full(fd, payload, header->length);
    if (result <= 0) 
    {
        if (result == 0) 
            errno = EPROTO;
        return -1;
    }

    return 1;
}

int frame_write(int fd, const FrameHeader *header, const uint8_t *payload)
{
    uint8_t wire[FRAME_HEADER_SIZE];

    if (frame_serialize(header, wire, sizeof(wire)) != 0 || (header->length > 0 && payload == NULL)) 
    {
        errno = EINVAL;
        return -1;
    }

    if (write_full(fd, wire, sizeof(wire)) != 0) 
        return -1;

    return write_full(fd, payload, header->length);
}