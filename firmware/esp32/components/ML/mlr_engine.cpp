/**
 * @file      mlr_engine.cpp
 * @brief     MLR baseline v1.1 dự báo nhiệt độ +2h.
 * @version   3.8.3-ble-prov-status
 *
 * Features:
 *   temperature, humidity, pressure_centered, hour_sin, hour_cos, day_sin, day_cos
 *   + hourly bias calibration.
 */
#include "mlr_engine.h"
#include <math.h>
#include <esp_log.h>

static const char *TAG = "MLR_ENGINE";

#define MLR_MODEL_VERSION      "mlr_baseline_v1_openmeteo"
#define MLR_BIAS_MODEL_VERSION "mlr_baseline_v1_1_hour_bias"

static const float B0        = 0.82040799f;
static const float B_TEMP    = 0.81467521f;
static const float B_HUM     = 0.02652940f;
static const float B_PRESS_C = 0.24688377f;
static const float B_HSIN    = 1.01861861f;
static const float B_HCOS    = -1.44343646f;
static const float B_DSIN    = 0.27184069f;
static const float B_DCOS    = -0.35914265f;

static const float MLR_HOUR_BIAS[24] = {
    +0.44039931f, -0.02987827f, -0.33634972f, -0.74354598f,
    -1.17894561f, -0.37599983f, +0.87882014f, +1.23518935f,
    +1.21797319f, +1.09458169f, +1.04017188f, +0.53173502f,
    +0.43024779f, +0.44799883f, -0.03336267f, -0.39325203f,
    -0.87257192f, -0.26569671f, +0.45311535f, +0.56503680f,
    +0.71923790f, +0.69673057f, +0.51936153f, +0.65205174f
};

static int clamp_hour(int h) {
    if (h < 0) return 0;
    if (h > 23) return 23;
    return h;
}

static int clamp_doy(int d) {
    if (d < 1) return 1;
    if (d > 366) return 366;
    return d;
}

void mlr_engine_init(void) {
    ESP_LOGI(TAG, "MLR init: %s + %s", MLR_MODEL_VERSION, MLR_BIAS_MODEL_VERSION);
}

float mlr_engine_predict_2h(float current_temp,
                            float current_hum,
                            float current_press,
                            int current_hour,
                            int day_of_year) {
    const int hour = clamp_hour(current_hour);
    const int doy = clamp_doy(day_of_year);

    const float pi = 3.14159265358979323846f;
    const float hour_angle = 2.0f * pi * ((float)hour / 24.0f);
    const float day_angle  = 2.0f * pi * ((float)doy / 366.0f);

    const float hour_sin = sinf(hour_angle);
    const float hour_cos = cosf(hour_angle);
    const float day_sin  = sinf(day_angle);
    const float day_cos  = cosf(day_angle);
    const float pressure_centered = current_press - 1000.0f;

    const float raw = B0
        + B_TEMP    * current_temp
        + B_HUM     * current_hum
        + B_PRESS_C * pressure_centered
        + B_HSIN    * hour_sin
        + B_HCOS    * hour_cos
        + B_DSIN    * day_sin
        + B_DCOS    * day_cos;

    const float pred = raw + MLR_HOUR_BIAS[hour];

    ESP_LOGI(TAG,
             "MLR v3.8.3 | T=%.2f H=%.2f P=%.2f | hour=%d doy=%d | raw=%.2f bias=%.2f -> pred=%.2f",
             current_temp, current_hum, current_press, hour, doy,
             raw, MLR_HOUR_BIAS[hour], pred);

    return pred;
}


