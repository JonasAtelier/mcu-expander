# mcu-expander

Add 16 digital input or output pins with a Microchip MCP23017 over I2C or MCP23S17 over SPI. Read a button, drive an LED or watch for pin changes.

**Platforms** · ESP-IDF 5.2+ and STM32 HAL. Hardware testing is pending. The host tests use simulated devices.

## Contents

- [Quick start](#quick-start)
- [Drivers and examples](#drivers-and-examples)
- [Using the library in your project](#using-the-library-in-your-project)
- [License](#license)

## Quick start

Use I2C address 0x20. Tie RESET to VDD, set the address pins and connect a button from GPB0 to ground.

In a terminal with ESP-IDF activated, get the example and select the target for an ESP32. Change the target if your board uses another ESP32 chip.

```sh
git clone https://github.com/JonasAtelier/mcu-expander.git expander
cd expander/examples/esp/mcp23017
idf.py set-target esp32
```

Check the [wiring guide](docs/hardware.md) and edit the settings in [main.c](examples/esp/mcp23017/main/main.c) to match your board. Connect the board by USB, then build, upload and open the serial monitor.

```sh
idf.py build flash monitor
```

Press the button to see its state in the monitor. With LEDs and series resistors fitted to port A, the example also lights each LED in turn.

## Drivers and examples

Both parts use the same pin and port API. Use `expander_write_port()` to update several outputs with a mask, or use the MCP23 interrupt settings to capture a change on an input. [Shared API](include/expander.h)

| Chip | Driver header | ESP-IDF project | STM32 HAL example |
|---|---|---|---|
| [MCP23017 (I2C)](docs/chips.md#mcp23017--mcp23s17) | [mcp23.h](include/mcp23.h) | [LEDs and button](examples/esp/mcp23017/) | [Buttons on the interrupt line](examples/stm/mcp23017/main.c) |
| [MCP23S17 (SPI)](docs/chips.md#mcp23017--mcp23s17) | [mcp23.h](include/mcp23.h) | [Two expanders on one chip select](examples/esp/mcp23s17/) | [Sixteen SPI outputs](examples/stm/mcp23s17/main.c) |

## Using the library in your project

- Add the component and enable the driver for your hardware. [Installation and build options](docs/installation.md)
- Open the ESP-IDF project for your hardware, or copy the STM32 example into a project with board setup already in place. [All ESP-IDF examples](examples/esp/) · [All STM32 examples](examples/stm/)
- Look up calls and chip-specific settings. [API usage](docs/usage.md) · [Hardware reference](docs/chips.md)

To run the simulated tests on your computer, run `make -C test test` from the library root. [Test requirements and coverage](docs/development.md)

## License

© 2026 JonasAtelier, [GPL-3.0-only](LICENSE)

<p align="center">
  <img src="assets/module.jpg"
       alt="An Adafruit MCP23017 I/O expander breakout"
       width="520">
  <br><sub>Adafruit MCP23017 breakout &mdash; photo <a href="https://www.adafruit.com/product/732">Adafruit Industries</a></sub>
</p>

[Browse the workshop catalog · ESP32 and STM32 drivers](https://github.com/JonasAtelier/jonas-atelier-libraries#esp32-and-stm32-drivers)
