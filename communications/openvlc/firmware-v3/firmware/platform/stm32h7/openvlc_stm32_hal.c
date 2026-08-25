#include "openvlc_stm32_hal.h"
#include "openvlc_stm32_tx_hal.h"
#include "openvlc_phy.h"
#include "openvlc_crc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#if __has_include("openvlc_board.h")
#include "openvlc_board.h"
#endif
#endif

#if defined(STM32H743xx) || defined(STM32H723xx)
#include "stm32h7xx_hal.h"
#include "stm32h7xx_it.h"
extern UART_HandleTypeDef huart3;
extern TIM_HandleTypeDef htim2;
extern COMP_HandleTypeDef hcomp1;
extern DAC_HandleTypeDef hdac1;
extern void Error_Handler(void);
#endif

extern volatile uint32_t openvlc_rx_hypothesis_budget;
extern volatile uint32_t openvlc_phy_dbg_stage;
extern volatile uint32_t openvlc_phy_dbg_sample_len;
extern volatile uint32_t openvlc_phy_dbg_sps;
extern volatile uint32_t openvlc_phy_dbg_len_raw;
extern volatile uint32_t openvlc_phy_dbg_payload_len;
extern volatile int32_t openvlc_phy_dbg_parse_status;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_single;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_split;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_cell0;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_cell1;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_train;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_syncs;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_maxbits;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_lenraw;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_result;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_pre_rejects;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_pre_badmax;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_pre_sfdmin;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_sfd_errors;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_lock_cell;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_relocks;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_mode;
extern volatile uint32_t openvlc_phy_dbg_phase_edits;
extern volatile uint32_t openvlc_phy_dbg_track_cell0_end;
extern volatile uint32_t openvlc_phy_dbg_track_cell1_end;
extern volatile uint32_t openvlc_phy_dbg_track_nominal_end;
extern volatile uint32_t openvlc_phy_dbg_timing_residual_peak;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_fail_timing;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_fail_no_sfd;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_fail_preamble;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_fail_parse;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_fail_crc;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_fail_len;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_fail_overflow;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_fail_incomplete;

#if defined(__GNUC__)
#define OPENVLC_DMA_BUFFER __attribute__((section(".dma_buffer"), aligned(32)))
#define OPENVLC_RX_BUFFER __attribute__((section(".rx_buffer"), aligned(32)))
#define OPENVLC_CPU_BUFFER __attribute__((section(".cpu_buffer"), aligned(32)))
/* First 128 KB of RAM_D1, covered by the non-cacheable MPU region (main.c). */
#define OPENVLC_DMA_RING __attribute__((section(".dma_ring"), aligned(32)))
#define OPENVLC_RX_HOT __attribute__((hot, optimize("O3")))
#define OPENVLC_RX_INLINE static inline __attribute__((always_inline))
#else
#define OPENVLC_DMA_BUFFER
#define OPENVLC_RX_BUFFER
#define OPENVLC_CPU_BUFFER
#define OPENVLC_DMA_RING
#define OPENVLC_RX_HOT
#define OPENVLC_RX_INLINE static inline
#endif

static uint16_t dma_selftest_word OPENVLC_DMA_BUFFER;
static uint16_t rx_accum_buffer[OPENVLC_RX_SAMPLE_BUFFER_LEN] OPENVLC_RX_BUFFER;
volatile uint32_t openvlc_stm32_start_step;
volatile uint32_t openvlc_mem_test_step;
volatile uint32_t openvlc_mem_test_result;
volatile uint32_t openvlc_comp_start_step;
volatile uint32_t openvlc_comp_dma_cr;
volatile uint32_t openvlc_comp_dma_fcr;
volatile uint32_t openvlc_comp_dma_ndtr;
volatile uint32_t openvlc_comp_poll_step;
volatile uint32_t openvlc_comp_poll_head;
volatile uint32_t openvlc_comp_poll_tail;
volatile uint32_t openvlc_comp_poll_burst_len;
volatile uint32_t openvlc_comp_level_samples;
volatile uint32_t openvlc_comp_level_changes;
volatile uint32_t openvlc_comp_test_zero_high;
volatile uint32_t openvlc_comp_test_full_high;
volatile uint32_t openvlc_comp_test_zero_dor;
volatile uint32_t openvlc_comp_test_full_dor;
static uint32_t openvlc_comp_last_level;
static bool openvlc_comp_level_valid;

int openvlc_stm32_memory_selftest(void)
{
	volatile uint16_t *dma = &dma_selftest_word;
	volatile uint16_t *rx = rx_accum_buffer;

	openvlc_mem_test_step = 1u;
	dma[0] = 0x0123u;
	openvlc_mem_test_step = 2u;
	rx[0] = 0x0456u;
	openvlc_mem_test_step = 3u;
	openvlc_mem_test_result = ((uint32_t)dma[0] << 16) | rx[0];
	openvlc_mem_test_step = 4u;
	return openvlc_mem_test_result == 0x01230456u ? 0 : -1;
}

/*
 * Comparator + TIM2 CH4 input-capture receiver.
 *
 * COMP1 slices PB0 vs DAC1_CH1 (hysteresis). Its output is routed internally to
 * TIM2_TI4; TIM2 timestamps both edges via DMA at the rate selected by
 * OPENVLC_TIM2_IC_TICK_HZ (16 MHz for the validated budget-100 profile).
 * Edges are grouped into bursts (split on idle gaps) and decoded by
 * openvlc_app_rx_edges().
 */
/*
 * 32768 edges x 4 B = exactly 128 KB: the ring fills the dedicated .dma_ring
 * section (first 128 KB of RAM_D1) which MPU region 1 marks NON-CACHEABLE so
 * the TIM2 DMA writes stay coherent with the D-cache-enabled CPU (see
 * main.c). 32768 edges = ~22 ms of backlog at the 1.5 M edge/s budget-50
 * flood; with D-cache the per-frame decode leaves several ms of slack per
 * 7.46 ms frame period, so the backlog stays small (watch
 * openvlc_edge_ring_peak). Constraints if you change it: power-of-two MPU
 * region size, 16-bit DMA NDTR (<= 65535 entries), and the board's real
 * 320 KB AXI SRAM (STM32H723, not H743).
 */
#ifndef OPENVLC_STREAM_POLL_CHUNK
/* Edges handed to the decoder per call. Small enough to stay on the stack and
 * to keep latency near one chunk, large enough that the call overhead is
 * negligible against ~1.4M edges/s. */
#define OPENVLC_STREAM_POLL_CHUNK 64u
#endif

#ifndef OPENVLC_EDGE_DMA_LEN
#define OPENVLC_EDGE_DMA_LEN 40960u
#endif
#ifndef OPENVLC_EDGE_BURST_LEN
#define OPENVLC_EDGE_BURST_LEN 32768u
#endif
/* TIM2 input-capture tick rate selected by the active board timing profile. */
#ifndef OPENVLC_TIM2_IC_TICK_HZ
#define OPENVLC_TIM2_IC_TICK_HZ 64000000u
#endif
/*
 * Idle longer than this ends a packet burst. The board timing profile sets
 * this from the Manchester cell duration. It must be shorter than the BBB
 * inter-frame service gap; otherwise queued frames merge and the single-frame
 * decoder returns only the first packet in the combined burst.
 */
#ifndef OPENVLC_EDGE_GAP_US
#define OPENVLC_EDGE_GAP_US 8u
#endif
#ifndef OPENVLC_EDGE_HARD_GAP_US
#define OPENVLC_EDGE_HARD_GAP_US OPENVLC_EDGE_GAP_US
#endif
#define OPENVLC_EDGE_GAP_TICKS ((OPENVLC_TIM2_IC_TICK_HZ / 1000000u) * OPENVLC_EDGE_GAP_US)
#define OPENVLC_EDGE_HARD_GAP_TICKS \
	((OPENVLC_TIM2_IC_TICK_HZ / 1000000u) * OPENVLC_EDGE_HARD_GAP_US)
#ifndef OPENVLC_EDGE_MIN_INTERVAL_TICKS
#define OPENVLC_EDGE_MIN_INTERVAL_TICKS 14u
#endif
#ifndef OPENVLC_RX_EDGE_FILTER_NONE
#define OPENVLC_RX_EDGE_FILTER_NONE       0u
#endif
#ifndef OPENVLC_RX_EDGE_FILTER_LEGACY
#define OPENVLC_RX_EDGE_FILTER_LEGACY     1u
#endif
#ifndef OPENVLC_RX_EDGE_FILTER_CONTEXTUAL
#define OPENVLC_RX_EDGE_FILTER_CONTEXTUAL 2u
#endif
#ifndef OPENVLC_RX_EDGE_FILTER_MODE
#define OPENVLC_RX_EDGE_FILTER_MODE OPENVLC_RX_EDGE_FILTER_LEGACY
#endif
#ifndef OPENVLC_EDGE_HARD_GLITCH_TICKS
#define OPENVLC_EDGE_HARD_GLITCH_TICKS \
	(OPENVLC_EDGE_MIN_INTERVAL_TICKS / 3u)
#endif
#ifndef OPENVLC_EDGE_CONTEXT_MARGIN_TICKS
#define OPENVLC_EDGE_CONTEXT_MARGIN_TICKS 2u
#endif
#ifndef OPENVLC_RX_CAPTURE_RAW
#define OPENVLC_RX_CAPTURE_RAW 0u
#endif
#ifndef OPENVLC_RX_CAPTURE_MAX_INTERVALS
#define OPENVLC_RX_CAPTURE_MAX_INTERVALS (OPENVLC_EDGE_BURST_LEN - 1u)
#endif
#ifndef OPENVLC_RX_CAPTURE_CHUNK_INTERVALS
#define OPENVLC_RX_CAPTURE_CHUNK_INTERVALS 384u
#endif
#if OPENVLC_RX_CAPTURE_CHUNK_INTERVALS > 442u
#error "OPENVLC_RX_CAPTURE_CHUNK_INTERVALS exceeds a 900-byte host record"
#endif

/*
 * Fragment-size window used only for the FRAG diagnostic counters: a burst
 * ending in this range is bigger than noise but smaller than a whole packet.
 */
#ifndef OPENVLC_FRAG_MIN_EDGES
#define OPENVLC_FRAG_MIN_EDGES 100u
#endif
#ifndef OPENVLC_FRAG_MAX_EDGES
#define OPENVLC_FRAG_MAX_EDGES 11000u
#endif

/*
 * The TIM2 edge-capture ring is the only DMA-written RAM buffer. It lives in
 * the .dma_ring section = exactly the first 128 KB of RAM_D1 (AXI SRAM, which
 * DMA1 can reach; RAM_D2 on this H723 is only 32 KB). MPU region 1 marks that
 * 128 KB non-cacheable so the ring stays coherent with the D-cache-enabled
 * CPU; all the CPU-only buffers below are cacheable .rx_buffer.
 */
static uint32_t edge_dma_buf[OPENVLC_EDGE_DMA_LEN] OPENVLC_DMA_RING;
static uint32_t edge_burst[OPENVLC_EDGE_BURST_LEN] OPENVLC_CPU_BUFFER;
#if OPENVLC_RX_CAPTURE
/*
 * Adjacent intervals are always shorter than the burst delimiter (hundreds
 * of timer ticks), so uint16_t is exact and keeps the reusable snapshot to
 * 32 KB. This buffer is CPU-only and therefore belongs in DTCM.
 */
static uint16_t rx_capture_intervals[OPENVLC_RX_CAPTURE_MAX_INTERVALS]
	OPENVLC_CPU_BUFFER;
typedef enum {
	RX_CAPTURE_ARMED = 0,
	RX_CAPTURE_BEGIN,
	RX_CAPTURE_DATA,
	RX_CAPTURE_END,
	RX_CAPTURE_DONE,
} rx_capture_state_t;
static volatile rx_capture_state_t rx_capture_state = RX_CAPTURE_ARMED;
static uint32_t rx_capture_count;
static uint32_t rx_capture_position;
static uint32_t rx_capture_clipped;
static uint32_t rx_capture_id;
static int32_t rx_capture_status;
static int32_t rx_capture_parse_status;
static uint32_t rx_capture_t0;
static uint32_t rx_capture_t1;
static uint32_t rx_capture_nominal;
static uint32_t rx_capture_residual_q8;
static uint32_t rx_capture_syncs;
static uint32_t rx_capture_mode;
static uint32_t rx_capture_hypothesis_budget;
static uint32_t rx_capture_lock;
static uint32_t rx_capture_len_raw;
static uint32_t rx_capture_warmup_ok;
static uint32_t rx_capture_frame_gap;
static uint32_t rx_capture_failures;
static uint32_t rx_capture_successes;
static uint32_t rx_capture_threshold_dac;
static uint32_t rx_capture_snapshot_ms;
static uint32_t rx_capture_hash;
static uint8_t rx_capture_trigger;
static bool rx_capture_qualified;
static bool rx_capture_prepared;
#endif
static uint32_t edge_dma_tail;
static uint32_t edge_burst_len;
static uint32_t edge_burst_raw_len;
static uint32_t edge_last_tick;
static bool edge_have_last;
static bool edge_burst_overflowed;
static bool edge_capture_running;
static bool edge_sync_to_gap;
static bool edge_soft_gap_checked;
static uint32_t edge_soft_gap_len;
static openvlc_status_t edge_soft_gap_status;
static uint32_t edge_previous_burst_start;
static uint32_t edge_filtered_period_ticks;
static bool edge_period_valid;
volatile uint32_t openvlc_edge_bursts_decoded;
volatile uint32_t openvlc_edge_total;
volatile uint32_t openvlc_edge_overflows;
/* Inter-frame gaps the delimiter actually saw, counted BEFORE any burst is
 * formed or disposed of. bp/sp/okp all count bursts that reached decode, so
 * they cannot distinguish "the edges never arrived" from "they arrived and
 * something dropped them". This can: at 125 fps on the wire it must read 125. */
volatile uint32_t openvlc_edge_gaps_seen;
#if defined(OPENVLC_RX_STREAMING) && OPENVLC_RX_STREAMING
extern uint32_t openvlc_stream_shadow_seen;
extern uint32_t openvlc_stream_shadow_ok;
extern uint32_t openvlc_stream_shadow_mismatch;
extern uint32_t openvlc_stream_dbg_absorbed;
extern uint32_t openvlc_stream_dbg_toolong;
extern uint32_t openvlc_stream_dbg_skipped;
extern uint32_t openvlc_stream_dbg_rearm;
extern uint32_t openvlc_stream_dbg_desync;
#if OPENVLC_RX_STREAMING >= 4
/* ref=<anomalie>/<acquisizioni>/<re-anchor della fase di appaiamento> */
extern uint32_t openvlc_ref_dbg_anomaly;
extern uint32_t openvlc_ref_dbg_acquired;
extern uint32_t openvlc_ref_dbg_reanchor;
#endif
void openvlc_stream_rx_debug(uint32_t *sfd_hits, uint32_t *len_ok,
			     uint32_t *len_bad, int32_t *last_parse);

/*
 * seen counts SFD hits and crc reads a measured zero, so every residual failure
 * dies between finding the SFD and completing a frame. len_valid/len_invalid
 * split that interval in two and the decoder has tracked them all along - they
 * were simply never put on the wire, the same omission that hid sa/sl/sk.
 */
static uint32_t openvlc_stream_len_ok_snapshot(void)
{
	uint32_t v = 0u;

	openvlc_stream_rx_debug(NULL, &v, NULL, NULL);
	return v;
}

