#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "driver/uart.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <bootloader_common.h>

#include "crypto_aead.h"

/*
 * ════════════════════════════════════════════════════════════════════
 * ROOT CA CERT NHÚNG TRỰC TIẾP (TLS DualLayer Strategy)
 * ════════════════════════════════════════════════════════════════════
 */
extern const char supabase_root_ca_pem_start[] asm("_binary_supabase_root_ca_pem_start");
extern const char supabase_root_ca_pem_end[]   asm("_binary_supabase_root_ca_pem_end");

/*
 * Đặt thành 1: Dùng Global Bundle (Phù hợp với Cloudflare/Supabase xoay vòng chứng chỉ)
 */
#define TLS_ONLY_BUNDLE_MODE  0

/* ─────────────────────────── PIN / UART CONFIG ─────────────────────────── */
#define E32_UART_NUM          UART_NUM_2
#define E32_UART_TX_PIN       GPIO_NUM_17
#define E32_UART_RX_PIN       GPIO_NUM_18
#define E32_M0_PIN            GPIO_NUM_37
#define E32_M1_PIN            GPIO_NUM_38
#define E32_AUX_PIN           GPIO_NUM_39

#define E32_FRAME_LEN         18
#define E32_FRAME_IDLE_MS     150
#define SENSOR_PLAINTEXT_LEN  10
#define ASCON_TAG4_LEN        4
#define ASCON_INPUT_LEN       (SENSOR_PLAINTEXT_LEN + ASCON_TAG4_LEN)

/* ─────────────────────────── E32 LORA CONFIG ───────────────────────────── */
#define E32_CFG_ADDH          0x00
#define E32_CFG_ADDL          0x17
#define E32_CFG_SPEED         0x3A
#define E32_CFG_CHAN          0x17
#define E32_CFG_OPTION        0x44
#define E32_CMD_WRITE_FLASH   0xC0

/* ─────────────────────────── WIFI CONFIG ───────────────────────────────── */
typedef struct { const char *ssid; const char *pass; } wifi_cred_t;
static const wifi_cred_t s_wifi_list[] = {
    {"Truong Lung",   "12345678"},
    {"LUCAS",         "12345678"},
    {"Phòng toàn trai đẹp", "aicungdeptrai<3"}
};
static const int NUM_WIFIS = sizeof(s_wifi_list) / sizeof(s_wifi_list[0]);
static int           s_wifi_idx       = 0;
static TimerHandle_t s_wifi_timer     = NULL;
static bool          s_wifi_connected = false;
#define WIFI_CONNECTED_BIT    BIT0

/* ─────────────────────────── SUPABASE CONFIG ───────────────────────────── */
#define SUPABASE_URL      "https://hbuluhjjfivezrrxesaz.supabase.co"
#define SUPABASE_TABLE    "weather_logs"
#define SUPABASE_ANON_KEY \
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9." \
    "eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImhidWx1" \
    "aGpqZml2ZXpycnhlc2F6Iiwicm9sZSI6ImFub24i" \
    "LCJpYXQiOjE3Nzg1MDE4MDksImV4cCI6MjA5NDA3" \
    "NzgwOX0.KhjB0T-8Yy34P3p37XipEutwVfraabsG274NL_88J4Q"
#define DEVICE_ID         "ESP32_LORA_GW"

#define HTTP_POST_TIMEOUT_MS    8000
#define HTTP_GET_TIMEOUT_MS    10000
#define HTTP_OTA_TIMEOUT_MS    60000

/* ─────────────────────────── MLOps GLOBALS ─────────────────────────────── */
#define AI_BUFFER_SIZE  6

typedef struct {
    float pressure;
    float humidity;
    float temperature;
} AI_DataPoint_t;

static QueueHandle_t s_ai_data_queue = NULL;

static bool  g_storm_warning  = false;
static bool  g_frost_warning  = false;
static float g_beta_press     = 0.0f;
static float g_beta_hum       = 0.0f;
static float g_beta_temp      = 0.0f;
static float g_predicted_temp = 0.0f;

