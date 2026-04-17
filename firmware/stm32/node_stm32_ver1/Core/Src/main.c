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

#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct __attribute__((packed)) {
    uint16_t frame_counter;     // 2 Byte: Bộ đếm gói tin (Dùng để sinh Nonce)
    uint8_t  ciphertext[9];     // 9 Byte: Dữ liệu thời tiết đã bị xáo trộn
    uint8_t  mac_tag[4];        // 4 Byte: Chữ ký xác thực (Đã chặt đuôi)
} LoRaTxFrame_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define TPS_SYNC_PORT   GPIOB
#define TPS_SYNC_PIN    GPIO_PIN_0



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
const uint8_t ASCON_SECRET_KEY[16] = {
    0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
    0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C
};
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
/* ─── HÀM CÀI ĐẶT BÁO THỨC RTC (25 GIÂY) ─────────────────────────────────── */
void Set_RTC_Alarm_25s(void)
{
    RTC_TimeTypeDef sTime = {0};
    RTC_AlarmTypeDef sAlarm = {0};

    // Đọc thời gian hiện tại
    HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);

    // Tính toán số giây cho báo thức (Xử lý tràn 60 giây)
    uint8_t target_sec = sTime.Seconds + 25;
    if (target_sec >= 60) {
        target_sec -= 60;
    }

    sAlarm.AlarmTime.Seconds = target_sec;
    sAlarm.Alarm = RTC_ALARM_A;
    HAL_RTC_SetAlarm_IT(&hrtc, &sAlarm, RTC_FORMAT_BIN);
}

