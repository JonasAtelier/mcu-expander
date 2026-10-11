# Hardware wiring

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

Names are the data sheet's, `docs/MCP23017-MCP23S17-...-DS20001952.pdf`
(DS20001952E, "DS"), Table 2-1. A breakout may silkscreen VSS as GND.

## MCP23017 — `examples/*/mcp23017`

| Pin | ESP-IDF (example) | STM32 (example) | Note |
|---|---|---|---|
| VDD | 3.3 V | 3.3 V | the chip takes 1.8–5.5 V; run it at the MCU's logic level |
| VSS | GND | GND | shared ground, always |
| SDA | GPIO 21 | I2C1 SDA | with a pull-up |
| SCL | GPIO 22 | I2C1 SCL | with a pull-up |
| A2, A1, A0 | GND or VDD | GND or VDD | address 0x20 plus the three-bit value you strap, so 0x20 to 0x27. Never floating |
| RESET | VDD | VDD | active low; the library never drives it |
| INTA | a GPIO, or open | — | PORTA's interrupt |
| INTB | a GPIO, or open | PB0, EXTI0 | PORTB's. With `int_mirror` both pins go active for either port, so wiring one is enough |
| GPA0–GPA7, GPB0–GPB7 | your LEDs and buttons | | pins 0–15; **GPA7 and GPB7 output only** on this part |

**I2C speed** depends on the supply (DS Table 1-3): 100 kHz from 1.8 V,
400 kHz from 2.7 V, 1.7 MHz only at 4.5–5.5 V. ESP-IDF: `scl_speed_hz`, 0
for 400 kHz. HAL: the handle's clock.

## MCP23S17 — `examples/*/mcp23s17`

| Pin | ESP-IDF (example) | STM32 (example) | Note |
|---|---|---|---|
| VDD, VSS | 3.3 V, GND | 3.3 V, GND | as above |
| SCK | GPIO 18 | SPI1 SCK | |
| SI | GPIO 23 (MOSI) | SPI1 MOSI | |
| SO | GPIO 19 (MISO) | SPI1 MISO | |
| CS | GPIO 5 | PA4 | one chip select can serve up to eight parts, told apart by A2..A0. On the HAL a push-pull output idling high, driven by the library |
| A2, A1, A0 | GND or VDD | GND or VDD | the part's address, 0–7; must be biased even when unused (DS 3.3.2) |
| RESET | VDD | VDD | as above |
| INTA, INTB | a GPIO | — | as above |

**SPI** is mode 0 (CPOL low, CPHA first edge). The part also takes mode
1,1, but then its CS must toggle once after power-up (DS Table 1-4, Note
1), so the library refuses it on the HAL and uses mode 0 on ESP-IDF. Clock
at most 10 MHz from 2.7 V, 5 MHz below (DS Table 1-4). ESP-IDF: `spi_hz`, 0
for 10 MHz, above it init refuses. HAL: the handle's prescaler, mode 0 and
8-bit frames checked at init.

**Several MCP23S17s on one chip select** must all use the same `int_*`
settings; see [the chip reference](chips.md#mcp23s17-addresses-and-haen).

## Pin numbering

| Pins | Chip |
|---|---|
| 0–7 | PORTA, `GPA0`–`GPA7` |
| 8–15 | PORTB, `GPB0`–`GPB7` |

Port-wide calls use the same numbering, bit 0 for pin 0: `0x0001` is GPA0,
`0x0100` is GPB0.

## Electrical limits

From the absolute maximum ratings (DS 1.0, p.3) -- limits, not operating
points:

- 25 mA into or out of any one pin; 150 mA out of VSS and 125 mA into VDD
  for the whole chip. A bank of bright LEDs wants a driver transistor.
- GPIO low is guaranteed at most 0.6 V at 8 mA (DS Table 1-1, D080).
- Pull-ups are the internal 100 kΩ ones (DS 3.5.7). On a long or noisy
  wire, fit a real resistor.
