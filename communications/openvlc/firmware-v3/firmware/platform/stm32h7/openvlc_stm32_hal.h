#ifndef OPENVLC_STM32_HAL_H
#define OPENVLC_STM32_HAL_H

#include <stddef.h>
#include <stdint.h>

#include "openvlc_app.h"

void openvlc_stm32_init(void);
int openvlc_stm32_memory_selftest(void);
int openvlc_stm32_start(void);
void openvlc_stm32_debug_poll(uint32_t now_ms);
void openvlc_stm32_host_poll(void);
int openvlc_stm32_rx_comparator_start(void);
void openvlc_stm32_rx_comparator_poll(void);

#endif
