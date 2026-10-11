/*
 * The core's cases -- modes, pins and ports through the ops, the pin and
 * mask checks, the optional interrupt ops, the INT hook and the log --
 * shared by esp/test_core.c and stm/test_core.c. Each includes this after
 * providing:
 *
 *   LOG_PREFIX             what the platform's log puts before "expander: "
 *   stub_log_last          the last log line
 *
 * What the chip's ops do lives in its driver, so test_mcp23.c checks that
 * against the real driver.
 */

static int fake_calls;
static int fake_ret;			/* fail the next op with this */
static uint32_t fake_latch, fake_pins, fake_mask, fake_levels;
static unsigned int fake_pin;
static enum expander_mode fake_mode;

static int fake_set_mode(struct expander_dev *e, unsigned int pin,
			 enum expander_mode mode)
{
	(void)e;
	fake_calls++;
	fake_pin = pin;
	fake_mode = mode;
	return fake_ret;
}

static int fake_write(struct expander_dev *e, uint32_t mask, uint32_t levels)
{
	(void)e;
	fake_calls++;
	fake_mask = mask;
	fake_levels = levels;
	if (fake_ret)
		return fake_ret;
	fake_latch = (fake_latch & ~mask) | (levels & mask);
	return 0;
}

static int fake_read(struct expander_dev *e, uint32_t *levels)
{
	(void)e;
	fake_calls++;
	if (fake_ret) {
		*levels = 0xDEAD;	/* the op scribbles; the caller must not see it */
		return fake_ret;
	}
	*levels = fake_pins;
	return 0;
}

static int fake_set_interrupt(struct expander_dev *e, unsigned int pin,
			      bool enable)
{
	(void)e;
	(void)enable;
	fake_calls++;
	fake_pin = pin;
	return fake_ret;
}

static int fake_read_interrupt(struct expander_dev *e, uint32_t *flags,
			       uint32_t *captured)
{
	(void)e;
	fake_calls++;
	*flags = 0x5;
	*captured = 0x7;
	return fake_ret;
}

static const struct expander_ops fake_ops = {
	.set_mode = fake_set_mode,
	.write = fake_write,
	.read = fake_read,
	.set_interrupt = fake_set_interrupt,
	.read_interrupt = fake_read_interrupt,
};

static const struct expander_ops fake_ops_no_irq = {
	.set_mode = fake_set_mode,
	.write = fake_write,
	.read = fake_read,
};

static struct expander_dev fake_dev(void)
{
	struct expander_dev e = {
		.name = "fake",
		.ops = &fake_ops,
		.pins = 8,
	};

	fake_calls = 0;
	fake_ret = 0;
	fake_latch = fake_pins = 0;
	return e;
}

static void test_pins(void)
{
	struct expander_dev e = fake_dev();
	bool level = false;

	assert(expander_set_mode(&e, 7, EXPANDER_INPUT_PULLUP) == 0);
	assert(fake_pin == 7 && fake_mode == EXPANDER_INPUT_PULLUP);
	assert(expander_write(&e, 3, true) == 0);
	assert(fake_mask == 0x08 && fake_levels == 0x08);
	assert(expander_write(&e, 3, false) == 0);
	assert(fake_mask == 0x08 && fake_levels == 0);
	fake_pins = 0x40;
	assert(expander_read(&e, 6, &level) == 0 && level);
	assert(expander_read(&e, 5, &level) == 0 && !level);

	/* Past the chip's pins, or a mode that does not exist: no call. */
	fake_calls = 0;
	assert(expander_set_mode(&e, 8, EXPANDER_OUTPUT) == -EINVAL);
	assert(expander_set_mode(&e, 0, (enum expander_mode)3) == -EINVAL);
	assert(expander_write(&e, 8, true) == -EINVAL);
	assert(expander_read(&e, 8, &level) == -EINVAL);
	assert(expander_read(&e, 0, NULL) == -EINVAL);
	assert(fake_calls == 0);

	/* A failed read leaves *level alone. */
	fake_ret = -EIO;
	level = true;
	assert(expander_read(&e, 0, &level) == -EIO && level);
}

