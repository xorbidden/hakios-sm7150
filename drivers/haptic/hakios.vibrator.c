/*
 * In place of drivers/haptic/hakios.haptic.c (the SM7325 phone's Awinic AW86927) for the Mi 9T, behind the same
 * calls (hakios_haptic_init, _quiesce, _pretend) and the same /dev/hakios-haptic, with boot-arg haptic=N (its levels
 * below).
 */
/*
 * The vibration motor of the Mi 9T (davinci): an eccentric-mass motor on the PM6150's vibrator LDO
 * (SPMI slave 1, registers 0x5300; the device tree's "qcom,qpnp-vibrator-ldo", 3.0 V, no overdrive).
 * Android's driver (drivers/leds/leds-qpnp-vibrator-ldo.c of the sm6150 kernel) does no more than
 * this: the voltage in millivolts into VSET (low byte at 0x40, high at 0x41), then EN_CTL (0x46)
 * bit 7 on, and STATUS1 (0x08) bit 7 (VREG_READY) waited for; off is EN_CTL bit 7 clear. There is
 * no waveform: the motor runs while the LDO is on.
 *
 * Writing to a PMIC is not like writing anywhere else: the PMICs make the phone's supplies. So every
 * write goes through one function that takes only this peripheral's three registers above, through
 * the arbiter's channel checked to be this peripheral's and ours to write, every write is read back,
 * and any surprise ends all writing for the run (the motor off first, if it can be). Transfers go one
 * at a time with the buttons' driver (hakios_spmi_lock(), drivers/core/hakios.platform.c). The arbiter is the same version 5 one as
 * redwood's, at the same addresses (the device tree's spmi@c440000), as in drivers/camera/hakios.torch.c.
 *
 * What iOS plays on its "Actuator" (its haptics, as waveforms of one channel: the audio plug-in's
 * actuator_write(), userland/src/audio_plugin.c) comes to /dev/hakios-haptic as on redwood. With no
 * waveform to give the motor, it runs while what comes is louder than SILENT_BELOW: on at once, for
 * MIN_ON_MS at least (the motor takes that long to be felt at all), off when it has been quiet for
 * QUIET_MS, and never longer than MAX_ON_MS at a stretch (then off until there has been quiet).
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   haptic=1      a look, reading only: the arbiter's channel, the peripheral's type, its registers
 *   haptic=2      ... and the motor on once for haptic_ms (default 200, at most 1000), then off
 *   haptic=3      ... no buzz at the start; the motor for iOS until the run ends (/dev/hakios-haptic)
 *
 * At the run's end (and on a panic's way out) the motor goes off.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern void IOSleep(unsigned int milliseconds);

static uint32_t level;
static bool broken, leaving, running_on;

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

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/* ---- the PMIC arbiter (version 5), as hakios.torch.c ---------------------------------------------------- */

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
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)
#define COMMAND_READ            (1u << 27)
#define TRANSFER_TIMEOUT_US     2000
#define DROPPED                 "the transfer was dropped"

#define VIB_ID                  0x153           /* slave 1, peripheral 0x53 */
#define VIB_TYPE                0x04
#define VIB_SUBTYPE             0x05
#define VIB_STATUS1             0x08            /* [7] VREG_READY */
#define VIB_VSET_LB             0x40
#define VIB_VSET_UB             0x41
#define VIB_EN_CTL              0x46            /* [7] on */
#define VIB_MV                  3000            /* "qcom,vib-ldo-volt-uv" 3000000 */

static vm_offset_t arb_core, arb_cnfg, arb_observer, vib_write;
static uint16_t vib_channel;
static bool vib_found, vib_writable;

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

