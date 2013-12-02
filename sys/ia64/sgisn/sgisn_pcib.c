/*-
 * Copyright (c) 2010-2013 Marcel Moolenaar
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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/bus.h>
#include <sys/busdma.h>
#include <sys/pcpu.h>
#include <sys/rman.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcib_private.h>

#include "busdma_if.h"
#include "pcib_if.h"

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/bus.h>
#include <machine/cpu.h>
#include <machine/pci_cfgreg.h>
#include <machine/resource.h>
#include <machine/sal.h>
#include <machine/sgisn.h>

#include <ia64/sgisn/sgisn_pcib.h>

#if PAGE_SIZE < 16384
#define	SGISN_PCIB_PAGE_SHIFT	12	/* 4KB */
#else
#define	SGISN_PCIB_PAGE_SHIFT	14	/* 16KB */
#endif
#define	SGISN_PCIB_PAGE_SIZE	(1UL << SGISN_PCIB_PAGE_SHIFT)
#define	SGISN_PCIB_PAGE_MASK	(SGISN_PCIB_PAGE_SIZE - 1UL)

static struct sgisn_fwdev sgisn_dev;
static struct sgisn_fwirq sgisn_irq;

struct sgisn_pcib_softc {
	device_t	sc_dev;
	struct sgisn_fwpcib *sc_fwbus;
	bus_addr_t	sc_ioaddr;
	bus_space_tag_t	sc_tag;
	bus_space_handle_t sc_hndl;
	u_int		sc_domain;
	u_int		sc_busnr;
	struct rman	sc_ioport;
	struct rman	sc_iomem;
	uint32_t	*sc_flush_intr[PCI_SLOTMAX + 1];
	uint64_t	*sc_flush_addr[PCI_SLOTMAX + 1];
	uint64_t	sc_ate[PCIB_REG_ATE_SIZE / 64];
	struct mtx	sc_ate_mtx;
};

static int sgisn_pcib_attach(device_t);
static int sgisn_pcib_probe(device_t);

static int sgisn_pcib_activate_resource(device_t, device_t, int, int,
    struct resource *);
static struct resource *sgisn_pcib_alloc_resource(device_t, device_t, int,
    int *, u_long, u_long, u_long, u_int);
static int sgisn_pcib_deactivate_resource(device_t, device_t, int, int,
    struct resource *);
static void sgisn_pcib_delete_resource(device_t, device_t, int, int);
static int sgisn_pcib_get_resource(device_t, device_t, int, int, u_long *,
    u_long *);
static struct resource_list *sgisn_pcib_get_resource_list(device_t, device_t);
static int sgisn_pcib_release_resource(device_t, device_t, int, int,
    struct resource *);
static int sgisn_pcib_set_resource(device_t, device_t, int, int, u_long,
    u_long);

static int sgisn_pcib_setup_intr(device_t, device_t, struct resource *, int,
    driver_filter_t *, driver_intr_t *, void *, void **);

static int sgisn_pcib_read_ivar(device_t, device_t, int, uintptr_t *);
static int sgisn_pcib_write_ivar(device_t, device_t, int, uintptr_t);

static int sgisn_pcib_maxslots(device_t);
static uint32_t sgisn_pcib_cfgread(device_t, u_int, u_int, u_int, u_int, int);
static void sgisn_pcib_cfgwrite(device_t, u_int, u_int, u_int, u_int, uint32_t,
    int);

static int sgisn_pcib_iommu_xlate(device_t, device_t, busdma_mtag_t);
static int sgisn_pcib_iommu_map(device_t, device_t, busdma_md_t, u_int,
    bus_addr_t *);
static int sgisn_pcib_iommu_unmap(device_t, device_t, busdma_md_t, u_int);
static int sgisn_pcib_iommu_sync(device_t, device_t, busdma_md_t, u_int,
    bus_addr_t, bus_size_t);

/*
 * Bus interface definitions.
 */
