#define _POSIX_C_SOURCE 200809L
#include "checked.h"
#define main switch_program_main
#include "../src/switch.c"
#undef main
#include <assert.h>

static void permits(sem_t *semaphore, int expected)
{
    int actual;
    assert(sem_getvalue(semaphore, &actual) == 0);
    assert(actual == expected);
}
int main(void)
{
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    MacTable table;
    assert(mac_table_init(&table) == 0);
    SharedStats memory = {.num_ports = 2};
    assert(pthread_mutex_init(&memory.mutex, NULL) == 0);
    StatsHandle stats = {.shared = &memory};
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t admission = PTHREAD_MUTEX_INITIALIZER;
    sem_t free_ports;
    assert(sem_init(&free_ports, 0, 2) == 0);
    Port ports[2] = {0};
    pthread_t sessions[2];
    int started[2] = {0};
    for (int i=0;i<2;i++) {
        ports[i].fd=-1; ports[i].ports=ports; ports[i].num_ports=2;
        ports[i].vlan=1; ports[i].stats=&stats;
        ports[i].mac_table=&table; ports[i].ports_lock=&lock;
        ports[i].free_ports=&free_ports;
        atomic_init(&ports[i].dropped,0);
        assert(pthread_mutex_init(&ports[i].transmit_lock,NULL)==0);
    }
    AdminContext admin = {.ports=ports,.num_ports=2,.ports_lock=&lock,
        .mac_table=&table,.free_ports=&free_ports,.admission_lock=&admission,
        .sessions=sessions,.session_started=started};
    char response[4096];
    assert(handle_admin("shutdown 1",response,sizeof(response),&admin)==0);
    permits(&free_ports,1);
    assert(handle_admin("shutdown 1",response,sizeof(response),&admin)==0);
    permits(&free_ports,1);
    assert(handle_admin("up 1",response,sizeof(response),&admin)==0);
    assert(handle_admin("up 1",response,sizeof(response),&admin)==0);
    permits(&free_ports,2);
    vsw_posix(puts("PASS: idle shutdown/up are idempotent and preserve semaphore capacity"), "puts");

    atomic_int stopping;
    atomic_init(&stopping,0);
    for (int round=0;round<20;round++) {
        int pair[2];
        assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
        assert(admit_client(&admin,pair[0],&stopping)==0);
        assert(started[0] && ports[0].active);
        permits(&free_ports,1);
        const uint8_t mac[6]={2,0,0,0,0,1};
        assert(mac_table_insert_vlan(&table,mac,1,0)==0);
        /* Leave receiver blocked partway through the wire header. */
        assert(write(pair[1],"x",1)==1);
        assert(handle_admin("shutdown 1",response,sizeof(response),&admin)==0);
        assert(ports[0].fd==-1 && ports[0].disabled && !started[0]);
        permits(&free_ports,1);
        int destination;
        assert(mac_table_lookup_vlan(&table,mac,1,&destination)==0);
        vsw_posix(close(pair[1]), "close");
        assert(handle_admin("up 1",response,sizeof(response),&admin)==0);
        permits(&free_ports,2);
    }
    vsw_posix(puts("PASS: 20 active shutdown/reopen cycles join sessions and remove MACs"), "puts");
    assert(handle_admin("shutdown 0",response,sizeof(response),&admin)==-1);
    assert(handle_admin("up 3",response,sizeof(response),&admin)==-1);
    assert(handle_admin("up 1 trailing",response,sizeof(response),&admin)==-1);
    vsw_posix(puts("PASS: invalid lifecycle commands rejected"), "puts");
    for(int i=0;i<2;i++) assert(pthread_mutex_destroy(&ports[i].transmit_lock)==0);
    assert(sem_destroy(&free_ports)==0);
    assert(pthread_mutex_destroy(&admission)==0);
    assert(pthread_mutex_destroy(&lock)==0);
    assert(pthread_mutex_destroy(&memory.mutex)==0);
    assert(mac_table_destroy(&table)==0);
}
