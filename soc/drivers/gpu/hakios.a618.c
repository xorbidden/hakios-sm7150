/*
 * The GPU of the Mi 9T (davinci, SM7150), an Adreno 618 (Linux: ADRENO_6XX_GEN1, the A630 family),
 * woken step by step as drivers/gpu/hakios.adreno.c wakes redwood's Adreno 642L (GEN4, the A660
 * family): the same way of going about it -- a step says what it is about to touch before it
 * touches it, reads come before writes, and a step that does not find what it needs ends the
 * sequence -- with this SoC's addresses, and where the generations differ, GEN1's way.
 * Boot-arg a618=N: steps 1 to N; nothing is touched without it. So far:
 *   1  reads   what feeds the GPU from outside: its clocks in the global clock controller, and
 *              the names and levels RPMh knows its supplies by
 *   2  reads   the fuse that says which speed grade of the GPU this is
 *   3  reads   the GPU's own clock controller, as the boot chain left it
 *   4  writes  the "CX" power switch on: the half of the GPU block that is not the 3D core
 *   5  writes  the clocks of that half on: the GPU's bus and memory clocks in the global
 *              controller, the GMU's clock at 200 MHz and the crystal's in its own
 *   6  reads   the first words from the GPU block itself: registers of the GMU (among them a
 *              counter that should be seen counting), of the bus interface and of the CX odds and ends
 *   7  reads   the GMU's own channel to RPMh (on GEN1 inside the GMU's block)
 *   8  reads   the SMMU in front of the GPU: its ID registers, and which stream goes where
 *   9  reads   that SMMU's first translation context
 *  10  writes  two words into the GMU's memory and reads them back: the way its firmware goes
 *  11  asks    TrustZone questions that pass no memory: whether it takes calls from this kernel,
 *              and which firmware images it says it authenticates
 *  12  asks    TrustZone to check the signature of the GPU's "zap shader", the signed image without
 *              which the 3D core stays locked in secure mode: its header (fw/a615_zap.mdt, the small
 *              image's) goes into memory of ours that TrustZone is pointed at
 *  13  writes  page tables for the memory the GMU needs (its log, its message queues), and the
 *              SMMU: a translation context with those tables, and the GMU's requests (stream 5,
 *              its kernel context) sent through it
 *  14  writes  the GMU's firmware (fw/a630_gmu.bin: what Linux and Xiaomi's kgsl load for the 618, a
 *              "legacy" image of 32K) into its own memory, through its registers, and reads it back
 *  15  writes  the GMU's channels to RPMh (its RSC's and the PDC's sequences, which on GEN1 are ours to
 *              write), its configuration, and lets its processor out of reset: the firmware runs
 *  16  talks   to the GMU: on GEN1 first the GX rail's vote, SPTPRAC and the queues' start, then version
 *              1 of its message protocol: its start, the version, the
 *              table of the GPU's and its own speeds with their RPMh votes, the A618's (fixed) memory
 *              bandwidth table, and a test message
 *  17  asks    the GMU for the 3D core's speed and power (GEN1's out-of-band requests); reads the core
 *  18  writes  the 3D core's translation context (SMMU context 0, stream 0), with its ring and its
 *              microcode (fw/a630_sqe.fw) in it
 *  19  asks    TrustZone to load the zap shader (fw/a615_zap.b02) into a page of ours (and, if it
 *              refuses, to give up one a kernel before this one left first)
 *  20  writes  the 3D core's registers (Linux's hw_init, as it is for the A618); starts its command
 *              processor and gives it its first command
 *  21  commands the 3D core out of secure mode, and to write a word into a page of ours
 *  22  commands a picture filled with three colours, and shows it on the panel (for 10 seconds)
 * a618_keep=1 (and gpu=1, which asks for steps 1 to 21 without 10 to 12 and without pauses): the
 * GPU is not stopped when the steps are over but kept for the system to draw with.
 * When the steps are over, what they started is stopped again in reverse: the GMU's processor held
 * in reset, then the SMMU's stream entry and context of step 13 switched off.
 * Boot-arg a618_pause=MS: how long the newest step waits after announcing a group of accesses
 * (default 2200: time for the loader, watching from another core, to note the line).
 *
 * Addresses are from the phone's own device tree (the stock dtb's qcom,kgsl-3d0@5000000,
 * qcom,gmu@506a000, qcom,gpucc (0x5090000), qcom,gdsc@509106c and @509100c, qfprom), register
 * layouts from Linux (drivers/clk/qcom/gcc-sm7150.c) and Xiaomi's davinci kernel
 * (drivers/clk/qcom/{gcc,gpucc}-sdmmagpie.c; Linux 7.0 has no SM7150 GPU clock controller).
 * Firmware for the later steps (the vendor's, never in the repository): fw-gpu/ next to the
 * repository's files, from the phone's /vendor/firmware -- a630_sqe.fw, a618_gmu.bin, a615_zap.mdt
 * and its .b00-.b02.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <kern/startup.h>
#include <sys/systm.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>
#include <pexpert/device_tree.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

static uint32_t pause_ms = 2200;
static bool quiet;                      /* gpu=1, and the GPU kept: registers are not said one by one */
static uint32_t step;                   /* the one being performed */
static uint32_t newest_step;            /* the highest one asked for: the one that has not been seen to work */

/* Every line goes to the loader's log as well, before the next register is touched (as hakios.adreno.c). */
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* ---- register access (one plain load or store: some of it may be the hypervisor's imitation) ---------- */

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

/* A stretch of registers; mapped when a step first needs it (no more than is used: room is small). */
struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static bool
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
	return b->base != 0;
}

static void
hold(void)
{
	if (step == newest_step && pause_ms) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("a618: step %u: %s\n", step, what);
	hold();
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	if (!need(b)) {
		printf("a618: %u: cannot map %s\n", step, b->name);
		return 0;
	}
	value = rd(b->base + offset);
	if (!quiet) {
		printf("a618: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	}
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	if (!need(b)) {
		return;
	}
	if (!quiet) {
		printf("a618: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	}
	hold();
	wr(b->base + offset, value);
}

/* Set bits of a register unless they are set; what it holds afterwards. */
static uint32_t
set(struct block *b, const char *name, uint32_t offset, uint32_t bits)
{
	uint32_t was = get(b, name, offset);

	if ((was & bits) == bits) {
		return was;
	}
	put(b, name, offset, was | bits);
	return get(b, name, offset);
}

/* Look at a register until the masked bits have the value, for so many milliseconds. */
static bool
wait_for(struct block *b, uint32_t offset, uint32_t mask, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((rd(b->base + offset) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	return (rd(b->base + offset) & mask) == value;
}

/* ---- step 1: the clocks from outside, and RPMh's names ------------------------------------------------ */

/*
 * The GPU's clocks in the global clock controller, by the names the stock dtb's GPU nodes give them
 * (offsets: gcc-sm7150.c, gcc-sdmmagpie.c; all but the memory's as on redwood): its two bus
 * connections, the memory, the registers of its clock controller (which Linux keeps on for good),
 * and the two taps of the 600 MHz PLL its clock controller takes as inputs.
 */
static struct block gcc_gpu = { "gcc", 0x00171000, 0x1000 };     /* gcc + 0x71000 */
static struct block gcc_vote = { "gcc", 0x00152000, 0x1000 };    /* gcc + 0x52000 */
static struct block gcc_ddrss = { "gcc", 0x00144000, 0x1000 };   /* gcc + 0x44000 */
#define GCC_PLL_VOTE            0x004           /* gcc + 0x52004: bit 15 gcc_gpu_gpll0_clk_src, 16 its divided tap */
#define GCC_GPU_CFG_AHB_CBCR    0x004           /* gcc + 0x71004 */
#define GCC_GPU_MEMNOC_GFX_CBCR 0x00c
#define GCC_GPU_SNOC_DVM_GFX_CBCR 0x018
#define GCC_DDRSS_GPU_AXI_CBCR  0x52c           /* gcc + 0x4452c (redwood: 0x71154) */
#define GCC_PLL_VOTE_GPU_DIV    (1u << 16)
#define CBCR_ENABLE             (1u << 0)       /* a clock's control register: on */
#define CBCR_OFF                (1u << 31)      /* ... and whether it is in fact off */

#define AOP_DICT_PHYS           0x0c3f0000UL    /* "qcom,cmd-db" (0xc3f000c): where the command database is */
#define AOP_DICT_OFFSET         0xc

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
static uint32_t cmd_db_size;

/* Where RPMh has a resource (0: nowhere) and, for a supply rail, the levels it knows (as hakios.adreno.c). */
static uint32_t
rpmh_resource(const char *name, uint16_t *levels, unsigned *level_count)
{
	char id[8] = { 0 };

	*level_count = 0;
	if (cmd_db == NULL) {
		vm_offset_t dict = ml_io_map(AOP_DICT_PHYS, 0x1000);
		uint32_t phys, size;

		if (dict == 0) {
			return 0;
		}
		phys = rd(dict + AOP_DICT_OFFSET);
		size = rd(dict + AOP_DICT_OFFSET + 4);
		if (phys == 0 || size < sizeof(struct cmd_db_header) || size > 0x100000) {
			return 0;
		}
		cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(phys, (size + 0xfff) & ~0xfffu);
		cmd_db_size = size;
		if (cmd_db == NULL) {
			return 0;
		}
	}
	if (cmd_db->magic[0] != 0xdb || cmd_db->magic[1] != 0x30 || cmd_db->magic[2] != 0x03 || cmd_db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512 || sizeof(*cmd_db) + slave->header_offset +
		    (size_t)slave->count * sizeof(*entry) > cmd_db_size) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data = cmd_db->data + slave->data_offset + entry[i].offset;
			unsigned count = entry[i].length / 2u;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if (sizeof(*cmd_db) + (size_t)slave->data_offset + entry[i].offset + entry[i].length > cmd_db_size) {
				count = 0;
			}
			for (unsigned l = 0; l < count && l < 16; l++) {
				levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
				if (l > 0 && levels[l] == 0) {
					break;
				}
				*level_count = l + 1;
			}
			return entry[i].address;
		}
	}
	return 0;
}

static void
show_rpmh_resource(const char *name)
{
	uint16_t level[16];
	unsigned count = 0;
	uint32_t address = rpmh_resource(name, level, &count);
	char levels[160];
	int at = 0;

	if (address == 0) {
		printf("a618: %u: RPMh has no \"%s\"\n", step, name);
		return;
	}
	levels[0] = 0;
	for (unsigned l = 0; l < count && at < (int)sizeof(levels) - 8; l++) {
		at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", level[l]);
	}
	printf("a618: %u: RPMh has \"%s\" at %#x; levels:%s\n", step, name, address, levels);
}

static bool
step_outside(void)
{
	uint32_t ahb;

	announce("read the GPU's clocks in the global clock controller");
	get(&gcc_vote, "pll votes", GCC_PLL_VOTE);
	ahb = get(&gcc_gpu, "gpu_cfg_ahb", GCC_GPU_CFG_AHB_CBCR);
	get(&gcc_gpu, "gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX_CBCR);
	get(&gcc_gpu, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR);
	get(&gcc_ddrss, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR);

	announce("look up the GPU's supply rails in the boot loader's list of RPMh resources");
	show_rpmh_resource("gfx.lvl");          /* the 3D core's own rail */
	show_rpmh_resource("cx.lvl");           /* the CX half's, shared with most of the SoC */
	show_rpmh_resource("mx.lvl");           /* memories */
	if (ahb & CBCR_OFF) {
		/* (the GPU's clock controller does not answer without it: step 3 would hang the bus) */
		printf("a618: %u: gpu_cfg_ahb is off: the GPU's clock controller cannot be read; ending here\n", step);
		return false;
	}
	return true;
}

/* ---- step 2: the fuses ------------------------------------------------------------------------------- */

/*
 * The stock dtb's qcom,gpu-speed-bin = <0x41a0 0x1fe00000 0x15>: the word at qfprom + 0x41a0 (in the
 * error-corrected copy from 0x4000 on, the one Linux reads), bits 21 to 28.
 */
static struct block qfprom = { "qfprom", 0x00784000, 0x1000 };
#define QFPROM_GPU_SPEED_BIN    0x1a0           /* 0x780000 + 0x41a0 */

static uint32_t speed_bin;

static bool
step_fuses(void)
{
	uint32_t word;

	announce("read the GPU's speed grade from the fuses (0x7841a0)");
	word = get(&qfprom, "gpu speed bin word", QFPROM_GPU_SPEED_BIN);
	speed_bin = (word & 0x1fe00000) >> 21;
	printf("a618: %u: speed bin %u\n", step, speed_bin);
	return true;
}

/* ---- step 3: the GPU's clock controller ----------------------------------------------------------------- */

/* The stock dtb's qcom,gpucc (0x5090000, 0x9000 long); offsets: gpucc-sdmmagpie.c -- the same as redwood's. */
static struct block gpucc = { "gpucc", 0x05090000, 0x2000 };
#define GPUCC_PLL0_MODE         0x0000          /* the 3D core's PLL: bit 31 locked, bit 0 output on; +4: times 19.2 MHz */
#define GPUCC_PLL1_MODE         0x0100          /* the GMU's */
#define GPUCC_CXO_AON_CBCR      0x1004
#define GPUCC_GX_GDSCR          0x100c          /* gpu_gx_gdsc (dt qcom,gdsc@509100c): the 3D core's power switch */
#define GPUCC_GX_GFX3D_RCGR     0x101c
#define GPUCC_GX_GFX3D_CBCR     0x1054
#define GPUCC_GX_GMU_CBCR       0x1064
#define GPUCC_CX_GDSCR          0x106c          /* gpu_cx_gdsc (dt qcom,gdsc@509106c) */
#define GPUCC_AHB_CBCR          0x1078
#define GPUCC_CX_GMU_CBCR       0x1098
#define GPUCC_CXO_CBCR          0x109c
#define GPUCC_GMU_RCGR          0x1120
#define GPUCC_CX_HW_CTRL        0x1540          /* dt: the cx gdsc's hw-ctrl-addr (syscon@5091540): bit 31, CX on */

#define GDSC_SW_COLLAPSE        (1u << 0)       /* a power switch's control register (gdsc.c): off, by software */
#define GDSC_HW_CONTROL         (1u << 1)       /* something else than software operates it */
#define GDSC_POWER_ON           (1u << 31)

static bool
step_gpucc(void)
{
	announce("read the GPU's clock controller as the boot chain left it (0x5090000)");
	get(&gpucc, "pll0 mode", GPUCC_PLL0_MODE);
	get(&gpucc, "pll0 l", GPUCC_PLL0_MODE + 4);
	get(&gpucc, "pll1 mode", GPUCC_PLL1_MODE);
	get(&gpucc, "pll1 l", GPUCC_PLL1_MODE + 4);
	get(&gpucc, "cxo_aon", GPUCC_CXO_AON_CBCR);
	get(&gpucc, "gx gdsc", GPUCC_GX_GDSCR);
	get(&gpucc, "gx_gfx3d rcg", GPUCC_GX_GFX3D_RCGR);
	get(&gpucc, "gx_gfx3d", GPUCC_GX_GFX3D_CBCR);
	get(&gpucc, "gx_gmu", GPUCC_GX_GMU_CBCR);
	get(&gpucc, "cx gdsc", GPUCC_CX_GDSCR);
	get(&gpucc, "ahb", GPUCC_AHB_CBCR);
	get(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR);
	get(&gpucc, "cxo", GPUCC_CXO_CBCR);
	get(&gpucc, "gmu rcg", GPUCC_GMU_RCGR);
	get(&gpucc, "gmu rcg cfg", GPUCC_GMU_RCGR + 4);
	return true;
}

/* ---- step 4: CX power --------------------------------------------------------------------------------- */

/* Not to touch the GPU's clock controller without the clock of its registers (gcc gpu_cfg_ahb). */
static bool
gpucc_answers(void)
{
	if (!need(&gcc_gpu) || !need(&gpucc)) {
		return false;
	}
	if (rd(gcc_gpu.base + GCC_GPU_CFG_AHB_CBCR) & CBCR_OFF) {
		printf("a618: %u: the GPU clock controller's registers have no clock (gcc gpu_cfg_ahb %#x)\n", step,
		    rd(gcc_gpu.base + GCC_GPU_CFG_AHB_CBCR));
		return false;
	}
	return true;
}

static bool
cx_is_on(void)
{
	return (rd(gpucc.base + GPUCC_CX_GDSCR) | rd(gpucc.base + GPUCC_CX_HW_CTRL)) & GDSC_POWER_ON;
}

/*
 * The switch hangs on the rail "cx.lvl", up for as long as the SoC runs; the vendor's driver
 * (gdsc-regulator.c) clears the collapse bit and waits for bit 31 of the hw-ctrl-addr register,
 * Linux for bit 31 of the control register itself: either will do.
 */
static bool
step_cx_power(void)
{
	uint32_t gdscr;

	if (!gpucc_answers()) {
		return false;
	}
	announce("switch the CX half of the GPU block on (gpu_cx_gdsc): clear the collapse bit of its power switch");
	gdscr = get(&gpucc, "cx gdsc", GPUCC_CX_GDSCR);
	get(&gpucc, "cx hw ctrl", GPUCC_CX_HW_CTRL);
	if (cx_is_on()) {
		/* (an earlier kernel of ours left it on: nothing of ours stops the GPU block yet, so as it is) */
		printf("a618: %u: CX is on already\n", step);
		return true;
	}
	if (gdscr & GDSC_HW_CONTROL) {
		printf("a618: %u: the switch is under hardware control: not ours to operate\n", step);
		return false;
	}
	put(&gpucc, "cx gdsc", GPUCC_CX_GDSCR, gdscr & ~GDSC_SW_COLLAPSE);
	for (int i = 0; i < 500 && !cx_is_on(); i++) {
		sleep_us(100);
	}
	get(&gpucc, "cx gdsc", GPUCC_CX_GDSCR);
	get(&gpucc, "cx hw ctrl", GPUCC_CX_HW_CTRL);
	if (!cx_is_on()) {
		printf("a618: %u: CX does not come on\n", step);
		return false;
	}
	printf("a618: %u: CX is on\n", step);
	return true;
}

/* ---- step 5: clocks ------------------------------------------------------------------------------------ */

#define RCG_UPDATE              (1u << 0)       /* command register (clk-rcg2.c): take the new source and divider */
#define RCG_SOURCE_MASK         0x371fu         /* [4:0] 2*divider-1, [10:8] source, [13:12] mode */
#define RCG_GPLL0_DIV(twice)    (6u << 8 | ((twice) - 1))       /* source 6: the 300 MHz tap; twice the divider wanted */

/* Turn a clock on and see it run. Not every clock's status can be believed (halt_check): `must' says which. */
static bool
clock_on(struct block *b, const char *name, uint32_t cbcr, bool must)
{
	if (!(set(b, name, cbcr, CBCR_ENABLE) & CBCR_OFF) || wait_for(b, cbcr, CBCR_OFF, 0, 10)) {
		return true;
	}
	printf("a618: %u: %s %s stays off (%#x)%s\n", step, b->name, name, rd(b->base + cbcr),
	    must ? "" : "; its status bit is not one to rely on, going on");
	return !must;
}

static bool
clock_source(const char *name, uint32_t rcgr, uint32_t source)
{
	uint32_t was = get(&gpucc, name, rcgr + 4);

	if ((was & RCG_SOURCE_MASK) == source) {
		return true;
	}
	put(&gpucc, name, rcgr + 4, (was & ~RCG_SOURCE_MASK) | source);
	put(&gpucc, name, rcgr, rd(gpucc.base + rcgr) | RCG_UPDATE);
	if (!wait_for(&gpucc, rcgr, RCG_UPDATE, 0, 10)) {
		printf("a618: %u: %s does not take its new source (command %#x)\n", step, name, rd(gpucc.base + rcgr));
		return false;
	}
	get(&gpucc, name, rcgr + 4);
	return true;
}

/*
 * The GMU's clocks at the rate Linux and the vendor's driver bring it up with: 200 MHz from the
 * 300 MHz tap of the global 600 MHz PLL, which runs anyway (gpucc-sdmmagpie.c: no "hub" clock on
 * this generation, nor an SMMU vote clock). Halt checks: gpucc-sdmmagpie.c (ahb's is a delay only).
 */
static bool
step_clocks(void)
{
	if (!gpucc_answers()) {
		return false;
	}
	if (!cx_is_on()) {
		printf("a618: %u: CX is off: step 4\n", step);
		return false;
	}
	announce("in the global clock controller: ask for the 300 MHz tap for the GPU; the GPU's bus and memory clocks on");
	set(&gcc_vote, "pll votes", GCC_PLL_VOTE, GCC_PLL_VOTE_GPU_DIV);
	if (!clock_on(&gcc_gpu, "gpu_memnoc_gfx", GCC_GPU_MEMNOC_GFX_CBCR, false) ||
	    !clock_on(&gcc_gpu, "gpu_snoc_dvm_gfx", GCC_GPU_SNOC_DVM_GFX_CBCR, true) ||
	    !clock_on(&gcc_ddrss, "ddrss_gpu_axi", GCC_DDRSS_GPU_AXI_CBCR, false)) {
		return false;
	}
	announce("in the GPU's clock controller: the GMU's clock to 200 MHz");
	if (!clock_source("gmu_clk_src", GPUCC_GMU_RCGR, RCG_GPLL0_DIV(3))) {        /* 300 MHz / 1.5 */
		return false;
	}
	announce("in the GPU's clock controller: the crystal's, the register bus's and the GMU's clocks on");
	if (!clock_on(&gpucc, "cxo", GPUCC_CXO_CBCR, true) ||
	    !clock_on(&gpucc, "ahb", GPUCC_AHB_CBCR, false) ||
	    !clock_on(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR, true)) {
		return false;
	}
	printf("a618: %u: the CX half's clocks run\n", step);
	return true;
}

/* ---- step 6: the first words from the GPU block ------------------------------------------------------- */

/*
 * The stock dtb's qcom,gmu "kgsl_gmu_reg" at 0x506a000, 0x31000 long. Offsets in a6xx_gmu.xml are in
 * words, the same for every A6xx (on GEN1 the GMU's RSC is inside this block, at +0x23000).
 */
static struct block gmu = { "gmu", 0x0506a000, 0x31000 };
#define GMU_WORD(n)             (4u * (n))
#define GMU_DTCM                GMU_WORD(0x1c00)        /* the processor's data memory (its code: 0x0c00) */
#define GMU_DTCM_VERSIONS       GMU_WORD(0x1c00 + 0xff8) /* five words where loaded firmware carries its versions */
#define GMU_CM3_SYSRESET        GMU_WORD(0x5000)        /* 1: the processor is held in reset */
#define GMU_CM3_BOOT_CONFIG     GMU_WORD(0x5001)
#define GMU_CM3_FW_INIT_RESULT  GMU_WORD(0x501c)        /* 0xbabeface once firmware has started */
#define GMU_CM3_CFG             GMU_WORD(0x502d)
#define GMU_ALWAYS_ON_COUNTER_L GMU_WORD(0x5088)        /* counts the 19.2 MHz crystal */
#define GMU_ALWAYS_ON_COUNTER_H GMU_WORD(0x5089)
#define GMU_SPTPRAC_PWR_CLK_STATUS GMU_WORD(0x50d0)     /* bit 6: the 3D core has no power, bit 7: no clock */
#define GMU_RPMH_POWER_STATE    GMU_WORD(0x50ec)
#define GMU_HFI_CTRL_STATUS     GMU_WORD(0x5180)
#define GMU_HFI_VERSION_INFO    GMU_WORD(0x5181)
#define GMU_GENERAL_7           GMU_WORD(0x51cc)
#define GMU_AO_HOST_INTERRUPT_STATUS GMU_WORD(0x9305)
#define GMU_RSCC_CONTROL_ACK    GMU_WORD(0x9308)
#define GMU_AO_GPU_CX_BUSY_STATUS GMU_WORD(0x930c)
#define GMU_AHB_FENCE_STATUS    GMU_WORD(0x9313)

/*
 * The stock dtb's "kgsl_3d0_reg_memory" at 0x5000000 is the 3D core, not to be touched without its
 * power -- but for its bus interface ("GBIF", words 0x3c00 on in a6xx.xml: all but the A630 have
 * one), which is in the CX half. "cx_misc" at 0x509e000: a page of odds and ends in the CX half.
 */
static struct block gbif = { "gbif", 0x05000000 + 4 * 0x3c00, 0x1000 };
#define GBIF_SCACHE_CNTL0       (4 * 0x01)
#define GBIF_HALT               (4 * 0x45)
#define GBIF_HALT_ACK           (4 * 0x46)
static struct block cx_misc = { "cx_misc", 0x0509e000, 0x1000 };
#define CX_MISC_SYSTEM_CACHE_CNTL_0 (4 * 1)
#define CX_MISC_SYSTEM_CACHE_CNTL_1 (4 * 2)

/* CX on and the GMU's clock running (what steps 4 and 5 leave): the GPU block's registers answer. */
static bool
cx_ready(void)
{
	uint32_t gmu_clock;

	if (!gpucc_answers()) {
		return false;
	}
	gmu_clock = rd(gpucc.base + GPUCC_CX_GMU_CBCR);
	if (!cx_is_on() || (gmu_clock & (CBCR_ENABLE | CBCR_OFF)) != CBCR_ENABLE) {
		printf("a618: %u: needs CX on (it is %s) and the GMU's clock running (cx_gmu %#x): steps 4 and 5\n", step,
		    cx_is_on() ? "on" : "off", gmu_clock);
		return false;
	}
	return true;
}

static bool
step_first_words(void)
{
	uint64_t start, nanos;
	uint32_t before, hz;

	if (!cx_ready()) {
		return false;
	}
	announce("read registers of the GMU (0x506a000): the first words from the GPU block itself");
	get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	get(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG);
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "cm3_cfg", GMU_CM3_CFG);
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	get(&gmu, "hfi_version_info", GMU_HFI_VERSION_INFO);
	get(&gmu, "general_7", GMU_GENERAL_7);
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "rscc_control_ack", GMU_RSCC_CONTROL_ACK);
	get(&gmu, "ao_gpu_cx_busy_status", GMU_AO_GPU_CX_BUSY_STATUS);
	get(&gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
	get(&gmu, "always_on_counter_h", GMU_ALWAYS_ON_COUNTER_H);
	get(&gmu, "always_on_counter_l", GMU_ALWAYS_ON_COUNTER_L);
	start = mach_absolute_time();
	before = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L);
	sleep_ms(10);
	hz = rd(gmu.base + GMU_ALWAYS_ON_COUNTER_L) - before;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	hz = nanos ? (uint32_t)((uint64_t)hz * NSEC_PER_SEC / nanos) : 0;
	printf("a618: %u: the GMU's counter counts at %u Hz: %s\n", step, hz, hz > 18000000 && hz < 20500000 ?
	    "the 19.2 MHz crystal: THE GPU BLOCK IS ALIVE" : "not the crystal's 19.2 MHz (firmware may start it): see the rest");
	for (uint32_t i = 0; i < 5; i++) {
		get(&gmu, "dtcm, where firmware has its versions", GMU_DTCM_VERSIONS + 4 * i);
	}
	announce("read the GPU's bus interface (0x500f000) and the CX odds and ends (0x509e000)");
	get(&gbif, "scache_cntl0", GBIF_SCACHE_CNTL0);
	get(&gbif, "halt", GBIF_HALT);
	get(&gbif, "halt_ack", GBIF_HALT_ACK);
	get(&cx_misc, "system_cache_cntl_0", CX_MISC_SYSTEM_CACHE_CNTL_0);
	get(&cx_misc, "system_cache_cntl_1", CX_MISC_SYSTEM_CACHE_CNTL_1);
	return true;
}

/* ---- step 7: the GMU's channel to RPMh ----------------------------------------------------------------- */

/*
 * The same kind of command slots through which this kernel asks RPMh for the panel's supplies, but
 * the GMU's own; through them its firmware raises the 3D core's rail. On GEN1 it is inside the GMU's
 * block, at +0x23000 (Linux a6xx_gmu.c: a block of its own, "rscc", only from the A650 family on).
 * Offsets in words: a6xx_gmu.xml, domain A6XX_RSCC.
 */
static struct block rscc = { "rscc", 0x0506a000 + 0x23000, 0x2000 };
#define RSCC_RSC_STATUS0_DRV0   (4 * 0x004)
#define RSCC_PDC_SLAVE_ID_DRV0  (4 * 0x00b)
#define RSCC_SEQ_BUSY_DRV0      (4 * 0x101)
#define RSCC_SEQ_MEM_0_DRV0     (4 * 0x180)
#define RSCC_TCS0_DRV0_STATUS   (4 * 0x346)

static bool
step_rscc(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("read the GMU's RPMh channel (0x508d000, inside the GMU's block on GEN1)");
	get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0);
	get(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0);
	get(&rscc, "seq_busy_drv0", RSCC_SEQ_BUSY_DRV0);
	get(&rscc, "seq_mem_0_drv0", RSCC_SEQ_MEM_0_DRV0);
	get(&rscc, "tcs0_drv0_status", RSCC_TCS0_DRV0_STATUS);
	get(&gmu, "rscc_control_ack", GMU_RSCC_CONTROL_ACK);
	return true;
}

/* ---- steps 8 and 9: the SMMU --------------------------------------------------------------------------- */

/*
 * The stock dtb's arm,smmu-kgsl@5040000 ("qcom,smmu-v2", 0x10000 long, 3-level tables), its streams
 * (gfx3d_user, gfx3d_secure, gmu_user, gmu_kernel): 0 the GPU, 2 secure content, 4 the GMU's user
 * and 5 its kernel context -- the GMU's firmware, queues and log buffers go through 5 (the Mi 10's
 * port of this driver: through 4 alone, the phone hung as the GMU left reset). Offsets: arm-smmu.h.
 */
static struct block smmu = { "smmu", 0x05040000, 0x10000 };
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020           /* [7:0] stream match registers */
#define SMMU_IDR1               0x024           /* bit 31: 64K pages, [30:28] log2(pages)-1, [7:0] translation contexts */
#define SMMU_IDR2               0x028
#define SMMU_IDR7               0x03c
#define SMMU_SGFSR              0x048
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))               /* in the second page; [17:16] kind of context */
#define SMMU_CB_SCTLR           0x00            /* a context's own page: bit 0 = translation on */
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TCR             0x30
#define SMMU_CB_FSR             0x58

