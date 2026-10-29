/*
 * The Mi 9T's radio for ath10k: a WCN3990 in the SoC, its firmware on the modem, and ath10k's own
 * bus for it (snoc.c) probed as Linux probes it, with a platform device made from the phone's
 * device tree node (icnss@18800000) -- what lx_bus.c is for ath11k on redwood, but with the
 * driver's bus file built and run as it is, so that this file is the platform under it:
 *
 *   the node        no properties; snoc.c's questions of it are answered here: "memory-region"
 *                   is the firmware's fixed MSA (wlan_msa_region@97d00000, 1 MB), the rest "no"
 *   its resources   the radio's registers ("membase", 0x18800000, 8 MB) and twelve interrupts,
 *                   one a copy engine (SPI 0x19e-0x1a9). The interrupts are not taken: every
 *                   millisecond, while the driver's NAPI is enabled, a thread calls each enabled
 *                   line's handler as if it had fired (it marks its engine pending), then the poll
 *                   (which serves the pending engines and unmasks them)
 *   its supplies    RPMh's LDOs, at the voltages of the downstream icnss and the node's own
 *                   vdd-cx-mx config, through the kernel (lx_os_supply())
 *   its memory      what the hypervisor gives the modem and the radio (qcom_scm_assign_mem()),
 *                   through the kernel (lx_os_assign()); the radio sees ours through a context of
 *                   the SMMU with page tables of ours (soc/drivers/wifi/hakios.wifi_sm7150.c: radio_map(), wifi_smmu=2)
 *   QMI             the kernel's helpers as they are (bsd/dev/hakios.qmi.c), on the modem's QRTR
 *
 * and the pieces of LinuxKPI's runtime that ath10k's files call and ath11k's built ones never
 * did: NAPI, a dummy net_device, interrupts, managed resources, the transmit queues' calls into
 * mac80211 (ath10k's txq path is not used: frames go through its tx op), a few of FreeBSD's.
 *
 * The registers are touched only by the driver, in mission mode: before any other mode is asked
 * for, the polling stops (on redwood, a read of the radio's registers outside it ended the kernel).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/time.h>

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <linux/soc/qcom/smem.h>
#include <net/mac80211.h>

#include "core.h"
#include "debug.h"
#include "hif.h"
#include "qmi.h"
#include "snoc.h"

#include <lx_os.h>
#include "lx.h"

#define MEMBASE_PHYS		0x18800000ull	/* dt: icnss "membase" */
#define MEMBASE_SIZE		0x800000ull
#define MSA_PHYS		0x97d00000ull	/* dt: wlan_msa_region@97d00000 (qcom,wlan-msa-fixed-region) */
#define MSA_SIZE		0x100000ull
#define IRQ_FIRST		1000		/* the numbers the copy engines' interrupts go by here (CE i: 1000 + i) */
#define CE_LINES		12

/* ---- the device, its node and its resources ---------------------------------------------------------- */

static struct device_node node = { .name = "qcom,icnss", .full_name = "/soc/qcom,icnss@18800000" };
static struct resource membase = {
	.start = MEMBASE_PHYS, .end = MEMBASE_PHYS + MEMBASE_SIZE - 1, .name = "membase", .flags = IORESOURCE_MEM,
};
static struct platform_device pdev = { .name = "ath10k_snoc", .id = -1, .num_resources = 1, .resource = &membase };

const void *
device_get_match_data(const struct device *dev)
{
	const struct of_device_id *id = lx_platform_driver->driver.of_match_table;

	return (dev == &pdev.dev && id != NULL ? id->data : NULL);	/* (snoc.c's one: "qcom,wcn3990-wifi") */
}

struct resource *
platform_get_resource_byname(struct platform_device *p, unsigned int type, const char *name)
{
	return (p == &pdev && type == IORESOURCE_MEM && strcmp(name, "membase") == 0 ? &membase : NULL);
}

int
platform_get_irq(struct platform_device *p, unsigned int n)
{
	return (n < CE_LINES ? IRQ_FIRST + (int)n : -ENXIO);
}

int
device_property_read_u32(struct device *dev, const char *name, u32 *out)
{
	return (-EINVAL);		/* ("qcom,xo-cal-data": none in the phone's node) */
}

