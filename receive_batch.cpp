/*
 * receive_batch.cpp
 */

#include "receive_batch.h"
#include "log.h"

int recvmmsg_batch = 1;
io_batch_statistics_t io_batch_statistics;

namespace {

#if defined(__linux__)
int recvmmsg_available = 1;
int recvmmsg_unit_test_force_enosys = 0;
#endif

int receive_one(int fd, received_datagram_t &packet, int want_address) {
    packet.address_len = sizeof(packet.address);
    if (want_address) {
        return recvfrom(fd, packet.data, sizeof(packet.data), 0,
                        (sockaddr *)&packet.address, &packet.address_len);
    }
    return recv(fd, packet.data, sizeof(packet.data), 0);
}

}  // namespace

void io_batch_statistics_t::clear() {
    memset(this, 0, sizeof(*this));
}

int get_receive_batch_size() {
    if (recvmmsg_batch < 1) return 1;
    if (recvmmsg_batch > max_receive_batch) return max_receive_batch;
    return recvmmsg_batch;
}

void report_io_batch_statistics(const char *role) {
    if (io_batch_statistics.recvmmsg_calls == 0 && io_batch_statistics.sendmmsg_calls == 0 &&
        io_batch_statistics.sendmmsg_fallback_packets == 0)
        return;

    mylog(log_info,
          "[report][io][%s] recv_batch:calls=%llu packets=%llu eagain=%llu fallback=%llu "
          "send_batch:calls=%llu packets=%llu partial=%llu eagain=%llu fallback_packets=%llu\n",
          role,
          io_batch_statistics.recvmmsg_calls, io_batch_statistics.recvmmsg_packets,
          io_batch_statistics.recvmmsg_eagain, io_batch_statistics.recvmmsg_fallbacks,
          io_batch_statistics.sendmmsg_calls, io_batch_statistics.sendmmsg_packets,
          io_batch_statistics.sendmmsg_partial_calls, io_batch_statistics.sendmmsg_eagain,
          io_batch_statistics.sendmmsg_fallback_packets);
    io_batch_statistics.clear();
}

udp_receive_batch_t::udp_receive_batch_t() {
    address_enabled = -1;
    for (int i = 0; i < max_receive_batch; i++) {
        packets[i].address_len = sizeof(packets[i].address);
        packets[i].len = 0;
#if defined(__linux__)
        memset(&messages[i], 0, sizeof(messages[i]));
        iovecs[i].iov_base = packets[i].data;
        iovecs[i].iov_len = sizeof(packets[i].data);
        messages[i].msg_hdr.msg_iov = &iovecs[i];
        messages[i].msg_hdr.msg_iovlen = 1;
#endif
    }
}

