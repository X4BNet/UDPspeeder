#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  bench/run-pps-qualification.sh [options]

Runs UDPspeeder through a temporary client/server network-namespace pair
connected by a veth. Every round records real payload bytes, payload PPS,
RTT distribution, CPU ticks, socket-memory/drop state, qdisc state, process
state, logs, and UDPspeeder batch counters.

Options:
  --binary PATH             UDPspeeder binary (default: ./speederv2)
  --client-binary PATH      client binary; defaults to --binary
  --server-binary PATH      server binary; defaults to --binary
  --out DIR                 artifact directory (default: /tmp/udpspeeder-pps-<timestamp>)
  --profile NAME            nofec, fec11, fec14, or adaptive (default: nofec)
  --rate PPS                requested payload PPS (default: 50000)
  --payload BYTES           payload bytes: 256, 512, or 1200 (default: 512)
  --seconds SECONDS         measured duration after warm-up (default: 10)
  --repeat N                repetitions of this case (default: 5)
  --recvmmsg-batch N        1..64 (default: 1)
  --sendmmsg                enable UDPspeeder's opt-in send batching
  --udp-gso                 enable experimental UDP segmentation for equal-size batches
  --udp-gso-segments N      cap UDP datagrams per GSO packet, 2..64 (default: 32)
  --endpoint MODE           echo (default, round-trip RTT) or sink (one-way forwarding PPS)
  --perf                    capture client/server software perf profiles
  --cpus E,S,C,TX,RX        echo, server, client, sender, receiver CPU ids (default: 0,1,2,3,4)
  --netem ARGS              e.g. 'loss 1% delay 20ms 5ms'; applied to both veth egresses
  --matrix                  run 10k/25k/50k PPS x 256/512/1200 bytes x --repeat rounds for all profiles
  --knee                    run 10k/25k/50k/75k/100k PPS x 256/512/1200 bytes for --profile
  -h, --help

The script needs root or passwordless sudo because it creates namespaces,
veth links, and optional netem qdiscs. All faults stay in those namespaces.
EOF
}

binary=./speederv2
client_binary=
server_binary=
out=
profile=nofec
rate=50000
payload=512
seconds=10
repeat=5
receive_batch=1
use_sendmmsg=0
use_udp_gso=0
udp_gso_segments=32
endpoint=echo
use_perf=0
netem=
matrix=0
knee=0
echo_cpu=0
server_cpu=1
client_cpu=2
sender_cpu=3
receiver_cpu=4

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary) binary=$2; shift 2 ;;
        --client-binary) client_binary=$2; shift 2 ;;
        --server-binary) server_binary=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        --profile) profile=$2; shift 2 ;;
        --rate) rate=$2; shift 2 ;;
        --payload) payload=$2; shift 2 ;;
        --seconds) seconds=$2; shift 2 ;;
        --repeat) repeat=$2; shift 2 ;;
        --recvmmsg-batch) receive_batch=$2; shift 2 ;;
        --sendmmsg) use_sendmmsg=1; shift ;;
        --udp-gso) use_udp_gso=1; shift ;;
        --udp-gso-segments) use_udp_gso=1; udp_gso_segments=$2; shift 2 ;;
        --endpoint) endpoint=$2; shift 2 ;;
        --perf) use_perf=1; shift ;;
        --cpus)
            IFS=, read -r echo_cpu server_cpu client_cpu sender_cpu receiver_cpu <<<"$2"
            shift 2
            ;;
        --netem) netem=$2; shift 2 ;;
        --matrix) matrix=1; shift ;;
        --knee) knee=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ ${EUID} -ne 0 ]]; then
    echo "run this privileged harness through sudo" >&2
    exit 2
fi

binary=$(readlink -f "$binary")
[[ -x "$binary" ]] || { echo "not executable: $binary" >&2; exit 2; }
if [[ -z "$client_binary" ]]; then client_binary=$binary; else client_binary=$(readlink -f "$client_binary"); fi
if [[ -z "$server_binary" ]]; then server_binary=$binary; else server_binary=$(readlink -f "$server_binary"); fi
[[ -x "$client_binary" ]] || { echo "not executable: $client_binary" >&2; exit 2; }
[[ -x "$server_binary" ]] || { echo "not executable: $server_binary" >&2; exit 2; }
[[ "$receive_batch" =~ ^[0-9]+$ ]] && ((receive_batch >= 1 && receive_batch <= 64)) || { echo "invalid --recvmmsg-batch" >&2; exit 2; }
[[ "$endpoint" == echo || "$endpoint" == sink ]] || { echo "--endpoint must be echo or sink" >&2; exit 2; }
if [[ $use_perf -eq 1 ]]; then
    command -v perf >/dev/null || { echo "--perf requires perf" >&2; exit 2; }
