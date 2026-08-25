#ifndef __TIM_H__
#define __TIM_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim1;
extern DMA_HandleTypeDef hdma_tim2_ch4;
extern DMA_HandleTypeDef hdma_tim1_ch4;

void MX_TIM2_Init(void);
void MX_TIM1_Init(void);

#ifdef __cplusplus
}
#endif
#endif
