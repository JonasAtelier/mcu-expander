/*
 * mcp23 — ESP-IDF driver for the Microchip MCP23017 16-bit I/O expander.
 *
 * Datasheet in docs/. Two I2C wires buy you 16 more GPIOs, each one an input,
 * an input with a pull-up, or an output — and the chip can raise an interrupt
 * when an input changes, so you do not have to poll it.
 *
 * Pins are numbered 0-15: 0-7 are PORTA (GPA0-GPA7), 8-15 are PORTB
 * (GPB0-GPB7). Port-wide calls use the same numbering, bit 0 for pin 0.
 */
/*
 * In mcu-expander: the same calls on the STM32 HAL, where they return 0 or
 * a negative errno; the MCP23S17, its SPI twin, through mcp23s17_init(),
 * which hands out the same handle; and mcp23_expander(), the handle as a
 * struct expander_dev. On the MCP23017, GPA7 and GPB7 (pins 7 and 15) are
 * output only (datasheet DS20001952E, 3.5.1): mcp23_set_mode() refuses to
 * make them inputs, and mcp23_set_interrupt() to watch them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#else
#include CMSIS_device_header
#endif

#include "expander.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCP23_ADDR_BASE 0x20    /* A2..A0 all low; the pins add 0 to 7 */
#define MCP23_PINS      16

typedef enum {
    MCP23_INPUT = 0,
    MCP23_INPUT_PULLUP,     /* ~100k internal pull-up, for buttons and switches */
    MCP23_OUTPUT,
} mcp23_mode_t;

typedef struct mcp23_t *mcp23_handle_t;

typedef struct {
    uint8_t  addr;            /* 0 means 0x20; otherwise 0x20-0x27 to match
                               * how you strapped A2/A1/A0 */
    uint32_t scl_speed_hz;    /* 0 means 400000, which needs 2.7 V or more;
                               * 1.7 MHz needs 4.5-5.5 V (DS Table 1-3) */
    bool     int_mirror;      /* OR the two ports' interrupts: INTA and INTB
                               * both go active for either port, so one pin
                               * wired to the MCU sees all sixteen */
    bool     int_open_drain;  /* open-drain INT, for sharing the line */
    bool     int_active_high; /* default is active-low; no effect with
                               * int_open_drain, which overrides it */
} mcp23_config_t;

#ifdef ESP_PLATFORM
/* Attach to an already-initialised I2C master bus and put every pin back to
 * a known state: all inputs, no pull-ups, no interrupts. cfg may be NULL for
 * defaults. Fails with ESP_ERR_NOT_FOUND if the chip does not answer. */
esp_err_t mcp23_init(i2c_master_bus_handle_t bus, const mcp23_config_t *cfg,
                     mcp23_handle_t *out);

/* One pin at a time. A read reads GPIO, which also clears a pending
 * interrupt; call mcp23_read_interrupt() first if one may be pending. */
esp_err_t mcp23_set_mode(mcp23_handle_t dev, uint8_t pin, mcp23_mode_t mode);
esp_err_t mcp23_write(mcp23_handle_t dev, uint8_t pin, bool level);
esp_err_t mcp23_read(mcp23_handle_t dev, uint8_t pin, bool *level);

/* All sixteen at once, in a single I2C transaction. Much faster than looping
 * over the pins, and the outputs change together. */
esp_err_t mcp23_write_port(mcp23_handle_t dev, uint16_t levels);
esp_err_t mcp23_read_port(mcp23_handle_t dev, uint16_t *levels);

/* Ask the chip to pull its INT pin when this input changes. Wire INT to an
 * ESP32 GPIO interrupt and stop polling. */
esp_err_t mcp23_set_interrupt(mcp23_handle_t dev, uint8_t pin, bool enable);

/* What caused the interrupt. `flags` marks which pins fired; `captured` is
 * the port as it was at that instant, which may differ from what the pins
 * read now. Either pointer may be NULL.
 *
 * Reading this clears the interrupt and releases the INT pin. */
esp_err_t mcp23_read_interrupt(mcp23_handle_t dev, uint16_t *flags,
                               uint16_t *captured);

esp_err_t mcp23_del(mcp23_handle_t dev);

/* The MCP23S17 on an SPI host already spi_bus_initialize()d. */
typedef struct {
    spi_host_device_t host;
    int      cs_gpio;
    uint32_t spi_hz;          /* 0 means 10 MHz; above that init refuses.
                               * 5 MHz at most below 2.7 V */
    uint8_t  addr;            /* 0-7, as A2/A1/A0 are strapped */
    bool     int_mirror;      /* as in mcp23_config_t; every MCP23S17 on one
                               * chip select must use the same three */
    bool     int_open_drain;
    bool     int_active_high;
} mcp23s17_config_t;

/* As mcp23_init(), for the SPI part; every other mcp23_* call takes the
 * handle it gives. */
esp_err_t mcp23s17_init(const mcp23s17_config_t *cfg, mcp23_handle_t *out);
#else
#ifdef HAL_I2C_MODULE_ENABLED
/* bus is HAL_I2C_Init()ed by the app; scl_speed_hz is ignored, the clock is
 * the handle's. -ENXIO if nothing answers at the address. */
int mcp23_init(I2C_HandleTypeDef *bus, const mcp23_config_t *cfg,
               mcp23_handle_t *out);
#endif

int mcp23_set_mode(mcp23_handle_t dev, uint8_t pin, mcp23_mode_t mode);
int mcp23_write(mcp23_handle_t dev, uint8_t pin, bool level);
int mcp23_read(mcp23_handle_t dev, uint8_t pin, bool *level);
int mcp23_write_port(mcp23_handle_t dev, uint16_t levels);
int mcp23_read_port(mcp23_handle_t dev, uint16_t *levels);
int mcp23_set_interrupt(mcp23_handle_t dev, uint8_t pin, bool enable);
int mcp23_read_interrupt(mcp23_handle_t dev, uint16_t *flags,
                         uint16_t *captured);
int mcp23_del(mcp23_handle_t dev);

#ifdef HAL_SPI_MODULE_ENABLED
/* The MCP23S17 on an SPI handle the app has HAL_SPI_Init()ed in mode 0
 * (CPOL low, CPHA first edge), 8-bit frames, at 10 MHz or less (5 MHz
 * below 2.7 V); chip select is a push-pull output idling high. */
typedef struct {
    SPI_HandleTypeDef *spi;
    GPIO_TypeDef      *cs_port;
    uint16_t           cs_pin;
    uint8_t            addr;            /* 0-7, as A2/A1/A0 are strapped */
    bool               int_mirror;      /* as in mcp23_config_t; every
                                         * MCP23S17 on one chip select
                                         * must use the same three */
    bool               int_open_drain;
    bool               int_active_high;
} mcp23s17_config_t;

int mcp23s17_init(const mcp23s17_config_t *cfg, mcp23_handle_t *out);
#endif
#endif

/* The handle as a struct expander_dev; NULL for a NULL handle. Valid until
 * mcp23_del(). */
struct expander_dev *mcp23_expander(mcp23_handle_t dev);

#ifdef __cplusplus
}
#endif
