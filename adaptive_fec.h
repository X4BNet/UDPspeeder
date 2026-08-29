/*
 * adaptive_fec.h
 *
 * Opt-in, peer-negotiated FEC control.  The existing FEC wire format remains
 * unchanged for data groups.  A new control frame type is only emitted after
 * --adaptive-fec is explicitly enabled; peers that do not answer the bounded
 * capability probes stay on the original configured FEC profile.
 */

#ifndef ADAPTIVE_FEC_H_
#define ADAPTIVE_FEC_H_

#include "fec_manager.h"

const unsigned char adaptive_fec_frame_type = 2;
const unsigned char adaptive_fec_protocol_version = 1;

struct adaptive_fec_config_t {
    int enabled = 0;
    int feedback_interval_us = 500 * 1000;
    int minimum_samples = 32;
    int recover_hold_us = 10 * 1000 * 1000;
    int incomplete_group_timeout_us = 1000 * 1000;

    fec_parameter_t normal;
    fec_parameter_t guard;
    fec_parameter_t degraded;

    int configure(const fec_parameter_t &base, const char *normal_profile, const char *guard_profile, const char *degraded_profile);
};

extern adaptive_fec_config_t g_adaptive_fec_config;
// Counters are intentionally opt-in.  They add a predictable branch to the
// direct path, but must not add per-packet writes to a production tunnel.
extern int adaptive_fec_statistics_enabled;

struct adaptive_fec_statistics_t {
    u64_t bypass_sent_packets = 0;
    u64_t bypass_sent_payload_bytes = 0;
    u64_t bypass_received_packets = 0;
    u64_t bypass_received_payload_bytes = 0;
    u64_t control_sent_packets = 0;
    u64_t control_received_packets = 0;
    u64_t mac_sent_packets = 0;
    u64_t mac_sent_bytes = 0;
    u64_t mac_received_packets = 0;
    u64_t mac_received_bytes = 0;
    u64_t profile_updates = 0;
    u64_t state_transitions = 0;

    void clear() {
        memset(this, 0, sizeof(*this));
    }
};

enum adaptive_fec_state_t {
    adaptive_fec_normal,
    adaptive_fec_guard,
    adaptive_fec_degraded,
    adaptive_fec_recover,
};

enum adaptive_fec_inbound_result_t {
    adaptive_fec_not_handled,
    adaptive_fec_consumed,
    adaptive_fec_bypass,
};

class adaptive_fec_controller_t : not_copy_able_t {
   private:
    adaptive_fec_config_t config;
    adaptive_fec_state_t state;
    int peer_capable;
    int profile_dirty;
    int hello_response_pending;
    int bypass_active_logged;
    int hello_attempts;
    int clean_windows;
    my_time_t last_hello_time;
    my_time_t last_feedback_time;
    my_time_t state_changed_time;
    u32_t control_sequence;
    u32_t bypass_sequence;
    int received_bypass_sequence;
    u32_t next_expected_bypass_sequence;
    u64_t bypass_reorder_bits[4];
    fec_decode_stats_t window_stats;
    adaptive_fec_statistics_t statistics;

    char control_buffer[128];
    char bypass_buffer[buf_len];

    const fec_parameter_t &profile_for_state() const;
    void set_state(adaptive_fec_state_t new_state, const char *reason);
    void update_from_feedback(const fec_decode_stats_t &stats);
    void note_bypass_sequence(u32_t sequence);
    int bypass_reorder_bit(int index) const;
    void set_bypass_reorder_bit(int index);
    void advance_bypass_reorder_window(int count);
    int build_hello(char *&data, int &len);
    int build_feedback(char *&data, int &len);
    void append_mac(char *data, int &length);
    int validate_mac(const char *data, int length);

   public:
    adaptive_fec_controller_t();

    int is_enabled() const {
        return config.enabled;
    }
    int can_bypass() const;
    adaptive_fec_state_t get_state() const {
        return state;
    }
    int take_profile_update(fec_parameter_t &profile);
    void observe_decoder_stats(const fec_decode_stats_t &stats);
    int build_pending_control(char *&data, int &len);
    int build_bypass(char *payload, int payload_len, char *&data, int &len);
    adaptive_fec_inbound_result_t process_inbound(char *data, int len, char *&payload, int &payload_len);
    void report_statistics(const char *role);
};

int adaptive_fec_unit_test();

#endif /* ADAPTIVE_FEC_H_ */
