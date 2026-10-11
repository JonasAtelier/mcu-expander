# Installation

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

The repository is `mcu-expander`; the forge package key, C API and
configuration options are `expander`. The clone command below names the
ESP-IDF component directory `expander`. It replaces `esp-mcp23-expander`
(forge key `mcp23`): a project that `REQUIRES mcp23` now
`REQUIRES expander`; `mcp23.h` and its calls are unchanged.

## With forge

stm-forge must be at `ee921c3` or later ("build: libraries may keep sources
in src/ subfolders"); older ones only compile `src/*.c`, so the chip driver
in `src/mcp23/` is never built and the link fails with undefined references
to the chip functions. esp-forge is unaffected: the component's CMakeLists
lists its sources.

With [esp-forge](https://github.com/JonasAtelier/esp-forge) or
[stm-forge](https://github.com/JonasAtelier/stm-forge). The catalog line in
`profiles/pkgversion` is the same for both:

```make
expander_GIT=https://github.com/JonasAtelier/mcu-expander@stable
```

and a target turns the library and its chip on in `configs/<target>_defconfig`:

```make
PKG_expander        = y
EXPANDER_MCP23      = y
# EXPANDER_DEBUG is not set
```

or with `make menu`. esp-forge turns each line into `CONFIG_EXPANDER_<CHIP>=y`
in `sdkconfig.defaults`; stm-forge passes it as `-DCONFIG_EXPANDER_<CHIP>=1`.
`EXPANDER_DEBUG = y` turns on `expander_dbg()` lines on the HAL; on ESP-IDF
the log level decides.

## ESP-IDF

Clone into your project's `components/` directory. A local component is
named after its directory, and that name is what you `REQUIRES`:

```sh
git clone https://github.com/JonasAtelier/mcu-expander components/expander
```

```cmake
idf_component_register(SRCS "main.c" REQUIRES expander driver)
```

Enable the chip under `idf.py menuconfig` → **Component config → GPIO
expander subsystem**, or put `CONFIG_EXPANDER_MCP23=y` in your
`sdkconfig.defaults` as each example in `examples/esp/` does. With the
subsystem on and no chip picked, the build warns. ESP-IDF 5.2 or newer is
required for the `i2c_master` API.

## STM32 HAL

Copy `include/` and `src/` into an STM32 project, compile every `.c` under
`src/`, chip folders included, add `include/` and `src/` to the include path
(`src/` only needs to reach this library's own sources), and define what the
forge would: `CMSIS_device_header` as your family header (`"stm32f4xx.h"`,
`"stm32f7xx.h"`, `"stm32g0xx.h"`), `USE_HAL_DRIVER`, and
`CONFIG_EXPANDER_MCP23=1`.

**No chip driver is built by default.** The core is always built and each
driver is opt-in: a chip whose option is absent or 0 compiles to nothing.
With the library on and no chip picked, the ESP-IDF CMakeLists warns and
`src/expander_port_stm.c` emits a `#warning`.

**The HAL modules are yours to enable:**

| Part | Needs in hal_conf | HAL source | Gives |
|---|---|---|---|
| MCP23017 | `HAL_I2C_MODULE_ENABLED` | `stm32<family>xx_hal_i2c.c` | `mcp23_init()` |
| MCP23S17 | `HAL_SPI_MODULE_ENABLED` | `stm32<family>xx_hal_spi.c` | `mcp23s17_init()` |

Either will do; each init is built only with its own module. With neither,
the build stops with an `#error` in `src/expander_priv.h` that says which
defines are missing. The bus layer's UART code sits under its own module
switch and builds to nothing without it.

**printf() is the log.** Lines go out through `printf()`, so retarget
newlib's `_write()` to a UART.
