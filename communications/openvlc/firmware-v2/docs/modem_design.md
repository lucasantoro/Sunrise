# Modem design

How a frame gets onto the light and back off it. Read
[wire_format.md](wire_format.md) first: this document assumes the frame layout
and the Manchester timing.

Everything runs on one STM32H723 at 550 MHz. Transmit and receive are
independent hardware paths that share only the CPU and the host UART.

---

## Transmitter

```
  host UART 2 Mbaud
        |
        v
  frame assembly            openvlc_frame_build()   -> preamble/SFD/header/CRC
        |
        v
  Manchester encode         256-entry LUT, byte -> 16 TIM1 compare values
        |
        v
  DMA2 -> TIM1->CCR1        PWM, one compare value per half-cell
        |
        v
  LED driver
```

### Encoding

TIM1 runs in PWM mode with `ARR` set to one half-cell. The DMA feeds it one
compare value per half-cell from a lookup table built once at boot: each of the
256 byte values maps to 16 compare values, two per bit. A compare of 0 holds
the output low for that half-cell, a compare of `ARR + 1` holds it high.

So the transmitter never computes Manchester at runtime — it copies 16 words
per byte. Encoding an 846-byte frame measures ~640 µs
(`enc_us` in the `TX` diagnostic line), which is the memcpy, not arithmetic.

### Cell timing

`OPENVLC_STM32_TX_CELL_TICKS` is derived from the selected budget profile and
must produce the same half-cell the receiver expects: 500 ns at the 1 Mbit/s
profile. The two are separate constants on separate clock domains — TIM1 for
transmit, TIM2 for the receive input capture — so a profile change has to move
both together, which is why they are derived from `OPENVLC_PHY_RATE_KBPS`
rather than set independently.

### Warm-up

`OPENVLC_STM32_TX_WARMUP_CELLS` (384) cells of modulation precede every frame,
before the preamble. `OPENVLC_TX_WARMUP_PRBS` makes them a pseudo-random
sequence rather than a constant pattern.

The warm-up exists because the receiver's analog front end has an automatic
gain control that ramps up during the dark gap between frames. Arriving at a
frame with the AGC at maximum gain means the first cells are amplified along
with the noise floor, and the preamble is lost. The warm-up gives the AGC
something to settle on that carries no information, so the preamble arrives
into a settled receiver.

384 cells is 384 µs, about 5% of the frame period. It is a real cost paid for a
real problem: shortening it degrades acquisition sharply rather than gracefully,
so treat it as a measured value and re-measure before changing it.

### Pacing

`OPENVLC_STM32_TX_TARGET_PERIOD_US` (7800) is the minimum interval between
frame starts. The host offers frames at 125 fps, one per 8000 µs.

The 200 µs of headroom is deliberate. With the send period equal to the host
period the queue occupancy is critically loaded: any jitter in host delivery
puts an arrival before its slot is free, and the queue overflows. The `qdrop`
counter is the symptom. Headroom converts that into a small, bounded wait.

The transmitter also enforces a minimum dark gap between frames
(`OPENVLC_TX_IDLE_GAP_US`), because the receiver's burst segmenter uses the
absence of edges to find frame boundaries — see below. That gap must stay
comfortably wider than `OPENVLC_EDGE_GAP_US` on the receive side, and there is
a build-time check that fails the compile if the two are ordered wrongly.

---

## Receiver

```
  photodiode + AGC front end
        |
        v
  COMP1                     DAC threshold, LOW hysteresis
        |
        v
  TIM2 input capture        both edges, hardware filter, DMA to a circular ring
        |
        v
  burst segmentation        split on >= OPENVLC_EDGE_GAP_US with no edges
        |
        v
  edge filtering            cancel narrow pulses in pairs
        |
        v
  Manchester decode         per-polarity cell tracking, SFD correlation
        |
        v
  frame parse               length, payload, Reed-Solomon, CRC
```

### Slicing

The comparator turns the analog waveform into edges by comparing it against a
DAC threshold (`OPENVLC_COMP_THRESHOLD_MV`). `OPENVLC_COMP_HYSTERESIS_LEVEL`
adds hysteresis so a slow crossing produces one edge instead of a burst of
them.

