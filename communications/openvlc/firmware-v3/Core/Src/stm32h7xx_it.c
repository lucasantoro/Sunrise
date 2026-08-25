/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32h7xx_it.c
  * @brief   Interrupt Service Routines.
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

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32h7xx_it.h"
#include "openvlc_transceiver_host.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "openvlc_board.h"
#include "openvlc_stm32_tx_hal.h"
#include <stdbool.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
volatile uint32_t openvlc_hf_cfsr = 0xBAD00001u;
volatile uint32_t openvlc_hf_hfsr = 0xBAD00002u;
volatile uint32_t openvlc_hf_dfsr = 0xBAD00003u;
volatile uint32_t openvlc_hf_afsr = 0xBAD00004u;
volatile uint32_t openvlc_hf_mmar = 0xBAD00005u;
volatile uint32_t openvlc_hf_bfar = 0xBAD00006u;
volatile uint32_t openvlc_hf_boot_step = 0xBAD00007u;
volatile uint32_t openvlc_hf_stm32_start_step = 0xBAD00008u;
volatile uint32_t openvlc_hf_comp_start_step = 0xBAD00009u;
volatile uint32_t openvlc_hf_comp_dma_cr = 0xBAD0000Au;
volatile uint32_t openvlc_hf_comp_dma_fcr = 0xBAD0000Bu;
volatile uint32_t openvlc_hf_comp_dma_ndtr = 0xBAD0000Cu;
volatile uint32_t openvlc_hf_comp_poll_step = 0xBAD0000Du;
volatile uint32_t openvlc_hf_comp_poll_head = 0xBAD0000Eu;
volatile uint32_t openvlc_hf_comp_poll_tail = 0xBAD0000Fu;
volatile uint32_t openvlc_hf_comp_poll_burst_len = 0xBAD00010u;
volatile openvlc_fault_record_t openvlc_fault_record = {
  .magic = 0xBAD0FA17u,
  .kind = 0u
};
volatile uint32_t openvlc_diag_usart3_irq_count;
volatile uint32_t openvlc_diag_usart3_irq_phase;
volatile uint32_t openvlc_diag_usart3_last_isr;
volatile uint32_t openvlc_diag_usart3_error_count;
volatile uint32_t openvlc_diag_usart3_overrun_count;
volatile uint32_t openvlc_diag_encode_irq_count;
volatile uint32_t openvlc_diag_encode_irq_phase;
/* Defined in main.c; snapshot it into the fault handler for post-mortem. */
extern volatile uint32_t openvlc_boot_step;
extern volatile uint32_t openvlc_stm32_start_step;
extern volatile uint32_t openvlc_comp_start_step;
extern volatile uint32_t openvlc_comp_dma_cr;
extern volatile uint32_t openvlc_comp_dma_fcr;
extern volatile uint32_t openvlc_comp_dma_ndtr;
extern volatile uint32_t openvlc_comp_poll_step;
extern volatile uint32_t openvlc_comp_poll_head;
extern volatile uint32_t openvlc_comp_poll_tail;
extern volatile uint32_t openvlc_comp_poll_burst_len;
extern volatile uint32_t openvlc_mem_test_step;
extern volatile uint32_t openvlc_main_phase;
extern volatile uint32_t openvlc_main_loop_count;
extern volatile uint32_t openvlc_main_last_tick;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static bool fault_stack_frame_valid(const uint32_t *frame)
{
  uintptr_t address = (uintptr_t)frame;

  return (address >= 0x20000000u && address <= (0x20020000u - 32u) &&
          (address & 0x3u) == 0u);
}