void *
devm_memremap(struct device *dev, resource_size_t offset, size_t size, unsigned long flags)
{
	/*
	 * The MSA: the driver keeps a mapping of it and touches it only for a core dump, which is not
	 * built. Once the hypervisor has given it to the modem a read of it would fault: never read.
	 */
	void *mapped = lx_os_map_registers(offset, size);

	return (mapped != NULL ? mapped : ERR_PTR(-ENOMEM));
}

/* the device tree: the node has nothing but the firmware's memory */
const struct property *
of_find_property(const struct device_node *np, const char *name, int *lenp)
{
	return (NULL);
}

const void *
of_get_property(const struct device_node *np, const char *name, int *lenp)
{
	return (NULL);
}

int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	return (-EINVAL);
}

struct device_node *
of_get_child_by_name(const struct device_node *np, const char *name)
{
	return (NULL);		/* (no "wifi-firmware": the firmware's memory is TrustZone's and the modem's, use_tz) */
}

int
of_reserved_mem_region_to_resource(const struct device_node *np, unsigned int index, struct resource *res)
{
	if (np != &node || index != 0) {
		return (-ENODEV);
	}
	res->start = MSA_PHYS;
	res->end = MSA_PHYS + MSA_SIZE - 1;
	res->name = "wlan_msa_region";
	res->flags = IORESOURCE_MEM;
	return (0);
}

/* a separate device for the firmware's memory behind the IOMMU: only with "wifi-firmware", which is not there */
int
of_dma_configure(struct device *dev, struct device_node *np, bool force_dma)
{
	return (-ENODEV);
}

struct platform_device *
platform_device_register_full(const struct platform_device_info *info)
{
	return (ERR_PTR(-ENODEV));
}

void
platform_device_unregister(struct platform_device *p)
{
}

struct iommu_domain *
iommu_paging_domain_alloc(struct device *dev)
{
	return (ERR_PTR(-ENODEV));
}

int
iommu_attach_device(struct iommu_domain *domain, struct device *dev)
{
	return (-ENODEV);
}

void
iommu_detach_device(struct iommu_domain *domain, struct device *dev)
{
}

void
iommu_domain_free(struct iommu_domain *domain)
{
}

int
iommu_map(struct iommu_domain *domain, unsigned long iova, phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
	return (-ENODEV);
}

size_t
iommu_unmap(struct iommu_domain *domain, unsigned long iova, size_t size)
{
	return (0);
}

/* The radio's own address, if the kernel was given one (wifi_mac); else the firmware's. */
int
linuxkpi_device_get_mac_address(struct device *dev, char *address)
{
	return (lx_os_radio_address((unsigned char *)address) ? 0 : -ENOENT);
}

/* ---- its supplies: RPMh's LDOs ---------------------------------------------------------------------- */

static const struct {
	const char *supply, *ldo;
	unsigned mv;
} supplies[] = {
	{ "vdd-0.8-cx-mx", "ldoa9", 640 },	/* (the node's qcom,vdd-cx-mx-config: 640 mV) */
	{ "vdd-1.8-xo", "ldoc1", 1800 },
	{ "vdd-1.3-rfa", "ldoc2", 1304 },
	{ "vdd-3.3-ch0", "ldoc10", 3312 },
};

int
devm_regulator_bulk_get(struct device *dev, int num, struct regulator_bulk_data *consumers)
{
	for (int i = 0; i < num; i++) {
		consumers[i].consumer = (struct regulator *)&supplies;	/* (not NULL: there is one) */
		consumers[i].ret = 0;
	}
	return (0);
}

int
regulator_bulk_enable(int num, struct regulator_bulk_data *consumers)
{
	for (int i = 0; i < num; i++) {
		unsigned s;

		for (s = 0; s < nitems(supplies) && strcmp(supplies[s].supply, consumers[i].supply) != 0; s++) {
		}
		if (s == nitems(supplies)) {
			lx_tell("lx: the radio's supply %s is not known here\n", consumers[i].supply);
			return (-ENODEV);
		}
		if (!lx_os_supply(supplies[s].ldo, supplies[s].mv)) {
			lx_tell("lx: the radio's supply %s (%s) did not come on\n", consumers[i].supply, supplies[s].ldo);
			return (-EIO);
		}
	}
	return (0);
}

