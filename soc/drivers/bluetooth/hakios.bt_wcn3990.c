/*
 * The SM7150's WCN3990 Bluetooth for the Mi 9T (devices/davinci), in place of drivers/bluetooth/hakios.bt.c (the
 * WCN6750's first look): its supplies, its UART's pins and clocks, its patch and NVM (fw/crbtfw21.tlv, crnv21.bin from
 * userland.sh), and a reset (bt=5). iOS does not use it yet.
 */
/*
 * The Bluetooth of the Mi 9T (davinci): the WCN3990's, the same chip as its Wi-Fi (hakios.wlfw10.c,
 * wifi/lx/src/lx_snoc.c), but over a UART of the SoC's own, not the modem: QUP wrapper 0's serial
 * engine 3 ("hsuart0": qcom,qup_uart@0x88c000, "qcom,msm-geni-serial-hs", enabled by the board's
 * overlay, dtbo 14), on GPIO 38 to 41 (function qup03, the south tile). The chip speaks HCI over it
 * (H4), as Linux's drivers/bluetooth/hci_qca.c and btqca.c drive it: a pulse to power it on, its
 * version, its patch and its NVM (the phone's own crbtfw21.tlv and crnv21.bin, from its bt_firmware
 * partition), a faster baud rate. Its supplies are the Wi-Fi's (ldoc1, ldoc2, ldoc10) and PM6150's
 * L10 for its I/O (dt: bt_wcn3990).
 *
 * Boot-arg bt=N does steps 1 to N; a step says what it is about to touch before it touches it.
 *
 *   1  reads   the engine's clocks as the boot chain left them (GCC's vote and branch registers),
 *              then -- only if they run: a block whose clock is off can hang the bus -- the
 *              engine itself (its firmware's revision says which protocol it was loaded with:
 *              2 is UART), and the four pins. On the phone: the engine's clock off, its source
 *              off, no vote of ours; the pins plain GPIOs (function 0)
 *   2  writes  the chip's supplies, through RPMh: PM6150's L10 (its I/O, 1.8 V) and the three it
 *              has with the Wi-Fi (ldoc1 1.8 V, ldoc2 1.304 V, ldoc10 3.312 V: in dt's ranges)
 *   3  writes  the four pins to the UART (qup03; dt's pulls: CTS 38 and RX 41 none, RTS 39 down,
 *              TX 40 up; 2 mA), the engine's clock source to 7.3728 MHz (GPLL0's even output,
 *              300 MHz, x 384/15625: Linux's table) and our votes for the engine's, the bus's and
 *              the wrapper's clocks; then reads the engine (as step 1 would). On the phone: the
 *              engine has the UART protocol (revision 0x20c), loaded by the boot chain
 *   4  talks   the UART set up (FIFO mode, polled: 8N1, Linux's packing), the chip powered on as
 *              hci_qca.c does it -- the "off" pulse 0xc0 at 2400 baud, the "on" pulse 0xfc at
 *              115200, flow control off for both, 100 ms -- and its first HCI command, the version
 *              request (01 00 fc 01 19): what comes back in half a second is said. On the phone:
 *              product 0xa, ROM 0x201, SoC 0x40020140 (our RTS must be held "ready" for it to speak)
 *   5  talks   the chip's patch (crbtfw21.tlv) and NVM (crnv21.bin) -- the phone's own, fw/ of the small
 *              image -- sent as btqca.c sends them (243-byte TLV segments, EDL 0x1e), the NVM's HCI
 *              tag told 115200 baud and NOT in-band sleep, which nothing here speaks (the phone's
 *              file has it on); then HCI reset and the controller's address. On the phone: the
 *              patch is taken (946 segments, 22 s at 115200)
 * Steps 4 on run on a thread of their own: the download takes some 20 s at 115200.
 *
 * Register layouts: Linux drivers/clk/qcom/gcc-sm7150.c, include/linux/soc/qcom/geni-se.h,
 * drivers/tty/serial/qcom_geni_serial.c, drivers/pinctrl/qcom/pinctrl-sm7150.c.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

#include <kern/startup.h>
#include <kern/thread_call.h>

extern bool hakios_rpmh_ldo(const char *name, uint32_t millivolts);    /* soc/drivers/core/hakios.rpmh.c */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define GCC_VOTE                0x5200c         /* the processors' votes for QUP wrapper 0's clocks */
#define GCC_VOTE_S3             (1u << 13)
#define GCC_VOTE_M_AHB          (1u << 6)
#define GCC_VOTE_S_AHB          (1u << 7)
#define GCC_VOTE_CORE           (1u << 8)
#define GCC_VOTE_CORE_2X        (1u << 9)
#define CBCR_OFF                (1u << 31)      /* a branch's "clock off" */

#define SE3_PHYS                0x0088c000UL    /* dt: qcom,qup_uart@0x88c000 */
#define SE3_SIZE                0x4000UL
#define GENI_STATUS             0x40
#define GENI_SER_M_CLK_CFG      0x48
#define GENI_FW_REVISION_RO     0x68            /* [15:8] the protocol: 1 SPI, 2 UART, 3 I2C */
#define SE_GENI_CLK_SEL         0x7c
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_TX_FIFO_STATUS  0x800
#define SE_GENI_RX_FIFO_STATUS  0x804

#define TLMM_SOUTH_PHYS         0x03d00000UL    /* the south tile, which has GPIO 38 to 41 */
#define GPIO_FIRST              38
#define GPIO_COUNT              4
/* a tile's pins are at 4K each by the pin's own number (pinctrl-msm.c): GPIO 38 at the tile + 0x26000 */
#define GPIO_PINS_PHYS          (TLMM_SOUTH_PHYS + 0x1000UL * GPIO_FIRST)
#define GPIO_CFG(base, n)       ((base) + 0x1000UL * ((n) - GPIO_FIRST))        /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(base, n)        (GPIO_CFG(base, n) + 4)

