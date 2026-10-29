/*
 * The speaker of the Mi 9T (davinci): its amplifier, an NXP TFA9874 (dt: tfa98xx@34 on qupv3_se4_i2c,
 * interrupt GPIO 56, its reset GPIO 12 or 67 by the board: the stock dtbo has three davinci boards,
 * 0x28 (dtbo 14, this phone: ro.boot.dtbo_idx) with GPIO 12, 0x27 and 0x29 with 67; boot-arg
 * tfa_reset=N, default 12), fed by the primary MI2S interface that the audio DSP drives
 * (soc/drivers/audio/hakios.sound_sm7150.c). It takes the place of drivers/audio/hakios.speaker.c (redwood's two
 * Awinic amplifiers) for the same calls (sys/hakios.audio.h: hakios_speaker_*).
 *
 * The TFA9874 has no DSP of its own: NXP's driver calls it a "probus" device, whose speaker
 * protection runs in the platform's DSP instead (Android: Qualcomm's, with the .msg and .vstep
 * files of the container). None of that here: the speakers' guard (drivers/audio/hakios.guard.c) stands in for it,
 * as it does on redwood. What is left is NXP's start of such a device (LineageOS's
 * android_kernel_xiaomi_sm6150, sound/soc/codecs: tfa_dsp.c tfaRunStartup() and tfa_dev_set_state(),
 * tfa_init.c tfa9874_specific()), driven from the phone's own container (/fw/mono.cnt, from its
 * vendor image's firmware/mono.cnt):
 *   - a pulse on the reset line; the revision (register 3: 0x0a74, 0x0b74, 0x0c74)
 *   - the I2C registers reset (I2CR), the revision's optimal settings (NXP's generated tables),
 *     AUDFS and FRACTDEL as they were
 *   - the container's device list: its register writes and bitfields (the keys, the interface,
 *     the DC-DC converter, the amplifier's gain); then its first profile, MUSIC_48000, up to its
 *     "default" marker
 *   - "init CF": MANSCONF, out of power-down, the clocks stable (CLKS: only with the MI2S bit clock
 *     running); "operating": the amplifier on (AMPE)
 * The chip has no volume register: the level is the samples' (hakios_speaker_volume() sets the factor
 * the sound driver applies to them).
 *
 * The bus is I2C by hand on GPIO 53 (data) and 54 (clock), as drivers/audio/hakios.speaker.c does it on redwood's.
 * A bitfield is NXP's 16-bit code: register << 8 | first bit << 4 | (bits - 1).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.audio.h>

uint32_t hakios_tfa_gain_q15(void);     /* the factor the SM7150's sound driver applies to the samples (no volume register) */

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	return length;
}

static bool leaving;                    /* a panic's way out: no sleeping */

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
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

/* ---- pins: the west tile for the bus, the north one for the reset line (Linux's pinctrl-sm7150.c) ---- */

#define TLMM_WEST               0x03500000UL
#define TLMM_NORTH              0x03900000UL
#define TLMM_SOUTH              0x03d00000UL
#define PIN_SDA                 53
#define PIN_SCL                 54
#define PIN_RESET               reset_pin
static unsigned reset_pin = 12;
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)

static vm_offset_t pin_page[3];         /* SDA, SCL, reset: one page each */

static vm_offset_t
pin_base(unsigned pin)
{
	unsigned i = pin == PIN_SDA ? 0 : pin == PIN_SCL ? 1 : 2;

	if (pin_page[i] == 0) {
		/* GPIO 12 is in the south tile, 67 in the north one, 53 and 54 in the west one */
		pin_page[i] = ml_io_map((pin == PIN_SDA || pin == PIN_SCL ? TLMM_WEST : pin == 12 ? TLMM_SOUTH : TLMM_NORTH) +
		    0x1000UL * pin, 0x1000);
	}
	return pin_page[i];
}

#define GPIO_CFG(n)             (pin_base(n))
#define GPIO_IO(n)              (pin_base(n) + 4)

