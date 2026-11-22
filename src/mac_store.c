#define _POSIX_C_SOURCE 200809L
#include "checked.h"
#include "mac_store.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORE_MAGIC 0x4d414353u
#define STORE_VERSION 1u

typedef struct {
    atomic_uint ready;
    uint32_t version;
    uint32_t image_size;
    pthread_mutex_t mutex;
    atomic_uint committed;
    MacStoreImage images[2];
} SharedMacStore;

static int valid_name(const char *name)
{
    return name != NULL && name[0]=='/' && name[1]!='\0' &&
           strchr(name+1,'/')==NULL && strlen(name)<256;
}
static int valid_entries(const MacStoreEntry *entries, size_t count, uint64_t sequence)
{
    if (count>MAC_STORE_CAPACITY || (count && entries==NULL))
        return 0;
    for (size_t i=0;i<count;i++) {
        const MacStoreEntry *entry=&entries[i];
        if (entry->vlan<1 || entry->vlan>4094 || entry->port<0 ||
            (uint32_t)entry->port>=MAC_STORE_MAX_PORTS || entry->last_seen_seconds<0 ||
            entry->last_seen_nanoseconds<0 || entry->last_seen_nanoseconds>=1000000000 ||
            entry->last_used>sequence)
            return 0;
        for (size_t j=0;j<i;j++)
            if (entry->vlan==entries[j].vlan &&
                memcmp(entry->mac,entries[j].mac,FRAME_MAC_SIZE)==0)
                return 0;
    }
    return 1;
}
static int valid_committed(SharedMacStore *shared)
{
    unsigned int slot=atomic_load_explicit(&shared->committed,memory_order_acquire);
    return slot<2 && valid_entries(shared->images[slot].entries,
        shared->images[slot].count,shared->images[slot].use_sequence);
}
static int lock_store(SharedMacStore *shared)
{
    int error=pthread_mutex_lock(&shared->mutex);
    if (error==EOWNERDEAD) {
        /* Only the inactive image can have an interrupted copy. */
        if (!valid_committed(shared)) {
            /* Unlock without consistent makes an unrecoverable object fail closed. */
            vsw_pthread(pthread_mutex_unlock(&shared->mutex), "pthread_mutex_unlock");
            errno=EPROTO;
            return -1;
        }
        error=pthread_mutex_consistent(&shared->mutex);
        if (error!=0) {
            vsw_pthread(pthread_mutex_unlock(&shared->mutex), "pthread_mutex_unlock");
            errno=error;
            return -1;
        }
    }
    if (error!=0) {errno=error;return -1;}
    if (!valid_committed(shared)) {
        vsw_pthread(pthread_mutex_unlock(&shared->mutex), "pthread_mutex_unlock");
        errno=EPROTO;
        return -1;
    }
    return 0;
}
static int unlock_store(SharedMacStore *shared)
{
    int error=pthread_mutex_unlock(&shared->mutex);
    if (error!=0) {errno=error;return -1;}
    return 0;
}
static SharedMacStore *get_shared(MacStore *store)
{
    if (store==NULL || store->mapping==NULL || store->mapping_size!=sizeof(SharedMacStore)) {
        errno=EINVAL;
        return NULL;
    }
    return store->mapping;
}
int mac_store_close(MacStore *store)
{
    if (store==NULL) {errno=EINVAL;return -1;}
    int saved=0;
    if (store->mapping!=NULL && munmap(store->mapping,store->mapping_size)==-1)
        saved=errno;
    if (store->fd!=-1 && close(store->fd)==-1 && saved==0)
        saved=errno;
    *store=(MacStore){.fd=-1};
    if (saved) {errno=saved;return -1;}
    return 0;
}
int mac_store_unlink(const char *name)
{
    if (!valid_name(name)) {errno=EINVAL;return -1;}
    return shm_unlink(name);
}
static int map_store(MacStore *store)
{
    void *memory=mmap(NULL,sizeof(SharedMacStore),PROT_READ|PROT_WRITE,
                      MAP_SHARED,store->fd,0);
    if (memory==MAP_FAILED)
        return -1;
    store->mapping=memory;
    store->mapping_size=sizeof(SharedMacStore);
    return 0;
}
int mac_store_create(MacStore *store, const char *name)
{
    if (store==NULL || !valid_name(name)) {errno=EINVAL;return -1;}
    *store=(MacStore){.fd=-1};
    store->fd=shm_open(name,O_CREAT|O_EXCL|O_RDWR,0600);
    if (store->fd==-1)
        return -1;
    if (ftruncate(store->fd,(off_t)sizeof(SharedMacStore))==-1 || map_store(store)==-1)
        goto fail;
    SharedMacStore *shared=store->mapping;
    memset(shared,0,sizeof(*shared));
    atomic_init(&shared->ready,0);
    atomic_init(&shared->committed,0);
    if (!atomic_is_lock_free(&shared->committed) || !atomic_is_lock_free(&shared->ready)) {
        errno=ENOTSUP;
        goto fail;
    }
    pthread_mutexattr_t attributes;
    int error=pthread_mutexattr_init(&attributes);
    if (error!=0) {errno=error;goto fail;}
    error=pthread_mutexattr_setpshared(&attributes,PTHREAD_PROCESS_SHARED);
    if (error==0)
        error=pthread_mutexattr_setrobust(&attributes,PTHREAD_MUTEX_ROBUST);
    if (error==0)
        error=pthread_mutex_init(&shared->mutex,&attributes);
    int attr_error=pthread_mutexattr_destroy(&attributes);
    if (error!=0 || attr_error!=0) {errno=error ? error : attr_error;goto fail;}
    shared->version=STORE_VERSION;
    shared->image_size=(uint32_t)sizeof(MacStoreImage);
    atomic_store_explicit(&shared->ready,STORE_MAGIC,memory_order_release);
    return 0;
fail:
    {
        int saved=errno;
        vsw_zero(mac_store_close(store), "mac_store_close");
        vsw_posix(shm_unlink(name), "shm_unlink");
        errno=saved;
        return -1;
    }
}
int mac_store_attach(MacStore *store, const char *name)
{
    if (store==NULL || !valid_name(name)) {errno=EINVAL;return -1;}
    *store=(MacStore){.fd=-1};
    store->fd=shm_open(name,O_RDWR,0);
    if (store->fd==-1)
        return -1;
    struct stat info;
    if (fstat(store->fd,&info)==-1)
        goto fail;
    if (info.st_size!=(off_t)sizeof(SharedMacStore)) {errno=EPROTO;goto fail;}
    if (map_store(store)==-1)
        goto fail;
    SharedMacStore *shared=store->mapping;
    if (!atomic_is_lock_free(&shared->committed) || !atomic_is_lock_free(&shared->ready)) {
        errno=ENOTSUP;
        goto fail;
    }
    if (atomic_load_explicit(&shared->ready,memory_order_acquire)!=STORE_MAGIC) {
        errno=EAGAIN;
        goto fail;
    }
    if (shared->version!=STORE_VERSION || shared->image_size!=sizeof(MacStoreImage)) {
        errno=EPROTO;
        goto fail;
    }
    if (lock_store(shared)==-1 || unlock_store(shared)==-1)
        goto fail;
    return 0;
fail:
    {
        int saved=errno;
        vsw_zero(mac_store_close(store), "mac_store_close");
        errno=saved;
        return -1;
    }
}
int mac_store_publish(MacStore *store, const MacStoreEntry *entries,
                      size_t count, uint64_t use_sequence)
{
    SharedMacStore *shared=get_shared(store);
    if (shared==NULL)
        return -1;
    if (!valid_entries(entries,count,use_sequence)) {errno=EINVAL;return -1;}
    /* Callers must not mutate their source entries during this call. */
    MacStoreImage next={.use_sequence=use_sequence,.count=(uint32_t)count};
    if (count)
        memcpy(next.entries,entries,count*sizeof(*entries));
    if (lock_store(shared)==-1)
        return -1;
    unsigned int current=atomic_load_explicit(&shared->committed,memory_order_relaxed);
    if (shared->images[current].generation==UINT64_MAX) {
        unlock_store(shared);
        errno=EOVERFLOW;
        return -1;
    }
    next.generation=shared->images[current].generation+1;
    unsigned int target=1-current;
    shared->images[target]=next;
    /* Commit only after the complete inactive image has been written. */
    atomic_store_explicit(&shared->committed,target,memory_order_release);
    return unlock_store(shared);
}
int mac_store_read(MacStore *store, MacStoreImage *image)
{
    SharedMacStore *shared=get_shared(store);
    if (shared==NULL)
        return -1;
    if (image==NULL) {errno=EINVAL;return -1;}
    if (lock_store(shared)==-1)
        return -1;
    unsigned int current=atomic_load_explicit(&shared->committed,memory_order_acquire);
    *image=shared->images[current];
    return unlock_store(shared);
}
#ifdef MAC_STORE_TESTING
int mac_store_test_hold(MacStore *store, int commit_first, int notify_fd)
{
    SharedMacStore *shared=get_shared(store);
    if (shared==NULL || lock_store(shared)==-1)
        return -1;
    unsigned int current=atomic_load_explicit(&shared->committed,memory_order_relaxed);
    unsigned int target=1-current;
    if (commit_first) {
        shared->images[target]=shared->images[current];
        shared->images[target].generation++;
        if (shared->images[target].count)
            shared->images[target].entries[0].port=3;
        atomic_store_explicit(&shared->committed,target,memory_order_release);
    } else {
        shared->images[target].count=MAC_STORE_CAPACITY+1;
        shared->images[target].entries[0].vlan=0;
    }
    if (write(notify_fd,"L",1)!=1) {
        unlock_store(shared);
        return -1;
    }
    for (;;) pause();
}
#endif
