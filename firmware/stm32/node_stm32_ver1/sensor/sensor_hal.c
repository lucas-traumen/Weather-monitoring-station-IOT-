/**
 * @file    sensor_hal.c
 * @brief   HAL tối giản cho STM32 weather node.
 */

#include "sensor_hal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern I2C_HandleTypeDef  hi2c1;
extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;

#define LORA_UART_CFG_BAUD      9600U
#define LORA_UART_RUN_BAUD      115200U

#define LORA_CFG_CMD_SAVE       0xC0U
#define LORA_CFG_CMD_READ       0xC1U

/* Mirror với config bên ESP32:
 * speed  = E32_UART_8N1 | E32_BAUD_115200 | E32_AIR_24K
 * option = E32_TRANS_TRANSPARENT | E32_IO_PUSH_PULL | E32_WAKE_250MS | E32_FEC_ON | E32_PWR_30DBM
 */
#define LORA_CFG_ADDH           0x00U
#define LORA_CFG_ADDL           0x17U
#define LORA_CFG_SPEED          0x3AU
#define LORA_CFG_CHAN           0x17U
#define LORA_CFG_OPTION         0x44U



static uint8_t sensor_lora_wait_aux_high(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();

    while (HAL_GPIO_ReadPin(LORA_AUX_PORT, LORA_AUX_PIN) == GPIO_PIN_RESET) {
        if ((HAL_GetTick() - t0) >= timeout_ms) {
            return SENSOR_ERR;
        }
    }

    return SENSOR_OK;
}

void sensor_delay_ms(uint32_t ms)
{
    HAL_Delay(ms);
}

void sensor_debug_print(const char *fmt, ...)
{
    char buf[160];
    va_list args;
    int len;

    va_start(args, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len <= 0) {
        return;
    }

    if (len > (int)sizeof(buf)) {
        len = (int)sizeof(buf);
    }

    (void)HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)len, 200);
}

void sensor_lora_normal(void)
{
    HAL_GPIO_WritePin(LORA_M0_PORT, LORA_M0_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LORA_M1_PORT, LORA_M1_PIN, GPIO_PIN_RESET);
    HAL_Delay(LORA_MODE_SETTLE_MS);
}

void sensor_lora_sleep(void)
{
    HAL_GPIO_WritePin(LORA_M0_PORT, LORA_M0_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LORA_M1_PORT, LORA_M1_PIN, GPIO_PIN_SET);
    HAL_Delay(LORA_MODE_SETTLE_MS);
}

