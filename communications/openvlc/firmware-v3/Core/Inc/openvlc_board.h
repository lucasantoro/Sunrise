#ifndef OPENVLC_BOARD_H
#define OPENVLC_BOARD_H

#include "stm32h7xx_hal.h"

#if !defined(STM32H723xx)
#error "stm32-transceiver-pi-hat must be built for STM32H723xx"
#endif

/*
 * Bring-up gate retained from the validated comparator receiver.
 *
 * 0: UART heartbeat only.
 * 1: UART + OpenVLC init.
 * 2: UART + OpenVLC init + COMP1/TIM2 capture start.
 * 3: full OpenVLC runtime polling.
 */
#ifndef OPENVLC_BOOT_STAGE
#define OPENVLC_BOOT_STAGE 3
#endif

/*
 * Runtime instrumentation profile.
 *
 * 0 = production: no periodic STM32 text logs, no 10 kHz TX register monitor
 *                  and no diagnostic second pass over captured bursts.
 * 1 = compact:    one RX/TX health record per logging period.
 * 2 = deep:       register snapshots, TX pin probe, decoder detail and the
 *                 one-shot RXFAIL interval capture.
 *
 * Functional counters used by capture protection and threshold control remain
 * active at every level. Change this single define when collecting a trace.
 */
#ifndef OPENVLC_DIAGNOSTIC_LEVEL
#define OPENVLC_DIAGNOSTIC_LEVEL 1u
#endif

#if OPENVLC_DIAGNOSTIC_LEVEL > 2u
#error "OPENVLC_DIAGNOSTIC_LEVEL must be 0, 1, or 2"
#endif

#ifndef OPENVLC_BURST_TRACE
#define OPENVLC_BURST_TRACE 0
#endif

#ifndef OPENVLC_MAX_PAYLOAD_BYTES
#define OPENVLC_MAX_PAYLOAD_BYTES 900u
#endif

#ifndef OPENVLC_BEAGLEBONE_RS_ECC_BYTES
#define OPENVLC_BEAGLEBONE_RS_ECC_BYTES 8u
#endif

/*
 * STM32 RX uses the internal comparator as the optical slicer:
 * PB0 -> COMP1 IN+, DAC1_CH1 threshold, COMP1 output -> TIM2 CH4 input capture.
 * The decoder consumes edge timestamps, matching the BeagleBone PRU receiver.
 */
#ifndef OPENVLC_RX_COMPARATOR
#define OPENVLC_RX_COMPARATOR 1
#endif

/*
 * Host forwarding to a Raspberry Pi bridge. When 1, decoded IP payloads and
 * runtime logs are queued and sent over USART3 instead of blocking the optical
 * decoder. Set 0 for a plain-text bring-up console.
 */
#ifndef OPENVLC_RX_HOST_FORWARD
#define OPENVLC_RX_HOST_FORWARD 1
#endif

/*
 * 2 Mbaud, the proven rate (4 Mbaud produced no output on this board's
 * ST-LINK VCP). The old ring-wrap problem at 2 Mbaud came from the BLOCKING
 * record send and later from a TXE interrupt for every byte. Host UART TX now
 * uses normal-mode DMA, so the main loop is not stalled or preempted per byte.
 * The 2 Mbaud line rate (134 records/s = ~1.06 Mbit/s payload, 53% duty) is
 * sufficient. Keep the Pi bridge at --baud 2000000.
 */
#ifndef OPENVLC_HOST_UART_BAUD
#if OPENVLC_RX_HOST_FORWARD
#define OPENVLC_HOST_UART_BAUD 2000000u
#else
#define OPENVLC_HOST_UART_BAUD 115200u
#endif
#endif

/*
 * Host-forward queue. Each decoded packet enqueues an IP record plus its log
 * lines, and the continuous-capture poll can decode several back-to-back
 * packets in ONE iteration during iperf bursts, so 8 slots overflowed and
 * dropped IP frames (hostdrop). 32 slots (~29 KB, lives in RAM_D1) absorb a
 * whole burst train.
 */
#ifndef OPENVLC_HOST_QUEUE_DEPTH
#define OPENVLC_HOST_QUEUE_DEPTH 32u
#endif

/*
 * Comparator slice level, expressed directly in MILLIVOLTS against the analog
 * reference (VREF). The firmware converts mV -> 12-bit DAC counts with
 * OPENVLC_MV_TO_DAC(). This is the human-readable knob; keep the DAC count as a
 * derived value only.
 *
 * Optimum = the signal DC mean, because a DC-balanced Manchester waveform has
 * its single-cell eye centred exactly there. Two oscilloscope captures at
 * 12.5 MS/s put the mean at ~1998 mV (single-cell eye 1920..2077 mV, only
 * ~157 mV open; the double-cell eye is ~397 mV). A threshold even ~80 mV high
 * drops the single cells and the impossible-run (>=4 cell) count explodes. So
 * centre on the mean, not on any fixed "plateau".
 */
#ifndef OPENVLC_VREF_MV
#define OPENVLC_VREF_MV 3300u
#endif
#define OPENVLC_MV_TO_DAC(mv) \
	((uint32_t)(((uint32_t)(mv) * 4095u + OPENVLC_VREF_MV / 2u) / \
		    OPENVLC_VREF_MV))
#define OPENVLC_DAC_TO_MV(dac) \
	((uint32_t)(((uint32_t)(dac) * OPENVLC_VREF_MV + 2047u) / 4095u))

/*
 * 2026-08-20 RAISED 1550 -> 1700 to match the reference implementation, which
 * runs the same board at VOLTAGE_OFFSET 1.7 V (DAC 2109 against our 1923).
 *
 * This is the one configuration difference that plausibly explains the dark-gap
 * comparator chatter, which has been the dominant variable on this link all
 * along: it doubled between two runs with identical firmware, and it decides
 * which decoder wins. 150 mV nearer the dark baseline is 150 mV deeper into the
 * noise floor. Same hardware, so there is no reason our operating point should
 * sit lower than theirs.
 *
 * MEASURED on hardware, one bench, changing ONLY this macro and the decoder:
 *
 *                    1550 mV              1700 mV
 *   mode 3 (ours)    122.5 fps  2.27%     105 fps  15.8%
 *   mode 4 (ref)      25   fps            83  fps  34%
 *
 * Raising it helps the reference decoder enormously and hurts ours, and the
 * counters say why: sa (comparator chatter) halves, 139/s -> 67/s, while sd
 * (dropouts INSIDE a frame) triples, 400/s -> 1249/s. At 1700 mV the comparator
 * starts missing weak in-frame transitions.
 *
 * So the threshold trades idle-line noise against lost transitions, and the two
 * decoders live on opposite sides of that trade. The reference one needs a
 * quiet dark gap because its acquisition trusts the first long interval it
 * sees - at 1550 it thrashed, ref=716/s with anomalies, acquisitions and
 * re-anchors all equal; at 1700 that fell to 132/s, one per frame. Ours instead
 * absorbs chatter but cannot reconstruct a transition that never happened.
 *
 * Ours wins at BOTH thresholds, so 1550 stands. It is also the point the
 * hysteresis and duty servo below were calibrated around. The optimum for our
 * decoder may well sit below 1550 - OPENVLC_COMP_THRESHOLD_SWEEP exists for
 * that and has never been run against the streaming path.
 *
 * Note the reference also runs with NO comparator hysteresis where we run LOW,
 * so its noise margin comes entirely from this threshold. Revert with one macro
 * if the duty servo or the decode rate objects; the trade-off is documented
 * around OPENVLC_COMP_THRESHOLD_AUTO.
 */
#ifndef OPENVLC_COMP_THRESHOLD_MV
#define OPENVLC_COMP_THRESHOLD_MV 1550u
#endif
#ifndef OPENVLC_COMP_THRESHOLD_DAC
#define OPENVLC_COMP_THRESHOLD_DAC OPENVLC_MV_TO_DAC(OPENVLC_COMP_THRESHOLD_MV)
#endif

/*
 * Comparator hysteresis is the first line of defence against chatter at a
 * slow/noisy AGC threshold crossing.  Raw TIM2 captures show a sharp split:
 * valid frames contain a median of two narrow pulses, while failed frames
 * contain tens of 5..11-tick pulses spread through the payload.  A software
 * edge gate cannot restore a valid transition once chatter has overlapped it,
 * A controlled MEDIUM-hysteresis A/B test increased the packet failure rate
 * from about 4.2% to 16.2%, raised the sub-12-tick population and split the
 * expected 125 packets/s into as many as 154 apparent bursts/s.  LOW is thus
 * the measured baseline; do not promote MEDIUM/HIGH without a new raw-capture
 * result. Keep this numeric and loggable; Core/Src/comp.c maps it to the HAL
 * value.
 */
#define OPENVLC_COMP_HYSTERESIS_NONE   0u
#define OPENVLC_COMP_HYSTERESIS_LOW    1u
#define OPENVLC_COMP_HYSTERESIS_MEDIUM 2u
#define OPENVLC_COMP_HYSTERESIS_HIGH   3u
#ifndef OPENVLC_COMP_HYSTERESIS_LEVEL
#define OPENVLC_COMP_HYSTERESIS_LEVEL OPENVLC_COMP_HYSTERESIS_LOW
#endif
#if OPENVLC_COMP_HYSTERESIS_LEVEL > OPENVLC_COMP_HYSTERESIS_HIGH
#error "OPENVLC_COMP_HYSTERESIS_LEVEL must be NONE, LOW, MEDIUM, or HIGH"
#endif

/* Keep the proven 1500 mV slicer point fixed for the recovery baseline. */
#ifndef OPENVLC_COMP_THRESHOLD_AUTO
#define OPENVLC_COMP_THRESHOLD_AUTO 0
#endif
/*
 * Servo/scan bounds, also in mV (derived to DAC counts). Bench scans on the
 * Pi HAT AGC path show the useful eye is low: 1500..1550 mV is the robust
 * region, while 1600 mV can look good briefly and then lose the preamble/SFD
 * as long/impossible runs build up. Keep the scan tight enough that
 * auto-thresholding does not periodically drive the live receiver through
 * known-bad slicer points.
 */
#ifndef OPENVLC_COMP_AUTO_MIN_MV
#define OPENVLC_COMP_AUTO_MIN_MV 1400u
#endif
#ifndef OPENVLC_COMP_AUTO_MAX_MV
#define OPENVLC_COMP_AUTO_MAX_MV 1700u
#endif
#ifndef OPENVLC_COMP_AUTO_STEP_MV
#define OPENVLC_COMP_AUTO_STEP_MV 25u
#endif
#ifndef OPENVLC_COMP_AUTO_MIN
#define OPENVLC_COMP_AUTO_MIN OPENVLC_MV_TO_DAC(OPENVLC_COMP_AUTO_MIN_MV)
#endif
#ifndef OPENVLC_COMP_AUTO_MAX
#define OPENVLC_COMP_AUTO_MAX OPENVLC_MV_TO_DAC(OPENVLC_COMP_AUTO_MAX_MV)
#endif
#ifndef OPENVLC_COMP_AUTO_STEP
#define OPENVLC_COMP_AUTO_STEP OPENVLC_MV_TO_DAC(OPENVLC_COMP_AUTO_STEP_MV)
#endif
#ifndef OPENVLC_COMP_AUTO_DWELL_S
#define OPENVLC_COMP_AUTO_DWELL_S 2u
#endif

/*
 * Self-centering threshold servo (duty -> 50%). Independent of the scan-based
 * AUTO above: measures the comparator output duty from the edge array and nudges
 * the DAC toward the signal DC mean (the single-cell eye centre). Off by default
 * so the value can first be validated in the "COMP DUTY" diagnostic log; enable
 * once the logged duty is confirmed monotonic with the DAC on the real board.
 */
#ifndef OPENVLC_COMP_DUTY_SERVO
#define OPENVLC_COMP_DUTY_SERVO 0
#endif
/* Servo update period. */
#ifndef OPENVLC_COMP_DUTY_SERVO_MS
#define OPENVLC_COMP_DUTY_SERVO_MS 1000u
#endif
/* Deadband around 500 permille where the servo holds (avoids lock jitter). */
#ifndef OPENVLC_COMP_DUTY_DEADBAND
#define OPENVLC_COMP_DUTY_DEADBAND 30u
#endif
/* Minimum accumulated tick-time in a window before the servo trusts the duty. */
#ifndef OPENVLC_COMP_DUTY_MIN_TICKS
#define OPENVLC_COMP_DUTY_MIN_TICKS 200000u
#endif
/*
 * Duty polarity resolver. The run polarity is fixed from the comparator level
 * (C1VAL) at flush; under dense traffic that instantaneous read may already sit
 * in the next burst, inverting the sense. This is a build-time switch resolved
 * on the bench: if raising OPENVLC_COMP_THRESHOLD_MV makes the logged COMP DUTY
 * go UP instead of down, set this to 1.
 */
#ifndef OPENVLC_COMP_DUTY_INVERT
#define OPENVLC_COMP_DUTY_INVERT 0
#endif
/*
 * Slow outer trim of the duty-servo target. Duty=50% is the DC mean, but the
 * long/impossible-run minimum can sit slightly off centre because comparator
 * hysteresis and propagation are asymmetric. The inner duty servo remains the
 * fast loop; this trim only relocates its setpoint. Active only when
 * OPENVLC_COMP_DUTY_SERVO=1.
 */