fi
for cpu in "$echo_cpu" "$server_cpu" "$client_cpu" "$sender_cpu" "$receiver_cpu"; do
    [[ "$cpu" =~ ^[0-9]+$ ]] || { echo "--cpus needs five comma-separated CPU ids" >&2; exit 2; }
done

if [[ -z "$out" ]]; then
    out="/tmp/udpspeeder-pps-$(date -u +%Y%m%dT%H%M%SZ)-$$"
fi
mkdir -p "$out/bin"

root=$(cd "$(dirname "$0")/.." && pwd)
cc -O2 -Wall -Wextra -Werror -o "$out/bin/udp_echo" "$root/bench/udp_echo.c"
cc -O2 -Wall -Wextra -Werror -o "$out/bin/udp_sink" "$root/bench/udp_sink.c"
cc -O2 -Wall -Wextra -Werror -pthread -o "$out/bin/udp_pps_generator" "$root/bench/udp_pps_generator.c"

run_id=$$
client_ns="usp-c-${run_id}"
server_ns="usp-s-${run_id}"
client_veth="usp${run_id: -6}c"
server_veth="usp${run_id: -6}s"
client_ip=10.254.251.1
server_ip=10.254.251.2
server_pid=
client_pid=
echo_pid=
client_perf_record_pid=
server_perf_record_pid=
client_perf_stat_pid=
server_perf_stat_pid=

cleanup_processes() {
    for pid in "${server_pid:-}" "${client_pid:-}" "${echo_pid:-}" "${client_perf_record_pid:-}" "${server_perf_record_pid:-}" "${client_perf_stat_pid:-}" "${server_perf_stat_pid:-}"; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
    done
    wait "${server_pid:-}" "${client_pid:-}" "${echo_pid:-}" "${client_perf_record_pid:-}" "${server_perf_record_pid:-}" "${client_perf_stat_pid:-}" "${server_perf_stat_pid:-}" 2>/dev/null || true
    server_pid=
    client_pid=
    echo_pid=
    client_perf_record_pid=
    server_perf_record_pid=
    client_perf_stat_pid=
    server_perf_stat_pid=
}

cleanup() {
    cleanup_processes
    ip netns del "$client_ns" 2>/dev/null || true
    ip netns del "$server_ns" 2>/dev/null || true
}
trap cleanup EXIT

ip netns add "$client_ns"
ip netns add "$server_ns"
ip link add "$client_veth" type veth peer name "$server_veth"
ip link set "$client_veth" netns "$client_ns"
ip link set "$server_veth" netns "$server_ns"
ip -n "$client_ns" link set lo up
ip -n "$server_ns" link set lo up
ip -n "$client_ns" addr add "$client_ip/30" dev "$client_veth"
ip -n "$server_ns" addr add "$server_ip/30" dev "$server_veth"
ip -n "$client_ns" link set "$client_veth" up
ip -n "$server_ns" link set "$server_veth" up

{
    date -u +%FT%TZ
    uname -a
    sha256sum "$client_binary" "$server_binary"
    "$client_binary" --help | sed -n '1,8p'
    if [[ $use_perf -eq 1 ]]; then perf --version; fi
    sysctl -n net.core.rmem_max net.core.wmem_max
    printf 'endpoint=%s udp_gso=%s udp_gso_segments=%s cpus=endpoint:%s server:%s client:%s sender:%s receiver:%s\n' \
        "$endpoint" "$use_udp_gso" "$udp_gso_segments" "$echo_cpu" "$server_cpu" "$client_cpu" "$sender_cpu" "$receiver_cpu"
} >"$out/environment.txt"

