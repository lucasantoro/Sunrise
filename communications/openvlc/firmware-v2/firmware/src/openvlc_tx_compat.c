#include "openvlc_tx_compat.h"

#include <string.h>

#include "openvlc_frame.h"
#include "openvlc_linecode.h"

/*
 * Warm-up cell generator, shared by BOTH encoders.
 *
 * One generator for both encoders. openvlc_tx_compat_frame_to_symbols() and
 * openvlc_tx_compat_packet_to_oc_words() must emit the same warm-up, and only
 * the second is what the STM32 actually calls - a second copy here silently
 * keeps the PRBS warm-up off the air.
 *
 * See OPENVLC_TX_WARMUP_PRBS in openvlc_board.h for the measurement behind it.
 */
typedef struct {
	uint16_t lfsr;
	bool     last;
	uint8_t  run;
	bool     pair_pending;
	bool     pair_value;
	uint32_t index;
} openvlc_warmup_gen_t;

static void warmup_gen_init(openvlc_warmup_gen_t *g)
{
	g->lfsr = (uint16_t)OPENVLC_TX_WARMUP_PRBS_SEED;
	g->last = false;
	g->run = 0u;
	g->pair_pending = false;
	g->pair_value = false;
	g->index = 0u;
}

/* Next warm-up CELL level. Two calls consume one Manchester pair. */
static bool warmup_gen_next(openvlc_warmup_gen_t *g)
{
#if defined(OPENVLC_TX_WARMUP_PRBS) && OPENVLC_TX_WARMUP_PRBS
	if (g->pair_pending) {
		g->pair_pending = false;
		return g->pair_value;
	}
	{
		bool value;

		g->lfsr = (uint16_t)(((g->lfsr << 1) |
				      (((g->lfsr >> 8) ^ (g->lfsr >> 4)) & 1u)) &
				     0x1ffu);
		value = (g->lfsr & 1u) != 0u;
		if (g->run >= 4u && value == g->last)
			value = !g->last;
		g->run = (value == g->last) ? (uint8_t)(g->run + 1u) : 1u;
		g->last = value;
		g->pair_value = value;
		g->pair_pending = true;
		return !value;
	}
#else
	return ((g->index++) & 1u) == 0u;
#endif
}


static const openvlc_tx_profile_t profile_budget100 = {
	.budget = 100u,
	.phy_rate_kbps = 500u,
	.timer_hz = OPENVLC_STM32_TX_TIMER_HZ,
	.cell_ticks = OPENVLC_STM32_TX_BUDGET100_CELL_TICKS,
	.cell_rate_hz = OPENVLC_STM32_TX_TIMER_HZ /
			OPENVLC_STM32_TX_BUDGET100_CELL_TICKS,
	.gap_cells = OPENVLC_STM32_TX_GAP_CELLS,
	.warmup_cells = OPENVLC_STM32_TX_WARMUP_CELLS,
};

static const openvlc_tx_profile_t profile_budget40 = {
	.budget = 40u,
	.phy_rate_kbps = 1250u,
	.timer_hz = OPENVLC_STM32_TX_TIMER_HZ,
	.cell_ticks = OPENVLC_STM32_TX_BUDGET40_CELL_TICKS,
	.cell_rate_hz = OPENVLC_STM32_TX_TIMER_HZ / OPENVLC_STM32_TX_BUDGET40_CELL_TICKS,
	.gap_cells = OPENVLC_STM32_TX_GAP_CELLS,
	.warmup_cells = OPENVLC_STM32_TX_WARMUP_CELLS,
};

static const openvlc_tx_profile_t profile_budget50 = {
	.budget = 50u,
	.phy_rate_kbps = 1000u,
	.timer_hz = OPENVLC_STM32_TX_TIMER_HZ,
	.cell_ticks = OPENVLC_STM32_TX_BUDGET50_CELL_TICKS,
	.cell_rate_hz = OPENVLC_STM32_TX_TIMER_HZ / OPENVLC_STM32_TX_BUDGET50_CELL_TICKS,
	.gap_cells = OPENVLC_STM32_TX_GAP_CELLS,
	.warmup_cells = OPENVLC_STM32_TX_WARMUP_CELLS,
};

const openvlc_tx_profile_t *openvlc_tx_profile_for_budget(uint32_t budget)
{
	if (budget == 40u)
		return &profile_budget40;
	if (budget == 50u)
		return &profile_budget50;
	if (budget == 100u)
		return &profile_budget100;
	return NULL;
}

const openvlc_tx_profile_t *openvlc_tx_default_profile(void)
{
	return openvlc_tx_profile_for_budget(OPENVLC_STM32_TX_PROFILE_BUDGET);
}

static size_t compat_rs_blocks(uint16_t payload_len)
{
	size_t encoded_len = (size_t)payload_len + 2u * 2u + 2u;

	return (encoded_len + OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE - 1u) /
	       OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE;
}

