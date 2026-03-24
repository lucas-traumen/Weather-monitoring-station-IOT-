/*
 SimpleKalmanFilter - Refactored for Multi-Instance (C)
 */ 
#ifndef KALMAN_H_
#define KALMAN_H_

// Đóng gói toàn bộ biến trạng thái vào 1 struct
typedef struct {
  float err_measure;
  float err_estimate;
  float q;
  float current_estimate;
  float last_estimate;
  float kalman_gain;
} SimpleKalmanFilter_t;

// Các hàm bây giờ phải truyền kèm con trỏ tới struct để biết đang tính cho bộ lọc nào
void SimpleKalmanFilter_Init(SimpleKalmanFilter_t *kf, float mea_e, float est_e, float q);
float SimpleKalmanFilter_Update(SimpleKalmanFilter_t *kf, float mea);

void SimpleKalmanFilter_SetMeasurementError(SimpleKalmanFilter_t *kf, float mea_e);
void SimpleKalmanFilter_SetEstimateError(SimpleKalmanFilter_t *kf, float est_e);
void SimpleKalmanFilter_SetProcessNoise(SimpleKalmanFilter_t *kf, float q);

float SimpleKalmanFilter_GetKalmanGain(SimpleKalmanFilter_t *kf);
float SimpleKalmanFilter_GetEstimateError(SimpleKalmanFilter_t *kf);

#endif