static device_method_t sgisn_pcib_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		sgisn_pcib_probe),
	DEVMETHOD(device_attach,	sgisn_pcib_attach),

	/* Bus interface */
	DEVMETHOD(bus_read_ivar,	sgisn_pcib_read_ivar),
	DEVMETHOD(bus_write_ivar,	sgisn_pcib_write_ivar),
	DEVMETHOD(bus_print_child,	bus_generic_print_child),
	DEVMETHOD(bus_activate_resource, sgisn_pcib_activate_resource),
	DEVMETHOD(bus_alloc_resource,	sgisn_pcib_alloc_resource),
	DEVMETHOD(bus_deactivate_resource, sgisn_pcib_deactivate_resource),
	DEVMETHOD(bus_delete_resource,	sgisn_pcib_delete_resource),
	DEVMETHOD(bus_get_resource,	sgisn_pcib_get_resource),
	DEVMETHOD(bus_get_resource_list, sgisn_pcib_get_resource_list),
	DEVMETHOD(bus_release_resource,	sgisn_pcib_release_resource),
	DEVMETHOD(bus_set_resource,	sgisn_pcib_set_resource),
	DEVMETHOD(bus_setup_intr,	sgisn_pcib_setup_intr),
	DEVMETHOD(bus_teardown_intr,	bus_generic_teardown_intr),

	/* pcib interface */
	DEVMETHOD(pcib_maxslots,	sgisn_pcib_maxslots),
	DEVMETHOD(pcib_read_config,	sgisn_pcib_cfgread),
	DEVMETHOD(pcib_write_config,	sgisn_pcib_cfgwrite),
	DEVMETHOD(pcib_route_interrupt,	pcib_route_interrupt),

	/* busdma interface */
	DEVMETHOD(busdma_iommu_xlate,	sgisn_pcib_iommu_xlate),
	DEVMETHOD(busdma_iommu_map,	sgisn_pcib_iommu_map),
	DEVMETHOD(busdma_iommu_unmap,	sgisn_pcib_iommu_unmap),
	DEVMETHOD(busdma_iommu_sync,	sgisn_pcib_iommu_sync),

	DEVMETHOD_END
};

static driver_t sgisn_pcib_driver = {
	"pcib",
	sgisn_pcib_methods,
	sizeof(struct sgisn_pcib_softc),
};

devclass_t pcib_devclass;

DRIVER_MODULE(pcib, shub, sgisn_pcib_driver, pcib_devclass, 0, 0);

static int
sgisn_pcib_maxslots(device_t dev)
{

	return (PCI_SLOTMAX);
}

static uint32_t
sgisn_pcib_cfgread(device_t dev, u_int bus, u_int slot, u_int func,
    u_int reg, int bytes)
{
	struct sgisn_pcib_softc *sc;
	uint32_t val;

	sc = device_get_softc(dev);

	val = pci_cfgregread((sc->sc_domain << 8) | bus, slot, func, reg,
	    bytes);
	return (val);
}

static void
sgisn_pcib_cfgwrite(device_t dev, u_int bus, u_int slot, u_int func,
    u_int reg, uint32_t val, int bytes)
{
	struct sgisn_pcib_softc *sc;

	sc = device_get_softc(dev);

	pci_cfgregwrite((sc->sc_domain << 8) | bus, slot, func, reg, val,
	    bytes);
}

static int
sgisn_pcib_activate_resource(device_t dev, device_t child, int type, int rid,
    struct resource *res)
{
	int error;

#ifdef SGISN_PCIB_DEBUG
	device_printf(dev, "%s(dev=%s, child=%s, type=%u, rid=%u, res=%p"
	    "[%#lx-%#lx])\n", __func__, device_get_nameunit(dev),
	    device_get_nameunit(child), type, rid, res, rman_get_start(res),
	    rman_get_end(res));
#endif

	error = rman_activate_resource(res);
	return (error);
}

