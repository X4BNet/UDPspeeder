/*
 * misc.cpp
 *
 *  Created on: Oct 26, 2017
 *      Author: root
 */

#include "misc.h"
#include "immediate_send_batch.h"

char fifo_file[1000] = "";

int mtu_warn = 1350;

int disable_mtu_warn = 1;
int disable_fec = 0;
int disable_checksum = 0;

int debug_force_flush_fec = 0;

int jitter_min = 0 * 1000;
int jitter_max = 0 * 1000;

int output_interval_min = 0 * 1000;
int output_interval_max = 0 * 1000;

int fix_latency = 0;

address_t local_addr, remote_addr;
address_t *out_addr = 0;
char *out_interface = 0;
// u32_t local_ip_uint32,remote_ip_uint32=0;
// char local_ip[100], remote_ip[100];
// int local_port = -1, remote_port = -1;

conn_manager_t conn_manager;
delay_manager_t delay_manager;
fd_manager_t fd_manager;

int time_mono_test = 0;

int delay_capacity = 0;

int use_sendmmsg = 0;

char sub_net[100] = "10.22.22.0";
u32_t sub_net_uint32 = 0;

char tun_dev[100] = "";

int keep_reconnect = 0;

int tun_mtu = 1500;

int mssfix = default_mtu;

int manual_set_tun = 0;
int persist_tun = 0;

char rs_par_str[rs_str_len] = "20:10";

int from_normal_to_fec(conn_info_t &conn_info, char *data, int len, int &out_n, char **&out_arr, int *&out_len, my_time_t *&out_delay) {
    static my_time_t out_delay_buf[max_fec_packet_num + 100] = {0};
    static char *combined_out_arr[max_fec_packet_num + 100];
    static int combined_out_len[max_fec_packet_num + 100];
    // static int out_len_buf[max_fec_packet_num+100]={0};
    // static int counter=0;
    out_delay = out_delay_buf;
    // out_len=out_len_buf;
    inner_stat_t &inner_stat = conn_info.stat.normal_to_fec;
    if (disable_fec) {
        if (data == 0) {
            out_n = 0;
            return 0;
        }
        // assert(data!=0);
        inner_stat.input_packet_num++;
        inner_stat.input_packet_size += len;
        inner_stat.output_packet_num++;
        inner_stat.output_packet_size += len;

        out_n = 1;
        static char *data_static;
        data_static = data;
        static int len_static;
        len_static = len;
        out_arr = &data_static;
        out_len = &len_static;
        out_delay[0] = 0;

    } else {
        if (data != 0) {
            inner_stat.input_packet_num++;
            inner_stat.input_packet_size += len;
        }

        fec_parameter_t updated_profile;
        if (conn_info.adaptive_fec.take_profile_update(updated_profile)) {
            conn_info.fec_encode_manager.request_fec_parameter(updated_profile);
        }

        int direct_bypass = 0;
        if (data != 0 && conn_info.adaptive_fec.can_bypass() && conn_info.fec_encode_manager.is_idle()) {
            char *bypass_data = 0;
            int bypass_data_len = 0;
            if (conn_info.adaptive_fec.build_bypass(data, len, bypass_data, bypass_data_len) == 0) {
                static int bypass_len;
                bypass_len = bypass_data_len;
                static char *bypass_arr;
                bypass_arr = bypass_data;
                out_arr = &bypass_arr;
                out_len = &bypass_len;
                out_delay_buf[0] = 0;
                out_n = 1;
                direct_bypass = 1;
            }
        }

        if (!direct_bypass) {
            if (data == 0 && conn_info.fec_encode_manager.is_idle()) {
                out_n = 0;
                out_arr = 0;
                out_len = 0;
            } else {
                conn_info.fec_encode_manager.input(data, len);
                conn_info.fec_encode_manager.output(out_n, out_arr, out_len);
                if (out_n < 0) out_n = 0;
            }
        }

        if (out_n > 0) {
            my_time_t common_latency = 0;

            if (!direct_bypass && fix_latency == 1 && conn_info.fec_encode_manager.get_type() == 0) {
                my_time_t current_time = get_current_time_us();
                my_time_t first_packet_time = conn_info.fec_encode_manager.get_first_packet_time();
                my_time_t tmp;
                assert(first_packet_time != 0);
                // mylog(log_info,"current_time=%llu first_packlet_time=%llu   fec_pending_time=%llu\n",current_time,first_packet_time,(my_time_t)fec_pending_time);
                if ((my_time_t)conn_info.fec_encode_manager.get_pending_time() >= (current_time - first_packet_time)) {
                    tmp = (my_time_t)conn_info.fec_encode_manager.get_pending_time() - (current_time - first_packet_time);
                    // mylog(log_info,"tmp=%llu\n",tmp);
                } else {
                    tmp = 0;
                    // mylog(log_info,"0\n");
                }
                common_latency += tmp;
            }

            common_latency += random_between(jitter_min, jitter_max);

            out_delay_buf[0] = common_latency;

            for (int i = 1; i < out_n; i++) {
                out_delay_buf[i] = out_delay_buf[i - 1] + (my_time_t)(random_between(output_interval_min, output_interval_max) / (out_n - 1));
            }
        }

        char *control_data = 0;
        int control_len = 0;
        if (conn_info.adaptive_fec.build_pending_control(control_data, control_len)) {
            assert(out_n + 1 < max_fec_packet_num + 100);
            combined_out_arr[0] = control_data;
            combined_out_len[0] = control_len;
            for (int i = 0; i < out_n; i++) {
                combined_out_arr[i + 1] = out_arr[i];
                combined_out_len[i + 1] = out_len[i];
            }
            for (int i = out_n; i > 0; i--) {
                out_delay_buf[i] = out_delay_buf[i - 1];
            }
            out_delay_buf[0] = 0;
            out_n++;
            out_arr = combined_out_arr;
            out_len = combined_out_len;
        }

        if (out_n > 0) {
            log_bare(log_trace, "seq= %u ", read_u32(out_arr[0]));
        }
        for (int i = 0; i < out_n; i++) {
            inner_stat.output_packet_num++;
            inner_stat.output_packet_size += out_len[i];

            log_bare(log_trace, "%d ", out_len[i]);
        }

        log_bare(log_trace, "\n");
    }

    mylog(log_trace, "from_normal_to_fec input_len=%d,output_n=%d\n", len, out_n);

    // for(int i=0;i<n;i++)
    //{
    // delay_send(0,dest,s_arr[i],s_len);
    //}
    // delay_send(0,dest,data,len);
    // delay_send(1000*1000,dest,data,len);
    return 0;
}
int from_fec_to_normal(conn_info_t &conn_info, char *data, int len, int &out_n, char **&out_arr, int *&out_len, my_time_t *&out_delay) {
    static my_time_t out_delay_buf[max_blob_packet_num + 100] = {0};
    out_delay = out_delay_buf;
    inner_stat_t &inner_stat = conn_info.stat.fec_to_normal;
    if (disable_fec) {
        assert(data != 0);
        inner_stat.input_packet_num++;
        inner_stat.input_packet_size += len;
        inner_stat.output_packet_num++;
        inner_stat.output_packet_size += len;

        if (data == 0) {
            out_n = 0;
            return 0;
        }
        out_n = 1;
        static char *data_static;
        data_static = data;
        static int len_static;
        len_static = len;
        out_arr = &data_static;
        out_len = &len_static;
        out_delay[0] = 0;
    } else {
        if (data != 0) {
            inner_stat.input_packet_num++;
            inner_stat.input_packet_size += len;
        }

        char *bypass_payload = 0;
        int bypass_payload_len = 0;
        adaptive_fec_inbound_result_t adaptive_result = conn_info.adaptive_fec.process_inbound(data, len, bypass_payload, bypass_payload_len);
        if (adaptive_result == adaptive_fec_consumed) {
            out_n = 0;
        } else if (adaptive_result == adaptive_fec_bypass) {
            static char *bypass_out_arr;
            static int bypass_out_len;
            bypass_out_arr = bypass_payload;
            bypass_out_len = bypass_payload_len;
            out_n = 1;
            out_arr = &bypass_out_arr;
            out_len = &bypass_out_len;
            out_delay_buf[0] = 0;
            inner_stat.output_packet_num++;
            inner_stat.output_packet_size += bypass_payload_len;
        } else {
            conn_info.fec_decode_manager.input(data, len);
            conn_info.fec_decode_manager.expire_incomplete_groups(g_adaptive_fec_config.enabled ? g_adaptive_fec_config.incomplete_group_timeout_us : 0);

            fec_decode_stats_t decoder_stats;
            conn_info.fec_decode_manager.take_statistics(decoder_stats);
            conn_info.adaptive_fec.observe_decoder_stats(decoder_stats);

            conn_info.fec_decode_manager.output(out_n, out_arr, out_len);
            if (out_n < 0) out_n = 0;
            for (int i = 0; i < out_n; i++) {
                out_delay_buf[i] = 0;

                inner_stat.output_packet_num++;
                inner_stat.output_packet_size += out_len[i];
            }
        }
    }

    mylog(log_trace, "from_fec_to_normal input_len=%d,output_n=%d,input_seq=%u\n", len, out_n, read_u32(data));

    //	printf("<n:%d>",n);
    /*
    for(int i=0;i<n;i++)
    {
            delay_send(0,dest,s_arr[i],len_arr[i]);
            //s_arr[i][len_arr[i]]=0;
            //printf("<%s>\n",s_arr[i]);
    }*/
    // my_send(dest,data,len);
    return 0;
}

