/*
 * The SM7150's audio DSP (and, compiled a second time as hakios.adsp_sm7150_modem.c, its modem) for the Mi 9T (devices/
 * davinci), in place of drivers/audio/hakios.adsp.c: the common driver with what the SM7150 needs woven through it (its
 * memory map, the APCS doorbell instead of the IPCC, FastRPC's root PD and the sensors' process, the modem as a second
 * instance for the Wi-Fi's firmware). Its addresses default to the SM7150's and take soc_<name>= as the common drivers do.
 * What follows is the common driver's own description, with the SM7150's differences in it.
 */
/*
 * The audio DSP of the POCO X5 Pro (redwood), started, and spoken to: step 1 of sound. Every
 * address below belongs to this phone, and nothing here is touched unless a boot-arg asks for it.
 *
 * The SoC's audio (LPASS: its clocks, its I2S and SoundWire, the codec's macros) does not belong
 * to the application processor. It is the audio DSP's -- a Hexagon of its own inside LPASS,
 * Android's "adsp" -- which stays in reset until somebody loads it, exactly as the Wi-Fi's does
 * (hakios.wifi.c): the phone's own signed image, adsp.mdt and adsp.b02 to adsp.b36 from
 * /vendor/firmware_mnt/image of its Android (20 MB: in the system image under /fw, or with ADSP=1 in
 * the small one, see image_file()), goes into the 64 MB reserved for it, TrustZone is
 * asked to authenticate and start it, and from there everything is conversation through the
 * shared memory: SMP2P for "up", "given up", "stopped"; GLINK for channels; QRTR on the channel
 * "IPCRTR" for the services the DSP offers. (Reading an LPASS clock register from here ends the
 * kernel with an external abort: the DSP owns that block, and talks about it only this way.)
 * What audio needs of it later -- the audio services, the codec, the speaker's path -- is for the
 * steps after this one; this file only brings the DSP up and the link with it.
 *
 * The way is the Wi-Fi DSP's, taken over from hakios.wifi.c with the audio DSP's numbers: its
 * comments say why each thing is done so. What is different is said here:
 *   - processor 2 (not 13) in the shared memory; its SMP2P items 443 (its) and 429 (ours), its
 *     last words in item 423; GLINK's three items in our partition with it
 *   - its doorbells are IPCC client 3 (LPASS): signal 0 GLINK, signal 2 SMP2P
 *   - TrustZone knows it as image 1; its memory is 0x86700000, 64 MB (dt: qcom,lpass@3700000,
 *     reserved-memory adsp@86700000), every byte of which the image takes
 *   - the always-on processor is told of it as "adsp"
 *   - the Wi-Fi DSP may be running at the same time (wifi=1): the mailbox to the always-on
 *     processor and the lock for allocating in the shared memory are the two things both use,
 *     each behind a lock both take (hakios.wifi.c); the doorbells are only counted here, and
 *     only the audio DSP's are cleared from here
 * The code is a copy, not a shared library, for now: step 1 is not to change what the Wi-Fi does.
 *
 * Boot-arg adsp_step=N performs steps 1 to N and stops; as with wifi_step, a step says what it is
 * about to touch before it touches it, and a step that does not find what it needs ends the
 * sequence. adsp=1 is all of them, on a thread beside the system's start.
 *
 *   1  reads   the shared memory: our partition with the DSP, its items, whether it has run
 *              since the phone was started (its SMP2P item is there if so), its last words
 *   2  reads   the firmware in the small image: is it whole, does it fit the reserved memory
 *   3  writes  the always-on processor's mailbox open, unless it is (the Wi-Fi may have opened it)
 *   4  asks    TrustZone to take the image's header (image 1) and where the image will be; a
 *              DSP that a kernel before this one left running is asked to stop and shut down first
 *   5  writes  the image into the DSP's memory, and reads it back
 *   6  writes  our SMP2P item (429) into the shared memory, and our two marks of GLINK to zero
 *   7  asks    RPMh for the DSP's rails, writes "adsp: on" to the always-on processor, asks
 *              TrustZone to authenticate and start the DSP, and watches for its first words
 *              (answering in our item what SMP2P has us answer)
 *   8  reads   GLINK's queues
 *   9  writes  our side of GLINK: the versions, the channel "IPCRTR" opened both ways, our intents;
 *              from here a thread of ours listens to the DSP
 *  10  talks   QRTR on that channel: hello both ways, and the services the DSP tells of, written
 *              down for adsp_listen seconds (default 3)
 *  11  talks   APR, on the channel apr_audio_svc that the DSP opens and we answer as well: whether
 *              the audio services are up, and which there are (see step_apr())
 *
 * Once everything is up with adsp=1, the sound's path takes over (hakios.sound.c, boot-arg sound=N):
 * hakios_adsp_apr() is how it speaks to the audio services (any service, port and token; what
 * nobody waits for goes to its handler of events), and it is stopped before the DSP is.
 *
 * What is started is stopped: when the steps are over (unless adsp_keep=1, and then when the run
 * ends), and on a panic's way back to the loader. The DSP is asked first (bit 0 of our entry
 * "master-kernel"), then TrustZone shuts it down, then the always-on processor hears "adsp: off".
 * A DSP that a kernel before this one left running is found by its item and treated the same way
 * before this one is started; one that was shut down without having said it stopped is not
 * started again unless adsp_again=1 (the Wi-Fi DSP reset the phone, twice, when that was tried).
 *
 * More boot-args: adsp_new=K (steps K on have not been seen to work, and wait adsp_pause=MS after
 * each announcement, default 2200: what the loader needs to note the end of the log), adsp_wait=S
 * (how long step 7 watches for "up", default 10), adsp_keep=1, adsp_again=1, adsp_return=1 (back
 * to the loader when the steps are over).
 *
 * Nothing here writes a clock or changes a pin. One request goes to RPMh, as Linux makes it: the
 * DSP's two rails, LCX and LMX, at level 384 while it boots, given back when it says it has asked
 * for its own (see rails()).
 *
 * Addresses are from the phone's own device tree (qcom,lpass@3700000, qcom,smp2p-adsp,
 * qcom,glink/adsp, qcom,ipcc@408000, power-controller@c300000, reserved-memory), layouts from
 * Linux (see hakios.wifi.c for the files).
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <kern/startup.h>
#include <sys/hakios.qmi.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/vm_kern.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);

/* What the Wi-Fi's DSP and this one both use (hakios.wifi.c). */
extern lck_mtx_t hakios_aop_mailbox_lock;
extern lck_mtx_t hakios_smem_alloc_lock;
extern lck_mtx_t hakios_rsc_lock;            /* RPMh's sequencer, shared with the other DSP's driver */

static LCK_GRP_DECLARE(adsp_lock_group, "hakios-adsp");

#define STEPS                   11
#define HAKIOS_SOC_BASE        0x00400000ul    /* where the loader says the phone's peripherals begin */

static uint32_t last_step, pause_ms = 2200, listen_seconds = 3;
static uint32_t step;                   /* the one being performed */
static uint32_t new_from;               /* the first of the steps that have not been seen to work: they wait */
static char last_write[96] = "nothing written";
static bool leaving;                    /* on the way back to the loader after a panic: no lock is taken, nothing sleeps */

/*
 * Every line goes to the log the loader reads from another core as well (as in hakios.wifi.c);
 * with adsp=1 only the few that say how it went (tell()), the rest to the kernel's own log.
 */
extern void loader_log_putc(char c);
extern void hakios_return_register(void (*quiesce)(void), void (*stop)(void));   /* osfmk/arm64/loader_return.c */
/* FastRPC's memory, behind the microphones' in what the SoC's sound driver shares with the DSP (hakios.sound_sm7150.c) */
bool hakios_audio_dsp_memory_fastrpc(uintptr_t *memory, uint64_t *dsp_address, uint32_t *bytes);
void hakios_audio_dsp_memory_fastrpc_put(void);

/* An address or number of the SoC: soc_<name>= on the command line, else this SoC's own (the default given). */
static vm_offset_t
soc_addr(const char *name, vm_offset_t dflt)
{
	char arg[40] = "soc_";
	uint64_t value = 0;

	strlcat(arg, name, sizeof(arg));
	return PE_parse_boot_argn(arg, &value, sizeof(value)) && value != 0 ? (vm_offset_t)value : dflt;
}

static uint32_t
soc_number(const char *name, uint32_t dflt)
{
	char arg[40] = "soc_";
	uint32_t value = 0;

	strlcat(arg, name, sizeof(arg));
	return PE_parse_boot_argn(arg, &value, sizeof(value)) ? value : dflt;
}

static LCK_MTX_DECLARE(say_lock, &adsp_lock_group);
static bool quiet;

static void
say_line(const char *line, bool aloud)
{
#ifdef HAKIOS_SM7150_MODEM_COPY
	char marked[440];

	/* the modem's copy says which processor a line is about: "adsp-modem: ..." */
	if (strncmp(line, "adsp:", 5) == 0) {
		snprintf(marked, sizeof(marked), "adsp-modem:%s", line + 5);
		line = marked;
	}
#endif
	if (leaving) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		return;
	}
	lck_mtx_lock(&say_lock);
	if (!quiet || aloud) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
	}
	printf("%s", line);
	lck_mtx_unlock(&say_lock);
}

static int say(const char *format, ...) __printflike(1, 2);
static int tell(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, false);
	return length;
}

static int
tell(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, true);
	return length;
}
#define printf say

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
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

/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

static bool
option(const char *name)
{
	uint32_t value = 0;

	return PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
}

/* ---- register access (as in hakios.wifi.c) ------------------------------------------------------- */

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

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/* Time for the loader to note what has just been said, where it is not yet known to be harmless. */
static void
hold(void)
{
	if (step >= new_from && pause_ms && !leaving) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("adsp: step %u: %s\n", step, what);
	hold();
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	value = rd(b->base + offset);
	printf("adsp: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("adsp: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}

/* ---- the shared memory (hakios.wifi.c, step 1) --------------------------------------------------- */

#define SMEM_PHYS               ((vm_offset_t)soc_addr("smem", 0x86000000UL))
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
#define SMEM_ADSP               (modem ? 1 : 2) /* dt: qcom,remote-pid of qcom,smp2p-adsp (-modem) and qcom,glink/adsp (/modem) */
/*
 * The modem (MPSS) is driven by a second copy of this file, hakios.adsp_sm7150_modem.c (HAKIOS_SM7150_MODEM_COPY: its state is
 * all this file's, so the two run side by side), with boot-arg adsp_modem=1 -- the Mi 9T's Wi-Fi firmware
 * runs on the modem. Until 25 October adsp_modem=1 turned this one copy into the modem's, and then there was
 * no audio DSP and no sound with the Wi-Fi on. The modem's numbers: the stock dt's qcom,mss@4080000
 * (pas-id 4, firmware "modem", modem_region@8b000000 of 0x8400000), qcom,smp2p-modem (items 435 in,
 * 428 out, APCS IPC bit 14), qcom,glink/modem (bit 12), its crash reason item 421, its rails cx and mss.
 * Nothing of the audio's (APR, the sound, FastRPC, the battery manager) is started then.
 */
static bool modem;
static void hakios_adsp_give_back(void);
/*
 * What starts once the modem's IPC router is up: the Wi-Fi firmware's exchange (soc/drivers/wifi/hakios.wlfw10.c), which
 * registers itself here when it is built (hakios_adsp_modem_on_link); nothing otherwise.
 */
static void (*modem_link_hook)(void);
#ifdef HAKIOS_SM7150_MODEM_COPY
void hakios_adsp_modem_on_link(void (*hook)(void));

void
hakios_adsp_modem_on_link(void (*hook)(void))
{
	modem_link_hook = hook;
}
#endif
#define SMEM_GLOBAL             0xfffe
#define SMEM_CANARY             0xa5a5

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
struct smem_item {
	uint32_t item, offset, size;
};

/*
 * The DSP's items (dt: qcom,smem of qcom,smp2p-adsp, <443 429>: the item the DSP writes first, then
 * ours -- the Wi-Fi's <617 616> are in the same order; taken the other way round, the DSP made
 * our item its own and never said "up" --, and qcom,smem-id of qcom,lpass@3700000).
 */
#define ITEM_SMP2P_OURS         (modem ? 428 : 429)
#define ITEM_SMP2P_THEIRS       (modem ? 435 : 443)
#define ITEM_CRASH_REASON       (modem ? 421 : 423)

static const volatile uint8_t *smem;

static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

static void
smem_poke(uint32_t offset, uint32_t value)
{
	wr((vm_offset_t)smem + offset, value);
}

static uint32_t
smem_word(uint32_t offset)
{
	return rd((vm_offset_t)smem + offset);
}

static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

/* An item the DSP and we share: in the partition of the two, or else in the global one. Where it is; 0: nowhere. */
static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[256];
	const uint16_t hosts[2][2] = { { SMEM_US, SMEM_ADSP }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

static void
show_partition(void)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("adsp: %u: there is no partition for us and the audio DSP (0 and 2)\n", step);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("adsp: %u: the partition for us and the audio DSP (0 and 2): at %#lx, %#x bytes, of which %#x free; %u items\n", step,
	    SMEM_PHYS + partition, size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("adsp: %u:   items (bytes):%s\n", step, line);
	}
}

/* ---- SMP2P ---------------------------------------------------------------------------------------- */

struct smp2p {
	uint32_t magic;
	uint32_t version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u
#define SMP2P_RESTART_DONE      (1u << 0)
#define SMP2P_RESTART_ACK       (1u << 1)
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))

/* The bits of the DSP's entry "slave-kernel" (dt: the interrupts of qcom,lpass@3700000). */
#define DSP_FATAL               (1u << 0)
#define DSP_READY               (1u << 1)
#define DSP_HANDOVER            (1u << 2)
#define DSP_STOP_ACK            (1u << 3)
#define DSP_SHUTDOWN_ACK        (1u << 7)

/* Our entry (dt: the child "master-kernel" of qcom,smp2p-adsp; "qcom,force-stop" of qcom,lpass@3700000). */
#define OUR_ENTRY_STOP          "master-kernel"
#define DSP_STOP                (1u << 0)

static bool
dsp_words(struct smp2p *words)
{
	static uint32_t at;
	uint32_t size = 0;

	if (at == 0) {
		at = smem_item(ITEM_SMP2P_THEIRS, &size);
		if (at == 0 || size < sizeof(*words)) {
			at = 0;
			return false;
		}
	}
	return smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}

static int
smp2p_entry(const struct smp2p *words, const char *name)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, name, sizeof(words->entry[i].name)) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static uint32_t
dsp_state(const struct smp2p *words)
{
	int entry = smp2p_entry(words, "slave-kernel");

	return entry < 0 ? 0 : words->entry[entry].value;
}

static void
show_words(const char *prefix, const struct smp2p *words)
{
	char line[320];
	int at = 0;

	line[0] = 0;
	for (unsigned i = 0; i < words->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
	}
	printf("adsp: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

static void
show_crash_reason(void)
{
	char text[81];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf("adsp: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
		return;
	}
	bzero(text, sizeof(text));
	smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	if (text[0] == 0) {
		printf("adsp: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("adsp: %u: THE AUDIO DSP'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}

/* ---- the doorbells, and the mailbox to the always-on processor (hakios.wifi.c, steps 2 and 7) ------ */

#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c
#define QMP_VERSION             0x04
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18
#define QMP_OUR_LINK            0x24
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64

static struct block aoss = { "mailbox", 0x0c300000, 0x400 };

#define IPCC_SEND               0x0c
#define IPCC_RECEIVED           0x10
#define IPCC_ENABLE             0x14
#define IPCC_DISABLE            0x18
#define IPCC_CLEAR              0x1c
#define IPCC_NOBODY             0xffffffffu
#define IPCC_AOP                0u
#define IPCC_LPASS              3u              /* dt: mboxes of qcom,smp2p-adsp and qcom,glink/adsp */
#define IPCC_GLINK              0u
#define IPCC_SMP2P              2u

static struct block ipcc = { "ipcc", 0x00408000, 0x1000 };

/*
 * Doorbells. Redwood's SoC has IPCC (a client and a signal per ring, and a register that says who
 * rang); the Mi 9T's SM7150 has not: there each ring is a bit of the APCS IPC register (dt:
 * syscon@17c0000c; the AOP's qcom,qmp-aop@c300000 qcom,irq-mask 0x1, the audio DSP's GLINK
 * mboxes <&apcs_glb 24>, its SMP2P qcom,ipc <&apcs 0 26>), and what the DSP rings comes as
 * interrupts this driver does not take (it only counted them anyway). Boot-arg
 * soc_apcs_ipc= (default the SM7150's, 0x17c0000c) chooses the APCS kind; soc_aop_bit,
 * soc_adsp_glink_bit and soc_adsp_smp2p_bit its bits (default the SM7150's: 0, 24, 26).
 */
static struct block apcs = { "apcs ipc", 0, 0x1000 };
static vm_offset_t apcs_ipc;            /* 0: IPCC */

static bool
ring_need(void)
{
	if (apcs.phys == 0 && soc_addr("apcs_ipc", 0x17c0000cUL) != 0) {
		apcs_ipc = soc_addr("apcs_ipc", 0x17c0000cUL);
		apcs.phys = (uint32_t)(apcs_ipc & ~0xfffUL);
	}
	if (apcs_ipc != 0) {
		need(&apcs);
		return apcs.base != 0;
	}
	need(&ipcc);
	return ipcc.base != 0;
}

static void
ring_send(uint32_t who)
{
	if (!ring_need()) {
		return;
	}
	if (apcs_ipc != 0) {
		uint32_t bit = who == (IPCC_AOP << 16 | 0) ? soc_number("aop_bit", 0) :
		    who == (IPCC_LPASS << 16 | IPCC_GLINK) ? soc_number("adsp_glink_bit", modem ? 12 : 24) :
		    soc_number("adsp_smp2p_bit", modem ? 14 : 26);

		wr(apcs.base + (apcs_ipc & 0xfffUL), 1u << bit);
		return;
	}
	wr(ipcc.base + IPCC_SEND, who);
}

static uint32_t
ring_received(void)
{
	return ring_need() && apcs_ipc == 0 ? rd(ipcc.base + IPCC_RECEIVED) : IPCC_NOBODY;
}

static void
ring_write(uint32_t offset, uint32_t who)
{
	if (ring_need() && apcs_ipc == 0) {
		wr(ipcc.base + offset, who);
	}
}

static struct {
	uint32_t glink, smp2p, said;
	uint64_t since;
} doorbell;

/*
 * The audio DSP's rings, counted and cleared. Another's ring (the Wi-Fi DSP's, the always-on
 * processor's) is left for whoever listens for it: IPCC names the same one until it is cleared,
 * so the look ends there.
 */
static void
doorbells(void)
{
	for (unsigned i = 0; i < 8; i++) {
		uint32_t who = ring_received();

		if (who == IPCC_NOBODY || who >> 16 != IPCC_LPASS) {
			return;
		}
		ring_write(IPCC_CLEAR, who);
		doorbell.glink += (who & 0xffff) == IPCC_GLINK;
		doorbell.smp2p += (who & 0xffff) == IPCC_SMP2P;
		if (doorbell.said < 24) {
			uint64_t now = now_us() - doorbell.since;

			doorbell.said++;
			printf("adsp: doorbell: +%llu.%03llu ms: signal %u of the audio DSP; heard and cleared\n", now / 1000, now % 1000,
			    who & 0xffff);
		}
	}
}

static uint32_t our_box;                /* where in the message memory our mailbox is; 0: not open */

static bool
mailbox_wait(uint32_t offset, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if (rd(aoss.base + offset) == value) {
			return true;
		}
		sleep_us(100);
	}
	return rd(aoss.base + offset) == value;
}

static void
ring_aop(void)
{
	ring_send(IPCC_AOP << 16 | 0);
}

/* Linux's qmp_open(), as hakios.wifi.c does it; what is done already is found done. Under the mailbox's lock. */
static bool
mailbox_open(void)
{
	uint32_t box, size;

	need(&aoss);
	ring_need();
	if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
		printf("adsp: %u: not a mailbox this knows\n", step);
		return false;
	}
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	if (size < QMP_MESSAGE + 4 || box < 0x3c || box + size > aoss.size || (box & 3)) {
		printf("adsp: %u: our mailbox is not where a message fits\n", step);
		return false;
	}
	if (rd(aoss.base + QMP_OUR_LINK) == QMP_UP && rd(aoss.base + QMP_OUR_LINK_ACK) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL) == QMP_UP && rd(aoss.base + QMP_THEIR_CHANNEL) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		printf("adsp: %u: the mailbox is open already (the Wi-Fi's start, or a kernel before this one, opened it)\n", step);
		our_box = box;
		return true;
	}
	put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, rd(aoss.base + QMP_THEIR_LINK));
	put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second; taken down again\n", step);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second; ours is taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second; taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	printf("adsp: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN\n", step);
	our_box = box;
	return true;
}

/* A message to the always-on processor (Linux's qmp_send()), under the lock the Wi-Fi's start takes too. */
static bool
mailbox_send(const char *text)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };
	bool sent = false;

	if (!leaving) {
		lck_mtx_lock(&hakios_aop_mailbox_lock);
	}
	if (our_box == 0 && (leaving || !mailbox_open())) {
		goto out;
	}
	strlcpy((char *)words, text, sizeof(words));
	printf("adsp: %u: about to tell the always-on processor \"%s\"\n", step, text);
	snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
	if (rd(aoss.base + our_box) != 0) {
		printf("adsp: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
		goto out;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	wr(aoss.base + our_box, QMP_MESSAGE);
	(void)rd(aoss.base + our_box);
	ring_aop();
	if (!mailbox_wait(our_box, 0, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
		wr(aoss.base + our_box, 0);
		goto out;
	}
	printf("adsp: %u: the always-on processor has taken it\n", step);
	sent = true;
out:
	if (!leaving) {
		lck_mtx_unlock(&hakios_aop_mailbox_lock);
	}
	return sent;
}

/* ---- RPMh: the DSP's two rails, asked for while it boots ------------------------------------------------ */

/*
 * The audio DSP's logic and memories run from LPASS's own rails, LCX and LMX (dt: vdd_cx and
 * vdd_mx of qcom,lpass@3700000 are PM7325 L5 and L4 "level" regulators, RPMh resources lcx.lvl and
 * lmx.lvl), which the boot loader leaves at retention (16). Linux and Android ask for level 384
 * while the DSP boots, until it says it has asked for its own (bit 2, "proxy-unvote"); without it
 * the DSP rang once and left "SFR Init: wdog or kernel error suspected." in its last words.
 *
 * A request goes to RPMh through the application processor's sequencer (dt: rsc@18200000, DRV 2,
 * qcom,tcs-offset 0xd00, qcom,tcs-config: TCSs 0 and 1 are for requests made at once), as Linux's
 * rpmh-rsc.c writes one: the command's message id, address and value into a TCS that is idle, the
 * command enabled and marked to be waited for, the TCS put in "active" mode and triggered, and its
 * bit in the status of completions looked for. A rail asked for by level is asked by position in
 * the list of its levels that the boot loader's command database keeps (cmd-db.c), the first
 * position at or above the level wanted.
 */
#define CMD_DB_PHYS             ((vm_offset_t)soc_addr("cmd_db", 0x85f20000UL))
#define CMD_DB_SIZE             0x20000UL

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

static const struct cmd_db_header *cmd_db;

static struct block rsc = { "rsc", 0x18220000, 0x10000 };
#define RSC_TCS_BASE            0xd00
#define RSC_IRQ_STATUS          (RSC_TCS_BASE + 0x04)   /* a bit per TCS: done */
#define RSC_IRQ_CLEAR           (RSC_TCS_BASE + 0x08)
#define RSC_TCS(n)              (RSC_TCS_BASE + 672 * (n))
#define TCS_WAIT_FOR_DONE       0x10
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18                    /* not zero: idle */
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))       /* message id, address, value, status */
#define TCS_ACTIVE_MODE         (1u << 16)
#define TCS_TRIGGER             (1u << 24)
#define MSGID_WRITE_WAITED      (8u | 1u << 8 | 1u << 16)       /* four bytes, an answer wanted, a write */
#define RPMH_TCS                0                       /* the first of the two for requests made at once */

/* The level position RPMh takes for that rail and level; -1: not found. */
static int
rail_position(const char *name, uint32_t level, uint32_t *address)
{
	char id[8] = { 0 };

	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (!(cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c)) {
		printf("adsp: %u: the boot loader's command database is not there\n", step);
		return -1;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data;
			char levels[160];
			int at;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				return -1;
			}
			data = cmd_db->data + slave->data_offset + entry[i].offset;
			at = snprintf(levels, sizeof(levels), "%s at %#x, levels:", name, entry[i].address);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				unsigned value = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && value == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", value);
			}
			printf("adsp: %u: RPMh has %s\n", step, levels);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				if ((uint32_t)(data[2 * l] | data[2 * l + 1] << 8) >= level) {
					*address = entry[i].address;
					return (int)l;
				}
			}
			return -1;
		}
	}
	printf("adsp: %u: RPMh has no \"%s\"\n", step, name);
	return -1;
}

