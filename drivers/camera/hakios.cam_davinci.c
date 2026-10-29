/*
 * In place of drivers/camera/hakios.camera.c (the SM7325 phone's cameras) for the Mi 9T, behind the same calls
 * (hakios_camera_init, hakios_camera_quiesce) and the same /dev/hakios-camera, with boot-arg cam=N (its levels below).
 */
/*
 * The cameras of the Mi 9T (davinci, SM7150): the first steps, the sensors spoken to. Every address
 * and number below belongs to this phone, from Android's own device trees (dtbo 14, qcom,cam-sensor@0
 * to @3, and the main dtb's regulators), the sensors' module files of its camera stack
 * (/vendor/lib64/camera/com.qti.sensormodule.davinci_*.bin, "Parameter Parser V1.3.1": the slave
 * address, the chip ID and where it is read, the power sequence) and the sm6150 kernel's drivers
 * (camcc-sdmmagpie.c, pinctrl-sdmmagpie.c, sdmmagpie-camera.dtsi), read as data. The main camera:
 *
 *   the sensor     a Sony IMX582 (48 MP, "davinci_ofilm_imx582"), on CCI 0 master 0 (GPIO 17 data,
 *                  18 clock, function cci_i2c, pull-ups), slave address 0x34; done by hand here, as
 *                  the other drivers do their I2C (the pins taken for it and given back as found)
 *   chip ID        a 16-bit register at the 16-bit address 0x0016: 0x0582
 *   reset          GPIO 26, high = running
 *   MCLK           camcc's mclk2 (RCG 0x5044, branch 0x505c) at 19.2 MHz from the crystal (the
 *                  RCG's source 0, divider 1, as Android's frequency table has it) on GPIO 15
 *                  (function cam_mclk, 2 mA); before it the camera's top power domain
 *                  (titan_top_gdsc, camcc 0xc1c4), as Android's "cam_clk" supply
 *   supplies       VIO PM6150's LDO 16 (RPMh "ldoa16") at 1.8 V; VANA PM6150L's BOB ("bobc1"), which
 *                  the device tree keeps at 3.296 V at least; VDIG a switch that PM6150L GPIO 3
 *                  turns on ("CAMW_VDIG_EN"), fed by PM6150L's SMPS 8 ("smpc8") at 1.352 V; and
 *                  "cam_v_custom2" PM6150L's LDO 9 ("ldoc9"), which the device tree keeps at 2.95 V
 *                  at least (the sensor asks 2.856 to 3.0)
 *   power-up       (the module file's): custom2 on, 5 ms; VANA; VDIG; VIO; MCLK on, 10 ms; reset
 *                  high, 2 ms. Power-down: reset low, 2 ms; MCLK off; VIO, VDIG, VANA off, 20 ms;
 *                  custom2 off
 *
 * BOB is the parent of other supplies: it is voted on here and never off again. The others are
 * nobody else's in this kernel (no other driver votes for them).
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   cam=1   a look, reading only: the camera's clocks and power domain, the pins, PM6150L GPIO 3
 *   cam=2   ... and the sensor powered up as Android powers it, its chip ID read, and powered down
 *           again (every pin and clock register put back as found)
 *   cam=3   the CSID's test pattern through RDI 0 and IFE-lite 0 into memory, which the loader hands
 *           to the host after the run (xl camera): the camera subsystem's clocks, SMMU and blocks
 *           without the sensor (capture_tpg() below)
 *   cam=4   the sensor's own frames that way, through CSIPHY 0 (capture_sensor() below)
 *   cam=5   the main camera for iOS: /dev/hakios-camera (serve_ios() below)
 *
 * With any of them, cam_popup=1: the front camera up, three seconds, and down again (popup_move() below).
 *
 * At the run's end (and on a panic's way out) whatever is on goes off.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.camera.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern lck_mtx_t hakios_rsc_lock;            /* RPMh's sequencer: drivers/core/hakios.platform.c, shared with every driver that writes RPMh */

static uint32_t level;
static bool broken, leaving;
static vm_offset_t keep;                /* memory in one piece for the frames, taken at boot (hakios_camera_init()) */
static uint64_t keep_bytes;

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[240];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

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
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_ms(uint32_t ms)
{
	if (leaving || !ml_get_interrupts_enabled()) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		say("cam: %s: no more writing for the rest of the run\n", why);
	}
}

/* ---- the sensors ------------------------------------------------------------------------------------------ */

/*
 * The two here so far (boot-arg cam_sensor=front for the front one), from dtbo 14's cam-sensor@0 and @2 and
 * their module files. Both on CCI 0 master 0, with the same supplies (VIO ldoa16, VANA bobc1, VDIG's feed
 * smpc8, custom ldoc9); the front one's VDIG switch is TLMM GPIO 57 ("CAMF_VDIG_EN"), the main one's PM6150L
 * GPIO 3, and its power-up has VIO before VDIG. The front one is behind the pop-up: up before its power-up,
 * down after its power-down.
 */
struct sensor {
	const char      *name, *chip, *tab;
	unsigned        mclk_pin, reset_pin, vdig_pin;      /* vdig_pin 0: PM6150L GPIO 3 */
	uint32_t        mclk_cmd, mclk_cbcr;
	const char      *mclk_name;
	bool            vio_before_vdig, popup, samsung;    /* samsung: 16-bit registers, gain 32 = 1x (else the IMX582's code) */
	uint8_t         slave;
	uint16_t        id_reg, id;
	unsigned        csiphy;
	uint32_t        timer_cmd, timer_cbcr, phy_cbcr;
	uint32_t        bayer, ios_sensor;                  /* 0 RGGB, 1 GRBG; userland's 1 back, 2 front */
	uint32_t        down_reset_ms, down_mclk_ms;
};
static const struct sensor sensors[2] = {
	{ "main", "IMX582", "camera-imx582.tab", 15, 26, 0, 0x5044, 0x505c, "MCLK2", false, false, false, 0x34, 0x0016, 0x0582, 0, 0x6004,
	  0x601c, 0x6020, 0, 1, 2, 0 },
	{ "front", "S5K3T2", "camera-s5k3t2.tab", 13, 23, 57, 0x5004, 0x501c, "MCLK0", true, true, true, 0xac, 0x0000, 0x3142, 1, 0x6028,
	  0x6040, 0x6044, 1, 2, 1, 1 },
};
static const struct sensor *hw = &sensors[0];

/* ---- pins: GPIOs 13 to 26 of the south tile (0x3d00000) ----------------------------------------------- */

#define TLMM_SOUTH_PHYS         0x03d00000UL
#define PIN_FIRST               13
#define PIN_LAST                26
#define PIN_MCLK                (hw->mclk_pin)
#define PIN_SDA                 17              /* CCI 0, master 0 */
#define PIN_SCL                 18
#define PIN_RESET               (hw->reset_pin)
#define GPIO_CFG(n)             (tlmm + 0x1000 * ((n) - PIN_FIRST))
#define GPIO_IO(n)              (tlmm + 0x1000 * ((n) - PIN_FIRST) + 4)
#define CFG_PULL_UP             3
#define CFG_FUNCTION(f)         ((uint32_t)(f) << 2)
#define CFG_DRIVE_MA(ma)        ((uint32_t)((ma) / 2 - 1) << 6)
#define CFG_OUTPUT              (1u << 9)
#define IO_OUT                  (1u << 1)

static vm_offset_t tlmm;
static unsigned pins[4];                /* the sensor's MCLK, CCI 0's two, the sensor's reset */
static uint32_t found_cfg[4], found_io[4];
static bool pins_found, pins_taken;

static void
show_pin(unsigned pin, const char *what)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	say("cam: GPIO %u (%s): function %u, %s, pull %u, %u mA, in %u out %u\n", pin, what, (cfg >> 2) & 0xf,
	    cfg & CFG_OUTPUT ? "output" : "input", cfg & 3, ((cfg >> 6) & 7) * 2 + 2, io & 1, (io >> 1) & 1);
}

static void
pins_find(void)
{
	pins[0] = PIN_MCLK;
	pins[1] = PIN_SDA;
	pins[2] = PIN_SCL;
	pins[3] = PIN_RESET;
	for (int i = 0; i < 4; i++) {
		found_cfg[i] = rd(GPIO_CFG(pins[i]));
		found_io[i] = rd(GPIO_IO(pins[i]));
	}
	pins_found = true;
}

static void
pins_give_back(void)
{
	if (!pins_taken) {
		return;
	}
	for (int i = 0; i < 4; i++) {
		wr(GPIO_IO(pins[i]), found_io[i]);
		wr(GPIO_CFG(pins[i]), found_cfg[i]);
	}
	pins_taken = false;
}

static void
reset_pin(bool high)
{
	pins_taken = true;
	wr(GPIO_IO(PIN_RESET), high ? IO_OUT : 0);
	wr(GPIO_CFG(PIN_RESET), CFG_FUNCTION(0) | CFG_DRIVE_MA(2) | CFG_OUTPUT);
}

/* ---- I2C by hand on CCI 0's pins (as in hakios.gt9886.c) -------------------------------------------------- */

static void
half_bit(void)
{
	spin_us(5);                     /* 100 kHz at most */
}

static void
line(unsigned pin, bool high)
{
	pins_taken = true;
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

/* a 16-bit register at a 16-bit address of the sensor (8-bit slave address `slave') */
static bool
sensor_read16(uint8_t slave, uint16_t reg, uint16_t *value)
{
	bool answered;
	uint8_t high = 0, low = 0;

	i2c_start();
	answered = i2c_write(slave) && i2c_write((uint8_t)(reg >> 8)) && i2c_write((uint8_t)reg);
	if (answered) {
		i2c_start();
		answered = i2c_write(slave | 1);
	}
	if (answered) {
		high = i2c_read(true);
		low = i2c_read(false);
	}
	i2c_stop();
	*value = (uint16_t)(high << 8 | low);
	return answered;
}

/* ---- supplies, through RPMh (as hakios.gt9886.c) -------------------------------------------------------- */

#define AOP_DICT_PHYS           0x0c3f0000UL    /* "qcom,cmd-db" (0xc3f000c): where the command database is */
#define AOP_DICT_OFFSET         0xc
#define RSC_PHYS                0x18220000UL    /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))   /* command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define VRM_VOLTAGE             0
#define VRM_ENABLE              4

static vm_offset_t rsc, cmd_db;
static uint32_t cmd_db_size;

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db == NULL || db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512 || sizeof(*db) + slave->header_offset +
		    (size_t)slave->count * sizeof(*entry) > cmd_db_size) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		spin_us(100);
	}
	say("cam: RPMh: timeout (register %#x = %#x)\n", reg, rd(TCS(reg)));
	return false;
}

static bool
rpmh_write_locked(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			say("cam: RPMh: the command slots stay busy\n");
			return false;
		}
		spin_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

/* one request at a time through command slot set 0 (hakios_rsc_lock: every driver that writes RPMh takes it) */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	bool done;

	if (leaving) {
		return rpmh_write_locked(address, data, count);
	}
	lck_mtx_lock(&hakios_rsc_lock);
	done = rpmh_write_locked(address, data, count);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done;
}

struct supply {
	const char *name, *what;
	uint32_t mv;
	bool never_off;
	uint32_t address;
	bool on;
};

enum { VIO, VANA, VDIG, CUSTOM, SUPPLIES };
static struct supply supplies[SUPPLIES] = {
	[VIO] = { "ldoa16", "VIO (PM6150 LDO 16)", 1800, false, 0, false },
	[VANA] = { "bobc1", "VANA (PM6150L BOB)", 3296, true, 0, false },
	[VDIG] = { "smpc8", "VDIG's feed (PM6150L SMPS 8)", 1352, false, 0, false },
	[CUSTOM] = { "ldoc9", "custom (PM6150L LDO 9)", 2950, false, 0, false },
};

static bool
rpmh_find(void)
{
	vm_offset_t dict = ml_io_map(AOP_DICT_PHYS, 0x1000);
	uint32_t db_phys, db_size;

	if (dict == 0) {
		return false;
	}
	db_phys = rd(dict + AOP_DICT_OFFSET);
	db_size = rd(dict + AOP_DICT_OFFSET + 4);
	if (db_phys == 0 || db_size < sizeof(struct cmd_db_header) || db_size > 0x100000) {
		say("cam: no command database (dictionary says %#x, %#x bytes)\n", db_phys, db_size);
		return false;
	}
	cmd_db = ml_io_map_wcomb(db_phys, (db_size + 0xfff) & ~0xfffu);
	cmd_db_size = db_size;
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	if (cmd_db == 0 || rsc == 0) {
		return false;
	}
	for (int i = 0; i < SUPPLIES; i++) {
		if ((supplies[i].address = rpmh_address(supplies[i].name)) == 0) {
			say("cam: %s is not in the command database\n", supplies[i].name);
			return false;
		}
	}
	return true;
}

static bool
supply_on(struct supply *s)
{
	uint32_t address[2] = { s->address + VRM_VOLTAGE, s->address + VRM_ENABLE }, data[2] = { s->mv, 1 };

	if (broken) {
		return false;
	}
	if (!rpmh_write(address, data, 2)) {
		give_up("an RPMh request that was not done");
		return false;
	}
	s->on = true;
	say("cam: %s on at %u mV (RPMh %s, %#x)\n", s->what, s->mv, s->name, s->address);
	return true;
}

static void
supply_off(struct supply *s)
{
	uint32_t address = s->address + VRM_ENABLE, data = 0;

	if (!s->on || s->never_off) {
		return;
	}
	s->on = false;
	say("cam: %s %s\n", s->what, rpmh_write(&address, &data, 1) ? "off" : "NOT OFF (RPMh)");
}

/* ---- PM6150L GPIO 3 (VDIG's switch), through the PMIC arbiter (as hakios.vibrator.c) ------------------------- */

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHNLS_PHYS          0x0c600000UL
#define ARB_SYSTEM              0               /* the device tree's "qcom,ee" */
#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS        512
#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define OBSERVER_DATA0          0x18
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)
#define COMMAND_READ            (1u << 27)
#define TRANSFER_TIMEOUT_US     2000
#define DROPPED                 "the transfer was dropped"

#define PGPIO_ID                0x4c2           /* PM6150L (slave 4), GPIO 3 (peripheral 0xc2) */
#define PGPIO_TYPE              0x04            /* 0x10: a GPIO */
#define PGPIO_MODE_CTL          0x40            /* [1:0] 1 output */
#define PGPIO_DIG_VIN_CTL       0x41
#define PGPIO_DIG_PULL_CTL      0x42            /* 5: no pull */
#define PGPIO_OUT_SOURCE_CTL    0x44            /* [7] the level, the normal function */
#define PGPIO_EN_CTL            0x46            /* [7] on */

static vm_offset_t arb_core, arb_cnfg, arb_observer, pgpio_write;
static uint16_t pgpio_channel;
static bool pgpio_found, pgpio_writable, pgpio_changed;
static const uint8_t pgpio_which[5] = { PGPIO_DIG_VIN_CTL, PGPIO_DIG_PULL_CTL, PGPIO_MODE_CTL, PGPIO_OUT_SOURCE_CTL, PGPIO_EN_CTL };
static uint8_t pgpio_found_regs[5];

static void
lock(void)
{
	if (!leaving) {
		hakios_spmi_lock();
	}
}

static void
unlock(void)
{
	if (!leaving) {
		hakios_spmi_unlock();
	}
}

static const char *
wait_done(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return !(status & STATUS_DONE) ? "no answer from the arbiter" : (status & STATUS_DENIED) ? "the arbiter refuses" :
	       (status & STATUS_FAILURE) ? "the transfer failed" : (status & STATUS_DROPPED) ? DROPPED : NULL;
}

static const char *
pgpio_read(uint8_t reg, uint8_t *value)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * pgpio_channel;
	const char *problem = "no channel";

	for (int tries = 0; pgpio_found && tries < 3; tries++) {
		lock();
		wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4);
		problem = wait_done(channel);
		*value = (uint8_t)rd(channel + OBSERVER_DATA0);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

