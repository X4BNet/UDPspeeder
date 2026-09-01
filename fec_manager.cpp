/*
 * fec_manager.cpp
 *
 *  Created on: Sep 27, 2017
 *      Author: root
 */

#include "fec_manager.h"
#include "log.h"
#include "common.h"
#include "lib/rs.h"
#include "fd_manager.h"

// int g_fec_data_num=20;
// int g_fec_redundant_num=10;
// int g_fec_mtu=1250;
// int g_fec_queue_len=200;
// int g_fec_timeout=8*1000; //8ms
// int g_fec_mode=0;

fec_parameter_t g_fec_par;

int debug_fec_enc = 0;
int debug_fec_dec = 0;
// int dynamic_update_fec=1;

const int encode_fast_send = 1;
const int decode_fast_send = 1;

int short_packet_optimize = 1;
int header_overhead = 40;

u32_t fec_buff_num = 2000;  // how many packet can fec_decode_manager hold. shouldn't be very large,or it will cost huge memory
size_t fec_decode_global_retained_payload_bytes = 0;
size_t fec_decode_global_retained_shard_count = 0;

blob_encode_t::blob_encode_t() {
    clear();
}
int blob_encode_t::clear() {
    counter = 0;
    current_len = (int)sizeof(u32_t);
    return 0;
}
void blob_encode_t::release_memory() {
    vector<char>().swap(input_buf);
    clear();
}

int blob_encode_t::get_num() {
    return counter;
}
int blob_encode_t::get_shard_len(int n) {
    return round_up_div(current_len, n);
}

int blob_encode_t::get_shard_len(int n, int next_packet_len) {
    return round_up_div(current_len + (int)sizeof(u16_t) + next_packet_len, n);
}

int blob_encode_t::input(char *s, int len) {
    assert(current_len + len + sizeof(u16_t) + 100 < (max_fec_packet_num + 5) * buf_len);
    assert(len <= 65535 && len >= 0);
    int required = current_len + (int)sizeof(u16_t) + len;
    if ((int)input_buf.size() < required) input_buf.resize(required);
    counter++;
    assert(counter <= max_blob_packet_num);
    write_u16(input_buf.data() + current_len, len);
    current_len += sizeof(u16_t);
    memcpy(input_buf.data() + current_len, s, len);
    current_len += len;
    return 0;
}

int blob_encode_t::output(int n, char **&s_arr, int &len) {
    len = round_up_div(current_len, n);
    write_u32(input_buf.data(), counter);
    for (int i = 0; i < n; i++) {
        output_buf[i] = input_buf.data() + len * i;
    }
    s_arr = output_buf;
    return 0;
}
blob_decode_t::blob_decode_t() {
    clear();
}
int blob_decode_t::clear() {
    current_len = 0;
    last_len = -1;
    counter = 0;
    return 0;
}
void blob_decode_t::release_memory() {
    vector<char>().swap(input_buf);
    vector<char *>().swap(output_buf);
    vector<int>().swap(output_len);
    clear();
}
int blob_decode_t::input(char *s, int len) {
    if (last_len != -1) {
        assert(last_len == len);
    }
    counter++;
    assert(counter <= max_fec_packet_num);
    last_len = len;
    assert(current_len + len + 100 < (max_fec_packet_num + 5) * buf_len);  // avoid overflow
    int required = current_len + len;
    if ((int)input_buf.size() < required) input_buf.resize(required);
    memcpy(input_buf.data() + current_len, s, len);
    current_len += len;
    return 0;
}
int blob_decode_t::output(int &n, char **&s_arr, int *&len_arr) {
    int parser_pos = 0;

    if (parser_pos + (int)sizeof(u32_t) > current_len) {
        mylog(log_info, "failed 0\n");
        return -1;
    }

    n = (int)read_u32(input_buf.data() + parser_pos);
    if (n > max_blob_packet_num) {
        mylog(log_info, "failed 1\n");
        return -1;
    }
    output_buf.resize(n);
    output_len.resize(n);
    s_arr = output_buf.data();
    len_arr = output_len.data();

    parser_pos += sizeof(u32_t);
    for (int i = 0; i < n; i++) {
        if (parser_pos + (int)sizeof(u16_t) > current_len) {
            mylog(log_info, "failed2 \n");
            return -1;
        }
        len_arr[i] = (int)read_u16(input_buf.data() + parser_pos);
        parser_pos += (int)sizeof(u16_t);
        if (parser_pos + len_arr[i] > current_len) {
            mylog(log_info, "failed 3 %d  %d %d\n", parser_pos, len_arr[i], current_len);
            return -1;
        }
        s_arr[i] = input_buf.data() + parser_pos;
        parser_pos += len_arr[i];
    }
    return 0;
}

