/*-
 * Copyright (c) 1997, Stefan Esser <se@freebsd.org>
 * Copyright (c) 2000, Michael Smith <msmith@freebsd.org>
 * Copyright (c) 2000, BSDi
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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <machine/pci_cfgreg.h>

enum {
	CFGMECH_NONE = 0,
	CFGMECH_1,
	CFGMECH_PCIE,
};

static int	pciereg_cfgread(int bus, unsigned slot, unsigned func,
		    unsigned reg, unsigned bytes);
static void	pciereg_cfgwrite(int bus, unsigned slot, unsigned func,
		    unsigned reg, int data, unsigned bytes);
static int	pcireg_cfgread(int bus, int slot, int func, int reg, int bytes);
static void	pcireg_cfgwrite(int bus, int slot, int func, int reg, int data, int bytes);

static int cfgmech;
static vm_offset_t pcie_base;
static int pcie_minbus, pcie_maxbus;
static struct mtx pcicfg_mtx;

/* 
 * Initialise access to PCI configuration space 
 */
int
pci_cfgregopen(void)
{
	uint64_t pciebar;
	uint16_t did, vid;

	if (cfgmech != CFGMECH_NONE)
		return (1);
	mtx_init(&pcicfg_mtx, "pcicfg", NULL, MTX_SPIN);
	cfgmech = CFGMECH_1;

	/*
	 * Grope around in the PCI config space to see if this is a
	 * chipset that is capable of doing memory-mapped config cycles.
	 * This also implies that it can do PCIe extended config cycles.
	 */

	/* Check for supported chipsets */
	vid = pci_cfgregread(0, 0, 0, PCIR_VENDOR, 2);
	did = pci_cfgregread(0, 0, 0, PCIR_DEVICE, 2);
	switch (vid) {
	case 0x8086:
		switch (did) {
		case 0x3590:
		case 0x3592:
			/* Intel 7520 or 7320 */
			pciebar = pci_cfgregread(0, 0, 0, 0xce, 2) << 16;
			pcie_cfgregopen(pciebar, 0, 255);
			break;
		case 0x2580:
		case 0x2584:
		case 0x2590:
			/* Intel 915, 925, or 915GM */
			pciebar = pci_cfgregread(0, 0, 0, 0x48, 4);
			pcie_cfgregopen(pciebar, 0, 255);
			break;
		}
	}

	return (1);
}

/* 
 * Read configuration space register
 */
u_int32_t
pci_cfgregread(int bus, int slot, int func, int reg, int bytes)
{
	uint32_t line;

	/*
	 * Some BIOS writers seem to want to ignore the spec and put
	 * 0 in the intline rather than 255 to indicate none.  Some use
	 * numbers in the range 128-254 to indicate something strange and
	 * apparently undocumented anywhere.  Assume these are completely bogus
	 * and map them to 255, which the rest of the PCI code recognizes as
	 * as an invalid IRQ.
	 */
	if (reg == PCIR_INTLINE && bytes == 1) {
		line = pcireg_cfgread(bus, slot, func, PCIR_INTLINE, 1);
		if (line == 0 || line >= 128)
			line = PCI_INVALID_IRQ;
		return (line);
	}
	return (pcireg_cfgread(bus, slot, func, reg, bytes));
}

/* 
 * Write configuration space register 
 */
void
pci_cfgregwrite(int bus, int slot, int func, int reg, u_int32_t data, int bytes)
{

	pcireg_cfgwrite(bus, slot, func, reg, data, bytes);
}

/* 
 * Configuration space access using direct register operations
 */

/* enable configuration space accesses and return data port address */
static int
pci_cfgenable(unsigned bus, unsigned slot, unsigned func, int reg, int bytes)
{
	int dataport = 0;

	if (bus <= PCI_BUSMAX && slot < 32 && func <= PCI_FUNCMAX &&
	    reg <= PCI_REGMAX && bytes != 3 && (unsigned) bytes <= 4 &&
	    (reg & (bytes - 1)) == 0) {
		outl(CONF1_ADDR_PORT, (1 << 31) | (bus << 16) | (slot << 11) 
		    | (func << 8) | (reg & ~0x03));
		dataport = CONF1_DATA_PORT + (reg & 0x03);
	}
	return (dataport);
}

