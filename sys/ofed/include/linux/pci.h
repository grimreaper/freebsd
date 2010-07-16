/*-
 * Copyright (c) 2010 Isilon Systems, Inc.
 * Copyright (c) 2010 iX Systems, Inc.
 * Copyright (c) 2010 Panasas, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef	_LINUX_PCI_H_
#define	_LINUX_PCI_H_

#include <linux/types.h>

#include <sys/pciio.h>
#include <sys/kobj.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pci_private.h>

#include <machine/resource.h>

#include <linux/init.h>
#include <linux/list.h>
#include <linux/dmapool.h>
#include <linux/dma-mapping.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <asm/atomic.h>
#include <linux/device.h>

struct pci_device_id {
	uint32_t	vendor;
	uint32_t	device;
        uint32_t	subvendor;
	uint32_t	subdevice;
	uint32_t	class_mask;
	uintptr_t	driver_data;
};

#define	MODULE_DEVICE_TABLE(bus, table)
#define	PCI_ANY_ID		(-1)
#define	PCI_VENDOR_ID_MELLANOX	0x15b3

#define PCI_VDEVICE(vendor, device)					\
	    PCI_VENDOR_ID_##vendor, (device), PCI_ANY_ID, PCI_ANY_ID, 0, 0

#define	to_pci_dev(n)	container_of(n, struct pci_dev, dev)

#define	IORESOURCE_MEM	SYS_RES_MEMORY
#define	IORESOURCE_IO	SYS_RES_IOPORT
#define	IORESOURCE_IRQ	SYS_RES_IRQ

struct pci_dev;

struct pci_driver {
	struct list_head		links;
	char				*name;
	struct pci_device_id		*id_table;
	int  (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
	void (*remove)(struct pci_dev *dev);
	driver_t			driver;
	devclass_t			bsdclass;
};

struct list_head pci_drivers;

#define	__devexit_p(x)	x

struct pci_dev {
	struct linux_device	dev;
	struct pci_driver	*pdrv;
	uint64_t		dma_mask;
	uint16_t		device;
	unsigned int		irq;
};

static inline struct resource_list_entry *
_pci_get_rle(struct pci_dev *pdev, int bar)
{
	struct pci_devinfo *dinfo;
	struct resource_list *rl;
	struct resource_list_entry *rle;

	dinfo = device_get_ivars(pdev->dev.bsddev);
	rl = &dinfo->resources;
	if ((rle = resource_list_find(rl, SYS_RES_MEMORY, bar)) == NULL)
		rle = resource_list_find(rl, SYS_RES_IOPORT, bar);
	return (rle);
}

static inline unsigned long
pci_resource_start(struct pci_dev *pdev, int bar)
{
	struct resource_list_entry *rle;

	if ((rle = _pci_get_rle(pdev, bar)) == NULL)
		return (0);
	return rle->start;
}

static inline unsigned long
pci_resource_len(struct pci_dev *pdev, int bar)
{
	struct resource_list_entry *rle;

	if ((rle = _pci_get_rle(pdev, bar)) == NULL)
		return (0);
	return rle->count;
}

/*
 * XXX All drivers just seem to want to inspect the type not flags.
 */
static inline int
pci_resource_flags(struct pci_dev *pdev, int bar)
{
	struct resource_list_entry *rle;

	if ((rle = _pci_get_rle(pdev, bar)) == NULL)
		return (0);
	return rle->type;
}

static inline const char *
pci_name(struct pci_dev *d)
{

	return device_get_desc(d->dev.bsddev);
}

static inline void *
pci_get_drvdata(struct pci_dev *pdev)
{

	return dev_get_drvdata(&pdev->dev);
}

static inline void
pci_set_drvdata(struct pci_dev *pdev, void *data)
{

	dev_set_drvdata(&pdev->dev, data);
}

static inline int
pci_enable_device(struct pci_dev *pdev)
{

	pci_enable_io(pdev->dev.bsddev, SYS_RES_IOPORT);
	pci_enable_io(pdev->dev.bsddev, SYS_RES_MEMORY);
	return (0);
}

static inline void
pci_disable_device(struct pci_dev *pdev)
{
}

static inline int
pci_set_master(struct pci_dev *pdev)
{

	pci_enable_busmaster(pdev->dev.bsddev);
	return (0);
}

static inline int
pci_request_region(struct pci_dev *pdev, int bar, const char *res_name)
{
	int rid;
	int type;

	type = pci_resource_flags(pdev, bar);

	rid = PCIR_BAR(bar);
	if (bus_alloc_resource_any(pdev->dev.bsddev, type, &rid,
	    RF_ACTIVE) == NULL)
		return (-EINVAL);
	return (0);
}

