/* $FreeBSD$ */
/*-
 * Copyright (c) 2008 Hans Petter Selasky. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "opt_ddb.h"

#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/linker_set.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <sys/unistd.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/priv.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#define	USB_DEBUG_VAR usb_ctrl_debug

#include <dev/usb/usb_core.h>
#include <dev/usb/usb_debug.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_dynamic.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_hub.h>

#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>

/* function prototypes  */

static device_probe_t usb_probe;
static device_attach_t usb_attach;
static device_detach_t usb_detach;

static void	usb_attach_sub(device_t, struct usb_bus *);
static void	usb_bus_mem_free_all(struct usb_bus *);

/* static variables */

#ifdef USB_DEBUG
static int usb_ctrl_debug = 0;

SYSCTL_NODE(_hw_usb, OID_AUTO, ctrl, CTLFLAG_RW, 0, "USB controller");
SYSCTL_INT(_hw_usb_ctrl, OID_AUTO, debug, CTLFLAG_RW, &usb_ctrl_debug, 0,
    "Debug level");
#endif

static int usb_no_boot_wait = 0;
TUNABLE_INT("hw.usb.no_boot_wait", &usb_no_boot_wait);
SYSCTL_INT(_hw_usb, OID_AUTO, no_boot_wait, CTLFLAG_RDTUN, &usb_no_boot_wait, 0,
    "No device enumerate waiting at boot.");

static devclass_t usb_devclass;

static device_method_t usb_methods[] = {
	DEVMETHOD(device_probe, usb_probe),
	DEVMETHOD(device_attach, usb_attach),
	DEVMETHOD(device_detach, usb_detach),
	DEVMETHOD(device_suspend, bus_generic_suspend),
	DEVMETHOD(device_resume, bus_generic_resume),
	DEVMETHOD(device_shutdown, bus_generic_shutdown),
	{0, 0}
};

static driver_t usb_driver = {
	.name = "usbus",
	.methods = usb_methods,
	.size = 0,
};

DRIVER_MODULE(usbus, ohci, usb_driver, usb_devclass, 0, 0);
DRIVER_MODULE(usbus, uhci, usb_driver, usb_devclass, 0, 0);
DRIVER_MODULE(usbus, ehci, usb_driver, usb_devclass, 0, 0);
DRIVER_MODULE(usbus, at91_udp, usb_driver, usb_devclass, 0, 0);
DRIVER_MODULE(usbus, uss820, usb_driver, usb_devclass, 0, 0);

/*------------------------------------------------------------------------*
 *	usb_probe
 *
 * This function is called from "{ehci,ohci,uhci}_pci_attach()".
 *------------------------------------------------------------------------*/
static int
usb_probe(device_t dev)
{

	DPRINTF("\n");
	return (0);
}

/*------------------------------------------------------------------------*
 *	usb_attach
 *------------------------------------------------------------------------*/
static int
usb_attach(device_t dev)
{
	struct usb_bus *bus = device_get_ivars(dev);

	DPRINTF("\n");

	if (bus == NULL) {
		device_printf(dev, "USB device has no ivars\n");
		return (ENXIO);
	}

	if (usb_no_boot_wait == 0) {
		/* delay vfs_mountroot until the bus is explored */
		bus->bus_roothold = root_mount_hold(device_get_nameunit(dev));
	}

	usb_attach_sub(dev, bus);

	return (0);			/* return success */
}

/*------------------------------------------------------------------------*
 *	usb_detach
 *------------------------------------------------------------------------*/
static int
usb_detach(device_t dev)
{
	struct usb_bus *bus = device_get_softc(dev);

	DPRINTF("\n");

	if (bus == NULL) {
		/* was never setup properly */
		return (0);
	}
	/* Stop power watchdog */
	usb_callout_drain(&bus->power_wdog);

	/* Let the USB explore process detach all devices. */
	if (bus->bus_roothold != NULL) {
		root_mount_rel(bus->bus_roothold);
		bus->bus_roothold = NULL;
	}

	taskqueue_enqueue(bus->explore_tq, &bus->detach_task);
	taskqueue_drain(bus->explore_tq, &bus->detach_task);

	/* Get rid of USB taskqueues */
	taskqueue_free(bus->giant_callback_tq);
	taskqueue_free(bus->non_giant_callback_tq);
	taskqueue_free(bus->control_xfer_tq);
	taskqueue_free(bus->explore_tq);

	return (0);
}

