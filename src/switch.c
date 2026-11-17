#define _POSIX_C_SOURCE 200809L
#include "frame_io.h"
#include "frame_queue.h"
#include "mac_table.h"
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define MAX_TEST_PORTS 64
#define MAC_AGE_SECONDS 5
#define MAC_SWEEP_SECONDS 1

typedef struct {
    MacTable *table;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    int stop;
    int failed;
} MacAging;

static int parse_number(const char *text, long max)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > max)
        return -1;
    return (int)value;
}

typedef struct Port {
    int fd;
    int num_ports;
    FrameQueue output;
    struct Port *ports;
    int receive_status;
    atomic_uint dropped;
    MacTable *mac_table;
} Port;

static void enqueue_frame(Port *target, SharedFrame *frame)
{
    if (frame_queue_try_push(&target->output, frame) == 0) {
        atomic_fetch_add(&target->dropped, 1);
    }
}

static void flood_frame(Port *source, SharedFrame *frame)
{
    for (int i = 0; i < source->num_ports; i++) {
        Port *target = &source->ports[i];
        if (target != source) {
            enqueue_frame(target, frame);
        }
    }
}

static int forward_frame(Port *source, SharedFrame *frame)
{
    int ingress = (int)(source - source->ports);
    int destination;

    int learned = mac_table_insert(source->mac_table, frame->header.src, ingress);
    if (learned != 0) 
        return learned;

    if ((frame->header.dst[0] & 1u) != 0) {
        flood_frame(source, frame);
        return 0;
    }

    int found = mac_table_lookup(source->mac_table, frame->header.dst, &destination);
    if (found < 0) 
        return -1;

    if (found == 0) 
        flood_frame(source, frame);
    else if (destination != ingress) 
        enqueue_frame(&source->ports[destination], frame);

    return 0;
}

static void *receiver_thread(void *arg)
{
    Port *port = arg;
    FrameHeader header;
    uint8_t payload[FRAME_MAX_PAYLOAD];
    for (;;) {
        int result = frame_read(port->fd, &header, payload);
        if (result == 0) {
            return NULL;
        }
        if (result < 0) {
            perror("frame_read");
            port->receive_status = EXIT_FAILURE;
            return NULL;
        }
        SharedFrame *frame = shared_frame_create(&header, payload);
        if (frame == NULL) {
            perror("shared_frame_create");
            port->receive_status = EXIT_FAILURE;
            return NULL;
        }
        int forward_result = forward_frame(port, frame);
        shared_frame_release(frame);
        if (forward_result == MAC_TABLE_PORT_LIMIT_REACHED) {
            int port_number = (int)(port - port->ports) + 1;
            fprintf(stderr, "Port %d shut down: MAC limit exceeded\n", port_number);

            frame_queue_close(&port->output);

            if (shutdown(port->fd, SHUT_RDWR) == -1 && errno != ENOTCONN) {
                perror("shutdown client");
                port->receive_status = EXIT_FAILURE;
            }

            return NULL;
        }
        if (forward_result != 0) {
            fprintf(stderr, "MAC table operation failed\n");
            port->receive_status = EXIT_FAILURE;
            return NULL;
        }
    }
}

static void *sender_thread(void *arg)
{
    Port *port = arg;
    SharedFrame *frame;
    while (frame_queue_pop(&port->output, &frame) == 1) {
        int result = frame_write(port->fd, &frame->header, frame->payload);
        int write_error = errno;
        shared_frame_release(frame);
        if (result != 0) {
            errno = write_error;
            perror("frame_write");
            frame_queue_close(&port->output);
            if (shutdown(port->fd, SHUT_RDWR) == -1 &&
                errno != ENOTCONN) {
                perror("shutdown client");
            }
            break;
        }
    }
    return NULL;
}

