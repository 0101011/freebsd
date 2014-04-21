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
 * Copyright (c) 2012 Spectra Logic Corporation
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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * This file contains the amd64 physical->virtual mapping management code.
 * This code used to reside in pmap.c previously and has been excised
 * to make things a bit more modularised.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/rwlock.h>
#include <sys/mutex.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>

#include <machine/md_var.h>

#include <amd64/xen/pmap_pv.h>

#ifdef PV_STATS
#define PV_STAT(x)	do { x ; } while (0)
#else
#define PV_STAT(x)	do { } while (0)
#endif

/*
 * Isolate the global pv list lock from data and other locks to prevent false
 * sharing within the cache.
 */
static struct {
	struct rwlock	lock;
	char		padding[CACHE_LINE_SIZE - sizeof(struct rwlock)];
} pvh_global __aligned(CACHE_LINE_SIZE);

#define	pvh_global_lock	pvh_global.lock

/*
 * Data for the pv entry allocation mechanism
 */
static TAILQ_HEAD(pch, pv_chunk) pv_chunks = TAILQ_HEAD_INITIALIZER(pv_chunks);
#define	NPV_LIST_LOCKS	MAXCPU
#define	PHYS_TO_PV_LIST_LOCK(pa)	\
			(&pv_list_locks[pa_index(pa) % NPV_LIST_LOCKS])
#define	VM_PAGE_TO_PV_LIST_LOCK(m)	\
			PHYS_TO_PV_LIST_LOCK(VM_PAGE_TO_PHYS(m))

static struct mtx pv_chunks_mutex;
static struct rwlock pv_list_locks[NPV_LIST_LOCKS];

vm_map_t	pv_map; /* Kernel submap for pc chunk alloc */

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

/*
 * Returns a new PV entry, allocating a new PV chunk from the system when
 * needed.  If this PV chunk allocation fails and a PV list lock pointer was
 * given, a PV chunk is reclaimed from an arbitrary pmap.  Otherwise, NULL is
 * returned.
 *
 */

#include <vm/vm_extern.h>
#include <vm/vm_kern.h>

static void
free_pv_chunk(struct pv_chunk *pc)
{
	vm_page_t m;
	mtx_lock(&pv_chunks_mutex);

	KASSERT(pc->pc_map[0] == PC_FREE0 && pc->pc_map[1] == PC_FREE1 &&
	    pc->pc_map[2] == PC_FREE2, ("Tried to free chunk in use"));

 	TAILQ_REMOVE(&pv_chunks, pc, pc_lru);
	mtx_unlock(&pv_chunks_mutex);
	PV_STAT(atomic_subtract_int(&pv_entry_spare, _NPCPV));
	PV_STAT(atomic_subtract_int(&pc_chunk_count, 1));
	PV_STAT(atomic_add_int(&pc_chunk_frees, 1));

	/* entire chunk is free, return it */
	bzero(pc, PAGE_SIZE);
	m = PHYS_TO_VM_PAGE(pmap_kextract((vm_offset_t)pc));
	pmap_kremove((vm_offset_t)pc);

	/* Free va range */
	kva_free((vm_offset_t) pc, PAGE_SIZE);

	dump_drop_page(m->phys_addr);
	vm_page_unwire(m, 0);
	vm_page_free(m);
}

/*
 * free the pv_entry back to the free list
 */
static void
free_pv_entry(pmap_t pmap, pv_entry_t pv)
{
	struct pv_chunk *pc;
	int idx, field, bit;

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

	/* 
	 * We don't reclaim the pc backing memory here, in case it's
	 * still being scanned. This is the responsibility of 
	 * pmap_free_pv_entry().
	 * XXX: This is quite fragile. pc management needs to be
	 * formalised a bit better.
	 */

}