static uint32_t openvlc_stream_len_bad_snapshot(void)
{
	uint32_t v = 0u;

	openvlc_stream_rx_debug(NULL, NULL, &v, NULL);
	return v;
}
#endif
volatile uint32_t openvlc_edge_hw_overcaptures;
volatile uint32_t openvlc_edge_max_burst;
volatile uint32_t openvlc_edge_glitches;
#if OPENVLC_RX_RAW_INTERVAL_HIST
/* Pre-deglitch consecutive-edge bins: 0..7, 8..11, 12..15, 16..19, 20..23. */
static volatile uint32_t openvlc_edge_raw_short_hist[5];
#endif
volatile uint32_t openvlc_edge_capture_restarts;
volatile uint32_t openvlc_edge_partial_drops;
volatile uint32_t openvlc_edge_short_bursts;
volatile uint32_t openvlc_edge_run1_total;
volatile uint32_t openvlc_edge_run2_total;
volatile uint32_t openvlc_edge_run3_total;
volatile uint32_t openvlc_edge_long_total;
volatile uint32_t openvlc_edge_long_max_ticks;
volatile uint32_t openvlc_edge_last_raw_burst_len;
volatile uint32_t openvlc_edge_last_burst_len;
volatile uint32_t openvlc_edge_last_ok_burst_len;
volatile uint32_t openvlc_edge_last_fail_burst_len;
volatile uint32_t openvlc_edge_last_short_burst_len;
volatile int32_t openvlc_edge_last_decode_status = 99;
volatile uint32_t openvlc_decode_last_ticks;
volatile uint32_t openvlc_decode_max_ticks;
volatile uint32_t openvlc_rx_last_decode_budget;
/*
 * Comparator output duty accumulators, for the self-centering threshold servo.
 * A DC-balanced Manchester waveform makes the comparator spend equal time HIGH
 * and LOW only when the slice threshold sits on the signal DC mean, which is
 * exactly the single-cell eye centre. So duty=50% == centred threshold. We
 * measure it from the edge array we already have: the flush is triggered by an
 * idle gap, so the comparator level during that gap equals the level after the
 * burst's last edge; reading C1VAL at flush therefore fixes the polarity of
 * every run without an ADC. Accumulated in ticks over many bursts; the 1 Hz
 * poll reads the delta.
 */
volatile uint64_t openvlc_comp_duty_high_ticks;
volatile uint64_t openvlc_comp_duty_total_ticks;
volatile uint32_t openvlc_comp_duty_permille_last; /* last burst, for logging */
/*
 * Ring backlog watermark: highest number of captured-but-unprocessed edges
 * seen at poll entry. If this approaches OPENVLC_EDGE_DMA_LEN the ring is
 * about to wrap (silent loss) - the per-frame processing is too slow.
 */
volatile uint32_t openvlc_edge_ring_peak;
volatile uint32_t openvlc_edge_ring_drops;
/* Mid-packet split diagnostics: silences > EDGE_GAP inside a frame. */
volatile uint32_t openvlc_frag_count;
volatile uint32_t openvlc_frag_last_at;        /* edges seen before the hole */
volatile uint32_t openvlc_frag_last_gap_ticks; /* hole length, TIM2 ticks */
/* CRC/RS-confirmed candidate boundaries and retained incomplete prefixes. */
volatile uint32_t openvlc_edge_soft_gap_probes;
volatile uint32_t openvlc_edge_soft_gap_completed;
volatile uint32_t openvlc_edge_soft_gap_bridged;
volatile uint32_t openvlc_edge_soft_gap_reused;
volatile uint32_t openvlc_host_frames_queued;
volatile uint32_t openvlc_host_frames_sent;
volatile uint32_t openvlc_host_frames_dropped;
volatile uint32_t openvlc_host_tx_errors;
/* Own reflected frames (src == OPENVLC_TX_SRC_ADDR) dropped before forward. */
volatile uint32_t openvlc_rx_self_dropped;
static openvlc_quality_t openvlc_last_quality;
static uint32_t openvlc_last_quality_valid;

#ifndef OPENVLC_RX_DECODE_PASS_BUDGET_US
#define OPENVLC_RX_DECODE_PASS_BUDGET_US 5500u
#endif
#ifndef OPENVLC_RX_DECODE_DEADLINE_GUARD_US
#define OPENVLC_RX_DECODE_DEADLINE_GUARD_US 750u
#endif
#ifndef OPENVLC_RX_PERIOD_FILTER_SHIFT
#define OPENVLC_RX_PERIOD_FILTER_SHIFT 3u
#endif
#ifndef OPENVLC_SFD_SYNC_HYPOTHESES_MAX
#define OPENVLC_SFD_SYNC_HYPOTHESES_MAX 4u
#endif
#ifndef OPENVLC_RX_DECODE_PASSES_MAX
#define OPENVLC_RX_DECODE_PASSES_MAX OPENVLC_SFD_SYNC_HYPOTHESES_MAX
#endif

/*
 * Convert the measured packet cadence into the number of complete decoder
 * passes that fit before the next packet. Shorter periods take effect
 * immediately; longer periods are learned slowly. One missing frame in an
 * otherwise 125 fps stream therefore cannot authorize a 12-40 ms retry and
 * start a capture-ring congestion cascade.
 */
OPENVLC_RX_INLINE uint32_t edge_cadence_current_budget(void)
{
	uint32_t budget = 1u;

	if (edge_filtered_period_ticks != 0u) {
		uint32_t period_us = (uint32_t)(
			((uint64_t)edge_filtered_period_ticks * 1000000u) /
			OPENVLC_TIM2_IC_TICK_HZ);

		if (period_us > OPENVLC_RX_DECODE_DEADLINE_GUARD_US) {
			budget =
				(period_us -
				 OPENVLC_RX_DECODE_DEADLINE_GUARD_US) /
				OPENVLC_RX_DECODE_PASS_BUDGET_US;
			if (budget == 0u)
				budget = 1u;
		}
	}
	if (budget > OPENVLC_RX_DECODE_PASSES_MAX)
		budget = OPENVLC_RX_DECODE_PASSES_MAX;
	return budget;
}

OPENVLC_RX_INLINE uint32_t edge_cadence_hypothesis_budget(
	uint32_t burst_start)
{
	if (edge_period_valid) {
		uint32_t period = burst_start - edge_previous_burst_start;

		if (edge_filtered_period_ticks == 0u ||
		    period < edge_filtered_period_ticks) {
			edge_filtered_period_ticks = period;
		} else {
			uint32_t rise =
				(period - edge_filtered_period_ticks) >>
				OPENVLC_RX_PERIOD_FILTER_SHIFT;

			if (rise == 0u && period != edge_filtered_period_ticks)
				rise = 1u;
			edge_filtered_period_ticks += rise;
		}
	} else {
		edge_period_valid = true;
	}
	edge_previous_burst_start = burst_start;
	return edge_cadence_current_budget();
}

OPENVLC_RX_INLINE uint32_t edge_dma_head(void)
{
	DMA_HandleTypeDef *hdma = htim2.hdma[TIM_DMA_ID_CC4];
	uint32_t ndtr;
	uint32_t head;

	/* If the capture DMA was never linked/started, hdma is NULL; reading
	 * NDTR through it would hard-fault. Report "no new edges" instead. */
	if (!hdma || !hdma->Instance)
		return 0u;
	ndtr = __HAL_DMA_GET_COUNTER(hdma);
	if (!ndtr || ndtr > OPENVLC_EDGE_DMA_LEN)
		return 0u;
	head = (uint32_t)OPENVLC_EDGE_DMA_LEN - ndtr;
	return head >= OPENVLC_EDGE_DMA_LEN ? 0u : head;
}

static int edge_capture_start_dma(void)
{
#if defined(STM32H743xx) || defined(STM32H723xx)
	DMA_HandleTypeDef *hdma;

	HAL_NVIC_DisableIRQ(DMA1_Stream1_IRQn);
	if (HAL_TIM_IC_Start_DMA(&htim2, TIM_CHANNEL_4, edge_dma_buf,
				 OPENVLC_EDGE_DMA_LEN) != HAL_OK)
		return -1;
	hdma = htim2.hdma[TIM_DMA_ID_CC4];
	if (hdma && hdma->Instance) {
		__HAL_DMA_DISABLE_IT(hdma, DMA_IT_TC | DMA_IT_HT |
					   DMA_IT_TE | DMA_IT_DME);
		__HAL_DMA_DISABLE_IT(hdma, DMA_IT_FE);
		openvlc_comp_dma_cr = ((DMA_Stream_TypeDef *)hdma->Instance)->CR;
		openvlc_comp_dma_fcr = ((DMA_Stream_TypeDef *)hdma->Instance)->FCR;
		openvlc_comp_dma_ndtr = ((DMA_Stream_TypeDef *)hdma->Instance)->NDTR;
	}
	NVIC_ClearPendingIRQ(DMA1_Stream1_IRQn);
	HAL_NVIC_DisableIRQ(DMA1_Stream1_IRQn);
#endif
	edge_dma_tail = 0;
	edge_capture_running = true;
	return 0;
}

#if defined(STM32H743xx) || defined(STM32H723xx)
static uint32_t comp_count_high_samples(void)
{
	uint32_t high = 0u;
	uint32_t i;

	for (i = 0u; i < 65536u; i++) {
		high += (COMP12->SR & COMP_SR_C1VAL) != 0u ? 1u : 0u;
		__NOP();
		__NOP();
		__NOP();
		__NOP();
	}
	return high;
}
#endif

#if !OPENVLC_RX_HOST_FORWARD
static void edge_capture_stop(void)
{
#if defined(STM32H743xx) || defined(STM32H723xx)
	DMA_HandleTypeDef *hdma = htim2.hdma[TIM_DMA_ID_CC4];

	if (edge_capture_running) {
		/*
		 * HAL_TIM_IC_Stop_DMA() uses HAL_DMA_Abort_IT(). The COMP receiver
		 * intentionally disables DMA IRQs, so that asynchronous abort never
		 * reaches its completion handler and the next start fails HAL_BUSY.
		 * Stop the same hardware synchronously instead.
		 */
		htim2.Instance->CCER &= ~TIM_CCER_CC4E;
		__HAL_TIM_DISABLE_DMA(&htim2, TIM_DMA_CC4);
		if (hdma && hdma->Instance)
			(void)HAL_DMA_Abort(hdma);
		__HAL_TIM_DISABLE(&htim2);
		TIM_CHANNEL_STATE_SET(&htim2, TIM_CHANNEL_4,
				      HAL_TIM_CHANNEL_STATE_READY);
		TIM_CHANNEL_N_STATE_SET(&htim2, TIM_CHANNEL_4,
					HAL_TIM_CHANNEL_STATE_READY);
	}
	NVIC_ClearPendingIRQ(DMA1_Stream1_IRQn);
	HAL_NVIC_DisableIRQ(DMA1_Stream1_IRQn);
#endif
	edge_capture_running = false;
}
#endif

#if OPENVLC_RX_DIAG_LOG && !OPENVLC_RX_HOST_FORWARD
static int edge_capture_restart_synced(void)
{
	edge_burst_len = 0;
	edge_burst_raw_len = 0;
	edge_burst_overflowed = false;
	edge_have_last = false;
	edge_sync_to_gap = true;
	edge_soft_gap_checked = false;
	if (edge_capture_start_dma() != 0)
		return -1;
	openvlc_edge_capture_restarts++;
	return 0;
}
#endif

#ifndef OPENVLC_COMP_INTERVAL_DUMP
#define OPENVLC_COMP_INTERVAL_DUMP 0
#endif
#ifndef OPENVLC_RX_CAPTURE
#define OPENVLC_RX_CAPTURE 0u
#endif
#ifndef OPENVLC_RX_CAPTURE_PERIOD_MS
#define OPENVLC_RX_CAPTURE_PERIOD_MS 20u
#endif
#ifndef OPENVLC_RX_CAPTURE_ARM_AFTER_OK
#define OPENVLC_RX_CAPTURE_ARM_AFTER_OK 20u
#endif
#ifndef OPENVLC_RX_CAPTURE_MIN_EDGES
#define OPENVLC_RX_CAPTURE_MIN_EDGES OPENVLC_RX_MIN_DECODE_EDGES
#endif
#ifndef OPENVLC_RX_CAPTURE_MAX_FAILURES
#define OPENVLC_RX_CAPTURE_MAX_FAILURES 1u
#endif
#ifndef OPENVLC_RX_CAPTURE_MAX_SUCCESSES
#define OPENVLC_RX_CAPTURE_MAX_SUCCESSES 0u
#endif
#ifndef OPENVLC_RX_CAPTURE_MIN_FRAME_GAP
#define OPENVLC_RX_CAPTURE_MIN_FRAME_GAP OPENVLC_RX_CAPTURE_ARM_AFTER_OK
#endif
#ifndef OPENVLC_RX_CAPTURE_OK_FRAME_GAP
#define OPENVLC_RX_CAPTURE_OK_FRAME_GAP \
	OPENVLC_RX_CAPTURE_MIN_FRAME_GAP
#endif

/*
 * Per-burst trace for diagnosing packet fragmentation: one line per flush with
 * the burst length, the gap that ended it, and the decode outcome. Costs ~7 ms
 * of UART per burst at 115200 - enable only while characterising the link.
 */
#ifndef OPENVLC_BURST_TRACE
#define OPENVLC_BURST_TRACE 0
#endif

static uint32_t edge_burst_end_gap; /* ticks of the gap that ended the burst */
#if OPENVLC_COMP_RUN_ANALYSIS
static uint32_t edge_run_diag_phase;

static void edge_analyze_burst_runs(void)
{
	uint32_t run1 = 0u;
	uint32_t run2 = 0u;
	uint32_t run3 = 0u;
	uint32_t run_long = 0u;
	uint32_t long_max = 0u;
#if defined(STM32H743xx) || defined(STM32H723xx)
	/*
	 * Polarity of every run, fixed once from the idle level. The flush is
	 * gap-triggered, so the comparator holds the level after the burst's last
	 * edge; C1VAL now == that level (idle_level). Run i-1 (between edge[i-1]
	 * and edge[i]) has level = idle_level ^ ((edge_burst_len-i) & 1).
	 */
	uint32_t idle_level = (COMP12->SR & COMP_SR_C1VAL) != 0u ? 1u : 0u;
	uint64_t high_ticks = 0u;
	uint64_t total_ticks = 0u;
	bool duty_eligible = edge_burst_len >= OPENVLC_RX_MIN_DECODE_EDGES;
#if OPENVLC_COMP_DUTY_INVERT
	idle_level ^= 1u;
#endif
#endif

	if (edge_burst_len < 2u) {
		return;
	}

	for (uint32_t i = 1u; i < edge_burst_len; i++) {
		uint32_t run = edge_burst[i] - edge_burst[i - 1u];

		if (run < OPENVLC_COMP_RUN1_MAX_TICKS)
			run1++;
		else if (run < OPENVLC_COMP_RUN2_MAX_TICKS)
			run2++;
		else if (run < OPENVLC_COMP_RUN3_MAX_TICKS)
			run3++;
		else {
			run_long++;
			if (run > long_max)
				long_max = run;
		}
#if defined(STM32H743xx) || defined(STM32H723xx)
		/*
		 * Diagnostic duty must reflect the actual comparator waveform seen
		 * by the decoder. Count only decode-sized bursts to reject noise and
		 * short fragments, but include every internal run up to the configured
		 * burst delimiter. Excluding >=3-cell runs hid the exact failure mode
		 * we care about: lost transitions from a marginal slicer threshold.
		 */
		if (duty_eligible && run < OPENVLC_EDGE_GAP_TICKS) {
			uint32_t level =
				idle_level ^ ((edge_burst_len - i) & 1u);

			total_ticks += run;
			if (level)
				high_ticks += run;
		}
#endif
	}
#if defined(STM32H743xx) || defined(STM32H723xx)
	if (total_ticks) {
		openvlc_comp_duty_high_ticks +=
			high_ticks * OPENVLC_COMP_RUN_DIAG_DECIMATION;
		openvlc_comp_duty_total_ticks +=
			total_ticks * OPENVLC_COMP_RUN_DIAG_DECIMATION;
		openvlc_comp_duty_permille_last =
			(uint32_t)((high_ticks * 1000u) / total_ticks);
	}
#endif

	openvlc_edge_run1_total +=
		run1 * OPENVLC_COMP_RUN_DIAG_DECIMATION;
	openvlc_edge_run2_total +=
		run2 * OPENVLC_COMP_RUN_DIAG_DECIMATION;
	openvlc_edge_run3_total +=
		run3 * OPENVLC_COMP_RUN_DIAG_DECIMATION;
	openvlc_edge_long_total +=
		run_long * OPENVLC_COMP_RUN_DIAG_DECIMATION;
	if (long_max > openvlc_edge_long_max_ticks)
		openvlc_edge_long_max_ticks = long_max;
}
#endif

