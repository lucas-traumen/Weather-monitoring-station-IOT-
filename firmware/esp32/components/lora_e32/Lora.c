#include "Lora.h"

#include <string.h>

#define E32_DEFAULT_AUX_TIMEOUT_MS   (2000U)
#define E32_FIXED_HEADER_LEN         (3U)
#define E32_CFG_RESP_LEN             (6U)
#define E32_CFG_READ_CMD             (0xC1U)

static int e32_has_required_io(const e32_io_t *io)
{
    return (io != NULL) &&
           (io->uart_write != NULL) &&
           (io->uart_read  != NULL) &&
           (io->delay_ms   != NULL) &&
           (io->tick_ms    != NULL);
}

static int e32_is_valid(const e32_t *dev)
{
    return (dev != NULL) &&
           (dev->is_init == 1U) &&
           (dev->io.uart_write != NULL) &&
           (dev->io.uart_read  != NULL) &&
           (dev->io.delay_ms   != NULL) &&
           (dev->io.tick_ms    != NULL);
}

static int e32_mode_to_levels(e32_mode_t mode, int *m0, int *m1)
{
    if ((m0 == NULL) || (m1 == NULL)) {
        return E32_ERR_ARG;
    }

    switch (mode) {
        case E32_MODE_NORMAL:
            *m0 = 0;
            *m1 = 0;
            return E32_OK;

        case E32_MODE_WAKEUP:
            *m0 = 1;
            *m1 = 0;
            return E32_OK;

        case E32_MODE_PWRSAVE:
            *m0 = 0;
            *m1 = 1;
            return E32_OK;

        case E32_MODE_SLEEP:
            *m0 = 1;
            *m1 = 1;
            return E32_OK;

        default:
            return E32_ERR_ARG;
    }
}

static int e32_write_bytes(e32_t *dev, const uint8_t *buf, size_t len)
{
    int wr;

    if (!e32_is_valid(dev) || (buf == NULL) || (len == 0U)) {
        return E32_ERR_ARG;
    }

    wr = dev->io.uart_write(dev->io.user, buf, len);
    if (wr < 0) {
        return E32_ERR_IO;
    }
    if ((size_t)wr != len) {
        return E32_ERR_IO;
    }

    return E32_OK;
}

static int e32_read_exact(e32_t *dev, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t off = 0U;
    uint32_t t0;
    uint32_t tout;

    if (!e32_is_valid(dev) || (buf == NULL) || (len == 0U)) {
        return E32_ERR_ARG;
    }

    tout = (timeout_ms == 0U) ? dev->aux_timeout_ms : timeout_ms;
    t0 = dev->io.tick_ms(dev->io.user);

    while (off < len) {
        uint32_t elapsed = dev->io.tick_ms(dev->io.user) - t0;
        uint32_t remain;
        int rd;

        if (elapsed >= tout) {
            return E32_ERR_TIMEOUT;
        }

        remain = tout - elapsed;
        rd = dev->io.uart_read(dev->io.user, &buf[off], len - off, remain);

        if (rd < 0) {
            return E32_ERR_IO;
        }

        if (rd == 0) {
            dev->io.delay_ms(dev->io.user, 1U);
            continue;
        }

        off += (size_t)rd;
    }

    return E32_OK;
}

static void e32_build_config_packet(uint8_t packet[6], e32_cfg_cmd_t cmd, const e32_config_t *cfg)
{
    packet[0] = (uint8_t)cmd;
    packet[1] = cfg->addh;
    packet[2] = cfg->addl;
    packet[3] = cfg->speed;
    packet[4] = cfg->chan;
    packet[5] = cfg->option;
}

static int e32_config_matches(const uint8_t cfg_bytes[6], const e32_config_t *cfg)
{
    if ((cfg_bytes == NULL) || (cfg == NULL)) {
        return 0;
    }

    /* Byte 0 thường là C0/C1 tùy command/response, bỏ qua byte này */
    return (cfg_bytes[1] == cfg->addh) &&
           (cfg_bytes[2] == cfg->addl) &&
           (cfg_bytes[3] == cfg->speed) &&
           (cfg_bytes[4] == cfg->chan) &&
           (cfg_bytes[5] == cfg->option);
}

int e32_init(e32_t *dev, const e32_io_t *io, uint32_t aux_timeout_ms)
{
    if ((dev == NULL) || !e32_has_required_io(io)) {
        return E32_ERR_ARG;
    }

    memset(dev, 0, sizeof(*dev));
    memcpy(&dev->io, io, sizeof(*io));

    dev->aux_timeout_ms = (aux_timeout_ms == 0U) ? E32_DEFAULT_AUX_TIMEOUT_MS : aux_timeout_ms;
    dev->has_mode_pins  = (uint8_t)((io->set_m0 != NULL) && (io->set_m1 != NULL));
    dev->has_aux        = (uint8_t)(io->get_aux != NULL);
    dev->is_init        = 1U;

    return E32_OK;
}

int e32_wait_aux_high(e32_t *dev, uint32_t timeout_ms)
{
    uint32_t t0;
    uint32_t tout;

    if (!e32_is_valid(dev)) {
        return E32_ERR_STATE;
    }

    if (dev->has_aux == 0U) {
        return E32_OK;
    }

    tout = (timeout_ms == 0U) ? dev->aux_timeout_ms : timeout_ms;
    t0 = dev->io.tick_ms(dev->io.user);

    while (dev->io.get_aux(dev->io.user) == 0) {
        if ((dev->io.tick_ms(dev->io.user) - t0) >= tout) {
            return E32_ERR_TIMEOUT;
        }
        dev->io.delay_ms(dev->io.user, 1U);
    }

    return E32_OK;
}

