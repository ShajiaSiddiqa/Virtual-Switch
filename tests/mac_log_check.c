#define _POSIX_C_SOURCE 200809L
#include "mac_table.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { MacTable *table; unsigned int count; } Context;
static void moved(void *arg, const uint8_t mac[6], uint16_t vlan, int old, int new)
{
    Context *context=arg;
    assert(mac[0]==2 && vlan==10 && old==0 && new==1);
    /* Callback must execute outside all MAC locks. */
    assert(pthread_mutex_trylock(&context->table->capacity_lock)==0);
    for(size_t i=0;i<MAC_TABLE_BUCKETS;i++) {
        assert(pthread_mutex_trylock(&context->table->buckets[i].lock)==0);
        assert(pthread_mutex_unlock(&context->table->buckets[i].lock)==0);
    }
    assert(pthread_mutex_unlock(&context->table->capacity_lock)==0);
    context->count++;
}
int main(void)
{
    MacTable table;
    assert(mac_table_init(&table)==0);
    Context context={.table=&table};
    mac_table_set_move_logger(&table,moved,&context);
    const uint8_t mac[6]={2,0,0,0,0,1};
    assert(mac_table_insert_vlan(&table,mac,10,0)==0);
    assert(context.count==0);
    assert(mac_table_insert_vlan(&table,mac,10,0)==0);
    assert(context.count==0);
    assert(mac_table_insert_vlan(&table,mac,10,1)==0);
    assert(context.count==1);
    assert(mac_table_destroy(&table)==0);
    puts("PASS: MAC move callback receives VLAN/ports and holds no MAC locks");
}
