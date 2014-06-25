/*-
 *
 * Copyright (c) 1991 Regents of the University of California.
 * All rights reserved.
 * Copyright (c) 1994 John S. Dyson
 * All rights reserved.
 * Copyright (c) 1994 David Greenman
 * All rights reserved.
 * Copyright (c) 2005 Alan L. Cox <alc@cs.rice.edu>
 * All rights reserved.
 * Copyright (c) 2012 Citrix Systems
 * All rights reserved.
 * Copyright (c) 2012, 2013 Spectra Logic Corporation
 * All rights reserved.
 * 
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department and William Jolitz of UUNET Technologies Inc.
 *
 * Portions of this software were developed by
 * Cherry G. Mathew <cherry.g.mathew@gmail.com> under sponsorship
 * from Spectra Logic Corporation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	from:	@(#)pmap.c	7.7 (Berkeley)	5/12/91
 */
/*-
 * Copyright (c) 2003 Networks Associates Technology, Inc.
 * All rights reserved.
 *
 * This software was developed for the FreeBSD Project by Jake Burkholder,
 * Safeport Network Services, and Network Associates Laboratories, the
 * Security Research Division of Network Associates, Inc. under
 * DARPA/SPAWAR contract N66001-01-C-8035 ("CBOSS"), as part of the DARPA
 * CHATS research program.
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


#define	AMD64_NPT_AWARE

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 *	Manages physical address maps.
 *
 *	In addition to hardware address maps, this
 *	module is called upon to provide software-use-only
 *	maps which may or may not be stored in the same
 *	form as hardware maps.  These pseudo-maps are
 *	used to store intermediate results from copy
 *	operations to and from address spaces.
 *
 *	Since the information managed by this module is
 *	also stored by the logical address mapping module,
 *	this module may throw away valid virtual-to-physical
 *	mappings at almost any time.  However, invalidations
 *	of virtual-to-physical mappings must be done as
 *	requested.
 *
 *	In order to cope with hardware architectures which
 *	make virtual-to-physical map invalidates expensive,
 *	this module may delay invalidate or reduced protection
 *	operations until such time as they are actually
 *	necessary.  This module is given full information as
 *	to which processors are currently using which maps,
 *	and to when physical maps must be made correct.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_cpu.h"
#include "opt_pmap.h"
#include "opt_smp.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cpuset.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/lock.h>
#include <sys/mman.h>
#include <sys/rwlock.h>
#include <sys/msgbuf.h>
#include <sys/mutex.h>

#ifdef SMP
#include <sys/smp.h>
#endif

#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/sysctl.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/vm_kern.h>
#include <vm/vm_page.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_extern.h>
#include <vm/vm_pageout.h>
#include <vm/vm_pager.h>
#include <vm/vm_radix.h>
#include <vm/vm_reserv.h>
#include <vm/uma.h>

#include <machine/md_var.h>
#include <machine/pcb.h>

#include <xen/hypervisor.h>
#include <machine/xen/xenvar.h>

#include <amd64/xen/mmu_map.h>
#include <amd64/xen/pmap_pv.h>

static __inline boolean_t
pmap_emulate_ad_bits(pmap_t pmap)
{

	return ((pmap->pm_flags & PMAP_EMULATE_AD_BITS) != 0);
}

static __inline pt_entry_t
pmap_valid_bit(pmap_t pmap)
{
	pt_entry_t mask;

	switch (pmap->pm_type) {
	case PT_X86:
		mask = X86_PG_V;
		break;
	case PT_EPT:
		if (pmap_emulate_ad_bits(pmap))
			mask = EPT_PG_EMUL_V;
		else
			mask = EPT_PG_READ;
		break;
	default:
		panic("pmap_valid_bit: invalid pm_type %d", pmap->pm_type);
	}

	return (mask);
}

static __inline pt_entry_t
pmap_rw_bit(pmap_t pmap)
{
	pt_entry_t mask;

	switch (pmap->pm_type) {
	case PT_X86:
		mask = X86_PG_RW;
		break;
	case PT_EPT:
		if (pmap_emulate_ad_bits(pmap))
			mask = EPT_PG_EMUL_RW;
		else
			mask = EPT_PG_WRITE;
		break;
	default:
		panic("pmap_rw_bit: invalid pm_type %d", pmap->pm_type);
	}

	return (mask);
}

static __inline pt_entry_t
pmap_global_bit(pmap_t pmap)
{
	pt_entry_t mask;

	switch (pmap->pm_type) {
	case PT_X86:
		mask = X86_PG_G;
		break;
	case PT_EPT:
		mask = 0;
		break;
	default:
		panic("pmap_global_bit: invalid pm_type %d", pmap->pm_type);
	}

	return (mask);
}

static __inline pt_entry_t
pmap_accessed_bit(pmap_t pmap)
{
	pt_entry_t mask;

	switch (pmap->pm_type) {
	case PT_X86:
		mask = X86_PG_A;
		break;
	case PT_EPT:
		if (pmap_emulate_ad_bits(pmap))
			mask = EPT_PG_READ;
		else
			mask = EPT_PG_A;
		break;
	default:
		panic("pmap_accessed_bit: invalid pm_type %d", pmap->pm_type);
	}

	return (mask);
}

static __inline pt_entry_t
pmap_modified_bit(pmap_t pmap)
{
	pt_entry_t mask;

	switch (pmap->pm_type) {
	case PT_X86:
		mask = X86_PG_M;
		break;
	case PT_EPT:
		if (pmap_emulate_ad_bits(pmap))
			mask = EPT_PG_WRITE;
		else
			mask = EPT_PG_M;
		break;
	default:
		panic("pmap_modified_bit: invalid pm_type %d", pmap->pm_type);
	}

	return (mask);
}

#if !defined(DIAGNOSTIC)
#ifdef __GNUC_GNU_INLINE__
#define PMAP_INLINE	__attribute__((__gnu_inline__)) inline
#else
#define PMAP_INLINE	extern inline
#endif
#else
#define PMAP_INLINE
#endif

#ifdef PV_STATS
#define PV_STAT(x)	do { x ; } while (0)
#else
#define PV_STAT(x)	do { } while (0)
#endif

#define	pa_index(pa)	((pa) >> PDRSHIFT)
#define	pa_to_pvh(pa)	(&pv_table[pa_index(pa)])

#define	NPV_LIST_LOCKS	MAXCPU

#define	PHYS_TO_PV_LIST_LOCK(pa)	\
			(&pv_list_locks[pa_index(pa) % NPV_LIST_LOCKS])

#define	CHANGE_PV_LIST_LOCK_TO_PHYS(lockp, pa)	do {	\
	struct rwlock **_lockp = (lockp);		\
	struct rwlock *_new_lock;			\
							\
	_new_lock = PHYS_TO_PV_LIST_LOCK(pa);		\
	if (_new_lock != *_lockp) {			\
		if (*_lockp != NULL)			\
			rw_wunlock(*_lockp);		\
		*_lockp = _new_lock;			\
		rw_wlock(*_lockp);			\
	}						\
} while (0)

#define	CHANGE_PV_LIST_LOCK_TO_VM_PAGE(lockp, m)	\
			CHANGE_PV_LIST_LOCK_TO_PHYS(lockp, VM_PAGE_TO_PHYS(m))

#define	RELEASE_PV_LIST_LOCK(lockp)		do {	\
	struct rwlock **_lockp = (lockp);		\
							\
	if (*_lockp != NULL) {				\
		rw_wunlock(*_lockp);			\
		*_lockp = NULL;				\
	}						\
} while (0)

#define	VM_PAGE_TO_PV_LIST_LOCK(m)	\
			PHYS_TO_PV_LIST_LOCK(VM_PAGE_TO_PHYS(m))

extern vm_offset_t pa_index; /* from machdep.c */
extern unsigned long physfree; /* from machdep.c */

struct pmap kernel_pmap_store;

#define ISDMAPVA(va) ((va) >= DMAP_MIN_ADDRESS && \
		      (va) <= DMAP_MAX_ADDRESS)
#define ISKERNELVA(va) ((va) >= VM_MIN_KERNEL_ADDRESS && \
			(va) <= VM_MAX_KERNEL_ADDRESS)
#define ISBOOTVA(va) ((va) >= KERNBASE && (va) <= (xenstack + 512 * 1024))

uintptr_t virtual_avail;	/* VA of first avail page (after kernel bss) */
uintptr_t virtual_end;	/* VA of last avail page (end of kernel AS) */

int nkpt;

static int ndmpdp;
vm_paddr_t dmaplimit;
uintptr_t kernel_vm_end = VM_MIN_KERNEL_ADDRESS;
pt_entry_t pg_nx = 0; /* XXX: Correctly handle this for Xen PV */

struct msgbuf *msgbufp = 0;

static int pg_ps_enabled = 1;
static SYSCTL_NODE(_vm, OID_AUTO, pmap, CTLFLAG_RD, 0, "VM/pmap parameters");

SYSCTL_INT(_vm_pmap, OID_AUTO, pg_ps_enabled, CTLFLAG_RDTUN, &pg_ps_enabled, 0,
    "Are large page mappings enabled?");

static u_int64_t	KPTphys;	/* phys addr of kernel level 1 */
static u_int64_t	KPDphys;	/* phys addr of kernel level 2 */
u_int64_t		KPDPphys;	/* phys addr of kernel level 3 */
u_int64_t		KPML4phys;	/* phys addr of kernel level 4 */

static u_int64_t	DMPTphys;	/* phys addr of direct mapped level 1 */
static u_int64_t	DMPDphys;	/* phys addr of direct mapped level 2 */
static u_int64_t	DMPDPphys;	/* phys addr of direct mapped level 3 */

static int		ndmpdpphys;	/* number of DMPDPphys pages */

static struct rwlock_padalign pvh_global_lock;

/*
 * Data for the pv entry allocation mechanism
 */
TAILQ_HEAD(pch, pv_chunk) pv_chunks = TAILQ_HEAD_INITIALIZER(pv_chunks);
struct mtx pv_chunks_mutex;
struct rwlock pv_list_locks[NPV_LIST_LOCKS];
static struct md_page *pv_table;

static int pmap_flags = 0; // XXX: PMAP_PDE_SUPERPAGE;	/* flags for x86 pmaps */

static void	free_pv_chunk(struct pv_chunk *pc);
static void	free_pv_entry(pmap_t pmap, pv_entry_t pv);
static pv_entry_t get_pv_entry(pmap_t pmap, struct rwlock **lockp);
static vm_page_t reclaim_pv_chunk(pmap_t locked_pmap, struct rwlock **lockp);
static void	pmap_pvh_free(struct md_page *pvh, pmap_t pmap, vm_offset_t va);
static pv_entry_t pmap_pvh_remove(struct md_page *pvh, pmap_t pmap,
		    vm_offset_t va);


static vm_page_t pmap_enter_quick_locked(pmap_t pmap, vm_offset_t va,
    vm_page_t m, vm_prot_t prot, vm_page_t mpte, struct rwlock **lockp);
static vm_page_t pmap_lookup_pt_page(pmap_t pmap, vm_offset_t va);

static void _pmap_unwire_ptp(pmap_t pmap, vm_offset_t va, vm_page_t m,
    struct spglist *free);
static int pmap_unuse_pt(pmap_t, vm_offset_t, pd_entry_t, struct spglist *);

static vm_paddr_t	boot_ptphys;	/* phys addr of start of
					 * kernel bootstrap tables
					 */
static vm_paddr_t	boot_ptendphys;	/* phys addr of end of kernel
					 * bootstrap page tables
					 */
extern int gdtset;
extern uint64_t xenstack; /* The stack Xen gives us at boot */
extern char *console_page; /* The shared ring for console i/o */
extern struct xenstore_domain_interface *xen_store; /* xenstore page */

extern vm_map_t pv_map;

/********************/
/* Inline functions */
/********************/

/* XXX: */

#define MACH_TO_DMAP(_m) PHYS_TO_DMAP(xpmap_mtop(_m))
#define DMAP_TO_MACH(_v) xpmap_ptom(DMAP_TO_PHYS(_v))

/* Return a non-clipped PD index for a given VA */
static __inline vm_pindex_t
pmap_pde_pindex(vm_offset_t va)
{
	return (va >> PDRSHIFT);
}


/* Return various clipped indexes for a given VA */
static __inline vm_pindex_t
pmap_pte_index(vm_offset_t va)
{

	return ((va >> PAGE_SHIFT) & ((1ul << NPTEPGSHIFT) - 1));
}

static __inline vm_pindex_t
pmap_pde_index(vm_offset_t va)
{

	return ((va >> PDRSHIFT) & ((1ul << NPDEPGSHIFT) - 1));
}

static __inline vm_pindex_t
pmap_pdpe_index(vm_offset_t va)
{

	return ((va >> PDPSHIFT) & ((1ul << NPDPEPGSHIFT) - 1));
}

static __inline vm_pindex_t
pmap_pml4e_index(vm_offset_t va)
{

	return ((va >> PML4SHIFT) & ((1ul << NPML4EPGSHIFT) - 1));
}

/* Return a pointer to the PML4 slot that corresponds to a VA */
static __inline pml4_entry_t *
pmap_pml4e(pmap_t pmap, vm_offset_t va)
{

	return (&pmap->pm_pml4[pmap_pml4e_index(va)]);
}

/* Return a pointer to the PDP slot that corresponds to a VA */
static __inline pdp_entry_t *
pmap_pml4e_to_pdpe(pml4_entry_t *pml4e, vm_offset_t va)
{
	pdp_entry_t *pdpe;

	pdpe = (pdp_entry_t *)MACH_TO_DMAP(*pml4e & PG_FRAME);
	return (&pdpe[pmap_pdpe_index(va)]);
}

/* Return a pointer to the PDP slot that corresponds to a VA */
static __inline pdp_entry_t *
pmap_pdpe(pmap_t pmap, vm_offset_t va)
{
	pml4_entry_t *pml4e;
	pt_entry_t PG_V;

	PG_V = pmap_valid_bit(pmap);
	pml4e = pmap_pml4e(pmap, va);
	if ((*pml4e & PG_V) == 0)
		return (NULL);
	return (pmap_pml4e_to_pdpe(pml4e, va));
}

/* Return a pointer to the PD slot that corresponds to a VA */
static __inline pd_entry_t *
pmap_pdpe_to_pde(pdp_entry_t *pdpe, vm_offset_t va)
{
	pd_entry_t *pde;

	pde = (pd_entry_t *)MACH_TO_DMAP(*pdpe & PG_FRAME);
	return (&pde[pmap_pde_index(va)]);
}

/* Return a pointer to the PD slot that corresponds to a VA */
static __inline pd_entry_t *
pmap_pde(pmap_t pmap, vm_offset_t va)
{
	pdp_entry_t *pdpe;
	pt_entry_t PG_V;

	PG_V = pmap_valid_bit(pmap);
	pdpe = pmap_pdpe(pmap, va);
	if (pdpe == NULL || (*pdpe & PG_V) == 0)
		return (NULL);
	return (pmap_pdpe_to_pde(pdpe, va));
}

/* Return a pointer to the PT slot that corresponds to a VA */
static __inline pt_entry_t *
pmap_pde_to_pte(pd_entry_t *pde, vm_offset_t va)
{
	pt_entry_t *pte;

	pte = (pt_entry_t *)MACH_TO_DMAP(*pde & PG_FRAME);
	return (&pte[pmap_pte_index(va)]);
}

/* Return a pointer to the PT slot that corresponds to a VA */
static __inline pt_entry_t *
pmap_pte(pmap_t pmap, vm_offset_t va)
{
	pd_entry_t *pde;
	pt_entry_t PG_V;

	PG_V = pmap_valid_bit(pmap);
	pde = pmap_pde(pmap, va);
	if (pde == NULL || (*pde & PG_V) == 0)
		return (NULL);
	if ((*pde & PG_PS) != 0)	/* compat with i386 pmap_pte() */
		return ((pt_entry_t *)pde);
	return (pmap_pde_to_pte(pde, va));
}

/* Index offset into a pagetable, for a given va */
static int
pt_index(uintptr_t va)
{
	return ((va & PDRMASK) >> PAGE_SHIFT);
}

static __inline void
pmap_resident_count_inc(pmap_t pmap, int count)
{

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	pmap->pm_stats.resident_count += count;
}

static __inline void
pmap_resident_count_dec(pmap_t pmap, int count)
{

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	pmap->pm_stats.resident_count -= count;
}

PMAP_INLINE pt_entry_t *
vtopte(vm_offset_t va)
{
	u_int64_t mask = ((1ul << (NPTEPGSHIFT + NPDEPGSHIFT + NPDPEPGSHIFT + NPML4EPGSHIFT)) - 1);

	KASSERT(va >= VM_MAXUSER_ADDRESS, ("vtopte on a uva/gpa 0x%0lx", va));

	return (PTmap + ((va >> PAGE_SHIFT) & mask));
}

static __inline pd_entry_t *
vtopde(vm_offset_t va)
{
	u_int64_t mask = ((1ul << (NPDEPGSHIFT + NPDPEPGSHIFT + NPML4EPGSHIFT)) - 1);

	KASSERT(va >= VM_MAXUSER_ADDRESS, ("vtopde on a uva/gpa 0x%0lx", va));

	return (PDmap + ((va >> PDRSHIFT) & mask));
}

/* return kernel virtual address of  'n' claimed physical pages at boot. */
static vm_paddr_t
allocpages(vm_paddr_t *firstaddr, int n)
{
	vm_paddr_t ret = *firstaddr;
	bzero((void *)PTOV(ret), n * PAGE_SIZE);
	*firstaddr += n * PAGE_SIZE;

	KASSERT(PTOV(*firstaddr) <= (xenstack + 512 * 1024), 
		("Attempt to use unmapped va\n"));

	return (ret);
}

/* 
 * At boot, xen guarantees us a 512kB padding area that is passed to
 * us. We must be careful not to spill the tables we create here
 * beyond this.
 */

/* Set page addressed by va to r/o */
static void
pmap_xen_setpages_ro(uintptr_t va, vm_size_t npages)
{
	vm_size_t i;
	pt_entry_t PG_V;
	uintptr_t tva;
	vm_paddr_t pa, ma;

	PG_V = pmap_valid_bit(kernel_pmap);

	for (i = 0; i < npages; i++) {
		tva = va + ptoa(i);
		pa = ISBOOTVA(tva) ? VTOP(tva) :
			ISDMAPVA(tva) ? DMAP_TO_PHYS(tva) :
			ISKERNELVA(tva) ? pmap_kextract(tva) :
			0;

		KASSERT(pa != 0, ("%s: Unknown kernel va \n", __func__));

		ma = phystomach(pa);

		PT_SET_MA(tva,
			  ma | PG_U | PG_V);

	}
}

/* Set page addressed by va to r/w */
static void
pmap_xen_setpages_rw(uintptr_t va, vm_size_t npages)
{
	vm_size_t i;

	pt_entry_t PG_V, PG_RW;
	uintptr_t tva;
	vm_paddr_t ma;

	PG_V = pmap_valid_bit(kernel_pmap);
	PG_RW = pmap_rw_bit(kernel_pmap);

	for (i = 0; i < npages; i++) {
		tva = va + ptoa(i);
		ma = phystomach(ISBOOTVA(tva) ? VTOP(tva) :
				ISDMAPVA(tva) ? DMAP_TO_PHYS(tva) :
				0);
		KASSERT(ma != 0, ("%s: Unknown kernel va \n", __func__));

			      
		PT_SET_MA(tva,
			  ma | PG_U | PG_V | PG_RW);
	}
}

extern int etext;	/* End of kernel text (virtual address) */
extern int end;		/* End of kernel binary (virtual address) */
/* Return pte flags according to kernel va access restrictions */

