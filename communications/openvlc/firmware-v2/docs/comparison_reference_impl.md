# v2 against the reference implementation

A colleague's `LiFi_Manchester_H7` runs the same STM32H723 on the same bench and
solves the same problem in 884 lines, where v2 uses about 12 700. This is what
the difference buys and what it costs, measured rather than argued.

Read [wire_format.md](wire_format.md) and [modem_design.md](modem_design.md)
first; this document assumes both.

---

## The short version

They are not competing implementations of the same design. They sit at opposite
ends of one trade-off:

> **The reference trusts the channel. v2 does not.**

Every structural difference below follows from that, and neither choice is
wrong — they are correct for different optical front ends.

---

## Protocol

| | reference | v2 |
|---|---|---|
| bit rate | ~1.1 Mbit/s (`PERIOD_HALF_BIT` 124) | 1.0 Mbit/s |
| preamble | 4 bytes of `0xFF` | 8 bytes of `0xAA` |
| SFD | 3 bytes: `2A FF AA` | 2 bytes: `99 4B` |
| length | 2 bytes, big-endian | 2 bytes, big-endian |
| addressing | none | `dst` + `src`, receiver drops its own `src` |
| max payload | 4000 B | 900 B |
| integrity | **none** | CRC-16/CCITT |
| FEC | **none** | Reed-Solomon, 8 parity per 200 B block |

The preamble difference matters more than it looks. `0xFF` in Manchester is a
run of ones, which produces **all short intervals**; `0xAA` alternates, which
produces one short then **all long**. A decoder's acquisition is tuned to one
of those patterns and will seed incorrectly on the other — this is not a
cosmetic choice, and porting a decoder across without changing it fails
silently.

### No integrity check

The reference receiver writes each decoded payload byte straight to the host
UART as it is produced. There is no CRC, no checksum and no FEC anywhere in it.

This is the single most important difference for anyone comparing numbers,
because **the two systems do not measure the same thing**. A corrupted frame in
v2 is a counted loss; in the reference it is delivered as corrupt bytes and
counted as a success. Comparing v2's loss percentage against the reference's
throughput is not a comparison.

It is a coherent design: on a link carrying IP, the kernel's IP and UDP/TCP
checksums reject the corrupt packet anyway, so integrity is pushed up the stack
and the PHY stays simple. The cost is visibility — you cannot tell how much you
are corrupting without instrumenting the layer above.

---

## Receiver

### Timer arrangement

The reference puts TIM8 in **reset mode** triggered by the capture input, so
the counter zeroes on every edge and `CCR1` holds the *interval* directly. v2
captures absolute timestamps on TIM2 and subtracts.

Functionally equivalent, with one consequence worth knowing: the reference
stores intervals as `uint16_t`, so at its timer clock the longest measurable
interval is 65535 ticks, about **238 µs**. Anything darker than that aliases.
Between frames it is dark for far longer, so the interval its decoder sees
after a gap is a wrapped, arbitrary value that can land inside a valid window
and be accepted as a bit. v2's 32-bit absolute timestamps see the gap for what
it is, which is what makes burst segmentation possible at all.

### Buffering

| | reference | v2 |
|---|---|---|
| DMA | double-buffered, 64 000 intervals per half | circular ring, drained continuously |
| decode trigger | transfer complete — about 5 frames' worth | per burst, segmented on the inter-frame gap |

The reference decodes in large batches; v2 decodes one frame at a time as it
arrives. The batch approach is simpler and has lower overhead. It also means
its notion of "a frame" is purely the byte-level state machine — it has no
concept of a burst boundary, which is why the aliasing above is survivable for
it and would not be for v2.

### Interval classification

Both use two windows and a decision at 1.5 cells. The reference uses
`eps = PERIOD_HALF_BIT * 0.5`; normalised, its windows are `[0.5, 2.5] × T`
with the boundary at `1.5 T`. **v2's are identical in normalised terms.** There
is no difference here at all, which is worth stating because it is where one
would expect to find one.

### What happens to an anomalous interval

This is the substantive decoder difference.

The reference's classifier has no `else`. An interval outside both windows
falls through — but `bit_pos--` sits at the bottom of the loop body, outside
the `if`, so the bit slot advances anyway and that bit reads 0. One zero bit,
sync untouched, carry on.