#if OPENVLC_RX_CAPTURE
static bool rx_capture_all_quotas_full(void)
{
	return rx_capture_failures >= OPENVLC_RX_CAPTURE_MAX_FAILURES &&
	       rx_capture_successes >= OPENVLC_RX_CAPTURE_MAX_SUCCESSES;
}

/*
 * Prepare one immutable interval snapshot. In raw mode this runs before edge
 * filtering; otherwise it receives the exact edge stream handed to the
 * decoder. It never formats text or queues UART data.
 */
static void rx_capture_prepare_edges(const uint32_t *edges, uint32_t edge_count)
{
	uint32_t interval_count;
	uint32_t hash = 2166136261u;

	rx_capture_prepared = false;
	if (rx_capture_state != RX_CAPTURE_ARMED || !edges || edge_count < 2u)
		return;

	interval_count = edge_count - 1u;
	if (interval_count > OPENVLC_RX_CAPTURE_MAX_INTERVALS)
		interval_count = OPENVLC_RX_CAPTURE_MAX_INTERVALS;
	rx_capture_clipped = (edge_count - 1u) - interval_count;
	for (uint32_t i = 0u; i < interval_count; i++) {
		uint32_t run = edges[i + 1u] - edges[i];

		if (run > UINT16_MAX) {
			run = UINT16_MAX;
			rx_capture_clipped++;
		}
		rx_capture_intervals[i] = (uint16_t)run;
		hash ^= (uint8_t)(run >> 8);
		hash *= 16777619u;
		hash ^= (uint8_t)run;
		hash *= 16777619u;
	}

	rx_capture_count = interval_count;
	rx_capture_hash = hash;
	rx_capture_prepared = true;
}

static void rx_capture_snapshot(openvlc_status_t status, bool successful)
{
	if (rx_capture_state != RX_CAPTURE_ARMED || !rx_capture_prepared ||
	    edge_burst_len < OPENVLC_RX_CAPTURE_MIN_EDGES)
		return;
	if (successful) {
		if (rx_capture_successes >= OPENVLC_RX_CAPTURE_MAX_SUCCESSES)
			return;
	} else if (rx_capture_failures >= OPENVLC_RX_CAPTURE_MAX_FAILURES) {
		return;
	}

	rx_capture_position = 0u;
	rx_capture_id++;
	rx_capture_status = (int32_t)status;
	rx_capture_parse_status = openvlc_phy_dbg_parse_status;
	rx_capture_t0 = openvlc_phy_dbg_sfdsync_cell0;
	rx_capture_t1 = openvlc_phy_dbg_sfdsync_cell1;
	rx_capture_nominal = openvlc_phy_dbg_track_nominal_end;
	rx_capture_residual_q8 = openvlc_phy_dbg_timing_residual_peak;
	rx_capture_syncs = openvlc_phy_dbg_sfdsync_syncs;
	rx_capture_mode = openvlc_phy_dbg_sfdsync_mode;
	rx_capture_hypothesis_budget = openvlc_rx_hypothesis_budget;
	rx_capture_lock = openvlc_phy_dbg_sfdsync_lock_cell;
	rx_capture_len_raw = openvlc_phy_dbg_len_raw;
	rx_capture_threshold_dac = HAL_DAC_GetValue(&hdac1, DAC_CHANNEL_1);
	rx_capture_snapshot_ms = HAL_GetTick();
	if (successful) {
		rx_capture_trigger = 0u;
		rx_capture_successes++;
	} else {
		rx_capture_failures++;
		if (openvlc_phy_dbg_parse_status == OPENVLC_ERR_CRC)
			rx_capture_trigger = 1u;
		else if (openvlc_phy_dbg_sfdsync_syncs == 0u)
			rx_capture_trigger = 2u;
		else if (openvlc_phy_dbg_parse_status == OPENVLC_ERR_ARG)
			rx_capture_trigger = 3u;
		else
			rx_capture_trigger = 4u;
	}

	/* Publish only after the complete snapshot and metadata are stable. */
	rx_capture_state = RX_CAPTURE_BEGIN;
}
#endif

/*
 * Decode a candidate packet boundary. A soft boundary is consumed only when
 * the complete frame passes CRC/RS; otherwise the filtered prefix stays in
 * edge_burst[] and acquisition continues. A hard boundary always accounts and
 * releases the candidate, preserving the existing failure diagnostics.
 */
static bool edge_flush_burst(bool final_boundary)
{
	uint32_t raw_burst_len = edge_burst_raw_len;
	uint32_t removed_edges = 0u;
	bool consumed = final_boundary;
	bool reuse_soft_failure = final_boundary && edge_soft_gap_checked &&
		edge_soft_gap_len == edge_burst_len &&
		edge_soft_gap_status != OPENVLC_OK;

	/* An overflowed burst cannot be decoded or captured; do no O(n) work. */
	if (!edge_burst_overflowed && !reuse_soft_failure) {
#if OPENVLC_RX_CAPTURE && OPENVLC_RX_CAPTURE_RAW
		rx_capture_prepare_edges(edge_burst, edge_burst_len);
#endif

#if OPENVLC_RX_EDGE_FILTER_MODE == OPENVLC_RX_EDGE_FILTER_LEGACY
		{
			uint32_t before = edge_burst_len;

			edge_burst_len = (uint32_t)openvlc_edge_cancel_short_pulses(
				edge_burst, edge_burst_len,
				OPENVLC_EDGE_MIN_INTERVAL_TICKS);
			removed_edges = before - edge_burst_len;
		}
#elif OPENVLC_RX_EDGE_FILTER_MODE == OPENVLC_RX_EDGE_FILTER_CONTEXTUAL
		edge_burst_len = (uint32_t)openvlc_edge_filter_timing_aware(
			edge_burst, edge_burst_len, OPENVLC_EDGE_MIN_INTERVAL_TICKS,
			OPENVLC_EDGE_HARD_GLITCH_TICKS,
			OPENVLC_EDGE_CONTEXT_MARGIN_TICKS, &removed_edges);
#elif OPENVLC_RX_EDGE_FILTER_MODE != OPENVLC_RX_EDGE_FILTER_NONE
#error "Invalid OPENVLC_RX_EDGE_FILTER_MODE"
#endif
		openvlc_edge_glitches += removed_edges;

#if OPENVLC_RX_CAPTURE && !OPENVLC_RX_CAPTURE_RAW
		rx_capture_prepare_edges(edge_burst, edge_burst_len);
#endif
	}

	openvlc_edge_last_decode_status = 99;
#if OPENVLC_COMP_INTERVAL_DUMP
	{
		/*
		 * One-shot: dump the head and tail intervals (timer ticks) of
		 * the first FAILING-sized burst (the ~7580-edge mystery bursts)
		 * so we can see whether a preamble is present at its start
		 * (uniform ~1-cell alternation) or it really starts mid-data
		 * (bimodal 1-cell/2-cell mix).
		 */
		static bool intv_dumped;

		if (!intv_dumped && edge_burst_len >= 7000u &&
		    edge_burst_len < 11000u) {
			intv_dumped = true;
			openvlc_platform_log("INTV burst_len=%lu head 48 intervals:\r\n",
					     (unsigned long)edge_burst_len);
			for (uint32_t i = 1; i < 49u && i < edge_burst_len; i += 16u) {
				char line[160];
				int pos = 0;

				for (uint32_t j = i; j < i + 16u && j < 49u &&
				     j < edge_burst_len; j++)
					pos += snprintf(line + pos,
							sizeof(line) - (size_t)pos,
							"%lu ",
							(unsigned long)(edge_burst[j] -
									edge_burst[j - 1u]));
				openvlc_platform_log("%s\r\n", line);
			}
			openvlc_platform_log("INTV tail 16 intervals:\r\n");
			{
				char line[160];
				int pos = 0;
				uint32_t start = edge_burst_len > 17u ?
					edge_burst_len - 16u : 1u;

				for (uint32_t j = start; j < edge_burst_len; j++)
					pos += snprintf(line + pos,
							sizeof(line) - (size_t)pos,
							"%lu ",
							(unsigned long)(edge_burst[j] -
									edge_burst[j - 1u]));
				openvlc_platform_log("%s\r\n", line);
			}
		}
	}
#endif
	if (!edge_burst_overflowed &&
	    edge_burst_len >= OPENVLC_RX_MIN_DECODE_EDGES) {
		openvlc_status_t decode_status;
#if defined(STM32H743xx) || defined(STM32H723xx)
		uint32_t decode_start = __HAL_TIM_GET_COUNTER(&htim2);
#endif

		{
			uint32_t cadence_budget = final_boundary ?
				edge_cadence_hypothesis_budget(edge_burst[0]) :
				edge_cadence_current_budget();

			/*
			 * Zero means unlimited to the portable decoder. Combine the
			 * cadence deadline with the pressure/TX limit selected by poll;
			 * the strictest non-zero limit wins.
			 * A speculative 4-us decode uses the learned cadence without
			 * advancing it; only a confirmed or hard boundary learns a new
			 * packet start.
			 */
			if (openvlc_rx_hypothesis_budget == 0u ||
			    cadence_budget < openvlc_rx_hypothesis_budget)
				openvlc_rx_hypothesis_budget = cadence_budget;
		}
		openvlc_rx_last_decode_budget =
			openvlc_rx_hypothesis_budget;
		if (reuse_soft_failure) {
			decode_status = edge_soft_gap_status;
			openvlc_app_commit_rx_failure(decode_status);
			openvlc_edge_soft_gap_reused++;
		} else if (final_boundary) {
			openvlc_app_rx_edges(edge_burst, edge_burst_len,
					     rx_accum_buffer,
					     OPENVLC_RX_SAMPLE_BUFFER_LEN);
			decode_status = openvlc_app_last_status();
		} else {
			openvlc_edge_soft_gap_probes++;
			decode_status = openvlc_app_try_rx_edges(
				edge_burst, edge_burst_len, rx_accum_buffer,
				OPENVLC_RX_SAMPLE_BUFFER_LEN);
			if (decode_status != OPENVLC_OK) {
				edge_soft_gap_len = edge_burst_len;
				edge_soft_gap_status = decode_status;
#if defined(STM32H743xx) || defined(STM32H723xx)
				openvlc_decode_last_ticks =
					__HAL_TIM_GET_COUNTER(&htim2) - decode_start;
				if (openvlc_decode_last_ticks >
				    openvlc_decode_max_ticks)
					openvlc_decode_max_ticks =
						openvlc_decode_last_ticks;
#endif
				return false;
			}
			consumed = true;
			openvlc_edge_soft_gap_completed++;
			/* Learn cadence only from a confirmed complete packet. */
			(void)edge_cadence_hypothesis_budget(edge_burst[0]);
		}
		openvlc_edge_bursts_decoded++;

		openvlc_edge_last_decode_status = (int32_t)decode_status;
		if (decode_status == OPENVLC_OK) {
			openvlc_edge_last_ok_burst_len = edge_burst_len;
		} else {
			openvlc_edge_last_fail_burst_len = edge_burst_len;
		}
#if OPENVLC_RX_CAPTURE
		/*
		 * Startup qualification is deliberately separate from sampling.
		 * Once the receiver has produced a clean run, later link errors do
		 * not make us reclassify normal traffic as startup transients.
		 */
		if (!rx_capture_qualified) {
			if (decode_status == OPENVLC_OK) {
				if (rx_capture_warmup_ok <
				    OPENVLC_RX_CAPTURE_ARM_AFTER_OK)
					rx_capture_warmup_ok++;
				if (rx_capture_warmup_ok >=
				    OPENVLC_RX_CAPTURE_ARM_AFTER_OK) {
					rx_capture_qualified = true;
					rx_capture_frame_gap = 0u;
				}
			}
		} else if (rx_capture_state == RX_CAPTURE_ARMED &&
			   !rx_capture_all_quotas_full()) {
			if (rx_capture_frame_gap < UINT32_MAX)
				rx_capture_frame_gap++;
			if (decode_status != OPENVLC_OK &&
			    rx_capture_frame_gap >=
				    OPENVLC_RX_CAPTURE_MIN_FRAME_GAP)
				rx_capture_snapshot(decode_status, false);
			else if (decode_status == OPENVLC_OK &&
				 rx_capture_frame_gap >=
					 OPENVLC_RX_CAPTURE_OK_FRAME_GAP)
				rx_capture_snapshot(decode_status, true);
		}
#endif
#if defined(STM32H743xx) || defined(STM32H723xx)
		if (!reuse_soft_failure) {
			openvlc_decode_last_ticks =
				__HAL_TIM_GET_COUNTER(&htim2) - decode_start;
			if (openvlc_decode_last_ticks > openvlc_decode_max_ticks)
				openvlc_decode_max_ticks = openvlc_decode_last_ticks;
		}
#endif
	} else if (!final_boundary) {
		return false;
	} else if (!edge_burst_overflowed && edge_burst_len > 1u) {
		openvlc_edge_short_bursts++;
		openvlc_edge_last_short_burst_len = edge_burst_len;
	}
	if (!consumed)
		return false;

	openvlc_edge_last_raw_burst_len = raw_burst_len;
	openvlc_edge_last_burst_len = edge_burst_len;
	if (edge_burst_len > openvlc_edge_max_burst)
		openvlc_edge_max_burst = edge_burst_len;
#if OPENVLC_COMP_RUN_ANALYSIS
	if (!edge_burst_overflowed &&
	    ++edge_run_diag_phase >= OPENVLC_COMP_RUN_DIAG_DECIMATION) {
		edge_run_diag_phase = 0u;
		edge_analyze_burst_runs();
	}
#endif
#if OPENVLC_BURST_TRACE
	if (edge_burst_len >= 16u)
		openvlc_platform_log("BURST len=%lu endgap_us=%lu st=%ld syncs=%lu prerej=%lu\r\n",
				     (unsigned long)edge_burst_len,
				     (unsigned long)(edge_burst_end_gap /
						     (OPENVLC_TIM2_IC_TICK_HZ / 1000000u)),
				     (long)openvlc_app_last_status(),
				     (unsigned long)openvlc_phy_dbg_sfdsync_syncs,
				     (unsigned long)openvlc_phy_dbg_sfdsync_pre_rejects);
#endif
	edge_burst_len = 0;
	edge_burst_raw_len = 0;
	edge_burst_overflowed = false;
	edge_have_last = false;
	edge_soft_gap_checked = false;
	edge_burst_end_gap = 0;
#if OPENVLC_RX_CAPTURE
	if (rx_capture_state == RX_CAPTURE_ARMED)
		rx_capture_prepared = false;
#endif
	return true;
}

int openvlc_stm32_rx_comparator_start(void)
{
#if defined(STM32H743xx) || defined(STM32H723xx)
	openvlc_comp_start_step = 1u;
	HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
			 OPENVLC_COMP_THRESHOLD_DAC);
	if (HAL_DAC_Start(&hdac1, DAC_CHANNEL_1) != HAL_OK)
		return -1;
	openvlc_comp_start_step = 2u;
	if (HAL_COMP_Start(&hcomp1) != HAL_OK)
		return -2;
	openvlc_comp_start_step = 3u;
	/*
	 * Brief analog-path self-test. It runs before capture, checks COMP1 at
	 * both DAC rails, and restores the configured receive threshold.
	 */
	if (HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 0u) != HAL_OK)
		return -5;
	HAL_Delay(2u);
	openvlc_comp_test_zero_dor = DAC1->DOR1 & DAC_DOR1_DACC1DOR;
	openvlc_comp_test_zero_high = comp_count_high_samples();
	if (HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 4095u) != HAL_OK)
		return -6;
	HAL_Delay(2u);
	openvlc_comp_test_full_dor = DAC1->DOR1 & DAC_DOR1_DACC1DOR;
	openvlc_comp_test_full_high = comp_count_high_samples();
	if (HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
			     OPENVLC_COMP_THRESHOLD_DAC) != HAL_OK)
		return -7;
	HAL_Delay(2u);
	if (HAL_TIMEx_TISelection(&htim2, TIM_TIM2_TI4_COMP1,
				  TIM_CHANNEL_4) != HAL_OK)
		return -3;
	openvlc_comp_start_step = 4u;
	/*
	 * This RX path is polled: DMA writes the circular capture ring and the
	 * main loop reads NDTR. HAL_TIM_IC_Start_DMA enables DMA interrupts, but
	 * we do not consume HT/TC callbacks here; at optical edge rates they can
	 * fault before the first main-loop poll. Keep the stream running, disable
	 * its IRQ sources, and poll the ring explicitly.
	 */
	if (edge_capture_start_dma() != 0)
		return -4;
	openvlc_comp_start_step = 5u;
	openvlc_comp_last_level =
		(COMP12->SR & COMP_SR_C1VAL) != 0u ? 1u : 0u;
	openvlc_comp_level_valid = true;
	openvlc_comp_start_step = 6u;
