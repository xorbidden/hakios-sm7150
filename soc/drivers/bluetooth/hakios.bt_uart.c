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
 */
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/termios.h>

extern int hakios_bt_uart_send(const uint8_t *bytes, uint32_t length);         /* hakios.bt_wcn3990.c */
extern uint32_t hakios_bt_uart_receive(uint8_t *bytes, uint32_t most);
void hakios_bt_uart_start(void);

#define HCI_TRACE       400
#define OPT_GETA        0x7413                  /* TIOCGETA's low bits (the option is the whole ioctl number) */
#define OPT_SETA        0x7414                  /* TIOCSETA's */
#define TX_SIZE         4096

static LCK_GRP_DECLARE(bt_lck_grp, "hk-bt-uart");
static LCK_MTX_DECLARE(bt_lock, &bt_lck_grp);   /* the client and the bytes to send */
static kern_ctl_ref bt_client_ref;              /* the connected client, if any */
static u_int32_t bt_client_unit;
static struct termios bt_termios;
static uint8_t tx[TX_SIZE];
static uint32_t tx_len;

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

			h4_bytes(&h4_in, in, got);
			/* (enqueued without our lock: the socket's is taken there, and bt_send comes with it held) */
			lck_mtx_lock(&bt_lock);
			ref = bt_client_ref;
			unit = bt_client_unit;
			lck_mtx_unlock(&bt_lock);
			if (ref != NULL && ctl_enqueuedata(ref, unit, in, got, 0) != 0) {
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
