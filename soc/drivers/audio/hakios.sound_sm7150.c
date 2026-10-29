/*
 * The SM7150's sound for the Mi 9T (devices/davinci), in place of drivers/audio/hakios.sound.c: the common driver with
 * what the SM7150 and this phone need woven through it -- the primary MI2S interface (AFE port 0x1000, GPIO 49 to 52 of
 * the west tile) instead of redwood's TDM, its bit clock from the DSP, the audio SMMU stream 0x1b21, FastRPC's memory
 * behind the microphones', and the amplifier the speaker interface reaches (drivers/audio/hakios.tfa9874.c), whose level
 * is the samples' (hakios_tfa_gain_q15()). What follows is the common driver's own description.
 */
/*
 * The phone's sound, step 3: what the audio DSP's services are told so that the speakers play.
 * Every address and number below belongs to the POCO X5 Pro (redwood), and nothing here happens
 * unless boot-arg sound=N asks for it, with adsp=1 (hakios.adsp.c), which starts the DSP and
 * calls hakios_sound_dsp_up() once its audio services are up.
 *
 * Under Android the speakers' sound goes from the audio server through three of the DSP's
 * services: ASM, a stream the samples are written into; ADM, the matrix that routes a stream to
 * a device path ("COPP"); and AFE, the port at the SoC's interface. The two amplifiers
 * (hakios.speaker.c) sit on the primary TDM interface, which the DSP runs: Android's
 * PRI_TDM_RX_0, AFE port 0x9000 in group 0x9100 (with 0x9002, unused). The samples themselves
 * lie in memory of ours that the DSP reads through the application processors' SMMU, as
 * stream 0x1801 (Android: qcom,msm-audio-ion, iommus <&apps_smmu 0x1801 0>, its addresses from
 * 0x10000000, and the stream's low four bits in bits 32 to 35 of every address the DSP is given:
 * qcom,smmu-sid-mask 0xf, msm_audio_ion.c).
 *
 *   sound=1  the port: the TDM pins to the interface, the interface's bit clock, the group, the
 *            port, started; whether the bit clock and the frame sync then toggle at the pins is
 *            looked at; held for sound_secs (default 3), then everything stopped and put back
 *   sound=2  ... and a stream: our memory shown to the DSP through the SMMU, a device path
 *            (ADM) to the port, an ASM stream routed to it, and a test tone -- 1 kHz at -20 dBFS,
 *            both channels -- written into it for sound_secs (at most 3); the DSP's "written"
 *            events say whether it takes the samples at 48 kHz. The amplifiers stay off.
 *   sound=3  ... and the amplifiers, with the tone playing: set up as Android's driver sets them
 *            up, started once the clock runs, faded in to sound_below eighths of a dB under
 *            Android's own level (default 160: 20 dB under, the least it may be is 0), faded
 *            out and powered down at the end. Audible.
 *   sound=4  what iOS plays (/dev/hakios-audio, hakios.audio.c), until the run ends: the port
 *            and the stream as above, kept running (silence when iOS writes nothing); the
 *            amplifiers set up once and powered down, started and faded in (the driver's quick
 *            fade) whenever a write is not silence, stopped after three seconds of silence; their
 *            level is what is set on the plug-in's volume control (ioctl HAKIOS_AUDIO_SET_VOLUME):
 *            so far under sound_below, which stays the loudest, and at 0 they stay off. (iOS sets
 *            that control only once, to the top, and applies its volume to the samples itself --
 *            see userland/src/audio_plugin.c --; at its volume 0 it writes silence, and the
 *            amplifiers stay off for that too.) On their way the samples go through the speakers'
 *            guard (hakios.guard.c), in place of the DSP's speaker protection that Android has:
 *            sound_gain eighths of a dB of gain (default and most 96, 12 dB: iOS's own processing
 *            keeps its top that far under full scale), a high-pass filter, three averages of the
 *            power at the speakers and a peak limiter. sound_guard_as=N, for a test: the guard
 *            reckons the amplifiers N eighths of a dB under Android's level (louder than they are)
 *            and holds back as it would there -- with sound_below=720 (the amplifiers at -90 dB)
 *            what it does at the loudest can be seen in the log and the recording without a sound.
 *
 * The numbers are Android's own, from the phone's live device tree (qcom,msm-dai-tdm-pri-rx and
 * its port, the pins pri_tdm_*_active), its mixer paths (the speaker: PRI_TDM_RX_0 at 48 kHz,
 * two channels, S32_LE) and its kernel (Xiaomi's redwood-s-oss, techpack/audio: asoc/lahaina.c,
 * asoc/msm-dai-q6-v2.c, dsp/q6afe.c, dsp/q6adm.c, dsp/q6asm.c):
 *   - the bit clock: the interface's internal one (clock 0x200, "PRI_TDM_IBIT"), attribute 4
 *     ("invert, couple no", msm-dai-q6-v2.c's default; the tree names none), at 48000 x 32 bits x
 *     2 slots = 3.072 MHz -- the machine driver sets that rate itself (lahaina.c, which on redwood
 *     halves its four slots to two: "Using default tdm max slot: 4" in Android's log), not the
 *     tree's 1.536 MHz
 *   - the group: ports 0x9000 and 0x9002, two 32-bit slots, mask 3, 48 kHz
 *   - the port: two channels of 32 bits at 48 kHz, linear PCM; sync mode 0, sync source 1, data
 *     out 0, inverted sync 1, data delay 1; slot mapping: channels at byte offsets 0 and 4 (slot
 *     0, the amplifier at 0x35; slot 1, the one at 0x34), MSB-aligned
 *   - the pins: GPIO 97 (bit clock) and 100 (frame sync) function mi2s0, 8 mA, no pull, output
 *     high; 98 and 99 (data in and out) function mi2s0, 8 mA, no pull (pinctrl-yupik.c: function
 *     1 for all four)
 *   - the path: ADM's device open (version 5) for playback to port 0x9000 with no processing
 *     (topology 0x10312), two channels, 24 bits, 48 kHz; the stream: ASM session 1, stream 1,
 *     opened for writing linear PCM of 24 bits (in 32), no processing (0x10c68), routed to that path
 *
 * What is started is stopped, and what is written is put back, when the time is over -- or, if
 * the DSP is to be shut down earlier (the run's end), before that (hakios_sound_dsp_down()).
 * The amplifiers are powered down on a panic's way out (hakios_sound_quiesce()).
 *
 * Message layouts: the files above, and Linux's sound/soc/qcom/qdsp6/ (q6afe.c, q6adm.c,
 * q6asm.c); the SMMU: hakios.wifi.c, which does the same for the Wi-Fi's stream.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.audio.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

/* The SMMU's match registers and contexts are found free and taken by the Wi-Fi's start as well (hakios.wifi.c). */
extern lck_mtx_t hakios_smmu_lock;

static LCK_GRP_DECLARE(sound_lock_group, "hakios-sound");
static LCK_MTX_DECLARE(sound_lock, &sound_lock_group);  /* the path being set up or taken down */

static uint32_t level, seconds = 3, below = 240, pause_ms = 1000, gain = HAKIOS_GUARD_MAX_GAIN;
static uint32_t guard_as = UINT32_MAX;  /* boot-arg sound_guard_as=N: the guard reckons the amplifiers at most N under (a test) */
static char last_write[96] = "nothing written";

/* ---- what is said: to the loader's log as well, as it happens ---------------------------------------- */

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

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

/* The time of day (UTC, as the host gave it: boot-arg epoch) and since the kernel began, for whoever listens. */
static void
say_when(const char *what)
{
	clock_sec_t secs;
	clock_usec_t usecs;
	uint64_t up = now_us();

	clock_get_calendar_microtime(&secs, &usecs);
	say("sound: %s at %02lu:%02lu:%02lu.%01u UTC (%llu.%01llu s after the kernel began)\n", what, (unsigned long)(secs / 3600 % 24),
	    (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60), (unsigned)(usecs / 100000), up / 1000000, up / 100000 % 10);
}

