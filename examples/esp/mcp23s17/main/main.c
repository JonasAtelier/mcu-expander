/*
 * Two MCP23S17s on one chip select, strapped 0 and 1: sixteen LEDs on the
 * first, sixteen buttons on the second: set up through the mcp23_* calls,
 * run through the expander_* ones. The INT pin of the second wakes a task
 * instead of polling.
 */
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "expander.h"
#include "expander_log.h"
#include "mcp23.h"

#define PIN_MISO    19
#define PIN_MOSI    23
#define PIN_SCLK    18
#define PIN_CS      5
#define PIN_INT     4       /* INTA of the button chip, open-drain */

static const char *TAG = "mcp23s17-example";
static TaskHandle_t waiter;

static void IRAM_ATTR on_int(void *arg)
{
    BaseType_t woken = pdFALSE;

    expander_irq(arg);
    vTaskNotifyGiveFromISR(waiter, &woken);
    portYIELD_FROM_ISR(woken);
}

void app_main(void)
{
    spi_bus_config_t bus = {
        .miso_io_num     = PIN_MISO,
        .mosi_io_num     = PIN_MOSI,
        .sclk_io_num     = PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
    };
    /* Both on one chip select: every int_* setting must match. */
    mcp23s17_config_t leds_cfg = {
        .host = SPI2_HOST, .cs_gpio = PIN_CS, .addr = 0,
        .int_mirror = true, .int_open_drain = true,
    };
    mcp23s17_config_t keys_cfg = leds_cfg;
    mcp23_handle_t leds_h, keys_h;
    struct expander_dev *leds, *keys;

    keys_cfg.addr = 1;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(mcp23s17_init(&leds_cfg, &leds_h));
    ESP_ERROR_CHECK(mcp23s17_init(&keys_cfg, &keys_h));
    leds = mcp23_expander(leds_h);
    keys = mcp23_expander(keys_h);

    for (uint8_t pin = 0; pin < 16; pin++) {
        ESP_ERROR_CHECK(mcp23_set_mode(leds_h, pin, MCP23_OUTPUT));
        ESP_ERROR_CHECK(mcp23_set_mode(keys_h, pin, MCP23_INPUT_PULLUP));
        ESP_ERROR_CHECK(mcp23_set_interrupt(keys_h, pin, true));
    }

    waiter = xTaskGetCurrentTaskHandle();
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_INT,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     /* INT is open-drain */
        .intr_type    = GPIO_INTR_NEGEDGE,      /* and active-low */
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_INT, on_int, keys));

    while (1) {
        uint32_t fired, captured;

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!expander_irq_pending(keys))
            continue;
        /* Reading clears the interrupt and lets INT go. */
        if (expander_read_interrupt(keys, &fired, &captured) == 0) {
            ESP_LOGI(TAG, "keys 0x%04lx changed", (unsigned long)fired);
            /* Buttons pull to ground: a pressed key lights its LED. */
            expander_write_port(leds, 0xFFFF, ~captured & 0xFFFF);
            expander_log_port(keys, captured);
        }
    }
}
