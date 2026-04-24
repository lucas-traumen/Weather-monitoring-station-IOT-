/**
 * @file    sensors.c
 * @brief   Điều phối SHT30 + BMP388 cho weather node.
 */

#include "sensors.h"
#include "sensor_hal.h"
#include "driver_sht30.h"
#include "driver_bmp388.h"
#include <string.h>

static sht30_handle_t  s_sht30;
static bmp388_handle_t s_bmp388;
static uint8_t         s_sht30_ok;
static uint8_t         s_bmp388_ok;

static uint16_t clamp_u16_from_float(float value, float scale)
{
    float scaled = value * scale;

    if (scaled <= 0.0f) {
        return 0U;
    }
    if (scaled >= 65535.0f) {
        return 65535U;
    }
    return (uint16_t)(scaled + 0.5f);
}

uint8_t Sensors_Init_Hardware(void)
{

	uint8_t ret = 0U;
    s_sht30_ok = 0U;
    s_bmp388_ok = 0U;

    DRIVER_SHT30_LINK_INIT(&s_sht30, sht30_handle_t);
    DRIVER_SHT30_LINK_IIC_INIT(&s_sht30,            sht30_iic_init);
    DRIVER_SHT30_LINK_IIC_DEINIT(&s_sht30,          sht30_iic_deinit);
    DRIVER_SHT30_LINK_IIC_READ_ADDRESS16(&s_sht30,  sht30_iic_read_address16);
    DRIVER_SHT30_LINK_IIC_WRITE_ADDRESS16(&s_sht30, sht30_iic_write_address16);
    DRIVER_SHT30_LINK_DELAY_MS(&s_sht30,            sensor_delay_ms);
    DRIVER_SHT30_LINK_DEBUG_PRINT(&s_sht30,         sensor_debug_print);
    DRIVER_SHT30_LINK_RECEIVE_CALLBACK(&s_sht30,    sht30_receive_callback);

    if ((sht30_set_addr_pin(&s_sht30, SHT30_ADDRESS_0) == 0) &&
        (sht30_init(&s_sht30) == 0) &&
        (sht30_set_repeatability(&s_sht30, SHT30_REPEATABILITY_HIGH) == 0))
    {
        s_sht30_ok = 1U;
        sensor_debug_print("[SENSORS] SHT30 init OK\r\n");
    }
    else
    {
        ret |= ERR_SHT30;
        sensor_debug_print("[SENSORS] SHT30 init FAILED\r\n");
    }

    DRIVER_BMP388_LINK_INIT(&s_bmp388, bmp388_handle_t);
    DRIVER_BMP388_LINK_IIC_INIT(&s_bmp388,         bmp388_iic_init);
    DRIVER_BMP388_LINK_IIC_DEINIT(&s_bmp388,       bmp388_iic_deinit);
    DRIVER_BMP388_LINK_IIC_READ(&s_bmp388,         bmp388_iic_read);
    DRIVER_BMP388_LINK_IIC_WRITE(&s_bmp388,        bmp388_iic_write);
    DRIVER_BMP388_LINK_DELAY_MS(&s_bmp388,         sensor_delay_ms);
    DRIVER_BMP388_LINK_DEBUG_PRINT(&s_bmp388,      sensor_debug_print);
    DRIVER_BMP388_LINK_RECEIVE_CALLBACK(&s_bmp388, bmp388_receive_callback);
    DRIVER_BMP388_LINK_SPI_INIT(&s_bmp388,         bmp388_spi_init_dummy);
    DRIVER_BMP388_LINK_SPI_DEINIT(&s_bmp388,       bmp388_spi_deinit_dummy);
    DRIVER_BMP388_LINK_SPI_READ(&s_bmp388,         bmp388_spi_read_dummy);
    DRIVER_BMP388_LINK_SPI_WRITE(&s_bmp388,        bmp388_spi_write_dummy);

    if ((bmp388_set_interface(&s_bmp388, BMP388_INTERFACE_IIC) == 0) &&
        (bmp388_set_addr_pin(&s_bmp388, BMP388_ADDRESS_ADO_HIGH) == 0) &&
        (bmp388_init(&s_bmp388) == 0) &&
        (bmp388_set_pressure(&s_bmp388, BMP388_BOOL_TRUE) == 0) &&
        (bmp388_set_temperature(&s_bmp388, BMP388_BOOL_TRUE) == 0) &&
        (bmp388_set_pressure_oversampling(&s_bmp388, BMP388_OVERSAMPLING_x16) == 0) &&
        (bmp388_set_temperature_oversampling(&s_bmp388, BMP388_OVERSAMPLING_x2) == 0) &&
        (bmp388_set_mode(&s_bmp388, BMP388_MODE_SLEEP_MODE) == 0))
    {
        s_bmp388_ok = 1U;
        sensor_debug_print("[SENSORS] BMP388 init OK\r\n");
    }
    else
    {
        ret |= ERR_BMP388;
        sensor_debug_print("[SENSORS] BMP388 init FAILED\r\n");
    }

    return ret;
}

