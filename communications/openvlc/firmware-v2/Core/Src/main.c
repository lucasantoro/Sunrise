/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "comp.h"
#include "dac.h"
#include "dma.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "openvlc_board.h"
#include "openvlc_stm32_hal.h"
#include "openvlc_stm32_tx_hal.h"
#include "openvlc_transceiver_host.h"
#include "stm32h7xx_it.h"
#include <stdio.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* See Core/Inc/openvlc_board.h for the bring-up stage table. */
#ifndef OPENVLC_BOOT_STAGE
#define OPENVLC_BOOT_STAGE 0
#endif

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
volatile uint32_t openvlc_boot_step = 0xB0070000u;
volatile uint32_t openvlc_main_loop_count;
volatile uint32_t openvlc_main_phase;
volatile uint32_t openvlc_main_last_tick;
static uint32_t openvlc_reset_flags;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void openvlc_tx_boot_pin_test(void)
{
#if OPENVLC_TX_BOOT_PIN_TEST_MS > 0u
  const uint32_t period = OPENVLC_STM32_TX_TIMER_HZ /
                          OPENVLC_TX_BOOT_PIN_TEST_HZ;

  if (period < 2u)
    Error_Handler();

  /* TIM1_CH1 PWM on PE9, independent of UART RX, framing and DMA. */
  CLEAR_BIT(TIM1->CR1, TIM_CR1_CEN);
  CLEAR_BIT(TIM1->CCER, TIM_CCER_CC1E);
  CLEAR_BIT(TIM1->BDTR, TIM_BDTR_MOE);
  TIM1->PSC = 0u;
  TIM1->ARR = period - 1u;
  TIM1->CCR1 = period / 2u;
  MODIFY_REG(TIM1->CCMR1,
             TIM_CCMR1_CC1S | TIM_CCMR1_OC1M | TIM_CCMR1_OC1PE,
             (6u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE);
  CLEAR_BIT(TIM1->CCER, TIM_CCER_CC1P);
  TIM1->EGR = TIM_EGR_UG;
  TIM1->SR = 0u;
  SET_BIT(TIM1->CCER, TIM_CCER_CC1E);
  SET_BIT(TIM1->BDTR, TIM_BDTR_MOE);
  SET_BIT(TIM1->CR1, TIM_CR1_CEN);
  HAL_Delay(OPENVLC_TX_BOOT_PIN_TEST_MS);

  /* Restore the operational OpenVLC timer profile. */
  CLEAR_BIT(TIM1->CR1, TIM_CR1_CEN);
  CLEAR_BIT(TIM1->CCER, TIM_CCER_CC1E);
  CLEAR_BIT(TIM1->BDTR, TIM_BDTR_MOE);
  TIM1->PSC = 0u;
  TIM1->ARR = OPENVLC_STM32_TX_CELL_TICKS - 1u;
  TIM1->CCR1 = 0u;
  TIM1->CCR4 = OPENVLC_STM32_TX_CELL_TICKS /
               OPENVLC_TX_OC_DMA_PHASE_DIV;
  TIM1->CNT = 0u;
  TIM1->EGR = TIM_EGR_UG;
  TIM1->SR = 0u;
#endif
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  openvlc_reset_flags = RCC->RSR;
  RCC->RSR |= RCC_RSR_RMVF;

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* USER CODE BEGIN CPU_CACHE_ENABLE */
#if OPENVLC_ENABLE_ICACHE
  /*
   * The edge decoder and Reed-Solomon parser execute from Flash. I-cache is
   * required for sustained reception.
   */
  SCB_InvalidateICache();
  SCB_EnableICache();
#endif
#if OPENVLC_ENABLE_DCACHE
  /*
   * D-cache halves the decode time (the RAM-bound hot path), restoring the
   * processing margin at the full 134 frame/s load. DMA coherency: the TIM2
     * capture ring occupies the first 160 KB of RAM_D1 and the three TIM1 TX
     * streams occupy the following ~94 KB. MPU region 1 therefore marks the
     * complete first 256 KB NON-CACHEABLE. The final 64 KB remains cacheable
     * for CPU-only RX/host buffers. This keeps TIM2 coherent and lets DMA2
     * prefetch TX cells without a per-frame D-cache clean on the shared AXI
     * bus.
   */
  {
    MPU_Region_InitTypeDef ring_region = {0};
    MPU_Region_InitTypeDef dma_region = {0};

    HAL_MPU_Disable();
    ring_region.Enable = MPU_REGION_ENABLE;
    ring_region.Number = MPU_REGION_NUMBER1;
    ring_region.BaseAddress = 0x24000000UL;
    ring_region.Size = MPU_REGION_SIZE_256KB;
    ring_region.SubRegionDisable = 0x00;
    ring_region.TypeExtField = MPU_TEX_LEVEL1;      /* normal memory */
    ring_region.AccessPermission = MPU_REGION_FULL_ACCESS;
    ring_region.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    ring_region.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
    ring_region.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    ring_region.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&ring_region);

    /* DMA1-visible D2 SRAM holds the circular USART3 RX buffer. */
    dma_region.Enable = MPU_REGION_ENABLE;
    dma_region.Number = MPU_REGION_NUMBER2;
    dma_region.BaseAddress = 0x30000000UL;
    dma_region.Size = MPU_REGION_SIZE_32KB;
    dma_region.SubRegionDisable = 0x00;
    dma_region.TypeExtField = MPU_TEX_LEVEL1;
    dma_region.AccessPermission = MPU_REGION_FULL_ACCESS;
    dma_region.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    dma_region.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
    dma_region.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    dma_region.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&dma_region);
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
  }
  SCB_EnableDCache();