pv_entry_t
pmap_get_pv_entry(pmap_t pmap)
{
	int bit, field;
	pv_entry_t pv;
	struct pv_chunk *pc;
	vm_page_t m;

	rw_assert(&pvh_global_lock, RA_LOCKED);
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);
	PV_STAT(atomic_add_long(&pv_entry_allocs, 1));
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
	m = vm_page_alloc(NULL, 0, VM_ALLOC_SYSTEM | VM_ALLOC_NOOBJ |
	    VM_ALLOC_WIRED);
	if (m == NULL) {
		panic("XXX: TODO: memory pressure reclaim\n");
	}

	PV_STAT(atomic_add_int(&pc_chunk_count, 1));
	PV_STAT(atomic_add_int(&pc_chunk_allocs, 1));
	dump_add_page(VM_PAGE_TO_PHYS(m));

	/* 
	 * On xen, we can't just use the DMAP, because it's RO.
	 * We thus explicitly allocate the space + mapping.
	 */

	pc = (void *) kva_alloc(PAGE_SIZE);

	KASSERT(pc != NULL, ("Failed to allocate VA for pv chunk\n"));

	pmap_kenter_ma((vm_offset_t)pc, xpmap_ptom(VM_PAGE_TO_PHYS(m)));


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

bool
pmap_put_pv_entry(pmap_t pmap, vm_offset_t va, vm_page_t m)
{
	pv_entry_t pv;

	KASSERT(m != NULL, ("Invalid page"));
	KASSERT(!(m->oflags & VPO_UNMANAGED),
		("Tried to manage an unmanaged page!"));
	PMAP_LOCK_ASSERT(pmap, MA_OWNED);

	if (pmap_find_pv_entry(pmap, va, m)) {
		return false; /* Duplicate */
	}

	rw_rlock(&pvh_global_lock);

	pv = pmap_get_pv_entry(pmap);
	pv->pv_va = va;
	TAILQ_INSERT_TAIL(&m->md.pv_list, pv, pv_next);

	rw_runlock(&pvh_global_lock);

	return true;
}


bool
pmap_free_pv_entry(pmap_t pmap, vm_offset_t va, vm_page_t m)
{
	bool found = false;
	pv_entry_t pv;
	struct pv_chunk *pc;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);

	rw_rlock(&pvh_global_lock);

	TAILQ_FOREACH(pv, &m->md.pv_list, pv_next) {
		if (pmap == PV_PMAP(pv) && va == pv->pv_va) {
			TAILQ_REMOVE(&m->md.pv_list, pv, pv_next);
			free_pv_entry(pmap, pv);
			pc = pv_to_chunk(pv);
			if (pc->pc_map[0] == PC_FREE0 && pc->pc_map[1] == PC_FREE1 &&
			    pc->pc_map[2] == PC_FREE2) {
				free_pv_chunk(pc);
			}
			found = true;
			break;
		}
	}

	rw_runlock(&pvh_global_lock);
	return found;
}

/* Find an existing p->v mapping. Returns NULL if nonexistent */
pv_entry_t
pmap_find_pv_entry(pmap_t pmap, vm_offset_t va, vm_page_t m)
{
	pv_entry_t pv = NULL;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);

	rw_rlock(&pvh_global_lock);
	TAILQ_FOREACH(pv, &m->md.pv_list, pv_next) {
		if (pmap == PV_PMAP(pv) && va == pv->pv_va) {
			break;
		}
	}

	rw_runlock(&pvh_global_lock);

	return pv;

}


/* This function may be called after pmap_pv_pmap_init() */
void
pmap_pv_init(void)
{
	int i;

 	/*
	 * Initialize the global pv list lock.
	 */
	rw_init(&pvh_global_lock, "pmap pv global");

	/*
	 * Initialize the pv chunk list mutex.
	 */

	mtx_init(&pv_chunks_mutex, "pmap pv chunk list", NULL, MTX_DEF);

	/*
	 * Initialize the pool of pv list locks.
	 */
	for (i = 0; i < NPV_LIST_LOCKS; i++)
		rw_init(&pv_list_locks[i], "pmap pv list");

}

/* Initialise per-pmap pv data. OK to call it before pmap_pv_init() */

void
pmap_pv_pmap_init(pmap_t pmap)
{
	TAILQ_INIT(&pmap->pm_pvchunk);
}

void
pmap_pv_vm_page_init(vm_page_t m)
{
	TAILQ_INIT(&m->md.pv_list);
}

/*
 * Return va mapping of vm_page
 * Returns VM_MAX_KERNEL_ADDRESS + 1 on error.
 */
 
