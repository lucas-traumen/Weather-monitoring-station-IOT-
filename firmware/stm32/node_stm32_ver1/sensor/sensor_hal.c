/**
 * @file    sensor_hal.c
 * @brief   Triển khai HAL - Giao tiếp thô với ngoại vi STM32 (Polling Mode)
 */

#include "sensor_hal.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Extern ngoại vi từ main.c do CubeMX sinh ra */
extern I2C_HandleTypeDef  hi2c1;
extern UART_HandleTypeDef huart1; // USART1: Debug Print
extern UART_HandleTypeDef huart2; // USART2: LoRa E32
extern IWDG_HandleTypeDef hiwdg;  // Phải có để đá chó khi chờ LoRa

/* ====================================================================
 * 1. QUẢN LÝ THỜI GIAN VÀ DEBUG
 * ==================================================================== */
void sensor_delay_ms(uint32_t ms)
{
    HAL_Delay(ms);
}

void sensor_debug_print(const char *const fmt, ...)
{
    char buf[128];
    va_list args;

    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0) {
        // In ra màn hình bằng Polling, tự thoát nếu kẹt cáp quá 100ms
        HAL_UART_Transmit(&huart1, (uint8_t*)buf, len, 100);
    }
}

/* ====================================================================
 * 2. ĐIỀU KHIỂN LORA E32 (Tích hợp chống treo AUX)
 * ==================================================================== */
void sensor_lora_transmit(uint8_t *payload, uint16_t len)
{
    // Bước 1: Kéo M0=0, M1=0 để đưa E32 về chế độ Normal (Truyền nhận)
    HAL_GPIO_WritePin(LORA_M0_PORT, LORA_M0_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LORA_M1_PORT, LORA_M1_PIN, GPIO_PIN_RESET);

    // Bước 2: Chờ module E32 sẵn sàng (AUX bật lên HIGH)
    while(HAL_GPIO_ReadPin(LORA_AUX_PORT, LORA_AUX_PIN) == GPIO_PIN_RESET) {
        HAL_IWDG_Refresh(&hiwdg); // Trấn an chó Watchdog
    }

    // Bước 3: Đẩy gói tin 15-Byte qua UART vào bộ đệm của E32
    HAL_UART_Transmit(&huart2, payload, len, 100);

    // Bước 4: Chờ E32 phát xong dữ liệu vô tuyến ra không trung (AUX lại lên HIGH)
    // Cực kỳ quan trọng: Nếu cắt điện ngủ lúc AUX đang LOW, gói tin sẽ rớt giữa chừng!
    while(HAL_GPIO_ReadPin(LORA_AUX_PORT, LORA_AUX_PIN) == GPIO_PIN_RESET) {
        HAL_IWDG_Refresh(&hiwdg);
    }

    sensor_debug_print("[LORA] Pushed %d bytes over the air.\r\n", len);
}

void sensor_lora_sleep(void)
{
    // Kéo M0=1, M1=1 để ép E32 vào chế độ Sleep Mode (Dòng rò 2uA)
    HAL_GPIO_WritePin(LORA_M0_PORT, LORA_M0_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LORA_M1_PORT, LORA_M1_PIN, GPIO_PIN_SET);
}

/* ====================================================================
 * 3. GIAO TIẾP I2C CHO SHT30 (Lệnh 16-bit)
 * ==================================================================== */
uint8_t sht30_iic_init(void)   { return 0; } // CubeMX đã init
uint8_t sht30_iic_deinit(void) { return 0; }

uint8_t sht30_iic_write_address16(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len)
{
    uint8_t tx[16];
    if (len > 14) return 1; // SHT30 không bao giờ ghi quá dài

    tx[0] = (uint8_t)(reg >> 8);
    tx[1] = (uint8_t)(reg & 0xFF);
    if (buf && len > 0) memcpy(&tx[2], buf, len);

    HAL_StatusTypeDef s = HAL_I2C_Master_Transmit(&hi2c1, addr, tx, 2 + len, I2C_TIMEOUT_MS);
    return (s == HAL_OK) ? 0 : 1;
}

uint8_t sht30_iic_read_address16(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len)
{
    uint8_t cmd[2];
    cmd[0] = (uint8_t)(reg >> 8);
    cmd[1] = (uint8_t)(reg & 0xFF);

    // 1. Gửi lệnh
    if (HAL_I2C_Master_Transmit(&hi2c1, addr, cmd, 2, I2C_TIMEOUT_MS) != HAL_OK) return 1;
    // 2. Đọc kết quả
    if (HAL_I2C_Master_Receive(&hi2c1, addr, buf, len, I2C_TIMEOUT_MS) != HAL_OK) return 1;

    return 0;
}

void sht30_receive_callback(uint16_t type) { (void)type; } // Không dùng ngắt

/* ====================================================================
 * 4. GIAO TIẾP I2C CHO BMP388 (Thanh ghi 8-bit chuẩn)
 * ==================================================================== */
uint8_t bmp388_iic_init(void)   { return 0; }
uint8_t bmp388_iic_deinit(void) { return 0; }

uint8_t bmp388_iic_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef s = HAL_I2C_Mem_Read(&hi2c1, addr, reg, I2C_MEMADD_SIZE_8BIT, buf, len, I2C_TIMEOUT_MS);
    return (s == HAL_OK) ? 0 : 1;
}

uint8_t bmp388_iic_write(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef s = HAL_I2C_Mem_Write(&hi2c1, addr, reg, I2C_MEMADD_SIZE_8BIT, buf, len, I2C_TIMEOUT_MS);
    return (s == HAL_OK) ? 0 : 1;
}

void bmp388_receive_callback(uint8_t type) { (void)type; }

/* ====================================================================
 * 5. HÀM DUMMY CHO SPI CỦA BMP388
 * ==================================================================== */
uint8_t bmp388_spi_init_dummy(void) { return 0; }
uint8_t bmp388_spi_deinit_dummy(void) { return 0; }
uint8_t bmp388_spi_read_dummy(uint8_t reg, uint8_t *buf, uint16_t len) { return 1; }
uint8_t bmp388_spi_write_dummy(uint8_t reg, uint8_t *buf, uint16_t len) { return 1; }