/* One of GPIO 3's five registers above written and read back (when `force', also after something went wrong). */
static bool
pgpio_put(uint8_t reg, uint8_t value, bool force)
{
	const char *problem = NULL;
	uint8_t back = 0xff;

	if ((broken && !force) || !pgpio_writable || pgpio_write == 0 || (reg != PGPIO_MODE_CTL && reg != PGPIO_DIG_VIN_CTL &&
	    reg != PGPIO_DIG_PULL_CTL && reg != PGPIO_OUT_SOURCE_CTL && reg != PGPIO_EN_CTL)) {
		give_up("a write to the PM6150L that may not be made");
		return false;
	}
	if (((rd(arb_core + ARB_CHANNEL(pgpio_channel)) >> 8) & 0xfff) != PGPIO_ID) {
		give_up("the arbiter's channel is no longer the PM6150L's GPIO 3");
		return false;
	}
	for (int tries = 0; tries < 3; tries++) {
		lock();
		wr(pgpio_write + CHANNEL_WDATA0, value);
		wr(pgpio_write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
		problem = wait_done(pgpio_write);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	if (problem != NULL || (problem = pgpio_read(reg, &back)) != NULL || back != value) {
		say("cam: PM6150L GPIO 3 %#04x <- %#04x: %s (reads %#04x)\n", reg, value, problem ? problem : "does not read back", back);
		give_up("a write to the PM6150L that failed");
		return false;
	}
	return true;
}

static bool
pgpio_find(void)
{
	uint32_t channels;
	uint8_t type = 0;

	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	if ((rd(arb_core + ARB_VERSION) & 0xf0000000u) != ARB_VERSION_5) {
		say("cam: the arbiter is version %#x, not 5: left alone\n", rd(arb_core + ARB_VERSION));
		return false;
	}
	channels = rd(arb_core + ARB_FEATURES) & 0x3ff;
	channels = channels > ARB_MAX_CHANNELS ? ARB_MAX_CHANNELS : channels;
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != PGPIO_ID) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		say("cam: channel %u is the PM6150L's GPIO 3 (%#x); written by system %u%s\n", n, map, owner,
		    owner == ARB_SYSTEM ? ", which is us" : "");
		if (!pgpio_found || owner == ARB_SYSTEM) {
			pgpio_found = true;
			pgpio_channel = (uint16_t)n;
			pgpio_writable = owner == ARB_SYSTEM;
		}
	}
	if (!pgpio_found) {
		say("cam: no channel of the arbiter is the PM6150L's GPIO 3 (%#x)\n", PGPIO_ID);
		return false;
	}
	if (pgpio_read(PGPIO_TYPE, &type) != NULL || type != 0x10) {
		say("cam: the PM6150L's GPIO 3 is of type %#04x, not a GPIO (0x10): left alone\n", type);
		pgpio_found = false;
		return false;
	}
	for (int i = 0; i < 5; i++) {
		if (pgpio_read(pgpio_which[i], &pgpio_found_regs[i]) != NULL) {
			pgpio_found = false;
			return false;
		}
	}
	say("cam: PM6150L GPIO 3 as found: VIN %#04x, pull %#04x, mode %#04x, source %#04x, enable %#04x\n", pgpio_found_regs[0],
	    pgpio_found_regs[1], pgpio_found_regs[2], pgpio_found_regs[3], pgpio_found_regs[4]);
	if (pgpio_writable) {
		pgpio_write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * pgpio_channel, 0x1000);
	}
	return true;
}

/* VDIG's switch: GPIO 3 an output of the normal function, VIN0, no pull, enabled, high (on) or low */
static bool
vdig_switch(bool on)
{
	pgpio_changed = true;
	if (on) {
		return pgpio_put(PGPIO_OUT_SOURCE_CTL, 0x00, false) && pgpio_put(PGPIO_DIG_VIN_CTL, 0, false) &&
		       pgpio_put(PGPIO_DIG_PULL_CTL, 5, false) && pgpio_put(PGPIO_MODE_CTL, 1, false) &&
		       pgpio_put(PGPIO_EN_CTL, 0x80, false) && pgpio_put(PGPIO_OUT_SOURCE_CTL, 0x80, false);
	}
	return pgpio_put(PGPIO_OUT_SOURCE_CTL, 0x00, true);
}

static void
vdig_give_back(void)
{
	if (!pgpio_changed) {
		return;
	}
	pgpio_changed = false;
	(void)pgpio_put(PGPIO_OUT_SOURCE_CTL, 0x00, true);
	for (int i = 0; i < 5; i++) {
		(void)pgpio_put(pgpio_which[i], pgpio_found_regs[i], true);
	}
	say("cam: PM6150L GPIO 3 put back as found\n");
}

/* ---- the front camera's pop-up motor ---------------------------------------------------------------- */

/*
 * A stepper motor behind a TI DRV8846 (dtbo 14's ti,drv8846@0; Xiaomi's drivers/misc/drv8846.c): its step
 * input is PM6150L's LPG channel 1 (SPMI slave 5, 0xbd00; dt pm6150l_pwm 1) on PM6150L GPIO 10 (function
 * func1, "pwm_out_gpio10_default"), the rest TLMM pins (gpio, pull-up, 2 mA: motor_gpio_default): mode 0
 * GPIO 91 and mode 1 GPIO 66 (step mode 2: 0 and 1), sleep GPIO 88 (high = running), direction GPIO 55
 * (low = up), the driver's power GPIO 90 (high). A move, drv8846_move(): direction, sleep high, then the
 * step pulses at half duty: 625 us for 50 ms, 52.083 us for 720 ms (and 5 ms more going down), 625 us for
 * 50 ms, then the pulses off and sleep low. There is no end switch: the time is what stops it (the module
 * leans on its stops). The LPG's periods as qpnp_lpg's search finds them: 19.2 MHz, 9 bits, prediv 3 and
 * exponent 3 (638.976 us), prediv 1 and exponent 1 (53.248 us), the value 256 (half).
 */
#define PIN_DIR                 55
#define PIN_MODE1               66
#define PIN_SLEEP               88
#define PIN_MOTOR_POWER         90
#define PIN_MODE0               91
#define LPG_ID(n)               (0x5bc + (n))   /* PM6150L (slave 5), LPG channel n (0xbc00, 0xbd00) */
#define LPG_SUBTYPE             0x05
#define LPG_PWM_SIZE_CLK        0x41
#define LPG_PREDIV_CLK          0x42
#define LPG_TYPE_CONFIG         0x43            /* [5] glitch removal */
#define LPG_VALUE_LSB           0x44
#define LPG_VALUE_MSB           0x45
#define LPG_ENABLE_CONTROL      0x46            /* [7] output, [2] source: the PWM value */
#define LPG_SYNC                0x47
#define STEP_GPIO_ID            0x4c9           /* PM6150L (slave 4), GPIO 10 */
#define PREDIV_RAMP             (1u << 5 | 3u)  /* prediv 3 (index 1), exponent 3 */
#define PREDIV_FAST             (0u << 5 | 1u)  /* prediv 1, exponent 1 */

/* A PMIC peripheral reached through the arbiter (as PM6150L GPIO 3 above), its writable registers 0x40..0x47. */
struct periph {
	uint16_t        id;
	const char      *name;
	bool            found, writable;
	uint16_t        channel;
	vm_offset_t     write;
	uint8_t         found_regs[8];
};
static struct periph lpg = { LPG_ID(1), "PM6150L LPG", false, false, 0, 0, { 0 } };
static struct periph step_gpio = { STEP_GPIO_ID, "PM6150L GPIO 10", false, false, 0, 0, { 0 } };

static const char *
periph_read(struct periph *p, uint8_t reg, uint8_t *value)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * p->channel;
	const char *problem = "no channel";

	for (int tries = 0; p->found && tries < 3; tries++) {
		lock();
		wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4);
		problem = wait_done(channel);
		*value = (uint8_t)rd(channel + OBSERVER_DATA0);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

static bool
periph_put(struct periph *p, uint8_t reg, uint8_t value, bool force)
{
	const char *problem = NULL;
	uint8_t back = 0xff;

	if ((broken && !force) || !p->writable || p->write == 0 || reg < 0x40 || reg > 0x47) {
		give_up("a PMIC write that may not be made");
		return false;
	}
	if (((rd(arb_core + ARB_CHANNEL(p->channel)) >> 8) & 0xfff) != p->id) {
		give_up("the arbiter's channel is no longer the peripheral's");
		return false;
	}
	for (int tries = 0; tries < 3; tries++) {
		lock();
		wr(p->write + CHANNEL_WDATA0, value);
		wr(p->write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
		problem = wait_done(p->write);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	/* (LPG_SYNC is a strobe: not read back) */
	/* (the LPG's ENABLE_CONTROL keeps only its output bit on this one, the PWM subtype: no source select) */
	uint8_t keeps = p == &lpg && reg == LPG_ENABLE_CONTROL ? 0x80 : 0xff;

	if (problem != NULL || (reg != LPG_SYNC && ((problem = periph_read(p, reg, &back)) != NULL || (back & keeps) != (value & keeps)))) {
		say("cam: %s %#04x <- %#04x: %s (reads %#04x)\n", p->name, reg, value, problem ? problem : "does not read back", back);
		give_up("a PMIC write that failed");
		return false;
	}
	return true;
}

/* the arbiter's channel for it (ours to write), and its registers 0x40..0x47 as found */
static bool
periph_find(struct periph *p)
{
	uint32_t channels = rd(arb_core + ARB_FEATURES) & 0x3ff;

	channels = channels > ARB_MAX_CHANNELS ? ARB_MAX_CHANNELS : channels;
	for (uint32_t n = 0; n < channels && !p->found; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));

		if (map != 0 && ((map >> 8) & 0xfff) == p->id && (rd(arb_cnfg + ARB_OWNER(n)) & 7) == ARB_SYSTEM) {
			p->found = p->writable = true;
			p->channel = (uint16_t)n;
			p->write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * n, 0x1000);
		}
	}
	for (int r = 0; p->found && r < 8; r++) {
		if (periph_read(p, (uint8_t)(0x40 + r), &p->found_regs[r]) != NULL) {
			p->found = false;
		}
	}
	if (!p->found || p->write == 0) {
		say("cam: no channel of the arbiter that we may write is %s's (%#x)\n", p->name, p->id);
		return false;
	}
	say("cam: %s (%#x): channel %u; 0x40..0x47 as found: %02x %02x %02x %02x %02x %02x %02x %02x\n", p->name, p->id, p->channel,
	    p->found_regs[0], p->found_regs[1], p->found_regs[2], p->found_regs[3], p->found_regs[4], p->found_regs[5], p->found_regs[6],
	    p->found_regs[7]);
	return true;
}

/* a TLMM pin anywhere (the motor's are on the north and west tiles): its registers, mapped once */
static vm_offset_t
tlmm_pin(unsigned pin)
{
	static const struct { uint16_t pin; uint32_t tile; } tiles[] = {
		{ PIN_DIR, 0x00100000 }, { PIN_MODE1, 0x00500000 }, { PIN_SLEEP, 0x00500000 }, { PIN_MOTOR_POWER, 0x00500000 },
		{ PIN_MODE0, 0x00500000 }, { 57, 0x00100000 },
	};
	static vm_offset_t mapped[sizeof(tiles) / sizeof(tiles[0])];

	for (unsigned i = 0; i < sizeof(tiles) / sizeof(tiles[0]); i++) {
		if (tiles[i].pin == pin) {
			if (mapped[i] == 0) {
				mapped[i] = ml_io_map(0x03400000UL + tiles[i].tile + 0x1000UL * pin, 0x1000);
			}
			return mapped[i];
		}
	}
	return 0;
}

static const unsigned motor_pins[5] = { PIN_DIR, PIN_MODE1, PIN_SLEEP, PIN_MOTOR_POWER, PIN_MODE0 };
static uint32_t motor_found_cfg[5], motor_found_io[5];
static bool motor_ready, popped_up;
static uint8_t lpg_subtype;

static void
motor_pin(unsigned pin, bool high)
{
	vm_offset_t at = tlmm_pin(pin);

	wr(at + 4, high ? IO_OUT : 0);
	wr(at, CFG_PULL_UP | CFG_FUNCTION(0) | CFG_DRIVE_MA(2) | CFG_OUTPUT);
}

/* the pins and the PMIC's parts as the probe leaves them: step mode 2, direction up, asleep, powered; GPIO 10 the LPG's */
static bool
motor_setup(void)
{
	if (motor_ready) {
		return true;
	}
	for (int i = 0; i < 5; i++) {
		if (tlmm_pin(motor_pins[i]) == 0) {
			return false;
		}
		motor_found_cfg[i] = rd(tlmm_pin(motor_pins[i]));
		motor_found_io[i] = rd(tlmm_pin(motor_pins[i]) + 4);
	}
	if (!periph_find(&lpg) || !periph_find(&step_gpio) || periph_read(&lpg, LPG_SUBTYPE, &lpg_subtype) != NULL) {
		return false;
	}
	motor_ready = true;
	motor_pin(PIN_MODE0, false);
	motor_pin(PIN_MODE1, true);
	motor_pin(PIN_DIR, false);
	motor_pin(PIN_SLEEP, false);
	motor_pin(PIN_MOTOR_POWER, true);
	/* GPIO 10: an output of func1 (2) that is read as well (mode 2: what reaches the pin is seen), VIN1, no pull, push-pull at drive 3, enabled */
	if (!periph_put(&step_gpio, 0x44, 0x02, false) || !periph_put(&step_gpio, 0x41, 0x01, false) ||
	    !periph_put(&step_gpio, 0x42, 0x05, false) || !periph_put(&step_gpio, 0x45, 0x03, false) ||
	    !periph_put(&step_gpio, 0x40, 0x02, false) || !periph_put(&step_gpio, 0x46, 0x80, false)) {
		return false;
	}
	/* the LPG: 9 bits from 19.2 MHz (its size bit where its subtype has it), half duty, the PWM value as the source, output off */
	uint8_t size = lpg_subtype == 0x0b ? 1u << 2 : 1u << 4;

	if (!periph_put(&lpg, LPG_ENABLE_CONTROL, 1u << 2, false) || !periph_put(&lpg, LPG_PWM_SIZE_CLK, size | 3, false) ||
	    !periph_put(&lpg, LPG_PREDIV_CLK, PREDIV_RAMP, false) || !periph_put(&lpg, LPG_VALUE_MSB, 1, false) ||
	    !periph_put(&lpg, LPG_VALUE_LSB, 0, false) || !periph_put(&lpg, LPG_SYNC, 1, false) ||
	    !periph_put(&lpg, LPG_TYPE_CONFIG, (uint8_t)(lpg.found_regs[3] | 1u << 5), false)) {
		return false;
	}
	say("cam: the pop-up motor's driver ready (LPG subtype %#04x)\n", lpg_subtype);
	wait_ms(10);
	return true;
}

static void
motor_give_back(void)
{
	if (!motor_ready) {
		return;
	}
	motor_ready = false;
	(void)periph_put(&lpg, LPG_ENABLE_CONTROL, lpg.found_regs[6] & ~0x80u, true);
	wr(tlmm_pin(PIN_SLEEP) + 4, 0);
	for (int i = 0; i < 5; i++) {
		wr(tlmm_pin(motor_pins[i]) + 4, motor_found_io[i]);
		wr(tlmm_pin(motor_pins[i]), motor_found_cfg[i]);
	}
	for (int r = 0; r < 7; r++) {           /* (0x47, the strobe, left alone) */
		(void)periph_put(&lpg, (uint8_t)(0x40 + r), lpg.found_regs[r], true);
	}
	for (int r = 0; r < 7; r++) {
		(void)periph_put(&step_gpio, (uint8_t)(0x40 + r), step_gpio.found_regs[r], true);
	}
	say("cam: the pop-up motor's pins and PMIC parts put back as found\n");
}

/* drv8846_move(): up (true) or down; false if a write failed (the pulses off then) */
static bool
popup_move(bool up)
{
	bool ok;

	if (!motor_setup()) {
		return false;
	}
	motor_pin(PIN_DIR, !up);
	motor_pin(PIN_SLEEP, true);
	ok = periph_put(&lpg, LPG_PREDIV_CLK, PREDIV_RAMP, !up) && periph_put(&lpg, LPG_SYNC, 1, !up) &&
	    periph_put(&lpg, LPG_ENABLE_CONTROL, 1u << 7 | 1u << 2, !up);
	if (ok) {
		wait_ms(50);
		ok = periph_put(&lpg, LPG_PREDIV_CLK, PREDIV_FAST, !up) && periph_put(&lpg, LPG_SYNC, 1, !up);
		{
			/* the pin's level (STATUS1 bit 0) a few hundred times: pulses there read both ways */
			uint64_t start = mach_absolute_time(), ns;
			unsigned high = 0, reads = 0;
			uint8_t status;

			for (; reads < 400; reads++) {
				if (periph_read(&step_gpio, 0x08, &status) == NULL && (status & 1)) {
					high++;
				}
			}
			absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
			say("cam: GPIO 10 with %s's pulses: high %u of %u reads (in %llu us)\n", lpg.name, high, reads, ns / 1000);
			wait_ms((up ? 720 : 725) > ns / 1000000 ? (uint32_t)((up ? 720 : 725) - ns / 1000000) : 1);
		}
		ok = ok && periph_put(&lpg, LPG_PREDIV_CLK, PREDIV_RAMP, !up) && periph_put(&lpg, LPG_SYNC, 1, !up);
		wait_ms(50);
	}
	(void)periph_put(&lpg, LPG_ENABLE_CONTROL, 1u << 2, true);
	motor_pin(PIN_SLEEP, false);
	popped_up = up;                 /* (tried: brought down at the end in any case) */
	say("cam: the front camera %s%s\n", up ? "up" : "down", ok ? "" : " -- A WRITE FAILED: the pulses stopped");
	return ok;
}

