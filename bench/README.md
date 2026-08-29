# UDPspeeder PPS qualification

`run-pps-qualification.sh` creates disposable client and server namespaces
joined by a veth, then runs a batched UDP echo endpoint, UDPspeeder client and
server, and an independently paced sender/receiver on separate CPUs. It never changes host
qdiscs or host network configuration.

Example 50k-PPS direct-path candidate:

```bash
sudo bench/run-pps-qualification.sh --binary /path/to/speederv2 \
  --profile nofec --rate 50000 --payload 512 --seconds 10 --repeat 5 \
  --recvmmsg-batch 32 --cpus 40,41,42,43,44
```

Add `--perf` for a separate attribution run. It writes client/server
`perf.data`, text reports, stack scripts, and software scheduling counters.
The profiler changes the workload, so use an unprofiled control for promotion
measurements and the profiled run only to identify hot paths.
Adaptive profiling additionally enables `--adaptive-fec-stats`; normal
benchmark rounds leave those diagnostic per-packet counters disabled.

Use `--netem 'delay 20ms 5ms loss 1%'` for the impaired stage. `--matrix`
runs the 10k/25k/50k PPS × 256/512/1200-byte × `--repeat` set (five by
default) for all profiles with the selected receive/send batching options.
`--knee` runs 10k/25k/50k/75k/100k PPS × 256/512/1200-byte × `--repeat` for
the selected profile, stopping a manual campaign once delivery or tail latency
degrades. Re-run the matrix
with batches `1`, `32`, and `64`, and with `--sendmmsg` where a profile emits
FEC bursts.

Unless the netem arguments include `limit`, the harness adds `limit 32768`.
This prevents netem's 1,000-packet default queue from silently adding severe
queue loss when a loss-triggered FEC profile raises the wire packet rate.

Artifacts include the binary hash, actual payload bytes/PPS, veth wire
bytes/PPS (including FEC overhead), latency percentiles, process CPU ticks and
seconds, socket drops, interface and qdisc counters, logs, and I/O batch
counters. The harness applies netem only to its disposable namespace veths.
Use a five-core set that is otherwise idle on shared runners; its exact CPU
mapping is written to `environment.txt`.

With `--perf`, artifacts always include `perf.data`, a text report, and raw
stacks. If `stackcollapse-perf.pl` and `flamegraph.pl` are on `PATH`, they also
include `<role>.flamegraph.svg`; otherwise the corresponding log records the
optional renderer requirement.

Use `--client-binary` and `--server-binary` to verify a wire-compatible change
against an older peer. Each artifact records both binary hashes.
