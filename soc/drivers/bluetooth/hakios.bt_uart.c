/*
 * The WCN3990's HCI as Darwin's Bluetooth UART (bt=6): the kernel controls com.apple.uart.bluetooth (BlueTool's) and
 * com.apple.uart.sk.bluetooth (bluetoothd's).
 *
 * On the iPhone X the controller is on uart1 and speaks H4 (the device tree's transport-encoding 3). Darwin 16.5 does
 * not open it as a tty: BlueTool and bluetoothd connect a PF_SYSTEM stream socket to a kernel control of that name
 * (CTLIOCGINFO, connect) and set the port up with the tty's ioctls as socket options -- TIOCGETA and TIOCSETA by their
 * low 16 bits (0x7413, 0x7414: a struct termios), and bluetoothd two of the 'T' group (0x5412, 0x5414) -- then write
 * and read H4 (2 November). Here both names lead to the WCN3990's UART, which hakios.bt_wcn3990.c has patched and
 * reset. A thread of its own moves the bytes, about once a millisecond each way (the UART is polled: its interrupts
 * reset the phone, 25 October). One client at a time, as with a port; the last to connect has it. The first HCI_TRACE
 * commands and events are written to the log by opcode: what Darwin asks of a Broadcom controller, and what ours
 * answers.
 *
 * bluetoothd does not read and write its socket: it sets 0x5414 ({1024, 63, 63}: a buffer's size and the slots of
 * each ring), asks 0x5412 for a UUID and opens a Skywalk channel to it (os_channel_create; whatever transport the
 * device tree names, H4 or H4BC). That is a kernel-pipe nexus of ours here, made on the first such question, as
 * XNU's own nx_kpipe_loopback.c makes its own: what bluetoothd puts in its transmit ring goes to the UART; what comes
 * from the UART is cut into H4 packets, one a slot of its receive ring, while a channel is open (else to the socket).
 */
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <skywalk/os_skywalk_private.h>
#include <skywalk/nexus/kpipe/nx_kernel_pipe.h>

extern int hakios_bt_uart_send(const uint8_t *bytes, uint32_t length);         /* hakios.bt_wcn3990.c */
extern uint32_t hakios_bt_uart_receive(uint8_t *bytes, uint32_t most);
void hakios_bt_uart_start(void);

#define HCI_TRACE       600
#define OPT_GETA        0x7413                  /* TIOCGETA's low bits (the option is the whole ioctl number) */
#define OPT_SETA        0x7414                  /* TIOCSETA's */
#define TX_SIZE         4096
#define OPT_PIPE        0x5414                  /* bluetoothd's: the pipe's buffer size and slots */
#define OPT_UUID        0x5412                  /* bluetoothd's: the nexus to open a channel to */
#define RX_PACKETS      64                      /* H4 packets from the controller waiting for the channel */
#define PACKET_MAX      1024

static LCK_GRP_DECLARE(bt_lck_grp, "hk-bt-uart");
static LCK_MTX_DECLARE(bt_lock, &bt_lck_grp);   /* the client and the bytes to send */
static kern_ctl_ref bt_client_ref;              /* the connected client, if any */
static u_int32_t bt_client_unit;
static struct termios bt_termios;
static uint8_t tx[TX_SIZE];
static uint32_t tx_len;

/* the Skywalk side (bt_lock): the nexus, its rings while a channel is open, the packets waiting for the receive ring */
static nexus_controller_t btnx_ctl;
static uuid_t btnx_dom_uuid, btnx_prov_uuid, btnx_uuid;
static uint32_t btnx_bufsz = PACKET_MAX, btnx_slots = 63;
static kern_channel_ring_t btnx_txring, btnx_rxring;
static kern_pbufpool_t btnx_rx_pp;
static struct {
	uint16_t len;
	uint8_t data[PACKET_MAX];
} rxq[RX_PACKETS];
static uint32_t rxq_head, rxq_count, rxq_dropped;
static uint8_t rx_packet[PACKET_MAX + 8];       /* the H4 packet being put together from the UART (the link thread's) */
static uint32_t rx_have, rx_need;

/* ---- the HCI trace ---------------------------------------------------------------------------------------- */

