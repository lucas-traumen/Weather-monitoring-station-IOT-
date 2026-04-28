#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"
#include "driver/uart.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

/*
 * ASCON component files needed:
 *   components/ascon/api.h
 *   components/ascon/crypto_aead.h          <-- add this header
 *   components/ascon/decrypt_tag4.c         <-- add this source
 *
 * crypto_aead_decrypt_tag4() verifies only the first 4 bytes of the ASCON tag.
 */
#include "crypto_aead.h"

/*
 * ESP32 gateway for EBYTE E32-433T30D UART LoRa module.
 *
 * - Dynamically configures E32 in Mode 3/Sleep at 9600 8N1.
 * - Switches E32 to Mode 0/Normal and receives STM32 frames at 115200 8N1.
 * - Connects to WiFi STA.
 * - Parses STM32 frame:
 *      4 bytes frame_counter little-endian
 *      9 bytes ciphertext
 *      4 bytes truncated ASCON tag
 *   Total: 17 bytes.
 *
 * IMPORTANT PIN NOTE:
 * On classic ESP32-WROOM/DevKit, GPIO34..GPIO39 are input-only.
 * Do NOT put M0/M1 on GPIO37/GPIO38 if your chip is classic ESP32.
 * If you are using ESP32-S3 and your board routes GPIO37/38/39, these pins can work.
 */

#define E32_UART_NUM          UART_NUM_2
#define E32_UART_TX_PIN       GPIO_NUM_17
#define E32_UART_RX_PIN       GPIO_NUM_18
#define E32_M0_PIN            GPIO_NUM_37
#define E32_M1_PIN            GPIO_NUM_38
#define E32_AUX_PIN           GPIO_NUM_39

#define E32_CFG_BAUDRATE      9600
#define E32_RUN_BAUDRATE      115200

#define E32_UART_RX_BUF       2048
#define E32_UART_TX_BUF       512

#define E32_FRAME_LEN         17
#define E32_FRAME_IDLE_MS     150
#define E32_AUX_TIMEOUT_MS    3000
#define E32_CFG_TIMEOUT_MS    1000

#define SENSOR_PLAINTEXT_LEN  9
#define ASCON_TAG4_LEN        4
#define ASCON_INPUT_LEN       (SENSOR_PLAINTEXT_LEN + ASCON_TAG4_LEN)

/*
 * WiFi config.
 * Replace these with your real WiFi credentials before flashing.
 */
#define WIFI_SSID             "LUCAS"
#define WIFI_PASS             "12345678"
#define WIFI_MAX_RETRY        5

#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1

/* Target E32 working parameters: C0 00 17 3A 17 44
 * ADDR   = 0x0017
 * SPEED  = 0x3A = 8N1 + local UART 115200 + air rate 2.4 kbps
 * CHAN   = 0x17 = 433 MHz for E32-433 series
 * OPTION = 0x44 = transparent + push-pull + 250 ms wake + FEC on + 30 dBm
 */
#define E32_CFG_ADDH          0x00
#define E32_CFG_ADDL          0x17
#define E32_CFG_SPEED         0x3A
#define E32_CFG_CHAN          0x17
#define E32_CFG_OPTION        0x44

#define E32_CMD_WRITE_FLASH   0xC0
#define E32_CMD_READ_CFG      0xC1
#define E32_CMD_WRITE_RAM     0xC2


#define ERR_SHT30   (1U << 0)
#define ERR_BMP388  (1U << 1)
#define ERR_VBAT    (1U << 2)

typedef struct __attribute__((packed)) {
    int16_t  env_temp_raw;     /* temperature * 100 */
    uint16_t env_hum_raw;      /* humidity * 100 */
    uint16_t air_press_raw;    /* (pressure_hPa - 900.0) * 100 */
    int8_t   board_temp_raw;   /* degC */
    uint8_t  batt_volt_raw;    /* raw/reserved */
    uint8_t  health_flag;
} SensorPayloadRaw_t;

typedef struct {
    uint32_t frame_counter;

    SensorPayloadRaw_t raw;

    float env_temp_c;
    float env_humidity_pct;
    float air_pressure_hpa;
    int   board_temp_c;
    uint8_t battery_raw;

    bool sht30_ok;
    bool bmp388_ok;
    bool vbat_ok;
    bool payload_ok;

    char health_text[64];
} SensorDecodedData_t;

static const char *TAG = "E32_GATEWAY";

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_wifi_retry_num = 0;

