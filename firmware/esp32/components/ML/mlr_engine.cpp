/**
 * @file      mlr_engine.cpp
 * @brief     Mô hình Hồi quy tuyến tính đa biến (MLR) dự báo nhiệt độ 2 giờ tới.
 * @details   Hệ số được huấn luyện ngoại tuyến trên Python/scikit-learn với kỹ thuật
 *            Data Shifting: biến mục tiêu Y là nhiệt độ tại t+2h, X là humidity/pressure tại t.
 *            Hai bộ hệ số riêng biệt cho ngữ cảnh Ban Ngày (6h-17h) và Ban Đêm (18h-5h).
 */
#include "mlr_engine.h"
#include <esp_log.h>

static const char* TAG = "MLR_ENGINE";

/**
 * @class MLRPredictor
 * @brief Lớp bao bọc các hệ số mô hình đã được huấn luyện.
 * @details Dễ dàng mở rộng sau này nếu cần thêm hàm nạp hệ số từ NVS/Cloud.
 */
class MLRPredictor {
private:
    /* --- HỆ SỐ MÔ HÌNH (LẤY TỪ QUÁ TRÌNH TRAINING PYTHON) --- */
    
    // Ngữ cảnh Ban Ngày (6h - 17h)
    const float DAY_C        = 28.5f;
    const float DAY_M1_HUM   = -0.15f;
    const float DAY_M2_PRESS = 0.08f;

    // Ngữ cảnh Ban Đêm (18h - 5h)
    const float NIGHT_C        = 22.0f;
    const float NIGHT_M1_HUM   = -0.05f;
    const float NIGHT_M2_PRESS = 0.02f;

public:
    MLRPredictor() {}

    /**
     * @brief Chạy suy luận MLR dự báo nhiệt độ 2 giờ tới.
     * @note  Tham số `temp` hiện tại chưa đưa vào phương trình (tránh đa cộng tuyến với Y).
     *        Giữ lại trong signature để dễ mở rộng sau (e.g. thêm feature lag nhiệt độ).
     */
    float predict(float temp, float hum, float press, int hour) {
        (void)temp; // Chủ động bỏ qua để tránh compiler warning
        float result = 0.0f;

        if (hour >= 6 && hour < 18) {
            result = DAY_C + (DAY_M1_HUM * hum) + (DAY_M2_PRESS * press);
            ESP_LOGI(TAG, "Ngữ cảnh [NGÀY] -> T_DựBáo: %.2f", result);
        } else {
            result = NIGHT_C + (NIGHT_M1_HUM * hum) + (NIGHT_M2_PRESS * press);
            ESP_LOGI(TAG, "Ngữ cảnh [ĐÊM] -> T_DựBáo: %.2f", result);
        }

        return result;
    }
};

/* Khởi tạo một đối tượng (Instance) duy nhất cho toàn chương trình */
static MLRPredictor predictor;

/* ================== C WRAPPERS ================== */
void mlr_engine_init(void) {
    ESP_LOGI(TAG, "C++ MLR Engine (2h Forecast) đã khởi tạo thành công.");
}

float mlr_engine_predict_2h(float current_temp, float current_hum, float current_press, int current_hour) {
    return predictor.predict(current_temp, current_hum, current_press, current_hour);
}

