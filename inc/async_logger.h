#ifndef ASYNC_LOGGER_H
#define ASYNC_LOGGER_H
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

#define LOGGER_CAPACITY 64
#define LOGGER_MESSAGE_SIZE 256

typedef struct {
    struct timespec timestamp;
    char message[LOGGER_MESSAGE_SIZE];
} LogRecord;

typedef struct {
    LogRecord records[LOGGER_CAPACITY];
    size_t head, count;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t thread;
    FILE *file;
    atomic_ulong dropped;
    int closing;
    int failed;
} AsyncLogger;

/* Returns 0 or -1 with errno. Initialize before publishing to producers. */
int async_logger_start(AsyncLogger *logger, const char *path);
/* 1 accepted, 0 dropped (busy/full/closing), -1 formatting/clock error. */
int async_logger_try_log(AsyncLogger *logger, const char *format, ...);
int async_logger_vtry_log(AsyncLogger *logger, const char *format, va_list args);
unsigned long async_logger_dropped(const AsyncLogger *logger);
/* Stop all producers first. Drains, joins, closes and destroys resources. */
int async_logger_stop(AsyncLogger *logger);
#endif
