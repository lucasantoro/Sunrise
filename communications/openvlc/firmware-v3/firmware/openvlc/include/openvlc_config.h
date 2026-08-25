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
 * 2026-08-19, measured by replaying 64 real captures (openvlc-rx-captures,
 * node-b): the 16-cell SFD is TOO SHORT for this channel. Correlated against
 * the ~800 cells of comparator chatter that precede every frame when the LED
 * is dark between bursts, it produces 0.38 FALSE LOCKS PER FRAME - and that
 * number does not move with tolerance (2 -> 0.43, 1 -> 0.38, 0 -> 0.38),
 * because the junk contains EXACT copies of any given 16-cell pattern. 56 of
 * the 64 captures failed with a false lock ahead of a true SFD that was
 * present with ZERO cell errors in every single one.
 *
 * Doubling to 32 cells takes the false-lock rate to 0.00 over the same corpus
 * (~45000 offsets, no match). The pattern itself barely matters at that
 * length, but 0x99 0x4B was picked by exhaustive search over all 2^16
 * Manchester-reachable words: peak aperiodic autocorrelation sidelobe 11
 * instead of 16, and it still shows zero false locks at tolerance 8/32 where
 * naive choices break down at 6.
 *
 * Note a raw m-sequence (or any CAZAC/Zadoff-Chu construction) is NOT usable
 * here: the interval classifier accepts runs of 1 or 2 cells only, and the
 * receiver is a 1-bit comparator with no amplitude. That confines the choice
 * to Manchester-reachable patterns.
 *
 * OPENVLC_SFD_BYTES is the revert switch. It changes the ON-AIR FORMAT, so
 * both ends must run the same value.
 */
/*
 * 2026-08-20 MEASURED on the host harness (build-host vs build-host-sfd1):
 * host_loopback_fast PASSES with OPENVLC_SFD_BYTES=1 and FAILS with 2, on the
 * synthetic mixed case id=54 (ppm=-11872, dcd=8, jitter=2, period=14). The
 * decode fails, not the deglitch filter (removed=462).
 *
 * Mechanism is arithmetic, not speculation: the correlator matches a fixed
 * pattern across N cells, so accumulated clock drift over the window grows
 * linearly with N. At -1.19% offset that is 0.19 cells across 16 and 0.38
 * across 32. Doubling the SFD doubles the drift it must absorb - a longer SFD
 * is NOT strictly more robust.
 *
 * Not urgent for the deployed link: real offset between two H723 crystals is
 * tens of ppm, not 11872, and the 32-cell SFD measured crc=0/sync=0 over 8492
 * frames on hardware. But the margin against frequency error is now smaller,
 * so revisit this if the link ever runs across different clock sources or wide
 * temperature. Reverting is one value: OPENVLC_SFD_BYTES=1.
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
 * 2026-08-07: 16 -> 8. This is a CHANNEL-OCCUPANCY change, not a robustness
 * one. At 828 B payload the frame needs 5 RS blocks, so 16 ECC bytes cost
 * 80 B = 1280 Manchester cells = 640 us of a 7576 us frame that has to fit in
 * an 8000 us period - 95% occupancy, which is why host-pacing jitter kept
 * costing whole frames (ovf/sync/ring drops) all through this session. The
 * reference LiFi_Manchester link runs 135 fps, i.e. a 7407 us period SHORTER
 * than our frame alone, and carries no FEC at all.
 * 8 ECC bytes still correct 4 byte-errors per 200 B block, and the measured
 * channel does not need more: crc has been 0 over tens of thousands of frames.
 * Saves 320 us/frame, taking occupancy 95% -> ~91% and the inter-frame gap
 * 424 -> ~744 us. Both nodes MUST run the same value - it is a wire format.
 * VERIFY with the CUMULATIVE `total=` field on both bridges, never the
 * instantaneous fps: TX was 625.2 frames per 5 s interval (125.04 fps) and RX
 * 615.0 (123.0 fps) before this change.
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
