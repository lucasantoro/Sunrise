#include "openvlc_stm32_tx_hal.h"

#include "openvlc_app.h" /* openvlc_platform_log */

#include <string.h>

#if defined(__GNUC__)
#define OPENVLC_WEAK __attribute__((weak))
#define OPENVLC_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#else
#define OPENVLC_WEAK
#define OPENVLC_STATIC_ASSERT(condition, message) \
	typedef char openvlc_static_assertion[(condition) ? 1 : -1]
#endif

#if defined(__has_include)
#if __has_include("openvlc_board.h")
#include "openvlc_board.h"
#endif
#endif

#ifndef OPENVLC_STM32_TX_INTERFRAME_GUARD_US
#define OPENVLC_STM32_TX_INTERFRAME_GUARD_US 0u
#endif
#ifndef OPENVLC_STM32_TX_TARGET_PERIOD_US
#define OPENVLC_STM32_TX_TARGET_PERIOD_US 0u
#endif

#if defined(STM32H743xx) || defined(STM32H723xx)
#include "stm32h7xx_hal.h"
#include "main.h"
extern TIM_HandleTypeDef htim1;
extern void Error_Handler(void);
#endif

#ifndef OPENVLC_TX_GPIO_PORT
#ifdef OPENVLC_TX_GPIO_Port
#define OPENVLC_TX_GPIO_PORT OPENVLC_TX_GPIO_Port
#else
#define OPENVLC_TX_GPIO_PORT GPIOA
#endif
#endif

#ifndef OPENVLC_TX_GPIO_PIN
#ifdef OPENVLC_TX_Pin
#define OPENVLC_TX_GPIO_PIN OPENVLC_TX_Pin
#else
#define OPENVLC_TX_GPIO_PIN GPIO_PIN_6
#endif
#endif

/*
 * RAM_D2 jitter experiment. OPENVLC_TX_DMA_IN_D2 places the DMA cell buffers in
 * RAM_D2 instead of RAM_D1. This was an early placement experiment; production
 * uses DMA2 for TX and keeps the full-size buffers in cacheable RAM_D1 with
 * explicit clean-before-DMA coherency.
 * Two MAX-size slots do not fit in 32 KB RAM_D2, so this also shrinks the
 * buffer: use it ONLY with the small fixed-payload test (OPENVLC_TX_HOST_MODE=0,
 * ~128-byte payload). Revert it for full-size / host traffic.
 */
#if defined(OPENVLC_TX_DMA_IN_D2) && OPENVLC_TX_DMA_IN_D2
#ifndef OPENVLC_STM32_TX_DMA_WORDS
#define OPENVLC_STM32_TX_DMA_WORDS 3072u   /* 4 OC slots x 6 KB = 24 KB < 32 KB */
#endif
#define OPENVLC_TX_BUFFER_SECTION ".dma_buffer"
#elif defined(OPENVLC_TX_DMA_NONCACHEABLE) && OPENVLC_TX_DMA_NONCACHEABLE
#ifndef OPENVLC_STM32_TX_DMA_WORDS
#define OPENVLC_STM32_TX_DMA_WORDS \
	(OPENVLC_MAX_SYMBOLS + OPENVLC_STM32_TX_WARMUP_CELLS + \
	 OPENVLC_STM32_TX_GAP_CELLS + 8u)
#endif
#define OPENVLC_TX_BUFFER_SECTION ".tx_dma_buffer"
#else
#ifndef OPENVLC_STM32_TX_DMA_WORDS
#define OPENVLC_STM32_TX_DMA_WORDS \
	(OPENVLC_MAX_SYMBOLS + OPENVLC_STM32_TX_WARMUP_CELLS + \
	 OPENVLC_STM32_TX_GAP_CELLS + 8u)
#endif
#define OPENVLC_TX_BUFFER_SECTION ".rx_buffer"
#endif

#if defined(OPENVLC_TX_USE_TIMER_OC) && OPENVLC_TX_USE_TIMER_OC
typedef uint16_t openvlc_tx_dma_word_t;
#ifndef OPENVLC_STM32_TX_SLOT_COUNT
#define OPENVLC_STM32_TX_SLOT_COUNT 4u
#endif
#else
typedef uint32_t openvlc_tx_dma_word_t;
#ifndef OPENVLC_STM32_TX_SLOT_COUNT
#define OPENVLC_STM32_TX_SLOT_COUNT 2u
#endif
#endif
#define OPENVLC_STM32_TX_DMA_WORDS_ALIGNED \
	((OPENVLC_STM32_TX_DMA_WORDS + 7u) & ~7u)

#if defined(OPENVLC_TX_USE_TIMER_OC) && OPENVLC_TX_USE_TIMER_OC
#define OPENVLC_TX_TIM_DMA_ID TIM_DMA_ID_CC4
#define OPENVLC_TX_TIM_DMA_SOURCE TIM_DMA_CC4
OPENVLC_STATIC_ASSERT(OPENVLC_STM32_TX_GAP_CELLS >= 3u,
		      "timer OC completion requires at least three low gap cells");
#else
#define OPENVLC_TX_TIM_DMA_ID TIM_DMA_ID_UPDATE
#define OPENVLC_TX_TIM_DMA_SOURCE TIM_DMA_UPDATE
#endif

#if defined(__GNUC__) && (defined(STM32H743xx) || defined(STM32H723xx))
#define OPENVLC_TX_BUFFER \
	__attribute__((section(OPENVLC_TX_BUFFER_SECTION), aligned(32)))
#else
#define OPENVLC_TX_BUFFER
#endif

typedef enum {
	TX_SLOT_FREE = 0,
	TX_SLOT_PREPARING,
	TX_SLOT_READY,
	TX_SLOT_ACTIVE,
} tx_slot_state_t;

typedef struct {
	volatile uint8_t state;
	uint32_t word_len;
	uint32_t cell_ticks;
	uint32_t generation;
	uint32_t enqueue_order;
	uint32_t high_words;
	uint32_t low_words;
	uint32_t word_checksum;
} tx_slot_t;

#if defined(OPENVLC_TX_DMA_NONCACHEABLE) && OPENVLC_TX_DMA_NONCACHEABLE
/*
 * The FLASH linker and MPU reserve exactly 96 KB at 0x24028000 for all TX
 * cell streams. Fail at compile time when payload, warm-up or slot changes
 * exceed that contract instead of reporting a later section overlap.
 */
OPENVLC_STATIC_ASSERT(
	OPENVLC_STM32_TX_SLOT_COUNT *
	OPENVLC_STM32_TX_DMA_WORDS_ALIGNED *
	sizeof(openvlc_tx_dma_word_t) <= 96u * 1024u,
	"TX DMA slots exceed the 96 KB non-cacheable MPU window");
#endif

static openvlc_tx_dma_word_t
	tx_dma_words[OPENVLC_STM32_TX_SLOT_COUNT]
		    [OPENVLC_STM32_TX_DMA_WORDS_ALIGNED] OPENVLC_TX_BUFFER;
