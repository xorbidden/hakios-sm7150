/*
 * The panel of the Mi 9T (davinci, SM7150): a Samsung AMS639RQ08, 1080 x 2340, a command-mode
 * panel on DSI 0. ABL switches it off on every `fastboot boot`, and the UEFI's DisplayDxe
 * brings it back only now and then, so this kernel brings it up itself.
 *
 * Boot-arg panel_on=1, on top of what the UEFI set up (it gets the display's clocks, its DSI link
 * and its pipeline right every time, but never touches the panel itself, and leaves CTL0 driving
 * no interface):
 *   1. the panel's two supplies switched on through RPMh (its reset line is GPIO 9 of the PMIC
 *      PM6150L, which the boot chain leaves released: not touched here);
 *   2. the DSI controller reset (the UEFI leaves it busy for good) and the panel's initialisation
 *      sent over DSI as Linux's panel-samsung-ams639rq08.c sends it;
 *   3. the pipeline finished: INTF1's tear check on the panel's TE line, CTL0 reset and set to
 *      drive INTF1, and the hardware's auto-refresh: a frame on every TE pulse, so whatever is in
 *      the framebuffer (the console, the window server's frames) is on the panel at 60 Hz.
 * Boot-arg panel_pattern=1: three colour bands in the framebuffer first; panel_kick=1: frames
 * started by software instead (~30 Hz), without the auto-refresh; panel_frames=0: no frames at all.
 *
 * Boot-arg panel_probe=1: a look at what the firmware left: which of the display's
 * clocks and its power domain are on, the panel's reset and tear-effect lines, and, when the
 * display block has power and clocks (reading it without them can hang the bus), the state of the
 * DSI controller and of the display pipeline that feeds it. Nothing is written. It looks once as
 * the framebuffer starts and once more 20 seconds later: the UEFI stays resident on core 0.
 *
 * Addresses and offsets: the SM7150 drivers of Linux 7.0 (dispcc-sm7150.c, gcc-sm7150.c,
 * pinctrl-sm7150.c, dsi.xml, dpu_5_2_sm7150.h, dpu_hw_*.c) and the phone's own device tree.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <console/video_console.h>
#include <sys/sysctl.h>

extern struct vc_info vinfo;            /* the boot framebuffer, as the text console has it mapped */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
#define WIMG_WCOMB              0x6             /* VM_WIMG_WCOMB (osfmk/arm/memory_types.h, not seen from here) */
/* devices/davinci/soc/drivers/core/hakios.rpmh.c */
extern uint32_t hakios_rpmh_address(const char *name);
extern bool hakios_rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count);
extern bool hakios_rpmh_ldo(const char *name, uint32_t millivolts);
/* the framebuffer (iokit/Kernel/MiniIOMobileFramebuffer.cpp) and the way back to the loader (osfmk/arm64/loader_return.c) */
struct hakios_panel {
	void            (*start)(void);
	bool            (*second_buffer)(uint64_t * phys, vm_offset_t * mapped);
	uint64_t        (*first_buffer)(void);
	void            (*show)(uint64_t phys);
};
extern void hakios_panel_register(const struct hakios_panel *panel);
extern void hakios_return_register(void (*quiesce)(void), void (*stop)(void));

#define DISPCC_PHYS             0x0af00000UL
#define DISPCC_SIZE             0x10000UL
/* only the pages used: the kernel's room for mappings is small (a 3 MB tile left too little for more) */
#define GCC_PHYS                0x0010b000UL    /* the display's AXI clocks, gcc + 0xb024 and 0xb070 */
#define GCC_SIZE                0x1000UL
#define GCC_DISP_HF_AXI         0x024
#define GCC_DISP_SF_AXI         0x070
#define TLMM_NORTH_PHYS         (0x03900000UL + 0x1000 * GPIO_TOUCH_IRQ)       /* GPIOs 9 to 32 */
#define TLMM_NORTH_SIZE         (0x1000 * (GPIO_ESD - GPIO_TOUCH_IRQ + 1))
#define MDSS_PHYS               0x0ae00000UL    /* the MDSS wrapper; the DPU proper ("mdp") at +0x1000 */
#define MDSS_SIZE               0x90000UL
#define DSI0_PHYS               0x0ae94000UL
#define DSI0_SIZE               0x400UL

/*
 * Where the DSI controller fetches a command from: the end of the display's reserved memory
 * ("Display Reserved", 0x9c000000-0x9d700000), past our 1080 x 2340 framebuffer. The display
 * engine already reads this region (its pipe fetches the framebuffer), so whatever the UEFI's
 * SMMU setup is, it lets the display block in here.
 */
#define DSI_CMD_BUF_PHYS        0x9d600000UL
#define DSI_CMD_BUF_SIZE        0x1000UL


#define CBCR_OFF                (1u << 31)      /* a branch clock's register: the clock is off */
#define GDSC_PWR_ON             (1u << 31)      /* a power domain's register: it is on */
#define DISPCC_MDSS_GDSCR       0x3000

#define GPIO_CFG(n)             (0x1000 * ((n) - GPIO_TOUCH_IRQ))
#define GPIO_IO(n)              (0x1000 * ((n) - GPIO_TOUCH_IRQ) + 4)
#define GPIO_TOUCH_IRQ          9               /* (not the panel's reset: that is PM6150L's GPIO 9) */
#define GPIO_TE                 10
#define GPIO_ESD                32

#define DPU(off)                (0x1000 + (off))
#define CTL(n)                  DPU(0x1000 + 0x200 * (n))
#define SSPP_VIG(n)             DPU(0x4000 + 0x2000 * (n))
#define SSPP_DMA(n)             DPU(0x24000 + 0x2000 * (n))
#define PINGPONG(n)             DPU(0x70000 + 0x800 * (n))
#define INTF1                   DPU(0x6a800)    /* DSI 0 */
#define LM0                     DPU(0x44000)

/* dpu_hw_ctl.c: the interface a CTL drives, and what a flush latches */
#define CTL_TOP                 0x014
#define CTL_FLUSH               0x018
#define CTL_START               0x01c
#define CTL_SW_RESET            0x030           /* [0]: reset, cleared by the hardware when done */
#define CTL_PREPARE             0x0d0           /* dpu_hw_ctl_trigger_pending(): before every command-mode kickoff */
#define CTL_TOP_INTF1           (2u << 4)       /* (INTF_1 & 0xf) << 4 */
#define CTL_TOP_CMD_MODE        (1u << 17)
#define CTL_FLUSH_VIG0          (1u << 0)
#define CTL_FLUSH_LM0           (1u << 6)
#define CTL_FLUSH_CTL           (1u << 17)
#define CTL_FLUSH_INTF1         (1u << 30)
#define INTF_MUX                0x25c           /* dpu_hw_intf_bind_pingpong_blk(): [3:0] the ping-pong, 0 = PP 0 */
/* DPU 5.x has the tear check in the INTF (dpu_hw_intf.c, Linux's has_intf_te) */
#define INTF_TEAR_MDP_VSYNC_SEL 0x280           /* 0: the TE GPIO (mdp_vsync, GPIO 10) */
#define INTF_TEAR_CHECK_EN      0x284
#define INTF_TEAR_SYNC_VSYNC    0x288           /* [19] counter on, [20] counted by the panel's TE, [15:0] clocks per line */
#define INTF_TEAR_SYNC_HEIGHT   0x28c
#define INTF_TEAR_SYNC_WRCOUNT  0x290
#define INTF_TEAR_VSYNC_INIT    0x294
#define INTF_TEAR_INT_COUNT     0x298           /* [31:16] frames, [15:0] the read pointer's line */
#define INTF_TEAR_SYNC_THRESH   0x29c
#define INTF_TEAR_START_POS     0x2a0
#define INTF_TEAR_RD_PTR_IRQ    0x2a4
#define INTF_TEAR_LINE_COUNT    0x2b0
#define INTF_TEAR_AUTOREFRESH   0x2b4           /* [31] on, [15:0] a frame every so many TE pulses */
#define DPU_INTR_CLEAR          DPU(0x018)

