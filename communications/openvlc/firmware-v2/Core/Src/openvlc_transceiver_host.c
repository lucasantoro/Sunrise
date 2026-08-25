#include "openvlc_transceiver_host.h"

#include "openvlc_board.h"
#include "openvlc_config.h"
#include "openvlc_stm32_hal.h"
#include "openvlc_stm32_tx_hal.h"
#include "openvlc_types.h"
#include "usart.h"

#include <stdio.h>
#include <string.h>

#define HOST_MAGIC0 0xa5u
#define HOST_MAGIC1 0x5au
#define HOST_MAGIC2 0xc3u
#define HOST_VERSION 1u
#define HOST_TYPE_IP 1u

#ifndef OPENVLC_TX_HOST_PREPARE_PER_POLL
#define OPENVLC_TX_HOST_PREPARE_PER_POLL 2u
#endif

#if defined(__GNUC__)
#define OPENVLC_DMA_BUFFER __attribute__((section(".dma_buffer"), aligned(32)))
#else
#define OPENVLC_DMA_BUFFER
#endif

typedef enum {
	HOST_WAIT_MAGIC0 = 0,
	HOST_WAIT_MAGIC1,
	HOST_WAIT_MAGIC2,
	HOST_READ_VERSION,
	HOST_READ_TYPE,
	HOST_READ_SEQ_HI,
	HOST_READ_SEQ_LO,
	HOST_READ_LEN_HI,
	HOST_READ_LEN_LO,
	HOST_READ_PAYLOAD,
	HOST_READ_CRC_HI,
	HOST_READ_CRC_LO,
} host_parser_state_t;

typedef struct {
	host_parser_state_t state;
	uint16_t seq;
	uint16_t len;
	uint16_t payload_pos;
	uint16_t crc;
	uint16_t rx_crc;
	uint8_t payload[OPENVLC_MAX_PAYLOAD_BYTES];
} host_parser_t;

typedef struct {
	uint32_t rx_bytes;
	uint32_t frames;
	uint32_t bytes;
	uint32_t crc_errors;
	uint32_t length_errors;
	uint32_t header_errors;
	uint32_t ip_errors;
	uint32_t queue_drops;
	uint32_t seq_gaps;
	uint32_t seq_reorders;
	uint16_t last_seq;
	uint16_t last_len;
	uint8_t have_seq;
} host_stats_t;

/*
 * Packet ring between the host parser and the TX encoder. Both run inside the
 * cooperative host service (parse first, then drain), so producer and consumer are
 * the same context and cannot race.
 */
typedef struct {
	openvlc_packet_t packets[OPENVLC_TX_PACKET_QUEUE_LEN];
	volatile uint8_t head;
	volatile uint8_t tail;
	volatile uint8_t count;
} tx_queue_t;

static uint8_t host_rx_dma[OPENVLC_TX_HOST_RX_DMA_BYTES] OPENVLC_DMA_BUFFER;
/* Owned by the cooperative host service. */
static volatile size_t host_rx_pos;
static host_parser_t parser;
static host_stats_t stats;
static tx_queue_t queue;
static volatile int last_tx_result;
static volatile uint32_t host_rx_wake_events;
static volatile uint32_t host_rx_dma_restarts;
static volatile uint32_t host_rx_dma_restart_errors;
static uint32_t host_rx_last_poll_ms;
static uint32_t host_rx_max_poll_gap_ms;
static uint32_t host_rx_late_polls;
static volatile uint8_t host_service_busy;
static volatile uint32_t host_service_reentries;