static pt_entry_t
pmap_xen_kernel_vaflags(uintptr_t va)
{
	pt_entry_t PG_RW;
	PG_RW = pmap_rw_bit(kernel_pmap);

	if ((va > (uintptr_t) &etext && /* .data, .bss et. al */
	     (va < (uintptr_t) &end))
	    ||
	    ((va > (uintptr_t)(xen_start_info->pt_base +
	    			xen_start_info->nr_pt_frames * PAGE_SIZE)) &&
	     va < PTOV(boot_ptphys))
	    ||
	    va > PTOV(boot_ptendphys)) {
		return PG_RW;
	}

	return 0;
}

uintptr_t tmpva;

CTASSERT(powerof2(NDMPML4E));

/* number of kernel PDP slots */
#define	NKPDPE(ptpgs)		howmany((ptpgs), NPDEPG)

static void
nkpt_init(vm_paddr_t addr)
{
	int pt_pages;
	
#ifdef NKPT
	pt_pages = NKPT;
#else
	
	pt_pages = howmany(addr, 1 << PDRSHIFT);
	pt_pages += NKPDPE(pt_pages);

	/*
	 * Add some slop beyond the bare minimum required for bootstrapping
	 * the kernel.
	 *
	 * This is quite important when allocating KVA for kernel modules.
	 * The modules are required to be linked in the negative 2GB of
	 * the address space.  If we run out of KVA in this region then
	 * pmap_growkernel() will need to allocate page table pages to map
	 * the entire 512GB of KVA space which is an unnecessary tax on
	 * physical memory.
	 */
	pt_pages += 8;		/* 16MB additional slop for kernel modules */
#endif
	nkpt = pt_pages;
}


/* create a linear mapping for a span of 'nkmapped' pages */

static void
create_pagetables(vm_paddr_t *firstaddr, int nkmapped)
{

	int i, j, ndm1g, nkpdpe;
	pt_entry_t *pt_p;
	pd_entry_t *pd_p;
	pdp_entry_t *pdp_p;
	pml4_entry_t *p4_p;

	boot_ptphys = *firstaddr; /* lowest available r/w area */

	/* Allocate page table pages for the direct map */
	ndmpdp = (ptoa(Maxmem) + NBPDP - 1) >> PDPSHIFT;
	if (ndmpdp < 4)		/* Minimum 4GB of dirmap */
		ndmpdp = 4;
	ndmpdpphys = howmany(ndmpdp, NPDPEPG);
	if (ndmpdpphys > NDMPML4E) {
		/*
		 * Each NDMPML4E allows 512 GB, so limit to that,
		 * and then readjust ndmpdp and ndmpdpphys.
		 */
		printf("NDMPML4E limits system to %d GB\n", NDMPML4E * 512);
		Maxmem = atop(NDMPML4E * NBPML4);
		ndmpdpphys = NDMPML4E;
		ndmpdp = NDMPML4E * NPDEPG;
	}
	DMPDPphys = allocpages(firstaddr, ndmpdpphys);
	ndm1g = 0;
	amd_feature = 0; /* XXX: revisit */
	if ((amd_feature & AMDID_PAGE1GB) != 0)
		ndm1g = ptoa(Maxmem) >> PDPSHIFT;
	if (ndm1g < ndmpdp)
		DMPDphys = allocpages(firstaddr, ndmpdp - ndm1g);
	dmaplimit = (vm_paddr_t)ndmpdp << PDPSHIFT;

	/* Allocate pages */
	KPML4phys = allocpages(firstaddr, 1);
	KPDPphys = allocpages(firstaddr, NKPML4E);

	/*
	 * Allocate the initial number of kernel page table pages required to
	 * bootstrap.  We defer this until after all memory-size dependent
	 * allocations are done (e.g. direct map), so that we don't have to

	 * build in too much slop in our estimate.
	 *
	 * Note that when NKPML4E > 1, we have an empty page underneath
	 * all but the KPML4I'th one, so we need NKPML4E-1 extra (zeroed)
	 * pages.  (pmap_enter requires a PD page to exist for each KPML4E.)
	 */

	nkpt_init(ptoa(nkmapped));
	nkpdpe = NKPDPE(nkpt);

	KPTphys = allocpages(firstaddr, nkpt);
	KPDphys = allocpages(firstaddr, nkpdpe);

	/* 
	 * Xen doesn't like aliased maps with unmatched access
	 * rights. Also, a duplicate set of PTs is wasteful. So we
	 * stitch together the Kernel map and the rest of unmapped RAM
	 * into the Direct map.
	 */

	int ndmpd;

	ndmpd = howmany(Maxmem, NPTEPG);
	ndmpd += NKPDPE(ndmpd);

	DMPTphys = allocpages(firstaddr, ndmpd);

	boot_ptendphys = *firstaddr - 1;

	/* We can't spill over beyond the 512kB padding */
	KASSERT(((boot_ptendphys - boot_ptphys) / 1024) <= 512,
		("bootstrap mapped memory insufficient.\n"));

	/* Fill in the underlying page table pages */
	/* Nominally read-only (but really R/W) from zero to physfree */
	/* XXX not fully used, underneath 2M pages */
	pt_p = (pt_entry_t *)KPTphys;

	/* Adjust for Xen */
	pt_p = (pt_entry_t *)PTOV(KPTphys);

	for (i = 0; ptoa(i) < ptoa(nkmapped); i++) {
		pt_p[i] = ptoa(i) | X86_PG_RW | X86_PG_V | X86_PG_G;

		/* Adjust to machine addr and attributes for Xen */
		pt_p[i] = phystomach(ptoa(i));
		pt_p[i] |= X86_PG_V | X86_PG_U |
			pmap_xen_kernel_vaflags(PTOV(i << PAGE_SHIFT));

	}

	/* Now map the page tables at their location within PTmap */
	pd_p = (pd_entry_t *)KPDphys;

	/* Adjust for Xen */
	pd_p = (pd_entry_t *)PTOV(KPDphys);

	for (i = 0; i < nkpt; i++) {
		pd_p[i] = (KPTphys + ptoa(i)) | X86_PG_RW | X86_PG_V;

		/* Adjust to machine addr and attributes for Xen */
		pd_p[i] = phystomach(KPTphys + ptoa(i));
		pmap_xen_setpages_ro(PTOV(KPTphys + ptoa(i)), 1);

		pd_p[i] |= X86_PG_RW | X86_PG_V | X86_PG_U;
	}

#ifdef LARGEFRAMES
	/* Map from zero to end of allocations under 2M pages */
	/* This replaces some of the KPTphys entries above */
	for (i = 0; (i << PDRSHIFT) < ptoa(nkmapped); i++) {
		pd_p[i] = (i << PDRSHIFT) | X86_PG_RW | X86_PG_V | PG_PS |
		    X86_PG_G;

		/* Adjust to machine addr and attributes for Xen */
		pd_p[i] = phystomach(i << PDRSHIFT);
		pd_p[i] |= X86_PG_RW | X86_PG_V | PG_PS | X86_PG_G | X86_PG_U;
	}

#endif


	/* And connect up the PD to the PDP (leaving room for L4 pages) */
	pdp_p = (pdp_entry_t *)(KPDPphys + ptoa(KPML4I - KPML4BASE));

	/* Adjust to machine addr and attributes for Xen */
	pdp_p = (pdp_entry_t *)(PTOV(KPDPphys + ptoa(KPML4I - KPML4BASE)));

	for (i = 0; i < nkpdpe; i++) {
		pdp_p[i + KPDPI] = (KPDphys + ptoa(i)) | X86_PG_RW | X86_PG_V |
		    PG_U;

		/* Adjust to machine addr and attributes for Xen */
		pdp_p[i + KPDPI] = phystomach(KPDphys + ptoa(i));
		pmap_xen_setpages_ro(PTOV(KPDphys + ptoa(i)), 1);

		pdp_p[i + KPDPI] |= X86_PG_RW | X86_PG_V | X86_PG_U;

	}

	pt_p = (pt_entry_t *)PTOV(DMPTphys);

	for (i = 0; ptoa(i) < ptoa(Maxmem); i++) {
		pt_p[i] = phystomach(ptoa(i));
		pt_p[i] |= X86_PG_V | X86_PG_U |
			pmap_xen_kernel_vaflags(PTOV(i << PAGE_SHIFT));
	}

	pd_p = (pd_entry_t *)PTOV(DMPDphys);

	for (i = 0; i < ndmpd; i++) {
		pd_p[i] = phystomach(DMPTphys + ptoa(i));
		pmap_xen_setpages_ro(PTOV(DMPTphys + ptoa(i)), 1);

		pd_p[i] |= X86_PG_RW | X86_PG_V | X86_PG_U;
	}

	i = 0; /* Reset for the pdp loop, below */
	pdp_p = (pdp_entry_t *)PTOV(DMPDPphys);


#ifdef LARGEFRAMES
	/*
	 * Now, set up the direct map region using 2MB and/or 1GB pages.  If
	 * the end of physical memory is not aligned to a 1GB page boundary,
	 * then the residual physical memory is mapped with 2MB pages.  Later,
	 * if pmap_mapdev{_attr}() uses the direct map for non-write-back
	 * memory, pmap_change_attr() will demote any 2MB or 1GB page mappings
	 * that are partially used. 
	 */
	pd_p = (pd_entry_t *)DMPDphys;
	for (i = NPDEPG * ndm1g, j = 0; i < NPDEPG * ndmpdp; i++, j++) {
		pd_p[j] = (vm_paddr_t)i << PDRSHIFT;
		/* Preset PG_M and PG_A because demotion expects it. */
		pd_p[j] |= X86_PG_RW | X86_PG_V | PG_PS | X86_PG_G |
		    X86_PG_M | X86_PG_A;

		/* Adjust to machine addr and attributes for Xen */
		pd_p[j] = phystomach(i << PDRSHIFT);
		pd_p[j] |= X86_PG_RW | X86_PG_V | PG_PS | X86_PG_G |
		    X86_PG_M | X86_PG_A | X86_PG_U;
	}

	pdp_p = (pdp_entry_t *)DMPDPphys;

	/* Adjust for Xen */
	pdp_p = (pdp_entry_t *)PTOV(DMPDPphys);

	for (i = 0; i < ndm1g; i++) {
		pdp_p[i] = (vm_paddr_t)i << PDPSHIFT;
		/* Preset PG_M and PG_A because demotion expects it. */
		pdp_p[i] |= X86_PG_RW | X86_PG_V | PG_PS | X86_PG_G |
		    X86_PG_M | X86_PG_A;

		/* Adjust to machine addr and attributes for Xen */
		pdp_p[i] = phystomach(i << PDPSHIFT);
		pdp_p[i] |= X86_PG_RW | X86_PG_V | PG_PS | X86_PG_G |
		    X86_PG_M | X86_PG_A | X86_PG_U;
	}
#endif

	for (j = 0; i < ndmpdp; i++, j++) {
		pdp_p[i] = DMPDphys + ptoa(j);
		pdp_p[i] |= X86_PG_RW | X86_PG_V | PG_U;

		/* Adjust to machine addr and attributes for Xen */
		pdp_p[i] = phystomach(DMPDphys + ptoa(j));
		pmap_xen_setpages_ro(PTOV(DMPDphys + ptoa(j)), 1);
		pdp_p[i] |= X86_PG_RW | X86_PG_V | X86_PG_U;
	}

	/* And recursively map PML4 to itself in order to get PTmap */
	p4_p = (pml4_entry_t *)KPML4phys;

	/* Adjust for Xen */
	p4_p = (pml4_entry_t *)PTOV(KPML4phys);

	p4_p[PML4PML4I] = KPML4phys;
	p4_p[PML4PML4I] |= X86_PG_RW | X86_PG_V | PG_U;

	/* Adjust to machine addr and attributes for Xen */
	p4_p[PML4PML4I] = phystomach(KPML4phys);
	p4_p[PML4PML4I] |= X86_PG_V |  PG_U;

	/* Connect the Direct Map slot(s) up to the PML4. */
	for (i = 0; i < ndmpdpphys; i++) {
		p4_p[DMPML4I + i] = DMPDPphys + ptoa(i);
		p4_p[DMPML4I + i] |= X86_PG_RW | X86_PG_V | PG_U;

		/* Adjust to machine addr and attributes for Xen */
		p4_p[DMPML4I + i] = phystomach(DMPDPphys + ptoa(i));
		pmap_xen_setpages_ro(PTOV(DMPDPphys + ptoa(i)), 1);

		p4_p[DMPML4I + i] |= X86_PG_RW | X86_PG_V | PG_U;
	}

	/* Connect the KVA slots up to the PML4 */
	for (i = 0; i < NKPML4E; i++) {
		p4_p[KPML4BASE + i] = KPDPphys + ptoa(i);
		p4_p[KPML4BASE + i] |= X86_PG_RW | X86_PG_V | PG_U;

		/* Adjust to machine addr and attributes for Xen */
		p4_p[KPML4BASE + i] = phystomach(KPDPphys + ptoa(i));

		pmap_xen_setpages_ro(PTOV(KPDPphys + ptoa(i)), 1);

		p4_p[KPML4BASE + i] |= X86_PG_RW | X86_PG_V | PG_U;
	}

	pmap_xen_setpages_ro(PTOV(KPML4phys), 1);

	xen_pgdir_pin(phystomach(VTOP((vm_offset_t)p4_p)));
}

/* 
 *
 * Map in the xen provided share page. Note: The console page is
 * mapped in later in the boot process, when kmem_alloc*() is
 * available. 
 */

static void
pmap_xen_bootpages(vm_paddr_t *firstaddr)
{
	uintptr_t va;
	vm_paddr_t ma;

	pt_entry_t PG_V, PG_RW;

	PG_V = pmap_valid_bit(kernel_pmap);
	PG_RW = pmap_rw_bit(kernel_pmap);

	/* i) Share info */

	/* 
	 * Steal some mapped VA from the nkmapped range (so that
	 * PT_SET_MA() will work). We can't however overlap into
	 * memory allocated by allocpages(). Adjust "xenstack" to
	 * reflect this. This is safe, because "xenstack" is not used
	 * as our stack bottom (See: amd64/xen/locore.S)
	 * 
	 */

	va = virtual_avail - PAGE_SIZE;
	KASSERT(PTOV(*firstaddr) < va,
		("Boot VAs exhausted!\n"));
	ma = xen_start_info->shared_info;
	PT_SET_MA(va, ma | PG_RW | PG_V | PG_U);
	HYPERVISOR_shared_info = (void *) va;

	/* ii) Console page */

	/* Get a va for console and map the console mfn into it */
	va -= PAGE_SIZE;
	KASSERT(PTOV(*firstaddr) < va,
		("Boot VAs exhausted!\n"));
	ma = xen_start_info->console.domU.mfn << PAGE_SHIFT;
	PT_SET_MA(va, ma | PG_RW | PG_V | PG_U);
	console_page = (void *)va;

	/* iii) xenstore shared page */
	/* Get a va for the xenstore shared page */
	va -= PAGE_SIZE;
	KASSERT(PTOV(*firstaddr) < va,
		("Boot VAs exhausted!\n"));
	ma = xen_start_info->store_mfn << PAGE_SHIFT;
	PT_SET_MA(va, ma | PG_RW | PG_V | PG_U);
	xen_store = (void *)va;

#if 0
	/* iv) Userland page table base */
	va = PTOV(allocpages(firstaddr, 1));
	/* XXX: assert for collision with stolen VAs above */
	bzero((void *)va, PAGE_SIZE);

	/* 
	 * x86_64 has 2 privilege rings and Xen keeps separate pml4
	 * pointers for each, which are sanity checked on every
	 * exit via hypervisor_iret. We therefore set up a zeroed out
	 * user page table pml4 to satisfy/fool xen.
	 */
	
	/* Mark the page r/o before pinning */
	pmap_xen_setpages_ro(va, 1);

	/* Pin the table */
	xen_pgdir_pin(phystomach(VTOP(va)));

	/* Register user page table with Xen */
	xen_pt_user_switch(xpmap_ptom(VTOP(va)));
#endif

}


/* alloc from linear mapped boot time virtual address space */
static uintptr_t
mmu_alloc(void)
{
	uintptr_t va;

	KASSERT(physfree != 0,
		("physfree must have been set before using mmu_alloc"));
				
	va = PTOV(allocpages(&physfree, atop(PAGE_SIZE)));

	/* 
	 * Xen requires the page table hierarchy to be R/O.
	 */

	pmap_xen_setpages_ro(va, atop(PAGE_SIZE));

	return va;
}

void
pmap_bootstrap(vm_paddr_t *firstaddr)
{

	int nkmapped;
	long tmpMaxmem;

	nkmapped = atop(VTOP(xenstack + 512 * 1024 + PAGE_SIZE));
	tmpMaxmem = Maxmem;

	/* Truncate bootstrap pages to fit in 512kb */
	//Maxmem = atop(pgtok(Maxmem)) <= 64 ? Maxmem : 64;

	create_pagetables(firstaddr, nkmapped);

	/* Switch to the new kernel tables */
	xen_pt_switch(xpmap_ptom(KPML4phys));

	/* Unpin old page table hierarchy, and mark all its pages r/w */
	xen_pgdir_unpin(phystomach(VTOP(xen_start_info->pt_base)));

	pmap_xen_setpages_rw(xen_start_info->pt_base,
			     xen_start_info->nr_pt_frames);

	/* And DMAP mappings */
	pmap_xen_setpages_rw(PHYS_TO_DMAP(VTOP(xen_start_info->pt_base)),
			     xen_start_info->nr_pt_frames);
		     
	/* 
	 * gc newly free pages (bootstrap PTs and bootstrap stack,
	 * mostly, I think.).
	 * Record the pages as available to the VM via phys_avail[] 
	 */

	/* This is the first free phys segment. see: xen.h */
	KASSERT(pa_index == 0, 
		("reclaimed page table pages are not the lowest available!"));

	dump_avail[pa_index + 1] = phys_avail[pa_index] = VTOP(xen_start_info->pt_base);
	dump_avail[pa_index + 2] = phys_avail[pa_index + 1] = phys_avail[pa_index] +
		ptoa(xen_start_info->nr_pt_frames);
	pa_index += 2;

	/*
	 * Xen guarantees mapped virtual addresses at boot time upto
	 * xenstack + 512KB. We want to use these for allocpages()
	 * and therefore don't want to touch these mappings since
	 * they're scarce resources. Move along to the end of
	 * guaranteed mapping.
	 *
	 * Note: Xen *may* provide mappings upto xenstack + 2MB, but
	 * this is not guaranteed. We therefore assum that only 512KB
	 * is available.
	 */

	virtual_avail = (uintptr_t) xenstack + 512 * 1024;
	/* XXX: Check we don't overlap xen pgdir entries. */
	virtual_end = VM_MAX_KERNEL_ADDRESS - PAGE_SIZE; 

	/* Map in Xen related pages into VA space */
	pmap_xen_bootpages(firstaddr);

	/*
	 * Initialize the kernel pmap (which is statically allocated).
	 */

	PMAP_LOCK_INIT(kernel_pmap);

	kernel_pmap->pm_pml4 = (pdp_entry_t *)PTOV(KPML4phys);
	kernel_pmap->pm_cr3 = xpmap_ptom((vm_offset_t) KPML4phys);
	kernel_pmap->pm_root.rt_root = 0;

	CPU_FILL(&kernel_pmap->pm_active);	/* don't allow deactivation */

	CPU_ZERO(&kernel_pmap->pm_save);
	TAILQ_INIT(&kernel_pmap->pm_pvchunk);
	kernel_pmap->pm_flags = pmap_flags;

 	/*
	 * Initialize the global pv list lock.
	 */
	rw_init(&pvh_global_lock, "pmap pv global");

	pmap_pv_init();

	/* Steal some memory (backing physical pages, and kva) */
	physmem -= atop(round_page(msgbufsize));

	msgbufp = (struct msgbuf *)PHYS_TO_DMAP(ptoa(physmem));
}

