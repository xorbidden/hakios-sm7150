/*
 * The touch screen of the Mi 9T (davinci, SM7150): a Goodix GT9886 ("Normandy"; the device tree
 * calls it gt9889) on I2C, posting to /dev/hakios-touch (sys/hakios.input.h) as the other
 * touch drivers do. Only with boot-arg touchscreen=1; it takes the place of drivers/input/hakios.goodix.c.
 *
 * Every address and number below belongs to this phone, from Android's own device tree (dtbo,
 * goodix@5d) and Xiaomi's driver for it (drivers/input/touchscreen/goodix_driver_gt9886 of their
 * davinci kernel: goodix_ts_core.c, goodix_ts_i2c.c):
 *   power      GPIO 5 high (the analog supply's enable), PM6150L's LDO 6 (RPMh "ldoc6") at 3.0 V,
 *              300 ms; then the reset (GPIO 8) low 2 ms and high, 100 ms
 *   I2C        the serial engine 7 (qupv3_se7_i2c at 0xa84000, function qup11 on GPIO 6 data and
 *              7 clock, no pull-ups under Android), address 0x5d; done by hand here (as in
 *              drivers/haptic/hakios.haptic.c), 16-bit register addresses, high byte first
 *   doze       before the chip is spoken to outside of a report: 0xaa to 0x30f0, until 0x3100 reads 0xbb
 *   registers  from the configuration Xiaomi's kernel carries built in (firmware/goodix_gt9886_cfg_f10.bin,
 *              both sensor packages alike): product id "9886" at 0x4535, the sensor's id at 0x4541,
 *              the reports at 0x4100 (the driver's own default, 0x824e, is something else on this chip)
 *   sensors    id 0 has both axes the other way round (dtbo: sensor0 goodix,x2x and goodix,y2y), id 1 not
 *   a report   the interrupt line (GPIO 9) low: 12 bytes at 0x4100 -- [0] 0x80 for a touch event,
 *              [1] the number of fingers in its low nibble, from [2] 8 bytes a finger (id, x low,
 *              x high, y low, y high, width, ...), the more than one read after that; the 8-bit
 *              sum of all of it is 0; then 0 written to 0x4100
 * The first 150 reports are logged raw. The chip keeps its firmware and its configuration in its own flash: nothing is sent to it.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern bool hakios_rpmh_ldo(const char *name, uint32_t millivolts);    /* soc/drivers/core/hakios.rpmh.c */

/* in place of the common Goodix driver (drivers/input/hakios.goodix.c): the input hub starts it */
void hakios_goodix_init(void);

/* only GPIOs 5 to 9 of the north tile (0x3900000): the kernel's room for mappings is small */
#define TLMM_PHYS               (0x03900000UL + 0x1000 * PIN_AVDD)
#define TLMM_SIZE               (0x1000 * (PIN_IRQ - PIN_AVDD + 1))
#define GPIO_CFG(n)             (tlmm + 0x1000 * ((n) - PIN_AVDD))
#define GPIO_IO(n)              (tlmm + 0x1000 * ((n) - PIN_AVDD) + 4)
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)

#define PIN_AVDD                5
#define PIN_SDA                 6
#define PIN_SCL                 7
#define PIN_RESET               8
#define PIN_IRQ                 9

#define GT_ADDRESS              0x5d
#define REG_DOZE_CTRL           0x30f0
#define REG_DOZE_STAT           0x3100
#define REG_PID                 0x4535
#define REG_SENSOR_ID           0x4541
#define REG_COORDS              0x4100
#define EVENT_TOUCH             0x80
#define BYTES_PER_COORD         8
#define MAX_FINGERS             10
#define PANEL_MAX_X             1079            /* goodix,panel-max-x 0x437 */
#define PANEL_MAX_Y             2339            /* goodix,panel-max-y 0x923 */


static vm_offset_t tlmm;
static bool flipped;                    /* sensor 0: both axes the other way round */
static unsigned reports_logged;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

/* ---- the touch supply, through RPMh (soc/drivers/core/hakios.rpmh.c) --------------------------------- */


/* the touch controller's supply: PM6150L's LDO 6, as Xiaomi's goodix_ts_power_on() sets it */
static bool
supply_on(void)
{
	return hakios_rpmh_ldo("ldoc6", 3000);
}

