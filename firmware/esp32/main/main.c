/**
 * @file      main.c
 * @brief     Firmware cho Trạm Gateway (ESP32) - Hệ thống Quan trắc Khí tượng IoT
 * @details   Chịu trách nhiệm nhận dữ liệu LoRa, giải mã ASCON-128a, chạy mô hình AI 
 * (Hồi quy tuyến tính đa biến - MLR) dự báo nhiệt độ, và đẩy dữ liệu lên 
 * Supabase qua giao thức HTTPS. Có tích hợp OTA Update.
 * @version   3.5 (MLR Nowcasting)
 * @date      2026
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

/* FreeRTOS Includes */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "freertos/queue.h"

/* ESP-IDF Core Includes */
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"

/* Networking & Cloud Includes */
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"

/* External Libraries */
#include "cJSON.h"
#include <bootloader_common.h>
#include "crypto_aead.h"

static const char *TAG = "WEATHER_GATEWAY";

/* ==============================================================================
 * 1. CẤU HÌNH PHẦN CỨNG (HARDWARE CONFIGURATION)
 * ============================================================================== */

/** @defgroup Pinout Cấu hình chân UART và GPIO điều khiển LoRa E32 */
#define E32_UART_NUM          UART_NUM_2
#define E32_UART_TX_PIN       GPIO_NUM_17
#define E32_UART_RX_PIN       GPIO_NUM_18
#define E32_M0_PIN            GPIO_NUM_37
#define E32_M1_PIN            GPIO_NUM_38
#define E32_AUX_PIN           GPIO_NUM_39

/** @defgroup LoRa_Frame Cấu hình khung truyền dữ liệu LoRa */
#define E32_FRAME_LEN         18    /**< Tổng chiều dài khung truyền LoRa (bytes) */
#define E32_FRAME_IDLE_MS     150   /**< Thời gian timeout để nhận biết kết thúc khung */
#define SENSOR_PLAINTEXT_LEN  10    /**< Kích thước dữ liệu gốc chưa mã hóa */
#define ASCON_TAG4_LEN        4     /**< Kích thước thẻ xác thực ASCON */
#define ASCON_INPUT_LEN       (SENSOR_PLAINTEXT_LEN + ASCON_TAG4_LEN)

/** @defgroup LoRa_Reg Các thanh ghi cấu hình LoRa E32-433T30D */
#define E32_CFG_ADDH          0x00
#define E32_CFG_ADDL          0x17
#define E32_CFG_SPEED         0x3A
#define E32_CFG_CHAN          0x17
#define E32_CFG_OPTION        0x44
#define E32_CMD_WRITE_FLASH   0xC0

/* ==============================================================================
 * 2. CẤU HÌNH MẠNG VÀ ĐÁM MÂY (NETWORK & CLOUD CONFIG)
 * ============================================================================== */

/**
 * @brief Thông tin danh sách WiFi dự phòng (Multi-AP Fallback)
 */
typedef struct { 
    const char *ssid; 
    const char *pass; 
} wifi_cred_t;

static const wifi_cred_t s_wifi_list[] = {
    {"Truong Lung",   "12345678"},
    {"LUCAS",         "12345678"},
    {"Phòng toàn trai đẹp", "aicungdeptrai<3"}
};
static const int NUM_WIFIS = sizeof(s_wifi_list) / sizeof(s_wifi_list[0]);
static int           s_wifi_idx       = 0;
static TimerHandle_t s_wifi_timer     = NULL;
static bool          s_wifi_connected = false;
static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT    BIT0

/** @defgroup Supabase Cấu hình API Supabase */
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

/* @brief Nhúng Root CA trực tiếp để bảo mật TLS khi gọi HTTPS */
extern const char supabase_root_ca_pem_start[] asm("_binary_supabase_root_ca_pem_start");
extern const char supabase_root_ca_pem_end[]   asm("_binary_supabase_root_ca_pem_end");

/* ==============================================================================
 * 3. CẤU TRÚC DỮ LIỆU & BẢO MẬT (DATA STRUCTS & SECURITY)
 * ============================================================================== */

/** @brief Khóa bí mật dùng chung giữa STM32 và ESP32 cho ASCON-128a */
static const uint8_t ASCON_SECRET_KEY[16] = {
    0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
    0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C
};

