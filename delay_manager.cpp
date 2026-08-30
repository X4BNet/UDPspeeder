/*
 * delay_manager.cpp
 *
 *  Created on: Sep 15, 2017
 *      Author: root
 */
#include "delay_manager.h"
#include "log.h"
#include "packet.h"
#include "receive_batch.h"

#if defined(__linux__)
#include <linux/udp.h>
#endif

namespace {

const int max_send_batch = 355;
char immediate_batch_buffer[max_send_batch][buf_len];

#if defined(__linux__)
// The event loop is single-threaded, but use thread-local storage so a
// future worker does not share mutable msghdr state.  sendmmsg only writes
// msg_len; all header fields are fully assigned below before each call.
struct sendmmsg_metadata_t {
    mmsghdr messages[max_send_batch] = {};
    iovec iovecs[max_send_batch] = {};
};
thread_local sendmmsg_metadata_t sendmmsg_metadata;
#endif

#if defined(__linux__)
// Linux caps UDP GSO at 64 segments.  Keep the aggregate within the maximum
// UDP payload too; large packets simply form a smaller GSO group.
const int udp_gso_kernel_max_segments = 64;
const int udp_gso_max_payload = 65507;
int udp_gso_available = 1;
#endif

socklen_t address_length(const address_t &address) {
    switch (((const sockaddr *)&address.inner)->sa_family) {
        case AF_INET:
            return sizeof(sockaddr_in);
        case AF_INET6:
            return sizeof(sockaddr_in6);
        default:
            assert(0 == 1);
            return 0;
    }
}

int same_dest(const dest_t &a, const dest_t &b) {
    if (a.type != b.type || a.cook != b.cook) return 0;
    switch (a.type) {
        case type_fd64:
            return a.inner.fd64 == b.inner.fd64;
        case type_fd:
        case type_write_fd:
            return a.inner.fd == b.inner.fd;
        case type_fd64_addr:
            return a.inner.fd64_addr.fd64 == b.inner.fd64_addr.fd64 &&
                   address_length(a.inner.fd64_addr.addr) == address_length(b.inner.fd64_addr.addr) &&
                   memcmp(&a.inner.fd64_addr.addr.inner, &b.inner.fd64_addr.addr.inner, address_length(a.inner.fd64_addr.addr)) == 0;
        case type_fd_addr:
            return a.inner.fd_addr.fd == b.inner.fd_addr.fd &&
                   address_length(a.inner.fd_addr.addr) == address_length(b.inner.fd_addr.addr) &&
                   memcmp(&a.inner.fd_addr.addr.inner, &b.inner.fd_addr.addr.inner, address_length(a.inner.fd_addr.addr)) == 0;
        default:
            return 0;
    }
}

int get_sendmmsg_destination(const dest_t &dest, int &fd, sockaddr *&address, socklen_t &address_len) {
    address = 0;
    address_len = 0;
    switch (dest.type) {
        case type_fd64:
            if (!fd_manager.exist(dest.inner.fd64)) return -1;
            fd = fd_manager.to_fd(dest.inner.fd64);
            return 0;
        case type_fd:
            fd = dest.inner.fd;
            return 0;
        case type_fd64_addr:
            if (!fd_manager.exist(dest.inner.fd64_addr.fd64)) return -1;
            fd = fd_manager.to_fd(dest.inner.fd64_addr.fd64);
            address = (sockaddr *)&dest.inner.fd64_addr.addr.inner;
            address_len = address_length(dest.inner.fd64_addr.addr);
            return 0;
        case type_fd_addr:
            fd = dest.inner.fd_addr.fd;
            address = (sockaddr *)&dest.inner.fd_addr.addr.inner;
            address_len = address_length(dest.inner.fd_addr.addr);
            return 0;
        default:
            return -1;
    }
}

int send_prepared_batch_without_gso(const dest_t &dest, char *const *data, const int *len, int n) {
    if (n <= 0) return 0;
    if (!use_sendmmsg || n == 1) {
        int result = 0;
        for (int i = 0; i < n; i++) {
            if (my_send_prepared(dest, data[i], len[i]) < 0) result = -1;
        }
        return result;
    }

#if defined(__linux__)
    int fd = -1;
    sockaddr *address = 0;
    socklen_t address_len = 0;
    if (get_sendmmsg_destination(dest, fd, address, address_len) == 0) {
        assert(n <= max_send_batch);
        for (int i = 0; i < n; i++) {
            mmsghdr &message = sendmmsg_metadata.messages[i];
            iovec &iov = sendmmsg_metadata.iovecs[i];
            iov.iov_base = data[i];
            iov.iov_len = len[i];
            message.msg_hdr.msg_name = address;
            message.msg_hdr.msg_namelen = address_len;
            message.msg_hdr.msg_iov = &iov;
            message.msg_hdr.msg_iovlen = 1;
            message.msg_hdr.msg_control = 0;
            message.msg_hdr.msg_controllen = 0;
            message.msg_hdr.msg_flags = 0;
        }

        int sent = sendmmsg(fd, sendmmsg_metadata.messages, n, 0);
        io_batch_statistics.sendmmsg_calls++;
        if (sent < 0) {
            if (get_sock_errno() == EAGAIN || get_sock_errno() == EWOULDBLOCK) {
                io_batch_statistics.sendmmsg_eagain++;
            }
            sent = 0;
        } else {
            io_batch_statistics.sendmmsg_packets += sent;
            if (sent < n) io_batch_statistics.sendmmsg_partial_calls++;
        }
        int result = 0;
        for (int i = sent; i < n; i++) {
            io_batch_statistics.sendmmsg_fallback_packets++;
            if (my_send_prepared(dest, data[i], len[i]) < 0) result = -1;
        }
        return result;
    }
#endif

    int result = 0;
    for (int i = 0; i < n; i++) {
        if (my_send_prepared(dest, data[i], len[i]) < 0) result = -1;
    }
    return result;
}

#if defined(__linux__)
int send_udp_gso_batch(const dest_t &dest, char *const *data, const int *len, int n) {
    int fd = -1;
    sockaddr *address = 0;
    socklen_t address_len = 0;
    if (!udp_gso_available || get_sendmmsg_destination(dest, fd, address, address_len) != 0) {
        io_batch_statistics.udp_gso_fallback_packets += n;
        return send_prepared_batch_without_gso(dest, data, len, n);
    }

    int result = 0;
    int i = 0;
    while (i < n) {
        int segment_len = len[i];
        if (segment_len <= 0 || segment_len > udp_gso_max_payload) {
            io_batch_statistics.udp_gso_fallback_packets++;
            if (my_send_prepared(dest, data[i], len[i]) < 0) result = -1;
            i++;
            continue;
        }

        int packet_count = 1;
        int maximum = min(min(udp_gso_max_segments, udp_gso_kernel_max_segments), udp_gso_max_payload / segment_len);
        while (packet_count < maximum && i + packet_count < n && len[i + packet_count] == segment_len) packet_count++;
        if (packet_count == 1) {
            io_batch_statistics.udp_gso_fallback_packets++;
            if (my_send_prepared(dest, data[i], len[i]) < 0) result = -1;
            i++;
            continue;
        }

        int total_len = packet_count * segment_len;
        // UDP_SEGMENT accepts a scatter/gather payload.  The ready batch
        // already owns one contiguous buffer per UDP datagram, so use those
        // buffers directly instead of copying a complete GSO super-packet.
        iovec iov[max_send_batch];
        for (int j = 0; j < packet_count; j++) {
            iov[j].iov_base = data[i + j];
            iov[j].iov_len = segment_len;
        }
        char control[CMSG_SPACE(sizeof(unsigned short))] = {};
        msghdr message = {};
        message.msg_name = address;
        message.msg_namelen = address_len;
        message.msg_iov = iov;
        message.msg_iovlen = packet_count;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
        assert(cmsg != 0);
        cmsg->cmsg_level = IPPROTO_UDP;
        cmsg->cmsg_type = UDP_SEGMENT;
        cmsg->cmsg_len = CMSG_LEN(sizeof(unsigned short));
        unsigned short segment_value = (unsigned short)segment_len;
        memcpy(CMSG_DATA(cmsg), &segment_value, sizeof(segment_value));

        int sent = sendmsg(fd, &message, 0);
        io_batch_statistics.udp_gso_calls++;
        if (sent == total_len) {
            io_batch_statistics.udp_gso_packets += packet_count;
            io_batch_statistics.udp_gso_bytes += total_len;
            i += packet_count;
            continue;
        }

        if (sent > 0 && sent <= total_len && sent % segment_len == 0) {
            int sent_packets = sent / segment_len;
            io_batch_statistics.udp_gso_packets += sent_packets;
            io_batch_statistics.udp_gso_bytes += sent;
            io_batch_statistics.udp_gso_partial_calls++;
            i += sent_packets;
        } else if (sent < 0) {
            int error = get_sock_errno();
            // Unsupported GSO and transient socket pressure must retain the
            // established sendmmsg/sendmsg behavior rather than dropping a
            // complete batch. Disable only permanent capability errors.
            if (error == EINVAL || error == EOPNOTSUPP || error == ENOPROTOOPT) udp_gso_available = 0;
        } else {
            io_batch_statistics.udp_gso_partial_calls++;
            return -1;
        }

        io_batch_statistics.udp_gso_fallback_packets += n - i;
        int fallback = send_prepared_batch_without_gso(dest, data + i, len + i, n - i);
        return result != 0 || fallback != 0 ? -1 : 0;
    }
    return result;
}
#endif

int send_prepared_batch(const dest_t &dest, char *const *data, const int *len, int n) {
    if (n <= 0) return 0;
#if defined(__linux__)
    if (use_udp_gso && n > 1) return send_udp_gso_batch(dest, data, len, n);
#endif
    return send_prepared_batch_without_gso(dest, data, len, n);
}

int cook_and_send_batch(const dest_t &dest, char *const *data, int *len, int n) {
    for (int i = 0; i < n; i++) {
        if (dest.cook) do_cook(data[i], len[i]);
    }
    return send_prepared_batch(dest, data, len, n);
}

int send_immediate_batch(const dest_t &dest, char *const *data, const int *len, int n) {
    assert(n > 1 && n <= max_send_batch);
    char *prepared[max_send_batch];
    int prepared_len[max_send_batch];
    for (int i = 0; i < n; i++) {
        assert(len[i] >= 0 && len[i] + 100 < buf_len);
        memcpy(immediate_batch_buffer[i], data[i], len[i]);
        prepared[i] = immediate_batch_buffer[i];
        prepared_len[i] = len[i];
    }
    return cook_and_send_batch(dest, prepared, prepared_len, n);
}

}  // namespace

