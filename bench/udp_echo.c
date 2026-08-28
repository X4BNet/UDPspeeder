#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { echo_batch_size = 32, echo_packet_size = 4096 };

static volatile sig_atomic_t keep_running = 1;

static void stop_echo(int signal_number) {
    (void)signal_number;
    keep_running = 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s listen_ip port\n", argv[0]);
        return 2;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    int socket_buffer = 10 * 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &socket_buffer, sizeof(socket_buffer)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &socket_buffer, sizeof(socket_buffer)) != 0) {
        perror("setsockopt");
        return 1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) {
        fprintf(stderr, "invalid listen address: %s\n", argv[1]);
        return 2;
    }
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("bind");
        return 1;
    }

    struct sigaction stop_action;
    memset(&stop_action, 0, sizeof(stop_action));
    stop_action.sa_handler = stop_echo;
    sigemptyset(&stop_action.sa_mask);
    // Do not set SA_RESTART: an idle recvmmsg must return EINTR so a
    // disposable benchmark namespace can clean up promptly.
    if (sigaction(SIGINT, &stop_action, NULL) != 0 || sigaction(SIGTERM, &stop_action, NULL) != 0) {
        perror("sigaction");
        return 1;
    }

    while (keep_running) {
        struct mmsghdr receive_messages[echo_batch_size];
        struct mmsghdr send_messages[echo_batch_size];
        struct iovec receive_iovecs[echo_batch_size];
        struct iovec send_iovecs[echo_batch_size];
        struct sockaddr_storage peers[echo_batch_size];
        char buffers[echo_batch_size][echo_packet_size];

        memset(receive_messages, 0, sizeof(receive_messages));
        memset(send_messages, 0, sizeof(send_messages));
        for (int i = 0; i < echo_batch_size; i++) {
            receive_iovecs[i].iov_base = buffers[i];
            receive_iovecs[i].iov_len = sizeof(buffers[i]);
            receive_messages[i].msg_hdr.msg_name = &peers[i];
            receive_messages[i].msg_hdr.msg_namelen = sizeof(peers[i]);
            receive_messages[i].msg_hdr.msg_iov = &receive_iovecs[i];
            receive_messages[i].msg_hdr.msg_iovlen = 1;
        }

        int received = recvmmsg(fd, receive_messages, echo_batch_size, 0, 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            perror("recvmmsg");
            return 1;
        }

        for (int i = 0; i < received; i++) {
            send_iovecs[i].iov_base = buffers[i];
            send_iovecs[i].iov_len = receive_messages[i].msg_len;
            send_messages[i].msg_hdr.msg_name = &peers[i];
            send_messages[i].msg_hdr.msg_namelen = receive_messages[i].msg_hdr.msg_namelen;
            send_messages[i].msg_hdr.msg_iov = &send_iovecs[i];
            send_messages[i].msg_hdr.msg_iovlen = 1;
        }

        int sent = sendmmsg(fd, send_messages, (unsigned int)received, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            perror("sendmmsg");
            return 1;
        }
        for (int i = sent; i < received; i++) {
            if (sendto(fd, buffers[i], receive_messages[i].msg_len, 0,
                       (struct sockaddr *)&peers[i], receive_messages[i].msg_hdr.msg_namelen) < 0) {
                perror("sendto");
                return 1;
            }
        }
    }

    close(fd);
    return 0;
}
