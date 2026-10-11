/* First: on ESP-IDF it brings in sdkconfig.h, which the #if below reads. */
#include "expander_priv.h"

#if CONFIG_EXPANDER_MCP23

#include <stdlib.h>
#include <string.h>

#include "expander_log.h"
#include "mcp23.h"
#include "mcp23_regs.h"

#define TIMEOUT_MS  100

#define TRY(expr) do { int _e = (expr); if (_e) return _e; } while (0)

struct mcp23_t {
    struct expander_dev   expander;  /* the category view; priv points back */
    struct expander_bus   bus;
    bool                  spi;       /* MCP23S17: no output-only pins */
    uint8_t               spi_addr;  /* A2..A0 in the SPI control byte */
    /* Shadows of the registers only this driver ever writes, so setting one
     * pin costs one transaction instead of a read then a write. */
    uint16_t iodir;
    uint16_t gppu;
    uint16_t olat;
    uint16_t gpinten;
};

/* ------------------------------------------------------------- bus ---- */

/*
 * One register access, on either part. I2C: the register address, then the
 * data (DS 3.2.2, Figure 3-6). SPI: the control byte, then the register
 * address, then the data, under one chip select (DS 3.2.3, Figure 3-7).
 */
static int reg_write(mcp23_handle_t d, uint8_t hw_addr, uint8_t reg,
                     const uint8_t *data, size_t len)
{
    uint8_t tx[3];

    if (!d->spi)
        return d->bus.ops->transfer(&d->bus, reg, data, len, NULL, 0);
    tx[0] = reg;
    memcpy(tx + 1, data, len);
    return d->bus.ops->transfer(&d->bus, mcp23_spi_opcode(hw_addr, false),
                                tx, 1 + len, NULL, 0);
}

static int reg_read(mcp23_handle_t d, uint8_t reg, uint8_t *data, size_t len)
{
    if (!d->spi)
        return d->bus.ops->transfer(&d->bus, reg, NULL, 0, data, len);
    return d->bus.ops->transfer(&d->bus, mcp23_spi_opcode(d->spi_addr, true),
                                &reg, 1, data, len);
}

static int reg_read16(mcp23_handle_t d, uint8_t reg, uint16_t *out)
{
    uint8_t buf[2];

    TRY(reg_read(d, reg, buf, sizeof buf));
    *out = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    return 0;
}

static int reg_write16(mcp23_handle_t d, uint8_t reg, uint16_t val)
{
    uint8_t buf[2] = {
        (uint8_t)(val & 0xFF),  /* PORTA */
        (uint8_t)(val >> 8),    /* PORTB */
    };

    return reg_write(d, d->spi_addr, reg, buf, sizeof buf);
}

static int reg_write8(mcp23_handle_t d, uint8_t reg, uint8_t val)
{
    return reg_write(d, d->spi_addr, reg, &val, 1);
}

/* Set or clear one bit of a shadowed register and push it to the chip. */
static int update_bit(mcp23_handle_t d, uint8_t reg, uint16_t *shadow,
                      uint8_t pin, bool set)
{
    uint16_t next = set ? (uint16_t)(*shadow | (1u << pin))
                        : (uint16_t)(*shadow & ~(1u << pin));

    if (next == *shadow)
        return 0;
    TRY(reg_write16(d, reg, next));
    *shadow = next;
    return 0;
}

/* ----------------------------------------------------------- pins ---- */

static bool output_only(mcp23_handle_t d, uint8_t pin)
{
    return !d->spi && (MCP23017_OUTPUT_ONLY & (1u << pin));
}

