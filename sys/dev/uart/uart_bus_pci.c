/*-
 * Copyright (c) 2006 Marcel Moolenaar
 * Copyright (c) 2001 M. Warner Losh
 * All rights reserved.
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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <machine/bus.h>
#include <sys/rman.h>
#include <machine/resource.h>

#include <dev/pci/pcivar.h>

#include <dev/uart/uart.h>
#include <dev/uart/uart_bus.h>

#define	DEFAULT_RCLK	1843200

static int uart_pci_probe(device_t dev);

static device_method_t uart_pci_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		uart_pci_probe),
	DEVMETHOD(device_attach,	uart_bus_attach),
	DEVMETHOD(device_detach,	uart_bus_detach),
	{ 0, 0 }
};

static driver_t uart_pci_driver = {
	uart_driver_name,
	uart_pci_methods,
	sizeof(struct uart_softc),
};

struct pci_id {
	uint16_t	vendor;
	uint16_t	device;
	uint16_t	subven;
	uint16_t	subdev;
	const char	*desc;
	int		rid;
	int		rclk;
};

static struct pci_id pci_ns8250_ids[] = {
{ 0x1028, 0x0008, 0xffff, 0, "Dell Remote Access Card III", 0x14,
	128 * DEFAULT_RCLK },
{ 0x1028, 0x0012, 0xffff, 0, "Dell RAC 4 Daughter Card Virtual UART", 0x14,
	128 * DEFAULT_RCLK },
{ 0x1033, 0x0074, 0x1033, 0x8014, "NEC RCV56ACF 56k Voice Modem", 0x10 },
{ 0x1033, 0x007d, 0x1033, 0x8012, "NEC RS232C", 0x10 },
{ 0x103c, 0x1048, 0x103c, 0x1227, "HP Diva Serial [GSP] UART - Powerbar SP2",
	0x10 },
{ 0x103c, 0x1048, 0x103c, 0x1301, "HP Diva RMP3", 0x14 },
{ 0x103c, 0x1290, 0xffff, 0, "HP Auxiliary Diva Serial Port", 0x18 },
{ 0x11c1, 0x0480, 0xffff, 0, "Agere Systems Venus Modem (V90, 56KFlex)", 0x14 },
{ 0x115d, 0x0103, 0xffff, 0, "Xircom Cardbus Ethernet + 56k Modem", 0x10 },
{ 0x12b9, 0x1008, 0xffff, 0, "3Com 56K FaxModem Model 5610", 0x10 },
{ 0x131f, 0x1000, 0xffff, 0, "Siig CyberSerial (1-port) 16550", 0x18 },
{ 0x131f, 0x1001, 0xffff, 0, "Siig CyberSerial (1-port) 16650", 0x18 },
{ 0x131f, 0x1002, 0xffff, 0, "Siig CyberSerial (1-port) 16850", 0x18 },
{ 0x131f, 0x2000, 0xffff, 0, "Siig CyberSerial (1-port) 16550", 0x10 },
{ 0x131f, 0x2001, 0xffff, 0, "Siig CyberSerial (1-port) 16650", 0x10 },
{ 0x131f, 0x2002, 0xffff, 0, "Siig CyberSerial (1-port) 16850", 0x10 },
{ 0x135c, 0x0190, 0xffff, 0, "Quatech SSCLP-100", 0x18 },
{ 0x135c, 0x01c0, 0xffff, 0, "Quatech SSCLP-200/300", 0x18 },
{ 0x135e, 0x7101, 0xffff, 0, "Sealevel Systems Single Port RS-232/422/485/530",
	0x18 },
{ 0x1407, 0x0110, 0xffff, 0, "Lava Computer mfg DSerial-PCI Port A", 0x10 },
{ 0x1407, 0x0111, 0xffff, 0, "Lava Computer mfg DSerial-PCI Port B", 0x10 },
{ 0x1409, 0x7168, 0x1409, 0x4025, "Timedia Technology Serial Port", 0x10,
	8 * DEFAULT_RCLK },
{ 0x1409, 0x7168, 0x1409, 0x4027, "Timedia Technology Serial Port", 0x10,
	8 * DEFAULT_RCLK },
{ 0x1409, 0x7168, 0x1409, 0x4028, "Timedia Technology Serial Port", 0x10,
	8 * DEFAULT_RCLK },
{ 0x1409, 0x7168, 0x1409, 0x5025, "Timedia Technology Serial Port", 0x10,
	8 * DEFAULT_RCLK },
{ 0x1409, 0x7168, 0x1409, 0x5027, "Timedia Technology Serial Port", 0x10,
	8 * DEFAULT_RCLK },
{ 0x1415, 0x950b, 0xffff, 0, "Oxford Semiconductor OXCB950 Cardbus 16950 UART",
	0x10, 16384000 },
{ 0x151f, 0x0000, 0xffff, 0, "TOPIC Semiconductor TP560 56k modem", 0x10 },
{ 0x9710, 0x9835, 0x1000, 1, "NetMos NM9835 Serial Port", 0x10 },
{ 0xdeaf, 0x9051, 0xffff, 0, "Middle Digital PC Weasel Serial Port", 0x10 },
{ 0xffff, 0, 0xffff, 0, NULL, 0, 0}
};

static struct pci_id *
uart_pci_match(device_t dev, struct pci_id *id)
{
	uint16_t device, subdev, subven, vendor;

	vendor = pci_get_vendor(dev);
	device = pci_get_device(dev);
	while (id->vendor != 0xffff &&
	    (id->vendor != vendor || id->device != device))
		id++;
	if (id->vendor == 0xffff)
		return (NULL);
	if (id->subven == 0xffff)
		return (id);
	subven = pci_get_subvendor(dev);
	subdev = pci_get_subdevice(dev);
	while (id->vendor == vendor && id->device == device &&
	    (id->subven != subven || id->subdev != subdev))
		id++;
	return ((id->vendor == vendor && id->device == device) ? id : NULL);
}

static int
uart_pci_probe(device_t dev)
{
	struct uart_softc *sc;
	struct pci_id *id;

	sc = device_get_softc(dev);

	id = uart_pci_match(dev, pci_ns8250_ids);
	if (id != NULL) {
		sc->sc_class = &uart_ns8250_class;
		goto match;
	}
	/* Add checks for non-ns8250 IDs here. */
	return (ENXIO);

 match:
	if (id->desc)
		device_set_desc(dev, id->desc);
	return (uart_bus_probe(dev, 0, id->rclk, id->rid, 0));
}

DRIVER_MODULE(uart, pci, uart_pci_driver, uart_devclass, 0, 0);
DRIVER_MODULE(uart, cardbus, uart_pci_driver, uart_devclass, 0, 0);
