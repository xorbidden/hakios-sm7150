/*
 * The SM7150's WCN3990: what is said to its firmware on the modem before ath10k takes over (devices/davinci), started
 * by the modem's link (hakios_adsp_modem_on_link). With wifi_ath10k=1 ath10k itself talks to the firmware (the way
 * the Mi 9T's Wi-Fi works); this file then only clears what an earlier kernel left in the radio's SMMU and starts it.
 */
/*
 * A STAND-IN, TO BE THROWN AWAY when ath10k's own qmi.c runs here (as hakios.wlfw.c is for ath11k's).
 *
 * The Mi 9T's Wi-Fi is a WCN3990, whose firmware runs on the modem (hakios.adsp.c with adsp_modem=1
 * starts it, serves its files, and gives the QMI helpers the modem's QRTR). Before ath10k touches
 * the radio it has an exchange with that firmware's service (WLFW, 69): the indications it wants,
 * what the host can do, where the firmware's memory (the MSA) is -- which the hypervisor then
 * gives to the modem and the Wi-Fi -- that it is ready, what the chip is, the board's data file,
 * the calibration report, and "ready" back. This file makes that exchange by hand, in the order and
 * with the values of ath10k's qmi.c (ath10k_qmi_event_server_arrive() and the msa_ready event), to
 * learn from the phone whether its firmware takes ath10k's messages and what it says of itself.
 *
 * With wlfw=2 it goes one step further, ath10k_snoc_hif_power_up()'s: the radio's supplies on
 * (dt: icnss's vdd-cx-mx ldoa9 at its qcom,vdd-cx-mx-config 640 mV, vdd-1.8-xo ldoc1, vdd-1.3-rfa
 * ldoc2, vdd-3.3-ch0 ldoc10, at downstream icnss's voltages; the node names no clock), the copy
 * engines' configuration (snoc.c's three tables) and the mode "mission". Nothing on our side sets
 * up the copy engines' rings yet, so after 2 s the mode is "off" again (ath10k_snoc_wlan_disable()).
 * The supplies stay on. Boot-arg wlfw=0: not at all. wlfw_smmu=1: the SMMU's state for the radio, read
 * (smmu_survey()).
 *
 * The messages' structures and tables are ath10k's own (qmi_wlfw_v01.[ch]), put into the build by
 * tools/mk-wlfw10.sh and not kept anywhere: nothing of them is in this file. Without them this
 * compiles to a line that says so.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>
#include <sys/hakios.qmi.h>
#include <sys/systm.h>

#include <kern/startup.h>

extern int hakios_lx_start(void);                       /* drivers/wifi/hakios.lx.c: ath10k's object, as the stack's driver */
extern void hakios_wifi_join_file(void);                /* hakios.wifi_sm7150.c: the network of fw/network, through the station */
extern bool hakios_rpmh_ldo(const char *name, uint32_t millivolts);    /* soc/drivers/core/hakios.rpmh.c */
extern void hakios_adsp_modem_on_link(void (*hook)(void));            /* soc/drivers/audio/hakios.adsp_sm7150_modem.c */
void hakios_wlfw10_start(void);
extern kern_return_t thread_terminate(thread_t thread);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/*
 * Boot-arg wlfw_smmu=1: how the boot chain left the application processor's SMMU for the radio's
 * stream, read and nothing written; wlfw_smmu=2: and let through if nothing matches it (below); wlfw_smmu=3:
 * that entry taken out (below). The radio's copy engines reach our memory through it (dt: icnss
 * "iommus = <&apps_smmu 0x240 0x1>", "qcom,smmu-s1-bypass"; apps-smmu@15000000 "qcom,skip-init": the
 * boot chain set it up and Linux leaves it so). Whether stream 0x240 is let through as it is
 * (bypass: the radio sees our physical addresses), translated (a context, whose tables are someone
 * else's), refused, or matched by nothing at all decides how the driver's memory is to be shown to it.
 */
#define APPS_SMMU_PHYS          0x15000000ull
#define APPS_SMMU_SIZE          0x100000u
#define RADIO_STREAM            0x240u

/* the SMMU's registers, mapped once */
static vm_offset_t
apps_smmu(void)
{
	static vm_offset_t mapped;

	if (mapped == 0) {
		mapped = ml_io_map(APPS_SMMU_PHYS, APPS_SMMU_SIZE);
		if (mapped == 0) {
			printf("wlfw10: smmu: its registers could not be mapped\n");
		}
	}
	return mapped;
}

