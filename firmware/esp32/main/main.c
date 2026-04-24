#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "Lora.h"

/* =========================
 * PIN MAP
 * ========================= */
#define LORA_UART_NUM          UART_NUM_2
#define LORA_UART_TX_PIN       GPIO_NUM_17
#define LORA_UART_RX_PIN       GPIO_NUM_18

#define LORA_M0_PIN            GPIO_NUM_37
#define LORA_M1_PIN            GPIO_NUM_38
#define LORA_AUX_PIN           GPIO_NUM_39

/* =========================
 * BAUD
 * =========================
 * E32 config mode: 9600
 * E32 run mode:    115200 (khớp với STM32 node hiện tại)
 */
#define LORA_CFG_BAUDRATE      9600
#define LORA_RUN_BAUDRATE      115200

/* =========================
 * FRAME / BUFFER
 * ========================= */
#define UART_BUF_SIZE          256
#define FRAME_SIZE             15
#define BURST_BUF_SIZE         64
#define BURST_GAP_MS           20

/* =========================
 * APP MODE
 * =========================
 * 0 = normal E32 receive
 * 1 = UART2 basic test
 * 2 = UART2 loopback test (cần nối tạm TX17 -> RX18)
 * 3 = UART2 RX activity watch
 */
#define APP_MODE_NORMAL_RX     0
#define APP_MODE_UART_BASIC    1
#define APP_MODE_UART_LOOPBACK 2
#define APP_MODE_UART_RX_WATCH 3

#define APP_MODE               APP_MODE_NORMAL_RX

static const char *TAG = "E32_TEST";

typedef struct {
    uart_port_t uart_num;
} app_ctx_t;

static app_ctx_t g_ctx = {
    .uart_num = LORA_UART_NUM
};

static void check_e32(int ret, const char *where)
{
    if (ret != E32_OK) {
        ESP_LOGE(TAG, "%s failed, ret=%d", where, ret);
        abort();
    }
}

static int port_uart_write(void *user, const uint8_t *data, size_t len)
{
    app_ctx_t *ctx = (app_ctx_t *)user;
    return uart_write_bytes(ctx->uart_num, data, len);
}

static int port_uart_read(void *user, uint8_t *data, size_t len, uint32_t timeout_ms)
{
    app_ctx_t *ctx = (app_ctx_t *)user;
    return uart_read_bytes(ctx->uart_num, data, len, pdMS_TO_TICKS(timeout_ms));
}

static void port_set_m0(void *user, int level)
{
    (void)user;
    gpio_set_level((gpio_num_t)LORA_M0_PIN, level);
}

static void port_set_m1(void *user, int level)
{
    (void)user;
    gpio_set_level((gpio_num_t)LORA_M1_PIN, level);
}

static int port_get_aux(void *user)
{
    (void)user;
    return gpio_get_level((gpio_num_t)LORA_AUX_PIN);
}

