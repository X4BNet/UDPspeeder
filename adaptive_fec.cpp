/*
 * adaptive_fec.cpp
 */

#include "adaptive_fec.h"
#include "packet.h"

namespace {

const u32_t adaptive_fec_magic = 0x41464543;  // "AFEC"
const int adaptive_fec_header_len = sizeof(u32_t) + 4 * sizeof(char) + sizeof(u32_t);
const int adaptive_fec_legacy_hello_without_mac_len = sizeof(u32_t) + 4 * sizeof(char) + sizeof(u32_t) + sizeof(u32_t) + 2 * sizeof(char);
const int adaptive_fec_hello = 1;
const int adaptive_fec_feedback = 2;
const int adaptive_fec_bypass_frame = 3;
const int adaptive_fec_mac_len = 2 * sizeof(u32_t);
const int adaptive_fec_legacy_hello_len = adaptive_fec_legacy_hello_without_mac_len + adaptive_fec_mac_len;
const int adaptive_fec_feedback_len = adaptive_fec_header_len + 4 * sizeof(u32_t) + adaptive_fec_mac_len;
const int adaptive_fec_bypass_header_len = adaptive_fec_header_len + sizeof(u32_t);

// This only protects the opt-in control plane when -k is omitted.  It must
// not become UDPspeeder's global key: doing that would silently enable XOR
// and break an otherwise compatible static-FEC peer.  Production deployments
// should still use a matching private -k value on both peers.
const char adaptive_fec_default_control_key[] = "UDPspeeder-adaptive-fec-v1";

const char *state_name(adaptive_fec_state_t state) {
    switch (state) {
        case adaptive_fec_normal:
            return "normal";
        case adaptive_fec_guard:
            return "guard";
        case adaptive_fec_degraded:
            return "degraded";
        case adaptive_fec_recover:
            return "recover";
        default:
            return "unknown";
    }
}

int parse_profile(fec_parameter_t &profile, const char *value) {
    if (value == 0 || value[0] == 0) return 0;

    char buffer[rs_str_len];
    if (strlen(value) >= sizeof(buffer)) return -1;
    strcpy(buffer, value);
    return profile.rs_from_str(buffer);
}

int profile_has_no_redundancy(const fec_parameter_t &profile) {
    for (int i = 0; i < profile.rs_cnt; i++) {
        if (profile.rs_par[i].y != 0) return 0;
    }
    return 1;
}

void write_control_header(char *buffer, u32_t sequence, int kind) {
    write_u32(buffer, sequence);
    buffer[sizeof(u32_t)] = (char)adaptive_fec_frame_type;
    buffer[sizeof(u32_t) + 1] = (char)kind;
    buffer[sizeof(u32_t) + 2] = (char)adaptive_fec_protocol_version;
    buffer[sizeof(u32_t) + 3] = 0;
    write_u32(buffer + sizeof(u32_t) + 4 * sizeof(char), adaptive_fec_magic);
}

int saturating_add(u32_t &target, u32_t value) {
    if (u32_t(-1) - target < value)
        target = u32_t(-1);
    else
        target += value;
    return 0;
}

u64_t rotate_left(u64_t value, int amount) {
    return (value << amount) | (value >> (64 - amount));
}

u64_t load_little_endian_u64(const unsigned char *data) {
    u64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value |= u64_t(data[i]) << (8 * i);
    }
    return value;
}