/** @defgroup Sensor_Flags Cờ báo lỗi phần cứng cảm biến */
#define ERR_SHT30   (1U << 0)
#define ERR_BMP388  (1U << 1)
#define ERR_VBAT    (1U << 2)

/**
 * @brief Cấu trúc dữ liệu thô (nén) nhận từ STM32
 */
typedef struct __attribute__((packed)) {
    int16_t  env_temp_raw;
    uint16_t env_hum_raw;
    uint16_t air_press_raw;
    int16_t  board_temp_raw;
    uint8_t  batt_volt_raw;
    uint8_t  health_flag;
} SensorPayloadRaw_t;

/**
 * @brief Cấu trúc dữ liệu sau khi giải mã và phục hồi giá trị thực (float)
 */
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

/* ==============================================================================
 * 4. HÀNG ĐỢI (QUEUES) VÀ AI MODEL (MLOps)
 * ============================================================================== */

/**
 * @brief Cấu trúc điểm dữ liệu đưa vào mô hình AI
 */
typedef struct {
    float pressure;
    float humidity;
    float temperature;
} AI_DataPoint_t;

static QueueHandle_t s_ai_data_queue = NULL;      /**< Hàng đợi nạp data cho Task AI */
static QueueHandle_t s_supabase_queue = NULL;     /**< Hàng đợi nạp data chờ gửi lên Cloud */

static float g_mlr_predicted_temp_2h = 0.0f;      /**< Lưu trữ kết quả dự báo nhiệt độ 2 giờ tới */

/* ==============================================================================
 * 5. CÁC HÀM TIỆN ÍCH (UTILITIES)
 * ============================================================================== */

/**
 * @brief Cấu hình Client HTTP sử dụng chứng chỉ gốc (Root CA) đã nhúng.
 * @param cfg Con trỏ trỏ tới cấu trúc cấu hình HTTP.
 */
static void tls_cfg_fill(esp_http_client_config_t *cfg) {
    cfg->cert_pem = supabase_root_ca_pem_start;
    cfg->crt_bundle_attach = NULL; // Tắt bundle hệ thống, dùng chứng chỉ Custom
}

/**
 * @brief Lấy thời gian hệ thống tính bằng mili-giây.
 * @return Thời gian hiện tại (ms).
 */
static int64_t now_ms(void) { 
    return esp_timer_get_time() / 1000LL; 
}

/**
 * @brief Chuyển đổi giá trị ADC thô (0-255) thành điện áp thực (Volts).
 * @param raw Giá trị ADC thô đo từ pin.
 * @return Điện áp pin tính bằng Volts (Dải 3.0V -> 4.2V).
 */
static inline float battery_raw_to_volt(uint8_t raw) {
    return 3.0f + ((float)raw / 255.0f) * (4.2f - 3.0f);
}

/* ==============================================================================
 * 6. NHIỆM VỤ AI BIÊN (EDGE AI TASK)
 * ============================================================================== */

/**
 * @brief Task xử lý AI trên vi điều khiển (Core 1).
 * @details Chạy mô hình Hồi quy tuyến tính đa biến (Multiple Linear Regression - MLR).
 * Tự động chuyển đổi bộ hệ số (Ngày/Đêm) dựa vào giờ thực tế để dự báo nhiệt độ 2 giờ tới.
 * @param pvParameters Tham số truyền vào Task (không dùng).
 */
static void ai_task(void *pvParameters) {
    ESP_LOGI(TAG, "[AI_ENGINE] Core %d khởi động.", xPortGetCoreID());

    while (1) {
        AI_DataPoint_t dp;
        // Task ngủ chờ dữ liệu (giải phóng CPU)
        if (xQueueReceive(s_ai_data_queue, &dp, portMAX_DELAY) == pdPASS) {
            
            // 1. Lấy giờ thực tế (đã đồng bộ qua NTP)
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            int current_hour = timeinfo.tm_hour;

            float C_COEF, M1_HUM, M2_PRESS;
            
            // 2. Chuyển đổi ngữ cảnh (Context Switching) cho mô hình
            if (current_hour >= 6 && current_hour < 18) {
                // Sử dụng trọng số Ban ngày (Nhiệt độ cơ sở cao, độ ẩm tác động mạnh)
                C_COEF = 28.5f; M1_HUM = -0.15f; M2_PRESS = 0.08f;
            } else {
                // Sử dụng trọng số Ban đêm (Nhiệt độ cơ sở thấp, độ ẩm ít tác động)
                C_COEF = 22.0f; M1_HUM = -0.05f; M2_PRESS = 0.02f;
            }

            // 3. Suy luận (Inference): Phương trình MLR dự đoán nhiệt độ 2 giờ
            g_mlr_predicted_temp_2h = C_COEF + (M1_HUM * dp.humidity) + (M2_PRESS * dp.pressure);
            
            ESP_LOGI(TAG, "[MLR_MODEL] Giờ: %d -> Dự báo nhiệt độ 2h tới: %.2f°C", current_hour, g_mlr_predicted_temp_2h);
        }
    }
}