/* ---- clocks: GCC's camera AHB, camcc's titan top power domain and MCLK2 -------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_CAMERA_AHB_CBCR     0xb008
#define GCC_CAMERA_XO_CBCR      0xb02c
#define CAMCC_PHYS              0x0ad00000UL
#define CAMCC_SIZE              0x10000UL
#define TITAN_TOP_GDSCR         0xc1c4
#define GDSC_COLLAPSE           (1u << 0)
#define GDSC_ON                 (1u << 31)
#define MCLK_CMD_RCGR           (hw->mclk_cmd)
#define MCLK_CFG_RCGR           (hw->mclk_cmd + 4)
#define MCLK_CBCR               (hw->mclk_cbcr)
#define CMD_UPDATE              (1u << 0)
#define CBCR_OFF                (1u << 31)

static vm_offset_t gcc, camcc;
static uint32_t found_gdscr, found_mclk_cfg_rcgr, found_mclk_cbcr;
static bool gdsc_switched, mclk_switched;

static bool
wait_bits(uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t waited = 0; waited <= us; waited += 10) {
		if ((rd(camcc + offset) & mask) == want) {
			return true;
		}
		spin_us(10);
	}
	return false;
}

static void
show_clocks(void)
{
	say("cam: GCC: camera AHB %#x, camera XO %#x\n", rd(gcc + GCC_CAMERA_AHB_CBCR), rd(gcc + GCC_CAMERA_XO_CBCR));
	if (!(rd(gcc + GCC_CAMERA_AHB_CBCR) & CBCR_OFF)) {
		say("cam: camcc: titan top GDSC %#x; %s RCG cmd %#x cfg %#x, branch %#x\n", rd(camcc + TITAN_TOP_GDSCR), hw->mclk_name,
		    rd(camcc + MCLK_CMD_RCGR), rd(camcc + MCLK_CFG_RCGR), rd(camcc + MCLK_CBCR));
	}
}

static bool
camcc_put(uint32_t offset, uint32_t value, uint32_t check_mask)
{
	if (broken || (offset != TITAN_TOP_GDSCR && offset != MCLK_CFG_RCGR && offset != MCLK_CMD_RCGR && offset != MCLK_CBCR)) {
		give_up("a camcc register that is not to be written");
		return false;
	}
	wr(camcc + offset, value);
	if ((rd(camcc + offset) & check_mask) != (value & check_mask)) {
		say("cam: camcc %#x <- %#x, but it reads %#x\n", offset, value, rd(camcc + offset));
		give_up("a camcc register that did not read back");
		return false;
	}
	return true;
}

static bool
gdsc_on(void)
{
	if (rd(gcc + GCC_CAMERA_AHB_CBCR) & CBCR_OFF) {
		give_up("GCC's camera AHB clock is off (camcc cannot be reached)");
		return false;
	}
	found_gdscr = rd(camcc + TITAN_TOP_GDSCR);
	if (!(found_gdscr & GDSC_COLLAPSE) && (found_gdscr & GDSC_ON)) {
		say("cam: the titan top power domain was on already\n");
		return true;
	}
	if (!camcc_put(TITAN_TOP_GDSCR, found_gdscr & ~GDSC_COLLAPSE, GDSC_COLLAPSE)) {
		return false;
	}
	gdsc_switched = true;
	if (!wait_bits(TITAN_TOP_GDSCR, GDSC_ON, GDSC_ON, 1000)) {
		say("cam: the titan top power domain does not come on (%#x)\n", rd(camcc + TITAN_TOP_GDSCR));
		give_up("a power domain that does not come on");
		return false;
	}
	say("cam: the titan top power domain on (%#x)\n", rd(camcc + TITAN_TOP_GDSCR));
	return true;
}

static void
gdsc_off(void)
{
	if (!gdsc_switched) {
		return;
	}
	gdsc_switched = false;
	wr(camcc + TITAN_TOP_GDSCR, rd(camcc + TITAN_TOP_GDSCR) | GDSC_COLLAPSE);
	say("cam: the titan top power domain %s (%#x)\n", wait_bits(TITAN_TOP_GDSCR, GDSC_ON, 0, 1000) ? "off" : "NOT OFF",
	    rd(camcc + TITAN_TOP_GDSCR));
}

/* MCLK2 at 19.2 MHz: the RCG from the crystal (source 0), divider 1, no M/N; then the branch; then the pin */
static bool
mclk_on(void)
{
	found_mclk_cfg_rcgr = rd(camcc + MCLK_CFG_RCGR);
	found_mclk_cbcr = rd(camcc + MCLK_CBCR);
	if (found_mclk_cbcr & 1) {
		give_up("the sensor's MCLK is on already (somebody else's)");
		return false;
	}
	mclk_switched = true;
	if (!camcc_put(MCLK_CFG_RCGR, (found_mclk_cfg_rcgr & ~0x371fu) | 0u << 8 | 1u, 0x371f) ||
	    !camcc_put(MCLK_CMD_RCGR, rd(camcc + MCLK_CMD_RCGR) | CMD_UPDATE, 0)) {
		return false;
	}
	if (!wait_bits(MCLK_CMD_RCGR, CMD_UPDATE, 0, 500)) {
		say("cam: %s's RCG does not take its configuration (cmd %#x)\n", hw->mclk_name, rd(camcc + MCLK_CMD_RCGR));
		give_up("an RCG that does not update");
		return false;
	}
	if (!camcc_put(MCLK_CBCR, found_mclk_cbcr | 1, 1)) {
		return false;
	}
	if (!wait_bits(MCLK_CBCR, CBCR_OFF, 0, 500)) {
		say("cam: %s's branch does not come on (%#x)\n", hw->mclk_name, rd(camcc + MCLK_CBCR));
		give_up("a clock that does not come on");
		return false;
	}
	pins_taken = true;
	wr(GPIO_CFG(PIN_MCLK), CFG_FUNCTION(1) | CFG_DRIVE_MA(2));      /* cam_mclk, no pull, 2 mA: cam_sensor_mclkN_active */
	say("cam: %s on at 19.2 MHz (RCG cfg %#x, branch %#x)\n", hw->mclk_name, rd(camcc + MCLK_CFG_RCGR), rd(camcc + MCLK_CBCR));
	return true;
}

static void
mclk_off(void)
{
	if (!mclk_switched) {
		return;
	}
	mclk_switched = false;
	if (pins_found) {
		wr(GPIO_CFG(PIN_MCLK), found_cfg[0]);
	}
	wr(camcc + MCLK_CBCR, rd(camcc + MCLK_CBCR) & ~1u);
	(void)wait_bits(MCLK_CBCR, CBCR_OFF, CBCR_OFF, 500);
	wr(camcc + MCLK_CFG_RCGR, found_mclk_cfg_rcgr);
	wr(camcc + MCLK_CMD_RCGR, rd(camcc + MCLK_CMD_RCGR) | CMD_UPDATE);
	(void)wait_bits(MCLK_CMD_RCGR, CMD_UPDATE, 0, 500);
	say("cam: %s off (branch %#x)\n", hw->mclk_name, rd(camcc + MCLK_CBCR));
}

/* ---- the sensor's power ------------------------------------------------------------------------------- */

static bool powered, titan_switched;
static bool rails_up(void), pll0_up(void);
static void pll0_down(void);
static uint32_t vdig_found_cfg, vdig_found_io;
static bool vdig_pin_taken;

/* VDIG's switch: the main sensor's PM6150L GPIO 3, the front one's TLMM GPIO 57 (an output, no pull, 2 mA) */
static bool
vdig_set(bool on)
{
	vm_offset_t at;

	if (hw->vdig_pin == 0) {
		return vdig_switch(on);
	}
	if ((at = tlmm_pin(hw->vdig_pin)) == 0) {
		return false;
	}
	if (!vdig_pin_taken) {
		vdig_found_cfg = rd(at);
		vdig_found_io = rd(at + 4);
		vdig_pin_taken = true;
	}
	wr(at + 4, on ? IO_OUT : 0);
	wr(at, CFG_FUNCTION(0) | CFG_DRIVE_MA(2) | CFG_OUTPUT);
	return true;
}

static void
vdig_put_back(void)
{
	if (vdig_pin_taken) {
		vm_offset_t at = tlmm_pin(hw->vdig_pin);

		wr(at + 4, vdig_found_io);
		wr(at, vdig_found_cfg);
		vdig_pin_taken = false;
	}
	vdig_give_back();
}

/* The module file's power-up: custom, 5 ms; VANA; VDIG and VIO (in the sensor's order); MCLK, 10 ms; reset high, 2 ms */
static bool
power_up(void)
{
	powered = true;
	if (hw->popup && !popup_move(true)) {
		return false;
	}
	reset_pin(false);
	if (!supply_on(&supplies[CUSTOM])) {
		return false;
	}
	wait_ms(5);
	if (!supply_on(&supplies[VANA])) {
		return false;
	}
	if (hw->vio_before_vdig ? !supply_on(&supplies[VIO]) || !supply_on(&supplies[VDIG]) || !vdig_set(true) :
	    !supply_on(&supplies[VDIG]) || !vdig_set(true) || !supply_on(&supplies[VIO])) {
		return false;
	}
	say("cam: VDIG's switch on (%s)\n", hw->vdig_pin ? "TLMM GPIO 57 high" : "PM6150L GPIO 3 high");
	if (!gdsc_on() || !mclk_on()) {
		return false;
	}
	wait_ms(10);
	reset_pin(true);
	wait_ms(2);
	say("cam: the %s powered up as Android powers it\n", hw->chip);
	return true;
}

/* The module file's power-down: reset low; MCLK off; VDIG and VIO, VANA off, 20 ms; custom off; and the rest as found */
static void
power_down(void)
{
	if (!powered) {
		return;
	}
	powered = false;
	if (pins_found) {
		reset_pin(false);
	}
	wait_ms(hw->down_reset_ms);
	mclk_off();
	wait_ms(hw->down_mclk_ms);
	if (!titan_switched) {
		gdsc_off();
	}
	if (pgpio_changed || vdig_pin_taken) {
		(void)vdig_set(false);
	}
	supply_off(&supplies[VDIG]);
	supply_off(&supplies[VIO]);
	supply_off(&supplies[VANA]);
	wait_ms(20);
	supply_off(&supplies[CUSTOM]);
	vdig_put_back();
	pins_give_back();
	say("cam: the %s powered down, the pins put back as found\n", hw->chip);
	if (hw->popup && popped_up) {
		(void)popup_move(false);
		motor_give_back();
	}
}

static bool
read_chip_id(void)
{
	uint16_t id = 0;
	bool answered = false;

	for (int tries = 0; tries < 3 && !answered; tries++) {
		answered = sensor_read16(hw->slave, hw->id_reg, &id);
	}
	if (!answered) {
		say("cam: the %s (%#x) does not answer on CCI 0\n", hw->chip, hw->slave);
	} else {
		say("cam: the %s's chip ID: %#06x (%s)\n", hw->chip, id, id == hw->id ? "as Android wants it: the sensor answers" :
		    "NOT what Android wants");
	}
	return answered && id == hw->id;
}

/* ---- cam=3: the camera subsystem's clocks, from the crystal ------------------------------------------- */

/*
 * camcc-sdmmagpie.c's RCGs for the path. For the test pattern (cam=3) each from its source 0 (the crystal,
 * 19.2 MHz, divider 1: every one of their tables starts there), no PLL. For a sensor (`fast') the rates of
 * their tables from PLL0 (main 1200, even 600 = source 2, odd 400 = source 3; the divider field 2n - 1):
 * slow_ahb 80, camnoc_axi 150, cphy_rx 300, csi0phytimer 300, ife_lite_csid 300, ife_lite 400 MHz.
 * Then the branches, GCC's two camera AXI ones among them. Each found as it was and put back so.
 */
struct rcg {
	const char *    name;
	uint32_t        cmd, source, divider;
	uint32_t        found_cfg;
	bool            set;
};
static struct rcg rcgs[] = {
	{ "slow_ahb", 0x7058, 3, 9, 0, false }, { "camnoc_axi", 0xc12c, 2, 7, 0, false }, { "cphy_rx", 0xa064, 2, 3, 0, false },
	{ "csi0phytimer", 0x6004, 2, 3, 0, false }, { "ife_lite_csid", 0xc020, 2, 3, 0, false }, { "ife_lite", 0xc004, 3, 1, 0, false },
};
struct branch {
	const char *    name;
	bool            gcc;
	uint32_t        cbcr;
	bool            set;
};
static struct branch branches[] = {
	{ "gcc camera_hf_axi", true, 0xb020, false }, { "gcc camera_sf_axi", true, 0xb06c, false },
	{ "cpas_ahb", false, 0xc124, false }, { "camnoc_axi", false, 0xc148, false }, { "csiphy0", false, 0x6020, false },
	{ "csi0phytimer", false, 0x601c, false }, { "ife_lite_csid", false, 0xc038, false },
	{ "ife_lite_cphy_rx", false, 0xc040, false }, { "ife_lite", false, 0xc01c, false },
	{ "csiphy0 (every CSIPHY's: dt cam_csiphy1 has it too)", false, 0x6020, false },
};
static vm_offset_t gcc_b000;            /* GCC's page 0xb000 (the camera's branches) */

static bool
clocks_up(bool fast)
{
	rcgs[3].cmd = hw->timer_cmd;            /* (the sensor's CSIPHY's timer and branches) */
	branches[4].cbcr = hw->phy_cbcr;
	branches[5].cbcr = hw->timer_cbcr;
	for (unsigned i = 0; i < sizeof(rcgs) / sizeof(rcgs[0]); i++) {
		struct rcg *r = &rcgs[i];
		uint32_t cfg = r->cmd + 4, want = fast ? r->source << 8 | r->divider : 0u << 8 | 1u;

		r->found_cfg = rd(camcc + cfg);
		wr(camcc + cfg, (r->found_cfg & ~0x371fu) | want);
		if ((rd(camcc + cfg) & 0x371f) != want) {
			say("cam: the RCG %s's configuration does not read back (%#x)\n", r->name, rd(camcc + cfg));
			give_up("an RCG that did not read back");
			return false;
		}
		r->set = true;
		wr(camcc + r->cmd, rd(camcc + r->cmd) | CMD_UPDATE);
		if (!wait_bits(r->cmd, CMD_UPDATE, 0, 500)) {
			say("cam: the RCG %s does not take its configuration (cmd %#x)\n", r->name, rd(camcc + r->cmd));
			give_up("an RCG that does not update");
			return false;
		}
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		struct branch *b = &branches[i];
		vm_offset_t address = b->gcc ? gcc_b000 + (b->cbcr - 0xb000) : camcc + b->cbcr;

		if (rd(address) & 1) {
			continue;               /* (on already: left on at the end too) */
		}
		wr(address, rd(address) | 1);
		b->set = true;
		for (unsigned waited = 0; rd(address) & CBCR_OFF; waited += 10) {
			if (waited > 500) {
				say("cam: the branch %s does not come on (%#x)\n", b->name, rd(address));
				give_up("a clock that does not come on");
				return false;
			}
			spin_us(10);
		}
	}
	say("cam: the camera's clocks on, %s (%u RCGs, %u branches)\n", fast ? "at their speeds from PLL0" : "at 19.2 MHz",
	    (unsigned)(sizeof(rcgs) / sizeof(rcgs[0])), (unsigned)(sizeof(branches) / sizeof(branches[0])));
	return true;
}

static void
clocks_down(void)
{
	bool any = false;

	for (int i = (int)(sizeof(branches) / sizeof(branches[0])) - 1; i >= 0; i--) {
		struct branch *b = &branches[i];
		vm_offset_t address = b->gcc ? gcc_b000 + (b->cbcr - 0xb000) : camcc + b->cbcr;

		if (b->set) {
			wr(address, rd(address) & ~1u);
			b->set = false;
			any = true;
		}
	}
	for (int i = (int)(sizeof(rcgs) / sizeof(rcgs[0])) - 1; i >= 0; i--) {
		struct rcg *r = &rcgs[i];

		if (r->set) {
			wr(camcc + r->cmd + 4, r->found_cfg);
			wr(camcc + r->cmd, rd(camcc + r->cmd) | CMD_UPDATE);
			(void)wait_bits(r->cmd, CMD_UPDATE, 0, 500);
			r->set = false;
			any = true;
		}
	}
	if (any) {
		say("cam: the camera's clocks off\n");
	}
}

/* ---- the frames' memory, and the SMMU (as hakios.camera.c, with this phone's streams) ------------------------ */

#define SMMU_PHYS               0x15000000UL
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define CAM_PTE                 (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)       /* as hakios.sound.c's */
#define FRAME_ADDRESSES         0x40000000ull           /* the IFE's addresses for the frames */
#define CAM_HEADER_BYTES        4096
#define CAM_FRAMES              2

/*
 * The IFEs' streams: dt msm_cam_smmu_ife's 14 entries come to three match registers, as Linux makes them
 * (arm_smmu_find_sme(): an entry that one already written covers takes no register of its own): 0x900/0x460
 * (with 0xd00, 0x920, 0xd20, 0x940, 0xd40), 0x880/0x460 (with 0xc80, 0x8a0, 0xca0, 0x8c0, 0xcc0) and
 * 0x820/0x440 (with 0xc20). The hypervisor keeps a match register as it was when it is given more than
 * that (0x900/0x4e0 read back unchanged), and two registers for one stream hang the phone.
 */
#define IFE_MATCHES             3
static const uint16_t ife_stream[IFE_MATCHES] = { 0x900, 0x880, 0x820 }, ife_mask[IFE_MATCHES] = { 0x460, 0x460, 0x440 };

/* What the loader hands to the host (tools/camraw.py reads it). */
struct cam_header {
	char            magic[8];       /* "RWCAM001" */
	uint32_t        width, height;  /* pixels */
	uint32_t        stride;         /* bytes from one line to the next */
	uint32_t        format;         /* 1: MIPI RAW10, packed (four pixels in five bytes) */
	uint32_t        bayer;          /* 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR */
	uint32_t        frames;         /* how many were written whole */
	uint32_t        frame_bytes;    /* each, from CAM_HEADER_BYTES on */
	uint32_t        source;         /* 0: the CSID's test pattern, 1: the sensor */
	char            sensor[16];
	uint32_t        last;           /* which of the frames' places holds the last one written */
};

