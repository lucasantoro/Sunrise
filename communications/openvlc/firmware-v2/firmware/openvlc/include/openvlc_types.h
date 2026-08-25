#ifndef OPENVLC_TYPES_H
#define OPENVLC_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "openvlc_config.h"

typedef enum {
	OPENVLC_OK = 0,
	OPENVLC_ERR_ARG = -1,
	OPENVLC_ERR_OVERFLOW = -2,
	OPENVLC_ERR_CRC = -3,
	OPENVLC_ERR_SYNC = -4,
	OPENVLC_ERR_LINE_CODE = -5,
	OPENVLC_ERR_QUALITY = -6,
} openvlc_status_t;

typedef struct {
	uint8_t dst;
	uint8_t src;
	uint16_t protocol;
	uint16_t payload_len;
	uint8_t payload[OPENVLC_MAX_PAYLOAD_BYTES];
} openvlc_packet_t;

typedef struct {
	uint32_t frames_seen;
	uint32_t frames_delivered;
	uint32_t crc_failed;
	uint32_t sync_failed;
	uint32_t quality_dropped;
	uint32_t tx_frames;
} openvlc_counters_t;

typedef struct {
	uint16_t high_mean;
	uint16_t low_mean;
	uint32_t signal_power;
	uint32_t noise_power;
	uint32_t snr_x1000;
	/* Kept for source compatibility; mirrors timing_jitter_x1000. */
	uint32_t jitter_x1000;
	/* RMS edge-timing residual / recovered half-cell, scaled by 1000. */
	uint32_t timing_jitter_x1000;
	/* Invalid Manchester pairs / all recovered pairs, scaled by 1000. */
	uint32_t manchester_bad_x1000;
	/* Corrected RS bytes / protected bytes, scaled by 1000. */
	uint32_t rs_correction_x1000;
	uint32_t timing_intervals;
	uint32_t timing_outliers;
	uint32_t manchester_pairs;
	uint16_t manchester_bad_pairs;
	uint16_t manchester_max_bad_run;
	uint16_t rs_corrected_bytes;
	int32_t snr_db_centi;
	int32_t matched_score;
	uint16_t adc_min;
	uint16_t adc_max;
	uint16_t threshold;
	uint16_t samples_per_symbol;
	/* PHY-only quality before Reed-Solomon correction is considered. */
	uint8_t pre_fec_quality;
	uint8_t link_quality;
	bool rs_checked;
	bool snr_valid;
} openvlc_quality_t;

typedef struct {
	uint8_t self_id;
	uint8_t peer_id;
	uint8_t line_code;
	uint16_t samples_per_symbol;
	int32_t mf_score_min;
	int32_t snr_min_db_centi;
} openvlc_runtime_config_t;

#endif
