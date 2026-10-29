/*
 * What snoc.c, the WCN3990's bus, asks of a Linux kernel and LinuxKPI's headers do not declare: the
 * platform device it is probed as, its supplies, and the IOMMU. The functions are the bus file's
 * (src/lx_snoc.c), on this phone's icnss node (the radio's registers at 0x18800000, twelve copy
 * engine interrupts, four RPMh supplies); the trivial ones are here.
 */
#ifndef _LX_MORE10_H_
#define _LX_MORE10_H_
#include <linux/types.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/iommu.h>
#include <linux/netdevice.h>

/* --- the platform device (linux/platform_device.h, ours): its register windows and interrupts by name, its properties */
struct resource *platform_get_resource_byname(struct platform_device *pdev, unsigned int type, const char *name);
int platform_get_irq(struct platform_device *pdev, unsigned int n);
int device_property_read_u32(struct device *dev, const char *name, u32 *out);
void *devm_memremap(struct device *dev, resource_size_t offset, size_t size, unsigned long flags);
const void *device_get_match_data(const struct device *dev);
/* request_irq() leaves the interrupt disabled (Linux's newer name for IRQ_NOAUTOEN) */
#define IRQF_NO_AUTOEN		0x00080000

/* --- a device of its own for the firmware's memory, behind the IOMMU (only with a "wifi-firmware"
 * node, which the Mi 9T's tree has not got: its MSA is a fixed region) */
struct platform_device_info {
	struct device *parent;
	struct fwnode_handle *fwnode;
	bool of_node_reused;
	const char *name;
	int id;
	const struct resource *res;
	unsigned int num_res;
	const void *data;
	size_t size_data;
	u64 dma_mask;
	const struct property_entry *properties;
};
struct platform_device *platform_device_register_full(const struct platform_device_info *info);
void platform_device_unregister(struct platform_device *pdev);
int of_dma_configure(struct device *dev, struct device_node *np, bool force_dma);
#define IOMMU_READ		(1 << 0)
#define IOMMU_WRITE		(1 << 1)
struct iommu_domain *iommu_paging_domain_alloc(struct device *dev);
int iommu_attach_device(struct iommu_domain *domain, struct device *dev);
void iommu_detach_device(struct iommu_domain *domain, struct device *dev);
void iommu_domain_free(struct iommu_domain *domain);
int iommu_map(struct iommu_domain *domain, unsigned long iova, phys_addr_t paddr, size_t size, int prot, gfp_t gfp);
size_t iommu_unmap(struct iommu_domain *domain, unsigned long iova, size_t size);

/* --- the supplies, by the device tree's names (icnss's vdd-*-supply): RPMh votes in the bus file */
struct regulator;
struct regulator_bulk_data {
	const char *supply;
	struct regulator *consumer;
	int init_load_uA;
	int ret;
};
int devm_regulator_bulk_get(struct device *dev, int num, struct regulator_bulk_data *consumers);
int regulator_bulk_enable(int num, struct regulator_bulk_data *consumers);
int regulator_bulk_disable(int num, struct regulator_bulk_data *consumers);

/* --- a device-tree string list (mac.c's "qcom,..." lookups): this phone's tree has none of them */
static inline int
of_property_read_string_index(const struct device_node *np, const char *name, int index, const char **out)
{
	return (-EINVAL);
}

/* NAPI in a thread of its own: the polling here runs in the bus file's thread anyway */
static inline int
netif_threaded_enable(struct net_device *dev)
{
	return (0);
}
#endif