static struct resource *
sgisn_pcib_alloc_resource(device_t dev, device_t child, int type, int *rid,
    u_long start, u_long end, u_long count, u_int flags)
{
	struct ia64_sal_result r;
	struct rman *rm;
	struct resource *rv;
	struct sgisn_pcib_softc *sc;
	device_t parent;
	void *vaddr;
	uint64_t base;
	uintptr_t func, slot;
	int bar, error;

#ifdef SGISN_PCIB_DEBUG
	device_printf(dev, "%s(dev=%s, child=%s, type=%u, rid=%u, "
	    "start=%#lx, end=%#lx, count=%#lx, flags=%x)\n", __func__,
	    device_get_nameunit(dev), device_get_nameunit(child), type,
	    *rid, start, end, count, flags);
#endif

	if (type == SYS_RES_IRQ)
		return (bus_generic_alloc_resource(dev, child, type, rid,
		    start, end, count, flags));

	bar = PCI_RID2BAR(*rid);
	if (bar < 0 || bar > PCIR_MAX_BAR_0)
		return (NULL);

	sc = device_get_softc(dev);
	rm = (type == SYS_RES_MEMORY) ? &sc->sc_iomem : &sc->sc_ioport;
	rv = rman_reserve_resource(rm, start, end, count, flags, child);
	if (rv == NULL)
		return (NULL);

	parent = device_get_parent(child);
	error = BUS_READ_IVAR(parent, child, PCI_IVAR_SLOT, &slot);
	if (error)
		goto fail;
	error = BUS_READ_IVAR(parent, child, PCI_IVAR_FUNCTION, &func);
	if (error)
		goto fail;

	r = ia64_sal_entry(SAL_SGISN_IODEV_INFO, sc->sc_domain, sc->sc_busnr,
	    (slot << 3) | func, ia64_tpa((uintptr_t)&sgisn_dev),
	    ia64_tpa((uintptr_t)&sgisn_irq), 0, 0);
	if (r.sal_status != 0)
		goto fail;

	base = sgisn_dev.dev_bar[bar] & 0x7fffffffffffffffL;
	if (base != start)
		device_printf(dev, "PCI bus address %#lx mapped to CPU "
		    "address %#lx\n", start, base);

#ifdef SGISN_PCIB_DEBUG
	device_printf(child, "nas=%#x, slice=%#x, cpuid=%#x, nr=%#x, "
	    "pin=%#x, xtaddr=%#lx, br_type=%#x, bridge=%p, dev=%p, "
	    "last=%#x, cookie=%#x, flags=%#x, refcnt=%#x\n",
	    sgisn_irq.irq_nasid, sgisn_irq.irq_slice, sgisn_irq.irq_cpuid,
	    sgisn_irq.irq_nr, sgisn_irq.irq_pin, sgisn_irq.irq_xtaddr,
	    sgisn_irq.irq_br_type, sgisn_irq.irq_bridge, sgisn_irq.irq_dev,
	    sgisn_irq.irq_last, sgisn_irq.irq_cookie, sgisn_irq.irq_flags,
	    sgisn_irq.irq_refcnt);
#endif

	/* I/O port space is presented as memory mapped I/O. */
	rman_set_bustag(rv, IA64_BUS_SPACE_MEM);
	vaddr = pmap_mapdev(base, count);
	rman_set_bushandle(rv, (bus_space_handle_t)vaddr);
	if (type == SYS_RES_MEMORY)
		rman_set_virtual(rv, vaddr);
	return (rv);

 fail:
	rman_release_resource(rv);
	return (NULL);
}

static int
sgisn_pcib_deactivate_resource(device_t dev, device_t child, int type, int rid,
    struct resource *res)
{
	int error;

#ifdef SGISN_PCIB_DEBUG
	device_printf(dev, "%s(dev=%s, child=%s, type=%u, rid=%u, res=%p"
	    "[%#lx-%#lx])\n", __func__, device_get_nameunit(dev),
	    device_get_nameunit(child), type, rid, res, rman_get_start(res),
	    rman_get_end(res));
#endif

	error = rman_deactivate_resource(res);
	return (error);
}