int send_immediate_batch_in_place(const dest_t &dest, char *const *data, int *len, int n) {
    if (n <= 0) return 0;
    assert(n <= max_send_batch);

    // Preserve delay_send_batch's random-drop semantics before cooking.
    char *kept_data[max_send_batch];
    int kept_len[max_send_batch];
    int kept_n = 0;
    for (int i = 0; i < n; i++) {
        if (dest.cook && random_drop != 0 && get_fake_random_number() % 10000 < (u32_t)random_drop) continue;
        kept_data[kept_n] = data[i];
        kept_len[kept_n] = len[i];
        kept_n++;
    }
    return cook_and_send_batch(dest, kept_data, kept_len, kept_n);
}

int udp_gso_unit_test() {
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0 && sender >= 0);

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(receiver, (sockaddr *)&address, sizeof(address)) == 0);
    socklen_t address_len = sizeof(address);
    assert(getsockname(receiver, (sockaddr *)&address, &address_len) == 0);

    char packets[4][64] = {};
    char *packet_ptrs[4];
    int packet_lens[4];
    for (int i = 0; i < 4; i++) {
        memset(packets[i], 'a' + i, sizeof(packets[i]));
        packet_ptrs[i] = packets[i];
        packet_lens[i] = sizeof(packets[i]);
    }

    dest_t dest = {};
    dest.type = type_fd_addr;
    dest.inner.fd_addr.fd = sender;
    dest.inner.fd_addr.addr.from_sockaddr((sockaddr *)&address, address_len);

    int saved_sendmmsg = use_sendmmsg;
    int saved_udp_gso = use_udp_gso;
    io_batch_statistics_t saved_statistics = io_batch_statistics;