static void test_ports(void)
{
	struct expander_dev e = fake_dev();
	uint32_t v = 9;

	/* Masked: only the pins asked for, and levels outside the mask
	 * never reach the chip. */
	assert(expander_write_port(&e, 0x0F, 0xFF) == 0);
	assert(fake_mask == 0x0F && fake_levels == 0x0F && fake_latch == 0x0F);
	assert(expander_write_port(&e, 0xF0, 0x30) == 0 && fake_latch == 0x3F);
	/* Bits past the chip's pins are refused. */
	assert(expander_write_port(&e, 0x100, 0) == -EINVAL);

	/* Read: only the chip's pins come back. */
	fake_pins = 0xFFFFFF5A;
	assert(expander_read_port(&e, &v) == 0 && v == 0x5A);
	fake_ret = -ETIMEDOUT;
	v = 9;
	assert(expander_read_port(&e, &v) == -ETIMEDOUT && v == 9);

	/* 32 pins: the whole word, no shift past its width. */
	e.pins = 32;
	fake_ret = 0;
	assert(expander_write_port(&e, UINT32_MAX, 1) == 0);
	fake_pins = 0x80000001;
	assert(expander_read_port(&e, &v) == 0 && v == 0x80000001);
}

static void test_interrupts(void)
{
	struct expander_dev e = fake_dev();
	uint32_t f = 0, c = 0;

	assert(expander_set_interrupt(&e, 2, true) == 0 && fake_pin == 2);
	assert(expander_set_interrupt(&e, 8, true) == -EINVAL);
	assert(expander_read_interrupt(&e, &f, &c) == 0 && f == 5 && c == 7);
	assert(expander_read_interrupt(&e, NULL, NULL) == 0);

	/* A failure leaves both alone. */
	fake_ret = -EIO;
	f = c = 1;
	assert(expander_read_interrupt(&e, &f, &c) == -EIO && f == 1 && c == 1);

	/* A chip without interrupts says so. */
	e.ops = &fake_ops_no_irq;
	fake_ret = 0;
	assert(expander_set_interrupt(&e, 0, true) == -EOPNOTSUPP);
	assert(expander_read_interrupt(&e, &f, &c) == -EOPNOTSUPP);
}

/* The INT hook: counted from the ISR side, seen once per batch, never
 * lost between a look and a clear. */
static void test_irq_hook(void)
{
	struct expander_dev e = fake_dev();

	assert(!expander_irq_pending(&e));
	expander_irq(&e);
	expander_irq(&e);
	assert(expander_irq_pending(&e));
	assert(!expander_irq_pending(&e));
	expander_irq(&e);
	assert(expander_irq_pending(&e));

	/* The counter wraps and still reads as pending. */
	e.irq_count = UINT32_MAX;
	e.irq_seen = UINT32_MAX;
	expander_irq(&e);
	assert(e.irq_count == 0 && expander_irq_pending(&e));

	expander_irq(NULL);
	assert(!expander_irq_pending(NULL));
}

static void test_unbound(void)
{
	struct expander_dev e = fake_dev();
	struct expander_ops partial = fake_ops;
	uint32_t v;
	bool b;

	/* Nothing to call is the caller's mistake, and nothing is called. */
	assert(expander_set_mode(NULL, 0, EXPANDER_OUTPUT) == -EINVAL);
	assert(expander_read_port(&e, NULL) == -EINVAL);
	e.ops = NULL;
	assert(expander_write(&e, 0, true) == -EINVAL);
	assert(expander_read(&e, 0, &b) == -EINVAL);
	/* An ops table without one of the three, or no pins, is refused. */
	partial.read = NULL;
	e.ops = &partial;
	assert(expander_write_port(&e, 1, 1) == -EINVAL);
	e.ops = &fake_ops;
	e.pins = 0;
	assert(expander_read_port(&e, &v) == -EINVAL);
	e.pins = 33;
	assert(expander_read_port(&e, &v) == -EINVAL);
	assert(fake_calls == 0);
}

static void test_log(void)
{
	struct expander_dev e = fake_dev();

	expander_err(&e, "no answer (%d)", -6);
	assert(!strcmp(stub_log_last,
		       LOG_PREFIX("E") "expander: fake: no answer (-6)"));

	e.pins = 16;
	expander_log_port(&e, 0x00A5);
	assert(!strcmp(stub_log_last, LOG_PREFIX("I")
		       "expander: fake: 16 pins, levels 0x00a5"));
	e.pins = 8;
	expander_log_port(&e, 0x5);
	assert(!strcmp(stub_log_last, LOG_PREFIX("I")
		       "expander: fake: 8 pins, levels 0x05"));
}

static void run_core_cases(void)
{
	test_pins();
	test_ports();
	test_interrupts();
	test_irq_hook();
	test_unbound();
	test_log();
}