#ifndef OPENVLC_COMP_DUTY_TARGET_PERMILLE
#define OPENVLC_COMP_DUTY_TARGET_PERMILLE 500u
#endif
#ifndef OPENVLC_COMP_DUTY_TRIM
#define OPENVLC_COMP_DUTY_TRIM 1
#endif
#ifndef OPENVLC_COMP_DUTY_TRIM_S
#define OPENVLC_COMP_DUTY_TRIM_S 10u
#endif
#ifndef OPENVLC_COMP_DUTY_TRIM_STEP
#define OPENVLC_COMP_DUTY_TRIM_STEP 8u
#endif
#ifndef OPENVLC_COMP_DUTY_TRIM_MIN
#define OPENVLC_COMP_DUTY_TRIM_MIN 440u
#endif
#ifndef OPENVLC_COMP_DUTY_TRIM_MAX
#define OPENVLC_COMP_DUTY_TRIM_MAX 560u
#endif
#ifndef OPENVLC_COMP_AUTO_RESCAN_BAD_S
#define OPENVLC_COMP_AUTO_RESCAN_BAD_S 60u
#endif
#ifndef OPENVLC_COMP_AUTO_RESCAN_QUALITY_BAD_S
#define OPENVLC_COMP_AUTO_RESCAN_QUALITY_BAD_S 2u
#endif
#ifndef OPENVLC_COMP_AUTO_RESCAN_MIN_OK_PPM
/*
 * Threshold control must not scan continuously while a separate framing or
 * decoder fault keeps packet delivery below a production target. Disable the
 * ratio-triggered rescan: a complete 60-second absence of valid packets still
 * reopens the search through OPENVLC_COMP_AUTO_RESCAN_BAD_S.
 */
#define OPENVLC_COMP_AUTO_RESCAN_MIN_OK_PPM 0u
#endif
#ifndef OPENVLC_COMP_AUTO_PERIODIC_RESCAN_S
#define OPENVLC_COMP_AUTO_PERIODIC_RESCAN_S 0u
#endif

/*
 * Edge-run quality bins, expressed relative to the nominal one-cell TIM2
 * interval selected by the active PHY profile. For budget 50
 * OPENVLC_COMP_NOMINAL_HALFCELL_TICKS=32, so these become:
 *   1-cell-ish < 52 ticks, 2-cell-ish < 88, 3-cell-ish < 120,
 *   long/impossible >= 120.
 * Manchester should rarely produce long/impossible runs; a high count means
 * the analog slicer is losing transitions even if the voltage swing is large.
 */
#ifndef OPENVLC_COMP_RUN1_MAX_TICKS
#define OPENVLC_COMP_RUN1_MAX_TICKS \
	((OPENVLC_COMP_NOMINAL_HALFCELL_TICKS * 13u) / 8u)
#endif
#ifndef OPENVLC_COMP_RUN2_MAX_TICKS
#define OPENVLC_COMP_RUN2_MAX_TICKS \
	((OPENVLC_COMP_NOMINAL_HALFCELL_TICKS * 11u) / 4u)
#endif
#ifndef OPENVLC_COMP_RUN3_MAX_TICKS
#define OPENVLC_COMP_RUN3_MAX_TICKS \
	((OPENVLC_COMP_NOMINAL_HALFCELL_TICKS * 15u) / 4u)
#endif

/* Keep runtime logging short; long interval/SFD dumps stall the polling RX.
 * INTERVAL_DUMP is a one-shot diagnostic: it prints the head/tail intervals of
 * the first fragment-sized burst. Enable only while diagnosing. */
#ifndef OPENVLC_COMP_INTERVAL_DUMP
#define OPENVLC_COMP_INTERVAL_DUMP 0
#endif

/*
 * Diagnostic RX population capture.
 *
 * Qualified successful and failed full-sized bursts share one reusable
 * uint16_t TIM2-interval snapshot. No text is formatted in the decode path,
 * but each full raw snapshot still adds about 24 KB to the same 2-Mbaud host
 * UART used for received packets. Keep this disabled for throughput/PER tests
 * and enable it only while collecting a bounded offline corpus.
 */
#ifndef OPENVLC_RX_CAPTURE
/*
 * OFF for production/measurement runs. Capture costs real link quality - with
 * it on, sync went 0.014% -> 3.6% and openvlc_frag_count 13k -> 37k on an
 * otherwise identical build. Turn it on ONLY to collect a corpus, and never
 * quote performance numbers from a capture-enabled run.
 * NOTE 2026-08-07: the Pi-side parser (raspberry-gateway/vlc_capture.py) wants
 * records starting with the 4-byte magic "OVCT" + an 88-byte BEGIN header
 * (VERSION 2, SUPPORTED_VERSIONS 1,2); the firmware currently emits something
 * else, so every record is rejected with "invalid capture record prefix".
 * Fix that format mismatch BEFORE relying on a capture run.
 */
#define OPENVLC_RX_CAPTURE 0u
#endif

/*
 * Streaming edge decoder, running in SHADOW MODE: the burst decoder stays
 * authoritative and delivers every packet, while the streaming decoder consumes
 * the same edges and only moves counters. Nothing it does can affect the link,
 * so this is safe to fly while the two are still being compared.
 *
 * Reported in the COMP line as st=<ok>/<seen> stm=<mismatch>. Read st against
 * ok: when st tracks ok on the real channel the streaming decoder is ready to
 * take over and the burst path can be retired. Measured offline on the
 * synthetic campaign it reaches 93/109 against the burst decoder's 108/109,
 * with perfect synchronisation (SFD found 109/109, length always valid) and
 * zero corrupt packets - the gap is payload bit accuracy under extreme
 * impairment, not framing.
 *
 * Modes:
 *   0 - compiled out; the device path is byte-identical to v2
 *   1 - shadow: burst decoder authoritative, streaming sampled 1 frame in 8
 *   2 - AUTHORITATIVE: the streaming decoder decodes every frame, but is still
 *       fed from the assembled burst - so it waits for a whole frame and the
 *       LATENCY IS UNCHANGED. Measured: 97.8% of seen frames, du 4500 -> 2575
 *       us, throughput 124.6 -> 123.9 fps. Half the job.
 *   3 - CONTINUOUS: the DMA ring is drained straight into the decoder and the
 *       burst pipeline does not run at all. This is the architecture the
 *       colleague's receiver uses and the only mode that takes the latency.
 *
 * 2026-08-20 set to 2. Shadow mode on hardware answered the question it was
 * built for: over 5716 real frames the streaming decoder reached 97.1% against
 * the burst decoder's 99.7%, with stm=0 - the two never once disagreed on a
 * packet. The 13.8 point gap seen on the synthetic campaign was an artefact of
 * an unrealistic stress model (+-12000 ppm, 462 removed edges); on this channel
 * it is 2.6 points.
 *
 * Shadow mode also proved it cannot run alongside: du went 4500 -> 7120 us of
 * an 8000 us period, the edge ring backed up (rp 8000 -> 31700), started
 * dropping (rd 0 -> 280) and throughput fell 124 -> 111 fps. Running one
 * decoder instead of two is not just simpler, it is the only affordable option.
 *
 * In mode 2 sc/jit in the COMP line read zero - openvlc_quality_t is produced
 * by the burst decoder and has no streaming equivalent yet. Everything else,
 * including seen/ok/crc/sync, stays meaningful.
 */
/*
 * Mode 3 COMP counters (the burst fields gd/fr/fa/fg/br/bl/bok/ovf/lock/sc/jit/
 * t0/t1/hc/du/dm are dead there by construction - no burst pipeline runs):
 *   st=<seen>/<ok>  frames the streaming decoder attempted / recovered
 *   stm=<n>         stream-vs-burst mismatches (always 0 in mode 3: no burst)
 *   sa=<n>          sub-legal intervals absorbed as comparator chatter
 *   sl=<n>          over-long intervals -> transitions bridged best-effort
 *   sk=<n>          missing transitions bridged
 *   sr=<n>          lanes re-armed at a frame boundary (OPENVLC_STREAM_GAP_REARM)
 *
 * sa/sl/sk were previously compiled but never reported; offline they read
 * absorbed=0 toolong=77 skipped=0, because the bench feeds pre-filtered edges.
 * On hardware the DMA ring feeds them raw, and the first real numbers were:
 * sa 100/s, sl 8342/s, sk 304/s against 123 fps. sl looks alarming but is
 * mostly benign - the decoder is unsynced through the ~7 ms dark gap, so every
 * comparator twitch there lands in it. What sl exposed is that sl-sk events
 * left the byte lanes dirty; see OPENVLC_STREAM_GAP_REARM.
 */
/*
 * 1 = the reference implementation's policy for an interval that falls outside
 * both timing windows: consume one bit slot and leave that bit at 0. Its
 * classifier has no else branch, but bit_pos-- is at the bottom of the loop
 * body outside it, so the slot advances anyway. Sync is untouched: its
 * wait_For_SFD is raised only by an SFD byte mismatch, never by a timing
 * anomaly. Its eps and 1.5T boundary already match ours in normalised terms,
 * so this is the substantive decoder difference that remains between the two.
 *
 * The offline campaign cannot decide it: 93/109 either way, with only the
 * failure attribution moving (bad_len 0->2, parse 16->14). The bench feeds
 * pre-filtered edges, so its 77 out-of-window intervals are all frame
 * boundaries - exactly the case where the two policies agree. Needs hardware.
 */
#ifndef OPENVLC_STREAM_ANOMALY_ZERO_BIT
#define OPENVLC_STREAM_ANOMALY_ZERO_BIT 0u
#endif

#ifndef OPENVLC_STREAM_GAP_REARM
#define OPENVLC_STREAM_GAP_REARM 1u  /* re-arm SFD search on a real gap */
#endif

#ifndef OPENVLC_STREAM_DEGLITCH
#define OPENVLC_STREAM_DEGLITCH 1u  /* cancel narrow pulses as the burst path does */
#endif

#ifndef OPENVLC_RX_STREAMING
#define OPENVLC_RX_STREAMING 3u
#endif

/*
 * 4 - REFERENCE: the ported reference edges->bits decoder replaces ours, with
 *     our framing, CRC and host path unchanged above it. MEASURED offline on
 *     109 identical degraded attempts: burst 108, reference port 103, our
 *     streaming decoder 93, zero corrupt packets from any of the three.
 */
#if OPENVLC_RX_STREAMING >= 4
#ifndef OPENVLC_RX_REFERENCE
#define OPENVLC_RX_REFERENCE 1u
#endif
#endif

/*
 * Capture the comparator intervals before software edge filtering.  A raw
 * trace is the only representation from which alternative glitch gates and
 * timing-aware filters can be replayed offline.  The option has no RAM or CPU
 * cost when OPENVLC_RX_CAPTURE is zero.
 */
#ifndef OPENVLC_RX_CAPTURE_RAW
#define OPENVLC_RX_CAPTURE_RAW 0u
#endif

#ifndef OPENVLC_RX_CAPTURE_CHUNK_INTERVALS
#define OPENVLC_RX_CAPTURE_CHUNK_INTERVALS 384u
#endif

#if OPENVLC_RX_CAPTURE_CHUNK_INTERVALS > 442u
#error "OPENVLC_RX_CAPTURE_CHUNK_INTERVALS exceeds a 900-byte host record"
#endif

#ifndef OPENVLC_RX_CAPTURE_PERIOD_MS
#define OPENVLC_RX_CAPTURE_PERIOD_MS 20u
#endif

#ifndef OPENVLC_RX_CAPTURE_MAX_INTERVALS
#define OPENVLC_RX_CAPTURE_MAX_INTERVALS 14000u
#endif

/*
 * Build a balanced offline corpus while retaining more failures than the old
 * 16-trace recorder. These are quotas, not buffers: the same 28 KB snapshot is
 * reused and streamed to the Pi one capture at a time.
 */
#ifndef OPENVLC_RX_CAPTURE_MAX_FAILURES
#define OPENVLC_RX_CAPTURE_MAX_FAILURES 60u
#endif
#ifndef OPENVLC_RX_CAPTURE_MAX_SUCCESSES
#define OPENVLC_RX_CAPTURE_MAX_SUCCESSES 4u
#endif

#if (OPENVLC_RX_CAPTURE_MAX_FAILURES + \
	OPENVLC_RX_CAPTURE_MAX_SUCCESSES) == 0u
#error "At least one RX capture quota must be non-zero"
#endif

/* Compatibility aliases for older local build overrides. */
#ifndef OPENVLC_RX_FAILURE_TRACE
#define OPENVLC_RX_FAILURE_TRACE OPENVLC_RX_CAPTURE
#endif
#ifndef OPENVLC_RX_FAILURE_TRACE_PERIOD_MS
#define OPENVLC_RX_FAILURE_TRACE_PERIOD_MS OPENVLC_RX_CAPTURE_PERIOD_MS
#endif

/*
 * Ignore capture-start fragments and let comparator/AGC timing settle before
 * arming. Qualification happens once after this many consecutive valid
 * packets; a failure before qualification restarts the count.
 */
#ifndef OPENVLC_RX_CAPTURE_ARM_AFTER_OK
#define OPENVLC_RX_CAPTURE_ARM_AFTER_OK 20u
#endif

#ifndef OPENVLC_RX_CAPTURE_MIN_EDGES
#define OPENVLC_RX_CAPTURE_MIN_EDGES 8000u /*OPENVLC_RX_MIN_DECODE_EDGES*/
#endif

/*
 * Space snapshots so adjacent packets and one disturbed burst cannot dominate
 * the corpus. Failures get priority after MIN_FRAME_GAP. A successful frame is
 * sampled less often, at OK_FRAME_GAP, until its independent quota is full.
 */
#ifndef OPENVLC_RX_CAPTURE_MIN_FRAME_GAP
#define OPENVLC_RX_CAPTURE_MIN_FRAME_GAP 20u
#endif
#ifndef OPENVLC_RX_CAPTURE_OK_FRAME_GAP
#define OPENVLC_RX_CAPTURE_OK_FRAME_GAP 64u
#endif

#if OPENVLC_RX_CAPTURE_OK_FRAME_GAP < OPENVLC_RX_CAPTURE_MIN_FRAME_GAP
#error "OPENVLC_RX_CAPTURE_OK_FRAME_GAP must be >= MIN_FRAME_GAP"
#endif

/* Compatibility aliases for older local build overrides. */
#ifndef OPENVLC_RX_FAILURE_TRACE_ARM_AFTER_OK
#define OPENVLC_RX_FAILURE_TRACE_ARM_AFTER_OK OPENVLC_RX_CAPTURE_ARM_AFTER_OK
#endif
#ifndef OPENVLC_RX_FAILURE_TRACE_MIN_EDGES
#define OPENVLC_RX_FAILURE_TRACE_MIN_EDGES OPENVLC_RX_CAPTURE_MIN_EDGES
#endif