static void
smmu_survey(void)
{
	static const char *const kinds[] = { "TRANSLATED", "LET THROUGH (bypass)", "REFUSED (fault)", "kind 3" };
	vm_offset_t smmu;
	uint32_t scr0, idr0, idr1, matches, contexts, page, pages, found = 0, used = 0, free_match = ~0u, match_at = ~0u;
	uint32_t enabled = 0;

	PE_parse_boot_argn("wlfw_smmu", &enabled, sizeof(enabled));
	if (!enabled) {
		return;
	}
	smmu = apps_smmu();
	if (smmu == 0) {
		return;
	}
	scr0 = *(volatile uint32_t *)(smmu + 0x000);
	idr0 = *(volatile uint32_t *)(smmu + 0x020);
	idr1 = *(volatile uint32_t *)(smmu + 0x024);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	page = idr1 >> 31 ? 0x10000 : 0x1000;
	pages = 2u << (idr1 >> 28 & 7);
	printf("wlfw10: smmu: sCR0 %#x (%s; a stream nobody matches is %s), IDR0 %#x, IDR1 %#x: %u match registers, %u contexts, "
	    "pages of %#x bytes, %u before the contexts'\n", scr0, (scr0 & 1) ? "OFF" : "on",
	    (scr0 & 1u << 10) ? "refused" : "let through", idr0, idr1, matches, contexts, page, pages);
	if (scr0 & 1u << 3) {
		printf("wlfw10: smmu: 16-bit stream ids (extended matching): not read further\n");
		return;
	}
	for (uint32_t n = 0; n < matches && n < 256; n++) {
		uint32_t smr = *(volatile uint32_t *)(smmu + 0x800 + 4 * n), s2cr = *(volatile uint32_t *)(smmu + 0xc00 + 4 * n);
		uint32_t id = smr & 0x7fff, mask = smr >> 16 & 0x7fff, cb = s2cr & 0xff;

		if (!(smr >> 31)) {
			if (free_match == ~0u) {
				free_match = n;
			}
			continue;
		}
		used++;
		if (((id ^ RADIO_STREAM) & ~mask) != 0) {
			continue;
		}
		found++;
		match_at = n;
		printf("wlfw10: smmu: THE RADIO'S STREAM %#x is matched by register %u (stream %#x mask %#x): s2cr %#x, %s",
		    RADIO_STREAM, n, id, mask, s2cr, kinds[s2cr >> 16 & 3]);
		if ((s2cr >> 16 & 3) == 0 && cb < contexts && 2 * pages * page == APPS_SMMU_SIZE) {
			vm_offset_t ctx = smmu + (pages + cb) * page;
			uint32_t cbar = *(volatile uint32_t *)(smmu + page + 4 * cb);

			printf(" by context %u: cbar %#x, sctlr %#x, tcr %#x, ttbr0 %#llx, fsr %#x", cb, cbar,
			    *(volatile uint32_t *)(ctx + 0x00), *(volatile uint32_t *)(ctx + 0x30),
			    *(volatile uint64_t *)(ctx + 0x20), *(volatile uint32_t *)(ctx + 0x58));
		}
		printf("\n");
	}
	printf("wlfw10: smmu: %u match registers in use; the radio's stream %#x is matched by %u of them%s\n", used, RADIO_STREAM,
	    found, found ? "" : (scr0 & 1u << 10) ? ": IT IS REFUSED" : ": it is let through as unmatched");
	/*
	 * wlfw_smmu=2: let it through untranslated, the radio seeing our physical addresses. Not as an S2CR
	 * of type bypass: the hypervisor turns that into "fault" (a run of 24 October wrote 0x10000 and
	 * read 0x20000). As Linux does on these SoCs (arm-smmu-qcom.c's bypass quirk; the dt's
	 * "qcom,smmu-s1-bypass"): the last context bank with translation off (SCTLR 0; CBAR: stage 1,
	 * stage 2 bypassed), and the stream translated by it. In the match register that has the stream,
	 * or a free one: the S2CR first, then the SMR made valid (arm-smmu's order). The SMMU is not
	 * reset between our kernels, so a later one finds it so.
	 */
	/*
	 * wlfw_smmu=3: the entry of wlfw_smmu=2 taken out again (its match register made invalid), for the
	 * stream to be given a context with page tables (hakios.wifi_sm7150.c's radio_map(), wifi_smmu=2): on this
	 * phone a context with translation off does not let the radio through -- its writes to our memory
	 * end in translation faults of that context (FSR 2, FAR in our rings; runs of 24 October) -- and
	 * two match registers for one stream would be a fault of their own.
	 */
	if (enabled == 3) {
		if (match_at != ~0u) {
			printf("wlfw10: smmu: about to make match register %u invalid (the entry for the radio's stream)\n", match_at);
			*(volatile uint32_t *)(smmu + 0x800 + 4 * match_at) = 0;
			__asm__ volatile ("dsb sy");
			printf("wlfw10: smmu: match register %u reads back %#x\n", match_at, *(volatile uint32_t *)(smmu + 0x800 + 4 * match_at));
		}
		return;
	}
	if (enabled >= 2) {
		uint32_t cb = contexts - 1, smr = 1u << 31 | 0x1u << 16 | RADIO_STREAM, s2cr = cb, n = match_at;
		vm_offset_t ctx;

		if (2 * pages * page != APPS_SMMU_SIZE || contexts == 0) {
			printf("wlfw10: smmu: its layout is not the device tree's: nothing written\n");
			return;
		}
		if (n == ~0u) {
			n = free_match;
		}
		if (n == ~0u) {
			printf("wlfw10: smmu: no free match register for the radio's stream\n");
			return;
		}
		for (uint32_t m = 0; m < matches && m < 256; m++) {
			uint32_t other = *(volatile uint32_t *)(smmu + 0x800 + 4 * m), other2 = *(volatile uint32_t *)(smmu + 0xc00 + 4 * m);

			if (m != n && (other >> 31) && (other2 >> 16 & 3) == 0 && (other2 & 0xff) == cb) {
				printf("wlfw10: smmu: context %u is used already (match register %u): nothing written\n", cb, m);
				return;
			}
		}
		ctx = smmu + (pages + cb) * page;
		printf("wlfw10: smmu: about to write context %u: sctlr 0 (translation off), cbar %#x (stage 1, stage 2 bypassed); then "
		    "match register %u: s2cr %#x (translated by it), smr %#x\n", cb, 1u << 16 | 3u << 8 | 0xfu << 12, n, s2cr, smr);
		*(volatile uint32_t *)(ctx + 0x00) = 0;
		*(volatile uint32_t *)(smmu + page + 4 * cb) = 1u << 16 | 3u << 8 | 0xfu << 12;
		__asm__ volatile ("dsb sy");
		*(volatile uint32_t *)(smmu + 0xc00 + 4 * n) = s2cr;
		__asm__ volatile ("dsb sy");
		*(volatile uint32_t *)(smmu + 0x800 + 4 * n) = smr;
		__asm__ volatile ("dsb sy");
		{
			uint32_t r_sctlr = *(volatile uint32_t *)(ctx + 0x00), r_cbar = *(volatile uint32_t *)(smmu + page + 4 * cb);
			uint32_t r_smr = *(volatile uint32_t *)(smmu + 0x800 + 4 * n), r_s2cr = *(volatile uint32_t *)(smmu + 0xc00 + 4 * n);
			bool through = r_smr == smr && r_s2cr == s2cr && (r_sctlr & 1) == 0 && (r_cbar >> 16 & 3) == 1;

			printf("wlfw10: smmu: read back: context %u sctlr %#x cbar %#x; match register %u smr %#x s2cr %#x: THE RADIO'S "
			    "STREAM IS %s\n", cb, r_sctlr, r_cbar, n, r_smr, r_s2cr, through ? "LET THROUGH (by a context with translation off)" :
			    "NOT LET THROUGH (a write did not hold)");
		}
	}
}

