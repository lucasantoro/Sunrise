#ifndef OPENVLC_TX_COMPAT_H
#define OPENVLC_TX_COMPAT_H

#include "openvlc_types.h"

typedef struct {
	uint32_t budget;
	uint32_t phy_rate_kbps;
	uint32_t timer_hz;
	uint32_t cell_ticks;
	uint32_t cell_rate_hz;
	uint32_t gap_cells;
	uint32_t warmup_cells;
} openvlc_tx_profile_t;

typedef struct {
	bool *symbols;
	size_t symbol_cap;
	size_t symbol_len;
} openvlc_tx_symbol_buffer_t;

typedef struct {
	uint32_t *words;
	size_t word_cap;
	size_t word_len;
} openvlc_tx_dma_buffer_t;

typedef struct {
	uint32_t high_words;
	uint32_t low_words;
	uint32_t checksum;
} openvlc_tx_oc_stats_t;

const openvlc_tx_profile_t *openvlc_tx_default_profile(void);
const openvlc_tx_profile_t *openvlc_tx_profile_for_budget(uint32_t budget);

openvlc_status_t openvlc_tx_compat_build_frame(const openvlc_packet_t *packet,
					    uint8_t *frame, size_t frame_cap,
					    size_t *frame_len,
					    uint16_t *physical_symbols);
openvlc_status_t openvlc_tx_compat_frame_to_symbols(const uint8_t *frame,
						 size_t frame_len,
						 openvlc_tx_symbol_buffer_t *out);
openvlc_status_t openvlc_tx_compat_packet_to_symbols(const openvlc_packet_t *packet,
						  openvlc_tx_symbol_buffer_t *out);

/* Build the TIM output-compare stream directly, without the intermediate
 * bool-symbol array used by the generic compatibility path. */
openvlc_status_t openvlc_tx_compat_packet_to_oc_words(
	const openvlc_packet_t *packet, const openvlc_tx_profile_t *profile,
	uint16_t *words, size_t word_cap, size_t *word_len,
	openvlc_tx_oc_stats_t *word_stats);

/*
 * Convert logical OOK cells to generic high/low DMA words. The timer-OC path
 * passes cell_ticks/0 for CCR1; the legacy GPIO path passes GPIO set/reset
 * BSRR masks. The output includes profile->gap_cells of low level.
 */
openvlc_status_t openvlc_tx_compat_symbols_to_bsrr(const bool *symbols,
						size_t symbol_len,
						const openvlc_tx_profile_t *profile,
						uint32_t set_word,
						uint32_t reset_word,
						openvlc_tx_dma_buffer_t *out);
openvlc_status_t openvlc_tx_compat_packet_to_bsrr(const openvlc_packet_t *packet,
					       const openvlc_tx_profile_t *profile,
					       uint32_t set_word,
					       uint32_t reset_word,
					       openvlc_tx_dma_buffer_t *out);

#endif
