#define _POSIX_C_SOURCE 200809L

#include "rate_limit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_thread(int error)
{
    if (error != 0) {
        fprintf(stderr, "Rate limiter: %s\n", strerror(error));
        abort();
    }
}

static struct timespec monotonic_now(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        perror("rate limiter clock");
        abort();
    }

    return now;
}

static struct timespec after_tick(struct timespec now)
{
    // Refill/check every 10 milliseconds. 
    now.tv_nsec += 10000000L;

    if (now.tv_nsec >= 1000000000L) {
        now.tv_sec++;
        now.tv_nsec -= 1000000000L;
    }

    return now;
}

// Caller holds the bucket mutex. Only the timer replenishes tokens.
static void refill(RateLimit *limit)
{
    struct timespec now = monotonic_now();

    double elapsed =
        (double)(now.tv_sec - limit->last_refill.tv_sec) +
        (double)(now.tv_nsec - limit->last_refill.tv_nsec) / 1e9;

    limit->last_refill = now;
    limit->tokens += elapsed * limit->rate;

    if (limit->tokens > limit->burst) {
        limit->tokens = limit->burst;
    }
}

static void *refill_worker(void *arg)
{
    RateLimit *limit = arg;

    check_thread(pthread_mutex_lock(&limit->mutex));

    while (!limit->closed) {
        struct timespec deadline = after_tick(monotonic_now());
        int error = 0;

        while (!limit->closed && error != ETIMEDOUT) {
            error = pthread_cond_timedwait( &limit->timer_wake, &limit->mutex, &deadline);

            if (error != 0 && error != ETIMEDOUT) 
                check_thread(error);
        }

        if (limit->closed) {
            break;
        }

        refill(limit);

        if (limit->tokens >= 1.0) {
            check_thread(pthread_cond_broadcast(&limit->ready));
        }
    }

    check_thread(pthread_mutex_unlock(&limit->mutex));
    return NULL;
}

int rate_limit_init(RateLimit *limit, unsigned int rate,
                    unsigned int burst)
{
    if (limit == NULL || rate == 0 || burst == 0) {
        return EINVAL;
    }

    int error = pthread_mutex_init(&limit->mutex, NULL);
    if (error != 0) {
        return error;
    }

    pthread_condattr_t attributes;
    error = pthread_condattr_init(&attributes);

    if (error != 0) {
        pthread_mutex_destroy(&limit->mutex);
        return error;
    }

    error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (error != 0) {
        pthread_condattr_destroy(&attributes);
        pthread_mutex_destroy(&limit->mutex);
        return error;
    }

    error = pthread_cond_init(&limit->ready, &attributes);
    if (error != 0) {
        pthread_condattr_destroy(&attributes);
        pthread_mutex_destroy(&limit->mutex);
        return error;
    }

    error = pthread_cond_init(&limit->timer_wake, &attributes);
    check_thread(pthread_condattr_destroy(&attributes));

    if (error != 0) {
        pthread_cond_destroy(&limit->ready);
        pthread_mutex_destroy(&limit->mutex);
        return error;
    }

    limit->rate = (double)rate;
    limit->burst = (double)burst;
    limit->tokens = (double)burst;
    limit->closed = 0;
    limit->last_refill = monotonic_now();

    error = pthread_create(&limit->timer, NULL, refill_worker, limit);

    if (error != 0) {
        pthread_cond_destroy(&limit->timer_wake);
        pthread_cond_destroy(&limit->ready);
        pthread_mutex_destroy(&limit->mutex);
    }

    return error;
}

int rate_limit_take(RateLimit *limit)
{
    check_thread(pthread_mutex_lock(&limit->mutex));

    while (!limit->closed && limit->tokens < 1.0) {
        struct timespec deadline = after_tick(monotonic_now());

        int error = pthread_cond_timedwait(
            &limit->ready, &limit->mutex, &deadline
        );

        if (error != 0 && error != ETIMEDOUT) {
            check_thread(error);
        }
    }

    int allowed = !limit->closed;

    if (allowed) {
        limit->tokens -= 1.0;
    }

    check_thread(pthread_mutex_unlock(&limit->mutex));
    return allowed;
}

void rate_limit_close(RateLimit *limit)
{
    check_thread(pthread_mutex_lock(&limit->mutex));
    limit->closed = 1;
    check_thread(pthread_cond_broadcast(&limit->ready));
    check_thread(pthread_cond_broadcast(&limit->timer_wake));
    check_thread(pthread_mutex_unlock(&limit->mutex));
}

void rate_limit_destroy(RateLimit *limit)
{
    rate_limit_close(limit);

    check_thread(pthread_join(limit->timer, NULL));
    check_thread(pthread_cond_destroy(&limit->timer_wake));
    check_thread(pthread_cond_destroy(&limit->ready));
    check_thread(pthread_mutex_destroy(&limit->mutex));
}