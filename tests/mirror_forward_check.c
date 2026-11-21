#define _POSIX_C_SOURCE 200809L
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
    AdminContext admin = {.ports=ports,.num_ports=4,.ports_lock=&lock,.mac_table=&table};
    char response[4096];
    assert(mac_table_insert_vlan(&table, b, 10, 1) == 0);
    assert(handle_admin("mirror 1 3", response, sizeof(response), &admin) == 0);
    assert(ports[2].sniffer && ports[0].mirror_target == 3);
    SharedFrame *frame = packet(10, a, b);
    assert(forward_frame(&ports[0], frame) == 0);
    assert(atomic_load(&frame->references) == 3);
    SharedFrame *normal, *copy;
    assert(frame_queue_try_pop(&ports[1].output, &normal) == 1);
    assert(frame_queue_try_pop(&ports[2].output, &copy) == 1);
    assert(normal == frame && copy == frame);
    assert(copy->header.vlan == 10);
    shared_frame_release(normal); shared_frame_release(copy);
    assert(atomic_load(&frame->references) == 1);
    shared_frame_release(frame);
    puts("PASS: known unicast mirrored across VLANs without payload copies");

    frame = packet(20, a, broadcast);
    assert(forward_frame(&ports[2], frame) == 0);
    shared_frame_release(frame);
    for (int i=0; i<4; i++) expect(&ports[i], 0, 20);
    puts("PASS: dedicated sniffer cannot inject ordinary traffic");

    for (unsigned int i=0; i<FRAME_QUEUE_CAPACITY; i++) {
        frame = packet(10, a, b);
        assert(forward_frame(&ports[0], frame) == 0);
        shared_frame_release(frame);
        expect(&ports[1], 1, 10);
    }
    unsigned int before = atomic_load(&ports[2].dropped);
    frame = packet(10, a, b);
    assert(forward_frame(&ports[0], frame) == 0);
    assert(atomic_load(&frame->references) == 2);
    shared_frame_release(frame);
    expect(&ports[1], 1, 10);
    assert(atomic_load(&ports[2].dropped) == before + 1);
    assert(memory.ports[2].frames_mirrored == 1 + FRAME_QUEUE_CAPACITY);
    puts("PASS: full sniffer queue drops only mirror copy and counts rejection");

    assert(handle_admin("mirror 1 1", response, sizeof(response), &admin) == -1);
    assert(handle_admin("mirror 3 2", response, sizeof(response), &admin) == -1);
    assert(handle_admin("mirror 2 1", response, sizeof(response), &admin) == -1);
    assert(handle_admin("trunk 3", response, sizeof(response), &admin) == -1);
    assert(handle_admin("mirror 1 0", response, sizeof(response), &admin) == 0);
    assert(!ports[2].sniffer && !ports[0].mirror_target);
    expect(&ports[2], 0, 10);
    frame = packet(10, a, b);
    assert(forward_frame(&ports[0], frame) == 0);
    shared_frame_release(frame);
    expect(&ports[1], 1, 10); expect(&ports[2], 0, 10);
    puts("PASS: validation, disable, queued-reference purge, and forwarding");
    for (int i=0; i<4; i++) {
        frame_queue_destroy(&ports[i].output);
        assert(pthread_mutex_destroy(&ports[i].transmit_lock) == 0);
    }
    assert(pthread_mutex_destroy(&lock) == 0);
    assert(pthread_mutex_destroy(&memory.mutex) == 0);
    assert(mac_table_destroy(&table) == 0);
}
