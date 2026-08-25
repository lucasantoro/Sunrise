#include "openvlc_linecode.h"

static const uint8_t code_4b5b[16] = {
	0x1e, 0x09, 0x14, 0x15, 0x0a, 0x0b, 0x0e, 0x0f,
	0x12, 0x13, 0x16, 0x17, 0x1a, 0x1b, 0x1c, 0x1d,
};

size_t openvlc_symbols_per_byte(uint8_t line_code)
{
	if (line_code == OPENVLC_LINE_MANCHESTER)
		return 16u;
	if (line_code == OPENVLC_LINE_4B5B)
		return 10u;
	return 0;
}

static int decode_4b5b(uint8_t code)
{
	for (int i = 0; i < 16; i++) {
		if (code_4b5b[i] == code)
			return i;
	}
	return -1;
}

openvlc_status_t openvlc_encode_symbols(const uint8_t *frame, size_t frame_len,
					 uint8_t line_code, bool *symbols,
					 size_t symbol_cap, size_t *symbol_len)
{
	size_t out = 0;

	if (!frame || !symbols || !symbol_len)
		return OPENVLC_ERR_ARG;

	for (size_t i = 0; i < frame_len; i++) {
		uint8_t byte = frame[i];

		if (i < OPENVLC_PREAMBLE_BYTES) {
			for (int bit = 7; bit >= 0; bit--) {
				if (out >= symbol_cap)
					return OPENVLC_ERR_OVERFLOW;
				symbols[out++] = ((byte >> bit) & 1u) != 0u;
			}
		} else if (line_code == OPENVLC_LINE_MANCHESTER) {
			for (int bit = 7; bit >= 0; bit--) {
				bool value = ((byte >> bit) & 1u) != 0u;
				if (out + 2u > symbol_cap)
					return OPENVLC_ERR_OVERFLOW;
				/* BeagleBone OpenVLC convention: 1 = LOW-HIGH, 0 = HIGH-LOW. */
				symbols[out++] = !value;
				symbols[out++] = value;
			}
		} else if (line_code == OPENVLC_LINE_4B5B) {
			uint8_t nibbles[2] = { code_4b5b[byte >> 4], code_4b5b[byte & 0x0f] };
			for (int n = 0; n < 2; n++) {
				for (int bit = 4; bit >= 0; bit--) {
					if (out >= symbol_cap)
						return OPENVLC_ERR_OVERFLOW;
					symbols[out++] = ((nibbles[n] >> bit) & 1u) != 0u;
				}
			}
		} else {
			return OPENVLC_ERR_LINE_CODE;
		}
	}

	*symbol_len = out;
	return OPENVLC_OK;
}

openvlc_status_t openvlc_decode_symbols(const bool *symbols, size_t symbol_len,
					 uint8_t line_code, uint8_t *frame,
					 size_t frame_cap, size_t *frame_len)
{
	size_t in = 0;
	size_t out = 0;
	size_t spb = openvlc_symbols_per_byte(line_code);

	if (!symbols || !frame || !frame_len || !spb)
		return OPENVLC_ERR_ARG;
	if (symbol_len < OPENVLC_PREAMBLE_BITS ||
	    ((symbol_len - OPENVLC_PREAMBLE_BITS) % spb))
		return OPENVLC_ERR_SYNC;

	for (size_t i = 0; i < OPENVLC_PREAMBLE_BYTES; i++) {
		uint8_t byte = 0;
		for (int bit = 7; bit >= 0; bit--, in++)
			byte |= symbols[in] ? (uint8_t)(1u << bit) : 0u;
		frame[out++] = byte;
	}

	while (in < symbol_len) {
		uint8_t byte = 0;

		if (out >= frame_cap)
			return OPENVLC_ERR_OVERFLOW;
		if (line_code == OPENVLC_LINE_MANCHESTER) {
			for (int bit = 7; bit >= 0; bit--) {
				bool a = symbols[in++];
				bool b = symbols[in++];
				if (a == b)
					return OPENVLC_ERR_LINE_CODE;
				/* BeagleBone OpenVLC convention: 1 = LOW-HIGH, 0 = HIGH-LOW. */
				if (b)
					byte |= (uint8_t)(1u << bit);
			}
		} else {
			uint8_t hi = 0;
			uint8_t lo = 0;
			int nibble;
			for (int bit = 4; bit >= 0; bit--, in++)
				hi |= symbols[in] ? (uint8_t)(1u << bit) : 0u;
			for (int bit = 4; bit >= 0; bit--, in++)
				lo |= symbols[in] ? (uint8_t)(1u << bit) : 0u;
			nibble = decode_4b5b(hi);
			if (nibble < 0)
				return OPENVLC_ERR_LINE_CODE;
			byte = (uint8_t)nibble << 4;
			nibble = decode_4b5b(lo);
			if (nibble < 0)
				return OPENVLC_ERR_LINE_CODE;
			byte |= (uint8_t)nibble;
		}
		frame[out++] = byte;
	}

	*frame_len = out;
	return OPENVLC_OK;
}
