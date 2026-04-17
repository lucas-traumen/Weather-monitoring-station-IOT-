/**
 * @file    sensor_hal.h
 * @brief   Hardware Abstraction Layer - Giao tiếp I2C, UART và GPIO
 * @note    Phiên bản Ultra-Low Power: Cắt bỏ DMA, dùng Polling an toàn với IWDG.
 */

#ifndef SENSOR_HAL_H
#define SENSOR_HAL_H

#include "stm32f1xx_hal.h"
#include "sensors.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── ĐỊNH NGHĨA CHÂN ĐIỀU KHIỂN LORA E32 ────────────────────────────────── */
// Bác sửa lại Port và Pin cho đúng với mạch thực tế trên CubeMX nhé
#define LORA_AUX_PORT   GPIOA
#define LORA_AUX_PIN    GPIO_PIN_4

#define LORA_M0_PORT    GPIOA
#define LORA_M0_PIN     GPIO_PIN_5
#define LORA_M1_PORT    GPIOA
#define LORA_M1_PIN     GPIO_PIN_6

#define I2C_TIMEOUT_MS  50  // Timeout I2C chống treo bus

/* ─── HÀM THỜI GIAN VÀ DEBUG ─────────────────────────────────────────────── */
void sensor_delay_ms(uint32_t ms);
void sensor_debug_print(const char *const fmt, ...);

/* ─── HÀM ĐIỀU KHIỂN LORA E32 ────────────────────────────────────────────── */
/**
 * @brief Bắn gói tin ra LoRa và chờ đến khi sóng vô tuyến thực sự bay đi.
 * Tích hợp "đá chó" IWDG trong lúc chờ chân AUX để chống reset.
 */
void sensor_lora_transmit(uint8_t *payload, uint16_t len);

/**
 * @brief Ép LoRa vào chế độ Sleep (M0=1, M1=1) để ăn dòng 2uA
 */
void sensor_lora_sleep(void);

/* ─── BINDING CHO THƯ VIỆN SHT30 (I2C1 - Lệnh 16-bit) ────────────────────── */
uint8_t sht30_iic_init(void);
uint8_t sht30_iic_deinit(void);
uint8_t sht30_iic_write_address16(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len);
uint8_t sht30_iic_read_address16(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len);
void    sht30_receive_callback(uint16_t type);

/* ─── BINDING CHO THƯ VIỆN BMP388 (I2C1 - Thanh ghi 8-bit) ───────────────── */
uint8_t bmp388_iic_init(void);
uint8_t bmp388_iic_deinit(void);
uint8_t bmp388_iic_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len);
uint8_t bmp388_iic_write(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len);
void    bmp388_receive_callback(uint8_t type);

/* ─── HÀM DUMMY CHO BMP388 (Ép kiểu I2C, vô hiệu hóa SPI) ────────────────── */
uint8_t bmp388_spi_init_dummy(void);
uint8_t bmp388_spi_deinit_dummy(void);
uint8_t bmp388_spi_read_dummy(uint8_t reg, uint8_t *buf, uint16_t len);
uint8_t bmp388_spi_write_dummy(uint8_t reg, uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_HAL_H */