struct h4 {
	const char *way;
	uint8_t packet[8];      /* the type and up to seven bytes of header and parameters */
	uint32_t have, need;    /* bytes of this packet so far, and its whole length once the header says it */
};
static struct h4 h4_out = { .way = "->" }, h4_in = { .way = "<-" };
static unsigned traced;

static void
h4_packet(const struct h4 *h)
{
	const uint8_t *p = h->packet;

	if (traced >= HCI_TRACE) {
		return;
	}
	traced++;
	if (p[0] == 1) {                        /* command: opcode, length */
		printf("bt-hci: %s command %#06x (ogf %#x ocf %#05x), %u bytes\n", h->way, p[1] | p[2] << 8, p[2] >> 2,
		    (p[1] | p[2] << 8) & 0x3ff, p[3]);
	} else if (p[0] == 4 && p[1] == 0x0e) {  /* command complete: credits, opcode, status */
		printf("bt-hci: %s complete %#06x status %#x\n", h->way, p[4] | p[5] << 8, p[6]);
	} else if (p[0] == 4 && p[1] == 0x0f) {  /* command status: status, credits, opcode */
		printf("bt-hci: %s status %#06x status %#x\n", h->way, p[5] | p[6] << 8, p[3]);
	} else if (p[0] == 4) {
		printf("bt-hci: %s event %#x, %u bytes\n", h->way, p[1], p[2]);
	} else {
		printf("bt-hci: %s packet of type %u, %u bytes\n", h->way, p[0], h->need);
	}
}

/* the bytes one way, cut into H4 packets for the trace */
static void
h4_bytes(struct h4 *h, const uint8_t *bytes, uint32_t length)
{
	for (uint32_t i = 0; i < length; i++) {
		if (h->have < sizeof(h->packet)) {
			h->packet[h->have] = bytes[i];
		}
		h->have++;
		if (h->need == 0) {
			const uint8_t *p = h->packet;

			/* the whole length, once the header is in: command 1+3+n, ACL 1+4+n, SCO 1+3+n, event 1+2+n */
			if ((p[0] == 1 || p[0] == 3) && h->have == 4) {
				h->need = 4 + p[3];
			} else if (p[0] == 2 && h->have == 5) {
				h->need = 5 + (p[3] | p[4] << 8);
			} else if (p[0] == 4 && h->have == 3) {
				h->need = 3 + p[2];
			} else if (p[0] < 1 || p[0] > 4) {
				h->have = 0;            /* (not H4: lost step) */
				continue;
			}
		}
		if (h->need != 0 && h->have == h->need) {
			h4_packet(h);
			h->have = h->need = 0;
		}
	}
}

/* ---- the Skywalk channel ---------------------------------------------------------------------------------- */

/*
 * The commands bluetoothd requires that the WCN3990 has not: answered here, never sent to it, and named in the
 * controller's list of what it supports (HCI_Read_Local_Supported_Commands: octet, bit; octet 0 when it is there) for
 * bluetoothd to send them.
 * Its "HCI initialization failed" when one is missing names the command (2 November: WriteDefaultErroneousDataReporting,
 * status 628, the list without it).
 */
static const struct {
	uint16_t opcode;
	uint8_t octet, bit;
	uint8_t params[4], nparams;     /* the return parameters after the status */
} emulated[] = {
	{ 0x0c5a, 18, 2, { 0 }, 1 },    /* Read_Default_Erroneous_Data_Reporting: off */
	{ 0x0c5b, 18, 3, { 0 }, 0 },    /* Write_Default_Erroneous_Data_Reporting */
	/*
	 * LE_Read_Periodic_Advertiser_List_Size: an extended advertising command to the WCN3990, after which it refuses
	 * the legacy ones (Command Disallowed, as the core specification has it) -- and bluetoothd advertises and scans
	 * with those: LE_Set_Scan_Parameters refused, "Could not start LE scan", a restart (2 November). A list of none.
	 */
	{ 0x204a, 0, 0, { 0 }, 1 },
};