extern uint64_t loader_recording[3];
extern uint64_t kvtophys(vm_offset_t va);
extern lck_mtx_t hakios_smmu_lock;

static vm_offset_t smmu, frames, smmu_second, smmu_context, page_tables;
static uint64_t frames_bytes;
static uint32_t smmu_match[IFE_MATCHES], smmu_old_smr[IFE_MATCHES], smmu_old_s2cr[IFE_MATCHES], smmu_context_n, smmu_pages_size,
    smmu_pages;
static bool smmu_on, smmu_matched;

static bool
frames_alloc(uint32_t width, uint32_t height, uint32_t stride, uint32_t source)
{
	struct cam_header *h;
	uint32_t frame_bytes = stride * height;

	frames_bytes = round_page(CAM_HEADER_BYTES + (uint64_t)CAM_FRAMES * frame_bytes);
	if (keep == 0 || keep_bytes < frames_bytes) {
		say("cam: no memory for %u frames (%llu KB; %llu KB taken at boot)\n", CAM_FRAMES, frames_bytes >> 10, keep_bytes >> 10);
		return false;
	}
	frames = keep;
	bzero((void *)frames, CAM_HEADER_BYTES);
	h = (struct cam_header *)frames;
	memcpy(h->magic, "RWCAM001", 8);
	h->width = width;
	h->height = height;
	h->stride = stride;
	h->format = 1;
	h->bayer = 0;                   /* (RGGB: the CSID's test pattern) */
	h->frame_bytes = frame_bytes;
	h->source = source;
	strlcpy(h->sensor, !source ? "CSID TPG" : hw->chip, sizeof(h->sensor));
	/* (what the processor wrote out of its caches; the IFE writes past them) */
	for (vm_offset_t line = frames; line < frames + frames_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	loader_recording[0] = kvtophys(frames);
	loader_recording[1] = frames_bytes;
	loader_recording[2] = (uint64_t)frames;
	say("cam: %llu KB of memory for %u frames at %#llx, handed to the loader for the host\n", frames_bytes >> 10, CAM_FRAMES,
	    loader_recording[0]);
	return true;
}

static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t idr0 = rd(smmu + SMMU_IDR0), idr1 = rd(smmu + SMMU_IDR1), found = 0;

	smmu_pages_size = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu_pages = 2u << (idr1 >> 28 & 7);
	if (2 * smmu_pages * smmu_pages_size != 0x100000) {
		say("cam: the SMMU's layout is not the one known here\n");
		return false;
	}
	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			for (int m = 0; m < IFE_MATCHES; m++) {
				if ((((smr & 0x7fff) ^ ife_stream[m]) & ~((smr >> 16 & 0x7fff) | ife_mask[m])) == 0) {
					say("cam: SMMU match register %u (%#x) is somebody's for the IFEs' streams: left alone\n", n, smr);
					return false;
				}
			}
		} else if (found < IFE_MATCHES) {
			smmu_match[found] = n;
			smmu_old_smr[found] = smr;
			smmu_old_s2cr[found] = s2cr;
			found++;
		}
	}
	for (smmu_context_n = 0; smmu_context_n < (idr1 & 0xff) && used[smmu_context_n]; smmu_context_n++) {
	}
	if (found < IFE_MATCHES || smmu_context_n >= (idr1 & 0xff)) {
		say("cam: the SMMU has not %d match registers and a context left\n", IFE_MATCHES);
		return false;
	}
	smmu_second = ml_io_map(SMMU_PHYS + smmu_pages_size, 0x1000);
	smmu_context = ml_io_map(SMMU_PHYS + (uint64_t)(smmu_pages + smmu_context_n) * smmu_pages_size, 0x1000);
	if (rd(smmu_context + SMMU_CB_SCTLR) & 1) {
		say("cam: SMMU context %u is switched on although no stream uses it: somebody's\n", smmu_context_n);
		return false;
	}
	return true;
}

static bool
smmu_put(vm_offset_t base, uint32_t offset, uint32_t value, uint32_t check)
{
	wr(base + offset, value);
	if ((rd(base + offset) & check) != (value & check)) {
		say("cam: SMMU %#x <- %#x, but it reads %#x\n", offset, value, rd(base + offset));
		give_up("an SMMU register that did not read back");
		return false;
	}
	return true;
}

/* The frames' memory behind FRAME_ADDRESSES for the IFEs' streams, through a free context with tables of ours. */
static bool
smmu_up(void)
{
	uint64_t *root, *second, table_root;
	uint32_t thirds = (uint32_t)((frames_bytes + 0x1fffff) >> 21), pages = (uint32_t)(frames_bytes >> 12);
	bool done = false;

	if (smmu == 0) {
		smmu = ml_io_map(SMMU_PHYS, 0x1000);
	}
	lck_mtx_lock(&hakios_smmu_lock);
	if (!smmu_find()) {
		lck_mtx_unlock(&hakios_smmu_lock);
		give_up("no place in the SMMU");
		return false;
	}
	if (kmem_alloc(kernel_map, &page_tables, (2 + thirds) * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		lck_mtx_unlock(&hakios_smmu_lock);
		give_up("no memory for page tables");
		return false;
	}
	root = (uint64_t *)page_tables;
	second = (uint64_t *)(page_tables + 4096);
	root[FRAME_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	for (uint32_t t = 0; t < thirds; t++) {
		uint64_t *third = (uint64_t *)(page_tables + (2 + t) * 4096);

		second[(FRAME_ADDRESSES >> 21 & 0x1ff) + t] = kvtophys((vm_offset_t)third) | 3;
		for (uint32_t p = 0; p < 512 && t * 512 + p < pages; p++) {
			third[p] = kvtophys(frames + 4096ull * (t * 512 + p)) | CAM_PTE;
		}
	}
	for (vm_offset_t line = page_tables; line < page_tables + (2 + thirds) * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(page_tables) | (uint64_t)smmu_context_n << 48;
	say("cam: SMMU: context %u gets our tables for %llu KB at %#llx; match registers %u to %u send the IFEs' streams through it\n",
	    smmu_context_n, frames_bytes >> 10, FRAME_ADDRESSES, smmu_match[0], smmu_match[IFE_MATCHES - 1]);
	if (smmu_put(smmu_context, SMMU_CB_SCTLR, 0, ~0u) && smmu_put(smmu_second, SMMU_CBA2R(smmu_context_n), 1, 1) &&
	    smmu_put(smmu_second, SMMU_CBAR(smmu_context_n), SMMU_CBAR_STAGE_1, 0x30000) &&
	    smmu_put(smmu_context, SMMU_CB_TCR2, SMMU_TCR2, 0) && smmu_put(smmu_context, SMMU_CB_TCR, SMMU_TCR_39_BITS, ~0u) &&
	    smmu_put(smmu_context, SMMU_CB_TTBR0, (uint32_t)table_root, ~0u) &&
	    smmu_put(smmu_context, SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32), ~0u) &&
	    smmu_put(smmu_context, SMMU_CB_TTBR1, 0, ~0u) && smmu_put(smmu_context, SMMU_CB_TTBR1 + 4, 0, ~0u) &&
	    smmu_put(smmu_context, SMMU_CB_MAIR0, SMMU_MAIR0, ~0u) && smmu_put(smmu_context, SMMU_CB_MAIR1, 0, ~0u)) {
		wr(smmu_context + SMMU_CB_FSR, SMMU_FSR_ALL);
		wr(smmu_context + SMMU_CB_TLBIASID, smmu_context_n);
		wr(smmu_context + SMMU_CB_TLBSYNC, 1);
		for (unsigned waited = 0; rd(smmu_context + SMMU_CB_TLBSTATUS) & 1 && waited <= 1000; waited++) {
			spin_us(10);
		}
		if (rd(smmu_context + SMMU_CB_TLBSTATUS) & 1) {
			give_up("the SMMU does not finish forgetting");
		} else if (smmu_put(smmu_context, SMMU_CB_SCTLR, SMMU_SCTLR_ON, 1)) {
			smmu_on = true;
			done = true;
			for (int m = 0; done && m < IFE_MATCHES; m++) {
				smmu_matched = true;
				done = smmu_put(smmu, SMMU_S2CR(smmu_match[m]), smmu_context_n, 0x300ff) &&
				    smmu_put(smmu, SMMU_SMR(smmu_match[m]), 1u << 31 | (uint32_t)ife_mask[m] << 16 | ife_stream[m], ~0u);
			}
		}
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	return done;
}

static void
smmu_faults(void)
{
	uint32_t status, global;

	if (!smmu_on) {
		return;
	}
	status = rd(smmu_context + SMMU_CB_FSR);
	global = rd(smmu + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("cam: the SMMU has refused the IFE nothing (context %u)\n", smmu_context_n);
		return;
	}
	say("cam: THE SMMU HAS REFUSED A REQUEST: context %u status %#x, address %#llx, syndrome %#x; global status %#x, stream %#x\n",
	    smmu_context_n, status, (uint64_t)rd(smmu_context + SMMU_CB_FAR + 4) << 32 | rd(smmu_context + SMMU_CB_FAR),
	    rd(smmu_context + SMMU_CB_FSYNR0), global, rd(smmu + SMMU_SGFSYNR1));
}

static void
smmu_down(void)
{
	if (!smmu_on && !smmu_matched) {
		return;
	}
	smmu_faults();
	if (!leaving) {
		lck_mtx_lock(&hakios_smmu_lock);
	}
	if (smmu_matched) {
		for (int m = 0; m < IFE_MATCHES; m++) {
			wr(smmu + SMMU_SMR(smmu_match[m]), smmu_old_smr[m]);
			wr(smmu + SMMU_S2CR(smmu_match[m]), smmu_old_s2cr[m]);
		}
	}
	if (smmu_on) {
		wr(smmu_context + SMMU_CB_SCTLR, 0);
		wr(smmu_context + SMMU_CB_TLBIASID, smmu_context_n);
		wr(smmu_context + SMMU_CB_TLBSYNC, 1);
	}
	if (!leaving) {
		lck_mtx_unlock(&hakios_smmu_lock);
	}
	smmu_on = false;
	smmu_matched = false;
	say("cam: the SMMU's match registers and context put back as they were found\n");
}

/* ---- CSID-lite 0 and IFE-lite 0: Titan 175's (csid-lite175, vfe-lite175), laid out as redwood's ---------- */

#define CSID_PHYS               0x0acc8000UL
#define IFE_PHYS                0x0acc4000UL

#define CSID_HW_VERSION         0x000
#define CSID_RST_STROBES        0x010
#define CSID_CSI2_RX_IRQ_STATUS 0x020
#define CSID_CSI2_RX_IRQ_MASK   0x024
#define CSID_CSI2_RX_IRQ_CLEAR  0x028
#define CSID_RDI0_IRQ_STATUS    0x030
#define CSID_RDI0_IRQ_MASK      0x034
#define CSID_RDI0_IRQ_CLEAR     0x038
#define CSID_TOP_IRQ_STATUS     0x070
#define CSID_TOP_IRQ_MASK       0x074
#define CSID_TOP_IRQ_CLEAR      0x078
#define CSID_IRQ_CMD            0x080
#define CSID_CSI2_RX_CFG0       0x100
#define CSID_CSI2_RX_CFG1       0x104
#define CSID_CSI2_RX_PKTS       0x160
#define CSID_CSI2_RX_ECC        0x164
#define CSID_CSI2_RX_CRC        0x168
#define CSID_RDI0_CFG0          0x200
#define CSID_RDI0_CFG1          0x204
#define CSID_RDI0_CTRL          0x208
#define CSID_RDI0_FRM_DROP_PATTERN 0x20c
#define CSID_RDI0_FRM_DROP_PERIOD  0x210
#define CSID_RDI0_IRQ_SUB_PATTERN  0x214
#define CSID_RDI0_IRQ_SUB_PERIOD   0x218
#define CSID_RDI0_PIX_DROP_PATTERN 0x224
#define CSID_RDI0_PIX_DROP_PERIOD  0x228
#define CSID_RDI0_LINE_DROP_PATTERN 0x22c
#define CSID_RDI0_LINE_DROP_PERIOD  0x230
#define CSID_RDI0_STATUS        0x250
#define CSID_RDI0_BYTES_PING    0x2e0
#define CSID_RDI0_BYTES_PONG    0x2e4
#define CSID_TPG_CTRL           0x600
#define CSID_TPG_VC_CFG0        0x604
#define CSID_TPG_VC_CFG1        0x608
#define CSID_TPG_LFSR_SEED      0x60c
#define CSID_TPG_DT0_CFG0       0x610
#define CSID_TPG_DT0_CFG1       0x614
#define CSID_TPG_DT0_CFG2       0x618
#define CSID_TPG_COLOR_BARS_CFG 0x640
#define CSID_TPG_COMMON_GEN_CFG 0x648
#define TPG_VC                  0xa
#define RAW10_DT                0x2b

#define IFE_HW_VERSION          0x0000
#define IFE_GLOBAL_RESET        0x0018
#define IFE_IRQ_MASK0           0x005c
#define IFE_IRQ_CLEAR0          0x0064
#define IFE_IRQ_CLEAR1          0x0068
#define IFE_IRQ_STATUS0         0x006c
#define IFE_IRQ_STATUS1         0x0070
#define IFE_IRQ_GLOBAL_CLEAR    0x0058
#define IFE_VIOLATION_STATUS    0x007c
#define IFE_REG_UPDATE          0x04ac
#define BUS_HW_VERSION          0x2000
#define BUS_CGC_OVD             0x200c
#define BUS_IRQ_MASK1           0x2048
#define BUS_IRQ_STATUS0         0x205c
#define BUS_IRQ_STATUS1         0x2060
#define BUS_IRQ_STATUS2         0x2064
#define BUS_COMP_ERROR          0x206c
#define BUS_ADDR_SYNC_FRAME_HDR 0x2080
#define BUS_ADDR_SYNC_NO_SYNC   0x2084
#define BUS_TEST_CTRL           0x211c
#define WM0_STATUS0             0x2200
#define WM0_STATUS1             0x2204
#define WM0_CFG                 0x2208
#define WM0_IMAGE_ADDR          0x2214
#define WM0_WIDTH               0x221c
#define WM0_HEIGHT              0x2220
#define WM0_PACKER              0x2224
#define WM0_STRIDE              0x2228
#define WM0_IRQ_SUB_PERIOD      0x2248
#define WM0_IRQ_SUB_PATTERN     0x224c
#define WM0_FRAMEDROP_PERIOD    0x2250
#define WM0_FRAMEDROP_PATTERN   0x2254
#define WM0_FRAME_INC           0x2258
#define WM0_BURST_LIMIT         0x225c

static vm_offset_t csid, ife;
static bool csid_running, ife_running, tpg_running;

/* A register of the camera blocks: written, and read back where it is one that keeps what it is given. */
static bool
blk_put(vm_offset_t base, const char *what, uint32_t offset, uint32_t value, bool check)
{
	wr(base + offset, value);
	__asm__ volatile ("dsb sy");
	if (check && rd(base + offset) != value) {
		say("cam: %s %#x <- %#x, but it reads %#x\n", what, offset, value, rd(base + offset));
		give_up("a camera register that did not read back");
		return false;
	}
	return true;
}

static bool
wait_reg(vm_offset_t base, uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t waited = 0; waited <= us; waited += 10) {
		if ((rd(base + offset) & mask) == want) {
			return true;
		}
		spin_us(10);
	}
	return false;
}

/* cam_ife_csid_global_reset() and reset_regs(): interrupts masked and cleared, then the registers, then the hardware. */
static bool
csid_reset(void)
{
	blk_put(csid, "CSID", CSID_CSI2_RX_IRQ_MASK, 0, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_IRQ_MASK + 0x10 * r, 0, false);
	}
	blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
	blk_put(csid, "CSID", CSID_CSI2_RX_IRQ_CLEAR, 0xfffffff, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_IRQ_CLEAR + 0x10 * r, 0x7fff, false);
	}
	blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
	blk_put(csid, "CSID", CSID_CSI2_RX_CFG1, 0x80, false);
	for (unsigned r = 0; r < 4; r++) {
		blk_put(csid, "CSID", CSID_RDI0_CFG0 + 0x100 * r, 2, false);
	}
	for (unsigned hw = 0; hw < 2; hw++) {
		blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		if (hw) {
			blk_put(csid, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		}
		blk_put(csid, "CSID", CSID_RST_STROBES, hw ? 0x1e : 0x1, false);
		if (!hw) {
			blk_put(csid, "CSID", CSID_TOP_IRQ_MASK, 1, false);
			blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
		}
		if (!wait_reg(csid, CSID_TOP_IRQ_STATUS, 1, 1, 100000)) {
			say("cam: the CSID's %s reset does not finish (top irq %#x)\n", hw ? "hardware" : "registers'",
			    rd(csid + CSID_TOP_IRQ_STATUS));
			give_up("a CSID that does not reset");
			return false;
		}
		blk_put(csid, "CSID", CSID_TOP_IRQ_CLEAR, 1, false);
		blk_put(csid, "CSID", CSID_IRQ_CMD, 1, false);
	}
	say("cam: CSID-lite 0 reset (version %#x)\n", rd(csid + CSID_HW_VERSION));
	return true;
}

/* The RX from the test pattern generator and RDI 0, for RAW10 of vc, written as it comes (packed). */
static bool
csid_config_tpg(uint32_t vc, uint32_t width, uint32_t height)
{
	uint32_t cfg0 = vc << 22 | RAW10_DT << 16 | 0u << 27 | 0xfu << 12 | 0u << 10 | 1u << 2 | 3u;

	if (!blk_put(csid, "CSID", CSID_CSI2_RX_CFG0, (4 - 1) | 0x3210u << 4, true) ||
	    !blk_put(csid, "CSID", CSID_CSI2_RX_CFG1, 1u << 6 | 1u, true)) {
		return false;
	}
	/* cam_ife_csid_config_tpg(): one data type, frames without end, blanking 0x3ff lines and 0x740 pixels, RAW10, colour bars */
	if (!blk_put(csid, "CSID", CSID_TPG_VC_CFG0, 0u << 16 | 1u << 10 | vc, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_VC_CFG1, 0x3ffu << 12 | 0x740, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_LFSR_SEED, 0x12345678, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG0, width << 16 | height, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG1, RAW10_DT, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_DT0_CFG2, (3u - 1) << 16 | 0x8, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_COLOR_BARS_CFG, 1u << 5, true) ||
	    !blk_put(csid, "CSID", CSID_TPG_COMMON_GEN_CFG, 0, true)) {
		return false;
	}
	/* cam_ife_csid_init_config_rdi_path() */
	if (!blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0, true) || !blk_put(csid, "CSID", CSID_RDI0_CFG1, 2, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PATTERN, 0, true) || !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PERIOD, 1, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_CTRL, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0 | 1u << 31, true)) {
		return false;
	}
	csid_running = true;
	/* cam_ife_csid_enable_rdi_path(): resume at the frame's boundary; the reset, overflow and count errors seen */
	return blk_put(csid, "CSID", CSID_RDI0_CTRL, 1, true) &&
	       blk_put(csid, "CSID", CSID_RDI0_IRQ_MASK, 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 6, true);
}

