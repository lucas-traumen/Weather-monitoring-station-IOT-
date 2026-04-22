/**
 * @file    sensors.c
 * @brief   Triển khai quản lý Cảm biến (SHT30, BMP388)
 */

#include "sensors.h"
#include "sensor_hal.h"
#include "driver_sht30.h"
#include "driver_bmp388.h"
#include <string.h>

/* ─── Biến nội bộ quản lý trạng thái thiết bị ─── */
static sht30_handle_t  s_sht30;
static bmp388_handle_t s_bmp388;
static uint8_t         s_sht30_ok  = 0;
static uint8_t         s_bmp388_ok = 0;

/* ====================================================================
 * 1. KHỞI TẠO PHẦN CỨNG (Ép cấu hình siêu phân giải & Tiết kiệm pin)
 * ==================================================================== */
uint8_t Sensors_Init_Hardware(void)
{
    uint8_t ret = 0;

    /* ----------------------------------------------------------------
     * A. KHỞI TẠO SHT30 (I2C1 - Địa chỉ 0x44)
     * ---------------------------------------------------------------- */
    DRIVER_SHT30_LINK_INIT(&s_sht30, sht30_handle_t);
    DRIVER_SHT30_LINK_IIC_INIT(&s_sht30,            sht30_iic_init);
    DRIVER_SHT30_LINK_IIC_DEINIT(&s_sht30,          sht30_iic_deinit);
    DRIVER_SHT30_LINK_IIC_READ_ADDRESS16(&s_sht30,  sht30_iic_read_address16);
    DRIVER_SHT30_LINK_IIC_WRITE_ADDRESS16(&s_sht30, sht30_iic_write_address16);
    DRIVER_SHT30_LINK_DELAY_MS(&s_sht30,            sensor_delay_ms);
    DRIVER_SHT30_LINK_DEBUG_PRINT(&s_sht30,         sensor_debug_print);
    DRIVER_SHT30_LINK_RECEIVE_CALLBACK(&s_sht30,    sht30_receive_callback);

    if ((sht30_set_addr_pin(&s_sht30, SHT30_ADDRESS_0) == 0) &&
        (sht30_init(&s_sht30) == 0))
    {
        // Cấu hình: Độ chính xác cao nhất (Nhiễu cực thấp)
        sht30_set_repeatability(&s_sht30, SHT30_REPEATABILITY_HIGH);
        s_sht30_ok = 1;
        sensor_debug_print("[SENSORS] SHT30 Init OK (High Repeatability).\r\n");
    }
    else
    {
        s_sht30_ok = 0;
        ret |= ERR_SHT30;
        sensor_debug_print("[SENSORS] SHT30 Init FAILED!\r\n");
    }

    /* ----------------------------------------------------------------
     * B. KHỞI TẠO BMP388 (I2C1 - Địa chỉ 0x76)
     * ---------------------------------------------------------------- */
    DRIVER_BMP388_LINK_INIT(&s_bmp388, bmp388_handle_t);
    DRIVER_BMP388_LINK_IIC_INIT(&s_bmp388,         bmp388_iic_init);
    DRIVER_BMP388_LINK_IIC_DEINIT(&s_bmp388,       bmp388_iic_deinit);
    DRIVER_BMP388_LINK_IIC_READ(&s_bmp388,         bmp388_iic_read);
    DRIVER_BMP388_LINK_IIC_WRITE(&s_bmp388,        bmp388_iic_write);
    DRIVER_BMP388_LINK_DELAY_MS(&s_bmp388,         sensor_delay_ms);
    DRIVER_BMP388_LINK_DEBUG_PRINT(&s_bmp388,      sensor_debug_print);
    DRIVER_BMP388_LINK_RECEIVE_CALLBACK(&s_bmp388, bmp388_receive_callback);

    // Gắn hàm dummy cho SPI (Bắt buộc phải có để LibDriver không báo lỗi con trỏ NULL)
    DRIVER_BMP388_LINK_SPI_INIT(&s_bmp388,         bmp388_spi_init_dummy);
    DRIVER_BMP388_LINK_SPI_DEINIT(&s_bmp388,       bmp388_spi_deinit_dummy);
    DRIVER_BMP388_LINK_SPI_READ(&s_bmp388,         bmp388_spi_read_dummy);
    DRIVER_BMP388_LINK_SPI_WRITE(&s_bmp388,        bmp388_spi_write_dummy);

    if ((bmp388_set_interface(&s_bmp388, BMP388_INTERFACE_IIC) == 0) &&
        (bmp388_set_addr_pin(&s_bmp388, BMP388_ADDRESS_ADO_HIGH) == 0) &&
        (bmp388_init(&s_bmp388) == 0))
    {
        // 1. Bật nguồn cho khối cảm biến Nhiệt và Áp suất bên trong chip
        bmp388_set_pressure(&s_bmp388, BMP388_BOOL_TRUE);
        bmp388_set_temperature(&s_bmp388, BMP388_BOOL_TRUE);

        // 2. Cấu hình Hardware Math (Lấy mẫu quá mức)
        // Áp suất 16x (Độ phân giải 20-bit, triệt tiêu nhiễu gió)
        // Nhiệt độ 2x (Độ phân giải 17-bit, đủ dùng giám sát bo mạch)
        bmp388_set_pressure_oversampling(&s_bmp388, BMP388_OVERSAMPLING_x16);
        bmp388_set_temperature_oversampling(&s_bmp388, BMP388_OVERSAMPLING_x2);

        // 3. Đưa chip vào Sleep Mode ngay lập tức để tiết kiệm pin tối đa
        bmp388_set_mode(&s_bmp388, BMP388_MODE_SLEEP_MODE);

        s_bmp388_ok = 1;
        sensor_debug_print("[SENSORS] BMP388 Init OK (OSR 16x, Sleep Mode).\r\n");
    }
    else
    {
        s_bmp388_ok = 0;
        ret |= ERR_BMP388;
        sensor_debug_print("[SENSORS] BMP388 Init FAILED!\r\n");
    }

    return ret;
}