static tx_slot_t tx_slots[OPENVLC_STM32_TX_SLOT_COUNT];
static volatile int tx_busy;
static volatile int8_t tx_active_slot = -1;
static openvlc_stm32_tx_stats_t tx_stats;
static int8_t tx_last_slot = -1;
static uint32_t tx_last_generation;
static uint32_t tx_generation;
static uint32_t tx_enqueue_order;
static uint32_t tx_last_started_order;
static volatile uint32_t tx_active_start_cycles;
static uint32_t tx_completion_baseline_cycles;
/* Line-cell period used to shape the inter-frame idle square wave. */
static uint32_t tx_idle_cell_ticks = OPENVLC_STM32_TX_CELL_TICKS;
/* Set while the inter-frame idle waveform is driving the TX pin. */
static volatile uint8_t tx_idle_active;
/*
 * Set by the DMA-completion ISR when a stream frame ends; consumed by
 * openvlc_stm32_tx_idle_poll() once OPENVLC_TX_IDLE_GAP_US of darkness has
 * elapsed. Deferring the keep-alive start is what gives the receiver an idle
 * interval to segment the burst on - see OPENVLC_TX_IDLE_GAP_US.
 */
static volatile uint8_t tx_idle_pending;
static volatile uint32_t tx_last_complete_cycles;
static volatile uint8_t tx_guard_pending;
static volatile uint8_t tx_guard_counted;
static volatile uint8_t tx_guard_timer_armed;
#if OPENVLC_TX_HW_DIAG
static openvlc_stm32_tx_hw_diag_t tx_hw_diag;
static openvlc_stm32_tx_hw_diag_t tx_hw_first_fault;
static uint32_t tx_diag_last_generation;
static uint32_t tx_diag_last_ndtr;
static uint32_t tx_diag_last_progress_cycles;
static uint32_t tx_diag_last_sample_cycles;

#define OPENVLC_TX_DMA_STREAM0_ERROR_FLAGS \
	(DMA_LISR_FEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_TEIF0)
#define OPENVLC_TX_DIAG_SAMPLE_CYCLES \
	(SystemCoreClock / 10000u) /* 100 us, low enough overhead for full duplex. */
#define OPENVLC_TX_DIAG_STALL_CYCLES \
	(SystemCoreClock / 2000u) /* 500 us: 1000 complete line cells at 2 MHz. */
#endif

#if defined(STM32H743xx) || defined(STM32H723xx)
static uint32_t tx_irq_lock(void)
{
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	return primask;
}

static void tx_irq_unlock(uint32_t primask)
{
	if (primask == 0u)
		__enable_irq();
}

static uint32_t tx_cycle_count(void)
{
	return DWT->CYCCNT;
}
#else
static uint32_t tx_irq_lock(void)
{
	return 0u;
}

static void tx_irq_unlock(uint32_t primask)
{
	(void)primask;
}

static uint32_t tx_cycle_count(void)
{
	return 0u;
}
#endif

#if defined(STM32H743xx) || defined(STM32H723xx)
static void tx_guard_timer_stop(void)
{
	TIM6->DIER = 0u;
	TIM6->CR1 = 0u;
	TIM6->SR = 0u;
	tx_guard_timer_armed = 0u;
}

static void tx_guard_timer_arm(uint32_t remaining_cycles)
{
	uint32_t cycles_per_us = SystemCoreClock / 1000000u;
	uint32_t remaining_us;

	if (!remaining_cycles || !cycles_per_us)
		return;
	remaining_us =
		(remaining_cycles + cycles_per_us - 1u) / cycles_per_us;
	if (!remaining_us)
		remaining_us = 1u;
	if (remaining_us > 0xffffu)
		remaining_us = 0xffffu;

	/*
	 * TIM6 is otherwise unused. Its APB1 timer clock is the same 192 MHz
	 * kernel used by the OpenVLC timer profile. Run it at 1 MHz in one-pulse
	 * mode so the guard deadline can wake TX even while thread mode is inside
	 * the synchronous RX decoder.
	 */
	TIM6->CR1 = 0u;
	TIM6->DIER = 0u;
	TIM6->PSC = (OPENVLC_STM32_TX_TIMER_HZ / 1000000u) - 1u;
	TIM6->ARR = remaining_us - 1u;
	TIM6->CNT = 0u;
	TIM6->EGR = TIM_EGR_UG;
	TIM6->SR = 0u;
	TIM6->DIER = TIM_DIER_UIE;
	tx_guard_timer_armed = 1u;
	TIM6->CR1 = TIM_CR1_OPM | TIM_CR1_CEN;
}
#endif

#if (defined(STM32H743xx) || defined(STM32H723xx)) && OPENVLC_TX_HW_DIAG
static void tx_read_hw_snapshot(openvlc_stm32_tx_hw_diag_t *out)
{
	DMA_HandleTypeDef *hdma = htim1.hdma[OPENVLC_TX_TIM_DMA_ID];
	DMA_Stream_TypeDef *stream =
		(hdma && hdma->Instance) ?
		(DMA_Stream_TypeDef *)hdma->Instance : NULL;
	int slot = tx_active_slot;

	memset(out, 0, sizeof(*out));
	out->busy = tx_busy ? 1u : 0u;
	out->active_slot = slot;
	if (slot >= 0 && slot < (int)OPENVLC_STM32_TX_SLOT_COUNT) {
		out->active_generation = tx_slots[slot].generation;
		out->active_words = tx_slots[slot].word_len;
	}
	out->tim_cr1 = TIM1->CR1;
	out->tim_dier = TIM1->DIER;
	out->tim_sr = TIM1->SR;
	out->tim_cnt = TIM1->CNT;
	out->tim_arr = TIM1->ARR;
	out->tim_ccr1 = TIM1->CCR1;
	out->tim_ccr4 = TIM1->CCR4;
	out->tim_ccer = TIM1->CCER;
	out->tim_bdtr = TIM1->BDTR;
	if (stream) {
		out->dma_cr = stream->CR;
		out->dma_ndtr = stream->NDTR;
		out->dma_par = stream->PAR;
		out->dma_m0ar = stream->M0AR;
		out->dma_fcr = stream->FCR;
	}
	out->dma_lisr = DMA2->LISR;
	if (hdma && hdma->DMAmuxChannel)
		out->dmamux_ccr = hdma->DMAmuxChannel->CCR;
	if (hdma && hdma->DMAmuxChannelStatus)
		out->dmamux_csr = hdma->DMAmuxChannelStatus->CSR;
	out->gpio_moder = OPENVLC_TX_GPIO_PORT->MODER;
	out->gpio_ospeedr = OPENVLC_TX_GPIO_PORT->OSPEEDR;
	out->gpio_pupdr = OPENVLC_TX_GPIO_PORT->PUPDR;
	out->gpio_idr = OPENVLC_TX_GPIO_PORT->IDR;
	out->gpio_odr = OPENVLC_TX_GPIO_PORT->ODR;
	out->gpio_afrh = OPENVLC_TX_GPIO_PORT->AFR[1];
}

