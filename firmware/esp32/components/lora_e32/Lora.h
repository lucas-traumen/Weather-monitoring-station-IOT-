#ifndef INC_LORA_H_
#define INC_LORA_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================
 * PUBLIC ERROR CODES
 * ========================= */
#define E32_OK             (0)
#define E32_ERR_ARG        (-1)
#define E32_ERR_IO         (-2)
#define E32_ERR_TIMEOUT    (-3)
#define E32_ERR_STATE      (-4)

/* =========================
 * E32 MODE
 * M1 M0
 * 0  0 : Normal
 * 0  1 : Wake-up
 * 1  0 : Power-saving
 * 1  1 : Sleep / Config
 * ========================= */
typedef enum {
    E32_MODE_NORMAL  = 0,
    E32_MODE_WAKEUP  = 1,
    E32_MODE_PWRSAVE = 2,
    E32_MODE_SLEEP   = 3
} e32_mode_t;

/* =========================
 * SPEED BYTE (SPED)
 * speed = uart_mode | uart_baud | air_rate
 * ========================= */
typedef enum {
    E32_UART_8N1 = (0x00 << 6),
    E32_UART_8O1 = (0x01 << 6),
    E32_UART_8E1 = (0x02 << 6),
} e32_uart_mode_t;

typedef enum {
    E32_BAUD_1200   = (0x00 << 3),
    E32_BAUD_2400   = (0x01 << 3),
    E32_BAUD_4800   = (0x02 << 3),
    E32_BAUD_9600   = (0x03 << 3),
    E32_BAUD_19200  = (0x04 << 3),
    E32_BAUD_38400  = (0x05 << 3),
    E32_BAUD_57600  = (0x06 << 3),
    E32_BAUD_115200 = (0x07 << 3),
} e32_uart_baud_t;

typedef enum {
    E32_AIR_03K2  = (0x00 << 0),
    E32_AIR_12K   = (0x01 << 0),
    E32_AIR_24K   = (0x02 << 0),
    E32_AIR_48K   = (0x03 << 0),
    E32_AIR_96K   = (0x04 << 0),
    E32_AIR_192K  = (0x05 << 0),
} e32_air_rate_t;

/* =========================
 * OPTION BYTE
 * option = trans_mode | io_mode | wakeup | fec | tx_power
 * ========================= */
typedef enum {
    E32_TRANS_TRANSPARENT = (0x00 << 7),
    E32_TRANS_FIXED       = (0x01 << 7),
} e32_trans_mode_t;

typedef enum {
    E32_IO_OPEN_DRAIN = (0x00 << 6),
    E32_IO_PUSH_PULL  = (0x01 << 6),
} e32_io_mode_t;

typedef enum {
    E32_WAKE_250MS  = (0x00 << 3),
    E32_WAKE_500MS  = (0x01 << 3),
    E32_WAKE_750MS  = (0x02 << 3),
    E32_WAKE_1000MS = (0x03 << 3),
    E32_WAKE_1250MS = (0x04 << 3),
    E32_WAKE_1500MS = (0x05 << 3),
    E32_WAKE_1750MS = (0x06 << 3),
    E32_WAKE_2000MS = (0x07 << 3),
} e32_wakeup_t;

typedef enum {
    E32_FEC_OFF = (0x00 << 2),
    E32_FEC_ON  = (0x01 << 2),
} e32_fec_t;

typedef enum {
    E32_PWR_30DBM = (0x00 << 0),
    E32_PWR_27DBM = (0x01 << 0),
    E32_PWR_24DBM = (0x02 << 0),
    E32_PWR_21DBM = (0x03 << 0),
} e32_tx_power_t;

/* =========================
 * CONFIG COMMAND
 * ========================= */
typedef enum {
    E32_CFG_SAVE_TO_FLASH = 0xC0, /* save permanently */
    E32_CFG_SAVE_TO_RAM   = 0xC2  /* temporary until power-cycle */
} e32_cfg_cmd_t;

/* =========================
 * MODULE CONFIG
 * speed  = OR-combine of uart mode + baud + air rate
 * option = OR-combine of trans mode + io mode + wakeup + fec + tx power
 * ========================= */
typedef struct {
    uint8_t addh;
    uint8_t addl;
    uint8_t speed;
    uint8_t chan;
    uint8_t option;
} e32_config_t;

/* =========================
 * PLATFORM IO CALLBACKS
 * user is opaque pointer to platform context
 * ========================= */
typedef struct {
    void *user;

    int      (*uart_write)(void *user, const uint8_t *data, size_t len);
    int      (*uart_read)(void *user, uint8_t *data, size_t len, uint32_t timeout_ms);

    void     (*set_m0)(void *user, int level);   /* nullable if hard-wired */
    void     (*set_m1)(void *user, int level);   /* nullable if hard-wired */
    int      (*get_aux)(void *user);             /* nullable if AUX unused */

    void     (*delay_ms)(void *user, uint32_t ms);
    uint32_t (*tick_ms)(void *user);
} e32_io_t;

/* =========================
 * DEVICE HANDLE
 * ========================= */
typedef struct {
    e32_io_t io;
    uint32_t aux_timeout_ms;
    uint8_t  has_mode_pins;
    uint8_t  has_aux;
    uint8_t  is_init;
} e32_t;

/* =========================
 * CORE API
 * ========================= */
int e32_init(e32_t *dev, const e32_io_t *io, uint32_t aux_timeout_ms);

int e32_wait_aux_high(e32_t *dev, uint32_t timeout_ms);
int e32_set_mode(e32_t *dev, e32_mode_t mode);

int e32_enter_normal(e32_t *dev);
int e32_enter_sleep(e32_t *dev);

int e32_write_raw(e32_t *dev, const uint8_t *buf, size_t len);
int e32_read_raw(e32_t *dev, uint8_t *buf, size_t len, uint32_t timeout_ms);

int e32_write_fixed(e32_t *dev,
                    uint8_t addh,
                    uint8_t addl,
                    uint8_t chan,
                    const uint8_t *buf,
                    size_t len);

int e32_write_config(e32_t *dev, e32_cfg_cmd_t cmd, const e32_config_t *cfg);
int e32_read_config(e32_t *dev, uint8_t out_cfg[6]);
int e32_write_config_verified(e32_t *dev, e32_cfg_cmd_t cmd, const e32_config_t *cfg, uint8_t out_cfg[6]);
#ifdef __cplusplus
}
#endif

#endif /* INC_LORA_H_ */
