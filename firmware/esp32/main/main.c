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
#include <stdlib.h>
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
#include "mlr_engine.h"

static const char *TAG = "WEATHER_GATEWAY";

/* ==============================================================================
 * 1. CẤU HÌNH PHẦN CỨNG (HARDWARE CONFIGURATION)
 * ============================================================================== */

/** @defgroup Pinout Cấu hình chân UART và GPIO điều khiển LoRa E32 */
#define E32_UART_NUM          UART_NUM_2
#define E32_UART_TX_PIN       GPIO_NUM_17
#define E32_UART_RX_PIN       GPIO_NUM_18
#define E32_M0_PIN            GPIO_NUM_6
#define E32_M1_PIN            GPIO_NUM_7
#define E32_AUX_PIN           GPIO_NUM_8

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

/*
 * E32 UART baudrate:
 * - CONFIG_BAUD: baud dùng để nói chuyện với module khi vào Sleep/Config mode.
 *   E32 mới hoặc reset factory thường là 9600.
 * - RUN_BAUD: baud sau khi ghi E32_CFG_SPEED = 0x3A.
 */
#define E32_CONFIG_BAUD       9600
#define E32_RUN_BAUD          115200
#define E32_CFG_PACKET_LEN    6
#define E32_AUX_TIMEOUT_MS    1500

/* Bật log raw UART để debug lúc chưa nhận được gói LoRa. Khi chạy ổn có thể đổi về 0. */
#define LORA_DEBUG_RAW_RX     1

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
 * @details Nhận dữ liệu cảm biến từ Queue, gọi C++ MLR Engine để dự báo nhiệt độ 2 giờ tới.
 * Toàn bộ logic hệ số và ngữ cảnh Ngày/Đêm được đóng gói trong mlr_engine.cpp.
 * @param pvParameters Tham số truyền vào Task (không dùng).
 */
