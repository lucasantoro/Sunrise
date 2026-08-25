# Wire format

What actually goes over the light, byte by byte and cell by cell. Everything
here is read from the source rather than remembered; the constants live in
`firmware/openvlc/include/openvlc_config.h` and `Core/Inc/openvlc_board.h`.

## Line code

Manchester, 1 Mbit/s. One **bit** is one **cell**; one cell is two half-cells.

```
  bit 1  =  LOW  then HIGH
  bit 0  =  HIGH then LOW
```

Timing at the 1000 kbit/s profile, in TIM2 input-capture ticks at 64 MHz:

| | ticks | time |
|---|---|---|
| half-cell | 32 | 500 ns |
| cell (one bit) | 64 | 1 µs |

A transition always falls at mid-cell, which is what carries the clock. A
transition also falls at the cell boundary whenever two consecutive bits are
equal. So the only two legal intervals between edges are **32** and **64**
ticks, and the decoder's whole job is telling those apart.

That is also why the duty cycle matters so much: see
[the demodulator](#demodulator).

## Frame

Built by `openvlc_frame_build()` in `firmware/openvlc/src/openvlc_frame.c`, in
this order:

```
+----------------+---------+--------+-----+-----+----------+---------+--------+
| preamble       | SFD     | length | dst | src | protocol | payload | CRC-16 |
| 8 x 0xAA       | 99 4B   | 2 B    | 1 B | 1 B | 2 B      | n B     | 2 B    |
+----------------+---------+--------+-----+-----+----------+---------+--------+
                 |<---------------- CRC covers this ---------------->|
```

| field | bytes | value |
|---|---|---|
| preamble | 8 | `0xAA` repeated (`OPENVLC_PREAMBLE_BYTE`) |
| SFD | 2 | `0x99 0x4B` (`OPENVLC_SFD_BYTE`, `OPENVLC_SFD_BYTE2`) |
| length | 2 | payload length, big-endian |
| dst | 1 | destination optical address |
| src | 1 | source optical address |
| protocol | 2 | big-endian, `0x0001` for IP |
| payload | n | up to `OPENVLC_MAX_PAYLOAD_BYTES` |
| CRC | 2 | CRC-16/CCITT over length..payload, big-endian |

Fixed overhead is 8 + 2 + 6 + 2 = **18 bytes**. A 828-byte payload — the size a
1500-byte-MTU-clamped TUN produces at 900 MTU — makes an 846-byte frame, which
is 6768 cells, **6.77 ms** on air.

### Addressing

Each node owns one address; node A is 7 and node B is 8
(`OPENVLC_ADDR_PEER_DEFAULT` / `OPENVLC_ADDR_SELF_DEFAULT`, swapped per node by
`OPENVLC_TRANSCEIVER_NODE` at build time).

**A receiver discards any frame whose `src` is its own address.** That is what
stops a node acting on its own transmission, and it is why the two boards must
never be flashed with the same node firmware.

### Why the preamble is 0xAA

`0xAA` is `10101010`. In Manchester, alternating bits mean the cell-boundary
transition is *never* present — the pattern is one short interval followed by
all long ones. That gives the receiver a run of unambiguous 2-cell intervals to
lock its timing onto before anything that matters arrives.

The consequence is worth knowing when comparing against other implementations:
a preamble of `0xFF` (all ones) produces the opposite — all *short* intervals.
Both work; they just present a different pattern to the acquisition logic, and
a decoder tuned for one will mis-seed on the other.

### Why the SFD is 0x99 0x4B

The start-of-frame delimiter has to be findable in a bit stream that has just
been sliding through a preamble, so it must not resemble the preamble and must
not resemble a shifted copy of itself. `0x99 0x4B` is 16 bits, i.e. 32 cells,
which the decoder correlates against rather than matching byte-aligned — the
byte boundary is not yet known when the search starts.

`OPENVLC_SFD_BYTES` can be set to 1, which selects a single `0xA3`. **This
changes the on-air format**: both ends must agree, and a mismatch presents as
100% sync failures with no other symptom.

## Forward error correction

Enabled by `OPENVLC_BEAGLEBONE_COMPAT` (on by default), which appends
Reed-Solomon parity over the frame:

| | |
|---|---|
| block size | 200 bytes (`OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE`) |
| parity | 8 bytes per block (`OPENVLC_BEAGLEBONE_RS_ECC_BYTES`) |
| corrects | 4 byte-errors per block |

An 828-byte payload needs 5 blocks, so RS costs 40 bytes — 640 cells, 320 µs of
airtime.

Both nodes **must** run the same ECC size: it is part of the wire format, not a
local robustness setting. Parity is a channel-occupancy decision as much as a
robustness one, because airtime is the scarce resource here — see
[the airtime budget](#airtime-budget).

A CRC failure therefore means something specific: the error survived RS, so
more than four bytes went wrong inside one 200-byte block. That is a much
stronger statement than a framing failure, which never reached either check.

## Airtime budget

The host paces frames at 125 fps, one every 8000 µs. At the 1 Mbit/s profile:

| | µs | share |
|---|---|---|
| payload 828 B + 18 B overhead + 40 B RS | 7088 | 89% |
| TX warm-up | ~380 | 5% |
| remaining dark gap | ~530 | 7% |

The transmitter is therefore modulating roughly **90% of the period**, and this
number governs more of the system's behaviour than anything else in this
document:

- there is almost no dark window in which a co-located receiver can work, so
  optical isolation between a node's own LED and its own photodiode is not
  optional — see `two_transceiver_test.md`
- the inter-frame gap is what the burst segmenter uses to find frame
  boundaries, and it is only a few hundred microseconds wide
- anything that lengthens a frame (larger payload, more RS parity) eats that
  gap directly

## Related

- [comparator_rx.md](comparator_rx.md) — the receive chain
- [manchester_recovery.md](manchester_recovery.md) — bit recovery detail
- [two_transceiver_test.md](two_transceiver_test.md) — running two nodes
- `../raspberry-gateway/NOTES.md` — every diagnostic counter, and how to read it
