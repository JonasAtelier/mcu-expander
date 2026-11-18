# Architecture, driver development and tests

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## Repository layout

```
include/                    public API, the only directory apps include
  expander.h                struct expander_dev, modes, pins and ports, interrupts, the INT hook
  expander_log.h            expander_err/warn/info/dbg, expander_log_port()
  mcp23.h                   the MCP23X17's handle API, mcp23s17_init() and mcp23_expander()
src/                        private to the library
  expander.c                the core: bound, pin and mask checks, the INT counter
  expander_log.c            expander_log_port()
  expander_priv.h           bus ops, attach and probe, errno maps
  expander_port_esp.c       all of that on ESP-IDF; empty elsewhere
  expander_port_stm.c       all of that on the STM32 HAL; empty under ESP-IDF
  mcp23/                    mcp23.c  mcp23_regs.h
test/                       host tests, `make -C test test`
  test_mcp23_regs.c         the register map against literals
  core_cases.h              core cases, shared by esp/ and stm/
  mcp23_cases.h             fake MCP23017s and MCP23S17s at register level and their cases, shared
  esp/                      suites on idf_stubs/, through expander_port_esp.c
  stm/                      suites on hal_stubs/, through expander_port_stm.c
  idf_stubs/  hal_stubs/    the slice of ESP-IDF and of the HAL the code calls
examples/
  esp/<chip>/               ESP-IDF projects: mcp23017 (esp-mcp23-expander's), mcp23s17
  stm/<chip>/               STM32 snippets: mcp23017, mcp23s17
docs/                       the five guides; the data sheet PDF here is gitignored
assets/                     README photo
CMakeLists.txt              ESP-IDF component; elsewhere a static library for add_subdirectory(); stm-forge globs src/**/*.c instead
                            the static library's options are Kconfig's bool entries, found by the type line directly under each `config`
Kconfig                     EXPANDER and EXPANDER_<CHIP>, for ESP-IDF and both forges' menus
idf_component.yml           ESP-IDF component manifest
```

Naming inside `src/<chip>/`, the same in every category library:

| File | Holds |
|---|---|
| `<chip>.c` | the driver: init, del, the ops, the chip's calls |
| `<chip>_regs.h` | register map: addresses, bit fields, values, each with its data sheet citation |
| `<chip>_proto.h` | serial protocol, for parts without registers |
| `<chip>_math.h` | pure conversions, compiled and asserted on the host |
| `<chip>_priv.h` | driver state, only if needed |

The MCP23X17 needs neither maths nor shared private state, so its folder is
`mcp23.c` and `mcp23_regs.h`. The chip driver keeps the original's
four-space style; the library's own files use tabs.

## Handles first

As in mcu-uwb: the chip's calls already had public names taking a handle,
and the ESP-IDF header had to stay as it was, so the handle stays the
device. `struct mcp23_t` embeds a `struct expander_dev`, init fills it and
points `priv` back at the handle, and `mcp23_expander()` hands it out. The
MCP23S17 is the same handle with a different transport: `mcp23s17_init()`
fills the same struct, and every other call takes either.

The category is what every port expander shares: a pin's mode, a masked
write and a read of the whole port, and, optionally, interrupts. The masked
write serves one pin and the whole port alike. Pins and masks are checked in
the core against `pins`, so a chip's ops never see one out of range. The INT
hook is in the core because it is the same for every chip: a counter the ISR
increments and the thread compares, on either platform.

## Platform ports

The driver in `src/` is the same C on both platforms; what differs is in
`src/expander_port_esp.c` and `src/expander_port_stm.c`: the bus ops,
attach and `expander_i2c_probe()` -- `i2c_master`, `spi_master` and the
UART driver on ESP-IDF; HAL I2C, SPI under a software chip select, and UART
on the HAL.

What only one platform has stays in the driver behind `#ifdef
ESP_PLATFORM`: the SPI clock and its 10 MHz limit on ESP-IDF; the SPI
handle checks on the HAL, and each init built only with its HAL module.

The handle calls return `esp_err_t` on ESP-IDF and errno on the HAL: the
driver works in errno and `EXPANDER_HANDLE_RET()` converts once where each
public call returns (`expander_esp_err()`).

Each chip file includes `expander_priv.h` before its `#if
CONFIG_EXPANDER_<CHIP>`: on ESP-IDF that is what brings in `sdkconfig.h`,
and a chip file that tested the option first would compile to nothing.

## Bus abstraction

The bus layer is mcu-stepper's: `struct expander_bus_ops` with
`transfer()`, `exchange()`, `receive()` and `detach()`, picked at run time
by which attach the driver calls. Every category library offers I2C, SPI
and UART, used or not, and `test/{esp,stm}/test_port_*` check all three. An
MCP23017 register access is one I2C `transfer()` with the register address
as `cmd`; an MCP23S17 one is one SPI `transfer()` with the control byte as
`cmd` and the register address leading `tx`. The I2C presence probe is
mcu-encoder's: `-ENXIO` when nothing ACKs, decided before anything is
allocated.