/* The two rails at that level (Linux's proxy request), or given back (level 0). True: RPMh has said done. */
static bool
rails(uint32_t level)
{
	const char *const names[2] = { modem ? "cx.lvl" : "lcx.lvl", modem ? "mss.lvl" : "lmx.lvl" };

	if (modem && level == 0) {
		/* (the application processor's vote for CX is what keeps the SoC's logic up: never given back) */
		printf("adsp: %u: the modem's rails are not given back (cx.lvl is the whole SoC's)\n", step);
		return true;
	}
	uint32_t addresses[2] = { 0, 0 }, values[2] = { 0, 0 }, done = 0;
	uint64_t began;

	for (unsigned r = 0; r < 2; r++) {
		int position = level == 0 ? 0 : rail_position(names[r], level, &addresses[r]);

		if (level == 0) {
			rail_position(names[r], 0, &addresses[r]);
		}
		if (position < 0 || addresses[r] == 0) {
			printf("adsp: %u: no position for %s at level %u: the rails are not asked\n", step, names[r], level);
			return false;
		}
		values[r] = (uint32_t)position;
	}
	need(&rsc);
	lck_mtx_lock(&hakios_rsc_lock);
	/* (the boot loader leaves the trigger set after its last request, done: idle is what the status says) */
	if (rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS) == 0) {
		printf("adsp: %u: TCS %u of the sequencer is busy (status %#x, control %#x): the rails are not asked\n", step, RPMH_TCS,
		    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL));
		lck_mtx_unlock(&hakios_rsc_lock);
		return false;
	}
	printf("adsp: %u: about to ask RPMh, through TCS %u, for lcx.lvl (%#x) = %u and lmx.lvl (%#x) = %u (level %u%s)\n", step,
	    RPMH_TCS, addresses[0], values[0], addresses[1], values[1], level, level == 0 ? ": given back" : "");
	snprintf(last_write, sizeof(last_write), "step %u asked RPMh for the audio DSP's rails at level %u", step, level);
	hold();
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	for (unsigned r = 0; r < 2; r++) {
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r), MSGID_WRITE_WAITED);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 4, addresses[r]);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 8, values[r]);
	}
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_WAIT_FOR_DONE, 3);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 3);
	settle();
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~TCS_TRIGGER);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_ACTIVE_MODE);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_TRIGGER);
	began = now_us();
	while (now_us() - began < 100000) {
		done = rd(rsc.base + RSC_IRQ_STATUS) & (1u << RPMH_TCS);
		if (done) {
			break;
		}
		sleep_us(50);
	}
	printf("adsp: %u: RPMh %s after %llu us; the commands' status %#x %#x, the TCS's %#x\n", step, done ? "HAS DONE IT" :
	    "HAS NOT SAID DONE", now_us() - began, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0) + 12),
	    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(1) + 12), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS));
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 0);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done != 0;
}

/* ---- step 1: the shared memory --------------------------------------------------------------------- */

static bool
step_survey(void)
{
	struct smp2p words;
	uint32_t size = 0;

	announce("read our partition with the audio DSP in the shared memory (the board's smem), and its items");
	show_partition();
	if (dsp_words(&words)) {
		show_words("the DSP's item (443) is there: it has run since the phone was started", &words);
	} else {
		printf("adsp: %u: the DSP's SMP2P item (443) is not there: it has not run since the phone was started\n", step);
	}
	printf("adsp: %u: our SMP2P item (429): %s; GLINK's descriptor (478): %s, our queue (479): %s, the DSP's (480): %s\n", step,
	    smem_item(ITEM_SMP2P_OURS, &size) ? "there" : "not there", smem_item(478, &size) ? "there" : "NOT THERE",
	    smem_item(479, &size) ? "there" : "NOT THERE", smem_item(480, &size) ? "there" : "not there (the DSP makes it)");
	show_crash_reason();
	ring_need();
	printf("adsp: %u: the doorbells say %#x\n", step, ring_received());
	return smem_partition(SMEM_US, SMEM_ADSP, &size, &size) != 0;
}

/* ---- step 2: the firmware -------------------------------------------------------------------------- */

#define ADSP_MEMORY             (modem ? (uint32_t)soc_addr("modem_mem", 0x8b000000u) : \
	                        (uint32_t)soc_addr("adsp_mem", 0x93900000u))     /* dt: reserved-memory of qcom,lpass (the Mi 9T's: 0x93900000) */
#define ADSP_MEMORY_SIZE        (modem ? soc_number("modem_mem_size", 0x08400000u) : \
	                        soc_number("adsp_mem_size", 0x02600000u))
#define PAS_ADSP                (modem ? 4 : 1) /* dt: qcom,pas-id */


/*
 * A file of /fw on the root file system, read whole into memory of ours: the system image, where
 * userland/build.sh puts fw-audio/ when it makes that image. It is compressed there, which the file
 * system undoes and the reading of the small image's memory below does not; and it is there only
 * once the root is mounted, so only adsp=1, which waits for that on its own thread, finds it so.
 */
static const uint8_t *
root_file(const char *name, uint64_t *size)
{
	char path[256];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/fw/%s", name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > ADSP_MEMORY_SIZE) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		printf("adsp: %u: %s: read error %d, %d bytes left\n", step, path, error, left);
		kfree_data(data, attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

static bool from_root;                  /* the image is in memory of ours (root_file()), to be given back once written */

/* For the sound (hakios.sound.c): a file of /fw on the root, read whole into memory of ours, and given back. */
const uint8_t *
hakios_adsp_fw_file(const char *name, uint64_t *size)
{
	return root_file(name, size);
}

void
hakios_adsp_fw_free(const uint8_t *data, uint64_t size)
{
	uint8_t *ours = __DECONST(uint8_t *, data);

	kfree_data(ours, size);
}

/* A file of fw/ in the small image (ADSP=1 userland/build.sh puts fw-audio/ there), else of /fw on the root (above). */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
	{
		const uint8_t *data = hakios_image_file("fw", name, size, NULL);  /* (hakios.image.c) */

		if (data != NULL) {
			return data;
		}
	}
	{
		const uint8_t *data = root_file(name, size);

		if (data != NULL) {
			from_root = true;
			return data;
		}
	}
	printf("adsp: %u: no fw/%s in the small image nor on the root: put the phone's /vendor/firmware_mnt/image/%s into "
	    "fw-audio/ and run SYSTEM=1 userland/build.sh (or ADSP=1, for the small image)\n", step, name, name);
	return NULL;
}


#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)
#define MAX_SEGMENTS            48

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

static struct {
	const uint8_t *header;
	uint64_t header_size;
	struct {
		uint32_t at;
		uint32_t file_size, memory_size;
		const uint8_t *file;
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size;
} image;

/* The image's files given back, if they were read into memory of ours. */
static void
image_forget(void)
{
	if (!from_root) {
		return;
	}
	if (image.header != NULL) {
		void *data = (void *)(uintptr_t)image.header;

		kfree_data(data, image.header_size);
	}
	for (unsigned s = 0; s < image.segments; s++) {
		if (image.segment[s].file != NULL) {
			void *data = (void *)(uintptr_t)image.segment[s].file;

			kfree_data(data, image.segment[s].file_size);
		}
	}
	bzero(&image, sizeof(image));
	from_root = false;
}

static bool
step_firmware(void)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0;
	uint32_t count, hashes = 0, lowest = ~0u, highest = 0, base = ~0u;
	bool relocatable = false;

	announce("find the audio DSP's image in the small image or on the root, and check it against its own header");
	image_forget();
	image.segments = 0;
	header = image_file(modem ? "modem.mdt" : "adsp.mdt", &size);
	if (header == NULL) {
		return false;
	}
	image.header = header;
	image.header_size = size;
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS || 52 + 32 * count > size || size > PAGE_SIZE) {
		printf("adsp: %u: fw/adsp.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, header[0],
		    header[1], header[2], header[3], count, size);
		goto fail;
	}
	/*
	 * Where the image's addresses start: redwood's is linked at the DSP's memory; an image marked
	 * relocatable (bit 27 of a segment's flags, Linux mdt_loader.c's QCOM_MDT_RELOCATABLE) is put
	 * there with its segments where they are from its lowest one -- the Mi 9T's is linked at
	 * 0x93c00000, and its memory is wherever the board says (qcom_mdt_load: mem_reloc).
	 */
	for (uint32_t i = 0; i < count; i++) {
		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		relocatable |= (segment.flags & (1u << 27)) != 0;
		if (segment.type == 1 && !MDT_HASH(segment.flags) && segment.memory_size != 0 && segment.paddr < base) {
			base = segment.paddr;
		}
	}
	if (!relocatable || base == ~0u) {
		base = ADSP_MEMORY;
	} else if (base != ADSP_MEMORY) {
		printf("adsp: %u: the image is relocatable and linked at %#x: put at %#x\n", step, base, ADSP_MEMORY);
	}
	for (uint32_t i = 0; i < count; i++) {
		char name[16];

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		if (segment.paddr < base || segment.file_size > segment.memory_size ||
		    (uint64_t)segment.paddr + segment.memory_size > (uint64_t)base + ADSP_MEMORY_SIZE) {
			printf("adsp: %u: segment %u (%#x, %#x bytes) is not within the DSP's memory\n", step, i, segment.paddr,
			    segment.memory_size);
			goto fail;
		}
		image.segment[image.segments].at = segment.paddr - base;
		image.segment[image.segments].file_size = segment.file_size;
		image.segment[image.segments].memory_size = segment.memory_size;
		image.segment[image.segments].file = NULL;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), modem ? "modem.b%02u" : "adsp.b%02u", i);
			image.segment[image.segments].file = image_file(name, &file_size);
			if (image.segment[image.segments].file == NULL) {
				goto fail;
			}
			if (file_size != segment.file_size) {
				printf("adsp: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
				image.segments++;               /* (for its file to be given back) */
				goto fail;
			}
			files += file_size;
		}
		lowest = segment.paddr < lowest ? segment.paddr : lowest;
		highest = segment.paddr + segment.memory_size > highest ? segment.paddr + segment.memory_size : highest;
		image.segments++;
	}
	if (image.segments == 0 || lowest != base || 52 + 32 * count + (uint64_t)hashes != size) {
		printf("adsp: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
		    "the header %u and the hashes %u\n", step, image.segments, lowest, size, 52 + 32 * count, hashes);
		goto fail;
	}
	image.header = header;
	image.header_size = size;
	image.size = ((highest + 0xfff) & ~0xfffu) - base;
	printf("adsp: %u: the image is whole (%s): a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, %#x "
	    "bytes of the DSP's %#x from %#x\n", step, from_root ? "from /fw on the root" : "from the small image", size, hashes,
	    image.segments, files, image.size, ADSP_MEMORY_SIZE, ADSP_MEMORY);
	return true;
fail:
	image_forget();
	image.segments = 0;
	return false;
}

/* ---- step 3: the mailbox --------------------------------------------------------------------------- */

static bool
step_mailbox(void)
{
	bool open;

	announce("open the mailbox to the always-on processor, unless it is open: words of its message memory, and its doorbell");
	lck_mtx_lock(&hakios_aop_mailbox_lock);
	open = mailbox_open();
	lck_mtx_unlock(&hakios_aop_mailbox_lock);
	return open;
}

/* ---- TrustZone, and stopping the DSP --------------------------------------------------------------- */

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1

static LCK_MTX_DECLARE(dsp_lock, &adsp_lock_group);     /* the DSP being started or stopped */
static bool rails_asked;                /* our request for its rails stands (until it asks for its own, or is shut down) */
static bool dsp_started;
static bool stopped_by_its_word;
static uint32_t our_item;
static bool smp2p_agreed, smp2p_restart_acks, smp2p_restart_acked;

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;

	function |= SCM_64 | SCM_SIP;
	printf("adsp: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("adsp: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("adsp: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static uint32_t
our_item_find(void)
{
	uint32_t size = 0, at = smem_item(ITEM_SMP2P_OURS, &size);

	return at != 0 && size >= sizeof(struct smp2p) ? at : 0;
}

/* What Linux does when the DSP rings for SMP2P: agree on the features once, and answer a restart. */
static void
dsp_listen(const struct smp2p *theirs)
{
	struct smp2p ours;

	if (our_item == 0 || !smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) {
		return;
	}
	if (!smp2p_agreed) {
		uint32_t features = (ours.version >> 8) & (theirs->version >> 8);

		if ((theirs->version & 0xff) != (ours.version & 0xff)) {
			return;
		}
		if (features != ours.version >> 8) {
			smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
		}
		smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
		smp2p_agreed = true;
		printf("adsp: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
	}
	if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
		smp2p_restart_acked = !smp2p_restart_acked;
		printf("adsp: %u: the DSP's flags say it has restarted (%#x): answered in our flags (bit 1 = %u), and its SMP2P doorbell\n",
		    step, theirs->flags, smp2p_restart_acked);
		smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		settle();
		ring_send(IPCC_LPASS << 16 | IPCC_SMP2P);
	}
}

/* Linux's qcom_q6v5_request_stop(): our bit "stop", the DSP's "stop ack" waited for, our bit taken back. */
static bool
dsp_ask_to_stop(uint32_t bound_ms)
{
	struct smp2p ours, theirs;
	uint32_t at = our_item_find(), state, value_at;
	uint64_t asked, now;
	int entry = -1;

	if (at == 0 || !smem_peek(at, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || (ours.version & 0xff) != SMP2P_VERSION ||
	    (entry = smp2p_entry(&ours, OUR_ENTRY_STOP)) < 0) {
		printf("adsp: %u: the DSP is not asked to stop first: there is no SMP2P item of ours to ask it with\n", step);
		return false;
	}
	if (!dsp_words(&theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
		printf("adsp: %u: the DSP is not asked to stop first: its item has no entry to answer in\n", step);
		return false;
	}
	state = dsp_state(&theirs);
	if (state & DSP_STOP_ACK) {
		printf("adsp: %u: the DSP is not asked to stop: its entry says %#x, that it has stopped when asked before\n", step, state);
		return true;
	}
	if (!(state & DSP_READY) || (state & DSP_FATAL)) {
		printf("adsp: %u: the DSP is not asked to stop first: its entry says %#x, not \"up\"\n", step, state);
		return false;
	}
	ring_need();
	value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
	printf("adsp: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#lx, and its SMP2P doorbell\n", step,
	    OUR_ENTRY_STOP, SMEM_PHYS + value_at);
	snprintf(last_write, sizeof(last_write), "step %u asked the audio DSP to stop", step);
	smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
	settle();
	ring_send(IPCC_LPASS << 16 | IPCC_SMP2P);
	asked = now = now_us();
	while (now - asked < bound_ms * 1000ull) {
		if (dsp_words(&theirs) && (dsp_state(&theirs) & (DSP_STOP_ACK | DSP_FATAL))) {
			break;
		}
		sleep_us(200);
		now = now_us();
	}
	state = dsp_words(&theirs) ? dsp_state(&theirs) : 0;
	printf("adsp: %u: %s after %llu.%03llu ms; its entry says %#x\n", step, state & DSP_STOP_ACK ?
	    "THE AUDIO DSP ACKNOWLEDGES THE STOP" : "THE AUDIO DSP DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000,
	    (now - asked) % 1000, state);
	smem_poke(value_at, ours.entry[entry].value & ~DSP_STOP);
	settle();
	ring_send(IPCC_LPASS << 16 | IPCC_SMP2P);
	return (state & DSP_STOP_ACK) != 0;
}

static void link_stop(void);

/* Asked to stop, then shut down by TrustZone, then the always-on processor told that the image is gone. */
static bool
dsp_shutdown(const char *why)
{
	uint64_t answer = ~0ull;
	int32_t result;

	printf("adsp: %u: %s: the audio DSP is asked to stop, then TrustZone to shut it down\n", step, why);
	if (!leaving) {
		link_stop();
	}
	stopped_by_its_word = dsp_ask_to_stop(leaving ? 2000 : 5000);
	result = scm("to shut the audio DSP down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT SHUT THE AUDIO DSP DOWN: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		return false;
	}
	printf("adsp: %u: THE AUDIO DSP IS SHUT DOWN\n", step);
	if (modem) {
		hakios_adsp_give_back();    /* (what the hypervisor gave the modem and the Wi-Fi, back to us alone, for the kernel after this) */
	}
	ring_need();
	ring_write(IPCC_DISABLE, IPCC_LPASS << 16 | IPCC_SMP2P);
	doorbells();
	dsp_started = false;
	mailbox_send(modem ? "{class: image, res: load_state, name: modem, val: off}" : "{class: image, res: load_state, name: adsp, val: off}");
	if (rails_asked) {
		rails(0);
		rails_asked = false;
	}
	return true;
}

/* ---- step 4: TrustZone takes the header ------------------------------------------------------------ */

static bool
step_header(void)
{
	struct smp2p words;
	vm_offset_t page = 0;
	uint64_t answer = ~0ull;
	int32_t result;

	if (image.segments == 0) {
		return false;
	}
	if (dsp_words(&words)) {
		show_words("the DSP has left words in the shared memory", &words);
		announce("ask TrustZone to shut down an audio DSP that a kernel before this one may have left running");
		if (!dsp_shutdown("it may be running")) {
			return false;
		}
		if (!stopped_by_its_word && !option("adsp_again")) {
			printf("adsp: %u: THE AUDIO DSP IS NOT STARTED AGAIN: it was shut down without having acknowledged a stop, and "
			    "with the Wi-Fi's DSP a start after that reset the phone (adsp_again=1 to try all the same)\n", step);
			return false;
		}
	} else {
		printf("adsp: %u: the DSP has left nothing in the shared memory: it has not run since the phone was started\n", step);
	}

	announce("hand the image's header to TrustZone to check its signature (image 1), in a page of ours");
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("adsp: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, image.header, image.header_size);
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
	result = scm("to take the audio DSP's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_ADSP, kvtophys(page), 0, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("adsp: %u: TRUSTZONE ACCEPTS THE AUDIO DSP'S SIGNATURE FROM THIS KERNEL\n", step);

	announce("tell TrustZone where the image will be");
	result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_ADSP, ADSP_MEMORY, image.size, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("adsp: %u: TrustZone takes the place: %#x bytes at %#x\n", step, image.size, ADSP_MEMORY);
	return true;
}

/* ---- step 5: the image, in the DSP's memory --------------------------------------------------------- */

static bool image_written;

static uint64_t
segment_word(unsigned s, uint32_t at)
{
	uint64_t word = 0;

	for (unsigned b = 0; b < 8 && at + b < image.segment[s].file_size; b++) {
		word |= (uint64_t)image.segment[s].file[at + b] << (8 * b);
	}
	return word;
}

/* Byte i of segment s as it goes into the DSP's memory: its file's, then zeroes. */
static uint8_t
segment_byte(unsigned s, uint32_t i)
{
	return i < image.segment[s].file_size ? image.segment[s].file[i] : 0;
}

static bool
step_image(void)
{
	volatile uint64_t *dsp_memory;
	uint64_t written = 0, cleared = 0, wrong = 0;

	if (image.segments == 0) {
		return false;
	}
	announce("put the image's segments into the audio DSP's memory (the board's adsp_mem: memory of nobody's until TrustZone "
	    "starts the DSP), with zeroes where a segment is longer than its file, and read them back");
	dsp_memory = (volatile uint64_t *)ml_io_map_unmappable(ADSP_MEMORY, ADSP_MEMORY_SIZE, VM_WIMG_WCOMB);
	snprintf(last_write, sizeof(last_write), "step %u wrote the image at %#x", step, ADSP_MEMORY);
	/*
	 * Eight bytes at a time where the DSP's memory is a multiple of eight, a byte at a time at a
	 * segment's ends where it is not (the Mi 9T's image has such segments; redwood's has none).
	 */
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint8_t *bytes = (volatile uint8_t *)dsp_memory + image.segment[s].at;
		uint32_t i = 0, size = image.segment[s].memory_size;

		for (; i < size && ((image.segment[s].at + i) & 7); i++) {
			bytes[i] = segment_byte(s, i);
		}
		for (; i + 8 <= size; i += 8) {
			*(volatile uint64_t *)(bytes + i) = segment_word(s, i);
		}
		for (; i < size; i++) {
			bytes[i] = segment_byte(s, i);
		}
		written += image.segment[s].file_size;
		cleared += image.segment[s].memory_size - image.segment[s].file_size;
	}
	settle();
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint8_t *bytes = (volatile uint8_t *)dsp_memory + image.segment[s].at;
		uint32_t i = 0, size = image.segment[s].memory_size;

		for (; i < size && ((image.segment[s].at + i) & 7); i++) {
			wrong += bytes[i] != segment_byte(s, i);
		}
		for (; i + 8 <= size; i += 8) {
			wrong += *(volatile uint64_t *)(bytes + i) != segment_word(s, i);
		}
		for (; i < size; i++) {
			wrong += bytes[i] != segment_byte(s, i);
		}
	}
	printf("adsp: %u: %u segments, %llu bytes written and %llu cleared; read back, %llu places differ\n", step,
	    image.segments, written, cleared, wrong);
	settle();
	ml_io_unmap((vm_offset_t)dsp_memory, ADSP_MEMORY_SIZE);
	image_written = wrong == 0;
	image_forget();                         /* (it is in the DSP's memory now: what was read of the root, given back) */
	return image_written;
}

/* ---- step 6: our SMP2P item ------------------------------------------------------------------------ */

static struct block smem_lock = { "shared memory's lock", 0x01f43000, 0x1000 };
#define LOCK_OWNER              0x000
#define LOCK_US                 1u

/*
 * An item of ours made in the partition, as Linux's qcom_smem_alloc_private() makes one (hakios.wifi.c:
 * our_item_make()): our SMP2P item, and on the Mi 9T our GLINK queue as well (see step 8).
 */
static bool
item_make(uint32_t partition, uint32_t item, uint32_t wanted)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (wanted + 7) & ~7u, need_bytes = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at;
	bool made = false;

	lck_mtx_lock(&hakios_smem_alloc_lock);         /* (the Wi-Fi's start allocates under the same hardware lock) */
	need(&smem_lock);
	printf("adsp: %u: about to take the lock (%#x), write an entry for item %u with %u bytes behind it at the partition's mark, "
	    "move the mark by %u, and release the lock\n", step, smem_lock.phys + LOCK_OWNER, item, item_size, need_bytes);
	snprintf(last_write, sizeof(last_write), "step %u took the shared memory's lock", step);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base + LOCK_OWNER, LOCK_US);
		owner = rd(smem_lock.base + LOCK_OWNER);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		printf("adsp: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
		lck_mtx_unlock(&hakios_smem_alloc_lock);
		return false;
	}
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("adsp: %u: the partition's marks make no sense: nothing is written\n", step);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.item == item ||
		    entry.size > header.size) {
			break;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached) {
		printf("adsp: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
		    "nothing is written\n", step, at, header.free_uncached);
		goto out;
	}
	if (at + need_bytes > header.free_cached) {
		printf("adsp: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need_bytes, header.free_cached - at);
		goto out;
	}
	smem_poke(partition + at, SMEM_CANARY | item << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - wanted);
	smem_poke(partition + at + 12, 0);
	settle();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need_bytes);
	settle();
	made = true;
out:
	wr(smem_lock.base + LOCK_OWNER, 0);
	printf("adsp: %u: the lock is released (it reads %u)\n", step, rd(smem_lock.base + LOCK_OWNER));
	lck_mtx_unlock(&hakios_smem_alloc_lock);
	return made;
}

static void glink_reset(void);

static bool
step_our_item(void)
{
	struct smem_partition header;
	struct smp2p ours;
	uint32_t cacheline = 0, size = 0, partition, name[4] = { 0 };

	announce("look for an SMP2P item of ours (429) in our partition with the audio DSP, make it if there is none, and fill it in");
	partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("adsp: %u: there is no such partition\n", step);
		return false;
	}
	our_item = our_item_find();
	if (our_item != 0) {
		printf("adsp: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
		    ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	} else {
		if (!item_make(partition, ITEM_SMP2P_OURS, sizeof(struct smp2p))) {
			return false;
		}
		our_item = our_item_find();
		if (our_item == 0) {
			printf("adsp: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
			return false;
		}
		printf("adsp: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	}
	/* zeroes; from processor 0 to 2, room for 16 entries, "master-kernel" at zero, feature bit 0; last the version */
	snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
	for (uint32_t at = 0; at < sizeof(ours); at += 4) {
		smem_poke(our_item + at, 0);
	}
	settle();
	smem_poke(our_item, SMP2P_MAGIC);
	smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_ADSP << 16);
	strlcpy((char *)name, OUR_ENTRY_STOP, sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		smem_poke(our_item + SMP2P_AT_NAME(0) + 4 * word, name[word]);
	}
	smem_poke(our_item + SMP2P_AT_COUNTS, 16 | 1u << 16);
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
	ring_need();
	ring_send(IPCC_LPASS << 16 | IPCC_SMP2P);
	if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
		printf("adsp: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
		return false;
	}
	show_words("our item reads back", &ours);
	glink_reset();
	return true;
}

/* ---- step 7: the DSP, started ---------------------------------------------------------------------- */

/*
 * Android's kernel (this phone, dmesg): the image loaded at 8.92 s, "brought out of reset" at
 * 9.10, "error ready" 40 ms later; the audio services' protection domain up at 9.39.
 * Here (runs 085341 and later): the DSP's SMP2P item appears 17 ms after TrustZone returns, with
 * a ring of its SMP2P and GLINK doorbells; at 41 ms "slave-kernel" says 6, up and asking for its
 * own power, and our request for the rails is given back; at 2.2 s it adds "sleepstate_see".
 * Asked to stop, it acknowledges within 25 ms (0x10e), and it starts again in the same session.
 */
static bool
step_start(void)
{
	struct smp2p words, last;
	uint64_t answer = ~0ull, started, now, ready_at = 0, limit;
	uint32_t state = 0, rang, changes = 0, wait_seconds = 10;
	int32_t result;
	bool have, spoke = false, handed_over = false;

	if (!image_written) {
		printf("adsp: %u: the image is not in place\n", step);
		return false;
	}
	PE_parse_boot_argn("adsp_wait", &wait_seconds, sizeof(wait_seconds));
	bzero(&last, sizeof(last));
	have = dsp_words(&last);
	announce("listen for the audio DSP's doorbells, ask RPMh for its rails, tell the always-on processor that its image is "
	    "loaded, and ask TrustZone to authenticate the image and let the DSP out of reset");
	ring_need();
	for (unsigned i = 0; i < 8 && (rang = ring_received()) != IPCC_NOBODY && rang >> 16 == IPCC_LPASS; i++) {
		ring_write(IPCC_CLEAR, rang);
	}
	ring_write(IPCC_ENABLE, IPCC_LPASS << 16 | IPCC_SMP2P);
	ring_write(IPCC_ENABLE, IPCC_LPASS << 16 | IPCC_GLINK);
	bzero(&doorbell, sizeof(doorbell));
	lck_mtx_lock(&dsp_lock);
	if (!rails(384)) {
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	rails_asked = true;
	if (!mailbox_send(modem ? "{class: image, res: load_state, name: modem, val: on}" : "{class: image, res: load_state, name: adsp, val: on}")) {
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	result = scm("to authenticate the audio DSP's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_ADSP, 0, 0,
	    &answer);
	doorbell.since = started = now_us();
	image_written = false;
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT START THE AUDIO DSP: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		mailbox_send(modem ? "{class: image, res: load_state, name: modem, val: off}" : "{class: image, res: load_state, name: adsp, val: off}");
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	dsp_started = true;
	lck_mtx_unlock(&dsp_lock);
	printf("adsp: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE AUDIO DSP OUT OF RESET; watching for %u s\n", step,
	    wait_seconds);
	limit = started + wait_seconds * 1000000ull;
	for (;;) {
		now = now_us();
		if (now >= limit || !dsp_started) {
			break;
		}
		doorbells();
		spoke = spoke || doorbell.glink + doorbell.smp2p != 0;
		if (dsp_words(&words) && (!have || memcmp(&words, &last, sizeof(words)) != 0)) {
			char when[64];

			snprintf(when, sizeof(when), "+%llu.%03llu ms: the DSP's item %s", (now - started) / 1000, (now - started) % 1000,
			    have ? "changes" : "APPEARS");
			if (changes++ < 40) {
				show_words(when, &words);
			}
			last = words;
			have = spoke = true;
			dsp_listen(&words);
			state = dsp_state(&words);
			if ((state & DSP_READY) && ready_at == 0) {
				ready_at = now;
				limit = now + 3000000ull;
				printf("adsp: %u: THE AUDIO DSP SAYS IT IS UP: \"slave-kernel\" says %#x, %llu.%03llu ms after TrustZone started "
				    "it\n", step, state, (now - started) / 1000, (now - started) % 1000);
			}
			if ((state & DSP_HANDOVER) && !handed_over) {
				handed_over = true;
				printf("adsp: %u: the DSP says it has asked for its own power (bit 2), %llu.%03llu ms after the start: ours is "
				    "given back\n", step, (now - started) / 1000, (now - started) % 1000);
				lck_mtx_lock(&dsp_lock);
				if (rails_asked) {
					rails(0);
					rails_asked = false;
				}
				lck_mtx_unlock(&dsp_lock);
			}
			if (state & DSP_FATAL) {
				printf("adsp: %u: THE AUDIO DSP SAYS IT HAS GIVEN UP\n", step);
				show_crash_reason();
				break;
			}
		}
		sleep_us(200);
	}
	printf("adsp: %u: the watch is over after %llu ms; its doorbells heard: GLINK %u, SMP2P %u\n", step,
	    (now_us() - started) / 1000, doorbell.glink, doorbell.smp2p);
	show_partition();
	if (our_item != 0 && smem_peek(our_item, &words, sizeof(words))) {
		show_words("our item (429) now", &words);
	}
	{
		/* (what is at the bottom of our partition, entry by entry, as it is now) */
		uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
		struct smem_partition header;
		struct smem_entry entry;

		if (partition != 0 && smem_peek(partition, &header, sizeof(header))) {
			printf("adsp: %u: the partition's header: hosts %u and %u, size %#x, free from the bottom at +%#x, from the top at "
			    "+%#x; the table's line size %u\n", step, header.host0, header.host1, header.size, header.free_uncached,
			    header.free_cached, cacheline);
			for (uint32_t at = sizeof(header), n = 0; at + sizeof(entry) <= header.size && n < 8; n++) {
				smem_peek(partition + at, &entry, sizeof(entry));
				printf("adsp: %u:   +%#x: canary %#x item %u size %u padding %u/%u\n", step, at, entry.canary, entry.item,
				    entry.size, entry.padding_data, entry.padding_header);
				if (entry.canary != SMEM_CANARY) {
					break;
				}
				at += sizeof(entry) + entry.padding_header + entry.size;
			}
		}
	}
	if (state & DSP_FATAL) {
		return false;
	}
	if (ready_at == 0) {
		printf("adsp: %u: %s\n", step, spoke ? "THE AUDIO DSP RUNS BUT HAS NOT SAID \"UP\"" : "NOT A WORD FROM THE AUDIO DSP");
		show_crash_reason();
	}
	return ready_at != 0;
}

/* ---- GLINK (hakios.wifi.c; items 478 to 480 of our partition with the audio DSP) ------------------- */

#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480
#define GLINK_OUR_TAIL          0
#define GLINK_OUR_HEAD          4
#define GLINK_THEIR_TAIL        8
#define GLINK_THEIR_HEAD        12
#define GLINK_RESERVE           16

enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1         1
#define GLINK_INTENT_REUSE      1u

struct glink_message {
	uint16_t command, param1;
	uint32_t param2;
};
struct glink_packet {
	struct glink_packet *next;
	uint32_t length;
	uint8_t data[];
};

/*
 * The channels we answer when the DSP opens them, each with our number for it and the intents
 * we give it when it is open both ways (how many buffers of which size the DSP may send into):
 * IPCRTR, which carries QRTR (dt: qcom,intents of qcom,adsp_qrtr), and apr_audio_svc, which
 * carries APR, the audio services' own protocol (below; Android's apr_tal_rpmsg.c asks for no
 * particular intents: what the DSP sends there is a few hundred bytes, up to a few kilobytes).
 */
enum { CHANNEL_QRTR, CHANNEL_APR, CHANNEL_PMIC, CHANNEL_FASTRPC, CHANNELS };

struct glink_channel {
	const char *name;
	uint32_t ours;                          /* our number for it */
	struct {
		uint32_t size, count;
	} intents[3];
	uint32_t theirs;                        /* the DSP's number for it; 0: it has not opened it */
	bool asked, acknowledged, open;         /* our open sent; acknowledged; both ways done and our intents told */
	bool request_answered, request_granted; /* our last request for an intent */
	struct {
		uint32_t id, size;
		bool there, in_use;
	} their_intents[64];                    /* what we may send into */
	struct {
		uint32_t size, fill;
		uint8_t *data;
	} our_intents[24];                      /* what the DSP may send into; an intent's number is its place here + 1 */
	uint32_t our_intent_count;
	struct glink_packet *first, *last;      /* received, in order */
};

static LCK_MTX_DECLARE(glink_lock, &adsp_lock_group);
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;
	bool up, broken;
	uint32_t features;
	bool agreed;
	struct glink_channel channel[CHANNELS];
	uint32_t said, heard, trace;
	char others[4][33];                     /* the channels the DSP opens that nobody here speaks (for the log) */
} glink = {
	.channel = {
		[CHANNEL_QRTR] = { .name = "IPCRTR", .ours = 1, .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } } },
		[CHANNEL_APR] = { .name = "apr_audio_svc", .ours = 2, .intents = { { 0x200, 8 }, { 0x1000, 4 }, { 0x2000, 2 } } },
		/* the battery manager's (battmgr below): answers of a few dozen bytes, a name of 128 */
		[CHANNEL_PMIC] = { .name = "PMIC_RTR_ADSP_APPS", .ours = 3, .intents = { { 0x200, 6 }, { 0x1000, 2 }, { 0, 0 } } },
		/* FastRPC's (fastrpc below): its answers are a context and a result, its callers' a message of 40 bytes */
		[CHANNEL_FASTRPC] = { .name = "fastrpcglink-apps-dsp", .ours = 4, .intents = { { 0x100, 8 }, { 0x1000, 2 }, { 0, 0 } } },
	},
};

static struct glink_channel *
glink_theirs(uint32_t theirs)
{
	for (unsigned c = 0; c < CHANNELS; c++) {
		if (glink.channel[c].theirs != 0 && glink.channel[c].theirs == theirs) {
			return &glink.channel[c];
		}
	}
	return NULL;
}

static bool
glink_find(void)
{
	uint32_t size = 0;

	glink.descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
	if (glink.descriptor == 0 || size != 32) {
		return false;
	}
	glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &glink.ours_size);
	glink.theirs = smem_item(ITEM_GLINK_THEIR_QUEUE, &glink.theirs_size);
	return glink.ours != 0 && glink.theirs != 0 && glink.ours_size >= 1024 && glink.theirs_size >= 1024 &&
	       !(glink.ours_size & 7) && !(glink.theirs_size & 7);
}

static void
glink_reset(void)
{
	uint32_t size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);

	if (descriptor == 0 || size != 32) {
		printf("adsp: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
		return;
	}
	printf("adsp: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
	    "to %#x\n", step, SMEM_PHYS + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
	    smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
	if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
		printf("adsp: %u: about to write our two marks there (+4, +8): 0\n", step);
		smem_poke(descriptor + GLINK_OUR_HEAD, 0);
		smem_poke(descriptor + GLINK_THEIR_TAIL, 0);
		settle();
	}
}

static uint32_t
glink_waiting(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_THEIR_HEAD), tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);

	if (head >= glink.theirs_size || tail >= glink.theirs_size) {
		return 0;
	}
	return head >= tail ? head - tail : glink.theirs_size - tail + head;
}