static void
sgisn_pcib_delete_resource(device_t dev, device_t child, int type, int rid)
{

#ifdef SGISN_PCIB_DEBUG
	device_printf(dev, "%s(dev=%s, child=%s, type=%u, rid=%u)\n",
	    __func__, device_get_nameunit(dev), device_get_nameunit(child),
	    type, rid);
#endif
}

static int
sgisn_pcib_get_resource(device_t dev, device_t child, int type, int rid,
    u_long *startp, u_long *countp)
{

#ifdef SGISN_PCIB_DEBUG
	device_printf(dev, "%s(dev=%s, child=%s, type=%u, rid=%u, "
	    "startp=%p, countp=%p)\n", __func__, device_get_nameunit(dev),
	    device_get_nameunit(child), type, rid, startp, countp);
#endif
	return (ENOENT);
}

static struct resource_list *
sgisn_pcib_get_resource_list(device_t dev, device_t child)
{

#ifdef SGISN_PCIB_DEBUG
	device_printf(dev, "%s(dev=%s, child=%s)\n", __func__,
	    device_get_nameunit(dev), device_get_nameunit(child));
#endif
	return (NULL);
}

static int
sgisn_pcib_release_resource(device_t dev, device_t child, int type, int rid,
    struct resource *res)
{
	int error;

#ifdef SGISN_PCIB_DEBUG
	device_printf(dev, "%s(dev=%s, child=%s, type=%u, rid=%u, res=%p"
	    "[%#lx-%#lx])\n", __func__, device_get_nameunit(dev),
	    device_get_nameunit(child), type, rid, res, rman_get_start(res),
	    rman_get_end(res));
#endif

	if (rman_get_flags(res) & RF_ACTIVE) {
		error = rman_deactivate_resource(res);
		if (error)
			return (error);
	}
	error = rman_release_resource(res);
	return (error);
}

static int
sgisn_pcib_set_resource(device_t dev, device_t child, int type, int rid,
    u_long start, u_long count)
{

#ifdef SGISN_PCIB_DEBUG
	device_printf(dev, "%s(dev=%s, child=%s, type=%u, rid=%u, "
	    "start=%#lx, count=%#lx)\n", __func__, device_get_nameunit(dev),
	    device_get_nameunit(child), type, rid, start, count);
#endif

	return (ENXIO);
}

static int
sgisn_pcib_setup_intr(device_t dev, device_t child, struct resource *irq,
    int flags, driver_filter_t *ifltr, driver_intr_t *ihdlr, void *arg,
    void **cookiep)
{
	struct sgisn_pcib_softc *sc;
	uint64_t ie;
	int error;

#ifdef SGISN_PCIB_DEBUG
	device_printf(dev, "%s(dev=%s, child=%s, irq=%lu, flags=%#x, "
	    "ifltr=%p, ihdlr=%p, arg=%p, cookiep=%p)\n", __func__,
	    device_get_nameunit(dev), device_get_nameunit(child),
	    rman_get_start(irq), flags, ifltr, ihdlr, arg, cookiep);
#endif

	sc = device_get_softc(dev);
	ie = bus_space_read_8(sc->sc_tag, sc->sc_hndl, PCIB_REG_INT_ENABLE);

#ifdef SGISN_PCIB_DEBUG
	device_printf(dev, "INT_ENABLE=%#lx\n", ie);
#endif

	error = bus_generic_setup_intr(dev, child, irq, flags, ifltr, ihdlr,
	    arg, cookiep);
	return (error);
}

