#define _POSIX_C_SOURCE 200809L
#define main switch_program_main
#include "../src/switch.c"
#undef main
#include <assert.h>
#include <sys/wait.h>

static void child_switch(int side, PipeLink links[2], int hosts[4][2],
                          int commands[2][2], int replies[2][2])
{
    assert(pipe_link_destroy(&links[1-side])==0);
    for(int i=0;i<4;i++) {
        close(hosts[i][0]);
        if(i/2!=side) close(hosts[i][1]);
    }
    for(int i=0;i<2;i++) {
        close(commands[i][1]);close(replies[i][0]);
        if(i!=side) {close(commands[i][0]);close(replies[i][1]);}
    }
    MacTable table;
    assert(mac_table_init(&table)==0);
    SharedStats memory={.num_ports=3};
    assert(pthread_mutex_init(&memory.mutex,NULL)==0);
    StatsHandle stats={.fd=-1,.shared=&memory};
    pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    sem_t free_ports;
    assert(sem_init(&free_ports,0,0)==0);
    Port ports[3]={0};
    pthread_t sessions[3];
    for(int i=0;i<3;i++) {
        Port *port=&ports[i];
        port->fd=i==2 ? links[side].read_fd : hosts[side*2+i][1];
        port->pipe_link=i==2 ? &links[side] : NULL;
        port->ports=ports;port->num_ports=3;port->vlan=i==1 ? 20 : 10;
        port->trunk=i==2;port->active=1;port->stats=&stats;
        port->mac_table=&table;port->ports_lock=&lock;port->free_ports=&free_ports;
        atomic_init(&port->dropped,0);
        assert(pthread_mutex_init(&port->transmit_lock,NULL)==0);
        frame_queue_init(&port->output);
        assert(rate_limit_init(&port->limiter,10000,100)==0);
    }
    for(int i=0;i<3;i++) assert(pthread_create(&sessions[i],NULL,port_session,&ports[i])==0);
    AdminContext admin={.ports=ports,.num_ports=3,.ports_lock=&lock,.mac_table=&table};
    char response[4096];
    assert(handle_admin("shutdown 3",response,sizeof(response),&admin)==-1);
    assert(handle_admin("up 3",response,sizeof(response),&admin)==-1);
    assert(handle_admin("vlan 3 20",response,sizeof(response),&admin)==-1);
    assert(handle_admin("trunk 0",response,sizeof(response),&admin)==-1);
    assert(handle_admin("mirror 3 1",response,sizeof(response),&admin)==-1);
    assert(write(replies[side][1],"R",1)==1);
    char command;
    while(read(commands[side][0],&command,1)==1 && command!='S') {
        assert(command=='M');
        uint8_t remote[6]={2,0,0,0,0,(uint8_t)(side==0 ? 2 : 1)};
        int port;
        assert(mac_table_lookup_vlan(&table,remote,10,&port)==1 && port==2);
        assert(mac_table_lookup_vlan(&table,remote,20,&port)==1 && port==2);
        assert(write(replies[side][1],"M",1)==1);
    }
    assert(pthread_mutex_lock(&lock)==0);
    for(int i=0;i<3;i++) {
        if(ports[i].fd!=-1) {
            if(ports[i].active) {
                ports[i].active=0;
                frame_queue_close(&ports[i].output);
                rate_limit_close(&ports[i].limiter);
            }
            stop_port_io(&ports[i]);
        }
    }
    assert(pthread_mutex_unlock(&lock)==0);
    for(int i=0;i<3;i++) {
        join_worker(sessions[i]);
        assert(pthread_mutex_destroy(&ports[i].transmit_lock)==0);
    }
    assert(pipe_link_destroy(&links[side])==0);
    assert(mac_table_destroy(&table)==0);
    assert(pthread_mutex_destroy(&memory.mutex)==0);
    assert(pthread_mutex_destroy(&lock)==0);
    assert(sem_destroy(&free_ports)==0);
    close(commands[side][0]);close(replies[side][1]);
    _exit(0);
}
static void send_packet(int fd,int source,int destination,int vlan,int priority)
{
    FrameHeader header={.length=3,.vlan=(uint16_t)vlan,.priority=(uint8_t)priority,
        .src={2,0,0,0,0,(uint8_t)source},.dst={2,0,0,0,0,(uint8_t)destination}};
    if(destination==255) memset(header.dst,255,6);
    assert(frame_write(fd,&header,(const uint8_t *)"abc")==0);
}
static void expect_packet(int fd,int source,int vlan,int priority)
{
    FrameHeader header;
    uint8_t payload[FRAME_MAX_PAYLOAD];
    assert(frame_read(fd,&header,payload)==1);
    assert(header.src[5]==source && header.vlan==vlan && header.priority==priority);
    assert(header.length==3 && memcmp(payload,"abc",3)==0);
}
static void quiet(int a,int b,int c)
{
    struct pollfd events[]={{.fd=a,.events=POLLIN},{.fd=b,.events=POLLIN},{.fd=c,.events=POLLIN}};
    assert(poll(events,3,100)==0);
}
int main(void)
{
    assert(setvbuf(stdout,NULL,_IONBF,0)==0);
    assert(signal(SIGPIPE,SIG_IGN)!=SIG_ERR);
    PipeLink links[2];
    assert(pipe_link_pair(links)==0);
    int hosts[4][2],commands[2][2],replies[2][2];
    for(int i=0;i<4;i++) {
        assert(socketpair(AF_UNIX,SOCK_STREAM,0,hosts[i])==0);
        const struct timeval timeout={.tv_sec=5};
        assert(setsockopt(hosts[i][0],SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);
    }
    for(int i=0;i<2;i++) {assert(pipe(commands[i])==0);assert(pipe(replies[i])==0);}
    pid_t children[2];
    for(int i=0;i<2;i++) {
        children[i]=fork();assert(children[i]>=0);
        if(children[i]==0) child_switch(i,links,hosts,commands,replies);
    }
    assert(pipe_link_destroy(&links[0])==0);assert(pipe_link_destroy(&links[1])==0);
    for(int i=0;i<4;i++) close(hosts[i][1]);
    for(int i=0;i<2;i++) {
        close(commands[i][0]);close(replies[i][1]);
        char ready;assert(read(replies[i][0],&ready,1)==1 && ready=='R');
    }
    int a10=hosts[0][0],a20=hosts[1][0],b10=hosts[2][0],b20=hosts[3][0];
    send_packet(a10,1,255,10,0);expect_packet(b10,1,10,0);quiet(a10,a20,b20);
    send_packet(b10,2,255,10,1);expect_packet(a10,2,10,1);quiet(b10,a20,b20);
    puts("PASS: two switch processes forward VLAN10 in both directions without echo or VLAN leakage");
    send_packet(a20,1,255,20,2);expect_packet(b20,1,20,2);quiet(a20,a10,b10);
    send_packet(b20,2,255,20,2);expect_packet(a20,2,20,2);quiet(b20,a10,b10);
    for(int i=0;i<2;i++) {
        assert(write(commands[i][1],"M",1)==1);
        char reply;assert(read(replies[i][0],&reply,1)==1 && reply=='M');
    }
    puts("PASS: identical MACs in VLAN10 and VLAN20 are learned independently behind each pipe trunk");
    send_packet(a10,1,2,10,0);expect_packet(b10,1,10,0);quiet(a10,a20,b20);
    send_packet(b20,2,1,20,2);expect_packet(a20,2,20,2);quiet(b20,a10,b10);
    puts("PASS: learned cross-switch unicast preserves VLAN, priority and payload");
    assert(write(a10,"x",1)==1);
    for(int i=0;i<2;i++) assert(write(commands[i][1],"S",1)==1);
    for(int i=0;i<2;i++) {
        int status;assert(waitpid(children[i],&status,0)==children[i]);
        assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
        close(commands[i][1]);close(replies[i][0]);
    }
    for(int i=0;i<4;i++) close(hosts[i][0]);
    puts("PASS: active and partial-frame sessions stop, join and release both pipe endpoints");
    return 0;
}
