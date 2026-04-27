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

#include "sensor_hal.h"
#include "api.h"
#include "crypto_aead.h"
#include "sensors.h"
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define TPS_SYNC_PORT   GPIOA
#define TPS_SYNC_PIN    GPIO_PIN_6

#define SENSOR_POWER_SETTLE_MS      20U
#define SENSOR_MEASURE_WAIT_MS      150U


#define APP_RTC_HEARTBEAT_SEC       10U
#define APP_TX_PERIOD_SEC           30U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;
I2C_HandleTypeDef hi2c2;

IWDG_HandleTypeDef hiwdg;

RTC_HandleTypeDef hrtc;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
static const uint8_t ASCON_SECRET_KEY[16] = {
    0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
    0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C
};

static uint16_t g_frame_counter = 0U;
static volatile uint8_t  g_rtc_tick_due = 0U;
static volatile uint32_t g_rtc_alarm_count = 0U;
static uint32_t g_tx_elapsed_sec = 0U;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_I2C2_Init(void);
static void MX_IWDG_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_RTC_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void debug_dump_hex(const char *prefix, const uint8_t *data, uint16_t len)
{
    uint16_t i;

    if ((prefix == NULL) || (data == NULL)) {
        return;
    }

    sensor_debug_print("%s (%uB):", prefix, (unsigned)len);
    for (i = 0U; i < len; i++) {
        sensor_debug_print(" %02X", data[i]);
    }
    sensor_debug_print("\r\n");
}

static uint8_t lora_cfg_is_target(const uint8_t cfg[6])
{
    return (cfg[0] == 0xC0 &&
            cfg[1] == 0x00 &&
            cfg[2] == 0x17 &&
            cfg[3] == 0x3A &&
            cfg[4] == 0x17 &&
            cfg[5] == 0x44);
}
static uint8_t Edge_Crypto_Pack(const SensorData_t *raw_data,
                                uint16_t counter,
                                LoRaTxFrame_t *tx_frame)
{
    uint8_t plaintext[9];
    uint8_t ascon_output[25];
    uint8_t nonce[16] = {0};
    unsigned long long clen = 0ULL;
    int ret;

    if ((raw_data == NULL) || (tx_frame == NULL)) {
        return SENSOR_ERR;
    }

    memset(tx_frame, 0, sizeof(*tx_frame));
    memcpy(plaintext, raw_data, sizeof(plaintext));

    nonce[0] = (uint8_t)(counter >> 8);
    nonce[1] = (uint8_t)(counter & 0xFFU);
    nonce[2] = 0x57U;
    nonce[3] = 0x53U;
    nonce[4] = 0x4EU;
    nonce[5] = 0x31U;

    ret = crypto_aead_encrypt(
        ascon_output, &clen,
        plaintext, sizeof(plaintext),
        NULL, 0,
        NULL,
        nonce,
        ASCON_SECRET_KEY
    );

    if ((ret != 0) || (clen < 13ULL)) {
        sensor_debug_print("[ASCON] Encrypt FAILED, skip TX\r\n");
        return SENSOR_ERR;
    }

    tx_frame->frame_counter = counter;
    memcpy(tx_frame->ciphertext, &ascon_output[0], sizeof(tx_frame->ciphertext));
    memcpy(tx_frame->mac_tag, &ascon_output[9], sizeof(tx_frame->mac_tag));

    return SENSOR_OK;
}
static void App_Delay_With_Watchdog(uint32_t ms)
{
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < ms) {
        HAL_IWDG_Refresh(&hiwdg);
        HAL_Delay(50);
    }
}
static void App_RTC_Seconds_To_Time(uint32_t seconds_of_day, RTC_TimeTypeDef *time)
{
    seconds_of_day %= 86400U;

    memset(time, 0, sizeof(*time));

    time->Hours   = (uint8_t)(seconds_of_day / 3600U);
    seconds_of_day %= 3600U;

    time->Minutes = (uint8_t)(seconds_of_day / 60U);
    time->Seconds = (uint8_t)(seconds_of_day % 60U);
}

static uint8_t App_RTC_Get_Seconds_Of_Day(uint32_t *seconds_out)
{
    RTC_TimeTypeDef now_time = {0};
    RTC_DateTypeDef now_date = {0};

    if (seconds_out == NULL) {
        return SENSOR_ERR;
    }

    if (HAL_RTC_GetTime(&hrtc, &now_time, RTC_FORMAT_BIN) != HAL_OK) {
        return SENSOR_ERR;
    }

    /*
     * Important: HAL yêu cầu đọc date sau khi đọc time
     * để unlock shadow register.
     */
    if (HAL_RTC_GetDate(&hrtc, &now_date, RTC_FORMAT_BIN) != HAL_OK) {
        return SENSOR_ERR;
    }

    *seconds_out =
        ((uint32_t)now_time.Hours * 3600U) +
        ((uint32_t)now_time.Minutes * 60U) +
        ((uint32_t)now_time.Seconds);

    return SENSOR_OK;
}