#define FB_PHYS                 0x9c000000UL    /* the UEFI's framebuffer, VIG0's source */
#define FB2_PHYS                0x9ca00000UL    /* a second one, past the first (10 MB), before the command buffer */
#define SSPP_SRC0_ADDR          0x14            /* of a pipe (dpu_hw_sspp.c): where it fetches from */
#define FB_WIDTH                1080
#define FB_HEIGHT               2340
#define FB_STRIDE               (FB_WIDTH * 4)
#define FB_VTOTAL               (FB_HEIGHT + 64 + 20 + 64)     /* the panel's vertical porches and pulse */
#define VSYNC_CLOCK_HZ          19200000        /* dispcc's mdss vsync clock: the crystal */

/* the DSI "6G" controller: dsi.xml's offsets plus 4 (DSI_6G_REG_SHIFT) */
#define DSI_HW_VERSION          0x000
#define DSI_CTRL                0x004
#define DSI_STATUS0             0x008
#define DSI_FIFO_STATUS         0x00c
#define DSI_CMD_DMA_CTRL        0x03c
#define DSI_CMD_DMA_FROM_MEMORY (1u << 28)      /* FROM_FRAME_BUFFER: the command is in memory (DMA_BASE) */
#define DSI_CMD_DMA_LOW_POWER   (1u << 26)
#define DSI_CMD_CFG0            0x040
#define DSI_DMA_BASE            0x048
#define DSI_DMA_LEN             0x04c
#define DSI_CMD_STREAM0_CTRL    0x058
#define DSI_CMD_STREAM0_TOTAL   0x05c
#define DSI_ACK_ERR_STATUS      0x068
#define DSI_TRIG_CTRL           0x084
#define DSI_TRIG_DMA            0x090
#define DSI_LANE_STATUS         0x0a8
#define DSI_LANE_CTRL           0x0ac
#define DSI_DLN0_PHY_ERR        0x0b4
#define DSI_TIMEOUT_STATUS      0x0c0
#define DSI_RESET               0x118
#define DSI_INTR_CTRL           0x110
#define DSI_INTR_CMD_DMA_DONE   (1u << 0)       /* status, cleared by writing it; the mask (bit 1) stays off */
#define DSI_INTR_CMD_MDP_DONE   (1u << 8)       /* a frame from the display pipeline is out */
#define DSI_STATUS_MDP_BUSY     (1u << 2)
#define DSI_FIFO_CMD_MDP_UNDERFLOW (1u << 7)    /* the pipeline did not keep up with the link in mid-frame */
#define DSI_CLK_CTRL            0x11c
#define DSI_CLK_STATUS          0x120

static vm_offset_t dispcc, gcc, tlmm, mdss, dsi0, cmd_buf;
static uint64_t cmd_buf_phys = DSI_CMD_BUF_PHYS;
static thread_call_t later_call;

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

static bool
clock_on(uint32_t cbcr)
{
	return (rd(dispcc + cbcr) & CBCR_OFF) == 0;
}

static void
probe(const char *when)
{
	static const struct {
		const char *name;
		uint32_t cbcr;
	} clocks[] = {
		{ "ahb", 0x2080 }, { "non-gdsc-ahb", 0x4004 }, { "mdp", 0x200c }, { "lut", 0x201c }, { "rot", 0x2014 },
		{ "vsync", 0x2024 }, { "byte0", 0x2028 }, { "byte0-intf", 0x202c }, { "pclk0", 0x2004 },
		{ "esc0", 0x2038 }, { "rscc-ahb", 0x400c }, { "rscc-vsync", 0x4008 },
	};
	uint32_t gdsc = rd(dispcc + DISPCC_MDSS_GDSCR);
	char line[256];
	size_t len = 0;

	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		int n = snprintf(line + len, sizeof(line) - len, " %s %s", clocks[i].name, clock_on(clocks[i].cbcr) ? "on" : "off");

		if (n > 0) {
			len = MIN(len + (size_t)n, sizeof(line) - 1);
		}
	}
	printf("panel probe (%s): MDSS power domain %s (%#x); dispcc:%s\n", when, (gdsc & GDSC_PWR_ON) ? "on" : "off",
	    gdsc, line);
	printf("panel probe (%s): gcc disp hf-axi %s, sf-axi %s; touch IRQ %u: cfg %#x io %#x, TE %u: cfg %#x io %#x, "
	    "ESD %u: cfg %#x io %#x\n", when,
	    (rd(gcc + GCC_DISP_HF_AXI) & CBCR_OFF) ? "off" : "on", (rd(gcc + GCC_DISP_SF_AXI) & CBCR_OFF) ? "off" : "on",
	    GPIO_TOUCH_IRQ, rd(tlmm + GPIO_CFG(GPIO_TOUCH_IRQ)), rd(tlmm + GPIO_IO(GPIO_TOUCH_IRQ)),
	    GPIO_TE, rd(tlmm + GPIO_CFG(GPIO_TE)), rd(tlmm + GPIO_IO(GPIO_TE)),
	    GPIO_ESD, rd(tlmm + GPIO_CFG(GPIO_ESD)), rd(tlmm + GPIO_IO(GPIO_ESD)));

	if (!(gdsc & GDSC_PWR_ON) || !clock_on(0x2080) || !clock_on(0x200c)) {
		printf("panel probe (%s): the display block has no power or clocks: not looking inside\n", when);
		return;
	}
	printf("panel probe (%s): MDSS hw %#x, DPU rev %#x; DSI0 hw %#x ctrl %#x status %#x fifo %#x cmd-cfg0 %#x "
	    "stream0 %#x/%#x ack-err %#x trig %#x lanes %#x/%#x phy-err %#x timeout %#x intr %#x clk %#x/%#x\n", when,
	    rd(mdss + 0x0), rd(mdss + DPU(0x0)),
	    rd(dsi0 + DSI_HW_VERSION), rd(dsi0 + DSI_CTRL), rd(dsi0 + DSI_STATUS0), rd(dsi0 + DSI_FIFO_STATUS),
	    rd(dsi0 + DSI_CMD_CFG0), rd(dsi0 + DSI_CMD_STREAM0_CTRL), rd(dsi0 + DSI_CMD_STREAM0_TOTAL),
	    rd(dsi0 + DSI_ACK_ERR_STATUS), rd(dsi0 + DSI_TRIG_CTRL), rd(dsi0 + DSI_LANE_STATUS), rd(dsi0 + DSI_LANE_CTRL),
	    rd(dsi0 + DSI_DLN0_PHY_ERR), rd(dsi0 + DSI_TIMEOUT_STATUS), rd(dsi0 + DSI_INTR_CTRL),
	    rd(dsi0 + DSI_CLK_CTRL), rd(dsi0 + DSI_CLK_STATUS));
	/* which of the blocks the firmware used is not known: the first few of each */
	for (unsigned n = 0; n < 3; n++) {
		printf("panel probe (%s): CTL%u layer0 %#x layer1 %#x top %#x flush %#x start %#x prepare %#x\n", when, n,
		    rd(mdss + CTL(n) + 0x00), rd(mdss + CTL(n) + 0x04), rd(mdss + CTL(n) + 0x14), rd(mdss + CTL(n) + 0x18),
		    rd(mdss + CTL(n) + 0x1c), rd(mdss + CTL(n) + 0xd0));
	}
	for (unsigned n = 0; n < 2; n++) {
		printf("panel probe (%s): PP%u te-en %#x sync-vsync %#x height %#x wrcount %#x int-count %#x rd-ptr %#x "
		    "line %#x autorefresh %#x\n", when, n,
		    rd(mdss + PINGPONG(n) + 0x00), rd(mdss + PINGPONG(n) + 0x04), rd(mdss + PINGPONG(n) + 0x08),
		    rd(mdss + PINGPONG(n) + 0x0c), rd(mdss + PINGPONG(n) + 0x14), rd(mdss + PINGPONG(n) + 0x20),
		    rd(mdss + PINGPONG(n) + 0x2c), rd(mdss + PINGPONG(n) + 0x30));
	}
	printf("panel probe (%s): INTF1 timing-en %#x config %#x config2 %#x panel-format %#x frames %#x line %#x "
	    "mux %#x status %#x\n", when,
	    rd(mdss + INTF1 + 0x000), rd(mdss + INTF1 + 0x004), rd(mdss + INTF1 + 0x060), rd(mdss + INTF1 + 0x090),
	    rd(mdss + INTF1 + 0x0ac), rd(mdss + INTF1 + 0x0b0), rd(mdss + INTF1 + 0x25c), rd(mdss + INTF1 + 0x26c));
	for (unsigned n = 0; n < 2; n++) {
		printf("panel probe (%s): VIG%u src-size %#x xy %#x out-size %#x addr %#x stride %#x format %#x op %#x\n",
		    when, n, rd(mdss + SSPP_VIG(n) + 0x00), rd(mdss + SSPP_VIG(n) + 0x08), rd(mdss + SSPP_VIG(n) + 0x0c),
		    rd(mdss + SSPP_VIG(n) + 0x14), rd(mdss + SSPP_VIG(n) + 0x24), rd(mdss + SSPP_VIG(n) + 0x30),
		    rd(mdss + SSPP_VIG(n) + 0x38));
	}
	for (unsigned n = 0; n < 3; n++) {
		printf("panel probe (%s): DMA%u src-size %#x xy %#x out-size %#x addr %#x stride %#x format %#x op %#x\n",
		    when, n, rd(mdss + SSPP_DMA(n) + 0x00), rd(mdss + SSPP_DMA(n) + 0x08), rd(mdss + SSPP_DMA(n) + 0x0c),
		    rd(mdss + SSPP_DMA(n) + 0x14), rd(mdss + SSPP_DMA(n) + 0x24), rd(mdss + SSPP_DMA(n) + 0x30),
		    rd(mdss + SSPP_DMA(n) + 0x38));
	}
}

