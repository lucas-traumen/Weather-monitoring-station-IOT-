/**
 * @file      mlr_engine.h
 * @brief     C++ Header giao tiếp cho Khối Suy luận Hồi quy đa biến (MLR)
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
 * @brief Chạy suy luận (Inference) dự báo nhiệt độ 2 giờ tới.
 * @param current_temp Nhiệt độ hiện tại (°C)
 * @param current_hum Độ ẩm hiện tại (%)
 * @param current_press Áp suất hiện tại (hPa)
 * @param current_hour Giờ hiện tại (0 - 23) để chia ngữ cảnh Ngày/Đêm
 * @return Nhiệt độ dự báo 2 giờ tới (°C)
 */
float mlr_engine_predict_2h(float current_temp, float current_hum, float current_press, int current_hour);

#ifdef __cplusplus
}
#endif

#endif /* MLR_ENGINE_H */