static void
rxq_push(const uint8_t *packet, uint32_t len)
{
	lck_mtx_lock(&bt_lock);
	if (len > PACKET_MAX || rxq_count == RX_PACKETS) {
		rxq_dropped++;
	} else {
		uint32_t at = (rxq_head + rxq_count) % RX_PACKETS;

		rxq[at].len = (uint16_t)len;
		memcpy(rxq[at].data, packet, len);
		rxq_count++;
	}
	lck_mtx_unlock(&bt_lock);
}

/* A command from bluetoothd: answered here (true), or for the controller */
static bool
shim_command(const uint8_t *packet, uint32_t len)
{
	uint16_t opcode;

	if (len < 4 || packet[0] != 1) {
		return false;
	}
	opcode = packet[1] | packet[2] << 8;
	for (unsigned i = 0; i < sizeof(emulated) / sizeof(emulated[0]); i++) {
		uint8_t event[16] = { 4, 0x0e, (uint8_t)(4 + emulated[i].nparams), 1, packet[1], packet[2], 0 };

		if (emulated[i].opcode != opcode) {
			continue;
		}
		memcpy(event + 7, emulated[i].params, emulated[i].nparams);
		h4_bytes(&h4_out, packet, len);
		printf("bt-hci: (command %#06x answered here, not sent to the controller)\n", opcode);
		h4_bytes(&h4_in, event, 7 + emulated[i].nparams);
		rxq_push(event, 7 + emulated[i].nparams);
		return true;
	}
	/*
	 * Broadcom's vendor commands (OGF 0x3f: Apple's for its chips, BD_VSC_*): the WCN3990, a Qualcomm, answers them
	 * with its own vendor event 0xff, which bluetoothd takes for no answer ("Invalid data size 0 (status=65535)",
	 * then a restart: BD_VSC_OLYMPIC_ANTENNA_PREF_B, 2 November). Done, as far as it is told -- and Broadcom's
	 * commands with a sub-command first say which one in their answer: given no return parameters at all, bluetoothd
	 * asserts (bcm_vsc.c:642, ByteStream_NumReadBytesAvail(*pBs) >= 1), and its parser of them wants more than one
	 * byte after the status (_BCM_ByteStreamEventParser: "Invalid data size 1"). So the answer is the status, the
	 * command's first parameter (its sub-command, if it has one) and three zeroes.
	 */
	if (packet[2] >> 2 == 0x3f) {
		uint8_t event[11] = { 4, 0x0e, 8, 1, packet[1], packet[2], 0, packet[3] > 0 && len > 4 ? packet[4] : 0, 0, 0, 0 };
		uint32_t elen = sizeof(event);
		static unsigned said;

		if (said++ < 64) {
			printf("bt-hci: (vendor command %#06x, %u bytes, answered here: done)\n", opcode, packet[3]);
		}
		rxq_push(event, elen);
		return true;
	}
	return false;
}

/* An event from the controller, before bluetoothd has it: the commands answered here added to its list */
static void
shim_event(uint8_t *packet, uint32_t len)
{
	/* 04 0e len credits opcode(2) status, then the 64 octets of the list */
	if (len >= 7 + 64 && packet[1] == 0x0e && (packet[4] | packet[5] << 8) == 0x1002 && packet[6] == 0) {
		for (unsigned i = 0; i < sizeof(emulated) / sizeof(emulated[0]); i++) {
			if (emulated[i].octet != 0) {   /* (0: listed by the controller already) */
				packet[7 + emulated[i].octet] |= 1u << emulated[i].bit;
			}
		}
	}
}

/* the controller's bytes, cut into whole H4 packets for the receive ring (the link thread's) */
static void
rx_to_channel(const uint8_t *bytes, uint32_t length)
{
	for (uint32_t i = 0; i < length; i++) {
		if (rx_have < sizeof(rx_packet)) {
			rx_packet[rx_have] = bytes[i];
		}
		rx_have++;
		if (rx_need == 0) {
			const uint8_t *p = rx_packet;

			if ((p[0] == 1 || p[0] == 3) && rx_have == 4) {
				rx_need = 4 + p[3];
			} else if (p[0] == 2 && rx_have == 5) {
				rx_need = 5 + (p[3] | p[4] << 8);
			} else if (p[0] == 4 && rx_have == 3) {
				rx_need = 3 + p[2];
			} else if (p[0] < 1 || p[0] > 4) {
				rx_have = 0;
				continue;
			}
		}
		if (rx_need == 0 || rx_have < rx_need) {
			continue;
		}
		if (rx_need <= sizeof(rx_packet)) {
			shim_event(rx_packet, rx_need);
		}
		rxq_push(rx_packet, rx_need);
		rx_have = rx_need = 0;
	}
}

