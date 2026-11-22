#ifndef RATE_LIMIT_H
#define RATE_LIMIT_H
#include <pthread.h>
#include <time.h>
typedef struct {
 pthread_mutex_t mutex;
 pthread_cond_t ready;
 pthread_cond_t timer_wake;
 pthread_t timer;
 double tokens, rate, burst;
 struct timespec last_refill;
 int closed;
} RateLimit;
int rate_limit_init(RateLimit *, unsigned int, unsigned int);
int rate_limit_take(RateLimit *);
void rate_limit_close(RateLimit *);
void rate_limit_destroy(RateLimit *);
#endif