/* ---- step 1: the panel's supplies, through RPMh (as drivers/input/hakios.goodix.c) ---- */



/*
 * The memory bandwidth the display's fetches need (boot-arg panel_bw=0: none): VIG0 reads the whole
 * 10 MB framebuffer 60 times a second, and the buses run only as fast as someone voted for. The
 * firmware's own votes are of a middle range (hakios.cpufreq.c measured it); with them, most
 * runs' frames underflowed (stripes). A vote, the most there is, as Linux's interconnect driver
 * casts it (icc-bcm-voter.c: commit, valid, the average and the peak, 14 bits each): on the memory
 * controller (MC0), the shared hub in front of it (SH0) and the multimedia bus the display block
 * fetches through (MM0), and the memory kept on for this processor (ACV, as the firmware had it).
 */
#define BCM_COMMIT              (1u << 30)
#define BCM_VALID               (1u << 29)
#define BCM_VOTE(x, y)          (BCM_COMMIT | BCM_VALID | ((uint32_t)(x) & 0x3fff) << 14 | ((uint32_t)(y) & 0x3fff))
#define BCM_ACV_APPS            8

static void
bandwidth_vote(void)
{
	uint32_t enabled = 1, address[3], data[3];
	uint32_t mc0 = hakios_rpmh_address("MC0"), sh0 = hakios_rpmh_address("SH0"), mm0 = hakios_rpmh_address("MM0"),
	    acv = hakios_rpmh_address("ACV");

	PE_parse_boot_argn("panel_bw", &enabled, sizeof(enabled));
	if (!enabled) {
		return;
	}
	if (mc0 == 0 || sh0 == 0 || mm0 == 0 || acv == 0) {
		printf("panel: no bandwidth vote (MC0 %#x, SH0 %#x, MM0 %#x, ACV %#x)\n", mc0, sh0, mm0, acv);
		return;
	}
	address[0] = mm0;
	data[0] = BCM_VOTE(0x3fff, 0x3fff);
	address[1] = mc0;
	data[1] = BCM_VOTE(0x3fff, 0x3fff);
	address[2] = sh0;
	data[2] = BCM_VOTE(0x3fff, 0x3fff);
	if (!hakios_rpmh_write(address, data, 3)) {
		return;
	}
	address[0] = acv;
	data[0] = BCM_VOTE(0, BCM_ACV_APPS);
	hakios_rpmh_write(address, data, 1);
	printf("panel: bandwidth voted, the most there is, on MM0 (%#x), MC0 (%#x), SH0 (%#x)\n", mm0, mc0, sh0);
}


static bool
panel_power(void)
{
	/* vddio 1.88 V, then vdda-3p3 3.0 V (the stock "dsi_panel_pwr_supply_labibb_amoled" order) */
	if (!hakios_rpmh_ldo("ldoa13", 1880) || !hakios_rpmh_ldo("ldoa18", 3000)) {
		return false;
	}
	bandwidth_vote();
	return true;
}

/* ---- step 2: the panel's initialisation over DSI ---- */

#define MIPI_DSI_DCS_SHORT_WRITE        0x05
#define MIPI_DSI_DCS_SHORT_WRITE_PARAM  0x15
#define MIPI_DSI_DCS_LONG_WRITE         0x39

static unsigned dsi_errors;
/*
 * One DCS write (the command byte, then its parameters) in low-power mode, through the controller's
 * command DMA: the packet in memory in the MSM layout (dsi_host.c's dsi_cmd_dma_add(): the two
 * header bytes after the type, then a byte of flags; the payload; 0xff up to four bytes), the
 * hardware adds ECC and checksum.
 */
