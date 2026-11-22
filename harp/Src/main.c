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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <math.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define TIM_CLK  64000000UL   // Clock Configuration의 APB1 Timer clocks 값
#define FS       32000U       // 소리 샘플링 주파수
#define AUDIO_N  256U         // 소리 버퍼 길이
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* ===== 4줄 하프 테스트: 줄마다 다른 음 (F103RB, 폴링) ===== */
#define NCH 4
static const uint8_t ch[NCH] = {0, 1, 4, 8};                                  // A0 A1 A2 A3
static const float note_hz[NCH] = {261.63f, 293.66f, 329.63f, 392.00f};      // 도 레 미 솔

static uint16_t audio_buf[AUDIO_N];     // DMA → TIM3->CCR1 (0~255, 무음 128)
static int8_t   sine_tab[256];
static uint32_t phase[NCH], phase_inc[NCH];
static int32_t  vol[NCH];               // 줄마다 현재 음량 0~32
volatile uint8_t gate[NCH];             // 1이면 그 줄 소리 남

static void sound_test_init(void) {
  for (int i = 0; i < 256; i++)
    sine_tab[i] = (int8_t)(127.0f * sinf(6.2831853f * i / 256.0f));
  for (int c = 0; c < NCH; c++)
    phase_inc[c] = (uint32_t)(note_hz[c] * 4294967296.0 / FS);
}

// DMA가 버퍼 반을 다 쓰면 호출: 끊긴 줄들의 음을 더해서 화음
static void fill_audio(uint16_t *dst, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    int32_t s = 0;
    for (int c = 0; c < NCH; c++) {
      int32_t target = gate[c] ? 32 : 0;       // 4줄 다 울려도 0~255 안에 들어오게 32
      if (vol[c] < target) vol[c]++; else if (vol[c] > target) vol[c]--;
      phase[c] += phase_inc[c];
      s += (sine_tab[phase[c] >> 24] * vol[c]) / 128;
    }
    dst[i] = (uint16_t)(128 + s);
  }
}

static void audio_init(void) {
  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
  RCC->AHBENR  |= RCC_AHBENR_DMA1EN;
  RCC->APB1ENR |= RCC_APB1ENR_TIM3EN | RCC_APB1ENR_TIM4EN;

  GPIOA->CRL = (GPIOA->CRL & ~(0xFU << (6 * 4))) | (0xBU << (6 * 4));   // PA6(D12) = TIM3_CH1
  GPIOA->CRL = (GPIOA->CRL & ~(0xFU << (5 * 4))) | (0x2U << (5 * 4));   // PA5(LD2) = 출력

  TIM3->PSC = 0; TIM3->ARR = 255; TIM3->CCR1 = 128;
  TIM3->CCMR1 = (6U << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;
  TIM3->CCER  = TIM_CCER_CC1E;
  TIM3->CR1   = TIM_CR1_ARPE | TIM_CR1_CEN;

  for (uint32_t i = 0; i < AUDIO_N; i++) audio_buf[i] = 128;

  DMA1_Channel7->CCR = 0;
  DMA1->IFCR = DMA_IFCR_CGIF7;
  DMA1_Channel7->CPAR  = (uint32_t)&TIM3->CCR1;
  DMA1_Channel7->CMAR  = (uint32_t)audio_buf;
  DMA1_Channel7->CNDTR = AUDIO_N;
  DMA1_Channel7->CCR = DMA_CCR_DIR | DMA_CCR_MSIZE_0 | DMA_CCR_PSIZE_0
                     | DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_PL_1
                     | DMA_CCR_HTIE | DMA_CCR_TCIE;
  NVIC_SetPriority(DMA1_Channel7_IRQn, 1);
  NVIC_EnableIRQ(DMA1_Channel7_IRQn);
  DMA1_Channel7->CCR |= DMA_CCR_EN;

  TIM4->PSC = 0; TIM4->ARR = TIM_CLK / FS - 1;
  TIM4->DIER = TIM_DIER_UDE;
  TIM4->CR1  = TIM_CR1_CEN;
}

void DMA1_Channel7_IRQHandler(void) {
  uint32_t isr = DMA1->ISR;
  if (isr & DMA_ISR_HTIF7) { DMA1->IFCR = DMA_IFCR_CHTIF7; fill_audio(&audio_buf[0], AUDIO_N / 2); }
  if (isr & DMA_ISR_TCIF7) { DMA1->IFCR = DMA_IFCR_CTCIF7; fill_audio(&audio_buf[AUDIO_N / 2], AUDIO_N / 2); }
}

/* ---- 광센서 4개: CPU가 차례로 읽음 (테스트용, DMA 아님) ---- */
static void adc_init(void) {
  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_ADC1EN;
  RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_ADCPRE) | RCC_CFGR_ADCPRE_DIV6;
  GPIOA->CRL &= ~((0xFU << (0 * 4)) | (0xFU << (1 * 4)) | (0xFU << (4 * 4)));  // PA0 PA1 PA4
  GPIOB->CRL &= ~(0xFU << (0 * 4));                                          // PB0
  ADC1->CR2 = ADC_CR2_ADON;
  HAL_Delay(1);
  ADC1->CR2 |= ADC_CR2_RSTCAL; while (ADC1->CR2 & ADC_CR2_RSTCAL);
  ADC1->CR2 |= ADC_CR2_CAL;    while (ADC1->CR2 & ADC_CR2_CAL);
  uint32_t smpr2 = 0;
  for (int c = 0; c < NCH; c++) smpr2 |= 7U << (3 * ch[c]);
  ADC1->SMPR2 = smpr2;
  ADC1->SQR1  = 0;
  ADC1->CR2   = ADC_CR2_ADON | ADC_CR2_EXTTRIG | (7U << ADC_CR2_EXTSEL_Pos);
}