static int
sgisn_pcib_probe(device_t dev)
{
	struct ia64_sal_result r;
	struct sgisn_fwpcib *fwbus;
	device_t parent;
	uintptr_t bus, seg;
	u_long addr;
	int res;

	parent = device_get_parent(dev);
	if (parent == NULL)
		return (ENXIO);

	if (BUS_READ_IVAR(parent, dev, SHUB_IVAR_PCISEG, &seg) ||
	    BUS_READ_IVAR(parent, dev, SHUB_IVAR_PCIBUS, &bus))
		return (ENXIO);

	r = ia64_sal_entry(SAL_SGISN_IOBUS_INFO, seg, bus,
	    ia64_tpa((uintptr_t)&addr), 0, 0, 0, 0);
	if (r.sal_status != 0 || addr == 0)
		return (ENXIO);
	fwbus = (void *)IA64_PHYS_TO_RR7(addr);
	switch (fwbus->fw_common.bus_asic) {
	case SGISN_PCIB_PIC:
	case SGISN_PCIB_TIOCP:
		device_set_desc(dev, "SGI PCI/PCI-X host controller");
		res = BUS_PROBE_DEFAULT;
		break;
	default:
		res = ENXIO;
		break;
	}
	return (res);
}

static int
sgisn_pcib_rm_init(struct sgisn_pcib_softc *sc, struct rman *rm,
    const char *what)
{
	char descr[128];
	int error;

	rm->rm_start = 0UL;
	rm->rm_end = 0x3ffffffffUL;		/* 16GB */
	rm->rm_type = RMAN_ARRAY;
	error = rman_init(rm);
	if (error)
		return (error);

	snprintf(descr, sizeof(descr), "PCI %u:%u local I/O %s addresses",
	    sc->sc_domain, sc->sc_busnr, what);
	rm->rm_descr = strdup(descr, M_DEVBUF);

	error = rman_manage_region(rm, rm->rm_start, rm->rm_end);
	if (error)
		rman_fini(rm);

	return (error);
}

static void
sgisn_pcib_setup_flush(struct sgisn_pcib_softc *sc)
{
	struct ia64_sal_result r;
	struct sgisn_fwflush *fwflush;
	device_t parent;
	uintptr_t nasid;
	size_t fwflushsz;
	u_int i, slot;

	fwflushsz = (PCI_SLOTMAX + 1) * sizeof(struct sgisn_fwflush);
	fwflush = contigmalloc(fwflushsz, M_TEMP, M_ZERO, 0UL, ~0UL, 16, 0);
	parent = device_get_parent(sc->sc_dev);
	BUS_READ_IVAR(parent, sc->sc_dev, SHUB_IVAR_NASID, &nasid);
	r = ia64_sal_entry(SAL_SGISN_IOBUS_FLUSH, nasid,
	    sc->sc_fwbus->fw_common.bus_xid, ia64_tpa((uintptr_t)fwflush),
	    0, 0, 0, 0);
	if (r.sal_status == 0) {
		for (i = 0; i <= PCI_SLOTMAX; i++) {
			if (fwflush[i].fld_pci_segment != sc->sc_domain ||
			    fwflush[i].fld_pci_bus != sc->sc_busnr)
				continue;
			slot = fwflush[i].fld_slot;
			if (slot > PCI_SLOTMAX)
				continue;
			sc->sc_flush_intr[slot] = fwflush[i].fld_intr;
			sc->sc_flush_addr[slot] = fwflush[i].fld_addr;
			device_printf(sc->sc_dev, "slot=%d: flush addr=%p, "
			    "intr=%p\n", slot, fwflush[i].fld_addr,
			    fwflush[i].fld_intr);
		}
	}
	contigfree(fwflush, fwflushsz, M_TEMP);
}