/* `count' (1 to 8) registers from `reg' on; NULL, or what went wrong; a dropped transfer asked for again twice */
static const char *
spmi_read(uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * vib_channel;
	const char *problem = "no channel";
	uint32_t data[2] = { 0, 0 };

	for (int tries = 0; vib_found && tries < 3; tries++) {
		lock();
		wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
		problem = wait_done(channel);
		data[0] = rd(channel + OBSERVER_DATA0);
		data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	if (problem != NULL) {
		return problem;
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		say("vib: %s: no more writing to the vibrator for the rest of the run\n", why);
	}
}

/* One of the three registers written and read back: false, and no more writing, if it cannot be or does not read back */
static bool
put(uint8_t reg, uint8_t value)
{
	const char *problem = NULL;
	uint8_t back = 0xff;

	if (broken || !(reg == VIB_VSET_LB || reg == VIB_VSET_UB || reg == VIB_EN_CTL) || !vib_writable || vib_write == 0) {
		give_up("a write that may not be made");
		return false;
	}
	if (((rd(arb_core + ARB_CHANNEL(vib_channel)) >> 8) & 0xfff) != VIB_ID) {
		give_up("the arbiter's channel is no longer the vibrator's");
		return false;
	}
	for (int tries = 0; tries < 3; tries++) {
		lock();
		wr(vib_write + CHANNEL_WDATA0, value);
		wr(vib_write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
		problem = wait_done(vib_write);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	if (problem != NULL || (problem = spmi_read(reg, &back, 1)) != NULL) {
		say("vib: %#04x <- %#04x: %s\n", reg, value, problem);
		give_up("a write that failed");
		return false;
	}
	if (back != value) {
		say("vib: %#04x <- %#04x, but it reads %#04x\n", reg, value, back);
		give_up("a register that did not read back as written");
		return false;
	}
	return true;
}

static bool
find(void)
{
	uint32_t version, channels;

	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	if ((version & 0xf0000000u) != ARB_VERSION_5) {
		say("vib: the arbiter is version %#x, not 5: left alone\n", version);
		return false;
	}
	channels = rd(arb_core + ARB_FEATURES) & 0x3ff;
	channels = channels > ARB_MAX_CHANNELS ? ARB_MAX_CHANNELS : channels;
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != VIB_ID) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		say("vib: channel %u is the vibrator's (%#x); written by system %u%s\n", n, map, owner,
		    owner == ARB_SYSTEM ? ", which is us" : "");
		if (!vib_found || owner == ARB_SYSTEM) {
			vib_found = true;
			vib_channel = (uint16_t)n;
			vib_writable = owner == ARB_SYSTEM;
		}
	}
	if (!vib_found) {
		say("vib: no channel of the arbiter is the vibrator's (%#x)\n", VIB_ID);
		return false;
	}
	if (vib_writable) {
		vib_write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * vib_channel, 0x1000);
	}
	return true;
}

static void
look(void)
{
	uint8_t r[8];

	if (spmi_read(VIB_TYPE, r, 2) == NULL) {
		say("vib: type %#04x, subtype %#04x\n", r[0], r[1]);
	}
	if (spmi_read(VIB_STATUS1, r, 1) == NULL) {
		say("vib: STATUS1 %#04x\n", r[0]);
	}
	if (spmi_read(VIB_VSET_LB, r, 8) == NULL) {
		say("vib: 0x40..0x47: %02x %02x %02x %02x %02x %02x %02x %02x (VSET %u mV, EN_CTL %#04x)\n", r[0], r[1], r[2], r[3],
		    r[4], r[5], r[6], r[7], r[0] | r[1] << 8, r[6]);
	}
}

/* ---- the motor ------------------------------------------------------------------------------------------ */

static bool
motor(bool on)
{
	uint8_t status = 0;

	if (on == running_on) {
		return true;
	}
	if (!on) {
		running_on = false;
		return put(VIB_EN_CTL, 0x00);
	}
	if (!put(VIB_VSET_LB, VIB_MV & 0xff) || !put(VIB_VSET_UB, VIB_MV >> 8) || !put(VIB_EN_CTL, 0x80)) {
		(void)put(VIB_EN_CTL, 0x00);
		return false;
	}
	running_on = true;
	for (int i = 0; i < 10 && spmi_read(VIB_STATUS1, &status, 1) == NULL && !(status & 0x80); i++) {
		IOSleep(1);
	}
	if (!(status & 0x80)) {
		say("vib: the LDO did not say ready (STATUS1 %#04x): off\n", status);
		running_on = false;
		broken = false;
		(void)put(VIB_EN_CTL, 0x00);
		give_up("the LDO not ready");
		return false;
	}
	return true;
}