/* Time for the loader to note what has just been said, before something not yet seen to be harmless. */
static void
announce(const char *what)
{
	say("sound: %s\n", what);
	if (pause_ms) {
		sleep_ms(pause_ms);
	}
}

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
	value = rd(b->base + offset);
	say("sound: %s %s (%#x) = %#x\n", b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
	say("sound: about to write %s %s at %#x: %#x\n", b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "wrote %#x to %s %s at %#x", value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}

/* ---- the TDM pins ------------------------------------------------------------------------------------- */

static struct block tlmm = { "pin controller", 0x0f100000, 0x100000 };
#define GPIO_CFG(n)             (0x1000u * (n))         /* [1:0] pull, [5:2] function, [8:6] drive (mA / 2 - 1), [9] output */
#define GPIO_IO(n)              (0x1000u * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_MI2S0               (1u << 2)
#define CFG_8MA                 (3u << 6)
#define CFG_OUTPUT              (1u << 9)

static const struct {
	unsigned pin;
	const char *name;
	uint32_t cfg, io;               /* Android's active state */
} tdm_pins[] = {
	{ 97, "gpio97 (bit clock)", CFG_MI2S0 | CFG_8MA | CFG_OUTPUT, 2 },
	{ 100, "gpio100 (frame sync)", CFG_MI2S0 | CFG_8MA | CFG_OUTPUT, 2 },
	{ 98, "gpio98 (data in)", CFG_MI2S0 | CFG_8MA, 0 },
	{ 99, "gpio99 (data out)", CFG_MI2S0 | CFG_8MA, 0 },
};
#define TDM_PINS                (sizeof(tdm_pins) / sizeof(tdm_pins[0]))

/*
 * The Mi 9T (davinci, SM7150): its one speaker's amplifier, an NXP TFA9874 (I2C 0x34 on GPIO 53 and 54),
 * is fed by the primary MI2S interface (dt: dai_mi2s0, qcom,msm-dai-q6-mi2s-dev-id 0, one line out),
 * whose pins are in the pin controller's west tile (0x3500000; Linux's pinctrl-sm7150.c): GPIO 49 the
 * bit clock, 50 the word select, both function 1 (pri_mi2s, pri_mi2s_ws), 51 the data out (SD0) and
 * 52 the data in (SD1), function 1 too.
 */
static const struct {
	unsigned pin;
	const char *name;
	uint32_t cfg, io;
} mi2s_pins[] = {
	{ 49, "gpio49 (bit clock)", CFG_MI2S0 | CFG_8MA | CFG_OUTPUT, 2 },
	{ 50, "gpio50 (word select)", CFG_MI2S0 | CFG_8MA | CFG_OUTPUT, 2 },
	{ 51, "gpio51 (data out)", CFG_MI2S0 | CFG_8MA, 0 },
	{ 52, "gpio52 (data in)", CFG_MI2S0 | CFG_8MA, 0 },
};
extern uint32_t hakios_tfa_gain_q15(void);     /* devices/davinci/drivers/audio/hakios.tfa9874.c */
static bool mi2s;                       /* the Mi 9T's way: chosen in hakios_sound_dsp_up() */
#define PIN(i)                  (mi2s ? mi2s_pins[i].pin : tdm_pins[i].pin)
#define PIN_NAME(i)             (mi2s ? mi2s_pins[i].name : tdm_pins[i].name)
#define PIN_CFG(i)              (mi2s ? mi2s_pins[i].cfg : tdm_pins[i].cfg)
#define PIN_IO(i)               (mi2s ? mi2s_pins[i].io : tdm_pins[i].io)
static uint32_t found_cfg[TDM_PINS], found_io[TDM_PINS];
static bool pins_taken;
/* the amplifiers: redwood's two Awinic ones (hakios.speaker.c) or the Mi 9T's TFA9874 (hakios.tfa.c) */
#define AMP_PREPARE(f, n) hakios_speaker_prepare(f, n)
#define AMP_START()     hakios_speaker_start()
#define AMP_VOLUME(b, ms) hakios_speaker_volume(b, ms)
#define AMP_HEALTHY()   hakios_speaker_healthy()
#define AMP_STOP()      hakios_speaker_stop()
#define AMP_QUIESCE()   hakios_speaker_quiesce()

/*
 * The Mi 9T's amplifier has no level of its own: its factor (Q15) on the samples, here, as they go into a period:
 * 24 bits in the top of each 32-bit word (Q31, what the DSP takes for 24 bits: apr_audio-v2.h). Turned down in 16
 * bits, the quiet sounds kept 8 or 9 bits and were heard distorted (26 October); in 24 bits they keep 16.
 */
static void
amp_pack(const int16_t *in, int32_t *out, unsigned count)
{
	int64_t q15 = mi2s ? hakios_tfa_gain_q15() : 32768;

	if (q15 > 32768) {
		q15 = 32768;
	}
	for (unsigned i = 0; i < count; i++) {
		out[i] = (int32_t)(((int64_t)in[i] * q15) >> 7) << 8;  /* 16 x Q15 >> 7: 24 bits, into the top */
	}
}

static void
pins_on(void)
{
	for (unsigned i = 0; i < TDM_PINS; i++) {
		found_cfg[i] = get(&tlmm, PIN_NAME(i), GPIO_CFG(PIN(i)));
		found_io[i] = get(&tlmm, PIN_NAME(i), GPIO_IO(PIN(i)));
	}
	pins_taken = true;
	for (unsigned i = 0; i < TDM_PINS; i++) {
		put(&tlmm, PIN_NAME(i), GPIO_IO(PIN(i)), PIN_IO(i));
		put(&tlmm, PIN_NAME(i), GPIO_CFG(PIN(i)), PIN_CFG(i));
	}
}

static void
pins_off(void)
{
	if (!pins_taken) {
		return;
	}
	for (unsigned i = 0; i < TDM_PINS; i++) {
		put(&tlmm, PIN_NAME(i), GPIO_CFG(PIN(i)), found_cfg[i]);
		put(&tlmm, PIN_NAME(i), GPIO_IO(PIN(i)), found_io[i] & 2);
		get(&tlmm, PIN_NAME(i), GPIO_CFG(PIN(i)));
	}
	pins_taken = false;
}

/* Whether a pin toggles: read many times as fast as it goes. */
static void
pin_activity(unsigned pin, const char *name, uint32_t *changes, uint32_t *ones)
{
	uint32_t last = rd(tlmm.base + GPIO_IO(pin)) & 1, reads = 20000;
	uint64_t began = now_us();

	*changes = *ones = 0;
	for (uint32_t i = 0; i < reads; i++) {
		uint32_t now = rd(tlmm.base + GPIO_IO(pin)) & 1;

		*changes += now != last;
		*ones += now;
		last = now;
	}
	say("sound: %s: %u changes and %u ones in %u reads (%llu us)\n", name, *changes, *ones, reads, now_us() - began);
}

/* ---- APR ------------------------------------------------------------------------------------------- */

#define APR_SVC_AFE             4
#define APR_SVC_ASM             7
#define APR_SVC_ADM             8
#define APR_BASIC_RSP_RESULT    0x000110e8u
#define APR_WAIT_MS             1000

static const char *
dsp_error(uint32_t status)
{
	static const char *const names[] = { "ok", "failed", "bad parameter", "unsupported", "version", "unexpected", "panic",
		                             "no resource", "bad handle", "already", "not ready", "pending", "busy", "aborted",
		                             "preempted", "continue", "immediate", "not implemented", "need more", "no memory",
		                             "not there" };

	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "an error this does not know";
}

/*
 * One command, its answer waited for: true if the answer is the basic one with status 0, or the
 * opcode `wanted' (then in *answer).
 */
static bool
command(const char *what, uint8_t service, uint16_t port, uint32_t opcode, const void *payload, uint32_t length, uint32_t wanted,
    struct hakios_apr_answer *answer)
{
	struct hakios_apr_answer local;

	if (answer == NULL) {
		answer = &local;
	}
	snprintf(last_write, sizeof(last_write), "sent %s (opcode %#x)", what, opcode);
	if (!hakios_adsp_apr(service, port, 0, opcode, payload, length, APR_WAIT_MS, answer)) {
		say("sound: %s (opcode %#x to service %u, port %#x): NO ANSWER\n", what, opcode, service, port);
		return false;
	}
	if (answer->opcode == APR_BASIC_RSP_RESULT && answer->words[0] == opcode) {
		say("sound: %s: %s%s\n", what, answer->words[1] == 0 ? "done" : "REFUSED: ", answer->words[1] == 0 ? "" :
		    dsp_error(answer->words[1]));
		return answer->words[1] == 0 && wanted == 0;
	}
	if (wanted != 0 && answer->opcode == wanted) {
		return true;
	}
	say("sound: %s: an answer not expected: opcode %#x, %u bytes: %#x %#x\n", what, answer->opcode, answer->length,
	    answer->words[0], answer->words[1]);
	return false;
}

/* ---- AFE: the port ---------------------------------------------------------------------------------- */

#define AFE_PORT_CMD_DEVICE_START               0x000100e5u
#define AFE_PORT_CMD_DEVICE_STOP                0x000100e6u
#define AFE_PORT_CMD_SET_PARAM_V2               0x000100efu
#define AFE_SVC_CMD_SET_PARAM                   0x000100f3u
#define AFE_MODULE_AUDIO_DEV_INTERFACE          0x0001020cu
#define AFE_PARAM_ID_TDM_CONFIG                 0x0001029du
#define AFE_MODULE_TDM                          0x0001028au
#define AFE_PARAM_ID_PORT_SLOT_MAPPING_CONFIG   0x00010297u
#define AFE_MODULE_GROUP_DEVICE                 0x00010254u
#define AFE_PARAM_ID_GROUP_DEVICE_ENABLE        0x00010256u
#define AFE_PARAM_ID_GROUP_DEVICE_TDM_CONFIG    0x0001029eu
#define AFE_MODULE_CLOCK_SET                    0x0001028fu
#define AFE_PARAM_ID_CLOCK_SET                  0x00010290u

#define PORT                    port_id         /* 0x9000 PRI_TDM_RX_0; the Mi 9T 0x1000 PRIMARY_MI2S_RX */
static uint16_t port_id = 0x9000;
#define GROUP                   0x9100          /* PRI_TDM_RX */
#define GROUP_OTHER_PORT        0x9002
#define PORT_INVALID            0xffff
#define CLOCK_PRI_TDM_IBIT      0x200
#define CLOCK_PRI_MI2S_IBIT     0x100           /* Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT */
#define AFE_PARAM_ID_I2S_CONFIG 0x0001020du
/*
 * The Mi 9T: 24-bit stereo at 48 kHz on one line, in 32-bit slots (3.072 MHz), as Android runs it (its mixer paths:
 * PRIM_MI2S_RX Format S24_LE; audio_platform_info: the speaker's bit_width 24; the machine driver clocks S24_LE as 32
 * bits a slot). At 16 bits (1.536 MHz) the amplifier reported a TDM error and the sound was distorted (26 October).
 */
#define MI2S_BITS               24
#define MI2S_SLOT_BITS          32
#define CLOCK_INVERT_COUPLE_NO  4
#define CLOCK_COUPLE_NO         1               /* the MI2S bit clock not inverted: Android's (sm6150.c mi2s_clk[]); inverted, the
						   amplifier took its bits on the wrong edge: TDM errors and distortion (26 October) */
#define RATE                    48000
#define SLOTS                   2
#define SLOT_BITS               32
#define PORT_BITS               32              /* the mixer path's S32_LE */

struct afe_param {
	uint32_t module, param;
	uint16_t size, reserved;
} __attribute__((packed));

struct afe_clock_set {
	uint32_t version, id, hz;
	uint16_t attribute, root;
	uint32_t enable;
} __attribute__((packed));

struct afe_group_tdm {
	uint32_t version;
	uint16_t group, reserved;
	uint16_t ports[8];
	uint32_t channels, rate, bits;
	uint16_t slots, slot_bits;
	uint32_t slot_mask;
} __attribute__((packed));

struct afe_group_enable {
	uint16_t group, enable;
} __attribute__((packed));

/* afe_param_id_i2s_cfg (q6afe.c, apr_audio-v2.h) */
struct afe_i2s {
	uint32_t version;
	uint16_t bits, channel_mode, mono_stereo, ws_src;
	uint32_t rate;
	uint16_t data_format, reserved;
} __attribute__((packed));
#define I2S_SD0                 1               /* AFE_PORT_I2S_SD0 */
#define I2S_STEREO              1
#define I2S_WS_INTERNAL         1

struct afe_tdm {
	uint32_t version, channels, rate, bits;
	uint16_t data_format, sync_mode, sync_source, slots, data_out, invert_sync, data_delay, slot_bits;
	uint32_t slot_mask;
} __attribute__((packed));

struct afe_slot_mapping {
	uint32_t version;
	uint16_t channels, bits;
	uint32_t align;
	uint16_t offsets[8];
} __attribute__((packed));

/* A parameter for the AFE as a whole (its clocks, its groups): in the packet, not in shared memory. */
static bool
afe_service_param(const char *what, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t packet[16 + sizeof(struct afe_param) + 64];
	uint32_t *head = (uint32_t *)packet;
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	head[0] = (uint32_t)sizeof(*p) + size;          /* payload size; address 0 and handle 0: in band */
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return command(what, APR_SVC_AFE, 0, AFE_SVC_CMD_SET_PARAM, packet, 16 + (uint32_t)sizeof(*p) + size, 0, NULL);
}

/* A parameter of the port. */
static bool
afe_port_param(const char *what, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t packet[16 + sizeof(struct afe_param) + 64];
	struct afe_param *p = (struct afe_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	*(uint16_t *)packet = PORT;
	*(uint16_t *)(packet + 2) = (uint16_t)(sizeof(*p) + size);
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return command(what, APR_SVC_AFE, 0, AFE_PORT_CMD_SET_PARAM_V2, packet, 16 + (uint32_t)sizeof(*p) + size, 0, NULL);
}

static bool clock_on, group_on, port_on;

static bool
afe_clock(bool on)
{
	struct afe_clock_set clock = { 1, CLOCK_PRI_TDM_IBIT, RATE * SLOT_BITS * SLOTS, CLOCK_INVERT_COUPLE_NO, 0, on };

	return afe_service_param(on ? "the TDM bit clock (0x200) at 3.072 MHz, on" : "the TDM bit clock, off", AFE_MODULE_CLOCK_SET,
	           AFE_PARAM_ID_CLOCK_SET, &clock, sizeof(clock));
}

static bool
afe_on(void)
{
	struct afe_group_tdm group = {
		.version = 1, .group = GROUP, .ports = { PORT, GROUP_OTHER_PORT, PORT_INVALID, PORT_INVALID, PORT_INVALID,
		                                         PORT_INVALID, PORT_INVALID, PORT_INVALID },
		.channels = SLOTS, .rate = RATE, .bits = SLOT_BITS, .slots = SLOTS, .slot_bits = SLOT_BITS, .slot_mask = 3,
	};
	struct afe_group_enable enable = { GROUP, 1 };
	struct afe_tdm tdm = {
		.version = 1, .channels = 2, .rate = RATE, .bits = PORT_BITS, .data_format = 0, .sync_mode = 0, .sync_source = 1,
		.slots = SLOTS, .data_out = 0, .invert_sync = 1, .data_delay = 1, .slot_bits = SLOT_BITS, .slot_mask = 3,
	};
	struct afe_slot_mapping mapping = { .version = 1, .channels = 2, .bits = PORT_BITS, .align = 0,
		                            .offsets = { 0, 4, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff } };
	uint16_t start[2] = { PORT, 0 };

	if (mi2s) {
		struct afe_clock_set bit_clock = { 1, CLOCK_PRI_MI2S_IBIT, RATE * MI2S_SLOT_BITS * 2, CLOCK_COUPLE_NO, 0, 1 };
		struct afe_i2s i2s = { .version = 1, .bits = MI2S_BITS, .channel_mode = I2S_SD0, .mono_stereo = I2S_STEREO,
			               .ws_src = I2S_WS_INTERNAL, .rate = RATE, .data_format = 0 };

		announce("the DSP is asked for the primary MI2S interface's bit clock, the port and its start (AFE)");
		clock_on = afe_service_param("the MI2S bit clock (0x100) at 3.072 MHz, on", AFE_MODULE_CLOCK_SET,
		    AFE_PARAM_ID_CLOCK_SET, &bit_clock, sizeof(bit_clock));
		if (!clock_on || !afe_port_param("the port 0x1000: I2S, data line 0, stereo, 24 bits (32-bit slots) at 48 kHz, word select ours",
		    AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_I2S_CONFIG, &i2s, sizeof(i2s))) {
			return false;
		}
		port_on = command("the port 0x1000, started", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_START, start, sizeof(start), 0,
		    NULL);
		return port_on;
	}
	announce("the DSP is asked for the TDM interface's bit clock, the group, the port and its start (AFE)");
	clock_on = afe_clock(true);
	if (!clock_on) {
		return false;
	}
	group_on = afe_service_param("the group 0x9100: ports 0x9000 0x9002, 2 slots of 32 bits, 48 kHz", AFE_MODULE_GROUP_DEVICE,
	    AFE_PARAM_ID_GROUP_DEVICE_TDM_CONFIG, &group, sizeof(group)) &&
	    afe_service_param("the group 0x9100, on", AFE_MODULE_GROUP_DEVICE, AFE_PARAM_ID_GROUP_DEVICE_ENABLE, &enable,
	    sizeof(enable));
	if (!group_on) {
		return false;
	}
	if (!afe_port_param("the port 0x9000: 2 channels of 32 bits at 48 kHz in 2 slots, sync 0 1, inverted, delay 1",
	    AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_TDM_CONFIG, &tdm, sizeof(tdm)) ||
	    !afe_port_param("the port's slots: channels at bytes 0 and 4", AFE_MODULE_TDM, AFE_PARAM_ID_PORT_SLOT_MAPPING_CONFIG,
	    &mapping, sizeof(mapping))) {
		return false;
	}
	port_on = command("the port 0x9000, started", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_START, start, sizeof(start), 0, NULL);
	return port_on;
}

static void
afe_off(void)
{
	uint16_t stop[2] = { PORT, 0 };
	struct afe_group_enable disable = { GROUP, 0 };

	if (port_on) {
		command(mi2s ? "the port 0x1000, stopped" : "the port 0x9000, stopped", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_STOP, stop, sizeof(stop), 0, NULL);
		port_on = false;
	}
	if (group_on) {
		afe_service_param("the group 0x9100, off", AFE_MODULE_GROUP_DEVICE, AFE_PARAM_ID_GROUP_DEVICE_ENABLE, &disable,
		    sizeof(disable));
		group_on = false;
	}
	if (clock_on && mi2s) {
		struct afe_clock_set bit_clock = { 1, CLOCK_PRI_MI2S_IBIT, RATE * MI2S_SLOT_BITS * 2, CLOCK_COUPLE_NO, 0, 0 };

		afe_service_param("the MI2S bit clock, off", AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, &bit_clock, sizeof(bit_clock));
		clock_on = false;
	}
	if (clock_on) {
		afe_clock(false);
		clock_on = false;
	}
}

/* ---- the DSP's view of our memory: stream 0x1801 of the SMMU ------------------------------------------- */

static struct block apps_smmu = { "apps smmu", 0x15000000, 0x2000 };
static struct block apps_smmu_second = { "apps smmu", 0, 0x1000 };              /* the second page: a word per context */
static struct block apps_smmu_context = { "apps smmu, the audio context", 0, 0x1000 };
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
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
#define PTE_ADDRESS             0x0000fffffffff000ull
#define AUDIO_PTE               (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)       /* as hakios.wifi.c's */

#define AUDIO_STREAM            audio_stream    /* 0x1801; the Mi 9T 0x1b21 (its msm-audio-ion: iommus <&apps_smmu 0x1b21 0>) */
static uint32_t audio_stream = 0x1801;
#define AUDIO_ADDRESSES         0x10000000ull   /* dt: qcom,iommu-dma-addr-pool of qcom,msm-audio-ion */
#define AUDIO_STREAM_BITS       (1ull << 32)    /* the stream's low bits (0x1801 & 0xf), where the DSP takes them from */

#define PERIODS                 8
#define PERIOD_FRAMES           960             /* 20 ms */
#define PERIOD_BYTES            (PERIOD_FRAMES * 8)     /* two channels of 32 bits: 24 at the top (amp_pack()) */
#define PERIOD_STRIDE           8192                    /* two pages each */
#define BUFFER_BYTES            (PERIODS * PERIOD_STRIDE)
#define MIC_BYTES               (16 * 4096)             /* the microphones' (hakios.mic.c), behind the periods */
#define FASTRPC_BYTES           (64 * 4096)             /* FastRPC's calls (hakios.adsp.c), behind the microphones': two sessions, a read of 64 KB fits each half */
#define SHARED_BYTES            (BUFFER_BYTES + MIC_BYTES + FASTRPC_BYTES)
/*
 * The Mi 9T's FastRPC session (dt: adsprpc-smd, iommus <&apps_smmu 0x1b23 0>) goes through the same
 * context, by a second match register (the SMMU takes no mask in one: a match register written with
 * one keeps its old value).
 */
#define FASTRPC_STREAM          0x1b23
#define FASTRPC_STREAM_BITS     (3ull << 32)

static struct {
	bool found, on, passed;         /* passed: the boot chain lets the stream through untranslated */
	uint32_t page, pages, match, context, old_smr, old_s2cr, old_sctlr;
	uint32_t match2, old_smr2, old_s2cr2;   /* FastRPC's stream (the Mi 9T); match2 ~0u: none */
	vm_offset_t tables;             /* three of 4K: the root, one of the second level, one of the third */
} space;
static vm_offset_t buffer;              /* BUFFER_BYTES of ours, the periods (and MIC_BYTES behind them: the microphones') */
static uint64_t buffer_address;         /* as the DSP is to know it */

/* Context n has our settings (39-bit addresses, our attributes): a kernel before this one set it up. */
static bool
space_left_ours(uint32_t n)
{
	vm_offset_t context = ml_io_map(apps_smmu.phys + (space.pages + n) * space.page, 0x1000);
	bool ours = (rd(context + SMMU_CB_TCR) == SMMU_TCR_39_BITS && rd(context + SMMU_CB_MAIR0) == SMMU_MAIR0);

	/* (the page stays mapped: ml_io_unmap() panics here, "PTE write is unaligned") */
	return ours;
}

static bool
space_find(void)
{
	bool used[256] = { false };
	uint32_t scr0 = get(&apps_smmu, "scr0", SMMU_SCR0), idr0 = get(&apps_smmu, "idr0", SMMU_IDR0);
	uint32_t idr1 = get(&apps_smmu, "idr1", SMMU_IDR1), matches = idr0 & 0xff, contexts = idr1 & 0xff;

	if (scr0 & 1u << 3) {
		say("sound: this SMMU numbers its streams with 16 bits, which nothing here is written for\n");
		return false;
	}
	space.page = idr1 >> 31 ? 0x10000 : 0x1000;
	space.pages = 2u << (idr1 >> 28 & 7);
	if (2 * space.pages * space.page != 0x100000) {
		say("sound: the SMMU says it has 2 x %u pages of %#x bytes, not a megabyte: its contexts are not looked for\n",
		    space.pages, space.page);
		return false;
	}
	space.match = space.match2 = ~0u;
	space.context = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n)), s2cr = rd(apps_smmu.base + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if ((smr >> 31) && mask == 0 && (stream == AUDIO_STREAM || (stream == FASTRPC_STREAM && AUDIO_STREAM == 0x1b21)) &&
		    (s2cr >> 16 & 3) == 0 && (s2cr & 0xff) < contexts && (space.context == ~0u || space.context == (s2cr & 0xff)) &&
		    space_left_ours(s2cr & 0xff)) {
			/* (a kernel before this one sent the stream through a context of ours, and did not put it back) */
			say("sound: match register %u (stream %#x, context %u) was left by a kernel before this one: taken\n", n, stream,
			    s2cr & 0xff);
			space.context = s2cr & 0xff;
			if (stream == AUDIO_STREAM) {
				space.match = n;
				space.old_smr = space.old_s2cr = 0;
			} else {
				space.match2 = n;
				space.old_smr2 = space.old_s2cr2 = 0;
			}
			continue;
		}
		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if (((stream ^ AUDIO_STREAM) & ~mask) == 0) {
				say("sound: match register %u covers the audio stream already: stream %#x mask %#x, s2cr %#x\n", n,
				    stream, mask, s2cr);
				if ((s2cr >> 16 & 3) == 1) {
					space.passed = true;
					return true;
				}
				say("sound: ... and does not let it through untranslated: it is somebody's, and is left alone\n");
				return false;
			}
		} else if (space.match == ~0u) {
			space.match = n;
			space.old_smr = smr;
			space.old_s2cr = s2cr;
		} else if (space.match2 == ~0u && AUDIO_STREAM == 0x1b21) {
			space.match2 = n;
			space.old_smr2 = smr;
			space.old_s2cr2 = s2cr;
		}
	}
	if (space.context == ~0u) {
		for (space.context = 0; space.context < contexts && used[space.context]; space.context++) {
		}
	}
	if (space.match == ~0u || space.context >= contexts) {
		say("sound: the SMMU has no match register or no context left\n");
		return false;
	}
	apps_smmu_second.phys = apps_smmu.phys + space.page;
	apps_smmu_context.phys = apps_smmu.phys + (space.pages + space.context) * space.page;
	say("sound: the boot chain has set nothing up for stream %#x; free: match register %u (smr %#x, s2cr %#x) and context %u, at "
	    "%#x\n", AUDIO_STREAM, space.match, space.old_smr, space.old_s2cr, space.context, apps_smmu_context.phys);
	space.old_sctlr = get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR);
	if ((space.old_sctlr & 1) && get(&apps_smmu_context, "tcr", SMMU_CB_TCR) == SMMU_TCR_39_BITS &&
	    get(&apps_smmu_context, "mair0", SMMU_CB_MAIR0) == SMMU_MAIR0) {
		/* (a kernel before this one, whose run ended without putting it back: its settings are ours) */
		say("sound: context %u is on, with our settings: left on by a kernel before this one, and taken\n", space.context);
		space.old_sctlr = 0;
	}
	if (space.old_sctlr & 1) {
		say("sound: CONTEXT %u IS SWITCHED ON although no stream is sent through it: it is somebody's, and is left alone\n",
		    space.context);
		return false;
	}
	space.found = true;
	return true;
}