static int
sgisn_pcib_attach(device_t dev)
{
	struct sgisn_pcib_softc *sc;
	device_t parent;
	uintptr_t addr, ivar;
	uint64_t ctrl;
	int error;

	sc = device_get_softc(dev);
	sc->sc_dev = dev;

	parent = device_get_parent(dev);
	BUS_READ_IVAR(parent, dev, SHUB_IVAR_PCIBUS, &ivar);
	sc->sc_busnr = ivar;
	BUS_READ_IVAR(parent, dev, SHUB_IVAR_PCISEG, &ivar);
	sc->sc_domain = ivar;

	error = sgisn_pcib_rm_init(sc, &sc->sc_ioport, "port");
	if (error)
		return (error);
	error = sgisn_pcib_rm_init(sc, &sc->sc_iomem, "memory");
	if (error) {
		rman_fini(&sc->sc_ioport);
		return (error);
	}

	(void)ia64_sal_entry(SAL_SGISN_IOBUS_INFO, sc->sc_domain, sc->sc_busnr,
	    ia64_tpa((uintptr_t)&addr), 0, 0, 0, 0);
	sc->sc_fwbus = (void *)IA64_PHYS_TO_RR7(addr);
	sc->sc_ioaddr = IA64_RR_MASK(sc->sc_fwbus->fw_common.bus_base);
	sc->sc_tag = IA64_BUS_SPACE_MEM;
	bus_space_map(sc->sc_tag, sc->sc_ioaddr, PCIB_REG_SIZE, 0,
	    &sc->sc_hndl);

	if (bootverbose)
		device_printf(dev, "ASIC=%x, XID=%u, TYPE=%u, MODE=%u\n",
		    sc->sc_fwbus->fw_common.bus_asic,
		    sc->sc_fwbus->fw_common.bus_xid,
		    sc->sc_fwbus->fw_type, sc->sc_fwbus->fw_mode);

	/* Set the preferred I/O MMU page size -- 4KB or 16KB. */
	ctrl = bus_space_read_8(sc->sc_tag, sc->sc_hndl, PCIB_REG_WGT_CTRL);
#if SGISN_PCIB_PAGE_SHIFT == 12
	ctrl &= ~(1UL << 21);
#else
	ctrl |= 1UL << 21;
#endif
	bus_space_write_8(sc->sc_tag, sc->sc_hndl, PCIB_REG_WGT_CTRL, ctrl);

	mtx_init(&sc->sc_ate_mtx, device_get_nameunit(dev), NULL, MTX_SPIN);

	if (sc->sc_fwbus->fw_common.bus_asic == SGISN_PCIB_PIC)
		sgisn_pcib_setup_flush(sc);

	device_add_child(dev, "pci", -1);
	error = bus_generic_attach(dev);
	return (error);
}

static int
sgisn_pcib_read_ivar(device_t dev, device_t child, int which, uintptr_t *res)
{
	struct sgisn_pcib_softc *sc = device_get_softc(dev);

	switch (which) {
	case PCIB_IVAR_BUS:
		*res = sc->sc_busnr;
		return (0);
	case PCIB_IVAR_DOMAIN:
		*res = sc->sc_domain;
		return (0);
	}
	return (ENOENT);
}

static int
sgisn_pcib_write_ivar(device_t dev, device_t child, int which, uintptr_t value)
{
	struct sgisn_pcib_softc *sc = device_get_softc(dev);

	switch (which) {
	case PCIB_IVAR_BUS:
		sc->sc_busnr = value;
		return (0);
	}
	return (ENOENT);
}

static int
sgisn_pcib_iommu_xlate(device_t bus, device_t dev, busdma_mtag_t mtag)
{
	struct sgisn_pcib_softc *sc = device_get_softc(bus);
	vm_paddr_t bndry = 0x80000000UL;

	/*
	 * 64-bit consistent DMA is only guaranteed for PCI-X. So, if we
	 * need consistent DMA and we're not in PCI-X mode, force 32-bit
	 * mappings.
	 */
	if ((mtag->dmt_flags & BUSDMA_ALLOC_COHERENT) &&
	    (sc->sc_fwbus->fw_mode & 1) == 0 &&
	    mtag->dmt_maxaddr == BUS_SPACE_MAXADDR)
		mtag->dmt_maxaddr >>= 4;

	/*
	 * Use a 31-bit direct-mapped window for PCI devices that are not
	 * 64-bit capable and we're not in PCI-X mode and we don't need
	 * consistent DMA.
	 * For 31-bit direct-mapped DMA we need to make sure allocations
	 * do not cross the 2G boundary.
	 */
	if (mtag->dmt_maxaddr < BUS_SPACE_MAXADDR &&
	    (sc->sc_fwbus->fw_mode & 1) == 0 &&
	    (mtag->dmt_flags & BUSDMA_ALLOC_COHERENT) == 0)
		mtag->dmt_flags |= BUSDMA_MD_IA64_DIRECT32;

	if (mtag->dmt_flags & BUSDMA_MD_IA64_DIRECT32) {
		mtag->dmt_maxaddr &= (bndry - 1);
		if (mtag->dmt_bndry == 0 || mtag->dmt_bndry > bndry)
			mtag->dmt_bndry = bndry;
	}
	return (0);
}

