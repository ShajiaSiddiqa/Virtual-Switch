#define _POSIX_C_SOURCE 200809L
#include "checked.h"
#include "mac_table.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int publish(void *context, const MacStoreImage *image)
{
    return mac_store_publish(context,image->entries,image->count,image->use_sequence);
}
static MacStoreEntry *find(MacStoreImage *image, int id)
{
    for(size_t i=0;i<image->count;i++)
        if(image->entries[i].mac[5]==id) return &image->entries[i];
    return NULL;
}
static int learn(MacTable *table,int id,int port)
{
    uint8_t mac[6]={2,0,0,0,0,(uint8_t)id};
    return mac_table_insert(table,mac,port);
}
static int lookup(MacTable *table,int id,int *port)
{
    uint8_t mac[6]={2,0,0,0,0,(uint8_t)id};
    return mac_table_lookup(table,mac,port);
}
typedef struct {MacTable *table;int id;} Worker;
static void *updates(void *argument)
{
    Worker *worker=argument;
    for(int i=0;i<100;i++) {
        assert(learn(worker->table,worker->id,worker->id-10)==0);
        int port;
        assert(lookup(worker->table,worker->id,&port)==1 && port==worker->id-10);
    }
    return NULL;
}
static int reject(void *context,const MacStoreImage *image)
{
    (void)context;(void)image;errno=EIO;return -1;
}
int main(void)
{
    assert(MAC_TABLE_CAPACITY==3);
    char name[128];
    vsw_format(snprintf(name,sizeof(name),"/vswitch_mac_recovery_%ld",(long)getpid()), sizeof(name));
    MacStore store;
    assert(mac_store_create(&store,name)==0);
    MacTable table;
    assert(mac_table_init(&table)==0);
    mac_table_set_checkpoint(&table,publish,&store);
    assert(learn(&table,1,0)==0 && learn(&table,2,1)==0 && learn(&table,3,2)==0);
    MacStoreImage image;
    assert(mac_store_read(&store,&image)==0 && image.count==3);
    int port;
    assert(lookup(&table,1,&port)==1 && port==0);
    assert(learn(&table,4,0)==0);
    assert(mac_store_read(&store,&image)==0);
    assert(find(&image,1) && !find(&image,2) && find(&image,3) && find(&image,4));
    assert(learn(&table,1,2)==0);
    assert(mac_store_read(&store,&image)==0 && find(&image,1)->port==2);
    vsw_posix(puts("PASS: learning, MAC move, lookup LRU and capacity eviction commit synchronously"), "puts");

    MacStoreImage original=image;
    assert(mac_table_destroy(&table)==0);
    assert(mac_table_init(&table)==0);
    assert(mac_table_restore(&table,&image)==0);
    assert(atomic_load(&table.use_sequence)==image.use_sequence);
    for(size_t i=0;i<MAC_TABLE_BUCKETS;i++)
        for(MacEntry *entry=table.buckets[i].head;entry;entry=entry->next) {
            MacStoreEntry *saved=find(&original,entry->mac[5]);
            assert(saved && entry->recovered);
            assert(entry->last_seen.tv_sec==saved->last_seen_seconds);
            assert(entry->last_seen.tv_nsec==saved->last_seen_nanoseconds);
            assert(entry->last_used==saved->last_used);
        }
    assert(lookup(&table,1,&port)==0);
    assert(mac_table_remove_port(&table,2)==0);
    mac_table_set_checkpoint(&table,publish,&store);
    assert(learn(&table,1,1)==0 && lookup(&table,1,&port)==1 && port==1);
    assert(mac_store_read(&store,&image)==0 && find(&image,1)->port==1);
    vsw_posix(puts("PASS: restore keeps timestamps/LRU; recovered hints require fresh source learning"), "puts");

    assert(mac_table_destroy(&table)==0);
    assert(mac_table_init(&table)==0);
    find(&image,3)->last_seen_seconds=0;
    find(&image,3)->last_seen_nanoseconds=0;
    assert(mac_table_restore(&table,&image)==0);
    mac_table_set_checkpoint(&table,publish,&store);
    assert(mac_table_expire(&table,60)==1);
    assert(mac_store_read(&store,&image)==0 && image.count==2 && !find(&image,3));
    assert(learn(&table,1,1)==0);
    assert(mac_table_remove_port(&table,1)==1);
    assert(mac_store_read(&store,&image)==0 && !find(&image,1));
    assert(mac_table_clear(&table)==0);
    assert(mac_store_read(&store,&image)==0 && image.count==0);
    vsw_posix(puts("PASS: aging, validated-port removal and administrative clear persist"), "puts");

    Worker arguments[2]={{.table=&table,.id=10},{.table=&table,.id=11}};
    pthread_t threads[2];
    for(int i=0;i<2;i++) assert(pthread_create(&threads[i],NULL,updates,&arguments[i])==0);
    for(int i=0;i<2;i++) assert(pthread_join(threads[i],NULL)==0);
    assert(mac_store_read(&store,&image)==0 && image.count==2);
    assert(find(&image,10) && find(&image,11));
    assert(image.use_sequence==atomic_load(&table.use_sequence));
    assert(find(&image,10)->last_used<=image.use_sequence);
    assert(find(&image,11)->last_used<=image.use_sequence);
    vsw_posix(puts("PASS: concurrent learning/lookups publish coherent images without stale snapshot overwrite"), "puts");

    assert(learn(&table,12,2)==0);
    mac_table_set_checkpoint(&table,reject,NULL);
    assert(learn(&table,13,3)==-1);
    /* Failed publication after eviction must not free a table-owned replacement. */
    mac_table_set_checkpoint(&table,NULL,NULL);
    assert(mac_table_destroy(&table)==0);
    assert(mac_table_init(&table)==0);
    MacStoreImage invalid={.count=2};
    invalid.entries[0]=(MacStoreEntry){.mac={2,0,0,0,0,1},.vlan=1,.port=0};
    invalid.entries[1]=invalid.entries[0];
    assert(mac_table_restore(&table,&invalid)==-1 && table.entry_count==0);
    assert(mac_table_destroy(&table)==0);
    assert(mac_store_close(&store)==0 && mac_store_unlink(name)==0);
    vsw_posix(puts("PASS: failed commits and invalid restores preserve memory ownership and cleanup"), "puts");
    return 0;
}