static uint32_t smmu_page, smmu_pages, smmu_contexts, smmu_matches;

/* The layout of the SMMU's register space from its ID registers; false if it is not what the device tree says. */
static bool
smmu_layout(void)
{
	uint32_t idr0 = get(&smmu, "idr0", SMMU_IDR0), idr1 = get(&smmu, "idr1", SMMU_IDR1);

	smmu_page = idr1 >> 31 ? 0x10000 : 0x1000;
	smmu_pages = 2u << (idr1 >> 28 & 7);
	smmu_contexts = idr1 & 0xff;
	smmu_matches = idr0 & 0xff;
	printf("a618: %u: SMMU: %u stream match registers, %u contexts, %u pages of %#x before the contexts' own\n", step,
	    smmu_matches, smmu_contexts, smmu_pages, smmu_page);
	if (2 * smmu_pages * smmu_page != smmu.size) {
		printf("a618: %u: that makes %#x of registers, the device tree says %#x: not reading further\n", step,
		    2 * smmu_pages * smmu_page, smmu.size);
		return false;
	}
	return true;
}

static bool
step_smmu(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("read the ID registers of the GPU's SMMU (0x5040000): global registers, which the hypervisor may answer for");
	if (!smmu_layout()) {
		return false;
	}
	get(&smmu, "idr2", SMMU_IDR2);
	get(&smmu, "idr7", SMMU_IDR7);
	get(&smmu, "scr0", SMMU_SCR0);
	get(&smmu, "sgfsr", SMMU_SGFSR);
	announce("read the SMMU's stream table and the kinds of its contexts, as the boot chain left them");
	for (uint32_t i = 0; i < smmu_matches && i < 16; i++) {
		get(&smmu, "smr", SMMU_SMR(i));
		get(&smmu, "s2cr", SMMU_S2CR(i));
	}
	for (uint32_t i = 0; i < smmu_contexts && i < 16; i++) {
		get(&smmu, "cbar", smmu_page + SMMU_CBAR(i));
	}
	return true;
}

/* Only context 0, the GPU's own: of the others at least one belongs to TrustZone (secure content). */
static bool
step_smmu_context(void)
{
	uint32_t context;

	if (!cx_ready()) {
		return false;
	}
	announce("read translation context 0 of the SMMU");
	if (!smmu_layout()) {
		return false;
	}
	context = smmu_pages * smmu_page;
	get(&smmu, "cb0 sctlr", context + SMMU_CB_SCTLR);
	get(&smmu, "cb0 ttbr0, low", context + SMMU_CB_TTBR0);
	get(&smmu, "cb0 ttbr0, high", context + SMMU_CB_TTBR0 + 4);
	get(&smmu, "cb0 tcr", context + SMMU_CB_TCR);
	get(&smmu, "cb0 fsr", context + SMMU_CB_FSR);
	return true;
}

/* ---- step 10: the GMU's memory -------------------------------------------------------------------------- */

/*
 * Firmware is loaded by writing it through these registers into the processor's memory while the
 * processor is held in reset (a6xx_gmu_fw_load). Two words of data memory go there and back, and
 * what was there is put back.
 */
static bool
step_gmu_memory(void)
{
	static const uint32_t pattern[2] = { 0x600d0001, 0x0a110002 };
	uint32_t was[2], back[2];

	if (!cx_ready()) {
		return false;
	}
	announce("hold the GMU's processor in reset, write two words into its data memory (0x5071000), read them back");
	/* (the result stays 0xbabeface after a run of ours held the processor in reset again: only a running one counts) */
	if (get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT) == 0xbabeface &&
	    !(get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET) & 1)) {
		printf("a618: %u: firmware is running in the GMU: leaving its memory alone\n", step);
		return false;
	}
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	get(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET);
	for (int i = 0; i < 2; i++) {
		was[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, pattern[i]);
	}
	for (int i = 0; i < 2; i++) {
		back[i] = get(&gmu, "dtcm", GMU_DTCM + 4 * i);
	}
	for (int i = 0; i < 2; i++) {
		put(&gmu, "dtcm", GMU_DTCM + 4 * i, was[i]);
	}
	printf("a618: %u: the GMU's memory %s\n", step, back[0] == pattern[0] && back[1] == pattern[1] ?
	    "TAKES WHAT IS WRITTEN: its firmware can be loaded" : "does not hold what was written");
	return back[0] == pattern[0] && back[1] == pattern[1];
}

/* ---- step 11: TrustZone -------------------------------------------------------------------------------- */

/*
 * The 3D core comes out of reset in "secure" mode, and only TrustZone lets it out: it does when
 * handed the vendor's signed zap shader to authenticate (a6xx_gpu.c, qcom_scm.c qcom_scm_pas_*; the
 * GPU is image 13). Questions that pass no memory show whether it listens to this kernel at all; a
 * call is an SMC, which the hypervisor passes on (qcom_scm-smc.c). As hakios.adreno.c asks them.
 */
#define SCM_FAST                (1ull << 31)            /* answered at once; without it a call may be preempted */
#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)            /* a call of the SoC maker's */
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_IS_CALL_AVAILABLE   SCM_CALL(6, 1)
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_IS_SUPPORTED    SCM_CALL(2, 7)
#define SCM_ONE_VALUE           1                       /* the word describing the arguments: one, a plain number */
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)            /* a number, and the address of memory it may read and write */
#define SCM_INTERRUPTED         1                       /* a call that was preempted says so, and is made again so */
#define PAS_GPU                 13

