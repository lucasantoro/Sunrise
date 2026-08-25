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
 * 2 Mbaud. 4 Mbaud produces no output on this board's ST-LINK VCP.
 *
 * Host UART TX uses normal-mode DMA, so the main loop is neither stalled nor
 * preempted per byte - a blocking send, or a TXE interrupt per byte, wraps the
 * ring at this rate.
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

#ifndef OPENVLC_COMP_THRESHOLD_MV
#define OPENVLC_COMP_THRESHOLD_MV 2100u
#endif
#ifndef OPENVLC_COMP_THRESHOLD_DAC
#define OPENVLC_COMP_THRESHOLD_DAC OPENVLC_MV_TO_DAC(OPENVLC_COMP_THRESHOLD_MV)
#endif

/*
 * Comparator hysteresis level.
 *
 * The first defence against chatter when the signal crosses the threshold slowly:
 * without it one crossing produces a burst of edges instead of one. Mapped to the
 * HAL value in Core/Src/comp.c; kept numeric here so it can be logged.
 *
 * More is not better. Hysteresis widens the window in which a real transition is
 * ignored, so it trades chatter against lost transitions - and a lost transition
 * is unrecoverable where chatter is filterable.
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
 * OFF for production and measurement runs. Capture costs real link quality:
 * sync failures and burst fragmentation both rise sharply with it enabled.
 * Turn it on ONLY to collect a corpus, and never quote performance numbers
 * from a capture-enabled run.
 *
 * KNOWN DEFECT, unfixed: the Pi-side parser (raspberry-gateway/vlc_capture.py)
 * expects each record to start with the magic "OVCT" and an 88-byte BEGIN
 * header, and this emitter does not produce that. Every record is rejected
 * with "invalid capture record prefix". Fix the format before relying on an
 * on-target capture run - the captures that do work today come from the
 * bridge, not from here.
 */
#define OPENVLC_RX_CAPTURE 0u
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
 * Build a balanced offline corpus, keeping a quota of each failure class
 * rather than whatever occurs first. These are quotas, not buffers: the same 28 KB snapshot is
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
 * Sweep the comparator threshold across a range, logging what each step
 * delivers.
 *
 * This is the measurement that has to come before OPENVLC_COMP_DUTY_SERVO is
 * turned on: the servo assumes the duty moves monotonically with the DAC code,
 * and only a sweep on the actual board establishes that. The COMP SWEEP log line
 * reports the threshold, the delivered frames and the resulting duty at each
 * step.
 *
 * The bounds are DAC codes, not millivolts.
 */
#ifndef OPENVLC_COMP_THRESHOLD_SWEEP
#define OPENVLC_COMP_THRESHOLD_SWEEP 0
#endif
/*
 * Bounds are DAC codes, not millivolts: 1500..3300 spans 1209..2659 mV, which
 * brackets the operating point of both receiver front ends. A range that does
 * not contain the board's actual threshold makes the sweep useless.
 *
 * Step 50 and a 3 s dwell give 36 points in under two minutes, because the TX
 * on this bench cannot stay lit for long. Narrow the step again once the duty
 * minimum is bracketed.
 */
#ifndef OPENVLC_COMP_SWEEP_MIN
#define OPENVLC_COMP_SWEEP_MIN 1500u
#endif
#ifndef OPENVLC_COMP_SWEEP_MAX
#define OPENVLC_COMP_SWEEP_MAX 3300u
#endif
#ifndef OPENVLC_COMP_SWEEP_STEP
#define OPENVLC_COMP_SWEEP_STEP 50u
#endif
#ifndef OPENVLC_COMP_SWEEP_DWELL_S
#define OPENVLC_COMP_SWEEP_DWELL_S 3u
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
 * Width below which an adjacent pair of edges is treated as comparator chatter
 * and cancelled.
 *
 * Cancelled in pairs so the Manchester level parity is preserved: removing one
 * edge would invert every level after it. This is a candidate aperture, not a
 * hard gate - OPENVLC_EDGE_HARD_GLITCH_TICKS below is the unconditional one.
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
 * Hardware input-capture filter on the comparator output, in TIM2 filter units.
 *
 * Rejects pulses shorter than a few timer clocks before they reach the DMA ring,
 * so chatter never costs ring space or software time. The software gates above
 * handle what survives it.
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
 * Keep the RX burst delimiter well below the TX low tail, and above the
 * longest interval that legitimately occurs inside a frame.
 *
 * Too long and consecutive frames join into one burst: the edge count then
 * exceeds the packet buffer, decodes take tens of milliseconds and the
 * capture ring starts dropping. Too short and a frame is cut in two.
 */
/* Shortest inter-frame gap measured on the wire (50 MS/s, 19 boundaries). */
#ifndef OPENVLC_TX_IDLE_GAP_MEASURED_MIN_US
#define OPENVLC_TX_IDLE_GAP_MEASURED_MIN_US 22u
#endif