/*
 * Boot-arg wifi_ath10k=1: in place of this file's exchange, ath10k itself (its object, tools/mk-ath10k.sh,
 * whose qmi.c makes the same exchange and goes on: the copy engines, HTC, WMI), on a thread of its own.
 * The radio's stream must be let through the SMMU first (wlfw_smmu=2).
 */
/*
 * What the SMMU has refused, read: the global fault registers (a stream nothing matches), and the
 * radio's context's own (a fault in its translation); with clear, cleared after.
 */
static void
smmu_faults(const char *when, bool clear)
{
	vm_offset_t smmu = apps_smmu();
	uint32_t idr1, contexts, page, pages;
	vm_offset_t ctx;

	if (smmu == 0) {
		return;
	}
	idr1 = *(volatile uint32_t *)(smmu + 0x024);
	contexts = idr1 & 0xff;
	page = idr1 >> 31 ? 0x10000 : 0x1000;
	pages = 2u << (idr1 >> 28 & 7);
	ctx = smmu + (pages + contexts - 1) * page;
	printf("wlfw10: smmu, %s: global fault: sGFSR %#x, sGFSYNR0 %#x, sGFSYNR1 %#x (its stream), sGFAR %#llx; the radio's "
	    "context %u: FSR %#x, FAR %#llx, FSYNR0 %#x\n", when, *(volatile uint32_t *)(smmu + 0x048),
	    *(volatile uint32_t *)(smmu + 0x050), *(volatile uint32_t *)(smmu + 0x054), *(volatile uint64_t *)(smmu + 0x040),
	    contexts - 1, *(volatile uint32_t *)(ctx + 0x58), *(volatile uint64_t *)(ctx + 0x60), *(volatile uint32_t *)(ctx + 0x68));
	if (clear) {
		/* (both write-one-to-clear: what is found now is not mistaken later for a fault of this run) */
		*(volatile uint32_t *)(smmu + 0x048) = *(volatile uint32_t *)(smmu + 0x048);
		*(volatile uint32_t *)(ctx + 0x58) = *(volatile uint32_t *)(ctx + 0x58);
		__asm__ volatile ("dsb sy");
		printf("wlfw10: smmu: cleared: sGFSR %#x, the radio's context's FSR %#x\n", *(volatile uint32_t *)(smmu + 0x048),
		    *(volatile uint32_t *)(ctx + 0x58));
	}
}