/*
 * Determine the appropriate bits to set in a PTE or PDE for a specified
 * caching mode.
 */
static int
pmap_cache_bits(pmap_t pmap, int mode, boolean_t is_pde)
{

	return 0;
}

/*
 *	Initialize a vm_page's machine-dependent fields.
 */
void
pmap_page_init(vm_page_t m)
{
	pmap_pv_vm_page_init(m);
}

/*
 *	Initialize the pmap module.
 *	Called by vm_init, to initialize any structures that the pmap
 *	system needs to map virtual memory.
 */

void
pmap_init(void)
{
	vm_page_t mpte;
	vm_size_t s;
	int i, pv_npg;

	/*
	 * Initialize the vm page array entries for the kernel pmap's
	 * page table pages.
	 */ 
	for (i = 0; i < nkpt; i++) {
		mpte = PHYS_TO_VM_PAGE(KPTphys + (i << PAGE_SHIFT));
		KASSERT(mpte >= vm_page_array &&
		    mpte < &vm_page_array[vm_page_array_size],
		    ("pmap_init: page table page is out of range"));
		mpte->pindex = pmap_pde_pindex(KERNBASE) + i;
		mpte->phys_addr = KPTphys + (i << PAGE_SHIFT);
	}

#ifdef DOM0 /* XXX: */
	/*
	 * If the kernel is running on a virtual machine, then it must assume
	 * that MCA is enabled by the hypervisor.  Moreover, the kernel must
	 * be prepared for the hypervisor changing the vendor and family that
	 * are reported by CPUID.  Consequently, the workaround for AMD Family
	 * 10h Erratum 383 is enabled if the processor's feature set does not
	 * include at least one feature that is only supported by older Intel
	 * or newer AMD processors.
	 */
	if (vm_guest == VM_GUEST_VM && (cpu_feature & CPUID_SS) == 0 &&
	    (cpu_feature2 & (CPUID2_SSSE3 | CPUID2_SSE41 | CPUID2_AESNI |
	    CPUID2_AVX | CPUID2_XSAVE)) == 0 && (amd_feature2 & (AMDID2_XOP |
	    AMDID2_FMA4)) == 0)
		workaround_erratum383 = 1;

#endif
	/*
	 * Are large page mappings enabled?
	 */
	TUNABLE_INT_FETCH("vm.pmap.pg_ps_enabled", &pg_ps_enabled);
	if (pg_ps_enabled) {
		KASSERT(MAXPAGESIZES > 1 && pagesizes[1] == 0,
		    ("pmap_init: can't assign to pagesizes[1]"));
		pagesizes[1] = NBPDR;
	}

	/*
	 * Initialize the pv chunk list mutex.
	 */
	mtx_init(&pv_chunks_mutex, "pmap pv chunk list", NULL, MTX_DEF);

	/*
	 * Initialize the pool of pv list locks.
	 */
	for (i = 0; i < NPV_LIST_LOCKS; i++)
		rw_init(&pv_list_locks[i], "pmap pv list");

	/*
	 * Calculate the size of the pv head table for superpages.
	 */
	for (i = 0; phys_avail[i + 1]; i += 2);
	pv_npg = round_2mpage(phys_avail[(i - 2) + 1]) / NBPDR;

	/*
	 * Allocate memory for the pv head table for superpages.
	 */
	s = (vm_size_t)(pv_npg * sizeof(struct md_page));
	s = round_page(s);
	pv_table = (struct md_page *)kmem_malloc(kernel_arena, s,
	    M_WAITOK | M_ZERO);
	for (i = 0; i < pv_npg; i++)
		TAILQ_INIT(&pv_table[i].pv_list);

	/* XXX: review the use of gdtset for the purpose below */


	gdtset = 1; /* xpq may assert for locking sanity from this point onwards */
}

static SYSCTL_NODE(_vm_pmap, OID_AUTO, pde, CTLFLAG_RD, 0,
    "2MB page mapping counters");

void
pmap_invalidate_page(pmap_t pmap, vm_offset_t va)
{
	pmap_invalidate_range(pmap, va, va + PAGE_SIZE);
}

void
pmap_invalidate_range(pmap_t pmap, vm_offset_t sva, vm_offset_t eva)
{

	vm_offset_t addr;
	/* XXX: TODO SMP */
	sched_pin();

	for (addr = sva; addr < eva; addr += PAGE_SIZE)
		invlpg(addr);

	sched_unpin();
}

void
pmap_invalidate_all(pmap_t pmap)
{

	if (pmap == kernel_pmap || !CPU_EMPTY(&pmap->pm_active))
		invltlb();
}

/*
 *	Routine:	pmap_extract
 *	Function:
 *		Extract the physical page address associated
 *		with the given map/virtual_address pair.
 */

vm_paddr_t 
pmap_extract(pmap_t pmap, vm_offset_t va)
{

	pdp_entry_t *pdpe;
	pd_entry_t *pde;
	pt_entry_t *pte, PG_V;
	vm_paddr_t pa;

	pa = 0;
	PG_V = pmap_valid_bit(pmap);

	PMAP_LOCK(pmap);
	pdpe = pmap_pdpe(pmap, va);
	if (pdpe != NULL && (*pdpe & PG_V) != 0) {
		if ((*pdpe & PG_PS) != 0)
			pa = (*pdpe & PG_PS_FRAME) | (va & PDPMASK);
		else {
			pde = pmap_pdpe_to_pde(pdpe, va);
			if ((*pde & PG_V) != 0) {
				if ((*pde & PG_PS) != 0) {
					pa = (*pde & PG_PS_FRAME) |
					    (va & PDRMASK);
				} else {
					pte = pmap_pde_to_pte(pde, va);
					pa = (*pte & PG_FRAME) |
					    (va & PAGE_MASK);
				}
			}
		}
	}
	PMAP_UNLOCK(pmap);
	return (pa);
}

/*
 *	Routine:	pmap_extract_and_hold
 *	Function:
 *		Atomically extract and hold the physical page
 *		with the given pmap and virtual address pair
 *		if that mapping permits the given protection.
 */

vm_page_t
pmap_extract_and_hold(pmap_t pmap, vm_offset_t va, vm_prot_t prot)
{
	pd_entry_t pde, *pdep;
	pt_entry_t pte, PG_RW, PG_V;
	vm_paddr_t pa;
	vm_page_t m;

	pa = 0;
	m = NULL;
	PG_RW = pmap_rw_bit(pmap);
	PG_V = pmap_valid_bit(pmap);
	PMAP_LOCK(pmap);
retry:
	pdep = pmap_pde(pmap, va);
	if (pdep != NULL && (pde = *pdep)) {
		if (pde & PG_PS) {
			if ((pde & PG_RW) || (prot & VM_PROT_WRITE) == 0) {
				if (vm_page_pa_tryrelock(pmap, (pde &
				    PG_PS_FRAME) | (va & PDRMASK), &pa))
					goto retry;
				m = MACH_TO_VM_PAGE((pde & PG_PS_FRAME) |
				    (va & PDRMASK));
				vm_page_hold(m);
			}
		} else {
			pte = *pmap_pde_to_pte(pdep, va);
			if ((pte & PG_V) &&
			    ((pte & PG_RW) || (prot & VM_PROT_WRITE) == 0)) {
				if (vm_page_pa_tryrelock(pmap, pte & PG_FRAME,
				    &pa))
					goto retry;
				m = MACH_TO_VM_PAGE(pte & PG_FRAME);
				vm_page_hold(m);
			}
		}
	}
	PA_UNLOCK_COND(pa);
	PMAP_UNLOCK(pmap);
	return (m);
}

vm_paddr_t
pmap_kextract(vm_offset_t va)
{
	vm_paddr_t pa, ma;

	ma = pmap_kextract_ma(va);
	pa = xpmap_mtop(ma);

	return pa;
}

vm_paddr_t
pmap_kextract_ma(vm_offset_t va)
{

	pd_entry_t pde;
	vm_paddr_t ma;

	if (va >= DMAP_MIN_ADDRESS && va < DMAP_MAX_ADDRESS) {
		ma = DMAP_TO_MACH(va);
	} else {
		pde = *vtopde(va);
#ifdef LARGEFRAMES
		if (pde & PG_PS) {
			ma = (pde & PG_PS_FRAME) | (va & PDRMASK);
		} else {
#endif
			/*
			 * Beware of a concurrent promotion that changes the
			 * PDE at this point!  For example, vtopte() must not
			 * be used to access the PTE because it would use the
			 * new PDE.  It is, however, safe to use the old PDE
			 * because the page table page is preserved by the
			 * promotion.
			 */
			ma = *pmap_pde_to_pte(&pde, va);
			ma = (ma & PG_FRAME) | (va & PAGE_MASK);
#ifdef LARGEFRAMES
		}
#endif
	}
	return ma;
}

/***************************************************
 * Low level mapping routines.....
 ***************************************************/

/*
 * Add a wired page to the kva.
 * Note: not SMP coherent.
 *
 * This function may be used before pmap_bootstrap() is called.
 */

void 
pmap_kenter(vm_offset_t va, vm_paddr_t pa)
{
	pmap_kenter_ma(va, xpmap_ptom(pa));
}

void 
pmap_kenter_ma(vm_offset_t va, vm_paddr_t ma)
{
	PT_SET_MA(va, ma | X86_PG_RW | X86_PG_V | X86_PG_U);
}

/*
 * Remove a page from the kernel pagetables.
 * Note: not SMP coherent.
 *
 * This function may be used before pmap_bootstrap() is called.
 */

PMAP_INLINE void
pmap_kremove(vm_offset_t va)
{
	PT_SET_MA(va, 0);
}

/*
 *	Used to map a range of physical addresses into kernel
 *	virtual address space.
 *
 *	The value passed in '*virt' is a suggested virtual address for
 *	the mapping. Architectures which can support a direct-mapped
 *	physical to virtual region can return the appropriate address
 *	within that region, leaving '*virt' unchanged. Other
 *	architectures should map the pages starting at '*virt' and
 *	update '*virt' with the first usable address after the mapped
 *	region.
 */

vm_offset_t
pmap_map(vm_offset_t *virt, vm_paddr_t start, vm_paddr_t end, int prot)
{
	return PHYS_TO_DMAP(start);
}

/*
 * Add a list of wired pages to the kva
 * this routine is only used for temporary
 * kernel mappings that do not need to have
 * page modification or references recorded.
 * Note that old mappings are simply written
 * over.  The page *must* be wired.
 * XXX: TODO SMP.
 * Note: SMP coherent.  Uses a ranged shootdown IPI.
 */

void
pmap_qenter(vm_offset_t sva, vm_page_t *ma, int count)
{
	KASSERT(count > 0, ("count > 0"));
	KASSERT(sva == trunc_page(sva),
		("sva not page aligned"));
	KASSERT(ma != NULL, ("ma != NULL"));


	pt_entry_t *endpte, oldpte, pa, *pte;
	vm_page_t m;

	int cache_bits = 0;


	oldpte = 0;

	pte = vtopte(sva);
	endpte = pte + count;

	while (pte < endpte) {
		m = *ma++;
		cache_bits = pmap_cache_bits(kernel_pmap, m->md.pat_mode, 0);
		pa = VM_PAGE_TO_PHYS(m) | cache_bits;
		if ((*pte & (PG_FRAME | X86_PG_PTE_CACHE)) != xpmap_ptom(pa)) {
			oldpte |= *pte;
			pte_store(pte, xpmap_ptom(pa) | X86_PG_U | X86_PG_RW | X86_PG_V);
		}
		pte++;
	}

	if (__predict_false((oldpte & X86_PG_V) != 0))
		pmap_invalidate_range(kernel_pmap, sva, sva + count *
		    PAGE_SIZE);
}

/*
 * This routine tears out page mappings from the
 * kernel -- it is meant only for temporary mappings.
 * Note: SMP coherent.  Uses a ranged shootdown IPI.
 */
void
pmap_qremove(vm_offset_t sva, int count)
{
	KASSERT(count > 0, ("count > 0"));
	KASSERT(sva == trunc_page(sva),
		("sva not page aligned"));

	vm_offset_t va;

	va = sva;
	while (count-- > 0) {
		pmap_kremove(va);
		va += PAGE_SIZE;
	}

	pmap_invalidate_range(kernel_pmap, sva, va);
}

/***************************************************
 * Page table page management routines.....
 ***************************************************/
static __inline void
pmap_free_zero_pages(struct spglist *free)
{
	vm_page_t m;

	while ((m = SLIST_FIRST(free)) != NULL) {
		SLIST_REMOVE_HEAD(free, plinks.s.ss);

		pmap_xen_setpages_rw(MACH_TO_DMAP(VM_PAGE_TO_MACH(m)), 1);

		/* Preserve the page's PG_ZERO setting. */
		vm_page_free_toq(m);
	}
}

/*
 * Schedule the specified unused page table page to be freed.  Specifically,
 * add the page to the specified list of pages that will be released to the
 * physical memory manager after the TLB has been updated.
 */
static __inline void
pmap_add_delayed_free_list(vm_page_t m, struct spglist *free,
    boolean_t set_PG_ZERO)
{

	if (set_PG_ZERO)
		m->flags |= PG_ZERO;
	else
		m->flags &= ~PG_ZERO;
	SLIST_INSERT_HEAD(free, m, plinks.s.ss);
}
	
/*
 * Inserts the specified page table page into the specified pmap's collection
 * of idle page table pages.  Each of a pmap's page table pages is responsible
 * for mapping a distinct range of virtual addresses.  The pmap's collection is
 * ordered by this virtual address range.
 */
static __inline int
pmap_insert_pt_page(pmap_t pmap, vm_page_t mpte)
{

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	return (vm_radix_insert(&pmap->pm_root, mpte));
}

/*
 * Looks for a page table page mapping the specified virtual address in the
 * specified pmap's collection of idle page table pages.  Returns NULL if there
 * is no page table page corresponding to the specified virtual address.
 */
static __inline vm_page_t
pmap_lookup_pt_page(pmap_t pmap, vm_offset_t va)
{

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	return (vm_radix_lookup(&pmap->pm_root, pmap_pde_pindex(va)));
}

/*
 * Removes the specified page table page from the specified pmap's collection
 * of idle page table pages.  The specified page table page must be a member of
 * the pmap's collection.
 */
static __inline void
pmap_remove_pt_page(pmap_t pmap, vm_page_t mpte)
{

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	vm_radix_remove(&pmap->pm_root, mpte->pindex);
}

/*
 * Decrements a page table page's wire count, which is used to record the
 * number of valid page table entries within the page.  If the wire count
 * drops to zero, then the page table page is unmapped.  Returns TRUE if the
 * page table page was unmapped and FALSE otherwise.
 */
static inline boolean_t
pmap_unwire_ptp(pmap_t pmap, vm_offset_t va, vm_page_t m, struct spglist *free)
{

	--m->wire_count;
	if (m->wire_count == 0) {
		_pmap_unwire_ptp(pmap, va, m, free);
		return (TRUE);
	} else
		return (FALSE);
}

static void
_pmap_unwire_ptp(pmap_t pmap, vm_offset_t va, vm_page_t m, struct spglist *free)
{

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	/*
	 * unmap the page table page
	 */
	if (m->pindex >= (NUPDE + NUPDPE)) {
		/* PDP page */
		pml4_entry_t *pml4;
		pml4 = pmap_pml4e(pmap, va);
		PT_CLEAR_VA(pml4, true);
	} else if (m->pindex >= NUPDE) {
		/* PD page */
		pdp_entry_t *pdp;
		pdp = pmap_pdpe(pmap, va);
		PT_CLEAR_VA(pdp, true);
	} else {
		/* PTE page */
		pd_entry_t *pd;
		pd = pmap_pde(pmap, va);
		PT_CLEAR_VA(pd, true);
	}
	pmap_resident_count_dec(pmap, 1);
	if (m->pindex < NUPDE) {
		/* We just released a PT, unhold the matching PD */
		vm_page_t pdpg;

		pdpg = MACH_TO_VM_PAGE(*pmap_pdpe(pmap, va) & PG_FRAME);
		pmap_unwire_ptp(pmap, va, pdpg, free);
	}
	if (m->pindex >= NUPDE && m->pindex < (NUPDE + NUPDPE)) {
		/* We just released a PD, unhold the matching PDP */
		vm_page_t pdppg;

		pdppg = MACH_TO_VM_PAGE(*pmap_pml4e(pmap, va) & PG_FRAME);
		pmap_unwire_ptp(pmap, va, pdppg, free);
	}

	/*
	 * This is a release store so that the ordinary store unmapping
	 * the page table page is globally performed before TLB shoot-
	 * down is begun.
	 */
	atomic_subtract_rel_int(&vm_cnt.v_wire_count, 1);

	/* 
	 * Put page on a list so that it is released after
	 * *ALL* TLB shootdown is done
	 */
	pmap_add_delayed_free_list(m, free, TRUE);
}


/*
 * After removing a page table entry, this routine is used to
 * conditionally free the page, and manage the hold/wire counts.
 */
static int
pmap_unuse_pt(pmap_t pmap, vm_offset_t va, pd_entry_t ptepde,
    struct spglist *free)
{
	vm_page_t mpte;

	if (va >= VM_MAXUSER_ADDRESS)
		return (0);
	KASSERT(ptepde != 0, ("pmap_unuse_pt: ptepde != 0"));
	mpte = MACH_TO_VM_PAGE(ptepde & PG_FRAME);
	return (pmap_unwire_ptp(pmap, va, mpte, free));
}

void
pmap_pinit0(pmap_t pmap)
{
	PMAP_LOCK_INIT(pmap);
	pmap->pm_pml4 = (void *) PTOV(KPML4phys);
	pmap->pm_cr3 = xpmap_ptom(KPML4phys);
	pmap->pm_root.rt_root = 0;
	CPU_ZERO(&pmap->pm_active);
	CPU_ZERO(&pmap->pm_save);
	PCPU_SET(curpmap, pmap);
	pmap_pv_pmap_init(pmap);
	bzero(&pmap->pm_stats, sizeof pmap->pm_stats);
	pmap->pm_pcid = -1;
}

int
pmap_pinit(pmap_t pmap)
{

	KASSERT(pmap != kernel_pmap, 
		("%s: kernel map re-initialised!", __func__));

	/*
	 * allocate the page directory page
	 */
	pmap->pm_pml4 = (void *) kmem_malloc(kernel_arena, PAGE_SIZE, M_ZERO);

	if (pmap->pm_pml4 == NULL) return 0;

	pmap->pm_cr3 = pmap_kextract_ma((vm_offset_t)pmap->pm_pml4);

	/* 
	 * We do not wire in kernel space, or the self-referencial
	 * entry in userspace pmaps becase both kernel and userland
	 * share ring3 privilege. The user/kernel context switch is
	 * arbitrated by the hypervisor by means of pre-loaded values
	 * for kernel and user %cr3. The userland parts of kernel VA
	 * may be conditionally overlaid with the VA of curthread,
	 * since the kernel occasionally needs to access userland
	 * process VA space.
	 */

	pmap_xen_setpages_ro((uintptr_t)pmap->pm_pml4, 1);

	/* Also mark DMAP alias r/o */
	pmap_xen_setpages_ro(MACH_TO_DMAP(pmap->pm_cr3), 1);

	xen_pgdir_pin(pmap->pm_cr3);

	pmap->pm_root.rt_root = 0;
	CPU_ZERO(&pmap->pm_active);
	pmap_pv_pmap_init(pmap);
	bzero(&pmap->pm_stats, sizeof pmap->pm_stats);
	pmap->pm_pcid = -1; /* No pcid for now */
	CPU_ZERO(&pmap->pm_save);

	return 1;
}

/*
 * This routine is called if the desired page table page does not exist.
 *
 * If page table page allocation fails, this routine may sleep before
 * returning NULL.  It sleeps only if a lock pointer was given.
 *
 * Note: If a page allocation fails at page table level two or three,
 * one or two pages may be held during the wait, only to be released
 * afterwards.  This conservative approach is easily argued to avoid
 * race conditions.
 */