static uint32_t tx_hw_fault_flags(const openvlc_stm32_tx_hw_diag_t *s)
{
	DMA_HandleTypeDef *hdma = htim1.hdma[OPENVLC_TX_TIM_DMA_ID];
	uint32_t faults = 0u;
	uint32_t pin_shift = POSITION_VAL(OPENVLC_TX_GPIO_PIN) * 2u;
	uint32_t af_shift = (POSITION_VAL(OPENVLC_TX_GPIO_PIN) - 8u) * 4u;
	uint32_t expected_m0ar = 0u;
	uint32_t completion_pending =
		s->busy && s->dma_ndtr == 0u &&
		((s->dma_lisr & DMA_LISR_TCIF0) != 0u);

	if (s->busy) {
		if (s->active_slot < 0 ||
		    s->active_slot >= (int32_t)OPENVLC_STM32_TX_SLOT_COUNT)
			faults |= OPENVLC_TX_HW_FAULT_SLOT;
		else
			expected_m0ar = (uint32_t)(uintptr_t)
				&tx_dma_words[s->active_slot][1];
		if ((s->tim_cr1 & TIM_CR1_CEN) == 0u)
			faults |= OPENVLC_TX_HW_FAULT_TIMER_STOPPED;
		/* In normal mode the stream clears EN at NDTR=0 before its completion
		 * IRQ runs. Do not report that short, legitimate hand-off as a fault;
		 * the independent progress monitor still flags it if it lasts 500 us. */
		if (!completion_pending && (s->dma_cr & DMA_SxCR_EN) == 0u)
			faults |= OPENVLC_TX_HW_FAULT_DMA_STOPPED;
		if ((s->tim_dier & OPENVLC_TX_TIM_DMA_SOURCE) == 0u)
			faults |= OPENVLC_TX_HW_FAULT_DMA_REQUEST;
#if !defined(OPENVLC_TX_SILENT_OUTPUT) || !OPENVLC_TX_SILENT_OUTPUT
		if ((s->tim_ccer & TIM_CCER_CC1E) == 0u)
			faults |= OPENVLC_TX_HW_FAULT_OUTPUT_GATE;
#endif
		if ((s->tim_bdtr & TIM_BDTR_MOE) == 0u)
			faults |= OPENVLC_TX_HW_FAULT_MOE;
		if (s->tim_arr != OPENVLC_STM32_TX_CELL_TICKS - 1u ||
		    s->tim_ccr4 != OPENVLC_STM32_TX_CELL_TICKS /
				       OPENVLC_TX_OC_DMA_PHASE_DIV)
			faults |= OPENVLC_TX_HW_FAULT_TIMING;
		if (s->tim_ccr1 != 0u &&
		    s->tim_ccr1 != OPENVLC_STM32_TX_CELL_TICKS)
			faults |= OPENVLC_TX_HW_FAULT_CCR1;
		if ((!completion_pending && s->dma_ndtr == 0u) ||
		    s->dma_ndtr >= s->active_words)
			faults |= OPENVLC_TX_HW_FAULT_NDTR;
		if (s->dma_par != (uint32_t)(uintptr_t)&TIM1->CCR1 ||
		    (expected_m0ar && s->dma_m0ar != expected_m0ar))
			faults |= OPENVLC_TX_HW_FAULT_ADDRESS;
	} else {
#if defined(OPENVLC_TX_IDLE_KEEPALIVE) && OPENVLC_TX_IDLE_KEEPALIVE
		/*
		 * Idle keep-alive deliberately leaves TIM1 and CH1 running between
		 * frames, so CEN/CC1E/MOE are expected to be set here. Only a DMA
		 * stream still enabled without an active slot is a real fault.
		 */
		if ((s->dma_cr & DMA_SxCR_EN) != 0u)
			faults |= OPENVLC_TX_HW_FAULT_OUTPUT_GATE;
#else
		if ((s->tim_cr1 & TIM_CR1_CEN) != 0u ||
		    (s->dma_cr & DMA_SxCR_EN) != 0u ||
		    (s->tim_ccer & TIM_CCER_CC1E) != 0u ||
		    (s->tim_bdtr & TIM_BDTR_MOE) != 0u)
			faults |= OPENVLC_TX_HW_FAULT_OUTPUT_GATE;
#endif
	}
	if (((s->gpio_moder >> pin_shift) & 3u) != 2u ||
	    ((s->gpio_afrh >> af_shift) & 15u) != GPIO_AF1_TIM1)
		faults |= OPENVLC_TX_HW_FAULT_GPIO;
	if ((s->dma_lisr & OPENVLC_TX_DMA_STREAM0_ERROR_FLAGS) != 0u)
		faults |= OPENVLC_TX_HW_FAULT_DMA_ERROR;
	if (hdma && hdma->DMAmuxChannelStatus &&
	    (s->dmamux_csr &
	     hdma->DMAmuxChannelStatusMask) != 0u)
		faults |= OPENVLC_TX_HW_FAULT_DMAMUX;
	return faults;
}
#endif

static int tx_find_slot(uint8_t state)
{
	for (uint32_t i = 0; i < OPENVLC_STM32_TX_SLOT_COUNT; i++) {
		if (tx_slots[i].state == state)
			return (int)i;
	}
	return -1;
}

/*
 * READY slots form a logical FIFO even though their storage is a reusable
 * array.  Index order is not age order: a newly reused low slot must never
 * overtake an older high slot.  Unsigned age also remains correct across the
 * uint32_t wrap because at most four orders are outstanding.
 */
static int tx_find_oldest_ready(void)
{
	int oldest = -1;
	uint32_t oldest_age = 0u;

	for (uint32_t i = 0; i < OPENVLC_STM32_TX_SLOT_COUNT; i++) {
		uint32_t age;

		if (tx_slots[i].state != TX_SLOT_READY)
			continue;
		age = tx_enqueue_order - tx_slots[i].enqueue_order;
		if (oldest < 0 || age > oldest_age) {
			oldest = (int)i;
			oldest_age = age;
		}
	}
	return oldest;
}

static uint32_t tx_pipeline_depth_locked(void)
{
	uint32_t depth = 0;

	for (uint32_t i = 0; i < OPENVLC_STM32_TX_SLOT_COUNT; i++) {
		if (tx_slots[i].state != TX_SLOT_FREE)
			depth++;
	}
	return depth;
}

