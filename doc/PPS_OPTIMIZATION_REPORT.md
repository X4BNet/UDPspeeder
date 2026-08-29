# UDPspeeder PPS optimization qualification

Date: 2026-08-29

## Environment and method

- Linux `6.12.90+deb13.1-amd64`; UDP maxima: 21,299,200 bytes.
- `linux-perf` 6.12.105 and liburing 2.9 were installed only in the test
  environment. Hardware PMU events are blocked by the container, so `perf`
  attribution uses call stacks and software task-clock/scheduling counters.
- The namespace/veth harness pins echo, server, client, generator TX, and
  generator RX to CPUs 32--36. Every saved round records binary hashes, real
  payload and wire PPS, RTT distribution, process CPU, socket memory/drops,
  qdisc/interface counters, process state, logs, and batch counters.
- Promotion measurements use the unprofiled harness. `--perf` is attribution
  only, because profiling changes the workload.

## Knee and attribution

One-round 256-byte no-FEC scouting found the useful-PPS knee between 50k and
75k PPS:

| Offered PPS | Delivery | p99 RTT | Client/server CPU |
| ---: | ---: | ---: | ---: |
| 10k | 99.974% | 5.183 ms | 5.37 / 5.50 s |
| 25k | 99.991% | 14.951 ms | 7.10 / 6.78 s |
| 50k | 99.912% | 65.269 ms | 9.53 / 8.94 s |
| 75k | 73.424% | 720.076 ms | 9.98 / 9.96 s |

At 25k adaptive-direct PPS, a profiled round delivered 99.986% with 5.354 ms
p99. Client/server task-clock was 6.873/6.226 seconds. `sendmmsg` plus
kernel UDP/IP transmit accounted for roughly 65--68% of samples; `recvmmsg`
was roughly 11--13%. Adaptive MAC validation was about 0.3%, and immediate
output staging about 0.2%. The next bottleneck is kernel transmit/softirq,
not RS arithmetic or the adaptive MAC.

The adaptive counters show the intended steady state: about 30k bypass frames
and 12 authenticated control frames per second per side in this bidirectional
echo workload, with no state changes in a no-loss run.

## Changes evaluated

| Candidate | Decision | Evidence |
| --- | --- | --- |
| Cache adaptive MAC key words | Rejected | Five 50k adaptive rounds showed no CPU or delivery benefit and a worse median p99 (54.823 ms vs. 41.562 ms in the paired baseline set). |
| Callback-owned in-place immediate send | Kept | Removes the second temporary copy before cook/send for zero-delay output; five 25k no-FEC rounds had median p99 2.082 ms vs. 2.209 ms and client CPU 6.20 s vs. 6.30 s, with no material delivery difference (99.992% vs. 99.994%). |
| Bounded bypass reordering window | Kept | A 256-packet sequence window stops delayed frames being charged as permanent loss. Unit coverage verifies reordered frames remain normal while a gap beyond the window selects Guard. |
| k=1 parity-first recovery metric | Kept | Replication still delivers immediately, but a parity-first frame is no longer labelled a confirmed recovery: it may be a delayed systematic frame. This prevents false 60--80% recovery feedback under ordinary delay variation. |
| Multi-observation degraded gate | Kept | Degraded now needs at least three recovery/loss observations plus the existing percentage threshold, avoiding a one-packet early-window jump. |
| io_uring receive backend | Rejected | See the io_uring section. The production tree remains libev/recvmmsg. |

The adaptive counters are deliberately opt-in (`--adaptive-fec-stats`) so
normal tunnels do not write per-packet diagnostic statistics.

## Loss/reordering qualification

The initial 1% loss test exposed a harness issue: netem's default 1,000 packet
limit added queue loss after FEC increased wire PPS. The harness now adds
`limit 32768` unless a stage explicitly supplies `limit`; qdisc artifacts make
the distinction visible.

The corrected adaptive controller was tested with the final candidate:

| Stage | Payload rate | Delivery | p99 RTT | Client wire PPS | State result |
| --- | ---: | ---: | ---: | ---: | --- |
| 1% loss, 20+/-5 ms | 10k PPS | 99.619% | 61.805 ms | 29.629k | Normal -> Guard -> Recover |
| 5% loss, 75+/-25 ms, 0.25% reorder | 5k PPS | 98.596% | 170.300 ms | 14.510k | Normal -> Guard -> Degraded -> Recover |

The 1% stage before the feedback correction sent about 49.1k wire PPS and
had a 228.2 ms p99. Afterwards it sent about 29.6k wire PPS with a 61.8 ms
p99. The qdisc drop counts in the final stages match the configured random
loss; socket queues did not overflow and netem did not report overlimits.

## io_uring attempt

An isolated compile-time experiment used `IORING_OP_RECVMSG` from the existing
libev callbacks while retaining `sendmmsg` output. It was deliberately not
merged. Its per-callback submit/reap model delivered only 77.618% at 10k
256-byte PPS, despite zero socket-buffer drops. It cannot compete with one
`recvmmsg` drain per readiness event.

`IORING_SETUP_SQPOLL` was also attempted and returned `EINVAL` in this
environment, so it fell back to recvmmsg rather than exercising SQPOLL.

The kernel mechanism itself was independently verified with liburing's
provided-buffer, multishot `RECVMSG` UDP echo sample:

| Backend | Offered PPS | Delivery | p99 RTT |
| --- | ---: | ---: | ---: |
| Multishot registered-buffer echo | 25k | 99.999% | 0.297 ms |
| Multishot registered-buffer echo | 50k | 98.498% | 7.808 ms |
| Ordinary UDP echo | 50k | 99.985% | 8.904 ms |

The multishot sample has different ownership and send semantics from
UDPspeeder and loses materially more packets at 50k. It is therefore not
promotion evidence. A useful future io_uring effort would need an eventfd/CQ
driven architecture and zero-copy handoff of provided buffers; it is not a
drop-in replacement for the libev receive callback.

## Artifacts

Artifacts are intentionally outside Git because perf data is large. Key local
directories for this qualification are:

- `/tmp/udpspeeder-knee-nofec-256-10000`, `...-25000`, `...-50000`,
  `...-75000`
- `/tmp/udpspeeder-direct-inplace-profile-adaptive-25k`
- `/tmp/udpspeeder-direct-inplace-baseline-nofec-25k` and
  `/tmp/udpspeeder-direct-inplace-candidate-nofec-25k`
- `/tmp/udpspeeder-adaptive-feedback-loss1-10k`
- `/tmp/udpspeeder-adaptive-feedback-loss5-5k`
- `/tmp/udpspeeder-uring-rx-smoke` and `/tmp/udpspeeder-uring-sqpoll-smoke`
- `/tmp/udpspeeder-uring-multishot-50k-generator.txt` and
  `/tmp/udpspeeder-recvmmsg-echo-50k-generator.txt`

No Amsterdam, relay, OpenVPN, DCO, or production configuration was changed.