/* ─── HÀM MÃ HÓA VÀ CẮT GỌT ASCON-128a ───────────────────────────────────── */
void Edge_Crypto_Pack(SensorData_t *raw_data, uint16_t counter, LoRaTxFrame_t *tx_frame)
{
    uint8_t plaintext[9];
    memcpy(plaintext, raw_data, 9); // Ép struct 9-byte sang mảng byte

    uint8_t ascon_output[25];       // Chứa 9-Byte Cipher + 16-Byte Tag
    unsigned long long clen = 0;

    // Sinh Nonce ngẫu nhiên (16 Byte). Dùng Frame Counter đắp vào 2 byte đầu
    uint8_t nonce[16] = {0};
    nonce[0] = (uint8_t)(counter >> 8);
    nonce[1] = (uint8_t)(counter & 0xFF);

    // Bơm vào lò luyện ASCON
    crypto_aead_encrypt(
        ascon_output, &clen,
        plaintext, 9,           // Dữ liệu mộc
        NULL, 0,                // Không dùng Associated Data
        NULL,                   // Không dùng Secret Nonce
        nonce,                  // Public Nonce 16-byte
        ASCON_SECRET_KEY        // Khóa bí mật
    );

    // [TRUNCATION] Đóng gói vào Frame 15-Byte bay qua LoRa
    tx_frame->frame_counter = counter;
    memcpy(tx_frame->ciphertext, &ascon_output[0], 9); // Bốc 9 byte Ciphertext
    memcpy(tx_frame->mac_tag, &ascon_output[9], 4);    // Bốc đúng 4 byte Tag đầu, vứt bỏ 12 byte cuối
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
  HAL_PWR_EnableBkUpAccess();

    // Xóa cờ Wakeup nếu MCU vừa tỉnh dậy từ Standby
    if (__HAL_PWR_GET_FLAG(PWR_FLAG_WU) != RESET) {
        __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WU);
    }
    uint16_t wakeup_ticks = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR2);
      /* USER CODE END 2 */

      /* ====================================================================
       * PHA NGỦ NÔNG (ĐÁ CHÓ VÀ ĐI NGỦ TIẾP) - Chạy 23 lần
       * ==================================================================== */
      if (wakeup_ticks < 24)
      {
          wakeup_ticks++;
          HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR2, wakeup_ticks); // Lưu lại số lần đã thức

          HAL_IWDG_Refresh(&hiwdg);       // Đá chó ngay lập tức
          Set_RTC_Alarm_25s();            // Đặt báo thức 25s sau kêu tiếp

          HAL_PWR_EnterSTANDBYMode();     // Cắt điện CPU, đi ngủ sâu
      }

      /* ====================================================================
       * PHA THỨC SÂU (10 PHÚT ĐÃ ĐẾN) - Đo đạc & Phát sóng
       * ==================================================================== */
      else
      {
          // 1. Reset bộ đếm gà gật về 0 cho chu kỳ 10 phút tiếp theo
          HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR2, 0);

          // 2. ÉP NGUỒN VÀ KHỞI TẠO NGOẠI VI
          // Kéo chân PS/SYNC của TPS63020 lên HIGH để lấy điện áp 3.3V phẳng lỳ (PWM mode)
          HAL_GPIO_WritePin(TPS_SYNC_PORT, TPS_SYNC_PIN, GPIO_PIN_SET);
          sensor_delay_ms(5); // Chờ 5ms cho nguồn điện xả hết nhiễu

          MX_USART1_UART_Init(); // Bật UART1 in Log
          MX_USART2_UART_Init(); // Bật UART2 nói chuyện LoRa
          MX_I2C1_Init();        // Bật I2C1 nói chuyện Cảm biến

          sensor_debug_print("\r\n\r\n[SYS] --- 10-MIN CYCLE WAKEUP ---\r\n");

          // 3. THU THẬP DỮ LIỆU (HARDWARE MATH)
          Sensors_Init_Hardware(); // Khởi tạo mảng cảm biến
          Sensors_Trigger_All();   // Cấp lệnh Trigger (SHT30 Single-shot, BMP388 Forced)

          // Delay 130ms: Bắt CPU đứng đợi ngoại vi tính toán OSR 16x.
          // Vì thời gian này ngắn < 26s nên con IWDG vẫn chưa cắn.
          sensor_delay_ms(130);

          SensorData_t my_payload;
          Sensors_Collect_And_Pack(&my_payload); // Thu hoạch và ép kiểu về 9-Byte

          // 4. MÃ HÓA ASCON-128a
          uint16_t current_counter = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR1); // Lấy số thứ tự gói tin
          LoRaTxFrame_t tx_frame;
          Edge_Crypto_Pack(&my_payload, current_counter, &tx_frame); // Ra lò mảng 15-Byte

          sensor_debug_print("[ASCON] Encrypted 15-Byte frame generated.\r\n");

          // 5. PHÁT SÓNG QUA LORA E32
          // Hàm này đã tích hợp vòng lặp chờ cờ AUX và đá chó tự động!
          sensor_lora_transmit((uint8_t*)&tx_frame, sizeof(LoRaTxFrame_t));

          // Ép LoRa đi ngủ ngay lập tức (Dòng tiêu thụ < 2uA)
          sensor_lora_sleep();

          // 6. DỌN DẸP CHIẾN TRƯỜNG & CHUẨN BỊ NGỦ
          // Tăng số đếm gói tin để lần sau sinh Nonce mới
          HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, current_counter + 1);

          // Nhả chân TPS63020 về LOW để ép nguồn chạy chế độ PFM siêu tiết kiệm (30uA)
          HAL_GPIO_WritePin(TPS_SYNC_PORT, TPS_SYNC_PIN, GPIO_PIN_RESET);

          sensor_debug_print("[SYS] Cycle complete. Entering Standby...\r\n");

          // Lên giường đắp chăn!
          HAL_IWDG_Refresh(&hiwdg);       // Đá chó lần cuối
          Set_RTC_Alarm_25s();            // Hẹn giờ gà gật 25s
          HAL_PWR_EnterSTANDBYMode();     // Cắt điện toàn bộ CPU
      }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */


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
  hiwdg.Init.Reload = 4095;
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
