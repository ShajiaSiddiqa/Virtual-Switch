#include "frame_io.h"
#include "frame_queue.h"

#include <stdatomic.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>

#define MAX_TEST_PORTS 64

static int parse_number(const char *text, long max)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > max) 
        return -1;

    return (int)value;
}


// typedef struct {
//     int fd;
//     int status;
//     const int *clients;
//     int num_ports;
// } Receiver;

typedef struct Port {
    int fd;
    int receive_status;
    struct Port *ports;
    int num_ports;
    FrameQueue output;
    atomic_uint dropped;
} Port;

// static pthread_mutex_t flood_lock = PTHREAD_MUTEX_INITIALIZER;

// static int flood_frame(const Receiver *source, const FrameHeader *header, const uint8_t *payload)
// {
//     int error = pthread_mutex_lock(&flood_lock);
//     if (error != 0) {
//         fprintf(stderr, "pthread_mutex_lock: %s\n", strerror(error));
//         return EXIT_FAILURE;
//     }

//     for (int i = 0; i < source->num_ports; i++) {
//         int target = source->clients[i];

//         if (target == source->fd) {
//             continue;
//         }

//         if (frame_write(target, header, payload) != 0) {
//             perror("frame_write");
//         }
//     }

//     error = pthread_mutex_unlock(&flood_lock);
//     if (error != 0) {
//         fprintf(stderr, "pthread_mutex_unlock: %s\n", strerror(error));
//         return EXIT_FAILURE;
//     }

//     return EXIT_SUCCESS;
// }

// static int receive_frames(const Receiver *receiver)
// {
//     FrameHeader header;
//     uint8_t payload[FRAME_MAX_PAYLOAD];

//     for (;;) {
//         int result = frame_read(receiver->fd, &header, payload);

//         if (result == 0) {
//             return EXIT_SUCCESS;
//         }

//         if (result < 0) {
//             perror("frame_read");
//             return EXIT_FAILURE;
//         }

//         if (printf("Frame: length=%u vlan=%u priority=%u\n",
//                    (unsigned)header.length, (unsigned)header.vlan,
//                    (unsigned)header.priority) < 0 ||
//             fwrite(payload, 1, header.length, stdout) != header.length ||
//             putchar('\n') == EOF || fflush(stdout) == EOF) {
//             perror("print frame");
//             return EXIT_FAILURE;
//         }

//         if (flood_frame(receiver, &header, payload) != EXIT_SUCCESS) {
//             return EXIT_FAILURE;
//         }
//     }
// }

// static void *receiver_thread(void *arg)
// {
//     Receiver *receiver = arg;
//     receiver->status = receive_frames(receiver);
//     return NULL;
// }

static void flood_frame(Port *source, const QueuedFrame *frame)
{
    for (int i = 0; i < source->num_ports; i++) {
        Port *target = &source->ports[i];

        if (target == source) {
            continue;
        }

        if (frame_queue_try_push(&target->output, frame) == 0) {
            atomic_fetch_add(&target->dropped, 1);
        }
    }
}

static void *receiver_thread(void *arg)
{
    Port *port = arg;
    QueuedFrame frame = {0};

    for (;;) {
        int result = frame_read(port->fd, &frame.header, frame.payload);

        if (result == 0) {
            return NULL;
        }

        if (result < 0) {
            perror("frame_read");
            port->receive_status = EXIT_FAILURE;
            return NULL;
        }

        flood_frame(port, &frame);
    }
}

static void *sender_thread(void *arg)
{
    Port *port = arg;
    QueuedFrame frame;

    while (frame_queue_pop(&port->output, &frame) == 1) {
        if (frame_write(port->fd, &frame.header, frame.payload) != 0) {
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

// static int serve_hosts(int listener, int num_ports)
// {
//     int clients[MAX_TEST_PORTS];
//     Receiver receivers[MAX_TEST_PORTS];
//     pthread_t threads[MAX_TEST_PORTS];
//     int accepted = 0;
//     int started = 0;
//     int status = EXIT_SUCCESS;

//     while (accepted < num_ports) {
//         int client;
//         do {
//             client = accept(listener, NULL, NULL);
//         } while (client == -1 && errno == EINTR);

//         if (client == -1) {
//             perror("accept");
//             status = EXIT_FAILURE;
//             break;
//         }

//         clients[accepted++] = client;
//     }

//     while (status == EXIT_SUCCESS && started < accepted) {
//         receivers[started] = (Receiver){
//             .fd = clients[started],
//             .clients = clients,
//             .num_ports = accepted
//         };

//         int error = pthread_create(&threads[started], NULL, receiver_thread,
//                                    &receivers[started]);
//         if (error != 0) {
//             fprintf(stderr, "pthread_create: %s\n", strerror(error));
//             status = EXIT_FAILURE;
//             break;
//         }

//         started++;
//     }

//     if (status == EXIT_FAILURE) {
//         for (int i = 0; i < accepted; i++) {
//             if (shutdown(clients[i], SHUT_RDWR) == -1 &&
//                 errno != ENOTCONN) {
//                 perror("shutdown client");
//             }
//         }
//     }

//     for (int i = 0; i < started; i++) {
//         int error = pthread_join(threads[i], NULL);
//         if (error != 0) {
//             fprintf(stderr, "pthread_join: %s\n", strerror(error));
//             exit(EXIT_FAILURE);
//         }

//         if (receivers[i].status != EXIT_SUCCESS) {
//             status = EXIT_FAILURE;
//         }
//     }

//     for (int i = 0; i < accepted; i++) {
//         if (close(clients[i]) == -1) {
//             perror("close client");
//             status = EXIT_FAILURE;
//         }
//     }

//     return status;
// }

static void join_worker(pthread_t thread)
{
    int error = pthread_join(thread, NULL);

    if (error != 0) {
        fprintf(stderr, "pthread_join: %s\n", strerror(error));
        exit(EXIT_FAILURE);
    }
}

static int serve_hosts(int listener, int num_ports)
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
    if (argc != 3) 
    {
        fprintf(stderr, "Usage: %s <tcp_port> <num_ports>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = parse_number(argv[1], 65535);
    int num_ports = parse_number(argv[2], MAX_TEST_PORTS);

    if (port == -1 || num_ports == -1) 
    {
        fprintf(stderr, "Use TCP port 1-65535 and num_ports 1-%d.\n", MAX_TEST_PORTS);
        return EXIT_FAILURE;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) 
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    int status = EXIT_FAILURE;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1) 
    {
        perror("bind");
        goto close_listener;
    }

    if (listen(listener, num_ports) == -1) 
    {
        perror("listen");
        goto close_listener;
    }

    status = serve_hosts(listener, num_ports);

close_listener:
    if (close(listener) == -1) 
    {
        perror("close listener");
        status = EXIT_FAILURE;
    }

    return status;
}