#endif
  /* USER CODE END CPU_CACHE_ENABLE */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_TIM2_Init();
  MX_TIM1_Init();
  MX_USART3_UART_Init();
  MX_COMP1_Init();
  MX_DAC1_Init();
  /* USER CODE BEGIN 2 */
  openvlc_boot_step = 10u;
  openvlc_tx_boot_pin_test();
  {
    const uint8_t boot_msg[] = "BOOT: USART3 OK\r\n";
    openvlc_boot_step = 11u;
    HAL_UART_Transmit(&huart3, (uint8_t *)boot_msg, sizeof(boot_msg) - 1u, 100);
    openvlc_boot_step = 12u;
  }
  {
    uint32_t pclk2 = HAL_RCC_GetPCLK2Freq();
    uint32_t timclk = ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE2) !=
                       RCC_D2CFGR_D2PPRE2_DIV1) ? pclk2 * 2u : pclk2;
    char hw_msg[128];
    int n = snprintf(hw_msg, sizeof(hw_msg),
                     "BOOT: H723 devid=0x%03lx sys=%lu tim=%lu\r\n",
                     (unsigned long)HAL_GetDEVID(),
                     (unsigned long)HAL_RCC_GetSysClockFreq(),
                     (unsigned long)timclk);
    if (n > 0)
      HAL_UART_Transmit(&huart3, (uint8_t *)hw_msg, (uint16_t)n, 100);
    /* Link identity banner: with three boards in the lab, a same-src pair
     * silently self-drops every peer frame (selfdrop==ok). Make the flashed
     * addressing visible at every boot so the mismatch is caught in seconds. */
    n = snprintf(hw_msg, sizeof(hw_msg),
                 "BOOT: node=%u src=%u dst=%u\r\n",
                 (unsigned int)OPENVLC_TRANSCEIVER_NODE,
                 (unsigned int)OPENVLC_TX_SRC_ADDR,
                 (unsigned int)OPENVLC_TX_DST_ADDR);
    if (n > 0)
      HAL_UART_Transmit(&huart3, (uint8_t *)hw_msg, (uint16_t)n, 100);
    if (HAL_GetDEVID() != 0x483u ||
        HAL_RCC_GetSysClockFreq() != 384000000u ||
        timclk != OPENVLC_STM32_TX_TIMER_HZ)
    {
      const uint8_t fail_msg[] = "BOOT: H723 device/clock mismatch\r\n";
      HAL_UART_Transmit(&huart3, (uint8_t *)fail_msg,
                        sizeof(fail_msg) - 1u, 100);
      Error_Handler();
    }
  }
  {
    char reset_msg[48];
    int n = snprintf(reset_msg, sizeof(reset_msg), "BOOT: reset_flags=0x%08lx\r\n",
                     (unsigned long)openvlc_reset_flags);
    if (n > 0)
    {
      HAL_UART_Transmit(&huart3, (uint8_t *)reset_msg, (uint16_t)n, 100);
    }
  }
#if OPENVLC_BOOT_STAGE >= 1
  openvlc_stm32_init();
#endif
#if OPENVLC_BOOT_STAGE >= 2
  if (openvlc_stm32_memory_selftest() != 0)
  {
    const uint8_t mem_fail_msg[] = "BOOT: RAM selftest failed\r\n";
    HAL_UART_Transmit(&huart3, (uint8_t *)mem_fail_msg, sizeof(mem_fail_msg) - 1u, 100);
    Error_Handler();
  }
  {
    const uint8_t mem_ok_msg[] = "BOOT: RAM selftest OK\r\n";
    HAL_UART_Transmit(&huart3, (uint8_t *)mem_ok_msg, sizeof(mem_ok_msg) - 1u, 100);
  }
  {
    const uint8_t start_msg[] = "BOOT: starting COMP1/DAC1/TIM2-IC\r\n";
    HAL_UART_Transmit(&huart3, (uint8_t *)start_msg, sizeof(start_msg) - 1u, 100);
  }
  if (openvlc_stm32_start() != 0)
  {
    const uint8_t fail_msg[] = "BOOT: openvlc_stm32_start failed\r\n";
    HAL_UART_Transmit(&huart3, (uint8_t *)fail_msg, sizeof(fail_msg) - 1u, 100);
    Error_Handler();
  }
  if (openvlc_transceiver_host_init() != 0)
  {
    const uint8_t fail_msg[] = "BOOT: TX/UART RX init failed\r\n";
    HAL_UART_Transmit(&huart3, (uint8_t *)fail_msg, sizeof(fail_msg) - 1u, 100);
    Error_Handler();
  }
  {
    const uint8_t ok_msg[] = "BOOT: COMP1/DAC1/TIM2-IC started\r\n";
    HAL_UART_Transmit(&huart3, (uint8_t *)ok_msg, sizeof(ok_msg) - 1u, 100);
  }
