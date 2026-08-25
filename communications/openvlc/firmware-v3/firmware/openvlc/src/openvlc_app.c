#include "openvlc_app.h"

#include <string.h>

#include "openvlc_frame.h"
#include "openvlc_phy.h"

#ifndef OPENVLC_STREAM_POLL_CHUNK_MAX
#define OPENVLC_STREAM_POLL_CHUNK_MAX 64u
#endif
#ifndef OPENVLC_STREAM_DEGLITCH
#define OPENVLC_STREAM_DEGLITCH 1u
#endif

#if OPENVLC_RX_STREAMING >= 4
void openvlc_ref_rx_reset(void);
void openvlc_ref_rx_debug(uint32_t *sfd_hits, uint32_t *len_ok,
			  uint32_t *len_bad, int32_t *last_parse);
openvlc_status_t openvlc_ref_rx_push(const openvlc_runtime_config_t *cfg,
				     const uint32_t *edges, size_t edge_count,
				     openvlc_packet_t *packet);
#endif

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

#if defined(OPENVLC_RX_STREAMING) && OPENVLC_RX_STREAMING
/*
 * Shadow mode. The burst decoder stays authoritative and its packet is the one
 * delivered; the streaming decoder runs on the SAME edges and only moves
 * counters. Nothing it does can affect the link, so this is safe to fly while
 * the two are still being compared on the real channel.
 *
 * Reported in the COMP line as st=<ok>/<seen> stm=<mismatch>. Promote the
 * streaming decoder only when st tracks ok on hardware, not before.
 */
uint32_t openvlc_stream_shadow_seen;
uint32_t openvlc_stream_shadow_ok;
uint32_t openvlc_stream_shadow_mismatch;

void openvlc_stream_rx_debug(uint32_t *sfd_hits, uint32_t *len_ok,
			     uint32_t *len_bad, int32_t *last_parse);

#define OPENVLC_STREAM_SHADOW_CHUNK 64u

/*
 * Sample rate. Running the shadow on EVERY frame costs ~2600 us on top of the
 * burst decoder's ~4500, which is 7120 of an 8000 us period: measured on
 * hardware the edge ring backed up (rp 8000 -> 31700), started dropping edges
 * (rd 0 -> 280 and climbing) and throughput fell 124 -> 111 fps. The statistics
 * do not need every frame - 1 in 8 still yields ~15 samples/s, which is
 * thousands per minute, at an eighth of the cost.
 */
#ifndef OPENVLC_STREAM_SHADOW_EVERY
#define OPENVLC_STREAM_SHADOW_EVERY 8u
#endif

/*
 * Run the streaming decoder over one burst's edges. Returns OPENVLC_OK with the
 * packet filled, or the truthful failure status: if the frame reached
 * openvlc_frame_parse() and was rejected there, that status is reported so the
 * COMP line still distinguishes crc from sync instead of blaming everything on
 * sync.
 */
static openvlc_status_t app_stream_decode(const uint32_t *edges,
					  size_t edge_count,
					  openvlc_packet_t *packet)
{
	size_t off;

	openvlc_stream_rx_reset();
	memset(packet, 0, sizeof(*packet));
	for (off = 0u; off < edge_count; off += OPENVLC_STREAM_SHADOW_CHUNK) {
		size_t n = edge_count - off;

		if (n > OPENVLC_STREAM_SHADOW_CHUNK)
			n = OPENVLC_STREAM_SHADOW_CHUNK;
		if (openvlc_stream_rx_push(&app_cfg, edges + off, n, packet) ==
		    OPENVLC_OK)
			return OPENVLC_OK;
	}
	{
		uint32_t sfd = 0u, lok = 0u, lbad = 0u;
		int32_t parse = 0;

		openvlc_stream_rx_debug(&sfd, &lok, &lbad, &parse);
		if (lok && parse != 0)
			return (openvlc_status_t)parse;
	}
	return OPENVLC_ERR_SYNC;
}

#if OPENVLC_RX_STREAMING < 2
static void app_stream_shadow(const uint32_t *edges, size_t edge_count,
			      openvlc_status_t burst_status,
			      const openvlc_packet_t *burst_packet)
{
	openvlc_packet_t packet;
	bool got;

	{
		static uint32_t tick;

		if (++tick % OPENVLC_STREAM_SHADOW_EVERY)
			return;
	}
	openvlc_stream_shadow_seen++;
	got = app_stream_decode(edges, edge_count, &packet) == OPENVLC_OK;
	if (!got)
		return;
	openvlc_stream_shadow_ok++;
	/* Only meaningful when the burst decoder also produced a packet. */
	if (burst_status == OPENVLC_OK && burst_packet &&
	    (packet.payload_len != burst_packet->payload_len ||
	     memcmp(packet.payload, burst_packet->payload,
		    packet.payload_len) != 0))
		openvlc_stream_shadow_mismatch++;
}
#endif /* OPENVLC_RX_STREAMING < 2 */
#endif