int udp_receive_batch_t::receive(int fd, int want_address, int maximum_packets) {
    if (maximum_packets < 1) return 0;
    int receive_count = min(maximum_packets, get_receive_batch_size());

    if (receive_count == 1) {
        int len = receive_one(fd, packets[0], want_address);
        if (len < 0) {
            int error = get_sock_errno();
            if (error == EAGAIN || error == EWOULDBLOCK) return 0;
            return -1;
        }
        packets[0].len = len;
        return 1;
    }

#if defined(__linux__)
    if (recvmmsg_available) {
        if (address_enabled != want_address) {
            address_enabled = want_address;
            for (int i = 0; i < max_receive_batch; i++) {
                messages[i].msg_hdr.msg_name = want_address ? (void *)&packets[i].address : 0;
                messages[i].msg_hdr.msg_namelen = want_address ? sizeof(packets[i].address) : 0;
            }
        }

        for (int i = 0; i < receive_count; i++) {
            packets[i].address_len = sizeof(packets[i].address);
            messages[i].msg_len = 0;
            messages[i].msg_hdr.msg_flags = 0;
            if (want_address) messages[i].msg_hdr.msg_namelen = sizeof(packets[i].address);
        }

        int received;
        if (recvmmsg_unit_test_force_enosys) {
            errno = ENOSYS;
            received = -1;
        } else {
            received = recvmmsg(fd, messages, receive_count, MSG_DONTWAIT, 0);
        }
        io_batch_statistics.recvmmsg_calls++;
        if (received >= 0) {
            io_batch_statistics.recvmmsg_packets += received;
            for (int i = 0; i < received; i++) {
                packets[i].len = messages[i].msg_len;
                if (want_address) packets[i].address_len = messages[i].msg_hdr.msg_namelen;
            }
            return received;
        }

        int error = get_sock_errno();
        if (error == EAGAIN || error == EWOULDBLOCK) {
            io_batch_statistics.recvmmsg_eagain++;
            return 0;
        }
        if (error != ENOSYS) return -1;

        recvmmsg_available = 0;
        recvmmsg_batch = 1;
        mylog(log_warn, "recvmmsg unavailable, falling back to one receive per callback\n");
    }
#endif

    io_batch_statistics.recvmmsg_fallbacks++;
    int len = receive_one(fd, packets[0], want_address);
    if (len < 0) {
        int error = get_sock_errno();
        if (error == EAGAIN || error == EWOULDBLOCK) return 0;
        return -1;
    }
    packets[0].len = len;
    return 1;
}

int receive_batch_unit_test() {
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0 && sender >= 0);

    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(receiver, (sockaddr *)&address, sizeof(address)) == 0);

    socklen_t address_len = sizeof(address);
    assert(getsockname(receiver, (sockaddr *)&address, &address_len) == 0);

    const char *messages[] = {"batch-one", "batch-two", "batch-three"};
    for (int i = 0; i < 3; i++) {
        assert(sendto(sender, messages[i], strlen(messages[i]), 0, (sockaddr *)&address, address_len) == (int)strlen(messages[i]));
    }

    int saved_batch = recvmmsg_batch;
    recvmmsg_batch = 32;
    udp_receive_batch_t *batch = new udp_receive_batch_t;
    assert(batch != 0);
    int received = batch->receive(receiver, 1, max_receive_batch);
    assert(received == 3);
    for (int i = 0; i < received; i++) {
        assert(batch->packets[i].len == (int)strlen(messages[i]));
        assert(memcmp(batch->packets[i].data, messages[i], batch->packets[i].len) == 0);
        assert(batch->packets[i].address_len > 0);
    }

    assert(sendto(sender, messages[0], strlen(messages[0]), 0, (sockaddr *)&address, address_len) == (int)strlen(messages[0]));
    recvmmsg_batch = 1;
    received = batch->receive(receiver, 1, max_receive_batch);
    assert(received == 1);
    assert(batch->packets[0].len == (int)strlen(messages[0]));
    assert(memcmp(batch->packets[0].data, messages[0], batch->packets[0].len) == 0);

#if defined(__linux__)
    // Exercise the ENOSYS path independently of the running kernel. The
    // first call must drain one packet through recvfrom and make subsequent
    // callbacks use the legacy receive path.
    assert(sendto(sender, messages[1], strlen(messages[1]), 0, (sockaddr *)&address, address_len) == (int)strlen(messages[1]));
    recvmmsg_batch = 32;
    recvmmsg_available = 1;
    recvmmsg_unit_test_force_enosys = 1;
    received = batch->receive(receiver, 1, max_receive_batch);
    assert(received == 1);
    assert(recvmmsg_batch == 1);
    assert(batch->packets[0].len == (int)strlen(messages[1]));
    assert(memcmp(batch->packets[0].data, messages[1], batch->packets[0].len) == 0);
    recvmmsg_unit_test_force_enosys = 0;
    recvmmsg_available = 1;
#endif
    recvmmsg_batch = saved_batch;

    delete batch;
    sock_close(sender);
    sock_close(receiver);
    return 0;
}
