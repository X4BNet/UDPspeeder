/*
 * receive_batch.h
 *
 * Linux UDP receive batching with an allocation-free fallback path.  The
 * caller owns the object and processes every returned packet before calling
 * receive() again, so packet storage remains valid for the whole callback.
 */

#ifndef RECEIVE_BATCH_H_
#define RECEIVE_BATCH_H_

#include "common.h"

const int max_receive_batch = 64;
const int max_receive_packets_per_callback = 128;

struct io_batch_statistics_t {
    u64_t recvmmsg_calls = 0;
    u64_t recvmmsg_packets = 0;
    u64_t recvmmsg_eagain = 0;
    u64_t recvmmsg_fallbacks = 0;

    u64_t sendmmsg_calls = 0;
    u64_t sendmmsg_packets = 0;
    u64_t sendmmsg_partial_calls = 0;
    u64_t sendmmsg_eagain = 0;
    u64_t sendmmsg_fallback_packets = 0;

    u64_t udp_gso_calls = 0;
    u64_t udp_gso_packets = 0;
    u64_t udp_gso_bytes = 0;
    u64_t udp_gso_partial_calls = 0;
    u64_t udp_gso_fallback_packets = 0;

    void clear();
};

extern int recvmmsg_batch;
extern io_batch_statistics_t io_batch_statistics;

int get_receive_batch_size();
void report_io_batch_statistics(const char *role);

struct received_datagram_t {
    char data[max_data_len + 1];
    address_t::storage_t address;
    socklen_t address_len;
    int len;
};

class udp_receive_batch_t : not_copy_able_t {
    int address_enabled;

#if defined(__linux__)
    mmsghdr messages[max_receive_batch];
    iovec iovecs[max_receive_batch];
#endif

   public:
    received_datagram_t packets[max_receive_batch];

    udp_receive_batch_t();

    // Returns zero when the non-blocking socket has no packet ready, a
    // positive packet count on success, and -1 for a non-recoverable error.
    int receive(int fd, int want_address, int maximum_packets);
};

int receive_batch_unit_test();

#endif /* RECEIVE_BATCH_H_ */