float THRESHOLD_BETA_PRESS_STORM = -0.05f;
float THRESHOLD_BETA_HUM_STORM   =  0.05f;
float THRESHOLD_BETA_TEMP_FROST  = -0.10f;

/* ─────────────────────────── SENSOR STRUCTS ────────────────────────────── */
#define ERR_SHT30   (1U << 0)
#define ERR_BMP388  (1U << 1)
#define ERR_VBAT    (1U << 2)

typedef struct __attribute__((packed)) {
    int16_t  env_temp_raw;
    uint16_t env_hum_raw;
    uint16_t air_press_raw;
    int16_t   board_temp_raw;
    uint8_t  batt_volt_raw;
    uint8_t  health_flag;
} SensorPayloadRaw_t;

typedef struct {
    uint32_t frame_counter;
    float    env_temp_c;
    float    env_humidity_pct;
    float    air_pressure_hpa;
    float    board_temp_c;      
    float    battery_volt;
    uint8_t  health_flag;       
    bool     sht30_ok;
    bool     bmp388_ok;
    bool     payload_ok;
    bool     ina219_ok;
} SensorDecodedData_t;

static const char *TAG = "WEATHER_GATEWAY";
static EventGroupHandle_t s_wifi_event_group = NULL;

static const uint8_t ASCON_SECRET_KEY[16] = {
    0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
    0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C
};

/* ═══════════════════════════════════════════════════════════════════
 * HELPER: Tạo esp_http_client_config_t đúng chuẩn TLS cho Supabase
 * ═══════════════════════════════════════════════════════════════════ */
static void tls_cfg_fill(esp_http_client_config_t *cfg)
{
    cfg->cert_pem = supabase_root_ca_pem_start;
    cfg->crt_bundle_attach = NULL;  // Bắt buộc set NULL để tắt bundle cũ
    ESP_LOGI(TAG, "[TLS] Chế độ: Sử dụng Custom PEM Root CA");
}
/* ═══════════════════════════════════════════════════════════════════
 * UTILITIES
 * ═══════════════════════════════════════════════════════════════════ */
static int64_t now_ms(void) { return esp_timer_get_time() / 1000LL; }

static inline float battery_raw_to_volt(uint8_t raw) {
    return 3.0f + ((float)raw / 255.0f) * (4.2f - 3.0f);
}

/* ═══════════════════════════════════════════════════════════════════
 * AI NOWCASTING (SLR) — Core 1
 * ═══════════════════════════════════════════════════════════════════ */
static float calculate_slope(float y[], int n) {
    float sx = 0, sy = 0, sxy = 0, sx2 = 0;
    for (int i = 0; i < n; i++) { sx += i; sy += y[i]; sxy += i*y[i]; sx2 += i*i; }
    float d = n*sx2 - sx*sx;
    return (d == 0.0f) ? 0.0f : (n*sxy - sx*sy) / d;
}

static float calculate_dew_point(float t, float h) { return t - ((100.0f - h) / 5.0f); }

