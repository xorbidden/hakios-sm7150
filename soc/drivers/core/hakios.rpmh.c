/*
 * RPMh for the drivers of the SM7150 port (devices/davinci): a resource's address by its name in the command
 * database the boot loader left, a PM6150/PM6150L LDO switched on at a voltage, and a bus clock manager voted
 * to the most there is. The panel, the touch screen, the cameras, Bluetooth and Wi-Fi need supplies and votes
 * that nothing else in the kernel gives them.
 *
 * Requests go through command slot set 0 of the application processor's RSC ("DRV 2"), one at a time, under
 * hakios_rsc_lock (drivers/core/hakios.platform.c), which the common drivers that write RPMh take as well.
 * Where things are: the command database through the always-on processor's dictionary (qcom,cmd-db at
 * 0xc3f000c), the RSC at soc_rsc (default 0x18220000, the same as the SM7325's). Register layout: Linux
 * drivers/soc/qcom/rpmh-rsc.c, cmd-db.c, regulator/qcom-rpmh-regulator.c, interconnect/qcom/bcm-voter.c.
 *
 * Status: seen on the Mi 9T (in the old port's hk_dsi.c): the panel's ldoa13/ldoa18, the touch screen's,
 * the cameras' and the radio's supplies, and the votes on MM0, MC0, SH0, SN9, SN0.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern lck_mtx_t hakios_rsc_lock;            /* RPMh's sequencer, shared with the common drivers */

/* what the SM7150's drivers call (each declares the ones it uses) */
uint32_t hakios_rpmh_address(const char *name);
bool hakios_rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count);
bool hakios_rpmh_ldo(const char *name, uint32_t millivolts);
bool hakios_rpmh_bcm_max(const char *name);

#define AOP_DICT_PHYS           0x0c3f0000UL    /* "qcom,cmd-db" (0xc3f000c): where the command database is */
#define AOP_DICT_OFFSET         0xc
#define RSC_DEFAULT_PHYS        0x18220000UL    /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))   /* command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7
#define BCM_COMMIT              (1u << 30)
#define BCM_VALID               (1u << 29)
#define BCM_VOTE(x, y)          (BCM_COMMIT | BCM_VALID | ((uint32_t)(x) & 0x3fff) << 14 | ((uint32_t)(y) & 0x3fff))

static vm_offset_t rsc, cmd_db;
static uint32_t cmd_db_size;

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

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("rpmh: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		/* (an SM7150's database is small, 4 KB or so: every entry of a slave must lie inside it) */
		if (slave->id == 0 || slave->count > 512 || sizeof(*db) + slave->header_offset +
		    (size_t)slave->count * sizeof(*entry) > cmd_db_size) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("rpmh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* The command database and the command slots, mapped once. */
static bool
rpmh_map(void)
{
	vm_offset_t dict, rsc_phys = RSC_DEFAULT_PHYS;
	uint32_t db_phys, db_size;

	if (cmd_db != 0 && rsc != 0) {
		return true;
	}
	PE_parse_boot_argn("soc_rsc", &rsc_phys, sizeof(rsc_phys));
	dict = ml_io_map(AOP_DICT_PHYS, 0x1000);
	if (dict == 0) {
		return false;
	}
	db_phys = rd(dict + AOP_DICT_OFFSET);
	db_size = rd(dict + AOP_DICT_OFFSET + 4);
	if (db_phys == 0 || db_size < sizeof(struct cmd_db_header) || db_size > 0x100000) {
		printf("rpmh: no command database (dictionary says %#x, %#x bytes)\n", db_phys, db_size);
		return false;
	}
	cmd_db = ml_io_map_wcomb(db_phys, (db_size + 0xfff) & ~0xfffu);    /* memory, not registers: no alignment rules */
	cmd_db_size = db_size;
	rsc = ml_io_map(rsc_phys, RSC_SIZE);
	if (cmd_db == 0 || rsc == 0) {
		return false;
	}
	printf("rpmh: command database at %#x, %#x bytes; RSC at %#lx\n", db_phys, db_size, rsc_phys);
	return true;
}

uint32_t
hakios_rpmh_address(const char *name)
{
	return rpmh_map() ? rpmh_address(name) : 0;
}

static bool
rpmh_write_locked(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("rpmh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);        /* the status bit follows the enable; the interrupt itself stays masked */
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}


/* Up to three register writes to RPMh, waited for; may sleep. */
bool
hakios_rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	bool done;

	if (count == 0 || count > 3 || !rpmh_map()) {
		return false;
	}
	lck_mtx_lock(&hakios_rsc_lock);
	done = rpmh_write_locked(address, data, count);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done;
}

/* An LDO on at this voltage, in its high-power mode; may sleep. */
bool
hakios_rpmh_ldo(const char *name, uint32_t millivolts)
{
	uint32_t base = hakios_rpmh_address(name);
	uint32_t address[3], data[3];

	if (base == 0) {
		printf("rpmh: %s is not in the command database\n", name);
		return false;
	}
	address[0] = base + VRM_VOLTAGE;
	data[0] = millivolts;
	address[1] = base + VRM_MODE;
	data[1] = VRM_MODE_LDO_HPM;
	address[2] = base + VRM_ENABLE;
	data[2] = 1;
	if (!hakios_rpmh_write(address, data, 3)) {
		return false;
	}
	printf("rpmh: %s (RPMh %#x) on at %u mV\n", name, base, millivolts);
	return true;
}

/* A bus clock manager voted to the most there is (commit, valid, average and peak at the top); may sleep. */
bool
hakios_rpmh_bcm_max(const char *name)
{
	uint32_t address = hakios_rpmh_address(name), data = BCM_VOTE(0x3fff, 0x3fff);

	if (address == 0) {
		printf("rpmh: no \"%s\" in the command database: not voted\n", name);
		return false;
	}
	if (!hakios_rpmh_write(&address, &data, 1)) {
		printf("rpmh: \"%s\" (%#x): the vote was not taken\n", name, address);
		return false;
	}
	printf("rpmh: \"%s\" (%#x) voted to the most there is\n", name, address);
	return true;
}