static errno_t
btnx_pre_connect(__unused kern_nexus_provider_t prov, __unused proc_t p, kern_nexus_t nexus,
    __unused nexus_port_t port, __unused kern_channel_t channel, __unused void **ctx)
{
	kern_pbufpool_t tx_pp = NULL, rx_pp = NULL;
	errno_t error = kern_nexus_get_pbufpool(nexus, &tx_pp, &rx_pp);

	lck_mtx_lock(&bt_lock);
	btnx_rx_pp = rx_pp;
	lck_mtx_unlock(&bt_lock);
	return error;
}

static errno_t
btnx_connected(__unused kern_nexus_provider_t prov, __unused kern_nexus_t nexus, __unused kern_channel_t channel)
{
	printf("bt: 6: a Skywalk channel is open: %s[%d]\n", proc_best_name(current_proc()), proc_pid(current_proc()));
	traced = 0;                             /* (each channel's start traced anew) */
	return 0;
}

static void
btnx_pre_disconnect(__unused kern_nexus_provider_t prov, __unused kern_nexus_t nexus, __unused kern_channel_t channel)
{
}

static void
btnx_disconnected(__unused kern_nexus_provider_t prov, __unused kern_nexus_t nexus, __unused kern_channel_t channel)
{
	printf("bt: 6: the Skywalk channel is closed (%u packets from the controller dropped)\n", rxq_dropped);
}

static errno_t
btnx_ring_init(__unused kern_nexus_provider_t prov, __unused kern_nexus_t nexus, __unused kern_channel_t channel,
    kern_channel_ring_t ring, boolean_t is_tx, __unused void **ctx)
{
	lck_mtx_lock(&bt_lock);
	if (is_tx) {
		btnx_txring = ring;
	} else {
		btnx_rxring = ring;
		rxq_head = rxq_count = 0;
	}
	lck_mtx_unlock(&bt_lock);
	return 0;
}

static void
btnx_ring_fini(__unused kern_nexus_provider_t prov, __unused kern_nexus_t nexus, kern_channel_ring_t ring)
{
	lck_mtx_lock(&bt_lock);
	if (ring == btnx_txring) {
		btnx_txring = NULL;
	}
	if (ring == btnx_rxring) {
		btnx_rxring = NULL;
	}
	lck_mtx_unlock(&bt_lock);
}

static errno_t
btnx_slot_init(__unused kern_nexus_provider_t prov, __unused kern_nexus_t nexus, __unused kern_channel_ring_t ring,
    __unused kern_channel_slot_t slot, __unused uint32_t index, __unused struct kern_slot_prop **prop, __unused void **ctx)
{
	return 0;
}

static void
btnx_slot_fini(__unused kern_nexus_provider_t prov, __unused kern_nexus_t nexus, __unused kern_channel_ring_t ring,
    __unused kern_channel_slot_t slot, __unused uint32_t index)
{
}