static void port_delay_ms(void *user, uint32_t ms)
{
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static uint32_t port_tick_ms(void *user)
{
    (void)user;
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void dump_hex(const char *prefix, const uint8_t *data, size_t len)
{
    printf("%s", prefix);
    for (size_t i = 0; i < len; i++) {
        printf(" %02X", data[i]);
    }
    printf("\n");
}

static void uart_lora_gpio_init(void)
{
    gpio_config_t io_m0 = {
        .pin_bit_mask = (1ULL << LORA_M0_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_m0));

    gpio_config_t io_m1 = {
        .pin_bit_mask = (1ULL << LORA_M1_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_m1));

    gpio_config_t io_aux = {
        .pin_bit_mask = (1ULL << LORA_AUX_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_aux));
}

static void uart_lora_driver_init(uint32_t baudrate)
{
    const uart_config_t cfg = {
        .baud_rate = (int)baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(LORA_UART_NUM, UART_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LORA_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(
        LORA_UART_NUM,
        LORA_UART_TX_PIN,
        LORA_UART_RX_PIN,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    ));

    /* Giữ RX sạch mức idle */
    ESP_ERROR_CHECK(gpio_set_pull_mode(LORA_UART_RX_PIN, GPIO_PULLUP_ONLY));
}

static void uart_lora_set_baud(uint32_t baudrate)
{
    ESP_ERROR_CHECK(uart_set_baudrate(LORA_UART_NUM, baudrate));
    uart_flush(LORA_UART_NUM);
}

/* =========================
 * UART2 CHECK FUNCTIONS
 * ========================= */
static void uart2_check_basic(void)
{
    uint32_t baud = 0;
    size_t buffered = 0;
    const char *msg = "UART2_BASIC_TEST\r\n";
    int wr;

    ESP_ERROR_CHECK(uart_get_baudrate(LORA_UART_NUM, &baud));
    ESP_LOGI(TAG, "[UART2] baud=%" PRIu32, baud);

    ESP_ERROR_CHECK(uart_get_buffered_data_len(LORA_UART_NUM, &buffered));
    ESP_LOGI(TAG, "[UART2] buffered before tx=%u", (unsigned)buffered);

    wr = uart_write_bytes(LORA_UART_NUM, msg, strlen(msg));
    if (wr < 0) {
        ESP_LOGE(TAG, "[UART2] uart_write_bytes failed");
        return;
    }

    ESP_LOGI(TAG, "[UART2] wrote %d bytes", wr);
    ESP_ERROR_CHECK(uart_wait_tx_done(LORA_UART_NUM, pdMS_TO_TICKS(200)));
    ESP_LOGI(TAG, "[UART2] tx done");
}

static void uart2_check_loopback(void)
{
    static const uint8_t pattern[] = {
        0x55, 0xAA, 0x00, 0x11, 0x22, 0x33, 0x5A, 0xA5
    };
    uint8_t rx[sizeof(pattern)] = {0};
    int wr, rd;

    ESP_LOGI(TAG, "[UART2] loopback test start");
    ESP_LOGI(TAG, "[UART2] jumper required: TX%d -> RX%d",
             LORA_UART_TX_PIN, LORA_UART_RX_PIN);

    uart_flush(LORA_UART_NUM);
    uart_flush_input(LORA_UART_NUM);

    wr = uart_write_bytes(LORA_UART_NUM, pattern, sizeof(pattern));
    if (wr != (int)sizeof(pattern)) {
        ESP_LOGE(TAG, "[UART2] loopback write failed, wr=%d", wr);
        return;
    }

    ESP_ERROR_CHECK(uart_wait_tx_done(LORA_UART_NUM, pdMS_TO_TICKS(200)));

    rd = uart_read_bytes(LORA_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "[UART2] loopback read %d bytes", rd);

    dump_hex("[UART2_TX]", pattern, sizeof(pattern));
    dump_hex("[UART2_RX]", rx, (rd > 0) ? (size_t)rd : 0);

    if ((rd == (int)sizeof(pattern)) && (memcmp(pattern, rx, sizeof(pattern)) == 0)) {
        ESP_LOGI(TAG, "[UART2] loopback PASS");
    } else {
        ESP_LOGW(TAG, "[UART2] loopback FAIL");
    }
}

static void uart2_check_rx_activity(uint32_t watch_ms)
{
    uint8_t buf[64];
    uint32_t start = port_tick_ms(NULL);

    ESP_LOGI(TAG, "[UART2] watch RX for %" PRIu32 " ms", watch_ms);

    while ((port_tick_ms(NULL) - start) < watch_ms) {
        int rd = uart_read_bytes(LORA_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (rd > 0) {
            ESP_LOGI(TAG, "[UART2] rx activity: %d bytes", rd);
            dump_hex("[UART2_RX_ACTIVITY]", buf, (size_t)rd);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    ESP_LOGI(TAG, "[UART2] watch done");
}

/* =========================
 * E32 NORMAL RX
 * ========================= */
static void process_burst(const uint8_t *buf, size_t len)
{
    if (len == 0) {
        return;
    }

    if (len != FRAME_SIZE) {
        ESP_LOGW(TAG, "drop burst len=%u", (unsigned)len);
        dump_hex("[DROP]", buf, len);
        return;
    }

    /* STM32F1 gửi raw struct little-endian */
    uint16_t counter = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);

    ESP_LOGI(TAG, "frame_counter=%u", counter);
    dump_hex("[FRAME]", buf, len);
}

static void run_normal_rx(void)
{
    e32_t radio;
    e32_io_t io = {
        .user       = &g_ctx,
        .uart_write = port_uart_write,
        .uart_read  = port_uart_read,
        .set_m0     = port_set_m0,
        .set_m1     = port_set_m1,
        .get_aux    = port_get_aux,
        .delay_ms   = port_delay_ms,
        .tick_ms    = port_tick_ms,
    };

    /* Match với node STM32:
       ADDR  = 0x0017
       SPEED = 0x3A = 115200, 8N1, 2.4Kbps
       CHAN  = 0x17
       OPT   = 0x44 = transparent, push-pull, 250ms, FEC on */
    e32_config_t cfg = {
        .addh = 0x00,
        .addl = 0x17,
        .speed = E32_UART_8N1 | E32_BAUD_115200 | E32_AIR_24K,
        .chan = 0x17,
        .option = E32_TRANS_TRANSPARENT |
                  E32_IO_PUSH_PULL |
                  E32_WAKE_250MS |
                  E32_FEC_ON |
                  E32_PWR_30DBM
    };

    uint8_t rx_chunk[32];
    uint8_t burst_buf[BURST_BUF_SIZE];
    size_t burst_pos = 0;
    uint32_t last_rx_ms = 0;

    /* Step 1: config baud 9600 */
    uart_lora_driver_init(LORA_CFG_BAUDRATE);
    uart_flush(LORA_UART_NUM);

    check_e32(e32_init(&radio, &io, 2000), "e32_init");

    ESP_LOGI(TAG, "CFG UART%d @ %d | TX=%d RX=%d | M0=%d M1=%d AUX=%d",
             LORA_UART_NUM, LORA_CFG_BAUDRATE,
             LORA_UART_TX_PIN, LORA_UART_RX_PIN,
             LORA_M0_PIN, LORA_M1_PIN, LORA_AUX_PIN);

    /* Step 2: write config to E32 */
    check_e32(e32_write_config(&radio, E32_CFG_SAVE_TO_FLASH, &cfg), "e32_write_config");

    /* Step 3: switch local UART to run baud */
    uart_lora_set_baud(LORA_RUN_BAUDRATE);

    /* Step 4: ensure E32 back to normal mode */
    check_e32(e32_enter_normal(&radio), "e32_enter_normal");

    uart_flush(LORA_UART_NUM);
    ESP_LOGI(TAG, "RUN UART%d @ %d", LORA_UART_NUM, LORA_RUN_BAUDRATE);
    ESP_LOGI(TAG, "E32 ready, waiting frames...");

    while (1) {
        int rd = e32_read_raw(&radio, rx_chunk, sizeof(rx_chunk), 10);
        uint32_t now_ms = port_tick_ms(NULL);

        if (rd > 0) {
            if ((burst_pos > 0) && ((now_ms - last_rx_ms) > BURST_GAP_MS)) {
                process_burst(burst_buf, burst_pos);
                burst_pos = 0;
            }

            last_rx_ms = now_ms;

            for (int i = 0; i < rd; i++) {
                if (burst_pos < sizeof(burst_buf)) {
                    burst_buf[burst_pos++] = rx_chunk[i];
                } else {
                    ESP_LOGW(TAG, "overflow burst, drop");
                    dump_hex("[OVERFLOW]", burst_buf, burst_pos);
                    burst_pos = 0;
                    break;
                }
            }
        } else {
            if ((burst_pos > 0) && ((now_ms - last_rx_ms) > BURST_GAP_MS)) {
                process_burst(burst_buf, burst_pos);
                burst_pos = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void app_main(void)
{
    uart_lora_gpio_init();

#if APP_MODE == APP_MODE_UART_BASIC
    uart_lora_driver_init(LORA_RUN_BAUDRATE);
    ESP_LOGI(TAG, "UART2 BASIC TEST | TX=%d RX=%d", LORA_UART_TX_PIN, LORA_UART_RX_PIN);
    uart2_check_basic();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

#elif APP_MODE == APP_MODE_UART_LOOPBACK
    uart_lora_driver_init(LORA_RUN_BAUDRATE);
    ESP_LOGI(TAG, "UART2 LOOPBACK TEST | TX=%d RX=%d", LORA_UART_TX_PIN, LORA_UART_RX_PIN);
    uart2_check_loopback();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

#elif APP_MODE == APP_MODE_UART_RX_WATCH
    uart_lora_driver_init(LORA_RUN_BAUDRATE);
    ESP_LOGI(TAG, "UART2 RX WATCH | TX=%d RX=%d", LORA_UART_TX_PIN, LORA_UART_RX_PIN);
    while (1) {
        uart2_check_rx_activity(3000);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

#else
    run_normal_rx();
#endif
}