void openvlc_app_rx_edges(const uint32_t *edges, size_t edge_count,
			  uint16_t *scratch, size_t scratch_cap)
{
	openvlc_packet_t packet;
	openvlc_quality_t quality;
	openvlc_status_t status;

	if (!edges || !edge_count || !scratch || !scratch_cap)
		return;
	app_counters.frames_seen++;
#if defined(OPENVLC_RX_STREAMING) && OPENVLC_RX_STREAMING >= 2
	/*
	 * Streaming decoder AUTHORITATIVE - the burst decoder is not called at
	 * all on this path. Measured on the real channel before the switch:
	 * streaming 97.1% against the burst decoder's 99.7% over 5716 frames,
	 * with zero disagreements between them (stm=0). The 2.6 point gap is
	 * the price; what it buys is one decoder instead of two, and the whole
	 * burst-segmentation failure class gone.
	 *
	 * openvlc_quality_t has no streaming equivalent, so sc/jit in the COMP
	 * line read zero in this mode. Do not read them as a signal collapse.
	 */
	(void)scratch;
	(void)scratch_cap;
	memset(&quality, 0, sizeof(quality));
	status = app_stream_decode(edges, edge_count, &packet);
	openvlc_stream_shadow_seen++;
	if (status == OPENVLC_OK)
		openvlc_stream_shadow_ok++;
#else
	status = openvlc_rx_edges_to_packet(&app_cfg, edges, edge_count,
					    scratch, scratch_cap, &packet, &quality);
#if defined(OPENVLC_RX_STREAMING) && OPENVLC_RX_STREAMING
	app_stream_shadow(edges, edge_count, status, &packet);
#endif
#endif
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
#if defined(OPENVLC_RX_STREAMING) && OPENVLC_RX_STREAMING >= 2
	/* Second RX entry point - the soft-gap probe. Must use the same decoder
	 * as the committed path, or "burst decoder disabled" would only be half
	 * true and the two could disagree on the same burst. */
	(void)scratch;
	(void)scratch_cap;
	memset(&quality, 0, sizeof(quality));
	status = app_stream_decode(edges, edge_count, &packet);
#else
	status = openvlc_rx_edges_to_packet(&app_cfg, edges, edge_count,
					    scratch, scratch_cap, &packet, &quality);
#endif
	if (status == OPENVLC_OK) {
		app_counters.frames_seen++;
		app_account_result(status, &packet, &quality);
	}
	return status;
}

#if defined(OPENVLC_RX_STREAMING) && OPENVLC_RX_STREAMING >= 3
void openvlc_app_commit_rx_stream(const uint32_t *edges, size_t count)
{
	/*
	 * Accounting without bursts. There is no "one burst = one attempt" here,
	 * so an attempt is defined as an SFD hit: the framing state machine
	 * counts those, and every one of them is settled exactly once - either
	 * as the packet that just completed, or as a sync failure. seen/ok/crc
	 * in the COMP line therefore keep the same meaning as on the burst path.
	 */
	static uint32_t attempts_done;
	openvlc_packet_t packet;
	openvlc_quality_t quality;
	uint32_t attempts = 0u;
	bool got;

	if (!edges || !count)
		return;
	memset(&quality, 0, sizeof(quality));
	memset(&packet, 0, sizeof(packet));
#if OPENVLC_RX_STREAMING >= 4
	/*
	 * Mode 4: the ported reference decoder. No deglitch - the original has
	 * none - and a single polarity, so the edges go in exactly as the DMA
	 * ring delivered them. Everything above bits is our unchanged framing.
	 */
	got = openvlc_ref_rx_push(&app_cfg, edges, count, &packet) == OPENVLC_OK;
#elif defined(OPENVLC_STREAM_DEGLITCH) && OPENVLC_STREAM_DEGLITCH
	/*
	 * The one real difference left between this path and the burst path:
	 * there, edges pass openvlc_edge_cancel_short_pulses() before decoding;
	 * here they arrive raw from the DMA ring with all the comparator
	 * chatter. Cancel narrow pulses in pairs so level parity is preserved,
	 * exactly as the burst path does. Glitches spanning a chunk boundary are
	 * missed, which is what the decoder's own absorption still covers.
	 */
	{
		static uint32_t scratch[OPENVLC_STREAM_POLL_CHUNK_MAX];
		size_t n = count > OPENVLC_STREAM_POLL_CHUNK_MAX ?
			   OPENVLC_STREAM_POLL_CHUNK_MAX : count;

		memcpy(scratch, edges, n * sizeof(scratch[0]));
		n = openvlc_edge_cancel_short_pulses(
			scratch, n, OPENVLC_EDGE_MIN_INTERVAL_TICKS);
		got = openvlc_stream_rx_push(&app_cfg, scratch, n, &packet) ==
		      OPENVLC_OK;
	}
#else
	got = openvlc_stream_rx_push(&app_cfg, edges, count, &packet) ==
	      OPENVLC_OK;
#endif
	if (got) {
		attempts_done++;
		app_counters.frames_seen++;
		openvlc_stream_shadow_seen++;
		openvlc_stream_shadow_ok++;
		app_account_result(OPENVLC_OK, &packet, &quality);
	}
	{
		int32_t last_parse = OPENVLC_ERR_SYNC;

#if OPENVLC_RX_STREAMING >= 4
		openvlc_ref_rx_debug(&attempts, NULL, NULL, &last_parse);
#else
		openvlc_stream_rx_debug(&attempts, NULL, NULL, &last_parse);
#endif
		while (attempts_done < attempts) {
			attempts_done++;
			app_counters.frames_seen++;
			openvlc_stream_shadow_seen++;
			/*
			 * Report what the framing layer actually returned. This
			 * hardcoded OPENVLC_ERR_SYNC before, which made crc=0
			 * in the COMP line an artefact of the accounting rather
			 * than a measurement - every mode-3 failure was filed
			 * as a framing failure by construction, so the one
			 * counter that separates "never framed the packet" from
			 * "framed it and the payload was corrupt" could not
			 * move. app_stream_decode() already did this correctly
			 * for mode 2; mode 3 did not inherit it.
			 *
			 * The distinction decides what to do about the residual
			 * loss. A CRC failure here has already survived the
			 * Reed-Solomon layer (8 ECC bytes per 200-byte block),
			 * so it means more than four bad bytes landed in one
			 * block -- a far stronger statement than a framing
			 * failure, which never reached either check.
			 */
			app_account_result((openvlc_status_t)last_parse,
					   &packet, &quality);
		}
	}
}
#endif

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
