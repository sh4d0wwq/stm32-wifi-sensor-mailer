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
#include "ssd1306.h"
#include "wifi_mail.h"
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */
#define LINK_RX_SIZE 128
#define LINK_BAUD          9600U   /* bit-banged UART from the TX board (PA8, TIM1 input capture) */
#define LINK_TIMEOUT_MS    3000U   /* no valid frame for this long -> show "NO LINK" */
#define WIFI_REJOIN_MS     30000U  /* retry period after a failed Wi-Fi join */
#define MAIL_PERIOD_MS     60000U  /* interval between e-mails with readings */
static volatile uint8_t link_rx[LINK_RX_SIZE];
static volatile uint16_t link_head;
static uint16_t link_tail;
static volatile uint32_t link_count;
static uint16_t link_bit;
static int oled_ok;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */
static int parse_line(const char *s, int *pot, int *ldr, int *tmp);
static void oled_show(const char *l0, const char *l1, const char *l2, const char *l3);
static void link_rx_init(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static int read_num(const char **ps, int *whole, int *frac)
{
  const char *s = *ps;
  int w = 0;
  if (*s < '0' || *s > '9')
  {
    return 0;
  }
  while (*s >= '0' && *s <= '9')
  {
    w = w * 10 + (*s - '0');
    s++;
  }
  if (*s != '.')
  {
    return 0;
  }
  s++;
  if (*s < '0' || *s > '9')
  {
    return 0;
  }
  *whole = w;
  *frac = *s - '0';
  *ps = s + 1;
  return 1;
}

static int parse_line(const char *s, int *pot, int *ldr, int *tmp)
{
  int pw, pf, lw, lf, tw, tf;
  if (*s++ != 'P' || !read_num(&s, &pw, &pf) || *s++ != ';')
  {
    return 0;
  }
  if (*s++ != 'L' || !read_num(&s, &lw, &lf) || *s++ != ';')
  {
    return 0;
  }
  if (*s++ != 'T' || !read_num(&s, &tw, &tf))
  {
    return 0;
  }
  *pot = pw * 10 + pf;
  *ldr = lw * 10 + lf;
  *tmp = tw * 10 + tf;
  return 1;
}

static void oled_show(const char *l0, const char *l1, const char *l2, const char *l3)
{
  if (!oled_ok)
  {
    return;
  }
  SSD1306_Clear();
  SSD1306_DrawString(0, 0, l0);
  SSD1306_DrawString(0, 1, l1);
  SSD1306_DrawString(0, 2, l2);
  SSD1306_DrawString(0, 3, l3);
  SSD1306_Update();
}

static void oled_readings(const char *title, int pot, int ldr, int tmp)
{
  char a[22], b[22], c[22];

  snprintf(a, sizeof(a), "POT %d.%d%%", pot / 10, pot % 10);
  snprintf(b, sizeof(b), "LDR %d.%d%%", ldr / 10, ldr % 10);
  snprintf(c, sizeof(c), "TEMP %d.%dC", tmp / 10, tmp % 10);
  oled_show(title, a, b, c);
}

static void tim_wait(uint16_t t0, uint16_t target)
{
  uint32_t spins = 40000U;

  while ((uint16_t)(TIM1->CNT - t0) < target && spins-- != 0U)
  {
  }
}

static void link_rx_init(void)
{
  GPIO_InitTypeDef gpio = {0};
  uint32_t timclk;

  gpio.Pin = GPIO_PIN_8;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &gpio);

  timclk = HAL_RCC_GetPCLK2Freq();
  if ((RCC->CFGR & RCC_CFGR_PPRE2) != RCC_CFGR_PPRE2_DIV1)
  {
    timclk *= 2U;
  }
  link_bit = (uint16_t)(timclk / LINK_BAUD);

  __HAL_RCC_TIM1_CLK_ENABLE();
  TIM1->CR1 = 0;
  TIM1->PSC = 0;
  TIM1->ARR = 0xFFFF;
  TIM1->CNT = 0;
  TIM1->CCMR1 = TIM_CCMR1_CC1S_0 | (3U << 4);
  TIM1->CCER = TIM_CCER_CC1P | TIM_CCER_CC1E;
  TIM1->DIER = TIM_DIER_CC1IE;
  TIM1->BDTR = TIM_BDTR_MOE;
  TIM1->EGR = TIM_EGR_UG;
  TIM1->SR = 0;
  TIM1->CR1 = TIM_CR1_CEN;
  HAL_NVIC_SetPriority(TIM1_CC_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(TIM1_CC_IRQn);
  HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
}

static void link_rx_byte(uint16_t t0)
{
  uint16_t bit = link_bit;
  uint8_t value = 0;
  uint16_t next;
  int i;

  tim_wait(t0, (uint16_t)(bit + bit / 2U));
  for (i = 0; i < 8; i++)
  {
    if ((GPIOA->IDR & GPIO_PIN_8) != 0U)
    {
      value |= (uint8_t)(1U << i);
    }
    tim_wait(t0, (uint16_t)(bit + bit / 2U + (uint16_t)(i + 1) * bit));
  }
  next = (uint16_t)((link_head + 1U) % LINK_RX_SIZE);
  if (next != link_tail)
  {
    link_rx[link_head] = value;
    link_head = next;
  }
  link_count++;
  TIM1->SR = (uint16_t)~(TIM_SR_CC1IF | TIM_SR_CC1OF);
}

void TIM1_CC_IRQHandler(void)
{
  uint16_t t0;

  if ((TIM1->SR & TIM_SR_CC1IF) == 0U)
  {
    return;
  }
  t0 = (uint16_t)TIM1->CCR1;
  TIM1->DIER &= (uint16_t)~TIM_DIER_CC1IE;
  link_rx_byte(t0);
  TIM1->DIER |= TIM_DIER_CC1IE;
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
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  link_rx_init();
  WiFi_StartRx();
  oled_ok = (SSD1306_Init() == 0);
  oled_show("LAB 4", "WIFI ...", "", "");
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    static char line[48];
    static uint8_t line_len;
    static int have;
    static int stale;
    static int pot, ldr, tmp;
    static uint32_t last_ms;
    static uint32_t oled_try;
    static uint32_t wait_ms;
    static int join_state;
    static uint32_t last_join;
    static uint32_t last_mail;
    static char title[22] = "HY-M302";
    uint16_t head;

    if (!oled_ok && (HAL_GetTick() - oled_try) > 1000U)
    {
      oled_try = HAL_GetTick();
      oled_ok = (SSD1306_Init() == 0);
    }

    if (join_state == 0 || (join_state == 2 && (HAL_GetTick() - last_join) >= WIFI_REJOIN_MS))
    {
      join_state = WiFi_Join() ? 1 : 2;
      last_join = HAL_GetTick();
      last_mail = 0U;
    }

    head = link_head;
    while (link_tail != head)
    {
      uint8_t byte = link_rx[link_tail];

      link_tail = (uint16_t)((link_tail + 1U) % LINK_RX_SIZE);
      if (byte == '\n')
      {
        line[line_len] = '\0';
        line_len = 0;
        if (parse_line(line, &pot, &ldr, &tmp))
        {
          have = 1;
          stale = 0;
          last_ms = HAL_GetTick();
          oled_readings(title, pot, ldr, tmp);
        }
      }
      else if (byte != '\r' && line_len < sizeof(line) - 1)
      {
        line[line_len++] = (char)byte;
      }
      else if (byte != '\r')
      {
        line_len = 0;
      }
    }

    if (!have && (HAL_GetTick() - wait_ms) > 500U)
    {
      char info[22];
      char raw[22];
      int i;
      int n = line_len;

      wait_ms = HAL_GetTick();
      if (n > 21)
      {
        n = 21;
      }
      for (i = 0; i < n; i++)
      {
        raw[i] = (line[i] >= 32 && line[i] < 127) ? line[i] : '.';
      }
      raw[n] = '\0';
      snprintf(info, sizeof(info), "B %lu", (unsigned long)link_count);
      oled_show("WAIT UART", info, n ? raw : "NO EDGE", "");
    }
    if (have && !stale && (HAL_GetTick() - last_ms) > LINK_TIMEOUT_MS)
    {
      stale = 1;
      oled_readings("NO LINK", pot, ldr, tmp);
    }
    if (have && join_state == 1 && (last_mail == 0U || (HAL_GetTick() - last_mail) >= MAIL_PERIOD_MS))
    {
      if (WiFi_SendReadings(pot, ldr, tmp))
      {
        snprintf(title, sizeof(title), "MAIL OK");
      }
      else
      {
        snprintf(title, sizeof(title), "MAIL %s", WiFi_FailText());
      }
      last_mail = HAL_GetTick() | 1U;
      oled_readings(stale ? "NO LINK" : title, pot, ldr, tmp);
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);

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
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(OLED_SCL_GPIO_Port, OLED_SCL_Pin|OLED_SDA_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : OLED_SCL_Pin OLED_SDA_Pin */
  GPIO_InitStruct.Pin = OLED_SCL_Pin|OLED_SDA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(OLED_SCL_GPIO_Port, &GPIO_InitStruct);

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
