# Adaptive FEC and direct-bypass qualification

Date: 2026-08-28 (UTC)  
Fork base: `b6a1b5941d9ebb6ef4f6866abdf1e03a47b99b2d`

## Scope

The implementation is intentionally opt-in and has not been used against the
Amsterdam relay. Control, feedback, and direct-bypass frames carry a SipHash-2-4
MAC. When `-k/--key` is omitted, that MAC uses the built-in
`UDPspeeder-adaptive-fec-v1` compatibility key; it preserves legacy no-key
operation but is public and is not an authentication secret. Production peers
must instead use the same private `-k/--key` value of at least 16 bytes. A peer
that does not answer the bounded capability probes remains on the existing
static FEC profile.

The state machine is receiver-driven:

```
normal (zero redundancy/direct bypass)
  -> guard (moderate receiver loss, recovery, or reordering)
  -> degraded (confirmed high loss or recovery)
  -> recover (three clean windows)
  -> normal (clean hold elapsed)
```

Default guard redundancy is one third of the configured `--fec` profile,
rounded up. Degraded uses the configured `--fec` profile.

The lab defaults enter Guard at 1% unrecoverable packets, 2% recovered packets,
or 5% reordering; Degraded starts at 3% unrecoverable or 12% recovered. The
first loss while Normal is expected to be unrecoverable because redundancy has
not yet been sent.

## Functional checks

* `make -j2` and `./speederv2 --unit-test` pass. The unit test covers default
  control-key operation, MAC rejection, legacy-shaped capability exchange,
  bypass payload integrity, receiver-observed sequence loss, a state
  transition, and parity-only k=1 recovery in both FEC modes.
* The CMake source list now includes the pre-existing missing
  `crc32/Crc32.cpp` unit. A Debug ASAN/UBSAN build ran the assertions-enabled
  unit suite and a 100-payload default-key client/server echo without sanitizer
  findings.
* Two upgraded local endpoints without `-k` delivered 120/120 payloads and
  logged capability confirmation plus direct bypass on each endpoint. The same
  test with a private explicit key also delivered 120/120 payloads.
* An upgraded no-key client with an unchanged no-key stock server delivered
  120/120 payloads. The stock decoder accepted the three bounded probes as
  valid zero-payload legacy FEC groups; the client never logged capability
  confirmation or bypass and stayed on static FEC as designed.
* A local UDP proxy dropped every fifth client-to-server datagram for a
  two-second interval, then restored delivery. Both keyed endpoints transitioned
  through impairment and recovery; the client logged
  `normal -> degraded -> recover -> normal` and resumed direct bypass. The
  recovery phase delivered 640 echo responses.

## Normal-load small-packet result

The keyed 512-byte UDP echo generator requested 8,000 payload packets/s for
six seconds (about 32.77 Mbit/s of useful payload). The static control used the
current `1:4,2:5,10:14,20:20,100:82` FEC profile on the stock binary. The
candidate used the same profile until negotiation, then the authenticated
direct-bypass normal state.

| Metric | Static FEC | Adaptive direct bypass |
| --- | ---: | ---: |
| Delivered payloads | 48,000 / 48,000 | 47,999 / 47,999 |
| Useful payload rate | 32.768 Mbit/s | 32.767 Mbit/s |
| Raw p50 | 17.38 ms | 0.48 ms |
| Raw p95 | 28.36 ms | 0.80 ms |
| Raw p99 | 32.71 ms | 1.01 ms |
| Client speeder CPU ticks | 292 | 270 |
| Server speeder CPU ticks | 364 | 318 |

This demonstrates a worthwhile normal-load benefit: no loss of useful rate or
probe-equivalent echo success, far lower queueing delay, and lower CPU. It is
not a blanket high-PPS promotion result: an earlier 20,000 packet/s loopback
exercise showed scheduler-sensitive tail variation. Repeat 10/50/100 Mbit/s
qualification with the keyed implementation, controlled CPU affinity, and
netem before a production rollout.

## k=1 replication fast path

For a single-data-shard FEC group, every parity shard is an exact replica of
the source shard. The encoder now copies the source into its existing fixed
output buffers rather than entering the generic RS matrix path; the decoder
points to the retained incoming shard rather than invoking the generic RS
decoder. The normal adaptive direct-bypass path already uses fixed buffers and
does not enter either FEC manager. No per-packet heap allocation was added.

An 8,000 packet/s, 512-byte, six-second loopback comparison used static
`-f1:4 --mode 1` (48,000 payloads; 32.768 Mbit/s useful payload). This isolates
the replication implementation from adaptive-state transitions, but is still a
socket/softirq-dominated rather than a pure codec benchmark.

| Metric | Stock generic RS | Candidate replication fast path |
| --- | ---: | ---: |
| Delivered payloads | 48,000 / 48,000 | 48,000 / 48,000 |
| Raw p50 | 0.401 ms | 0.403 ms |
| Raw p95 | 11.976 ms | 9.983 ms |
| Raw p99 | 17.375 ms | 16.008 ms |
| Client speeder CPU ticks | 482 | 486 |
| Server speeder CPU ticks | 557 | 561 |

The one controlled loopback run found no measurable system-level CPU reduction
(the 0.7–0.8% increase is normal scheduling noise), and its tail-latency delta
is not enough to claim a p99 win. It did confirm identical delivery and valid
interoperation with the stock decoder. Keep the fast path for its simpler
correct k=1 handling and removal of generic RS work, but do not promote it as
a demonstrated end-to-end PPS gain until repeated affinity-pinned 50/100
Mbit/s tests show a repeatable CPU or tail-latency improvement.
