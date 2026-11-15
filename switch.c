#include "frame_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

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

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <tcp_port> <num_ports>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = parse_number(argv[1], 65535);
    int num_ports = parse_number(argv[2], 1);

    if (port == -1 || num_ports == -1) {
        fprintf(stderr, "Use TCP port 1-65535 and num_ports=1 for this step.\n");
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
    if (listen(listener, 1) == -1) {
        perror("listen");
        goto close_listener;
    }

    int client;
    do {
        client = accept(listener, NULL, NULL);
    } while (client == -1 && errno == EINTR);

    if (client == -1) {
        perror("accept");
        goto close_listener;
    }

    status = receive_frames(client);
    if (close(client) == -1) {
        perror("close client");
        status = EXIT_FAILURE;
    }

close_listener:
    if (close(listener) == -1) {
        perror("close listener");
        status = EXIT_FAILURE;
    }
    return status;
}