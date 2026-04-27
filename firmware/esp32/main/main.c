#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"

/*
 * ESP32 gateway for EBYTE E32-433T30D UART LoRa module.
 * - Dynamically configures E32 in Mode 3/Sleep at 9600 8N1.
 * - Switches E32 to Mode 0/Normal and receives STM32 frames at 115200 8N1.
 * - Expected frame from STM32: 15 bytes:
 *     uint16_t frame_counter little-endian + 9 bytes ciphertext + 4 bytes MAC tag.
 *
 * IMPORTANT PIN NOTE:
 * On classic ESP32-WROOM/DevKit, GPIO34..GPIO39 are input-only. Do NOT put M0/M1
 * on GPIO37/GPIO38 if you need dynamic configuration. Default below uses:
 *   ESP32 TX17 -> E32 RXD
 *   ESP32 RX18 <- E32 TXD
 *   ESP32 GPIO26 -> E32 M0
 *   ESP32 GPIO27 -> E32 M1
 *   ESP32 GPIO34 <- E32 AUX
 * If you are using ESP32-S3 and your board really routes GPIO37/38/39, you may
 * change the three mode/AUX defines below.
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

#define E32_FRAME_LEN         15
#define E32_FRAME_IDLE_MS     150
#define E32_AUX_TIMEOUT_MS    3000
#define E32_CFG_TIMEOUT_MS    1000

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

static const char *TAG = "E32_GATEWAY";

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

    /* Datasheet recommends waiting after AUX high; use a conservative margin. */
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

static void process_frame(const uint8_t frame[E32_FRAME_LEN])
{
    static bool have_last_counter = false;
    static uint16_t last_counter = 0;

    const uint16_t counter = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);

    ESP_LOGI(TAG, "RX frame_counter=%" PRIu16, counter);
    dump_hex("[FRAME]", frame, E32_FRAME_LEN);
    dump_hex("[CIPHERTEXT_9B]", &frame[2], 9);
    dump_hex("[MAC_TAG_4B]", &frame[11], 4);

    if (have_last_counter) {
        const uint16_t expected = (uint16_t)(last_counter + 1U);
        if (counter != expected) {
            ESP_LOGW(TAG, "counter jump: last=%" PRIu16 ", now=%" PRIu16 ", expected=%" PRIu16,
                     last_counter, counter, expected);
        }
    }

    last_counter = counter;
    have_last_counter = true;
}

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
