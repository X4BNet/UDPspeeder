# 50k-PPS receive-batch qualification

Date: 2026-08-28

Scope: UDPspeeder only. No OpenVPN, DCO, relay, Amsterdam, or io_uring changes
were made.

## Result

`--recvmmsg-batch` is implemented, tested, and remains opt-in with its legacy
default of `1`. Native `-O3` release controls still fail the admission gate at
50k actual 512-byte payload PPS: batch 32 delivered 97.36% with 288 ms p99;
batch 64 delivered 98.91% with 114 ms p99. Neither meets 99.5% delivery, zero
UDP receive-buffer drops, and p99 <= 10 ms, so neither becomes the default.

The first part of this report was generated with a CMake binary that carries
AddressSanitizer and UBSan even in its Debug configuration. Those data remain
as sanitizer diagnostics, but they are not release-performance evidence. The
native `-O3` results below are authoritative for performance decisions.

## Implementation

- Added Linux `--recvmmsg-batch <1..64>`, default `1`.
- Preallocated message headers, I/O vectors, source addresses, and packet
  storage for the client local/remote and server local/remote receive paths.
  Datagrams retain arrival order; each libev callback processes at most 128.
- Preserved the `recv`/`recvfrom` path for batch `1`, non-Linux builds, and
  `ENOSYS`. An `ENOSYS` result logs once and switches the process to batch `1`.
- Added `--report` I/O counters for receive calls/packets/`EAGAIN`/fallback and
  sendmmsg calls/packets/partial sends/`EAGAIN`/fallback packets.
- Added a namespace/veth benchmark with paced generator, batched echo endpoint,
  CPU affinity, optional in-namespace netem, actual payload and wire PPS,
  latency percentiles, process CPU, socket/interface/qdisc counters, logs, and
  batch statistics.
- Added unit coverage for batched receive order and source addresses, the
  legacy receive path, and a forced `ENOSYS` fallback. Existing unit coverage
  exercises adaptive direct-bypass and k=1 replicated FEC recovery.
- The benchmark helper uses non-restarting `SIGTERM` handling, so every
  disposable namespace is cleaned up even when the echo endpoint is idle.

No io_uring backend was added. Batching receive draining can be qualified
without changing the libev architecture; this result does not justify a second
I/O backend yet.

## Environment

| Item | Value |
|---|---|
| Source revision | `64091e971e335f436927e1b834e79ee63e83e747` |
| Kernel | `6.12.90+deb13.1-amd64` |
| `net.core.rmem_max` / `wmem_max` | `21,299,200` bytes |
| Initial 50k binary SHA-256 | `db8a76cc2ff01c3e8518e6953107a4f34735f6d60e435784d543fa62b8b1b6df` |
| Final sanitizer binary SHA-256 | `3de5493fbfaa747513a50b06810994ec67f3547ed900fc9695a71d3097fd330e` |
| Native FEC baseline SHA-256 | `849e42695c15db19f255dc5e5bb3e5226e27ef78121df5f6a165d161ffa4e060` |
| Native k=1 direct candidate SHA-256 | `d5fe8fe9b2a3e6aa6cf0785a8269d36a449e63c5683f2b6da5646fddbefa20e5` |

## Historical sanitizer diagnostics

The following early CMake/ASan measurements are retained only to diagnose
memory safety and receive behavior. They must not be used for throughput,
latency, FEC, or promotion claims; sanitizer instrumentation materially alters
this packet-rate workload.

| Receive batch | Delivery | p99 RTT | Client/server CPU | Decision |
|---:|---:|---:|---:|---|
| 1 | 36.7% | 1446 ms | 9.97 / 10.03 s | Baseline fails |
| 32 | 39.6% | 1714 ms | 7.10 / 6.94 s | Fails; no default |
| 64 | 63.6% | 502 ms | 10.02 / 9.44 s | Better, but fails gate |

The 50k packet-size endpoint runs with batch 64 also failed: 256-byte packets
delivered 60.1% with 1462 ms p99, and 1200-byte packets delivered 41.2% with
827 ms p99. This is not a qualified small-packet setting.

## Historical sanitizer diagnostics: 10k controls and loss

Three independent final-binary no-FEC, 512-byte, batch-32 controls at 10k
actual PPS delivered 99.973%, 99.975%, and 99.973%. Their p99 values were
4.709, 5.131, and 15.001 ms. Wire rate was 10,000.4--10,000.6 PPS and
46.48 Mbit/s, versus 40.96 Mbit/s of payload.

Single 10-second impaired stages used batch 32 and 512-byte 10k PPS payload:

| Stage/profile | Delivery | p99 RTT | Wire PPS | Observation |
|---|---:|---:|---:|---|
| 1% loss + 20+/-5 ms, no FEC | 88.5% | 897 ms | 9.3k | Return-path loss and queueing dominate |
| 1% loss + 20+/-5 ms, `1:1` | 57.9% | 2563 ms | 19.6k | Redundancy doubled PPS and worsened delivery |
| 1% loss + 20+/-5 ms, `1:4` | 14.4% | 4641 ms | 34.2k | PPS collapse |
| 1% loss + 20+/-5 ms, adaptive | 12.7% | 5606 ms | 29.7k | Escalated rapidly to degraded profile |
| 5% loss + 75+/-25 ms + 0.25% reorder, no FEC | 88.0% | 200 ms | 9.4k | Expected bidirectional loss floor |
| 5% loss + 75+/-25 ms + 0.25% reorder, `1:1` | 66.1% | 211 ms | 13.0k | Extra PPS still costs delivery |