#endif
	edge_burst_len = 0;
	edge_burst_raw_len = 0;
	edge_burst_overflowed = false;
	edge_have_last = false;
	edge_sync_to_gap = false;
	edge_soft_gap_checked = false;
	edge_previous_burst_start = 0u;
	edge_filtered_period_ticks = 0u;
	edge_period_valid = false;
	openvlc_rx_last_decode_budget = 0u;
	return 0;
}

OPENVLC_RX_HOT void openvlc_stm32_rx_comparator_poll(void)
{
#if OPENVLC_RX_DEEP_DEBUG_LOG
	uint32_t comp_level;
#endif
	uint32_t head = edge_dma_head();
	uint32_t backlog;
#if OPENVLC_RX_DIAG_LOG
	uint32_t processed_edges = 0u;
#endif

	if (!edge_capture_running)
		return;
#if defined(STM32H743xx) || defined(STM32H723xx)
	/* CC4OF means a new comparator edge arrived before DMA consumed CCR4.
	 * Unlike the software burst/ring counters, this directly exposes DMA
	 * starvation or bus contention. The flag is sticky until software clears it. */
	if ((TIM2->SR & TIM_SR_CC4OF) != 0u) {
		openvlc_edge_hw_overcaptures++;
		__HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_CC4OF);
	}
#if OPENVLC_RX_DEEP_DEBUG_LOG
	/* Independent probe of COMP1_OUT, before the TIM2 input mux and DMA. */
	comp_level = (COMP12->SR & COMP_SR_C1VAL) != 0u ? 1u : 0u;
	openvlc_comp_level_samples++;
	if (openvlc_comp_level_valid && comp_level != openvlc_comp_last_level)
		openvlc_comp_level_changes++;
	openvlc_comp_last_level = comp_level;
	openvlc_comp_level_valid = true;
#endif
#endif
	/* Ring backlog watermark: detect how close we get to a silent wrap. */
	backlog = head >= edge_dma_tail ? head - edge_dma_tail :
		  OPENVLC_EDGE_DMA_LEN - edge_dma_tail + head;
#if OPENVLC_RX_DIAG_LOG
	if (backlog > openvlc_edge_ring_peak)
		openvlc_edge_ring_peak = backlog;
#endif
	/*
	 * A damaged frame must not consume enough CPU to overflow the capture
	 * ring. This pressure/TX limit is combined at burst flush with the stricter
	 * start-to-start cadence deadline, which protects a continuous stream even
	 * when backlog is still low at poll entry.
	 */
	if (backlog > OPENVLC_EDGE_DMA_LEN / 8u)
		openvlc_rx_hypothesis_budget = 1u;
	else if (openvlc_stm32_tx_busy() ||
		 openvlc_stm32_tx_pipeline_depth() != 0u)
		openvlc_rx_hypothesis_budget = 2u;
	else
		openvlc_rx_hypothesis_budget = 0u;
	/*
	 * Congestion-collapse guard. Past 3/4 of the ring a silent wrap is
	 * imminent (or already happened): the oldest edges - the preamble of
	 * the burst being assembled - get overwritten, every decode then fails,
	 * and failed bursts cost more than good ones, so the backlog never
	 * drains on its own. Drop the whole backlog, resync to the next burst
	 * gap, and pay a couple of lost frames instead of a dead link.
	 */
	if (backlog > (OPENVLC_EDGE_DMA_LEN / 4u) * 3u) {
		edge_dma_tail = head;
		edge_burst_len = 0;
		edge_burst_raw_len = 0;
		edge_burst_overflowed = false;
		edge_have_last = false;
		edge_sync_to_gap = true;
		edge_soft_gap_checked = false;
		openvlc_edge_ring_drops++;
		return;
	}
#if OPENVLC_RX_DEEP_DEBUG_LOG
	openvlc_comp_poll_step = 1u;
	openvlc_comp_poll_head = head;
	openvlc_comp_poll_tail = edge_dma_tail;
	openvlc_comp_poll_burst_len = edge_burst_len;
#endif
#if defined(OPENVLC_RX_STREAMING) && OPENVLC_RX_STREAMING >= 3
	/*
	 * Continuous RX. The DMA ring is drained straight into the decoder and
	 * edge_burst[] is never touched: no burst assembly, no gap segmentation,
	 * no SFD search window, no idle-timeout flush. This is the whole point
	 * of the streaming decoder - fed from a burst it still waits for a
	 * complete frame, so the latency is unchanged; fed from the ring it
	 * decodes as the light arrives.
	 *
	 * Consequences to expect in the COMP line: gd, fr, fa, fg, br, bl, bok,
	 * ovf, lock all stop moving - they describe a burst pipeline that no
	 * longer runs. seen/ok/crc/sync stay meaningful (see
	 * openvlc_app_commit_rx_stream). du/dm become per-chunk, not per-frame.
	 */
	while (edge_dma_tail != head) {
		uint32_t chunk[OPENVLC_STREAM_POLL_CHUNK];
		uint32_t n = 0u;

		while (edge_dma_tail != head &&
		       n < OPENVLC_STREAM_POLL_CHUNK) {
			chunk[n++] = edge_dma_buf[edge_dma_tail];
			if (++edge_dma_tail >= OPENVLC_EDGE_DMA_LEN)
				edge_dma_tail = 0u;
		}
		openvlc_app_commit_rx_stream(chunk, n);
	}
	return;
#endif
	while (edge_dma_tail != head) {
		uint32_t t = edge_dma_buf[edge_dma_tail];
#if OPENVLC_RX_RAW_INTERVAL_HIST
		uint32_t raw_interval = edge_have_last ?
			t - edge_last_tick : UINT32_MAX;
#endif
		bool yield_after_edge = false;

#if OPENVLC_RX_DEEP_DEBUG_LOG
		openvlc_comp_poll_step = 2u;
#endif
		if (++edge_dma_tail >= OPENVLC_EDGE_DMA_LEN)
			edge_dma_tail = 0;
#if OPENVLC_RX_DIAG_LOG
		processed_edges++;
#endif
#if OPENVLC_RX_DEEP_DEBUG_LOG
		openvlc_comp_poll_tail = edge_dma_tail;
#endif
		if (edge_sync_to_gap) {
			if (edge_have_last &&
			    (t - edge_last_tick) > OPENVLC_EDGE_GAP_TICKS) {
				edge_sync_to_gap = false;
				edge_soft_gap_checked = false;
				edge_burst_len = 0;
				edge_burst_raw_len = 1u;
				edge_burst_overflowed = false;
				edge_burst[edge_burst_len++] = t;
#if OPENVLC_RX_DEEP_DEBUG_LOG
				openvlc_comp_poll_burst_len = edge_burst_len;
#endif
			}
			edge_last_tick = t;
			edge_have_last = true;
			continue;
		}
		if (edge_have_last &&
		    (t - edge_last_tick) > OPENVLC_EDGE_GAP_TICKS) {
			uint32_t gap = t - edge_last_tick;

			openvlc_edge_gaps_seen++;
			uint32_t decoded_before = openvlc_edge_bursts_decoded;
			bool consumed = false;
			bool hard_boundary =
				gap > OPENVLC_EDGE_HARD_GAP_TICKS;

			/*
			 * The candidate gap is completion-driven: a CRC/RS-valid packet
			 * is delivered immediately, while an incomplete prefix survives
			 * until the hard gap. This is the useful property of a continuous
			 * interval decoder without removing our validated frame parser.
			 */
			edge_burst_end_gap = gap;
#if OPENVLC_EDGE_HARD_GAP_US > OPENVLC_EDGE_GAP_US
			if (hard_boundary) {
				if (edge_burst_len >= OPENVLC_FRAG_MIN_EDGES &&
				    edge_burst_len < OPENVLC_FRAG_MAX_EDGES) {
					openvlc_frag_count++;
					openvlc_frag_last_at = edge_burst_len;
					openvlc_frag_last_gap_ticks = gap;
				}
				consumed = edge_flush_burst(true);
			}
#if !OPENVLC_RX_CAPTURE
			else if (!edge_soft_gap_checked) {
				bool attempted = !edge_burst_overflowed &&
					edge_burst_len >= OPENVLC_RX_MIN_DECODE_EDGES;

				consumed = edge_flush_burst(false);
				if (!consumed && attempted) {
					edge_soft_gap_checked = true;
					openvlc_edge_soft_gap_bridged++;
				}
			} else {
				/* The idle-time poll already rejected this prefix. */
				openvlc_edge_soft_gap_bridged++;
			}
#endif
#else
			(void)hard_boundary;
			if (edge_burst_len >= OPENVLC_FRAG_MIN_EDGES &&
			    edge_burst_len < OPENVLC_FRAG_MAX_EDGES) {
				openvlc_frag_count++;
				openvlc_frag_last_at = edge_burst_len;
				openvlc_frag_last_gap_ticks = gap;
			}
			consumed = edge_flush_burst(true);
#endif
			yield_after_edge =
				openvlc_edge_bursts_decoded != decoded_before;
			if (!consumed)
				edge_soft_gap_checked = false;
		}
		if (edge_burst_overflowed) {
			edge_last_tick = t;
			edge_have_last = true;
			continue;
		}
#if OPENVLC_RX_DEEP_DEBUG_LOG
		openvlc_comp_poll_step = 3u;
#endif
		edge_burst_raw_len++;
#if OPENVLC_RX_RAW_INTERVAL_HIST
		/*
		 * One comparison is paid by every edge. Only genuinely short
		 * intervals enter the bin chain and write RAM. Inter-frame gaps and
		 * normal 30/34-tick cells therefore add no counter-write pressure.
		 */
		if (raw_interval < 24u) {
			uint32_t bin;

			if (raw_interval < 8u)
				bin = 0u;
			else if (raw_interval < 12u)
				bin = 1u;
			else if (raw_interval < 16u)
				bin = 2u;
			else if (raw_interval < 20u)
				bin = 3u;
			else
				bin = 4u;
			openvlc_edge_raw_short_hist[bin]++;
		}
#endif
		/* Preserve raw edges; filtering runs once with packet-local context. */
		if (edge_burst_len >= OPENVLC_EDGE_BURST_LEN) {
			openvlc_edge_overflows++;
			/*
			 * A full buffer used to set edge_burst_overflowed, which
			 * every decode/capture path treats as poison - the burst
			 * was thrown away whole. That discards a frame we already
			 * hold: the payload sits at the FRONT of edge_burst[], and
			 * what does not fit is the TX idle keep-alive that trails
			 * it. Since the keep-alive modulates continuously there is
			 * no inter-frame gap to segment on, so the idle lands in
			 * the same burst as the frame; when the host's pacing
			 * stutters (idle_us 2477 vs idlemin_us 422 = 6x) the extra
			 * ~5000 edges push a normal 13030-edge burst past 16384.
			 * That was ~2 losses/s on an otherwise crc=0 link, i.e.
			 * 4x the sync failures and the dominant loss term.
			 *
			 * So decode what we captured instead of dropping it, then
			 * resync at the next real gap to skip the remaining idle.
			 * This cannot deliver a corrupt packet: a genuinely
			 * truncated burst still fails CRC/RS exactly as before.
			 * Enlarging the buffer was rejected - it is already 64 KB
			 * and would only move the cliff, not remove it.
			 */
			(void)edge_flush_burst(true);
			edge_sync_to_gap = true;
			edge_soft_gap_checked = false;
			edge_last_tick = t;
			edge_have_last = true;
			continue;
		}
		edge_burst[edge_burst_len++] = t;
#if OPENVLC_RX_DEEP_DEBUG_LOG
		openvlc_comp_poll_burst_len = edge_burst_len;
#endif
		edge_last_tick = t;
		edge_have_last = true;
		/*
		 * Full-duplex fairness: never decode two completed packet-sized
		 * bursts in one foreground visit. A second synchronous decode can
		 * keep the host parser and TX slot refill asleep for 10-17 ms, burst
		 * the UART queue and let TIM2 reach its 3/4-ring guard. The current
		 * edge has already been preserved as the head of the next burst, so
		 * returning here is lossless; the next main-loop iteration resumes at
		 * edge_dma_tail after servicing UART/TX.
		 */
		if (yield_after_edge) {
#if OPENVLC_RX_DIAG_LOG
			openvlc_edge_total += processed_edges;
#endif
#if OPENVLC_RX_DEEP_DEBUG_LOG
			openvlc_comp_poll_step = 4u;
#endif
			return;
		}
	}

	/*
	 * Finalize a burst as soon as the configured idle interval has elapsed.
	 * The old path discovered the gap only when the first edge of the NEXT
	 * frame arrived. It therefore wasted the complete inter-frame idle time
	 * and began synchronous decoding while the following packet was already
	 * entering the DMA ring. TIM2 is free-running, so this timeout is precise
	 * to one foreground poll and also delivers the final packet when TX stops.
	 */
#if (defined(STM32H743xx) || defined(STM32H723xx)) && \
	OPENVLC_RX_IDLE_TIMEOUT_FLUSH
	if (!edge_sync_to_gap && edge_have_last &&
	    (edge_burst_len != 0u || edge_burst_overflowed)) {
		uint32_t idle_head = edge_dma_head();

		/*
		 * `head` above is only the snapshot taken on function entry. At
		 * 1.6 Medges/s DMA can append several transitions while that snapshot
		 * is being drained. Comparing the free-running timer with the last
		 * CONSUMED edge in that situation falsely reports an idle gap and
		 * splits a live packet. Finalize only when a fresh DMA-head sample
		 * proves that the ring is empty, then verify it once more after
		 * sampling the timer to close the DMA/CPU race.
		 */
		if (idle_head == edge_dma_tail) {
			uint32_t idle_ticks =
				__HAL_TIM_GET_COUNTER(&htim2) - edge_last_tick;

			__DMB();
			if (idle_ticks > OPENVLC_EDGE_GAP_TICKS &&
			    edge_dma_head() == edge_dma_tail) {
				edge_burst_end_gap = idle_ticks;
#if OPENVLC_EDGE_HARD_GAP_US > OPENVLC_EDGE_GAP_US
				if (idle_ticks > OPENVLC_EDGE_HARD_GAP_TICKS) {
					if (edge_burst_len >= OPENVLC_FRAG_MIN_EDGES &&
					    edge_burst_len < OPENVLC_FRAG_MAX_EDGES) {
						openvlc_frag_count++;
						openvlc_frag_last_at = edge_burst_len;
						openvlc_frag_last_gap_ticks = idle_ticks;
					}
					(void)edge_flush_burst(true);
				}
#if !OPENVLC_RX_CAPTURE
				else if (!edge_soft_gap_checked) {
					bool attempted = !edge_burst_overflowed &&
						edge_burst_len >=
							OPENVLC_RX_MIN_DECODE_EDGES;

					if (!edge_flush_burst(false) && attempted)
						edge_soft_gap_checked = true;
				}
#endif
#else
				if (edge_burst_len >= OPENVLC_FRAG_MIN_EDGES &&
				    edge_burst_len < OPENVLC_FRAG_MAX_EDGES) {
					openvlc_frag_count++;
					openvlc_frag_last_at = edge_burst_len;
					openvlc_frag_last_gap_ticks = idle_ticks;
				}
				(void)edge_flush_burst(true);
#endif
			}
		}
	}
#endif
#if OPENVLC_RX_DEEP_DEBUG_LOG
	openvlc_comp_poll_step = 4u;
#endif
#if OPENVLC_RX_DIAG_LOG
	openvlc_edge_total += processed_edges;
