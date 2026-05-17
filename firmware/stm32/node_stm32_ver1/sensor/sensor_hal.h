/**
 * @file    sensor_hal.h
 * @brief   HAL mỏng cho cảm biến, debug UART và LoRa E32.
 */

#ifndef SENSOR_HAL_H
#define SENSOR_HAL_H

#include "main.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_AUX_PORT        LORA_AUX_GPIO_Port
#define LORA_AUX_PIN         LORA_AUX_Pin
#define LORA_M0_PORT         LORA_M0_GPIO_Port
#define LORA_M0_PIN          LORA_M0_Pin
#define LORA_M1_PORT         LORA_M1_GPIO_Port
#define LORA_M1_PIN          LORA_M1_Pin

#define I2C_TIMEOUT_MS       100U
#define LORA_AUX_TIMEOUT_MS  2000U
#define LORA_MODE_SETTLE_MS  5U

typedef enum {
    SENSOR_OK = 0,
    SENSOR_ERR = 1
} sensor_status_t;
typedef struct __attribute__((packed)) {
    uint32_t frame_counter;
    uint8_t  ciphertext[10];
    uint8_t  mac_tag[4];
} LoRaTxFrame_t;


void sensor_delay_ms(uint32_t ms);
void sensor_debug_print(const char *fmt, ...);

void sensor_lora_normal(void);
void sensor_lora_sleep(void);
uint8_t sensor_lora_transmit(const uint8_t *payload, uint16_t len);
uint8_t sensor_lora_write_default_config(void);
uint8_t sensor_lora_read_config(uint8_t out_cfg[6]);

uint8_t sht30_iic_init(void);
uint8_t sht30_iic_deinit(void);
uint8_t sht30_iic_write_address16(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len);
uint8_t sht30_iic_read_address16(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len);
void    sht30_receive_callback(uint16_t type);

uint8_t bmp388_iic_init(void);
uint8_t bmp388_iic_deinit(void);
uint8_t bmp388_iic_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len);
uint8_t bmp388_iic_write(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len);
void    bmp388_receive_callback(uint8_t type);

uint8_t bmp388_spi_init_dummy(void);
uint8_t bmp388_spi_deinit_dummy(void);
uint8_t bmp388_spi_read_dummy(uint8_t reg, uint8_t *buf, uint16_t len);
uint8_t bmp388_spi_write_dummy(uint8_t reg, uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_HAL_H */
