/*
 * host_loopback.c - host-side self-test for the STM32 comparator RX decoder.
 *
 * It builds a BeagleBone-format frame (828-byte payload, Reed-Solomon encoded),
 * synthesises the comparator edge-timestamp stream that the optical front-end
 * would produce for it (one half-cell = HCELL ticks, with timing jitter), and
 * checks that openvlc_rx_edges_to_packet() -> comp_decode_sfd_sync() recovers
 * the exact payload. A second pass injects narrow glitch-pulse pairs to verify
 * openvlc_edge_cancel_short_pulses() restores the clean edge sequence.
 *
 * There is no ADC/sample path any more; this exercises the single live RX path.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "openvlc_app.h"
#include "openvlc_frame.h"
#include "openvlc_linecode.h"
#include "openvlc_phy.h"

#ifndef OPENVLC_TEST_HCELL
#define OPENVLC_TEST_HCELL 64u
#endif

#ifndef OPENVLC_TEST_DCD_DELAY
#define OPENVLC_TEST_DCD_DELAY 24u
#endif

#ifndef OPENVLC_TEST_DCD_STEP
#define OPENVLC_TEST_DCD_STEP 8u
#endif

#ifndef OPENVLC_TEST_JITTER
#define OPENVLC_TEST_JITTER 2
#endif

#ifndef OPENVLC_TEST_GLITCH_MIN
#define OPENVLC_TEST_GLITCH_MIN 14u
#endif

#ifndef OPENVLC_COMP_RUN_BIAS_DIV
#define OPENVLC_COMP_RUN_BIAS_DIV 16u
#endif

static unsigned lcg_state = 1;

extern volatile uint32_t openvlc_phy_dbg_sfdsync_single;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_cell0;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_cell1;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_train;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_syncs;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_lock_cell;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_pre_rejects;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_mode;
extern volatile uint32_t openvlc_phy_dbg_timing_residual_peak;
extern volatile uint32_t openvlc_phy_dbg_phase_edits;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_maxbits;
extern volatile int32_t openvlc_phy_dbg_parse_status;

void openvlc_platform_log(const char *fmt, ...)
{
	(void)fmt;
}

void openvlc_platform_on_packet(const openvlc_packet_t *packet,
				const openvlc_quality_t *quality)
{
	(void)packet;
	(void)quality;
}

/* Deterministic pseudo-random value in [-amplitude, +amplitude]. */
static int noise(int amplitude)
{
	lcg_state = lcg_state * 1664525u + 1013904223u;
	return (int)((lcg_state >> 16) % (unsigned)(2 * amplitude + 1)) - amplitude;
}

static size_t beaglebone_rs_blocks(uint16_t payload_len)
{
	size_t encoded_len = (size_t)payload_len + 2u * 2u + 2u;

	return (encoded_len + OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE - 1u) /
	       OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE;
}

/* Build the on-air BeagleBone frame: preamble, SFD, length, header, payload,
 * Reed-Solomon parity. Mirrors what the BeagleBone TX puts on the wire. */
static int build_beaglebone_frame(const openvlc_packet_t *packet, uint8_t *frame,
				  size_t frame_cap, size_t *frame_len)
{
	size_t out = 0;
	size_t ecc_bytes;
	size_t data_base;
	size_t encoded_len;
	uint16_t symbol_len;

	if (!packet || !frame || !frame_len)
		return 0;
	ecc_bytes = OPENVLC_BEAGLEBONE_RS_ECC_BYTES *
		    beaglebone_rs_blocks(packet->payload_len);
	*frame_len = OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES + 2u + OPENVLC_HEADER_BYTES +
		     packet->payload_len + ecc_bytes;
	if (*frame_len > frame_cap)
		return 0;
	symbol_len = (uint16_t)(OPENVLC_PREAMBLE_BITS +
				((*frame_len - OPENVLC_PREAMBLE_BYTES) *
				 openvlc_symbols_per_byte(OPENVLC_LINE_MANCHESTER)) +
				1u);
	for (size_t i = 0; i < OPENVLC_PREAMBLE_BYTES; i++)
		frame[out++] = OPENVLC_PREAMBLE_BYTE;
	OPENVLC_SFD_EMIT(frame, out);
	frame[out++] = (uint8_t)(symbol_len >> 8);
	frame[out++] = (uint8_t)symbol_len;
	frame[out++] = 0;
	frame[out++] = packet->dst;
	frame[out++] = 0;
	frame[out++] = packet->src;
	frame[out++] = (uint8_t)(packet->protocol >> 8);
	frame[out++] = (uint8_t)packet->protocol;
	memcpy(&frame[out], packet->payload, packet->payload_len);
	out += packet->payload_len;
	data_base = OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES + 2u;
	encoded_len = (size_t)packet->payload_len + 2u * 2u + 2u;
	if (openvlc_frame_beaglebone_encode_rs(&frame[data_base], encoded_len,
					       &frame[out], ecc_bytes) != OPENVLC_OK)
		return 0;
	return 1;
}

/* Expand a frame into its line-symbol (cell) sequence: the preamble is raw
 * bits, the rest is Manchester (1 = LOW-HIGH, 0 = HIGH-LOW). */
static int beaglebone_symbols_from_frame(const uint8_t *frame, size_t frame_len,
					 bool *symbols, size_t symbol_cap,
					 size_t *symbol_len)
{
	size_t out = 0;

	if (!frame || !symbols || !symbol_len ||
	    frame_len < OPENVLC_PREAMBLE_BYTES)
		return 0;
	for (size_t i = 0; i < OPENVLC_PREAMBLE_BYTES; i++) {
		for (uint8_t bit = 0; bit < 8u; bit++) {
			if (out >= symbol_cap)
				return 0;
			symbols[out++] = ((frame[i] >> (7u - bit)) & 1u) != 0u;
		}
	}
	for (size_t i = OPENVLC_PREAMBLE_BYTES; i < frame_len; i++) {
		for (uint8_t bit = 0; bit < 8u; bit++) {
			bool value = ((frame[i] >> (7u - bit)) & 1u) != 0u;

			if (out + 1u >= symbol_cap)
				return 0;
			symbols[out++] = !value;
			symbols[out++] = value;
		}
	}
	*symbol_len = out;
	return 1;
}

