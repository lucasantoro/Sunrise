#ifndef OPENVLC_CONFIG_H
#define OPENVLC_CONFIG_H

#include <stdint.h>

#if defined(__has_include)
#if __has_include("openvlc_board.h")
#include "openvlc_board.h"
#endif
#endif

#ifndef OPENVLC_SAMPLES_PER_SYMBOL
#define OPENVLC_SAMPLES_PER_SYMBOL          4u
#endif

#define OPENVLC_LINE_MANCHESTER             1u
#define OPENVLC_LINE_4B5B                   2u

#ifndef OPENVLC_LINE_CODE
#define OPENVLC_LINE_CODE                   OPENVLC_LINE_MANCHESTER
#endif

#ifndef OPENVLC_PREAMBLE_BYTES
#define OPENVLC_PREAMBLE_BYTES              8u
#endif

#define OPENVLC_PREAMBLE_BITS               (OPENVLC_PREAMBLE_BYTES * 8u)
#define OPENVLC_PREAMBLE_BYTE               0xaau
/*
 * Start-of-frame delimiter.
 *
 * 32 cells, not 16. A 16-cell delimiter is too short for this channel: the
 * comparator chatter that precedes every frame while the LED is dark contains
 * exact copies of any given 16-cell pattern, so correlation locks onto junk
 * ahead of the real SFD. Length is what fixes that, not tolerance.
 *
 * Within 32 cells the pattern barely matters, but 0x99 0x4B has a peak aperiodic
 * autocorrelation sidelobe of 11 rather than 16, which keeps the match sharp at
 * loose tolerances.
 *
 * A raw m-sequence or a CAZAC construction is not usable here: the interval
 * classifier accepts runs of one or two cells only, and the receiver is a 1-bit
 * comparator with no amplitude. The choice is confined to Manchester-reachable
 * patterns.
 *
 * OPENVLC_SFD_BYTES changes the ON-AIR FORMAT. Both ends must run the same value.
 */
#ifndef OPENVLC_SFD_BYTES
#define OPENVLC_SFD_BYTES                   2u
#endif

#if OPENVLC_SFD_BYTES == 2u
#define OPENVLC_SFD_BYTE                    0x99u
#define OPENVLC_SFD_BYTE2                   0x4bu
#define OPENVLC_SFD_WORD                    (((OPENVLC_SFD_BYTE) << 8) | (OPENVLC_SFD_BYTE2))
#elif OPENVLC_SFD_BYTES == 1u
#define OPENVLC_SFD_BYTE                    0xa3u
#define OPENVLC_SFD_WORD                    (OPENVLC_SFD_BYTE)
#else
#error "OPENVLC_SFD_BYTES must be 1 or 2"
#endif

/* Manchester cells occupied by the SFD; also the correlator window width. */
#define OPENVLC_SFD_CELLS                   (OPENVLC_SFD_BYTES * 16u)
#define OPENVLC_SFD_BITS                    (OPENVLC_SFD_BYTES * 8u)
/* Window mask for the cell-domain correlator (32 cells = full uint32). */
#define OPENVLC_SFD_MASK ((OPENVLC_SFD_CELLS >= 32u) ? 0xffffffffu : ((1u << (OPENVLC_SFD_CELLS & 31u)) - 1u))
/* Bit `i` of the SFD, MSB-first index (i = OPENVLC_SFD_BITS-1 .. 0). */
#define OPENVLC_SFD_BIT(i)                  ((uint8_t)(((uint32_t)OPENVLC_SFD_WORD >> (i)) & 1u))
#if OPENVLC_SFD_BYTES == 2u
#define OPENVLC_SFD_EMIT(buf, idx) do { (buf)[(idx)++] = OPENVLC_SFD_BYTE; (buf)[(idx)++] = OPENVLC_SFD_BYTE2; } while (0)
#define OPENVLC_SFD_MATCH(p)                ((p)[0] == OPENVLC_SFD_BYTE && (p)[1] == OPENVLC_SFD_BYTE2)
#else
#define OPENVLC_SFD_EMIT(buf, idx)          do { (buf)[(idx)++] = OPENVLC_SFD_BYTE; } while (0)
#define OPENVLC_SFD_MATCH(p)                ((p)[0] == OPENVLC_SFD_BYTE)
#endif

#ifndef OPENVLC_SFD_SYNC_PREAMBLE_CELLS
#define OPENVLC_SFD_SYNC_PREAMBLE_CELLS     32u
#endif

#if OPENVLC_SFD_SYNC_PREAMBLE_CELLS > 48u
#error "OPENVLC_SFD_SYNC_PREAMBLE_CELLS must be <= 48"
#endif

#if OPENVLC_SFD_SYNC_PREAMBLE_CELLS > OPENVLC_PREAMBLE_BITS
#error "SFD preamble gate cannot exceed the configured raw preamble"
#endif

#define OPENVLC_ADDR_SELF_DEFAULT           8u
#define OPENVLC_ADDR_PEER_DEFAULT           7u
#define OPENVLC_PROTOCOL_DEFAULT            0x0001u

#ifndef OPENVLC_BEAGLEBONE_COMPAT
#define OPENVLC_BEAGLEBONE_COMPAT           1
#endif

#ifndef OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE
#define OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE    200u
#endif

/*
 * Reed-Solomon parity per block, in bytes. Corrects 4 byte-errors per 200-byte
 * block.
 *
 * This is a channel-occupancy decision as much as a robustness one. An 828-byte
 * payload needs 5 blocks, so each parity byte costs 5 bytes on air: 8 parity is
 * 40 bytes, 640 Manchester cells, 320 us of a frame that has to fit inside an
 * 8000 us period. Airtime is the scarce resource here, not error correction -
 * see docs/wire_format.md for the budget.
 *
 * Both nodes MUST run the same value: it is part of the wire format, not a local
 * robustness setting.
 */