static int set_mode(mcp23_handle_t d, uint8_t pin, mcp23_mode_t mode)
{
    if (pin >= MCP23_PINS)
        return -EINVAL;

    switch (mode) {
    case MCP23_OUTPUT:
        TRY(update_bit(d, MCP23_GPPU, &d->gppu, pin, false));
        return update_bit(d, MCP23_IODIR, &d->iodir, pin, false);

    case MCP23_INPUT:
    case MCP23_INPUT_PULLUP:
        if (output_only(d, pin))
            return -EINVAL;
        /* Direction first: a pin leaving output mode should stop driving
         * before its pull-up is touched. */
        TRY(update_bit(d, MCP23_IODIR, &d->iodir, pin, true));
        return update_bit(d, MCP23_GPPU, &d->gppu, pin,
                          mode == MCP23_INPUT_PULLUP);

    default:
        return -EINVAL;
    }
}

static int write_port(mcp23_handle_t d, uint16_t levels)
{
    TRY(reg_write16(d, MCP23_OLAT, levels));
    d->olat = levels;
    return 0;
}

static int set_interrupt(mcp23_handle_t d, uint8_t pin, bool enable)
{
    if (pin >= MCP23_PINS || (enable && output_only(d, pin)))
        return -EINVAL;

    /* DEFVAL and INTCON stay as init left them, 0: compare against the
     * previous value, so any change fires (DS 3.5.5, p.20). */
    return update_bit(d, MCP23_GPINTEN, &d->gpinten, pin, enable);
}

static int read_interrupt(mcp23_handle_t d, uint16_t *flags,
                          uint16_t *captured)
{
    uint16_t value;

    if (flags) {
        TRY(reg_read16(d, MCP23_INTF, &value));
        *flags = value;
    }

    /* Reading INTCAP is what clears the interrupt (DS 3.6.4, p.25), so do
     * it even when the caller did not ask for the captured value. */
    TRY(reg_read16(d, MCP23_INTCAP, &value));
    if (captured)
        *captured = value;
    return 0;
}

/* ----------------------------------------------- the expander_dev ops -- */

static mcp23_handle_t of(struct expander_dev *expander)
{
    return expander->priv;
}

static int op_set_mode(struct expander_dev *expander, unsigned int pin,
                       enum expander_mode mode)
{
    static const mcp23_mode_t modes[] = {
        [EXPANDER_INPUT] = MCP23_INPUT,
        [EXPANDER_INPUT_PULLUP] = MCP23_INPUT_PULLUP,
        [EXPANDER_OUTPUT] = MCP23_OUTPUT,
    };

    return set_mode(of(expander), (uint8_t)pin, modes[mode]);
}

/* A whole port is written as such; a part of one goes through the shadow,
 * and not at all if nothing changes. */
static int op_write(struct expander_dev *expander, uint32_t mask,
                    uint32_t levels)
{
    mcp23_handle_t d = of(expander);
    uint16_t next = (uint16_t)((d->olat & ~mask) | (levels & mask));

    if (mask != 0xFFFF && next == d->olat)
        return 0;
    return write_port(d, next);
}

static int op_read(struct expander_dev *expander, uint32_t *levels)
{
    uint16_t v;

    TRY(reg_read16(of(expander), MCP23_GPIO, &v));
    *levels = v;
    return 0;
}

static int op_set_interrupt(struct expander_dev *expander, unsigned int pin,
                            bool enable)
{
    return set_interrupt(of(expander), (uint8_t)pin, enable);
}

static int op_read_interrupt(struct expander_dev *expander, uint32_t *flags,
                             uint32_t *captured)
{
    uint16_t f, c;

    TRY(read_interrupt(of(expander), &f, &c));
    *flags = f;
    *captured = c;
    return 0;
}

static const struct expander_ops mcp23_ops = {
    .set_mode = op_set_mode,
    .write = op_write,
    .read = op_read,
    .set_interrupt = op_set_interrupt,
    .read_interrupt = op_read_interrupt,
};

/* ------------------------------------------------------------- init ---- */

static void release(mcp23_handle_t d)
{
    if (d->bus.ops)
        expander_bus_detach(&d->bus);
    free(d);
}