int delay_send(my_time_t delay, const dest_t &dest, char *data, int len) {
    // int rand=random()%100;
    // mylog(log_info,"rand = %d\n",rand);

    if (dest.cook && random_drop != 0) {
        if (get_fake_random_number() % 10000 < (u32_t)random_drop) {
            return 0;
        }
    }
    return delay_manager.add(delay, dest, data, len);
    ;
}

int delay_send_batch(const dest_t &dest, char *const *data, const int *len, const my_time_t *delay, int n) {
    if (n <= 0) return 0;
    if (n == 1) return delay_send(delay[0], dest, data[0], len[0]);

    char *kept_data[max_fec_packet_num + 100];
    int kept_len[max_fec_packet_num + 100];
    my_time_t kept_delay[max_fec_packet_num + 100];
    int kept_n = 0;
    for (int i = 0; i < n; i++) {
        if (dest.cook && random_drop != 0 && get_fake_random_number() % 10000 < (u32_t)random_drop) continue;
        kept_data[kept_n] = data[i];
        kept_len[kept_n] = len[i];
        kept_delay[kept_n] = delay[i];
        kept_n++;
    }
    return delay_manager.add_batch(kept_delay, dest, kept_data, kept_len, kept_n);
}

int print_parameter() {
    mylog(log_info, "jitter_min=%d jitter_max=%d output_interval_min=%d output_interval_max=%d fec_timeout=%d fec_mtu=%d fec_queue_len=%d fec_mode=%d\n",
          jitter_min / 1000, jitter_max / 1000, output_interval_min / 1000, output_interval_max / 1000, g_fec_par.timeout / 1000, g_fec_par.mtu, g_fec_par.queue_len, g_fec_par.mode);
    mylog(log_info, "fec_str=%s\n", rs_par_str);
    mylog(log_info, "fec_inner_parameter=%s\n", g_fec_par.rs_to_str());
    mylog(log_info, "sendmmsg=%s\n", use_sendmmsg ? "enabled" : "disabled");
    mylog(log_info, "recvmmsg_batch=%d\n", get_receive_batch_size());
    if (g_adaptive_fec_config.enabled) {
        mylog(log_info, "adaptive_fec enabled: feedback_ms=%d recover_hold_ms=%d min_samples=%d\n",
              g_adaptive_fec_config.feedback_interval_us / 1000, g_adaptive_fec_config.recover_hold_us / 1000, g_adaptive_fec_config.minimum_samples);
        mylog(log_info, "adaptive_fec normal=%s\n", g_adaptive_fec_config.normal.rs_to_str());
        mylog(log_info, "adaptive_fec guard=%s\n", g_adaptive_fec_config.guard.rs_to_str());
        mylog(log_info, "adaptive_fec degraded=%s\n", g_adaptive_fec_config.degraded.rs_to_str());
    }
    return 0;
}
int handle_command(char *s) {
    int len = strlen(s);
    while (len >= 1 && s[len - 1] == '\n')
        s[len - 1] = 0;
    mylog(log_info, "got data from fifo,len=%d,s=[%s]\n", len, s);
    int a = -1, b = -1;
    if (strncmp(s, "fec", strlen("fec")) == 0) {
        mylog(log_info, "got command [fec]\n");
        char tmp_str[max_fec_packet_num * 10 + 100];
        fec_parameter_t tmp_par;
        sscanf(s, "fec %s", tmp_str);
        /*
        if(a<1||b<0||a+b>254)
        {
                mylog(log_warn,"invalid value\n");
                return -1;
        }*/
        int ret = tmp_par.rs_from_str(tmp_str);
        if (ret != 0) {
            mylog(log_warn, "failed to parse [%s]\n", tmp_str);
            return -1;
        }
        int version = g_fec_par.version;
        g_fec_par.copy_fec(tmp_par);
        g_fec_par.version = version + 1;
        strcpy(rs_par_str, tmp_str);
        // g_fec_data_num=a;
        // g_fec_redundant_num=b;
    } else if (strncmp(s, "mtu", strlen("mtu")) == 0) {
        mylog(log_info, "got command [mtu]\n");
        sscanf(s, "mtu %d", &a);
        if (a < 100 || a > 2000) {
            mylog(log_warn, "invalid value\n");
            return -1;
        }
        g_fec_par.mtu = a;
    } else if (strncmp(s, "queue-len", strlen("queue-len")) == 0) {
        mylog(log_info, "got command [queue-len]\n");
        sscanf(s, "queue-len %d", &a);
        if (a < 1 || a > 10000) {
            mylog(log_warn, "invalid value\n");
            return -1;
        }
        g_fec_par.queue_len = a;
    } else if (strncmp(s, "mode", strlen("mode")) == 0) {
        mylog(log_info, "got command [mode]\n");
        sscanf(s, "mode %d", &a);
        if (a != 0 && a != 1) {
            mylog(log_warn, "invalid value\n");
            return -1;
        }
        if (g_fec_par.mode != a) {
            g_fec_par.mode = a;

            assert(g_fec_par.rs_from_str(rs_par_str) == 0);  // re parse rs_par_str,not necessary at the moment, for further use
            g_fec_par.version++;
        }
    } else if (strncmp(s, "timeout", strlen("timeout")) == 0) {
        mylog(log_info, "got command [timeout]\n");
        sscanf(s, "timeout %d", &a);
        if (a < 0 || a > 1000) {
            mylog(log_warn, "invalid value\n");
            return -1;
        }
        g_fec_par.timeout = a * 1000;
    } else {
        mylog(log_info, "unknown command\n");
    }
    print_parameter();

    return 0;
}