static uint32_t wanted;
static vm_offset_t gcc, se, pins;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb sy");
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, 1000 * NSEC_PER_USEC);
}

/* the engine's registers, said; only while its clocks run */
static void
engine_say(unsigned step)
{
	uint32_t fw, protocol;

	if (se == 0 && (se = ml_io_map(SE3_PHYS, SE3_SIZE)) == 0) {
		printf("bt: %u: the engine not mapped\n", step);
		return;
	}
	fw = rd(se + GENI_FW_REVISION_RO);
	protocol = fw >> 8 & 0xff;
	printf("bt: %u: SERIAL ENGINE 3: firmware revision %#x: protocol %u (%s), version %u; status %#x; clock cfg %#x, sel %#x; "
	    "irq status %#x; tx fifo %#x, rx fifo %#x\n", step, fw, protocol, protocol == 2 ? "UART" : protocol == 1 ? "SPI" :
	    protocol == 3 ? "I2C" : protocol == 0 ? "NONE: no firmware loaded" : "?", fw & 0xff, rd(se + GENI_STATUS),
	    rd(se + GENI_SER_M_CLK_CFG), rd(se + SE_GENI_CLK_SEL), rd(se + SE_GENI_M_IRQ_STATUS), rd(se + SE_GENI_TX_FIFO_STATUS),
	    rd(se + SE_GENI_RX_FIFO_STATUS));
}

/* ---- step 1: what the boot chain left ---------------------------------------------------------- */

static bool
survey(void)
{
	static const struct {
		const char *name;
		uint32_t cbcr;
	} branches[] = {
		{ "se3", 0x173c0 }, { "m-ahb", 0x17004 }, { "s-ahb", 0x17008 }, { "core", 0x1700c }, { "core-2x", 0x17014 },
	};
	vm_offset_t tlmm;
	uint32_t vote, off = 0;

	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	if (gcc == 0) {
		printf("bt: 1: GCC not mapped\n");
		return false;
	}
	vote = rd(gcc + GCC_VOTE);
	printf("bt: 1: GCC's votes for QUP wrapper 0 (%#x): %#x: se3 %s, m-ahb %s, s-ahb %s, core %s, core-2x %s\n", GCC_VOTE, vote,
	    vote & GCC_VOTE_S3 ? "yes" : "NO", vote & GCC_VOTE_M_AHB ? "yes" : "NO", vote & GCC_VOTE_S_AHB ? "yes" : "NO",
	    vote & GCC_VOTE_CORE ? "yes" : "NO", vote & GCC_VOTE_CORE_2X ? "yes" : "NO");
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		uint32_t cbcr = rd(gcc + branches[i].cbcr);

		printf("bt: 1: branch %s (%#x): %#x, %s\n", branches[i].name, branches[i].cbcr, cbcr, cbcr & CBCR_OFF ? "OFF" : "running");
		off |= cbcr & CBCR_OFF ? 1u << i : 0;
	}
	printf("bt: 1: the engine's clock source (rcg %#x): cmd %#x, cfg %#x\n", 0x173c4, rd(gcc + 0x173c4), rd(gcc + 0x173c8));
	if ((off & 0x7) != 0) {
		printf("bt: 1: THE ENGINE'S CLOCKS DO NOT ALL RUN (se3/m-ahb/s-ahb): its registers are not read\n");
	} else {
		engine_say(1);
	}
	if ((tlmm = pins = ml_io_map(GPIO_PINS_PHYS, 0x1000UL * GPIO_COUNT)) == 0) {
		printf("bt: 1: the pins' tile not mapped\n");
		return false;
	}
	for (unsigned n = GPIO_FIRST; n < GPIO_FIRST + GPIO_COUNT; n++) {
		uint32_t cfg = rd(GPIO_CFG(tlmm, n)), io = rd(GPIO_IO(tlmm, n));

		printf("bt: 1: GPIO %u: cfg %#x (function %u%s, pull %u, drive %u, %s), level %u\n", n, cfg, cfg >> 2 & 0xf,
		    (cfg >> 2 & 0xf) == 1 ? ": qup03" : "", cfg & 3, cfg >> 6 & 7, cfg & (1u << 9) ? "output" : "input", io & 1);
	}
	return true;
}

/* ---- step 2: the chip's supplies --------------------------------------------------------------------- */

static bool
power(void)
{
	static const struct {
		const char *ldo;
		uint32_t mv;
		const char *what;
	} supplies[] = {
		{ "ldoa10", 1800, "io" }, { "ldoc1", 1800, "xtal" }, { "ldoc2", 1304, "core" }, { "ldoc10", 3312, "pa" },
	};

	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		printf("bt: 2: about to ask RPMh for %s (vdd-%s) at %u mV\n", supplies[i].ldo, supplies[i].what, supplies[i].mv);
		if (!hakios_rpmh_ldo(supplies[i].ldo, supplies[i].mv)) {
			printf("bt: 2: %s DID NOT COME ON\n", supplies[i].ldo);
			return false;
		}
	}
	printf("bt: 2: THE CHIP'S FOUR SUPPLIES ARE ON\n");
	return true;
}

/* ---- step 3: the pins, the clocks, and a look at the engine ----------------------------------------- */

#define RCG_CMD                 0x173c4
#define RCG_CFG                 (RCG_CMD + 0x4)
#define RCG_M                   (RCG_CMD + 0x8)
#define RCG_N                   (RCG_CMD + 0xc)
#define RCG_D                   (RCG_CMD + 0x10)
#define RCG_CMD_UPDATE          (1u << 0)
#define RCG_CMD_ROOT_OFF        (1u << 31)

