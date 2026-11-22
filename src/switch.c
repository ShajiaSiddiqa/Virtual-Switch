#define _POSIX_C_SOURCE 200809L
#include "checked.h"
#include "io.h"
#include "pipe_link.h"
#include "async_logger.h"
#include <stdarg.h>
#include "control.h"
#include "frame_io.h"
#include "mac_table.h"
#include "shm_stats.h"
#include "frame_queue.h"
#include <poll.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
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
#include "signal_control.h"

#define MAX_TEST_PORTS 64
#ifndef MAC_AGE_SECONDS
#define MAC_AGE_SECONDS 5
#endif
#define MAC_SWEEP_SECONDS 1
#define PORT_WAIT_SECONDS 2

#include "rate_limit.h"

#ifndef PORT_RATE_FPS
#define PORT_RATE_FPS 1000
#endif

#ifndef PORT_RATE_BURST
#define PORT_RATE_BURST 10
#endif

/* Published before workers start, cleared only after all producers join. */
static AsyncLogger *runtime_logger;
static atomic_ulong logging_errors;

static void switch_log(const char *format, ...)
{
    int saved = errno;
    va_list args;
    va_start(args, format);
    if (runtime_logger != NULL) {
        if (async_logger_vtry_log(runtime_logger, format, args) == -1)
            atomic_fetch_add_explicit(&logging_errors, 1, memory_order_relaxed);
    } else {
        vfprintf(stderr, format, args);
        size_t length = strlen(format);
        if (length == 0 || format[length - 1] != '\n')
            fputc('\n', stderr);
    }
    va_end(args);
    errno = saved;
}

static void switch_error(const char *operation)
{
    int saved = errno;
    switch_log("%s: %s", operation, strerror(saved));
    errno = saved;
}

