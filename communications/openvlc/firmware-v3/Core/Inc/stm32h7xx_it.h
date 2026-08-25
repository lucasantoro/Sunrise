/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32h7xx_it.h
  * @brief   This file contains the headers of the interrupt handlers.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __STM32H7xx_IT_H
#define __STM32H7xx_IT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

typedef struct
{
  uint32_t magic;
  uint32_t kind;
  uint32_t ipsr;
  uint32_t exc_return;
  uint32_t stack_pointer;
  uint32_t r0;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r12;
  uint32_t lr;
  uint32_t pc;
  uint32_t xpsr;
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t dfsr;
  uint32_t afsr;
  uint32_t mmfar;
  uint32_t bfar;
  uint32_t shcsr;
  uint32_t icsr;
  uint32_t main_phase;
  uint32_t main_loop_count;
  uint32_t main_last_tick;
} openvlc_fault_record_t;

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);
void DMA1_Stream1_IRQHandler(void);
void DMA1_Stream2_IRQHandler(void);
void DMA1_Stream3_IRQHandler(void);
void DMA2_Stream0_IRQHandler(void);
void TIM6_DAC_IRQHandler(void);
/* USER CODE BEGIN EFP */
extern volatile openvlc_fault_record_t openvlc_fault_record;
extern volatile uint32_t openvlc_diag_usart3_irq_count;
extern volatile uint32_t openvlc_diag_usart3_irq_phase;
extern volatile uint32_t openvlc_diag_usart3_last_isr;
extern volatile uint32_t openvlc_diag_usart3_error_count;
extern volatile uint32_t openvlc_diag_usart3_overrun_count;
extern volatile uint32_t openvlc_diag_encode_irq_count;
extern volatile uint32_t openvlc_diag_encode_irq_phase;

void openvlc_fault_capture(uint32_t kind, uint32_t *stack,
                           uint32_t exc_return) __attribute__((noreturn));

/* USER CODE END EFP */

#ifdef __cplusplus
}
#endif

#endif /* __STM32H7xx_IT_H */