/* One call. What TrustZone returns: 0 or an error of its own, and a word of answer. */
static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t *answer)
{
	uint64_t session = 0;

	printf("a618: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	hold();
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
			printf("a618: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	printf("a618: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static uint64_t scm_how = SCM_FAST | SCM_64 | SCM_SIP;  /* the convention TrustZone answered to (step 11) */

static bool
step_trustzone(void)
{
	/* the stock dtb's qcom,pas-id of the audio, modem and video firmware, for comparison */
	static const struct {
		const char *name;
		uint32_t id;
	} images[] = { { "audio", 1 }, { "modem", 4 }, { "video", 9 }, { "GPU zap shader", PAS_GPU } };
	uint64_t answer = 0;
	uint32_t fast[4] = { 0 }, slow[4] = { 0 };

	announce("ask TrustZone what Linux asks it first: is the call \"is this call available\" available");
	if (scm("whether it can be asked", scm_how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE, SCM_SIP | SCM_IS_CALL_AVAILABLE,
	    0, 0, &answer) != 0 || answer != 1) {
		scm_how = SCM_FAST | SCM_SIP;   /* the same with 32-bit arguments, Linux's second try */
		if (scm("the same, the 32-bit way", scm_how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
		    SCM_SIP | SCM_IS_CALL_AVAILABLE, 0, 0, &answer) != 0 || answer != 1) {
			printf("a618: %u: TrustZone does not answer the way it answers Linux\n", step);
			return false;
		}
	}
	announce("ask TrustZone whether it has the calls that authenticate a firmware image");
	scm("for the call that takes an image's signature", scm_how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
	    SCM_SIP | SCM_PAS_INIT_IMAGE, 0, 0, &answer);
	scm("for the call that authenticates and starts an image", scm_how | SCM_IS_CALL_AVAILABLE, SCM_ONE_VALUE,
	    SCM_SIP | SCM_PAS_AUTH_AND_RESET, 0, 0, &answer);
	announce("ask TrustZone which images it \"supports\": three this phone starts through it, and the GPU's");
	for (unsigned i = 0; i < 4; i++) {
		answer = 0;
		fast[i] = scm(images[i].name, scm_how | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, images[i].id, 0, 0, &answer) == 0 &&
		    answer;
		answer = 0;
		slow[i] = scm(images[i].name, (scm_how & ~SCM_FAST) | SCM_PAS_IS_SUPPORTED, SCM_ONE_VALUE, images[i].id, 0, 0,
		    &answer) == 0 && answer;
	}
	printf("a618: %u: \"supported\", asked at once / as Linux asks: audio %u/%u, modem %u/%u, video %u/%u, GPU %u/%u\n",
	    step, fast[0], slow[0], fast[1], slow[1], fast[2], slow[2], fast[3], slow[3]);
	return true;
}

/* ---- step 12: the zap shader's signature ----------------------------------------------------------------- */

/* A file of the small image that travels with the kernel (fw-gpu/ goes into its fw/; drivers/storage/hakios.image.c finds it). */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

/*
 * The first of the three calls with which Linux has a zap shader loaded (mdt_loader.c
 * qcom_mdt_pas_init, qcom_scm_pas_init_image): TrustZone is given the image's header -- the ELF
 * header and the block of hashes and certificates, all of a615_zap.mdt -- and checks the signature.
 * Nothing of the GPU is involved. The header goes in a page of ours, which TrustZone protects while
 * it works (the hypervisor arranges its access): so it is not touched again, and never freed.
 */
static bool
zap_header(uint64_t *phys)
{
	const uint8_t *header;
	vm_offset_t page = 0;
	uint64_t size = 0;
	uint32_t segments, hashes;
	int disk = -1;

	header = hakios_image_file("fw", "a615_zap.mdt", &size, &disk);
	if (header == NULL) {
		printf("a618: %u: no fw/a615_zap.mdt in the small image: put the phone's /vendor/firmware/a615_zap.* into "
		    "fw-gpu/ and run userland/build.sh\n", step);
		return false;
	}
	printf("a618: %u: fw/a615_zap.mdt: %llu bytes in the image of /dev/md%d\n", step, size, disk);
	/* a 32-bit ELF: the count of segments at 44, the table of them at 52, 32 bytes each, the file size of one at +16 */
	segments = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || segments < 2 || size > PAGE_SIZE) {
		printf("a618: %u: that is not such a header (%02x %02x %02x %02x, %u segments)\n", step, header[0], header[1],
		    header[2], header[3], segments);
		return false;
	}
	memcpy(&hashes, header + 52 + 32 + 16, sizeof(hashes));
	printf("a618: %u: an ELF header of %u segments, %u bytes with its table, then %llu bytes; by the table, %u of hashes\n",
	    step, segments, 52 + 32 * segments, size - (52 + 32 * segments), hashes);
	if (52 + 32 * segments + (uint64_t)hashes != size) {
		printf("a618: %u: the file is not the header followed by the hashes alone: not what TrustZone takes\n", step);
		return false;
	}
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("a618: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, header, size);
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));      /* TrustZone reads memory, not our cache */
	}
	__asm__ volatile ("dsb sy");
	*phys = kvtophys(page);
	return true;
}

static bool
step_zap_signature(void)
{
	uint64_t header = 0, answer = ~0ull;
	int32_t result;

	announce("find the zap shader's header, fw/a615_zap.mdt, in the small image that came with the kernel");
	if (!zap_header(&header)) {
		return false;
	}
	announce("hand the header to TrustZone to check its signature (image 13); a call that may be preempted, as Linux's is");
	result = scm("to take the zap shader's signature", (scm_how & ~SCM_FAST) | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER,
	    PAS_GPU, header, 0, &answer);
	if (result == 0 && answer == 0) {
		printf("a618: %u: TRUSTZONE ACCEPTS THE ZAP SHADER'S SIGNATURE FROM THIS KERNEL\n", step);
		return true;
	}
	printf("a618: %u: TrustZone does not take it: the call returns %d, its answer is %#llx\n", step, result, answer);
	return false;
}

/* ---- memory the GPU block reaches (hakios.adreno.c's, unchanged: the same tables, 3 levels of 4K pages) ---- */

#define PTE_TABLE               3ull
#define PTE_PAGE                3ull
#define PTE_CACHED              (1ull << 2)     /* memory attribute 1 of SMMU_MAIR0; without it 0, uncached */
#define PTE_UNPRIVILEGED        (1ull << 6)     /* the 3D core's accesses are; the GMU's are not */
#define PTE_SHARED              (2ull << 8)
#define PTE_ACCESSED            (1ull << 10)
#define PTE_NOT_GLOBAL          (1ull << 11)
#define PTE_ADDRESS             0x0000fffffffff000ull
#define PTE_GMU                 (PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL)

#define SMMU_S2CR_FAULT         (2u << 16)
#define SMMU_SMR_VALID          (1u << 31)
#define SMMU_STREAM_MASK        0               /* the stock dtb's GPU streams have no mask cell (#iommu-cells 1), and
						 * this SMMU has 3 bits of stream ID (idr0 0x48016605): redwood's 0x400
						 * made the SMR read back 0 */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)       /* our tables alone decide; weakest defaults */
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))                       /* in the second page, like CBAR */
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12) /* on; faults recorded, no interrupt */
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)   /* one table of 4K pages, cached walks */
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)       /* 36 bits of physical address, 16 of context tag */
#define SMMU_MAIR0              0xf404ff44      /* Linux's: 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL            0xc00001fe      /* every kind of fault, and "several" */


/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

/* Push what this processor has written to a stretch of memory out of its cache, and forget it: the GPU block reads memory. */
static void
to_memory(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
}

#define SPACE_TABLES            1024            /* 2 GB of addresses, were they all in use */

struct gpu_space {
	const char *name;
	uint32_t context;               /* which of the SMMU's */
	uint32_t control;               /* bits of its control register besides the usual */
	uint32_t table_count;
	uint64_t *root;
	struct {
		uint64_t phys;
		uint64_t *table;
		bool changed;           /* since it was last written out of this processor's cache (space_commit()) */
	} tables[SPACE_TABLES];
	uint64_t *last_entry, *last_table;      /* the level 2 entry looked up last, and its table */
	uint32_t last_changed;                  /* the table an entry was written in last */
};

