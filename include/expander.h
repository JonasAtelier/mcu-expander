/*
 * expander -- a small Linux-gpiochip-style GPIO expander subsystem for
 * ESP-IDF and the STM32 HAL.
 *
 * One struct expander_dev per chip. It holds what every port expander
 * shares: a pin's mode, its level, the whole port at once, and, where the
 * chip has them, interrupts on change. Pins are numbered from 0; port-wide
 * calls use the same numbering, bit n for pin n, in a uint32_t.
 *
 * Each chip keeps its own handle API; the handle holds a struct
 * expander_dev, and <chip>_expander() hands it out. The handle's init fills
 * it and its del ends it: there is no probe or remove of its own.
 *
 * Every int-returning function returns 0 or a negative errno.
 */
#ifndef EXPANDER_H
#define EXPANDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum expander_mode {
	EXPANDER_INPUT,
	EXPANDER_INPUT_PULLUP,	/* the chip's internal pull-up */
	EXPANDER_OUTPUT,
};

struct expander_dev;

struct expander_ops {
	int (*set_mode)(struct expander_dev *expander, unsigned int pin,
			enum expander_mode mode);
	/* the pins in mask take their bit of levels; the others keep theirs */
	int (*write)(struct expander_dev *expander, uint32_t mask,
		     uint32_t levels);
	/* every pin's level as the chip reads it now */
	int (*read)(struct expander_dev *expander, uint32_t *levels);
	/* optional: interrupt when this input changes */
	int (*set_interrupt)(struct expander_dev *expander, unsigned int pin,
			     bool enable);
	/* optional: which pins fired, and the port as it was then; clears it */
	int (*read_interrupt)(struct expander_dev *expander, uint32_t *flags,
			      uint32_t *captured);
};

struct expander_dev {
	const char *name;		/* "mcp23017"; log prefix */
	const struct expander_ops *ops;
	void *priv;			/* the chip's handle */
	uint8_t pins;			/* 1..32 */

	/* expander_irq() and expander_irq_pending(); see below */
	volatile uint32_t irq_count;
	uint32_t irq_seen;
};

int expander_set_mode(struct expander_dev *expander, unsigned int pin,
		      enum expander_mode mode);

/* One pin. A write to a pin that is an input sets its latch for later. */
int expander_write(struct expander_dev *expander, unsigned int pin,
		   bool level);
int expander_read(struct expander_dev *expander, unsigned int pin,
		  bool *level);

/* Every pin at once, in as few bus transactions as the chip allows. Pins
 * outside mask keep their level; bits past the chip's pins are refused. */
int expander_write_port(struct expander_dev *expander, uint32_t mask,
			uint32_t levels);
int expander_read_port(struct expander_dev *expander, uint32_t *levels);

/*
 * Interrupt on change, and what fired: -EOPNOTSUPP on a chip without.
 * flags and captured may each be NULL; reading clears the interrupt and
 * lets the chip's INT line go.
 */
int expander_set_interrupt(struct expander_dev *expander, unsigned int pin,
			   bool enable);
int expander_read_interrupt(struct expander_dev *expander, uint32_t *flags,
			    uint32_t *captured);

/*
 * The INT line, on either platform. Call expander_irq() from the app's GPIO
 * ISR or EXTI callback: it only counts, and never touches the bus. In
 * thread context, expander_irq_pending() is true once for every batch of
 * counts since it last looked; then call expander_read_interrupt() there.
 * One caller of expander_irq_pending() per device.
 *
 * On ESP-IDF expander_irq() is in IRAM and touches only the struct
 * expander_dev, in RAM, so it is safe in an ISR registered with
 * ESP_INTR_FLAG_IRAM, which runs while flash is off; the dev must not be
 * const or in flash.
 */
void expander_irq(struct expander_dev *expander);
bool expander_irq_pending(struct expander_dev *expander);

#ifdef __cplusplus
}
#endif

#endif /* EXPANDER_H */