static bool
dcs(const uint8_t *bytes, unsigned len)
{
	volatile uint8_t *buf = (volatile uint8_t *)cmd_buf;
	unsigned size, i;
	uint64_t until;

	if (len == 0 || len > 64) {
		return false;
	}
	if (len <= 2) {
		buf[0] = bytes[0];
		buf[1] = len == 2 ? bytes[1] : 0;
		buf[2] = len == 2 ? MIPI_DSI_DCS_SHORT_WRITE_PARAM : MIPI_DSI_DCS_SHORT_WRITE;
		buf[3] = 0x80;                  /* the last packet */
		size = 4;
	} else {
		buf[0] = (uint8_t)len;
		buf[1] = 0;
		buf[2] = MIPI_DSI_DCS_LONG_WRITE;
		buf[3] = 0x80 | 0x40;           /* the last packet, a long one */
		for (i = 0; i < len; i++) {
			buf[4 + i] = bytes[i];
		}
		size = 4 + len;
		while (size & 3) {
			buf[size++] = 0xff;
		}
	}
	__builtin_arm_dsb(0xf);           /* dsb sy */

	wr(dsi0 + DSI_INTR_CTRL, rd(dsi0 + DSI_INTR_CTRL) | DSI_INTR_CMD_DMA_DONE);  /* (write one to clear) */
	wr(dsi0 + DSI_DMA_BASE, (uint32_t)cmd_buf_phys);
	wr(dsi0 + DSI_DMA_LEN, size);
	__builtin_arm_dsb(0xf);           /* dsb sy */
	wr(dsi0 + DSI_TRIG_DMA, 1);
	__builtin_arm_dsb(0xf);           /* dsb sy */

	nanoseconds_to_absolutetime(200 * NSEC_PER_MSEC, &until);  /* as long as Linux waits */
	until += mach_absolute_time();
	while ((rd(dsi0 + DSI_INTR_CTRL) & DSI_INTR_CMD_DMA_DONE) == 0) {
		if (mach_absolute_time() > until) {
			if (dsi_errors++ < 4) {
				printf("panel: DCS %02x (%u bytes): no DMA done after 200 ms; status %#x fifo %#x ack-err %#x "
				    "intr %#x dma-ctrl %#x\n", bytes[0], len, rd(dsi0 + DSI_STATUS0), rd(dsi0 + DSI_FIFO_STATUS),
				    rd(dsi0 + DSI_ACK_ERR_STATUS), rd(dsi0 + DSI_INTR_CTRL), rd(dsi0 + DSI_CMD_DMA_CTRL));
			}
			return false;
		}
	}
	wr(dsi0 + DSI_INTR_CTRL, rd(dsi0 + DSI_INTR_CTRL) | DSI_INTR_CMD_DMA_DONE);
	return true;
}

#define DCS(...) do { static const uint8_t b_[] = { __VA_ARGS__ }; ok += dcs(b_, sizeof(b_)); n++; } while (0)

/*
 * The controller as the UEFI leaves it is busy for good (command engine, DMA and MDP path: status
 * 7) and takes no command. A reset of it the way Linux's dsi_sw_reset() does it: off, every clock
 * of it forced on, reset, and then the UEFI's configuration (link, lanes, timing, streams) put back.
 */
static void
dsi_soft_reset(void)
{
	/* Every register of the controller's is put back but these: status, the triggers, the reset itself. */
	static const uint16_t not_config[] = {
		0x000 /* HW_VERSION */, 0x004 /* CTRL: last */, 0x008 /* STATUS0 */, 0x00c /* FIFO_STATUS */,
		0x068 /* ACK_ERR_STATUS */, 0x090 /* TRIG_DMA */, 0x0b4 /* DLN0_PHY_ERR */, 0x0c0 /* TIMEOUT_STATUS */,
		0x110 /* INTR_CTRL */, 0x118 /* RESET */, 0x11c /* CLK_CTRL: last */, 0x120 /* CLK_STATUS */,
		0x12c /* PHY_RESET */, 0x184 /* TPG stream 0 trigger */, 0x1f4 /* VERSION */,
	};
	static uint32_t saved[0x300 / 4];
	uint32_t ctrl = rd(dsi0 + DSI_CTRL), clk = rd(dsi0 + DSI_CLK_CTRL);

	for (unsigned i = 0; i < 0x300 / 4; i++) {
		saved[i] = rd(dsi0 + 4 * i);
	}
	wr(dsi0 + DSI_CTRL, ctrl & ~1u);
	__builtin_arm_dsb(0xf);
	wr(dsi0 + DSI_CLK_CTRL, clk | 0x3f);            /* the controller can only be reset with its clocks running */
	__builtin_arm_dsb(0xf);
	wr(dsi0 + DSI_RESET, 1);
	sleep_us(20000);                                /* DSI_RESET_TOGGLE_DELAY_MS */
	wr(dsi0 + DSI_RESET, 0);
	__builtin_arm_dsb(0xf);
	for (unsigned i = 0; i < 0x300 / 4; i++) {
		bool skip = false;

		for (unsigned k = 0; k < sizeof(not_config) / sizeof(not_config[0]); k++) {
			skip |= not_config[k] == 4 * i;
		}
		if (!skip && rd(dsi0 + 4 * i) != saved[i]) {
			wr(dsi0 + 4 * i, saved[i]);
		}
	}
	wr(dsi0 + DSI_CLK_CTRL, clk);
	wr(dsi0 + DSI_CTRL, ctrl);
	wr(dsi0 + DSI_FIFO_STATUS, 0xffffffff);         /* its error flags stay until written: only new ones after this */
	__builtin_arm_dsb(0xf);
	printf("panel: DSI reset: status %#x ctrl %#x cmd-cfg0 %#x dma-ctrl %#x lanes %#x\n", rd(dsi0 + DSI_STATUS0),
	    rd(dsi0 + DSI_CTRL), rd(dsi0 + DSI_CMD_CFG0), rd(dsi0 + DSI_CMD_DMA_CTRL), rd(dsi0 + DSI_LANE_STATUS));
}