static bool
tpg_start(void)
{
	/* cam_ife_csid_tpg_start(): four lanes' worth */
	if (!blk_put(csid, "CSID", CSID_TPG_CTRL, 4u << 20 | 0x80u << 8 | 3u << 4 | 7u, true)) {
		return false;
	}
	tpg_running = true;
	return true;
}

/* cam_vfe_top_reset(), cam_vfe_bus_init_hw(), cam_vfe_bus_start_wm() and update_wm() for RDI 0 in frame mode. */
static bool
ife_config(uint64_t address, uint32_t frame_inc)
{
	blk_put(ife, "IFE", IFE_IRQ_MASK0, 1u << 31, false);
	blk_put(ife, "IFE", IFE_GLOBAL_RESET, 0x3f9f, false);
	if (!wait_reg(ife, IFE_IRQ_STATUS0, 1u << 31, 1u << 31, 100000)) {
		say("cam: the IFE does not reset (irq %#x)\n", rd(ife + IFE_IRQ_STATUS0));
		give_up("an IFE that does not reset");
		return false;
	}
	blk_put(ife, "IFE", IFE_IRQ_CLEAR0, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_CLEAR1, 0xffffffff, false);
	blk_put(ife, "IFE", IFE_IRQ_GLOBAL_CLEAR, 1, false);
	say("cam: IFE-lite 0 reset (version %#x, bus %#x)\n", rd(ife + IFE_HW_VERSION), rd(ife + BUS_HW_VERSION));
	if (!blk_put(ife, "IFE", BUS_ADDR_SYNC_FRAME_HDR, 0, true) || !blk_put(ife, "IFE", BUS_CGC_OVD, 0xfffff, false) ||
	    !blk_put(ife, "IFE", BUS_TEST_CTRL, 0, false) || !blk_put(ife, "IFE", BUS_ADDR_SYNC_NO_SYNC, 0xffffff, false) ||
	    !blk_put(ife, "IFE", WM0_BURST_LIMIT, 0xf, true) || !blk_put(ife, "IFE", WM0_WIDTH, 0xff01, true) ||
	    !blk_put(ife, "IFE", WM0_HEIGHT, 0, true) || !blk_put(ife, "IFE", WM0_PACKER, 0, true) ||
	    !blk_put(ife, "IFE", WM0_STRIDE, 0xff01, true) || !blk_put(ife, "IFE", WM0_FRAMEDROP_PERIOD, 0, true) ||
	    !blk_put(ife, "IFE", WM0_FRAMEDROP_PATTERN, 1, true) || !blk_put(ife, "IFE", WM0_IRQ_SUB_PERIOD, 0, true) ||
	    !blk_put(ife, "IFE", WM0_IRQ_SUB_PATTERN, 1, true) || !blk_put(ife, "IFE", BUS_IRQ_MASK1, 1, false) ||
	    !blk_put(ife, "IFE", WM0_IMAGE_ADDR, (uint32_t)address, false) || !blk_put(ife, "IFE", WM0_FRAME_INC, frame_inc, true) ||
	    !blk_put(ife, "IFE", WM0_CFG, 3, true)) {
		return false;
	}
	ife_running = true;
	/* the RDI 0 resource's start: its register update */
	return blk_put(ife, "IFE", IFE_REG_UPDATE, 0x2, false);
}

static void
show_paths(const char *when)
{
	say("cam: %s: CSID rx irq %#x, packets %u, ecc %#x, crc %u; RDI0 irq %#x status %#x, bytes %u/%u\n", when,
	    rd(csid + CSID_CSI2_RX_IRQ_STATUS), rd(csid + CSID_CSI2_RX_PKTS), rd(csid + CSID_CSI2_RX_ECC), rd(csid + CSID_CSI2_RX_CRC),
	    rd(csid + CSID_RDI0_IRQ_STATUS), rd(csid + CSID_RDI0_STATUS), rd(csid + CSID_RDI0_BYTES_PING), rd(csid + CSID_RDI0_BYTES_PONG));
	say("cam: %s: IFE irq %#x %#x, violation %#x; bus irq %#x %#x %#x, comp error %#x; WM0 status %#x %#x\n", when,
	    rd(ife + IFE_IRQ_STATUS0), rd(ife + IFE_IRQ_STATUS1), rd(ife + IFE_VIOLATION_STATUS), rd(ife + BUS_IRQ_STATUS0),
	    rd(ife + BUS_IRQ_STATUS1), rd(ife + BUS_IRQ_STATUS2), rd(ife + BUS_COMP_ERROR), rd(ife + WM0_STATUS0), rd(ife + WM0_STATUS1));
}

static void
paths_down(void)
{
	if (tpg_running) {
		wr(csid + CSID_TPG_CTRL, 0);
		tpg_running = false;
	}
	if (csid_running) {
		/* cam_ife_csid_disable_rdi_path(): halted at once, then the path off */
		wr(csid + CSID_RDI0_IRQ_MASK, 0);
		wr(csid + CSID_RDI0_CTRL, (rd(csid + CSID_RDI0_CTRL) & ~3u) | 2);
		wr(csid + CSID_RDI0_CFG0, rd(csid + CSID_RDI0_CFG0) & ~(1u << 31));
		wr(csid + CSID_CSI2_RX_CFG0, 0);
		csid_running = false;
	}
	if (ife_running) {
		wr(ife + WM0_CFG, 0);
		wr(ife + IFE_REG_UPDATE, 0x2);
		spin_us(1000);
		ife_running = false;
	}
}

/* The camera's blocks up to their clocks; false (and everything taken back by titan_down()) on any fault. */
static bool
titan_up(void)
{
	if (csid == 0) {
		csid = ml_io_map(CSID_PHYS, 0x1000);
		ife = ml_io_map(IFE_PHYS, 0x4000);
		gcc_b000 = ml_io_map(GCC_PHYS + 0xb000, 0x1000);
	}
	if (csid == 0 || ife == 0 || gcc_b000 == 0) {
		give_up("no mappings for the camera's blocks");
		return false;
	}
	titan_switched = true;
	if (level < 4) {
		return gdsc_on() && clocks_up(false);
	}
	return gdsc_on() && rails_up() && pll0_up() && clocks_up(true);
}

static void
titan_down(void)
{
	if (!titan_switched) {
		return;
	}
	titan_switched = false;
	paths_down();
	smmu_down();
	clocks_down();
	pll0_down();
	if (!powered) {
		gdsc_off();             /* (the sensor's MCLK needs it too: power_down() then switches it off) */
	}
}

/* cam=3: a few frames of the CSID's test pattern through RDI 0 and IFE-lite 0 into memory. */
static void
capture_tpg(void)
{
	const uint32_t width = 1600, height = 1200, stride = width * 10 / 8;
	struct cam_header *h;

	if (!frames_alloc(width, height, stride, 0) || !titan_up() || !smmu_up() || !csid_reset() ||
	    !csid_config_tpg(TPG_VC, width, height) || !ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, stride * height)) {
		titan_down();
		return;
	}
	show_paths("configured");
	if (!tpg_start()) {
		titan_down();
		return;
	}
	h = (struct cam_header *)frames;
	for (unsigned ms = 0; ms < 3000; ms += 100) {
		wait_ms(100);
		if (rd(ife + BUS_IRQ_STATUS1) & 1) {
			h->frames = 1;
			say("cam: write master 0 says its frame is done after %u ms\n", ms + 100);
			break;
		}
	}
	show_paths("after the frames");
	smmu_faults();
	titan_down();
	/* (what the IFE wrote, a look: the first bytes of the frame, past the processor's caches) */
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES + 256; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	const uint8_t *p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	say("cam: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ... (frames done: %u)\n", p[0], p[1], p[2],
	    p[3], p[4], p[5], p[6], p[7], p[8], p[9], h->frames);
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
}

/* ---- cam=4: the sensor's own frames ------------------------------------------------------------------- */

/*
 * The IMX582's tables are Android's, read at run time: tools/camsensor.py makes camera-imx582.tab out of
 * the module file (userland/build.sh puts it into fw/ of the small image). Android's sequence: power up,
 * the chip ID, "init" (408 writes), the mode's table (boot-arg cam_mode=N, default 3: 1920x1080 RAW10 on
 * four D-PHY lanes, 60 frames a second, pixel clock 153.96 MHz); camcc's PLL0 and the clocks at their
 * speeds, the SMMU, CSID-lite 0's RX from CSIPHY 0 and RDI 0, IFE-lite 0's write master as for cam=3,
 * CSIPHY 0 ("qcom,csiphy-v1.2": cam_csiphy_1_2_hwreg.h); then "on", the frames, "off".
 */
struct ctab_head {
	char            magic[8];       /* "RWCTAB02" */
	uint32_t        slave, address_bytes, data_bytes, id_address, id, id_mask, lane_assign, modes;
};
struct ctab_mode {
	uint32_t        width, height, x, y, lanes, cphy, settle, pixel_clock, line_length, frame_length, fps_milli, data_type;
};
struct ctab_entry {
	uint16_t        address, data;
	uint8_t         address_bytes, data_bytes, operation, zero;
	uint32_t        delay_us;
};

static uint32_t delay_ms_tables;

static const struct ctab_head *ctab;
static const struct ctab_mode *ctab_modes;
static const uint8_t *ctab_tables;
static uint64_t ctab_size;

static bool
ctab_load(void)
{
	const uint8_t *at, *end;

	ctab = (const struct ctab_head *)hakios_image_file("fw", hw->tab, &ctab_size, NULL);
	if (ctab == NULL || ctab_size < sizeof(*ctab) || memcmp(ctab->magic, "RWCTAB02", 8) != 0 || ctab->modes == 0 ||
	    ctab->modes > 16) {
		say("cam: no %s (RWCTAB02) in fw/ of the small image (made from fw-camera/ when it is built)\n", hw->tab);
		ctab = NULL;
		return false;
	}
	end = (const uint8_t *)ctab + ctab_size;
	ctab_modes = (const struct ctab_mode *)(ctab + 1);
	at = (const uint8_t *)(ctab_modes + ctab->modes);
	for (int p = 0; p < 2; p++) {
		if (at + 12 > end) {
			ctab = NULL;
			return false;
		}
		at += 12 + *(const uint32_t *)(at + 8) * sizeof(struct ctab_entry);
	}
	ctab_tables = at;
	if (ctab_tables + 4 > end) {
		ctab = NULL;
		return false;
	}
	say("cam: the %s's tables: slave %#x, chip ID %#x at %#x, lanes %#x, %u modes, %u tables\n", hw->chip, ctab->slave, ctab->id,
	    ctab->id_address, ctab->lane_assign, ctab->modes, *(const uint32_t *)ctab_tables);
	return true;
}

static const struct ctab_entry *
ctab_find(const char *name, uint32_t *count)
{
	const uint8_t *at = ctab_tables + 4, *end = (const uint8_t *)ctab + ctab_size;
	uint32_t blocks = *(const uint32_t *)ctab_tables;

	for (uint32_t t = 0; t < blocks && at + 12 <= end; t++) {
		uint32_t n = *(const uint32_t *)(at + 8);

		if (strncmp((const char *)at, name, 8) == 0 && at + 12 + n * sizeof(struct ctab_entry) <= end) {
			*count = n;
			return (const struct ctab_entry *)(at + 12);
		}
		at += 12 + n * sizeof(struct ctab_entry);
	}
	*count = 0;
	return NULL;
}

/* One of the sensor's tables written in order, as Android's sensor driver writes it (each byte acknowledged). */
static bool
sensor_table(const char *name)
{
	uint32_t count;
	const struct ctab_entry *e = ctab_find(name, &count);

	if (e == NULL) {
		say("cam: the tables have no \"%s\"\n", name);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		bool answered;

		if (e[i].operation != 0 || e[i].address_bytes < 1 || e[i].address_bytes > 2 || e[i].data_bytes < 1 || e[i].data_bytes > 2) {
			say("cam: \"%s\" %u: an operation not known here (%u, %u/%u bytes)\n", name, i, e[i].operation, e[i].address_bytes,
			    e[i].data_bytes);
			give_up("a sensor table not understood");
			return false;
		}
		i2c_start();
		answered = !broken && i2c_write((uint8_t)ctab->slave) && (e[i].address_bytes < 2 || i2c_write((uint8_t)(e[i].address >> 8))) &&
		    i2c_write((uint8_t)e[i].address) && (e[i].data_bytes < 2 || i2c_write((uint8_t)(e[i].data >> 8))) &&
		    i2c_write((uint8_t)e[i].data);
		i2c_stop();
		if (!answered) {
			say("cam: \"%s\" %u: %#x <- %#x not acknowledged\n", name, i, e[i].address, e[i].data);
			give_up("a sensor write nobody acknowledged");
			return false;
		}
		if (e[i].delay_us != 0) {
			/* (cam_delay_ms=1: the tables' delays taken as milliseconds -- the S5K3T2's soft reset says 3) */
			uint32_t us = delay_ms_tables ? e[i].delay_us * 1000 : e[i].delay_us;

			wait_ms(us >= 1000 ? (us > 100000 ? 100 : us / 1000) : 0);
			spin_us(us < 1000 ? us : 0);
		}
	}
	say("cam: the sensor's \"%s\": %u writes\n", name, count);
	return true;
}

/* ---- the rails CX and MX, and camcc's PLL0 ---------------------------------------------------------- */

/*
 * The clocks at their speeds want CX at "low" (400 MHz IFE-lite) and MX at "low_l1" (PLL0 at 1200 MHz):
 * both are voted at nominal here (RPMh level 256), and never given back: the boot chain may have voted
 * for them through the same channel, and a vote given back would take its with it.
 */
#define RAIL_LEVEL              256
static bool rails_voted;

static int
rpmh_level_index(const char *name, uint32_t level, uint32_t *address)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	*address = 0;
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; db != NULL && s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *levels;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			*address = entry[i].address;
			if (sizeof(*db) + (size_t)slave->data_offset + entry[i].offset + entry[i].length > cmd_db_size) {
				return -1;
			}
			levels = db->data + slave->data_offset + entry[i].offset;
			for (unsigned l = 0; l < entry[i].length / 2u; l++) {
				uint16_t v = (uint16_t)(levels[2 * l] | levels[2 * l + 1] << 8);

				if (l > 0 && v == 0) {
					break;
				}
				if (v >= level) {
					return (int)l;
				}
			}
			return -1;
		}
	}
	return -1;
}

static bool
rails_up(void)
{
	uint32_t address[2], value[2];
	int cx = rpmh_level_index("cx.lvl", RAIL_LEVEL, &address[0]), mx = rpmh_level_index("mx.lvl", RAIL_LEVEL, &address[1]);

	if (rails_voted) {
		return true;
	}
	if (cx < 0 || mx < 0) {
		say("cam: RPMh has no level %u for CX or MX\n", RAIL_LEVEL);
		give_up("no rail levels");
		return false;
	}
	value[0] = (uint32_t)cx;
	value[1] = (uint32_t)mx;
	if (!rpmh_write(address, value, 2)) {
		give_up("RPMh did not take the camera's rails");
		return false;
	}
	rails_voted = true;
	say("cam: CX (%#x) and MX (%#x) voted at level %u (positions %u and %u), and left so\n", address[0], address[1], RAIL_LEVEL,
	    value[0], value[1]);
	return true;
}

