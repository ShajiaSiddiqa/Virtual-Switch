#define _POSIX_C_SOURCE 200809L

#include "signal_control.h"

#include <errno.h>
#include <string.h>

static int thread_error(int error)
{
    errno = error;
    return -1;
}

int signal_control_block(sigset_t *signals)
{
    if (signals == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (sigemptyset(signals) == -1 || sigaddset(signals, SIGINT) == -1 || sigaddset(signals, SIGHUP) == -1 || sigaddset(signals, SIGUSR1) == -1) 
        return -1;

    int error = pthread_sigmask(SIG_BLOCK, signals, NULL);

    return error == 0 ? 0 : thread_error(error);
}

static void *signal_thread(void *arg)
{
    SignalControl *control = arg;

    for (;;) {
        int received;
        int error = sigwait(&control->signals, &received);

        if (error != 0) {
            control->failed = error;
            return NULL;
        }

        if (atomic_load(&control->stopping)) 
            return NULL;

        SignalAction action = NULL;

        switch (received) {
        case SIGINT:
            action = control->shutdown;
            break;

        case SIGHUP:
            action = control->reload;
            break;

        case SIGUSR1:
            action = control->dump;
            break;
        }

        if (action != NULL && action(control->context) != 0) 
            control->failed = EIO;
    }
}

int signal_control_start( SignalControl *control, const sigset_t *signals, SignalAction shutdown, SignalAction reload, SignalAction dump, void *context)
{
    if (control == NULL || signals == NULL || shutdown == NULL || reload == NULL || dump == NULL) {
        errno = EINVAL;
        return -1;
    }

    control->signals = *signals;
    control->shutdown = shutdown;
    control->reload = reload;
    control->dump = dump;
    control->context = context;
    control->started = 0;
    control->failed = 0;

    atomic_init(&control->stopping, 0);

    int error = pthread_create( &control->thread, NULL, signal_thread, control);

    if (error != 0) 
        return thread_error(error);

    control->started = 1;
    return 0;
}

int signal_control_stop(SignalControl *control)
{
    if (control == NULL || !control->started) {
        errno = EINVAL;
        return -1;
    }

    atomic_store(&control->stopping, 1);

    int wake_error = pthread_kill(control->thread, SIGUSR1);
    int error = pthread_join(control->thread, NULL);

    if (error != 0) 
        return thread_error(error);

    control->started = 0;

    if (wake_error != 0 && wake_error != ESRCH) 
        return thread_error(wake_error);

    if (control->failed != 0) 
        return thread_error(control->failed);

    return 0;
}