static void log_mac_move(void *context, const uint8_t mac[FRAME_MAC_SIZE],
                         uint16_t vlan, int old_port, int new_port)
{
    AsyncLogger *logger = context;
    if (async_logger_try_log(logger,
        "MAC moved: %02x:%02x:%02x:%02x:%02x:%02x VLAN %u port %d -> port %d",
        (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
        (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5],
        (unsigned)vlan, old_port + 1, new_port + 1) == -1)
        atomic_fetch_add_explicit(&logging_errors, 1, memory_order_relaxed);
}

static void log_control_error(void *context, const char *operation, int error)
{
    (void)context;
    switch_log("%s: %s", operation, strerror(error));
}

typedef struct {
    MacTable *table;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    int stop;
    int failed;
    unsigned int max_age_seconds;
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
    PipeLink *pipe_link; /* Reserved physical trunk; never reused by TCP admission. */
    int num_ports;
    RateLimit limiter;
    sem_t *free_ports;
    FrameQueue output;
    StatsHandle *stats;
    struct Port *ports;
    int receive_status;
    atomic_uint dropped;
    MacTable *mac_table;
    pthread_mutex_t *ports_lock;
    int active;
    int disabled;
    uint16_t vlan;
    int trunk;
    int sniffer;
    int mirror_target; /* 1-based destination; zero disables mirroring. */
    pthread_mutex_t transmit_lock;
} Port;

static void record_stats(Port *port, StatsCounter counter)
{
    size_t index = (size_t)(port - port->ports);

    if (shm_stats_add(port->stats, index, counter) == -1) {
        switch_error("shm_stats_add");
        exit(EXIT_FAILURE);
    }
}

typedef struct {
    Port *ports;
    int num_ports;
    pthread_mutex_t *ports_lock;
    MacTable *mac_table;
    sem_t *free_ports;
    pthread_mutex_t *admission_lock;
    pthread_t *sessions;
    int *session_started;
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


    int length = snprintf(response, capacity, "MAC                PORT  VLAN\n");
    if (length < 0 || (size_t)length >= capacity) 
        return -1;

    size_t used = (size_t)length;

    for (size_t i = 0; i < count; i++) {
        const uint8_t *mac = records[i].mac;

        length = snprintf(response + used, capacity - used,
                          "%02x:%02x:%02x:%02x:%02x:%02x  %d  %u\n",
                          (unsigned)mac[0], (unsigned)mac[1],
                          (unsigned)mac[2], (unsigned)mac[3],
                          (unsigned)mac[4], (unsigned)mac[5],
                          records[i].port + 1, (unsigned)records[i].vlan);

        if (length < 0 || (size_t)length >= capacity - used) 
            return admin_message(response, capacity, "Response too large\n", -1);

        used += (size_t)length;
    }

    return 0;
}

static int show_ports(AdminContext *admin, char *response, size_t capacity)
{
    int states[MAX_TEST_PORTS];
    unsigned int vlans[MAX_TEST_PORTS];
    int trunks[MAX_TEST_PORTS];
    int sniffers[MAX_TEST_PORTS];
    int mirrors[MAX_TEST_PORTS];

    int error = pthread_mutex_lock(admin->ports_lock);
    if (error != 0) 
        return admin_message(response, capacity, "Port lock failed\n", -1);

    for (int i = 0; i < admin->num_ports; i++) {
        vlans[i] = admin->ports[i].vlan;
        trunks[i] = admin->ports[i].trunk;
        sniffers[i] = admin->ports[i].sniffer;
        mirrors[i] = admin->ports[i].mirror_target;
        states[i] = admin->ports[i].disabled ? 3 :
                    (admin->ports[i].fd == -1 ? 0 :
                    (admin->ports[i].active ? 1 : 2));
    }

    error = pthread_mutex_unlock(admin->ports_lock);
    if (error != 0) 
        return admin_message(response, capacity, "Port unlock failed\n", -1);

    int length = snprintf(response, capacity, "PORT  STATE  VLAN  MODE  MIRROR\n");
    if (length < 0 || (size_t)length >= capacity) 
        return -1;

    size_t used = (size_t)length;
    const char *names[] = {"FREE", "ACTIVE", "CLOSING", "DOWN"};

    for (int i = 0; i < admin->num_ports; i++) {
        length = snprintf(response + used, capacity - used, "%d  %s  %u  %s  %d\n", i + 1, names[states[i]],
                          vlans[i], sniffers[i] ? "SNIFFER" :
                          (trunks[i] ? "TRUNK" : "ACCESS"), mirrors[i]);

        if (length < 0 || (size_t)length >= capacity - used) 
            return admin_message(response, capacity, "Response too large\n", -1);

        used += (size_t)length;
    }

    return 0;
}

/*
 * Policy changes are serialized by the one admin worker.
 * Lock order: transmit locks ascending -> ports_lock -> MAC locks / queue locks.
 * No sender waits for tokens or queue availability holding transmit_lock.
 */
static int change_vlan_policy(AdminContext *admin, int number, int vlan,
                              int trunk_command, char *response, size_t capacity)
{
    int affected[MAX_TEST_PORTS] = {0};
    int error = pthread_mutex_lock(admin->ports_lock);
    if (error != 0)
        return admin_message(response, capacity, "Port lock failed\n", -1);

    if (number != 0 && admin->ports[number - 1].sniffer) {
        if (pthread_mutex_unlock(admin->ports_lock) != 0)
            abort();
        return admin_message(response, capacity, "Disable mirroring to this sniffer first\n", -1);
    }

    for (int i = 0; i < admin->num_ports; i++) {
        if (admin->ports[i].pipe_link != NULL &&
            ((trunk_command && number != i + 1) || (!trunk_command && number == i + 1))) {
            vsw_pthread(pthread_mutex_unlock(admin->ports_lock), "pthread_mutex_unlock");
            return admin_message(response, capacity, "Pipe trunk is fixed for this launch\n", -1);
        }
    }
    /* Only the single admin thread changes these policy fields. */
    if (trunk_command) {
        for (int i = 0; i < admin->num_ports; i++)
            affected[i] = admin->ports[i].trunk != (i == number - 1);
    } else {
        Port *port = &admin->ports[number - 1];
        affected[number - 1] = port->vlan != vlan || port->trunk;
    }
    if (pthread_mutex_unlock(admin->ports_lock) != 0)
        abort();

    int locked = 0;
    for (; locked < admin->num_ports; locked++) {
        if (!affected[locked])
            continue;
        error = pthread_mutex_lock(&admin->ports[locked].transmit_lock);
        if (error != 0) {
            while (locked > 0) {
                --locked;
                if (affected[locked])
                    vsw_pthread(pthread_mutex_unlock(&admin->ports[locked].transmit_lock), "pthread_mutex_unlock");
            }
            return admin_message(response, capacity, "Transmit lock failed\n", -1);
        }
    }

    error = pthread_mutex_lock(admin->ports_lock);
    if (error != 0) {
        while (locked > 0) {
            --locked;
            if (affected[locked])
                vsw_pthread(pthread_mutex_unlock(&admin->ports[locked].transmit_lock), "pthread_mutex_unlock");
        }
        return admin_message(response, capacity, "Port lock failed\n", -1);
    }

    int result = 0;
    for (int i = 0; i < admin->num_ports; i++) {
        if (affected[i] && mac_table_remove_port(admin->mac_table, i) < 0) {
            result = -1;
            break;
        }
    }

    if (result == 0) {
        for (int i = 0; i < admin->num_ports; i++) {
            Port *port = &admin->ports[i];
            if (!affected[i])
                continue;
            if (port->active) {
                size_t removed = frame_queue_clear(&port->output);
                atomic_fetch_add(&port->dropped, (unsigned int)removed);
                for (size_t j = 0; j < removed; j++)
                    record_stats(port, STATS_FRAMES_DROPPED);
            }
            if (trunk_command)
                port->trunk = i == number - 1;
            else {
                port->trunk = 0;
                port->vlan = (uint16_t)vlan;
            }
        }
    }

    if (pthread_mutex_unlock(admin->ports_lock) != 0)
        abort();
    while (locked > 0) {
        --locked;
        if (affected[locked] &&
            pthread_mutex_unlock(&admin->ports[locked].transmit_lock) != 0)
            abort();
    }

    if (result == 0)
        switch_log("Admin: %s port=%d vlan=%d", trunk_command ? "trunk" : "vlan", number, vlan);
    return admin_message(response, capacity,
        result == 0 ? "VLAN policy updated\n" : "MAC cleanup failed\n", result);
}

/* Only the admin worker changes mirror policy; ports_lock protects readers. */
static int change_mirror(AdminContext *admin, int source, int target,
                          char *response, size_t capacity)
{
    int affected[MAX_TEST_PORTS] = {0};
    if (pthread_mutex_lock(admin->ports_lock) != 0)
        abort();
    Port *port = &admin->ports[source - 1];
    if (port->pipe_link != NULL || port->sniffer || source == target ||
        (target != 0 && (admin->ports[target - 1].trunk ||
                        admin->ports[target - 1].mirror_target != 0))) {
        if (pthread_mutex_unlock(admin->ports_lock) != 0)
            abort();
        return admin_message(response, capacity,
            "Invalid mirror: no self-mirroring, trunk destination, or chains\n", -1);
    }
    if (port->mirror_target == target) {
        if (pthread_mutex_unlock(admin->ports_lock) != 0)
            abort();
        return admin_message(response, capacity, "Mirror unchanged\n", 0);
    }
    if (port->mirror_target != 0)
        affected[port->mirror_target - 1] = 1;
    if (target != 0)
        affected[target - 1] = 1;
    if (pthread_mutex_unlock(admin->ports_lock) != 0)
        abort();

    for (int i = 0; i < admin->num_ports; i++) {
        if (affected[i] && pthread_mutex_lock(&admin->ports[i].transmit_lock) != 0)
            abort();
    }
    if (pthread_mutex_lock(admin->ports_lock) != 0)
        abort();

    int result = 0;
    for (int i = 0; i < admin->num_ports; i++) {
        if (affected[i] && mac_table_remove_port(admin->mac_table, i) < 0) {
            result = -1;
            break;
        }
    }
    if (result == 0) {
        for (int i = 0; i < admin->num_ports; i++) {
            if (affected[i] && admin->ports[i].active) {
                size_t removed = frame_queue_clear(&admin->ports[i].output);
                atomic_fetch_add(&admin->ports[i].dropped, (unsigned int)removed);
                for (size_t j = 0; j < removed; j++)
                    record_stats(&admin->ports[i], STATS_FRAMES_DROPPED);
            }
        }
        port->mirror_target = target;
        for (int i = 0; i < admin->num_ports; i++)
            admin->ports[i].sniffer = 0;
        for (int i = 0; i < admin->num_ports; i++) {
            int destination = admin->ports[i].mirror_target;
            if (destination != 0)
                admin->ports[destination - 1].sniffer = 1;
        }
    }
    if (pthread_mutex_unlock(admin->ports_lock) != 0)
        abort();
    for (int i = admin->num_ports; i > 0; i--) {
        if (affected[i - 1] &&
            pthread_mutex_unlock(&admin->ports[i - 1].transmit_lock) != 0)
            abort();
    }
    if (result == 0)
        switch_log("Admin: mirror source=%d sniffer=%d", source, target);
    return admin_message(response, capacity,
        result == 0 ? "Mirror updated\n" : "MAC cleanup failed\n", result);
}

static void join_worker(pthread_t thread);
static void stop_port_io(Port *port);

/* Admission lock serializes semaphore/slot changes and session joins. */
static int set_port_state(AdminContext *admin, int number, int up,
                           char *response, size_t capacity)
{
    int index = number - 1;
    Port *port = &admin->ports[index];
    if (port->pipe_link != NULL)
        return admin_message(response, capacity, "Pipe trunk is fixed for this launch\n", -1);
    if (pthread_mutex_lock(admin->admission_lock) != 0)
        abort();
    if (pthread_mutex_lock(admin->ports_lock) != 0)
        abort();

    if (up) {
        if (port->disabled) {
            /* shutdown acknowledges only after joining the old session. */
            if (port->fd != -1)
                abort();
            port->disabled = 0;
            if (sem_post(admin->free_ports) == -1)
                abort();
        }
    } else if (!port->disabled) {
        port->disabled = 1;
        if (port->fd == -1) {
            /* An idle enabled slot already contributes exactly one permit. */
            int result;
            do {
                result = sem_trywait(admin->free_ports);
            } while (result == -1 && errno == EINTR);
            if (result == -1)
                abort();
        } else {
            if (port->active) {
                port->active = 0;
                frame_queue_close(&port->output);
                rate_limit_close(&port->limiter);
                size_t removed = frame_queue_clear(&port->output);
                atomic_fetch_add(&port->dropped, (unsigned int)removed);
                for (size_t j = 0; j < removed; j++)
                    record_stats(port, STATS_FRAMES_DROPPED);
            }
            /* Socket remains live until session cleanup takes ports_lock. */
            stop_port_io(port);
        }
    }

    if (pthread_mutex_unlock(admin->ports_lock) != 0)
        abort();

    if (!up && admin->session_started[index]) {
        join_worker(admin->sessions[index]);
        admin->session_started[index] = 0;
    }

    if (pthread_mutex_unlock(admin->admission_lock) != 0)
        abort();
    switch_log("Admin: port %d %s", number, up ? "enabled" : "shut down");
    return admin_message(response, capacity,
        up ? "Port enabled\n" : "Port shut down\n", 0);
}

static int handle_admin(const char *command, char *response, size_t capacity, void *context)
{
    AdminContext *admin = context;

    char port_text[16], vlan_text[16], extra;
    if (strncmp(command, "shutdown ", 9) == 0 || strncmp(command, "up ", 3) == 0) {
        int up = strncmp(command, "up ", 3) == 0;
        const char *argument = command + (up ? 3 : 9);
        if (sscanf(argument, "%15s %c", port_text, &extra) != 1)
            return admin_message(response, capacity, "Usage: shutdown|up <port>\n", -1);
        int number = parse_number(port_text, admin->num_ports);
        if (number < 0)
            return admin_message(response, capacity, "Invalid port\n", -1);
        return set_port_state(admin, number, up, response, capacity);
    }
    if (strncmp(command, "mirror ", 7) == 0) {
        if (sscanf(command, "mirror %15s %15s %c", port_text, vlan_text, &extra) != 2)
            return admin_message(response, capacity, "Usage: mirror <source> <sniffer|0>\n", -1);
        int source = parse_number(port_text, admin->num_ports);
        int target = strcmp(vlan_text, "0") == 0 ? 0 :
                     parse_number(vlan_text, admin->num_ports);
        if (source < 0 || target < 0)
            return admin_message(response, capacity, "Invalid mirror port\n", -1);
        return change_mirror(admin, source, target, response, capacity);
    }
    if (strncmp(command, "vlan ", 5) == 0) {
        if (sscanf(command, "vlan %15s %15s %c", port_text, vlan_text, &extra) != 2)
            return admin_message(response, capacity, "Usage: vlan <port> <vlan>\n", -1);
        int number = parse_number(port_text, admin->num_ports);
        int vlan = parse_number(vlan_text, 4094);
        if (number < 0 || vlan < 0)
            return admin_message(response, capacity, "Invalid port or VLAN\n", -1);
        return change_vlan_policy(admin, number, vlan, 0, response, capacity);
    }
    if (strncmp(command, "trunk ", 6) == 0) {
        if (sscanf(command, "trunk %15s %c", port_text, &extra) != 1)
            return admin_message(response, capacity, "Usage: trunk <port|0>\n", -1);
        int number = strcmp(port_text, "0") == 0 ? 0 :
                     parse_number(port_text, admin->num_ports);
        if (number < 0)
            return admin_message(response, capacity, "Invalid trunk port\n", -1);
        return change_vlan_policy(admin, number, 0, 1, response, capacity);
    }

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

        switch_log("Admin: MAC table cleared");
        return admin_message(response, capacity, "MAC table cleared\n", 0);
    }

    return admin_message(response, capacity, "Unknown command\n", -1);
}

