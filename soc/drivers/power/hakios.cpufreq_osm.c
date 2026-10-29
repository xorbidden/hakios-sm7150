/*
 * The clock speed of the processor cores the kernel runs on, on the SM7150 (the Mi 9T, davinci), in place
 * of drivers/power/hakios.cpufreq.c, whose EPSS is the SM7325's. Only with boot-arg cpufreq=1.
 *
 * The SM7150 has EPSS's predecessor, the OSM: the same table of frequencies per group of cores, at other
 * offsets and with a row every 32 bytes, and the hardware switches the clock source and the voltage by
 * itself when told which row is wanted (Linux drivers/cpufreq/qcom-cpufreq-hw.c, qcom_soc_data). Two
 * groups: cores 0 to 5 (Kryo 470 Silver) at 0x18323000, 6 and 7 (Kryo 470 Gold) at 0x18325800 (the
 * phone's device tree, qcom,cpucc@18321000; its third block is the L3 cache's). Each group that has one
 * of our cores goes to its top row.
 *
 * What the common driver does besides, voting for the memory's bandwidth, is left out: its addresses are
 * the SM7325's, and the SM7150's boot loader leaves the memory fast enough.
 *
 * Status: seen on the Mi 9T (as cpufreq_osm=1 of the old port): all eight cores at their top speed, the
 * home screen in 48 s instead of 4 min.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

void hakios_cpufreq_init(void);
void hakios_ddr_quiesce(void);

#define OSM_SILVER_PHYS         0x18323000UL
#define OSM_GOLD_PHYS           0x18325800UL
#define OSM_DOMAIN_SIZE         0x1000UL
#define OSM_FREQ_TABLE          0x110           /* a word per row: [31:30] clock source, [7:0] multiple of 19.2 MHz */
#define OSM_TABLE_ROW           32
#define OSM_TABLE_ROWS          40
#define OSM_PERF_STATE          0x920           /* the row that is wanted */
#define OSM_FIRST_GOLD          6               /* the first core of the big group, by its number in MPIDR */
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000          /* source 0: a fixed 300 MHz */

/* the loader's log as well: this runs early, and the kernel's own log of a run keeps only its end */
static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
}

/* How fast a simple loop runs, in thousands of iterations per millisecond. */
static uint32_t
loop_speed(void)
{
	volatile uint64_t sink = 0;
	uint64_t start = mach_absolute_time(), nanos;

	for (uint32_t i = 0; i < 2000000; i++) {
		sink += i;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	return nanos ? (uint32_t)(2000000ULL * 1000 / nanos) : 0;
}

static void
raise_group(const char *name, vm_offset_t phys, uint32_t cores)
{
	uint32_t top = 0, top_khz = 0, was;
	vm_offset_t base = ml_io_map(phys, OSM_DOMAIN_SIZE);

	if (base == 0) {
		tell("cpufreq_osm: %s cores: cannot map %lx\n", name, phys);
		return;
	}
	for (uint32_t row = 0; row < OSM_TABLE_ROWS; row++) {
		uint32_t entry = *(volatile uint32_t *)(base + OSM_FREQ_TABLE + OSM_TABLE_ROW * row);
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;

		if (row > 0 && khz <= top_khz) {
			break;                  /* the table ends by repeating its last row */
		}
		top = row;
		top_khz = khz;
	}
	was = *(volatile uint32_t *)(base + OSM_PERF_STATE);
	*(volatile uint32_t *)(base + OSM_PERF_STATE) = top;
	delay_for_interval(20, NSEC_PER_MSEC);
	tell("cpufreq_osm: %s cores (%u of ours): row %u -> %u of the frequency table, %u MHz\n", name, cores, was, top,
	    top_khz / 1000);
}

void
hakios_cpufreq_init(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t enabled = 0, silver = 0, gold = 0, before;

	if (!PE_parse_boot_argn("cpufreq", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		if (((topology->cpus[i].phys_id >> 8) & 0xff) < OSM_FIRST_GOLD) {
			silver++;
		} else {
			gold++;
		}
	}
	before = loop_speed();
	if (silver != 0) {
		raise_group("small", OSM_SILVER_PHYS, silver);
	}
	if (gold != 0) {
		raise_group("big", OSM_GOLD_PHYS, gold);
	}
	printf("cpufreq_osm: a loop ran at %u, now at %u\n", before, loop_speed());
}

/* no memory vote was made, so none is taken back on the way to the loader */
void
hakios_ddr_quiesce(void)
{
}