static uint64_t preamble_gate_history(bool valid)
{
	uint64_t history = 0;

	for (uint32_t i = 0; i < OPENVLC_SFD_SYNC_PREAMBLE_CELLS; i++) {
		bool symbol = (i & 1u) == 0u;

		if (!valid && i == OPENVLC_SFD_SYNC_PREAMBLE_CELLS / 2u)
			symbol = (i & 1u) != 0u;
		history = (history << 1) | (symbol ? 1u : 0u);
	}
	for (int bit = (int)OPENVLC_SFD_BITS - 1; bit >= 0; bit--) {
		bool value = OPENVLC_SFD_BIT(bit) != 0u;

		history = (history << 1) | (value ? 0u : 1u);
		history = (history << 1) | (value ? 1u : 0u);
	}
	return history;
}

/*
 * Deterministic synthetic-channel campaign.  It exercises the complete
 * timestamp decoder with reproducible combinations of clock error,
 * polarity-dependent crossing delay, edge jitter, capture phase and narrow
 * comparator pulses.  A destructive case may either be corrected exactly or
 * rejected; accepting a packet with different contents is always a failure.
 */
static unsigned campaign_random(unsigned *state)
{
	*state = *state * 1664525u + 1013904223u;
	return *state;
}

static int campaign_noise(unsigned *state, int amplitude)
{
	if (amplitude <= 0)
		return 0;
	return (int)((campaign_random(state) >> 16) %
		     (unsigned)(2 * amplitude + 1)) - amplitude;
}

static size_t campaign_generate_edges(const bool *cells, size_t cell_count,
				      uint32_t halfcell, uint32_t rising_delay,
				      int jitter, int clock_ppm,
				      unsigned seed, bool clip_origin,
				      uint32_t *edges, size_t edge_cap)
{
	size_t count = 0u;
	unsigned state = seed;

	if (!cells || cell_count < 2u || !edges || edge_cap == 0u)
		return 0u;
	edges[count++] = 0u;
	for (size_t i = 1u; i < cell_count; i++) {
		int64_t scaled;
		int64_t crossing;

		if (cells[i] == cells[i - 1u])
			continue;
		scaled = (int64_t)i * (int64_t)halfcell *
			 (1000000ll + (int64_t)clock_ppm);
		crossing = (scaled + 500000ll) / 1000000ll;
		if (cells[i])
			crossing += rising_delay;
		crossing += campaign_noise(&state, jitter);
		if (crossing <= 0 || count >= edge_cap ||
		    (count > 0u && (uint32_t)crossing <= edges[count - 1u]))
			return 0u;
		edges[count++] = (uint32_t)crossing;
	}
	if (clip_origin && count > 1u) {
		memmove(edges, edges + 1u, (count - 1u) * sizeof(edges[0]));
		count--;
	}
	return count;
}

static size_t campaign_insert_glitches(const uint32_t *clean,
				       size_t clean_count, uint32_t halfcell,
				       uint32_t hard_glitch, unsigned period,
				       uint32_t *raw, size_t raw_cap)
{
	size_t out = 0u;
	unsigned eligible = 0u;
	uint32_t first = hard_glitch > 2u ? hard_glitch - 2u : 1u;
	uint32_t second = first + 1u;

	if (!clean || clean_count == 0u || !raw || raw_cap < clean_count)
		return 0u;
	raw[out++] = clean[0];
	for (size_t i = 1u; i < clean_count; i++) {
		uint32_t run = clean[i] - clean[i - 1u];

		if (period != 0u && run >= 2u * halfcell - halfcell / 4u &&
		    (++eligible % period) == 0u) {
			if (out + 3u > raw_cap || second >= run)
				return 0u;
			raw[out++] = clean[i - 1u] + first;
			raw[out++] = clean[i - 1u] + second;
		}
		raw[out++] = clean[i];
	}
	return out;
}

/* 1 = exact packet, 0 = rejected, -1 = corrupted packet was accepted. */
static int campaign_decode(const openvlc_runtime_config_t *cfg,
			   const openvlc_packet_t *expected,
			   const uint32_t *edges, size_t edge_count)
{
	static uint16_t scratch[OPENVLC_RX_SAMPLE_BUFFER_LEN];
	openvlc_packet_t packet = {0};
	openvlc_quality_t quality = {0};
	openvlc_status_t status = openvlc_rx_edges_to_packet(
		cfg, edges, edge_count, scratch, OPENVLC_RX_SAMPLE_BUFFER_LEN,
		&packet, &quality);

	if (status != OPENVLC_OK)
		return 0;
	if (packet.dst != expected->dst || packet.src != expected->src ||
	    packet.protocol != expected->protocol ||
	    packet.payload_len != expected->payload_len ||
	    memcmp(packet.payload, expected->payload, expected->payload_len) != 0)
		return -1;
	return 1;
}