fec_encode_manager_t::~fec_encode_manager_t() {
    clear_all();
    // fd_manager.fd64_close(timer_fd64);
}
/*
u64_t fec_encode_manager_t::get_timer_fd64()
{
        return timer_fd64;
}*/

fec_encode_manager_t::fec_encode_manager_t() {
    // int timer_fd;

    /*
    if ((timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) < 0)
    {
            mylog(log_fatal,"timer_fd create error");
            myexit(1);
    }
    timer_fd64=fd_manager.create(timer_fd);*/

    /////reset_fec_parameter(g_fec_data_num,g_fec_redundant_num,g_fec_mtu,g_fec_queue_len,g_fec_timeout,g_fec_mode);

    fec_par.clone(g_fec_par);
    has_pending_fec_update = 0;
    clear_data();
}
char *fec_encode_manager_t::input_slot(int index) {
    assert(index >= 0 && index < max_fec_packet_num + 5);
    size_t required = size_t(index + 1) * buf_len;
    if (input_buf.size() < required) input_buf.resize(required);
    return input_buf.data() + size_t(index) * buf_len;
}
/*
int fec_encode_manager_t::reset_fec_parameter(int data_num,int redundant_num,int mtu,int queue_len,int timeout,int mode)
{
        fec_data_num=data_num;
        fec_redundant_num=redundant_num;
        fec_mtu=mtu;
        fec_queue_len=queue_len;
        fec_timeout=timeout;
        fec_mode=mode;

        assert(data_num+redundant_num<max_fec_packet_num);

        //clear();

        clear_data();
        return 0;
}*/
int fec_encode_manager_t::append(char *s, int len /*,int &is_first_packet*/) {
    if (counter == 0) {
        first_packet_time = get_current_time_us();

        const double m = 1000 * 1000;

        ev_timer_stop(loop, &timer);
        ev_timer_set(&timer, fec_par.timeout / m, 0);
        ev_timer_start(loop, &timer);
    }
    if (fec_par.mode == 0)  // for type 0 use blob
    {
        assert(blob_encode.input(s, len) == 0);
    } else if (fec_par.mode == 1)  // for tpe 1 use  input_buf and counter
    {
        mylog(log_trace, "counter=%d\n", counter);
        assert(len <= 65535 && len >= 0);
        // assert(len<=fec_mtu);//relax this limitation
        char *p = input_slot(counter) + sizeof(u32_t) + 4 * sizeof(char);  // copy directly to final position,avoid unnecessary copy.
        // remember to change this,if protocol is modified

        write_u16(p, (u16_t)((u32_t)len));  // TODO  omit this u16 for data packet while sending
        p += sizeof(u16_t);
        memcpy(p, s, len);
        input_len[counter] = len + sizeof(u16_t);
    } else {
        assert(0 == 1);
    }
    counter++;
    return 0;
}
int fec_encode_manager_t::input(char *s, int len /*,int &is_first_packet*/) {
    if (counter == 0 && has_pending_fec_update) {
        fec_par.clone(pending_fec_par);
        has_pending_fec_update = 0;
    } else if (counter == 0 && fec_par.version != g_fec_par.version) {
        fec_par.clone(g_fec_par);
    }

    int about_to_fec = 0;
    int delayed_append = 0;
    // int counter_back=counter;
    assert(fec_par.mode == 0 || fec_par.mode == 1);

    if (fec_par.mode == 0 && s != 0 && counter == 0) {
        int out_len = blob_encode.get_shard_len(fec_par.get_tail().x, len);
        if (out_len > fec_par.mtu) {
            mylog(log_warn, "message too long ori_len=%d out_len=%d fec_mtu=%d,ignored\n", len, out_len, fec_par.mtu);
            return -1;
        }
    }
    if (fec_par.mode == 1 && s != 0 && len > fec_par.mtu) {
        mylog(log_warn, "mode==1,message len=%d,len>fec_mtu,fec_mtu=%d,packet may not be delivered\n", len, fec_par.mtu);
        // return -1;
    }
    if (s == 0 && counter == 0) {
        mylog(log_warn, "unexpected s==0&&counter==0\n");
        return -1;
    }
    if (s == 0) about_to_fec = 1;  // now

    if (fec_par.mode == 0 && blob_encode.get_shard_len(fec_par.get_tail().x, len) > fec_par.mtu) {
        about_to_fec = 1;
        delayed_append = 1;
    }  // fec then add packet

    if (fec_par.mode == 0) assert(counter < fec_par.queue_len);  // counter will never equal fec_pending_num,if that happens fec should already been done.
    if (fec_par.mode == 1) assert(counter < fec_par.get_tail().x);

    if (s != 0 && !delayed_append) {
        append(s, len);
    }

    if (fec_par.mode == 0 && counter == fec_par.queue_len) about_to_fec = 1;

    if (fec_par.mode == 1 && counter == fec_par.get_tail().x) about_to_fec = 1;

    if (about_to_fec) {
        char **blob_output = 0;
        int fec_len = -1;
        mylog(log_trace, "counter=%d\n", counter);

        if (counter == 0) {
            mylog(log_warn, "unexpected counter==0 here\n");
            return -1;
        }

        int actual_data_num;
        int actual_redundant_num;

        if (fec_par.mode == 0) {
            int tail_x = fec_par.get_tail().x;
            int tail_y = fec_par.get_tail().y;
            actual_data_num = tail_x;
            actual_redundant_num = tail_y;

            if (short_packet_optimize) {
                // A mode-0 blob can hold fewer application packets than the
                // FEC data width. Choosing (for example) 2:5 for a blob with
                // one small TLS datagram turns it into seven wire packets
                // even though 1:4 is available. Limit the optimizer to the
                // blob count; exceed it only when a larger width is needed to
                // keep a large packet below the MTU.
                int blob_packet_count = blob_encode.get_num();
                assert(blob_packet_count > 0);
                u32_t best_len = 0;
                int best_data_num = 0;
                int smallest_mtu_safe_data_num = 0;
                assert(tail_x <= fec_par.rs_cnt);
                for (int i = 1; i <= tail_x; i++) {
                    assert(fec_par.rs_par[i - 1].x == i);
                    int tmp_x = fec_par.rs_par[i - 1].x;
                    int tmp_y = fec_par.rs_par[i - 1].y;
                    assert(tmp_x == i);
                    u32_t shard_len = blob_encode.get_shard_len(tmp_x, 0);
                    if (shard_len > (u32_t)fec_par.mtu) continue;

                    if (smallest_mtu_safe_data_num == 0) smallest_mtu_safe_data_num = tmp_x;
                    if (tmp_x > blob_packet_count) continue;

                    u32_t new_len = (shard_len + header_overhead) * (tmp_x + tmp_y);
                    if (best_data_num == 0 || new_len < best_len) {
                        best_len = new_len;
                        best_data_num = tmp_x;
                    }
                }
                if (best_data_num == 0) best_data_num = smallest_mtu_safe_data_num;
                if (best_data_num == 0) best_data_num = tail_x;  // retain the legacy oversized-MTU fallback
                actual_data_num = best_data_num;
                assert(best_data_num >= 1 && best_data_num <= fec_par.rs_cnt);
                actual_redundant_num = fec_par.rs_par[best_data_num - 1].y;
            }

            assert(blob_encode.output(actual_data_num, blob_output, fec_len) == 0);

            if (debug_fec_enc)
                mylog(log_debug, "[enc]seq=%08x x=%d y=%d len=%d cnt=%d\n", seq, actual_data_num, actual_redundant_num, fec_len, counter);
            else
                mylog(log_trace, "[enc]seq=%08x x=%d y=%d len=%d cnt=%d\n", seq, actual_data_num, actual_redundant_num, fec_len, counter);
        } else {
            assert(counter <= fec_par.rs_cnt);
            actual_data_num = counter;
            actual_redundant_num = fec_par.rs_par[counter - 1].y;

            int sum_ori = 0;
            for (int i = 0; i < counter; i++) {
                sum_ori += input_len[i];
                assert(input_len[i] >= 0);
                if (input_len[i] > fec_len) fec_len = input_len[i];
            }

            int sum = fec_len * counter;

            if (debug_fec_enc)
                mylog(log_debug, "[enc]seq=%08x x=%d y=%d len=%d sum_ori=%d sum=%d\n", seq, actual_data_num, actual_redundant_num, fec_len, sum_ori, sum);
            else
                mylog(log_trace, "[enc]seq=%08x x=%d y=%d len=%d sum_ori=%d sum=%d\n", seq, actual_data_num, actual_redundant_num, fec_len, sum_ori, sum);
        }

        // mylog(log_trace,"%d %d %d\n",actual_data_num,actual_redundant_num,fec_len);

        // Allocate only the slots needed by this completed group. This keeps
        // an idle encoder small while making all pointers below stable.
        input_slot(actual_data_num + actual_redundant_num - 1);
        char *tmp_output_buf[max_fec_packet_num + 5] = {0};
        for (int i = 0; i < actual_data_num + actual_redundant_num; i++) {
            char *slot = input_slot(i);
            int tmp_idx = 0;

            write_u32(slot + tmp_idx, seq);
            tmp_idx += sizeof(u32_t);
            slot[tmp_idx++] = (unsigned char)fec_par.mode;
            if (fec_par.mode == 1 && i < actual_data_num) {
                // A k=1 mode-1 group can be recovered from any one shard.
                // Advertise its actual shape on the systematic frame so a
                // new decoder can directly deliver it without allocating a
                // group. Older decoders already accept this normal k=1 FEC
                // header, so this stays wire compatible.
                if (actual_data_num == 1) {
                    slot[tmp_idx++] = (unsigned char)actual_data_num;
                    slot[tmp_idx++] = (unsigned char)actual_redundant_num;
                } else {
                    slot[tmp_idx++] = (unsigned char)0;
                    slot[tmp_idx++] = (unsigned char)0;
                }
            } else {
                slot[tmp_idx++] = (unsigned char)actual_data_num;
                slot[tmp_idx++] = (unsigned char)actual_redundant_num;
            }
            slot[tmp_idx++] = (unsigned char)i;

            tmp_output_buf[i] = slot + tmp_idx;  //////caution ,trick here.

            if (fec_par.mode == 0) {
                output_len[i] = tmp_idx + fec_len;
                if (i < actual_data_num) {
                    memcpy(slot + tmp_idx, blob_output[i], fec_len);
                }
            } else {
                if (i < actual_data_num) {
                    output_len[i] = tmp_idx + input_len[i];
                    memset(tmp_output_buf[i] + input_len[i], 0, fec_len - input_len[i]);
                } else
                    output_len[i] = tmp_idx + fec_len;
            }
            output_buf[i] = slot;  // output_buf points to same block of memory with different offset
        }

        if (0) {
            printf("seq=%u,fec_len=%d,%d %d,before fec\n", seq, fec_len, actual_data_num, actual_redundant_num);

            for (int i = 0; i < actual_data_num; i++) {
                char *debug_slot = input_slot(i);
                printf("{");
                for (int j = 0; j < 8 + fec_len; j++) {
                    log_bare(log_warn, "0x%02x,", (u32_t)(unsigned char)debug_slot[j]);
                }
                printf("},\n");
                // log_bare(log_warn,"")
            }
        }
        // A k=1 group is pure replication: all systematic and parity shards
        // contain the same bytes.  Avoid the RS matrix lookup/allocation path
        // and reuse the encoder's fixed input buffers directly.  This is a
        // common adaptive-FEC guard/degraded shape for short packets.
        if (actual_data_num == 1) {
            for (int i = 1; i < actual_data_num + actual_redundant_num; i++) {
                memcpy(tmp_output_buf[i], tmp_output_buf[0], fec_len);
            }
        } else {
            rs_encode2(actual_data_num, actual_data_num + actual_redundant_num, tmp_output_buf, fec_len);
        }

        if (0) {
            printf("seq=%u,fec_len=%d,%d %d,after fec\n", seq, fec_len, actual_data_num, actual_redundant_num);
            for (int i = 0; i < actual_data_num + actual_redundant_num; i++) {
                printf("{");
                for (int j = 0; j < 8 + fec_len; j++) {
                    log_bare(log_warn, "0x%02x,", (u32_t)(unsigned char)output_buf[i][j]);
                }
                printf("},\n");
                // log_bare(log_warn,"")
            }
        }

        // mylog(log_trace,"!!! s= %d\n");
        assert(ready_for_output == 0);
        ready_for_output = 1;
        first_packet_time_for_output = first_packet_time;
        first_packet_time = 0;
        seq++;
        counter = 0;
        output_n = actual_data_num + actual_redundant_num;
        blob_encode.clear();

        my_itimerspec its;
        memset(&its, 0, sizeof(its));
        ev_timer_stop(loop, &timer);
        // timerfd_settime(timer_fd,TFD_TIMER_ABSTIME,&its,0);

        if (encode_fast_send && fec_par.mode == 1) {
            int packet_to_send[max_fec_packet_num + 5] = {0};
            int packet_to_send_counter = 0;

            // assert(counter!=0);
            if (s != 0)
                packet_to_send[packet_to_send_counter++] = actual_data_num - 1;
            for (int i = actual_data_num; i < actual_data_num + actual_redundant_num; i++) {
                packet_to_send[packet_to_send_counter++] = i;
            }
            output_n = packet_to_send_counter;  // re write
            for (int i = 0; i < packet_to_send_counter; i++) {
                output_buf[i] = output_buf[packet_to_send[i]];
                output_len[i] = output_len[packet_to_send[i]];
            }
        }
    } else {
        if (encode_fast_send && s != 0 && fec_par.mode == 1) {
            assert(counter >= 1);
            assert(counter <= 255);
            int input_buf_idx = counter - 1;
            assert(ready_for_output == 0);
            ready_for_output = 1;
            first_packet_time_for_output = 0;
            output_n = 1;

            int tmp_idx = 0;
            char *slot = input_slot(input_buf_idx);
            write_u32(slot + tmp_idx, seq);
            tmp_idx += sizeof(u32_t);

            slot[tmp_idx++] = (unsigned char)fec_par.mode;
            slot[tmp_idx++] = (unsigned char)0;
            slot[tmp_idx++] = (unsigned char)0;
            slot[tmp_idx++] = (unsigned char)((u32_t)input_buf_idx);

            output_len[0] = input_len[input_buf_idx] + tmp_idx;
            output_buf[0] = slot;

            if (0) {
                printf("seq=%u,buf_idx=%d\n", seq, input_buf_idx);
                for (int j = 0; j < output_len[0]; j++) {
                    log_bare(log_warn, "0x%02x,", (u32_t)(unsigned char)output_buf[0][j]);
                }
                printf("\n");
            }
        }
    }

    if (s != 0 && delayed_append) {
        assert(fec_par.mode != 1);
        append(s, len);
    }

    return 0;
}

