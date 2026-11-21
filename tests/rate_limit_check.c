#define _POSIX_C_SOURCE 200809L

#include "rate_limit.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>

static double now_seconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);

    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void *take_until_closed(void *arg)
{
    RateLimit *limit = arg;

    while (rate_limit_take(limit)) {
        // Exit only when the bucket closes. 
    }

    return NULL;
}

int main(void)
{
    RateLimit limit;

    assert(rate_limit_init(&limit, 0, 1) == EINVAL);
    assert(rate_limit_init(&limit, 20, 0) == EINVAL);
    puts("PASS: invalid settings rejected");

    double start = now_seconds();
    assert(rate_limit_init(&limit, 20, 1) == 0);

    for (int i = 0; i < 6; i++) {
        assert(rate_limit_take(&limit) == 1);
    }

    double elapsed = now_seconds() - start;

    // Clean up before reporting a timing failure. 
    rate_limit_destroy(&limit);

    printf("Measured: six tokens took %.3f seconds\n", elapsed);
    assert(elapsed >= 0.24);

    puts("PASS: token-bucket rate bound");

    assert(rate_limit_init(&limit, 1, 1) == 0);
    assert(rate_limit_take(&limit) == 1);

    pthread_t waiter;
    assert(pthread_create(&waiter, NULL, take_until_closed, &limit) == 0);

    rate_limit_close(&limit);
    assert(pthread_join(waiter, NULL) == 0);
    assert(rate_limit_take(&limit) == 0);

    rate_limit_destroy(&limit);
    puts("PASS: close terminates consumer and joins refill thread");

    return 0;
}