static void mirror_copy(Port *owner, SharedFrame *frame, int copied[MAX_TEST_PORTS])
{
    int destination = owner->mirror_target;
    if (destination == 0 || copied[destination - 1]) return;
    copied[destination - 1] = 1;
    Port *sniffer = &owner->ports[destination - 1];
    if (sniffer->active && frame_queue_try_push(&sniffer->output, frame) == 1) {
        record_stats(sniffer, STATS_FRAMES_MIRRORED);
    } else {
        atomic_fetch_add(&sniffer->dropped, 1);
        record_stats(sniffer, STATS_FRAMES_DROPPED);
    }
}

static void enqueue_frame(Port *target, SharedFrame *frame, int copied[MAX_TEST_PORTS])
{
    if (!target->active || target->sniffer ||
        (!target->trunk && target->vlan != frame->header.vlan)) return;
    if (frame_queue_try_push(&target->output, frame) == 0) {
        atomic_fetch_add(&target->dropped, 1);
        record_stats(target, STATS_FRAMES_DROPPED);
    } else {
        /* Capture accepted egress as well as ingress; never re-mirror a copy. */
        mirror_copy(target, frame, copied);
    }
}

static void flood_frame(Port *source, SharedFrame *frame, int copied[MAX_TEST_PORTS])
{
    for (int i = 0; i < source->num_ports; i++)
        if (&source->ports[i] != source)
            enqueue_frame(&source->ports[i], frame, copied);
}

