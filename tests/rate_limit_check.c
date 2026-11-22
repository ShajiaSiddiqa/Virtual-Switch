#define _POSIX_C_SOURCE 200809L
#include "rate_limit.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
static void *blocked(void *argument) {assert(rate_limit_take(argument)==0);return NULL;}
int main(void)
{
    RateLimit limiter;
    assert(rate_limit_init(NULL,1,1)==EINVAL);
    assert(rate_limit_init(&limiter,0,1)==EINVAL);
    assert(rate_limit_init(&limiter,1,0)==EINVAL);
    assert(puts("PASS: invalid rate/burst settings rejected")>=0);
    struct timespec start,finish;
    /* Start before initialization so instrumentation startup delay is counted. */
    assert(clock_gettime(CLOCK_MONOTONIC,&start)==0);
    assert(rate_limit_init(&limiter,20,1)==0);
    for(int i=0;i<6;i++) assert(rate_limit_take(&limiter)==1);
    assert(clock_gettime(CLOCK_MONOTONIC,&finish)==0);
    double elapsed=(double)(finish.tv_sec-start.tv_sec)+(double)(finish.tv_nsec-start.tv_nsec)/1e9;
    assert(elapsed>=0.24);
    rate_limit_destroy(&limiter);
    assert(printf("PASS: six tokens at 20/s with burst 1 took %.3f seconds\n",elapsed)>=0);
    assert(rate_limit_init(&limiter,1,1)==0);
    assert(rate_limit_take(&limiter)==1);
    pthread_t thread;
    assert(pthread_create(&thread,NULL,blocked,&limiter)==0);
    rate_limit_close(&limiter);
    assert(pthread_join(thread,NULL)==0);
    rate_limit_destroy(&limiter);
    assert(puts("PASS: close terminates token consumer and joins refill worker")>=0);
    return 0;
}