static uint64_t *
space_new_table(struct gpu_space *space)
{
	static vm_offset_t spare;
	static unsigned spare_left;
	uint64_t *table;

	if (space->table_count == SPACE_TABLES) {
		return NULL;
	}
	if (spare_left == 0) {
		if (kmem_alloc(kernel_map, &spare, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
		    KERN_SUCCESS) {
			return NULL;
		}
		spare_left = PAGE_SIZE / 4096;
	}
	table = (uint64_t *)(spare + PAGE_SIZE - 4096 * spare_left--);
	space->tables[space->table_count].phys = kvtophys((vm_offset_t)table);
	space->tables[space->table_count++].table = table;
	return table;
}

/* The table an entry of a higher table leads to; made if the entry is empty. */
/* An entry of one of the tables has been written: that table is to go out to memory. */
static void
space_changed(struct gpu_space *space, const uint64_t *entry)
{
	if ((uintptr_t)entry - (uintptr_t)space->tables[space->last_changed].table < 4096) {
		space->tables[space->last_changed].changed = true;      /* page after page of one buffer */
		return;
	}
	for (uint32_t i = space->table_count; i-- > 0;) {
		if ((uintptr_t)entry - (uintptr_t)space->tables[i].table < 4096) {
			space->tables[i].changed = true;
			space->last_changed = i;
			return;
		}
	}
}

/* The SMMU reads the tables from memory, not from this processor's cache. */
static void
space_commit(struct gpu_space *space)
{
	for (uint32_t i = 0; i < space->table_count; i++) {
		if (space->tables[i].changed) {
			to_memory((vm_offset_t)space->tables[i].table, 4096);
			space->tables[i].changed = false;
		}
	}
}

static uint64_t *
space_next(struct gpu_space *space, uint64_t *entry)
{
	uint64_t *table;

	if (*entry & 1) {
		for (uint32_t i = 0; i < space->table_count; i++) {
			if (space->tables[i].phys == (*entry & PTE_ADDRESS)) {
				return space->tables[i].table;
			}
		}
		return NULL;
	}
	table = space_new_table(space);
	if (table != NULL) {
		*entry = kvtophys((vm_offset_t)table) | PTE_TABLE;
		space_changed(space, entry);
	}
	return table;
}

/* One 4K page. */
static bool
space_map(struct gpu_space *space, uint64_t address, uint64_t phys, uint64_t attributes)
{
	uint64_t *level2, *level3;

	if (address >> 39 || (space->root == NULL && (space->root = space_new_table(space)) == NULL)) {
		return false;
	}
	level2 = space_next(space, &space->root[address >> 30 & 0x1ff]);
	if (level2 == NULL) {
		return false;
	}
	if (space->last_entry == &level2[address >> 21 & 0x1ff]) {
		level3 = space->last_table;             /* page after page of one buffer: the same table */
	} else {
		level3 = space_next(space, &level2[address >> 21 & 0x1ff]);
		space->last_entry = &level2[address >> 21 & 0x1ff];
		space->last_table = level3;
	}
	if (level3 == NULL) {
		space->last_entry = NULL;
		return false;
	}
	level3[address >> 12 & 0x1ff] = phys == 0 && attributes == 0 ? 0 : phys | attributes | PTE_PAGE;
	space_changed(space, level3);
	return true;
}

/* Memory of ours that the GPU block sees at an address of its own. */
struct gpu_block {
	uint64_t address;               /* the GPU's */
	uint32_t size;
	vm_offset_t memory;             /* ours */
};

static bool
block_new(struct gpu_space *space, struct gpu_block *block, uint64_t address, uint32_t size, uint64_t attributes)
{
	block->address = address;
	block->size = (size + 4095) & ~4095u;
	if (kmem_alloc(kernel_map, &block->memory, round_page(block->size), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	for (uint32_t offset = 0; offset < block->size; offset += 4096) {
		if (!space_map(space, address + offset, kvtophys(block->memory + offset), attributes)) {
			return false;
		}
	}
	to_memory(block->memory, block->size);
	printf("a618: %u: %s sees %#llx..%#llx in memory of ours (%#llx)\n", step, space->name, address,
	    address + block->size - 1, kvtophys(block->memory));
	return true;
}

/*
 * A second view of a block without this processor's cache in between, for memory that both
 * sides write a word at a time. Only blocks of one page of ours: those are in one piece.
 */
static vm_offset_t
block_uncached(const struct gpu_block *block)
{
	uint64_t phys = kvtophys(block->memory);

	for (uint32_t offset = 0; offset < block->size; offset += 4096) {
		if (kvtophys(block->memory + offset) != phys + offset) {
			return 0;
		}
	}
	return ml_io_map_wcomb(phys, block->size);
}

static uint32_t
smmu_context(const struct gpu_space *space)
{
	return (smmu_pages + space->context) * smmu_page;
}

/* What a context has recorded of a request it could not serve. */
static bool
space_faulted(const struct gpu_space *space)
{
	uint32_t context = smmu_context(space), status = rd(smmu.base + context + SMMU_CB_FSR);

	if ((status & SMMU_FSR_ALL) == 0) {             /* bits 10:9 only tell the format of the syndrome */
		return false;
	}
	printf("a618: %u: THE SMMU REFUSED %s: status %#x, address %#llx, syndrome %#x\n", step, space->name, status,
	    (uint64_t)rd(smmu.base + context + SMMU_CB_FAR + 4) << 32 | rd(smmu.base + context + SMMU_CB_FAR),
	    rd(smmu.base + context + SMMU_CB_FSYNR0));
	return true;
}

/*
 * One more stream through a context that is on. A match is taken only as the device tree has
 * it, a stream and the mask 0x400: one entry for streams 0 and 1 (mask 0x401) reads back 0.
 */
static bool
space_stream(struct gpu_space *space, uint32_t match, uint32_t stream)
{
	put(&smmu, "s2cr", SMMU_S2CR(match), space->context);
	put(&smmu, "smr", SMMU_SMR(match), SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream);
	if (get(&smmu, "smr", SMMU_SMR(match)) != (SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream) ||
	    (get(&smmu, "s2cr", SMMU_S2CR(match)) & 0x300ff) != space->context) {
		printf("a618: %u: the SMMU did not take what was written: stream %u is not going through context %u\n", step,
		    stream, space->context);
		return false;
	}
	return true;
}

/* Give a context our tables and send a stream through it; `match` is the entry of the stream table to use. */
static bool
space_on(struct gpu_space *space, uint32_t match, uint32_t stream)
{
	uint32_t context = smmu_context(space);
	uint64_t root = space->tables[0].phys | (uint64_t)space->context << 48;        /* tagged with the context's number */

	for (uint32_t i = 0; i < space->table_count; i++) {
		to_memory((vm_offset_t)space->tables[i].table, 4096);
	}
	put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, 0);
	put(&smmu, "cba2r", smmu_page + SMMU_CBA2R(space->context), SMMU_CBA2R_64_BIT);
	put(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context), SMMU_CBAR_STAGE_1);
	put(&smmu, "cb tcr2", context + SMMU_CB_TCR2, SMMU_TCR2);
	put(&smmu, "cb tcr", context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	put(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0, (uint32_t)root);
	put(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	put(&smmu, "cb ttbr1, low", context + SMMU_CB_TTBR1, 0);
	put(&smmu, "cb ttbr1, high", context + SMMU_CB_TTBR1 + 4, 0);
	put(&smmu, "cb mair0", context + SMMU_CB_MAIR0, SMMU_MAIR0);
	put(&smmu, "cb mair1", context + SMMU_CB_MAIR1, 0);
	put(&smmu, "cb fsr", context + SMMU_CB_FSR, SMMU_FSR_ALL);
	put(&smmu, "cb tlbiasid", context + SMMU_CB_TLBIASID, space->context);
	put(&smmu, "cb tlbsync", context + SMMU_CB_TLBSYNC, 1);
	if (!wait_for(&smmu, context + SMMU_CB_TLBSTATUS, 1, 0, 100)) {
		printf("a618: %u: the SMMU does not finish forgetting old translations (%#x)\n", step,
		    rd(smmu.base + context + SMMU_CB_TLBSTATUS));
		return false;
	}
	put(&smmu, "cb sctlr", context + SMMU_CB_SCTLR, SMMU_SCTLR_ON | space->control);
	put(&smmu, "s2cr", SMMU_S2CR(match), space->context);
	put(&smmu, "smr", SMMU_SMR(match), SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream);

	/* the global ones are the hypervisor's to grant: see what it made of them */
	if (get(&smmu, "smr", SMMU_SMR(match)) != (SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | stream) ||
	    (get(&smmu, "s2cr", SMMU_S2CR(match)) & 0x300ff) != space->context ||
	    (get(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context)) & 0x30000) != (SMMU_CBAR_STAGE_1 & 0x30000) ||
	    !(get(&smmu, "cb sctlr", context + SMMU_CB_SCTLR) & 1) ||
	    get(&smmu, "cb ttbr0, low", context + SMMU_CB_TTBR0) != (uint32_t)root ||
	    get(&smmu, "cb ttbr0, high", context + SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32)) {
		printf("a618: %u: the SMMU did not take what was written: stream %u is not going through context %u\n", step,
		    stream, space->context);
		return false;
	}
	get(&smmu, "cb tcr", context + SMMU_CB_TCR);
	get(&smmu, "cba2r", smmu_page + SMMU_CBA2R(space->context));
	get(&smmu, "cb tcr2", context + SMMU_CB_TCR2);
	get(&smmu, "cb fsr", context + SMMU_CB_FSR);
	get(&smmu, "sgfsr", SMMU_SGFSR);
	return true;
}

/* A stream goes nowhere again, and its context translates nothing. */
static void
space_off(struct gpu_space *space, uint32_t match)
{
	put(&smmu, "smr", SMMU_SMR(match), 0);
	put(&smmu, "s2cr", SMMU_S2CR(match), SMMU_S2CR_FAULT);
	put(&smmu, "cb sctlr", smmu_context(space) + SMMU_CB_SCTLR, 0);
}


/* ---- step 13: memory for the GMU ------------------------------------------------------------------------ */

/*
 * On GEN1 the GMU's firmware asks for no memory of its own ("legacy": a6xx_gmu_fw_load writes all of
 * it into the GMU's code and data memory); Linux gives it, in the GMU's uncached range from
 * 0x60000000 (a6xx_gmu_init, a6xx_gmu_memory_alloc, for the A630 family): a dummy page at 0x60000000,
 * then 16K for debugging, 16K for its log and 16K for the table and queues of its message protocol
 * ("HFI"), in that order. The firmware is told where the log and the queues are (step 15).
 */
#define GMU_DUMMY_ADDRESS       0x60000000u
#define GMU_DEBUG_ADDRESS       0x60001000u
#define GMU_LOG_ADDRESS         0x60005000u
#define GMU_HFI_ADDRESS         0x60009000u
#define GMU_REGION_SIZE         0x4000u
#define GMU_STREAM              5               /* the stock dtb's gmu_kernel */
#define GMU_MATCH               1               /* entry of the stream table; 0 is kept for the 3D core */

static struct gpu_space gmu_space = { .name = "the GMU", .context = 1 };
static struct gpu_block gmu_dummy, gmu_debug, gmu_log, gmu_hfi;
static bool gmu_space_is_on;

static bool
step_gmu_space(void)
{
	if (!cx_ready()) {
		return false;
	}
	announce("make the memory the GMU needs: a dummy page, debugging, its log, its message queues");
	/* (4K: this kernel's PAGE_SIZE is 16K, and a page of that size ran into the debugging region) */
	if (!block_new(&gmu_space, &gmu_dummy, GMU_DUMMY_ADDRESS, 0x1000, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_debug, GMU_DEBUG_ADDRESS, GMU_REGION_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_log, GMU_LOG_ADDRESS, GMU_REGION_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_hfi, GMU_HFI_ADDRESS, GMU_REGION_SIZE, PTE_GMU)) {
		printf("a618: %u: no memory for the GMU\n", step);
		return false;
	}
	announce("give the SMMU's context 1 these tables and send the GMU's requests (stream 5) through it: the first "
	    "writes to the SMMU");
	if (!smmu_layout()) {
		return false;
	}
	gmu_space_is_on = true;         /* whatever of it has been written by the time something fails is taken back */
	return space_on(&gmu_space, GMU_MATCH, GMU_STREAM);
}

/* ---- step 14: the GMU's firmware ----------------------------------------------------------------------- */

#define GMU_ITCM                GMU_WORD(0x0c00)        /* REG_A6XX_GMU_CM3_ITCM_START: code, then data, 32K in all */
#define GMU_LEGACY_FIRMWARE_MAX 0x8000u

static const uint8_t *gmu_firmware;
static uint64_t gmu_firmware_size;

static bool gmu_runs;

static bool
step_gmu_firmware(void)
{
	uint32_t wrong = 0;
	int disk = -1;

	if (!cx_ready()) {
		return false;
	}
	announce("find the GMU's firmware, fw/a630_gmu.bin, in the small image");
	gmu_firmware = hakios_image_file("fw", "a630_gmu.bin", &gmu_firmware_size, &disk);
	if (gmu_firmware == NULL) {
		printf("a618: %u: no fw/a630_gmu.bin in the small image: put the phone's /vendor/firmware/a630_gmu.bin into "
		    "fw-gpu/ and run userland/build.sh\n", step);
		return false;
	}
	printf("a618: %u: fw/a630_gmu.bin: %llu bytes in the image of /dev/md%d\n", step, gmu_firmware_size, disk);
	if (gmu_firmware_size > GMU_LEGACY_FIRMWARE_MAX || (gmu_firmware_size & 3)) {
		printf("a618: %u: more than the GMU's 32K, or not whole words: not a legacy image\n", step);
		return false;
	}
	announce("hold the GMU's processor in reset and write the firmware into its memory (from 0x506d000), then read it back");
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	for (uint64_t offset = 0; offset < gmu_firmware_size; offset += 4) {
		uint32_t word;

		memcpy(&word, gmu_firmware + offset, sizeof(word));
		wr(gmu.base + GMU_ITCM + offset, word);
	}
	for (uint64_t offset = 0; offset < gmu_firmware_size; offset += 4) {
		uint32_t word;

		memcpy(&word, gmu_firmware + offset, sizeof(word));
		wrong += rd(gmu.base + GMU_ITCM + offset) != word;
	}
	if (wrong) {
		printf("a618: %u: %u words of the GMU's own memory do not read back as written\n", step, wrong);
		return false;
	}
	printf("a618: %u: THE GMU'S FIRMWARE IS LOADED (%llu bytes); the words where firmware has its versions: %#x %#x %#x\n",
	    step, gmu_firmware_size, rd(gmu.base + GMU_DTCM_VERSIONS), rd(gmu.base + GMU_DTCM_VERSIONS + 8),
	    rd(gmu.base + GMU_DTCM_VERSIONS + 16));
	return true;
}

/* ---- step 15: start the GMU ---------------------------------------------------------------------------- */

/* GMU registers (a6xx_gmu.xml: from the GPU's base, in words; less 0x1a800 from the GMU's) */
#define GMU_ICACHE_CONFIG       GMU_WORD(0x4c00)
#define GMU_DCACHE_CONFIG       GMU_WORD(0x4c01)
#define GMU_SYS_BUS_CONFIG      GMU_WORD(0x4c0f)
#define GMU_PWR_COL_INTER_FRAME_CTRL GMU_WORD(0x50c0)
#define GMU_RPMH_CTRL           GMU_WORD(0x50e8)
#define GMU_RPMH_CTRL_ALL       0xf11           /* the RPMh interface; LLC, DDR, MX, CX and GFX votes */
#define GMU_PWR_COL_CP_MSG      GMU_WORD(0x5100)        /* at start: where the log is, and its pages - 1 */
#define GMU_PWR_COL_CP_RESP     GMU_WORD(0x5101)        /* ... and the log's write index */
#define GMU_HFI_SFR_ADDR        GMU_WORD(0x5182)        /* at start: which GPU this is */
#define GMU_HFI_QTBL_INFO       GMU_WORD(0x5184)
#define GMU_HFI_QTBL_ADDR       GMU_WORD(0x5185)
#define GMU_RSCC_CONTROL_REQ    GMU_WORD(0x9307)
#define GMU_AHB_FENCE_RANGE_0   GMU_WORD(0x9311)
#define GMU_CHIP                0x06018000      /* the A618's chip id 0x06010800 the way the GMU wants it (a6xx_gmu_fw_start) */
#define GMU_STARTED             0xbabeface      /* fw_init_result, for firmware with no version where newer ones have it */

/* The RSC's own registers (domain A6XX_RSCC, words, from 0x508d000) */
#define RSCC_PDC_SEQ_START_ADDR (4 * 0x008)
#define RSCC_PDC_MATCH_VALUE_LO (4 * 0x009)
#define RSCC_PDC_MATCH_VALUE_HI (4 * 0x00a)
#define RSCC_HIDDEN_TCS_CMD0_ADDR (4 * 0x00d)
#define RSCC_HIDDEN_TCS_CMD0_DATA (4 * 0x00e)
#define RSCC_OVERRIDE_START_ADDR (4 * 0x100)

/*
 * The PDC: the stock dtb's qcom,gmu "kgsl_gmu_pdc_cfg" (0xb290000) and "kgsl_gmu_pdc_seq" (0xb490000);
 * on GEN1 its sequence and its commands are ours to write (on the A650 family the AOP's: redwood's
 * driver had none of this). Offsets in words, domain A6XX_PDC (a6xx.xml).
 */
static struct block pdc = { "pdc", 0x0b290000, 0x6000 };
static struct block pdc_seq = { "pdc seq", 0x0b490000, 0x1000 };
#define PDC_ENABLE              (4 * 0x1140)
#define PDC_SEQ_START_ADDR      (4 * 0x1148)
#define PDC_TCS1_CONTROL        (4 * 0x1572)
#define PDC_TCS1_CMD_ENABLE     (4 * 0x1573)
#define PDC_TCS1_CMD_WAIT       (4 * 0x1574)
#define PDC_TCS1_CMD0           (4 * 0x1575)    /* +0 message id, +4 address, +8 data; 16 bytes a command */
#define PDC_TCS3_CONTROL        (4 * 0x15d6)
#define PDC_TCS3_CMD_ENABLE     (4 * 0x15d7)
#define PDC_TCS3_CMD_WAIT       (4 * 0x15d8)
#define PDC_TCS3_CMD0           (4 * 0x15d9)
#define PDC_A618_ADDRESS        0x30090         /* a6xx_gmu_rpmh_init: the A618's (and the 640 family's) */

/* The table at the head of the HFI region and its two queues (a6xx_hfi.h, a6xx_hfi_init) */
struct hfi_queue_header {
	uint32_t status, iova, type, size, msg_size, dropped, rx_watermark, tx_watermark, rx_request, tx_request;
	uint32_t read_index, write_index;
};
struct hfi_table_header {
	uint32_t version, size, qhdr0_offset, qhdr_size, num_queues, active_queues;
};
#define HFI_QUEUES              2               /* 0: ours to the GMU; 1: its answers */
#define HFI_RESPONSE_ID_LEGACY  4               /* the answers' queue id on GEN1 ("legacy"; 1 later) */

static volatile uint8_t *hfi;           /* the uncached view of the HFI region */

static void
hfi_table(void)
{
	volatile struct hfi_table_header *table = (volatile struct hfi_table_header *)hfi;
	volatile struct hfi_queue_header *queue = (volatile struct hfi_queue_header *)(hfi + sizeof(*table));

	table->version = 0;
	table->size = sizeof(struct hfi_table_header) + HFI_QUEUES * sizeof(struct hfi_queue_header);
	table->qhdr0_offset = sizeof(struct hfi_table_header) / 4;
	table->qhdr_size = sizeof(struct hfi_queue_header) / 4;
	table->num_queues = HFI_QUEUES;
	table->active_queues = HFI_QUEUES;
	for (uint32_t n = 0; n < HFI_QUEUES; n++) {
		queue[n].iova = GMU_HFI_ADDRESS + 0x1000 * (n + 1);
		queue[n].type = 10 << 8 | (n == 0 ? 0 : HFI_RESPONSE_ID_LEGACY);
		queue[n].status = 1;
		queue[n].size = 0x1000 / 4;
		queue[n].msg_size = 0;
		queue[n].dropped = 0;
		queue[n].rx_watermark = 1;
		queue[n].tx_watermark = 1;
		queue[n].rx_request = 1;
		queue[n].tx_request = 0;
		queue[n].read_index = 0;
		queue[n].write_index = 0;
	}
	__asm__ volatile ("dsb sy");
}

/* a6xx_gmu_rpmh_init, as it is for the A618 */
static void
rpmh_sequences(void)
{
	static const uint32_t rscc_seq[5] = { 0xa7a506a0, 0xa1e6a6e7, 0xa2e081e1, 0xe9a982e2, 0x0020e8a8 };
	static const uint32_t pdc_code[5] = { 0xfebea1e1, 0xa5a4a3a2, 0x8382a6e0, 0xbce3e284, 0x002081fc };
	static const struct {
		uint32_t control, enable, wait, cmd0, data[3];
	} tcs[2] = {
		{ PDC_TCS1_CONTROL, PDC_TCS1_CMD_ENABLE, PDC_TCS1_CMD_WAIT, PDC_TCS1_CMD0, { 1, 0x0, 0x0 } },
		{ PDC_TCS3_CONTROL, PDC_TCS3_CMD_ENABLE, PDC_TCS3_CMD_WAIT, PDC_TCS3_CMD0, { 2, 0x2, 0x3 } },
	};
	static const uint32_t address[3] = { 0x30010, 0x30000, PDC_A618_ADDRESS };

	put(&rscc, "rsc_status0_drv0 (no clock gating)", RSCC_RSC_STATUS0_DRV0, 1u << 24);
	put(&rscc, "pdc_slave_id_drv0", RSCC_PDC_SLAVE_ID_DRV0, 1);
	for (uint32_t i = 0; i < 3; i++) {
		put(&rscc, "hidden tcs data", RSCC_HIDDEN_TCS_CMD0_DATA + 8 * i, i == 2 ? 0x80000000 : 0);
		put(&rscc, "hidden tcs address", RSCC_HIDDEN_TCS_CMD0_ADDR + 8 * i, 0);
	}
	put(&rscc, "override_start_addr", RSCC_OVERRIDE_START_ADDR, 0);
	put(&rscc, "pdc_seq_start_addr", RSCC_PDC_SEQ_START_ADDR, 0x4520);
	put(&rscc, "pdc_match_value_lo", RSCC_PDC_MATCH_VALUE_LO, 0x4510);
	put(&rscc, "pdc_match_value_hi", RSCC_PDC_MATCH_VALUE_HI, 0x4514);
	for (uint32_t i = 0; i < 5; i++) {
		put(&rscc, "seq_mem_drv0", RSCC_SEQ_MEM_0_DRV0 + 4 * i, rscc_seq[i]);
	}
	for (uint32_t i = 0; i < 5; i++) {
		put(&pdc_seq, "seq_mem", 4 * i, pdc_code[i]);
	}
	for (uint32_t t = 0; t < 2; t++) {
		put(&pdc, "tcs cmd_enable_bank", tcs[t].enable, 7);
		put(&pdc, "tcs cmd_wait_for_cmpl_bank", tcs[t].wait, 0);
		put(&pdc, "tcs control", tcs[t].control, 0);
		for (uint32_t c = 0; c < 3; c++) {
			put(&pdc, "tcs cmd msgid", tcs[t].cmd0 + 16 * c, 0x10108);
			put(&pdc, "tcs cmd addr", tcs[t].cmd0 + 16 * c + 4, address[c]);
			put(&pdc, "tcs cmd data", tcs[t].cmd0 + 16 * c + 8, tcs[t].data[c]);
		}
	}
	put(&pdc, "seq_start_addr", PDC_SEQ_START_ADDR, 0);
	put(&pdc, "enable_pdc", PDC_ENABLE, 0x80000001);
	__asm__ volatile ("dsb sy");
}

/*
 * a6xx_rpmh_start: the GMU's RSC awake. Only if it is asleep (Linux: only after a6xx_rpmh_stop has
 * put it there; a first start skips this, and asked anyway the RSC does not answer).
 */
#define RSCC_ASLEEP             (1u << 16)      /* of rsc_status0_drv0 */

static bool
rsc_wake(void)
{
	if (!(get(&rscc, "rsc_status0_drv0", RSCC_RSC_STATUS0_DRV0) & RSCC_ASLEEP)) {
		return true;
	}
	put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 1u << 1);
	if (!wait_for(&gmu, GMU_RSCC_CONTROL_ACK, 1u << 1, 1u << 1, 10)) {
		printf("a618: %u: the GMU's RSC does not answer (control ack %#x)\n", step, rd(gmu.base + GMU_RSCC_CONTROL_ACK));
		return false;
	}
	if (!wait_for(&rscc, RSCC_SEQ_BUSY_DRV0, ~0u, 0, 10)) {
		printf("a618: %u: the RSC's sequence stays busy (%#x)\n", step, rd(rscc.base + RSCC_SEQ_BUSY_DRV0));
		return false;
	}
	put(&gmu, "rscc_control_req", GMU_RSCC_CONTROL_REQ, 0);
	return true;
}

/* a6xx_gmu_fw_start, a6xx_gmu_power_config and a6xx_gmu_start, as they are for the A618 */
static bool
step_gmu_start(void)
{
	uint32_t result = 0;

	if (!cx_ready() || !gmu_space_is_on || gmu_firmware == NULL) {
		printf("a618: %u: needs CX, the GMU's memory and its firmware: steps 4, 5, 13 and 14\n", step);
		return false;
	}
	announce("the GMU's clock's recommended wake and sleep times (Xiaomi's gpucc-sdmmagpie.c), and the HFI table");
	put(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR, (rd(gpucc.base + GPUCC_CX_GMU_CBCR) & ~0xff0u) | 0xff0u);
	get(&gpucc, "cx_gmu", GPUCC_CX_GMU_CBCR);
	hfi = (volatile uint8_t *)block_uncached(&gmu_hfi);
	if (hfi == NULL) {
		printf("a618: %u: the HFI region is not in one piece\n", step);
		return false;
	}
	hfi_table();

	announce("write the GMU's RSC's and the PDC's sequences (on GEN1 the host's to write)");
	rpmh_sequences();

	announce("the GMU's memory kept, its RSC awake, and its configuration");
	put(&gmu, "general_7 (tcm retention)", GMU_GENERAL_7, 1);
	if (!rsc_wake()) {
		return false;
	}
	put(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT, 0);
	put(&gmu, "cm3_boot_config", GMU_CM3_BOOT_CONFIG, 0x02);
	put(&gmu, "hfi_qtbl_addr", GMU_HFI_QTBL_ADDR, GMU_HFI_ADDRESS);
	put(&gmu, "hfi_qtbl_info", GMU_HFI_QTBL_INFO, 1);
	put(&gmu, "ahb_fence_range_0", GMU_AHB_FENCE_RANGE_0, 1u << 31 | 0xau << 18 | 0xa0);
	put(&gmu, "cm3_cfg", GMU_CM3_CFG, 0x4052);
	put(&gmu, "hfi_sfr_addr (the chip)", GMU_HFI_SFR_ADDR, GMU_CHIP);
	put(&gmu, "pwr_col_cp_msg (the log)", GMU_PWR_COL_CP_MSG, GMU_LOG_ADDRESS | (GMU_REGION_SIZE / 0x1000 - 1));
	put(&gmu, "sys_bus_config", GMU_SYS_BUS_CONFIG, 1);
	put(&gmu, "icache_config", GMU_ICACHE_CONFIG, 1);
	put(&gmu, "dcache_config", GMU_DCACHE_CONFIG, 1);
	put(&gmu, "pwr_col_inter_frame_ctrl", GMU_PWR_COL_INTER_FRAME_CTRL, 0x9c40400);
	put(&gmu, "rpmh_ctrl", GMU_RPMH_CTRL, rd(gmu.base + GMU_RPMH_CTRL) | GMU_RPMH_CTRL_ALL);

	announce("let the GMU's processor out of reset, and wait for its firmware to say it has started");
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
	/* what an earlier start left in the interrupt bits is not this one's (a6xx_gmu_irq_enable clears them too) */
	put(&gmu, "gmu2host_intr_clr", GMU_WORD(0x5191), ~0u);
	put(&gmu, "ao_host_interrupt_clr", GMU_WORD(0x9304), ~0u);
	get(&gmu, "gmu2host_intr_info, cleared", GMU_WORD(0x5192));
	put(&gmu, "pwr_col_cp_resp (the log's write index)", GMU_PWR_COL_CP_RESP, 0);
	put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 0);
	gmu_runs = true;                /* (from here on, the end holds it in reset again) */
	for (int i = 0; i < 1000 && (result = rd(gmu.base + GMU_CM3_FW_INIT_RESULT)) != GMU_STARTED; i++) {
		sleep_us(100);
	}
	get(&gmu, "cm3_fw_init_result", GMU_CM3_FW_INIT_RESULT);
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	get(&gmu, "ao_host_interrupt_status", GMU_AO_HOST_INTERRUPT_STATUS);
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	get(&smmu, "cb1 fsr", smmu_context(&gmu_space) + SMMU_CB_FSR);
	get(&gmu, "gmu2host_intr_info (bit 23: its processor faulted)", GMU_WORD(0x5192));
	sleep_us(100000);
	get(&gmu, "gmu2host_intr_info, 100 ms later", GMU_WORD(0x5192));
	if (result != GMU_STARTED) {
		printf("a618: %u: the GMU's firmware does not say it started (%#x)\n", step, result);
		return false;
	}
	printf("a618: %u: THE GMU'S FIRMWARE RUNS\n", step);
	return true;
}

