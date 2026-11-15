#ifndef FRAME_H
#define FRAME_H

#include <stddef.h>
#include <stdint.h>

#define FRAME_MAC_SIZE    6
#define FRAME_HEADER_SIZE 17
#define FRAME_MAX_PAYLOAD 1500

typedef struct __attribute__((packed)) 
{
    uint16_t length;              // 0 to 1500 bytes 
    uint8_t src[FRAME_MAC_SIZE];  
    uint8_t dst[FRAME_MAC_SIZE];  
    uint16_t vlan;                // 1 to 4094 
    uint8_t priority;             // 0 = high, 1 = normal, 2 = low
} FrameHeader;

_Static_assert(sizeof(FrameHeader) == FRAME_HEADER_SIZE,"FrameHeader must be exactly 17 bytes");

int frame_serialize(const FrameHeader *header, uint8_t *buffer, size_t buffer_size);

int frame_deserialize(const uint8_t *buffer, size_t buffer_size, FrameHeader *header);

#endif

