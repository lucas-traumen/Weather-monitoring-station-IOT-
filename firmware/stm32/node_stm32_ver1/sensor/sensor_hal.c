/**
 * @file    sensor_hal.c
 * @brief   Triển khai HAL — SHT30 (I2C1) + BMP388 (I2C1) + UART DMA
 */

#include "sensor_hal.h"
#include "driver_sht30.h"
#include "../protocol/ringbuffer.h"
#include "../protocol/min.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>


#define ENABLE_DEBUG_LOG  1
#define TX_RING_BUF_SIZE 1024U  // Bắt buộc phải là Lũy thừa của 2 (Power of 2)

static ring_buffer_t tx_ring_buffer;
static char tx_buffer_data[TX_RING_BUF_SIZE];

static volatile uint8_t  tx_dma_running = 0;
static volatile uint16_t tx_dma_send_len = 0;

/* --- HÀM KHỞI TẠO (Bác nhớ gọi hàm này 1 lần trong main.c) --- */
void Sensor_UART_DMA_Init(void)
{
    ring_buffer_init(&tx_ring_buffer, tx_buffer_data, TX_RING_BUF_SIZE);
}

/* --- HÀM GỌI DMA ĐI GIAO HÀNG --- */
static void RingBuf_StartTx(void)
{
    __disable_irq(); // Tạm tắt ngắt để an toàn

    if (!tx_dma_running && !ring_buffer_is_empty(&tx_ring_buffer))
    {
        uint16_t head = tx_ring_buffer.head_index;
        uint16_t tail = tx_ring_buffer.tail_index;

        /* Tìm độ dài liên tục lớn nhất mà DMA có thể đọc được trong mảng vật lý */
        if (head > tail) {
            tx_dma_send_len = head - tail; // Dữ liệu nằm thẳng một mạch
        } else {
            tx_dma_send_len = TX_RING_BUF_SIZE - tail; // Dữ liệu bị vòng, chỉ đọc tới cuối mảng
        }

        tx_dma_running = 1;
        HAL_UART_Transmit_DMA(&huart1, (uint8_t*)&tx_ring_buffer.buffer[tail], tx_dma_send_len);
    }

    __enable_irq();
}

/* --- HÀM NGẮT KHI DMA GIAO XONG --- */
void Sensor_UART_TxCpltCallback(void)
{
    /* Dịch chuyển điểm Tail (điểm lấy dữ liệu) đi một đoạn bằng số byte DMA vừa đọc */
    tx_ring_buffer.tail_index = (tx_ring_buffer.tail_index + tx_dma_send_len) & RING_BUFFER_MASK((&tx_ring_buffer));
    tx_dma_running = 0;

    /* Giao tiếp phần còn lại (nếu có) */
    RingBuf_StartTx();
}

/* ══════════════════════════════════════════════════════════════════════════
 * GIAO TIẾP VỚI THƯ VIỆN MIN PROTOCOL (min.c sẽ gọi các hàm này)
 * ══════════════════════════════════════════════════════════════════════════ */
uint16_t min_tx_space(uint8_t port)
{
    // Tính toán số byte còn trống
    return TX_RING_BUF_SIZE - 1 - ring_buffer_num_items(&tx_ring_buffer);
}

void min_tx_byte(uint8_t port, uint8_t byte)
{
    ring_buffer_queue(&tx_ring_buffer, (char)byte);
}

void min_tx_start(uint8_t port)
{
    __disable_irq(); // Khóa ngắt khi MIN bắt đầu đóng gói Frame
}

void min_tx_finished(uint8_t port)
{
    __enable_irq();  // Mở ngắt
    RingBuf_StartTx(); // Kích hoạt DMA
}

uint32_t min_time_ms(void)
{
    return HAL_GetTick(); // Cung cấp thời gian cho MIN
}

/* ══════════════════════════════════════════════════════════════════════════
 * HÀM IN LOG DEBUG CỦA BÁC
 * ══════════════════════════════════════════════════════════════════════════ */
void sensor_debug_print(const char *const fmt, ...)
{
#if ENABLE_DEBUG_LOG
    char buf[256];
    va_list args;

    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0)
    {
        __disable_irq();
        ring_buffer_queue_arr(&tx_ring_buffer, buf, len);
        __enable_irq();
        RingBuf_StartTx();
    }
#endif
}