static void tx_complete_common(int dma_error)
{
	int slot = tx_active_slot;
	int ready;

#if defined(STM32H743xx) || defined(STM32H723xx)
	uint32_t complete_cycles = tx_cycle_count();
	DMA_HandleTypeDef *hdma = htim1.hdma[OPENVLC_TX_TIM_DMA_ID];

	if (slot >= 0 && slot < (int)OPENVLC_STM32_TX_SLOT_COUNT &&
	    tx_slots[slot].word_len >= 2u) {
		uint32_t timer_ratio =
			SystemCoreClock / OPENVLC_STM32_TX_TIMER_HZ;
		uint32_t expected_cycles =
			((tx_slots[slot].word_len - 2u) *
			 tx_slots[slot].cell_ticks + htim1.Instance->CCR4) *
			timer_ratio;
		uint32_t duration_cycles =
			complete_cycles - tx_active_start_cycles;
		uint32_t late_cycles = duration_cycles > expected_cycles ?
			duration_cycles - expected_cycles : 0u;
		uint32_t excess_cycles;

		tx_stats.last_duration_cycles = duration_cycles;
		/*
		 * HAL_DMA_IRQHandler and exception entry add a fixed delay between
		 * the final peripheral write and this callback. Learn that baseline
		 * and report only additional scheduling jitter; the old absolute
		 * comparison falsely marked every healthy frame as late.
		 */
		if (late_cycles < tx_completion_baseline_cycles)
			tx_completion_baseline_cycles = late_cycles;
		excess_cycles =
			late_cycles - tx_completion_baseline_cycles;
		if (excess_cycles > tx_stats.max_completion_late_cycles)
			tx_stats.max_completion_late_cycles = excess_cycles;
		if (excess_cycles >
		    tx_slots[slot].cell_ticks * timer_ratio)
			tx_stats.late_completions++;
	}
	if (hdma && hdma->Instance) {
		tx_stats.last_dma_fcr =
			((DMA_Stream_TypeDef *)hdma->Instance)->FCR;
		tx_stats.last_dma_error = hdma->ErrorCode;
	}
	__HAL_TIM_DISABLE_DMA(&htim1, OPENVLC_TX_TIM_DMA_SOURCE);
#if defined(OPENVLC_TX_USE_TIMER_OC) && OPENVLC_TX_USE_TIMER_OC
	CLEAR_BIT(htim1.Instance->CR1, TIM_CR1_CEN);
#else
	(void)HAL_TIM_Base_Stop(&htim1);
#endif
#if defined(OPENVLC_TX_USE_TIMER_OC) && OPENVLC_TX_USE_TIMER_OC
#if defined(OPENVLC_TX_IDLE_KEEPALIVE) && OPENVLC_TX_IDLE_KEEPALIVE
	/*
	 * Idle keep-alive. Going dark here is what lets the receiver AGC ramp to
	 * full gain and chatter on its own noise floor, which buries the next
	 * frame's preamble/SFD. Instead leave TIM1 free-running as a plain 50%
	 * square wave at the line-cell rate: DMA is already stopped above, so no
	 * cell data is consumed and the output is a constant alternating pattern
	 * identical to the warm-up. It carries no framing and cannot be mistaken
	 * for a packet, but it holds the AGC at its operating point.
	 * tx_arm_and_start() rewrites ARR/CCR1 and restarts the counter for the
	 * next frame, so no extra teardown is needed here.
	 *
	 * Only hold the light on for an actual STREAM. tx_last_complete_cycles is
	 * still the PREVIOUS completion at this point, so the gap below is the
	 * real inter-frame interval. Isolated packets - the bridge emits about one
	 * control frame per second even when the user is not transmitting - must
	 * leave the LED dark, otherwise it visibly blinks once per second and
	 * illuminates the local photodiode for no benefit: a lone frame has no
	 * successor whose AGC settling we could protect.
	 */
	{
		uint32_t cycles_per_ms = SystemCoreClock / 1000u;
		uint32_t gap = tx_cycle_count() - tx_last_complete_cycles;
		bool streaming =
			tx_last_complete_cycles != 0u && cycles_per_ms != 0u &&
			gap <= (OPENVLC_TX_IDLE_STREAM_GAP_MS * cycles_per_ms);

		/*
		 * Go dark either way. When streaming, only ARM the keep-alive:
		 * openvlc_stm32_tx_idle_poll() turns the light back on after
		 * OPENVLC_TX_IDLE_GAP_US, and that short darkness is the idle
		 * interval the receiver segments the burst on. Starting it here
		 * (as this code used to) leaves the line modulating without a
		 * break, so the keep-alive lands inside the frame's own burst.
		 */
		CLEAR_BIT(htim1.Instance->CCER, TIM_CCER_CC1E);
		CLEAR_BIT(htim1.Instance->BDTR, TIM_BDTR_MOE);
		CLEAR_BIT(htim1.Instance->CCMR1, TIM_CCMR1_OC1PE);
		htim1.Instance->CCR1 = 0u;
		SET_BIT(htim1.Instance->CCMR1, TIM_CCMR1_OC1PE);
		tx_idle_active = 0u;
		tx_idle_pending = streaming ? 1u : 0u;
	}
#else
	/*
	 * DMA completes while CH1 is already inside the low inter-frame tail.
	 * Disconnect the timer output; the TX-pin pulldown preserves the low level.
	 */
	CLEAR_BIT(htim1.Instance->CCER, TIM_CCER_CC1E);
	CLEAR_BIT(htim1.Instance->BDTR, TIM_BDTR_MOE);
	CLEAR_BIT(htim1.Instance->CCMR1, TIM_CCMR1_OC1PE);
	htim1.Instance->CCR1 = 0u;
	SET_BIT(htim1.Instance->CCMR1, TIM_CCMR1_OC1PE);
#endif
#else
	HAL_GPIO_WritePin(OPENVLC_TX_GPIO_PORT, OPENVLC_TX_GPIO_PIN,
			  GPIO_PIN_RESET);
#endif
#endif
	if (dma_error)
		tx_stats.dma_errors++;
	else
		tx_stats.completed++;
	if (slot >= 0 && slot < (int)OPENVLC_STM32_TX_SLOT_COUNT)
		tx_slots[slot].state = TX_SLOT_FREE;
	tx_active_slot = -1;
	tx_busy = 0;
	tx_last_complete_cycles = tx_cycle_count();
	tx_guard_pending =
		OPENVLC_STM32_TX_INTERFRAME_GUARD_US != 0u ||
		OPENVLC_STM32_TX_TARGET_PERIOD_US != 0u;
	tx_guard_counted = 0u;
	tx_guard_timer_armed = 0u;
	ready = tx_find_oldest_ready();
	if (!dma_error && ready < 0)
		tx_stats.pipeline_empty++;
	/* Notify the host layer without doing encoder work in IRQ context. */
	openvlc_stm32_tx_slot_freed();
}

OPENVLC_WEAK void openvlc_stm32_tx_slot_freed(void)
{
}

#if defined(STM32H743xx) || defined(STM32H723xx)
static void tx_dma_complete(DMA_HandleTypeDef *hdma)
{
	uint32_t started_before;

	(void)hdma;
	started_before = tx_stats.started;
	tx_complete_common(0);
	/*
	 * RX burst decoding can keep the main loop busy longer than an optical
	 * frame. Chain an already encoded slot here so TIM1 does not sit idle
	 * waiting for the next foreground service call. A configured real-time
	 * guard deliberately forbids IRQ chaining; the foreground poll starts the
	 * next slot after the guard deadline instead.
	 */
	openvlc_stm32_tx_service();
	if (tx_stats.started != started_before)
		tx_stats.isr_chained++;
}

static void tx_dma_error(DMA_HandleTypeDef *hdma)
{
	(void)hdma;
	tx_complete_common(1);
}

#if !defined(OPENVLC_TX_DMA_NONCACHEABLE) || !OPENVLC_TX_DMA_NONCACHEABLE
static void tx_clean_dcache(const openvlc_tx_dma_word_t *words,
			    size_t word_len)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
	uintptr_t start;
	uintptr_t end;

	/*
	 * Only run cache maintenance when the D-cache is actually enabled. With
	 * it disabled the DMA buffer is already coherent. Production enables
	 * D-cache, so clean the completed TX slot before DMA2 reads it.
	 */
	if ((SCB->CCR & SCB_CCR_DC_Msk) == 0u)
		return;

	start = (uintptr_t)words;
	end = start + word_len * sizeof(words[0]);
	start &= ~(uintptr_t)31u;
	end = (end + 31u) & ~(uintptr_t)31u;
	SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
#else
	(void)words;
	(void)word_len;
#endif
}
#endif

/*
 * Arm TIM1 CH4 DMA to clock out one prepared TX slot.
 */
