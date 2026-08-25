# Packet-local timing and Manchester recovery

The receiver must separate two different faults:

- a comparator edge that moved in time because of threshold, slew rate, duty
  distortion, or propagation delay;
- a transition that never crossed the threshold and therefore does not exist.

The first case is recoverable by clock tracking. The second is information
loss and can only be repaired when framing, Manchester constraints, and final
RS/CRC validation make the missing value unambiguous.

## Active timing model

For every burst, the alternating preamble supplies robust, polarity-specific
cell estimates `t0` and `t1`; `T` is their average. The decoder does not assume
that the configured nominal duration is exact and does not reuse an unverified
packet's timing as truth.

Each inter-edge interval starts from the polarity-specific local estimate:

```text
expected(n, polarity) = t[polarity] + (n - 1) * T
```

The run decision is shifted conservatively by 1/16 of the packet-local cell.
This changes a 32-tick profile by only 2 ticks, but prevents intervals on the
one/two-cell boundary from being rounded upward into a false inserted cell.
The correction scales with `T` for all profiles. That remains the primary
decision and therefore preserves the behavior of already-good packets.

Real 828-byte failures exposed the opposite, genuinely ambiguous case. One
wrong cell decision changes Manchester pairing until another timing error
changes it back. During that window roughly half the pairs are invalid even
though the comparator transitions still retain the data. The recovery pass
detects this sustained error density, searches locally for the one-cell phase
boundary, then uses the decoded physical length to choose an insertion or
deletion at each boundary. It does not depend on a particular 58- or 80-tick
value.

This pass is enabled only after an exact preamble/SFD lock, runs only when the
primary path fails, and can win only through complete RS/CRC validation.
Isolated bad pairs remain untouched for FEC: recovery requires at least four
bad pairs in a 16-pair window, at least two consecutive bad pairs, and a clear
improvement after the edit.

The active production path quantises each run independently. The experimental
bounded grid was removed because live traffic showed that even short timing
memory could turn local crossing motion into repeatable packet loss.

The old integer IIR and edge-by-edge DPLL phase carry were also removed. They
mixed propagation jitter or a missing crossing into following decisions. In
the supplied real captures DPLL carry changed 3/4 decodable packets into 0/4,
while independent quantisation followed by bounded phase recovery preserved
the decodable packets.

## Synchronization and repair

The decoder:

1. requires the bounded alternating preamble gate;
2. correlates SFD phase and polarity with a limited Hamming distance;
3. tries the primary timing decision, then the CRC-guarded phase recovery;
4. detects sustained Manchester phase slips without packet-index or
   timer-value special cases;
5. rejects long runs of invalid Manchester pairs;
6. rejects a decoded length that cannot fit in the remaining burst;
7. delivers only a frame passing RS/CRC validation;
8. always retries the direct low-cost path first on the next packet.

Equal-pair repair is deliberately limited. Increasing its limits can turn
payload patterns into false locks; it does not make a physically absent edge
more observable.

## Diagnostics

The `COMP` line reports `t0`, `t1`, `tn` and `trq`. `trq` is the peak absolute
run residual in Q8 timer ticks. Convert it with `trq / 256`. Large values show
that at least one selected run was far from its ideal duration; they do not
alter following run decisions. Compare it with `bad`, `badrun`,
`crc`/`rs`, `bf` and `bok`; a single field is not enough to diagnose the link.
`hb` reports the current hypothesis budget (`0` means the configured maximum).
In `mode`, bit 4 (`16`) identifies the phase-capable fallback (`mode=27` when
combined with the measured boundary decisions). `pe` is the number of actual
one-cell edits used for the last successful packet; `pe=0` means that the
boundary decision alone was sufficient. The normal fast path remains
`mode=3`. Set `OPENVLC_RX_PHASE_RECOVERY=0` only for an explicit A/B test.

For hardware correlation, capture PB0 (COMP1 IN+), optional PC5 COMP1_OUT
(`OPENVLC_COMP_DEBUG_OUTPUT=1`), and PE9 TX. Return the debug output to zero for
production tests because its switching can couple into the analog path.