v2 instead recovers actively: it absorbs sub-legal intervals as comparator
chatter, bridges short runs of missing transitions with best-effort bits, and
re-arms on a genuine gap.

The reference's approach is viable *because it has no CRC*. A dropped or
shifted bit becomes corrupt delivered data. Under a CRC the same event becomes
a rejected frame, so v2 has to try to repair it.

### Acquisition

| | reference | v2 |
|---|---|---|
| lock | first long interval after the preamble seeds bit = 0 | correlate 32 cells of SFD against the recovered stream |
| re-arm | `wait_For_SFD` on an SFD byte mismatch | preamble gate plus multiple alignment hypotheses |
| polarity | single | both tracked |
| per-polarity timing | none | `t0`/`t1` learned separately |

The reference's acquisition is one line and trusts the first long interval it
sees. That is safe when the idle line is silent, because the first long can
only be the preamble's.

### Analog front end

| | reference | v2 |
|---|---|---|
| threshold | 1.7 V fixed | configurable, 1.55 V in service |
| hysteresis | **none** | LOW |
| capture input filter | none | `OPENVLC_COMP_TIM_IC_FILTER` |
| threshold servo | none | duty servo and sweep available |

---

## Transmitter

Structurally very close. Both build a 256-entry Manchester lookup table mapping
each byte to 16 timer compare values and DMA it into a PWM compare register;
both stop the timer between frames (`BURST_MODE` is defined in the reference,
so its line goes dark exactly as v2's does).

Two differences:

- the reference enables **both** `CC1E` and `CC1NE`, driving the complementary
  output as well; v2 drives `CC1E` only. Whether that matters depends on the
  board wiring — check the schematic before copying either way.
- v2 prepends **384 cells of warm-up** before the preamble; the reference has
  none beyond its 4 preamble bytes. That warm-up exists because v2's receiver
  AGC ramps during the dark gap and would otherwise amplify its own noise floor
  into the start of the frame.

---

## Measured

### Offline, identical degraded samples

109 attempts from the synthetic channel campaign, same edges to all three:

| decoder | recovered | corrupt |
|---|---|---|
| v2 burst decoder | 108 | 0 |
| reference algorithm, ported | 103 | 0 |
| v3 streaming decoder | 93 | 0 |

On a clean channel the reference's approach is **better than v3's streaming
decoder** and close to v2's burst decoder, in a fraction of the code.

### On hardware

The port was run on the real link as a selectable mode. Changing only the
comparator threshold:

| | 1550 mV | 1700 mV |
|---|---|---|
| v2 burst decoder | **122.5 fps, 0.30% loss** | 105 fps, 15.8% |
| reference algorithm | 25 fps | 83 fps, 34% |

Two things fall out of that table.

**The optimum threshold is different for each decoder.** Raising it halved the
comparator chatter (`sa` 139/s → 67/s) and tripled in-frame dropouts
(`sd` 400/s → 1249/s). The reference algorithm needs the quiet idle line; v2
tolerates chatter but cannot reconstruct a transition that never happened. They
sit on opposite sides of the same knob.

**The reference algorithm's failure here is acquisition, not bit recovery.** At
1550 mV it acquired on only ~28 of 125 frames per second, but decoded 97% of
the ones it did acquire. Its diagnostic counters showed anomalies, acquisitions
and re-anchors all at the same rate — it was thrashing in the dark gap,
repeatedly seeding on comparator noise with the wrong half-cell pairing phase
and riding the error through the whole frame.

---

## What to take from it

**The reference is the better design for a clean optical front end.** Less
code, fewer states, fewer ways to be wrong, and on a silent idle line it wins.
If the analog side were quiet, the honest recommendation would be to adopt its
structure.

**v2's extra machinery is not sophistication for its own sake — it is what
keeps the link up on this bench.** Preamble correlation, dual polarity,
per-polarity timing and active anomaly recovery are dead weight on a clean
channel, which is exactly why the reference beats v3's streaming decoder
offline. They are what turn 28 acquisitions per second into 125 when the idle
line is not silent.

**The choice is upstream of the decoder.** What decides which approach wins is
not the software: it is what the comparator does when there is no signal. Fix
the optics and the simpler design becomes available; leave them as they are and
the simpler design cannot hold the link.

That is also the answer to "which method is better for a visible-light link" in
general: there is no better decoder in the abstract, only a better
threshold-and-decoder *pair*, and the analog operating point is chosen first.