/* ==============================================================================
 * 7. NHIỆM VỤ ĐÁM MÂY VÀ GIAO TIẾP MẠNG (CLOUD & NETWORKING)
 * ============================================================================== */

/**
 * @brief Hàm đóng gói JSON và thực hiện HTTP POST lên bảng dữ liệu Supabase.
 * @param d Con trỏ chứa dữ liệu đã giải mã từ cảm biến.
 */
static void supabase_post_sensor(const SensorDecodedData_t *d) {
    if (!s_wifi_connected) return;

    // Chuỗi mô tả trạng thái pin
    char bat_str[16];
    if (d->ina219_ok) {
        snprintf(bat_str, sizeof(bat_str), "%.3f", d->battery_volt);
    } else {
        strcpy(bat_str, "null"); 
    }
    
    // Đóng gói JSON Payload
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "{"
        "\"frame_counter\":%"PRIu32","      
        "\"temperature\":%.2f,"
        "\"humidity\":%.2f,"
        "\"pressure\":%.2f,"
        "\"board_temp\":%.2f,"                
        "\"battery\":%s,"
        "\"predicted_temp_2h\":%.2f,"
        "\"device_id\":\"%s\""
        "}",
        d->frame_counter,                   
        d->sht30_ok  ? d->env_temp_c       : 0.0f,
        d->sht30_ok  ? d->env_humidity_pct : 0.0f,
        d->bmp388_ok ? d->air_pressure_hpa : 0.0f,
        d->board_temp_c,                    
        bat_str, 
        g_mlr_predicted_temp_2h, 
        DEVICE_ID
    );

    ESP_LOGI(TAG, "[SUPABASE] Chuẩn bị POST: %s", body);

    char url[160];
    snprintf(url, sizeof(url), "%s/rest/v1/%s", SUPABASE_URL, SUPABASE_TABLE);

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = HTTP_POST_TIMEOUT_MS,
        .buffer_size       = 4096,
        .buffer_size_tx    = 2048,
        .keep_alive_enable = true, // Giữ kết nối TCP để POST nhanh hơn ở lần sau
    };
    tls_cfg_fill(&http_cfg); 

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) { ESP_LOGE(TAG, "[SUPABASE] Khởi tạo HTTP Client thất bại."); return; }

    esp_http_client_set_header(client, "Content-Type",  "application/json");
    esp_http_client_set_header(client, "apikey",        SUPABASE_ANON_KEY);
    esp_http_client_set_header(client, "Authorization", "Bearer " SUPABASE_ANON_KEY);
    esp_http_client_set_header(client, "Prefer",        "return=minimal"); // Tối ưu băng thông trả về
    esp_http_client_set_post_field(client, body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int st = esp_http_client_get_status_code(client);
        if (st == 201 || st == 200)
            ESP_LOGI(TAG, "[SUPABASE] ✅ HTTP %d - Đã lưu dữ liệu.", st);
        else
            ESP_LOGW(TAG, "[SUPABASE] ⚠ HTTP %d — Kiểm tra quyền RLS trên Supabase.", st);
    } else {
        ESP_LOGE(TAG, "[SUPABASE] ❌ Lỗi mạng: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

/**
 * @brief Task xử lý Hàng đợi Cloud (Mô hình Consumer).
 * @details Cách ly quá trình kết nối mạng (chặn luồng/blocking) khỏi quá trình bắt sóng vô tuyến.
 * @param pvParameters Tham số truyền vào Task.
 */
static void supabase_task(void *pvParameters) {
    SensorDecodedData_t data_to_post;

    while (1) {
        // Lấy dữ liệu từ hàng đợi, block vô hạn nếu hàng đợi trống
        if (xQueueReceive(s_supabase_queue, &data_to_post, portMAX_DELAY) == pdPASS) {
            supabase_post_sensor(&data_to_post);
        }
    }
}

/* ==============================================================================
 * 8. HỆ THỐNG CẬP NHẬT QUA MẠNG (OTA UPDATE)
 * ============================================================================== */

/**
 * @brief Gọi API GET đến Supabase để kiểm tra xem có URL Firmware mới hay không.
 * @param out_url Chuỗi buffer chứa URL trả về.
 * @param max_len Kích thước buffer.
 * @return esp_err_t ESP_OK nếu lấy được URL hợp lệ, ngược lại trả về lỗi.
 */
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

/**
 * @brief Task chạy ngầm kiểm tra và thực thi OTA mỗi 30 phút.
 * @param pvParameters Tham số truyền vào Task.
 */
static void ota_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(45000)); // Đợi 45 giây lúc khởi động để ưu tiên các Task khác

    char dynamic_ota_url[512]  = {0};
    char last_flashed_url[512] = {0};

    while (1) {
        if (s_wifi_connected) {
            if (fetch_supabase_ota_url(dynamic_ota_url, sizeof(dynamic_ota_url)) == ESP_OK) {
                
                bool is_old = false;
                nvs_handle_t nvs_h;
                // Kiểm tra xem URL này đã từng được flash chưa (tránh flash lặp lại liên tục)
                if (nvs_open("ota_store", NVS_READWRITE, &nvs_h) == ESP_OK) {
                    size_t sz = sizeof(last_flashed_url);
                    if (nvs_get_str(nvs_h, "last_url", last_flashed_url, &sz) == ESP_OK)
                        is_old = (strcmp(dynamic_ota_url, last_flashed_url) == 0);
                    nvs_close(nvs_h);
                }

                if (!is_old) {
                    ESP_LOGW(TAG, "[OTA_ENGINE] 🚀 Phát hiện Firmware mới: %s", dynamic_ota_url);

                    esp_http_client_config_t ota_http_cfg = {
                        .url               = dynamic_ota_url,
                        .timeout_ms        = HTTP_OTA_TIMEOUT_MS,
                        .buffer_size       = 4096,
                        .keep_alive_enable = true,
                    };
                    tls_cfg_fill(&ota_http_cfg);

                    const esp_https_ota_config_t ota_cfg = { .http_config = &ota_http_cfg };

                    if (esp_https_ota(&ota_cfg) == ESP_OK) {
                        ESP_LOGI(TAG, "[OTA_ENGINE] ✅ Nạp thành công. Lưu lịch sử và khởi động lại...");
                        if (nvs_open("ota_store", NVS_READWRITE, &nvs_h) == ESP_OK) {
                            nvs_set_str(nvs_h, "last_url", dynamic_ota_url);
                            nvs_commit(nvs_h);
                            nvs_close(nvs_h);
                        }
                        vTaskDelay(pdMS_TO_TICKS(3000));
                        esp_restart();
                    } else {
                        ESP_LOGE(TAG, "[OTA_ENGINE] ❌ Nạp Firmware thất bại.");
                    }
                }
            }
        }
        // Ngủ 30 phút rồi mới kiểm tra lại
        vTaskDelay(pdMS_TO_TICKS(30UL * 60UL * 1000UL));
    }
}

