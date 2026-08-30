#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { sink_batch_size = 64, sink_packet_size = 4096 };

static volatile sig_atomic_t keep_running = 1;

static void stop_sink(int signal_number) {
    (void)signal_number;
    keep_running = 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s listen_ip port expected_packets\n", argv[0]);
        return 2;
    }

    uint64_t expected = strtoull(argv[3], 0, 10);
    if (expected == 0) {
        fprintf(stderr, "expected_packets must be positive\n");
        return 2;
    }
    uint8_t *seen = calloc((size_t)expected, sizeof(*seen));
    if (seen == 0) {
        perror("calloc");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        free(seen);
        return 1;
    }
    int socket_buffer = 10 * 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &socket_buffer, sizeof(socket_buffer)) != 0) {
        perror("setsockopt");
        close(fd);
        free(seen);
        return 1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("bind");
        close(fd);
        free(seen);
        return 1;
    }

    struct sigaction stop_action;
    memset(&stop_action, 0, sizeof(stop_action));
    stop_action.sa_handler = stop_sink;
    sigemptyset(&stop_action.sa_mask);
    if (sigaction(SIGINT, &stop_action, NULL) != 0 || sigaction(SIGTERM, &stop_action, NULL) != 0) {
        perror("sigaction");
        close(fd);
        free(seen);
        return 1;
    }

    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t unique = 0;
    uint64_t duplicates = 0;
    uint64_t invalid = 0;
    while (keep_running) {
        struct mmsghdr messages[sink_batch_size];
        struct iovec iovecs[sink_batch_size];
        char buffers[sink_batch_size][sink_packet_size];
        memset(messages, 0, sizeof(messages));
        for (int i = 0; i < sink_batch_size; i++) {
            iovecs[i].iov_base = buffers[i];
            iovecs[i].iov_len = sizeof(buffers[i]);
            messages[i].msg_hdr.msg_iov = &iovecs[i];
            messages[i].msg_hdr.msg_iovlen = 1;
        }

        int received = recvmmsg(fd, messages, sink_batch_size, MSG_WAITFORONE, 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            perror("recvmmsg");
            close(fd);
            free(seen);
            return 1;
        }
        for (int i = 0; i < received; i++) {
            packets++;
            bytes += messages[i].msg_len;
            if (messages[i].msg_len < sizeof(uint32_t)) {
                invalid++;
                continue;
            }
            uint32_t wire_sequence;
            memcpy(&wire_sequence, buffers[i], sizeof(wire_sequence));
            uint64_t sequence = ntohl(wire_sequence);
            if (sequence >= expected) {
                invalid++;
            } else if (seen[sequence]) {
                duplicates++;
            } else {
                seen[sequence] = 1;
                unique++;
            }
        }
    }

    printf("sink_packets=%llu sink_unique=%llu sink_bytes=%llu sink_duplicates=%llu sink_invalid=%llu\n",
           (unsigned long long)packets, (unsigned long long)unique, (unsigned long long)bytes,
           (unsigned long long)duplicates, (unsigned long long)invalid);
    close(fd);
    free(seen);
    return 0;
}