## Adding a chip

1. `include/foo.h`: the chip's handle API, with the platform's handle types
   behind `#ifdef ESP_PLATFORM`, and
   `struct expander_dev *foo_expander(foo_handle_t)`.
2. `src/foo/foo.c`, in a folder of its own. Include `expander_priv.h` first,
   then wrap the driver in `#if CONFIG_EXPANDER_FOO` (never `#ifdef`).
   Embed a `struct expander_dev` in the handle; give it `set_mode`, a
   masked `write` and `read`, and the interrupt ops if the chip has them.
3. In init: set `expander.name` first, refuse a bad config, check presence,
   allocate, attach, configure, and only on success set `expander.ops`,
   `expander.priv` and `expander.pins`. Check every bus transfer. Work in
   errno; return `EXPANDER_HANDLE_RET(err)`.
4. Add `CONFIG_EXPANDER_FOO` to the no-chip checks in `CMakeLists.txt` (its
   ESP-IDF part; the `add_subdirectory()` part reads `Kconfig`) and
   `src/expander_port_stm.c`, a HAL module `#error` to `src/expander_priv.h`
   if it needs one, its sources to `CMakeLists.txt`, a `config EXPANDER_FOO`
   to `Kconfig`, `# EXPANDER_FOO is not set` to both forges' defconfigs, and
   its tests to `test/Makefile`.

## Tests

```sh
make -C test test
```

Host-only; no toolchain, HAL or ESP-IDF needed. Nothing is checked against
real silicon. esp-mcp23-expander had no tests; all of these are new.

The fake chips in `test/mcp23_cases.h` know nothing of the driver. They keep
each chip's registers by port and model both register maps and the switch
between them, the address pointer and SEQOP, IOCON at two addresses, GPIO
reading pins and writing OLAT, IPOL, interrupt-on-change against the last
value or DEFVAL with INTF and INTCAP latched until GPIO or INTCAP is read,
MIRROR, and the MCP23S17's control byte with HAEN deciding which parts
answer -- two answering one read fails the test.

Each fix is checked by a test that fails without it, on both platforms:
dropping the BANK write, the IPOL reset, the MCP23S17's HAEN write through
address 0, the output-only check, the probe, the read-back, one checked
init write, or updating a register's RAM copy before the write succeeds.

| Binary | Checks |
|---|---|
| `test_mcp23_regs` | every address, IOCON bit, control byte, the output-only mask and the SPI limit against the data sheet, as literals |
| `esp/test_core`, `stm/test_core` | modes and pins through the ops; masked writes keep the rest and drop bits outside the mask; reads masked to the chip's pins; 32-pin chips; out-of-range pins, masks and modes refused without a call; failed reads leave the caller's value alone; optional interrupt ops, `-EOPNOTSUPP` without; the INT counter, once per batch, across its wrap; unbound devices; the log line; on the HAL `expander_dbg()` |
| `esp/test_mcp23`, `stm/test_mcp23` | init into the known state, IOCON from the config, the last address; **a warm chip in `BANK = 1` with IPOL, INTCON, DEFVAL, GPINTEN, GPPU, OLAT and IODIR dirty, all back**; nothing at the address refused before allocating; bad addresses and NULLs; a part that does not take its writes, `-ENODEV`; **every init transaction failed in turn, persistent and one-shot, its errno back and nothing leaked**; outputs, inputs with pull-ups, **pins 7 and 15 refused as inputs and interrupts on the MCP23017**, nothing sent; a failed write retried; whole-port writes in one transaction, masked ones through the category, none when nothing changes; interrupts: INTF and INTCAP, cleared by INTCAP or GPIO, INTCAP kept, MIRROR; **the MCP23S17**: init, no output-only pins, MISO floating high or low is no chip; **two MCP23S17s on one chip select in either order**. ESP-IDF: the probe and device at the strapped address and SCL clock, removed at del and after a failed init; SPI at 10 MHz unless asked, mode 0, over 10 MHz refused before adding; the `esp_err_t` codes. HAL: the address shifted; every SPI transaction in its own chip select, released after a failure; mode, phase, frame size and NULLs refused before the bus; the INT line through an EXTI callback, `expander_irq_pending()` and the interrupt read |
| `esp/test_port_esp`, `stm/test_port_stm` | the bus ops as in mcu-stepper (I2C, SPI, UART with exchange and receive, the HAL's no-overrun echo) and the presence probe with its codes |

All build under `-std=gnu11 -Wall -Wextra -Werror -pedantic` with UBSan
(`-fsanitize=undefined -fno-sanitize-recover=all`), and `-Wl,--wrap` (GNU
ld) to count allocations and capture `printf()`.
