#define _POSIX_C_SOURCE 200809L
#include "checked.h"
#include "async_logger.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    AsyncLogger *logger;
    unsigned int id;
    unsigned int accepted;
} Producer;

static void *produce(void *arg)
{
    Producer *producer = arg;
    for (unsigned int i = 0; i < 1000; i++) {
        int result = async_logger_try_log(producer->logger,
                                         "producer=%u sequence=%u", producer->id, i);
        assert(result == 0 || result == 1);
        producer->accepted += result == 1;
    }
    return NULL;
}
static void *contended(void *arg)
{
    assert(async_logger_try_log(arg, "must drop immediately") == 0);
    return NULL;
}

int main(void)
{
    char path[] = "build/logger-check-XXXXXX";
    int fd = mkstemp(path);
    assert(fd != -1);
    assert(close(fd) == 0);
    AsyncLogger logger;
    assert(async_logger_start(&logger, path) == 0);

    assert(pthread_mutex_lock(&logger.mutex) == 0);
    pthread_t busy;
    assert(pthread_create(&busy, NULL, contended, &logger) == 0);
    assert(pthread_join(busy, NULL) == 0);
    assert(async_logger_dropped(&logger) == 1);
    assert(pthread_mutex_unlock(&logger.mutex) == 0);
    vsw_posix(puts("PASS: contended producer drops without waiting for queue lock"), "puts");

    Producer producers[4];
    pthread_t threads[4];
    for (unsigned int i = 0; i < 4; i++) {
        producers[i] = (Producer){.logger = &logger, .id = i};
        assert(pthread_create(&threads[i], NULL, produce, &producers[i]) == 0);
    }
    unsigned int accepted = 0;
    for (unsigned int i = 0; i < 4; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
        accepted += producers[i].accepted;
    }
    assert(accepted > 0);
    unsigned long dropped = async_logger_dropped(&logger);
    assert(accepted + dropped == 4001);
    assert(async_logger_stop(&logger) == 0);
    vsw_posix(puts("PASS: concurrent accepted and dropped message accounting"), "puts");

    FILE *file = fopen(path, "r");
    assert(file);
    unsigned int lines = 0;
    char text[512];
    while (fgets(text, sizeof(text), file) != NULL) {
        assert(strlen(text) >= 26);
        assert(text[4] == '-' && text[10] == 'T' && text[23] == 'Z');
        assert(strstr(text, "producer=") != NULL);
        assert(strchr(text, '\n') != NULL);
        lines++;
    }
    assert(!ferror(file));
    assert(fclose(file) == 0);
    assert(lines == accepted);
    assert(unlink(path) == 0);
    vsw_posix(puts("PASS: timestamped complete records and drain-on-stop"), "puts");
    return 0;
}
