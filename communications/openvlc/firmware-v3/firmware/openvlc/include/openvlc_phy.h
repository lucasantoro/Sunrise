#ifndef OPENVLC_PHY_H
#define OPENVLC_PHY_H

#include "openvlc_types.h"

/*
 * Comparator + timer input-capture RX path (the only RX path on STM32).
 *
 * edges[] holds the TIM2 input-capture tick timestamps of consecutive
 * comparator-output transitions for one captured burst (monotonically
 * increasing, both edges, already filtered when an edge filter is enabled).
 * The signal level toggles on
 * every edge; absolute polarity is unknown and is resolved by the SFD-symbol
 * correlation inside the decoder. See comp_decode_sfd_sync() in openvlc_phy.c.
 *
 * scratch_samples[]/scratch_cap are unused by the edge decoder and kept only
 * for source-compatibility with the caller.
 */
openvlc_status_t openvlc_rx_edges_to_packet(const openvlc_runtime_config_t *cfg,
					    const uint32_t *edges, size_t edge_count,
					    uint16_t *scratch_samples,
					    size_t scratch_cap,
					    openvlc_packet_t *packet,
					    openvlc_quality_t *quality);

/*
 * Comparator chatter appears as a narrow pulse, hence as two adjacent edges.
 * Remove both edges in-place so Manchester level parity remains unchanged.
 */
size_t openvlc_edge_cancel_short_pulses(uint32_t *edges, size_t edge_count,
				       uint32_t min_interval);

/*
 * Packet-local comparator filter. Ultra-short pulses are removed directly;
 * pulses in the contextual aperture are removed only when joining their two
 * neighbours fits the learned per-polarity cell timing better than keeping
 * all three runs. Level parity is preserved by removing edges in pairs.
 */
size_t openvlc_edge_filter_timing_aware(uint32_t *edges, size_t edge_count,
				       uint32_t candidate_interval,
				       uint32_t hard_glitch_interval,
				       uint32_t decision_margin,
				       uint32_t *removed_edges);

#if defined(OPENVLC_RX_STREAMING) && OPENVLC_RX_STREAMING
/*
 * Streaming edge decoder. Consumes input-capture timestamps incrementally and
 * emits packets as they complete, with no burst buffer, no gap segmentation and
 * no SFD search window - the failure classes those three create do not exist
 * here. It replaces ONLY the edges->bits stage: the bit-level framing state
 * machine (SFD / length / payload) and openvlc_frame_parse() are reused
 * unchanged, so RS, CRC and the header layer are identical to the burst path.
 *
 * Returns OPENVLC_OK on the push that completes a packet, otherwise
 * OPENVLC_ERR_SYNC. Call reset() once before the first push.
 */
void openvlc_stream_rx_reset(void);
openvlc_status_t openvlc_stream_rx_push(const openvlc_runtime_config_t *cfg,
					const uint32_t *edges,
					size_t edge_count,
					openvlc_packet_t *packet);
#endif

#if defined(OPENVLC_TEST_API)
bool openvlc_test_history_has_preamble(uint64_t history,
				      uint32_t history_count);
size_t openvlc_test_phase_repair(uint8_t *symbols, size_t count,
				 size_t capacity, size_t target_count,
				 uint32_t *out_edits);
void openvlc_test_set_cell_overrides(size_t first_interval, int first_delta,
				     size_t second_interval, int second_delta,
				     size_t third_interval, int third_delta);
#endif

#endif