static void
glink_peek(void *to, uint32_t offset, uint32_t count)
{
	uint32_t at = (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + offset) % glink.theirs_size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = smem[glink.theirs + (at + i) % glink.theirs_size];
	}
}

static void
glink_advance(uint32_t count)
{
	smem_poke(glink.descriptor + GLINK_THEIR_TAIL, (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + count) % glink.theirs_size);
}

static uint32_t
glink_room(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), tail = smem_word(glink.descriptor + GLINK_OUR_TAIL), room;

	if (head >= glink.ours_size || tail >= glink.ours_size) {
		return 0;
	}
	room = tail <= head ? glink.ours_size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

static bool
glink_put(const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
	volatile uint8_t *queue = (volatile uint8_t *)(uintptr_t)smem + glink.ours;
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
	uint32_t padded = (length + 7) & ~7u;

	if (!glink.up || glink_room() < padded) {
		return false;
	}
	for (uint32_t i = 0; i < padded; i++) {
		queue[(head + i) % glink.ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	settle();
	smem_poke(glink.descriptor + GLINK_OUR_HEAD, (head + padded) % glink.ours_size);
	settle();
	ring_send(IPCC_LPASS << 16 | IPCC_GLINK);
	glink.said++;
	return true;
}

static const char *
glink_name(unsigned command)
{
	static const char *const names[] = {
		"version", "version-ack", "open", "close", "open-ack", "intent", "done", "intent-request", "intent-request-ack",
		"data", "10", "close-ack", "data-more", "read-notify", "done-reuse", "signals"
	};

	return command < sizeof(names) / sizeof(names[0]) ? names[command] : "?";
}

static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = glink_put(&message, sizeof(message), more, more_length);

	if (glink.trace > 0 || !sent) {
		glink.trace -= glink.trace > 0;
		printf("adsp: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
	}
}

static bool
glink_offer(struct glink_channel *channel, uint32_t size)
{
	uint32_t words[2] = { size, channel->our_intent_count + 1 }, i = channel->our_intent_count;

	if (i >= sizeof(channel->our_intents) / sizeof(channel->our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (channel->our_intents[i].data == NULL || channel->our_intents[i].size != size) {
		channel->our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
	}
	if (channel->our_intents[i].data == NULL) {
		return false;
	}
	channel->our_intents[i].size = size;
	channel->our_intents[i].fill = 0;
	channel->our_intent_count++;
	glink_say(GLINK_INTENT, channel->ours, 1, words, sizeof(words));
	return true;
}

/* Everything the DSP has put into its queue, taken out and acted on (Linux's qcom_glink_native_rx()). With glink_lock held. */
static void
glink_pump(void)
{
	struct glink_message message;
	struct glink_channel *channel;
	uint32_t words[4];
	char name[33];

	while (glink.up && !glink.broken) {
		uint32_t waiting = glink_waiting(), length = sizeof(message);

		if (waiting < sizeof(message)) {
			return;
		}
		glink_peek(&message, 0, sizeof(message));
		if (message.command == GLINK_OPEN) {
			length += message.param2;
		} else if (message.command == GLINK_INTENT) {
			length += 8 * message.param2;
		} else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			glink_peek(words, 8, 8);
			length = 16 + words[0];
		}
		length = (length + 7) & ~7u;
		if (length > glink.theirs_size - 8) {
			printf("adsp: glink: A MESSAGE OF %u BYTES (%s %u %#x): not believed, and nothing more is read\n", length,
			    glink_name(message.command), message.param1, message.param2);
			glink.broken = true;
			return;
		}
		if (waiting < length) {
			return;
		}
		glink.heard++;
		if (glink.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE &&
		    message.command != GLINK_INTENT && message.command != GLINK_DONE_REUSE) {
			glink.trace--;
			printf("adsp: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
		}
		switch (message.command) {
		case GLINK_VERSION:
			if (message.param1 == GLINK_VERSION_1) {
				glink.features &= message.param2;
			}
			glink_advance(length);
			glink_say(GLINK_VERSION_ACK, GLINK_VERSION_1, glink.features, NULL, 0);
			break;
		case GLINK_VERSION_ACK:
			glink_advance(length);
			if (message.param1 == GLINK_VERSION_1 && message.param2 == glink.features) {
				glink.agreed = true;
			} else if (message.param1 != 0) {
				glink.features &= message.param2;
				glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
			}
			break;
		case GLINK_OPEN:
			bzero(name, sizeof(name));
			glink_peek(name, sizeof(message), message.param2 < sizeof(name) - 1 ? message.param2 : sizeof(name) - 1);
			glink_advance(length);
			channel = NULL;
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (strcmp(name, glink.channel[c].name) == 0) {
					channel = &glink.channel[c];
				}
			}
			if (channel == NULL) {
				printf("adsp: glink: the DSP opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", name,
				    message.param1);
				for (unsigned i = 0; i < 4; i++) {
					if (glink.others[i][0] == 0) {
						strlcpy(glink.others[i], name, sizeof(glink.others[i]));
						break;
					}
				}
				break;
			}
			printf("adsp: glink: the DSP opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
			    name, message.param1, channel->ours);
			channel->theirs = message.param1;
			glink_say(GLINK_OPEN_ACK, channel->theirs, 0, NULL, 0);
			if (!channel->asked) {
				uint32_t size = (uint32_t)strlen(channel->name) + 1;

				bzero(name, sizeof(name));
				strlcpy(name, channel->name, sizeof(name));
				glink_say(GLINK_OPEN, channel->ours, size, name, (size + 7) & ~7u);
				channel->asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].ours) {
					glink.channel[c].acknowledged = true;
				}
			}
			break;
		case GLINK_INTENT:
			channel = glink_theirs(message.param1);
			for (uint32_t i = 0; channel != NULL && i < message.param2; i++) {
				glink_peek(words, 8 + 8 * i, 8);
				for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
					if (!channel->their_intents[n].there || channel->their_intents[n].id == words[1]) {
						channel->their_intents[n].there = true;
						channel->their_intents[n].in_use = false;
						channel->their_intents[n].size = words[0];
						channel->their_intents[n].id = words[1];
						break;
					}
				}
			}
			glink_advance(length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			channel = glink_theirs(message.param1);
			for (unsigned n = 0; channel != NULL && n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
				if (channel->their_intents[n].there && channel->their_intents[n].id == message.param2) {
					channel->their_intents[n].in_use = false;
					channel->their_intents[n].there = message.command == GLINK_DONE_REUSE;
				}
			}
			glink_advance(length);
			break;
		case GLINK_INTENT_REQUEST:
			glink_advance(length);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				bool granted = glink_offer(channel, message.param2);

				glink_say(GLINK_INTENT_REQUEST_ACK, channel->ours, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].theirs || message.param1 == glink.channel[c].ours) {
					glink.channel[c].request_answered = true;
					glink.channel[c].request_granted = message.param2 != 0;
				}
			}
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			channel = glink_theirs(message.param1);
			if (channel == NULL || intent >= channel->our_intent_count ||
			    words[0] > channel->our_intents[intent].size - channel->our_intents[intent].fill) {
				printf("adsp: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
				    message.param1, message.param2);
				glink_advance(length);
				break;
			}
			glink_peek(channel->our_intents[intent].data + channel->our_intents[intent].fill, 16, words[0]);
			channel->our_intents[intent].fill += words[0];
			glink_advance(length);
			if (words[1] == 0) {
				struct glink_packet *packet = kalloc_data(sizeof(*packet) + channel->our_intents[intent].fill, Z_WAITOK);

				if (packet != NULL) {
					packet->next = NULL;
					packet->length = channel->our_intents[intent].fill;
					memcpy(packet->data, channel->our_intents[intent].data, packet->length);
					if (channel->last != NULL) {
						channel->last->next = packet;
					} else {
						channel->first = packet;
					}
					channel->last = packet;
				}
				channel->our_intents[intent].fill = 0;
				glink_say(GLINK_DONE_REUSE, channel->ours, message.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			glink_advance(length);
			ring_send(IPCC_LPASS << 16 | IPCC_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				printf("adsp: glink: THE DSP CLOSES \"%s\"\n", channel->name);
				channel->open = false;
				channel->theirs = 0;
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			glink_advance(length);
			break;
		default:
			printf("adsp: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
			    message.param1, message.param2);
			glink.broken = true;
			return;
		}
		for (unsigned c = 0; c < CHANNELS; c++) {
			channel = &glink.channel[c];
			if (channel->theirs != 0 && channel->acknowledged && !channel->open) {
				channel->open = true;
				printf("adsp: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the DSP's %u); telling the DSP our intents\n",
				    channel->name, channel->ours, channel->theirs);
				for (unsigned i = 0; i < 3; i++) {
					for (unsigned n = 0; n < channel->intents[i].count; n++) {
						glink_offer(channel, channel->intents[i].size);
					}
				}
			}
		}
	}
}

/* One whole message to the DSP on a channel; waits up to five seconds for an intent of the DSP's that it fits. */
static int
glink_send(struct glink_channel *channel, const void *data, uint32_t length)
{
	struct {
		struct glink_message message;
		uint32_t size, left;
	} header = { { GLINK_DATA, (uint16_t)channel->ours, 0 }, length, 0 };
	bool asked = false, fits;
	int found = -1;

	lck_mtx_lock(&glink_lock);
	if (glink.up && length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
		lck_mtx_unlock(&glink_lock);
		return EMSGSIZE;
	}
	for (unsigned waited = 0;; waited++) {
		glink_pump();
		if (!glink.up || !channel->open || glink.broken) {
			lck_mtx_unlock(&glink_lock);
			return ENOTCONN;
		}
		found = -1;
		fits = false;
		for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
			if (!channel->their_intents[n].there || channel->their_intents[n].size < length) {
				continue;
			}
			fits = true;
			if (!channel->their_intents[n].in_use &&
			    (found < 0 || channel->their_intents[n].size < channel->their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
		}
		if (!fits && !asked) {
			channel->request_answered = false;
			glink_say(GLINK_INTENT_REQUEST, channel->ours, length, NULL, 0);
			asked = true;
		}
		if (waited >= 5000 || (!fits && asked && channel->request_answered && !channel->request_granted)) {
			lck_mtx_unlock(&glink_lock);
			return found < 0 ? ENOBUFS : EAGAIN;
		}
		lck_mtx_unlock(&glink_lock);
		sleep_ms(1);
		lck_mtx_lock(&glink_lock);
	}
	channel->their_intents[found].in_use = true;
	header.message.param2 = channel->their_intents[found].id;
	glink_put(&header, sizeof(header), data, length);
	lck_mtx_unlock(&glink_lock);
	return 0;
}

/* ---- QRTR: hello, and the services the DSP tells of (hakios.wifi.c, without ports of our own yet) ---- */

#define QRTR_US                 1u
#define QRTR_CONTROL            0xfffffffeu
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7,
       QRTR_NEW_LOOKUP = 10, QRTR_DEL_LOOKUP = 11 };

struct qrtr_header {
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
	uint32_t command;
	uint32_t words[4];
};

static struct {
	uint32_t service, instance, node, port;
	bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer, qrtr_in, qrtr_out, qrtr_hellos, qrtr_lookups;
/* what the DSP looked for on our side (the first 16): what it is waiting for before it starts more */
static struct {
	uint32_t service, instance;
} qrtr_wanted[16];

/*
 * The names of the QMI services that matter for what comes next (the numbers are the ones every
 * Qualcomm client uses; Linux has them as constants beside each client): the sensors' own service is
 * how a sensor client would speak to the DSP's sensor framework, and it only appears once the DSP's
 * sensor protection domain has started -- which waits for the service locator and notifier it looks
 * for on our side. kern.hakios.adsp_services tells, on the phone, which of these there are.
 */
static const char *
qrtr_service_name(uint32_t service)
{
	switch (service) {
	case 16: return "location (LOC)";
	case 43: return "subsystem control (SSCTL)";
	case 64: return "service locator (SERVREG_LOC)";
	case 66: return "service notifier (SERVREG_NOTIF)";
	case 69: return "Wi-Fi firmware (WLFW)";
	case 400: return "sensors (SNS client, SEE)";
	default: return "";
	}
}

static int
adsp_services_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	static char text[4096];
	size_t n = 0;

	n += snprintf(text + n, sizeof(text) - n, "offered by the DSP:\n");
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]) && n < sizeof(text); i++) {
		if (qrtr_servers[i].there) {
			n += snprintf(text + n, sizeof(text) - n, "  %u version %u instance %u at %u:%u %s\n", qrtr_servers[i].service,
			    qrtr_servers[i].instance & 0xff, qrtr_servers[i].instance >> 8, qrtr_servers[i].node, qrtr_servers[i].port,
			    qrtr_service_name(qrtr_servers[i].service));
		}
	}
	n += snprintf(text + n, sizeof(text) - n, n < sizeof(text) ? "looked for by the DSP on our side (%u times):\n" : "", qrtr_lookups);
	for (unsigned i = 0; i < sizeof(qrtr_wanted) / sizeof(qrtr_wanted[0]) && i < qrtr_lookups && n < sizeof(text); i++) {
		n += snprintf(text + n, sizeof(text) - n, "  %u instance %u %s\n", qrtr_wanted[i].service, qrtr_wanted[i].instance,
		    qrtr_service_name(qrtr_wanted[i].service));
	}
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}
#ifndef HAKIOS_SM7150_MODEM_COPY                  /* (the audio DSP's copy has them; hakios.adsp_sm7150_modem.c is the modem's) */
SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, adsp_services, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    adsp_services_sysctl, "A", "the QMI services the audio DSP offers, and those it looks for on our side");
#endif

static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet;
	int error;

	packet = kalloc_data(sizeof(*packet) + padded, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		return ENOMEM;
	}
	packet->version = 1;
	packet->type = type;
	packet->from_node = QRTR_US;
	packet->from_port = from_port;
	packet->size = length;
	packet->to_node = to_node;
	packet->to_port = to_port;
	memcpy(packet + 1, data, length);
	error = glink_send(&glink.channel[CHANNEL_QRTR], packet, (uint32_t)sizeof(*packet) + padded);
	kfree_data(packet, sizeof(*packet) + padded);
	qrtr_out += error == 0;
	return error;
}