int
regulator_bulk_disable(int num, struct regulator_bulk_data *consumers)
{
	return (0);		/* (they stay on: the cx-mx one is shared with the rest of the SoC) */
}

/* ---- its memory, given by the hypervisor; the modem's notices; the shared memory ------------------- */

int
qcom_scm_assign_mem(phys_addr_t mem_addr, size_t mem_sz, u64 *src, const struct qcom_scm_vmperm *newvm,
    unsigned int dest_cnt)
{
	unsigned from[64], to[8], nfrom = 0;
	u64 now = 0;

	for (unsigned v = 0; v < 64; v++) {
		if (*src & (1ull << v)) {
			from[nfrom++] = v;
		}
	}
	for (unsigned i = 0; i < dest_cnt && i < nitems(to); i++) {
		to[i] = (unsigned)newvm[i].vmid;
		now |= 1ull << newvm[i].vmid;
	}
	if (lx_os_assign(mem_addr, mem_sz, from, nfrom, to, dest_cnt) != 0) {
		return (-EINVAL);
	}
	*src = now;
	return (0);
}

static struct notifier_block *modem_notices;

void *
qcom_register_ssr_notifier(const char *name, struct notifier_block *nb)
{
	modem_notices = nb;	/* (the modem is up already when the driver starts; its stopping is not told yet) */
	return (&modem_notices);
}

int
qcom_unregister_ssr_notifier(void *notify, struct notifier_block *nb)
{
	modem_notices = NULL;
	return (0);
}

void *
qcom_smem_get(unsigned int host, unsigned int item, size_t *size)
{
	return (ERR_PTR(-ENOENT));	/* (the firmware's version in the image table: informative only) */
}

/* ---- QMI: the kernel's helpers, and the mode the firmware is asked for ------------------------------- */

static void poll_quiet(void);
static volatile bool mission;		/* the firmware has been asked for mission mode, and for no other since */

#undef qmi_send_request
int
lx_qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure)
{
	if (msg_id == QMI_WLFW_WLAN_MODE_REQ_V01) {
		u32 mode = ((const struct wlfw_wlan_mode_req_msg_v01 *)structure)->mode;

		if (mode != QMI_WLFW_MISSION_V01) {
			poll_quiet();
		}
		mission = mode == QMI_WLFW_MISSION_V01;
		lx_tell("lx: the firmware is asked for mode %u (0: mission, 4: off)\n", mode);
	}
	return (qmi_send_request(qmi, sq, txn, msg_id, largest, ei, structure));
}

/* ---- interrupts: recorded, and polled for -------------------------------------------------------------- */

static struct {
	irq_handler_t handler;
	void *argument;
	bool enabled;
} lines[CE_LINES];

int
lkpi_request_irq(struct device *dev, unsigned int irq, irq_handler_t handler, irq_handler_t thread, unsigned long flags,
    const char *name, void *argument)
{
	if (irq < IRQ_FIRST || irq >= IRQ_FIRST + CE_LINES) {
		return (-EINVAL);
	}
	lines[irq - IRQ_FIRST].handler = handler;
	lines[irq - IRQ_FIRST].argument = argument;
	lines[irq - IRQ_FIRST].enabled = !(flags & IRQF_NO_AUTOEN);
	return (0);
}

void
lkpi_free_irq(unsigned int irq, void *argument)
{
	if (irq >= IRQ_FIRST && irq < IRQ_FIRST + CE_LINES) {
		lines[irq - IRQ_FIRST].handler = NULL;
		lines[irq - IRQ_FIRST].enabled = false;
	}
}

int
lkpi_enable_irq(unsigned int irq)
{
	if (irq >= IRQ_FIRST && irq < IRQ_FIRST + CE_LINES) {
		lines[irq - IRQ_FIRST].enabled = true;
	}
	return (0);
}

void
lkpi_disable_irq(unsigned int irq)
{
	if (irq >= IRQ_FIRST && irq < IRQ_FIRST + CE_LINES) {
		lines[irq - IRQ_FIRST].enabled = false;
	}
}

/* ---- NAPI: one poll, run by one thread ------------------------------------------------------------------ */