static void empty_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
}
int unit_test() {
    assert(adaptive_fec_unit_test() == 0);
    assert(receive_batch_unit_test() == 0);
    assert(immediate_send_batch_unit_test() == 0);

    // Exercise the encoder/decoder k=1 replication fast path, including
    // recovery from a parity-only mode-0 group and a mode-1 group.  The
    // manager owns fixed send/receive buffers, so this also guards the
    // allocation-free hot path used by short adaptive-FEC groups.
    {
        fec_parameter_t saved_fec = g_fec_par;
        char k1_profile[] = "1:4";
        assert(g_fec_par.rs_from_str(k1_profile) == 0);
        g_fec_par.version++;
        g_fec_par.mode = 0;

        fec_encode_manager_t *encoder = new fec_encode_manager_t;
        fec_decode_manager_t *decoder = new fec_decode_manager_t;
        assert(encoder != 0 && decoder != 0);
        encoder->set_loop_and_cb(ev_default_loop(0), empty_cb);
        char packet[] = "k1-replication-mode-zero";
        assert(encoder->input(packet, (int)strlen(packet)) == 0);
        assert(encoder->input(0, 0) == 0);

        int encoded_count;
        char **encoded;
        int *encoded_lengths;
        assert(encoder->output(encoded_count, encoded, encoded_lengths) == 0);
        assert(encoded_count == 5);
        assert(decoder->input(encoded[4], encoded_lengths[4]) == 0);

        int decoded_count;
        char **decoded;
        int *decoded_lengths;
        assert(decoder->output(decoded_count, decoded, decoded_lengths) == 0);
        assert(decoded_count == 1);
        assert(decoded_lengths[0] == (int)strlen(packet));
        assert(memcmp(decoded[0], packet, decoded_lengths[0]) == 0);

        g_fec_par.mode = 1;
        fec_encode_manager_t *mode_one_encoder = new fec_encode_manager_t;
        fec_decode_manager_t *mode_one_decoder = new fec_decode_manager_t;
        assert(mode_one_encoder != 0 && mode_one_decoder != 0);
        mode_one_encoder->set_loop_and_cb(ev_default_loop(0), empty_cb);
        char mode_one_packet[] = "k1-replication-mode-one";
        assert(mode_one_encoder->input(mode_one_packet, (int)strlen(mode_one_packet)) == 0);
        assert(mode_one_encoder->output(encoded_count, encoded, encoded_lengths) == 0);
        assert(encoded_count == 5);

        // The systematic k=1 frame now carries a full FEC header. A new
        // decoder can forward it directly, then suppress its replicas.
        assert((unsigned char)encoded[0][sizeof(u32_t) + sizeof(char)] == 1);
        assert((unsigned char)encoded[0][sizeof(u32_t) + 2 * sizeof(char)] == 4);
        assert(mode_one_decoder->input(encoded[0], encoded_lengths[0]) == 0);
        assert(mode_one_decoder->output(decoded_count, decoded, decoded_lengths) == 0);
        assert(decoded_count == 1);
        assert(decoded_lengths[0] == (int)strlen(mode_one_packet));
        assert(memcmp(decoded[0], mode_one_packet, decoded_lengths[0]) == 0);
        assert(mode_one_decoder->input(encoded[4], encoded_lengths[4]) == 0);
        assert(mode_one_decoder->output(decoded_count, decoded, decoded_lengths) == 0);
        assert(decoded_count == -1);

        // A parity-only arrival must still recover the same k=1 payload.
        assert(mode_one_decoder->clear() == 0);
        assert(mode_one_decoder->input(encoded[4], encoded_lengths[4]) == 0);
        assert(mode_one_decoder->output(decoded_count, decoded, decoded_lengths) == 0);
        assert(decoded_count == 1);
        assert(decoded_lengths[0] == (int)strlen(mode_one_packet));
        assert(memcmp(decoded[0], mode_one_packet, decoded_lengths[0]) == 0);
        fec_decode_stats_t recovery_stats;
        assert(mode_one_decoder->take_statistics(recovery_stats) == 0);
        assert(recovery_stats.delivered_packets == 1);
        assert(recovery_stats.recovered_packets == 1);

        // An old sender advertises the systematic frame with data_num=0.
        // The new decoder must retain that generic group and suppress the
        // later full-header parity rather than delivering the packet twice.
        char old_systematic[buf_len];
        memcpy(old_systematic, encoded[0], encoded_lengths[0]);
        old_systematic[sizeof(u32_t) + sizeof(char)] = 0;
        old_systematic[sizeof(u32_t) + 2 * sizeof(char)] = 0;
        assert(mode_one_decoder->clear() == 0);
        assert(mode_one_decoder->input(old_systematic, encoded_lengths[0]) == 0);
        assert(mode_one_decoder->output(decoded_count, decoded, decoded_lengths) == 0);
        assert(decoded_count == 1);
        assert(mode_one_decoder->input(encoded[4], encoded_lengths[4]) == 0);
        assert(mode_one_decoder->output(decoded_count, decoded, decoded_lengths) == 0);
        assert(decoded_count == 0);

        delete mode_one_decoder;
        delete mode_one_encoder;
        delete decoder;
        delete encoder;
        g_fec_par = saved_fec;
    }
    {
        union test_t {
            u64_t u64;
            char arry[8];
        } test111;

        assert((void *)&test111.u64 == (void *)&test111.arry[0]);
        // printf("%llx,%llx\n",&ttt.u64,&ttt.arry[0]);

        //		printf("%lld\n",get_fake_random_number_64());
        //		printf("%lld\n",get_fake_random_number_64());
        //		printf("%lld\n",get_fake_random_number_64());

        //		printf("%x\n",get_fake_random_number());
        //		printf("%x\n",get_fake_random_number());
        //		printf("%x\n",get_fake_random_number());

        char buf[10];
        get_fake_random_chars(buf, 10);
        for (int i = 0; i < 10; i++)
            printf("<%d>", (int)buf[i]);
        printf("\n");

        get_fake_random_chars(buf, 10);
        for (int i = 0; i < 10; i++)
            printf("<%d>", (int)buf[i]);
        printf("\n");
    }

    int i, j, k;
    void *code = fec_new(3, 6);
    char arr[6][100] =
        {
            "aaa", "bbb", "ccc", "ddd", "eee", "fff"};
    char *data[6];
    for (i = 0; i < 6; i++) {
        data[i] = arr[i];
    }
    rs_encode2(3, 6, data, 3);
    // printf("%d %d",(int)(unsigned char)arr[5][0],(int)('a'^'b'^'c'^'d'^'e'));

    for (i = 0; i < 6; i++) {
        printf("<%s>", data[i]);
    }

    data[0] = 0;
    // data[1]=0;
    // data[5]=0;

    int ret = rs_decode2(3, 6, data, 3);
    printf("ret:%d\n", ret);

    for (i = 0; i < 6; i++) {
        printf("<%s>", data[i]);
    }
    fec_free(code);

    char arr2[6][100] =
        {
            "aaa11111", "", "ccc333333333", "ddd444", "eee5555", "ff6666"};
    blob_encode_t blob_encode;
    for (int i = 0; i < 6; i++)
        blob_encode.input(arr2[i], strlen(arr2[i]));

    char **output;
    int shard_len;
    blob_encode.output(7, output, shard_len);

    printf("<shard_len:%d>", shard_len);
    blob_decode_t blob_decode;
    for (int i = 0; i < 7; i++) {
        blob_decode.input(output[i], shard_len);
    }

    char **decode_output;
    int *len_arr;
    int num;

    ret = blob_decode.output(num, decode_output, len_arr);

    printf("<num:%d,ret:%d>\n", num, ret);
    for (int i = 0; i < num; i++) {
        char buf[1000] = {0};
        memcpy(buf, decode_output[i], len_arr[i]);
        printf("<%d:%s>", len_arr[i], buf);
    }
    printf("\n");
    static fec_encode_manager_t fec_encode_manager;
    static fec_decode_manager_t fec_decode_manager;

    // dynamic_update_fec=0;

    fec_encode_manager.set_loop_and_cb(ev_default_loop(0), empty_cb);

    {
        string a = "11111";
        string b = "22";
        string c = "33333333";

        fec_encode_manager.input((char *)a.c_str(), a.length());
        fec_encode_manager.input((char *)b.c_str(), b.length());
        fec_encode_manager.input((char *)c.c_str(), c.length());
        fec_encode_manager.input(0, 0);

        int n;
        char **s_arr;
        int *len;

        fec_encode_manager.output(n, s_arr, len);
        printf("<n:%d,len:%d>", n, len[0]);

        for (int i = 0; i < n; i++) {
            fec_decode_manager.input(s_arr[i], len[i]);
        }

        {
            int n;
            char **s_arr;
            int *len_arr;
            fec_decode_manager.output(n, s_arr, len_arr);
            printf("<n:%d>", n);
            for (int i = 0; i < n; i++) {
                s_arr[i][len_arr[i]] = 0;
                printf("<%s>\n", s_arr[i]);
            }
        }
    }

    {
        string a = "aaaaaaa";
        string b = "bbbbbbbbbbbbb";
        string c = "ccc";

        fec_encode_manager.input((char *)a.c_str(), a.length());
        fec_encode_manager.input((char *)b.c_str(), b.length());
        fec_encode_manager.input((char *)c.c_str(), c.length());
        fec_encode_manager.input(0, 0);

        int n;
        char **s_arr;
        int *len;

        fec_encode_manager.output(n, s_arr, len);
        printf("<n:%d,len:%d>", n, len[0]);

        for (int i = 0; i < n; i++) {
            if (i == 1 || i == 3 || i == 5 || i == 0)
                fec_decode_manager.input(s_arr[i], len[i]);
        }

        {
            int n;
            char **s_arr;
            int *len_arr;
            fec_decode_manager.output(n, s_arr, len_arr);
            printf("<n:%d>", n);
            for (int i = 0; i < n; i++) {
                s_arr[i][len_arr[i]] = 0;
                printf("<%s>\n", s_arr[i]);
            }
        }
    }

    printf("ok here.\n");
    for (int i = 0; i < 10; i++) {
        string a = "aaaaaaaaaaaaaaaaaaaaaaa";
        string b = "bbbbbbbbbbbbb";
        string c = "cccccccccccccccccc";

        printf("======\n");
        int n;
        char **s_arr;
        int *len;
        fec_decode_manager.output(n, s_arr, len);

        // fec_encode_manager.reset_fec_parameter(3,2,g_fec_mtu,g_fec_queue_len,g_fec_timeout,1);

        fec_parameter_t &fec_par = fec_encode_manager.get_fec_par();
        fec_par.mtu = g_fec_par.mtu;
        fec_par.queue_len = g_fec_par.queue_len;
        fec_par.timeout = g_fec_par.timeout;
        fec_par.mode = 1;
        fec_par.rs_from_str((char *)"3:2");

        fec_encode_manager.input((char *)a.c_str(), a.length());
        fec_encode_manager.output(n, s_arr, len);

        printf("n=<%d>\n", n);
        assert(n == 1);

        fec_decode_manager.input(s_arr[0], len[0]);

        fec_decode_manager.output(n, s_arr, len);
        assert(n == 1);
        printf("%s\n", s_arr[0]);

        fec_encode_manager.input((char *)b.c_str(), b.length());
        fec_encode_manager.output(n, s_arr, len);
        assert(n == 1);
        // fec_decode_manager.input(s_arr[0],len[0]);

        fec_encode_manager.input((char *)c.c_str(), c.length());
        fec_encode_manager.output(n, s_arr, len);

        assert(n == 3);

        fec_decode_manager.input(s_arr[0], len[0]);
        // printf("n=%d\n",n);

        {
            int n;
            char **s_arr;
            int *len;
            fec_decode_manager.output(n, s_arr, len);
            assert(n == 1);
            printf("%s\n", s_arr[0]);
        }

        fec_decode_manager.input(s_arr[1], len[1]);

        {
            int n;
            char **s_arr;
            int *len;
            fec_decode_manager.output(n, s_arr, len);
            assert(n == 1);
            printf("n=%d\n", n);
            s_arr[0][len[0]] = 0;
            printf("%s\n", s_arr[0]);
        }
    }

    myexit(0);
    return 0;
}