/*
 * ---- the service locator (QMI service 64, SERVREG_LOC): Linux's qcom_pd_mapper.c ----------------
 *
 * The DSP's protection domains find each other's services through a locator on our side, which
 * Linux has as qcom_pd_mapper.c (and userspace as pd-mapper, from the DSP's own .jsn files):
 * asked for a service ("tms/servreg", "avs/audio"), it answers with the domains that offer it.
 * Without it the Mi 9T's DSP lets its sensor process hang on the question, until that process's
 * watchdog takes the whole DSP down ("err_qdi.c:1040:EF:sensor_process ... dog_virtual_user.c",
 * 36 s after the start, 2026-10-23). The domains are the DSP's (its adspr.jsn, adsps.jsn,
 * adspua.jsn; Linux's sm7150_domains): every one offers tms/servreg, the audio one avs/audio.
 * Announced to the DSP once it has said hello, at our port 1, as Linux does it: version 0x101,
 * instance 0.
 */
#define SERVREG_LOCATOR_SERVICE         0x40
#define SERVREG_LOCATOR_VERSION         0x101
#define SERVREG_GET_DOMAIN_LIST_REQ     0x21
#define SERVREG_LOC_PFR_REQ             0x24
#define SERVREG_NAME_LENGTH             64
#define SERVREG_DOMAIN_LIST_LENGTH      32
#define LOCATOR_PORT                    1

struct servreg_location_entry {
	char name[SERVREG_NAME_LENGTH + 1];
	uint8_t service_data_valid;
	uint32_t service_data;
	uint32_t instance;
};
struct servreg_get_domain_list_req {
	char service_name[SERVREG_NAME_LENGTH + 1];
	uint8_t domain_offset_valid;
	uint32_t domain_offset;
};
struct servreg_get_domain_list_resp {
	struct qmi_response_type_v01 resp;
	uint8_t total_domains_valid;
	uint16_t total_domains;
	uint8_t db_rev_count_valid;
	uint16_t db_rev_count;
	uint8_t domain_list_valid;
	uint32_t domain_list_len;
	struct servreg_location_entry domain_list[SERVREG_DOMAIN_LIST_LENGTH];
};
struct servreg_loc_pfr_resp {
	struct qmi_response_type_v01 resp;
};

/* (Linux's qcom_pdr_msg.c, element for element) */
static const struct qmi_elem_info servreg_location_entry_ei[] = {
	{ .data_type = QMI_STRING, .elem_len = SERVREG_NAME_LENGTH + 1, .elem_size = sizeof(char), .array_type = NO_ARRAY,
	  .offset = __offsetof(struct servreg_location_entry, name) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .offset = __offsetof(struct servreg_location_entry, instance) },
	{ .data_type = QMI_UNSIGNED_1_BYTE, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .offset = __offsetof(struct servreg_location_entry, service_data_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .offset = __offsetof(struct servreg_location_entry, service_data) },
	{ .data_type = QMI_EOTI }
};
static const struct qmi_elem_info servreg_get_domain_list_req_ei[] = {
	{ .data_type = QMI_STRING, .elem_len = SERVREG_NAME_LENGTH + 1, .elem_size = sizeof(char), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = __offsetof(struct servreg_get_domain_list_req, service_name) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = __offsetof(struct servreg_get_domain_list_req, domain_offset_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = __offsetof(struct servreg_get_domain_list_req, domain_offset) },
	{ .data_type = QMI_EOTI }
};
static const struct qmi_elem_info servreg_get_domain_list_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct qmi_response_type_v01), .array_type = NO_ARRAY,
	  .tlv_type = 0x02, .offset = __offsetof(struct servreg_get_domain_list_resp, resp), .ei_array = qmi_response_type_v01_ei },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY, .tlv_type = 0x10,
	  .offset = __offsetof(struct servreg_get_domain_list_resp, total_domains_valid) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = __offsetof(struct servreg_get_domain_list_resp, total_domains) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY, .tlv_type = 0x11,
	  .offset = __offsetof(struct servreg_get_domain_list_resp, db_rev_count_valid) },
	{ .data_type = QMI_UNSIGNED_2_BYTE, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x11, .offset = __offsetof(struct servreg_get_domain_list_resp, db_rev_count) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY, .tlv_type = 0x12,
	  .offset = __offsetof(struct servreg_get_domain_list_resp, domain_list_valid) },
	{ .data_type = QMI_DATA_LEN, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY, .tlv_type = 0x12,
	  .offset = __offsetof(struct servreg_get_domain_list_resp, domain_list_len) },
	{ .data_type = QMI_STRUCT, .elem_len = SERVREG_DOMAIN_LIST_LENGTH, .elem_size = sizeof(struct servreg_location_entry),
	  .array_type = VAR_LEN_ARRAY, .tlv_type = 0x12, .offset = __offsetof(struct servreg_get_domain_list_resp, domain_list),
	  .ei_array = servreg_location_entry_ei },
	{ .data_type = QMI_EOTI }
};
static const struct qmi_elem_info servreg_loc_pfr_resp_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct qmi_response_type_v01), .array_type = NO_ARRAY,
	  .tlv_type = 0x02, .offset = __offsetof(struct servreg_loc_pfr_resp, resp), .ei_array = qmi_response_type_v01_ei },
	{ .data_type = QMI_EOTI }
};

/*
 * The root domain offers "tms/pdr_enabled" too (Linux's adsp_root_pd_pdr): with it the DSP restarts a
 * protection domain that fails on its own instead of giving up altogether. Without it, the Mi 9T's
 * sensor process -- which hangs here, most likely on its files, that Android serves over FastRPC
 * (the channel fastrpcglink-apps-dsp, not answered here) -- takes the audio down with it after
 * 37 s, a domain restart notice (PFR) the last thing the DSP says (2026-10-23).
 */
static const struct {
	const char *domain;
	uint32_t instance;
	const char *also;               /* a service it offers besides tms/servreg */
} locator_domains[] = {
	{ "msm/adsp/audio_pd", 74, "avs/audio" },
	{ "msm/adsp/root_pd", 74, "tms/pdr_enabled" },
	{ "msm/adsp/sensor_pd", 74, NULL },
};
/* the modem's (its modemr.jsn and modemuw.jsn; Linux's mpss_root_pd and mpss_wlan_pd) */
static const struct {
	const char *domain;
	uint32_t instance;
	const char *also, *also2;
} locator_modem_domains[] = {
	{ "msm/modem/root_pd", 180, "tms/pdr_enabled", NULL },
	{ "msm/modem/wlan_pd", 180, "wlan/fw", "kernel/elf_loader" },
};
static uint32_t locator_questions;

static int qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data,
    uint32_t length);

static void
locator_request(const struct qrtr_header *header, const uint8_t *message, uint32_t length)
{
	static uint8_t answer[2400];
	static struct servreg_get_domain_list_resp list;
	uint16_t transaction, id;
	int size = -1;

	if (length < QMI_HEADER_SIZE || message[0] != QMI_REQUEST) {
		return;
	}
	transaction = (uint16_t)(message[1] | message[2] << 8);
	id = (uint16_t)(message[3] | message[4] << 8);
	if (id == SERVREG_GET_DOMAIN_LIST_REQ) {
		struct servreg_get_domain_list_req request;
		uint32_t offset;

		bzero(&request, sizeof(request));
		bzero(&list, sizeof(list));
		hakios_qmi_decode(message, length, servreg_get_domain_list_req_ei, &request);
		offset = request.domain_offset_valid ? request.domain_offset : 0;
		list.resp.result = QMI_RESULT_SUCCESS_V01;
		list.db_rev_count_valid = 1;
		list.db_rev_count = 1;
		list.total_domains_valid = 1;
		for (unsigned i = 0; modem && i < sizeof(locator_modem_domains) / sizeof(locator_modem_domains[0]); i++) {
			if (strcmp(request.service_name, "tms/servreg") != 0 &&
			    strcmp(request.service_name, locator_modem_domains[i].also) != 0 &&
			    !(locator_modem_domains[i].also2 != NULL && strcmp(request.service_name, locator_modem_domains[i].also2) == 0)) {
				continue;
			}
			list.domain_list_valid = 1;
			if (list.total_domains++ >= offset && list.domain_list_len < SERVREG_DOMAIN_LIST_LENGTH) {
				strlcpy(list.domain_list[list.domain_list_len].name, locator_modem_domains[i].domain, SERVREG_NAME_LENGTH + 1);
				list.domain_list[list.domain_list_len++].instance = locator_modem_domains[i].instance;
			}
		}
		for (unsigned i = 0; !modem && i < sizeof(locator_domains) / sizeof(locator_domains[0]); i++) {
			uint32_t sensors = 1;

			/* boot-arg adsp_locator_sensors=0: the sensor domain not named (an experiment: is it left down then?) */
			PE_parse_boot_argn("adsp_locator_sensors", &sensors, sizeof(sensors));
			if (!sensors && strcmp(locator_domains[i].domain, "msm/adsp/sensor_pd") == 0) {
				continue;
			}
			if (strcmp(request.service_name, "tms/servreg") != 0 &&
			    !(locator_domains[i].also != NULL && strcmp(request.service_name, locator_domains[i].also) == 0)) {
				continue;
			}
			list.domain_list_valid = 1;
			if (list.total_domains++ >= offset && list.domain_list_len < SERVREG_DOMAIN_LIST_LENGTH) {
				strlcpy(list.domain_list[list.domain_list_len].name, locator_domains[i].domain, SERVREG_NAME_LENGTH + 1);
				list.domain_list[list.domain_list_len++].instance = locator_domains[i].instance;
			}
		}
		size = hakios_qmi_encode(QMI_RESPONSE, id, transaction, servreg_get_domain_list_resp_ei, &list, answer, sizeof(answer));
		if (locator_questions++ < 16) {
			printf("adsp: locator: node %u port %u asks where \"%s\" is: %u domains\n", header->from_node, header->from_port,
			    request.service_name, list.total_domains);
		}
	} else if (id == SERVREG_LOC_PFR_REQ) {
		struct servreg_loc_pfr_resp done = { { QMI_RESULT_SUCCESS_V01, QMI_ERR_NONE_V01 } };

		size = hakios_qmi_encode(QMI_RESPONSE, id, transaction, servreg_loc_pfr_resp_ei, &done, answer, sizeof(answer));
		printf("adsp: locator: node %u tells of a protection domain's restart (PFR): noted\n", header->from_node);
	} else {
		printf("adsp: locator: a request %#x this does not know: not answered\n", id);
		return;
	}
	if (size > 0) {
		qrtr_out_packet(QRTR_DATA, LOCATOR_PORT, header->from_node, header->from_port, answer, (uint32_t)size);
	}
}


/* ---- rmtfs: the modem's file systems (adsp_modem=1) ------------------------------------------------ */

/*
 * The modem keeps its settings (EFS) in four partitions of the phone's storage, which it reads and
 * writes through the application processor: Linux's rmtfs (github.com/linux-msm/rmtfs, its
 * qmi_rmtfs.qmi), QMI service 14 version 1 instance 0 at a port of ours. The modem opens a
 * partition by its name (/boot/modem_fs1 -> modemst1, modem_fs2 -> modemst2, modem_fsg -> fsg,
 * modem_fsc -> fsc), asks for a buffer (alloc_buff: the address of memory both of us may touch),
 * and has sectors copied between the partition and that buffer (rw_iovec). Without it the modem
 * waits for its EFS and gives up after some 40 s (2026-10-24).
 *
 * Here the partitions are read whole, once, from /dev/rufs_NAME (hakios.ufs.c, with ufs=2), and the
 * modem's writes go to that copy alone: nothing here ever writes to the phone's storage. The
 * buffer, 2 MB of ours, is given to the modem as well by the hypervisor (TrustZone's
 * QCOM_SCM_MP_ASSIGN, as Linux's qcom_scm_assign_mem() for qcom,rmtfs-mem with qcom,vmid MSS_MSA).
 */
#define RMTFS_PORT              2
#define RMTFS_SERVICE           14
#define RMTFS_VERSION           1
#define RMTFS_BUFFER            0x200000u
#define RMTFS_SECTOR            512u
#define RMTFS_MAX_PART          (8u << 20)
#define VMID_HLOS               3
#define VMID_MSS_MSA            15
#define PERM_RW                 6       /* QCOM_SCM_PERM_READ | WRITE */

static struct {
	const char *path, *partition;
	uint8_t *data;
	uint32_t size;
	bool tried;
} rmtfs_parts[] = {
	{ "/boot/modem_fs1", "modemst1", NULL, 0, false },
	{ "/boot/modem_fs2", "modemst2", NULL, 0, false },
	{ "/boot/modem_fsg", "fsg", NULL, 0, false },
	{ "/boot/modem_fsc", "fsc", NULL, 0, false },
};
static vm_offset_t rmtfs_buffer;
static uint64_t rmtfs_buffer_phys;
static bool rmtfs_assigned;
static uint32_t rmtfs_requests, rmtfs_reads, rmtfs_writes;

/*
 * TrustZone: memory given by the hypervisor from the VMs that have it (src) to others (dst, each
 * read-write), as Linux's qcom_scm_assign_mem() (QCOM_SCM_MP_ASSIGN, seven arguments: the four past
 * the third in a buffer at x5). 0: done. For rmtfs (below) and the Wi-Fi's MSA (hakios.wlfw10.c).
 */
int
hakios_adsp_assign(uint64_t phys, uint64_t size, const uint32_t *src, unsigned n_src, const uint32_t *dst, unsigned n_dst)
{
	vm_offset_t page;
	struct {
		uint64_t addr, size;
	} *region;
	uint32_t *source;
	struct {
		uint32_t vmid, perm;
		uint64_t ctx;
		uint32_t ctx_size, unused;
	} *dest;
	uint64_t *extra, base;
	int32_t result = -1;
	uint64_t session = 0;

	if (n_src > 8 || n_dst > 4 || kmem_alloc_contig(kernel_map, &page, 4096, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return -1;
	}
	base = kvtophys(page);
	region = (void *)page;
	source = (void *)(page + 64);
	dest = (void *)(page + 128);
	extra = (void *)(page + 256);
	region->addr = phys;
	region->size = size;
	for (unsigned i = 0; i < n_src; i++) {
		source[i] = src[i];
	}
	for (unsigned i = 0; i < n_dst; i++) {
		dest[i].vmid = dst[i];
		dest[i].perm = PERM_RW;
	}
	extra[0] = 4 * n_src;                   /* src_sz */
	extra[1] = base + 128;                  /* dest */
	extra[2] = n_dst * sizeof(dest[0]);     /* dest_sz */
	extra[3] = 0;
	for (vm_offset_t line = page; line < page + 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	printf("adsp: about to ask TrustZone to give %#llx bytes at %#llx from %u VMs (the first %u) to %u (the first %u)\n",
	    size, phys, n_src, src[0], n_dst, dst[0]);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? (SCM_CALL(0x0c, 0x16) | SCM_64 | SCM_SIP) : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = 7 | 1u << 4 | 1u << 8 | 1u << 12;       /* RO VAL RO VAL RO VAL VAL */
		register uint64_t x2 __asm__("x2") = base;
		register uint64_t x3 __asm__("x3") = sizeof(*region);
		register uint64_t x4 __asm__("x4") = base + 64;
		register uint64_t x5 __asm__("x5") = base + 256;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			result = (int32_t)x0;
			printf("adsp: TrustZone returns %d (%#llx)\n", result, x1);
			break;
		}
	}
	/* (the page stays: TrustZone may keep looking at it) */
	return result;
}

/* rmtfs's buffer to the modem too; owners as they are now: 0 HLOS (the phone just started), 1 HLOS and the modem, 2 the modem */
static int
rmtfs_assign(uint64_t phys, uint64_t size, unsigned owners)
{
	static const uint32_t hlos[] = { VMID_HLOS }, both[] = { VMID_HLOS, VMID_MSS_MSA }, mss[] = { VMID_MSS_MSA };

	return hakios_adsp_assign(phys, size, owners == 0 ? hlos : owners == 1 ? both : mss, owners == 1 ? 2 : 1, both, 2);
}

/*
 * Memory given to the modem (and the Wi-Fi) is ours alone again once the modem is shut down, as
 * Linux's rmtfs_mem and ath10k give it back: a kernel after this one, on a phone that has not been
 * restarted, finds it as a phone just started has it (else the hypervisor refuses to give it again).
 */
static struct {
	uint64_t phys, size;
	uint32_t owners[4];
	unsigned count;
} given[8];
static unsigned given_count;

void
hakios_adsp_given(uint64_t phys, uint64_t size, const uint32_t *owners, unsigned count)
{
	if (given_count < 8 && count <= 4) {
		given[given_count].phys = phys;
		given[given_count].size = size;
		bcopy(owners, given[given_count].owners, count * sizeof(owners[0]));
		given[given_count++].count = count;
	}
}

static void
hakios_adsp_give_back(void)
{
	static const uint32_t hlos[] = { VMID_HLOS };

	for (unsigned i = 0; i < given_count; i++) {
		printf("adsp: %#llx (%#llx bytes) back to us alone: %s\n", given[i].phys, given[i].size,
		    hakios_adsp_assign(given[i].phys, given[i].size, given[i].owners, given[i].count, hlos, 1) == 0 ? "done" : "REFUSED");
	}
	given_count = 0;
}

/* the partition behind a path, read whole the first time */
static int
rmtfs_open_part(const char *path)
{
	for (unsigned i = 0; i < sizeof(rmtfs_parts) / sizeof(rmtfs_parts[0]); i++) {
		char dev[64];
		vnode_t vnode = NULLVP;
		vfs_context_t context = vfs_context_kernel();
		uint32_t got = 0;

		if (strcmp(path, rmtfs_parts[i].path) != 0) {
			continue;
		}
		if (rmtfs_parts[i].tried) {
			return rmtfs_parts[i].data != NULL ? (int)i : -1;
		}
		rmtfs_parts[i].tried = true;
		snprintf(dev, sizeof(dev), "/dev/rufs_%s", rmtfs_parts[i].partition);
		rmtfs_parts[i].data = kalloc_data(RMTFS_MAX_PART, Z_WAITOK | Z_ZERO);
		if (rmtfs_parts[i].data == NULL) {
			return -1;
		}
		if (vnode_open(dev, FREAD, 0, 0, &vnode, context) == 0) {
			while (got < RMTFS_MAX_PART) {
				int left = 0;

				if (vn_rdwr(UIO_READ, vnode, (caddr_t)rmtfs_parts[i].data + got, 0x10000, got, UIO_SYSSPACE, 0,
				    vfs_context_ucred(context), &left, vfs_context_proc(context)) != 0 || left != 0) {
					got += 0x10000 - (uint32_t)left;
					break;
				}
				got += 0x10000;
			}
			vnode_close(vnode, FREAD, context);
			printf("adsp: rmtfs: %s: %u KB read from %s (the modem's writes stay in this copy)\n", path, got / 1024, dev);
		} else {
			/* (no ufs=2, or no such partition: an empty one, which the modem may format, in memory only) */
			got = 2u << 20;
			printf("adsp: rmtfs: %s: no %s: an empty partition of %u KB, in memory only\n", path, dev, got / 1024);
		}
		rmtfs_parts[i].size = got & ~(RMTFS_SECTOR - 1);
		return (int)i;
	}
	return -1;
}

/* a TLV of a QMI message: its value and length, or NULL */
static const uint8_t *
qmi_tlv(const uint8_t *message, uint32_t length, uint8_t type, uint16_t *tlv_length)
{
	for (uint32_t at = QMI_HEADER_SIZE; at + 3 <= length;) {
		uint16_t l = (uint16_t)(message[at + 1] | message[at + 2] << 8);

		if (at + 3 + l > length) {
			return NULL;
		}
		if (message[at] == type) {
			*tlv_length = l;
			return message + at + 3;
		}
		at += 3u + l;
	}
	return NULL;
}

static uint32_t
qmi_put_tlv(uint8_t *to, uint32_t at, uint8_t type, const void *value, uint16_t length)
{
	to[at] = type;
	to[at + 1] = (uint8_t)length;
	to[at + 2] = (uint8_t)(length >> 8);
	bcopy(value, to + at + 3, length);
	return at + 3u + length;
}

static void
rmtfs_request(const struct qrtr_header *header, const uint8_t *message, uint32_t length)
{
	uint8_t answer[64];
	uint16_t transaction, id, l = 0;
	uint16_t result[2] = { 0, 0 };          /* success, no error */
	uint32_t at = QMI_HEADER_SIZE, caller = 0;
	const uint8_t *v;

	if (length < QMI_HEADER_SIZE || message[0] != QMI_REQUEST) {
		return;
	}
	transaction = (uint16_t)(message[1] | message[2] << 8);
	id = (uint16_t)(message[3] | message[4] << 8);
	rmtfs_requests++;
	if ((v = qmi_tlv(message, length, 1, &l)) != NULL && l >= 4 && id != 1) {
		bcopy(v, &caller, 4);
	}
	switch (id) {
	case 1: {       /* open: the path; caller_id */
		char path[64] = "";
		int part;

		if ((v = qmi_tlv(message, length, 1, &l)) != NULL) {
			bcopy(v, path, min(l, sizeof(path) - 1));
		}
		part = rmtfs_open_part(path);
		printf("adsp: rmtfs: the modem opens \"%s\": %s\n", path, part >= 0 ? "given" : "NOT KNOWN");
		if (part < 0) {
			result[0] = 1;
			result[1] = 1;
			at = qmi_put_tlv(answer, at, 2, result, 4);
		} else {
			caller = (uint32_t)part;
			at = qmi_put_tlv(answer, at, 2, result, 4);
			at = qmi_put_tlv(answer, at, 0x10, &caller, 4);
		}
		break;
	}
	case 2:         /* close */
		at = qmi_put_tlv(answer, at, 2, result, 4);
		break;
	case 4: {       /* alloc_buff: caller_id, size; the buffer's address */
		uint32_t size = 0;

		if ((v = qmi_tlv(message, length, 2, &l)) != NULL && l >= 4) {
			bcopy(v, &size, 4);
		}
		printf("adsp: rmtfs: the modem asks for a buffer of %u KB: %s\n", size / 1024, rmtfs_assigned && size <= RMTFS_BUFFER ?
		    "ours" : "none");
		if (!rmtfs_assigned || size > RMTFS_BUFFER) {
			result[0] = 1;
			result[1] = 1;
			at = qmi_put_tlv(answer, at, 2, result, 4);
		} else {
			at = qmi_put_tlv(answer, at, 2, result, 4);
			at = qmi_put_tlv(answer, at, 0x10, &rmtfs_buffer_phys, 8);
		}
		break;
	}
	case 3: {       /* rw_iovec: caller_id, direction (0 read, 1 write), {sector, buffer offset, sectors}[], force sync */
		uint8_t direction = 0;
		const uint8_t *list;
		uint16_t list_length = 0;
		bool ok = caller < sizeof(rmtfs_parts) / sizeof(rmtfs_parts[0]) && rmtfs_parts[caller].data != NULL;

		if ((v = qmi_tlv(message, length, 2, &l)) != NULL && l >= 1) {
			direction = v[0];
		}
		list = qmi_tlv(message, length, 3, &list_length);
		if (list == NULL || list_length < 1) {
			ok = false;
		}
		for (uint32_t n = 0, i = 0; ok && i < list[0] && 1 + 12 * (i + 1) <= list_length; i++) {
			uint32_t e[3];
			uint64_t from, bytes;

			(void)n;
			bcopy(list + 1 + 12 * i, e, 12);
			/* (the place in the buffer is the modem's address of it, as Linux's rmtfs takes it) */
			e[1] -= (uint32_t)rmtfs_buffer_phys;
			from = (uint64_t)e[0] * RMTFS_SECTOR;
			bytes = (uint64_t)e[2] * RMTFS_SECTOR;
			if (from + bytes > rmtfs_parts[caller].size || (uint64_t)e[1] + bytes > RMTFS_BUFFER) {
				printf("adsp: rmtfs: %s of sectors %u+%u at %#x of the buffer: outside the partition (%u KB)\n",
				    direction ? "write" : "read", e[0], e[2], e[1], rmtfs_parts[caller].size / 1024);
				ok = false;
				break;
			}
			if (direction == 0) {
				bcopy(rmtfs_parts[caller].data + from, (void *)(rmtfs_buffer + e[1]), (size_t)bytes);
				for (vm_offset_t line = rmtfs_buffer + e[1]; line < rmtfs_buffer + e[1] + bytes; line += 64) {
					__asm__ volatile ("dc civac, %0" : : "r" (line));
				}
				rmtfs_reads++;
			} else {
				for (vm_offset_t line = rmtfs_buffer + e[1]; line < rmtfs_buffer + e[1] + bytes; line += 64) {
					__asm__ volatile ("dc civac, %0" : : "r" (line));
				}
				__asm__ volatile ("dsb sy");
				bcopy((void *)(rmtfs_buffer + e[1]), rmtfs_parts[caller].data + from, (size_t)bytes);
				rmtfs_writes++;
			}
		}
		__asm__ volatile ("dsb sy");
		if (rmtfs_reads + rmtfs_writes <= 40) {
			printf("adsp: rmtfs: %s %s of %s: %s\n", direction ? "write" : "read", list ? "list" : "(none)",
			    caller < 4 ? rmtfs_parts[caller].partition : "?", ok ? "done" : "REFUSED");
		}
		if (!ok) {
			result[0] = 1;
			result[1] = 1;
		}
		at = qmi_put_tlv(answer, at, 2, result, 4);
		break;
	}
	case 5: {       /* dev_error: none */
		uint8_t none = 0;

		at = qmi_put_tlv(answer, at, 2, result, 4);
		at = qmi_put_tlv(answer, at, 0x10, &none, 1);
		break;
	}
	default:
		printf("adsp: rmtfs: a request %#x this does not know\n", id);
		result[0] = 1;
		result[1] = 2;
		at = qmi_put_tlv(answer, at, 2, result, 4);
		break;
	}
	answer[0] = QMI_RESPONSE;
	answer[1] = (uint8_t)transaction;
	answer[2] = (uint8_t)(transaction >> 8);
	answer[3] = (uint8_t)id;
	answer[4] = (uint8_t)(id >> 8);
	answer[5] = (uint8_t)(at - QMI_HEADER_SIZE);
	answer[6] = (uint8_t)((at - QMI_HEADER_SIZE) >> 8);
	qrtr_out_packet(QRTR_DATA, RMTFS_PORT, header->from_node, header->from_port, answer, at);
}

