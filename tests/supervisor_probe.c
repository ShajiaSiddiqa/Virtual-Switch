#define _POSIX_C_SOURCE 200809L
#include "checked.h"
#include "mac_store.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    assert(argc==2);
    int loop=strcmp(argv[1],"crash-loop")==0;
    int ignore=strcmp(argv[1],"ignore-stop")==0;
    int clean=strcmp(argv[1],"clean-exit")==0;
    sigset_t signals;
    vsw_posix(sigemptyset(&signals), "sigemptyset");vsw_posix(sigaddset(&signals,SIGTERM), "sigaddset");
    vsw_posix(sigaddset(&signals,SIGHUP), "sigaddset");vsw_posix(sigaddset(&signals,SIGUSR1), "sigaddset");
    if (!ignore) vsw_posix(sigaddset(&signals,SIGINT), "sigaddset");
    else assert(signal(SIGINT,SIG_IGN)!=SIG_ERR);
    assert(pthread_sigmask(SIG_BLOCK,&signals,NULL)==0);
    const char *name=getenv("VSWITCH_MAC_STORE");
    const char *report=getenv("SUPERVISOR_PROBE_LOG");
    assert(name && report);
    MacStore store;
    assert(mac_store_attach(&store,name)==0);
    MacStoreImage image;
    assert(mac_store_read(&store,&image)==0);
    const char *restart_text=getenv("VSWITCH_RESTART_COUNT");
    assert(restart_text);
    if (atoi(restart_text)>0) assert(image.count==1 && image.generation==1);
    if (image.count==0) {
        MacStoreEntry entry={.mac={2,0,0,0,0,1},.vlan=10,.port=1,
            .last_seen_seconds=123,.last_seen_nanoseconds=456,.last_used=8};
        assert(mac_store_publish(&store,&entry,1,8)==0);
        assert(mac_store_read(&store,&image)==0);
    }
    assert(image.count==1 && image.entries[0].mac[5]==1 && image.entries[0].vlan==10);
    assert(image.entries[0].port==1 && image.entries[0].last_seen_seconds==123);
    assert(image.entries[0].last_seen_nanoseconds==456 && image.entries[0].last_used==8);
    /* Deliberately leave disposable worker IPC for supervisor cleanup. */
    int socket_marker=open(getenv("VSWITCH_SOCKET"),O_CREAT|O_EXCL|O_WRONLY,0600);
    assert(socket_marker>=0);assert(close(socket_marker)==0);
    int stats_marker=shm_open(getenv("VSWITCH_STATS"),O_CREAT|O_EXCL|O_RDWR,0600);
    assert(stats_marker>=0);assert(close(stats_marker)==0);
    FILE *file=fopen(report,"a");
    assert(file);
    assert(fprintf(file,"%ld %llu %u %s\n",(long)getpid(),
        (unsigned long long)image.generation,image.count,name)>0);
    assert(fclose(file)==0);
    if (loop || clean) {assert(mac_store_close(&store)==0);return loop ? 7 : 0;}
    int received;
    for (;;) {
        assert(sigwait(&signals,&received)==0);
        if (received==SIGINT || received==SIGTERM) break;
        FILE *notice=fopen(report,"a");
        assert(notice);
        assert(fprintf(notice,"SIGNAL %ld %d\n",(long)getpid(),received)>0);
        assert(fclose(notice)==0);
    }
    assert(mac_store_close(&store)==0);
    return 0;
}