static bool
clocks_and_pins(void)
{
	/* pin, pull (0 none, 1 down, 3 up) -- function 1 (qup03), drive 2 mA (0) */
	static const struct {
		unsigned pin, pull;
		const char *what;
	} lines[] = { { 38, 0, "CTS" }, { 39, 1, "RTS" }, { 40, 3, "TX" }, { 41, 0, "RX" } };
	const uint32_t m = 384, n = 15625, votes = GCC_VOTE_S3 | GCC_VOTE_M_AHB | GCC_VOTE_S_AHB | GCC_VOTE_CORE | GCC_VOTE_CORE_2X;
	uint32_t cfg;

	for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
		cfg = 1u << 2 | lines[i].pull;
		printf("bt: 3: about to write GPIO %u (%s): cfg %#x (was %#x)\n", lines[i].pin, lines[i].what, cfg,
		    rd(GPIO_CFG(pins, lines[i].pin)));
		wr(GPIO_CFG(pins, lines[i].pin), cfg);
	}
	/* clk-rcg2's way: M, N as ~(n - m), D as ~n (half duty), the source and the pre-divider, dual edge; then UPDATE */
	cfg = 6u << 8 | 1u /* pre-divider 1: 2 * 1 - 1 */ | 2u << 12;
	printf("bt: 3: about to write the engine's clock source: m %u, n %u, cfg %#x (GPLL0 even, dual edge), then update\n", m, n, cfg);
	wr(gcc + RCG_M, m);
	wr(gcc + RCG_N, ~(n - m) & 0xffff);
	wr(gcc + RCG_D, ~n & 0xffff);
	wr(gcc + RCG_CFG, cfg);
	wr(gcc + RCG_CMD, rd(gcc + RCG_CMD) | RCG_CMD_UPDATE);
	for (unsigned ms = 0; rd(gcc + RCG_CMD) & RCG_CMD_UPDATE; ms++) {
		if (ms == 100) {
			printf("bt: 3: THE CLOCK SOURCE DID NOT TAKE THE UPDATE (cmd %#x)\n", rd(gcc + RCG_CMD));
			return false;
		}
		sleep_ms(1);
	}
	printf("bt: 3: about to vote for the engine's clocks: %#x |= %#x\n", rd(gcc + GCC_VOTE), votes);
	wr(gcc + GCC_VOTE, rd(gcc + GCC_VOTE) | votes);
	for (unsigned ms = 0; rd(gcc + 0x173c0) & CBCR_OFF; ms++) {
		if (ms == 100) {
			printf("bt: 3: THE ENGINE'S CLOCK DOES NOT RUN (cbcr %#x, the source's cmd %#x)\n", rd(gcc + 0x173c0), rd(gcc + RCG_CMD));
			return false;
		}
		sleep_ms(1);
	}
	printf("bt: 3: THE ENGINE'S CLOCK RUNS: cbcr %#x, the source's cmd %#x cfg %#x; the votes %#x\n", rd(gcc + 0x173c0),
	    rd(gcc + RCG_CMD), rd(gcc + RCG_CFG), rd(gcc + GCC_VOTE));
	engine_say(3);
	for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
		printf("bt: 3: GPIO %u (%s): cfg %#x, level %u\n", lines[i].pin, lines[i].what, rd(GPIO_CFG(pins, lines[i].pin)),
		    rd(GPIO_IO(pins, lines[i].pin)) & 1);
	}
	return true;
}

/* ---- step 4: the UART, and the chip's first words --------------------------------------------------- */

#define GENI_CGC_CTRL           0x28
#define GENI_OUTPUT_CTRL_REG    0x24
#define GENI_FORCE_DEFAULT_REG  0x20
#define GENI_SER_S_CLK_CFG      0x4c
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_GENI_BYTE_GRAN       0x254
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_UART_TX_TRANS_CFG    0x25c
#define SE_UART_TX_WORD_LEN     0x268
#define SE_UART_TX_STOP_BIT_LEN 0x26c
#define SE_UART_TX_TRANS_LEN    0x270
#define SE_UART_RX_TRANS_CFG    0x280
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_UART_RX_WORD_LEN     0x28c
#define SE_UART_RX_STALE_CNT    0x294
#define SE_UART_TX_PARITY_CFG   0x2a4
#define SE_UART_RX_PARITY_CFG   0x2a8
#define SE_UART_LOOPBACK_CFG    0x22c
#define SE_UART_MANUAL_RFR      0x2ac
#define UART_MANUAL_RFR_EN      (1u << 31)
#define UART_RFR_READY          (1u << 0)
#define SE_GENI_M_CMD_CTRL      0x604
#define SE_GENI_S_CMD_CTRL      0x634
#define GENI_CMD_ABORT          (1u << 1)
#define GENI_CMD_CANCEL         (1u << 2)
#define S_GENI_CMD_ACTIVE       (1u << 12)      /* in GENI_STATUS */
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_CMD0          0x630
#define SE_GENI_S_IRQ_STATUS    0x640
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFOn        0x700
#define SE_GENI_RX_FIFOn        0x780
#define SE_GENI_RX_WATERMARK    0x810
#define SE_GENI_RX_RFR_WATERMARK 0x814
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_S_IRQ_EN        0x644
#define SE_DMA_TX_IRQ_STAT      0xc40
#define SE_DMA_TX_IRQ_CLR       0xc44
#define SE_DMA_TX_FSM_RST       0xc58
#define SE_DMA_RX_IRQ_STAT      0xd40
#define SE_DMA_RX_IRQ_CLR       0xd44
#define SE_DMA_RX_LEN_IN        0xd54
#define SE_DMA_RX_FSM_RST       0xd58
#define DMA_RESET_DONE          (1u << 3)
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_DMA_GENERAL_CFG      0xe30
#define UART_CTS_MASK           (1u << 1)       /* in TX_TRANS_CFG: CTS ignored (no flow control) */
#define UART_START_TX           (1u << 27)
#define UART_START_READ         (1u << 27)
#define M_CMD_DONE              (1u << 0)
#define M_GENI_CMD_ACTIVE       (1u << 0)       /* in GENI_STATUS */
#define RX_FIFO_WC(st)          ((st) & 0x1ffffff)
#define RX_LAST                 (1u << 31)
#define RX_LAST_VALID(st)       ((st) >> 28 & 7)
#define QUP_WRAP0_PHYS          0x008c0000UL    /* dt: qupv3_0 (qcom,wrapper-core): its hardware version at +4 */
#define SE_CLK_HZ               7372800u