/*
 * Threshold sweep. Ran 2026-08-07 on the bad node, 1700..2300 DAC by 25, 5 s
 * dwell, two full cycles. RESULT, recorded so nobody repeats it:
 *
 *   - r2023 (sub-cell chatter) is STRICTLY MONOTONIC in the threshold across the
 *     whole 600-count range: ~2050/s at 1700 rising to ~44600/s at 2300, roughly
 *     doubling every 150 counts. There is NO minimum inside the window, so there
 *     is no "correct operating point" to find.
 *   - CRC failures do NOT follow it. Per 627-frame dwell the CRC delta stayed a
 *     scattered 1..33 while r2023 varied 20x, and the two cycles disagreed at the
 *     same thresholds (1850 gave +4 then +33). The losses are TEMPORAL bursts,
 *     independent of the slicer point.
 *   - `delivered` is useless here: the source is paced at 125 fps, so it pins at
 *     ~620/dwell everywhere and only sags at the very worst points.
 *
 * So the residual loss on that board is NOT threshold placement, and r2023 is a
 * threshold-position indicator, not a loss predictor - it is only comparable
 * BETWEEN boards at the SAME threshold (which is how the board asymmetry was
 * found: 45/s vs ~4300/s at 1923). The remaining difference is analog amplitude
 * or noise in that board's RX front end. Left disabled; re-enable only to
 * characterise a NEW board, and read r2023 against a known-good reference.
 */
#ifndef OPENVLC_COMP_THRESHOLD_SWEEP
#define OPENVLC_COMP_THRESHOLD_SWEEP 0
#endif
#ifndef OPENVLC_COMP_SWEEP_MIN
#define OPENVLC_COMP_SWEEP_MIN 1700u
#endif
#ifndef OPENVLC_COMP_SWEEP_MAX
#define OPENVLC_COMP_SWEEP_MAX 2300u
#endif
#ifndef OPENVLC_COMP_SWEEP_STEP
#define OPENVLC_COMP_SWEEP_STEP 25u
#endif
#ifndef OPENVLC_COMP_SWEEP_DWELL_S
#define OPENVLC_COMP_SWEEP_DWELL_S 5u
#endif

#ifndef OPENVLC_SAMPLES_PER_SYMBOL
#define OPENVLC_SAMPLES_PER_SYMBOL 5u
#endif

/* Match the BeagleBone TX setup: OPENVLC_PREAMBLE_LEN=8. */
#ifndef OPENVLC_PREAMBLE_BYTES
#define OPENVLC_PREAMBLE_BYTES 8u
#endif

#ifndef OPENVLC_ENABLE_MPU
#define OPENVLC_ENABLE_MPU 0
#endif

/*
 * The Cortex-M7 decoder executes from internal Flash. Without I-cache, a valid
 * 780-byte BBB frame takes about 35 ms to decode, while 400 kbit/s traffic
 * presents one frame roughly every 15 ms. Instruction caching is independent
 * of DMA coherency and is therefore safe for the TIM2 capture path.
 */
#ifndef OPENVLC_ENABLE_ICACHE
#define OPENVLC_ENABLE_ICACHE 1
#endif

/*
 * D-cache for all CPU buffers (edge_burst, host queue, frame buffers, AXI
 * SRAM in general). DMA coherency is preserved by placing the TIM2 capture
 * ring in a dedicated 128 KB NON-CACHEABLE MPU region at the start of RAM_D1
 * (section .dma_ring, MPU region 1 - see the cache setup in main.c).
 *
 * Why: without D-cache every decode RAM access pays the AXI latency and the
 * per-frame work (~7.4 ms) sits right at the 134 frame/s period (7.46 ms);
 * at full 800 kbit/s load the poll backlog random-walks into the capture ring
 * limit and wraps silently (~5 wraps/s, ~4.5 frames lost each). With D-cache
 * the decode roughly halves and the margin returns.
 */
#ifndef OPENVLC_ENABLE_DCACHE
#define OPENVLC_ENABLE_DCACHE 1
#endif

/*
 * HAT production diagnostic: immediately after peripheral setup, drive PE9
 * (TIM1_CH1 / OWC_TX) with a scope-friendly square wave.  This runs before
 * the UART parser and optical stack, so it isolates the pin/timer/PCB path.
 */
#ifndef OPENVLC_TX_BOOT_PIN_TEST_MS
#define OPENVLC_TX_BOOT_PIN_TEST_MS 0u
#endif
#ifndef OPENVLC_TX_BOOT_PIN_TEST_HZ
#define OPENVLC_TX_BOOT_PIN_TEST_HZ 100000u
#endif

/*
 * Keep only the transition-clocked decoder, like the known-good BeagleBone PRU
 * RX. TIM2 input capture already gives clean edge timestamps, so sampled
 * symbol recovery and matched-filter scan are not part of this STM32 profile.
 */
/*
 * BeagleBone Manchester timing profile:
 *   500  -> original standalone TX budget 100, about 16 TIM2 ticks/cell;
 *   1000 -> fast TX budget 50, TIM2 capture at 64 MHz.
 *   1250 -> experimental TX budget 40, about 26 TIM2 ticks/cell.
 *
 * These values identify the approximate line-cell rate, not the decoded
 * Manchester payload bit rate. TX and RX profiles must always match.
 */
/*
 * Close a burst as soon as the comparator has remained idle for the delimiter.
 * The implementation samples the DMA head twice around the free-running timer
 * read, so an edge arriving during the decision cannot split a live packet.
 * Waiting for the first edge of the NEXT frame starts a synchronous decode
 * roughly one inter-frame guard late and needlessly grows the capture backlog.
 */
#ifndef OPENVLC_RX_IDLE_TIMEOUT_FLUSH
#define OPENVLC_RX_IDLE_TIMEOUT_FLUSH 1u
#endif

#ifndef OPENVLC_PHY_RATE_KBPS
#define OPENVLC_PHY_RATE_KBPS 1000u
#endif

/* Comparator-edge filter policies, common to every PHY profile. */
#define OPENVLC_RX_EDGE_FILTER_NONE       0u
#define OPENVLC_RX_EDGE_FILTER_LEGACY     1u
#define OPENVLC_RX_EDGE_FILTER_CONTEXTUAL 2u
#ifndef OPENVLC_RX_EDGE_FILTER_MODE
#if OPENVLC_PHY_RATE_KBPS == 1000u
#define OPENVLC_RX_EDGE_FILTER_MODE OPENVLC_RX_EDGE_FILTER_LEGACY
#else
#define OPENVLC_RX_EDGE_FILTER_MODE OPENVLC_RX_EDGE_FILTER_CONTEXTUAL
#endif
#endif

#if OPENVLC_PHY_RATE_KBPS == 1000u

#ifndef OPENVLC_TIM2_IC_TICK_HZ
#define OPENVLC_TIM2_IC_TICK_HZ 64000000u
#endif

/* TIM2 kernel clock is 192 MHz (SYSCLK 384). 192/64 = 3 -> prescaler 2. */
#ifndef OPENVLC_TIM2_IC_PRESCALER
#define OPENVLC_TIM2_IC_PRESCALER 2u
#endif

/*
 * 2026-08-06 corpus (68 raw v2 captures at thr=1923/1550 mV: 32 good, 17 sync,
 * 12 crc, 6 decode, 1 length) replayed through the host decoder. Sweeping the
 * gate from 0 to 24 ticks gives a single sharp optimum at 8 ticks:
 *
 *   gate  ok/32  sync/17  total/68        gate  ok/32  sync/17  total/68
 *      6     31        5        37          12     32        8        40 (old)
 *      7     31        9        42          16     31        9        40
 *      8     32       12        45 (peak)   20     31        7        39
 *      9     32       11        43          24     31       10        41
 *
 * Eight ticks was the optimum for that corpus, whose failures were dominated
 * by head-of-burst chatter.
 *
 * 2026-08-06 RE-MEASURED after OPENVLC_TX_IDLE_KEEPALIVE removed the AGC
 * settling transient. The failure population changed completely: a fresh
 * 64-capture corpus (32 good, 31 crc, 1 decode) at 1550 mV shows head chatter
 * gone in BOTH classes (0 sub-8-tick pulses in the first 1400 raw intervals),
 * and the discriminator is now purely PAYLOAD chatter - every one of the 32
 * good frames has exactly 0, while 30 of 31 crc failures have more. With the
 * corrupting pulses now inside the data instead of the preamble, the gate
 * optimum moves up:
 *
 *   gate   8 -> 42/64      gate  11 -> 45/64 (peak)     gate  14 -> 42/64
 *   gate  10 -> 44/64      gate  13 -> 44/64
 *
 * All 32 known-good frames survive at every gate from 9 to 14, so 11 is a
 * safe peak rather than a cliff. Unlike the previous corpus the hypothesis
 * budget now matters as much as the gate (see
 * OPENVLC_SFD_SYNC_HYPOTHESES_MAX): 5 recoveries at budget 1 versus 13 at 2.
 * Do not change this without replaying a fresh balanced raw corpus.
 */
#ifndef OPENVLC_EDGE_MIN_INTERVAL_TICKS
#define OPENVLC_EDGE_MIN_INTERVAL_TICKS 11u
#endif

/*
 * Edge filtering has three explicit policies:
 *   NONE       - preserve every captured comparator transition;
 *   LEGACY     - cancel every adjacent pair below EDGE_MIN_INTERVAL_TICKS;
 *   CONTEXTUAL - cancel an ultra-short pulse unconditionally, otherwise only
 *                when its neighbouring intervals fit the packet timing model
 *                better after removal.
 *
 * EDGE_MIN_INTERVAL_TICKS is therefore a candidate/glitch aperture, not the
 * minimum duration that the timing estimator is allowed to learn.
 */
#ifndef OPENVLC_EDGE_HARD_GLITCH_TICKS
#define OPENVLC_EDGE_HARD_GLITCH_TICKS 8u
#endif
#ifndef OPENVLC_EDGE_CONTEXT_MARGIN_TICKS
#define OPENVLC_EDGE_CONTEXT_MARGIN_TICKS 2u
#endif

/*
 * The v2 live scan showed a stable 23..24-tick short cell but only ~1.2 M
 * captured edges/s, with no TIM2 overcapture. ICFilter=7 required about 167 ns
 * of stable input and could therefore erase a narrowed real pulse before DMA.
 * Filter 5 halves that hardware aperture to about 83 ns. Live captures now
 * show chatter pulses below the valid-cell cluster. A raw balanced
 * 32-good/32-fail corpus retained all 32 good frames with a 12-tick gate and
 * recovered 14 failures; the contextual filter recovered none, a 20-tick
 * gate recovered only four, and 21 ticks rejected a known-good frame. Twelve
 * ticks (187.5 ns at 64 MHz) is therefore the measured physical floor, not a
 * broad timing tolerance. Do not raise it without a new good-frame corpus.
 *
 * 2026-08-07 - RAISED 5 -> 6 (about 83 ns -> 125 ns aperture). The TX idle gap
 * (OPENVLC_TX_IDLE_GAP_US) buys clean burst segmentation but makes the AGC
 * re-settle after every frame, which floods the r07 bin (0..7 ticks = 0..110
 * ns) with ~100k pulses/s. Filter 5 stops only below 83 ns, so the 83..110 ns
 * part reaches the DMA ring and the decoder walks every one of them: the ring
 * peak went 9.5k -> 35k and openvlc_edge_ring_drops appeared (~0.36/s). A ring
 * drop discards the WHOLE backlog (~2-3 frames) without touching seen/ovf/sync,
 * which is why the COMP counters said 0.53% while the bridge measured 1.05%
 * end-to-end.
 * 125 ns = 8 ticks sits BELOW the 12-tick physical floor documented above with
 * 4 ticks of margin, so unlike ICFilter=7 (167 ns, only 20 ns under the floor)
 * it cannot erase a narrowed real pulse - it only removes noise the software
 * gate would discard anyway, but does it before DMA and CPU pay for it.
 * VERIFY: r07 must collapse and rd must stop climbing, while bok stays ~12180
 * and crc stays ~0. If bok or crc degrade, the aperture reached real cells -
 * go straight back to 5, do not try 7.
 */
#ifndef OPENVLC_COMP_TIM_IC_FILTER
#define OPENVLC_COMP_TIM_IC_FILTER 6u
#endif

/*
 * Working copy for ONE packet burst (~11.4k edges for the iperf frame).
 * 16384 (64 KB) leaves ~40% headroom. Keep this small: it shares RAM_D1
 * (512 KB) with the 192 KB edge-capture DMA ring; the previous 98304 (384 KB)
 * overflowed RAM_D1 once the ring moved there.
 */
#ifndef OPENVLC_EDGE_BURST_LEN
#define OPENVLC_EDGE_BURST_LEN 16384u
#endif

#ifndef OPENVLC_COMP_MIN_HALFCELL_TICKS
#define OPENVLC_COMP_MIN_HALFCELL_TICKS 12u
#endif

/*
 * TX budget 50 -> ~0.5 us line cell. At a 64 MHz TIM2 tick that is ~32 ticks
 * per cell, so the nominal one-cell duration is 32 (the timing model targets
 * 2x this = 64 ticks for the two polarity-delayed cells, matching the
 * comp_estimate_timing_model comment). 64 would target 128 ticks and reject the
 * real ~64-tick cell pair, dropping the decoder onto the crude fallback.
 */
#ifndef OPENVLC_COMP_NOMINAL_HALFCELL_TICKS
#define OPENVLC_COMP_NOMINAL_HALFCELL_TICKS 32u
#endif

#ifndef OPENVLC_COMP_SPLIT_HIST_MAX_TICKS
#define OPENVLC_COMP_SPLIT_HIST_MAX_TICKS 256u
#endif

#ifndef OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS
#define OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS 96u
#endif

/*
 * Keep the RX delimiter well below the 32-cell TX low tail (~16 us).
 * A live auto-threshold test proved that 8 us can join consecutive frames:
 * bursts then exceed the 16384-edge packet buffer, pathological decodes take
 * tens of milliseconds and the circular capture ring starts dropping data.
 * A balanced raw corpus exposed valid packets with internal intervals up to
 * 247 TIM2 ticks (3.86 us) and failed packets truncated at the old 4 us
 * delimiter. Six microseconds retains those crossings while staying below the
 * measured-unsafe 8-us setting. Live A/B testing rejected a 4-us speculative
 * full decode followed by a 6-us hard boundary: even with cached failures it
 * kept the STM32 close to its 8-ms packet deadline and increased ring pressure.
 */