openvlc_status_t openvlc_tx_compat_build_frame(const openvlc_packet_t *packet,
					    uint8_t *frame, size_t frame_cap,
					    size_t *frame_len,
					    uint16_t *physical_symbols)
{
	size_t out = 0;
	size_t ecc_bytes;
	size_t encoded_len;
	size_t data_base;
	uint32_t symbol_len;

	if (!packet || !frame || !frame_len)
		return OPENVLC_ERR_ARG;
	if (packet->payload_len > OPENVLC_MAX_PAYLOAD_BYTES)
		return OPENVLC_ERR_ARG;

	ecc_bytes = OPENVLC_BEAGLEBONE_RS_ECC_BYTES *
		    compat_rs_blocks(packet->payload_len);
	*frame_len = OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES + 2u +
		     OPENVLC_HEADER_BYTES + packet->payload_len + ecc_bytes;
	if (*frame_len > frame_cap)
		return OPENVLC_ERR_OVERFLOW;

	symbol_len = OPENVLC_PREAMBLE_BITS +
		     (uint32_t)((*frame_len - OPENVLC_PREAMBLE_BYTES) *
				openvlc_symbols_per_byte(OPENVLC_LINE_MANCHESTER)) +
		     1u;
	if (symbol_len > 0xffffu)
		return OPENVLC_ERR_OVERFLOW;

	for (size_t i = 0; i < OPENVLC_PREAMBLE_BYTES; i++)
		frame[out++] = OPENVLC_PREAMBLE_BYTE;
	OPENVLC_SFD_EMIT(frame, out);
	frame[out++] = (uint8_t)(symbol_len >> 8);
	frame[out++] = (uint8_t)symbol_len;
	frame[out++] = 0u;
	frame[out++] = packet->dst;
	frame[out++] = 0u;
	frame[out++] = packet->src;
	frame[out++] = (uint8_t)(packet->protocol >> 8);
	frame[out++] = (uint8_t)packet->protocol;
	memcpy(&frame[out], packet->payload, packet->payload_len);
	out += packet->payload_len;

	data_base = OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES + 2u;
	encoded_len = (size_t)packet->payload_len + 2u * 2u + 2u;
	if (openvlc_frame_beaglebone_encode_rs(&frame[data_base],
					       encoded_len,
					       &frame[out],
					       ecc_bytes) != OPENVLC_OK)
		return OPENVLC_ERR_CRC;

	if (physical_symbols)
		*physical_symbols = (uint16_t)symbol_len;
	return OPENVLC_OK;
}

openvlc_status_t openvlc_tx_compat_frame_to_symbols(const uint8_t *frame,
						 size_t frame_len,
						 openvlc_tx_symbol_buffer_t *out)
{
	size_t pos = 0;

	if (!frame || !out || !out->symbols)
		return OPENVLC_ERR_ARG;
	if (frame_len < OPENVLC_PREAMBLE_BYTES)
		return OPENVLC_ERR_ARG;
	if (OPENVLC_STM32_TX_WARMUP_CELLS > out->symbol_cap)
		return OPENVLC_ERR_OVERFLOW;

	{
		openvlc_warmup_gen_t wg;

		warmup_gen_init(&wg);
		for (uint32_t i = 0; i < OPENVLC_STM32_TX_WARMUP_CELLS; i++)
			out->symbols[pos++] = warmup_gen_next(&wg);
	}

	for (size_t i = 0; i < OPENVLC_PREAMBLE_BYTES; i++) {
		for (uint8_t bit = 0; bit < 8u; bit++) {
			if (pos >= out->symbol_cap)
				return OPENVLC_ERR_OVERFLOW;
			out->symbols[pos++] =
				((frame[i] >> (7u - bit)) & 1u) != 0u;
		}
	}

	for (size_t i = OPENVLC_PREAMBLE_BYTES; i < frame_len; i++) {
		for (uint8_t bit = 0; bit < 8u; bit++) {
			bool value = ((frame[i] >> (7u - bit)) & 1u) != 0u;

			if (pos + 2u > out->symbol_cap)
				return OPENVLC_ERR_OVERFLOW;
			out->symbols[pos++] = !value;
			out->symbols[pos++] = value;
		}
	}

	if (pos >= out->symbol_cap)
		return OPENVLC_ERR_OVERFLOW;
	out->symbols[pos++] = false;

	out->symbol_len = pos;
	return OPENVLC_OK;
}

openvlc_status_t openvlc_tx_compat_packet_to_symbols(const openvlc_packet_t *packet,
						  openvlc_tx_symbol_buffer_t *out)
{
	uint8_t frame[OPENVLC_MAX_FRAME_BYTES];
	size_t frame_len = 0;
	openvlc_status_t status;

	status = openvlc_tx_compat_build_frame(packet, frame, sizeof(frame),
					    &frame_len, NULL);
	if (status != OPENVLC_OK)
		return status;
	status = openvlc_tx_compat_frame_to_symbols(frame, frame_len, out);
	memset(frame, 0, sizeof(frame));
	return status;
}