if [[ -n "$netem" ]]; then
    # netem defaults to a 1,000-packet queue. That turns a requested loss
    # test into an unintended burst-drop test after FEC increases wire PPS.
    # Keep an explicitly requested limit, otherwise cover the largest
    # qualified 100 ms-delay FEC burst without allowing host-wide effects.
    if [[ " $netem " =~ [[:space:]]limit[[:space:]] ]]; then
        netem_args=$netem
    else
        netem_args="$netem limit 32768"
    fi
    ip netns exec "$client_ns" tc qdisc replace dev "$client_veth" root netem $netem_args
    ip netns exec "$server_ns" tc qdisc replace dev "$server_veth" root netem $netem_args
fi

ticks() {
    awk '{print $14 + $15}' "/proc/$1/stat"
}

capture_interface_counters() {
    local namespace=$1
    local interface=$2
    local destination=$3
    ip netns exec "$namespace" sh -c '
        for counter in rx_bytes tx_bytes rx_packets tx_packets rx_dropped tx_dropped; do
            printf "%s=" "$counter"
            cat "/sys/class/net/'"$interface"'/statistics/$counter"
        done
    ' >"$destination"
}

counter_value() {
    local file=$1
    local counter=$2
    awk -F= -v counter="$counter" '$1 == counter { print $2; exit }' "$file"
}

profile_args() {
    case "$1" in
        nofec) printf '%s\0' --disable-fec ;;
        fec11) printf '%s\0' -f1:1 --mode 1 --timeout 0 ;;
        fec14) printf '%s\0' -f1:4 --mode 1 --timeout 0 ;;
        adaptive)
            if [[ $use_perf -eq 1 ]]; then
                printf '%s\0' -f1:4 --mode 1 --timeout 0 --adaptive-fec --adaptive-feedback-ms 100 --adaptive-fec-stats
            else
                printf '%s\0' -f1:4 --mode 1 --timeout 0 --adaptive-fec --adaptive-feedback-ms 100
            fi
            ;;
        *) echo "unknown profile: $1" >&2; exit 2 ;;
    esac
}