/* ==============================================================================
 * 9. KẾT NỐI WIFI (WIFI STATION)
 * ============================================================================== */

static void wifi_retry_timer_cb(TimerHandle_t xTimer) { 
    esp_wifi_connect(); 
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        // Chiến thuật vòng lặp: Rớt mạng thì thử mạng tiếp theo trong danh sách
        s_wifi_idx = (s_wifi_idx + 1) % NUM_WIFIS;
        ESP_LOGW(TAG, "[WIFI] Mất kết nối. Đang thử mạng: %s", s_wifi_list[s_wifi_idx].ssid);
        
        wifi_config_t wcfg = {0};
        strncpy((char *)wcfg.sta.ssid,     s_wifi_list[s_wifi_idx].ssid, sizeof(wcfg.sta.ssid)     - 1);
        strncpy((char *)wcfg.sta.password, s_wifi_list[s_wifi_idx].pass, sizeof(wcfg.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &wcfg);
        
        xTimerStart(s_wifi_timer, 0); // Thử lại sau 5 giây
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        xTimerStop(s_wifi_timer, 0);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "[WIFI] ✅ Đã kết nối thành công, IP được cấp phát.");
    }
}

/**
 * @brief Khởi tạo giao diện mạng và bắt đầu quá trình dò tìm WiFi.
 */
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

