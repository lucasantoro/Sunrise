#include "usart.h"

#include "openvlc_board.h"

UART_HandleTypeDef huart3;
DMA_HandleTypeDef hdma_usart3_rx;
DMA_HandleTypeDef hdma_usart3_tx;

void MX_USART3_UART_Init(void)
{
	huart3.Instance = USART3;
	huart3.Init.BaudRate = OPENVLC_HOST_UART_BAUD;
	huart3.Init.WordLength = UART_WORDLENGTH_8B;
	huart3.Init.StopBits = UART_STOPBITS_1;
	huart3.Init.Parity = UART_PARITY_NONE;
	huart3.Init.Mode = UART_MODE_TX_RX;
	huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart3.Init.OverSampling = UART_OVERSAMPLING_16;
	huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
	huart3.Init.ClockPrescaler = UART_PRESCALER_DIV1;
	huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
	if (HAL_UART_Init(&huart3) != HAL_OK)
		Error_Handler();
	if (HAL_UARTEx_SetTxFifoThreshold(&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
		Error_Handler();
	if (HAL_UARTEx_SetRxFifoThreshold(&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
		Error_Handler();
	if (HAL_UARTEx_EnableFifoMode(&huart3) != HAL_OK)
		Error_Handler();
}

void HAL_UART_MspInit(UART_HandleTypeDef *handle)
{
	GPIO_InitTypeDef gpio = {0};
	RCC_PeriphCLKInitTypeDef clock = {0};

	if (handle->Instance != USART3)
		return;
	clock.PeriphClockSelection = RCC_PERIPHCLK_USART3;
	clock.Usart234578ClockSelection = RCC_USART234578CLKSOURCE_D2PCLK1;
	if (HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK)
		Error_Handler();
	__HAL_RCC_USART3_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();
	gpio.Pin = HOST_UART_TX_Pin | HOST_UART_RX_Pin;
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	gpio.Alternate = GPIO_AF7_USART3;
	HAL_GPIO_Init(GPIOB, &gpio);

	/* Keep host RX away from the continuous optical TX traffic on DMA2.
	 * DMA1 Stream2 shares the controller with capture but not its stream. */
	hdma_usart3_rx.Instance = DMA1_Stream2;
	hdma_usart3_rx.Init.Request = DMA_REQUEST_USART3_RX;
	hdma_usart3_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
	hdma_usart3_rx.Init.PeriphInc = DMA_PINC_DISABLE;
	hdma_usart3_rx.Init.MemInc = DMA_MINC_ENABLE;
	hdma_usart3_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
	hdma_usart3_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
	hdma_usart3_rx.Init.Mode = DMA_CIRCULAR;
	hdma_usart3_rx.Init.Priority = DMA_PRIORITY_VERY_HIGH;
	hdma_usart3_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
	if (HAL_DMA_Init(&hdma_usart3_rx) != HAL_OK)
		Error_Handler();
	__HAL_LINKDMA(handle, hdmarx, hdma_usart3_rx);

	/*
	 * Host forwarding is continuous at profile 1000. A TXE-driven transfer
	 * would interrupt the decoder once per UART byte, so transmit complete
	 * records through a normal-mode DMA stream instead.
	 */
	hdma_usart3_tx.Instance = DMA1_Stream3;
	hdma_usart3_tx.Init.Request = DMA_REQUEST_USART3_TX;
	hdma_usart3_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
	hdma_usart3_tx.Init.PeriphInc = DMA_PINC_DISABLE;
	hdma_usart3_tx.Init.MemInc = DMA_MINC_ENABLE;
	hdma_usart3_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
	hdma_usart3_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
	hdma_usart3_tx.Init.Mode = DMA_NORMAL;
	hdma_usart3_tx.Init.Priority = DMA_PRIORITY_LOW;
	hdma_usart3_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
	if (HAL_DMA_Init(&hdma_usart3_tx) != HAL_OK)
		Error_Handler();
	__HAL_LINKDMA(handle, hdmatx, hdma_usart3_tx);
}

void HAL_UART_MspDeInit(UART_HandleTypeDef *handle)
{
	if (handle->Instance != USART3)
		return;
	__HAL_RCC_USART3_CLK_DISABLE();
	HAL_GPIO_DeInit(GPIOB, HOST_UART_TX_Pin | HOST_UART_RX_Pin);
	HAL_DMA_DeInit(handle->hdmarx);
	HAL_DMA_DeInit(handle->hdmatx);
}
