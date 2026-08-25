#ifndef OPENVLC_STM32_TX_HAL_H
#define OPENVLC_STM32_TX_HAL_H

#include <stddef.h>
#include <stdint.h>

#include "openvlc_tx_compat.h"

typedef struct {
	uint32_t queued;
	uint32_t started;
	uint32_t completed;
	uint32_t isr_chained;
	uint32_t pipeline_empty;
	uint32_t guard_waits;
	uint32_t last_idle_cycles;
	uint32_t min_idle_cycles;
	uint32_t max_idle_cycles;
	uint32_t busy_drops;
	uint32_t fifo_violations;
	uint32_t encode_errors;
	uint32_t dma_errors;
	uint32_t last_encode_cycles;
	uint32_t max_encode_cycles;
	uint32_t last_symbols;
	uint32_t last_dma_words;
	uint32_t last_budget;
	uint32_t last_cell_ticks;
	uint32_t last_high_words;
	uint32_t last_low_words;
	uint32_t last_word_checksum;
	uint32_t last_duration_cycles;
	uint32_t max_completion_late_cycles;
	uint32_t late_completions;
	uint32_t config_errors;
	uint32_t last_dma_fcr;
	uint32_t last_dma_error;
} openvlc_stm32_tx_stats_t;

/*
 * Live register snapshot of the TX path. This deliberately reports the
 * peripheral state rather than inferring pin activity from completed buffers.
 * A non-zero fault_flags value is diagnostic only: the monitor never restarts
 * TIM1 or DMA, so the original failure remains observable on the scope.
 */
typedef struct {
	uint32_t samples;
	uint32_t fault_events;
	uint32_t fault_flags;
	uint32_t latched_fault_flags;
	uint32_t busy;
	int32_t active_slot;
	uint32_t active_generation;
	uint32_t active_words;
	uint32_t tim_cr1;
	uint32_t tim_dier;
	uint32_t tim_sr;
	uint32_t tim_cnt;
	uint32_t tim_arr;
	uint32_t tim_ccr1;
	uint32_t tim_ccr4;
	uint32_t tim_ccer;
	uint32_t tim_bdtr;
	uint32_t dma_cr;
	uint32_t dma_ndtr;
	uint32_t dma_par;
	uint32_t dma_m0ar;
	uint32_t dma_fcr;
	uint32_t dma_lisr;
	uint32_t dmamux_ccr;
	uint32_t dmamux_csr;
	uint32_t gpio_moder;
	uint32_t gpio_ospeedr;
	uint32_t gpio_pupdr;
	uint32_t gpio_idr;
	uint32_t gpio_odr;
	uint32_t gpio_afrh;
} openvlc_stm32_tx_hw_diag_t;

#define OPENVLC_TX_HW_FAULT_SLOT          (1u << 0)
#define OPENVLC_TX_HW_FAULT_TIMER_STOPPED (1u << 1)
#define OPENVLC_TX_HW_FAULT_DMA_STOPPED   (1u << 2)
#define OPENVLC_TX_HW_FAULT_DMA_REQUEST   (1u << 3)
#define OPENVLC_TX_HW_FAULT_OUTPUT_GATE   (1u << 4)
#define OPENVLC_TX_HW_FAULT_MOE           (1u << 5)
#define OPENVLC_TX_HW_FAULT_TIMING        (1u << 6)
#define OPENVLC_TX_HW_FAULT_CCR1          (1u << 7)
#define OPENVLC_TX_HW_FAULT_GPIO          (1u << 8)
#define OPENVLC_TX_HW_FAULT_NDTR          (1u << 9)
#define OPENVLC_TX_HW_FAULT_ADDRESS       (1u << 10)
#define OPENVLC_TX_HW_FAULT_DMA_ERROR     (1u << 11)
#define OPENVLC_TX_HW_FAULT_DMAMUX        (1u << 12)
#define OPENVLC_TX_HW_FAULT_STALLED       (1u << 13)

/*
 * Wire-free physical-output probe. The register snapshot proves each CCR1 value
 * is individually valid but CANNOT see the SEQUENCE the DMA clocks out, so a
 * stream that emits mostly-zeros (dim LED) with correct RAM hi/lo and correct
 * frame duration is invisible to it. This probe tight-loop-samples the TX pin's
 * own input register (PE9 = GPIOE->IDR bit 9) for a few microseconds during a
 * live frame and reports the REAL toggle count and high fraction seen at the pad.
 *   - toggling ~50% at ~2 MHz while the LED is dim  => PE9 is correct, the fault
 *     is electrical/optical (case B/C).
 *   - stuck (edges~0, high~0 or ~samples)           => the digital output is wrong
 *     (case A: TIM1/DMA/GPIO/firmware).
 * It reads only IDR, never touches the timer or DMA, so TX keeps running.
 */
typedef struct {
	uint32_t samples;      /* IDR reads taken */
	uint32_t cycles;       /* DWT cycles spanned by the sampling window */
	uint32_t high_samples; /* reads with PE9 == 1 */
	uint32_t edges;        /* level changes between consecutive reads */
	uint32_t busy_start;   /* tx_busy at window start */
	uint32_t busy_end;     /* tx_busy at window end */
	int32_t  active_slot;
} openvlc_stm32_tx_pin_probe_t;

void openvlc_stm32_tx_pin_probe(openvlc_stm32_tx_pin_probe_t *out);
/* One-shot startup banner: build id, PHY/profile, TX-silent, clocks, DMA buffer. */
void openvlc_stm32_tx_banner(void);

int openvlc_stm32_tx_init(void);
int openvlc_stm32_tx_send_packet(const openvlc_packet_t *packet);
/* Re-transmit the last encoded packet without re-encoding (cheap repeat). */
int openvlc_stm32_tx_resend(void);
/* Start a prepared frame, if any, and report whether another can be encoded. */
void openvlc_stm32_tx_service(void);
/*
 * Stop the inter-frame idle keep-alive once the host has been quiet for
 * OPENVLC_TX_IDLE_KEEPALIVE_MS, so the LED only modulates while a stream is
 * actually running. Cheap and safe to call from the main loop every pass.
 */
void openvlc_stm32_tx_idle_poll(void);
/* TIM6 one-shot callback used to end the inter-frame scheduling guard. */
void openvlc_stm32_tx_guard_irq(void);
int openvlc_stm32_tx_can_accept(void);
uint32_t openvlc_stm32_tx_pipeline_depth(void);
int openvlc_stm32_tx_busy(void);
const openvlc_stm32_tx_stats_t *openvlc_stm32_tx_stats(void);
void openvlc_stm32_tx_diag_poll(void);
void openvlc_stm32_tx_hw_snapshot(openvlc_stm32_tx_hw_diag_t *snapshot);
void openvlc_stm32_tx_hw_first_fault(openvlc_stm32_tx_hw_diag_t *snapshot);
/*
 * Called from the TX DMA completion ISR after a slot returns to FREE. The
 * default and transceiver implementations are non-blocking; the bounded
 * foreground scheduler refills the slot on its next pass.
 */
void openvlc_stm32_tx_slot_freed(void);

#if defined(OPENVLC_TEST_API)
int openvlc_test_tx_select_oldest_ready(const uint32_t *orders,
					 size_t count, uint32_t newest);
#endif

#endif
