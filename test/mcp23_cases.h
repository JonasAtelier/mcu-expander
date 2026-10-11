/*
 * Fake MCP23017s and MCP23S17s at register level, and this library's cases
 * against them, shared by esp/test_mcp23.c and stm/test_mcp23.c. The fake
 * keeps each chip's registers by port and models what the driver relies on
 * (DS20001952E): both register maps, IOCON.BANK switching between them
 * (Tables 3-4, 3-5); the address pointer and SEQOP (3.2.1); IOCON at two
 * addresses; GPIO reading pins, OLAT for outputs, inverted by IPOL; writes
 * to GPIO landing in OLAT (3.4); interrupt-on-change against the last value
 * or DEFVAL, INTF and INTCAP latched until GPIO or INTCAP is read, and
 * MIRROR (3.6); the MCP23S17 control byte, and its address pins counting
 * only with HAEN (3.3.2, 3.5.6). Each platform file feeds it from its own
 * bus calls -- spi_fails() first, to fail the transaction a case asks for,
 * then fake_i2c_write/read() or fake_spi() -- and provides:
 *
 *   RET(err)               what the handle API returns for a negative errno
 *   I2C_BUS                the bus handle mcp23_init() takes
 *   s17_cfg(addr)          an MCP23S17 config on that platform's SPI bus
 *
 * esp-mcp23-expander had no test; all of this is new.
 */

#include "mcp23_regs.h"

extern char stub_log_last[256];

/* ------------------------------------------------------- the fake chip --- */

/* Register kinds, in the order of both maps (DS Tables 3-4 and 3-5). */
enum { K_IODIR, K_IPOL, K_GPINTEN, K_DEFVAL, K_INTCON, K_IOCON, K_GPPU,
       K_INTF, K_INTCAP, K_GPIO, K_OLAT, K_COUNT };

struct fake_chip {
	bool present;
	bool spi;
	uint8_t addr;			/* I2C 7-bit, or SPI A2..A0 */
	uint8_t reg[K_COUNT][2];	/* [kind][port]; IOCON in [K_IOCON][0] */
	uint8_t ptr;
	uint16_t pins;			/* what drives the input pins */
	uint16_t last;			/* the compare-with-last reference */
	bool stuck;			/* writes ignored: not an MCP23 */
};

#define CHIPS 3
static struct fake_chip chips[CHIPS];
static int xfers, spi_fail_at = -1;
static bool spi_fail_once;
static int miso_level;			/* SPI with nobody answering: 0 or 0xFF */

static uint8_t iocon_of_chip(const struct fake_chip *c)
{
	return c->reg[K_IOCON][0];
}

static bool bank1(const struct fake_chip *c)
{
	return iocon_of_chip(c) & MCP23_IOCON_BANK;
}

/* An address to kind and port; false past the map. */
static bool decode(const struct fake_chip *c, uint8_t a, int *kind, int *port)
{
	if (bank1(c)) {
		*port = a >> 4;
		*kind = a & 0x0F;
		return *port < 2 && *kind < K_COUNT;
	}
	*kind = a / 2;
	*port = a % 2;
	return *kind < K_COUNT;
}

static uint16_t pair(const struct fake_chip *c, int kind)
{
	return (uint16_t)(c->reg[kind][0] | c->reg[kind][1] << 8);
}

static void set_pair(struct fake_chip *c, int kind, uint16_t v)
{
	c->reg[kind][0] = (uint8_t)v;
	c->reg[kind][1] = (uint8_t)(v >> 8);
}

/* The port as GPIO reads it: outputs show their latch, inputs the pin,
 * inverted where IPOL says (DS 3.5.2). */
static uint16_t port_value(const struct fake_chip *c)
{
	uint16_t dir = pair(c, K_IODIR);

	return (uint16_t)(((c->pins & dir) | (pair(c, K_OLAT) & ~dir)) ^
			  (pair(c, K_IPOL) & dir));
}