#if defined(__linux__)
    int saved_gso_available = udp_gso_available;
    udp_gso_available = 1;
#endif
    use_sendmmsg = 1;
    use_udp_gso = 1;
    io_batch_statistics.clear();
    assert(send_immediate_batch_in_place(dest, packet_ptrs, packet_lens, 4) == 0);

    for (int i = 0; i < 4; i++) {
        char received[64];
        int received_len = recv(receiver, received, sizeof(received), 0);
        assert(received_len == (int)sizeof(received));
        assert(memcmp(received, packets[i], sizeof(received)) == 0);
    }
#if defined(__linux__)
    // Restricted or older kernels may cleanly fall back to sendmmsg. In both
    // cases every application datagram must remain separately observable.
    assert(io_batch_statistics.udp_gso_packets == 4 || io_batch_statistics.udp_gso_fallback_packets >= 4);
    udp_gso_available = saved_gso_available;
#endif
    io_batch_statistics = saved_statistics;
    use_sendmmsg = saved_sendmmsg;
    use_udp_gso = saved_udp_gso;
    sock_close(sender);
    sock_close(receiver);
    return 0;
}

int delay_data_t::handle() {
    return my_send(dest, data, len) >= 0;
}

delay_manager_t::delay_manager_t() {
    capacity = 0;

    // if ((timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) < 0)
    //{
    //	mylog(log_fatal,"timer_fd create error");
    //	myexit(1);
    // }

    // itimerspec zero_its;
    // memset(&zero_its, 0, sizeof(zero_its));

    // timerfd_settime(timer_fd, TFD_TIMER_ABSTIME, &zero_its, 0);
}
delay_manager_t::~delay_manager_t() {
    // TODO ,we currently dont need to deconstruct it
}

