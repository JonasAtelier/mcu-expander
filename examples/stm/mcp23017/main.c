/*
 * MCP23017: eight LEDs on PORTA, eight buttons on PORTB, the buttons
 * reported through the INT line instead of polling.
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the
 * I2C bus are the app's -- here hi2c1 as CubeMX generates it
 * (MX_I2C1_Init()), at up to 400 kHz (1.7 MHz needs a 4.5-5.5 V supply),
 * with pull-ups on SDA and SCL; INTB on PB0, an EXTI input. Needs
 * HAL_I2C_MODULE_ENABLED and EXPANDER_MCP23 = y.
 *
 * GPA7 and GPB7 are output only on the MCP23017: pin 15 stays an output
 * here, so seven buttons, GPB0-GPB6.
 */
#include CMSIS_device_header

#include "expander.h"
#include "expander_log.h"
#include "mcp23.h"

extern I2C_HandleTypeDef hi2c1;		/* from the app's MX_I2C1_Init() */

static struct expander_dev *io;

/* The INT line's EXTI: only a count; the bus is read in the main loop. */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
	if (pin == GPIO_PIN_0)
		expander_irq(io);
}

void buttons_task(void)
{
	mcp23_handle_t h;
	unsigned int pin;

	/* NULL config: address 0x20, INT active-low push-pull. -ENXIO:
	 * nothing answers there. */
	if (mcp23_init(&hi2c1, NULL, &h))
		return;
	io = mcp23_expander(h);

	for (pin = 0; pin < 8; pin++)
		expander_set_mode(io, pin, EXPANDER_OUTPUT);
	for (pin = 8; pin < 15; pin++) {
		expander_set_mode(io, pin, EXPANDER_INPUT_PULLUP);
		expander_set_interrupt(io, pin, true);
	}
	expander_set_mode(io, 15, EXPANDER_OUTPUT);

	for (;;) {
		uint32_t fired, captured;

		if (!expander_irq_pending(io))
			continue;		/* or sleep: __WFI() */
		if (expander_read_interrupt(io, &fired, &captured))
			continue;
		/* A pressed button pulls its pin low: light the LED below it. */
		expander_write_port(io, 0x00FF, ~(captured >> 8) & 0x7F);
		expander_log_port(io, captured);
	}
}