static vm_page_t
_pmap_allocpte(pmap_t pmap, vm_pindex_t ptepindex, struct rwlock **lockp)
{
	vm_page_t m, pdppg, pdpg;
	pt_entry_t PG_A, PG_M, PG_RW, PG_V;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);

	PG_A = pmap_accessed_bit(pmap);
	PG_M = pmap_modified_bit(pmap);
	PG_V = pmap_valid_bit(pmap);
	PG_RW = pmap_rw_bit(pmap);

	/*
	 * Allocate a page table page.
	 */
	if ((m = vm_page_alloc(NULL, ptepindex, VM_ALLOC_NOOBJ |
	    VM_ALLOC_WIRED | VM_ALLOC_ZERO)) == NULL) {
		if (lockp != NULL) {
			RELEASE_PV_LIST_LOCK(lockp);
			PMAP_UNLOCK(pmap);
			rw_runlock(&pvh_global_lock);
			VM_WAIT;
			rw_rlock(&pvh_global_lock);
			PMAP_LOCK(pmap);
		}

		/*
    		 * Indicate the need to retry.  While waiting, the page table
		 * page may have been allocated.
		 */
		return (NULL);
	}
	if ((m->flags & PG_ZERO) == 0) {
		pmap_zero_page(m);
	}

	pmap_xen_setpages_ro(MACH_TO_DMAP(VM_PAGE_TO_MACH(m)), 1);

	/*
	 * Map the pagetable page into the process address space, if
	 * it isn't already there.
	 */

	if (ptepindex >= (NUPDE + NUPDPE)) {
		pml4_entry_t *pml4;
		vm_pindex_t pml4index;

		/* Wire up a new PDPE page */
		pml4index = ptepindex - (NUPDE + NUPDPE);
		pml4 = &pmap->pm_pml4[pml4index];
		PT_SET_VA_MA(pml4, VM_PAGE_TO_MACH(m) | PG_U | PG_RW | PG_V | PG_A | PG_M, true);
	} else if (ptepindex >= NUPDE) {
		vm_pindex_t pml4index;
		vm_pindex_t pdpindex;
		pml4_entry_t *pml4;
		pdp_entry_t *pdp;

		/* Wire up a new PDE page */
		pdpindex = ptepindex - NUPDE;
		pml4index = pdpindex >> NPML4EPGSHIFT;

		pml4 = &pmap->pm_pml4[pml4index];
		if ((*pml4 & PG_V) == 0) {
			/* Have to allocate a new pdp, recurse */
			if (_pmap_allocpte(pmap, NUPDE + NUPDPE + pml4index,
			    lockp) == NULL) {
				--m->wire_count;
				atomic_subtract_int(&vm_cnt.v_wire_count, 1);
				pmap_xen_setpages_rw(MACH_TO_DMAP(VM_PAGE_TO_MACH(m)), 1);
				vm_page_free_zero(m);
				return (NULL);
			}
		} else {

			/* Add reference to pdp page */
			pdppg = MACH_TO_VM_PAGE(*pml4 & PG_FRAME);
			pdppg->wire_count++;
		}
		pdp = (pdp_entry_t *)MACH_TO_DMAP(*pml4 & PG_FRAME);

		/* Now find the pdp page */
		pdp = &pdp[pdpindex & ((1ul << NPDPEPGSHIFT) - 1)];
		PT_SET_VA_MA(pdp, VM_PAGE_TO_MACH(m) | PG_U | PG_RW | PG_V | PG_A | PG_M, true);
	} else {
		vm_pindex_t pml4index;
		vm_pindex_t pdpindex;
		pml4_entry_t *pml4;
		pdp_entry_t *pdp;
		pd_entry_t *pd;

		/* Wire up a new PTE page */
		pdpindex = ptepindex >> NPDPEPGSHIFT;
		pml4index = pdpindex >> NPML4EPGSHIFT;

		/* First, find the pdp and check that its valid. */
		pml4 = &pmap->pm_pml4[pml4index];
		if ((*pml4 & PG_V) == 0) {
			/* Have to allocate a new pd, recurse */
			if (_pmap_allocpte(pmap, NUPDE + pdpindex,
			    lockp) == NULL) {
				--m->wire_count;
				atomic_subtract_int(&vm_cnt.v_wire_count, 1);
				pmap_xen_setpages_rw(MACH_TO_DMAP(VM_PAGE_TO_MACH(m)), 1);
				vm_page_free_zero(m);
				return (NULL);
			}
			pdp = (pdp_entry_t *)MACH_TO_DMAP(*pml4 & PG_FRAME);
			pdp = &pdp[pdpindex & ((1ul << NPDPEPGSHIFT) - 1)];
		} else {
			pdp = (pdp_entry_t *)MACH_TO_DMAP(*pml4 & PG_FRAME);
			pdp = &pdp[pdpindex & ((1ul << NPDPEPGSHIFT) - 1)];
			if ((*pdp & PG_V) == 0) {
				/* Have to allocate a new pd, recurse */
				if (_pmap_allocpte(pmap, NUPDE + pdpindex,
				    lockp) == NULL) {
					--m->wire_count;
					atomic_subtract_int(&vm_cnt.v_wire_count,
					    1);
					pmap_xen_setpages_rw(MACH_TO_DMAP(VM_PAGE_TO_MACH(m)), 1);
					vm_page_free_zero(m);
					return (NULL);
				}
			} else {
				/* Add reference to the pd page */
				pdpg = MACH_TO_VM_PAGE(*pdp & PG_FRAME);
				pdpg->wire_count++;
			}
		}
		pd = (pd_entry_t *)MACH_TO_DMAP(*pdp & PG_FRAME);

		/* Now we know where the page directory page is */
		pd = &pd[ptepindex & ((1ul << NPDEPGSHIFT) - 1)];
		PT_SET_VA_MA(pd, VM_PAGE_TO_MACH(m) | PG_U | PG_RW | PG_V | PG_A | PG_M, true);
	}

	pmap_resident_count_inc(pmap, 1);

	return (m);
}

static vm_page_t
pmap_allocpde(pmap_t pmap, vm_offset_t va, struct rwlock **lockp)
{
	vm_pindex_t pdpindex, ptepindex;
	pdp_entry_t *pdpe, PG_V;
	vm_page_t pdpg;

	PG_V = pmap_valid_bit(pmap);

retry:
	pdpe = pmap_pdpe(pmap, va);
	if (pdpe != NULL && (*pdpe & PG_V) != 0) {
		/* Add a reference to the pd page. */
		pdpg = MACH_TO_VM_PAGE(*pdpe & PG_FRAME);
		pdpg->wire_count++;
	} else {
		/* Allocate a pd page. */
		ptepindex = pmap_pde_pindex(va);
		pdpindex = ptepindex >> NPDPEPGSHIFT;
		pdpg = _pmap_allocpte(pmap, NUPDE + pdpindex, lockp);
		if (pdpg == NULL && lockp != NULL)
			goto retry;
	}
	return (pdpg);
}

static vm_page_t
pmap_allocpte(pmap_t pmap, vm_offset_t va, struct rwlock **lockp)
{
	vm_pindex_t ptepindex;
	pd_entry_t *pd, PG_V;
	vm_page_t m;

	PG_V = pmap_valid_bit(pmap);

	/*
	 * Calculate pagetable page index
	 */
	ptepindex = pmap_pde_pindex(va);
retry:
	/*
	 * Get the page directory entry
	 */
	pd = pmap_pde(pmap, va);
#ifdef LARGEFRAMES
	/*
	 * This supports switching from a 2MB page to a
	 * normal 4K page.
	 */
	if (pd != NULL && (*pd & (PG_PS | PG_V)) == (PG_PS | PG_V)) {
		if (!pmap_demote_pde_locked(pmap, pd, va, lockp)) {
			/*
			 * Invalidation of the 2MB page mapping may have caused
			 * the deallocation of the underlying PD page.
			 */
			pd = NULL;
		}
	}
#endif

	/*
	 * If the page table page is mapped, we just increment the
	 * hold count, and activate it.
	 */
	if (pd != NULL && (*pd & PG_V) != 0) {
		m = MACH_TO_VM_PAGE(*pd & PG_FRAME);
		m->wire_count++;
	} else {
		/*
		 * Here if the pte page isn't mapped, or if it has been
		 * deallocated.
		 */

		m = _pmap_allocpte(pmap, ptepindex, lockp);

		if (m == NULL && lockp != NULL)
			goto retry;
	}
	return (m);
}

/***************************************************
 * Pmap allocation/deallocation routines.
 ***************************************************/

/*
 * Release any resources held by the given physical map.
 * Called when a pmap initialized by pmap_pinit is being released.
 * Should only be called if the map contains no valid mappings.
 */

void
pmap_release(pmap_t pmap)
{
	KASSERT(pmap != kernel_pmap,
		("%s: kernel pmap released", __func__));

	KASSERT(pmap->pm_stats.resident_count == 0,
	    ("pmap_release: pmap resident count %ld != 0",
	    pmap->pm_stats.resident_count));

	KASSERT(vm_radix_is_empty(&pmap->pm_root),
	    ("pmap_release: pmap has reserved page table page(s)"));

	xen_pgdir_unpin(pmap->pm_cr3);
	pmap_xen_setpages_rw(MACH_TO_DMAP(pmap->pm_cr3), 1);
	pmap_xen_setpages_rw((uintptr_t)pmap->pm_pml4, 1);

	bzero(pmap->pm_pml4, PAGE_SIZE);
	kmem_free(kernel_arena, (vm_offset_t)pmap->pm_pml4, PAGE_SIZE);
}

/*
 * grow the number of kernel page table entries, if needed
 */
void
pmap_growkernel(vm_offset_t addr)
{
	vm_paddr_t paddr;
	vm_page_t nkpg;
	pd_entry_t *pde, newpdir;
	pdp_entry_t *pdpe;

	mtx_assert(&kernel_map->system_mtx, MA_OWNED);

	/*
	 * Return if "addr" is within the range of kernel page table pages
	 * that were preallocated during pmap bootstrap.  Moreover, leave
	 * "kernel_vm_end" and the kernel page table as they were.
	 *
	 * The correctness of this action is based on the following
	 * argument: vm_map_findspace() allocates contiguous ranges of the
	 * kernel virtual address space.  It calls this function if a range
	 * ends after "kernel_vm_end".  If the kernel is mapped between
	 * "kernel_vm_end" and "addr", then the range cannot begin at
	 * "kernel_vm_end".  In fact, its beginning address cannot be less
	 * than the kernel.  Thus, there is no immediate need to allocate
	 * any new kernel page table pages between "kernel_vm_end" and
	 * "KERNBASE".
	 */
	if (KERNBASE < addr && addr <= KERNBASE + nkpt * NBPDR)
		return;

	addr = roundup2(addr, NBPDR);
	if (addr - 1 >= kernel_map->max_offset)
		addr = kernel_map->max_offset;
	while (kernel_vm_end < addr) {
		pdpe = pmap_pdpe(kernel_pmap, kernel_vm_end);
		if ((*pdpe & X86_PG_V) == 0) {
			/* We need a new PDP entry */
			nkpg = vm_page_alloc(NULL, kernel_vm_end >> PDPSHIFT,
			    VM_ALLOC_INTERRUPT | VM_ALLOC_NOOBJ |
			    VM_ALLOC_WIRED | VM_ALLOC_ZERO);
			if (nkpg == NULL)
				panic("pmap_growkernel: no memory to grow kernel");
			if ((nkpg->flags & PG_ZERO) == 0)
				pmap_zero_page(nkpg);
			paddr = VM_PAGE_TO_PHYS(nkpg);

			/* First make the DMAP alias to paddr r/o */
			pmap_xen_setpages_ro(PHYS_TO_DMAP(paddr), 1);
			pde_store(pdpe,
				 (pdp_entry_t)(xpmap_ptom(paddr) | 
					       X86_PG_V | X86_PG_RW |
					       X86_PG_A | X86_PG_M |
					       X86_PG_U));

			continue; /* try again */
		}

		pde = pmap_pdpe_to_pde(pdpe, kernel_vm_end);
		if ((*pde & X86_PG_V) != 0) {
			kernel_vm_end = (kernel_vm_end + NBPDR) & ~PDRMASK;
			if (kernel_vm_end - 1 >= kernel_map->max_offset) {
				kernel_vm_end = kernel_map->max_offset;
				break;                       
			}
			continue;
		}
		nkpg = vm_page_alloc(NULL, pmap_pde_pindex(kernel_vm_end),
		    VM_ALLOC_INTERRUPT | VM_ALLOC_NOOBJ | VM_ALLOC_WIRED |
		    VM_ALLOC_ZERO);
		if (nkpg == NULL)
			panic("pmap_growkernel: no memory to grow kernel");
		if ((nkpg->flags & PG_ZERO) == 0)
			pmap_zero_page(nkpg);
		paddr = VM_PAGE_TO_PHYS(nkpg);

		/* First make the DMAP alias to paddr r/o */
		pmap_xen_setpages_ro(PHYS_TO_DMAP(paddr), 1);
		newpdir = xpmap_ptom(paddr) | X86_PG_V | X86_PG_RW | X86_PG_A | X86_PG_M | X86_PG_U;
		pde_store(pde, newpdir);

		kernel_vm_end = (kernel_vm_end + NBPDR) & ~PDRMASK;
		if (kernel_vm_end - 1 >= kernel_map->max_offset) {
			kernel_vm_end = kernel_map->max_offset;
			break;                       
		}
	}
}

/***************************************************
 * page management routines.
 ***************************************************/

CTASSERT(sizeof(struct pv_chunk) == PAGE_SIZE);
CTASSERT(_NPCM == 3);
CTASSERT(_NPCPV == 168);

static __inline struct pv_chunk *
pv_to_chunk(pv_entry_t pv)
{

	return ((struct pv_chunk *)((uintptr_t)pv & ~(uintptr_t)PAGE_MASK));
}

#define PV_PMAP(pv) (pv_to_chunk(pv)->pc_pmap)

#define	PC_FREE0	0xfffffffffffffffful
#define	PC_FREE1	0xfffffffffffffffful
#define	PC_FREE2	0x000000fffffffffful

static const uint64_t pc_freemask[_NPCM] = { PC_FREE0, PC_FREE1, PC_FREE2 };

#ifdef PV_STATS
static int pc_chunk_count, pc_chunk_allocs, pc_chunk_frees, pc_chunk_tryfail;

SYSCTL_INT(_vm_pmap, OID_AUTO, pc_chunk_count, CTLFLAG_RD, &pc_chunk_count, 0,
	"Current number of pv entry chunks");
SYSCTL_INT(_vm_pmap, OID_AUTO, pc_chunk_allocs, CTLFLAG_RD, &pc_chunk_allocs, 0,
	"Current number of pv entry chunks allocated");
SYSCTL_INT(_vm_pmap, OID_AUTO, pc_chunk_frees, CTLFLAG_RD, &pc_chunk_frees, 0,
	"Current number of pv entry chunks frees");
SYSCTL_INT(_vm_pmap, OID_AUTO, pc_chunk_tryfail, CTLFLAG_RD, &pc_chunk_tryfail, 0,
	"Number of times tried to get a chunk page but failed.");

static long pv_entry_frees, pv_entry_allocs, pv_entry_count;
static int pv_entry_spare;

SYSCTL_LONG(_vm_pmap, OID_AUTO, pv_entry_frees, CTLFLAG_RD, &pv_entry_frees, 0,
	"Current number of pv entry frees");
SYSCTL_LONG(_vm_pmap, OID_AUTO, pv_entry_allocs, CTLFLAG_RD, &pv_entry_allocs, 0,
	"Current number of pv entry allocs");
SYSCTL_LONG(_vm_pmap, OID_AUTO, pv_entry_count, CTLFLAG_RD, &pv_entry_count, 0,
	"Current number of pv entries");
SYSCTL_INT(_vm_pmap, OID_AUTO, pv_entry_spare, CTLFLAG_RD, &pv_entry_spare, 0,
	"Current number of spare pv entries");
#endif

/*
 * We are in a serious low memory condition.  Resort to
 * drastic measures to free some pages so we can allocate
 * another pv entry chunk.
 *
 * Returns NULL if PV entries were reclaimed from the specified pmap.
 *
 * We do not, however, unmap 2mpages because subsequent accesses will
 * allocate per-page pv entries until repromotion occurs, thereby
 * exacerbating the shortage of free pv entries.
 */
static vm_page_t
reclaim_pv_chunk(pmap_t locked_pmap, struct rwlock **lockp)
{
	struct pch new_tail;
	struct pv_chunk *pc;
	struct md_page *pvh;
	pd_entry_t *pde;
	pmap_t pmap;
	pt_entry_t *pte, tpte;
	pt_entry_t PG_G, PG_A, PG_M, PG_RW;
	pv_entry_t pv;
	vm_offset_t va;
	vm_page_t m, m_pc;
	struct spglist free;
	uint64_t inuse;
	int bit, field, freed;

	rw_assert(&pvh_global_lock, RA_LOCKED);
	PMAP_LOCK_ASSERT(locked_pmap, MA_OWNED);
	KASSERT(lockp != NULL, ("reclaim_pv_chunk: lockp is NULL"));
	pmap = NULL;
	m_pc = NULL;
	PG_G = PG_A = PG_M = PG_RW = 0;
	SLIST_INIT(&free);
	TAILQ_INIT(&new_tail);
	mtx_lock(&pv_chunks_mutex);
	while ((pc = TAILQ_FIRST(&pv_chunks)) != NULL && SLIST_EMPTY(&free)) {
		TAILQ_REMOVE(&pv_chunks, pc, pc_lru);
		mtx_unlock(&pv_chunks_mutex);
		if (pmap != pc->pc_pmap) {
			if (pmap != NULL) {
				pmap_invalidate_all(pmap);
				if (pmap != locked_pmap)
					PMAP_UNLOCK(pmap);
			}
			pmap = pc->pc_pmap;
			/* Avoid deadlock and lock recursion. */
			if (pmap > locked_pmap) {
				RELEASE_PV_LIST_LOCK(lockp);
				PMAP_LOCK(pmap);
			} else if (pmap != locked_pmap &&
			    !PMAP_TRYLOCK(pmap)) {
				pmap = NULL;
				TAILQ_INSERT_TAIL(&new_tail, pc, pc_lru);
				mtx_lock(&pv_chunks_mutex);
				continue;
			}
			PG_G = pmap_global_bit(pmap);
			PG_A = pmap_accessed_bit(pmap);
			PG_M = pmap_modified_bit(pmap);
			PG_RW = pmap_rw_bit(pmap);
		}

		/*
		 * Destroy every non-wired, 4 KB page mapping in the chunk.
		 */
		freed = 0;
		for (field = 0; field < _NPCM; field++) {
			for (inuse = ~pc->pc_map[field] & pc_freemask[field];
			    inuse != 0; inuse &= ~(1UL << bit)) {
				bit = bsfq(inuse);
				pv = &pc->pc_pventry[field * 64 + bit];
				va = pv->pv_va;
				pde = pmap_pde(pmap, va);
				if ((*pde & PG_PS) != 0)
					continue;
				pte = pmap_pde_to_pte(pde, va);
				if ((*pte & PG_W) != 0)
					continue;
				tpte = pte_load_clear(pte);
				if ((tpte & PG_G) != 0)
					pmap_invalidate_page(pmap, va);
				m = MACH_TO_VM_PAGE(tpte & PG_FRAME);
				if ((tpte & (PG_M | PG_RW)) == (PG_M | PG_RW))
					vm_page_dirty(m);
				if ((tpte & PG_A) != 0)
					vm_page_aflag_set(m, PGA_REFERENCED);
				CHANGE_PV_LIST_LOCK_TO_VM_PAGE(lockp, m);
				TAILQ_REMOVE(&m->md.pv_list, pv, pv_next);
				m->md.pv_gen++;
				if (TAILQ_EMPTY(&m->md.pv_list) &&
				    (m->flags & PG_FICTITIOUS) == 0) {
					pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
					if (TAILQ_EMPTY(&pvh->pv_list)) {
						vm_page_aflag_clear(m,
						    PGA_WRITEABLE);
					}
				}
				pc->pc_map[field] |= 1UL << bit;
				pmap_unuse_pt(pmap, va, *pde, &free);
				freed++;
			}
		}
		if (freed == 0) {
			TAILQ_INSERT_TAIL(&new_tail, pc, pc_lru);
			mtx_lock(&pv_chunks_mutex);
			continue;
		}
		/* Every freed mapping is for a 4 KB page. */
		pmap_resident_count_dec(pmap, freed);
		PV_STAT(atomic_add_long(&pv_entry_frees, freed));
		PV_STAT(atomic_add_int(&pv_entry_spare, freed));
		PV_STAT(atomic_subtract_long(&pv_entry_count, freed));
		TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
		if (pc->pc_map[0] == PC_FREE0 && pc->pc_map[1] == PC_FREE1 &&
		    pc->pc_map[2] == PC_FREE2) {
			PV_STAT(atomic_subtract_int(&pv_entry_spare, _NPCPV));
			PV_STAT(atomic_subtract_int(&pc_chunk_count, 1));
			PV_STAT(atomic_add_int(&pc_chunk_frees, 1));
			/* Entire chunk is free; return it. */
			m_pc = PHYS_TO_VM_PAGE(DMAP_TO_PHYS((vm_offset_t)pc));
			dump_drop_page(m_pc->phys_addr);
			mtx_lock(&pv_chunks_mutex);
			break;
		}
		TAILQ_INSERT_HEAD(&pmap->pm_pvchunk, pc, pc_list);
		TAILQ_INSERT_TAIL(&new_tail, pc, pc_lru);
		mtx_lock(&pv_chunks_mutex);
		/* One freed pv entry in locked_pmap is sufficient. */
		if (pmap == locked_pmap)
			break;
	}
	TAILQ_CONCAT(&pv_chunks, &new_tail, pc_lru);
	mtx_unlock(&pv_chunks_mutex);
	if (pmap != NULL) {
		pmap_invalidate_all(pmap);
		if (pmap != locked_pmap)
			PMAP_UNLOCK(pmap);
	}
	if (m_pc == NULL && !SLIST_EMPTY(&free)) {
		m_pc = SLIST_FIRST(&free);
		SLIST_REMOVE_HEAD(&free, plinks.s.ss);
		/* Recycle a freed page table page. */
		m_pc->wire_count = 1;
		atomic_add_int(&vm_cnt.v_wire_count, 1);
	}
	pmap_free_zero_pages(&free);
	return (m_pc);
}

