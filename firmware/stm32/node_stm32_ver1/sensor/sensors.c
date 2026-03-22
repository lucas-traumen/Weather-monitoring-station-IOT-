/**
 * @file    sensors.c
 * @brief   Sensor Manager — SHT30 (I2C1) + BMP388 (SPI1)
 */

#include "sensors.h"
#include "sensor_hal.h"
#include "driver_sht30.h"
#include "driver_bmp388.h"
#include <math.h>
#include <string.h>

/* ─── Handle nội bộ ──────────────────────────────────────────────────────── */
static sht30_handle_t  s_sht30;
static bmp388_handle_t s_bmp388;
static uint8_t         s_sht30_ok  = 0;
static uint8_t         s_bmp388_ok = 0;

/* ─── Hằng vật lý tính độ cao ────────────────────────────────────────────── */
#define SEA_LEVEL_PA   101325.0f

static float calc_altitude(float pa)
{
    /* Barometric formula: h = 44330 × [1 − (P/P₀)^(1/5.255)] */
    return 44330.0f * (1.0f - powf(pa / SEA_LEVEL_PA, 1.0f / 5.255f));
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Khởi tạo SHT30 — I2C, single-shot read, repeatability HIGH
 * ══════════════════════════════════════════════════════════════════════════ */
static uint8_t _init_sht30(void)
{
    DRIVER_SHT30_LINK_INIT(&s_sht30, sht30_handle_t);

    DRIVER_SHT30_LINK_IIC_INIT(&s_sht30,            sht30_iic_init);
    DRIVER_SHT30_LINK_IIC_DEINIT(&s_sht30,          sht30_iic_deinit);
    DRIVER_SHT30_LINK_IIC_READ_ADDRESS16(&s_sht30,  sht30_iic_read_address16);
    DRIVER_SHT30_LINK_IIC_WRITE_ADDRESS16(&s_sht30, sht30_iic_write_address16);
    DRIVER_SHT30_LINK_DELAY_MS(&s_sht30,            sensor_delay_ms);
    DRIVER_SHT30_LINK_DEBUG_PRINT(&s_sht30,         sensor_debug_print);
    DRIVER_SHT30_LINK_RECEIVE_CALLBACK(&s_sht30,    sht30_receive_callback);

    if (sht30_set_addr_pin(&s_sht30, SHT30_ADDRESS_0)             != 0) return 1;
    if (sht30_init(&s_sht30)                                        != 0) return 1;
    if (sht30_set_repeatability(&s_sht30, SHT30_REPEATABILITY_HIGH) != 0) return 1;


    sensor_debug_print("[SHT30]  Khoi tao OK (I2C1, addr=0x44)\r\n");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Khởi tạo BMP388 — I2C mode (CSB=VCC, SDO=GND → addr 0x76)
 * ══════════════════════════════════════════════════════════════════════════ */
static uint8_t _init_bmp388(void)
{
    DRIVER_BMP388_LINK_INIT(&s_bmp388, bmp388_handle_t);

    /* Link IIC (không link SPI) */
    DRIVER_BMP388_LINK_IIC_INIT(&s_bmp388,         bmp388_iic_init);
    DRIVER_BMP388_LINK_IIC_DEINIT(&s_bmp388,       bmp388_iic_deinit);
    DRIVER_BMP388_LINK_IIC_READ(&s_bmp388,         bmp388_iic_read);
    DRIVER_BMP388_LINK_IIC_WRITE(&s_bmp388,        bmp388_iic_write);
    DRIVER_BMP388_LINK_DELAY_MS(&s_bmp388,         sensor_delay_ms);
    DRIVER_BMP388_LINK_DEBUG_PRINT(&s_bmp388,      sensor_debug_print);
    DRIVER_BMP388_LINK_RECEIVE_CALLBACK(&s_bmp388, bmp388_receive_callback);
    DRIVER_BMP388_LINK_SPI_INIT(&s_bmp388,         bmp388_spi_init_dummy);
    DRIVER_BMP388_LINK_SPI_DEINIT(&s_bmp388,       bmp388_spi_deinit_dummy);
    DRIVER_BMP388_LINK_SPI_READ(&s_bmp388,         bmp388_spi_read_dummy);
    DRIVER_BMP388_LINK_SPI_WRITE(&s_bmp388,        bmp388_spi_write_dummy);

    /* Chọn giao tiếp I2C và địa chỉ (SDO=GND → 0x76) */
    if (bmp388_set_interface(&s_bmp388, BMP388_INTERFACE_IIC)       != 0) return 1;
    if (bmp388_set_addr_pin(&s_bmp388, BMP388_ADDRESS_ADO_HIGH)      != 0) return 1;

    /* Khởi tạo chip (soft reset nội bộ + đọc bộ hiệu chuẩn) */
    if (bmp388_init(&s_bmp388) != 0) return 1;

    /* Oversampling:
     *   Áp suất  ×8  — cân bằng noise/tốc độ, phù hợp đo độ cao
     *   Nhiệt độ ×1  — đủ độ chính xác cho nhiệt độ bo mạch */
    if (bmp388_set_pressure_oversampling(&s_bmp388,    BMP388_OVERSAMPLING_x8) != 0) return 1;
    if (bmp388_set_temperature_oversampling(&s_bmp388, BMP388_OVERSAMPLING_x1) != 0) return 1;

    /* IIR filter hệ số 3 — giảm noise từ rung động/va chạm */
    if (bmp388_set_filter_coefficient(&s_bmp388, BMP388_FILTER_COEFFICIENT_3) != 0) return 1;

    /* ODR 25 Hz */
    if (bmp388_set_odr(&s_bmp388, BMP388_ODR_25_HZ) != 0) return 1;

    /* Bật cả pressure sensor và temperature sensor */
    if (bmp388_set_pressure(&s_bmp388,    BMP388_BOOL_TRUE) != 0) return 1;
    if (bmp388_set_temperature(&s_bmp388, BMP388_BOOL_TRUE) != 0) return 1;

    /* Normal mode — đo liên tục */
    if (bmp388_set_mode(&s_bmp388, BMP388_MODE_NORMAL_MODE) != 0) return 1;

    sensor_debug_print("[BMP388] Khoi tao OK (I2C1, addr=0x76)\r\n");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  API công khai
 * ══════════════════════════════════════════════════════════════════════════ */

uint8_t Sensors_Init(void)
{
    uint8_t ret = 0;

    s_sht30_ok  = (_init_sht30()  == 0) ? 1 : 0;
    s_bmp388_ok = (_init_bmp388() == 0) ? 1 : 0;

    if (!s_sht30_ok)  { sensor_debug_print("[SENSORS] LOI: SHT30 that bai!\r\n");  ret |= 0x01; }
    if (!s_bmp388_ok) { sensor_debug_print("[SENSORS] LOI: BMP388 that bai!\r\n"); ret |= 0x02; }

    return ret;
}

uint8_t Sensors_ReadSHT30(SHT30_Data_t *out)
{
    if (!out) return 1;
    memset(out, 0, sizeof(*out));
    if (!s_sht30_ok) return 1;

    uint16_t tr, hr;
    float    tc, hpct;

    /* Clock stretching TẮT — STM32F103 I2C không hỗ trợ ổn định */
    if (sht30_single_read(&s_sht30, SHT30_BOOL_FALSE,
                          &tr, &tc, &hr, &hpct) != 0)
    {
        sensor_debug_print("[SHT30] LOI: doc du lieu that bai!\r\n");
        return 1;
    }

    out->temperature_c = tc;
    out->humidity_pct  = hpct;
    out->valid         = 1;
    return 0;
}

uint8_t Sensors_ReadBMP388(BMP388_Data_t *out)
{
    if (!out) return 1;
    memset(out, 0, sizeof(*out));
    if (!s_bmp388_ok) return 1;

    uint32_t tr, pr;
    float    tc, pa;

    if (bmp388_read_temperature_pressure(&s_bmp388, &tr, &tc, &pr, &pa) != 0)
    {
        sensor_debug_print("[BMP388] LOI: doc du lieu that bai!\r\n");
        return 1;
    }

    out->temperature_c = tc;
    out->pressure_pa   = pa;
    out->pressure_hpa  = pa / 100.0f;
    out->altitude_m    = calc_altitude(pa);
    out->valid         = 1;
    return 0;
}

void Sensors_ReadAll(SensorData_t *data)
{
    if (!data) return;
    Sensors_ReadSHT30(&data->env);
    Sensors_ReadBMP388(&data->board);
}

void Sensors_Print(const SensorData_t *data)
{
    if (!data) return;

    sensor_debug_print("========================================\r\n");

    sensor_debug_print("[ SHT30  | Moi truong ngoai ]\r\n");
    if (data->env.valid)
    {
        sensor_debug_print("  Nhiet do ngoai : %6.2f degC\r\n", data->env.temperature_c);
        sensor_debug_print("  Do am tuong doi: %6.2f %%RH\r\n",  data->env.humidity_pct);
    }
    else
        sensor_debug_print("  [!] Loi doc du lieu\r\n");

    sensor_debug_print("[ BMP388 | Bo mach         ]\r\n");
    if (data->board.valid)
    {
        sensor_debug_print("  Nhiet do bo mach: %6.2f degC\r\n", data->board.temperature_c);
        sensor_debug_print("  Ap suat         : %7.2f hPa\r\n",   data->board.pressure_hpa);
        sensor_debug_print("  Do cao uoc tinh : %7.1f m\r\n",     data->board.altitude_m);
    }
    else
        sensor_debug_print("  [!] Loi doc du lieu\r\n");

    sensor_debug_print("========================================\r\n\r\n");
}

void Sensors_Deinit(void)
{
    if (s_sht30_ok)  { sht30_deinit(&s_sht30);   s_sht30_ok  = 0; }
    if (s_bmp388_ok) { bmp388_deinit(&s_bmp388);  s_bmp388_ok = 0; }
}
