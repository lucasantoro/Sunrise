/* Offline replay of a two-column oscilloscope/logic-analyser CSV through the
 * same comparator-edge decoder used by the STM32 firmware. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openvlc_phy.h"

#ifndef OPENVLC_CAPTURE_TIMER_HZ
#define OPENVLC_CAPTURE_TIMER_HZ 64000000u
#endif
#ifndef OPENVLC_CAPTURE_GAP_US
#define OPENVLC_CAPTURE_GAP_US 4u
#endif

extern volatile uint32_t openvlc_phy_dbg_stage;
extern volatile uint32_t openvlc_phy_dbg_len_raw;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_cell0;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_cell1;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_train;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_syncs;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_mode;
extern volatile uint32_t openvlc_phy_dbg_track_nominal_end;
extern volatile uint32_t openvlc_phy_dbg_timing_residual_peak;

static const char *status_name(openvlc_status_t status)
{
	switch (status) {
	case OPENVLC_OK: return "OK";
	case OPENVLC_ERR_ARG: return "ARG";
	case OPENVLC_ERR_OVERFLOW: return "OVERFLOW";
	case OPENVLC_ERR_CRC: return "CRC";
	case OPENVLC_ERR_SYNC: return "SYNC";
	case OPENVLC_ERR_LINE_CODE: return "LINE";
	case OPENVLC_ERR_QUALITY: return "QUALITY";
	default: return "UNKNOWN";
	}
}

static uint32_t payload_hash(const openvlc_packet_t *packet)
{
	uint32_t hash = 2166136261u;

	for (uint16_t i = 0u; i < packet->payload_len; i++) {
		hash ^= packet->payload[i];
		hash *= 16777619u;
	}
	return hash;
}

static void decode_burst(uint32_t *edges, size_t count, unsigned index)
{
	openvlc_runtime_config_t cfg = { 0 };
	openvlc_packet_t packet;
	openvlc_quality_t quality;
	uint16_t scratch[OPENVLC_RX_SAMPLE_BUFFER_LEN];
	size_t clean_count;
	openvlc_status_t status;

	clean_count = openvlc_edge_cancel_short_pulses(
		edges, count, OPENVLC_EDGE_MIN_INTERVAL_TICKS);
	status = openvlc_rx_edges_to_packet(&cfg, edges, clean_count, scratch,
					    OPENVLC_RX_SAMPLE_BUFFER_LEN,
					    &packet, &quality);
	printf("burst=%u raw=%lu clean=%lu status=%s(%d) payload=%u "
	       "src=%u dst=%u hash=%08lx t0=%lu t1=%lu tn=%lu train=%lu "
	       "trq=%lu syncs=%lu mode=%lu bad=%u/%lu badrun=%u lqi=%u "
	       "stage=%lu lenraw=%lu\n",
	       index, (unsigned long)count, (unsigned long)clean_count,
	       status_name(status), (int)status, packet.payload_len, packet.src,
	       packet.dst, (unsigned long)payload_hash(&packet),
	       (unsigned long)openvlc_phy_dbg_sfdsync_cell0,
	       (unsigned long)openvlc_phy_dbg_sfdsync_cell1,
	       (unsigned long)openvlc_phy_dbg_track_nominal_end,
	       (unsigned long)openvlc_phy_dbg_sfdsync_train,
	       (unsigned long)openvlc_phy_dbg_timing_residual_peak,
	       (unsigned long)openvlc_phy_dbg_sfdsync_syncs,
	       (unsigned long)openvlc_phy_dbg_sfdsync_mode,
	       quality.manchester_bad_pairs,
	       (unsigned long)quality.manchester_pairs,
	       quality.manchester_max_bad_run, quality.link_quality,
	       (unsigned long)openvlc_phy_dbg_stage,
	       (unsigned long)openvlc_phy_dbg_len_raw);
}

int main(int argc, char **argv)
{
	FILE *file;
	char line[256];
	double threshold;
	double first_time = 0.0;
	double previous_time = 0.0;
	double previous_value = 0.0;
	uint32_t *edges;
	size_t edge_count = 0u;
	size_t edge_capacity = 32768u;
	uint32_t gap_ticks =
		(uint32_t)(((uint64_t)OPENVLC_CAPTURE_TIMER_HZ *
			    OPENVLC_CAPTURE_GAP_US) / 1000000u);
	unsigned burst = 0u;
	int have_previous = 0;

	if (argc != 3) {
		fprintf(stderr, "usage: %s capture.csv threshold_volts\n", argv[0]);
		return 2;
	}
	threshold = strtod(argv[2], NULL);
	file = fopen(argv[1], "r");
	if (!file) {
		perror(argv[1]);
		return 2;
	}
	edges = malloc(edge_capacity * sizeof(*edges));
	if (!edges) {
		fclose(file);
		return 2;
	}
	(void)fgets(line, sizeof(line), file); /* header */
	while (fgets(line, sizeof(line), file)) {
		double time;
		double value;
		double crossing;
		double fraction;
		uint32_t tick;
		char *comma = strchr(line, ',');

		if (!comma)
			continue;
		*comma = '\0';
		time = strtod(line, NULL);
		value = strtod(comma + 1, NULL);
		if (!have_previous) {
			first_time = time;
			previous_time = time;
			previous_value = value;
			have_previous = 1;
			continue;
		}
		if ((previous_value < threshold && value >= threshold) ||
		    (previous_value >= threshold && value < threshold)) {
			fraction = (threshold - previous_value) /
				   (value - previous_value);
			crossing = previous_time + fraction * (time - previous_time);
			tick = (uint32_t)llround(
				(crossing - first_time) * OPENVLC_CAPTURE_TIMER_HZ);
			if (edge_count && tick - edges[edge_count - 1u] > gap_ticks) {
				decode_burst(edges, edge_count, ++burst);
				edge_count = 0u;
			}
			if (edge_count == edge_capacity) {
				size_t new_capacity = edge_capacity * 2u;
				uint32_t *new_edges = realloc(
					edges, new_capacity * sizeof(*edges));

				if (!new_edges) {
					free(edges);
					fclose(file);
					return 2;
				}
				edges = new_edges;
				edge_capacity = new_capacity;
			}
			edges[edge_count++] = tick;
		}
		previous_time = time;
		previous_value = value;
	}
	if (edge_count)
		decode_burst(edges, edge_count, ++burst);
	printf("threshold=%.4fV bursts=%u\n", threshold, burst);
	free(edges);
	fclose(file);
	return 0;
}