static void ai_task(void *pvParameters) {
    ESP_LOGI(TAG, "[AI_ENGINE] Core %d", xPortGetCoreID());
    AI_DataPoint_t buf[AI_BUFFER_SIZE];
    float ph[AI_BUFFER_SIZE], hh[AI_BUFFER_SIZE], th[AI_BUFFER_SIZE];
    int head = 0, count = 0;

    while (1) {
        AI_DataPoint_t dp;
        if (xQueueReceive(s_ai_data_queue, &dp, portMAX_DELAY) == pdPASS) {
            buf[head] = dp;
            head = (head + 1) % AI_BUFFER_SIZE;
            if (count < AI_BUFFER_SIZE) count++;

            if (count == AI_BUFFER_SIZE) {
                int idx = head;
                for (int i = 0; i < AI_BUFFER_SIZE; i++) {
                    ph[i] = buf[idx].pressure;
                    hh[i] = buf[idx].humidity;
                    th[i] = buf[idx].temperature;
                    idx = (idx + 1) % AI_BUFFER_SIZE;
                }
                g_beta_press = calculate_slope(ph, AI_BUFFER_SIZE);
                g_beta_hum   = calculate_slope(hh, AI_BUFFER_SIZE);
                g_beta_temp  = calculate_slope(th, AI_BUFFER_SIZE);

                g_storm_warning  = (g_beta_press < THRESHOLD_BETA_PRESS_STORM &&
                                    g_beta_hum   > THRESHOLD_BETA_HUM_STORM);
                g_predicted_temp = dp.temperature + (g_beta_temp * AI_BUFFER_SIZE);
                g_frost_warning  = (g_beta_temp < THRESHOLD_BETA_TEMP_FROST &&
                                    g_predicted_temp <= calculate_dew_point(dp.temperature, dp.humidity));

                ESP_LOGI(TAG, "[AI_ENGINE] β_Press=%.4f β_Hum=%.4f β_Temp=%.4f",
                         g_beta_press, g_beta_hum, g_beta_temp);
                if (g_storm_warning) ESP_LOGW(TAG, "[AI_ENGINE] ⚠ CẢNH BÁO DÔNG LỐC!");
                if (g_frost_warning) ESP_LOGW(TAG, "[AI_ENGINE] ❄ CẢNH BÁO SƯƠNG GIÁ! Dự báo: %.2f°C", g_predicted_temp);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * SUPABASE POST
 * ═══════════════════════════════════════════════════════════════════ */
static void supabase_post_sensor(const SensorDecodedData_t *d) {
    if (!s_wifi_connected) return;

    const char *trend = (d->air_pressure_hpa < 1000.0f) ? "falling"
                      : (d->air_pressure_hpa > 1020.0f) ? "rising"
                      : "stable";
    char bat_str[16];
    if (d->ina219_ok) {
        snprintf(bat_str, sizeof(bat_str), "%.3f", d->battery_volt);
    } else {
        strcpy(bat_str, "null"); 
    }
    
    char body[768];
    // Đã sửa lỗi JSON duplicate key "battery" tại đây
    int body_len = snprintf(body, sizeof(body),
        "{"
        "\"frame_counter\":%"PRIu32","      
        "\"temperature\":%.2f,"
        "\"humidity\":%.2f,"
        "\"pressure\":%.2f,"
        "\"board_temp\":%.2f,"                
        "\"battery\":%s,"
        "\"pressure_trend\":\"%s\","
        "\"storm_warning\":%s,"
        "\"frost_warning\":%s,"
        "\"beta_pressure\":%.4f,"
        "\"beta_humidity\":%.4f,"
        "\"beta_temperature\":%.4f,"
        "\"predicted_temp\":%.2f,"
        "\"device_id\":\"%s\""
        "}",
        d->frame_counter,                   
        d->sht30_ok  ? d->env_temp_c       : 0.0f,
        d->sht30_ok  ? d->env_humidity_pct : 0.0f,
        d->bmp388_ok ? d->air_pressure_hpa : 0.0f,
        d->board_temp_c,                    
        bat_str, 
        trend,
        g_storm_warning ? "true" : "false",
        g_frost_warning ? "true" : "false",
        g_beta_press, g_beta_hum, g_beta_temp,
        g_predicted_temp, DEVICE_ID
    );

    ESP_LOGI(TAG, "[SUPABASE] POST: %s", body);

    char url[160];
    snprintf(url, sizeof(url), "%s/rest/v1/%s", SUPABASE_URL, SUPABASE_TABLE);

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = HTTP_POST_TIMEOUT_MS,
        .buffer_size       = 4096,
        .buffer_size_tx    = 2048,
        .keep_alive_enable = true,
    };
    tls_cfg_fill(&http_cfg);  /* ← gắn TLS DualLayer */

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) { ESP_LOGE(TAG, "[SUPABASE] init failed"); return; }

    esp_http_client_set_header(client, "Content-Type",  "application/json");
    esp_http_client_set_header(client, "apikey",        SUPABASE_ANON_KEY);
    esp_http_client_set_header(client, "Authorization", "Bearer " SUPABASE_ANON_KEY);
    esp_http_client_set_header(client, "Prefer",        "return=minimal");
    esp_http_client_set_post_field(client, body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int st = esp_http_client_get_status_code(client);
        if (st == 201 || st == 200)
            ESP_LOGI(TAG, "[SUPABASE] ✅ HTTP %d", st);
        else
            ESP_LOGW(TAG, "[SUPABASE] ⚠ HTTP %d — kiểm tra RLS/table", st);
    } else {
        ESP_LOGE(TAG, "[SUPABASE] ❌ %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

/* ═══════════════════════════════════════════════════════════════════
 * OTA ENGINE — Core 1
 * ═══════════════════════════════════════════════════════════════════ */
static esp_err_t fetch_supabase_ota_url(char *out_url, size_t max_len) {
    char url[256];
    snprintf(url, sizeof(url),
             "%s/rest/v1/device_configs?device_id=eq.%s&select=ota_url",
             SUPABASE_URL, DEVICE_ID);

    esp_http_client_config_t http_cfg = {
        .url            = url,
        .method         = HTTP_METHOD_GET,
        .timeout_ms     = HTTP_GET_TIMEOUT_MS,
        .buffer_size    = 4096,
    };
    tls_cfg_fill(&http_cfg);

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "apikey",        SUPABASE_ANON_KEY);
    esp_http_client_set_header(client, "Authorization", "Bearer " SUPABASE_ANON_KEY);

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) content_length = 2048;

    char *buf = malloc(content_length + 1);
    if (!buf) { esp_http_client_cleanup(client); return ESP_ERR_NO_MEM; }

    int read_len = esp_http_client_read(client, buf, content_length);
    esp_http_client_cleanup(client);

    if (read_len <= 0) { free(buf); return ESP_FAIL; }
    buf[read_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_FAIL;

    esp_err_t result = ESP_FAIL;
    if (cJSON_IsArray(root) && cJSON_GetArraySize(root) > 0) {
        cJSON *item    = cJSON_GetArrayItem(root, 0);
        cJSON *url_obj = cJSON_GetObjectItemCaseSensitive(item, "ota_url");
        if (cJSON_IsString(url_obj) && url_obj->valuestring) {
            strncpy(out_url, url_obj->valuestring, max_len - 1);
            out_url[max_len - 1] = '\0';
            result = ESP_OK;
        }
    }
    cJSON_Delete(root);
    return result;
}

static void ota_task(void *pvParameters) {
    ESP_LOGI(TAG, "[OTA_ENGINE] Core %d — chờ 45 s", xPortGetCoreID());
    vTaskDelay(pdMS_TO_TICKS(45000));

    char dynamic_ota_url[512]  = {0};
    char last_flashed_url[512] = {0};

    while (1) {
        if (s_wifi_connected) {
            ESP_LOGI(TAG, "[OTA_ENGINE] Quét device_configs...");

            if (fetch_supabase_ota_url(dynamic_ota_url, sizeof(dynamic_ota_url)) == ESP_OK) {
                bool is_old = false;
                nvs_handle_t nvs_h;
                if (nvs_open("ota_store", NVS_READWRITE, &nvs_h) == ESP_OK) {
                    size_t sz = sizeof(last_flashed_url);
                    if (nvs_get_str(nvs_h, "last_url", last_flashed_url, &sz) == ESP_OK)
                        is_old = (strcmp(dynamic_ota_url, last_flashed_url) == 0);
                    nvs_close(nvs_h);
                }

                if (is_old) {
                    ESP_LOGI(TAG, "[OTA_ENGINE] ⏭ Firmware đã cập nhật, bỏ qua.");
                } else {
                    ESP_LOGW(TAG, "[OTA_ENGINE] 🚀 Firmware mới: %s", dynamic_ota_url);

                    esp_http_client_config_t ota_http_cfg = {
                        .url               = dynamic_ota_url,
                        .timeout_ms        = HTTP_OTA_TIMEOUT_MS,
                        .buffer_size       = 4096,
                        .keep_alive_enable = true,
                    };
                    tls_cfg_fill(&ota_http_cfg);

                    const esp_https_ota_config_t ota_cfg = { .http_config = &ota_http_cfg };

                    if (esp_https_ota(&ota_cfg) == ESP_OK) {
                        ESP_LOGI(TAG, "[OTA_ENGINE] ✅ Flash thành công. Khởi động lại...");
                        if (nvs_open("ota_store", NVS_READWRITE, &nvs_h) == ESP_OK) {
                            nvs_set_str(nvs_h, "last_url", dynamic_ota_url);
                            nvs_commit(nvs_h);
                            nvs_close(nvs_h);
                        }
                        vTaskDelay(pdMS_TO_TICKS(3000));
                        esp_restart();
                    } else {
                        ESP_LOGE(TAG, "[OTA_ENGINE] ❌ Flash thất bại.");
                    }
                }
            } else {
                ESP_LOGD(TAG, "[OTA_ENGINE] Không có OTA URL.");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30UL * 60UL * 1000UL));
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * WiFi STA — multi-AP fallback
 * ═══════════════════════════════════════════════════════════════════ */
static void wifi_retry_timer_cb(TimerHandle_t xTimer) { esp_wifi_connect(); }

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        s_wifi_idx = (s_wifi_idx + 1) % NUM_WIFIS;
        ESP_LOGW(TAG, "[WIFI] Thử mạng: %s", s_wifi_list[s_wifi_idx].ssid);
        wifi_config_t wcfg = {0};
        strncpy((char *)wcfg.sta.ssid,     s_wifi_list[s_wifi_idx].ssid, sizeof(wcfg.sta.ssid)     - 1);
        strncpy((char *)wcfg.sta.password, s_wifi_list[s_wifi_idx].pass, sizeof(wcfg.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &wcfg);
        xTimerStart(s_wifi_timer, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        xTimerStop(s_wifi_timer, 0);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "[WIFI] ✅ IP cấp phát OK.");
    }
}

static void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    s_wifi_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(5000), pdFALSE, NULL, wifi_retry_timer_cb);
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid,     s_wifi_list[0].ssid, sizeof(wcfg.sta.ssid)     - 1);
    strncpy((char *)wcfg.sta.password, s_wifi_list[0].pass, sizeof(wcfg.sta.password) - 1);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_start();
}

/* ═══════════════════════════════════════════════════════════════════
 * ASCON / SENSOR DECODE
 * ═══════════════════════════════════════════════════════════════════ */
static int16_t  read_i16_le(const uint8_t *p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint16_t read_u16_le(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static void build_nonce_from_counter(uint32_t c, uint8_t n[16]) {
    memset(n, 0, 16);
    n[0] = (uint8_t)(c >> 24); n[1] = (uint8_t)(c >> 16);
    n[2] = (uint8_t)(c >>  8); n[3] = (uint8_t)(c);
    n[4] = 0x57U; n[5] = 0x53U; n[6] = 0x4EU; n[7] = 0x31U;
}

static bool sensor_payload_decode(const uint8_t pt[SENSOR_PLAINTEXT_LEN],
                                  uint32_t counter, SensorDecodedData_t *out) {
    memset(out, 0, sizeof(*out));
    SensorPayloadRaw_t raw;
    raw.env_temp_raw   = read_i16_le(&pt[0]);
    raw.env_hum_raw    = read_u16_le(&pt[2]);
    raw.air_press_raw  = read_u16_le(&pt[4]);
    raw.board_temp_raw = read_i16_le(&pt[6]);               
    raw.batt_volt_raw  = pt[8];
    raw.health_flag    = pt[9];

    out->frame_counter = counter;             
    out->health_flag   = raw.health_flag;             
    out->sht30_ok      = ((raw.health_flag & ERR_SHT30)  == 0);
    out->bmp388_ok     = ((raw.health_flag & ERR_BMP388) == 0);
    out->payload_ok    = out->sht30_ok && out->bmp388_ok;
    out->ina219_ok     = ((raw.health_flag & ERR_VBAT)   == 0);
    out->board_temp_c  = ((float)raw.board_temp_raw) / 100.0f;   
    out->battery_volt  = battery_raw_to_volt(pt[8]);
    
    if (out->sht30_ok) {
        out->env_temp_c       = (float)raw.env_temp_raw  / 100.0f;
        out->env_humidity_pct = (float)raw.env_hum_raw   / 100.0f;
    }
    if (out->bmp388_ok) {
        out->air_pressure_hpa = 900.0f + (float)raw.air_press_raw / 100.0f;
    }
    if (out->ina219_ok) {
        out->battery_volt = battery_raw_to_volt(pt[8]);
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * FRAME PROCESSING
 * ═══════════════════════════════════════════════════════════════════ */
static void process_frame(const uint8_t frame[E32_FRAME_LEN]) {
    uint32_t counter =  (uint32_t)frame[0]
                     | ((uint32_t)frame[1] <<  8)
                     | ((uint32_t)frame[2] << 16)
                     | ((uint32_t)frame[3] << 24);

    ESP_LOGI(TAG, "[LORA_RX] Counter=%"PRIu32" — ASCON-128a...", counter);

    uint8_t nonce[16];
    build_nonce_from_counter(counter, nonce);

    uint8_t ascon_input[ASCON_INPUT_LEN];
    memcpy(&ascon_input[0], &frame[4],  10);
    memcpy(&ascon_input[10], &frame[14], 4);

    uint8_t plaintext[SENSOR_PLAINTEXT_LEN] = {0};
    unsigned long long mlen = 0ULL;

    if (crypto_aead_decrypt_tag4(plaintext, &mlen, NULL,
                                 ascon_input, sizeof(ascon_input),
                                 NULL, 0, nonce, ASCON_SECRET_KEY) != 0) {
        ESP_LOGE(TAG, "[LORA_RX] ❌ ASCON tag lỗi!"); return;
    }

    SensorDecodedData_t decoded;
    if (!sensor_payload_decode(plaintext, counter, &decoded)) return;

    if (decoded.payload_ok) {
        ESP_LOGI(TAG, "[LORA_RX] ✅ T=%.2f°C H=%.2f%% P=%.2fhPa BdT=%.2f°C BAT=%.3fV",
                 decoded.env_temp_c, decoded.env_humidity_pct,
                 decoded.air_pressure_hpa,decoded.board_temp_c, decoded.battery_volt);

        AI_DataPoint_t dp = {
            .pressure = decoded.air_pressure_hpa,
            .humidity = decoded.env_humidity_pct,
            .temperature = decoded.env_temp_c
        };
        xQueueSend(s_ai_data_queue, &dp, 0);
    } else {
        ESP_LOGW(TAG, "[LORA_RX] Cảm biến báo lỗi phần cứng.");
    }
    supabase_post_sensor(&decoded);
}

/* ═══════════════════════════════════════════════════════════════════
 * RECEIVER LOOP
 * ═══════════════════════════════════════════════════════════════════ */
static void receiver_loop(void) {
    uint8_t rx[64], frame[E32_FRAME_LEN];
    size_t  frame_pos = 0; int64_t last_byte_time = 0;
    ESP_LOGI(TAG, "[LORA_RX] Sẵn sàng, khung %d byte", E32_FRAME_LEN);
    while (1) {
        int rd = uart_read_bytes(E32_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(50));
        int64_t t_now = now_ms();
        if (rd <= 0) {
            if (frame_pos > 0 && (t_now - last_byte_time) > 1000) frame_pos = 0;
            continue;
        }
        for (int i = 0; i < rd; ++i) {
            int64_t b = now_ms();
            if (frame_pos > 0 && (b - last_byte_time) > E32_FRAME_IDLE_MS) frame_pos = 0;
            frame[frame_pos++] = rx[i];
            last_byte_time = b;
            if (frame_pos == E32_FRAME_LEN) { process_frame(frame); frame_pos = 0; }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * E32 LORA INIT
 * ═══════════════════════════════════════════════════════════════════ */
static void lora_e32_init_config(void) {
    ESP_LOGI(TAG, "[LORA_CFG] Ghi cấu hình E32...");
    gpio_set_level(E32_M0_PIN, 1); gpio_set_level(E32_M1_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t cfg[6] = { E32_CMD_WRITE_FLASH, E32_CFG_ADDH, E32_CFG_ADDL,
                       E32_CFG_SPEED, E32_CFG_CHAN, E32_CFG_OPTION };
    uart_write_bytes(E32_UART_NUM, cfg, sizeof(cfg));
    uart_wait_tx_done(E32_UART_NUM, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(E32_M0_PIN, 0); gpio_set_level(E32_M1_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "[LORA_CFG] Module sẵn sàng, kênh 0x%02X.", E32_CFG_CHAN);
}

/* ═══════════════════════════════════════════════════════════════════
 * app_main
 * ═══════════════════════════════════════════════════════════════════ */
void app_main(void) {
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  TRẠM QUAN TRẮC KHÍ TƯỢNG GATEWAY V3.3 START  ");
    ESP_LOGI(TAG, "================================================");

    gpio_set_direction(E32_M0_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(E32_M1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(E32_M0_PIN, 0); gpio_set_level(E32_M1_PIN, 0);

    uart_driver_install(E32_UART_NUM, 2048, 512, 0, NULL, 0);
    uart_config_t uc = {
        .baud_rate  = 115200, .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(E32_UART_NUM, &uc);
    uart_set_pin(E32_UART_NUM, E32_UART_TX_PIN, E32_UART_RX_PIN, -1, -1);

    lora_e32_init_config();
    wifi_init_sta();

    /* ── Chờ WiFi kết nối TRƯỚC khi khởi động NTP ──────────────────────
     * wifi_init_sta() chỉ gọi esp_wifi_start() rồi return ngay (async).
     * Nếu khởi động SNTP khi chưa có IP → NTP không sync được → đồng hồ
     * vẫn là epoch 1970 → TLS xác minh cert THẤT BẠI ("not yet valid").
     * ─────────────────────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "[BOOT] Chờ WiFi trước khi đồng bộ NTP...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "[BOOT] ⚠ WiFi chưa kết nối sau 30s — NTP/TLS có thể thất bại!");
    }

    /* Bắt đầu cấu hình và đồng bộ NTP (chỉ sau khi có mạng) */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com"); /* backup server */
    esp_sntp_init();

    /* --- ĐOẠN CODE ĐÃ ĐƯỢC CẬP NHẬT MÚI GIỜ --- */
    int retry = 0;
    const int retry_count = 20; 
    time_t now = 0;
    struct tm timeinfo = { 0 };
    
    // Kiểm tra xem năm hiện tại đã lớn hơn 2023 chưa (1970 là chưa sync)
    while (timeinfo.tm_year < (2023 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "[NTP] Đang chờ đồng bộ thời gian từ Internet... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000)); // Đợi 2 giây
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (timeinfo.tm_year < (2023 - 1900)) {
        ESP_LOGE(TAG, "[NTP] ❌ Không đồng bộ được thời gian! TLS chắc chắn sẽ thất bại.");
    } else {
        // Cấu hình múi giờ UTC+7 (Cú pháp POSIX là UTC-7)
        setenv("TZ", "UTC-7", 1);
        tzset();
        
        ESP_LOGI(TAG, "[NTP] ✅ Đồng hồ đã sync chuẩn: %s", ctime(&now));
    }
    /* ------------------------------------------- */
    
    s_ai_data_queue = xQueueCreate(10, sizeof(AI_DataPoint_t));
    xTaskCreatePinnedToCore(ai_task,  "AI_TASK",  8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(ota_task, "OTA_TASK", 8192, NULL, 4, NULL, 1);
   
    receiver_loop();
}