static int tx_arm_and_start(int slot)
{
	DMA_HandleTypeDef *hdma = htim1.hdma[OPENVLC_TX_TIM_DMA_ID];
	openvlc_tx_dma_word_t *words = tx_dma_words[slot];
	uint32_t word_len = tx_slots[slot].word_len;
	uint32_t cell_ticks = tx_slots[slot].cell_ticks;
	openvlc_tx_dma_word_t *dma_words = words;
	uint32_t dma_word_len = word_len;

	if (!hdma || !hdma->Instance || word_len == 0u)
		return -3;
	/* Keep the inter-frame idle waveform at this frame's line-cell rate. */
	if (cell_ticks)
		tx_idle_cell_ticks = cell_ticks;
	/* A real frame takes the timer over from any idle waveform. */
	tx_idle_active = 0u;
	tx_stats.last_dma_words = word_len;
	tx_stats.last_high_words = tx_slots[slot].high_words;
	tx_stats.last_low_words = tx_slots[slot].low_words;
	tx_stats.last_word_checksum = tx_slots[slot].word_checksum;
	hdma->XferCpltCallback = tx_dma_complete;
	hdma->XferErrorCallback = tx_dma_error;
	/*
	 * The slot was cleaned before it changed from PREPARING to READY.
	 * Nothing may modify it while READY/ACTIVE, so starting a chained slot
	 * from the DMA-complete IRQ needs no cache maintenance here. In the old
	 * path this cleaned a ~24 kB frame inside a priority-1 interrupt and could
	 * stall comparator-ring draining long enough to lose RX frames.
	 */

	__HAL_TIM_DISABLE_DMA(&htim1, OPENVLC_TX_TIM_DMA_SOURCE);
#if defined(OPENVLC_TX_USE_TIMER_OC) && OPENVLC_TX_USE_TIMER_OC
	CLEAR_BIT(htim1.Instance->CR1, TIM_CR1_CEN);
#else
	(void)HAL_TIM_Base_Stop(&htim1);
#endif
	__HAL_TIM_SET_AUTORELOAD(&htim1, cell_ticks - 1u);
	__HAL_TIM_SET_COUNTER(&htim1, 0u);
#if defined(OPENVLC_TX_USE_TIMER_OC) && OPENVLC_TX_USE_TIMER_OC
	/*
	 * Seed the active CCR with cell 0. CH4 DMA writes cell N+1 into the CCR1
	 * preload during cell N, so the next update changes the TX pin exactly on the
	 * timer boundary. DMA completion occurs in the low tail because the frame
	 * builder appends 32+ low cells; stopping there cannot clip payload data.
	 *
	 * Dropping CC1E here parks PE9 on its pulldown while CCR1 is seeded and the
	 * DMA is rebuilt, then reconnects it: electrically that is a runt on the
	 * optical line at every frame start. The reference LiFi_Manchester design
	 * avoids it by never touching CC1E/MOE after init and toggling only CEN,
	 * so keeping the channel connected looked strictly better.
	 *
	 * MEASURED 2026-08-06: it is NOT. Leaving CC1E set made the link worse -
	 * decode 97.0% -> 95.2%, CRC 2.55% -> 4.33%, and the 16..23-tick raw bins
	 * grew systematically (r2023 3.1-3.7k -> 4.1-7.5k, r1619 0.03-0.2k ->
	 * 0.13-1.8k) with the local TX idle, i.e. the damage arrived from the peer
	 * running the same build. Reason: with the channel still connected the pin
	 * HOLDS the seed level through the whole DMA rebuild, so whenever cell 0 is
	 * high the LED stays lit for microseconds at every frame start. That is a
	 * DC step into the AGC 125 times a second - an optical disturbance, which
	 * costs more than the sub-microsecond electrical runt it removes. Parking
	 * the pin low is the cheaper of the two.
	 *
	 * Set OPENVLC_TX_KEEP_OUTPUT_ENABLED=1 to re-test the alternative.
	 */
#if !defined(OPENVLC_TX_KEEP_OUTPUT_ENABLED) || !OPENVLC_TX_KEEP_OUTPUT_ENABLED
	CLEAR_BIT(htim1.Instance->CCER, TIM_CCER_CC1E);
#endif
	CLEAR_BIT(htim1.Instance->CCMR1, TIM_CCMR1_OC1PE);
	htim1.Instance->CCR1 = words[0];
	SET_BIT(htim1.Instance->CCMR1, TIM_CCMR1_OC1PE);
	htim1.Instance->CCR4 =
		cell_ticks / OPENVLC_TX_OC_DMA_PHASE_DIV;
	if (htim1.Instance->CCR4 == 0u)
		htim1.Instance->CCR4 = 1u;
	dma_words = words + 1u;
	dma_word_len = word_len - 1u;
	if (dma_word_len == 0u)
		return -3;
#endif
	htim1.Instance->SR = 0u;
	if (HAL_DMA_Start_IT(hdma, (uint32_t)dma_words,
#if defined(OPENVLC_TX_USE_TIMER_OC) && OPENVLC_TX_USE_TIMER_OC
			     (uint32_t)&htim1.Instance->CCR1,
#else
			     (uint32_t)&OPENVLC_TX_GPIO_PORT->BSRR,
#endif
			     dma_word_len) != HAL_OK) {
		tx_stats.dma_errors++;
		return -4;
	}
	if ((htim1.Instance->ARR != cell_ticks - 1u) ||
	    ((((DMA_Stream_TypeDef *)hdma->Instance)->FCR &
	      DMA_SxFCR_DMDIS) == 0u) ||
	    ((((DMA_Stream_TypeDef *)hdma->Instance)->CR &
	      DMA_SxCR_PL) != DMA_PRIORITY_VERY_HIGH))
		tx_stats.config_errors++;
	__HAL_TIM_ENABLE_DMA(&htim1, OPENVLC_TX_TIM_DMA_SOURCE);
#if defined(OPENVLC_TX_USE_TIMER_OC) && OPENVLC_TX_USE_TIMER_OC
	/*
	 * Keep these writes adjacent. Calling HAL_TIM_Base_Start after enabling
	 * CH1 would extend the first high preamble cell by the HAL call latency.
	 */
#if defined(OPENVLC_TX_SILENT_OUTPUT) && OPENVLC_TX_SILENT_OUTPUT
	/* DIAGNOSTIC build: run the complete TX chain (encode, TIM1, DMA, IRQs,
	 * UART) with CH1 never connected to the pin, so the TX output stays low. */
#else
	SET_BIT(htim1.Instance->CCER, TIM_CCER_CC1E);
#endif
	/* TIM1 is an advanced-control timer: MOE gates CH1 independently of CC1E. */
	SET_BIT(htim1.Instance->BDTR, TIM_BDTR_MOE);
	tx_active_start_cycles = tx_cycle_count();
	SET_BIT(htim1.Instance->CR1, TIM_CR1_CEN);
#else
	if (HAL_TIM_Base_Start(&htim1) != HAL_OK) {
		__HAL_TIM_DISABLE_DMA(&htim1, OPENVLC_TX_TIM_DMA_SOURCE);
		(void)HAL_DMA_Abort(hdma);
		tx_stats.dma_errors++;
		return -5;
	}
#endif
	tx_stats.started++;
	return 0;
}
#endif

