/*
 * MCP23S17: sixteen outputs on SPI, chased one at a time.
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the
 * SPI bus are the app's -- here hspi1 as CubeMX generates it
 * (MX_SPI1_Init()), mode 0 (CPOL low, CPHA 1 edge), 8-bit frames, software
 * NSS, 10 MHz or less (5 MHz below 2.7 V); CS on PA4, a push-pull output
 * idling high. Needs HAL_SPI_MODULE_ENABLED and EXPANDER_MCP23 = y.
 */
#include CMSIS_device_header

#include "expander.h"
#include "mcp23.h"

extern SPI_HandleTypeDef hspi1;		/* from the app's MX_SPI1_Init() */

void chase_task(void)
{
	const mcp23s17_config_t cfg = {
		.spi     = &hspi1,
		.cs_port = GPIOA,
		.cs_pin  = GPIO_PIN_4,
		.addr    = 0,		/* A2..A0 strapped low */
	};
	struct expander_dev *io;
	mcp23_handle_t h;
	unsigned int step = 0;

	/* -ENODEV: what was written did not read back -- nothing there. */
	if (mcp23s17_init(&cfg, &h))
		return;
	io = mcp23_expander(h);
	for (unsigned int pin = 0; pin < 16; pin++)
		expander_set_mode(io, pin, EXPANDER_OUTPUT);

	for (;;) {
		/* One transaction: all sixteen change together. */
		expander_write_port(io, 0xFFFF, 1u << step);
		step = (step + 1) % 16;
		HAL_Delay(100);
	}
}