/*
 * free the pv_entry back to the free list
 */
static void
free_pv_entry(pmap_t pmap, pv_entry_t pv)
{
	struct pv_chunk *pc;
	int idx, field, bit;

	rw_assert(&pvh_global_lock, RA_LOCKED);
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	PV_STAT(atomic_add_long(&pv_entry_frees, 1));
	PV_STAT(atomic_add_int(&pv_entry_spare, 1));
	PV_STAT(atomic_subtract_long(&pv_entry_count, 1));
	pc = pv_to_chunk(pv);
	idx = pv - &pc->pc_pventry[0];
	field = idx / 64;
	bit = idx % 64;
	pc->pc_map[field] |= 1ul << bit;
	if (pc->pc_map[0] != PC_FREE0 || pc->pc_map[1] != PC_FREE1 ||
	    pc->pc_map[2] != PC_FREE2) {
		/* 98% of the time, pc is already at the head of the list. */
		if (__predict_false(pc != TAILQ_FIRST(&pmap->pm_pvchunk))) {
			TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
			TAILQ_INSERT_HEAD(&pmap->pm_pvchunk, pc, pc_list);
		}
		return;
	}
	TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
	free_pv_chunk(pc);
}

static void
free_pv_chunk(struct pv_chunk *pc)
{
	vm_page_t m;

	mtx_lock(&pv_chunks_mutex);
 	TAILQ_REMOVE(&pv_chunks, pc, pc_lru);
	mtx_unlock(&pv_chunks_mutex);
	PV_STAT(atomic_subtract_int(&pv_entry_spare, _NPCPV));
	PV_STAT(atomic_subtract_int(&pc_chunk_count, 1));
	PV_STAT(atomic_add_int(&pc_chunk_frees, 1));
	/* entire chunk is free, return it */
	m = PHYS_TO_VM_PAGE(DMAP_TO_PHYS((vm_offset_t)pc));
	dump_drop_page(m->phys_addr);
	vm_page_unwire(m, 0);
	vm_page_free(m);
}

/*
 * Returns a new PV entry, allocating a new PV chunk from the system when
 * needed.  If this PV chunk allocation fails and a PV list lock pointer was
 * given, a PV chunk is reclaimed from an arbitrary pmap.  Otherwise, NULL is
 * returned.
 *
 * The given PV list lock may be released.
 */
static pv_entry_t
get_pv_entry(pmap_t pmap, struct rwlock **lockp)
{
	int bit, field;
	pv_entry_t pv;
	struct pv_chunk *pc;
	vm_page_t m;

	rw_assert(&pvh_global_lock, RA_LOCKED);
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	PV_STAT(atomic_add_long(&pv_entry_allocs, 1));
retry:
	pc = TAILQ_FIRST(&pmap->pm_pvchunk);
	if (pc != NULL) {
		for (field = 0; field < _NPCM; field++) {
			if (pc->pc_map[field]) {
				bit = bsfq(pc->pc_map[field]);
				break;
			}
		}
		if (field < _NPCM) {
			pv = &pc->pc_pventry[field * 64 + bit];
			pc->pc_map[field] &= ~(1ul << bit);
			/* If this was the last item, move it to tail */
			if (pc->pc_map[0] == 0 && pc->pc_map[1] == 0 &&
			    pc->pc_map[2] == 0) {
				TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
				TAILQ_INSERT_TAIL(&pmap->pm_pvchunk, pc,
				    pc_list);
			}
			PV_STAT(atomic_add_long(&pv_entry_count, 1));
			PV_STAT(atomic_subtract_int(&pv_entry_spare, 1));
			return (pv);
		}
	}

	/* No free items, allocate another chunk */
	m = vm_page_alloc(NULL, 0, VM_ALLOC_NORMAL | VM_ALLOC_NOOBJ |
	    VM_ALLOC_WIRED);
	if (m == NULL) {
		if (lockp == NULL) {
			PV_STAT(pc_chunk_tryfail++);
			return (NULL);
		}
		m = reclaim_pv_chunk(pmap, lockp);
		if (m == NULL)
			goto retry;
	}
	PV_STAT(atomic_add_int(&pc_chunk_count, 1));
	PV_STAT(atomic_add_int(&pc_chunk_allocs, 1));
	dump_add_page(m->phys_addr);
	pc = (void *)PHYS_TO_DMAP(m->phys_addr);
	invlpg((vm_offset_t)pc);
	pc->pc_pmap = pmap;
	pc->pc_map[0] = PC_FREE0 & ~1ul;	/* preallocated bit 0 */
	pc->pc_map[1] = PC_FREE1;
	pc->pc_map[2] = PC_FREE2;
	mtx_lock(&pv_chunks_mutex);
	TAILQ_INSERT_TAIL(&pv_chunks, pc, pc_lru);
	mtx_unlock(&pv_chunks_mutex);
	pv = &pc->pc_pventry[0];
	TAILQ_INSERT_HEAD(&pmap->pm_pvchunk, pc, pc_list);
	PV_STAT(atomic_add_long(&pv_entry_count, 1));
	PV_STAT(atomic_add_int(&pv_entry_spare, _NPCPV - 1));
	return (pv);
}

/*
 * Returns the number of one bits within the given PV chunk map element.
 */
static int
popcnt_pc_map_elem(uint64_t elem)
{
	int count;

	/*
	 * This simple method of counting the one bits performs well because
	 * the given element typically contains more zero bits than one bits.
	 */
	count = 0;
	for (; elem != 0; elem &= elem - 1)
		count++;
	return (count);
}

/*
 * Ensure that the number of spare PV entries in the specified pmap meets or
 * exceeds the given count, "needed".
 *
 * The given PV list lock may be released.
 */
static void
reserve_pv_entries(pmap_t pmap, int needed, struct rwlock **lockp)
{
	struct pch new_tail;
	struct pv_chunk *pc;
	int avail, free;
	vm_page_t m;

	rw_assert(&pvh_global_lock, RA_LOCKED);
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	KASSERT(lockp != NULL, ("reserve_pv_entries: lockp is NULL"));

	/*
	 * Newly allocated PV chunks must be stored in a private list until
	 * the required number of PV chunks have been allocated.  Otherwise,
	 * reclaim_pv_chunk() could recycle one of these chunks.  In
	 * contrast, these chunks must be added to the pmap upon allocation.
	 */
	TAILQ_INIT(&new_tail);
retry:
	avail = 0;
	TAILQ_FOREACH(pc, &pmap->pm_pvchunk, pc_list) {
		if ((cpu_feature2 & CPUID2_POPCNT) == 0) {
			free = popcnt_pc_map_elem(pc->pc_map[0]);
			free += popcnt_pc_map_elem(pc->pc_map[1]);
			free += popcnt_pc_map_elem(pc->pc_map[2]);
		} else {
			free = popcntq(pc->pc_map[0]);
			free += popcntq(pc->pc_map[1]);
			free += popcntq(pc->pc_map[2]);
		}
		if (free == 0)
			break;
		avail += free;
		if (avail >= needed)
			break;
	}
	for (; avail < needed; avail += _NPCPV) {
		m = vm_page_alloc(NULL, 0, VM_ALLOC_NORMAL | VM_ALLOC_NOOBJ |
		    VM_ALLOC_WIRED);
		if (m == NULL) {
			m = reclaim_pv_chunk(pmap, lockp);
			if (m == NULL)
				goto retry;
		}
		PV_STAT(atomic_add_int(&pc_chunk_count, 1));
		PV_STAT(atomic_add_int(&pc_chunk_allocs, 1));
		dump_add_page(m->phys_addr);
		pc = (void *)PHYS_TO_DMAP(m->phys_addr);
		pc->pc_pmap = pmap;
		pc->pc_map[0] = PC_FREE0;
		pc->pc_map[1] = PC_FREE1;
		pc->pc_map[2] = PC_FREE2;
		TAILQ_INSERT_HEAD(&pmap->pm_pvchunk, pc, pc_list);
		TAILQ_INSERT_TAIL(&new_tail, pc, pc_lru);
		PV_STAT(atomic_add_int(&pv_entry_spare, _NPCPV));
	}
	if (!TAILQ_EMPTY(&new_tail)) {
		mtx_lock(&pv_chunks_mutex);
		TAILQ_CONCAT(&pv_chunks, &new_tail, pc_lru);
		mtx_unlock(&pv_chunks_mutex);
	}
}

/*
 * First find and then remove the pv entry for the specified pmap and virtual
 * address from the specified pv list.  Returns the pv entry if found and NULL
 * otherwise.  This operation can be performed on pv lists for either 4KB or
 * 2MB page mappings.
 */
static __inline pv_entry_t
pmap_pvh_remove(struct md_page *pvh, pmap_t pmap, vm_offset_t va)
{
	pv_entry_t pv;

	rw_assert(&pvh_global_lock, RA_LOCKED);
	TAILQ_FOREACH(pv, &pvh->pv_list, pv_next) {
		if (pmap == PV_PMAP(pv) && va == pv->pv_va) {
			TAILQ_REMOVE(&pvh->pv_list, pv, pv_next);
			pvh->pv_gen++;
			break;
		}
	}
	return (pv);
}

void pmap_xen_userload(pmap_t pmap)
{
	KASSERT(pmap != kernel_pmap, 
		("Kernel pmap requested on user load.\n"));

	int i;
	for (i = 0; i < NUPML4E; i++) {
		pml4_entry_t pml4e;
		pml4e = (pmap->pm_pml4[i]);
		PT_SET_VA_MA((pml4_entry_t *)PTOV(KPML4phys) + i, pml4e, false);
	}
	PT_UPDATES_FLUSH();
	invltlb();

	/* Tell xen about user pmap switch */
	xen_pt_user_switch(pmap->pm_cr3);
}


/*
 * First find and then destroy the pv entry for the specified pmap and virtual
 * address.  This operation can be performed on pv lists for either 4KB or 2MB
 * page mappings.
 */
static void
pmap_pvh_free(struct md_page *pvh, pmap_t pmap, vm_offset_t va)
{
	pv_entry_t pv;

	pv = pmap_pvh_remove(pvh, pmap, va);
	KASSERT(pv != NULL, ("pmap_pvh_free: pv not found"));
	free_pv_entry(pmap, pv);
}

/*
 * Conditionally create the PV entry for a 4KB page mapping if the required
 * memory can be allocated without resorting to reclamation.
 */
static boolean_t
pmap_try_insert_pv_entry(pmap_t pmap, vm_offset_t va, vm_page_t m,
    struct rwlock **lockp)
{
	pv_entry_t pv;

	rw_assert(&pvh_global_lock, RA_LOCKED);
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	/* Pass NULL instead of the lock pointer to disable reclamation. */
	if ((pv = get_pv_entry(pmap, NULL)) != NULL) {
		pv->pv_va = va;
		CHANGE_PV_LIST_LOCK_TO_VM_PAGE(lockp, m);
		TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_next);
		m->md.pv_gen++;
		return (TRUE);
	} else
		return (FALSE);
}

#ifdef SMP
void pmap_lazyfix_action(void);

void
pmap_lazyfix_action(void)
{
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
}
#endif /* SMP */

/*
 *	Set the physical protection on the
 *	specified range of this map as requested.
 */