static inline void
pci_release_region(struct pci_dev *pdev, int bar)
{
	struct resource_list_entry *rle;

	if ((rle = _pci_get_rle(pdev, bar)) == NULL)
		return;
	bus_release_resource(pdev->dev.bsddev, rle->type, rle->rid, rle->res);
}

static inline void
pci_disable_msix(struct pci_dev *pdev)
{

	pci_release_msi(pdev->dev.bsddev);
}

static struct pci_driver *
linux_pci_find(device_t dev, struct pci_device_id **idp)
{
	struct pci_device_id *id;
	struct pci_driver *pdrv;
	uint16_t vendor;
	uint16_t device;

	vendor = pci_get_vendor(dev);
	device = pci_get_device(dev);

	list_for_each_entry(pdrv, &pci_drivers, links) {
		for (id = pdrv->id_table; id->vendor != 0; id++) {
			if (vendor == id->vendor && device == id->device) {
				*idp = id;
				return (pdrv);
			}
		}
	}
	return (NULL);
}

static inline int
linux_pci_probe(device_t dev)
{
	struct pci_device_id *id;
	struct pci_driver *pdrv;

	if ((pdrv = linux_pci_find(dev, &id)) == NULL)
		return (ENXIO);
	device_set_desc(dev, pdrv->name);
	return (0);
}

static inline int
linux_pci_attach(device_t dev)
{
	struct pci_dev *pdev;
	struct pci_driver *pdrv;
	struct pci_device_id *id;
	int error;

	pdrv = linux_pci_find(dev, &id);
	pdev = device_get_softc(dev);
	pdev->dev.bsddev = dev;
	pdev->device = device_get_unit(dev);
	pdev->dev.dma_mask = &pdev->dma_mask;
	pdev->pdrv = pdrv;
	kobject_init(&pdev->dev.kobj, NULL);
	kobject_set_name(&pdev->dev.kobj, device_get_nameunit(dev));
	error = pdrv->probe(pdev, id);
	if (error)
		return (-error);
	return (0);
}

static inline int
linux_pci_detach(device_t dev)
{
	struct pci_dev *pdev;

	pdev = device_get_softc(dev);
	pdev->pdrv->remove(pdev);
	return (0);
}

static device_method_t pci_methods[] = {
	DEVMETHOD(device_probe, linux_pci_probe),
	DEVMETHOD(device_attach, linux_pci_attach),
	DEVMETHOD(device_detach, linux_pci_detach),
	{0, 0}
};

static inline int
pci_register_driver(struct pci_driver *pdrv)
{
	devclass_t bus;
	int error;

	if (pci_drivers.prev == NULL && pci_drivers.next == NULL)
		INIT_LIST_HEAD(&pci_drivers);
	list_add(&pdrv->links, &pci_drivers);
	bus = devclass_find("pci");
	pdrv->driver.name = pdrv->name;
	pdrv->driver.methods = pci_methods;
	pdrv->driver.size = sizeof(struct pci_dev);
	error = devclass_add_driver(bus, &pdrv->driver, BUS_PASS_DEFAULT,
	    &pdrv->bsdclass);
	if (error)
		return (-error);
	return (0);
}

static inline void
pci_unregister_driver(struct pci_driver *pdrv)
{
	devclass_t bus;

	list_del(&pdrv->links);
	bus = devclass_find("pci");
	devclass_delete_driver(bus, &pdrv->driver);
}

#define	PCI_DMA_BIDIRECTIONAL	0
#define	PCI_DMA_TODEVICE	1
#define	PCI_DMA_FROMDEVICE	2
#define	PCI_DMA_NONE		3

#define	pci_pool		dma_pool
#define pci_pool_destroy	dma_pool_destroy
#define pci_pool_alloc		dma_pool_alloc
#define pci_pool_free		dma_pool_free
#define	pci_pool_create(name, pdev, size, align, allocation)		\
	    dma_pool_create(name, &(pdev)->dev, size, align, allocation)
#define	pci_free_consistent(hwdev, size, vaddr, dma_handle)		\
	    dma_free_coherent((hwdev) == NULL ? NULL : &(hwdev)->dev,	\
		size, vaddr, dma_handle)
#define	pci_map_sg(hwdev, sg, nents, direction)				\
	    dma_map_sg((hwdev) == NULL ? NULL : &(hwdev->dev),		\
		sg, nents, (enum dma_data_direction)direction)
#define	pci_unmap_sg(hwdev, sg, nents, direction)			\
	    dma_unmap_sg((hwdev) == NULL ? NULL : &(hwdev)->dev,	\
		sg, nents, (enum dma_data_direction)direction)
#define	pci_set_dma_mask(pdev, mask)	dma_set_mask(&(pdev)->dev, (mask))
#define	pci_set_consistent_dma_mask(pdev, mask)				\
	    dma_set_coherent_mask(&(pdev)->dev, (mask))

#endif	/* _LINUX_PCI_H_ */
