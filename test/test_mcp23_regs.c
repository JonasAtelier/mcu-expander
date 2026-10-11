/*
 * Host-side check of src/mcp23/mcp23_regs.h: the addresses, bits and
 * control byte against the data sheet, written out here as literals so a
 * macro cannot agree with itself.
 */
#include <assert.h>
#include <stdio.h>

#include "../src/mcp23/mcp23_regs.h"

int main(void)
{
	/* DS Table 3-5, BANK = 0: each register's PORTA address. */
	assert(MCP23_IODIR == 0x00 && MCP23_IPOL == 0x02);
	assert(MCP23_GPINTEN == 0x04 && MCP23_DEFVAL == 0x06);
	assert(MCP23_INTCON == 0x08 && MCP23_IOCON == 0x0A);
	assert(MCP23_GPPU == 0x0C && MCP23_INTF == 0x0E);
	assert(MCP23_INTCAP == 0x10 && MCP23_GPIO == 0x12);
	assert(MCP23_OLAT == 0x14);
	/* DS Table 3-4, BANK = 1: IOCON. */
	assert(MCP23_IOCON_BANK1 == 0x05);

	/* DS Register 3-5. */
	assert(MCP23_IOCON_BANK == 0x80 && MCP23_IOCON_MIRROR == 0x40);
	assert(MCP23_IOCON_SEQOP == 0x20 && MCP23_IOCON_DISSLW == 0x10);
	assert(MCP23_IOCON_HAEN == 0x08 && MCP23_IOCON_ODR == 0x04);
	assert(MCP23_IOCON_INTPOL == 0x02);

	/* DS Figure 3-5: 0100 A2 A1 A0 R/W. */
	assert(mcp23_spi_opcode(0, false) == 0x40);
	assert(mcp23_spi_opcode(0, true) == 0x41);
	assert(mcp23_spi_opcode(7, false) == 0x4E);
	assert(mcp23_spi_opcode(5, true) == 0x4B);
	assert(mcp23_spi_opcode(8, false) == 0x40);	/* three bits only */
	assert(MCP23_I2C_BASE == 0x20);

	/* GPA7 and GPB7: pins 7 and 15. */
	assert(MCP23017_OUTPUT_ONLY == 0x8080);
	assert(MCP23_IODIR_POR == 0xFFFF && MCP23_PIN_COUNT == 16);
	assert(MCP23S17_SPI_MAX_HZ == 10000000);

	printf("mcp23 registers: all checks passed\n");
	return 0;
}