static uint8_t iocon_of(bool mirror, bool open_drain, bool active_high)
{
    uint8_t iocon = 0;

    if (mirror)
        iocon |= MCP23_IOCON_MIRROR;
    if (open_drain)
        iocon |= MCP23_IOCON_ODR;
    if (active_high)
        iocon |= MCP23_IOCON_INTPOL;
    return iocon;
}

/*
 * Put the chip in a known state, whatever a warm MCU restart left behind,
 * and check it took. The handle's bus is attached.
 */
static int setup(mcp23_handle_t d, uint8_t iocon)
{
    uint8_t back[11];

    /* IOCON.BANK = 1 moves every register (DS Table 3-4, p.17), and the
     * byte below would land on OLATA. Address 0x05 is IOCON with BANK = 1
     * and GPINTENB with BANK = 0, which is cleared below anyway: writing 0
     * there gets BANK = 0 either way. */
    TRY(reg_write8(d, MCP23_IOCON_BANK1, 0x00));

    /* IOCON is the one register that is not a port pair - it appears at both
     * 0x0A and 0x0B and holds the same byte, so write it alone. SEQOP stays
     * 0: the pairs below rely on the pointer incrementing. */
    TRY(reg_write8(d, MCP23_IOCON, iocon));

    /* Every pin an input, no pull-ups, no interrupts, latches low -- and,
     * which esp-mcp23-expander left as they were, no inversion and
     * interrupts compared against the last value: all POR values (DS Table
     * 3-5, p.17-18). */
    d->iodir = MCP23_IODIR_POR;
    d->gppu = 0;
    d->olat = 0;
    d->gpinten = 0;
    TRY(reg_write16(d, MCP23_IODIR, d->iodir));
    TRY(reg_write16(d, MCP23_GPPU, d->gppu));
    TRY(reg_write16(d, MCP23_OLAT, d->olat));
    TRY(reg_write16(d, MCP23_GPINTEN, d->gpinten));
    TRY(reg_write16(d, MCP23_IPOL, 0));
    TRY(reg_write16(d, MCP23_DEFVAL, 0));
    TRY(reg_write16(d, MCP23_INTCON, 0));

    /* IODIR through IOCON, 0x00-0x0A, in one sequential read: what was just
     * written. On SPI nothing acknowledges, so this is the only sign a chip
     * is there at all; MISO left floating reads all ones or all zeros, and
     * IODIR and IOCON cannot both match either. */
    TRY(reg_read(d, MCP23_IODIR, back, sizeof back));
    for (size_t i = 0; i < sizeof back; i++) {
        uint8_t want = i < 2 ? 0xFF : i < 10 ? 0x00 : iocon;

        if (back[i] != want) {
            expander_err(&d->expander,
                         "register 0x%02X reads 0x%02X, wrote 0x%02X",
                         (unsigned)i, (unsigned)back[i], (unsigned)want);
            return -ENODEV;
        }
    }

    d->expander.ops = &mcp23_ops;
    d->expander.priv = d;
    d->expander.pins = MCP23_PINS;
    return 0;
}

static int check_addr(const mcp23_config_t *cfg)
{
    if (cfg->addr != 0 && (cfg->addr < MCP23_ADDR_BASE ||
                           cfg->addr > MCP23_ADDR_BASE + 7))
        return -EINVAL;
    return 0;
}

#if defined(ESP_PLATFORM) || defined(HAL_I2C_MODULE_ENABLED)
static int init_i2c(expander_i2c_bus bus, const mcp23_config_t *cfg,
                    mcp23_handle_t *out)
{
    static const mcp23_config_t defaults = { 0 };
    mcp23_handle_t d;
    uint8_t addr;
    int err;

    if (bus == NULL || out == NULL)
        return -EINVAL;
    if (cfg == NULL)
        cfg = &defaults;
    TRY(check_addr(cfg));
    addr = cfg->addr ? cfg->addr : MCP23_ADDR_BASE;

    /* Presence first, before anything is allocated: nothing ACKing is
     * -ENXIO. esp-mcp23-expander took a timed-out first read for that. */
    TRY(expander_i2c_probe(bus, addr, TIMEOUT_MS));

    d = calloc(1, sizeof *d);
    if (d == NULL)
        return -ENOMEM;
    d->expander.name = "mcp23017";

    err = expander_i2c_attach(&d->bus, bus, addr, cfg->scl_speed_hz,
                              TIMEOUT_MS);
    if (!err)
        err = setup(d, iocon_of(cfg->int_mirror, cfg->int_open_drain,
                                cfg->int_active_high));
    if (err) {
        release(d);
        return err;
    }
    *out = d;
    return 0;
}
#endif

