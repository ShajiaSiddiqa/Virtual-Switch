#include "mac_table.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    MacTable table;
    const uint8_t mac[FRAME_MAC_SIZE] = {0x02, 0, 0, 0, 0, 1};
    MacRecord records[MAC_TABLE_CAPACITY];
    size_t count = 99;
    int port = -1;

    assert(mac_table_init(&table) == 0);
    assert(mac_table_insert(&table, mac, 2) == 0);

    assert(mac_table_snapshot(&table, records, 0, &count) == -1);
    assert(errno == ENOSPC);
    assert(count == 99);

    assert(mac_table_snapshot(&table, records,
                               MAC_TABLE_CAPACITY, &count) == 0);
    assert(count == 1);
    assert(memcmp(records[0].mac, mac, FRAME_MAC_SIZE) == 0);
    assert(records[0].port == 2);

    assert(mac_table_clear(&table) == 0);
    assert(table.entry_count == 0);
    assert(mac_table_lookup(&table, mac, &port) == 0);

    /* The snapshot remains valid after live entries are freed. */
    assert(memcmp(records[0].mac, mac, FRAME_MAC_SIZE) == 0);

    assert(mac_table_snapshot(&table, records,
                               MAC_TABLE_CAPACITY, &count) == 0);
    assert(count == 0);
    assert(mac_table_clear(&table) == 0);

    assert(mac_table_insert(&table, mac, 3) == 0);
    assert(mac_table_lookup(&table, mac, &port) == 1 && port == 3);
    assert(mac_table_destroy(&table) == 0);

    puts("PASS: MAC snapshot, bounds, clear, and relearning");
    return 0;
}