/*
 * A run with no edges for at least this long ends a burst.
 *
 * This is how frame boundaries are recovered without decoding anything: the
 * transmitter's inter-frame dark gap exceeds it, and nothing inside a frame
 * should. A dead stretch inside a frame - signal present but not crossing the
 * threshold - is therefore read as a boundary and cuts the frame in two, which
 * the fr and fg counters report.
 *
 * Must stay below OPENVLC_TX_IDLE_GAP_MEASURED_MIN_US with margin.
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
 * Cells of preamble that must precede a candidate SFD for the match to be
 * accepted.
 *
 * The SFD is found by correlating against the recovered cell stream rather than
 * by matching bytes, because the byte boundary is not known when the search
 * starts. Requiring a preamble run in front of it is what stops noise that
 * happens to correlate from being taken for a frame start.
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
 * correlator accept this bounded distance. Do not widen it: at distance 3 the
 * correlator matches comparator fragments faster than frames. Keep the last
 * independently validated distance here.
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
 * Cells of modulation sent before the preamble of every frame.
 *
 * The receiver's AGC ramps toward maximum gain during the dark gap between
 * frames. A frame arriving into a fully-ramped front end has its opening cells
 * amplified along with the noise floor, and the preamble is lost. The warm-up
 * gives the AGC something with no information in it to settle on, so the
 * preamble reaches a settled receiver.
 *
 * OPENVLC_TX_WARMUP_PRBS makes those cells pseudo-random rather than a constant
 * pattern, so the AGC sees realistic transition density.
 *
 * 384 cells is 384 us, about 5% of the frame period. Acquisition degrades
 * sharply rather than gracefully when this is shortened, so treat it as a
 * measured value: re-measure before changing it, and check the decoder failure
 * rate rather than the throughput, which hides the effect.
 */
#ifndef OPENVLC_STM32_TX_WARMUP_CELLS
#define OPENVLC_STM32_TX_WARMUP_CELLS 384u
#endif

/*
 * Fill the warm-up with a pseudo-random sequence instead of a constant pattern.
 *
 * A constant pattern presents the AGC with one transition density; real payload
 * data presents another. The PRBS makes the settling condition resemble what
 * follows it.
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
 * Keep the transmitter modulating between frames instead of going dark.
 *
 * Off. A continuously lit line removes the AGC ramp that the warm-up exists to
 * absorb, but it also removes the dark gap the receiver uses to segment bursts,
 * which is the more important of the two.
 */
#ifndef OPENVLC_TX_IDLE_KEEPALIVE
#define OPENVLC_TX_IDLE_KEEPALIVE 0u
#endif

/*
 * Duty cycle of the idle keep-alive pattern, when it is enabled.
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
 * parking the pin on its pulldown. Off: holding the channel connected also
 * holds the seed level, so a high cell 0 leaves the LED lit through the whole
 * DMA rebuild and steps the receiver AGC 125 times a second - more costly than
 * the electrical runt it avoids. Kept as an A/B switch; see the rationale in
 * tx_arm_and_start().
 */
#ifndef OPENVLC_TX_KEEP_OUTPUT_ENABLED
#define OPENVLC_TX_KEEP_OUTPUT_ENABLED 0u
#endif

/*
 * Minimum dark period the transmitter leaves between frames.
 *
 * The receiver finds frame boundaries by looking for a run with no edges longer
 * than OPENVLC_EDGE_GAP_US. This value is the transmitter's side of that
 * contract and must stay clear of it, with room for main-loop poll jitter on top
 * - the build-time check below enforces the ordering.
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
 * How far into a burst the SFD search looks before giving up.
 *
 * Derived from the warm-up rather than set independently: the warm-up puts the
 * end of the SFD near cell (warm-up + 80), and the window has to reach past that
 * with room for a clipped or noisy burst head. Deriving it prevents the warm-up
 * being changed later while silently putting the real SFD out of reach.
 */
#define OPENVLC_SFD_SYNC_SEARCH_CELLS 2048u

#ifndef OPENVLC_SFD_SYNC_SEARCH_CELLS
#if defined(OPENVLC_TX_IDLE_KEEPALIVE) && OPENVLC_TX_IDLE_KEEPALIVE
/*
 * How far into a burst the SFD search looks.
 *
 * An absolute value, deliberately NOT derived from the warm-up. What sits in
 * front of the SFD is dominated by whatever leading idle the burst started
 * inside, not by the warm-up, so deriving it would shrink the window in the
 * wrong direction whenever the warm-up was reduced.
 *
 * The window must cover leading idle plus warm-up, preamble and SFD, with
 * margin for a burst that begins early in the gap. Idle carries the same
 * alternating pattern as the preamble, so scanning across it is harmless and
 * costs only correlator work.
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
 * Alternative cell alignments tried before an acquisition is abandoned.
 *
 * Each hypothesis costs decode time, and decode time is bounded by the frame
 * period - watch du against 8000 us when raising this.
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
 * remains comfortably inside the ~8 ms packet cadence. Multi-pass searches do
 * not: they back the capture ring up.
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
#define OPENVLC_TRANSCEIVER_NODE 1u
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
 * Minimum optical start-to-start period, enforced on the STM32 rather than
 * trusted from the host.
 *
 * An absolute period rather than a post-frame guard: a guard makes the rate
 * payload-dependent, while a fixed launch clock gives every frame the same
 * 125 fps cadence instead of frame trains followed by millisecond holes.
 *
 * It must sit BELOW the host's own 8000 us pacing. Equal periods put the
 * queue at rho = 1, where a finite buffer in front of bursty arrivals
 * overflows by construction and qdrop climbs; the headroom is what lets the
 * transmitter catch up after a clump. Throughput is unchanged either way -
 * the Pi still offers 125 fps and this is a floor, not a target.
 *
 * At the maximum 900-byte payload the frame can exceed 8 ms after FEC, and
 * the scheduler then starts the next frame on completion: this is a minimum
 * period, not a deadline it can miss. Set to 0 only for a saturation test.
 *
 * Airtime for an 828-byte frame is 7264 us, so 7800 leaves a 536 us
 * inter-frame gap - comfortably above OPENVLC_EDGE_GAP_US, which the receiver
 * needs to segment bursts. The guard below rejects values that would close
 * that gap toward the delimiter plus main-loop poll jitter.
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
