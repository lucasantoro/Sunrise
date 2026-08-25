#include "openvlc_frame.h"

#include <string.h>

#include "openvlc_crc.h"

#if defined(__GNUC__) && (defined(STM32H743xx) || defined(STM32H723xx))
#define OPENVLC_FEC_HOT __attribute__((hot, optimize("O3")))
#else
#define OPENVLC_FEC_HOT
#endif

static uint16_t rs_alpha_to[256];
static uint16_t rs_index_of[256];
static uint16_t rs_genpoly[OPENVLC_BEAGLEBONE_RS_ECC_BYTES + 1u];
/*
 * One RS syndrome byte applies the same GF(256) transition for every received
 * byte: next = byte ^ alpha(log(previous) + root). Precompute that transition
 * once. The live clean-frame path then performs one indexed byte load instead
 * of log lookup + modulo reduction + alpha lookup for every root and byte.
 */
static uint8_t
	rs_syndrome_step[OPENVLC_BEAGLEBONE_RS_ECC_BYTES][256];
static bool rs_ready;
static int rs_last_corrected;
static bool rs_last_checked;

static int rs_modnn(int x)
{
	while (x >= 255) {
		x -= 255;
		x = (x >> 8) + (x & 255);
	}
	return x;
}

static void rs_init(void)
{
	uint16_t sr;

	if (rs_ready)
		return;

	rs_index_of[0] = 255;
	rs_alpha_to[255] = 0;
	sr = 1;
	for (int i = 0; i < 255; i++) {
		rs_index_of[sr] = (uint16_t)i;
		rs_alpha_to[i] = sr;
		sr <<= 1;
		if (sr & 0x100u)
			sr ^= 0x11du;
		sr &= 0xffu;
	}

	rs_genpoly[0] = 1;
	for (int i = 0, root = 0; i < (int)OPENVLC_BEAGLEBONE_RS_ECC_BYTES;
	     i++, root++) {
		rs_genpoly[i + 1] = 1;
		for (int j = i; j > 0; j--) {
			if (rs_genpoly[j] != 0) {
				rs_genpoly[j] =
					rs_genpoly[j - 1] ^
					rs_alpha_to[rs_modnn((int)rs_index_of[rs_genpoly[j]] +
							      root)];
			} else {
				rs_genpoly[j] = rs_genpoly[j - 1];
			}
		}
		rs_genpoly[0] = rs_alpha_to[rs_modnn((int)rs_index_of[rs_genpoly[0]] +
						     root)];
	}
	for (int i = 0; i <= (int)OPENVLC_BEAGLEBONE_RS_ECC_BYTES; i++)
		rs_genpoly[i] = rs_index_of[rs_genpoly[i]];
	for (int root = 0;
	     root < (int)OPENVLC_BEAGLEBONE_RS_ECC_BYTES; root++) {
		rs_syndrome_step[root][0] = 0u;
		for (int value = 1; value < 256; value++) {
			rs_syndrome_step[root][value] =
				(uint8_t)rs_alpha_to[rs_modnn(
					(int)rs_index_of[value] + root)];
		}
	}

	rs_ready = true;
}

void openvlc_frame_init(void)
{
	/*
	 * Build GF and syndrome-transition tables before comparator capture
	 * starts. Lazy initialization in the first valid RX frame creates a
	 * one-off deadline miss and can seed an avoidable DMA-ring backlog.
	 */
	rs_init();
}

static OPENVLC_FEC_HOT bool rs_compute_syndromes(
	const uint8_t *data, size_t len,
	const uint8_t *parity, uint16_t *syn)
{
	enum { nroots = OPENVLC_BEAGLEBONE_RS_ECC_BYTES, fcr = 0, prim = 1 };
	uint16_t syn_error = 0u;

	for (int i = 0; i < nroots; i++)
		syn[i] = data[0];
	for (size_t j = 1u; j < len; j++) {
		for (int i = 0; i < nroots; i++)
			syn[i] = data[j] ^
				rs_syndrome_step[(fcr + i) * prim][syn[i]];
	}
	for (int j = 0; j < nroots; j++) {
		for (int i = 0; i < nroots; i++)
			syn[i] = parity[j] ^
				rs_syndrome_step[(fcr + i) * prim][syn[i]];
	}
	for (int i = 0; i < nroots; i++)
		syn_error |= syn[i];
	return syn_error != 0u;
}

