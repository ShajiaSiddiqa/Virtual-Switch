#define _POSIX_C_SOURCE 200809L
#include "mac_store.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAC_STORE_TESTING
#error Compile this test and mac_store.c with -DMAC_STORE_TESTING
#endif

static void exited(pid_t child)
{
    int status;
    assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
}
static void crash_owner(MacStore *store, const char *name, int commit_first)
{
    int notification[2];
    assert(pipe(notification)==0);
    pid_t child=fork();
    assert(child>=0);
    if (child==0) {
        close(notification[0]);
        assert(mac_store_close(store)==0);
        MacStore writer;
        assert(mac_store_attach(&writer,name)==0);
        /* Remains inside this hook with the mutex locked until parent kills it. */
        mac_store_test_hold(&writer,commit_first,notification[1]);
        _exit(99);
    }
    close(notification[1]);
    char byte;
    assert(read(notification[0],&byte,1)==1 && byte=='L');
    close(notification[0]);
    assert(kill(child,SIGKILL)==0);
    int status;
    assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
}
static void concurrent_publishers(MacStore *store, const char *name)
{
    MacStoreImage before;
    assert(mac_store_read(store,&before)==0);
    pid_t writers[2];
    for (int i=0;i<2;i++) {
        writers[i]=fork();assert(writers[i]>=0);
        if (writers[i]==0) {
            assert(mac_store_close(store)==0);
            MacStore own;
            assert(mac_store_attach(&own,name)==0);
            MacStoreEntry pair[2]={
                {.mac={2,0,0,0,0,10},.vlan=10,.port=i,.last_seen_seconds=123,.last_used=7},
                {.mac={2,0,0,0,0,10},.vlan=20,.port=i,.last_seen_seconds=123,.last_used=7}
            };
            for (int j=0;j<100;j++) assert(mac_store_publish(&own,pair,2,7)==0);
            assert(mac_store_close(&own)==0);
            _exit(0);
        }
    }
    uint64_t previous=before.generation;
    for (int i=0;i<100;i++) {
        MacStoreImage image;
        assert(mac_store_read(store,&image)==0);
        assert(image.generation>=previous);
        previous=image.generation;
        if (image.generation>before.generation) {
            assert(image.count==2);
            assert(image.entries[0].port==image.entries[1].port);
            assert(image.entries[0].vlan==10 && image.entries[1].vlan==20);
        }
    }
    for (int i=0;i<2;i++) exited(writers[i]);
    MacStoreImage after;
    assert(mac_store_read(store,&after)==0);
    assert(after.generation==before.generation+200);
    assert(after.count==2 && after.entries[0].port==after.entries[1].port);
}
int main(int argc, char *argv[])
{
    if (argc==3 && strcmp(argv[1],"--attach-check")==0) {
        MacStore fresh;
        MacStoreImage recovered;
        assert(mac_store_attach(&fresh,argv[2])==0);
        assert(mac_store_read(&fresh,&recovered)==0);
        assert(recovered.generation==1 && recovered.count==2 && recovered.use_sequence==9);
        assert(recovered.entries[0].port==1 && recovered.entries[0].vlan==10);
        assert(recovered.entries[1].port==2 && recovered.entries[1].vlan==20);
        assert(mac_store_close(&fresh)==0);
        return 0;
    }
    assert(argc==1);
    assert(setvbuf(stdout,NULL,_IONBF,0)==0);
    char name[128];
    snprintf(name,sizeof(name),"/vswitch_mac_store_check_%ld",(long)getpid());
    MacStore store;
    assert(mac_store_create(&store,name)==0);
    MacStore duplicate;
    assert(mac_store_create(&duplicate,name)==-1 && errno==EEXIST);
    MacStoreImage image;
    assert(mac_store_read(&store,&image)==0 && image.count==0 && image.generation==0);
    MacStoreEntry entries[2]={
        {.mac={2,0,0,0,0,1},.vlan=10,.port=1,.last_seen_seconds=123,
         .last_seen_nanoseconds=456,.last_used=8},
        {.mac={2,0,0,0,0,1},.vlan=20,.port=2,.last_seen_seconds=125,
         .last_seen_nanoseconds=789,.last_used=9}
    };
    assert(mac_store_publish(&store,entries,2,9)==0);
    assert(mac_store_close(&store)==0);
    assert(mac_store_attach(&store,name)==0);
    assert(mac_store_read(&store,&image)==0);
    assert(image.count==2 && image.generation==1 && image.use_sequence==9);
    assert(image.entries[0].vlan==10 && image.entries[1].vlan==20);
    assert(image.entries[0].last_seen_nanoseconds==456 && image.entries[1].last_used==9);
    pid_t fresh=fork();
    assert(fresh>=0);
    if (fresh==0) {
        assert(mac_store_close(&store)==0);
        execl(argv[0],argv[0],"--attach-check",name,(char *)NULL);
        _exit(98);
    }
    exited(fresh);
    puts("PASS: close/reattach and fresh exec preserve MAC, VLAN, port, aging time and LRU state");

    entries[1].vlan=10;
    assert(mac_store_publish(&store,entries,2,9)==-1 && errno==EINVAL);
    entries[1].vlan=20;
    entries[1].port=64;
    assert(mac_store_publish(&store,entries,2,9)==-1 && errno==EINVAL);
    entries[1].port=2;
    assert(mac_store_publish(&store,entries,2,8)==-1 && errno==EINVAL);
    assert(mac_store_publish(&store,NULL,1,9)==-1 && errno==EINVAL);
    assert(mac_store_publish(&store,entries,MAC_STORE_CAPACITY+1,9)==-1 && errno==EINVAL);
    assert(mac_store_read(&store,&image)==0 && image.generation==1);
    puts("PASS: invalid, duplicate and oversized images rejected without changing committed data");

    crash_owner(&store,name,0);
    assert(mac_store_close(&store)==0);
    assert(mac_store_attach(&store,name)==0);
    assert(mac_store_read(&store,&image)==0);
    assert(image.generation==1 && image.count==2 && image.entries[0].port==1);
    puts("PASS: owner killed during inactive copy leaves previous committed image intact");

    crash_owner(&store,name,1);
    assert(mac_store_read(&store,&image)==0);
    assert(image.generation==2 && image.entries[0].port==3);
    assert(mac_store_publish(&store,entries,2,9)==0);
    puts("PASS: owner killed after commit preserves new image and mutex remains usable");

    concurrent_publishers(&store,name);
    puts("PASS: two publishers commit 200 complete images with monotonic generations");
    assert(mac_store_publish(&store,NULL,0,0)==0);
    assert(mac_store_read(&store,&image)==0 && image.count==0);
    assert(mac_store_close(&store)==0);
    assert(mac_store_attach(&store,name)==0);
    assert(mac_store_read(&store,&image)==0 && image.count==0);
    assert(mac_store_close(&store)==0);
    assert(mac_store_unlink(name)==0);
    assert(mac_store_attach(&store,name)==-1 && errno==ENOENT);
    puts("PASS: empty image persists; explicit owner unlink removes the object");
    return 0;
}
