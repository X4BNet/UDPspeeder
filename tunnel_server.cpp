/*
 * tunnel.cpp
 *
 *  Created on: Oct 26, 2017
 *      Author: root
 */

#include "tunnel.h"
#include "immediate_send_batch.h"

static void conn_timer_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents);
static void fec_encode_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents);
static void remote_cb(struct ev_loop *loop, struct ev_io *watcher, int revents);

namespace {

// libev invokes remote callbacks serially. Shared preallocated stores avoid a
// large per-conversation receive allocation while preserving packet lifetime
// until each synchronous callback finishes.
udp_receive_batch_t local_receive_batch;
udp_receive_batch_t remote_receive_batch;
immediate_send_batch_t local_output_batch;
immediate_send_batch_t remote_output_batch;

int receive_event_limit() {
    return get_receive_batch_size() > 1 ? max_receive_packets_per_callback : 1;
}

}  // namespace

enum tmp_mode_t { is_fec_timeout = 0,
                  is_conn_timer };

void data_from_fec_timeout_or_conn_timer(conn_info_t &conn_info, tmp_mode_t mode) {
    // fd64_t fd64=events[idx].data.u64;
    // mylog(log_trace,"events[idx].data.u64 >u32_t(-1),%llu\n",(u64_t)events[idx].data.u64);

    // assert(fd_manager.exist_info(fd64));
    // ip_port_t ip_port=fd_manager.get_info(fd64).ip_port;

    // conn_info_t &conn_info=conn_manager.find(ip_port);
    address_t &addr = conn_info.addr;
    assert(conn_manager.exist(addr));

    int &local_listen_fd = conn_info.local_listen_fd;

    int out_n = -2;
    char **out_arr;
    int *out_len;
    my_time_t *out_delay;

    dest_t dest;
    dest.inner.fd_addr.fd = local_listen_fd;
    dest.inner.fd_addr.addr = addr;
    dest.type = type_fd_addr;
    dest.cook = 1;

    if (mode == is_fec_timeout) {
        // uint64_t value;
        // if((ret=read(fd_manager.to_fd(fd64), &value, 8))!=8)
        //{
        //	mylog(log_trace,"fd_manager.to_fd(fd64), &value, 8)!=8 ,%d\n",ret);
        //	continue;
        // }
        // if(value==0)
        //{
        //	mylog(log_trace,"value==0\n");
        //	continue;
        // }
        // assert(value==1);
        from_normal_to_fec(conn_info, 0, 0, out_n, out_arr, out_len, out_delay);
    } else if (mode == is_conn_timer) {
        // uint64_t value;
        // read(conn_info.timer.get_timer_fd(), &value, 8);
        conn_info.conv_manager.s.clear_inactive();
        if (debug_force_flush_fec || conn_info.adaptive_fec.is_enabled()) {
            from_normal_to_fec(conn_info, 0, 0, out_n, out_arr, out_len, out_delay);
            delay_send_batch(dest, out_arr, out_len, out_delay, out_n);
            if (conn_info.fec_encode_manager != 0) conn_info.fec_encode_manager->release_output_storage();
        }

        if (conn_info.stat.report_as_server(addr)) conn_info.adaptive_fec.report_statistics("server");
        return;
    } else {
        assert(0 == 1);
    }

    mylog(log_trace, "out_n=%d\n", out_n);
    delay_send_batch(dest, out_arr, out_len, out_delay, out_n);
    if (conn_info.fec_encode_manager != 0) conn_info.fec_encode_manager->release_output_storage();
}

