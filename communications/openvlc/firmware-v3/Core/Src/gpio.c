#include "gpio.h"

#include "openvlc_board.h"

void MX_GPIO_Init(void)
{
	GPIO_InitTypeDef gpio = {0};

	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOD_CLK_ENABLE();
	__HAL_RCC_GPIOE_CLK_ENABLE();
	__HAL_RCC_GPIOH_CLK_ENABLE();

	HAL_GPIO_WritePin(OPENVLC_HEARTBEAT_GPIO_PORT,
			  OPENVLC_HEARTBEAT_GPIO_PIN | OPENVLC_FAULT_GPIO_PIN,
			  GPIO_PIN_RESET);
	gpio.Pin = OPENVLC_HEARTBEAT_GPIO_PIN | OPENVLC_FAULT_GPIO_PIN;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOA, &gpio);

	HAL_GPIO_WritePin(OPENVLC_TX_GPIO_PORT, OPENVLC_TX_GPIO_PIN,
			  GPIO_PIN_RESET);
	gpio.Pin = OPENVLC_TX_GPIO_PIN;
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Pull = GPIO_PULLDOWN;
	/* The external driver input is high impedance and 2 Mcell/s remains within
	 * the LOW-slew capability. Keep PE9 slow: full-duplex logs show that faster
	 * edges couple directly into the comparator path without helping DMA. */
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	gpio.Alternate = GPIO_AF1_TIM1;
	HAL_GPIO_Init(OPENVLC_TX_GPIO_PORT, &gpio);

	HAL_GPIO_WritePin(OPENVLC_TX_EN_GPIO_PORT, OPENVLC_TX_EN_GPIO_PIN,
			  GPIO_PIN_RESET);
	gpio.Pin = OPENVLC_TX_EN_GPIO_PIN;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pull = GPIO_PULLDOWN;
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(OPENVLC_TX_EN_GPIO_PORT, &gpio);
}