static const uint8_t ASCON_SECRET_KEY[16] = {
    0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
    0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C
};

typedef enum {
    E32_MODE_NORMAL = 0,     /* M1=0, M0=0 */
    E32_MODE_WAKEUP = 1,     /* M1=0, M0=1 */
    E32_MODE_POWER_SAVE = 2, /* M1=1, M0=0 */
    E32_MODE_SLEEP = 3       /* M1=1, M0=1, config mode */
} e32_mode_t;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static void dump_hex(const char *prefix, const uint8_t *data, size_t len)
{
    printf("%s", prefix);
    for (size_t i = 0; i < len; ++i) {
        printf(" %02X", data[i]);
    }
    printf("\n");
}

/* ----------------------------- WiFi STA ----------------------------- */

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_START)) {
        esp_wifi_connect();
    } else if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_DISCONNECTED)) {
        if (s_wifi_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_num++;
            ESP_LOGW(TAG, "retry WiFi connection, attempt=%d", s_wifi_retry_num);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP)) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    esp_err_t ret;

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = nvs_flash_init();
    if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) || (ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs init failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            &instance_any_id
        ),
        TAG,
        "wifi event register failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL,
            &instance_got_ip
        ),
        TAG,
        "ip event register failed"
    );

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set wifi config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    ESP_LOGI(TAG, "WiFi STA started, connecting to %s", WIFI_SSID);

    const EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(15000)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        return ESP_OK;
    }

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi failed to connect");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "WiFi connection timeout; continuing LoRa RX");
    return ESP_ERR_TIMEOUT;
}

/* ----------------------------- E32 helpers ----------------------------- */

static bool cfg_matches_target(const uint8_t cfg[6])
{
    return (cfg[1] == E32_CFG_ADDH) &&
           (cfg[2] == E32_CFG_ADDL) &&
           (cfg[3] == E32_CFG_SPEED) &&
           (cfg[4] == E32_CFG_CHAN) &&
           (cfg[5] == E32_CFG_OPTION);
}