#ifndef OPENVLC_BEAGLEBONE_RS_ECC_BYTES
#define OPENVLC_BEAGLEBONE_RS_ECC_BYTES     8u
#endif

#if OPENVLC_BEAGLEBONE_RS_ECC_BYTES == 0u
#error "OPENVLC_BEAGLEBONE_RS_ECC_BYTES must be > 0"
#endif

#if (OPENVLC_BEAGLEBONE_RS_ECC_BYTES & 1u) != 0u
#error "OPENVLC_BEAGLEBONE_RS_ECC_BYTES must be even"
#endif

#if OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE + OPENVLC_BEAGLEBONE_RS_ECC_BYTES > 255u
#error "RS block size + ECC bytes must fit GF(256) codeword length"
#endif

#ifndef OPENVLC_MAX_PAYLOAD_BYTES
#define OPENVLC_MAX_PAYLOAD_BYTES           800u
#endif

#define OPENVLC_HEADER_BYTES                6u
#define OPENVLC_CRC_BYTES                   2u
#define OPENVLC_BEAGLEBONE_ENCODED_BYTES(payload_bytes) \
	((payload_bytes) + 2u * 2u + 2u)
#define OPENVLC_BEAGLEBONE_RS_BLOCKS(payload_bytes) \
	((OPENVLC_BEAGLEBONE_ENCODED_BYTES(payload_bytes) + \
	  OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE - 1u) / OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE)
#define OPENVLC_BEAGLEBONE_FRAME_BYTES(payload_bytes) \
	(OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES + 2u + OPENVLC_HEADER_BYTES + \
	 (payload_bytes) + \
	 OPENVLC_BEAGLEBONE_RS_ECC_BYTES * OPENVLC_BEAGLEBONE_RS_BLOCKS(payload_bytes))
#define OPENVLC_BEAGLEBONE_SYMBOLS(payload_bytes) \
	(OPENVLC_PREAMBLE_BITS + \
	 ((OPENVLC_BEAGLEBONE_FRAME_BYTES(payload_bytes) - OPENVLC_PREAMBLE_BYTES) * 16u) + \
	 1u)
#define OPENVLC_NATIVE_MAX_FRAME_BYTES \
	(OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES + OPENVLC_HEADER_BYTES + \
	 OPENVLC_MAX_PAYLOAD_BYTES + OPENVLC_CRC_BYTES)
#if OPENVLC_BEAGLEBONE_COMPAT
#define OPENVLC_MAX_FRAME_BYTES \
	((OPENVLC_NATIVE_MAX_FRAME_BYTES > OPENVLC_BEAGLEBONE_FRAME_BYTES(OPENVLC_MAX_PAYLOAD_BYTES)) ? \
	 OPENVLC_NATIVE_MAX_FRAME_BYTES : OPENVLC_BEAGLEBONE_FRAME_BYTES(OPENVLC_MAX_PAYLOAD_BYTES))
#else
#define OPENVLC_MAX_FRAME_BYTES OPENVLC_NATIVE_MAX_FRAME_BYTES
#endif

#define OPENVLC_MAX_SYMBOLS_MANCHESTER \
	(OPENVLC_PREAMBLE_BITS + \
	 ((1u + OPENVLC_HEADER_BYTES + OPENVLC_MAX_PAYLOAD_BYTES + OPENVLC_CRC_BYTES) * 16u))

#define OPENVLC_MAX_SYMBOLS_4B5B \
	(OPENVLC_PREAMBLE_BITS + \
	 ((1u + OPENVLC_HEADER_BYTES + OPENVLC_MAX_PAYLOAD_BYTES + OPENVLC_CRC_BYTES) * 10u))

#if OPENVLC_BEAGLEBONE_COMPAT
#define OPENVLC_NATIVE_MAX_SYMBOLS \
	((OPENVLC_MAX_SYMBOLS_MANCHESTER > OPENVLC_MAX_SYMBOLS_4B5B) ? \
	 OPENVLC_MAX_SYMBOLS_MANCHESTER : OPENVLC_MAX_SYMBOLS_4B5B)
#define OPENVLC_MAX_SYMBOLS \
	((OPENVLC_NATIVE_MAX_SYMBOLS > OPENVLC_BEAGLEBONE_SYMBOLS(OPENVLC_MAX_PAYLOAD_BYTES)) ? \
	 OPENVLC_NATIVE_MAX_SYMBOLS : OPENVLC_BEAGLEBONE_SYMBOLS(OPENVLC_MAX_PAYLOAD_BYTES))
#else
#define OPENVLC_MAX_SYMBOLS \
	((OPENVLC_MAX_SYMBOLS_MANCHESTER > OPENVLC_MAX_SYMBOLS_4B5B) ? \
	 OPENVLC_MAX_SYMBOLS_MANCHESTER : OPENVLC_MAX_SYMBOLS_4B5B)
#endif

#ifndef OPENVLC_RX_SAMPLE_BUFFER_LEN
/*
 * COMP/TIM2 RX decodes edge timestamps directly. The edge path passes this
 * buffer only to satisfy the shared app API, and openvlc_rx_edges_to_packet()
 * ignores it. Keep it tiny so RAM_D1 can hold packet-sized edge bursts.
 */
#define OPENVLC_RX_SAMPLE_BUFFER_LEN        16u
#endif

#ifndef OPENVLC_MF_SCORE_MIN
#define OPENVLC_MF_SCORE_MIN                500
#endif

#ifndef OPENVLC_SNR_MIN_DB_CENTI
#define OPENVLC_SNR_MIN_DB_CENTI            (-100000)
#endif

#endif