static OPENVLC_FEC_HOT int rs_decode_block(
	uint8_t *data, size_t len, uint8_t *parity)
{
	enum { nn = 255, nroots = OPENVLC_BEAGLEBONE_RS_ECC_BYTES, fcr = 0, prim = 1, iprim = 1 };
	int deg_lambda, el, deg_omega;
	int i, j, r, k, pad;
	uint16_t q, tmp, num1, num2, den, discr_r, syn_error;
	uint16_t lambda[nroots + 1], syn[nroots];
	uint16_t b[nroots + 1], t[nroots + 1], omega[nroots + 1];
	uint16_t root[nroots], reg[nroots + 1], loc[nroots];
	int count = 0;

	if (!data || !parity || len == 0u || len > 255u - nroots)
		return -1;
	rs_init();
	pad = nn - nroots - (int)len;
	if (pad < 0 || pad >= nn)
		return -1;

	syn_error = rs_compute_syndromes(data, len, parity, syn) ? 1u : 0u;
	for (i = 0; i < nroots; i++) {
		syn[i] = rs_index_of[syn[i]];
	}
	if (!syn_error)
		return 0;

	memset(&lambda[1], 0, nroots * sizeof(lambda[0]));
	lambda[0] = 1;
	for (i = 0; i < nroots + 1; i++)
		b[i] = rs_index_of[lambda[i]];

	r = 0;
	el = 0;
	while (++r <= nroots) {
		discr_r = 0;
		for (i = 0; i < r; i++) {
			if (lambda[i] != 0 && syn[r - i - 1] != nn) {
				discr_r ^= rs_alpha_to[rs_modnn((int)rs_index_of[lambda[i]] +
								(int)syn[r - i - 1])];
			}
		}
		discr_r = rs_index_of[discr_r];
		if (discr_r == nn) {
			memmove(&b[1], b, nroots * sizeof(b[0]));
			b[0] = nn;
		} else {
			t[0] = lambda[0];
			for (i = 0; i < nroots; i++) {
				if (b[i] != nn) {
					t[i + 1] = lambda[i + 1] ^
						rs_alpha_to[rs_modnn((int)discr_r +
								    (int)b[i])];
				} else {
					t[i + 1] = lambda[i + 1];
				}
			}
			if (2 * el <= r - 1) {
				el = r - el;
				for (i = 0; i <= nroots; i++) {
					b[i] = lambda[i] == 0 ? nn :
						(uint16_t)rs_modnn((int)rs_index_of[lambda[i]] -
								   (int)discr_r + nn);
				}
			} else {
				memmove(&b[1], b, nroots * sizeof(b[0]));
				b[0] = nn;
			}
			memcpy(lambda, t, (nroots + 1) * sizeof(t[0]));
		}
	}

	deg_lambda = 0;
	for (i = 0; i < nroots + 1; i++) {
		lambda[i] = rs_index_of[lambda[i]];
		if (lambda[i] != nn)
			deg_lambda = i;
	}

	memcpy(&reg[1], &lambda[1], nroots * sizeof(reg[0]));
	count = 0;
	for (i = 1, k = iprim - 1; i <= nn; i++, k = rs_modnn(k + iprim)) {
		q = 1;
		for (j = deg_lambda; j > 0; j--) {
			if (reg[j] != nn) {
				reg[j] = (uint16_t)rs_modnn((int)reg[j] + j);
				q ^= rs_alpha_to[reg[j]];
			}
		}
		if (q != 0)
			continue;
		root[count] = (uint16_t)i;
		loc[count] = (uint16_t)k;
		if (++count == deg_lambda)
			break;
	}
	if (deg_lambda != count)
		return -1;

	deg_omega = deg_lambda - 1;
	for (i = 0; i <= deg_omega; i++) {
		tmp = 0;
		for (j = i; j >= 0; j--) {
			if (syn[i - j] != nn && lambda[j] != nn) {
				tmp ^= rs_alpha_to[rs_modnn((int)syn[i - j] +
							    (int)lambda[j])];
			}
		}
		omega[i] = rs_index_of[tmp];
	}

	for (j = count - 1; j >= 0; j--) {
		num1 = 0;
		for (i = deg_omega; i >= 0; i--) {
			if (omega[i] != nn)
				num1 ^= rs_alpha_to[rs_modnn((int)omega[i] + i * root[j])];
		}
		num2 = rs_alpha_to[rs_modnn(root[j] * (fcr - 1) + nn)];
		den = 0;
		for (i = ((deg_lambda < nroots - 1) ? deg_lambda : nroots - 1) & ~1;
		     i >= 0; i -= 2) {
			if (lambda[i + 1] != nn)
				den ^= rs_alpha_to[rs_modnn((int)lambda[i + 1] +
							    i * root[j])];
		}
		if (loc[j] < pad)
			return -1;
		if (num1 != 0 && den != 0) {
			uint16_t cor = rs_alpha_to[rs_modnn((int)rs_index_of[num1] +
							    (int)rs_index_of[num2] +
							    nn - (int)rs_index_of[den])];
			if (loc[j] < nn - nroots)
				data[loc[j] - pad] ^= (uint8_t)cor;
			else
				parity[loc[j] - (nn - nroots)] ^= (uint8_t)cor;
		}
	}

	/*
	 * Berlekamp-Massey can produce a locator set for an uncorrectable word.
	 * Never deliver that tentative payload: verify the complete corrected
	 * codeword, including parity-symbol corrections, before accepting it.
	 */
	if (rs_compute_syndromes(data, len, parity, syn))
		return -1;
	return count;
}