static int run_synthetic_channel_campaign(
	const openvlc_runtime_config_t *cfg, const openvlc_packet_t *expected,
	const bool *cells, size_t cell_count, uint32_t halfcell)
{
	static uint32_t clean[OPENVLC_MAX_SYMBOLS + 8u];
	static uint32_t raw[OPENVLC_MAX_SYMBOLS * 2u];
	static uint32_t damaged[OPENVLC_MAX_SYMBOLS * 2u];
	static const int ppm_values[] = {-10000, 0, 10000};
	uint32_t dcd_values[] = {0u, halfcell / 8u, halfcell / 4u};
	unsigned benign = 0u;
	unsigned safety = 0u;
	unsigned rejected = 0u;
	int max_jitter = (int)(halfcell / 16u);

	if (max_jitter < 1)
		max_jitter = 1;

	/* Cartesian boundary grid: every point is inside the required envelope. */
	for (size_t p = 0u; p < sizeof(ppm_values) / sizeof(ppm_values[0]); p++) {
		for (size_t d = 0u; d < sizeof(dcd_values) / sizeof(dcd_values[0]); d++) {
			for (int jitter = 0; jitter <= max_jitter; jitter++) {
				for (unsigned clipped = 0u; clipped <= 1u; clipped++) {
					size_t clean_count = campaign_generate_edges(
						cells, cell_count, halfcell, dcd_values[d],
						jitter, ppm_values[p],
						0x5a170001u + benign, clipped != 0u,
						clean, sizeof(clean) / sizeof(clean[0]));

					if (clean_count == 0u ||
					    campaign_decode(cfg, expected, clean, clean_count) != 1) {
						fprintf(stderr,
							"synthetic grid failed: ppm=%d dcd=%lu jitter=%d clipped=%u edges=%zu\n",
							ppm_values[p], (unsigned long)dcd_values[d],
							jitter, clipped, clean_count);
						return 0;
					}
					benign++;
				}
			}
		}
	}

	/* Reproducible mixed cases, including raw narrow comparator pulses. */
	for (unsigned case_id = 0u; case_id < 96u; case_id++) {
		unsigned state = 0xc001d00du ^ (case_id * 0x9e3779b9u);
		int ppm = (int)(campaign_random(&state) % 30001u) - 15000;
		uint32_t dcd = campaign_random(&state) % (halfcell / 4u + 1u);
		int jitter = (int)(campaign_random(&state) %
					    (unsigned)(max_jitter + 1));
		unsigned period = 3u + campaign_random(&state) % 13u;
		size_t clean_count = campaign_generate_edges(
			cells, cell_count, halfcell, dcd, jitter, ppm, state,
			(case_id & 1u) != 0u, clean,
			sizeof(clean) / sizeof(clean[0]));
		size_t raw_count = campaign_insert_glitches(
			clean, clean_count, halfcell,
			OPENVLC_EDGE_HARD_GLITCH_TICKS, period, raw,
			sizeof(raw) / sizeof(raw[0]));
		uint32_t removed = 0u;

		if (clean_count == 0u || raw_count == 0u)
			return 0;
		raw_count = openvlc_edge_filter_timing_aware(
			raw, raw_count, OPENVLC_EDGE_MIN_INTERVAL_TICKS,
			OPENVLC_EDGE_HARD_GLITCH_TICKS,
			OPENVLC_EDGE_CONTEXT_MARGIN_TICKS, &removed);
		if (removed == 0u ||
		    campaign_decode(cfg, expected, raw, raw_count) != 1) {
			fprintf(stderr,
				"synthetic mixed case failed: id=%u seed=%08x ppm=%d dcd=%lu jitter=%d period=%u removed=%lu\n",
				case_id, state, ppm, (unsigned long)dcd, jitter,
				period, (unsigned long)removed);
			return 0;
		}
		benign++;
	}

	/*
	 * Destructive campaign: remove transition pairs throughout the frame.
	 * FEC/timing recovery may repair a case, but a successful decode must be
	 * byte-for-byte identical.  This catches false-positive CRC/repair paths.
	 */
	{
		size_t clean_count = campaign_generate_edges(
			cells, cell_count, halfcell, 0u, 0, 0, 1u, false,
			clean, sizeof(clean) / sizeof(clean[0]));

		if (clean_count < 200u)
			return 0;
		for (unsigned case_id = 0u; case_id < 32u; case_id++) {
			size_t at = 80u +
				(case_id * (clean_count - 164u)) / 31u;
			int result;

			memcpy(damaged, clean, clean_count * sizeof(clean[0]));
			memmove(&damaged[at], &damaged[at + 2u],
				(clean_count - at - 2u) * sizeof(damaged[0]));
			result = campaign_decode(cfg, expected, damaged,
						 clean_count - 2u);
			if (result < 0) {
				fprintf(stderr,
					"synthetic safety failure: corrupt packet accepted at case=%u edge=%zu\n",
					case_id, at);
				return 0;
			}
			if (result == 0)
				rejected++;
			safety++;
		}

		/* Gross truncation must never look like a complete valid frame. */
		for (unsigned divisor = 2u; divisor <= 4u; divisor++) {
			if (campaign_decode(cfg, expected, clean,
					    clean_count / divisor) != 0) {
				fprintf(stderr,
					"synthetic truncation accepted: divisor=%u\n",
					divisor);
				return 0;
			}
			safety++;
			rejected++;
		}
	}

	printf("synthetic channel campaign OK: benign=%u safety=%u rejected=%u\n",
	       benign, safety, rejected);
	return 1;
}