static void
panel_init(void)
{
	unsigned ok = 0, n = 0;
	uint32_t dma_ctrl = rd(dsi0 + DSI_CMD_DMA_CTRL), ctrl = rd(dsi0 + DSI_CTRL);

	printf("panel: DSI before: ctrl %#x status %#x dma-ctrl %#x trig %#x intr %#x\n", ctrl, rd(dsi0 + DSI_STATUS0),
	    dma_ctrl, rd(dsi0 + DSI_TRIG_CTRL), rd(dsi0 + DSI_INTR_CTRL));
	if (rd(dsi0 + DSI_STATUS0) & 0x3) {
		dsi_soft_reset();
		dma_ctrl = rd(dsi0 + DSI_CMD_DMA_CTRL);
		ctrl = rd(dsi0 + DSI_CTRL);
	}
	/* commands from memory, in low-power mode (stock "dsi_lp_mode"); the controller on and in command mode */
	wr(dsi0 + DSI_CMD_DMA_CTRL, dma_ctrl | DSI_CMD_DMA_FROM_MEMORY | DSI_CMD_DMA_LOW_POWER);
	wr(dsi0 + DSI_CTRL, ctrl | (1u << 2) | (1u << 0));

	/* panel-samsung-ams639rq08.c, ams639rq08_on() */
	DCS(0xf0, 0x5a, 0x5a);
	DCS(0xfc, 0x5a, 0x5a);
	DCS(0xb0, 0x0c);
	DCS(0xff, 0x10);
	DCS(0xb0, 0x2f);
	DCS(0xd1, 0x01);
	DCS(0xf0, 0xa5, 0xa5);
	DCS(0xfc, 0xa5, 0xa5);
	DCS(0x11);                                      /* sleep out */
	sleep_us(10000);
	DCS(0xf0, 0x5a, 0x5a);
	DCS(0x35, 0x00);                                /* tear effect on, vertical blanking */
	DCS(0xb7, 0x01, 0x4b);
	DCS(0xb0, 0x06);
	DCS(0xb7, 0x10);
	DCS(0xf0, 0xa5, 0xa5);
	DCS(0x2b, 0x00, 0x00, 0x09, 0x23);              /* page address: rows 0-2339 */
	DCS(0xf0, 0x5a, 0x5a);
	DCS(0xfc, 0x5a, 0x5a);
	DCS(0xb0, 0x23);
	DCS(0xd1, 0x11);
	DCS(0xe9, 0x11, 0x55, 0xa6, 0x75, 0xa3, 0xb9, 0xa1, 0x4a, 0x00, 0x1a, 0xb8);
	DCS(0xe1, 0x00, 0x00, 0x02, 0x02, 0x42, 0x02);
	DCS(0xe2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	DCS(0xb0, 0x0c);
	DCS(0xe1, 0x19);
	DCS(0xf0, 0xa5, 0xa5);
	DCS(0xfc, 0xa5, 0xa5);
	DCS(0x53, 0x20);                                /* write control display: brightness control on */
	DCS(0x51, 0x00, 0x00);
	DCS(0x55, 0x00);                                /* write power save */
	sleep_us(67000);
	DCS(0x29);                                      /* display on */
	/* Linux leaves the brightness to its backlight device: here a quarter of the 11-bit range */
	DCS(0x51, 0x01, 0xff);

	printf("panel: %u of %u commands sent; DSI after: status %#x ack-err %#x intr %#x\n", ok, n,
	    rd(dsi0 + DSI_STATUS0), rd(dsi0 + DSI_ACK_ERR_STATUS), rd(dsi0 + DSI_INTR_CTRL));
}

/* ---- step 3: frames ---- */

/*
 * The UEFI leaves CTL0 in command mode with VIG0 on mixer 0, but driving no interface (CTL_TOP
 * 0x20000): no frame ever leaves. Here it drives INTF1 (DSI 0) through ping-pong 0, everything is
 * latched once, and then a CTL_START sends the framebuffer to the panel: a frame of a command-mode
 * panel is one such transfer into its memory. No tear check yet (the transfer does not wait for
 * the panel's TE line): a frame may tear.
 *
 * The transfers are started by a thread of ours ~30 times a second, whenever the last one is out
 * (the DSI's MDP path idle), whatever is in the framebuffer: a refresh in software, so the console
 * and the window server reach the panel without the framebuffer driver knowing of it.
 */
static uint32_t frames_started, frames_busy, dsi_resets;

static void dsi_soft_reset(void);
static void ctl0_reset(void);
static void frames_go(void);

/* a clock's RCG (dispcc-sm7150.c): CFG at +4 holds the source ([10:8]) and the divider ([4:0], 2 x div - 1) */
#define RCG_CFG(cmd_rcgr)       ((cmd_rcgr) + 4)
#define RCG_MDP                 0x20c8
#define RCG_BYTE0               0x2110
#define RCG_PCLK0               0x2098
#define RCG_ESC0                0x2148
#define DPU_INTR_STATUS         DPU(0x014)      /* MDP_SSPP_TOP0_INTR: INTF1 underrun 26, vsync 27, PP0 done 8 */
#define DPU_INTR2_STATUS        DPU(0x01c)
#define MDSS_HW_INTR_STATUS     0x010

static void
pipeline_report(const char *when)
{
	printf("panel: frames (%s): CTL0 top %#x flush %#x start %#x; LM0 op %#x out %#x; INTF1 mux %#x frames %#x "
	    "line %#x status %#x; PP0 line %#x; DSI status %#x intr %#x fifo %#x; %u started, %u while busy\n", when,
	    rd(mdss + CTL(0) + CTL_TOP), rd(mdss + CTL(0) + CTL_FLUSH), rd(mdss + CTL(0) + CTL_START),
	    rd(mdss + LM0 + 0x00), rd(mdss + LM0 + 0x04),
	    rd(mdss + INTF1 + INTF_MUX), rd(mdss + INTF1 + 0x0ac), rd(mdss + INTF1 + 0x0b0), rd(mdss + INTF1 + 0x26c),
	    rd(mdss + PINGPONG(0) + 0x2c), rd(dsi0 + DSI_STATUS0), rd(dsi0 + DSI_INTR_CTRL), rd(dsi0 + DSI_FIFO_STATUS),
	    frames_started, frames_busy);
	printf("panel: frames (%s): tear check %#x vsync %#x int-count %#x line %#x autorefresh %#x\n", when,
	    rd(mdss + INTF1 + INTF_TEAR_CHECK_EN), rd(mdss + INTF1 + INTF_TEAR_SYNC_VSYNC),
	    rd(mdss + INTF1 + INTF_TEAR_INT_COUNT), rd(mdss + INTF1 + INTF_TEAR_LINE_COUNT),
	    rd(mdss + INTF1 + INTF_TEAR_AUTOREFRESH));
	printf("panel: frames (%s): clocks: mdp cmd %#x cfg %#x, byte0 %#x/%#x, pclk0 %#x/%#x, esc0 %#x/%#x; "
	    "interrupts: DPU top %#x, top2 %#x, MDSS %#x; %u DSI resets\n", when,
	    rd(dispcc + RCG_MDP), rd(dispcc + RCG_CFG(RCG_MDP)), rd(dispcc + RCG_BYTE0), rd(dispcc + RCG_CFG(RCG_BYTE0)),
	    rd(dispcc + RCG_PCLK0), rd(dispcc + RCG_CFG(RCG_PCLK0)), rd(dispcc + RCG_ESC0), rd(dispcc + RCG_CFG(RCG_ESC0)),
	    rd(mdss + DPU_INTR_STATUS), rd(mdss + DPU_INTR2_STATUS), rd(mdss + MDSS_HW_INTR_STATUS), dsi_resets);
}

/* Three bands, red, green and blue (in whichever order the pipe's format puts them), over the whole screen. */
static void
test_pattern(void)
{
	/* the console's mapping of it (a second 10 MB mapping finds no room; PE_state has the physical address) */
	vm_offset_t fb = (vm_offset_t)vinfo.v_baseaddr;

	if (fb == 0 || vinfo.v_width != FB_WIDTH || vinfo.v_height != FB_HEIGHT || vinfo.v_rowbytes != FB_STRIDE) {
		printf("panel: the boot framebuffer is not the expected 1080 x 2340: no test pattern\n");
		return;
	}
	for (unsigned y = 0; y < FB_HEIGHT; y++) {
		uint32_t color = y < FB_HEIGHT / 3 ? 0x00ff0000 : y < FB_HEIGHT * 2 / 3 ? 0x0000ff00 : 0x000000ff;
		volatile uint32_t *row = (volatile uint32_t *)(fb + (vm_offset_t)y * FB_STRIDE);

		for (unsigned x = 0; x < FB_WIDTH; x++) {
			row[x] = color;
		}
	}
	__builtin_arm_dsb(0xf);
}

static void
frame_start(void)
{
	if (rd(dsi0 + DSI_STATUS0) & DSI_STATUS_MDP_BUSY) {
		frames_busy++;
		return;
	}
	wr(dsi0 + DSI_INTR_CTRL, rd(dsi0 + DSI_INTR_CTRL) | DSI_INTR_CMD_MDP_DONE);
	wr(mdss + CTL(0) + CTL_PREPARE, 1);
	wr(mdss + CTL(0) + CTL_START, 1);
	frames_started++;
}

static uint32_t panel_kick;      /* boot-arg panel_kick=1: frames started by software, not by the hardware's auto-refresh */

static void
refresh_main(__unused void *arg, __unused wait_result_t result)
{
	uint32_t busy_before = 0;

	for (unsigned n = 0; !panel_kick; n++) {
		/* the hardware sends the frames itself: only a word now and then */
		if (n == 1 || (n > 0 && n % 300 == 0)) {
			pipeline_report(n == 1 ? "after a second" : "every 5 minutes");
		}
		/*
		 * A transfer that underflowed (the DSI out of pixels in mid-frame: stripes) never recovers by
		 * itself: the auto-refresh repeats it. It happens now and then from the first frame on, with
		 * the same configuration that gives clean frames other times, so: everything stopped and reset
		 * as at the start, and the frames going again.
		 */
		if ((rd(dsi0 + DSI_FIFO_STATUS) & DSI_FIFO_CMD_MDP_UNDERFLOW) && dsi_resets < 5) {
			printf("panel: frames underflowed (DSI status %#x fifo %#x): restarting them (%u of at most 5)\n",
			    rd(dsi0 + DSI_STATUS0), rd(dsi0 + DSI_FIFO_STATUS), dsi_resets + 1);
			wr(mdss + INTF1 + INTF_TEAR_AUTOREFRESH, 0);
			sleep_us(40000);
			ctl0_reset();
			dsi_soft_reset();
			frames_go();
			dsi_resets++;
		}
		delay_for_interval(1, NSEC_PER_SEC);
	}
	for (unsigned n = 0;; n++) {
		frame_start();
		/* stuck on a frame for a third of a second: the DSI reset, and on (an experiment) */
		if (frames_busy - busy_before >= 10) {
			if (dsi_resets < 3) {
				pipeline_report("stuck, before a DSI reset");
			}
			dsi_soft_reset();
			dsi_resets++;
			busy_before = frames_busy;
		} else if ((rd(dsi0 + DSI_STATUS0) & DSI_STATUS_MDP_BUSY) == 0) {
			busy_before = frames_busy;
		}
		if (n == 30 || n % 600 == 0) {
			pipeline_report(n == 30 ? "after a second" : "every 20 s");
		}
		delay_for_interval(33, NSEC_PER_MSEC);
	}
}

/*
 * CTL0 as an earlier start left it may have a start and a flush pending for good, or a frame in
 * progress: a reset of it (dpu_hw_ctl_reset_control()), and then what the UEFI had staged on it put back.
 */
static void
ctl0_reset(void)
{
	uint32_t layer0 = rd(mdss + CTL(0) + 0x00), layer1 = rd(mdss + CTL(0) + 0x04);
	uint32_t top = rd(mdss + CTL(0) + CTL_TOP);
	unsigned waited = 0;

	wr(mdss + CTL(0) + CTL_SW_RESET, 1);
	while ((rd(mdss + CTL(0) + CTL_SW_RESET) & 1) && waited < 2000) {
		sleep_us(20);
		waited += 20;
	}
	printf("panel: CTL0 reset (%s after %u us): start %#x flush %#x top %#x layer0 %#x; putting back top %#x "
	    "layer0 %#x layer1 %#x\n", (rd(mdss + CTL(0) + CTL_SW_RESET) & 1) ? "NOT done" : "done", waited,
	    rd(mdss + CTL(0) + CTL_START), rd(mdss + CTL(0) + CTL_FLUSH), rd(mdss + CTL(0) + CTL_TOP),
	    rd(mdss + CTL(0) + 0x00), top, layer0, layer1);
	wr(mdss + CTL(0) + 0x00, layer0);
	wr(mdss + CTL(0) + 0x04, layer1);
	wr(mdss + CTL(0) + CTL_TOP, top);
}

static bool te_from_panel;      /* the panel's TE line pulses (else the INTF's own vsync paces the frames) */

/*
 * The frames going: the tear check as Linux sets it up (dpu_encoder_phys_cmd_tearcheck_config(),
 * dpu_hw_intf_enable_te()), so a frame goes out in step with the panel's TE line (without it, CTL0
 * took one or two starts and then kept the next one pending for good); unless panel_kick=1, the
 * hardware's auto-refresh, a frame on every TE pulse (as for a splash screen); CTL0 driving INTF1
 * through ping-pong 0; everything latched, and a start.
 */
static uint64_t front_phys = FB_PHYS;   /* the framebuffer VIG0 fetches from (hakios_dsi_show) */
static bool frames_running;
static bool swap_driven;                /* the auto-refresh off: a transfer for each frame shown (hakios_dsi_show) */
static vm_offset_t fb2;                 /* the second framebuffer, mapped while the kernel's room for that is still there */

static void
frames_go(void)
{
	const uint32_t start_pos = FB_HEIGHT, thresh = 4;

	wr(mdss + INTF1 + INTF_TEAR_MDP_VSYNC_SEL, 0);
	wr(mdss + INTF1 + INTF_TEAR_SYNC_VSYNC, (1u << 19) | (te_from_panel ? 1u << 20 : 0) | VSYNC_CLOCK_HZ / (FB_VTOTAL * 60));
	wr(mdss + INTF1 + INTF_TEAR_SYNC_HEIGHT, FB_VTOTAL * 2);
	wr(mdss + INTF1 + INTF_TEAR_VSYNC_INIT, FB_HEIGHT);
	wr(mdss + INTF1 + INTF_TEAR_RD_PTR_IRQ, FB_HEIGHT + 1);
	wr(mdss + INTF1 + INTF_TEAR_START_POS, start_pos);
	wr(mdss + INTF1 + INTF_TEAR_SYNC_THRESH, (thresh << 16) | thresh);
	wr(mdss + INTF1 + INTF_TEAR_SYNC_WRCOUNT, start_pos + thresh + 1);
	/*
	 * In this order: the tear check on (auto-refresh off) and three TE pulses for its counter to
	 * fall in step with the panel; one frame started by hand; the auto-refresh only after that.
	 * With all of it at once, most runs' frames underflowed from the first one on (stripes); the
	 * clean ones were those whose tear check an earlier run had left running long before.
	 */
	wr(mdss + INTF1 + INTF_TEAR_AUTOREFRESH, 0);
	wr(mdss + INTF1 + INTF_TEAR_CHECK_EN, 1);
	sleep_us(50000);

	wr(mdss + CTL(0) + CTL_TOP, rd(mdss + CTL(0) + CTL_TOP) | CTL_TOP_INTF1 | CTL_TOP_CMD_MODE);
	wr(mdss + INTF1 + INTF_MUX, rd(mdss + INTF1 + INTF_MUX) & ~0xfu);
	wr(mdss + DPU_INTR_CLEAR, 0xffffffff);
	wr(dsi0 + DSI_FIFO_STATUS, 0xffffffff);         /* its error flags stay until written: only new ones after this */
	wr(mdss + SSPP_VIG(0) + SSPP_SRC0_ADDR, (uint32_t)front_phys);
	wr(mdss + CTL(0) + CTL_FLUSH, CTL_FLUSH_VIG0 | CTL_FLUSH_LM0 | CTL_FLUSH_CTL | CTL_FLUSH_INTF1);
	__builtin_arm_dsb(0xf);
	frame_start();
	if (!panel_kick && !swap_driven) {
		sleep_us(20000);                        /* the first frame out (one refresh and some) */
		wr(mdss + INTF1 + INTF_TEAR_AUTOREFRESH, (1u << 31) | 1);
	}
	frames_running = true;
	/*
	 * Mapped once. Not by ml_io_map(_wcomb): its addresses come out of an 8 MB submap for all of
	 * the kernel's device mappings, and 10 MB more are a kmem_alloc panic (2026-10-23); the
	 * "unmappable" kind has them from the kernel's map.
	 */
	if (fb2 == 0 && vinfo.v_rowbytes * vinfo.v_height != 0 && FB2_PHYS + (uint64_t)vinfo.v_rowbytes * vinfo.v_height <= DSI_CMD_BUF_PHYS) {
		fb2 = ml_io_map_unmappable(FB2_PHYS, (vm_size_t)vinfo.v_rowbytes * vinfo.v_height, WIMG_WCOMB);
	}
}

/*
 * Two framebuffers, so that a frame is never drawn into the memory the panel is being sent from:
 * with one, the window server's frames are copied in while the auto-refresh sends it, and the
 * panel shows the top of one frame over the bottom of the one before (tearing). The framebuffer
 * driver (MiniIOMobileFramebuffer) draws into the one not shown and then hands it over
 * (hakios_dsi_show). From the first of those on, the frames are no longer the auto-refresh's
 * but one transfer for each frame shown, as Linux's dpu driver sends them to a command-mode panel
 * (the panel keeps the last one in its own memory): the transfer before finished, VIG0 pointed
 * at the new framebuffer, flushed, and a transfer started, which the tear check holds back
 * until the panel's scan is out of its way. (Flips under the running auto-refresh reset the
 * phone, 2026-10-23.) Both framebuffers are in the stock dtb's cont_splash_region
 * (0x9c000000-0x9d700000), which the display engine is let into. A run ends with VIG0 at the
 * first again and the auto-refresh on (hakios_dsi_quiesce, registered with loader_return): that is how
 * the UEFI, the loader and the next kernel have it.
 */
static bool
hakios_dsi_second_buffer(uint64_t *phys, vm_offset_t *mapped)
{
	if (!frames_running || fb2 == 0 || FB_PHYS + (uint64_t)vinfo.v_rowbytes * vinfo.v_height > FB2_PHYS) {
		return false;
	}
	*phys = FB2_PHYS;
	*mapped = fb2;
	return true;
}

static uint64_t
hakios_dsi_first_buffer(void)
{
	return FB_PHYS;
}

/*
 * The transfer started last finished; so long at the most. Not the DSI's busy bit: that is set only
 * once the tear check lets the transfer begin, and right after the start reads idle (a "transfer"
 * of 2 to 5 us, 2026-10-23). Ping-pong 0's "done" (MDP_SSPP_TOP0_INTR bit 8, raw, cleared before
 * each start) is set when the frame is out.
 */
#define DPU_INTR_PP0_DONE       (1u << 8)
static bool transfer_pending;

static bool
transfer_done(uint32_t microseconds)
{
	for (uint32_t waited = 0; transfer_pending && !(rd(mdss + DPU_INTR_STATUS) & DPU_INTR_PP0_DONE); waited += 100) {
		if (waited >= microseconds) {
			return false;
		}
		sleep_us(100);
	}
	transfer_pending = false;
	return true;
}

static uint32_t shows, shows_late;

/*
 * The brightness, as iOS asks for it (sysctl kern.hakios.panel_brightness, 0 to 2047: the panel's
 * DCS 0x51 has 11 bits; userland/src/inject.c sets it from CoreBrightness's BrightnessSystem in the
 * window server). Sent in low-power mode through the command DMA, only between two transfers: with
 * the auto-refresh off (from the first frame shown on), under the lock that hakios_dsi_show() takes
 * around its frame start; before that only noted, and sent with the first frame. When no frames
 * come (nothing on the screen changes), a call 20 ms after the request sends it.
 */
static LCK_GRP_DECLARE(show_lck_grp, "hk-dsi-show");
static LCK_MTX_DECLARE(show_lock, &show_lck_grp);
static int brightness = 0x1ff;          /* what the panel's start-up sets (a quarter) */
static bool brightness_pending;
static thread_call_t brightness_call;

/* With show_lock held and no transfer under way. */
static void
brightness_send(void)
{
	uint8_t b[3] = { 0x51, (uint8_t)(brightness >> 8), (uint8_t)brightness };

	if (brightness_pending && swap_driven && dcs(b, 3)) {
		brightness_pending = false;
	}
}

static void
brightness_later(__unused thread_call_param_t a, __unused thread_call_param_t b)
{
	lck_mtx_lock(&show_lock);
	if (brightness_pending && transfer_done(50000)) {
		brightness_send();
	}
	lck_mtx_unlock(&show_lock);
}

static int
sysctl_panel_brightness SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = brightness, error = sysctl_handle_int(oidp, &value, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (value < 0 || value > 2047) {
		return EINVAL;
	}
	lck_mtx_lock(&show_lock);
	brightness = value;
	brightness_pending = true;
	lck_mtx_unlock(&show_lock);
	if (brightness_call == NULL) {
		brightness_call = thread_call_allocate(brightness_later, NULL);
	}
	if (brightness_call != NULL) {
		uint64_t delay;

		nanoseconds_to_absolutetime(20 * NSEC_PER_MSEC, &delay);
		thread_call_enter_delayed(brightness_call, mach_absolute_time() + delay);
	}
	return 0;
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, panel_brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    0, 0, sysctl_panel_brightness, "I", "the panel's brightness, 0 to 2047 (DCS 0x51)");

static void
hakios_dsi_show(uint64_t phys)
{
	lck_mtx_lock(&show_lock);
	if (!swap_driven) {
		swap_driven = true;
		wr(mdss + INTF1 + INTF_TEAR_AUTOREFRESH, 0);
		printf("panel: from now on a transfer for each frame shown, the auto-refresh off\n");
	}
	if (!transfer_done(50000)) {
		shows_late++;
	}
	brightness_send();
	front_phys = phys;
	__builtin_arm_dsb(0xf);                 /* (the frame in memory before the pipe is told to fetch it) */
	wr(mdss + SSPP_VIG(0) + SSPP_SRC0_ADDR, (uint32_t)phys);
	wr(mdss + CTL(0) + CTL_FLUSH, CTL_FLUSH_VIG0 | CTL_FLUSH_LM0 | CTL_FLUSH_CTL | CTL_FLUSH_INTF1);
	wr(mdss + DPU_INTR_CLEAR, DPU_INTR_PP0_DONE);
	__builtin_arm_dsb(0xf);
	frame_start();
	transfer_pending = true;
	if (shows < 20) {
		/* how long one transfer takes, start to end (the tear check's wait for the panel included) */
		static uint64_t longest, shortest = ~0ull;
		uint64_t began = mach_absolute_time(), took;

		transfer_done(100000);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &took);
		longest = took > longest ? took : longest;
		shortest = took < shortest ? took : shortest;
		if (shows == 19) {
			printf("panel: a transfer of a frame takes %llu to %llu us (the panel shows one every 16667 us)\n",
			    shortest / 1000, longest / 1000);
		}
	}
	if (++shows == 1 || shows % 1000 == 0) {
		printf("panel: %u frames shown, %u of them after the one before had taken 50 ms\n", shows, shows_late);
	}
	lck_mtx_unlock(&show_lock);
}