static inline void oc_word_append(uint16_t *words, size_t *pos,
				  uint16_t value, uint16_t high,
				  openvlc_tx_oc_stats_t *stats)
{
	words[(*pos)++] = value;
	if (value == high)
		stats->high_words++;
	else
		stats->low_words++;
	/* Ordered FNV-1a over the exact halfwords handed to DMA2. */
	stats->checksum ^= value;
	stats->checksum *= 16777619u;
}

openvlc_status_t openvlc_tx_compat_packet_to_oc_words(
	const openvlc_packet_t *packet, const openvlc_tx_profile_t *profile,
	uint16_t *words, size_t word_cap, size_t *word_len,
	openvlc_tx_oc_stats_t *word_stats)
{
	uint8_t frame[OPENVLC_MAX_FRAME_BYTES];
	size_t frame_len = 0u;
	size_t required;
	size_t pos = 0u;
	openvlc_status_t status;
	uint16_t high;
	openvlc_tx_oc_stats_t stats = {
		.high_words = 0u,
		.low_words = 0u,
		.checksum = 2166136261u,
	};

	if (!packet || !profile || !words || !word_len)
		return OPENVLC_ERR_ARG;
	if (profile->cell_ticks > UINT16_MAX)
		return OPENVLC_ERR_OVERFLOW;
	status = openvlc_tx_compat_build_frame(packet, frame, sizeof(frame),
					       &frame_len, NULL);
	if (status != OPENVLC_OK)
		return status;
	required = profile->warmup_cells + OPENVLC_PREAMBLE_BITS +
		   (frame_len - OPENVLC_PREAMBLE_BYTES) * 16u +
		   1u + profile->gap_cells + 1u;
	if (required > word_cap)
		return OPENVLC_ERR_OVERFLOW;

	high = (uint16_t)profile->cell_ticks;
	{
		openvlc_warmup_gen_t wg;

		warmup_gen_init(&wg);
		for (uint32_t i = 0u; i < profile->warmup_cells; i++)
			oc_word_append(words, &pos,
				       warmup_gen_next(&wg) ? high : 0u,
				       high, &stats);
	}
	for (size_t i = 0u; i < OPENVLC_PREAMBLE_BYTES; i++) {
		for (uint8_t bit = 0u; bit < 8u; bit++)
			oc_word_append(words, &pos,
				       ((frame[i] >> (7u - bit)) & 1u) ?
					       high : 0u,
				       high, &stats);
	}
	for (size_t i = OPENVLC_PREAMBLE_BYTES; i < frame_len; i++) {
		for (uint8_t bit = 0u; bit < 8u; bit++) {
			bool value = ((frame[i] >> (7u - bit)) & 1u) != 0u;

			oc_word_append(words, &pos, value ? 0u : high,
				       high, &stats);
			oc_word_append(words, &pos, value ? high : 0u,
				       high, &stats);
		}
	}
	/* Preserve the existing frame-tail cell, configured gap, and final low
	 * guard cell exactly; DMA completion therefore remains inside low level. */
	oc_word_append(words, &pos, 0u, high, &stats);
	for (uint32_t i = 0u; i < profile->gap_cells; i++)
		oc_word_append(words, &pos, 0u, high, &stats);
	oc_word_append(words, &pos, 0u, high, &stats);
	*word_len = pos;
	if (word_stats)
		*word_stats = stats;
	return OPENVLC_OK;
}

openvlc_status_t openvlc_tx_compat_symbols_to_bsrr(const bool *symbols,
						size_t symbol_len,
						const openvlc_tx_profile_t *profile,
						uint32_t set_word,
						uint32_t reset_word,
						openvlc_tx_dma_buffer_t *out)
{
	size_t total;

	if (!symbols || !profile || !out || !out->words)
		return OPENVLC_ERR_ARG;
	total = symbol_len + profile->gap_cells + 1u;
	if (total > out->word_cap)
		return OPENVLC_ERR_OVERFLOW;

	out->word_len = 0;
	for (size_t i = 0; i < symbol_len; i++)
		out->words[out->word_len++] = symbols[i] ? set_word : reset_word;
	for (uint32_t i = 0; i < profile->gap_cells; i++)
		out->words[out->word_len++] = reset_word;
	out->words[out->word_len++] = reset_word;
	return OPENVLC_OK;
}

openvlc_status_t openvlc_tx_compat_packet_to_bsrr(const openvlc_packet_t *packet,
					       const openvlc_tx_profile_t *profile,
					       uint32_t set_word,
					       uint32_t reset_word,
					       openvlc_tx_dma_buffer_t *out)
{
	static bool symbols[OPENVLC_MAX_SYMBOLS];
	openvlc_tx_symbol_buffer_t symbol_out = {
		.symbols = symbols,
		.symbol_cap = OPENVLC_MAX_SYMBOLS,
		.symbol_len = 0,
	};
	openvlc_status_t status;

	status = openvlc_tx_compat_packet_to_symbols(packet, &symbol_out);
	if (status != OPENVLC_OK)
		return status;
	return openvlc_tx_compat_symbols_to_bsrr(symbol_out.symbols,
					      symbol_out.symbol_len,
					      profile, set_word, reset_word,
					      out);
}