#if defined(ESP_PLATFORM) || defined(HAL_SPI_MODULE_ENABLED)
/*
 * The MCP23S17 listens at A2..A0 only once IOCON.HAEN is set; until then it
 * answers to 000, whatever its pins say (DS 3.5.6, p.20). So IOCON, with
 * HAEN, goes first through address 0 -- which every MCP23S17 on this chip
 * select still at HAEN = 0 hears, and so does one strapped 000: hence the
 * rule that they all use the same int_* settings. After it each listens at
 * its own address, and no two answer the same read.
 */
static int setup_spi(mcp23_handle_t d, uint8_t iocon)
{
    uint8_t v = iocon | MCP23_IOCON_HAEN;

    TRY(reg_write(d, 0, MCP23_IOCON, &v, 1));
    return setup(d, v);
}

static int check_spi(const mcp23s17_config_t *cfg, mcp23_handle_t *out)
{
    if (cfg == NULL || out == NULL || cfg->addr > 7)
        return -EINVAL;
    return 0;
}
#endif

#ifdef ESP_PLATFORM
static int init_spi(const mcp23s17_config_t *cfg, mcp23_handle_t *out)
{
    mcp23_handle_t d;
    uint32_t hz;
    int err;

    TRY(check_spi(cfg, out));
    hz = cfg->spi_hz ? cfg->spi_hz : MCP23S17_SPI_MAX_HZ;
    if (hz > MCP23S17_SPI_MAX_HZ)
        return -EINVAL;

    d = calloc(1, sizeof *d);
    if (d == NULL)
        return -ENOMEM;
    d->expander.name = "mcp23s17";
    d->spi = true;
    d->spi_addr = cfg->addr;

    /* Mode 0; the part also takes 1,1, but then only after a CS toggle
     * (DS Table 1-4, Note 1, p.8). */
    err = expander_spi_attach(&d->bus, cfg->host, cfg->cs_gpio, hz, 0);
    if (!err)
        err = setup_spi(d, iocon_of(cfg->int_mirror, cfg->int_open_drain,
                                    cfg->int_active_high));
    if (err) {
        release(d);
        return err;
    }
    *out = d;
    return 0;
}
#elif defined(HAL_SPI_MODULE_ENABLED)
static int init_spi(const mcp23s17_config_t *cfg, mcp23_handle_t *out)
{
    mcp23_handle_t d;
    int err;

    TRY(check_spi(cfg, out));
    /* Mode 0 (DS Table 1-4), bytes. The handle is the app's, so refuse a
     * wrong one before the bus is touched. */
    if (!cfg->spi || !cfg->cs_port ||
        cfg->spi->Init.CLKPolarity != SPI_POLARITY_LOW ||
        cfg->spi->Init.CLKPhase != SPI_PHASE_1EDGE ||
        cfg->spi->Init.DataSize != SPI_DATASIZE_8BIT)
        return -EINVAL;

    d = calloc(1, sizeof *d);
    if (d == NULL)
        return -ENOMEM;
    d->expander.name = "mcp23s17";
    d->spi = true;
    d->spi_addr = cfg->addr;

    err = expander_spi_attach(&d->bus, cfg->spi, cfg->cs_port, cfg->cs_pin,
                              TIMEOUT_MS);
    if (!err)
        err = setup_spi(d, iocon_of(cfg->int_mirror, cfg->int_open_drain,
                                    cfg->int_active_high));
    if (err) {
        release(d);
        return err;
    }
    *out = d;
    return 0;
}
#endif