run_case() {
    local case_name=$1
    local case_profile=$2
    local case_rate=$3
    local case_payload=$4
    local case_iteration=$5
    local dir="$out/$case_name/run-$case_iteration"
    local -a fec_args=()
    local -a send_args=()
    mkdir -p "$dir"
    mapfile -d '' fec_args < <(profile_args "$case_profile")
    [[ $use_sendmmsg -eq 1 ]] && send_args+=(--sendmmsg)
    [[ $use_udp_gso -eq 1 ]] && send_args+=(--udp-gso --udp-gso-segments "$udp_gso_segments")

    if [[ $endpoint == sink ]]; then
        local expected_packets=$((case_rate * seconds * 2 + 1024))
        ip netns exec "$server_ns" taskset -c "$echo_cpu" "$out/bin/udp_sink" "$server_ip" 41000 "$expected_packets" >"$dir/sink.log" 2>&1 & echo_pid=$!
    else
        ip netns exec "$server_ns" taskset -c "$echo_cpu" "$out/bin/udp_echo" "$server_ip" 41000 >"$dir/echo.log" 2>&1 & echo_pid=$!
    fi
    ip netns exec "$server_ns" taskset -c "$server_cpu" "$server_binary" -s -l"$server_ip":41001 -r"$server_ip":41000 \
        "${fec_args[@]}" --sock-buf 10240 --recvmmsg-batch "$receive_batch" "${send_args[@]}" --report 1 --disable-color --log-level 4 >"$dir/server.log" 2>&1 & server_pid=$!
    ip netns exec "$client_ns" taskset -c "$client_cpu" "$client_binary" -c -l"$client_ip":41002 -r"$server_ip":41001 \
        "${fec_args[@]}" --sock-buf 10240 --recvmmsg-batch "$receive_batch" "${send_args[@]}" --report 1 --disable-color --log-level 4 >"$dir/client.log" 2>&1 & client_pid=$!
    sleep 1

    if [[ $use_perf -eq 1 ]]; then
        perf record -q -o "$dir/client.perf.data" -F 499 -g --call-graph dwarf -p "$client_pid" -- sleep "$seconds" >"$dir/client-perf-record.log" 2>&1 & client_perf_record_pid=$!
        perf record -q -o "$dir/server.perf.data" -F 499 -g --call-graph dwarf -p "$server_pid" -- sleep "$seconds" >"$dir/server-perf-record.log" 2>&1 & server_perf_record_pid=$!
        perf stat -x, -o "$dir/client-perf-stat.csv" -e task-clock,context-switches,cpu-migrations,page-faults -p "$client_pid" -- sleep "$seconds" >"$dir/client-perf-stat.log" 2>&1 & client_perf_stat_pid=$!
        perf stat -x, -o "$dir/server-perf-stat.csv" -e task-clock,context-switches,cpu-migrations,page-faults -p "$server_pid" -- sleep "$seconds" >"$dir/server-perf-stat.log" 2>&1 & server_perf_stat_pid=$!
    fi

    local client_before server_before client_after server_after
    client_before=$(ticks "$client_pid")
    server_before=$(ticks "$server_pid")
    capture_interface_counters "$client_ns" "$client_veth" "$dir/client-interface-before.txt"
    capture_interface_counters "$server_ns" "$server_veth" "$dir/server-interface-before.txt"
    local -a generator_args=("$client_ip" 41002 "$case_rate" "$seconds" "$case_payload" "$sender_cpu" "$receiver_cpu")
    [[ $endpoint == sink ]] && generator_args+=(--one-way)
    ip netns exec "$client_ns" "$out/bin/udp_pps_generator" "${generator_args[@]}" >"$dir/generator.txt" 2>&1 &
    local generator_pid=$!
    sleep 2
    ip netns exec "$client_ns" ss -u -a -n -m >"$dir/client-sockets-at-load.txt"
    ip netns exec "$server_ns" ss -u -a -n -m >"$dir/server-sockets-at-load.txt"
    ip netns exec "$client_ns" tc -s qdisc show dev "$client_veth" >"$dir/client-qdisc-at-load.txt"
    ip netns exec "$server_ns" tc -s qdisc show dev "$server_veth" >"$dir/server-qdisc-at-load.txt"
    ps -L -o pid,lwp,psr,ni,stat,pcpu,comm -p "$client_pid","$server_pid","$echo_pid" >"$dir/process-at-load.txt"
    wait "$generator_pid"
    if [[ $endpoint == sink ]]; then
        # The generator stops at its scheduled send boundary. Let the two
        # speeder processes drain their already accepted packets before the
        # sequence-counting endpoint emits its result.
        sleep 1
        kill -TERM "$echo_pid" 2>/dev/null || true
        wait "$echo_pid" 2>/dev/null || true
        echo_pid=
    fi
    for perf_pid in "$client_perf_record_pid" "$server_perf_record_pid" "$client_perf_stat_pid" "$server_perf_stat_pid"; do
        [[ -n "$perf_pid" ]] && wait "$perf_pid" || true
    done
    if [[ $use_perf -eq 1 ]]; then
        for role in client server; do
            if [[ -s "$dir/$role.perf.data" ]]; then
                perf script -i "$dir/$role.perf.data" >"$dir/$role.perf.script" 2>"$dir/$role-perf-script.log" || true
                perf report --stdio -i "$dir/$role.perf.data" --percent-limit 0 >"$dir/$role.perf-report.txt" 2>&1 || true
                # Keep raw stacks unconditionally. Render an SVG when the
                # standard FlameGraph tools are available, but do not make
                # qualification depend on a presentation-only dependency.
                if command -v stackcollapse-perf.pl >/dev/null && command -v flamegraph.pl >/dev/null; then
                    stackcollapse-perf.pl "$dir/$role.perf.script" >"$dir/$role.perf.folded" 2>"$dir/$role-flamegraph.log" || true
                    flamegraph.pl --title "UDPspeeder $role" "$dir/$role.perf.folded" >"$dir/$role.flamegraph.svg" 2>>"$dir/$role-flamegraph.log" || true
                else
                    printf 'Raw perf script retained; install FlameGraph stackcollapse-perf.pl and flamegraph.pl to render SVG.\n' >"$dir/$role-flamegraph.log"
                fi
            fi
        done
    fi
    client_after=$(ticks "$client_pid")
    server_after=$(ticks "$server_pid")
    local clock_ticks client_cpu_ticks server_cpu_ticks
    clock_ticks=$(getconf CLK_TCK)
    client_cpu_ticks=$((client_after-client_before))
    server_cpu_ticks=$((server_after-server_before))
    printf 'client_cpu_ticks=%s client_cpu_seconds=%.3f server_cpu_ticks=%s server_cpu_seconds=%.3f\n' \
        "$client_cpu_ticks" "$(awk -v ticks="$client_cpu_ticks" -v hz="$clock_ticks" 'BEGIN { printf "%.3f", ticks / hz }')" \
        "$server_cpu_ticks" "$(awk -v ticks="$server_cpu_ticks" -v hz="$clock_ticks" 'BEGIN { printf "%.3f", ticks / hz }')" >"$dir/cpu.txt"
    capture_interface_counters "$client_ns" "$client_veth" "$dir/client-interface-final.txt"
    capture_interface_counters "$server_ns" "$server_veth" "$dir/server-interface-final.txt"
    local client_tx_before client_tx_after client_tx_bytes_before client_tx_bytes_after wire_packets wire_bytes
    client_tx_before=$(counter_value "$dir/client-interface-before.txt" tx_packets)
    client_tx_after=$(counter_value "$dir/client-interface-final.txt" tx_packets)
    client_tx_bytes_before=$(counter_value "$dir/client-interface-before.txt" tx_bytes)
    client_tx_bytes_after=$(counter_value "$dir/client-interface-final.txt" tx_bytes)
    wire_packets=$((client_tx_after-client_tx_before))
    wire_bytes=$((client_tx_bytes_after-client_tx_bytes_before))
    printf 'client_wire_packets=%s client_wire_pps=%.3f client_wire_bytes=%s client_wire_mbit=%.3f\n' \
        "$wire_packets" "$(awk -v packets="$wire_packets" -v duration="$seconds" 'BEGIN { printf "%.3f", packets / duration }')" \
        "$wire_bytes" "$(awk -v bytes="$wire_bytes" -v duration="$seconds" 'BEGIN { printf "%.3f", bytes * 8 / duration / 1000000 }')" >"$dir/wire.txt"
    ip netns exec "$client_ns" tc -s qdisc show dev "$client_veth" >"$dir/client-qdisc-final.txt"
    ip netns exec "$server_ns" tc -s qdisc show dev "$server_veth" >"$dir/server-qdisc-final.txt"
    rg '\[report\]\[io\]' "$dir/client.log" >"$dir/client-batch-counters.txt" || true
    rg '\[report\]\[io\]' "$dir/server.log" >"$dir/server-batch-counters.txt" || true
    rg '\[report\]\[adaptive-fec\]' "$dir/client.log" >"$dir/client-adaptive-fec-counters.txt" || true
    rg '\[report\]\[adaptive-fec\]' "$dir/server.log" >"$dir/server-adaptive-fec-counters.txt" || true
    printf '%s endpoint=%s ' "$case_name/run-$case_iteration" "$endpoint" | tee -a "$out/summary.txt"
    tr '\n' ' ' <"$dir/generator.txt" | tee -a "$out/summary.txt"
    if [[ $endpoint == sink ]]; then tr '\n' ' ' <"$dir/sink.log" | tee -a "$out/summary.txt"; fi
    tr '\n' ' ' <"$dir/cpu.txt" | tee -a "$out/summary.txt"
    tr '\n' ' ' <"$dir/wire.txt" | tee -a "$out/summary.txt"
    printf '\n' | tee -a "$out/summary.txt"
    cleanup_processes
    sleep 1
}