void openvlc_stm32_tx_pin_probe(openvlc_stm32_tx_pin_probe_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
#if defined(STM32H743xx) || defined(STM32H723xx)
	{
		/*
		 * Sample the TX pad directly. 4096 back-to-back IDR reads span
		 * roughly 30-60 us (~60-120 line cells at 2 Mcell/s), enough to
		 * measure toggling and duty. Interrupts stay enabled: a rare
		 * preemption only merges two runs, which barely perturbs the
		 * aggregate edge/high counts. Read-only - never gates the timer.
		 */
		const uint32_t samples = 4096u;
		volatile uint32_t *idr = &OPENVLC_TX_GPIO_PORT->IDR;
		uint32_t pin = (uint32_t)OPENVLC_TX_GPIO_PIN;
		uint32_t prev = (*idr & pin) ? 1u : 0u;
		uint32_t high = prev;
		uint32_t edges = 0u;
		uint32_t start;
		uint32_t i;

		out->busy_start = tx_busy ? 1u : 0u;
		out->active_slot = tx_active_slot;
		start = tx_cycle_count();
		for (i = 1u; i < samples; i++) {
			uint32_t s = (*idr & pin) ? 1u : 0u;

			high += s;
			edges += (s ^ prev);
			prev = s;
		}
		out->cycles = tx_cycle_count() - start;
		out->busy_end = tx_busy ? 1u : 0u;
		out->samples = samples;
		out->high_samples = high;
		out->edges = edges;
	}
#endif
}

void openvlc_stm32_tx_banner(void)
{
#if defined(STM32H743xx) || defined(STM32H723xx)
	const openvlc_tx_profile_t *profile = openvlc_tx_default_profile();
	uint32_t cell_ticks = profile ? profile->cell_ticks : 0u;
	uint32_t cell_hz = cell_ticks ?
		OPENVLC_STM32_TX_TIMER_HZ / cell_ticks : 0u;

	openvlc_platform_log(
		"TXCFG build=%s-%s node=%u phy=%uk budget=%u silent=%u "
		"timhz=%lu cellticks=%lu cellrate_khz=%lu period_us=%u guard_us=%u "
		"slots=%u wordbytes=%u bufbase=%08lx bufbytes=%lu noncache=%u\r\n",
		__DATE__, __TIME__,
		(unsigned int)OPENVLC_TRANSCEIVER_NODE,
		(unsigned int)OPENVLC_PHY_RATE_KBPS,
		(unsigned int)OPENVLC_STM32_TX_PROFILE_BUDGET,
		(unsigned int)(OPENVLC_TX_SILENT_OUTPUT ? 1u : 0u),
		(unsigned long)OPENVLC_STM32_TX_TIMER_HZ,
		(unsigned long)cell_ticks,
		(unsigned long)(cell_hz / 1000u),
		(unsigned int)OPENVLC_STM32_TX_TARGET_PERIOD_US,
		(unsigned int)OPENVLC_STM32_TX_INTERFRAME_GUARD_US,
		(unsigned int)OPENVLC_STM32_TX_SLOT_COUNT,
		(unsigned int)sizeof(openvlc_tx_dma_word_t),
		(unsigned long)(uintptr_t)&tx_dma_words[0][0],
		(unsigned long)sizeof(tx_dma_words),
		(unsigned int)(
#if defined(OPENVLC_TX_DMA_NONCACHEABLE) && OPENVLC_TX_DMA_NONCACHEABLE
			1u
#else
			0u
#endif
			));
#endif
}

int openvlc_stm32_tx_init(void)
{
	const openvlc_tx_profile_t *profile = openvlc_tx_default_profile();

	if (!profile)
		return -1;
	memset(&tx_stats, 0, sizeof(tx_stats));
	memset(tx_slots, 0, sizeof(tx_slots));
	tx_busy = 0;
	tx_active_slot = -1;
	tx_last_slot = -1;
	tx_last_generation = 0u;
	tx_generation = 0u;
	tx_enqueue_order = 0u;
	tx_last_started_order = 0u;
	tx_active_start_cycles = 0u;
	tx_completion_baseline_cycles = UINT32_MAX;
	tx_last_complete_cycles = 0u;
	tx_guard_pending = 0u;
	tx_guard_counted = 0u;
	tx_guard_timer_armed = 0u;
	tx_stats.min_idle_cycles = UINT32_MAX;
#if OPENVLC_TX_HW_DIAG
	memset(&tx_hw_diag, 0, sizeof(tx_hw_diag));
	memset(&tx_hw_first_fault, 0, sizeof(tx_hw_first_fault));
	tx_diag_last_generation = 0u;
	tx_diag_last_ndtr = 0u;
	tx_diag_last_progress_cycles = 0u;
	tx_diag_last_sample_cycles = 0u;
#endif
	tx_stats.last_budget = profile->budget;
	tx_stats.last_cell_ticks = profile->cell_ticks;
	if (profile->cell_ticks)
		tx_idle_cell_ticks = profile->cell_ticks;
#if defined(STM32H743xx) || defined(STM32H723xx)
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0u;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	__HAL_RCC_TIM6_CLK_ENABLE();
	tx_guard_timer_stop();
	HAL_NVIC_SetPriority(TIM6_DAC_IRQn, 2u, 0u);
	HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
	HAL_GPIO_WritePin(OPENVLC_TX_GPIO_PORT, OPENVLC_TX_GPIO_PIN,
			  GPIO_PIN_RESET);
	if (htim1.Instance) {
		__HAL_TIM_SET_PRESCALER(&htim1, 0u);
		__HAL_TIM_SET_AUTORELOAD(&htim1, profile->cell_ticks - 1u);
		__HAL_TIM_SET_COUNTER(&htim1, 0u);
	}
#endif
	return 0;
}

int openvlc_stm32_tx_busy(void)
{
	return tx_busy;
}

uint32_t openvlc_stm32_tx_pipeline_depth(void)
{
	uint32_t primask = tx_irq_lock();
	uint32_t depth = tx_pipeline_depth_locked();

	tx_irq_unlock(primask);
	return depth;
}

int openvlc_stm32_tx_can_accept(void)
{
	uint32_t primask = tx_irq_lock();
	int free_slot = tx_find_slot(TX_SLOT_FREE);

	tx_irq_unlock(primask);
	return free_slot >= 0;
}

const openvlc_stm32_tx_stats_t *openvlc_stm32_tx_stats(void)
{
	return &tx_stats;
}

void openvlc_stm32_tx_diag_poll(void)
{
#if (defined(STM32H743xx) || defined(STM32H723xx)) && OPENVLC_TX_HW_DIAG
	openvlc_stm32_tx_hw_diag_t sample;
	uint32_t now;
	uint32_t faults;
	uint32_t primask;

	now = tx_cycle_count();
	if ((now - tx_diag_last_sample_cycles) < OPENVLC_TX_DIAG_SAMPLE_CYCLES)
		return;
	tx_diag_last_sample_cycles = now;
	primask = tx_irq_lock();
	tx_read_hw_snapshot(&sample);
	tx_irq_unlock(primask);
	faults = tx_hw_fault_flags(&sample);
	if (sample.busy) {
		if (sample.active_generation != tx_diag_last_generation ||
		    sample.dma_ndtr != tx_diag_last_ndtr) {
			tx_diag_last_generation = sample.active_generation;
			tx_diag_last_ndtr = sample.dma_ndtr;
			tx_diag_last_progress_cycles = now;
		} else if ((now - tx_diag_last_progress_cycles) >
			   OPENVLC_TX_DIAG_STALL_CYCLES) {
			faults |= OPENVLC_TX_HW_FAULT_STALLED;
		}
	} else {
		tx_diag_last_generation = 0u;
		tx_diag_last_ndtr = 0u;
		tx_diag_last_progress_cycles = now;
	}
	sample.samples = tx_hw_diag.samples + 1u;
	sample.fault_events = tx_hw_diag.fault_events;
	sample.latched_fault_flags = tx_hw_diag.latched_fault_flags;
	sample.fault_flags = faults;
	if (faults) {
		if (tx_hw_diag.fault_flags == 0u ||
		    tx_hw_diag.fault_flags != faults)
			sample.fault_events++;
		sample.latched_fault_flags |= faults;
		if (tx_hw_diag.latched_fault_flags == 0u) {
			tx_hw_first_fault = sample;
			tx_hw_first_fault.latched_fault_flags = faults;
		}
	}
	tx_hw_diag = sample;
#endif
}

