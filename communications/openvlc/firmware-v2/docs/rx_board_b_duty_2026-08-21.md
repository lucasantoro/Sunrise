# New RX board — duty-cycle distortion — 2026-08-21

Status: **open**. The link does not decode on the new receiver board. Cause
identified and measured; fix not yet applied. Parked deliberately.

## Symptom

Every frame is seen and attempted, none survives:

```
COMP ... bp=122 sp=122 okp=0 ... t0=21 t1=41 tn=32 thr=2606
         seen=2080 ok=0 crc=1198 sync=882
```

`bp == sp` with `okp == 0` is the signature: the burst segmenter finds one
burst per frame at the right rate, the decoder attempts every one, and the
payload never survives. `crc` and `sync` both climb; nothing reaches the host.

## Evidence

31 on-device captures, `docs/captures/rx-board-b-2026-08-21.tar.gz`, taken at
threshold 2000 mV (DAC 2482). Metadata is unanimous across all 31:

| field | value |
|---|---|
| `cell0_ticks` | 23–26 (median 24) |
| `cell1_ticks` | 36–42 (median 40) |
| `nominal_ticks` | 32 |
| `length_raw` | **0 on all 31** |
| `syncs` | 1 on 23 of 31, 0 on the rest |
| `edge_count` | ~11 800–12 000 |

**The cell period is correct.** 24 + 40 = 64 = 2 × 32 ticks, so the 1 Mbit/s
profile and the clock are fine. What is wrong is the crossing point: the duty
is **37.5 / 62.5**, not 50/50.

Interval histogram over all 31 captures (365 627 intervals, 64 MHz ticks):

```
  ~24  (1T narrow)   37.2%     far from the boundary, safe
  ~40  (1T wide)     35.3%     8 ticks from the boundary
  ~56  (2T narrow)   13.2%     8 ticks from the boundary
  ~72  (2T wide)     12.5%     far from the boundary, safe
  sub-legal (<16)     1.4%

  mass within +-4 ticks of the 1T/2T boundary: 8.86%
```

The four clusters sit exactly where a duty-distortion model predicts. The
decoder's 1T/2T boundary is a single global `1.5 x 32 = 48` ticks. At 50/50
duty the neighbouring classes would be 32 and 64 — 32 ticks apart. At 37/63
"1T wide" (40) and "2T narrow" (56) are only **16 ticks apart**, so the
decision margin is halved while the observed spread is +-5..6 ticks. The tails
overlap: ~7% of intervals are ambiguous, i.e. roughly 830 wrong bits per frame.

That is why the SFD still locks — it is found by correlation, which tolerates
the error — and why the two length bytes immediately after it read `0` every
single time.

## Not the cause

- **Not the optics or the scope.** The cell period is exact. The signal is fine;
  the comparator's crossing point is not.
- **Not the anti-glitch gates.** `EDGE_MIN_INTERVAL_TICKS=11`,
  `EDGE_HARD_GLITCH_TICKS=8`. The narrow half-cells are 22–25 ticks, far above
  both, so the filter is not eating real data.
- **Not new.** `capture_validation_2026-07-21.md` records the same distortion on
  the previous board at 28/36 (sum 64). The new board is worse: 24/40. The old
  board decoded at every threshold from 1.30 to 2.30 V; this one decodes at
  none tested so far.

## Offline replay reproduces it

`failure_replay` reads the OVCT `.bin` captures directly. Built as
`failure_replay_fast`, run against all 31:

```
replay: 0/31 decoded
trace=1 edges=11324 ... status=SYNC(-4) parse=-4 payload=0 t0=23 t1=40 tn=32
```

The bench reproduces the hardware failure exactly. **All further work on this
board can be done offline, with no TX-on time**, which matters because this TX
cannot stay lit for long.

## What was changed (not yet flashed)

`OPENVLC_COMP_THRESHOLD_SWEEP` enabled, and its range fixed. The old bounds
were calibrated for the previous receiver and are **DAC codes, not millivolts**:
1700..2300 spans 1370..1853 mV, entirely *below* this board's 2100 mV operating
point — the sweep could never have reached it.

```
OPENVLC_COMP_SWEEP_MIN     1700 -> 1500   (1209 mV)
OPENVLC_COMP_SWEEP_MAX     2300 -> 3300   (2659 mV)
OPENVLC_COMP_SWEEP_STEP      25 -> 50
OPENVLC_COMP_SWEEP_DWELL_S    5 -> 3      36 points, under two minutes
```

The `COMP SWEEP` log line now reports the duty, which is the question
`OPENVLC_COMP_DUTY_SERVO`'s own header says to answer before enabling it —
"enable once the logged duty is confirmed monotonic with the DAC on the real
board":

```
COMP SWEEP thr=<dac> thr_mv=<mV> delivered=<n> c0=<ticks> c1=<ticks> duty=<permille> next_thr=<dac>
```

Current duty is ~625 permille. Target is 500.

## Next step, and the fork in it

Run the sweep. Two outcomes, and they lead to different places:

1. **`duty` moves monotonically with `thr_mv` and crosses 500.** That threshold
   is the answer; set it, then enable `OPENVLC_COMP_DUTY_SERVO=1` to hold it.
   Check `OPENVLC_COMP_DUTY_INVERT` if the duty moves the wrong way.

2. **`duty` stays near 625 across 1209..2659 mV.** Then no threshold fixes it:
   the asymmetry is in the *shape* of the signal — unequal rise and fall slew in
   the new front end — not in the DC level. There is already a hint of this: the
   captures at 2000 mV and the COMP log at 2100 mV show identical `c0/c1`, so
   100 mV moved the duty by not one tick.

For outcome 2 the fix is either analog (TIA bandwidth, coupling) or software,
and the software option is clean: **the decoder already measures `cell0=24` and
`cell1=40`; it just does not use them for the decision boundary.** A per-polarity
boundary — `1.5 x 24 = 36` for the narrow level, `1.5 x 40 = 60` for the wide —
restores a comfortable margin on both classes instead of halving it on both.

Caveat worth carrying: per-polarity tracking was tried in the v3 streaming
decoder and made things *worse* there (93/109 -> 63/109). That was on a channel
with near-50/50 duty, where the per-polarity estimate only added noise. Here the
asymmetry is real and stable across 31 captures, so it is the right tool — but
verify it on the replay corpus before flashing, which now costs nothing.

## Reproduce

```bash
mkdir -p /tmp/cap && tar xzf docs/captures/rx-board-b-2026-08-21.tar.gz -C /tmp/cap
cmake -S firmware -B build -G Ninja && cmake --build build --target failure_replay_fast
for f in /tmp/cap/*.bin; do ./build/failure_replay_fast "$f"; done
```
