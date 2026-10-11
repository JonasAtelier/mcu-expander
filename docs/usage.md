# API usage

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## The handle and the category view

The MCP23X17 keeps the handle API esp-mcp23-expander had: `mcp23_init()`
(MCP23017) or `mcp23s17_init()` (MCP23S17) fills a handle, every chip call
takes it, and `mcp23_del()` frees it. The handle holds a
`struct expander_dev`, and `mcp23_expander(h)` hands it out:

| Call | Does |
|---|---|
| `expander_set_mode(e, pin, mode)` | `EXPANDER_INPUT`, `EXPANDER_INPUT_PULLUP` or `EXPANDER_OUTPUT` |
| `expander_write(e, pin, level)` / `expander_read(e, pin, &level)` | one pin |
| `expander_write_port(e, mask, levels)` | the pins in `mask` take their bit of `levels`, the rest keep theirs |
| `expander_read_port(e, &levels)` | every pin, bit n for pin n |
| `expander_set_interrupt(e, pin, enable)` | interrupt when this input changes |
| `expander_read_interrupt(e, &flags, &captured)` | which pins fired and the port as it was then; clears it |
| `expander_irq(e)` / `expander_irq_pending(e)` | the INT line's ISR side and thread side; below |
| `expander_log_port(e, levels)` | one info line |

`e->pins` is the chip's pin count, 16 here. Both views are the same chip:
`mcp23_write()` and `expander_write()` set one latch. Pins and masks past
`pins` are `-EINVAL`, and nothing is sent. The `struct expander_dev *` is
valid from a successful init to the del.

## Pins, ports and transactions

| Call | Bus transactions |
|---|---|
| `mcp23_write`, `expander_write` | one, or none if the latch already holds that level |
| `mcp23_write_port`, `expander_write_port` with every pin in the mask | one, always |
| `expander_write_port` with some pins | one, or none if nothing changes |
| `mcp23_set_mode` | one or two (pull-up, direction), none if nothing changes |
| `mcp23_read`, `mcp23_read_port`, `expander_read*` | one |

The driver keeps the direction, pull-up, latch and interrupt-enable
registers in RAM, so a one-pin change costs one write, not a read and a
write. A failed write leaves its copy as the chip still has it.

A write to an input sets its latch for when it becomes an output. Pull-ups
only act on inputs.