/*------------------------------------------------------------------------*
 *	usb_bus_explore
 *
 * This function is used to explore the device tree from the root.
 *------------------------------------------------------------------------*/
static void
usb_bus_explore(void *arg, int npending)
{
	struct usb_bus *bus = arg;
	struct usb_device *udev = bus->devices[USB_ROOT_HUB_ADDR];

	USB_BUS_LOCK(bus);
	if (udev && udev->hub) {
		if (bus->do_probe) {
			bus->do_probe = 0;
			bus->generation++;
		}

#ifdef DDB
		/*
		 * The following three lines of code are only here to
		 * recover from DDB:
		 */
		taskqueue_unblock(bus->control_xfer_tq);
		taskqueue_unblock(bus->giant_callback_tq);
		taskqueue_unblock(bus->non_giant_callback_tq);
#endif

		USB_BUS_UNLOCK(bus);

#if USB_HAVE_POWERD
		/*
		 * First update the USB power state!
		 */
		usb_bus_powerd(bus);
#endif
		 /* Explore the Root USB HUB. */
		(udev->hub->explore) (udev);
		USB_BUS_LOCK(bus);
	}
	if (bus->bus_roothold != NULL) {
		root_mount_rel(bus->bus_roothold);
		bus->bus_roothold = NULL;
	}
	USB_BUS_UNLOCK(bus);
}

/*------------------------------------------------------------------------*
 *	usb_bus_detach
 *
 * This function is used to detach the device tree from the root.
 *------------------------------------------------------------------------*/
static void
usb_bus_detach(void *arg, int npending)
{
	struct usb_bus *bus = arg;
	struct usb_device *udev = bus->devices[USB_ROOT_HUB_ADDR];
	device_t dev = bus->bdev;

	USB_BUS_LOCK(bus);
	/* clear the softc */
	device_set_softc(dev, NULL);
	USB_BUS_UNLOCK(bus);

	/* detach children first */
	mtx_lock(&Giant);
	bus_generic_detach(dev);
	mtx_unlock(&Giant);

	/*
	 * Free USB device and all subdevices, if any.
	 */
	usb_free_device(udev, 0);

	USB_BUS_LOCK(bus);
	/* clear bdev variable last */
	bus->bdev = NULL;
	USB_BUS_UNLOCK(bus);
}

static void
usb_power_wdog(void *arg)
{
	struct usb_bus *bus = arg;

	USB_BUS_LOCK_ASSERT(bus, MA_OWNED);

	usb_callout_reset(&bus->power_wdog,
	    4 * hz, usb_power_wdog, arg);

#ifdef DDB
	/*
	 * The following line of code is only here to recover from
	 * DDB:
	 */
	taskqueue_unblock(bus->explore_tq);	/* recover from DDB */
#endif

#if USB_HAVE_POWERD
	USB_BUS_UNLOCK(bus);

	usb_bus_power_update(bus);

	USB_BUS_LOCK(bus);
#endif
}

/*------------------------------------------------------------------------*
 *	usb_bus_attach
 *
 * This function attaches USB in context of the explore thread.
 *------------------------------------------------------------------------*/