static bool
level(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

/* ---- I2C by hand (drivers/audio/hakios.speaker.c's) --------------------------------------------------------------------- */

static uint32_t half_bit_us = 5;

static void
half_bit(void)
{
	spin_us(half_bit_us);
}

static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
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
	for (unsigned stretched = 0; !level(PIN_SCL) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = level(PIN_SDA);
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

static uint32_t taken_cfg[2], taken_io[2];

static bool
bus_take(void)
{
	taken_cfg[0] = rd(GPIO_CFG(PIN_SDA));
	taken_cfg[1] = rd(GPIO_CFG(PIN_SCL));
	taken_io[0] = rd(GPIO_IO(PIN_SDA));
	taken_io[1] = rd(GPIO_IO(PIN_SCL));
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	if (!level(PIN_SDA) || !level(PIN_SCL)) {
		wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
		wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
		wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
		wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
		return false;
	}
	return true;
}

static void
bus_give_back(void)
{
	wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
	wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
	wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
	wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
}

#define TFA_ADDRESS             0x34

/* A 16-bit register of the TFA9874, most significant byte first. False if nobody answers. */
static bool
reg_read(uint8_t reg, uint16_t *value)
{
	bool answered;

	if (!bus_take()) {
		say("tfa: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA, level(PIN_SDA),
		    PIN_SCL, level(PIN_SCL));
		return false;
	}
	i2c_start();
	answered = i2c_write(TFA_ADDRESS << 1) && i2c_write(reg);
	if (answered) {
		i2c_start();
		answered = i2c_write(TFA_ADDRESS << 1 | 1);
	}
	if (answered) {
		*value = (uint16_t)(i2c_read(true) << 8);
		*value |= i2c_read(false);
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

static uint32_t writes, writes_failed;

static bool
reg_write(uint8_t reg, uint16_t value)
{
	bool answered;

	if (!bus_take()) {
		writes_failed++;
		return false;
	}
	i2c_start();
	answered = i2c_write(TFA_ADDRESS << 1) && i2c_write(reg) && i2c_write((uint8_t)(value >> 8)) && i2c_write((uint8_t)value);
	i2c_stop();
	bus_give_back();
	writes++;
	writes_failed += !answered;
	return answered;
}

/* NXP's bitfields: register << 8 | first bit << 4 | (bits - 1). */
static bool
bf_get(uint16_t field, uint16_t *value)
{
	uint16_t reg;
	unsigned pos = (field >> 4) & 15, len = (field & 15) + 1;

	if (!reg_read((uint8_t)(field >> 8), &reg)) {
		return false;
	}
	*value = (uint16_t)((reg >> pos) & ((1u << len) - 1));
	return true;
}

static bool
bf_set(uint16_t field, uint16_t value)
{
	uint16_t reg;
	unsigned pos = (field >> 4) & 15, len = (field & 15) + 1;
	uint16_t mask = (uint16_t)(((1u << len) - 1) << pos);

	if (!reg_read((uint8_t)(field >> 8), &reg)) {
		return false;
	}
	return reg_write((uint8_t)(field >> 8), (uint16_t)((reg & ~mask) | ((value << pos) & mask)));
}

/* tfa9874_tfafieldnames.h */
#define BF_PWDN                 0x0000
#define BF_I2CR                 0x0010
#define BF_AMPE                 0x0030
#define BF_DCA                  0x0040
#define BF_MANSCONF             0x0120
#define BF_AUDFS                0x0203
#define BF_FRACTDEL             0x0255
#define BF_REV                  0x030f
#define BF_CLKS                 0x1170
#define BF_MANSTATE             0x1433
#define BF_TDMERR               0x1060          /* (tfa9874_tfafieldnames.h) */
#define BF_TDMSTAT              0x1192
#define BF_TDMNBCK              0x20c3          /* bit clocks in a frame: 0 32, 2 64 */
#define BF_TDMSLLN              0x2144          /* bits in a slot, less one */
#define BF_TDMSSIZE             0x2224          /* bits of a sample in a slot, less one */
#define REG_STATUS              0x10            /* VDDS, DCOCPOK, OTDS, OCDS, UVDS ... (sticky, cleared on read) */
/*
 * OTDS (bit 2) and UVDS (bit 4) are set while the temperature and the supply are fine: NXP's
 * tfa_dsp.c reports "misc errors" when they read 0 (as when CLKS does). OCDS (bit 3), an overcurrent
 * of the amplifier, is a fault when set.
 */
#define STATUS_OK_BITS          (1u << 2 | 1u << 4)
#define STATUS_OVERCURRENT      (1u << 3)

/* ---- the container (tfa98xx_parameters.h: nxpTfaContainer, nxpTfaDeviceList, nxpTfaProfileList) ---- */

enum { DSC_REGISTER = 2, DSC_BITFIELD = 16, DSC_DEFAULT = 17, DSC_PROFILE = 1 };

static const uint8_t *cnt;
static uint64_t cnt_size;

static uint32_t
cnt_word(uint32_t at)
{
	uint32_t w = 0;

	if (at + 4 <= cnt_size) {
		memcpy(&w, cnt + at, 4);
	}
	return w;
}

static uint16_t
cnt_half(uint32_t at)
{
	uint16_t h = 0;

	if (at + 2 <= cnt_size) {
		memcpy(&h, cnt + at, 2);
	}
	return h;
}

/* One item of a list: a register patch or a bitfield written; the others (files for the platform's DSP, filters) left. */
static bool
item_write(uint32_t descriptor, unsigned *registers, unsigned *bitfields)
{
	uint32_t at = descriptor & 0xffffff, type = descriptor >> 24;

	if (type == DSC_REGISTER && at + 5 <= cnt_size) {
		uint8_t reg = cnt[at];
		uint16_t value = cnt_half(at + 1), mask = cnt_half(at + 3), old = 0;

		(*registers)++;
		if (mask != 0xffff && !reg_read(reg, &old)) {
			return false;
		}
		return reg_write(reg, (uint16_t)((old & ~mask) | (value & mask)));
	}
	if (type == DSC_BITFIELD && at + 4 <= cnt_size) {
		(*bitfields)++;
		return bf_set(cnt_half(at + 2), cnt_half(at));
	}
	return true;
}

/* The container's first device list, and its profile `profile` (by its order in that list), up to the profile's "default". */
static bool
container_write(unsigned profile, unsigned *registers, unsigned *bitfields, char *profile_name, size_t name_size)
{
	uint32_t device, length, seen = 0;

	if (cnt == NULL || cnt_size < 52 || cnt[0] != 'P' || cnt[1] != 'M' || cnt_half(40) < 1) {
		say("tfa: /fw/mono.cnt is not a container this knows\n");
		return false;
	}
	device = cnt_word(46) & 0xffffff;
	length = device < cnt_size ? cnt[device] : 0;
	for (uint32_t i = 0; i < length; i++) {
		uint32_t d = cnt_word(device + 12 + 4 * i);

		if ((d >> 24) == DSC_PROFILE) {
			uint32_t p = d & 0xffffff, head = cnt_word(p), name = cnt_word(p + 4) & 0xffffff, items = (head & 0xff) - 1;

			if (seen++ != profile) {
				continue;
			}
			if (name < cnt_size) {
				strlcpy(profile_name, (const char *)cnt + name, name_size);
			}
			for (uint32_t j = 0; j < items; j++) {
				uint32_t e = cnt_word(p + 8 + 4 * j);

				if ((e >> 24) == DSC_DEFAULT) {
					break;
				}
				if (!item_write(e, registers, bitfields)) {
					return false;
				}
			}
			continue;
		}
		if (!item_write(d, registers, bitfields)) {
			return false;
		}
	}
	return seen > profile;
}

/* ---- NXP's optimal settings by revision (tfa_init.c, tfa9874_specific(): generated code) ------------- */

static const struct {
	uint8_t reg;
	uint16_t value;
} rev_0a74[] = {
	{ 0x02, 0x22a8 }, { 0x51, 0x0020 }, { 0x52, 0x57dc }, { 0x58, 0x16a4 }, { 0x61, 0x0110 }, { 0x66, 0x0701 },
	{ 0x6f, 0x00a3 }, { 0x70, 0x07f8 }, { 0x73, 0x0007 }, { 0x74, 0x5068 }, { 0x75, 0x0d28 }, { 0x83, 0x0594 },
	{ 0x84, 0x0001 }, { 0x85, 0x0001 }, { 0x88, 0x0000 }, { 0xc4, 0x2001 },
}, rev_0b74[] = {
	{ 0x02, 0x22a8 }, { 0x51, 0x0020 }, { 0x52, 0x57dc }, { 0x58, 0x16a4 }, { 0x61, 0x0110 }, { 0x66, 0x0701 },
	{ 0x6f, 0x00a3 }, { 0x70, 0x07f8 }, { 0x73, 0x0047 }, { 0x74, 0x5068 }, { 0x75, 0x0d28 }, { 0x83, 0x0595 },
	{ 0x84, 0x0001 }, { 0x85, 0x0001 }, { 0x88, 0x0000 }, { 0xc4, 0x2001 },
}, rev_0c74[] = {
	{ 0x02, 0x22c8 }, { 0x52, 0x57dc }, { 0x53, 0x003e }, { 0x56, 0x0400 }, { 0x61, 0x0110 }, { 0x6f, 0x00a5 },
	{ 0x70, 0x07f8 }, { 0x73, 0x0047 }, { 0x74, 0x5098 }, { 0x75, 0x8d28 }, { 0x80, 0x0000 }, { 0x83, 0x0799 },
	{ 0x84, 0x0081 },
};

static bool
optimal_settings(uint16_t rev)
{
	uint16_t value = 0;
	unsigned count;
	const typeof(rev_0a74[0]) *table;

	/* unlock key 1 and 2 (tfa9874_specific(), tfa98xx_key2(tfa, 0)) */
	if (!reg_write(0x0f, 0x5a6b) || !reg_read(0xfb, &value) || !reg_write(0xa0, value ^ 0x005a) || !reg_write(0xa1, 0x005a) ||
	    !reg_write(0x0f, 0)) {
		return false;
	}
	switch (rev) {
	case 0x0a74: table = rev_0a74; count = sizeof(rev_0a74) / sizeof(rev_0a74[0]); break;
	case 0x0b74: table = rev_0b74; count = sizeof(rev_0b74) / sizeof(rev_0b74[0]); break;
	case 0x0c74: table = rev_0c74; count = sizeof(rev_0c74) / sizeof(rev_0c74[0]); break;
	default:
		say("tfa: no optimal settings for revision %#x (NXP has them for 0x0a74, 0x0b74, 0x0c74): left as they are\n", rev);
		return true;
	}
	for (unsigned i = 0; i < count; i++) {
		if (!reg_write(table[i].reg, table[i].value)) {
			return false;
		}
	}
	return true;
}

/* ---- the speaker interface (sys/hakios.audio.h) ----------------------------------------------------------------------------- */

static bool prepared, started;
static uint16_t revision;

/* A pulse on the reset line (Xiaomi's tfa98xx_ext_reset(): high, 5 ms, low), and who the chip is. */
static bool
reset_and_ask(void)
{
	wr(GPIO_CFG(PIN_RESET), CFG_OUTPUT);           /* function 0 (a GPIO), 2 mA, no pull */
	wr(GPIO_IO(PIN_RESET), 2);
	wait_ms(5);
	wr(GPIO_IO(PIN_RESET), 0);
	wait_ms(5);
	if (!bf_get(BF_REV, &revision)) {
		uint32_t found = 0;

		say("tfa: nobody answers at I2C 0x%x on gpio%u and gpio%u (pins: cfg %#x %#x, levels %u %u; reset cfg %#x io %#x)\n",
		    TFA_ADDRESS, PIN_SDA, PIN_SCL, rd(GPIO_CFG(PIN_SDA)), rd(GPIO_CFG(PIN_SCL)), level(PIN_SDA), level(PIN_SCL),
		    rd(GPIO_CFG(PIN_RESET)), rd(GPIO_IO(PIN_RESET)));
		/* who answers on this bus at all: an address byte each, acknowledged or not */
		for (uint8_t a = 0x08; a < 0x78; a++) {
			bool ack;

			if (!bus_take()) {
				say("tfa: the bus is not at rest any more\n");
				break;
			}
			i2c_start();
			ack = i2c_write((uint8_t)(a << 1));
			i2c_stop();
			bus_give_back();
			if (ack) {
				found++;
				say("tfa: I2C address 0x%x answers on this bus\n", a);
			}
		}
		say("tfa: %u addresses answer on gpio%u/gpio%u\n", found, PIN_SDA, PIN_SCL);
		/* can the pins pull the lines down at all? */
		if (bus_take()) {
			bool sda_low, scl_low;

			line(PIN_SDA, false);
			half_bit();
			sda_low = !level(PIN_SDA);
			line(PIN_SDA, true);
			line(PIN_SCL, false);
			half_bit();
			scl_low = !level(PIN_SCL);
			line(PIN_SCL, true);
			half_bit();
			bus_give_back();
			say("tfa: driven low, the data line reads %s and the clock line %s\n", sda_low ? "low" : "STILL HIGH",
			    scl_low ? "low" : "STILL HIGH");
		}
		/* ... and at a quarter of the speed, once more */
		half_bit_us = 20;
		if (bf_get(BF_REV, &revision)) {
			say("tfa: at 25 kHz the amplifier answers: revision %#x\n", revision);
			return (revision & 0xff) == 0x74;
		}
		say("tfa: at 25 kHz nobody answers either\n");
		return false;
	}
	say("tfa: the amplifier answers: revision %#x%s\n", revision, (revision & 0xff) == 0x74 ? " (a TFA9874)" :
	    " -- NOT A TFA9874: left alone");
	return (revision & 0xff) == 0x74;
}

/* Found, reset, set up from the container: powered down, the amplifier off. `container` is /fw/mono.cnt. */
bool
hakios_speaker_prepare(const uint8_t *container, uint64_t size)
{
	uint16_t audfs = 8, fractdel = 0;
	unsigned registers = 0, bitfields = 0;
	char profile[48] = "?";

	cnt = container;
	cnt_size = size;
	prepared = started = false;
	reset_pin = 12;
	PE_parse_boot_argn("tfa_reset", &reset_pin, sizeof(reset_pin));
	if (reset_pin != 12 && reset_pin != 67) {
		say("tfa: tfa_reset=%u: only 12 and 67 are known (their tiles): 12\n", reset_pin);
		reset_pin = 12;
	}
	if (!reset_and_ask()) {
		return false;
	}
	/* tfaRunStartup(): AUDFS and FRACTDEL kept across the reset of the registers */
	bf_get(BF_AUDFS, &audfs);
	bf_get(BF_FRACTDEL, &fractdel);
	if (!reg_write(0x00, 1u << 1) || !optimal_settings(revision) || !bf_set(BF_AUDFS, audfs) || !bf_set(BF_FRACTDEL, fractdel)) {
		say("tfa: the chip stopped answering while it was being set up\n");
		return false;
	}
	if (!container_write(0, &registers, &bitfields, profile, sizeof(profile))) {
		say("tfa: the container's settings could not all be written\n");
		return false;
	}
	/* (the profile turns the amplifier and the converter on: off again until hakios_speaker_start()) */
	bf_set(BF_AMPE, 0);
	bf_set(BF_DCA, 0);
	/*
	 * The frame: not in the container. Android's driver sets it from the stream's format when it starts
	 * (tfa_dsp.c, tfaRunSpeakerBoost(): "support 16bit/24bit/32bit audio data"). Android runs this phone's
	 * MI2S port at 24 bits (S24_LE) in 32-bit slots, so: 64 bit clocks a frame (TDMNBCK 2), slots and
	 * samples of 32 bits (TDMSLLN, TDMSSIZE 31) -- what the port of soc/drivers/audio/hakios.sound_sm7150.c sends (3.072 MHz). With
	 * no frame set the chip played nothing that could be heard; at 16 bits it reported a TDM error and
	 * the sound was distorted (26 October).
	 */
	{
		uint16_t r20 = 0, r21 = 0, r22 = 0;

		reg_read(0x20, &r20);
		reg_read(0x21, &r21);
		reg_read(0x22, &r22);
		bf_set(BF_TDMNBCK, 2);
		bf_set(BF_TDMSLLN, 31);
		bf_set(BF_TDMSSIZE, 31);
		say("tfa: the frame as Android sets it for 24 bits (TDMNBCK 2, TDMSLLN 31, TDMSSIZE 31); the chip had 0x20 %#x, "
		    "0x21 %#x, 0x22 %#x\n", r20, r21, r22);
	}
	say("tfa: set up from /fw/mono.cnt: %u register writes and %u bitfields of the device list and of profile \"%s\"; "
	    "%u writes on the bus, %u not acknowledged\n", registers, bitfields, profile, writes, writes_failed);
	prepared = true;
	return true;
}

/* Out of power-down and operating (tfa_dev_set_state(): init CF, then operating): only while the MI2S bit clock runs. */
bool
hakios_speaker_start(void)
{
	uint16_t clocks = 0, state = 0;

	if (!prepared) {
		return false;
	}
	bf_set(BF_MANSCONF, 1);
	bf_set(BF_PWDN, 0);
	for (int i = 0; i < 50 && bf_get(BF_CLKS, &clocks) && !clocks; i++) {
		wait_ms(2);
	}
	if (!clocks) {
		say("tfa: the clocks do not settle (CLKS 0): is the MI2S bit clock running? Powered down again\n");
		bf_set(BF_PWDN, 1);
		return false;
	}
	bf_set(BF_DCA, 1);
	bf_set(BF_AMPE, 1);
	{
		uint16_t status = 0;

		reg_read(REG_STATUS, &status);  /* (what the power-up has left in the sticky bits, read and so cleared) */
		say("tfa: status after the start %#x\n", status);
	}
	bf_get(BF_MANSTATE, &state);
	say("tfa: operating: clocks stable, the converter and the amplifier on (manager state %u)\n", state);
	{
		uint16_t r20 = 0, r21 = 0, r22 = 0, r23 = 0, error = 0, tdm = 0, fs = 0;

		reg_read(0x20, &r20);
		reg_read(0x21, &r21);
		reg_read(0x22, &r22);
		reg_read(0x23, &r23);
		bf_get(BF_TDMERR, &error);
		bf_get(BF_TDMSTAT, &tdm);
		bf_get(BF_AUDFS, &fs);
		say("tfa: its interface: 0x20 %#x 0x21 %#x 0x22 %#x 0x23 %#x; TDM error %u, TDM status %#x; AUDFS %u (8: 48 kHz)\n",
		    r20, r21, r22, r23, error, tdm, fs);
	}
	started = true;
	return true;
}

/*
 * The level, below Android's, in eighths of a dB: the chip has none of its own, so it is the factor
 * soc/drivers/audio/hakios.sound_sm7150.c puts on the samples (Q15). 10^(-x/160) for x in eighths of a dB, by steps of 6.02 dB
 * (a halving) and a table within one.
 */
static uint32_t level_q15 = 0;          /* silent until asked */

uint32_t
hakios_tfa_gain_q15(void)
{
	return level_q15;
}

bool
hakios_speaker_volume(uint32_t below, __unused uint32_t step_ms)
{
	static const uint16_t within[48] = {    /* 32768 * 10^(-k/160), k = 0..47 */
		32768, 32300, 31838, 31383, 30935, 30493, 30057, 29628, 29205, 28787, 28376, 27970, 27571, 27177, 26789, 26406,
		26029, 25657, 25290, 24929, 24573, 24221, 23875, 23534, 23198, 22867, 22540, 22218, 21900, 21587, 21279, 20975,
		20675, 20380, 20089, 19802, 19519, 19240, 18965, 18694, 18427, 18164, 17904, 17648, 17396, 17147, 16902, 16661,
	};
	uint32_t halvings = below / 48;

	level_q15 = halvings >= 15 ? 0 : (uint32_t)within[below % 48] >> halvings;
	return started;
}

/* No fault reported (overtemperature, overcurrent, undervoltage: sticky, cleared by the read); on one, powered down. */
bool
hakios_speaker_healthy(void)
{
	uint16_t status = 0;

	if (!started || !reg_read(REG_STATUS, &status)) {
		return started ? false : true;
	}
	if ((status & STATUS_OK_BITS) != STATUS_OK_BITS || (status & STATUS_OVERCURRENT)) {
		say("tfa: THE AMPLIFIER REPORTS A FAULT (status %#x: %s%s%s): powered down\n", status, !(status & 4) ? "overtemperature " :
		    "", (status & 8) ? "overcurrent " : "", !(status & 16) ? "undervoltage" : "");
		hakios_speaker_stop();
		return false;
	}
	{
		/* the TDM error said at the start: does it stay while the sound plays? (each change said, and the first 3 looks) */
		static uint16_t said_error = 0xffff, said_tdm;
		static unsigned looks;
		uint16_t error = 0, tdm = 0;

		bf_get(BF_TDMERR, &error);
		bf_get(BF_TDMSTAT, &tdm);
		if (looks++ < 3 || error != said_error || tdm != said_tdm) {
			say("tfa: while it plays: TDM error %u, TDM status %#x (status %#x)\n", error, tdm, status);
			said_error = error;
			said_tdm = tdm;
		}
	}
	return true;
}

void
hakios_speaker_stop(void)
{
	level_q15 = 0;
	if (started || prepared) {
		bf_set(BF_AMPE, 0);
		bf_set(BF_DCA, 0);
		bf_set(BF_PWDN, 1);
		say("tfa: powered down (%u writes on the bus, %u not acknowledged)\n", writes, writes_failed);
	}
	started = false;
}

void
hakios_speaker_quiesce(void)
{
	leaving = true;
	if (started || prepared) {
		bf_set(BF_AMPE, 0);
		bf_set(BF_PWDN, 1);
	}
	started = prepared = false;
}

/* The rest of the speaker interface: nothing to probe before the sound's start, no watch of its own (the guard's is all). */
void
hakios_speaker_init(void)
{
}

uint32_t
hakios_speaker_monitor(__unused bool tell)
{
	return 0;
}