static openvlc_status_t rs_encode_block(const uint8_t *data, size_t len,
					uint8_t *parity)
{
	enum { nn = 255, nroots = OPENVLC_BEAGLEBONE_RS_ECC_BYTES };
	uint16_t par[nroots];
	int pad;

	if (!data || !parity || len == 0u || len > 255u - nroots)
		return OPENVLC_ERR_ARG;
	rs_init();
	pad = nn - nroots - (int)len;
	if (pad < 0 || pad >= nn)
		return OPENVLC_ERR_ARG;
	memset(par, 0, sizeof(par));
	for (size_t i = 0; i < len; i++) {
		uint16_t fb = rs_index_of[data[i] ^ par[0]];

		if (fb != nn) {
			for (int j = 1; j < nroots; j++) {
				par[j] ^= rs_alpha_to[rs_modnn((int)fb +
							       (int)rs_genpoly[nroots - j])];
			}
		}
		memmove(&par[0], &par[1], sizeof(uint16_t) * (nroots - 1));
		par[nroots - 1] = fb != nn ?
			rs_alpha_to[rs_modnn((int)fb + (int)rs_genpoly[0])] : 0;
	}
	for (int i = 0; i < nroots; i++)
		parity[i] = (uint8_t)par[i];
	return OPENVLC_OK;
}

openvlc_status_t openvlc_frame_beaglebone_encode_rs(const uint8_t *data,
						    size_t encoded_len,
						    uint8_t *parity,
						    size_t parity_len)
{
	size_t blocks;

	if (!data || !parity)
		return OPENVLC_ERR_ARG;
	blocks = (encoded_len + OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE - 1u) /
		 OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE;
	if (parity_len < blocks * OPENVLC_BEAGLEBONE_RS_ECC_BYTES)
		return OPENVLC_ERR_OVERFLOW;
	for (size_t block = 0; block < blocks; block++) {
		size_t offset = block * OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE;
		size_t block_len = encoded_len - offset;
		openvlc_status_t status;

		if (block_len > OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE)
			block_len = OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE;
		status = rs_encode_block(&data[offset], block_len,
					 &parity[block * OPENVLC_BEAGLEBONE_RS_ECC_BYTES]);
		if (status != OPENVLC_OK)
			return status;
	}
	return OPENVLC_OK;
}

