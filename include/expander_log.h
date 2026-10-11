/*
 * Kernel dev_err()-style logging for GPIO expander drivers and their users.
 *
 * ESP-IDF: everything goes out under the one ESP-IDF tag "expander", prefixed
 * with the device's name, so one esp_log_level_set("expander", ...) controls
 * every chip and a line still says which chip it came from. There is
 * deliberately no level machinery of our own; expander_dbg() is also subject
 * to CONFIG_LOG_MAXIMUM_LEVEL, and CONFIG_EXPANDER_DEBUG is not used.
 *
 * STM32 HAL: every line goes out through printf() as "expander: <name>: ...",
 * so a line still says which chip it came from. Where printf() ends up is
 * the app's business: with newlib, retarget _write() to a UART. There is
 * deliberately no level machinery; expander_dbg() alone is compiled out
 * unless CONFIG_EXPANDER_DEBUG is 1 (stm-forge: EXPANDER_DEBUG = y).
 */
#ifndef EXPANDER_LOG_H
#define EXPANDER_LOG_H

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#include <stdio.h>
#endif

#include "expander.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#define EXPANDER_LOG_TAG	"expander"

#define expander_err(expander, fmt, ...) \
	ESP_LOGE(EXPANDER_LOG_TAG, "%s: " fmt, (expander)->name, ##__VA_ARGS__)
#define expander_warn(expander, fmt, ...) \
	ESP_LOGW(EXPANDER_LOG_TAG, "%s: " fmt, (expander)->name, ##__VA_ARGS__)
#define expander_info(expander, fmt, ...) \
	ESP_LOGI(EXPANDER_LOG_TAG, "%s: " fmt, (expander)->name, ##__VA_ARGS__)
#define expander_dbg(expander, fmt, ...) \
	ESP_LOGD(EXPANDER_LOG_TAG, "%s: " fmt, (expander)->name, ##__VA_ARGS__)

#else /* STM32 HAL */
#define expander_err(expander, fmt, ...) \
	printf("expander: %s: " fmt "\n", (expander)->name, ##__VA_ARGS__)
#define expander_warn(expander, fmt, ...) \
	printf("expander: %s: " fmt "\n", (expander)->name, ##__VA_ARGS__)
#define expander_info(expander, fmt, ...) \
	printf("expander: %s: " fmt "\n", (expander)->name, ##__VA_ARGS__)

#if CONFIG_EXPANDER_DEBUG
#define expander_dbg(expander, fmt, ...) \
	printf("expander: %s: " fmt "\n", (expander)->name, ##__VA_ARGS__)
#else
/* Still type-checked, so a debug line cannot rot while it is off. */
#define expander_dbg(expander, fmt, ...) do { \
	if (0) \
		printf("expander: %s: " fmt "\n", (expander)->name, \
		       ##__VA_ARGS__); \
} while (0)
#endif
#endif /* ESP_PLATFORM */

/* One info line: the port's levels, pin 0 in bit 0, as many hex digits as
 * the chip has pins. */
void expander_log_port(const struct expander_dev *expander, uint32_t levels);

#ifdef __cplusplus
}
#endif

#endif /* EXPANDER_LOG_H */