static void
ath10k_runs(__unused void *parameter, __unused wait_result_t wait)
{
	uint32_t station = 0;
	int error;

	smmu_faults("before ath10k", true);
	error = hakios_lx_start();
	printf("wlfw10: ath10k's start returns %d\n", error);
	if (error == 0 && PE_parse_boot_argn("wifi_station", &station, sizeof(station)) && station != 0) {
		/* the station is offered to iOS's Wi-Fi side: the network of the file joined through it */
		hakios_wifi_join_file();
	}
	smmu_faults("after ath10k's start", false);
	thread_terminate(current_thread());
}

/*
 * Every match register for the radio's stream made invalid: what a kernel before this one left
 * (wlfw_smmu=2's entry, or a context with page tables in that kernel's memory, if it did not end
 * in order) must not be found by hakios.wifi_sm7150.c's radio_map() beside its own -- two matches for one
 * stream -- nor be in force while the radio writes. Before ath10k starts, with wifi_smmu=2.
 */
static void
smmu_unmatch(void)
{
	vm_offset_t smmu = apps_smmu();
	uint32_t matches;

	if (smmu == 0) {
		return;
	}
	matches = *(volatile uint32_t *)(smmu + 0x020) & 0xff;
	for (uint32_t n = 0; n < matches && n < 256; n++) {
		uint32_t smr = *(volatile uint32_t *)(smmu + 0x800 + 4 * n);

		if ((smr >> 31) && (((smr & 0x7fff) ^ RADIO_STREAM) & ~(smr >> 16 & 0x7fff)) == 0) {
			*(volatile uint32_t *)(smmu + 0x800 + 4 * n) = 0;
			__asm__ volatile ("dsb sy");
			printf("wlfw10: smmu: match register %u (smr %#x, s2cr %#x) for the radio's stream, left by a kernel before this "
			    "one: made invalid (reads %#x)\n", n, smr, *(volatile uint32_t *)(smmu + 0xc00 + 4 * n),
			    *(volatile uint32_t *)(smmu + 0x800 + 4 * n));
		}
	}
}

static bool
ath10k_instead(void)
{
	uint32_t wanted = 0, smmu_wanted = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("wifi_ath10k", &wanted, sizeof(wanted)) || wanted == 0) {
		return false;
	}
	if (PE_parse_boot_argn("wifi_smmu", &smmu_wanted, sizeof(smmu_wanted)) && smmu_wanted == 2) {
		smmu_unmatch();
	}
	printf("wlfw10: wifi_ath10k: ATH10K ITSELF talks to the Wi-Fi firmware, not this file\n");
	if (kernel_thread_start(ath10k_runs, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return true;
}

#if !__has_include("hakios.wlfw_ath10k.h")

void
hakios_wlfw10_start(void)
{
	smmu_survey();
	if (ath10k_instead()) {
		return;
	}
	printf("wlfw10: THE EXCHANGE IS NOT BUILT IN: ath10k's message descriptions were not there when this kernel was built "
	    "(its own exchange is not carried over: wifi_ath10k=1 is the way)\n");
}

#else
#include "hakios.wlfw_ath10k.h"

int hakios_adsp_assign(uint64_t phys, uint64_t size, const uint32_t *src, unsigned n_src, const uint32_t *dst, unsigned n_dst);
void hakios_adsp_given(uint64_t phys, uint64_t size, const uint32_t *owners, unsigned count);

#define CLIENT_ID               0x4b4e454c      /* ath10k's ATH10K_QMI_CLIENT_ID */
#define MSA_PHYS                0x97d00000ull   /* dt: wlan_msa_region@97d00000 (qcom,wlan-msa-fixed-region) */
#define MSA_SIZE                0x100000u       /* dt: qcom,wlan-msa-memory */
#define VMID_HLOS               3
#define VMID_MSS_MSA            15
#define VMID_WLAN               0x18
#define VMID_WLAN_CE            0x19
#define TIMEOUT_MS              5000

static struct qmi_handle handle;
static volatile bool server_there, msa_ready, fw_ready;
static uint32_t server_node, server_port, wanted = 1;
static bool started;

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, 1000 * NSEC_PER_USEC);
}