vm_offset_t
pmap_pv_vm_page_to_v(pmap_t pmap, vm_page_t m)
{
	pv_entry_t pv;

	rw_rlock(&pvh_global_lock);
	TAILQ_FOREACH(pv, &m->md.pv_list, pv_next) {
		if (PV_PMAP(pv) == pmap) { /* We return the first hit */
			rw_runlock(&pvh_global_lock);
			return pv->pv_va;
		}
	}

	rw_runlock(&pvh_global_lock);
	return VM_MAX_KERNEL_ADDRESS + 1;
}

/*
 * Query if a given vm_page is mapped in the pmap
 */
bool
pmap_pv_vm_page_mapped(pmap_t pmap, vm_page_t m)
{
	return (pmap_pv_vm_page_to_v(pmap, m) == 
		(VM_MAX_KERNEL_ADDRESS + 1)) ? false : true;

}

/*
 * Iterate through all va mappings a physical page is part of, until
 * callback returns 'true'. 
 * Returns the number of iterations.
 */

int
pmap_pv_iterate(vm_page_t m, pv_cb_t cb, iterate_flags iflag)
{
	int iter = 0;
	pv_entry_t next_pv, pv;

	switch(iflag) {
	case PV_RO_ITERATE:
		rw_rlock(&pvh_global_lock);
		break;
	case PV_RW_ITERATE:
		rw_wlock(&pvh_global_lock);
		break;
	default:
		panic("%s: unknown iterate flag, %d, requested\n", __func__, iflag);
	}

	TAILQ_FOREACH_SAFE(pv, &m->md.pv_list, pv_next, next_pv) {
		iter++;
		if (cb(PV_PMAP(pv), pv->pv_va, m)) break;
	}

	switch(iflag) {
	case PV_RO_ITERATE:
		rw_runlock(&pvh_global_lock);
		break;
	case PV_RW_ITERATE:
		rw_wunlock(&pvh_global_lock);
		break;
	default:
		panic("%s: unknown iterate flag, %d, requested\n", __func__, iflag);
	}
	return iter;
}

/*
 * Iterate through all pv mappings in a pmap, until callback returns
 * 'true'.
 * Returns the number of iterations.
 */

int
pmap_pv_iterate_map(pmap_t pmap, pv_cb_t cb)
{
	int iter = 0;

	int field, idx;
	int64_t bit;
	uint64_t inuse, bitmask;
	pv_entry_t pv;
	struct pv_chunk *pc, *npc;

	PMAP_LOCK_ASSERT(pmap, MA_OWNED);

	TAILQ_FOREACH_SAFE(pc, &pmap->pm_pvchunk, pc_list, npc) {
		iter++;
		for (field = 0; field < _NPCM; field++) {
			inuse = ~pc->pc_map[field] & pc_freemask[field];
			while (inuse != 0) {
				bool cbresult = false;
				bit = bsfq(inuse);
				bitmask = 1UL << bit;
				idx = field * 64 + bit;
				pv = &pc->pc_pventry[idx];
				inuse &= ~bitmask;

				cbresult = cb(PV_PMAP(pv), pv->pv_va, NULL);

				/* 
				 * Check to see the chunk was not
				 * freed by callback. If it is,
				 * reclaim chunk memory.
				 */

				if (pc->pc_map[0] == PC_FREE0 && pc->pc_map[1] == PC_FREE1 &&
				        pc->pc_map[2] == PC_FREE2) {
					goto nextpc;
				}

				if (TAILQ_EMPTY(&pmap->pm_pvchunk)) {
					/* Chunks were all freed in the callback! Bail. */
					goto done_iterating;
				}

				/* Try the next va */
				if (cbresult == false) break;
			}
		}
	nextpc:
		continue;
	}
done_iterating:
	return iter;
}

/*
 * Destroy all pv mappings for a given physical page.
 */
void
pmap_pv_page_unmap(vm_page_t m)
{
	pv_entry_t pv, next_pv;

	rw_wlock(&pvh_global_lock);
	TAILQ_FOREACH_SAFE(pv, &m->md.pv_list, pv_next, next_pv) {
		TAILQ_REMOVE(&m->md.pv_list, pv, pv_next);
	}
	rw_wunlock(&pvh_global_lock);
	return;
}
