#include "Lora.h"

#include <string.h>

#define E32_DEFAULT_AUX_TIMEOUT_MS   (2000U)
#define E32_FIXED_HEADER_LEN         (3U)

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

static int e32_read_bytes(e32_t *dev, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    int rd;

    if (!e32_is_valid(dev) || (buf == NULL) || (len == 0U)) {
        return E32_ERR_ARG;
    }

    rd = dev->io.uart_read(dev->io.user, buf, len, timeout_ms);
    if (rd < 0) {
        return E32_ERR_IO;
    }

    return rd;
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
        /* Module đang bị hard-wire mode ngoài mạch */
        return E32_OK;
    }

    ret = e32_mode_to_levels(mode, &m0, &m1);
    if (ret != E32_OK) {
        return ret;
    }

    dev->io.set_m0(dev->io.user, m0);
    dev->io.set_m1(dev->io.user, m1);

    /* chờ chân mode settle */
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
    return e32_read_bytes(dev, buf, len, timeout_ms);
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

    packet[0] = (uint8_t)cmd;
    packet[1] = cfg->addh;
    packet[2] = cfg->addl;
    packet[3] = cfg->speed;
    packet[4] = cfg->chan;
    packet[5] = cfg->option;

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