void
pmap_protect(pmap_t pmap, vm_offset_t sva, vm_offset_t eva, vm_prot_t prot)
{
	vm_offset_t va_next;
	pml4_entry_t *pml4e;
	pdp_entry_t *pdpe;
	pd_entry_t ptpaddr, *pde;
	pt_entry_t *pte, PG_G, PG_M, PG_RW, PG_V;
	boolean_t anychanged, pv_lists_locked;

	if ((prot & VM_PROT_READ) == VM_PROT_NONE) {
		pmap_remove(pmap, sva, eva);
		return;
	}

	if ((prot & (VM_PROT_WRITE|VM_PROT_EXECUTE)) ==
	    (VM_PROT_WRITE|VM_PROT_EXECUTE))
		return;

	PG_G = pmap_global_bit(pmap);
	PG_M = pmap_modified_bit(pmap);
	PG_V = pmap_valid_bit(pmap);
	PG_RW = pmap_rw_bit(pmap);
	pv_lists_locked = FALSE;
#ifdef LARGEFRAMES
resume:
#endif
	anychanged = FALSE;

	PMAP_LOCK(pmap);
	for (; sva < eva; sva = va_next) {

		pml4e = pmap_pml4e(pmap, sva);
		if ((*pml4e & PG_V) == 0) {
			va_next = (sva + NBPML4) & ~PML4MASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		pdpe = pmap_pml4e_to_pdpe(pml4e, sva);
		if ((*pdpe & PG_V) == 0) {
			va_next = (sva + NBPDP) & ~PDPMASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		va_next = (sva + NBPDR) & ~PDRMASK;
		if (va_next < sva)
			va_next = eva;

		pde = pmap_pdpe_to_pde(pdpe, sva);
		ptpaddr = *pde;

		/*
		 * Weed out invalid mappings.
		 */
		if (ptpaddr == 0 || (ptpaddr & PG_V) == 0)
			continue;

#ifdef LARGEFRAMES
		/*
		 * Check for large page.
		 */
		if ((ptpaddr & PG_PS) != 0) {
			/*
			 * Are we protecting the entire large page?  If not,
			 * demote the mapping and fall through.
			 */
			if (sva + NBPDR == va_next && eva >= va_next) {
				/*
				 * The TLB entry for a PG_G mapping is
				 * invalidated by pmap_protect_pde().
				 */
				if (pmap_protect_pde(pmap, pde, sva, prot))
					anychanged = TRUE;
				continue;
			} else {
				if (!pv_lists_locked) {
					pv_lists_locked = TRUE;
					if (!rw_try_rlock(&pvh_global_lock)) {
						if (anychanged)
							pmap_invalidate_all(
							    pmap);
						PMAP_UNLOCK(pmap);
						rw_rlock(&pvh_global_lock);
						goto resume;
					}
				}
				if (!pmap_demote_pde(pmap, pde, sva)) {
					/*
					 * The large page mapping was
					 * destroyed.
					 */
					continue;
				}
			}
		}
#endif

		if (va_next > eva)
			va_next = eva;

		for (pte = pmap_pde_to_pte(pde, sva); sva != va_next; pte++,
		    sva += PAGE_SIZE) {
			pt_entry_t obits, pbits;
			vm_page_t m;

retry:
			obits = pbits = *pte;
			if ((pbits & PG_V) == 0)
				continue;

			if ((prot & VM_PROT_WRITE) == 0) {
				if ((pbits & (PG_MANAGED | PG_M | PG_RW)) ==
				    (PG_MANAGED | PG_M | PG_RW)) {
					m = MACH_TO_VM_PAGE(pbits & PG_FRAME);
					vm_page_dirty(m);
				}
				pbits &= ~(PG_RW | PG_M);
			}
			if ((prot & VM_PROT_EXECUTE) == 0)
				pbits |= pg_nx;

			if (pbits != obits) {
				PT_SET_VA_MA(pte, pbits, true);
				if (*pte != pbits)
					goto retry;
				if (obits & PG_G)
					pmap_invalidate_page(pmap, sva);
				else
					anychanged = TRUE;
			}
		}
	}

	if (anychanged)
		pmap_invalidate_all(pmap);
	if (pv_lists_locked)
		rw_runlock(&pvh_global_lock);

	PMAP_UNLOCK(pmap);
}

/*
 *	Insert the given physical page (p) at
 *	the specified virtual address (v) in the
 *	target physical map with the protection requested.
 *
 *	If specified, the page will be wired down, meaning
 *	that the related pte can not be reclaimed.
 *
 *	NB:  This is the only routine which MAY NOT lazy-evaluate
 *	or lose information.  That is, this routine must actually
 *	insert this page into the given map NOW.
 */
void
pmap_enter(pmap_t pmap, vm_offset_t va, vm_prot_t access, vm_page_t m,
    vm_prot_t prot, boolean_t wired)
{
	struct rwlock *lock;
	pd_entry_t *pde;
	pt_entry_t *pte, PG_G, PG_A, PG_M, PG_RW, PG_V;
	pt_entry_t newpte, origpte;
	pv_entry_t pv;
	vm_paddr_t opa, pa;
	vm_page_t mpte, om;

	PG_A = pmap_accessed_bit(pmap);
	PG_G = pmap_global_bit(pmap);
	PG_M = pmap_modified_bit(pmap);
	PG_V = pmap_valid_bit(pmap);
	PG_RW = pmap_rw_bit(pmap);

	va = trunc_page(va);
	KASSERT(va <= VM_MAX_KERNEL_ADDRESS, ("pmap_enter: toobig"));
	KASSERT(va < UPT_MIN_ADDRESS || va >= UPT_MAX_ADDRESS,
	    ("pmap_enter: invalid to pmap_enter page table pages (va: 0x%lx)",
	    va));
	KASSERT((m->oflags & VPO_UNMANAGED) != 0 || va < kmi.clean_sva ||
	    va >= kmi.clean_eva,
	    ("pmap_enter: managed mapping within the clean submap"));
	if ((m->oflags & VPO_UNMANAGED) == 0 && !vm_page_xbusied(m))
		VM_OBJECT_ASSERT_WLOCKED(m->object);
	pa = VM_PAGE_TO_PHYS(m);
	newpte = (pt_entry_t)(phystomach(pa) | PG_A | PG_V);
	if ((access & VM_PROT_WRITE) != 0)
		newpte |= PG_M;
	if ((prot & VM_PROT_WRITE) != 0)
		newpte |= PG_RW;
	KASSERT((newpte & (PG_M | PG_RW)) != PG_M,
	    ("pmap_enter: access includes VM_PROT_WRITE but prot doesn't"));
	if ((prot & VM_PROT_EXECUTE) == 0)
		newpte |= pg_nx;
	if (wired)
		newpte |= PG_W;
	if (va < VM_MAXUSER_ADDRESS)
		newpte |= PG_U;
	/* On xen this is a security hole  unless you know what you're doing
	if (pmap == kernel_pmap)
		newpte |= PG_G;
	*/

	newpte |= pmap_cache_bits(pmap, m->md.pat_mode, 0);

	/*
	 * Set modified bit gratuitously for writeable mappings if
	 * the page is unmanaged. We do not want to take a fault
	 * to do the dirty bit accounting for these mappings.
	 */
	if ((m->oflags & VPO_UNMANAGED) != 0) {
		if ((newpte & PG_RW) != 0)
			newpte |= PG_M;
	}

	mpte = NULL;

	lock = NULL;
	rw_rlock(&pvh_global_lock);
	PMAP_LOCK(pmap);

	/*
	 * In the case that a page table page is not
	 * resident, we are creating it here.
	 */
retry:
	pde = pmap_pde(pmap, va);

	if (pde != NULL && (*pde & PG_V) != 0 && ((*pde & PG_PS) == 0)) // ||
		/* XXX: PG_PS: pmap_demote_pde_locked(pmap, pde, va, &lock) */ {
		pte = pmap_pde_to_pte(pde, va);
		if (va < VM_MAXUSER_ADDRESS && mpte == NULL) {
			mpte = MACH_TO_VM_PAGE(*pde & PG_FRAME);
			mpte->wire_count++;
		}
	} else if (va < VM_MAXUSER_ADDRESS) {
		/*
		 * Here if the pte page isn't mapped, or if it has been
		 * deallocated.
		 */
		mpte = _pmap_allocpte(pmap, pmap_pde_pindex(va), &lock);
		//panic(__func__);
		goto retry;
	} else
		panic("pmap_enter: invalid page directory va=%#lx", va);

	origpte = *pte;

	/*
	 * Is the specified virtual address already mapped?
	 */
	if ((origpte & PG_V) != 0) {
		/*
		 * Wiring change, just update stats. We don't worry about
		 * wiring PT pages as they remain resident as long as there
		 * are valid mappings in them. Hence, if a user page is wired,
		 * the PT page will be also.
		 */
		if ((newpte & PG_W) != 0 && (origpte & PG_W) == 0)
			pmap->pm_stats.wired_count++;
		else if ((newpte & PG_W) == 0 && (origpte & PG_W) != 0)
			pmap->pm_stats.wired_count--;

		/*
		 * Remove the extra PT page reference.
		 */
		if (mpte != NULL) {
			mpte->wire_count--;
			KASSERT(mpte->wire_count > 0,
			    ("pmap_enter: missing reference to page table page,"
			     " va: 0x%lx", va));
		}

		/*
		 * Has the physical page changed?
		 */
		opa = machtophys(origpte & PG_FRAME);
		if (opa == pa) {
			/*
			 * No, might be a protection or wiring change.
			 */
			if ((origpte & PG_MANAGED) != 0) {
				newpte |= PG_MANAGED;
				if ((newpte & PG_RW) != 0)
					vm_page_aflag_set(m, PGA_WRITEABLE);
			}
			if (((origpte ^ newpte) & ~(PG_M | PG_A)) == 0)
				goto unchanged;
			goto validate;
		}
	} else {
		/*
		 * Increment the counters.
		 */
		if ((newpte & PG_W) != 0)
			pmap->pm_stats.wired_count++;
		pmap_resident_count_inc(pmap, 1);
	}

	/*
	 * Enter on the PV list if part of our managed memory.
	 */
	if ((m->oflags & VPO_UNMANAGED) == 0) {
		newpte |= PG_MANAGED;
		pv = get_pv_entry(pmap, &lock);
		pv->pv_va = va;
		CHANGE_PV_LIST_LOCK_TO_PHYS(&lock, pa);
		TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_next);
		m->md.pv_gen++;
		if ((newpte & PG_RW) != 0)
			vm_page_aflag_set(m, PGA_WRITEABLE);
	}

	/*
	 * Update the PTE.
	 */
	if ((origpte & PG_V) != 0) {
validate:
		origpte = pte_load_store(pte, newpte);
		opa = machtophys(origpte & PG_FRAME);
		if (opa != pa) {
			if ((origpte & PG_MANAGED) != 0) {
				om = PHYS_TO_VM_PAGE(opa);
				if ((origpte & (PG_M | PG_RW)) == (PG_M |
				    PG_RW))
					vm_page_dirty(om);
				if ((origpte & PG_A) != 0)
					vm_page_aflag_set(om, PGA_REFERENCED);
				CHANGE_PV_LIST_LOCK_TO_PHYS(&lock, opa);
				pmap_pvh_free(&om->md, pmap, va);
				if ((om->aflags & PGA_WRITEABLE) != 0 &&
				    TAILQ_EMPTY(&om->md.pv_list) &&
				    ((om->flags & PG_FICTITIOUS) != 0 ||
				    TAILQ_EMPTY(&pa_to_pvh(opa)->pv_list)))
					vm_page_aflag_clear(om, PGA_WRITEABLE);
			}
		} else if ((newpte & PG_M) == 0 && (origpte & (PG_M |
		    PG_RW)) == (PG_M | PG_RW)) {
			if ((origpte & PG_MANAGED) != 0)
				vm_page_dirty(m);

			/*
			 * Although the PTE may still have PG_RW set, TLB
			 * invalidation may nonetheless be required because
			 * the PTE no longer has PG_M set.
			 */
		} else if ((origpte & PG_NX) != 0 || (newpte & PG_NX) == 0) {
			/*
			 * This PTE change does not require TLB invalidation.
			 */
			goto unchanged;
		}
		if ((origpte & PG_A) != 0)
			pmap_invalidate_page(pmap, va);
	} else {
		pte_store(pte, newpte);
		if (pmap != kernel_pmap) {
		  pmap_xen_userload(pmap); /*XXX: Move to kernel (re) entry ? */
		}
		 
	} /* XXX: remove braces */

unchanged:

	/*
	 * If both the page table page and the reservation are fully
	 * populated, then attempt promotion.
	 */
#ifdef LARGEFRAMES
	if ((mpte == NULL || mpte->wire_count == NPTEPG) &&
	    pmap_ps_enabled(pmap) &&
	    (m->flags & PG_FICTITIOUS) == 0 &&
	    vm_reserv_level_iffullpop(m) == 0)
		pmap_promote_pde(pmap, pde, va, &lock);
#endif

	if (lock != NULL)
		rw_wunlock(lock);
	rw_runlock(&pvh_global_lock);
	PMAP_UNLOCK(pmap);
}

/*
 * Maps a sequence of resident pages belonging to the same object.
 * The sequence begins with the given page m_start.  This page is
 * mapped at the given virtual address start.  Each subsequent page is
 * mapped at a virtual address that is offset from start by the same
 * amount as the page is offset from m_start within the object.  The
 * last page in the sequence is the page with the largest offset from
 * m_start that can be mapped at a virtual address less than the given
 * virtual address end.  Not every virtual page between start and end
 * is mapped; only those for which a resident page exists with the
 * corresponding offset from m_start are mapped.
 */

void
pmap_enter_object(pmap_t pmap, vm_offset_t start, vm_offset_t end,
    vm_page_t m_start, vm_prot_t prot)
{
	struct rwlock *lock;
	vm_offset_t va;
	vm_page_t m, mpte;
	vm_pindex_t diff, psize;

	VM_OBJECT_ASSERT_LOCKED(m_start->object);

	psize = atop(end - start);
	mpte = NULL;
	m = m_start;
	lock = NULL;
	rw_rlock(&pvh_global_lock);
	PMAP_LOCK(pmap);
	while (m != NULL && (diff = m->pindex - m_start->pindex) < psize) {
		va = start + ptoa(diff);
#ifdef LARGEFRAMES
		if ((va & PDRMASK) == 0 && va + NBPDR <= end &&
		    (VM_PAGE_TO_PHYS(m) & PDRMASK) == 0 &&
		    pmap_ps_enabled(pmap) &&
		    vm_reserv_level_iffullpop(m) == 0 &&
		    pmap_enter_pde(pmap, va, m, prot, &lock))
			m = &m[NBPDR / PAGE_SIZE - 1];
		else
#endif
			mpte = pmap_enter_quick_locked(pmap, va, m, prot,
			    mpte, &lock);
		m = TAILQ_NEXT(m, listq);
	}
	if (lock != NULL)
		rw_wunlock(lock);
	rw_runlock(&pvh_global_lock);
	PMAP_UNLOCK(pmap);
}

/*
 * this code makes some *MAJOR* assumptions:
 * 1. Current pmap & pmap exists.
 * 2. Not wired.
 * 3. Read access.
 * 4. No page table pages.
 * but is *MUCH* faster than pmap_enter...
 */

void
pmap_enter_quick(pmap_t pmap, vm_offset_t va, vm_page_t m, vm_prot_t prot)
{
	struct rwlock *lock;

	lock = NULL;
	rw_rlock(&pvh_global_lock);
	PMAP_LOCK(pmap);
	(void)pmap_enter_quick_locked(pmap, va, m, prot, NULL, &lock);
	if (lock != NULL)
		rw_wunlock(lock);
	rw_runlock(&pvh_global_lock);
	PMAP_UNLOCK(pmap);
}

static vm_page_t
pmap_enter_quick_locked(pmap_t pmap, vm_offset_t va, vm_page_t m,
    vm_prot_t prot, vm_page_t mpte, struct rwlock **lockp)
{
	struct spglist free;
	pt_entry_t *pte, PG_V;
	vm_paddr_t pa;

	KASSERT(va < kmi.clean_sva || va >= kmi.clean_eva ||
	    (m->oflags & VPO_UNMANAGED) != 0,
	    ("pmap_enter_quick_locked: managed mapping within the clean submap"));
	PG_V = pmap_valid_bit(pmap);
	rw_assert(&pvh_global_lock, RA_LOCKED);
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);

	/*
	 * In the case that a page table page is not
	 * resident, we are creating it here.
	 */
	if (va < VM_MAXUSER_ADDRESS) {
		vm_pindex_t ptepindex;
		pd_entry_t *ptepa;

		/*
		 * Calculate pagetable page index
		 */
		ptepindex = pmap_pde_pindex(va);
		if (mpte && (mpte->pindex == ptepindex)) {
			mpte->wire_count++;
		} else {
			/*
			 * Get the page directory entry
			 */
			ptepa = pmap_pde(pmap, va);

			/*
			 * If the page table page is mapped, we just increment
			 * the hold count, and activate it.  Otherwise, we
			 * attempt to allocate a page table page.  If this
			 * attempt fails, we don't retry.  Instead, we give up.
			 */
			if (ptepa && (*ptepa & PG_V) != 0) {
				if (*ptepa & PG_PS)
					return (NULL);
				mpte = MACH_TO_VM_PAGE(*ptepa & PG_FRAME);
				mpte->wire_count++;
			} else {
				/*
				 * Pass NULL instead of the PV list lock
				 * pointer, because we don't intend to sleep.
				 */
				mpte = _pmap_allocpte(pmap, ptepindex, NULL);
				if (mpte == NULL)
					return (mpte);
			}
		}
		pte = (pt_entry_t *)PHYS_TO_DMAP(VM_PAGE_TO_PHYS(mpte));
		pte = &pte[pmap_pte_index(va)];
	} else {
		mpte = NULL;
		pte = vtopte(va);
	}
	if (*pte) {
		if (mpte != NULL) {
			mpte->wire_count--;
			mpte = NULL;
		}
		return (mpte);
	}

	/*
	 * Enter on the PV list if part of our managed memory.
	 */
	if ((m->oflags & VPO_UNMANAGED) == 0 &&
	    !pmap_try_insert_pv_entry(pmap, va, m, lockp)) {
		if (mpte != NULL) {
			SLIST_INIT(&free);
			if (pmap_unwire_ptp(pmap, va, mpte, &free)) {
				pmap_invalidate_page(pmap, va);
				pmap_free_zero_pages(&free);
			}
			mpte = NULL;
		}
		return (mpte);
	}

	/*
	 * Increment counters
	 */
	pmap_resident_count_inc(pmap, 1);

	pa = VM_PAGE_TO_PHYS(m) | pmap_cache_bits(pmap, m->md.pat_mode, 0);
	if ((prot & VM_PROT_EXECUTE) == 0)
		pa |= pg_nx;

	/*
	 * Now validate mapping with RO protection
	 */
	if ((m->oflags & VPO_UNMANAGED) != 0) {
		pte_store(pte, phystomach(pa) | PG_V | PG_U);
	}
	else {
		pte_store(pte, phystomach(pa) | PG_V | PG_U | PG_MANAGED);
	}
	return (mpte);
}

void *
pmap_kenter_temporary(vm_paddr_t pa, int i)
{
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
	return NULL;
}

void
pmap_object_init_pt(pmap_t pmap, vm_offset_t addr,
		    vm_object_t object, vm_pindex_t pindex,
		    vm_size_t size)
{
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
}

#ifdef LARGEFRAMES
/*
 * pmap_remove_pde: do the things to unmap a superpage in a process
 */
static int
pmap_remove_pde(pmap_t pmap, pd_entry_t *pdq, vm_offset_t sva,
    struct spglist *free, struct rwlock **lockp)
{
	struct md_page *pvh;
	pd_entry_t oldpde;
	vm_offset_t eva, va;
	vm_page_t m, mpte;
	pt_entry_t PG_G, PG_A, PG_M, PG_RW;

	PG_G = pmap_global_bit(pmap);
	PG_A = pmap_accessed_bit(pmap);
	PG_M = pmap_modified_bit(pmap);
	PG_RW = pmap_rw_bit(pmap);

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	KASSERT((sva & PDRMASK) == 0,
	    ("pmap_remove_pde: sva is not 2mpage aligned"));
	oldpde = pte_load_clear(pdq);
	if (oldpde & PG_W)
		pmap->pm_stats.wired_count -= NBPDR / PAGE_SIZE;

	/*
	 * Machines that don't support invlpg, also don't support
	 * PG_G.
	 */
	if (oldpde & PG_G)
		pmap_invalidate_page(kernel_pmap, sva);
	pmap_resident_count_dec(pmap, NBPDR / PAGE_SIZE);
	if (oldpde & PG_MANAGED) {
		CHANGE_PV_LIST_LOCK_TO_PHYS(lockp, machtophys(oldpde & PG_PS_FRAME));
		pvh = pa_to_pvh(machtophys(oldpde & PG_PS_FRAME));
		pmap_pvh_free(pvh, pmap, sva);
		eva = sva + NBPDR;
		for (va = sva, m = MACH_TO_VM_PAGE(oldpde & PG_PS_FRAME);
		    va < eva; va += PAGE_SIZE, m++) {
			if ((oldpde & (PG_M | PG_RW)) == (PG_M | PG_RW))
				vm_page_dirty(m);
			if (oldpde & PG_A)
				vm_page_aflag_set(m, PGA_REFERENCED);
			if (TAILQ_EMPTY(&m->md.pv_list) &&
			    TAILQ_EMPTY(&pvh->pv_list))
				vm_page_aflag_clear(m, PGA_WRITEABLE);
		}
	}
	if (pmap == kernel_pmap) {
		pmap_remove_kernel_pde(pmap, pdq, sva);
	} else {
		mpte = pmap_lookup_pt_page(pmap, sva);
		if (mpte != NULL) {
			pmap_remove_pt_page(pmap, mpte);
			pmap_resident_count_dec(pmap, 1);
			KASSERT(mpte->wire_count == NPTEPG,
			    ("pmap_remove_pde: pte page wire count error"));
			mpte->wire_count = 0;
			pmap_add_delayed_free_list(mpte, free, FALSE);
			atomic_subtract_int(&vm_cnt.v_wire_count, 1);
		}
	}
	return (pmap_unuse_pt(pmap, sva, *pmap_pdpe(pmap, sva), free));
}

#endif

/*
 * pmap_remove_pte: do the things to unmap a page in a process
 */
static int
pmap_remove_pte(pmap_t pmap, pt_entry_t *ptq, vm_offset_t va, 
    pd_entry_t ptepde, struct spglist *free, struct rwlock **lockp)
{
	struct md_page *pvh;
	pt_entry_t oldpte, PG_A, PG_M, PG_RW;
	vm_page_t m;

	PG_A = pmap_accessed_bit(pmap);
	PG_M = pmap_modified_bit(pmap);
	PG_RW = pmap_rw_bit(pmap);

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	oldpte = pte_load_clear(ptq);
	if (oldpte & PG_W)
		pmap->pm_stats.wired_count -= 1;
	pmap_resident_count_dec(pmap, 1);
	if (oldpte & PG_MANAGED) {
		m = MACH_TO_VM_PAGE(oldpte & PG_FRAME);
		if ((oldpte & (PG_M | PG_RW)) == (PG_M | PG_RW))
			vm_page_dirty(m);
		if (oldpte & PG_A)
			vm_page_aflag_set(m, PGA_REFERENCED);
		CHANGE_PV_LIST_LOCK_TO_VM_PAGE(lockp, m);
		pmap_pvh_free(&m->md, pmap, va);
		if (TAILQ_EMPTY(&m->md.pv_list) &&
		    (m->flags & PG_FICTITIOUS) == 0) {
			pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
			if (TAILQ_EMPTY(&pvh->pv_list))
				vm_page_aflag_clear(m, PGA_WRITEABLE);
		}
	}
	return (pmap_unuse_pt(pmap, va, ptepde, free));
}

/*
 * Remove a single page from a process address space
 */
static void
pmap_remove_page(pmap_t pmap, vm_offset_t va, pd_entry_t *pde,
    struct spglist *free)
{
	struct rwlock *lock;
	pt_entry_t *pte, PG_V;

	PG_V = pmap_valid_bit(pmap);
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	if ((*pde & PG_V) == 0)
		return;
	pte = pmap_pde_to_pte(pde, va);
	if ((*pte & PG_V) == 0)
		return;
	lock = NULL;
	pmap_remove_pte(pmap, pte, va, *pde, free, &lock);
	if (lock != NULL)
		rw_wunlock(lock);
	pmap_invalidate_page(pmap, va);
}

/*
 *	Remove the given range of addresses from the specified map.
 *
 *	It is assumed that the start and end are properly
 *	rounded to the page size.
 */