/* Is the framebuffer shown before the last one no longer being sent? (hakios_dsi_show waits for its transfer.) */

/* (from loader_return, maybe in a panic: no sleeping, only spinning) */
static void
hakios_dsi_quiesce(void)
{
	uint64_t until;

	if (!frames_running || (!swap_driven && front_phys == FB_PHYS)) {
		return;
	}
	for (int i = 0; i < 1000000 && (rd(dsi0 + DSI_STATUS0) & DSI_STATUS_MDP_BUSY); i++) {
	}
	front_phys = FB_PHYS;
	wr(mdss + SSPP_VIG(0) + SSPP_SRC0_ADDR, FB_PHYS);
	wr(mdss + CTL(0) + CTL_FLUSH, CTL_FLUSH_VIG0 | CTL_FLUSH_LM0 | CTL_FLUSH_CTL | CTL_FLUSH_INTF1);
	__builtin_arm_dsb(0xf);
	frame_start();
	if (swap_driven && !panel_kick) {
		/* as frames_go: the first frame out, then the auto-refresh */
		nanoseconds_to_absolutetime(20 * NSEC_PER_MSEC, &until);
		for (until += mach_absolute_time(); mach_absolute_time() < until;) {
		}
		wr(mdss + INTF1 + INTF_TEAR_AUTOREFRESH, (1u << 31) | 1);
		swap_driven = false;
	}
}