#endif

}
void openvlc_stm32_init(void)
{
	openvlc_runtime_config_t cfg = {
		.self_id = OPENVLC_ADDR_SELF_DEFAULT,
		.peer_id = OPENVLC_ADDR_PEER_DEFAULT,
		.line_code = OPENVLC_LINE_CODE,
		.samples_per_symbol = OPENVLC_SAMPLES_PER_SYMBOL,
		.mf_score_min = OPENVLC_MF_SCORE_MIN,
		.snr_min_db_centi = OPENVLC_SNR_MIN_DB_CENTI,
	};

	openvlc_app_init(&cfg);
	memset(&openvlc_last_quality, 0, sizeof(openvlc_last_quality));
	openvlc_last_quality_valid = 0u;
#if (defined(STM32H743xx) || defined(STM32H723xx)) && OPENVLC_RX_HOST_FORWARD
	/* Host UART DMA completion and RX error/timeout handling. */
	HAL_NVIC_SetPriority(USART3_IRQn, 6u, 0u);
	HAL_NVIC_EnableIRQ(USART3_IRQn);
#endif
	openvlc_platform_log("OpenVLC STM32 init: node=%u phy=%u>%u line=%u preamble=%u rate=%uk tx_budget=%u tx_cell=%u rs=%u COMP1_PB0 threshold_dac=%u hyst=%u tim2_ic=%luHz gap_us=%u/%u edgefilter=%u gate=%u hard=%u timingmin=%u host=%u baud=%lu icache=%u rxlocal=%u capture=%u capraw=%u capok=%u capfail=%u caparm=%u capgap=%u/%u capmin=%u\r\n",
			     (unsigned int)OPENVLC_TRANSCEIVER_NODE,
			     (unsigned int)OPENVLC_TX_SRC_ADDR,
			     (unsigned int)OPENVLC_TX_DST_ADDR,
			     cfg.line_code, (unsigned int)OPENVLC_PREAMBLE_BYTES,
			     (unsigned int)OPENVLC_PHY_RATE_KBPS,
			     (unsigned int)OPENVLC_STM32_TX_PROFILE_BUDGET,
			     (unsigned int)OPENVLC_STM32_TX_CELL_TICKS,
			     (unsigned int)OPENVLC_BEAGLEBONE_RS_ECC_BYTES,
			     (unsigned int)OPENVLC_COMP_THRESHOLD_DAC,
			     (unsigned int)OPENVLC_COMP_HYSTERESIS_LEVEL,
			     (unsigned long)OPENVLC_TIM2_IC_TICK_HZ,
			     (unsigned int)OPENVLC_EDGE_GAP_US,
			     (unsigned int)OPENVLC_EDGE_HARD_GAP_US,
			     (unsigned int)OPENVLC_RX_EDGE_FILTER_MODE,
			     (unsigned int)OPENVLC_EDGE_MIN_INTERVAL_TICKS,
			     (unsigned int)OPENVLC_EDGE_HARD_GLITCH_TICKS,
			     (unsigned int)OPENVLC_COMP_MIN_HALFCELL_TICKS,
			     (unsigned int)OPENVLC_RX_HOST_FORWARD,
			     (unsigned long)OPENVLC_HOST_UART_BAUD,
			     (unsigned int)((SCB->CCR & SCB_CCR_IC_Msk) != 0u),
			     (unsigned int)OPENVLC_RX_LOCAL_TIMING,
			     (unsigned int)OPENVLC_RX_CAPTURE,
			     (unsigned int)OPENVLC_RX_CAPTURE_RAW,
			     (unsigned int)OPENVLC_RX_CAPTURE_MAX_SUCCESSES,
			     (unsigned int)OPENVLC_RX_CAPTURE_MAX_FAILURES,
			     (unsigned int)OPENVLC_RX_CAPTURE_ARM_AFTER_OK,
			     (unsigned int)OPENVLC_RX_CAPTURE_MIN_FRAME_GAP,
			     (unsigned int)OPENVLC_RX_CAPTURE_OK_FRAME_GAP,
			     (unsigned int)OPENVLC_RX_CAPTURE_MIN_EDGES);
}

int openvlc_stm32_start(void)
{
	/* Comparator + TIM2 input-capture receiver. */
	openvlc_stm32_start_step = 1u;
	if (openvlc_stm32_rx_comparator_start() != 0)
		return -10;
	openvlc_stm32_start_step = 3u;
	return 0;
}

#if OPENVLC_COMP_THRESHOLD_AUTO && \
	(defined(STM32H743xx) || defined(STM32H723xx))
static uint32_t comp_auto_current_threshold = OPENVLC_COMP_THRESHOLD_DAC;

static uint32_t comp_auto_clamp_threshold(uint32_t threshold)
{
	if (threshold < OPENVLC_COMP_AUTO_MIN)
		return OPENVLC_COMP_AUTO_MIN;
	if (threshold > OPENVLC_COMP_AUTO_MAX)
		return OPENVLC_COMP_AUTO_MAX;
	return threshold;
}

static void comp_auto_apply_threshold(uint32_t threshold)
{
	threshold = comp_auto_clamp_threshold(threshold);
	if (HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
			     threshold) != HAL_OK)
		return;
	comp_auto_current_threshold = threshold;

	/*
	 * Changing the analog slicer threshold in the middle of a burst can
	 * create one artificial edge. Drop the partial burst and restart at the
	 * next idle gap so the threshold search scores complete frames only.
	 */
	edge_burst_len = 0u;
	edge_burst_raw_len = 0u;
	edge_burst_overflowed = false;
	edge_have_last = false;
	edge_sync_to_gap = true;
	edge_soft_gap_checked = false;
}

static int32_t comp_auto_score(uint32_t bursts, uint32_t ok,
			       uint32_t glitches, uint32_t long_gaps)
{
	uint32_t glitch_per_burst;
	uint32_t long_per_burst;
	int32_t score;

	if (!bursts)
		return -2000000000;

	/*
	 * Tune for packet quality, not just activity.
	 *
	 * On the Pi HAT comparator path a poor threshold often still produces a
	 * plausible number of bursts but loses SFD/sync on a few percent of them.
	 * A delivered frame already proves preamble, SFD, length and RS/CRC, so its
	 * delivered-frame count is the authoritative metric because every candidate
	 * has the same dwell time. Do not divide it by the detected burst count:
	 * with a marginal slicer, one optical frame may be split into several
	 * fragments, which made the old ratio prefer 10/184 over 18/625 even though
	 * the latter threshold delivered almost twice as much useful traffic.
	 * Glitch and long-run counts are payload dependent; retain them only as
	 * bounded tie-breakers that cannot outweigh one additional valid frame.
	 */
	glitch_per_burst = glitches / bursts;
	long_per_burst = long_gaps / bursts;
	if (glitch_per_burst > 100u)
		glitch_per_burst = 100u;
	if (long_per_burst > 100u)
		long_per_burst = 100u;
	score = (int32_t)(ok * 100000u);
	score -= (int32_t)glitch_per_burst;
	score -= (int32_t)(long_per_burst * 2u);
	return score;
}

static void comp_auto_threshold_poll(uint32_t now_ms,
				     const openvlc_counters_t *cc)
{
	enum {
		AUTO_IDLE = 0,
		AUTO_SCAN = 1,
		AUTO_LOCKED = 2,
	};
	static uint8_t state;
	static bool initialized;
	static uint32_t candidate_thr;
	static uint32_t best_thr;
	static int32_t best_score;
	static uint32_t mark_ms;
	static uint32_t mark_bursts;
	static uint32_t mark_ok;
	static uint32_t mark_crc;
	static uint32_t mark_sync;
	static uint32_t mark_glitch;
	static uint32_t mark_long;
	static uint32_t mark_fail_timing;
	static uint32_t mark_fail_no_sfd;
	static uint32_t mark_fail_preamble;
	static uint32_t mark_fail_parse;
	static uint32_t no_ok_activity_s;
	static uint32_t bad_quality_s;
	static uint32_t last_good_ms;
	uint32_t now_bursts = openvlc_edge_bursts_decoded;
	uint32_t now_ok = cc ? cc->frames_delivered : 0u;
	uint32_t now_crc = cc ? cc->crc_failed : 0u;
	uint32_t now_sync = cc ? cc->sync_failed : 0u;
	uint32_t dwell_ms = OPENVLC_COMP_AUTO_DWELL_S * 1000u;

#if !OPENVLC_RX_DIAG_LOG
	/*
	 * Keep the diagnostic marks state-compatible with a logging build while
	 * making their intentionally write-only role explicit to -Wall.
	 */
	(void)mark_crc;
	(void)mark_sync;
	(void)mark_fail_timing;
	(void)mark_fail_no_sfd;
	(void)mark_fail_preamble;
	(void)mark_fail_parse;
#endif

	if (!initialized) {
		initialized = true;
		state = AUTO_IDLE;
		comp_auto_apply_threshold(OPENVLC_COMP_THRESHOLD_DAC);
		last_good_ms = now_ms;
	}

	if (state == AUTO_SCAN) {
		if (now_ms - mark_ms >= dwell_ms) {
			uint32_t bursts = now_bursts - mark_bursts;
			uint32_t ok = now_ok - mark_ok;
#if OPENVLC_RX_DIAG_LOG
			uint32_t crc = now_crc - mark_crc;
			uint32_t sync = now_sync - mark_sync;
#endif
			uint32_t glitches = openvlc_edge_glitches - mark_glitch;
			uint32_t long_gaps = openvlc_edge_long_total - mark_long;
#if OPENVLC_RX_DIAG_LOG
			uint32_t fail_timing =
				openvlc_phy_dbg_sfdsync_fail_timing -
				mark_fail_timing;
			uint32_t fail_no_sfd =
				openvlc_phy_dbg_sfdsync_fail_no_sfd -
				mark_fail_no_sfd;
			uint32_t fail_preamble =
				openvlc_phy_dbg_sfdsync_fail_preamble -
				mark_fail_preamble;
			uint32_t fail_parse =
				openvlc_phy_dbg_sfdsync_fail_parse -
				mark_fail_parse;
#endif
			int32_t score = comp_auto_score(bursts, ok, glitches,
							long_gaps);

			if (score > best_score) {
				best_score = score;
				best_thr = candidate_thr;
			}

#if OPENVLC_RX_DIAG_LOG
			openvlc_platform_log(
				"COMP AUTO scan thr=%lu thr_mv=%lu bursts=%lu ok=%lu crc=%lu sync=%lu glitch=%lu long=%lu ft=%lu fn=%lu fp=%lu fx=%lu score=%ld best=%lu best_mv=%lu\r\n",
				(unsigned long)candidate_thr,
				(unsigned long)OPENVLC_DAC_TO_MV(candidate_thr),
				(unsigned long)bursts,
				(unsigned long)ok,
				(unsigned long)crc,
				(unsigned long)sync,
				(unsigned long)glitches,
				(unsigned long)long_gaps,
				(unsigned long)fail_timing,
				(unsigned long)fail_no_sfd,
				(unsigned long)fail_preamble,
				(unsigned long)fail_parse,
				(long)score,
				(unsigned long)best_thr,
				(unsigned long)OPENVLC_DAC_TO_MV(best_thr));
#endif

			if (candidate_thr + OPENVLC_COMP_AUTO_STEP >
			    OPENVLC_COMP_AUTO_MAX) {
				if (best_score <= -2000000000)
					best_thr = comp_auto_clamp_threshold(
						OPENVLC_COMP_THRESHOLD_DAC);
				comp_auto_apply_threshold(best_thr);
				state = AUTO_LOCKED;
				no_ok_activity_s = 0u;
				bad_quality_s = 0u;
				last_good_ms = now_ms;
				mark_ms = now_ms;
				mark_bursts = now_bursts;
				mark_ok = now_ok;
				mark_crc = now_crc;
				mark_sync = now_sync;
				mark_glitch = openvlc_edge_glitches;
				mark_long = openvlc_edge_long_total;
				mark_fail_timing =
					openvlc_phy_dbg_sfdsync_fail_timing;
				mark_fail_no_sfd =
					openvlc_phy_dbg_sfdsync_fail_no_sfd;
				mark_fail_preamble =
					openvlc_phy_dbg_sfdsync_fail_preamble;
				mark_fail_parse =
					openvlc_phy_dbg_sfdsync_fail_parse;
#if OPENVLC_RX_DIAG_LOG
				openvlc_platform_log(
					"COMP AUTO lock thr=%lu thr_mv=%lu score=%ld\r\n",
					(unsigned long)best_thr,
					(unsigned long)OPENVLC_DAC_TO_MV(best_thr),
					(long)best_score);
#endif
				return;
			}

			candidate_thr += OPENVLC_COMP_AUTO_STEP;
			comp_auto_apply_threshold(candidate_thr);
			mark_ms = now_ms;
			mark_bursts = now_bursts;
			mark_ok = now_ok;
			mark_crc = now_crc;
			mark_sync = now_sync;
			mark_glitch = openvlc_edge_glitches;
			mark_long = openvlc_edge_long_total;
			mark_fail_timing =
				openvlc_phy_dbg_sfdsync_fail_timing;
			mark_fail_no_sfd =
				openvlc_phy_dbg_sfdsync_fail_no_sfd;
			mark_fail_preamble =
				openvlc_phy_dbg_sfdsync_fail_preamble;
			mark_fail_parse =
				openvlc_phy_dbg_sfdsync_fail_parse;
		}
		return;
	}

	{
	uint32_t window_ms = now_ms - mark_ms;
	uint32_t window_bursts = now_bursts - mark_bursts;
	uint32_t window_ok = now_ok - mark_ok;
#if OPENVLC_RX_DIAG_LOG
	uint32_t window_long = openvlc_edge_long_total - mark_long;
#endif

	/*
	 * This control function runs on every foreground-loop visit. Evaluate a
	 * locked threshold only once per real one-second window; otherwise the
	 * marks are refreshed every few microseconds, window_bursts remains zero,
	 * and poor quality can never trigger a rescan.
	 */
	if (state == AUTO_LOCKED && window_ms < 1000u)
		return;

	if (window_ok) {
		last_good_ms = now_ms;
		no_ok_activity_s = 0u;
	} else if (window_bursts && window_ms >= 1000u) {
		no_ok_activity_s++;
	}

	if (state == AUTO_IDLE) {
		if (now_bursts == 0u) {
			mark_ms = now_ms;
			mark_bursts = now_bursts;
			mark_ok = now_ok;
			mark_crc = now_crc;
			mark_sync = now_sync;
			mark_glitch = openvlc_edge_glitches;
			mark_long = openvlc_edge_long_total;
			mark_fail_timing =
				openvlc_phy_dbg_sfdsync_fail_timing;
			mark_fail_no_sfd =
				openvlc_phy_dbg_sfdsync_fail_no_sfd;
			mark_fail_preamble =
				openvlc_phy_dbg_sfdsync_fail_preamble;
			mark_fail_parse =
				openvlc_phy_dbg_sfdsync_fail_parse;
			return;
		}
	} else if (state == AUTO_LOCKED) {
		uint32_t ok_ppm = window_bursts ?
			(uint32_t)(((uint64_t)window_ok * 1000000u) /
				   window_bursts) :
			1000000u;
		bool poor_ok =
			window_bursts >= 20u &&
			ok_ppm < OPENVLC_COMP_AUTO_RESCAN_MIN_OK_PPM;
		bool bad_for_too_long =
			no_ok_activity_s >= OPENVLC_COMP_AUTO_RESCAN_BAD_S;
		bool bad_quality_for_too_long;
		bool periodic =
			OPENVLC_COMP_AUTO_PERIODIC_RESCAN_S &&
			now_ms - last_good_ms >=
				OPENVLC_COMP_AUTO_PERIODIC_RESCAN_S * 1000u;

		/* Long-run counts depend on the transmitted payload and are not a
		 * reason to disturb an otherwise valid link. Rescan only from the
		 * end-to-end decoder success ratio (or complete loss of activity). */
		if (poor_ok)
			bad_quality_s++;
		else
			bad_quality_s = 0u;
		bad_quality_for_too_long =
			bad_quality_s >= OPENVLC_COMP_AUTO_RESCAN_QUALITY_BAD_S;

		if (!bad_for_too_long && !bad_quality_for_too_long &&
		    !periodic) {
			mark_ms = now_ms;
			mark_bursts = now_bursts;
			mark_ok = now_ok;
			mark_crc = now_crc;
			mark_sync = now_sync;
			mark_glitch = openvlc_edge_glitches;
			mark_long = openvlc_edge_long_total;
			mark_fail_timing =
				openvlc_phy_dbg_sfdsync_fail_timing;
			mark_fail_no_sfd =
				openvlc_phy_dbg_sfdsync_fail_no_sfd;
			mark_fail_preamble =
				openvlc_phy_dbg_sfdsync_fail_preamble;
			mark_fail_parse =
				openvlc_phy_dbg_sfdsync_fail_parse;
			return;
		}

#if OPENVLC_RX_DIAG_LOG
		openvlc_platform_log(
			"COMP AUTO rescan reason=%lu okppm=%lu long=%lu bursts=%lu badq=%lu thr=%lu thr_mv=%lu\r\n",
			(unsigned long)(bad_for_too_long ? 1u :
					(bad_quality_for_too_long ? 2u : 3u)),
			(unsigned long)ok_ppm,
			(unsigned long)window_long,
			(unsigned long)window_bursts,
			(unsigned long)bad_quality_s,
			(unsigned long)comp_auto_current_threshold,
			(unsigned long)OPENVLC_DAC_TO_MV(
				comp_auto_current_threshold));
#endif
	}
	}

	state = AUTO_SCAN;
	candidate_thr = OPENVLC_COMP_AUTO_MIN;
	best_thr = comp_auto_clamp_threshold(OPENVLC_COMP_THRESHOLD_DAC);
	best_score = -2000000000;
	comp_auto_apply_threshold(candidate_thr);
	mark_ms = now_ms;
	mark_bursts = now_bursts;
	mark_ok = now_ok;
	mark_crc = now_crc;
	mark_sync = now_sync;
	mark_glitch = openvlc_edge_glitches;
	mark_long = openvlc_edge_long_total;
	mark_fail_timing = openvlc_phy_dbg_sfdsync_fail_timing;
	mark_fail_no_sfd = openvlc_phy_dbg_sfdsync_fail_no_sfd;
	mark_fail_preamble = openvlc_phy_dbg_sfdsync_fail_preamble;
	mark_fail_parse = openvlc_phy_dbg_sfdsync_fail_parse;
#if OPENVLC_RX_DIAG_LOG
	openvlc_platform_log("COMP AUTO start min=%lu min_mv=%lu max=%lu max_mv=%lu step=%lu step_mv=%lu dwell=%lus\r\n",
			     (unsigned long)OPENVLC_COMP_AUTO_MIN,
			     (unsigned long)OPENVLC_DAC_TO_MV(
				     OPENVLC_COMP_AUTO_MIN),
			     (unsigned long)OPENVLC_COMP_AUTO_MAX,
			     (unsigned long)OPENVLC_DAC_TO_MV(
				     OPENVLC_COMP_AUTO_MAX),
			     (unsigned long)OPENVLC_COMP_AUTO_STEP,
			     (unsigned long)OPENVLC_COMP_AUTO_STEP_MV,
			     (unsigned long)OPENVLC_COMP_AUTO_DWELL_S);
#endif
}
#endif

