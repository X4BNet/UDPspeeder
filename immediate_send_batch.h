/*
 * A callback-owned, allocation-free send staging area.  recvmmsg can return
 * many packets in one readiness notification; decoded output must be copied
 * before the next decoder invocation reuses its storage, then it can be sent
 * through delay_send_batch without a timer or aggregation delay.
 */

#ifndef IMMEDIATE_SEND_BATCH_H_
#define IMMEDIATE_SEND_BATCH_H_

#include "receive_batch.h"

class immediate_send_batch_t : not_copy_able_t {
    static const int capacity = max_receive_packets_per_callback;

    dest_t destination;
    char storage[capacity][buf_len];
    char *data[capacity];
    int len[capacity];
    my_time_t delay[capacity];
    int count;

   public:
    immediate_send_batch_t();

    // Packets remain in arrival order. A UDPspeeder instance has one egress
    // destination for a callback, so the first packet establishes the batch
    // destination and subsequent packets only need to be copied.
    int add(my_time_t packet_delay, const dest_t &dest, const char *packet, int packet_len);
    int flush();
    void clear();
};

int immediate_send_batch_unit_test();

#endif /* IMMEDIATE_SEND_BATCH_H_ */