static void
panel_frames(void)
{
	thread_t thread;
	uint32_t pattern = 0;

	PE_parse_boot_argn("panel_kick", &panel_kick, sizeof(panel_kick));
	PE_parse_boot_argn("panel_pattern", &pattern, sizeof(pattern));
	pipeline_report("before");
	if (pattern) {
		test_pattern();
	}
	ctl0_reset();
	/* Does the panel's TE line pulse? (A pulse at 60 Hz is short: one look at the pin says nothing.) */
	unsigned edges = 0, last = rd(tlmm + GPIO_IO(GPIO_TE)) & 1;
	uint64_t until, now;

	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &until);
	for (until += mach_absolute_time(), now = 0; now < until; now = mach_absolute_time()) {
		unsigned level = rd(tlmm + GPIO_IO(GPIO_TE)) & 1;

		edges += level && !last;
		last = level;
	}
	te_from_panel = edges >= 3;
	printf("panel: TE (GPIO %u): %u rising edges in 100 ms: %s\n", GPIO_TE, edges,
	    te_from_panel ? "the panel's TE paces the frames" : "no TE from the panel, the INTF's own vsync paces them");
	frames_go();
	sleep_us(50000);
	pipeline_report("50 ms after the first");
	if (kernel_thread_start(refresh_main, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "panel refresh");
		thread_deallocate(thread);
	}
}