static void
rmtfs_start(uint32_t node)
{
	struct qrtr_control server = { .command = QRTR_NEW_SERVER, .words = { RMTFS_SERVICE, RMTFS_VERSION, QRTR_US, RMTFS_PORT } };
	int error;

	if (rmtfs_buffer == 0) {
		/*
		 * Below 4 GB (the modem's addresses of it are 32 bits) and in one piece, which XNU does not
		 * give: memory reserved for the video core, which nothing here starts (dt: pil_video_region@93400000,
		 * 5 MB), outside what XNU manages; soc_rmtfs_mem= another place.
		 */
		rmtfs_buffer_phys = soc_addr("rmtfs_mem", 0x93400000u);
		rmtfs_buffer = ml_io_map_unmappable((vm_offset_t)rmtfs_buffer_phys, RMTFS_BUFFER, VM_WIMG_WCOMB);
		if (rmtfs_buffer == 0) {
			printf("adsp: rmtfs: the modem's buffer at %#llx could not be mapped\n", rmtfs_buffer_phys);
			return;
		}
		bzero((void *)rmtfs_buffer, RMTFS_BUFFER);
		if (rmtfs_buffer_phys + RMTFS_BUFFER > 0x100000000ull) {
			printf("adsp: rmtfs: the buffer is at %#llx, above 4 GB, where the modem cannot reach it\n", rmtfs_buffer_phys);
		}
		for (vm_offset_t line = rmtfs_buffer; line < rmtfs_buffer + RMTFS_BUFFER; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy");
		rmtfs_assigned = rmtfs_assign(rmtfs_buffer_phys, RMTFS_BUFFER, 0) == 0 ||
		    rmtfs_assign(rmtfs_buffer_phys, RMTFS_BUFFER, 1) == 0 || rmtfs_assign(rmtfs_buffer_phys, RMTFS_BUFFER, 2) == 0;
		if (rmtfs_assigned) {
			static const uint32_t both[] = { VMID_HLOS, VMID_MSS_MSA };

			hakios_adsp_given(rmtfs_buffer_phys, RMTFS_BUFFER, both, 2);
		}
	}
	/* (the version and instance: the instance is the version's high bits, as in the locator's words) */
	error = qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, node, QRTR_CONTROL, &server, sizeof(server));
	printf("adsp: qrtr: our rmtfs (14) at port %u, its buffer at %#llx (%s), told to node %u: %s\n", RMTFS_PORT,
	    rmtfs_buffer_phys, rmtfs_assigned ? "the modem's too" : "NOT GIVEN TO THE MODEM", node, error == 0 ? "SENT" : "NOT SENT");
}


/* ---- TFTP over QRTR: the modem's files (adsp_modem=1) ----------------------------------------------- */

/*
 * What the modem loads after its start -- its WLAN domain's firmware wlanmdsp.mbn, the radio's board
 * data bdwlan.*, and more -- it reads from the application processor by TFTP over QRTR (Linux's
 * linux-msm/tqftpserv): service 4096 version 1 instance 0 at our port 3. A read request (RRQ:
 * name, "octet", options blksize, tsize, wsize, rsize, seek, timeoutms) is answered from a port of
 * its own, with an OACK of the options (tsize the file's size) or at once with block 1; each ACK n
 * is answered with blocks n+1 to n+wsize; a block shorter than blksize ends the file (or rsize
 * bytes from seek). /readonly/firmware/image/NAME and /readonly/vendor/firmware(_mnt/image)/NAME
 * are /fw/NAME on the root (fw-modem/ there). Writes (WRQ, to /readwrite/) are taken and dropped.
 */
#define TFTP_PORT               3
#define TFTP_SERVICE            4096
#define TFTP_FIRST_PORT         0x4000
#define TFTP_CLIENTS            8
enum { TFTP_RRQ = 1, TFTP_WRQ, TFTP_DATA, TFTP_ACK, TFTP_ERROR, TFTP_OACK };

static struct {
	bool used, writing;
	uint32_t node, port;            /* the modem's end */
	char name[96];
	const uint8_t *data;
	uint64_t size, seek, end;
	uint32_t blksize, wsize, sent;
} tftp[TFTP_CLIENTS];
static uint32_t tftp_files;

static void
tftp_error(uint32_t from_port, uint32_t node, uint32_t port, uint16_t code, const char *text)
{
	uint8_t packet[80];
	size_t l = strlen(text);

	packet[0] = 0;
	packet[1] = TFTP_ERROR;
	packet[2] = (uint8_t)(code >> 8);
	packet[3] = (uint8_t)code;
	strlcpy((char *)packet + 4, text, sizeof(packet) - 4);
	qrtr_out_packet(QRTR_DATA, from_port, node, port, packet, (uint32_t)(4 + min(l, sizeof(packet) - 5) + 1));
}

static void
tftp_close(unsigned c)
{
	if (tftp[c].data != NULL) {
		hakios_adsp_fw_free(tftp[c].data, tftp[c].size);
	}
	bzero(&tftp[c], sizeof(tftp[c]));
}

/* blocks after `last`, as many as the window: false when the file is over */
static bool
tftp_send_after(unsigned c, uint32_t last)
{
	static uint8_t packet[4 + 0x10000];

	for (uint32_t b = last + 1; b <= last + tftp[c].wsize; b++) {
		uint64_t at = tftp[c].seek + (uint64_t)(b - 1) * tftp[c].blksize;
		uint32_t n = at >= tftp[c].end ? 0 : (uint32_t)min((uint64_t)tftp[c].blksize, tftp[c].end - at);

		if (at > tftp[c].end) {
			return false;
		}
		packet[0] = 0;
		packet[1] = TFTP_DATA;
		packet[2] = (uint8_t)(b >> 8);
		packet[3] = (uint8_t)b;
		if (n) {
			bcopy(tftp[c].data + at, packet + 4, n);
		}
		if (qrtr_out_packet(QRTR_DATA, TFTP_FIRST_PORT + c, tftp[c].node, tftp[c].port, packet, 4 + n) != 0) {
			printf("adsp: tftp: block %u of %s could not be sent\n", b, tftp[c].name);
			return false;
		}
		tftp[c].sent = b;
		if (n < tftp[c].blksize) {
			return false;
		}
	}
	return true;
}

/* a request to our port 3 */
static void
tftp_request(const struct qrtr_header *header, const uint8_t *data, uint32_t length)
{
	char buffer[512], *name, *mode, *p, *end, oack[256];
	uint32_t opcode, blksize = 512, wsize = 1, o = 2;
	uint64_t rsize = 0, seek = 0;
	bool options = false, want_tsize = false, want_timeout = false, want_rsize = false, want_seek = false, want_wsize = false;
	unsigned timeoutms = 1000, c;
	const char *base;
	int at;

	if (length < 4 || length > sizeof(buffer) - 1) {
		return;
	}
	bcopy(data, buffer, length);
	buffer[length] = 0;
	end = buffer + length;
	opcode = (uint32_t)(data[0] << 8 | data[1]);
	name = buffer + 2;
	mode = name + strnlen(name, (size_t)(end - name)) + 1;
	if (mode >= end) {
		return;
	}
	for (p = mode + strnlen(mode, (size_t)(end - mode)) + 1; p < end;) {
		char *key = p, *value = p + strnlen(p, (size_t)(end - p)) + 1;

		if (value >= end) {
			break;
		}
		options = true;
		if (strcmp(key, "blksize") == 0) {
			blksize = (uint32_t)strtoul(value, NULL, 10);
		} else if (strcmp(key, "tsize") == 0) {
			want_tsize = true;
		} else if (strcmp(key, "wsize") == 0) {
			wsize = (uint32_t)strtoul(value, NULL, 10);
			want_wsize = true;
		} else if (strcmp(key, "rsize") == 0) {
			rsize = strtoul(value, NULL, 10);
			want_rsize = true;
		} else if (strcmp(key, "seek") == 0) {
			seek = strtoul(value, NULL, 10);
			want_seek = true;
		} else if (strcmp(key, "timeoutms") == 0) {
			timeoutms = (unsigned)strtoul(value, NULL, 10);
			want_timeout = true;
		}
		p = value + strnlen(value, (size_t)(end - value)) + 1;
	}
	if (blksize < 8 || blksize > 0x10000 - 64) {
		blksize = 512;
	}
	if (wsize < 1 || wsize > 64) {
		wsize = 1;
	}
	for (c = 0; c < TFTP_CLIENTS && tftp[c].used; c++) {
	}
	if (c == TFTP_CLIENTS) {
		/* (the oldest goes: the modem does not always say it is done) */
		c = 0;
		tftp_close(0);
	}
	if (opcode == TFTP_WRQ) {
		uint8_t ack[4] = { 0, TFTP_ACK, 0, 0 };

		printf("adsp: tftp: the modem writes \"%s\": taken and dropped\n", name);
		tftp[c].used = tftp[c].writing = true;
		tftp[c].node = header->from_node;
		tftp[c].port = header->from_port;
		strlcpy(tftp[c].name, name, sizeof(tftp[c].name));
		tftp[c].blksize = blksize;
		if (options) {
			at = snprintf(oack, sizeof(oack), "%c%c", 0, TFTP_OACK);
			at += snprintf(oack + at, sizeof(oack) - (size_t)at, "blksize%c%u%c", 0, blksize, 0);
			qrtr_out_packet(QRTR_DATA, TFTP_FIRST_PORT + c, header->from_node, header->from_port, oack, (uint32_t)at);
		} else {
			qrtr_out_packet(QRTR_DATA, TFTP_FIRST_PORT + c, header->from_node, header->from_port, ack, 4);
		}
		return;
	}
	if (opcode != TFTP_RRQ) {
		return;
	}
	base = NULL;
	if (strncmp(name, "/readonly/firmware/image/", 25) == 0) {
		base = name + 25;
	} else if (strncmp(name, "/readonly/vendor/firmware_mnt/image/", 36) == 0) {
		base = name + 36;
	} else if (strncmp(name, "/readonly/vendor/firmware/", 26) == 0) {
		base = name + 26;
	} else if (strncmp(name, "/readonly/firmware/", 19) == 0) {
		base = name + 19;
	}
	tftp[c].used = true;
	tftp[c].node = header->from_node;
	tftp[c].port = header->from_port;
	strlcpy(tftp[c].name, name, sizeof(tftp[c].name));
	tftp[c].data = base != NULL && strchr(base, '/') == NULL ? root_file(base, &tftp[c].size) : NULL;
	if (tftp_files++ < 60) {
		printf("adsp: tftp: the modem reads \"%s\" (blksize %u, wsize %u%s%s): %s%s\n", name, blksize, wsize,
		    want_rsize ? ", rsize" : "", want_seek ? ", seek" : "", tftp[c].data ? "there, " : "NOT THERE", "");
	}
	if (tftp[c].data == NULL) {
		tftp_error(TFTP_FIRST_PORT + c, header->from_node, header->from_port, 1, "file not found");
		tftp_close(c);
		return;
	}
	tftp[c].seek = min(seek, tftp[c].size);
	tftp[c].end = rsize ? min(tftp[c].seek + rsize, tftp[c].size) : tftp[c].size;
	tftp[c].blksize = blksize;
	tftp[c].wsize = wsize;
	if (!options) {
		if (!tftp_send_after(c, 0)) {
			tftp_close(c);
		}
		return;
	}
	at = 2;
	oack[0] = 0;
	oack[1] = TFTP_OACK;
	at += snprintf(oack + at, sizeof(oack) - (size_t)at, "blksize%c%u%c", 0, blksize, 0);
	if (want_timeout) {
		at += snprintf(oack + at, sizeof(oack) - (size_t)at, "timeoutms%c%u%c", 0, timeoutms, 0);
	}
	if (want_tsize) {
		at += snprintf(oack + at, sizeof(oack) - (size_t)at, "tsize%c%llu%c", 0, tftp[c].size, 0);
	}
	if (want_wsize) {
		at += snprintf(oack + at, sizeof(oack) - (size_t)at, "wsize%c%u%c", 0, wsize, 0);
	}
	if (want_rsize) {
		at += snprintf(oack + at, sizeof(oack) - (size_t)at, "rsize%c%llu%c", 0, rsize, 0);
	}
	if (want_seek) {
		at += snprintf(oack + at, sizeof(oack) - (size_t)at, "seek%c%llu%c", 0, seek, 0);
	}
	(void)o;
	qrtr_out_packet(QRTR_DATA, TFTP_FIRST_PORT + c, header->from_node, header->from_port, oack, (uint32_t)at);
}

/* an ACK, an error or (a write's) data, to a transfer's port */
static void
tftp_transfer(const struct qrtr_header *header, const uint8_t *data, uint32_t length)
{
	unsigned c = header->to_port - TFTP_FIRST_PORT;
	uint32_t opcode, block;

	if (c >= TFTP_CLIENTS || !tftp[c].used || length < 4) {
		return;
	}
	opcode = (uint32_t)(data[0] << 8 | data[1]);
	block = (uint32_t)(data[2] << 8 | data[3]);
	if (opcode == TFTP_ERROR) {
		/* (error 9, "end of transfer", is how the modem ends a read it has had enough of) */
		tftp_close(c);
		return;
	}
	if (tftp[c].writing) {
		if (opcode == TFTP_DATA) {
			uint8_t ack[4] = { 0, TFTP_ACK, (uint8_t)(block >> 8), (uint8_t)block };

			qrtr_out_packet(QRTR_DATA, header->to_port, header->from_node, header->from_port, ack, 4);
			if (length - 4 < tftp[c].blksize) {
				tftp_close(c);
			}
		}
		return;
	}
	if (opcode == TFTP_ACK && !tftp_send_after(c, block)) {
		/* (the last block is out: the transfer stays until its ACK, an error, or the next one needs the place) */
		if (block >= tftp[c].sent) {
			tftp_close(c);
		}
	}
}

static void
tftp_start(uint32_t node)
{
	struct qrtr_control server = { .command = QRTR_NEW_SERVER, .words = { TFTP_SERVICE, 1, QRTR_US, TFTP_PORT } };
	int error = qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, node, QRTR_CONTROL, &server, sizeof(server));

	printf("adsp: qrtr: our TFTP (4096) at port %u, told to node %u: %s\n", TFTP_PORT, node, error == 0 ? "SENT" : "NOT SENT");
}


/* ---- QRTR for the QMI helpers (sys/hakios.qrtr.h): ports of ours for clients, as hakios.wifi.c's ------------- */

/*
 * The QMI helpers (hakios.qmi.c) talk through these to the processor on this link: a client's port, its
 * datagrams, and the servers it looks up -- told from the link's thread, once each, as they appear.
 * With adsp_modem=1 the helpers are given this transport when the modem says hello: the Wi-Fi
 * firmware's service (WLFW, 69) is on the modem.
 */
#include <sys/hakios.qrtr.h>

#define QRTR_FIRST_CLIENT_PORT  0x100

struct hakios_qrtr_port {
	uint32_t number;                        /* 0: free */
	hakios_qrtr_receive_t receive;
	hakios_qrtr_server_t server;
	void *context;
	struct {
		uint32_t service, instance;     /* the instance as on the wire: version | instance << 8; 0: any */
		bool asked;
		uint64_t told;                  /* the servers of qrtr_servers[] it has been told of */
	} lookups[4];
};

static LCK_MTX_DECLARE(qrtr_lock, &adsp_lock_group);
static struct hakios_qrtr_port qrtr_ports[16];
static uint32_t qrtr_next_port = QRTR_FIRST_CLIENT_PORT;

/* From the link's thread: lookups told of the servers they have not been told of yet. */
static void
qrtr_fresh(void)
{
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		for (unsigned l = 0; l < 4; l++) {
			for (unsigned i = 0; i < 64 && i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
				hakios_qrtr_server_t server;
				void *context;
				uint32_t service, instance, node, port;

				lck_mtx_lock(&qrtr_lock);
				if (qrtr_ports[p].number == 0 || !qrtr_ports[p].lookups[l].asked || !qrtr_servers[i].there ||
				    (qrtr_ports[p].lookups[l].told & 1ull << i) || qrtr_servers[i].service != qrtr_ports[p].lookups[l].service ||
				    (qrtr_ports[p].lookups[l].instance != 0 && qrtr_servers[i].instance != qrtr_ports[p].lookups[l].instance)) {
					lck_mtx_unlock(&qrtr_lock);
					continue;
				}
				qrtr_ports[p].lookups[l].told |= 1ull << i;
				server = qrtr_ports[p].server;
				context = qrtr_ports[p].context;
				service = qrtr_servers[i].service;
				instance = qrtr_servers[i].instance;
				node = qrtr_servers[i].node;
				port = qrtr_servers[i].port;
				lck_mtx_unlock(&qrtr_lock);
				if (server != NULL) {
					server(context, true, service, instance & 0xff, instance >> 8, node, port);
				}
			}
		}
	}
}

/* A datagram to one of the clients' ports: true if there is such a port. From the link's thread. */
static bool
qrtr_to_client(const struct qrtr_header *header, const uint8_t *data)
{
	hakios_qrtr_receive_t receive = NULL;
	void *context = NULL;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		if (qrtr_ports[p].number != 0 && qrtr_ports[p].number == header->to_port) {
			receive = qrtr_ports[p].receive;
			context = qrtr_ports[p].context;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	if (receive == NULL) {
		return false;
	}
	receive(context, header->from_node, header->from_port, data, header->size);
	return true;
}

static struct hakios_qrtr_port *
qrtr_open(hakios_qrtr_receive_t receive, hakios_qrtr_server_t server, void *context)
{
	struct hakios_qrtr_port *port = NULL;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]) && port == NULL; p++) {
		if (qrtr_ports[p].number == 0) {
			port = &qrtr_ports[p];
			bzero(port, sizeof(*port));
			port->number = qrtr_next_port++;
			port->receive = receive;
			port->server = server;
			port->context = context;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return port;
}

static int
qrtr_send(struct hakios_qrtr_port *port, uint32_t node, uint32_t to, const void *data, size_t length)
{
	if (port == NULL || port->number == 0 || length > 8192) {
		return EINVAL;
	}
	return qrtr_out_packet(QRTR_DATA, port->number, node, to, data, (uint32_t)length);
}

static int
qrtr_lookup(struct hakios_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	int error = ENOSPC;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned l = 0; l < 4 && error != 0 && port != NULL && port->number != 0; l++) {
		if (!port->lookups[l].asked) {
			port->lookups[l].service = service;
			port->lookups[l].instance = instance != 0 ? (version & 0xff) | instance << 8 : 0;
			port->lookups[l].asked = true;
			port->lookups[l].told = 0;
			error = 0;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return error;
}

static void
qrtr_close(struct hakios_qrtr_port *port)
{
	struct qrtr_control gone = { .command = QRTR_DEL_CLIENT, .words = { QRTR_US, port ? port->number : 0 } };

	if (port == NULL || port->number == 0) {
		return;
	}
	lck_mtx_lock(&qrtr_lock);
	port->number = 0;
	lck_mtx_unlock(&qrtr_lock);
	if (qrtr_peer != 0 || modem) {
		qrtr_out_packet(QRTR_DEL_CLIENT, gone.words[1], qrtr_peer, QRTR_CONTROL, &gone, sizeof(gone));
	}
}

static const struct hakios_qrtr_ops modem_qrtr_ops = { .open = qrtr_open, .send = qrtr_send, .lookup = qrtr_lookup, .close = qrtr_close };

static void
qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	int found = -1, error;

	switch (control->command) {
	case QRTR_HELLO:
		if (qrtr_hellos++ != 0) {
			break;
		}
		error = qrtr_out_packet(QRTR_HELLO, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &hello, sizeof(hello));
		printf("adsp: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
		{
			struct qrtr_control locator = { .command = QRTR_NEW_SERVER,
				                        .words = { SERVREG_LOCATOR_SERVICE, SERVREG_LOCATOR_VERSION, QRTR_US, LOCATOR_PORT } };

			error = qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &locator, sizeof(locator));
			printf("adsp: qrtr: our service locator (64) at port %u, told to node %u: %s\n", LOCATOR_PORT, header->from_node,
			    error == 0 ? "SENT" : "NOT SENT");
		}
		if (modem) {
			rmtfs_start(header->from_node);
			tftp_start(header->from_node);
			hakios_qmi_set_transport(&modem_qrtr_ops);  /* (the QMI helpers' clients: hakios.wlfw10.c) */
			if (modem_link_hook != NULL) {
				modem_link_hook();
			}
		}
		break;
	case QRTR_NEW_SERVER:
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				found = (int)i;
				break;
			}
			if (!qrtr_servers[i].there && found < 0) {
				found = (int)i;
			}
		}
		if (found >= 0) {
			qrtr_servers[found].there = true;
			qrtr_servers[found].service = control->words[0];
			qrtr_servers[found].instance = control->words[1];
			qrtr_servers[found].node = control->words[2];
			qrtr_servers[found].port = control->words[3];
		}
		printf("adsp: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
		break;
	case QRTR_DEL_SERVER:
		printf("adsp: qrtr: service %u at node %u port %u has gone\n", control->words[0], control->words[2], control->words[3]);
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				qrtr_servers[i].there = false;
			}
		}
		break;
	case QRTR_NEW_LOOKUP:
		/* the DSP looks for a service of ours (a protection domain's locator, say): there are none yet */
		if (qrtr_lookups < sizeof(qrtr_wanted) / sizeof(qrtr_wanted[0])) {
			qrtr_wanted[qrtr_lookups].service = control->words[0];
			qrtr_wanted[qrtr_lookups].instance = control->words[1];
		}
		if (qrtr_lookups++ < 16) {
			printf("adsp: qrtr: the DSP looks for service %u (%#x), instance %u: nobody here offers it\n", control->words[0],
			    control->words[0], control->words[1]);
		}
		break;
	case QRTR_BYE:
		printf("adsp: qrtr: BYE from node %u\n", header->from_node);
		bzero(qrtr_servers, sizeof(qrtr_servers));
		break;
	default:
		break;
	}
}