/* disable configuration space accesses */
static void
pci_cfgdisable(void)
{

	/*
	 * Do nothing.  Writing a 0 to the address port can apparently
	 * confuse some bridges and cause spurious access failures.
	 */
}

static int
pcireg_cfgread(int bus, int slot, int func, int reg, int bytes)
{
	int data = -1;
	int port;

	if (cfgmech == CFGMECH_PCIE) {
		data = pciereg_cfgread(bus, slot, func, reg, bytes);
		return (data);
	}

	mtx_lock_spin(&pcicfg_mtx);
	port = pci_cfgenable(bus, slot, func, reg, bytes);
	if (port != 0) {
		switch (bytes) {
		case 1:
			data = inb(port);
			break;
		case 2:
			data = inw(port);
			break;
		case 4:
			data = inl(port);
			break;
		}
		pci_cfgdisable();
	}
	mtx_unlock_spin(&pcicfg_mtx);
	return (data);
}

static void
pcireg_cfgwrite(int bus, int slot, int func, int reg, int data, int bytes)
{
	int port;

	if (cfgmech == CFGMECH_PCIE) {
		pciereg_cfgwrite(bus, slot, func, reg, data, bytes);
		return;
	}

	mtx_lock_spin(&pcicfg_mtx);
	port = pci_cfgenable(bus, slot, func, reg, bytes);
	if (port != 0) {
		switch (bytes) {
		case 1:
			outb(port, data);
			break;
		case 2:
			outw(port, data);
			break;
		case 4:
			outl(port, data);
			break;
		}
		pci_cfgdisable();
	}
	mtx_unlock_spin(&pcicfg_mtx);
}

int
pcie_cfgregopen(uint64_t base, uint8_t minbus, uint8_t maxbus)
{

	if (minbus != 0)
		return (0);

	if (bootverbose)
		printf("PCIe: Memory Mapped configuration base @ 0x%lx\n",
		    base);

	/* XXX: We should make sure this really fits into the direct map. */
	pcie_base = (vm_offset_t)pmap_mapdev(base, (maxbus + 1) << 20);
	pcie_minbus = minbus;
	pcie_maxbus = maxbus;
	cfgmech = CFGMECH_PCIE;
	return (1);
}

#define PCIE_VADDR(base, reg, bus, slot, func)	\
	((base)				+	\
	((((bus) & 0xff) << 20)		|	\
	(((slot) & 0x1f) << 15)		|	\
	(((func) & 0x7) << 12)		|	\
	((reg) & 0xfff)))

static int
pciereg_cfgread(int bus, unsigned slot, unsigned func, unsigned reg,
    unsigned bytes)
{
	volatile vm_offset_t va;
	int data = -1;

	if (bus < pcie_minbus || bus > pcie_maxbus || slot >= 32 ||
	    func > PCI_FUNCMAX || reg >= 0x1000)
		return (-1);

	va = PCIE_VADDR(pcie_base, reg, bus, slot, func);

	switch (bytes) {
	case 4:
		data = *(volatile uint32_t *)(va);
		break;
	case 2:
		data = *(volatile uint16_t *)(va);
		break;
	case 1:
		data = *(volatile uint8_t *)(va);
		break;
	}

	return (data);
}

static void
pciereg_cfgwrite(int bus, unsigned slot, unsigned func, unsigned reg, int data,
    unsigned bytes)
{
	volatile vm_offset_t va;

	if (bus < pcie_minbus || bus > pcie_maxbus || slot >= 32 ||
	    func > PCI_FUNCMAX || reg >= 0x1000)
		return;

	va = PCIE_VADDR(pcie_base, reg, bus, slot, func);

	switch (bytes) {
	case 4:
		*(volatile uint32_t *)(va) = data;
		break;
	case 2:
		*(volatile uint16_t *)(va) = data;
		break;
	case 1:
		*(volatile uint8_t *)(va) = data;
		break;
	}
}
