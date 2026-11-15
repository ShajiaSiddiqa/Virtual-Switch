#include "frame_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int parse_mac(const char *text, uint8_t mac[FRAME_MAC_SIZE])
{
    const char *digits = "0123456789abcdefABCDEF";

    if (strlen(text) != 17) {
        return -1;
    }

    for (size_t i = 0; i < FRAME_MAC_SIZE; i++) {
        size_t offset = i * 3;
        const char *high = strchr(digits, text[offset]);
        const char *low = strchr(digits, text[offset + 1]);

        if (high == NULL || low == NULL ||
            (i < FRAME_MAC_SIZE - 1 && text[offset + 2] != ':')) {
            return -1;
        }

        int high_value = (int)(high - digits);
        int low_value = (int)(low - digits);

        if (high_value >= 16) {
            high_value -= 6;
        }
        if (low_value >= 16) {
            low_value -= 6;
        }

        mac[i] = (uint8_t)(high_value * 16 + low_value);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <switch_ip> <tcp_port> <mac>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *end;
    errno = 0;
    long port = strtol(argv[2], &end, 10);
    if (errno != 0 || end == argv[2] || *end != '\0' ||
        port < 1 || port > 65535) {
        fprintf(stderr, "TCP port must be between 1 and 65535.\n");
        return EXIT_FAILURE;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port)
    };

    int converted = inet_pton(AF_INET, argv[1], &address.sin_addr);
    if (converted != 1) {
        if (converted == 0) {
            fprintf(stderr, "Use a numeric IPv4 address.\n");
        } else {
            perror("inet_pton");
        }
        return EXIT_FAILURE;
    }

    const uint8_t payload[] = "hello";
    FrameHeader header = {
        .length = sizeof(payload) - 1,
        .dst = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        .vlan = 1,
        .priority = 1
    };

    if (parse_mac(argv[3], header.src) != 0) {
        fprintf(stderr, "Use a MAC address such as 02:00:00:00:00:01.\n");
        return EXIT_FAILURE;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int status = EXIT_SUCCESS;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("connect");
        status = EXIT_FAILURE;
    } else if (frame_write(fd, &header, payload) != 0) {
        perror("frame_write");
        status = EXIT_FAILURE;
    }

    if (close(fd) == -1) {
        perror("close");
        status = EXIT_FAILURE;
    }

    return status;
}