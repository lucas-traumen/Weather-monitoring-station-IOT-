/**
 * @file    sensors.h
 * @brief   Quản lý toàn bộ hệ thống Cảm biến (Hardware Math & Edge Packing)
 * - SHT30 (Nhiệt độ, Độ ẩm)
 * - BMP388 (Áp suất, Nhiệt độ bo mạch)
 * - INA219 (Đo áp pin - Chờ tích hợp)
 * @note    Cấu trúc dữ liệu được ép chặt thành 9 Byte để tối ưu Payload LoRa.
 */

#ifndef SENSORS_H
#define SENSORS_H

#include "stm32f1xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── CẤU TRÚC GÓI TIN ĐÃ "ÉP CÂN" (Đúng 9 Bytes) ────────────────────────── */
/* Lưu ý: Sử dụng __attribute__((packed)) để vô hiệu hóa padding của trình biên dịch ARM,
 * đảm bảo kích thước struct chính xác tuyệt đối là 9 byte trong bộ nhớ. */
typedef struct __attribute__((packed)) {
    int16_t  env_temp;    // 2 Byte: (Nhiệt độ SHT30 * 100)           | VD: 25.34°C -> 2534
    uint16_t env_hum;     // 2 Byte: (Độ ẩm SHT30 * 100)              | VD: 65.12%  -> 6512
    uint16_t air_press;   // 2 Byte: ((Áp suất hPa - 900.0) * 100)    | VD: 1013.25 -> 11325
    int8_t   board_temp;  // 1 Byte: (Nhiệt độ mạch BMP388 nguyên °C) | VD: 32.5°C  -> 32
    uint8_t  batt_volt;   // 1 Byte: (Áp Pin * 10)                    | VD: 4.1V    -> 41
    uint8_t  health_flag; // 1 Byte: Cờ báo lỗi phần cứng             | 0x00 = Mạch khỏe mạnh
} SensorData_t;

/* Cờ báo lỗi phần cứng (Dùng phép toán bitwise OR để ghép cờ) */
#define ERR_SHT30   (1 << 0)  // Lỗi 0x01: Không đọc được SHT30
#define ERR_BMP388  (1 << 1)  // Lỗi 0x02: Không đọc được BMP388
#define ERR_INA219  (1 << 2)  // Lỗi 0x04: Không đọc được INA219

/* ─── API QUẢN LÝ CẢM BIẾN CHÍNH ─────────────────────────────────────────── */

/**
 * @brief  Khởi tạo và cấu hình mảng cảm biến.
 * - SHT30: Cấu hình High Repeatability.
 * - BMP388: Cấu hình OSR 16x (Áp suất), 2x (Nhiệt độ), Sleep Mode.
 * @return Cờ trạng thái lỗi (0x00 nếu thành công toàn bộ).
 */
uint8_t Sensors_Init_Hardware(void);

/**
 * @brief  Đánh thức và ra lệnh đo đồng loạt cho các cảm biến.
 * Hàm này là Non-blocking (Không chờ đợi).
 * - SHT30: Bắn lệnh Single-Shot.
 * - BMP388: Chuyển sang Forced Mode.
 */
void Sensors_Trigger_All(void);

/**
 * @brief  Thu hoạch dữ liệu từ cảm biến, áp dụng ép kiểu về số nguyên
 * và đóng gói vào cấu trúc 9-Byte.
 * Lưu ý: Bắt buộc phải gọi hàm này sau khi gọi Trigger_All() ít nhất 130ms.
 * @param  payload Con trỏ tới struct SensorData_t để lưu kết quả.
 */
void Sensors_Collect_And_Pack(SensorData_t *payload);

#ifdef __cplusplus
}
#endif

#endif /* SENSORS_H */

