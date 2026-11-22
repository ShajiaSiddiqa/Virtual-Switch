#define _POSIX_C_SOURCE 200809L
#include "../../inc/checked.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int number(const char *text, int minimum, int maximum)
{
    char *end;
    errno=0;
    long value=strtol(text,&end,10);
    return errno || end==text || *end || value<minimum || value>maximum ? -1 : (int)value;
}
static void set_value(const char *key, const char *value)
{
    if(setenv(key,value,1)==-1) { perror("setenv"); _exit(127); }
}
static double monotonic(void)
{
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now)==-1) { perror("clock_gettime"); exit(1); }
    return (double)now.tv_sec+(double)now.tv_nsec/1e9;
}
static void child(int index, int ab[2], int ba[2], const char *tcp,
                  const char *ports, pid_t owner, const sigset_t *old_mask)
{
    int input=index==0 ? ba[0] : ab[0];
    int output=index==0 ? ab[1] : ba[1];
    int fds[]={ab[0],ab[1],ba[0],ba[1]};
    for(size_t i=0;i<4;i++)
        if(fds[i]!=input && fds[i]!=output) vsw_posix(close(fds[i]), "close");
    char value[128];
    vsw_format(snprintf(value,sizeof(value),"%d",input), sizeof(value));set_value("SWITCH_PIPE_READ_FD",value);
    vsw_format(snprintf(value,sizeof(value),"%d",output), sizeof(value));set_value("SWITCH_PIPE_WRITE_FD",value);
    vsw_format(snprintf(value,sizeof(value),"/tmp/vswitch-%ld-%c.sock",(long)owner,'a'+index), sizeof(value));
    set_value("VSWITCH_SOCKET",value);
    vsw_format(snprintf(value,sizeof(value),"/vswitch_%ld_%c_stats",(long)owner,'a'+index), sizeof(value));
    set_value("VSWITCH_STATS",value);
    vsw_format(snprintf(value,sizeof(value),"build/two-%ld-%c.log",(long)owner,'a'+index), sizeof(value));
    set_value("SWITCH_LOG_PATH",value);
    vsw_format(snprintf(value,sizeof(value),"build/two-%ld-%c-mac.txt",(long)owner,'a'+index), sizeof(value));
    set_value("SWITCH_DUMP_PATH",value);
    vsw_format(snprintf(value,sizeof(value),"build/two-%ld-%c.conf",(long)owner,'a'+index), sizeof(value));
    set_value("SWITCH_CONFIG_PATH",value);
    int error=pthread_sigmask(SIG_SETMASK,old_mask,NULL);
    if(error) { errno=error;perror("restore signal mask");_exit(127); }
    const char *executable=getenv("VSWITCH_BINARY");
    if(executable==NULL) executable="./switch";
    char *const args[]={(char *)executable,(char *)tcp,(char *)ports,NULL};
    execvp(executable,args);
    perror("exec switch");_exit(127);
}
int main(int argc,char *argv[])
{
    if(argc!=4 || number(argv[1],1,65535)<0 || number(argv[2],1,65535)<0 ||
       number(argv[3],2,64)<0 || number(argv[1],1,65535)==number(argv[2],1,65535)) {
        vsw_posix(fprintf(stderr,"Usage: %s <tcp_A> <tcp_B> <ports_per_switch:2-64>\n",argv[0]), "fprintf");
        return 1;
    }
    sigset_t signals, old_mask;
    vsw_posix(sigemptyset(&signals), "sigemptyset");vsw_posix(sigaddset(&signals,SIGINT), "sigaddset");vsw_posix(sigaddset(&signals,SIGTERM), "sigaddset");
    vsw_posix(sigaddset(&signals,SIGCHLD), "sigaddset");
    int error=pthread_sigmask(SIG_BLOCK,&signals,&old_mask);
    if(error) { errno=error;perror("signal mask");return 1; }
    int ab[2],ba[2];
    if(pipe(ab)==-1) { perror("pipe A-B");return 1; }
    if(pipe(ba)==-1) { perror("pipe B-A");vsw_posix(close(ab[0]), "close");vsw_posix(close(ab[1]), "close");return 1; }
    pid_t owner=getpid(),children[2]={0,0};
    int status=0, stopping=0;
    for(int i=0;i<2;i++) {
        children[i]=fork();
        if(children[i]==0) child(i,ab,ba,argv[i+1],argv[3],owner,&old_mask);
        if(children[i]==-1) { perror("fork");children[i]=0;status=1;stopping=1;break; }
        vsw_posix(printf("Switch %c PID=%ld TCP=%s SOCKET=/tmp/vswitch-%ld-%c.sock STATS=/vswitch_%ld_%c_stats\n",
               'A'+i,(long)children[i],argv[i+1],(long)owner,'a'+i,(long)owner,'a'+i), "printf");
        vsw_posix(fflush(stdout), "fflush");
    }
    vsw_posix(close(ab[0]), "close");vsw_posix(close(ab[1]), "close");vsw_posix(close(ba[0]), "close");vsw_posix(close(ba[1]), "close");
    int sent=0;
    double deadline=0;
    while(children[0]!=0 || children[1]!=0) {
        for(int i=0;i<2;i++) {
            if(children[i]==0) continue;
            int code;
            pid_t result=waitpid(children[i],&code,WNOHANG);
            if(result==children[i]) {
                if(!stopping || !WIFEXITED(code) || WEXITSTATUS(code)!=0) status=1;
                children[i]=0;stopping=1;
            } else if(result==-1 && errno!=EINTR) {
                perror("waitpid");children[i]=0;status=1;stopping=1;
            }
        }
        if(children[0]==0 && children[1]==0) break;
        if(stopping && !sent) {
            for(int i=0;i<2;i++) if(children[i]>0) vsw_kill(children[i],SIGINT);
            sent=1;deadline=monotonic()+10;
        }
        if(sent && monotonic()>=deadline) {
            for(int i=0;i<2;i++) if(children[i]>0) vsw_kill(children[i],SIGKILL);
            status=1;deadline=monotonic()+10;
        }
        const struct timespec interval={.tv_nsec=100000000};
        int received=sigtimedwait(&signals,NULL,&interval);
        if(received==SIGINT || received==SIGTERM) stopping=1;
        else if(received==-1 && errno!=EAGAIN && errno!=EINTR) {
            perror("sigtimedwait");status=1;stopping=1;
        }
    }
    /* Names belong to this launch; remove leftovers only after both children exit. */
    for(int i=0;i<2;i++) {
        char name[128];
        vsw_format(snprintf(name,sizeof(name),"/tmp/vswitch-%ld-%c.sock",(long)owner,'a'+i), sizeof(name));
        if(unlink(name)==-1 && errno!=ENOENT) status=1;
        vsw_format(snprintf(name,sizeof(name),"/vswitch_%ld_%c_stats",(long)owner,'a'+i), sizeof(name));
        if(shm_unlink(name)==-1 && errno!=ENOENT) status=1;
    }
    return status;
}