static int forward_frame_locked(Port *source, SharedFrame *frame)
{
    int ingress = (int)(source - source->ports);
    int destination;
    if (!source->active || source->sniffer ||
        (!source->trunk && source->vlan != frame->header.vlan)) {
        atomic_fetch_add(&source->dropped, 1);
        record_stats(source, STATS_FRAMES_DROPPED);
        return 0;
    }
    int learned = mac_table_insert_vlan(source->mac_table, frame->header.src,
                                         frame->header.vlan, ingress);
    if (learned != 0) return learned;
    int copied[MAX_TEST_PORTS] = {0};
    mirror_copy(source, frame, copied);
    if ((frame->header.dst[0] & 1u) != 0) {
        flood_frame(source, frame, copied);
        return 0;
    }
    int found = mac_table_lookup_vlan(source->mac_table, frame->header.dst,
                                      frame->header.vlan, &destination);
    if (found < 0) return -1;
    if (found == 0) flood_frame(source, frame, copied);
    else if (destination != ingress)
        enqueue_frame(&source->ports[destination], frame, copied);
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
        int result = port->pipe_link != NULL
            ? pipe_link_receive(port->pipe_link, &header, payload)
            : frame_read(port->fd, &header, payload);
        if (result == 0) 
            return NULL;

        if (result < 0) {
            if (port->pipe_link != NULL && errno == ECANCELED)
                return NULL;
            switch_error("frame_read");
            port->receive_status = EXIT_FAILURE;
            return NULL;
        }
        record_stats(port, STATS_FRAMES_IN);

        SharedFrame *frame = shared_frame_create(&header, payload);
        if (frame == NULL) {
            switch_error("shared_frame_create");
            port->receive_status = EXIT_FAILURE;
            return NULL;
        }
        int forward_result = forward_frame(port, frame);
        shared_frame_release(frame);
        if (forward_result == MAC_TABLE_PORT_LIMIT_REACHED) {
            int port_number = (int)(port - port->ports) + 1;
            switch_log( "Port %d shut down: MAC limit exceeded\n", port_number);

            frame_queue_close(&port->output);

            stop_port_io(port);

            return NULL;
        }
        if (forward_result != 0) {
            switch_log( "MAC table operation failed\n");
            port->receive_status = EXIT_FAILURE;
            return NULL;
        }
    }
}

static void *sender_thread(void *arg)
{
    Port *port = arg;
    SharedFrame *frame;

    while (frame_queue_wait(&port->output)) {
        if (!rate_limit_take(&port->limiter))
            break;
        if (pthread_mutex_lock(&port->transmit_lock) != 0)
            abort();
        /* Admin may have flushed the queue while this sender awaited tokens. */
        if (!frame_queue_try_pop(&port->output, &frame)) {
            if (pthread_mutex_unlock(&port->transmit_lock) != 0)
                abort();
            continue;
        }
        int result = port->pipe_link != NULL
            ? pipe_link_send(port->pipe_link, &frame->header, frame->payload)
            : frame_write(port->fd, &frame->header, frame->payload);
        int write_error = errno;
        shared_frame_release(frame);
        if (pthread_mutex_unlock(&port->transmit_lock) != 0)
            abort();

        if (result == 0) {
            record_stats(port, STATS_FRAMES_OUT);
            continue;
        }
        errno = write_error;
        if (port->pipe_link == NULL || errno != ECANCELED)
            switch_error("frame_write");
        frame_queue_close(&port->output);
        stop_port_io(port);
        break;
    }
    return NULL;
}