static struct {
	struct napi_struct *napi;
	volatile bool enabled;		/* napi_enable() .. napi_disable() */
	volatile bool running;		/* the poll is being run now */
	volatile bool thread;		/* the thread runs */
	unsigned long long polls, done;
} poll;

static void
poller(void *argument)
{
	for (;;) {
		struct napi_struct *napi = poll.napi;

		if (napi != NULL && poll.enabled && mission) {
			poll.running = true;
			if (poll.enabled && mission) {
				/*
				 * As if each copy engine's interrupt had come: its handler (snoc.c's per-engine one)
				 * masks it and marks it pending, and the poll serves the pending ones and unmasks
				 * them; the poll serves no engine that is not marked.
				 */
				for (unsigned i = 0; i < CE_LINES; i++) {
					if (lines[i].enabled && lines[i].handler != NULL) {
						lines[i].handler(IRQ_FIRST + (int)i, lines[i].argument);
					}
				}
				poll.done += (unsigned long long)napi->poll(napi, 64);
				poll.polls++;
			}
			poll.running = false;
		}
		lx_pause_ms(1);
	}
}

/* no poll runs, and none starts until napi_enable() (before a mode other than mission) */
static void
poll_quiet(void)
{
	poll.enabled = false;
	while (poll.running) {
		lx_pause_ms(1);
	}
}

void
linuxkpi_netif_napi_add(struct net_device *dev, struct napi_struct *napi, int (*napi_poll)(struct napi_struct *, int))
{
	napi->dev = dev;
	napi->poll = napi_poll;
	napi->budget = 64;
	poll.napi = napi;
	if (!poll.thread && lx_os_thread("lx-napi", poller, NULL) == 0) {
		poll.thread = true;
	}
}

void
linuxkpi_netif_napi_del(struct napi_struct *napi)
{
	if (poll.napi == napi) {
		poll_quiet();
		poll.napi = NULL;
	}
}

void
linuxkpi_napi_enable(struct napi_struct *napi)
{
	poll.enabled = true;
}

void
linuxkpi_napi_disable(struct napi_struct *napi)
{
	poll_quiet();
}

bool
linuxkpi_napi_schedule(struct napi_struct *napi)
{
	return (true);		/* (the thread runs it within a millisecond anyway) */
}

bool
linuxkpi_napi_complete(struct napi_struct *napi)
{
	return (true);
}

void
linuxkpi_napi_synchronize(struct napi_struct *napi)
{
	while (poll.running) {
		lx_pause_ms(1);
	}
}

/* the dummy net_device ath10k hangs its NAPI on */
struct net_device *
linuxkpi_alloc_netdev(size_t len, const char *name, uint32_t flags, void (*setup)(struct net_device *))
{
	struct net_device *dev = kzalloc(sizeof(*dev) + len, GFP_KERNEL);

	if (dev != NULL && setup != NULL) {
		setup(dev);
	}
	return (dev);
}

void
linuxkpi_free_netdev(struct net_device *dev)
{
	kfree(dev);
}

/* ---- managed resources: kept for as long as the layer is up -------------------------------------------- */

void *
lkpi_devres_alloc(void (*release)(struct device *, void *), size_t size, gfp_t gfp)
{
	return (kzalloc(size, gfp));
}

void
lkpi_devres_add(struct device *dev, void *resource)
{
}

void
lkpi_devm_kmalloc_release(struct device *dev, void *resource)
{
}

void *
linuxkpi_dmam_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t flag)
{
	return (linux_dma_alloc_coherent(dev, size, dma_handle, flag));
}

int
linux_dma_tag_init(struct device *dev, u64 mask)
{
	return (0);		/* (what the radio reaches is the pool lx_base.c hands out: its addresses are the radio's) */
}

int
linux_dma_tag_init_coherent(struct device *dev, u64 mask)
{
	return (0);
}

/* ---- packets ----------------------------------------------------------------------------------------- */

struct sk_buff *
linuxkpi_skb_copy(const struct sk_buff *skb, gfp_t gfp)
{
	struct sk_buff *copy = dev_alloc_skb(skb->len + skb_headroom(skb));

	if (copy == NULL) {
		return (NULL);
	}
	skb_reserve(copy, skb_headroom(skb));
	skb_put_data(copy, skb->data, skb->len);
	memcpy(copy->cb, skb->cb, sizeof(copy->cb));
	return (copy);
}