**Where that threshold sits relative to the centre of the signal is the single
most important analog parameter in the system.** Off-centre, the two levels no
longer occupy equal time, and that is fatal rather than cosmetic — the reason
is in [the decision margin](#the-decision-margin) below.

`OPENVLC_COMP_DUTY_SERVO` can chase the threshold toward a 50/50 duty
automatically. It is off by default because its correctness depends on the duty
being monotonic in the DAC code on the specific board, which
`OPENVLC_COMP_THRESHOLD_SWEEP` establishes in one run.

### Capture

TIM2 captures both edges of the comparator output and DMAs the timestamps into
a circular buffer, so no edge depends on software latency. The hardware input
filter (`OPENVLC_COMP_TIM_IC_FILTER`) rejects pulses shorter than a few timer
clocks before they ever reach the ring.

The ring is the one place where load can silently destroy a measurement: if it
wraps before software drains it, edges are lost mid-frame and every downstream
counter is describing a truncated input. `rd` and `ovf` in the `COMP` line are
the alarms, and they are the first thing to read on any bad log.

### Burst segmentation

A frame is a run of edges with no gap longer than `OPENVLC_EDGE_GAP_US` (14 µs).
The dark period between frames exceeds that, so the segmenter recovers frame
boundaries without needing to decode anything.

The failure mode to know: if a *dead stretch* appears inside a frame — a period
where the signal exists but stops crossing the threshold — the segmenter cuts
the frame in two and both halves fail. `fr` counts that, `fg` reports the gap
that caused it. A `fg` just above 14 µs is this exact case, and it points at
slicing, not at the decoder.

### The decision margin

The decoder has to separate a 1-cell interval from a 2-cell one. Centred, those
sit at 32 and 64 ticks and the boundary goes at 48 — **16 ticks of margin on
each side**.

Duty distortion moves them toward each other. With half-cells at `t0` and `t1`
instead of 32 and 32:

```
  1-cell wide   = max(t0, t1)
  2-cell narrow = min(t0, t1) + 32
```

At 22/42 those are 42 and 54: **12 ticks apart instead of 32**. The observed
spread of real intervals is ±5 to 6 ticks, so the two classes overlap and the
payload is lost while the SFD still locks — it is found by correlation, which
tolerates the error.

This is why `t0` and `t1` are the most diagnostic numbers on the `COMP` line,
and why they must be read as a pair:

- **`t0 + t1` far from 64** — the clock is wrong, not the threshold
- **`t0` far from `t1`** — the threshold is off centre, and the margin is
  shrinking toward the failure above

Two distinct causes produce it, and they need different fixes: an asymmetric
receiver front end (unequal rise and fall slew, which no threshold fully
corrects), or light from the node's own transmitter raising the baseline under
a fixed threshold.

### Acquisition

The decoder does not know where a byte begins when a burst starts, so the SFD
is found by correlating the recovered cell stream against the 32-cell SFD
pattern rather than by matching bytes. `OPENVLC_SFD_SYNC_PREAMBLE_CELLS` (32)
cells of preamble must precede it for the match to be accepted, which rejects
noise that happens to resemble the SFD.

`OPENVLC_SFD_SYNC_HYPOTHESES_MAX` (6) bounds how many alternative alignments
are tried before giving up. Each hypothesis costs decode time, and decode time
is bounded by the frame period — `du` against 8000 µs is the budget.

### Per-polarity tracking

The decoder learns `t0` and `t1` separately and classifies each interval
against its own polarity's estimate. That is what lets it work at all under
moderate duty distortion.

It is not a substitute for a centred threshold. Tracking corrects the *centre*
of each class; it cannot widen the gap between them, and the gap is what the
decision needs.

---

## Where the numbers are

Live counters and how to read them: `../../raspberry-gateway/NOTES.md`.

Offline replay of a captured failure: `failure_replay` in `firmware/tests/`,
which takes the `.bin` captures the bridge writes and runs the real decoder
over them, with no hardware attached.