static void
qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header header;
	uint32_t at;

	if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else if (length >= 16 && data[0] == 3) {
		bzero(&header, sizeof(header));
		header.type = data[1];
		header.confirm = data[2] & 1;
		memcpy(&header.size, data + 4, 4);
		header.from_node = (uint32_t)(data[8] | data[9] << 8);
		header.from_port = (uint32_t)(data[10] | data[11] << 8);
		header.to_node = (uint32_t)(data[12] | data[13] << 8);
		header.to_port = (uint32_t)(data[14] | data[15] << 8);
		header.from_port = header.from_port == 0xfffe ? QRTR_CONTROL : header.from_port;
		header.to_port = header.to_port == 0xfffe ? QRTR_CONTROL : header.to_port;
		at = 16 + data[3];
	} else {
		printf("adsp: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
		return;
	}
	if (header.size == 0 || at + header.size > length) {
		printf("adsp: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
		return;
	}
	qrtr_in++;
	if (qrtr_peer == 0) {
		qrtr_peer = header.from_node;
	}
	if (header.type == QRTR_RESUME_TX) {
		return;
	}
	if (header.confirm) {
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };

		qrtr_out_packet(QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
	}
	if (header.to_port == QRTR_CONTROL || header.type == QRTR_HELLO || header.type == QRTR_BYE ||
	    header.type == QRTR_NEW_SERVER || header.type == QRTR_DEL_SERVER || header.type == QRTR_NEW_LOOKUP) {
		struct qrtr_control control = { 0 };

		memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
		qrtr_control(&header, &control);
		return;
	}
	if (header.type == QRTR_DATA && header.to_port == LOCATOR_PORT) {
		locator_request(&header, data + at, header.size);
		return;
	}
	if (header.type == QRTR_DATA && header.to_port == RMTFS_PORT && modem) {
		rmtfs_request(&header, data + at, header.size);
		return;
	}
	if (header.type == QRTR_DATA && header.to_port == TFTP_PORT && modem) {
		tftp_request(&header, data + at, header.size);
		return;
	}
	if (header.type == QRTR_DATA && header.to_port >= TFTP_FIRST_PORT && header.to_port < TFTP_FIRST_PORT + TFTP_CLIENTS && modem) {
		tftp_transfer(&header, data + at, header.size);
		return;
	}
	if (header.type == QRTR_DATA && qrtr_to_client(&header, data + at)) {
		return;
	}
	printf("adsp: qrtr: a datagram of %u bytes from node %u port %u to our port %u: nobody here listens there\n", header.size,
	    header.from_node, header.from_port, header.to_port);
}

/* ---- APR: the audio services' protocol, on the channel apr_audio_svc ------------------------------------- */

/*
 * What Android's audio drivers say to the DSP's audio services (Linux: drivers/soc/qcom/apr.c,
 * include/linux/soc/qcom/apr.h; Android's q6core.c): a packet of a header of eight words -- a
 * word of kind, header length and version and the packet's length, who it is from and to (a
 * service, a domain, a port each: we are domain 5, the DSP 4), a token the answer carries back,
 * an opcode -- and what the opcode takes. The DSP's core service (3) answers whether the audio
 * services are up (AVCS_CMD_ADSP_EVENT_GET_STATE, which Android asks again and again after the DSP
 * has started, until the answer is 1) and which services it has and in which versions.
 */
#define APR_DOMAIN_ADSP                 4
#define APR_DOMAIN_APPS                 5
#define APR_SVC_ADSP_CORE               3
#define APR_MSG_TYPE_SEQ_CMD            2
#define APR_HDR_FIELD(type, bytes)      ((uint16_t)((type) << 8 | ((bytes) / 4) << 4))      /* version 0 */
#define APR_BASIC_RSP_RESULT            0x000110e8u
#define AVCS_GET_VERSIONS               0x00012905u
#define AVCS_GET_VERSIONS_RSP           0x00012906u
#define AVCS_CMD_ADSP_EVENT_GET_STATE   0x0001290cu
#define AVCS_CMDRSP_ADSP_EVENT_GET_STATE 0x0001290du

struct apr_header {
	uint16_t field, size;
	uint8_t from_service, from_domain;
	uint16_t from_port;
	uint8_t to_service, to_domain;
	uint16_t to_port;
	uint32_t token, opcode;
};

static struct {
	uint32_t opcode, token, length;
	uint32_t words[64];
	volatile bool there;
} apr_answer;
static uint32_t apr_in, apr_out, apr_next_token = 1;
static bool audio_up;                   /* the core service has said that the audio services are up (step 11) */
static uint32_t apr_waited_for;         /* the token a question waits for the answer to; 0: none */
static hakios_apr_events_t apr_events; /* the sound's (hakios.sound.c): what nobody waits for */
static hakios_apr_events_t apr_events_second;  /* the microphones' (hakios.mic.c) */
static LCK_MTX_DECLARE(apr_lock, &adsp_lock_group);     /* one question at a time */

/* A packet from the DSP's audio services. From the link's thread. */
static void
apr_input(const uint8_t *data, uint32_t length)
{
	struct apr_header header;
	uint32_t header_length, payload;

	if (length < sizeof(header)) {
		printf("adsp: apr: %u bytes, no APR packet\n", length);
		return;
	}
	memcpy(&header, data, sizeof(header));
	header_length = ((header.field >> 4) & 0xf) * 4;
	if (header_length < sizeof(header) || header_length > length) {
		printf("adsp: apr: a header of %u bytes in a packet of %u: dropped\n", header_length, length);
		return;
	}
	payload = (header.size < length ? header.size : length) - header_length;
	apr_in++;
	if (header.token != apr_waited_for || apr_waited_for == 0) {
		hakios_apr_events_t events = apr_events;
		uint32_t words[64];

		bzero(words, sizeof(words));
		memcpy(words, data + header_length, payload < sizeof(words) ? payload : sizeof(words));
		if (events != NULL && events(header.from_service, header.from_port, header.opcode, header.token, words,
		    payload < sizeof(words) ? payload : sizeof(words))) {
			return;
		}
		events = apr_events_second;
		if (events != NULL && events(header.from_service, header.from_port, header.opcode, header.token, words,
		    payload < sizeof(words) ? payload : sizeof(words))) {
			return;
		}
	}
	if (apr_in <= 40) {
		uint32_t w[4] = { 0, 0, 0, 0 };

		memcpy(w, data + header_length, payload < sizeof(w) ? payload : sizeof(w));
		printf("adsp: apr: < from service %u of domain %u (port %u) to %u of %u (port %u): opcode %#x, token %u, %u bytes: %#x %#x "
		    "%#x %#x\n", header.from_service, header.from_domain, header.from_port, header.to_service, header.to_domain,
		    header.to_port, header.opcode, header.token, payload, w[0], w[1], w[2], w[3]);
	}
	bzero(apr_answer.words, sizeof(apr_answer.words));
	memcpy(apr_answer.words, data + header_length, payload < sizeof(apr_answer.words) ? payload : sizeof(apr_answer.words));
	apr_answer.length = payload;
	apr_answer.opcode = header.opcode;
	apr_answer.token = header.token;
	apr_answer.there = true;
}

/*
 * A command to one of the DSP's services (and its answer waited for, with ms): true when it was
 * sent and, if waited for, answered; the answer is in apr_answer and, if asked for, in *answer.
 * The port is the service's own numbering of what the command is for (ASM: session << 8 | stream).
 */
bool
hakios_adsp_apr(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t ms, struct hakios_apr_answer *answer)
{
	uint8_t packet[sizeof(struct apr_header) + 256];
	struct apr_header *header = (struct apr_header *)packet;
	bool answered = false;
	int error;

	if (length > sizeof(packet) - sizeof(*header) || !glink.channel[CHANNEL_APR].open) {
		return false;
	}
	bzero(packet, sizeof(packet));
	header->field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, sizeof(*header));
	header->size = (uint16_t)(sizeof(*header) + length);
	header->from_service = service;
	header->from_domain = APR_DOMAIN_APPS;
	header->from_port = port;
	header->to_service = service;
	header->to_domain = APR_DOMAIN_ADSP;
	header->to_port = port;
	header->opcode = opcode;
	if (length != 0) {
		memcpy(packet + sizeof(*header), payload, length);
	}
	if (ms == 0) {
		/* (sent and not waited for: what comes back is the events' handler's) */
		header->token = token;
		error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
		apr_out += error == 0;
		return error == 0;
	}
	lck_mtx_lock(&apr_lock);
	if (token == 0) {
		token = apr_next_token++;
		if (apr_next_token >= 0x01000000u) {
			apr_next_token = 1;
		}
	}
	header->token = token;
	apr_answer.there = false;
	apr_waited_for = token;
	error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
	if (error != 0) {
		printf("adsp: apr: > opcode %#x to service %u: NOT SENT (%d)\n", opcode, service, error);
	} else {
		apr_out++;
		for (uint32_t waited = 0; waited < ms && !answered; waited++) {
			if (apr_answer.there && apr_answer.token == token) {
				answered = true;
				if (answer != NULL) {
					answer->opcode = apr_answer.opcode;
					answer->token = apr_answer.token;
					answer->length = apr_answer.length;
					memcpy(answer->words, apr_answer.words, sizeof(answer->words));
				}
				break;
			}
			sleep_ms(1);
		}
	}
	apr_waited_for = 0;
	lck_mtx_unlock(&apr_lock);
	return answered;
}

void
hakios_adsp_apr_events(hakios_apr_events_t handler)
{
	apr_events = handler;
}

void
hakios_adsp_apr_events_second(hakios_apr_events_t handler)
{
	apr_events_second = handler;
}

/* A command to one of the DSP's services, and its answer waited for: true, and the answer in apr_answer, if it came. */
static bool
apr_ask(uint8_t service, uint32_t opcode, const void *payload, uint32_t length, uint32_t ms)
{
	return hakios_adsp_apr(service, 0, 0, opcode, payload, length, ms, NULL);
}

/* ---- the battery manager, on the channel PMIC_RTR_ADSP_APPS ------------------------------------------ */

/*
 * On this SoC the audio DSP also runs the battery's and the charger's management -- the fuel gauge,
 * the charging -- and the operating system only asks it how things are: Android's
 * qti_battery_charger.c, Linux's qcom_battmgr.c over pmic_glink.c, on the channel PMIC_RTR_ADSP_APPS
 * that the DSP opens. A message is a header (owner 32778, the battery manager; type 1, request and
 * answer, or 2, a notice; an opcode) and its words; a property is asked for with opcode 0x30 (the
 * battery's) or 0x32 (the USB port's), the battery's number (0), the property's number and a word
 * that a get does not use, and is answered with the property's number, its value and a result (0:
 * good). Only gets are sent from here: nothing is set, no limit, no current, no notice asked for.
 * With adsp=1 (battmgr=0: not) it is asked every BATTMGR_SECONDS while the DSP runs, and what it says
 * goes to bsd/dev/hakios.battery.c (hakios_battery_report()), and from there to the power source
 * that iOS reads (iokit/Kernel/HKBattery.cpp). battmgr=2: every property once, for the log.
 * Units: capacity in hundredths of a percent, voltage in uV, current in uA (more than 0 while the
 * battery takes charge), the temperature in hundredths of a degree, charges in uAh.
 * The properties' numbers are this phone's firmware's, read with battmgr=2 and checked against what
 * Android shows at the same moment (dumpsys battery, /sys/class/power_supply/battery): up to 9 they
 * are those of Linux's qcom_battmgr.c (sm8350), from there on one more property comes before the
 * temperature, so the temperature is 13, not 12, and the charge counter, the cycles, the designed and
 * the learnt full charge are 15 to 18.
 */
#define BATTMGR_OWNER           32778
#define BATTMGR_REQUEST         1
#define BATTMGR_NOTICE          2
#define BATTMGR_BATTERY_GET     0x30
#define BATTMGR_USB_GET         0x32
#define BATT_STATUS             0               /* Linux's POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
#define BATT_HEALTH             1
#define BATT_PRESENT            2
#define BATT_CAPACITY           4
#define BATT_VOLT_NOW           7
#define BATT_CURR_NOW           9
#define BATT_TEMP               13              /* hundredths of a degree: 3900 when Android says 39.0 */
#define BATT_CHG_COUNTER        15              /* the charge in the battery now, uAh */
#define BATT_CYCLE_COUNT        16
#define BATT_CHG_FULL_DESIGN    17              /* 5000000 uAh */
#define BATT_CHG_FULL           18              /* the full charge as the fuel gauge has learnt it, uAh */
#define USB_VOLT_NOW            1               /* uV */
#define USB_CURR_NOW            3               /* uA, what the port gives */
#define USB_TYPE                6               /* Linux's POWER_SUPPLY_USB_TYPE_*: 1 here, a computer's port */
#define USB_ONLINE              0
#define BATTMGR_SECONDS         10
/*
 * What Android's qti_battery_charger.c does besides asking (Xiaomi's kernel for this phone, GPL): it asks the
 * DSP for its notices once the channel is up (BC_SET_NOTIFY_REQ, a notice-type message of four zero words),
 * and it reads Xiaomi's own properties of the charger with opcode 0x50 (BC_XM_STATUS_GET), numbered as that
 * kernel's enum xm_property_id with this phone's configuration (CONFIG_REDWOOD_FOR_BUILD, CONFIG_BQ2597X,
 * CONFIG_BQ_CLOUD_AUTHENTICATION) leaves them. battmgr=3 asks for every one of the 115 once, for the log, to
 * hold the numbers against what Android shows in /sys/class/qcom-battery. Nothing is set from here.
 */
#define BATTMGR_SET_NOTIFY      0x04
#define BATTMGR_XM_GET          0x50
#define BATTMGR_XM_PROPS        115
#define USB_CURR_MAX            4               /* uA */
#define USB_INPUT_CURR_LIMIT    5               /* uA */
#define USB_REAL_TYPE           11
#define XM_COOL_MODE            20
#define XM_INPUT_SUSPEND        45
#define XM_REAL_TYPE            46
#define XM_FASTCHGMODE          63
#define XM_THERMAL_REMOVE       65
#define XM_NIGHT_CHARGING       77
#define XM_SMART_BATT           78
#define XM_FG1_SOH              82
#define XM_FG1_TAMBIENT         89

struct battmgr_message {
	uint32_t owner, type, opcode;
	uint32_t words[3];                      /* asked: battery, property, value; answered: property, value, result */
};

static struct {
	volatile uint32_t answers, notices;
	uint32_t opcode, property, value, result, length;
	bool wanted;
	volatile bool runs;
} battmgr;

/* the charger as the DSP and Xiaomi's properties describe it, for kern.hakios.charger */
static char battmgr_charger[768] = "charger: not asked yet (adsp=1, and the DSP's battery manager up)\n";

static int
battmgr_charger_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	static char copy[sizeof(battmgr_charger)];

	memcpy(copy, battmgr_charger, sizeof(copy));
	copy[sizeof(copy) - 1] = 0;
	return SYSCTL_OUT(req, copy, strlen(copy) + 1);
}
#ifndef HAKIOS_SM7150_MODEM_COPY
SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, charger, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    battmgr_charger_sysctl, "A", "the charger, as the audio DSP's battery manager and Xiaomi's properties say");
#endif

static void
battmgr_input(const uint8_t *data, uint32_t length)
{
	struct battmgr_message m;

	bzero(&m, sizeof(m));
	memcpy(&m, data, length < sizeof(m) ? length : sizeof(m));
	if (m.owner != BATTMGR_OWNER || length < 12) {
		printf("adsp: battmgr: < %u bytes of owner %u, type %u, opcode %#x: not the battery manager's\n", length, m.owner,
		    m.type, m.opcode);
		return;
	}
	if (m.type == BATTMGR_NOTICE) {
		if (battmgr.notices++ < 8) {
			tell("adsp: battmgr: < a notice, opcode %#x, %u bytes (%#x %#x %#x)\n", m.opcode, length, m.words[0], m.words[1],
			    m.words[2]);
		}
		return;
	}
	battmgr.opcode = m.opcode;
	battmgr.property = m.words[0];
	battmgr.value = m.words[1];
	battmgr.result = m.words[2];
	battmgr.length = length;
	os_atomic_inc(&battmgr.answers, release);
}

/* One property asked for and its answer waited for (a second at most); one thread asks at a time (battmgr_thread). */
static bool
battmgr_get(uint32_t opcode, uint32_t property, uint32_t *value)
{
	struct battmgr_message q = { BATTMGR_OWNER, BATTMGR_REQUEST, opcode, { 0, property, 0 } };
	uint32_t before = os_atomic_load(&battmgr.answers, acquire);

	if (glink_send(&glink.channel[CHANNEL_PMIC], &q, sizeof(q)) != 0) {
		return false;
	}
	for (unsigned ms = 0; ms < 1000; ms++) {
		if (os_atomic_load(&battmgr.answers, acquire) != before) {
			if (battmgr.opcode != opcode || battmgr.property != property || battmgr.length < sizeof(struct battmgr_message)) {
				before = battmgr.answers;       /* (somebody else's answer: wait on) */
				continue;
			}
			if (battmgr.result != 0) {
				return false;
			}
			*value = battmgr.value;
			return true;
		}
		sleep_ms(1);
	}
	return false;
}

/* the DSP's notices asked for, as Android does once its battery manager's channel is up */
static void
battmgr_notify_enable(void)
{
	struct {
		uint32_t owner, type, opcode;
		uint32_t battery, power_state, low_capacity, high_capacity;
	} q = { BATTMGR_OWNER, BATTMGR_NOTICE, BATTMGR_SET_NOTIFY, 0, 0, 0, 0 };
	uint32_t on = 1;

	PE_parse_boot_argn("battmgr_notify", &on, sizeof(on));
	if (PE_parse_boot_argn("battmgr_xm", &on, sizeof(on)) && on == 0) {
		return;
	}
	if (on == 0) {
		return;
	}
	tell("adsp: battmgr: the DSP's notices asked for (BC_SET_NOTIFY_REQ, as Android does): %s\n",
	    glink_send(&glink.channel[CHANNEL_PMIC], &q, sizeof(q)) == 0 ? "sent" : "NOT SENT");
}

/* the charger's state, Xiaomi's properties among it, into kern.hakios.charger (and the log when it changes) */
static void
battmgr_charger_look(uint32_t usb)
{
	uint32_t xm = 1;

	/* battmgr_xm=0: the battery alone, as before the charger's own story was asked for */
	if (PE_parse_boot_argn("battmgr_xm", &xm, sizeof(xm)) && xm == 0) {
		return;
	}
	static const struct { uint32_t opcode, property; const char *name; } asked[] = {
		{ BATTMGR_USB_GET, USB_CURR_MAX, "usb_current_max_ua" },
		{ BATTMGR_USB_GET, USB_INPUT_CURR_LIMIT, "usb_input_limit_ua" },
		{ BATTMGR_USB_GET, USB_REAL_TYPE, "usb_real_type" },
		{ BATTMGR_XM_GET, XM_INPUT_SUSPEND, "input_suspend" },
		{ BATTMGR_XM_GET, XM_REAL_TYPE, "xm_real_type" },
		{ BATTMGR_XM_GET, XM_FASTCHGMODE, "fastchg_mode" },
		{ BATTMGR_XM_GET, XM_THERMAL_REMOVE, "thermal_remove" },
		{ BATTMGR_XM_GET, XM_COOL_MODE, "cool_mode" },
		{ BATTMGR_XM_GET, XM_NIGHT_CHARGING, "night_charging" },
		{ BATTMGR_XM_GET, XM_SMART_BATT, "smart_batt" },
		{ BATTMGR_XM_GET, XM_FG1_SOH, "soh_percent" },
		{ BATTMGR_XM_GET, XM_FG1_TAMBIENT, "fg_ambient" },
	};
	static char text[sizeof(battmgr_charger)];
	size_t n = 0;

	n += snprintf(text + n, sizeof(text) - n, "usb_online %u\n", usb);
	for (unsigned i = 0; i < sizeof(asked) / sizeof(asked[0]) && n < sizeof(text); i++) {
		uint32_t v = 0;

		if (battmgr_get(asked[i].opcode, asked[i].property, &v)) {
			n += snprintf(text + n, sizeof(text) - n, "%s %d\n", asked[i].name, (int)v);
		} else {
			n += snprintf(text + n, sizeof(text) - n, "%s -\n", asked[i].name);
		}
	}
	if (strcmp(text, battmgr_charger) != 0) {
		memcpy(battmgr_charger, text, sizeof(battmgr_charger));
		battmgr_charger[sizeof(battmgr_charger) - 1] = 0;
		tell("adsp: battmgr: the charger now:\n%s", battmgr_charger);
	}
}

static void
battmgr_thread(__unused void *parameter, __unused wait_result_t result)
{
	battmgr.runs = true;
	for (unsigned ms = 0; ms < 15000 && battmgr.wanted && !glink.channel[CHANNEL_PMIC].open; ms += 10) {
		sleep_ms(10);
	}
	if (!glink.channel[CHANNEL_PMIC].open) {
		tell("adsp: battmgr: the DSP has not opened PMIC_RTR_ADSP_APPS: nothing is asked\n");
		battmgr.runs = false;
		thread_terminate(current_thread());
	}
	{
		/* boot-arg battmgr=2: every property once, for the log (which number is which on this phone's firmware) */
		uint32_t mode = 1;

		PE_parse_boot_argn("battmgr", &mode, sizeof(mode));
		battmgr_notify_enable();
		/* battmgr=3: every one of Xiaomi's properties once (opcode 0x50) */
		for (uint32_t p = 0; mode == 3 && p < BATTMGR_XM_PROPS; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_XM_GET, p, &v);

			tell("adsp: battmgr: xiaomi property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
		for (uint32_t p = 0; mode == 2 && p < 32; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_BATTERY_GET, p, &v);

			tell("adsp: battmgr: battery property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
		for (uint32_t p = 0; mode == 2 && p < 12; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_USB_GET, p, &v);

			tell("adsp: battmgr: USB property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
	}
	for (unsigned round = 0; battmgr.wanted && glink.up && glink.channel[CHANNEL_PMIC].open; round++) {
		struct hakios_battery_state b = { .size = sizeof(b) };
		uint32_t capacity = 0, status = 0, present = 0, volt = 0, curr = 0, temp = 0, usb = 0;
		uint32_t usb_type = 0, usb_volt = 0, usb_curr = 0, charge = 0;
		static uint32_t full, design, cycles;
		bool good = battmgr_get(BATTMGR_BATTERY_GET, BATT_CAPACITY, &capacity) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_STATUS, &status) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_PRESENT, &present) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_VOLT_NOW, &volt) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_CURR_NOW, &curr) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_TEMP, &temp) &&
		    battmgr_get(BATTMGR_USB_GET, USB_ONLINE, &usb);

		if (good) {
			/* (what may be missing without harm, and the capacities and the cycles only now and then: they hardly change) */
			(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_COUNTER, &charge);
			if (usb) {
				(void)battmgr_get(BATTMGR_USB_GET, USB_TYPE, &usb_type);
				(void)battmgr_get(BATTMGR_USB_GET, USB_VOLT_NOW, &usb_volt);
				(void)battmgr_get(BATTMGR_USB_GET, USB_CURR_NOW, &usb_curr);
			}
			if (round % 30 == 0 || full == 0) {
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_FULL, &full);
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_FULL_DESIGN, &design);
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CYCLE_COUNT, &cycles);
			}
			b.percent_hundredths = (int32_t)capacity;
			b.voltage_uv = (int32_t)volt;
			b.current_ua = (int32_t)curr;
			b.temperature_hundredths = (int32_t)temp;
			b.present = present != 0;
			b.external = usb != 0;
			b.status = (int32_t)status;
			b.charge_uah = (int32_t)charge;
			b.full_uah = (int32_t)full;
			b.design_uah = (int32_t)design;
			b.cycles = (int32_t)cycles;
			b.usb_type = (int32_t)usb_type;
			b.usb_voltage_uv = (int32_t)usb_volt;
			b.usb_current_ua = (int32_t)usb_curr;
			hakios_battery_report(&b);
			/* the charger's own story: at the start, whenever the cable comes or goes, and every minute */
			{
				static uint32_t usb_before = 0xffffffff;

				if (round % 6 == 0 || usb != usb_before) {
					battmgr_charger_look(usb);
					usb_before = usb;
				}
			}
		}
		if (round < 3 || round % 6 == 0 || !good) {
			tell("adsp: battmgr: %s capacity %u (hundredths of a percent), status %u, present %u, %d uV, %d uA, temperature %d "
			    "(hundredths of a degree), charge %u uAh, USB online %u (type %u, %u uV, %u uA)\n",
			    good ? "the DSP says:" : "NO ANSWER to all of", capacity, status, present, (int)volt, (int)curr, (int)temp,
			    charge, usb, usb_type, usb_volt, usb_curr);
			if (round % 30 == 0 && good) {
				tell("adsp: battmgr: full charge %u uAh, as designed %u uAh, %u cycles\n", full, design, cycles);
			}
		}
		for (unsigned ms = 0; ms < BATTMGR_SECONDS * 1000 && battmgr.wanted && glink.up; ms += 50) {
			sleep_ms(50);
		}
	}
	battmgr.runs = false;
	thread_terminate(current_thread());
}

