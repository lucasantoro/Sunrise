#ifndef OPENVLC_TRANSCEIVER_HOST_H
#define OPENVLC_TRANSCEIVER_HOST_H

#include <stdint.h>

int openvlc_transceiver_host_init(void);
void openvlc_transceiver_host_poll(void);
void openvlc_transceiver_host_log(uint32_t now_ms);
void openvlc_transceiver_host_encode_isr(void);

#endif