const uint8_t rfc1042_header[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };

/* ---- mac80211's transmit queues: not used (frames go through the driver's tx op) --------------------- */

struct sk_buff *
linuxkpi_ieee80211_tx_dequeue(struct ieee80211_hw *hw, struct ieee80211_txq *txq)
{
	return (NULL);
}

struct ieee80211_txq *
linuxkpi_ieee80211_next_txq(struct ieee80211_hw *hw, uint8_t ac)
{
	return (NULL);
}

void
linuxkpi_ieee80211_schedule_txq(struct ieee80211_hw *hw, struct ieee80211_txq *txq, bool withoutpkts)
{
}

void
linuxkpi_ieee80211_txq_schedule_start(struct ieee80211_hw *hw, uint8_t ac)
{
}

void
linuxkpi_ieee80211_txq_get_depth(struct ieee80211_txq *txq, unsigned long *frame_cnt, unsigned long *byte_cnt)
{
	if (frame_cnt != NULL) {
		*frame_cnt = 0;
	}
	if (byte_cnt != NULL) {
		*byte_cnt = 0;
	}
}

void
linuxkpi_ieee80211_wake_queue(struct ieee80211_hw *hw, int q)
{
}

void
linuxkpi_ieee80211_stop_queue(struct ieee80211_hw *hw, int q)
{
}

uint8_t
linuxkpi_ieee80211_get_tid(struct ieee80211_hdr *hdr, bool nonqos_ok)
{
	if (!ieee80211_is_data_qos(hdr->frame_control)) {
		return (nonqos_ok ? IEEE80211_NONQOS_TID : 0);
	}
	return (*ieee80211_get_qos_ctl(hdr) & IEEE80211_QOS_CTL_TID_MASK);
}

struct ieee80211_regdomain *
lkpi_get_linuxkpi_ieee80211_regdomain(size_t n)
{
	return (kzalloc(sizeof(struct ieee80211_regdomain) + n * sizeof(struct ieee80211_reg_rule), GFP_KERNEL));
}

uint32_t
linuxkpi_cfg80211_calculate_bitrate(struct rate_info *rate)
{
	return (rate->legacy);
}

/* ---- FreeBSD's, under LinuxKPI ------------------------------------------------------------------------- */

void
DELAY(int usec)
{
	unsigned long long until = lx_os_nanoseconds() + (unsigned long long)usec * 1000ull;

	while (lx_os_nanoseconds() < until) {
	}
}

int
pause_sbt(const char *wmesg, sbintime_t sbt, sbintime_t pr, int flags)
{
	unsigned long long ns = (unsigned long long)(((__uint128_t)sbt * 1000000000ull) >> 32);

	lx_pause_ms((unsigned)((ns + 999999ull) / 1000000ull));
	return (EWOULDBLOCK);
}

void
nanouptime(struct timespec *tsp)
{
	unsigned long long ns = lx_os_nanoseconds();

	tsp->tv_sec = (time_t)(ns / 1000000000ull);
	tsp->tv_nsec = (long)(ns % 1000000000ull);
}

int
eventratecheck(struct timeval *lasttime, int *cureps, int maxeps)
{
	return (1);
}

int
device_printf(device_t dev, const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	lx_vformat(line, sizeof(line) - 1, format, &list);
	va_end(list);
	lx_line_end(line, sizeof(line));
	lx_os_print(line);
	return (0);
}

/* one line of a hex dump (ath10k_dbg_dump()): bytes in hex, a group at a time, the row's bytes as text */
int
lkpi_hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf, size_t linebuflen, bool ascii)
{
	const uint8_t *b = buf;
	size_t at = 0, n = len < (size_t)rowsize ? len : (size_t)rowsize;

	if (linebuflen == 0) {
		return (0);
	}
	linebuf[0] = 0;
	for (size_t i = 0; i < n && at + 4 < linebuflen; i++) {
		at += (size_t)snprintf(linebuf + at, linebuflen - at, "%s%02x", i > 0 && groupsize > 0 && i % (size_t)groupsize == 0 ?
		    " " : "", b[i]);
	}
	if (ascii && at + n + 3 < linebuflen) {
		linebuf[at++] = ' ';
		linebuf[at++] = ' ';
		for (size_t i = 0; i < n; i++) {
			linebuf[at++] = b[i] >= 0x20 && b[i] < 0x7f ? (char)b[i] : '.';
		}
		linebuf[at] = 0;
	}
	return ((int)at);
}

