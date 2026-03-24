/*
 * ema.h
 *
 *  Created on: Mar 23, 2026
 *      Author: ADMIN
 */

#ifndef EMA_H_
#define EMA_H_

#include <stdint.h>

/* --- Cấu trúc Lọc Thông Minh (Dynamic EMA) --- */
typedef struct {
    float alpha_slow;     // Hệ số dùng khi môi trường ổn định (vd: 0.1 - Lọc nhiễu mạnh)
    float alpha_fast;     // Hệ số dùng khi có biến động (vd: 0.8 - Phản ứng siêu nhanh)
    float step_threshold; // Ngưỡng chênh lệch để kích hoạt alpha_fast (vd: 0.5 độ C)
    float out;            // Giá trị lưu trữ sau lọc
    uint8_t init;         // Cờ khởi tạo
} DynamicEMA_t;

// Hàm khởi tạo và tính toán
void DynamicEMA_Init(DynamicEMA_t *f, float a_slow, float a_fast, float threshold);
float DynamicEMA_Update(DynamicEMA_t *f, float new_val);


#endif /* EMA_H_ */
