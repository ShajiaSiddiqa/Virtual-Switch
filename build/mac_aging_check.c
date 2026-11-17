#define _POSIX_C_SOURCE 200809L

#include "mac_table.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

int main(void)
{
    MacTable table;
    const uint8_t mac[FRAME_MAC_SIZE] = {0x02, 0, 0, 0, 0, 1};
    int port = -1;

    assert(mac_table_init(&table) == 0);
    assert(mac_table_insert(&table, mac, 2) == 0);
    assert(mac_table_lookup(&table, mac, &port) == 1 && port == 2);
    assert(mac_table_expire(&table, 60) == 0);

    struct timespec remaining = {.tv_sec = 1, .tv_nsec = 100000000};
    while (nanosleep(&remaining, &remaining) == -1) {
        assert(errno == EINTR);
    }

    assert(mac_table_expire(&table, 1) == 1);
    assert(mac_table_lookup(&table, mac, &port) == 0);

    assert(mac_table_insert(&table, mac, 3) == 0);
    assert(mac_table_lookup(&table, mac, &port) == 1 && port == 3);
    assert(mac_table_destroy(&table) == 0);

    puts("PASS: fresh entry kept, stale entry expired, MAC relearned");
    return 0;
}