void Sensors_Trigger_All(void)
{
    if (s_sht30_ok) {
        (void)sht30_set_reg(&s_sht30, 0x2400U);
    }

    if (s_bmp388_ok) {
        (void)bmp388_set_mode(&s_bmp388, BMP388_MODE_FORCED_MODE);
    }
}

void Sensors_Collect_And_Pack(SensorData_t *payload)
{
    if (payload == NULL) {
        return;
    }

    memset(payload, 0, sizeof(*payload));

    if (s_sht30_ok) {
        uint16_t t_raw;
        uint16_t h_raw;
        float tc;
        float hpct;

        if (sht30_single_read(&s_sht30, SHT30_BOOL_FALSE, &t_raw, &tc, &h_raw, &hpct) == 0) {
            float hum = hpct;
            if (hum < 0.0f) {
                hum = 0.0f;
            }
            if (hum > 100.0f) {
                hum = 100.0f;
            }

            payload->env_temp = (int16_t)(tc * 100.0f);
            payload->env_hum  = clamp_u16_from_float(hum, 100.0f);
        } else {
            payload->health_flag |= ERR_SHT30;
            payload->env_temp = (int16_t)0xFFFF;
            payload->env_hum = 0xFFFFU;
        }
    } else {
        payload->health_flag |= ERR_SHT30;
        payload->env_temp = (int16_t)0xFFFF;
        payload->env_hum = 0xFFFFU;
    }

    if (s_bmp388_ok) {
        uint32_t p_raw;
        uint32_t t_raw;
        float p_pa;
        float tc;

        if (bmp388_read_temperature_pressure(&s_bmp388, &t_raw, &tc, &p_raw, &p_pa) == 0) {
            float p_hpa = p_pa / 100.0f;

            if (p_hpa < 900.0f) {
                payload->air_press = 0U;
            } else {
                payload->air_press = clamp_u16_from_float(p_hpa - 900.0f, 100.0f);
            }

            if (tc > 127.0f) {
                payload->board_temp = 127;
            } else if (tc < -128.0f) {
                payload->board_temp = -128;
            } else {
                payload->board_temp = (int8_t)tc;
            }
        } else {
            payload->health_flag |= ERR_BMP388;
            payload->air_press = 0xFFFFU;
            payload->board_temp = (int8_t)0xFF;
        }
    } else {
        payload->health_flag |= ERR_BMP388;
        payload->air_press = 0xFFFFU;
        payload->board_temp = (int8_t)0xFF;
    }

    payload->batt_volt = 0U;

    sensor_debug_print("[DATA] T=%d H=%u P=%u BT=%d VB=%u HF=0x%02X\r\n",
                       payload->env_temp,
                       payload->env_hum,
                       payload->air_press,
                       payload->board_temp,
                       payload->batt_volt,
                       payload->health_flag);
}
