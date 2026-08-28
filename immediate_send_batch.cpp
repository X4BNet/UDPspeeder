#include "immediate_send_batch.h"

#include "delay_manager.h"
#include "log.h"
#include "misc.h"

immediate_send_batch_t::immediate_send_batch_t() : count(0) {
    destination = dest_t();
}

void immediate_send_batch_t::clear() {
    count = 0;
}

int immediate_send_batch_t::flush() {
    if (count == 0) return 0;

    int result = delay_send_batch(destination, data, len, delay, count);
    count = 0;
    return result;
}

int immediate_send_batch_t::add(my_time_t packet_delay, const dest_t &dest, const char *packet, int packet_len) {
    // FEC and the conversation wrapper can make a valid wire frame slightly
    // larger than max_data_len. Match delay_manager_t::add's actual storage
    // limit rather than silently dropping that edge case.
    if (packet_len < 0 || packet_len + 100 >= buf_len) {
        mylog(log_warn, "immediate send batch packet length invalid: %d\n", packet_len);
        return -1;
    }

    if (count == capacity && flush() != 0) return -1;

    if (count == 0) destination = dest;
    memcpy(storage[count], packet, packet_len);
    data[count] = storage[count];
    len[count] = packet_len;
    delay[count] = packet_delay;
    count++;
    return 0;
}

int immediate_send_batch_unit_test() {
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0 && sender >= 0);

    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(receiver, (sockaddr *)&address, sizeof(address)) == 0);
    socklen_t address_len = sizeof(address);
    assert(getsockname(receiver, (sockaddr *)&address, &address_len) == 0);

    dest_t dest = {};
    dest.type = type_fd_addr;
    dest.inner.fd_addr.fd = sender;
    dest.inner.fd_addr.addr.from_sockaddr((sockaddr *)&address, address_len);

    int saved_sendmmsg = use_sendmmsg;
    io_batch_statistics_t saved_statistics = io_batch_statistics;
    use_sendmmsg = 1;
    io_batch_statistics.clear();

    immediate_send_batch_t batch;
    const char *messages[] = {"batch-one", "batch-two", "batch-three"};
    for (int i = 0; i < 3; i++) {
        assert(batch.add(0, dest, messages[i], strlen(messages[i])) == 0);
    }
    assert(batch.flush() == 0);

    for (int i = 0; i < 3; i++) {
        char received[32];
        int received_len = recv(receiver, received, sizeof(received), 0);
        assert(received_len == (int)strlen(messages[i]));
        assert(memcmp(received, messages[i], received_len) == 0);
    }

#if defined(__linux__)
    assert(io_batch_statistics.sendmmsg_calls == 1);
    assert(io_batch_statistics.sendmmsg_packets == 3);
#endif

    io_batch_statistics = saved_statistics;
    use_sendmmsg = saved_sendmmsg;
    close(sender);
    close(receiver);
    return 0;
}