static void join_worker(pthread_t thread)
{
    int error = pthread_join(thread, NULL);
    if (error != 0) {
        switch_log( "pthread_join: %s\n", strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void check_aging_thread(int error, const char *operation)
{
    if (error != 0) {
        switch_log( "%s: %s\n", operation, strerror(error));
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
            switch_error("clock_gettime");
            aging->failed = 1;
            break;
        }
        deadline.tv_sec += MAC_SWEEP_SECONDS;

        int result = 0;
        while (!aging->stop && result == 0) {
            result = pthread_cond_timedwait(&aging->wake, &aging->mutex, &deadline);
        }

        if (result != 0 && result != ETIMEDOUT) {
            switch_log( "aging wait: %s\n", strerror(result));
            aging->failed = 1;
            break;
        }
        if (aging->stop) 
            break;

        unsigned int max_age = aging->max_age_seconds;
        check_aging_thread(pthread_mutex_unlock(&aging->mutex), "aging unlock");
        int removed = mac_table_expire(aging->table, max_age);
        check_aging_thread(pthread_mutex_lock(&aging->mutex), "aging lock");

        if (removed < 0) {
            switch_log( "MAC aging sweep failed\n");
            aging->failed = 1;
            break;
        }
    }

    check_aging_thread(pthread_mutex_unlock(&aging->mutex), "aging unlock");
    return NULL;
}

static void stop_port_io(Port *port)
{
    if (port->pipe_link != NULL) {
        if (pipe_link_stop(port->pipe_link) == -1)
            switch_error("pipe_link_stop");
        return;
    }
    if (shutdown(port->fd, SHUT_RDWR) == -1 && errno != ENOTCONN) {
        switch_error("shutdown client");
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
        switch_log( "pthread_create sender: %s\n", strerror(error));
        port->receive_status = EXIT_FAILURE;
    }

    check_aging_thread(pthread_mutex_lock(port->ports_lock), "ports lock");
    port->active = 0;
    check_aging_thread(pthread_mutex_unlock(port->ports_lock), "ports unlock");

    frame_queue_close(&port->output);
    rate_limit_close(&port->limiter);
    stop_port_io(port);

    if (error == 0) {
        join_worker(sender);
    }

    rate_limit_destroy(&port->limiter);

    check_aging_thread(pthread_mutex_lock(port->ports_lock), "ports lock");

    if (mac_table_remove_port(port->mac_table, (int)(port - port->ports)) < 0) {
        switch_log( "Could not remove disconnected port's MAC entries\n");
        exit(EXIT_FAILURE);
    }

    frame_queue_destroy(&port->output);

    if (port->pipe_link == NULL && close(port->fd) == -1) {
        switch_error("close client");
    }
    port->fd = -1;

    /* Publish the free slot and its permit atomically with respect to admin. */
    if (port->pipe_link == NULL && !port->disabled && sem_post(port->free_ports) == -1) {
        switch_error("sem_post");
        exit(EXIT_FAILURE);
    }
    check_aging_thread(pthread_mutex_unlock(port->ports_lock), "ports unlock");
    return NULL;
}

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

typedef struct {
    atomic_int stopping;
    int wake_write;
    sem_t *free_ports;
    MacTable *mac_table;
    MacAging *aging;
} SwitchSignals;

static int request_switch_shutdown(void *arg)
{
    SwitchSignals *state = arg;

    if (atomic_exchange(&state->stopping, 1))
        return 0;
    switch_log("SIGINT: graceful shutdown requested");

    int status = 0;
    const char byte = 'S';
    ssize_t result;

    do {
        result = write(state->wake_write, &byte, 1);
    } while (result == -1 && errno == EINTR);

    if (result != 1 && !(result == -1 && errno == EAGAIN)) {
        switch_error("shutdown wake");
        status = -1;
    }

    if (sem_post(state->free_ports) == -1) {
        switch_error("shutdown sem_post");
        status = -1;
    }

    return status;
}

static int dump_switch_mac(void *arg)
{
    SwitchSignals *state = arg;
    char response[CONTROL_RESPONSE_SIZE];

    if (show_mac(state->mac_table, response, sizeof(response)) != 0) {
        switch_log( "Could not snapshot MAC table for dump\n");
        return -1;
    }

    FILE *file = fopen(getenv("SWITCH_DUMP_PATH") != NULL ? getenv("SWITCH_DUMP_PATH") : "mac_dump.txt", "w");
    if (file == NULL) {
        switch_error("open MAC dump");
        return -1;
    }

    int status = 0;

    if (fputs(response, file) == EOF) {
        switch_error("write MAC dump");
        status = -1;
    }

    if (fclose(file) == EOF) {
        switch_error("close MAC dump");
        status = -1;
    }

    if (status == 0) 
        switch_log("MAC table dumped to %s", getenv("SWITCH_DUMP_PATH") != NULL ? getenv("SWITCH_DUMP_PATH") : "mac_dump.txt");

    return status;
}

static int reload_switch_config(void *arg)
{
    SwitchSignals *state = arg;
    FILE *file = fopen(getenv("SWITCH_CONFIG_PATH") != NULL ? getenv("SWITCH_CONFIG_PATH") : "vswitch.conf", "r");

    if (file == NULL) {
        switch_error("open vswitch.conf");
        return -1;
    }
 
    char text[64];
    int valid = fgets(text, sizeof(text), file) != NULL;

    if (valid) {
        text[strcspn(text, "\r\n")] = '\0';

        int extra = fgetc(file);
        if (extra != EOF || ferror(file)) 
            valid = 0;
    }

    if (fclose(file) == EOF) {
        switch_error("close vswitch.conf");
        return -1;
    }

    int seconds = valid ? parse_number(text, 86400) : -1;

    if (seconds == -1) {
        switch_log( "Invalid vswitch.conf: use one integer from 1 to 86400\n");
        return -1;
    }

    int error = pthread_mutex_lock(&state->aging->mutex);
    if (error != 0) {
        switch_log( "config lock: %s\n", strerror(error));
        return -1;
    }

    state->aging->max_age_seconds = (unsigned int)seconds;

    error = pthread_mutex_unlock(&state->aging->mutex);
    if (error != 0) {
        switch_log( "config unlock: %s\n", strerror(error));
        return -1;
    }

    switch_log( "Config reloaded: MAC age = %d seconds\n", seconds);
    return 0;
}

/* Returns 0 handled, 1 shutdown requested, or -1 fatal failure. */
static int admit_client(AdminContext *admin, int fd, atomic_int *stopping)
{
    int status = 0;
    int permit = 0;
    if (pthread_mutex_lock(admin->admission_lock) != 0)
        abort();

    int admitted = wait_for_port(admin->free_ports);
    if (atomic_load(stopping)) {
        status = 1;
        goto close_pending;
    }
    if (admitted != 1) {
        if (admitted == 0) {
            const char reply[] = "switch full";
            if (write_full(fd, reply, sizeof(reply) - 1) != 0)
                switch_error("write switch full");
        } else {
            switch_error("sem_timedwait");
            status = -1;
        }
        goto close_pending;
    }
    permit = 1;

    if (pthread_mutex_lock(admin->ports_lock) != 0)
        abort();
    int slot = 0;
    while (slot < admin->num_ports &&
           (admin->ports[slot].fd != -1 || admin->ports[slot].disabled ||
            admin->ports[slot].pipe_link != NULL))
        slot++;
    if (pthread_mutex_unlock(admin->ports_lock) != 0)
        abort();
    if (slot == admin->num_ports) {
        switch_log( "Free-port semaphore and slots disagree\n");
        status = -1;
        goto close_pending;
    }

    if (admin->session_started[slot]) {
        join_worker(admin->sessions[slot]);
        admin->session_started[slot] = 0;
    }
    const struct timeval send_timeout = {.tv_sec = 2};
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) == -1) {
        switch_error("setsockopt SO_SNDTIMEO");
        status = -1;
        goto close_pending;
    }
    Port *port = &admin->ports[slot];
    int error = rate_limit_init(&port->limiter, PORT_RATE_FPS, PORT_RATE_BURST);
    if (error != 0) {
        switch_log( "rate_limit_init: %s\n", strerror(error));
        status = -1;
        goto close_pending;
    }
    if (pthread_mutex_lock(admin->ports_lock) != 0)
        abort();
    port->fd = fd;
    port->receive_status = EXIT_SUCCESS;
    frame_queue_init(&port->output);
    port->active = 1;
    error = pthread_create(&admin->sessions[slot], NULL, port_session, port);
    if (error != 0) {
        switch_log( "pthread_create session: %s\n", strerror(error));
        port->active = 0;
        frame_queue_destroy(&port->output);
        rate_limit_destroy(&port->limiter);
        port->fd = -1;
        status = -1;
    } else {
        admin->session_started[slot] = 1;
    }
    if (pthread_mutex_unlock(admin->ports_lock) != 0)
        abort();
    if (status == -1)
        goto close_pending;
    if (pthread_mutex_unlock(admin->admission_lock) != 0)
        abort();
    return 0;

close_pending:
    if (close(fd) == -1)
        switch_error("close pending client");
    if (permit && sem_post(admin->free_ports) == -1)
        abort();
    if (pthread_mutex_unlock(admin->admission_lock) != 0)
        abort();
    return status;
}