static uint8_t App_RTC_Schedule_Next_Alarm(uint32_t after_seconds)
{
    RTC_AlarmTypeDef alarm = {0};
    RTC_TimeTypeDef alarm_time = {0};
    uint32_t now_seconds = 0U;
    uint32_t alarm_seconds = 0U;

    if (after_seconds == 0U) {
        after_seconds = 1U;
    }

    if (App_RTC_Get_Seconds_Of_Day(&now_seconds) != SENSOR_OK) {
        sensor_debug_print("[RTC] Get time FAILED\r\n");
        return SENSOR_ERR;
    }

    alarm_seconds = (now_seconds + after_seconds) % 86400U;
    App_RTC_Seconds_To_Time(alarm_seconds, &alarm_time);

    alarm.AlarmTime = alarm_time;
    alarm.Alarm = RTC_ALARM_A;

    /*
     * Không fail nếu alarm cũ chưa tồn tại.
     */
    (void)HAL_RTC_DeactivateAlarm(&hrtc, RTC_ALARM_A);

    if (HAL_RTC_SetAlarm_IT(&hrtc, &alarm, RTC_FORMAT_BIN) != HAL_OK) {
        sensor_debug_print("[RTC] Set alarm FAILED\r\n");
        return SENSOR_ERR;
    }

    sensor_debug_print(
        "[RTC] Next TX alarm in %lu sec at %02u:%02u:%02u\r\n",
        (unsigned long)after_seconds,
        alarm_time.Hours,
        alarm_time.Minutes,
        alarm_time.Seconds
    );

    return SENSOR_OK;
}

void HAL_RTC_AlarmAEventCallback(RTC_HandleTypeDef *hrtc_cb)
{
    if (hrtc_cb->Instance == RTC) {
    	g_rtc_tick_due  = 1U;
        g_rtc_alarm_count++;
    }
}
uint8_t init_status;
uint8_t tx_ok;
SensorData_t payload;
LoRaTxFrame_t tx_frame;

