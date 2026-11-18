# mcu-expander

Add 16 digital input or output pins with a Microchip MCP23017 over I2C or MCP23S17 over SPI. Read a button, drive an LED or watch for pin changes.

**Platforms** · ESP-IDF 5.2+ and STM32 HAL. Hardware testing is pending. The host tests use simulated devices.

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

## Using the library in your project

- Add the component and enable the driver for your hardware. [Installation and build options](docs/installation.md)
- Start from an example for your chip. The STM32 examples fit into a project with board setup already in place. [ESP-IDF examples](examples/esp/) · [STM32 examples](examples/stm/)
- Look up calls and chip-specific settings. [API usage](docs/usage.md) · [Hardware reference](docs/chips.md)

To run the simulated tests on your computer, run `make -C test test` from the library root. [Test requirements and coverage](docs/development.md)

## License

[MIT](LICENSE)

<p align="center">
  <img src="assets/module.jpg"
       alt="An Adafruit MCP23017 I/O expander breakout"
       width="520">
  <br><sub>Adafruit MCP23017 breakout &mdash; photo <a href="https://www.adafruit.com/product/732">Adafruit Industries</a></sub>
</p>