/* Shortest inter-frame gap measured on the wire (50 MS/s, 19 boundaries). */
#ifndef OPENVLC_TX_IDLE_GAP_MEASURED_MIN_US
#define OPENVLC_TX_IDLE_GAP_MEASURED_MIN_US 22u
#endif

/*
 * 2026-08-18 REVISED to 14 us. The 6 us it replaces, and the "8 us is unsafe"
 * finding that one rests on, both predate OPENVLC_TX_IDLE_GAP_US: back then
 * the only boundary was the ~16 us TX low tail, so 8 us really did join
 * frames. The transmitter now punches a deliberate dark gap, and a 50 MS/s
 * capture of 19 consecutive inter-frame boundaries measured it at 22.9-32.5 us
 * (min 22.9), not 10 - openvlc_stm32_tx_idle_poll() is polled from the main
 * loop, so the configured 10 us is a floor plus loop latency.
 *
 * Meanwhile the live log shows the delimiter firing on holes INSIDE frames:
 * openvlc_frag_last_gap_us reports 6-9 us with openvlc_frag_last_at at
 * 100-4400 edges, i.e. whole frames cut in half a few times a second, and
 * every fragment is a lost frame.
 *
 * 14 us sits between the two populations with margin on both sides: 55% above
 * the longest observed intra-frame hole, 39% below the shortest measured
 * inter-frame gap.
 *
 * Direction of risk: raising this makes MERGING more likely, not less (a
 * merged burst overruns OPENVLC_EDGE_BURST_LEN and shows up as `ovf`). That is
 * the right trade here only because fragments cost ~1-2 frames/s while `ovf`
 * costs roughly one every few minutes. If `ovf` climbs faster than `fr` falls,
 * this went too far.
 */
#ifndef OPENVLC_EDGE_GAP_US
#define OPENVLC_EDGE_GAP_US 14u
#endif

#if OPENVLC_EDGE_GAP_US >= OPENVLC_TX_IDLE_GAP_MEASURED_MIN_US
#error "RX delimiter must stay below the measured TX inter-frame gap"
#endif

/*
 * Keep candidate and hard boundaries equal in the deployed profile. The
 * completion-driven two-boundary mechanism remains available for controlled
 * experiments, but a full speculative decode is not deadline-safe at 125 fps.
 */
#ifndef OPENVLC_EDGE_HARD_GAP_US
#define OPENVLC_EDGE_HARD_GAP_US OPENVLC_EDGE_GAP_US
#endif

/*
 * Require this many alternating raw preamble cells immediately before an SFD
 * match. The 8-byte 0xAA preamble provides 64 cells. 32 proved too strict
 * once the re-tuned AGC distorts the burst head: the gate then rejects the
 * TRUE SFD on every frame (SFDSYNC pre_rej) and the decoder later false-locks
 * inside the repetitive iperf payload (deterministic garbage lenraw, lock
 * thousands of cells into the burst). 12 still rejects isolated 0xA3 cell
 * patterns that lack any training run, while tolerating an AGC-damaged
 * leading 3/4 of the preamble. Note the gate cannot reject false locks inside
 * long alternating payload stretches at ANY value, so larger values buy no
 * extra protection in iperf-like traffic.
 */
#ifndef OPENVLC_SFD_SYNC_PREAMBLE_CELLS
#define OPENVLC_SFD_SYNC_PREAMBLE_CELLS 12u
#endif

/*
 * At 1 Mbit/s the active profile has the same comparator/slicer failure mode
 * seen in the 500/1250 profiles: one adjacent raw-preamble cell pair can be
 * merged while the true SFD remains recoverable. Requiring a perfect 12-cell
 * alternating gate rejects those real SFDs and shows up as COMP fp growth.
 * Keep this at one pair only; looser gates increase payload false-locks.
 */
#ifndef OPENVLC_SFD_SYNC_PREAMBLE_MAX_BAD_PAIRS
#define OPENVLC_SFD_SYNC_PREAMBLE_MAX_BAD_PAIRS 1u
#endif
#ifndef OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_BAD_PAIRS
#define OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_BAD_PAIRS 1u
#endif
#ifndef OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_SFD_ERRORS
#define OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_SFD_ERRORS 0u
#endif

/*
 * Scope captures show that an otherwise recoverable AGC waveform can corrupt
 * two isolated SFD cells and isolated first cells of Manchester pairs. The
 * strict alternating preamble gate remains mandatory; only then may the SFD
 * correlator accept this bounded distance. A distance-3 A/B capture became a
 * comparator-fragment storm before it could provide a valid decoder result,
 * so keep the last independently validated distance here.
 */
#ifndef OPENVLC_SFD_SYNC_MAX_CELL_ERRORS
#define OPENVLC_SFD_SYNC_MAX_CELL_ERRORS 1u
#endif

#ifndef OPENVLC_SFD_SYNC_MAX_CONSECUTIVE_BAD_PAIRS
#define OPENVLC_SFD_SYNC_MAX_CONSECUTIVE_BAD_PAIRS 8u
#endif

/*
 * Equal Manchester pairs are a slicer defect: one of the two line cells was
 * moved across the comparator threshold. On the Pi HAT analog path, the
 * oscilloscope capture shows the first cell remains the stable reference after
 * short-pulse cancellation; reconstruct the missing second cell as !first.
 */
#ifndef OPENVLC_SFD_SYNC_BAD_PAIR_INVERT_FIRST
#define OPENVLC_SFD_SYNC_BAD_PAIR_INVERT_FIRST 1u
#endif

#elif OPENVLC_PHY_RATE_KBPS == 1250u

/*
 * TX budget 40 profile. Keep the 64 MHz capture clock so one nominal 400 ns
 * line cell is represented by about 25.6 timer ticks. The measured value may
 * be slightly higher because the PRU loop has fixed instruction overhead.
 */
#ifndef OPENVLC_TIM2_IC_TICK_HZ
#define OPENVLC_TIM2_IC_TICK_HZ 64000000u
#endif

#ifndef OPENVLC_TIM2_IC_PRESCALER
#define OPENVLC_TIM2_IC_PRESCALER 2u
#endif

/* ICFilter=7 samples at fDTS/4 and requires eight stable samples: 167 ns at
 * the 192 MHz TIM2 kernel clock. This rejects TX-induced comparator pulses
 * before DMA while preserving the shortest valid budget-40 transitions. */
#ifndef OPENVLC_EDGE_MIN_INTERVAL_TICKS
#define OPENVLC_EDGE_MIN_INTERVAL_TICKS 0u
#endif

#ifndef OPENVLC_EDGE_HARD_GLITCH_TICKS
#define OPENVLC_EDGE_HARD_GLITCH_TICKS 6u
#endif
#ifndef OPENVLC_EDGE_CONTEXT_MARGIN_TICKS
#define OPENVLC_EDGE_CONTEXT_MARGIN_TICKS 2u
#endif

#ifndef OPENVLC_COMP_TIM_IC_FILTER
#define OPENVLC_COMP_TIM_IC_FILTER 7u
#endif

#ifndef OPENVLC_EDGE_BURST_LEN
#define OPENVLC_EDGE_BURST_LEN 16384u
#endif

#ifndef OPENVLC_COMP_MIN_HALFCELL_TICKS
#define OPENVLC_COMP_MIN_HALFCELL_TICKS 12u
#endif

#ifndef OPENVLC_COMP_NOMINAL_HALFCELL_TICKS
#define OPENVLC_COMP_NOMINAL_HALFCELL_TICKS 26u
#endif

#ifndef OPENVLC_COMP_SPLIT_HIST_MAX_TICKS
#define OPENVLC_COMP_SPLIT_HIST_MAX_TICKS 192u
#endif

#ifndef OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS
#define OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS 80u
#endif

/*
 * The budget-40 TX wrapper retains a 32-cell inter-frame gap, approximately
 * 12.8 us. A 4 us RX delimiter remains safely below that gap and above every
 * valid Manchester run.
 */
#ifndef OPENVLC_EDGE_GAP_US
#define OPENVLC_EDGE_GAP_US 4u
#endif

#ifndef OPENVLC_SFD_SYNC_PREAMBLE_CELLS
#define OPENVLC_SFD_SYNC_PREAMBLE_CELLS 12u
#endif

/*
 * Comparator edges can occasionally merge one preamble cell pair while the SFD
 * itself remains decodable. Accept one damaged adjacent pair in the raw
 * preamble gate; keep this low because a loose preamble gate increases
 * false-locks inside payload.
 */
#ifndef OPENVLC_SFD_SYNC_PREAMBLE_MAX_BAD_PAIRS
#define OPENVLC_SFD_SYNC_PREAMBLE_MAX_BAD_PAIRS 1u
#endif

#ifndef OPENVLC_SFD_SYNC_MAX_CELL_ERRORS
#define OPENVLC_SFD_SYNC_MAX_CELL_ERRORS 1u
#endif

#ifndef OPENVLC_SFD_SYNC_MAX_CONSECUTIVE_BAD_PAIRS
#define OPENVLC_SFD_SYNC_MAX_CONSECUTIVE_BAD_PAIRS 8u
#endif

#ifndef OPENVLC_SFD_SYNC_BAD_PAIR_INVERT_FIRST
#define OPENVLC_SFD_SYNC_BAD_PAIR_INVERT_FIRST 1u
#endif

#elif OPENVLC_PHY_RATE_KBPS == 500u

#ifndef OPENVLC_TIM2_IC_TICK_HZ
#define OPENVLC_TIM2_IC_TICK_HZ 16000000u
#endif

/* TIM2 kernel clock is 192 MHz (SYSCLK 384). 192/16 = 12 -> prescaler 11. */
#ifndef OPENVLC_TIM2_IC_PRESCALER
#define OPENVLC_TIM2_IC_PRESCALER 11u
#endif

#ifndef OPENVLC_EDGE_MIN_INTERVAL_TICKS
#define OPENVLC_EDGE_MIN_INTERVAL_TICKS 8u
#endif

#ifndef OPENVLC_EDGE_HARD_GLITCH_TICKS
#define OPENVLC_EDGE_HARD_GLITCH_TICKS 3u
#endif
#ifndef OPENVLC_EDGE_CONTEXT_MARGIN_TICKS
#define OPENVLC_EDGE_CONTEXT_MARGIN_TICKS 1u
#endif

#ifndef OPENVLC_COMP_TIM_IC_FILTER
#define OPENVLC_COMP_TIM_IC_FILTER 7u
#endif

/*
 * Working copy for ONE packet burst (~11.4k edges for the iperf frame).
 * 16384 (64 KB) leaves ~40% headroom. Keep this small: it shares RAM_D1
 * (512 KB) with the 192 KB edge-capture DMA ring; the previous 98304 (384 KB)
 * overflowed RAM_D1 once the ring moved there.
 */
#ifndef OPENVLC_EDGE_BURST_LEN
#define OPENVLC_EDGE_BURST_LEN 16384u
#endif

#ifndef OPENVLC_COMP_MIN_HALFCELL_TICKS
#define OPENVLC_COMP_MIN_HALFCELL_TICKS 8u
#endif

#ifndef OPENVLC_COMP_NOMINAL_HALFCELL_TICKS
#define OPENVLC_COMP_NOMINAL_HALFCELL_TICKS 16u
#endif

#ifndef OPENVLC_COMP_SPLIT_HIST_MAX_TICKS
#define OPENVLC_COMP_SPLIT_HIST_MAX_TICKS 96u
#endif

#ifndef OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS
#define OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS 28u
#endif

/*
 * A valid budget-100 Manchester frame has a transition at least every two
 * approximately 1 us cells. The previous 25 us delimiter could merge
 * back-to-back frames when the BBB TX queue stayed full; the decoder then
 * returned the first packet and discarded the remainder of the merged burst.
 */
#ifndef OPENVLC_EDGE_GAP_US
#define OPENVLC_EDGE_GAP_US 8u
#endif

#ifndef OPENVLC_SFD_SYNC_PREAMBLE_CELLS
#define OPENVLC_SFD_SYNC_PREAMBLE_CELLS 12u
#endif

#ifndef OPENVLC_SFD_SYNC_PREAMBLE_MAX_BAD_PAIRS
#define OPENVLC_SFD_SYNC_PREAMBLE_MAX_BAD_PAIRS 1u
#endif

#ifndef OPENVLC_SFD_SYNC_MAX_CELL_ERRORS
#define OPENVLC_SFD_SYNC_MAX_CELL_ERRORS 1u
#endif

#ifndef OPENVLC_SFD_SYNC_MAX_CONSECUTIVE_BAD_PAIRS
#define OPENVLC_SFD_SYNC_MAX_CONSECUTIVE_BAD_PAIRS 8u
#endif

#ifndef OPENVLC_SFD_SYNC_BAD_PAIR_INVERT_FIRST
#define OPENVLC_SFD_SYNC_BAD_PAIR_INVERT_FIRST 1u
#endif

#else
#error "OPENVLC_PHY_RATE_KBPS must be 500, 1000, or 1250"
#endif

#ifndef OPENVLC_EDGE_HARD_GAP_US
#define OPENVLC_EDGE_HARD_GAP_US OPENVLC_EDGE_GAP_US
#endif
#if OPENVLC_EDGE_HARD_GAP_US < OPENVLC_EDGE_GAP_US
#error "Hard RX gap must be greater than or equal to the candidate gap"
#endif

#if OPENVLC_RX_EDGE_FILTER_MODE > OPENVLC_RX_EDGE_FILTER_CONTEXTUAL
#error "Invalid OPENVLC_RX_EDGE_FILTER_MODE"
#endif
#if OPENVLC_EDGE_HARD_GLITCH_TICKS == 0u
#error "OPENVLC_EDGE_HARD_GLITCH_TICKS must be non-zero"
#endif
#if OPENVLC_EDGE_HARD_GLITCH_TICKS > OPENVLC_COMP_MIN_HALFCELL_TICKS
#error "Hard glitch gate must not exceed the shortest valid RX cell"
#endif
#if OPENVLC_EDGE_HARD_GLITCH_TICKS > OPENVLC_EDGE_MIN_INTERVAL_TICKS
#error "Hard glitch gate must not exceed the contextual candidate gate"
#endif
#if OPENVLC_RX_EDGE_FILTER_MODE == OPENVLC_RX_EDGE_FILTER_LEGACY && \
	OPENVLC_EDGE_MIN_INTERVAL_TICKS > OPENVLC_COMP_MIN_HALFCELL_TICKS
