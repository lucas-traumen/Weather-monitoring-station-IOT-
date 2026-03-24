#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"

#include "min.h" // Thư viện MIN Protocol

static const char *TAG = "GATEWAY_MIN";

/* ─── CẤU HÌNH UART ─── */
#define UART_PORT_NUM      UART_NUM_1
#define UART_BAUD_RATE     115200
#define UART_TX_PIN        17  // Nối với RX của STM32
#define UART_RX_PIN        18  // Nối với TX của STM32

#define MIN_ID_TELEMETRY   0x01

/* Khai báo Context cho MIN */
struct min_context min_ctx;

/* ══════════════════════════════════════════════════════════════════════════
 * 1. KẾT NỐI WI-FI (STATION MODE)
 * ══════════════════════════════════════════════════════════════════════════ */
#define WIFI_SSID      "LUCAS" // Bác nhớ điền tên WiFi
#define WIFI_PASS      "12345678"    // Và mật khẩu vào đây nhé


static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI("WIFI", "Đang kết nối đến AP...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGW("WIFI", "Mất kết nối, đang thử lại...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI("WIFI", "KẾT NỐI THÀNH CÔNG! IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );
}

/* ══════════════════════════════════════════════════════════════════════════
 * 2. CÁC HÀM XỬ LÝ MIN PROTOCOL
 * ══════════════════════════════════════════════════════════════════════════ */
uint32_t min_time_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

uint16_t min_tx_space(uint8_t port) { return 512; }

void min_tx_byte(uint8_t port, uint8_t byte) {
    uart_write_bytes(UART_PORT_NUM, (const char *)&byte, 1);
}

void min_tx_start(uint8_t port) { }
void min_tx_finished(uint8_t port) { }

/* Hàm giải mã cục data 13 bytes từ STM32 */
void min_application_handler(uint8_t min_id, uint8_t const *min_payload, uint8_t len_payload, uint8_t port) 
{
    if (min_id == MIN_ID_TELEMETRY && len_payload == 13) {
        uint8_t idx = 0;
        
        int16_t t_out_raw = (min_payload[idx] << 8) | min_payload[idx+1]; idx += 2;
        float temp_out = t_out_raw / 100.0f;

        uint16_t h_out_raw = (min_payload[idx] << 8) | min_payload[idx+1]; idx += 2;
        float hum_out = h_out_raw / 100.0f;

        int16_t t_board_raw = (min_payload[idx] << 8) | min_payload[idx+1]; idx += 2;
        float temp_board = t_board_raw / 100.0f;

        uint32_t press_raw = (min_payload[idx] << 24) | (min_payload[idx+1] << 16) | 
                             (min_payload[idx+2] << 8)  | min_payload[idx+3]; idx += 4;
        float pressure = press_raw / 100.0f;

        int16_t alt_raw = (min_payload[idx] << 8) | min_payload[idx+1]; idx += 2;
        float altitude = alt_raw / 10.0f;

        uint8_t battery = min_payload[idx];

        ESP_LOGI(TAG, "Môi trường: %.2f °C | %.2f %%RH", temp_out, hum_out);
        ESP_LOGI(TAG, "Bo mạch   : %.2f °C | %.2f hPa | %.1f m", temp_board, pressure, altitude);
        ESP_LOGI(TAG, "Pin       : %d %%", battery);
        ESP_LOGI(TAG, "-----------------------------------");
    }
}

/* Task ngầm chạy song song để hứng dữ liệu UART */
static void uart_rx_task(void *arg)
{
    uint8_t rx_buf[128];
    while (1) {
        int rx_bytes = uart_read_bytes(UART_PORT_NUM, rx_buf, sizeof(rx_buf), 20 / portTICK_PERIOD_MS);
        if (rx_bytes > 0) {
            min_poll(&min_ctx, rx_buf, (uint32_t)rx_bytes);
        } else {
            min_poll(&min_ctx, NULL, 0);
        }
    }
}
#ifdef MIN_DEBUG_PRINTING
void min_debug_print(const char *msg, ...)
{
    char buf[256]; // Bộ đệm tạm để chứa chuỗi
    va_list args;

    /* 1. Gom tất cả các tham số (..., %d, %f) lại thành một chuỗi hoàn chỉnh */
    va_start(args, msg);
    vsnprintf(buf, sizeof(buf), msg, args);
    va_end(args);

    /* 2. Nhét thêm chữ [MIN] vào trước để dễ phân biệt, rồi đẩy vào kho DMA */
	ESP_LOGW("MIN_CORE", " %s",buf);
}
#endif
/* ══════════════════════════════════════════════════════════════════════════
 * 3. HÀM MAIN (CHỈ CHẠY 1 LẦN LÚC CẤP NGUỒN)
 * ══════════════════════════════════════════════════════════════════════════ */
void app_main(void)
{
    ESP_LOGI(TAG, "Khởi động Gateway ESP32-S3...");

    /* 0. Khởi tạo bộ nhớ Flash (Bắt buộc phải làm trước khi gọi Wi-Fi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 1. Khởi tạo và kết nối Wi-Fi */
    wifi_init_sta();

    /* 2. Cấu hình phần cứng UART */
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, 1024 * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* 3. Khởi tạo bộ đệm MIN và phóng Task đọc UART */
    min_init_context(&min_ctx, 0);
    xTaskCreate(uart_rx_task, "uart_rx_task", 4096, NULL, 10, NULL);

    /* 4. Vòng lặp duy trì hệ thống */
    while (true) {
        // Mọi thứ đã được các Task và ngắt xử lý, hàm main chỉ cần ngủ để nhường CPU
        vTaskDelay(pdMS_TO_TICKS(10000)); // Ngủ 10 giây
    }
}
