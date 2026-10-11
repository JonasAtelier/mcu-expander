# Chip reference

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## MCP23017 / MCP23S17

Microchip's 16-bit I/O expanders, the MCP23X17 family: two 8-bit ports,
each pin an input, an input with a 100 kΩ pull-up, or an output, with
interrupt-on-change. The two parts "differ only in the serial interface"
(DS 3.0, p.12): the MCP23017 is on I2C, the MCP23S17 on SPI, and one driver
serves both. Data sheet: DS20001952E (Rev E, July 2026),
`docs/MCP23017-MCP23S17-...-DS20001952.pdf` (gitignored), "DS".

| Function | Does |
|---|---|
| `mcp23_init(bus, &cfg, &h)` | MCP23017: probe, attach, put the chip in a known state, check it |
| `mcp23s17_init(&cfg, &h)` | MCP23S17: the same, on SPI |
| `mcp23_set_mode(h, pin, mode)` | input, input with pull-up, or output |
| `mcp23_write` / `mcp23_read` | one pin |
| `mcp23_write_port` / `mcp23_read_port` | all sixteen, one transaction |
| `mcp23_set_interrupt` / `mcp23_read_interrupt` | interrupt on change; what fired |
| `mcp23_expander(h)` | the handle as a `struct expander_dev` |
| `mcp23_del(h)` | detach and free |

### Registers

The power-on layout, `IOCON.BANK = 0`, puts each register's PORTA and PORTB
bytes side by side (DS Table 3-5, p.17–18), and with `IOCON.SEQOP = 0` the
address pointer increments (DS 3.2.1, p.13). So one 2-byte access is one
16-bit register, low byte PORTA (pins 0–7), high byte PORTB (pins 8–15):

| Register | Address | Does | POR |
|---|---|---|---|
| IODIR | 0x00 | 1 = input (DS 3.5.1) | 0xFFFF |
| IPOL | 0x02 | 1 = GPIO reads the pin inverted (DS 3.5.2) | 0 |
| GPINTEN | 0x04 | interrupt-on-change enable (DS 3.5.3) | 0 |
| DEFVAL | 0x06 | compare value (DS 3.5.4) | 0 |
| INTCON | 0x08 | 1 = compare with DEFVAL, 0 = with the last value (DS 3.5.5) | 0 |
| IOCON | 0x0A, 0x0B | configuration, one byte at both (DS 3.5.6) | 0 |
| GPPU | 0x0C | 1 = 100 kΩ pull-up on an input (DS 3.5.7) | 0 |
| INTF | 0x0E | which pin fired, read-only (DS 3.5.8) | 0 |
| INTCAP | 0x10 | the port when it fired (DS 3.5.9) | 0 |
| GPIO | 0x12 | the pins; a write goes to OLAT (DS 3.4, 3.5.10) | 0 |
| OLAT | 0x14 | the output latches (DS 3.5.11) | 0 |

IOCON (DS Register 3-5, p.21): BANK bit 7, MIRROR 6, SEQOP 5, DISSLW 4,
HAEN 3, ODR 2, INTPOL 1. The driver writes MIRROR, ODR and INTPOL from the
config, HAEN on the MCP23S17, and keeps BANK and SEQOP at 0.

`src/mcp23/mcp23_regs.h` holds every value with its citation;
`test/test_mcp23_regs.c` checks them against literals.

### Start-up

`mcp23_init()` and `mcp23s17_init()`, in order:

1. MCP23017: probe the address; nothing there is `-ENXIO`, before anything
   is allocated.
2. Attach: add the I2C or SPI device on ESP-IDF; bind the handle on the HAL.
3. MCP23S17: IOCON with HAEN through address 0 (below).
4. Write 0 to 0x05. With `BANK = 1` that is IOCON (DS Table 3-4, p.17), so
   the chip returns to `BANK = 0`; with `BANK = 0` it is GPINTENB, cleared
   in step 6 anyway. Without this, a chip that another firmware left in
   `BANK = 1` -- RESET tied high, an MCU restart -- would take the next
   write at 0x0A as OLATA.