/* ---------------------------------------------------- the handle API -- */

#ifdef ESP_PLATFORM
esp_err_t mcp23_init(i2c_master_bus_handle_t bus, const mcp23_config_t *cfg,
                     mcp23_handle_t *out)
{
    return EXPANDER_HANDLE_RET(init_i2c(bus, cfg, out));
}

esp_err_t mcp23s17_init(const mcp23s17_config_t *cfg, mcp23_handle_t *out)
{
    return EXPANDER_HANDLE_RET(init_spi(cfg, out));
}
#else
#ifdef HAL_I2C_MODULE_ENABLED
int mcp23_init(I2C_HandleTypeDef *bus, const mcp23_config_t *cfg,
               mcp23_handle_t *out)
{
    return init_i2c(bus, cfg, out);
}
#endif
#ifdef HAL_SPI_MODULE_ENABLED
int mcp23s17_init(const mcp23s17_config_t *cfg, mcp23_handle_t *out)
{
    return init_spi(cfg, out);
}
#endif
#endif

expander_handle_ret mcp23_set_mode(mcp23_handle_t dev, uint8_t pin,
                                   mcp23_mode_t mode)
{
    if (dev == NULL)
        return EXPANDER_HANDLE_RET(-EINVAL);
    return EXPANDER_HANDLE_RET(set_mode(dev, pin, mode));
}

expander_handle_ret mcp23_write(mcp23_handle_t dev, uint8_t pin, bool level)
{
    if (dev == NULL || pin >= MCP23_PINS)
        return EXPANDER_HANDLE_RET(-EINVAL);
    return EXPANDER_HANDLE_RET(update_bit(dev, MCP23_OLAT, &dev->olat, pin,
                                          level));
}

expander_handle_ret mcp23_read(mcp23_handle_t dev, uint8_t pin, bool *level)
{
    uint16_t port;
    int err;

    if (dev == NULL || level == NULL || pin >= MCP23_PINS)
        return EXPANDER_HANDLE_RET(-EINVAL);
    err = reg_read16(dev, MCP23_GPIO, &port);
    if (!err)
        *level = (port & (1u << pin)) != 0;
    return EXPANDER_HANDLE_RET(err);
}

expander_handle_ret mcp23_write_port(mcp23_handle_t dev, uint16_t levels)
{
    if (dev == NULL)
        return EXPANDER_HANDLE_RET(-EINVAL);
    return EXPANDER_HANDLE_RET(write_port(dev, levels));
}

expander_handle_ret mcp23_read_port(mcp23_handle_t dev, uint16_t *levels)
{
    if (dev == NULL || levels == NULL)
        return EXPANDER_HANDLE_RET(-EINVAL);
    return EXPANDER_HANDLE_RET(reg_read16(dev, MCP23_GPIO, levels));
}

expander_handle_ret mcp23_set_interrupt(mcp23_handle_t dev, uint8_t pin,
                                        bool enable)
{
    if (dev == NULL)
        return EXPANDER_HANDLE_RET(-EINVAL);
    return EXPANDER_HANDLE_RET(set_interrupt(dev, pin, enable));
}

expander_handle_ret mcp23_read_interrupt(mcp23_handle_t dev, uint16_t *flags,
                                         uint16_t *captured)
{
    if (dev == NULL)
        return EXPANDER_HANDLE_RET(-EINVAL);
    return EXPANDER_HANDLE_RET(read_interrupt(dev, flags, captured));
}

expander_handle_ret mcp23_del(mcp23_handle_t dev)
{
    if (dev == NULL)
        return EXPANDER_HANDLE_RET(-EINVAL);
    release(dev);
    return EXPANDER_HANDLE_RET(0);
}

struct expander_dev *mcp23_expander(mcp23_handle_t dev)
{
    return dev ? &dev->expander : NULL;
}

#endif /* CONFIG_EXPANDER_MCP23 */