static void process_remote_datagram(conn_info_t &conn_info, fd64_t fd64, char *data, int data_len, immediate_send_batch_t &output_batch) {
    if (!fd_manager.exist(fd64)) {
        mylog(log_warn, "!fd_manager.exist(fd64)\n");
        return;
    }
    assert(conn_info.conv_manager.s.is_data_used(fd64));

    u32_t conv = conn_info.conv_manager.s.find_conv_by_data(fd64);
    conn_info.conv_manager.s.update_active_time(conv);
    conn_info.update_active_time();

    if (data_len == max_data_len + 1) {
        mylog(log_warn, "huge packet from upper level, data_len > %d, packet truncated, dropped\n", max_data_len);
        return;
    }
    if (!disable_mtu_warn && data_len >= mtu_warn) {
        mylog(log_warn, "huge packet,data len=%d (>=%d).strongly suggested to set a smaller mtu at upper level,to get rid of this warn\n ", data_len, mtu_warn);
    }

    char *new_data;
    int new_len;
    put_conv(conv, data, data_len, new_data, new_len);

    int out_n;
    char **out_arr;
    int *out_len;
    my_time_t *out_delay;
    dest_t dest;
    dest.inner.fd_addr.fd = conn_info.local_listen_fd;
    dest.inner.fd_addr.addr = conn_info.addr;
    dest.type = type_fd_addr;
    dest.cook = 1;
    from_normal_to_fec(conn_info, new_data, new_len, out_n, out_arr, out_len, out_delay);
    for (int i = 0; i < out_n; i++) {
        output_batch.add(out_delay[i], dest, out_arr[i], out_len[i]);
    }
    if (conn_info.fec_encode_manager != 0) conn_info.fec_encode_manager->release_output_storage();
}