static uint32_t sampling = 32;

static void
uart_baud(uint32_t baud)
{
	uint32_t div = (SE_CLK_HZ + baud * sampling - 1) / (baud * sampling), cfg = div << 4 | 1;   /* SER_CLK_EN */
	bool reading = (rd(se + GENI_STATUS) & S_GENI_CMD_ACTIVE) != 0;

	/*
	 * The receiving sequencer keeps the clock its command started with: stopped first and started
	 * again after, as set_termios() in qcom_geni_serial.c does. Without it a phone just started from
	 * Android read nothing, not even the loopback (runs of 25 October: its clock cfg was 0x11).
	 */
	if (reading) {
		wr(se + SE_GENI_S_CMD_CTRL, GENI_CMD_CANCEL);
		for (unsigned ms = 0; ms < 20 && (rd(se + GENI_STATUS) & S_GENI_CMD_ACTIVE); ms++) {
			sleep_ms(1);
		}
		if (rd(se + GENI_STATUS) & S_GENI_CMD_ACTIVE) {
			wr(se + SE_GENI_S_CMD_CTRL, GENI_CMD_ABORT);
			for (unsigned ms = 0; ms < 20 && (rd(se + GENI_STATUS) & S_GENI_CMD_ACTIVE); ms++) {
				sleep_ms(1);
			}
		}
		wr(se + SE_GENI_S_IRQ_CLEAR, 0xffffffff);
	}
	wr(se + GENI_SER_M_CLK_CFG, cfg);
	wr(se + GENI_SER_S_CLK_CFG, cfg);
	if (reading) {
		wr(se + SE_GENI_S_CMD0, UART_START_READ);
	}
	printf("bt: 4: %u baud: divider %u (%u Hz / %u x %u), clock cfg %#x\n", baud, div, SE_CLK_HZ, baud, sampling, cfg);
}

static void
uart_flow(bool on)
{
	uint32_t cfg = rd(se + SE_UART_TX_TRANS_CFG);

	wr(se + SE_UART_TX_TRANS_CFG, on ? cfg & ~UART_CTS_MASK : cfg | UART_CTS_MASK);
}