/* ==============================================================================
 * 10. GIẢI MÃ BẢO MẬT & DỮ LIỆU CẢM BIẾN (CRYPTO & DECODING)
 * ============================================================================== */

/* Helper functions để đọc kiểu dữ liệu Little Endian từ mảng byte */
static int16_t  read_i16_le(const uint8_t *p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint16_t read_u16_le(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

/**
 * @brief Xây dựng vector khởi tạo (Nonce) 16-byte cho thuật toán ASCON-128a.
 * @details Dựa trên Frame Counter để chống lại các cuộc tấn công phát lại (Replay Attacks).
 * @param c Giá trị Frame counter hiện tại.
 * @param n Mảng đích lưu Nonce.
 */
static void build_nonce_from_counter(uint32_t c, uint8_t n[16]) {
    memset(n, 0, 16);
    n[0] = (uint8_t)(c >> 24); n[1] = (uint8_t)(c >> 16);
    n[2] = (uint8_t)(c >>  8); n[3] = (uint8_t)(c);
    // Chuỗi signature phân biệt hệ thống ("WSN1")
    n[4] = 0x57U; n[5] = 0x53U; n[6] = 0x4EU; n[7] = 0x31U;
}

/**
 * @brief Dịch các byte raw thành giá trị vật lý (nhiệt độ, độ ẩm...).
 * @param pt Mảng 10-byte chứa plaintext thu được sau khi giải mã.
 * @param counter Frame counter gắn kèm.
 * @param out Struct đích chứa dữ liệu đã làm tròn/phục hồi.
 * @return true nếu giải mã thành công.
 */
static bool sensor_payload_decode(const uint8_t pt[SENSOR_PLAINTEXT_LEN], uint32_t counter, SensorDecodedData_t *out) {
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
    
    /* Phân tích cờ trạng thái phần cứng của cảm biến tại node (STM32) */
    out->sht30_ok      = ((raw.health_flag & ERR_SHT30)  == 0);
    out->bmp388_ok     = ((raw.health_flag & ERR_BMP388) == 0);
    out->payload_ok    = out->sht30_ok && out->bmp388_ok;
    out->ina219_ok     = ((raw.health_flag & ERR_VBAT)   == 0);
    
    /* Phục hồi dữ liệu dạng số thực */
    out->board_temp_c  = ((float)raw.board_temp_raw) / 100.0f;   
    out->battery_volt  = battery_raw_to_volt(pt[8]);
    
    if (out->sht30_ok) {
        out->env_temp_c       = (float)raw.env_temp_raw  / 100.0f;
        out->env_humidity_pct = (float)raw.env_hum_raw   / 100.0f;
    }
    if (out->bmp388_ok) {
        out->air_pressure_hpa = 900.0f + (float)raw.air_press_raw / 100.0f;
    }

    return true;
}

/**
 * @brief Hàm Pipeline: Xử lý 1 khung truyền đầy đủ nhận từ module vô tuyến LoRa.
 * @details Đọc Counter -> Xây Nonce -> Giải mã ASCON -> Dịch Payload -> Nạp vào Queues.
 * @param frame Khung truyền (18 bytes).
 */
static void process_frame(const uint8_t frame[E32_FRAME_LEN]) {
    uint32_t counter =  (uint32_t)frame[0]
                     | ((uint32_t)frame[1] <<  8)
                     | ((uint32_t)frame[2] << 16)
                     | ((uint32_t)frame[3] << 24);

    ESP_LOGI(TAG, "[LORA_RX] Gói tin Counter=%"PRIu32" — Đang giải mã ASCON-128a...", counter);

    uint8_t nonce[16];
    build_nonce_from_counter(counter, nonce);

    uint8_t ascon_input[ASCON_INPUT_LEN];
    memcpy(&ascon_input[0], &frame[4],  10);
    memcpy(&ascon_input[10], &frame[14], 4); // Lấy ASCON TAG (4 byte cuối)

    uint8_t plaintext[SENSOR_PLAINTEXT_LEN] = {0};
    unsigned long long mlen = 0ULL;

    /* Xác thực và Giải mã bằng thư viện AEAD ASCON */
    if (crypto_aead_decrypt_tag4(plaintext, &mlen, NULL,
                                 ascon_input, sizeof(ascon_input),
                                 NULL, 0, nonce, ASCON_SECRET_KEY) != 0) {
        ESP_LOGE(TAG, "[LORA_RX] ❌ Lỗi xác thực ASCON TAG (Bị can thiệp hoặc sai khóa)."); 
        return;
    }

    SensorDecodedData_t decoded;
    if (!sensor_payload_decode(plaintext, counter, &decoded)) return;

    if (decoded.payload_ok) {
        ESP_LOGI(TAG, "[LORA_RX] ✅ T=%.2f°C H=%.2f%% P=%.2fhPa",
                 decoded.env_temp_c, decoded.env_humidity_pct,
                 decoded.air_pressure_hpa);

        /* 1. Đẩy dữ liệu vào mô hình AI (Producer cho Queue s_ai_data_queue) */
        AI_DataPoint_t dp = {
            .pressure = decoded.air_pressure_hpa,
            .humidity = decoded.env_humidity_pct,
            .temperature = decoded.env_temp_c
        };
        xQueueSend(s_ai_data_queue, &dp, 0); // Không block nếu đầy
    }
    
    /* 2. Đẩy dữ liệu nguyên vẹn sang hàng đợi của Supabase HTTP POST */
    if (xQueueSend(s_supabase_queue, &decoded, pdMS_TO_TICKS(10)) != pdPASS) {
        ESP_LOGW(TAG, "[LORA_RX] ⚠ Mạng chậm, Queue Supabase đầy. Rớt gói để tránh nghẽn UART.");
    }
}

/* ==============================================================================
 * 11. GIAO TIẾP VÔ TUYẾN LORA (RADIO UART LOOP)
 * ============================================================================== */

/**
 * @brief Vòng lặp chính liên tục lắng nghe tín hiệu UART từ module LoRa E32.
 * @details Được gán mức ưu tiên cao nhất (Priority 10) để tránh rớt khung truyền vô tuyến.
 */
static void receiver_loop(void) {
    uint8_t rx[64], frame[E32_FRAME_LEN];
    size_t  frame_pos = 0; 
    int64_t last_byte_time = 0;

    ESP_LOGI(TAG, "[LORA_RX] Vòng lặp UART sẵn sàng. Kích thước Frame = %d byte.", E32_FRAME_LEN);

    while (1) {
        // Đọc từng khối byte từ UART
        int rd = uart_read_bytes(E32_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(50));
        int64_t t_now = now_ms();
        
        if (rd <= 0) {
            // Xóa buffer nếu qua thời gian timeout mà chưa nhận đủ 1 frame
            if (frame_pos > 0 && (t_now - last_byte_time) > 1000) frame_pos = 0;
            continue;
        }

        // Ráp nối các mảnh byte thành 1 khung truyền hoàn chỉnh (18 byte)
        for (int i = 0; i < rd; ++i) {
            int64_t b = now_ms();
            // Nếu có sự đứt quãng giữa các byte vượt mức cho phép, reset frame
            if (frame_pos > 0 && (b - last_byte_time) > E32_FRAME_IDLE_MS) frame_pos = 0;
            
            frame[frame_pos++] = rx[i];
            last_byte_time = b;
            
            // Khi đủ chiều dài, gửi qua hàm Pipeline xử lý
            if (frame_pos == E32_FRAME_LEN) { 
                process_frame(frame); 
                frame_pos = 0; 
            }
        }
    }
}

/**
 * @brief Cấu hình thông số mặc định cho mạch E32 khi khởi động.
 */
static void lora_e32_init_config(void) {
    ESP_LOGI(TAG, "[LORA_CFG] Bắt đầu thiết lập module E32...");
    // Chuyển E32 vào chế độ Sleep/Config (M0=1, M1=1)
    gpio_set_level(E32_M0_PIN, 1); 
    gpio_set_level(E32_M1_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Gửi lệnh cài đặt Hex
    uint8_t cfg[6] = { E32_CMD_WRITE_FLASH, E32_CFG_ADDH, E32_CFG_ADDL,
                       E32_CFG_SPEED, E32_CFG_CHAN, E32_CFG_OPTION };
    uart_write_bytes(E32_UART_NUM, cfg, sizeof(cfg));
    uart_wait_tx_done(E32_UART_NUM, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(100));

    // Trả E32 về chế độ Hoạt động bình thường (M0=0, M1=0)
    gpio_set_level(E32_M0_PIN, 0); 
    gpio_set_level(E32_M1_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "[LORA_CFG] E32 thiết lập thành công.");
}

/* ==============================================================================
 * 12. HÀM MAIN (APP ENTRY POINT)
 * ============================================================================== */

/**
 * @brief Hàm khởi động chương trình chính của FreeRTOS ESP-IDF.
 */
void app_main(void) {
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  TRẠM QUAN TRẮC MLR NOWCASTING (2H FORECAST)  ");
    ESP_LOGI(TAG, "================================================");

    /* 1. Init các chân GPIO cho LoRa */
    gpio_set_direction(E32_M0_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(E32_M1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(E32_M0_PIN, 0); 
    gpio_set_level(E32_M1_PIN, 0);

    /* 2. Cài đặt UART 2 cho LoRa */
    uart_driver_install(E32_UART_NUM, 2048, 512, 0, NULL, 0);
    uart_config_t uc = {
        .baud_rate  = 115200, 
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE, 
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE, 
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(E32_UART_NUM, &uc);
    uart_set_pin(E32_UART_NUM, E32_UART_TX_PIN, E32_UART_RX_PIN, -1, -1);

    /* 3. Khởi động LoRa và kết nối Mạng */
    lora_e32_init_config();
    wifi_init_sta();

    /* 4. Khối lệnh chờ WiFi & Đồng bộ thời gian thực (NTP) */
    // Block luồng này tối đa 30s để lấy IP
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com"); // Server dự phòng
    esp_sntp_init();

    int retry = 0;
    const int retry_count = 20; 
    time_t now = 0;
    struct tm timeinfo = { 0 };
    
    // Đồng hồ xuất phát từ 1970. Chờ cho đến khi lấy được năm >= 2023 từ Internet
    while (timeinfo.tm_year < (2023 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "[NTP] Chờ đồng bộ thời gian từ Internet (%d/%d)...", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (timeinfo.tm_year >= (2023 - 1900)) {
        // Đồng bộ thành công, cài đặt múi giờ Việt Nam (UTC+7)
        setenv("TZ", "UTC-7", 1);
        tzset();
        ESP_LOGI(TAG, "[NTP] ✅ Đồng hồ đã sync chuẩn.");
    } else {
        ESP_LOGE(TAG, "[NTP] ❌ Đồng bộ thời gian thất bại. TLS sẽ gặp lỗi!");
    }
    
    /* 5. Khởi tạo hàng đợi liên giao tiếp (Queues) */
    s_ai_data_queue = xQueueCreate(10, sizeof(AI_DataPoint_t));
    s_supabase_queue = xQueueCreate(10, sizeof(SensorDecodedData_t));

    /* 6. Gán các Tasks ngầm chạy trên FreeRTOS */
    xTaskCreatePinnedToCore(ai_task,  "AI_TASK",  8192, NULL, 5, NULL, 1);   // Core 1 (Tính toán AI)
    xTaskCreatePinnedToCore(ota_task, "OTA_TASK", 8192, NULL, 4, NULL, 1);   // Core 1 (Kiểm tra bản cập nhật)
    xTaskCreatePinnedToCore(supabase_task, "SBASE_TASK", 8192, NULL, 4, NULL, 0); // Core 0 (Đẩy dữ liệu HTTP)

    /* 7. Trả quyền Core 0 lại cho vòng lặp UART LoRa (Bảo vệ tính toàn vẹn Radio) */
    vTaskPrioritySet(NULL, 10); 
    receiver_loop();
}