if [[ $matrix -eq 1 && $knee -eq 1 ]]; then
    echo "--matrix and --knee are mutually exclusive" >&2
    exit 2
elif [[ $matrix -eq 1 ]]; then
    for case_profile in nofec fec11 fec14 adaptive; do
        for case_payload in 256 512 1200; do
            for case_rate in 10000 25000 50000; do
                for case_iteration in $(seq 1 "$repeat"); do
                    run_case "${case_profile}-${case_rate}pps-${case_payload}b-b${receive_batch}" "$case_profile" "$case_rate" "$case_payload" "$case_iteration"
                done
            done
        done
    done
elif [[ $knee -eq 1 ]]; then
    for case_payload in 256 512 1200; do
        for case_rate in 10000 25000 50000 75000 100000; do
            for case_iteration in $(seq 1 "$repeat"); do
                run_case "${profile}-${case_rate}pps-${case_payload}b-b${receive_batch}" "$profile" "$case_rate" "$case_payload" "$case_iteration"
            done
        done
    done
else
    for case_iteration in $(seq 1 "$repeat"); do
        run_case "${profile}-${rate}pps-${payload}b-b${receive_batch}" "$profile" "$rate" "$payload" "$case_iteration"
    done
fi

printf 'artifacts=%s\n' "$out"
