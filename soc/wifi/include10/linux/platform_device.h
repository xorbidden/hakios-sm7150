/*
 * Platform devices, as snoc.c is one: LinuxKPI's header has only placeholders (its driver has no
 * probe, and registering one fails). Here a device carries its resources, and the one driver a
 * file declares with module_platform_driver() is published as lx_platform_driver, for the bus file
 * (src/lx_snoc.c) to probe with the device it makes from this phone's icnss node.
 */
#ifndef _LINUXKPI_LINUX_PLATFORM_DEVICE_H
#define _LINUXKPI_LINUX_PLATFORM_DEVICE_H
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/ioport.h>

struct platform_device {
	const char		*name;
	int			id;
	bool			id_auto;
	struct device		dev;
	u32			num_resources;
	struct resource		*resource;
};

struct platform_driver {
	int			(*probe)(struct platform_device *);
	void			(*remove)(struct platform_device *);
	void			(*shutdown)(struct platform_device *);
	struct device_driver	driver;
};

#define	dev_is_platform(dev)	(false)
#define	to_platform_device(d)	container_of((d), struct platform_device, dev)

static inline void *
dev_get_platdata(struct device *dev)
{
	return (NULL);
}
static inline void
platform_set_drvdata(struct platform_device *pdev, void *data)
{
	dev_set_drvdata(&pdev->dev, data);
}
static inline void *
platform_get_drvdata(const struct platform_device *pdev)
{
	return (dev_get_drvdata(&pdev->dev));
}

/* the driver of the file that declares one; registering is the bus file's probing */
extern struct platform_driver *const lx_platform_driver;
#define	module_platform_driver(drv)	struct platform_driver *const lx_platform_driver = &(drv)
#endif