#error "Legacy glitch gate must not exceed the shortest valid RX cell"
#endif
#if OPENVLC_COMP_MIN_HALFCELL_TICKS > OPENVLC_COMP_NOMINAL_HALFCELL_TICKS
#error "Shortest valid RX cell must not exceed the nominal RX cell"
#endif

/*
 * TX warm-up and RX acquisition must remain coherent. The warm-up puts the end
 * of SFD near cell (warm-up + 80); reserve another 48+ cells for a clipped or
 * noisy burst head. Deriving the search window below prevents changing warm-up
 * later while silently making the real SFD unreachable.
 *
 * 2026-08-06 sizing. The transmitter goes fully dark between frames: at 125 fps
 * a 5.3 ms frame leaves ~2.7 ms (34% of the period) with the LED off, during
 * which the receiver AGC ramps to maximum gain and amplifies its own noise
 * floor. The raw v2 capture corpus shows the resulting comparator chatter
 * extends to raw interval ~600-700, i.e. ~300-350 us of AGC re-settling, while
 * a 384-cell warm-up only covers 192 us. Failed frames therefore carry ~600
 * extra head edges that poison the timing estimate (t0/t1 comes out inverted,
 * 28/36 instead of 36/28) and the SFD correlation never locks - even though
 * their payload is clean and 14 of 17 such frames decode once the corrupted
 * prefix is removed. 768 cells (384 us) covers the measured settling with
 * margin.
 *
 * Growing the warm-up is NOT the fix: every warm-up cell is stored per TX slot,
 * and the static assert in openvlc_stm32_tx_hal.c caps all slots at the 96 KB
 * non-cacheable MPU window. 384 cells already fill it, and even dropping to 3
 * slots does not buy a warm-up long enough to cover the settling. The warm-up
 * would also only shorten the recovery, never prevent the AGC from ramping in
 * the first place. The actual fix is OPENVLC_TX_IDLE_KEEPALIVE below: keep the
 * LED modulating between frames so the AGC never unlocks, exactly as the
 * reference LiFi_Manchester design does with its IDLE_BYTE state.
 *
 * 2026-08-07 - TRIED 384 -> 128 AND REVERTED. The reasoning was that with the
 * keep-alive on there is no dark period left, so the warm-up re-settles an AGC
 * that never moved, and shortening it would buy inter-frame gap, burst-buffer
 * headroom and decode CPU. MEASURED: it bought NONE of them and cost 0.5 pp.
 *   - Nothing about the burst changed: br stayed 13030, lock stayed 1315,
 *     du/dm stayed 5215/5237. A burst spans gap-to-gap, i.e. a whole 8000 us
 *     frame period, so shortening the frame by 256 cells just lengthens the
 *     trailing keep-alive by 256 cells. The burst length is INVARIANT and so is
 *     everything derived from it. (Those two counters are therefore useless as
 *     a "did the new build reach the transmitter" check - use words/tx_us in
 *     the TX line of the SENDING node instead.)
 *   - It made the link worse: loss 0.93% -> 1.44%, sync 0.54% -> 0.82%, ovf
 *     0.39% -> 0.62%. Mechanism: OPENVLC_SFD_SYNC_SEARCH_CELLS below is DERIVED
 *     from this constant, so cutting the warm-up also cut the SFD search window
 *     2560 -> 2304 while the leading idle inside the burst did not shrink at
 *     all. The margin between lock=1315 and the window fell from 1245 to 989
 *     cells, so host-pacing jitter pushes the SFD outside the window sooner.
 * LESSON: the leading material in a burst is dominated by keep-alive idle, not
 * by the warm-up, so tying the SFD search window to the warm-up is wrong. If
 * the warm-up is ever reduced again, give SEARCH_CELLS its own absolute value
 * first and change one thing at a time.
 *
 * 2026-08-07 (second attempt, this time correctly): 384 -> 128. The ROUND 11
 * attempt failed only because OPENVLC_SFD_SYNC_SEARCH_CELLS was DERIVED from
 * this constant and shrank with it; that derivation has now been removed and
 * the window is a fixed 2560, so cutting the warm-up no longer narrows the SFD
 * search. Worth doing now that ROUND 14 proved channel occupancy is the real
 * constraint: 256 fewer cells = 128 us off every frame, on top of the 320 us
 * the RS reduction already returned. 128 cells still leave 64 us of settling
 * for the cold start, when the keep-alive has timed out and the LED really was
 * dark. WATCH: `lock` should fall by ~256 and `br` by ~256; if instead the
 * FIRST frames after an idle period start failing (fn climbing right after a
 * pause) the cold-start margin was cut too far - go to 256, not back to 384.
 *
 * REVERTED IMMEDIATELY: 128 cells DESTROYED the link - loss 0.4% -> 28.8%,
 * sync 931 failures in 3238 frames, `lock` reporting garbage positions (13284,
 * 13534, 13664 - far outside the 2560-cell window) and `pre` preamble rejects
 * 0 -> ~460 with pbad=4. The reasoning above ("the keep-alive removed the dark
 * period, so the warm-up is redundant") was written BEFORE the same session
 * added OPENVLC_TX_IDLE_GAP_US, which deliberately puts 10 us of DARKNESS in
 * front of every single frame. The warm-up exists precisely to cover the AGC
 * re-settling after darkness, so the gap makes it MORE necessary, not less:
 * with only 128 cells the preamble arrives before the comparator has settled,
 * the correlator rejects it or locks onto payload noise. THE TWO SETTINGS ARE
 * COUPLED - never shorten the warm-up while OPENVLC_TX_IDLE_GAP_US > 0.
 */
#ifndef OPENVLC_STM32_TX_WARMUP_CELLS
#define OPENVLC_STM32_TX_WARMUP_CELLS 384u
#endif

/*
 * Warm-up cell pattern.
 *
 * 2026-08-18, measured with a 50 MS/s logic analyser on the receive front end
 * (analog.csv, 19 inter-frame gaps): the warm-up as originally written is a
 * strict cell-rate alternation, i.e. a 1 MHz square wave, and so is the raw
 * 0xAA preamble that follows it. Together they put 224 us of PERIODIC signal
 * in front of every SFD. Over that window the received waveform is not a
 * square wave at all - an FFT puts 4.9-5.0 MHz on top, the FIFTH HARMONIC of
 * the 1 MHz fundamental, at ~40 transitions per 4 us where Manchester allows
 * 8. The payload region of the same capture is clean (0.25-1.25 MHz, perfect
 * 3.2 V edges), because random Manchester spreads its energy and never pumps
 * one harmonic coherently.
 *
 * So the warm-up is unreadable in exactly the place the SFD lands, and the
 * cause is its periodicity, not its length or the AGC.
 *
 * With this enabled the warm-up is Manchester-encoded pseudo-random data
 * instead: identical cell statistics to the payload (runs of 1 or 2 cells, so
 * every interval stays in the 32/64-tick set the receiver already accepts, and
 * the mark/space balance that holds the AGC is unchanged), but no periodicity
 * to excite the resonance.
 *
 * The generator caps identical-bit runs at 4, which caps alternating-CELL runs
 * at 8 - below OPENVLC_SFD_SYNC_PREAMBLE_CELLS (12). The preamble gate can
 * therefore never be satisfied inside the warm-up, so the warm-up cannot
 * produce a false SFD lock however the random bits fall.
 *
 * Set to 0 to restore the plain alternation for an A/B test.
 */
#ifndef OPENVLC_TX_WARMUP_PRBS
#define OPENVLC_TX_WARMUP_PRBS 1u
#endif

/* Non-zero LFSR seed. Fixed, so a capture is reproducible frame to frame. */
#ifndef OPENVLC_TX_WARMUP_PRBS_SEED
#define OPENVLC_TX_WARMUP_PRBS_SEED 0x1FBu
#endif

#if OPENVLC_TX_WARMUP_PRBS && (OPENVLC_STM32_TX_WARMUP_CELLS & 1u)
#error "PRBS warm-up emits Manchester pairs; warm-up cell count must be even"
#endif

/*
 * Inter-frame idle keep-alive. With this disabled the transmitter disconnects
 * TIM1_CH1 after every frame and the LED stays dark for ~2.7 ms out of each
 * 8 ms period, which is what lets the receiver AGC ramp to full gain and
 * chatter on its own noise floor. When enabled, TIM1 keeps free-running
 * between frames as a plain 50% square wave at the line-cell rate (ARR =
 * 2*cell_ticks-1, CCR1 = cell_ticks) with the DMA stopped: the same waveform
 * the warm-up already emits, so it carries no data and cannot be mistaken for
 * a frame, but it holds the AGC at its operating point. Costs no RAM.
 * Set to 0 to restore the previous dark-idle behaviour for an A/B test.
 *
 * 2026-08-19 SET TO 0. Measured against a transmitter confirmed at 125.0 fps
 * (wiretx=125.0), the loss splits as: 3.14% total, of which only 0.246% is the
 * decoder (35 crc + 63 sync out of 39810) and 2.90% is frames that never even
 * become a burst - seen runs at 121.4/s against 125.0 on the wire, and no
 * counter accounts for the difference (ovf 1->2, rd flat at 2, hwo flat at 1,
 * fr +0.09/s).
 *
 * The one unexplained anomaly is gd = 2222/s: 17.8 spurious gaps per 8 ms
 * frame period. A Manchester frame cannot produce them - they are all inside
 * the keep-alive, which therefore is not reaching the comparator as a clean
 * square wave but crossing back under threshold ~18 times per idle interval.
 * That injects 2200 false burst boundaries per second into the very path that
 * segments frames, which is where the 2.90% is being lost.
 *
 * Raising OPENVLC_TX_IDLE_GAP_US 10 -> 20 (a genuine inconsistency against the
 * 14 us delimiter, and still worth its guard) changed none of it: decoder
 * 0.245% -> 0.246%, ok/s 120.6 -> 121.1, gd 1950 -> 2222. So the boundary was
 * never the problem.
 *
 * Dark idle is NOT simply the old configuration: the junk prefix it reinstates
 * is what the 32-cell SFD, SEARCH_CELLS pinned to 2048 and the PRBS warm-up
 * were built to survive. Set back to 1 to restore the keep-alive.
 *
 * CONFIRMED on 8492 consecutive frames: crc 0, sync 0 - the decoder stopped
 * failing entirely (0.246% -> 0.000%), ok/s 121.1 -> 122.4, total loss
 * 3.14% -> 2.08%. The keep-alive was also injecting glitches, not just false
 * boundaries: sub-cell intervals in the 20-23 tick bin fell ~4200/s -> ~1750/s
 * and edge-ring drops 2-3 -> 0.
 *
 * Note the mechanism proposed above was WRONG in one respect: gd did not move
 * (2222/s -> 2264/s). Those gaps are the comparator chattering in whatever
 * idle exists, dark or lit, and are not a keep-alive artefact. Do not use gd
 * to reason about the keep-alive.
 *
 * The residual 2.08% is entirely pre-decoder - frames that never become a
 * burst - and no counter accounts for it. That is the next thing to chase, and
 * it is NOT in the decode path.
 */
#ifndef OPENVLC_TX_IDLE_KEEPALIVE
#define OPENVLC_TX_IDLE_KEEPALIVE 0u
#endif

/*
 * Duty cycle of the inter-frame keep-alive square wave, in percent.
 *
 * 2026-08-19. The keep-alive removes the dark period between frames, and with
 * it the comparator chatter that gives every burst a random 486-2353 cell junk
 * prefix - the measured root cause of the sync failures. But at 50% duty it
 * cost 4.5 points of frame loss, because a 50% square wave has its FIFTH
 * harmonic at MAXIMUM amplitude, and that harmonic sits at 5 MHz, exactly on
 * the resonance measured in the receive front end (analog.csv FFT: 4.85-5.06
 * MHz dominating the whole warm-up window).
 *
 * The n-th harmonic of a square wave of duty d goes as |sin(n*pi*d)|, which is
 * ZERO whenever n*d is an integer. At d = 0.40 the fifth harmonic vanishes
 * exactly, the third drops 0.106 -> 0.062, and the fundamental - the part that
 * actually holds the AGC - only loses 5% (0.318 -> 0.303).
 *
 * So the keep-alive can be kept without paying for it: same light, same rate,
 * no 5 MHz drive. 60% works identically (sin(3*pi) = 0 as well) if more mean
 * optical power is ever wanted.
 *
 * Set to 50 to restore the old waveform for an A/B.
 */
#ifndef OPENVLC_TX_IDLE_DUTY_PERCENT
#define OPENVLC_TX_IDLE_DUTY_PERCENT 40u
#endif
#if OPENVLC_TX_IDLE_DUTY_PERCENT < 10u || OPENVLC_TX_IDLE_DUTY_PERCENT > 90u
#error "OPENVLC_TX_IDLE_DUTY_PERCENT must stay between 10 and 90"
#endif
/*
 * How long the keep-alive keeps modulating after the last transmitted frame.
 * It must outlast a normal inter-frame gap (2.7 ms at 125 fps) by a wide
 * margin so the AGC never unlocks during a live stream, but it must also stop
 * once the host really has nothing to send: otherwise the LED stays lit for
 * ever, wasting power and illuminating the local photodiode while the node is
 * only receiving. 250 ms is ~90 inter-frame gaps, so any active traffic keeps
 * it running continuously, while an idle link goes dark a quarter second after
 * the last packet.
 */