/* what bluetoothd has put in its transmit ring, to the UART */
static errno_t
btnx_sync_tx(__unused kern_nexus_provider_t prov, __unused kern_nexus_t nexus, kern_channel_ring_t ring,
    __unused uint32_t flags)
{
	kern_channel_slot_t slot, last = NULL;
	uint32_t avail = kern_channel_available_slot_count(ring);
	bool answered = false;

	for (slot = kern_channel_get_next_slot(ring, NULL, NULL); slot != NULL && avail > 0;
	    slot = kern_channel_get_next_slot(ring, slot, NULL), avail--) {
		kern_packet_t ph = kern_channel_slot_get_packet(ring, slot);

		last = slot;
		if (ph == 0) {
			continue;
		}
		for (kern_buflet_t buf = kern_packet_get_next_buflet(ph, NULL); buf != NULL;
		    buf = kern_packet_get_next_buflet(ph, buf)) {
			const uint8_t *data = (const uint8_t *)kern_buflet_get_data_address(buf) + kern_buflet_get_data_offset(buf);
			uint32_t len = kern_buflet_get_data_length(buf);

			if (shim_command(data, len)) {  /* (bluetoothd puts one H4 packet in a slot) */
				answered = true;
				continue;
			}
			lck_mtx_lock(&bt_lock);
			if (tx_len + len <= sizeof(tx)) {
				memcpy(tx + tx_len, data, len);
				tx_len += len;
			}
			lck_mtx_unlock(&bt_lock);
		}
	}
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
	}
	if (answered && btnx_rxring != NULL) {
		kern_channel_notify(btnx_rxring, 0);
	}
	return 0;
}

/* the packets from the controller, into bluetoothd's receive ring */
static errno_t
btnx_sync_rx(__unused kern_nexus_provider_t prov, __unused kern_nexus_t nexus, kern_channel_ring_t ring,
    __unused uint32_t flags)
{
	kern_channel_slot_t slot, last = NULL;
	uint32_t avail;

	(void)kern_channel_reclaim(ring);
	avail = kern_channel_available_slot_count(ring);
	lck_mtx_lock(&bt_lock);
	for (slot = kern_channel_get_next_slot(ring, NULL, NULL); slot != NULL && avail > 0 && rxq_count > 0;
	    slot = kern_channel_get_next_slot(ring, slot, NULL), avail--) {
		kern_packet_t ph = kern_channel_slot_get_packet(ring, slot);
		kern_buflet_t buf;

		if (ph != 0) {                  /* (one the user has given back) */
			kern_channel_slot_detach_packet(ring, slot, ph);
			kern_pbufpool_free(btnx_rx_pp, ph);
		}
		if (btnx_rx_pp == NULL || kern_pbufpool_alloc_nosleep(btnx_rx_pp, 1, &ph) != 0) {
			break;
		}
		buf = kern_packet_get_next_buflet(ph, NULL);
		memcpy((uint8_t *)kern_buflet_get_data_address(buf), rxq[rxq_head].data, rxq[rxq_head].len);
		kern_buflet_set_data_offset(buf, 0);
		kern_buflet_set_data_length(buf, rxq[rxq_head].len);
		kern_packet_finalize(ph);
		kern_channel_slot_attach_packet(ring, slot, ph);
		rxq_head = (rxq_head + 1) % RX_PACKETS;
		rxq_count--;
		last = slot;
	}
	lck_mtx_unlock(&bt_lock);
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
	}
	return 0;
}

static errno_t
btnx_dom_init(__unused kern_nexus_domain_provider_t dom)
{
	return 0;
}

static void
btnx_dom_fini(__unused kern_nexus_domain_provider_t dom)
{
}