const uint32_t crc32_tab[] = {
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
};

/* ---- the entries ---------------------------------------------------------------------------------- */

static struct ath10k *volatile driver;		/* the driver, while it is up: between lx_start() and lx_stop() */

int
lx_start(void)
{
	struct ath10k *ar;
	unsigned long long began;
	bool up;
	int ret;

	if (lx_base_init() != 0 || lx_pool_init() != 0) {
		lx_tell("lx: no thread, or no memory the radio can be given\n");
		return (ENOMEM);
	}
	ath10k_debug_mask = lx_os_option("wifi_quiet") ? 0 : ATH10K_DBG_SNOC | ATH10K_DBG_QMI | ATH10K_DBG_BOOT |
	    ATH10K_DBG_HTC | ATH10K_DBG_WMI | ATH10K_DBG_MAC | (lx_os_option("wifi_htt") ? ATH10K_DBG_HTT : 0);
	pdev.dev.of_node = &node;
	pdev.dev.kobj.name = "wifi";
	/* (LinuxKPI's dma_set_mask() wants FreeBSD's busdma state here; nothing reads it: it must only be there) */
	pdev.dev.dma_priv = &pdev;

	/* snoc.c: ath10k_snoc_probe(), as the platform calls it */
	lx_tell("lx: ath10k's bus (snoc) is probed for the WCN3990 at %#llx\n", MEMBASE_PHYS);
	if ((ret = lx_platform_driver->probe(&pdev)) != 0) {
		lx_tell("lx: the probe fails: %d\n", ret);
		return (-ret);
	}
	ar = platform_get_drvdata(&pdev);

	/*
	 * The rest happens on the driver's threads: qmi.c's exchange with the firmware, then, when it says
	 * "ready", core.c's start (power, mission mode, the copy engines, HTC, WMI) and mac80211's
	 * registration. It went well when core.c says it has registered.
	 */
	began = lx_os_nanoseconds();
	while (!test_bit(ATH10K_FLAG_CORE_REGISTERED, &ar->dev_flags) && lx_os_nanoseconds() - began < 90000000000ull) {
		lx_pause_ms(10);
	}
	up = test_bit(ATH10K_FLAG_CORE_REGISTERED, &ar->dev_flags);
	lx_tell("lx: %llu ms after its probe ath10k %s; its flags %#lx, state %d; %llu polls of the copy engines, %llu "
	    "entries done\n", (lx_os_nanoseconds() - began) / 1000000ull, up ? "IS UP: the firmware has been through WMI's "
	    "\"ready\", and the radio is registered" : "HAS NOT GOT UP", ar->dev_flags, ar->state, poll.polls, poll.done);
	if (!up) {
		return (EIO);
	}
	lx_tell("lx: THE RADIO: firmware %s, hardware %s; address %02x:%02x:%02x:%02x:%02x:%02x\n", ar->hw->wiphy->fw_version,
	    ar->hw_params.name, ar->mac_addr[0], ar->mac_addr[1], ar->mac_addr[2], ar->mac_addr[3], ar->mac_addr[4], ar->mac_addr[5]);
	driver = ar;
	if (lx_os_option("wifi_station")) {
		return (lx_station_offer());
	} else if (lx_os_option("wifi_join")) {
		return (lx_station_join());
	} else if (lx_os_option("wifi_scan")) {
		return (lx_station_scan());
	}
	return (0);
}

void
lx_stop(void)
{
	if (driver == NULL) {
		return;
	}
	driver = NULL;
	lx_station_withdraw();
	lx_station_leave();
	lx_platform_driver->remove(&pdev);
	lx_tell("lx: ath10k is down again\n");
}

int
lx_transmit(const unsigned char *frame, unsigned long length)
{
	return (driver != NULL ? lx_station_send(frame, length) : ENETDOWN);
}