/* Interrupt-on-change, per port, while nothing is latched (DS 3.6.4). */
static void evaluate(struct fake_chip *c)
{
	uint16_t now = port_value(c);
	uint16_t en = pair(c, K_GPINTEN) & pair(c, K_IODIR);
	uint16_t ref = (uint16_t)((pair(c, K_INTCON) & pair(c, K_DEFVAL)) |
				  (~pair(c, K_INTCON) & c->last));

	for (int p = 0; p < 2; p++) {
		uint8_t fired = (uint8_t)(((now ^ ref) & en) >> (8 * p));

		if (c->reg[K_INTF][p] || !fired)
			continue;
		c->reg[K_INTF][p] = fired;
		c->reg[K_INTCAP][p] = (uint8_t)(now >> (8 * p));
	}
	for (int p = 0; p < 2; p++)
		if (!c->reg[K_INTF][p])
			c->last = (uint16_t)((c->last & ~(0xFF << (8 * p))) |
					     (now & (0xFF << (8 * p))));
}

/* The INT pins: INTA, INTB as active, before polarity. */
static bool int_active(const struct fake_chip *c, int port)
{
	if (iocon_of_chip(c) & MCP23_IOCON_MIRROR)
		return c->reg[K_INTF][0] || c->reg[K_INTF][1];
	return c->reg[K_INTF][port] != 0;
}

static void fake_set_pins(struct fake_chip *c, uint16_t pins)
{
	c->pins = pins;
	evaluate(c);
}

static void advance(struct fake_chip *c)
{
	if (iocon_of_chip(c) & MCP23_IOCON_SEQOP)
		return;
	c->ptr++;
	/* Past the last register the pointer goes back to 0 (DS 3.2.3.3). */
	if (c->ptr > (bank1(c) ? 0x1A : 0x15))
		c->ptr = 0;
}

static void reg_put(struct fake_chip *c, uint8_t v)
{
	int kind, port;

	if (!c->stuck && decode(c, c->ptr, &kind, &port)) {
		switch (kind) {
		case K_IOCON:
			c->reg[K_IOCON][0] = v & 0xFE;	/* bit 0 reads 0 */
			break;
		case K_INTF:
		case K_INTCAP:
			break;				/* read-only */
		case K_GPIO:
			c->reg[K_OLAT][port] = v;	/* DS 3.4 */
			break;
		default:
			c->reg[kind][port] = v;
		}
	}
	advance(c);
	evaluate(c);
}

static uint8_t reg_get(struct fake_chip *c)
{
	int kind, port;
	uint8_t v = 0;

	if (decode(c, c->ptr, &kind, &port)) {
		if (kind == K_IOCON)
			v = iocon_of_chip(c);
		else if (kind == K_GPIO)
			v = (uint8_t)(port_value(c) >> (8 * port));
		else
			v = c->reg[kind][port];
		/* Reading GPIO or INTCAP clears that port's interrupt, and the
		 * port as it is now becomes what the next change is measured
		 * from (DS 3.6.5). */
		if (kind == K_GPIO || kind == K_INTCAP) {
			uint16_t m = (uint16_t)(0xFF << (8 * port));

			c->reg[K_INTF][port] = 0;
			c->last = (uint16_t)((c->last & ~m) | (port_value(c) & m));
			evaluate(c);
		}
	}
	advance(c);
	return v;
}

/* Power-on: IODIR all ones, the rest 0 (DS Table 3-5). */
static void chip_por(struct fake_chip *c, bool spi, uint8_t addr)
{
	memset(c, 0, sizeof *c);
	c->present = true;
	c->spi = spi;
	c->addr = addr;
	set_pair(c, K_IODIR, 0xFFFF);
}

/* What another firmware may leave across a warm MCU restart. */
static void chip_dirty(struct fake_chip *c)
{
	set_pair(c, K_IPOL, 0xFFFF);
	set_pair(c, K_INTCON, 0x00FF);
	set_pair(c, K_DEFVAL, 0x0F0F);
	set_pair(c, K_GPINTEN, 0x0101);
	set_pair(c, K_GPPU, 0xFFFF);
	set_pair(c, K_OLAT, 0x5555);
	set_pair(c, K_IODIR, 0x1234);
	c->reg[K_IOCON][0] = MCP23_IOCON_BANK | MCP23_IOCON_SEQOP;
}

