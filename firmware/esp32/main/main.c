/**
 * @file      main.c
 * @brief     Firmware cho Trạm Gateway (ESP32) - Hệ thống Quan trắc Khí tượng IoT
 * @details   Chịu trách nhiệm nhận dữ liệu LoRa, giải mã ASCON-128a, chạy mô hình AI 
 * (Hồi quy tuyến tính đa biến - MLR) dự báo nhiệt độ, và đẩy dữ liệu lên 
 * Supabase qua giao thức HTTPS. Có tích hợp OTA Update.
 * @version   3.8.3.17-production-wifi-ble
 * @date      2026
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
#include "sdkconfig.h"
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
#include "esp_partition.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include "esp_task_wdt.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

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
#define ASCON_TAG4_LEN        4     /**< Tag LoRa legacy STM32 -> ESP32: giữ 4 byte để không phá node STM32 hiện tại */
#define ASCON_INPUT_LEN       (SENSOR_PLAINTEXT_LEN + ASCON_TAG4_LEN)

/*
 * ASCON Cloud tag ESP32 -> Edge Function:
 * - Tăng lên full 16 byte để chống giả mạo/sửa gói mạnh hơn.
 * - LoRa vẫn giữ tag4 ở bản này để không đổi frame STM32.
 * - payload_key2 = ciphertext 10 byte + tag16 16 byte = 26 byte = 52 hex.
 */
#define ASCON_CLOUD_TAG_LEN        16
#define ASCON_CLOUD_INPUT_LEN      (SENSOR_PLAINTEXT_LEN + ASCON_CLOUD_TAG_LEN)
#define ASCON_CLOUD_HEX_LEN        (ASCON_CLOUD_INPUT_LEN * 2)

/** @defgroup LoRa_Reg Các thanh ghi cấu hình LoRa E32-433T30D */
#define E32_CFG_ADDH          0x00
#define E32_CFG_ADDL          0x17
#define E32_CFG_SPEED         0x3A
#define E32_CFG_CHAN          0x17
#define E32_CFG_OPTION        0x44
#define E32_CMD_WRITE_FLASH   0xC0
#define E32_CMD_READ_CONFIG    0xC1

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

/* Bật log raw UART để debug lúc chưa nhận được gói LoRa. Khi chạy thật để 0 để giảm tải log. */
#define LORA_DEBUG_RAW_RX     0

/*
 * RAM-only replay check:
 * - Phù hợp với quy trình demo/lab: reset cả STM32 + ESP32 + DB thì counter bắt đầu lại từ 0.
 * - Không lưu NVS để tránh kẹt khi bạn chủ động reset toàn bộ hệ thống.
 * - Nếu chỉ reset STM32 mà không reset ESP32, counter STM32 về 0 sẽ bị bỏ; khi đó tạm đổi về 0 để debug.
 */
#define LORA_ENABLE_RAM_REPLAY_CHECK 1

/* ==============================================================================
 * 2. CẤU HÌNH MẠNG VÀ ĐÁM MÂY (NETWORK & CLOUD CONFIG)
 * ============================================================================== */

/**
 * @brief Phiên bản firmware hiện tại.
 * @note  Luôn cập nhật khi thay đổi logic lớn để web/gateway_status theo dõi đúng.
 */
#define GW_VERSION                 "3.8.3.17-production-wifi-ble"
#define DEVICE_ID                  "ESP32_LORA_GW"
#define APP_TIMEZONE_POSIX         "ICT-7"   /* POSIX TZ: ICT-7 = UTC+7 */

/**
 * @brief BLE WiFi Provisioning.
 *
 * Lần đầu boot hoặc chưa có credential trong NVS:
 *   - Gateway quảng bá BLE với tên WSN-GW-xxxxxx.
 *   - Dùng app ESP BLE Provisioning để quét, chọn WiFi, nhập password.
 *   - Credential được lưu vào NVS của WiFi driver.
 */
#define WIFI_PROV_POP              "duck27005"
#define WIFI_PROV_SERVICE_PREFIX   "WSN-GW"
#define WIFI_PROV_WAIT_MS          (0UL)      /* 0 = production: BLE provisioning chờ vô hạn, không timeout */
#define WIFI_MAX_RETRY             5

/* Production: không dùng hardcoded fallback. Nếu WiFi sai/mất NVS -> giữ BLE để nhập lại. */
#define APP_ENABLE_HARDCODED_WIFI_FALLBACK  0

static int  s_wifi_retry_count = 0;
static bool s_wifi_using_fallback = false;
static bool s_ble_prov_active = false;
static bool s_wifi_connect_allowed = false;
static bool s_wifi_connected = false;
static bool s_prov_mgr_ready = false;
static bool s_mqtt_enabled = false;
static bool s_mqtt_connected = false;
static char s_mqtt_broker[96] = "";
static char s_ip_addr[20] = "0.0.0.0";
static EventGroupHandle_t s_wifi_event_group = NULL;

#define WIFI_CONNECTED_BIT         BIT0
#define WIFI_FAIL_BIT              BIT1

/** @defgroup Supabase Cấu hình Supabase / Edge Function */
#define SUPABASE_URL      "https://hbuluhjjfivezrrxesaz.supabase.co"
#define EDGE_FUNCTION_URL "https://hbuluhjjfivezrrxesaz.supabase.co/functions/v1/ingest-weather"

/*
 * Token này phải giống Supabase Secret: INGEST_DEVICE_TOKEN.
 * Không dùng service_role key trong firmware.
 */
#define INGEST_DEVICE_TOKEN "CHANGE_THIS_LONG_RANDOM_DEVICE_TOKEN"

/* Chỉ dùng anon key cho SELECT OTA config. Không dùng để INSERT weather_logs. */
#define SUPABASE_ANON_KEY \
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9." \
    "eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImhidWx1" \
    "aGpqZml2ZXpycnhlc2F6Iiwicm9sZSI6ImFub24i" \
    "LCJpYXQiOjE3Nzg1MDE4MDksImV4cCI6MjA5NDA3" \
    "NzgwOX0.KhjB0T-8Yy34P3p37XipEutwVfraabsG274NL_88J4Q"

#define HTTP_POST_TIMEOUT_MS       10000
#define HTTP_GET_TIMEOUT_MS        10000
#define HTTP_OTA_TIMEOUT_MS        60000
#define GATEWAY_STATUS_PERIOD_MS   60000