int e32_set_mode(e32_t *dev, e32_mode_t mode)
{
    int m0;
    int m1;
    int ret;

    if (!e32_is_valid(dev)) {
        return E32_ERR_STATE;
    }

    if (dev->has_mode_pins == 0U) {
        return E32_OK;
    }

    ret = e32_mode_to_levels(mode, &m0, &m1);
    if (ret != E32_OK) {
        return ret;
    }

    dev->io.set_m0(dev->io.user, m0);
    dev->io.set_m1(dev->io.user, m1);

    dev->io.delay_ms(dev->io.user, 2U);

    return e32_wait_aux_high(dev, dev->aux_timeout_ms);
}

int e32_enter_normal(e32_t *dev)
{
    return e32_set_mode(dev, E32_MODE_NORMAL);
}

int e32_enter_sleep(e32_t *dev)
{
    return e32_set_mode(dev, E32_MODE_SLEEP);
}

int e32_write_raw(e32_t *dev, const uint8_t *buf, size_t len)
{
    int ret;

    if (!e32_is_valid(dev) || (buf == NULL) || (len == 0U)) {
        return E32_ERR_ARG;
    }

    ret = e32_wait_aux_high(dev, dev->aux_timeout_ms);
    if (ret != E32_OK) {
        return ret;
    }

    ret = e32_write_bytes(dev, buf, len);
    if (ret != E32_OK) {
        return ret;
    }

    return e32_wait_aux_high(dev, dev->aux_timeout_ms);
}

int e32_read_raw(e32_t *dev, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    return e32_read_exact(dev, buf, len, timeout_ms);
}

int e32_write_fixed(e32_t *dev,
                    uint8_t addh,
                    uint8_t addl,
                    uint8_t chan,
                    const uint8_t *buf,
                    size_t len)
{
    uint8_t hdr[E32_FIXED_HEADER_LEN];
    int ret;

    if (!e32_is_valid(dev) || (buf == NULL) || (len == 0U)) {
        return E32_ERR_ARG;
    }

    hdr[0] = addh;
    hdr[1] = addl;
    hdr[2] = chan;

    ret = e32_wait_aux_high(dev, dev->aux_timeout_ms);
    if (ret != E32_OK) {
        return ret;
    }

    ret = e32_write_bytes(dev, hdr, sizeof(hdr));
    if (ret != E32_OK) {
        return ret;
    }

    ret = e32_write_bytes(dev, buf, len);
    if (ret != E32_OK) {
        return ret;
    }

    return e32_wait_aux_high(dev, dev->aux_timeout_ms);
}

int e32_read_config(e32_t *dev, uint8_t out_cfg[6])
{
    uint8_t cmd[3] = { E32_CFG_READ_CMD, E32_CFG_READ_CMD, E32_CFG_READ_CMD };
    int ret;

    if (!e32_is_valid(dev) || (out_cfg == NULL)) {
        return E32_ERR_ARG;
    }

    ret = e32_enter_sleep(dev);
    if (ret != E32_OK) {
        return ret;
    }

    ret = e32_wait_aux_high(dev, dev->aux_timeout_ms);
    if (ret != E32_OK) {
        return ret;
    }

    ret = e32_write_bytes(dev, cmd, sizeof(cmd));
    if (ret != E32_OK) {
        return ret;
    }

    ret = e32_read_exact(dev, out_cfg, E32_CFG_RESP_LEN, dev->aux_timeout_ms);
    if (ret != E32_OK) {
        return ret;
    }

    return e32_enter_normal(dev);
}

int e32_write_config(e32_t *dev, e32_cfg_cmd_t cmd, const e32_config_t *cfg)
{
    uint8_t packet[6];
    int ret;

    if (!e32_is_valid(dev) || (cfg == NULL)) {
        return E32_ERR_ARG;
    }

    if ((cmd != E32_CFG_SAVE_TO_FLASH) && (cmd != E32_CFG_SAVE_TO_RAM)) {
        return E32_ERR_ARG;
    }

    ret = e32_enter_sleep(dev);
    if (ret != E32_OK) {
        return ret;
    }

    e32_build_config_packet(packet, cmd, cfg);

    ret = e32_write_bytes(dev, packet, sizeof(packet));
    if (ret != E32_OK) {
        return ret;
    }

    ret = e32_wait_aux_high(dev, dev->aux_timeout_ms);
    if (ret != E32_OK) {
        return ret;
    }

    return e32_enter_normal(dev);
}

int e32_write_config_verified(e32_t *dev, e32_cfg_cmd_t cmd, const e32_config_t *cfg, uint8_t out_cfg[6])
{
    uint8_t verify_buf[6];
    uint8_t *target = (out_cfg != NULL) ? out_cfg : verify_buf;
    int ret;

    if (!e32_is_valid(dev) || (cfg == NULL)) {
        return E32_ERR_ARG;
    }

    ret = e32_write_config(dev, cmd, cfg);
    if (ret != E32_OK) {
        return ret;
    }

    /* Caller vẫn đang giữ UART local ở 9600, nên đọc lại config ngay */
    ret = e32_read_config(dev, target);
    if (ret != E32_OK) {
        return ret;
    }

    if (!e32_config_matches(target, cfg)) {
        return E32_ERR_IO;
    }

    return E32_OK;
}