static void process_local_datagram(struct ev_loop *loop, int local_listen_fd, char *data, int data_len, address_t addr, immediate_send_batch_t *output_batch = 0) {
    int ret;

    if (data_len == max_data_len + 1) {
        mylog(log_warn, "huge packet, data_len > %d, packet truncated, dropped\n", max_data_len);
        return;
    }

    mylog(log_trace, "Received packet from %s,len: %d\n", addr.get_str(), data_len);

    if (!disable_mtu_warn && data_len >= mtu_warn)  ///////////////////////delete this for type 0 in future
    {
        mylog(log_warn, "huge packet,data len=%d (>=%d).strongly suggested to set a smaller mtu at upper level,to get rid of this warn\n ", data_len, mtu_warn);
    }

    if (de_cook(data, data_len) != 0) {
        mylog(log_debug, "de_cook error");
        return;
    }

    if (!conn_manager.exist(addr)) {
        // Keep an unauthenticated/unrecognised source in the tiny admission
        // path. A connection used to allocate every FEC buffer here before
        // the decoder could reject even a malformed inner frame.
        // A reconnect can legitimately send its first payload as an
        // authenticated adaptive-FEC direct-bypass (type 2) frame. The FEC
        // validator deliberately accepts only types 0/1, so admit type 2
        // only after its full control header and MAC have been verified.
        if (validate_fec_frame(data, data_len) != 0 && validate_adaptive_fec_frame(data, data_len) != 0) {
            mylog(log_debug, "ignored new peer with invalid fec frame\n");
            return;
        }
        if (conn_manager.mp.size() >= max_conn_num) {
            mylog(log_warn, "new connection %s ignored bc max_conn_num exceed\n", addr.get_str());
            return;
        }

        // conn_manager.insert(addr);
        conn_info_t &conn_info = conn_manager.find_insert(addr);
        conn_info.addr = addr;
        conn_info.loop = ev_default_loop(0);
        conn_info.local_listen_fd = local_listen_fd;

        // u64_t fec_fd64=conn_info.fec_encode_manager.get_timer_fd64();
        // mylog(log_debug,"fec_fd64=%llu\n",fec_fd64);
        // ev.events = EPOLLIN;
        // ev.data.u64 = fec_fd64;
        // ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd_manager.to_fd(fec_fd64), &ev);

        // fd_manager.get_info(fec_fd64).ip_port=ip_port;

        conn_info.timer.data = &conn_info;
        ev_init(&conn_info.timer, conn_timer_cb);
        ev_timer_set(&conn_info.timer, 0, timer_interval / 1000.0);
        ev_timer_start(loop, &conn_info.timer);

        // conn_info.timer.add_fd64_to_epoll(epoll_fd);
        // conn_info.timer.set_timer_repeat_us(timer_interval*1000);

        // mylog(log_debug,"conn_info.timer.get_timer_fd64()=%llu\n",conn_info.timer.get_timer_fd64());

        // u64_t timer_fd64=conn_info.timer.get_timer_fd64();
        // fd_manager.get_info(timer_fd64).ip_port=ip_port;

        conn_info.set_fec_encode_callback(fec_encode_cb);

        mylog(log_info, "new connection from %s\n", addr.get_str());
    }
    conn_info_t &conn_info = conn_manager.find_insert(addr);

    conn_info.update_active_time();
    int out_n;
    char **out_arr;
    int *out_len;
    my_time_t *out_delay;
    from_fec_to_normal(conn_info, data, data_len, out_n, out_arr, out_len, out_delay);

    mylog(log_trace, "out_n= %d\n", out_n);
    for (int i = 0; i < out_n; i++) {
        u32_t conv;
        char *new_data;
        int new_len;
        if (get_conv(conv, out_arr[i], out_len[i], new_data, new_len) != 0) {
            mylog(log_debug, "get_conv failed");
            continue;
        }

        if (!conn_info.conv_manager.s.is_conv_used(conv)) {
            if (conn_info.conv_manager.s.get_size() >= max_conv_num) {
                mylog(log_warn, "ignored new udp connect bc max_conv_num exceed\n");
                continue;
            }

            int new_udp_fd;
            ret = new_connected_socket2(new_udp_fd, remote_addr, out_addr, out_interface);

            if (ret != 0) {
                mylog(log_warn, "[%s]new_connected_socket failed\n", addr.get_str());
                continue;
            }

            fd64_t fd64 = fd_manager.create(new_udp_fd);
            // ev.events = EPOLLIN;
            // ev.data.u64 = fd64;
            // ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_udp_fd, &ev);

            conn_info.conv_manager.s.insert_conv(conv, fd64);
            fd_manager.get_info(fd64).addr = addr;

            ev_io &io_watcher = fd_manager.get_info(fd64).io_watcher;
            io_watcher.u64 = fd64;
            io_watcher.data = &conn_info;

            ev_init(&io_watcher, remote_cb);
            ev_io_set(&io_watcher, new_udp_fd, EV_READ);
            ev_io_start(conn_info.loop, &io_watcher);

            mylog(log_info, "[%s]new conv %x,fd %d created,fd64=%llu\n", addr.get_str(), conv, new_udp_fd, fd64);
        }
        conn_info.conv_manager.s.update_active_time(conv);
        fd64_t fd64 = conn_info.conv_manager.s.find_data_by_conv(conv);
        dest_t dest;
        dest.type = type_fd64;
        dest.inner.fd64 = fd64;
        if (output_batch != 0)
            output_batch->add(out_delay[i], dest, new_data, new_len);
        else
            delay_send(out_delay[i], dest, new_data, new_len);
    }

    // Answer a verified adaptive-FEC probe in this receive turn. Waiting for
    // the 400 ms maintenance timer prolongs the static, high-redundancy
    // bootstrap phase and can make a sparse TLS handshake self-sustainingly
    // lossy.
    if (conn_info.adaptive_fec.needs_immediate_control()) {
        char *control_data = 0;
        int control_len = 0;
        if (conn_info.adaptive_fec.build_pending_control(control_data, control_len)) {
            dest_t control_dest;
            control_dest.inner.fd_addr.fd = local_listen_fd;
            control_dest.inner.fd_addr.addr = addr;
            control_dest.type = type_fd_addr;
            control_dest.cook = 1;
            if (output_batch != 0)
                output_batch->add(0, control_dest, control_data, control_len);
            else
                delay_send(0, control_dest, control_data, control_len);
        }
    }
    if (conn_info.fec_decode_manager != 0) conn_info.fec_decode_manager->release_output_storage();
}