#ifndef OPENVLC_TX_IDLE_KEEPALIVE_MS
#define OPENVLC_TX_IDLE_KEEPALIVE_MS 250u
#endif
/*
 * Maximum inter-frame gap still considered a live stream. The keep-alive only
 * has value between frames that belong to the same burst of traffic: it exists
 * to stop the receiver AGC unlocking before the NEXT frame. An isolated packet
 * has no such successor, so holding the LED on afterwards only makes it blink
 * and pointlessly illuminates the local photodiode. The Raspberry bridge emits
 * roughly one control frame per second even when the user is not transmitting,
 * so anything slower than this threshold must go dark immediately. At 125 fps
 * the real gap is 8 ms, leaving a wide margin; 100 ms still counts a 10 fps
 * stream as live.
 */
/*
 * Keep TIM1_CH1 connected to PE9 while a frame is being armed, instead of
 * parking the pin on its pulldown. Tested 2026-08-06 and MEASURED WORSE
 * (decode 97.0% -> 95.2%, CRC 2.55% -> 4.33%, sub-cell raw bins up 2-8x):
 * holding the channel connected also holds the seed level, so a high cell 0
 * leaves the LED lit through the whole DMA rebuild and kicks the receiver AGC
 * 125 times a second. Kept only as an A/B switch; see the rationale in
 * tx_arm_and_start().
 */
#ifndef OPENVLC_TX_KEEP_OUTPUT_ENABLED
#define OPENVLC_TX_KEEP_OUTPUT_ENABLED 0u
#endif

/*
 * Dark gap punched between the end of a frame and the start of the idle
 * keep-alive. The keep-alive holds the AGC, but it also REMOVED the only thing
 * the receiver's burst segmentation has to cut on: with the LED modulating
 * continuously there is no idle interval, so the keep-alive accumulates in the
 * same burst as the frame. That is the single root cause of BOTH remaining
 * loss terms measured in ROUND 10/11 - `ovf` (burst passes the 16384-edge
 * ceiling) and `sync` (the SFD ends up past OPENVLC_SFD_SYNC_SEARCH_CELLS),
 * together ~0.93% on a link whose crc is literally 0. Neither failure mode
 * exists in the reference LiFi_Manchester receiver because it decodes as a
 * stream and has no burst buffer and no search window at all.
 *
 * 20 us is comfortably above every profile's OPENVLC_EDGE_GAP_US (4/6/8) so
 * the receiver always segments here, and it is ~5% of the 424 us steady-state
 * inter-frame idle, far too short for the AGC to start ramping. The frame then
 * owns its burst alone: br ~12000 instead of 13030 plus variable idle, and the
 * SFD lands at the warm-up (~384) instead of 1315. The keep-alive that follows
 * forms its own bursts which carry no SFD and are rejected in ~800 us (measured
 * du on no-lock bursts), so the CPU budget stays at ~6 ms of the 8 ms period.
 *
 * MEASURED at 20 us: it works - lock 1315 -> ~500, loss 0.93% -> 0.56%, crc
 * still 0, and decode got cheaper (du/dm 5215/5237 -> 4960/4985) because the
 * burst no longer carries the idle. But 20 us of darkness is enough for the
 * AGC to move, and it then re-settles for ~300 us while the comparator
 * chatters: gpm 0 -> 84-99 permille, r07 ~1/s -> ~100k/s, and the DMA ring
 * peak jumped 9.5k -> 35k with openvlc_edge_ring_drops appearing for the first
 * time. That chatter lands in the IDLE burst so it never corrupts data, but a
 * ring drop discards whatever else is in flight, so it is not free.
 * Reduced to 10 us: still comfortably above every profile's EDGE_GAP_US (4/6/8)
 * even after main-loop poll jitter (maxpoll ~7 us pushes the real gap to
 * 10-17 us), while halving the AGC excursion. If rd still climbs, go to 8 us;
 * if bursts stop segmenting (lock back at ~1300) it went too low.
 *
 * 2026-08-19 RAISED to 20 us. The "comfortably above every profile's
 * EDGE_GAP_US (4/6/8)" above stopped being true when the deployed profile's
 * delimiter went 6 -> 14 us to stop fragmentation: the transmitter went dark
 * for 10 us and the receiver needed more than 14, so the boundary this whole
 * mechanism exists to create simply did not exist. Frames only kept decoding
 * because the keep-alive itself chatters (gd ~1900/s = ~15 spurious gaps per
 * 8 ms period) and one of those chatter gaps usually landed near the frame
 * edge by luck. The predicted symptom of "it went too low" was in the log
 * verbatim: lock sat at ~470 on lucky frames and jumped to 1181-1935 on the
 * rest, i.e. bursts that opened inside the idle instead of at the frame.
 *
 * 20 us restores a deterministic boundary: above the 14 us delimiter with
 * margin for main-loop poll jitter (maxpoll ~11 us), and still well under the
 * 22 us natural gap this profile was measured against. The AGC excursion that
 * argued for 10 us is real but lands in the IDLE burst, where it costs
 * correlator work and not frames.
 */
#ifndef OPENVLC_TX_IDLE_GAP_US
#define OPENVLC_TX_IDLE_GAP_US 20u
#endif

/*
 * The transmitter's dark window is the ONLY thing that segments a burst once
 * the keep-alive is on, so it must clear the receiver's delimiter by more than
 * the main-loop poll jitter that shortens it. OPENVLC_TX_IDLE_GAP_MEASURED_MIN_US
 * above guards the delimiter against the natural gap, which is the gap with the
 * keep-alive OFF - it does not constrain this one, and that is exactly how the
 * two drifted apart.
 */
#if defined(OPENVLC_TX_IDLE_KEEPALIVE) && OPENVLC_TX_IDLE_KEEPALIVE
#if OPENVLC_TX_IDLE_GAP_US <= (OPENVLC_EDGE_GAP_US + 4u)
#error "TX idle gap must clear the RX delimiter: bursts would never segment"
#endif
#if OPENVLC_TX_IDLE_GAP_US >= OPENVLC_TX_IDLE_GAP_MEASURED_MIN_US
#error "TX idle gap must stay under the measured natural inter-frame gap"
#endif
#endif

#ifndef OPENVLC_TX_IDLE_STREAM_GAP_MS
#define OPENVLC_TX_IDLE_STREAM_GAP_MS 100u
#endif
/*
 * 2026-08-18. Turning OPENVLC_TX_IDLE_KEEPALIVE off silently selected the
 * 512-cell branch below, because that branch was written for a link whose
 * bursts hold the frame alone. Measured with the keep-alive off: `lock` reads
 * 443-508 in every log line - pressed right against 512 - and `fn` (no SFD
 * found) grew 229 out of 234 sync failures, i.e. 7.6% of bursts. The window,
 * not the signal, is what those frames hit.
 *
 * Pinned to 2048 ahead of the conditional so it no longer moves when the
 * keep-alive is toggled. The search stops at the lock, so frames that acquire
 * at ~450 pay nothing for the larger ceiling.
 */
#define OPENVLC_SFD_SYNC_SEARCH_CELLS 2048u

#ifndef OPENVLC_SFD_SYNC_SEARCH_CELLS
#if defined(OPENVLC_TX_IDLE_KEEPALIVE) && OPENVLC_TX_IDLE_KEEPALIVE
/*
 * With the inter-frame idle keep-alive the transmitter never goes dark, so the
 * receiver no longer segments exactly on the frame boundary: a burst begins
 * somewhere inside the idle square wave and carries a variable run of idle
 * cells before the real warm-up. Measured on the first keep-alive build:
 * bursts of ~13030 edges against a ~12160-edge frame, i.e. ~870 leading idle
 * cells, which pushed the SFD (normally near cell 448) out to ~1320 and past
 * the old 512-cell window - every frame then failed with lock=0 / no-SFD even
 * though chatter had collapsed from ~11800 to ~90 sub-8-tick pulses per
 * second. The window must therefore cover the leading idle plus warm-up,
 * preamble and SFD, with margin for a burst that starts earlier in the gap.
 * Idle is the same alternating pattern as the preamble, so scanning across it
 * is harmless; it only costs correlator work.
 *
 * 2026-08-07: DECOUPLED from the warm-up, now an absolute 2560. Deriving it
 * from OPENVLC_STM32_TX_WARMUP_CELLS is a design flaw: what actually sits in
 * front of the SFD is dominated by leading keep-alive IDLE, not by the warm-up,
 * so shrinking the warm-up shrank the window in the wrong direction. That is
 * exactly what made the first warm-up reduction fail (loss 0.93% -> 1.44%,
 * margin between lock and the window fell 1245 -> 989 cells). 2560 is the value
 * the derived form produced at warm-up 384, so this changes nothing on its own.
 */
#define OPENVLC_SFD_SYNC_SEARCH_CELLS 2560u
#else
#define OPENVLC_SFD_SYNC_SEARCH_CELLS 512u
#endif
#endif
#ifndef OPENVLC_SFD_SYNC_MIN_LOCK_CELL
#define OPENVLC_SFD_SYNC_MIN_LOCK_CELL 256u
#endif

/*
 * An exact SFD remains admissible from MIN_LOCK_CELL onward.  A fuzzy SFD
 * (one or two damaged cells), however, is not credible in the first part of
 * the 384-cell TX warm-up: accepting it there can consume both bounded SFD
 * locks before the real delimiter near cell 464.  Live failures locked at
 * cells 270..346, whereas a 674-capture replay found 350 to remove those
 * candidates with one recovered frame and no baseline regressions.  The
 * slower profiles have not been validated against this packet layout rule.
 */
#ifndef OPENVLC_RX_FUZZY_SFD_MIN_LOCK_CELL
#if OPENVLC_PHY_RATE_KBPS == 1000u
#define OPENVLC_RX_FUZZY_SFD_MIN_LOCK_CELL 350u
#else
#define OPENVLC_RX_FUZZY_SFD_MIN_LOCK_CELL 0u
#endif
#endif
#if OPENVLC_RX_FUZZY_SFD_MIN_LOCK_CELL > OPENVLC_SFD_SYNC_SEARCH_CELLS
#error "Fuzzy SFD minimum lock cell must remain inside the SFD search window"
#endif
#if OPENVLC_SFD_SYNC_SEARCH_CELLS < \
	(OPENVLC_STM32_TX_WARMUP_CELLS + 80u)
#error "SFD search must cover TX warm-up, 64-cell preamble, and 16-cell SFD"
#endif

/*
 * Timing training needs more context than SFD search. A real RXFAIL capture
 * contains a clean ~1031-interval alternating run whose polarity-specific
 * medians are 37/27 ticks; using only the first 192 cells collapses that model
 * to 32/32 and loses the frame. 1536 edges retain the measured training run
 * while avoiding a redundant scan over the complete ~11800-edge payload.
 */
#ifndef OPENVLC_RX_TIMING_SEARCH_EDGES
#define OPENVLC_RX_TIMING_SEARCH_EDGES 1536u
#endif

/*
 * 2026-08-06 corpus (64 raw captures at 1550 mV with the TX idle keep-alive
 * active, so all remaining failures are payload-chatter CRC errors). Replayed
 * at the measured-best 11-tick gate, recovery saturates at TWO hypotheses:
 *
 *   budget 1 -> 37/64      budget 2 -> 45/64      budget 3/4/full -> 45/64
 *
 * Two is therefore the whole benefit at the lowest cost: a failing frame pays
 * one extra decoder pass, not five. Successful frames are unaffected because
 * the pass loop stops at the winning hypothesis, and the preferred-hypothesis
 * ordering keeps that first. This matters because a failing frame already
 * costs ~5.2 ms against an 8 ms frame period, so the runtime cadence budget in
 * openvlc_stm32_hal.c may still clamp this further under load - that throttle
 * is intentional and must stay.
 */
#ifndef OPENVLC_SFD_SYNC_HYPOTHESES_MAX
#define OPENVLC_SFD_SYNC_HYPOTHESES_MAX 6u
#endif
#ifndef OPENVLC_SFD_SYNC_LOCKS_MAX
#define OPENVLC_SFD_SYNC_LOCKS_MAX 6u
#endif
#if OPENVLC_SFD_SYNC_HYPOTHESES_MAX == 0u
#error "OPENVLC_SFD_SYNC_HYPOTHESES_MAX must be non-zero"
#endif
#if OPENVLC_SFD_SYNC_LOCKS_MAX == 0u
#error "OPENVLC_SFD_SYNC_LOCKS_MAX must be non-zero"
#endif

/*
 * Every additional SFD hypothesis scans the complete burst again. Current
 * STM32H723 journal measurements are about 4.9 ms for a valid 828-byte pass.
 * Failed/retry paths can take longer, so reserve 5.5 ms per complete pass plus
 * the deadline guard and derive the usable pass count from packet cadence.
 */
#ifndef OPENVLC_RX_DECODE_PASS_BUDGET_US
#define OPENVLC_RX_DECODE_PASS_BUDGET_US 5500u
#endif
#ifndef OPENVLC_RX_DECODE_DEADLINE_GUARD_US
#define OPENVLC_RX_DECODE_DEADLINE_GUARD_US 750u
#endif
#ifndef OPENVLC_RX_PERIOD_FILTER_SHIFT
#define OPENVLC_RX_PERIOD_FILTER_SHIFT 3u
#endif

/*
 * CRC/RS-guarded Manchester phase recovery. The normal decoder always runs
 * first. If it rejects the frame, this fallback detects sustained pair errors
 * (at least 4 in 16), inserts/removes one cell to restore pair alignment, and
 * accepts the result only after full frame validation. Set to 0 for A/B tests;
 * the remaining parameters normally need no field tuning.
 */
#ifndef OPENVLC_RX_PHASE_RECOVERY
#define OPENVLC_RX_PHASE_RECOVERY 1u
#endif

/*
 * Cached phase reconstruction. On the STM32H723, the bounded forward repair
 * pass followed by one parse is the measured realtime fast path. Deferring the
 * repair looks faster on the host replay, but costs about 1.4 ms per full frame
 * on the target and increases capture-ring pressure. The complete frame CRC/RS
 * remains the acceptance gate.
 */
