#ifndef OPENVLC_LINECODE_H
#define OPENVLC_LINECODE_H

#include "openvlc_types.h"

size_t openvlc_symbols_per_byte(uint8_t line_code);
openvlc_status_t openvlc_encode_symbols(const uint8_t *frame, size_t frame_len,
					 uint8_t line_code, bool *symbols,
					 size_t symbol_cap, size_t *symbol_len);
openvlc_status_t openvlc_decode_symbols(const bool *symbols, size_t symbol_len,
					 uint8_t line_code, uint8_t *frame,
					 size_t frame_cap, size_t *frame_len);

#endif