static OPENVLC_FEC_HOT openvlc_status_t beaglebone_rs_correct(
	uint8_t *data, size_t encoded_len,
	const uint8_t *parity_base, size_t parity_len)
{
	size_t blocks;
	int total_corrected = 0;

	rs_last_checked = true;
	rs_last_corrected = 0;
	if (!data || !parity_base)
		return OPENVLC_ERR_ARG;
	blocks = (encoded_len + OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE - 1u) /
		 OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE;
	if (parity_len < blocks * OPENVLC_BEAGLEBONE_RS_ECC_BYTES)
		return OPENVLC_ERR_SYNC;
	for (size_t block = 0; block < blocks; block++) {
		size_t offset = block * OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE;
		size_t block_len = encoded_len - offset;
		uint8_t parity[OPENVLC_BEAGLEBONE_RS_ECC_BYTES];
		int corrected;

		if (block_len > OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE)
			block_len = OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE;
		memcpy(parity,
		       &parity_base[block * OPENVLC_BEAGLEBONE_RS_ECC_BYTES],
		       sizeof(parity));
		corrected = rs_decode_block(&data[offset], block_len, parity);
		if (corrected < 0)
			return OPENVLC_ERR_CRC;
		total_corrected += corrected;
	}
	rs_last_corrected = total_corrected;
	return OPENVLC_OK;
}

int openvlc_frame_last_rs_corrected(void)
{
	return rs_last_corrected;
}

bool openvlc_frame_last_rs_checked(void)
{
	return rs_last_checked;
}

static bool beaglebone_payload_len_from_frame_len(size_t frame_len, uint16_t *payload_len)
{
#if OPENVLC_BEAGLEBONE_COMPAT
	const size_t fixed = OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES + 2u + OPENVLC_HEADER_BYTES;
	const size_t max_blocks =
		(OPENVLC_BEAGLEBONE_ENCODED_BYTES(OPENVLC_MAX_PAYLOAD_BYTES) +
		 OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE - 1u) /
		OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE;

	if (!payload_len || frame_len < fixed + OPENVLC_BEAGLEBONE_RS_ECC_BYTES)
		return false;
	for (size_t blocks = 1u; blocks <= max_blocks; blocks++) {
		size_t ecc = OPENVLC_BEAGLEBONE_RS_ECC_BYTES * blocks;
		size_t candidate;
		size_t encoded_len;
		size_t actual_blocks;

		if (frame_len < fixed + ecc)
			continue;
		candidate = frame_len - fixed - ecc;
		if (candidate > OPENVLC_MAX_PAYLOAD_BYTES)
			continue;
		encoded_len = OPENVLC_BEAGLEBONE_ENCODED_BYTES(candidate);
		actual_blocks =
			(encoded_len + OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE - 1u) /
			OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE;
		if (actual_blocks != blocks)
			continue;
		*payload_len = (uint16_t)candidate;
		return true;
	}
#else
	(void)frame_len;
	(void)payload_len;
#endif
	return false;
}

openvlc_status_t openvlc_frame_build(const openvlc_packet_t *packet,
				     uint8_t *frame, size_t frame_cap,
				     size_t *frame_len)
{
	size_t out = 0;
	uint16_t crc;

	if (!packet || !frame || !frame_len)
		return OPENVLC_ERR_ARG;
	if (packet->payload_len > OPENVLC_MAX_PAYLOAD_BYTES)
		return OPENVLC_ERR_ARG;
	if (frame_cap < OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES + OPENVLC_HEADER_BYTES +
			packet->payload_len + OPENVLC_CRC_BYTES)
		return OPENVLC_ERR_OVERFLOW;

	for (size_t i = 0; i < OPENVLC_PREAMBLE_BYTES; i++)
		frame[out++] = OPENVLC_PREAMBLE_BYTE;
	OPENVLC_SFD_EMIT(frame, out);
	frame[out++] = (uint8_t)(packet->payload_len >> 8);
	frame[out++] = (uint8_t)packet->payload_len;
	frame[out++] = packet->dst;
	frame[out++] = packet->src;
	frame[out++] = (uint8_t)(packet->protocol >> 8);
	frame[out++] = (uint8_t)packet->protocol;
	memcpy(&frame[out], packet->payload, packet->payload_len);
	out += packet->payload_len;

	crc = openvlc_crc16_ccitt(&frame[OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES],
				  OPENVLC_HEADER_BYTES + packet->payload_len);
	frame[out++] = (uint8_t)(crc >> 8);
	frame[out++] = (uint8_t)crc;
	*frame_len = out;
	return OPENVLC_OK;
}

