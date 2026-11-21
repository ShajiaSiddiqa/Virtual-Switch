#ifndef SIGNAL_CONTROL_H
#define SIGNAL_CONTROL_H

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>

typedef int (*SignalAction)(void *context);

typedef struct {
    pthread_t thread;
    sigset_t signals;
    atomic_int stopping;
    int started;
    int failed;

    SignalAction shutdown;
    SignalAction reload;
    SignalAction dump;
    void *context;
} SignalControl;

// Block SIGINT, SIGHUP, and SIGUSR1 in the calling thread.Call from main before creating any worker threads. 
int signal_control_block(sigset_t *signals);

// Start the signal worker with the blocked signal set. All callbacks and context must remain valid until stop returns.*
int signal_control_start(SignalControl *control,  const sigset_t *signals,  SignalAction shutdown,  SignalAction reload,  SignalAction dump,  void *context);

// Request worker termination, wake it, and join it. Call from the coordinating thread.
int signal_control_stop(SignalControl *control);

#endif