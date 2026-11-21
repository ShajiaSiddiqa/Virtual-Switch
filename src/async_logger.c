#define _POSIX_C_SOURCE 200809L
#include "async_logger.h"
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static void check_thread(int error)
{
    if (error != 0) {
        fprintf(stderr, "Logger synchronization: %s\n", strerror(error));
        abort();
    }
}

static int write_record(FILE *file, const LogRecord *record)
{
    struct tm utc;
    char date[32];
    if (gmtime_r(&record->timestamp.tv_sec, &utc) == NULL ||
        strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc) == 0)
        return -1;
    return fprintf(file, "%s.%03ldZ %s\n", date,
                   record->timestamp.tv_nsec / 1000000L,
                   record->message) < 0 ? -1 : 0;
}

static void *logger_worker(void *arg)
{
    AsyncLogger *logger = arg;
    for (;;) {
        check_thread(pthread_mutex_lock(&logger->mutex));
        while (logger->count == 0 && !logger->closing)
            check_thread(pthread_cond_wait(&logger->ready, &logger->mutex));
        if (logger->count == 0) {
            check_thread(pthread_mutex_unlock(&logger->mutex));
            return NULL;
        }
        LogRecord record = logger->records[logger->head];
        logger->head = (logger->head + 1) % LOGGER_CAPACITY;
        logger->count--;
        check_thread(pthread_mutex_unlock(&logger->mutex));
        /* File I/O is never performed while holding the queue mutex. */
        if (write_record(logger->file, &record) == -1)
            logger->failed = 1;
    }
}

int async_logger_start(AsyncLogger *logger, const char *path)
{
    if (logger == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }
    logger->head = logger->count = 0;
    logger->closing = logger->failed = 0;
    atomic_init(&logger->dropped, 0);
    logger->file = fopen(path, "a");
    if (logger->file == NULL)
        return -1;
    /* Each newline becomes visible promptly; only the logger thread writes. */
    if (setvbuf(logger->file, NULL, _IOLBF, 0) != 0) {
        fclose(logger->file);
        logger->file = NULL;
        errno = EIO;
        return -1;
    }
    int error = pthread_mutex_init(&logger->mutex, NULL);
    if (error != 0)
        goto close_file;
    error = pthread_cond_init(&logger->ready, NULL);
    if (error != 0)
        goto destroy_mutex;
    error = pthread_create(&logger->thread, NULL, logger_worker, logger);
    if (error == 0)
        return 0;
    check_thread(pthread_cond_destroy(&logger->ready));
destroy_mutex:
    check_thread(pthread_mutex_destroy(&logger->mutex));
close_file:
    fclose(logger->file);
    logger->file = NULL;
    errno = error;
    return -1;
}

int async_logger_vtry_log(AsyncLogger *logger, const char *format, va_list args)
{
    if (logger == NULL || format == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* Fail immediately if another producer or the consumer owns the lock. */
    int error = pthread_mutex_trylock(&logger->mutex);
    if (error == EBUSY) {
        atomic_fetch_add_explicit(&logger->dropped, 1, memory_order_relaxed);
        return 0;
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    if (logger->closing || logger->count == LOGGER_CAPACITY) {
        atomic_fetch_add_explicit(&logger->dropped, 1, memory_order_relaxed);
        check_thread(pthread_mutex_unlock(&logger->mutex));
        return 0;
    }
    size_t tail = (logger->head + logger->count) % LOGGER_CAPACITY;
    LogRecord *record = &logger->records[tail];
    if (clock_gettime(CLOCK_REALTIME, &record->timestamp) == -1) {
        int saved = errno;
        check_thread(pthread_mutex_unlock(&logger->mutex));
        errno = saved;
        return -1;
    }
    int length = vsnprintf(record->message, sizeof(record->message), format, args);
    if (length < 0) {
        check_thread(pthread_mutex_unlock(&logger->mutex));
        errno = EIO;
        return -1;
    }
    if ((size_t)length >= sizeof(record->message)) {
        const char suffix[] = " [truncated]";
        memcpy(record->message + sizeof(record->message) - sizeof(suffix),
               suffix, sizeof(suffix));
    }
    /* Keep one log record on one physical line. */
    for (char *p = record->message; *p != '\0'; p++) {
        if (*p == '\n' || *p == '\r')
            *p = ' ';
    }
    logger->count++;
    check_thread(pthread_cond_signal(&logger->ready));
    check_thread(pthread_mutex_unlock(&logger->mutex));
    return 1;
}

int async_logger_try_log(AsyncLogger *logger, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = async_logger_vtry_log(logger, format, args);
    va_end(args);
    return result;
}

unsigned long async_logger_dropped(const AsyncLogger *logger)
{
    return atomic_load_explicit(&logger->dropped, memory_order_relaxed);
}

int async_logger_stop(AsyncLogger *logger)
{
    check_thread(pthread_mutex_lock(&logger->mutex));
    logger->closing = 1;
    check_thread(pthread_cond_broadcast(&logger->ready));
    check_thread(pthread_mutex_unlock(&logger->mutex));
    check_thread(pthread_join(logger->thread, NULL));
    int failed = logger->failed;
    if (fclose(logger->file) == EOF)
        failed = 1;
    logger->file = NULL;
    check_thread(pthread_cond_destroy(&logger->ready));
    check_thread(pthread_mutex_destroy(&logger->mutex));
    if (failed) {
        errno = EIO;
        return -1;
    }
    return 0;
}