#if (defined(STM32H743xx) || defined(STM32H723xx)) && \
	(OPENVLC_RX_DIAG_LOG || OPENVLC_COMP_DUTY_SERVO)
/*
 * Windowed comparator duty for the diagnostic log and, when enabled, the
 * self-centering servo. Returns the duty over the elapsed window in per-mille,
 * or UINT32_MAX when there was not enough traffic to trust it. Advances the
 * caller's running marks.
 */
static uint32_t comp_duty_window(uint64_t *last_high, uint64_t *last_total)
{
	uint64_t high = openvlc_comp_duty_high_ticks - *last_high;
	uint64_t total = openvlc_comp_duty_total_ticks - *last_total;

	*last_high = openvlc_comp_duty_high_ticks;
	*last_total = openvlc_comp_duty_total_ticks;
	if (total < OPENVLC_COMP_DUTY_MIN_TICKS)
		return UINT32_MAX;
	return (uint32_t)((high * 1000u) / total);
}

#if OPENVLC_COMP_DUTY_SERVO
static uint32_t comp_duty_servo_dac = OPENVLC_COMP_THRESHOLD_DAC;
/*
 * Inner loop target. duty=50% marks the signal DC mean, but the measured
 * impossible-run minimum can sit 1-2 threshold steps off centre because
 * comparator hysteresis and propagation are asymmetric, so the slow outer loop
 * below trims this target to minimise longps - the direct measure of lost
 * transitions - instead of holding the proxy at exactly 500.
 */
static uint32_t comp_duty_target = OPENVLC_COMP_DUTY_TARGET_PERMILLE;
static uint32_t comp_duty_trim_last_ms;
static uint32_t comp_duty_trim_mark_long;
static uint32_t comp_duty_trim_mark_bursts;
static uint32_t comp_duty_trim_prev_rate = UINT32_MAX;
static int32_t comp_duty_trim_dir = 1;
volatile uint32_t openvlc_comp_duty_target_now =
	OPENVLC_COMP_DUTY_TARGET_PERMILLE;

static void comp_duty_target_trim(uint32_t now_ms)
{
#if OPENVLC_COMP_DUTY_TRIM
	uint32_t elapsed = now_ms - comp_duty_trim_last_ms;
	uint32_t bursts;
	uint32_t rate;

	if (!comp_duty_trim_last_ms) {
		comp_duty_trim_last_ms = now_ms;
		comp_duty_trim_mark_long = openvlc_edge_long_total;
		comp_duty_trim_mark_bursts = openvlc_edge_bursts_decoded;
		return;
	}
	if (elapsed < OPENVLC_COMP_DUTY_TRIM_S * 1000u)
		return;
	bursts = openvlc_edge_bursts_decoded - comp_duty_trim_mark_bursts;
	if (bursts < 16u) {
		/* Not enough traffic to judge; restart the window. */
		comp_duty_trim_last_ms = now_ms;
		comp_duty_trim_mark_long = openvlc_edge_long_total;
		comp_duty_trim_mark_bursts = openvlc_edge_bursts_decoded;
		return;
	}
	/* Impossible runs per 1000 bursts: threshold-quality objective. */
	rate = (uint32_t)(((uint64_t)(openvlc_edge_long_total -
				      comp_duty_trim_mark_long) * 1000u) /
			  bursts);
	if (comp_duty_trim_prev_rate != UINT32_MAX &&
	    rate > comp_duty_trim_prev_rate)
		comp_duty_trim_dir = -comp_duty_trim_dir; /* got worse: reverse */
	comp_duty_trim_prev_rate = rate;
	if (comp_duty_trim_dir > 0) {
		if (comp_duty_target + OPENVLC_COMP_DUTY_TRIM_STEP <=
		    OPENVLC_COMP_DUTY_TRIM_MAX)
			comp_duty_target += OPENVLC_COMP_DUTY_TRIM_STEP;
		else
			comp_duty_trim_dir = -1;
	} else {
		if (comp_duty_target >= OPENVLC_COMP_DUTY_TRIM_MIN +
					OPENVLC_COMP_DUTY_TRIM_STEP)
			comp_duty_target -= OPENVLC_COMP_DUTY_TRIM_STEP;
		else
			comp_duty_trim_dir = 1;
	}
	openvlc_comp_duty_target_now = comp_duty_target;
#if OPENVLC_RX_DIAG_LOG
	openvlc_platform_log("COMP TRIM longrate=%lu target=%lu dir=%ld\r\n",
			     (unsigned long)rate,
			     (unsigned long)comp_duty_target,
			     (long)comp_duty_trim_dir);
#endif
	comp_duty_trim_last_ms = now_ms;
	comp_duty_trim_mark_long = openvlc_edge_long_total;
	comp_duty_trim_mark_bursts = openvlc_edge_bursts_decoded;
#else
	(void)now_ms;
#endif
}

static void comp_duty_servo_poll(uint32_t duty)
{
	static bool init;

	if (!init) {
		init = true;
		comp_duty_servo_dac = OPENVLC_COMP_THRESHOLD_DAC;
		HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
				 comp_duty_servo_dac);
		return;
	}
	if (duty == UINT32_MAX)
		return; /* not enough traffic this window */

	if (duty > comp_duty_target + OPENVLC_COMP_DUTY_DEADBAND) {
		/* Too much time HIGH -> threshold below the DC mean -> raise it. */
		if (comp_duty_servo_dac + OPENVLC_COMP_AUTO_STEP <=
		    OPENVLC_COMP_AUTO_MAX)
			comp_duty_servo_dac += OPENVLC_COMP_AUTO_STEP;
		else
			return;
	} else if (duty + OPENVLC_COMP_DUTY_DEADBAND < comp_duty_target) {
		if (comp_duty_servo_dac >=
		    OPENVLC_COMP_AUTO_MIN + OPENVLC_COMP_AUTO_STEP)
			comp_duty_servo_dac -= OPENVLC_COMP_AUTO_STEP;
		else
			return;
	} else {
		return; /* within deadband: hold */
	}
	HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
			 comp_duty_servo_dac);
}
#endif
#endif

/*
 * Threshold control is functional, not diagnostic. Keep it alive when text
 * logging is disabled; older code accidentally ran AUTO/servo only from the
 * diagnostic branch.
 */
static void openvlc_stm32_rx_control_poll(uint32_t now_ms)
{
#if OPENVLC_COMP_THRESHOLD_AUTO && \
	(defined(STM32H743xx) || defined(STM32H723xx))
	comp_auto_threshold_poll(now_ms, openvlc_app_counters());
#endif
#if OPENVLC_COMP_DUTY_SERVO && \
	(defined(STM32H743xx) || defined(STM32H723xx))
	static uint32_t last_control_ms;
	static uint64_t duty_last_high;
	static uint64_t duty_last_total;

	if ((now_ms - last_control_ms) >= OPENVLC_COMP_DUTY_SERVO_MS) {
		uint32_t duty;

		last_control_ms = now_ms;
		duty = comp_duty_window(&duty_last_high, &duty_last_total);
		comp_duty_target_trim(now_ms);
		comp_duty_servo_poll(duty);
	}
#else
	(void)now_ms;
#endif
}

void openvlc_stm32_debug_poll(uint32_t now_ms)
{
	openvlc_stm32_rx_control_poll(now_ms);
#if OPENVLC_RX_DIAG_LOG
	static uint32_t last_log_ms;
	static uint32_t previous_edges;
	static uint32_t previous_glitches;
	static uint32_t previous_long_gaps;
#if OPENVLC_RX_RAW_INTERVAL_HIST
	static uint32_t previous_raw_short[5];
#endif
	static uint32_t previous_bursts;
	static uint32_t previous_seen;
	static uint32_t previous_ok;
#if OPENVLC_RX_DEEP_DEBUG_LOG && \
	(defined(STM32H743xx) || defined(STM32H723xx))
	static uint32_t previous_comp_changes;
#endif
	const openvlc_counters_t *cc;
	uint32_t elapsed_ms;
	uint32_t edge_delta;
	uint32_t glitch_delta;
	uint32_t long_delta;
	uint32_t burst_delta;
	uint32_t seen_delta;
	uint32_t ok_delta;
	uint32_t edge_rate;
	uint32_t long_rate;
	uint32_t burst_rate;
	uint32_t seen_rate;
	uint32_t ok_rate;
	uint32_t glitch_permille;
#if OPENVLC_RX_RAW_INTERVAL_HIST
	uint32_t raw_short_rate[5];
	uint32_t raw_ge24_rate;
#endif
	uint32_t decode_last_us;
	uint32_t decode_max_us;
	uint32_t threshold_dac = OPENVLC_COMP_THRESHOLD_DAC;
#if OPENVLC_RX_DEEP_DEBUG_LOG && \
	(defined(STM32H743xx) || defined(STM32H723xx))
	uint32_t comp_level = 0u;
	uint32_t comp_change_delta = 0u;
	uint32_t comp_change_rate = 0u;
#endif
#if !OPENVLC_RX_HOST_FORWARD
	bool resume_capture = false;
#endif

	elapsed_ms = now_ms - last_log_ms;
	if (elapsed_ms < OPENVLC_RX_DIAG_LOG_PERIOD_MS)
		return;
	last_log_ms = now_ms;
	edge_delta = openvlc_edge_total - previous_edges;
	glitch_delta = openvlc_edge_glitches - previous_glitches;
	long_delta = openvlc_edge_long_total - previous_long_gaps;
	previous_edges = openvlc_edge_total;
	previous_glitches = openvlc_edge_glitches;
	previous_long_gaps = openvlc_edge_long_total;
	edge_rate = (uint32_t)(((uint64_t)edge_delta * 1000u) / elapsed_ms);
	long_rate = (uint32_t)(((uint64_t)long_delta * 1000u) / elapsed_ms);
	glitch_permille = edge_delta ?
		(uint32_t)(((uint64_t)glitch_delta * 1000u) / edge_delta) : 0u;
#if OPENVLC_RX_RAW_INTERVAL_HIST
	{
		uint32_t short_rate_sum = 0u;

		for (size_t bin = 0u; bin < 5u; bin++) {
			uint32_t current = openvlc_edge_raw_short_hist[bin];
			uint32_t delta = current - previous_raw_short[bin];

			previous_raw_short[bin] = current;
			raw_short_rate[bin] = (uint32_t)(
				((uint64_t)delta * 1000u) / elapsed_ms);
			short_rate_sum += raw_short_rate[bin];
		}
		raw_ge24_rate = edge_rate > short_rate_sum ?
			edge_rate - short_rate_sum : 0u;
	}
#endif
	cc = openvlc_app_counters();
	burst_delta = openvlc_edge_bursts_decoded - previous_bursts;
	seen_delta = (cc ? cc->frames_seen : 0u) - previous_seen;
	ok_delta = (cc ? cc->frames_delivered : 0u) - previous_ok;
	burst_rate = (uint32_t)(((uint64_t)burst_delta * 1000u) / elapsed_ms);
	seen_rate = (uint32_t)(((uint64_t)seen_delta * 1000u) / elapsed_ms);
	ok_rate = (uint32_t)(((uint64_t)ok_delta * 1000u) / elapsed_ms);
	decode_last_us = (uint32_t)(((uint64_t)openvlc_decode_last_ticks *
				     1000000u) / OPENVLC_TIM2_IC_TICK_HZ);
	decode_max_us = (uint32_t)(((uint64_t)openvlc_decode_max_ticks *
				    1000000u) / OPENVLC_TIM2_IC_TICK_HZ);
#if OPENVLC_RX_DEEP_DEBUG_LOG && \
	(defined(STM32H743xx) || defined(STM32H723xx))
	comp_change_delta = openvlc_comp_level_changes - previous_comp_changes;
	comp_change_rate = (uint32_t)(((uint64_t)comp_change_delta * 1000u) /
				      elapsed_ms);
	previous_comp_changes = openvlc_comp_level_changes;
#endif
	previous_bursts = openvlc_edge_bursts_decoded;
	previous_seen = cc ? cc->frames_seen : 0u;
	previous_ok = cc ? cc->frames_delivered : 0u;
#if defined(STM32H743xx) || defined(STM32H723xx)
	{
		static uint64_t duty_last_high;
		static uint64_t duty_last_total;
		uint32_t duty = comp_duty_window(&duty_last_high,
						 &duty_last_total);

		/*
		 * Always report the windowed comparator duty and the DAC/mV it
		 * corresponds to. Validate on the bench that duty falls as the
		 * threshold (mV) rises and crosses ~500 permille at the centre
		 * before enabling OPENVLC_COMP_DUTY_SERVO.
		 */
		if (duty != UINT32_MAX)
			openvlc_platform_log(
				"COMP DUTY duty=%lu.%lu%% last=%lu.%lu%% thr_dac=%lu thr_mv=%lu\r\n",
				(unsigned long)(duty / 10u),
				(unsigned long)(duty % 10u),
				(unsigned long)(openvlc_comp_duty_permille_last /
						10u),
				(unsigned long)(openvlc_comp_duty_permille_last %
						10u),
				(unsigned long)HAL_DAC_GetValue(&hdac1,
								DAC_CHANNEL_1),
				(unsigned long)OPENVLC_DAC_TO_MV(
					HAL_DAC_GetValue(&hdac1,
							 DAC_CHANNEL_1)));
	}
	threshold_dac = HAL_DAC_GetValue(&hdac1, DAC_CHANNEL_1);
#if OPENVLC_RX_DEEP_DEBUG_LOG
	comp_level = (COMP12->SR & COMP_SR_C1VAL) != 0u ? 1u : 0u;
#endif
#endif

	/*
	 * Plain-text console output is blocking, so pause capture before writing
	 * diagnostics. Host-forward mode only queues the text; TIM2/DMA continues
	 * running and the queue is drained later from the main loop.
	 */
#if !OPENVLC_RX_HOST_FORWARD
	if (edge_capture_running) {
		edge_capture_stop();
		if (edge_burst_len)
			openvlc_edge_partial_drops++;
		edge_burst_len = 0;
		edge_burst_raw_len = 0;
		edge_burst_overflowed = false;
		edge_have_last = false;
		resume_capture = true;
	}
#endif

#if OPENVLC_COMP_THRESHOLD_SWEEP && !OPENVLC_COMP_THRESHOLD_AUTO && \
	(defined(STM32H743xx) || defined(STM32H723xx))
	{
		static uint32_t sweep_thr = OPENVLC_COMP_SWEEP_MIN;
		static uint32_t sweep_change_ms;
		static uint32_t sweep_ok_mark;
		static bool sweep_init;

		if (!sweep_init) {
			sweep_init = true;
			sweep_change_ms = now_ms;
			sweep_ok_mark = cc ? cc->frames_delivered : 0u;
			HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
					 sweep_thr);
		}
		if (now_ms - sweep_change_ms >=
		    OPENVLC_COMP_SWEEP_DWELL_S * 1000u) {
			uint32_t oknow = cc ? cc->frames_delivered : 0u;
			uint32_t prev = sweep_thr;
			uint32_t delivered = oknow - sweep_ok_mark;

			sweep_thr += OPENVLC_COMP_SWEEP_STEP;
			if (sweep_thr > OPENVLC_COMP_SWEEP_MAX)
				sweep_thr = OPENVLC_COMP_SWEEP_MIN;
			HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
					 sweep_thr);
			sweep_change_ms = now_ms;
			sweep_ok_mark = oknow;
			openvlc_platform_log("COMP SWEEP thr=%lu delivered=%lu next_thr=%lu\r\n",
					     (unsigned long)prev,
					     (unsigned long)delivered,
					     (unsigned long)sweep_thr);
		}
	}