static void App_Run_One_Cycle(void)
{


    memset(&payload, 0, sizeof(payload));
    memset(&tx_frame, 0, sizeof(tx_frame));

    HAL_IWDG_Refresh(&hiwdg);

    sensor_debug_print("\r\n[SYS] ===== APP CYCLE START =====\r\n");

    App_Delay_With_Watchdog(SENSOR_POWER_SETTLE_MS);

    init_status = Sensors_Init_Hardware();
    sensor_debug_print("[SENSORS] Init status = 0x%02X\r\n", init_status);

    HAL_IWDG_Refresh(&hiwdg);

    Sensors_Trigger_All();
    App_Delay_With_Watchdog(SENSOR_MEASURE_WAIT_MS);

    Sensors_Collect_And_Pack(&payload);
    debug_dump_hex("[PAYLOAD]", (const uint8_t *)&payload, sizeof(payload));

    if (Edge_Crypto_Pack(&payload, g_frame_counter, &tx_frame) != SENSOR_OK) {
        sensor_debug_print("[SYS] Skip LoRa TX because crypto failed\r\n");
        sensor_lora_sleep();
        return;
    }

    debug_dump_hex("[FRAME]", (const uint8_t *)&tx_frame, sizeof(tx_frame));

    tx_ok = sensor_lora_transmit((const uint8_t *)&tx_frame, sizeof(tx_frame));
    if (tx_ok != SENSOR_OK) {
        sensor_debug_print("[SYS] LoRa TX FAILED for frame=%u\r\n", g_frame_counter);
        sensor_lora_sleep();
        return;
    }

    g_frame_counter++;

    sensor_lora_sleep();

    HAL_IWDG_Refresh(&hiwdg);

    sensor_debug_print("[SYS] ===== APP CYCLE END =====\r\n");
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
  MX_I2C1_Init();
  MX_I2C2_Init();
  MX_IWDG_Init();
  MX_USART2_UART_Init();
  MX_USART1_UART_Init();
  MX_RTC_Init();
  /* USER CODE BEGIN 2 */
  uint8_t lora_cfg[6];

  if (sensor_lora_read_config(lora_cfg) == SENSOR_OK) {
      debug_dump_hex("[LORA_CFG]", lora_cfg, sizeof(lora_cfg));

      if (!lora_cfg_is_target(lora_cfg)) {
          sensor_debug_print("[SYS] E32 config mismatch, writing target config\r\n");

          if (sensor_lora_write_default_config() != SENSOR_OK) {
              sensor_debug_print("[SYS] E32 config write FAILED\r\n");
          } else {
              sensor_debug_print("[SYS] E32 config write OK\r\n");
          }
      } else {
          sensor_debug_print("[SYS] E32 config already OK\r\n");
      }
  } else {
      sensor_debug_print("[SYS] E32 config read FAILED, try write once\r\n");

      if (sensor_lora_write_default_config() != SENSOR_OK) {
          sensor_debug_print("[SYS] E32 config write FAILED\r\n");
      }
  }

  sensor_lora_sleep();
  HAL_IWDG_Refresh(&hiwdg);
  g_rtc_tick_due = 0U;
  g_rtc_alarm_count = 0U;
  g_tx_elapsed_sec = 0U;

  /*
   * Alarm đầu tiên phải là heartbeat, không phải TX period.
   * Vì IWDG ~25s, không được chờ 30s ngay từ đầu.
   */
  if (App_RTC_Schedule_Next_Alarm(APP_RTC_HEARTBEAT_SEC) != SENSOR_OK) {
      sensor_debug_print("[SYS] RTC heartbeat start FAILED\r\n");
  } else {
      sensor_debug_print("[SYS] RTC heartbeat started\r\n");
  }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	  if (g_rtc_tick_due != 0U) {
	          g_rtc_tick_due = 0U;

	          /*
	           * Lập lịch alarm kế tiếp NGAY khi vừa thức dậy.
	           * Làm vậy để thời gian chạy App_Run_One_Cycle()
	           * không làm trôi lịch RTC.
	           */
	          if (App_RTC_Schedule_Next_Alarm(APP_RTC_HEARTBEAT_SEC) != SENSOR_OK) {
	              sensor_debug_print("[SYS] RTC reschedule FAILED\r\n");
	          }

	          /*
	           * Đây mới là nơi đá IWDG chính thức khi RTC heartbeat chạy.
	           * Nếu main bị treo, nó sẽ không tới đây và IWDG sẽ reset.
	           */
	          HAL_IWDG_Refresh(&hiwdg);

	          sensor_debug_print(
	              "\r\n[RTC] Heartbeat #%lu, tx_elapsed=%lu/%lu sec\r\n",
	              (unsigned long)g_rtc_alarm_count,
	              (unsigned long)g_tx_elapsed_sec,
	              (unsigned long)APP_TX_PERIOD_SEC
	          );

	          g_tx_elapsed_sec += APP_RTC_HEARTBEAT_SEC;

	          if (g_tx_elapsed_sec >= APP_TX_PERIOD_SEC) {
	              g_tx_elapsed_sec = 0U;

	              sensor_debug_print("[RTC] TX due\r\n");

	              App_Run_One_Cycle();

	              /*
	               * Sau khi hoàn tất một chu kỳ đo + mã hóa + gửi LoRa,
	               * refresh thêm lần nữa.
	               */
	              HAL_IWDG_Refresh(&hiwdg);
	          }
	      }

	      /*
	       * __WFI vẫn có thể bị SysTick đánh thức,
	       * nhưng vì ta KHÔNG refresh IWDG ở đầu vòng lặp nữa,
	       * SysTick không còn vô tình nuôi watchdog.
	       */
	      __WFI();
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
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
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
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_RTC;
  PeriphClkInit.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief I2C2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C2_Init(void)
{

  /* USER CODE BEGIN I2C2_Init 0 */

  /* USER CODE END I2C2_Init 0 */

  /* USER CODE BEGIN I2C2_Init 1 */

  /* USER CODE END I2C2_Init 1 */
  hi2c2.Instance = I2C2;
  hi2c2.Init.ClockSpeed = 100000;
  hi2c2.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c2.Init.OwnAddress1 = 0;
  hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c2.Init.OwnAddress2 = 0;
  hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C2_Init 2 */

  /* USER CODE END I2C2_Init 2 */

}

/**
  * @brief IWDG Initialization Function
  * @param None
  * @retval None
  */
static void MX_IWDG_Init(void)
{

  /* USER CODE BEGIN IWDG_Init 0 */

  /* USER CODE END IWDG_Init 0 */

  /* USER CODE BEGIN IWDG_Init 1 */

  /* USER CODE END IWDG_Init 1 */
  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
  hiwdg.Init.Reload = 3906;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IWDG_Init 2 */

  /* USER CODE END IWDG_Init 2 */

}

/**
  * @brief RTC Initialization Function
  * @param None
  * @retval None
  */
static void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef DateToUpdate = {0};

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.AsynchPrediv = RTC_AUTO_1_SECOND;
  hrtc.Init.OutPut = RTC_OUTPUTSOURCE_ALARM;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN Check_RTC_BKUP */

  /* USER CODE END Check_RTC_BKUP */

  /** Initialize RTC and set the Time and Date
  */
  sTime.Hours = 0x0;
  sTime.Minutes = 0x0;
  sTime.Seconds = 0x0;

  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  DateToUpdate.WeekDay = RTC_WEEKDAY_MONDAY;
  DateToUpdate.Month = RTC_MONTH_JANUARY;
  DateToUpdate.Date = 0x1;
  DateToUpdate.Year = 0x0;

  if (HAL_RTC_SetDate(&hrtc, &DateToUpdate, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RTC_Init 2 */

  /* USER CODE END RTC_Init 2 */

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
  HAL_GPIO_WritePin(LED_DEBUG_GPIO_Port, LED_DEBUG_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, LORA_M0_Pin|LORA_M1_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_DEBUG_Pin */
  GPIO_InitStruct.Pin = LED_DEBUG_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_DEBUG_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LORA_AUX_Pin */
  GPIO_InitStruct.Pin = LORA_AUX_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(LORA_AUX_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : LORA_M0_Pin LORA_M1_Pin */
  GPIO_InitStruct.Pin = LORA_M0_Pin|LORA_M1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

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

#ifdef  USE_FULL_ASSERT
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