void openvlc_stm32_tx_hw_snapshot(openvlc_stm32_tx_hw_diag_t *snapshot)
{
	if (!snapshot)
		return;
#if (defined(STM32H743xx) || defined(STM32H723xx)) && OPENVLC_TX_HW_DIAG
	{
		uint32_t primask = tx_irq_lock();
		*snapshot = tx_hw_diag;
		tx_irq_unlock(primask);
	}
#else
	memset(snapshot, 0, sizeof(*snapshot));
#endif
}

void openvlc_stm32_tx_hw_first_fault(openvlc_stm32_tx_hw_diag_t *snapshot)
{
	if (!snapshot)
		return;
#if (defined(STM32H743xx) || defined(STM32H723xx)) && OPENVLC_TX_HW_DIAG
	{
		uint32_t primask = tx_irq_lock();
		*snapshot = tx_hw_first_fault;
		tx_irq_unlock(primask);
	}
#else
	memset(snapshot, 0, sizeof(*snapshot));
#endif
}

void openvlc_stm32_tx_idle_poll(void)
{
#if (defined(STM32H743xx) || defined(STM32H723xx)) && \
	defined(OPENVLC_TX_IDLE_KEEPALIVE) && OPENVLC_TX_IDLE_KEEPALIVE
	uint32_t primask;
	uint32_t cycles_per_ms = SystemCoreClock / 1000u;
	uint32_t limit;

	/*
	 * Deferred keep-alive start. The frame ended, the line has been dark for
	 * OPENVLC_TX_IDLE_GAP_US - long enough for the receiver to close the
	 * burst on that gap - so re-light it to hold the AGC until the next
	 * frame. tx_busy is re-checked under PRIMASK because tx_arm_and_start()
	 * may take the timer over in between; it rewrites ARR/CCR1 itself, so
	 * losing the race simply means no keep-alive for this interval.
	 */
	if (tx_idle_pending && cycles_per_ms) {
		uint32_t cycles_per_us = cycles_per_ms / 1000u;
		uint32_t gap = OPENVLC_TX_IDLE_GAP_US * cycles_per_us;

		if (cycles_per_us &&
		    (tx_cycle_count() - tx_last_complete_cycles) >= gap) {
			primask = tx_irq_lock();
			if (tx_idle_pending && !tx_busy) {
				CLEAR_BIT(htim1.Instance->CCMR1,
					  TIM_CCMR1_OC1PE);
				/*
				 * Duty is NOT 50%: see
				 * OPENVLC_TX_IDLE_DUTY_PERCENT. A 50% square wave
				 * puts its fifth harmonic - 5 MHz at the 1 MHz cell
				 * rate - at maximum, right on the measured receive
				 * resonance. 40% nulls that harmonic exactly.
				 */
				htim1.Instance->CCR1 =
					(2u * tx_idle_cell_ticks *
					 OPENVLC_TX_IDLE_DUTY_PERCENT + 50u) / 100u;
				SET_BIT(htim1.Instance->CCMR1, TIM_CCMR1_OC1PE);
				__HAL_TIM_SET_AUTORELOAD(
					&htim1, 2u * tx_idle_cell_ticks - 1u);
				__HAL_TIM_SET_COUNTER(&htim1, 0u);
				htim1.Instance->EGR = TIM_EGR_UG;
				htim1.Instance->SR = 0u;
				SET_BIT(htim1.Instance->CCER, TIM_CCER_CC1E);
				SET_BIT(htim1.Instance->BDTR, TIM_BDTR_MOE);
				SET_BIT(htim1.Instance->CR1, TIM_CR1_CEN);
				tx_idle_active = 1u;
			}
			tx_idle_pending = 0u;
			tx_irq_unlock(primask);
		}
	}

	if (!tx_idle_active || tx_busy || !cycles_per_ms)
		return;
	limit = OPENVLC_TX_IDLE_KEEPALIVE_MS * cycles_per_ms;
	if ((tx_cycle_count() - tx_last_complete_cycles) < limit)
		return;

	/*
	 * The host has had nothing to send for OPENVLC_TX_IDLE_KEEPALIVE_MS, so
	 * stop illuminating: hold the AGC only while a stream is actually
	 * running. Re-check tx_busy under PRIMASK because a frame may be armed
	 * between the test above and the teardown.
	 */
	primask = tx_irq_lock();
	if (!tx_busy && tx_idle_active) {
		CLEAR_BIT(htim1.Instance->CR1, TIM_CR1_CEN);
		CLEAR_BIT(htim1.Instance->CCER, TIM_CCER_CC1E);
		CLEAR_BIT(htim1.Instance->BDTR, TIM_BDTR_MOE);
		CLEAR_BIT(htim1.Instance->CCMR1, TIM_CCMR1_OC1PE);
		htim1.Instance->CCR1 = 0u;
		SET_BIT(htim1.Instance->CCMR1, TIM_CCMR1_OC1PE);
		tx_idle_active = 0u;
	}
	tx_irq_unlock(primask);
#endif
}

void openvlc_stm32_tx_service(void)
{
#if defined(STM32H743xx) || defined(STM32H723xx)
	uint32_t primask;
	int slot;
	int status;

	primask = tx_irq_lock();
	if (tx_busy) {
		tx_irq_unlock(primask);
		return;
	}
	slot = tx_find_oldest_ready();
	if (slot < 0) {
		tx_irq_unlock(primask);
		return;
	}
#if defined(STM32H743xx) || defined(STM32H723xx)
	if (tx_guard_pending) {
		uint32_t now_cycles = tx_cycle_count();
		uint32_t elapsed = now_cycles - tx_last_complete_cycles;
		uint32_t guard_cycles =
			(SystemCoreClock / 1000000u) *
			OPENVLC_STM32_TX_INTERFRAME_GUARD_US;
		uint32_t period_cycles =
			(SystemCoreClock / 1000000u) *
			OPENVLC_STM32_TX_TARGET_PERIOD_US;
		int32_t guard_remaining = guard_cycles ?
			(int32_t)(tx_last_complete_cycles + guard_cycles -
				  now_cycles) : 0;
		int32_t period_remaining = period_cycles ?
			(int32_t)(tx_active_start_cycles + period_cycles -
				  now_cycles) : 0;
		uint32_t remaining_cycles = 0u;

		if (guard_remaining > 0)
			remaining_cycles = (uint32_t)guard_remaining;
		if (period_remaining > 0 &&
		    (uint32_t)period_remaining > remaining_cycles)
			remaining_cycles = (uint32_t)period_remaining;

		if (remaining_cycles != 0u) {
			if (!tx_guard_counted) {
				tx_stats.guard_waits++;
				tx_guard_counted = 1u;
			}
			if (!tx_guard_timer_armed)
				tx_guard_timer_arm(remaining_cycles);
			tx_irq_unlock(primask);
			return;
		}
		if (tx_guard_timer_armed)
			tx_guard_timer_stop();
		tx_stats.last_idle_cycles = elapsed;
		if (elapsed < tx_stats.min_idle_cycles)
			tx_stats.min_idle_cycles = elapsed;
		if (elapsed > tx_stats.max_idle_cycles)
			tx_stats.max_idle_cycles = elapsed;
		tx_guard_pending = 0u;
		tx_guard_counted = 0u;
	}
#endif
	tx_slots[slot].state = TX_SLOT_ACTIVE;
	if (tx_last_started_order != 0u &&
	    tx_slots[slot].enqueue_order != tx_last_started_order + 1u)
		tx_stats.fifo_violations++;
	tx_last_started_order = tx_slots[slot].enqueue_order;
	tx_active_slot = (int8_t)slot;
	tx_busy = 1;
	tx_irq_unlock(primask);

	status = tx_arm_and_start(slot);
	if (status != 0) {
		primask = tx_irq_lock();
		tx_slots[slot].state = TX_SLOT_FREE;
		tx_active_slot = -1;
		tx_busy = 0;
		tx_irq_unlock(primask);
	}
#else
	int slot = tx_find_oldest_ready();

	if (slot >= 0) {
		tx_slots[slot].state = TX_SLOT_FREE;
		tx_stats.started++;
		tx_stats.completed++;
	}
#endif
}

