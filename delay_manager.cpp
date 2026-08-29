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

namespace {

const int max_send_batch = 355;
char immediate_batch_buffer[max_send_batch][buf_len];

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

int send_prepared_batch(const dest_t &dest, char *const *data, const int *len, int n) {
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
        mmsghdr messages[max_send_batch];
        iovec iovecs[max_send_batch];
        assert(n <= max_send_batch);
        for (int i = 0; i < n; i++) {
            memset(&messages[i], 0, sizeof(messages[i]));
            iovecs[i].iov_base = data[i];
            iovecs[i].iov_len = len[i];
            messages[i].msg_hdr.msg_iov = &iovecs[i];
            messages[i].msg_hdr.msg_iovlen = 1;
            messages[i].msg_hdr.msg_name = address;
            messages[i].msg_hdr.msg_namelen = address_len;
        }

        int sent = sendmmsg(fd, messages, n, 0);
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
    if (!use_sendmmsg || n == 1) {
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