static int
sgisn_pcib_iommu_map(device_t bus, device_t dev, busdma_md_t md, u_int idx,
    bus_addr_t *ba_p)
{
	struct sgisn_pcib_softc *sc = device_get_softc(bus);
	busdma_tag_t tag;
	bus_addr_t maxaddr = 0x80000000UL;
	bus_addr_t addr, ba, size;
	uint64_t bits;
	u_int ate, bitshft, count, entry, flags;

	ba = *ba_p;

	flags = busdma_md_get_flags(md);
	if ((flags & BUSDMA_MD_IA64_DIRECT32) && ba < maxaddr) {
		addr = ba | maxaddr;
		*ba_p = addr;
		return (0);
	}

	tag = busdma_md_get_tag(md);
	maxaddr = busdma_tag_get_maxaddr(tag);
	if (maxaddr == BUS_SPACE_MAXADDR) {
		addr = ba;
		if (flags & BUSDMA_ALLOC_COHERENT)
			addr |= 1UL << 56;	/* bar */
		else if ((sc->sc_fwbus->fw_mode & 1) == 0)
			addr |= 1UL << 59;	/* prefetch */
		if (sc->sc_fwbus->fw_common.bus_asic == SGISN_PCIB_PIC)
			addr |= (u_long)sc->sc_fwbus->fw_hub_xid << 60;
		else
			addr |= 1UL << 60;	/* memory */
		*ba_p = addr;
		return (0);
	}

	/*
	 * 32-bit mapped DMA.
	 */
	size = busdma_md_get_size(md, idx);
	count = ((ba & SGISN_PCIB_PAGE_MASK) + size +
	    SGISN_PCIB_PAGE_MASK) >> SGISN_PCIB_PAGE_SHIFT;

	/* Our bitmap allocation routine doesn't handle straddling longs. */
	if (count > 64) {
		device_printf(sc->sc_dev, "IOMMU: more than 64 entries needed "
		    "for DMA\n");
		return (E2BIG);
	}

	mtx_lock_spin(&sc->sc_ate_mtx);

	ate = 0;
	entry = ~0;
	bitshft = 0;
	while (ate < (PCIB_REG_ATE_SIZE / 64) && entry == ~0) {
		bits = sc->sc_ate[ate];
		/* Move to the next long if this one is full. */
		if (bits == ~0UL) {
			ate++;
			continue;
		}
		/* If this long is empty, take it (catches count == 64). */
		if (bits == 0UL) {
			entry = ate * 64;
			break;
		}
		/* Avoid undefined behaviour below for 1 << count. */
		if (count == 64) {
			ate++;
			continue;
		}
		do {
			if ((bits & ((1UL << count) - 1UL)) == 0) {
				entry = ate * 64 + bitshft;
				break;
			}
			while ((bits & 1UL) == 0) {
				bits >>= 1;
				bitshft++;
			}
			while (bitshft <= (64 - count) && (bits & 1UL) != 0) {
				bits >>= 1;
				bitshft++;
			}
		} while (bitshft <= (64 - count));
		if (entry == ~0) {
			ate++;
			bitshft = 0;
		}
	}
	if (entry != ~0) {
		KASSERT(ate < (PCIB_REG_ATE_SIZE / 64), ("foo: ate"));
		KASSERT(bitshft <= (64 - count), ("foo: bitshft"));
		KASSERT(entry == (ate * 64 + bitshft), ("foo: math"));
		bits = (count < 64) ? ((1UL << count) - 1UL) << bitshft : ~0UL;
		KASSERT((sc->sc_ate[ate] & bits) == 0UL, ("foo: bits"));
		sc->sc_ate[ate] |= bits;
	}

	mtx_unlock_spin(&sc->sc_ate_mtx);

	if (entry == ~0) {
		device_printf(sc->sc_dev, "IOMMU: cannot find %u free entries "
		    "for DMA\n", count);
		return (ENOSPC);
	}

	*ba_p = (1UL << 30) | (ba & SGISN_PCIB_PAGE_MASK) |
	    (SGISN_PCIB_PAGE_SIZE * entry);

	ba &= ~SGISN_PCIB_PAGE_MASK;
	ba |= 1 << 0;		/* valid */
	if (flags & BUSDMA_ALLOC_COHERENT)
		ba |= 1 << 4;	/* bar */
	else if ((sc->sc_fwbus->fw_mode & 1) == 0)
		ba |= 1 << 3;	/* prefetch */
	if (sc->sc_fwbus->fw_common.bus_asic == SGISN_PCIB_PIC)
		ba |= (u_long)sc->sc_fwbus->fw_hub_xid << 8;
	while (count > 0) {
		bus_space_write_8(sc->sc_tag, sc->sc_hndl,
		    PCIB_REG_ATE(entry), ba);
		ba += SGISN_PCIB_PAGE_SIZE;
		entry++;
		count--;
	}
	return (0);
}

