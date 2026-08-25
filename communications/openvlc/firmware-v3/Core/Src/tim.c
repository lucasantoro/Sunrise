#include "tim.h"

#include "openvlc_board.h"

TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim1;
DMA_HandleTypeDef hdma_tim2_ch4;
DMA_HandleTypeDef hdma_tim1_ch4;

void MX_TIM2_Init(void)
{
	TIM_ClockConfigTypeDef clock = {0};
	TIM_MasterConfigTypeDef master = {0};
	TIM_IC_InitTypeDef capture = {0};

	htim2.Instance = TIM2;
	htim2.Init.Prescaler = OPENVLC_TIM2_IC_PRESCALER;
	htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim2.Init.Period = UINT32_MAX;
	htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
	if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
		Error_Handler();
	clock.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
	if (HAL_TIM_ConfigClockSource(&htim2, &clock) != HAL_OK)
		Error_Handler();
	if (HAL_TIM_IC_Init(&htim2) != HAL_OK)
		Error_Handler();
	master.MasterOutputTrigger = TIM_TRGO_RESET;
	master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
	if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &master) != HAL_OK)
		Error_Handler();
	capture.ICPolarity = TIM_INPUTCHANNELPOLARITY_BOTHEDGE;
	capture.ICSelection = TIM_ICSELECTION_DIRECTTI;
	capture.ICPrescaler = TIM_ICPSC_DIV1;
	capture.ICFilter = OPENVLC_COMP_TIM_IC_FILTER;
	if (HAL_TIM_IC_ConfigChannel(&htim2, &capture, TIM_CHANNEL_4) != HAL_OK)
		Error_Handler();
}

void MX_TIM1_Init(void)
{
	TIM_ClockConfigTypeDef clock = {0};
	TIM_MasterConfigTypeDef master = {0};

	htim1.Instance = TIM1;
	htim1.Init.Prescaler = 0;
	htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim1.Init.Period = OPENVLC_STM32_TX_CELL_TICKS - 1u;
	htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim1.Init.RepetitionCounter = 0u;
	htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
	if (HAL_TIM_Base_Init(&htim1) != HAL_OK)
		Error_Handler();
	clock.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
	if (HAL_TIM_ConfigClockSource(&htim1, &clock) != HAL_OK)
		Error_Handler();
	master.MasterOutputTrigger = TIM_TRGO_RESET;
	master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
	if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &master) != HAL_OK)
		Error_Handler();

	MODIFY_REG(TIM1->CCMR1,
		   TIM_CCMR1_CC1S | TIM_CCMR1_OC1M | TIM_CCMR1_OC1PE,
		   (6u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE);
	TIM1->CCR1 = 0u;
	TIM1->CCER &= ~(TIM_CCER_CC1P | TIM_CCER_CC1E);
	MODIFY_REG(TIM1->CCMR2,
		   TIM_CCMR2_CC4S | TIM_CCMR2_OC4M | TIM_CCMR2_OC4PE, 0u);
	TIM1->CCR4 = OPENVLC_STM32_TX_CELL_TICKS / OPENVLC_TX_OC_DMA_PHASE_DIV;
	TIM1->CCER &= ~(TIM_CCER_CC4P | TIM_CCER_CC4E);
	TIM1->BDTR &= ~TIM_BDTR_MOE;
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *handle)
{
	if (handle->Instance == TIM2) {
		__HAL_RCC_TIM2_CLK_ENABLE();

		hdma_tim2_ch4.Instance = DMA1_Stream1;
		hdma_tim2_ch4.Init.Request = DMA_REQUEST_TIM2_CH4;
		hdma_tim2_ch4.Init.Direction = DMA_PERIPH_TO_MEMORY;
		hdma_tim2_ch4.Init.PeriphInc = DMA_PINC_DISABLE;
		hdma_tim2_ch4.Init.MemInc = DMA_MINC_ENABLE;
		hdma_tim2_ch4.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
		hdma_tim2_ch4.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
		hdma_tim2_ch4.Init.Mode = DMA_CIRCULAR;
		hdma_tim2_ch4.Init.Priority = DMA_PRIORITY_VERY_HIGH;
		hdma_tim2_ch4.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
		if (HAL_DMA_Init(&hdma_tim2_ch4) != HAL_OK)
			Error_Handler();
		__HAL_LINKDMA(handle, hdma[TIM_DMA_ID_CC4], hdma_tim2_ch4);
	} else if (handle->Instance == TIM1) {
		__HAL_RCC_TIM1_CLK_ENABLE();
		/* Keep the high-rate TX cell stream off DMA1. TIM2 capture is the
		 * latency-sensitive RX path and must not contend with ~2.5 M TX DMA
		 * requests/s inside the same DMA controller. */
		hdma_tim1_ch4.Instance = DMA2_Stream0;
		hdma_tim1_ch4.Init.Request = DMA_REQUEST_TIM1_CH4;
		hdma_tim1_ch4.Init.Direction = DMA_MEMORY_TO_PERIPH;
		hdma_tim1_ch4.Init.PeriphInc = DMA_PINC_DISABLE;
		hdma_tim1_ch4.Init.MemInc = DMA_MINC_ENABLE;
#if defined(OPENVLC_TX_USE_TIMER_OC) && OPENVLC_TX_USE_TIMER_OC
		hdma_tim1_ch4.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
		hdma_tim1_ch4.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
#else
		hdma_tim1_ch4.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
		hdma_tim1_ch4.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
#endif
		hdma_tim1_ch4.Init.Mode = DMA_NORMAL;
		/*
		 * Prefetch several halfword cells from AXI SRAM. Direct mode required
		 * an AXI read and APB write inside every sub-microsecond preload
		 * window; the FIFO leaves only the deterministic APB write critical.
		 */
		hdma_tim1_ch4.Init.Priority = DMA_PRIORITY_VERY_HIGH;
		hdma_tim1_ch4.Init.FIFOMode = DMA_FIFOMODE_ENABLE;
		hdma_tim1_ch4.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_FULL;
		hdma_tim1_ch4.Init.MemBurst = DMA_MBURST_SINGLE;
		hdma_tim1_ch4.Init.PeriphBurst = DMA_PBURST_SINGLE;
		if (HAL_DMA_Init(&hdma_tim1_ch4) != HAL_OK)
			Error_Handler();
		__HAL_LINKDMA(handle, hdma[TIM_DMA_ID_CC4], hdma_tim1_ch4);
	}
}

void HAL_TIM_Base_MspDeInit(TIM_HandleTypeDef *handle)
{
	if (handle->Instance == TIM2) {
		__HAL_RCC_TIM2_CLK_DISABLE();
		HAL_DMA_DeInit(handle->hdma[TIM_DMA_ID_CC4]);
	} else if (handle->Instance == TIM1) {
		__HAL_RCC_TIM1_CLK_DISABLE();
		HAL_DMA_DeInit(handle->hdma[TIM_DMA_ID_CC4]);
	}
}