void sip_round(u64_t &v0, u64_t &v1, u64_t &v2, u64_t &v3) {
    v0 += v1;
    v1 = rotate_left(v1, 13);
    v1 ^= v0;
    v0 = rotate_left(v0, 32);
    v2 += v3;
    v3 = rotate_left(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = rotate_left(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = rotate_left(v1, 17);
    v1 ^= v2;
    v2 = rotate_left(v2, 32);
}

u64_t adaptive_fec_mac(const char *data, int length) {
    const char *configured_key = key_string[0] == 0 ? adaptive_fec_default_control_key : key_string;
    assert(strlen(configured_key) >= 16);
    const unsigned char *key = (const unsigned char *)configured_key;
    u64_t k0 = load_little_endian_u64(key);
    u64_t k1 = load_little_endian_u64(key + 8);

    u64_t v0 = 0x736f6d6570736575ULL ^ k0;
    u64_t v1 = 0x646f72616e646f6dULL ^ k1;
    u64_t v2 = 0x6c7967656e657261ULL ^ k0;
    u64_t v3 = 0x7465646279746573ULL ^ k1;
    const unsigned char *input = (const unsigned char *)data;
    int remaining = length;
    while (remaining >= 8) {
        u64_t message = load_little_endian_u64(input);
        v3 ^= message;
        sip_round(v0, v1, v2, v3);
        sip_round(v0, v1, v2, v3);
        v0 ^= message;
        input += 8;
        remaining -= 8;
    }

    u64_t last = u64_t(length) << 56;
    for (int i = 0; i < remaining; i++) {
        last |= u64_t(input[i]) << (8 * i);
    }
    v3 ^= last;
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    v0 ^= last;
    v2 ^= 0xff;
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

void append_mac(char *data, int &length) {
    u64_t mac = adaptive_fec_mac(data, length);
    write_u32(data + length, (u32_t)(mac >> 32));
    write_u32(data + length + sizeof(u32_t), (u32_t)mac);
    length += adaptive_fec_mac_len;
}

int validate_mac(const char *data, int length) {
    if (length < adaptive_fec_mac_len) return 0;
    int payload_length = length - adaptive_fec_mac_len;
    u64_t expected = (u64_t(read_u32((char *)data + payload_length)) << 32) | read_u32((char *)data + payload_length + sizeof(u32_t));
    return adaptive_fec_mac(data, payload_length) == expected;
}

}  // namespace

adaptive_fec_config_t g_adaptive_fec_config;

int adaptive_fec_config_t::configure(const fec_parameter_t &base, const char *normal_profile, const char *guard_profile, const char *degraded_profile) {
    normal.clone(base);
    guard.clone(base);
    degraded.clone(base);

    for (int i = 0; i < normal.rs_cnt; i++) {
        normal.rs_par[i].y = 0;
    }

    if (guard_profile == 0 || guard_profile[0] == 0) {
        for (int i = 0; i < guard.rs_cnt; i++) {
            if (guard.rs_par[i].y > 0) guard.rs_par[i].y = (guard.rs_par[i].y + 2) / 3;
        }
    }

    if (parse_profile(normal, normal_profile) != 0 || parse_profile(guard, guard_profile) != 0 || parse_profile(degraded, degraded_profile) != 0) {
        return -1;
    }

    return 0;
}

adaptive_fec_controller_t::adaptive_fec_controller_t() {
    config = g_adaptive_fec_config;
    state = adaptive_fec_normal;
    peer_capable = 0;
    profile_dirty = 0;
    hello_response_pending = 0;
    bypass_active_logged = 0;
    hello_attempts = 0;
    clean_windows = 0;
    last_hello_time = 0;
    last_feedback_time = 0;
    state_changed_time = get_current_time_us();
    control_sequence = get_fake_random_number();
    bypass_sequence = get_fake_random_number();
    received_bypass_sequence = 0;
    last_received_bypass_sequence = 0;
    window_stats = fec_decode_stats_t();
}

const fec_parameter_t &adaptive_fec_controller_t::profile_for_state() const {
    switch (state) {
        case adaptive_fec_normal:
            return config.normal;
        case adaptive_fec_guard:
        case adaptive_fec_recover:
            return config.guard;
        case adaptive_fec_degraded:
        default:
            return config.degraded;
    }
}

void adaptive_fec_controller_t::set_state(adaptive_fec_state_t new_state, const char *reason) {
    if (state == new_state) return;

    mylog(log_info, "adaptive-fec state %s -> %s (%s)\n", state_name(state), state_name(new_state), reason);
    state = new_state;
    if (new_state != adaptive_fec_normal) bypass_active_logged = 0;
    state_changed_time = get_current_time_us();
    clean_windows = 0;
    profile_dirty = 1;
}

int adaptive_fec_controller_t::can_bypass() const {
    return config.enabled && peer_capable && state == adaptive_fec_normal && profile_has_no_redundancy(config.normal);
}

int adaptive_fec_controller_t::take_profile_update(fec_parameter_t &profile) {
    if (!config.enabled || !peer_capable || !profile_dirty) return 0;

    profile.clone(profile_for_state());
    profile_dirty = 0;
    return 1;
}

void adaptive_fec_controller_t::observe_decoder_stats(const fec_decode_stats_t &stats) {
    if (!config.enabled) return;

    saturating_add(window_stats.delivered_packets, stats.delivered_packets);
    saturating_add(window_stats.recovered_packets, stats.recovered_packets);
    saturating_add(window_stats.unrecoverable_packets, stats.unrecoverable_packets);
    saturating_add(window_stats.reordered_packets, stats.reordered_packets);
}

void adaptive_fec_controller_t::note_bypass_sequence(u32_t sequence) {
    if (!received_bypass_sequence) {
        received_bypass_sequence = 1;
        last_received_bypass_sequence = sequence;
        saturating_add(window_stats.delivered_packets, 1);
        return;
    }

    if (sequence == last_received_bypass_sequence) {
        saturating_add(window_stats.reordered_packets, 1);
        return;
    }

    u32_t delta = sequence - last_received_bypass_sequence;
    if (delta < 0x80000000U) {
        if (delta > 1) saturating_add(window_stats.unrecoverable_packets, delta - 1);
        last_received_bypass_sequence = sequence;
    } else {
        saturating_add(window_stats.reordered_packets, 1);
    }
    saturating_add(window_stats.delivered_packets, 1);
}

void adaptive_fec_controller_t::update_from_feedback(const fec_decode_stats_t &stats) {
    u32_t samples = stats.delivered_packets + stats.unrecoverable_packets;
    if (samples < (u32_t)config.minimum_samples) return;

    u32_t unrecoverable_per_mille = (u32_t)((u64_t)stats.unrecoverable_packets * 1000 / samples);
    u32_t recovered_per_mille = (u32_t)((u64_t)stats.recovered_packets * 1000 / samples);
    u32_t reordered_per_mille = (u32_t)((u64_t)stats.reordered_packets * 1000 / samples);

    mylog(log_debug, "adaptive-fec feedback samples=%u delivered=%u recovered=%u unrecoverable=%u reordered=%u\n",
          samples, stats.delivered_packets, stats.recovered_packets, stats.unrecoverable_packets, stats.reordered_packets);

    int severe = unrecoverable_per_mille >= 30 || recovered_per_mille >= 120;
    int impaired = unrecoverable_per_mille >= 10 || recovered_per_mille >= 20 || reordered_per_mille >= 50;
    int clean = unrecoverable_per_mille == 0 && recovered_per_mille == 0 && reordered_per_mille < 10;

    if (severe) {
        set_state(adaptive_fec_degraded, "receiver loss/recovery threshold");
        return;
    }
    if (impaired) {
        // A degraded link needs a clean-window recovery path. Do not step
        // down to Guard merely because its latest window is less severe.
        if (state != adaptive_fec_degraded) set_state(adaptive_fec_guard, "receiver loss/reordering threshold");
        return;
    }

    if (!clean) {
        clean_windows = 0;
        return;
    }

    clean_windows++;
    if ((state == adaptive_fec_guard || state == adaptive_fec_degraded) && clean_windows >= 3) {
        set_state(adaptive_fec_recover, "three clean receiver windows");
        return;
    }
    if (state == adaptive_fec_recover && clean_windows >= 3 && get_current_time_us() - state_changed_time >= (my_time_t)config.recover_hold_us) {
        set_state(adaptive_fec_normal, "recovery hold elapsed");
    }
}

int adaptive_fec_controller_t::build_hello(char *&data, int &len) {
    // The probe is a valid legacy type-0, k=1/n=1 FEC group whose blob has
    // zero payloads.  A stock peer decodes and discards it without seeing an
    // unknown control type, while an upgraded peer recognizes the AFEC tail
    // before handing it to the FEC decoder.
    write_u32(control_buffer, control_sequence++);
    control_buffer[sizeof(u32_t)] = 0;
    control_buffer[sizeof(u32_t) + 1] = 1;
    control_buffer[sizeof(u32_t) + 2] = 0;
    control_buffer[sizeof(u32_t) + 3] = 0;
    write_u32(control_buffer + sizeof(u32_t) + 4 * sizeof(char), 0);
    write_u32(control_buffer + sizeof(u32_t) + 4 * sizeof(char) + sizeof(u32_t), adaptive_fec_magic);
    control_buffer[sizeof(u32_t) + 4 * sizeof(char) + 2 * sizeof(u32_t)] = (char)adaptive_fec_hello;
    control_buffer[sizeof(u32_t) + 4 * sizeof(char) + 2 * sizeof(u32_t) + 1] = (char)adaptive_fec_protocol_version;
    data = control_buffer;
    len = adaptive_fec_legacy_hello_without_mac_len;
    append_mac(data, len);
    assert(len == adaptive_fec_legacy_hello_len);
    return 1;
}

int adaptive_fec_controller_t::build_feedback(char *&data, int &len) {
    write_control_header(control_buffer, control_sequence++, adaptive_fec_feedback);
    int offset = adaptive_fec_header_len;
    write_u32(control_buffer + offset, window_stats.delivered_packets);
    offset += sizeof(u32_t);
    write_u32(control_buffer + offset, window_stats.recovered_packets);
    offset += sizeof(u32_t);
    write_u32(control_buffer + offset, window_stats.unrecoverable_packets);
    offset += sizeof(u32_t);
    write_u32(control_buffer + offset, window_stats.reordered_packets);
    offset += sizeof(u32_t);
    append_mac(control_buffer, offset);
    assert(offset == adaptive_fec_feedback_len);
    window_stats = fec_decode_stats_t();
    data = control_buffer;
    len = offset;
    return 1;
}

int adaptive_fec_controller_t::build_pending_control(char *&data, int &len) {
    if (!config.enabled) return 0;

    my_time_t now = get_current_time_us();
    if (hello_response_pending || (!peer_capable && hello_attempts < 3 && (last_hello_time == 0 || now - last_hello_time >= (my_time_t)config.feedback_interval_us))) {
        last_hello_time = now;
        if (!hello_response_pending) hello_attempts++;
        hello_response_pending = 0;
        return build_hello(data, len);
    }

    u32_t samples = window_stats.delivered_packets + window_stats.unrecoverable_packets;
    if (peer_capable && samples >= (u32_t)config.minimum_samples && (last_feedback_time == 0 || now - last_feedback_time >= (my_time_t)config.feedback_interval_us)) {
        last_feedback_time = now;
        return build_feedback(data, len);
    }
    return 0;
}

int adaptive_fec_controller_t::build_bypass(char *payload, int payload_len, char *&data, int &len) {
    if (!can_bypass() || payload_len < 0 || payload_len + adaptive_fec_bypass_header_len >= (int)sizeof(bypass_buffer)) return -1;

    write_control_header(bypass_buffer, control_sequence++, adaptive_fec_bypass_frame);
    write_u32(bypass_buffer + adaptive_fec_header_len, bypass_sequence++);
    memcpy(bypass_buffer + adaptive_fec_bypass_header_len, payload, payload_len);
    data = bypass_buffer;
    len = payload_len + adaptive_fec_bypass_header_len;
    append_mac(data, len);
    if (!bypass_active_logged) {
        mylog(log_info, "adaptive-fec direct bypass active\n");
        bypass_active_logged = 1;
    }
    return 0;
}

adaptive_fec_inbound_result_t adaptive_fec_controller_t::process_inbound(char *data, int len, char *&payload, int &payload_len) {
    if (!config.enabled || len < (int)(sizeof(u32_t) + 4 * sizeof(char))) return adaptive_fec_not_handled;

    int is_legacy_hello = len == adaptive_fec_legacy_hello_len &&
                          (unsigned char)data[sizeof(u32_t)] == 0 &&
                          (unsigned char)data[sizeof(u32_t) + 1] == 1 &&
                          (unsigned char)data[sizeof(u32_t) + 2] == 0 &&
                          (unsigned char)data[sizeof(u32_t) + 3] == 0 &&
                          read_u32(data + sizeof(u32_t) + 4 * sizeof(char)) == 0 &&
                          read_u32(data + sizeof(u32_t) + 4 * sizeof(char) + sizeof(u32_t)) == adaptive_fec_magic &&
                          (unsigned char)data[sizeof(u32_t) + 4 * sizeof(char) + 2 * sizeof(u32_t)] == adaptive_fec_hello &&
                          (unsigned char)data[sizeof(u32_t) + 4 * sizeof(char) + 2 * sizeof(u32_t) + 1] == adaptive_fec_protocol_version;

    if (is_legacy_hello) {
        if (!validate_mac(data, len)) {
            mylog(log_warn, "invalid adaptive-fec capability probe\n");
            return adaptive_fec_consumed;
        }
        if (!peer_capable) {
            peer_capable = 1;
            profile_dirty = 1;
            mylog(log_info, "adaptive-fec peer capability confirmed\n");
        }
        hello_response_pending = 1;
        return adaptive_fec_consumed;
    }

    if ((unsigned char)data[sizeof(u32_t)] != adaptive_fec_frame_type) return adaptive_fec_not_handled;

    if (len < adaptive_fec_header_len + adaptive_fec_mac_len || (unsigned char)data[sizeof(u32_t) + 2] != adaptive_fec_protocol_version || read_u32(data + sizeof(u32_t) + 4 * sizeof(char)) != adaptive_fec_magic || !validate_mac(data, len)) {
        mylog(log_warn, "invalid adaptive-fec control frame\n");
        return adaptive_fec_consumed;
    }

    int kind = (unsigned char)data[sizeof(u32_t) + 1];
    if (kind == adaptive_fec_hello) {
        if (!peer_capable) {
            peer_capable = 1;
            profile_dirty = 1;
            mylog(log_info, "adaptive-fec peer capability confirmed\n");
        }
        hello_response_pending = 1;
        return adaptive_fec_consumed;
    }
    if (kind == adaptive_fec_feedback) {
        if (len != adaptive_fec_feedback_len) {
            mylog(log_warn, "invalid adaptive-fec feedback length\n");
            return adaptive_fec_consumed;
        }
        fec_decode_stats_t stats;
        int offset = adaptive_fec_header_len;
        stats.delivered_packets = read_u32(data + offset);
        offset += sizeof(u32_t);
        stats.recovered_packets = read_u32(data + offset);
        offset += sizeof(u32_t);
        stats.unrecoverable_packets = read_u32(data + offset);
        offset += sizeof(u32_t);
        stats.reordered_packets = read_u32(data + offset);
        if (!peer_capable) {
            peer_capable = 1;
            profile_dirty = 1;
            mylog(log_info, "adaptive-fec peer capability confirmed\n");
        }
        update_from_feedback(stats);
        return adaptive_fec_consumed;
    }
    if (kind == adaptive_fec_bypass_frame) {
        if (len < adaptive_fec_bypass_header_len + adaptive_fec_mac_len) {
            mylog(log_warn, "invalid adaptive-fec bypass length\n");
            return adaptive_fec_consumed;
        }
        if (!peer_capable) {
            peer_capable = 1;
            profile_dirty = 1;
            mylog(log_info, "adaptive-fec peer capability confirmed\n");
        }
        note_bypass_sequence(read_u32(data + adaptive_fec_header_len));
        payload = data + adaptive_fec_bypass_header_len;
        payload_len = len - adaptive_fec_bypass_header_len - adaptive_fec_mac_len;
        return adaptive_fec_bypass;
    }

    mylog(log_warn, "unknown adaptive-fec control frame\n");
    return adaptive_fec_consumed;
}

int adaptive_fec_unit_test() {
    adaptive_fec_config_t saved = g_adaptive_fec_config;
    char saved_key[sizeof(key_string)];
    strcpy(saved_key, key_string);
    // The built-in control key is deliberately usable when legacy XOR remains
    // disabled.  This is the compatibility/default-key path, not a security
    // assertion; production coverage below is exercised by the end-to-end
    // explicit-key test.
    key_string[0] = 0;
    fec_parameter_t base;
    char base_profile[] = "1:4,2:5,10:14";
    assert(base.rs_from_str(base_profile) == 0);

    g_adaptive_fec_config.enabled = 1;
    g_adaptive_fec_config.feedback_interval_us = 0;
    g_adaptive_fec_config.minimum_samples = 1;
    g_adaptive_fec_config.recover_hold_us = 0;
    assert(g_adaptive_fec_config.configure(base, 0, "1:1,2:1,10:2", 0) == 0);

    adaptive_fec_controller_t sender;
    adaptive_fec_controller_t receiver;
    char *control = 0;
    int control_len = 0;
    char *payload = 0;
    int payload_len = 0;

    assert(sender.build_pending_control(control, control_len) == 1);
    char corrupted_control[128];
    memcpy(corrupted_control, control, control_len);
    corrupted_control[control_len - 1] ^= 1;
    assert(receiver.process_inbound(corrupted_control, control_len, payload, payload_len) == adaptive_fec_consumed);
    assert(!receiver.can_bypass());
    assert(receiver.process_inbound(control, control_len, payload, payload_len) == adaptive_fec_consumed);
    assert(receiver.build_pending_control(control, control_len) == 1);
    assert(sender.process_inbound(control, control_len, payload, payload_len) == adaptive_fec_consumed);
    assert(sender.can_bypass());

    char test_payload[] = "adaptive-fec-bypass";
    assert(sender.build_bypass(test_payload, strlen(test_payload), control, control_len) == 0);
    assert(receiver.process_inbound(control, control_len, payload, payload_len) == adaptive_fec_bypass);
    assert(payload_len == (int)strlen(test_payload));
    assert(memcmp(payload, test_payload, payload_len) == 0);

    // A sequence gap is receiver-observed loss.  It must make the sender
    // leave normal once the feedback is returned.
    assert(sender.build_bypass(test_payload, strlen(test_payload), control, control_len) == 0);
    assert(sender.build_bypass(test_payload, strlen(test_payload), control, control_len) == 0);
    assert(receiver.process_inbound(control, control_len, payload, payload_len) == adaptive_fec_bypass);
    assert(receiver.build_pending_control(control, control_len) == 1);
    assert(sender.process_inbound(control, control_len, payload, payload_len) == adaptive_fec_consumed);
    assert(sender.get_state() == adaptive_fec_degraded || sender.get_state() == adaptive_fec_guard);

    g_adaptive_fec_config = saved;
    strcpy(key_string, saved_key);
    return 0;
}