/* The nexus bluetoothd opens a channel to: made once; its instance's UUID, or an errno */
static errno_t
btnx_make(void)
{
	struct kern_nexus_domain_provider_init dom_init = {
		.nxdpi_version = KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION,
		.nxdpi_init = btnx_dom_init,
		.nxdpi_fini = btnx_dom_fini,
	};
	struct kern_nexus_provider_init prov_init = {
		.nxpi_version = KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION,
		.nxpi_flags = NXPIF_VIRTUAL_DEVICE,
		.nxpi_pre_connect = btnx_pre_connect,
		.nxpi_connected = btnx_connected,
		.nxpi_pre_disconnect = btnx_pre_disconnect,
		.nxpi_disconnected = btnx_disconnected,
		.nxpi_ring_init = btnx_ring_init,
		.nxpi_ring_fini = btnx_ring_fini,
		.nxpi_slot_init = btnx_slot_init,
		.nxpi_slot_fini = btnx_slot_fini,
		.nxpi_sync_tx = btnx_sync_tx,
		.nxpi_sync_rx = btnx_sync_rx,
	};
	struct kern_nexus_init nx_init = {
		.nxi_version = KERN_NEXUS_CURRENT_VERSION,
	};
	nexus_attr_t nxa = NULL;
	uuid_string_t text;
	errno_t error;

	if (!uuid_is_null(btnx_uuid)) {
		return 0;
	}
	if ((error = kern_nexus_register_domain_provider(NEXUS_TYPE_KERNEL_PIPE, (const uint8_t *)"hakios.bluetooth",
	    &dom_init, sizeof(dom_init), &btnx_dom_uuid)) != 0 ||
	    (error = kern_nexus_controller_create(&btnx_ctl)) != 0 ||
	    (error = kern_nexus_attr_create(&nxa)) != 0) {
		goto out;
	}
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_ANONYMOUS, 1);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_TX_RINGS, 1);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_RX_RINGS, 1);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_TX_SLOTS, btnx_slots);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_RX_SLOTS, btnx_slots);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_SLOT_BUF_SIZE, btnx_bufsz);
	if ((error = kern_nexus_controller_register_provider(btnx_ctl, btnx_dom_uuid, (const uint8_t *)"com.apple.uart.sk.bluetooth",
	    &prov_init, sizeof(prov_init), nxa, &btnx_prov_uuid)) != 0 ||
	    (error = kern_nexus_controller_alloc_provider_instance(btnx_ctl, btnx_prov_uuid, NULL, NULL, &btnx_uuid, &nx_init)) != 0) {
		goto out;
	}
	uuid_unparse_upper(btnx_uuid, text);
	printf("bt: 6: the Skywalk nexus for bluetoothd: %s (%u slots of %u bytes each way)\n", text, btnx_slots, btnx_bufsz);
out:
	if (nxa != NULL) {
		kern_nexus_attr_destroy(nxa);
	}
	if (error != 0) {
		printf("bt: 6: the Skywalk nexus could not be made: %d\n", error);
	}
	return error;
}

/* ---- the link: client <-> UART ---------------------------------------------------------------------------- */

static void
link_main(__unused void *parameter, __unused wait_result_t wait)
{
	static uint8_t out[TX_SIZE], in[512];

	printf("bt: 6: the link to the controller runs (com.apple.uart.bluetooth, com.apple.uart.sk.bluetooth)\n");
	for (;;) {
		uint32_t sent, got;

		lck_mtx_lock(&bt_lock);
		sent = tx_len;
		memcpy(out, tx, sent);
		tx_len = 0;
		lck_mtx_unlock(&bt_lock);
		if (sent > 0) {
			h4_bytes(&h4_out, out, sent);
			if (hakios_bt_uart_send(out, sent) != 0) {
				printf("bt: 6: %u bytes not sent to the controller\n", sent);
			}
		}
		got = hakios_bt_uart_receive(in, sizeof(in));  /* (waits a millisecond when nothing has come) */
		if (got > 0) {
			kern_ctl_ref ref;
			u_int32_t unit;

			kern_channel_ring_t ring;

			h4_bytes(&h4_in, in, got);
			/* (enqueued without our lock: the socket's is taken there, and bt_send comes with it held) */
			lck_mtx_lock(&bt_lock);
			ref = bt_client_ref;
			unit = bt_client_unit;
			ring = btnx_rxring;
			lck_mtx_unlock(&bt_lock);
			if (ring != NULL) {
				rx_to_channel(in, got);
				kern_channel_notify(ring, 0);
			} else if (ref != NULL && ctl_enqueuedata(ref, unit, in, got, 0) != 0) {
				printf("bt: 6: %u bytes from the controller not taken by the client\n", got);
			}
		}
	}
}

/* ---- the kernel controls -------------------------------------------------------------------------------- */

static errno_t
bt_connect(kern_ctl_ref ref, struct sockaddr_ctl *sac, __unused void **unitinfo)
{
	lck_mtx_lock(&bt_lock);
	bt_client_ref = ref;
	bt_client_unit = sac->sc_unit;
	tx_len = 0;
	h4_out.have = h4_out.need = 0;
	lck_mtx_unlock(&bt_lock);
	printf("bt: 6: connected: %s[%d], unit %u\n", proc_best_name(current_proc()), proc_pid(current_proc()), sac->sc_unit);
	return 0;
}

