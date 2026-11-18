/*
 * expander_priv.h on the STM32 HAL. stm-forge compiles every .c in src/, so
 * under ESP-IDF this file is empty; the ESP-IDF CMakeLists never lists it.
 */
#ifndef ESP_PLATFORM

#include <string.h>

#include "expander.h"
#include "expander_priv.h"

/*
 * The core is always built; each chip driver only under its own option. With
 * the library built and no chip picked, say so: this file is compiled only
 * when the library is, so this is the CMakeLists' "no chip" warning for every
 * build that never reads that file (stm-forge, a hand-made project).
 */
#if !(CONFIG_EXPANDER_MCP23)
#warning "expander: no chip driver is selected; define CONFIG_EXPANDER_<CHIP>=1 (stm-forge: EXPANDER_<CHIP> = y)"
#endif

#if defined(HAL_I2C_MODULE_ENABLED) || defined(HAL_SPI_MODULE_ENABLED) || \
    defined(HAL_UART_MODULE_ENABLED)
/* Nothing to undo: the handle is the app's, and nothing was added to it. */
static void expander_hal_detach(struct expander_bus *bus)
{
	(void)bus;
}
#endif

#ifdef HAL_I2C_MODULE_ENABLED
/*
 * HAL_I2C_IsDeviceReady() returns HAL_ERROR when no trial was ACKed, which is
 * -ENXIO. A busy or stuck bus keeps its own errno. On the F4, no pull-ups or
 * SDA held low sets the peripheral's BUSY flag, so the HAL gives up after
 * 25 ms with HAL_BUSY, -EAGAIN; HAL_TIMEOUT, -ETIMEDOUT, means a START never
 * completed.
 */
int expander_i2c_probe(expander_i2c_bus handle, uint8_t address,
		       uint32_t timeout_ms)
{
	HAL_StatusTypeDef status;

	status = HAL_I2C_IsDeviceReady(handle, (uint16_t)(address << 1), 1,
				       timeout_ms);
	return status == HAL_ERROR ? -ENXIO : expander_hal_errno(status);
}

/* HAL_I2C_Mem_Read(): cmd, repeated start, burst. Mem_Write(): cmd, data. */
static int expander_i2c_transfer(struct expander_bus *bus, uint8_t cmd,
				 const uint8_t *tx, size_t tx_len, uint8_t *rx,
				 size_t rx_len)
{
	uint16_t address = (uint16_t)(bus->i2c.address << 1);

	if ((tx_len && rx_len) || tx_len > EXPANDER_BUS_MAX_LEN ||
	    rx_len > UINT16_MAX)
		return -EINVAL;
	if (rx_len)
		return expander_hal_errno(HAL_I2C_Mem_Read(bus->i2c.handle,
							   address, cmd,
							   I2C_MEMADD_SIZE_8BIT,
							   rx, (uint16_t)rx_len,
							   bus->timeout_ms));
	if (tx_len)
		return expander_hal_errno(HAL_I2C_Mem_Write(bus->i2c.handle,
							    address, cmd,
							    I2C_MEMADD_SIZE_8BIT,
							    (uint8_t *)tx,
							    (uint16_t)tx_len,
							    bus->timeout_ms));
	/* A bare command, for a part with no registers: its own START, STOP. */
	return expander_hal_errno(HAL_I2C_Master_Transmit(bus->i2c.handle,
							  address, &cmd, 1,
							  bus->timeout_ms));
}

static const struct expander_bus_ops expander_i2c_ops = {
	.kind = EXPANDER_BUS_I2C,
	.transfer = expander_i2c_transfer,
	.detach = expander_hal_detach,
};

int expander_i2c_attach(struct expander_bus *bus, expander_i2c_bus handle,
			uint8_t address, uint32_t scl_hz, uint32_t timeout_ms)
{
	(void)scl_hz;
	bus->ops = &expander_i2c_ops;
	bus->timeout_ms = timeout_ms;
	bus->i2c.handle = handle;
	bus->i2c.address = address;
	return 0;
}
#endif /* HAL_I2C_MODULE_ENABLED */