static int
new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	server_node = service->node;
	server_port = service->port;
	hakios_qmi_connect(qmi->sock, service->node, service->port);
	server_there = true;
	return 0;
}

static void
del_server(__unused struct qmi_handle *qmi, __unused struct qmi_service *service)
{
	printf("wlfw10: THE WI-FI FIRMWARE'S SERVICE HAS GONE\n");
	server_there = false;
}

static void
on_msa_ready(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	msa_ready = true;
}

static void
on_fw_ready(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	fw_ready = true;
}

static const struct qmi_ops ops = { .new_server = new_server, .del_server = del_server };
static const struct qmi_msg_handler handlers[] = {
	{ .type = QMI_INDICATION, .msg_id = QMI_WLFW_MSA_READY_IND_V01, .ei = wlfw_msa_ready_ind_msg_v01_ei,
	  .decoded_size = sizeof(struct wlfw_msa_ready_ind_msg_v01), .fn = on_msa_ready },
	{ .type = QMI_INDICATION, .msg_id = QMI_WLFW_FW_READY_IND_V01, .ei = wlfw_fw_ready_ind_msg_v01_ei,
	  .decoded_size = sizeof(struct wlfw_fw_ready_ind_msg_v01), .fn = on_fw_ready },
	{ 0 },
};

/* One request and its answer: the decoding's result (not negative), or a negative errno. */
static int
ask(const char *what, int id, size_t largest, const struct qmi_elem_info *req_ei, const void *req,
    const struct qmi_elem_info *resp_ei, void *resp, const struct qmi_response_type_v01 *result)
{
	struct qmi_txn txn;
	int ret = qmi_txn_init(&handle, &txn, resp_ei, resp);

	if (ret < 0) {
		printf("wlfw10: %s: no transaction (%d)\n", what, ret);
		return ret;
	}
	ret = qmi_send_request(&handle, NULL, &txn, id, largest, req_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		printf("wlfw10: %s: NOT SENT (%d)\n", what, ret);
		return ret;
	}
	ret = hakios_qmi_txn_wait_ms(&txn, TIMEOUT_MS);
	if (ret < 0) {
		printf("wlfw10: %s: NO ANSWER (%d)\n", what, ret);
		return ret;
	}
	printf("wlfw10: %s: result %u, error %u\n", what, result->result, result->error);
	return ret;
}

static int
mode(const char *what, enum wlfw_driver_mode_enum_v01 to)
{
	struct wlfw_wlan_mode_req_msg_v01 req = { 0 };
	struct wlfw_wlan_mode_resp_msg_v01 resp = { 0 };
	int ret;

	req.mode = to;
	req.hw_debug_valid = 1;
	req.hw_debug = 0;
	ret = ask(what, QMI_WLFW_WLAN_MODE_REQ_V01, WLFW_WLAN_MODE_REQ_MSG_V01_MAX_MSG_LEN, wlfw_wlan_mode_req_msg_v01_ei, &req,
	    wlfw_wlan_mode_resp_msg_v01_ei, &resp, &resp.resp);
	return ret < 0 ? ret : resp.resp.result == QMI_RESULT_SUCCESS_V01 ? 0 : -EINVAL;
}