void openvlc_stm32_tx_guard_irq(void)
{
#if defined(STM32H743xx) || defined(STM32H723xx)
	if ((TIM6->SR & TIM_SR_UIF) == 0u)
		return;
	tx_guard_timer_stop();
	openvlc_stm32_tx_service();
#endif
}

int openvlc_stm32_tx_send_packet(const openvlc_packet_t *packet)
{
	const openvlc_tx_profile_t *profile = openvlc_tx_default_profile();
#if !defined(OPENVLC_TX_USE_TIMER_OC) || !OPENVLC_TX_USE_TIMER_OC
	openvlc_tx_dma_buffer_t dma;
#endif
	openvlc_status_t status;
	uint32_t primask;
	uint32_t encode_start;
	uint32_t encode_cycles;
	int slot;

	if (!profile || !packet)
		return -1;

	primask = tx_irq_lock();
	slot = tx_find_slot(TX_SLOT_FREE);
	if (slot < 0) {
		tx_stats.busy_drops++;
		tx_irq_unlock(primask);
		return -2;
	}
	tx_slots[slot].state = TX_SLOT_PREPARING;
	tx_irq_unlock(primask);

	encode_start = tx_cycle_count();

#if defined(OPENVLC_TX_USE_TIMER_OC) && OPENVLC_TX_USE_TIMER_OC
	{
		size_t word_len = 0u;
		openvlc_tx_oc_stats_t word_stats;

		status = openvlc_tx_compat_packet_to_oc_words(
			packet, profile, tx_dma_words[slot],
			OPENVLC_STM32_TX_DMA_WORDS, &word_len,
			&word_stats);
		if (status == OPENVLC_OK) {
			tx_slots[slot].word_len = (uint32_t)word_len;
			tx_slots[slot].high_words = word_stats.high_words;
			tx_slots[slot].low_words = word_stats.low_words;
			tx_slots[slot].word_checksum = word_stats.checksum;
		}
	}
#else
	dma.words = tx_dma_words[slot];
	dma.word_cap = OPENVLC_STM32_TX_DMA_WORDS;
	dma.word_len = 0;
	status = openvlc_tx_compat_packet_to_bsrr(packet, profile,
					       (uint32_t)OPENVLC_TX_GPIO_PIN,
					       (uint32_t)OPENVLC_TX_GPIO_PIN << 16,
					       &dma);
#endif
	if (status != OPENVLC_OK) {
		encode_cycles = tx_cycle_count() - encode_start;
		tx_stats.encode_errors++;
		tx_stats.last_encode_cycles = encode_cycles;
		if (encode_cycles > tx_stats.max_encode_cycles)
			tx_stats.max_encode_cycles = encode_cycles;
		primask = tx_irq_lock();
		tx_slots[slot].state = TX_SLOT_FREE;
		tx_irq_unlock(primask);
		return (int)status;
	}

#if !defined(OPENVLC_TX_USE_TIMER_OC) || !OPENVLC_TX_USE_TIMER_OC
	tx_slots[slot].word_len = (uint32_t)dma.word_len;
#endif
	tx_slots[slot].cell_ticks = profile->cell_ticks;
	/*
	 * Cacheable fallback builds clean before publishing READY. Production
	 * places all TX slots in the non-cacheable .tx_dma_buffer MPU window, so
	 * no cache writeback can contend with an active TIM1 stream.
	 */
#if (defined(STM32H743xx) || defined(STM32H723xx)) && \
	(!defined(OPENVLC_TX_DMA_NONCACHEABLE) || \
	 !OPENVLC_TX_DMA_NONCACHEABLE)
	tx_clean_dcache(tx_dma_words[slot], tx_slots[slot].word_len);
#endif
#if defined(STM32H743xx) || defined(STM32H723xx)
	/* Publish READY only after every cell store is globally visible to DMA2. */
	__DSB();
#endif
	encode_cycles = tx_cycle_count() - encode_start;
	tx_slots[slot].generation = ++tx_generation;
	tx_stats.queued++;
	tx_stats.last_encode_cycles = encode_cycles;
	if (encode_cycles > tx_stats.max_encode_cycles)
		tx_stats.max_encode_cycles = encode_cycles;
	tx_stats.last_dma_words = tx_slots[slot].word_len;
	tx_stats.last_symbols =
		tx_slots[slot].word_len - profile->gap_cells - 1u;
	tx_stats.last_budget = profile->budget;
	tx_stats.last_cell_ticks = profile->cell_ticks;
	primask = tx_irq_lock();
	tx_last_slot = (int8_t)slot;
	tx_last_generation = tx_slots[slot].generation;
	tx_slots[slot].enqueue_order = ++tx_enqueue_order;
	tx_slots[slot].state = TX_SLOT_READY;
	tx_irq_unlock(primask);

	openvlc_stm32_tx_service();
	return 0;
}

int openvlc_stm32_tx_resend(void)
{
	uint32_t primask = tx_irq_lock();
	int slot = tx_last_slot;

	if (slot < 0 ||
	    slot >= (int)OPENVLC_STM32_TX_SLOT_COUNT ||
	    tx_slots[slot].generation != tx_last_generation) {
		tx_irq_unlock(primask);
		return -1;
	}
	if (tx_slots[slot].state != TX_SLOT_FREE) {
		tx_stats.busy_drops++;
		tx_irq_unlock(primask);
		return -2;
	}
	tx_slots[slot].enqueue_order = ++tx_enqueue_order;
	tx_slots[slot].state = TX_SLOT_READY;
	tx_stats.queued++;
	tx_irq_unlock(primask);
	openvlc_stm32_tx_service();
	return 0;
}

#if defined(OPENVLC_TEST_API)
int openvlc_test_tx_select_oldest_ready(const uint32_t *orders,
					 size_t count, uint32_t newest)
{
	if (!orders || count > OPENVLC_STM32_TX_SLOT_COUNT)
		return -1;
	memset(tx_slots, 0, sizeof(tx_slots));
	tx_enqueue_order = newest;
	for (size_t i = 0; i < count; i++) {
		tx_slots[i].state = TX_SLOT_READY;
		tx_slots[i].enqueue_order = orders[i];
	}
	return tx_find_oldest_ready();
}
#endif
