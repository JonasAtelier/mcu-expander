#include "expander_log.h"

void expander_log_port(const struct expander_dev *expander, uint32_t levels)
{
	expander_info(expander, "%u pins, levels 0x%0*lx",
		      (unsigned)expander->pins, (expander->pins + 3) / 4,
		      (unsigned long)levels);
}