static uint8_t sensor_lora_uart2_set_baud(uint32_t baud)
{
    if (huart2.Init.BaudRate == baud) {
        return SENSOR_OK;
    }

    if (HAL_UART_DeInit(&huart2) != HAL_OK) {
        return SENSOR_ERR;
    }

    huart2.Init.BaudRate = baud;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart2) != HAL_OK) {
        return SENSOR_ERR;
    }

    return SENSOR_OK;
}
uint8_t sensor_lora_write_default_config(void)
{
    const uint8_t cfg[6] = {
        LORA_CFG_CMD_SAVE,
        LORA_CFG_ADDH,
        LORA_CFG_ADDL,
        LORA_CFG_SPEED,
        LORA_CFG_CHAN,
        LORA_CFG_OPTION
    };

    sensor_debug_print("[LORA] Write config start\r\n");

    /* Mode 3 / Sleep-Config */
    sensor_lora_sleep();

    if (sensor_lora_uart2_set_baud(LORA_UART_CFG_BAUD) != SENSOR_OK) {
        sensor_debug_print("[LORA] UART2 switch to 9600 failed\r\n");
        return SENSOR_ERR;
    }

    if (sensor_lora_wait_aux_high(LORA_AUX_TIMEOUT_MS) != SENSOR_OK) {
        sensor_debug_print("[LORA] AUX timeout before CFG write\r\n");
        return SENSOR_ERR;
    }

    if (HAL_UART_Transmit(&huart2, (uint8_t *)cfg, sizeof(cfg), 500) != HAL_OK) {
        sensor_debug_print("[LORA] CFG TX failed\r\n");
        return SENSOR_ERR;
    }

    if (sensor_lora_wait_aux_high(LORA_AUX_TIMEOUT_MS) != SENSOR_OK) {
        sensor_debug_print("[LORA] AUX timeout after CFG write\r\n");
        return SENSOR_ERR;
    }

    if (sensor_lora_uart2_set_baud(LORA_UART_RUN_BAUD) != SENSOR_OK) {
        sensor_debug_print("[LORA] UART2 restore 115200 failed\r\n");
        return SENSOR_ERR;
    }

    sensor_lora_normal();

    if (sensor_lora_wait_aux_high(LORA_AUX_TIMEOUT_MS) != SENSOR_OK) {
        sensor_debug_print("[LORA] AUX timeout after return normal\r\n");
        return SENSOR_ERR;
    }

    sensor_debug_print("[LORA] Config write OK: C0 00 17 3A 17 44\r\n");
    return SENSOR_OK;
}
uint8_t sensor_lora_read_config(uint8_t out_cfg[6])
{
    const uint8_t cmd[3] = { LORA_CFG_CMD_READ, LORA_CFG_CMD_READ, LORA_CFG_CMD_READ };

    if (out_cfg == NULL) {
        return SENSOR_ERR;
    }

    sensor_lora_sleep();

    if (sensor_lora_uart2_set_baud(LORA_UART_CFG_BAUD) != SENSOR_OK) {
        sensor_debug_print("[LORA] UART2 switch to 9600 failed\r\n");
        return SENSOR_ERR;
    }

    if (sensor_lora_wait_aux_high(LORA_AUX_TIMEOUT_MS) != SENSOR_OK) {
        sensor_debug_print("[LORA] AUX timeout before CFG read\r\n");
        return SENSOR_ERR;
    }

    if (HAL_UART_Transmit(&huart2, (uint8_t *)cmd, sizeof(cmd), 500) != HAL_OK) {
        sensor_debug_print("[LORA] CFG read cmd TX failed\r\n");
        return SENSOR_ERR;
    }

    if (HAL_UART_Receive(&huart2, out_cfg, 6U, 500) != HAL_OK) {
        sensor_debug_print("[LORA] CFG read RX failed\r\n");
        return SENSOR_ERR;
    }

    if (sensor_lora_uart2_set_baud(LORA_UART_RUN_BAUD) != SENSOR_OK) {
        sensor_debug_print("[LORA] UART2 restore 115200 failed\r\n");
        return SENSOR_ERR;
    }

    sensor_lora_normal();

    if (sensor_lora_wait_aux_high(LORA_AUX_TIMEOUT_MS) != SENSOR_OK) {
        sensor_debug_print("[LORA] AUX timeout after CFG read\r\n");
        return SENSOR_ERR;
    }

    return SENSOR_OK;
}

uint8_t sensor_lora_transmit(const uint8_t *payload, uint16_t len)
{
    HAL_StatusTypeDef hal_ret;

    if ((payload == NULL) || (len == 0U)) {
        sensor_debug_print("[LORA] Invalid TX buffer\r\n");
        return SENSOR_ERR;
    }

    sensor_lora_normal();

    if (sensor_lora_wait_aux_high(LORA_AUX_TIMEOUT_MS) != SENSOR_OK) {
        sensor_debug_print("[LORA] AUX timeout before TX\r\n");
        return SENSOR_ERR;
    }

    hal_ret = HAL_UART_Transmit(&huart2, (uint8_t *)payload, len, 500);
    if (hal_ret != HAL_OK) {
        sensor_debug_print("[LORA] UART TX failed: %d\r\n", (int)hal_ret);
        return SENSOR_ERR;
    }
   // HAL_UART_Transmit(&huart2, (uint8_t*)"UART2", strlen("UART2")-1, 500);

    if (sensor_lora_wait_aux_high(LORA_AUX_TIMEOUT_MS) != SENSOR_OK) {
        sensor_debug_print("[LORA] AUX timeout after TX\r\n");
        return SENSOR_ERR;
    }

    sensor_debug_print("[LORA] Sent %u bytes over UART2\r\n", (unsigned)len);
    return SENSOR_OK;
}