int fec_encode_manager_t::output(int &n, char **&s_arr, int *&len) {
    if (!ready_for_output) {
        n = -1;
        len = 0;
        s_arr = 0;
    } else {
        n = output_n;
        len = output_len;
        s_arr = output_buf;
        ready_for_output = 0;
    }
    return 0;
}
/*
int fec_decode_manager_t::re_init()
{
        clear();
        return 0;
}*/

int validate_fec_frame(const char *s, int len) {
    const int header_len = sizeof(u32_t) + 4 * sizeof(char);
    if (s == 0 || len < header_len) return -1;

    int type = (unsigned char)s[sizeof(u32_t)];
    int data_num = (unsigned char)s[sizeof(u32_t) + 1];
    int redundant_num = (unsigned char)s[sizeof(u32_t) + 2];
    int inner_index = (unsigned char)s[sizeof(u32_t) + 3];
    int payload_len = len - header_len;
    if (type != 0 && type != 1) return -1;
    if (type == 0 && data_num == 0) return -1;
    if (data_num + redundant_num >= max_fec_packet_num) return -1;

    if (type == 0) return inner_index < data_num + redundant_num ? 0 : -1;

    if (payload_len < (int)sizeof(u16_t)) return -1;
    if (data_num == 0) {
        return redundant_num == 0 && (int)(read_u16((char *)s + header_len) + sizeof(u16_t)) == payload_len ? 0 : -1;
    }
    return inner_index < data_num + redundant_num ? 0 : -1;
}