#ifndef OPENVLC_RX_PRIMARY_PHASE_RECOVERY
#define OPENVLC_RX_PRIMARY_PHASE_RECOVERY 1u
#endif
#ifndef OPENVLC_RX_PHASE_REPAIR_BEFORE_DECODE
#define OPENVLC_RX_PHASE_REPAIR_BEFORE_DECODE 1u
#endif
/*
 * Track the one-cell duration independently for both comparator levels inside
 * each packet.  This is packet-local state: it is reset for every burst and
 * follows slow edge displacement without carrying a timing decision into the
 * next frame.  Keep this aligned with the host replay configuration so a
 * captured RXFAIL trace exercises the same decoder on STM32 and offline.
 */
#ifndef OPENVLC_RX_LOCAL_TIMING
#define OPENVLC_RX_LOCAL_TIMING 1u
#endif

/*
 * Jointly classify adjacent opposite-polarity intervals when their total is
 * two cells. This rejects false 43/44-tick two-cell decisions caused by
 * threshold crossing movement while adding only one look-ahead per pair.
 */
#ifndef OPENVLC_RX_PAIR_TIMING
#define OPENVLC_RX_PAIR_TIMING 1u
#endif
#ifndef OPENVLC_RX_PAIR_TIMING_MODE
#define OPENVLC_RX_PAIR_TIMING_MODE 2u
#endif

/*
 * Preserve the classic event/differential run decision in a compact one-bit
 * cell stream while the primary two-cell quantizer is already traversing the
 * edge ring.  It is parsed only after a complete primary frame fails CRC, so
 * valid and sync-failed bursts pay no second frame parse.  The 677-capture
 * regression corpus recovered nine CRC-valid frames with zero lost baseline
 * frames.  CRC/RS remains mandatory; this is not the unchecked colleague
 * decoder path.
 */
#ifndef OPENVLC_RX_DIFFERENTIAL_FALLBACK
#if OPENVLC_PHY_RATE_KBPS == 1000u
#define OPENVLC_RX_DIFFERENTIAL_FALLBACK 0u
#else
#define OPENVLC_RX_DIFFERENTIAL_FALLBACK 0u
#endif
#endif
#ifndef OPENVLC_RX_PAIR_SUM_TOL_DIV
#define OPENVLC_RX_PAIR_SUM_TOL_DIV 4u
#endif
#if OPENVLC_RX_PAIR_SUM_TOL_DIV == 0u
#error "OPENVLC_RX_PAIR_SUM_TOL_DIV must be non-zero"
#endif

/* Timer-bin tolerance around the empirically validated stretched two-cell
 * shoulder. It runs in the primary pass and scales with the PHY profile. */
#ifndef OPENVLC_COMP_SHOULDER_TOL_DIV
#define OPENVLC_COMP_SHOULDER_TOL_DIV 16u
#endif
#if OPENVLC_COMP_SHOULDER_TOL_DIV == 0u
#error "OPENVLC_COMP_SHOULDER_TOL_DIV must be non-zero"
#endif
/* Maximum CRC-guarded retries on a failed packet; zero disables the fallback. */
#ifndef OPENVLC_RX_THREE_CELL_RETRY_MAX
#define OPENVLC_RX_THREE_CELL_RETRY_MAX 0u
#endif
/*
 * On a rejected frame only, retry the single most ambiguous near-boundary
 * run-length decision with the upper cell count. Good frames pay no retry.
 * A live pass is about 170-180 us for an 828-byte packet; one CRC-gated pass
 * remains comfortably inside the measured ~8 ms packet cadence while avoiding
 * the multi-pass searches that previously caused capture-ring backlog.
 */
#ifndef OPENVLC_RX_BOUNDARY_RETRY_MAX
#if OPENVLC_PHY_RATE_KBPS == 1000u
#define OPENVLC_RX_BOUNDARY_RETRY_MAX 1u
#else
#define OPENVLC_RX_BOUNDARY_RETRY_MAX 0u
#endif
#endif
#ifndef OPENVLC_RX_BOUNDARY_RETRY_TOL_DIV
#define OPENVLC_RX_BOUNDARY_RETRY_TOL_DIV 8u
#endif
#if OPENVLC_RX_BOUNDARY_RETRY_TOL_DIV == 0u
#error "OPENVLC_RX_BOUNDARY_RETRY_TOL_DIV must be non-zero"
#endif
/* Primary hypotheses and all CRC-gated fallbacks share this total pass cap. */
#ifndef OPENVLC_RX_DECODE_PASSES_MAX
#define OPENVLC_RX_DECODE_PASSES_MAX 2u
#endif
#if OPENVLC_RX_DECODE_PASSES_MAX == 0u
#error "OPENVLC_RX_DECODE_PASSES_MAX must be non-zero"
#endif
#ifndef OPENVLC_RX_PHASE_WINDOW_PAIRS
#define OPENVLC_RX_PHASE_WINDOW_PAIRS 16u
#endif
#ifndef OPENVLC_RX_PHASE_TRIGGER_BAD_PAIRS
#define OPENVLC_RX_PHASE_TRIGGER_BAD_PAIRS 4u
#endif
#ifndef OPENVLC_RX_PHASE_TRIGGER_BAD_RUN
#define OPENVLC_RX_PHASE_TRIGGER_BAD_RUN 2u
#endif
#ifndef OPENVLC_RX_PHASE_MAX_EDITS
#define OPENVLC_RX_PHASE_MAX_EDITS 5u
#endif
/*
 * Store the already-expanded symbols of the primary pass. The ordinary
 * Manchester/frame parser consumes that stream first. Only a rejected frame
 * enters list/phase/local recovery; timing estimation, edge quantisation and
 * SFD search are never repeated. This keeps valid-frame work bounded to one
 * byte store per reconstructed line cell plus the normal parser.
 */
#ifndef OPENVLC_RX_LOCAL_SYMBOL_RECOVERY
#define OPENVLC_RX_LOCAL_SYMBOL_RECOVERY 0u
#endif
#ifndef OPENVLC_RX_LOCAL_SYMBOL_CANDIDATES
#define OPENVLC_RX_LOCAL_SYMBOL_CANDIDATES 4u
#endif
#ifndef OPENVLC_RX_LOCAL_SYMBOL_MARGIN_TICKS
#define OPENVLC_RX_LOCAL_SYMBOL_MARGIN_TICKS 4u
#endif

/*
 * Experimental CRC-gated list decoder. Offline it can recover ambiguous
 * n-1/n/n+1 run counts, but live profiling found that the CRC-valid path may
 * rank as late as 51. Replaying that many complete RS/frame parses takes
 * longer than the 1-Mbit/s packet cadence and overflows the edge-DMA ring.
 * Keep it disabled in the production profile until validation is incremental
 * or deadline-aware.
 */
#ifndef OPENVLC_RX_LIST_RECOVERY
#define OPENVLC_RX_LIST_RECOVERY 0u
#endif
#ifndef OPENVLC_RX_LIST_CANDIDATES
#define OPENVLC_RX_LIST_CANDIDATES 16u
#endif
#ifndef OPENVLC_RX_LIST_BEAM_WIDTH
#define OPENVLC_RX_LIST_BEAM_WIDTH 16u
#endif
#ifndef OPENVLC_RX_LIST_MAX_EDITS
#define OPENVLC_RX_LIST_MAX_EDITS 6u
#endif
#ifndef OPENVLC_RX_LIST_MAX_TRIALS
#define OPENVLC_RX_LIST_MAX_TRIALS 2u
#endif
#if OPENVLC_RX_LIST_CANDIDATES > 32u
#error "OPENVLC_RX_LIST_CANDIDATES exceeds the 64-bit choice map"
#endif
#if OPENVLC_RX_LIST_BEAM_WIDTH == 0u
#error "OPENVLC_RX_LIST_BEAM_WIDTH must be non-zero"
#endif
#if OPENVLC_RX_LIST_MAX_TRIALS == 0u
#error "OPENVLC_RX_LIST_MAX_TRIALS must be non-zero"
#endif

#ifndef OPENVLC_RX_DIAG_LOG
#define OPENVLC_RX_DIAG_LOG (OPENVLC_DIAGNOSTIC_LEVEL >= 1u)
#endif

/*
 * Classify consecutive TIM2 DMA intervals before the software deglitch gate.
 * Only sub-24-tick intervals need explicit counters; the >=24-tick rate is
 * derived from the already-maintained raw edge rate. This keeps deployment
 * builds at zero cost and avoids a RAM write for every normal optical edge.
 */
#ifndef OPENVLC_RX_RAW_INTERVAL_HIST
#define OPENVLC_RX_RAW_INTERVAL_HIST OPENVLC_RX_DIAG_LOG
#endif

/*
 * The run histogram and comparator-duty telemetry require a second pass over
 * every captured edge after glitch cancellation.  At 2 Mcell/s that pass is
 * diagnostic only but otherwise inspects roughly 1.4 million intervals/s and
 * can make full-duplex RX fall behind.  Sample one burst out of N and scale
 * the accumulated counters; last-burst values remain the latest real sample.
 * This does not decimate capture, slicing or decoding.
 */
#ifndef OPENVLC_COMP_RUN_DIAG_DECIMATION
#define OPENVLC_COMP_RUN_DIAG_DECIMATION 32u
#endif

#if OPENVLC_COMP_RUN_DIAG_DECIMATION == 0u
#error "OPENVLC_COMP_RUN_DIAG_DECIMATION must be > 0"
#endif

#ifndef OPENVLC_RX_RATE_LOG
#define OPENVLC_RX_RATE_LOG 0
#endif

#ifndef OPENVLC_TX_DIAG_LOG
#define OPENVLC_TX_DIAG_LOG (OPENVLC_DIAGNOSTIC_LEVEL >= 1u)
#endif

/* The register/pin monitor is intentionally separate from compact TX stats:
 * it reads a large peripheral snapshot at 10 kHz and is deep-debug only. */
#ifndef OPENVLC_TX_HW_DIAG
#define OPENVLC_TX_HW_DIAG (OPENVLC_DIAGNOSTIC_LEVEL >= 2u)
#endif

#ifndef OPENVLC_TX_DIAG_LOG_PERIOD_MS
#define OPENVLC_TX_DIAG_LOG_PERIOD_MS 1000u
#endif

#ifndef OPENVLC_TX_HOST_PREPARE_PER_POLL
#define OPENVLC_TX_HOST_PREPARE_PER_POLL 1u
#endif

#if OPENVLC_TX_DIAG_LOG_PERIOD_MS == 0u
#error "OPENVLC_TX_DIAG_LOG_PERIOD_MS must be > 0"
#endif

#if OPENVLC_TX_HOST_PREPARE_PER_POLL == 0u
#error "OPENVLC_TX_HOST_PREPARE_PER_POLL must be > 0"
#endif

#ifndef OPENVLC_RX_DIAG_LOG_PERIOD_MS
#define OPENVLC_RX_DIAG_LOG_PERIOD_MS 1000u
#endif

#if OPENVLC_RX_DIAG_LOG_PERIOD_MS == 0u
#error "OPENVLC_RX_DIAG_LOG_PERIOD_MS must be > 0"
#endif

/*
 * Keep high-rate iperf tests on the lean diagnostics path by default. The
 * COMP/RXRATE lines already expose comparator quality, ring backlog and decoder
 * runtime. Enable this only when chasing register-level setup or SFD internals:
 * it adds several formatted log lines from the RX poll loop.
 */
#ifndef OPENVLC_RX_DEEP_DEBUG_LOG
#define OPENVLC_RX_DEEP_DEBUG_LOG (OPENVLC_DIAGNOSTIC_LEVEL >= 2u)
#endif

/* Heartbeat GPIO remains active, but periodic "ALIVE" text is unnecessary on
 * a production binary and competes with host-forward records on USART3. */
#ifndef OPENVLC_ALIVE_LOG
#define OPENVLC_ALIVE_LOG (OPENVLC_DIAGNOSTIC_LEVEL >= 1u)
#endif

/*
 * A second pass over all intervals is needed only by duty/auto-threshold
 * control or an explicit deep trace. Compact health logging must remain a
 * constant-time snapshot: enabling it must not add an O(edge_count) pass to
 * the 125 fps full-duplex path.
 */
#ifndef OPENVLC_COMP_RUN_ANALYSIS
#define OPENVLC_COMP_RUN_ANALYSIS \
	(OPENVLC_RX_DEEP_DEBUG_LOG || OPENVLC_COMP_THRESHOLD_AUTO || \
	 OPENVLC_COMP_DUTY_SERVO)
#endif

#ifndef OPENVLC_DECODER_DIAGNOSTICS
#define OPENVLC_DECODER_DIAGNOSTICS (OPENVLC_DIAGNOSTIC_LEVEL >= 2u)
#endif

/*
 * Link-quality timing is telemetry, not a decoding input. Sampling one in
 * 32 post-SFD intervals still provides hundreds of measurements in an
 * 800-byte packet while avoiding thousands of 64-bit timing squares per
 * second. Manchester-pair and post-FEC quality remain exact.
 */
#ifndef OPENVLC_RX_QUALITY_DECIMATION
#define OPENVLC_RX_QUALITY_DECIMATION 32u
#endif

#if OPENVLC_RX_QUALITY_DECIMATION == 0u
#error "OPENVLC_RX_QUALITY_DECIMATION must be > 0"
#endif

/*
 * Single profile selector.
 *
 * Change only OPENVLC_PHY_RATE_KBPS above and the board derives the matching
 * RX timing profile and STM32 TX profile from it:
 *   500  -> TX budget 100, TIM1 cell = 192 ticks, ~1.0 us line cell
 *   1000 -> TX budget 50,  TIM1 cell = 96 ticks,  ~0.5 us line cell
 *   1250 -> TX budget 40,  TIM1 cell = 78 ticks,  ~0.406 us line cell
 */
#ifndef OPENVLC_STM32_TX_PROFILE_BUDGET
#if OPENVLC_PHY_RATE_KBPS == 500u
#define OPENVLC_STM32_TX_PROFILE_BUDGET 100u
#elif OPENVLC_PHY_RATE_KBPS == 1000u
#define OPENVLC_STM32_TX_PROFILE_BUDGET 50u
#elif OPENVLC_PHY_RATE_KBPS == 1250u
#define OPENVLC_STM32_TX_PROFILE_BUDGET 40u
#else
#error "OPENVLC_PHY_RATE_KBPS must be 500, 1000, or 1250"
#endif
#endif