/* ---- step 16: talking to the GMU (HFI, version 1) ------------------------------------------------------ */

#define GMU_GMU2HOST_INTR_CLR   GMU_WORD(0x5191)
#define GMU_GMU2HOST_INTR_INFO  GMU_WORD(0x5192)        /* bit 0: a message for us */
#define GMU_HOST2GMU_INTR_SET   GMU_WORD(0x5194)        /* bit 0: a message for the GMU */
#define GMU_HFI_CTRL_INIT       GMU_WORD(0x5186)
#define GMU_BOOT_SLUMBER_OPTION GMU_WORD(0x23f8)
#define GMU_GX_VOTE_IDX         GMU_WORD(0x23f9)
#define GMU_MX_VOTE_IDX         GMU_WORD(0x23fa)
#define GMU_GX_SPTPRAC_POWER_CONTROL GMU_WORD(0x0081)
#define GMU_OOB_BOOT_SET        22              /* GEN1's out-of-band bits for "boot/slumber" (cleared on the ack's) */
#define GMU_OOB_BOOT_ACK        30
#define GMU_OOB_GPU_SET         16              /* "keep the 3D core powered", cleared on 24 */
#define GMU_OOB_GPU_ACK         24
#define GMU_OOB_DCVS_SET        23              /* "change the speed", cleared on 31 */
#define GMU_OOB_DCVS_ACK        31
#define HFI_MSG_CMD             0
#define HFI_F2H_MSG_ERROR       100
#define HFI_H2F_MSG_INIT        0
#define HFI_H2F_MSG_FW_VERSION  1
#define HFI_H2F_MSG_BW_TABLE    3
#define HFI_H2F_MSG_PERF_TABLE  4
#define HFI_H2F_MSG_TEST        5
#define HFI_HEADER_ID(h)        ((h) & 0xff)
#define HFI_HEADER_SIZE(h)      (((h) >> 8) & 0xff)
#define HFI_HEADER_SEQNUM(h)    (((h) >> 20) & 0xfff)
#define GMU_COLD_BOOT           1

static uint32_t hfi_seqnum;
static bool gmu_talks, core_is_on;

static volatile struct hfi_queue_header *
hfi_queue(uint32_t n)
{
	return (volatile struct hfi_queue_header *)(hfi + sizeof(struct hfi_table_header)) + n;
}

static volatile uint32_t *
hfi_data(uint32_t n)
{
	return (volatile uint32_t *)(hfi + 0x1000 * (n + 1));
}

/* What can be seen of a GMU that crashed or does not answer. */
static void
gmu_post_mortem(void)
{
	uint32_t context = smmu_context(&gmu_space);
	volatile uint32_t *log = gmu_log.memory ? (volatile uint32_t *)block_uncached(&gmu_log) : NULL;
	volatile uint32_t *debug = gmu_debug.memory ? (volatile uint32_t *)block_uncached(&gmu_debug) : NULL;

	printf("a618: %u: the GMU after the fact: gmu2host %#x, ao host interrupts %#x, init result %#x, sysreset %#x\n", step,
	    rd(gmu.base + GMU_GMU2HOST_INTR_INFO), rd(gmu.base + GMU_AO_HOST_INTERRUPT_STATUS),
	    rd(gmu.base + GMU_CM3_FW_INIT_RESULT), rd(gmu.base + GMU_CM3_SYSRESET));
	printf("a618: %u: its SMMU context: fsr %#x, far %#x%08x, fsynr0 %#x; global sgfsr %#x\n", step,
	    rd(smmu.base + context + SMMU_CB_FSR), rd(smmu.base + context + SMMU_CB_FAR + 4),
	    rd(smmu.base + context + SMMU_CB_FAR), rd(smmu.base + context + SMMU_CB_FSYNR0), rd(smmu.base + SMMU_SGFSR));
	if (hfi != NULL) {
		for (uint32_t n = 0; n < HFI_QUEUES; n++) {
			printf("a618: %u: hfi queue %u: status %u, read %u, write %u, rx request %u, dropped %u\n", step, n,
			    hfi_queue(n)->status, hfi_queue(n)->read_index, hfi_queue(n)->write_index, hfi_queue(n)->rx_request,
			    hfi_queue(n)->dropped);
		}
	}
	for (int which = 0; which < 2; which++) {
		volatile uint32_t *w = which ? debug : log;

		if (w != NULL) {
			printf("a618: %u: its %s: %08x %08x %08x %08x %08x %08x %08x %08x\n", step, which ? "debugging region" : "log",
			    w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
		}
	}
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	get(&gmu, "ahb_fence_status", GMU_AHB_FENCE_STATUS);
}

/* a6xx_gmu_set_oob, GEN1's bits: a request outside the queues, and its ack taken */
static bool
gmu_oob(const char *what, uint32_t set, uint32_t ack)
{
	put(&gmu, "host2gmu_intr_set (out of band)", GMU_HOST2GMU_INTR_SET, 1u << set);
	if (!wait_for(&gmu, GMU_GMU2HOST_INTR_INFO, 1u << ack, 1u << ack, 10)) {
		printf("a618: %u: the GMU does not take %s (gmu2host %#x)\n", step, what, rd(gmu.base + GMU_GMU2HOST_INTR_INFO));
		gmu_post_mortem();
		return false;
	}
	put(&gmu, "gmu2host_intr_clr", GMU_GMU2HOST_INTR_CLR, 1u << ack);
	return true;
}

/* a6xx_gmu_clear_oob: GEN1 ends a request by raising the bit of its ack */
static void
gmu_oob_clear(uint32_t ack)
{
	put(&gmu, "host2gmu_intr_set (out of band, the end of a request)", GMU_HOST2GMU_INTR_SET, 1u << ack);
}

/* a6xx_hfi_queue_read, for GEN1 (no rounding of the index to four words) */
static uint32_t
hfi_read(uint32_t *data, uint32_t words)
{
	volatile struct hfi_queue_header *header = hfi_queue(1);
	volatile uint32_t *queue = hfi_data(1);
	uint32_t index = header->read_index, h, size;

	if (index == header->write_index) {
		header->rx_request = 1;
		return 0;
	}
	h = queue[index];
	size = HFI_HEADER_SIZE(h);
	if (size == 0 || size > words) {
		printf("a618: %u: an answer of %u words, more than %u: the queue is not what it should be\n", step, size, words);
		header->read_index = header->write_index;
		return 0;
	}
	for (uint32_t i = 0; i < size; i++) {
		data[i] = queue[index];
		index = (index + 1) % header->size;
	}
	__asm__ volatile ("dsb sy");
	header->read_index = index;
	return size;
}

/* a6xx_hfi_send_msg / a6xx_hfi_queue_write / a6xx_hfi_wait_for_ack: one message, and the GMU's answer to it */
static bool
hfi_send(const char *what, uint32_t id, uint32_t *message, uint32_t words, uint32_t *payload, uint32_t payload_words)
{
	volatile struct hfi_queue_header *header = hfi_queue(0);
	volatile uint32_t *queue = hfi_data(0);
	uint32_t seq = ++hfi_seqnum % 0xfff, index = header->write_index, answer[19];

	message[0] = seq << 20 | HFI_MSG_CMD << 16 | words << 8 | id;
	printf("a618: %u: about to send the GMU %s (%u words, number %u)\n", step, what, words, seq);
	hold();
	for (uint32_t i = 0; i < words; i++) {
		queue[index] = message[i];
		index = (index + 1) % header->size;
	}
	__asm__ volatile ("dsb sy");
	header->write_index = index;
	__asm__ volatile ("dsb sy");
	wr(gmu.base + GMU_HOST2GMU_INTR_SET, 1);
	for (;;) {
		uint32_t got;

		if (!wait_for(&gmu, GMU_GMU2HOST_INTR_INFO, 1, 1, 1000)) {
			printf("a618: %u: no answer to %s within a second (gmu2host %#x%s)\n", step, what,
			    rd(gmu.base + GMU_GMU2HOST_INTR_INFO), rd(gmu.base + GMU_GMU2HOST_INTR_INFO) & (1u << 23) ?
			    ": THE GMU'S PROCESSOR FAULTED" : "");
			gmu_post_mortem();
			return false;
		}
		wr(gmu.base + GMU_GMU2HOST_INTR_CLR, 1);
		while ((got = hfi_read(answer, sizeof(answer) / 4)) != 0) {
			if (HFI_HEADER_ID(answer[0]) == HFI_F2H_MSG_ERROR) {
				printf("a618: %u: the GMU reports an error: code %#x\n", step, got > 1 ? answer[1] : 0);
				continue;
			}
			if (got < 3 || HFI_HEADER_SEQNUM(answer[1]) != seq) {
				continue;       /* (an answer to something earlier) */
			}
			if (answer[2] != 0) {
				printf("a618: %u: the GMU answers %s with error %u\n", step, what, answer[2]);
				return false;
			}
			for (uint32_t i = 0; payload != NULL && i < payload_words && 3 + i < got; i++) {
				payload[i] = answer[3 + i];
			}
			printf("a618: %u: the GMU takes %s\n", step, what);
			return true;
		}
	}
}

/*
 * a6xx_gmu_rpmh_arc_votes_init: a vote for a level -- the first of the primary rail's levels that
 * reaches it, and the secondary's (mx) that does, or else its highest that is not 0.
 */
static uint32_t
arc_vote(uint32_t level, const uint16_t *pri, unsigned pri_count, const uint16_t *sec, unsigned sec_count)
{
	uint32_t pindex = 0, sindex = 0;
	unsigned j;

	for (j = 0; j < pri_count && pri[j] < level; j++) {
	}
	pindex = j < pri_count ? j : pri_count - 1;
	for (j = 0; j < sec_count; j++) {
		if (sec[j] >= level) {
			sindex = j;
			break;
		} else if (sec[j]) {
			sindex = j;
		}
	}
	return (uint32_t)(pri[pindex] & 0xffff) << 16 | sindex << 8 | pindex;
}

/*
 * The speeds: the stock dtb's qcom,gpu-pwrlevels for speed bin 128 (step 2's), lowest first and "off"
 * before them (a6xx_gmu_get_freqs), each with the RPMh level gpucc-sdmmagpie.c's gx_gfx3d_clk_src
 * gives it (the lowest whose rate_max reaches it); the GMU's: 200 MHz at CX's lowest.
 */
#define RPMH_MIN_SVS            48
#define RPMH_LOW_SVS            64
#define RPMH_SVS                128
#define RPMH_SVS_L1             192
#define RPMH_NOM                256
#define RPMH_NOM_L1             320

static const struct {
	uint32_t khz, level;
} gpu_speeds[] = {
	{ 0, 0 }, { 180000, RPMH_MIN_SVS }, { 267000, RPMH_LOW_SVS }, { 355000, RPMH_SVS }, { 430000, RPMH_SVS_L1 },
	{ 565000, RPMH_NOM }, { 610000, RPMH_NOM_L1 },
}, gmu_speeds[] = {
	{ 0, 0 }, { 200000, RPMH_MIN_SVS },
};

static bool
step_hfi(void)
{
	uint16_t gfx[16], cx[16], mx[16];
	unsigned gfx_count = 0, cx_count = 0, mx_count = 0;
	uint32_t version = 0;

	if (!gmu_runs || hfi == NULL) {
		printf("a618: %u: the GMU's firmware is not running: step 15\n", step);
		return false;
	}
	if (rpmh_resource("gfx.lvl", gfx, &gfx_count) == 0 || rpmh_resource("cx.lvl", cx, &cx_count) == 0 ||
	    rpmh_resource("mx.lvl", mx, &mx_count) == 0 || gfx_count == 0 || cx_count == 0 || mx_count == 0) {
		printf("a618: %u: RPMh's levels are not there (step 1)\n", step);
		return false;
	}
	announce("the GX rail: its vote for the highest speed, and the boot request (a6xx_gmu_gfx_rail_on)");
	{
		uint32_t vote = arc_vote(gpu_speeds[sizeof(gpu_speeds) / sizeof(gpu_speeds[0]) - 1].level, gfx, gfx_count, mx, mx_count);

		put(&gmu, "boot_slumber_option", GMU_BOOT_SLUMBER_OPTION, 0);
		put(&gmu, "gx_vote_idx", GMU_GX_VOTE_IDX, vote & 0xff);
		put(&gmu, "mx_vote_idx", GMU_MX_VOTE_IDX, (vote >> 8) & 0xff);
		if (!gmu_oob("the boot request", GMU_OOB_BOOT_SET, GMU_OOB_BOOT_ACK)) {
			return false;
		}
	}
	announce("SPTPRAC on (a6xx_sptprac_enable)");
	put(&gmu, "gx_sptprac_power_control", GMU_GX_SPTPRAC_POWER_CONTROL, 0x778000);
	if (!wait_for(&gmu, GMU_SPTPRAC_PWR_CLK_STATUS, 0x38, 0x28, 10)) {
		printf("a618: %u: SPTPRAC does not come on (%#x)\n", step, rd(gmu.base + GMU_SPTPRAC_PWR_CLK_STATUS));
		return false;
	}
	announce("the HFI queues started (a6xx_gmu_hfi_start)");
	put(&gmu, "hfi_ctrl_init", GMU_HFI_CTRL_INIT, 1);
	if (!wait_for(&gmu, GMU_HFI_CTRL_STATUS, 1, 1, 10)) {
		printf("a618: %u: the GMU does not start its queues (%#x)\n", step, rd(gmu.base + GMU_HFI_CTRL_STATUS));
		gmu_post_mortem();
		return false;
	}
	announce("start the GMU's message protocol: its start message, with where its debugging region is");
	{
		uint32_t m[5] = { 0, 0, GMU_DEBUG_ADDRESS, GMU_REGION_SIZE, GMU_COLD_BOOT };

		if (!hfi_send("its start (HFI_H2F_MSG_INIT)", HFI_H2F_MSG_INIT, m, 5, NULL, 0)) {
			return false;
		}
	}
	announce("ask the GMU for its version (offering 1.10, as Linux does)");
	{
		uint32_t m[2] = { 0, 1u << 28 | 1u << 19 | 1u << 17 };

		if (!hfi_send("the version question", HFI_H2F_MSG_FW_VERSION, m, 2, &version, 1)) {
			return false;
		}
		printf("a618: %u: the GMU's firmware is version %#x\n", step, version);
	}
	announce("send the GMU the table of speeds with their RPMh votes");
	{
		uint32_t m[3 + 2 * 16 + 2 * 4] = { 0 };

		m[1] = sizeof(gpu_speeds) / sizeof(gpu_speeds[0]);
		m[2] = sizeof(gmu_speeds) / sizeof(gmu_speeds[0]);
		for (uint32_t i = 0; i < m[1]; i++) {
			m[3 + 2 * i] = arc_vote(gpu_speeds[i].level, gfx, gfx_count, mx, mx_count);
			m[3 + 2 * i + 1] = gpu_speeds[i].khz;
			printf("a618: %u: the 3D core at %u kHz: RPMh level %u, vote %#x\n", step, gpu_speeds[i].khz,
			    gpu_speeds[i].level, m[3 + 2 * i]);
		}
		for (uint32_t i = 0; i < m[2]; i++) {
			m[3 + 2 * 16 + 2 * i] = arc_vote(gmu_speeds[i].level, cx, cx_count, mx, mx_count);
			m[3 + 2 * 16 + 2 * i + 1] = gmu_speeds[i].khz;
			printf("a618: %u: the GMU at %u kHz: RPMh level %u, vote %#x\n", step, gmu_speeds[i].khz, gmu_speeds[i].level,
			    m[3 + 2 * 16 + 2 * i]);
		}
		if (!hfi_send("the table of speeds (HFI_H2F_MSG_PERF_TABLE, version 1)", HFI_H2F_MSG_PERF_TABLE, m,
		    sizeof(m) / 4, NULL, 0)) {
			return false;
		}
	}
	announce("send the GMU the memory bandwidth table: the A618's, one \"off\" entry (a618_build_bw_table)");
	{
		/* a6xx_hfi_msg_bw_table: header, 5 counts and masks, cnoc addresses[6], cnoc data[2][6], ddr addresses[8], ddr data[16][8] */
		static uint32_t m[1 + 5 + 6 + 12 + 8 + 128];

		memset(m, 0, sizeof(m));
		m[1] = 1;                       /* bw_level_num */
		m[2] = 1;                       /* cnoc_cmds_num */
		m[3] = 3;                       /* ddr_cmds_num */
		m[4] = 0x01;                    /* cnoc_wait_bitmask */
		m[5] = 0x01;                    /* ddr_wait_bitmask */
		m[6] = 0x5007c;                 /* cnoc_cmds_addrs[0] */
		m[12] = 0x40000000;             /* cnoc_cmds_data[0][0] */
		m[18] = 0x60000001;             /* cnoc_cmds_data[1][0] */
		m[24] = 0x50000;                /* ddr_cmds_addrs: MC0, SH0, ... */
		m[25] = 0x5003c;
		m[26] = 0x5000c;
		m[32] = m[33] = m[34] = 0x40000000;     /* ddr_cmds_data[0][0..2] */
		if (!hfi_send("the bandwidth table (HFI_H2F_MSG_BW_TABLE)", HFI_H2F_MSG_BW_TABLE, m, sizeof(m) / 4, NULL, 0)) {
			return false;
		}
	}
	announce("a test message, the last Linux sends at start");
	{
		uint32_t m[1] = { 0 };

		if (!hfi_send("the test message", HFI_H2F_MSG_TEST, m, 1, NULL, 0)) {
			return false;
		}
	}
	printf("a618: %u: THE GMU TALKS: its message protocol is up\n", step);
	gmu_talks = true;
	get(&gmu, "hfi_ctrl_status", GMU_HFI_CTRL_STATUS);
	get(&smmu, "cb1 fsr", smmu_context(&gmu_space) + SMMU_CB_FSR);
	return true;
}

/* ---- step 17: the 3D core powered ------------------------------------------------------------------------ */

/* The 3D core's own registers (a6xx.xml, words, from the GPU's base: the stock dtb's kgsl_3d0_reg_memory) */
static struct block gpu = { "gpu", 0x05000000, 0x40000 };     /* all of it: hw_init writes up to word 0xf810 */
#define GPU_WORD(n)             (4u * (n))
#define RBBM_CLOCK_CNTL         GPU_WORD(0x00ae)
#define RBBM_INT_0_STATUS       GPU_WORD(0x0201)
#define RBBM_STATUS             GPU_WORD(0x0210)
#define CP_RB_BASE              GPU_WORD(0x0800)
#define CP_HW_FAULT             GPU_WORD(0x0821)
#define GMU_DCVS_ACK_OPTION     GMU_WORD(0x23fc)
#define GMU_DCVS_PERF_SETTING   GMU_WORD(0x23fd)
#define GMU_DCVS_BW_SETTING     GMU_WORD(0x23fe)
#define GMU_DCVS_RETURN         GMU_WORD(0x23ff)
#define GX_GDSC_ON              (1u << 31)      /* of the gx gdscr: PWR_ON */
#define SPTPRAC_OFF             0xc0            /* of sptprac_pwr_clk_status: the 3D core without power, without clock */

static uint32_t speed_wanted = 1;      /* the index into gpu_speeds; gpu=1: the highest */
static uint32_t core_khz;

static bool
step_core_power(void)
{
	uint32_t speed = speed_wanted;

	if (!gmu_talks) {
		printf("a618: %u: the GMU does not talk yet: step 16\n", step);
		return false;
	}
	PE_parse_boot_argn("a618_speed", &speed, sizeof(speed));
	if (speed < 1 || speed >= sizeof(gpu_speeds) / sizeof(gpu_speeds[0])) {
		speed = 1;
	}
	core_khz = gpu_speeds[speed].khz;
	announce("ask the GMU for the 3D core's speed (a6xx_gmu_set_freq: GEN1's \"DCVS\" request)");
	printf("a618: %u: the speed wanted: %u MHz (a618_speed=%u)\n", step, gpu_speeds[speed].khz / 1000, speed);
	put(&gmu, "dcvs_ack_option", GMU_DCVS_ACK_OPTION, 0);
	put(&gmu, "dcvs_perf_setting", GMU_DCVS_PERF_SETTING, 3u << 28 | speed);
	put(&gmu, "dcvs_bw_setting (the GMU's choice)", GMU_DCVS_BW_SETTING, 0xff);
	if (!gmu_oob("the speed", GMU_OOB_DCVS_SET, GMU_OOB_DCVS_ACK)) {
		return false;
	}
	gmu_oob_clear(GMU_OOB_DCVS_ACK);
	if (get(&gmu, "dcvs_return", GMU_DCVS_RETURN) != 0) {
		printf("a618: %u: the GMU does not set the speed (%d)\n", step, (int)rd(gmu.base + GMU_DCVS_RETURN));
		return false;
	}

	announce("ask the GMU to power the 3D core and keep it powered (GEN1's \"GPU_SET\" request)");
	get(&gpucc, "gx gdsc", GPUCC_GX_GDSCR);
	get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS);
	if (!gmu_oob("the request for power", GMU_OOB_GPU_SET, GMU_OOB_GPU_ACK)) {
		return false;
	}
	core_is_on = true;
	get(&gmu, "rpmh_power_state", GMU_RPMH_POWER_STATE);
	if (!(get(&gpucc, "gx gdsc", GPUCC_GX_GDSCR) & GX_GDSC_ON) ||
	    (get(&gmu, "sptprac_pwr_clk_status", GMU_SPTPRAC_PWR_CLK_STATUS) & SPTPRAC_OFF) != 0) {
		printf("a618: %u: the GMU says yes, but the 3D core has no power\n", step);
		return false;
	}

	announce("read registers of the 3D core itself (0x5000000): the first time it has power");
	get(&gpu, "rbbm_status", RBBM_STATUS);
	get(&gpu, "rbbm_int_0_status", RBBM_INT_0_STATUS);
	get(&gpu, "rbbm_clock_cntl", RBBM_CLOCK_CNTL);
	get(&gpu, "cp_rb_base", CP_RB_BASE);
	get(&gpu, "cp_hw_fault", CP_HW_FAULT);
	get(&smmu, "cb1 fsr", smmu_context(&gmu_space) + SMMU_CB_FSR);
	printf("a618: %u: THE 3D CORE HAS POWER AND ANSWERS\n", step);
	return true;
}

