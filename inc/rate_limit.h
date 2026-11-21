#ifndef RATE_LIMIT_H
#define RATE_LIMIT_H

#include <pthread.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_cond_t timer_wake;
    pthread_t timer;

    double tokens;
    double rate;
    double burst;
    struct timespec last_refill;

    int closed;
} RateLimit;

// rate and burst must be positive
int rate_limit_init(RateLimit *limit, unsigned int rate,
                    unsigned int burst);

// Returns 1 after consuming a token, or 0 after close. 
int rate_limit_take(RateLimit *limit);

// Wake the timer and all waiting senders. 
void rate_limit_close(RateLimit *limit);

// Call after all sender users finish. Joins the timer and frees resources. 
void rate_limit_destroy(RateLimit *limit);

#endif