int fec_decode_manager_t::clear() {
    // Decoders can be destroyed while incomplete groups are retained.
    // Return their quota before clearing the containers themselves.
    assert(fec_decode_global_retained_payload_bytes >= retained_payload_bytes);
    assert(fec_decode_global_retained_shard_count >= retained_shard_count);
    fec_decode_global_retained_payload_bytes -= retained_payload_bytes;
    fec_decode_global_retained_shard_count -= retained_shard_count;
    anti_replay.clear();
    mp.clear();
    mp.rehash(0);
    retained_payload_bytes = 0;
    retained_shard_count = 0;
    has_completed_group = 0;
    blob_decode.release_memory();
    ready_for_output = 0;
    statistics = fec_decode_stats_t();

    return 0;
}

void fec_decode_manager_t::erase_group(unordered_map<u32_t, fec_group_t>::iterator it, int account_loss) {
    u32_t seq = it->first;
    fec_group_t &group = it->second;
    int count = (int)group.group_mp.size();
    if (account_loss && group.data_num > 0 && count < group.data_num) {
        statistics.unrecoverable_packets += group.data_num;
        if (debug_fec_dec)
            mylog(log_debug, "[dec][failed]seq=%08x x=%d y=%d cnt=%d\n", seq, group.data_num, group.redundant_num, count);
        else
            mylog(log_trace, "[dec][failed]seq=%08x x=%d y=%d cnt=%d\n", seq, group.data_num, group.redundant_num, count);
    }
    for (auto shard = group.group_mp.begin(); shard != group.group_mp.end(); ++shard) {
        retained_payload_bytes -= shard->second.buf.size();
        fec_decode_global_retained_payload_bytes -= shard->second.buf.size();
    }
    retained_shard_count -= group.group_mp.size();
    fec_decode_global_retained_shard_count -= group.group_mp.size();
    anti_replay.set_invalid(seq);
    mp.erase(it);
}