static void fakes_reset(void)
{
	memset(chips, 0, sizeof chips);
	chip_por(&chips[0], false, 0x20);
	xfers = 0;
	spi_fail_at = -1;
	spi_fail_once = false;
	miso_level = 0;
}

static struct fake_chip *i2c_chip(uint8_t addr)
{
	for (int i = 0; i < CHIPS; i++)
		if (chips[i].present && !chips[i].spi && chips[i].addr == addr)
			return &chips[i];
	return NULL;
}

/* An I2C write: the register address, then data (DS 3.2.2.1). */
static void fake_i2c_write(uint8_t addr, const uint8_t *b, size_t n)
{
	struct fake_chip *c = i2c_chip(addr);

	xfers++;
	assert(c && n >= 1);
	c->ptr = b[0];
	for (size_t i = 1; i < n; i++)
		reg_put(c, b[i]);
}

/* An I2C read after the register address was written (DS 3.2.2.2). */
static void fake_i2c_read(uint8_t addr, uint8_t reg, uint8_t *b, size_t n)
{
	struct fake_chip *c = i2c_chip(addr);

	xfers++;
	assert(c);
	c->ptr = reg;
	for (size_t i = 0; i < n; i++)
		b[i] = reg_get(c);
}

/* An SPI transaction: control byte, register address, data (DS 3.2.3).
 * Every MCP23S17 on the chip select whose address matches takes part. */
static void fake_spi(const uint8_t *out, uint8_t *in, size_t n)
{
	bool answered = false;

	xfers++;
	memset(in, miso_level, n);
	if (n < 2 || (out[0] & 0xF0) != 0x40)
		return;
	for (int i = 0; i < CHIPS; i++) {
		struct fake_chip *c = &chips[i];
		uint8_t hw = (iocon_of_chip(c) & MCP23_IOCON_HAEN) ? c->addr : 0;

		if (!c->present || !c->spi || ((out[0] >> 1) & 7) != hw)
			continue;
		c->ptr = out[1];
		for (size_t k = 2; k < n; k++) {
			if (out[0] & 1) {
				assert(!answered);	/* two drivers on MISO */
				in[k] = reg_get(c);
			} else {
				reg_put(c, out[k]);
			}
		}
		if (out[0] & 1)
			answered = true;
	}
}

/* Whether the platform should fail this transaction instead: every one
 * from spi_fail_at on, or with spi_fail_once that one alone. */
static bool spi_fails(void)
{
	if (spi_fail_at < 0)
		return false;
	if (!spi_fail_once)
		return xfers >= spi_fail_at;
	if (xfers != spi_fail_at)
		return false;
	xfers++;			/* it happened; the next one is fine */
	return true;
}

/* ------------------------------------------------------ allocations ------ */

void *__real_calloc(size_t n, size_t size);
void __real_free(void *p);
void *__wrap_calloc(size_t n, size_t size);
void __wrap_free(void *p);

static int allocs, frees;

void *__wrap_calloc(size_t n, size_t size)
{
	allocs++;
	return __real_calloc(n, size);
}

void __wrap_free(void *p)
{
	if (p)
		frees++;
	__real_free(p);
}

/* ------------------------------------------------------------ helpers ---- */

static mcp23_handle_t open_i2c(const mcp23_config_t *cfg)
{
	mcp23_handle_t h = NULL;

	assert(mcp23_init(I2C_BUS, cfg, &h) == RET(0));
	assert(h);
	return h;
}

/* The chip's state, as the driver should have left it after init. */
static void assert_known_state(const struct fake_chip *c, uint8_t iocon)
{
	assert(iocon_of_chip(c) == iocon);
	assert(pair(c, K_IODIR) == 0xFFFF && pair(c, K_IPOL) == 0);
	assert(pair(c, K_GPINTEN) == 0 && pair(c, K_DEFVAL) == 0);
	assert(pair(c, K_INTCON) == 0 && pair(c, K_GPPU) == 0);
	assert(pair(c, K_OLAT) == 0);
}