static void
battmgr_start(void)
{
	thread_t thread;

	uint32_t wanted = 1;            /* (on with adsp=1 unless battmgr=0) */

	PE_parse_boot_argn("battmgr", &wanted, sizeof(wanted));
	if (wanted == 0 || battmgr.runs) {
		return;
	}
	if (option("battery")) {
		/* (the loader's way, through Qualcomm's UEFI charger drivers: one way to the battery at a time) */
		tell("adsp: battmgr: not with battery=1: the battery is asked about the loader's way\n");
		return;
	}
	battmgr.wanted = true;
	if (kernel_thread_start(battmgr_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static void
battmgr_stop(void)
{
	battmgr.wanted = false;
	for (unsigned ms = 0; ms < 2000 && battmgr.runs; ms++) {
		sleep_ms(1);
	}
}

/* ---- FastRPC: the sensors' process on the DSP ------------------------------------------------------- */

/*
 * The DSP's sensors process (its static "sensors PD") gives the whole DSP up some 36 s after the start
 * unless somebody on our side attaches to it, as Android's sensors daemon does and Linux's
 * hexagonrpcd -s (FASTRPC_IOCTL_INIT_ATTACH_SNS): a FastRPC call of handle 1 (the init handle),
 * method 0 (attach), with one input: our client's number. The call is Linux's drivers/misc/fastrpc.c:
 * a message of 40 bytes on the channel "fastrpcglink-apps-dsp", pointing at a page that holds the
 * arguments' descriptions and the arguments; the answer is the message's context and a result.
 * The page is in memory the DSP sees through the SMMU (hakios_audio_dsp_memory_fastrpc(), hakios.sound.c).
 * Boot-arg fastrpc=0: not attached.
 */

#define FASTRPC_INIT_HANDLE     1
#define FASTRPC_ATTACH          0
#define FASTRPC_ROOT_PD         0       /* (Linux's fastrpc.c: ROOT_PD, SENSORS_PD: the low bits of a call's context) */
#define FASTRPC_SENSORS_PD      2
#define FASTRPC_SCALARS(method, in, out) ((uint32_t)(method) << 24 | (uint32_t)(in) << 16 | (uint32_t)(out) << 8)
#define FASTRPC_META(n)         ((16 + 8 + 16) * (n) + 8 * 16 + 4 * 64)  /* the arguments, their lists and pages, fds, crcs */
#define FASTRPC_ALIGN           128

struct fastrpc_msg {
	int32_t client_id, tid;
	uint64_t ctx;
	uint32_t handle, sc;
	uint64_t addr, size;
};

static struct {
	bool wanted;
	uint32_t answers;
} fastrpc;

/*
 * Two sessions, as hexagonrpcd runs two (rootpd and sensorspd): the root PD's first, and the sensors'
 * process only once the root PD's listener answers. With the sensors' process alone the DSP gives up
 * 36 s after its start (sns_sig_remote_; on mainline SM7150, sm7150-mainline/linux#65: "the sensor_process
 * watchdog crashes the ADSP at about 52 s" unless sensorspd starts after rootpd). Each has its own client
 * number, its own half of the memory the DSP sees, and its own answer; the files' state is shared.
 */
struct fastrpc_session {
	const char *name;
	uint32_t pd;                    /* the low bits of each of its calls' contexts */
	int32_t client;                 /* our "process" in its attach (Linux: the caller's tgid) */
	uintptr_t memory;               /* its half of FASTRPC_BYTES (hakios.sound.c) */
	uint64_t dsp;                   /* ... at this address of the DSP's (FastRPC's stream in bits 32 and up) */
	uint32_t bytes, context;
	volatile bool answered, runs, listening;
	int32_t result;
	uint64_t answered_ctx;
};
static struct fastrpc_session fastrpc_root = { .name = "the root PD", .pd = FASTRPC_ROOT_PD, .client = 1 },
    fastrpc_sensors = { .name = "the sensors' process", .pd = FASTRPC_SENSORS_PD, .client = 2 };
static LCK_MTX_DECLARE(fastrpc_files_lock, &adsp_lock_group);  /* apps_std's open files, for both listeners */

static void
fastrpc_input(const uint8_t *data, uint32_t length)
{
	char line[200];
	int at = 0;

	for (uint32_t i = 0; i < length && i < 48 && at < (int)sizeof(line) - 4; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, "%02x", data[i]);
	}
	fastrpc.answers++;
	if (length >= 12) {
		uint64_t ctx;
		struct fastrpc_session *session;

		bcopy(data, &ctx, 8);
		session = (ctx & 0xf) == FASTRPC_SENSORS_PD ? &fastrpc_sensors : &fastrpc_root;
		session->answered_ctx = ctx;
		bcopy(data + 8, &session->result, 4);
		session->answered = true;
	}
	tell("adsp: fastrpc: < %u bytes: %s\n", length, line);
}

struct fastrpc_buf {
	void *p;
	uint32_t length;
};

#define FASTRPC_REMOTECTL       0       /* the DSP's handles: its module loader ... */
#define FASTRPC_LISTENER        3       /* ... and the listener (hexagonrpcd's adsp_listener.h) */
#define FASTRPC_UNSUPPORTED     20      /* AEE_EUNSUPPORTED */
#define FASTRPC_FAILED          1       /* AEE_EFAILED */

/*
 * One call to the sensors' process: inputs and outputs of ours laid out as Linux's fastrpc_get_args()
 * does (the arguments, their lists of pages and the pages first; each buffer then on 128 bytes), the
 * outputs copied back. The DSP's result, or -1 (not sent, or no answer while we wait: wait_ms 0 waits
 * as long as FastRPC is wanted).
 */
static int32_t
fastrpc_call(struct fastrpc_session *session, uint32_t handle, uint32_t method, unsigned n_in, const struct fastrpc_buf *in,
    unsigned n_out, struct fastrpc_buf *out, uint32_t wait_ms)
{
	const uintptr_t fastrpc_memory = session->memory;
	const uint64_t fastrpc_dsp = session->dsp;
	const uint32_t fastrpc_bytes = session->bytes;
	unsigned n = n_in + n_out;
	struct {
		uint64_t pv, len;
	} *args = (void *)fastrpc_memory;
	struct {
		uint32_t num, pgidx;
	} *list = (void *)(fastrpc_memory + 16 * n);
	struct {
		uint64_t addr, size;
	} *pages = (void *)(fastrpc_memory + 24 * n);
	uint32_t at = FASTRPC_META(n), place[16];
	struct fastrpc_msg msg;

	if (n > 16) {
		return -1;
	}
	bzero((void *)fastrpc_memory, fastrpc_bytes);
	for (unsigned i = 0; i < n; i++) {
		uint32_t length = i < n_in ? in[i].length : out[i - n_in].length;
		uint64_t first, last;

		at = (at + FASTRPC_ALIGN - 1) & ~(FASTRPC_ALIGN - 1);
		if (at + length > fastrpc_bytes) {
			tell("adsp: fastrpc: a call of %u buffers does not fit our %u bytes\n", n, fastrpc_bytes);
			return -1;
		}
		place[i] = at;
		first = (fastrpc_dsp + at) & ~0xfffull;
		last = (fastrpc_dsp + at + (length ? length - 1 : 0)) & ~0xfffull;
		args[i].pv = fastrpc_dsp + at;
		args[i].len = length;
		list[i].num = length ? 1 : 0;
		list[i].pgidx = i;
		pages[i].addr = first;
		pages[i].size = last - first + 4096;
		if (i < n_in && length) {
			bcopy(in[i].p, (void *)(fastrpc_memory + at), length);
		}
		at += length;
	}
	for (vm_offset_t line = fastrpc_memory; line < fastrpc_memory + at; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));    /* the DSP reads memory, not our caches */
	}
	__asm__ volatile ("dsb sy");
	session->context = session->context % 255 + 1;
	msg.client_id = session->client;
	msg.tid = session->client;
	msg.ctx = (uint64_t)session->context << 4 | session->pd;
	msg.handle = handle;
	msg.sc = method > 30 ? FASTRPC_SCALARS(31, n_in, n_out) : FASTRPC_SCALARS(method, n_in, n_out);
	msg.addr = fastrpc_dsp;
	msg.size = (at + 4095) & ~4095u;
	session->answered = false;
	if (glink_send(&glink.channel[CHANNEL_FASTRPC], &msg, sizeof(msg)) != 0) {
		tell("adsp: fastrpc: the call could not be sent\n");
		return -1;
	}
	for (unsigned ms = 0; (wait_ms == 0 ? fastrpc.wanted : ms < wait_ms) && !session->answered; ms++) {
		sleep_ms(1);
		if (wait_ms == 0 && ms % 5000 == 4999) {
			tell("adsp: fastrpc: %s: still waiting for the DSP's next call (%u s, context %#llx)\n", session->name,
			    (ms + 1) / 1000, msg.ctx);
		}
	}
	if (!session->answered) {
		if (wait_ms) {
			tell("adsp: fastrpc: %s: NO ANSWER in %u ms to handle %u method %u\n", session->name, wait_ms, handle, method);
		}
		return -1;
	}
	if (session->answered_ctx != msg.ctx) {
		tell("adsp: fastrpc: %s: the answer is for context %#llx, not ours (%#llx)\n", session->name, session->answered_ctx,
		    msg.ctx);
	}
	for (vm_offset_t line = fastrpc_memory; line < fastrpc_memory + at; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));    /* (what the DSP has written, not what our caches had) */
	}
	__asm__ volatile ("dsb sy");
	for (unsigned i = 0; i < n_out; i++) {
		if (out[i].length) {
			bcopy((void *)(fastrpc_memory + place[n_in + i]), out[i].p, out[i].length);
		}
	}
	return session->result;
}

/*
 * The DSP's calls to us come as the answers of adsp_listener's next2 (hexagonrpcd's listener.c): its
 * handle, scalars and input buffers, each buffer a 4-byte size, then (on 8 bytes) the bytes; our
 * answer goes with the next next2, the output buffers encoded the same way. Our handles, as
 * hexagonrpcd's: 0 remotectl (open and close of our modules), 1 apps_std (the files), 2 apps_mem.
 */
#define LISTENER_INBUFS         2048
#define LISTENER_OUTBUFS        (64 * 1024)     /* (a read's bytes go back in it) */

struct listener_call {
	uint32_t handle, sc, method, n_in;
	const uint8_t *in[16];
	uint32_t in_length[16];
};

static bool
listener_decode(const uint8_t *data, uint32_t length, struct listener_call *call)
{
	uint32_t at = 0;

	call->n_in = call->sc >> 16 & 0xff;
	if (call->n_in > 16) {
		return false;
	}
	for (unsigned i = 0; i < call->n_in; i++) {
		uint32_t size;

		if (at + 4 > length) {
			return false;
		}
		bcopy(data + at, &size, 4);
		at += 4;
		if (size && (at & 7)) {
			at = (at + 7) & ~7u;
		}
		if (at + size > length) {
			return false;
		}
		call->in[i] = data + at;
		call->in_length[i] = size;
		at += size;
	}
	call->method = call->sc >> 24 & 0x1f;
	if (call->method == 31 && call->n_in && call->in_length[0] >= 4) {
		bcopy(call->in[0], &call->method, 4);
	}
	return true;
}

/* an output buffer of our answer: its size, then (on 8 bytes) its bytes */
static uint32_t
listener_encode(uint8_t *to, uint32_t at, const void *data, uint32_t size)
{
	if (at + 16 + size > LISTENER_OUTBUFS) {
		return at;
	}
	bcopy(&size, to + at, 4);
	at += 4;
	if (size) {
		if (at & 7) {
			bzero(to + at, 8 - (at & 7));
			at = (at + 7) & ~7u;
		}
		if (data != NULL) {
			bcopy(data, to + at, size);
		} else {
			bzero(to + at, size);
		}
		at += size;
	}
	return at;
}

static const char *const apps_std_names[32] = {
	[1] = "freopen", [2] = "fflush", [3] = "fclose", [4] = "fread", [9] = "fseek", [19] = "fopen_with_env",
	[26] = "opendir", [27] = "closedir", [28] = "readdir", [29] = "mkdir", [31] = "stat",
};

/*
 * apps_std: the files the DSP's processes read, as hexagonrpcd serves them (its apps_std.c and
 * rpcd_builder.c): the phone's own, read-only, from /fw/hexagonfs on the root (userland/build.sh puts
 * fw-sensors/ there), with /fw/hexagonfs.idx listing every entry ("d|f size /path" a line). The
 * sensors' process asks for /vendor/etc/sensors/sns_reg_config, the sensors' .json files in
 * /vendor/etc/sensors/config, the registry it made before in /mnt/vendor/persist/sensors/registry,
 * the SoC's /sys/devices/soc0, and its libraries by ADSP_LIBRARY_PATH (/usr/lib/qcom/adsp).
 */
#define HFS_FDS                 32

struct hfs_entry {
	char path[120];
	uint32_t size;
	bool dir;
};

static struct hfs_entry *hfs;
static uint32_t hfs_count;
static bool hfs_tried;
static struct {
	bool used, dir;
	uint32_t entry, at;             /* at: the place in the file, or the next entry to look at in the directory */
	const uint8_t *data;
	uint64_t size;
} hfs_fd[HFS_FDS];

static void
hfs_load(void)
{
	const uint8_t *index;
	uint64_t size;
	uint32_t lines = 0;

	if (hfs_tried) {
		return;
	}
	hfs_tried = true;
	index = root_file("hexagonfs.idx", &size);
	if (index == NULL) {
		tell("adsp: fastrpc: no /fw/hexagonfs.idx: no file is served to the DSP\n");
		return;
	}
	for (uint64_t i = 0; i < size; i++) {
		lines += index[i] == '\n';
	}
	hfs = kalloc_data(sizeof(*hfs) * (lines + 1), Z_WAITOK | Z_ZERO);
	for (uint64_t i = 0; hfs != NULL && i < size;) {
		uint64_t end = i;
		struct hfs_entry *e = &hfs[hfs_count];
		uint32_t n = 0;

		while (end < size && index[end] != '\n') {
			end++;
		}
		if (end - i > 4 && (index[i] == 'd' || index[i] == 'f') && index[i + 1] == ' ') {
			uint64_t j = i + 2;

			e->dir = index[i] == 'd';
			while (j < end && index[j] >= '0' && index[j] <= '9') {
				n = n * 10 + (uint32_t)(index[j++] - '0');
			}
			e->size = n;
			if (j < end && index[j] == ' ' && end - j - 1 < sizeof(e->path)) {
				bcopy(index + j + 1, e->path, (size_t)(end - j - 1));
				hfs_count++;
			}
		}
		i = end + 1;
	}
	hakios_adsp_fw_free(index, size);
	tell("adsp: fastrpc: %u files and directories to serve the DSP (/fw/hexagonfs)\n", hfs_count);
}

