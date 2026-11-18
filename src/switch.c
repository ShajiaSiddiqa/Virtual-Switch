#define _POSIX_C_SOURCE 200809L
#include "io.h"
#include "control.h"
#include "frame_io.h"
#include "mac_table.h"
#include "frame_queue.h"
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define MAX_TEST_PORTS 64
#define MAC_AGE_SECONDS 5
#define MAC_SWEEP_SECONDS 1
#define PORT_WAIT_SECONDS 2


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
    sem_t *free_ports;
    FrameQueue output;
    struct Port *ports;
    int receive_status;
    atomic_uint dropped;
    MacTable *mac_table;
    pthread_mutex_t *ports_lock;
int active;
} Port;

typedef struct {
    Port *ports;
    int num_ports;
    pthread_mutex_t *ports_lock;
    MacTable *mac_table;
} AdminContext;

static int admin_message(char *response, size_t capacity,  const char *message, int result)
{
    int length = snprintf(response, capacity, "%s", message);

    if (length < 0 || (size_t)length >= capacity) 
        return -1;

    return result;
}

static int show_mac(MacTable *table, char *response, size_t capacity)
{
    MacRecord records[MAC_TABLE_CAPACITY];
    size_t count;

    if (mac_table_snapshot(table, records, MAC_TABLE_CAPACITY, &count) != 0) 
        return admin_message(response, capacity, "MAC snapshot failed\n", -1);


    int length = snprintf(response, capacity, "MAC                PORT\n");
    if (length < 0 || (size_t)length >= capacity) 
        return -1;

    size_t used = (size_t)length;

    for (size_t i = 0; i < count; i++) {
        const uint8_t *mac = records[i].mac;

        length = snprintf(response + used, capacity - used,
                          "%02x:%02x:%02x:%02x:%02x:%02x  %d\n",
                          (unsigned)mac[0], (unsigned)mac[1],
                          (unsigned)mac[2], (unsigned)mac[3],
                          (unsigned)mac[4], (unsigned)mac[5],
                          records[i].port + 1);

        if (length < 0 || (size_t)length >= capacity - used) 
            return admin_message(response, capacity, "Response too large\n", -1);

        used += (size_t)length;
    }

    return 0;
}

static int show_ports(AdminContext *admin, char *response, size_t capacity)
{
    int states[MAX_TEST_PORTS];

    int error = pthread_mutex_lock(admin->ports_lock);
    if (error != 0) 
        return admin_message(response, capacity, "Port lock failed\n", -1);

    for (int i = 0; i < admin->num_ports; i++) {
        states[i] = admin->ports[i].fd == -1 ? 0 :
                    (admin->ports[i].active ? 1 : 2);
    }

    error = pthread_mutex_unlock(admin->ports_lock);
    if (error != 0) 
        return admin_message(response, capacity, "Port unlock failed\n", -1);

    int length = snprintf(response, capacity, "PORT  STATE\n");
    if (length < 0 || (size_t)length >= capacity) 
        return -1;

    size_t used = (size_t)length;
    const char *names[] = {"FREE", "ACTIVE", "CLOSING"};

    for (int i = 0; i < admin->num_ports; i++) {
        length = snprintf(response + used, capacity - used, "%d  %s\n", i + 1, names[states[i]]);

        if (length < 0 || (size_t)length >= capacity - used) 
            return admin_message(response, capacity, "Response too large\n", -1);

        used += (size_t)length;
    }

    return 0;
}

static int handle_admin(const char *command, char *response, size_t capacity, void *context)
{
    AdminContext *admin = context;

    if (strcmp(command, "show mac") == 0) 
        return show_mac(admin->mac_table, response, capacity);

    if (strcmp(command, "show ports") == 0) 
        return show_ports(admin, response, capacity);

    if (strcmp(command, "clear mac") == 0) {
        int error = pthread_mutex_lock(admin->ports_lock);
        if (error != 0) 
            return admin_message(response, capacity, "Port lock failed\n", -1);

        int result = mac_table_clear(admin->mac_table);
        error = pthread_mutex_unlock(admin->ports_lock);

        if (result != 0 || error != 0) 
            return admin_message(response, capacity, "MAC clear failed\n", -1);

        return admin_message(response, capacity, "MAC table cleared\n", 0);
    }

    return admin_message(response, capacity, "Unknown command\n", -1);
}

