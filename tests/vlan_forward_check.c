#define _POSIX_C_SOURCE 200809L
#include "checked.h"
#define main switch_program_main
#include "../src/switch.c"
#undef main
#include <assert.h>

static SharedFrame *packet(uint16_t vlan, const uint8_t *src, const uint8_t *dst)
{
    FrameHeader h = {.length = 1, .vlan = vlan, .priority = 1};
    memcpy(h.src, src, FRAME_MAC_SIZE);
    memcpy(h.dst, dst, FRAME_MAC_SIZE);
    const uint8_t payload = 42;
    SharedFrame *frame = shared_frame_create(&h, &payload);
    assert(frame);
    return frame;
}
static void expect(Port *port, int present, uint16_t vlan)
{
    SharedFrame *frame;
    assert(frame_queue_try_pop(&port->output, &frame) == present);
    if (present) {
        assert(frame->header.vlan == vlan);
        shared_frame_release(frame);
    }
}
int main(void)
{
    MacTable table;
    assert(mac_table_init(&table) == 0);
    SharedStats memory = {.num_ports = 4};
    assert(pthread_mutex_init(&memory.mutex, NULL) == 0);
    StatsHandle stats = {.shared = &memory};
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    Port ports[4] = {0};
    for (int i = 0; i < 4; i++) {
        ports[i].fd = -1;
        ports[i].active = 1;
        ports[i].ports = ports;
        ports[i].num_ports = 4;
        ports[i].vlan = i == 2 ? 20 : 10;
        ports[i].trunk = i == 3;
        ports[i].stats = &stats;
        ports[i].mac_table = &table;
        ports[i].ports_lock = &lock;
        atomic_init(&ports[i].dropped, 0);
        frame_queue_init(&ports[i].output);
        assert(pthread_mutex_init(&ports[i].transmit_lock, NULL) == 0);
    }
    const uint8_t a[6] = {2,0,0,0,0,1}, b[6] = {2,0,0,0,0,2};
    const uint8_t broadcast[6] = {255,255,255,255,255,255};
    SharedFrame *frame = packet(10, a, broadcast);
    assert(forward_frame(&ports[0], frame) == 0);
    shared_frame_release(frame);
    expect(&ports[0], 0, 10); expect(&ports[1], 1, 10);
    expect(&ports[2], 0, 10); expect(&ports[3], 1, 10);
    vsw_posix(puts("PASS: access broadcast isolated and trunk tag preserved"), "puts");

    frame = packet(20, a, broadcast);
    assert(forward_frame(&ports[0], frame) == 0);
    shared_frame_release(frame);
    int destination;
    assert(mac_table_lookup_vlan(&table, a, 20, &destination) == 0);
    for (int i = 0; i < 4; i++) expect(&ports[i], 0, 20);
    vsw_posix(puts("PASS: mismatched access ingress dropped before learning"), "puts");

    frame = packet(20, a, broadcast);
    assert(forward_frame(&ports[3], frame) == 0);
    shared_frame_release(frame);
    expect(&ports[0], 0, 20); expect(&ports[1], 0, 20);
    expect(&ports[2], 1, 20); expect(&ports[3], 0, 20);
    assert(mac_table_insert_vlan(&table, b, 10, 1) == 0);
    assert(mac_table_insert_vlan(&table, b, 20, 2) == 0);
    frame = packet(10, a, b);
    assert(forward_frame(&ports[0], frame) == 0);
    shared_frame_release(frame);
    expect(&ports[1], 1, 10); expect(&ports[2], 0, 10);
    expect(&ports[3], 0, 10);
    vsw_posix(puts("PASS: trunk ingress isolation and VLAN-specific known unicast"), "puts");

    frame = packet(10, a, broadcast);
    assert(forward_frame(&ports[0], frame) == 0);
    shared_frame_release(frame);
    AdminContext admin = {.ports=ports,.num_ports=4,.ports_lock=&lock,.mac_table=&table};
    char response[4096];
    assert(handle_admin("vlan 2 20", response, sizeof(response), &admin) == 0);
    expect(&ports[1], 0, 10); expect(&ports[3], 1, 10);
    assert(ports[1].vlan == 20);
    assert(mac_table_lookup_vlan(&table, b, 10, &destination) == 0);
    assert(handle_admin("vlan 0 20", response, sizeof(response), &admin) == -1);
    assert(handle_admin("vlan 2 4095", response, sizeof(response), &admin) == -1);
    assert(handle_admin("vlan 2 20 trailing", response, sizeof(response), &admin) == -1);
    assert(handle_admin("trunk 1", response, sizeof(response), &admin) == 0);
    assert(ports[0].trunk && !ports[3].trunk);
    assert(handle_admin("trunk 0", response, sizeof(response), &admin) == 0);
    for (int i = 0; i < 4; i++) assert(!ports[i].trunk);
    vsw_posix(puts("PASS: live VLAN purge, MAC invalidation, command validation, single trunk"), "puts");
    for (int i = 0; i < 4; i++) {
        frame_queue_destroy(&ports[i].output);
        assert(pthread_mutex_destroy(&ports[i].transmit_lock) == 0);
    }
    assert(pthread_mutex_destroy(&lock) == 0);
    assert(pthread_mutex_destroy(&memory.mutex) == 0);
    assert(mac_table_destroy(&table) == 0);
}
