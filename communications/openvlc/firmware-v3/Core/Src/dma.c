#include "dma.h"

void MX_DMA_Init(void)
{
	__HAL_RCC_DMA1_CLK_ENABLE();
	__HAL_RCC_DMA2_CLK_ENABLE();

	/* TIM2 capture is polled through NDTR; its DMA IRQ remains disabled. */
	HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 1u, 0u);
	HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
	HAL_NVIC_SetPriority(DMA1_Stream2_IRQn, 4u, 0u);
	HAL_NVIC_EnableIRQ(DMA1_Stream2_IRQn);
	HAL_NVIC_SetPriority(DMA1_Stream3_IRQn, 6u, 0u);
	HAL_NVIC_EnableIRQ(DMA1_Stream3_IRQn);
}