/* ---- step 18: the 3D core's memory and its microcode ------------------------------------------------------ */

/*
 * The 3D core takes its orders as words in a ring of memory, which its command processor ("SQE",
 * with microcode of its own: a630_sqe.fw) reads up to where a register says we have written. It
 * reaches memory through the SMMU as stream 0 (the stock dtb's gfx3d_user; the A618 has no second
 * one, LPAC, which the A660 has), with context 0 and tables of ours.
 */
#define CORE_STREAM             0
#define CORE_MATCH              0
#define PTE_CORE                (PTE_GMU | PTE_UNPRIVILEGED)
#define PTE_READ_ONLY           (1ull << 7)     /* Linux gives the core its microcode so (MSM_BO_GPU_READONLY) */
#define CORE_RING_ADDRESS       0x01000000u
#define CORE_RING_SIZE          0x8000u         /* Linux's; 8192 words */
#define CORE_SQE_ADDRESS        0x01100000u
#define CORE_SCRATCH_ADDRESS    0x01200000u
#define SMMU_SCTLR_SERVE_AFTER_FAULT (1u << 8)  /* qcom_adreno_smmu_write_sctlr: after a fault the core's other requests are served */

static struct gpu_space core_space = { .name = "the 3D core", .context = 0, .control = SMMU_SCTLR_SERVE_AFTER_FAULT };
static struct gpu_block core_ring, core_sqe, core_scratch;
static bool core_space_is_on, zap_is_loaded;

static bool
step_core_space(void)
{
	const uint8_t *microcode;
	uint64_t size = 0;
	int disk = -1;

	if (!core_is_on) {
		printf("a618: %u: the 3D core has no power: step 17\n", step);
		return false;
	}
	announce("make the 3D core's memory: its ring, its microcode (fw/a630_sqe.fw), a page to try writing to");
	microcode = hakios_image_file("fw", "a630_sqe.fw", &size, &disk);
	if (microcode == NULL || size < 8 || size > 0x40000) {
		printf("a618: %u: no fw/a630_sqe.fw (or not one) in the small image\n", step);
		return false;
	}
	if (!block_new(&core_space, &core_ring, CORE_RING_ADDRESS, CORE_RING_SIZE, PTE_CORE) ||
	    !block_new(&core_space, &core_sqe, CORE_SQE_ADDRESS, (uint32_t)size - 4, PTE_CORE | PTE_READ_ONLY) ||
	    !block_new(&core_space, &core_scratch, CORE_SCRATCH_ADDRESS, 4096, PTE_CORE)) {
		return false;
	}
	memcpy((void *)core_sqe.memory, microcode + 4, size - 4);       /* the file's first word is not microcode */
	to_memory(core_sqe.memory, core_sqe.size);
	printf("a618: %u: the microcode: %llu bytes, its version word %#x\n", step, size - 4, ((const uint32_t *)microcode)[1]);

	announce("give the SMMU's context 0 these tables and send the 3D core's requests (stream 0) through it");
	core_space_is_on = true;
	if (!space_on(&core_space, CORE_MATCH, CORE_STREAM)) {
		return false;
	}
	get(&gpu, "rbbm_status", RBBM_STATUS);
	return !space_faulted(&core_space);
}

/* ---- step 19: the zap shader, loaded ---------------------------------------------------------------------- */

/*
 * The rest of what Linux does with the zap shader (qcom_mdt_pas_init, qcom_mdt_load): TrustZone is
 * given the header again (step 12's), told where the image will be, the image's one segment
 * (a615_zap.b02: relocatable, paddr 0x5000, 0x7c0 bytes -- so 0x1000 of memory, the segment at its
 * start) is put there, and TrustZone asked to authenticate it and run it. Android's place for it,
 * gpu_region@97f15000, is in what Mu-Silicium's map of this phone hands out as UEFI's own heap (its
 * "PIL Reserved" ends at 0x97600000), where the loader that watches us lives: so a page of ours
 * instead, never touched again and never freed. A kernel that ends in order has TrustZone give the
 * shader up again (steps_undo). Asking it to give one up when it has none since the phone started is
 * not what redwood's TrustZone makes of it (a yes): this one resets the phone (2026-10-23, gpu=1 on a
 * phone just started, step 12 left out). So the header is offered first, and only when that is
 * refused -- a shader from a kernel that did not end in order, or step 12's header of this run --
 * is TrustZone asked to give that up, and the header offered again.
 */
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_THREE_VALUES        3
#define ZAP_SIZE                0x1000u

static bool
zap_unload(void)
{
	uint64_t answer = ~0ull;

	return scm("to give up the zap shader", scm_how | SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_GPU, 0, 0, &answer) == 0 &&
	       answer == 0;
}

static bool
step_zap_load(void)
{
	const uint8_t *segment;
	vm_offset_t page = 0;
	uint64_t size = 0, header = 0, answer = ~0ull, phys;
	int disk = -1;

	if (!core_space_is_on) {
		printf("a618: %u: the 3D core's memory is not there: step 18\n", step);
		return false;
	}
	segment = hakios_image_file("fw", "a615_zap.b02", &size, &disk);
	if (segment == NULL || size == 0 || size > ZAP_SIZE) {
		printf("a618: %u: no fw/a615_zap.b02 (or not one) in the small image\n", step);
		return false;
	}
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("a618: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, segment, size);
	to_memory(page, PAGE_SIZE);
	phys = kvtophys(page);
	printf("a618: %u: the zap shader (%llu bytes, begins %02x %02x %02x %02x) in a page of ours at %#llx\n", step, size,
	    segment[0], segment[1], segment[2], segment[3], phys);

	announce("hand the zap shader's header to TrustZone, tell it where the image is, and ask it to authenticate and run it");
	if (!zap_header(&header)) {
		return false;
	}
	if (scm("to take the zap shader's signature", (scm_how & ~SCM_FAST) | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_GPU,
	    header, 0, &answer) != 0 || answer != 0) {
		printf("a618: %u: TrustZone does not take the signature (%#llx): it has a zap shader or a header from before; "
		    "asked to give that up, and the header offered again\n", step, answer);
		if (!zap_unload() || !zap_header(&header) ||
		    scm("to take the zap shader's signature", (scm_how & ~SCM_FAST) | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER,
		    PAS_GPU, header, 0, &answer) != 0 || answer != 0) {
			printf("a618: %u: TrustZone does not take the signature after that either (%#llx)\n", step, answer);
			return false;
		}
	}
	if (scm("where the image is", scm_how | SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_GPU, phys, ZAP_SIZE, &answer) != 0 ||
	    answer != 0) {
		printf("a618: %u: TrustZone does not take the place (%#llx)\n", step, answer);
		return false;
	}
	if (scm("to authenticate the zap shader and run it", (scm_how & ~SCM_FAST) | SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE,
	    PAS_GPU, 0, 0, &answer) != 0 || answer != 0) {
		printf("a618: %u: TrustZone does not authenticate the zap shader (%#llx)\n", step, answer);
		return false;
	}
	printf("a618: %u: TRUSTZONE HAS AUTHENTICATED AND STARTED THE ZAP SHADER\n", step);
	zap_is_loaded = true;
	return true;
}

/* ---- step 20: the 3D core's registers, its command processor started, its first command ------------------- */

#define GBIF_HALT               (4 * 0x45)      /* of the bus interface (step 6's block) */
#define GBIF_HALT_ACK           (4 * 0x46)
#define RBBM_VBIF_CLIENT_QOS_CNTL GPU_WORD(0x0010)
#define RBBM_GBIF_HALT          GPU_WORD(0x0016)
#define RBBM_GBIF_HALT_ACK      GPU_WORD(0x0017)
#define RBBM_INTERFACE_HANG_INT_CNTL GPU_WORD(0x001f)
#define RBBM_PERFCTR_CNTL       GPU_WORD(0x0500)
#define RBBM_PERFCTR_GPU_BUSY_MASKED GPU_WORD(0x050b)
#define RBBM_NC_MODE_CNTL       GPU_WORD(0x0534)
#define CP_RB_CNTL              GPU_WORD(0x0802)
#define CP_RB_RPTR              GPU_WORD(0x0806)
#define CP_RB_WPTR              GPU_WORD(0x0807)
#define CP_SQE_CNTL             GPU_WORD(0x0808)
#define CP_INTERRUPT_STATUS     GPU_WORD(0x0823)
#define CP_PROTECT_STATUS       GPU_WORD(0x0824)
#define CP_SQE_INSTR_BASE       GPU_WORD(0x0830)
#define CP_ROQ_THRESHOLDS_1     GPU_WORD(0x08c1)
#define CP_ROQ_THRESHOLDS_2     GPU_WORD(0x08c2)
#define CP_MEM_POOL_SIZE        GPU_WORD(0x08c3)
#define CP_AHB_CNTL             GPU_WORD(0x098d)
#define UCHE_MODE_CNTL          GPU_WORD(0x0e01)
#define UCHE_WRITE_RANGE_MAX    GPU_WORD(0x0e05)
#define UCHE_WRITE_THRU_BASE    GPU_WORD(0x0e07)
#define UCHE_TRAP_BASE          GPU_WORD(0x0e09)
#define UCHE_GMEM_RANGE_MIN     GPU_WORD(0x0e0b)
#define UCHE_GMEM_RANGE_MAX     GPU_WORD(0x0e0d)
#define UCHE_CACHE_WAYS         GPU_WORD(0x0e17)
#define UCHE_FILTER_CNTL        GPU_WORD(0x0e18)
#define UCHE_CLIENT_PF          GPU_WORD(0x0e19)
#define RB_NC_MODE_CNTL         GPU_WORD(0x8e08)
#define PC_DBG_ECO_CNTL         GPU_WORD(0x9e00)
#define SP_NC_MODE_CNTL         GPU_WORD(0xae02)
#define TPL1_NC_MODE_CNTL       GPU_WORD(0xb604)
#define RBBM_SECVID_TSB_TRUSTED_BASE GPU_WORD(0xf800)
#define RBBM_SECVID_TSB_TRUSTED_SIZE GPU_WORD(0xf802)
#define RBBM_SECVID_TSB_CNTL    GPU_WORD(0xf803)
#define CP_RB_CNTL_VALUE        (12u | 2u << 8 | 1u << 27)      /* 2^12 pairs of words; no copy of the read position (no APRIV) */
#define UCHE_TRAP               0x0001fffffffff000ull   /* Linux's uche_trap_base for A6xx */
#define GMEM_BASE               0x00100000ull
#define GMEM_SIZE               0x80000u                /* the A618's 512K */

/* commands (Mesa's adreno_pm4.xml); a packet is a header word and so many more */
#define CP_MEM_WRITE            0x3d
#define CP_ME_INIT              0x48
#define CP_SET_SECURE_MODE      0x66

static bool core_runs;
#define RING_WORDS              (CORE_RING_SIZE / 4)
static uint32_t ring_at;                /* where the next word goes: the ring goes round */
static uint32_t ring_sent;              /* up to where the core has been told to read */

static void
put64(struct block *b, const char *name, uint32_t offset, uint64_t value)
{
	put(b, name, offset, (uint32_t)value);
	put(b, name, offset + 4, (uint32_t)(value >> 32));
}

static uint32_t
odd_parity(uint32_t v)
{
	return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}

static void
ring(uint32_t word)
{
	((uint32_t *)core_ring.memory)[ring_at] = word;
	ring_at = (ring_at + 1) % RING_WORDS;
}

/* The header of a command with so many words after it. */
static void
ring_command(uint32_t command, uint32_t words)
{
	ring(7u << 28 | words | odd_parity(words) << 15 | command << 16 | odd_parity(command) << 23);
}

/* What has been written to the ring since the last time: out to memory, and the core told how far to read. */
static void
ring_send(void)
{
	if (ring_at >= ring_sent) {
		to_memory(core_ring.memory + 4 * ring_sent, 4 * (ring_at - ring_sent));
	} else {
		to_memory(core_ring.memory + 4 * ring_sent, 4 * (RING_WORDS - ring_sent));
		to_memory(core_ring.memory, 4 * ring_at);
	}
	ring_sent = ring_at;
	wr(gpu.base + CP_RB_WPTR, ring_at);
}

/* Let the 3D core have what is in the ring, and wait until it has taken all of it and rests. */
static bool
ring_run(const char *what)
{
	uint32_t taken = ~0u, status = ~0u;

	printf("a618: %u: about to have the 3D core %s: the ring up to word %u\n", step, what, ring_at);
	hold();
	ring_send();
	for (int i = 0; i < 20000 && (taken != ring_at || (status & ~1u)); i++) {
		sleep_us(100);
		taken = rd(gpu.base + CP_RB_RPTR);
		status = rd(gpu.base + RBBM_STATUS);
	}
	if (taken == ring_at && !(status & ~1u) && rd(gpu.base + CP_HW_FAULT) == 0 && !space_faulted(&core_space)) {
		printf("a618: %u: the 3D core has done it (ring read up to %u, status %#x)\n", step, taken, status);
		return true;
	}
	printf("a618: %u: the 3D core does not get through: ring read up to %u of %u, status %#x, interrupts %#x, "
	    "command processor: fault %#x, interrupts %#x, protection %#x\n", step, taken, ring_at, status,
	    rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS),
	    rd(gpu.base + CP_PROTECT_STATUS));
	space_faulted(&core_space);
	return false;
}

/*
 * Linux's hw_init for the A618 (ADRENO_6XX_GEN1, GBIF), less what only matters once it draws for
 * programs: clock gating, the protection of registers against their commands, interrupts (polled
 * here), counters beyond "busy".
 */