/* ------------------------------------------------------------- cases ---- */

static void test_init(void)
{
	mcp23_config_t cfg = { .int_mirror = true, .int_open_drain = true };
	struct expander_dev *e;
	mcp23_handle_t h;

	fakes_reset();
	allocs = frees = 0;
	h = open_i2c(NULL);
	assert_known_state(&chips[0], 0x00);
	e = mcp23_expander(h);
	assert(e && !strcmp(e->name, "mcp23017") && e->ops && e->priv == h);
	assert(e->pins == 16);
	assert(mcp23_expander(NULL) == NULL);
	assert(mcp23_del(h) == RET(0) && allocs == frees);

	/* IOCON from the config: MIRROR and ODR (DS Register 3-5). */
	fakes_reset();
	h = open_i2c(&cfg);
	assert_known_state(&chips[0], 0x44);
	mcp23_del(h);

	/* The last address there is. */
	fakes_reset();
	chips[0].addr = 0x27;
	cfg = (mcp23_config_t){ .addr = 0x27, .int_active_high = true };
	h = open_i2c(&cfg);
	assert_known_state(&chips[0], 0x02);
	mcp23_del(h);
}

/*
 * A warm MCU restart finds the chip as the last firmware left it. Upstream
 * wrote IOCON at 0x0A assuming BANK = 0 -- with BANK = 1 that is OLATA --
 * and never cleared IPOL, DEFVAL or INTCON. Now every one is back.
 */
static void test_init_warm(void)
{
	mcp23_handle_t h;

	fakes_reset();
	chip_dirty(&chips[0]);
	h = open_i2c(NULL);
	assert(!bank1(&chips[0]));
	assert_known_state(&chips[0], 0x00);
	mcp23_del(h);

	/* BANK = 0 but the rest dirty: inputs read true again. */
	fakes_reset();
	chip_dirty(&chips[0]);
	chips[0].reg[K_IOCON][0] = 0;
	h = open_i2c(NULL);
	assert_known_state(&chips[0], 0x00);
	fake_set_pins(&chips[0], 0x0001);
	{
		bool level;

		assert(mcp23_read(h, 0, &level) == RET(0) && level);
	}
	mcp23_del(h);
}

static void test_init_failures(void)
{
	mcp23_config_t cfg = { .addr = 0x21 };
	mcp23_handle_t h = NULL;
	int good;

	/* Nothing at the address: refused before anything is allocated. */
	fakes_reset();
	allocs = 0;
	assert(mcp23_init(I2C_BUS, &cfg, &h) == RET(-ENXIO) && !h);
	assert(allocs == 0 && xfers == 0);

	/* Bad arguments, before the bus. */
	cfg.addr = 0x28;
	assert(mcp23_init(I2C_BUS, &cfg, &h) == RET(-EINVAL));
	cfg.addr = 0x1F;
	assert(mcp23_init(I2C_BUS, &cfg, &h) == RET(-EINVAL));
	assert(mcp23_init(I2C_BUS, NULL, NULL) == RET(-EINVAL));
	assert(allocs == 0 && xfers == 0);

	/* Something answers, but its registers do not take what was written. */
	fakes_reset();
	chip_dirty(&chips[0]);
	chips[0].reg[K_IOCON][0] = 0;
	chips[0].stuck = true;
	allocs = frees = 0;
	assert(mcp23_init(I2C_BUS, NULL, &h) == RET(-ENODEV) && !h);
	assert(allocs == frees);
	assert(strstr(stub_log_last, "mcp23017: register 0x00 reads"));

	/* Every transaction of init failing in turn: from there on, or that
	 * one alone. Each failure ends init with its errno, nothing leaked. */
	fakes_reset();
	h = open_i2c(NULL);
	good = xfers;
	mcp23_del(h);
	/* BANK, IOCON, seven pairs, the read-back. */
	assert(good == 10);
	for (int once = 0; once < 2; once++) {
		for (int k = 0; k < good; k++) {
			fakes_reset();
			spi_fail_at = k;
			spi_fail_once = once;
			allocs = frees = 0;
			h = NULL;
			assert(mcp23_init(I2C_BUS, NULL, &h) == RET(-ETIMEDOUT));
			assert(!h && allocs == frees);
		}
	}
}