static void
space_faults(void)
{
	uint32_t status, global;

	if (!space.on) {
		return;
	}
	status = rd(apps_smmu_context.base + SMMU_CB_FSR);
	global = rd(apps_smmu.base + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("sound: the SMMU has refused the DSP nothing (context %u: fsr %#x; sgfsr 0)\n", space.context, status);
		return;
	}
	say("sound: THE SMMU HAS REFUSED A REQUEST: context %u: status %#x, address %#llx, syndrome %#x; the SMMU as a whole: status "
	    "%#x, stream %#x\n", space.context, status, (uint64_t)rd(apps_smmu_context.base + SMMU_CB_FAR + 4) << 32 |
	    rd(apps_smmu_context.base + SMMU_CB_FAR), rd(apps_smmu_context.base + SMMU_CB_FSYNR0), global,
	    rd(apps_smmu.base + SMMU_SGFSYNR1));
}

static void space_off(void);

/* Our buffer behind the DSP's addresses from AUDIO_ADDRESSES on, through a context of ours. */
static bool
space_on(void)
{
	uint64_t *root, *second, *third, table_root;
	uint32_t smr = SMMU_SMR(space.match), s2cr = SMMU_S2CR(space.match), sees = 1u << 31 | AUDIO_STREAM;


	if (kmem_alloc(kernel_map, &space.tables, 3 * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	root = (uint64_t *)space.tables;
	second = (uint64_t *)(space.tables + 4096);
	third = (uint64_t *)(space.tables + 8192);
	root[AUDIO_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	second[AUDIO_ADDRESSES >> 21 & 0x1ff] = kvtophys((vm_offset_t)third) | 3;
	for (unsigned page = 0; page < SHARED_BYTES / 4096; page++) {
		third[(AUDIO_ADDRESSES >> 12 & 0x1ff) + page] = kvtophys(buffer + 4096 * page) | AUDIO_PTE;
	}
	for (vm_offset_t line = space.tables; line < space.tables + 3 * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(space.tables) | (uint64_t)space.context << 48;
	say("sound: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets page tables of ours (root %#llx) for the %u KB of the periods and the microphones' "
	    "at %#llx, and match register %u sends stream %#x through it\n", space.context, table_root & PTE_ADDRESS,
	    SHARED_BYTES / 1024, AUDIO_ADDRESSES, space.match, AUDIO_STREAM);
	if (pause_ms) {
		sleep_ms(pause_ms);
	}
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, 0);
	put(&apps_smmu_second, "cba2r", SMMU_CBA2R(space.context), SMMU_CBA2R_64_BIT);
	put(&apps_smmu_second, "cbar", SMMU_CBAR(space.context), SMMU_CBAR_STAGE_1);
	put(&apps_smmu_context, "tcr2", SMMU_CB_TCR2, SMMU_TCR2);
	put(&apps_smmu_context, "tcr", SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0, (uint32_t)table_root);
	put(&apps_smmu_context, "ttbr0, high", SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32));
	put(&apps_smmu_context, "ttbr1, low", SMMU_CB_TTBR1, 0);
	put(&apps_smmu_context, "ttbr1, high", SMMU_CB_TTBR1 + 4, 0);
	put(&apps_smmu_context, "mair0", SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&apps_smmu_context, "mair1", SMMU_CB_MAIR1, 0);
	put(&apps_smmu_context, "fsr", SMMU_CB_FSR, SMMU_FSR_ALL);
	put(&apps_smmu_context, "tlbiasid", SMMU_CB_TLBIASID, space.context);
	put(&apps_smmu_context, "tlbsync", SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(apps_smmu_context.base + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			say("sound: the SMMU does not finish forgetting old translations\n");
			return false;
		}
		sleep_ms(1);
	}
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	space.on = true;
	put(&apps_smmu, "s2cr", s2cr, space.context);
	put(&apps_smmu, "smr", smr, sees);
	if (space.match2 != ~0u) {
		put(&apps_smmu, "s2cr, FastRPC's", SMMU_S2CR(space.match2), space.context);
		put(&apps_smmu, "smr, FastRPC's", SMMU_SMR(space.match2), 1u << 31 | FASTRPC_STREAM);
		say("sound: FastRPC's stream %#x %s context %u too (match register %u)\n", FASTRPC_STREAM,
		    get(&apps_smmu, "smr, FastRPC's", SMMU_SMR(space.match2)) == (1u << 31 | FASTRPC_STREAM) ? "GOES THROUGH" :
		    "does NOT go through", space.context, space.match2);
	}
	if (get(&apps_smmu, "smr", smr) != sees || (get(&apps_smmu, "s2cr", s2cr) & 0x300ff) != space.context ||
	    !(get(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR) & 1) ||
	    get(&apps_smmu_context, "ttbr0, low", SMMU_CB_TTBR0) != (uint32_t)table_root) {
		say("sound: THE SMMU DID NOT TAKE WHAT WAS WRITTEN: stream %#x is not going through context %u\n", AUDIO_STREAM,
		    space.context);
		space_off();
		return false;
	}
	say("sound: THE AUDIO STREAM GOES THROUGH CONTEXT %u of the SMMU, with our page tables\n", space.context);
	return true;
}

static void
space_off(void)
{
	if (!space.on) {
		return;
	}
	space_faults();
	say("sound: the audio stream and context %u of the SMMU are put back as they were found\n", space.context);
	put(&apps_smmu, "smr", SMMU_SMR(space.match), space.old_smr);
	put(&apps_smmu, "s2cr", SMMU_S2CR(space.match), space.old_s2cr);
	if (space.match2 != ~0u) {
		put(&apps_smmu, "smr, FastRPC's", SMMU_SMR(space.match2), space.old_smr2);
		put(&apps_smmu, "s2cr, FastRPC's", SMMU_S2CR(space.match2), space.old_s2cr2);
	}
	put(&apps_smmu_context, "sctlr", SMMU_CB_SCTLR, space.old_sctlr);
	get(&apps_smmu, "smr", SMMU_SMR(space.match));
	space.on = false;
}

/*
 * The DSP's view of our memory is one: the sound's periods and the microphones' buffers behind them
 * (hakios.mic.c), through the same stream and context. Whoever comes first sets it up, whoever
 * leaves last takes it down. user: 0 the sound, 1 the microphones, 2 FastRPC. With hakios_smmu_lock held.
 */
static uint32_t space_users;

static bool
shared_get(unsigned user)
{
	if (space_users == 0) {
		if (buffer == 0 && kmem_alloc(kernel_map, &buffer, SHARED_BYTES, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			buffer = 0;
			say("sound: no memory for the DSP's buffers\n");
			return false;
		}
		if (!space.found && !space.passed && !space_find()) {
			return false;
		}
		if (!space.passed && !space.on && !space_on()) {
			return false;
		}
		buffer_address = space.passed ? (kvtophys(buffer) | AUDIO_STREAM_BITS) : (AUDIO_ADDRESSES | AUDIO_STREAM_BITS);
		if (space.passed) {
			for (unsigned page = 1; page < SHARED_BYTES / 4096; page++) {
				if (kvtophys(buffer + 4096 * page) != kvtophys(buffer) + 4096 * page) {
					say("sound: the DSP's buffers are not one piece of memory, and the stream is not translated\n");
					return false;
				}
			}
		}
	}
	space_users |= 1u << user;
	return true;
}

static void
shared_put(unsigned user)
{
	if (!(space_users & 1u << user)) {
		return;
	}
	space_users &= ~(1u << user);
	if (space_users == 0) {
		space_off();
	}
}

bool
hakios_audio_dsp_memory(uintptr_t *memory, uint64_t *dsp_address, uint32_t *bytes)
{
	bool ok;

	lck_mtx_lock(&hakios_smmu_lock);
	ok = shared_get(1);
	lck_mtx_unlock(&hakios_smmu_lock);
	if (ok) {
		*memory = buffer + BUFFER_BYTES;
		*dsp_address = buffer_address + BUFFER_BYTES;
		*bytes = MIC_BYTES;
	}
	return ok;
}

static void davinci_choose(void);

bool
hakios_audio_dsp_memory_fastrpc(uintptr_t *memory, uint64_t *dsp_address, uint32_t *bytes)
{
	bool ok;

	davinci_choose();
	lck_mtx_lock(&hakios_smmu_lock);
	ok = shared_get(2);
	lck_mtx_unlock(&hakios_smmu_lock);
	if (ok) {
		*memory = buffer + BUFFER_BYTES + MIC_BYTES;
		*dsp_address = ((buffer_address & 0xffffffffull) + BUFFER_BYTES + MIC_BYTES) |
		    (AUDIO_STREAM == 0x1b21 ? FASTRPC_STREAM_BITS : AUDIO_STREAM_BITS);
		*bytes = FASTRPC_BYTES;
	}
	return ok;
}

void
hakios_audio_dsp_memory_fastrpc_put(void)
{
	lck_mtx_lock(&hakios_smmu_lock);
	shared_put(2);
	lck_mtx_unlock(&hakios_smmu_lock);
}

void
hakios_audio_dsp_memory_put(void)
{
	lck_mtx_lock(&hakios_smmu_lock);
	shared_put(1);
	lck_mtx_unlock(&hakios_smmu_lock);
}

/* ---- ADM and ASM: the path and the stream --------------------------------------------------------------- */

#define ADM_CMD_DEVICE_OPEN_V5                  0x00010326u
#define ADM_CMDRSP_DEVICE_OPEN_V5               0x00010329u
#define ADM_CMD_DEVICE_CLOSE_V5                 0x00010327u
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5          0x00010325u
#define ADM_PATH_PLAYBACK                       1
#define ADM_MATRIX_AUDIO_RX                     0
#define ADM_TOPOLOGY_NONE                       0x00010312u
#define ADM_BITS                                24

#define ASM_CMD_SHARED_MEM_MAP_REGIONS          0x00010d92u
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS       0x00010d93u
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS        0x00010d94u
#define ASM_STREAM_CMD_OPEN_WRITE_V3            0x00010db3u
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2        0x00010d98u
#define ASM_SESSION_CMD_RUN_V2                  0x00010daau
#define ASM_DATA_CMD_WRITE_V2                   0x00010dabu
#define ASM_DATA_EVENT_WRITE_DONE_V2            0x00010d99u
#define ASM_SESSION_EVENT_RX_UNDERFLOW          0x00010c17u
#define ASM_STREAM_CMD_CLOSE                    0x00010bcdu
#define ASM_TOPOLOGY_NONE                       0x00010c68u
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2      0x00010da5u
#define ASM_SHMEM_4K_POOL                       3
#define SESSION                                 1
#define STREAM_PORT                             (SESSION << 8 | 1)
#define WRITE_TOKEN                             0x80000000u     /* | the period: what a write's "done" carries back */
#define PCM_CHANNEL_FL                          1
#define PCM_CHANNEL_FR                          2

static volatile bool stopping;          /* the run is ending: whatever plays stops */
static bool mapped, copp_open, stream_open;
static uint32_t map_handle, copp;
static volatile uint32_t returned;      /* the periods the DSP has said it is done with (a bit each) */
static volatile uint32_t written_done, underflows, write_errors;

/* The DSP's events: the "done" of every write, and underflows. On the link's thread. */
static bool
events(uint8_t service, __unused uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload, uint32_t length)
{
	if (service != APR_SVC_ASM) {
		return false;
	}
	if (opcode == ASM_DATA_EVENT_WRITE_DONE_V2 && (token & WRITE_TOKEN)) {
		if (length >= 16 && payload[3] != 0) {
			OSIncrementAtomic((volatile SInt32 *)&write_errors);
		}
		OSIncrementAtomic((volatile SInt32 *)&written_done);
		OSBitOrAtomic(1u << (token & 31), &returned);
		return true;
	}
	if (opcode == APR_BASIC_RSP_RESULT && (token & WRITE_TOKEN)) {
		/* (a write the DSP refused outright: it answers it as a command) */
		OSIncrementAtomic((volatile SInt32 *)&write_errors);
		OSIncrementAtomic((volatile SInt32 *)&written_done);
		OSBitOrAtomic(1u << (token & 31), &returned);
		return true;
	}
	if (opcode == ASM_SESSION_EVENT_RX_UNDERFLOW) {
		OSIncrementAtomic((volatile SInt32 *)&underflows);
		return true;
	}
	return false;
}

static bool
stream_on(void)
{
	struct {
		uint16_t pool, regions;
		uint32_t property;
		uint32_t low, high, size;
	} __attribute__((packed)) map = { ASM_SHMEM_4K_POOL, 1, 0, (uint32_t)buffer_address, (uint32_t)(buffer_address >> 32),
		                          BUFFER_BYTES };
	struct {
		uint16_t flags, mode, endpoint1, endpoint2;
		uint32_t topology;
		uint16_t channels, bits;
		uint32_t rate;
		uint8_t map[8];
	} __attribute__((packed)) open = { 0, ADM_PATH_PLAYBACK, PORT, 0xffff, ADM_TOPOLOGY_NONE, 2, ADM_BITS, RATE,
		                           { PCM_CHANNEL_FL, PCM_CHANNEL_FR } };
	struct {
		uint32_t matrix, sessions;
		uint16_t session, copps;
		uint16_t copp[2];
	} __attribute__((packed)) route = { ADM_MATRIX_AUDIO_RX, 1, SESSION, 1, { 0, 0 } };
	struct {
		uint32_t flags;
		uint16_t sink, bits;
		uint32_t topology, format;
	} __attribute__((packed)) write = { 0, 0, 24, ASM_TOPOLOGY_NONE, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 };
	struct {
		uint32_t size;
		uint16_t channels, bits;
		uint32_t rate;
		uint16_t is_signed, reserved;
		uint8_t map[8];
	} __attribute__((packed)) format = { 20, 2, 24, RATE, 1, 0, { PCM_CHANNEL_FL, PCM_CHANNEL_FR } };
	uint32_t run[3] = { 0, 0, 0 };
	struct hakios_apr_answer answer;

	announce("the DSP is given our periods (ASM), a device path to the port (ADM) and a stream routed to it (ASM)");
	if (!command("our periods, mapped for the DSP", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_MAP_REGIONS, &map, sizeof(map),
	    ASM_CMDRSP_SHARED_MEM_MAP_REGIONS, &answer)) {
		return false;
	}
	mapped = true;
	map_handle = answer.words[0];
	say("sound: our %u KB at %#llx are the DSP's handle %#x\n", BUFFER_BYTES / 1024, buffer_address, map_handle);
	if (!command("a device path to the port: playback, no processing, 2 channels of 24 bits at 48 kHz", APR_SVC_ADM, PORT,
	    ADM_CMD_DEVICE_OPEN_V5, &open, sizeof(open), ADM_CMDRSP_DEVICE_OPEN_V5, &answer)) {
		return false;
	}
	if (answer.words[0] != 0) {
		say("sound: the device path: REFUSED: %s\n", dsp_error(answer.words[0]));
		return false;
	}
	copp_open = true;
	copp = answer.words[1] & 0xffff;        /* (the upper half is reserved) */
	route.copp[0] = (uint16_t)copp;
	say("sound: the device path is open: COPP %u\n", copp);
	if (!command("the stream, opened for writing: 24-bit linear PCM, no processing", APR_SVC_ASM, STREAM_PORT,
	    ASM_STREAM_CMD_OPEN_WRITE_V3, &write, sizeof(write), 0, NULL)) {
		return false;
	}
	stream_open = true;
	return command("session 1 routed to the device path", APR_SVC_ADM, 0, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, &route, sizeof(route),
	           0, NULL) &&
	       command("the stream's format: 2 channels of 24 bits (the top of 32) at 48 kHz, left and right", APR_SVC_ASM, STREAM_PORT,
	           ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2, &format, sizeof(format), 0, NULL) &&
	       command("the session, run", APR_SVC_ASM, STREAM_PORT, ASM_SESSION_CMD_RUN_V2, run, sizeof(run), 0, NULL);
}

static void
stream_off(void)
{
	uint32_t handle = map_handle;

	if (stream_open) {
		command("the stream, closed", APR_SVC_ASM, STREAM_PORT, ASM_STREAM_CMD_CLOSE, NULL, 0, 0, NULL);
		stream_open = false;
	}
	if (copp_open) {
		command("the device path, closed", APR_SVC_ADM, (uint16_t)copp, ADM_CMD_DEVICE_CLOSE_V5, NULL, 0, 0, NULL);
		copp_open = false;
	}
	if (mapped) {
		command("our periods, unmapped", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, &handle, sizeof(handle), 0, NULL);
		mapped = false;
	}
}

/* ---- the test tone ----------------------------------------------------------------------------------- */

/* 1 kHz at 48 kHz, -20 dBFS (a peak of 3277): one cycle */
static const int16_t tone[48] = {
	0, 428, 848, 1254, 1638, 1995, 2317, 2600, 2838, 3028, 3165, 3249,
	3277, 3249, 3165, 3028, 2838, 2600, 2317, 1995, 1638, 1254, 848, 428,
	0, -428, -848, -1254, -1638, -1995, -2317, -2600, -2838, -3028, -3165, -3249,
	-3277, -3249, -3165, -3028, -2838, -2600, -2317, -1995, -1638, -1254, -848, -428,
};
static uint32_t tone_at;

static void
period_fill(unsigned period, bool silent)
{
	static int16_t samples[2 * PERIOD_FRAMES];
	int32_t *words = (int32_t *)(buffer + PERIOD_STRIDE * period);

	for (unsigned frame = 0; frame < PERIOD_FRAMES; frame++) {
		int16_t sample = silent ? 0 : tone[tone_at++ % 48];

		samples[2 * frame] = sample;
		samples[2 * frame + 1] = sample;
	}
	amp_pack(samples, words, 2 * PERIOD_FRAMES);
	for (vm_offset_t line = (vm_offset_t)words; line < (vm_offset_t)words + PERIOD_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));        /* the DSP reads memory, not our caches */
	}
	__asm__ volatile ("dsb sy");
}

static bool
period_write(unsigned period)
{
	uint32_t write[8] = { (uint32_t)(buffer_address + PERIOD_STRIDE * period), (uint32_t)((buffer_address + PERIOD_STRIDE * period) >> 32),
		              map_handle, PERIOD_BYTES, period, 0, 0, 0 };

	return hakios_adsp_apr(APR_SVC_ASM, STREAM_PORT, WRITE_TOKEN | period, ASM_DATA_CMD_WRITE_V2, write, sizeof(write), 0, NULL);
}

/*
 * The tone, written period after period as the DSP gives them back, for `ms'; then silence until
 * the DSP has given every period back. What came back and how fast is said.
 */
static bool
play(uint32_t ms, bool amplifiers)
{
	uint64_t began, last_look, ended;
	uint32_t sent = 0, done_before = written_done, healthy_looks = 0;
	bool ok = true, tone_over = false;

	returned = 0;
	tone_at = 0;
	for (unsigned p = 0; p < PERIODS; p++) {
		period_fill(p, false);
	}
	began = last_look = now_us();
	for (unsigned p = 0; p < PERIODS; p++) {
		if (!period_write(p)) {
			say("sound: the write of period %u could not be sent\n", p);
			return false;
		}
		sent++;
	}
	for (;;) {
		uint32_t back = returned;
		uint64_t now = now_us();

		if (back != 0) {
			OSBitAndAtomic(~back, &returned);
			for (unsigned p = 0; p < PERIODS; p++) {
				if (!(back & 1u << p) || tone_over) {
					continue;
				}
				period_fill(p, false);
				if (!period_write(p)) {
					say("sound: the write of period %u could not be sent\n", p);
					ok = false;
					tone_over = true;
					break;
				}
				sent++;
			}
		}
		if (!tone_over && (now - began >= ms * 1000ull || stopping)) {
			tone_over = true;
		}
		if (tone_over && written_done - done_before >= sent) {
			break;
		}
		if (now - began > ms * 1000ull + 2000000) {
			say("sound: THE DSP HAS NOT GIVEN EVERY PERIOD BACK 2 s after the end (%u of %u)\n", written_done - done_before,
			    sent);
			ok = false;
			break;
		}
		if (written_done == done_before && now - began > 1000000) {
			say("sound: THE DSP HAS GIVEN NO PERIOD BACK IN A SECOND\n");
			ok = false;
			break;
		}
		if (amplifiers && now - last_look >= 200000) {
			last_look = now;
			healthy_looks++;
			if (!AMP_HEALTHY()) {
				ok = false;
				tone_over = true;
			}
		}
		sleep_ms(2);
	}
	ended = now_us();
	{
		uint32_t done = written_done - done_before;
		uint64_t frames = (uint64_t)done * PERIOD_FRAMES, us = ended - began;

		say("sound: %u periods written, %u given back (%u with an error), %u underflows, in %llu ms: the DSP took %llu frames a "
		    "second%s\n", sent, done, write_errors, underflows, us / 1000, us ? frames * 1000000 / us : 0,
		    amplifiers ? "" : " (the amplifiers off: nothing to hear)");
		if (amplifiers) {
			say("sound: the amplifiers' status looked at %u times while it played\n", healthy_looks);
		}
	}
	return ok;
}

/* ---- sound=4: what iOS plays ------------------------------------------------------------------------------ */

/*
 * /dev/hakios-audio's writer (the audio server's real-time thread, hakios.audio.c) puts frames
 * into a ring and does not wait; the feeder, a thread of ours, takes them from there into each
 * period the DSP gives back (and silence when there is nothing), so the stream never stops
 * while the run lasts. The amplifiers are only on while there is sound: the watcher starts them
 * (and fades them in to sound_below) when a write is not silence, and stops them after three
 * seconds of silence or no writes.
 */
#define RING_FRAMES             32768           /* 0.68 s; a power of two, the indices run on and wrap */
#define SILENCE                 8               /* samples no larger than this are silence (as hakios.audio.c's) */
#define QUIET_BEFORE_OFF_US     3000000ull

static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_head, ring_tail;  /* frames: written by the sink, taken by the feeder */
static volatile uint64_t last_sound_us;         /* when the latest write that was not silence came */
static volatile bool feeder_runs, watcher_runs;
static uint64_t fed_frames, silent_frames, ios_frames;
static uint32_t partial_periods;        /* periods part sound, part silence: a gap heard (the end of a sound is one) */
static volatile uint32_t ios_peak;              /* the largest sample since the amplifiers last started */

/* From the writer's thread, under hakios.audio.c's lock: never waits. */
static uint32_t
ios_sink(const int16_t *frames, uint32_t count)
{
	uint32_t head = ring_head, room = RING_FRAMES - (head - ring_tail), taken = count < room ? count : room;
	bool sound = false;

	for (uint32_t i = 0; i < taken; i++) {
		int16_t left = frames[2 * i], right = frames[2 * i + 1];
		uint32_t l = left < 0 ? (uint32_t)-(int32_t)left : (uint32_t)left, r = right < 0 ? (uint32_t)-(int32_t)right : (uint32_t)right;

		ring[2 * ((head + i) % RING_FRAMES)] = left;
		ring[2 * ((head + i) % RING_FRAMES) + 1] = right;
		sound |= l > SILENCE || r > SILENCE;
		if (l > ios_peak || r > ios_peak) {
			ios_peak = l > r ? l : r;
		}
	}
	OSMemoryBarrier();
	ring_head = head + taken;
	ios_frames += taken;
	if (sound) {
		last_sound_us = now_us();
	}
	return taken;
}

static uint32_t guard_periods;                  /* periods through the guard */
static volatile bool guard_on;                  /* the amplifiers are on: what the guard lets through is heard */

/* tenths of a dB as text: "-5.7" */
static const char *
db_text(char *text, int tenths)
{
	snprintf(text, 16, "%s%d.%d", tenths < 0 ? "-" : "", (tenths < 0 ? -tenths : tenths) / 10, (tenths < 0 ? -tenths : tenths) % 10);
	return text;
}

/* What the guard has done since it was last said (hakios.guard.c), as one line; nothing if `quiet'. */
static void
guard_say(const char *when, bool quiet)
{
	struct hakios_guard_report r;
	char t[6][16];

	hakios_guard_take_report(&r);
	if (r.blocks == 0 || quiet) {
		return;
	}
	/* (levels at the speakers: dB under a full-scale square wave at Android's level; gains in dB) */
	say("sound: the guard%s: peaks in %u, out %u (of 32768); gain now %s dB, least %s dB, held down by the 0.2 s/3 s/60 s "
	    "averages in %u/%u/%u of %u periods; peaks limited in %u frames (by %s dB at most); averages %s/%s/%s dB\n",
	    when, r.peak_in, r.peak_out, db_text(t[0], hakios_guard_decibels(r.gain * r.gain >> 30)),
	    db_text(t[1], hakios_guard_decibels(r.least_gain * r.least_gain >> 30)), r.rms_blocks, r.fast_blocks, r.slow_blocks,
	    r.blocks, r.limited_frames, db_text(t[2], hakios_guard_decibels((uint64_t)r.least_peak_gain * r.least_peak_gain >> 2)),
	    db_text(t[3], hakios_guard_decibels(r.rms)), db_text(t[4], hakios_guard_decibels(r.fast)),
	    db_text(t[5], hakios_guard_decibels(r.slow)));
}

/* A period from the ring, the rest of it silence. */
static void
period_fill_ios(unsigned period)
{
	static int16_t samples[2 * PERIOD_FRAMES];     /* (the feeder's thread only) */
	int32_t *words = (int32_t *)(buffer + PERIOD_STRIDE * period);
	uint32_t tail = ring_tail, available = ring_head - tail, n = available < PERIOD_FRAMES ? available : PERIOD_FRAMES;

	OSMemoryBarrier();
	for (uint32_t i = 0; i < n; i++) {
		samples[2 * i] = ring[2 * ((tail + i) % RING_FRAMES)];
		samples[2 * i + 1] = ring[2 * ((tail + i) % RING_FRAMES) + 1];
	}
	bzero(samples + 2 * n, (PERIOD_FRAMES - n) * 4);
	OSMemoryBarrier();
	ring_tail = tail + n;
	fed_frames += n;
	silent_frames += PERIOD_FRAMES - n;
	partial_periods += n != 0 && n < PERIOD_FRAMES;
	/* the guard, on every period, silence too: its filter and its averages go on (hakios.guard.c) */
	hakios_guard_process(samples, PERIOD_FRAMES);
	if (n != 0 && hakios_audio_recording_fed()) {
		hakios_audio_record_fed(samples, n);       /* (what the guard let through, before the level) */
	}
	amp_pack(samples, words, 2 * PERIOD_FRAMES);
	guard_periods++;
	for (vm_offset_t line = (vm_offset_t)words; line < (vm_offset_t)words + PERIOD_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
}

static void
feeder_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t sent = 0, failed = 0, pending = 0;
	unsigned next = 0;

	returned = 0;
	for (unsigned p = 0; p < PERIODS; p++) {
		period_fill_ios(p);
		sent += period_write(p);
	}
	while (!stopping) {
		uint32_t back = returned;
		static bool was_on;

		if (guard_on && !was_on) {
			guard_say("", true);    /* (what it did to the silence meanwhile is of no interest) */
			guard_periods = 0;
		} else if (was_on && !guard_on) {
			guard_say(" until the amplifiers stopped", false);
		} else if (guard_on && guard_periods >= 250) {
			guard_say(", 5 s", false);
			guard_periods = 0;
		}
		was_on = guard_on;
		if (back != 0) {
			OSBitAndAtomic(~back, &returned);
			pending |= back;
		}
		/*
		 * The periods go back in the order they come (the DSP plays them in the order written), and
		 * each only once the ring holds a whole period: a period filled at once with what there
		 * happened to be, the rest silence, put a gap in the sound every 20 ms (the crackle of
		 * 26 October: iOS writes at its own pace, in chunks of its own size). Silence is filled in
		 * only when the DSP is down to its last two periods (40 ms), as at the end of a sound.
		 */
		while (pending & 1u << next) {
			if (ring_head - ring_tail < PERIOD_FRAMES && __builtin_popcount(pending) < PERIODS - 2) {
				break;
			}
			pending &= ~(1u << next);
			period_fill_ios(next);
			if (period_write(next)) {
				sent++;
			} else {
				failed++;
			}
			next = (next + 1) % PERIODS;
		}
		sleep_ms(1);
	}
	say("sound: the feeder stops: %u periods written (%u could not be sent), %u given back (%u with an error), %u underflows; "
	    "from iOS %llu frames came, %llu went to the DSP, %llu of silence filled in; %u periods part sound, part silence\n", sent,
	    failed, written_done, write_errors, underflows, ios_frames, fed_frames, silent_frames, partial_periods);
	feeder_runs = false;
	thread_terminate(current_thread());
}

/* The amplifiers' level as the guard is to reckon with it (sound_guard_as: louder than it is, for a test). */
static void
guard_level(uint32_t level_below)
{
	hakios_guard_attenuation(level_below < guard_as ? level_below : guard_as);
}

/*
 * The amplifiers are off. The guard goes on at their level all the same: what it lets through now reaches them
 * 160 ms later, behind the periods queued at the DSP, and if that is sound, they will have been started by then
 * (70 ms after it comes). While they are off nothing but silence comes, and the averages fall as a speaker cools.
 * Only when nothing can be heard at all -- iOS's volume at 0, the amplifiers given up on -- is the guard told so.
 */
static void
guard_off(bool unheard)
{
	guard_on = false;
	if (unheard) {
		hakios_guard_attenuation(HAKIOS_GUARD_SILENT);
	}
}

/*
 * The amplifiers on while there is sound, at the volume iOS has set (hakios_audio_volume(): how
 * far under the loudest, sound_below under Android's level), and off while iOS's volume is 0;
 * their status looked at every 200 ms while they are on. On the sound's thread.
 */
static void
watch(void)
{
	bool on = false, given_up = false, muted = false;
	uint64_t last_look = 0, last_monitor = 0;
	uint32_t starts = 0, changes = 0, generation = 0, extra = 0, monitor = 0, monitor_looks = 0;

	guard_level(below);                     /* (from the first sample on: see guard_off()) */

	while (!stopping) {
		uint64_t now = now_us(), sounded = last_sound_us;
		uint32_t extra_now, generation_now;
		bool muted_now;

		if (hakios_audio_volume(&extra_now, &muted_now, &generation_now) && generation_now != generation) {
			generation = generation_now;
			extra = extra_now;
			muted = muted_now;
			changes++;
			say("sound: iOS sets the volume: %s%u.%03u dB under the loudest, which is %u.%03u dB under Android's level%s\n",
			    muted ? "muted (0), " : "", extra / 8, extra % 8 * 125, below / 8, below % 8 * 125,
			    on ? (muted ? ": the amplifiers are stopped" : ": the amplifiers follow") : "");
			if (muted) {
				guard_off(true);
				if (on) {
					AMP_STOP();
					on = false;
				}
			} else if (on && !AMP_VOLUME(below + extra, 1)) {
				guard_off(true);
				on = false;
				given_up = true;
			} else if (!given_up) {
				guard_level(below + extra);
			}
		}
		if (!on && !given_up && !muted && sounded != 0 && now - sounded < 100000) {
			say_when("iOS plays: the amplifiers are started");
			starts++;
			ios_peak = 0;
			if (!AMP_START() || !AMP_VOLUME(below + extra, 1)) {
				say("sound: THE AMPLIFIERS DID NOT START; iOS's sound goes on to the DSP, unheard\n");
				guard_off(true);
				given_up = true;
			} else {
				on = true;
				guard_on = true;
				last_look = now_us();
				last_monitor = 0;
				monitor_looks = 0;
				/* (what is written now is heard after the periods already queued at the DSP: PERIODS x 20 ms) */
				say("sound: the amplifiers are up %llu ms after the first write that was not silence; that write reaches "
				    "them %u ms after it was made, behind the periods queued at the DSP\n", (last_look - sounded) / 1000,
				    PERIODS * PERIOD_FRAMES / 48);
			}
		} else if (on && now - sounded > QUIET_BEFORE_OFF_US) {
			say_when("three seconds of silence: the amplifiers are stopped");
			say("sound: what iOS wrote while they were on peaked at %u%% of full scale (%u)\n", ios_peak * 100 / 32768,
			    ios_peak);
			guard_off(false);
			AMP_STOP();
			on = false;
		} else if (on && now - last_look >= 200000) {
			last_look = now;
			if (!AMP_HEALTHY()) {
				guard_off(true);
				on = false;
				given_up = true;
			} else if (now - last_monitor >= 1000000) {
				/* the amplifiers' temperature and the battery as they see it, every second: a reduction if Android's table asks for one */
				uint32_t wanted;

				last_monitor = now;
				wanted = mi2s ? 0 : hakios_speaker_monitor(monitor_looks++ % 10 == 0);   /* (the TFA9874 has no watch of its own) */
				if (wanted != monitor) {
					say("sound: the amplifiers' watch asks for %u.%03u dB less (it was %u.%03u)\n", wanted / 8, wanted % 8 * 125,
					    monitor / 8, monitor % 8 * 125);
					monitor = wanted;
					hakios_guard_monitor(monitor);
				}
			}
		}
		sleep_ms(5);
	}
	guard_off(true);
	say("sound: the watcher stops; the amplifiers were started %u times, iOS set the volume %u times%s\n", starts, changes,
	    given_up ? ", and the amplifiers were then given up on" : "");
	watcher_runs = false;
}

/* ---- the whole ---------------------------------------------------------------------------------------- */

static bool running, taken_down;

static void
take_down(void)
{
	stopping = true;
	if (level >= 4) {
		hakios_audio_set_sink("nothing", NULL);
	}
	for (unsigned ms = 0; ms < 3000 && (feeder_runs || watcher_runs); ms++) {
		sleep_ms(1);
	}
	lck_mtx_lock(&sound_lock);
	if (running && !taken_down) {
		taken_down = true;
		AMP_STOP();
		stream_off();
		afe_off();
		pins_off();
		lck_mtx_lock(&hakios_smmu_lock);
		shared_put(0);
		lck_mtx_unlock(&hakios_smmu_lock);
		hakios_adsp_apr_events(NULL);
		say("sound: everything of the sound's is stopped and put back\n");
	}
	lck_mtx_unlock(&sound_lock);
}

static void
sound_thread(__unused void *parameter, __unused wait_result_t result)
{
	const uint8_t *acf = NULL;
	uint64_t acf_size = 0;
	uint32_t changes, ones, sync_changes, sync_ones;
	bool ok = false;

	lck_mtx_lock(&sound_lock);
	if (taken_down) {
		lck_mtx_unlock(&sound_lock);
		thread_terminate(current_thread());
	}
	if (level == 4) {
		say("sound: level 4: what iOS plays, through the port, a stream and the amplifiers, until the run ends\n");
	} else {
		say("sound: level %u: %s, for %u s\n", level, level == 1 ? "the speakers' TDM port" : level == 2 ?
		    "the port and a stream with a test tone, the amplifiers off" :
		    "the port, a stream with a test tone, and the amplifiers", seconds);
	}
	if (level >= 3) {
		acf = hakios_adsp_fw_file(mi2s ? "mono.cnt" : "aw882xx_acf.bin", &acf_size);
		if (acf == NULL) {
			say("sound: no /fw/%s: put the phone's (%s) into fw-audio/ and run SYSTEM=1 userland/build.sh\n",
			    mi2s ? "mono.cnt" : "aw882xx_acf.bin", mi2s ? "its vendor image's firmware/mono.cnt" :
			    "/vendor/etc/aw882xx_acf.bin");
			goto out;
		}
	}
	if (level >= 2) {
		lck_mtx_lock(&hakios_smmu_lock);
		ok = shared_get(0);
		lck_mtx_unlock(&hakios_smmu_lock);
		if (!ok) {
			goto out;
		}
		ok = false;
		hakios_adsp_apr_events(events);
	}
	announce(mi2s ? "the MI2S pins (GPIO 49 to 52) to the interface, as Android's active state has them" :
	    "the TDM pins (GPIO 97 to 100) to the interface, as Android's active state has them");
	pins_on();
	if (!afe_on()) {
		goto out;
	}
	sleep_ms(20);
	pin_activity(PIN(0), PIN_NAME(0), &changes, &ones);
	pin_activity(PIN(1), PIN_NAME(1), &sync_changes, &sync_ones);
	say("sound: %s\n", changes > 100 && sync_changes > 4 ? "THE BIT CLOCK AND THE FRAME SYNC RUN at the pins" :
	    "THE PINS DO NOT SHOW A RUNNING BIT CLOCK");
	if (level == 1) {
		sleep_ms(seconds * 1000);
		ok = true;
		goto out;
	}
	if (!stream_on()) {
		goto out;
	}
	if (level == 4) {
		thread_t feeder;

		announce("the amplifiers, set up from Android's Music profile, powered down until iOS plays");
		if (!AMP_PREPARE(acf, acf_size)) {
			goto out;
		}
		hakios_adsp_fw_free(acf, acf_size);
		acf = NULL;
		feeder_runs = true;
		if (kernel_thread_start(feeder_thread, NULL, &feeder) != KERN_SUCCESS) {
			feeder_runs = false;
			say("sound: no thread for the feeder\n");
			goto out;
		}
		thread_deallocate(feeder);
		watcher_runs = true;
		hakios_audio_set_sink("the speakers (the audio DSP, hakios.sound.c)", ios_sink);
		say("sound: WHAT IOS PLAYS GOES TO THE SPEAKERS from here on, %u.%03u dB under Android's level, until the run ends\n",
		    below / 8, below % 8 * 125);
		say("sound: through the guard (hakios.guard.c): %u.%03u dB of gain first (sound_gain), then high-pass at 200 Hz; peaks "
		    "3 dB, the 0.2 s/3 s/60 s power 6/9/12 dB under full scale at Android's level\n", gain / 8, gain % 8 * 125);
		if (guard_as < below) {
			say("sound: sound_guard_as=%u: FOR A TEST the guard reckons the amplifiers %u.%03u dB under Android's level, louder "
			    "than they are, and holds back as much as it would there\n", guard_as, guard_as / 8, guard_as % 8 * 125);
		}
		lck_mtx_unlock(&sound_lock);
		watch();
		thread_terminate(current_thread());
	}
	if (level >= 3) {
		/* the clock runs (the port is started, the stream as well): the amplifiers can lock to it */
		announce("the amplifiers, set up from Android's Music profile, then started at the lowest volume");
		if (!AMP_PREPARE(acf, acf_size) || !AMP_START()) {
			goto out;
		}
	}
	/* (the amplifiers are faded in before the tone begins: they fade in on silence, and the tone starts at that level) */
	if (level >= 3 && !AMP_VOLUME(below, 50)) {
		goto out;
	}
	if (level >= 3) {
		say("sound: THE TONE PLAYS NOW for %u s: 1 kHz at -20 dBFS, both speakers, %u.%03u dB under Android's level\n",
		    seconds, below / 8, below % 8 * 125);
	}
	say_when("the tone begins");
	ok = play(seconds * 1000, level >= 3);
	say_when("the tone has ended");
	space_faults();
out:
	if (acf != NULL) {
		hakios_adsp_fw_free(acf, acf_size);
	}
	lck_mtx_unlock(&sound_lock);
	take_down();
	say("sound: %s; last: %s\n", ok ? "LEVEL DONE" : "STOPPED SHORT", last_write);
	thread_terminate(current_thread());
}

static void
davinci_choose(void)
{
	if (!mi2s) {
		mi2s = true;
		port_id = 0x1000;
		audio_stream = 0x1b21;          /* (its low four bits are 1 as well: the address bits stay) */
		tlmm.phys = 0x03500000;         /* the west tile, GPIO 49 to 52 */
		tlmm.size = 0x40000;
	}
}

void
hakios_sound_dsp_up(void)
{
	thread_t thread;

	davinci_choose();
	if (!PE_parse_boot_argn("sound", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 4) {
		level = 4;
	}
	PE_parse_boot_argn("sound_secs", &seconds, sizeof(seconds));
	PE_parse_boot_argn("sound_below", &below, sizeof(below));
	PE_parse_boot_argn("sound_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("sound_guard_as", &guard_as, sizeof(guard_as));
	PE_parse_boot_argn("sound_gain", &gain, sizeof(gain));
	if (gain > HAKIOS_GUARD_MAX_GAIN) {
		gain = HAKIOS_GUARD_MAX_GAIN;
	}
	hakios_guard_configure(true, gain);
	if (level >= 2 && level <= 3 && seconds > 3) {
		seconds = 3;            /* the test tone: three seconds at most */
	}
	running = true;
	if (kernel_thread_start(sound_thread, NULL, &thread) != KERN_SUCCESS) {
		running = false;
		say("sound: no thread\n");
		return;
	}
	thread_deallocate(thread);
}

void
hakios_sound_dsp_down(void)
{
	if (running) {
		take_down();
	}
}

void
hakios_sound_quiesce(void)
{
	AMP_QUIESCE();
}