/* ====================================================================
 * 2. ĐÁNH THỨC VÀ ĐO ĐỒNG LOẠT (Gọi trước khi Delay 130ms)
 * ==================================================================== */
void Sensors_Trigger_All(void)
{
    // Kích hoạt SHT30 đo 1 lần (Lệnh 0x2400: High Repeatability, Clock Stretching Disabled)
    if (s_sht30_ok) {
        sht30_set_reg(&s_sht30, 0x2400);
    }

    // Kích hoạt BMP388 chuyển từ Sleep sang Forced Mode (Đo 1 lần rồi tự ngủ lại)
    if (s_bmp388_ok) {
        bmp388_set_mode(&s_bmp388, BMP388_MODE_FORCED_MODE);
    }

    // Lưu ý: Không dùng HAL_Delay ở đây. Trả luồng chạy về main.c để chờ 130ms.
}

/* ====================================================================
 * 3. THU HOẠCH, ÉP KIỂU VÀ ĐÓNG GÓI 9-BYTE (Gọi sau khi Delay 130ms)
 * ==================================================================== */
void Sensors_Collect_And_Pack(SensorData_t *payload)
{
    // Dọn sạch rác trong RAM, khởi tạo cờ sức khỏe an toàn
    memset(payload, 0, sizeof(SensorData_t));
    payload->health_flag = 0x00;

    /* ----------------------------------------------------------------
     * A. ĐỌC VÀ ÉP KIỂU SHT30
     * ---------------------------------------------------------------- */
    if (s_sht30_ok) {
        uint16_t t_raw, h_raw;
        float tc, hpct;

        // Vì ta đã đợi 130ms, chip SHT30 chắc chắn đã đo xong.
        // Hàm read này sẽ lập tức kéo dữ liệu từ thanh ghi ra I2C.
        if (sht30_single_read(&s_sht30, SHT30_BOOL_FALSE, &t_raw, &tc, &h_raw, &hpct) == 0) {
            // Ép kiểu: Lấy 2 số thập phân (VD: 25.34 -> 2534)
            payload->env_temp = (int16_t)(tc * 100.0f);
            payload->env_hum  = (uint16_t)(hpct * 100.0f);
        } else {
            payload->health_flag |= ERR_SHT30;
            payload->env_temp = 0xFFFF; // Dấu hiệu nhận biết lỗi trên Cloud
            payload->env_hum  = 0xFFFF;
        }
    } else {
        payload->health_flag |= ERR_SHT30;
    }

    /* ----------------------------------------------------------------
     * B. ĐỌC VÀ ÉP KIỂU BMP388
     * ---------------------------------------------------------------- */
    if (s_bmp388_ok) {
        uint32_t p_raw, t_raw;
        float p_pa, tc;

        // Tương tự, BMP388 đã hoàn thành quá trình OSR 16x phức tạp.
        if (bmp388_read_temperature_pressure(&s_bmp388, &t_raw, &tc, &p_raw, &p_pa) == 0) {
            float p_hpa = p_pa / 100.0f;

            // Ép kiểu Áp suất (Giới hạn giải đo bình thường của Trái Đất: 900 -> 1555 hPa)
            // Công thức: (Áp suất thực - 900) * 100. Giúp nhét vừa vào uint16_t.
            if (p_hpa >= 900.0f && p_hpa <= 1555.0f) {
                payload->air_press = (uint16_t)((p_hpa - 900.0f) * 100.0f);
            } else {
                payload->air_press = 0;
            }

            // Ép kiểu Nhiệt độ bo mạch (Chỉ cần quan tâm phần nguyên, VD: 32.7°C -> 32°C)
            payload->board_temp = (int8_t)tc;
        } else {
            payload->health_flag |= ERR_BMP388;
            payload->air_press  = 0xFFFF;
            payload->board_temp = 0xFF;
        }
    } else {
        payload->health_flag |= ERR_BMP388;
    }

    /* ----------------------------------------------------------------
     * C. ĐỌC INA219 (Dự phòng vị trí)
     * ---------------------------------------------------------------- */
    // TODO: Gắn hàm đọc INA219 vào đây trong tương lai
    payload->batt_volt = 0;
    payload->health_flag |= ERR_INA219; // Bật cờ cảnh báo INA219 chưa được kết nối

    /* ----------------------------------------------------------------
     * D. IN RA TERMINAL DEBUG
     * ---------------------------------------------------------------- */
    sensor_debug_print("\r\n--- EDGE NODE PAYLOAD (9 BYTES) ---\r\n");
    sensor_debug_print("Env Temp  : %d (*0.01 degC)\r\n", payload->env_temp);
    sensor_debug_print("Env Hum   : %d (*0.01 %%RH)\r\n", payload->env_hum);
    sensor_debug_print("Air Press : %d (+900 hPa)\r\n", payload->air_press);
    sensor_debug_print("Board Temp: %d (degC)\r\n", payload->board_temp);
    sensor_debug_print("Batt Volt : %d (*0.1 V)\r\n", payload->batt_volt);
    sensor_debug_print("Health Flag: 0x%02X\r\n", payload->health_flag);
    sensor_debug_print("-----------------------------------\r\n\r\n");
}