**On the MCP23017, GPA7 and GPB7 (pins 7 and 15) are output only**:
`mcp23_set_mode()` refuses `INPUT` and `INPUT_PULLUP` on them, and
`mcp23_set_interrupt()` refuses to enable them, with `-EINVAL` /
`ESP_ERR_INVALID_ARG`. Init leaves them as the chip powers up, inputs,
high-impedance, until you make them outputs. See
[the chip reference](chips.md#the-mcp23017s-output-only-pins).

## Interrupts and the INT line

```c
mcp23_set_mode(h, 8, MCP23_INPUT_PULLUP);
mcp23_set_interrupt(h, 8, true);
```

The chip then drives INT when pin 8 changes, latches which pins fired
(`flags`) and the port at that instant (`captured`), and holds INT until
the interrupt is read. **You must call `mcp23_read_interrupt()` or
`expander_read_interrupt()`**: until then the chip reports no further
change on that port. A plain `mcp23_read()` reads GPIO, which also clears
the interrupt and can lose `captured`; read the interrupt first.

The bus must not be touched from an ISR. The hook, on either platform:

```c
/* ISR side: the GPIO ISR (ESP-IDF) or EXTI callback (HAL) */
expander_irq(mcp23_expander(h));          /* only counts */

/* thread side */
if (expander_irq_pending(e))              /* true once per batch of counts */
    expander_read_interrupt(e, &flags, &captured);
```

It is a counter, not a flag, so an edge that lands between the thread's look
and its clear is never lost. One thread per device calls
`expander_irq_pending()`.

INT is active-low push-pull by default; `int_active_high` makes it
active-high, and `int_open_drain` makes it open-drain for sharing a line,
which overrides `int_active_high` (DS Register 3-5). `int_mirror` ORs the
two ports, so INTA and INTB both go active for either.

## Buses

- **MCP23017 on ESP-IDF:** an `i2c_master_bus_handle_t` the app has
  created; the driver checks something answers at the address
  (`i2c_master_probe()`), then adds its device at `scl_speed_hz` (0:
  400 kHz) and removes it at del.
- **MCP23017 on the HAL:** an `I2C_HandleTypeDef *` the app has
  `HAL_I2C_Init()`ed; presence by `HAL_I2C_IsDeviceReady()`; the clock is
  the handle's and `scl_speed_hz` is ignored.
- **MCP23S17 on ESP-IDF:** an SPI host the app has `spi_bus_initialize()`d;
  the driver adds its device on `cs_gpio` at `spi_hz` (0: 10 MHz), mode 0,
  and removes it at del. Two MCP23S17s on one chip select are two devices
  with the same `cs_gpio`.
- **MCP23S17 on the HAL:** an `SPI_HandleTypeDef *` in mode 0 with 8-bit
  frames, checked at init, and a chip select port and pin.

## Logging

On ESP-IDF everything goes under the log tag `expander`, prefixed with the
chip name (`mcp23017`, `mcp23s17`). On the HAL every line goes through
`printf()` as `expander: <chip>: ...`. Your own code can use the same
`expander_err()`, `expander_warn()`, `expander_info()` and `expander_dbg()`
from `expander_log.h`. `expander_dbg()` is compiled out on the HAL unless
`CONFIG_EXPANDER_DEBUG` is 1 (stm-forge: `EXPANDER_DEBUG = y`).

## Errors

`expander_*` calls return 0 or a negative errno on both platforms. The chip
calls return `esp_err_t` on ESP-IDF, as esp-mcp23-expander did, and errno on
the HAL:

| Meaning | HAL | ESP-IDF |
|---|---|---|
| bad argument or config, NULL handle, pin out of range, an output-only pin as input, an SPI handle not mode 0 / 8-bit, `spi_hz` over 10 MHz | `-EINVAL` | `ESP_ERR_INVALID_ARG` |
| no memory for the handle | `-ENOMEM` | `ESP_ERR_NO_MEM` |
| MCP23017: nothing answers at the address | `-ENXIO` | `ESP_ERR_NOT_FOUND` |
| the registers do not read back what init wrote: no MCP23X17 there (on SPI, the only sign) | `-ENODEV` | `ESP_ERR_NOT_FOUND` |
| the bus timed out | `-ETIMEDOUT` | `ESP_ERR_TIMEOUT` |
| `expander_set/read_interrupt` on a chip without | `-EOPNOTSUPP` | — |
| bus busy (HAL_BUSY) | `-EAGAIN` | — |
| other bus failure, a NACK mid-transfer included | `-EIO` | `ESP_FAIL` |

On ESP-IDF a bus error passes through errno on its way up, and the map is
lossy in that direction: an IDF code with no errno of its own comes back as
`ESP_FAIL`. `strerror(-ret)` names any errno.

## Changes from esp-mcp23-expander

`mcp23.h` is token-for-token the original's on ESP-IDF, plus
`mcp23s17_config_t`, `mcp23s17_init()`, `mcp23_expander()` and the headers
they need. Behaviour changes, deliberately:

- **A chip left in the other register layout is put back** (an upstream bug
  fixed). Init wrote IOCON at 0x0A assuming `IOCON.BANK = 0`; after a warm
  MCU restart following firmware that set `BANK = 1`, 0x0A is OLATA, so the
  IOCON byte drove the output latches and every later access hit the wrong
  register. Init now writes 0 to 0x05 first -- IOCON with `BANK = 1`,
  GPINTENB with `BANK = 0`, which init clears anyway.
- **Init resets IPOL, DEFVAL and INTCON too.** The original promised "a
  known state" but left them as a warm restart found them: inputs could read
  inverted, and interrupts compare against DEFVAL.
- **GPA7 and GPB7 refuse to be inputs on the MCP23017**, as the data sheet
  has said since Rev D. The original allowed it.
- **Presence is checked with an I2C probe**, before anything is allocated,
  and **init reads back what it wrote**. The original took a timed-out first
  read as "not found", which a NACK need not be. Nothing at the address is
  still `ESP_ERR_NOT_FOUND`; so is a part that answers but does not hold
  what was written.
- `mcp23_del()` returns `ESP_OK`; the original passed on
  `i2c_master_bus_rm_device()`'s result.
- Log lines go under the tag `expander` as `mcp23017: ...`; the original
  logged nothing.
- Errors pass through errno inside the driver, so an I2C error with no errno
  of its own comes back as `ESP_FAIL` rather than the original IDF code.
- Comments in `mcp23.h` corrected: 1.7 MHz needs a 4.5–5.5 V supply;
  `int_mirror` ORs the two ports rather than moving them to INTA;
  `int_open_drain` overrides `int_active_high`; a pin read clears a pending
  interrupt.