/*
 * OTA fast-check:
 * - Không đợi 45 giây như bản cũ.
 * - ESP32 kiểm tra device_configs mỗi 5 giây sau khi WiFi/NTP sẵn sàng.
 * - Quá trình OTA dùng esp_https_ota_begin/perform/finish để có thể yield,
 *   tránh Task Watchdog khi ghi flash lâu.
 */
#define OTA_BOOT_DELAY_MS          3000
#define OTA_CHECK_PERIOD_MS        5000
#define OTA_FAIL_BACKOFF_MS        30000
#define OTA_URL_MAX_LEN            512
#define OTA_HTTP_RX_BUFFER_SIZE    8192
#define OTA_HTTP_TX_BUFFER_SIZE    2048
#define SHA256_HEX_LEN             64
static char s_running_sha256_hex[SHA256_HEX_LEN + 1] = {0};
static volatile bool s_ota_in_progress = false;
static volatile int s_http_in_flight = 0;
static portMUX_TYPE s_http_ota_mux = portMUX_INITIALIZER_UNLOCKED;

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

/**
 * @brief Khóa ASCON key2 dùng riêng cho ESP32 -> Edge Function.
 * @note  Supabase Secret phải là ASCON_KEY2_HEX=668ab13302c94de19f7021a85cd47710
 */