/* camcc's PLL0, a Fabia: clk-alpha-pll.c's offsets, camcc-sdmmagpie.c's 1200 MHz configuration (main 1200, even 600, odd 400) */
#define PLL0_MODE               0x00            /* [0] outputs, [2] out of reset, [23] update logic bypass, [31] locked */
#define PLL0_L_VAL              0x04
#define PLL0_CAL_L_VAL          0x08
#define PLL0_USER_CTL           0x0c            /* [2:0] the outputs, [11:8] even divider, [15:12] odd */
#define PLL0_TEST_CTL           0x1c
#define PLL0_OPMODE             0x2c            /* 0 standby, 1 run */
#define PLL0_FRAC_VAL           0x38
static bool pll0_switched;

static bool
camcc_any(uint32_t offset, uint32_t value, uint32_t check)
{
	wr(camcc + offset, value);
	if ((rd(camcc + offset) & check) != (value & check)) {
		say("cam: camcc %#x <- %#x, but it reads %#x\n", offset, value, rd(camcc + offset));
		give_up("a camcc register that did not read back");
		return false;
	}
	return true;
}

static bool
pll0_up(void)
{
	if (rd(camcc + PLL0_MODE) & 1) {
		give_up("camcc's PLL0 is running already (somebody else's)");
		return false;
	}
	pll0_switched = true;
	/* clk_fabia_pll_configure() */
	if (!camcc_any(PLL0_L_VAL, 0x3e, ~0u) || !camcc_any(PLL0_CAL_L_VAL, 0x3f, ~0u) || !camcc_any(PLL0_FRAC_VAL, 0x8000, ~0u) ||
	    !camcc_any(PLL0_TEST_CTL, 0x40000000, ~0u) ||
	    !camcc_any(PLL0_USER_CTL, (rd(camcc + PLL0_USER_CTL) & ~0xff00u) | 0x31u << 8, 0xff00) ||
	    !camcc_any(PLL0_MODE, rd(camcc + PLL0_MODE) | 1u << 23, 1u << 23)) {
		return false;
	}
	/* clk_fabia_pll_enable(): outputs off, standby, out of reset, run, locked, the outputs on */
	if (!camcc_any(PLL0_MODE, rd(camcc + PLL0_MODE) & ~1u, 1) || !camcc_any(PLL0_OPMODE, 0, 1) ||
	    !camcc_any(PLL0_MODE, rd(camcc + PLL0_MODE) | 1u << 2, 1u << 2) || !camcc_any(PLL0_OPMODE, 1, 1)) {
		return false;
	}
	if (!wait_bits(PLL0_MODE, 1u << 31, 1u << 31, 1000)) {
		say("cam: camcc's PLL0 does not lock (mode %#x)\n", rd(camcc + PLL0_MODE));
		give_up("a PLL that does not lock");
		return false;
	}
	if (!camcc_any(PLL0_USER_CTL, rd(camcc + PLL0_USER_CTL) | 7, 7) || !camcc_any(PLL0_MODE, rd(camcc + PLL0_MODE) | 1, 1)) {
		return false;
	}
	say("cam: camcc's PLL0 locked at 1200 MHz (mode %#x, user_ctl %#x)\n", rd(camcc + PLL0_MODE), rd(camcc + PLL0_USER_CTL));
	return true;
}

static void
pll0_down(void)
{
	if (!pll0_switched) {
		return;
	}
	pll0_switched = false;
	wr(camcc + PLL0_MODE, rd(camcc + PLL0_MODE) & ~1u);
	wr(camcc + PLL0_USER_CTL, rd(camcc + PLL0_USER_CTL) & ~7u);
	wr(camcc + PLL0_OPMODE, 0);
	wr(camcc + PLL0_MODE, rd(camcc + PLL0_MODE) & ~(1u << 2));
	say("cam: camcc's PLL0 off (mode %#x)\n", rd(camcc + PLL0_MODE));
}

/* ---- CSIPHY 0, v1.2 (D-PHY) ------------------------------------------------------------------------------ */

#define CSIPHY_PHYS(n)          (0x0ace0000UL + 0x2000UL * (n))
static vm_offset_t csiphy;
static bool csiphy_running;

/* csiphy_2ph_v1_2_reg: for each lane position (data lane 0, the clock lane, data lanes 1, 2, 3) { address, value, delay ms, kind } */
static const struct {
	uint16_t        address;
	uint8_t         value, delay;
	char            kind;           /* D a value, L the settle count's low byte, N nothing */
} dphy12[5][23] = {
	{ /* lane position 0 */
		{ 0x0030, 0x00, 0x00, 'D' }, { 0x0904, 0x00, 0x00, 'D' }, { 0x0910, 0x00, 0x00, 'D' }, { 0x0900, 0x0f, 0x00, 'D' }, { 0x0908, 0x06, 0x00, 'D' }, { 0x0904, 0x07, 0x00, 'D' }, { 0x002c, 0x01, 0x00, 'D' }, { 0x0034, 0x0f, 0x00, 'D' }, { 0x0010, 0x50, 0x00, 'D' }, { 0x001c, 0x0a, 0x00, 'D' }, { 0x0014, 0x60, 0x00, 'D' }, { 0x0028, 0x00, 0x00, 'D' }, { 0x003c, 0xb8, 0x00, 'D' }, { 0x0000, 0x91, 0x00, 'D' }, { 0x0004, 0x0c, 0x00, 'D' }, { 0x0020, 0x00, 0x00, 'D' }, { 0x0008, 0x10, 0x00, 'L' }, { 0x0010, 0x52, 0x00, 'D' }, { 0x0038, 0xfe, 0x00, 'D' }, { 0x005c, 0xc0, 0x00, 'D' }, { 0x0060, 0x0d, 0x00, 'D' }, { 0x0800, 0x02, 0x00, 'D' }, { 0x0000, 0x00, 0x00, 'N' } },
	{ /* lane position 1 */
		{ 0x0730, 0x00, 0x00, 'D' }, { 0x0c84, 0x00, 0x00, 'D' }, { 0x0c90, 0x00, 0x00, 'D' }, { 0x0c80, 0x0f, 0x00, 'D' }, { 0x0c88, 0x06, 0x00, 'D' }, { 0x0c84, 0x07, 0x00, 'D' }, { 0x072c, 0x01, 0x00, 'D' }, { 0x0734, 0x0f, 0x00, 'D' }, { 0x0710, 0x50, 0x00, 'D' }, { 0x071c, 0x0a, 0x00, 'D' }, { 0x0714, 0x60, 0x00, 'D' }, { 0x0728, 0x04, 0x00, 'D' }, { 0x073c, 0xb8, 0x00, 'D' }, { 0x0700, 0x80, 0x00, 'D' }, { 0x0704, 0x0c, 0x00, 'D' }, { 0x0720, 0x00, 0x00, 'D' }, { 0x0708, 0x04, 0x00, 'L' }, { 0x070c, 0xff, 0x00, 'D' }, { 0x0710, 0x52, 0x00, 'D' }, { 0x0738, 0x1f, 0x00, 'D' }, { 0x0800, 0x02, 0x00, 'D' }, { 0x0000, 0x00, 0x00, 'N' }, { 0x0000, 0x00, 0x00, 'N' } },
	{ /* lane position 2 */
		{ 0x0230, 0x00, 0x00, 'D' }, { 0x0a04, 0x00, 0x00, 'D' }, { 0x0a10, 0x00, 0x00, 'D' }, { 0x0a00, 0x0f, 0x00, 'D' }, { 0x0a08, 0x06, 0x00, 'D' }, { 0x0a04, 0x07, 0x00, 'D' }, { 0x022c, 0x01, 0x00, 'D' }, { 0x0234, 0x0f, 0x00, 'D' }, { 0x0210, 0x50, 0x00, 'D' }, { 0x021c, 0x0a, 0x00, 'D' }, { 0x0214, 0x60, 0x00, 'D' }, { 0x0228, 0x00, 0x00, 'D' }, { 0x023c, 0xb8, 0x00, 'D' }, { 0x0200, 0x91, 0x00, 'D' }, { 0x0204, 0x0c, 0x00, 'D' }, { 0x0220, 0x00, 0x00, 'D' }, { 0x0208, 0x04, 0x00, 'L' }, { 0x0210, 0x52, 0x00, 'D' }, { 0x0238, 0xfe, 0x00, 'D' }, { 0x025c, 0xc0, 0x00, 'D' }, { 0x0260, 0x0d, 0x00, 'D' }, { 0x0800, 0x02, 0x00, 'D' }, { 0x0000, 0x00, 0x00, 'N' } },
	{ /* lane position 3 */
		{ 0x0430, 0x00, 0x00, 'D' }, { 0x0b04, 0x00, 0x00, 'D' }, { 0x0b10, 0x00, 0x00, 'D' }, { 0x0b00, 0x0f, 0x00, 'D' }, { 0x0b08, 0x06, 0x00, 'D' }, { 0x0b04, 0x07, 0x00, 'D' }, { 0x042c, 0x01, 0x00, 'D' }, { 0x0434, 0x0f, 0x00, 'D' }, { 0x0410, 0x50, 0x00, 'D' }, { 0x041c, 0x0a, 0x00, 'D' }, { 0x0414, 0x60, 0x00, 'D' }, { 0x0428, 0x00, 0x00, 'D' }, { 0x043c, 0xb8, 0x00, 'D' }, { 0x0400, 0x91, 0x00, 'D' }, { 0x0404, 0x0c, 0x00, 'D' }, { 0x0420, 0x00, 0x00, 'D' }, { 0x0408, 0x04, 0x00, 'L' }, { 0x0410, 0x52, 0x00, 'D' }, { 0x0438, 0xfe, 0x00, 'D' }, { 0x045c, 0xc0, 0x00, 'D' }, { 0x0460, 0x0d, 0x00, 'D' }, { 0x0800, 0x02, 0x00, 'D' }, { 0x0000, 0x00, 0x00, 'N' } },
	{ /* lane position 4 */
		{ 0x0630, 0x00, 0x00, 'D' }, { 0x0c04, 0x00, 0x00, 'D' }, { 0x0c10, 0x00, 0x00, 'D' }, { 0x0c00, 0x0f, 0x00, 'D' }, { 0x0c08, 0x06, 0x00, 'D' }, { 0x0c04, 0x07, 0x00, 'D' }, { 0x062c, 0x01, 0x00, 'D' }, { 0x0634, 0x0f, 0x00, 'D' }, { 0x0610, 0x50, 0x00, 'D' }, { 0x061c, 0x0a, 0x00, 'D' }, { 0x0614, 0x60, 0x00, 'D' }, { 0x0628, 0x00, 0x00, 'D' }, { 0x063c, 0xb8, 0x00, 'D' }, { 0x0600, 0x91, 0x00, 'D' }, { 0x0604, 0x0c, 0x00, 'D' }, { 0x0620, 0x00, 0x00, 'D' }, { 0x0608, 0x04, 0x00, 'L' }, { 0x0610, 0x52, 0x00, 'D' }, { 0x0638, 0xfe, 0x00, 'D' }, { 0x065c, 0xc0, 0x00, 'D' }, { 0x0660, 0x0d, 0x00, 'D' }, { 0x0800, 0x02, 0x00, 'D' }, { 0x0000, 0x00, 0x00, 'N' } },
};

static bool
csiphy_up(unsigned lanes, uint32_t lane_assign, unsigned settle)
{
	static const uint8_t irq_masks[11] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };
	uint32_t lane_enable = 0, lane_mask = 1u << 1;

	static vm_offset_t csiphys[4];

	if (csiphys[hw->csiphy] == 0 && (csiphys[hw->csiphy] = ml_io_map(CSIPHY_PHYS(hw->csiphy), 0x2000)) == 0) {
		return false;
	}
	csiphy = csiphys[hw->csiphy];
	/* lane_mask: data lane n at bit 0, 2, 3, 4 (bit 1 the clock lane); lane_enable: data lane n at bit 2n, the clock lane at 7 */
	for (unsigned l = 0; l < lanes; l++, lane_assign >>= 4) {
		unsigned n = lane_assign & 0xf;

		if (n > 3) {
			give_up("a lane assignment not known here");
			return false;
		}
		lane_mask |= n == 0 ? 1u : 1u << (n + 1);
		lane_enable |= 1u << (2 * n);
	}
	lane_enable |= 0x80;
	/* csiphy_reset_reg_1_2 */
	wr(csiphy + 0x814, 0x00);
	spin_us(5000);
	wr(csiphy + 0x818, 0x00);
	wr(csiphy + 0x81c, 0x00);
	wr(csiphy + 0x800, 0x01);
	spin_us(1000);
	wr(csiphy + 0x800, 0x00);
	csiphy_running = true;
	/* csiphy_common_reg_1_2, two-phase */
	if (!blk_put(csiphy, "CSIPHY", 0x814, lane_enable, true) || !blk_put(csiphy, "CSIPHY", 0x818, 0x01, true) ||
	    !blk_put(csiphy, "CSIPHY", 0x81c, 0x52, true)) {
		return false;
	}
	wr(csiphy + 0x800, 0x03);
	spin_us(1000);
	wr(csiphy + 0x800, 0x02);
	for (unsigned pos = 0; pos < 5; pos++) {
		if (!(lane_mask & 1u << pos)) {
			continue;
		}
		for (unsigned i = 0; i < 23; i++) {
			uint32_t v = dphy12[pos][i].kind == 'L' ? (settle & 0xff) : dphy12[pos][i].value;

			if (dphy12[pos][i].kind == 'N') {
				continue;
			}
			wr(csiphy + dphy12[pos][i].address, v);
			if (dphy12[pos][i].delay) {
				spin_us(1000u * dphy12[pos][i].delay);
			}
		}
	}
	for (unsigned i = 0; i < 11; i++) {
		wr(csiphy + 0x82c + 4 * i, irq_masks[i]);
		wr(csiphy + 0x858 + 4 * i, 0xff);
	}
	wr(csiphy + 0x828, 0x1);
	wr(csiphy + 0x828, 0x0);
	say("cam: CSIPHY %u configured: %u D-PHY lanes (positions %#x, enabled %#x), settle count %u\n", hw->csiphy, lanes, lane_mask,
	    lane_enable, settle);
	return true;
}

static void
show_csiphy(const char *when)
{
	char line[200];
	int at = snprintf(line, sizeof(line), "cam: CSIPHY %u %s, interrupt status:", hw->csiphy, when);

	for (unsigned i = 0; i < 11 && at < (int)sizeof(line) - 6; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x", rd(csiphy + 0x8b0 + 4 * i) & 0xff);
	}
	say("%s\n", line);
}

static void
csiphy_down(void)
{
	if (!csiphy_running) {
		return;
	}
	csiphy_running = false;
	wr(csiphy + 0x814, 0x00);
	spin_us(5);
	wr(csiphy + 0x818, 0x00);
	wr(csiphy + 0x81c, 0x00);
	wr(csiphy + 0x800, 0x01);
	spin_us(1);
	wr(csiphy + 0x800, 0x00);
}

/* CSID-lite 0's RX from CSIPHY 0 (D-PHY, `lanes' of `lane_cfg') and RDI 0 for RAW10 of virtual channel 0 */
static bool
csid_config_phy(unsigned lanes, uint32_t lane_cfg)
{
	uint32_t cfg0 = 0u << 22 | RAW10_DT << 16 | 0xfu << 12 | 1u << 2 | 3u;

	if (!blk_put(csid, "CSID", CSID_CSI2_RX_CFG0, (lanes - 1) | lane_cfg << 4 | (uint32_t)hw->csiphy << 20, true) ||
	    !blk_put(csid, "CSID", CSID_CSI2_RX_CFG1, 1u << 6 | 1u, true) || !blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_CFG1, 2, true) || !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PERIOD, 1, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_FRM_DROP_PATTERN, 0, true) || !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PERIOD, 1, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_IRQ_SUB_PATTERN, 0, true) || !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_PIX_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PATTERN, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_LINE_DROP_PERIOD, 1, true) || !blk_put(csid, "CSID", CSID_RDI0_CTRL, 0, true) ||
	    !blk_put(csid, "CSID", CSID_RDI0_CFG0, cfg0 | 1u << 31, true)) {
		return false;
	}
	csid_running = true;
	return blk_put(csid, "CSID", CSID_RDI0_CTRL, 1, true) &&
	       blk_put(csid, "CSID", CSID_RDI0_IRQ_MASK, 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 6, true);
}

static bool
sensor_write8(uint16_t reg, uint8_t value)
{
	bool answered;

	i2c_start();
	answered = !broken && i2c_write((uint8_t)ctab->slave) && i2c_write((uint8_t)(reg >> 8)) && i2c_write((uint8_t)reg) &&
	    i2c_write(value);
	i2c_stop();
	if (!answered) {
		say("cam: %#x <- %#x not acknowledged\n", reg, value);
		give_up("a sensor write nobody acknowledged");
	}
	return answered;
}

/*
 * The exposure, which Android's camera stack writes after the tables (they leave it at nothing: a frame of
 * black level): the module file's registers, the coarse integration time in lines at 0x0202/0x0203 and the
 * analog gain at 0x0204/0x0205 (the IMX582's code: gain = 1024 / (1024 - code)), between group hold on and
 * off. Boot-args cam_shutter=LINES (default the frame's length less 10) and cam_gain=CODE (default 896, 8x).
 */
