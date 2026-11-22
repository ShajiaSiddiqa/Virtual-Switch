#include "checked.h"
#include "mac_table.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    MacTable table;
    const uint8_t mac[FRAME_MAC_SIZE] = {2,0,0,0,0,1};
    int port = -1;
    assert(mac_table_init(&table) == 0);
    assert(mac_table_insert_vlan(&table, mac, 10, 0) == 0);
    assert(mac_table_insert_vlan(&table, mac, 20, 1) == 0);
    assert(mac_table_lookup_vlan(&table, mac, 10, &port) == 1 && port == 0);
    assert(mac_table_lookup_vlan(&table, mac, 20, &port) == 1 && port == 1);
    assert(mac_table_lookup_vlan(&table, mac, 30, &port) == 0);
    vsw_posix(puts("PASS: same MAC remains independent in different VLANs"), "puts");
    assert(mac_table_insert_vlan(&table, mac, 10, 2) == 0);
    assert(mac_table_lookup_vlan(&table, mac, 10, &port) == 1 && port == 2);
    assert(mac_table_lookup_vlan(&table, mac, 20, &port) == 1 && port == 1);
    vsw_posix(puts("PASS: MAC move changes only its own VLAN"), "puts");
    MacRecord records[MAC_TABLE_CAPACITY];
    size_t count = 0;
    assert(mac_table_snapshot(&table, records, MAC_TABLE_CAPACITY, &count) == 0);
    assert(count == 2);
    assert((records[0].vlan == 10 && records[1].vlan == 20) ||
           (records[0].vlan == 20 && records[1].vlan == 10));
    assert(mac_table_insert_vlan(&table, mac, 0, 0) == -1);
    assert(mac_table_lookup_vlan(&table, mac, 4095, &port) == -1);
    assert(mac_table_remove_port(&table, 2) == 1);
    assert(mac_table_lookup_vlan(&table, mac, 10, &port) == 0);
    assert(mac_table_lookup_vlan(&table, mac, 20, &port) == 1);
    assert(mac_table_insert(&table, mac, 0) == 0);
    assert(mac_table_lookup(&table, mac, &port) == 1 && port == 0);
    assert(mac_table_clear(&table) == 0);
    assert(mac_table_destroy(&table) == 0);
    vsw_posix(puts("PASS: snapshot, validation, removal, legacy API, and clear"), "puts");
}