#ifdef HAL_SPI_MODULE_ENABLED
/*
 * One transaction under a software chip select: CS low, cmd and tx out --
 * full duplex with zeros after them when there is something to read -- CS
 * high. CS goes high even when the transfer failed; see expander_bus_ops.
 */
static int expander_spi_transfer(struct expander_bus *bus, uint8_t cmd,
				 const uint8_t *tx, size_t tx_len, uint8_t *rx,
				 size_t rx_len)
{
	uint8_t out[1 + EXPANDER_BUS_MAX_LEN] = { 0 };
	uint8_t in[1 + EXPANDER_BUS_MAX_LEN];
	uint16_t n = (uint16_t)(1 + tx_len + rx_len);
	HAL_StatusTypeDef status;

	if (tx_len + rx_len > EXPANDER_BUS_MAX_LEN)
		return -EINVAL;
	out[0] = cmd;
	if (tx_len)
		memcpy(out + 1, tx, tx_len);

	HAL_GPIO_WritePin(bus->spi.cs_port, bus->spi.cs_pin, GPIO_PIN_RESET);
	if (rx_len)
		status = HAL_SPI_TransmitReceive(bus->spi.handle, out, in, n,
						 bus->timeout_ms);
	else
		status = HAL_SPI_Transmit(bus->spi.handle, out, n,
					  bus->timeout_ms);
	HAL_GPIO_WritePin(bus->spi.cs_port, bus->spi.cs_pin, GPIO_PIN_SET);
	if (status == HAL_OK && rx_len)
		memcpy(rx, in + 1 + tx_len, rx_len);
	return expander_hal_errno(status);
}

/* One frame, full duplex: rx[i] is what came back while tx[i] went out. */
static int expander_spi_exchange(struct expander_bus *bus, const uint8_t *tx,
				 uint8_t *rx, size_t len)
{
	uint8_t out[1 + EXPANDER_BUS_MAX_LEN];
	uint8_t in[1 + EXPANDER_BUS_MAX_LEN];
	HAL_StatusTypeDef status;

	if (!len || len > sizeof(out))
		return -EINVAL;
	memcpy(out, tx, len);

	HAL_GPIO_WritePin(bus->spi.cs_port, bus->spi.cs_pin, GPIO_PIN_RESET);
	status = HAL_SPI_TransmitReceive(bus->spi.handle, out, in,
					 (uint16_t)len, bus->timeout_ms);
	HAL_GPIO_WritePin(bus->spi.cs_port, bus->spi.cs_pin, GPIO_PIN_SET);
	if (status == HAL_OK)
		memcpy(rx, in, len);
	return expander_hal_errno(status);
}

static const struct expander_bus_ops expander_spi_ops = {
	.kind = EXPANDER_BUS_SPI,
	.transfer = expander_spi_transfer,
	.exchange = expander_spi_exchange,
	.detach = expander_hal_detach,
};

int expander_spi_attach(struct expander_bus *bus, SPI_HandleTypeDef *handle,
			GPIO_TypeDef *cs_port, uint16_t cs_pin,
			uint32_t timeout_ms)
{
	bus->ops = &expander_spi_ops;
	bus->timeout_ms = timeout_ms;
	bus->spi.handle = handle;
	bus->spi.cs_port = cs_port;
	bus->spi.cs_pin = cs_pin;
	/* Deselected first, so the first transfer starts on a falling edge. */
	HAL_GPIO_WritePin(cs_port, cs_pin, GPIO_PIN_SET);
	return 0;
}
#endif /* HAL_SPI_MODULE_ENABLED */

#ifdef HAL_UART_MODULE_ENABLED
/*
 * Blocking HAL_UART_Receive() hands back whatever already sits in the data
 * register, so a late byte from an earlier exchange would be read as the
 * start of this reply. Drop it first: clear an overrun, then read the data
 * register empty (on the F7 that macro is a flush request). Bounded by the
 * timeout, in case the line never goes quiet.
 */