static const uint8_t ASCON_CLOUD_KEY2[16] = {
    0x66, 0x8A, 0xB1, 0x33, 0x02, 0xC9, 0x4D, 0xE1,
    0x9F, 0x70, 0x21, 0xA8, 0x5C, 0xD4, 0x77, 0x10
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
    float    predicted_temp_2h;
    bool     predicted_ok;
    uint8_t  plaintext[SENSOR_PLAINTEXT_LEN];
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

static QueueHandle_t s_supabase_queue = NULL;     /**< Hàng đợi nạp data chờ gửi lên Cloud */

static float g_mlr_predicted_temp_2h = 0.0f;      /**< Lưu trữ kết quả dự báo nhiệt độ 2 giờ tới */

#if LORA_ENABLE_RAM_REPLAY_CHECK
static uint32_t s_last_frame_counter = 0;
static bool s_have_last_frame_counter = false;
#endif

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

static bool app_time_is_valid(void) {
    time_t now = 0;
    struct tm t = {0};

    time(&now);
    localtime_r(&now, &t);

    return t.tm_year >= (2023 - 1900);
}

static bool http_try_begin(const char *log_tag) {
    bool allow = false;

    portENTER_CRITICAL(&s_http_ota_mux);
    if (!s_ota_in_progress) {
        s_http_in_flight++;
        allow = true;
    }
    portEXIT_CRITICAL(&s_http_ota_mux);

    if (!allow) {
        ESP_LOGW(TAG, "[%s] Skip POST vì OTA đang chạy.", log_tag);
    }

    return allow;
}

static void http_end(void) {
    portENTER_CRITICAL(&s_http_ota_mux);
    if (s_http_in_flight > 0) {
        s_http_in_flight--;
    }
    portEXIT_CRITICAL(&s_http_ota_mux);
}

static int http_in_flight_get(void) {
    int n = 0;

    portENTER_CRITICAL(&s_http_ota_mux);
    n = s_http_in_flight;
    portEXIT_CRITICAL(&s_http_ota_mux);

    return n;
}

static void ota_set_in_progress(bool in_progress) {
    portENTER_CRITICAL(&s_http_ota_mux);
    s_ota_in_progress = in_progress;
    portEXIT_CRITICAL(&s_http_ota_mux);
}

/**
 * @brief Chuyển đổi giá trị ADC thô (0-255) thành điện áp thực (Volts).
 * @param raw Giá trị ADC thô đo từ pin.
 * @return Điện áp pin tính bằng Volts (Dải 3.0V -> 4.2V).
 */
static inline float battery_raw_to_volt(uint8_t raw) {
    return 3.0f + ((float)raw / 255.0f) * (4.2f - 3.0f);
}

static void build_nonce_from_counter(uint32_t c, uint8_t n[16]);

static void app_set_timezone_utc7(void) {
    setenv("TZ", APP_TIMEZONE_POSIX, 1);
    tzset();
}

static void app_log_time_now(const char *prefix) {
    time_t now = 0;
    struct tm utc = {0};
    struct tm local = {0};

    time(&now);
    gmtime_r(&now, &utc);
    localtime_r(&now, &local);

    ESP_LOGI(TAG,
             "%s UTC=%04d-%02d-%02d %02d:%02d:%02d | LOCAL=%04d-%02d-%02d %02d:%02d:%02d | hour=%d doy=%d TZ=%s",
             prefix,
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec,
             local.tm_hour, local.tm_yday + 1, APP_TIMEZONE_POSIX);
}

static int app_get_wifi_rssi(void) {
    wifi_ap_record_t ap = {0};
    if (s_wifi_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

static void bytes_to_hex(const uint8_t *in, size_t len, char *out, size_t out_len) {
    static const char hex[] = "0123456789abcdef";
    if (out_len < (len * 2 + 1)) {
        if (out_len > 0) out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        out[i * 2]     = hex[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static float app_predict_temp_2h(float temperature, float humidity, float pressure) {
    time_t now = 0;
    struct tm timeinfo = {0};

    time(&now);
    localtime_r(&now, &timeinfo);

    int local_hour = timeinfo.tm_hour;
    int day_of_year = timeinfo.tm_yday + 1;

    float pred = mlr_engine_predict_2h(
        temperature,
        humidity,
        pressure,
        local_hour,
        day_of_year
    );

    ESP_LOGI(TAG,
             "[MLR_TIME] local=%04d-%02d-%02d %02d:%02d:%02d | hour=%d doy=%d | pred=%.2f°C",
             timeinfo.tm_year + 1900,
             timeinfo.tm_mon + 1,
             timeinfo.tm_mday,
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec,
             local_hour,
             day_of_year,
             pred);

    return pred;
}

/* ==============================================================================
 * 6. MLR FORECAST
 * ==============================================================================
 * v3.8.3.15:
 * - Không dùng AI task riêng nữa.
 * - MLR được tính đồng bộ ngay khi nhận LoRa frame trong process_frame().
 * - Giữ nguyên app_predict_temp_2h().
 */

/* ==============================================================================
 * 7. NHIỆM VỤ ĐÁM MÂY VÀ GIAO TIẾP MẠNG (CLOUD & NETWORKING)
 * ============================================================================== */

/**
 * @brief POST JSON lên Supabase Edge Function ingest-weather.
 * @note  Không dùng anon INSERT. Edge Function dùng service_role để ghi DB.
 */
static esp_err_t edge_post_json(const char *json_body, const char *log_tag) {
    if (!s_wifi_connected) {
        ESP_LOGW(TAG, "[%s] Bỏ qua POST vì WiFi chưa connected.", log_tag);
        return ESP_ERR_WIFI_NOT_CONNECT;
    }

    if (!http_try_begin(log_tag)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_http_client_config_t http_cfg = {
        .url               = EDGE_FUNCTION_URL,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = HTTP_POST_TIMEOUT_MS,
        .buffer_size       = 4096,
        .buffer_size_tx    = 2048,
        .keep_alive_enable = true,
    };
    tls_cfg_fill(&http_cfg);

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "[%s] Khởi tạo HTTP client thất bại.", log_tag);
        http_end();
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-device-token", INGEST_DEVICE_TOKEN);
    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err == ESP_OK && status >= 200 && status < 300) {
        ESP_LOGI(TAG, "[%s] ✅ Edge HTTP %d", log_tag, status);
    } else if (err == ESP_OK) {
        ESP_LOGW(TAG, "[%s] ⚠ Edge HTTP %d. Kiểm tra Edge logs / token / schema.", log_tag, status);
    } else {
        ESP_LOGE(TAG, "[%s] ❌ Edge POST lỗi: %s", log_tag, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    http_end();
    return err;
}


static bool is_valid_sha256_hex(const char *s) {
    if (!s || strlen(s) != SHA256_HEX_LEN) return false;
    for (size_t i = 0; i < SHA256_HEX_LEN; ++i) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

/**
 * @brief Tính SHA256 của app partition đang chạy.
 * @details Dùng để phân biệt firmware cũ/mới ổn định hơn ota_url hoặc version string.
 */
static esp_err_t app_update_running_sha256_cache(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGW(TAG, "[APP_HASH] Không lấy được running partition.");
        return ESP_FAIL;
    }

    uint8_t sha[32] = {0};
    esp_err_t err = esp_partition_get_sha256(running, sha);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[APP_HASH] esp_partition_get_sha256 failed: %s", esp_err_to_name(err));
        return err;
    }

    bytes_to_hex(sha, sizeof(sha), s_running_sha256_hex, sizeof(s_running_sha256_hex));
    ESP_LOGI(TAG, "[APP_HASH] running_partition=%s sha256=%s",
             running->label ? running->label : "unknown",
             s_running_sha256_hex);
    return ESP_OK;
}

static const char *app_get_running_sha256_hex(void) {
    if (s_running_sha256_hex[0] == '\0') {
        (void)app_update_running_sha256_cache();
    }
    return s_running_sha256_hex;
}

/**
 * @brief Mã hóa plaintext sensor bằng ASCON key2 để gửi lên Edge Function.
 * @details v3.8.3.14 dùng full tag 16 byte cho cloud:
 *          payload_key2 = ciphertext 10 byte + tag16 16 byte = 26 byte = 52 hex.
 * @note  Tag dài hơn không làm ciphertext "khó giải mã" hơn, nhưng tăng mạnh
 *        khả năng chống giả mạo/sửa gói hợp lệ.
 */
static bool build_payload_key2_hex(const SensorDecodedData_t *d, char out_hex[ASCON_CLOUD_HEX_LEN + 1]) {
    uint8_t nonce[16];
    build_nonce_from_counter(d->frame_counter, nonce);

    uint8_t ct_full[ASCON_CLOUD_INPUT_LEN] = {0};
    unsigned long long clen = 0ULL;

    if (crypto_aead_encrypt(ct_full, &clen,
                            d->plaintext, SENSOR_PLAINTEXT_LEN,
                            NULL, 0, NULL, nonce, ASCON_CLOUD_KEY2) != 0) {
        ESP_LOGE(TAG, "[EDGE_SENSOR] ASCON key2 encrypt failed");
        return false;
    }

    if (clen != ASCON_CLOUD_INPUT_LEN) {
        ESP_LOGE(TAG, "[EDGE_SENSOR] ASCON key2 output length invalid: %llu, expected=%d",
                 clen, ASCON_CLOUD_INPUT_LEN);
        return false;
    }

    bytes_to_hex(ct_full, ASCON_CLOUD_INPUT_LEN, out_hex, ASCON_CLOUD_HEX_LEN + 1);
    return true;
}

/**
 * @brief Đóng gói sensor packet và POST lên Edge Function.
 */
static void edge_post_sensor(const SensorDecodedData_t *d) {
    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "[EDGE_SENSOR] Skip POST vì OTA đang chạy.");
        return;
    }
    char payload_key2_hex[ASCON_CLOUD_HEX_LEN + 1] = {0};
    if (!build_payload_key2_hex(d, payload_key2_hex)) return;

    char pred_str[24];
    if (d->predicted_ok) snprintf(pred_str, sizeof(pred_str), "%.2f", d->predicted_temp_2h);
    else strcpy(pred_str, "null");

    char body[1050];
    snprintf(body, sizeof(body),
        "{"
        "\"type\":\"sensor\","
        "\"device_id\":\"%s\","
        "\"firmware_version\":\"%s\","
        "\"firmware_sha256\":\"%s\","
        "\"frame_counter\":%"PRIu32","
        "\"payload_key2\":\"%s\","
        "\"predicted_temp_2h\":%s,"
        "\"model_version\":\"mlr_baseline_v1_1_hour_bias\","
        "\"model_source\":\"openmeteo_baseline_utc7\","
        "\"wifi_rssi\":%d,"
        "\"mqtt_enabled\":%s,"
        "\"mqtt_connected\":%s,"
        "\"mqtt_broker\":\"%s\","
        "\"ip_address\":\"%s\","
        "\"uptime_sec\":%lld,"
        "\"free_heap\":%lu"
        "}",
        DEVICE_ID,
        GW_VERSION,
        app_get_running_sha256_hex(),
        d->frame_counter,
        payload_key2_hex,
        pred_str,
        app_get_wifi_rssi(),
        s_mqtt_enabled ? "true" : "false",
        s_mqtt_connected ? "true" : "false",
        s_mqtt_broker,
        s_ip_addr,
        (long long)(esp_timer_get_time() / 1000000LL),
        (unsigned long)esp_get_free_heap_size()
    );

    ESP_LOGI(TAG, "[EDGE_SENSOR] POST frame=%"PRIu32" payload_key2(tag16)=%s", d->frame_counter, payload_key2_hex);
    edge_post_json(body, "EDGE_SENSOR");
}

/**
 * @brief Task Consumer: gửi sensor packet từ queue lên Edge Function.
 */
static void edge_task(void *pvParameters) {
    (void)pvParameters;
    SensorDecodedData_t data_to_post;

    while (1) {
        if (xQueueReceive(s_supabase_queue, &data_to_post, portMAX_DELAY) == pdPASS) {
            edge_post_sensor(&data_to_post);
        }
    }
}

/**
 * @brief Heartbeat riêng của ESP32 gateway để web biết gateway còn sống.
 * @note  Không phụ thuộc STM32 có gửi LoRa hay không.
 */
static void gateway_status_task(void *pvParameters) {
    (void)pvParameters;

    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        if (s_ota_in_progress) {
            ESP_LOGW(TAG, "[GW_STATUS] Skip heartbeat vì OTA đang chạy.");
            vTaskDelay(pdMS_TO_TICKS(GATEWAY_STATUS_PERIOD_MS));
            continue;
        }

        if (s_wifi_connected) {
            char body[850];
            snprintf(body, sizeof(body),
                "{"
                "\"type\":\"gateway_status\","
                "\"device_id\":\"%s\","
                "\"firmware_version\":\"%s\","
                "\"firmware_sha256\":\"%s\","
                "\"wifi_rssi\":%d,"
                "\"mqtt_enabled\":%s,"
                "\"mqtt_connected\":%s,"
                "\"mqtt_broker\":\"%s\","
                "\"ip_address\":\"%s\","
                "\"uptime_sec\":%lld,"
                "\"free_heap\":%lu,"
                "\"status_note\":\"gateway heartbeat\""
                "}",
                DEVICE_ID,
                GW_VERSION,
                app_get_running_sha256_hex(),
                app_get_wifi_rssi(),
                s_mqtt_enabled ? "true" : "false",
                s_mqtt_connected ? "true" : "false",
                s_mqtt_broker,
                s_ip_addr,
                (long long)(esp_timer_get_time() / 1000000LL),
                (unsigned long)esp_get_free_heap_size()
            );

            edge_post_json(body, "GW_STATUS");
        }

        vTaskDelay(pdMS_TO_TICKS(GATEWAY_STATUS_PERIOD_MS));
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
typedef struct {
    char    ota_url[OTA_URL_MAX_LEN];
    char    ota_sha256[SHA256_HEX_LEN + 1];
    int64_t ota_seq;
    bool    has_ota_seq;
    bool    has_ota_sha256;
} ota_config_t;

static esp_err_t fetch_supabase_ota_config_select(const char *select_fields, ota_config_t *out_cfg) {
    if (!out_cfg || !select_fields) return ESP_ERR_INVALID_ARG;
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->ota_seq = -1;

    char url[320];
    snprintf(url, sizeof(url),
             "%s/rest/v1/device_configs?device_id=eq.%s&select=%s&limit=1",
             SUPABASE_URL, DEVICE_ID, select_fields);

    esp_http_client_config_t http_cfg = {
        .url            = url,
        .method         = HTTP_METHOD_GET,
        .timeout_ms     = HTTP_GET_TIMEOUT_MS,
        .buffer_size    = 4096,
        .buffer_size_tx = 1024,
    };
    tls_cfg_fill(&http_cfg);

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "apikey",        SUPABASE_ANON_KEY);
    esp_http_client_set_header(client, "Authorization", "Bearer " SUPABASE_ANON_KEY);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (content_length <= 0 || content_length > 2048) content_length = 2048;

    char *buf = calloc(1, content_length + 1);
    if (!buf) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int read_len = esp_http_client_read(client, buf, content_length);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300 || read_len <= 0) {
        ESP_LOGW(TAG, "[OTA_ENGINE] GET config fail HTTP=%d select=%s body=%.*s",
                 status, select_fields, read_len > 0 ? read_len : 0, buf);
        free(buf);
        return ESP_FAIL;
    }

    buf[read_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_FAIL;

    esp_err_t result = ESP_ERR_NOT_FOUND;
    if (cJSON_IsArray(root) && cJSON_GetArraySize(root) > 0) {
        cJSON *item = cJSON_GetArrayItem(root, 0);

        cJSON *url_obj = cJSON_GetObjectItemCaseSensitive(item, "ota_url");
        if (cJSON_IsString(url_obj) && url_obj->valuestring && url_obj->valuestring[0] != '\0') {
            strncpy(out_cfg->ota_url, url_obj->valuestring, sizeof(out_cfg->ota_url) - 1);
            result = ESP_OK;
        }

        cJSON *sha_obj = cJSON_GetObjectItemCaseSensitive(item, "ota_sha256");
        if (cJSON_IsString(sha_obj) && sha_obj->valuestring && is_valid_sha256_hex(sha_obj->valuestring)) {
            strncpy(out_cfg->ota_sha256, sha_obj->valuestring, sizeof(out_cfg->ota_sha256) - 1);
            out_cfg->has_ota_sha256 = true;
        }

        cJSON *seq_obj = cJSON_GetObjectItemCaseSensitive(item, "ota_seq");
        if (cJSON_IsNumber(seq_obj)) {
            out_cfg->ota_seq = (int64_t)seq_obj->valuedouble;
            out_cfg->has_ota_seq = true;
        }
    }

    cJSON_Delete(root);
    return result;
}

/**
 * @brief Lấy OTA config từ Supabase.
 * @note  Ưu tiên schema mới ota_url,ota_seq. Nếu DB chưa có ota_seq thì fallback ota_url.
 */
static esp_err_t fetch_supabase_ota_config(ota_config_t *out_cfg) {
    esp_err_t err = fetch_supabase_ota_config_select("ota_url,ota_seq,ota_sha256", out_cfg);
    if (err == ESP_OK) return ESP_OK;

    ota_config_t fallback = {0};
    err = fetch_supabase_ota_config_select("ota_url,ota_sha256", &fallback);
    if (err == ESP_OK) {
        fallback.has_ota_seq = false;
        fallback.ota_seq = -1;
        *out_cfg = fallback;
        return ESP_OK;
    }

    ota_config_t legacy = {0};
    err = fetch_supabase_ota_config_select("ota_url", &legacy);
    if (err == ESP_OK) {
        legacy.has_ota_seq = false;
        legacy.has_ota_sha256 = false;
        legacy.ota_seq = -1;
        *out_cfg = legacy;
        return ESP_OK;
    }

    return err;
}

static bool ota_is_already_applied(const ota_config_t *cfg) {
    if (!cfg || cfg->ota_url[0] == '\0') return true;

    /*
     * Lưu ý quan trọng:
     * - SHA256 web tính là SHA256 của file .bin gốc.
     * - esp_partition_get_sha256() trên ESP-IDF trả SHA theo image/partition,
     *   có thể KHÔNG trùng raw file SHA256 của browser.
     *
     * Vì vậy chống OTA lặp phải ưu tiên so với NVS last_sha đã lưu sau OTA thành công.
     * Nếu spam upload cùng 1 file .bin nhưng URL/ota_seq khác, ota_sha256 vẫn giống last_sha -> skip.
     */
    nvs_handle_t nvs_h;
    char last_sha[SHA256_HEX_LEN + 1] = {0};
    char last_url[OTA_URL_MAX_LEN] = {0};
    int64_t last_seq = -1;
    bool have_last_sha = false;
    bool have_last_url = false;
    bool have_last_seq = false;

    if (nvs_open("ota_store", NVS_READWRITE, &nvs_h) == ESP_OK) {
        size_t sha_sz = sizeof(last_sha);
        have_last_sha = (nvs_get_str(nvs_h, "last_sha", last_sha, &sha_sz) == ESP_OK);

        size_t url_sz = sizeof(last_url);
        have_last_url = (nvs_get_str(nvs_h, "last_url", last_url, &url_sz) == ESP_OK);

        have_last_seq = (nvs_get_i64(nvs_h, "last_seq", &last_seq) == ESP_OK);
        nvs_close(nvs_h);
    }

    if (cfg->has_ota_seq && have_last_seq && cfg->ota_seq <= last_seq) {
        ESP_LOGI(TAG, "[OTA_ENGINE] Skip: ota_seq cũ. target=%lld last=%lld",
                 (long long)cfg->ota_seq, (long long)last_seq);
        return true;
    }

    if (cfg->has_ota_sha256 && have_last_sha) {
        if (strcasecmp(cfg->ota_sha256, last_sha) == 0) {
            ESP_LOGI(TAG, "[OTA_ENGINE] Skip: ota_sha256 đã apply trong NVS. sha=%s", cfg->ota_sha256);
            return true;
        }
    }

    /*
     * So running SHA chỉ là phụ trợ. Không dùng mismatch running-vs-browser để ép OTA,
     * vì hai loại SHA có thể khác hệ quy chiếu.
     */
    if (cfg->has_ota_sha256) {
        const char *running_sha = app_get_running_sha256_hex();
        if (running_sha && running_sha[0] != '\0' && strcasecmp(cfg->ota_sha256, running_sha) == 0) {
            ESP_LOGI(TAG, "[OTA_ENGINE] Skip: ota_sha256 trùng running partition sha256.");
            return true;
        }
    }

    if (!cfg->has_ota_sha256 && have_last_url && strcmp(cfg->ota_url, last_url) == 0) {
        ESP_LOGI(TAG, "[OTA_ENGINE] Skip: ota_url đã apply.");
        return true;
    }

    if (cfg->has_ota_sha256) {
        ESP_LOGI(TAG, "[OTA_ENGINE] Cần OTA: target raw-bin sha256=%s last_sha=%s running_partition_sha=%s",
                 cfg->ota_sha256,
                 have_last_sha ? last_sha : "none",
                 app_get_running_sha256_hex());
    }

    return false;
}

static void ota_mark_applied(const ota_config_t *cfg) {
    if (!cfg) return;

    nvs_handle_t nvs_h;
    if (nvs_open("ota_store", NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_str(nvs_h, "last_url", cfg->ota_url);
        if (cfg->has_ota_seq) {
            nvs_set_i64(nvs_h, "last_seq", cfg->ota_seq);
        }
        if (cfg->has_ota_sha256) {
            nvs_set_str(nvs_h, "last_sha", cfg->ota_sha256);
        }
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
}

/**
 * @brief OTA non-blocking style: begin/perform/finish, có yield để tránh Task WDT.
 */
static esp_err_t ota_perform_url_iterative(const char *ota_url) {
    if (!ota_url || ota_url[0] == '\0') return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t ota_http_cfg = {
        .url               = ota_url,
        .timeout_ms        = HTTP_OTA_TIMEOUT_MS,
        .buffer_size       = OTA_HTTP_RX_BUFFER_SIZE,
        .buffer_size_tx    = OTA_HTTP_TX_BUFFER_SIZE,
        .keep_alive_enable = true,
    };
    tls_cfg_fill(&ota_http_cfg);

    esp_https_ota_config_t ota_cfg = {
        .http_config = &ota_http_cfg,
    };

    esp_https_ota_handle_t ota_handle = NULL;

    /* Không add OTA_TASK vào Task WDT: esp_https_ota_begin/flash write có thể block lâu hơn timeout.
     * Vẫn yield trong perform loop để IDLE task chạy, tránh WDT hệ thống. */

    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[OTA_ENGINE] esp_https_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    int64_t last_log_ms = now_ms();

    while (1) {
        err = esp_https_ota_perform(ota_handle);

        /* OTA_TASK không đăng ký WDT nên không reset WDT tại đây. */

        /* Cho IDLE task chạy, tránh log task_wdt khi ghi flash lâu. */
        vTaskDelay(pdMS_TO_TICKS(1));

        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int64_t t = now_ms();
            if ((t - last_log_ms) > 5000) {
                ESP_LOGI(TAG, "[OTA_ENGINE] Đang tải/ghi OTA... written=%d bytes",
                         esp_https_ota_get_image_len_read(ota_handle));
                last_log_ms = t;
            }
            continue;
        }

        break;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[OTA_ENGINE] esp_https_ota_perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        return err;
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "[OTA_ENGINE] Firmware download chưa đủ dữ liệu.");
        esp_https_ota_abort(ota_handle);
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[OTA_ENGINE] esp_https_ota_finish failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

/**
 * @brief OTA fast check.
 * @details ESP32 kiểm tra device_configs mỗi 5 giây sau khi WiFi/NTP sẵn sàng.
 *          Đây là gần realtime, không phải chờ 30 phút như bản cũ.
 */
static void ota_task(void *pvParameters) {
    (void)pvParameters;

    vTaskDelay(pdMS_TO_TICKS(OTA_BOOT_DELAY_MS));

    while (1) {
        if (!s_wifi_connected) {
            vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_PERIOD_MS));
            continue;
        }

        ota_config_t cfg = {0};
        esp_err_t fetch_err = fetch_supabase_ota_config(&cfg);

        if (fetch_err == ESP_OK && cfg.ota_url[0] != '\0') {
            if (!ota_is_already_applied(&cfg)) {
                if (cfg.has_ota_seq) {
                    ESP_LOGW(TAG, "[OTA_ENGINE] 🚀 Firmware mới: ota_seq=%lld sha256=%s url=%s",
                             (long long)cfg.ota_seq,
                             cfg.has_ota_sha256 ? cfg.ota_sha256 : "none",
                             cfg.ota_url);
                } else {
                    ESP_LOGW(TAG, "[OTA_ENGINE] 🚀 Firmware mới: sha256=%s url=%s",
                             cfg.has_ota_sha256 ? cfg.ota_sha256 : "none",
                             cfg.ota_url);
                }

                ota_set_in_progress(true);

                /* Chờ các HTTP POST đang chạy kết thúc để không tranh TLS/CPU với OTA. */
                int wait_http_ms = 0;
                int in_flight = http_in_flight_get();
                while (in_flight > 0 && wait_http_ms < 15000) {
                    ESP_LOGW(TAG, "[OTA_ENGINE] Chờ HTTP task kết thúc trước OTA... in_flight=%d", in_flight);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    wait_http_ms += 500;
                    in_flight = http_in_flight_get();
                }

                esp_err_t ota_err = ota_perform_url_iterative(cfg.ota_url);
                if (ota_err == ESP_OK) {
                    ESP_LOGI(TAG, "[OTA_ENGINE] ✅ Nạp thành công. Lưu lịch sử và khởi động lại...");
                    ota_mark_applied(&cfg);

                    vTaskDelay(pdMS_TO_TICKS(1500));
                    esp_restart();
                } else {
                    ota_set_in_progress(false);
                    ESP_LOGE(TAG, "[OTA_ENGINE] ❌ OTA thất bại: %s. Backoff %d ms.",
                             esp_err_to_name(ota_err), OTA_FAIL_BACKOFF_MS);
                    vTaskDelay(pdMS_TO_TICKS(OTA_FAIL_BACKOFF_MS));
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_PERIOD_MS));
    }
}

/* ==============================================================================
 * 9. KẾT NỐI WIFI (BLE PROVISIONING PRODUCTION)
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

static void get_ble_service_name(char *service_name, size_t max_len);

static esp_err_t wifi_prov_mgr_init_if_needed(void) {
    if (s_prov_mgr_ready) return ESP_OK;

    network_prov_mgr_config_t prov_cfg = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE,
    };

    esp_err_t err = network_prov_mgr_init(prov_cfg);
    if (err == ESP_OK) {
        s_prov_mgr_ready = true;
    }
    return err;
}

static void wifi_prov_mgr_deinit_if_ready(void) {
    if (s_prov_mgr_ready) {
        network_prov_mgr_deinit();
        s_prov_mgr_ready = false;
    }
}

static esp_err_t wifi_start_ble_provisioning_now(const char *reason, bool clear_old_wifi_credentials) {
    char service_name[32] = {0};
    get_ble_service_name(service_name, sizeof(service_name));

    s_ble_prov_active = true;
    s_wifi_connect_allowed = false;
    s_wifi_using_fallback = false;
    s_wifi_retry_count = 0;
    s_wifi_connected = false;
    strncpy(s_ip_addr, "0.0.0.0", sizeof(s_ip_addr));
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    if (clear_old_wifi_credentials) {
        ESP_LOGW(TAG, "[WIFI] Xóa credential WiFi cũ trong NVS để nhập lại bằng BLE.");
        esp_wifi_disconnect();
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP_ERROR_CHECK(esp_wifi_restore());
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    }

    ESP_ERROR_CHECK(wifi_prov_mgr_init_if_needed());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_LOGW(TAG, "[BLE_PROV] %s", reason ? reason : "Bắt đầu BLE provisioning.");
    ESP_LOGW(TAG, "[BLE_PROV] Device name: %s", service_name);
    ESP_LOGW(TAG, "[BLE_PROV] PoP: %s", WIFI_PROV_POP);
    ESP_LOGI(TAG, "[WIFI] Production flow: BLE provisioning chờ vô hạn, nhập sai thì nhập lại, không fallback hardcoded.");

    const char *service_key = NULL; /* BLE không dùng service_key */
    return network_prov_mgr_start_provisioning(
        NETWORK_PROV_SECURITY_1,
        WIFI_PROV_POP,
        service_name,
        service_key
    );
}

static void get_ble_service_name(char *service_name, size_t max_len) {
    uint8_t eth_mac[6] = {0};
    esp_read_mac(eth_mac, ESP_MAC_WIFI_STA);
    snprintf(service_name, max_len, "%s-%02X%02X%02X",
             WIFI_PROV_SERVICE_PREFIX, eth_mac[3], eth_mac[4], eth_mac[5]);
}

static void app_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;

    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
            case NETWORK_PROV_START:
                s_ble_prov_active = true;
                s_wifi_connect_allowed = false;
                s_wifi_using_fallback = false;
                ESP_LOGI(TAG, "[BLE_PROV] Provisioning started. Dùng app ESP BLE Provisioning để nhập WiFi.");
                break;

            case NETWORK_PROV_WIFI_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                size_t pass_len = strnlen((const char *)wifi_sta_cfg->password, sizeof(wifi_sta_cfg->password));
                ESP_LOGI(TAG, "[BLE_PROV] Nhận credential SSID=%s PASS_LEN=%u",
                         (const char *)wifi_sta_cfg->ssid, (unsigned)pass_len);
                break;
            }

            case NETWORK_PROV_WIFI_CRED_FAIL: {
                network_prov_wifi_sta_fail_reason_t *reason = (network_prov_wifi_sta_fail_reason_t *)event_data;
                s_ble_prov_active = true;
                s_wifi_connect_allowed = false;
                ESP_LOGE(TAG, "[BLE_PROV] Credential fail, reason=%s",
                         (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) ? "AUTH_ERROR" : "AP_NOT_FOUND");
                break;
            }

            case NETWORK_PROV_WIFI_CRED_SUCCESS:
                s_ble_prov_active = false;
                s_wifi_connect_allowed = true;
                s_wifi_using_fallback = false;
                s_wifi_retry_count = 0;
                ESP_LOGI(TAG, "[BLE_PROV] ✅ WiFi credential hợp lệ.");
                break;

            case NETWORK_PROV_END:
                s_ble_prov_active = false;
                ESP_LOGI(TAG, "[BLE_PROV] End. Deinit provisioning manager để giải phóng BLE RAM.");
                wifi_prov_mgr_deinit_if_ready();
                break;

            default:
                break;
        }
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                if (s_ble_prov_active && !s_wifi_connect_allowed) {
                    ESP_LOGI(TAG, "[WIFI] STA_START trong BLE provisioning -> chờ credential, không tự connect.");
                } else if (s_wifi_connect_allowed) {
                    ESP_LOGI(TAG, "[WIFI] STA_START -> connect bằng credential hiện có.");
                    esp_wifi_connect();
                } else {
                    ESP_LOGI(TAG, "[WIFI] STA_START nhưng chưa có quyền connect -> skip.");
                }
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                s_wifi_connected = false;
                strncpy(s_ip_addr, "0.0.0.0", sizeof(s_ip_addr));
                xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

                if (s_ble_prov_active && !s_wifi_using_fallback) {
                    ESP_LOGW(TAG, "[WIFI] Disconnected trong BLE provisioning -> để provisioning manager xử lý, app không retry chồng.");
                    break;
                }

                if (s_wifi_connect_allowed) {
                    if (s_wifi_retry_count < WIFI_MAX_RETRY) {
                        s_wifi_retry_count++;
                        ESP_LOGW(TAG, "[WIFI] Disconnected. Retry %d/%d", s_wifi_retry_count, WIFI_MAX_RETRY);
                        esp_wifi_connect();
                    } else {
                        ESP_LOGE(TAG, "[WIFI] Kết nối credential trong NVS thất bại quá nhiều lần.");
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    }
                } else {
                    ESP_LOGW(TAG, "[WIFI] Disconnected nhưng chưa có credential/app không cho connect -> skip retry.");
                }
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&event->ip_info.ip));

        s_wifi_retry_count = 0;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "[WIFI] ✅ Connected. IP=%s RSSI=%d", s_ip_addr, app_get_wifi_rssi());
    }
}

