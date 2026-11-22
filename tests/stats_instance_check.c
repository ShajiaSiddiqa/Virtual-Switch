#define _POSIX_C_SOURCE 200809L
#include "shm_stats.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    char a[128],b[128];
    snprintf(a,sizeof(a),"/vswitch_instance_%ld_a",(long)getpid());
    snprintf(b,sizeof(b),"/vswitch_instance_%ld_b",(long)getpid());
    StatsHandle owner_a,owner_b,reader;
    assert(setenv("VSWITCH_STATS",a,1)==0);
    assert(shm_stats_create(&owner_a,2)==0);
    assert(setenv("VSWITCH_STATS",b,1)==0);
    assert(shm_stats_create(&owner_b,3)==0);
    assert(shm_stats_attach(&reader)==0);
    assert(shm_stats_add(&reader,0,STATS_FRAMES_IN)==0);
    assert(shm_stats_close(&reader)==0);
    PortStats counters[3];size_t count;
    assert(shm_stats_snapshot(&owner_a,counters,3,&count)==0 && count==2);
    assert(counters[0].frames_in==0);
    assert(shm_stats_snapshot(&owner_b,counters,3,&count)==0 && count==3);
    assert(counters[0].frames_in==1);
    /* Environment currently names B; closing A must still unlink A. */
    assert(shm_stats_close(&owner_a)==0);
    assert(setenv("VSWITCH_STATS",a,1)==0);
    assert(shm_stats_attach(&reader)==-1 && errno==ENOENT);
    assert(setenv("VSWITCH_STATS",b,1)==0);
    assert(shm_stats_attach(&reader)==0);
    assert(shm_stats_close(&reader)==0);
    assert(shm_stats_close(&owner_b)==0);
    assert(shm_stats_attach(&reader)==-1 && errno==ENOENT);
    puts("PASS: independent statistics names, correct attachment and owner-specific cleanup");
    return 0;
}
