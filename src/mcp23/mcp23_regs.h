/*
 * MCP23017 / MCP23S17 register map, control byte and power-on values.
 *
 * Every value here is quoted from Microchip's data sheet DS20001952E
 * (docs/MCP23017-MCP23S17-...-DS20001952.pdf, "DS"), with its section,
 * table or register and page. The two parts differ only in the serial
 * interface (DS 3.0, p.12): one register map serves both.
 *
 * Kept free of platform headers so the host test checks the packing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ---- Register addresses with IOCON.BANK = 0, the POR default: DS Table
 * 3-1 (p.12) and Table 3-5 (p.17-18). Each pair is PORTA then PORTB, and
 * with SEQOP = 0 the address pointer increments (DS 3.2.1, p.13), so a
 * 2-byte access covers both ports as a 16-bit word: low byte PORTA (pins
 * 0-7), high byte PORTB (pins 8-15). ---- */
#define MCP23_IODIR    0x00 /* 1 = input; DS 3.5.1, p.18 */
#define MCP23_IPOL     0x02 /* 1 = GPIO reads inverted; DS 3.5.2, p.18 */
#define MCP23_GPINTEN  0x04 /* interrupt-on-change enable; DS 3.5.3, p.19 */
#define MCP23_DEFVAL   0x06 /* compare value, with INTCON; DS 3.5.4, p.19 */
#define MCP23_INTCON   0x08 /* 1 = compare with DEFVAL, 0 = with the last
                             * value; DS 3.5.5, p.20 */
#define MCP23_IOCON    0x0A /* also at 0x0B, the same byte; DS Table 3-5 */
#define MCP23_GPPU     0x0C /* 1 = 100 kOhm pull-up; DS 3.5.7, p.22 */
#define MCP23_INTF     0x0E /* which pin fired, read-only; DS 3.5.8, p.22 */
#define MCP23_INTCAP   0x10 /* the port when it fired; DS 3.5.9, p.23 */
#define MCP23_GPIO     0x12 /* the pins; DS 3.5.10, p.23 */
#define MCP23_OLAT     0x14 /* the output latches; DS 3.5.11, p.24 */

/* IOCON's address with IOCON.BANK = 1 (DS Table 3-4, p.17). With BANK = 0
 * the same address is GPINTENB (DS Table 3-5). */
#define MCP23_IOCON_BANK1 0x05

/* ---- IOCON bits: DS Register 3-5, p.21 ---- */
#define MCP23_IOCON_BANK    (1u << 7) /* 1 = ports in separate banks */
#define MCP23_IOCON_MIRROR  (1u << 6) /* INTA and INTB OR'ed together */
#define MCP23_IOCON_SEQOP   (1u << 5) /* 1 = the address pointer stays put */
#define MCP23_IOCON_DISSLW  (1u << 4) /* 1 = SDA slew rate control off */
#define MCP23_IOCON_HAEN    (1u << 3) /* MCP23S17 only: use A2..A0 */
#define MCP23_IOCON_ODR     (1u << 2) /* open-drain INT; overrides INTPOL */
#define MCP23_IOCON_INTPOL  (1u << 1) /* 1 = INT active-high */

/* ---- Power-on values, both ports: IODIR all ones, every other register
 * 0x00 (DS Table 3-5, p.17-18, "POR/RST value"). ---- */
#define MCP23_IODIR_POR 0xFFFFu

/* ---- Addresses ---- */
#define MCP23_PIN_COUNT   16
/* MCP23017: 0b0100 A2 A1 A0 (DS 3.3.1 and Figure 3-4, p.15). */
#define MCP23_I2C_BASE    0x20
/* MCP23S17: the control byte is 0b0100 A2 A1 A0 R/W (DS 3.3.2 and Figure
 * 3-5, p.15); A2..A0 only count with IOCON.HAEN = 1, and read as 000
 * otherwise (DS 3.5.6, p.20). */
static inline uint8_t mcp23_spi_opcode(uint8_t hw_addr, bool read)
{
	return (uint8_t)(0x40 | ((hw_addr & 0x07) << 1) | (read ? 1 : 0));
}

/*
 * The MCP23017's GPA7 and GPB7 -- pins 7 and 15 here -- are output only:
 * "IO7 must be set via I2C interface to '0' (output)" (DS 3.5.1 Note,
 * p.18; DS p.1, Rev D and later). The MCP23S17 has no such limit.
 */
#define MCP23017_OUTPUT_ONLY ((1u << 7) | (1u << 15))

/* SPI clock limit: 10 MHz at 2.7-5.5 V, 5 MHz at 1.8-5.5 V (DS Table 1-4,
 * p.8). */
#define MCP23S17_SPI_MAX_HZ 10000000u