/**
 * @brief Init WiFi production: NVS credential -> BLE provisioning vô hạn, không hardcoded fallback.
 */
static void wifi_init_ble_provisioning_or_connect(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(s_wifi_event_group ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    /* Giảm log noise từ Security1 khi BLE provisioning trao đổi khóa. */
    esp_log_level_set("security1", ESP_LOG_WARN);

    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &app_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &app_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &app_event_handler, NULL));

    ESP_ERROR_CHECK(wifi_prov_mgr_init_if_needed());

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        ESP_ERROR_CHECK(wifi_start_ble_provisioning_now("Chưa có WiFi trong NVS.", false));
    } else {
        s_ble_prov_active = false;
        s_wifi_connect_allowed = true;
        s_wifi_using_fallback = false;
        s_wifi_retry_count = 0;

        ESP_LOGI(TAG, "[WIFI] Đã có WiFi credential trong NVS. Kết nối trực tiếp.");
        wifi_prov_mgr_deinit_if_ready();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
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

    memcpy(out->plaintext, pt, SENSOR_PLAINTEXT_LEN);
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

#if LORA_ENABLE_RAM_REPLAY_CHECK
    if (s_have_last_frame_counter && counter <= s_last_frame_counter) {
        ESP_LOGW(TAG, "[LORA_RX] Bỏ frame cũ/replay. counter=%"PRIu32" last=%"PRIu32,
                 counter, s_last_frame_counter);
        return;
    }
#endif

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

    if (mlen != SENSOR_PLAINTEXT_LEN) {
        ESP_LOGE(TAG, "[LORA_RX] Plaintext length sai: %llu, expected=%d",
                 mlen, SENSOR_PLAINTEXT_LEN);
        return;
    }

#if LORA_ENABLE_RAM_REPLAY_CHECK
    s_last_frame_counter = counter;
    s_have_last_frame_counter = true;
#endif

    SensorDecodedData_t decoded;
    if (!sensor_payload_decode(plaintext, counter, &decoded)) return;

    if (decoded.payload_ok) {
        ESP_LOGI(TAG, "[LORA_RX] ✅ T=%.2f°C H=%.2f%% P=%.2fhPa",
                 decoded.env_temp_c, decoded.env_humidity_pct,
                 decoded.air_pressure_hpa);

        /* 1. Chạy MLR đồng bộ khi đồng hồ đã sync hợp lệ. */
        if (app_time_is_valid()) {
            decoded.predicted_temp_2h = app_predict_temp_2h(
                decoded.env_temp_c,
                decoded.env_humidity_pct,
                decoded.air_pressure_hpa
            );
            decoded.predicted_ok = true;
            g_mlr_predicted_temp_2h = decoded.predicted_temp_2h;
        } else {
            decoded.predicted_ok = false;
            ESP_LOGW(TAG, "[MLR_TIME] Đồng hồ chưa sync, gửi predicted_temp_2h=null cho frame=%"PRIu32, counter);
        }
    }
    
    /* 2. Đẩy dữ liệu nguyên vẹn sang hàng đợi Edge Function HTTP POST */
    if (xQueueSend(s_supabase_queue, &decoded, pdMS_TO_TICKS(10)) != pdPASS) {
        ESP_LOGW(TAG, "[LORA_RX] ⚠ Mạng chậm, Queue Edge đầy. Rớt gói để tránh nghẽn UART.");
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

    /* E32 cần thời gian để đổi mode. AUX HIGH nghĩa là module đã rảnh. */
    vTaskDelay(pdMS_TO_TICKS(100));
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

    /*
     * Rollback theo bản cũ ổn định:
     * - E32 thường echo lại 6 byte config ngay sau khi ghi C0.
     * - Một số module/clone không trả lời đúng lệnh readback C1 C1 C1,
     *   hoặc đổi baud quá nhanh làm readback về 00 00 00.
     * - Vì vậy KHÔNG dùng C1 readback làm điều kiện bắt buộc nữa.
     * - Nếu echo đúng 6 byte config thì xem như config OK.
     */
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

    /*
     * Không hard fail khi không echo đúng:
     * Có trường hợp module đã nhận config nhưng không echo đủ do timing/UART buffer.
     * Trả ESP_FAIL để thử baud kế tiếp, nhưng init vẫn sẽ đưa module về Normal và nghe thử.
     */
    ESP_LOGW(TAG, "[LORA_CFG] E32 chưa phản hồi đúng ở baud=%d", baud);
    return ESP_FAIL;
}

/**
 * @brief Cấu hình thông số mặc định cho mạch E32 khi khởi động.
 * @details Rollback về logic cũ: thử 9600 trước, nếu không được thì thử 115200.
 *          Không quét nhiều baud và không ép readback C1 vì làm module báo FAIL giả.
 */
static esp_err_t lora_e32_init_config(void) {
    ESP_LOGI(TAG, "[LORA_CFG] Bắt đầu thiết lập module E32 kiểu ổn định cũ...");

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
        /*
         * Không dừng hệ thống:
         * Nếu module đã được cấu hình từ trước hoặc readback/echo không ổn định,
         * vẫn nghe ở RUN_BAUD để không mất gói LoRa.
         */
        ESP_LOGW(TAG, "[LORA_CFG] ⚠ Không xác nhận được config E32. Vẫn chuyển sang Normal mode để nghe thử.");
        ESP_LOGW(TAG, "[LORA_CFG] Nếu không có [LORA_RX_RAW], kiểm tra TX/RX chéo, M0/M1/AUX, nguồn E32, baud và channel.");
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
    ESP_LOGI(TAG, "[VERSION] GW_VERSION=%s", GW_VERSION);
    ESP_LOGI(TAG, "[TARGET] CONFIG_IDF_TARGET=%s", CONFIG_IDF_TARGET);
    (void)app_update_running_sha256_cache();

    /* 0. NVS dùng cho WiFi provisioning + OTA. */
    app_nvs_init();

    /* 1. Timezone phải set trước khi LoRa/MLR task chạy. */
    app_set_timezone_utc7();
    app_log_time_now("[TIME_BOOT]");

    /* 2. Init MLR trước khi có frame LoRa đầu tiên. */
    mlr_engine_init();

    /* 3. Init GPIO điều khiển LoRa E32 */
    gpio_set_direction(E32_M0_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(E32_M1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(E32_AUX_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(E32_AUX_PIN, GPIO_PULLUP_ONLY);

    gpio_set_level(E32_M0_PIN, 0);
    gpio_set_level(E32_M1_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "[PINOUT] E32_TX(GPIO%d)->RXD_E32, E32_RX(GPIO%d)<-TXD_E32, M0=%d, M1=%d, AUX=%d",
             E32_UART_TX_PIN, E32_UART_RX_PIN, E32_M0_PIN, E32_M1_PIN, E32_AUX_PIN);

    /* 4. Cài đặt UART2 cho LoRa. Ban đầu dùng 9600 để config được module factory. */
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

    /* 5. Cấu hình E32: thử 9600 -> 115200, kiểm tra ACK, sau đó về Normal mode. */
    esp_err_t lora_cfg_result = lora_e32_init_config();
    if (lora_cfg_result != ESP_OK) {
        ESP_LOGW(TAG, "[BOOT] LoRa config chưa xác nhận được. Xem log [LORA_CFG] để kiểm tra phần cứng.");
    }

    /* 6. Tạo queue trước khi bật task nhận LoRa để không rớt dữ liệu. */
    s_supabase_queue = xQueueCreate(20, sizeof(SensorDecodedData_t));
    if (!s_supabase_queue) {
        ESP_LOGE(TAG, "[BOOT] Không tạo được queue Edge. Restart...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    /* 7. Bật task LoRa sớm, không chờ WiFi/NTP để tránh bỏ lỡ gói vô tuyến. */
    xTaskCreatePinnedToCore(lora_receiver_task, "LORA_RX_TASK", 8192, NULL, 10, NULL, 0);

    /* 8. WiFi production: NVS credential -> BLE provisioning vô hạn, không hardcoded fallback. */
    wifi_init_ble_provisioning_or_connect();

    while (!s_wifi_connected) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY
        );

        if (bits & WIFI_CONNECTED_BIT) {
            break;
        }

        if (bits & WIFI_FAIL_BIT) {
            ESP_LOGE(TAG, "[WIFI] Credential trong NVS kết nối thất bại quá nhiều lần -> chuyển sang BLE provisioning để nhập lại.");
            xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_ERROR_CHECK(wifi_start_ble_provisioning_now("WiFi NVS fail. Vui lòng nhập lại WiFi bằng app ESP BLE Provisioning.", true));
        }
    }

    /* 9. Đồng bộ NTP. Nếu fail thì LoRa vẫn chạy, nhưng TLS/MLR theo giờ có thể chưa chuẩn. */
    if (s_wifi_connected) {
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
            app_set_timezone_utc7();
            ESP_LOGI(TAG, "[NTP] ✅ Đồng hồ đã sync chuẩn.");
            app_log_time_now("[TIME_CHECK]");
        } else {
            ESP_LOGE(TAG, "[NTP] ❌ Đồng bộ thời gian thất bại. TLS có thể lỗi nếu đồng hồ sai.");
        }
    }

    /* 10. Bật task cloud/status sau khi network đã init. */
    xTaskCreatePinnedToCore(edge_task, "EDGE_TASK", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(gateway_status_task, "GW_STATUS_TASK", 8192, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(ota_task, "OTA_TASK", 8192, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "[BOOT] Gateway v%s đã chạy. LoRa RX -> Edge Function POST.", GW_VERSION);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
