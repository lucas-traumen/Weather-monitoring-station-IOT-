#include "telemetry.h"




/**
 * @brief Hàm đóng gói và gửi toàn bộ dữ liệu qua MIN Protocol
 * @param min_ctx: Context của MIN
 * @param data: Con trỏ chứa dữ liệu cảm biến (đã qua lọc Kalman/Average)
 * @param battery_pct: Phần trăm pin (0-100)
 */
void Telemetry_Send_MIN(struct min_context *min_ctx, const SensorData_t *data, uint8_t battery_pct)
{
    uint8_t payload[13]; // Tổng cộng 13 byte dữ liệu
    uint8_t idx = 0;

    // 1. Temp Out (SHT30) -> int16_t (Nhân 100 để giữ 2 số thập phân)
    int16_t t_out = (int16_t)(data->env.temperature_c * 100);
    payload[idx++] = (t_out >> 8) & 0xFF;
    payload[idx++] = t_out & 0xFF;

    // 2. Hum Out (SHT30) -> uint16_t (Nhân 100)
    uint16_t h_out = (uint16_t)(data->env.humidity_pct * 100);
    payload[idx++] = (h_out >> 8) & 0xFF;
    payload[idx++] = h_out & 0xFF;

    // 3. Temp Board (BMP388) -> int16_t (Nhân 100)
    int16_t t_board = (int16_t)(data->board.temperature_c * 100);
    payload[idx++] = (t_board >> 8) & 0xFF;
    payload[idx++] = t_board & 0xFF;

    // 4. Pressure (BMP388) -> uint32_t (Nhân 100)
    uint32_t press = (uint32_t)(data->board.pressure_hpa * 100);
    payload[idx++] = (press >> 24) & 0xFF;
    payload[idx++] = (press >> 16) & 0xFF;
    payload[idx++] = (press >> 8) & 0xFF;
    payload[idx++] = press & 0xFF;

    // 5. Altitude -> int16_t (Nhân 10 để giữ 1 số thập phân - vd 45.3m -> 453)
    int16_t alt = (int16_t)(data->board.altitude_m * 10);
    payload[idx++] = (alt >> 8) & 0xFF;
    payload[idx++] = alt & 0xFF;

    // 6. Battery -> uint8_t (1 byte)
    payload[idx++] = battery_pct;

    // Gửi qua bộ MIN Protocol
    min_send_frame(min_ctx, MIN_ID_TELEMETRY, payload, idx);
}

