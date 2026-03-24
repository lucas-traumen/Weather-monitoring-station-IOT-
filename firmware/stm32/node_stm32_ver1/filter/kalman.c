#include "kalman.h"
#include <math.h>

void SimpleKalmanFilter_Init(SimpleKalmanFilter_t *kf, float mea_e, float est_e, float q)
{
    kf->err_measure = mea_e;
    kf->err_estimate = est_e;
    kf->q = q;
    kf->current_estimate = 0.0f;
    kf->last_estimate = 0.0f;
    kf->kalman_gain = 0.0f;
}

float SimpleKalmanFilter_Update(SimpleKalmanFilter_t *kf, float mea)
{
    kf->kalman_gain = kf->err_estimate / (kf->err_estimate + kf->err_measure);
    kf->current_estimate = kf->last_estimate + kf->kalman_gain * (mea - kf->last_estimate);
    kf->err_estimate = (1.0f - kf->kalman_gain) * kf->err_estimate + fabs(kf->last_estimate - kf->current_estimate) * kf->q;
    kf->last_estimate = kf->current_estimate;

    return kf->current_estimate;
}

void SimpleKalmanFilter_SetMeasurementError(SimpleKalmanFilter_t *kf, float mea_e) {
    kf->err_measure = mea_e;
}

void SimpleKalmanFilter_SetEstimateError(SimpleKalmanFilter_t *kf, float est_e) {
    kf->err_estimate = est_e;
}

void SimpleKalmanFilter_SetProcessNoise(SimpleKalmanFilter_t *kf, float q) {
    kf->q = q;
}

float SimpleKalmanFilter_GetKalmanGain(SimpleKalmanFilter_t *kf) {
    return kf->kalman_gain;
}

float SimpleKalmanFilter_GetEstimateError(SimpleKalmanFilter_t *kf) {
    return kf->err_estimate;
}