These observations remain useful for exercising the netem topology, but are
not native-performance or FEC-promotion evidence.

## Native release FEC direct-delivery experiment

The k=1 mode-1 encoder now marks its systematic packet with its valid `1:N`
FEC shape. A new decoder can then immediately deliver either that packet or a
parity-only recovery packet without allocating a FEC group, copying the shard,
or invoking generic decode bookkeeping. It marks the sequence complete and
suppresses replicas. Old senders still use their `data_num=0` systematic frame;
the new decoder detects the existing generic group before a parity shard and
falls back without duplicate delivery. Old decoders accept the valid full k=1
header from a new sender.

The authoritative release comparison used 512-byte payloads, batch 32,
`--sendmmsg`, CPUs 32--36, and static `1:4` (50k wire PPS at the 10k stage):

| Stage | Delivery | Client CPU | Server CPU | p99 RTT |
|---|---:|---:|---:|---:|
| 10k PPS baseline, 20 s | 99.986% | 14.76 s | 15.05 s | 11.915 ms |
| 10k PPS direct k=1, 20 s | 99.986% | 14.16 s | 13.75 s | 10.269 ms |
| 10k PPS + 1% loss baseline, 10 s | 52.872% | 8.75 s | 8.76 s | 1309 ms |
| 10k PPS + 1% loss direct k=1, 10 s | 59.559% | 9.91 s | 9.92 s | 921 ms |

The no-loss 20-second result is a 4.1% client CPU reduction, 8.6% server CPU
reduction, and 13.8% p99 improvement with identical delivery. The loss result
delivers more traffic with lower p99, so its increased CPU reflects useful
work rather than a regression. Mixed new-client/old-server and
old-client/new-server runs both delivered 99.968% at 5k PPS, confirming the
wire-compatible fallback.

This direct k=1 change is retained. It meets the requested repeatable positive
improvement threshold without increased drops or tail latency.

## sendmmsg and prior FEC allocation decisions

`--sendmmsg` remains opt-in. The historical sanitizer A/B must not decide its
promotion. The native k=1 comparison used it solely to prevent send syscall
count from masking decoder work; it does not make sendmmsg a default.

The fixed shard-index-table and group-pool experiments were removed. They did
not produce a repeatable gain. The retained direct k=1 delivery path replaces
generic decoder bookkeeping only when its buffer lifetime is synchronous and
validated.

## Raw artifacts

All artifacts are retained locally under `/tmp` and contain `environment.txt`,
`summary.txt`, generator distributions, process CPU, socket/interface/qdisc
snapshots, logs, and batch counters:

- `/tmp/udpspeeder-qual-final-b1-clean`
- `/tmp/udpspeeder-qual-final-b32`
- `/tmp/udpspeeder-qual-final-b64`
- `/tmp/udpspeeder-qual-50k-b32-cpu32`
- `/tmp/udpspeeder-qual-50k-b64-cpu32`
- `/tmp/udpspeeder-qual-final-10k-b32`, `/tmp/udpspeeder-qual-final-10k-b32-r2`, and `/tmp/udpspeeder-qual-final-10k-b32-r3-clean`
- `/tmp/udpspeeder-qual-loss1-nofec`, `/tmp/udpspeeder-qual-loss1-fec11`, `/tmp/udpspeeder-qual-loss1-fec14`, and `/tmp/udpspeeder-qual-loss1-adaptive`
- `/tmp/udpspeeder-qual-loss5-nofec` and `/tmp/udpspeeder-qual-loss5-fec11`
- `/tmp/udpspeeder-qual-fec14-send-off` and `/tmp/udpspeeder-qual-fec14-send-on`
- `/tmp/udpspeeder-release-nofec-50k-b32-r1` and `/tmp/udpspeeder-release-nofec-50k-b64-r1`
- `/tmp/udpspeeder-k1-direct-baseline-o3-r1` through `-r4` and `/tmp/udpspeeder-k1-direct-candidate-final-r1`
- `/tmp/udpspeeder-k1-direct-baseline-o3-8k-r1`, `-r2`, and their candidate counterparts
- `/tmp/udpspeeder-k1-direct-baseline-o3-10k-r1`, `-r2`, `-r3-20s`, and their candidate counterparts
- `/tmp/udpspeeder-k1-direct-baseline-o3-10k-loss1`, `/tmp/udpspeeder-k1-direct-candidate-final-10k-loss1`, and `/tmp/udpspeeder-k1-direct-candidate-final-5k-loss5`
- `/tmp/udpspeeder-k1-direct-mixed-new-client` and `/tmp/udpspeeder-k1-direct-mixed-new-server`

## Next qualification

Run the full matrix on a runner with five exclusive cores using
`bench/run-pps-qualification.sh --matrix --cpus E,S,C,TX,RX`, separately for
batches 1, 32, and 64. Repeat the selected no-loss candidates under the two
netem stages. Promote only if the stated 50k gate is met for every required
packet size and loss stage. If receive drops are eliminated but a core remains
saturated, then an opt-in, compile-time-isolated io_uring prototype can be
reconsidered; do not add SQPOLL or UDP zero-copy.