static void local_listen_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
    assert(!(revents & EV_ERROR));

    int processed = 0;
    int event_limit = receive_event_limit();
    local_output_batch.clear();
    while (processed < event_limit) {
        int requested = min(get_receive_batch_size(), event_limit - processed);
        int received = local_receive_batch.receive(watcher->fd, 1, requested);
        if (received < 0) {
            mylog(log_warn, "recv_from failed,err=%s\n", get_sock_error());
            local_output_batch.flush();
            return;
        }
        if (received == 0) {
            local_output_batch.flush();
            return;
        }
        for (int i = 0; i < received; i++) {
            address_t addr;
            addr.from_sockaddr((sockaddr *)&local_receive_batch.packets[i].address, local_receive_batch.packets[i].address_len);
            process_local_datagram(loop, watcher->fd, local_receive_batch.packets[i].data, local_receive_batch.packets[i].len, addr, &local_output_batch);
        }
        processed += received;
        if (received < requested) {
            local_output_batch.flush();
            return;
        }
    }
    local_output_batch.flush();
}

static void remote_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
    assert(!(revents & EV_ERROR));

    conn_info_t &conn_info = *((conn_info_t *)watcher->data);
    fd64_t fd64 = watcher->u64;
    if (!fd_manager.exist(fd64)) return;

    int processed = 0;
    int event_limit = receive_event_limit();
    remote_output_batch.clear();
    while (processed < event_limit) {
        int requested = min(get_receive_batch_size(), event_limit - processed);
        int received = remote_receive_batch.receive(fd_manager.to_fd(fd64), 0, requested);
        if (received < 0) {
            mylog(log_warn, "recv failed,err=%s\n", get_sock_error());
            remote_output_batch.flush();
            return;
        }
        if (received == 0) {
            remote_output_batch.flush();
            return;
        }
        for (int i = 0; i < received; i++) {
            process_remote_datagram(conn_info, fd64, remote_receive_batch.packets[i].data, remote_receive_batch.packets[i].len, remote_output_batch);
        }
        processed += received;
        if (received < requested) {
            remote_output_batch.flush();
            return;
        }
    }
    remote_output_batch.flush();
}

static void fifo_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
    assert(!(revents & EV_ERROR));

    int fifo_fd = watcher->fd;

    char buf[buf_len];
    int len = read(fifo_fd, buf, sizeof(buf));
    if (len < 0) {
        mylog(log_warn, "fifo read failed len=%d,errno=%s\n", len, get_sock_error());
        return;
    }
    buf[len] = 0;
    handle_command(buf);
}

static void delay_manager_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    assert(!(revents & EV_ERROR));

    // uint64_t value;
    // read(delay_manager.get_timer_fd(), &value, 8);
    // mylog(log_trace,"events[idx].data.u64 == (u64_t)delay_manager.get_timer_fd()\n");

    // do nothing
}

static void fec_encode_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    assert(!(revents & EV_ERROR));

    conn_info_t &conn_info = *((conn_info_t *)watcher->data);

    data_from_fec_timeout_or_conn_timer(conn_info, is_fec_timeout);
}

static void conn_timer_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    assert(!(revents & EV_ERROR));

    conn_info_t &conn_info = *((conn_info_t *)watcher->data);

    data_from_fec_timeout_or_conn_timer(conn_info, is_conn_timer);
}

static void prepare_cb(struct ev_loop *loop, struct ev_prepare *watcher, int revents) {
    assert(!(revents & EV_ERROR));

    delay_manager.check();
}

static void global_timer_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    assert(!(revents & EV_ERROR));

    // uint64_t value;
    // read(timer.get_timer_fd(), &value, 8);
    conn_manager.expire_fec_incomplete_groups(g_adaptive_fec_config.enabled ? g_adaptive_fec_config.incomplete_group_timeout_us : fec_incomplete_group_timeout_us);
    conn_manager.clear_inactive();
    mylog(log_trace, "events[idx].data.u64==(u64_t)timer.get_timer_fd()\n");
}

