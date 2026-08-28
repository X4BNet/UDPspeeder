#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct generator_state {
    int fd;
    struct sockaddr_in destination;
    uint64_t requested_pps;
    uint64_t duration_ns;
    int payload_bytes;
    uint64_t capacity;
    uint64_t *sent_at_ns;
    uint64_t *latency_ns;
    uint8_t *received;
    volatile uint64_t sent;
    volatile uint64_t unique_received;
    volatile uint64_t sender_start_ns;
    volatile uint64_t sender_end_ns;
    volatile int sender_done;
    int sender_cpu;
    int receiver_cpu;
};

static uint64_t now_ns(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000000ULL + value.tv_nsec;
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : a > b;
}

static void pin_current_thread(int cpu) {
    if (cpu < 0) return;
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(cpu, &cpus);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus) != 0) {
        perror("pthread_setaffinity_np");
        exit(1);
    }
}

static void *send_packets(void *opaque) {
    struct generator_state *state = opaque;
    pin_current_thread(state->sender_cpu);
    char *payload = calloc(1, (size_t)state->payload_bytes);
    if (payload == 0) return 0;

    uint64_t start = now_ns() + 100000000ULL;
    state->sender_start_ns = start;
    uint64_t end = start + state->duration_ns;
    uint64_t sequence = 0;
    while (sequence < state->capacity) {
        uint64_t scheduled = start + sequence * 1000000000ULL / state->requested_pps;
        if (now_ns() >= end) break;

        if (scheduled > now_ns()) {
            struct timespec deadline;
            deadline.tv_sec = scheduled / 1000000000ULL;
            deadline.tv_nsec = scheduled % 1000000000ULL;
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, 0) == EINTR) {
            }
        }
        if (now_ns() >= end) break;

        uint32_t wire_sequence = htonl((uint32_t)sequence);
        memcpy(payload, &wire_sequence, sizeof(wire_sequence));
        uint64_t sent_at = now_ns();
        state->sent_at_ns[sequence] = sent_at;
        (void)send(state->fd, payload, (size_t)state->payload_bytes, 0);
        state->sent = sequence + 1;
        sequence++;
    }
    state->sender_end_ns = now_ns();
    state->sender_done = 1;
    free(payload);
    return 0;
}

static void *receive_packets(void *opaque) {
    struct generator_state *state = opaque;
    pin_current_thread(state->receiver_cpu);
    char buffer[4096];
    uint64_t deadline = 0;
    while (!state->sender_done || now_ns() < deadline) {
        if (state->sender_done && deadline == 0) deadline = now_ns() + 1000000000ULL;

        struct pollfd poll_fd;
        poll_fd.fd = state->fd;
        poll_fd.events = POLLIN;
        poll_fd.revents = 0;
        int ready = poll(&poll_fd, 1, 20);
        if (ready <= 0) continue;

        for (;;) {
            int length = recv(state->fd, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (length < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                break;
            }
            if (length < (int)sizeof(uint32_t)) continue;
            uint32_t wire_sequence;
            memcpy(&wire_sequence, buffer, sizeof(wire_sequence));
            uint64_t sequence = ntohl(wire_sequence);
            if (sequence >= state->sent || state->received[sequence]) continue;
            state->received[sequence] = 1;
            uint64_t sent_at = state->sent_at_ns[sequence];
            if (sent_at != 0) {
                state->latency_ns[state->unique_received++] = now_ns() - sent_at;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 6 && argc != 8) {
        fprintf(stderr, "usage: %s target_ip port pps seconds payload_bytes [sender_cpu receiver_cpu]\n", argv[0]);
        return 2;
    }

    struct generator_state state;
    memset(&state, 0, sizeof(state));
    state.requested_pps = strtoull(argv[3], 0, 10);
    uint64_t seconds = strtoull(argv[4], 0, 10);
    state.payload_bytes = atoi(argv[5]);
    if (state.requested_pps == 0 || seconds == 0 || state.payload_bytes < (int)sizeof(uint32_t) || state.payload_bytes > 4096) {
        fprintf(stderr, "invalid rate, duration, or payload size\n");
        return 2;
    }
    state.duration_ns = seconds * 1000000000ULL;
    state.sender_cpu = -1;
    state.receiver_cpu = -1;
    if (argc == 8) {
        state.sender_cpu = atoi(argv[6]);
        state.receiver_cpu = atoi(argv[7]);
    }
    // A missed deadline is caught up with a bounded burst rather than
    // extending the measurement window and silently lowering offered PPS.
    state.capacity = state.requested_pps * seconds * 2 + 1024;
    state.sent_at_ns = calloc(state.capacity, sizeof(*state.sent_at_ns));
    state.latency_ns = calloc(state.capacity, sizeof(*state.latency_ns));
    state.received = calloc(state.capacity, sizeof(*state.received));
    if (!state.sent_at_ns || !state.latency_ns || !state.received) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    state.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (state.fd < 0) {
        perror("socket");
        return 1;
    }
    int socket_buffer = 10 * 1024 * 1024;
    if (setsockopt(state.fd, SOL_SOCKET, SO_RCVBUF, &socket_buffer, sizeof(socket_buffer)) != 0 ||
        setsockopt(state.fd, SOL_SOCKET, SO_SNDBUF, &socket_buffer, sizeof(socket_buffer)) != 0) {
        perror("setsockopt");
        return 1;
    }
    memset(&state.destination, 0, sizeof(state.destination));
    state.destination.sin_family = AF_INET;
    state.destination.sin_port = htons((unsigned short)atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &state.destination.sin_addr) != 1 ||
        connect(state.fd, (struct sockaddr *)&state.destination, sizeof(state.destination)) != 0) {
        perror("connect");
        return 1;
    }

    pthread_t sender;
    pthread_t receiver;
    if (pthread_create(&receiver, 0, receive_packets, &state) != 0 || pthread_create(&sender, 0, send_packets, &state) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        return 1;
    }
    pthread_join(sender, 0);
    pthread_join(receiver, 0);

    qsort(state.latency_ns, state.unique_received, sizeof(*state.latency_ns), compare_u64);
    double elapsed_seconds = (state.sender_end_ns - state.sender_start_ns) / 1000000000.0;
    double actual_pps = elapsed_seconds > 0 ? state.sent / elapsed_seconds : 0;
    double success = state.sent > 0 ? 100.0 * state.unique_received / state.sent : 0;
    uint64_t p50 = state.unique_received ? state.latency_ns[(state.unique_received - 1) * 50 / 100] : 0;
    uint64_t p95 = state.unique_received ? state.latency_ns[(state.unique_received - 1) * 95 / 100] : 0;
    uint64_t p99 = state.unique_received ? state.latency_ns[(state.unique_received - 1) * 99 / 100] : 0;
    printf("sent=%llu received=%llu payload_bytes=%llu success=%.3f%% actual_pps=%.0f payload_mbit=%.2f p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f\n",
           (unsigned long long)state.sent, (unsigned long long)state.unique_received,
           (unsigned long long)(state.sent * (uint64_t)state.payload_bytes), success, actual_pps,
           actual_pps * state.payload_bytes * 8.0 / 1000000.0,
           p50 / 1000000.0, p95 / 1000000.0, p99 / 1000000.0);

    close(state.fd);
    free(state.received);
    free(state.latency_ns);
    free(state.sent_at_ns);
    return 0;
}