static void join_worker(pthread_t thread)
{
    int error = pthread_join(thread, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_join: %s\n", strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void check_aging_thread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void *mac_aging_worker(void *arg)
{
    MacAging *aging = arg;

    check_aging_thread(pthread_mutex_lock(&aging->mutex), "aging lock");

    while (!aging->stop) {
        struct timespec deadline;

        if (clock_gettime(CLOCK_REALTIME, &deadline) == -1) {
            perror("clock_gettime");
            aging->failed = 1;
            break;
        }
        deadline.tv_sec += MAC_SWEEP_SECONDS;

        int result = 0;
        while (!aging->stop && result == 0) {
            result = pthread_cond_timedwait(&aging->wake, &aging->mutex,
                                            &deadline);
        }

        if (result != 0 && result != ETIMEDOUT) {
            fprintf(stderr, "aging wait: %s\n", strerror(result));
            aging->failed = 1;
            break;
        }
        if (aging->stop) {
            break;
        }

        check_aging_thread(pthread_mutex_unlock(&aging->mutex), "aging unlock");
        int removed = mac_table_expire(aging->table, MAC_AGE_SECONDS);
        check_aging_thread(pthread_mutex_lock(&aging->mutex), "aging lock");

        if (removed < 0) {
            fprintf(stderr, "MAC aging sweep failed\n");
            aging->failed = 1;
            break;
        }
    }

    check_aging_thread(pthread_mutex_unlock(&aging->mutex), "aging unlock");
    return NULL;
}

static int serve_hosts(int listener, int num_ports, MacTable *mac_table)
{
    Port ports[MAX_TEST_PORTS];
    pthread_t receivers[MAX_TEST_PORTS];
    pthread_t senders[MAX_TEST_PORTS];
    int accepted = 0;
    int receivers_started = 0;
    int senders_started = 0;
    int status = EXIT_SUCCESS;
    const struct timeval send_timeout = {.tv_sec = 2};
    while (accepted < num_ports) {
        int fd;
        do {
            fd = accept(listener, NULL, NULL);
        } while (fd == -1 && errno == EINTR);
        if (fd == -1) {
            perror("accept");
            status = EXIT_FAILURE;
            break;
        }
        Port *port = &ports[accepted];
        port->fd = fd;
        port->receive_status = EXIT_SUCCESS;
        port->ports = ports;
        port->num_ports = num_ports;
        port->mac_table = mac_table;
        atomic_init(&port->dropped, 0);
        frame_queue_init(&port->output);
        accepted++;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                       &send_timeout, sizeof(send_timeout)) == -1) {
            perror("setsockopt SO_SNDTIMEO");
            status = EXIT_FAILURE;
            break;
        }
    }
    while (status == EXIT_SUCCESS && senders_started < accepted) {
        int i = senders_started;
        int error = pthread_create(&senders[i], NULL, sender_thread,
                                   &ports[i]);
        if (error != 0) {
            fprintf(stderr, "pthread_create sender: %s\n", strerror(error));
            status = EXIT_FAILURE;
            break;
        }
        senders_started++;
    }
    while (status == EXIT_SUCCESS && receivers_started < accepted) {
        int i = receivers_started;
        int error = pthread_create(&receivers[i], NULL, receiver_thread,
                                   &ports[i]);
        if (error != 0) {
            fprintf(stderr, "pthread_create receiver: %s\n", strerror(error));
            status = EXIT_FAILURE;
            break;
        }
        receivers_started++;
    }
    if (status == EXIT_FAILURE) {
        for (int i = 0; i < accepted; i++) {
            if (shutdown(ports[i].fd, SHUT_RDWR) == -1 && errno != ENOTCONN) {
                perror("shutdown client");
            }
        }
    }
    for (int i = 0; i < receivers_started; i++) {
        join_worker(receivers[i]);
        if (ports[i].receive_status != EXIT_SUCCESS) {
            status = EXIT_FAILURE;
        }
    }
    for (int i = 0; i < accepted; i++) {
        frame_queue_close(&ports[i].output);
    }
    for (int i = 0; i < senders_started; i++) {
        join_worker(senders[i]);
    }
    for (int i = 0; i < accepted; i++) {
        printf("Port %d: %u dropped deliveries\n",
               i + 1, atomic_load(&ports[i].dropped));
        frame_queue_destroy(&ports[i].output);
        if (close(ports[i].fd) == -1) {
            perror("close client");
            status = EXIT_FAILURE;
        }
    }
    return status;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <tcp_port> <num_ports>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = parse_number(argv[1], 65535);
    int num_ports = parse_number(argv[2], MAX_TEST_PORTS);

    if (port == -1 || num_ports == -1) {
        fprintf(stderr, "Use TCP port 1-65535 and num_ports 1-%d.\n",
                MAX_TEST_PORTS);
        return EXIT_FAILURE;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int status = EXIT_FAILURE;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind");
        goto close_listener;
    }

    if (listen(listener, num_ports) == -1) {
        perror("listen");
        goto close_listener;
    }

    MacTable mac_table;
    if (mac_table_init(&mac_table) != 0) {
        fprintf(stderr, "Could not initialize MAC table\n");
        goto close_listener;
    }

    MacAging aging = {
        .table = &mac_table,
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .wake = PTHREAD_COND_INITIALIZER
    };
    pthread_t aging_thread;

    int error = pthread_create(&aging_thread, NULL, mac_aging_worker, &aging);
    if (error != 0) {
        fprintf(stderr, "pthread_create MAC aging: %s\n", strerror(error));
        status = EXIT_FAILURE;
    } else {
        status = serve_hosts(listener, num_ports, &mac_table);

        check_aging_thread(pthread_mutex_lock(&aging.mutex), "aging lock");
        aging.stop = 1;
        check_aging_thread(pthread_cond_signal(&aging.wake), "aging signal");
        check_aging_thread(pthread_mutex_unlock(&aging.mutex), "aging unlock");

        join_worker(aging_thread);
        if (aging.failed) {
            status = EXIT_FAILURE;
        }
    }

    check_aging_thread(pthread_cond_destroy(&aging.wake),
                       "aging condition destroy");
    check_aging_thread(pthread_mutex_destroy(&aging.mutex),
                       "aging mutex destroy");

    if (mac_table_destroy(&mac_table) != 0) {
        fprintf(stderr, "Could not destroy MAC table\n");
        status = EXIT_FAILURE;
    }

close_listener:
    if (close(listener) == -1) {
        perror("close listener");
        status = EXIT_FAILURE;
    }

    return status;
}