static bool
exposure(const struct ctab_mode *mode)
{
	uint32_t lines = mode->frame_length > 20 ? mode->frame_length - 10 : 1, gain = hw->samsung ? 8 * 32 : 896;

	(void)PE_parse_boot_argn("cam_shutter", &lines, sizeof(lines));
	(void)PE_parse_boot_argn("cam_gain", &gain, sizeof(gain));
	lines = lines > 0xffff ? 0xffff : lines;
	gain = hw->samsung ? (gain > 16 * 32 ? 16 * 32 : gain < 32 ? 32 : gain) : gain > 1008 ? 1008 : gain;
	if (!sensor_table("holdon") || !sensor_write8(0x0202, (uint8_t)(lines >> 8)) || !sensor_write8(0x0203, (uint8_t)lines) ||
	    !sensor_write8(0x0204, (uint8_t)(gain >> 8)) || !sensor_write8(0x0205, (uint8_t)gain) || !sensor_table("holdoff")) {
		return false;
	}
	say("cam: exposure %u lines of %u, analog gain %u (%u.%02ux)\n", lines, mode->frame_length, gain,
	    hw->samsung ? gain / 32 : 1024 / (1024 - gain), hw->samsung ? gain * 100 / 32 % 100 : 1024 * 100 / (1024 - gain) % 100);
	return true;
}

static void
capture_sensor(void)
{
	uint32_t mode_n = 3, settle = 0, wanted = 30, width, height, stride;
	const struct ctab_mode *mode;
	struct cam_header *h;
	char res[8];

	if (!ctab_load()) {
		return;
	}
	(void)PE_parse_boot_argn("cam_delay_ms", &delay_ms_tables, sizeof(delay_ms_tables));
	mode_n = hw->samsung ? 1 : 3;           /* (the front one's 2592x1940, the main one's 1920x1080) */
	(void)PE_parse_boot_argn("cam_mode", &mode_n, sizeof(mode_n));
	mode_n = mode_n < ctab->modes ? mode_n : ctab->modes - 1;
	mode = &ctab_modes[mode_n];
	snprintf(res, sizeof(res), "res%u", mode_n);
	width = mode->width;
	height = mode->height;
	stride = (width * 10 / 8 + 15) & ~15u;
	(void)PE_parse_boot_argn("cam_frames", &wanted, sizeof(wanted));
	wanted = wanted < 1 ? 1 : wanted > 300 ? 300 : wanted;
	if (!PE_parse_boot_argn("cam_settle", &settle, sizeof(settle)) || settle == 0) {
		settle = mode->settle;
	}
	if (mode->cphy || mode->lanes < 1 || mode->lanes > 4 || mode->data_type != RAW10_DT || width % 4 != 0) {
		say("cam: mode %u: %u lanes, data type %#x: not what this is written for\n", mode_n, mode->lanes, mode->data_type);
		return;
	}
	say("cam: mode %u: %ux%u, %u D-PHY lanes, %u.%03u frames/s, settle count %u\n", mode_n, width, height, mode->lanes,
	    mode->fps_milli / 1000, mode->fps_milli % 1000, settle);
	if (!frames_alloc(width, height, stride, 1)) {
		return;
	}
	h = (struct cam_header *)frames;
	h->bayer = hw->bayer;           /* (the module file's colorFilterArrangement: the IMX582's RGGB, the S5K3T2's GRBG) */
	if (!power_up() || !read_chip_id() || !sensor_table("init") || !sensor_table(res) || !titan_up() ||
	    !smmu_up() || !csid_reset() || !csid_config_phy(mode->lanes, ctab->lane_assign & ((1u << (4 * mode->lanes)) - 1)) ||
	    !ife_config(FRAME_ADDRESSES + CAM_HEADER_BYTES, stride * height) || !csiphy_up(mode->lanes, ctab->lane_assign, settle)) {
		csiphy_down();
		titan_down();
		power_down();
		return;
	}
	show_paths("configured");
	if (exposure(mode) && sensor_table("on")) {
		if (hw->samsung) {
			uint16_t before = 0, after = 0;

			(void)sensor_read16(hw->slave, 0x0004, &before);
			wait_ms(200);
			(void)sensor_read16(hw->slave, 0x0004, &after);
			say("cam: the sensor's frame counter (0x0005): %u, 200 ms later %u\n", before & 0xff, after & 0xff);
		}
		uint32_t sweep = 0;

		if (PE_parse_boot_argn("cam_phy_sweep", &sweep, sizeof(sweep)) && sweep != 0) {
			/* the CSID's PHY selection 0..3, 300 ms each: which one brings packets */
			uint32_t cfg0 = rd(csid + CSID_CSI2_RX_CFG0);

			for (uint32_t sel = 0; sel < 4; sel++) {
				uint32_t was = rd(csid + CSID_CSI2_RX_PKTS);

				wr(csid + CSID_CSI2_RX_CFG0, (cfg0 & ~(3u << 20)) | sel << 20);
				wait_ms(300);
				say("cam: the CSID from PHY %u: %u packets in 300 ms (rx irq %#x)\n", sel, rd(csid + CSID_CSI2_RX_PKTS) - was,
				    rd(csid + CSID_CSI2_RX_IRQ_STATUS));
			}
			wr(csid + CSID_CSI2_RX_CFG0, cfg0);
		}
		/* frames one after the other into the two places, the last of `wanted' kept */
		for (unsigned ms = 0, done = 0; ms < 5000 && done < wanted; ms += 2) {
			spin_us(2000);
			if (!(rd(ife + BUS_IRQ_STATUS1) & 1)) {
				continue;
			}
			done++;
			if (done <= 3 || done == wanted) {
				say("cam: frame %u written, %u ms after the sensor was started\n", done, ms + 2);
			}
			wr(ife + 0x2054, 1);            /* BUS_IRQ_CLEAR1 */
			wr(ife + 0x2068, 1);            /* BUS_IRQ_GLOBAL_CLEAR */
			if (done < wanted) {
				wr(ife + WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + CAM_HEADER_BYTES + (uint64_t)(done & 1) * stride * height));
				wr(ife + IFE_REG_UPDATE, 0x2);
			}
			h->frames = done < CAM_FRAMES ? done : CAM_FRAMES;
			h->last = (done - 1) & 1;
		}
		show_paths("after the frames");
		show_csiphy("after the frames");
		(void)sensor_table("off");
	}
	smmu_faults();
	csiphy_down();
	titan_down();
	power_down();
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES + 256; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	const uint8_t *p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	say("cam: the frame's first bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x ... (frames done: %u)\n", p[0], p[1], p[2],
	    p[3], p[4], p[5], p[6], p[7], p[8], p[9], h->frames);
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
}

/* ---- cam=5: the main camera for iOS (/dev/hakios-camera) ------------------------------------------- */

/*
 * The IMX582's frames for a program (iOS's camera stack in mediaserverd, libhakios-media.dylib), for as
 * long as it holds /dev/hakios-camera open (sys/hakios.camera.h), as hakios.camera.c gives redwood's: the sensor
 * powered up and streaming at the first read after the open, everything down again at the last close,
 * after IDLE_MS without a read, and on any fault (and then for the rest of the run). The sequence is
 * cam=4's. The frames go into three places in turn (kernel memory, mapped page by page through the
 * SMMU): one the IFE writes, one a read may be copying, one ready; at each start of frame the address
 * of the place for the frame after it goes to the write master (taken at the next one).
 *
 * The frames say they are sensor 1's (what userland's back camera asks for); the other sensors are not
 * here yet: HAKIOS_CAMERA_SENSOR for them changes nothing. There is no lens motor yet (HAKIOS_CAMERA_NO_FOCUS).
 * The gain the program sees is linear (GAIN_UNITY is 1x); the IMX582's analog gain code is
 * 1024 - 1024 / gain (gain_code()), at most 16x.
 */
#define RING                    3
#define PLACE_SPARE             (256u << 10)    /* room after each frame: some come longer than the mode's lines */
#define IDLE_MS                 1000
#define FRAME_TIMEOUT_MS        1500
#define EXPOSURE_MARGIN         40
#define GAIN_UNITY              256
#define GAIN_MAX                (16 * GAIN_UNITY)
#define FRAME_LINES_MAX         0xfff0
#define RDI0_SOF                (1u << 27)      /* IFE_IRQ_STATUS0 (cam_vfe_lite17x.h: sof_irq_mask of RDI 0) */
#define WM0_DONE                1u              /* BUS_IRQ_STATUS1 */
#define BUS_IRQ_CLEAR1          0x2054
#define BUS_IRQ_GLOBAL_CLEAR    0x2068

enum { PLACE_FREE, PLACE_WRITING, PLACE_READY, PLACE_READING };

static LCK_GRP_DECLARE(ios_lck_grp, "hakios-camera");
static LCK_MTX_DECLARE(ios_lock, &ios_lck_grp);
static struct {
	uint8_t         state;
	uint64_t        sequence, time;
	uint32_t        exposure_lines, gain, frame_lines;
} place[RING];
static vm_offset_t ring;
static uint64_t ring_bytes, ring_tables_bytes, ring_place_bytes;
static uint32_t ring_frame_bytes, ios_odd_frames, ios_stride;
static const struct ctab_mode *ios_mode;
static uint32_t ios_mode_n, ios_line_ns;
static volatile bool ios_open, ios_streaming, ios_wanted;  /* open; frames coming; a read wants them */
static uint64_t ios_last_read, ios_given, ios_frames_done, ios_writes_late;
static struct { uint32_t lines, gain, frame_lines; bool pending; } ios_ask;
static struct { uint32_t lines, gain, frame_lines; uint64_t from; } ios_now, ios_next;
static volatile unsigned ios_switch_to;         /* HAKIOS_CAMERA_SENSOR: 1 + the sensor to change to, between sessions */

static uint64_t
ios_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

/* exposure_lines, gain, frame_lines within what the sensor takes (frame_lines 0: the shortest that holds the exposure) */
static void
exposure_limits(uint32_t *lines, uint32_t *gain, uint32_t *frame_lines)
{
	uint32_t shortest = ios_mode->frame_length;

	*gain = *gain < GAIN_UNITY ? GAIN_UNITY : *gain > GAIN_MAX ? GAIN_MAX : *gain;
	*lines = *lines < 4 ? 4 : *lines > FRAME_LINES_MAX - EXPOSURE_MARGIN ? FRAME_LINES_MAX - EXPOSURE_MARGIN : *lines;
	if (*frame_lines == 0) {
		*frame_lines = *lines + EXPOSURE_MARGIN > shortest ? *lines + EXPOSURE_MARGIN : shortest;
	}
	*frame_lines = *frame_lines < shortest ? shortest : *frame_lines > FRAME_LINES_MAX ? FRAME_LINES_MAX : *frame_lines;
	if (*lines > *frame_lines - EXPOSURE_MARGIN) {
		*lines = *frame_lines - EXPOSURE_MARGIN;
	}
}

/* a linear gain (GAIN_UNITY is 1x) as the IMX582's analog gain code */
static uint32_t
gain_code(uint32_t gain)
{
	return 1024 - 1024 * GAIN_UNITY / gain;
}

/* The exposure, gain and frame length in the sensor's group hold: 0x0340 frame length, 0x0202 lines, 0x0204 gain code */
static bool
exposure_write(uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	uint32_t code = hw->samsung ? gain * 32 / GAIN_UNITY : gain_code(gain);       /* (the S5K3T2's: 32 is 1x) */

	if (!sensor_table("holdon") || !sensor_write8(0x0340, (uint8_t)(frame_lines >> 8)) || !sensor_write8(0x0341, (uint8_t)frame_lines) ||
	    !sensor_write8(0x0202, (uint8_t)(lines >> 8)) || !sensor_write8(0x0203, (uint8_t)lines) ||
	    !sensor_write8(0x0204, (uint8_t)(code >> 8)) || !sensor_write8(0x0205, (uint8_t)code) || !sensor_table("holdoff")) {
		give_up("a sensor write nobody acknowledged");
		return false;
	}
	return true;
}

static void
ring_free(void)
{
	if (page_tables != 0) {
		kmem_free(kernel_map, page_tables, ring_tables_bytes);
		page_tables = 0;
	}
	if (ring != 0) {
		kmem_free(kernel_map, ring, ring_bytes);
		ring = 0;
	}
	frames = 0;
	frames_bytes = 0;
}

static bool
ring_alloc(uint32_t frame_bytes)
{
	ring_frame_bytes = frame_bytes;
	ring_place_bytes = round_page(frame_bytes) + PLACE_SPARE;
	ring_bytes = RING * ring_place_bytes;
	if (kmem_alloc(kernel_map, &ring, ring_bytes, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("cam: no memory for %u frames (%llu KB)\n", RING, ring_bytes >> 10);
		ring = 0;
		return false;
	}
	for (vm_offset_t line = ring; line < ring + ring_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	frames = ring;                  /* (what smmu_up() maps) */
	frames_bytes = ring_bytes;
	ring_tables_bytes = (2 + ((ring_bytes + 0x1fffff) >> 21)) * 4096;
	return true;
}

/* The session's end, whatever ended it: the sensor's "off", the blocks, the supplies, the memory. */
static void
ios_stop(const char *why)
{
	lck_mtx_lock(&ios_lock);
	ios_streaming = false;
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	lck_mtx_unlock(&ios_lock);
	wakeup(&ios_frames_done);
	if (!broken && ctab != NULL) {
		(void)sensor_table("off");
	}
	if (csid_running || ife_running) {
		show_paths("at the end");
	}
	smmu_faults();
	csiphy_down();
	titan_down();
	power_down();
	ring_free();
	say("cam: the frames for iOS stopped (%s): %llu done, %llu given, %llu written over\n", why, ios_frames_done, ios_given,
	    ios_writes_late);
}

static bool
ios_start(void)
{
	uint32_t lines, gain, frame_lines;
	char res[8];

	ios_frames_done = ios_given = ios_writes_late = 0;
	ios_odd_frames = 0;
	snprintf(res, sizeof(res), "res%u", ios_mode_n);
	if (!ring_alloc(ios_stride * ios_mode->height)) {
		return false;
	}
	if (!power_up() || !read_chip_id() || !sensor_table("init") || !sensor_table(res) || !titan_up() || !smmu_up() || !csid_reset() ||
	    !csid_config_phy(ios_mode->lanes, ctab->lane_assign & ((1u << (4 * ios_mode->lanes)) - 1)) ||
	    !ife_config(FRAME_ADDRESSES, 0) || !csiphy_up(ios_mode->lanes, ctab->lane_assign, ios_mode->settle)) {
		return false;
	}
	lck_mtx_lock(&ios_lock);
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	place[0].state = PLACE_WRITING;
	lines = ios_ask.lines;
	gain = ios_ask.gain;
	frame_lines = ios_ask.frame_lines;
	ios_ask.pending = false;
	lck_mtx_unlock(&ios_lock);
	exposure_limits(&lines, &gain, &frame_lines);
	if (!exposure_write(lines, gain, frame_lines) || !sensor_table("on")) {
		return false;
	}
	ios_now.lines = ios_next.lines = lines;
	ios_now.gain = ios_next.gain = gain;
	ios_now.frame_lines = ios_next.frame_lines = frame_lines;
	ios_next.from = 0;
	say("cam: the frames for iOS: mode %u, %ux%u, %u lines of %u ns a frame at the shortest; exposure %u lines, gain %u/%u, "
	    "frame %u lines\n", ios_mode_n, ios_mode->width, ios_mode->height, ios_mode->frame_length, ios_line_ns, lines, gain, GAIN_UNITY,
	    frame_lines);
	return true;
}

static void
ios_session(void)
{
	int writing = 0, next = 0;      /* (place 0's address is the write master's from ife_config(): taken at the first SOF) */
	uint64_t last_frame;

	if (!ios_start()) {
		give_up("the camera did not start");
		ios_stop("it did not start");
		return;
	}
	last_frame = ios_ms();
	wr(ife + IFE_IRQ_CLEAR0, 0xffffffff);
	wr(ife + IFE_IRQ_GLOBAL_CLEAR, 1);
	lck_mtx_lock(&ios_lock);
	ios_streaming = true;
	ios_last_read = ios_ms();
	lck_mtx_unlock(&ios_lock);
	while (!broken && ios_open && ios_switch_to == 0) {
		uint32_t top, bus;
		uint64_t now;

		wait_ms(1);
		now = ios_ms();
		top = rd(ife + IFE_IRQ_STATUS0);
		bus = rd(ife + BUS_IRQ_STATUS1);
		if (!(top & RDI0_SOF) && !(bus & WM0_DONE)) {
			if (now - last_frame > FRAME_TIMEOUT_MS) {
				show_paths("no frame");
				give_up("no frame for 1.5 s");
				break;
			}
			if (now - ios_last_read > IDLE_MS) {
				break;
			}
			continue;
		}
		wr(ife + IFE_IRQ_CLEAR0, top & RDI0_SOF);
		wr(ife + IFE_IRQ_GLOBAL_CLEAR, 1);
		wr(ife + BUS_IRQ_CLEAR1, bus & WM0_DONE);
		wr(ife + BUS_IRQ_GLOBAL_CLEAR, 1);
		lck_mtx_lock(&ios_lock);
		if (bus & WM0_DONE) {
			uint32_t ping = rd(csid + CSID_RDI0_BYTES_PING), pong = rd(csid + CSID_RDI0_BYTES_PONG);
			uint32_t data = ios_mode->width * 10 / 8 * ios_mode->height;

			last_frame = now;
			ios_frames_done++;
			if ((ping != data || (pong != data && ios_frames_done > 1)) && ios_odd_frames++ < 8) {
				say("cam: frame %llu: the CSID counted %u and %u bytes (the mode's frame is %u)\n", ios_frames_done, ping, pong, data);
			}
			if (ios_frames_done >= ios_next.from && ios_next.from != 0) {
				ios_now = ios_next;
				ios_next.from = 0;
			}
			place[writing].state = PLACE_READY;
			place[writing].sequence = ios_frames_done;
			place[writing].time = mach_absolute_time();
			place[writing].exposure_lines = ios_now.lines;
			place[writing].gain = ios_now.gain;
			place[writing].frame_lines = ios_now.frame_lines;
		}
		if (top & RDI0_SOF) {
			if (next >= 0) {
				writing = next;         /* (the address given at the last SOF, taken at this one) */
				next = -1;
			} else {
				ios_writes_late++;      /* no address waiting: this frame goes where the last one went */
			}
			place[writing].state = PLACE_WRITING;
			for (int i = 0; i < RING; i++) {
				int c = (writing + 1 + i) % RING;

				if (c != writing && place[c].state != PLACE_READING && (next < 0 || place[c].sequence < place[next].sequence)) {
					next = c;
				}
			}
			if (next >= 0) {
				wr(ife + WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + (uint64_t)next * ring_place_bytes));
				wr(ife + IFE_REG_UPDATE, 0x2);
			}
		}
		bool ask = (bus & WM0_DONE) && ios_ask.pending;
		uint32_t lines = ios_ask.lines, gain = ios_ask.gain, frame_lines = ios_ask.frame_lines;

		ios_ask.pending = ios_ask.pending && !ask;
		lck_mtx_unlock(&ios_lock);
		if (bus & WM0_DONE) {
			wakeup(&ios_frames_done);
		}
		if (ask) {
			exposure_limits(&lines, &gain, &frame_lines);
			if (lines != ios_now.lines || gain != ios_now.gain || frame_lines != ios_now.frame_lines) {
				if (!exposure_write(lines, gain, frame_lines)) {
					break;
				}
				ios_next.lines = lines;
				ios_next.gain = gain;
				ios_next.frame_lines = frame_lines;
				ios_next.from = ios_frames_done + 2;
			}
		}
		if ((bus & WM0_DONE) && ios_frames_done % 30 == 0) {
			if (rd(smmu_context + SMMU_CB_FSR) & SMMU_FSR_ALL) {
				smmu_faults();
				give_up("the SMMU refused the IFE");
				break;
			}
			if (ios_frames_done == 30 || ios_frames_done % 900 == 0) {
				show_paths("streaming");
			}
		}
	}
	ios_stop(broken ? "a fault" : !ios_open ? "closed" : ios_switch_to != 0 ? "another camera asked for" : "nobody reads");
}

static int
camdev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (broken) {
		return EIO;
	}
	ios_open = true;
	return 0;
}

