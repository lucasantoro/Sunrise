#include "openvlc_app.h"

#include <string.h>

#include "openvlc_frame.h"
#include "openvlc_phy.h"

/*
 * The per-packet VLC_RX log costs ~1 ms of UART per packet. In host-forward
 * mode (Raspberry bridge) it competes with IP records for the serial link and
 * caps the deliverable frame rate, while the 1 Hz COMP summary already carries
 * the counters - so it is disabled there by default. Define
 * OPENVLC_PER_PACKET_LOG_DISABLED=0 to force it back on.
 */
#ifndef OPENVLC_PER_PACKET_LOG_DISABLED
#if defined(OPENVLC_RX_HOST_FORWARD) && OPENVLC_RX_HOST_FORWARD
#define OPENVLC_PER_PACKET_LOG_DISABLED 1
#else
#define OPENVLC_PER_PACKET_LOG_DISABLED 0
#endif
#endif

static openvlc_runtime_config_t app_cfg;
static openvlc_counters_t app_counters;
static volatile uint32_t app_last_status;

uint8_t openvlc_quality_finalize(openvlc_quality_t *quality,
				 uint16_t payload_len,
				 bool rs_checked,
				 int rs_corrected)
{
	uint32_t protected_bytes = (uint32_t)payload_len + OPENVLC_HEADER_BYTES;
	uint32_t rs_rate_x1000 = 0u;
	uint32_t rs_penalty = 0u;
	uint32_t lqi;

	if (!quality)
		return 0u;
	quality->rs_checked = rs_checked;
	quality->rs_corrected_bytes = 0u;
	quality->rs_correction_x1000 = 0u;

	if (rs_checked && rs_corrected > 0) {
		uint32_t corrected = (uint32_t)rs_corrected;

		quality->rs_corrected_bytes =
			corrected > UINT16_MAX ? UINT16_MAX : (uint16_t)corrected;
		if (protected_bytes)
			rs_rate_x1000 =
				(uint32_t)(((uint64_t)corrected * 1000u +
					    protected_bytes / 2u) /
					   protected_bytes);
		quality->rs_correction_x1000 = rs_rate_x1000;
		rs_penalty = (rs_rate_x1000 * 3u + 5u) / 10u;
		if (rs_penalty > 30u)
			rs_penalty = 30u;
	}

	/* Always start from the PHY result so repeated finalization is stable. */
	lqi = quality->pre_fec_quality;
	quality->link_quality =
		(uint8_t)(rs_penalty >= lqi ? 0u : lqi - rs_penalty);
	return quality->link_quality;
}

void openvlc_app_init(const openvlc_runtime_config_t *cfg)
{
	openvlc_frame_init();
	if (cfg)
		app_cfg = *cfg;
	else {
		app_cfg.self_id = OPENVLC_ADDR_SELF_DEFAULT;
		app_cfg.peer_id = OPENVLC_ADDR_PEER_DEFAULT;
		app_cfg.line_code = OPENVLC_LINE_CODE;
		app_cfg.samples_per_symbol = OPENVLC_SAMPLES_PER_SYMBOL;
		app_cfg.mf_score_min = OPENVLC_MF_SCORE_MIN;
		app_cfg.snr_min_db_centi = OPENVLC_SNR_MIN_DB_CENTI;
	}
	memset(&app_counters, 0, sizeof(app_counters));
}

static void app_account_result(openvlc_status_t status,
				const openvlc_packet_t *packet,
				openvlc_quality_t *quality)
{
	if (status == OPENVLC_OK) {
		bool rs_checked = openvlc_frame_last_rs_checked();
		int rs_corrected = rs_checked ?
			openvlc_frame_last_rs_corrected() : -1;
		uint32_t timing_percent_x10 = quality->timing_jitter_x1000;

		openvlc_quality_finalize(quality, packet->payload_len,
					 rs_checked, rs_corrected);
		app_counters.frames_delivered++;
		app_last_status = status;
#if !OPENVLC_PER_PACKET_LOG_DISABLED
		openvlc_platform_log("VLC_RX packet len=%u lqi=%u tjit=%lu.%lu%% bad=%u/%lu badrun=%u tout=%lu rs=%d\r\n",
				     packet->payload_len,
				     quality->link_quality,
				     (unsigned long)(timing_percent_x10 / 10u),
				     (unsigned long)(timing_percent_x10 % 10u),
				     quality->manchester_bad_pairs,
				     (unsigned long)quality->manchester_pairs,
				     quality->manchester_max_bad_run,
				     (unsigned long)quality->timing_outliers,
				     rs_corrected);
#else
		(void)timing_percent_x10;
#endif
		openvlc_platform_on_packet(packet, quality);
	} else if (status == OPENVLC_ERR_CRC) {
		app_counters.crc_failed++;
		app_last_status = status;
	} else if (status == OPENVLC_ERR_QUALITY) {
		app_counters.quality_dropped++;
		app_last_status = status;
	} else {
		app_counters.sync_failed++;
		app_last_status = status;
	}
}

void openvlc_app_rx_edges(const uint32_t *edges, size_t edge_count,
			  uint16_t *scratch, size_t scratch_cap)
{
	openvlc_packet_t packet;
	openvlc_quality_t quality;
	openvlc_status_t status;

	if (!edges || !edge_count || !scratch || !scratch_cap)
		return;
	app_counters.frames_seen++;
	status = openvlc_rx_edges_to_packet(&app_cfg, edges, edge_count,
					    scratch, scratch_cap, &packet, &quality);
	app_account_result(status, &packet, &quality);
}

openvlc_status_t openvlc_app_try_rx_edges(const uint32_t *edges,
					  size_t edge_count,
					  uint16_t *scratch,
					  size_t scratch_cap)
{
	openvlc_packet_t packet;
	openvlc_quality_t quality;
	openvlc_status_t status;

	if (!edges || !edge_count || !scratch || !scratch_cap)
		return OPENVLC_ERR_ARG;
	status = openvlc_rx_edges_to_packet(&app_cfg, edges, edge_count,
					    scratch, scratch_cap, &packet, &quality);
	if (status == OPENVLC_OK) {
		app_counters.frames_seen++;
		app_account_result(status, &packet, &quality);
	}
	return status;
}

void openvlc_app_commit_rx_failure(openvlc_status_t status)
{
	openvlc_packet_t packet = { 0 };
	openvlc_quality_t quality = { 0 };

	/* A successful candidate was already delivered by try_rx_edges(). */
	if (status == OPENVLC_OK)
		return;
	app_counters.frames_seen++;
	app_account_result(status, &packet, &quality);
}

const openvlc_counters_t *openvlc_app_counters(void)
{
	return &app_counters;
}

openvlc_status_t openvlc_app_last_status(void)
{
	return (openvlc_status_t)app_last_status;
}