static uint16_t crc16_update(uint16_t crc, uint8_t byte)
{
	uint8_t bit;

	crc ^= (uint16_t)byte << 8;
	for (bit = 0; bit < 8u; bit++)
		crc = (crc & 0x8000u) ?
			(uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
	return crc;
}

static void parser_reset(void)
{
	memset(&parser, 0, sizeof(parser));
	parser.state = HOST_WAIT_MAGIC0;
	parser.crc = 0xffffu;
}

static uint16_t valid_ip_len(const uint8_t *payload, uint16_t len)
{
	uint16_t total;

	if (!payload || len == 0u)
		return 0u;
	if ((payload[0] >> 4) == 4u) {
		uint8_t header_len;

		if (len < 20u)
			return 0u;
		header_len = (uint8_t)((payload[0] & 0x0fu) * 4u);
		total = (uint16_t)(((uint16_t)payload[2] << 8) | payload[3]);
		return (header_len >= 20u && total >= header_len && total <= len) ?
			total : 0u;
	}
	if ((payload[0] >> 4) == 6u) {
		if (len < 40u)
			return 0u;
		total = (uint16_t)(40u + (((uint16_t)payload[4] << 8) |
					 payload[5]));
		return total <= len ? total : 0u;
	}
	return 0u;
}

static void queue_ip(const uint8_t *payload, uint16_t len, uint16_t seq)
{
	openvlc_packet_t *packet;
	uint16_t ip_len = valid_ip_len(payload, len);

	if (ip_len == 0u) {
		stats.ip_errors++;
		return;
	}
	if (queue.count >= OPENVLC_TX_PACKET_QUEUE_LEN) {
		stats.queue_drops++;
		return;
	}
	if (stats.have_seq) {
		uint16_t expected = (uint16_t)(stats.last_seq + 1u);
		uint16_t delta = (uint16_t)(seq - expected);

		if (seq != expected) {
			if (delta < 0x8000u)
				stats.seq_gaps += delta;
			else
				stats.seq_reorders++;
		}
	}
	packet = &queue.packets[queue.tail];
	memset(packet, 0, sizeof(*packet));
	packet->dst = OPENVLC_TX_DST_ADDR;
	packet->src = OPENVLC_TX_SRC_ADDR;
	packet->protocol = OPENVLC_PROTOCOL_DEFAULT;
	packet->payload_len = ip_len;
	memcpy(packet->payload, payload, ip_len);
	/* Parser and encode drain both run inside the same thread-mode service, so
	 * this publish cannot race the consumer (same context, sequential). */
	queue.tail = (uint8_t)((queue.tail + 1u) % OPENVLC_TX_PACKET_QUEUE_LEN);
	queue.count++;
	stats.frames++;
	stats.bytes += ip_len;
	stats.last_seq = seq;
	stats.last_len = ip_len;
	stats.have_seq = 1u;
}

static void feed_byte(uint8_t byte)
{
	switch (parser.state) {
	case HOST_WAIT_MAGIC0:
		if (byte == HOST_MAGIC0)
			parser.state = HOST_WAIT_MAGIC1;
		break;
	case HOST_WAIT_MAGIC1:
		parser.state = byte == HOST_MAGIC1 ? HOST_WAIT_MAGIC2 :
			(byte == HOST_MAGIC0 ? HOST_WAIT_MAGIC1 : HOST_WAIT_MAGIC0);
		break;
	case HOST_WAIT_MAGIC2:
		if (byte == HOST_MAGIC2) {
			parser.crc = 0xffffu;
			parser.state = HOST_READ_VERSION;
		} else {
			stats.header_errors++;
			parser.state = byte == HOST_MAGIC0 ?
				HOST_WAIT_MAGIC1 : HOST_WAIT_MAGIC0;
		}
		break;
	case HOST_READ_VERSION:
		if (byte != HOST_VERSION) {
			stats.header_errors++;
			parser_reset();
			break;
		}
		parser.crc = crc16_update(parser.crc, byte);
		parser.state = HOST_READ_TYPE;
		break;
	case HOST_READ_TYPE:
		if (byte != HOST_TYPE_IP) {
			stats.header_errors++;
			parser_reset();
			break;
		}
		parser.crc = crc16_update(parser.crc, byte);
		parser.state = HOST_READ_SEQ_HI;
		break;
	case HOST_READ_SEQ_HI:
		parser.seq = (uint16_t)byte << 8;
		parser.crc = crc16_update(parser.crc, byte);
		parser.state = HOST_READ_SEQ_LO;
		break;
	case HOST_READ_SEQ_LO:
		parser.seq |= byte;
		parser.crc = crc16_update(parser.crc, byte);
		parser.state = HOST_READ_LEN_HI;
		break;
	case HOST_READ_LEN_HI:
		parser.len = (uint16_t)byte << 8;
		parser.crc = crc16_update(parser.crc, byte);
		parser.state = HOST_READ_LEN_LO;
		break;
	case HOST_READ_LEN_LO:
		parser.len |= byte;
		parser.crc = crc16_update(parser.crc, byte);
		parser.payload_pos = 0u;
		if (parser.len > OPENVLC_MAX_PAYLOAD_BYTES) {
			stats.length_errors++;
			parser_reset();
		} else {
			parser.state = parser.len ? HOST_READ_PAYLOAD : HOST_READ_CRC_HI;
		}
		break;
	case HOST_READ_PAYLOAD:
		parser.payload[parser.payload_pos++] = byte;
		parser.crc = crc16_update(parser.crc, byte);
		if (parser.payload_pos >= parser.len)
			parser.state = HOST_READ_CRC_HI;
		break;
	case HOST_READ_CRC_HI:
		parser.rx_crc = (uint16_t)byte << 8;
		parser.state = HOST_READ_CRC_LO;
		break;
	case HOST_READ_CRC_LO:
		parser.rx_crc |= byte;
		if (parser.rx_crc == parser.crc)
			queue_ip(parser.payload, parser.len, parser.seq);
		else
			stats.crc_errors++;
		parser_reset();
		break;
	default:
		parser_reset();
		break;
	}
}

int openvlc_transceiver_host_init(void)
{
	memset(&stats, 0, sizeof(stats));
	memset(&queue, 0, sizeof(queue));
	memset(host_rx_dma, 0, sizeof(host_rx_dma));
	parser_reset();
	host_rx_pos = 0u;
	last_tx_result = 0;
	host_rx_wake_events = 0u;
	host_rx_dma_restarts = 0u;
	host_rx_dma_restart_errors = 0u;
	host_rx_last_poll_ms = HAL_GetTick();
	host_rx_max_poll_gap_ms = 0u;
	host_rx_late_polls = 0u;
	host_service_busy = 0u;
	host_service_reentries = 0u;
	if (openvlc_stm32_tx_init() != 0)
		return -1;
	openvlc_stm32_tx_banner();
	if (HAL_UART_Receive_DMA(&huart3, host_rx_dma,
				 OPENVLC_TX_HOST_RX_DMA_BYTES) != HAL_OK)
		return -3;
	__HAL_DMA_DISABLE_IT(huart3.hdmarx, DMA_IT_HT);
	/* RX errors are non-blocking for our permanent circular DMA and are
	 * consumed by USART3_IRQHandler. The HAL error path aborts the DMA. */
	__HAL_UART_DISABLE_IT(&huart3, UART_IT_ERR);
	openvlc_platform_log(
		"TXCFG rev=txpace1 node=%u src=%u dst=%u silent=%u phy=%u "
		"budget=%u tim_hz=%lu cell=%u line_hz=%lu slots=%u dma_nc=%u "
		"phase=1/%u period_us=%u guard_us=%u\r\n",
		(unsigned int)OPENVLC_TRANSCEIVER_NODE,
		(unsigned int)OPENVLC_TX_SRC_ADDR,
		(unsigned int)OPENVLC_TX_DST_ADDR,
		(unsigned int)OPENVLC_TX_SILENT_OUTPUT,
		(unsigned int)OPENVLC_PHY_RATE_KBPS,
		(unsigned int)OPENVLC_STM32_TX_PROFILE_BUDGET,
		(unsigned long)OPENVLC_STM32_TX_TIMER_HZ,
		(unsigned int)OPENVLC_STM32_TX_CELL_TICKS,
		(unsigned long)(OPENVLC_STM32_TX_TIMER_HZ /
				OPENVLC_STM32_TX_CELL_TICKS),
		(unsigned int)OPENVLC_STM32_TX_SLOT_COUNT,
		(unsigned int)OPENVLC_TX_DMA_NONCACHEABLE,
		(unsigned int)OPENVLC_TX_OC_DMA_PHASE_DIV,
		(unsigned int)OPENVLC_STM32_TX_TARGET_PERIOD_US,
		(unsigned int)OPENVLC_STM32_TX_INTERFRAME_GUARD_US);
	return 0;
}

void openvlc_transceiver_host_poll(void)
{
	size_t dma_pos;
	uint32_t dma_active;
	uint32_t now_ms;
	uint32_t poll_gap;

	now_ms = HAL_GetTick();
	poll_gap = now_ms - host_rx_last_poll_ms;
	host_rx_last_poll_ms = now_ms;
	if (poll_gap > host_rx_max_poll_gap_ms)
		host_rx_max_poll_gap_ms = poll_gap;
	/* 8192 bytes at 2 Mbaud hold about 41 ms.  A 30 ms foreground
	 * scheduling gap is therefore actionable even before a DMA overrun. */
	if (poll_gap >= 30u)
		host_rx_late_polls++;

	/*
	 * Service a READY optical slot on every foreground pass. With the
	 * inter-frame guard enabled the completion IRQ intentionally cannot chain
	 * the next slot, and there may be no new UART bytes after several frames
	 * have already been prepared. Tying this call to parser activity would
	 * otherwise leave a READY slot dormant until the next host packet.
	 */
	openvlc_stm32_tx_service();

	dma_active = ((huart3.Instance->CR3 & USART_CR3_DMAR) != 0u) &&
		      ((((DMA_Stream_TypeDef *)huart3.hdmarx->Instance)->CR &
			DMA_SxCR_EN) != 0u);
	if (!dma_active) {
		(void)HAL_UART_AbortReceive(&huart3);
		host_rx_pos = 0u;
		if (HAL_UART_Receive_DMA(&huart3, host_rx_dma,
					 OPENVLC_TX_HOST_RX_DMA_BYTES) == HAL_OK) {
			__HAL_DMA_DISABLE_IT(huart3.hdmarx, DMA_IT_HT);
			__HAL_UART_DISABLE_IT(&huart3, UART_IT_ERR);
			host_rx_dma_restarts++;
		} else {
			host_rx_dma_restart_errors++;
		}
	}

	dma_pos = OPENVLC_TX_HOST_RX_DMA_BYTES -
		__HAL_DMA_GET_COUNTER(huart3.hdmarx);
	if (dma_pos >= OPENVLC_TX_HOST_RX_DMA_BYTES)
		dma_pos = 0u;
	if ((host_rx_pos != dma_pos) ||
	    (queue.count && openvlc_stm32_tx_can_accept())) {
		host_rx_wake_events++;
		openvlc_transceiver_host_encode_isr();
	}
}

void openvlc_transceiver_host_encode_isr(void)
{
	size_t dma_pos;
	uint32_t prepared = 0u;
	uint32_t primask = __get_PRIMASK();

	/* The normal caller is foreground, but retain the legacy software-IRQ
	 * entry safely: parser, packet queue and encoder are single-owner state. */
	__disable_irq();
	if (host_service_busy) {
		host_service_reentries++;
		if (!primask)
			__enable_irq();
		return;
	}
	host_service_busy = 1u;
	if (!primask)
		__enable_irq();

	/* Refill free DMA slots before parsing another UART burst.  This keeps
	 * already accepted packets ahead of newly arrived packets and prevents
	 * an avoidable software-queue overflow after a long optical decode. */
	openvlc_stm32_tx_service();
	while (queue.count && prepared < OPENVLC_TX_HOST_PREPARE_PER_POLL &&
	       openvlc_stm32_tx_can_accept()) {
		last_tx_result =
			openvlc_stm32_tx_send_packet(&queue.packets[queue.head]);
		if (last_tx_result != 0)
			break;
		queue.head = (uint8_t)((queue.head + 1u) %
				      OPENVLC_TX_PACKET_QUEUE_LEN);
		queue.count--;
		prepared++;
	}

	dma_pos = OPENVLC_TX_HOST_RX_DMA_BYTES -
		__HAL_DMA_GET_COUNTER(huart3.hdmarx);
	if (dma_pos >= OPENVLC_TX_HOST_RX_DMA_BYTES)
		dma_pos = 0u;
	while (host_rx_pos != dma_pos) {
		feed_byte(host_rx_dma[host_rx_pos]);
		stats.rx_bytes++;
		host_rx_pos = (host_rx_pos + 1u) % OPENVLC_TX_HOST_RX_DMA_BYTES;
	}

	while (queue.count && prepared < OPENVLC_TX_HOST_PREPARE_PER_POLL &&
	       openvlc_stm32_tx_can_accept()) {
		last_tx_result =
			openvlc_stm32_tx_send_packet(&queue.packets[queue.head]);
		if (last_tx_result != 0)
			break;
		queue.head = (uint8_t)((queue.head + 1u) %
				      OPENVLC_TX_PACKET_QUEUE_LEN);
		queue.count--;
		prepared++;
	}
	primask = __get_PRIMASK();
	__disable_irq();
	host_service_busy = 0u;
	if (!primask)
		__enable_irq();
}

void openvlc_stm32_tx_slot_freed(void)
{
	/* The foreground services the freed slot after draining comparator RX. */
}

void openvlc_transceiver_host_log(uint32_t now_ms)
{
#if OPENVLC_TX_DIAG_LOG
	static uint32_t last_log;
	static uint32_t last_started;
	const openvlc_stm32_tx_stats_t *tx;
	uint32_t elapsed_ms;
	uint32_t started_delta;
	uint32_t started_fps_x10;
#if OPENVLC_TX_HW_DIAG
	openvlc_stm32_tx_hw_diag_t hw;
	openvlc_stm32_tx_hw_diag_t first;
#endif

	if ((now_ms - last_log) < OPENVLC_TX_DIAG_LOG_PERIOD_MS)
		return;
	elapsed_ms = now_ms - last_log;
	last_log = now_ms;
	tx = openvlc_stm32_tx_stats();
	started_delta = tx->started - last_started;
	last_started = tx->started;
	started_fps_x10 = elapsed_ms ?
		(uint32_t)(((uint64_t)started_delta * 10000u +
			    elapsed_ms / 2u) / elapsed_ms) : 0u;
#if OPENVLC_TX_HW_DIAG
	openvlc_stm32_tx_hw_snapshot(&hw);
	openvlc_stm32_tx_hw_first_fault(&first);
#endif
	openvlc_platform_log(
		"TX uart=%lu frames=%lu kbytes=%lu q=%u pipe=%lu qdrop=%lu "
		"seqgap=%lu reorder=%lu crc=%lu len=%lu hdr=%lu ip=%lu started=%lu "
		"fps=%lu.%lu "
		"done=%lu chain=%lu empty=%lu wake=%lu rxrst=%lu rxrerr=%lu "
		"late=%lu maxpoll=%lu reentry=%lu fifoerr=%lu guard=%lu "
		"idle_us=%lu idlemin_us=%lu idlemax_us=%lu words=%lu "
		"hi=%lu lo=%lu hash=%08lx tx_us=%lu txlate=%lu latemax_us=%lu "
		"cfg=%lu fcr=%08lx derr=%08lx enc_us=%lu encmax_us=%lu "
		"enc=%lu dma=%lu ret=%d\r\n",
		(unsigned long)stats.rx_bytes,
		(unsigned long)stats.frames,
		(unsigned long)(stats.bytes / 1024u),
		(unsigned int)queue.count,
		(unsigned long)openvlc_stm32_tx_pipeline_depth(),
		(unsigned long)stats.queue_drops,
		(unsigned long)stats.seq_gaps,
		(unsigned long)stats.seq_reorders,
		(unsigned long)stats.crc_errors,
		(unsigned long)stats.length_errors,
		(unsigned long)stats.header_errors,
		(unsigned long)stats.ip_errors,
		(unsigned long)tx->started,
		(unsigned long)(started_fps_x10 / 10u),
		(unsigned long)(started_fps_x10 % 10u),
		(unsigned long)tx->completed,
		(unsigned long)tx->isr_chained,
		(unsigned long)tx->pipeline_empty,
		(unsigned long)host_rx_wake_events,
		(unsigned long)host_rx_dma_restarts,
		(unsigned long)host_rx_dma_restart_errors,
		(unsigned long)host_rx_late_polls,
		(unsigned long)host_rx_max_poll_gap_ms,
		(unsigned long)host_service_reentries,
		(unsigned long)tx->fifo_violations,
		(unsigned long)tx->guard_waits,
		(unsigned long)(tx->last_idle_cycles / 384u),
		(unsigned long)(tx->min_idle_cycles == UINT32_MAX ? 0u :
				tx->min_idle_cycles / 384u),
		(unsigned long)(tx->max_idle_cycles / 384u),
		(unsigned long)tx->last_dma_words,
		(unsigned long)tx->last_high_words,
		(unsigned long)tx->last_low_words,
		(unsigned long)tx->last_word_checksum,
		(unsigned long)(tx->last_duration_cycles / 384u),
		(unsigned long)tx->late_completions,
		(unsigned long)(tx->max_completion_late_cycles / 384u),
		(unsigned long)tx->config_errors,
		(unsigned long)tx->last_dma_fcr,
		(unsigned long)tx->last_dma_error,
		(unsigned long)(tx->last_encode_cycles / 384u),
		(unsigned long)(tx->max_encode_cycles / 384u),
		(unsigned long)tx->encode_errors,
		(unsigned long)tx->dma_errors,
		last_tx_result);
#if OPENVLC_TX_HW_DIAG
	openvlc_platform_log(
		"TXHW n=%lu busy=%lu slot=%ld gen=%lu words=%lu fault=%08lx "
		"latched=%08lx fev=%lu cr1=%08lx dier=%08lx sr=%08lx cnt=%lu "
		"arr=%lu ccr1=%lu ccr4=%lu ccer=%08lx bdtr=%08lx\r\n",
		(unsigned long)hw.samples,
		(unsigned long)hw.busy,
		(long)hw.active_slot,
		(unsigned long)hw.active_generation,
		(unsigned long)hw.active_words,
		(unsigned long)hw.fault_flags,
		(unsigned long)hw.latched_fault_flags,
		(unsigned long)hw.fault_events,
		(unsigned long)hw.tim_cr1,
		(unsigned long)hw.tim_dier,
		(unsigned long)hw.tim_sr,
		(unsigned long)hw.tim_cnt,
		(unsigned long)hw.tim_arr,
		(unsigned long)hw.tim_ccr1,
		(unsigned long)hw.tim_ccr4,
		(unsigned long)hw.tim_ccer,
		(unsigned long)hw.tim_bdtr);
	openvlc_platform_log(
		"TXDMA cr=%08lx ndtr=%lu par=%08lx m0=%08lx fcr=%08lx "
		"lisr=%08lx mux=%08lx muxsr=%08lx gpio_m=%08lx gpio_s=%08lx "
		"gpio_p=%08lx gpio_i=%08lx gpio_o=%08lx gpio_af=%08lx "
		"first=%08lx first_gen=%lu first_ndtr=%lu\r\n",
		(unsigned long)hw.dma_cr,
		(unsigned long)hw.dma_ndtr,
		(unsigned long)hw.dma_par,
		(unsigned long)hw.dma_m0ar,
		(unsigned long)hw.dma_fcr,
		(unsigned long)hw.dma_lisr,
		(unsigned long)hw.dmamux_ccr,
		(unsigned long)hw.dmamux_csr,
		(unsigned long)hw.gpio_moder,
		(unsigned long)hw.gpio_ospeedr,
		(unsigned long)hw.gpio_pupdr,
		(unsigned long)hw.gpio_idr,
		(unsigned long)hw.gpio_odr,
		(unsigned long)hw.gpio_afrh,
		(unsigned long)first.fault_flags,
		(unsigned long)first.active_generation,
		(unsigned long)first.dma_ndtr);
	if (first.fault_flags != 0u) {
		openvlc_platform_log(
			"TXFIRST fault=%08lx busy=%lu slot=%ld gen=%lu words=%lu "
			"cr1=%08lx dier=%08lx sr=%08lx cnt=%lu arr=%lu ccr1=%lu "
			"ccr4=%lu ccer=%08lx bdtr=%08lx dcr=%08lx ndtr=%lu "
			"par=%08lx m0=%08lx fcr=%08lx lisr=%08lx mux=%08lx "
			"muxsr=%08lx gm=%08lx gs=%08lx gp=%08lx gi=%08lx "
			"go=%08lx gaf=%08lx\r\n",
			(unsigned long)first.fault_flags,
			(unsigned long)first.busy,
			(long)first.active_slot,
			(unsigned long)first.active_generation,
			(unsigned long)first.active_words,
			(unsigned long)first.tim_cr1,
			(unsigned long)first.tim_dier,
			(unsigned long)first.tim_sr,
			(unsigned long)first.tim_cnt,
			(unsigned long)first.tim_arr,
			(unsigned long)first.tim_ccr1,
			(unsigned long)first.tim_ccr4,
			(unsigned long)first.tim_ccer,
			(unsigned long)first.tim_bdtr,
			(unsigned long)first.dma_cr,
			(unsigned long)first.dma_ndtr,
			(unsigned long)first.dma_par,
			(unsigned long)first.dma_m0ar,
			(unsigned long)first.dma_fcr,
			(unsigned long)first.dma_lisr,
			(unsigned long)first.dmamux_ccr,
			(unsigned long)first.dmamux_csr,
			(unsigned long)first.gpio_moder,
			(unsigned long)first.gpio_ospeedr,
			(unsigned long)first.gpio_pupdr,
			(unsigned long)first.gpio_idr,
			(unsigned long)first.gpio_odr,
			(unsigned long)first.gpio_afrh);
	}
#if !defined(OPENVLC_TX_PIN_PROBE) || OPENVLC_TX_PIN_PROBE
	{
		/*
		 * Physical PE9 measurement (wire-free). dutyx10 and khz are what
		 * the pad ACTUALLY did; they close the CCR1-sequence blind spot of
		 * the register snapshot above. Interpret against a live frame
		 * (busy=1/1): dutyx10~500 and khz~2000 => pin correct even if the
		 * LED is dim (electrical/optical fault); edges~0 => digital fault.
		 */
		openvlc_stm32_tx_pin_probe_t probe;
		uint32_t us;
		uint32_t dutyx10;
		uint32_t khz;

		openvlc_stm32_tx_pin_probe(&probe);
		us = probe.cycles / (SystemCoreClock / 1000000u);
		dutyx10 = probe.samples ?
			(probe.high_samples * 1000u) / probe.samples : 0u;
		/* edges are level changes; a full line cell is two changes, so the
		 * cell rate in kHz = edges / 2 / us * 1000 = edges * 500 / us. */
		khz = us ? (probe.edges * 500u) / us : 0u;
		openvlc_platform_log(
			"TXPIN busy=%lu/%lu slot=%ld samp=%lu us=%lu high=%lu "
			"edges=%lu dutyx10=%lu cell_khz=%lu\r\n",
			(unsigned long)probe.busy_start,
			(unsigned long)probe.busy_end,
			(long)probe.active_slot,
			(unsigned long)probe.samples,
			(unsigned long)us,
			(unsigned long)probe.high_samples,
			(unsigned long)probe.edges,
			(unsigned long)dutyx10,
			(unsigned long)khz);
	}
#endif
#endif
#else
	(void)now_ms;
#endif
}