/* ath10k_snoc_hif_power_up() as far as the firmware's part: the supplies, the configuration, "mission" */
static void
power_up(void)
{
	static const struct { const char *name; uint32_t mv; } supply[] = {
		{ "ldoa9", 640 }, { "ldoc1", 1800 }, { "ldoc2", 1304 }, { "ldoc10", 3312 },
	};
	static struct wlfw_wlan_cfg_req_msg_v01 cfg;
	struct wlfw_wlan_cfg_resp_msg_v01 cfg_resp = { 0 };
	uint32_t i;
	int ret;

	for (i = 0; i < sizeof(supply) / sizeof(supply[0]); i++) {
		if (!hakios_rpmh_ldo(supply[i].name, supply[i].mv)) {
			printf("wlfw10: the radio's supply %s did not come on: no further\n", supply[i].name);
			return;
		}
	}
	printf("wlfw10: the radio's four supplies are on\n");

	bzero(&cfg, sizeof(cfg));
	cfg.host_version_valid = 0;
	cfg.tgt_cfg_valid = 1;
	cfg.tgt_cfg_len = sizeof(target_ce_config_wlan) / sizeof(target_ce_config_wlan[0]);
	for (i = 0; i < cfg.tgt_cfg_len && i < QMI_WLFW_MAX_NUM_CE_V01; i++) {
		cfg.tgt_cfg[i].pipe_num = target_ce_config_wlan[i].pipenum;
		cfg.tgt_cfg[i].pipe_dir = target_ce_config_wlan[i].pipedir;
		cfg.tgt_cfg[i].nentries = target_ce_config_wlan[i].nentries;
		cfg.tgt_cfg[i].nbytes_max = target_ce_config_wlan[i].nbytes_max;
		cfg.tgt_cfg[i].flags = target_ce_config_wlan[i].flags;
	}
	cfg.tgt_cfg_len = i;
	cfg.svc_cfg_valid = 1;
	cfg.svc_cfg_len = sizeof(target_service_to_ce_map_wlan) / sizeof(target_service_to_ce_map_wlan[0]);
	for (i = 0; i < cfg.svc_cfg_len && i < QMI_WLFW_MAX_NUM_SVC_V01; i++) {
		cfg.svc_cfg[i].service_id = target_service_to_ce_map_wlan[i].service_id;
		cfg.svc_cfg[i].pipe_dir = target_service_to_ce_map_wlan[i].pipedir;
		cfg.svc_cfg[i].pipe_num = target_service_to_ce_map_wlan[i].pipenum;
	}
	cfg.svc_cfg_len = i;
	cfg.shadow_reg_valid = 1;
	cfg.shadow_reg_len = sizeof(target_shadow_reg_cfg_map) / sizeof(target_shadow_reg_cfg_map[0]);
	for (i = 0; i < cfg.shadow_reg_len && i < QMI_WLFW_MAX_NUM_SHADOW_REG_V01; i++) {
		cfg.shadow_reg[i].id = target_shadow_reg_cfg_map[i].ce_id;
		cfg.shadow_reg[i].offset = target_shadow_reg_cfg_map[i].reg_offset;
	}
	cfg.shadow_reg_len = i;
	ret = ask("the copy engines' configuration", QMI_WLFW_WLAN_CFG_REQ_V01, WLFW_WLAN_CFG_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_wlan_cfg_req_msg_v01_ei, &cfg, wlfw_wlan_cfg_resp_msg_v01_ei, &cfg_resp, &cfg_resp.resp);
	if (ret < 0 || cfg_resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		printf("wlfw10: the configuration (%u pipes, %u services, %u shadow registers) NOT TAKEN\n", cfg.tgt_cfg_len,
		    cfg.svc_cfg_len, cfg.shadow_reg_len);
		return;
	}
	printf("wlfw10: the configuration taken: %u pipes, %u services, %u shadow registers\n", cfg.tgt_cfg_len,
	    cfg.svc_cfg_len, cfg.shadow_reg_len);
	if (mode("the mode: mission", QMI_WLFW_MISSION_V01) != 0) {
		printf("wlfw10: the mode \"mission\" NOT TAKEN\n");
		return;
	}
	printf("wlfw10: THE WI-FI FIRMWARE IS IN MISSION MODE: the copy engines (0x18800000) are the driver's next\n");
	sleep_ms(2000);
	printf("wlfw10: 2 s later the firmware's service is %s\n", server_there ? "still there" : "GONE (it crashed?)");
	if (server_there) {
		mode("the mode: off (ath10k_snoc_wlan_disable)", QMI_WLFW_OFF_V01);
	}
}

