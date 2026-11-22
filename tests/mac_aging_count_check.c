#define _POSIX_C_SOURCE 200809L
#include "mac_table.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    MacTable table;
    assert(mac_table_init(&table)==0);
    for(size_t i=0;i<MAC_TABLE_CAPACITY;i++) {
        uint8_t mac[6]={2,0,0,0,(uint8_t)(i>>8),(uint8_t)i};
        assert(mac_table_insert(&table,mac,(int)(i/MAC_TABLE_PORT_LIMIT))==0);
    }
    assert(table.entry_count==MAC_TABLE_CAPACITY);
    /* No concurrent users: make all source timestamps deterministically stale. */
    for(size_t i=0;i<MAC_TABLE_BUCKETS;i++)
        for(MacEntry *entry=table.buckets[i].head;entry;entry=entry->next)
            entry->last_seen=(struct timespec){0};
    assert(mac_table_expire(&table,1)==MAC_TABLE_CAPACITY);
    assert(table.entry_count==0);
    uint8_t replacement[6]={2,1,2,3,4,5};
    assert(mac_table_insert(&table,replacement,0)==0);
    assert(table.entry_count==1);
    assert(mac_table_destroy(&table)==0);
    assert(puts("PASS: aging a full table resets capacity accounting and allows new learning")>=0);
    return 0;
}