/*
int delay_manager_t::get_timer_fd()
{
        return timer_fd;
}*/

// int add(my_time_t delay,const dest_t &dest,const char *data,int len);
int delay_manager_t::add(my_time_t delay, const dest_t &dest, char *data, int len) {
    delay_data_t delay_data;
    delay_data.dest = dest;
    // delay_data.data=data;
    delay_data.len = len;

    if (capacity != 0 && int(delay_mp.size()) >= capacity) {
        mylog(log_warn, "max pending packet reached,ignored\n");
        return -1;
    }
    if (delay == 0) {
        static char buf[buf_len];
        delay_data.data = buf;
        memcpy(buf, data, len);
        int ret = delay_data.handle();
        if (ret != 0) {
            mylog(log_trace, "handle() return %d\n", ret);
        }
        return 0;
    }

    delay_data_t tmp = delay_data;
    tmp.data = (char *)malloc(delay_data.len + 100);
    if (!tmp.data) {
        mylog(log_warn, "malloc() returned null in delay_manager_t::add()");
        return -1;
    }
    memcpy(tmp.data, data, delay_data.len);

    my_time_t tmp_time = get_current_time_us();
    tmp_time += delay;

    delay_mp.insert(make_pair(tmp_time, tmp));

    ////check();  check every time when add, is it better ??

    return 0;
}

int delay_manager_t::add_batch(const my_time_t *delay, const dest_t &dest, char *const *data, const int *len, int n) {
    if (n <= 0) return 0;
    if ((!use_sendmmsg && !use_udp_gso) || n == 1) {
        int result = 0;
        for (int i = 0; i < n; i++) {
            if (add(delay[i], dest, data[i], len[i]) != 0) result = -1;
        }
        return result;
    }

    int result = 0;
    int i = 0;
    while (i < n) {
        if (delay[i] != 0) {
            if (add(delay[i], dest, data[i], len[i]) != 0) result = -1;
            i++;
            continue;
        }

        int first = i;
        while (i < n && delay[i] == 0 && i - first < max_send_batch) i++;
        int batch_n = i - first;
        if (batch_n == 1) {
            if (add(0, dest, data[first], len[first]) != 0) result = -1;
        } else if (send_immediate_batch(dest, data + first, len + first, batch_n) != 0) {
            result = -1;
        }
    }
    return result;
}

int delay_manager_t::check() {
    if (!delay_mp.empty()) {
        my_time_t current_time;
        while (1) {
            multimap<my_time_t, delay_data_t>::iterator it = delay_mp.begin();
            if (it == delay_mp.end()) break;

            current_time = get_current_time_us();
            if (it->first > current_time) break;

            dest_t dest = it->second.dest;
            char *data[max_send_batch];
            int len[max_send_batch];
            int n = 0;
            while (it != delay_mp.end() && it->first <= current_time && n < max_send_batch && same_dest(dest, it->second.dest)) {
                data[n] = it->second.data;
                len[n] = it->second.len;
                n++;
                it = delay_mp.erase(it);
            }

            int ret = cook_and_send_batch(dest, data, len, n);
            if (ret != 0) {
                mylog(log_trace, "batched handle() return %d\n", ret);
            }
            for (int i = 0; i < n; i++) {
                free(data[i]);
            }
        }
        if (!delay_mp.empty()) {
            const double m = 1000 * 1000;
            double timer_value = delay_mp.begin()->first / m - get_current_time_us() / m;  // be aware of negative value, and be aware of uint
            if (timer_value < 0) timer_value = 0;                                          // set it to 0 if negative, although libev support negative value
            ev_timer_stop(loop, &timer);
            ev_timer_set(&timer, timer_value, 0);
            ev_timer_start(loop, &timer);
        } else {
            ev_timer_stop(loop, &timer);  // not necessary
        }
    }
    return 0;
}