/* the UART as qcom_geni_serial.c sets one up (port_setup, set_termios), in FIFO mode with nothing but polling */
static void
uart_setup(void)
{
	vm_offset_t wrap = ml_io_map(QUP_WRAP0_PHYS, 0x1000);
	uint32_t version = wrap != 0 ? rd(wrap + 4) : 0;

	sampling = version >= 0x20050000 ? 16 : 32;     /* (QUP_SE_VERSION_2_5: half the oversampling) */
	printf("bt: 4: the QUP wrapper's hardware version %#x: %u samples a bit\n", version, sampling);
	/*
	 * The engine keeps its state from one of our kernels to the next: a read the kernel before started
	 * is still running, and another on top of it does nothing (a run of 24 October: not even the
	 * loopback came back). Both sequencers' commands cancelled, aborted if that is not enough, as
	 * qcom_geni_serial.c does before it sets a port up; what is in the receive FIFO thrown away.
	 */
	{
		static const struct {
			uint32_t ctrl, active;
			const char *which;
		} seq[] = { { SE_GENI_S_CMD_CTRL, S_GENI_CMD_ACTIVE, "receive" }, { SE_GENI_M_CMD_CTRL, M_GENI_CMD_ACTIVE, "send" } };
		uint32_t thrown = 0;

		for (unsigned i = 0; i < 2; i++) {
			if (!(rd(se + GENI_STATUS) & seq[i].active)) {
				continue;
			}
			wr(se + seq[i].ctrl, GENI_CMD_CANCEL);
			for (unsigned ms = 0; ms < 20 && (rd(se + GENI_STATUS) & seq[i].active); ms++) {
				sleep_ms(1);
			}
			if (rd(se + GENI_STATUS) & seq[i].active) {
				wr(se + seq[i].ctrl, GENI_CMD_ABORT);
				for (unsigned ms = 0; ms < 20 && (rd(se + GENI_STATUS) & seq[i].active); ms++) {
					sleep_ms(1);
				}
			}
			printf("bt: 4: the %s sequencer's command (left by a kernel before) %s\n", seq[i].which,
			    rd(se + GENI_STATUS) & seq[i].active ? "IS STILL ACTIVE" : "is stopped");
		}
		for (uint32_t words; (words = RX_FIFO_WC(rd(se + SE_GENI_RX_FIFO_STATUS))) != 0 && thrown < 4096;) {
			for (uint32_t w = 0; w < words; w++, thrown++) {
				(void)rd(se + SE_GENI_RX_FIFOn);
			}
		}
		if (thrown != 0) {
			printf("bt: 4: %u words left in the receive FIFO, thrown away\n", thrown);
		}
		wr(se + SE_UART_MANUAL_RFR, 0);
	}
	/*
	 * Android drives this UART in the engine's DMA mode (msm-geni-serial-hs): after it, with DMA mode
	 * merely switched off, the loopback got nothing (a run of 25 October, the phone just started from
	 * Android). Both of the engine's DMA state machines reset, as geni_se_rx_dma_unprep's callers do.
	 * And no interrupt out of the engine: this driver polls, the status bits are set all the same, and
	 * with Linux's enables (geni_se_init()) the phone reset itself some 15 s into the patch's download
	 * (a run of 25 October, a few thousand bytes received by then).
	 */
	{
		static const struct {
			uint32_t rst, stat, clr;
			const char *which;
		} dma[] = { { SE_DMA_TX_FSM_RST, SE_DMA_TX_IRQ_STAT, SE_DMA_TX_IRQ_CLR, "send" },
			    { SE_DMA_RX_FSM_RST, SE_DMA_RX_IRQ_STAT, SE_DMA_RX_IRQ_CLR, "receive" } };

		printf("bt: 4: as found: DMA mode %#x, receive DMA %u bytes in, interrupts on m %#x s %#x\n",
		    rd(se + SE_GENI_DMA_MODE_EN), rd(se + SE_DMA_RX_LEN_IN), rd(se + SE_GENI_M_IRQ_EN), rd(se + SE_GENI_S_IRQ_EN));
		for (unsigned i = 0; i < 2; i++) {
			unsigned ms = 0;

			wr(se + dma[i].rst, 1);
			for (; ms < 20 && !(rd(se + dma[i].stat) & DMA_RESET_DONE); ms++) {
				sleep_ms(1);
			}
			printf("bt: 4: the %s DMA's state machine reset%s\n", dma[i].which, ms == 20 ? ": NO RESET DONE" : "");
			wr(se + dma[i].clr, 0xffffffff);
		}
		wr(se + SE_GENI_M_IRQ_EN, 0);
		wr(se + SE_GENI_S_IRQ_EN, 0);
	}
	wr(se + GENI_CGC_CTRL, rd(se + GENI_CGC_CTRL) | 0x7f);
	wr(se + SE_DMA_GENERAL_CFG, rd(se + SE_DMA_GENERAL_CFG) | 0xf);
	wr(se + GENI_OUTPUT_CTRL_REG, 0x7f);
	wr(se + GENI_FORCE_DEFAULT_REG, 1);
	wr(se + SE_GSI_EVENT_EN, 0);
	wr(se + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(se + SE_GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(se + SE_IRQ_EN, 0);          /* (nor from the engine as a whole: GENI M/S, DMA send/receive) */
	wr(se + SE_GENI_DMA_MODE_EN, rd(se + SE_GENI_DMA_MODE_EN) & ~1u);
	/* 8 bits a word, four words to a FIFO entry, the first in the lowest byte (geni_se_config_packing()) */
	wr(se + SE_GENI_TX_PACKING_CFG0, 0x4380e);
	wr(se + SE_GENI_TX_PACKING_CFG1, 0xc3e0e);
	wr(se + SE_GENI_RX_PACKING_CFG0, 0x4380e);
	wr(se + SE_GENI_RX_PACKING_CFG1, 0xc3e0e);
	wr(se + SE_GENI_BYTE_GRAN, 0);
	wr(se + SE_GENI_RX_WATERMARK, 2);
	wr(se + SE_GENI_RX_RFR_WATERMARK, 14);
	wr(se + SE_UART_RX_STALE_CNT, 10 * 16);         /* (bits per character x STALE_TIMEOUT) */
	wr(se + SE_UART_TX_WORD_LEN, 8);
	wr(se + SE_UART_RX_WORD_LEN, 8);
	wr(se + SE_UART_TX_STOP_BIT_LEN, 0);            /* one */
	wr(se + SE_UART_TX_PARITY_CFG, 0);
	wr(se + SE_UART_RX_PARITY_CFG, 0);
	wr(se + SE_UART_RX_TRANS_CFG, 0);
	wr(se + SE_UART_TX_TRANS_CFG, UART_CTS_MASK);
	/* the secondary sequencer reads from here on, at the speed it is to read */
	uart_baud(115200);
	wr(se + SE_GENI_S_CMD0, UART_START_READ);
}

/* bytes out, the main sequencer's command done; 0 or an errno */
static int
uart_send(const uint8_t *bytes, uint32_t length)
{
	for (unsigned ms = 0; rd(se + GENI_STATUS) & M_GENI_CMD_ACTIVE; ms++) {
		if (ms == 100) {
			return EBUSY;
		}
		sleep_ms(1);
	}
	wr(se + SE_GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(se + SE_UART_TX_TRANS_LEN, length);
	wr(se + SE_GENI_M_CMD0, UART_START_TX);
	for (uint32_t at = 0; at < length; at += 4) {
		uint32_t word = 0;

		for (uint32_t k = 0; k < 4 && at + k < length; k++) {
			word |= (uint32_t)bytes[at + k] << (8 * k);
		}
		for (unsigned spin = 0; (rd(se + SE_GENI_TX_FIFO_STATUS) & 0xfffffff) >= 15; spin++) {
			if (spin == 100000) {
				return EIO;
			}
		}
		wr(se + SE_GENI_TX_FIFOn, word);
	}
	for (unsigned ms = 0; !(rd(se + SE_GENI_M_IRQ_STATUS) & M_CMD_DONE); ms++) {
		if (ms == 1000) {
			printf("bt: 4: the send of %u bytes is not done (irq status %#x, status %#x)\n", length,
			    rd(se + SE_GENI_M_IRQ_STATUS), rd(se + GENI_STATUS));
			return ETIMEDOUT;
		}
		sleep_ms(1);
	}
	wr(se + SE_GENI_M_IRQ_CLEAR, M_CMD_DONE);
	return 0;
}

/* what has come in, for up to ms milliseconds; the number of bytes */
static uint32_t
uart_receive(uint8_t *bytes, uint32_t most, uint32_t ms)
{
	uint32_t got = 0;

	for (uint32_t waited = 0; waited <= ms && got < most;) {
		uint32_t st = rd(se + SE_GENI_RX_FIFO_STATUS), words = RX_FIFO_WC(st);

		if (words == 0) {
			sleep_ms(1);
			waited++;
			continue;
		}
		for (uint32_t w = 0; w < words; w++) {
			uint32_t word = rd(se + SE_GENI_RX_FIFOn), valid = 4;

			if (w == words - 1 && (st & RX_LAST) && RX_LAST_VALID(st) != 0) {
				valid = RX_LAST_VALID(st);
			}
			for (uint32_t k = 0; k < valid && got < most; k++) {
				bytes[got++] = (uint8_t)(word >> (8 * k));
			}
		}
		wr(se + SE_GENI_S_IRQ_CLEAR, rd(se + SE_GENI_S_IRQ_STATUS));
	}
	return got;
}

static void
say_bytes(const char *what, const uint8_t *bytes, uint32_t length)
{
	char line[3 * 64 + 1];
	uint32_t n = length < 64 ? length : 64;

	for (uint32_t i = 0; i < n; i++) {
		snprintf(line + 3 * i, 4, " %02x", bytes[i]);
	}
	line[3 * n] = 0;
	printf("bt: 4: %s (%u bytes):%s%s\n", what, length, n ? line : " nothing", length > n ? " ..." : "");
}

static bool
chip_hello(void)
{
	static const uint8_t version[] = { 0x01, 0x00, 0xfc, 0x01, 0x19 };      /* HCI command 0xfc00: EDL, patch version */
	uint8_t pulse, in[256];
	uint32_t got;
	int error;

	uart_setup();
	printf("bt: 4: the UART is set up: status %#x, rx fifo %#x\n", rd(se + GENI_STATUS), rd(se + SE_GENI_RX_FIFO_STATUS));

	/* our own sending and receiving, without the chip: the engine's loopback (TX into RX, inside it) */
	{
		static const uint8_t test[] = { 0x55, 0xaa, 0x01, 0x02, 0x03 };

		uart_baud(115200);
		wr(se + SE_UART_LOOPBACK_CFG, 1);
		error = uart_send(test, sizeof(test));
		got = uart_receive(in, sizeof(in), 50);
		wr(se + SE_UART_LOOPBACK_CFG, 0);
		printf("bt: 4: LOOPBACK: sent %u bytes (%d), received %u: %s\n", (unsigned)sizeof(test), error, got,
		    got == sizeof(test) && memcmp(in, test, sizeof(test)) == 0 ? "THE SAME: sending and receiving work" : "NOT THE SAME");
		say_bytes("loopback received", in, got);
	}
	/* our RTS (the chip's CTS) held "ready": the chip sends only while it is */
	wr(se + SE_UART_MANUAL_RFR, UART_MANUAL_RFR_EN | UART_RFR_READY);
	printf("bt: 4: our RTS held ready: GPIO 38 (CTS) %u, 39 (RTS) %u, 40 (TX) %u, 41 (RX) %u\n", rd(GPIO_IO(pins, 38)) & 1,
	    rd(GPIO_IO(pins, 39)) & 1, rd(GPIO_IO(pins, 40)) & 1, rd(GPIO_IO(pins, 41)) & 1);

	uart_baud(2400);
	uart_flow(false);
	pulse = 0xc0;
	printf("bt: 4: about to send the \"off\" pulse 0xc0 at 2400 baud\n");
	if ((error = uart_send(&pulse, 1)) != 0) {
		printf("bt: 4: NOT SENT (%d)\n", error);
		return false;
	}
	sleep_ms(10);
	uart_baud(115200);
	pulse = 0xfc;
	printf("bt: 4: about to send the \"on\" pulse 0xfc at 115200 baud\n");
	if ((error = uart_send(&pulse, 1)) != 0) {
		printf("bt: 4: NOT SENT (%d)\n", error);
		return false;
	}
	sleep_ms(100);
	got = uart_receive(in, sizeof(in), 50);
	say_bytes("after the pulses, the chip says", in, got);

	uart_flow(true);
	printf("bt: 4: about to send the version request (flow control on; CTS is %u)\n", rd(GPIO_IO(pins, 38)) & 1);
	if ((error = uart_send(version, sizeof(version))) != 0) {
		printf("bt: 4: NOT SENT (%d); CTS is %u\n", error, rd(GPIO_IO(pins, 38)) & 1);
		return false;
	}
	got = uart_receive(in, sizeof(in), 500);
	say_bytes("THE CHIP ANSWERS", in, got);
	if (got >= 7 && in[0] == 0x04 && in[1] == 0xff) {
		/* the vendor event: 00 02 (EDL, the version), then product id (4), patch (2), rom (2), soc id (4) */
		const uint8_t *v = in + 5;

		if (got >= 5 + 12) {
			printf("bt: 4: THE WCN3990 IS ALIVE: product %#x, patch %#x, rom %#x, soc %#x\n", v[0] | v[1] << 8 | v[2] << 16 |
			    (uint32_t)v[3] << 24, v[4] | v[5] << 8, v[6] | v[7] << 8, v[8] | v[9] << 8 | v[10] << 16 | (uint32_t)v[11] << 24);
		}
		return true;
	}
	return got != 0;
}

/* ---- step 5: the patch and the NVM, reset, the address ------------------------------------------------ */

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern kern_return_t thread_terminate(thread_t thread);

static uint8_t rx[1024];
static uint32_t rx_have;

/* one HCI event (H4: 04, code, length, parameters) into ev, within ms; its length, or 0 */
static uint32_t
event_wait(uint8_t *ev, uint32_t most, uint32_t ms)
{
	for (uint32_t waited = 0;; waited += 5) {
		while (rx_have > 0 && rx[0] != 0x04) {          /* (anything but an event start is dropped) */
			memmove(rx, rx + 1, --rx_have);
		}
		if (rx_have >= 3 && rx_have >= 3u + rx[2]) {
			uint32_t n = 3u + rx[2];

			memcpy(ev, rx, n < most ? n : most);
			memmove(rx, rx + n, rx_have - n);
			rx_have -= n;
			return n;
		}
		if (waited >= ms) {
			return 0;
		}
		rx_have += uart_receive(rx + rx_have, sizeof(rx) - rx_have, 5);
	}
}

/* an HCI command (H4 01), its answer the next event that is not the chip's own logging; that event's length */
static uint32_t
command(uint16_t opcode, const uint8_t *params, uint8_t length, uint8_t *ev, uint32_t most, uint32_t ms)
{
	uint8_t out[4 + 255];
	int error;

	out[0] = 0x01;
	out[1] = (uint8_t)opcode;
	out[2] = (uint8_t)(opcode >> 8);
	out[3] = length;
	memcpy(out + 4, params, length);
	if ((error = uart_send(out, 4u + length)) != 0) {
		printf("bt: 5: command %#x NOT SENT (%d)\n", opcode, error);
		return 0;
	}
	if (ms == 0) {
		return 0;
	}
	/*
	 * A command complete or status for another opcode is not the answer, nor is any other event: after
	 * the patch the chip gives a command credit (complete for opcode 0) and, once after HCI reset, an
	 * event 01 of no length (runs of 25 October: the address or the version request got it).
	 */
	for (unsigned stale = 0;; stale++) {
		uint32_t got = event_wait(ev, most, ms);
		uint16_t of = got >= 6 && ev[1] == 0x0e ? (uint16_t)(ev[4] | ev[5] << 8) :
		    got >= 7 && ev[1] == 0x0f ? (uint16_t)(ev[5] | ev[6] << 8) : got == 0 ? opcode : 0xffff;

		if (opcode == 0xfc00 || of == opcode || stale == 3) {
			return got;
		}
		say_bytes("an answer to another command, passed over", ev, got);
	}
}

/* a file to the chip in TLV segments; skip: only the last segment is answered (the patch's "download mode" 3) */
static bool
download(const char *name, const uint8_t *data, uint32_t size, bool skip)
{
	uint8_t seg[2 + 243], ev[260];
	uint32_t at = 0, segments = 0;
	uint64_t began = mach_absolute_time(), ns;

	while (at < size) {
		uint32_t n = size - at < 243 ? size - at : 243, got;
		bool last = at + n == size;

		seg[0] = 0x1e;          /* EDL_PATCH_TLV_REQ_CMD */
		seg[1] = (uint8_t)n;
		memcpy(seg + 2, data + at, n);
		got = command(0xfc00, seg, (uint8_t)(n + 2), ev, sizeof(ev), skip && !last ? 0 : 2000);
		segments++;
		/* a command complete for opcode 0 is the chip giving back a command credit (one comes late after the patch): not the answer */
		for (unsigned more = 0; more < 3 && got >= 6 && ev[1] == 0x0e && ev[4] == 0 && ev[5] == 0; more++) {
			got = event_wait(ev, sizeof(ev), 2000);
		}
		if (!(skip && !last)) {
			/* the vendor event: ff, length, 00 (EDL), 04 (download), the result */
			if (got < 6 || ev[1] != 0xff || ev[4] != 0x04 || ev[5] != 0) {
				printf("bt: 5: %s: segment %u (at %u of %u): %s\n", name, segments, at, size, got == 0 ? "NO ANSWER" :
				    "AN ANSWER THAT IS NOT \"TAKEN\"");
				say_bytes("its answer", ev, got);
				return false;
			}
		}
		at += n;
	}
	/* (after a download whose segments were not answered one by one, a command complete may follow: taken if it comes) */
	if (skip) {
		event_wait(ev, sizeof(ev), 100);
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
	printf("bt: 5: %s: %u bytes in %u segments, TAKEN, %llu ms\n", name, size, segments, ns / 1000000);
	return true;
}

static bool
firmware(void)
{
	const uint8_t *patch = NULL, *nvm_file = NULL;
	uint8_t *nvm = NULL, ev[260];
	uint64_t patch_size = 0, nvm_size = 0;
	uint32_t got, off, length;
	bool done = false;

	for (unsigned s = 0; s < 120 && (patch = hakios_image_file("fw", "crbtfw21.tlv", &patch_size, NULL)) == NULL; s++) {
		sleep_ms(1000);         /* (the small image is read when the system has mounted it) */
	}
	nvm_file = hakios_image_file("fw", "crnv21.bin", &nvm_size, NULL);
	if (patch == NULL || nvm_file == NULL || patch_size < 16 || nvm_size < 16) {
		printf("bt: 5: the chip's files are not in fw/ of the small image (crbtfw21.tlv %s, crnv21.bin %s)\n",
		    patch ? "there" : "MISSING", nvm_file ? "there" : "MISSING");
		goto out;
	}
	printf("bt: 5: the patch: %llu bytes, type %u, download mode %u, product %#x, ROM %#x, version %#x\n", patch_size, patch[0],
	    patch[14], patch[16] | patch[17] << 8, patch[18] | patch[19] << 8, patch[20] | patch[21] << 8);
	if (!download("the patch", patch, (uint32_t)patch_size, patch[14] == 3)) {
		goto out;
	}

	/* the NVM, edited as btqca.c's qca_tlv_check_data() does, but for 115200 baud and without in-band sleep */
	if ((nvm = kalloc_data((vm_size_t)nvm_size, Z_WAITOK)) == NULL) {
		goto out;
	}
	memcpy(nvm, nvm_file, (size_t)nvm_size);
	off = nvm[0] == 4 ? 4 : 0;      /* (an enclosing header of type 4: the first set of tags follows it) */
	length = nvm[off + 1] | nvm[off + 2] << 8 | nvm[off + 3] << 16;
	if (nvm[off] != 2 || off + 4 + length > nvm_size) {
		printf("bt: 5: the NVM is not of the kind expected (type %u, %u bytes)\n", nvm[off], length);
		goto out;
	}
	for (uint32_t at = off + 4; at + 12 <= off + 4 + length;) {
		uint32_t tag = nvm[at] | nvm[at + 1] << 8, len = nvm[at + 2] | nvm[at + 3] << 8;
		uint8_t *data = nvm + at + 12;

		if (tag == 17 && len >= 3) {            /* EDL_TAG_ID_HCI: the transport */
			printf("bt: 5: the NVM's transport tag: %02x %02x %02x: in-band sleep %s, baud set to 115200 (was index %u)\n",
			    data[0], data[1], data[2], data[0] & 0x80 ? "TAKEN OUT (the file has it on)" : "off", data[2]);
			data[0] &= 0x7f;                /* no in-band sleep: its wake and sleep bytes are not spoken here */
			data[2] = 0;                    /* QCA_BAUDRATE_115200 */
		} else if (tag == 2 && len == 6) {      /* EDL_TAG_ID_BD_ADDR */
			printf("bt: 5: the NVM has an address\n");
		}
		at += 12 + len;
	}
	if (!download("the NVM", nvm, (uint32_t)nvm_size, false)) {
		goto out;
	}
	sleep_ms(50);

	got = command(0x0c03, NULL, 0, ev, sizeof(ev), 1000);   /* HCI_Reset */
	say_bytes("HCI reset", ev, got);
	if (got < 7 || ev[1] != 0x0e || ev[6] != 0) {
		printf("bt: 5: HCI RESET NOT ANSWERED WITH SUCCESS\n");
		goto out;
	}
	got = command(0x1009, NULL, 0, ev, sizeof(ev), 1000);   /* HCI_Read_BD_ADDR */
	if (got >= 13 && ev[1] == 0x0e && ev[6] == 0) {
		printf("bt: 5: THE CONTROLLER IS UP: patched, reset; its address %02x:%02x:%02x:%02x:%02x:%02x\n", ev[12], ev[11], ev[10],
		    ev[9], ev[8], ev[7]);
		done = true;
	} else {
		say_bytes("the address's answer", ev, got);
	}
	got = command(0x1001, NULL, 0, ev, sizeof(ev), 1000);   /* HCI_Read_Local_Version_Information */
	if (got >= 14 && ev[1] == 0x0e && ev[6] == 0) {
		printf("bt: 5: HCI version %u (revision %#x), LMP version %u (subversion %#x), maker %#x\n", ev[7], ev[8] | ev[9] << 8,
		    ev[10], ev[13] | ev[14] << 8, ev[11] | ev[12] << 8);
	} else {
		say_bytes("the version's answer", ev, got);
	}
out:
	/* (the patch and the NVM file stay: hakios_image_file() hands out files it keeps; freeing them panicked) */
	if (nvm != NULL) {
		kfree_data(nvm, (vm_size_t)nvm_size);
	}
	return done;
}

/* steps 4 on, beside the system's start */
static void
later_steps(__unused void *parameter, __unused wait_result_t wait)
{
	static bool (*const steps[])(void) = { chip_hello, firmware };

	for (unsigned step = 4; step <= wanted && step - 4 < sizeof(steps) / sizeof(steps[0]); step++) {
		if (!steps[step - 4]()) {
			printf("bt: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("bt: step %u: done\n", step);
	}
	thread_terminate(current_thread());
}

static void
bt_start(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	printf("bt: the Mi 9T's Bluetooth, steps 1 to %u\n", wanted);
	{
		static bool (*const steps[])(void) = { survey, power, clocks_and_pins };
		thread_t thread;

		for (unsigned step = 1; step <= wanted && step <= sizeof(steps) / sizeof(steps[0]); step++) {
			if (!steps[step - 1]()) {
				printf("bt: step %u: FAILED; the steps after it are not attempted\n", step);
				return;
			}
			printf("bt: step %u: done\n", step);
		}
		if (wanted >= 4 && kernel_thread_start(later_steps, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}

/*
 * bt=N: started a second after the kernel's start, on a thread call (its steps sleep and take RPMh's lock, which the
 * early boot where initcalls run cannot), in place of drivers/bluetooth/hakios.bt.c.
 */
static void
bt_init(void)
{
	static thread_call_t call;
	uint64_t deadline;

	PE_parse_boot_argn("bt", &wanted, sizeof(wanted));
	if (wanted == 0) {
		return;
	}
	call = thread_call_allocate(bt_start, NULL);
	clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, bt_init);