#endif

	openvlc_platform_log("COMP ep=%lu bp=%lu sp=%lu okp=%lu gpm=%lu"
#if OPENVLC_RX_RAW_INTERVAL_HIST
			     " r07=%lu r811=%lu r1215=%lu r1619=%lu r2023=%lu r24p=%lu"
#endif
			     " gd=%lu lp=%lu hc=%lu t0=%lu t1=%lu tn=%lu trq=%lu thr=%lu seen=%lu ok=%lu crc=%lu sync=%lu hq=%lu hd=%lu rp=%lu rd=%lu hwo=%lu ovf=%lu fr=%lu fa=%lu fg=%lu sg=%lu/%lu/%lu/%lu hb=%lu db=%lu br=%lu bl=%lu bok=%lu bf=%lu bsh=%lu bst=%ld du=%lu dm=%lu sf=%lu lock=%lu ss=%lu pre=%lu pbad=%lu pse=%lu se=%lu m=%lu pe=%lu ps=%ld ft=%lu fn=%lu fp=%lu fx=%lu fc=%lu fl=%lu fo=%lu fi=%lu sc=%lu jit=%lu.%lu"
#if defined(OPENVLC_RX_STREAMING) && OPENVLC_RX_STREAMING
			     " mode=%u st=%lu/%lu stm=%lu sa=%lu sl=%lu sk=%lu sr=%lu sd=%lu slen=%lu/%lu"
#if OPENVLC_RX_STREAMING >= 4
			     " ref=%lu/%lu/%lu"
#endif
#endif
			     "\r\n",
			     (unsigned long)edge_rate,
			     (unsigned long)burst_rate,
			     (unsigned long)seen_rate,
			     (unsigned long)ok_rate,
			     (unsigned long)glitch_permille,
#if OPENVLC_RX_RAW_INTERVAL_HIST
			     (unsigned long)raw_short_rate[0],
			     (unsigned long)raw_short_rate[1],
			     (unsigned long)raw_short_rate[2],
			     (unsigned long)raw_short_rate[3],
			     (unsigned long)raw_short_rate[4],
			     (unsigned long)raw_ge24_rate,
#endif
			     (unsigned long)openvlc_edge_gaps_seen,
			     (unsigned long)long_rate,
			     (unsigned long)openvlc_phy_dbg_sps,
			     (unsigned long)openvlc_phy_dbg_track_cell0_end,
			     (unsigned long)openvlc_phy_dbg_track_cell1_end,
			     (unsigned long)openvlc_phy_dbg_track_nominal_end,
			     (unsigned long)openvlc_phy_dbg_timing_residual_peak,
			     (unsigned long)threshold_dac,
			     (unsigned long)(cc ? cc->frames_seen : 0u),
			     (unsigned long)(cc ? cc->frames_delivered : 0u),
			     (unsigned long)(cc ? cc->crc_failed : 0u),
			     (unsigned long)(cc ? cc->sync_failed : 0u),
			     (unsigned long)openvlc_host_frames_queued,
			     (unsigned long)openvlc_host_frames_dropped,
			     (unsigned long)openvlc_edge_ring_peak,
			     (unsigned long)openvlc_edge_ring_drops,
			     (unsigned long)openvlc_edge_hw_overcaptures,
			     (unsigned long)openvlc_edge_overflows,
			     (unsigned long)openvlc_frag_count,
			     (unsigned long)openvlc_frag_last_at,
			     (unsigned long)(openvlc_frag_last_gap_ticks /
					     (OPENVLC_TIM2_IC_TICK_HZ / 1000000u)),
			     (unsigned long)openvlc_edge_soft_gap_probes,
			     (unsigned long)openvlc_edge_soft_gap_completed,
			     (unsigned long)openvlc_edge_soft_gap_bridged,
			     (unsigned long)openvlc_edge_soft_gap_reused,
			     (unsigned long)openvlc_rx_hypothesis_budget,
			     (unsigned long)openvlc_rx_last_decode_budget,
			     (unsigned long)openvlc_edge_last_raw_burst_len,
			     (unsigned long)openvlc_edge_last_burst_len,
			     (unsigned long)openvlc_edge_last_ok_burst_len,
			     (unsigned long)openvlc_edge_last_fail_burst_len,
			     (unsigned long)openvlc_edge_last_short_burst_len,
			     (long)openvlc_edge_last_decode_status,
			     (unsigned long)decode_last_us,
			     (unsigned long)decode_max_us,
			     (unsigned long)openvlc_phy_dbg_sfdsync_result,
			     (unsigned long)openvlc_phy_dbg_sfdsync_lock_cell,
			     (unsigned long)openvlc_phy_dbg_sfdsync_syncs,
			     (unsigned long)openvlc_phy_dbg_sfdsync_pre_rejects,
			     (unsigned long)openvlc_phy_dbg_sfdsync_pre_badmax,
			     (unsigned long)openvlc_phy_dbg_sfdsync_pre_sfdmin,
			     (unsigned long)openvlc_phy_dbg_sfdsync_sfd_errors,
			     (unsigned long)openvlc_phy_dbg_sfdsync_mode,
			     (unsigned long)openvlc_phy_dbg_phase_edits,
			     (long)openvlc_phy_dbg_parse_status,
			     (unsigned long)openvlc_phy_dbg_sfdsync_fail_timing,
			     (unsigned long)openvlc_phy_dbg_sfdsync_fail_no_sfd,
			     (unsigned long)openvlc_phy_dbg_sfdsync_fail_preamble,
			     (unsigned long)openvlc_phy_dbg_sfdsync_fail_parse,
			     (unsigned long)openvlc_phy_dbg_sfdsync_fail_crc,
			     (unsigned long)openvlc_phy_dbg_sfdsync_fail_len,
			     (unsigned long)openvlc_phy_dbg_sfdsync_fail_overflow,
			     (unsigned long)openvlc_phy_dbg_sfdsync_fail_incomplete,
			     /*
			      * Quality is a per-packet measurement: report it only
			      * when at least one frame was delivered in this log
			      * window, and consume the snapshot afterwards. A
			      * stale snapshot must never masquerade as a live
			      * measurement.
			      */
			     (unsigned long)(openvlc_last_quality_valid && ok_delta ?
					     openvlc_last_quality.link_quality : 0u),
			     (unsigned long)((openvlc_last_quality_valid && ok_delta ?
					      openvlc_last_quality.timing_jitter_x1000 : 0u) / 10u),
			     (unsigned long)((openvlc_last_quality_valid && ok_delta ?
					      openvlc_last_quality.timing_jitter_x1000 : 0u) % 10u)
#if defined(OPENVLC_RX_STREAMING) && OPENVLC_RX_STREAMING
			     ,
			     (unsigned)OPENVLC_RX_STREAMING,
			     (unsigned long)openvlc_stream_shadow_ok,
			     (unsigned long)openvlc_stream_shadow_seen,
			     (unsigned long)openvlc_stream_shadow_mismatch,
			     (unsigned long)openvlc_stream_dbg_absorbed,
			     (unsigned long)openvlc_stream_dbg_toolong,
			     (unsigned long)openvlc_stream_dbg_skipped,
			     (unsigned long)openvlc_stream_dbg_rearm,
			     (unsigned long)openvlc_stream_dbg_desync,
			     (unsigned long)openvlc_stream_len_ok_snapshot(),
			     (unsigned long)openvlc_stream_len_bad_snapshot()
#endif
#if OPENVLC_RX_STREAMING >= 4
			     ,(unsigned long)openvlc_ref_dbg_anomaly,
			     (unsigned long)openvlc_ref_dbg_acquired,
			     (unsigned long)openvlc_ref_dbg_reanchor
#endif
			     );
	if (!ok_delta)
		openvlc_last_quality_valid = 0u;
#if OPENVLC_RX_DEEP_DEBUG_LOG && \
	(defined(STM32H743xx) || defined(STM32H723xx))
	openvlc_platform_log("COMPHW level=%lu swchg=%lu swchgps=%lu samples=%lu test0hi=%lu testfhi=%lu dor0=%lu dorf=%lu dac_cr=0x%08lx dac_mcr=0x%08lx cfgr=0x%08lx sr=0x%08lx pb0mode=%lu pc5mode=%lu pc5af=%lu tim_cr1=0x%08lx ccmr2=0x%08lx ccer=0x%08lx dier=0x%08lx tim_sr=0x%08lx cnt=%lu tisel=0x%08lx dma_cr=0x%08lx ndtr=%lu dmamux=%lu\r\n",
			     (unsigned long)comp_level,
			     (unsigned long)openvlc_comp_level_changes,
			     (unsigned long)comp_change_rate,
			     (unsigned long)openvlc_comp_level_samples,
			     (unsigned long)openvlc_comp_test_zero_high,
			     (unsigned long)openvlc_comp_test_full_high,
			     (unsigned long)openvlc_comp_test_zero_dor,
			     (unsigned long)openvlc_comp_test_full_dor,
			     (unsigned long)DAC1->CR,
			     (unsigned long)DAC1->MCR,
			     (unsigned long)COMP1->CFGR,
			     (unsigned long)COMP12->SR,
			     (unsigned long)(GPIOB->MODER & 3u),
			     (unsigned long)((GPIOC->MODER >> (5u * 2u)) & 3u),
			     (unsigned long)((GPIOC->AFR[0] >> (5u * 4u)) & 15u),
			     (unsigned long)TIM2->CR1,
			     (unsigned long)TIM2->CCMR2,
			     (unsigned long)TIM2->CCER,
			     (unsigned long)TIM2->DIER,
			     (unsigned long)TIM2->SR,
			     (unsigned long)TIM2->CNT,
			     (unsigned long)TIM2->TISEL,
			     (unsigned long)DMA1_Stream1->CR,
			     (unsigned long)DMA1_Stream1->NDTR,
			     (unsigned long)(DMAMUX1_Channel1->CCR &
					     DMAMUX_CxCR_DMAREQ_ID));
#endif
	/* Per-interval watermark: reset after each report so the COMP line
	 * shows the ring backlog of the LAST second, not an all-time maximum
	 * dominated by the startup flood. */
	openvlc_edge_ring_peak = 0;
#if OPENVLC_RX_RATE_LOG
	openvlc_platform_log("RXRATE burstps=%lu seenps=%lu okps=%lu dec_us=%lu decmax_us=%lu gap_us=%u/%u sg=%lu/%lu/%lu/%lu\r\n",
			     (unsigned long)burst_rate,
			     (unsigned long)seen_rate,
			     (unsigned long)ok_rate,
			     (unsigned long)decode_last_us,
			     (unsigned long)decode_max_us,
			     (unsigned int)OPENVLC_EDGE_GAP_US,
			     (unsigned int)OPENVLC_EDGE_HARD_GAP_US,
			     (unsigned long)openvlc_edge_soft_gap_probes,
			     (unsigned long)openvlc_edge_soft_gap_completed,
			     (unsigned long)openvlc_edge_soft_gap_bridged,
			     (unsigned long)openvlc_edge_soft_gap_reused);
#endif
	openvlc_decode_max_ticks = 0u;
#if OPENVLC_RX_DEEP_DEBUG_LOG
	openvlc_platform_log("FRAG count=%lu last_at=%lu last_gap_us=%lu\r\n",
			     (unsigned long)openvlc_frag_count,
			     (unsigned long)openvlc_frag_last_at,
			     (unsigned long)(openvlc_frag_last_gap_ticks /
					     (OPENVLC_TIM2_IC_TICK_HZ / 1000000u)));
	openvlc_platform_log("PHY status=%ld stage=%lu pstat=%ld payload=%lu len_raw=%lu sps=%lu edges=%lu\r\n",
			     (long)openvlc_app_last_status(),
			     (unsigned long)openvlc_phy_dbg_stage,
			     (long)openvlc_phy_dbg_parse_status,
			     (unsigned long)openvlc_phy_dbg_payload_len,
			     (unsigned long)openvlc_phy_dbg_len_raw,
			     (unsigned long)openvlc_phy_dbg_sps,
			     (unsigned long)openvlc_phy_dbg_sample_len);
	openvlc_platform_log("SFDSYNC single=%lu cell=%lu/%lu train=%lu sfderr=%lu split=%lu syncs=%lu pre_rej=%lu relock=%lu mode=%lu phase_edits=%lu maxbits=%lu lenraw=%lu res=%lu lock=%lu\r\n",
			     (unsigned long)openvlc_phy_dbg_sfdsync_single,
			     (unsigned long)openvlc_phy_dbg_sfdsync_cell0,
			     (unsigned long)openvlc_phy_dbg_sfdsync_cell1,
			     (unsigned long)openvlc_phy_dbg_sfdsync_train,
			     (unsigned long)openvlc_phy_dbg_sfdsync_sfd_errors,
			     (unsigned long)openvlc_phy_dbg_sfdsync_split,
			     (unsigned long)openvlc_phy_dbg_sfdsync_syncs,
			     (unsigned long)openvlc_phy_dbg_sfdsync_pre_rejects,
			     (unsigned long)openvlc_phy_dbg_sfdsync_relocks,
			     (unsigned long)openvlc_phy_dbg_sfdsync_mode,
			     (unsigned long)openvlc_phy_dbg_phase_edits,
			     (unsigned long)openvlc_phy_dbg_sfdsync_maxbits,
			     (unsigned long)openvlc_phy_dbg_sfdsync_lenraw,
			     (unsigned long)openvlc_phy_dbg_sfdsync_result,
			     (unsigned long)openvlc_phy_dbg_sfdsync_lock_cell);
#endif
#if !OPENVLC_RX_HOST_FORWARD
	if (resume_capture && edge_capture_restart_synced() != 0) {
		openvlc_platform_log(
			"COMP ERROR: capture restart after diagnostics failed\r\n");
		Error_Handler();
	}
#endif
#else
	(void)now_ms;
#endif
}
#if (defined(STM32H743xx) || defined(STM32H723xx)) && OPENVLC_RX_HOST_FORWARD
/*
 * Queue records while the optical burst is being decoded. The main loop
 * restarts TIM2/DMA before transmitting them, so serial output cannot extend
 * the optical receiver's blind interval.
 *
 * Wire format:
 *   A5 5A C3 | version=1 | type | sequence(BE) | length(BE) |
 *   payload | crc16_ccitt(version..payload, BE)
 */
