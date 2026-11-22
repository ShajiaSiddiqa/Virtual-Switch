#define _POSIX_C_SOURCE 200809L
#include "pipe_link.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static FrameHeader sample(void)
{
    FrameHeader header = {.length=FRAME_MAX_PAYLOAD,.vlan=37,.priority=2,
        .src={2,0,0,0,0,1}, .dst={2,0,0,0,0,2}};
    return header;
}
static void wait_child(pid_t child)
{
    int status;
    assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
}
static void bidirectional(void)
{
    PipeLink links[2];
    assert(pipe_link_pair(links)==0);
    uint8_t payload[FRAME_MAX_PAYLOAD];
    memset(payload,0xa5,sizeof(payload));
    pid_t child=fork();
    assert(child>=0);
    if(child==0) {
        assert(pipe_link_destroy(&links[0])==0);
        FrameHeader received;
        uint8_t data[FRAME_MAX_PAYLOAD];
        assert(pipe_link_receive(&links[1],&received,data)==1);
        assert(received.length==FRAME_MAX_PAYLOAD && received.vlan==37 && received.priority==2);
        assert(memcmp(data,payload,sizeof(data))==0);
        uint8_t wire[FRAME_HEADER_SIZE];
        assert(frame_serialize(&received,wire,sizeof(wire))==0);
        assert(write(links[1].write_fd,wire,5)==5);
        assert(write(links[1].write_fd,wire+5,sizeof(wire)-5)==(ssize_t)(sizeof(wire)-5));
        assert(write(links[1].write_fd,data,sizeof(data))==(ssize_t)sizeof(data));
        received.length=0;
        assert(pipe_link_send(&links[1],&received,NULL)==0);
        assert(pipe_link_destroy(&links[1])==0);
        _exit(0);
    }
    assert(pipe_link_destroy(&links[1])==0);
    FrameHeader sent=sample(), received;
    uint8_t data[FRAME_MAX_PAYLOAD];
    assert(pipe_link_send(&links[0],&sent,payload)==0);
    assert(pipe_link_receive(&links[0],&received,data)==1);
    assert(received.vlan==37 && received.priority==2 && received.length==sizeof(payload));
    assert(memcmp(received.src,sent.src,6)==0 && memcmp(received.dst,sent.dst,6)==0);
    assert(memcmp(data,payload,sizeof(data))==0);
    assert(pipe_link_receive(&links[0],&received,data)==1 && received.length==0);
    assert(pipe_link_receive(&links[0],&received,data)==0);
    assert(pipe_link_destroy(&links[0])==0);
    wait_child(child);
    puts("PASS: two-process bidirectional frames, fragmented header, VLAN/priority and clean EOF");
}
static void partial_eof(void)
{
    PipeLink links[2];
    assert(pipe_link_pair(links)==0);
    pid_t child=fork();
    assert(child>=0);
    if(child==0) {
        assert(pipe_link_destroy(&links[0])==0);
        assert(write(links[1].write_fd,"short",5)==5);
        assert(pipe_link_destroy(&links[1])==0);
        _exit(0);
    }
    assert(pipe_link_destroy(&links[1])==0);
    FrameHeader header;
    uint8_t data[FRAME_MAX_PAYLOAD];
    assert(pipe_link_receive(&links[0],&header,data)==-1 && errno==EPROTO);
    assert(pipe_link_destroy(&links[0])==0);
    wait_child(child);
    puts("PASS: peer closing mid-header reports protocol error");
}
static void truncated_payload_and_broken_pipe(void)
{
    PipeLink links[2];
    FrameHeader header=sample(), received;
    uint8_t wire[FRAME_HEADER_SIZE], payload[FRAME_MAX_PAYLOAD]={0};
    assert(pipe_link_pair(links)==0);
    assert(frame_serialize(&header,wire,sizeof(wire))==0);
    assert(write(links[1].write_fd,wire,sizeof(wire))==(ssize_t)sizeof(wire));
    assert(write(links[1].write_fd,"abc",3)==3);
    assert(pipe_link_destroy(&links[1])==0);
    assert(pipe_link_receive(&links[0],&received,payload)==-1 && errno==EPROTO);
    assert(pipe_link_destroy(&links[0])==0);

    assert(pipe_link_pair(links)==0);
    assert(pipe_link_destroy(&links[1])==0);
    assert(pipe_link_send(&links[0],&header,payload)==-1 && errno==EPIPE);
    assert(pipe_link_destroy(&links[0])==0);
    puts("PASS: truncated payload rejected and broken pipe handled without SIGPIPE termination");
}
static void adoption(void)
{
    int ab[2],ba[2];
    assert(pipe(ab)==0 && pipe(ba)==0);
    PipeLink a,b;
    assert(pipe_link_adopt(&a,ab[0],ab[1])==-1 && errno==EINVAL);
    assert(pipe_link_adopt(&a,ba[0],ab[1])==0);
    assert(pipe_link_adopt(&b,ab[0],ba[1])==0);
    FrameHeader sent=sample(),received;
    uint8_t payload[FRAME_MAX_PAYLOAD]={0},actual[FRAME_MAX_PAYLOAD];
    assert(pipe_link_send(&a,&sent,payload)==0);
    assert(pipe_link_receive(&b,&received,actual)==1);
    assert(received.vlan==sent.vlan && received.length==sent.length);
    assert(memcmp(payload,actual,sizeof(payload))==0);
    assert(pipe_link_destroy(&a)==0 && pipe_link_destroy(&b)==0);
    puts("PASS: inherited descriptors adopted and a single-pipe loopback rejected");
}
static void *receive_until_stop(void *arg)
{
    FrameHeader header;
    uint8_t data[FRAME_MAX_PAYLOAD];
    assert(pipe_link_receive(arg,&header,data)==-1 && errno==ECANCELED);
    return NULL;
}
static void *send_until_stop(void *arg)
{
    FrameHeader header=sample();
    uint8_t payload[FRAME_MAX_PAYLOAD]={0};
    assert(pipe_link_send(arg,&header,payload)==-1 && errno==ECANCELED);
    return NULL;
}
static void cancellation(void)
{
    PipeLink links[2];
    pthread_t reader;
    assert(pipe_link_pair(links)==0);
    assert(pthread_create(&reader,NULL,receive_until_stop,&links[0])==0);
    assert(pipe_link_stop(&links[0])==0);
    assert(pipe_link_stop(&links[0])==0);
    assert(pthread_join(reader,NULL)==0);
    assert(pipe_link_destroy(&links[0])==0);
    assert(pipe_link_destroy(&links[1])==0);
    puts("PASS: stop wakes receiver and is idempotent");

    assert(pipe_link_pair(links)==0);
    uint8_t filler[4096]={0};
    for(;;) {
        ssize_t count=write(links[0].write_fd,filler,sizeof(filler));
        if(count>0) continue;
        assert(count==-1 && (errno==EAGAIN || errno==EWOULDBLOCK));
        break;
    }
    pthread_t writer;
    assert(pthread_create(&writer,NULL,send_until_stop,&links[0])==0);
    assert(pipe_link_stop(&links[0])==0);
    assert(pthread_join(writer,NULL)==0);
    assert(pipe_link_destroy(&links[0])==0);
    assert(pipe_link_destroy(&links[1])==0);
    puts("PASS: stop terminates sender when peer does not drain a full pipe");
}
int main(void)
{
    assert(setvbuf(stdout,NULL,_IONBF,0)==0);
    assert(signal(SIGPIPE,SIG_IGN)!=SIG_ERR);
    bidirectional();
    partial_eof();
    truncated_payload_and_broken_pipe();
    adoption();
    cancellation();
    return 0;
}