int main(int argc, char **argv)
{
	openvlc_runtime_config_t cfg = {
		.self_id = 8,
		.peer_id = 7,
		.line_code = OPENVLC_LINE_CODE,
		.samples_per_symbol = OPENVLC_SAMPLES_PER_SYMBOL,
		.mf_score_min = OPENVLC_MF_SCORE_MIN,
		.snr_min_db_centi = OPENVLC_SNR_MIN_DB_CENTI,
	};
	openvlc_packet_t tx = {0};
	openvlc_packet_t rx = {0};
	static bool symbols[OPENVLC_MAX_SYMBOLS];
	static bool distorted_symbols[OPENVLC_MAX_SYMBOLS];
	/* Unused-but-required scratch buffer for the edge decoder API. */
	static uint16_t scratch[OPENVLC_RX_SAMPLE_BUFFER_LEN];
	openvlc_quality_t quality;
	openvlc_status_t status;
	uint8_t bbb_frame[OPENVLC_MAX_FRAME_BYTES];
	uint8_t rs_test_frame[OPENVLC_MAX_FRAME_BYTES];
	size_t bbb_frame_len = 0;
	size_t symbol_len = 0;

	if (argc == 2) {
		static uint32_t captured_edges[OPENVLC_MAX_SYMBOLS + 8u];
		size_t captured_count = 0u;
		unsigned long value;
		FILE *input = fopen(argv[1], "r");

		if (!input) {
			fprintf(stderr, "cannot open edge capture: %s\n", argv[1]);
			return 1;
		}
		while (captured_count <
		       sizeof(captured_edges) / sizeof(captured_edges[0]) &&
		       fscanf(input, "%lu", &value) == 1) {
			captured_edges[captured_count++] = (uint32_t)value;
		}
		fclose(input);
		memset(&quality, 0, sizeof(quality));
		status = openvlc_rx_edges_to_packet(
			&cfg, captured_edges, captured_count, scratch,
			OPENVLC_RX_SAMPLE_BUFFER_LEN, &rx, &quality);
		printf("capture status=%d edges=%zu len=%u half=%u bad=%u/%lu lqi=%u\n",
		       (int)status, captured_count, rx.payload_len,
		       quality.samples_per_symbol,
		       quality.manchester_bad_pairs,
		       (unsigned long)quality.manchester_pairs,
		       quality.link_quality);
		return status == OPENVLC_OK ? 0 : 2;
	}

	if (!openvlc_test_history_has_preamble(
		    preamble_gate_history(true),
		    OPENVLC_SFD_SYNC_PREAMBLE_CELLS + 16u) ||
	    openvlc_test_history_has_preamble(
		    preamble_gate_history(false),
		    OPENVLC_SFD_SYNC_PREAMBLE_CELLS + 16u)) {
		fprintf(stderr, "preamble gate unit test failed\n");
		return 1;
	}
	printf("preamble gate OK: cells=%u\n",
	       (unsigned)OPENVLC_SFD_SYNC_PREAMBLE_CELLS);

	/*
	 * Real iperf-sized BeagleBone layout: 828 payload bytes -> physical
	 * length field 14737 and five Reed-Solomon blocks.
	 */
	tx.dst = 8;
	tx.src = 7;
	tx.protocol = OPENVLC_PROTOCOL_DEFAULT;
	tx.payload_len = 828;
	for (uint16_t i = 0; i < tx.payload_len; i++)
		tx.payload[i] = (uint8_t)(i * 37u + 11u);
	if (!build_beaglebone_frame(&tx, bbb_frame, sizeof(bbb_frame),
				    &bbb_frame_len))
		return 1;
	/*
	 * RS integrity: parity symbols are part of the codeword and must be
	 * correctable; an over-capacity word must never be delivered after a
	 * tentative Berlekamp-Massey correction.
	 */
	memcpy(rs_test_frame, bbb_frame, bbb_frame_len);
	rs_test_frame[bbb_frame_len - 1u] ^= 0x5au;
	memset(&rx, 0, sizeof(rx));
	status = openvlc_frame_parse(rs_test_frame, bbb_frame_len, &rx);
	if (status != OPENVLC_OK || rx.payload_len != tx.payload_len ||
	    memcmp(rx.payload, tx.payload, tx.payload_len) != 0) {
		fprintf(stderr, "RS parity-symbol correction failed: status=%d\n",
			(int)status);
		return 1;
	}
	memcpy(rs_test_frame, bbb_frame, bbb_frame_len);
	for (size_t i = 0u; i < 9u; i++)
		rs_test_frame[OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES + 2u + i] ^=
			(uint8_t)(0x31u + i);
	memset(&rx, 0, sizeof(rx));
	if (openvlc_frame_parse(rs_test_frame, bbb_frame_len, &rx) ==
	    OPENVLC_OK) {
		fprintf(stderr, "RS accepted an over-capacity codeword\n");
		return 1;
	}
	printf("RS correction and post-validation OK\n");

	{
		enum {
			HCELL = OPENVLC_TEST_HCELL,
			GLITCH_MIN = OPENVLC_TEST_GLITCH_MIN
		};
		static uint32_t ideal_edges[OPENVLC_MAX_SYMBOLS + 8];
		static uint32_t edges[OPENVLC_MAX_SYMBOLS + 8];
		static uint32_t dcd_edges[OPENVLC_MAX_SYMBOLS + 8];
		static uint32_t pair_edges[OPENVLC_MAX_SYMBOLS + 8];
		static uint32_t tie_edges[OPENVLC_MAX_SYMBOLS + 8];
		static uint32_t shoulder_edges[OPENVLC_MAX_SYMBOLS + 8];
		static uint32_t clipped_edges[OPENVLC_MAX_SYMBOLS + 8];
		static uint32_t missing_pulse_edges[OPENVLC_MAX_SYMBOLS + 8];
		static uint32_t distorted_edges[OPENVLC_MAX_SYMBOLS + 8];
		static uint32_t noisy_edges[OPENVLC_MAX_SYMBOLS * 2];
		static uint32_t contextual_edges[OPENVLC_MAX_SYMBOLS * 2];
#if OPENVLC_TEST_HCELL == 32u
		static uint32_t short_cell_edges[OPENVLC_MAX_SYMBOLS + 8];
#endif
		size_t ideal_edge_count = 0;
		size_t edge_count = 0;
		size_t dcd_edge_count = 0;
		size_t clipped_edge_count = 0;
		size_t missing_pulse_edge_count = 0;
		size_t distorted_edge_count = 0;
		size_t noisy_count = 0;
		uint32_t t = 0;
		uint8_t ideal_lqi;
		uint8_t jittered_lqi;
		openvlc_quality_t jittered_quality;

		/* Synthesise the comparator edge stream from the frame symbols:
		 * an edge timestamp at every cell where the level changes. */
		if (!beaglebone_symbols_from_frame(bbb_frame, bbb_frame_len,
						   symbols, OPENVLC_MAX_SYMBOLS,
						   &symbol_len))
			return 1;
		ideal_edges[ideal_edge_count++] = 0;
		edges[edge_count++] = 0;
		dcd_edges[dcd_edge_count++] = 0;
		for (size_t i = 1; i < symbol_len; i++) {
			t += HCELL;
			if (symbols[i] != symbols[i - 1]) {
				int jit = noise(OPENVLC_TEST_JITTER);
				uint32_t crossing_delay =
					symbols[i] ? OPENVLC_TEST_DCD_DELAY : 0u;

				ideal_edges[ideal_edge_count++] = t;
				edges[edge_count++] = (uint32_t)((int)t + jit);
				dcd_edges[dcd_edge_count++] =
					t + crossing_delay;
			}
		}

		/* Ideal timing must decode with zero measured residual. */
		memset(&rx, 0, sizeof(rx));
		memset(&quality, 0, sizeof(quality));
		status = openvlc_rx_edges_to_packet(&cfg, ideal_edges,
						    ideal_edge_count, scratch,
						    OPENVLC_RX_SAMPLE_BUFFER_LEN,
						    &rx, &quality);
		if (status != OPENVLC_OK || rx.payload_len != tx.payload_len ||
		    memcmp(rx.payload, tx.payload, tx.payload_len) != 0 ||
		    quality.timing_jitter_x1000 != 0u ||
		    quality.manchester_bad_pairs != 0u ||
		    quality.manchester_pairs == 0u ||
		    quality.link_quality != 100u) {
			fprintf(stderr,
				"ideal metric failed: status=%d len=%u jitter=%lu bad=%u/%lu lqi=%u\n",
				(int)status, rx.payload_len,
				(unsigned long)quality.timing_jitter_x1000,
				quality.manchester_bad_pairs,
				(unsigned long)quality.manchester_pairs,
				quality.link_quality);
			return 1;
		}
		ideal_lqi = openvlc_quality_finalize(&quality, rx.payload_len,
						    true, 0);

		/*
		 * A candidate gap must be non-destructive: an incomplete prefix is
		 * rejected without changing application counters, while the complete
		 * CRC/RS-valid burst is delivered exactly once. This is the portable
		 * contract used by the STM32 4-us/6-us acquisition boundary.
		 */
		openvlc_app_init(&cfg);
		status = openvlc_app_try_rx_edges(
			ideal_edges, ideal_edge_count / 2u, scratch,
			OPENVLC_RX_SAMPLE_BUFFER_LEN);
		if (status == OPENVLC_OK ||
		    openvlc_app_counters()->frames_seen != 0u ||
		    openvlc_app_counters()->frames_delivered != 0u ||
		    openvlc_app_counters()->crc_failed != 0u ||
		    openvlc_app_counters()->sync_failed != 0u) {
			fprintf(stderr,
				"candidate prefix changed application state: status=%d seen=%lu delivered=%lu\n",
				(int)status,
				(unsigned long)openvlc_app_counters()->frames_seen,
				(unsigned long)openvlc_app_counters()->frames_delivered);
			return 1;
		}
		openvlc_app_commit_rx_failure(status);
		if (openvlc_app_counters()->frames_seen != 1u ||
		    openvlc_app_counters()->frames_delivered != 0u ||
		    openvlc_app_counters()->crc_failed +
			    openvlc_app_counters()->quality_dropped +
			    openvlc_app_counters()->sync_failed != 1u) {
			fprintf(stderr, "cached candidate failure was not accounted once\n");
			return 1;
		}
		openvlc_app_init(&cfg);
		status = openvlc_app_try_rx_edges(
			ideal_edges, ideal_edge_count, scratch,
			OPENVLC_RX_SAMPLE_BUFFER_LEN);
		if (status != OPENVLC_OK ||
		    openvlc_app_counters()->frames_seen != 1u ||
		    openvlc_app_counters()->frames_delivered != 1u) {
			fprintf(stderr,
				"complete candidate was not delivered once: status=%d seen=%lu delivered=%lu\n",
				(int)status,
				(unsigned long)openvlc_app_counters()->frames_seen,
				(unsigned long)openvlc_app_counters()->frames_delivered);
			return 1;
		}

		/* Jittered timestamps must still decode but report degradation. */
		memset(&rx, 0, sizeof(rx));
		memset(&quality, 0, sizeof(quality));
		status = openvlc_rx_edges_to_packet(&cfg, edges, edge_count,
						    scratch,
						    OPENVLC_RX_SAMPLE_BUFFER_LEN,
						    &rx, &quality);
		if (status != OPENVLC_OK || rx.payload_len != tx.payload_len ||
		    memcmp(rx.payload, tx.payload, tx.payload_len) != 0 ||
		    quality.timing_jitter_x1000 == 0u ||
		    quality.timing_intervals == 0u ||
		    quality.manchester_pairs == 0u) {
			fprintf(stderr,
				"comparator metric failed: status=%d len=%u edges=%zu jitter=%lu intervals=%lu pairs=%lu timing=%lu/%lu/%lu train=%lu syncs=%lu lock=%lu prerej=%lu phaseq=%lu\n",
				(int)status, rx.payload_len, edge_count,
				(unsigned long)quality.timing_jitter_x1000,
				(unsigned long)quality.timing_intervals,
				(unsigned long)quality.manchester_pairs,
				(unsigned long)openvlc_phy_dbg_sfdsync_single,
				(unsigned long)openvlc_phy_dbg_sfdsync_cell0,
				(unsigned long)openvlc_phy_dbg_sfdsync_cell1,
				(unsigned long)openvlc_phy_dbg_sfdsync_train,
				(unsigned long)openvlc_phy_dbg_sfdsync_syncs,
				(unsigned long)openvlc_phy_dbg_sfdsync_lock_cell,
				(unsigned long)openvlc_phy_dbg_sfdsync_pre_rejects,
				(unsigned long)openvlc_phy_dbg_timing_residual_peak);
			return 1;
		}
		jittered_lqi = openvlc_quality_finalize(&quality, rx.payload_len,
						       true, 0);
		jittered_quality = quality;
		if (jittered_lqi >= ideal_lqi) {
			fprintf(stderr,
				"LQI did not respond to jitter: ideal=%u jittered=%u\n",
				ideal_lqi, jittered_lqi);
			return 1;
		}
		printf("comparator metrics OK: ideal_lqi=%u jittered_lqi=%u jitter=%lu.%lu%%\n",
		       ideal_lqi, jittered_lqi,
		       (unsigned long)(quality.timing_jitter_x1000 / 10u),
		       (unsigned long)(quality.timing_jitter_x1000 % 10u));

		/* Model a polarity-dependent comparator crossing delay. */
		memset(&rx, 0, sizeof(rx));
		memset(&quality, 0, sizeof(quality));
		status = openvlc_rx_edges_to_packet(&cfg, dcd_edges,
						    dcd_edge_count, scratch,
						    OPENVLC_RX_SAMPLE_BUFFER_LEN,
						    &rx, &quality);
		if (status != OPENVLC_OK || rx.payload_len != tx.payload_len ||
		    memcmp(rx.payload, tx.payload, tx.payload_len) != 0 ||
		    quality.samples_per_symbol != HCELL) {
			fprintf(stderr,
				"duty-cycle decode failed: status=%d len=%u edges=%zu half=%u\n",
				(int)status, rx.payload_len, dcd_edge_count,
				quality.samples_per_symbol);
			return 1;
		}
		printf("comparator duty-cycle recovery OK: half=%u ticks\n",
		       quality.samples_per_symbol);

		/*
		 * Packet-local threshold movement can move the shared crossing of
		 * two one-cell intervals without moving either outer edge. The
		 * captured 1-Mbit/s failure contained six 20+44 / 21+43 pairs:
		 * independent run decisions inserted six false cells although each
		 * pair still totalled exactly two half-cells. Reproduce that defect
		 * and require the pair-total invariant to recover the exact frame.
		 */
		memcpy(pair_edges, dcd_edges,
		       dcd_edge_count * sizeof(pair_edges[0]));
		{
			size_t changed = 0u;
			uint32_t crossing_shift =
				HCELL / 2u + HCELL / 8u;

			for (size_t i = dcd_edge_count / 5u;
			     i + 1u < dcd_edge_count && changed < 6u; i++) {
				uint32_t first =
					pair_edges[i] - pair_edges[i - 1u];
				uint32_t second =
					pair_edges[i + 1u] - pair_edges[i];

				/* Exercise both possible pair alignments. */
				if (((i - 1u) & 1u) !=
					    (changed < 3u ? 0u : 1u) ||
				    first + second != 2u * HCELL)
					continue;
				if (first >= crossing_shift +
					    OPENVLC_TEST_GLITCH_MIN)
					pair_edges[i] -= crossing_shift;
				else if (second >= crossing_shift +
						 OPENVLC_TEST_GLITCH_MIN)
					pair_edges[i] += crossing_shift;
				else
					continue;
				changed++;
			}
			if (changed != 6u) {
				fprintf(stderr,
					"not enough one-cell pairs for pair-timing regression\n");
				return 1;
			}
		}
		memset(&rx, 0, sizeof(rx));
		memset(&quality, 0, sizeof(quality));
		status = openvlc_rx_edges_to_packet(
			&cfg, pair_edges, dcd_edge_count, scratch,
			OPENVLC_RX_SAMPLE_BUFFER_LEN, &rx, &quality);
		if (status != OPENVLC_OK ||
		    rx.payload_len != tx.payload_len ||
		    memcmp(rx.payload, tx.payload, tx.payload_len) != 0) {
			fprintf(stderr,
				"pair-timing recovery failed: status=%d len=%u mode=%lu\n",
				(int)status, rx.payload_len,
				(unsigned long)openvlc_phy_dbg_sfdsync_mode);
			return 1;
		}
		printf("comparator pair-timing recovery OK: mode=%lu\n",
		       (unsigned long)openvlc_phy_dbg_sfdsync_mode);

		/*
		 * Regression for the two captured Pi-HAT failures: a real
		 * level-0 two-cell interval can land exactly on the biased
		 * 58-tick decision boundary. The primary round-down path loses
		 * one cell and remains half a cell out of phase; the measured
		 * boundary rule must participate in restoring the original payload.
		 */
		if ((HCELL % OPENVLC_COMP_RUN_BIAS_DIV) == 0u) {
			memcpy(tie_edges, dcd_edges,
			       dcd_edge_count * sizeof(tie_edges[0]));
			{
			size_t tie_edge = 0u;
			const uint32_t tie_ticks =
				HCELL - OPENVLC_TEST_DCD_DELAY +
				HCELL / 2u +
				HCELL / OPENVLC_COMP_RUN_BIAS_DIV;

			for (size_t i = 100u; i < dcd_edge_count; i++) {
				uint32_t run = tie_edges[i] - tie_edges[i - 1u];
				uint32_t parity = (uint32_t)((i - 1u) & 1u);

				if (parity == 0u && run == 2u * HCELL -
				    OPENVLC_TEST_DCD_DELAY) {
					uint32_t shift = run - tie_ticks;

					for (size_t j = i; j < dcd_edge_count; j++)
						tie_edges[j] -= shift;
					tie_edge = i;
					break;
				}
			}
			if (!tie_edge) {
				fprintf(stderr,
					"no level-0 double interval for tie regression\n");
				return 1;
			}
			}
			memset(&rx, 0, sizeof(rx));
			memset(&quality, 0, sizeof(quality));
			status = openvlc_rx_edges_to_packet(
				&cfg, tie_edges, dcd_edge_count, scratch,
				OPENVLC_RX_SAMPLE_BUFFER_LEN, &rx, &quality);
			if (status != OPENVLC_OK ||
			    rx.payload_len != tx.payload_len ||
			    memcmp(rx.payload, tx.payload,
				   tx.payload_len) != 0 ||
			    openvlc_phy_dbg_sfdsync_mode !=
				    (OPENVLC_RX_PRIMARY_PHASE_RECOVERY ? 19u : 3u)) {
				fprintf(stderr,
					"primary timing-tie recovery failed: status=%d len=%u mode=%lu\n",
					(int)status, rx.payload_len,
					(unsigned long)
						openvlc_phy_dbg_sfdsync_mode);
				return 1;
			}
			printf("primary timing-tie recovery OK: mode=%lu\n",
			       (unsigned long)openvlc_phy_dbg_sfdsync_mode);
		} else {
			printf("level-0 timing-tie recovery N/A: fractional bias\n");
		}

		/*
		 * A second real trace had two stretched level-1 two-cell runs at
		 * the exact 80-tick shoulder. The primary path calls both three
		 * cells and is out of phase only between them. Reproduce that
		 * bounded window and require the measured shoulder rule in the
		 * boundary-enabled path.
		 */
		memcpy(shoulder_edges, dcd_edges,
		       dcd_edge_count * sizeof(shoulder_edges[0]));
		{
			uint32_t changed = 0u;
			const uint32_t normal_ticks =
				2u * HCELL + OPENVLC_TEST_DCD_DELAY;
			const uint32_t shoulder_ticks =
				HCELL + OPENVLC_TEST_DCD_DELAY +
				2u * HCELL - HCELL / 4u + 1u;

			for (size_t i = dcd_edge_count / 4u;
			     i < dcd_edge_count &&
			     changed < 2u; i++) {
				uint32_t run =
					shoulder_edges[i] - shoulder_edges[i - 1u];
				uint32_t parity = (uint32_t)((i - 1u) & 1u);

				if (parity != 1u || run != normal_ticks ||
				    (changed == 1u && i < dcd_edge_count / 2u))
					continue;
				for (size_t j = i; j < dcd_edge_count; j++)
					shoulder_edges[j] +=
						shoulder_ticks - normal_ticks;
				changed++;
			}
			if (changed != 2u) {
				fprintf(stderr,
					"not enough level-1 intervals for shoulder regression\n");
				return 1;
			}
		}
		memset(&rx, 0, sizeof(rx));
		memset(&quality, 0, sizeof(quality));
		status = openvlc_rx_edges_to_packet(
			&cfg, shoulder_edges, dcd_edge_count, scratch,
			OPENVLC_RX_SAMPLE_BUFFER_LEN, &rx, &quality);
		if (status != OPENVLC_OK ||
		    rx.payload_len != tx.payload_len ||
		    memcmp(rx.payload, tx.payload, tx.payload_len) != 0 ||
		    openvlc_phy_dbg_sfdsync_mode !=
			    (OPENVLC_RX_PRIMARY_PHASE_RECOVERY ? 19u : 3u)) {
			fprintf(stderr,
				"primary timing-shoulder recovery failed: status=%d len=%u mode=%lu\n",
				(int)status, rx.payload_len,
				(unsigned long)openvlc_phy_dbg_sfdsync_mode);
			return 1;
		}
		printf("primary timing-shoulder recovery OK: mode=%lu\n",
		       (unsigned long)openvlc_phy_dbg_sfdsync_mode);

		/*
		 * Real input capture has no synthetic edge at frame start. Drop
		 * the first timestamp and verify that preamble-local timing
		 * recovery still finds the SFD for several crossing delays.
		 */
		for (uint32_t delay = OPENVLC_TEST_DCD_STEP;
		     delay <= OPENVLC_TEST_DCD_DELAY;
		     delay += OPENVLC_TEST_DCD_STEP) {
			size_t generated = 0u;

			t = 0u;
			for (size_t i = 1; i < symbol_len; i++) {
				t += HCELL;
				if (symbols[i] != symbols[i - 1]) {
					uint32_t crossing_delay =
						symbols[i] ? delay : 0u;

					if (generated != 0u)
						clipped_edges[generated - 1u] =
							t + crossing_delay;
					generated++;
				}
			}
			if (generated < 2u)
				return 1;
			clipped_edge_count = generated - 1u;
			memset(&rx, 0, sizeof(rx));
			memset(&quality, 0, sizeof(quality));
			status = openvlc_rx_edges_to_packet(
				&cfg, clipped_edges, clipped_edge_count,
				scratch, OPENVLC_RX_SAMPLE_BUFFER_LEN,
				&rx, &quality);
			if (status != OPENVLC_OK ||
			    rx.payload_len != tx.payload_len ||
			    memcmp(rx.payload, tx.payload,
				   tx.payload_len) != 0 ||
			    quality.samples_per_symbol != HCELL) {
				fprintf(stderr,
					"clipped duty-cycle decode failed: delay=%lu status=%d len=%u edges=%zu half=%u\n",
					(unsigned long)delay, (int)status,
					rx.payload_len,
					clipped_edge_count,
					quality.samples_per_symbol);
				return 1;
			}
		}
		printf("comparator clipped-preamble recovery OK\n");

		/*
		 * Suppress one complete one-cell pulse in the final half of the raw
		 * preamble. Two missing transitions merge three alternating cells
		 * into one measured three-cell gap. The edge decoder must reinsert
		 * the missing centre pulse before applying the strict preamble gate.
		 */
		for (size_t i = 0; i < ideal_edge_count; i++) {
			if (i == 40u || i == 41u)
				continue;
			missing_pulse_edges[missing_pulse_edge_count++] =
				ideal_edges[i];
		}
		memset(&rx, 0, sizeof(rx));
		memset(&quality, 0, sizeof(quality));
		status = openvlc_rx_edges_to_packet(
			&cfg, missing_pulse_edges, missing_pulse_edge_count,
			scratch, OPENVLC_RX_SAMPLE_BUFFER_LEN, &rx, &quality);
		if (status != OPENVLC_OK ||
		    rx.payload_len != tx.payload_len ||
		    memcmp(rx.payload, tx.payload, tx.payload_len) != 0) {
			fprintf(stderr,
				"missing-pulse recovery failed: status=%d len=%u edges=%zu half=%u\n",
				(int)status, rx.payload_len,
				missing_pulse_edge_count,
				quality.samples_per_symbol);
			return 1;
		}
		printf("comparator missing-pulse recovery OK: half=%u ticks\n",
		       quality.samples_per_symbol);

		/*
		 * Reproduce distributed missing comparator transitions: two SFD
		 * cells are damaged and sparse transitions are suppressed through
		 * the payload. A recovered frame is valid only if every decoded
		 * field and payload byte matches; this also guards the CRC-based
		 * three-to-two-cell retry against an accidental acceptance.
		 */
		memcpy(distorted_symbols, symbols,
		       symbol_len * sizeof(distorted_symbols[0]));
		distorted_symbols[OPENVLC_PREAMBLE_BITS + 2u] =
			distorted_symbols[OPENVLC_PREAMBLE_BITS + 3u];
		distorted_symbols[OPENVLC_PREAMBLE_BITS + 6u] =
			distorted_symbols[OPENVLC_PREAMBLE_BITS + 7u];
		/* Keep the physical length intact; this case exercises payload
		 * reconstruction and FEC, not an unknowable corrupted frame size. */
		for (size_t pair = 20u;
		     OPENVLC_PREAMBLE_BITS + 16u + 2u * pair + 1u <
			     symbol_len;
		     pair += 256u) {
			size_t first =
				OPENVLC_PREAMBLE_BITS + 16u + 2u * pair;

#if OPENVLC_SFD_SYNC_BAD_PAIR_INVERT_FIRST
			/* Pi HAT policy: first cell survives, second crossing is lost. */
			distorted_symbols[first + 1u] =
				distorted_symbols[first];
#else
			/* Alternate analog path: second cell survives. */
			distorted_symbols[first] =
				distorted_symbols[first + 1u];
#endif
		}
		t = 0u;
		distorted_edges[distorted_edge_count++] = 0u;
		for (size_t i = 1u; i < symbol_len; i++) {
			t += HCELL;
			if (distorted_symbols[i] != distorted_symbols[i - 1u])
				distorted_edges[distorted_edge_count++] = t;
		}
		memset(&rx, 0, sizeof(rx));
		memset(&quality, 0, sizeof(quality));
		status = openvlc_rx_edges_to_packet(
			&cfg, distorted_edges, distorted_edge_count, scratch,
			OPENVLC_RX_SAMPLE_BUFFER_LEN, &rx, &quality);
#if OPENVLC_RX_THREE_CELL_RETRY_MAX
		if (status != OPENVLC_OK ||
		    rx.dst != tx.dst || rx.src != tx.src ||
		    rx.protocol != tx.protocol ||
		    rx.payload_len != tx.payload_len ||
		    memcmp(rx.payload, tx.payload, tx.payload_len) != 0) {
			fprintf(stderr,
				"distributed transition recovery failed: status=%d parse=%ld len=%u edges=%zu bad=%u/%lu run=%u mode=%lu edits=%lu bits=%lu\n",
				(int)status,
				(long)openvlc_phy_dbg_parse_status,
				rx.payload_len,
				distorted_edge_count,
				quality.manchester_bad_pairs,
				(unsigned long)quality.manchester_pairs,
				quality.manchester_max_bad_run,
				(unsigned long)openvlc_phy_dbg_sfdsync_mode,
				(unsigned long)openvlc_phy_dbg_phase_edits,
				(unsigned long)openvlc_phy_dbg_sfdsync_maxbits);
			return 1;
		}
		printf("distributed transition recovery OK: mode=%lu edits=%lu\n",
		       (unsigned long)openvlc_phy_dbg_sfdsync_mode,
		       (unsigned long)openvlc_phy_dbg_phase_edits);
#else
		if (status == OPENVLC_OK) {
			fputs("bounded baseline unexpectedly accepted distributed "
			      "transition damage\n", stderr);
			return 1;
		}
		printf("distributed transition retry disabled: status=%d\n",
		       (int)status);
#endif

		{
			openvlc_quality_t corrected = jittered_quality;
			uint8_t corrected_lqi =
				openvlc_quality_finalize(&corrected, tx.payload_len,
							 true, 8);
			uint8_t repeated_lqi =
				openvlc_quality_finalize(&corrected, tx.payload_len,
							 true, 8);

			if (corrected_lqi >= jittered_lqi ||
			    repeated_lqi != corrected_lqi ||
			    corrected.rs_corrected_bytes != 8u ||
			    corrected.rs_correction_x1000 == 0u) {
				fprintf(stderr,
					"RS LQI penalty failed: base=%u corrected=%u repeated=%u rs=%u rate=%lu\n",
					jittered_lqi, corrected_lqi, repeated_lqi,
					corrected.rs_corrected_bytes,
					(unsigned long)corrected.rs_correction_x1000);
				return 1;
			}
		}

		/*
		 * Insert narrow pulse pairs inside long runs; pair cancellation
		 * must recover the exact edge sequence before decoding.
		 */
		noisy_edges[noisy_count++] = edges[0];
		for (size_t i = 1; i < edge_count; i++) {
			uint32_t run = edges[i] - edges[i - 1u];

			if (run >= 2u * HCELL - 4u &&
			    noisy_count + 3u < OPENVLC_MAX_SYMBOLS * 2u) {
				noisy_edges[noisy_count++] =
					edges[i - 1u] + GLITCH_MIN + 2u;
				noisy_edges[noisy_count++] =
					edges[i - 1u] + GLITCH_MIN + 5u;
			}
			noisy_edges[noisy_count++] = edges[i];
		}
		memcpy(contextual_edges, noisy_edges,
		       noisy_count * sizeof(contextual_edges[0]));
		{
			uint32_t removed = 0u;
			size_t contextual_count = openvlc_edge_filter_timing_aware(
				contextual_edges, noisy_count, GLITCH_MIN,
				OPENVLC_EDGE_HARD_GLITCH_TICKS,
				OPENVLC_EDGE_CONTEXT_MARGIN_TICKS, &removed);

			if (contextual_count != edge_count || removed == 0u ||
			    memcmp(contextual_edges, edges,
				   edge_count * sizeof(edges[0])) != 0) {
				fprintf(stderr,
					"contextual edge filter failed: clean=%zu expected=%zu removed=%lu\n",
					contextual_count, edge_count,
					(unsigned long)removed);
				return 1;
			}
		}
		noisy_count = openvlc_edge_cancel_short_pulses(
			noisy_edges, noisy_count, GLITCH_MIN);
		if (noisy_count != edge_count ||
		    memcmp(noisy_edges, edges, edge_count * sizeof(edges[0])) != 0) {
			fprintf(stderr,
				"edge cancellation failed: clean=%zu expected=%zu\n",
				noisy_count, edge_count);
			return 1;
		}

		/* Decode the deglitched stream. */
		memset(&rx, 0, sizeof(rx));
		memset(&quality, 0, sizeof(quality));
		status = openvlc_rx_edges_to_packet(&cfg, noisy_edges, noisy_count,
						    scratch,
						    OPENVLC_RX_SAMPLE_BUFFER_LEN,
						    &rx, &quality);
		if (status != OPENVLC_OK || rx.payload_len != tx.payload_len ||
		    memcmp(rx.payload, tx.payload, tx.payload_len) != 0) {
			fprintf(stderr,
				"deglitched decode failed: status=%d len=%u edges=%zu\n",
				(int)status, rx.payload_len, noisy_count);
			return 1;
		}
		printf("comparator deglitch OK: edges=%zu payload=%u\n",
		       noisy_count, rx.payload_len);

#if OPENVLC_TEST_HCELL == 32u
		/*
		 * A 15-tick crossing displacement produces valid 17/47-tick cells.
		 * They fall inside the old 20-tick blind gate and must survive the
		 * contextual filter after its packet-local model learns both levels.
		 */
		{
			size_t short_count = 0u;
			uint32_t removed = 0u;

			t = 0u;
			short_cell_edges[short_count++] = 0u;
			for (size_t i = 1u; i < symbol_len; i++) {
				t += HCELL;
				if (symbols[i] != symbols[i - 1u])
					short_cell_edges[short_count++] =
						t + (symbols[i] ? 15u : 0u);
			}
			short_count = openvlc_edge_filter_timing_aware(
				short_cell_edges, short_count, GLITCH_MIN,
				OPENVLC_EDGE_HARD_GLITCH_TICKS,
				OPENVLC_EDGE_CONTEXT_MARGIN_TICKS, &removed);
			if (removed != 0u) {
				fprintf(stderr,
					"contextual filter removed valid 17-tick cells: %lu edges\n",
					(unsigned long)removed);
				return 1;
			}
			memset(&rx, 0, sizeof(rx));
			memset(&quality, 0, sizeof(quality));
			status = openvlc_rx_edges_to_packet(
				&cfg, short_cell_edges, short_count, scratch,
				OPENVLC_RX_SAMPLE_BUFFER_LEN, &rx, &quality);
			if (status != OPENVLC_OK ||
			    rx.payload_len != tx.payload_len ||
			    memcmp(rx.payload, tx.payload, tx.payload_len) != 0) {
				fprintf(stderr,
					"17/47-tick valid-cell decode failed: status=%d len=%u\n",
					(int)status, rx.payload_len);
				return 1;
			}
		}
#endif
		if (!run_synthetic_channel_campaign(&cfg, &tx, symbols, symbol_len,
						    HCELL))
			return 1;
	}
	return 0;
}
