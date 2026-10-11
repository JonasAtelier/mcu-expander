/*
 * Host-side check of the mcp23 driver on the STM32 HAL: ../mcp23_cases.h is
 * the fake chips and the shared cases; this file feeds them from
 * HAL_I2C_Mem_Write/Read() and from HAL_SPI_Transmit/TransmitReceive()
 * under a software chip select, and checks what only the HAL side has --
 * the SPI handle checks, the chip select framing, the shifted I2C address,
 * and the INT line through an EXTI callback.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "expander.h"
#include "mcp23.h"

#define RET(err)	(err)

static I2C_HandleTypeDef hi2c;
#define I2C_BUS		(&hi2c)

static SPI_HandleTypeDef hspi = {
	.Init = {
		.CLKPolarity = SPI_POLARITY_LOW,
		.CLKPhase = SPI_PHASE_1EDGE,
		.DataSize = SPI_DATASIZE_8BIT,
	},
};
static GPIO_TypeDef cs_port;

static mcp23s17_config_t s17_cfg(uint8_t addr)
{
	return (mcp23s17_config_t){
		.spi = &hspi,
		.cs_port = &cs_port,
		.cs_pin = GPIO_PIN_4,
		.addr = addr,
	};
}

#include "../mcp23_cases.h"

/* ------------------------------------------------- the HAL side of it --- */

static int probes;
static uint16_t probe_addr;

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h, uint16_t addr,
					uint32_t trials, uint32_t timeout)
{
	assert(h == &hi2c && trials >= 1 && timeout > 0);
	probes++;
	probe_addr = addr;
	return i2c_chip((uint8_t)(addr >> 1)) ? HAL_OK : HAL_ERROR;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t addr,
				    uint16_t mem, uint16_t mem_size,
				    uint8_t *data, uint16_t size,
				    uint32_t timeout)
{
	uint8_t b[1 + 32];

	assert(h == &hi2c && mem_size == I2C_MEMADD_SIZE_8BIT &&
	       size < sizeof b && timeout > 0);
	if (spi_fails())
		return HAL_TIMEOUT;
	b[0] = (uint8_t)mem;
	memcpy(b + 1, data, size);
	fake_i2c_write((uint8_t)(addr >> 1), b, 1 + (size_t)size);
	return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t addr,
				   uint16_t mem, uint16_t mem_size,
				   uint8_t *data, uint16_t size,
				   uint32_t timeout)
{
	assert(h == &hi2c && mem_size == I2C_MEMADD_SIZE_8BIT && timeout > 0);
	if (spi_fails())
		return HAL_TIMEOUT;
	fake_i2c_read((uint8_t)(addr >> 1), (uint8_t)mem, data, size);
	return HAL_OK;
}

static bool cs_low;
static int cs_frames;

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
	assert(port == &cs_port && pin == GPIO_PIN_4);
	if (state == GPIO_PIN_RESET)
		cs_frames++;
	cs_low = state == GPIO_PIN_RESET;
}

static HAL_StatusTypeDef spi_xfer(SPI_HandleTypeDef *h, const uint8_t *tx,
				  uint8_t *rx, uint16_t n)
{
	assert(h == &hspi && cs_low);
	if (spi_fails())
		return HAL_TIMEOUT;
	fake_spi(tx, rx, n);
	return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, const uint8_t *data,
				   uint16_t size, uint32_t timeout)
{
	uint8_t in[64];

	assert(size <= sizeof in && timeout > 0);
	return spi_xfer(h, data, in, size);
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h,
					  const uint8_t *tx, uint8_t *rx,
					  uint16_t size, uint32_t timeout)
{
	assert(timeout > 0);
	return spi_xfer(h, tx, rx, size);
}

/* ----------------------------------------------------------- STM cases --- */

/* The 7-bit address goes to the HAL shifted. */
static void test_i2c_address(void)
{
	mcp23_config_t cfg = { .addr = 0x25 };
	mcp23_handle_t h;

	fakes_reset();
	chips[0].addr = 0x25;
	probes = 0;
	h = open_i2c(&cfg);
	assert(probes == 1 && probe_addr == 0x4A);
	mcp23_del(h);
}

/* Every SPI transaction in a chip select of its own, high again after. */
static void test_spi_framing(void)
{
	mcp23s17_config_t cfg = s17_cfg(0);
	mcp23_handle_t h;
	int before;

	fakes_reset();
	chips[0].present = false;
	chip_por(&chips[1], true, 0);
	cs_frames = 0;
	assert(mcp23s17_init(&cfg, &h) == 0);
	assert(cs_frames == xfers && !cs_low);

	/* A failed transfer still lets chip select go. */
	before = cs_frames;
	spi_fail_at = xfers;
	assert(mcp23_write_port(h, 1) == -ETIMEDOUT);
	assert(cs_frames == before + 1 && !cs_low);
	spi_fail_at = -1;
	mcp23_del(h);
}

/* The app's handle must be mode 0 with bytes; anything else is refused
 * before the bus is touched or anything allocated. */
static void test_spi_handle_checks(void)
{
	mcp23s17_config_t cfg = s17_cfg(0);
	mcp23_handle_t h = NULL;

	fakes_reset();
	allocs = 0;
	hspi.Init.CLKPolarity = SPI_POLARITY_HIGH;
	assert(mcp23s17_init(&cfg, &h) == -EINVAL);
	hspi.Init.CLKPolarity = SPI_POLARITY_LOW;
	hspi.Init.CLKPhase = SPI_PHASE_2EDGE;
	assert(mcp23s17_init(&cfg, &h) == -EINVAL);
	hspi.Init.CLKPhase = SPI_PHASE_1EDGE;
	hspi.Init.DataSize = SPI_DATASIZE_16BIT;
	assert(mcp23s17_init(&cfg, &h) == -EINVAL);
	hspi.Init.DataSize = SPI_DATASIZE_8BIT;
	cfg.spi = NULL;
	assert(mcp23s17_init(&cfg, &h) == -EINVAL);
	cfg = s17_cfg(0);
	cfg.cs_port = NULL;
	assert(mcp23s17_init(&cfg, &h) == -EINVAL);
	assert(!h && xfers == 0 && allocs == 0);
}

/*
 * The INT line as an app wires it: the EXTI callback only calls
 * expander_irq(); the main loop sees expander_irq_pending() and reads the
 * interrupt over the bus.
 */
static struct expander_dev *exti_dev;

static void exti_callback(void)
{
	expander_irq(exti_dev);
}

static void test_exti_hook(void)
{
	mcp23_handle_t h;
	uint32_t flags, cap;

	fakes_reset();
	h = open_i2c(NULL);
	exti_dev = mcp23_expander(h);
	assert(expander_set_mode(exti_dev, 4, EXPANDER_INPUT_PULLUP) == 0);
	assert(expander_set_interrupt(exti_dev, 4, true) == 0);
	assert(!expander_irq_pending(exti_dev));

	fake_set_pins(&chips[0], 0x0010);
	assert(int_active(&chips[0], 0));
	exti_callback();
	assert(expander_irq_pending(exti_dev));
	assert(!expander_irq_pending(exti_dev));	/* once per batch */
	assert(expander_read_interrupt(exti_dev, &flags, &cap) == 0);
	assert(flags == 0x0010 && cap == 0x0010 && !int_active(&chips[0], 0));
	mcp23_del(h);
}

int main(void)
{
	run_mcp23_cases();
	test_i2c_address();
	test_spi_framing();
	test_spi_handle_checks();
	test_exti_hook();
	printf("all mcp23 checks passed (STM32 HAL)\n");
	return 0;
}
