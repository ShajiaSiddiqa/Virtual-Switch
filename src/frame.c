#include "frame.h"

#include <arpa/inet.h>  
#include <string.h>     

enum {
    LENGTH_OFFSET = 0,
    SRC_OFFSET = 2,
    DST_OFFSET = 8,
    VLAN_OFFSET = 14,
    PRIORITY_OFFSET = 16
};

static int header_fields_valid(uint16_t length, uint16_t vlan, uint8_t priority)
{
    return length <= FRAME_MAX_PAYLOAD &&
           vlan >= 1 && vlan <= 4094 &&
           priority <= 2;
}

// FrameHeader -> Bytes
int frame_serialize(const FrameHeader *header, uint8_t *buffer, size_t buffer_size)
{
    if (header == NULL || buffer == NULL || buffer_size < FRAME_HEADER_SIZE) 
        return -1;
    
    if (!header_fields_valid(header->length, header->vlan, header->priority)) 
        return -1;

    uint16_t network_length = htons(header->length);
    uint16_t network_vlan = htons(header->vlan);

    memcpy(buffer + LENGTH_OFFSET, &network_length, sizeof(network_length));
    memcpy(buffer + SRC_OFFSET, header->src, FRAME_MAC_SIZE);
    memcpy(buffer + DST_OFFSET, header->dst, FRAME_MAC_SIZE);
    memcpy(buffer + VLAN_OFFSET, &network_vlan, sizeof(network_vlan));
    buffer[PRIORITY_OFFSET] = header->priority;

    return 0;
}

//  Bytes -> FrameHeader
int frame_deserialize(const uint8_t *buffer, size_t buffer_size, FrameHeader *header)
{
    if (buffer == NULL || header == NULL || buffer_size < FRAME_HEADER_SIZE) 
        return -1;

    uint16_t network_length;
    uint16_t network_vlan;

    memcpy(&network_length, buffer + LENGTH_OFFSET, sizeof(network_length));
    memcpy(&network_vlan, buffer + VLAN_OFFSET, sizeof(network_vlan));

    uint16_t length = ntohs(network_length);
    uint16_t vlan = ntohs(network_vlan);
    uint8_t priority = buffer[PRIORITY_OFFSET];

    if (!header_fields_valid(length, vlan, priority)) 
        return -1;

    header->length = length;
    memcpy(header->src, buffer + SRC_OFFSET, FRAME_MAC_SIZE);
    memcpy(header->dst, buffer + DST_OFFSET, FRAME_MAC_SIZE);
    header->vlan = vlan;
    header->priority = priority;

    return 0;
}