static void test_pins(void)
{
	mcp23_handle_t h;
	struct fake_chip *c;
	bool level;

	fakes_reset();
	c = &chips[0];
	h = open_i2c(NULL);

	/* An output: pull-up off first, then the direction. */
	assert(mcp23_set_mode(h, 0, MCP23_OUTPUT) == RET(0));
	assert(pair(c, K_IODIR) == 0xFFFE);
	assert(mcp23_write(h, 0, true) == RET(0) && pair(c, K_OLAT) == 1);
	assert(mcp23_read(h, 0, &level) == RET(0) && level);
	assert(mcp23_write(h, 0, false) == RET(0) && pair(c, K_OLAT) == 0);

	/* An input with its pull-up, on PORTB. */
	assert(mcp23_set_mode(h, 8, MCP23_INPUT_PULLUP) == RET(0));
	assert(pair(c, K_GPPU) == 0x0100 && pair(c, K_IODIR) == 0xFFFE);
	fake_set_pins(c, 0x0100);
	assert(mcp23_read(h, 8, &level) == RET(0) && level);
	fake_set_pins(c, 0);
	assert(mcp23_read(h, 8, &level) == RET(0) && !level);
	assert(mcp23_set_mode(h, 8, MCP23_INPUT) == RET(0));
	assert(pair(c, K_GPPU) == 0);

	/* GPA7 and GPB7 are output only on the MCP23017 (DS 3.5.1 Note):
	 * inputs and interrupts refused, outputs fine, nothing written. */
	xfers = 0;
	assert(mcp23_set_mode(h, 7, MCP23_INPUT) == RET(-EINVAL));
	assert(mcp23_set_mode(h, 15, MCP23_INPUT_PULLUP) == RET(-EINVAL));
	assert(mcp23_set_interrupt(h, 7, true) == RET(-EINVAL));
	assert(mcp23_set_interrupt(h, 15, true) == RET(-EINVAL));
	assert(expander_set_mode(mcp23_expander(h), 7, EXPANDER_INPUT) ==
	       -EINVAL);
	assert(xfers == 0);
	assert(mcp23_set_interrupt(h, 15, false) == RET(0));
	assert(mcp23_set_mode(h, 7, MCP23_OUTPUT) == RET(0));
	assert(mcp23_set_mode(h, 15, MCP23_OUTPUT) == RET(0));
	assert(pair(c, K_IODIR) == 0x7F7E);

	/* Out of range, and modes that do not exist. */
	assert(mcp23_set_mode(h, 16, MCP23_OUTPUT) == RET(-EINVAL));
	assert(mcp23_set_mode(h, 1, (mcp23_mode_t)3) == RET(-EINVAL));
	assert(mcp23_write(h, 16, true) == RET(-EINVAL));
	assert(mcp23_read(h, 16, &level) == RET(-EINVAL));

	/* A failed write leaves the shadow as the chip has it, so the retry
	 * writes. */
	spi_fail_at = xfers;
	spi_fail_once = true;
	assert(mcp23_write(h, 2, true) == RET(-ETIMEDOUT));
	assert(pair(c, K_OLAT) == 0);
	assert(mcp23_write(h, 2, true) == RET(0) && pair(c, K_OLAT) == 4);
	spi_fail_at = -1;
	spi_fail_once = false;
	mcp23_del(h);
}