void process_arg(int argc, char *argv[]) {
    int is_client = 0, is_server = 0;
    int i, j, k;
    int opt;
    int adaptive_fec_requested = 0;
    char adaptive_normal_profile[rs_str_len] = "";
    char adaptive_guard_profile[rs_str_len] = "";
    char adaptive_degraded_profile[rs_str_len] = "";
    static struct option long_options[] =
        {
            {"log-level", required_argument, 0, 1},
            {"log-position", no_argument, 0, 1},
            {"disable-color", no_argument, 0, 1},
            {"enable-color", no_argument, 0, 1},
            {"disable-filter", no_argument, 0, 1},
            {"disable-fec", no_argument, 0, 1},
            {"disable-obscure", no_argument, 0, 1},
            {"disable-xor", no_argument, 0, 1},
            {"disable-checksum", no_argument, 0, 1},
            {"fix-latency", no_argument, 0, 1},
            {"sock-buf", required_argument, 0, 1},
            {"random-drop", required_argument, 0, 1},
            {"report", required_argument, 0, 1},
            {"delay-capacity", required_argument, 0, 1},
            {"mtu", required_argument, 0, 1},
            {"mode", required_argument, 0, 1},
            {"timeout", required_argument, 0, 1},
            {"decode-buf", required_argument, 0, 1},
            {"queue-len", required_argument, 0, 'q'},
            {"fec", required_argument, 0, 'f'},
            {"jitter", required_argument, 0, 'j'},
            {"out-addr", required_argument, 0, 1},
            {"out-interface", required_argument, 0, 1},
            {"key", required_argument, 0, 'k'},
            {"header-overhead", required_argument, 0, 1},
            //{"debug-fec", no_argument,    0, 1},
            {"debug-fec-enc", no_argument, 0, 1},
            {"debug-fec-dec", no_argument, 0, 1},
            {"fifo", required_argument, 0, 1},
            {"sub-net", required_argument, 0, 1},
            {"tun-dev", required_argument, 0, 1},
            {"tun-mtu", required_argument, 0, 1},
            {"mssfix", required_argument, 0, 1},
            {"keep-reconnect", no_argument, 0, 1},
            {"persist-tun", no_argument, 0, 1},
            {"manual-set-tun", no_argument, 0, 1},
            {"adaptive-fec", no_argument, 0, 1},
            {"adaptive-normal", required_argument, 0, 1},
            {"adaptive-guard", required_argument, 0, 1},
            {"adaptive-degraded", required_argument, 0, 1},
            {"adaptive-feedback-ms", required_argument, 0, 1},
            {"adaptive-recover-ms", required_argument, 0, 1},
            {"sendmmsg", no_argument, 0, 1},
            {"recvmmsg-batch", required_argument, 0, 1},
            {"interval", required_argument, 0, 'i'},
            {NULL, 0, 0, 0}};
    int option_index = 0;
    assert(g_fec_par.rs_from_str(rs_par_str) == 0);

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--unit-test") == 0) {
            unit_test();
            myexit(0);
        }
    }

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--log-level") == 0) {
            if (i < argc - 1) {
                sscanf(argv[i + 1], "%d", &log_level);
                if (0 <= log_level && log_level < log_end) {
                } else {
                    log_bare(log_fatal, "invalid log_level\n");
                    myexit(-1);
                }
            }
        }
        if (strcmp(argv[i], "--enable-color") == 0) {
            enable_log_color = 1;
        }
        if (strcmp(argv[i], "--disable-color") == 0) {
            enable_log_color = 0;
        }
    }

    mylog(log_info, "argc=%d ", argc);

    for (i = 0; i < argc; i++) {
        log_bare(log_info, "%s ", argv[i]);
    }
    log_bare(log_info, "\n");

    int no_l = 1, no_r = 1;
    while ((opt = getopt_long(argc, argv, "l:r:hcsk:j:f:p:n:i:q:", long_options, &option_index)) != -1) {
        // string opt_key;
        // opt_key+=opt;
        switch (opt) {
            case 'k':
                sscanf(optarg, "%s\n", key_string);
                mylog(log_debug, "key=%s\n", key_string);
                if (strlen(key_string) == 0) {
                    mylog(log_fatal, "key len=0??\n");
                    myexit(-1);
                }
                break;
            case 'j':
                if (strchr(optarg, ':') == 0) {
                    int jitter;
                    sscanf(optarg, "%d\n", &jitter);
                    if (jitter < 0 || jitter > 1000 * 10) {
                        mylog(log_fatal, "jitter must be between 0 and 10,000(10 second)\n");
                        myexit(-1);
                    }
                    jitter_min = 0;
                    jitter_max = jitter;

                } else {
                    sscanf(optarg, "%d:%d\n", &jitter_min, &jitter_max);
                    if (jitter_min < 0 || jitter_max < 0 || jitter_min > jitter_max) {
                        mylog(log_fatal, " must satisfy  0<=jmin<=jmax\n");
                        myexit(-1);
                    }
                }
                jitter_min *= 1000;
                jitter_max *= 1000;
                break;
            case 'i':
                if (strchr(optarg, ':') == 0) {
                    int output_interval = -1;
                    sscanf(optarg, "%d\n", &output_interval);
                    if (output_interval < 0 || output_interval > 1000 * 10) {
                        mylog(log_fatal, "output_interval must be between 0 and 10,000(10 second)\n");
                        myexit(-1);
                    }
                    output_interval_min = output_interval_max = output_interval;
                } else {
                    sscanf(optarg, "%d:%d\n", &output_interval_min, &output_interval_max);
                    if (output_interval_min < 0 || output_interval_max < 0 || output_interval_min > output_interval_max) {
                        mylog(log_fatal, " must satisfy  0<=output_interval_min<=output_interval_max\n");
                        myexit(-1);
                    }
                }
                output_interval_min *= 1000;
                output_interval_max *= 1000;
                break;
            case 'f':
                if (strchr(optarg, ':') == 0) {
                    mylog(log_fatal, "invalid format for f");
                    myexit(-1);
                } else {
                    strcpy(rs_par_str, optarg);
                    // sscanf(optarg,"%d:%d\n",&g_fec_data_num,&g_fec_redundant_num);
                    /*
                    if(g_fec_data_num<1 ||g_fec_redundant_num<0||g_fec_data_num+g_fec_redundant_num>254)
                    {
                            mylog(log_fatal,"fec_data_num<1 ||fec_redundant_num<0||fec_data_num+fec_redundant_num>254\n");
                            myexit(-1);
                    }*/
                }
                break;
            case 'q':
                sscanf(optarg, "%d", &g_fec_par.queue_len);
                if (g_fec_par.queue_len < 1 || g_fec_par.queue_len > 10000) {
                    mylog(log_fatal, "fec_pending_num should be between 1 and 10000\n");
                    myexit(-1);
                }
                break;
            case 'c':
                is_client = 1;
                break;
            case 's':
                is_server = 1;
                break;
            case 'l':
                no_l = 0;
                local_addr.from_str(optarg);
                break;
            case 'r':
                no_r = 0;
                remote_addr.from_str(optarg);
                break;
            case 'h':
                break;
            case 1:
                if (strcmp(long_options[option_index].name, "log-level") == 0) {
                } else if (strcmp(long_options[option_index].name, "disable-filter") == 0) {
                    disable_replay_filter = 1;
                    // enable_log_color=0;
                } else if (strcmp(long_options[option_index].name, "disable-color") == 0) {
                    // enable_log_color=0;
                } else if (strcmp(long_options[option_index].name, "enable-color") == 0) {
                    // enable_log_color=0;
                } else if (strcmp(long_options[option_index].name, "disable-fec") == 0) {
                    disable_fec = 1;
                } else if (strcmp(long_options[option_index].name, "disable-obscure") == 0) {
                    mylog(log_info, "obscure disabled\n");
                    disable_obscure = 1;
                } else if (strcmp(long_options[option_index].name, "disable-xor") == 0) {
                    mylog(log_info, "xor disabled\n");
                    disable_xor = 1;
                } else if (strcmp(long_options[option_index].name, "disable-checksum") == 0) {
                    disable_checksum = 1;
                    mylog(log_warn, "checksum disabled\n");
                } else if (strcmp(long_options[option_index].name, "fix-latency") == 0) {
                    mylog(log_info, "fix-latency enabled\n");
                    fix_latency = 1;
                }

                else if (strcmp(long_options[option_index].name, "log-position") == 0) {
                    enable_log_position = 1;
                } else if (strcmp(long_options[option_index].name, "random-drop") == 0) {
                    sscanf(optarg, "%d", &random_drop);
                    if (random_drop < 0 || random_drop > 10000) {
                        mylog(log_fatal, "random_drop must be between 0 10000 \n");
                        myexit(-1);
                    }
                    mylog(log_info, "random_drop=%d\n", random_drop);
                } else if (strcmp(long_options[option_index].name, "delay-capacity") == 0) {
                    sscanf(optarg, "%d", &delay_capacity);

                    if (delay_capacity < 0) {
                        mylog(log_fatal, "delay_capacity must be >=0 \n");
                        myexit(-1);
                    }
                } else if (strcmp(long_options[option_index].name, "report") == 0) {
                    sscanf(optarg, "%d", &report_interval);

                    if (report_interval <= 0) {
                        mylog(log_fatal, "report_interval must be >0 \n");
                        myexit(-1);
                    }
                } else if (strcmp(long_options[option_index].name, "sock-buf") == 0) {
                    int tmp = -1;
                    sscanf(optarg, "%d", &tmp);
                    if (10 <= tmp && tmp <= 10 * 1024) {
                        socket_buf_size = tmp * 1024;
                    } else {
                        mylog(log_fatal, "sock-buf value must be between 1 and 10240 (kbyte) \n");
                        myexit(-1);
                    }
                } else if (strcmp(long_options[option_index].name, "decode-buf") == 0) {
                    sscanf(optarg, "%d", &fec_buff_num);
                    if (fec_buff_num < 300 || fec_buff_num > 20000) {
                        mylog(log_fatal, "decode-buf value must be between 300 and 20000 (kbyte) \n");
                        myexit(-1);
                    }
                    mylog(log_info, "decode-buf=%d\n", fec_buff_num);
                } else if (strcmp(long_options[option_index].name, "mode") == 0) {
                    sscanf(optarg, "%d", &g_fec_par.mode);
                    if (g_fec_par.mode != 0 && g_fec_par.mode != 1) {
                        mylog(log_fatal, "mode should be 0 or 1\n");
                        myexit(-1);
                    }
                } else if (strcmp(long_options[option_index].name, "mtu") == 0) {
                    sscanf(optarg, "%d", &g_fec_par.mtu);
                    if (g_fec_par.mtu < 100 || g_fec_par.mtu > 2000) {
                        mylog(log_fatal, "fec_mtu should be between 100 and 2000\n");
                        myexit(-1);
                    }
                } else if (strcmp(long_options[option_index].name, "out-addr") == 0) {
                    // has_b = true;
                    mylog(log_debug, "out-addr=%s\n", optarg);
                    out_addr = new address_t();
                    out_addr->from_str(optarg);
                } else if (strcmp(long_options[option_index].name, "out-interface") == 0) {
                    out_interface = new char[strlen(optarg) + 10];
                    sscanf(optarg, "%s\n", out_interface);
                    mylog(log_debug, "out-interface=%s\n", out_interface);
                    if (strlen(out_interface) == 0) {
                        mylog(log_fatal, "out_interface string len=0??\n");
                        myexit(-1);
                    }
                } else if (strcmp(long_options[option_index].name, "timeout") == 0) {
                    sscanf(optarg, "%d", &g_fec_par.timeout);
                    if (g_fec_par.timeout < 0 || g_fec_par.timeout > 1000) {
                        mylog(log_fatal, "fec_pending_time should be between 0 and 1000(1s)\n");
                        myexit(-1);
                    }
                    g_fec_par.timeout *= 1000;
                } else if (strcmp(long_options[option_index].name, "debug-fec-enc") == 0) {
                    debug_fec_enc = 1;
                    mylog(log_info, "debug_fec_enc enabled\n");
                } else if (strcmp(long_options[option_index].name, "debug-fec-dec") == 0) {
                    debug_fec_dec = 1;
                    mylog(log_info, "debug_fec_dec enabled\n");
                } else if (strcmp(long_options[option_index].name, "fifo") == 0) {
                    sscanf(optarg, "%s", fifo_file);

                    mylog(log_info, "fifo_file =%s \n", fifo_file);
                } else if (strcmp(long_options[option_index].name, "keep-reconnect") == 0) {
                    keep_reconnect = 1;
                    mylog(log_info, "keep_reconnect enabled\n");
                } else if (strcmp(long_options[option_index].name, "manual-set-tun") == 0) {
                    manual_set_tun = 1;
                    mylog(log_info, "manual_set_tun enabled\n");
                } else if (strcmp(long_options[option_index].name, "persist-tun") == 0) {
                    persist_tun = 1;
                    mylog(log_info, "persist_tun enabled\n");
                } else if (strcmp(long_options[option_index].name, "sub-net") == 0) {
                    sscanf(optarg, "%s", sub_net);
                    mylog(log_info, "sub_net %s\n", sub_net);

                } else if (strcmp(long_options[option_index].name, "tun-dev") == 0) {
                    sscanf(optarg, "%s", tun_dev);
                    mylog(log_info, "tun_dev=%s\n", tun_dev);

                } else if (strcmp(long_options[option_index].name, "tun-mtu") == 0) {
                    sscanf(optarg, "%d", &tun_mtu);
                    mylog(log_warn, "changed tun_mtu,tun_mtu=%d\n", tun_mtu);
                } else if (strcmp(long_options[option_index].name, "header-overhead") == 0) {
                    sscanf(optarg, "%d", &header_overhead);
                    mylog(log_warn, "changed header_overhead,header_overhead=%d\n", header_overhead);
                } else if (strcmp(long_options[option_index].name, "mssfix") == 0) {
                    sscanf(optarg, "%d", &mssfix);
                    mylog(log_warn, "mssfix=%d\n", mssfix);
                } else if (strcmp(long_options[option_index].name, "adaptive-fec") == 0) {
                    adaptive_fec_requested = 1;
                } else if (strcmp(long_options[option_index].name, "adaptive-normal") == 0) {
                    adaptive_fec_requested = 1;
                    if (strlen(optarg) >= sizeof(adaptive_normal_profile)) {
                        mylog(log_fatal, "adaptive normal FEC profile is too long\n");
                        myexit(-1);
                    }
                    strcpy(adaptive_normal_profile, optarg);
                } else if (strcmp(long_options[option_index].name, "adaptive-guard") == 0) {
                    adaptive_fec_requested = 1;
                    if (strlen(optarg) >= sizeof(adaptive_guard_profile)) {
                        mylog(log_fatal, "adaptive guard FEC profile is too long\n");
                        myexit(-1);
                    }
                    strcpy(adaptive_guard_profile, optarg);
                } else if (strcmp(long_options[option_index].name, "adaptive-degraded") == 0) {
                    adaptive_fec_requested = 1;
                    if (strlen(optarg) >= sizeof(adaptive_degraded_profile)) {
                        mylog(log_fatal, "adaptive degraded FEC profile is too long\n");
                        myexit(-1);
                    }
                    strcpy(adaptive_degraded_profile, optarg);
                } else if (strcmp(long_options[option_index].name, "adaptive-feedback-ms") == 0) {
                    adaptive_fec_requested = 1;
                    int milliseconds = -1;
                    sscanf(optarg, "%d", &milliseconds);
                    if (milliseconds < 100 || milliseconds > 10000) {
                        mylog(log_fatal, "adaptive feedback interval must be between 100 and 10000ms\n");
                        myexit(-1);
                    }
                    g_adaptive_fec_config.feedback_interval_us = milliseconds * 1000;
                } else if (strcmp(long_options[option_index].name, "adaptive-recover-ms") == 0) {
                    adaptive_fec_requested = 1;
                    int milliseconds = -1;
                    sscanf(optarg, "%d", &milliseconds);
                    if (milliseconds < 1000 || milliseconds > 60000) {
                        mylog(log_fatal, "adaptive recovery hold must be between 1000 and 60000ms\n");
                        myexit(-1);
                    }
                    g_adaptive_fec_config.recover_hold_us = milliseconds * 1000;
                } else if (strcmp(long_options[option_index].name, "sendmmsg") == 0) {
#if defined(__linux__)
                    use_sendmmsg = 1;
#else
                    mylog(log_fatal, "--sendmmsg is only supported on Linux\n");
                    myexit(-1);
#endif
                } else if (strcmp(long_options[option_index].name, "recvmmsg-batch") == 0) {
                    sscanf(optarg, "%d", &recvmmsg_batch);
                    if (recvmmsg_batch < 1 || recvmmsg_batch > max_receive_batch) {
                        mylog(log_fatal, "recvmmsg-batch must be between 1 and %d\n", max_receive_batch);
                        myexit(-1);
                    }
#if !defined(__linux__)
                    if (recvmmsg_batch > 1) {
                        mylog(log_warn, "recvmmsg is unavailable on this platform; using one receive per callback\n");
                        recvmmsg_batch = 1;
                    }
#endif
                } else {
                    mylog(log_fatal, "unknown option\n");
                    myexit(-1);
                }
                break;
            default:
                mylog(log_fatal, "unknown option <%x>", opt);
                myexit(-1);
        }
    }

    if (is_client == 0 && is_server == 0) {
        mylog(log_fatal, "-s -c hasn't been set\n");
        myexit(-1);
    }
    if (is_client == 1 && is_server == 1) {
        mylog(log_fatal, "-s -c can't be both set\n");
        myexit(-1);
    }
    if (is_client == 1) {
        program_mode = client_mode;
    } else {
        program_mode = server_mode;
    }

    if (working_mode == tunnel_mode) {
        if (no_l)
            mylog(log_fatal, "error: -l not found\n");
        if (no_r)
            mylog(log_fatal, "error: -r not found\n");
        if (no_l || no_r)
            myexit(-1);
    } else if (working_mode == tun_dev_mode) {
        if (program_mode == client_mode && no_r) {
            mylog(log_fatal, "error: -r not found\n");
            myexit(-1);
        } else if (program_mode == server_mode && no_l) {
            mylog(log_fatal, "error: -l not found\n");
            myexit(-1);
        }
    }

    int ret = g_fec_par.rs_from_str(rs_par_str);
    if (ret != 0) {
        mylog(log_fatal, "failed to parse [%s]\n", rs_par_str);
        myexit(-1);
    }

    if (adaptive_fec_requested) {
        if (key_string[0] != 0 && strlen(key_string) < 16) {
            mylog(log_fatal, "--adaptive-fec needs a 16+ byte -k/--key value, or no key to use the built-in compatibility key\n");
            myexit(-1);
        }
        if (g_adaptive_fec_config.configure(g_fec_par, adaptive_normal_profile, adaptive_guard_profile, adaptive_degraded_profile) != 0) {
            mylog(log_fatal, "failed to parse adaptive FEC profile\n");
            myexit(-1);
        }
        g_adaptive_fec_config.enabled = 1;
    }

    print_parameter();
}
