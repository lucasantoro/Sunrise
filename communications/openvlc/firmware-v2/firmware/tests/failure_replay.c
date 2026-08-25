/*
 * Replay an RXFAIL trace emitted by the STM32 through the exact comparator
 * edge decoder linked into the host test build.
 *
 * The input may be a raw journalctl file: prefixes before RXFAIL_* are ignored.
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openvlc_phy.h"

extern volatile uint32_t openvlc_rx_hypothesis_budget;
extern volatile uint32_t openvlc_phy_dbg_stage;
extern volatile uint32_t openvlc_phy_dbg_len_raw;
extern volatile int32_t openvlc_phy_dbg_parse_status;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_cell0;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_cell1;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_train;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_syncs;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_mode;
extern volatile uint32_t openvlc_phy_dbg_phase_edits;
extern volatile uint32_t openvlc_phy_dbg_list_trials;
extern volatile uint32_t openvlc_phy_dbg_local_trials;
extern volatile uint32_t openvlc_phy_dbg_sfdsync_lock_cell;
extern volatile uint32_t openvlc_phy_dbg_track_nominal_end;
extern volatile uint32_t openvlc_phy_dbg_timing_residual_peak;
#define SEARCH_MAX_CANDIDATES 24u
#ifndef OPENVLC_EDGE_MIN_INTERVAL_TICKS
#define OPENVLC_EDGE_MIN_INTERVAL_TICKS 20u
#endif
#ifndef OPENVLC_EDGE_HARD_GLITCH_TICKS
#define OPENVLC_EDGE_HARD_GLITCH_TICKS 8u
#endif
#ifndef OPENVLC_EDGE_CONTEXT_MARGIN_TICKS
#define OPENVLC_EDGE_CONTEXT_MARGIN_TICKS 2u
#endif
#ifndef OPENVLC_RX_EDGE_FILTER_LEGACY
#define OPENVLC_RX_EDGE_FILTER_LEGACY 1u
#endif
#ifndef OPENVLC_RX_EDGE_FILTER_CONTEXTUAL
#define OPENVLC_RX_EDGE_FILTER_CONTEXTUAL 2u
#endif
#ifndef OPENVLC_RX_EDGE_FILTER_MODE
#define OPENVLC_RX_EDGE_FILTER_MODE OPENVLC_RX_EDGE_FILTER_CONTEXTUAL
#endif

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

static int hex_nibble(int character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	character = tolower((unsigned char)character);
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	return -1;
}

static int parse_hex_word(const char *text, uint16_t *value)
{
	unsigned result = 0u;

	for (unsigned i = 0u; i < 4u; i++) {
		int nibble = hex_nibble((unsigned char)text[i]);

		if (nibble < 0)
			return -1;
		result = (result << 4) | (unsigned)nibble;
	}
	*value = (uint16_t)result;
	return 0;
}

static uint32_t read_be32(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) |
	       ((uint32_t)data[1] << 16) |
	       ((uint32_t)data[2] << 8) |
	       data[3];
}

static uint32_t fnv1a32(const uint8_t *data, size_t length)
{
	uint32_t hash = 2166136261u;

	for (size_t i = 0u; i < length; i++) {
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}

/*
 * Offline model of the timer input-capture stability filter.  A transition
 * reaches the filtered output only after the input has remained at the new
 * level for hold_ticks.  Unlike pair cancellation this handles a whole
 * comparator-chatter train as one settling event.  Captures are already past
 * the deployed TIM2 filter, so this is an A/B model for a stronger hardware
 * aperture, not part of the production decoder.
 */
static size_t edge_filter_stable(uint32_t *edges, size_t edge_count,
				 uint32_t hold_ticks)
{
	size_t out = 0u;
	unsigned input_level = 0u;
	unsigned output_level = 0u;

	if (!edges || edge_count == 0u || hold_ticks == 0u)
		return edge_count;
	for (size_t i = 0u; i < edge_count; i++) {
		uint32_t edge = edges[i];
		uint32_t next = i + 1u < edge_count ? edges[i + 1u] : UINT32_MAX;

		input_level ^= 1u;
		if (input_level != output_level && next - edge >= hold_ticks) {
			edges[out++] = edge + hold_ticks;
			output_level = input_level;
		}
	}
	return out;
}