void openvlc_fault_capture(uint32_t kind, uint32_t *stack,
                           uint32_t exc_return)
{
  uint32_t *frame = stack;

  __disable_irq();
  openvlc_fault_record.magic = 0xFA170000u | (kind & 0xFFFFu);
  openvlc_fault_record.kind = kind;
  openvlc_fault_record.ipsr = __get_IPSR();
  openvlc_fault_record.exc_return = exc_return;
  openvlc_fault_record.stack_pointer = (uint32_t)(uintptr_t)stack;

  /* With lazy FPU stacking disabled, bit 4 clear means 18 FP words precede
   * the standard eight-word Cortex exception frame. */
  if (stack != NULL && (exc_return & (1u << 4)) == 0u)
    frame += 18u;
  if (fault_stack_frame_valid(frame))
  {
    openvlc_fault_record.r0 = frame[0];
    openvlc_fault_record.r1 = frame[1];
    openvlc_fault_record.r2 = frame[2];
    openvlc_fault_record.r3 = frame[3];
    openvlc_fault_record.r12 = frame[4];
    openvlc_fault_record.lr = frame[5];
    openvlc_fault_record.pc = frame[6];
    openvlc_fault_record.xpsr = frame[7];
  }
  openvlc_fault_record.cfsr = SCB->CFSR;
  openvlc_fault_record.hfsr = SCB->HFSR;
  openvlc_fault_record.dfsr = SCB->DFSR;
  openvlc_fault_record.afsr = SCB->AFSR;
  openvlc_fault_record.mmfar = SCB->MMFAR;
  openvlc_fault_record.bfar = SCB->BFAR;
  openvlc_fault_record.shcsr = SCB->SHCSR;
  openvlc_fault_record.icsr = SCB->ICSR;
  openvlc_fault_record.main_phase = openvlc_main_phase;
  openvlc_fault_record.main_loop_count = openvlc_main_loop_count;
  openvlc_fault_record.main_last_tick = openvlc_main_last_tick;

  /* PA9/FLAG_2 is a fault indicator that also works before HAL GPIO init. */
  RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN;
  (void)RCC->AHB4ENR;
  GPIOA->MODER = (GPIOA->MODER & ~(3u << (9u * 2u))) |
                 (1u << (9u * 2u));
  GPIOA->OTYPER &= ~GPIO_PIN_9;
  GPIOA->PUPDR &= ~(3u << (9u * 2u));
  GPIOA->BSRR = GPIO_PIN_9;
  __DSB();
  __ISB();
  while (1) { }
}
/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern DMA_HandleTypeDef hdma_tim2_ch4;
extern DMA_HandleTypeDef hdma_tim1_ch4;
extern DMA_HandleTypeDef hdma_usart3_rx;
extern DMA_HandleTypeDef hdma_usart3_tx;
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void) __attribute__((naked));
void NMI_Handler(void)
{
  __asm volatile(
    "movs r0, #1\n"
    "tst lr, #4\n"
    "ite eq\n"
    "mrseq r1, msp\n"
    "mrsne r1, psp\n"
    "mov r2, lr\n"
    "b openvlc_fault_capture\n");
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void) __attribute__((naked));
void HardFault_Handler(void)
{
  __asm volatile(
    "movs r0, #2\n"
    "tst lr, #4\n"
    "ite eq\n"
    "mrseq r1, msp\n"
    "mrsne r1, psp\n"
    "mov r2, lr\n"
    "b openvlc_fault_capture\n");
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void) __attribute__((naked));
void MemManage_Handler(void)
{
  __asm volatile(
    "movs r0, #3\n"
    "tst lr, #4\n"
    "ite eq\n"
    "mrseq r1, msp\n"
    "mrsne r1, psp\n"
    "mov r2, lr\n"
    "b openvlc_fault_capture\n");
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void) __attribute__((naked));
void BusFault_Handler(void)
{
  __asm volatile(
    "movs r0, #4\n"
    "tst lr, #4\n"
    "ite eq\n"
    "mrseq r1, msp\n"
    "mrsne r1, psp\n"
    "mov r2, lr\n"
    "b openvlc_fault_capture\n");
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void) __attribute__((naked));
void UsageFault_Handler(void)
{
  __asm volatile(
    "movs r0, #5\n"
    "tst lr, #4\n"
    "ite eq\n"
    "mrseq r1, msp\n"
    "mrsne r1, psp\n"
    "mov r2, lr\n"
    "b openvlc_fault_capture\n");
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void)
{
  /* USER CODE BEGIN SVCall_IRQn 0 */

  /* USER CODE END SVCall_IRQn 0 */
  /* USER CODE BEGIN SVCall_IRQn 1 */

  /* USER CODE END SVCall_IRQn 1 */
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  */
void PendSV_Handler(void)
{
  /* USER CODE BEGIN PendSV_IRQn 0 */

  /* USER CODE END PendSV_IRQn 0 */
  /* USER CODE BEGIN PendSV_IRQn 1 */

  /* USER CODE END PendSV_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */

  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32H7xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32h7xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles DMA1 stream1 global interrupt.
  */
void DMA1_Stream1_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Stream1_IRQn 0 */
  return;

  /* USER CODE END DMA1_Stream1_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_tim2_ch4);
  /* USER CODE BEGIN DMA1_Stream1_IRQn 1 */

  /* USER CODE END DMA1_Stream1_IRQn 1 */
}

void DMA2_Stream0_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_tim1_ch4);
}

/**
  * @brief This function handles the OpenVLC TX inter-frame one-shot timer.
  */
void TIM6_DAC_IRQHandler(void)
{
  openvlc_stm32_tx_guard_irq();
}

void DMA1_Stream2_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_usart3_rx);
}

void DMA1_Stream3_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_usart3_tx);
}

void LPTIM5_IRQHandler(void)
{
  openvlc_diag_encode_irq_count++;
  openvlc_diag_encode_irq_phase = 1u;
  openvlc_transceiver_host_encode_isr();
  openvlc_diag_encode_irq_phase = 0u;
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