static errno_t
bt_disconnect(kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo)
{
	lck_mtx_lock(&bt_lock);
	if (bt_client_ref == ref && bt_client_unit == unit) {
		bt_client_ref = NULL;
	}
	lck_mtx_unlock(&bt_lock);
	printf("bt: 6: disconnected: unit %u\n", unit);
	return 0;
}

static errno_t
bt_send(kern_ctl_ref ref, u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	size_t length = mbuf_pkthdr_len(m);
	errno_t error = 0;

	lck_mtx_lock(&bt_lock);
	if (ref != bt_client_ref || unit != bt_client_unit) {
		error = ENOTCONN;               /* (another has the port now) */
	} else if (tx_len + length > sizeof(tx)) {
		error = ENOBUFS;
	} else if ((error = mbuf_copydata(m, 0, length, tx + tx_len)) == 0) {
		tx_len += (uint32_t)length;
	}
	lck_mtx_unlock(&bt_lock);
	mbuf_freem(m);
	return error;
}

static errno_t
bt_setopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t len)
{
	if ((opt & 0xffff) == OPT_SETA && len >= sizeof(bt_termios)) {
		memcpy(&bt_termios, data, sizeof(bt_termios));
		printf("bt: 6: the port set up: %lu baud, cflag %#lx (the UART stays at its own speed)\n",
		    (unsigned long)bt_termios.c_ospeed, (unsigned long)bt_termios.c_cflag);
	} else if ((opt & 0xffff) == OPT_PIPE && len >= 12) {
		const uint32_t *v = data;

		/* (taken for the nexus if it is not made yet) */
		printf("bt: 6: the pipe asked for: buffers of %u bytes, %u and %u slots\n", v[0], v[1], v[2]);
		if (uuid_is_null(btnx_uuid) && v[0] >= 64 && v[0] <= 16384 && v[1] >= 2 && v[1] <= 1024) {
			btnx_bufsz = v[0];
			btnx_slots = v[1];
		}
	} else {
		printf("bt: 6: option %#x set (%zu bytes)\n", opt, len);
	}
	return 0;
}

static errno_t
bt_getopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data, size_t *len)
{
	if ((opt & 0xffff) == OPT_GETA && data != NULL && *len >= sizeof(bt_termios)) {
		memcpy(data, &bt_termios, sizeof(bt_termios));
		*len = sizeof(bt_termios);
	} else if ((opt & 0xffff) == OPT_UUID && data != NULL && *len >= sizeof(uuid_t)) {
		errno_t error = btnx_make();

		if (error != 0) {
			return error;
		}
		memcpy(data, btnx_uuid, sizeof(uuid_t));
		*len = sizeof(uuid_t);
	} else {
		printf("bt: 6: option %#x asked for (%zu bytes): zeroes\n", opt, *len);
		if (data != NULL) {
			bzero(data, *len);
		}
	}
	return 0;
}

static errno_t
bt_register(const char *name)
{
	struct kern_ctl_reg reg;
	kern_ctl_ref ref;

	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, name, sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	reg.ctl_sendsize = 16 * 1024;
	reg.ctl_recvsize = 64 * 1024;
	reg.ctl_connect = bt_connect;
	reg.ctl_disconnect = bt_disconnect;
	reg.ctl_send = bt_send;
	reg.ctl_setopt = bt_setopt;
	reg.ctl_getopt = bt_getopt;
	return ctl_register(&reg, &ref);
}

/* Once, when the controller is up (hakios.bt_wcn3990.c, step 6). */
void
hakios_bt_uart_start(void)
{
	static bool started;
	thread_t thread;
	errno_t error;

	if (started) {
		return;
	}
	started = true;
	bt_termios.c_cflag = CS8 | CREAD | CLOCAL;
	bt_termios.c_ispeed = bt_termios.c_ospeed = 115200;
	if ((error = bt_register("com.apple.uart.bluetooth")) != 0 || (error = bt_register("com.apple.uart.sk.bluetooth")) != 0) {
		printf("bt: 6: the kernel controls could not be registered: %d\n", error);
		return;
	}
	if (kernel_thread_start(link_main, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "bluetooth: the link to the controller");
		thread_deallocate(thread);
	}
}
