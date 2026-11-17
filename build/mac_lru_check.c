#include "mac_table.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    MacTable table;
    const uint8_t a[FRAME_MAC_SIZE] = {0x02, 0, 0, 0, 0, 1};
    const uint8_t b[FRAME_MAC_SIZE] = {0x02, 0, 0, 0, 0, 2};
    const uint8_t c[FRAME_MAC_SIZE] = {0x02, 0, 0, 0, 0, 3};
    int port = -1;

    assert(mac_table_init(&table) == 0);
    assert(mac_table_insert(&table, a, 1) == 0);
    assert(mac_table_insert(&table, b, 2) == 0);

    /* Updating B in a full table must leave A in place. */
    assert(mac_table_insert(&table, b, 4) == 0);
    assert(table.entry_count == 2);
    assert(mac_table_lookup(&table, b, &port) == 1 && port == 4);

    /* Looking up A makes B the least recently used entry. */
    assert(mac_table_lookup(&table, a, &port) == 1 && port == 1);
    assert(mac_table_insert(&table, c, 3) == 0);

    assert(table.entry_count == 2);
    assert(mac_table_lookup(&table, b, &port) == 0);
    assert(mac_table_lookup(&table, a, &port) == 1 && port == 1);
    assert(mac_table_lookup(&table, c, &port) == 1 && port == 3);

    assert(mac_table_destroy(&table) == 0);
    puts("PASS: updating a full table keeps both entries; inserting C evicts B");
    return 0;
}