static int serve_hosts(int listener, int num_ports, MacTable *mac_table, StatsHandle *stats, const sigset_t *signals,MacAging *aging, PipeLink *pipe_link)
{
    Port ports[MAX_TEST_PORTS] = {0};
    pthread_t sessions[MAX_TEST_PORTS];
    int session_started[MAX_TEST_PORTS] = {0};
    pthread_mutex_t ports_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t admission_lock = PTHREAD_MUTEX_INITIALIZER;
    sem_t free_ports;
    int status = EXIT_SUCCESS;

    if (sem_init(&free_ports, 0, (unsigned int)(num_ports - (pipe_link != NULL))) == -1) {
        switch_error("sem_init");
        check_aging_thread(pthread_mutex_destroy(&admission_lock), "admission destroy");
    check_aging_thread(pthread_mutex_destroy(&ports_lock), "ports destroy");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < num_ports; i++) {
        ports[i].fd = -1;
        ports[i].vlan = 1;
        check_aging_thread(pthread_mutex_init(&ports[i].transmit_lock, NULL),
                           "transmit lock init");
        ports[i].ports = ports;
        ports[i].stats = stats;
        ports[i].num_ports = num_ports;
        ports[i].mac_table = mac_table;
        ports[i].free_ports = &free_ports;
        ports[i].ports_lock = &ports_lock;
        atomic_init(&ports[i].dropped, 0);
    }

    AdminContext admin = {
        .ports = ports,
        .num_ports = num_ports,
        .ports_lock = &ports_lock,
        .mac_table = mac_table,
        .free_ports = &free_ports,
        .admission_lock = &admission_lock,
        .sessions = sessions,
        .session_started = session_started
    };

    if (pipe_link != NULL) {
        ports[num_ports - 1].pipe_link = pipe_link;
        ports[num_ports - 1].trunk = 1;
    }

    ControlServer control;

    if (control_start_logged(&control, handle_admin, &admin, log_control_error, NULL) == -1) {
        switch_error("control_start");

        if (sem_destroy(&free_ports) == -1) 
            switch_error("sem_destroy");

        for (int i = 0; i < num_ports; i++)
            check_aging_thread(pthread_mutex_destroy(&ports[i].transmit_lock),
                               "transmit lock destroy");
    check_aging_thread(pthread_mutex_destroy(&admission_lock), "admission destroy");
    check_aging_thread(pthread_mutex_destroy(&ports_lock), "ports destroy");
        return EXIT_FAILURE;
    }

    // struct pollfd listener_poll = {.fd = listener, .events = POLLIN};

    int signal_pipe[2];

    if (pipe(signal_pipe) == -1) {
        switch_error("signal pipe");

        if (control_stop(&control) == -1) {
            switch_error("control_stop");
            exit(EXIT_FAILURE);
        }

        vsw_posix(sem_destroy(&free_ports), "sem_destroy");
        for (int i = 0; i < num_ports; i++)
            check_aging_thread(pthread_mutex_destroy(&ports[i].transmit_lock),
                               "transmit lock destroy");
    check_aging_thread(pthread_mutex_destroy(&admission_lock), "admission destroy");
    check_aging_thread(pthread_mutex_destroy(&ports_lock), "ports destroy");
        return EXIT_FAILURE;
    }

    int flags = fcntl(signal_pipe[1], F_GETFL);

    if (flags == -1 || fcntl(signal_pipe[1], F_SETFL, flags | O_NONBLOCK) == -1) {
        switch_error("signal pipe nonblocking");

        if (control_stop(&control) == -1) {
            switch_error("control_stop");
            exit(EXIT_FAILURE);
        }

        vsw_posix(close(signal_pipe[0]), "close");
        vsw_posix(close(signal_pipe[1]), "close");
        vsw_posix(sem_destroy(&free_ports), "sem_destroy");
        for (int i = 0; i < num_ports; i++)
            check_aging_thread(pthread_mutex_destroy(&ports[i].transmit_lock),
                               "transmit lock destroy");
    check_aging_thread(pthread_mutex_destroy(&admission_lock), "admission destroy");
    check_aging_thread(pthread_mutex_destroy(&ports_lock), "ports destroy");
        return EXIT_FAILURE;
    }

    SwitchSignals signal_state = {
        .wake_write = signal_pipe[1],
        .free_ports = &free_ports,
        .mac_table = mac_table,
        .aging = aging
    };

    atomic_init(&signal_state.stopping, 0);

    SignalControl signal_control;

    if (signal_control_start(
            &signal_control, signals,
            request_switch_shutdown,
            reload_switch_config,
            dump_switch_mac,
            &signal_state) == -1) {
        switch_error("signal_control_start");

        if (control_stop(&control) == -1) {
            switch_error("control_stop");
            exit(EXIT_FAILURE);
        }

        vsw_posix(close(signal_pipe[0]), "close");
        vsw_posix(close(signal_pipe[1]), "close");
        vsw_posix(sem_destroy(&free_ports), "sem_destroy");
        for (int i = 0; i < num_ports; i++)
            check_aging_thread(pthread_mutex_destroy(&ports[i].transmit_lock),
                               "transmit lock destroy");
    check_aging_thread(pthread_mutex_destroy(&admission_lock), "admission destroy");
    check_aging_thread(pthread_mutex_destroy(&ports_lock), "ports destroy");
        return EXIT_FAILURE;
    }

    if (pipe_link != NULL) {
        Port *trunk = &ports[num_ports - 1];
        int error = rate_limit_init(&trunk->limiter, PORT_RATE_FPS, PORT_RATE_BURST);
        if (error != 0) {
            switch_log("Pipe rate limiter: %s", strerror(error));
            status = EXIT_FAILURE;
        } else {
            check_aging_thread(pthread_mutex_lock(&ports_lock), "ports lock");
            trunk->fd = pipe_link->read_fd;
            frame_queue_init(&trunk->output);
            trunk->active = 1;
            error = pthread_create(&sessions[num_ports - 1], NULL, port_session, trunk);
            if (error != 0) {
                trunk->active = 0;
                trunk->fd = -1;
                frame_queue_destroy(&trunk->output);
                rate_limit_destroy(&trunk->limiter);
                status = EXIT_FAILURE;
            } else {
                session_started[num_ports - 1] = 1;
            }
            check_aging_thread(pthread_mutex_unlock(&ports_lock), "ports unlock");
        }
    }

    struct pollfd events[2] = {
        {.fd = listener, .events = POLLIN},
        {.fd = signal_pipe[0], .events = POLLIN}
    };

    for (; status == EXIT_SUCCESS;) {
             
        if (atomic_load(&signal_state.stopping)) 
            break;

        int ready = poll(events, 2, -1);

        if (ready == -1 && errno == EINTR) 
            continue;

        if (ready == -1) {
            switch_error("poll");
            status = EXIT_FAILURE;
            break;
        }

        if (atomic_load(&signal_state.stopping)) 
            break;

        if (events[1].revents != 0) {
            switch_log( "Unexpected shutdown pipe event\n");
            status = EXIT_FAILURE;
            break;
        }

        if ((events[0].revents & POLLIN) == 0) {
            switch_log( "Listener stopped accepting connections\n");
            status = EXIT_FAILURE;
            break;
        }


        int fd = accept(listener, NULL, NULL);
        if (fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            switch_error("accept");
            status = EXIT_FAILURE;
            break;
        }

        int result = admit_client(&admin, fd, &signal_state.stopping);
        if (result != 0) {
            if (result < 0)
                status = EXIT_FAILURE;
            break;
        }
    }

    if (signal_control_stop(&signal_control) == -1) {
        switch_error("signal_control_stop");
        status = EXIT_FAILURE;

        if (signal_control.started) 
            exit(EXIT_FAILURE);
    }

    if (close(signal_pipe[0]) == -1) {
        switch_error("close signal pipe");
        status = EXIT_FAILURE;
    }

    if (close(signal_pipe[1]) == -1) {
        switch_error("close signal pipe");
        status = EXIT_FAILURE;
    }


    if (control_stop(&control) == -1) {
        switch_error("control_stop");
        exit(EXIT_FAILURE);
    }

    check_aging_thread(pthread_mutex_lock(&ports_lock), "ports lock");
// cleanup loop
    for (int i = 0; i < num_ports; i++) {
        if (ports[i].fd != -1) {
            /* A closing session may already have destroyed its limiter. */
            if (ports[i].active) {
                ports[i].active = 0;
                frame_queue_close(&ports[i].output);
                rate_limit_close(&ports[i].limiter);
            }
            stop_port_io(&ports[i]);
        }
    }

    check_aging_thread(pthread_mutex_unlock(&ports_lock), "ports unlock");

    for (int i = 0; i < num_ports; i++) {
        if (session_started[i]) 
            join_worker(sessions[i]);

        vsw_posix(printf("Port %d: %u dropped deliveries\n", i + 1, atomic_load(&ports[i].dropped)), "printf");
    }

    if (sem_destroy(&free_ports) == -1) {
        switch_error("sem_destroy");
        status = EXIT_FAILURE;
    }

    for (int i = 0; i < num_ports; i++)
            check_aging_thread(pthread_mutex_destroy(&ports[i].transmit_lock),
                               "transmit lock destroy");
    check_aging_thread(pthread_mutex_destroy(&admission_lock), "admission destroy");
    check_aging_thread(pthread_mutex_destroy(&ports_lock), "ports destroy");
    return status;
}