5. IOCON from the config.
6. Every register to its power-on value: IODIR all inputs, GPPU, OLAT,
   GPINTEN, IPOL, DEFVAL and INTCON 0 (DS Table 3-5).
7. Read 0x00–0x0A back in one access and compare with what was written.
   Anything else is `-ENODEV`. On SPI nothing acknowledges, so this is the
   only sign a chip is there: MISO left floating reads all ones or all
   zeros, and IODIR and IOCON cannot both match either.

Every transfer's result is checked; the first failure ends init with its
code and frees what init had set up. Ten transactions on the MCP23017,
eleven on the MCP23S17.

### The MCP23017's output-only pins

Since Rev D (June 2022) the data sheet says GPA7 and GPB7 "are output only
for MCP23017" (DS p.1), and the IODIR note reads "For MCP23017, IO7 must be
set via I2C interface to '0' (output)" (DS 3.5.1, p.18). esp-mcp23-expander
let them be inputs.

On the MCP23017 the driver refuses `MCP23_INPUT` and `MCP23_INPUT_PULLUP`
on pins 7 and 15, and refuses to enable their interrupts, with `-EINVAL`.
Making them outputs, writing them and reading them back are fine. Init
leaves them as the chip powers up -- inputs, high-impedance -- rather than
driving them: whatever the board has there should not be fought before the
app says what the pins are. Set them to outputs early. The MCP23S17 has no
such limit.

### MCP23S17: addresses and HAEN

The SPI control byte is `0100 A2 A1 A0 R/W` (DS 3.3.2 and Figure 3-5,
p.15), followed by the register address and the data, all under one chip
select (DS 3.2.3, Figure 3-7). Up to eight parts can share one chip select,
told apart by A2..A0 -- but only once `IOCON.HAEN` is set; until then every
part answers to address 000, whatever its pins (DS 3.5.6, p.20).

So the driver always sets HAEN, and writes it first through address 0. That
write reaches every MCP23S17 on the chip select still at HAEN = 0 -- they
all move to their own addresses, and no two answer the same read after it
-- and also the part strapped 000, whether or not it was set up already.
Hence the rule: **all MCP23S17s on one chip select use the same
`int_mirror`, `int_open_drain` and `int_active_high`**, or the last init
decides the part at 000's INT pin. Any order of init works.

A part that another firmware left with `BANK = 1` takes the IOCON write
through address 0 (step 3) at 0x0A, which with `BANK = 1` is OLATA: port A
briefly drives `iocon | HAEN` on any pin that is still an output, until
steps 4–6 put `BANK = 0`, OLAT 0 and every pin back to an input. Such a
part with HAEN = 0 and strapped elsewhere than 000 does not hear step 4 at
its own address at all; power-cycle it or hold RESET low once.

The MCP23017's address pins are always enabled (DS Register 3-5 Note 1).

### Interrupts

Only inputs with GPINTEN set can interrupt (DS 3.6.4, p.25). The driver
leaves INTCON at 0: each pin is compared with its last value, so any change
fires, and after the interrupt is cleared the port as it then is becomes the
reference for the next change (DS 3.6.5). DEFVAL compare is not offered.

On the first change the chip copies the port into INTCAP and sets INTF;
further changes on that port do not interrupt until it is cleared, by
reading INTCAP or GPIO of that port (DS 3.6.4, Table 3-6). INTCAP keeps its
value until the next interrupt. Reading GPIO first while another change is
pending can lose INTCAP (DS 3.6.4 Note), which is why
`mcp23_read_interrupt()` reads INTF and INTCAP and never GPIO.

## Provenance

Ported from esp-mcp23-expander master (cf2df2d). The driver is the
original's, moved onto this library's bus layer and errno, with the changes
listed in [API usage](usage.md#changes-from-esp-mcp23-expander). Its eight
register addresses and three IOCON bits agree with the data sheet; the
others are new here, each cited. The MCP23S17 transport is new.