static bool
step_core_start(void)
{
	static const uint32_t address_modes[] = { 0x0842, 0x0c01, 0x8601, 0x8e05, 0x9e01, 0xbe05, 0xa601, 0x9601, 0x0e00, 0xae01,
		                                  0xb601, 0xf810 };

	if (!core_is_on || !core_space_is_on) {
		printf("a618: %u: needs the 3D core powered and its memory: steps 17 and 18\n", step);
		return false;
	}
	announce("write the 3D core's registers as Linux does before it starts the command processor");
	put(&gbif, "halt", GBIF_HALT, 0);
	put(&gpu, "rbbm_gbif_halt", RBBM_GBIF_HALT, 0);
	put(&gpu, "rbbm_secvid_tsb_cntl", RBBM_SECVID_TSB_CNTL, 0);
	put64(&gpu, "rbbm_secvid_tsb_trusted_base", RBBM_SECVID_TSB_TRUSTED_BASE, 0);
	put(&gpu, "rbbm_secvid_tsb_trusted_size", RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
	for (unsigned i = 0; i < sizeof(address_modes) / sizeof(address_modes[0]); i++) {
		put(&gpu, "address mode: 64 bits", GPU_WORD(address_modes[i]), 1);
	}
	put(&gpu, "rbbm_vbif_client_qos_cntl", RBBM_VBIF_CLIENT_QOS_CNTL, 3);
	put(&gpu, "rbbm_perfctr_gpu_busy_masked", RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
	put64(&gpu, "uche_write_range_max", UCHE_WRITE_RANGE_MAX, UCHE_TRAP + 0xfc0);
	put64(&gpu, "uche_trap_base", UCHE_TRAP_BASE, UCHE_TRAP);
	put64(&gpu, "uche_write_thru_base", UCHE_WRITE_THRU_BASE, UCHE_TRAP);
	put64(&gpu, "uche_gmem_range_min", UCHE_GMEM_RANGE_MIN, GMEM_BASE);
	put64(&gpu, "uche_gmem_range_max", UCHE_GMEM_RANGE_MAX, GMEM_BASE + GMEM_SIZE - 1);
	put(&gpu, "uche_filter_cntl", UCHE_FILTER_CNTL, 0x804);
	put(&gpu, "uche_cache_ways", UCHE_CACHE_WAYS, 0x4);
	put(&gpu, "cp_roq_thresholds_2", CP_ROQ_THRESHOLDS_2, 0x010000c0);
	put(&gpu, "cp_roq_thresholds_1", CP_ROQ_THRESHOLDS_1, 0x8040362c);
	put(&gpu, "cp_mem_pool_size", CP_MEM_POOL_SIZE, 128);
	put(&gpu, "pc_dbg_eco_cntl (prim fifo threshold)", PC_DBG_ECO_CNTL, 0x00180000);
	put(&gpu, "cp_ahb_cntl", CP_AHB_CNTL, 1);
	put(&gpu, "rbbm_perfctr_cntl", RBBM_PERFCTR_CNTL, 1);
	/*
	 * a6xx_set_ubwc_config with SM7150's (ubwc_config.c): UBWC 2.0, level 2 and 3 swizzling, highest
	 * bank bit 14 -- so hbb_lo 1 and nothing else.
	 */
	put(&gpu, "rb_nc_mode_cntl", RB_NC_MODE_CNTL, 1u << 1);
	put(&gpu, "tpl1_nc_mode_cntl", TPL1_NC_MODE_CNTL, 1u << 1);
	put(&gpu, "sp_nc_mode_cntl", SP_NC_MODE_CNTL, 1u << 1);
	put(&gpu, "uche_mode_cntl", UCHE_MODE_CNTL, 1u << 21);
	put(&gpu, "rbbm_nc_mode_cntl", RBBM_NC_MODE_CNTL, 0);
	put(&gpu, "rbbm_interface_hang_int_cntl", RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | 0x1fffff);
	put(&gpu, "uche_client_pf", UCHE_CLIENT_PF, 1u << 7 | 1);
	put64(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE, CORE_SQE_ADDRESS);
	put64(&gpu, "cp_rb_base", CP_RB_BASE, CORE_RING_ADDRESS);
	put(&gpu, "cp_rb_cntl", CP_RB_CNTL, CP_RB_CNTL_VALUE);
	get(&gpu, "cp_sqe_instr_base", CP_SQE_INSTR_BASE);
	get(&gpu, "cp_rb_base", CP_RB_BASE);
	get(&gpu, "cp_rb_cntl", CP_RB_CNTL);
	get(&gpu, "rbbm_status", RBBM_STATUS);

	announce("start the 3D core's command processor");
	core_runs = true;
	put(&gpu, "cp_sqe_cntl", CP_SQE_CNTL, 1);
	sleep_ms(20);
	printf("a618: %u: alive after it; status %#x, interrupts %#x, command processor: fault %#x, interrupts %#x, "
	    "protection %#x, ring read up to %u\n", step, rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS),
	    rd(gpu.base + CP_HW_FAULT), rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS),
	    rd(gpu.base + CP_RB_RPTR));
	if (space_faulted(&core_space)) {
		return false;
	}

	announce("give the 3D core its first command, \"initialize\" (a6xx_cp_init)");
	ring_command(CP_ME_INIT, 8);
	ring(0x0000002f);
	ring(0x00000003);                       /* several hardware contexts */
	ring(0x20000000);                       /* error detection */
	ring(0);
	ring(0);
	ring(0);
	ring(0);
	ring(0);
	if (!ring_run("initialize its command processor")) {
		return false;
	}
	printf("a618: %u: THE 3D CORE TAKES COMMANDS\n", step);
	return true;
}

/* ---- step 21: out of secure mode; a word written on command ------------------------------------------------ */

static bool
step_core_commands(void)
{
	volatile uint32_t *scratch = (volatile uint32_t *)core_scratch.memory;

	if (!core_runs || !zap_is_loaded) {
		printf("a618: %u: needs the command processor running and the zap shader: steps 19 and 20\n", step);
		return false;
	}
	announce("command the 3D core out of secure mode (it asks the zap shader), then to write a word into a page of ours");
	ring_command(CP_SET_SECURE_MODE, 1);
	ring(0);
	if (!ring_run("leave secure mode")) {
		return false;
	}
	ring_command(CP_MEM_WRITE, 3);
	ring(CORE_SCRATCH_ADDRESS);
	ring(0);
	ring(0x600dc0de);
	if (!ring_run("write a word to memory")) {
		return false;
	}
	to_memory(core_scratch.memory, 64);
	printf("a618: %u: the page reads %#x: %s\n", step, scratch[0], scratch[0] == 0x600dc0de ?
	    "THE 3D CORE WRITES OUR MEMORY ON COMMAND" : "not what it was told to write");
	return scratch[0] == 0x600dc0de;
}

/* ---- step 22: a frame filled by the GPU, on the panel ------------------------------------------------------ */

/*
 * The simplest thing the 3D core draws: its 2D engine filling a rectangle with one colour (Mesa's
 * fd6_blitter.cc: fd6_clear_surface; the registers under their older names), three times for three
 * bands. The picture is 32-bit pixels in rows of a multiple of 64 bytes, in memory of ours; the
 * framebuffer driver copies it to the panel.
 */
#define RB_CCU_CNTL             0x8e07          /* register numbers: these are written by command */
#define GRAS_2D_BLIT_CNTL       0x8400
#define GRAS_2D_DST_TL          0x8405
#define GRAS_2D_DST_BR          0x8406
#define RB_2D_BLIT_CNTL         0x8c00
#define RB_2D_PIXEL_CNTL        0x8c01
#define RB_2D_DST_INFO          0x8c17
#define RB_2D_DST               0x8c18
#define RB_2D_DST_PITCH         0x8c1a
#define RB_2D_SRC_SOLID_C0      0x8c2c
#define SP_2D_DST_FORMAT        0xacc0
#define CCU_BYPASS              (0x10u << 23)   /* Mesa's for drawing straight into memory: colour at 64K (one CCU) */
#define FORMAT_8888             0x30
#define BLIT_SOLID_8888         (1u << 7 | FORMAT_8888 << 8 | 0xfu << 20 | 0x10u << 24) /* one colour; all channels; 8-bit */
#define DST_BGRA_LINEAR         (FORMAT_8888 | 1u << 10)        /* rows of pixels, blue first in memory */
#define EVENT_CACHE_FLUSH       0x04
#define EVENT_FLUSH_COLOR       0x1d
#define CP_WAIT_FOR_IDLE        0x26
#define CP_BLIT                 0x2c
#define CP_EVENT_WRITE          0x46
#define CP_SET_MARKER           0x65
#define CORE_TARGET_ADDRESS     0x10000000u
#define TARGET_WIDTH            1080
#define TARGET_HEIGHT           2340
#define TARGET_ROW              4352            /* 1088 pixels: the next multiple of 64 bytes */

extern void MiniIOMobileFramebufferShow(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t row_bytes);

static struct gpu_block core_target;

/* A register of the 3D core (by its word number) written by command, in the order of the other commands. */
static void
ring_register(uint32_t number, uint32_t value)
{
	ring(4u << 28 | 1 | odd_parity(1) << 7 | number << 8 | odd_parity(number) << 27);
	ring(value);
}

static void
ring_fill(uint32_t top, uint32_t bottom, uint32_t red, uint32_t green, uint32_t blue)
{
	ring_register(GRAS_2D_DST_TL, top << 16);
	ring_register(GRAS_2D_DST_BR, (TARGET_WIDTH - 1) | (bottom - 1) << 16);
	ring_register(RB_2D_SRC_SOLID_C0, red);         /* a register for each part of the colour, 0 to 255 */
	ring_register(RB_2D_SRC_SOLID_C0 + 1, green);
	ring_register(RB_2D_SRC_SOLID_C0 + 2, blue);
	ring_register(RB_2D_SRC_SOLID_C0 + 3, 255);
	ring_register(RB_2D_BLIT_CNTL, BLIT_SOLID_8888);
	ring_register(GRAS_2D_BLIT_CNTL, BLIT_SOLID_8888);
	ring_register(SP_2D_DST_FORMAT, 1 | FORMAT_8888 << 3 | 0xfu << 12);
	ring_register(RB_2D_PIXEL_CNTL, 0);
	ring_register(RB_2D_DST_INFO, DST_BGRA_LINEAR);
	ring_register(RB_2D_DST, CORE_TARGET_ADDRESS);
	ring_register(RB_2D_DST + 1, 0);
	ring_register(RB_2D_DST_PITCH, TARGET_ROW >> 6);
	ring_command(CP_WAIT_FOR_IDLE, 0);
	ring_command(CP_BLIT, 1);
	ring(3);                                /* "scale": the 2D engine's one operation */
}

static bool
step_frame(void)
{
	const volatile uint8_t *pixel;
	const uint32_t middle = TARGET_HEIGHT / 2 * TARGET_ROW + TARGET_WIDTH * 2, last = (TARGET_HEIGHT - 1) * TARGET_ROW +
	    (TARGET_WIDTH - 1) * 4;

	if (!core_runs) {
		printf("a618: %u: the command processor does not run: step 20\n", step);
		return false;
	}
	announce("make a picture's worth of memory for the 3D core (10 MB) and have it fill three bands of colour");
	if (!block_new(&core_space, &core_target, CORE_TARGET_ADDRESS, TARGET_ROW * TARGET_HEIGHT, PTE_CORE)) {
		return false;
	}
	/* tables that the SMMU has in use have grown: it has not seen these addresses, so nothing stale to forget */
	for (uint32_t i = 0; i < core_space.table_count; i++) {
		to_memory((vm_offset_t)core_space.tables[i].table, 4096);
	}
	ring_command(CP_SET_MARKER, 1);
	ring(12);                               /* "2D blits follow" */
	ring_register(RB_CCU_CNTL, CCU_BYPASS);
	ring_fill(0, TARGET_HEIGHT / 3, 255, 128, 0);           /* orange */
	ring_fill(TARGET_HEIGHT / 3, 2 * TARGET_HEIGHT / 3, 255, 255, 255);
	ring_fill(2 * TARGET_HEIGHT / 3, TARGET_HEIGHT, 0, 128, 255);
	/*
	 * What it has drawn out of its caches into memory. Each of the two events is followed by an
	 * address and a word that the core writes there when it is done (Linux's a6xx_submit; Mesa's
	 * tu6_emit_event_write: the events named ..._TS).
	 */
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_FLUSH_COLOR);
	ring(CORE_SCRATCH_ADDRESS + 32);
	ring(0);
	ring(0x0c0c0c0c);
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_CACHE_FLUSH);
	ring(CORE_SCRATCH_ADDRESS + 16);
	ring(0);
	ring(0x0d0e0d0e);
	ring_command(CP_WAIT_FOR_IDLE, 0);
	if (!ring_run("fill a picture")) {
		return false;
	}
	to_memory(core_scratch.memory, 64);
	printf("a618: %u: the words it writes when its caches are in memory: %#x, %#x (told: 0xc0c0c0c, 0xd0e0d0e)\n", step,
	    ((volatile uint32_t *)core_scratch.memory)[8], ((volatile uint32_t *)core_scratch.memory)[4]);
	to_memory(core_target.memory, core_target.size);
	pixel = (const volatile uint8_t *)core_target.memory;
	printf("a618: %u: the picture's first pixel is %02x %02x %02x %02x (blue green red alpha; told: 00 80 ff ff), one in "
	    "the middle %02x %02x %02x %02x, one in the last row %02x %02x %02x %02x\n", step, pixel[0], pixel[1], pixel[2],
	    pixel[3], pixel[middle], pixel[middle + 1], pixel[middle + 2], pixel[middle + 3], pixel[last], pixel[last + 1],
	    pixel[last + 2], pixel[last + 3]);
	if ((pixel[0] | pixel[1] | pixel[2]) == 0 || pixel[middle] == 0 || (pixel[last] | pixel[last + 1] | pixel[last + 2]) == 0) {
		printf("a618: %u: the picture is not what the 3D core was told to draw\n", step);
		return false;
	}
	MiniIOMobileFramebufferShow((const uint8_t *)core_target.memory, TARGET_WIDTH, TARGET_HEIGHT, TARGET_ROW);
	printf("a618: %u: A PICTURE DRAWN BY THE GPU IS ON THE PANEL: orange, white, blue from the top\n", step);
	sleep_ms(10000);                        /* (to be seen before the console or the system draws over it) */
	return true;
}

/* a6xx_bus_clear_pending_transactions for GBIF: the bus halted, its clients first, then its arbiter; let go again */
static void
core_bus_halt(void)
{
	wr(gbif.base + GBIF_HALT, 1);
	wait_for(&gbif, GBIF_HALT_ACK, 1, 1, 10);
	wr(gbif.base + GBIF_HALT, 2);
	wait_for(&gbif, GBIF_HALT_ACK, 2, 2, 10);
	wr(gbif.base + GBIF_HALT, 0);
}

/* GMU registers for its way down (a6xx_gmu_notify_slumber, a6xx_sptprac_disable, a6xx_rpmh_stop) */
#define GMU_CX_BUSY             (1u << 23)      /* GPUBUSYIGNAHB */
#define GMU_AO_AHB_FENCE_CTRL   GMU_WORD(0x9310)
#define GX_GDSC_RETAIN_FF       (1u << 11)

/* A6xx's way of putting a talking GMU to sleep with the 3D core off, as Linux's a6xx_gmu_shutdown is for GEN1 */
static void
gmu_slumber(void)
{
	if (core_is_on) {
		printf("a618: when the steps are over: the GMU no longer asked to keep the 3D core powered\n");
		gmu_oob_clear(GMU_OOB_GPU_ACK);
		core_is_on = false;
	}
	printf("a618: when the steps are over: SPTPRAC off, the GMU told to slumber\n");
	put(&gpucc, "gx gdsc (retain)", GPUCC_GX_GDSCR, rd(gpucc.base + GPUCC_GX_GDSCR) | GX_GDSC_RETAIN_FF);
	put(&gmu, "gx_sptprac_power_control", GMU_GX_SPTPRAC_POWER_CONTROL, 0x778001);
	if (!wait_for(&gmu, GMU_SPTPRAC_PWR_CLK_STATUS, 0x04, 0x04, 10)) {
		printf("a618: SPTPRAC does not go off (%#x)\n", rd(gmu.base + GMU_SPTPRAC_PWR_CLK_STATUS));
	}
	put(&gmu, "boot_slumber_option (slumber)", GMU_BOOT_SLUMBER_OPTION, 1);
	if (gmu_oob("the slumber request", GMU_OOB_BOOT_SET, GMU_OOB_BOOT_ACK)) {
		gmu_oob_clear(GMU_OOB_BOOT_ACK);
		get(&gmu, "rpmh_power_state (0xf: slumber)", GMU_RPMH_POWER_STATE);
	}
	put(&gmu, "ao_ahb_fence_ctrl", GMU_AO_AHB_FENCE_CTRL, 0);
	if (!wait_for(&gmu, GMU_AO_GPU_CX_BUSY_STATUS, GMU_CX_BUSY, 0, 10)) {
		printf("a618: the GMU does not go idle (cx busy status %#x)\n", rd(gmu.base + GMU_AO_GPU_CX_BUSY_STATUS));
	}
	get(&gpucc, "gx gdsc", GPUCC_GX_GDSCR);
	gmu_talks = false;
}

