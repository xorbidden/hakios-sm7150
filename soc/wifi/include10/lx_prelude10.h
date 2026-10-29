/*
 * Included before every file of ath10k (tools/mk-ath10k.sh), after the prelude ath11k's files have
 * (lx_prelude.h). What ath10k uses of Linux beyond that, small things each; the headers beside this
 * one (include10/) are found before LinuxKPI's and ours, and serve ath10k alone.
 */
#include <lx_prelude.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/of.h>
#include <linux/err.h>
#include <lx_more10.h>

/* guard(spinlock_bh), as Linux's spinlock.h defines it beside LinuxKPI's spinlock_irqsave */
DEFINE_LOCK_GUARD_1(spinlock_bh, spinlock_t, spin_lock_bh(_T->lock), spin_unlock_bh(_T->lock))

/* the device tree's byte properties: this phone's has none of the ones ath10k asks for */
static inline int
of_property_read_u8_array(const struct device_node *np, const char *name, u8 *out, size_t n)
{
	return (-EINVAL);
}
static inline int
of_property_read_u8(const struct device_node *np, const char *name, u8 *out)
{
	return (-EINVAL);
}

/* calibration data from an NVMEM cell: there is none here, the board's data comes over QMI */
struct nvmem_cell;
static inline struct nvmem_cell *
devm_nvmem_cell_get(struct device *dev, const char *id)
{
	return (ERR_PTR(-ENOENT));
}
static inline void *
nvmem_cell_read(struct nvmem_cell *cell, size_t *len)
{
	return (ERR_PTR(-ENOENT));
}

/*
 * What the WCN3990's platform (lx_snoc.c) asks of the kernel beyond the common host's (lx_os.h): its supplies, RPMh's
 * LDOs by name ("ldoc1") and voltage, 1 when on; and its firmware's memory given to the modem and the radio by the
 * hypervisor (owners by VMID), 0 or an errno. Answered by soc/drivers/wifi/hakios.wifi_sm7150.c.
 */
int	 lx_os_supply(const char *name, unsigned millivolts);
int	 lx_os_assign(unsigned long long physical, unsigned long long size, const unsigned *from, unsigned from_count,
	    const unsigned *to, unsigned to_count);