static void test_ports(void)
{
	struct expander_dev *e;
	mcp23_handle_t h;
	struct fake_chip *c;
	uint16_t v = 0;
	uint32_t w = 7;
	int before;

	fakes_reset();
	c = &chips[0];
	h = open_i2c(NULL);
	e = mcp23_expander(h);
	for (uint8_t pin = 0; pin < 8; pin++)
		assert(mcp23_set_mode(h, pin, MCP23_OUTPUT) == RET(0));

	/* All sixteen in one transaction. */
	before = xfers;
	assert(mcp23_write_port(h, 0xA55A) == RET(0));
	assert(xfers == before + 1 && pair(c, K_OLAT) == 0xA55A);
	fake_set_pins(c, 0x1200);
	assert(mcp23_read_port(h, &v) == RET(0) && v == 0x125A);

	/* Through the category: a masked write keeps the rest. */
	assert(expander_write_port(e, 0x00FF, 0x0001) == 0);
	assert(pair(c, K_OLAT) == 0xA501);
	assert(expander_write(e, 1, true) == 0 && pair(c, K_OLAT) == 0xA503);
	before = xfers;
	assert(expander_write(e, 1, true) == 0 && xfers == before);
	assert(expander_read_port(e, &w) == 0 && w == 0x1203);
	assert(expander_write_port(e, 0x10000, 0) == -EINVAL);

	/* A failed read leaves the caller's value alone. */
	spi_fail_at = xfers;
	v = 99;
	assert(mcp23_read_port(h, &v) == RET(-ETIMEDOUT) && v == 99);
	w = 99;
	assert(expander_read_port(e, &w) == -ETIMEDOUT && w == 99);
	spi_fail_at = -1;
	mcp23_del(h);
}

static void test_interrupts(void)
{
	mcp23_config_t cfg = { .int_mirror = true };
	struct expander_dev *e;
	mcp23_handle_t h;
	struct fake_chip *c;
	uint16_t flags, cap;
	uint32_t f32, c32;
	bool level;

	fakes_reset();
	c = &chips[0];
	h = open_i2c(NULL);
	e = mcp23_expander(h);
	assert(mcp23_set_interrupt(h, 3, true) == RET(0));
	assert(mcp23_set_interrupt(h, 9, true) == RET(0));
	assert(pair(c, K_GPINTEN) == 0x0208);

	/* Pin 3 changes: INTA, the port captured. */
	fake_set_pins(c, 0x0008);
	assert(int_active(c, 0) && !int_active(c, 1));
	fake_set_pins(c, 0x0000);		/* gone again: still latched */
	assert(mcp23_read_interrupt(h, &flags, &cap) == RET(0));
	assert(flags == 0x0008 && cap == 0x0008);
	assert(!int_active(c, 0));

	/* Through the category, and with NULLs: INTCAP read all the same. */
	fake_set_pins(c, 0x0200);
	assert(int_active(c, 1));
	assert(expander_read_interrupt(e, &f32, &c32) == 0);
	/* INTCAPA still holds its last capture: INTCAP changes only when an
	 * interrupt occurs (DS 3.5.9). */
	assert(f32 == 0x0200 && c32 == 0x0208 && !int_active(c, 1));
	fake_set_pins(c, 0x0000);
	assert(int_active(c, 1));
	assert(mcp23_read_interrupt(h, NULL, NULL) == RET(0));
	assert(!int_active(c, 1));

	/* Reading GPIO clears it too (DS 3.6.4). */
	fake_set_pins(c, 0x0008);
	assert(mcp23_read(h, 3, &level) == RET(0) && level);
	assert(!int_active(c, 0));
	mcp23_del(h);

	/* MIRROR: either port drives both pins. */
	fakes_reset();
	c = &chips[0];
	h = open_i2c(&cfg);
	assert(mcp23_set_interrupt(h, 9, true) == RET(0));
	fake_set_pins(c, 0x0200);
	assert(int_active(c, 0) && int_active(c, 1));
	mcp23_del(h);
}

