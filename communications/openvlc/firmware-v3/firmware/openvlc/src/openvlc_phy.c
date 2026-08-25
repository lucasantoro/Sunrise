/*
 * openvlc_phy.c - OpenVLC physical layer for the STM32 port.
 *
 * ============================================================================
 * RX PIPELINE (the only RX path on STM32: optical comparator front-end)
 * ============================================================================
 *
 *   light -> photodiode/AGC -> PB0 ---------------\
 *                                        COMP1 (+) vs DAC1_CH1 threshold (-)
 *   COMP1 output --> TIM2_CH4 input capture (both edges) --> DMA ring
 *                                                                |
 *   platform poll groups captures into a "burst" on idle gaps,  |
 *   filters narrow pulses with packet-local timing, and hands    v
 *   the edge-timestamp array to openvlc_rx_edges_to_packet().
 *
 * A "burst" is therefore an array of TIM2 tick timestamps, one per comparator
 * transition. The optical line is OOK: the signal level simply toggles on every
 * edge, so all timing information lives in the gaps BETWEEN edges.
 *
 * ----------------------------------------------------------------------------
 * ON-AIR FRAME (produced by the BeagleBone TX, openvlc_frame.c mirrors it)
 * ----------------------------------------------------------------------------
 *   [ preamble 8x 0xAA ][ SFD 0xA3 ][ len(2) | header(4) | payload | FEC ]
 *     |__ sent RAW (one bit per cell)    |__ everything from the SFD on is
 *         -> a steady 1010... square          Manchester-coded (1 = LOW-HIGH,
 *            wave, all "single" cells          0 = HIGH-LOW), i.e. two cells
 *                                              per bit. The BeagleBone wraps
 *                                              the payload in Reed-Solomon
 *                                              (8 byte corrections / block).
 *
 * In the edge stream this means:
 *   - a "single" gap  (~1 cell)  = one line cell;
 *   - a "double" gap  (~2 cells) = two equal cells (a Manchester bit boundary
 *                                  between two differing data bits).
 *
 * ----------------------------------------------------------------------------
 * DECODER (comp_decode_sfd_sync + comp_sfd_sync_pass)
 * ----------------------------------------------------------------------------
 *   1. comp_estimate_timing_model(): find the alternating raw-preamble run and
 *      estimate separate one-cell durations for the two edge polarities.
 *      Their average is the nominal half-cell; whole-burst histograms are only
 *      a fallback when the preamble timing run is unavailable.
 *   2. Expand every gap with its polarity-specific one-cell duration and the
 *      nominal period for additional cells.
 *   3. Slide a 16-symbol shift register; an SFD pattern match is accepted only
 *      when the immediately preceding symbols contain the alternating raw
 *      preamble training sequence. The SFD inverse also resolves unknown
 *      optical polarity. This is the preamble+SFD synchronisation, mirroring
 *      the BeagleBone PRU receiver without locking on an isolated payload/noise
 *      occurrence of 0xA3.
 *   4. From the lock, Manchester-pair the symbols and feed the recovered bits
 *      into the framing state machine (manchester_stream_feed_bit): find SFD,
 *      read the 16-bit length, collect the payload, then openvlc_frame_parse()
 *      validates native CRC or applies BeagleBone Reed-Solomon.
 *   A corrupted pair is passed through as a best-effort bit (RS repairs it); a
 *      burst-clipped final bit is flushed so the last frame still completes.
 *
 * TX is intentionally separate: openvlc_tx_compat.c builds the BeagleBone wire
 * format directly into TIM1 output-compare DMA words.
 *
 * The openvlc_phy_dbg_* globals below are bring-up telemetry only.
 */

#include "openvlc_phy.h"

#include <string.h>

#include "openvlc_frame.h"
#include "openvlc_linecode.h"

#include "openvlc_app.h"

#ifndef OPENVLC_DECODER_DIAGNOSTICS
#if defined(OPENVLC_TEST_API)
#define OPENVLC_DECODER_DIAGNOSTICS 1
#else
#define OPENVLC_DECODER_DIAGNOSTICS 0
#endif
#endif

#ifndef OPENVLC_RX_QUALITY_DECIMATION
#define OPENVLC_RX_QUALITY_DECIMATION 1u
#endif

#if defined(__GNUC__) && (defined(STM32H743xx) || defined(STM32H723xx))
#define OPENVLC_BULK_BUFFER __attribute__((section(".rx_buffer"), aligned(32)))
#define OPENVLC_RX_HOT __attribute__((hot, optimize("O3")))
#define OPENVLC_RX_ALWAYS_INLINE static inline __attribute__((always_inline))
#elif defined(__GNUC__)
#define OPENVLC_BULK_BUFFER
#define OPENVLC_RX_HOT __attribute__((hot, optimize("O3")))
#define OPENVLC_RX_ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#define OPENVLC_BULK_BUFFER
#define OPENVLC_RX_HOT
#define OPENVLC_RX_ALWAYS_INLINE static inline
#endif

/*
 * Decoder diagnostics. The platform samples these into the periodic serial log
 * during bring-up. They are best-effort snapshots of the last decoded burst
 * (no synchronisation) and have no functional role.
 */
volatile uint32_t openvlc_phy_dbg_stage;        /* 20 = entered, 22 = SFD-sync OK */
volatile uint32_t openvlc_phy_dbg_sample_len;   /* edge count in the burst */
volatile uint32_t openvlc_phy_dbg_sps;          /* estimated half-cell, ticks */
volatile uint32_t openvlc_phy_dbg_len_raw;      /* raw 16-bit length field */
volatile uint32_t openvlc_phy_dbg_payload_len;  /* decoded payload bytes */
volatile int32_t  openvlc_phy_dbg_parse_status; /* frame-parse status code */

/* SFD-correlated decoder per-burst detail (see comp_decode_sfd_sync). */
volatile uint32_t openvlc_phy_dbg_sfdsync_single;  /* half-cell ticks used */
volatile uint32_t openvlc_phy_dbg_sfdsync_split;   /* single/double boundary */
volatile uint32_t openvlc_phy_dbg_sfdsync_cell0;   /* parity-0 one-cell ticks */
volatile uint32_t openvlc_phy_dbg_sfdsync_cell1;   /* parity-1 one-cell ticks */
volatile uint32_t openvlc_phy_dbg_sfdsync_train;   /* preamble intervals used */
volatile uint32_t openvlc_phy_dbg_sfdsync_syncs;   /* SFD locks in the burst */
volatile uint32_t openvlc_phy_dbg_sfdsync_maxbits; /* bits fed to the framer */
volatile uint32_t openvlc_phy_dbg_sfdsync_lenraw;  /* length seen after SFD */
volatile uint32_t openvlc_phy_dbg_sfdsync_result;  /* 1 = frame decoded OK */
volatile uint32_t openvlc_phy_dbg_sfdsync_pre_rejects; /* SFD without preamble */
volatile uint32_t openvlc_phy_dbg_sfdsync_pre_badmax; /* worst preamble damage */
volatile uint32_t openvlc_phy_dbg_sfdsync_pre_sfdmin; /* best rejected SFD */
/*
 * Cell index of the most recent SFD lock. A genuine lock sits at the end of
 * optional TX warm-up + preamble + SFD. With the 384-cell STM32 warm-up this
 * is near 464 cells; without warm-up it is near 80 cells.
 */
volatile uint32_t openvlc_phy_dbg_sfdsync_lock_cell;
volatile uint32_t openvlc_phy_dbg_sfdsync_sfd_errors; /* accepted SFD cell errors */
volatile uint32_t openvlc_phy_dbg_sfdsync_relocks; /* SFD locks abandoned in-burst */
volatile uint32_t openvlc_phy_dbg_sfdsync_mode; /* repair/bad-pair hypothesis */
volatile uint32_t openvlc_phy_dbg_phase_edits; /* one-cell phase repairs */
volatile uint32_t openvlc_phy_dbg_list_trials; /* CRC-gated list paths tried */
volatile uint32_t openvlc_phy_dbg_local_trials; /* local frame parses tried */
/*
 * Runtime pass budget written by the capture platform:
 * 0 = configured maximum, 1/2 = bound work while RX/TX is busy.
 */
volatile uint32_t openvlc_rx_hypothesis_budget;
#if defined(OPENVLC_TEST_API)
static size_t openvlc_test_override_interval[3] = {
	SIZE_MAX, SIZE_MAX, SIZE_MAX
};
static int openvlc_test_override_delta[3];

void openvlc_test_set_cell_overrides(size_t first_interval, int first_delta,
				     size_t second_interval, int second_delta,
				     size_t third_interval, int third_delta)
{
	openvlc_test_override_interval[0] = first_interval;
	openvlc_test_override_interval[1] = second_interval;
	openvlc_test_override_interval[2] = third_interval;
	openvlc_test_override_delta[0] = first_delta;
	openvlc_test_override_delta[1] = second_delta;
	openvlc_test_override_delta[2] = third_delta;
}
#endif