/* ---- I2C by hand (as in drivers/haptic/hakios.haptic.c) ----------------------------------------------------------- */

static void
half_bit(void)
{
	spin_us(5);                     /* 100 kHz at most */
}

static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level_of(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, true);
	half_bit();
}

static bool
i2c_bit(bool out)
{
	bool in;

	line(PIN_SDA, out);
	half_bit();
	line(PIN_SCL, true);
	for (unsigned stretched = 0; !level_of(PIN_SCL) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = level_of(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

static bool
i2c_write(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);
	return byte;
}

/* `count' bytes from the 16-bit register address `reg' on */
static bool
regs_read(uint16_t reg, uint8_t *values, unsigned count)
{
	bool answered;

	i2c_start();
	answered = i2c_write(GT_ADDRESS << 1) && i2c_write((uint8_t)(reg >> 8)) && i2c_write((uint8_t)reg);
	if (answered) {
		i2c_start();
		answered = i2c_write(GT_ADDRESS << 1 | 1);
	}
	for (unsigned i = 0; answered && i < count; i++) {
		values[i] = i2c_read(i + 1 < count);
	}
	i2c_stop();
	return answered;
}

static bool
regs_write(uint16_t reg, const uint8_t *values, unsigned count)
{
	bool answered;

	i2c_start();
	answered = i2c_write(GT_ADDRESS << 1) && i2c_write((uint8_t)(reg >> 8)) && i2c_write((uint8_t)reg);
	for (unsigned i = 0; answered && i < count; i++) {
		answered = i2c_write(values[i]);
	}
	i2c_stop();
	return answered;
}

/* goodix_set_i2c_doze_mode(dev, false): keep the chip awake for what follows */
static bool
doze_off(void)
{
	uint8_t value = 0xaa, status = 0;

	regs_write(REG_DOZE_CTRL, &value, 1);
	sleep_us(1000);
	for (int i = 0; i < 10; i++) {
		if (regs_read(REG_DOZE_STAT, &status, 1) && status == 0xbb) {
			return true;
		}
		if (status != 0xaa) {
			regs_write(REG_DOZE_CTRL, &value, 1);
		}
		sleep_us(10000);
	}
	printf("gt9886: the chip does not leave its doze mode (status %#x)\n", status);
	return false;
}

static void
doze_on(void)
{
	uint8_t value = 0xcc;

	regs_write(REG_DOZE_CTRL, &value, 1);
}

/* ---- reports -------------------------------------------------------------------------------------- */

static uint32_t down_mask;              /* fingers (by the chip's id) down after the last report */
static uint32_t last_x[MAX_FINGERS], last_y[MAX_FINGERS];      /* where each was last, scaled */

static uint32_t
scale(uint32_t value, uint32_t max)
{
	return value >= max ? 65535 : value * 65535 / max;
}

static void
report(void)
{
	uint8_t buf[4 + BYTES_PER_COORD * MAX_FINGERS], sum = 0, zero = 0;
	unsigned fingers;
	uint32_t now_down = 0;

	if (!regs_read(REG_COORDS, buf, 4 + BYTES_PER_COORD)) {
		return;
	}
	if ((buf[0] & EVENT_TOUCH) == 0) {
		regs_write(REG_COORDS, &zero, 1);
		return;
	}
	fingers = buf[1] & 0x0f;
	if (fingers > MAX_FINGERS) {
		fingers = 0;
	} else if (fingers > 1 && !regs_read(REG_COORDS + 4 + BYTES_PER_COORD, &buf[4 + BYTES_PER_COORD],
	    (fingers - 1) * BYTES_PER_COORD)) {
		return;
	}
	for (unsigned i = 0; i < fingers * BYTES_PER_COORD + 4; i++) {
		sum += buf[i];
	}
	regs_write(REG_COORDS, &zero, 1);       /* the handshake: the chip may report again */
	if (reports_logged < 150) {
		reports_logged++;
		printf("gt9886: report %u: status %#x, %u finger(s)", reports_logged, buf[0], fingers);
		for (unsigned f = 0; f < fingers; f++) {
			const uint8_t *c = &buf[2 + f * BYTES_PER_COORD];

			printf(", id %u at (%u, %u) w %u", c[0] & 0x0f, c[1] | c[2] << 8, c[3] | c[4] << 8, c[5]);
		}
		printf("%s\n", sum ? " (checksum wrong)" : "");
	}
	if (sum != 0) {
		return;
	}
	for (unsigned f = 0; f < fingers; f++) {
		const uint8_t *c = &buf[2 + f * BYTES_PER_COORD];
		unsigned id = c[0] & 0x0f;
		uint32_t x = c[1] | c[2] << 8, y = c[3] | c[4] << 8;

		/* (nothing that is not a finger on the screen reaches the window server as a touch) */
		if (id >= MAX_FINGERS || x > PANEL_MAX_X || y > PANEL_MAX_Y) {
			continue;
		}
		if (flipped) {
			x = PANEL_MAX_X - x;
			y = PANEL_MAX_Y - y;
		}
		now_down |= 1u << id;
		last_x[id] = scale(x, PANEL_MAX_X);
		last_y[id] = scale(y, PANEL_MAX_Y);
		hakios_touch_post((down_mask & (1u << id)) ? HAKIOS_TOUCH_MOVE : HAKIOS_TOUCH_DOWN, id, last_x[id], last_y[id]);
	}
	for (unsigned id = 0; id < MAX_FINGERS; id++) {
		if ((down_mask & ~now_down) & (1u << id)) {
			/*
			 * where the finger was last (as drivers/input/hakios.goodix.c): the window server takes the lift's position
			 * as the touch's last, and a lift at (0, 0) ended every tap outside its button
			 */
			hakios_touch_post(HAKIOS_TOUCH_UP, id, last_x[id], last_y[id]);
		}
	}
	down_mask = now_down;
}

static void
touch_main(__unused void *arg, __unused wait_result_t result)
{
	for (;;) {
		/* the chip pulls its interrupt line low while it has a report */
		if (!level_of(PIN_IRQ)) {
			report();
		}
		delay_for_interval(4, NSEC_PER_MSEC);
	}
}

void
hakios_goodix_init(void)
{
	uint32_t enabled = 0;
	uint8_t pid[4] = { 0 }, sensor = 0xff;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	if (tlmm == 0) {
		return;
	}
	/* the interrupt line an input (an earlier kernel of ours drove it), the reset held */
	wr(GPIO_CFG(PIN_IRQ), 0);
	wr(GPIO_IO(PIN_RESET), 0);
	wr(GPIO_CFG(PIN_RESET), CFG_OUTPUT);
	/* goodix_ts_power_on(): the analog supply's enable, the supply, 300 ms */
	wr(GPIO_IO(PIN_AVDD), 2);
	wr(GPIO_CFG(PIN_AVDD), CFG_OUTPUT);
	if (!supply_on()) {
		return;
	}
	sleep_us(300000);
	/* goodix_hw_reset(): low 2 ms, high, 100 ms */
	sleep_us(2000);
	wr(GPIO_IO(PIN_RESET), 2);
	sleep_us(100000);
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	printf("gt9886: after reset: SDA %u SCL %u IRQ %u\n", level_of(PIN_SDA), level_of(PIN_SCL), level_of(PIN_IRQ));

	if (!doze_off()) {
		return;
	}
	/* the chip must say what it is before anything it reports is believed */
	if (!regs_read(REG_PID, pid, sizeof(pid)) || memcmp(pid, "9886", 4) != 0) {
		printf("gt9886: product id at %#x: %02x %02x %02x %02x, not \"9886\": not the expected chip, not used\n",
		    REG_PID, pid[0], pid[1], pid[2], pid[3]);
		doze_on();
		return;
	}
	regs_read(REG_SENSOR_ID, &sensor, 1);
	flipped = (sensor & 0x0f) == 0;
	printf("gt9886: product id \"9886\", sensor %u%s\n", sensor & 0x0f, flipped ? " (both axes turned round)" : "");
	{
		uint8_t zero = 0;

		regs_write(REG_COORDS, &zero, 1);
	}
	doze_on();
	if (kernel_thread_start(touch_main, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "gt9886 touch");
		thread_deallocate(thread);
		printf("gt9886: watching the interrupt line (GPIO %u)\n", PIN_IRQ);
	}
}