static void
exchange(__unused void *parameter, __unused wait_result_t wait)
{
	struct wlfw_ind_register_req_msg_v01 ind = { 0 };
	struct wlfw_ind_register_resp_msg_v01 ind_resp = { 0 };
	struct wlfw_host_cap_req_msg_v01 cap_host = { 0 };
	struct wlfw_host_cap_resp_msg_v01 cap_host_resp = { 0 };
	struct wlfw_msa_info_req_msg_v01 msa = { 0 };
	static struct wlfw_msa_info_resp_msg_v01 msa_resp;
	struct wlfw_msa_ready_req_msg_v01 ready = { 0 };
	struct wlfw_msa_ready_resp_msg_v01 ready_resp = { 0 };
	struct wlfw_cap_req_msg_v01 cap = { 0 };
	static struct wlfw_cap_resp_msg_v01 cap_resp;
	static struct wlfw_bdf_download_req_msg_v01 bdf;
	struct wlfw_bdf_download_resp_msg_v01 bdf_resp = { 0 };
	static struct wlfw_cal_report_req_msg_v01 cal;
	struct wlfw_cal_report_resp_msg_v01 cal_resp = { 0 };
	const uint8_t *board = NULL;
	uint64_t board_size = 0;
	char name[24];
	int ret;

	if (qmi_handle_init(&handle, 8192, &ops, handlers) != 0 || qmi_add_lookup(&handle, WLFW_SERVICE_ID_V01,
	    WLFW_SERVICE_VERS_V01, 0) != 0) {
		printf("wlfw10: no QMI handle\n");
		goto out;
	}
	for (unsigned ms = 0; ms < 60000 && !server_there; ms += 10) {
		sleep_ms(10);
	}
	if (!server_there) {
		printf("wlfw10: the Wi-Fi firmware's service (69) did not come in 60 s\n");
		goto out;
	}
	printf("wlfw10: THE WI-FI FIRMWARE'S SERVICE is at node %u port %u: ath10k's exchange, by hand\n", server_node, server_port);

	ind.client_id_valid = 1;
	ind.client_id = CLIENT_ID;
	ind.fw_ready_enable_valid = 1;
	ind.fw_ready_enable = 1;
	ind.msa_ready_enable_valid = 1;
	ind.msa_ready_enable = 1;
	ret = ask("indications", QMI_WLFW_IND_REGISTER_REQ_V01, WLFW_IND_REGISTER_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_ind_register_req_msg_v01_ei, &ind, wlfw_ind_register_resp_msg_v01_ei, &ind_resp, &ind_resp.resp);
	if (ret < 0 || ind_resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		goto out;
	}
	if (ind_resp.fw_status_valid) {
		printf("wlfw10: the firmware's status %#llx%s\n", (unsigned long long)ind_resp.fw_status,
		    (ind_resp.fw_status & QMI_WLFW_FW_READY_V01) ? ": READY ALREADY" : "");
	}

	cap_host.daemon_support_valid = 1;
	cap_host.daemon_support = 0;
	ret = ask("the host's capabilities", QMI_WLFW_HOST_CAP_REQ_V01, WLFW_HOST_CAP_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_host_cap_req_msg_v01_ei, &cap_host, wlfw_host_cap_resp_msg_v01_ei, &cap_host_resp, &cap_host_resp.resp);
	if (ret < 0) {
		goto out;
	}

	msa.msa_addr = MSA_PHYS;
	msa.size = MSA_SIZE;
	ret = ask("the MSA", QMI_WLFW_MSA_INFO_REQ_V01, WLFW_MSA_INFO_REQ_MSG_V01_MAX_MSG_LEN, wlfw_msa_info_req_msg_v01_ei, &msa,
	    wlfw_msa_info_resp_msg_v01_ei, &msa_resp, &msa_resp.resp);
	if (ret < 0 || msa_resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		goto out;
	}
	sleep_ms(20);   /* (ath10k's: the XPU's update not straight after the answer) */
	for (uint32_t i = 0; i < msa_resp.mem_region_info_len && i < QMI_WLFW_MAX_MEM_REG_V01; i++) {
		static const uint32_t hlos[] = { VMID_HLOS };
		static const uint32_t to[] = { VMID_MSS_MSA, VMID_WLAN, VMID_WLAN_CE };
		const struct wlfw_memory_region_info_s_v01 *m = &msa_resp.mem_region_info[i];
		bool inside = m->region_addr >= MSA_PHYS && m->region_addr + m->size <= MSA_PHYS + MSA_SIZE;

		printf("wlfw10: MSA region %u: %#llx, %#x bytes, %ssecure%s\n", i, (unsigned long long)m->region_addr, m->size,
		    m->secure_flag ? "" : "not ", inside ? "" : ": OUTSIDE THE MSA, not given");
		if (inside && hakios_adsp_assign(m->region_addr, m->size, hlos, 1, to, m->secure_flag ? 2 : 3) != 0) {
			printf("wlfw10: the hypervisor did not give MSA region %u to the modem and the Wi-Fi\n", i);
		} else if (inside) {
			hakios_adsp_given(m->region_addr, m->size, to, m->secure_flag ? 2 : 3);    /* (back to us when the modem stops) */
		}
	}
	ret = ask("the MSA is ready", QMI_WLFW_MSA_READY_REQ_V01, WLFW_MSA_READY_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_msa_ready_req_msg_v01_ei, &ready, wlfw_msa_ready_resp_msg_v01_ei, &ready_resp, &ready_resp.resp);
	if (ret < 0) {
		goto out;
	}

	ret = ask("the capabilities", QMI_WLFW_CAP_REQ_V01, WLFW_CAP_REQ_MSG_V01_MAX_MSG_LEN, wlfw_cap_req_msg_v01_ei, &cap,
	    wlfw_cap_resp_msg_v01_ei, &cap_resp, &cap_resp.resp);
	if (ret >= 0) {
		cap_resp.fw_build_id[sizeof(cap_resp.fw_build_id) - 1] = 0;
		cap_resp.fw_version_info.fw_build_timestamp[sizeof(cap_resp.fw_version_info.fw_build_timestamp) - 1] = 0;
		printf("wlfw10: THE CHIP: id %#x (%s), revision %#x; board id %#x (%s); SoC id %#x; firmware %#x of %s, \"%s\"\n",
		    cap_resp.chip_info.chip_id, cap_resp.chip_info_valid ? "given" : "not given", cap_resp.chip_info.chip_family,
		    cap_resp.board_info.board_id, cap_resp.board_info_valid ? "given" : "not given", cap_resp.soc_info.soc_id,
		    cap_resp.fw_version_info.fw_version, cap_resp.fw_version_info.fw_build_timestamp, cap_resp.fw_build_id);
	}

	/* ath10k's msa_ready event: the board's data, then the calibration report */
	for (unsigned ms = 0; ms < 2000 && !msa_ready; ms += 10) {
		sleep_ms(10);
	}
	printf("wlfw10: the firmware %s \"MSA ready\"\n", msa_ready ? "has said" : "HAS NOT SAID");
	if (cap_resp.board_info_valid && cap_resp.board_info.board_id != 0xff) {
		snprintf(name, sizeof(name), "bdwlan.b%02x", cap_resp.board_info.board_id & 0xff);
		board = hakios_adsp_fw_file(name, &board_size);
	}
	if (board == NULL) {
		strlcpy(name, "bdwlan.bin", sizeof(name));
		board = hakios_adsp_fw_file(name, &board_size);
	}
	if (board == NULL) {
		printf("wlfw10: no board data (/fw/bdwlan.*)\n");
	} else {
		uint32_t left = (uint32_t)board_size, segment = 0;
		const uint8_t *at = board;

		printf("wlfw10: the board's data: /fw/%s, %llu bytes\n", name, board_size);
		while (left) {
			bzero(&bdf, sizeof(bdf));
			bdf.valid = 1;
			bdf.file_id_valid = 1;
			bdf.file_id = 0;
			bdf.total_size_valid = 1;
			bdf.total_size = (uint32_t)board_size;
			bdf.seg_id_valid = 1;
			bdf.seg_id = segment++;
			bdf.data_valid = 1;
			bdf.end_valid = 1;
			bdf.data_len = left > QMI_WLFW_MAX_DATA_SIZE_V01 ? QMI_WLFW_MAX_DATA_SIZE_V01 : left;
			bdf.end = bdf.data_len == left;
			bcopy(at, bdf.data, bdf.data_len);
			ret = ask("a piece of the board's data", QMI_WLFW_BDF_DOWNLOAD_REQ_V01, WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN,
			    wlfw_bdf_download_req_msg_v01_ei, &bdf, wlfw_bdf_download_resp_msg_v01_ei, &bdf_resp, &bdf_resp.resp);
			/* (the last piece's CRC check fails on some boards' files, and the firmware takes the file all the same) */
			if (ret < 0 || (bdf_resp.resp.result != QMI_RESULT_SUCCESS_V01 &&
			    !(bdf.end && bdf_resp.resp.error == QMI_ERR_MALFORMED_MSG_V01))) {
				break;
			}
			left -= bdf.data_len;
			at += bdf.data_len;
		}
		hakios_adsp_fw_free(board, board_size);
		printf("wlfw10: the board's data: %s in %u pieces\n", left == 0 ? "TAKEN" : "NOT TAKEN", segment);
	}
	ask("the calibration report", QMI_WLFW_CAL_REPORT_REQ_V01, WLFW_CAL_REPORT_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_cal_report_req_msg_v01_ei, &cal, wlfw_cal_report_resp_msg_v01_ei, &cal_resp, &cal_resp.resp);
	for (unsigned ms = 0; ms < 10000 && !fw_ready; ms += 10) {
		sleep_ms(10);
	}
	printf("wlfw10: %s\n", fw_ready ? "THE WI-FI FIRMWARE SAYS IT IS READY (\"fw ready\"): the driver's part comes next" :
	    "the firmware has not said \"fw ready\" in 10 s");
	if (fw_ready && wanted >= 2) {
		power_up();
	}
out:
	thread_terminate(current_thread());
}

void
hakios_wlfw10_start(void)
{
	thread_t thread;

	PE_parse_boot_argn("wlfw", &wanted, sizeof(wanted));
	if (started || wanted == 0) {
		return;
	}
	started = true;
	smmu_survey();
	if (ath10k_instead()) {
		return;
	}
	if (kernel_thread_start(exchange, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

#endif

/* Started by the modem's copy of the audio DSP's driver once its IPC router is up: registered at boot. */
static void
wlfw10_register(void)
{
	hakios_adsp_modem_on_link(hakios_wlfw10_start);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, wlfw10_register);
