#define _POSIX_C_SOURCE 200809L
#include "mac_store.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Single-threaded parent: all signal handling is synchronous. */
typedef struct {
    char store[128], socket[128], stats[128], log[128], dump[128], config[128];
} Instance;

static int setting(const char *name, int fallback, int minimum, int maximum)
{
    const char *text=getenv(name);
    if (text==NULL) return fallback;
    char *end;
    errno=0;
    long value=strtol(text,&end,10);
    if (errno || end==text || *end || value<minimum || value>maximum) {
        fprintf(stderr,"Invalid %s: expected %d-%d\n",name,minimum,maximum);
        return -1;
    }
    return (int)value;
}
static double now(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC,&value)==-1)
        return -1;
    return (double)value.tv_sec+(double)value.tv_nsec/1e9;
}
static int remove_worker_ipc(const Instance *instance)
{
    int status=0;
    if (unlink(instance->socket)==-1 && errno!=ENOENT) {
        perror("unlink worker socket");status=-1;
    }
    if (shm_unlink(instance->stats)==-1 && errno!=ENOENT) {
        perror("unlink worker statistics");status=-1;
    }
    return status;
}
static void child_environment(const Instance *instance, int restarts)
{
    const char *keys[]={"VSWITCH_MAC_STORE","VSWITCH_SOCKET","VSWITCH_STATS",
        "SWITCH_LOG_PATH","SWITCH_DUMP_PATH","SWITCH_CONFIG_PATH"};
    const char *values[]={instance->store,instance->socket,instance->stats,
        instance->log,instance->dump,instance->config};
    for (size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++)
        if (setenv(keys[i],values[i],1)==-1) {perror("setenv child");_exit(127);}
    char restart_text[32];
    snprintf(restart_text,sizeof(restart_text),"%d",restarts);
    if (setenv("VSWITCH_RESTART_COUNT",restart_text,1)==-1) {perror("setenv restart count");_exit(127);}
    /* This standalone supervisor manages a single switch, not a pipe-linked pair. */
    if (unsetenv("SWITCH_PIPE_READ_FD")==-1 || unsetenv("SWITCH_PIPE_WRITE_FD")==-1) {
        perror("clear inherited pipe settings");_exit(127);
    }
}
static int pending(const sigset_t *signals, long milliseconds)
{
    const struct timespec interval={.tv_sec=milliseconds/1000,
                                    .tv_nsec=(milliseconds%1000)*1000000};
    return sigtimedwait(signals,NULL,&interval);
}
static int backoff(const sigset_t *signals, int milliseconds, int *stopping)
{
    double start=now();
    if (start<0) return -1;
    double deadline=start+(double)milliseconds/1000;
    for (;;) {
        double current=now();
        if (current<0) return -1;
        if (current>=deadline) return 0;
        long remaining=(long)((deadline-current)*1000)+1;
        int received=pending(signals,remaining);
        if (received==SIGINT || received==SIGTERM) {*stopping=1;return 0;}
        if (received==-1 && errno!=EAGAIN && errno!=EINTR) return -1;
        /* CHLD/HUP/USR1 during backoff do not target a stale worker PID. */
    }
}
static int wait_worker(pid_t child, const sigset_t *signals, int grace_ms,
                        int *stopping, int *child_status)
{
    int sent=0, forced=0;
    double deadline=0;
    for (;;) {
        int received=pending(signals,0);
        if (received==SIGINT || received==SIGTERM) *stopping=1;
        else if (received==SIGHUP || received==SIGUSR1) {
            if (!*stopping && kill(child,received)==-1 && errno!=ESRCH) return -1;
        } else if (received==-1 && errno!=EAGAIN && errno!=EINTR) return -1;

        pid_t result=waitpid(child,child_status,WNOHANG);
        if (result==child) return forced ? 1 : 0;
        if (result==-1 && errno!=EINTR) return -1;
        if (*stopping && !sent) {
            if (kill(child,SIGINT)==-1 && errno!=ESRCH) return -1;
            double current=now();
            if (current<0) return -1;
            deadline=current+(double)grace_ms/1000;
            sent=1;
        }
        if (sent && !forced) {
            double current=now();
            if (current<0) return -1;
            if (current>=deadline) {
                fprintf(stderr,"Supervisor: graceful stop timed out; killing worker %ld\n",(long)child);
                if (kill(child,SIGKILL)==-1 && errno!=ESRCH) return -1;
                forced=1;
            }
        }
        received=pending(signals,100);
        if (received==SIGINT || received==SIGTERM) *stopping=1;
        else if (received==SIGHUP || received==SIGUSR1) {
            if (!*stopping && kill(child,received)==-1 && errno!=ESRCH) return -1;
        } else if (received==-1 && errno!=EAGAIN && errno!=EINTR) return -1;
    }
}
int main(int argc, char *argv[])
{
    if (argc<2 || strcmp(argv[1],"--help")==0) {
        fprintf(stderr,"Usage: %s <worker_executable> [worker_arguments...]\n",argv[0]);
        return argc<2 ? 1 : 0;
    }
    int restart_limit=setting("VSWITCH_RESTART_LIMIT",5,0,100);
    int delay_ms=setting("VSWITCH_RESTART_DELAY_MS",250,10,60000);
    int grace_ms=setting("VSWITCH_SUPERVISOR_GRACE_MS",5000,100,60000);
    if (restart_limit<0 || delay_ms<0 || grace_ms<0) return 1;
    sigset_t signals, old_mask;
    sigemptyset(&signals);
    const int numbers[]={SIGINT,SIGTERM,SIGCHLD,SIGHUP,SIGUSR1};
    for (size_t i=0;i<sizeof(numbers)/sizeof(numbers[0]);i++) sigaddset(&signals,numbers[i]);
    int error=pthread_sigmask(SIG_BLOCK,&signals,&old_mask);
    if (error) {errno=error;perror("supervisor signal mask");return 1;}
    /* Inherited SIG_IGN for CHLD would otherwise make waitpid lose children. */
    struct sigaction default_action={.sa_handler=SIG_DFL};
    sigemptyset(&default_action.sa_mask);
    for (size_t i=0;i<sizeof(numbers)/sizeof(numbers[0]);i++) {
        if (sigaction(numbers[i],&default_action,NULL)==-1) {
            perror("supervisor signal disposition");return 1;
        }
    }
    Instance instance;
    long id=(long)getpid();
    snprintf(instance.store,sizeof(instance.store),"/vswitch_super_%ld_mac",id);
    snprintf(instance.socket,sizeof(instance.socket),"/tmp/vswitch-super-%ld.sock",id);
    snprintf(instance.stats,sizeof(instance.stats),"/vswitch_super_%ld_stats",id);
    snprintf(instance.log,sizeof(instance.log),"build/super-%ld.log",id);
    snprintf(instance.dump,sizeof(instance.dump),"build/super-%ld-mac.txt",id);
    snprintf(instance.config,sizeof(instance.config),"build/super-%ld.conf",id);
    MacStore store;
    if (mac_store_create(&store,instance.store)==-1) {perror("create supervisor MAC store");return 1;}
    printf("SUPERVISOR pid=%ld store=%s socket=%s stats=%s\n",id,instance.store,instance.socket,instance.stats);
    fflush(stdout);
    int status=0, stopping=0, restarts=0;
    while (!stopping) {
        MacStoreImage recovery;
        if (mac_store_read(&store,&recovery)==-1) {perror("validate recovery image");status=1;break;}
        pid_t child=fork();
        if (child==-1) {perror("fork worker");status=1;break;}
        if (child==0) {
            if (mac_store_close(&store)==-1) {perror("close inherited mapping");_exit(127);}
            child_environment(&instance,restarts);
            error=pthread_sigmask(SIG_SETMASK,&old_mask,NULL);
            if (error) {errno=error;perror("restore worker signal mask");_exit(127);}
            execvp(argv[1],&argv[1]);
            perror("exec worker");_exit(127);
        }
        printf("WORKER pid=%ld restart=%d recovered_generation=%llu entries=%u\n",
               (long)child,restarts,(unsigned long long)recovery.generation,recovery.count);
        fflush(stdout);
        int code;
        int waited=wait_worker(child,&signals,grace_ms,&stopping,&code);
        if (waited<0) {
            perror("wait worker");
            kill(child,SIGKILL);
            while (waitpid(child,&code,0)==-1 && errno==EINTR) {}
            status=1;break;
        }
        /* Worker has been reaped; only now remove its stale IPC resources. */
        if (remove_worker_ipc(&instance)==-1) {status=1;break;}
        if (waited>0) {status=1;break;}
        if (stopping) {
            if (!WIFEXITED(code) || WEXITSTATUS(code)!=0) status=1;
            break;
        }
        if (WIFEXITED(code) && WEXITSTATUS(code)==0) break;
        if (WIFEXITED(code) && WEXITSTATUS(code)==127) {
            fprintf(stderr,"Supervisor: exec/startup failure; not restarting\n");status=1;break;
        }
        if (restarts>=restart_limit) {
            fprintf(stderr,"Supervisor: restart limit reached (%d)\n",restart_limit);status=1;break;
        }
        if (WIFSIGNALED(code))
            fprintf(stderr,"Supervisor: worker died from signal %d; restarting\n",WTERMSIG(code));
        else
            fprintf(stderr,"Supervisor: worker exited with status %d; restarting\n",WEXITSTATUS(code));
        if (backoff(&signals,delay_ms,&stopping)==-1) {perror("restart backoff");status=1;break;}
        if (!stopping) restarts++;
    }
    if (remove_worker_ipc(&instance)==-1) status=1;
    if (mac_store_close(&store)==-1) {perror("close supervisor MAC store");status=1;}
    if (mac_store_unlink(instance.store)==-1) {perror("unlink supervisor MAC store");status=1;}
    printf("SUPERVISOR stopped status=%d restarts=%d\n",status,restarts);
    return status;
}
