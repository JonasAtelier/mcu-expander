#include <errno.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define EXPANDER_IRAM	IRAM_ATTR
#else
#define EXPANDER_IRAM
#endif

#include "expander.h"

static bool expander_bound(const struct expander_dev *expander)
{
	return expander && expander->ops && expander->ops->set_mode &&
	       expander->ops->write && expander->ops->read &&
	       expander->pins >= 1 && expander->pins <= 32;
}

/* The chip's pins, as a mask: all 32 bits for a 32-pin chip. */
static uint32_t expander_all(const struct expander_dev *expander)
{
	return expander->pins >= 32 ? UINT32_MAX :
				      (UINT32_C(1) << expander->pins) - 1;
}

int expander_set_mode(struct expander_dev *expander, unsigned int pin,
		      enum expander_mode mode)
{
	if (!expander_bound(expander) || pin >= expander->pins)
		return -EINVAL;
	if (mode != EXPANDER_INPUT && mode != EXPANDER_INPUT_PULLUP &&
	    mode != EXPANDER_OUTPUT)
		return -EINVAL;

	return expander->ops->set_mode(expander, pin, mode);
}

int expander_write(struct expander_dev *expander, unsigned int pin,
		   bool level)
{
	if (!expander_bound(expander) || pin >= expander->pins)
		return -EINVAL;

	return expander->ops->write(expander, UINT32_C(1) << pin,
				    level ? UINT32_C(1) << pin : 0);
}

int expander_read(struct expander_dev *expander, unsigned int pin,
		  bool *level)
{
	uint32_t levels;
	int ret;

	if (!expander_bound(expander) || pin >= expander->pins || !level)
		return -EINVAL;

	ret = expander->ops->read(expander, &levels);
	if (ret)
		return ret;
	*level = (levels >> pin) & 1;
	return 0;
}

int expander_write_port(struct expander_dev *expander, uint32_t mask,
			uint32_t levels)
{
	if (!expander_bound(expander) || (mask & ~expander_all(expander)))
		return -EINVAL;

	return expander->ops->write(expander, mask, levels & mask);
}

/* Into scratch first, so a failed read leaves *levels alone. */
int expander_read_port(struct expander_dev *expander, uint32_t *levels)
{
	uint32_t v;
	int ret;

	if (!expander_bound(expander) || !levels)
		return -EINVAL;

	ret = expander->ops->read(expander, &v);
	if (ret)
		return ret;
	*levels = v & expander_all(expander);
	return 0;
}

int expander_set_interrupt(struct expander_dev *expander, unsigned int pin,
			   bool enable)
{
	if (!expander_bound(expander) || pin >= expander->pins)
		return -EINVAL;
	if (!expander->ops->set_interrupt)
		return -EOPNOTSUPP;

	return expander->ops->set_interrupt(expander, pin, enable);
}

int expander_read_interrupt(struct expander_dev *expander, uint32_t *flags,
			    uint32_t *captured)
{
	uint32_t f, c;
	int ret;

	if (!expander_bound(expander))
		return -EINVAL;
	if (!expander->ops->read_interrupt)
		return -EOPNOTSUPP;

	ret = expander->ops->read_interrupt(expander, &f, &c);
	if (ret)
		return ret;
	if (flags)
		*flags = f;
	if (captured)
		*captured = c;
	return 0;
}

/* A count, not a flag: a flag cleared by the thread could lose an edge
 * that landed between its look and its clear. In IRAM on ESP-IDF, for an
 * ISR that runs with flash off; it calls nothing. */
EXPANDER_IRAM void expander_irq(struct expander_dev *expander)
{
	if (expander)
		expander->irq_count++;
}

bool expander_irq_pending(struct expander_dev *expander)
{
	uint32_t now;

	if (!expander)
		return false;
	now = expander->irq_count;
	if (now == expander->irq_seen)
		return false;
	expander->irq_seen = now;
	return true;
}