static int persist_mac_image(void *context, const MacStoreImage *image)
{
    return mac_store_publish(context, image->entries, image->count, image->use_sequence);
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        switch_log( "Usage: %s <tcp_port> <num_ports>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = parse_number(argv[1], 65535);
    int num_ports = parse_number(argv[2], MAX_TEST_PORTS);

    if (port == -1 || num_ports == -1) {
        switch_log( "Use TCP port 1-65535 and num_ports 1-%d.\n", MAX_TEST_PORTS);
        return EXIT_FAILURE;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        switch_error("signal");
        return EXIT_FAILURE;
    }

    sigset_t managed_signals;

    if (signal_control_block(&managed_signals) == -1) {
        switch_error("signal_control_block");
        return EXIT_FAILURE;
    }

    PipeLink inherited_link;
    PipeLink *pipe_link = NULL;
    const char *read_text = getenv("SWITCH_PIPE_READ_FD");
    const char *write_text = getenv("SWITCH_PIPE_WRITE_FD");
    if (read_text != NULL || write_text != NULL) {
        char *end_read = NULL, *end_write = NULL;
        errno = 0;
        long read_fd = read_text != NULL ? strtol(read_text, &end_read, 10) : -1;
        int read_error = errno;
        errno = 0;
        long write_fd = write_text != NULL ? strtol(write_text, &end_write, 10) : -1;
        if (read_error || errno || read_text == NULL || write_text == NULL ||
            end_read == read_text || end_write == write_text || *end_read || *end_write ||
            read_fd < 0 || write_fd < 0 || read_fd > INT_MAX || write_fd > INT_MAX ||
            num_ports < 2) {
            vsw_posix(fprintf(stderr, "Invalid inherited pipe descriptors or port count\n"), "fprintf");
            return EXIT_FAILURE;
        }
        if (pipe_link_adopt(&inherited_link, (int)read_fd, (int)write_fd) == -1) {
            perror("pipe_link_adopt");
            return EXIT_FAILURE;
        }
        pipe_link = &inherited_link;
    }

    AsyncLogger logger;
    const char *log_path = getenv("SWITCH_LOG_PATH");
    if (log_path == NULL)
        log_path = "switch.log";
    if (async_logger_start(&logger, log_path) == -1) {
        perror("async_logger_start");
        if (pipe_link != NULL) vsw_zero(pipe_link_destroy(pipe_link), "pipe_link_destroy");
        return EXIT_FAILURE;
    }
    atomic_init(&logging_errors, 0);
    runtime_logger = &logger;
    switch_log("Switch starting: tcp_port=%d ports=%d", port, num_ports);

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        switch_error("socket");
        if (pipe_link != NULL) vsw_zero(pipe_link_destroy(pipe_link), "pipe_link_destroy");
        runtime_logger = NULL;
        if (async_logger_stop(&logger) == -1)
            perror("async_logger_stop");
        return EXIT_FAILURE;
    }

    int status = EXIT_FAILURE;
    MacStore recovery_store = {.fd = -1};
    int recovery_attached = 0;
    int reuse_address = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                   sizeof(reuse_address)) == -1) {
        switch_error("setsockopt SO_REUSEADDR");
        goto close_listener;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1) {
        switch_error("bind");
        goto close_listener;
    }

    if (listen(listener, num_ports) == -1) {
        switch_error("listen");
        goto close_listener;
    }

    MacTable mac_table;
    if (mac_table_init(&mac_table) != 0) {
        switch_log( "Could not initialize MAC table\n");
        goto close_listener;
    }
    mac_table_set_move_logger(&mac_table, log_mac_move, &logger);
    const char *store_name = getenv("VSWITCH_MAC_STORE");
    if (store_name != NULL) {
        MacStoreImage image;
        if (mac_store_attach(&recovery_store, store_name) != 0) {
            switch_error("attach recovery store");
            vsw_zero(mac_table_destroy(&mac_table), "mac_table_destroy");
            goto close_listener;
        }
        recovery_attached = 1;
        if (mac_store_read(&recovery_store, &image) != 0 ||
            mac_table_restore(&mac_table, &image) != 0) {
            switch_error("restore MAC image");
            vsw_zero(mac_table_destroy(&mac_table), "mac_table_destroy");
            goto close_listener;
        }
        mac_table_set_checkpoint(&mac_table, persist_mac_image, &recovery_store);
        switch_log("Recovered MAC image: generation=%llu entries=%u",
                   (unsigned long long)image.generation, image.count);
    }
    StatsHandle stats;

    if (shm_stats_create(&stats, (size_t)num_ports) == -1) {
        switch_error("shm_stats_create");

        if (mac_table_destroy(&mac_table) != 0) 
            switch_log( "Could not destroy MAC table\n");

        goto close_listener;
    }


    // sigset_t stop_signals;
    // if (sigemptyset(&stop_signals) == -1 ||
    //     sigaddset(&stop_signals, SIGINT) == -1) {
    //     switch_error("configure SIGINT");
    //     return EXIT_FAILURE;
    // }

    // int signal_error = pthread_sigmask(SIG_BLOCK, &stop_signals, NULL);
    // if (signal_error != 0) {
    //     switch_log( "pthread_sigmask: %s\n", strerror(signal_error));
    //     return EXIT_FAILURE;
    // }

    MacAging aging = {
        .table = &mac_table,
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .wake = PTHREAD_COND_INITIALIZER,
        .max_age_seconds = MAC_AGE_SECONDS
    };
    pthread_t aging_thread;

    int error = pthread_create(&aging_thread, NULL, mac_aging_worker, &aging);
    if (error != 0) {
        switch_log( "pthread_create MAC aging: %s\n", strerror(error));
        status = EXIT_FAILURE;
    } else {
        status = serve_hosts(listener, num_ports, &mac_table, &stats,&managed_signals, &aging, pipe_link);
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
        switch_log( "Could not destroy MAC table\n");
        status = EXIT_FAILURE;
    }

    if (shm_stats_close(&stats) == -1) {
        switch_error("shm_stats_close");
        status = EXIT_FAILURE;
    }

close_listener:
    if (recovery_attached && mac_store_close(&recovery_store) != 0) {
        switch_error("close recovery store");
        status = EXIT_FAILURE;
    }
    if (pipe_link != NULL && pipe_link_destroy(pipe_link) == -1) {
        switch_error("pipe_link_destroy");
        status = EXIT_FAILURE;
    }
    if (close(listener) == -1) {
        switch_error("close listener");
        status = EXIT_FAILURE;
    }

    switch_log("Switch stopped: status=%d", status);
    /* Every signal/admin/aging/session producer has finished at this point. */
    unsigned long dropped_logs = async_logger_dropped(&logger);
    unsigned long failed_logs = atomic_load_explicit(&logging_errors, memory_order_relaxed);
    runtime_logger = NULL;
    if (async_logger_stop(&logger) == -1) {
        perror("async_logger_stop");
        status = EXIT_FAILURE;
    }
    vsw_posix(printf("Logger: %lu dropped messages, %lu submission errors\n",
           dropped_logs, failed_logs), "printf");
    return status;
}