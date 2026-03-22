/**
 * @file    sensors.h
 * @brief   Quản lý SHT30 (môi trường ngoài) + BMP388 (bo mạch / áp suất)
 *
 *  ┌───────────┬────────────────────────┬────────────────────────────────┐
 *  │  Sensor   │  Vị trí               │  Dữ liệu                       │
 *  ├───────────┼────────────────────────┼────────────────────────────────┤
 *  │  SHT30    │  Không khí bên ngoài  │  Nhiệt độ ngoài + Độ ẩm       │
 *  ├───────────┼────────────────────────┼────────────────────────────────┤
 *  │  BMP388   │  Trên bo mạch / trong │  Nhiệt độ PCB + Áp suất       │
 *  │           │  vỏ thiết bị          │  + Độ cao ước tính             │
 *  └───────────┴────────────────────────┴────────────────────────────────┘
 */

#ifndef SENSORS_H
#define SENSORS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Struct dữ liệu ─────────────────────────────────────────────────────── */

typedef struct
{
    float   temperature_c;   /**< Nhiệt độ môi trường ngoài (°C) */
    float   humidity_pct;    /**< Độ ẩm tương đối (%)            */
    uint8_t valid;           /**< 1 = hợp lệ, 0 = lỗi           */
} SHT30_Data_t;

typedef struct
{
    float   temperature_c;   /**< Nhiệt độ bo mạch (°C)          */
    float   pressure_pa;     /**< Áp suất (Pa)                    */
    float   pressure_hpa;    /**< Áp suất (hPa)                   */
    float   altitude_m;      /**< Độ cao so với mực nước biển (m) */
    uint8_t valid;           /**< 1 = hợp lệ, 0 = lỗi           */
} BMP388_Data_t;

/** Struct tổng hợp — dùng để truyền toàn bộ dữ liệu một lần */
typedef struct
{
    SHT30_Data_t  env;       /**< Môi trường ngoài (SHT30)        */
    BMP388_Data_t board;     /**< Bo mạch (BMP388)                 */
} SensorData_t;

/* ─── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief  Khởi tạo cả hai sensor.
 * @return 0=OK | bit0=SHT30 lỗi | bit1=BMP388 lỗi
 */
uint8_t Sensors_Init(void);

/** Đọc SHT30 (nhiệt độ + độ ẩm). @return 0=OK, 1=lỗi */
uint8_t Sensors_ReadSHT30(SHT30_Data_t *out);

/** Đọc BMP388 (nhiệt độ + áp suất + độ cao). @return 0=OK, 1=lỗi */
uint8_t Sensors_ReadBMP388(BMP388_Data_t *out);

/** Đọc tất cả vào SensorData_t */
void Sensors_ReadAll(SensorData_t *data);

/** In toàn bộ dữ liệu qua UART debug */
void Sensors_Print(const SensorData_t *data);

/** Tắt cả hai sensor */
void Sensors_Deinit(void);

#ifdef __cplusplus
}
#endif
#endif /* SENSORS_H */