static esp_err_t e32_wait_aux_high(uint32_t timeout_ms, const char *where)
{
    const int64_t start = now_ms();

    while (gpio_get_level(E32_AUX_PIN) == 0) {
        if ((uint32_t)(now_ms() - start) >= timeout_ms) {
            ESP_LOGE(TAG, "AUX timeout at %s", where);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return ESP_OK;
}

static esp_err_t e32_set_mode(e32_mode_t mode)
{
    int m0 = 0;
    int m1 = 0;

    switch (mode) {
    case E32_MODE_NORMAL:
        m0 = 0;
        m1 = 0;
        break;
    case E32_MODE_WAKEUP:
        m0 = 1;
        m1 = 0;
        break;
    case E32_MODE_POWER_SAVE:
        m0 = 0;
        m1 = 1;
        break;
    case E32_MODE_SLEEP:
        m0 = 1;
        m1 = 1;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(e32_wait_aux_high(E32_AUX_TIMEOUT_MS, "before mode switch"), TAG, "AUX busy");

    gpio_set_level(E32_M0_PIN, m0);
    gpio_set_level(E32_M1_PIN, m1);

    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(e32_wait_aux_high(E32_AUX_TIMEOUT_MS, "after mode switch"), TAG, "AUX busy");
    vTaskDelay(pdMS_TO_TICKS(3));

    return ESP_OK;
}

static esp_err_t uart_set_baud(uint32_t baudrate)
{
    ESP_RETURN_ON_ERROR(uart_wait_tx_done(E32_UART_NUM, pdMS_TO_TICKS(200)), TAG, "uart tx wait failed");
    ESP_RETURN_ON_ERROR(uart_set_baudrate(E32_UART_NUM, baudrate), TAG, "uart set baud failed");
    ESP_RETURN_ON_ERROR(uart_flush_input(E32_UART_NUM), TAG, "uart flush input failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static void board_gpio_init(void)
{
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << E32_M0_PIN) | (1ULL << E32_M1_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_conf));

    gpio_set_level(E32_M0_PIN, 0);
    gpio_set_level(E32_M1_PIN, 0);

    gpio_config_t aux_conf = {
        .pin_bit_mask = (1ULL << E32_AUX_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&aux_conf));
}

static void uart_init(uint32_t baudrate)
{
    const uart_config_t uart_conf = {
        .baud_rate = (int)baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(E32_UART_NUM, E32_UART_RX_BUF, E32_UART_TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(E32_UART_NUM, &uart_conf));
    ESP_ERROR_CHECK(uart_set_pin(E32_UART_NUM, E32_UART_TX_PIN, E32_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(gpio_set_pull_mode(E32_UART_RX_PIN, GPIO_PULLUP_ONLY));
    ESP_ERROR_CHECK(uart_flush_input(E32_UART_NUM));
}

static esp_err_t e32_find_config_response(const uint8_t *buf, size_t len, uint8_t out_cfg[6])
{
    if (len < 6) {
        return ESP_ERR_NOT_FOUND;
    }

    for (size_t i = 0; i + 6 <= len; ++i) {
        const uint8_t head = buf[i];
        if ((head == E32_CMD_WRITE_FLASH) || (head == E32_CMD_WRITE_RAM) || (head == E32_CMD_READ_CFG)) {
            memcpy(out_cfg, &buf[i], 6);
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t e32_read_config(uint8_t out_cfg[6])
{
    const uint8_t cmd[3] = { E32_CMD_READ_CFG, E32_CMD_READ_CFG, E32_CMD_READ_CFG };
    uint8_t rx[64];
    size_t total = 0;
    const int64_t start = now_ms();

    memset(rx, 0, sizeof(rx));
    ESP_RETURN_ON_ERROR(e32_wait_aux_high(E32_AUX_TIMEOUT_MS, "read config"), TAG, "AUX busy");
    ESP_ERROR_CHECK(uart_flush_input(E32_UART_NUM));

    const int written = uart_write_bytes(E32_UART_NUM, cmd, sizeof(cmd));
    if (written != (int)sizeof(cmd)) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(uart_wait_tx_done(E32_UART_NUM, pdMS_TO_TICKS(200)), TAG, "config command tx failed");

    while ((uint32_t)(now_ms() - start) < E32_CFG_TIMEOUT_MS) {
        const int rd = uart_read_bytes(E32_UART_NUM, &rx[total], sizeof(rx) - total, pdMS_TO_TICKS(20));
        if (rd < 0) {
            return ESP_FAIL;
        }
        if (rd > 0) {
            total += (size_t)rd;
            if (e32_find_config_response(rx, total, out_cfg) == ESP_OK) {
                return ESP_OK;
            }
            if (total >= sizeof(rx)) {
                break;
            }
        }
    }

    if (total > 0) {
        dump_hex("[E32_CFG_RX_UNPARSED]", rx, total);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t e32_write_target_config(void)
{
    const uint8_t pkt[6] = {
        E32_CMD_WRITE_FLASH,
        E32_CFG_ADDH,
        E32_CFG_ADDL,
        E32_CFG_SPEED,
        E32_CFG_CHAN,
        E32_CFG_OPTION,
    };

    ESP_RETURN_ON_ERROR(e32_wait_aux_high(E32_AUX_TIMEOUT_MS, "write config"), TAG, "AUX busy");
    ESP_ERROR_CHECK(uart_flush_input(E32_UART_NUM));

    dump_hex("[E32_CFG_WRITE]", pkt, sizeof(pkt));
    const int written = uart_write_bytes(E32_UART_NUM, pkt, sizeof(pkt));
    if (written != (int)sizeof(pkt)) {
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(uart_wait_tx_done(E32_UART_NUM, pdMS_TO_TICKS(200)), TAG, "config write tx failed");
    ESP_RETURN_ON_ERROR(e32_wait_aux_high(E32_AUX_TIMEOUT_MS, "after config write"), TAG, "AUX busy after config write");
    vTaskDelay(pdMS_TO_TICKS(80));
    return ESP_OK;
}

static esp_err_t e32_dynamic_configure(void)
{
    uint8_t cfg[6] = {0};

    ESP_LOGI(TAG, "dynamic E32 config start: target C0 %02X %02X %02X %02X %02X",
             E32_CFG_ADDH, E32_CFG_ADDL, E32_CFG_SPEED, E32_CFG_CHAN, E32_CFG_OPTION);

    for (int attempt = 1; attempt <= 3; ++attempt) {
        ESP_LOGI(TAG, "config attempt %d", attempt);

        ESP_RETURN_ON_ERROR(e32_set_mode(E32_MODE_SLEEP), TAG, "cannot enter sleep/config mode");
        ESP_RETURN_ON_ERROR(uart_set_baud(E32_CFG_BAUDRATE), TAG, "cannot set config baud");

        esp_err_t read_err = e32_read_config(cfg);
        if (read_err == ESP_OK) {
            dump_hex("[E32_CFG_READ]", cfg, sizeof(cfg));
            if (cfg_matches_target(cfg)) {
                ESP_LOGI(TAG, "E32 already has target config");
                return ESP_OK;
            }
            ESP_LOGW(TAG, "E32 config differs; rewriting");
        } else {
            ESP_LOGW(TAG, "read config failed: %s; rewriting anyway", esp_err_to_name(read_err));
        }

        ESP_RETURN_ON_ERROR(e32_write_target_config(), TAG, "write target config failed");

        memset(cfg, 0, sizeof(cfg));
        read_err = e32_read_config(cfg);
        if (read_err == ESP_OK) {
            dump_hex("[E32_CFG_VERIFY]", cfg, sizeof(cfg));
            if (cfg_matches_target(cfg)) {
                ESP_LOGI(TAG, "E32 config verified OK");
                return ESP_OK;
            }
            ESP_LOGW(TAG, "verify mismatch after write");
        } else {
            ESP_LOGW(TAG, "verify read failed after write: %s", esp_err_to_name(read_err));
        }

        vTaskDelay(pdMS_TO_TICKS(300));
    }

    return ESP_FAIL;
}

/* ----------------------------- ASCON tag4 parser ----------------------------- */

static void build_nonce_from_counter(uint32_t counter, uint8_t nonce[16])
{
    memset(nonce, 0, 16);

    /* Must match STM32 Edge_Crypto_Pack(). */
    nonce[0] = (uint8_t)(counter >> 24);
    nonce[1] = (uint8_t)(counter >> 16);
    nonce[2] = (uint8_t)(counter >> 8);
    nonce[3] = (uint8_t)(counter);

    nonce[4] = 0x57U;
    nonce[5] = 0x53U;
    nonce[6] = 0x4EU;
    nonce[7] = 0x31U;
}
static int16_t read_i16_le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void sensor_health_to_text(uint8_t flags, char *out, size_t out_len)
{
    if ((out == NULL) || (out_len == 0)) {
        return;
    }

    out[0] = '\0';

    if (flags == 0U) {
        snprintf(out, out_len, "OK");
        return;
    }

    if ((flags & ERR_SHT30) != 0U) {
        strncat(out, "SHT30_ERR ", out_len - strlen(out) - 1);
    }

    if ((flags & ERR_BMP388) != 0U) {
        strncat(out, "BMP388_ERR ", out_len - strlen(out) - 1);
    }

    if ((flags & ERR_VBAT) != 0U) {
        strncat(out, "VBAT_ERR ", out_len - strlen(out) - 1);
    }
}

static bool sensor_payload_decode(const uint8_t plaintext[SENSOR_PLAINTEXT_LEN],
                                  uint32_t frame_counter,
                                  SensorDecodedData_t *out)
{
    if ((plaintext == NULL) || (out == NULL)) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    out->frame_counter = frame_counter;

    /*
     * STM32 sends packed SensorData_t as 9 bytes:
     * byte 0..1 : int16_t  env_temp_raw
     * byte 2..3 : uint16_t env_hum_raw
     * byte 4..5 : uint16_t air_press_raw
     * byte 6    : int8_t   board_temp_raw
     * byte 7    : uint8_t  batt_volt_raw
     * byte 8    : uint8_t  health_flag
     */
    out->raw.env_temp_raw   = read_i16_le(&plaintext[0]);
    out->raw.env_hum_raw    = read_u16_le(&plaintext[2]);
    out->raw.air_press_raw  = read_u16_le(&plaintext[4]);
    out->raw.board_temp_raw = (int8_t)plaintext[6];
    out->raw.batt_volt_raw  = plaintext[7];
    out->raw.health_flag    = plaintext[8];

    out->sht30_ok =
        ((out->raw.health_flag & ERR_SHT30) == 0U) &&
        (out->raw.env_temp_raw != (int16_t)0xFFFF) &&
        (out->raw.env_hum_raw != 0xFFFFU);

    out->bmp388_ok =
        ((out->raw.health_flag & ERR_BMP388) == 0U) &&
        (out->raw.air_press_raw != 0xFFFFU) &&
        (out->raw.board_temp_raw != (int8_t)0xFF);

    out->vbat_ok =
        ((out->raw.health_flag & ERR_VBAT) == 0U);

    out->payload_ok = out->sht30_ok && out->bmp388_ok;

    if (out->sht30_ok) {
        out->env_temp_c = ((float)out->raw.env_temp_raw) / 100.0f;
        out->env_humidity_pct = ((float)out->raw.env_hum_raw) / 100.0f;
    }

    if (out->bmp388_ok) {
        out->air_pressure_hpa = 900.0f + (((float)out->raw.air_press_raw) / 100.0f);
        out->board_temp_c = (int)out->raw.board_temp_raw;
    }

    out->battery_raw = out->raw.batt_volt_raw;

    sensor_health_to_text(out->raw.health_flag,
                          out->health_text,
                          sizeof(out->health_text));

    return true;
}

static void sensor_payload_print(const SensorDecodedData_t *data)
{
    if (data == NULL) {
        return;
    }

    ESP_LOGI(TAG, "========== DECODED SENSOR DATA ==========");
    ESP_LOGI(TAG, "Frame counter : %" PRIu32, data->frame_counter);

    if (data->sht30_ok) {
        ESP_LOGI(TAG, "Env temp      : %.2f degC", data->env_temp_c);
        ESP_LOGI(TAG, "Env humidity  : %.2f %%", data->env_humidity_pct);
    } else {
        ESP_LOGW(TAG, "Env temp      : N/A");
        ESP_LOGW(TAG, "Env humidity  : N/A");
    }

    if (data->bmp388_ok) {
        ESP_LOGI(TAG, "Air pressure  : %.2f hPa", data->air_pressure_hpa);
        ESP_LOGI(TAG, "Board temp    : %d degC", data->board_temp_c);
    } else {
        ESP_LOGW(TAG, "Air pressure  : N/A");
        ESP_LOGW(TAG, "Board temp    : N/A");
    }

    ESP_LOGI(TAG, "Battery raw   : %u%s",
             data->battery_raw,
             data->vbat_ok ? "" : " (VBAT error)");

    ESP_LOGI(TAG, "Health flag   : 0x%02X (%s)",
             data->raw.health_flag,
             data->health_text);

    ESP_LOGI(TAG,
             "RAW fields    : T=%d H=%u P=%u BT=%d VB=%u HF=0x%02X",
             data->raw.env_temp_raw,
             data->raw.env_hum_raw,
             data->raw.air_press_raw,
             data->raw.board_temp_raw,
             data->raw.batt_volt_raw,
             data->raw.health_flag);

    ESP_LOGI(TAG, "=========================================");
}
static void process_frame(const uint8_t frame[E32_FRAME_LEN])
{
    static bool have_last_counter = false;
    static uint32_t last_counter = 0;

    uint32_t counter;
    uint8_t nonce[16];
    uint8_t ascon_input[ASCON_INPUT_LEN];
    uint8_t plaintext[SENSOR_PLAINTEXT_LEN];
    unsigned long long mlen = 0ULL;
    int dec_ret;

    /* STM32 sends uint32_t frame_counter in little-endian memory order. */
    counter =
        ((uint32_t)frame[0]) |
        ((uint32_t)frame[1] << 8) |
        ((uint32_t)frame[2] << 16) |
        ((uint32_t)frame[3] << 24);

    ESP_LOGI(TAG, "RX frame_counter=%" PRIu32, counter);

    dump_hex("[FRAME_17B]", frame, E32_FRAME_LEN);
    dump_hex("[CIPHERTEXT_9B]", &frame[4], 9);
    dump_hex("[ASCON_TAG_4B]", &frame[13], 4);

    if (have_last_counter) {
        const uint32_t expected = last_counter + 1U;
        if (counter != expected) {
            ESP_LOGW(TAG,
                     "counter jump: last=%" PRIu32 ", now=%" PRIu32 ", expected=%" PRIu32,
                     last_counter, counter, expected);
        }
    }

    last_counter = counter;
    have_last_counter = true;

    /* crypto_aead_decrypt_tag4() expects ciphertext[9] || tag4[4]. */
    memcpy(&ascon_input[0], &frame[4], 9);
    memcpy(&ascon_input[9], &frame[13], 4);

    build_nonce_from_counter(counter, nonce);

    memset(plaintext, 0, sizeof(plaintext));

    dec_ret = crypto_aead_decrypt_tag4(
        plaintext,
        &mlen,
        NULL,
        ascon_input,
        sizeof(ascon_input),
        NULL,
        0,
        nonce,
        ASCON_SECRET_KEY
    );

    if ((dec_ret != 0) || (mlen != SENSOR_PLAINTEXT_LEN)) {
        ESP_LOGE(TAG, "ASCON tag4 decrypt/verify FAILED ret=%d mlen=%llu",
                 dec_ret, mlen);
        return;
    }

    ESP_LOGI(TAG, "ASCON tag4 decrypt OK");
    dump_hex("[PLAINTEXT_9B]", plaintext, sizeof(plaintext));
	SensorDecodedData_t decoded;

	if (sensor_payload_decode(plaintext, counter, &decoded)) {
	    sensor_payload_print(&decoded);
	} else {
	    ESP_LOGE(TAG, "Sensor payload decode FAILED");
	}
}

/* ----------------------------- LoRa receiver ----------------------------- */

static void receiver_loop(void)
{
    uint8_t rx[64];
    uint8_t frame[E32_FRAME_LEN];
    size_t frame_pos = 0;
    int64_t last_byte_time = 0;

    ESP_LOGI(TAG, "receiver loop started; waiting for %d-byte STM32 frames", E32_FRAME_LEN);

    while (1) {
        const int rd = uart_read_bytes(E32_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(50));
        const int64_t t_now = now_ms();

        if (rd < 0) {
            ESP_LOGE(TAG, "uart_read_bytes failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (rd == 0) {
            if ((frame_pos > 0) && ((t_now - last_byte_time) > 1000)) {
                ESP_LOGW(TAG, "discard stale partial frame len=%u", (unsigned)frame_pos);
                dump_hex("[PARTIAL_DROP]", frame, frame_pos);
                frame_pos = 0;
            }
            continue;
        }

        ESP_LOGI(TAG, "RAW UART RX len=%d", rd);
        dump_hex("[RAW_RX]", rx, (size_t)rd);

        for (int i = 0; i < rd; ++i) {
            const int64_t b_now = now_ms();

            if ((frame_pos > 0) && ((b_now - last_byte_time) > E32_FRAME_IDLE_MS)) {
                ESP_LOGW(TAG, "idle gap, discard partial frame len=%u", (unsigned)frame_pos);
                dump_hex("[PARTIAL_DROP]", frame, frame_pos);
                frame_pos = 0;
            }

            frame[frame_pos++] = rx[i];
            last_byte_time = b_now;

            if (frame_pos == E32_FRAME_LEN) {
                process_frame(frame);
                frame_pos = 0;
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 E32 gateway boot");
    ESP_LOGI(TAG, "UART%d TX=%d RX=%d | M0=%d M1=%d AUX=%d",
             E32_UART_NUM, E32_UART_TX_PIN, E32_UART_RX_PIN, E32_M0_PIN, E32_M1_PIN, E32_AUX_PIN);

    board_gpio_init();
    uart_init(E32_CFG_BAUDRATE);

    esp_err_t wifi_err = wifi_init_sta();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi not ready: %s; continue LoRa RX", esp_err_to_name(wifi_err));
    }

    esp_err_t cfg_err = e32_dynamic_configure();
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "dynamic E32 config failed: %s", esp_err_to_name(cfg_err));
        ESP_LOGE(TAG, "continuing in normal RX at 115200; if no frame arrives, check M0/M1 output-capable pins, AUX, TX/RX cross, common GND, and power");
    }

    esp_err_t mode_err = e32_set_mode(E32_MODE_NORMAL);
    if (mode_err != ESP_OK) {
        ESP_LOGE(TAG, "cannot confirm normal mode with AUX: %s", esp_err_to_name(mode_err));
        ESP_LOGW(TAG, "forcing M0=0 M1=0 and continuing RX");
        gpio_set_level(E32_M0_PIN, 0);
        gpio_set_level(E32_M1_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_ERROR_CHECK(uart_set_baud(E32_RUN_BAUDRATE));
    ESP_ERROR_CHECK(uart_flush_input(E32_UART_NUM));

    ESP_LOGI(TAG, "E32 normal mode requested, UART%d @ %d, target params: ADDR=0x%02X%02X SPEED=0x%02X CHAN=0x%02X OPTION=0x%02X",
             E32_UART_NUM, E32_RUN_BAUDRATE, E32_CFG_ADDH, E32_CFG_ADDL,
             E32_CFG_SPEED, E32_CFG_CHAN, E32_CFG_OPTION);

    receiver_loop();
}