static void test_spi_part(void)
{
	mcp23s17_config_t cfg = s17_cfg(0);
	struct fake_chip *c = &chips[1];
	mcp23_handle_t h = NULL;
	bool level;

	/* HAEN always set on the SPI part (DS 3.5.6). */
	fakes_reset();
	chips[0].present = false;
	chip_por(c, true, 0);
	allocs = frees = 0;
	assert(mcp23s17_init(&cfg, &h) == RET(0));
	assert(!strcmp(mcp23_expander(h)->name, "mcp23s17"));
	assert_known_state(c, MCP23_IOCON_HAEN);

	/* No output-only pins on the SPI part. */
	assert(mcp23_set_mode(h, 7, MCP23_INPUT) == RET(0));
	assert(mcp23_set_interrupt(h, 15, true) == RET(0));
	fake_set_pins(c, 0x0080);
	assert(mcp23_read(h, 7, &level) == RET(0) && level);
	assert(mcp23_del(h) == RET(0) && allocs == frees);

	/* Nothing on the bus: MISO floating either way is no chip. */
	for (int m = 0; m < 2; m++) {
		fakes_reset();
		chips[0].present = false;
		miso_level = m ? 0xFF : 0x00;
		allocs = frees = 0;
		h = NULL;
		assert(mcp23s17_init(&cfg, &h) == RET(-ENODEV) && !h);
		assert(allocs == frees);
	}

	cfg.addr = 8;
	assert(mcp23s17_init(&cfg, &h) == RET(-EINVAL));
	assert(mcp23s17_init(NULL, &h) == RET(-EINVAL));
}

/*
 * Two MCP23S17s on one chip select, strapped 0 and 5. Until HAEN is set
 * both answer to address 0 (DS 3.5.6): without the first IOCON write
 * through address 0, the one at 5 never answers at 5, and the one at 0
 * shares MISO with it. Either order of init works.
 */
static void test_spi_shared_cs(void)
{
	mcp23s17_config_t c0 = s17_cfg(0), c5 = s17_cfg(5);
	mcp23_handle_t h0, h5;

	fakes_reset();
	chips[0].present = false;
	chip_por(&chips[1], true, 0);
	chip_por(&chips[2], true, 5);
	assert(mcp23s17_init(&c0, &h0) == RET(0));
	assert(mcp23s17_init(&c5, &h5) == RET(0));
	assert(iocon_of_chip(&chips[1]) == MCP23_IOCON_HAEN);
	assert(iocon_of_chip(&chips[2]) == MCP23_IOCON_HAEN);

	assert(mcp23_set_mode(h5, 4, MCP23_OUTPUT) == RET(0));
	assert(mcp23_write_port(h5, 0x0010) == RET(0));
	assert(mcp23_write_port(h0, 0x8000) == RET(0));
	assert(pair(&chips[2], K_OLAT) == 0x0010 &&
	       pair(&chips[2], K_IODIR) == 0xFFEF);
	assert(pair(&chips[1], K_OLAT) == 0x8000 &&
	       pair(&chips[1], K_IODIR) == 0xFFFF);
	mcp23_del(h0);
	mcp23_del(h5);

	/* The other order. */
	fakes_reset();
	chips[0].present = false;
	chip_por(&chips[1], true, 0);
	chip_por(&chips[2], true, 5);
	assert(mcp23s17_init(&c5, &h5) == RET(0));
	assert(mcp23s17_init(&c0, &h0) == RET(0));
	assert(mcp23_write_port(h5, 0x0001) == RET(0));
	assert(pair(&chips[2], K_OLAT) == 0x0001 && pair(&chips[1], K_OLAT) == 0);
	mcp23_del(h0);
	mcp23_del(h5);
}

static void test_null_arguments(void)
{
	uint16_t v;
	bool b;

	assert(mcp23_set_mode(NULL, 0, MCP23_OUTPUT) == RET(-EINVAL));
	assert(mcp23_write(NULL, 0, true) == RET(-EINVAL));
	assert(mcp23_read(NULL, 0, &b) == RET(-EINVAL));
	assert(mcp23_write_port(NULL, 0) == RET(-EINVAL));
	assert(mcp23_read_port(NULL, &v) == RET(-EINVAL));
	assert(mcp23_set_interrupt(NULL, 0, true) == RET(-EINVAL));
	assert(mcp23_read_interrupt(NULL, &v, &v) == RET(-EINVAL));
	assert(mcp23_del(NULL) == RET(-EINVAL));
}

static void run_mcp23_cases(void)
{
	test_init();
	test_init_warm();
	test_init_failures();
	test_pins();
	test_ports();
	test_interrupts();
	test_spi_part();
	test_spi_shared_cs();
	test_null_arguments();
}