volatile uint32_t openvlc_phy_dbg_sfdsync_fail_timing; /* timing model reject */
volatile uint32_t openvlc_phy_dbg_sfdsync_fail_no_sfd; /* no usable SFD lock */
volatile uint32_t openvlc_phy_dbg_sfdsync_fail_preamble; /* SFD lacked preamble */
volatile uint32_t openvlc_phy_dbg_sfdsync_fail_parse; /* SFD lock, frame reject */
volatile uint32_t openvlc_phy_dbg_sfdsync_fail_crc; /* CRC/RS reject after SFD */
volatile uint32_t openvlc_phy_dbg_sfdsync_fail_len; /* invalid length after SFD */
volatile uint32_t openvlc_phy_dbg_sfdsync_fail_overflow; /* frame too large */
volatile uint32_t openvlc_phy_dbg_sfdsync_fail_incomplete; /* truncated frame */
volatile uint32_t openvlc_phy_dbg_track_cell0_end; /* final parity-0 cell ticks */
volatile uint32_t openvlc_phy_dbg_track_cell1_end; /* final parity-1 cell ticks */
volatile uint32_t openvlc_phy_dbg_track_nominal_end; /* final nominal cell ticks */
volatile uint32_t openvlc_phy_dbg_timing_residual_peak; /* peak run residual, Q8 */
static uint32_t isqrt64(uint64_t value)
{
	uint64_t bit = (uint64_t)1u << 62;
	uint64_t root = 0;

	while (bit > value)
		bit >>= 2;
	while (bit) {
		if (value >= root + bit) {
			value -= root + bit;
			root = (root >> 1) + bit;
		} else {
			root >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t)root;
}

static uint32_t scale_ratio_x1000(uint32_t numerator, uint32_t denominator)
{
	if (!denominator)
		return 0u;
	return (uint32_t)(((uint64_t)numerator * 1000u + denominator / 2u) /
			  denominator);
}

static void quality_set_edge_metrics(openvlc_quality_t *quality,
				     uint32_t single_ticks,
				     uint64_t timing_error_sq,
				     uint32_t timing_intervals,
				     uint32_t timing_outliers,
				     uint32_t manchester_pairs,
				     uint32_t manchester_bad_pairs)
{
	uint32_t timing_jitter_x1000 = 0u;
	uint32_t bad_x1000;
	uint32_t timing_penalty;
	uint32_t bad_penalty;
	uint32_t lqi;

	if (!quality)
		return;

	if (single_ticks && timing_intervals) {
		uint64_t scaled_mean_sq =
			(timing_error_sq * 1000000u) / timing_intervals;
		uint32_t rms_x1000 = isqrt64(scaled_mean_sq);

		timing_jitter_x1000 =
			(rms_x1000 + single_ticks / 2u) / single_ticks;
	}

	bad_x1000 = scale_ratio_x1000(manchester_bad_pairs,
				      manchester_pairs);

	/*
	 * Packet-level pre-FEC LQI:
	 *   - timing residuals up to 2% are treated as normal quantisation;
	 *   - each additional 0.5% costs one point, capped at 40;
	 *   - invalid Manchester pairs cost up to 30 points.
	 * Reed-Solomon adds the final post-FEC penalty in openvlc_app.c.
	 */
	timing_penalty = timing_jitter_x1000 > 20u ?
		(timing_jitter_x1000 - 20u + 4u) / 5u : 0u;
	if (timing_penalty > 40u)
		timing_penalty = 40u;
	bad_penalty = (bad_x1000 * 3u + 5u) / 10u;
	if (bad_penalty > 30u)
		bad_penalty = 30u;
	lqi = timing_penalty + bad_penalty;
	lqi = lqi >= 100u ? 0u : 100u - lqi;

	quality->signal_power = 0u;
	quality->noise_power = 0u;
	quality->snr_x1000 = 0u;
	quality->snr_db_centi = -100000;
	quality->snr_valid = false;
	quality->timing_jitter_x1000 = timing_jitter_x1000;
	quality->jitter_x1000 = timing_jitter_x1000;
	quality->manchester_bad_x1000 = bad_x1000;
	quality->timing_intervals = timing_intervals;
	quality->timing_outliers = timing_outliers;
	quality->manchester_pairs = manchester_pairs;
	quality->manchester_bad_pairs =
		manchester_bad_pairs > UINT16_MAX ? UINT16_MAX :
		(uint16_t)manchester_bad_pairs;
	quality->pre_fec_quality = (uint8_t)lqi;
	quality->link_quality = (uint8_t)lqi;
}

static void comp_accumulate_dcd_timing(uint32_t run, uint32_t cells,
				       uint32_t base_ticks,
				       uint32_t nominal_ticks,
				       uint64_t *error_sq,
				       uint32_t *intervals,
				       uint32_t *outliers)
{
	uint32_t expected;
	uint32_t residual;

	if (!base_ticks || !nominal_ticks || !cells || !error_sq ||
	    !intervals || !outliers)
		return;
	expected = base_ticks + (cells - 1u) * nominal_ticks;
	residual = run > expected ? run - expected : expected - run;
	/*
	 * Bound one pathological interval to one nominal half-cell so it is
	 * visible without dominating a long valid packet. The expected interval
	 * includes the polarity-specific comparator crossing delay.
	 */
	if (residual > nominal_ticks / 2u)
		(*outliers)++;
	if (residual > nominal_ticks)
		residual = nominal_ticks;
	*error_sq += (uint64_t)residual * residual;
	(*intervals)++;
}

static bool beaglebone_payload_len_from_byte_len(size_t byte_len, uint16_t *payload_len)
{
#if OPENVLC_BEAGLEBONE_COMPAT
	const size_t fixed = OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES + 2u + OPENVLC_HEADER_BYTES;
	const size_t max_blocks =
		(OPENVLC_BEAGLEBONE_ENCODED_BYTES(OPENVLC_MAX_PAYLOAD_BYTES) +
		 OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE - 1u) /
		OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE;

	if (!payload_len || byte_len < fixed + OPENVLC_BEAGLEBONE_RS_ECC_BYTES)
		return false;
	for (size_t blocks = 1u; blocks <= max_blocks; blocks++) {
		size_t ecc = OPENVLC_BEAGLEBONE_RS_ECC_BYTES * blocks;
		size_t candidate;
		size_t encoded_len;
		size_t actual_blocks;

		if (byte_len < fixed + ecc)
			continue;
		candidate = byte_len - fixed - ecc;
		if (candidate > OPENVLC_MAX_PAYLOAD_BYTES)
			continue;
		encoded_len = OPENVLC_BEAGLEBONE_ENCODED_BYTES(candidate);
		actual_blocks =
			(encoded_len + OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE - 1u) /
			OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE;
		if (actual_blocks != blocks)
			continue;
		*payload_len = (uint16_t)candidate;
		return true;
	}
#else
	(void)byte_len;
	(void)payload_len;
#endif
	return false;
}

static bool beaglebone_data_bits_from_symbol_len(uint16_t symbol_len,
						 uint32_t tx_preamble_bits,
						 uint32_t *data_bits_needed)
{
#if OPENVLC_BEAGLEBONE_COMPAT
	const uint32_t spb = (uint32_t)openvlc_symbols_per_byte(OPENVLC_LINE_MANCHESTER);
	/*
	 * Symbols ahead of the payload: the SFD (16 cells per byte), the
	 * 2-byte physical length (32 cells) and the single trailing symbol
	 * that openvlc_tx_compat_frame_to_symbols() appends. This was
	 * hardcoded 49, which silently assumed a ONE-byte SFD: with a 2-byte
	 * SFD the receiver looked for the payload 16 cells early and every
	 * frame ended short (fi = one per frame).
	 */
	const uint32_t symbols_before_data =
		tx_preamble_bits + (16u * OPENVLC_SFD_BYTES) + 33u;
	uint32_t body_symbols;
	uint32_t data_bits;
	size_t artificial_frame_len;
	uint16_t payload_len;

	if (!data_bits_needed || !spb || !tx_preamble_bits)
		return false;
	if (symbol_len <= symbols_before_data)
		return false;
	if (((uint32_t)symbol_len - 1u - tx_preamble_bits) % spb)
		return false;
	body_symbols = (uint32_t)symbol_len - symbols_before_data;
	if (body_symbols % 16u)
		return false;
	data_bits = body_symbols / 2u;
	artificial_frame_len = OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES + 2u +
			       (size_t)data_bits / 8u;
	if (!beaglebone_payload_len_from_byte_len(artificial_frame_len, &payload_len))
		return false;
	(void)payload_len;
	*data_bits_needed = data_bits;
	return true;
#else
	(void)symbol_len;
	(void)tx_preamble_bits;
	(void)data_bits_needed;
	return false;
#endif
}

static bool beaglebone_data_bits_from_any_preamble(uint16_t symbol_len,
						   uint32_t *data_bits_needed)
{
	if (beaglebone_data_bits_from_symbol_len(symbol_len,
						OPENVLC_PREAMBLE_BITS,
						data_bits_needed))
		return true;
	if (OPENVLC_PREAMBLE_BITS != 32u &&
	    beaglebone_data_bits_from_symbol_len(symbol_len, 32u,
						data_bits_needed))
		return true;
	if (OPENVLC_PREAMBLE_BITS != 64u &&
	    beaglebone_data_bits_from_symbol_len(symbol_len, 64u,
						data_bits_needed))
		return true;
	return false;
}
static OPENVLC_RX_HOT void frame_append_bit(
	uint8_t *frame, size_t frame_cap, size_t *frame_len,
	uint8_t bit, bool *ok)
{
	size_t byte_pos;
	uint8_t bit_pos;

	if (!ok || !*ok || !frame || !frame_len)
		return;
	byte_pos = *frame_len / 8u;
	bit_pos = (uint8_t)(7u - (*frame_len % 8u));
	if (byte_pos >= frame_cap) {
		*ok = false;
		return;
	}
	if ((*frame_len % 8u) == 0u)
		frame[byte_pos] = 0;
	if (bit)
		frame[byte_pos] |= (uint8_t)(1u << bit_pos);
	(*frame_len)++;
}

typedef struct {
	uint8_t phase;
	uint32_t cell_index;
	bool have_first;
	bool first_cell;
	enum {
		MANCHESTER_SEARCH_SFD,
		MANCHESTER_READ_LENGTH,
		MANCHESTER_READ_DATA,
	} state;
	uint16_t sfd;   /* SFD shift register, OPENVLC_SFD_BITS wide */
	uint16_t len_raw;
	uint32_t length_bits;
	uint32_t data_bits;
	uint32_t data_bits_needed;
	uint32_t emitted_bits;
	uint32_t sfd_hits;
	uint32_t len_valid_hits;
	uint32_t len_invalid_hits;
	size_t frame_bits;
	bool append_ok;
	uint16_t last_len_raw;
	uint32_t last_data_bits_needed;
	uint32_t last_frame_bits;
	int32_t last_parse_status;
} openvlc_manchester_stream_t;

static void manchester_stream_reset_frame(openvlc_manchester_stream_t *dec)
{
	dec->state = MANCHESTER_SEARCH_SFD;
	dec->sfd = 0;
	dec->len_raw = 0;
	dec->length_bits = 0;
	dec->data_bits = 0;
	dec->data_bits_needed = 0;
	dec->frame_bits = 0;
	dec->append_ok = true;
}

static void manchester_stream_init(openvlc_manchester_stream_t *dec, uint8_t phase)
{
	memset(dec, 0, sizeof(*dec));
	dec->phase = phase;
	dec->append_ok = true;
	dec->last_parse_status = OPENVLC_ERR_SYNC;
}

static void manchester_stream_gap(openvlc_manchester_stream_t *dec)
{
	dec->cell_index = 0;
	dec->have_first = false;
	manchester_stream_reset_frame(dec);
}

static OPENVLC_RX_HOT openvlc_status_t manchester_stream_feed_bit(
	const openvlc_runtime_config_t *cfg,
	openvlc_manchester_stream_t *dec, uint8_t bit,
	openvlc_packet_t *packet,
	uint8_t *frame,
	size_t frame_cap,
	size_t *out_frame_len,
	uint16_t *out_len_raw)
{
	dec->emitted_bits++;
	if (dec->state == MANCHESTER_SEARCH_SFD) {
		dec->sfd = (uint16_t)(((dec->sfd << 1) | bit) &
				      ((1u << OPENVLC_SFD_BITS) - 1u));
		if (dec->sfd != (uint16_t)OPENVLC_SFD_WORD)
			return OPENVLC_ERR_SYNC;

		dec->sfd_hits++;
		for (size_t p = 0; p < OPENVLC_PREAMBLE_BYTES; p++)
			frame[p] = OPENVLC_PREAMBLE_BYTE;
		{
			size_t sfd_at = OPENVLC_PREAMBLE_BYTES;
			OPENVLC_SFD_EMIT(frame, sfd_at);
		}
		dec->frame_bits = (OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES) * 8u;
		dec->append_ok = true;
		dec->len_raw = 0;
		dec->length_bits = 0;
		dec->data_bits = 0;
		dec->data_bits_needed = 0;
		dec->state = MANCHESTER_READ_LENGTH;
		return OPENVLC_ERR_SYNC;
	}

	frame_append_bit(frame, frame_cap, &dec->frame_bits, bit, &dec->append_ok);
	if (!dec->append_ok) {
		manchester_stream_reset_frame(dec);
		return OPENVLC_ERR_SYNC;
	}

	if (dec->state == MANCHESTER_READ_LENGTH) {
		dec->len_raw = (uint16_t)((dec->len_raw << 1) | bit);
		dec->length_bits++;
		if (dec->length_bits != 16u)
			return OPENVLC_ERR_SYNC;

		if (dec->len_raw > OPENVLC_MAX_PAYLOAD_BYTES) {
			if (!beaglebone_data_bits_from_any_preamble(
				    dec->len_raw, &dec->data_bits_needed)) {
				dec->len_invalid_hits++;
				dec->last_len_raw = dec->len_raw;
				dec->last_data_bits_needed = 0;
				dec->last_frame_bits = (uint32_t)dec->frame_bits;
				dec->last_parse_status = OPENVLC_ERR_ARG;
				manchester_stream_reset_frame(dec);
				return OPENVLC_ERR_SYNC;
			}
		} else {
			dec->data_bits_needed = (OPENVLC_HEADER_BYTES - 2u +
						 (uint32_t)dec->len_raw +
						 OPENVLC_CRC_BYTES) * 8u;
		}
		if (!dec->data_bits_needed ||
		    dec->frame_bits + dec->data_bits_needed > frame_cap * 8u) {
			dec->len_invalid_hits++;
			dec->last_len_raw = dec->len_raw;
			dec->last_data_bits_needed = dec->data_bits_needed;
			dec->last_frame_bits = (uint32_t)dec->frame_bits;
			dec->last_parse_status = OPENVLC_ERR_OVERFLOW;
			manchester_stream_reset_frame(dec);
			return OPENVLC_ERR_SYNC;
		}
		dec->last_len_raw = dec->len_raw;
		dec->len_valid_hits++;
		dec->last_data_bits_needed = dec->data_bits_needed;
		dec->last_frame_bits = (uint32_t)dec->frame_bits;
		dec->last_parse_status = OPENVLC_ERR_SYNC;
		dec->state = MANCHESTER_READ_DATA;
		return OPENVLC_ERR_SYNC;
	}

	dec->data_bits++;
	if (dec->state == MANCHESTER_READ_DATA &&
	    dec->data_bits >= dec->data_bits_needed) {
		openvlc_status_t status;
		size_t frame_len;

		if (dec->frame_bits % 8u) {
			dec->last_len_raw = dec->len_raw;
			dec->last_data_bits_needed = dec->data_bits_needed;
			dec->last_frame_bits = (uint32_t)dec->frame_bits;
			dec->last_parse_status = OPENVLC_ERR_SYNC;
			manchester_stream_reset_frame(dec);
			return OPENVLC_ERR_SYNC;
		}
		frame_len = dec->frame_bits / 8u;
		status = openvlc_frame_parse(frame, frame_len, packet);
		(void)cfg;
		dec->last_len_raw = dec->len_raw;
		dec->last_data_bits_needed = dec->data_bits_needed;
		dec->last_frame_bits = (uint32_t)dec->frame_bits;
		dec->last_parse_status = status;
		if (status == OPENVLC_OK) {
			*out_frame_len = frame_len;
			if (out_len_raw)
				*out_len_raw = dec->len_raw;
			return OPENVLC_OK;
		}
		manchester_stream_reset_frame(dec);
	}
	return OPENVLC_ERR_SYNC;
}

#ifndef OPENVLC_COMP_MIN_HALFCELL_TICKS
#define OPENVLC_COMP_MIN_HALFCELL_TICKS 2u
#endif

#ifndef OPENVLC_EDGE_MIN_INTERVAL_TICKS
#define OPENVLC_EDGE_MIN_INTERVAL_TICKS 14u
#endif

#ifndef OPENVLC_COMP_THRESHOLD_DAC
#define OPENVLC_COMP_THRESHOLD_DAC 2700u
#endif

#ifndef OPENVLC_COMP_SPLIT_HIST_MAX_TICKS
#define OPENVLC_COMP_SPLIT_HIST_MAX_TICKS 96u
#endif

#ifndef OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS
#define OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS 28u
#endif

#ifndef OPENVLC_COMP_NOMINAL_HALFCELL_TICKS
#define OPENVLC_COMP_NOMINAL_HALFCELL_TICKS 0u
#endif

#ifndef OPENVLC_COMP_MAX_RUN_CELLS
#define OPENVLC_COMP_MAX_RUN_CELLS 8u
#endif

/*
 * Adaptive one-cell timing anchor. The compile-time
 * OPENVLC_COMP_NOMINAL_HALFCELL_TICKS is only the SEED/prior used to reject
 * false histogram peaks; the true cell duration is measured per burst from the
 * 0xAA preamble (comp_estimate_preamble_timing). Track that measured value
 * across bursts so the anchor follows slow rate/temperature/clock drift, while
 * the compile-time value bounds it to +-40% so a corrupt burst can never drag
 * the estimator onto a wrong period. Seeded to the compile-time nominal and
 * updated only after a fully validated (CRC/RS-good) decode.
 */
#if OPENVLC_COMP_NOMINAL_HALFCELL_TICKS > 0u
static uint32_t g_nominal_anchor = OPENVLC_COMP_NOMINAL_HALFCELL_TICKS;
#define OPENVLC_NOMINAL_ANCHOR_MIN \
	((OPENVLC_COMP_NOMINAL_HALFCELL_TICKS * 3u) / 5u)
#define OPENVLC_NOMINAL_ANCHOR_MAX \
	((OPENVLC_COMP_NOMINAL_HALFCELL_TICKS * 7u) / 5u)

#ifndef OPENVLC_COMP_ADAPTIVE_ANCHOR
#define OPENVLC_COMP_ADAPTIVE_ANCHOR 1
#endif

static void comp_anchor_update(uint32_t measured_nominal)
{
#if !OPENVLC_COMP_ADAPTIVE_ANCHOR
	/* A/B off: anchor stays at the compile-time seed (== old behaviour). */
	(void)measured_nominal;
	return;
#else
	if (measured_nominal < OPENVLC_NOMINAL_ANCHOR_MIN ||
	    measured_nominal > OPENVLC_NOMINAL_ANCHOR_MAX)
		return;
	/* First-order low-pass (3/4 old + 1/4 new) to reject per-burst jitter. */
	g_nominal_anchor = (g_nominal_anchor * 3u + measured_nominal + 2u) / 4u;
#endif
}

volatile uint32_t openvlc_phy_dbg_nominal_anchor; /* current adaptive anchor */
#endif

size_t openvlc_edge_cancel_short_pulses(uint32_t *edges, size_t edge_count,
				       uint32_t min_interval)
{
	size_t out = 0;

	if (!edges || !edge_count || !min_interval)
		return edge_count;

	for (size_t i = 0; i < edge_count; i++) {
		uint32_t edge = edges[i];

		if (out && edge - edges[out - 1u] < min_interval) {
			/*
			 * A narrow comparator pulse contributes a rising/falling pair.
			 * Pop its first edge and discard this second edge. Keeping only
			 * one of them would invert every following Manchester level.
			 */
			out--;
			continue;
		}
		edges[out++] = edge;
	}
	return out;
}

/*
 * Estimate the cell timing of a burst from the gap histogram. Returns the
 * single/double split (a gap below it is one cell, above it is two) and reports
 * the two histogram peaks: *out_short_peak = half-cell ticks ("single"),
 * *out_long_peak = full-cell ticks ("double"). Returns 0 if no clear peak.
 */
static OPENVLC_RX_HOT uint32_t comp_estimate_pru_split(
	const uint32_t *edges, size_t edge_count,
	uint32_t *out_short_peak, uint32_t *out_long_peak)
{
	uint16_t hist[OPENVLC_COMP_SPLIT_HIST_MAX_TICKS + 1u];
	uint32_t short_peak = 0;
	uint32_t long_peak = 0;
	uint32_t short_count = 0;
	uint32_t long_count = 0;
	uint32_t short_max = OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS;
	uint32_t long_min;

	if (!edges || edge_count < 2u)
		return 0;
	memset(hist, 0, sizeof(hist));
	for (size_t i = 1; i < edge_count; i++) {
		uint32_t run = edges[i] - edges[i - 1u];

		if (run > OPENVLC_COMP_SPLIT_HIST_MAX_TICKS)
			continue;
		if (hist[run] != UINT16_MAX)
			hist[run]++;
	}
	if (short_max > OPENVLC_COMP_SPLIT_HIST_MAX_TICKS)
		short_max = OPENVLC_COMP_SPLIT_HIST_MAX_TICKS;
	for (uint32_t t = OPENVLC_COMP_MIN_HALFCELL_TICKS; t <= short_max; t++) {
		if (hist[t] > short_count) {
			short_count = hist[t];
			short_peak = t;
		}
	}
	if (!short_peak)
		return 0;

	long_min = short_peak + (short_peak / 3u);
	if (long_min <= short_peak)
		long_min = short_peak + 1u;
	for (uint32_t t = long_min; t <= OPENVLC_COMP_SPLIT_HIST_MAX_TICKS; t++) {
		if (hist[t] > long_count) {
			long_count = hist[t];
			long_peak = t;
		}
	}
	if (out_short_peak)
		*out_short_peak = short_peak;
	if (out_long_peak)
		*out_long_peak = long_peak;
	/*
	 * The PRU decoder only needs a discriminator above the short
	 * Manchester-boundary cluster. The scope sweeps show short runs around
	 * 14-18 ticks and long runs around 31-35 ticks; the midpoint is too high
	 * when the long cluster is distorted. Track the short peak instead.
	 */
	return short_peak + 4u;
}

typedef struct {
	uint32_t level_ticks[2];
	uint32_t nominal_ticks;
	uint32_t training_intervals;
} comp_timing_model_t;

#ifndef OPENVLC_RX_LOCAL_TIMING
#define OPENVLC_RX_LOCAL_TIMING 1u
#endif
#ifndef OPENVLC_RX_PAIR_TIMING
#define OPENVLC_RX_PAIR_TIMING 1u
#endif
#define OPENVLC_RX_PAIR_TIMING_INDEPENDENT 0u
#define OPENVLC_RX_PAIR_TIMING_GENERAL 1u
#define OPENVLC_RX_PAIR_TIMING_TWO_CELL 2u
#ifndef OPENVLC_RX_PAIR_TIMING_MODE
#define OPENVLC_RX_PAIR_TIMING_MODE OPENVLC_RX_PAIR_TIMING_TWO_CELL
#endif
#if OPENVLC_RX_PAIR_TIMING_MODE != OPENVLC_RX_PAIR_TIMING_INDEPENDENT && \
	OPENVLC_RX_PAIR_TIMING_MODE != OPENVLC_RX_PAIR_TIMING_GENERAL && \
	OPENVLC_RX_PAIR_TIMING_MODE != OPENVLC_RX_PAIR_TIMING_TWO_CELL
#error "invalid OPENVLC_RX_PAIR_TIMING_MODE"
#endif
#ifndef OPENVLC_RX_DIFFERENTIAL_FALLBACK
#define OPENVLC_RX_DIFFERENTIAL_FALLBACK 0u
#endif
#ifndef OPENVLC_RX_EXACT_SFD
#define OPENVLC_RX_EXACT_SFD 0u
#endif
#ifndef OPENVLC_RX_FUZZY_SFD_MIN_LOCK_CELL
#define OPENVLC_RX_FUZZY_SFD_MIN_LOCK_CELL 0u
#endif
#if OPENVLC_RX_DIFFERENTIAL_FALLBACK && \
	OPENVLC_RX_PAIR_TIMING_MODE != OPENVLC_RX_PAIR_TIMING_TWO_CELL
#error "differential fallback requires the two-cell primary quantizer"
#endif
#ifndef OPENVLC_RX_PAIR_SUM_TOL_DIV
#define OPENVLC_RX_PAIR_SUM_TOL_DIV 4u
#endif
#if OPENVLC_RX_PAIR_SUM_TOL_DIV == 0u
#error "OPENVLC_RX_PAIR_SUM_TOL_DIV must be non-zero"
#endif
#ifndef OPENVLC_RX_THREE_CELL_RETRY_MAX
#define OPENVLC_RX_THREE_CELL_RETRY_MAX 4u
#endif
#ifndef OPENVLC_RX_BOUNDARY_RETRY_MAX
#define OPENVLC_RX_BOUNDARY_RETRY_MAX 8u
#endif
#ifndef OPENVLC_RX_BOUNDARY_RETRY_TOL_DIV
#define OPENVLC_RX_BOUNDARY_RETRY_TOL_DIV 16u
#endif
#if OPENVLC_RX_BOUNDARY_RETRY_TOL_DIV == 0u
#error "OPENVLC_RX_BOUNDARY_RETRY_TOL_DIV must be non-zero"
#endif

typedef struct {
	uint32_t level_ticks[2];
	uint32_t nominal_ticks;
	/*
	 * Peak packet-local distance from the selected run hypothesis, in Q8
	 * timer ticks. It is diagnostic only: feeding one edge residual into the
	 * next interval was disproved by real 50-MS/s captures because a missing or
	 * pattern-shifted crossing propagated a local error through the packet.
	 */
	uint32_t residual_peak_q8;
#if OPENVLC_RX_LOCAL_TIMING
	uint32_t one_cell_sum[2];
	uint8_t one_cell_count[2];
#endif
} comp_packet_timing_tracker_t;

#ifndef OPENVLC_COMP_RUN_BIAS_DIV
#define OPENVLC_COMP_RUN_BIAS_DIV 16u
#endif
#ifndef OPENVLC_COMP_SHOULDER_TOL_DIV
#define OPENVLC_COMP_SHOULDER_TOL_DIV 16u
#endif
#if OPENVLC_COMP_SHOULDER_TOL_DIV == 0u
#error "OPENVLC_COMP_SHOULDER_TOL_DIV must be non-zero"
#endif

#if OPENVLC_RX_LOCAL_TIMING
static OPENVLC_RX_HOT void comp_track_one_cell(
	comp_packet_timing_tracker_t *tracker,
	uint32_t parity, uint32_t run)
{
	uint32_t level;
	uint32_t gate;
	uint32_t count;
	uint32_t mean;
	uint32_t max_step;

	if (!tracker || parity > 1u || !tracker->nominal_ticks)
		return;
	level = tracker->level_ticks[parity];
	gate = tracker->nominal_ticks / 4u;
	if (!gate)
		gate = 1u;
	if (run + gate < level || run > level + gate)
		return;

	tracker->one_cell_sum[parity] += run;
	count = (uint32_t)tracker->one_cell_count[parity] + 1u;
	if (count < 8u) {
		tracker->one_cell_count[parity] = (uint8_t)count;
		return;
	}

	/*
	 * Eight-sample gated block mean: O(1) work per edge instead of sorting
	 * a rolling median for nearly every one-cell interval. Clamp each block
	 * update so one locally biased pattern cannot drag the packet tracker
	 * across a one/two-cell decision boundary.
	 */
	mean = (tracker->one_cell_sum[parity] + 4u) / 8u;
	max_step = tracker->nominal_ticks / 8u;
	if (!max_step)
		max_step = 1u;
	if (mean > level + max_step)
		mean = level + max_step;
	else if (level > mean + max_step)
		mean = level - max_step;
	tracker->level_ticks[parity] = mean;
	tracker->one_cell_sum[parity] = 0u;
	tracker->one_cell_count[parity] = 0u;
}
#endif

static uint32_t comp_hist_window(const uint16_t *hist, uint32_t tick,
				 uint32_t max_tick)
{
	uint32_t first = tick > 2u ? tick - 2u : 0u;
	uint32_t last = tick + 2u < max_tick ? tick + 2u : max_tick;
	uint32_t total = 0;

	for (uint32_t t = first; t <= last; t++)
		total += hist[t];
	return total;
}

static uint32_t comp_hist_median(const uint16_t *hist, uint32_t first,
				 uint32_t last, uint32_t count)
{
	uint32_t cumulative = 0;
	uint32_t middle = (count + 1u) / 2u;

	for (uint32_t tick = first; tick <= last; tick++) {
		cumulative += hist[tick];
		if (cumulative >= middle)
			return tick;
	}
	return 0u;
}

/*
 * The raw 0xAA preamble changes level every cell. Comparator crossing delay
 * can stretch one edge polarity and shorten the other, but two consecutive
 * preamble intervals still add up to two nominal cells. Find the longest such
 * run and estimate each interval parity from that run only. This prevents the
 * much larger Manchester payload histogram from masquerading as the one-cell
 * timing model.
 */
static OPENVLC_RX_HOT bool comp_estimate_preamble_timing(
	const uint32_t *edges, size_t edge_count, comp_timing_model_t *model)
{
#if OPENVLC_COMP_NOMINAL_HALFCELL_TICKS > 0u
	const uint32_t target = 2u * g_nominal_anchor;
	const uint32_t tolerance = g_nominal_anchor / 4u;
	const uint32_t required =
		OPENVLC_SFD_SYNC_PREAMBLE_CELLS > 8u ?
		OPENVLC_SFD_SYNC_PREAMBLE_CELLS : 8u;
	size_t run_start = 0u;
	size_t run_len = 0u;
	size_t best_start = 0u;
	size_t best_len = 0u;
	uint32_t previous = 0u;
	bool have_previous = false;
	uint16_t hist[2][OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS + 1u];
	uint32_t count[2] = { 0u, 0u };
	uint32_t level0;
	uint32_t level1;
	uint32_t sum;
	uint32_t error;

	if (!edges || !model || edge_count < required + 1u)
		return false;

	for (size_t i = 1u; i < edge_count; i++) {
		uint32_t interval = edges[i] - edges[i - 1u];
		bool valid =
			interval >= OPENVLC_COMP_MIN_HALFCELL_TICKS &&
			interval <= OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS;

		if (!valid) {
			have_previous = false;
			run_len = 0u;
			continue;
		}
		if (!have_previous) {
			run_start = i;
			run_len = 1u;
			previous = interval;
			have_previous = true;
		} else {
			sum = previous + interval;
			error = sum > target ? sum - target : target - sum;
			if (error <= tolerance) {
				run_len++;
			} else {
				run_start = i;
				run_len = 1u;
			}
			previous = interval;
		}
		if (run_len > best_len) {
			best_start = run_start;
			best_len = run_len;
		}
	}
	if (best_len < required)
		return false;

	memset(hist, 0, sizeof(hist));
	for (size_t i = best_start; i < best_start + best_len; i++) {
		uint32_t interval = edges[i] - edges[i - 1u];
		uint32_t parity = (uint32_t)((i - 1u) & 1u);

		if (hist[parity][interval] != UINT16_MAX)
			hist[parity][interval]++;
		count[parity]++;
	}
	if (count[0] < 4u || count[1] < 4u)
		return false;
	level0 = comp_hist_median(hist[0], OPENVLC_COMP_MIN_HALFCELL_TICKS,
				  OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS,
				  count[0]);
	level1 = comp_hist_median(hist[1], OPENVLC_COMP_MIN_HALFCELL_TICKS,
				  OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS,
				  count[1]);
	if (!level0 || !level1)
		return false;
	sum = level0 + level1;
	error = sum > target ? sum - target : target - sum;
	if (error > tolerance)
		return false;

	model->level_ticks[0] = level0;
	model->level_ticks[1] = level1;
	model->nominal_ticks = (sum + 1u) / 2u;
	model->training_intervals = (uint32_t)best_len;
	return model->nominal_ticks >= OPENVLC_COMP_MIN_HALFCELL_TICKS;
#else
	(void)edges;
	(void)edge_count;
	(void)model;
	return false;
#endif
}

/*
 * Comparator propagation delay is polarity dependent. At the high-rate
 * profile this produces two different one-cell intervals, for example 16 and
 * 48 ticks around a nominal 32-tick half-cell. Their overlap with valid
 * two-cell intervals makes a single global threshold ambiguous.
 *
 * Captured edges alternate polarity, so build one histogram for each interval
 * parity. Select the two supported peaks whose sum is closest to two nominal
 * half-cells. Later quantisation uses the parity-specific one-cell duration.
 */
static OPENVLC_RX_HOT bool comp_estimate_timing_model(
	const uint32_t *edges, size_t edge_count, comp_timing_model_t *model)
{
#if OPENVLC_COMP_NOMINAL_HALFCELL_TICKS > 0u
	uint16_t hist[2][OPENVLC_COMP_SPLIT_HIST_MAX_TICKS + 1u];
	uint32_t support[2][OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS + 1u];
	uint32_t target_sum = 2u * g_nominal_anchor;
	uint32_t tolerance = g_nominal_anchor / 2u;
	uint32_t best_error = UINT32_MAX;
	uint32_t best_support = 0u;
	uint32_t best_raw_support = 0u;
	uint32_t best0 = 0u;
	uint32_t best1 = 0u;

	if (!edges || !model || edge_count < 2u)
		return false;
	if (comp_estimate_preamble_timing(edges, edge_count, model))
		return true;
	memset(hist, 0, sizeof(hist));
	memset(support, 0, sizeof(support));
	for (size_t i = 1; i < edge_count; i++) {
		uint32_t run = edges[i] - edges[i - 1u];
		uint32_t parity = (uint32_t)((i - 1u) & 1u);

		if (run > OPENVLC_COMP_SPLIT_HIST_MAX_TICKS)
			continue;
		if (hist[parity][run] != UINT16_MAX)
			hist[parity][run]++;
	}
	for (uint32_t parity = 0; parity < 2u; parity++) {
		for (uint32_t t = OPENVLC_COMP_MIN_HALFCELL_TICKS;
		     t <= OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS; t++) {
			support[parity][t] = comp_hist_window(
				hist[parity], t,
				OPENVLC_COMP_SPLIT_HIST_MAX_TICKS);
		}
	}
	for (uint32_t a = OPENVLC_COMP_MIN_HALFCELL_TICKS;
	     a <= OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS; a++) {
		/*
		 * The alternating preamble contributes only about 64 intervals
		 * inside an iperf frame with more than 11,000 edges. Do not scale
		 * this gate from the dominant payload histogram peak or the
		 * polarity-delayed preamble cell disappears from consideration.
		 */
		if (support[0][a] < 4u)
			continue;
		for (uint32_t b = OPENVLC_COMP_MIN_HALFCELL_TICKS;
		     b <= OPENVLC_COMP_SPLIT_SHORT_MAX_TICKS; b++) {
			uint32_t sum;
			uint32_t error;
			uint32_t combined;
			uint32_t raw_combined;

			if (support[1][b] < 4u)
				continue;
			sum = a + b;
			error = sum > target_sum ? sum - target_sum :
						 target_sum - sum;
			if (error > tolerance)
				continue;
			combined = support[0][a] + support[1][b];
			raw_combined = hist[0][a] + hist[1][b];
			if (error < best_error ||
			    (error == best_error &&
			     raw_combined > best_raw_support) ||
			    (error == best_error &&
			     raw_combined == best_raw_support &&
			     combined > best_support)) {
				best_error = error;
				best_support = combined;
				best_raw_support = raw_combined;
				best0 = a;
				best1 = b;
			}
		}
	}
	if (best_error != UINT32_MAX) {
		model->level_ticks[0] = best0;
		model->level_ticks[1] = best1;
		model->nominal_ticks = (best0 + best1 + 1u) / 2u;
		model->training_intervals = 0u;
		return model->nominal_ticks >=
		       OPENVLC_COMP_MIN_HALFCELL_TICKS;
	}
#endif
	{
		uint32_t short_peak = 0;
		uint32_t long_peak = 0;

		if (!comp_estimate_pru_split(edges, edge_count, &short_peak,
					     &long_peak))
			return false;
		(void)long_peak;
		model->level_ticks[0] = short_peak;
		model->level_ticks[1] = short_peak;
		model->nominal_ticks = short_peak;
		model->training_intervals = 0u;
		return short_peak >= OPENVLC_COMP_MIN_HALFCELL_TICKS;
	}
}

#ifndef OPENVLC_RX_TIMING_SEARCH_EDGES
#define OPENVLC_RX_TIMING_SEARCH_EDGES 1536u
#endif

static OPENVLC_RX_HOT uint32_t comp_filter_run_residual(
	uint32_t run, uint32_t base, uint32_t nominal, uint32_t max_cells,
	uint32_t *selected_cells)
{
	uint32_t cells = 1u;
	uint32_t expected;

	if (!base || !nominal || !max_cells)
		return UINT32_MAX / 4u;
	if (run > base) {
		cells += (run - base + nominal / 2u) / nominal;
		if (cells > max_cells)
			cells = max_cells;
	}
	expected = base + (cells - 1u) * nominal;
	if (selected_cells)
		*selected_cells = cells;
	return run > expected ? run - expected : expected - run;
}

OPENVLC_RX_HOT size_t openvlc_edge_filter_timing_aware(
	uint32_t *edges, size_t edge_count, uint32_t candidate_interval,
	uint32_t hard_glitch_interval, uint32_t decision_margin,
	uint32_t *removed_edges)
{
	comp_timing_model_t model = {0};
	size_t training_edges = edge_count;
	size_t out = 0u;
	size_t original_count = edge_count;

	if (removed_edges)
		*removed_edges = 0u;
	if (!edges || edge_count == 0u || candidate_interval == 0u)
		return edge_count;
	if (hard_glitch_interval > candidate_interval)
		hard_glitch_interval = candidate_interval;
	if (training_edges > OPENVLC_RX_TIMING_SEARCH_EDGES)
		training_edges = OPENVLC_RX_TIMING_SEARCH_EDGES;

	/* Preserve the exact legacy behaviour when no trustworthy model exists. */
	if (!comp_estimate_timing_model(edges, training_edges, &model)) {
		size_t filtered = openvlc_edge_cancel_short_pulses(
			edges, edge_count, candidate_interval);

		if (removed_edges)
			*removed_edges = (uint32_t)(edge_count - filtered);
		return filtered;
	}

	for (size_t i = 0u; i < edge_count; i++) {
		uint32_t edge = edges[i];
		bool cancel_pair = false;

		if (out >= 2u && i + 1u < edge_count) {
			uint32_t short_run = edge - edges[out - 1u];

			if (short_run < candidate_interval) {
				/*
				 * Pulses below the physical hard floor cannot be valid cells.
				 * Resolve those immediately; the residual calculation below
				 * is reserved for the ambiguous part of the aperture.
				 */
				if (short_run < hard_glitch_interval) {
					cancel_pair = true;
				} else {
					uint32_t left = edges[out - 1u] - edges[out - 2u];
					uint32_t right = edges[i + 1u] - edge;
					uint32_t merged = edges[i + 1u] - edges[out - 2u];
					uint32_t outer_parity = (uint32_t)((out - 2u) & 1u);
					uint32_t inner_parity = outer_parity ^ 1u;
					uint32_t keep_error;
					uint32_t drop_error;
					uint32_t merged_cells = 0u;

					keep_error = comp_filter_run_residual(
						left, model.level_ticks[outer_parity],
						model.nominal_ticks, OPENVLC_COMP_MAX_RUN_CELLS,
						NULL);
					keep_error += comp_filter_run_residual(
						short_run, model.level_ticks[inner_parity],
						model.nominal_ticks, 1u, NULL);
					keep_error += comp_filter_run_residual(
						right, model.level_ticks[outer_parity],
						model.nominal_ticks, OPENVLC_COMP_MAX_RUN_CELLS,
						NULL);
					drop_error = comp_filter_run_residual(
						merged, model.level_ticks[outer_parity],
						model.nominal_ticks, OPENVLC_COMP_MAX_RUN_CELLS,
						&merged_cells);

					if (merged_cells != 0u &&
					    short_run + decision_margin <
						model.level_ticks[inner_parity] &&
					    drop_error + decision_margin < keep_error)
						cancel_pair = true;
				}
			}
		}

		if (cancel_pair) {
			/* Pop the first edge and discard the second; parity is unchanged. */
			out--;
			continue;
		}
		edges[out++] = edge;
	}

	if (removed_edges)
		*removed_edges = (uint32_t)(original_count - out);
	return out;
}

#if OPENVLC_DECODER_DIAGNOSTICS
static uint32_t comp_expected_run_ticks(uint32_t cells, uint32_t base,
					uint32_t nominal)
{
	if (cells <= 1u)
		return base;
	return base + (cells - 1u) * nominal;
}
#endif

static void comp_packet_timing_init(comp_packet_timing_tracker_t *tracker,
				    const comp_timing_model_t *model)
{
	if (!tracker || !model)
		return;
	tracker->level_ticks[0] = model->level_ticks[0];
	tracker->level_ticks[1] = model->level_ticks[1];
	tracker->nominal_ticks = model->nominal_ticks;
	tracker->residual_peak_q8 = 0u;
#if OPENVLC_RX_LOCAL_TIMING
	memset(tracker->one_cell_sum, 0, sizeof(tracker->one_cell_sum));
	memset(tracker->one_cell_count, 0, sizeof(tracker->one_cell_count));
#endif
}

static OPENVLC_RX_HOT uint32_t comp_quantize_run(
	comp_packet_timing_tracker_t *tracker,
	uint32_t parity, uint32_t run, bool boundary_recovery)
{
	uint32_t cells = 1u;
	uint32_t measured_q8;
	uint32_t decision_q8;
	uint32_t base_q8;
	uint32_t nominal_q8;
	uint32_t base;
	uint32_t nominal;

	if (!tracker || parity > 1u || !tracker->nominal_ticks)
		return 1u;
	base = tracker->level_ticks[parity];
	nominal = tracker->nominal_ticks;
	measured_q8 = run << 8;
	base_q8 = base << 8;
	nominal_q8 = nominal << 8;
	/*
	 * Comparator/filter crossing movement puts a few real intervals exactly
	 * on a one-cell/two-cell decision boundary. Real Pi-HAT captures show that
	 * rounding those upward inserts false cells. Move only the decision point
	 * by 1/16 cell (2 ticks in the 1-Mbit/s profile); keep the measured timing
	 * unchanged for diagnostics. This recovered all four captured packets and
	 * remains proportional for the 500/1250 profiles.
	 */
	decision_q8 = measured_q8 >
		(nominal_q8 / OPENVLC_COMP_RUN_BIAS_DIV) ?
		measured_q8 - nominal_q8 / OPENVLC_COMP_RUN_BIAS_DIV : 0u;

	/*
	 * Expected run(k) is an arithmetic progression:
	 *     base + (k - 1) * nominal.
	 * The old implementation searched every possible k and evaluated up to
	 * OPENVLC_COMP_MAX_RUN_CELLS 64-bit distances for every captured edge.
	 * Nearest-integer quantisation is exactly the same decision. Captures
	 * require polarity 0 to choose the upper count on an exact boundary;
	 * other ties stay on the lower count. At 800-byte,
	 * 1-Mbit/s traffic this removes tens of thousands of inner-loop operations
	 * from every packet and leaves DMA enough time to drain its capture ring.
	 */
	if (!boundary_recovery && decision_q8 > base_q8) {
		/*
		 * Primary realtime path: compare directly with the arithmetic
		 * progression midpoints. Real Manchester runs are almost always one
		 * or two cells, so this executes one or two additions and avoids a
		 * hardware divide plus modulo for every comparator interval.
		 */
		uint32_t boundary_q8 = base_q8 + nominal_q8 / 2u;

		while (cells < OPENVLC_COMP_MAX_RUN_CELLS &&
		       (decision_q8 > boundary_q8 ||
			(parity == 0u && decision_q8 == boundary_q8))) {
			cells++;
			boundary_q8 += nominal_q8;
		}
	} else if (decision_q8 > base_q8) {
		uint32_t delta_q8 = decision_q8 - base_q8;
		uint32_t rounded = delta_q8 + nominal_q8 / 2u;
		bool exact_tie = (rounded % nominal_q8) == 0u;

		/* Primary path chooses the lower cell; guarded fallback tests up. */
		if (rounded && !(boundary_recovery && parity == 0u &&
				 exact_tie))
			rounded--;
		cells += rounded / nominal_q8;
	}
	/*
	 * Captured level-1 shoulders near 80 ticks are stretched two-cell runs,
	 * not three physical cells. This used to be an exact-match rule in the
	 * second hypothesis only. A live 81-tick capture proved that comparator
	 * movement spans more than one integer timer bin, while the cadence
	 * budget permits only the primary pass. Keep the correction local and
	 * proportional to the active profile.
	 */
	if (parity == 1u && cells == 3u) {
		uint32_t shoulder =
			base + 2u * nominal - nominal / 4u;
		uint32_t tolerance =
			nominal / OPENVLC_COMP_SHOULDER_TOL_DIV;

		if (!tolerance)
			tolerance = 1u;
		if (run + tolerance >= shoulder &&
		    run <= shoulder + tolerance)
			cells = 2u;
	}
	if (cells > OPENVLC_COMP_MAX_RUN_CELLS)
		cells = OPENVLC_COMP_MAX_RUN_CELLS;
#if OPENVLC_DECODER_DIAGNOSTICS
	uint32_t expected_q8;

	expected_q8 = comp_expected_run_ticks(cells, base, nominal) << 8;
	{
		uint32_t magnitude = measured_q8 >= expected_q8 ?
			measured_q8 - expected_q8 : expected_q8 - measured_q8;

		if (magnitude > tracker->residual_peak_q8)
			tracker->residual_peak_q8 = magnitude;
	}
#endif
#if OPENVLC_RX_LOCAL_TIMING
	if (cells == 1u)
		comp_track_one_cell(tracker, parity, run);
#endif
	return cells;
}

#if OPENVLC_RX_BOUNDARY_RETRY_MAX
/*
 * Collect only lower/upper cell-count decisions within a narrow proportional
 * margin of the boundary. Real 1-Mbit/s Pi-HAT captures needed the upper
 * decision for a 74-tick exact tie and for a 40-tick run two timer ticks below
 * the biased one/two-cell boundary.
 *
 * The realtime path keeps its deterministic lower-tie behaviour because
 * changing that policy globally regresses earlier captures. If the complete
 * frame later fails, CRC provides enough evidence to try the opposite
 * decision at these few packet-local boundaries. Use the preamble-trained
 * reference here: the adaptive one-cell tracker may already have moved after
 * the missing transition and would otherwise hide the original ambiguity.
 */
static size_t comp_collect_boundary_ties(
	const comp_timing_model_t *timing, const uint32_t *edges,
	size_t edge_count, size_t *candidates, size_t capacity)
{
	comp_packet_timing_tracker_t tracker = {0};
	uint32_t candidate_margins[OPENVLC_RX_BOUNDARY_RETRY_MAX];
	size_t count = 0u;

	if (!timing || !edges || !candidates || !capacity)
		return 0u;
	comp_packet_timing_init(&tracker, timing);
	for (size_t i = timing->training_intervals + 1u;
	     i < edge_count; i++) {
		uint32_t run = edges[i] - edges[i - 1u];
		uint32_t parity = (uint32_t)((i - 1u) & 1u);
		uint32_t base = timing->level_ticks[parity];
		uint32_t nominal = timing->nominal_ticks;
		uint32_t measured_q8 = run << 8;
		uint32_t nominal_q8 = nominal << 8;
		uint32_t bias_q8 = nominal_q8 / OPENVLC_COMP_RUN_BIAS_DIV;
		uint32_t decision_q8 =
			measured_q8 > bias_q8 ? measured_q8 - bias_q8 : 0u;
		uint32_t base_q8 = base << 8;
		bool near_boundary = false;
		uint32_t cells;

		if (nominal && decision_q8 > base_q8) {
			uint32_t remainder =
				(decision_q8 - base_q8) % nominal_q8;
			uint32_t boundary = nominal_q8 / 2u;
			uint32_t margin = remainder > boundary ?
				remainder - boundary : boundary - remainder;
			uint32_t tolerance =
				nominal_q8 / OPENVLC_RX_BOUNDARY_RETRY_TOL_DIV;

			if (!tolerance)
				tolerance = 1u;
			near_boundary = margin <= tolerance;
		}
		cells = comp_quantize_run(&tracker, parity, run, false);
		if (near_boundary && cells < OPENVLC_COMP_MAX_RUN_CELLS) {
			uint32_t remainder =
				(decision_q8 - base_q8) % nominal_q8;
			uint32_t boundary = nominal_q8 / 2u;
			uint32_t margin = remainder > boundary ?
				remainder - boundary : boundary - remainder;
			size_t slot = count;

			/*
			 * Retry the most ambiguous decision first, not the earliest
			 * decision in the packet.  The live profile grants only one
			 * extra pass, so chronological ordering made recovery depend on
			 * where an unrelated near-tie happened to occur.
			 */
			if (slot >= capacity) {
				slot = capacity - 1u;
				if (margin >= candidate_margins[slot])
					continue;
			} else {
				count++;
			}
			while (slot > 0u &&
			       margin < candidate_margins[slot - 1u]) {
				if (slot < capacity) {
					candidate_margins[slot] =
						candidate_margins[slot - 1u];
					candidates[slot] = candidates[slot - 1u];
				}
				slot--;
			}
			candidate_margins[slot] = margin;
			candidates[slot] = i - 1u;
		}
	}
	return count;
}
#endif

#if OPENVLC_RX_THREE_CELL_RETRY_MAX
static size_t comp_collect_three_cell_candidates(
	const comp_timing_model_t *timing, const uint32_t *edges,
	size_t edge_count, size_t *candidates, size_t capacity)
{
	comp_packet_timing_tracker_t tracker = {0};
	size_t count = 0u;

	if (!timing || !edges || !candidates || !capacity)
		return 0u;
	comp_packet_timing_init(&tracker, timing);
	for (size_t i = 1u; i < edge_count && count < capacity; i++) {
		uint32_t run = edges[i] - edges[i - 1u];
		uint32_t parity = (uint32_t)((i - 1u) & 1u);
		uint32_t cells =
			comp_quantize_run(&tracker, parity, run, true);

		if (cells == 3u)
			candidates[count++] = i - 1u;
	}
	return count;
}
#endif

static void comp_packet_timing_export_debug(
	const comp_packet_timing_tracker_t *tracker)
{
	if (!tracker)
		return;
	openvlc_phy_dbg_track_cell0_end = tracker->level_ticks[0];
	openvlc_phy_dbg_track_cell1_end = tracker->level_ticks[1];
	openvlc_phy_dbg_track_nominal_end = tracker->nominal_ticks;
	openvlc_phy_dbg_timing_residual_peak = tracker->residual_peak_q8;
}

/*
 * Manchester cannot contain a constant run longer than two line cells.
 * Therefore a measured 3+ cell gap is not valid data: it means that one or
 * more short opposite-polarity pulses disappeared at the analog slicer. Put
 * those pulses back into the reconstructed symbol stream while preserving the
 * observed edge at the end of the run (the last generated cell remains at the
 * captured level).
 *
 * mode 0: no repair.
 * mode 1: repair from the start of the long run:  L,!L,L[,!L,L...].
 * mode 2: repair from the end of the long run:    [...L,!L,]L,!L,L.
 *
 * A 3-cell run has only one possible interior pulse, so modes 1 and 2 are
 * identical. A 4-cell run is ambiguous; trying both modes covers early/late
 * missing pulses without touching the analog threshold.
 */
static OPENVLC_RX_HOT uint8_t comp_symbol_for_run(
	bool level, uint32_t cells, uint32_t cell_index,
	uint8_t repair_mode)
{
	uint8_t symbol = level ? 1u : 0u;

	if (repair_mode && cells >= 3u && cell_index > 0u &&
	    cell_index + 1u < cells) {
		bool invert;

		if (repair_mode == 1u)
			invert = (cell_index & 1u) != 0u;
		else
			invert = ((cells - 1u - cell_index) & 1u) != 0u;
		if (invert)
			symbol ^= 1u;
	}
	return symbol;
}

/*
 * A wrong one-cell decision changes Manchester pair alignment until a second
 * timing error happens to change it back. In that interval roughly half of
 * the pairs are invalid even though the comparator transitions still contain
 * the data. Detect that distinctive high-density error window and realign by
 * one cell. The already-decoded physical length decides whether each boundary
 * needs an insertion or deletion. Isolated bad pairs are left untouched for
 * Reed-Solomon; at least four bad pairs in 16 are required.
 *
 * This is deliberately a CRC/RS-guarded fallback. The primary decoder remains
 * a straight FIFO timing quantizer, and no repaired frame can be delivered
 * unless the complete codeword validates.
 */
#ifndef OPENVLC_RX_PHASE_WINDOW_PAIRS
#define OPENVLC_RX_PHASE_WINDOW_PAIRS 16u
#endif
#ifndef OPENVLC_RX_PHASE_TRIGGER_BAD_PAIRS
#define OPENVLC_RX_PHASE_TRIGGER_BAD_PAIRS 4u
#endif
#ifndef OPENVLC_RX_PHASE_TRIGGER_BAD_RUN
#define OPENVLC_RX_PHASE_TRIGGER_BAD_RUN 2u
#endif
#ifndef OPENVLC_RX_PHASE_SCAN_STEP_PAIRS
#define OPENVLC_RX_PHASE_SCAN_STEP_PAIRS 4u
#endif
#ifndef OPENVLC_RX_PHASE_SEARCH_CELLS
#define OPENVLC_RX_PHASE_SEARCH_CELLS 16u
#endif
#ifndef OPENVLC_RX_PHASE_SCORE_PAIRS
#define OPENVLC_RX_PHASE_SCORE_PAIRS 64u
#endif
#ifndef OPENVLC_RX_PHASE_SCORE_BACK_PAIRS
#define OPENVLC_RX_PHASE_SCORE_BACK_PAIRS 4u
#endif
#ifndef OPENVLC_RX_PHASE_MIN_IMPROVEMENT
#define OPENVLC_RX_PHASE_MIN_IMPROVEMENT 3u
#endif
#ifndef OPENVLC_RX_PHASE_MAX_EDITS
#define OPENVLC_RX_PHASE_MAX_EDITS 8u
#endif
#ifndef OPENVLC_RX_PHASE_RESCAN_ALL
#define OPENVLC_RX_PHASE_RESCAN_ALL 0u
#endif
#ifndef OPENVLC_RX_PHASE_REPAIR_BEFORE_DECODE
#define OPENVLC_RX_PHASE_REPAIR_BEFORE_DECODE 0u
#endif
#ifndef OPENVLC_RX_PHASE_RECOVERY
#define OPENVLC_RX_PHASE_RECOVERY 1u
#endif
#ifndef OPENVLC_RX_LOCAL_SYMBOL_RECOVERY
#define OPENVLC_RX_LOCAL_SYMBOL_RECOVERY 1u
#endif
#ifndef OPENVLC_RX_LOCAL_SYMBOL_CANDIDATES
#define OPENVLC_RX_LOCAL_SYMBOL_CANDIDATES 4u
#endif
#ifndef OPENVLC_RX_LOCAL_SYMBOL_MARGIN_TICKS
#define OPENVLC_RX_LOCAL_SYMBOL_MARGIN_TICKS 4u
#endif
#ifndef OPENVLC_RX_LIST_CANDIDATES
#define OPENVLC_RX_LIST_CANDIDATES 32u
#endif
#ifndef OPENVLC_RX_LIST_RECOVERY
#define OPENVLC_RX_LIST_RECOVERY 0u
#endif
#ifndef OPENVLC_RX_LIST_BEAM_WIDTH
#define OPENVLC_RX_LIST_BEAM_WIDTH 64u
#endif
#ifndef OPENVLC_RX_LIST_MAX_EDITS
#define OPENVLC_RX_LIST_MAX_EDITS 6u
#endif
#ifndef OPENVLC_SFD_SYNC_LOCKS_MAX
#define OPENVLC_SFD_SYNC_LOCKS_MAX 4u
#endif
#if OPENVLC_SFD_SYNC_LOCKS_MAX == 0u
#error "OPENVLC_SFD_SYNC_LOCKS_MAX must be non-zero"
#endif
#ifndef OPENVLC_RX_LIST_MAX_TRIALS
#define OPENVLC_RX_LIST_MAX_TRIALS OPENVLC_RX_LIST_BEAM_WIDTH
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
#if OPENVLC_MAX_SYMBOLS + OPENVLC_RX_PHASE_MAX_EDITS > UINT16_MAX
#error "list decoder symbol positions exceed uint16_t"
#endif

typedef struct {
	uint16_t position;
	uint8_t cells;
	uint8_t level;
	uint8_t margin;
} comp_local_symbol_candidate_t;

typedef struct {
	uint64_t choices;
	uint16_t output_count;
	uint16_t bad_pairs;
	uint16_t timing_cost;
	uint8_t bad_run;
	uint8_t max_bad_run;
	uint8_t edits;
	uint8_t first_symbol;
	uint8_t have_first;
} comp_list_path_t;

static uint8_t g_comp_phase_symbols[
	OPENVLC_MAX_SYMBOLS + OPENVLC_RX_PHASE_MAX_EDITS]
	OPENVLC_BULK_BUFFER;
static union {
	uint8_t timing_margin[
		OPENVLC_MAX_SYMBOLS + OPENVLC_RX_PHASE_MAX_EDITS];
	comp_list_path_t paths[OPENVLC_RX_LIST_BEAM_WIDTH * 4u];
} g_comp_phase_workspace OPENVLC_BULK_BUFFER;
#define g_comp_phase_timing_margin g_comp_phase_workspace.timing_margin

#if OPENVLC_RX_DIFFERENTIAL_FALLBACK
/*
 * The primary two-cell invariant and the classic differential Manchester
 * decision are complementary on asymmetric comparator crossings.  Preserve
 * the independent run decision while the primary edge pass is already hot in
 * cache.  One bit per reconstructed line cell costs less than 2 KB at the
 * profile-1000 frame size and, unlike another edge hypothesis, adds no second
 * traversal of the TIM capture ring.
 */
#define OPENVLC_RX_ALT_PREFIX_CELLS 512u
#define OPENVLC_RX_ALT_SYMBOL_CAPACITY \
	(OPENVLC_RX_ALT_PREFIX_CELLS + OPENVLC_MAX_SYMBOLS + \
	 OPENVLC_RX_PHASE_MAX_EDITS)
static uint8_t g_comp_alt_symbols[
	(OPENVLC_RX_ALT_SYMBOL_CAPACITY + 7u) / 8u]
	OPENVLC_BULK_BUFFER;
static bool g_comp_differential_won;

OPENVLC_RX_ALWAYS_INLINE bool comp_alt_symbol_append(size_t *count,
						     uint8_t symbol)
{
	size_t index;
	uint8_t mask;

	if (!count || *count >= OPENVLC_RX_ALT_SYMBOL_CAPACITY)
		return false;
	index = *count;
	mask = (uint8_t)(1u << (index & 7u));
	if (symbol)
		g_comp_alt_symbols[index >> 3] |= mask;
	else
		g_comp_alt_symbols[index >> 3] &= (uint8_t)~mask;
	(*count)++;
	return true;
}

OPENVLC_RX_ALWAYS_INLINE uint8_t comp_alt_symbol_get(size_t index)
{
	return (uint8_t)((g_comp_alt_symbols[index >> 3] >> (index & 7u)) &
			 1u);
}
#endif

static uint8_t comp_run_boundary_margin(uint32_t run, uint32_t base,
					uint32_t nominal)
{
	uint32_t measured_q8;
	uint32_t decision_q8;
	uint32_t base_q8;
	uint32_t nominal_q8;
	uint32_t remainder;
	uint32_t margin_q8;

	if (!nominal)
		return UINT8_MAX;
	measured_q8 = run << 8;
	base_q8 = base << 8;
	nominal_q8 = nominal << 8;
	decision_q8 = measured_q8 >
		nominal_q8 / OPENVLC_COMP_RUN_BIAS_DIV ?
		measured_q8 - nominal_q8 / OPENVLC_COMP_RUN_BIAS_DIV : 0u;
	if (decision_q8 <= base_q8)
		return UINT8_MAX;
	remainder = (decision_q8 - base_q8) % nominal_q8;
	margin_q8 = remainder >= nominal_q8 / 2u ?
		remainder - nominal_q8 / 2u :
		nominal_q8 / 2u - remainder;
	margin_q8 = (margin_q8 + 128u) >> 8;
	return margin_q8 >= UINT8_MAX ? UINT8_MAX : (uint8_t)margin_q8;
}

/*
 * Distance from the nearest boundary around an already-selected run length.
 * Unlike comp_run_boundary_margin(), this primary-path form needs no modulo:
 * the quantizer has already supplied the selected cell count, so its lower
 * and upper midpoints are known directly.
 */
#if OPENVLC_RX_LOCAL_SYMBOL_RECOVERY
static OPENVLC_RX_HOT uint8_t comp_selected_boundary_margin(
	uint32_t run, uint32_t base, uint32_t nominal, uint32_t cells)
{
	uint32_t measured_q8;
	uint32_t decision_q8;
	uint32_t nominal_q8;
	uint32_t upper_q8;
	uint32_t margin_q8;

	if (!nominal || !cells)
		return UINT8_MAX;
	measured_q8 = run << 8;
	nominal_q8 = nominal << 8;
	decision_q8 = measured_q8 >
		nominal_q8 / OPENVLC_COMP_RUN_BIAS_DIV ?
		measured_q8 - nominal_q8 / OPENVLC_COMP_RUN_BIAS_DIV : 0u;
	upper_q8 = (base << 8) + (cells - 1u) * nominal_q8 +
		   nominal_q8 / 2u;
	margin_q8 = decision_q8 > upper_q8 ?
		decision_q8 - upper_q8 : upper_q8 - decision_q8;
	if (cells > 1u) {
		uint32_t lower_q8 = upper_q8 - nominal_q8;
		uint32_t lower_margin = decision_q8 > lower_q8 ?
			decision_q8 - lower_q8 : lower_q8 - decision_q8;

		if (lower_margin < margin_q8)
			margin_q8 = lower_margin;
	}
	margin_q8 = (margin_q8 + 128u) >> 8;
	return margin_q8 >= UINT8_MAX ? UINT8_MAX : (uint8_t)margin_q8;
}
#endif

static uint8_t comp_symbol_with_edit(const uint8_t *symbols, size_t count,
				     size_t position, bool insert,
				     uint8_t inserted, size_t index)
{
	if (insert) {
		if (index < position)
			return symbols[index];
		if (index == position)
			return inserted;
		return symbols[index - 1u];
	}
	if (index < position)
		return symbols[index];
	if (index + 1u < count)
		return symbols[index + 1u];
	return symbols[count - 1u];
}

static uint32_t comp_count_bad_pairs_with_edit(
	const uint8_t *symbols, size_t count,
	size_t position, bool insert, uint8_t inserted,
	size_t first_pair, size_t pair_count)
{
	size_t edited_count = insert ? count + 1u : count - 1u;
	size_t available_pairs = edited_count / 2u;
	size_t last_pair = first_pair + pair_count;
	uint32_t bad = 0u;

	if (last_pair > available_pairs)
		last_pair = available_pairs;
	for (size_t pair = first_pair; pair < last_pair; pair++) {
		uint8_t first = comp_symbol_with_edit(
			symbols, count, position, insert, inserted, 2u * pair);
		uint8_t second = comp_symbol_with_edit(
			symbols, count, position, insert, inserted,
			2u * pair + 1u);

		if (first == second)
			bad++;
	}
	return bad;
}

static size_t comp_phase_repair_symbols(uint8_t *symbols,
					uint8_t *timing_margin,
					size_t count,
					size_t capacity,
					size_t target_count,
					uint32_t *out_edits)
{
	size_t pair = 0u;
	uint32_t edits = 0u;

	if (out_edits)
		*out_edits = 0u;
	if (!symbols || count < 2u * OPENVLC_RX_PHASE_WINDOW_PAIRS)
		return count;

	while (2u * (pair + OPENVLC_RX_PHASE_WINDOW_PAIRS) <= count &&
	       edits < OPENVLC_RX_PHASE_MAX_EDITS) {
		uint32_t baseline = 0u;
		uint32_t bad_run = 0u;
		uint32_t max_bad_run = 0u;
		bool insert;
		size_t center;
		size_t search_first;
		size_t search_last;
		size_t best_position = 0u;
		uint8_t best_value = 0u;
		uint32_t best_score = UINT32_MAX;
		uint32_t best_timing_rank = UINT32_MAX;

		for (size_t p = pair;
		     p < pair + OPENVLC_RX_PHASE_WINDOW_PAIRS; p++) {
			if (symbols[2u * p] == symbols[2u * p + 1u]) {
				baseline++;
				bad_run++;
				if (bad_run > max_bad_run)
					max_bad_run = bad_run;
			} else {
				bad_run = 0u;
			}
		}
		if (baseline < OPENVLC_RX_PHASE_TRIGGER_BAD_PAIRS ||
		    max_bad_run < OPENVLC_RX_PHASE_TRIGGER_BAD_RUN) {
			pair += OPENVLC_RX_PHASE_SCAN_STEP_PAIRS;
			continue;
		}

		/*
		 * A phase boundary tells us that one cell is wrong, but not
		 * whether it was inserted or lost. The decoded physical length
		 * gives the packet's exact cell budget. Stay around target-1 so
		 * either a complete burst or the normal one-cell tail flush can
		 * finish it. This selects insert/delete/insert for a clipped
		 * capture and delete/delete for two independently over-quantized
		 * runs without timer-value special cases.
		 */
		insert = target_count ?
			count <= target_count - 1u :
			(edits & 1u) == 0u;
		/* `pair` is the start of the 16-pair detection window. */
		center = 2u * pair + OPENVLC_RX_PHASE_WINDOW_PAIRS;
		if (center >= count)
			center = count - 1u;
		search_first = center > OPENVLC_RX_PHASE_SEARCH_CELLS ?
			center - OPENVLC_RX_PHASE_SEARCH_CELLS : 0u;
		search_last = center + OPENVLC_RX_PHASE_SEARCH_CELLS;
		if (search_last >= count)
			search_last = count - 1u;
		if (timing_margin) {
			uint32_t local_rank = UINT32_MAX;
			bool local_insert = insert;

			/*
			 * Net packet length cannot identify the local operation
			 * when one interval contributes an extra cell and another
			 * loses one. Follow the least-confident timing decision in
			 * this slip window: one cell means insert, two or more
			 * means delete. CRC/RS still validates the finished frame.
			 */
			for (size_t position = search_first;
			     position <= search_last; position++) {
				uint8_t encoded = timing_margin[position];
				uint32_t rank;

				if (encoded == UINT8_MAX)
					continue;
				rank = encoded & 0x7fu;
				if (rank < local_rank) {
					local_rank = rank;
					local_insert =
						(encoded & 0x80u) != 0u;
				}
			}
			if (local_rank != UINT32_MAX)
				insert = local_insert;
		}
		if (insert && count >= capacity)
			break;
		if (!insert && count <= 1u)
			break;

		for (size_t position = search_first;
		     position <= search_last; position++) {
			uint32_t value_count = insert ? 2u : 1u;

			for (uint32_t value = 0u; value < value_count; value++) {
				size_t score_first =
					position / 2u >
						OPENVLC_RX_PHASE_SCORE_BACK_PAIRS ?
					position / 2u -
						OPENVLC_RX_PHASE_SCORE_BACK_PAIRS :
					0u;
				uint32_t score =
					comp_count_bad_pairs_with_edit(
						symbols, count, position,
						insert, (uint8_t)value,
						score_first,
						OPENVLC_RX_PHASE_SCORE_PAIRS);
				uint32_t timing_rank =
					timing_margin &&
					timing_margin[position] != UINT8_MAX ?
					timing_margin[position] & 0x7fu :
					UINT32_MAX;
				size_t best_distance =
					best_position > center ?
					best_position - center :
					center - best_position;
				size_t distance =
					position > center ?
					position - center :
					center - position;

				if (score < best_score ||
				    (score == best_score &&
				     timing_rank < best_timing_rank) ||
				    (score == best_score &&
				     timing_rank == best_timing_rank &&
				     distance < best_distance) ||
				    (score == best_score &&
				     timing_rank == best_timing_rank &&
				     distance == best_distance &&
				     position < best_position)) {
					best_score = score;
					best_timing_rank = timing_rank;
					best_position = position;
					best_value = (uint8_t)value;
				}
			}
		}
		if (best_score + OPENVLC_RX_PHASE_MIN_IMPROVEMENT >
		    baseline) {
			pair += OPENVLC_RX_PHASE_SCAN_STEP_PAIRS;
			continue;
		}

		if (insert) {
			memmove(&symbols[best_position + 1u],
				&symbols[best_position],
				count - best_position);
			symbols[best_position] = best_value;
			if (timing_margin) {
				memmove(&timing_margin[best_position + 1u],
					&timing_margin[best_position],
					count - best_position);
				timing_margin[best_position] = UINT8_MAX;
			}
			count++;
		} else {
			memmove(&symbols[best_position],
				&symbols[best_position + 1u],
				count - best_position - 1u);
			if (timing_margin)
				memmove(&timing_margin[best_position],
					&timing_margin[best_position + 1u],
					count - best_position - 1u);
			count--;
		}
		edits++;
		/*
		 * The edit changes every following pair boundary, but it cannot
		 * change any pair wholly before the edited cell. Rewind far enough
		 * to cover the detector and its complete candidate-search aperture
		 * instead of rescanning the already validated packet prefix. This
		 * preserves the original decision while bounding multiple repairs to
		 * approximately one forward traversal of the frame.
		 */
		if (OPENVLC_RX_PHASE_RESCAN_ALL) {
			pair = 0u;
		} else {
			size_t edited_pair = best_position / 2u;
			size_t rewind = OPENVLC_RX_PHASE_WINDOW_PAIRS +
				OPENVLC_RX_PHASE_SEARCH_CELLS;

			pair = edited_pair > rewind ? edited_pair - rewind : 0u;
		}
	}
	if (out_edits)
		*out_edits = edits;
	return count;
}

#if defined(OPENVLC_TEST_API)
size_t openvlc_test_phase_repair(uint8_t *symbols, size_t count,
				 size_t capacity, size_t target_count,
				 uint32_t *out_edits)
{
	return comp_phase_repair_symbols(symbols, NULL, count, capacity,
					 target_count, out_edits);
}
#endif

static bool comp_decode_phase_symbols(
	const openvlc_runtime_config_t *cfg, const uint8_t *symbols, size_t count,
	uint8_t polarity, bool bad_pair_invert_first,
	openvlc_packet_t *packet, uint8_t *frame, size_t frame_cap,
	openvlc_manchester_stream_t *stream, size_t *frame_len,
	uint16_t *len_raw, uint32_t *manchester_pairs,
	uint32_t *bad_pairs, uint32_t *max_consecutive_bad_pairs,
	uint32_t *maxbits)
{
	uint32_t consecutive_bad_pairs = 0u;
	int bit;

	manchester_stream_init(stream, 0u);
	*frame_len = 0u;
	*len_raw = 0u;
	*manchester_pairs = 0u;
	*bad_pairs = 0u;
	*max_consecutive_bad_pairs = 0u;
	for (bit = (int)OPENVLC_SFD_BITS - 1; bit >= 0; bit--)
		(void)manchester_stream_feed_bit(
			cfg, stream,
			OPENVLC_SFD_BIT(bit),
			packet, frame, frame_cap, frame_len, len_raw);

	for (size_t index = 0u; index + 1u < count; index += 2u) {
		uint8_t first = symbols[index];
		uint8_t second = symbols[index + 1u];
		uint8_t recovered = second;
		uint8_t out_bit;
		openvlc_status_t status;

		(*manchester_pairs)++;
		if (first == second) {
			(*bad_pairs)++;
			consecutive_bad_pairs++;
			if (consecutive_bad_pairs > *max_consecutive_bad_pairs)
				*max_consecutive_bad_pairs =
					consecutive_bad_pairs;
			if (bad_pair_invert_first)
				recovered = (uint8_t)(first ^ 1u);
		} else {
			consecutive_bad_pairs = 0u;
		}
		out_bit = polarity ? (uint8_t)(recovered ^ 1u) : recovered;
		status = manchester_stream_feed_bit(
			cfg, stream, out_bit, packet, frame, frame_cap,
			frame_len, len_raw);
		if (stream->emitted_bits > *maxbits)
			*maxbits = stream->emitted_bits;
		if (status == OPENVLC_OK)
			return true;
	}
	return false;
}

#if OPENVLC_RX_DIFFERENTIAL_FALLBACK
/* SFD helpers are implemented below the generic Manchester parser. */
static uint32_t comp_hamming32(uint32_t value);
static bool comp_history_accepts_preamble(uint64_t history,
					  uint32_t history_count,
					  uint32_t sfd_errors,
					  uint32_t *out_bad_pairs);
#ifndef OPENVLC_SFD_SYNC_MAX_CELL_ERRORS
#define OPENVLC_SFD_SYNC_MAX_CELL_ERRORS 0u
#endif
#ifndef OPENVLC_SFD_SYNC_SEARCH_CELLS
#define OPENVLC_SFD_SYNC_SEARCH_CELLS 512u
#endif
#ifndef OPENVLC_SFD_SYNC_MIN_LOCK_CELL
#define OPENVLC_SFD_SYNC_MIN_LOCK_CELL 0u
#endif
/*
 * Decode the independently quantised line-cell stream only after the primary
 * CRC/RS path has rejected the burst.  SFD correlation is performed on the
 * compact cell stream, not on the edge ring, so the expensive timing model and
 * run reconstruction are never repeated.  A candidate can leave this function
 * only through openvlc_frame_parse() inside comp_decode_phase_symbols().
 */
static bool comp_decode_differential_fallback(
	const openvlc_runtime_config_t *cfg, size_t symbol_count,
	uint32_t sfd_pat, bool bad_pair_invert_first,
	openvlc_packet_t *packet, uint8_t *frame, size_t frame_cap,
	openvlc_manchester_stream_t *stream, size_t *frame_len,
	uint16_t *len_raw, uint32_t *manchester_pairs,
	uint32_t *bad_pairs, uint32_t *max_consecutive_bad_pairs,
	uint32_t *maxbits, uint32_t *phase_edits)
{
	uint32_t sr = 0u;
	uint64_t history = 0u;
	uint32_t history_count = 0u;
	uint32_t locks = 0u;
	size_t search_limit = symbol_count;

	if (search_limit > OPENVLC_SFD_SYNC_SEARCH_CELLS)
		search_limit = OPENVLC_SFD_SYNC_SEARCH_CELLS;
	for (size_t index = 0u; index < search_limit; index++) {
		uint8_t symbol = comp_alt_symbol_get(index);
		uint32_t direct_errors;
		uint32_t inverse_errors;
		uint32_t sfd_errors;
		uint32_t preamble_bad_pairs = 0u;
		uint8_t polarity;
		size_t post_count;
		size_t target_count = 0u;
		size_t repaired_count;
		uint32_t edits = 0u;

		history = (history << 1) | symbol;
		if (history_count < 64u)
			history_count++;
		sr = (uint32_t)(((sr << 1) | symbol) & OPENVLC_SFD_MASK);
		if (index + 1u < OPENVLC_SFD_SYNC_MIN_LOCK_CELL)
			continue;
		direct_errors = comp_hamming32(sr ^ sfd_pat);
		inverse_errors = OPENVLC_SFD_CELLS - direct_errors;
		sfd_errors = direct_errors < inverse_errors ?
			direct_errors : inverse_errors;
		/* Prefer an exact alternative lock; fuzzy locks are already explored
		 * by the primary path and can consume both bounded candidates just
		 * before the independently reconstructed true SFD. */
		if (sfd_errors != 0u ||
		    !comp_history_accepts_preamble(
			    history, history_count, sfd_errors,
			    &preamble_bad_pairs))
			continue;
		if (locks++ >= OPENVLC_SFD_SYNC_LOCKS_MAX)
			break;

		polarity = direct_errors <= inverse_errors ? 0u : 1u;
		post_count = symbol_count - index - 1u;
		if (post_count > sizeof(g_comp_phase_symbols))
			post_count = sizeof(g_comp_phase_symbols);
		for (size_t post = 0u; post < post_count; post++)
			g_comp_phase_symbols[post] =
				comp_alt_symbol_get(index + 1u + post);

		if (post_count >= 32u) {
			uint16_t physical_len = 0u;
			uint32_t data_bits_needed = 0u;

			for (size_t bit = 0u; bit < 16u; bit++) {
				uint8_t recovered =
					g_comp_phase_symbols[2u * bit + 1u];
				uint8_t out_bit = polarity ?
					(uint8_t)(recovered ^ 1u) : recovered;

				physical_len = (uint16_t)((physical_len << 1) |
							  out_bit);
			}
			if (physical_len > OPENVLC_MAX_PAYLOAD_BYTES) {
				(void)beaglebone_data_bits_from_any_preamble(
					physical_len, &data_bits_needed);
			} else {
				data_bits_needed =
					(OPENVLC_HEADER_BYTES - 2u +
					 (uint32_t)physical_len +
					 OPENVLC_CRC_BYTES) * 8u;
			}
			if (data_bits_needed &&
			    data_bits_needed <= OPENVLC_MAX_SYMBOLS / 2u)
				target_count =
					2u * (16u + (size_t)data_bits_needed);
		}
		if (!target_count)
			continue;

		repaired_count = comp_phase_repair_symbols(
			g_comp_phase_symbols, NULL, post_count,
			sizeof(g_comp_phase_symbols), target_count, &edits);
		/*
		 * A capture can end one or two Manchester bits before the exact
		 * length-derived boundary.  The primary decoder has a four-bit tail
		 * flush for this case.  Extend only to the known physical length here;
		 * RS and CRC still reject every incorrect completion.
		 */
		while (repaired_count < target_count &&
		       repaired_count < sizeof(g_comp_phase_symbols) &&
		       target_count - repaired_count <= 8u) {
			g_comp_phase_symbols[repaired_count] = repaired_count ?
				(uint8_t)(g_comp_phase_symbols[repaired_count - 1u] ^ 1u) :
				0u;
			repaired_count++;
		}
		if (comp_decode_phase_symbols(
			    cfg, g_comp_phase_symbols, repaired_count,
			    polarity, bad_pair_invert_first, packet, frame,
			    frame_cap, stream, frame_len, len_raw,
			    manchester_pairs, bad_pairs,
			    max_consecutive_bad_pairs, maxbits)) {
			if (phase_edits)
				*phase_edits = edits;
			openvlc_phy_dbg_sfdsync_lock_cell =
				(uint32_t)(index + 1u);
			openvlc_phy_dbg_len_raw = *len_raw;
			return true;
		}
	}
	return false;
}
#endif

#if OPENVLC_RX_LOCAL_SYMBOL_RECOVERY
/*
 * The primary pass already expanded every comparator interval into line
 * symbols. If one or two ambiguous run decisions leave the reconstructed
 * burst short/long, repair those local decisions in-place and run only the
 * inexpensive Manchester/frame stage once. The edge timing model, quantizer
 * and SFD search are not repeated.
 *
 * Candidates are applied from right to left so their recorded positions stay
 * valid. CRC/RS remains the acceptance gate; this path can never manufacture
 * an unvalidated packet.
 */
static bool comp_local_symbol_recover(
	const openvlc_runtime_config_t *cfg,
	uint8_t *symbols, size_t *count, size_t capacity,
	const comp_local_symbol_candidate_t *candidates,
	size_t candidate_count, size_t target_count,
	uint8_t polarity, bool bad_pair_invert_first,
	openvlc_packet_t *packet, uint8_t *frame, size_t frame_cap,
	openvlc_manchester_stream_t *stream, size_t *frame_len,
	uint16_t *len_raw, uint32_t *manchester_pairs,
	uint32_t *bad_pairs, uint32_t *max_consecutive_bad_pairs,
	uint32_t *maxbits, uint32_t *recovery_edits)
{
	bool insert;
	size_t edit_count;
	size_t repaired_count;
	uint32_t repaired_edits = 0u;
	size_t original_count;

	if (!symbols || !count || !candidates ||
	    !target_count || !recovery_edits)
		return false;
	original_count = *count;
	if (original_count > sizeof(g_comp_phase_timing_margin))
		return false;
	memcpy(g_comp_phase_timing_margin, symbols, original_count);

	/*
	 * The capture deliberately continues four cells beyond the expected
	 * frame boundary so a clipped tail can still complete.  Consequently,
	 * its total symbol count cannot tell whether an interior ambiguous run
	 * was one cell short or one cell long.  Try the physically adjacent
	 * count for each low-margin candidate and let frame CRC/RS select it.
	 * This is bounded by OPENVLC_RX_LOCAL_SYMBOL_CANDIDATES and does not
	 * repeat timing estimation, edge expansion, or SFD search.
	 */
	for (size_t selected = 0u; selected < candidate_count; selected++) {
		const comp_local_symbol_candidate_t *candidate =
			&candidates[selected];
		size_t edit_position = candidate->position + 1u;

		if (candidate->position >= original_count ||
		    edit_position > original_count)
			continue;

		/* A one/two-cell boundary decision may have lost one cell. */
		if (candidate->cells <= 2u && original_count < capacity) {
			uint8_t inserted = candidate->cells >= 2u ?
				(uint8_t)(candidate->level ^ 1u) :
				candidate->level;

			memcpy(symbols, g_comp_phase_timing_margin,
			       original_count);
			memmove(&symbols[edit_position + 1u],
				&symbols[edit_position],
				original_count - edit_position);
			symbols[edit_position] = inserted;
			*count = original_count + 1u;
			if (*count > target_count)
				*count = target_count;
			while (*count < target_count && *count < capacity) {
				symbols[*count] = *count ?
					(uint8_t)(symbols[*count - 1u] ^ 1u) :
					0u;
				(*count)++;
			}
			if (*count == target_count) {
				openvlc_phy_dbg_local_trials++;
				if (comp_decode_phase_symbols(
					    cfg, symbols, *count, polarity,
					    bad_pair_invert_first, packet, frame,
					    frame_cap, stream, frame_len, len_raw,
					    manchester_pairs, bad_pairs,
					    max_consecutive_bad_pairs, maxbits)) {
					*recovery_edits = 1u;
					return true;
				}
			}
		}

		/* A two/three-cell boundary decision may have added one cell. */
		if (candidate->cells >= 2u &&
		    edit_position < original_count) {
			memcpy(symbols, g_comp_phase_timing_margin,
			       original_count);
			memmove(&symbols[edit_position],
				&symbols[edit_position + 1u],
				original_count - edit_position - 1u);
			*count = original_count - 1u;
			if (*count > target_count)
				*count = target_count;
			while (*count < target_count && *count < capacity) {
				symbols[*count] = *count ?
					(uint8_t)(symbols[*count - 1u] ^ 1u) :
					0u;
				(*count)++;
			}
			if (*count == target_count) {
				openvlc_phy_dbg_local_trials++;
				if (comp_decode_phase_symbols(
					    cfg, symbols, *count, polarity,
					    bad_pair_invert_first, packet, frame,
					    frame_cap, stream, frame_len, len_raw,
					    manchester_pairs, bad_pairs,
					    max_consecutive_bad_pairs, maxbits)) {
					*recovery_edits = 1u;
					return true;
				}
			}
		}
	}

	memcpy(symbols, g_comp_phase_timing_margin, original_count);
	*count = original_count;
	insert = *count < target_count;
	if (*count == target_count)
		return false;
	if (insert)
		edit_count = target_count - *count == 1u ? 1u : 2u;
	else
		edit_count = 2u;
	if (candidate_count < edit_count)
		return false;

	for (size_t selected = edit_count; selected != 0u; selected--) {
		const comp_local_symbol_candidate_t *candidate =
			&candidates[selected - 1u];
		size_t edit_position = candidate->position + 1u;

		if (candidate->position >= *count ||
		    edit_position > *count)
			return false;
		if (insert) {
			uint8_t inserted;

			if (*count >= capacity || candidate->cells > 2u)
				return false;
			inserted = candidate->cells >= 2u ?
				(uint8_t)(candidate->level ^ 1u) :
				candidate->level;
			memmove(&symbols[edit_position + 1u],
				&symbols[edit_position],
				*count - edit_position);
			symbols[edit_position] = inserted;
			(*count)++;
		} else {
			if (candidate->cells < 2u ||
			    edit_position >= *count)
				return false;
			memmove(&symbols[edit_position],
				&symbols[edit_position + 1u],
				*count - edit_position - 1u);
			(*count)--;
		}
	}

	/*
	 * A wrong run-length decision can both change the total cell count and
	 * leave Manchester pairing displaced. Repair the bounded local count
	 * candidates first, then realign the saved symbol stream. This remains a
	 * single Manchester/frame retry and never repeats edge quantisation or
	 * SFD acquisition.
	 */
	repaired_count = comp_phase_repair_symbols(
		symbols, NULL, *count, capacity, target_count, &repaired_edits);
	if (repaired_edits != 0u)
		*count = repaired_count;
	*recovery_edits = (uint32_t)edit_count + repaired_edits;

	/*
	 * A burst may end with the final comparator transition clipped. Complete
	 * only the exact physical-length budget; the complemented cell forms a
	 * legal Manchester pair and CRC/RS decides whether that final bit was
	 * recoverable.
	 */
	while (*count < target_count && *count < capacity) {
		symbols[*count] =
			*count ? (uint8_t)(symbols[*count - 1u] ^ 1u) : 0u;
		(*count)++;
	}
	if (*count < target_count)
		return false;
	if (*count > target_count)
		*count = target_count;

	openvlc_phy_dbg_local_trials++;
	return comp_decode_phase_symbols(
		cfg, symbols, *count, polarity, bad_pair_invert_first,
		packet, frame, frame_cap, stream, frame_len, len_raw,
		manchester_pairs, bad_pairs, max_consecutive_bad_pairs,
		maxbits);
}

static OPENVLC_RX_HOT void comp_list_feed_symbol(
	comp_list_path_t *path, uint8_t symbol)
{
	path->output_count++;
	if (!path->have_first) {
		path->first_symbol = symbol;
		path->have_first = true;
		return;
	}
	path->have_first = false;
	if (path->first_symbol == symbol) {
		path->bad_pairs++;
		if (path->bad_run != UINT8_MAX)
			path->bad_run++;
		if (path->bad_run > path->max_bad_run)
			path->max_bad_run = path->bad_run;
	} else {
		path->bad_run = 0u;
	}
}

static uint32_t comp_list_path_score(const comp_list_path_t *path)
{
	uint32_t slip_run =
		path->max_bad_run > 1u ? path->max_bad_run - 1u : 0u;

	return path->bad_pairs * 16u +
	       slip_run * 256u +
	       (uint32_t)path->edits * 64u +
	       path->timing_cost;
}

static void comp_list_feed_original(comp_list_path_t *path,
				    const uint8_t *symbols,
				    size_t first, size_t last)
{
	for (size_t position = first; position < last; position++)
		comp_list_feed_symbol(path, symbols[position]);
}

/*
 * Rank a bounded list of cell-count paths without duplicating packet data.
 * Each path stores only two bits per ambiguous interval. The complete symbol
 * sequence is reconstructed and passed through the normal RS/CRC parser only
 * for the final beam, so memory and runtime are deterministic.
 */
static bool comp_list_symbol_recover(
	const openvlc_runtime_config_t *cfg,
	const uint8_t *symbols, size_t symbol_count, size_t capacity,
	const comp_local_symbol_candidate_t *candidates,
	size_t candidate_count, size_t target_count, uint8_t repair_mode,
	uint8_t polarity, bool bad_pair_invert_first,
	openvlc_packet_t *packet, uint8_t *frame, size_t frame_cap,
	openvlc_manchester_stream_t *stream, size_t *frame_len,
	uint16_t *len_raw, uint32_t *manchester_pairs,
	uint32_t *bad_pairs, uint32_t *max_consecutive_bad_pairs,
	uint32_t *maxbits, uint32_t *recovery_edits)
{
	comp_list_path_t *beam = g_comp_phase_workspace.paths;
	comp_list_path_t *generated =
		beam + OPENVLC_RX_LIST_BEAM_WIDTH;
	uint64_t final_choices[OPENVLC_RX_LIST_BEAM_WIDTH];
	uint8_t final_edits[OPENVLC_RX_LIST_BEAM_WIDTH];
	size_t beam_count = 1u;
	size_t cursor = 0u;

	typedef char comp_list_workspace_must_fit[
		sizeof(g_comp_phase_timing_margin) >=
				sizeof(comp_list_path_t) *
					OPENVLC_RX_LIST_BEAM_WIDTH * 4u ?
			1 : -1];
	(void)sizeof(comp_list_workspace_must_fit);
	if (!symbols || !candidates || !candidate_count || !target_count ||
	    !recovery_edits || target_count > capacity)
		return false;
	openvlc_phy_dbg_list_trials = 0u;
	memset(beam, 0, sizeof(*beam) * OPENVLC_RX_LIST_BEAM_WIDTH);

	for (size_t candidate_index = 0u;
	     candidate_index < candidate_count; candidate_index++) {
		const comp_local_symbol_candidate_t *candidate =
			&candidates[candidate_index];
		size_t run_end = candidate->position + candidate->cells;
		size_t next_position = symbol_count;
		size_t generated_count = 0u;

		if (candidate->position < cursor || run_end > symbol_count)
			continue;
		if (candidate_index + 1u < candidate_count)
			next_position = candidates[candidate_index + 1u].position;
		if (next_position < run_end || next_position > symbol_count)
			next_position = run_end;

		for (size_t path_index = 0u;
		     path_index < beam_count; path_index++) {
			comp_list_path_t prefix = beam[path_index];

			comp_list_feed_original(
				&prefix, symbols, cursor, candidate->position);
			for (uint8_t choice = 0u; choice < 3u; choice++) {
				comp_list_path_t path = prefix;
				uint32_t cells = candidate->cells;

				if (choice == 1u) {
					if (cells <= 1u ||
					    path.edits >=
						    OPENVLC_RX_LIST_MAX_EDITS)
						continue;
					cells--;
				} else if (choice == 2u) {
					if (cells >= OPENVLC_COMP_MAX_RUN_CELLS ||
					    path.edits >=
						    OPENVLC_RX_LIST_MAX_EDITS)
						continue;
					cells++;
				}
				if (choice != 0u) {
					path.edits++;
					path.timing_cost =
						(uint16_t)(path.timing_cost +
							candidate->margin + 1u);
				}
				path.choices |=
					(uint64_t)choice << (2u * candidate_index);
				for (uint32_t cell = 0u; cell < cells; cell++)
					comp_list_feed_symbol(
						&path,
						comp_symbol_for_run(
							candidate->level != 0u,
							cells, cell,
							repair_mode));
				comp_list_feed_original(
					&path, symbols, run_end,
					next_position);
				generated[generated_count++] = path;
			}
		}

		/* Stable insertion sort: at most 3 * beam-width small records. */
		for (size_t index = 1u; index < generated_count; index++) {
			comp_list_path_t value = generated[index];
			uint32_t value_score = comp_list_path_score(&value);
			size_t position = index;

			while (position != 0u &&
			       value_score <
				       comp_list_path_score(
					       &generated[position - 1u])) {
				generated[position] = generated[position - 1u];
				position--;
			}
			generated[position] = value;
		}
		{
			bool selected[OPENVLC_RX_LIST_BEAM_WIDTH * 3u] = {0};

			beam_count = 0u;
			/*
			 * Preserve one best path for every net cell-count
			 * displacement. A global top-N otherwise collapses onto
			 * many equivalent zero-bad-pair paths and discards the
			 * CRC-valid insert/delete combination.
			 */
			for (int32_t wanted_delta =
				     -(int32_t)OPENVLC_RX_LIST_MAX_EDITS;
			     wanted_delta <=
				     (int32_t)OPENVLC_RX_LIST_MAX_EDITS;
			     wanted_delta++) {
				for (size_t index = 0u;
				     index < generated_count; index++) {
					int32_t delta =
						(int32_t)generated[index].
							output_count -
						(int32_t)next_position;

					if (!selected[index] &&
					    delta == wanted_delta) {
						beam[beam_count++] =
							generated[index];
						selected[index] = true;
						break;
					}
				}
			}
			/* Also retain the best path at each edit depth. */
			for (uint32_t wanted_edits = 0u;
			     wanted_edits <= OPENVLC_RX_LIST_MAX_EDITS &&
			     beam_count < OPENVLC_RX_LIST_BEAM_WIDTH;
			     wanted_edits++) {
				for (size_t index = 0u;
				     index < generated_count; index++) {
					if (!selected[index] &&
					    generated[index].edits ==
						    wanted_edits) {
						beam[beam_count++] =
							generated[index];
						selected[index] = true;
						break;
					}
				}
			}
			for (size_t index = 0u;
			     index < generated_count &&
			     beam_count < OPENVLC_RX_LIST_BEAM_WIDTH;
			     index++) {
				if (selected[index])
					continue;
				beam[beam_count++] = generated[index];
			}
		}
		cursor = next_position;
	}

	/* Diversity controls pruning; final CRC trials run cheapest path first. */
	for (size_t index = 1u; index < beam_count; index++) {
		comp_list_path_t value = beam[index];
		uint32_t value_score = comp_list_path_score(&value);
		size_t position = index;

		while (position != 0u &&
		       value_score <
			       comp_list_path_score(&beam[position - 1u])) {
			beam[position] = beam[position - 1u];
			position--;
		}
		beam[position] = value;
	}
	for (size_t path_index = 0u; path_index < beam_count; path_index++) {
		final_choices[path_index] = beam[path_index].choices;
		final_edits[path_index] = beam[path_index].edits;
	}
	for (size_t path_index = 0u;
	     path_index < beam_count &&
	     path_index < OPENVLC_RX_LIST_MAX_TRIALS;
	     path_index++) {
		uint64_t choices = final_choices[path_index];
		size_t input = 0u;
		size_t output = 0u;

		for (size_t candidate_index = 0u;
		     candidate_index < candidate_count; candidate_index++) {
			const comp_local_symbol_candidate_t *candidate =
				&candidates[candidate_index];
			uint8_t choice = (uint8_t)(
				(choices >> (2u * candidate_index)) & 3u);
			uint32_t cells = candidate->cells;

			if (candidate->position < input ||
			    candidate->position + candidate->cells >
				    symbol_count)
				continue;
			while (input < candidate->position &&
			       output < target_count)
				g_comp_phase_timing_margin[output++] =
					symbols[input++];
			if (choice == 1u && cells > 1u)
				cells--;
			else if (choice == 2u &&
				 cells < OPENVLC_COMP_MAX_RUN_CELLS)
				cells++;
			for (uint32_t cell = 0u;
			     cell < cells && output < target_count; cell++)
				g_comp_phase_timing_margin[output++] =
					comp_symbol_for_run(
						candidate->level != 0u, cells,
						cell, repair_mode);
			input = candidate->position + candidate->cells;
		}
		while (input < symbol_count && output < target_count)
			g_comp_phase_timing_margin[output++] = symbols[input++];
		while (output < target_count) {
			g_comp_phase_timing_margin[output] = output ?
				(uint8_t)(
					g_comp_phase_timing_margin[output - 1u] ^
					1u) :
				0u;
			output++;
		}
		openvlc_phy_dbg_list_trials++;
		if (comp_decode_phase_symbols(
			    cfg, g_comp_phase_timing_margin, target_count,
			    polarity, bad_pair_invert_first, packet, frame,
			    frame_cap, stream, frame_len, len_raw,
			    manchester_pairs, bad_pairs,
			    max_consecutive_bad_pairs, maxbits)) {
			*recovery_edits = final_edits[path_index];
			return true;
		}
	}
	return false;
}
#endif

/*
 * Robust SFD-correlated edge decoder (validated offline against an
 * oscilloscope capture of the BeagleBone TX: decodes 60/60 packets clean).
 *
 * Unlike the run-length "active/skip" heuristic, this method:
 *   1. estimates the base half-cell from the interval histogram (short_peak);
 *   2. expands each edge interval into round(run / half-cell) line symbols
 *      (1 for a single cell, 2 for a double, ...) at the alternating level;
 *   3. slides a 16-symbol shift register and locks when it matches the
 *      Manchester symbol pattern of the SFD byte (or its inverse, which also
 *      resolves optical polarity) - this is the "preamble+SFD sync" the
 *      BeagleBone PRU receiver performs;
 *   4. from the lock point, Manchester-pairs the symbols (a,b) with a!=b and
 *      feeds the recovered bits into the existing framing state machine.
 *
 * Because it re-synchronises on every SFD, a stray glitch only costs one
 * packet instead of desyncing the whole burst.
 */
static uint32_t comp_manch_pattern(uint32_t word, unsigned nbits)
{
	uint32_t pat = 0;
	int b;

	/* BeagleBone OpenVLC convention: 1 = LOW-HIGH (01), 0 = HIGH-LOW (10). */
	for (b = (int)nbits - 1; b >= 0; b--) {
		pat = (uint32_t)(pat << 2);
		pat |= ((word >> b) & 1u) ? 0x1u : 0x2u;
	}
	return pat;
}

static uint32_t comp_hamming32(uint32_t value)
{
	uint32_t n = 0u;

	while (value) {
		value &= value - 1u;
		n++;
	}
	return n;
}

#ifndef OPENVLC_SFD_SYNC_PREAMBLE_MAX_BAD_PAIRS
#define OPENVLC_SFD_SYNC_PREAMBLE_MAX_BAD_PAIRS 0u
#endif

#ifndef OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_BAD_PAIRS
#define OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_BAD_PAIRS \
	OPENVLC_SFD_SYNC_PREAMBLE_MAX_BAD_PAIRS
#endif

#ifndef OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_SFD_ERRORS
#define OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_SFD_ERRORS 0u
#endif

/*
 * The lower 16 history bits contain the candidate Manchester SFD. Check the
 * raw line cells immediately before it for the alternating 0xAA training
 * waveform. Polarity is deliberately ignored: only adjacent-cell transitions
 * matter.
 */
static bool comp_history_preamble_score(uint64_t history,
					uint32_t history_count,
					uint32_t *out_bad_pairs)
{
	uint32_t i;
	uint32_t bad_pairs = 0u;

	if (out_bad_pairs)
		*out_bad_pairs = 0u;
	if (OPENVLC_SFD_SYNC_PREAMBLE_CELLS == 0u)
		return true;
	/*
	 * Skip the SFD's own cells before reading the preamble behind it.
	 * This was hardcoded 16 and silently examined the first half of a
	 * 32-cell SFD instead of the preamble, which is not alternating, so
	 * every frame was rejected at the gate (fp = one per frame).
	 */
	if (history_count <
	    OPENVLC_SFD_CELLS + (uint32_t)OPENVLC_SFD_SYNC_PREAMBLE_CELLS)
		return false;

	for (i = 0; i + 1u < OPENVLC_SFD_SYNC_PREAMBLE_CELLS; i++) {
		uint32_t shift = OPENVLC_SFD_CELLS + i;
		uint64_t adjacent = (history >> shift) & 0x3u;

		if (adjacent == 0u || adjacent == 0x3u)
			bad_pairs++;
	}
	if (out_bad_pairs)
		*out_bad_pairs = bad_pairs;
	return true;
}

static bool comp_history_accepts_preamble(uint64_t history,
					  uint32_t history_count,
					  uint32_t sfd_errors,
					  uint32_t *out_bad_pairs)
{
	uint32_t bad_pairs = 0u;

	if (!comp_history_preamble_score(history, history_count, &bad_pairs))
		return false;
	if (out_bad_pairs)
		*out_bad_pairs = bad_pairs;
	if (bad_pairs <= OPENVLC_SFD_SYNC_PREAMBLE_MAX_BAD_PAIRS)
		return true;
	/*
	 * If the SFD itself is a strong match, tolerate a little more damage in
	 * the immediately preceding raw alternating preamble. This recovers
	 * AGC/comparator-clipped true SFDs without allowing weak SFD candidates
	 * from payload-like traffic to pass the gate.
	 */
	return sfd_errors <= OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_SFD_ERRORS &&
	       bad_pairs <= OPENVLC_SFD_SYNC_PREAMBLE_RELAXED_BAD_PAIRS;
}

#if defined(OPENVLC_TEST_API)
static bool comp_history_has_preamble(uint64_t history, uint32_t history_count)
{
	uint32_t bad_pairs = 0u;

	if (!comp_history_preamble_score(history, history_count, &bad_pairs))
		return false;
	return bad_pairs <= OPENVLC_SFD_SYNC_PREAMBLE_MAX_BAD_PAIRS;
}

bool openvlc_test_history_has_preamble(uint64_t history,
				      uint32_t history_count)
{
	return comp_history_has_preamble(history, history_count);
}
#endif

/*
 * A bounded SFD distance is accepted only after the strict raw-preamble gate.
 * After lock, isolated equal Manchester pairs are quality defects rather than
 * proof of phase loss. A consecutive run is the useful phase-slip detector.
 */
#ifndef OPENVLC_SFD_SYNC_MAX_CELL_ERRORS
#define OPENVLC_SFD_SYNC_MAX_CELL_ERRORS 0u
#endif

#ifndef OPENVLC_SFD_SYNC_MAX_CONSECUTIVE_BAD_PAIRS
#define OPENVLC_SFD_SYNC_MAX_CONSECUTIVE_BAD_PAIRS 8u
#endif

#ifndef OPENVLC_SFD_SYNC_BAD_PAIR_INVERT_FIRST
#define OPENVLC_SFD_SYNC_BAD_PAIR_INVERT_FIRST 0u
#endif

/*
 * A genuine frame syncs within preamble+SFD = ~80 cells of the burst start.
 * If no sync is found in this many cells, the burst has no preamble (e.g. its
 * head was overwritten by a capture-ring wrap) and scanning the remaining
 * ~22k cells x2 passes only burns ~7 ms per burst. That cost is what turns a
 * transient overload into a self-sustaining congestion collapse (failed
 * decode is slower than the frame period, so the backlog never drains).
 * Capping the search makes a corrupted burst ~100x cheaper than a good one.
 */
#ifndef OPENVLC_SFD_SYNC_SEARCH_CELLS
#define OPENVLC_SFD_SYNC_SEARCH_CELLS 512u
#endif
#ifndef OPENVLC_SFD_SYNC_MIN_LOCK_CELL
#define OPENVLC_SFD_SYNC_MIN_LOCK_CELL 0u
#endif
#ifndef OPENVLC_RX_TIMING_SEARCH_EDGES
#define OPENVLC_RX_TIMING_SEARCH_EDGES 1536u
#endif

static OPENVLC_RX_HOT openvlc_status_t comp_sfd_sync_pass(
	const openvlc_runtime_config_t *cfg,
	const uint32_t *edges, size_t edge_count,
	const comp_timing_model_t *timing,
	uint32_t sfd_pat,
	uint8_t repair_mode,
	bool bad_pair_invert_first,
	bool boundary_recovery,
	uint8_t phase_recovery,
	bool local_symbol_recovery,
	size_t shrink_interval,
	size_t expand_interval,
	openvlc_packet_t *packet, openvlc_quality_t *quality,
	uint8_t *frame, size_t frame_cap,
	uint32_t *out_syncs, uint32_t *out_maxbits, uint16_t *out_lenraw,
	uint32_t *out_preamble_rejects, int32_t *out_parse_status)
{
	uint32_t sr = 0;
	uint64_t history = 0;
	bool synced = false;
	uint8_t pol = 0;
	uint8_t phase = 0;
	uint8_t a_sym = 0;
	bool level = false;
	uint32_t nsyms = 0;
	uint16_t len_raw = 0;
	size_t frame_len = 0;
	uint32_t syncs = 0;
	uint32_t maxbits = 0;
	uint32_t bad_pairs = 0;
	uint32_t consecutive_bad_pairs = 0;
	uint32_t max_consecutive_bad_pairs = 0;
	uint32_t manchester_pairs = 0;
	uint64_t timing_error_sq = 0;
	uint32_t timing_intervals = 0;
	uint32_t timing_outliers = 0;
	uint32_t timing_sample_phase = 0;
	uint32_t history_count = 0;
	uint32_t preamble_rejects = 0;
	uint32_t relocks = 0;
	size_t phase_symbol_count = 0u;
	uint32_t phase_edits = 0u;
#if OPENVLC_RX_DIFFERENTIAL_FALLBACK
	size_t alt_symbol_count = 0u;
	bool alt_symbol_overflow = false;
#endif
#if OPENVLC_RX_PAIR_TIMING_MODE == OPENVLC_RX_PAIR_TIMING_GENERAL
	bool pair_next_valid = false;
	uint32_t pair_next_cells = 1u;
#elif OPENVLC_RX_PAIR_TIMING_MODE == OPENVLC_RX_PAIR_TIMING_TWO_CELL
	bool pair_force_next = false;
#endif
	openvlc_manchester_stream_t stream;
	comp_packet_timing_tracker_t tracker = {0};
#if OPENVLC_RX_LOCAL_SYMBOL_RECOVERY
	comp_local_symbol_candidate_t local_candidates[
		OPENVLC_RX_LOCAL_SYMBOL_CANDIDATES];
	comp_local_symbol_candidate_t list_candidates[
		OPENVLC_RX_LIST_CANDIDATES];
	size_t local_candidate_count = 0u;
	size_t list_candidate_count = 0u;
	size_t local_symbol_count = 0u;
	bool local_capture_active = false;
	bool local_frame_failed = false;
	size_t local_target_count = 0u;
#else
	(void)local_symbol_recovery;
#endif

	comp_packet_timing_init(&tracker, timing);
	manchester_stream_init(&stream, 0u);
	for (size_t i = 1; i < edge_count; i++) {
		uint32_t run = edges[i] - edges[i - 1u];
		uint32_t parity = (uint32_t)((i - 1u) & 1u);
		uint32_t base = tracker.level_ticks[parity];
		uint32_t nominal = tracker.nominal_ticks;
		uint8_t run_timing_margin = UINT8_MAX;
#if OPENVLC_RX_PAIR_TIMING_MODE == OPENVLC_RX_PAIR_TIMING_GENERAL
		uint32_t n;

		if (pair_next_valid) {
			n = pair_next_cells;
			pair_next_valid = false;
		} else {
			n = comp_quantize_run(
				&tracker, parity, run,
				boundary_recovery && synced);
		}
#elif OPENVLC_RX_PAIR_TIMING_MODE == OPENVLC_RX_PAIR_TIMING_TWO_CELL
		bool pair_force_one = pair_force_next;

		pair_force_next = false;
#endif
#if OPENVLC_RX_PAIR_TIMING
#if OPENVLC_RX_PAIR_TIMING_MODE == OPENVLC_RX_PAIR_TIMING_GENERAL
		/*
		 * General paired quantizer used by the earlier realtime baseline.
		 * Quantize the pair sum, then select the positive decomposition whose
		 * two level-specific residuals are smallest.
		 */
		if (parity == 0u && i + 1u < edge_count && nominal != 0u) {
			uint32_t next_run = edges[i + 1u] - edges[i];
			uint32_t pair_sum = run + next_run;
			uint32_t pair_cells =
				(pair_sum + nominal / 2u) / nominal;
			uint32_t target = pair_cells * nominal;
			uint32_t pair_error = pair_sum > target ?
				pair_sum - target : target - pair_sum;
			uint32_t next_n = comp_quantize_run(
				&tracker, 1u, next_run,
				boundary_recovery && synced);

			pair_next_valid = true;
			pair_next_cells = next_n;
			if (pair_cells >= 2u &&
			    pair_cells <= 2u * OPENVLC_COMP_MAX_RUN_CELLS &&
			    pair_error <= nominal / OPENVLC_RX_PAIR_SUM_TOL_DIV) {
				uint32_t best_first = n;
				uint32_t best_cost = UINT32_MAX;

				for (uint32_t first = 1u;
				     first < pair_cells; first++) {
					uint32_t second = pair_cells - first;
					uint32_t expected0 =
						tracker.level_ticks[0] +
						(first - 1u) * nominal;
					uint32_t expected1 =
						tracker.level_ticks[1] +
						(second - 1u) * nominal;
					uint32_t cost0 = run > expected0 ?
						run - expected0 : expected0 - run;
					uint32_t cost1 = next_run > expected1 ?
						next_run - expected1 : expected1 - next_run;
					uint32_t cost = cost0 + cost1;

					if (cost < best_cost) {
						best_cost = cost;
						best_first = first;
					}
				}
				n = best_first;
				pair_next_cells = pair_cells - best_first;
		}
	}
#elif OPENVLC_RX_PAIR_TIMING_MODE == OPENVLC_RX_PAIR_TIMING_TWO_CELL
		/*
		 * Opposite comparator crossings move in opposite directions when
		 * threshold/AGC delay changes. A real capture contained 20+44 and
		 * 21+43 tick pairs: every pair still spans exactly two 32-tick
		 * cells, but independent quantisation turns the 43/44-tick member
		 * into two cells and inserts a false line symbol.
		 *
		 * Each captured interval contains at least one cell. Therefore, if
		 * two adjacent intervals together span two cells, 1+1 is the only
		 * physically possible decomposition. Apply that invariant to the
		 * first eligible adjacent pair before the independent fallback.
		 * Pair alignment is deliberately not tied to an even/odd index:
		 * after a moved or missing crossing, the useful two-cell evidence
		 * can straddle that arbitrary boundary. Pairs are non-overlapping
		 * through pair_force_next. This is one bounded look-ahead, not an
		 * additional decoder hypothesis.
		 */
		if (!pair_force_one && i + 1u < edge_count &&
		    nominal != 0u) {
			uint32_t next_run = edges[i + 1u] - edges[i];
			uint32_t pair_sum = run + next_run;
			uint32_t target = nominal * 2u;
			uint32_t tolerance =
				nominal / OPENVLC_RX_PAIR_SUM_TOL_DIV;

			if (!tolerance)
				tolerance = 1u;
			if (pair_sum + tolerance >= target &&
			    pair_sum <= target + tolerance) {
				pair_force_one = true;
				pair_force_next = true;
			}
		}
#endif
#endif
#if OPENVLC_RX_PAIR_TIMING_MODE != OPENVLC_RX_PAIR_TIMING_GENERAL
		uint32_t n = comp_quantize_run(
			&tracker, parity, run,
			boundary_recovery && synced);

#if OPENVLC_RX_PAIR_TIMING_MODE == OPENVLC_RX_PAIR_TIMING_TWO_CELL
#if OPENVLC_RX_DIFFERENTIAL_FALLBACK
		uint32_t independent_n = n;
#endif
		if (pair_force_one)
			n = 1u;
#endif
#endif
		if (i - 1u == shrink_interval && n == 3u)
			n = 2u;
		if (i - 1u == expand_interval &&
		    n < OPENVLC_COMP_MAX_RUN_CELLS)
			n++;
#if defined(OPENVLC_TEST_API)
		for (size_t override = 0u; override < 3u; override++) {
			if (openvlc_test_override_interval[override] != i - 1u)
				continue;
			if (openvlc_test_override_delta[override] < 0 && n > 1u)
				n--;
			else if (openvlc_test_override_delta[override] > 0 &&
				 n < OPENVLC_COMP_MAX_RUN_CELLS)
				n++;
		}
#endif

#if OPENVLC_RX_DIFFERENTIAL_FALLBACK
		if (!alt_symbol_overflow) {
			for (uint32_t alt = 0u; alt < independent_n; alt++) {
				uint8_t alt_symbol = comp_symbol_for_run(
					level, independent_n, alt, repair_mode);

				if (!comp_alt_symbol_append(
					    &alt_symbol_count, alt_symbol)) {
					alt_symbol_overflow = true;
					break;
				}
			}
		}
#endif

		if (OPENVLC_RX_LIST_RECOVERY && phase_recovery && synced) {
			uint8_t local_timing_margin =
				comp_run_boundary_margin(run, base, nominal);
			uint8_t packet_timing_margin =
				comp_run_boundary_margin(
					run, timing->level_ticks[parity],
					timing->nominal_ticks);

			run_timing_margin =
				local_timing_margin < packet_timing_margin ?
					local_timing_margin : packet_timing_margin;
		}
#if OPENVLC_RX_LOCAL_SYMBOL_RECOVERY
		if (phase_recovery && synced) {
			uint8_t margin = comp_selected_boundary_margin(
				run, timing->level_ticks[parity],
				timing->nominal_ticks, n);

			if (margin <= OPENVLC_RX_LOCAL_SYMBOL_MARGIN_TICKS ||
			    n >= 3u) {
				size_t slot = list_candidate_count;
				bool store = true;
				uint32_t priority =
					(uint32_t)margin + (n >= 3u ? 0u : 32u);

				if (slot >= OPENVLC_RX_LIST_CANDIDATES) {
					uint32_t worst_priority = 0u;
					size_t worst = 0u;

					for (size_t candidate = 0u;
					     candidate <
						     OPENVLC_RX_LIST_CANDIDATES;
					     candidate++) {
						uint32_t candidate_priority =
							(uint32_t)
								list_candidates[
									candidate].
									margin +
							(list_candidates[candidate].
									 cells >=
								 3u ?
								 0u : 32u);

						if (candidate_priority >
						    worst_priority) {
							worst_priority =
								candidate_priority;
							worst = candidate;
						}
					}
					if (priority < worst_priority)
						slot = worst;
					else
						store = false;
				} else {
					list_candidate_count++;
				}
				if (store) {
					list_candidates[slot].position =
						(uint16_t)phase_symbol_count;
					list_candidates[slot].cells =
						(uint8_t)n;
					list_candidates[slot].level =
						level ? 1u : 0u;
					list_candidates[slot].margin =
						margin;
				}
			}
		}
		if (local_symbol_recovery && local_capture_active &&
		    !local_frame_failed &&
		    local_candidate_count <
			    OPENVLC_RX_LOCAL_SYMBOL_CANDIDATES) {
			uint8_t margin = comp_selected_boundary_margin(
				run, timing->level_ticks[parity],
				timing->nominal_ticks, n);

			if (margin <=
				    OPENVLC_RX_LOCAL_SYMBOL_MARGIN_TICKS) {
				local_candidates[local_candidate_count].position =
					(uint16_t)local_symbol_count;
				local_candidates[local_candidate_count].cells =
					(uint8_t)n;
				local_candidates[local_candidate_count].level =
					level ? 1u : 0u;
				local_candidates[local_candidate_count].margin =
					margin;
				local_candidate_count++;
			}
		}
#endif

		if (synced &&
		    ++timing_sample_phase >= OPENVLC_RX_QUALITY_DECIMATION) {
			timing_sample_phase = 0u;
			comp_accumulate_dcd_timing(
				run, n, base, nominal,
				&timing_error_sq, &timing_intervals,
				&timing_outliers);
		}
		for (uint32_t k = 0; k < n; k++) {
			uint8_t sym = comp_symbol_for_run(
				level, n, k, repair_mode);

			nsyms++;
			history = (history << 1) | sym;
			if (history_count < 64u)
				history_count++;
#if OPENVLC_RX_LOCAL_SYMBOL_RECOVERY
			if (local_symbol_recovery && local_capture_active &&
			    local_symbol_count < sizeof(g_comp_phase_symbols)) {
				g_comp_phase_symbols[local_symbol_count] = sym;
				local_symbol_count++;
			}
			if (local_symbol_recovery && local_frame_failed) {
				if (local_symbol_count >= local_target_count + 4u)
					goto local_recover;
				continue;
			}
#endif
			if (!synced) {
				uint32_t direct_errors;
				uint32_t inverse_errors;
				uint32_t sfd_errors;

				if (syncs == 0u &&
				    nsyms > OPENVLC_SFD_SYNC_SEARCH_CELLS)
					goto no_sync;
				sr = (uint32_t)(((sr << 1) | sym) & OPENVLC_SFD_MASK);
				direct_errors =
					comp_hamming32(sr ^ sfd_pat);
				/* sfd_inv is the exact 16-cell complement. */
				inverse_errors = OPENVLC_SFD_CELLS - direct_errors;
				sfd_errors = direct_errors < inverse_errors ?
					direct_errors : inverse_errors;
				if (nsyms >= 16u &&
				    nsyms >= OPENVLC_SFD_SYNC_MIN_LOCK_CELL &&
				    sfd_errors <= (OPENVLC_RX_EXACT_SFD ? 0u :
					    OPENVLC_SFD_SYNC_MAX_CELL_ERRORS) &&
				    (sfd_errors == 0u ||
				     nsyms >= OPENVLC_RX_FUZZY_SFD_MIN_LOCK_CELL)) {
					int b;
					uint32_t preamble_bad_pairs = 0u;

					if (!comp_history_accepts_preamble(
						    history, history_count,
						    sfd_errors,
						    &preamble_bad_pairs)) {
						if (preamble_bad_pairs >
						    openvlc_phy_dbg_sfdsync_pre_badmax)
							openvlc_phy_dbg_sfdsync_pre_badmax =
								preamble_bad_pairs;
						if (sfd_errors <
						    openvlc_phy_dbg_sfdsync_pre_sfdmin)
							openvlc_phy_dbg_sfdsync_pre_sfdmin =
								sfd_errors;
						preamble_rejects++;
						continue;
					}
					if (preamble_bad_pairs >
					    openvlc_phy_dbg_sfdsync_pre_badmax)
						openvlc_phy_dbg_sfdsync_pre_badmax =
							preamble_bad_pairs;
					/*
					 * A valid optical burst needs one SFD lock.
					 * Repeated payload/noise locks after a failed
					 * frame can each invoke a complete frame/RS
					 * parse and starve the DMA ring. Bound that
					 * work independently from edge count.
					 */
					if (syncs >= OPENVLC_SFD_SYNC_LOCKS_MAX)
						goto no_sync;
					pol = direct_errors <= inverse_errors ?
						0u : 1u;
					synced = true;
					phase = 0u;
					bad_pairs = 0;
					consecutive_bad_pairs = 0;
					max_consecutive_bad_pairs = 0;
					manchester_pairs = 0;
					timing_error_sq = 0;
					timing_intervals = 0;
					timing_outliers = 0;
					timing_sample_phase = 0;
					syncs++;
					openvlc_phy_dbg_sfdsync_sfd_errors =
						sfd_errors;
					openvlc_phy_dbg_sfdsync_lock_cell =
						nsyms;
					manchester_stream_init(&stream, 0u);
					phase_symbol_count = 0u;
#if OPENVLC_RX_LOCAL_SYMBOL_RECOVERY
					if (local_symbol_recovery &&
					    !local_capture_active) {
						local_capture_active = true;
						local_symbol_count = 0u;
						local_candidate_count = 0u;
					}
					if (phase_recovery)
						list_candidate_count = 0u;
#endif
					if (!phase_recovery) {
						/* Seed framing with the locked SFD byte. */
						for (b = (int)OPENVLC_SFD_BITS - 1; b >= 0; b--)
							(void)manchester_stream_feed_bit(
								cfg, &stream,
								OPENVLC_SFD_BIT(b),
								packet, frame, frame_cap,
								&frame_len, &len_raw);
					}
				}
				continue;
			}
			if (phase_recovery) {
				if (phase_symbol_count >=
				    sizeof(g_comp_phase_symbols))
					goto no_sync;
				g_comp_phase_symbols[phase_symbol_count] = sym;
				g_comp_phase_timing_margin[phase_symbol_count] =
					run_timing_margin == UINT8_MAX ?
					UINT8_MAX :
					(uint8_t)(run_timing_margin |
						(n == 1u ? 0x80u : 0u));
				phase_symbol_count++;
				/*
				 * A cached phase pass cannot discover a false SFD until
				 * the end of the burst unless its 16-bit physical length
				 * is checked here. Validate that first field as soon as its
				 * 32 Manchester cells are available, so a clipped warm-up
				 * can be rejected and SFD search can continue in the same
				 * burst. This is a fixed 16-bit check, not another pass.
				 */
				if (phase_symbol_count == 32u) {
					uint16_t physical_len = 0u;
					uint32_t data_bits_needed = 0u;

					for (size_t bit = 0u; bit < 16u; bit++) {
						uint8_t recovered =
							g_comp_phase_symbols[2u * bit + 1u];
						uint8_t out_bit = pol ?
							(uint8_t)(recovered ^ 1u) : recovered;

						physical_len = (uint16_t)(
							(physical_len << 1) | out_bit);
					}
					if (physical_len > OPENVLC_MAX_PAYLOAD_BYTES) {
						(void)beaglebone_data_bits_from_any_preamble(
							physical_len, &data_bits_needed);
					} else {
						data_bits_needed =
							(OPENVLC_HEADER_BYTES - 2u +
							 (uint32_t)physical_len +
							 OPENVLC_CRC_BYTES) * 8u;
					}
					if (!data_bits_needed ||
					    data_bits_needed > OPENVLC_MAX_SYMBOLS / 2u) {
						synced = false;
						sr = 0u;
						nsyms = 0u;
						history = 0u;
						history_count = 0u;
						phase_symbol_count = 0u;
						bad_pairs = 0u;
						consecutive_bad_pairs = 0u;
						max_consecutive_bad_pairs = 0u;
						manchester_pairs = 0u;
						comp_packet_timing_init(&tracker, timing);
						relocks++;
					}
				}
				continue;
			}
			if (phase == 0u) {
				a_sym = sym;
				phase = 1u;
				continue;
			}
			phase = 0u;
			manchester_pairs++;
			if (a_sym == sym) {
				/*
				 * A damaged Manchester pair has two equal cells.
				 * Some analog paths preserve the observed second
				 * cell; the Pi HAT capture instead shows short-pulse
				 * cancellation preserving the first cell and losing
				 * the opposite second cell. Count the defect either
				 * way, but allow the board profile to choose the
				 * reconstruction policy.
				 */
				bad_pairs++;
				consecutive_bad_pairs++;
				if (consecutive_bad_pairs >
				    max_consecutive_bad_pairs)
					max_consecutive_bad_pairs =
						consecutive_bad_pairs;
				if (consecutive_bad_pairs >
				    OPENVLC_SFD_SYNC_MAX_CONSECUTIVE_BAD_PAIRS) {
#if OPENVLC_RX_LOCAL_SYMBOL_RECOVERY
					/*
					 * Once a plausible physical length has been
					 * decoded, an extended bad-pair run can be the
					 * phase slip caused by one ambiguous run count.
					 * Keep collecting only to that frame's known
					 * boundary, then let the bounded local repair
					 * retry the saved symbols.  Dropping sync here
					 * used to discard both the valid length and the
					 * candidate that could repair the frame.
					 */
					if (local_symbol_recovery &&
					    local_capture_active &&
					    stream.last_data_bits_needed != 0u) {
						local_target_count =
							2u * (16u +
							(size_t)stream.
								last_data_bits_needed);
						local_frame_failed = true;
						continue;
					}
#endif
					synced = false;
					sr = 0;
					nsyms = 0;
					history = 0;
					history_count = 0;
					bad_pairs = 0;
					consecutive_bad_pairs = 0;
					max_consecutive_bad_pairs = 0;
					manchester_pairs = 0;
					timing_error_sq = 0;
					timing_intervals = 0;
					timing_outliers = 0;
					timing_sample_phase = 0;
					comp_packet_timing_init(&tracker, timing);
					if (stream.emitted_bits > maxbits)
						maxbits = stream.emitted_bits;
					manchester_stream_gap(&stream);
					continue;
				}
			} else
				consecutive_bad_pairs = 0;
			{
				uint8_t recovered_sym = sym;
				uint8_t out_bit;
				openvlc_status_t status;

				if (a_sym == sym && bad_pair_invert_first)
					recovered_sym = (uint8_t)(a_sym ^ 1u);
				out_bit = pol ? (uint8_t)(recovered_sym ^ 1u) :
						recovered_sym;
				status = manchester_stream_feed_bit(
					cfg, &stream, out_bit, packet, frame,
					frame_cap, &frame_len, &len_raw);
				if (stream.emitted_bits > maxbits)
					maxbits = stream.emitted_bits;
				if (status == OPENVLC_OK)
					goto win;
				/*
				 * Length sanity against the burst itself: right
				 * after the 16-bit length is accepted, the frame
				 * still needs data_bits_needed Manchester bits =
				 * 2x that in line cells. The burst's remaining
				 * cell budget is known from its tick span; a
				 * corrupted length that cannot possibly fit is a
				 * false lock - reject NOW instead of consuming
				 * thousands of cells in the wrong phase first.
				 * 25% slack covers timing-estimate error.
				 */
				if (stream.state == MANCHESTER_READ_DATA &&
				    stream.data_bits == 0u &&
				    stream.data_bits_needed) {
					uint64_t span = edges[edge_count - 1u] -
							edges[i - 1u];
					uint64_t cells_left = tracker.nominal_ticks ?
						span / tracker.nominal_ticks : 0u;
					uint64_t cells_needed =
						(uint64_t)stream.data_bits_needed * 2u;

					if (cells_needed > cells_left +
							   cells_left / 4u + 8u) {
						synced = false;
						sr = 0u;
						nsyms = 0u;
						history = 0u;
						history_count = 0u;
						bad_pairs = 0u;
						consecutive_bad_pairs = 0u;
						max_consecutive_bad_pairs = 0u;
						manchester_pairs = 0u;
						timing_error_sq = 0u;
						timing_intervals = 0u;
						timing_outliers = 0u;
						timing_sample_phase = 0u;
						comp_packet_timing_init(&tracker,
									timing);
						relocks++;
						manchester_stream_gap(&stream);
						continue;
					}
				}
				if (stream.state == MANCHESTER_SEARCH_SFD) {
#if OPENVLC_RX_LOCAL_SYMBOL_RECOVERY
					if (local_symbol_recovery &&
					    local_capture_active &&
					    stream.last_parse_status !=
						    OPENVLC_OK &&
					    stream.last_data_bits_needed != 0u) {
						local_target_count =
							2u * (16u +
							(size_t)stream.
								last_data_bits_needed);
						local_frame_failed = true;
						continue;
					}
#endif
					/*
					 * The locked SFD led to an impossible length,
					 * overflow, CRC/RS failure, or other parse
					 * reject. Treat it as a false lock and resume
					 * SFD search within the same burst instead of
					 * consuming the remaining symbols in the wrong
					 * phase.
					 */
					synced = false;
					sr = 0u;
					nsyms = 0u;
					history = 0u;
					history_count = 0u;
					bad_pairs = 0u;
					consecutive_bad_pairs = 0u;
					max_consecutive_bad_pairs = 0u;
					manchester_pairs = 0u;
					timing_error_sq = 0u;
					timing_intervals = 0u;
					timing_outliers = 0u;
					timing_sample_phase = 0u;
					comp_packet_timing_init(&tracker, timing);
					relocks++;
					continue;
				}
			}
		}
		level = !level;
	}

	if (phase_recovery && synced) {
		size_t phase_target_cells = 0u;

		if (phase_symbol_count >= 32u) {
			uint16_t physical_len = 0u;
			uint32_t data_bits_needed = 0u;

			for (size_t bit = 0u; bit < 16u; bit++) {
				uint8_t recovered =
					g_comp_phase_symbols[2u * bit + 1u];
				uint8_t out_bit =
					pol ? (uint8_t)(recovered ^ 1u) :
					recovered;

				physical_len =
					(uint16_t)((physical_len << 1) |
						   out_bit);
			}
			if (physical_len > OPENVLC_MAX_PAYLOAD_BYTES) {
				(void)beaglebone_data_bits_from_any_preamble(
					physical_len, &data_bits_needed);
			} else {
				data_bits_needed =
					(OPENVLC_HEADER_BYTES - 2u +
					 (uint32_t)physical_len +
					 OPENVLC_CRC_BYTES) * 8u;
			}
			if (data_bits_needed &&
			    data_bits_needed <=
				    (OPENVLC_MAX_SYMBOLS / 2u))
				phase_target_cells =
					2u * (16u + data_bits_needed);
		}

		/*
		 * Realtime experiment: repair the cached cell stream before its only
		 * Manchester/frame parse. This removes the full uncorrected parse paid
		 * by every damaged frame. The detector is deliberately conservative;
		 * CRC/RS remains the final acceptance gate.
		 */
		if (OPENVLC_RX_PHASE_REPAIR_BEFORE_DECODE) {
			phase_symbol_count = comp_phase_repair_symbols(
				g_comp_phase_symbols, g_comp_phase_timing_margin,
				phase_symbol_count, sizeof(g_comp_phase_symbols),
				phase_target_cells, &phase_edits);
			if (comp_decode_phase_symbols(
				    cfg, g_comp_phase_symbols, phase_symbol_count,
				    pol, bad_pair_invert_first, packet, frame,
				    frame_cap, &stream, &frame_len, &len_raw,
				    &manchester_pairs, &bad_pairs,
				    &max_consecutive_bad_pairs, &maxbits))
				goto win;
			goto phase_decode_failed;
		}

		/*
		 * Fast path for a clean packet: decode the cached symbols exactly
		 * once before constructing any list candidates. The bounded beam is
		 * therefore paid only by a frame that already failed CRC/RS.
		 */
		if (comp_decode_phase_symbols(
			    cfg, g_comp_phase_symbols, phase_symbol_count,
			    pol, bad_pair_invert_first, packet, frame,
			    frame_cap, &stream, &frame_len, &len_raw,
			    &manchester_pairs, &bad_pairs,
			    &max_consecutive_bad_pairs, &maxbits))
			goto win;

#if OPENVLC_RX_LOCAL_SYMBOL_RECOVERY
		if (OPENVLC_RX_LIST_RECOVERY &&
		    phase_target_cells != 0u &&
		    list_candidate_count != 0u) {
			uint32_t list_edits = 0u;

			for (size_t index = 1u;
			     index < list_candidate_count; index++) {
				comp_local_symbol_candidate_t value =
					list_candidates[index];
				size_t position = index;

				while (position != 0u &&
				       value.position <
					       list_candidates[position - 1u].
						       position) {
					list_candidates[position] =
						list_candidates[position - 1u];
					position--;
				}
				list_candidates[position] = value;
			}
			if (comp_list_symbol_recover(
				    cfg, g_comp_phase_symbols,
				    phase_symbol_count,
				    sizeof(g_comp_phase_symbols),
				    list_candidates, list_candidate_count,
				    phase_target_cells, repair_mode, pol,
				    bad_pair_invert_first, packet, frame,
				    frame_cap, &stream, &frame_len, &len_raw,
				    &manchester_pairs, &bad_pairs,
				    &max_consecutive_bad_pairs, &maxbits,
				    &list_edits)) {
				phase_edits = list_edits;
				goto win;
			}
		}
#endif

		phase_symbol_count = comp_phase_repair_symbols(
			g_comp_phase_symbols,
#if OPENVLC_RX_LOCAL_SYMBOL_RECOVERY
			list_candidate_count != 0u ?
				NULL : g_comp_phase_timing_margin,
#else
			g_comp_phase_timing_margin,
#endif
			phase_symbol_count,
			sizeof(g_comp_phase_symbols),
			phase_target_cells, &phase_edits);
		phase = 0u;
		if (comp_decode_phase_symbols(
			    cfg, g_comp_phase_symbols, phase_symbol_count,
			    pol, bad_pair_invert_first, packet, frame,
			    frame_cap, &stream, &frame_len, &len_raw,
			    &manchester_pairs, &bad_pairs,
			    &max_consecutive_bad_pairs, &maxbits))
			goto win;
		if (phase_symbol_count & 1u) {
			phase = 1u;
			a_sym = g_comp_phase_symbols[phase_symbol_count - 1u];
		}
	phase_decode_failed:
		;
	}

	/*
	 * The burst boundary can clip the final line transition(s), leaving the
	 * frame one or two bits short of completion. Feed a few best-effort
	 * trailing bits so it can finish; a guessed bit is just one more byte
	 * for Reed-Solomon to repair.
	 */
	if (synced) {
		uint32_t t;

		for (t = 0; t < 4u; t++) {
			uint8_t guess;

			if (phase == 1u)
				guess = pol ? a_sym : (uint8_t)(a_sym ^ 1u);
			else
				guess = pol ? (uint8_t)(level ? 0u : 1u) :
					      (uint8_t)(level ? 1u : 0u);
			phase = 0u;
			if (manchester_stream_feed_bit(
				    cfg, &stream, guess, packet, frame,
				    frame_cap, &frame_len, &len_raw) ==
			    OPENVLC_OK) {
				if (stream.emitted_bits > maxbits)
					maxbits = stream.emitted_bits;
				goto win;
			}
			if (stream.emitted_bits > maxbits)
				maxbits = stream.emitted_bits;
		}
	}

#if OPENVLC_RX_LOCAL_SYMBOL_RECOVERY
local_recover:
	if (local_target_count == 0u &&
	    stream.last_data_bits_needed != 0u)
		local_target_count =
			2u * (16u + (size_t)stream.last_data_bits_needed);
	if (local_symbol_recovery && local_capture_active &&
	    stream.last_data_bits_needed != 0u) {
		int32_t original_parse_status = stream.last_parse_status;
		uint16_t original_len_raw = stream.last_len_raw;
		uint32_t original_data_bits_needed =
			stream.last_data_bits_needed;
		size_t original_symbol_count = local_symbol_count;
		size_t repaired_symbol_count;
		uint32_t repaired_edits = 0u;
		uint32_t local_edits = 0u;

		if (original_symbol_count != local_target_count) {
			if (comp_local_symbol_recover(
			    cfg, g_comp_phase_symbols, &local_symbol_count,
			    sizeof(g_comp_phase_symbols),
			    local_candidates, local_candidate_count,
			    local_target_count, pol, bad_pair_invert_first,
			    packet, frame, frame_cap, &stream, &frame_len,
			    &len_raw, &manchester_pairs, &bad_pairs,
			    &max_consecutive_bad_pairs, &maxbits,
			    &local_edits)) {
				phase_edits = local_edits;
				goto win;
			}
		} else {
			repaired_symbol_count = comp_phase_repair_symbols(
				g_comp_phase_symbols, NULL,
				original_symbol_count,
				sizeof(g_comp_phase_symbols),
				local_target_count, &repaired_edits);
			while (repaired_edits != 0u &&
			       repaired_symbol_count < local_target_count &&
			       repaired_symbol_count <
				       sizeof(g_comp_phase_symbols)) {
				g_comp_phase_symbols[repaired_symbol_count] =
					repaired_symbol_count ?
						(uint8_t)(
							g_comp_phase_symbols[
								repaired_symbol_count -
								1u] ^
							1u) :
						0u;
				repaired_symbol_count++;
			}
			if (repaired_symbol_count > local_target_count)
				repaired_symbol_count = local_target_count;
			if (repaired_edits != 0u &&
			    comp_decode_phase_symbols(
				    cfg, g_comp_phase_symbols,
				    repaired_symbol_count, pol,
				    bad_pair_invert_first, packet, frame,
				    frame_cap, &stream, &frame_len, &len_raw,
				    &manchester_pairs, &bad_pairs,
				    &max_consecutive_bad_pairs, &maxbits)) {
				phase_edits = repaired_edits;
				goto win;
			}
		}
		stream.last_parse_status = original_parse_status;
		stream.last_len_raw = original_len_raw;
		stream.last_data_bits_needed = original_data_bits_needed;
	}
#endif

no_sync:
#if OPENVLC_RX_DIFFERENTIAL_FALLBACK
	if (stream.last_parse_status == OPENVLC_ERR_CRC &&
	    !alt_symbol_overflow && alt_symbol_count != 0u &&
	    comp_decode_differential_fallback(
		    cfg, alt_symbol_count, sfd_pat,
		    bad_pair_invert_first, packet, frame, frame_cap,
		    &stream, &frame_len, &len_raw, &manchester_pairs,
		    &bad_pairs, &max_consecutive_bad_pairs, &maxbits,
		    &phase_edits)) {
		g_comp_differential_won = true;
		goto win;
	}
#endif
	*out_syncs = syncs;
	*out_maxbits = maxbits;
	*out_lenraw = stream.last_len_raw ? stream.last_len_raw : len_raw;
	*out_preamble_rejects = preamble_rejects;
	*out_parse_status = stream.last_parse_status;
	openvlc_phy_dbg_sfdsync_relocks = relocks;
	openvlc_phy_dbg_phase_edits = phase_edits;
	comp_packet_timing_export_debug(&tracker);
	return OPENVLC_ERR_SYNC;

win:
	if (quality) {
		memset(quality, 0, sizeof(*quality));
		quality->samples_per_symbol =
			(uint16_t)tracker.nominal_ticks;
		quality->threshold = OPENVLC_COMP_THRESHOLD_DAC;
		quality->matched_score = (int32_t)stream.emitted_bits;
		quality_set_edge_metrics(quality, tracker.nominal_ticks,
					 timing_error_sq,
					 timing_intervals, timing_outliers,
					 manchester_pairs, bad_pairs);
		quality->manchester_max_bad_run =
			max_consecutive_bad_pairs > UINT16_MAX ? UINT16_MAX :
			(uint16_t)max_consecutive_bad_pairs;
	}
	*out_syncs = syncs;
	*out_maxbits = maxbits;
	*out_lenraw = len_raw;
	*out_preamble_rejects = preamble_rejects;
	*out_parse_status = OPENVLC_OK;
	openvlc_phy_dbg_sfdsync_relocks = relocks;
	openvlc_phy_dbg_phase_edits = phase_edits;
	comp_packet_timing_export_debug(&tracker);
	return OPENVLC_OK;
}

static OPENVLC_RX_HOT openvlc_status_t comp_decode_sfd_sync(
	const openvlc_runtime_config_t *cfg,
	const uint32_t *edges, size_t edge_count,
	openvlc_packet_t *packet,
	openvlc_quality_t *quality,
	uint8_t *frame, size_t frame_cap)
{
	comp_timing_model_t timing;
	uint32_t sfd_pat;
	uint32_t best_syncs = 0u;
	uint32_t best_maxbits = 0u;
	uint16_t best_lenraw = 0u;
#if OPENVLC_RX_DIFFERENTIAL_FALLBACK
	g_comp_differential_won = false;
#endif
	uint32_t best_preamble_rejects = 0u;
	uint32_t best_mode = 0u;
	int32_t best_parse_status = OPENVLC_ERR_SYNC;
	size_t timing_edge_count = edge_count;
	const bool primary_bad_pair_policy =
		OPENVLC_SFD_SYNC_BAD_PAIR_INVERT_FIRST != 0u;
#ifndef OPENVLC_RX_PRIMARY_PHASE_RECOVERY
#define OPENVLC_RX_PRIMARY_PHASE_RECOVERY 0u
#endif
	const struct {
		uint8_t repair_mode;
		bool bad_pair_invert_first;
		bool boundary_recovery;
		uint8_t phase_recovery;
	} hypotheses[] = {
#if OPENVLC_RX_PRIMARY_PHASE_RECOVERY
		/*
		 * Saturated realtime profile: spend the single available pass on the
		 * packet-cached phase-aware decoder.  Running the streaming pass first
		 * leaves no deadline for this path on an 8 ms packet cadence.
		 */
		{ 1u, primary_bad_pair_policy, false, 1u },
#else
		/*
		 * Primary realtime path: decode each reconstructed cell immediately.
		 * It neither stores nor scans a packet-sized symbol array and is the
		 * only hypothesis used by the saturated STM32 profile.
		 */
		{ 1u, primary_bad_pair_policy, false, 0u },
#if OPENVLC_RX_PHASE_RECOVERY
		/*
		 * Cached phase/list recovery is retained for offline replay and
		 * lower-rate profiles that explicitly grant another hypothesis.
		 */
		{ 1u, primary_bad_pair_policy, false, 1u },
#endif
#endif
		/* Alternate boundary tie policy remains a final compatibility pass. */
		{ 1u, primary_bad_pair_policy, true, 0u },
	};
	size_t hypothesis_count = sizeof(hypotheses) / sizeof(hypotheses[0]);
	size_t passes_used = 0u;
#ifdef OPENVLC_SFD_SYNC_HYPOTHESES_MAX
	size_t hypothesis_limit = OPENVLC_SFD_SYNC_HYPOTHESES_MAX;
#else
	size_t hypothesis_limit = 3u;
#endif

	if (!edges || edge_count < 32u) {
		openvlc_phy_dbg_sfdsync_fail_no_sfd++;
		return OPENVLC_ERR_SYNC;
	}
	/*
	 * Timing training needs more context than SFD search because the AGC can
	 * distort the burst head. The validated capture exposes its reliable
	 * polarity-specific 37/27-tick run inside the first ~1031 intervals.
	 * Scanning beyond the dedicated training window into the remaining
	 * ~10,000 payload edges is a redundant full-packet pass.
	 */
	if (timing_edge_count > OPENVLC_RX_TIMING_SEARCH_EDGES)
		timing_edge_count = OPENVLC_RX_TIMING_SEARCH_EDGES;
	if (!comp_estimate_timing_model(edges, timing_edge_count, &timing)) {
		openvlc_phy_dbg_sfdsync_fail_timing++;
		return OPENVLC_ERR_SYNC;
	}
	if (openvlc_rx_hypothesis_budget != 0u &&
	    (size_t)openvlc_rx_hypothesis_budget < hypothesis_limit)
		hypothesis_limit = openvlc_rx_hypothesis_budget;
	openvlc_phy_dbg_sps = timing.nominal_ticks;
	sfd_pat = comp_manch_pattern(OPENVLC_SFD_WORD, OPENVLC_SFD_BITS);
	openvlc_phy_dbg_sfdsync_single = timing.nominal_ticks;
	openvlc_phy_dbg_sfdsync_cell0 = timing.level_ticks[0];
	openvlc_phy_dbg_sfdsync_cell1 = timing.level_ticks[1];
	openvlc_phy_dbg_sfdsync_train = timing.training_intervals;
	openvlc_phy_dbg_sfdsync_split = timing.nominal_ticks;
	openvlc_phy_dbg_sfdsync_result = 0u;
	openvlc_phy_dbg_sfdsync_pre_rejects = 0u;
	openvlc_phy_dbg_sfdsync_pre_badmax = 0u;
	openvlc_phy_dbg_sfdsync_pre_sfdmin = 99u;
	openvlc_phy_dbg_sfdsync_sfd_errors = 0u;
	openvlc_phy_dbg_sfdsync_relocks = 0u;
	openvlc_phy_dbg_sfdsync_lock_cell = 0u;
	openvlc_phy_dbg_phase_edits = 0u;
	openvlc_phy_dbg_list_trials = 0u;
	openvlc_phy_dbg_local_trials = 0u;
	openvlc_phy_dbg_sfdsync_mode = 0u;
	openvlc_phy_dbg_track_cell0_end = timing.level_ticks[0];
	openvlc_phy_dbg_track_cell1_end = timing.level_ticks[1];
	openvlc_phy_dbg_track_nominal_end = timing.nominal_ticks;
	openvlc_phy_dbg_timing_residual_peak = 0u;
	if (hypothesis_limit > hypothesis_count)
		hypothesis_limit = hypothesis_count;

	for (size_t h = 0u; h < hypothesis_limit; h++) {
		uint8_t repair_mode = hypotheses[h].repair_mode;
		bool bad_pair_invert_first = hypotheses[h].bad_pair_invert_first;
		bool boundary_recovery = hypotheses[h].boundary_recovery;
		uint8_t phase_recovery = hypotheses[h].phase_recovery;
		uint32_t mode = ((uint32_t)repair_mode << 1) |
				(bad_pair_invert_first ? 1u : 0u) |
				(boundary_recovery ? 8u : 0u) |
				(phase_recovery ? 16u : 0u);
		uint32_t syncs = 0u;
		uint32_t maxbits = 0u;
		uint16_t lenraw = 0u;
		uint32_t preamble_rejects = 0u;
		int32_t parse_status = OPENVLC_ERR_SYNC;
		openvlc_status_t status;

		if (openvlc_rx_hypothesis_budget != 0u &&
		    passes_used >= openvlc_rx_hypothesis_budget)
			break;
		passes_used++;
		status = comp_sfd_sync_pass(
			cfg, edges, edge_count, &timing, sfd_pat,
			repair_mode, bad_pair_invert_first, boundary_recovery,
			phase_recovery, h == 0u, SIZE_MAX, SIZE_MAX,
			packet, quality, frame, frame_cap,
			&syncs, &maxbits, &lenraw, &preamble_rejects,
			&parse_status);

		if (syncs > best_syncs ||
		    (syncs == best_syncs && maxbits > best_maxbits) ||
		    (syncs == best_syncs && maxbits == best_maxbits &&
		     preamble_rejects > best_preamble_rejects)) {
			best_syncs = syncs;
			best_maxbits = maxbits;
			best_lenraw = lenraw;
			best_preamble_rejects = preamble_rejects;
			best_mode = mode;
			best_parse_status = parse_status;
		}
		if (status != OPENVLC_OK)
			continue;

#if OPENVLC_COMP_NOMINAL_HALFCELL_TICKS > 0u
		comp_anchor_update(timing.nominal_ticks);
		openvlc_phy_dbg_nominal_anchor = g_nominal_anchor;
#endif
		openvlc_phy_dbg_stage = 22u;
		openvlc_phy_dbg_sps = timing.nominal_ticks;
		openvlc_phy_dbg_len_raw = lenraw;
		openvlc_phy_dbg_payload_len = packet->payload_len;
		openvlc_phy_dbg_parse_status = OPENVLC_OK;
		openvlc_phy_dbg_sfdsync_split = timing.nominal_ticks;
		openvlc_phy_dbg_sfdsync_syncs = syncs;
		openvlc_phy_dbg_sfdsync_maxbits = maxbits;
		openvlc_phy_dbg_sfdsync_lenraw = lenraw;
		openvlc_phy_dbg_sfdsync_result = 1u;
		openvlc_phy_dbg_sfdsync_pre_rejects = preamble_rejects;
		openvlc_phy_dbg_sfdsync_mode = mode;
#if OPENVLC_RX_DIFFERENTIAL_FALLBACK
		if (g_comp_differential_won)
			openvlc_phy_dbg_sfdsync_mode |= 32u;
#endif
		return OPENVLC_OK;
	}

	/*
	 * A real comparator transition can be narrowed until the capture filter
	 * no longer reports it. Two runs then merge and a two-cell interval is
	 * quantised as three. The ordinary long-run reconstruction is correct
	 * when three physical cells really elapsed, so do not change the primary
	 * path. If every normal hypothesis failed after finding an SFD, retry only
	 * the few packet-local three-cell decisions, one at a time, as two cells.
	 * Length, Reed-Solomon and CRC still have to validate the complete frame;
	 * a guessed packet can never escape this fallback.
	 */
#if OPENVLC_RX_THREE_CELL_RETRY_MAX
	if (best_syncs != 0u) {
		size_t candidates[OPENVLC_RX_THREE_CELL_RETRY_MAX];
		size_t candidate_count = comp_collect_three_cell_candidates(
			&timing, edges, edge_count, candidates,
			OPENVLC_RX_THREE_CELL_RETRY_MAX);

		for (size_t candidate = 0u;
		     candidate < candidate_count; candidate++) {
			uint32_t syncs = 0u;
			uint32_t maxbits = 0u;
			uint16_t lenraw = 0u;
			uint32_t preamble_rejects = 0u;
			int32_t parse_status = OPENVLC_ERR_SYNC;
			openvlc_status_t status;

			if (openvlc_rx_hypothesis_budget != 0u &&
			    passes_used >= openvlc_rx_hypothesis_budget)
				break;
			passes_used++;
			status = comp_sfd_sync_pass(
				cfg, edges, edge_count, &timing, sfd_pat,
				1u, primary_bad_pair_policy, true, 0u,
				false, candidates[candidate], SIZE_MAX,
				packet, quality, frame, frame_cap,
				&syncs, &maxbits, &lenraw,
				&preamble_rejects, &parse_status);

			if (status != OPENVLC_OK)
				continue;
#if OPENVLC_COMP_NOMINAL_HALFCELL_TICKS > 0u
			comp_anchor_update(timing.nominal_ticks);
			openvlc_phy_dbg_nominal_anchor = g_nominal_anchor;
#endif
			openvlc_phy_dbg_stage = 22u;
			openvlc_phy_dbg_sps = timing.nominal_ticks;
			openvlc_phy_dbg_len_raw = lenraw;
			openvlc_phy_dbg_payload_len = packet->payload_len;
			openvlc_phy_dbg_parse_status = OPENVLC_OK;
			openvlc_phy_dbg_sfdsync_split = timing.nominal_ticks;
			openvlc_phy_dbg_sfdsync_syncs = syncs;
			openvlc_phy_dbg_sfdsync_maxbits = maxbits;
			openvlc_phy_dbg_sfdsync_lenraw = lenraw;
			openvlc_phy_dbg_sfdsync_result = 1u;
			openvlc_phy_dbg_sfdsync_pre_rejects =
				preamble_rejects;
			openvlc_phy_dbg_sfdsync_mode = 11u;
			openvlc_phy_dbg_phase_edits = 1u;
			return OPENVLC_OK;
		}
	}
#endif

	/*
	 * Preserve the proven lower-boundary decision in the realtime path, then
	 * use the packet CRC to test the upper decision only inside the narrow
	 * configured boundary margin. Correct packets never enter this fallback.
	 */
#if OPENVLC_RX_BOUNDARY_RETRY_MAX
	if (best_syncs != 0u) {
		size_t candidates[OPENVLC_RX_BOUNDARY_RETRY_MAX];
		size_t candidate_count = comp_collect_boundary_ties(
			&timing, edges, edge_count, candidates,
			OPENVLC_RX_BOUNDARY_RETRY_MAX);

		for (size_t candidate = 0u;
		     candidate < candidate_count; candidate++) {
			uint32_t syncs = 0u;
			uint32_t maxbits = 0u;
			uint16_t lenraw = 0u;
			uint32_t preamble_rejects = 0u;
			int32_t parse_status = OPENVLC_ERR_SYNC;
			openvlc_status_t status;

			if (openvlc_rx_hypothesis_budget != 0u &&
			    passes_used >= openvlc_rx_hypothesis_budget)
				break;
			passes_used++;
			status = comp_sfd_sync_pass(
				cfg, edges, edge_count, &timing, sfd_pat,
				1u, primary_bad_pair_policy, false, 0u,
				false, SIZE_MAX, candidates[candidate],
				packet, quality, frame, frame_cap,
				&syncs, &maxbits, &lenraw,
				&preamble_rejects, &parse_status);

			if (status != OPENVLC_OK)
				continue;
#if OPENVLC_COMP_NOMINAL_HALFCELL_TICKS > 0u
			comp_anchor_update(timing.nominal_ticks);
			openvlc_phy_dbg_nominal_anchor = g_nominal_anchor;
#endif
			openvlc_phy_dbg_stage = 22u;
			openvlc_phy_dbg_sps = timing.nominal_ticks;
			openvlc_phy_dbg_len_raw = lenraw;
			openvlc_phy_dbg_payload_len = packet->payload_len;
			openvlc_phy_dbg_parse_status = OPENVLC_OK;
			openvlc_phy_dbg_sfdsync_split = timing.nominal_ticks;
			openvlc_phy_dbg_sfdsync_syncs = syncs;
			openvlc_phy_dbg_sfdsync_maxbits = maxbits;
			openvlc_phy_dbg_sfdsync_lenraw = lenraw;
			openvlc_phy_dbg_sfdsync_result = 1u;
			openvlc_phy_dbg_sfdsync_pre_rejects =
				preamble_rejects;
			openvlc_phy_dbg_sfdsync_mode = 35u;
			openvlc_phy_dbg_phase_edits = 1u;
			return OPENVLC_OK;
		}
	}
#endif

	openvlc_phy_dbg_sfdsync_split = timing.nominal_ticks;
	openvlc_phy_dbg_sfdsync_syncs = best_syncs;
	openvlc_phy_dbg_sfdsync_maxbits = best_maxbits;
	openvlc_phy_dbg_sfdsync_lenraw = best_lenraw;
	openvlc_phy_dbg_sfdsync_pre_rejects = best_preamble_rejects;
	openvlc_phy_dbg_sfdsync_mode = best_mode;
	openvlc_phy_dbg_parse_status = best_parse_status;
	if (best_syncs == 0u) {
		if (best_preamble_rejects)
			openvlc_phy_dbg_sfdsync_fail_preamble++;
		else
			openvlc_phy_dbg_sfdsync_fail_no_sfd++;
	} else {
		openvlc_phy_dbg_sfdsync_fail_parse++;
		switch ((openvlc_status_t)best_parse_status) {
		case OPENVLC_ERR_CRC:
			openvlc_phy_dbg_sfdsync_fail_crc++;
			return OPENVLC_ERR_CRC;
		case OPENVLC_ERR_ARG:
			openvlc_phy_dbg_sfdsync_fail_len++;
			break;
		case OPENVLC_ERR_OVERFLOW:
			openvlc_phy_dbg_sfdsync_fail_overflow++;
			break;
		case OPENVLC_ERR_SYNC:
		default:
			openvlc_phy_dbg_sfdsync_fail_incomplete++;
			break;
		}
	}
	return OPENVLC_ERR_SYNC;
}
OPENVLC_RX_HOT openvlc_status_t openvlc_rx_edges_to_packet(
	const openvlc_runtime_config_t *cfg,
	const uint32_t *edges, size_t edge_count,
	uint16_t *scratch_samples, size_t scratch_cap,
	openvlc_packet_t *packet, openvlc_quality_t *quality)
{
	static uint8_t frame[OPENVLC_MAX_FRAME_BYTES] OPENVLC_BULK_BUFFER;

	if (!cfg || !edges || !packet)
		return OPENVLC_ERR_ARG;
	(void)scratch_samples;
	(void)scratch_cap;
	memset(packet, 0, sizeof(*packet));
	if (quality)
		memset(quality, 0, sizeof(*quality));

	openvlc_phy_dbg_stage = 20u;
	openvlc_phy_dbg_sample_len = (uint32_t)edge_count;
	openvlc_phy_dbg_sps = 0;
	openvlc_phy_dbg_len_raw = 0;
	openvlc_phy_dbg_payload_len = 0;
	openvlc_phy_dbg_parse_status = OPENVLC_ERR_SYNC;
	openvlc_phy_dbg_sfdsync_mode = 0u;
	openvlc_phy_dbg_track_cell0_end = 0u;
	openvlc_phy_dbg_track_cell1_end = 0u;
	openvlc_phy_dbg_track_nominal_end = 0u;
	openvlc_phy_dbg_timing_residual_peak = 0u;
	if (edge_count < OPENVLC_PREAMBLE_BITS / 2u) {
		openvlc_phy_dbg_sfdsync_fail_no_sfd++;
		return OPENVLC_ERR_SYNC;
	}

	/*
	 * The SFD-correlated decoder owns the whole decode: it estimates the
	 * cell timing, locks on the SFD symbol pattern, Manchester-decodes the
	 * payload and runs the frame parse (CRC + Reed-Solomon). It sets
	 * openvlc_phy_dbg_stage to 22 on success.
	 */
	return comp_decode_sfd_sync(cfg, edges, edge_count, packet, quality,
				    frame, sizeof(frame));
}

#if defined(OPENVLC_RX_STREAMING) && OPENVLC_RX_STREAMING
/*
 * ---------------------------------------------------------------------------
 * Streaming edge decoder (openvlc_stream_rx_*)
 *
 * Pulse widths are classified against the nominal half-cell with a wide
 * tolerance rather than tracked to the tick. That is deliberate: a cell
 * narrowed by ISI still classifies correctly, where a tight timing model has
 * to repair it or drop the frame.
 *
 * Bit phase needs no guessing. A 2T interval can only run from one mid-bit
 * transition to the next, so the edge that ENDS a long interval is always a
 * mid-bit - an unambiguous sync anchor. After a mid-bit: a long interval means
 * the next edge is also mid-bit; a short interval means the next edge is a bit
 * boundary and the one after it is the mid-bit.
 *
 * Optical polarity is unknown, and unlike the burst path there is no correlator
 * to resolve it, so both polarities run as parallel lanes. The lane whose SFD
 * matches completes a frame; the other never does. Two small state machines
 * cost far less than one correlator over a search window.
 * ---------------------------------------------------------------------------
 */
#ifndef OPENVLC_STREAM_EPS_DIV
#define OPENVLC_STREAM_EPS_DIV 2u	/* eps = T/2, mirrors EPS_VALUE = 0.5 */
#endif

/*
 * Per-polarity half-cell tracking. OFF BY DEFAULT - it was implemented on a
 * strong prior (the device logs read t0=36 / t1=27 against a 32 nominal, and
 * the burst decoder does track the two separately) and it MADE THINGS WORSE:
 * frames decoded on the synthetic campaign fell 93/109 -> 63/109, and sync
 * failures that had been exactly zero appeared (no_sfd 0 -> 6, bad_len 0 -> 5).
 * The adaptive estimate poisons itself - most likely it is updated from
 * intervals seen before sync and from d/2 on long intervals, both noisy, and
 * the clamp window is wide enough to let it drift. Kept behind the flag rather
 * than deleted so the next attempt starts from measured ground, not from the
 * same prior a second time.
 */
#ifndef OPENVLC_STREAM_TRACK_POLARITY
#define OPENVLC_STREAM_TRACK_POLARITY 0u
#endif

/*
 * How many half-cells an over-long interval may span and still be treated as a
 * missing transition rather than a lost burst. Measured: over-long intervals
 * caused 77 of 77 desyncs, sub-legal ones zero, so this - not glitch absorption
 * - is where the streaming decoder loses frames.
 */
#ifndef OPENVLC_STREAM_ANOMALY_ZERO_BIT
#define OPENVLC_STREAM_ANOMALY_ZERO_BIT 0u  /* 1 = reference policy: one zero bit */
#endif
#ifndef OPENVLC_STREAM_MAX_SKIP
#define OPENVLC_STREAM_MAX_SKIP 4u
#endif

/*
 * Half-cell width at which a gap is treated as a frame boundary and the byte
 * lanes are re-armed, rather than merely desyncing the interval state.
 *
 * MEASURED A/B on hardware, same rig, ~5500 frames each:
 *   5  (every non-bridgeable gap)  sync 2.84/s  loss 2.27%  bridge 122.5 fps
 *   32 (>= 16 us of silence)       sync 5.40/s  loss 3.92%  bridge 120.0 fps
 *
 * 32 came from a model that the dark period between frames is ONE silence, so
 * a 16 us threshold would fire once per frame: predicted sr ~125/s. It measured
 * sr ~1520/s - about twelve silences of >= 16 us per frame period. The dark gap
 * is fragmented by comparator chatter and is NOT identifiable by duration at
 * this scale, so the model was wrong, and so was the conclusion drawn from it
 * (that mid-payload abandonment came from the re-arm firing inside frames).
 *
 * What the A/B does establish, independently of any model: carrying the lanes
 * across 5-31 half-cell gaps costs more than it recovers. Lower is better, and
 * MAX_SKIP + 1 is the floor - anything below is bridged and never reaches here.
 */
#ifndef OPENVLC_STREAM_REARM_CELLS
#define OPENVLC_STREAM_REARM_CELLS (OPENVLC_STREAM_MAX_SKIP + 1u)
#endif
/* Absolute override, so the tolerance can be swept without touching the code. */
#ifndef OPENVLC_STREAM_EPS_TICKS
#define OPENVLC_STREAM_EPS_TICKS 	(OPENVLC_COMP_NOMINAL_HALFCELL_TICKS / OPENVLC_STREAM_EPS_DIV)
#endif

typedef struct {
	openvlc_manchester_stream_t fsm;
	uint8_t frame[OPENVLC_MAX_FRAME_BYTES];
} openvlc_stream_lane_t;

static struct {
	uint32_t last_edge;
	uint32_t carry;		/* absorbed sub-legal intervals */
	uint32_t t_hi;		/* tracked half-cell, level high */
	uint32_t t_lo;		/* tracked half-cell, level low  */
	bool have_last;
	bool synced;
	bool pending_half;	/* saw the boundary half, mid-bit comes next */
	bool level;		/* toggles every edge; absolute sense unknown */
	bool inited;
	openvlc_stream_lane_t lane[2];	/* [0] as decoded, [1] inverted */
} openvlc_stream_rx;

/* Diagnostics: how often each recovery path actually fires. */
uint32_t openvlc_stream_dbg_absorbed;	/* sub-legal intervals merged */
uint32_t openvlc_stream_dbg_toolong;	/* over-long intervals -> resync */
uint32_t openvlc_stream_dbg_desync;	/* sync lost for any reason */
uint32_t openvlc_stream_dbg_skipped;	/* missing transitions bridged */
uint32_t openvlc_stream_dbg_rearm;	/* lanes re-armed on a real gap    */

void openvlc_stream_rx_reset(void)
{
	memset(&openvlc_stream_rx, 0, sizeof(openvlc_stream_rx));
	openvlc_stream_rx.t_hi = OPENVLC_COMP_NOMINAL_HALFCELL_TICKS;
	openvlc_stream_rx.t_lo = OPENVLC_COMP_NOMINAL_HALFCELL_TICKS;
	manchester_stream_init(&openvlc_stream_rx.lane[0].fsm, 0u);
	manchester_stream_init(&openvlc_stream_rx.lane[1].fsm, 0u);
	openvlc_stream_rx.inited = true;
}

/*
 * Report the framing state machine of whichever polarity lane got furthest, so
 * a failed decode can be attributed instead of guessed at: no SFD found at all,
 * SFD found but the length field rejected, or a full frame that failed parse.
 */
void openvlc_stream_rx_debug(uint32_t *sfd_hits, uint32_t *len_ok,
			     uint32_t *len_bad, int32_t *last_parse)
{
	const openvlc_manchester_stream_t *a = &openvlc_stream_rx.lane[0].fsm;
	const openvlc_manchester_stream_t *b = &openvlc_stream_rx.lane[1].fsm;
	const openvlc_manchester_stream_t *best =
		(b->sfd_hits > a->sfd_hits) ? b : a;

	if (sfd_hits)
		*sfd_hits = best->sfd_hits;
	if (len_ok)
		*len_ok = best->len_valid_hits;
	if (len_bad)
		*len_bad = best->len_invalid_hits;
	if (last_parse)
		*last_parse = best->last_parse_status;
}

static openvlc_status_t openvlc_stream_emit_bit(
	const openvlc_runtime_config_t *cfg, uint8_t bit,
	openvlc_packet_t *packet)
{
	unsigned lane;

	for (lane = 0u; lane < 2u; lane++) {
		openvlc_stream_lane_t *l = &openvlc_stream_rx.lane[lane];
		size_t frame_len = 0u;
		uint16_t len_raw = 0u;
		uint8_t b = (lane == 0u) ? bit : (uint8_t)(bit ^ 1u);

		if (manchester_stream_feed_bit(cfg, &l->fsm, b, packet, l->frame,
					       sizeof(l->frame), &frame_len,
					       &len_raw) == OPENVLC_OK)
			return OPENVLC_OK;
	}
	return OPENVLC_ERR_SYNC;
}

openvlc_status_t openvlc_stream_rx_push(const openvlc_runtime_config_t *cfg,
					const uint32_t *edges,
					size_t edge_count,
					openvlc_packet_t *packet)
{
	const uint32_t nominal = OPENVLC_COMP_NOMINAL_HALFCELL_TICKS;
	const uint32_t eps = OPENVLC_STREAM_EPS_TICKS;
	const uint32_t t_min = nominal / 2u;
	const uint32_t t_max = nominal * 2u;
	openvlc_status_t result = OPENVLC_ERR_SYNC;
	size_t i;

	if (!openvlc_stream_rx.inited)
		openvlc_stream_rx_reset();
	if (!edges || !packet)
		return OPENVLC_ERR_SYNC;

	for (i = 0u; i < edge_count; i++) {
		uint32_t now = edges[i];
		uint32_t d;
		uint32_t t_half;
		bool is_short, is_long, valid, level_during;

		if (!openvlc_stream_rx.have_last) {
			openvlc_stream_rx.last_edge = now;
			openvlc_stream_rx.have_last = true;
			continue;
		}
		d = now - openvlc_stream_rx.last_edge;
		openvlc_stream_rx.last_edge = now;
		/* Every edge flips the level, glitch edges included. */
		openvlc_stream_rx.level = !openvlc_stream_rx.level;

		/*
		 * Absorb sub-legal intervals instead of dropping sync. A glitch
		 * is a narrow pulse, so it splits one real interval into three
		 * (a, g, rest) and adds TWO edges. Accumulating until the total
		 * is legal restores the real width, and because the absorbed
		 * count stays odd the level parity comes out right on its own -
		 * no special case needed.
		 *
		 * The asymmetry is deliberate: too SHORT is a glitch and is
		 * recoverable, too LONG is a genuine missing transition or an
		 * inter-frame gap and must still reset. Dropping sync on both,
		 * as before, threw away a whole frame for one spurious pulse.
		 */
		openvlc_stream_rx.carry += d;
		d = openvlc_stream_rx.carry;

		/*
		 * Per-polarity nominal. The LED driver is not symmetric: the
		 * device logs read t0=36 / t1=27 ticks against a 32 nominal, a
		 * 33% difference between the two levels, and the synthetic
		 * campaign reproduces it with DCD_STEP. Classifying both levels
		 * against one nominal puts intervals on the wrong side of the
		 * 1.5T boundary and produces exactly the payload bit errors the
		 * failure attribution pointed at (no_sfd=0, bad_len=0,
		 * parse=16). This is what t0/t1 do in the burst decoder.
		 *
		 * level_during is the level the line held for THIS interval,
		 * i.e. the one before the edge that just toggled it.
		 */
		level_during = !openvlc_stream_rx.level;
#if defined(OPENVLC_STREAM_TRACK_POLARITY) && OPENVLC_STREAM_TRACK_POLARITY
		t_half = level_during ? openvlc_stream_rx.t_hi :
					openvlc_stream_rx.t_lo;
#else
		t_half = nominal;
#endif
		if (d + eps < t_half) {
			openvlc_stream_dbg_absorbed++;
			continue;	/* keep absorbing */
		}
		openvlc_stream_rx.carry = 0u;

		/*
		 * Two independent jobs, previously conflated in eps: WHERE the
		 * 1T/2T decision falls, and HOW FAR out an interval is still
		 * legal. The eps sweep showed the decision point dominates -
		 * yield peaked exactly where t_half + eps landed on 1.5T and
		 * fell away on both sides. So put the boundary at 1.5T (the
		 * midpoint between the two legal widths) and let eps bound
		 * validity only.
		 *
		 * MEASURED on the synthetic campaign (109 attempts, burst
		 * decoder 108). eps in ticks -> frames decoded:
		 *   coupled   12:56.9%  16:84.4%  20:67.0%  24:37.6%
		 *   decoupled 12:67.9%  16:85.3%  20:85.3%  24:85.3%
		 * The peak barely moved; what disappeared is the cliff. Being
		 * generous with the tolerance now costs nothing, so a channel
		 * with wider timing spread than this model degrades gracefully
		 * instead of falling off. Zero corrupt packets at all 16 points
		 * of both sweeps.
		 */
		valid = (d + eps >= t_half) && (d <= 2u * t_half + eps);
		is_long = valid && (d >= t_half + t_half / 2u);
		is_short = valid && !is_long;

#if defined(OPENVLC_STREAM_TRACK_POLARITY) && OPENVLC_STREAM_TRACK_POLARITY
		if (valid) {
			/* Slow exponential average, clamped so a run of bad
			 * intervals cannot drag the estimate away for good. */
			uint32_t obs = is_long ? (d / 2u) : d;
			uint32_t *est = level_during ?
					&openvlc_stream_rx.t_hi :
					&openvlc_stream_rx.t_lo;

			*est = (*est * 7u + obs) / 8u;
			if (*est < t_min)
				*est = t_min;
			else if (*est > t_max)
				*est = t_max;
		}
#else
		(void)t_min;
		(void)t_max;
		(void)level_during;
#endif

		if (!is_short && !is_long) {
			uint32_t cells = (d + t_half / 2u) / t_half;

			openvlc_stream_dbg_toolong++;
#if defined(OPENVLC_STREAM_ANOMALY_ZERO_BIT) && OPENVLC_STREAM_ANOMALY_ZERO_BIT
			/*
			 * The reference implementation's policy, read from its
			 * source. Its classifier has no else branch:
			 *   if      (in_range(val, PERIOD_HALF_BIT, eps)) ...
			 *   else if (in_range(val, PERIOD_BIT,      eps)) ...
			 * but bit_pos-- sits at the BOTTOM of the loop body,
			 * outside that if/else, so it runs regardless. An
			 * out-of-window interval therefore consumes exactly one
			 * bit slot and leaves that bit at 0, because byte is
			 * never OR'd for that position. It is not ignored, and
			 * sync is untouched - wait_For_SFD is raised only by an
			 * SFD byte mismatch, never by a timing anomaly.
			 *
			 * So: one zero bit, no desync, no re-arm, no bridging.
			 * Four policies now exist here and none of the other
			 * three is this: full reset (original), best-effort
			 * bridging of cells/2 bits, and desync + re-arm.
			 *
			 * Note the theoretical cost, which is why this is a flag
			 * and not a default: a long interval means a transition
			 * was lost, i.e. TWO cells passed, so emitting one bit
			 * drops a bit and shifts everything after it. That is
			 * survivable there because that receiver has no CRC -
			 * it forwards decoded bytes straight to the UART - so a
			 * shifted frame becomes corrupt data delivered rather
			 * than a counted loss. Here the CRC will reject it.
			 */
			(void)cells;
			(void)openvlc_stream_emit_bit(cfg, 0u, packet);
			continue;
#else
			/*
			 * An over-long interval means a transition was lost,
			 * not that the burst ended - provided the gap is still
			 * a small whole number of half-cells. Keep the phase
			 * (advancing an odd number of half-cells flips mid-bit
			 * vs boundary, an even number does not) and emit
			 * best-effort bits for the cells we could not observe.
			 * They are wrong as often as right, which is what the
			 * Reed-Solomon layer downstream is for: 8 ECC bytes per
			 * 200-byte block via OPENVLC_BEAGLEBONE_COMPAT, good
			 * for 4 byte-errors per block. A bridged run inside one
			 * block is repairable; spread across several is not,
			 * which is why MAX_SKIP stays small. The burst decoder
			 * does the same with corrupted pairs.
			 */
			if (openvlc_stream_rx.synced &&
			    cells >= 3u && cells <= OPENVLC_STREAM_MAX_SKIP) {
				uint32_t k;

				openvlc_stream_dbg_skipped++;
				for (k = 0u; k < cells / 2u; k++) {
					if (openvlc_stream_emit_bit(
						    cfg,
						    openvlc_stream_rx.level ?
							    1u : 0u,
						    packet) == OPENVLC_OK)
						result = OPENVLC_OK;
				}
				if (cells & 1u)
					openvlc_stream_rx.pending_half =
						!openvlc_stream_rx.pending_half;
				continue;
			}
			if (openvlc_stream_rx.synced)
				openvlc_stream_dbg_desync++;
			openvlc_stream_rx.synced = false;
			openvlc_stream_rx.pending_half = false;
#if defined(OPENVLC_STREAM_GAP_REARM) && OPENVLC_STREAM_GAP_REARM
			/*
			 * MEASURED, not assumed: on hardware sp reads ~121
			 * against a 125 fps pace while sync counts only ~1.5/s,
			 * so ~3 frames/s are lost without ever being counted as
			 * an attempt - they never produce an SFD hit at all.
			 * Offline this class did not exist (no_sfd=0) because
			 * the bench feeds one clean frame at a time.
			 *
			 * The cause is here. This branch cleared the interval
			 * state but left the two byte-level lanes untouched, so
			 * whatever the comparator chattered during the ~7 ms
			 * dark gap stayed in them. A lane parked in
			 * MANCHESTER_PAYLOAD on a noise-born length then eats
			 * the next real preamble and SFD as payload bytes, and
			 * that frame is lost silently.
			 *
			 * The burst pipeline never had this problem: segmenting
			 * on the gap re-armed the decoder at every frame
			 * boundary. Re-arm explicitly instead - the streaming
			 * equivalent of that boundary. manchester_stream_gap()
			 * resets framing only; sfd_hits and friends survive, so
			 * the attempt accounting in openvlc_app_commit_rx_stream
			 * stays monotonic.
			 *
			 * Cost: a frame whose internal gap exceeds MAX_SKIP
			 * half-cells is now abandoned rather than carried, where
			 * before it might still have completed. Which way that
			 * trades is what the flag is for.
			 */
			/*
			 * Only a real inter-frame silence re-arms. MEASURED:
			 * with the re-arm firing on every non-bridgeable gap,
			 * slen read 6820/1 against seen=6821 - every SFD hit
			 * reads a VALID length, and exactly one was rejected in
			 * the whole run. So the residual failures are not
			 * framing and not the length field: they are frames
			 * abandoned mid-payload, which is why crc stayed at a
			 * measured 0 (they never reach openvlc_frame_parse, so
			 * no CRC is ever computed). An in-frame dropout of a
			 * few half-cells was being treated as a frame boundary.
			 *
			 * That reading did not survive its own test: raising the
			 * threshold to 32 half-cells made the loss worse, not
			 * better (2.27% -> 3.92%). See OPENVLC_STREAM_REARM_CELLS
			 * for the A/B and why the model behind it was wrong. The
			 * threshold stays as a knob, at its measured best.
			 */
			if (cells >= OPENVLC_STREAM_REARM_CELLS) {
				openvlc_stream_dbg_rearm++;
				manchester_stream_gap(
					&openvlc_stream_rx.lane[0].fsm);
				manchester_stream_gap(
					&openvlc_stream_rx.lane[1].fsm);
			}
#endif
			continue;
#endif
		}

		if (!openvlc_stream_rx.synced) {
			if (is_long) {
				openvlc_stream_rx.synced = true;
				openvlc_stream_rx.pending_half = false;
				if (openvlc_stream_emit_bit(
					    cfg,
					    openvlc_stream_rx.level ? 1u : 0u,
					    packet) == OPENVLC_OK)
					result = OPENVLC_OK;
			}
			continue;
		}

		if (openvlc_stream_rx.pending_half) {
			if (!is_short) {
				openvlc_stream_rx.synced = false;
				openvlc_stream_rx.pending_half = false;
				continue;
			}
			openvlc_stream_rx.pending_half = false;
		} else if (!is_long) {
			openvlc_stream_rx.pending_half = true;
			continue;	/* bit boundary carries no bit */
		}

		if (openvlc_stream_emit_bit(cfg,
					    openvlc_stream_rx.level ? 1u : 0u,
					    packet) == OPENVLC_OK)
			result = OPENVLC_OK;
	}
	return result;
}
#endif /* OPENVLC_RX_STREAMING */

#if defined(OPENVLC_RX_REFERENCE) && OPENVLC_RX_REFERENCE
/*
 * Faithful port of the reference implementation's edges->bits stage
 * (demodulate_Manchester() in LiFi_Manchester.c), so the two strategies can be
 * compared on one link instead of argued about.
 *
 * Ported exactly:
 *   - two adjacent windows, in_range(val, T/2, eps) and in_range(val, T, eps),
 *     with eps = T/2 * 0.5. On its 124-tick half-cell that is [62,186] and
 *     [186,310]; on our 32-tick half-cell it is [16,48] and [48,80]. Identical
 *     once normalised, boundary at 1.5T either way.
 *   - bit_repeat pairing: a short interval emits a bit and arms bit_repeat, and
 *     the following interval is then consumed without emitting anything.
 *   - a long interval flips the bit and emits it.
 *   - an out-of-window interval emits ONE bit of value 0. Its classifier has no
 *     else branch, but bit_pos-- sits at the bottom of the loop outside it, so
 *     the slot advances with the byte never OR'd for that position. Sync is not
 *     touched.
 *   - acquisition: while unsynced, shorts only set bit = 1; the first LONG
 *     interval seeds bit = 0, emits it, and unlocks. Its 0xFF preamble is all
 *     short intervals, so the first long is the entry into the SFD.
 *
 * Deliberately NOT ported: its wait_For_SFD re-anchor on an SFD byte mismatch.
 * That exists to recover bit alignment after a failed match; our byte layer
 * already slides the SFD search bit by bit, which subsumes it. Everything from
 * bits upward - SFD, length, payload, CRC - is our unchanged framing, which is
 * the point: this isolates the edges->bits strategy as the only variable.
 *
 * Single polarity, as in the original, and no edge filtering: absolute polarity
 * cannot matter to a pulse-width decode, only the initial seed can, and that is
 * OPENVLC_REF_SEED_BIT.
 */
/*
 * Bit value seeded at acquisition, i.e. the bit the first long interval after
 * the preamble represents. Framing-dependent, not a tuning knob.
 *
 * The original seeds 0 because its preamble is 0xFF: in Manchester that is a
 * run of 1s, hence all SHORT intervals, and the first long is the transition
 * into the leading 0 of its SFD byte 0x2A. Our preamble is 0xAA - alternating
 * 1s and 0s - which is one short followed by all longs, and the bit at that
 * first long is 1, not 0.
 *
 * MEASURED, 109-attempt campaign: seed 0 -> ref_ok=0, seed 1 -> ref_ok=103.
 * Acquisition succeeded either way (acquired=109); with the wrong seed the
 * whole bit stream is inverted and the SFD never matches.
 */
/*
 * Consecutive in-window intervals required before acquisition is declared.
 *
 * 1 = faithful to the original, which seeds on the FIRST long interval after
 * the preamble. That is safe on its bench because its idle line is silent, so
 * the first long can only be the preamble's. MEASURED here it is not: with the
 * comparator chattering through the dark gap the decoder seeds on a NOISE long
 * with the wrong half-cell pairing phase, then rides the whole frame with no
 * anomaly to re-anchor it. Hardware, mode 4 faithful: ~28 acquisitions/s
 * against a 125 fps pace, ref=81950/81941/81940 - anomalies, acquisitions and
 * re-anchors all the same number, i.e. thrashing in the gap. Of the frames it
 * did acquire it decoded 97.1%, so the bit recovery was never the problem.
 *
 * Raising this requires a RUN of legal intervals before trusting the anchor,
 * which noise cannot supply and a real preamble always can.
 */
/*
 * Default 16, NOT the faithful 1. Set 1 to reproduce the original exactly; the
 * measurement above is what that costs on this hardware. 16 legal intervals is
 * ~8-16 us of uninterrupted signal: our 8-byte 0xAA preamble supplies ~65 of
 * them, chatter does not supply 16 in a row. Offline the gate is free - 103/109
 * at 1, 4, 8, 16 and 32 alike - because that bench has no idle-line noise to
 * reject, so only hardware can show what it buys.
 */
#ifndef OPENVLC_REF_ACQ_RUN
#define OPENVLC_REF_ACQ_RUN 16u
#endif

#ifndef OPENVLC_REF_SEED_BIT
#define OPENVLC_REF_SEED_BIT 1u
#endif

uint32_t openvlc_ref_dbg_anomaly;	/* out-of-window -> one zero bit    */
uint32_t openvlc_ref_dbg_acquired;	/* long intervals that unlocked     */
uint32_t openvlc_ref_dbg_reanchor;	/* pairing phase re-seeded          */

static struct {
	openvlc_manchester_stream_t fsm;
	uint8_t frame[OPENVLC_MAX_FRAME_BYTES];
	uint32_t last_edge;
	bool have_last;
	bool synced;
	bool bit_repeat;
	uint8_t bit;
	uint32_t run;
	bool inited;
} openvlc_ref_rx;

void openvlc_ref_rx_reset(void)
{
	memset(&openvlc_ref_rx, 0, sizeof(openvlc_ref_rx));
	manchester_stream_init(&openvlc_ref_rx.fsm, 0u);
	openvlc_ref_rx.inited = true;
}

void openvlc_ref_rx_debug(uint32_t *sfd_hits, uint32_t *len_ok,
			  uint32_t *len_bad, int32_t *last_parse)
{
	const openvlc_manchester_stream_t *f = &openvlc_ref_rx.fsm;

	if (sfd_hits)
		*sfd_hits = f->sfd_hits;
	if (len_ok)
		*len_ok = f->len_valid_hits;
	if (len_bad)
		*len_bad = f->len_invalid_hits;
	if (last_parse)
		*last_parse = f->last_parse_status;
}

static openvlc_status_t openvlc_ref_emit_bit(
	const openvlc_runtime_config_t *cfg, uint8_t bit,
	openvlc_packet_t *packet)
{
	size_t frame_len = 0u;
	uint16_t len_raw = 0u;

	return manchester_stream_feed_bit(cfg, &openvlc_ref_rx.fsm, bit, packet,
					  openvlc_ref_rx.frame,
					  sizeof(openvlc_ref_rx.frame),
					  &frame_len, &len_raw);
}

/* in_range_u32() from the original, unsigned-wrap trick and all. */
static inline bool openvlc_ref_in_range(uint32_t val, uint32_t centre,
					uint32_t eps)
{
	return (uint32_t)(val - (centre - eps)) <= (2u * eps);
}

openvlc_status_t openvlc_ref_rx_push(const openvlc_runtime_config_t *cfg,
				     const uint32_t *edges, size_t edge_count,
				     openvlc_packet_t *packet)
{
	const uint32_t t_half = OPENVLC_COMP_NOMINAL_HALFCELL_TICKS;
	const uint32_t t_bit = 2u * t_half;
	const uint32_t eps = t_half / 2u;
	openvlc_status_t result = OPENVLC_ERR_SYNC;
	size_t i;

	if (!openvlc_ref_rx.inited)
		openvlc_ref_rx_reset();
	if (!edges || !packet)
		return OPENVLC_ERR_SYNC;

	for (i = 0u; i < edge_count; i++) {
		uint32_t now = edges[i];
		uint32_t d;
		uint8_t emit;

		if (!openvlc_ref_rx.have_last) {
			openvlc_ref_rx.last_edge = now;
			openvlc_ref_rx.have_last = true;
			continue;
		}
		d = now - openvlc_ref_rx.last_edge;
		openvlc_ref_rx.last_edge = now;

		if (openvlc_ref_rx.bit_repeat) {
			openvlc_ref_rx.bit_repeat = false;
			continue;
		}

		if (!openvlc_ref_rx.synced) {
			if (openvlc_ref_in_range(d, t_half, eps) ||
			    openvlc_ref_in_range(d, t_bit, eps))
				openvlc_ref_rx.run++;
			else
				openvlc_ref_rx.run = 0u;

			if (openvlc_ref_in_range(d, t_half, eps)) {
				openvlc_ref_rx.bit = 1u;
			} else if (openvlc_ref_rx.run >= OPENVLC_REF_ACQ_RUN &&
				   openvlc_ref_in_range(d, t_bit, eps)) {
				openvlc_ref_rx.bit = OPENVLC_REF_SEED_BIT;
				openvlc_ref_rx.synced = true;
				openvlc_ref_dbg_acquired++;
				if (openvlc_ref_emit_bit(cfg,
							 openvlc_ref_rx.bit,
							 packet) == OPENVLC_OK)
					result = OPENVLC_OK;
			}
			continue;
		}

		if (openvlc_ref_in_range(d, t_half, eps)) {
			openvlc_ref_rx.bit_repeat = true;
			emit = openvlc_ref_rx.bit;
		} else if (openvlc_ref_in_range(d, t_bit, eps)) {
			openvlc_ref_rx.bit = (uint8_t)(openvlc_ref_rx.bit ^ 1u);
			emit = openvlc_ref_rx.bit;
		} else {
			/*
			 * The original leaves `bit` alone here and simply never
			 * ORs the byte for this position, so the slot reads 0
			 * while the running bit state carries on untouched.
			 * Clobbering bit instead corrupts every interval after
			 * it, which is what this port did on its first run:
			 * ref_ok=0 with acquired=109.
			 */
			openvlc_ref_dbg_anomaly++;
			/*
			 * Re-anchor while the SFD has not been found yet. This
			 * is the original's wait_For_SFD, which the first cut
			 * of this port left out on the argument that our
			 * sliding SFD search subsumes it. It does not: the
			 * sliding search fixes BYTE alignment, while the thing
			 * that breaks here is the half-cell PAIRING phase -
			 * which short pairs with which - and only re-seeding
			 * fixes that. With the phase wrong every short pair
			 * straddles a bit boundary and the frame is lost, which
			 * is what halved the throughput on hardware: sp fell
			 * from ~125 to ~60 while crc finally started counting.
			 *
			 * The original reaches the same place by a different
			 * route: during the dark gap its byte machine keeps
			 * comparing noise against SFD_BYTE, every mismatch sets
			 * wait_For_SFD, so it re-anchors continuously until a
			 * real frame arrives. Gating on "SFD not yet found" is
			 * that same condition, expressed against our framing.
			 */
			if (openvlc_ref_rx.fsm.state == MANCHESTER_SEARCH_SFD) {
				openvlc_ref_rx.synced = false;
				openvlc_ref_rx.bit_repeat = false;
				openvlc_ref_rx.run = 0u;
				openvlc_ref_dbg_reanchor++;
				continue;
			}
			emit = 0u;
		}
		if (openvlc_ref_emit_bit(cfg, emit, packet) == OPENVLC_OK)
			result = OPENVLC_OK;
	}
	return result;
}
#endif /* OPENVLC_RX_REFERENCE */
