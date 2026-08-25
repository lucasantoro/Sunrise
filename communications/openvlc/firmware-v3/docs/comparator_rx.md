# Comparator-edge receiver

## Signal path

```text
AGC/front end -> protected PB0 -> COMP1 IN+
DAC1 CH1 ----------------------> COMP1 IN-
COMP1 OUT -> internal TIM2 TI4 -> CH4 both-edge capture -> DMA1 Stream1
```

COMP1 runs in high-speed, low-hysteresis mode. Low hysteresis minimizes pulse
width shrinkage; the timer input filter removes much shorter digital glitches.
The filter is clocked from the 192-MHz timer kernel before the TIM2 prescaler.
The 1-Mbit/s profile uses filter value 5 (about 83 ns); the 500/1250 profiles
currently use value 7 (about 167 ns).

The timestamp ring is 32-bit circular DMA memory and is polled through NDTR;
its transfer interrupt is intentionally disabled. The deployed 1-Mbit/s
profile uses one 6-us delimiter. The raw corpus contains valid in-frame
intervals up to 3.86 us, while 8 us was observed merging adjacent packets.
A completion-driven 4/6-us experiment was rejected by live profiling: running
the complete CRC/RS decoder at the candidate boundary kept the single-core
STM32 close to its 8-ms packet deadline even when the failure result was cached.

The complete raw burst is retained until its per-polarity cell durations have
been estimated. Software filtering then removes a narrow pulse only when it is
below the hard-glitch limit or when joining its adjacent runs gives a better
packet-local timing fit. The legacy blind pair-cancellation policy remains
selectable for controlled A/B tests.

The compact `COMP` line still reports `sg=P/C/B/R` for controlled two-boundary
experiments. All four values remain zero in the deployed single-boundary
configuration.

## CRC-gated differential fallback

The profile-1000 decoder keeps the pair-aware `1+1` reconstruction as its
primary timing decision. During that same edge traversal it also stores the
classic differential decision (each interval independently quantised as one or
more line cells) in a bit-packed stream. A valid primary frame is returned
immediately. The alternative stream is parsed only after the primary path has
found a complete frame and failed CRC; sync failures do not pay this work.

This imports the useful event-driven property of the reference
`LiFi_Manchester` decoder without importing its unchecked delivery policy:
length, Reed-Solomon and CRC validation remain mandatory. On the deduplicated
677-capture regression corpus it changed 386 valid decodes to 395, recovering
nine CRC-valid frames and losing none of the baseline-valid frames. In compact
diagnostics `m=51` is primary mode 19 plus the differential-fallback bit 32.

Select the complete front-end policy with one definition in
`Core/Inc/openvlc_board.h`:

```c
#define OPENVLC_RX_EDGE_FILTER_MODE OPENVLC_RX_EDGE_FILTER_LEGACY     /* 1000 profile */
#define OPENVLC_RX_EDGE_FILTER_MODE OPENVLC_RX_EDGE_FILTER_CONTEXTUAL /* experiment */
#define OPENVLC_RX_EDGE_FILTER_MODE OPENVLC_RX_EDGE_FILTER_NONE       /* raw experiment */
```

Use `NONE` only for a bounded diagnostic test: real extra comparator edges can
then reach the Manchester decoder unchanged.

PC5 is not part of capture. Set `OPENVLC_COMP_DEBUG_OUTPUT=1` only when an
oscilloscope view of COMP1_OUT is required, then disable it for link tests.

## Threshold policy

The DAC threshold may be selected by the automatic scan/servo, but it must not
move while a burst is being decoded. Threshold changes are made between bursts
and the partial capture is discarded so the next packet acquires a fresh
preamble and timing model.

The best threshold is not simply the one that produces 50% global duty. It is
the one that minimizes missing/extra transitions while preserving sufficient
margin on both crossings. Use validated packet count, impossible long runs,
glitches, CRC/RS result, and run residual together.

## Limits

The packet-local polarity timing model handles small early/late edge motion
and duty-cycle distortion. It cannot uniquely recover an arbitrary transition
that the comparator never generated. If `trq` is large, or failed bursts are
complete-sized and show clustered bad pairs, improve the
analog eye/threshold or reduce the line-cell rate before increasing repair
tolerances.
