#include "ema.h"
#include <math.h>

void DynamicEMA_Init(DynamicEMA_t *f, float a_slow, float a_fast, float threshold) {
    f->alpha_slow = a_slow;
    f->alpha_fast = a_fast;
    f->step_threshold = threshold;
    f->out = 0.0f;
    f->init = 0;
}

float DynamicEMA_Update(DynamicEMA_t *f, float new_val) {
    // 1. Nếu là lần đo đầu tiên khi vừa bật máy -> Lấy luôn giá trị thật
    if (!f->init) {
        f->out = new_val;
        f->init = 1;
        return f->out;
    }

    // 2. Tính độ lệch giữa giá trị đo được và giá trị đang hiển thị
    float diff = fabs(new_val - f->out);

    // 3. Tự động chuyển số (Fast hoặc Slow)
    float current_alpha;
    if (diff > f->step_threshold) {
        current_alpha = f->alpha_fast; // Biến động lớn -> Đuổi theo ngay lập tức
    } else {
        current_alpha = f->alpha_slow; // Biến động nhỏ -> Khả năng cao là nhiễu, giữ ổn định
    }

    // 4. Cập nhật đầu ra theo công thức EMA
    f->out = (current_alpha * new_val) + ((1.0f - current_alpha) * f->out);

    return f->out;
}