void
pmap_remove(pmap_t pmap, vm_offset_t sva, vm_offset_t eva)
{
	struct rwlock *lock;
	vm_offset_t va, va_next;
	pml4_entry_t *pml4e;
	pdp_entry_t *pdpe;
	pd_entry_t ptpaddr, *pde;
	pt_entry_t *pte, PG_G, PG_V;
	struct spglist free;
	int anyvalid;

	PG_G = pmap_global_bit(pmap);
	PG_V = pmap_valid_bit(pmap);

	/*
	 * Perform an unsynchronized read.  This is, however, safe.
	 */
	if (pmap->pm_stats.resident_count == 0)
		return;

	anyvalid = 0;
	SLIST_INIT(&free);

	rw_rlock(&pvh_global_lock);
	PMAP_LOCK(pmap);

	/*
	 * special handling of removing one page.  a very
	 * common operation and easy to short circuit some
	 * code.
	 */
	if (sva + PAGE_SIZE == eva) {
		pde = pmap_pde(pmap, sva);
		if (pde && (*pde & PG_PS) == 0) {
			pmap_remove_page(pmap, sva, pde, &free);
			goto out;
		}
	}

	lock = NULL;
	for (; sva < eva; sva = va_next) {

		if (pmap->pm_stats.resident_count == 0)
			break;

		pml4e = pmap_pml4e(pmap, sva);
		if ((*pml4e & PG_V) == 0) {
			va_next = (sva + NBPML4) & ~PML4MASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		pdpe = pmap_pml4e_to_pdpe(pml4e, sva);
		if ((*pdpe & PG_V) == 0) {
			va_next = (sva + NBPDP) & ~PDPMASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}

		/*
		 * Calculate index for next page table.
		 */
		va_next = (sva + NBPDR) & ~PDRMASK;
		if (va_next < sva)
			va_next = eva;

		pde = pmap_pdpe_to_pde(pdpe, sva);
		ptpaddr = *pde;

		/*
		 * Weed out invalid mappings.
		 */
		if (ptpaddr == 0)
			continue;

#ifdef LARGEFRAMES
		/*
		 * Check for large page.
		 */
		if ((ptpaddr & PG_PS) != 0) {
			/*
			 * Are we removing the entire large page?  If not,
			 * demote the mapping and fall through.
			 */
			if (sva + NBPDR == va_next && eva >= va_next) {
				/*
				 * The TLB entry for a PG_G mapping is
				 * invalidated by pmap_remove_pde().
				 */
				if ((ptpaddr & PG_G) == 0)
					anyvalid = 1;
				pmap_remove_pde(pmap, pde, sva, &free, &lock);
				continue;
			} else if (!pmap_demote_pde_locked(pmap, pde, sva,
			    &lock)) {
				/* The large page mapping was destroyed. */
				continue;
			} else
				ptpaddr = *pde;
		}
#endif
		/*
		 * Limit our scan to either the end of the va represented
		 * by the current page table page, or to the end of the
		 * range being removed.
		 */
		if (va_next > eva)
			va_next = eva;

		va = va_next;
		for (pte = pmap_pde_to_pte(pde, sva); sva != va_next; pte++,
		    sva += PAGE_SIZE) {
			if (*pte == 0) {
				if (va != va_next) {
					pmap_invalidate_range(pmap, va, sva);
					va = va_next;
				}
				continue;
			}
			if ((*pte & PG_G) == 0)
				anyvalid = 1;
			else if (va == va_next)
				va = sva;
			if (pmap_remove_pte(pmap, pte, sva, ptpaddr, &free,
			    &lock)) {
				sva += PAGE_SIZE;
				break;
			}
		}
		if (va != va_next)
			pmap_invalidate_range(pmap, va, sva);
	}
	if (lock != NULL)
		rw_wunlock(lock);
out:
	if (anyvalid)
		pmap_invalidate_all(pmap);
	rw_runlock(&pvh_global_lock);	
	PMAP_UNLOCK(pmap);
	pmap_free_zero_pages(&free);
}

/*
 *	Routine:	pmap_remove_all
 *	Function:
 *		Removes this physical page from
 *		all physical maps in which it resides.
 *		Reflects back modify bits to the pager.
 *
 *	Notes:
 *		Original versions of this routine were very
 *		inefficient because they iteratively called
 *		pmap_remove (slow...)
 */

void
pmap_remove_all(vm_page_t m)
{
	struct md_page *pvh;
	pv_entry_t pv;
	pmap_t pmap;
	pt_entry_t *pte, tpte, PG_A, PG_M, PG_RW;
	pd_entry_t *pde;
#ifdef LARGEFRAMES
	vm_offset_t va;
#endif
	struct spglist free;

	KASSERT((m->oflags & VPO_UNMANAGED) == 0,
	    ("pmap_remove_all: page %p is not managed", m));
	SLIST_INIT(&free);
	rw_wlock(&pvh_global_lock);
	if ((m->flags & PG_FICTITIOUS) != 0)
		goto small_mappings;
	pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
#ifdef LARGEFRAMES
	while ((pv = TAILQ_FIRST(&pvh->pv_list)) != NULL) {
		pmap = PV_PMAP(pv);
		PMAP_LOCK(pmap);
		va = pv->pv_va;
		pde = pmap_pde(pmap, va);
		(void)pmap_demote_pde(pmap, pde, va);
		PMAP_UNLOCK(pmap);
	}
#endif
small_mappings:
	while ((pv = TAILQ_FIRST(&m->md.pv_list)) != NULL) {
		pmap = PV_PMAP(pv);
		PMAP_LOCK(pmap);
		PG_A = pmap_accessed_bit(pmap);
		PG_M = pmap_modified_bit(pmap);
		PG_RW = pmap_rw_bit(pmap);
		pmap_resident_count_dec(pmap, 1);
		pde = pmap_pde(pmap, pv->pv_va);
		KASSERT((*pde & PG_PS) == 0, ("pmap_remove_all: found"
		    " a 2mpage in page %p's pv list", m));
		pte = pmap_pde_to_pte(pde, pv->pv_va);
		tpte = pte_load_clear(pte);
		if (tpte & PG_W)
			pmap->pm_stats.wired_count--;
		if (tpte & PG_A)
			vm_page_aflag_set(m, PGA_REFERENCED);

		/*
		 * Update the vm_page_t clean and reference bits.
		 */
		if ((tpte & (PG_M | PG_RW)) == (PG_M | PG_RW))
			vm_page_dirty(m);
		pmap_unuse_pt(pmap, pv->pv_va, *pde, &free);
		pmap_invalidate_page(pmap, pv->pv_va);
		TAILQ_REMOVE(&m->md.pv_list, pv, pv_next);
		m->md.pv_gen++;
		free_pv_entry(pmap, pv);
		PMAP_UNLOCK(pmap);
	}
	vm_page_aflag_clear(m, PGA_WRITEABLE);
	rw_wunlock(&pvh_global_lock);
	pmap_free_zero_pages(&free);
}

void
pmap_change_wiring(pmap_t pmap, vm_offset_t va, boolean_t wired)
{
	/* 
	 * Nothing to do - page table backing pages are the only pages
	 * that are implicitly "wired". These are managed by uma(9).
	 */
}

/*
 *	Copy the range specified by src_addr/len
 *	from the source map to the range dst_addr/len
 *	in the destination map.
 *
 *	This routine is only advisory and need not do anything.
 */

void
pmap_copy(pmap_t dst_pmap, pmap_t src_pmap, vm_offset_t dst_addr, 
	  vm_size_t len, vm_offset_t src_addr)
{
	struct rwlock *lock;
	struct spglist free;
	vm_offset_t addr;
	vm_offset_t end_addr = src_addr + len;
	vm_offset_t va_next;
	pt_entry_t PG_A, PG_M, PG_V;


	if (dst_addr != src_addr)
		return;

	if (dst_pmap->pm_type != src_pmap->pm_type)
		return;

	/*
	 * EPT page table entries that require emulation of A/D bits are
	 * sensitive to clearing the PG_A bit (aka EPT_PG_READ). Although
	 * we clear PG_M (aka EPT_PG_WRITE) concomitantly, the PG_U bit
	 * (aka EPT_PG_EXECUTE) could still be set. Since some EPT
	 * implementations flag an EPT misconfiguration for exec-only
	 * mappings we skip this function entirely for emulated pmaps.
	 */
	if (pmap_emulate_ad_bits(dst_pmap))
		return;

	lock = NULL;

	rw_rlock(&pvh_global_lock);

	if (dst_pmap < src_pmap) {
		PMAP_LOCK(dst_pmap);
		PMAP_LOCK(src_pmap);
	} else {
		PMAP_LOCK(src_pmap);
		PMAP_LOCK(dst_pmap);
	}


	PG_A = pmap_accessed_bit(dst_pmap);
	PG_M = pmap_modified_bit(dst_pmap);
	PG_V = pmap_valid_bit(dst_pmap);

	for (addr = src_addr; addr < end_addr; addr = va_next) {
		pt_entry_t *src_pte, *dst_pte;
		vm_page_t /* dstmpde,  */dstmpte, srcmpte;
		pml4_entry_t *pml4e;
		pdp_entry_t *pdpe;
		pd_entry_t srcptepaddr, *pde;

		KASSERT(addr < UPT_MIN_ADDRESS,
		    ("pmap_copy: invalid to pmap_copy page tables"));

		pml4e = pmap_pml4e(src_pmap, addr);
		if ((*pml4e & PG_V) == 0) {
			va_next = (addr + NBPML4) & ~PML4MASK;
			if (va_next < addr)
				va_next = end_addr;
			continue;
		}

		pdpe = pmap_pml4e_to_pdpe(pml4e, addr);
		if ((*pdpe & PG_V) == 0) {
			va_next = (addr + NBPDP) & ~PDPMASK;
			if (va_next < addr)
				va_next = end_addr;
			continue;
		}

		va_next = (addr + NBPDR) & ~PDRMASK;
		if (va_next < addr)
			va_next = end_addr;

		pde = pmap_pdpe_to_pde(pdpe, addr);

		if ((*pde & PG_V) == 0) {
		  continue;
		}

		srcptepaddr = *pde;

		if ((srcptepaddr & PG_FRAME) == 0)
			continue;
			
#ifdef LARGEFRAMES
		if (srcptepaddr & PG_PS) {
			if ((addr & PDRMASK) != 0 || addr + NBPDR > end_addr)
				continue;
			dstmpde = pmap_allocpde(dst_pmap, addr, NULL);
			if (dstmpde == NULL)
				break;
			pde = (pd_entry_t *)
			    PHYS_TO_DMAP(VM_PAGE_TO_PHYS(dstmpde));
			pde = &pde[pmap_pde_index(addr)];
			if (*pde == 0 && ((srcptepaddr & PG_MANAGED) == 0 ||
			    pmap_pv_insert_pde(dst_pmap, addr, srcptepaddr &
			    PG_PS_FRAME, &lock))) {
				*pde = srcptepaddr & ~PG_W;
				pmap_resident_count_inc(dst_pmap, NBPDR / PAGE_SIZE);
			} else
				dstmpde->wire_count--;
			continue;
		}
#endif

		srcptepaddr &= PG_FRAME;

		srcmpte = MACH_TO_VM_PAGE(srcptepaddr);

		KASSERT(srcmpte->wire_count > 0,
		    ("pmap_copy: source page table page is unused"));

		if (va_next > end_addr)
			va_next = end_addr;

		src_pte = (pt_entry_t *)MACH_TO_DMAP(srcptepaddr);
		src_pte = &src_pte[pmap_pte_index(addr)];
		dstmpte = NULL;
		while (addr < va_next) {
			pt_entry_t ptetemp;
			ptetemp = *src_pte;
			/*
			 * we only virtual copy managed pages
			 */
			if ((ptetemp & PG_MANAGED) != 0) {
				if (dstmpte != NULL &&
				    dstmpte->pindex == pmap_pde_pindex(addr))
					dstmpte->wire_count++;
				else if ((dstmpte = pmap_allocpte(dst_pmap,
				    addr, NULL)) == NULL)
					goto out;
				dst_pte = (pt_entry_t *)
				    PHYS_TO_DMAP(VM_PAGE_TO_PHYS(dstmpte));
				dst_pte = &dst_pte[pmap_pte_index(addr)];
				if (*dst_pte == 0 &&
				    pmap_try_insert_pv_entry(dst_pmap, addr,
				    MACH_TO_VM_PAGE(ptetemp & PG_FRAME),
				    &lock)) {
					/*
					 * Clear the wired, modified, and
					 * accessed (referenced) bits
					 * during the copy.
					 */

					PT_SET_VA_MA(dst_pte, ptetemp & 
						     ~(PG_W | PG_M | PG_A),
						     true);

					pmap_resident_count_inc(dst_pmap, 1);
				} else {
					SLIST_INIT(&free);
					if (pmap_unwire_ptp(dst_pmap, addr,
					    dstmpte, &free)) {
						pmap_invalidate_page(dst_pmap,
						    addr);
						pmap_free_zero_pages(&free);
					}
					goto out;
				}
				if (dstmpte->wire_count >= srcmpte->wire_count)
					break;
			}
			addr += PAGE_SIZE;
			src_pte++;
		}
	}
out:
	if (lock != NULL)
		rw_wunlock(lock);
	rw_runlock(&pvh_global_lock);
	PMAP_UNLOCK(src_pmap);
	PMAP_UNLOCK(dst_pmap);
}

void
pmap_copy_page(vm_page_t msrc, vm_page_t mdst)
{
	vm_offset_t ma_src, ma_dst;
	vm_offset_t va_src, va_dst;

	KASSERT(msrc != NULL && mdst != NULL,
		("Invalid source or destination page!"));

	va_src = kva_alloc(PAGE_SIZE * 2);
	va_dst = va_src + PAGE_SIZE;

	KASSERT(va_src != 0,
		("Out of kernel virtual space!"));

	ma_src = VM_PAGE_TO_MACH(msrc);
	ma_dst = VM_PAGE_TO_MACH(mdst);

	pmap_kenter_ma(va_src, ma_src);
	pmap_kenter_ma(va_dst, ma_dst);

	pagecopy((void *)va_src, (void *)va_dst);

	pmap_kremove(va_src);
	pmap_kremove(va_dst);

	kva_free(va_src, PAGE_SIZE * 2);
}

int unmapped_buf_allowed = 1;

void
pmap_copy_pages(vm_page_t ma[], vm_offset_t a_offset, vm_page_t mb[],
    vm_offset_t b_offset, int xfersize)
{
	void *a_cp, *b_cp;
	vm_offset_t a_pg, b_pg;
	vm_offset_t a_pg_offset, b_pg_offset;
	int cnt;

	a_pg = kva_alloc(PAGE_SIZE * 2);
	b_pg = a_pg + PAGE_SIZE;

	KASSERT(a_pg != 0,
		("Out of kernel virtual space!"));

	while (xfersize > 0) {
		a_pg_offset = a_offset & PAGE_MASK;
		cnt = min(xfersize, PAGE_SIZE - a_pg_offset);
		pmap_kenter_ma(a_pg, 
			VM_PAGE_TO_MACH(ma[a_offset >> PAGE_SHIFT]));
		a_cp = (char *)a_pg + a_pg_offset;

		b_pg_offset = b_offset & PAGE_MASK;
		cnt = min(cnt, PAGE_SIZE - b_pg_offset);
		pmap_kenter_ma(b_pg,
			VM_PAGE_TO_MACH(mb[b_offset >> PAGE_SHIFT]));
		b_cp = (char *)b_pg + b_pg_offset;
		bcopy(a_cp, b_cp, cnt);
		a_offset += cnt;
		b_offset += cnt;
		xfersize -= cnt;
	}

	pmap_kremove(a_pg);
	pmap_kremove(b_pg);

	kva_free(a_pg, PAGE_SIZE * 2);
}

/*
 *	pmap_zero_page zeros the specified hardware page by mapping
 *	the page into KVM and using bzero to clear its contents.
 */
void
pmap_zero_page(vm_page_t m)
{
	vm_offset_t va = PHYS_TO_DMAP(VM_PAGE_TO_PHYS(m));

	pagezero((void *)va);
}

/*
 *	pmap_zero_page_area zeros the specified hardware page by mapping 
 *	the page into KVM and using bzero to clear its contents.
 *
 *	off and size may not cover an area beyond a single hardware page.
 */
void
pmap_zero_page_area(vm_page_t m, int off, int size)
{
	vm_offset_t va = PHYS_TO_DMAP(VM_PAGE_TO_PHYS(m));

	if (off == 0 && size == PAGE_SIZE)
		pagezero((void *)va);
	else
		bzero((char *)va + off, size);
}

void
pmap_zero_page_idle(vm_page_t m)
{
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
}

void
pmap_activate(struct thread *td)
{
	pmap_t	pmap, oldpmap;
	u_int	cpuid;

	critical_enter();
	pmap = vmspace_pmap(td->td_proc->p_vmspace);
	oldpmap = PCPU_GET(curpmap);
	cpuid = PCPU_GET(cpuid);
#ifdef SMP
	CPU_CLR_ATOMIC(cpuid, &oldpmap->pm_active);
	CPU_SET_ATOMIC(cpuid, &pmap->pm_active);
	CPU_SET_ATOMIC(cpuid, &pmap->pm_save);
#else
	CPU_CLR(cpuid, &oldpmap->pm_active);
	CPU_SET(cpuid, &pmap->pm_active);
	CPU_SET(cpuid, &pmap->pm_save);
#endif
	td->td_pcb->pcb_cr3 = pmap->pm_cr3;
	if (__predict_false(pmap == kernel_pmap)) {
		load_cr3(pmap->pm_cr3);
	}
	else {
		pmap_xen_userload(pmap);
	}

	PCPU_SET(curpmap, pmap);
	critical_exit();
}

/*
 * Destroy all managed, non-wired mappings in the given user-space
 * pmap.  This pmap cannot be active on any processor besides the
 * caller.
 *                                                                                
 * This function cannot be applied to the kernel pmap.  Moreover, it
 * is not intended for general use.  It is only to be used during
 * process termination.  Consequently, it can be implemented in ways
 * that make it faster than pmap_remove().  First, it can more quickly
 * destroy mappings by iterating over the pmap's collection of PV
 * entries, rather than searching the page table.  Second, it doesn't
 * have to test and clear the page table entries atomically, because
 * no processor is currently accessing the user address space.  In
 * particular, a page table entry's dirty bit won't change state once
 * this function starts.
 */
void
pmap_remove_pages(pmap_t pmap)
{
	pd_entry_t ptepde;
	pt_entry_t *pte, tpte;
	pt_entry_t PG_M, PG_RW, PG_V;
	struct spglist free;
	vm_page_t m, mpte, mt;
	pv_entry_t pv;
	struct md_page *pvh;
	struct pv_chunk *pc, *npc;
	struct rwlock *lock;
	int64_t bit;
	uint64_t inuse, bitmask;
	int allfree, field, freed, idx;
	boolean_t superpage;
	vm_paddr_t pa;

	/*
	 * Assert that the given pmap is only active on the current
	 * CPU.  Unfortunately, we cannot block another CPU from
	 * activating the pmap while this function is executing.
	 */
	KASSERT(pmap == PCPU_GET(curpmap), ("non-current pmap %p", pmap));
#if 0 /* XXX */
#ifdef INVARIANTS
	{
		cpuset_t other_cpus;

		other_cpus = all_cpus;
		critical_enter();
		CPU_CLR(PCPU_GET(cpuid), &other_cpus);
		CPU_AND(&other_cpus, &pmap->pm_active);
		critical_exit();
		KASSERT(CPU_EMPTY(&other_cpus), ("pmap active %p", pmap));
	}
#endif
#endif

	lock = NULL;
	PG_M = pmap_modified_bit(pmap);
	PG_V = pmap_valid_bit(pmap);
	PG_RW = pmap_rw_bit(pmap);

	SLIST_INIT(&free);
	rw_rlock(&pvh_global_lock);
	PMAP_LOCK(pmap);
	TAILQ_FOREACH_SAFE(pc, &pmap->pm_pvchunk, pc_list, npc) {
		allfree = 1;
		freed = 0;
		for (field = 0; field < _NPCM; field++) {
			inuse = ~pc->pc_map[field] & pc_freemask[field];
			while (inuse != 0) {
				bit = bsfq(inuse);
				bitmask = 1UL << bit;
				idx = field * 64 + bit;
				pv = &pc->pc_pventry[idx];
				inuse &= ~bitmask;

				pte = pmap_pdpe(pmap, pv->pv_va);
				ptepde = *pte;
				pte = pmap_pdpe_to_pde(pte, pv->pv_va);
				tpte = *pte;
				if ((tpte & (PG_PS | PG_V)) == PG_V) {
					superpage = FALSE;
					ptepde = tpte;
					pte = (pt_entry_t *)PHYS_TO_DMAP(tpte &
					    PG_FRAME);
					pte = &pte[pmap_pte_index(pv->pv_va)];
					tpte = *pte;
				} else {
					/*
					 * Keep track whether 'tpte' is a
					 * superpage explicitly instead of
					 * relying on PG_PS being set.
					 *
					 * This is because PG_PS is numerically
					 * identical to PG_PTE_PAT and thus a
					 * regular page could be mistaken for
					 * a superpage.
					 */
					superpage = TRUE;
				}

				if ((tpte & PG_V) == 0) {
					panic("bad pte va %lx pte %lx",
					    pv->pv_va, tpte);
				}

/*
 * We cannot remove wired pages from a process' mapping at this time
 */
				if (tpte & PG_W) {
					allfree = 0;
					continue;
				}

				if (superpage)
					pa = machtophys(tpte & PG_PS_FRAME);
				else
					pa = machtophys(tpte & PG_FRAME);

				m = PHYS_TO_VM_PAGE(pa);
				KASSERT(m->phys_addr == pa,
				    ("vm_page_t %p phys_addr mismatch %016jx %016jx",
				    m, (uintmax_t)m->phys_addr,
				    (uintmax_t)tpte));

				KASSERT((m->flags & PG_FICTITIOUS) != 0 ||
				    m < &vm_page_array[vm_page_array_size],
				    ("pmap_remove_pages: bad tpte %#jx",
				    (uintmax_t)tpte));

				pte_clear(pte);

				/*
				 * Update the vm_page_t clean/reference bits.
				 */
				if ((tpte & (PG_M | PG_RW)) == (PG_M | PG_RW)) {
					if (superpage) {
						for (mt = m; mt < &m[NBPDR / PAGE_SIZE]; mt++)
							vm_page_dirty(mt);
					} else
						vm_page_dirty(m);
				}

				CHANGE_PV_LIST_LOCK_TO_VM_PAGE(&lock, m);

				/* Mark free */
				pc->pc_map[field] |= bitmask;
				if (superpage) {
					pmap_resident_count_dec(pmap, NBPDR / PAGE_SIZE);
					pvh = pa_to_pvh(machtophys(tpte & PG_PS_FRAME));
					TAILQ_REMOVE(&pvh->pv_list, pv, pv_next);
					pvh->pv_gen++;
					if (TAILQ_EMPTY(&pvh->pv_list)) {
						for (mt = m; mt < &m[NBPDR / PAGE_SIZE]; mt++)
							if ((mt->aflags & PGA_WRITEABLE) != 0 &&
							    TAILQ_EMPTY(&mt->md.pv_list))
								vm_page_aflag_clear(mt, PGA_WRITEABLE);
					}
					mpte = pmap_lookup_pt_page(pmap, pv->pv_va);
					if (mpte != NULL) {
						pmap_remove_pt_page(pmap, mpte);
						pmap_resident_count_dec(pmap, 1);
						KASSERT(mpte->wire_count == NPTEPG,
						    ("pmap_remove_pages: pte page wire count error"));
						mpte->wire_count = 0;
						pmap_add_delayed_free_list(mpte, &free, FALSE);
						atomic_subtract_int(&vm_cnt.v_wire_count, 1);
					}
				} else {
					pmap_resident_count_dec(pmap, 1);
					TAILQ_REMOVE(&m->md.pv_list, pv, pv_next);
					m->md.pv_gen++;
					if ((m->aflags & PGA_WRITEABLE) != 0 &&
					    TAILQ_EMPTY(&m->md.pv_list) &&
					    (m->flags & PG_FICTITIOUS) == 0) {
						pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
						if (TAILQ_EMPTY(&pvh->pv_list))
							vm_page_aflag_clear(m, PGA_WRITEABLE);
					}
				}
				pmap_unuse_pt(pmap, pv->pv_va, ptepde, &free);
				freed++;
			}
		}
		PV_STAT(atomic_add_long(&pv_entry_frees, freed));
		PV_STAT(atomic_add_int(&pv_entry_spare, freed));
		PV_STAT(atomic_subtract_long(&pv_entry_count, freed));
		if (allfree) {
			TAILQ_REMOVE(&pmap->pm_pvchunk, pc, pc_list);
			free_pv_chunk(pc);
		}
	}
	if (lock != NULL)
		rw_wunlock(lock);
	pmap_invalidate_all(pmap);
	rw_runlock(&pvh_global_lock);
	PMAP_UNLOCK(pmap);
	pmap_free_zero_pages(&free);
}

void
pmap_page_set_memattr(vm_page_t m, vm_memattr_t ma)
{
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
}

static bool
pv_dummy(pmap_t pmap, vm_offset_t va, vm_page_t m)
{
	return true; /* stop at the first iteration */
}

boolean_t
pmap_page_is_mapped(vm_page_t m)
{

	if ((m->oflags & VPO_UNMANAGED) != 0)
		return (FALSE);

	return pmap_pv_iterate(m, pv_dummy, PV_RO_ITERATE);
}

boolean_t
pmap_page_exists_quick(pmap_t pmap, vm_page_t m)
{
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
	return 0;
}


/*
 *	pmap_page_wired_mappings:
 *
 *	Return the number of managed mappings to the given physical page
 *	that are wired.
 */
int
pmap_page_wired_mappings(vm_page_t m)
{
	struct rwlock *lock;
	struct md_page *pvh;
	pmap_t pmap;
	pt_entry_t *pte;
	pv_entry_t pv;
	int count, md_gen, pvh_gen;

	if ((m->oflags & VPO_UNMANAGED) != 0)
		return (0);
	rw_rlock(&pvh_global_lock);
	lock = VM_PAGE_TO_PV_LIST_LOCK(m);
	rw_rlock(lock);
restart:
	count = 0;
	TAILQ_FOREACH(pv, &m->md.pv_list, pv_next) {
		pmap = PV_PMAP(pv);
		if (!PMAP_TRYLOCK(pmap)) {
			md_gen = m->md.pv_gen;
			rw_runlock(lock);
			PMAP_LOCK(pmap);
			rw_rlock(lock);
			if (md_gen != m->md.pv_gen) {
				PMAP_UNLOCK(pmap);
				goto restart;
			}
		}
		pte = pmap_pte(pmap, pv->pv_va);
		if ((*pte & PG_W) != 0)
			count++;
		PMAP_UNLOCK(pmap);
	}
	if ((m->flags & PG_FICTITIOUS) == 0) {
		pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
		TAILQ_FOREACH(pv, &pvh->pv_list, pv_next) {
			pmap = PV_PMAP(pv);
			if (!PMAP_TRYLOCK(pmap)) {
				md_gen = m->md.pv_gen;
				pvh_gen = pvh->pv_gen;
				rw_runlock(lock);
				PMAP_LOCK(pmap);
				rw_rlock(lock);
				if (md_gen != m->md.pv_gen ||
				    pvh_gen != pvh->pv_gen) {
					PMAP_UNLOCK(pmap);
					goto restart;
				}
			}
			pte = pmap_pde(pmap, pv->pv_va);
			if ((*pte & PG_W) != 0)
				count++;
			PMAP_UNLOCK(pmap);
		}
	}
	rw_runlock(lock);
	rw_runlock(&pvh_global_lock);
	return (count);
}

boolean_t
pmap_is_modified(vm_page_t m)
{
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
	return 0;
}

boolean_t
pmap_is_referenced(vm_page_t m)
{
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
	return 0;
}

/*
 *	pmap_is_prefaultable:
 *
 *	Return whether or not the specified virtual address is elgible
 *	for prefault.
 */
boolean_t
pmap_is_prefaultable(pmap_t pmap, vm_offset_t addr)
{
	pd_entry_t *pde;
	pt_entry_t *pte, PG_V;
	boolean_t rv;

	PG_V = pmap_valid_bit(pmap);
	rv = FALSE;
	PMAP_LOCK(pmap);
	pde = pmap_pde(pmap, addr);
	if (pde != NULL && (*pde & (PG_PS | PG_V)) == PG_V) {
		pte = pmap_pde_to_pte(pde, addr);
		rv = (*pte & PG_V) == 0;
	}
	PMAP_UNLOCK(pmap);
	return (rv);
}

/*
 *	Apply the given advice to the specified range of addresses within the
 *	given pmap.  Depending on the advice, clear the referenced and/or
 *	modified flags in each mapping and set the mapped page's dirty field.
 */
void
pmap_advise(pmap_t pmap, vm_offset_t sva, vm_offset_t eva, int advice)
{
#ifdef LARGEFRAMES
	struct rwlock *lock;
#endif
	pml4_entry_t *pml4e;
	pdp_entry_t *pdpe;
	pd_entry_t oldpde, *pde;
	pt_entry_t *pte, PG_A, PG_G, PG_M, PG_RW, PG_V;
	vm_offset_t va_next;
	vm_page_t m;
	boolean_t anychanged, pv_lists_locked;

	if (advice != MADV_DONTNEED && advice != MADV_FREE)
		return;

	/*
	 * A/D bit emulation requires an alternate code path when clearing
	 * the modified and accessed bits below. Since this function is
	 * advisory in nature we skip it entirely for pmaps that require
	 * A/D bit emulation.
	 */
	if (pmap_emulate_ad_bits(pmap))
		return;

	PG_A = pmap_accessed_bit(pmap);
	PG_G = pmap_global_bit(pmap);
	PG_M = pmap_modified_bit(pmap);
	PG_V = pmap_valid_bit(pmap);
	PG_RW = pmap_rw_bit(pmap);

	pv_lists_locked = FALSE;
#ifdef LARGEFRAMES
resume:
#endif 
	anychanged = FALSE;
	PMAP_LOCK(pmap);
	for (; sva < eva; sva = va_next) {
		pml4e = pmap_pml4e(pmap, sva);
		if ((*pml4e & PG_V) == 0) {
			va_next = (sva + NBPML4) & ~PML4MASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}
		pdpe = pmap_pml4e_to_pdpe(pml4e, sva);
		if ((*pdpe & PG_V) == 0) {
			va_next = (sva + NBPDP) & ~PDPMASK;
			if (va_next < sva)
				va_next = eva;
			continue;
		}
		va_next = (sva + NBPDR) & ~PDRMASK;
		if (va_next < sva)
			va_next = eva;
		pde = pmap_pdpe_to_pde(pdpe, sva);
		oldpde = *pde;
		if ((oldpde & PG_V) == 0)
			continue;
#ifdef LARGEFRAMES
		else if ((oldpde & PG_PS) != 0) {
			if ((oldpde & PG_MANAGED) == 0)
				continue;
			if (!pv_lists_locked) {
				pv_lists_locked = TRUE;
				if (!rw_try_rlock(&pvh_global_lock)) {
					if (anychanged)
						pmap_invalidate_all(pmap);
					PMAP_UNLOCK(pmap);
					rw_rlock(&pvh_global_lock);
					goto resume;
				}
			}
			lock = NULL;
			if (!pmap_demote_pde_locked(pmap, pde, sva, &lock)) {
				if (lock != NULL)
					rw_wunlock(lock);

				/*
				 * The large page mapping was destroyed.
				 */
				continue;
			}

			/*
			 * Unless the page mappings are wired, remove the
			 * mapping to a single page so that a subsequent
			 * access may repromote.  Since the underlying page
			 * table page is fully populated, this removal never
			 * frees a page table page.
			 */
			if ((oldpde & PG_W) == 0) {
				pte = pmap_pde_to_pte(pde, sva);
				KASSERT((*pte & PG_V) != 0,
				    ("pmap_advise: invalid PTE"));
				pmap_remove_pte(pmap, pte, sva, *pde, NULL,
				    &lock);
				anychanged = TRUE;
			}
			if (lock != NULL)
				rw_wunlock(lock);
		}
#endif /* LARGEFRAMES */
		if (va_next > eva)
			va_next = eva;
		for (pte = pmap_pde_to_pte(pde, sva); sva != va_next; pte++,
		    sva += PAGE_SIZE) {
			if ((*pte & (PG_MANAGED | PG_V)) != (PG_MANAGED |
			    PG_V))
				continue;
			else if ((*pte & (PG_M | PG_RW)) == (PG_M | PG_RW)) {
				if (advice == MADV_DONTNEED) {
					/*
					 * Future calls to pmap_is_modified()
					 * can be avoided by making the page
					 * dirty now.
					 */
					m = PHYS_TO_VM_PAGE(*pte & PG_FRAME);
					vm_page_dirty(m);
				}
				/* Xen pte updates are atomic */
				pte_store(pte, *pte & ~(PG_M | PG_A));
			} else if ((*pte & PG_A) != 0)
				/* Xen pte updates are atomic */
				pte_store(pte, *pte & ~PG_A);
			else
				continue;
			if ((*pte & PG_G) != 0)
				pmap_invalidate_page(pmap, sva);
			else
				anychanged = TRUE;
		}
	}
	if (anychanged)
		pmap_invalidate_all(pmap);
	if (pv_lists_locked)
		rw_runlock(&pvh_global_lock);
	PMAP_UNLOCK(pmap);
}

void
pmap_clear_modify(vm_page_t m)
{
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
}

void *
pmap_mapbios(vm_paddr_t pa, vm_size_t size)
{

	KASSERT(0, ("XXX: %s: TODO\n", __func__));
	return NULL;
}

/*
 * Clear the write and modified bits in each of the given page's mappings.
 */
void
pmap_remove_write(vm_page_t m)
{

	struct md_page *pvh;
	pmap_t pmap;
	struct rwlock *lock;
	pv_entry_t /* XXX: LARGEFRAMES next_pv, */ pv;
	pd_entry_t *pde;
	pt_entry_t oldpte, *pte, PG_M, PG_RW;
#ifdef LARGEFRAMES
	vm_offset_t va;
#endif
	int pvh_gen, md_gen;

	KASSERT((m->oflags & VPO_UNMANAGED) == 0,
	    ("pmap_remove_write: page %p is not managed", m));

	/*
	 * If the page is not exclusive busied, then PGA_WRITEABLE cannot be
	 * set by another thread while the object is locked.  Thus,
	 * if PGA_WRITEABLE is clear, no page table entries need updating.
	 */
	VM_OBJECT_ASSERT_WLOCKED(m->object);
	if (!vm_page_xbusied(m) && (m->aflags & PGA_WRITEABLE) == 0)
		return;
	rw_rlock(&pvh_global_lock);
	lock = VM_PAGE_TO_PV_LIST_LOCK(m);
	pvh = pa_to_pvh(VM_PAGE_TO_PHYS(m));
retry_pv_loop:
	rw_wlock(lock);
	if ((m->flags & PG_FICTITIOUS) != 0)
		goto small_mappings;
#ifdef LARGEFRAMES
	TAILQ_FOREACH_SAFE(pv, &pvh->pv_list, pv_next, next_pv) {
		pmap = PV_PMAP(pv);
		if (!PMAP_TRYLOCK(pmap)) {
			pvh_gen = pvh->pv_gen;
			rw_wunlock(lock);
			PMAP_LOCK(pmap);
			rw_wlock(lock);
			if (pvh_gen != pvh->pv_gen) {
				PMAP_UNLOCK(pmap);
				rw_wunlock(lock);
				goto retry_pv_loop;
			}
		}
		PG_RW = pmap_rw_bit(pmap);
		va = pv->pv_va;
		pde = pmap_pde(pmap, va);
		if ((*pde & PG_RW) != 0)
			(void)pmap_demote_pde_locked(pmap, pde, va, &lock);
		KASSERT(lock == VM_PAGE_TO_PV_LIST_LOCK(m),
		    ("inconsistent pv lock %p %p for page %p",
		    lock, VM_PAGE_TO_PV_LIST_LOCK(m), m));
		PMAP_UNLOCK(pmap);
	}
#endif
small_mappings:
	TAILQ_FOREACH(pv, &m->md.pv_list, pv_next) {
		pmap = PV_PMAP(pv);
		if (!PMAP_TRYLOCK(pmap)) {
			pvh_gen = pvh->pv_gen;
			md_gen = m->md.pv_gen;
			rw_wunlock(lock);
			PMAP_LOCK(pmap);
			rw_wlock(lock);
			if (pvh_gen != pvh->pv_gen ||
			    md_gen != m->md.pv_gen) {
				PMAP_UNLOCK(pmap);
				rw_wunlock(lock);
				goto retry_pv_loop;
			}
		}
		PG_M = pmap_modified_bit(pmap);
		PG_RW = pmap_rw_bit(pmap);
		pde = pmap_pde(pmap, pv->pv_va);
		KASSERT((*pde & PG_PS) == 0,
		    ("pmap_remove_write: found a 2mpage in page %p's pv list",
		    m));
		pte = pmap_pde_to_pte(pde, pv->pv_va);
retry:
		oldpte = *pte;
		if (oldpte & PG_RW) {
			if (!atomic_cmpset_long(pte, oldpte, oldpte &
			    ~(PG_RW | PG_M)))
				goto retry;
			if ((oldpte & PG_M) != 0)
				vm_page_dirty(m);
			pmap_invalidate_page(pmap, pv->pv_va);
		}
		PMAP_UNLOCK(pmap);
	}
	rw_wunlock(lock);
	vm_page_aflag_clear(m, PGA_WRITEABLE);
	rw_runlock(&pvh_global_lock);
}

/*
 *	pmap_ts_referenced:
 *
 *	Return a count of reference bits for a page, clearing those bits.
 *	It is not necessary for every reference bit to be cleared, but it
 *	is necessary that 0 only be returned when there are truly no
 *	reference bits set.
 *
 */

int
pmap_ts_referenced(vm_page_t m)
{
	/*
	 * XXX: we don't clear refs yet. We just return non-zero if at
	 * least one reference exists.
	 * This obeys the required semantics - but only just.
	 */
	KASSERT((m->oflags & VPO_UNMANAGED) == 0,
	    ("pmap_ts_referenced: page %p is not managed", m));

	return pmap_pv_iterate(m, pv_dummy, PV_RO_ITERATE);
}

void
pmap_sync_icache(pmap_t pm, vm_offset_t va, vm_size_t sz)
{
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
}

/*
 *	Increase the starting virtual address of the given mapping if a
 *	different alignment might result in more superpage mappings.
 */
void
pmap_align_superpage(vm_object_t object, vm_ooffset_t offset,
    vm_offset_t *addr, vm_size_t size)
{
	vm_offset_t superpage_offset;

	if (size < NBPDR)
		return;
	if (object != NULL && (object->flags & OBJ_COLORED) != 0)
		offset += ptoa(object->pg_color);
	superpage_offset = offset & PDRMASK;
	if (size - ((NBPDR - superpage_offset) & PDRMASK) < NBPDR ||
	    (*addr & PDRMASK) == superpage_offset)
		return;
	if ((*addr & PDRMASK) < superpage_offset)
		*addr = (*addr & ~PDRMASK) + superpage_offset;
	else
		*addr = ((*addr + PDRMASK) & ~PDRMASK) + superpage_offset;
}

void
pmap_suspend()
{
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
}

void
pmap_resume()
{
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
}

int
pmap_mincore(pmap_t pmap, vm_offset_t addr, vm_paddr_t *locked_pa)
{	
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
	return -1;
}

void *
pmap_mapdev(vm_paddr_t pa, vm_size_t size)
{
  	KASSERT(0, ("XXX: %s: TODO\n", __func__));
	return NULL;
}

void
pmap_unmapdev(vm_offset_t va, vm_size_t size)
{
	KASSERT(0, ("XXX: %s: TODO\n", __func__));
}

int
pmap_change_attr(vm_offset_t va, vm_size_t size, int mode)
{
		KASSERT(0, ("XXX: %s: TODO\n", __func__));
		return -1;
}
