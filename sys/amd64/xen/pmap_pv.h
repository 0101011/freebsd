/*-
 * Copyright (c) 2011-2012 Spectra Logic Corporation
 * All rights reserved.
 *
 * This software was developed by Cherry G. Mathew <cherry@FreeBSD.org>
 * under sponsorship from Spectra Logic Corporation.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 *
 * $FreeBSD$
 */

#ifndef _MACHINE_PMAP_PV_H_
#define	_MACHINE_PMAP_PV_H_

/* 
 * Used as a callback when iterating through multiple pmaps 
 * If the callback returns 'true', iteration is stopped.
 */
typedef bool pv_cb_t(pmap_t pmap, vm_offset_t va, vm_page_t m) ;

void pmap_pv_init(void);
void pmap_pv_pmap_init(pmap_t pmap);
void pmap_pv_vm_page_init(vm_page_t m);
vm_offset_t pmap_pv_vm_page_to_v(pmap_t pmap, vm_page_t m);
bool pmap_pv_vm_page_mapped(pmap_t pmap, vm_page_t m);
pv_entry_t pmap_get_pv_entry(pmap_t pmap);
bool pmap_put_pv_entry(pmap_t pmap, vm_offset_t va, vm_page_t m);
bool pmap_free_pv_entry(pmap_t pmap, vm_offset_t va, vm_page_t m);
pv_entry_t pmap_find_pv_entry(pmap_t pmap, vm_offset_t va, vm_page_t m);
int pmap_pv_iterate(vm_page_t m, pv_cb_t cb);
int pmap_pv_iterate_map(pmap_t pmap, pv_cb_t cb);
void pmap_pv_page_unmap(vm_page_t m);

#endif /* !_MACHINE_PMAP_PV_H_ */
