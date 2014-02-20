/*	$NetBSD: pte.h,v 1.1 2001/11/23 17:39:04 thorpej Exp $	*/

/*-
 * Copyright (c) 1994 Mark Brinicombe.
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the RiscBSD team.
 * 4. The name "RiscBSD" nor the name of the author may be used to
 *    endorse or promote products derived from this software without specific
 *    prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY RISCBSD ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL RISCBSD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#ifndef _MACHINE_PTE_H_
#define _MACHINE_PTE_H_

#ifndef LOCORE
typedef	uint64_t	pd_entry_t;		/* page directory entry */
typedef	uint64_t	pt_entry_t;		/* page table entry */
#endif

/* Block and Page attributes */
/* TODO: Add the upper attributes */
#define	ATTR_nG		(1 << 11)
#define	ATTR_AF		(1 << 10)
#define	ATTR_SH(x)	((x) << 8)
#define	ATTR_AP(x)	((x) << 6)
#define	ATTR_NS		(1 << 5)
#define	ATTR_IDX(x)	((x) << 3)

/* Level 0 table, 512GiB per entry */
#define	L0_SHIFT	39
#define	L0_INVAL	0x0 /* An invalid address */
#define	L0_BLOCK	0x1 /* A block */
	/* 0x2 also marks an invalid address */
#define	L0_TABLE	0x3 /* A next-level table */

/* Level 1 table, 1GiB per entry */
#define	L1_SHIFT	30
#define	L1_INVAL	L0_INVAL
#define	L1_BLOCK	L0_BLOCK
#define	L1_TABLE	L0_TABLE

/* Level 2 table, 2MiB per entry */
#define	L2_SHIFT	21
#define	L2_INVAL	L0_INVAL
#define	L2_BLOCK	L0_BLOCK
#define	L2_TABLE	L0_TABLE

/* Level 3 table, 4KiB per entry */
#define	L3_SHIFT	12
#define	L3_INVAL	0x0
	/* 0x1 is reserved */
	/* 0x2 also marks an invalid address */
#define	L3_TABLE	0x3

#define	Ln_ADDR_MASK	((1 << 9) - 1)

#endif /* !_MACHINE_PTE_H_ */

/* End of pte.h */
