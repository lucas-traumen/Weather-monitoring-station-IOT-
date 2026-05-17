/**
 * @file    sensors.h
 * @brief   Gói gom dữ liệu cảm biến cho weather node.
 */

#ifndef SENSORS_H
#define SENSORS_H

#include "stm32f1xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) {
    int16_t  env_temp;    /* SHT30 temperature * 100 */
    uint16_t env_hum;     /* SHT30 humidity * 100 */
    uint16_t air_press;   /* (hPa - 900.0) * 100 */
    int16_t   board_temp;  /* BMP388 board temperature in degC */
    uint8_t  batt_volt;   /* Reserved for future VBAT measurement, currently 0 */
    uint8_t  health_flag; /* Bitmask error flags */
} SensorData_t;

#define ERR_SHT30   (1U << 0)
#define ERR_BMP388  (1U << 1)
#define ERR_VBAT    (1U << 2) /* reserved for later */

uint8_t Sensors_Init_Hardware(void);
void    Sensors_Trigger_All(void);
void    Sensors_Collect_And_Pack(SensorData_t *payload);

#ifdef __cplusplus
}
#endif

#endif /* SENSORS_H */