static void enqueue_frame(Port *target, SharedFrame *frame)
{
    if (!target->active) 
        return;

    if (frame_queue_try_push(&target->output, frame) == 0) 
        atomic_fetch_add(&target->dropped, 1);
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

static int forward_frame_locked(Port *source, SharedFrame *frame)
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

static int forward_frame(Port *source, SharedFrame *frame)
{
    if (pthread_mutex_lock(source->ports_lock) != 0) 
        return -1;

    int result = forward_frame_locked(source, frame);

    if (pthread_mutex_unlock(source->ports_lock) != 0) 
        return -1;

    return result;
}

static void *receiver_thread(void *arg)
{
    Port *port = arg;
    FrameHeader header;
    uint8_t payload[FRAME_MAX_PAYLOAD];
    for (;;) {
        int result = frame_read(port->fd, &header, payload);
        if (result == 0) 
            return NULL;

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
            result = pthread_cond_timedwait(&aging->wake, &aging->mutex, &deadline);
        }

        if (result != 0 && result != ETIMEDOUT) {
            fprintf(stderr, "aging wait: %s\n", strerror(result));
            aging->failed = 1;
            break;
        }
        if (aging->stop) 
            break;

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

static void stop_port_io(int fd)
{
    if (shutdown(fd, SHUT_RDWR) == -1 && errno != ENOTCONN) {
        perror("shutdown client");
    }
}

static void *port_session(void *arg)
{
    Port *port = arg;
    pthread_t sender;

    int error = pthread_create(&sender, NULL, sender_thread, port);
    if (error == 0) {
        receiver_thread(port);
    } else {
        fprintf(stderr, "pthread_create sender: %s\n", strerror(error));
        port->receive_status = EXIT_FAILURE;
    }

    check_aging_thread(pthread_mutex_lock(port->ports_lock), "ports lock");
    port->active = 0;
    check_aging_thread(pthread_mutex_unlock(port->ports_lock), "ports unlock");

    frame_queue_close(&port->output);
    stop_port_io(port->fd);

    if (error == 0) {
        join_worker(sender);
    }

    check_aging_thread(pthread_mutex_lock(port->ports_lock), "ports lock");

    if (mac_table_remove_port(port->mac_table, (int)(port - port->ports)) < 0) {
        fprintf(stderr, "Could not remove disconnected port's MAC entries\n");
        exit(EXIT_FAILURE);
    }

    frame_queue_destroy(&port->output);

    if (close(port->fd) == -1) {
        perror("close client");
    }
    port->fd = -1;

    check_aging_thread(pthread_mutex_unlock(port->ports_lock), "ports unlock");

    if (sem_post(port->free_ports) == -1) {
        perror("sem_post");
        exit(EXIT_FAILURE);
    }

    return NULL;
}

// Returns 1 for a permit, 0 for timeout, or -1 on error. 
static int wait_for_port(sem_t *free_ports)
{
    struct timespec deadline;

    if (clock_gettime(CLOCK_REALTIME, &deadline) == -1) {
        return -1;
    }
    deadline.tv_sec += PORT_WAIT_SECONDS;

    int result;
    do {
        result = sem_timedwait(free_ports, &deadline);
    } while (result == -1 && errno == EINTR);

    if (result == 0) 
        return 1;

    return errno == ETIMEDOUT ? 0 : -1;
}

static int serve_hosts(int listener, int num_ports, MacTable *mac_table)
{
    Port ports[MAX_TEST_PORTS] = {0};
    pthread_t sessions[MAX_TEST_PORTS];
    int session_started[MAX_TEST_PORTS] = {0};
    pthread_mutex_t ports_lock = PTHREAD_MUTEX_INITIALIZER;
    sem_t free_ports;
    int status = EXIT_SUCCESS;

    if (sem_init(&free_ports, 0, (unsigned int)num_ports) == -1) {
        perror("sem_init");
        check_aging_thread(pthread_mutex_destroy(&ports_lock), "ports destroy");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < num_ports; i++) {
        ports[i].fd = -1;
        ports[i].num_ports = num_ports;
        ports[i].ports = ports;
        ports[i].ports_lock = &ports_lock;
        ports[i].free_ports = &free_ports;
        ports[i].mac_table = mac_table;
        atomic_init(&ports[i].dropped, 0);
    }

    AdminContext admin = {
        .ports = ports,
        .num_ports = num_ports,
        .ports_lock = &ports_lock,
        .mac_table = mac_table
    };

    ControlServer control;

    if (control_start(&control, handle_admin, &admin) == -1) {
        perror("control_start");

        if (sem_destroy(&free_ports) == -1) 
            perror("sem_destroy");

        check_aging_thread(pthread_mutex_destroy(&ports_lock), "ports destroy");
        return EXIT_FAILURE;
    }

    struct pollfd listener_poll = {.fd = listener, .events = POLLIN};

    for (;;) {
        sigset_t pending;
        if (sigpending(&pending) == -1) {
            perror("sigpending");
            status = EXIT_FAILURE;
            break;
        }
        if (sigismember(&pending, SIGINT) == 1) 
            break;

        int ready = poll(&listener_poll, 1, 200);
        if (ready == 0 || (ready == -1 && errno == EINTR)) {
            continue;
        }
        if (ready == -1) {
            perror("poll");
            status = EXIT_FAILURE;
            break;
        }
        if ((listener_poll.revents & POLLIN) == 0) {
            fprintf(stderr, "Listener stopped accepting connections\n");
            status = EXIT_FAILURE;
            break;
        }

        int fd = accept(listener, NULL, NULL);
        if (fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            status = EXIT_FAILURE;
            break;
        }

        int admitted = wait_for_port(&free_ports);
        if (admitted != 1) {
            if (admitted == 0) {
                const char reply[] = "switch full";
                if (write_full(fd, reply, sizeof(reply) - 1) != 0) {
                    perror("write switch full");
                }
            } else {
                perror("sem_timedwait");
                status = EXIT_FAILURE;
            }

            if (close(fd) == -1) 
                perror("close pending client");
            if (admitted < 0) 
                break;

            continue;
        }

        check_aging_thread(pthread_mutex_lock(&ports_lock), "ports lock");

        int slot = 0;
        while (slot < num_ports && ports[slot].fd != -1) {
            slot++;
        }

        check_aging_thread(pthread_mutex_unlock(&ports_lock), "ports unlock");

        if (slot == num_ports) {
            fprintf(stderr, "Free-port semaphore and slots disagree\n");
            if (close(fd) == -1) {
                perror("close pending client");
            }
            status = EXIT_FAILURE;
            break;
        }

        if (session_started[slot]) {
            join_worker(sessions[slot]);
            session_started[slot] = 0;
        }

        const struct timeval send_timeout = {.tv_sec = 2};
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                       &send_timeout, sizeof(send_timeout)) == -1) {
            perror("setsockopt SO_SNDTIMEO");
            if (close(fd) == -1) 
                perror("close client");
            status = EXIT_FAILURE;
            break;
        }

        Port *port = &ports[slot];
        check_aging_thread(pthread_mutex_lock(&ports_lock), "ports lock");

        port->fd = fd;
        port->receive_status = EXIT_SUCCESS;
        frame_queue_init(&port->output);
        port->active = 1;

        int error = pthread_create(&sessions[slot], NULL, port_session, port);
        if (error != 0) {
            fprintf(stderr, "pthread_create session: %s\n", strerror(error));
            port->active = 0;
            frame_queue_destroy(&port->output);
            if (close(fd) == -1) {
                perror("close client");
            }
            port->fd = -1;
            status = EXIT_FAILURE;
        } else {
            session_started[slot] = 1;
        }

        check_aging_thread(pthread_mutex_unlock(&ports_lock), "ports unlock");

        if (status != EXIT_SUCCESS) 
            break;
    }

    if (control_stop(&control) == -1) {
        perror("control_stop");
        exit(EXIT_FAILURE);
    }

    check_aging_thread(pthread_mutex_lock(&ports_lock), "ports lock");

    for (int i = 0; i < num_ports; i++) {
        if (ports[i].fd != -1) {
            ports[i].active = 0;
            stop_port_io(ports[i].fd);
        }
    }

    check_aging_thread(pthread_mutex_unlock(&ports_lock), "ports unlock");

    for (int i = 0; i < num_ports; i++) {
        if (session_started[i]) 
            join_worker(sessions[i]);

        printf("Port %d: %u dropped deliveries\n", i + 1, atomic_load(&ports[i].dropped));
    }

    if (sem_destroy(&free_ports) == -1) {
        perror("sem_destroy");
        status = EXIT_FAILURE;
    }

    check_aging_thread(pthread_mutex_destroy(&ports_lock), "ports destroy");
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
        fprintf(stderr, "Use TCP port 1-65535 and num_ports 1-%d.\n", MAX_TEST_PORTS);
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

    sigset_t stop_signals;
    if (sigemptyset(&stop_signals) == -1 ||
        sigaddset(&stop_signals, SIGINT) == -1) {
        perror("configure SIGINT");
        return EXIT_FAILURE;
    }

    int signal_error = pthread_sigmask(SIG_BLOCK, &stop_signals, NULL);
    if (signal_error != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(signal_error));
        return EXIT_FAILURE;
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

    check_aging_thread(pthread_cond_destroy(&aging.wake), "aging condition destroy");
    check_aging_thread(pthread_mutex_destroy(&aging.mutex), "aging mutex destroy");

    if (mac_table_destroy(&mac_table) != 0) 
    {
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