/*
 * The UEFI (and an earlier kernel of ours) leaves the hardware's auto-refresh running. As Linux takes
 * over a splash screen: auto-refresh off, the transfer in progress given two frames to end, and CTL0
 * reset before the DSI is.
 */
static bool
pipeline_idle(void)
{
	uint32_t refresh = rd(mdss + INTF1 + INTF_TEAR_AUTOREFRESH);
	unsigned waited;

	if (!(refresh & (1u << 31))) {
		return true;
	}
	wr(mdss + INTF1 + INTF_TEAR_AUTOREFRESH, refresh & ~(1u << 31));
	for (waited = 0; waited < 50 && (rd(dsi0 + DSI_STATUS0) & DSI_STATUS_MDP_BUSY); waited++) {
		sleep_us(1000);
	}
	/*
	 * A transfer that never ends (the last run's frames underflowed) is dropped at its source: CTL0
	 * reset before the DSI is, or the pipeline goes on pushing the rest of its frame into a DSI that
	 * has just come out of its reset, and every frame after that underflows.
	 */
	printf("panel: the last run's auto-refresh stopped; DSI %s after %u ms (status %#x)\n",
	    (rd(dsi0 + DSI_STATUS0) & DSI_STATUS_MDP_BUSY) ? "still busy" : "idle", waited, rd(dsi0 + DSI_STATUS0));
	ctl0_reset();
	return true;
}

static void
probe_later(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	probe("20 s later");
}

static void
hakios_dsi_start(void)
{
	uint32_t enabled = 0, on = 0;
	uint64_t deadline;

	PE_parse_boot_argn("panel_probe", &enabled, sizeof(enabled));
	PE_parse_boot_argn("panel_on", &on, sizeof(on));
	if (!enabled && !on) {
		return;
	}
	dispcc = ml_io_map(DISPCC_PHYS, DISPCC_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	tlmm = ml_io_map(TLMM_NORTH_PHYS, TLMM_NORTH_SIZE);
	mdss = ml_io_map(MDSS_PHYS, MDSS_SIZE);
	dsi0 = ml_io_map(DSI0_PHYS, DSI0_SIZE);
	if (dispcc == 0 || gcc == 0 || tlmm == 0 || mdss == 0 || dsi0 == 0) {
		printf("panel probe: cannot map the display's registers\n");
		return;
	}
	if (enabled) {
		probe("framebuffer start");
	}
	if (on) {
		/* the panel's side only where the UEFI did its part: the display block on and clocked */
		if (!(rd(dispcc + DISPCC_MDSS_GDSCR) & GDSC_PWR_ON) || !clock_on(0x2080) || !clock_on(0x200c) ||
		    !clock_on(0x2028) || !clock_on(0x2038)) {
			printf("panel: the UEFI left the display block off: not bringing the panel up\n");
		} else if (!pipeline_idle()) {
			printf("panel: the pipeline does not go idle: not bringing the panel up\n");
		} else if ((cmd_buf = ml_io_map_wcomb(DSI_CMD_BUF_PHYS, DSI_CMD_BUF_SIZE)) == 0) {
			printf("panel: cannot map the command buffer\n");
		} else if (panel_power()) {
			panel_init();
			/* boot-arg panel_frames=0: the panel up, no frames (the DSI left idle for the next run) */
			uint32_t frames = 1;

			PE_parse_boot_argn("panel_frames", &frames, sizeof(frames));
			if (frames) {
				panel_frames();
			}
			if (enabled) {
				probe("after the panel's bring-up");
			}
		}
	}
	if (!enabled) {
		return;
	}
	later_call = thread_call_allocate(probe_later, NULL);
	clock_interval_to_deadline(20, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(later_call, deadline);
}

static const struct hakios_panel dsi_panel = {
	.start = hakios_dsi_start,
	.second_buffer = hakios_dsi_second_buffer,
	.first_buffer = hakios_dsi_first_buffer,
	.show = hakios_dsi_show,
};

/*
 * Registered at boot; the framebuffer starts the panel just before itself (hakios_panel_start, GenericARMPE),
 * and a run ends with VIG0 on the first framebuffer and the auto-refresh on again (quiesce).
 */
static void
hakios_dsi_register(void)
{
	uint32_t probe_on = 0, on = 0;

	PE_parse_boot_argn("panel_probe", &probe_on, sizeof(probe_on));
	PE_parse_boot_argn("panel_on", &on, sizeof(on));
	if (!probe_on && !on) {
		return;
	}
	hakios_panel_register(&dsi_panel);
	hakios_return_register(hakios_dsi_quiesce, NULL);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_dsi_register);
