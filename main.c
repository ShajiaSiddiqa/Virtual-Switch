#include "frame.h"
#include "mac_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int passed, const char *message)
{
    if (!passed) 
    {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void test_frame(void)
{
    const FrameHeader original = {
        .length = 258,
        .src = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01},
        .dst = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        .vlan = 291,
        .priority = 1
    };

    const uint8_t expected[FRAME_HEADER_SIZE] = {
        0x01, 0x02,                          /* Length: 258 */
        0x02, 0x00, 0x00, 0x00, 0x00, 0x01,  /* Source MAC */
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  /* Destination MAC */
        0x01, 0x23,                          /* VLAN: 291 */
        0x01                                 /* Normal priority */
    };

    uint8_t wire[FRAME_HEADER_SIZE];
    FrameHeader decoded;

    check(frame_serialize(&original, wire, sizeof(wire)) == 0,
          "Serialize a valid header");
    check(memcmp(wire, expected, sizeof(wire)) == 0,
          "Serialized bytes match the wire format");

    check(frame_deserialize(expected, sizeof(expected), &decoded) == 0,
          "Deserialize a valid header");
    check(decoded.length == original.length, "Decoded length");
    check(decoded.vlan == original.vlan, "Decoded VLAN");
    check(decoded.priority == original.priority, "Decoded priority");
    check(memcmp(decoded.src, original.src, FRAME_MAC_SIZE) == 0,
          "Decoded source MAC");
    check(memcmp(decoded.dst, original.dst, FRAME_MAC_SIZE) == 0,
          "Decoded destination MAC");

    check(frame_deserialize(expected, sizeof(expected) - 1, &decoded) == -1,
          "Reject a truncated header");

    wire[FRAME_HEADER_SIZE - 1] = 3;
    check(frame_deserialize(wire, sizeof(wire), &decoded) == -1,
          "Reject an invalid priority");

    puts("PASS: Wire header");
}

typedef struct {
    MacTable *table;
    uint8_t mac[FRAME_MAC_SIZE];
    int expected_port;
} WorkerArgs;

static void *learn_and_lookup(void *arg)
{
    const WorkerArgs *worker = arg;

    for (int i = 0; i < 1000; i++) {
        int actual_port = -1;

        check(mac_table_insert(worker->table, worker->mac,
                               worker->expected_port) == 0,
              "Worker inserts its MAC");
        check(mac_table_lookup(worker->table, worker->mac, &actual_port) == 1,
              "Worker finds its MAC");
        check(actual_port == worker->expected_port,
              "Worker finds the correct port");
    }

    return NULL;
}

static void test_mac_table(void)
{
    MacTable table;
    const uint8_t mac[FRAME_MAC_SIZE] = {0x02, 0, 0, 0, 0, 0x05};
    int port = -1;

    check(mac_table_init(&table) == 0, "Initialize the MAC table");
    check(mac_table_lookup(&table, mac, &port) == 0, "Unknown MAC misses");

    check(mac_table_insert(&table, mac, 2) == 0, "Learn MAC on port 2");
    check(mac_table_lookup(&table, mac, &port) == 1 && port == 2,
          "Find MAC on port 2");

    check(mac_table_insert(&table, mac, 5) == 0, "Move MAC to port 5");
    check(mac_table_lookup(&table, mac, &port) == 1 && port == 5,
          "Find MAC on its new port");

    puts("PASS: MAC lookup and move");

    WorkerArgs first = {&table, {0x02, 0, 0, 0, 0, 0x01}, 1};
    WorkerArgs second = {&table, {0x02, 0, 0, 0, 0, 0x11}, 3};
    pthread_t first_thread;
    pthread_t second_thread;

    check(pthread_create(&first_thread, NULL, learn_and_lookup, &first) == 0,
          "Start first worker");
    check(pthread_create(&second_thread, NULL, learn_and_lookup, &second) == 0,
          "Start second worker");

    check(pthread_join(first_thread, NULL) == 0, "Join first worker");
    check(pthread_join(second_thread, NULL) == 0, "Join second worker");

    check(mac_table_destroy(&table) == 0, "Destroy the MAC table");
    puts("PASS: Concurrent MAC insert and lookup");
}

int main(void)
{
    test_frame();
    test_mac_table();
    puts("All foundation tests passed.");
    return EXIT_SUCCESS;
}