uint8_t sht30_iic_init(void)
{
    return SENSOR_OK;
}

uint8_t sht30_iic_deinit(void)
{
    return SENSOR_OK;
}

uint8_t sht30_iic_write_address16(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len)
{
    uint8_t tx[18];
    HAL_StatusTypeDef ret;

    if (len > 16U) {
        return SENSOR_ERR;
    }

    tx[0] = (uint8_t)(reg >> 8);
    tx[1] = (uint8_t)(reg & 0xFFU);
    if ((buf != NULL) && (len > 0U)) {
        memcpy(&tx[2], buf, len);
    }

    ret = HAL_I2C_Master_Transmit(&hi2c1, addr, tx, (uint16_t)(2U + len), I2C_TIMEOUT_MS);
    if (ret != HAL_OK) {
        sensor_debug_print("[SHT30] I2C write fail: err=0x%08lX\r\n", hi2c1.ErrorCode);
        return SENSOR_ERR;
    }

    return SENSOR_OK;
}

uint8_t sht30_iic_read_address16(uint8_t addr, uint16_t reg, uint8_t *buf, uint16_t len)
{
    uint8_t cmd[2];

    cmd[0] = (uint8_t)(reg >> 8);
    cmd[1] = (uint8_t)(reg & 0xFFU);

    if (HAL_I2C_Master_Transmit(&hi2c1, addr, cmd, 2U, I2C_TIMEOUT_MS) != HAL_OK) {
        sensor_debug_print("[SHT30] I2C cmd fail: err=0x%08lX\r\n", hi2c1.ErrorCode);
        return SENSOR_ERR;
    }

    if (HAL_I2C_Master_Receive(&hi2c1, addr, buf, len, I2C_TIMEOUT_MS) != HAL_OK) {
        sensor_debug_print("[SHT30] I2C read fail: err=0x%08lX\r\n", hi2c1.ErrorCode);
        return SENSOR_ERR;
    }

    return SENSOR_OK;
}

void sht30_receive_callback(uint16_t type)
{
    (void)type;
}

uint8_t bmp388_iic_init(void)
{
    return SENSOR_OK;
}

uint8_t bmp388_iic_deinit(void)
{
    return SENSOR_OK;
}

uint8_t bmp388_iic_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef ret;

    ret = HAL_I2C_Mem_Read(&hi2c1, addr, reg, I2C_MEMADD_SIZE_8BIT, buf, len, I2C_TIMEOUT_MS);
    if (ret != HAL_OK) {
        sensor_debug_print("[BMP388] I2C read fail: err=0x%08lX\r\n", hi2c1.ErrorCode);
        return SENSOR_ERR;
    }

    return SENSOR_OK;
}

uint8_t bmp388_iic_write(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef ret;

    ret = HAL_I2C_Mem_Write(&hi2c1, addr, reg, I2C_MEMADD_SIZE_8BIT, buf, len, I2C_TIMEOUT_MS);
    if (ret != HAL_OK) {
        sensor_debug_print("[BMP388] I2C write fail: err=0x%08lX\r\n", hi2c1.ErrorCode);
        return SENSOR_ERR;
    }

    return SENSOR_OK;
}

void bmp388_receive_callback(uint8_t type)
{
    (void)type;
}

uint8_t bmp388_spi_init_dummy(void)
{
    return SENSOR_OK;
}

uint8_t bmp388_spi_deinit_dummy(void)
{
    return SENSOR_OK;
}

uint8_t bmp388_spi_read_dummy(uint8_t reg, uint8_t *buf, uint16_t len)
{
    (void)reg;
    (void)buf;
    (void)len;
    return SENSOR_ERR;
}

uint8_t bmp388_spi_write_dummy(uint8_t reg, uint8_t *buf, uint16_t len)
{
    (void)reg;
    (void)buf;
    (void)len;
    return SENSOR_ERR;
}