void sensor_delay_ms(uint32_t ms)
{
    HAL_Delay(ms);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SHT30 HAL — I2C1
 *  SHT30 dùng lệnh 16-bit: byte MSB gửi trước, rồi LSB.
 *  Không có "register address" theo nghĩa chuẩn; lệnh chính là reg 16-bit.
 * ══════════════════════════════════════════════════════════════════════════ */

uint8_t sht30_iic_init(void)
{
    /* hi2c1 đã được MX_I2C1_Init() khởi tạo — không cần làm gì thêm */
    return 0;
}

uint8_t sht30_iic_deinit(void)
{
    return 0;
}

uint8_t sht30_iic_write_address16(uint8_t addr, uint16_t reg,
                                  uint8_t *buf, uint16_t len)
{
    /* Gói lệnh 16-bit + data vào 1 frame để gửi 1 lần (tối đa 32 byte data) */
    if (len > 30U) return 1;

    uint8_t tx[2U + 30U];
    tx[0] = (uint8_t)(reg >> 8);
    tx[1] = (uint8_t)(reg & 0xFFU);
    if (buf && len) memcpy(&tx[2], buf, len);

    HAL_StatusTypeDef s = HAL_I2C_Master_Transmit(
            &hi2c1, addr, tx, (uint16_t)(2U + len),
            SENSOR_I2C_TIMEOUT_MS);
    return (s == HAL_OK) ? 0U : 1U;
}

uint8_t sht30_iic_read_address16(uint8_t addr, uint16_t reg,
                                 uint8_t *buf, uint16_t len)
{
    uint8_t cmd[2];
    cmd[0] = (uint8_t)(reg >> 8);
    cmd[1] = (uint8_t)(reg & 0xFFU);

    /* Gửi lệnh */
    HAL_StatusTypeDef s = HAL_I2C_Master_Transmit(
            &hi2c1, addr, cmd, 2U, SENSOR_I2C_TIMEOUT_MS);
    if (s != HAL_OK) return 1U;

    /* Đọc dữ liệu */
    s = HAL_I2C_Master_Receive(
            &hi2c1, addr, buf, len, SENSOR_I2C_TIMEOUT_MS);
    return (s == HAL_OK) ? 0U : 1U;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  BMP388 HAL — I2C1 (cùng bus với SHT30)
 *
 *  BMP388 I2C mode: CSB pin nối VCC (HIGH) để chọn I2C.
 *  SDO pin quyết định địa chỉ:  SDO=GND → 0x76,  SDO=VCC → 0x77.
 *  Driver truyền addr đã dịch trái 1 bit (0x76<<1 = 0xEC).
 * ══════════════════════════════════════════════════════════════════════════ */

uint8_t bmp388_iic_init(void)
{
    /* hi2c1 dùng chung với SHT30, đã được MX_I2C1_Init() khởi tạo */
    return 0;
}

uint8_t bmp388_iic_deinit(void)
{
    return 0;
}

uint8_t bmp388_iic_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef s = HAL_I2C_Mem_Read(
            &hi2c1, addr, reg,
            I2C_MEMADD_SIZE_8BIT,
            buf, len,
            SENSOR_I2C_TIMEOUT_MS);
    return (s == HAL_OK) ? 0U : 1U;
}

uint8_t bmp388_iic_write(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len)
{
    HAL_StatusTypeDef s = HAL_I2C_Mem_Write(
            &hi2c1, addr, reg,
            I2C_MEMADD_SIZE_8BIT,
            buf, len,
            SENSOR_I2C_TIMEOUT_MS);
    return (s == HAL_OK) ? 0U : 1U;
}

/* ─── Callbacks ──────────────────────────────────────────────────────────── */

void sht30_receive_callback(uint16_t type)
{
    if (type & SHT30_STATUS_TEMPERATURE_ALERT)
        sensor_debug_print("[SHT30] CANH BAO: Nhiet do vuot nguong!\r\n");
    if (type & SHT30_STATUS_HUMIDITY_ALERT)
        sensor_debug_print("[SHT30] CANH BAO: Do am vuot nguong!\r\n");
}

void bmp388_receive_callback(uint8_t type)
{
    (void)type;
}

uint8_t bmp388_spi_init_dummy(void) { return 0; }
uint8_t bmp388_spi_deinit_dummy(void) { return 0; }
uint8_t bmp388_spi_read_dummy(uint8_t reg, uint8_t *buf, uint16_t len) { return 1; }
uint8_t bmp388_spi_write_dummy(uint8_t reg, uint8_t *buf, uint16_t len) { return 1; }
