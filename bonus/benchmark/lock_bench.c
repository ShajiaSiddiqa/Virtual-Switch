#define _POSIX_C_SOURCE 200809L
#include "../../inc/checked.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ENTRY_COUNT 256u
#define BUCKET_COUNT 16u
#define MAX_THREADS 64u

typedef struct {
    uint8_t mac[6];
    uint16_t vlan;
    unsigned int port;
    uint64_t updates;
    int next;
} Entry;

typedef enum { GLOBAL, STRIPED, RWLOCK } Mode;
typedef struct {
    Entry entries[ENTRY_COUNT];
    int heads[BUCKET_COUNT];
    pthread_mutex_t global;
    pthread_mutex_t stripes[BUCKET_COUNT];
    pthread_rwlock_t rwlock;
    Mode mode;
} Table;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready_changed;
    pthread_cond_t start;
    unsigned int ready;
    int go;
} Gate;

typedef struct {
    Table *table;
    Gate *gate;
    uint64_t operations, random, writes, checksum, finished;
    unsigned int read_percent, hot_percent;
} Worker;

static void check(int error)
{
    if (error != 0) {
        vsw_posix(fprintf(stderr, "pthread: %s\n", strerror(error)), "fprintf");
        exit(EXIT_FAILURE);
    }
}
static uint64_t now_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
static uint64_t number(const char *text, uint64_t minimum, uint64_t maximum)
{
    if (text[0] < '0' || text[0] > '9') {
        vsw_posix(fprintf(stderr, "Invalid unsigned number: %s\n", text), "fprintf");
        exit(EXIT_FAILURE);
    }
    char *end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end != '\0' || value < minimum || value > maximum) {
        vsw_posix(fprintf(stderr, "Number out of range: %s\n", text), "fprintf");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value;
}
static uint64_t random_next(uint64_t *state)
{
    /* Each worker has independent state: there is no shared rand() lock. */
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}
static void make_key(unsigned int key, uint8_t mac[6], uint16_t *vlan)
{
    memset(mac, 0, 6);
    mac[0] = 2;
    mac[4] = (uint8_t)(key >> 8);
    mac[5] = (uint8_t)key;
    *vlan = (uint16_t)(1 + key / 64);
}
static unsigned int bucket_for(const uint8_t mac[6], uint16_t vlan)
{
    unsigned int hash = vlan;
    for (unsigned int i = 0; i < 6; i++)
        hash = hash * 31u + mac[i];
    return hash % BUCKET_COUNT;
}
static void table_init(Table *table, Mode mode)
{
    table->mode = mode;
    for (unsigned int i = 0; i < BUCKET_COUNT; i++) {
        table->heads[i] = -1;
        check(pthread_mutex_init(&table->stripes[i], NULL));
    }
    check(pthread_mutex_init(&table->global, NULL));
    check(pthread_rwlock_init(&table->rwlock, NULL));
    for (unsigned int i = 0; i < ENTRY_COUNT; i++) {
        Entry *entry = &table->entries[i];
        make_key(i, entry->mac, &entry->vlan);
        entry->port = i % 64;
        entry->updates = 0;
        unsigned int bucket = bucket_for(entry->mac, entry->vlan);
        entry->next = table->heads[bucket];
        table->heads[bucket] = (int)i;
    }
}
static void acquire(Table *table, unsigned int bucket, int write)
{
    if (table->mode == GLOBAL)
        check(pthread_mutex_lock(&table->global));
    else if (table->mode == STRIPED)
        check(pthread_mutex_lock(&table->stripes[bucket]));
    else if (write)
        check(pthread_rwlock_wrlock(&table->rwlock));
    else
        check(pthread_rwlock_rdlock(&table->rwlock));
}
static void release(Table *table, unsigned int bucket)
{
    if (table->mode == GLOBAL)
        check(pthread_mutex_unlock(&table->global));
    else if (table->mode == STRIPED)
        check(pthread_mutex_unlock(&table->stripes[bucket]));
    else
        check(pthread_rwlock_unlock(&table->rwlock));
}
static void *run_worker(void *arg)
{
    Worker *worker = arg;
    check(pthread_mutex_lock(&worker->gate->mutex));
    worker->gate->ready++;
    check(pthread_cond_signal(&worker->gate->ready_changed));
    while (!worker->gate->go)
        check(pthread_cond_wait(&worker->gate->start, &worker->gate->mutex));
    check(pthread_mutex_unlock(&worker->gate->mutex));

    for (uint64_t i = 0; i < worker->operations; i++) {
        unsigned int key = (unsigned int)(random_next(&worker->random) % ENTRY_COUNT);
        int hot = (random_next(&worker->random) % 100) < worker->hot_percent;
        if (hot)
            key = 0;
        int write = (random_next(&worker->random) % 100) >= worker->read_percent;
        uint8_t mac[6];
        uint16_t vlan;
        make_key(key, mac, &vlan);
        unsigned int bucket = bucket_for(mac, vlan);
        Table *table = worker->table;
        acquire(table, bucket, write);
        Entry *found = NULL;
        for (int link = table->heads[bucket]; link != -1;
             link = table->entries[link].next) {
            Entry *entry = &table->entries[link];
            if (entry->vlan == vlan && memcmp(entry->mac, mac, 6) == 0) {
                found = entry;
                break;
            }
        }
        if (found == NULL) {
            vsw_posix(fprintf(stderr, "Preloaded entry missing\n"), "fprintf");
            abort();
        }
        if (write) {
            found->port = (found->port + 1) % 64;
            found->updates++;
            worker->writes++;
        }
        worker->checksum += found->port;
        release(table, bucket);
    }
    worker->finished = now_ns();
    return NULL;
}
int main(int argc, char **argv)
{
    if (argc != 7) {
        vsw_posix(fprintf(stderr, "Usage: %s global|striped|rwlock threads ops_per_thread read_percent hot_percent seed\n", argv[0]), "fprintf");
        return EXIT_FAILURE;
    }
    Mode mode;
    if (strcmp(argv[1], "global") == 0) mode = GLOBAL;
    else if (strcmp(argv[1], "striped") == 0) mode = STRIPED;
    else if (strcmp(argv[1], "rwlock") == 0) mode = RWLOCK;
    else { vsw_posix(fprintf(stderr, "Unknown lock mode\n"), "fprintf"); return EXIT_FAILURE; }
    unsigned int threads = (unsigned int)number(argv[2], 1, MAX_THREADS);
    uint64_t operations = number(argv[3], 1, UINT64_C(100000000));
    unsigned int reads = (unsigned int)number(argv[4], 0, 100);
    unsigned int hot = (unsigned int)number(argv[5], 0, 100);
    uint64_t seed = number(argv[6], 0, UINT64_MAX);
    Table table;
    table_init(&table, mode);
    Gate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER,
                 .ready_changed = PTHREAD_COND_INITIALIZER,
                 .start = PTHREAD_COND_INITIALIZER};
    Worker workers[MAX_THREADS] = {0};
    pthread_t ids[MAX_THREADS];
    for (unsigned int i = 0; i < threads; i++) {
        workers[i] = (Worker){.table=&table, .gate=&gate, .operations=operations,
            .random=seed + i * UINT64_C(0x9e3779b97f4a7c15),
            .read_percent=reads, .hot_percent=hot};
        check(pthread_create(&ids[i], NULL, run_worker, &workers[i]));
    }
    check(pthread_mutex_lock(&gate.mutex));
    while (gate.ready != threads)
        check(pthread_cond_wait(&gate.ready_changed, &gate.mutex));
    uint64_t start = now_ns();
    gate.go = 1;
    check(pthread_cond_broadcast(&gate.start));
    check(pthread_mutex_unlock(&gate.mutex));
    uint64_t finish = start, writes = 0, checksum = 0;
    for (unsigned int i = 0; i < threads; i++) {
        check(pthread_join(ids[i], NULL));
        if (workers[i].finished > finish) finish = workers[i].finished;
        writes += workers[i].writes;
        checksum += workers[i].checksum;
    }
    uint64_t actual_updates = 0;
    for (unsigned int i = 0; i < ENTRY_COUNT; i++)
        actual_updates += table.entries[i].updates;
    if (writes != actual_updates) {
        vsw_posix(fprintf(stderr, "Lost updates: expected=%" PRIu64 " actual=%" PRIu64 "\n", writes, actual_updates), "fprintf");
        return EXIT_FAILURE;
    }
    double seconds = (double)(finish - start) / 1e9;
    uint64_t total = operations * threads;
    vsw_posix(printf("{\"mode\":\"%s\",\"threads\":%u,\"ops_per_thread\":%" PRIu64
           ",\"read_percent\":%u,\"hot_percent\":%u,\"seed\":%" PRIu64
           ",\"operations\":%" PRIu64 ",\"writes\":%" PRIu64
           ",\"updates\":%" PRIu64 ",\"checksum\":%" PRIu64
           ",\"seconds\":%.9f,\"ops_per_second\":%.3f}\n",
           argv[1],threads,operations,reads,hot,seed,total,writes,actual_updates,
           checksum,seconds,(double)total/seconds), "printf");
    check(pthread_cond_destroy(&gate.start));
    check(pthread_cond_destroy(&gate.ready_changed));
    check(pthread_mutex_destroy(&gate.mutex));
    check(pthread_mutex_destroy(&table.global));
    check(pthread_rwlock_destroy(&table.rwlock));
    for (unsigned int i=0;i<BUCKET_COUNT;i++)
        check(pthread_mutex_destroy(&table.stripes[i]));
    return EXIT_SUCCESS;
}
