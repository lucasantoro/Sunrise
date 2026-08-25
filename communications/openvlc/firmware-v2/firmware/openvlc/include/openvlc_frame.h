#ifndef OPENVLC_FRAME_H
#define OPENVLC_FRAME_H

#include "openvlc_types.h"

void openvlc_frame_init(void);
openvlc_status_t openvlc_frame_build(const openvlc_packet_t *packet,
				     uint8_t *frame, size_t frame_cap,
				     size_t *frame_len);
openvlc_status_t openvlc_frame_parse(uint8_t *frame, size_t frame_len,
				     openvlc_packet_t *packet);
openvlc_status_t openvlc_frame_beaglebone_encode_rs(const uint8_t *data,
						    size_t encoded_len,
						    uint8_t *parity,
						    size_t parity_len);
int openvlc_frame_last_rs_corrected(void);
bool openvlc_frame_last_rs_checked(void);

#endif