/* (the last close) */
static int
camdev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	ios_open = false;
	ios_wanted = false;
	wakeup(&ios_open);
	return 0;
}

static int
camdev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_frame f;
	uint64_t since = ios_ms();
	int b = -1, error = 0;

	lck_mtx_lock(&ios_lock);
	ios_last_read = ios_ms();
	if (!ios_streaming && !ios_wanted) {
		ios_wanted = true;
		wakeup(&ios_open);
	}
	for (;;) {
		if (broken) {
			lck_mtx_unlock(&ios_lock);
			return EIO;
		}
		b = -1;
		for (int i = 0; i < RING; i++) {
			if (place[i].state == PLACE_READY && place[i].sequence > ios_given && (b < 0 || place[i].sequence > place[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0) {
			break;
		}
		if (ios_ms() - since > (ios_streaming ? 3000u : 15000u)) {      /* (the sensor's tables take a while) */
			lck_mtx_unlock(&ios_lock);
			return EAGAIN;
		}
		if (!ios_streaming && !ios_wanted) {
			ios_wanted = true;
			wakeup(&ios_open);
		}
		struct timespec ts = { 0, 100 * NSEC_PER_MSEC };
		error = msleep(&ios_frames_done, &ios_lock, PZERO | PCATCH, "hakios-camera", &ts);
		if (error != 0 && error != EWOULDBLOCK) {
			lck_mtx_unlock(&ios_lock);
			return error;
		}
		ios_last_read = ios_ms();
	}
	place[b].state = PLACE_READING;
	ios_given = place[b].sequence;
	bzero(&f, sizeof(f));
	memcpy(f.magic, HAKIOS_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = ios_mode->width;
	f.height = ios_mode->height;
	f.stride = ios_stride;
	f.format = 1;
	f.bayer = hw->bayer;
	f.black = 64;
	f.sequence = place[b].sequence;
	f.time = place[b].time;
	f.exposure_lines = place[b].exposure_lines;
	f.line_ns = ios_line_ns;
	f.gain = place[b].gain;
	f.gain_unity = GAIN_UNITY;
	f.gain_max = GAIN_MAX;
	f.frame_lines = place[b].frame_lines;
	f.frame_lines_min = ios_mode->frame_length;
	f.frame_lines_max = FRAME_LINES_MAX;
	f.margin = EXPOSURE_MARGIN;
	f.sensor = hw->ios_sensor;
	f.focus = HAKIOS_CAMERA_NO_FOCUS;
	lck_mtx_unlock(&ios_lock);

	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	if (error == 0 && uio_resid(uio) > 0) {
		vm_offset_t at = ring + (uint64_t)b * ring_place_bytes;
		uint32_t n = (uint32_t)MIN((uint64_t)ring_frame_bytes, (uint64_t)uio_resid(uio));

		/* (what the IFE wrote went past the processor's caches) */
		for (vm_offset_t line = at & ~63ul; line < at + n; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy");
		error = uiomove((caddr_t)at, (int)n, uio);
		if (f.sequence <= 3) {
			say("cam: frame %llu read: %u bytes\n", f.sequence, n);
		}
	}
	lck_mtx_lock(&ios_lock);
	if (place[b].state == PLACE_READING) {
		place[b].state = PLACE_READY;
	}
	ios_last_read = ios_ms();
	lck_mtx_unlock(&ios_lock);
	return error;
}

/* HAKIOS_CAMERA_KEEP: a picture for the host, in the piece of memory taken at boot that the loader hands over. */
static int
keep_picture(const struct hakios_camera_command *c, struct uio *uio)
{
	struct cam_header *h;
	uint64_t want = round_page(CAM_HEADER_BYTES + (uint64_t)c->bytes);

	if (c->bytes == 0 || c->d < 1 || c->d > 3 || (uint64_t)c->c * c->b > (uint64_t)c->bytes * 2) {
		return EINVAL;
	}
	if (keep == 0 || keep_bytes < want) {
		return ENOSPC;
	}
	h = (struct cam_header *)keep;
	bzero(h, CAM_HEADER_BYTES);
	memcpy(h->magic, "RWCAM001", 8);
	h->width = c->a;
	h->height = c->b;
	h->stride = c->c;
	h->format = c->d;
	h->bayer = 0;
	h->frame_bytes = c->bytes;
	h->source = 2;
	strlcpy(h->sensor, "iOS", sizeof(h->sensor));
	int error = uiomove((caddr_t)(keep + CAM_HEADER_BYTES), (int)MIN((uint64_t)c->bytes, (uint64_t)uio_resid(uio)), uio);
	h->frames = error == 0 ? 1 : 0;
	for (vm_offset_t line = keep; line < keep + want; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	loader_recording[0] = kvtophys(keep);
	loader_recording[1] = want;
	loader_recording[2] = (uint64_t)keep;
	say("cam: a picture from iOS for the host: %ux%u, format %u, %u bytes\n", c->a, c->b, c->d, c->bytes);
	return error;
}

static int
camdev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_command c;
	int error;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || (error = uiomove((caddr_t)&c, sizeof(c), uio)) != 0) {
		return EINVAL;
	}
	switch (c.command) {
	case HAKIOS_CAMERA_EXPOSURE:
		lck_mtx_lock(&ios_lock);
		ios_ask.lines = c.a;
		ios_ask.gain = c.b;
		ios_ask.frame_lines = c.c;
		ios_ask.pending = true;
		lck_mtx_unlock(&ios_lock);
		return 0;
	case HAKIOS_CAMERA_KEEP:
		return keep_picture(&c, uio);
	case HAKIOS_CAMERA_FOCUS:
		return 0;                       /* (no lens motor yet) */
	case HAKIOS_CAMERA_SENSOR:
		for (unsigned i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
			if (sensors[i].ios_sensor == c.a) {
				if (&sensors[i] == hw) {
					return 0;       /* (the one already: nothing stopped, nothing started) */
				}
				lck_mtx_lock(&ios_lock);
				ios_switch_to = i + 1;
				lck_mtx_unlock(&ios_lock);
				wakeup(&ios_open);
				return 0;
			}
		}
		say("cam: HAKIOS_CAMERA_SENSOR %u: no such camera here\n", c.a);
		return 0;
	default:
		return EINVAL;
	}
}

static const struct cdevsw camdev_cdevsw = {
	.d_open = camdev_open, .d_close = camdev_close, .d_read = camdev_read, .d_write = camdev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* The device node, made at boot (before mediaserverd starts: its capture stack looks for cameras once). */
static bool camdev_made;

static void
camdev_make(void)
{
	int major = cdevsw_add(-1, &camdev_cdevsw);

	camdev_made = major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-camera") != NULL;
}

/*
 * cam=5: a session whenever a read wants frames, until a fault. The mode: boot-arg cam_mode, else the one
 * with the most pixels at 4:3 and at most 30 frames a second (4000x3000, mode 1); the first exposure until
 * the program asks for its own: cam_shutter (lines), cam_gain (linear, 256 is 1x), else a room's.
 */
/* The sensor the sessions take, and its mode (cam_mode, else the most pixels at about 4:3, at most 30 frames a second and 13 MP). */
static bool
ios_select(unsigned index)
{
	uint32_t mode_n = ~0u;

	hw = &sensors[index];
	pins_find();
	if (!ctab_load()) {
		return false;
	}
	(void)PE_parse_boot_argn("cam_mode", &mode_n, sizeof(mode_n));
	if (index != 0 || mode_n >= ctab->modes) {
		uint64_t best = 0;

		mode_n = 0;
		for (uint32_t m = 0; m < ctab->modes; m++) {
			const struct ctab_mode *o = &ctab_modes[m];
			uint64_t pixels = (uint64_t)o->width * o->height;
			uint32_t w3 = o->width * 3, h4 = o->height * 4;

			if ((w3 > h4 ? w3 - h4 : h4 - w3) <= w3 / 100 && o->fps_milli <= 31000 && pixels <= 13000000 && pixels > best) {
				best = pixels;
				mode_n = m;
			}
		}
	}
	ios_mode_n = mode_n;
	ios_mode = &ctab_modes[mode_n];
	if (ios_mode->cphy || ios_mode->lanes < 1 || ios_mode->lanes > 4 || ios_mode->data_type != RAW10_DT || ios_mode->width % 4 != 0 ||
	    ios_mode->fps_milli == 0 || ios_mode->frame_length == 0) {
		say("cam: mode %u is not what this is written for\n", mode_n);
		return false;
	}
	ios_stride = (ios_mode->width * 10 / 8 + 15) & ~15u;
	ios_line_ns = (uint32_t)(1000000000000ull / ((uint64_t)ios_mode->fps_milli * ios_mode->frame_length));
	ios_ask.lines = ios_mode->frame_length - EXPOSURE_MARGIN;
	ios_ask.gain = 8 * GAIN_UNITY;
	ios_ask.frame_lines = 0;
	(void)PE_parse_boot_argn("cam_shutter", &ios_ask.lines, sizeof(ios_ask.lines));
	(void)PE_parse_boot_argn("cam_gain", &ios_ask.gain, sizeof(ios_ask.gain));
	say("cam: /dev/hakios-camera: the %s camera's frames (%s, mode %u: %ux%u, %u.%03u frames/s at the most), whenever it is read\n",
	    hw->name, hw->chip, mode_n, ios_mode->width, ios_mode->height, ios_mode->fps_milli / 1000, ios_mode->fps_milli % 1000);
	return true;
}

/*
 * cam=5: a session whenever a read wants frames, another camera whenever one is asked for (HAKIOS_CAMERA_SENSOR:
 * userland's 1 the main one, 2 the front one), until a fault. The first exposure until the program asks for
 * its own: cam_shutter (lines), cam_gain (linear, 256 is 1x), else a room's.
 */
static void
serve_ios(void)
{
	if (!camdev_made) {
		say("cam: /dev/hakios-camera cannot be made\n");
		return;
	}
	if (!ios_select((unsigned)(hw - sensors))) {
		return;
	}
	while (!broken) {
		lck_mtx_lock(&ios_lock);
		while (!broken && !(ios_open && ios_wanted) && ios_switch_to == 0) {
			(void)msleep(&ios_open, &ios_lock, PZERO, "hakios-camera-idle", NULL);
		}
		unsigned to = ios_switch_to;

		ios_switch_to = 0;
		lck_mtx_unlock(&ios_lock);
		if (broken) {
			break;
		}
		if (to != 0) {
			if (&sensors[to - 1] != hw && !ios_select(to - 1)) {
				give_up("a camera that cannot be used");
				break;
			}
			continue;
		}
		lck_mtx_lock(&ios_lock);
		ios_wanted = false;
		lck_mtx_unlock(&ios_lock);
		ios_session();
	}
	say("cam: /dev/hakios-camera: no more frames in this run\n");
}

static void
look(void)
{
	show_clocks();
	show_pin(PIN_MCLK, hw->mclk_name);
	show_pin(PIN_SDA, "CCI 0 data");
	show_pin(PIN_SCL, "CCI 0 clock");
	show_pin(PIN_RESET, "the sensor's reset");
}

static void
cam_thread(__unused void *arg, __unused wait_result_t result)
{
	if (level == 5) {
		camdev_make();          /* (before mediaserverd looks for cameras) */
	}
	wait_ms(3000);                  /* after the others have come up */
	{
		char which[16] = "";

		if (PE_parse_boot_arg_str("cam_sensor", which, sizeof(which)) && strcmp(which, "front") == 0) {
			hw = &sensors[1];
		}
	}
	say("cam: level %u: the Mi 9T's %s camera (%s)\n", level, hw->name, hw->chip);
	gcc = ml_io_map(GCC_PHYS + 0xb000, 0x1000) - 0xb000;
	camcc = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
	tlmm = ml_io_map(TLMM_SOUTH_PHYS + 0x1000 * PIN_FIRST, 0x1000 * (PIN_LAST - PIN_FIRST + 1));
	if (gcc == 0 || camcc == 0 || tlmm == 0) {
		say("cam: no mappings\n");
		thread_terminate(current_thread());
		return;
	}
	pins_find();
	look();
	if (!pgpio_find() || !rpmh_find()) {
		say("cam: done (nothing switched)\n");
		thread_terminate(current_thread());
		return;
	}
	{
		uint32_t popup = 0;

		if (PE_parse_boot_argn("cam_popup", &popup, sizeof(popup)) && popup != 0) {
			/* cam_popup=1: LPG channel 1 (the device tree's); 2: channel 0, then channel 1 */
			for (unsigned n = popup >= 2 ? 0 : 1; n <= 1 && !broken; n++) {
				lpg.id = LPG_ID(n);
				lpg.found = lpg.writable = false;
				lpg.name = n ? "PM6150L LPG 1" : "PM6150L LPG 0";
				if (popup_move(true)) {
					wait_ms(3000);
				}
				(void)popup_move(false);
				motor_give_back();
				wait_ms(3000);
			}
		}
	}
	if (level == 5) {
		serve_ios();
	} else if (level == 4) {
		capture_sensor();
		look();
	} else if (level == 3) {
		capture_tpg();
		look();
	} else if (level == 2) {
		if (power_up()) {
			read_chip_id();
		}
		power_down();
		look();
	}
	say("cam: done\n");
	thread_terminate(current_thread());
}

void
hakios_camera_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cam", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 5 ? 5 : level;
	if (level >= 3) {
		/* the frames' memory in one piece (cam_keep_mb, default 8: two 1600x1200 or 1920x1080 RAW10 frames), taken now, while memory still is in large pieces */
		uint32_t keep_mb = level == 5 ? 24 : 8;   /* (cam=5: a picture iOS keeps for the host, HAKIOS_CAMERA_KEEP) */

		(void)PE_parse_boot_argn("cam_keep_mb", &keep_mb, sizeof(keep_mb));
		keep_bytes = (uint64_t)(keep_mb < 4 ? 4 : keep_mb > 128 ? 128 : keep_mb) << 20;
		if (kmem_alloc_contig(kernel_map, &keep, keep_bytes, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("cam: no memory in one piece for the frames (%llu MB)\n", keep_bytes >> 20);
			keep = 0;
			keep_bytes = 0;
		}
	}
	if (kernel_thread_start(cam_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: whatever was switched on, off. */
void
hakios_camera_quiesce(void)
{
	if (popped_up) {
		leaving = true;
		broken = false;
		(void)popup_move(false);        /* (the front camera never left up) */
		motor_give_back();
	}
	if (level < 2 || tlmm == 0) {
		return;
	}
	leaving = true;
	broken = false;                 /* (one last time, whatever went before) */
	csiphy_down();
	titan_down();
	power_down();
}
