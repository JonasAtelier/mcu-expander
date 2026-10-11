/* Chase eight LEDs on PORTA while watching a button on PORTB. */
#include <stdio.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mcp23.h"

#define SDA_GPIO    21
#define SCL_GPIO    22
#define BUTTON_PIN  8       /* GPB0, wired to ground through the button */

static const char *TAG = "mcp23-example";

void app_main(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = SDA_GPIO,
        .scl_io_num = SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    mcp23_handle_t io;
    bool was_pressed = false;
    int step = 0;

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    ESP_ERROR_CHECK(mcp23_init(bus, NULL, &io));    /* NULL = address 0x20 */

    for (uint8_t pin = 0; pin < 8; pin++)
        ESP_ERROR_CHECK(mcp23_set_mode(io, pin, MCP23_OUTPUT));

    /* The pull-up means the pin reads high until the button pulls it down. */
    ESP_ERROR_CHECK(mcp23_set_mode(io, BUTTON_PIN, MCP23_INPUT_PULLUP));

    while (1) {
        bool pressed;

        /* One transaction sets all eight LEDs, so they change together. */
        ESP_ERROR_CHECK(mcp23_write_port(io, 1u << step));
        step = (step + 1) % 8;

        if (mcp23_read(io, BUTTON_PIN, &pressed) == ESP_OK) {
            pressed = !pressed;             /* active low */
            if (pressed != was_pressed) {
                ESP_LOGI(TAG, "button %s", pressed ? "down" : "up");
                was_pressed = pressed;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