OPENVLC_FEC_HOT openvlc_status_t openvlc_frame_parse(
	uint8_t *frame, size_t frame_len, openvlc_packet_t *packet)
{
	size_t pos = OPENVLC_PREAMBLE_BYTES;
	uint16_t payload_len;
	uint16_t beagle_payload_len;
	uint16_t got_crc;
	uint16_t calc_crc;
	openvlc_status_t status;

	if (!frame || !packet)
		return OPENVLC_ERR_ARG;
	rs_last_checked = false;
	rs_last_corrected = 0;
	if (frame_len < OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES + OPENVLC_HEADER_BYTES + OPENVLC_CRC_BYTES)
		return OPENVLC_ERR_SYNC;
	for (size_t i = 0; i < OPENVLC_PREAMBLE_BYTES; i++) {
		if (frame[i] != OPENVLC_PREAMBLE_BYTE)
			return OPENVLC_ERR_SYNC;
	}
	if (!OPENVLC_SFD_MATCH(&frame[pos]))
		return OPENVLC_ERR_SYNC;
	pos += OPENVLC_SFD_BYTES;

	payload_len = ((uint16_t)frame[pos] << 8) | frame[pos + 1u];
	pos += 2;
#if OPENVLC_BEAGLEBONE_COMPAT
	if (payload_len > OPENVLC_MAX_PAYLOAD_BYTES &&
	    beaglebone_payload_len_from_frame_len(frame_len, &beagle_payload_len)) {
		uint8_t *rs_data;
		size_t encoded_len;
		size_t blocks;
		size_t parity_base;

		payload_len = beagle_payload_len;
		encoded_len = (size_t)payload_len + 2u * 2u + 2u;
		blocks = (encoded_len + OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE - 1u) /
			 OPENVLC_BEAGLEBONE_RS_BLOCK_SIZE;
		parity_base = pos + encoded_len;
		if (parity_base + blocks * OPENVLC_BEAGLEBONE_RS_ECC_BYTES >
		    frame_len)
			return OPENVLC_ERR_SYNC;
		rs_data = &frame[pos];
		status = beaglebone_rs_correct(rs_data, encoded_len, &frame[parity_base],
					       frame_len - parity_base);
		if (status != OPENVLC_OK)
			return status;
		packet->payload_len = payload_len;
		packet->dst = rs_data[1];
		packet->src = rs_data[3];
		packet->protocol = ((uint16_t)rs_data[4] << 8) | rs_data[5];
		memcpy(packet->payload, &rs_data[6], payload_len);
		return OPENVLC_OK;
	}
#endif
	if (payload_len > OPENVLC_MAX_PAYLOAD_BYTES)
		return OPENVLC_ERR_ARG;
	if (frame_len != OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES + OPENVLC_HEADER_BYTES +
			 payload_len + OPENVLC_CRC_BYTES)
		return OPENVLC_ERR_SYNC;

	{
		size_t meta_pos = pos;
		size_t payload_pos = meta_pos + (OPENVLC_HEADER_BYTES - 2u);
		size_t crc_pos = payload_pos + payload_len;

		got_crc = ((uint16_t)frame[crc_pos] << 8) |
			  frame[crc_pos + 1u];
		calc_crc = openvlc_crc16_ccitt(
			&frame[OPENVLC_PREAMBLE_BYTES + OPENVLC_SFD_BYTES],
			OPENVLC_HEADER_BYTES + payload_len);
		if (got_crc != calc_crc)
			return OPENVLC_ERR_CRC;

		packet->payload_len = payload_len;
		packet->dst = frame[meta_pos];
		packet->src = frame[meta_pos + 1u];
		packet->protocol = ((uint16_t)frame[meta_pos + 2u] << 8) |
				   frame[meta_pos + 3u];
		memcpy(packet->payload, &frame[payload_pos], payload_len);
	}
	return OPENVLC_OK;
}
