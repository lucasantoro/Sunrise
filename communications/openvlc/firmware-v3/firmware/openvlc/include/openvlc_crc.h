#ifndef OPENVLC_CRC_H
#define OPENVLC_CRC_H

#include <stddef.h>
#include <stdint.h>

uint16_t openvlc_crc16_ccitt(const uint8_t *data, size_t len);

#endif

