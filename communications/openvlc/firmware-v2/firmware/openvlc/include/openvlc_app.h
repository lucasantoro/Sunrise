#ifndef OPENVLC_APP_H
#define OPENVLC_APP_H

#include "openvlc_types.h"

void openvlc_app_init(const openvlc_runtime_config_t *cfg);
/*
 * Deliver one captured comparator edge burst (TIM2 input-capture tick
 * timestamps) to the decoder, then account/log/deliver the result. This is the
 * single RX entry point on STM32; the platform comparator poll calls it once
 * per detected burst. scratch/scratch_cap is an unused compatibility buffer.
 */
void openvlc_app_rx_edges(const uint32_t *edges, size_t edge_count,
			  uint16_t *scratch, size_t scratch_cap);
/*
 * Speculatively decode a candidate frame boundary. A CRC/RS-valid packet is
 * accounted and delivered exactly like openvlc_app_rx_edges(); a rejected or
 * incomplete candidate has no effect on application counters/status. This lets
 * the STM32 acquisition layer bridge a short comparator dropout without
 * reporting a false frame failure or discarding the packet prefix.
 */
openvlc_status_t openvlc_app_try_rx_edges(const uint32_t *edges,
					  size_t edge_count,
					  uint16_t *scratch,
					  size_t scratch_cap);
/* Account a previously decoded, non-OK candidate without decoding it again. */
void openvlc_app_commit_rx_failure(openvlc_status_t status);
const openvlc_counters_t *openvlc_app_counters(void);
openvlc_status_t openvlc_app_last_status(void);
/*
 * Apply post-FEC information to the packet-level LQI. This updates the
 * quality structure for logging/callback use; it never accepts or rejects a
 * packet. Repeated calls are stable because the RS penalty is applied to the
 * stored pre-FEC value.
 */
uint8_t openvlc_quality_finalize(openvlc_quality_t *quality,
				 uint16_t payload_len,
				 bool rs_checked,
				 int rs_corrected);

void openvlc_platform_log(const char *fmt, ...);
void openvlc_platform_on_packet(const openvlc_packet_t *packet,
				const openvlc_quality_t *quality);

#endif
