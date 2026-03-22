/**
 * @file    sensor_hal.h
 * @brief   HAL interface — SHT30 (I2C1) + BMP388 (I2C1) + UART DMA debug
 *
 * Cả hai sensor dùng chung I2C1 bus (PB6/PB7).
 * Địa chỉ khác nhau nên không xung đột:
 *   SHT30 : ADDR = GND → 0x44
 *   BMP388: SDO  = GND → 0x76  (SDO=VCC → 0x77)
 *
 * ┌────────────────────────────────────────────────────────────────────┐
 * │               Kết nối phần cứng BluePill (STM32F103C8)            │
 * ├──────────────┬──────────────────────┬──────────────────────────────┤
 * │  SHT30       │  I2C1 chung          │                              │
 * │  VDD         │  3.3V                │                              │
 * │  GND         │  GND                 │                              │
 * │  ADDR        │  GND  →  addr 0x44   │                              │
 * │  SCL         │  PB6                 │  Pull-up 4.7kΩ → 3.3V      │
 * │  SDA         │  PB7                 │  Pull-up 4.7kΩ → 3.3V      │
 * ├──────────────┼──────────────────────┼──────────────────────────────┤
 * │  BMP388      │  I2C1 chung          │                              │
 * │  VDD/VDDIO   │  3.3V                │                              │
 * │  GND         │  GND                 │                              │
 * │  SDO         │  GND  →  addr 0x76   │  SDO=VCC → 0x77            │
 * │  CSB         │  3.3V → chọn I2C    │  CSB=HIGH bắt buộc I2C mode │
 * │  SCL         │  PB6  (cùng bus)     │                              │
 * │  SDA         │  PB7  (cùng bus)     │                              │
 * ├──────────────┼──────────────────────┼──────────────────────────────┤
 * │  USART1      │  PA9=TX  PA10=RX     │  115200-8N1, DMA TX Ch4     │
 * └──────────────┴──────────────────────┴──────────────────────────────┘
 *
 * CubeMX settings:
 *   I2C1  : Standard Mode 100 kHz, No Own Address, Disable General Call
 *   USART1: Async 115200, DMA TX → DMA1 Channel 4, Mode=Normal
 *   SYS   : Timebase source = TIM1
 *   Linker: thêm -lm (powf dùng trong tính độ cao)
 */

#ifndef SENSOR_HAL_H
#define SENSOR_HAL_H

#include "stm32f1xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Extern handles (CubeMX sinh ra) ─────────────────────────────────── */
extern I2C_HandleTypeDef  hi2c1;
extern UART_HandleTypeDef huart1;
extern DMA_HandleTypeDef  hdma_usart1_tx;

/* ─── Timeout ──────────────────────────────────────────────────────────── */
#define SENSOR_I2C_TIMEOUT_MS   500U

/* ══════════════════════════════════════════════════════════════════════════
 *  SHT30 HAL — I2C1, lệnh 16-bit (MSB trước)
 * ══════════════════════════════════════════════════════════════════════════ */
uint8_t sht30_iic_init(void);
uint8_t sht30_iic_deinit(void);
uint8_t sht30_iic_write_address16(uint8_t addr, uint16_t reg,
                                  uint8_t *buf, uint16_t len);
uint8_t sht30_iic_read_address16(uint8_t addr, uint16_t reg,
                                 uint8_t *buf, uint16_t len);

/* ══════════════════════════════════════════════════════════════════════════
 *  BMP388 HAL — I2C1, thanh ghi 8-bit chuẩn
 *  Địa chỉ: SDO=GND → 0x76 (đã dịch trái 1 bit = 0xEC)
 *           SDO=VCC → 0x77 (đã dịch trái 1 bit = 0xEE)
 * ══════════════════════════════════════════════════════════════════════════ */
uint8_t bmp388_iic_init(void);
uint8_t bmp388_iic_deinit(void);
uint8_t bmp388_iic_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len);
uint8_t bmp388_iic_write(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len);

/* ══════════════════════════════════════════════════════════════════════════
 *  Hàm dùng chung
 * ══════════════════════════════════════════════════════════════════════════ */
/*
 * khởi tạo DMA
 */
void Sensor_UART_DMA_Init(void);

/** Delay ms (wrapper HAL_Delay) */
void sensor_delay_ms(uint32_t ms);

/**
 * @brief In chuỗi debug ra UART1 qua DMA (non-blocking).
 *        Dùng double-buffer để tránh ghi đè khi DMA chưa hoàn thành.
 */
void sensor_debug_print(const char *const fmt, ...);

/** Callback alert SHT30 (nhiệt độ / độ ẩm vượt ngưỡng) */
void sht30_receive_callback(uint16_t type);

/** Callback ngắt BMP388 (nếu dùng chân INT) */
void bmp388_receive_callback(uint8_t type);

uint8_t bmp388_spi_init_dummy(void);
uint8_t bmp388_spi_deinit_dummy(void);
uint8_t bmp388_spi_read_dummy(uint8_t reg, uint8_t *buf, uint16_t len);
uint8_t bmp388_spi_write_dummy(uint8_t reg, uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif
#endif /* SENSOR_HAL_H */