int fec_decode_manager_t::make_room(size_t bytes, u32_t protected_seq) {
    if (bytes > fec_decode_payload_limit) return -1;
    while (retained_shard_count >= fec_buff_num || retained_payload_bytes + bytes > fec_decode_payload_limit ||
           fec_decode_global_retained_shard_count >= fec_decode_global_shard_limit ||
           fec_decode_global_retained_payload_bytes + bytes > fec_decode_global_payload_limit) {
        auto victim = mp.end();
        for (auto it = mp.begin(); it != mp.end(); ++it) {
            if (it->first == protected_seq) continue;
            if (victim == mp.end() || it->second.first_seen_time < victim->second.first_seen_time) victim = it;
        }
        if (victim == mp.end()) return -1;
        erase_group(victim, 1);
    }
    return 0;
}

void fec_decode_manager_t::release_output_storage() {
    if (ready_for_output) return;
    if (has_completed_group) {
        auto it = mp.find(completed_group_seq);
        if (it != mp.end()) erase_group(it, 0);
        has_completed_group = 0;
    }
    if (mp.empty()) blob_decode.release_memory();
}

int fec_decode_manager_t::input(char *s, int len) {
    assert(s != 0);
    assert(len + 100 < buf_len);  // guaranteed by upper level
    release_output_storage();
    if (validate_fec_frame(s, len) != 0) {
        mylog(log_warn, "invalid fec frame\n");
        return -1;
    }

    const int header_len = sizeof(u32_t) + 4 * sizeof(char);
    u32_t seq = read_u32(s);
    int type = (unsigned char)s[sizeof(u32_t)];
    int data_num = (unsigned char)s[sizeof(u32_t) + 1];
    int redundant_num = (unsigned char)s[sizeof(u32_t) + 2];
    int inner_index = (unsigned char)s[sizeof(u32_t) + 3];
    int payload_len = len - header_len;

    if (!anti_replay.is_valid(seq)) {
        mylog(log_trace, "!anti_replay.is_valid(seq) ,seq =%u\n", seq);
        return 0;
    }

    // k=1 mode-1 frames are replication. They need no retained FEC state.
    if (type == 1 && data_num == 1 && (inner_index == 0 || mp.find(seq) == mp.end())) {
        int direct_len = (int)read_u16(s + header_len);
        if (direct_len != payload_len - (int)sizeof(u16_t) || direct_len > max_data_len) {
            mylog(log_warn, "invalid k=1 replicated frame len=%d payload_len=%d\n", payload_len, direct_len);
            return -1;
        }
        assert(ready_for_output == 0);
        output_n = 1;
        output_s_arr_buf[0] = s + header_len + sizeof(u16_t);
        output_len_arr_buf[0] = direct_len;
        output_s_arr = output_s_arr_buf;
        output_len_arr = output_len_arr_buf;
        ready_for_output = 1;
        statistics.delivered_packets++;
        anti_replay.set_invalid(seq);
        return 0;
    }

    auto group_it = mp.find(seq);
    if (group_it == mp.end()) {
        group_it = mp.emplace(seq, fec_group_t()).first;
        group_it->second.first_seen_time = get_current_time_us();
    }
    fec_group_t &group = group_it->second;
    if (group.group_mp.find(inner_index) != group.group_mp.end()) {
        mylog(log_debug, "dup fec index\n");
        return -1;
    }
    if (group.type == -1)
        group.type = type;
    else if (group.type != type) {
        mylog(log_warn, "type mismatch\n");
        return -1;
    }
    if (data_num != 0) {
        if (group.data_num == -1) {
            group.data_num = data_num;
            group.redundant_num = redundant_num;
            group.len = payload_len;
        } else if (group.data_num != data_num || group.redundant_num != redundant_num || group.len != payload_len) {
            mylog(log_warn, "inconsistent fec group header\n");
            return -1;
        }
    }

    if (make_room(payload_len, seq) != 0) {
        mylog(log_warn, "fec decode memory limit reached, dropping seq=%u\n", seq);
        group_it = mp.find(seq);
        if (group_it != mp.end()) erase_group(group_it, 1);
        return -1;
    }
    fec_data_t shard;
    shard.len = payload_len;
    shard.buf.assign(s + header_len, s + header_len + payload_len);
    retained_payload_bytes += shard.buf.size();
    retained_shard_count++;
    fec_decode_global_retained_payload_bytes += shard.buf.size();
    fec_decode_global_retained_shard_count++;
    group.group_mp.emplace(inner_index, std::move(shard));
    if (group.highest_inner_index >= 0 && inner_index < group.highest_inner_index) statistics.reordered_packets++;
    if (inner_index > group.highest_inner_index) group.highest_inner_index = inner_index;

    int about_to_fec = 0;
    if (type == 0) {
        if ((int)group.group_mp.size() > data_num) {
            mylog(log_warn, "inner_mp.size()>data_num\n");
            erase_group(mp.find(seq), 1);
            return -1;
        }
        about_to_fec = (int)group.group_mp.size() == data_num;
    } else if (group.data_num != -1) {
        if ((int)group.group_mp.size() > group.data_num + 1) {
            mylog(log_warn, "inner_mp.size()>data_num+1\n");
            erase_group(mp.find(seq), 1);
            return -1;
        }
        about_to_fec = (int)group.group_mp.size() >= group.data_num;
    }

    if (!about_to_fec) {
        if (decode_fast_send && type == 1 && data_num == 0) {
            fec_data_t &current = group.group_mp.find(inner_index)->second;
            assert(ready_for_output == 0);
            output_n = 1;
            output_s_arr_buf[0] = current.buf.data() + sizeof(u16_t);
            output_len_arr_buf[0] = current.len - sizeof(u16_t);
            output_s_arr = output_s_arr_buf;
            output_len_arr = output_len_arr_buf;
            ready_for_output = 1;
        }
        return 0;
    }

    int group_data_num = group.data_num;
    int group_redundant_num = group.redundant_num;
    int x_got = 0;
    int y_got = 0;
    if (type == 0) {
        char *fec_tmp_arr[max_fec_packet_num + 5] = {0};
        for (auto it = group.group_mp.begin(); it != group.group_mp.end(); ++it) {
            if (it->first < group_data_num)
                x_got++;
            else
                y_got++;
            fec_tmp_arr[it->first] = it->second.buf.data();
        }
        if (group_data_num == 1)
            fec_tmp_arr[0] = group.group_mp.begin()->second.buf.data();
        else
            assert(rs_decode2(group_data_num, group_data_num + group_redundant_num, fec_tmp_arr, payload_len) == 0);

        blob_decode.clear();
        for (int i = 0; i < group_data_num; i++) blob_decode.input(fec_tmp_arr[i], payload_len);
        if (blob_decode.output(output_n, output_s_arr, output_len_arr) != 0) {
            mylog(log_warn, "blob_decode failed\n");
            erase_group(mp.find(seq), 1);
            return -1;
        }
        statistics.delivered_packets += output_n;
        if (group_data_num > x_got) statistics.recovered_packets += group_data_num - x_got;
        ready_for_output = 1;
    } else {
        int max_len = -1;
        int data_check_ok = 1;
        int missed_packet[max_fec_packet_num + 5];
        int missed_packet_counter = 0;
        for (int i = 0; i < group_data_num + group_redundant_num; i++) output_s_arr_buf[i] = 0;
        for (auto it = group.group_mp.begin(); it != group.group_mp.end(); ++it) {
            if (it->first < group_data_num)
                x_got++;
            else
                y_got++;
            output_s_arr_buf[it->first] = it->second.buf.data();
            if (it->second.len < (int)sizeof(u16_t)) data_check_ok = 0;
            if (it->second.len > max_len) max_len = it->second.len;
        }
        if (max_len != group.len) data_check_ok = 0;
        if (!data_check_ok) {
            mylog(log_warn, "invalid mode-1 fec group\n");
            erase_group(mp.find(seq), 1);
            return -1;
        }
        size_t padding = 0;
        for (auto it = group.group_mp.begin(); it != group.group_mp.end(); ++it) {
            if (max_len > it->second.len) padding += max_len - it->second.len;
        }
        if (padding != 0 && make_room(padding, seq) != 0) {
            mylog(log_warn, "fec decode memory limit reached while normalizing mode-1 shards\n");
            erase_group(mp.find(seq), 1);
            return -1;
        }
        for (auto it = group.group_mp.begin(); it != group.group_mp.end(); ++it) {
            assert(max_len >= it->second.len);
            it->second.buf.resize(max_len, 0);
            output_s_arr_buf[it->first] = it->second.buf.data();
        }
        retained_payload_bytes += padding;
        fec_decode_global_retained_payload_bytes += padding;
        for (int i = 0; i < group_data_num; i++) {
            if (output_s_arr_buf[i] == 0 || i == inner_index) missed_packet[missed_packet_counter++] = i;
        }
        if (group_data_num == 1)
            output_s_arr_buf[0] = group.group_mp.begin()->second.buf.data();
        else
            assert(rs_decode2(group_data_num, group_data_num + group_redundant_num, output_s_arr_buf, max_len) == 0);

        int fec_result_ok = 1;
        for (int i = 0; i < group_data_num; i++) {
            output_len_arr_buf[i] = read_u16(output_s_arr_buf[i]);
            output_s_arr_buf[i] += sizeof(u16_t);
            if (output_len_arr_buf[i] > max_data_len) fec_result_ok = 0;
        }
        if (!fec_result_ok) {
            mylog(log_warn, "invalid recovered mode-1 packet\n");
            erase_group(mp.find(seq), 1);
            return -1;
        }
        statistics.delivered_packets += group_data_num;
        if (group_data_num > x_got) statistics.recovered_packets += group_data_num - x_got;
        output_n = group_data_num;
        if (decode_fast_send) {
            output_n = missed_packet_counter;
            for (int i = 0; i < missed_packet_counter; i++) {
                output_s_arr_buf[i] = output_s_arr_buf[missed_packet[i]];
                output_len_arr_buf[i] = output_len_arr_buf[missed_packet[i]];
            }
        }
        output_s_arr = output_s_arr_buf;
        output_len_arr = output_len_arr_buf;
        ready_for_output = 1;
    }

    anti_replay.set_invalid(seq);
    completed_group_seq = seq;
    has_completed_group = 1;
    return 0;
}
int fec_decode_manager_t::output(int &n, char **&s_arr, int *&len_arr) {
    if (!ready_for_output) {
        n = -1;
        s_arr = 0;
        len_arr = 0;
    } else {
        ready_for_output = 0;
        n = output_n;
        s_arr = output_s_arr;
        len_arr = output_len_arr;
    }
    return 0;
}

int fec_decode_manager_t::expire_incomplete_groups(my_time_t maximum_age_us) {
    if (maximum_age_us == 0) return 0;
    my_time_t now = get_current_time_us();
    for (auto it = mp.begin(); it != mp.end();) {
        fec_group_t &group = it->second;
        if (group.data_num > 0 && group.first_seen_time != 0 && now - group.first_seen_time >= maximum_age_us && (int)group.group_mp.size() < group.data_num) {
            auto erase_it = it++;
            erase_group(erase_it, 1);
        } else {
            ++it;
        }
    }
    return 0;
}