/* ---- vib=3: iOS's actuator ------------------------------------------------------------------------------ */

#define SILENT_BELOW            1500            /* of 32767: quieter than this is no vibration */
#define MIN_ON_MS               40
#define QUIET_MS                30
#define MAX_ON_MS               3000

static volatile uint64_t heard_us, quiet_since_us;
static uint64_t written_frames;
static uint32_t buzzes, capped;

static int
vib_dev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		if (bytes > (user_ssize_t)sizeof(samples)) {
			bytes = sizeof(samples);
		}
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		written_frames += (uint64_t)bytes / 2;
		if (peak > SILENT_BELOW) {
			heard_us = now_us();
		} else if (heard_us == 0 || now_us() - heard_us > QUIET_MS * 1000ull) {
			quiet_since_us = now_us();
		}
	}
	return error;
}

static int
vib_dev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw vib_cdevsw = {
	.d_open = vib_dev_open, .d_close = vib_dev_open, .d_read = eno_rdwrt, .d_write = vib_dev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
serve_ios(void)
{
	uint64_t on_since = 0;
	bool held_off = false;
	int major = cdevsw_add(-1, &vib_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") == NULL) {
		say("vib: no /dev/hakios-haptic: iOS's haptics are not felt\n");
		return;
	}
	say("vib: the motor for iOS from here on (/dev/hakios-haptic): on while its haptics are louder than %u, %u ms at "
	    "least, %u ms at most at a stretch\n", SILENT_BELOW, MIN_ON_MS, MAX_ON_MS);
	while (!leaving && !broken) {
		uint64_t now = now_us(), heard = heard_us;
		bool active = heard != 0 && now - heard < QUIET_MS * 1000ull;

		if (held_off && !active) {
			held_off = false;
		}
		if (active && !running_on && !held_off) {
			if (motor(true)) {
				on_since = now;
				buzzes++;
			}
		} else if (running_on && now - on_since >= MAX_ON_MS * 1000ull) {
			motor(false);
			held_off = true;
			capped++;
		} else if (running_on && !active && now - on_since >= MIN_ON_MS * 1000ull) {
			motor(false);
		}
		IOSleep(5);
	}
}

static void
vib_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t ms = 200;

	say("vib: level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ? "the motor on once" :
	    "the motor for iOS until the run ends");
	if (find()) {
		look();
		if (level == 2) {
			PE_parse_boot_argn("haptic_ms", &ms, sizeof(ms));
			ms = ms < 20 ? 20 : ms > 1000 ? 1000 : ms;
			if (motor(true)) {
				say("vib: THE MOTOR RUNS for %u ms\n", ms);
				IOSleep(ms);
			}
			motor(false);
			look();
		} else if (level >= 3) {
			serve_ios();
		}
	}
	thread_terminate(current_thread());
}

void
hakios_haptic_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("haptic", &level, sizeof(level)) || level == 0) {
		return;
	}
	level = level > 3 ? 3 : level;
	if (kernel_thread_start(vib_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: the motor off (no locks: a stopped processor may hold one). */
void
hakios_haptic_quiesce(void)
{
	if (level == 0 || !vib_found) {
		return;
	}
	leaving = true;
	if (running_on && vib_writable && vib_write != 0) {
		wr(vib_write + CHANNEL_WDATA0, 0);
		wr(vib_write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)VIB_EN_CTL << 4);
		(void)wait_done(vib_write);
		running_on = false;
	}
	if (level >= 3) {
		say("vib: at the run's end: %u vibrations (%u cut at %u ms), from %llu frames iOS wrote; the motor is off\n", buzzes,
		    capped, MAX_ON_MS, written_frames);
	}
}

/*
 * A test's stand-in for iOS's waveform (the microphone driver's mic_tap=1: it hears the motor), as the common
 * driver's: as if a write of this loudness (of 32767) had just come, or silence for 0.
 */
void hakios_haptic_pretend(uint32_t peak);

void
hakios_haptic_pretend(uint32_t peak)
{
	if (peak > SILENT_BELOW) {
		heard_us = now_us();
	} else {
		quiet_since_us = now_us();
	}
}