static void ai_task(void *pvParameters) {
    ESP_LOGI(TAG, "[AI_ENGINE] Core %d khởi động.", xPortGetCoreID());
    mlr_engine_init();

    while (1) {
        AI_DataPoint_t dp;
        // Task ngủ chờ dữ liệu (giải phóng CPU)
        if (xQueueReceive(s_ai_data_queue, &dp, portMAX_DELAY) == pdPASS) {

            // Lấy giờ thực tế (đã đồng bộ qua NTP) để MLR Engine chọn ngữ cảnh Ngày/Đêm
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);

            // Suy luận (Inference): Gọi C++ MLR Engine dự báo nhiệt độ 2 giờ tới
            g_mlr_predicted_temp_2h = mlr_engine_predict_2h(
                dp.temperature, dp.humidity, dp.pressure, timeinfo.tm_hour
            );

            ESP_LOGI(TAG, "[AI_ENGINE] Giờ: %d -> Dự báo nhiệt độ 2h tới: %.2f°C",
                     timeinfo.tm_hour, g_mlr_predicted_temp_2h);
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

    /*
     * Dữ liệu lỗi nên gửi null thay vì 0.0 để database không hiểu nhầm là giá trị thật.
     * Supabase/PostgREST chấp nhận number/null trong JSON.
     */
    char temp_str[24], hum_str[24], press_str[24], bat_str[24];
    if (d->sht30_ok) {
        snprintf(temp_str, sizeof(temp_str), "%.2f", d->env_temp_c);
        snprintf(hum_str,  sizeof(hum_str),  "%.2f", d->env_humidity_pct);
    } else {
        strcpy(temp_str, "null");
        strcpy(hum_str,  "null");
    }

    if (d->bmp388_ok) {
        snprintf(press_str, sizeof(press_str), "%.2f", d->air_pressure_hpa);
    } else {
        strcpy(press_str, "null");
    }

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
        "\"temperature\":%s,"
        "\"humidity\":%s,"
        "\"pressure\":%s,"
        "\"board_temp\":%.2f,"
        "\"battery\":%s,"
        "\"predicted_temp_2h\":%.2f,"
        "\"device_id\":\"%s\""
        "}",
        d->frame_counter,
        temp_str,
        hum_str,
        press_str,
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

/**
 * @brief Khởi tạo NVS một lần ở đầu chương trình.
 */
static void app_nvs_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

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
    ESP_ERROR_CHECK(esp_netif_init());
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
static esp_err_t e32_wait_aux_high(uint32_t timeout_ms) {
    int64_t start_ms = now_ms();

    while ((now_ms() - start_ms) < timeout_ms) {
        if (gpio_get_level(E32_AUX_PIN) == 1) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "[LORA_CFG] AUX vẫn LOW sau %"PRIu32" ms", timeout_ms);
    return ESP_ERR_TIMEOUT;
}

static void e32_set_mode(uint8_t m0, uint8_t m1, const char *mode_name) {
    gpio_set_level(E32_M0_PIN, m0 ? 1 : 0);
    gpio_set_level(E32_M1_PIN, m1 ? 1 : 0);

    /* Datasheet E32 cần một khoảng thời gian để chuyển mode. AUX HIGH nghĩa là module rảnh. */
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_err_t aux = e32_wait_aux_high(E32_AUX_TIMEOUT_MS);

    ESP_LOGI(TAG, "[LORA_CFG] Mode=%s M0=%d M1=%d AUX=%d (%s)",
             mode_name, m0, m1, gpio_get_level(E32_AUX_PIN), esp_err_to_name(aux));
}

static esp_err_t e32_write_config_at_baud(int baud) {
    ESP_LOGI(TAG, "[LORA_CFG] Thử ghi config E32 ở UART baud=%d", baud);

    ESP_ERROR_CHECK(uart_set_baudrate(E32_UART_NUM, baud));
    uart_flush_input(E32_UART_NUM);

    /* Sleep/Config mode: M0=1, M1=1 */
    e32_set_mode(1, 1, "SLEEP_CONFIG");
    uart_flush_input(E32_UART_NUM);

    uint8_t cfg[E32_CFG_PACKET_LEN] = {
        E32_CMD_WRITE_FLASH,
        E32_CFG_ADDH,
        E32_CFG_ADDL,
        E32_CFG_SPEED,
        E32_CFG_CHAN,
        E32_CFG_OPTION
    };

    ESP_LOGI(TAG, "[LORA_CFG] Gửi config:");
    ESP_LOG_BUFFER_HEXDUMP(TAG, cfg, sizeof(cfg), ESP_LOG_INFO);

    int wr = uart_write_bytes(E32_UART_NUM, cfg, sizeof(cfg));
    uart_wait_tx_done(E32_UART_NUM, pdMS_TO_TICKS(500));

    if (wr != sizeof(cfg)) {
        ESP_LOGE(TAG, "[LORA_CFG] Gửi config thiếu byte: %d/%d", wr, (int)sizeof(cfg));
        return ESP_FAIL;
    }

    /* Sau khi ghi config, module thường trả lại đúng 6 byte config. */
    uint8_t resp[16] = {0};
    int rd = uart_read_bytes(E32_UART_NUM, resp, sizeof(cfg), pdMS_TO_TICKS(1200));

    ESP_LOGI(TAG, "[LORA_CFG] Phản hồi config rd=%d", rd);
    if (rd > 0) {
        ESP_LOG_BUFFER_HEXDUMP(TAG, resp, rd, ESP_LOG_INFO);
    }

    if (rd == sizeof(cfg) && memcmp(resp, cfg, sizeof(cfg)) == 0) {
        ESP_LOGI(TAG, "[LORA_CFG] ✅ E32 nhận config OK ở baud=%d", baud);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "[LORA_CFG] E32 chưa phản hồi đúng ở baud=%d", baud);
    return ESP_FAIL;
}

/**
 * @brief Cấu hình thông số mặc định cho mạch E32 khi khởi động.
 * @details Thử 9600 trước vì E32 mặc định thường là 9600. Nếu module đã từng được
 *          cấu hình sang 115200 thì thử tiếp 115200. Sau đó đưa module về Normal mode.
 */
static esp_err_t lora_e32_init_config(void) {
    ESP_LOGI(TAG, "[LORA_CFG] Bắt đầu thiết lập module E32...");

    esp_err_t err = e32_write_config_at_baud(E32_CONFIG_BAUD);
    if (err != ESP_OK) {
        err = e32_write_config_at_baud(E32_RUN_BAUD);
    }

    /* Normal mode: M0=0, M1=0 */
    e32_set_mode(0, 0, "NORMAL");

    /* Sau khi E32_CFG_SPEED=0x3A, UART của module chạy ở 115200. */
    ESP_ERROR_CHECK(uart_set_baudrate(E32_UART_NUM, E32_RUN_BAUD));
    uart_flush_input(E32_UART_NUM);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[LORA_CFG] ✅ E32 đã sẵn sàng ở Normal mode, UART=%d", E32_RUN_BAUD);
    } else {
        ESP_LOGE(TAG, "[LORA_CFG] ❌ Không xác nhận được config E32. Vẫn chuyển sang Normal mode để nghe thử.");
        ESP_LOGE(TAG, "[LORA_CFG] Kiểm tra dây TX/RX chéo, M0/M1/AUX, nguồn E32, baud hiện tại và channel.");
    }

    return err;
}

/**
 * @brief Vòng lặp chính liên tục lắng nghe tín hiệu UART từ module LoRa E32.
 */
static void receiver_loop(void) {
    uint8_t rx[64], frame[E32_FRAME_LEN];
    size_t  frame_pos = 0;
    int64_t last_byte_time = 0;

    ESP_LOGI(TAG, "[LORA_RX] UART sẵn sàng. Frame=%d byte, RX pin=%d, TX pin=%d, baud=%d",
             E32_FRAME_LEN, E32_UART_RX_PIN, E32_UART_TX_PIN, E32_RUN_BAUD);

    while (1) {
        int rd = uart_read_bytes(E32_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(50));
        int64_t t_now = now_ms();

        if (rd <= 0) {
            if (frame_pos > 0 && (t_now - last_byte_time) > 1000) {
                ESP_LOGW(TAG, "[LORA_RX] Timeout khi mới nhận %u/%d byte -> xóa frame dở",
                         (unsigned)frame_pos, E32_FRAME_LEN);
                frame_pos = 0;
            }
            continue;
        }

#if LORA_DEBUG_RAW_RX
        ESP_LOGI(TAG, "[LORA_RX_RAW] rd=%d", rd);
        ESP_LOG_BUFFER_HEXDUMP(TAG, rx, rd, ESP_LOG_INFO);
#endif

        for (int i = 0; i < rd; ++i) {
            int64_t b = now_ms();

            if (frame_pos > 0 && (b - last_byte_time) > E32_FRAME_IDLE_MS) {
                ESP_LOGW(TAG, "[LORA_RX] Idle gap %lld ms khi mới nhận %u/%d byte -> reset",
                         (long long)(b - last_byte_time), (unsigned)frame_pos, E32_FRAME_LEN);
                frame_pos = 0;
            }

            frame[frame_pos++] = rx[i];
            last_byte_time = b;

            if (frame_pos == E32_FRAME_LEN) {
                ESP_LOGI(TAG, "[LORA_RX] Đủ 18 byte, xử lý frame:");
                ESP_LOG_BUFFER_HEXDUMP(TAG, frame, E32_FRAME_LEN, ESP_LOG_INFO);
                process_frame(frame);
                frame_pos = 0;
            }
        }
    }
}

static void lora_receiver_task(void *pvParameters) {
    (void)pvParameters;
    receiver_loop();
    vTaskDelete(NULL);
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

    /* 0. NVS dùng cho WiFi + OTA. Khởi tạo một lần ở đầu chương trình. */
    app_nvs_init();

    /* 1. Init GPIO điều khiển LoRa E32 */
    gpio_set_direction(E32_M0_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(E32_M1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(E32_AUX_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(E32_AUX_PIN, GPIO_FLOATING);

    gpio_set_level(E32_M0_PIN, 0);
    gpio_set_level(E32_M1_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "[PINOUT] E32_TX(GPIO%d)->RXD_E32, E32_RX(GPIO%d)<-TXD_E32, M0=%d, M1=%d, AUX=%d",
             E32_UART_TX_PIN, E32_UART_RX_PIN, E32_M0_PIN, E32_M1_PIN, E32_AUX_PIN);

    /* 2. Cài đặt UART2 cho LoRa. Ban đầu dùng 9600 để config được module factory. */
    ESP_ERROR_CHECK(uart_driver_install(E32_UART_NUM, 4096, 1024, 0, NULL, 0));
    uart_config_t uc = {
        .baud_rate  = E32_CONFIG_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(E32_UART_NUM, &uc));
    ESP_ERROR_CHECK(uart_set_pin(E32_UART_NUM, E32_UART_TX_PIN, E32_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_flush_input(E32_UART_NUM);

    /* 3. Cấu hình E32: thử 9600 -> 115200, kiểm tra ACK, sau đó về Normal mode. */
    esp_err_t lora_cfg_result = lora_e32_init_config();
    if (lora_cfg_result != ESP_OK) {
        ESP_LOGW(TAG, "[BOOT] LoRa config chưa xác nhận được. Xem log [LORA_CFG] để kiểm tra phần cứng.");
    }

    /* 4. Tạo Queue trước khi bật task nhận LoRa để không rớt dữ liệu. */
    s_ai_data_queue = xQueueCreate(10, sizeof(AI_DataPoint_t));
    s_supabase_queue = xQueueCreate(20, sizeof(SensorDecodedData_t));

    if (!s_ai_data_queue || !s_supabase_queue) {
        ESP_LOGE(TAG, "[BOOT] Không tạo được queue. Restart...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    /* 5. Bật task LoRa sớm, không chờ WiFi/NTP để tránh bỏ lỡ gói vô tuyến. */
    xTaskCreatePinnedToCore(lora_receiver_task, "LORA_RX_TASK", 8192, NULL, 10, NULL, 0);

    /* 6. Bật AI task. Supabase/OTA sẽ bật sau khi init WiFi. */
    xTaskCreatePinnedToCore(ai_task, "AI_TASK", 8192, NULL, 5, NULL, 1);

    /* 7. Kết nối mạng */
    wifi_init_sta();

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));

    /* 8. Đồng bộ NTP. Nếu fail thì LoRa vẫn chạy, chỉ TLS/AI theo giờ có thể chưa chuẩn. */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_init();

    int retry = 0;
    const int retry_count = 20;
    time_t now = 0;
    struct tm timeinfo = {0};

    while (timeinfo.tm_year < (2023 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "[NTP] Chờ đồng bộ thời gian từ Internet (%d/%d)...", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (timeinfo.tm_year >= (2023 - 1900)) {
        /* POSIX TZ: UTC-7 nghĩa là múi giờ thực tế UTC+7. */
        setenv("TZ", "UTC-7", 1);
        tzset();
        ESP_LOGI(TAG, "[NTP] ✅ Đồng hồ đã sync chuẩn.");
    } else {
        ESP_LOGE(TAG, "[NTP] ❌ Đồng bộ thời gian thất bại. TLS có thể lỗi nếu đồng hồ sai.");
    }

    /* 9. Bật task cloud sau khi network đã init. */
    xTaskCreatePinnedToCore(supabase_task, "SBASE_TASK", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(ota_task, "OTA_TASK", 8192, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "[BOOT] Gateway đã chạy. Theo dõi log [LORA_RX_RAW] để biết UART có byte vào hay không.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}