int tunnel_server_event_loop() {
    int i, j, k;
    int ret;
    int yes = 1;
    // int epoll_fd;
    // int remote_fd;

    int local_listen_fd;
    new_listen_socket2(local_listen_fd, local_addr);

    // epoll_fd = epoll_create1(0);
    // assert(epoll_fd>0);

    // const int max_events = 4096;
    // struct epoll_event ev, events[max_events];
    // if (epoll_fd < 0) {
    //	mylog(log_fatal,"epoll return %d\n", epoll_fd);
    //	myexit(-1);
    // }

    struct ev_loop *loop = ev_default_loop(0);
    assert(loop != NULL);

    // ev.events = EPOLLIN;
    // ev.data.u64 = local_listen_fd;
    // ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, local_listen_fd, &ev);
    // if (ret!=0) {
    //	mylog(log_fatal,"add  udp_listen_fd error\n");
    //	myexit(-1);
    // }
    struct ev_io local_listen_watcher;
    ev_io_init(&local_listen_watcher, local_listen_cb, local_listen_fd, EV_READ);
    ev_io_start(loop, &local_listen_watcher);

    // ev.events = EPOLLIN;
    // ev.data.u64 = delay_manager.get_timer_fd();
    // ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, delay_manager.get_timer_fd(), &ev);
    // if (ret!= 0) {
    //	mylog(log_fatal,"add delay_manager.get_timer_fd() error\n");
    //	myexit(-1);
    // }

    delay_manager.set_loop_and_cb(loop, delay_manager_cb);

    // mylog(log_debug," delay_manager.get_timer_fd() =%d\n", delay_manager.get_timer_fd());

    mylog(log_info, "now listening at %s\n", local_addr.get_str());

    // my_timer_t timer;
    // timer.add_fd_to_epoll(epoll_fd);
    // timer.set_timer_repeat_us(timer_interval*1000);

    ev_timer global_timer;
    ev_init(&global_timer, global_timer_cb);
    ev_timer_set(&global_timer, 0, timer_interval / 1000.0);
    ev_timer_start(loop, &global_timer);

    // mylog(log_debug," timer.get_timer_fd() =%d\n",timer.get_timer_fd());

    struct ev_io fifo_watcher;

    int fifo_fd = -1;

    if (fifo_file[0] != 0) {
        fifo_fd = create_fifo(fifo_file);
        // ev.events = EPOLLIN;
        // ev.data.u64 = fifo_fd;

        // ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fifo_fd, &ev);
        // if (ret!= 0) {
        // mylog(log_fatal,"add fifo_fd to epoll error %s\n",strerror(errno));
        // myexit(-1);
        //}
        ev_io_init(&fifo_watcher, fifo_cb, fifo_fd, EV_READ);
        ev_io_start(loop, &fifo_watcher);

        mylog(log_info, "fifo_file=%s\n", fifo_file);
    }

    ev_prepare prepare_watcher;
    ev_init(&prepare_watcher, prepare_cb);
    ev_prepare_start(loop, &prepare_watcher);

    ev_run(loop, 0);

    mylog(log_warn, "ev_run returned\n");
    myexit(0);

    /*
    while(1)////////////////////////
    {

            if(about_to_exit) myexit(0);

            int nfds = epoll_wait(epoll_fd, events, max_events, 180 * 1000);
            if (nfds < 0) {  //allow zero
                    if(errno==EINTR  )
                    {
                            mylog(log_info,"epoll interrupted by signal,continue\n");
                    }
                    else
                    {
                            mylog(log_fatal,"epoll_wait return %d,%s\n", nfds,strerror(errno));
                            myexit(-1);
                    }
            }
            int idx;
            for (idx = 0; idx < nfds; ++idx)
            {
                    if(events[idx].data.u64==(u64_t)timer.get_timer_fd())
                    {

                    }

                    else if (events[idx].data.u64 == (u64_t)fifo_fd)
                    {

                    }

                    else if (events[idx].data.u64 == (u64_t)local_listen_fd)
                    {


                    }
                else if (events[idx].data.u64 == (u64_t)delay_manager.get_timer_fd()) {

                    }
                    else if (events[idx].data.u64 >u32_t(-1))
                    {


                    }
                    else
                    {
                            mylog(log_fatal,"unknown fd,this should never happen\n");
                            myexit(-1);
                    }
            }

    }*/

    return 0;
}
