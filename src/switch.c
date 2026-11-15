#include "frame_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>

#define MAX_TEST_PORTS 64

static int parse_number(const char *text, long max)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' ||
        value < 1 || value > max) {
        return -1;
    }

    return (int)value;
}

static int receive_frames(int client)
{
    FrameHeader header;
    uint8_t payload[FRAME_MAX_PAYLOAD];

    for (;;) {
        int result = frame_read(client, &header, payload);

        if (result == 0) {
            return EXIT_SUCCESS;
        }
        if (result < 0) {
            perror("frame_read");
            return EXIT_FAILURE;
        }

        if (printf("Frame: length=%u vlan=%u priority=%u\n",
                   (unsigned)header.length, (unsigned)header.vlan,
                   (unsigned)header.priority) < 0 ||
            fwrite(payload, 1, header.length, stdout) != header.length ||
            putchar('\n') == EOF || fflush(stdout) == EOF) {
            perror("print frame");
            return EXIT_FAILURE;
        }
    }
}

typedef struct {
    int fd;
    int status;
} Receiver;

static void *receiver_thread(void *arg)
{
    Receiver *receiver = arg;
    receiver->status = receive_frames(receiver->fd);
    return NULL;
}

static int serve_hosts(int listener, int num_ports)
{
    Receiver receivers[MAX_TEST_PORTS];
    pthread_t threads[MAX_TEST_PORTS];
    int started = 0;
    int status = EXIT_SUCCESS;

    while (started < num_ports) {
        int client;
        do {
            client = accept(listener, NULL, NULL);
        } while (client == -1 && errno == EINTR);

        if (client == -1) {
            perror("accept");
            status = EXIT_FAILURE;
            break;
        }

        receivers[started] = (Receiver){.fd = client};
        int error = pthread_create(&threads[started], NULL, receiver_thread,
                                   &receivers[started]);
        if (error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            if (close(client) == -1) {
                perror("close client");
            }
            status = EXIT_FAILURE;
            break;
        }

        started++;
    }

    if (status == EXIT_FAILURE) {
        for (int i = 0; i < started; i++) {
            if (shutdown(receivers[i].fd, SHUT_RDWR) == -1 &&
                errno != ENOTCONN) {
                perror("shutdown client");
            }
        }
    }

    for (int i = 0; i < started; i++) {
        int error = pthread_join(threads[i], NULL);
        if (error != 0) {
            fprintf(stderr, "pthread_join: %s\n", strerror(error));
            exit(EXIT_FAILURE);
        }

        if (receivers[i].status != EXIT_SUCCESS) {
            status = EXIT_FAILURE;
        }
        if (close(receivers[i].fd) == -1) {
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

    status = serve_hosts(listener, num_ports);

close_listener:
    if (close(listener) == -1) {
        perror("close listener");
        status = EXIT_FAILURE;
    }

    return status;
}