static void
usb_bus_attach(void *arg, int npending)
{
	struct usb_bus *bus = arg;
	struct usb_device *child;
	device_t dev = bus->bdev;
	usb_error_t err;
	enum usb_dev_speed speed;

	DPRINTF("\n");

	switch (bus->usbrev) {
	case USB_REV_1_0:
		speed = USB_SPEED_FULL;
		device_printf(bus->bdev, "12Mbps Full Speed USB v1.0\n");
		break;
	case USB_REV_1_1:
		speed = USB_SPEED_FULL;
		device_printf(bus->bdev, "12Mbps Full Speed USB v1.1\n");
		break;
	case USB_REV_2_0:
		speed = USB_SPEED_HIGH;
		device_printf(bus->bdev, "480Mbps High Speed USB v2.0\n");
		break;
	case USB_REV_2_5:
		speed = USB_SPEED_VARIABLE;
		device_printf(bus->bdev, "480Mbps Wireless USB v2.5\n");
		break;
	default:
		device_printf(bus->bdev, "Unsupported USB revision\n");
		return;
	}

	/* default power_mask value */
	bus->hw_power_state =
	  USB_HW_POWER_CONTROL |
	  USB_HW_POWER_BULK |
	  USB_HW_POWER_INTERRUPT |
	  USB_HW_POWER_ISOC |
	  USB_HW_POWER_NON_ROOT_HUB;

	/* make sure power is set at least once */
	if (bus->methods->set_hw_power != NULL)
		(bus->methods->set_hw_power) (bus);

	/* Allocate the Root USB device */
	child = usb_alloc_device(bus->bdev, bus, NULL, 0, 0, 1,
	    speed, USB_MODE_HOST);
	if (child) {
		err = usb_probe_and_attach(child,
		    USB_IFACE_INDEX_ANY);
		if (!err) {
			if (bus->devices[USB_ROOT_HUB_ADDR] == NULL ||
			    bus->devices[USB_ROOT_HUB_ADDR]->hub == NULL)
				err = USB_ERR_NO_ROOT_HUB;
		}
	} else
		err = USB_ERR_NOMEM;

	if (err) {
		device_printf(bus->bdev, "Root HUB problem, error=%s\n",
		    usbd_errstr(err));
	}

	USB_BUS_LOCK(bus);
	/* set softc - we are ready */
	device_set_softc(dev, bus);
	/* start watchdog */
	usb_power_wdog(bus);
	USB_BUS_UNLOCK(bus);
}

/*------------------------------------------------------------------------*
 *	usb_attach_sub
 *
 * This function creates a thread which runs the USB attach code.
 *------------------------------------------------------------------------*/
static void
usb_attach_sub(device_t dev, struct usb_bus *bus)
{
	const char *pname = device_get_nameunit(dev);

	GIANT_REQUIRED;
	if (usb_devclass_ptr == NULL)
		usb_devclass_ptr = devclass_find("usbus");

	/* Initialise USB explore taskqueue and tasks */
	bus->explore_tq = taskqueue_create("usb_explore_taskq", M_WAITOK,
	    taskqueue_thread_enqueue, &bus->explore_tq);
	/*
	 * NOTE: the thread count always should be 1.  If more, it couldn't
	 * guarantee the serialization between attach, detach and explore.
	 */
	taskqueue_start_threads(&bus->explore_tq, 1, USB_PRI_MED,
	    "USB explore taskq");
	TASK_INIT(&bus->attach_task, 0, usb_bus_attach, bus);
	TASK_INIT(&bus->detach_task, 0, usb_bus_detach, bus);
	TASK_INIT(&bus->explore_task, 0, usb_bus_explore, bus);

	/* Creates USB taskqueues for callback and control transfer */
	bus->giant_callback_tq = taskqueue_create("usb_giant_callback_taskq",
	    M_WAITOK, taskqueue_thread_enqueue, &bus->giant_callback_tq);
	bus->non_giant_callback_tq =
	    taskqueue_create("usb_non_giant_callback_taskq", M_WAITOK,
	    taskqueue_thread_enqueue, &bus->non_giant_callback_tq);
	bus->control_xfer_tq = taskqueue_create("usb_ctrlxfer_taskq", M_WAITOK,
	    taskqueue_thread_enqueue, &bus->control_xfer_tq);
	/* Creates taskqueue threads */
	taskqueue_start_threads(&bus->giant_callback_tq, 1, USB_PRI_MED,
	    "%s giant callback taskq", pname);
	taskqueue_start_threads(&bus->non_giant_callback_tq, 1, USB_PRI_HIGH,
	    "%s non giant callback taskq", pname);
	taskqueue_start_threads(&bus->control_xfer_tq, 1, USB_PRI_MED,
	    "%s control xfer taskq", pname);

	/* Get final attach going */
	taskqueue_enqueue(bus->explore_tq, &bus->attach_task);
	/* Do initial explore */
	usb_needs_explore(bus, 1);
}

SYSUNINIT(usb_bus_unload, SI_SUB_KLD, SI_ORDER_ANY, usb_bus_unload, NULL);

/*------------------------------------------------------------------------*
 *	usb_bus_mem_flush_all_cb
 *------------------------------------------------------------------------*/
