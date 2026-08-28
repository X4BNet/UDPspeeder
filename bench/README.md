# UDPspeeder PPS qualification

`run-pps-qualification.sh` creates disposable client and server namespaces
joined by a veth, then runs a batched UDP echo endpoint, UDPspeeder client and
server, and an independently paced sender/receiver on separate CPUs. It never changes host
qdiscs or host network configuration.

Example 50k-PPS direct-path candidate:

```bash
sudo bench/run-pps-qualification.sh --binary /path/to/speederv2 \
  --profile nofec --rate 50000 --payload 512 --seconds 10 --repeat 3 \
  --recvmmsg-batch 32 --cpus 40,41,42,43,44
```

Use `--netem 'delay 20ms 5ms loss 1%'` for the impaired stage. `--matrix`
runs the 10k/25k/50k PPS × 256/512/1200-byte × three-repeat set for all
profiles with the selected receive/send batching options. Re-run that matrix
with batches `1`, `32`, and `64`, and with `--sendmmsg` where a profile emits
FEC bursts.

Artifacts include the binary hash, actual payload bytes/PPS, veth wire
bytes/PPS (including FEC overhead), latency percentiles, process CPU ticks and
seconds, socket drops, interface and qdisc counters, logs, and I/O batch
counters. The harness applies netem only to its disposable namespace veths.
Use a five-core set that is otherwise idle on shared runners; its exact CPU
mapping is written to `environment.txt`.