/*
 * Each queue slot holds the COMPLETE wire frame, built at enqueue time in
 * main-loop context. The interrupt path then only has to start the next
 * transfer: no copies, no CRC in IRQ context, and the in-flight buffer is the
 * queue slot itself (popped only on completion).
 */
typedef struct {
	uint16_t wire_len;
	uint8_t wire[OPENVLC_MAX_PAYLOAD_BYTES + 11u];
} openvlc_host_record_t;

/* ~29 KB at depth 32: keep it in RAM_D1 with the other large CPU buffers. */
static openvlc_host_record_t host_queue[OPENVLC_HOST_QUEUE_DEPTH] OPENVLC_RX_BUFFER;
static uint16_t host_next_sequence;
static volatile uint8_t host_queue_head;
static volatile uint8_t host_queue_tail;
static volatile uint8_t host_queue_count;
static volatile bool host_tx_busy;

/*
 * Start transmitting the record at the queue tail. Must be called either from
 * a USART3/DMA completion ISR or with interrupts masked, with host_tx_busy
 * already true.
 * The record is popped by the completion callback, not here, so the buffer
 * stays valid for the whole transfer.
 */
static void host_tx_start(void)
{
	openvlc_host_record_t *record = &host_queue[host_queue_tail];

#if OPENVLC_ENABLE_DCACHE
	/*
	 * host_queue[] lives in the cacheable final 64 KB of AXI SRAM. DMA1 sees
	 * main memory rather than dirty D-cache lines, so publish the complete wire
	 * record before handing it to USART3 TX DMA.
	 */
	{
		uintptr_t start = (uintptr_t)record->wire & ~(uintptr_t)31u;
		uintptr_t end = ((uintptr_t)record->wire + record->wire_len + 31u) &
				~(uintptr_t)31u;

		SCB_CleanDCache_by_Addr((uint32_t *)start,
					   (int32_t)(end - start));
		__DSB();
	}
#endif
	if (HAL_UART_Transmit_DMA(&huart3, record->wire,
				  record->wire_len) != HAL_OK) {
		/* Drop this record and give up until the next poll. */
		openvlc_host_tx_errors++;
		if (++host_queue_tail >= OPENVLC_HOST_QUEUE_DEPTH)
			host_queue_tail = 0u;
		host_queue_count--;
		host_tx_busy = false;
	} else {
		/* Only transfer-complete/error events are useful for normal-mode TX. */
		__HAL_DMA_DISABLE_IT(huart3.hdmatx, DMA_IT_HT);
	}
}

/*
 * Transfer complete: pop the sent record and chain the next one immediately
 * from IRQ context. DMA removes the former per-byte TXE interrupt load while
 * keeping the UART wire fully utilised independently of main-loop cadence.
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
	if (huart->Instance != USART3)
		return;
	openvlc_host_frames_sent++;
	if (++host_queue_tail >= OPENVLC_HOST_QUEUE_DEPTH)
		host_queue_tail = 0u;
	host_queue_count--;
	if (host_queue_count)
		host_tx_start();
	else
		host_tx_busy = false;
}

/*
 * CubeMX does not enable the USART3 interrupt (the .ioc has it off), so the
 * IRQ handler and the NVIC enable live here, where they survive code
 * regeneration. Priority is low: the TX path is not time-critical compared to
 * the capture path.
 */
void USART3_IRQHandler(void)
{
	uint32_t isr;
	uint32_t rx_errors;

	openvlc_diag_usart3_irq_count++;
	openvlc_diag_usart3_irq_phase = 1u;
	isr = USART3->ISR;
	openvlc_diag_usart3_last_isr = isr;

	/* RX uses permanent circular DMA. Consume receive errors here and reserve
	 * HAL_UART_IRQHandler() for the final DMA-TX TC event. */
	rx_errors = isr & (USART_ISR_PE | USART_ISR_FE |
			   USART_ISR_NE | USART_ISR_ORE);
	if (rx_errors != 0u) {
		uint32_t clear = 0u;

		openvlc_diag_usart3_error_count++;
		if ((rx_errors & USART_ISR_PE) != 0u)
			clear |= UART_CLEAR_PEF;
		if ((rx_errors & USART_ISR_FE) != 0u)
			clear |= UART_CLEAR_FEF;
		if ((rx_errors & USART_ISR_NE) != 0u)
			clear |= UART_CLEAR_NEF;
		if ((rx_errors & USART_ISR_ORE) != 0u) {
			clear |= UART_CLEAR_OREF;
			openvlc_diag_usart3_overrun_count++;
		}
		__HAL_UART_CLEAR_FLAG(&huart3, clear);
		huart3.ErrorCode = HAL_UART_ERROR_NONE;
	}
	openvlc_diag_usart3_irq_phase = 3u;
	HAL_UART_IRQHandler(&huart3);
	openvlc_diag_usart3_irq_phase = 0u;
}

static bool host_frame_enqueue(uint8_t type, const uint8_t *data, uint16_t len)
{
	openvlc_host_record_t *record;
	uint16_t crc;
	uint16_t sequence = host_next_sequence++;
	uint32_t primask;

	/*
	 * The sequence number advances even when the record is dropped, so the
	 * bridge's seq_gap counter directly measures records lost here.
	 */
	if ((!data && len) || len > OPENVLC_MAX_PAYLOAD_BYTES ||
	    host_queue_count >= OPENVLC_HOST_QUEUE_DEPTH) {
		openvlc_host_frames_dropped++;
		return false;
	}
	/*
	 * The head slot is free (count < depth) and the ISR only ever touches
	 * the tail, so the wire frame can be built without locking.
	 */
	record = &host_queue[host_queue_head];
	record->wire[0] = 0xA5u;
	record->wire[1] = 0x5Au;
	record->wire[2] = 0xC3u;
	record->wire[3] = 0x01u;
	record->wire[4] = type;
	record->wire[5] = (uint8_t)(sequence >> 8);
	record->wire[6] = (uint8_t)sequence;
	record->wire[7] = (uint8_t)(len >> 8);
	record->wire[8] = (uint8_t)len;
	if (len)
		memcpy(&record->wire[9], data, len);
	crc = openvlc_crc16_ccitt(&record->wire[3], (size_t)len + 6u);
	record->wire[9u + len] = (uint8_t)(crc >> 8);
	record->wire[10u + len] = (uint8_t)crc;
	record->wire_len = (uint16_t)(len + 11u);
	/* Publish the slot: head/count are shared with UART/DMA completion ISRs. */
	primask = __get_PRIMASK();
	__disable_irq();
	if (++host_queue_head >= OPENVLC_HOST_QUEUE_DEPTH)
		host_queue_head = 0u;
	host_queue_count++;
	if (!primask)
		__enable_irq();
	openvlc_host_frames_queued++;
	return true;
}

#if OPENVLC_RX_CAPTURE
#define OPENVLC_HOST_TYPE_CAPTURE 0x03u
#define OPENVLC_CAPTURE_MAGIC 0x4f564354u
#define OPENVLC_CAPTURE_VERSION 2u
#define OPENVLC_CAPTURE_BEGIN 1u
#define OPENVLC_CAPTURE_DATA 2u
#define OPENVLC_CAPTURE_END 3u
#define OPENVLC_CAPTURE_BEGIN_LEN 88u
#define OPENVLC_CAPTURE_DATA_HEADER_LEN 16u
#define OPENVLC_CAPTURE_END_LEN 20u

static void capture_put_u16(uint8_t *out, uint16_t value)
{
	out[0] = (uint8_t)(value >> 8);
	out[1] = (uint8_t)value;
}

static void capture_put_u32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)(value >> 24);
	out[1] = (uint8_t)(value >> 16);
	out[2] = (uint8_t)(value >> 8);
	out[3] = (uint8_t)value;
}

static void capture_put_prefix(uint8_t *out, uint8_t kind)
{
	capture_put_u32(out, OPENVLC_CAPTURE_MAGIC);
	out[4] = OPENVLC_CAPTURE_VERSION;
	out[5] = kind;
}

static void rx_capture_dump_poll(void)
{
	static uint32_t last_emit_ms;
	uint8_t record[OPENVLC_CAPTURE_DATA_HEADER_LEN +
		       2u * OPENVLC_RX_CAPTURE_CHUNK_INTERVALS];
	uint32_t now_ms;
	uint16_t length = 0u;

	if (rx_capture_state == RX_CAPTURE_ARMED ||
	    rx_capture_state == RX_CAPTURE_DONE)
		return;
	now_ms = HAL_GetTick();
	if (now_ms - last_emit_ms < OPENVLC_RX_CAPTURE_PERIOD_MS)
		return;
	/*
	 * Preserve two slots for a decoded IP packet and its ordinary health log.
	 * If the UART queue is momentarily busy, retain the current dump offset
	 * and retry later; the trace can therefore never contain silent holes.
	 */
	if (host_queue_count >= OPENVLC_HOST_QUEUE_DEPTH - 2u)
		return;

	switch (rx_capture_state) {
	case RX_CAPTURE_BEGIN:
		capture_put_prefix(record, OPENVLC_CAPTURE_BEGIN);
		record[6] = rx_capture_trigger;
		record[7] = (rx_capture_clipped ? 1u : 0u) |
			(OPENVLC_RX_CAPTURE_RAW ? 2u : 0u);
		capture_put_u32(&record[8], rx_capture_id);
		capture_put_u32(&record[12], OPENVLC_TIM2_IC_TICK_HZ);
		capture_put_u32(&record[16], rx_capture_count);
		capture_put_u32(&record[20], rx_capture_count + 1u);
		capture_put_u32(&record[24],
				(uint32_t)rx_capture_status);
		capture_put_u32(&record[28],
				(uint32_t)rx_capture_parse_status);
		capture_put_u32(&record[32], OPENVLC_PHY_RATE_KBPS);
		capture_put_u32(&record[36],
				rx_capture_threshold_dac);
		capture_put_u32(&record[40],
				(rx_capture_threshold_dac * 3300u +
				 2047u) / 4095u);
		capture_put_u32(&record[44], rx_capture_t0);
		capture_put_u32(&record[48], rx_capture_t1);
		capture_put_u32(&record[52], rx_capture_nominal);
		capture_put_u32(&record[56],
				rx_capture_residual_q8);
		capture_put_u32(&record[60], rx_capture_syncs);
		capture_put_u32(&record[64], rx_capture_mode);
		capture_put_u32(&record[68],
				rx_capture_hypothesis_budget);
		capture_put_u32(&record[72], rx_capture_lock);
		capture_put_u32(&record[76], rx_capture_len_raw);
		capture_put_u32(&record[80],
				rx_capture_snapshot_ms);
		capture_put_u32(&record[84], rx_capture_hash);
		length = OPENVLC_CAPTURE_BEGIN_LEN;
		break;
	case RX_CAPTURE_DATA: {
		uint32_t start = rx_capture_position;
		uint32_t remaining = rx_capture_count - start;
		uint16_t emitted = remaining >
			OPENVLC_RX_CAPTURE_CHUNK_INTERVALS ?
			OPENVLC_RX_CAPTURE_CHUNK_INTERVALS :
			(uint16_t)remaining;

		capture_put_prefix(record, OPENVLC_CAPTURE_DATA);
		capture_put_u16(&record[6], emitted);
		capture_put_u32(&record[8], rx_capture_id);
		capture_put_u32(&record[12], start);
		for (uint16_t i = 0u; i < emitted; i++) {
			capture_put_u16(
				&record[OPENVLC_CAPTURE_DATA_HEADER_LEN +
					2u * i],
				rx_capture_intervals[rx_capture_position]);
			rx_capture_position++;
		}
		length = (uint16_t)(OPENVLC_CAPTURE_DATA_HEADER_LEN +
				    2u * emitted);
		break;
	}
	case RX_CAPTURE_END:
		capture_put_prefix(record, OPENVLC_CAPTURE_END);
		record[6] = (rx_capture_clipped ? 1u : 0u) |
			(OPENVLC_RX_CAPTURE_RAW ? 2u : 0u);
		record[7] = 0u;
		capture_put_u32(&record[8], rx_capture_id);
		capture_put_u32(&record[12], rx_capture_count);
		capture_put_u32(&record[16], rx_capture_hash);
		length = OPENVLC_CAPTURE_END_LEN;
		break;
	default:
		return;
	}
	if (!host_frame_enqueue(OPENVLC_HOST_TYPE_CAPTURE, record, length))
		return;

	last_emit_ms = now_ms;
	if (rx_capture_state == RX_CAPTURE_BEGIN)
		rx_capture_state = RX_CAPTURE_DATA;
	else if (rx_capture_state == RX_CAPTURE_DATA &&
		 rx_capture_position >= rx_capture_count)
		rx_capture_state = RX_CAPTURE_END;
	else if (rx_capture_state == RX_CAPTURE_END) {
		/*
		 * The END record is already copied into the host queue, so the
		 * snapshot buffer can be reused safely. Start a fresh frame-spacing
		 * window so adjacent captures cannot dominate either population.
		 */
		rx_capture_frame_gap = 0u;
		rx_capture_state = rx_capture_all_quotas_full() ?
			RX_CAPTURE_DONE : RX_CAPTURE_ARMED;
	}
}
#endif
#endif

void openvlc_stm32_host_poll(void)
{
#if (defined(STM32H743xx) || defined(STM32H723xx)) && OPENVLC_RX_HOST_FORWARD
	uint32_t primask;

#if OPENVLC_RX_CAPTURE
	rx_capture_dump_poll();
#endif
	/*
	 * Only the FIRST record of an idle period is kicked from here; after
	 * that the completion interrupt chains the queue on its own.
	 */
	if (host_tx_busy || !host_queue_count)
		return;
	primask = __get_PRIMASK();
	__disable_irq();
	if (!host_tx_busy && host_queue_count) {
		host_tx_busy = true;
		host_tx_start();
	}
	if (!primask)
		__enable_irq();
#endif
}

void openvlc_platform_log(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if ((size_t)n >= sizeof(buf))
		n = (int)sizeof(buf) - 1;
#if defined(STM32H743xx) || defined(STM32H723xx)
#if OPENVLC_RX_HOST_FORWARD
	/*
	 * Keep one queue slot available for a decoded IP packet. A success log is
	 * emitted immediately before openvlc_platform_on_packet(), so allowing
	 * logs to fill the queue could discard useful data while retaining only
	 * its diagnostic line.
	 */
	if (OPENVLC_HOST_QUEUE_DEPTH > 1u &&
	    host_queue_count < OPENVLC_HOST_QUEUE_DEPTH - 1u)
		(void)host_frame_enqueue(0x02u, (const uint8_t *)buf,
					 (uint16_t)n);
	else
		openvlc_host_frames_dropped++;
#else
	HAL_UART_Transmit(&huart3, (uint8_t *)buf, (uint16_t)n, 100);
#endif
#else
	fwrite(buf, 1, (size_t)n, stdout);
#endif
}

void openvlc_platform_on_packet(const openvlc_packet_t *packet,
				const openvlc_quality_t *quality)
{
	if (quality) {
		openvlc_last_quality = *quality;
		openvlc_last_quality_valid = 1u;
	}
#if (defined(STM32H743xx) || defined(STM32H723xx)) && OPENVLC_RX_HOST_FORWARD
#ifdef OPENVLC_TX_SRC_ADDR
	/*
	 * A frame carrying our own TX source address is our own light coupled
	 * back into the local photodiode. Forwarding it would let the Pi route
	 * it back out tun0 -> re-transmission -> a traffic feedback loop that
	 * inflates the TX load and destabilises the LED. Drop it here.
	 * NOTE: the two transceivers MUST therefore use different
	 * OPENVLC_TX_SRC_ADDR values (e.g. 7 and 8, dst mirrored).
	 */
	if (packet->src == OPENVLC_TX_SRC_ADDR) {
		openvlc_rx_self_dropped++;
		return;
	}
#endif
	/* Forward the decoded IP datagram to the Pi bridge -> tun0. */
	(void)host_frame_enqueue(0x01u, packet->payload, packet->payload_len);
#else
	openvlc_platform_log("payload_len=%u first=%02x\r\n",
			     packet->payload_len,
			     packet->payload_len ? packet->payload[0] : 0);
#endif
}