#if USB_HAVE_BUSDMA
static void
usb_bus_mem_flush_all_cb(struct usb_bus *bus, struct usb_page_cache *pc,
    struct usb_page *pg, usb_size_t size, usb_size_t align)
{

	usb_pc_cpu_flush(pc);
}
#endif

/*------------------------------------------------------------------------*
 *	usb_bus_mem_flush_all - factored out code
 *------------------------------------------------------------------------*/
#if USB_HAVE_BUSDMA
void
usb_bus_mem_flush_all(struct usb_bus *bus)
{

	if (bus->busmem_func != NULL)
		bus->busmem_func(bus, usb_bus_mem_flush_all_cb);
}
#endif

/*------------------------------------------------------------------------*
 *	usb_bus_mem_alloc_all_cb
 *------------------------------------------------------------------------*/
#if USB_HAVE_BUSDMA
static void
usb_bus_mem_alloc_all_cb(struct usb_bus *bus, struct usb_page_cache *pc,
    struct usb_page *pg, usb_size_t size, usb_size_t align)
{

	/* need to initialize the page cache */
	pc->tag_parent = bus->dma_parent_tag;

	if (usb_pc_alloc_mem(pc, pg, size, align))
		bus->alloc_failed = 1;
}
#endif

/*------------------------------------------------------------------------*
 *	usb_bus_mem_alloc_all - factored out code
 *
 * Returns:
 *    0: Success
 * Else: Failure
 *------------------------------------------------------------------------*/
static uint8_t
usb_bus_mem_alloc_all(struct usb_bus *bus, bus_dma_tag_t dmat)
{

#if USB_HAVE_BUSDMA
	usb_dma_tag_setup(bus->dma_parent_tag, bus->dma_tags,
	    dmat, &bus->bus_mtx, NULL, 32, USB_BUS_DMA_TAG_MAX, NULL);
	if (bus->busmem_func != NULL)
		bus->busmem_func(bus, usb_bus_mem_alloc_all_cb);
#endif
	if (bus->alloc_failed)
		usb_bus_mem_free_all(bus);
	return (bus->alloc_failed);
}

/*------------------------------------------------------------------------*
 *	usb_bus_mem_free_all_cb
 *------------------------------------------------------------------------*/
#if USB_HAVE_BUSDMA
static void
usb_bus_mem_free_all_cb(struct usb_bus *bus, struct usb_page_cache *pc,
    struct usb_page *pg, usb_size_t size, usb_size_t align)
{

	usb_pc_free_mem(pc);
}
#endif

/*------------------------------------------------------------------------*
 *	usb_bus_mem_free_all - factored out code
 *------------------------------------------------------------------------*/
static void
usb_bus_mem_free_all(struct usb_bus *bus)
{

#if USB_HAVE_BUSDMA
	if (bus->busmem_func != NULL)
		bus->busmem_func(bus, usb_bus_mem_free_all_cb);
	usb_dma_tag_unsetup(bus->dma_parent_tag);
#endif
}

int
usb_bus_struct_init(struct usb_bus *bus, device_t dev,
    struct usb_device **udevs, uint8_t udevsmax,
    void (*busmem_func)(struct usb_bus *, usb_bus_mem_callback_t *))
{

	if (udevsmax > USB_MAX_DEVICES || udevsmax < USB_MIN_DEVICES ||
	    udevs == NULL) {
		DPRINTFN(0, "Devices field has not been "
		    "initialised properly\n");
		return (ENXIO);
	}

	/* initialise some bus fields */
	bus->parent = dev;
	bus->devices = udevs;
	bus->devices_max = udevsmax;
	bus->busmem_func = busmem_func;
	bus->alloc_failed = 0;
	bus->generation = 1;

	mtx_init(&bus->bus_mtx, device_get_nameunit(bus->parent),
	    NULL, MTX_DEF);

	usb_callout_init_mtx(&bus->power_wdog, &bus->bus_mtx, 0);

	TAILQ_INIT(&bus->intr_q.head);

	/* get all DMA memory */
	if (usb_bus_mem_alloc_all(bus, USB_GET_DMA_TAG(dev)))
		return (ENOMEM);
	return (0);
}

void
usb_bus_struct_fini(struct usb_bus *bus)
{

	usb_bus_mem_free_all(bus);
	mtx_destroy(&bus->bus_mtx);
}
