# 50k-PPS receive-batch qualification

Date: 2026-08-28

Scope: UDPspeeder only. No OpenVPN, DCO, relay, Amsterdam, or io_uring changes
were made.

## Result

`--recvmmsg-batch` is implemented, tested, and remains opt-in with its legacy
default of `1`. Neither batch 32 nor batch 64 meets the admission gate at 50k
actual 512-byte payload PPS: 99.5% delivery, zero UDP receive-buffer drops,
and p99 <= 10 ms. Do not make either batch size the default.

The evidence does show that receive draining is material. In the initial
three-round CPU-affinitized run, batch 64 improved the median delivery from
36.7% to 63.6% and reduced median p99 from 1446 ms to 502 ms relative to the
legacy path. It still saturated a core and dropped packets. A later run on
otherwise quiescent CPUs independently failed the gate: batch 32 delivered
54.8% with 752 ms p99, and batch 64 delivered 41.8% with 1072 ms p99; the
latter had 402,933 client socket drops at the two-second sample.

The host is shared, so exact tails vary across rounds. The results are still
decisive for promotion because every 50k run has large delivery loss and
socket drops.

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

## 50k no-FEC, 512-byte payload, three runs

The initial three-run comparison used CPUs 0--4. Values are medians of the
three raw artifacts; the binary SHA is the initial 50k SHA above.

| Receive batch | Delivery | p99 RTT | Client/server CPU | Decision |
|---:|---:|---:|---:|---|
| 1 | 36.7% | 1446 ms | 9.97 / 10.03 s | Baseline fails |
| 32 | 39.6% | 1714 ms | 7.10 / 6.94 s | Fails; no default |
| 64 | 63.6% | 502 ms | 10.02 / 9.44 s | Better, but fails gate |

The 50k packet-size endpoint runs with batch 64 also failed: 256-byte packets
delivered 60.1% with 1462 ms p99, and 1200-byte packets delivered 41.2% with
827 ms p99. This is not a qualified small-packet setting.

## 10k clean control and loss results

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

These are recovery observations, not FEC promotions. The static/adaptive
profiles fail the required 95% success and recovery criteria at this rate.

## sendmmsg and FEC allocation decisions

`--sendmmsg` remains opt-in. One static `1:4` run at 10k PPS improved delivery
from 58.6% to 72.5% and p99 from 1811 ms to 805 ms, but subsequent shared-host
runs varied substantially and all still saturated the FEC path near 50k wire
PPS. It therefore has no default-promotion evidence.

The fixed shard-index-table and group-pool experiments were removed. They did
not achieve the required 10% delivered-PPS or CPU-per-delivered-packet gain
without regression. The existing k=1 retained-buffer replication path remains
covered and unchanged.

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

## Next qualification

Run the full matrix on a runner with five exclusive cores using
`bench/run-pps-qualification.sh --matrix --cpus E,S,C,TX,RX`, separately for
batches 1, 32, and 64. Repeat the selected no-loss candidates under the two
netem stages. Promote only if the stated 50k gate is met for every required
packet size and loss stage. If receive drops are eliminated but a core remains
saturated, then an opt-in, compile-time-isolated io_uring prototype can be
reconsidered; do not add SQPOLL or UDP zero-copy.
