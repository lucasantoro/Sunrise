#include <stdint.h>
#include <stdio.h>

#include "openvlc_stm32_tx_hal.h"

#define TEST_SYMBOL_CAP \
	(OPENVLC_MAX_SYMBOLS + OPENVLC_STM32_TX_WARMUP_CELLS + \
	 OPENVLC_STM32_TX_GAP_CELLS + 2u)

static int test_direct_oc_encoding(void)
{
	static const uint16_t lengths[] = { 0u, 1u, 193u, 194u, 195u,
					     899u, 900u };
	static bool symbols[TEST_SYMBOL_CAP];
	static uint16_t words[TEST_SYMBOL_CAP];
	openvlc_tx_symbol_buffer_t symbol_out = {
		.symbols = symbols,
		.symbol_cap = TEST_SYMBOL_CAP,
		.symbol_len = 0u,
	};
	openvlc_packet_t packet = {0};
	const openvlc_tx_profile_t *profile = openvlc_tx_default_profile();
	size_t word_len = 0u;
	size_t expected_len;

	packet.dst = 8u;
	packet.src = 7u;
	packet.protocol = OPENVLC_PROTOCOL_DEFAULT;
	for (size_t test = 0u; test < sizeof(lengths) / sizeof(lengths[0]);
	     test++) {
		packet.payload_len = lengths[test];
		for (size_t i = 0u; i < packet.payload_len; i++)
			packet.payload[i] = (uint8_t)(i * 37u + 11u);
		symbol_out.symbol_len = 0u;
		word_len = 0u;
		if (openvlc_tx_compat_packet_to_symbols(&packet, &symbol_out) !=
		    OPENVLC_OK)
			return 1;
		if (openvlc_tx_compat_packet_to_oc_words(
			    &packet, profile, words, TEST_SYMBOL_CAP, &word_len,
			    NULL) !=
		    OPENVLC_OK)
			return 1;
		expected_len = symbol_out.symbol_len + profile->gap_cells + 1u;
		if (word_len != expected_len)
			return 1;
		for (size_t i = 0u; i < symbol_out.symbol_len; i++) {
			uint16_t expected = symbols[i] ?
				(uint16_t)profile->cell_ticks : 0u;

			if (words[i] != expected)
				return 1;
		}
		for (size_t i = symbol_out.symbol_len; i < word_len; i++) {
			if (words[i] != 0u)
				return 1;
		}
	}
	return 0;
}

static int test_tx_profiles(void)
{
	const openvlc_tx_profile_t *budget40 =
		openvlc_tx_profile_for_budget(40u);
	const openvlc_tx_profile_t *budget50 =
		openvlc_tx_profile_for_budget(50u);
	const openvlc_tx_profile_t *budget100 =
		openvlc_tx_profile_for_budget(100u);

	if (!budget40 || !budget50 || !budget100) {
		fputs("one or more TX profiles are missing\n", stderr);
		return 1;
	}
	if (budget40->phy_rate_kbps != 1250u ||
	    budget40->cell_ticks != OPENVLC_STM32_TX_BUDGET40_CELL_TICKS ||
	    budget50->phy_rate_kbps != 1000u ||
	    budget50->cell_ticks != OPENVLC_STM32_TX_BUDGET50_CELL_TICKS ||
	    budget100->phy_rate_kbps != 500u ||
	    budget100->cell_ticks != OPENVLC_STM32_TX_BUDGET100_CELL_TICKS ||
	    budget100->cell_rate_hz !=
		    OPENVLC_STM32_TX_TIMER_HZ /
			    OPENVLC_STM32_TX_BUDGET100_CELL_TICKS) {
		fputs("TX profile parameters invalid\n", stderr);
		return 1;
	}
	if (openvlc_tx_default_profile() != budget50 ||
	    budget50->warmup_cells != OPENVLC_STM32_TX_WARMUP_CELLS ||
	    budget50->gap_cells != OPENVLC_STM32_TX_GAP_CELLS) {
		fputs("production TX profile selection invalid\n", stderr);
		return 1;
	}
	return 0;
}

static int expect_slot(const char *name, const uint32_t *orders,
		       size_t count, uint32_t newest, int expected)
{
	int actual = openvlc_test_tx_select_oldest_ready(orders, count, newest);

	if (actual == expected)
		return 0;
	fprintf(stderr, "%s: expected slot %d, got %d\n",
		name, expected, actual);
	return 1;
}

int main(void)
{
	const uint32_t reused_low_slot[] = { 5u, 2u, 3u };
	const uint32_t oldest_high_slot[] = { 8u, 9u, 7u };
	const uint32_t wrapped_order[] = { UINT32_MAX, 0u, 1u };
	const uint32_t too_many_slots[] = { 1u, 2u, 3u, 4u };
	int failed = 0;

	failed += expect_slot("reused low slot", reused_low_slot, 3u, 5u, 1);
	failed += expect_slot("oldest high slot", oldest_high_slot, 3u, 10u, 2);
	failed += expect_slot("uint32 wrap", wrapped_order, 3u, 1u, 0);
	failed += expect_slot("invalid count", too_many_slots, 4u, 4u, -1);
	if (test_direct_oc_encoding() != 0) {
		fputs("direct OC encoding differs from compatibility path\n", stderr);
		failed++;
	}
	failed += test_tx_profiles();

	if (failed)
		return 1;
	puts("TX scheduler FIFO tests passed");
	return 0;
}