static void expander_uart_drop_input(struct expander_bus *bus)
{
	UART_HandleTypeDef *handle = bus->uart;
	uint32_t start = HAL_GetTick();

	__HAL_UART_CLEAR_OREFLAG(handle);
	while (__HAL_UART_GET_FLAG(handle, UART_FLAG_RXNE) &&
	       (uint32_t)(HAL_GetTick() - start) <= bus->timeout_ms)
		__HAL_UART_FLUSH_DRREGISTER(handle);
}

static int expander_uart_transfer(struct expander_bus *bus, uint8_t cmd,
				  const uint8_t *tx, size_t tx_len,
				  uint8_t *rx, size_t rx_len)
{
	uint8_t out[1 + EXPANDER_BUS_MAX_LEN];
	int ret;

	if (tx_len > EXPANDER_BUS_MAX_LEN || rx_len > UINT16_MAX)
		return -EINVAL;
	out[0] = cmd;
	if (tx_len)
		memcpy(out + 1, tx, tx_len);

	expander_uart_drop_input(bus);
	ret = expander_hal_errno(HAL_UART_Transmit(bus->uart, out,
						   (uint16_t)(1 + tx_len),
						   bus->timeout_ms));
	if (ret || !rx_len)
		return ret;
	/* A reply that stops short runs into the timeout: HAL_TIMEOUT. */
	return expander_hal_errno(HAL_UART_Receive(bus->uart, rx,
						   (uint16_t)rx_len,
						   bus->timeout_ms));
}

/*
 * The echo, a byte at a time. Blocking HAL_UART_Transmit() of a whole
 * datagram never reads the line while it sends, and the data register holds
 * one byte: on a single wire the second echoed byte overruns the first
 * (ORE), the rest are lost, and the reply that follows comes up misaligned.
 * So send one byte and take its echo before the next goes out; only one
 * byte is ever in flight, and nothing overruns.
 *
 * The other way, a half-duplex USART (HDSEL) that leaves the receiver deaf
 * while it sends, needs the app to set the handle up so and the driver to
 * switch direction around each datagram -- a different contract with the
 * app, and nothing this library can check from a handle.
 */
static int expander_uart_exchange(struct expander_bus *bus, const uint8_t *tx,
				  uint8_t *rx, size_t len)
{
	size_t i;
	int ret;

	if (!len || len > 1 + EXPANDER_BUS_MAX_LEN)
		return -EINVAL;

	expander_uart_drop_input(bus);
	for (i = 0; i < len; i++) {
		uint8_t byte = tx[i];

		ret = expander_hal_errno(HAL_UART_Transmit(bus->uart, &byte, 1,
							   bus->timeout_ms));
		if (ret)
			return ret;
		ret = expander_hal_errno(HAL_UART_Receive(bus->uart, &rx[i], 1,
							  bus->timeout_ms));
		if (ret)
			return ret;
	}
	return 0;
}

static int expander_uart_receive(struct expander_bus *bus, uint8_t *rx,
				 size_t rx_len)
{
	if (rx_len > UINT16_MAX)
		return -EINVAL;
	return expander_hal_errno(HAL_UART_Receive(bus->uart, rx,
						   (uint16_t)rx_len,
						   bus->timeout_ms));
}

static const struct expander_bus_ops expander_uart_ops = {
	.kind = EXPANDER_BUS_UART,
	.transfer = expander_uart_transfer,
	.exchange = expander_uart_exchange,
	.receive = expander_uart_receive,
	.detach = expander_hal_detach,
};

int expander_uart_attach(struct expander_bus *bus, UART_HandleTypeDef *handle,
			 uint32_t timeout_ms)
{
	bus->ops = &expander_uart_ops;
	bus->timeout_ms = timeout_ms;
	bus->uart = handle;
	return 0;
}
#endif /* HAL_UART_MODULE_ENABLED */

#endif /* !ESP_PLATFORM */
