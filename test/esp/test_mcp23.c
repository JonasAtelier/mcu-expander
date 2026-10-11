/*
 * Host-side check of the mcp23 driver on ESP-IDF: ../mcp23_cases.h is the
 * fake chips and the shared cases; this file feeds them from the
 * i2c_master and spi_master calls, and checks what only ESP-IDF has -- the
 * devices added to the app's buses and removed again, their clocks, and the
 * esp_err_t codes callers of esp-mcp23-expander saw.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#include "expander.h"
#include "expander_priv.h"
#include "mcp23.h"

#define RET(err)	expander_esp_err(err)

static int bus_tag;
#define I2C_BUS		((i2c_master_bus_handle_t)(void *)&bus_tag)

#define PIN_CS		5

static mcp23s17_config_t s17_cfg(uint8_t addr)
{
	return (mcp23s17_config_t){
		.host = SPI2_HOST,
		.cs_gpio = PIN_CS,
		.addr = addr,
	};
}

#include "../mcp23_cases.h"

/* ------------------------------------------------- the IDF side of it --- */

/* The devices added: an I2C one remembers its address. */
struct i2c_master_dev_t {
	bool live;
	uint8_t addr;
	uint32_t scl_hz;
};
static struct i2c_master_dev_t i2c_devs[4];
static int i2c_devices, probes;
static uint16_t probe_addr;

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
			   int timeout_ms)
{
	assert(bus == I2C_BUS && timeout_ms > 0);
	probes++;
	probe_addr = address;
	return i2c_chip((uint8_t)address) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
				    const i2c_device_config_t *config,
				    i2c_master_dev_handle_t *out)
{
	assert(bus == I2C_BUS &&
	       config->dev_addr_length == I2C_ADDR_BIT_LEN_7);
	for (int i = 0; i < 4; i++) {
		if (!i2c_devs[i].live) {
			i2c_devs[i] = (struct i2c_master_dev_t){
				.live = true,
				.addr = (uint8_t)config->device_address,
				.scl_hz = config->scl_speed_hz,
			};
			i2c_devices++;
			*out = &i2c_devs[i];
			return ESP_OK;
		}
	}
	abort();
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{
	assert(dev->live);
	dev->live = false;
	i2c_devices--;
	return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,
			      const uint8_t *data, size_t len, int timeout_ms)
{
	assert(dev->live && timeout_ms > 0);
	if (spi_fails())
		return ESP_ERR_TIMEOUT;
	fake_i2c_write(dev->addr, data, len);
	return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
				      const uint8_t *tx, size_t tx_len,
				      uint8_t *rx, size_t rx_len,
				      int timeout_ms)
{
	assert(dev->live && tx_len == 1 && timeout_ms > 0);
	if (spi_fails())
		return ESP_ERR_TIMEOUT;
	fake_i2c_read(dev->addr, tx[0], rx, rx_len);
	return ESP_OK;
}

/* Two MCP23S17s on one chip select are two devices with the same pin. */
static struct spi_device_t {
	bool live;
	int hz, mode, cs;
} spi_devs[3];
static int spi_devices;
#define spi_dev spi_devs[0]

esp_err_t spi_bus_add_device(spi_host_device_t host,
			     const spi_device_interface_config_t *config,
			     spi_device_handle_t *out)
{
	assert(host == SPI2_HOST);
	for (int i = 0; i < 3; i++) {
		if (!spi_devs[i].live) {
			spi_devs[i] = (struct spi_device_t){
				.live = true,
				.hz = config->clock_speed_hz,
				.mode = config->mode,
				.cs = config->spics_io_num,
			};
			spi_devices++;
			*out = &spi_devs[i];
			return ESP_OK;
		}
	}
	abort();
}

esp_err_t spi_bus_remove_device(spi_device_handle_t dev)
{
	assert(dev->live);
	dev->live = false;
	spi_devices--;
	return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t dev,
				      spi_transaction_t *t)
{
	assert(dev->live && t->length % 8 == 0);
	assert(dev->mode == 0 && dev->hz <= 10000000);
	if (spi_fails())
		return ESP_ERR_TIMEOUT;
	fake_spi(t->tx_buffer, t->rx_buffer, t->length / 8);
	return ESP_OK;
}

/* ----------------------------------------------------------- ESP cases --- */

/* The device at the strapped address and scl_speed_hz (0: 400 kHz), added
 * after the probe and removed at del, or after a failed init. */
static void test_i2c_device(void)
{
	mcp23_config_t cfg = { .addr = 0x23, .scl_speed_hz = 1000000 };
	mcp23_handle_t h;

	fakes_reset();
	probes = 0;
	h = open_i2c(NULL);
	assert(probes == 1 && probe_addr == 0x20 && i2c_devices == 1);
	assert(i2c_devs[0].addr == 0x20 && i2c_devs[0].scl_hz == 400000);
	assert(mcp23_del(h) == ESP_OK && i2c_devices == 0);

	fakes_reset();
	chips[0].addr = 0x23;
	h = open_i2c(&cfg);
	assert(probe_addr == 0x23 && i2c_devs[0].scl_hz == 1000000);
	mcp23_del(h);

	fakes_reset();
	spi_fail_at = 3;
	assert(mcp23_init(I2C_BUS, NULL, &h) == ESP_ERR_TIMEOUT);
	assert(i2c_devices == 0);
}

/* 10 MHz unless asked for less; above 10 MHz refused before adding. */
static void test_spi_device(void)
{
	mcp23s17_config_t cfg = s17_cfg(0);
	mcp23_handle_t h;

	fakes_reset();
	chips[0].present = false;
	chip_por(&chips[1], true, 0);
	assert(mcp23s17_init(&cfg, &h) == ESP_OK);
	assert(spi_dev.hz == 10000000 && spi_dev.mode == 0 &&
	       spi_dev.cs == PIN_CS);
	assert(mcp23_del(h) == ESP_OK && spi_devices == 0);

	cfg.spi_hz = 2000000;
	assert(mcp23s17_init(&cfg, &h) == ESP_OK && spi_dev.hz == 2000000);
	mcp23_del(h);

	cfg.spi_hz = 10000001;
	assert(mcp23s17_init(&cfg, &h) == ESP_ERR_INVALID_ARG);
	assert(spi_devices == 0);
}

/* The codes esp-mcp23-expander's callers saw. */
static void test_esp_errors(void)
{
	mcp23_handle_t h = NULL;

	fakes_reset();
	chips[0].present = false;
	assert(mcp23_init(I2C_BUS, NULL, &h) == ESP_ERR_NOT_FOUND);
	assert(mcp23_init(NULL, NULL, &h) == ESP_ERR_INVALID_ARG);
	fakes_reset();
	chip_dirty(&chips[0]);
	chips[0].stuck = true;
	assert(mcp23_init(I2C_BUS, NULL, &h) == ESP_ERR_NOT_FOUND);
	assert(i2c_devices == 0);
}

int main(void)
{
	run_mcp23_cases();
	assert(i2c_devices == 0 && spi_devices == 0);
	test_i2c_device();
	test_spi_device();
	test_esp_errors();
	printf("all mcp23 checks passed (ESP-IDF)\n");
	return 0;
}
