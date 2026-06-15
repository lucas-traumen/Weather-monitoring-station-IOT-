/**
 * @file      mlr_engine.h
 * @version   3.8-baseline-mlr
 * @brief     C interface cho MLR baseline Open-Meteo + hourly bias correction.
 */
#ifndef MLR_ENGINE_H
#define MLR_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo khối tính toán MLR.
 */
void mlr_engine_init(void);

/**
 * @brief Chạy suy luận dự báo nhiệt độ 2 giờ tới.
 *
 * Công thức:
 * predicted_temp_2h =
 *   b0
 * + b_temp * temperature
 * + b_hum * humidity
 * + b_press * (pressure - 1000)
 * + b_hour_sin * hour_sin
 * + b_hour_cos * hour_cos
 * + b_day_sin * day_sin
 * + b_day_cos * day_cos
 * + bias_by_hour[current_hour]
 *
 * @param current_temp  Nhiệt độ hiện tại (°C)
 * @param current_hum   Độ ẩm hiện tại (%)
 * @param current_press Áp suất hiện tại (hPa)
 * @param current_hour  Giờ hiện tại 0..23 theo UTC+7
 * @param day_of_year   Ngày trong năm 1..366
 * @return Nhiệt độ dự báo sau 2 giờ (°C)
 */
float mlr_engine_predict_2h(float current_temp,
                            float current_hum,
                            float current_press,
                            int current_hour,
                            int day_of_year);

#ifdef __cplusplus
}
#endif

#endif /* MLR_ENGINE_H */