/* What the steps started that must not outlive them, in reverse. */
static void
steps_undo(void)
{
	if (core_runs) {
		step = 99;
		printf("a618: when the steps are over: the 3D core before it is stopped: status %#x, ring read up to %u of %u; "
		    "the GMU sees it busy: %#x\n", rd(gpu.base + RBBM_STATUS), rd(gpu.base + CP_RB_RPTR), ring_at,
		    rd(gmu.base + GMU_AO_GPU_CX_BUSY_STATUS));
		put(&gpu, "cp_sqe_cntl (the command processor stopped)", CP_SQE_CNTL, 0);
		core_bus_halt();
		printf("a618: when the steps are over: the bus halted and let go (gbif halt_ack %#x)\n", rd(gbif.base + GBIF_HALT_ACK));
		core_runs = false;
	}
	if (zap_is_loaded) {
		step = 99;
		printf("a618: when the steps are over: TrustZone %s the zap shader\n", zap_unload() ? "gives up" : "DOES NOT GIVE UP");
		zap_is_loaded = false;
	}
	if (core_space_is_on) {
		step = 99;
		printf("a618: when the steps are over: the 3D core's stream and its SMMU context off again\n");
		space_off(&core_space, CORE_MATCH);
		core_space_is_on = false;
	}
	if (gmu_talks) {
		step = 99;
		gmu_slumber();
	}
	if (gmu_runs) {
		step = 99;
		printf("a618: when the steps are over: the GMU's processor held in reset\n");
		put(&gmu, "cm3_sysreset", GMU_CM3_SYSRESET, 1);
		gmu_runs = false;
	}
	if (gmu_space_is_on) {
		step = 99;
		printf("a618: when the steps are over: the GMU's stream and its SMMU context off again\n");
		space_off(&gmu_space, GMU_MATCH);
		gmu_space_is_on = false;
	}
}

/* ---- the GPU kept for the system to draw with ------------------------------------------------------- */

/*
 * Boot-arg gpu=1 on this phone: steps 1 to 21, less the ones that only look (10 to 12), no pauses,
 * and the GPU not stopped again but kept for the service HakiosGPU (drivers/gpu/hakios.gpu.cpp),
 * which hands it to programs through the interface of Linux's msm driver, which is what Mesa's
 * freedreno speaks. The functions it calls are hakios.adreno.c's (hakios_gpu_*), and each of
 * those hands over to the one here (a618_gpu_*), this file being its backend; the allocator of
 * the 3D core's addresses is GPU-agnostic and stays there. This is hakios.adreno.c's "the GPU
 * kept on", for the A618, less its recovery from a hung core (gpu_recover): a submission that does
 * not come back is reported, and the GPU stays stopped.
 */
#define GPU_CHIP_ID             0x06010800ull   /* the A618 (Linux's catalog: qcom,sm7150) */
#define GPU_FENCE_AT            0x40            /* in the scratch page */
#define GPU_TIMES_AT            0x400           /* ... and when the core began and ended each of its last submissions */
#define GPU_TIMES               64
#define CP_ALWAYS_ON_COUNTER    0x0980          /* ticks of the 19.2 MHz crystal, 64 bits: for the core to read itself */
#define CP_REG_TO_MEM           0x3e
#define CP_INDIRECT_BUFFER      0x3f
#define CP_SCRATCH_REG2         0x0885
#define CP_IB1_BASE             GPU_WORD(0x0928)
#define CP_IB1_REM_SIZE         GPU_WORD(0x092a)
#define CP_IB2_BASE             GPU_WORD(0x092b)
#define CP_IB2_REM_SIZE         GPU_WORD(0x092d)
#define EVENT_INVALIDATE_DEPTH  0x18
#define EVENT_INVALIDATE_COLOR  0x19

/* drivers/gpu/hakios.adreno.c: the service HakiosGPU and the 3D core's addresses, over this GPU as its backend */
struct hakios_gpu_backend {
	const char *    name;
	void            (*init)(void);
	bool            (*kept)(void);
	bool            (*parameter)(uint32_t which, uint64_t *value);
	bool            (*map)(uint64_t address, uint64_t phys, bool read_only);
	void            (*mapped)(void);
	void            (*unmap)(uint64_t address, uint64_t size);
	bool            (*done)(uint32_t fence);
	uint32_t        (*last)(void);
	void            (*worked)(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
	uint32_t        (*submit)(const uint64_t *addresses, const uint32_t *words, uint32_t count);
	void            (*report)(const char *why);
	void            (*stop)(void);
	void            (*quiesce)(void);
};
extern void hakios_gpu_backend_register(const struct hakios_gpu_backend *backend);
extern void hakios_gpu_backend_kept(void);
extern uint32_t gpu_log;
extern void hakios_tl(const char *format, ...) __printflike(1, 2);

static bool gpu_kept;
static uint32_t gpu_fence;              /* the number of the last submission */
static uint32_t gpu_timed;              /* ... of the last whose times have been added up */
static uint64_t gpu_ticks;
static uint32_t gpu_timed_count, gpu_timed_by[7];
static uint32_t gpu_exact = 1;          /* boot-arg gpu_exact=0: the core does not wait to be idle before it notes the time */

static bool
a618_kept(void)
{
	return gpu_kept;
}

/* The answers to msm's GET_PARAM, as Linux's adreno_get_param gives them for this GPU (the numbers are msm_drm.h's). */
static bool
a618_gpu_parameter(uint32_t which, uint64_t *value)
{
	switch (which) {
	case 0x01:                              /* GPU_ID: none (Linux's sm7150 entry has no revn), the chip ID says it */
	case 0x08:                              /* PP_PGTABLE */
	case 0x09:                              /* FAULTS */
	case 0x0a:                              /* SUSPENDS */
	case 0x11:                              /* RAYTRACING */
	case 0x13:                              /* MACROTILE_MODE: SM7150's ubwc config has none */
		*value = 0;
		return true;
	case 0x02:                              /* GMEM_SIZE */
		*value = GMEM_SIZE;
		return true;
	case 0x06:                              /* GMEM_BASE: 1M below the A650 family */
		*value = GMEM_BASE;
		return true;
	case 0x03:                              /* CHIP_ID, with the speed bin above it */
		*value = GPU_CHIP_ID | (uint64_t)speed_bin << 32;
		return true;
	case 0x04:                              /* MAX_FREQ */
		*value = core_khz * 1000ull;
		return true;
	case 0x05:                              /* TIMESTAMP */
		*value = mach_absolute_time();
		return true;
	case 0x07:                              /* PRIORITIES */
		*value = 1;
		return true;
	case 0x10:                              /* HIGHEST_BANK_BIT */
		*value = 14;
		return true;
	case 0x12:                              /* UBWC_SWIZZLE: levels 2 and 3 */
		*value = 6;
		return true;
	default:
		return false;                   /* (VA_START, VA_SIZE: as hakios.adreno.c without gpu_va) */
	}
}

static bool
a618_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return space_map(&core_space, address, phys, PTE_CORE | (read_only ? PTE_READ_ONLY : 0));
}

static void
a618_gpu_mapped(void)
{
	space_commit(&core_space);
}

static void
a618_gpu_unmap(uint64_t address, uint64_t size)
{
	uint32_t context = smmu_context(&core_space);

	for (uint64_t offset = 0; offset < size; offset += 4096) {
		space_map(&core_space, address + offset, 0, 0);
	}
	space_commit(&core_space);
	wr(smmu.base + context + SMMU_CB_TLBIASID, core_space.context);
	wr(smmu.base + context + SMMU_CB_TLBSYNC, 1);
	for (int i = 0; i < 1000 && (rd(smmu.base + context + SMMU_CB_TLBSTATUS) & 1); i++) {
	}
}

static bool
a618_gpu_done(uint32_t fence)
{
	volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);

	to_memory((vm_offset_t)word, 4);
	return (int32_t)(*word - fence) >= 0;
}

static uint32_t
a618_gpu_last(void)
{
	return gpu_fence;
}

/* The core is to write down what time it is: at one end of submission `fence`. */
static void
ring_time(uint32_t fence, uint32_t end)
{
	ring_command(CP_REG_TO_MEM, 3);
	ring(CP_ALWAYS_ON_COUNTER | 2u << 18 | 1u << 30);       /* two words, a 64-bit register */
	ring(CORE_SCRATCH_ADDRESS + GPU_TIMES_AT + 16 * (fence % GPU_TIMES) + 8 * end);
	ring(0);
}

/* Add up the times of the submissions the core has finished since this was last done. */
static void
gpu_times(void)
{
	volatile uint64_t *times = (volatile uint64_t *)(core_scratch.memory + GPU_TIMES_AT);
	uint32_t done;

	to_memory(core_scratch.memory, 4096);
	done = *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);
	if (done - gpu_timed > GPU_TIMES) {
		gpu_timed = done - GPU_TIMES;           /* (the older ones have been written over) */
	}
	while (gpu_timed != done) {
		uint32_t slot = ++gpu_timed % GPU_TIMES;

		if (gpu_timed != 0 && times[2 * slot + 1] > times[2 * slot]) {
			static const uint32_t under[6] = { 1920, 4800, 9600, 19200, 38400, 76800 };     /* (19.2 ticks a microsecond) */
			uint64_t ticks = times[2 * slot + 1] - times[2 * slot];
			unsigned by = 0;

			if (gpu_log) {
				hakios_tl("tl: q %u took %llu\n", gpu_timed, ticks * 10 / 192);
			}
			while (by < 6 && ticks >= under[by]) {
				by++;
			}
			gpu_timed_by[by]++;
			gpu_ticks += ticks;
			gpu_timed_count++;
		}
	}
}

static void
a618_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	gpu_times();
	*microseconds = gpu_ticks * 10 / 192;
	*submissions = gpu_timed_count;
	memcpy(by, gpu_timed_by, sizeof(gpu_timed_by));
	bzero(gpu_timed_by, sizeof(gpu_timed_by));
	gpu_ticks = 0;
	gpu_timed_count = 0;
}

/*
 * Command buffers for the 3D core to run, in order: each an address and a count of words. As
 * Linux's a6xx_submit puts them in the ring. Returns the submission's number; 0: the ring is
 * full and the core is not reading it.
 */
static uint32_t
a618_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t wanted = 26 + 4 * count, room = 0;

	for (int i = 0; i < 20000; i++) {
		room = (rd(gpu.base + CP_RB_RPTR) + RING_WORDS - ring_at - 1) % RING_WORDS;
		if (room >= wanted) {
			break;
		}
		sleep_us(100);
	}
	if (room < wanted) {
		return 0;
	}
	gpu_times();
	if (++gpu_fence == 0) {
		gpu_fence = 1;
	}
	ring_time(gpu_fence, 0);
	ring_command(CP_EVENT_WRITE, 1);
	ring(EVENT_INVALIDATE_DEPTH);
	ring_command(CP_EVENT_WRITE, 1);
	ring(EVENT_INVALIDATE_COLOR);
	for (uint32_t i = 0; i < count; i++) {
		ring_command(CP_INDIRECT_BUFFER, 3);
		ring((uint32_t)addresses[i]);
		ring((uint32_t)(addresses[i] >> 32));
		ring(words[i]);
	}
	if (gpu_exact) {
		ring_command(CP_WAIT_FOR_IDLE, 0);      /* the time is the pipeline's, not the command processor's, which is ahead of it */
	}
	ring_time(gpu_fence, 1);
	ring_register(CP_SCRATCH_REG2, gpu_fence);
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_CACHE_FLUSH);
	ring(CORE_SCRATCH_ADDRESS + GPU_FENCE_AT);
	ring(0);
	ring(gpu_fence);
	ring_send();
	return gpu_fence;
}

/* A submission does not come back: what the 3D core and its SMMU context say. */
static void
a618_gpu_report(const char *why)
{
	uint32_t context = smmu_context(&core_space);

	printf("a618: gpu: %s: submission %u is the last, the core has done %u; ring: read up to %u, written up to %u; status "
	    "%#x, interrupts %#x; command processor: fault %#x, interrupts %#x, protection %#x\n", why, gpu_fence,
	    *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT), rd(gpu.base + CP_RB_RPTR), ring_at,
	    rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_HW_FAULT),
	    rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS));
	printf("a618: gpu:   in the first command buffer at %#llx, %u words to go; in the second at %#llx, %u to go\n",
	    (uint64_t)rd(gpu.base + CP_IB1_BASE + 4) << 32 | rd(gpu.base + CP_IB1_BASE), rd(gpu.base + CP_IB1_REM_SIZE),
	    (uint64_t)rd(gpu.base + CP_IB2_BASE + 4) << 32 | rd(gpu.base + CP_IB2_BASE), rd(gpu.base + CP_IB2_REM_SIZE));
	step = 0;
	if (space_faulted(&core_space)) {
		wr(smmu.base + context + SMMU_CB_FSR, SMMU_FSR_ALL);    /* so that the next one shows */
	}
}

/*
 * The run ends in order (hakios_adreno_stop, HakiosGPUStop): what the core has been given it
 * finishes first, half a second at the most, and then everything the steps started is stopped
 * again, as at the end of a run of the steps alone.
 */
static void
a618_gpu_stop(void)
{
	uint32_t waited = 0;

	if (!gpu_kept) {
		return;
	}
	gpu_kept = false;
	quiet = false;
	while (!a618_gpu_done(gpu_fence) && waited < 5000) {
		sleep_us(100);
		waited++;
	}
	printf("a618: the run is about to end: the GPU is given back (%s after %u us)\n",
	    a618_gpu_done(gpu_fence) ? "it had finished what it was given" : "STILL AT WORK", 100 * waited);
	steps_undo();
}

/*
 * A panic: nothing may wait, and the GPU block must not go on reading or writing memory that the
 * next kernel is about to use. Its bus halted, the GMU's processor in reset, both SMMU contexts
 * off; the zap shader stays (step 19 of the next kernel has TrustZone give it up first).
 */
static void
a618_quiesce(void)
{
	if (core_runs) {
		wr(gpu.base + CP_SQE_CNTL, 0);
		wr(gbif.base + GBIF_HALT, 1);
		for (int i = 0; i < 100000 && !(rd(gbif.base + GBIF_HALT_ACK) & 1); i++) {
		}
		wr(gbif.base + GBIF_HALT, 2);
		for (int i = 0; i < 100000 && !(rd(gbif.base + GBIF_HALT_ACK) & 2); i++) {
		}
		core_runs = false;
	}
	if (gmu_runs) {
		wr(gmu.base + GMU_CM3_SYSRESET, 1);
		gmu_runs = false;
	}
	if (core_space_is_on) {
		wr(smmu.base + smmu_context(&core_space) + SMMU_CB_SCTLR, 0);
		core_space_is_on = false;
	}
	if (gmu_space_is_on) {
		wr(smmu.base + smmu_context(&gmu_space) + SMMU_CB_SCTLR, 0);
		gmu_space_is_on = false;
	}
	gpu_kept = false;
}

/* Is the GPU's firmware in the small image? (gpu=1 says nothing more than that it is not, when it is not.) */
static bool
firmware_there(void)
{
	static const char *const files[] = { "a630_gmu.bin", "a630_sqe.fw", "a615_zap.mdt", "a615_zap.b02" };
	uint64_t size;
	int disk;

	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		if (hakios_image_file("fw", files[i], &size, &disk) == NULL) {
			printf("a618: THE GPU IS NOT STARTED: the small image has no fw/%s (the phone's /vendor/firmware/%s goes "
			    "into fw-gpu/); the system draws with its software renderer\n", files[i], files[i]);
			return false;
		}
	}
	return true;
}

static void
a618_init(void)
{
	static bool (*const steps[])(void) = { step_outside, step_fuses, step_gpucc, step_cx_power, step_clocks,
		step_first_words, step_rscc, step_smmu, step_smmu_context, step_gmu_memory, step_trustzone, step_zap_signature,
		step_gmu_space, step_gmu_firmware, step_gmu_start, step_hfi, step_core_power,
		step_core_space, step_zap_load, step_core_start, step_core_commands,
		step_frame };
	uint32_t last = 0, keep = 0, skip = 0, on = 0;

	if (!PE_parse_boot_argn("a618", &last, sizeof(last))) {
		if (!(PE_parse_boot_argn("gpu", &on, sizeof(on)) && on) || !firmware_there()) {
			return;
		}
		/* gpu=1: as "a618=21 a618_skip=0x1c00 a618_pause=0 a618_keep=1" would, at the highest speed */
		last = 21;
		skip = 1u << 10 | 1u << 11 | 1u << 12;
		pause_ms = 0;
		keep = 1;
		quiet = true;
		speed_wanted = sizeof(gpu_speeds) / sizeof(gpu_speeds[0]) - 1;
	}
	if (last == 0) {
		return;
	}
	PE_parse_boot_argn("a618_pause", &pause_ms, sizeof(pause_ms));
	PE_parse_boot_argn("a618_skip", &skip, sizeof(skip));
	PE_parse_boot_argn("a618_keep", &keep, sizeof(keep));
	PE_parse_boot_argn("a618_speed", &speed_wanted, sizeof(speed_wanted));
	if (last > sizeof(steps) / sizeof(steps[0])) {
		last = sizeof(steps) / sizeof(steps[0]);
	}
	for (newest_step = last; newest_step > 0 && (skip & 1u << newest_step); newest_step--) {
	}
	if (pause_ms == 0) {
		newest_step = 0;
	}
	for (step = 1; step <= last; step++) {
		if (skip & 1u << step) {
			continue;
		}
		if (!steps[step - 1]()) {
			printf("a618: stopped at step %u%s\n", step, keep ? ": THE GPU IS NOT STARTED, the system draws with its "
			    "software renderer" : "");
			quiet = false;
			steps_undo();
			return;
		}
	}
	printf("a618: steps 1 to %u done\n", last);
	if (keep && core_runs && last >= 21) {
		bzero((void *)core_scratch.memory, 4096);
		to_memory(core_scratch.memory, 4096);
		PE_parse_boot_argn("gpu_exact", &gpu_exact, sizeof(gpu_exact));
		gpu_kept = true;
		quiet = true;
		printf("a618: THE GPU STAYS ON for the system to draw with (the 3D core at %u MHz): the service HakiosGPU\n",
		    core_khz / 1000);
		hakios_gpu_backend_kept();
		return;
	}
	quiet = false;
	steps_undo();
}

static const struct hakios_gpu_backend a618_backend = {
	.name = "the Adreno 618 (SM7150)",
	.init = a618_init,
	.kept = a618_kept,
	.parameter = a618_gpu_parameter,
	.map = a618_gpu_map,
	.mapped = a618_gpu_mapped,
	.unmap = a618_gpu_unmap,
	.done = a618_gpu_done,
	.last = a618_gpu_last,
	.worked = a618_gpu_worked,
	.submit = a618_gpu_submit,
	.report = a618_gpu_report,
	.stop = a618_gpu_stop,
	.quiesce = a618_quiesce,
};

/* Registered at boot: from then on the common Adreno driver's steps are not run, and its hakios_gpu_* are this GPU's. */
static void
a618_register(void)
{
	hakios_gpu_backend_register(&a618_backend);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, a618_register);