static int
sgisn_pcib_iommu_unmap(device_t bus, device_t dev, busdma_md_t md, u_int idx)
{
	struct sgisn_pcib_softc *sc = device_get_softc(bus);
	bus_addr_t ba, size;
	uint64_t bits;
	u_int ate, bitshft, count, entry;

	ba = busdma_md_get_busaddr(md, idx);
	if ((ba >> 30) != 1)
		return (0);

	/*
	 * 32-bit mapped DMA
	 */
	size = busdma_md_get_size(md, idx);
	count = ((ba & SGISN_PCIB_PAGE_MASK) + size +
	    SGISN_PCIB_PAGE_MASK) >> SGISN_PCIB_PAGE_SHIFT;
	ba &= (1 << 29) - 1;
	entry = (ba >> SGISN_PCIB_PAGE_SHIFT);

	KASSERT(count <= 64, ("foo: count"));
	KASSERT((entry + count) <= PCIB_REG_ATE_SIZE, ("foo"));
	bitshft = entry % 64;
	KASSERT(bitshft <= (64 - count), ("foo: bitshft"));
	bits = (count < 64) ? ((1UL << count) - 1UL) << bitshft : ~0UL;
	ate = entry / 64;

	while (count > 0) {
		bus_space_write_8(sc->sc_tag, sc->sc_hndl,
		    PCIB_REG_ATE(entry), 0);
		entry++;
		count--;
	}

	mtx_lock_spin(&sc->sc_ate_mtx);

	sc->sc_ate[ate] &= ~bits;

	mtx_unlock_spin(&sc->sc_ate_mtx);

	return (0);
}

static int
sgisn_pcib_iommu_sync(device_t bus, device_t dev, busdma_md_t md, u_int op,
    bus_addr_t addr, bus_size_t size)
{
	struct sgisn_pcib_softc *sc = device_get_softc(bus);
	volatile uint64_t *fladdr;
	volatile uint32_t *flintr;
	uintptr_t slot;
	int error;

	if ((op & BUSDMA_SYNC_POSTREAD) != BUSDMA_SYNC_POSTREAD)
		return (0);

	error = BUS_READ_IVAR(device_get_parent(dev), dev, PCI_IVAR_SLOT,
	    &slot);
	if (error)
		return (error);

	fladdr = sc->sc_flush_addr[slot];
	flintr = sc->sc_flush_intr[slot];
	if (fladdr != NULL && flintr != NULL) {
		*fladdr = 0;
		*flintr = 1;
		while (*fladdr != 0x10f)
			cpu_spinwait();
	}
	return (0);
}