int main(int argc, char **argv)
{
	FILE *file;
	char line[1024];
	uint16_t *intervals = NULL;
	uint32_t *edges = NULL;
	size_t interval_count = 0u;
	size_t received = 0u;
	size_t csv_capacity = 0u;
	unsigned trace_id = 0u;
	unsigned end_id = 0u;
	unsigned clipped = 0u;
	int begin_seen = 0;
	int end_seen = 0;
	int csv_mode = 0;
	openvlc_runtime_config_t cfg = { 0 };
	openvlc_packet_t packet;
	openvlc_quality_t quality;
	uint16_t scratch[OPENVLC_RX_SAMPLE_BUFFER_LEN];
	openvlc_status_t status;
	unsigned search_depth = 0u;
	uint32_t hypothesis_budget = 0u;
	uint32_t deglitch_ticks = 0u;
	uint32_t removed_edges = 0u;
	int deglitch_option_seen = 0;
	int contextual_option_seen = 0;
	int stable_option_seen = 0;
	uint32_t contextual_candidate = 0u;
	uint32_t contextual_hard = 0u;
	uint32_t contextual_margin = 0u;
	uint32_t stable_ticks = 0u;
	int capture_raw = 0;
	const char *filter_name = "none";

	if (argc < 2 || argc > 6) {
		fprintf(stderr,
			"usage: %s capture [--search[=1|2|3]] [--budget=N] "
			"[--deglitch=N] [--stable=N] [--contextual=C,H,M]\n",
			argv[0]);
		return 2;
	}
	for (int arg = 2; arg < argc; arg++) {
		if (strcmp(argv[arg], "--search") == 0) {
			search_depth = 3u;
		} else if (strncmp(argv[arg], "--search=", 9u) == 0) {
			char *end = NULL;
			unsigned long value = strtoul(argv[arg] + 9u, &end, 10);

			if (!end || *end != '\0' || value < 1u || value > 3u) {
				fprintf(stderr, "invalid search depth: %s\n",
					argv[arg]);
				return 2;
			}
			search_depth = (unsigned)value;
		} else if (strncmp(argv[arg], "--budget=", 9u) == 0) {
			char *end = NULL;
			unsigned long value = strtoul(argv[arg] + 9u, &end, 10);

			if (!end || *end != '\0' || value > UINT32_MAX) {
				fprintf(stderr, "invalid hypothesis budget: %s\n",
					argv[arg]);
				return 2;
			}
			hypothesis_budget = (uint32_t)value;
		} else if (strncmp(argv[arg], "--deglitch=", 11u) == 0) {
			char *end = NULL;
			unsigned long value = strtoul(argv[arg] + 11u, &end, 10);

			if (!end || *end != '\0' || value > UINT16_MAX) {
				fprintf(stderr, "invalid deglitch threshold: %s\n",
					argv[arg]);
				return 2;
			}
			deglitch_ticks = (uint32_t)value;
			deglitch_option_seen = 1;
		} else if (strncmp(argv[arg], "--stable=", 9u) == 0) {
			char *end = NULL;
			unsigned long value = strtoul(argv[arg] + 9u, &end, 10);

			if (!end || *end != '\0' || value == 0u ||
			    value > UINT16_MAX) {
				fprintf(stderr, "invalid stability threshold: %s\n",
					argv[arg]);
				return 2;
			}
			stable_ticks = (uint32_t)value;
			stable_option_seen = 1;
		} else if (strncmp(argv[arg], "--contextual=", 13u) == 0) {
			unsigned candidate;
			unsigned hard;
			unsigned margin;
			char tail;

			if (sscanf(argv[arg] + 13u, "%u,%u,%u%c", &candidate,
				   &hard, &margin, &tail) != 3 || candidate == 0u ||
			    candidate > UINT16_MAX || hard > candidate ||
			    margin > UINT16_MAX) {
				fprintf(stderr, "invalid contextual filter: %s\n",
					argv[arg]);
				return 2;
			}
			contextual_candidate = candidate;
			contextual_hard = hard;
			contextual_margin = margin;
			contextual_option_seen = 1;
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[arg]);
			return 2;
		}
	}
	file = fopen(argv[1], "rb");
	if (!file) {
		perror(argv[1]);
		return 2;
	}
	{
		uint8_t begin[88];
		size_t prefix = fread(begin, 1u, sizeof(begin), file);

		if (prefix >= 4u && memcmp(begin, "OVCT", 4u) == 0) {
			uint8_t *raw;
			uint32_t expected_hash;

			if (prefix != sizeof(begin) ||
			    (begin[4] != 1u && begin[4] != 2u) ||
			    begin[5] != 1u ||
			    (begin[7] & 1u) != 0u) {
				fprintf(stderr, "invalid OVCT capture header\n");
				fclose(file);
				return 2;
			}
			trace_id = read_be32(&begin[8]);
			capture_raw = begin[4] >= 2u && (begin[7] & 2u) != 0u;
			interval_count = read_be32(&begin[16]);
			expected_hash = read_be32(&begin[84]);
			if (interval_count == 0u ||
			    interval_count > SIZE_MAX / sizeof(*intervals)) {
				fprintf(stderr, "invalid OVCT interval count\n");
				fclose(file);
				return 2;
			}
			raw = malloc(interval_count * 2u);
			intervals = malloc(interval_count * sizeof(*intervals));
			if (!raw || !intervals ||
			    fread(raw, 2u, interval_count, file) !=
				    interval_count ||
			    fgetc(file) != EOF) {
				fprintf(stderr, "incomplete OVCT capture\n");
				free(raw);
				free(intervals);
				fclose(file);
				return 2;
			}
			if (fnv1a32(raw, interval_count * 2u) !=
			    expected_hash) {
				fprintf(stderr, "OVCT capture hash mismatch\n");
				free(raw);
				free(intervals);
				fclose(file);
				return 2;
			}
			for (size_t i = 0u; i < interval_count; i++)
				intervals[i] =
					(uint16_t)(((uint16_t)raw[2u * i] << 8) |
						   raw[2u * i + 1u]);
			free(raw);
			received = interval_count;
			begin_seen = 1;
			end_seen = 1;
			fclose(file);
			file = NULL;
		} else {
			rewind(file);
		}
	}
	if (file != NULL) {
		while (fgets(line, sizeof(line), file)) {
		char *marker = strstr(line, "RXFAIL_BEGIN ");

		if (!csv_mode &&
		    strncmp(line, "interval_index,edge_index,timestamp_ticks,"
			    "interval_ticks,", 52u) == 0) {
			csv_mode = 1;
			begin_seen = 1;
			continue;
		}
		if (csv_mode) {
			unsigned index;
			unsigned edge;
			unsigned timestamp;
			unsigned run;

			if (sscanf(line, "%u,%u,%u,%u",
				   &index, &edge, &timestamp, &run) != 4 ||
			    index != received || run > UINT16_MAX) {
				fprintf(stderr,
					"invalid trace CSV row at interval=%lu\n",
					(unsigned long)received);
				fclose(file);
				free(intervals);
				return 2;
			}
			if (received == csv_capacity) {
				size_t next_capacity =
					csv_capacity ? csv_capacity * 2u : 1024u;
				uint16_t *next = realloc(
					intervals,
					next_capacity * sizeof(*intervals));

				if (!next) {
					fclose(file);
					free(intervals);
					return 2;
				}
				intervals = next;
				csv_capacity = next_capacity;
			}
			intervals[received++] = (uint16_t)run;
			(void)edge;
			(void)timestamp;
			continue;
		}
		if (marker) {
			char *runs_field = strstr(marker, "runs=");
			unsigned runs = 0u;

			if (sscanf(marker, "RXFAIL_BEGIN v=1 id=%u",
				   &trace_id) != 1 ||
			    !runs_field ||
			    sscanf(runs_field, "runs=%u", &runs) != 1 ||
			    runs == 0u) {
				fprintf(stderr, "invalid RXFAIL_BEGIN\n");
				fclose(file);
				return 2;
			}
			free(intervals);
			interval_count = runs;
			intervals = calloc(interval_count, sizeof(*intervals));
			if (!intervals) {
				fclose(file);
				return 2;
			}
			received = 0u;
			begin_seen = 1;
			end_seen = 0;
			continue;
		}
		marker = strstr(line, "RXFAIL_DATA ");
		if (marker && begin_seen) {
			char *hex = strstr(marker, "hex=");
			unsigned id = 0u;
			unsigned offset = 0u;
			size_t words;

			if (!hex ||
			    sscanf(marker, "RXFAIL_DATA id=%u off=%u",
				   &id, &offset) != 2 ||
			    id != trace_id || offset != received) {
				fprintf(stderr,
					"non-contiguous RXFAIL_DATA at received=%lu\n",
					(unsigned long)received);
				fclose(file);
				free(intervals);
				return 2;
			}
			hex += 4;
			words = strspn(hex, "0123456789abcdefABCDEF") / 4u;
			if (words == 0u || received + words > interval_count) {
				fprintf(stderr, "invalid RXFAIL_DATA payload\n");
				fclose(file);
				free(intervals);
				return 2;
			}
			for (size_t i = 0u; i < words; i++) {
				if (parse_hex_word(hex + i * 4u,
						   &intervals[received + i]) != 0) {
					fprintf(stderr, "invalid interval hex\n");
					fclose(file);
					free(intervals);
					return 2;
				}
			}
			received += words;
			continue;
		}
		marker = strstr(line, "RXFAIL_END ");
		if (marker && begin_seen) {
			unsigned runs = 0u;

			if (sscanf(marker,
				   "RXFAIL_END id=%u runs=%u clipped=%u",
				   &end_id, &runs, &clipped) != 3 ||
			    end_id != trace_id || runs != interval_count) {
				fprintf(stderr, "invalid RXFAIL_END\n");
				fclose(file);
				free(intervals);
				return 2;
			}
			end_seen = 1;
		}
		}
		fclose(file);
	}
	if (csv_mode) {
		interval_count = received;
		end_seen = received != 0u;
	}
	if (!begin_seen || !end_seen || received != interval_count || clipped) {
		fprintf(stderr,
			"incomplete trace: begin=%d end=%d received=%lu/%lu "
			"clipped=%u\n",
			begin_seen, end_seen, (unsigned long)received,
			(unsigned long)interval_count, clipped);
		free(intervals);
		return 2;
	}

	edges = malloc((interval_count + 1u) * sizeof(*edges));
	if (!edges) {
		free(intervals);
		return 2;
	}
	edges[0] = 0u;
	for (size_t i = 0u; i < interval_count; i++)
		edges[i + 1u] = edges[i] + intervals[i];
	if (stable_option_seen && capture_raw) {
		size_t before = interval_count + 1u;
		size_t filtered_count = edge_filter_stable(
			edges, before, stable_ticks);

		filter_name = "stable";
		deglitch_ticks = stable_ticks;
		removed_edges = (uint32_t)(before - filtered_count);
		if (filtered_count < 2u) {
			fprintf(stderr, "stability filter removed the complete trace\n");
			free(edges);
			free(intervals);
			return 2;
		}
		interval_count = filtered_count - 1u;
		for (size_t i = 0u; i < interval_count; i++)
			intervals[i] =
				(uint16_t)(edges[i + 1u] - edges[i]);
	} else if (contextual_option_seen && capture_raw) {
		deglitch_ticks = contextual_candidate;
		size_t filtered_count = openvlc_edge_filter_timing_aware(
			edges, interval_count + 1u, contextual_candidate,
			contextual_hard, contextual_margin, &removed_edges);

		filter_name = "contextual";
		if (filtered_count < 2u) {
			fprintf(stderr, "contextual filter removed the complete trace\n");
			free(edges);
			free(intervals);
			return 2;
		}
		interval_count = filtered_count - 1u;
		for (size_t i = 0u; i < interval_count; i++)
			intervals[i] =
				(uint16_t)(edges[i + 1u] - edges[i]);
	} else if (!deglitch_option_seen && capture_raw &&
	    OPENVLC_RX_EDGE_FILTER_MODE == OPENVLC_RX_EDGE_FILTER_CONTEXTUAL) {
		deglitch_ticks = OPENVLC_EDGE_MIN_INTERVAL_TICKS;
		size_t filtered_count = openvlc_edge_filter_timing_aware(
			edges, interval_count + 1u,
			OPENVLC_EDGE_MIN_INTERVAL_TICKS,
			OPENVLC_EDGE_HARD_GLITCH_TICKS,
			OPENVLC_EDGE_CONTEXT_MARGIN_TICKS, &removed_edges);

		filter_name = "contextual";
		if (filtered_count < 2u) {
			fprintf(stderr, "contextual filter removed the complete trace\n");
			free(edges);
			free(intervals);
			return 2;
		}
		interval_count = filtered_count - 1u;
		for (size_t i = 0u; i < interval_count; i++)
			intervals[i] =
				(uint16_t)(edges[i + 1u] - edges[i]);
	} else if ((!deglitch_option_seen && capture_raw &&
		    OPENVLC_RX_EDGE_FILTER_MODE == OPENVLC_RX_EDGE_FILTER_LEGACY) ||
		   deglitch_ticks != 0u) {
		size_t before = interval_count + 1u;

		if (!deglitch_option_seen)
			deglitch_ticks = OPENVLC_EDGE_MIN_INTERVAL_TICKS;
		size_t filtered_count = openvlc_edge_cancel_short_pulses(
			edges, before, deglitch_ticks);

		filter_name = "legacy";
		removed_edges = (uint32_t)(before - filtered_count);
		if (filtered_count < 2u) {
			fprintf(stderr, "deglitch removed the complete trace\n");
			free(edges);
			free(intervals);
			return 2;
		}
		interval_count = filtered_count - 1u;
		for (size_t i = 0u; i < interval_count; i++)
			intervals[i] =
				(uint16_t)(edges[i + 1u] - edges[i]);
	}

	memset(&packet, 0, sizeof(packet));
	memset(&quality, 0, sizeof(quality));
	openvlc_rx_hypothesis_budget = hypothesis_budget;
	status = openvlc_rx_edges_to_packet(
		&cfg, edges, interval_count + 1u, scratch,
		OPENVLC_RX_SAMPLE_BUFFER_LEN, &packet, &quality);
	printf("trace=%u edges=%lu domain=%s filter=%s gate=%lu removed=%lu status=%s(%d) parse=%ld payload=%u "
	       "src=%u dst=%u t0=%lu t1=%lu tn=%lu train=%lu trq=%lu "
	       "syncs=%lu mode=%lu edits=%lu local=%lu trials=%lu lock=%lu lenraw=%lu stage=%lu "
	       "bad=%u/%lu badrun=%u lqi=%u\n",
	       trace_id, (unsigned long)(interval_count + 1u),
	       capture_raw ? "raw" : "filtered", filter_name,
	       (unsigned long)deglitch_ticks,
	       (unsigned long)removed_edges,
	       status_name(status), (int)status,
	       (long)openvlc_phy_dbg_parse_status, packet.payload_len,
	       packet.src, packet.dst,
	       (unsigned long)openvlc_phy_dbg_sfdsync_cell0,
	       (unsigned long)openvlc_phy_dbg_sfdsync_cell1,
	       (unsigned long)openvlc_phy_dbg_track_nominal_end,
	       (unsigned long)openvlc_phy_dbg_sfdsync_train,
	       (unsigned long)openvlc_phy_dbg_timing_residual_peak,
	       (unsigned long)openvlc_phy_dbg_sfdsync_syncs,
	       (unsigned long)openvlc_phy_dbg_sfdsync_mode,
	       (unsigned long)openvlc_phy_dbg_phase_edits,
	       (unsigned long)openvlc_phy_dbg_local_trials,
	       (unsigned long)openvlc_phy_dbg_list_trials,
	       (unsigned long)openvlc_phy_dbg_sfdsync_lock_cell,
	       (unsigned long)openvlc_phy_dbg_len_raw,
	       (unsigned long)openvlc_phy_dbg_stage,
	       quality.manchester_bad_pairs,
	       (unsigned long)quality.manchester_pairs,
	       quality.manchester_max_bad_run, quality.link_quality);
	if (status != OPENVLC_OK && search_depth != 0u) {
		size_t candidates[SEARCH_MAX_CANDIDATES];
		size_t candidate_count = 0u;
		uint32_t t0 = openvlc_phy_dbg_sfdsync_cell0;
		uint32_t t1 = openvlc_phy_dbg_sfdsync_cell1;
		uint32_t nominal = openvlc_phy_dbg_track_nominal_end;

		for (size_t i = 0u; i < interval_count &&
		     candidate_count < SEARCH_MAX_CANDIDATES; i++) {
			uint32_t base = (i & 1u) ? t1 : t0;
			uint32_t run = intervals[i];
			uint32_t adjusted = run > nominal / 16u ?
				run - nominal / 16u : 0u;
			uint32_t delta = adjusted > base ? adjusted - base : 0u;
			uint32_t rem = nominal ? delta % nominal : 0u;
			uint32_t margin = rem > nominal / 2u ?
				rem - nominal / 2u : nominal / 2u - rem;
			uint32_t cells = nominal ?
				1u + (delta + nominal / 2u - 1u) / nominal : 1u;

			if (margin <= 4u || cells >= 3u)
				candidates[candidate_count++] = i;
		}
		printf("search candidates=%lu\n", (unsigned long)candidate_count);
		for (size_t a = 0u; a < candidate_count; a++) {
			for (int da = -1; da <= 1; da += 2) {
				memset(&packet, 0, sizeof(packet));
				memset(&quality, 0, sizeof(quality));
				openvlc_test_set_cell_overrides(
					candidates[a], da, SIZE_MAX, 0,
					SIZE_MAX, 0);
				status = openvlc_rx_edges_to_packet(
					&cfg, edges, interval_count + 1u, scratch,
					OPENVLC_RX_SAMPLE_BUFFER_LEN,
					&packet, &quality);
				if (status == OPENVLC_OK) {
					printf("search recovered single=%lu delta=%d "
					       "payload=%u mode=%lu edits=%lu\n",
					       (unsigned long)candidates[a], da,
					       packet.payload_len,
					       (unsigned long)
						       openvlc_phy_dbg_sfdsync_mode,
					       (unsigned long)
						       openvlc_phy_dbg_phase_edits);
					goto search_done;
				}
			}
		}
		if (search_depth == 1u)
			goto search_not_found;
		for (size_t a = 0u; a < candidate_count; a++) {
			for (size_t b = a + 1u; b < candidate_count; b++) {
				for (int da = -1; da <= 1; da += 2) {
					for (int db = -1; db <= 1; db += 2) {
						memset(&packet, 0, sizeof(packet));
						memset(&quality, 0,
						       sizeof(quality));
						openvlc_test_set_cell_overrides(
							candidates[a], da,
							candidates[b], db,
							SIZE_MAX, 0);
						status =
							openvlc_rx_edges_to_packet(
								&cfg, edges,
								interval_count + 1u,
								scratch,
								OPENVLC_RX_SAMPLE_BUFFER_LEN,
								&packet,
								&quality);
						if (status == OPENVLC_OK) {
							printf(
								"search recovered pair=%lu:%d,%lu:%d "
								"payload=%u mode=%lu edits=%lu\n",
								(unsigned long)candidates[a],
								da,
								(unsigned long)candidates[b],
								db,
								packet.payload_len,
								(unsigned long)
									openvlc_phy_dbg_sfdsync_mode,
								(unsigned long)
									openvlc_phy_dbg_phase_edits);
							goto search_done;
						}
					}
				}
			}
		}
		if (search_depth == 2u)
			goto search_not_found;
		for (size_t a = 0u; a < candidate_count; a++) {
			for (size_t b = a + 1u; b < candidate_count; b++) {
				for (size_t c = b + 1u; c < candidate_count; c++) {
					for (int da = -1; da <= 1; da += 2) {
						for (int db = -1; db <= 1; db += 2) {
							for (int dc = -1; dc <= 1; dc += 2) {
								memset(&packet, 0, sizeof(packet));
								memset(&quality, 0, sizeof(quality));
								openvlc_test_set_cell_overrides(
									candidates[a], da,
									candidates[b], db,
									candidates[c], dc);
								status = openvlc_rx_edges_to_packet(
									&cfg, edges,
									interval_count + 1u,
									scratch,
									OPENVLC_RX_SAMPLE_BUFFER_LEN,
									&packet, &quality);
								if (status == OPENVLC_OK) {
									printf(
										"search recovered triple=%lu:%d,%lu:%d,%lu:%d "
										"payload=%u mode=%lu edits=%lu\n",
										(unsigned long)candidates[a], da,
										(unsigned long)candidates[b], db,
										(unsigned long)candidates[c], dc,
										packet.payload_len,
										(unsigned long)
											openvlc_phy_dbg_sfdsync_mode,
										(unsigned long)
											openvlc_phy_dbg_phase_edits);
									goto search_done;
								}
							}
						}
					}
				}
			}
		}
	search_not_found:
		printf("search found no recovery up to %u interval edit%s\n",
		       search_depth, search_depth == 1u ? "" : "s");
search_done:
		openvlc_test_set_cell_overrides(
			SIZE_MAX, 0, SIZE_MAX, 0, SIZE_MAX, 0);
	}

	free(edges);
	free(intervals);
	return status == OPENVLC_OK ? 0 : 1;
}