#endif

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    static uint32_t last_alive_ms = 0;

#if OPENVLC_RX_DEEP_DEBUG_LOG
    openvlc_main_phase = 1u;
    openvlc_main_loop_count++;
    openvlc_main_last_tick = HAL_GetTick();
#endif
#if OPENVLC_BOOT_STAGE >= 3
#if OPENVLC_RX_DEEP_DEBUG_LOG
    openvlc_main_phase = 2u;
#endif
    openvlc_stm32_rx_comparator_poll();
#endif
#if OPENVLC_RX_DEEP_DEBUG_LOG
    openvlc_main_phase = 3u;
#endif
    openvlc_transceiver_host_poll();
    /* Release the TX idle keep-alive when the host stops sending. */
    openvlc_stm32_tx_idle_poll();
#if OPENVLC_TX_HW_DIAG
    openvlc_stm32_tx_diag_poll();
#endif
#if OPENVLC_BOOT_STAGE >= 2
#if OPENVLC_RX_DEEP_DEBUG_LOG
    openvlc_main_phase = 4u;
#endif
#if OPENVLC_RX_DIAG_LOG || OPENVLC_COMP_THRESHOLD_AUTO || \
    OPENVLC_COMP_DUTY_SERVO
    openvlc_stm32_debug_poll(HAL_GetTick());
#endif
#if OPENVLC_TX_DIAG_LOG
    openvlc_transceiver_host_log(HAL_GetTick());
#endif
#endif
#if OPENVLC_RX_DEEP_DEBUG_LOG
    openvlc_main_phase = 5u;
#endif
#if OPENVLC_RX_HOST_FORWARD
    openvlc_stm32_host_poll();
#endif
    if (HAL_GetTick() - last_alive_ms >= 1000u)
    {
      HAL_GPIO_TogglePin(OPENVLC_HEARTBEAT_GPIO_PORT,
                        OPENVLC_HEARTBEAT_GPIO_PIN);
#if OPENVLC_ALIVE_LOG
#if OPENVLC_RX_HOST_FORWARD
      last_alive_ms = HAL_GetTick();
      openvlc_platform_log("ALIVE\r\n");
#else
      const uint8_t alive_msg[] = "ALIVE\r\n";
      last_alive_ms = HAL_GetTick();
      HAL_UART_Transmit(&huart3, (uint8_t *)alive_msg, sizeof(alive_msg) - 1u, 10);
#endif
#else
      last_alive_ms = HAL_GetTick();
#endif
    }
#if OPENVLC_RX_DEEP_DEBUG_LOG
    openvlc_main_phase = 6u;
#endif
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  /*
   * VOS1 supports up to 400 MHz CPU; we run 384 MHz so no SYSCFG overdrive
   * (VOS0) is needed. SCALE3 only allowed up to 200 MHz.
   */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  __HAL_RCC_PLL_PLLSOURCE_CONFIG(RCC_PLLSOURCE_HSI);

  /*
   * HSI 64 MHz -> PLL1 -> SYSCLK 384 MHz.
   *   ref = 64 / PLLM(4)   = 16 MHz
   *   VCO = ref * PLLN(24) = 384 MHz (wide VCO range)
   *   SYSCLK = VCO / PLLP(1) = 384 MHz
   * 384 was chosen so the APB timer clock becomes 192 MHz, an exact multiple
   * of both 16 MHz (budget-100) and 64 MHz (budget-50) TIM2 capture ticks.
   */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;            /* HSI = 64 MHz */
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 24;
  RCC_OscInitStruct.PLL.PLLP = 1;
  RCC_OscInitStruct.PLL.PLLQ = 8;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;    /* 8-16 MHz input */
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;    /* 192-836 MHz VCO */
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /*
   * CPU 384 MHz, HCLK (AXI/AHB) 192 MHz, all APB = 96 MHz so every APB timer
   * kernel clock is 192 MHz. Flash needs 2 wait states at 192 MHz / VOS1.
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;    /* CPU = 384 MHz */
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;      /* HCLK = 192 MHz */
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;     /* 96 MHz */
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;     /* 96 MHz, TIM = 192 */
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;     /* 96 MHz, TIM = 192 */
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;     /* 96 MHz */

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  openvlc_fault_capture(
      6u, NULL, (uint32_t)(uintptr_t)__builtin_return_address(0));
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