/* name (relative ones in base), made a path of the index's: "//" and "/./" taken out, no "/" at the end */
static int
hfs_find(const char *base, const char *name, uint32_t name_length)
{
	char path[256], clean[256];
	uint32_t c = 0;

	if (name_length == 0 || name_length > 200) {
		return -1;
	}
	snprintf(path, sizeof(path), "%s%.*s", name[0] == '/' ? "" : base, (int)name_length, name);
	for (uint32_t i = 0; path[i] != 0 && c < sizeof(clean) - 1; i++) {
		if (path[i] == '/' && (path[i + 1] == '/' || (path[i + 1] == '.' && (path[i + 2] == '/' || path[i + 2] == 0)))) {
			if (path[i + 1] == '.') {
				i++;
			}
			continue;
		}
		clean[c++] = path[i];
	}
	while (c > 1 && clean[c - 1] == '/') {
		c--;
	}
	clean[c] = 0;
	if (strcmp(clean, "/") == 0 || c == 0) {
		return (int)hfs_count;          /* (the root: one past the entries) */
	}
	for (uint32_t i = 0; i < hfs_count; i++) {
		if (strcmp(hfs[i].path, clean) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static int
hfs_open(int entry)
{
	for (int fd = 0; fd < HFS_FDS; fd++) {
		if (!hfs_fd[fd].used) {
			bzero(&hfs_fd[fd], sizeof(hfs_fd[fd]));
			hfs_fd[fd].used = true;
			hfs_fd[fd].entry = (uint32_t)entry;
			hfs_fd[fd].dir = entry == (int)hfs_count || hfs[entry].dir;
			if (!hfs_fd[fd].dir && hfs[entry].size) {
				char name[140];

				snprintf(name, sizeof(name), "hexagonfs%s", hfs[entry].path);
				hfs_fd[fd].data = root_file(name, &hfs_fd[fd].size);
				if (hfs_fd[fd].data == NULL) {
					hfs_fd[fd].used = false;
					return -1;
				}
			}
			return fd;
		}
	}
	return -1;
}

static void
hfs_close(uint32_t fd)
{
	if (fd < HFS_FDS && hfs_fd[fd].used) {
		if (hfs_fd[fd].data != NULL) {
			hakios_adsp_fw_free(hfs_fd[fd].data, hfs_fd[fd].size);
		}
		bzero(&hfs_fd[fd], sizeof(hfs_fd[fd]));
	}
}

static bool
hfs_name_ok(const struct listener_call *call, unsigned i)
{
	return call->n_in > i && call->in_length[i] > 0 && call->in[i][call->in_length[i] - 1] == 0;
}

static uint32_t apps_std_calls[32], apps_std_refused[32];

static uint32_t
apps_std(const struct listener_call *call, uint8_t *out, uint32_t *out_length)
{
	const uint8_t *in = call->in[0];
	uint32_t prim = call->n_in ? call->in_length[0] : 0, at = 0, fd;
	uint32_t method = call->method < 32 ? call->method : 0;
	const char *name = apps_std_names[method] ? apps_std_names[method] : "?";
	int entry;

	hfs_load();
	apps_std_calls[method]++;
	switch (call->method) {
	case 19: {      /* fopen_with_env: the variable, its delimiter, the name, the mode; out the descriptor */
		const char *base = "/";
		int opened;

		if (!hfs_name_ok(call, 1) || !hfs_name_ok(call, 3) || !hfs_name_ok(call, 4)) {
			return 14;
		}
		if (strcmp((const char *)call->in[1], "ADSP_LIBRARY_PATH") == 0) {
			base = "/usr/lib/qcom/adsp/";
		} else if (strcmp((const char *)call->in[1], "ADSP_AVS_CFG_PATH") == 0) {
			base = "/vendor/etc/acdbdata/";
		}
		if (call->in[4][0] == 'w' || call->in[4][0] == 'a') {
			if (apps_std_refused[method]++ < 40) {
				tell("adsp: fastrpc: apps_std fopen \"%s\" (\"%s\") for writing: refused (read-only)\n",
				    (const char *)call->in[3], (const char *)call->in[4]);
			}
			return 20;
		}
		entry = hfs_find(base, (const char *)call->in[3], call->in_length[3] - 1);
		opened = entry < 0 || entry == (int)hfs_count || hfs[entry].dir ? -1 : hfs_open(entry);
		if (apps_std_calls[method] <= 120) {
			tell("adsp: fastrpc: apps_std fopen $%s \"%s\": %s\n", (const char *)call->in[1], (const char *)call->in[3],
			    opened >= 0 ? "opened" : entry < 0 ? "NOT THERE" : "not opened");
		}
		if (opened < 0) {
			return 1;
		}
		fd = (uint32_t)opened;
		*out_length = listener_encode(out, 0, &fd, 4);
		return 0;
	}
	case 4: {       /* fread: in {fd, room}; out {read, at the end}, the bytes */
		uint32_t room, words[2] = { 0, 0 }, left;

		if (prim < 8) {
			return 14;
		}
		bcopy(in, &fd, 4);
		bcopy(in + 4, &room, 4);
		if (fd >= HFS_FDS || !hfs_fd[fd].used || hfs_fd[fd].dir || room + 32 > LISTENER_OUTBUFS) {
			if (room + 32 > LISTENER_OUTBUFS) {
				tell("adsp: fastrpc: apps_std fread of %u bytes: more than we can give back at once\n", room);
			}
			return 1;
		}
		left = hfs_fd[fd].size > hfs_fd[fd].at ? (uint32_t)(hfs_fd[fd].size - hfs_fd[fd].at) : 0;
		words[0] = min(left, room);
		words[1] = words[0] < room;
		at = listener_encode(out, at, words, 8);
		at = listener_encode(out, at, NULL, room);
		if (words[0]) {
			bcopy(hfs_fd[fd].data + hfs_fd[fd].at, out + at - room, words[0]);
		}
		hfs_fd[fd].at += words[0];
		*out_length = at;
		return 0;
	}
	case 9: {       /* fseek: fd, offset, whence (0 set, 1 current, 2 end) */
		int32_t offset;
		uint32_t whence;
		int64_t to;

		if (prim < 12) {
			return 14;
		}
		bcopy(in, &fd, 4);
		bcopy(in + 4, &offset, 4);
		bcopy(in + 8, &whence, 4);
		if (fd >= HFS_FDS || !hfs_fd[fd].used || hfs_fd[fd].dir || whence > 2) {
			return 1;
		}
		to = (whence == 0 ? 0 : whence == 1 ? (int64_t)hfs_fd[fd].at : (int64_t)hfs_fd[fd].size) + offset;
		if (to < 0) {
			return 1;
		}
		hfs_fd[fd].at = (uint32_t)to;
		return 0;
	}
	case 3:         /* fclose: fd */
	case 2:         /* fflush: fd */
		if (prim < 4) {
			return 14;
		}
		bcopy(in, &fd, 4);
		if (call->method == 3) {
			hfs_close(fd);
		}
		return 0;
	case 26: {      /* opendir: the name; out the directory (8 bytes) */
		uint64_t dir;
		int opened;

		if (!hfs_name_ok(call, 1)) {
			return 14;
		}
		entry = hfs_find("/", (const char *)call->in[1], call->in_length[1] - 1);
		opened = entry < 0 || (entry != (int)hfs_count && !hfs[entry].dir) ? -1 : hfs_open(entry);
		tell("adsp: fastrpc: apps_std opendir \"%s\": %s\n", (const char *)call->in[1], opened >= 0 ? "opened" : "NOT THERE");
		if (opened < 0) {
			return 1;
		}
		dir = (uint64_t)opened;
		*out_length = listener_encode(out, 0, &dir, 8);
		return 0;
	}
	case 27: {      /* closedir: the directory (8 bytes) */
		uint64_t dir;

		if (prim < 8) {
			return 14;
		}
		bcopy(in, &dir, 8);
		hfs_close((uint32_t)dir);
		return 0;
	}
	case 28: {      /* readdir: the directory; out {inode, name[256], at the end} */
		struct {
			uint32_t inode;
			char name[256];
			uint32_t end;
		} next;
		uint64_t dir;
		const char *of;
		size_t of_length;

		if (prim < 8) {
			return 14;
		}
		bcopy(in, &dir, 8);
		if (dir >= HFS_FDS || !hfs_fd[dir].used || !hfs_fd[dir].dir) {
			return 1;
		}
		bzero(&next, sizeof(next));
		of = hfs_fd[dir].entry == hfs_count ? "" : hfs[hfs_fd[dir].entry].path;
		of_length = strlen(of);
		for (; hfs_fd[dir].at < hfs_count; hfs_fd[dir].at++) {
			const char *p = hfs[hfs_fd[dir].at].path;

			/* a child: of's path, "/", and a name with no "/" in it */
			if (strncmp(p, of, of_length) == 0 && p[of_length] == '/' && p[of_length + 1] != 0 &&
			    strchr(p + of_length + 1, '/') == NULL) {
				strlcpy(next.name, p + of_length + 1, sizeof(next.name));
				hfs_fd[dir].at++;
				break;
			}
		}
		next.end = next.name[0] == 0;
		*out_length = listener_encode(out, 0, &next, sizeof(next));
		return 0;
	}
	case 31: {      /* stat: the name; out tsz, dev, ino, mode, nlink, rdev, size, the times */
		struct {
			uint64_t tsz, dev, ino;
			uint32_t mode, nlink;
			uint64_t rdev, size;
			int64_t times[6];
		} __attribute__((packed)) st;

		if (!hfs_name_ok(call, 1)) {
			if (apps_std_refused[method]++ < 10) {
				tell("adsp: fastrpc: apps_std stat with a name of %u bytes that does not end: refused\n",
				    call->n_in > 1 ? call->in_length[1] : 0);
			}
			return 14;
		}
		entry = hfs_find("/usr/lib/qcom/adsp/", (const char *)call->in[1], call->in_length[1] - 1);
		if (option("fastrpc_hide") && strnstr((const char *)call->in[1], "sns_reg_config", call->in_length[1]) != NULL) {
			entry = -1;     /* (boot-arg fastrpc_hide=1: as if it were not there, to see what the DSP does then) */
		}
		if (apps_std_calls[method] <= 120) {
			tell("adsp: fastrpc: apps_std stat \"%s\": %s\n", (const char *)call->in[1], entry < 0 ? "NOT THERE" : "there");
		}
		if (entry < 0) {
			return 1;
		}
		bzero(&st, sizeof(st));
		st.ino = (uint64_t)entry + 2;
		st.nlink = 1;
		if (entry == (int)hfs_count || hfs[entry].dir) {
			st.mode = 0040755;
		} else {
			st.mode = 0100644;
			st.size = hfs[entry].size;
		}
		*out_length = listener_encode(out, 0, &st, sizeof(st));
		return 0;
	}
	default:
		if (apps_std_refused[method]++ < 20) {
			tell("adsp: fastrpc: apps_std %s (method %u, scalars %#x): refused\n", name, call->method, call->sc);
		}
		return 20;
	}
}

/* One of the DSP's calls: our result, and the encoded output buffers in out (their length in *out_length). */
static uint32_t
listener_serve(const struct listener_call *call, uint8_t *out, uint32_t *out_length)
{
	uint32_t at = 0;

	*out_length = 0;
	if (call->handle == FASTRPC_REMOTECTL && call->method == 0 && call->n_in == 2 && call->in_length[0] >= 8) {
		/* open: in {name length, error's room}, name; out {handle, error}, the error's text */
		const char *name = (const char *)call->in[1];
		uint32_t room, answer[2] = { 0, 0 };

		bcopy(call->in[0] + 4, &room, 4);
		if (call->in_length[1] == 0 || name[call->in_length[1] - 1] != 0) {
			return 14;      /* AEE_EBADPARM */
		}
		if (strcmp(name, "apps_std") == 0) {
			answer[0] = 1;
		} else if (strcmp(name, "apps_mem") == 0) {
			answer[0] = 2;
		} else {
			answer[1] = (uint32_t)-5;
		}
		tell("adsp: fastrpc: the DSP opens our \"%s\": %s\n", name, answer[1] ? "we have none" : "given");
		at = listener_encode(out, at, answer, 8);
		at = listener_encode(out, at, NULL, room > 256 ? 256 : room);
		*out_length = at;
		return answer[1] ? (uint32_t)-5 : 0;
	}
	if (call->handle == FASTRPC_REMOTECTL && call->method == 1 && call->n_in >= 1 && call->in_length[0] >= 8) {
		uint32_t room, none = 0;

		bcopy(call->in[0] + 4, &room, 4);
		at = listener_encode(out, at, &none, 4);
		at = listener_encode(out, at, NULL, room > 256 ? 256 : room);
		*out_length = at;
		return 0;
	}
	if (call->handle == 1) {
		return apps_std(call, out, out_length);
	}
	tell("adsp: fastrpc: the DSP calls our handle %u, method %u (scalars %#x): unsupported\n", call->handle, call->method,
	    call->sc);
	return FASTRPC_UNSUPPORTED;
}

static void fastrpc_session_start(struct fastrpc_session *session);

static void
fastrpc_thread(void *parameter, __unused wait_result_t result)
{
	struct fastrpc_session *session = parameter;
	int32_t client = session->client, answer;
	uint8_t *inbufs = NULL, *outbufs = NULL;
	uint32_t calls = 0, rctx = 0, res = 0xffffffff, out_length = 0;
	uintptr_t memory;
	uint64_t dsp;
	uint32_t bytes;

	session->runs = true;
	for (unsigned ms = 0; ms < 15000 && fastrpc.wanted && !glink.channel[CHANNEL_FASTRPC].open; ms += 10) {
		sleep_ms(10);
	}
	if (!glink.channel[CHANNEL_FASTRPC].open) {
		tell("adsp: fastrpc: the channel \"fastrpcglink-apps-dsp\" is not open: the DSP has not opened it\n");
		goto out;
	}
	if (!hakios_audio_dsp_memory_fastrpc(&memory, &dsp, &bytes)) {
		tell("adsp: fastrpc: no memory the DSP sees\n");
		goto out;
	}
	/* the root PD the first half, the sensors' process the second */
	session->bytes = bytes / 2;
	session->memory = memory + (session == &fastrpc_sensors ? session->bytes : 0);
	session->dsp = dsp + (session == &fastrpc_sensors ? session->bytes : 0);
	tell("adsp: fastrpc: the channel is open; our page at %#llx for the DSP: attaching to %s\n", session->dsp, session->name);
	{
		struct fastrpc_buf in = { &client, sizeof(client) };

		answer = fastrpc_call(session, FASTRPC_INIT_HANDLE, FASTRPC_ATTACH, 1, &in, 0, NULL, 5000);
	}
	tell("adsp: fastrpc: %s: %s (result %d, %#x)\n", session->name, answer == 0 ? "ATTACHED" : "IT DOES NOT TAKE US", answer,
	    (uint32_t)answer);
	if (answer != 0) {
		goto out;
	}
	/* the DSP's default listener: opened through its remotectl, registered; then the listener's init2 */
	{
		static const char name[] = "adsp_default_listener";
		uint32_t lengths[2] = { sizeof(name), 256 }, opened[2] = { 0, 0 };
		char error[256];
		struct fastrpc_buf in[2] = { { lengths, 8 }, { (void *)(uintptr_t)name, sizeof(name) } };
		struct fastrpc_buf out[2] = { { opened, 8 }, { error, sizeof(error) } };

		answer = fastrpc_call(session, FASTRPC_REMOTECTL, 0, 2, in, 2, out, 5000);
		error[sizeof(error) - 1] = 0;
		tell("adsp: fastrpc: the DSP's \"%s\": result %d, handle %#x, error %d \"%s\"\n", name, answer, opened[0],
		    (int32_t)opened[1], error);
		if (answer != 0 || opened[1] != 0) {
			goto out;
		}
		answer = fastrpc_call(session, opened[0], 0, 0, NULL, 0, NULL, 5000);
		tell("adsp: fastrpc: the default listener registered: result %d\n", answer);
		if (answer != 0) {
			goto out;
		}
	}
	answer = fastrpc_call(session, FASTRPC_LISTENER, 3, 0, NULL, 0, NULL, 5000);
	tell("adsp: fastrpc: %s: the listener's init2: result %d; FROM HERE ON WE ANSWER IT\n", session->name, answer);
	if (answer != 0) {
		goto out;
	}
	session->listening = true;
	if (session == &fastrpc_root) {
		fastrpc_session_start(&fastrpc_sensors);        /* (only now: see struct fastrpc_session) */
	}
	inbufs = kalloc_data(LISTENER_INBUFS, Z_WAITOK | Z_ZERO);
	outbufs = kalloc_data(LISTENER_OUTBUFS, Z_WAITOK | Z_ZERO);
	if (inbufs == NULL || outbufs == NULL) {
		goto out;
	}
	while (fastrpc.wanted) {
		uint32_t words[4] = { rctx, res, out_length, LISTENER_INBUFS }, got[4] = { 0, 0, 0, 0 };
		struct fastrpc_buf in[2] = { { words, 16 }, { outbufs, out_length } };
		struct fastrpc_buf out[2] = { { got, 16 }, { inbufs, LISTENER_INBUFS } };
		struct listener_call call;

		answer = fastrpc_call(session, FASTRPC_LISTENER, 4, 2, in, 2, out, 0);
		if (answer != 0) {
			if (fastrpc.wanted) {
				tell("adsp: fastrpc: %s: the listener's next2 gives %d (%#x) after %u calls: we stop listening\n",
				    session->name, answer, (uint32_t)answer, calls);
			}
			break;
		}
		calls++;
		rctx = got[0];
		call.handle = got[1];
		call.sc = got[2];
		if (!listener_decode(inbufs, MIN(got[3], LISTENER_INBUFS), &call)) {
			tell("adsp: fastrpc: a call of the DSP's we cannot read (handle %u, scalars %#x, %u bytes)\n", call.handle,
			    call.sc, got[3]);
			res = 14;
			out_length = 0;
			continue;
		}
		if (calls <= 60) {
			char line[120];
			int w = 0;

			for (uint32_t i = 0; call.n_in && i < call.in_length[0] && i < 24 && w < (int)sizeof(line) - 4; i++) {
				w += snprintf(line + w, sizeof(line) - (size_t)w, "%02x", call.in[0][i]);
			}
			tell("adsp: fastrpc: %s: call %u: handle %u scalars %#x method %u, %u inputs (%u, %u, %u bytes...), %u bytes in all: %s\n",
			    session->name, calls, call.handle, call.sc, call.method, call.n_in, call.n_in > 0 ? call.in_length[0] : 0,
			    call.n_in > 1 ? call.in_length[1] : 0, call.n_in > 2 ? call.in_length[2] : 0, got[3], line);
		}
		lck_mtx_lock(&fastrpc_files_lock);
		res = listener_serve(&call, outbufs, &out_length);
		lck_mtx_unlock(&fastrpc_files_lock);
		if (calls <= 60) {
			tell("adsp: fastrpc: ... answered %d with %u bytes\n", (int32_t)res, res ? 0 : out_length);
		}
		if (res != 0) {
			out_length = 0;
		}
	}
out:
	/* (the memory stays: the DSP may still look at it) */
	if (inbufs != NULL) {
		kfree_data(inbufs, LISTENER_INBUFS);
	}
	if (outbufs != NULL) {
		kfree_data(outbufs, LISTENER_OUTBUFS);
	}
	/* the root PD that does not take us: the sensors' process all the same, as before there were two */
	if (session == &fastrpc_root && !session->listening) {
		fastrpc_session_start(&fastrpc_sensors);
	}
	session->runs = false;
	thread_terminate(current_thread());
}

static void
fastrpc_session_start(struct fastrpc_session *session)
{
	thread_t thread;

	if (session->runs || !fastrpc.wanted) {
		return;
	}
	session->runs = true;
	if (kernel_thread_start(fastrpc_thread, session, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		session->runs = false;
	}
}

static void
fastrpc_start(void)
{
	uint32_t wanted = 1;

	PE_parse_boot_argn("fastrpc", &wanted, sizeof(wanted));
	if (wanted == 0 || fastrpc_root.runs) {
		return;
	}
	fastrpc.wanted = true;
	/* fastrpc=2: the sensors' process alone, as before 26 October */
	fastrpc_session_start(wanted == 2 ? &fastrpc_sensors : &fastrpc_root);
}

/* ---- the link's thread ----------------------------------------------------------------------------- */

static volatile bool link_wanted, link_runs;

static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct smp2p words, last;
	struct glink_packet *packet;
	bool gave_up = false;

	bzero(&last, sizeof(last));
	dsp_words(&last);
	while (link_wanted) {
		sleep_us(1000);
		doorbells();
		if (modem) {
			qrtr_fresh();           /* (the QMI helpers' lookups told of servers that have come) */
		}
		if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
			last = words;
			dsp_listen(&words);
			if ((dsp_state(&words) & DSP_FATAL) && !gave_up) {
				gave_up = true;
				printf("adsp: link: THE AUDIO DSP SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", dsp_state(&words));
				show_crash_reason();
			}
		}
		for (;;) {
			unsigned from = CHANNELS;

			lck_mtx_lock(&glink_lock);
			glink_pump();
			packet = NULL;
			for (unsigned c = 0; c < CHANNELS && packet == NULL; c++) {
				struct glink_channel *channel = &glink.channel[c];

				if ((packet = channel->first) != NULL) {
					channel->first = packet->next;
					channel->last = channel->first == NULL ? NULL : channel->last;
					from = c;
				}
			}
			lck_mtx_unlock(&glink_lock);
			if (packet == NULL) {
				break;
			}
			if (from == CHANNEL_QRTR) {
				qrtr_input(packet->data, packet->length);
			} else if (from == CHANNEL_PMIC) {
				battmgr_input(packet->data, packet->length);
			} else if (from == CHANNEL_FASTRPC) {
				fastrpc_input(packet->data, packet->length);
			} else {
				apr_input(packet->data, packet->length);
			}
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
	}
	link_runs = false;
	thread_terminate(current_thread());
}

static void
link_stop(void)
{
	struct glink_packet *packet;

	if (link_runs) {
		link_wanted = false;
		for (unsigned ms = 0; ms < 2000 && link_runs; ms++) {
			sleep_ms(1);
		}
		printf("adsp: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams, APR %u and %u "
		    "packets; its doorbells heard: GLINK %u, SMP2P %u\n", link_runs ? "THE THREAD DOES NOT END" : "down", glink.said,
		    glink.heard, qrtr_out, qrtr_in, apr_out, apr_in, doorbell.glink, doorbell.smp2p);
	}
	lck_mtx_lock(&glink_lock);
	glink.up = false;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->open = false;
		while ((packet = channel->first) != NULL) {
			channel->first = packet->next;
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		channel->last = NULL;
	}
	lck_mtx_unlock(&glink_lock);
	bzero(qrtr_servers, sizeof(qrtr_servers));
	qrtr_peer = 0;
}

/* ---- step 8: GLINK's queues, looked at -------------------------------------------------------------- */

static bool
step_queues(void)
{
	struct glink_message message;
	uint32_t head, tail;

	announce("find GLINK's descriptor and the two queues, and read what the audio DSP has put into its own");
	/*
	 * Linux's qcom_glink_smem_register() makes the descriptor (478) and our queue (479, 16 KB) once
	 * the DSP is up, and finds the DSP's queue (480). The Mi 9T's DSP makes 478 and 480 itself and
	 * leaves 479 to us (redwood's had made all three): made here when it is the one missing.
	 */
	if (!glink_find() && glink.descriptor != 0 && glink.theirs != 0 && glink.ours == 0) {
		uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);

		printf("adsp: %u: our GLINK queue (item %u) is not there: made, as Linux makes it\n", step, ITEM_GLINK_OUR_QUEUE);
		if (partition != 0) {
			item_make(partition, ITEM_GLINK_OUR_QUEUE, 16384);
		}
	}
	if (!glink_find()) {
		printf("adsp: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
		    "the DSP's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
		return false;
	}
	head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
	tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
	printf("adsp: %u: our queue: %u bytes at %#lx, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#lx, written "
	    "to %#x, read by us to %#x\n", step, glink.ours_size, SMEM_PHYS + glink.ours, smem_word(glink.descriptor + GLINK_OUR_HEAD),
	    smem_word(glink.descriptor + GLINK_OUR_TAIL), glink.theirs_size, SMEM_PHYS + glink.theirs, head, tail);
	for (uint32_t at = 0, shown = 0; at + sizeof(message) <= head && head < glink.theirs_size && tail == 0 && shown < 8; shown++) {
		char name[33] = "";
		uint32_t length = sizeof(message);

		glink_peek(&message, at, sizeof(message));
		if (message.command == GLINK_OPEN) {
			glink_peek(name, at + 8, message.param2 < 32 ? message.param2 : 32);
			length += message.param2;
		}
		printf("adsp: %u:   in the DSP's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
		    message.param2, name);
		if (message.command != GLINK_VERSION && message.command != GLINK_OPEN) {
			break;
		}
		at += (length + 7) & ~7u;
	}
	return true;
}

/* ---- step 9: GLINK, spoken -------------------------------------------------------------------------- */

static bool
step_link(void)
{
	thread_t thread;
	uint64_t began;

	if (!dsp_started || glink.descriptor == 0) {
		return false;
	}
	announce("take up GLINK with the audio DSP: our version, then whatever its messages call for; a thread of ours listens "
	    "from here on, every millisecond");
	snprintf(last_write, sizeof(last_write), "step %u took up GLINK", step);
	lck_mtx_lock(&glink_lock);
	glink.features = GLINK_INTENT_REUSE;
	glink.agreed = glink.broken = false;
	glink.said = glink.heard = 0;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->asked = channel->acknowledged = channel->open = false;
		channel->theirs = channel->our_intent_count = 0;
		bzero(channel->their_intents, sizeof(channel->their_intents));
	}
	apr_in = apr_out = 0;
	audio_up = false;
	bzero(glink.others, sizeof(glink.others));
	glink.trace = 60;
	glink.up = true;
	qrtr_hellos = qrtr_in = qrtr_out = qrtr_lookups = 0;
	doorbell.since = began = now_us();
	glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
	lck_mtx_unlock(&glink_lock);
	link_wanted = link_runs = true;
	if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
		link_wanted = link_runs = false;
		printf("adsp: %u: no thread\n", step);
		return false;
	}
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < 5000 && !glink.channel[CHANNEL_QRTR].open && !glink.broken; ms++) {
		sleep_ms(1);
	}
	sleep_ms(50);
	printf("adsp: %u: %llu ms after our version: versions %s (features %#x); the channel \"IPCRTR\" is %s, \"apr_audio_svc\" %s; "
	    "the DSP also opened \"%s\" \"%s\" \"%s\" \"%s\"\n", step, (now_us() - began) / 1000, glink.agreed ? "agreed" :
	    "NOT AGREED", glink.features, glink.channel[CHANNEL_QRTR].open ? "OPEN" : "NOT OPEN",
	    glink.channel[CHANNEL_APR].open ? "OPEN" : "NOT OPEN", glink.others[0], glink.others[1], glink.others[2], glink.others[3]);
	return glink.channel[CHANNEL_QRTR].open;
}

/* ---- step 10: QRTR ---------------------------------------------------------------------------------- */

static bool
step_qrtr(void)
{
	unsigned services = 0;

	if (!glink.channel[CHANNEL_QRTR].open) {
		return false;
	}
	PE_parse_boot_argn("adsp_listen", &listen_seconds, sizeof(listen_seconds));
	announce("listen to QRTR on the channel: the DSP's hello (answered from the link's thread), and the services it tells of");
	for (unsigned ms = 0; ms < listen_seconds * 1000; ms++) {
		sleep_ms(1);
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	printf("adsp: %u: after %u s: %s; the DSP is node %u and has told of %u services; it looked for %u of ours; QRTR received %u "
	    "and sent %u datagrams\n", step, listen_seconds, qrtr_hellos ? "HELLO BOTH WAYS" : "NO HELLO FROM THE DSP", qrtr_peer,
	    services, qrtr_lookups, qrtr_in, qrtr_out);
	return qrtr_hellos != 0;
}

/* ---- step 11: APR ---------------------------------------------------------------------------------- */

static bool
step_apr(void)
{
	uint32_t wait_seconds = 10, asked = 0, state = 0;
	uint64_t began = now_us();

	if (modem) {
		/* (the modem has no APR: watched for 60 s instead, for what it asks of us and says) */
		printf("adsp: %u: the modem: no APR; 60 s of watching what it does\n", step);
		for (unsigned s = 0; s < 60 && !glink.broken; s++) {
			sleep_ms(1000);
		}
		return true;
	}

	PE_parse_boot_argn("adsp_apr_wait", &wait_seconds, sizeof(wait_seconds));
	announce("ask the audio DSP's core service over APR whether the audio services are up (again and again, as Android "
	    "does), then which services it has");
	for (unsigned ms = 0; ms < 3000 && !glink.channel[CHANNEL_APR].open; ms++) {
		sleep_ms(1);
	}
	if (!glink.channel[CHANNEL_APR].open) {
		printf("adsp: %u: the channel \"apr_audio_svc\" is not open: the DSP has not opened it\n", step);
		return false;
	}
	while (now_us() - began < wait_seconds * 1000000ull) {
		asked++;
		if (apr_ask(APR_SVC_ADSP_CORE, AVCS_CMD_ADSP_EVENT_GET_STATE, NULL, 0, 500) &&
		    apr_answer.opcode == AVCS_CMDRSP_ADSP_EVENT_GET_STATE) {
			state = apr_answer.words[0];
			if (state == 1) {
				break;
			}
		}
		sleep_ms(200);
	}
	audio_up = state == 1;
	printf("adsp: %u: %s after %llu ms and %u questions (the last answer: opcode %#x, %u bytes, %#x)\n", step, state == 1 ?
	    "THE AUDIO SERVICES ARE UP" : "THE AUDIO SERVICES DO NOT SAY THEY ARE UP", (now_us() - began) / 1000, asked,
	    apr_answer.opcode, apr_answer.length, apr_answer.words[0]);
	if (apr_ask(APR_SVC_ADSP_CORE, AVCS_GET_VERSIONS, NULL, 0, 1000)) {
		char line[400];
		int at = 0;

		for (uint32_t i = 0; i < apr_answer.length / 4 && i < 64 && at < (int)sizeof(line) - 12; i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %#x", apr_answer.words[i]);
		}
		printf("adsp: %u: the services and their versions (opcode %#x, %u bytes):%s\n", step, apr_answer.opcode,
		    apr_answer.length, line);
	} else {
		printf("adsp: %u: no answer to the question of the services' versions\n", step);
	}
	return state == 1;
}

/* ---- the steps, and their end ----------------------------------------------------------------------- */

static bool (*const steps[STEPS])(void) = {
	step_survey, step_firmware, step_mailbox, step_header, step_image, step_our_item, step_start, step_queues, step_link,
	step_qrtr, step_apr,
};

static bool keep;                       /* the DSP and the link stay up while the system runs (adsp_keep, adsp=1) */

static void
steps_over(uint32_t done)
{
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && done == last_step && keep) {
		printf("adsp: THE AUDIO DSP STAYS ON while the system runs%s\n", link_runs ? ", and the link with it" : "");
	} else if (dsp_started) {
		dsp_shutdown("the steps are over");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* When the run ends in order: a DSP that runs is shut down, for the next kernel to start again. */
void
hakios_adsp_stop(void)
{
	battmgr_stop();
	fastrpc.wanted = false;
	hakios_sound_dsp_down();               /* the amplifiers, the stream and the port first, while the DSP still listens */
	hakios_mic_dsp_down();                 /* and what the microphones' path has asked for */
	lck_mtx_lock(&dsp_lock);
	if (dsp_started) {
		unsigned services = 0;
		uint32_t node = qrtr_peer;
		bool down;

		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			services += qrtr_servers[i].there;
		}
		down = dsp_shutdown("the run is about to end");
		/* (said again at the end: a long run's log keeps its beginning and its end, and the middle may be gone) */
		tell("adsp: the audio DSP is off: it had been up%s (node %u, %u services; audio services %s), it %s, and TrustZone %s\n",
		    node ? " and linked" : ", NOT LINKED", node, services, audio_up ? "up" : "NOT UP", stopped_by_its_word ?
		    "stopped when asked" : "DID NOT SAY IT HAD STOPPED", down ? "has shut it down" : "DID NOT SHUT IT DOWN");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* The same on a panic's way back to the loader: one processor, interrupts off, no lock, no sleep. */
void
hakios_adsp_quiesce(void)
{
	hakios_sound_quiesce();                /* the amplifiers off, whatever the DSP's state */
	if (!dsp_started) {
		return;
	}
	leaving = true;
	new_from = ~0u;
	dsp_shutdown("the kernel is on its way back to the loader");
}

static void
run_steps(void)
{
	uint32_t done = 0;

	for (step = 1; step <= last_step; step++) {
		if (!steps[step - 1]()) {
			printf("adsp: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("adsp: step %u: done\n", step);
		done = step;
	}
	step = done;
	new_from = STEPS + 1;
	steps_over(done);
	printf("adsp: over after step %u; %s\n", done, last_write);
}

static void
whole_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t began = now_us();
	unsigned services = 0;

	/* (the firmware may be on the root file system, which is not mounted yet when this begins: a minute at most) */
	for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
		sleep_ms(200);
	}
	run_steps();
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	if (step == STEPS && dsp_started) {
		tell("adsp: THE %s IS UP AND LINKED%s, %llu ms after the kernel began on it (the root's waited for included): node "
		    "%u, %u services; it stays on\n", modem ? "MODEM" : "AUDIO DSP", modem ? "" : ", its audio services up (APR)",
		    (now_us() - began) / 1000, qrtr_peer, services);
		if (!modem) {
			hakios_sound_dsp_up();         /* (boot-arg sound=N: the speakers' path, hakios.sound.c) */
			hakios_mic_dsp_up();           /* (boot-arg mic=N: the microphones' path, hakios.mic.c) */
			battmgr_start();                /* (how the battery is, from the DSP; battmgr=0: not asked) */
			fastrpc_start();                /* (the sensors' process attached to, on the Mi 9T; fastrpc=0: not) */
		}
	} else {
		tell("adsp: THE %s DID NOT COME UP: step %u failed (%s); see the kernel's own log for the steps\n",
		    modem ? "MODEM" : "AUDIO DSP", step + 1,
		    last_write);
	}
	thread_terminate(current_thread());
}

/* Called while the system starts (drivers/input/hakios.input.c), after the Wi-Fi's start has been set going. */
#ifndef HAKIOS_SM7150_MODEM_COPY
static void adsp_start(void);
void hakios_adsp_modem_init(void);      /* hakios.adsp_sm7150_modem.c */

void
hakios_adsp_init(void)
{
	adsp_start();
	hakios_adsp_modem_init();       /* adsp_modem=1: the modem beside it, from the second copy of this file */
}

static void
adsp_start(void)
#else
void
hakios_adsp_init(void)
#endif
{
	uint32_t on = 0;
	thread_t thread;

#ifdef HAKIOS_SM7150_MODEM_COPY
	/*
	 * The modem's copy of this file (hakios.adsp_sm7150_modem.c): adsp_modem=1 brings the modem up beside the audio
	 * DSP, all its steps, on its own thread (on the Mi 9T its firmware runs the Wi-Fi). adsp_step is the
	 * audio DSP's.
	 */
	modem = true;
	(void)on;
	if (!option("adsp_modem")) {
		return;
	}
	hakios_return_register(hakios_adsp_quiesce, hakios_adsp_stop);  /* (named hakios_adsp_modem_*: no common file knows them) */
#else
	PE_parse_boot_argn("adsp_step", &last_step, sizeof(last_step));
	modem = false;
	if (last_step == 0 && (!PE_parse_boot_argn("adsp", &on, sizeof(on)) || on != 1)) {
		return;
	}
#endif
	if (pe_arm_get_soc_base_phys() != HAKIOS_SOC_BASE) {
		printf("adsp: adsp=1 and adsp_step=N are for the phone's audio DSP, and this is not the phone\n");
		return;
	}
	if (last_step == 0) {
		/* all of it, on a thread of its own, and left up */
		last_step = STEPS;
		keep = true;
		quiet = true;
		pause_ms = 0;
		listen_seconds = 1;
		new_from = STEPS + 1;
		if (kernel_thread_start(whole_thread, NULL, &thread) != KERN_SUCCESS) {
			printf("adsp: no thread: the audio DSP is not brought up\n");
			return;
		}
		thread_deallocate(thread);
		tell(modem ? "adsp: the modem is being brought up beside the system's start (adsp_modem=1)\n" :
		    "adsp: the audio DSP is being brought up beside the system's start (adsp=1)\n");
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	keep = option("adsp_keep");
	new_from = last_step;
	PE_parse_boot_argn("adsp_new", &new_from, sizeof(new_from));
	PE_parse_boot_argn("adsp_pause", &pause_ms, sizeof(pause_ms));
	printf("adsp: steps up to %u; those from %u on are new and wait %u ms after each announcement\n", last_step, new_from,
	    pause_ms);
	run_steps();
	if (option("adsp_return")) {
		printf("adsp: back to the loader (adsp_return)\n");
		loader_return();
	}
	printf("adsp: the system goes on starting\n");
}