#if (OPENVLC_PHY_RATE_KBPS == 500u) && \
	(OPENVLC_STM32_TX_PROFILE_BUDGET != 100u)
#error "OPENVLC_PHY_RATE_KBPS=500 requires OPENVLC_STM32_TX_PROFILE_BUDGET=100"
#elif (OPENVLC_PHY_RATE_KBPS == 1000u) && \
	(OPENVLC_STM32_TX_PROFILE_BUDGET != 50u)
#error "OPENVLC_PHY_RATE_KBPS=1000 requires OPENVLC_STM32_TX_PROFILE_BUDGET=50"
#elif (OPENVLC_PHY_RATE_KBPS == 1250u) && \
	(OPENVLC_STM32_TX_PROFILE_BUDGET != 40u)
#error "OPENVLC_PHY_RATE_KBPS=1250 requires OPENVLC_STM32_TX_PROFILE_BUDGET=40"
#endif

/* Pi HAT TX: TIM1_CH1 on PE9; PB5 keeps the alternate LED path off. */
#define OPENVLC_TX_GPIO_PORT GPIOE
#define OPENVLC_TX_GPIO_PIN  GPIO_PIN_9
#define OPENVLC_TX_EN_GPIO_PORT GPIOB
#define OPENVLC_TX_EN_GPIO_PIN  GPIO_PIN_5

/*
 * COMP1 reaches TIM2_CH4 through the internal TISEL mux, so driving COMP1_OUT
 * on PC5 is not required for reception and adds a continuous high-rate digital
 * aggressor beside the analog front end. Keep it off in production; set to 1
 * only when PC5 is needed as a comparator-output scope probe.
 */
#ifndef OPENVLC_COMP_DEBUG_OUTPUT
#define OPENVLC_COMP_DEBUG_OUTPUT 0u
#endif

/* HAT schematic: PA8=FLAG_1 (green heartbeat), PA9=FLAG_2 (blue fault). */
#define OPENVLC_HEARTBEAT_GPIO_PORT GPIOA
#define OPENVLC_HEARTBEAT_GPIO_PIN  GPIO_PIN_8
#define OPENVLC_FAULT_GPIO_PORT     GPIOA
#define OPENVLC_FAULT_GPIO_PIN      GPIO_PIN_9

#define OPENVLC_TX_HOST_UART_BAUD OPENVLC_HOST_UART_BAUD
#define OPENVLC_TX_HOST_RX_DMA_BYTES 8192u
#define OPENVLC_TX_PACKET_QUEUE_LEN 16u
/* USART3 receiver timeout in baud periods: 2000 / 2 Mbaud = 1 ms. */

/*
 * A zero-payload frame still contains 384 alternating warm-up cells, the
 * 64-cell preamble and at least 200 mandatory Manchester mid-bit edges.  A
 * burst below 512 captured edges therefore cannot be a complete frame.  Keep
 * these AGC-settling fragments out of both the decoder and its cadence model.
 */
#define OPENVLC_RX_MIN_DECODE_EDGES 512u

#define OPENVLC_TX_HOST_MODE 1u
#define OPENVLC_TX_DMA_IN_D2 0u
#define OPENVLC_TX_USE_TIMER_OC 1u
#define OPENVLC_TX_DMA_NONCACHEABLE 1u
#define OPENVLC_STM32_TX_SLOT_COUNT 3u
/* Request the next CCR1 preload at 1/8 cell, leaving 7/8 cell for the
 * peripheral write even under simultaneous RX/host traffic. */
#define OPENVLC_TX_OC_DMA_PHASE_DIV 8u

/*
 * Full-duplex isolation test. At 1, packet encoding, TIM1, DMA2 and completion
 * IRQ chaining all remain active, but CH1 is never
 * connected to PE9. A loss that remains in this mode is software/bus load; a
 * loss that disappears is electrical coupling from PE9/the external driver.
 * Keep 0 for normal optical transmission.
 */
#ifndef OPENVLC_TX_SILENT_OUTPUT
#define OPENVLC_TX_SILENT_OUTPUT 0u
#endif
/*
 * Link addressing. The standalone stm32-tx / BBB peer transmits src=7,dst=8;
 * the transceiver uses the MIRRORED pair so (a) the RX self-drop
 * (src == OPENVLC_TX_SRC_ADDR) rejects only our own reflected frames and never
 * the peer's src=7 traffic, and (b) a future second transceiver just swaps
 * these two values.
 */
#ifndef OPENVLC_TRANSCEIVER_NODE
#define OPENVLC_TRANSCEIVER_NODE 2u
#endif

#if OPENVLC_TRANSCEIVER_NODE == 1u
#define OPENVLC_TX_SRC_ADDR 7u
#define OPENVLC_TX_DST_ADDR 8u
#elif OPENVLC_TRANSCEIVER_NODE == 2u
#define OPENVLC_TX_SRC_ADDR 8u
#define OPENVLC_TX_DST_ADDR 7u
#else
#error "OPENVLC_TRANSCEIVER_NODE must be 1 or 2"
#endif
#ifndef OPENVLC_STM32_TX_TIMER_HZ
#define OPENVLC_STM32_TX_TIMER_HZ 192000000u
#endif
#ifndef OPENVLC_STM32_TX_GAP_CELLS
#define OPENVLC_STM32_TX_GAP_CELLS 32u
#endif

/*
 * Enforce the optical start-to-start period in hardware. The Raspberry pacer
 * smooths Linux/TUN traffic, but UART buffering can still fill several READY
 * slots at once. A fixed post-frame guard makes the rate payload-dependent;
 * an absolute period gives every packet the same 125 fps launch clock and
 * prevents 7.41 ms frame trains followed by millisecond holes.
 *
 * An 828-byte iperf payload occupies about 7.45 ms at the 1-Mbit/s profile,
 * including warm-up and the low tail. A maximum 900-byte payload can exceed
 * 8 ms after FEC expansion; in that case the scheduler starts the next frame
 * only after completion, so this value is a minimum start period rather than
 * an impossible hard deadline. Set to 0 only for a saturation test.
 *
 * 2026-08-19 LOWERED 8000 -> 7600. At 8000 this cap equalled the Raspberry
 * pacer's own rate exactly, so the drain rate could never exceed the arrival
 * rate: rho = 1.000. A finite queue in front of a server with rho = 1 and
 * bursty arrivals overflows by construction, and that is the whole residual
 * loss. Measured on the transmitter: qdrop 2.8/s and seqgap 2.8/s with
 * q = 14-16 of 16 (OPENVLC_TX_PACKET_QUEUE_LEN), while empty = 12.4/s shows the
 * same queue running dry 12 times a second - clumped arrivals, exactly the
 * "UART buffering can still fill several READY slots at once" this comment
 * already anticipated. The cap was smoothing the output but removing all
 * ability to catch up after a clump.
 *
 * A period below the source rate gives a drain ceiling above it, so the queue
 * empties between bursts instead of random-walking into the wall. Throughput
 * does NOT change - the Pi still paces at 125 fps and this is a floor, not a
 * target.
 *
 * 7600 MEASURED: it works, and it fixed the thing it was meant to fix. qdrop
 * and seqgap both froze at 288 across 2627 consecutive frames, q fell from
 * 14-16 of 16 to 0-1, and end-to-end loss went 2.08% -> 0.26% (124.7 fps
 * delivered of 125.0). But the RECEIVER got measurably worse in the same run:
 * decoder failures 0.000% (8492 frames) -> 0.285% (6660 frames), sub-cell
 * intervals in the 20-23 tick bin ~1750/s -> 2600-3400/s, jit 3.4-4.4 -> 3.7-5.1.
 * Hypothesis, not established: the pre-frame darkness now spans a much wider
 * range (idle_us 334 us while catching up, to 7 ms while idle) against a fixed
 * 384-cell warm-up sized for one condition.
 *
 * 2026-08-19 RAISED to 7800 to test that hypothesis: rho = 0.975 still leaves
 * 128.2 fps of drain against a 125 fps source - ample catch-up - while the
 * minimum inter-frame gap goes back to 536 us instead of 336, halfway to the
 * 736 us the receiver saw when it was making zero errors. If qdrop stays at 0
 * AND the decoder failures fall back toward zero, this is the optimum. If qdrop
 * resumes, 7600 was necessary and the 0.26% is the price.
 *
 * 7800 MEASURED - KEEP THIS VALUE, but the hypothesis above was WRONG.
 * qdrop = 0 and seqgap = 0 across 2378 frames from a cold boot, idle_us pinned
 * at 535 (idlemin 534). So both 7800 and 7600 fix the queue completely.
 * The decoder did NOT recover: 4 failures in 1632 frames = 0.245%, against
 * 0.285% at 7600. With only 4 events the 95% Poisson interval is 0.067%-0.627%,
 * which contains the 7600 result - the two are statistically indistinguishable
 * and the pre-frame darkness is not the mechanism.
 *
 * The residual is NOT ours to tune. Between the 8000 run (0.000%) and both
 * later runs, sub-cell intervals in the 20-23 tick bin went ~1750/s ->
 * 2600-3400/s while nothing in the scheduler touches the analog front end.
 * That is a signal-quality change - time of day, position, sunlight - and the
 * 8000 comparison is therefore not a controlled one. Do not chase it with more
 * scheduler constants.
 *
 * 7800 is preferred over 7600 on margin alone: identical measured outcome, but
 * a 536 us inter-frame gap instead of 336, and closer to the original design.
 * The residual failures are all sync/fn (SFD not found in the burst) with crc
 * at zero - the one failure class that burst segmentation creates and a
 * streaming per-pulse decoder does not have.
 *
 * Floor check: airtime is 7264 us for an 828-byte frame (14530 cells), so 7800
 * leaves a 536 us inter-frame gap and 7600 leaves 336 us - both far above
 * OPENVLC_EDGE_GAP_US, so the receiver segments either way. Do not go below
 * ~7400: at 136 us the gap starts approaching the delimiter plus main-loop
 * poll jitter.
 *
 * NOT the warm-up: airtime occupies only 90.8% of the 8000 us period, so the
 * wire was never the constraint and shortening OPENVLC_STM32_TX_WARMUP_CELLS
 * would have bought 64 us against a problem that is not about microseconds.
 * It would also be actively unsafe now - see the REVERTED note there: cutting
 * the warm-up broke the link when only 10 us of darkness preceded each frame,
 * and with the keep-alive off that darkness is now 735 us.
 */
#ifndef OPENVLC_STM32_TX_TARGET_PERIOD_US
#define OPENVLC_STM32_TX_TARGET_PERIOD_US 7800u
#endif

#if OPENVLC_STM32_TX_TARGET_PERIOD_US != 0u && 	OPENVLC_STM32_TX_TARGET_PERIOD_US < 7400u
#error "TX target period too short: inter-frame gap approaches the RX delimiter"
#endif

#if OPENVLC_STM32_TX_TARGET_PERIOD_US > 1000000u
#error "OPENVLC_STM32_TX_TARGET_PERIOD_US must not exceed one second"
#endif

/* Optional extra minimum idle after DMA completion. The absolute period above
 * already supplies the production scheduling margin. */
#ifndef OPENVLC_STM32_TX_INTERFRAME_GUARD_US
#define OPENVLC_STM32_TX_INTERFRAME_GUARD_US 0u
#endif

#if OPENVLC_STM32_TX_INTERFRAME_GUARD_US > 1000000u
#error "OPENVLC_STM32_TX_INTERFRAME_GUARD_US must not exceed one second"
#endif

#ifndef OPENVLC_STM32_TX_BUDGET40_CELL_TICKS
#define OPENVLC_STM32_TX_BUDGET40_CELL_TICKS 78u
#endif
#ifndef OPENVLC_STM32_TX_BUDGET50_CELL_TICKS
#define OPENVLC_STM32_TX_BUDGET50_CELL_TICKS 96u
#endif
#ifndef OPENVLC_STM32_TX_BUDGET100_CELL_TICKS
#define OPENVLC_STM32_TX_BUDGET100_CELL_TICKS 192u
#endif

#ifndef OPENVLC_STM32_TX_CELL_TICKS
#if OPENVLC_STM32_TX_PROFILE_BUDGET == 100u
#define OPENVLC_STM32_TX_CELL_TICKS OPENVLC_STM32_TX_BUDGET100_CELL_TICKS
#elif OPENVLC_STM32_TX_PROFILE_BUDGET == 50u
#define OPENVLC_STM32_TX_CELL_TICKS OPENVLC_STM32_TX_BUDGET50_CELL_TICKS
#elif OPENVLC_STM32_TX_PROFILE_BUDGET == 40u
#define OPENVLC_STM32_TX_CELL_TICKS OPENVLC_STM32_TX_BUDGET40_CELL_TICKS
#else
#error "OPENVLC_STM32_TX_PROFILE_BUDGET must be 40, 50, or 100"
#endif
#endif

/*
 * Clock-domain invariants. TIM1 and TIM2 share the 192-MHz APB timer kernel;
 * these checks prevent a profile edit from silently making TX and RX disagree
 * about the physical line-cell duration.
 */
#if (OPENVLC_STM32_TX_TIMER_HZ % OPENVLC_TIM2_IC_TICK_HZ) != 0u
#error "TX timer clock must be an integer multiple of the TIM2 capture clock"
#endif
#if (OPENVLC_STM32_TX_TIMER_HZ / OPENVLC_TIM2_IC_TICK_HZ) != \
	(OPENVLC_TIM2_IC_PRESCALER + 1u)
#error "TIM2 prescaler does not match the configured timer/capture clocks"
#endif
#if OPENVLC_STM32_TX_CELL_TICKS != \
	(OPENVLC_COMP_NOMINAL_HALFCELL_TICKS * \
	 (OPENVLC_TIM2_IC_PRESCALER + 1u))
#error "TX cell duration and nominal RX cell duration are inconsistent"
#endif
#if OPENVLC_TX_OC_DMA_PHASE_DIV == 0u
#error "OPENVLC_TX_OC_DMA_PHASE_DIV must be non-zero"
#endif
#if (OPENVLC_STM32_TX_CELL_TICKS / OPENVLC_TX_OC_DMA_PHASE_DIV) == 0u
#error "TX DMA preload point must fall inside the line cell"
#endif

#endif