static uint16_t adc_read(uint8_t channel) {
  ADC1->SQR3 = channel;
  ADC1->CR2 |= ADC_CR2_SWSTART;
  while (!(ADC1->SR & ADC_SR_EOC));
  return (uint16_t)ADC1->DR;
}

static uint16_t adc_read_avg(uint8_t channel) {    // 8번 평균
  uint32_t s = 0;
  for (int k = 0; k < 8; k++) s += adc_read(channel);
  return (uint16_t)(s / 8);
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

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
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  sound_test_init();
  audio_init();
  adc_init();

  for (int k = 0; k < 6; k++) { GPIOA->ODR ^= (1U << 5); HAL_Delay(150); }   // 시작 표시 3번
  GPIOA->ODR &= ~(1U << 5);

  #define CUT_ON   150     // 기준보다 이만큼 어두워지면 "끊김"
  #define CUT_OFF   90     // 이 안으로 돌아오면 "복귀"
  char msg[100];
  uint16_t lit[NCH];
  uint32_t sum[NCH] = {0};
  for (int k = 0; k < 50; k++) {                   // 1초: 손 대지 말 것
    for (int c = 0; c < NCH; c++) sum[c] += adc_read_avg(ch[c]);
    HAL_Delay(20);
  }
  for (int c = 0; c < NCH; c++) lit[c] = (uint16_t)(sum[c] / 50);
  uint32_t last_print = 0, track = 0;
  int drop[NCH];
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	    uint8_t any = 0;
	    for (int c = 0; c < NCH; c++) {
	      uint16_t v = adc_read_avg(ch[c]);
	      drop[c] = (int)lit[c] - (int)v;                       // 어두워진 만큼(+)
	      if (!gate[c] && drop[c] > CUT_ON)  gate[c] = 1;
	      if (gate[c]  && drop[c] < CUT_OFF) gate[c] = 0;
	      if (!gate[c] && track == 0) {                         // 안 가린 줄만 기준값이 천천히 따라감
	        if (v > lit[c]) lit[c]++; else if (v < lit[c]) lit[c]--;
	      }
	      any |= gate[c];
	    }
	    if (++track >= 20) track = 0;
	    if (any) GPIOA->ODR |= (1U << 5); else GPIOA->ODR &= ~(1U << 5);   // LD2 = 하나라도 끊김

	    if (HAL_GetTick() - last_print >= 100) {
	      last_print = HAL_GetTick();
	      int n = snprintf(msg, sizeof msg, "S2(A1)  now=%4d  lit=%4u  drop=%5d  cut=%u\r\n",
	                       (int)lit[1] - drop[1], lit[1], drop[1], gate[1]);
	      HAL_UART_Transmit(&huart2, (uint8_t *)msg, n, 100);
	    }
	    HAL_Delay(5);
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

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL16;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LD2_Pin */
  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
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
