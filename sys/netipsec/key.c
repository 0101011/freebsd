/*	$FreeBSD$	*/
/*	$KAME: key.c,v 1.191 2001/06/27 10:46:49 sakane Exp $	*/

/*-
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
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
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This code is referd to RFC 2367
 */

#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_ipsec.h"

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/fnv_hash.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/malloc.h>
#include <sys/rmlock.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/refcount.h>
#include <sys/syslog.h>

#include <vm/uma.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/vnet.h>
#include <net/raw_cb.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/in_var.h>

#ifdef INET6
#include <netinet/ip6.h>
#include <netinet6/in6_var.h>
#include <netinet6/ip6_var.h>
#endif /* INET6 */

#if defined(INET) || defined(INET6)
#include <netinet/in_pcb.h>
#endif
#ifdef INET6
#include <netinet6/in6_pcb.h>
#endif /* INET6 */

#include <net/pfkeyv2.h>
#include <netipsec/keydb.h>
#include <netipsec/key.h>
#include <netipsec/keysock.h>
#include <netipsec/key_debug.h>

#include <netipsec/ipsec.h>
#ifdef INET6
#include <netipsec/ipsec6.h>
#endif

#include <netipsec/xform.h>
#include <machine/stdarg.h>

/* randomness */
#include <sys/random.h>

#define FULLMASK	0xff
#define	_BITS(bytes)	((bytes) << 3)

/*
 * Note on SA reference counting:
 * - SAs that are not in DEAD state will have (total external reference + 1)
 *   following value in reference count field.  they cannot be freed and are
 *   referenced from SA header.
 * - SAs that are in DEAD state will have (total external reference)
 *   in reference count field.  they are ready to be freed.  reference from
 *   SA header will be removed in key_delsav(), when the reference count
 *   field hits 0 (= no external reference other than from SA header.
 */

VNET_DEFINE(u_int32_t, key_debug_level) = 0;
static VNET_DEFINE(u_int, key_spi_trycnt) = 1000;
static VNET_DEFINE(u_int32_t, key_spi_minval) = 0x100;
static VNET_DEFINE(u_int32_t, key_spi_maxval) = 0x0fffffff;	/* XXX */
static VNET_DEFINE(u_int32_t, policy_id) = 0;
/*interval to initialize randseed,1(m)*/
static VNET_DEFINE(u_int, key_int_random) = 60;
/* interval to expire acquiring, 30(s)*/
static VNET_DEFINE(u_int, key_larval_lifetime) = 30;
/* counter for blocking SADB_ACQUIRE.*/
static VNET_DEFINE(int, key_blockacq_count) = 10;
/* lifetime for blocking SADB_ACQUIRE.*/
static VNET_DEFINE(int, key_blockacq_lifetime) = 20;
/* preferred old sa rather than new sa.*/
static VNET_DEFINE(int, key_preferred_oldsa) = 1;
#define	V_key_spi_trycnt	VNET(key_spi_trycnt)
#define	V_key_spi_minval	VNET(key_spi_minval)
#define	V_key_spi_maxval	VNET(key_spi_maxval)
#define	V_policy_id		VNET(policy_id)
#define	V_key_int_random	VNET(key_int_random)
#define	V_key_larval_lifetime	VNET(key_larval_lifetime)
#define	V_key_blockacq_count	VNET(key_blockacq_count)
#define	V_key_blockacq_lifetime	VNET(key_blockacq_lifetime)
#define	V_key_preferred_oldsa	VNET(key_preferred_oldsa)

static VNET_DEFINE(u_int32_t, acq_seq) = 0;
#define	V_acq_seq		VNET(acq_seq)

static VNET_DEFINE(uint32_t, sp_genid) = 0;
#define	V_sp_genid		VNET(sp_genid)

/* SPD */
TAILQ_HEAD(secpolicy_queue, secpolicy);
LIST_HEAD(secpolicy_list, secpolicy);
static VNET_DEFINE(struct secpolicy_queue, sptree[IPSEC_DIR_MAX]);
static struct rmlock sptree_lock;
#define	V_sptree		VNET(sptree)
#define	SPTREE_LOCK_INIT()      rm_init(&sptree_lock, "sptree")
#define	SPTREE_LOCK_DESTROY()   rm_destroy(&sptree_lock)
#define	SPTREE_RLOCK_TRACKER    struct rm_priotracker sptree_tracker
#define	SPTREE_RLOCK()          rm_rlock(&sptree_lock, &sptree_tracker)
#define	SPTREE_RUNLOCK()        rm_runlock(&sptree_lock, &sptree_tracker)
#define	SPTREE_RLOCK_ASSERT()   rm_assert(&sptree_lock, RA_RLOCKED)
#define	SPTREE_WLOCK()          rm_wlock(&sptree_lock)
#define	SPTREE_WUNLOCK()        rm_wunlock(&sptree_lock)
#define	SPTREE_WLOCK_ASSERT()   rm_assert(&sptree_lock, RA_WLOCKED)
#define	SPTREE_UNLOCK_ASSERT()  rm_assert(&sptree_lock, RA_UNLOCKED)

/* Hash table for lookup SP using unique id */
static VNET_DEFINE(struct secpolicy_list *, sphashtbl);
static VNET_DEFINE(u_long, sphash_mask);
#define	V_sphashtbl		VNET(sphashtbl)
#define	V_sphash_mask		VNET(sphash_mask)

#define	SPHASH_NHASH_LOG2	7
#define	SPHASH_NHASH		(1 << SPHASH_NHASH_LOG2)
#define	SPHASH_HASHVAL(id)	(key_u32hash(id) & V_sphash_mask)
#define	SPHASH_HASH(id)		&V_sphashtbl[SPHASH_HASHVAL(id)]

/* SAD */
TAILQ_HEAD(secashead_queue, secashead);
LIST_HEAD(secashead_list, secashead);
static VNET_DEFINE(struct secashead_queue, sahtree);
static struct rmlock sahtree_lock;
#define	V_sahtree		VNET(sahtree)
#define	SAHTREE_LOCK_INIT()	rm_init(&sahtree_lock, "sahtree")
#define	SAHTREE_LOCK_DESTROY()	rm_destroy(&sahtree_lock)
#define	SAHTREE_RLOCK_TRACKER	struct rm_priotracker sahtree_tracker
#define	SAHTREE_RLOCK()		rm_rlock(&sahtree_lock, &sahtree_tracker)
#define	SAHTREE_RUNLOCK()	rm_runlock(&sahtree_lock, &sahtree_tracker)
#define	SAHTREE_RLOCK_ASSERT()	rm_assert(&sahtree_lock, RA_RLOCKED)
#define	SAHTREE_WLOCK()		rm_wlock(&sahtree_lock)
#define	SAHTREE_WUNLOCK()	rm_wunlock(&sahtree_lock)
#define	SAHTREE_WLOCK_ASSERT()	rm_assert(&sahtree_lock, RA_WLOCKED)
#define	SAHTREE_UNLOCK_ASSERT()	rm_assert(&sahtree_lock, RA_UNLOCKED)

/* Hash table for lookup in SAD using SA addresses */
static VNET_DEFINE(struct secashead_list *, sahaddrhashtbl);
static VNET_DEFINE(u_long, sahaddrhash_mask);
#define	V_sahaddrhashtbl	VNET(sahaddrhashtbl)
#define	V_sahaddrhash_mask	VNET(sahaddrhash_mask)

#define	SAHHASH_NHASH_LOG2	7
#define	SAHHASH_NHASH		(1 << SAHHASH_NHASH_LOG2)
#define	SAHADDRHASH_HASHVAL(saidx)	\
    (key_saidxhash(saidx) & V_sahaddrhash_mask)
#define	SAHADDRHASH_HASH(saidx)		\
    &V_sahaddrhashtbl[SAHADDRHASH_HASHVAL(saidx)]

/* Hash table for lookup in SAD using SPI */
LIST_HEAD(secasvar_list, secasvar);
static VNET_DEFINE(struct secasvar_list *, savhashtbl);
static VNET_DEFINE(u_long, savhash_mask);
#define	V_savhashtbl		VNET(savhashtbl)
#define	V_savhash_mask		VNET(savhash_mask)
#define	SAVHASH_NHASH_LOG2	7
#define	SAVHASH_NHASH		(1 << SAVHASH_NHASH_LOG2)
#define	SAVHASH_HASHVAL(spi)	(key_u32hash(spi) & V_savhash_mask)
#define	SAVHASH_HASH(spi)	&V_savhashtbl[SAVHASH_HASHVAL(spi)]

static uint32_t
key_saidxhash(const struct secasindex *saidx)
{
	uint32_t hval;

	hval = fnv_32_buf(&saidx->proto, sizeof(saidx->proto),
	    FNV1_32_INIT);
	switch (saidx->dst.sa.sa_family) {
#ifdef INET
	case AF_INET:
		hval = fnv_32_buf(&saidx->src.sin.sin_addr,
		    sizeof(in_addr_t), hval);
		hval = fnv_32_buf(&saidx->dst.sin.sin_addr,
		    sizeof(in_addr_t), hval);
		break;
#endif
#ifdef INET6
	case AF_INET6:
		hval = fnv_32_buf(&saidx->src.sin6.sin6_addr,
		    sizeof(struct in6_addr), hval);
		hval = fnv_32_buf(&saidx->dst.sin6.sin6_addr,
		    sizeof(struct in6_addr), hval);
		break;
#endif
	default:
		hval = 0;
		ipseclog((LOG_DEBUG, "%s: unknown address family %d",
		    __func__, saidx->dst.sa.sa_family));
	}
	return (hval);
}

static uint32_t
key_u32hash(uint32_t val)
{

	return (fnv_32_buf(&val, sizeof(val), FNV1_32_INIT));
}

							/* registed list */
static VNET_DEFINE(LIST_HEAD(_regtree, secreg), regtree[SADB_SATYPE_MAX + 1]);
#define	V_regtree		VNET(regtree)
static struct mtx regtree_lock;
#define	REGTREE_LOCK_INIT() \
	mtx_init(&regtree_lock, "regtree", "fast ipsec regtree", MTX_DEF)
#define	REGTREE_LOCK_DESTROY()	mtx_destroy(&regtree_lock)
#define	REGTREE_LOCK()		mtx_lock(&regtree_lock)
#define	REGTREE_UNLOCK()	mtx_unlock(&regtree_lock)
#define	REGTREE_LOCK_ASSERT()	mtx_assert(&regtree_lock, MA_OWNED)

static VNET_DEFINE(LIST_HEAD(_acqtree, secacq), acqtree); /* acquiring list */
#define	V_acqtree		VNET(acqtree)
static struct mtx acq_lock;
#define	ACQ_LOCK_INIT() \
	mtx_init(&acq_lock, "acqtree", "fast ipsec acquire list", MTX_DEF)
#define	ACQ_LOCK_DESTROY()	mtx_destroy(&acq_lock)
#define	ACQ_LOCK()		mtx_lock(&acq_lock)
#define	ACQ_UNLOCK()		mtx_unlock(&acq_lock)
#define	ACQ_LOCK_ASSERT()	mtx_assert(&acq_lock, MA_OWNED)

							/* SP acquiring list */
static VNET_DEFINE(LIST_HEAD(_spacqtree, secspacq), spacqtree);
#define	V_spacqtree		VNET(spacqtree)
static struct mtx spacq_lock;
#define	SPACQ_LOCK_INIT() \
	mtx_init(&spacq_lock, "spacqtree", \
		"fast ipsec security policy acquire list", MTX_DEF)
#define	SPACQ_LOCK_DESTROY()	mtx_destroy(&spacq_lock)
#define	SPACQ_LOCK()		mtx_lock(&spacq_lock)
#define	SPACQ_UNLOCK()		mtx_unlock(&spacq_lock)
#define	SPACQ_LOCK_ASSERT()	mtx_assert(&spacq_lock, MA_OWNED)

/* search order for SAs */
static const u_int saorder_state_valid_prefer_old[] = {
	SADB_SASTATE_DYING, SADB_SASTATE_MATURE,
};
static const u_int saorder_state_valid_prefer_new[] = {
	SADB_SASTATE_MATURE, SADB_SASTATE_DYING,
};
static const u_int saorder_state_alive[] = {
	/* except DEAD */
	SADB_SASTATE_MATURE, SADB_SASTATE_DYING, SADB_SASTATE_LARVAL
};
static const u_int saorder_state_any[] = {
	SADB_SASTATE_MATURE, SADB_SASTATE_DYING,
	SADB_SASTATE_LARVAL, SADB_SASTATE_DEAD
};

static const int minsize[] = {
	sizeof(struct sadb_msg),	/* SADB_EXT_RESERVED */
	sizeof(struct sadb_sa),		/* SADB_EXT_SA */
	sizeof(struct sadb_lifetime),	/* SADB_EXT_LIFETIME_CURRENT */
	sizeof(struct sadb_lifetime),	/* SADB_EXT_LIFETIME_HARD */
	sizeof(struct sadb_lifetime),	/* SADB_EXT_LIFETIME_SOFT */
	sizeof(struct sadb_address),	/* SADB_EXT_ADDRESS_SRC */
	sizeof(struct sadb_address),	/* SADB_EXT_ADDRESS_DST */
	sizeof(struct sadb_address),	/* SADB_EXT_ADDRESS_PROXY */
	sizeof(struct sadb_key),	/* SADB_EXT_KEY_AUTH */
	sizeof(struct sadb_key),	/* SADB_EXT_KEY_ENCRYPT */
	sizeof(struct sadb_ident),	/* SADB_EXT_IDENTITY_SRC */
	sizeof(struct sadb_ident),	/* SADB_EXT_IDENTITY_DST */
	sizeof(struct sadb_sens),	/* SADB_EXT_SENSITIVITY */
	sizeof(struct sadb_prop),	/* SADB_EXT_PROPOSAL */
	sizeof(struct sadb_supported),	/* SADB_EXT_SUPPORTED_AUTH */
	sizeof(struct sadb_supported),	/* SADB_EXT_SUPPORTED_ENCRYPT */
	sizeof(struct sadb_spirange),	/* SADB_EXT_SPIRANGE */
	0,				/* SADB_X_EXT_KMPRIVATE */
	sizeof(struct sadb_x_policy),	/* SADB_X_EXT_POLICY */
	sizeof(struct sadb_x_sa2),	/* SADB_X_SA2 */
	sizeof(struct sadb_x_nat_t_type),/* SADB_X_EXT_NAT_T_TYPE */
	sizeof(struct sadb_x_nat_t_port),/* SADB_X_EXT_NAT_T_SPORT */
	sizeof(struct sadb_x_nat_t_port),/* SADB_X_EXT_NAT_T_DPORT */
	sizeof(struct sadb_address),	/* SADB_X_EXT_NAT_T_OAI */
	sizeof(struct sadb_address),	/* SADB_X_EXT_NAT_T_OAR */
	sizeof(struct sadb_x_nat_t_frag),/* SADB_X_EXT_NAT_T_FRAG */
};
static const int maxsize[] = {
	sizeof(struct sadb_msg),	/* SADB_EXT_RESERVED */
	sizeof(struct sadb_sa),		/* SADB_EXT_SA */
	sizeof(struct sadb_lifetime),	/* SADB_EXT_LIFETIME_CURRENT */
	sizeof(struct sadb_lifetime),	/* SADB_EXT_LIFETIME_HARD */
	sizeof(struct sadb_lifetime),	/* SADB_EXT_LIFETIME_SOFT */
	0,				/* SADB_EXT_ADDRESS_SRC */
	0,				/* SADB_EXT_ADDRESS_DST */
	0,				/* SADB_EXT_ADDRESS_PROXY */
	0,				/* SADB_EXT_KEY_AUTH */
	0,				/* SADB_EXT_KEY_ENCRYPT */
	0,				/* SADB_EXT_IDENTITY_SRC */
	0,				/* SADB_EXT_IDENTITY_DST */
	0,				/* SADB_EXT_SENSITIVITY */
	0,				/* SADB_EXT_PROPOSAL */
	0,				/* SADB_EXT_SUPPORTED_AUTH */
	0,				/* SADB_EXT_SUPPORTED_ENCRYPT */
	sizeof(struct sadb_spirange),	/* SADB_EXT_SPIRANGE */
	0,				/* SADB_X_EXT_KMPRIVATE */
	0,				/* SADB_X_EXT_POLICY */
	sizeof(struct sadb_x_sa2),	/* SADB_X_SA2 */
	sizeof(struct sadb_x_nat_t_type),/* SADB_X_EXT_NAT_T_TYPE */
	sizeof(struct sadb_x_nat_t_port),/* SADB_X_EXT_NAT_T_SPORT */
	sizeof(struct sadb_x_nat_t_port),/* SADB_X_EXT_NAT_T_DPORT */
	0,				/* SADB_X_EXT_NAT_T_OAI */
	0,				/* SADB_X_EXT_NAT_T_OAR */
	sizeof(struct sadb_x_nat_t_frag),/* SADB_X_EXT_NAT_T_FRAG */
};

#define	SADB_CHECKLEN(_mhp, _ext)			\
    ((_mhp)->extlen[(_ext)] < minsize[(_ext)] || (maxsize[(_ext)] != 0 && \
	((_mhp)->extlen[(_ext)] > maxsize[(_ext)])))
#define	SADB_CHECKHDR(_mhp, _ext)	((_mhp)->ext[(_ext)] == NULL)

static VNET_DEFINE(int, ipsec_esp_keymin) = 256;
static VNET_DEFINE(int, ipsec_esp_auth) = 0;
static VNET_DEFINE(int, ipsec_ah_keymin) = 128;

#define	V_ipsec_esp_keymin	VNET(ipsec_esp_keymin)
#define	V_ipsec_esp_auth	VNET(ipsec_esp_auth)
#define	V_ipsec_ah_keymin	VNET(ipsec_ah_keymin)

#ifdef SYSCTL_DECL
SYSCTL_DECL(_net_key);
#endif

SYSCTL_INT(_net_key, KEYCTL_DEBUG_LEVEL,	debug,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(key_debug_level), 0, "");

/* max count of trial for the decision of spi value */
SYSCTL_INT(_net_key, KEYCTL_SPI_TRY, spi_trycnt,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(key_spi_trycnt), 0, "");

/* minimum spi value to allocate automatically. */
SYSCTL_INT(_net_key, KEYCTL_SPI_MIN_VALUE, spi_minval,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(key_spi_minval), 0, "");

/* maximun spi value to allocate automatically. */
SYSCTL_INT(_net_key, KEYCTL_SPI_MAX_VALUE, spi_maxval,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(key_spi_maxval), 0, "");

/* interval to initialize randseed */
SYSCTL_INT(_net_key, KEYCTL_RANDOM_INT, int_random,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(key_int_random), 0, "");

/* lifetime for larval SA */
SYSCTL_INT(_net_key, KEYCTL_LARVAL_LIFETIME, larval_lifetime,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(key_larval_lifetime), 0, "");

/* counter for blocking to send SADB_ACQUIRE to IKEd */
SYSCTL_INT(_net_key, KEYCTL_BLOCKACQ_COUNT, blockacq_count,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(key_blockacq_count), 0, "");

/* lifetime for blocking to send SADB_ACQUIRE to IKEd */
SYSCTL_INT(_net_key, KEYCTL_BLOCKACQ_LIFETIME, blockacq_lifetime,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(key_blockacq_lifetime), 0, "");

/* ESP auth */
SYSCTL_INT(_net_key, KEYCTL_ESP_AUTH, esp_auth,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ipsec_esp_auth), 0, "");

/* minimum ESP key length */
SYSCTL_INT(_net_key, KEYCTL_ESP_KEYMIN, esp_keymin,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ipsec_esp_keymin), 0, "");

/* minimum AH key length */
SYSCTL_INT(_net_key, KEYCTL_AH_KEYMIN, ah_keymin,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ipsec_ah_keymin), 0, "");

/* perfered old SA rather than new SA */
SYSCTL_INT(_net_key, KEYCTL_PREFERED_OLDSA, preferred_oldsa,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(key_preferred_oldsa), 0, "");

#define __LIST_CHAINED(elm) \
	(!((elm)->chain.le_next == NULL && (elm)->chain.le_prev == NULL))
#define LIST_INSERT_TAIL(head, elm, type, field) \
do {\
	struct type *curelm = LIST_FIRST(head); \
	if (curelm == NULL) {\
		LIST_INSERT_HEAD(head, elm, field); \
	} else { \
		while (LIST_NEXT(curelm, field)) \
			curelm = LIST_NEXT(curelm, field);\
		LIST_INSERT_AFTER(curelm, elm, field);\
	}\
} while (0)

#define KEY_CHKSASTATE(head, sav, name) \
do { \
	if ((head) != (sav)) {						\
		ipseclog((LOG_DEBUG, "%s: state mismatched (TREE=%d SA=%d)\n", \
			(name), (head), (sav)));			\
		break;							\
	}								\
} while (0)

#define KEY_CHKSPDIR(head, sp, name) \
do { \
	if ((head) != (sp)) {						\
		ipseclog((LOG_DEBUG, "%s: direction mismatched (TREE=%d SP=%d), " \
			"anyway continue.\n",				\
			(name), (head), (sp)));				\
	}								\
} while (0)

MALLOC_DEFINE(M_IPSEC_SA, "secasvar", "ipsec security association");
MALLOC_DEFINE(M_IPSEC_SAH, "sahead", "ipsec sa head");
MALLOC_DEFINE(M_IPSEC_SP, "ipsecpolicy", "ipsec security policy");
MALLOC_DEFINE(M_IPSEC_SR, "ipsecrequest", "ipsec security request");
MALLOC_DEFINE(M_IPSEC_MISC, "ipsec-misc", "ipsec miscellaneous");
MALLOC_DEFINE(M_IPSEC_SAQ, "ipsec-saq", "ipsec sa acquire");
MALLOC_DEFINE(M_IPSEC_SAR, "ipsec-reg", "ipsec sa acquire");

static VNET_DEFINE(uma_zone_t, key_lft_zone);
#define	V_key_lft_zone		VNET(key_lft_zone)

/*
 * set parameters into secpolicyindex buffer.
 * Must allocate secpolicyindex buffer passed to this function.
 */
#define KEY_SETSECSPIDX(_dir, s, d, ps, pd, ulp, idx) \
do { \
	bzero((idx), sizeof(struct secpolicyindex));                         \
	(idx)->dir = (_dir);                                                 \
	(idx)->prefs = (ps);                                                 \
	(idx)->prefd = (pd);                                                 \
	(idx)->ul_proto = (ulp);                                             \
	bcopy((s), &(idx)->src, ((const struct sockaddr *)(s))->sa_len);     \
	bcopy((d), &(idx)->dst, ((const struct sockaddr *)(d))->sa_len);     \
} while (0)

/*
 * set parameters into secasindex buffer.
 * Must allocate secasindex buffer before calling this function.
 */
#define KEY_SETSECASIDX(p, m, r, s, d, idx) \
do { \
	bzero((idx), sizeof(struct secasindex));                             \
	(idx)->proto = (p);                                                  \
	(idx)->mode = (m);                                                   \
	(idx)->reqid = (r);                                                  \
	bcopy((s), &(idx)->src, ((const struct sockaddr *)(s))->sa_len);     \
	bcopy((d), &(idx)->dst, ((const struct sockaddr *)(d))->sa_len);     \
} while (0)

/* key statistics */
struct _keystat {
	u_long getspi_count; /* the avarage of count to try to get new SPI */
} keystat;

struct sadb_msghdr {
	struct sadb_msg *msg;
	struct sadb_ext *ext[SADB_EXT_MAX + 1];
	int extoff[SADB_EXT_MAX + 1];
	int extlen[SADB_EXT_MAX + 1];
};

#ifndef IPSEC_DEBUG2
static struct callout key_timer;
#endif

static struct secasvar *key_allocsa_policy(const struct secasindex *);
static void key_freesp_so(struct secpolicy **);
static struct secasvar *key_do_allocsa_policy(struct secashead *, u_int);
static void key_unlink(struct secpolicy *);
static struct secpolicy *key_getsp(struct secpolicyindex *);
static struct secpolicy *key_getspbyid(u_int32_t);
static u_int32_t key_newreqid(void);
static struct mbuf *key_gather_mbuf(struct mbuf *,
	const struct sadb_msghdr *, int, int, ...);
static int key_spdadd(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static u_int32_t key_getnewspid(void);
static int key_spddelete(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static int key_spddelete2(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static int key_spdget(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static int key_spdflush(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static int key_spddump(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static struct mbuf *key_setdumpsp(struct secpolicy *,
	u_int8_t, u_int32_t, u_int32_t);
static struct mbuf *key_sp2mbuf(struct secpolicy *);
static size_t key_getspreqmsglen(struct secpolicy *);
static int key_spdexpire(struct secpolicy *);
static struct secashead *key_newsah(struct secasindex *);
static void key_delsah(struct secashead *);
static struct secasvar *key_newsav(struct mbuf *,
	const struct sadb_msghdr *, struct secashead *, int *,
	const char*, int);
#define	KEY_NEWSAV(m, sadb, sah, e)				\
	key_newsav(m, sadb, sah, e, __FILE__, __LINE__)
static void key_delsav(struct secasvar *);
static struct secashead *key_getsah(struct secasindex *);
static struct secasvar *key_checkspidup(struct secasindex *, u_int32_t);
static struct secasvar *key_getsavbyspi(struct secashead *, u_int32_t);
static int key_setsaval(struct secasvar *, struct mbuf *,
	const struct sadb_msghdr *);
static int key_mature(struct secasvar *);
static struct mbuf *key_setdumpsa(struct secasvar *, u_int8_t,
	u_int8_t, u_int32_t, u_int32_t);
static struct mbuf *key_setsadbmsg(u_int8_t, u_int16_t, u_int8_t,
	u_int32_t, pid_t, u_int16_t);
static struct mbuf *key_setsadbsa(struct secasvar *);
static struct mbuf *key_setsadbaddr(u_int16_t,
	const struct sockaddr *, u_int8_t, u_int16_t);
#ifdef IPSEC_NAT_T
static struct mbuf *key_setsadbxport(u_int16_t, u_int16_t);
static struct mbuf *key_setsadbxtype(u_int16_t);
#endif
static void key_porttosaddr(struct sockaddr *, u_int16_t);
#define	KEY_PORTTOSADDR(saddr, port)				\
	key_porttosaddr((struct sockaddr *)(saddr), (port))
static struct mbuf *key_setsadbxsa2(u_int8_t, u_int32_t, u_int32_t);
static struct mbuf *key_setsadbxpolicy(u_int16_t, u_int8_t,
	u_int32_t, u_int32_t);
static struct seckey *key_dup_keymsg(const struct sadb_key *, u_int, 
				     struct malloc_type *);
static struct seclifetime *key_dup_lifemsg(const struct sadb_lifetime *src,
					    struct malloc_type *type);
#ifdef INET6
static int key_ismyaddr6(struct sockaddr_in6 *);
#endif

/* flags for key_cmpsaidx() */
#define CMP_HEAD	1	/* protocol, addresses. */
#define CMP_MODE_REQID	2	/* additionally HEAD, reqid, mode. */
#define CMP_REQID	3	/* additionally HEAD, reaid. */
#define CMP_EXACTLY	4	/* all elements. */
static int key_cmpsaidx(const struct secasindex *,
    const struct secasindex *, int);
static int key_cmpspidx_exactly(struct secpolicyindex *,
    struct secpolicyindex *);
static int key_cmpspidx_withmask(struct secpolicyindex *,
    struct secpolicyindex *);
static int key_sockaddrcmp(const struct sockaddr *,
    const struct sockaddr *, int);
static int key_bbcmp(const void *, const void *, u_int);
static u_int16_t key_satype2proto(u_int8_t);
static u_int8_t key_proto2satype(u_int16_t);

static int key_getspi(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static u_int32_t key_do_getnewspi(struct sadb_spirange *,
					struct secasindex *);
static int key_update(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
#ifdef IPSEC_DOSEQCHECK
static struct secasvar *key_getsavbyseq(struct secashead *, u_int32_t);
#endif
static int key_add(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static int key_setident(struct secashead *, struct mbuf *,
	const struct sadb_msghdr *);
static struct mbuf *key_getmsgbuf_x1(struct mbuf *,
	const struct sadb_msghdr *);
static int key_delete(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static int key_delete_all(struct socket *, struct mbuf *,
	const struct sadb_msghdr *, u_int16_t);
static int key_get(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);

static void key_getcomb_setlifetime(struct sadb_comb *);
static struct mbuf *key_getcomb_esp(void);
static struct mbuf *key_getcomb_ah(void);
static struct mbuf *key_getcomb_ipcomp(void);
static struct mbuf *key_getprop(const struct secasindex *);

static int key_acquire(const struct secasindex *, struct secpolicy *);
static struct secacq *key_newacq(const struct secasindex *);
static struct secacq *key_getacq(const struct secasindex *);
static struct secacq *key_getacqbyseq(u_int32_t);
static struct secspacq *key_newspacq(struct secpolicyindex *);
static struct secspacq *key_getspacq(struct secpolicyindex *);
static int key_acquire2(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static int key_register(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static int key_expire(struct secasvar *, int);
static int key_flush(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static int key_dump(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static int key_promisc(struct socket *, struct mbuf *,
	const struct sadb_msghdr *);
static int key_senderror(struct socket *, struct mbuf *, int);
static int key_validate_ext(const struct sadb_ext *, int);
static int key_align(struct mbuf *, struct sadb_msghdr *);
static struct mbuf *key_setlifetime(struct seclifetime *src, 
				     u_int16_t exttype);
static struct mbuf *key_setkey(struct seckey *src, u_int16_t exttype);

#if 0
static const char *key_getfqdn(void);
static const char *key_getuserfqdn(void);
#endif

#define	DBG_IPSEC_INITREF(t, p)	do {				\
	refcount_init(&(p)->refcnt, 1);				\
	KEYDBG(KEY_STAMP,					\
	    printf("%s: Initialize refcnt %s(%p) = %u\n",	\
	    __func__, #t, (p), (p)->refcnt));			\
} while (0)
#define	DBG_IPSEC_ADDREF(t, p)	do {				\
	refcount_acquire(&(p)->refcnt);				\
	KEYDBG(KEY_STAMP,					\
	    printf("%s: Acquire refcnt %s(%p) -> %u\n",		\
	    __func__, #t, (p), (p)->refcnt));			\
} while (0)
#define	DBG_IPSEC_DELREF(t, p)	do {				\
	KEYDBG(KEY_STAMP,					\
	    printf("%s: Release refcnt %s(%p) -> %u\n",		\
	    __func__, #t, (p), (p)->refcnt - 1));		\
	refcount_release(&(p)->refcnt);				\
} while (0)

#define	IPSEC_INITREF(t, p)	refcount_init(&(p)->refcnt, 1)
#define	IPSEC_ADDREF(t, p)	refcount_acquire(&(p)->refcnt)
#define	IPSEC_DELREF(t, p)	refcount_release(&(p)->refcnt)

#define	SP_INITREF(p)	IPSEC_INITREF(SP, p)
#define	SP_ADDREF(p)	IPSEC_ADDREF(SP, p)
#define	SP_DELREF(p)	IPSEC_DELREF(SP, p)

#define	SAH_INITREF(p)	IPSEC_INITREF(SAH, p)
#define	SAH_ADDREF(p)	IPSEC_ADDREF(SAH, p)
#define	SAH_DELREF(p)	IPSEC_DELREF(SAH, p)

#define	SAV_INITREF(p)	IPSEC_INITREF(SAV, p)
#define	SAV_ADDREF(p)	IPSEC_ADDREF(SAV, p)
#define	SAV_DELREF(p)	IPSEC_DELREF(SAV, p)

/*
 * Update the refcnt while holding the SPTREE lock.
 */
void
key_addref(struct secpolicy *sp)
{

	SP_ADDREF(sp);
}

/*
 * Return 0 when there are known to be no SP's for the specified
 * direction.  Otherwise return 1.  This is used by IPsec code
 * to optimize performance.
 */
int
key_havesp(u_int dir)
{

	return (dir == IPSEC_DIR_INBOUND || dir == IPSEC_DIR_OUTBOUND ?
		TAILQ_FIRST(&V_sptree[dir]) != NULL : 1);
}

/* %%% IPsec policy management */
/*
 * Return current SPDB generation.
 */
uint32_t
key_getspgen(void)
{

	return (V_sp_genid);
}

static int
key_checksockaddrs(struct sockaddr *src, struct sockaddr *dst)
{

	/* family match */
	if (src->sa_family != dst->sa_family)
		return (EINVAL);
	/* sa_len match */
	if (src->sa_len != dst->sa_len)
		return (EINVAL);
	switch (src->sa_family) {
#ifdef INET
	case AF_INET:
		if (src->sa_len != sizeof(struct sockaddr_in))
			return (EINVAL);
		break;
#endif
#ifdef INET6
	case AF_INET6:
		if (src->sa_len != sizeof(struct sockaddr_in6))
			return (EINVAL);
		break;
#endif
	default:
		return (EAFNOSUPPORT);
	}
	return (0);
}

/*
 * allocating a SP for OUTBOUND or INBOUND packet.
 * Must call key_freesp() later.
 * OUT:	NULL:	not found
 *	others:	found and return the pointer.
 */
struct secpolicy *
key_allocsp(struct secpolicyindex *spidx, u_int dir)
{
	SPTREE_RLOCK_TRACKER;
	struct secpolicy *sp;

	IPSEC_ASSERT(spidx != NULL, ("null spidx"));
	IPSEC_ASSERT(dir == IPSEC_DIR_INBOUND || dir == IPSEC_DIR_OUTBOUND,
		("invalid direction %u", dir));

	SPTREE_RLOCK();
	TAILQ_FOREACH(sp, &V_sptree[dir], chain) {
		if (key_cmpspidx_withmask(&sp->spidx, spidx)) {
			SP_ADDREF(sp);
			break;
		}
	}
	SPTREE_RUNLOCK();

	if (sp != NULL) {	/* found a SPD entry */
		sp->lastused = time_second;
		KEYDBG(IPSEC_STAMP,
		    printf("%s: return SP(%p)\n", __func__, sp));
		KEYDBG(IPSEC_DATA, kdebug_secpolicy(sp));
	} else {
		KEYDBG(IPSEC_DATA,
		    printf("%s: lookup failed for ", __func__);
		    kdebug_secpolicyindex(spidx, NULL));
	}
	return (sp);
}

/*
 * Allocating an SA entry for an *OUTBOUND* packet.
 * OUT:	positive:	corresponding SA for given saidx found.
 *	NULL:		SA not found, but will be acquired, check *error
 *			for acquiring status.
 */
struct secasvar *
key_allocsa_policy(struct secpolicy *sp, const struct secasindex *saidx,
    int *error)
{
	SAHTREE_RLOCK_TRACKER;
	struct secashead *sah;
	struct secasvar *sav;

	IPSEC_ASSERT(saidx != NULL, ("null saidx"));
	IPSEC_ASSERT(saidx->mode == IPSEC_MODE_TRANSPORT ||
		saidx->mode == IPSEC_MODE_TUNNEL,
		("unexpected policy %u", saidx->mode));

	/*
	 * We check new SA in the IPsec request because a different
	 * SA may be involved each time this request is checked, either
	 * because new SAs are being configured, or this request is
	 * associated with an unconnected datagram socket, or this request
	 * is associated with a system default policy.
	 */
	SAHTREE_RLOCK();
	LIST_FOREACH(sah, SAHADDRHASH_HASH(saidx), addrhash) {
		KEYDBG(IPSEC_DUMP,
		    printf("%s: checking SAH\n", __func__);
		    kdebug_secash(sah, "  "));
		if (key_cmpsaidx(&sah->saidx, saidx, CMP_MODE_REQID))
			break;

	}
	if (sah != NULL) {
		/*
		 * Allocate the oldest SA available according to
		 * draft-jenkins-ipsec-rekeying-03.
		 */
		if (V_key_preferred_oldsa)
			sav = TAILQ_LAST(&sah->savtree_alive, secasvar_queue);
		else
			sav = TAILQ_FIRST(&sah->savtree_alive);
		if (sav != NULL)
			SAV_ADDREF(sav);
	} else
		sav = NULL;
	SAHTREE_RUNLOCK();

	if (sav != NULL) {
		*error = 0;
		KEYDBG(IPSEC_STAMP,
		    printf("%s: chosen SA(%p) for SP(%p)\n", __func__,
			sav, sp));
		KEYDBG(IPSEC_DATA, kdebug_secasv(sav));
		return (sav); /* return referenced SA */
	}

	/* there is no SA */
	*error = key_acquire(saidx, sp);
	if ((*error) != 0)
		ipseclog((LOG_DEBUG,
		    "%s: error %d returned from key_acquire()\n",
			__func__, *error));
	KEYDBG(IPSEC_STAMP,
	    printf("%s: acquire SA for SP(%p), error %d\n",
		__func__, sp, *error));
	KEYDBG(IPSEC_DATA, kdebug_secasindex(saidx, NULL));
	return (NULL);
}

/*
 * allocating a usable SA entry for a *INBOUND* packet.
 * Must call key_freesav() later.
 * OUT: positive:	pointer to a usable sav (i.e. MATURE or DYING state).
 *	NULL:		not found, or error occurred.
 *
 * In the comparison, no source address is used--for RFC2401 conformance.
 * To quote, from section 4.1:
 *	A security association is uniquely identified by a triple consisting
 *	of a Security Parameter Index (SPI), an IP Destination Address, and a
 *	security protocol (AH or ESP) identifier.
 * Note that, however, we do need to keep source address in IPsec SA.
 * IKE specification and PF_KEY specification do assume that we
 * keep source address in IPsec SA.  We see a tricky situation here.
 */
struct secasvar *
key_allocsa(union sockaddr_union *dst, uint8_t proto, uint32_t spi)
{
	SAHTREE_RLOCK_TRACKER;
	struct secasvar *sav;
	int chkport;

	IPSEC_ASSERT(dst != NULL, ("null dst address"));

	chkport = 0;
	SAHTREE_RLOCK();
	LIST_FOREACH(sav, SAVHASH_HASH(spi), spihash) {
		if (sav->spi == spi)
			break;
	}
	/*
	 * We use single SPI namespace for all protocols, so it is
	 * impossible to have SPI duplicates in the SAVHASH.
	 * XXXAE: this breaks TCP_SIGNATURE.
	 */
	if (sav != NULL) {
#ifdef IPSEC_NAT_T
		/*
		 * Really only check ports when this is a NAT-T
		 * SA.  Otherwise other lookups providing ports
		 * might suffer.
		 */
		chkport = (sav->natt_type != 0 &&
		    dst->sa.sa_family == AF_INET &&
		    dst->sa.sa_len == sizeof(struct sockaddr_in) &&
		    dst->sin.sin_port != 0);
#endif
		if (sav->state != SADB_SASTATE_LARVAL &&
		    sav->sah->saidx.proto == proto &&
		    key_sockaddrcmp(&dst->sa, &sav->sah->saidx.dst.sa,
			chkport) == 0)
			SAV_ADDREF(sav);
		else
			sav = NULL;
	}
	SAHTREE_RUNLOCK();

	if (sav == NULL) {
		KEYDBG(IPSEC_STAMP,
		    char buf[IPSEC_ADDRSTRLEN];
		    printf("%s: SA not found for spi %u proto %u dst %s\n",
			__func__, ntohl(spi), proto, ipsec_address(dst, buf,
			sizeof(buf))));
	} else {
		KEYDBG(IPSEC_STAMP,
		    printf("%s: return SA(%p)\n", __func__, sav));
		KEYDBG(IPSEC_DATA, kdebug_secasv(sav));
	}
	return (sav);
}

struct secasvar *
key_allocsa_tunnel(union sockaddr_union *src, union sockaddr_union *dst,
    uint8_t proto)
{
	SAHTREE_RLOCK_TRACKER;
	struct secasindex saidx;
	struct secashead *sah;
	struct secasvar *sav;

	IPSEC_ASSERT(src != NULL, ("null src address"));
	IPSEC_ASSERT(dst != NULL, ("null dst address"));

	KEY_SETSECASIDX(proto, IPSEC_MODE_TUNNEL, 0, &src->sa,
	    &dst->sa, &saidx);

	sav = NULL;
	SAHTREE_RLOCK();
	LIST_FOREACH(sah, SAHADDRHASH_HASH(&saidx), addrhash) {
		if (IPSEC_MODE_TUNNEL != sah->saidx.mode)
			continue;
		if (proto != sah->saidx.proto)
			continue;
		if (key_sockaddrcmp(&src->sa, &sav->sah->saidx.src.sa, 0) != 0)
			continue;
		if (key_sockaddrcmp(&dst->sa, &sav->sah->saidx.dst.sa, 0) != 0)
			continue;
		/* XXXAE: is key_preferred_oldsa reasonably?*/
		if (V_key_preferred_oldsa)
			sav = TAILQ_LAST(&sah->savtree_alive, secasvar_queue);
		else
			sav = TAILQ_FIRST(&sah->savtree_alive);
		if (sav != NULL) {
			SAV_ADDREF(sav);
			break;
		}
	}
	SAHTREE_RUNLOCK();
	KEYDBG(IPSEC_STAMP,
	    printf("%s: return SA(%p)\n", __func__, sav));
	if (sav != NULL)
		KEYDBG(IPSEC_DATA, kdebug_secasv(sav));
	return (sav);
}

/*
 * Must be called after calling key_allocsp().
 */
void
key_freesp(struct secpolicy **spp)
{
	struct secpolicy *sp = *spp;

	IPSEC_ASSERT(sp != NULL, ("null sp"));
	if (SP_DELREF(sp) == 0)
		return;

	KEYDBG(IPSEC_STAMP,
	    printf("%s: last reference to SP(%p)\n", __func__, sp));
	KEYDBG(IPSEC_DATA, kdebug_secpolicy(sp));

	*spp = NULL;
	while (sp->tcount > 0)
		ipsec_delisr(sp->req[--sp->tcount]);
	free(sp, M_IPSEC_SP);
}

static void
key_unlink(struct secpolicy *sp)
{

	IPSEC_ASSERT(sp->spidx.dir == IPSEC_DIR_INBOUND ||
	    sp->spidx.dir == IPSEC_DIR_OUTBOUND,
	    ("invalid direction %u", sp->spidx.dir));
	SPTREE_UNLOCK_ASSERT();

	KEYDBG(KEY_STAMP,
	    printf("%s: SP(%p)\n", __func__, sp));
	SPTREE_WLOCK();
	if (sp->state != IPSEC_SPSTATE_ALIVE) {
		/* SP is already unlinked */
		SPTREE_WUNLOCK();
		return;
	}
	sp->state = IPSEC_SPSTATE_DEAD;
	TAILQ_REMOVE(&V_sptree[sp->spidx.dir], sp, chain);
	LIST_REMOVE(sp, idhash);
	V_sp_genid++;
	SPTREE_WUNLOCK();
	key_freesp(&sp);
}

/*
 * insert a secpolicy into the SP database. Lower priorities first
 */
static void
key_insertsp(struct secpolicy *newsp)
{
	struct secpolicy *sp;

	SPTREE_WLOCK_ASSERT();
	TAILQ_FOREACH(sp, &V_sptree[newsp->spidx.dir], chain) {
		if (newsp->priority < sp->priority) {
			TAILQ_INSERT_BEFORE(sp, newsp, chain);
			goto done;
		}
	}
	TAILQ_INSERT_TAIL(&V_sptree[newsp->spidx.dir], newsp, chain);
done:
	LIST_INSERT_HEAD(SPHASH_HASH(newsp->id), newsp, idhash);
	newsp->state = IPSEC_SPSTATE_ALIVE;
	V_sp_genid++;
}

void
key_addrefsa(struct secasvar *sav, const char* where, int tag)
{

	IPSEC_ASSERT(sav != NULL, ("null sav"));
	IPSEC_ASSERT(sav->refcnt > 0, ("refcount must exist"));

	SAV_ADDREF(sav);
}

/*
 * Must be called after calling key_allocsa().
 * This function is called by key_freesp() to free some SA allocated
 * for a policy.
 */
void
key_freesav(struct secasvar **psav)
{
	struct secasvar *sav = *psav;

	IPSEC_ASSERT(sav != NULL, ("null sav"));
	if (SAV_DELREF(sav) == 0)
		return;

	KEYDBG(IPSEC_STAMP,
	    printf("%s: last reference to SA(%p)\n", __func__, sav));

	*psav = NULL;
	key_delsav(sav);
}

/*
 * Unlink SA from SAH and SPI hash under SAHTREE_WLOCK.
 * Expect that SA has extra reference due to lookup.
 * Release this references, also release SAH reference after unlink.
 */
static void
key_unlinksav(struct secasvar *sav)
{
	struct secashead *sah;

	KEYDBG(KEY_STAMP,
	    printf("%s: SA(%p)\n", __func__, sav));

	SAHTREE_UNLOCK_ASSERT();
	SAHTREE_WLOCK();
	if (sav->state == SADB_SASTATE_DEAD) {
		/* SA is already unlinked */
		SAHTREE_WUNLOCK();
		return;
	}
	/* Unlink from SAH */
	if (sav->state == SADB_SASTATE_LARVAL)
		TAILQ_REMOVE(&sav->sah->savtree_larval, sav, chain);
	else
		TAILQ_REMOVE(&sav->sah->savtree_alive, sav, chain);
	/* Unlink from SPI hash */
	LIST_REMOVE(sav, spihash);
	sav->state = SADB_SASTATE_DEAD;
	sah = sav->sah;
	SAHTREE_WUNLOCK();
	key_freesav(&sav);
	/* Since we are unlinked, release reference to SAH */
	key_freesah(&sah);
}

/* %%% SPD management */
/*
 * search SPD
 * OUT:	NULL	: not found
 *	others	: found, pointer to a SP.
 */
static struct secpolicy *
key_getsp(struct secpolicyindex *spidx)
{
	SPTREE_RLOCK_TRACKER;
	struct secpolicy *sp;

	IPSEC_ASSERT(spidx != NULL, ("null spidx"));

	SPTREE_RLOCK();
	TAILQ_FOREACH(sp, &V_sptree[spidx->dir], chain) {
		if (key_cmpspidx_exactly(spidx, &sp->spidx)) {
			SP_ADDREF(sp);
			break;
		}
	}
	SPTREE_RUNLOCK();

	return sp;
}

/*
 * get SP by index.
 * OUT:	NULL	: not found
 *	others	: found, pointer to referenced SP.
 */
static struct secpolicy *
key_getspbyid(uint32_t id)
{
	SPTREE_RLOCK_TRACKER;
	struct secpolicy *sp;

	SPTREE_RLOCK();
	LIST_FOREACH(sp, SPHASH_HASH(id), idhash) {
		if (sp->id == id) {
			SP_ADDREF(sp);
			break;
		}
	}
	SPTREE_RUNLOCK();
	return (sp);
}

struct secpolicy *
key_newsp(void)
{
	struct secpolicy *sp;

	sp = malloc(sizeof(*sp), M_IPSEC_SP, M_NOWAIT | M_ZERO);
	if (sp != NULL)
		SP_INITREF(sp);
	return (sp);
}

/*
 * create secpolicy structure from sadb_x_policy structure.
 * NOTE: `state', `secpolicyindex' and 'id' in secpolicy structure
 * are not set, so must be set properly later.
 */
struct secpolicy *
key_msg2sp(struct sadb_x_policy *xpl0, size_t len, int *error)
{
	struct secpolicy *newsp;

	IPSEC_ASSERT(xpl0 != NULL, ("null xpl0"));
	IPSEC_ASSERT(len >= sizeof(*xpl0), ("policy too short: %zu", len));

	if (len != PFKEY_EXTLEN(xpl0)) {
		ipseclog((LOG_DEBUG, "%s: Invalid msg length.\n", __func__));
		*error = EINVAL;
		return NULL;
	}

	if ((newsp = key_newsp()) == NULL) {
		*error = ENOBUFS;
		return NULL;
	}

	newsp->spidx.dir = xpl0->sadb_x_policy_dir;
	newsp->policy = xpl0->sadb_x_policy_type;
	newsp->priority = xpl0->sadb_x_policy_priority;
	newsp->tcount = 0;

	/* check policy */
	switch (xpl0->sadb_x_policy_type) {
	case IPSEC_POLICY_DISCARD:
	case IPSEC_POLICY_NONE:
	case IPSEC_POLICY_ENTRUST:
	case IPSEC_POLICY_BYPASS:
		break;

	case IPSEC_POLICY_IPSEC:
	    {
		struct sadb_x_ipsecrequest *xisr;
		struct ipsecrequest *isr;
		int tlen;

		/* validity check */
		if (PFKEY_EXTLEN(xpl0) < sizeof(*xpl0)) {
			ipseclog((LOG_DEBUG, "%s: Invalid msg length.\n",
				__func__));
			key_freesp(&newsp);
			*error = EINVAL;
			return NULL;
		}

		tlen = PFKEY_EXTLEN(xpl0) - sizeof(*xpl0);
		xisr = (struct sadb_x_ipsecrequest *)(xpl0 + 1);

		while (tlen > 0) {
			/* length check */
			if (xisr->sadb_x_ipsecrequest_len < sizeof(*xisr)) {
				ipseclog((LOG_DEBUG, "%s: invalid ipsecrequest "
					"length.\n", __func__));
				key_freesp(&newsp);
				*error = EINVAL;
				return NULL;
			}

			if (newsp->tcount >= IPSEC_MAXREQ) {
				ipseclog((LOG_DEBUG,
				    "%s: too many ipsecrequests.\n",
				    __func__));
				key_freesp(&newsp);
				*error = EINVAL;
				return (NULL);
			}

			/* allocate request buffer */
			/* NB: data structure is zero'd */
			isr = ipsec_newisr();
			if (isr == NULL) {
				ipseclog((LOG_DEBUG,
				    "%s: No more memory.\n", __func__));
				key_freesp(&newsp);
				*error = ENOBUFS;
				return NULL;
			}

			newsp->req[newsp->tcount++] = isr;

			/* set values */
			switch (xisr->sadb_x_ipsecrequest_proto) {
			case IPPROTO_ESP:
			case IPPROTO_AH:
			case IPPROTO_IPCOMP:
				break;
			default:
				ipseclog((LOG_DEBUG,
				    "%s: invalid proto type=%u\n", __func__,
				    xisr->sadb_x_ipsecrequest_proto));
				key_freesp(&newsp);
				*error = EPROTONOSUPPORT;
				return NULL;
			}
			isr->saidx.proto =
			    (uint8_t)xisr->sadb_x_ipsecrequest_proto;

			switch (xisr->sadb_x_ipsecrequest_mode) {
			case IPSEC_MODE_TRANSPORT:
			case IPSEC_MODE_TUNNEL:
				break;
			case IPSEC_MODE_ANY:
			default:
				ipseclog((LOG_DEBUG,
				    "%s: invalid mode=%u\n", __func__,
				    xisr->sadb_x_ipsecrequest_mode));
				key_freesp(&newsp);
				*error = EINVAL;
				return NULL;
			}
			isr->saidx.mode = xisr->sadb_x_ipsecrequest_mode;

			switch (xisr->sadb_x_ipsecrequest_level) {
			case IPSEC_LEVEL_DEFAULT:
			case IPSEC_LEVEL_USE:
			case IPSEC_LEVEL_REQUIRE:
				break;
			case IPSEC_LEVEL_UNIQUE:
				/* validity check */
				/*
				 * If range violation of reqid, kernel will
				 * update it, don't refuse it.
				 */
				if (xisr->sadb_x_ipsecrequest_reqid
						> IPSEC_MANUAL_REQID_MAX) {
					ipseclog((LOG_DEBUG,
					    "%s: reqid=%d range "
					    "violation, updated by kernel.\n",
					    __func__,
					    xisr->sadb_x_ipsecrequest_reqid));
					xisr->sadb_x_ipsecrequest_reqid = 0;
				}

				/* allocate new reqid id if reqid is zero. */
				if (xisr->sadb_x_ipsecrequest_reqid == 0) {
					u_int32_t reqid;
					if ((reqid = key_newreqid()) == 0) {
						key_freesp(&newsp);
						*error = ENOBUFS;
						return NULL;
					}
					isr->saidx.reqid = reqid;
					xisr->sadb_x_ipsecrequest_reqid = reqid;
				} else {
				/* set it for manual keying. */
					isr->saidx.reqid =
					    xisr->sadb_x_ipsecrequest_reqid;
				}
				break;

			default:
				ipseclog((LOG_DEBUG, "%s: invalid level=%u\n",
					__func__,
					xisr->sadb_x_ipsecrequest_level));
				key_freesp(&newsp);
				*error = EINVAL;
				return NULL;
			}
			isr->level = xisr->sadb_x_ipsecrequest_level;

			/* set IP addresses if there */
			/* XXXAE: those are needed only for tunnel mode */
			if (xisr->sadb_x_ipsecrequest_len > sizeof(*xisr)) {
				struct sockaddr *paddr;

				paddr = (struct sockaddr *)(xisr + 1);
				/* validity check */
				if (paddr->sa_len
				    > sizeof(isr->saidx.src)) {
					ipseclog((LOG_DEBUG, "%s: invalid "
						"request address length.\n",
						__func__));
					key_freesp(&newsp);
					*error = EINVAL;
					return NULL;
				}
				bcopy(paddr, &isr->saidx.src, paddr->sa_len);
				paddr = (struct sockaddr *)((caddr_t)paddr +
				    paddr->sa_len);

				/* validity check */
				if (paddr->sa_len
				    > sizeof(isr->saidx.dst)) {
					ipseclog((LOG_DEBUG, "%s: invalid "
						"request address length.\n",
						__func__));
					key_freesp(&newsp);
					*error = EINVAL;
					return NULL;
				}
				/* AF family should match */
				if (paddr->sa_family !=
				    isr->saidx.src.sa.sa_family) {
					ipseclog((LOG_DEBUG, "%s: address "
					    "family doesn't match.\n",
						__func__));
					key_freesp(&newsp);
					*error = EINVAL;
					return (NULL);
				}
				bcopy(paddr, &isr->saidx.dst, paddr->sa_len);
			}
			tlen -= xisr->sadb_x_ipsecrequest_len;

			/* validity check */
			if (tlen < 0) {
				ipseclog((LOG_DEBUG, "%s: becoming tlen < 0.\n",
					__func__));
				key_freesp(&newsp);
				*error = EINVAL;
				return NULL;
			}

			xisr = (struct sadb_x_ipsecrequest *)((caddr_t)xisr
			                 + xisr->sadb_x_ipsecrequest_len);
		}
		/* XXXAE: LARVAL SP */
		if (newsp->tcount < 1) {
			ipseclog((LOG_DEBUG, "%s: valid IPSEC transforms "
			    "not found.\n", __func__));
			key_freesp(&newsp);
			*error = EINVAL;
			return (NULL);
		}
	    }
		break;
	default:
		ipseclog((LOG_DEBUG, "%s: invalid policy type.\n", __func__));
		key_freesp(&newsp);
		*error = EINVAL;
		return NULL;
	}

	*error = 0;
	return (newsp);
}

uint32_t
key_newreqid(void)
{
	static uint32_t auto_reqid = IPSEC_MANUAL_REQID_MAX + 1;

	if (auto_reqid == ~0)
		auto_reqid = IPSEC_MANUAL_REQID_MAX + 1;
	else
		auto_reqid++;

	/* XXX should be unique check */
	return (auto_reqid);
}

/*
 * copy secpolicy struct to sadb_x_policy structure indicated.
 */
static struct mbuf *
key_sp2mbuf(struct secpolicy *sp)
{
	struct mbuf *m;
	size_t tlen;

	tlen = key_getspreqmsglen(sp);
	m = m_get2(tlen, M_NOWAIT, MT_DATA, 0);
	if (m == NULL)
		return (NULL);
	m_align(m, tlen);
	m->m_len = tlen;
	if (key_sp2msg(sp, m->m_data, &tlen) != 0) {
		m_freem(m);
		return (NULL);
	}
	return (m);
}

int
key_sp2msg(struct secpolicy *sp, void *request, size_t *len)
{
	struct sadb_x_ipsecrequest *xisr;
	struct sadb_x_policy *xpl;
	struct ipsecrequest *isr;
	size_t xlen, ilen;
	caddr_t p;
	int error, i;

	IPSEC_ASSERT(sp != NULL, ("null policy"));

	xlen = sizeof(*xpl);
	if (*len < xlen)
		return (EINVAL);

	error = 0;
	bzero(request, *len);
	xpl = (struct sadb_x_policy *)request;
	xpl->sadb_x_policy_exttype = SADB_X_EXT_POLICY;
	xpl->sadb_x_policy_type = sp->policy;
	xpl->sadb_x_policy_dir = sp->spidx.dir;
	xpl->sadb_x_policy_id = sp->id;
	xpl->sadb_x_policy_priority = sp->priority;

	/* if is the policy for ipsec ? */
	if (sp->policy == IPSEC_POLICY_IPSEC) {
		p = (caddr_t)xpl + sizeof(*xpl);
		for (i = 0; i < sp->tcount; i++) {
			isr = sp->req[i];
			ilen = PFKEY_ALIGN8(sizeof(*xisr) +
			    isr->saidx.src.sa.sa_len +
			    isr->saidx.dst.sa.sa_len);
			xlen += ilen;
			if (xlen > *len) {
				error = ENOBUFS;
				/* Calculate needed size */
				continue;
			}
			xisr = (struct sadb_x_ipsecrequest *)p;
			xisr->sadb_x_ipsecrequest_len = ilen;
			xisr->sadb_x_ipsecrequest_proto = isr->saidx.proto;
			xisr->sadb_x_ipsecrequest_mode = isr->saidx.mode;
			xisr->sadb_x_ipsecrequest_level = isr->level;
			xisr->sadb_x_ipsecrequest_reqid = isr->saidx.reqid;

			p += sizeof(*xisr);
			bcopy(&isr->saidx.src, p, isr->saidx.src.sa.sa_len);
			p += isr->saidx.src.sa.sa_len;
			bcopy(&isr->saidx.dst, p, isr->saidx.dst.sa.sa_len);
			p += isr->saidx.dst.sa.sa_len;
		}
	}
	xpl->sadb_x_policy_len = PFKEY_UNIT64(xlen);
	if (error == 0)
		*len = xlen;
	else
		*len = sizeof(*xpl);
	return (error);
}

/* m will not be freed nor modified */
static struct mbuf *
key_gather_mbuf(struct mbuf *m, const struct sadb_msghdr *mhp,
    int ndeep, int nitem, ...)
{
	va_list ap;
	int idx;
	int i;
	struct mbuf *result = NULL, *n;
	int len;

	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));

	va_start(ap, nitem);
	for (i = 0; i < nitem; i++) {
		idx = va_arg(ap, int);
		if (idx < 0 || idx > SADB_EXT_MAX)
			goto fail;
		/* don't attempt to pull empty extension */
		if (idx == SADB_EXT_RESERVED && mhp->msg == NULL)
			continue;
		if (idx != SADB_EXT_RESERVED  &&
		    (mhp->ext[idx] == NULL || mhp->extlen[idx] == 0))
			continue;

		if (idx == SADB_EXT_RESERVED) {
			len = PFKEY_ALIGN8(sizeof(struct sadb_msg));

			IPSEC_ASSERT(len <= MHLEN, ("header too big %u", len));

			MGETHDR(n, M_NOWAIT, MT_DATA);
			if (!n)
				goto fail;
			n->m_len = len;
			n->m_next = NULL;
			m_copydata(m, 0, sizeof(struct sadb_msg),
			    mtod(n, caddr_t));
		} else if (i < ndeep) {
			len = mhp->extlen[idx];
			n = m_get2(len, M_NOWAIT, MT_DATA, 0);
			if (n == NULL)
				goto fail;
			m_align(n, len);
			n->m_len = len;
			m_copydata(m, mhp->extoff[idx], mhp->extlen[idx],
			    mtod(n, caddr_t));
		} else {
			n = m_copym(m, mhp->extoff[idx], mhp->extlen[idx],
			    M_NOWAIT);
		}
		if (n == NULL)
			goto fail;

		if (result)
			m_cat(result, n);
		else
			result = n;
	}
	va_end(ap);

	if ((result->m_flags & M_PKTHDR) != 0) {
		result->m_pkthdr.len = 0;
		for (n = result; n; n = n->m_next)
			result->m_pkthdr.len += n->m_len;
	}

	return result;

fail:
	m_freem(result);
	va_end(ap);
	return NULL;
}

/*
 * SADB_X_SPDADD, SADB_X_SPDSETIDX or SADB_X_SPDUPDATE processing
 * add an entry to SP database, when received
 *   <base, address(SD), (lifetime(H),) policy>
 * from the user(?).
 * Adding to SP database,
 * and send
 *   <base, address(SD), (lifetime(H),) policy>
 * to the socket which was send.
 *
 * SPDADD set a unique policy entry.
 * SPDSETIDX like SPDADD without a part of policy requests.
 * SPDUPDATE replace a unique policy entry.
 *
 * XXXAE: serialize this in PF_KEY to avoid races.
 * m will always be freed.
 */
static int
key_spdadd(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	struct secpolicyindex spidx;
	struct sadb_address *src0, *dst0;
	struct sadb_x_policy *xpl0, *xpl;
	struct sadb_lifetime *lft = NULL;
	struct secpolicy *newsp;
	int error;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	if (SADB_CHECKHDR(mhp, SADB_EXT_ADDRESS_SRC) ||
	    SADB_CHECKHDR(mhp, SADB_EXT_ADDRESS_DST) ||
	    SADB_CHECKHDR(mhp, SADB_X_EXT_POLICY)) {
		ipseclog((LOG_DEBUG,
		    "%s: invalid message: missing required header.\n",
		    __func__));
		return key_senderror(so, m, EINVAL);
	}
	if (SADB_CHECKLEN(mhp, SADB_EXT_ADDRESS_SRC) ||
	    SADB_CHECKLEN(mhp, SADB_EXT_ADDRESS_DST) ||
	    SADB_CHECKLEN(mhp, SADB_X_EXT_POLICY)) {
		ipseclog((LOG_DEBUG,
		    "%s: invalid message: wrong header size.\n", __func__));
		return key_senderror(so, m, EINVAL);
	}
	if (!SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_HARD)) {
		if (SADB_CHECKLEN(mhp, SADB_EXT_LIFETIME_HARD)) {
			ipseclog((LOG_DEBUG,
			    "%s: invalid message: wrong header size.\n",
			    __func__));
			return key_senderror(so, m, EINVAL);
		}
		lft = (struct sadb_lifetime *)mhp->ext[SADB_EXT_LIFETIME_HARD];
	}

	src0 = (struct sadb_address *)mhp->ext[SADB_EXT_ADDRESS_SRC];
	dst0 = (struct sadb_address *)mhp->ext[SADB_EXT_ADDRESS_DST];
	xpl0 = (struct sadb_x_policy *)mhp->ext[SADB_X_EXT_POLICY];

	/*
	 * Note: do not parse SADB_X_EXT_NAT_T_* here:
	 * we are processing traffic endpoints.
	 */

	/* check the direciton */
	switch (xpl0->sadb_x_policy_dir) {
	case IPSEC_DIR_INBOUND:
	case IPSEC_DIR_OUTBOUND:
		break;
	default:
		ipseclog((LOG_DEBUG, "%s: invalid SP direction.\n", __func__));
		return key_senderror(so, m, EINVAL);
	}
	/* key_spdadd() accepts DISCARD, NONE and IPSEC. */
	if (xpl0->sadb_x_policy_type != IPSEC_POLICY_DISCARD &&
	    xpl0->sadb_x_policy_type != IPSEC_POLICY_NONE &&
	    xpl0->sadb_x_policy_type != IPSEC_POLICY_IPSEC) {
		ipseclog((LOG_DEBUG, "%s: invalid policy type.\n", __func__));
		return key_senderror(so, m, EINVAL);
	}

	/* policy requests are mandatory when action is ipsec. */
	if (xpl0->sadb_x_policy_type == IPSEC_POLICY_IPSEC &&
	    mhp->extlen[SADB_X_EXT_POLICY] <= sizeof(*xpl0)) {
		ipseclog((LOG_DEBUG,
		    "%s: policy requests required.\n", __func__));
		return key_senderror(so, m, EINVAL);
	}

	error = key_checksockaddrs((struct sockaddr *)(src0 + 1),
	    (struct sockaddr *)(dst0 + 1));
	if (error != 0 ||
	    src0->sadb_address_proto != dst0->sadb_address_proto) {
		ipseclog((LOG_DEBUG, "%s: invalid sockaddr.\n", __func__));
		return key_senderror(so, m, error);
	}
	/* make secindex */
	KEY_SETSECSPIDX(xpl0->sadb_x_policy_dir,
	                src0 + 1,
	                dst0 + 1,
	                src0->sadb_address_prefixlen,
	                dst0->sadb_address_prefixlen,
	                src0->sadb_address_proto,
	                &spidx);
	/* Checking there is SP already or not. */
	newsp = key_getsp(&spidx);
	if (newsp != NULL) {
		if (mhp->msg->sadb_msg_type == SADB_X_SPDUPDATE) {
			KEYDBG(KEY_STAMP,
			    printf("%s: unlink SP(%p) for SPDUPDATE\n",
				__func__, newsp));
			KEYDBG(KEY_DATA, kdebug_secpolicy(newsp));
			key_unlink(newsp);
			key_freesp(&newsp);
		} else {
			key_freesp(&newsp);
			ipseclog((LOG_DEBUG, "%s: a SP entry exists already.",
			    __func__));
			return (key_senderror(so, m, EEXIST));
		}
	}

	/* allocate new SP entry */
	if ((newsp = key_msg2sp(xpl0, PFKEY_EXTLEN(xpl0), &error)) == NULL) {
		return key_senderror(so, m, error);
	}

	newsp->lastused = newsp->created = time_second;
	newsp->lifetime = lft ? lft->sadb_lifetime_addtime : 0;
	newsp->validtime = lft ? lft->sadb_lifetime_usetime : 0;
	bcopy(&spidx, &newsp->spidx, sizeof(spidx));

	/* XXXAE: there is race between key_getsp() and key_insertsp() */
	SPTREE_WLOCK();
	if ((newsp->id = key_getnewspid()) == 0) {
		SPTREE_WUNLOCK();
		key_freesp(&newsp);
		return key_senderror(so, m, ENOBUFS);
	}
	key_insertsp(newsp);
	SPTREE_WUNLOCK();

	KEYDBG(KEY_STAMP,
	    printf("%s: SP(%p)\n", __func__, newsp));
	KEYDBG(KEY_DATA, kdebug_secpolicy(newsp));

    {
	struct mbuf *n, *mpolicy;
	struct sadb_msg *newmsg;
	int off;

	/*
	 * Note: do not send SADB_X_EXT_NAT_T_* here:
	 * we are sending traffic endpoints.
	 */

	/* create new sadb_msg to reply. */
	if (lft) {
		n = key_gather_mbuf(m, mhp, 2, 5, SADB_EXT_RESERVED,
		    SADB_X_EXT_POLICY, SADB_EXT_LIFETIME_HARD,
		    SADB_EXT_ADDRESS_SRC, SADB_EXT_ADDRESS_DST);
	} else {
		n = key_gather_mbuf(m, mhp, 2, 4, SADB_EXT_RESERVED,
		    SADB_X_EXT_POLICY,
		    SADB_EXT_ADDRESS_SRC, SADB_EXT_ADDRESS_DST);
	}
	if (!n)
		return key_senderror(so, m, ENOBUFS);

	if (n->m_len < sizeof(*newmsg)) {
		n = m_pullup(n, sizeof(*newmsg));
		if (!n)
			return key_senderror(so, m, ENOBUFS);
	}
	newmsg = mtod(n, struct sadb_msg *);
	newmsg->sadb_msg_errno = 0;
	newmsg->sadb_msg_len = PFKEY_UNIT64(n->m_pkthdr.len);

	off = 0;
	mpolicy = m_pulldown(n, PFKEY_ALIGN8(sizeof(struct sadb_msg)),
	    sizeof(*xpl), &off);
	if (mpolicy == NULL) {
		/* n is already freed */
		return key_senderror(so, m, ENOBUFS);
	}
	xpl = (struct sadb_x_policy *)(mtod(mpolicy, caddr_t) + off);
	if (xpl->sadb_x_policy_exttype != SADB_X_EXT_POLICY) {
		m_freem(n);
		return key_senderror(so, m, EINVAL);
	}
	xpl->sadb_x_policy_id = newsp->id;

	m_freem(m);
	return key_sendup_mbuf(so, n, KEY_SENDUP_ALL);
    }
}

/*
 * get new policy id.
 * OUT:
 *	0:	failure.
 *	others: success.
 */
static uint32_t
key_getnewspid(void)
{
	struct secpolicy *sp;
	uint32_t newid = 0;
	int count = V_key_spi_trycnt;	/* XXX */

	SPTREE_WLOCK_ASSERT();
	while (count--) {
		if (V_policy_id == ~0) /* overflowed */
			newid = V_policy_id = 1;
		else
			newid = ++V_policy_id;
		LIST_FOREACH(sp, SPHASH_HASH(newid), idhash) {
			if (sp->id == newid)
				break;
		}
		if (sp == NULL)
			break;
	}
	if (count == 0 || newid == 0) {
		ipseclog((LOG_DEBUG, "%s: failed to allocate policy id.\n",
		    __func__));
		return (0);
	}
	return (newid);
}

/*
 * SADB_SPDDELETE processing
 * receive
 *   <base, address(SD), policy(*)>
 * from the user(?), and set SADB_SASTATE_DEAD,
 * and send,
 *   <base, address(SD), policy(*)>
 * to the ikmpd.
 * policy(*) including direction of policy.
 *
 * m will always be freed.
 */
static int
key_spddelete(struct socket *so, struct mbuf *m,
    const struct sadb_msghdr *mhp)
{
	struct secpolicyindex spidx;
	struct sadb_address *src0, *dst0;
	struct sadb_x_policy *xpl0;
	struct secpolicy *sp;

	IPSEC_ASSERT(so != NULL, ("null so"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	if (SADB_CHECKHDR(mhp, SADB_EXT_ADDRESS_SRC) ||
	    SADB_CHECKHDR(mhp, SADB_EXT_ADDRESS_DST) ||
	    SADB_CHECKHDR(mhp, SADB_X_EXT_POLICY)) {
		ipseclog((LOG_DEBUG,
		    "%s: invalid message: missing required header.\n",
		    __func__));
		return key_senderror(so, m, EINVAL);
	}
	if (SADB_CHECKLEN(mhp, SADB_EXT_ADDRESS_SRC) ||
	    SADB_CHECKLEN(mhp, SADB_EXT_ADDRESS_DST) ||
	    SADB_CHECKLEN(mhp, SADB_X_EXT_POLICY)) {
		ipseclog((LOG_DEBUG,
		    "%s: invalid message: wrong header size.\n", __func__));
		return key_senderror(so, m, EINVAL);
	}

	src0 = (struct sadb_address *)mhp->ext[SADB_EXT_ADDRESS_SRC];
	dst0 = (struct sadb_address *)mhp->ext[SADB_EXT_ADDRESS_DST];
	xpl0 = (struct sadb_x_policy *)mhp->ext[SADB_X_EXT_POLICY];

	/* check the direciton */
	switch (xpl0->sadb_x_policy_dir) {
	case IPSEC_DIR_INBOUND:
	case IPSEC_DIR_OUTBOUND:
		break;
	default:
		ipseclog((LOG_DEBUG, "%s: invalid SP direction.\n", __func__));
		return key_senderror(so, m, EINVAL);
	}
	/* Only DISCARD, NONE and IPSEC are allowed */
	if (xpl0->sadb_x_policy_type != IPSEC_POLICY_DISCARD &&
	    xpl0->sadb_x_policy_type != IPSEC_POLICY_NONE &&
	    xpl0->sadb_x_policy_type != IPSEC_POLICY_IPSEC) {
		ipseclog((LOG_DEBUG, "%s: invalid policy type.\n", __func__));
		return key_senderror(so, m, EINVAL);
	}

	/*
	 * Note: do not parse SADB_X_EXT_NAT_T_* here:
	 * we are processing traffic endpoints.
	 */

	if (key_checksockaddrs((struct sockaddr *)(src0 + 1),
	    (struct sockaddr *)(dst0 + 1)) != 0 ||
	    src0->sadb_address_proto != dst0->sadb_address_proto) {
		ipseclog((LOG_DEBUG, "%s: invalid sockaddr.\n", __func__));
		return key_senderror(so, m, EINVAL);
	}
	/* make secindex */
	KEY_SETSECSPIDX(xpl0->sadb_x_policy_dir,
	                src0 + 1,
	                dst0 + 1,
	                src0->sadb_address_prefixlen,
	                dst0->sadb_address_prefixlen,
	                src0->sadb_address_proto,
	                &spidx);

	/* Is there SP in SPD ? */
	if ((sp = key_getsp(&spidx)) == NULL) {
		ipseclog((LOG_DEBUG, "%s: no SP found.\n", __func__));
		return key_senderror(so, m, EINVAL);
	}

	/* save policy id to buffer to be returned. */
	xpl0->sadb_x_policy_id = sp->id;

	KEYDBG(KEY_STAMP,
	    printf("%s: SP(%p)\n", __func__, sp));
	KEYDBG(KEY_DATA, kdebug_secpolicy(sp));
	key_unlink(sp);
	key_freesp(&sp);

    {
	struct mbuf *n;
	struct sadb_msg *newmsg;

	/*
	 * Note: do not send SADB_X_EXT_NAT_T_* here:
	 * we are sending traffic endpoints.
	 */

	/* create new sadb_msg to reply. */
	n = key_gather_mbuf(m, mhp, 1, 4, SADB_EXT_RESERVED,
	    SADB_X_EXT_POLICY, SADB_EXT_ADDRESS_SRC, SADB_EXT_ADDRESS_DST);
	if (!n)
		return key_senderror(so, m, ENOBUFS);

	newmsg = mtod(n, struct sadb_msg *);
	newmsg->sadb_msg_errno = 0;
	newmsg->sadb_msg_len = PFKEY_UNIT64(n->m_pkthdr.len);

	m_freem(m);
	return key_sendup_mbuf(so, n, KEY_SENDUP_ALL);
    }
}

/*
 * SADB_SPDDELETE2 processing
 * receive
 *   <base, policy(*)>
 * from the user(?), and set SADB_SASTATE_DEAD,
 * and send,
 *   <base, policy(*)>
 * to the ikmpd.
 * policy(*) including direction of policy.
 *
 * m will always be freed.
 */
static int
key_spddelete2(struct socket *so, struct mbuf *m,
    const struct sadb_msghdr *mhp)
{
	struct secpolicy *sp;
	uint32_t id;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	if (SADB_CHECKHDR(mhp, SADB_X_EXT_POLICY) ||
	    SADB_CHECKLEN(mhp, SADB_X_EXT_POLICY)) {
		ipseclog((LOG_DEBUG, "%s: invalid message is passed.\n",
		    __func__));
		return key_senderror(so, m, EINVAL);
	}

	id = ((struct sadb_x_policy *)
	    mhp->ext[SADB_X_EXT_POLICY])->sadb_x_policy_id;

	/* Is there SP in SPD ? */
	if ((sp = key_getspbyid(id)) == NULL) {
		ipseclog((LOG_DEBUG, "%s: no SP found for id %u.\n",
		    __func__, id));
		return key_senderror(so, m, EINVAL);
	}

	KEYDBG(KEY_STAMP,
	    printf("%s: SP(%p)\n", __func__, sp));
	KEYDBG(KEY_DATA, kdebug_secpolicy(sp));
	key_unlink(sp);
	key_freesp(&sp);

    {
	struct mbuf *n, *nn;
	struct sadb_msg *newmsg;
	int off, len;

	/* create new sadb_msg to reply. */
	len = PFKEY_ALIGN8(sizeof(struct sadb_msg));

	MGETHDR(n, M_NOWAIT, MT_DATA);
	if (n && len > MHLEN) {
		if (!(MCLGET(n, M_NOWAIT))) {
			m_freem(n);
			n = NULL;
		}
	}
	if (!n)
		return key_senderror(so, m, ENOBUFS);

	n->m_len = len;
	n->m_next = NULL;
	off = 0;

	m_copydata(m, 0, sizeof(struct sadb_msg), mtod(n, caddr_t) + off);
	off += PFKEY_ALIGN8(sizeof(struct sadb_msg));

	IPSEC_ASSERT(off == len, ("length inconsistency (off %u len %u)",
		off, len));

	n->m_next = m_copym(m, mhp->extoff[SADB_X_EXT_POLICY],
	    mhp->extlen[SADB_X_EXT_POLICY], M_NOWAIT);
	if (!n->m_next) {
		m_freem(n);
		return key_senderror(so, m, ENOBUFS);
	}

	n->m_pkthdr.len = 0;
	for (nn = n; nn; nn = nn->m_next)
		n->m_pkthdr.len += nn->m_len;

	newmsg = mtod(n, struct sadb_msg *);
	newmsg->sadb_msg_errno = 0;
	newmsg->sadb_msg_len = PFKEY_UNIT64(n->m_pkthdr.len);

	m_freem(m);
	return key_sendup_mbuf(so, n, KEY_SENDUP_ALL);
    }
}

/*
 * SADB_X_SPDGET processing
 * receive
 *   <base, policy(*)>
 * from the user(?),
 * and send,
 *   <base, address(SD), policy>
 * to the ikmpd.
 * policy(*) including direction of policy.
 *
 * m will always be freed.
 */
static int
key_spdget(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	struct secpolicy *sp;
	struct mbuf *n;
	uint32_t id;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	if (SADB_CHECKHDR(mhp, SADB_X_EXT_POLICY) ||
	    SADB_CHECKLEN(mhp, SADB_X_EXT_POLICY)) {
		ipseclog((LOG_DEBUG, "%s: invalid message is passed.\n",
		    __func__));
		return key_senderror(so, m, EINVAL);
	}

	id = ((struct sadb_x_policy *)
	    mhp->ext[SADB_X_EXT_POLICY])->sadb_x_policy_id;

	/* Is there SP in SPD ? */
	if ((sp = key_getspbyid(id)) == NULL) {
		ipseclog((LOG_DEBUG, "%s: no SP found for id %u.\n",
		    __func__, id));
		return key_senderror(so, m, ENOENT);
	}

	n = key_setdumpsp(sp, SADB_X_SPDGET, mhp->msg->sadb_msg_seq,
	    mhp->msg->sadb_msg_pid);
	key_freesp(&sp);
	if (n != NULL) {
		m_freem(m);
		return key_sendup_mbuf(so, n, KEY_SENDUP_ONE);
	} else
		return key_senderror(so, m, ENOBUFS);
}

/*
 * SADB_X_SPDACQUIRE processing.
 * Acquire policy and SA(s) for a *OUTBOUND* packet.
 * send
 *   <base, policy(*)>
 * to KMD, and expect to receive
 *   <base> with SADB_X_SPDACQUIRE if error occurred,
 * or
 *   <base, policy>
 * with SADB_X_SPDUPDATE from KMD by PF_KEY.
 * policy(*) is without policy requests.
 *
 *    0     : succeed
 *    others: error number
 */
int
key_spdacquire(struct secpolicy *sp)
{
	struct mbuf *result = NULL, *m;
	struct secspacq *newspacq;

	IPSEC_ASSERT(sp != NULL, ("null secpolicy"));
	IPSEC_ASSERT(sp->req == NULL, ("policy exists"));
	IPSEC_ASSERT(sp->policy == IPSEC_POLICY_IPSEC,
		("policy not IPSEC %u", sp->policy));

	/* Get an entry to check whether sent message or not. */
	newspacq = key_getspacq(&sp->spidx);
	if (newspacq != NULL) {
		if (V_key_blockacq_count < newspacq->count) {
			/* reset counter and do send message. */
			newspacq->count = 0;
		} else {
			/* increment counter and do nothing. */
			newspacq->count++;
			SPACQ_UNLOCK();
			return (0);
		}
		SPACQ_UNLOCK();
	} else {
		/* make new entry for blocking to send SADB_ACQUIRE. */
		newspacq = key_newspacq(&sp->spidx);
		if (newspacq == NULL)
			return ENOBUFS;
	}

	/* create new sadb_msg to reply. */
	m = key_setsadbmsg(SADB_X_SPDACQUIRE, 0, 0, 0, 0, 0);
	if (!m)
		return ENOBUFS;

	result = m;

	result->m_pkthdr.len = 0;
	for (m = result; m; m = m->m_next)
		result->m_pkthdr.len += m->m_len;

	mtod(result, struct sadb_msg *)->sadb_msg_len =
	    PFKEY_UNIT64(result->m_pkthdr.len);

	return key_sendup_mbuf(NULL, m, KEY_SENDUP_REGISTERED);
}

/*
 * SADB_SPDFLUSH processing
 * receive
 *   <base>
 * from the user, and free all entries in secpctree.
 * and send,
 *   <base>
 * to the user.
 * NOTE: what to do is only marking SADB_SASTATE_DEAD.
 *
 * m will always be freed.
 */
static int
key_spdflush(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	struct secpolicy_queue drainq;
	struct sadb_msg *newmsg;
	struct secpolicy *sp, *nextsp;
	u_int dir;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	if (m->m_len != PFKEY_ALIGN8(sizeof(struct sadb_msg)))
		return key_senderror(so, m, EINVAL);

	TAILQ_INIT(&drainq);
	SPTREE_WLOCK();
	for (dir = 0; dir < IPSEC_DIR_MAX; dir++) {
		TAILQ_CONCAT(&drainq, &V_sptree[dir], chain);
	}
	/*
	 * We need to set state to DEAD for each policy to be sure,
	 * that another thread won't try to unlink it.
	 * Also remove SP from sphash.
	 */
	TAILQ_FOREACH(sp, &drainq, chain) {
		sp->state = IPSEC_SPSTATE_DEAD;
		LIST_REMOVE(sp, idhash);
	}
	V_sp_genid++;
	SPTREE_WUNLOCK();
	sp = TAILQ_FIRST(&drainq);
	while (sp != NULL) {
		nextsp = TAILQ_NEXT(sp, chain);
		KEY_FREESP(&sp);
		sp = nextsp;
	}

	if (sizeof(struct sadb_msg) > m->m_len + M_TRAILINGSPACE(m)) {
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		return key_senderror(so, m, ENOBUFS);
	}

	if (m->m_next)
		m_freem(m->m_next);
	m->m_next = NULL;
	m->m_pkthdr.len = m->m_len = PFKEY_ALIGN8(sizeof(struct sadb_msg));
	newmsg = mtod(m, struct sadb_msg *);
	newmsg->sadb_msg_errno = 0;
	newmsg->sadb_msg_len = PFKEY_UNIT64(m->m_pkthdr.len);

	return key_sendup_mbuf(so, m, KEY_SENDUP_ALL);
}

/*
 * SADB_SPDDUMP processing
 * receive
 *   <base>
 * from the user, and dump all SP leaves
 * and send,
 *   <base> .....
 * to the ikmpd.
 *
 * m will always be freed.
 */
static int
key_spddump(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	SPTREE_RLOCK_TRACKER;
	struct secpolicy *sp;
	int cnt;
	u_int dir;
	struct mbuf *n;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	/* search SPD entry and get buffer size. */
	cnt = 0;
	SPTREE_RLOCK();
	for (dir = 0; dir < IPSEC_DIR_MAX; dir++) {
		TAILQ_FOREACH(sp, &V_sptree[dir], chain) {
			cnt++;
		}
	}

	if (cnt == 0) {
		SPTREE_RUNLOCK();
		return key_senderror(so, m, ENOENT);
	}

	for (dir = 0; dir < IPSEC_DIR_MAX; dir++) {
		TAILQ_FOREACH(sp, &V_sptree[dir], chain) {
			--cnt;
			n = key_setdumpsp(sp, SADB_X_SPDDUMP, cnt,
			    mhp->msg->sadb_msg_pid);

			if (n)
				key_sendup_mbuf(so, n, KEY_SENDUP_ONE);
		}
	}

	SPTREE_RUNLOCK();
	m_freem(m);
	return 0;
}

static struct mbuf *
key_setdumpsp(struct secpolicy *sp, u_int8_t type, u_int32_t seq,
    u_int32_t pid)
{
	struct mbuf *result = NULL, *m;
	struct seclifetime lt;

	m = key_setsadbmsg(type, 0, SADB_SATYPE_UNSPEC, seq, pid, sp->refcnt);
	if (!m)
		goto fail;
	result = m;

	/*
	 * Note: do not send SADB_X_EXT_NAT_T_* here:
	 * we are sending traffic endpoints.
	 */
	m = key_setsadbaddr(SADB_EXT_ADDRESS_SRC,
	    &sp->spidx.src.sa, sp->spidx.prefs,
	    sp->spidx.ul_proto);
	if (!m)
		goto fail;
	m_cat(result, m);

	m = key_setsadbaddr(SADB_EXT_ADDRESS_DST,
	    &sp->spidx.dst.sa, sp->spidx.prefd,
	    sp->spidx.ul_proto);
	if (!m)
		goto fail;
	m_cat(result, m);

	m = key_sp2msg(sp);
	if (!m)
		goto fail;
	m_cat(result, m);

	if(sp->lifetime){
		lt.addtime=sp->created;
		lt.usetime= sp->lastused;
		m = key_setlifetime(&lt, SADB_EXT_LIFETIME_CURRENT);
		if (!m)
			goto fail;
		m_cat(result, m);
		
		lt.addtime=sp->lifetime;
		lt.usetime= sp->validtime;
		m = key_setlifetime(&lt, SADB_EXT_LIFETIME_HARD);
		if (!m)
			goto fail;
		m_cat(result, m);
	}

	if ((result->m_flags & M_PKTHDR) == 0)
		goto fail;

	if (result->m_len < sizeof(struct sadb_msg)) {
		result = m_pullup(result, sizeof(struct sadb_msg));
		if (result == NULL)
			goto fail;
	}

	result->m_pkthdr.len = 0;
	for (m = result; m; m = m->m_next)
		result->m_pkthdr.len += m->m_len;

	mtod(result, struct sadb_msg *)->sadb_msg_len =
	    PFKEY_UNIT64(result->m_pkthdr.len);

	return result;

fail:
	m_freem(result);
	return NULL;
}

/*
 * get PFKEY message length for security policy and request.
 */
static size_t
key_getspreqmsglen(struct secpolicy *sp)
{
	size_t tlen, len;
	int i;

	tlen = sizeof(struct sadb_x_policy);
	/* if is the policy for ipsec ? */
	if (sp->policy != IPSEC_POLICY_IPSEC)
		return (tlen);

	/* get length of ipsec requests */
	for (i = 0; i < sp->tcount; i++) {
		len = sizeof(struct sadb_x_ipsecrequest)
			+ sp->req[i]->saidx.src.sa.sa_len
			+ sp->req[i]->saidx.dst.sa.sa_len;

		tlen += PFKEY_ALIGN8(len);
	}
	return (tlen);
}

/*
 * SADB_SPDEXPIRE processing
 * send
 *   <base, address(SD), lifetime(CH), policy>
 * to KMD by PF_KEY.
 *
 * OUT:	0	: succeed
 *	others	: error number
 */
static int
key_spdexpire(struct secpolicy *sp)
{
	struct sadb_lifetime *lt;
	struct mbuf *result = NULL, *m;
	int len, error = -1;

	IPSEC_ASSERT(sp != NULL, ("null secpolicy"));

	KEYDBG(KEY_STAMP,
	    printf("%s: SP(%p)\n", __func__, sp));
	KEYDBG(KEY_DATA, kdebug_secpolicy(sp));

	/* set msg header */
	m = key_setsadbmsg(SADB_X_SPDEXPIRE, 0, 0, 0, 0, 0);
	if (!m) {
		error = ENOBUFS;
		goto fail;
	}
	result = m;

	/* create lifetime extension (current and hard) */
	len = PFKEY_ALIGN8(sizeof(*lt)) * 2;
	m = m_get2(len, M_NOWAIT, MT_DATA, 0);
	if (m == NULL) {
		error = ENOBUFS;
		goto fail;
	}
	m_align(m, len);
	m->m_len = len;
	bzero(mtod(m, caddr_t), len);
	lt = mtod(m, struct sadb_lifetime *);
	lt->sadb_lifetime_len = PFKEY_UNIT64(sizeof(struct sadb_lifetime));
	lt->sadb_lifetime_exttype = SADB_EXT_LIFETIME_CURRENT;
	lt->sadb_lifetime_allocations = 0;
	lt->sadb_lifetime_bytes = 0;
	lt->sadb_lifetime_addtime = sp->created;
	lt->sadb_lifetime_usetime = sp->lastused;
	lt = (struct sadb_lifetime *)(mtod(m, caddr_t) + len / 2);
	lt->sadb_lifetime_len = PFKEY_UNIT64(sizeof(struct sadb_lifetime));
	lt->sadb_lifetime_exttype = SADB_EXT_LIFETIME_HARD;
	lt->sadb_lifetime_allocations = 0;
	lt->sadb_lifetime_bytes = 0;
	lt->sadb_lifetime_addtime = sp->lifetime;
	lt->sadb_lifetime_usetime = sp->validtime;
	m_cat(result, m);

	/*
	 * Note: do not send SADB_X_EXT_NAT_T_* here:
	 * we are sending traffic endpoints.
	 */

	/* set sadb_address for source */
	m = key_setsadbaddr(SADB_EXT_ADDRESS_SRC,
	    &sp->spidx.src.sa,
	    sp->spidx.prefs, sp->spidx.ul_proto);
	if (!m) {
		error = ENOBUFS;
		goto fail;
	}
	m_cat(result, m);

	/* set sadb_address for destination */
	m = key_setsadbaddr(SADB_EXT_ADDRESS_DST,
	    &sp->spidx.dst.sa,
	    sp->spidx.prefd, sp->spidx.ul_proto);
	if (!m) {
		error = ENOBUFS;
		goto fail;
	}
	m_cat(result, m);

	/* set secpolicy */
	m = key_sp2mbuf(sp);
	if (!m) {
		error = ENOBUFS;
		goto fail;
	}
	m_cat(result, m);

	if ((result->m_flags & M_PKTHDR) == 0) {
		error = EINVAL;
		goto fail;
	}

	if (result->m_len < sizeof(struct sadb_msg)) {
		result = m_pullup(result, sizeof(struct sadb_msg));
		if (result == NULL) {
			error = ENOBUFS;
			goto fail;
		}
	}

	result->m_pkthdr.len = 0;
	for (m = result; m; m = m->m_next)
		result->m_pkthdr.len += m->m_len;

	mtod(result, struct sadb_msg *)->sadb_msg_len =
	    PFKEY_UNIT64(result->m_pkthdr.len);

	return key_sendup_mbuf(NULL, result, KEY_SENDUP_REGISTERED);

 fail:
	if (result)
		m_freem(result);
	return error;
}

/* %%% SAD management */
/*
 * allocating and initialize new SA head.
 * OUT:	NULL	: failure due to the lack of memory.
 *	others	: pointer to new SA head.
 */
static struct secashead *
key_newsah(struct secasindex *saidx)
{
	struct secashead *sah;

	sah = malloc(sizeof(struct secashead), M_IPSEC_SAH,
	    M_NOWAIT | M_ZERO);
	if (sah == NULL)
		return (NULL);
	TAILQ_INIT(&sah->savtree_larval);
	TAILQ_INIT(&sah->savtree_alive);
	sah->saidx = *saidx;
	sah->state = SADB_SASTATE_DEAD;
	SAH_INITREF(sah);

	KEYDBG(KEY_STAMP,
	    printf("%s: SAH(%p)\n", __func__, sah));
	KEYDBG(KEY_DATA, kdebug_secash(sah, NULL));
	return (sah);
}

static void
key_freesah(struct secashead **psah)
{
	struct secashead *sah = *psah;

	if (SAH_DELREF(sah) == 0)
		return;

	KEYDBG(KEY_STAMP,
	    printf("%s: last reference to SAH(%p)\n", __func__, sah));
	KEYDBG(KEY_DATA, kdebug_secash(sah, NULL));

	*psah = NULL;
	key_delsah(sah);
}

static void
key_delsah(struct secashead *sah)
{
	IPSEC_ASSERT(sah != NULL, ("NULL sah"));
	IPSEC_ASSERT(sah->state == SADB_SASTATE_DEAD,
	    ("Attempt to free non DEAD SAH %p", sah));
	IPSEC_ASSERT(TAILQ_EMPTY(&sah->savtree_larval),
	    ("Attempt to free SAH %p with LARVAL SA", sah));
	IPSEC_ASSERT(TAILQ_EMPTY(&sah->savtree_alive),
	    ("Attempt to free SAH %p with ALIVE SA", sah));

	free(sah, M_IPSEC_SAH);
}

/*
 * allocating a new SA for key_add() and key_getspi() call,
 * and copy the values of mhp into new buffer.
 * When SAD message type is SADB_GETSPI set SA state to LARVAL.
 * For SADB_ADD create and initialize SA with MATURE state.
 * OUT:	NULL	: fail
 *	others	: pointer to new secasvar.
 */
static struct secasvar *
key_newsav(const struct sadb_msghdr *mhp, struct secasindex *saidx,
    uint32_t spi, int *errp)
{
	struct secashead *sah;
	struct secasvar *sav;
	int isnew;

	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));
	IPSEC_ASSERT(mhp->msg->sadb_msg_type == SADB_GETSPI ||
	    mhp->msg->sadb_msg_type == SADB_ADD, ("wrong message type"));

	sav = NULL;
	sah = NULL;
	/* check SPI value */
	switch (saidx->proto) {
	case IPPROTO_ESP:
	case IPPROTO_AH:
		/*
		 * RFC 4302, 2.4. Security Parameters Index (SPI), SPI values
		 * 1-255 reserved by IANA for future use,
		 * 0 for implementation specific, local use.
		 */
		if (ntohl(spi) <= 255) {
			ipseclog((LOG_DEBUG, "%s: illegal range of SPI %u.\n",
			    __func__, ntohl(spi)));
			*errp = EINVAL;
			goto done;
		}
		break;
	}

	sav = malloc(sizeof(struct secasvar), M_IPSEC_SA, M_NOWAIT | M_ZERO);
	if (sav == NULL) {
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		*errp = ENOBUFS;
		goto done;
	}
	sav->lft_c = uma_zalloc(V_key_lft_zone, M_NOWAIT);
	if (sav->lft_c == NULL) {
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		free(sav, M_IPSEC_SA), sav = NULL;
		*errp = ENOBUFS;
		goto done;
	}
	counter_u64_zero(sav->lft_c_allocations);
	counter_u64_zero(sav->lft_c_bytes);

	sav->spi = spi;
	sav->seq = mhp->msg->sadb_msg_seq;
	sav->state = SADB_SASTATE_LARVAL;
	sav->pid = (pid_t)mhp->msg->sadb_msg_pid;
	SAV_INITREF(sav);
	SECASVAR_LOCK_INIT(sav);
again:
	sah = key_getsah(saidx);
	if (sah == NULL) {
		/* create a new SA index */
		sah = key_newsah(saidx);
		if (sah == NULL) {
			ipseclog((LOG_DEBUG,
			    "%s: No more memory.\n", __func__));
			*errp = ENOBUFS;
			goto done;
		}
		isnew = 1;
	} else
		isnew = 0;

	sav->sah = sah;
	if (mhp->msg->sadb_msg_type == SADB_GETSPI) {
		sav->created = time_second;
	} else if (sav->state == SADB_SASTATE_LARVAL) {
		/*
		 * Do not call key_setsaval() second time in case
		 * of `goto again`. We will have MATURE state.
		 */
		*errp = key_setsaval(sav, mhp);
		if (*errp != 0)
			goto done;
		sav->state = SADB_SASTATE_MATURE;
	}

	SAHTREE_WLOCK();
	/*
	 * Check that existing SAH wasn't unlinked.
	 * Since we didn't hold the SAHTREE lock, it is possible,
	 * that callout handler or key_flush() or key_delete() could
	 * unlink this SAH.
	 */
	if (isnew == 0 && sah->state == SADB_SASTATE_DEAD) {
		SAHTREE_WUNLOCK();
		key_freesah(&sah);	/* reference from key_getsah() */
		goto again;
	}
	if (isnew != 0) {
		/*
		 * Add new SAH into SADB.
		 *
		 * XXXAE: we can serialize key_add and key_getspi calls, so
		 * several threads will not fight in the race.
		 * Otherwise we should check under SAHTREE lock, that this
		 * SAH would not added twice.
		 */
		TAILQ_INSERT_HEAD(&V_sahtree, sah, chain);
		/* Add new SAH into hash by addresses */
		LIST_INSERT_HEAD(SAHADDRHASH_HASH(saidx), sah, addrhash);
		/* Now we are linked in the chain */
		sah->state = SADB_SASTATE_MATURE;
		/*
		 * SAV references this new SAH.
		 * In case of existing SAH we reuse reference
		 * from key_getsah().
		 */
		SAH_ADDREF(sah);
	}
	/* Link SAV with SAH */
	if (sav->state == SADB_SASTATE_MATURE)
		TAILQ_INSERT_HEAD(&sah->savtree_alive, sav, chain);
	else
		TAILQ_INSERT_HEAD(&sah->savtree_larval, sav, chain);
	/* Add SAV into SPI hash */
	LIST_INSERT_HEAD(SAVHASH_HASH(sav->spi), sav, spihash);
	SAHTREE_WUNLOCK();
	*errp = 0;	/* success */
done:
	if (*errp != 0) {
		if (sav != NULL) {
			SECASVAR_LOCK_DESTROY(sav);
			uma_zfree(V_key_lft_zone, sav->lft_c);
			free(sav, M_IPSEC_SA), sav = NULL;
		}
		if (sah != NULL)
			key_freesah(&sah);
	}
	return (sav);
}

/*
 * free() SA variable entry.
 */
static void
key_cleansav(struct secasvar *sav)
{

	/*
	 * Cleanup xform state.  Note that zeroize'ing causes the
	 * keys to be cleared; otherwise we must do it ourself.
	 */
	if (sav->tdb_xform != NULL) {
		sav->tdb_xform->xf_zeroize(sav);
		sav->tdb_xform = NULL;
	} else {
		if (sav->key_auth != NULL)
			bzero(sav->key_auth->key_data, _KEYLEN(sav->key_auth));
		if (sav->key_enc != NULL)
			bzero(sav->key_enc->key_data, _KEYLEN(sav->key_enc));
	}
	if (sav->key_auth != NULL) {
		if (sav->key_auth->key_data != NULL)
			free(sav->key_auth->key_data, M_IPSEC_MISC);
		free(sav->key_auth, M_IPSEC_MISC);
		sav->key_auth = NULL;
	}
	if (sav->key_enc != NULL) {
		if (sav->key_enc->key_data != NULL)
			free(sav->key_enc->key_data, M_IPSEC_MISC);
		free(sav->key_enc, M_IPSEC_MISC);
		sav->key_enc = NULL;
	}
	if (sav->replay != NULL) {
		free(sav->replay, M_IPSEC_MISC);
		sav->replay = NULL;
	}
	if (sav->lft_h != NULL) {
		free(sav->lft_h, M_IPSEC_MISC);
		sav->lft_h = NULL;
	}
	if (sav->lft_s != NULL) {
		free(sav->lft_s, M_IPSEC_MISC);
		sav->lft_s = NULL;
	}
}

/*
 * free() SA variable entry.
 */
static void
key_delsav(struct secasvar *sav)
{
	IPSEC_ASSERT(sav != NULL, ("null sav"));
	IPSEC_ASSERT(sav->state == SADB_SASTATE_DEAD,
	    ("attempt to free non DEAD SA %p", sav));
	IPSEC_ASSERT(sav->refcnt == 0, ("reference count %u > 0",
	    sav->refcnt));

	/* SA must be unlinked from the chain and hashtbl */
	key_cleansav(sav);
	SECASVAR_LOCK_DESTROY(sav);
	uma_zfree(V_key_lft_zone, sav->lft_c);
	free(sav, M_IPSEC_SA);
}

/*
 * search SAH.
 * OUT:
 *	NULL	: not found
 *	others	: found, referenced pointer to a SAH.
 */
static struct secashead *
key_getsah(struct secasindex *saidx)
{
	SAHTREE_RLOCK_TRACKER;
	struct secashead *sah;

	SAHTREE_RLOCK();
	LIST_FOREACH(sah, SAHADDRHASH_HASH(saidx), addrhash) {
	    if (key_cmpsaidx(&sah->saidx, saidx, CMP_MODE_REQID) != 0) {
		    SAH_ADDREF(sah);
		    break;
	    }
	}
	SAHTREE_RUNLOCK();
	return (sah);
}

/*
 * Check not to be duplicated SPI.
 * OUT:
 *	0	: not found
 *	1	: found SA with given SPI.
 */
static int
key_checkspidup(uint32_t spi)
{
	SAHTREE_RLOCK_TRACKER;
	struct secasvar *sav;

	/* Assume SPI is in network byte order */
	SAHTREE_RLOCK();
	LIST_FOREACH(sav, SAVHASH_HASH(spi), spihash) {
		if (sav->spi == spi)
			break;
	}
	SAHTREE_RUNLOCK();
	return (sav != NULL);
}

/*
 * Search SA by SPI.
 * OUT:
 *	NULL	: not found
 *	others	: found, referenced pointer to a SA.
 */
static struct secasvar *
key_getsavbyspi(uint32_t spi)
{
	SAHTREE_RLOCK_TRACKER;
	struct secasvar *sav;

	/* Assume SPI is in network byte order */
	SAHTREE_RLOCK();
	LIST_FOREACH(sav, SAVHASH_HASH(spi), spihash) {
		if (sav->spi != spi)
			continue;
		SAV_ADDREF(sav);
		break;
	}
	SAHTREE_RUNLOCK();
	return (sav);
}

static int
key_updatelifetimes(struct secasvar *sav, const struct sadb_msghdr *mhp)
{
	struct seclifetime *lft_h, *lft_s, *tmp;

	/* Lifetime extension is optional, check that it is present. */
	if (SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_HARD) &&
	    SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_SOFT)) {
		/*
		 * In case of SADB_UPDATE we may need to change
		 * existing lifetimes.
		 */
		if (sav->state == SADB_SASTATE_MATURE) {
			lft_h = lft_s = NULL;
			goto reset;
		}
		return (0);
	}
	/* XXXAE: what should we do with CURRENT lifetime? */
	/* Both HARD and SOFT extensions must present */
	if ((SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_HARD) &&
	    !SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_SOFT)) ||
	    (SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_SOFT) &&
	    !SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_HARD))) {
		ipseclog((LOG_DEBUG,
		    "%s: invalid message: missing required header.\n",
		    __func__));
		return (EINVAL);
	}
	if (SADB_CHECKLEN(mhp, SADB_EXT_LIFETIME_HARD) ||
	    SADB_CHECKLEN(mhp, SADB_EXT_LIFETIME_SOFT)) {
		ipseclog((LOG_DEBUG,
		    "%s: invalid message: wrong header size.\n", __func__));
		return (EINVAL);
	}
	lft_h = key_dup_lifemsg((const struct sadb_lifetime *)
	    mhp->ext[SADB_EXT_LIFETIME_HARD], M_IPSEC_MISC);
	if (lft_h == NULL) {
		PFKEYSTAT_INC(in_nomem);
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		return (ENOBUFS);
	}
	lft_s = key_dup_lifemsg((const struct sadb_lifetime *)
	    mhp->ext[SADB_EXT_LIFETIME_SOFT], M_IPSEC_MISC);
	if (lft_s == NULL) {
		PFKEYSTAT_INC(in_nomem);
		free(lft_h, M_IPSEC_MISC);
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		return (ENOBUFS);
	}
reset:
	if (sav->state != SADB_SASTATE_LARVAL) {
		/*
		 * key_update() holds reference to this SA,
		 * so it won't be deleted in meanwhile.
		 */
		SECASVAR_LOCK(sav);
		tmp = sav->lft_h;
		sav->lft_h = lft_h;
		lft_h = tmp;

		tmp = sav->lft_s;
		sav->lft_s = lft_s;
		lft_s = tmp;
		SECASVAR_UNLOCK(sav);
		if (lft_h != NULL)
			free(lft_h, M_IPSEC_MISC);
		if (lft_s != NULL)
			free(lft_s, M_IPSEC_MISC);
		return (0);
	}
	/* We can update lifetime without holding a lock */
	IPSEC_ASSERT(sav->lft_h == NULL, ("lft_h is already initialized\n"));
	IPSEC_ASSERT(sav->lft_s == NULL, ("lft_s is already initialized\n"));
	sav->lft_h = lft_h;
	sav->lft_s = lft_s;
	return (0);
}

/*
 * copy SA values from PF_KEY message except *SPI, SEQ, PID and TYPE*.
 * You must update these if need. Expects only LARVAL SAs.
 * OUT:	0:	success.
 *	!0:	failure.
 */
static int
key_setsaval(struct secasvar *sav, const struct sadb_msghdr *mhp)
{
	const struct sadb_sa *sa0;
	const struct sadb_key *key0;
	size_t len;
	int error;

	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));
	IPSEC_ASSERT(sav->state == SADB_SASTATE_LARVAL,
	    ("Attempt to update non LARVAL SA"));

	/* XXX rewrite */
	error = key_setident(sav->sah, mhp);
	if (error != 0)
		goto fail;

	error = key_setnatt(sav, mhp);
	if (error != 0)
		goto fail;

	/* SA */
	if (!SADB_CHECKHDR(mhp, SADB_EXT_SA)) {
		if (SADB_CHECKLEN(mhp, SADB_EXT_SA)) {
			error = EINVAL;
			goto fail;
		}
		sa0 = (const struct sadb_sa *)mhp->ext[SADB_EXT_SA];
		sav->alg_auth = sa0->sadb_sa_auth;
		sav->alg_enc = sa0->sadb_sa_encrypt;
		sav->flags = sa0->sadb_sa_flags;

		/* replay window */
		if ((sa0->sadb_sa_flags & SADB_X_EXT_OLD) == 0) {
			sav->replay = malloc(sizeof(struct secreplay) +
			    sa0->sadb_sa_replay, M_IPSEC_MISC,
			    M_NOWAIT | M_ZERO);
			if (sav->replay == NULL) {
				PFKEYSTAT_INC(in_nomem);
				ipseclog((LOG_DEBUG, "%s: No more memory.\n",
					__func__));
				error = ENOBUFS;
				goto fail;
			}
			if (sa0->sadb_sa_replay != 0)
				sav->replay->bitmap = (caddr_t)(sav->replay+1);
			sav->replay->wsize = sa0->sadb_sa_replay;
		}
	}

	/* Authentication keys */
	if (!SADB_CHECKHDR(mhp, SADB_EXT_KEY_AUTH)) {
		if (SADB_CHECKLEN(mhp, SADB_EXT_KEY_AUTH)) {
			error = EINVAL;
			goto fail;
		}
		error = 0;
		key0 = (const struct sadb_key *)mhp->ext[SADB_EXT_KEY_AUTH];
		len = mhp->extlen[SADB_EXT_KEY_AUTH];
		switch (mhp->msg->sadb_msg_satype) {
		case SADB_SATYPE_AH:
		case SADB_SATYPE_ESP:
		case SADB_X_SATYPE_TCPSIGNATURE:
			if (len == PFKEY_ALIGN8(sizeof(struct sadb_key)) &&
			    sav->alg_auth != SADB_X_AALG_NULL)
				error = EINVAL;
			break;
		case SADB_X_SATYPE_IPCOMP:
		default:
			error = EINVAL;
			break;
		}
		if (error) {
			ipseclog((LOG_DEBUG, "%s: invalid key_auth values.\n",
				__func__));
			goto fail;
		}

		sav->key_auth = key_dup_keymsg(key0, len, M_IPSEC_MISC);
		if (sav->key_auth == NULL ) {
			ipseclog((LOG_DEBUG, "%s: No more memory.\n",
				  __func__));
			PFKEYSTAT_INC(in_nomem);
			error = ENOBUFS;
			goto fail;
		}
	}

	/* Encryption key */
	if (!SADB_CHECKHDR(mhp, SADB_EXT_KEY_ENCRYPT)) {
		if (SADB_CHECKLEN(mhp, SADB_EXT_KEY_ENCRYPT)) {
			error = EINVAL;
			goto fail;
		}
		error = 0;
		key0 = (const struct sadb_key *)mhp->ext[SADB_EXT_KEY_ENCRYPT];
		len = mhp->extlen[SADB_EXT_KEY_ENCRYPT];
		switch (mhp->msg->sadb_msg_satype) {
		case SADB_SATYPE_ESP:
			if (len == PFKEY_ALIGN8(sizeof(struct sadb_key)) &&
			    sav->alg_enc != SADB_EALG_NULL) {
				error = EINVAL;
				break;
			}
			sav->key_enc = key_dup_keymsg(key0, len, M_IPSEC_MISC);
			if (sav->key_enc == NULL) {
				ipseclog((LOG_DEBUG, "%s: No more memory.\n",
					__func__));
				PFKEYSTAT_INC(in_nomem);
				error = ENOBUFS;
				goto fail;
			}
			break;
		case SADB_X_SATYPE_IPCOMP:
			if (len != PFKEY_ALIGN8(sizeof(struct sadb_key)))
				error = EINVAL;
			sav->key_enc = NULL;	/*just in case*/
			break;
		case SADB_SATYPE_AH:
		case SADB_X_SATYPE_TCPSIGNATURE:
		default:
			error = EINVAL;
			break;
		}
		if (error) {
			ipseclog((LOG_DEBUG, "%s: invalid key_enc value.\n",
				__func__));
			goto fail;
		}
	}

	/* set iv */
	sav->ivlen = 0;
	switch (mhp->msg->sadb_msg_satype) {
	case SADB_SATYPE_AH:
		if (sav->flags & SADB_X_EXT_DERIV) {
			ipseclog((LOG_DEBUG, "%s: invalid flag (derived) "
			    "given to AH SA.\n", __func__));
			error = EINVAL;
			goto fail;
		}
		if (sav->alg_enc != SADB_EALG_NONE) {
			ipseclog((LOG_DEBUG, "%s: protocol and algorithm "
			    "mismated.\n", __func__));
			error = EINVAL;
			goto fail;
		}
		error = xform_init(sav, XF_AH);
		break;
	case SADB_SATYPE_ESP:
		if ((sav->flags & (SADB_X_EXT_OLD | SADB_X_EXT_DERIV)) ==
		    (SADB_X_EXT_OLD | SADB_X_EXT_DERIV)) {
			ipseclog((LOG_DEBUG, "%s: invalid flag (derived) "
			    "given to old-esp.\n", __func__));
			error = EINVAL;
			goto fail;
		}
		error = xform_init(sav, XF_ESP);
		break;
	case SADB_X_SATYPE_IPCOMP:
		if (sav->alg_auth != SADB_AALG_NONE) {
			ipseclog((LOG_DEBUG, "%s: protocol and algorithm "
			    "mismated.\n", __func__));
			error = EINVAL;
			goto fail;
		}
		if ((sav->flags & SADB_X_EXT_RAWCPI) == 0 &&
		    ntohl(sav->spi) >= 0x10000) {
			ipseclog((LOG_DEBUG, "%s: invalid cpi for IPComp.\n",
			    __func__));
			error = EINVAL;
			goto fail;
		}
		error = xform_init(sav, XF_IPCOMP);
		break;
	case SADB_X_SATYPE_TCPSIGNATURE:
		if (sav->alg_enc != SADB_EALG_NONE) {
			ipseclog((LOG_DEBUG, "%s: protocol and algorithm "
			    "mismated.\n", __func__));
			error = EINVAL;
			goto fail;
		}
		error = xform_init(sav, XF_TCPSIGNATURE);
		break;
	default:
		ipseclog((LOG_DEBUG, "%s: Invalid satype.\n", __func__));
		error = EPROTONOSUPPORT;
		goto fail;
	}
	if (error) {
		ipseclog((LOG_DEBUG, "%s: unable to initialize SA type %u.\n",
		    __func__, mhp->msg->sadb_msg_satype));
		goto fail;
	}

	/* Initialize lifetime for CURRENT */
	sav->firstused = 0;
	sav->created = time_second;

	/* lifetimes for HARD and SOFT */
	error = key_updatelifetimes(sav, mhp);
	if (error == 0)
		return (0);
fail:
	key_cleansav(sav);
	return (error);
}

/*
 * subroutine for SADB_GET and SADB_DUMP.
 */
static struct mbuf *
key_setdumpsa(struct secasvar *sav, uint8_t type, uint8_t satype,
    uint32_t seq, uint32_t pid)
{
	struct seclifetime lft_c;
	struct mbuf *result = NULL, *tres = NULL, *m;
	int i, dumporder[] = {
		SADB_EXT_SA, SADB_X_EXT_SA2,
		SADB_EXT_LIFETIME_HARD, SADB_EXT_LIFETIME_SOFT,
		SADB_EXT_LIFETIME_CURRENT, SADB_EXT_ADDRESS_SRC,
		SADB_EXT_ADDRESS_DST, SADB_EXT_ADDRESS_PROXY,
		SADB_EXT_KEY_AUTH, SADB_EXT_KEY_ENCRYPT,
		SADB_EXT_IDENTITY_SRC, SADB_EXT_IDENTITY_DST,
		SADB_EXT_SENSITIVITY,
#ifdef IPSEC_NAT_T
		SADB_X_EXT_NAT_T_TYPE,
		SADB_X_EXT_NAT_T_SPORT, SADB_X_EXT_NAT_T_DPORT,
		SADB_X_EXT_NAT_T_OAI, SADB_X_EXT_NAT_T_OAR,
		SADB_X_EXT_NAT_T_FRAG,
#endif
	};

	m = key_setsadbmsg(type, 0, satype, seq, pid, sav->refcnt);
	if (m == NULL)
		goto fail;
	result = m;

	for (i = nitems(dumporder) - 1; i >= 0; i--) {
		m = NULL;
		switch (dumporder[i]) {
		case SADB_EXT_SA:
			m = key_setsadbsa(sav);
			if (!m)
				goto fail;
			break;

		case SADB_X_EXT_SA2:
			m = key_setsadbxsa2(sav->sah->saidx.mode,
					sav->replay ? sav->replay->count : 0,
					sav->sah->saidx.reqid);
			if (!m)
				goto fail;
			break;

		case SADB_EXT_ADDRESS_SRC:
			m = key_setsadbaddr(SADB_EXT_ADDRESS_SRC,
			    &sav->sah->saidx.src.sa,
			    FULLMASK, IPSEC_ULPROTO_ANY);
			if (!m)
				goto fail;
			break;

		case SADB_EXT_ADDRESS_DST:
			m = key_setsadbaddr(SADB_EXT_ADDRESS_DST,
			    &sav->sah->saidx.dst.sa,
			    FULLMASK, IPSEC_ULPROTO_ANY);
			if (!m)
				goto fail;
			break;

		case SADB_EXT_KEY_AUTH:
			if (!sav->key_auth)
				continue;
			m = key_setkey(sav->key_auth, SADB_EXT_KEY_AUTH);
			if (!m)
				goto fail;
			break;

		case SADB_EXT_KEY_ENCRYPT:
			if (!sav->key_enc)
				continue;
			m = key_setkey(sav->key_enc, SADB_EXT_KEY_ENCRYPT);
			if (!m)
				goto fail;
			break;

		case SADB_EXT_LIFETIME_CURRENT:
			lft_c.addtime = sav->created;
			lft_c.allocations = (uint32_t)counter_u64_fetch(
			    sav->lft_c_allocations);
			lft_c.bytes = counter_u64_fetch(sav->lft_c_bytes);
			lft_c.usetime = sav->firstused;
			m = key_setlifetime(&lft_c, SADB_EXT_LIFETIME_CURRENT);
			if (!m)
				goto fail;
			break;

		case SADB_EXT_LIFETIME_HARD:
			if (!sav->lft_h)
				continue;
			m = key_setlifetime(sav->lft_h, 
					    SADB_EXT_LIFETIME_HARD);
			if (!m)
				goto fail;
			break;

		case SADB_EXT_LIFETIME_SOFT:
			if (!sav->lft_s)
				continue;
			m = key_setlifetime(sav->lft_s, 
					    SADB_EXT_LIFETIME_SOFT);

			if (!m)
				goto fail;
			break;

#ifdef IPSEC_NAT_T
		case SADB_X_EXT_NAT_T_TYPE:
			m = key_setsadbxtype(sav->natt_type);
			if (!m)
				goto fail;
			break;

		case SADB_X_EXT_NAT_T_DPORT:
			m = key_setsadbxport(
			    key_portfromsaddr(&sav->sah->saidx.dst.sa),
			    SADB_X_EXT_NAT_T_DPORT);
			if (!m)
				goto fail;
			break;

		case SADB_X_EXT_NAT_T_SPORT:
			m = key_setsadbxport(
			    key_portfromsaddr(&sav->sah->saidx.src.sa),
			    SADB_X_EXT_NAT_T_SPORT);
			if (!m)
				goto fail;
			break;

		case SADB_X_EXT_NAT_T_OAI:
		case SADB_X_EXT_NAT_T_OAR:
		case SADB_X_EXT_NAT_T_FRAG:
			/* We do not (yet) support those. */
			continue;
#endif

		case SADB_EXT_ADDRESS_PROXY:
		case SADB_EXT_IDENTITY_SRC:
		case SADB_EXT_IDENTITY_DST:
			/* XXX: should we brought from SPD ? */
		case SADB_EXT_SENSITIVITY:
		default:
			continue;
		}

		if (!m)
			goto fail;
		if (tres)
			m_cat(m, tres);
		tres = m;
	}

	m_cat(result, tres);
	tres = NULL;
	if (result->m_len < sizeof(struct sadb_msg)) {
		result = m_pullup(result, sizeof(struct sadb_msg));
		if (result == NULL)
			goto fail;
	}

	result->m_pkthdr.len = 0;
	for (m = result; m; m = m->m_next)
		result->m_pkthdr.len += m->m_len;

	mtod(result, struct sadb_msg *)->sadb_msg_len =
	    PFKEY_UNIT64(result->m_pkthdr.len);

	return result;

fail:
	m_freem(result);
	m_freem(tres);
	return NULL;
}

/*
 * set data into sadb_msg.
 */
static struct mbuf *
key_setsadbmsg(u_int8_t type, u_int16_t tlen, u_int8_t satype, u_int32_t seq,
    pid_t pid, u_int16_t reserved)
{
	struct mbuf *m;
	struct sadb_msg *p;
	int len;

	len = PFKEY_ALIGN8(sizeof(struct sadb_msg));
	if (len > MCLBYTES)
		return NULL;
	MGETHDR(m, M_NOWAIT, MT_DATA);
	if (m && len > MHLEN) {
		if (!(MCLGET(m, M_NOWAIT))) {
			m_freem(m);
			m = NULL;
		}
	}
	if (!m)
		return NULL;
	m->m_pkthdr.len = m->m_len = len;
	m->m_next = NULL;

	p = mtod(m, struct sadb_msg *);

	bzero(p, len);
	p->sadb_msg_version = PF_KEY_V2;
	p->sadb_msg_type = type;
	p->sadb_msg_errno = 0;
	p->sadb_msg_satype = satype;
	p->sadb_msg_len = PFKEY_UNIT64(tlen);
	p->sadb_msg_reserved = reserved;
	p->sadb_msg_seq = seq;
	p->sadb_msg_pid = (u_int32_t)pid;

	return m;
}

/*
 * copy secasvar data into sadb_address.
 */
static struct mbuf *
key_setsadbsa(struct secasvar *sav)
{
	struct mbuf *m;
	struct sadb_sa *p;
	int len;

	len = PFKEY_ALIGN8(sizeof(struct sadb_sa));
	m = m_get2(len, M_NOWAIT, MT_DATA, 0);
	if (m == NULL)
		return (NULL);
	m_align(m, len);
	m->m_len = len;
	p = mtod(m, struct sadb_sa *);
	bzero(p, len);
	p->sadb_sa_len = PFKEY_UNIT64(len);
	p->sadb_sa_exttype = SADB_EXT_SA;
	p->sadb_sa_spi = sav->spi;
	p->sadb_sa_replay = (sav->replay != NULL ? sav->replay->wsize : 0);
	p->sadb_sa_state = sav->state;
	p->sadb_sa_auth = sav->alg_auth;
	p->sadb_sa_encrypt = sav->alg_enc;
	p->sadb_sa_flags = sav->flags;

	return m;
}

/*
 * set data into sadb_address.
 */
static struct mbuf *
key_setsadbaddr(u_int16_t exttype, const struct sockaddr *saddr,
    u_int8_t prefixlen, u_int16_t ul_proto)
{
	struct mbuf *m;
	struct sadb_address *p;
	size_t len;

	len = PFKEY_ALIGN8(sizeof(struct sadb_address)) +
	    PFKEY_ALIGN8(saddr->sa_len);
	m = m_get2(len, M_NOWAIT, MT_DATA, 0);
	if (m == NULL)
		return (NULL);
	m_align(m, len);
	m->m_len = len;
	p = mtod(m, struct sadb_address *);

	bzero(p, len);
	p->sadb_address_len = PFKEY_UNIT64(len);
	p->sadb_address_exttype = exttype;
	p->sadb_address_proto = ul_proto;
	if (prefixlen == FULLMASK) {
		switch (saddr->sa_family) {
		case AF_INET:
			prefixlen = sizeof(struct in_addr) << 3;
			break;
		case AF_INET6:
			prefixlen = sizeof(struct in6_addr) << 3;
			break;
		default:
			; /*XXX*/
		}
	}
	p->sadb_address_prefixlen = prefixlen;
	p->sadb_address_reserved = 0;

	bcopy(saddr,
	    mtod(m, caddr_t) + PFKEY_ALIGN8(sizeof(struct sadb_address)),
	    saddr->sa_len);

	return m;
}

/*
 * set data into sadb_x_sa2.
 */
static struct mbuf *
key_setsadbxsa2(u_int8_t mode, u_int32_t seq, u_int32_t reqid)
{
	struct mbuf *m;
	struct sadb_x_sa2 *p;
	size_t len;

	len = PFKEY_ALIGN8(sizeof(struct sadb_x_sa2));
	m = m_get2(len, M_NOWAIT, MT_DATA, 0);
	if (m == NULL)
		return (NULL);
	m_align(m, len);
	m->m_len = len;
	p = mtod(m, struct sadb_x_sa2 *);

	bzero(p, len);
	p->sadb_x_sa2_len = PFKEY_UNIT64(len);
	p->sadb_x_sa2_exttype = SADB_X_EXT_SA2;
	p->sadb_x_sa2_mode = mode;
	p->sadb_x_sa2_reserved1 = 0;
	p->sadb_x_sa2_reserved2 = 0;
	p->sadb_x_sa2_sequence = seq;
	p->sadb_x_sa2_reqid = reqid;

	return m;
}

#ifdef IPSEC_NAT_T
/*
 * Set a type in sadb_x_nat_t_type.
 */
static struct mbuf *
key_setsadbxtype(u_int16_t type)
{
	struct mbuf *m;
	size_t len;
	struct sadb_x_nat_t_type *p;

	len = PFKEY_ALIGN8(sizeof(struct sadb_x_nat_t_type));

	m = m_get2(len, M_NOWAIT, MT_DATA, 0);
	if (m == NULL)
		return (NULL);
	m_align(m, len);
	m->m_len = len;
	p = mtod(m, struct sadb_x_nat_t_type *);

	bzero(p, len);
	p->sadb_x_nat_t_type_len = PFKEY_UNIT64(len);
	p->sadb_x_nat_t_type_exttype = SADB_X_EXT_NAT_T_TYPE;
	p->sadb_x_nat_t_type_type = type;

	return (m);
}
/*
 * Set a port in sadb_x_nat_t_port.
 * In contrast to default RFC 2367 behaviour, port is in network byte order.
 */
static struct mbuf *
key_setsadbxport(u_int16_t port, u_int16_t type)
{
	struct mbuf *m;
	size_t len;
	struct sadb_x_nat_t_port *p;

	len = PFKEY_ALIGN8(sizeof(struct sadb_x_nat_t_port));

	m = m_get2(len, M_NOWAIT, MT_DATA, 0);
	if (m == NULL)
		return (NULL);
	m_align(m, len);
	m->m_len = len;
	p = mtod(m, struct sadb_x_nat_t_port *);

	bzero(p, len);
	p->sadb_x_nat_t_port_len = PFKEY_UNIT64(len);
	p->sadb_x_nat_t_port_exttype = type;
	p->sadb_x_nat_t_port_port = port;

	return (m);
}

/*
 * Get port from sockaddr. Port is in network byte order.
 */
uint16_t
key_portfromsaddr(struct sockaddr *sa)
{

	switch (sa->sa_family) {
#ifdef INET
	case AF_INET:
		return ((struct sockaddr_in *)sa)->sin_port;
#endif
#ifdef INET6
	case AF_INET6:
		return ((struct sockaddr_in6 *)sa)->sin6_port;
#endif
	}
	return (0);
}
#endif /* IPSEC_NAT_T */

/*
 * Set port in struct sockaddr. Port is in network byte order.
 */
static void
key_porttosaddr(struct sockaddr *sa, uint16_t port)
{

	switch (sa->sa_family) {
#ifdef INET
	case AF_INET:
		((struct sockaddr_in *)sa)->sin_port = port;
		break;
#endif
#ifdef INET6
	case AF_INET6:
		((struct sockaddr_in6 *)sa)->sin6_port = port;
		break;
#endif
	default:
		ipseclog((LOG_DEBUG, "%s: unexpected address family %d.\n",
			__func__, sa->sa_family));
		break;
	}
}

/*
 * set data into sadb_x_policy
 */
static struct mbuf *
key_setsadbxpolicy(u_int16_t type, u_int8_t dir, u_int32_t id, u_int32_t priority)
{
	struct mbuf *m;
	struct sadb_x_policy *p;
	size_t len;

	len = PFKEY_ALIGN8(sizeof(struct sadb_x_policy));
	m = m_get2(len, M_NOWAIT, MT_DATA, 0);
	if (m == NULL)
		return (NULL);
	m_align(m, len);
	m->m_len = len;
	p = mtod(m, struct sadb_x_policy *);

	bzero(p, len);
	p->sadb_x_policy_len = PFKEY_UNIT64(len);
	p->sadb_x_policy_exttype = SADB_X_EXT_POLICY;
	p->sadb_x_policy_type = type;
	p->sadb_x_policy_dir = dir;
	p->sadb_x_policy_id = id;
	p->sadb_x_policy_priority = priority;

	return m;
}

/* %%% utilities */
/* Take a key message (sadb_key) from the socket and turn it into one
 * of the kernel's key structures (seckey).
 *
 * IN: pointer to the src
 * OUT: NULL no more memory
 */
struct seckey *
key_dup_keymsg(const struct sadb_key *src, u_int len,
    struct malloc_type *type)
{
	struct seckey *dst;
	dst = (struct seckey *)malloc(sizeof(struct seckey), type, M_NOWAIT);
	if (dst != NULL) {
		dst->bits = src->sadb_key_bits;
		dst->key_data = (char *)malloc(len, type, M_NOWAIT);
		if (dst->key_data != NULL) {
			bcopy((const char *)src + sizeof(struct sadb_key), 
			      dst->key_data, len);
		} else {
			ipseclog((LOG_DEBUG, "%s: No more memory.\n", 
				  __func__));
			free(dst, type);
			dst = NULL;
		}
	} else {
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", 
			  __func__));

	}
	return dst;
}

/* Take a lifetime message (sadb_lifetime) passed in on a socket and
 * turn it into one of the kernel's lifetime structures (seclifetime).
 *
 * IN: pointer to the destination, source and malloc type
 * OUT: NULL, no more memory
 */

static struct seclifetime *
key_dup_lifemsg(const struct sadb_lifetime *src, struct malloc_type *type)
{
	struct seclifetime *dst;

	dst = malloc(sizeof(*dst), type, M_NOWAIT);
	if (dst == NULL) {
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		return (NULL);
	}
	dst->allocations = src->sadb_lifetime_allocations;
	dst->bytes = src->sadb_lifetime_bytes;
	dst->addtime = src->sadb_lifetime_addtime;
	dst->usetime = src->sadb_lifetime_usetime;
	return (dst);
}

/* compare my own address
 * OUT:	1: true, i.e. my address.
 *	0: false
 */
int
key_ismyaddr(struct sockaddr *sa)
{

	IPSEC_ASSERT(sa != NULL, ("null sockaddr"));
	switch (sa->sa_family) {
#ifdef INET
	case AF_INET:
		return (in_localip(satosin(sa)->sin_addr));
#endif
#ifdef INET6
	case AF_INET6:
		return key_ismyaddr6((struct sockaddr_in6 *)sa);
#endif
	}

	return 0;
}

#ifdef INET6
/*
 * compare my own address for IPv6.
 * 1: ours
 * 0: other
 */
static int
key_ismyaddr6(struct sockaddr_in6 *sin6)
{
	struct in6_addr in6;

	if (!IN6_IS_SCOPE_LINKLOCAL(&sin6->sin6_addr))
		return (in6_localip(&sin6->sin6_addr));

	/* Convert address into kernel-internal form */
	in6 = sin6->sin6_addr;
	in6.s6_addr16[1] = htons(sin6->sin6_scope_id & 0xffff);
	return (in6_localip(&in6));
}
#endif /*INET6*/

/*
 * compare two secasindex structure.
 * flag can specify to compare 2 saidxes.
 * compare two secasindex structure without both mode and reqid.
 * don't compare port.
 * IN:  
 *      saidx0: source, it can be in SAD.
 *      saidx1: object.
 * OUT: 
 *      1 : equal
 *      0 : not equal
 */
static int
key_cmpsaidx(const struct secasindex *saidx0, const struct secasindex *saidx1,
    int flag)
{
	int chkport = 0;

	/* sanity */
	if (saidx0 == NULL && saidx1 == NULL)
		return 1;

	if (saidx0 == NULL || saidx1 == NULL)
		return 0;

	if (saidx0->proto != saidx1->proto)
		return 0;

	if (flag == CMP_EXACTLY) {
		if (saidx0->mode != saidx1->mode)
			return 0;
		if (saidx0->reqid != saidx1->reqid)
			return 0;
		if (bcmp(&saidx0->src, &saidx1->src, saidx0->src.sa.sa_len) != 0 ||
		    bcmp(&saidx0->dst, &saidx1->dst, saidx0->dst.sa.sa_len) != 0)
			return 0;
	} else {

		/* CMP_MODE_REQID, CMP_REQID, CMP_HEAD */
		if (flag == CMP_MODE_REQID
		  ||flag == CMP_REQID) {
			/*
			 * If reqid of SPD is non-zero, unique SA is required.
			 * The result must be of same reqid in this case.
			 */
			if (saidx1->reqid != 0 && saidx0->reqid != saidx1->reqid)
				return 0;
		}

		if (flag == CMP_MODE_REQID) {
			if (saidx0->mode != IPSEC_MODE_ANY
			 && saidx0->mode != saidx1->mode)
				return 0;
		}

#ifdef IPSEC_NAT_T
		/*
		 * If NAT-T is enabled, check ports for tunnel mode.
		 * Do not check ports if they are set to zero in the SPD.
		 * Also do not do it for native transport mode, as there
		 * is no port information available in the SP.
		 */
		if ((saidx1->mode == IPSEC_MODE_TUNNEL ||
		     (saidx1->mode == IPSEC_MODE_TRANSPORT &&
		      saidx1->proto == IPPROTO_ESP)) &&
		    saidx1->src.sa.sa_family == AF_INET &&
		    saidx1->dst.sa.sa_family == AF_INET &&
		    ((const struct sockaddr_in *)(&saidx1->src))->sin_port &&
		    ((const struct sockaddr_in *)(&saidx1->dst))->sin_port)
			chkport = 1;
#endif /* IPSEC_NAT_T */

		if (key_sockaddrcmp(&saidx0->src.sa, &saidx1->src.sa, chkport) != 0) {
			return 0;
		}
		if (key_sockaddrcmp(&saidx0->dst.sa, &saidx1->dst.sa, chkport) != 0) {
			return 0;
		}
	}

	return 1;
}

/*
 * compare two secindex structure exactly.
 * IN:
 *	spidx0: source, it is often in SPD.
 *	spidx1: object, it is often from PFKEY message.
 * OUT:
 *	1 : equal
 *	0 : not equal
 */
static int
key_cmpspidx_exactly(struct secpolicyindex *spidx0,
    struct secpolicyindex *spidx1)
{
	/* sanity */
	if (spidx0 == NULL && spidx1 == NULL)
		return 1;

	if (spidx0 == NULL || spidx1 == NULL)
		return 0;

	if (spidx0->prefs != spidx1->prefs
	 || spidx0->prefd != spidx1->prefd
	 || spidx0->ul_proto != spidx1->ul_proto)
		return 0;

	return key_sockaddrcmp(&spidx0->src.sa, &spidx1->src.sa, 1) == 0 &&
	       key_sockaddrcmp(&spidx0->dst.sa, &spidx1->dst.sa, 1) == 0;
}

/*
 * compare two secindex structure with mask.
 * IN:
 *	spidx0: source, it is often in SPD.
 *	spidx1: object, it is often from IP header.
 * OUT:
 *	1 : equal
 *	0 : not equal
 */
static int
key_cmpspidx_withmask(struct secpolicyindex *spidx0,
    struct secpolicyindex *spidx1)
{
	/* sanity */
	if (spidx0 == NULL && spidx1 == NULL)
		return 1;

	if (spidx0 == NULL || spidx1 == NULL)
		return 0;

	if (spidx0->src.sa.sa_family != spidx1->src.sa.sa_family ||
	    spidx0->dst.sa.sa_family != spidx1->dst.sa.sa_family ||
	    spidx0->src.sa.sa_len != spidx1->src.sa.sa_len ||
	    spidx0->dst.sa.sa_len != spidx1->dst.sa.sa_len)
		return 0;

	/* if spidx.ul_proto == IPSEC_ULPROTO_ANY, ignore. */
	if (spidx0->ul_proto != (u_int16_t)IPSEC_ULPROTO_ANY
	 && spidx0->ul_proto != spidx1->ul_proto)
		return 0;

	switch (spidx0->src.sa.sa_family) {
	case AF_INET:
		if (spidx0->src.sin.sin_port != IPSEC_PORT_ANY
		 && spidx0->src.sin.sin_port != spidx1->src.sin.sin_port)
			return 0;
		if (!key_bbcmp(&spidx0->src.sin.sin_addr,
		    &spidx1->src.sin.sin_addr, spidx0->prefs))
			return 0;
		break;
	case AF_INET6:
		if (spidx0->src.sin6.sin6_port != IPSEC_PORT_ANY
		 && spidx0->src.sin6.sin6_port != spidx1->src.sin6.sin6_port)
			return 0;
		/*
		 * scope_id check. if sin6_scope_id is 0, we regard it
		 * as a wildcard scope, which matches any scope zone ID. 
		 */
		if (spidx0->src.sin6.sin6_scope_id &&
		    spidx1->src.sin6.sin6_scope_id &&
		    spidx0->src.sin6.sin6_scope_id != spidx1->src.sin6.sin6_scope_id)
			return 0;
		if (!key_bbcmp(&spidx0->src.sin6.sin6_addr,
		    &spidx1->src.sin6.sin6_addr, spidx0->prefs))
			return 0;
		break;
	default:
		/* XXX */
		if (bcmp(&spidx0->src, &spidx1->src, spidx0->src.sa.sa_len) != 0)
			return 0;
		break;
	}

	switch (spidx0->dst.sa.sa_family) {
	case AF_INET:
		if (spidx0->dst.sin.sin_port != IPSEC_PORT_ANY
		 && spidx0->dst.sin.sin_port != spidx1->dst.sin.sin_port)
			return 0;
		if (!key_bbcmp(&spidx0->dst.sin.sin_addr,
		    &spidx1->dst.sin.sin_addr, spidx0->prefd))
			return 0;
		break;
	case AF_INET6:
		if (spidx0->dst.sin6.sin6_port != IPSEC_PORT_ANY
		 && spidx0->dst.sin6.sin6_port != spidx1->dst.sin6.sin6_port)
			return 0;
		/*
		 * scope_id check. if sin6_scope_id is 0, we regard it
		 * as a wildcard scope, which matches any scope zone ID. 
		 */
		if (spidx0->dst.sin6.sin6_scope_id &&
		    spidx1->dst.sin6.sin6_scope_id &&
		    spidx0->dst.sin6.sin6_scope_id != spidx1->dst.sin6.sin6_scope_id)
			return 0;
		if (!key_bbcmp(&spidx0->dst.sin6.sin6_addr,
		    &spidx1->dst.sin6.sin6_addr, spidx0->prefd))
			return 0;
		break;
	default:
		/* XXX */
		if (bcmp(&spidx0->dst, &spidx1->dst, spidx0->dst.sa.sa_len) != 0)
			return 0;
		break;
	}

	/* XXX Do we check other field ?  e.g. flowinfo */

	return 1;
}

#ifdef satosin
#undef satosin
#endif
#define satosin(s) ((const struct sockaddr_in *)s)
#ifdef satosin6
#undef satosin6
#endif
#define satosin6(s) ((const struct sockaddr_in6 *)s)
/* returns 0 on match */
static int
key_sockaddrcmp(const struct sockaddr *sa1, const struct sockaddr *sa2,
    int port)
{
	if (sa1->sa_family != sa2->sa_family || sa1->sa_len != sa2->sa_len)
		return 1;

	switch (sa1->sa_family) {
#ifdef INET
	case AF_INET:
		if (sa1->sa_len != sizeof(struct sockaddr_in))
			return 1;
		if (satosin(sa1)->sin_addr.s_addr !=
		    satosin(sa2)->sin_addr.s_addr) {
			return 1;
		}
		if (port && satosin(sa1)->sin_port != satosin(sa2)->sin_port)
			return 1;
		break;
#endif
#ifdef INET6
	case AF_INET6:
		if (sa1->sa_len != sizeof(struct sockaddr_in6))
			return 1;	/*EINVAL*/
		if (satosin6(sa1)->sin6_scope_id !=
		    satosin6(sa2)->sin6_scope_id) {
			return 1;
		}
		if (!IN6_ARE_ADDR_EQUAL(&satosin6(sa1)->sin6_addr,
		    &satosin6(sa2)->sin6_addr)) {
			return 1;
		}
		if (port &&
		    satosin6(sa1)->sin6_port != satosin6(sa2)->sin6_port) {
			return 1;
		}
		break;
#endif
	default:
		if (bcmp(sa1, sa2, sa1->sa_len) != 0)
			return 1;
		break;
	}

	return 0;
}

/* returns 0 on match */
int
key_sockaddrcmp_withmask(const struct sockaddr *sa1,
    const struct sockaddr *sa2, size_t mask)
{
	if (sa1->sa_family != sa2->sa_family || sa1->sa_len != sa2->sa_len)
		return (1);

	switch (sa1->sa_family) {
#ifdef INET
	case AF_INET:
		return (!key_bbcmp(&satosin(sa1)->sin_addr,
		    &satosin(sa2)->sin_addr, mask));
#endif
#ifdef INET6
	case AF_INET6:
		if (satosin6(sa1)->sin6_scope_id !=
		    satosin6(sa2)->sin6_scope_id)
			return (1);
		return (!key_bbcmp(&satosin6(sa1)->sin6_addr,
		    &satosin6(sa2)->sin6_addr, mask));
#endif
	}
	return (1);
}
#undef satosin
#undef satosin6

/*
 * compare two buffers with mask.
 * IN:
 *	addr1: source
 *	addr2: object
 *	bits:  Number of bits to compare
 * OUT:
 *	1 : equal
 *	0 : not equal
 */
static int
key_bbcmp(const void *a1, const void *a2, u_int bits)
{
	const unsigned char *p1 = a1;
	const unsigned char *p2 = a2;

	/* XXX: This could be considerably faster if we compare a word
	 * at a time, but it is complicated on LSB Endian machines */

	/* Handle null pointers */
	if (p1 == NULL || p2 == NULL)
		return (p1 == p2);

	while (bits >= 8) {
		if (*p1++ != *p2++)
			return 0;
		bits -= 8;
	}

	if (bits > 0) {
		u_int8_t mask = ~((1<<(8-bits))-1);
		if ((*p1 & mask) != (*p2 & mask))
			return 0;
	}
	return 1;	/* Match! */
}

static void
key_flush_spd(time_t now)
{
	SPTREE_RLOCK_TRACKER;
	struct secpolicy_list drainq;
	struct secpolicy *sp, *nextsp;
	u_int dir;

	LIST_INIT(&drainq);
	SPTREE_RLOCK();
	for (dir = 0; dir < IPSEC_DIR_MAX; dir++) {
		TAILQ_FOREACH(sp, &V_sptree[dir], chain) {
			if (sp->lifetime == 0 && sp->validtime == 0)
				continue;
			if ((sp->lifetime &&
			    now - sp->created > sp->lifetime) ||
			    (sp->validtime &&
			    now - sp->lastused > sp->validtime)) {
				/* Hold extra reference to send SPDEXPIRE */
				SP_ADDREF(sp);
				LIST_INSERT_HEAD(&drainq, sp, drainq);
			}
		}
	}
	SPTREE_RUNLOCK();
	if (LIST_EMPTY(&drainq))
		return;

	SPTREE_WLOCK();
	sp = LIST_FIRST(&drainq);
	while (sp != NULL) {
		nextsp = LIST_NEXT(sp, drainq);
		/* Check that SP is still linked */
		if (sp->state != IPSEC_SPSTATE_ALIVE) {
			LIST_REMOVE(sp, drainq);
			key_freesp(&sp); /* release extra reference */
			sp = nextsp;
			continue;
		}
		TAILQ_REMOVE(&V_sptree[sp->spidx.dir], sp, chain);
		LIST_REMOVE(sp, idhash);
		sp->state = IPSEC_SPSTATE_DEAD;
		sp = nextsp;
	}
	V_sp_genid++;
	SPTREE_WUNLOCK();

	sp = LIST_FIRST(&drainq);
	while (sp != NULL) {
		nextsp = LIST_NEXT(sp, drainq);
		key_spdexpire(sp);
		key_freesp(&sp); /* release extra reference */
		key_freesp(&sp); /* release last reference */
		sp = nextsp;
	}
}

static void
key_flush_sad(time_t now)
{
	SAHTREE_RLOCK_TRACKER;
	struct secashead_list emptyq;
	struct secasvar_list drainq, hexpireq, sexpireq, freeq;
	struct secashead *sah, *nextsah;
	struct secasvar *sav, *nextsav;

	LIST_INIT(&drainq);
	LIST_INIT(&hexpireq);
	LIST_INIT(&sexpireq);
	LIST_INIT(&emptyq);

	SAHTREE_RLOCK();
	TAILQ_FOREACH(sah, &V_sahtree, chain) {
		/* Check for empty SAH */
		if (TAILQ_EMPTY(&sah->savtree_larval) &&
		    TAILQ_EMPTY(&sah->savtree_alive)) {
			SAH_ADDREF(sah);
			LIST_INSERT_HEAD(&emptyq, sah, drainq);
			continue;
		}
		/* Add all stale LARVAL SAs into drainq */
		TAILQ_FOREACH(sav, &sah->savtree_larval, chain) {
			if (now - sav->created < V_key_larval_lifetime)
				continue;
			SAV_ADDREF(sav);
			LIST_INSERT_HEAD(&drainq, sav, drainq);
		}
		TAILQ_FOREACH(sav, &sah->savtree_alive, chain) {
			/* lifetimes aren't specified */
			if (sav->lft_h == NULL)
				continue;
			SECASVAR_LOCK(sav);
			/*
			 * Check again with lock held, because it may
			 * be updated by SADB_UPDATE.
			 */
			if (sav->lft_h == NULL) {
				SECASVAR_UNLOCK(sav);
				continue;
			}
			/*
			 * RFC 2367:
			 * HARD lifetimes MUST take precedence over SOFT
			 * lifetimes, meaning if the HARD and SOFT lifetimes
			 * are the same, the HARD lifetime will appear on the
			 * EXPIRE message.
			 */
			/* check HARD lifetime */
			if ((sav->lft_h->addtime != 0 &&
			    now - sav->created > sav->lft_h->addtime) ||
			    (sav->lft_h->usetime != 0 && sav->firstused &&
			    now - sav->firstused > sav->lft_h->usetime) ||
			    (sav->lft_h->bytes != 0 && counter_u64_fetch(
			        sav->lft_c_bytes) > sav->lft_h->bytes)) {
				SECASVAR_UNLOCK(sav);
				SAV_ADDREF(sav);
				LIST_INSERT_HEAD(&hexpireq, sav, drainq);
				continue;
			}
			/* check SOFT lifetime (only for MATURE SAs) */
			if (sav->state == SADB_SASTATE_MATURE && (
			    (sav->lft_s->addtime != 0 &&
			    now - sav->created > sav->lft_s->addtime) ||
			    (sav->lft_s->usetime != 0 && sav->firstused &&
			    now - sav->firstused > sav->lft_s->usetime) ||
			    (sav->lft_s->bytes != 0 && counter_u64_fetch(
				sav->lft_c_bytes) > sav->lft_s->bytes))) {
				SECASVAR_UNLOCK(sav);
				SAV_ADDREF(sav);
				LIST_INSERT_HEAD(&sexpireq, sav, drainq);
				continue;
			}
			SECASVAR_UNLOCK(sav);
		}
	}
	SAHTREE_RUNLOCK();

	if (LIST_EMPTY(&emptyq) && LIST_EMPTY(&drainq) &&
	    LIST_EMPTY(&hexpireq) && LIST_EMPTY(&sexpireq))
		return;

	LIST_INIT(&freeq);
	SAHTREE_WLOCK();
	/* Unlink stale LARVAL SAs */
	sav = LIST_FIRST(&drainq);
	while (sav != NULL) {
		nextsav = LIST_NEXT(sav, drainq);
		/* Check that SA is still LARVAL */
		if (sav->state != SADB_SASTATE_LARVAL) {
			LIST_REMOVE(sav, drainq);
			LIST_INSERT_HEAD(&freeq, sav, drainq);
			sav = nextsav;
			continue;
		}
		TAILQ_REMOVE(&sav->sah->savtree_larval, sav, chain);
		LIST_REMOVE(sav, spihash);
		sav->state = SADB_SASTATE_DEAD;
		sav = nextsav;
	}
	/* Unlink all SAs with expired HARD lifetime */
	sav = LIST_FIRST(&hexpireq);
	while (sav != NULL) {
		nextsav = LIST_NEXT(sav, drainq);
		/* Check that SA is not unlinked */
		if (sav->state == SADB_SASTATE_DEAD) {
			LIST_REMOVE(sav, drainq);
			LIST_INSERT_HEAD(&freeq, sav, drainq);
			sav = nextsav;
			continue;
		}
		TAILQ_REMOVE(&sav->sah->savtree_alive, sav, chain);
		LIST_REMOVE(sav, spihash);
		sav->state = SADB_SASTATE_DEAD;
		sav = nextsav;
	}
	/* Mark all SAs with expired SOFT lifetime as DYING */
	sav = LIST_FIRST(&sexpireq);
	while (sav != NULL) {
		nextsav = LIST_NEXT(sav, drainq);
		/* Check that SA is not unlinked */
		if (sav->state == SADB_SASTATE_DEAD) {
			LIST_REMOVE(sav, drainq);
			LIST_INSERT_HEAD(&freeq, sav, drainq);
			sav = nextsav;
			continue;
		}
		/*
		 * NOTE: this doesn't change SA order in the chain.
		 */
		sav->state = SADB_SASTATE_DYING;
		sav = nextsav;
	}
	/* Unlink empty SAHs */
	sah = LIST_FIRST(&emptyq);
	while (sah != NULL) {
		nextsah = LIST_NEXT(sah, drainq);
		/* Check that SAH is still empty and not unlinked */
		if (sah->state == SADB_SASTATE_DEAD ||
		    !TAILQ_EMPTY(&sah->savtree_larval) ||
		    !TAILQ_EMPTY(&sah->savtree_alive)) {
			LIST_REMOVE(sah, drainq);
			key_freesah(&sah); /* release extra reference */
			sah = nextsah;
			continue;
		}
		TAILQ_REMOVE(&V_sahtree, sah, chain);
		LIST_REMOVE(sah, addrhash);
		sah->state = SADB_SASTATE_DEAD;
		sah = nextsah;
	}
	SAHTREE_WUNLOCK();

	/* Send SPDEXPIRE messages */
	sav = LIST_FIRST(&hexpireq);
	while (sav != NULL) {
		nextsav = LIST_NEXT(sav, drainq);
		key_expire(sav, 1);
		key_freesah(&sav->sah); /* release reference from SAV */
		key_freesav(&sav); /* release extra reference */
		key_freesav(&sav); /* release last reference */
		sav = nextsav;
	}
	sav = LIST_FIRST(&sexpireq);
	while (sav != NULL) {
		nextsav = LIST_NEXT(sav, drainq);
		key_expire(sav, 0);
		key_freesav(&sav); /* release extra reference */
		sav = nextsav;
	}
	/* Free stale LARVAL SAs */
	sav = LIST_FIRST(&drainq);
	while (sav != NULL) {
		nextsav = LIST_NEXT(sav, drainq);
		key_freesah(&sav->sah); /* release reference from SAV */
		key_freesav(&sav); /* release extra reference */
		key_freesav(&sav); /* release last reference */
		sav = nextsav;
	}
	/* Free SAs that were unlinked/changed by someone else */
	sav = LIST_FIRST(&freeq);
	while (sav != NULL) {
		nextsav = LIST_NEXT(sav, drainq);
		key_freesav(&sav); /* release extra reference */
		sav = nextsav;
	}
	/* Free empty SAH */
	sah = LIST_FIRST(&emptyq);
	while (sah != NULL) {
		nextsah = LIST_NEXT(sah, drainq);
		key_freesah(&sah); /* release extra reference */
		key_freesah(&sah); /* release last reference */
		sah = nextsah;
	}
}

static void
key_flush_acq(time_t now)
{
	struct secacq *acq, *nextacq;

	/* ACQ tree */
	ACQ_LOCK();
	for (acq = LIST_FIRST(&V_acqtree); acq != NULL; acq = nextacq) {
		nextacq = LIST_NEXT(acq, chain);
		if (now - acq->created > V_key_blockacq_lifetime
		 && __LIST_CHAINED(acq)) {
			LIST_REMOVE(acq, chain);
			free(acq, M_IPSEC_SAQ);
		}
	}
	ACQ_UNLOCK();
}

static void
key_flush_spacq(time_t now)
{
	struct secspacq *acq, *nextacq;

	/* SP ACQ tree */
	SPACQ_LOCK();
	for (acq = LIST_FIRST(&V_spacqtree); acq != NULL; acq = nextacq) {
		nextacq = LIST_NEXT(acq, chain);
		if (now - acq->created > V_key_blockacq_lifetime
		 && __LIST_CHAINED(acq)) {
			LIST_REMOVE(acq, chain);
			free(acq, M_IPSEC_SAQ);
		}
	}
	SPACQ_UNLOCK();
}

/*
 * time handler.
 * scanning SPD and SAD to check status for each entries,
 * and do to remove or to expire.
 * XXX: year 2038 problem may remain.
 */
static void
key_timehandler(void *arg)
{
	VNET_ITERATOR_DECL(vnet_iter);
	time_t now = time_second;

	VNET_LIST_RLOCK_NOSLEEP();
	VNET_FOREACH(vnet_iter) {
		CURVNET_SET(vnet_iter);
		key_flush_spd(now);
		key_flush_sad(now);
		key_flush_acq(now);
		key_flush_spacq(now);
		CURVNET_RESTORE();
	}
	VNET_LIST_RUNLOCK_NOSLEEP();

#ifndef IPSEC_DEBUG2
	/* do exchange to tick time !! */
	callout_schedule(&key_timer, hz);
#endif /* IPSEC_DEBUG2 */
}

u_long
key_random()
{
	u_long value;

	key_randomfill(&value, sizeof(value));
	return value;
}

void
key_randomfill(void *p, size_t l)
{
	size_t n;
	u_long v;
	static int warn = 1;

	n = 0;
	n = (size_t)read_random(p, (u_int)l);
	/* last resort */
	while (n < l) {
		v = random();
		bcopy(&v, (u_int8_t *)p + n,
		    l - n < sizeof(v) ? l - n : sizeof(v));
		n += sizeof(v);

		if (warn) {
			printf("WARNING: pseudo-random number generator "
			    "used for IPsec processing\n");
			warn = 0;
		}
	}
}

/*
 * map SADB_SATYPE_* to IPPROTO_*.
 * if satype == SADB_SATYPE then satype is mapped to ~0.
 * OUT:
 *	0: invalid satype.
 */
static uint8_t
key_satype2proto(uint8_t satype)
{
	switch (satype) {
	case SADB_SATYPE_UNSPEC:
		return IPSEC_PROTO_ANY;
	case SADB_SATYPE_AH:
		return IPPROTO_AH;
	case SADB_SATYPE_ESP:
		return IPPROTO_ESP;
	case SADB_X_SATYPE_IPCOMP:
		return IPPROTO_IPCOMP;
	case SADB_X_SATYPE_TCPSIGNATURE:
		return IPPROTO_TCP;
	default:
		return 0;
	}
	/* NOTREACHED */
}

/*
 * map IPPROTO_* to SADB_SATYPE_*
 * OUT:
 *	0: invalid protocol type.
 */
static uint8_t
key_proto2satype(uint8_t proto)
{
	switch (proto) {
	case IPPROTO_AH:
		return SADB_SATYPE_AH;
	case IPPROTO_ESP:
		return SADB_SATYPE_ESP;
	case IPPROTO_IPCOMP:
		return SADB_X_SATYPE_IPCOMP;
	case IPPROTO_TCP:
		return SADB_X_SATYPE_TCPSIGNATURE;
	default:
		return 0;
	}
	/* NOTREACHED */
}

/* %%% PF_KEY */
/*
 * SADB_GETSPI processing is to receive
 *	<base, (SA2), src address, dst address, (SPI range)>
 * from the IKMPd, to assign a unique spi value, to hang on the INBOUND
 * tree with the status of LARVAL, and send
 *	<base, SA(*), address(SD)>
 * to the IKMPd.
 *
 * IN:	mhp: pointer to the pointer to each header.
 * OUT:	NULL if fail.
 *	other if success, return pointer to the message to send.
 */
static int
key_getspi(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	struct secasindex saidx;
	struct sadb_address *src0, *dst0;
	struct secasvar *sav;
	uint32_t reqid, spi;
	int error;
	uint8_t mode, proto;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	if (SADB_CHECKHDR(mhp, SADB_EXT_ADDRESS_SRC) ||
	    SADB_CHECKHDR(mhp, SADB_EXT_ADDRESS_DST)
#ifdef PFKEY_STRICT_CHECKS
	    || SADB_CHECKHDR(mhp, SADB_EXT_SPIRANGE)
#endif
	    ) {
		ipseclog((LOG_DEBUG,
		    "%s: invalid message: missing required header.\n",
		    __func__));
		error = EINVAL;
		goto fail;
	}
	if (SADB_CHECKLEN(mhp, SADB_EXT_ADDRESS_SRC) ||
	    SADB_CHECKLEN(mhp, SADB_EXT_ADDRESS_DST)
#ifdef PFKEY_STRICT_CHECKS
	    || SADB_CHECKLEN(mhp, SADB_EXT_SPIRANGE)
#endif
	    ) {
		ipseclog((LOG_DEBUG,
		    "%s: invalid message: wrong header size.\n", __func__));
		error = EINVAL;
		goto fail;
	}
	if (SADB_CHECKHDR(mhp, SADB_X_EXT_SA2)) {
		mode = IPSEC_MODE_ANY;
		reqid = 0;
	} else {
		if (SADB_CHECKLEN(mhp, SADB_X_EXT_SA2)) {
			ipseclog((LOG_DEBUG,
			    "%s: invalid message: wrong header size.\n",
			    __func__));
			error = EINVAL;
			goto fail;
		}
		mode = ((struct sadb_x_sa2 *)
		    mhp->ext[SADB_X_EXT_SA2])->sadb_x_sa2_mode;
		reqid = ((struct sadb_x_sa2 *)
		    mhp->ext[SADB_X_EXT_SA2])->sadb_x_sa2_reqid;
	}

	src0 = (struct sadb_address *)(mhp->ext[SADB_EXT_ADDRESS_SRC]);
	dst0 = (struct sadb_address *)(mhp->ext[SADB_EXT_ADDRESS_DST]);

	/* map satype to proto */
	if ((proto = key_satype2proto(mhp->msg->sadb_msg_satype)) == 0) {
		ipseclog((LOG_DEBUG, "%s: invalid satype is passed.\n",
			__func__));
		error = EINVAL;
		goto fail;
	}
	error = key_checksockaddrs((struct sockaddr *)(src0 + 1),
	    (struct sockaddr *)(dst0 + 1));
	if (error != 0) {
		ipseclog((LOG_DEBUG, "%s: invalid sockaddr.\n", __func__));
		error = EINVAL;
		goto fail;
	}
	KEY_SETSECASIDX(proto, mode, reqid, src0 + 1, dst0 + 1, &saidx);
	/*
	 * Make sure the port numbers are zero.
	 * In case of NAT-T we will update them later if needed.
	 */
	KEY_PORTTOSADDR(&saidx.src, 0);
	KEY_PORTTOSADDR(&saidx.dst, 0);

	/* SPI allocation */
	spi = key_do_getnewspi(
	    (struct sadb_spirange *)mhp->ext[SADB_EXT_SPIRANGE], &saidx);
	if (spi == 0) {
		/*
		 * Requested SPI or SPI range is not available or
		 * already used.
		 */
		error = EEXIST;
		goto fail;
	}
	sav = key_newsav(mhp, &saidx, spi, &error);
	if (sav == NULL)
		goto fail;

	if (sav->seq != 0) {
		/*
		 * RFC2367:
		 * If the SADB_GETSPI message is in response to a
		 * kernel-generated SADB_ACQUIRE, the sadb_msg_seq
		 * MUST be the same as the SADB_ACQUIRE message.
		 *
		 * XXXAE: However it doesn't definethe behaviour how to
		 * check this and what to do if it doesn't match.
		 * Also what we should do if it matches?
		 *
		 * We can compare saidx used in SADB_ACQUIRE with saidx
		 * used in SADB_GETSPI, but this probably can break
		 * existing software. For now just warn if it doesn't match.
		 *
		 * XXXAE: anyway it looks useless.
		 */
		key_acqdone(&saidx, sav->seq);
	}
	KEYDBG(KEY_STAMP,
	    printf("%s: SA(%p)\n", __func__, sav));
	KEYDBG(KEY_DATA, kdebug_secasv(sav));

    {
	struct mbuf *n, *nn;
	struct sadb_sa *m_sa;
	struct sadb_msg *newmsg;
	int off, len;

	/* create new sadb_msg to reply. */
	len = PFKEY_ALIGN8(sizeof(struct sadb_msg)) +
	    PFKEY_ALIGN8(sizeof(struct sadb_sa));

	MGETHDR(n, M_NOWAIT, MT_DATA);
	if (len > MHLEN) {
		if (!(MCLGET(n, M_NOWAIT))) {
			m_freem(n);
			n = NULL;
		}
	}
	if (!n) {
		error = ENOBUFS;
		goto fail;
	}

	n->m_len = len;
	n->m_next = NULL;
	off = 0;

	m_copydata(m, 0, sizeof(struct sadb_msg), mtod(n, caddr_t) + off);
	off += PFKEY_ALIGN8(sizeof(struct sadb_msg));

	m_sa = (struct sadb_sa *)(mtod(n, caddr_t) + off);
	m_sa->sadb_sa_len = PFKEY_UNIT64(sizeof(struct sadb_sa));
	m_sa->sadb_sa_exttype = SADB_EXT_SA;
	m_sa->sadb_sa_spi = spi; /* SPI is already in network byte order */
	off += PFKEY_ALIGN8(sizeof(struct sadb_sa));

	IPSEC_ASSERT(off == len,
		("length inconsistency (off %u len %u)", off, len));

	n->m_next = key_gather_mbuf(m, mhp, 0, 2, SADB_EXT_ADDRESS_SRC,
	    SADB_EXT_ADDRESS_DST);
	if (!n->m_next) {
		m_freem(n);
		error = ENOBUFS;
		goto fail;
	}

	if (n->m_len < sizeof(struct sadb_msg)) {
		n = m_pullup(n, sizeof(struct sadb_msg));
		if (n == NULL)
			return key_sendup_mbuf(so, m, KEY_SENDUP_ONE);
	}

	n->m_pkthdr.len = 0;
	for (nn = n; nn; nn = nn->m_next)
		n->m_pkthdr.len += nn->m_len;

	newmsg = mtod(n, struct sadb_msg *);
	newmsg->sadb_msg_seq = sav->seq;
	newmsg->sadb_msg_errno = 0;
	newmsg->sadb_msg_len = PFKEY_UNIT64(n->m_pkthdr.len);

	m_freem(m);
	return key_sendup_mbuf(so, n, KEY_SENDUP_ONE);
    }

fail:
	return (key_senderror(so, m, error));
}

/*
 * allocating new SPI
 * called by key_getspi().
 * OUT:
 *	0:	failure.
 *	others: success, SPI in network byte order.
 */
static uint32_t
key_do_getnewspi(struct sadb_spirange *spirange, struct secasindex *saidx)
{
	uint32_t min, max, newspi, t;
	int count = V_key_spi_trycnt;

	/* set spi range to allocate */
	if (spirange != NULL) {
		min = spirange->sadb_spirange_min;
		max = spirange->sadb_spirange_max;
	} else {
		min = V_key_spi_minval;
		max = V_key_spi_maxval;
	}
	/* IPCOMP needs 2-byte SPI */
	if (saidx->proto == IPPROTO_IPCOMP) {
		if (min >= 0x10000)
			min = 0xffff;
		if (max >= 0x10000)
			max = 0xffff;
		if (min > max) {
			t = min; min = max; max = t;
		}
	}

	if (min == max) {
		if (!key_checkspidup(htonl(min))) {
			ipseclog((LOG_DEBUG, "%s: SPI %u exists already.\n",
			    __func__, min));
			return 0;
		}

		count--; /* taking one cost. */
		newspi = min;
	} else {

		/* init SPI */
		newspi = 0;

		/* when requesting to allocate spi ranged */
		while (count--) {
			/* generate pseudo-random SPI value ranged. */
			newspi = min + (key_random() % (max - min + 1));
			if (!key_checkspidup(htonl(newspi)))
				break;
		}

		if (count == 0 || newspi == 0) {
			ipseclog((LOG_DEBUG,
			    "%s: failed to allocate SPI.\n", __func__));
			return 0;
		}
	}

	/* statistics */
	keystat.getspi_count =
	    (keystat.getspi_count + V_key_spi_trycnt - count) / 2;

	return (htonl(newspi));
}

/*
 * SADB_UPDATE processing
 * receive
 *   <base, SA, (SA2), (lifetime(HSC),) address(SD), (address(P),)
 *       key(AE), (identity(SD),) (sensitivity)>
 * from the ikmpd, and update a secasvar entry whose status is SADB_SASTATE_LARVAL.
 * and send
 *   <base, SA, (SA2), (lifetime(HSC),) address(SD), (address(P),)
 *       (identity(SD),) (sensitivity)>
 * to the ikmpd.
 *
 * m will always be freed.
 */
static int
key_update(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	struct secasindex saidx;
	struct sadb_address *src0, *dst0;
	struct sadb_sa *sa0;
	struct secasvar *sav;
	uint32_t reqid;
	int error;
	uint8_t mode, proto;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	/* map satype to proto */
	if ((proto = key_satype2proto(mhp->msg->sadb_msg_satype)) == 0) {
		ipseclog((LOG_DEBUG, "%s: invalid satype is passed.\n",
		    __func__));
		return key_senderror(so, m, EINVAL);
	}

	if (SADB_CHECKHDR(mhp, SADB_EXT_SA) ||
	    SADB_CHECKHDR(mhp, SADB_EXT_ADDRESS_SRC) ||
	    SADB_CHECKHDR(mhp, SADB_EXT_ADDRESS_DST) ||
	    (mhp->msg->sadb_msg_satype == SADB_SATYPE_ESP && (
		SADB_CHECKHDR(mhp, SADB_EXT_KEY_ENCRYPT) ||
		SADB_CHECKLEN(mhp, SADB_EXT_KEY_ENCRYPT))) ||
	    (mhp->msg->sadb_msg_satype == SADB_SATYPE_AH && (
		SADB_CHECKHDR(mhp, SADB_EXT_KEY_AUTH) ||
		SADB_CHECKLEN(mhp, SADB_EXT_KEY_AUTH))) ||
	    (SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_HARD) &&
		!SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_SOFT)) ||
	    (SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_SOFT) &&
		!SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_HARD))) {
		ipseclog((LOG_DEBUG,
		    "%s: invalid message: missing required header.\n",
		    __func__));
		return key_senderror(so, m, EINVAL);
	}
	if (SADB_CHECKLEN(mhp, SADB_EXT_SA) ||
	    SADB_CHECKLEN(mhp, SADB_EXT_ADDRESS_SRC) ||
	    SADB_CHECKLEN(mhp, SADB_EXT_ADDRESS_DST)) {
		ipseclog((LOG_DEBUG,
		    "%s: invalid message: wrong header size.\n", __func__));
		return key_senderror(so, m, EINVAL);
	}
	if (SADB_CHECKHDR(mhp, SADB_X_EXT_SA2)) {
		mode = IPSEC_MODE_ANY;
		reqid = 0;
	} else {
		if (SADB_CHECKLEN(mhp, SADB_X_EXT_SA2)) {
			ipseclog((LOG_DEBUG,
			    "%s: invalid message: wrong header size.\n",
			    __func__));
			return key_senderror(so, m, EINVAL);
		}
		mode = ((struct sadb_x_sa2 *)
		    mhp->ext[SADB_X_EXT_SA2])->sadb_x_sa2_mode;
		reqid = ((struct sadb_x_sa2 *)
		    mhp->ext[SADB_X_EXT_SA2])->sadb_x_sa2_reqid;
	}

	sa0 = (struct sadb_sa *)mhp->ext[SADB_EXT_SA];
	src0 = (struct sadb_address *)(mhp->ext[SADB_EXT_ADDRESS_SRC]);
	dst0 = (struct sadb_address *)(mhp->ext[SADB_EXT_ADDRESS_DST]);

	/*
	 * Only SADB_SASTATE_MATURE SAs may be submitted in an
	 * SADB_UPDATE message.
	 */
	if (sa0->sadb_sa_state != SADB_SASTATE_MATURE) {
		ipseclog((LOG_DEBUG, "%s: invalid state.\n", __func__));
#ifdef PFKEY_STRICT_CHECKS
		return key_senderror(so, m, EINVAL);
#endif
	}
	error = key_checksockaddrs((struct sockaddr *)(src0 + 1),
	    (struct sockaddr *)(dst0 + 1));
	if (error != 0) {
		ipseclog((LOG_DEBUG, "%s: invalid sockaddr.\n", __func__));
		return key_senderror(so, m, error);
	}
	KEY_SETSECASIDX(proto, mode, reqid, src0 + 1, dst0 + 1, &saidx);
	/*
	 * Make sure the port numbers are zero.
	 * In case of NAT-T we will update them later if needed.
	 */
	KEY_PORTTOSADDR(&saidx.src, 0);
	KEY_PORTTOSADDR(&saidx.dst, 0);

	sav = key_getsavbyspi(sa0->sadb_sa_spi);
	if (sav == NULL) {
		ipseclog((LOG_DEBUG, "%s: no SA found for SPI %u\n",
		    __func__, ntohl(sa0->sadb_sa_spi)));
		return key_senderror(so, m, EINVAL);
	}
	/*
	 * Check that SADB_UPDATE issued by the same process that did
	 * SADB_GETSPI or SADB_ADD.
	 */
	if (sav->pid != mhp->msg->sadb_msg_pid) {
		ipseclog((LOG_DEBUG,
		    "%s: pid mismatched (SPI %u, pid %u vs. %u)\n", __func__,
		    ntohl(sav->spi), sav->pid, mhp->msg->sadb_msg_pid));
		key_freesav(&sav);
		return key_senderror(so, m, EINVAL);
	}
	/*
	 * XXXAE: saidx should match with SA. Use CMP_MODE_REQID since we
	 * didn't set ports for NAT-T yet and exactly match may fail.
	 */
	if (key_cmpsaidx(&sav->sah->saidx, &saidx, CMP_MODE_REQID) == 0) {
		ipseclog((LOG_DEBUG, "%s: saidx mismatched for SPI %u",
		    __func__, ntohl(sav->spi)));
		key_freesav(&sav);
		return key_senderror(so, m, ESRCH);
	}

	if (sav->state == SADB_SASTATE_LARVAL) {
		/*
		 * We can set any values except src, dst and SPI.
		 */
		error = key_setsaval(sav, mhp);
		if (error != 0) {
			key_freesav(&sav);
			return (key_senderror(so, m, error));
		}
		/* Change SA state to MATURE */
		SAHTREE_WLOCK();
		if (sav->state != SADB_SASTATE_LARVAL) {
			/* SA was deleted or another thread made it MATURE. */
			SAHTREE_WUNLOCK();
			key_freesav(&sav);
			return (key_senderror(so, m, ESRCH));
		}
		/*
		 * NOTE: we keep SAs in savtree_alive ordered by created
		 * time. When SA's state changed from LARVAL to MATURE,
		 * we update its created time in key_setsaval() and move
		 * it into head of savtree_alive.
		 */
		TAILQ_REMOVE(&sav->sah->savtree_larval, sav, chain);
		TAILQ_INSERT_HEAD(&sav->sah->savtree_alive, sav, chain);
		sav->state = SADB_SASTATE_MATURE;
		SAHTREE_WUNLOCK();
	} else {
		/*
		 * For DYING and MATURE SA we can change only state
		 * and lifetimes. Report EINVAL if something else attempted
		 * to change.
		 */
		if (!SADB_CHECKHDR(mhp, SADB_EXT_KEY_ENCRYPT) ||
		    !SADB_CHECKHDR(mhp, SADB_EXT_KEY_AUTH)) {
			key_freesav(&sav);
			return (key_senderror(so, m, EINVAL));
		}
		error = key_updatelifetimes(sav, mhp);
		if (error != 0) {
			key_freesav(&sav);
			return (key_senderror(so, m, error));
		}
		/* Check that SA is still alive */
		SAHTREE_WLOCK();
		if (sav->state == SADB_SASTATE_DEAD) {
			/* SA was unlinked */
			SAHTREE_WUNLOCK();
			key_freesav(&sav);
			return (key_senderror(so, m, ESRCH));
		}
		/*
		 * NOTE: there is possible state moving from DYING to MATURE,
		 * but this doesn't change created time, so we won't reorder
		 * this SA.
		 */
		sav->state = SADB_SASTATE_MATURE;
		SAHTREE_WUNLOCK();
	}
	KEYDBG(KEY_STAMP,
	    printf("%s: SA(%p)\n", __func__, sav));
	KEYDBG(KEY_DATA, kdebug_secasv(sav));
	key_freesav(&sav);

    {
	struct mbuf *n;

	/* set msg buf from mhp */
	n = key_getmsgbuf_x1(m, mhp);
	if (n == NULL) {
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		return key_senderror(so, m, ENOBUFS);
	}

	m_freem(m);
	return key_sendup_mbuf(so, n, KEY_SENDUP_ALL);
    }
}

/*
 * SADB_ADD processing
 * add an entry to SA database, when received
 *   <base, SA, (SA2), (lifetime(HSC),) address(SD), (address(P),)
 *       key(AE), (identity(SD),) (sensitivity)>
 * from the ikmpd,
 * and send
 *   <base, SA, (SA2), (lifetime(HSC),) address(SD), (address(P),)
 *       (identity(SD),) (sensitivity)>
 * to the ikmpd.
 *
 * IGNORE identity and sensitivity messages.
 *
 * m will always be freed.
 */
static int
key_add(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	struct secasindex saidx;
	struct sadb_address *src0, *dst0;
	struct sadb_sa *sa0;
	struct secasvar *sav;
	uint32_t reqid;
	uint8_t mode, proto;
	int error;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	/* map satype to proto */
	if ((proto = key_satype2proto(mhp->msg->sadb_msg_satype)) == 0) {
		ipseclog((LOG_DEBUG, "%s: invalid satype is passed.\n",
		    __func__));
		return key_senderror(so, m, EINVAL);
	}

	if (SADB_CHECKHDR(mhp, SADB_EXT_SA) ||
	    SADB_CHECKHDR(mhp, SADB_EXT_ADDRESS_SRC) ||
	    SADB_CHECKHDR(mhp, SADB_EXT_ADDRESS_DST) ||
	    (mhp->msg->sadb_msg_satype == SADB_SATYPE_ESP && (
		SADB_CHECKHDR(mhp, SADB_EXT_KEY_ENCRYPT) ||
		SADB_CHECKLEN(mhp, SADB_EXT_KEY_ENCRYPT))) ||
	    (mhp->msg->sadb_msg_satype == SADB_SATYPE_AH && (
		SADB_CHECKHDR(mhp, SADB_EXT_KEY_AUTH) ||
		SADB_CHECKLEN(mhp, SADB_EXT_KEY_AUTH))) ||
	    (SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_HARD) &&
		!SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_SOFT)) ||
	    (SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_SOFT) &&
		!SADB_CHECKHDR(mhp, SADB_EXT_LIFETIME_HARD))) {
		ipseclog((LOG_DEBUG,
		    "%s: invalid message: missing required header.\n",
		    __func__));
		return key_senderror(so, m, EINVAL);
	}
	if (SADB_CHECKLEN(mhp, SADB_EXT_SA) ||
	    SADB_CHECKLEN(mhp, SADB_EXT_ADDRESS_SRC) ||
	    SADB_CHECKLEN(mhp, SADB_EXT_ADDRESS_DST)) {
		ipseclog((LOG_DEBUG,
		    "%s: invalid message: wrong header size.\n", __func__));
		return key_senderror(so, m, EINVAL);
	}
	if (SADB_CHECKHDR(mhp, SADB_X_EXT_SA2)) {
		mode = IPSEC_MODE_ANY;
		reqid = 0;
	} else {
		if (SADB_CHECKLEN(mhp, SADB_X_EXT_SA2)) {
			ipseclog((LOG_DEBUG,
			    "%s: invalid message: wrong header size.\n",
			    __func__));
			return key_senderror(so, m, EINVAL);
		}
		mode = ((struct sadb_x_sa2 *)
		    mhp->ext[SADB_X_EXT_SA2])->sadb_x_sa2_mode;
		reqid = ((struct sadb_x_sa2 *)
		    mhp->ext[SADB_X_EXT_SA2])->sadb_x_sa2_reqid;
	}

	sa0 = (struct sadb_sa *)mhp->ext[SADB_EXT_SA];
	src0 = (struct sadb_address *)mhp->ext[SADB_EXT_ADDRESS_SRC];
	dst0 = (struct sadb_address *)mhp->ext[SADB_EXT_ADDRESS_DST];

	/*
	 * Only SADB_SASTATE_MATURE SAs may be submitted in an
	 * SADB_ADD message.
	 */
	if (sa0->sadb_sa_state != SADB_SASTATE_MATURE) {
		ipseclog((LOG_DEBUG, "%s: invalid state.\n", __func__));
#ifdef PFKEY_STRICT_CHECKS
		return key_senderror(so, m, EINVAL);
#endif
	}
	error = key_checksockaddrs((struct sockaddr *)(src0 + 1),
	    (struct sockaddr *)(dst0 + 1));
	if (error != 0) {
		ipseclog((LOG_DEBUG, "%s: invalid sockaddr.\n", __func__));
		return key_senderror(so, m, error);
	}
	KEY_SETSECASIDX(proto, mode, reqid, src0 + 1, dst0 + 1, &saidx);
	/*
	 * Make sure the port numbers are zero.
	 * In case of NAT-T we will update them later if needed.
	 */
	KEY_PORTTOSADDR(&saidx.src, 0);
	KEY_PORTTOSADDR(&saidx.dst, 0);

	/* We can create new SA only if SPI is different. */
	sav = key_getsavbyspi(sa0->sadb_sa_spi);
	if (sav != NULL) {
		key_freesav(&sav);
		ipseclog((LOG_DEBUG, "%s: SA already exists.\n", __func__));
		return key_senderror(so, m, EEXIST);
	}

	sav = key_newsav(mhp, &saidx, sa0->sadb_sa_spi, &error);
	if (sav == NULL)
		return key_senderror(so, m, error);
	KEYDBG(KEY_STAMP,
	    printf("%s: return SA(%p)\n", __func__, sav));
	KEYDBG(KEY_DATA, kdebug_secasv(sav));
	/*
	 * If SADB_ADD was in response to SADB_ACQUIRE, we need to schedule
	 * ACQ for deletion.
	 */
	if (sav->seq != 0)
		key_acqdone(&saidx, sav->seq);

    {
	/*
	 * Don't call key_freesav() on error here, as we would like to
	 * keep the SA in the database.
	 */
	struct mbuf *n;

	/* set msg buf from mhp */
	n = key_getmsgbuf_x1(m, mhp);
	if (n == NULL) {
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		return key_senderror(so, m, ENOBUFS);
	}

	m_freem(m);
	return key_sendup_mbuf(so, n, KEY_SENDUP_ALL);
    }
}

static int
key_setnatt(struct secasvar *sav, const struct sadb_msghdr *mhp)
{
#ifdef IPSEC_NAT_T
	struct sadb_x_nat_t_port *sport, *dport;
	struct sadb_x_nat_t_type *type;

	if (!SADB_CHECKHDR(mhp, SADB_X_EXT_NAT_T_TYPE) &&
	    !SADB_CHECKHDR(mhp, SADB_X_EXT_NAT_T_SPORT) &&
	    !SADB_CHECKHDR(mhp, SADB_X_EXT_NAT_T_DPORT)) {
		if (SADB_CHECKLEN(mhp, SADB_X_EXT_NAT_T_TYPE) ||
		    SADB_CHECKLEN(mhp, SADB_X_EXT_NAT_T_SPORT) ||
		    SADB_CHECKLEN(mhp, SADB_X_EXT_NAT_T_DPORT)) {
			ipseclog((LOG_DEBUG,
			    "%s: invalid message: wrong header size.\n",
			    __func__));
			return (EINVAL);
		}

		type = (struct sadb_x_nat_t_type *)
		    mhp->ext[SADB_X_EXT_NAT_T_TYPE];
		sport = (struct sadb_x_nat_t_port *)
		    mhp->ext[SADB_X_EXT_NAT_T_SPORT];
		dport = (struct sadb_x_nat_t_port *)
		    mhp->ext[SADB_X_EXT_NAT_T_DPORT];

		sav->natt_type = type->sadb_x_nat_t_type_type;
		KEY_PORTTOSADDR(&sav->sah->saidx.src,
		    sport->sadb_x_nat_t_port_port);
		KEY_PORTTOSADDR(&sav->sah->saidx.dst,
		    dport->sadb_x_nat_t_port_port);
	} else
		return (0);
	if (!SADB_CHECKHDR(mhp, SADB_X_EXT_NAT_T_OAI) &&
	    !SADB_CHECKHDR(mhp, SADB_X_EXT_NAT_T_OAR)) {
		if (SADB_CHECKLEN(mhp, SADB_X_EXT_NAT_T_OAI) ||
		    SADB_CHECKLEN(mhp, SADB_X_EXT_NAT_T_OAR)) {
			ipseclog((LOG_DEBUG,
			    "%s: invalid message: wrong header size.\n",
			    __func__));
			return (EINVAL);
		}
		ipseclog((LOG_DEBUG, "%s: NAT-T OAi/r present\n", __func__));
	}
	if (!SADB_CHECKHDR(mhp, SADB_X_EXT_NAT_T_FRAG)) {
		if (SADB_CHECKLEN(mhp, SADB_X_EXT_NAT_T_FRAG)) {
			ipseclog((LOG_DEBUG,
			    "%s: invalid message: wrong header size.\n",
			    __func__));
			return (EINVAL);
		}
		ipseclog((LOG_DEBUG, "%s: NAT-T frag present\n", __func__));
#if 0
		struct sadb_x_nat_t_frag *frag;
		frag = (struct sadb_x_nat_t_frag *)
		    mhp->ext[SADB_X_EXT_NAT_T_FRAG];
		/*
		 * In case SADB_X_EXT_NAT_T_FRAG was not given, leave it at 0.
		 * We should actually check for a minimum MTU here, if we
		 * want to support it in ip_output.
		 */
		sav->natt_esp_frag_len = frag->sadb_x_nat_t_frag_fraglen;
#endif
	}
#endif
	return (0);
}

static int
key_setident(struct secashead *sah, const struct sadb_msghdr *mhp)
{
	const struct sadb_ident *idsrc, *iddst;
	int idsrclen, iddstlen;

	IPSEC_ASSERT(sah != NULL, ("null secashead"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	/* don't make buffer if not there */
	if (SADB_CHECKHDR(mhp, SADB_EXT_IDENTITY_SRC) &&
	    SADB_CHECKHDR(mhp, SADB_EXT_IDENTITY_DST)) {
		sah->idents = NULL;
		sah->identd = NULL;
		return (0);
	}

	if (SADB_CHECKHDR(mhp, SADB_EXT_IDENTITY_SRC) ||
	    SADB_CHECKHDR(mhp, SADB_EXT_IDENTITY_DST)) {
		ipseclog((LOG_DEBUG, "%s: invalid identity.\n", __func__));
		return (EINVAL);
	}

	idsrc = (const struct sadb_ident *)mhp->ext[SADB_EXT_IDENTITY_SRC];
	iddst = (const struct sadb_ident *)mhp->ext[SADB_EXT_IDENTITY_DST];
	idsrclen = mhp->extlen[SADB_EXT_IDENTITY_SRC];
	iddstlen = mhp->extlen[SADB_EXT_IDENTITY_DST];

	/* validity check */
	if (idsrc->sadb_ident_type != iddst->sadb_ident_type) {
		ipseclog((LOG_DEBUG, "%s: ident type mismatch.\n", __func__));
		return EINVAL;
	}

	switch (idsrc->sadb_ident_type) {
	case SADB_IDENTTYPE_PREFIX:
	case SADB_IDENTTYPE_FQDN:
	case SADB_IDENTTYPE_USERFQDN:
	default:
		/* XXX do nothing */
		sah->idents = NULL;
		sah->identd = NULL;
	 	return 0;
	}

	/* make structure */
	sah->idents = malloc(sizeof(struct secident), M_IPSEC_MISC, M_NOWAIT);
	if (sah->idents == NULL) {
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		return ENOBUFS;
	}
	sah->identd = malloc(sizeof(struct secident), M_IPSEC_MISC, M_NOWAIT);
	if (sah->identd == NULL) {
		free(sah->idents, M_IPSEC_MISC);
		sah->idents = NULL;
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		return ENOBUFS;
	}
	sah->idents->type = idsrc->sadb_ident_type;
	sah->idents->id = idsrc->sadb_ident_id;

	sah->identd->type = iddst->sadb_ident_type;
	sah->identd->id = iddst->sadb_ident_id;

	return 0;
}

/*
 * m will not be freed on return.
 * it is caller's responsibility to free the result. 
 */
static struct mbuf *
key_getmsgbuf_x1(struct mbuf *m, const struct sadb_msghdr *mhp)
{
	struct mbuf *n;

	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	/* create new sadb_msg to reply. */
	n = key_gather_mbuf(m, mhp, 1, 9, SADB_EXT_RESERVED,
	    SADB_EXT_SA, SADB_X_EXT_SA2,
	    SADB_EXT_ADDRESS_SRC, SADB_EXT_ADDRESS_DST,
	    SADB_EXT_LIFETIME_HARD, SADB_EXT_LIFETIME_SOFT,
	    SADB_EXT_IDENTITY_SRC, SADB_EXT_IDENTITY_DST);
	if (!n)
		return NULL;

	if (n->m_len < sizeof(struct sadb_msg)) {
		n = m_pullup(n, sizeof(struct sadb_msg));
		if (n == NULL)
			return NULL;
	}
	mtod(n, struct sadb_msg *)->sadb_msg_errno = 0;
	mtod(n, struct sadb_msg *)->sadb_msg_len =
	    PFKEY_UNIT64(n->m_pkthdr.len);

	return n;
}

/*
 * SADB_DELETE processing
 * receive
 *   <base, SA(*), address(SD)>
 * from the ikmpd, and set SADB_SASTATE_DEAD,
 * and send,
 *   <base, SA(*), address(SD)>
 * to the ikmpd.
 *
 * m will always be freed.
 */
static int
key_delete(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	struct secasindex saidx;
	struct sadb_address *src0, *dst0;
	struct secasvar *sav;
	struct sadb_sa *sa0;
	uint8_t proto;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	/* map satype to proto */
	if ((proto = key_satype2proto(mhp->msg->sadb_msg_satype)) == 0) {
		ipseclog((LOG_DEBUG, "%s: invalid satype is passed.\n",
		    __func__));
		return key_senderror(so, m, EINVAL);
	}

	if (SADB_CHECKHDR(mhp, SADB_EXT_ADDRESS_SRC) ||
	    SADB_CHECKHDR(mhp, SADB_EXT_ADDRESS_DST) ||
	    SADB_CHECKLEN(mhp, SADB_EXT_ADDRESS_SRC) ||
	    SADB_CHECKLEN(mhp, SADB_EXT_ADDRESS_DST)) {
		ipseclog((LOG_DEBUG, "%s: invalid message is passed.\n",
		    __func__));
		return key_senderror(so, m, EINVAL);
	}

	src0 = (struct sadb_address *)(mhp->ext[SADB_EXT_ADDRESS_SRC]);
	dst0 = (struct sadb_address *)(mhp->ext[SADB_EXT_ADDRESS_DST]);

	if (key_checksockaddrs((struct sockaddr *)(src0 + 1),
	    (struct sockaddr *)(dst0 + 1)) != 0) {
		ipseclog((LOG_DEBUG, "%s: invalid sockaddr.\n", __func__));
		return (key_senderror(so, m, EINVAL));
	}
	KEY_SETSECASIDX(proto, IPSEC_MODE_ANY, 0, src0 + 1, dst0 + 1, &saidx);
	/*
	 * Make sure the port numbers are zero.
	 * In case of NAT-T we will update them later if needed.
	 */
	KEY_PORTTOSADDR(&saidx.src, 0);
	KEY_PORTTOSADDR(&saidx.dst, 0);

	if (SADB_CHECKHDR(mhp, SADB_EXT_SA)) {
		/*
		 * Caller wants us to delete all non-LARVAL SAs
		 * that match the src/dst.  This is used during
		 * IKE INITIAL-CONTACT.
		 * XXXAE: this looks like some extension to RFC2367.
		 */
		ipseclog((LOG_DEBUG, "%s: doing delete all.\n", __func__));
		return (key_delete_all(so, m, mhp, &saidx));
	}
	if (SADB_CHECKLEN(mhp, SADB_EXT_SA)) {
		ipseclog((LOG_DEBUG,
		    "%s: invalid message: wrong header size.\n", __func__));
		return (key_senderror(so, m, EINVAL));
	}
	sa0 = (struct sadb_sa *)mhp->ext[SADB_EXT_SA];
	sav = key_getsavbyspi(sa0->sadb_sa_spi);
	if (sav == NULL) {
		ipseclog((LOG_DEBUG, "%s: no SA found for SPI %u.\n",
		    __func__, ntohl(sa0->sadb_sa_spi)));
		return (key_senderror(so, m, ESRCH));
	}
	if (key_cmpsaidx(&sav->sah->saidx, &saidx, CMP_HEAD) == 0) {
		ipseclog((LOG_DEBUG, "%s: saidx mismatched for SPI %u.\n",
		    __func__, ntohl(sav->spi)));
		key_freesav(&sav);
		return (key_senderror(so, m, ESRCH));
	}
	KEYDBG(KEY_STAMP,
	    printf("%s: SA(%p)\n", __func__, sav));
	KEYDBG(KEY_DATA, kdebug_secasv(sav));
	key_unlinksav(sav);
	key_freesav(&sav);

    {
	struct mbuf *n;
	struct sadb_msg *newmsg;

	/* create new sadb_msg to reply. */
	/* XXX-BZ NAT-T extensions? */
	n = key_gather_mbuf(m, mhp, 1, 4, SADB_EXT_RESERVED,
	    SADB_EXT_SA, SADB_EXT_ADDRESS_SRC, SADB_EXT_ADDRESS_DST);
	if (!n)
		return key_senderror(so, m, ENOBUFS);

	if (n->m_len < sizeof(struct sadb_msg)) {
		n = m_pullup(n, sizeof(struct sadb_msg));
		if (n == NULL)
			return key_senderror(so, m, ENOBUFS);
	}
	newmsg = mtod(n, struct sadb_msg *);
	newmsg->sadb_msg_errno = 0;
	newmsg->sadb_msg_len = PFKEY_UNIT64(n->m_pkthdr.len);

	m_freem(m);
	return key_sendup_mbuf(so, n, KEY_SENDUP_ALL);
    }
}

/*
 * delete all SAs for src/dst.  Called from key_delete().
 */
static int
key_delete_all(struct socket *so, struct mbuf *m,
    const struct sadb_msghdr *mhp, u_int16_t proto)
{
	struct sadb_address *src0, *dst0;
	struct secasindex saidx;
	struct secashead *sah;
	struct secasvar *sav, *nextsav;
	u_int stateidx, state;

	src0 = (struct sadb_address *)(mhp->ext[SADB_EXT_ADDRESS_SRC]);
	dst0 = (struct sadb_address *)(mhp->ext[SADB_EXT_ADDRESS_DST]);

	/* XXX boundary check against sa_len */
	KEY_SETSECASIDX(proto, IPSEC_MODE_ANY, 0, src0 + 1, dst0 + 1, &saidx);

	/*
	 * Make sure the port numbers are zero.
	 * In case of NAT-T we will update them later if needed.
	 */
	KEY_PORTTOSADDR(&saidx.src, 0);
	KEY_PORTTOSADDR(&saidx.dst, 0);

#ifdef IPSEC_NAT_T
	/*
	 * Handle NAT-T info if present.
	 */

	if (mhp->ext[SADB_X_EXT_NAT_T_SPORT] != NULL &&
	    mhp->ext[SADB_X_EXT_NAT_T_DPORT] != NULL) {
		struct sadb_x_nat_t_port *sport, *dport;

		if (mhp->extlen[SADB_X_EXT_NAT_T_SPORT] < sizeof(*sport) ||
		    mhp->extlen[SADB_X_EXT_NAT_T_DPORT] < sizeof(*dport)) {
			ipseclog((LOG_DEBUG, "%s: invalid message.\n",
			    __func__));
			return key_senderror(so, m, EINVAL);
		}

		sport = (struct sadb_x_nat_t_port *)
		    mhp->ext[SADB_X_EXT_NAT_T_SPORT];
		dport = (struct sadb_x_nat_t_port *)
		    mhp->ext[SADB_X_EXT_NAT_T_DPORT];

		if (sport)
			KEY_PORTTOSADDR(&saidx.src,
			    sport->sadb_x_nat_t_port_port);
		if (dport)
			KEY_PORTTOSADDR(&saidx.dst,
			    dport->sadb_x_nat_t_port_port);
	}
#endif

	SAHTREE_LOCK();
	LIST_FOREACH(sah, &V_sahtree, chain) {
		if (sah->state == SADB_SASTATE_DEAD)
			continue;
		if (key_cmpsaidx(&sah->saidx, &saidx, CMP_HEAD) == 0)
			continue;

		/* Delete all non-LARVAL SAs. */
		for (stateidx = 0;
		     stateidx < _ARRAYLEN(saorder_state_alive);
		     stateidx++) {
			state = saorder_state_alive[stateidx];
			if (state == SADB_SASTATE_LARVAL)
				continue;
			for (sav = LIST_FIRST(&sah->savtree[state]);
			     sav != NULL; sav = nextsav) {
				nextsav = LIST_NEXT(sav, chain);
				/* sanity check */
				if (sav->state != state) {
					ipseclog((LOG_DEBUG, "%s: invalid "
						"sav->state (queue %d SA %d)\n",
						__func__, state, sav->state));
					continue;
				}
				
				key_sa_chgstate(sav, SADB_SASTATE_DEAD);
				KEY_FREESAV(&sav);
			}
		}
	}
	SAHTREE_UNLOCK();
    {
	struct mbuf *n;
	struct sadb_msg *newmsg;

	/* create new sadb_msg to reply. */
	/* XXX-BZ NAT-T extensions? */
	n = key_gather_mbuf(m, mhp, 1, 3, SADB_EXT_RESERVED,
	    SADB_EXT_ADDRESS_SRC, SADB_EXT_ADDRESS_DST);
	if (!n)
		return key_senderror(so, m, ENOBUFS);

	if (n->m_len < sizeof(struct sadb_msg)) {
		n = m_pullup(n, sizeof(struct sadb_msg));
		if (n == NULL)
			return key_senderror(so, m, ENOBUFS);
	}
	newmsg = mtod(n, struct sadb_msg *);
	newmsg->sadb_msg_errno = 0;
	newmsg->sadb_msg_len = PFKEY_UNIT64(n->m_pkthdr.len);

	m_freem(m);
	return key_sendup_mbuf(so, n, KEY_SENDUP_ALL);
    }
}

/*
 * SADB_GET processing
 * receive
 *   <base, SA(*), address(SD)>
 * from the ikmpd, and get a SP and a SA to respond,
 * and send,
 *   <base, SA, (lifetime(HSC),) address(SD), (address(P),) key(AE),
 *       (identity(SD),) (sensitivity)>
 * to the ikmpd.
 *
 * m will always be freed.
 */
static int
key_get(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	struct sadb_sa *sa0;
	struct sadb_address *src0, *dst0;
	struct secasindex saidx;
	struct secashead *sah;
	struct secasvar *sav = NULL;
	u_int16_t proto;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	/* map satype to proto */
	if ((proto = key_satype2proto(mhp->msg->sadb_msg_satype)) == 0) {
		ipseclog((LOG_DEBUG, "%s: invalid satype is passed.\n",
			__func__));
		return key_senderror(so, m, EINVAL);
	}

	if (mhp->ext[SADB_EXT_SA] == NULL ||
	    mhp->ext[SADB_EXT_ADDRESS_SRC] == NULL ||
	    mhp->ext[SADB_EXT_ADDRESS_DST] == NULL) {
		ipseclog((LOG_DEBUG, "%s: invalid message is passed.\n",
			__func__));
		return key_senderror(so, m, EINVAL);
	}
	if (mhp->extlen[SADB_EXT_SA] < sizeof(struct sadb_sa) ||
	    mhp->extlen[SADB_EXT_ADDRESS_SRC] < sizeof(struct sadb_address) ||
	    mhp->extlen[SADB_EXT_ADDRESS_DST] < sizeof(struct sadb_address)) {
		ipseclog((LOG_DEBUG, "%s: invalid message is passed.\n",
			__func__));
		return key_senderror(so, m, EINVAL);
	}

	sa0 = (struct sadb_sa *)mhp->ext[SADB_EXT_SA];
	src0 = (struct sadb_address *)mhp->ext[SADB_EXT_ADDRESS_SRC];
	dst0 = (struct sadb_address *)mhp->ext[SADB_EXT_ADDRESS_DST];

	/* XXX boundary check against sa_len */
	KEY_SETSECASIDX(proto, IPSEC_MODE_ANY, 0, src0 + 1, dst0 + 1, &saidx);

	/*
	 * Make sure the port numbers are zero.
	 * In case of NAT-T we will update them later if needed.
	 */
	KEY_PORTTOSADDR(&saidx.src, 0);
	KEY_PORTTOSADDR(&saidx.dst, 0);

#ifdef IPSEC_NAT_T
	/*
	 * Handle NAT-T info if present.
	 */

	if (mhp->ext[SADB_X_EXT_NAT_T_SPORT] != NULL &&
	    mhp->ext[SADB_X_EXT_NAT_T_DPORT] != NULL) {
		struct sadb_x_nat_t_port *sport, *dport;

		if (mhp->extlen[SADB_X_EXT_NAT_T_SPORT] < sizeof(*sport) ||
		    mhp->extlen[SADB_X_EXT_NAT_T_DPORT] < sizeof(*dport)) {
			ipseclog((LOG_DEBUG, "%s: invalid message.\n",
			    __func__));
			return key_senderror(so, m, EINVAL);
		}

		sport = (struct sadb_x_nat_t_port *)
		    mhp->ext[SADB_X_EXT_NAT_T_SPORT];
		dport = (struct sadb_x_nat_t_port *)
		    mhp->ext[SADB_X_EXT_NAT_T_DPORT];

		if (sport)
			KEY_PORTTOSADDR(&saidx.src,
			    sport->sadb_x_nat_t_port_port);
		if (dport)
			KEY_PORTTOSADDR(&saidx.dst,
			    dport->sadb_x_nat_t_port_port);
	}
#endif

	/* get a SA header */
	SAHTREE_LOCK();
	LIST_FOREACH(sah, &V_sahtree, chain) {
		if (sah->state == SADB_SASTATE_DEAD)
			continue;
		if (key_cmpsaidx(&sah->saidx, &saidx, CMP_HEAD) == 0)
			continue;

		/* get a SA with SPI. */
		sav = key_getsavbyspi(sah, sa0->sadb_sa_spi);
		if (sav)
			break;
	}
	SAHTREE_UNLOCK();
	if (sah == NULL) {
		ipseclog((LOG_DEBUG, "%s: no SA found.\n", __func__));
		return key_senderror(so, m, ENOENT);
	}

    {
	struct mbuf *n;
	u_int8_t satype;

	/* map proto to satype */
	if ((satype = key_proto2satype(sah->saidx.proto)) == 0) {
		ipseclog((LOG_DEBUG, "%s: there was invalid proto in SAD.\n",
			__func__));
		return key_senderror(so, m, EINVAL);
	}

	/* create new sadb_msg to reply. */
	n = key_setdumpsa(sav, SADB_GET, satype, mhp->msg->sadb_msg_seq,
	    mhp->msg->sadb_msg_pid);
	if (!n)
		return key_senderror(so, m, ENOBUFS);

	m_freem(m);
	return key_sendup_mbuf(so, n, KEY_SENDUP_ONE);
    }
}

/* XXX make it sysctl-configurable? */
static void
key_getcomb_setlifetime(struct sadb_comb *comb)
{

	comb->sadb_comb_soft_allocations = 1;
	comb->sadb_comb_hard_allocations = 1;
	comb->sadb_comb_soft_bytes = 0;
	comb->sadb_comb_hard_bytes = 0;
	comb->sadb_comb_hard_addtime = 86400;	/* 1 day */
	comb->sadb_comb_soft_addtime = comb->sadb_comb_soft_addtime * 80 / 100;
	comb->sadb_comb_soft_usetime = 28800;	/* 8 hours */
	comb->sadb_comb_hard_usetime = comb->sadb_comb_hard_usetime * 80 / 100;
}

/*
 * XXX reorder combinations by preference
 * XXX no idea if the user wants ESP authentication or not
 */
static struct mbuf *
key_getcomb_esp()
{
	struct sadb_comb *comb;
	struct enc_xform *algo;
	struct mbuf *result = NULL, *m, *n;
	int encmin;
	int i, off, o;
	int totlen;
	const int l = PFKEY_ALIGN8(sizeof(struct sadb_comb));

	m = NULL;
	for (i = 1; i <= SADB_EALG_MAX; i++) {
		algo = esp_algorithm_lookup(i);
		if (algo == NULL)
			continue;

		/* discard algorithms with key size smaller than system min */
		if (_BITS(algo->maxkey) < V_ipsec_esp_keymin)
			continue;
		if (_BITS(algo->minkey) < V_ipsec_esp_keymin)
			encmin = V_ipsec_esp_keymin;
		else
			encmin = _BITS(algo->minkey);

		if (V_ipsec_esp_auth)
			m = key_getcomb_ah();
		else {
			IPSEC_ASSERT(l <= MLEN,
				("l=%u > MLEN=%lu", l, (u_long) MLEN));
			MGET(m, M_NOWAIT, MT_DATA);
			if (m) {
				M_ALIGN(m, l);
				m->m_len = l;
				m->m_next = NULL;
				bzero(mtod(m, caddr_t), m->m_len);
			}
		}
		if (!m)
			goto fail;

		totlen = 0;
		for (n = m; n; n = n->m_next)
			totlen += n->m_len;
		IPSEC_ASSERT((totlen % l) == 0, ("totlen=%u, l=%u", totlen, l));

		for (off = 0; off < totlen; off += l) {
			n = m_pulldown(m, off, l, &o);
			if (!n) {
				/* m is already freed */
				goto fail;
			}
			comb = (struct sadb_comb *)(mtod(n, caddr_t) + o);
			bzero(comb, sizeof(*comb));
			key_getcomb_setlifetime(comb);
			comb->sadb_comb_encrypt = i;
			comb->sadb_comb_encrypt_minbits = encmin;
			comb->sadb_comb_encrypt_maxbits = _BITS(algo->maxkey);
		}

		if (!result)
			result = m;
		else
			m_cat(result, m);
	}

	return result;

 fail:
	if (result)
		m_freem(result);
	return NULL;
}

static void
key_getsizes_ah(const struct auth_hash *ah, int alg, u_int16_t* min,
    u_int16_t* max)
{

	*min = *max = ah->keysize;
	if (ah->keysize == 0) {
		/*
		 * Transform takes arbitrary key size but algorithm
		 * key size is restricted.  Enforce this here.
		 */
		switch (alg) {
		case SADB_X_AALG_MD5:	*min = *max = 16; break;
		case SADB_X_AALG_SHA:	*min = *max = 20; break;
		case SADB_X_AALG_NULL:	*min = 1; *max = 256; break;
		case SADB_X_AALG_SHA2_256: *min = *max = 32; break;
		case SADB_X_AALG_SHA2_384: *min = *max = 48; break;
		case SADB_X_AALG_SHA2_512: *min = *max = 64; break;
		default:
			DPRINTF(("%s: unknown AH algorithm %u\n",
				__func__, alg));
			break;
		}
	}
}

/*
 * XXX reorder combinations by preference
 */
static struct mbuf *
key_getcomb_ah()
{
	struct sadb_comb *comb;
	struct auth_hash *algo;
	struct mbuf *m;
	u_int16_t minkeysize, maxkeysize;
	int i;
	const int l = PFKEY_ALIGN8(sizeof(struct sadb_comb));

	m = NULL;
	for (i = 1; i <= SADB_AALG_MAX; i++) {
#if 1
		/* we prefer HMAC algorithms, not old algorithms */
		if (i != SADB_AALG_SHA1HMAC &&
		    i != SADB_AALG_MD5HMAC  &&
		    i != SADB_X_AALG_SHA2_256 &&
		    i != SADB_X_AALG_SHA2_384 &&
		    i != SADB_X_AALG_SHA2_512)
			continue;
#endif
		algo = ah_algorithm_lookup(i);
		if (!algo)
			continue;
		key_getsizes_ah(algo, i, &minkeysize, &maxkeysize);
		/* discard algorithms with key size smaller than system min */
		if (_BITS(minkeysize) < V_ipsec_ah_keymin)
			continue;

		if (!m) {
			IPSEC_ASSERT(l <= MLEN,
				("l=%u > MLEN=%lu", l, (u_long) MLEN));
			MGET(m, M_NOWAIT, MT_DATA);
			if (m) {
				M_ALIGN(m, l);
				m->m_len = l;
				m->m_next = NULL;
			}
		} else
			M_PREPEND(m, l, M_NOWAIT);
		if (!m)
			return NULL;

		comb = mtod(m, struct sadb_comb *);
		bzero(comb, sizeof(*comb));
		key_getcomb_setlifetime(comb);
		comb->sadb_comb_auth = i;
		comb->sadb_comb_auth_minbits = _BITS(minkeysize);
		comb->sadb_comb_auth_maxbits = _BITS(maxkeysize);
	}

	return m;
}

/*
 * not really an official behavior.  discussed in pf_key@inner.net in Sep2000.
 * XXX reorder combinations by preference
 */
static struct mbuf *
key_getcomb_ipcomp()
{
	struct sadb_comb *comb;
	struct comp_algo *algo;
	struct mbuf *m;
	int i;
	const int l = PFKEY_ALIGN8(sizeof(struct sadb_comb));

	m = NULL;
	for (i = 1; i <= SADB_X_CALG_MAX; i++) {
		algo = ipcomp_algorithm_lookup(i);
		if (!algo)
			continue;

		if (!m) {
			IPSEC_ASSERT(l <= MLEN,
				("l=%u > MLEN=%lu", l, (u_long) MLEN));
			MGET(m, M_NOWAIT, MT_DATA);
			if (m) {
				M_ALIGN(m, l);
				m->m_len = l;
				m->m_next = NULL;
			}
		} else
			M_PREPEND(m, l, M_NOWAIT);
		if (!m)
			return NULL;

		comb = mtod(m, struct sadb_comb *);
		bzero(comb, sizeof(*comb));
		key_getcomb_setlifetime(comb);
		comb->sadb_comb_encrypt = i;
		/* what should we set into sadb_comb_*_{min,max}bits? */
	}

	return m;
}

/*
 * XXX no way to pass mode (transport/tunnel) to userland
 * XXX replay checking?
 * XXX sysctl interface to ipsec_{ah,esp}_keymin
 */
static struct mbuf *
key_getprop(const struct secasindex *saidx)
{
	struct sadb_prop *prop;
	struct mbuf *m, *n;
	const int l = PFKEY_ALIGN8(sizeof(struct sadb_prop));
	int totlen;

	switch (saidx->proto)  {
	case IPPROTO_ESP:
		m = key_getcomb_esp();
		break;
	case IPPROTO_AH:
		m = key_getcomb_ah();
		break;
	case IPPROTO_IPCOMP:
		m = key_getcomb_ipcomp();
		break;
	default:
		return NULL;
	}

	if (!m)
		return NULL;
	M_PREPEND(m, l, M_NOWAIT);
	if (!m)
		return NULL;

	totlen = 0;
	for (n = m; n; n = n->m_next)
		totlen += n->m_len;

	prop = mtod(m, struct sadb_prop *);
	bzero(prop, sizeof(*prop));
	prop->sadb_prop_len = PFKEY_UNIT64(totlen);
	prop->sadb_prop_exttype = SADB_EXT_PROPOSAL;
	prop->sadb_prop_replay = 32;	/* XXX */

	return m;
}

/*
 * SADB_ACQUIRE processing called by key_checkrequest() and key_acquire2().
 * send
 *   <base, SA, address(SD), (address(P)), x_policy,
 *       (identity(SD),) (sensitivity,) proposal>
 * to KMD, and expect to receive
 *   <base> with SADB_ACQUIRE if error occurred,
 * or
 *   <base, src address, dst address, (SPI range)> with SADB_GETSPI
 * from KMD by PF_KEY.
 *
 * XXX x_policy is outside of RFC2367 (KAME extension).
 * XXX sensitivity is not supported.
 * XXX for ipcomp, RFC2367 does not define how to fill in proposal.
 * see comment for key_getcomb_ipcomp().
 *
 * OUT:
 *    0     : succeed
 *    others: error number
 */
static int
key_acquire(const struct secasindex *saidx, struct secpolicy *sp)
{
	union sockaddr_union addr;
	struct mbuf *result, *m;
	struct secacq *newacq;
	u_int32_t seq;
	int error;
	u_int16_t ul_proto;
	u_int8_t mask, satype;

	IPSEC_ASSERT(saidx != NULL, ("null saidx"));
	satype = key_proto2satype(saidx->proto);
	IPSEC_ASSERT(satype != 0, ("null satype, protocol %u", saidx->proto));

	error = -1;
	result = NULL;
	ul_proto = IPSEC_ULPROTO_ANY;
	/*
	 * We never do anything about acquirng SA.  There is anather
	 * solution that kernel blocks to send SADB_ACQUIRE message until
	 * getting something message from IKEd.  In later case, to be
	 * managed with ACQUIRING list.
	 */
	/* Get an entry to check whether sending message or not. */
	if ((newacq = key_getacq(saidx)) != NULL) {
		if (V_key_blockacq_count < newacq->count) {
			/* reset counter and do send message. */
			newacq->count = 0;
		} else {
			/* increment counter and do nothing. */
			newacq->count++;
			return 0;
		}
	} else {
		/* make new entry for blocking to send SADB_ACQUIRE. */
		if ((newacq = key_newacq(saidx)) == NULL)
			return ENOBUFS;
	}


	seq = newacq->seq;
	m = key_setsadbmsg(SADB_ACQUIRE, 0, satype, seq, 0, 0);
	if (!m) {
		error = ENOBUFS;
		goto fail;
	}
	result = m;

	/*
	 * No SADB_X_EXT_NAT_T_* here: we do not know
	 * anything related to NAT-T at this time.
	 */

	/*
	 * set sadb_address for saidx's.
	 *
	 * Note that if sp is supplied, then we're being called from
	 * key_checkrequest and should supply port and protocol information.
	 */
	if (sp != NULL && (sp->spidx.ul_proto == IPPROTO_TCP ||
	    sp->spidx.ul_proto == IPPROTO_UDP))
		ul_proto = sp->spidx.ul_proto;

	addr = saidx->src;
	mask = FULLMASK;
	if (ul_proto != IPSEC_ULPROTO_ANY) {
		switch (sp->spidx.src.sa.sa_family) {
		case AF_INET:
			if (sp->spidx.src.sin.sin_port != IPSEC_PORT_ANY) {
				addr.sin.sin_port = sp->spidx.src.sin.sin_port;
				mask = sp->spidx.prefs;
			}
			break;
		case AF_INET6:
			if (sp->spidx.src.sin6.sin6_port != IPSEC_PORT_ANY) {
				addr.sin6.sin6_port = sp->spidx.src.sin6.sin6_port;
				mask = sp->spidx.prefs;
			}
			break;
		default:
			break;
		}
	}
	m = key_setsadbaddr(SADB_EXT_ADDRESS_SRC, &addr.sa, mask, ul_proto);
	if (!m) {
		error = ENOBUFS;
		goto fail;
	}
	m_cat(result, m);

	addr = saidx->dst;
	mask = FULLMASK;
	if (ul_proto != IPSEC_ULPROTO_ANY) {
		switch (sp->spidx.dst.sa.sa_family) {
		case AF_INET:
			if (sp->spidx.dst.sin.sin_port != IPSEC_PORT_ANY) {
				addr.sin.sin_port = sp->spidx.dst.sin.sin_port;
				mask = sp->spidx.prefd;
			}
			break;
		case AF_INET6:
			if (sp->spidx.dst.sin6.sin6_port != IPSEC_PORT_ANY) {
				addr.sin6.sin6_port = sp->spidx.dst.sin6.sin6_port;
				mask = sp->spidx.prefd;
			}
			break;
		default:
			break;
		}
	}
	m = key_setsadbaddr(SADB_EXT_ADDRESS_DST, &addr.sa, mask, ul_proto);
	if (!m) {
		error = ENOBUFS;
		goto fail;
	}
	m_cat(result, m);

	/* XXX proxy address (optional) */

	/* set sadb_x_policy */
	if (sp) {
		m = key_setsadbxpolicy(sp->policy, sp->spidx.dir, sp->id, sp->priority);
		if (!m) {
			error = ENOBUFS;
			goto fail;
		}
		m_cat(result, m);
	}

	/* XXX identity (optional) */
#if 0
	if (idexttype && fqdn) {
		/* create identity extension (FQDN) */
		struct sadb_ident *id;
		int fqdnlen;

		fqdnlen = strlen(fqdn) + 1;	/* +1 for terminating-NUL */
		id = (struct sadb_ident *)p;
		bzero(id, sizeof(*id) + PFKEY_ALIGN8(fqdnlen));
		id->sadb_ident_len = PFKEY_UNIT64(sizeof(*id) + PFKEY_ALIGN8(fqdnlen));
		id->sadb_ident_exttype = idexttype;
		id->sadb_ident_type = SADB_IDENTTYPE_FQDN;
		bcopy(fqdn, id + 1, fqdnlen);
		p += sizeof(struct sadb_ident) + PFKEY_ALIGN8(fqdnlen);
	}

	if (idexttype) {
		/* create identity extension (USERFQDN) */
		struct sadb_ident *id;
		int userfqdnlen;

		if (userfqdn) {
			/* +1 for terminating-NUL */
			userfqdnlen = strlen(userfqdn) + 1;
		} else
			userfqdnlen = 0;
		id = (struct sadb_ident *)p;
		bzero(id, sizeof(*id) + PFKEY_ALIGN8(userfqdnlen));
		id->sadb_ident_len = PFKEY_UNIT64(sizeof(*id) + PFKEY_ALIGN8(userfqdnlen));
		id->sadb_ident_exttype = idexttype;
		id->sadb_ident_type = SADB_IDENTTYPE_USERFQDN;
		/* XXX is it correct? */
		if (curproc && curproc->p_cred)
			id->sadb_ident_id = curproc->p_cred->p_ruid;
		if (userfqdn && userfqdnlen)
			bcopy(userfqdn, id + 1, userfqdnlen);
		p += sizeof(struct sadb_ident) + PFKEY_ALIGN8(userfqdnlen);
	}
#endif

	/* XXX sensitivity (optional) */

	/* create proposal/combination extension */
	m = key_getprop(saidx);
#if 0
	/*
	 * spec conformant: always attach proposal/combination extension,
	 * the problem is that we have no way to attach it for ipcomp,
	 * due to the way sadb_comb is declared in RFC2367.
	 */
	if (!m) {
		error = ENOBUFS;
		goto fail;
	}
	m_cat(result, m);
#else
	/*
	 * outside of spec; make proposal/combination extension optional.
	 */
	if (m)
		m_cat(result, m);
#endif

	if ((result->m_flags & M_PKTHDR) == 0) {
		error = EINVAL;
		goto fail;
	}

	if (result->m_len < sizeof(struct sadb_msg)) {
		result = m_pullup(result, sizeof(struct sadb_msg));
		if (result == NULL) {
			error = ENOBUFS;
			goto fail;
		}
	}

	result->m_pkthdr.len = 0;
	for (m = result; m; m = m->m_next)
		result->m_pkthdr.len += m->m_len;

	mtod(result, struct sadb_msg *)->sadb_msg_len =
	    PFKEY_UNIT64(result->m_pkthdr.len);

	return key_sendup_mbuf(NULL, result, KEY_SENDUP_REGISTERED);

 fail:
	if (result)
		m_freem(result);
	return error;
}

static struct secacq *
key_newacq(const struct secasindex *saidx)
{
	struct secacq *newacq;

	/* get new entry */
	newacq = malloc(sizeof(struct secacq), M_IPSEC_SAQ, M_NOWAIT|M_ZERO);
	if (newacq == NULL) {
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		return NULL;
	}

	/* copy secindex */
	bcopy(saidx, &newacq->saidx, sizeof(newacq->saidx));
	newacq->seq = (V_acq_seq == ~0 ? 1 : ++V_acq_seq);
	newacq->created = time_second;
	newacq->count = 0;

	/* add to acqtree */
	ACQ_LOCK();
	LIST_INSERT_HEAD(&V_acqtree, newacq, chain);
	ACQ_UNLOCK();

	return newacq;
}

static struct secacq *
key_getacq(const struct secasindex *saidx)
{
	struct secacq *acq;

	ACQ_LOCK();
	LIST_FOREACH(acq, &V_acqtree, chain) {
		if (key_cmpsaidx(saidx, &acq->saidx, CMP_EXACTLY))
			break;
	}
	ACQ_UNLOCK();

	return acq;
}

static struct secacq *
key_getacqbyseq(u_int32_t seq)
{
	struct secacq *acq;

	ACQ_LOCK();
	LIST_FOREACH(acq, &V_acqtree, chain) {
		if (acq->seq == seq)
			break;
	}
	ACQ_UNLOCK();

	return acq;
}

static struct secspacq *
key_newspacq(struct secpolicyindex *spidx)
{
	struct secspacq *acq;

	/* get new entry */
	acq = malloc(sizeof(struct secspacq), M_IPSEC_SAQ, M_NOWAIT|M_ZERO);
	if (acq == NULL) {
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		return NULL;
	}

	/* copy secindex */
	bcopy(spidx, &acq->spidx, sizeof(acq->spidx));
	acq->created = time_second;
	acq->count = 0;

	/* add to spacqtree */
	SPACQ_LOCK();
	LIST_INSERT_HEAD(&V_spacqtree, acq, chain);
	SPACQ_UNLOCK();

	return acq;
}

static struct secspacq *
key_getspacq(struct secpolicyindex *spidx)
{
	struct secspacq *acq;

	SPACQ_LOCK();
	LIST_FOREACH(acq, &V_spacqtree, chain) {
		if (key_cmpspidx_exactly(spidx, &acq->spidx)) {
			/* NB: return holding spacq_lock */
			return acq;
		}
	}
	SPACQ_UNLOCK();

	return NULL;
}

/*
 * SADB_ACQUIRE processing,
 * in first situation, is receiving
 *   <base>
 * from the ikmpd, and clear sequence of its secasvar entry.
 *
 * In second situation, is receiving
 *   <base, address(SD), (address(P),) (identity(SD),) (sensitivity,) proposal>
 * from a user land process, and return
 *   <base, address(SD), (address(P),) (identity(SD),) (sensitivity,) proposal>
 * to the socket.
 *
 * m will always be freed.
 */
static int
key_acquire2(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	const struct sadb_address *src0, *dst0;
	struct secasindex saidx;
	struct secashead *sah;
	u_int16_t proto;
	int error;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	/*
	 * Error message from KMd.
	 * We assume that if error was occurred in IKEd, the length of PFKEY
	 * message is equal to the size of sadb_msg structure.
	 * We do not raise error even if error occurred in this function.
	 */
	if (mhp->msg->sadb_msg_len == PFKEY_UNIT64(sizeof(struct sadb_msg))) {
		struct secacq *acq;

		/* check sequence number */
		if (mhp->msg->sadb_msg_seq == 0) {
			ipseclog((LOG_DEBUG, "%s: must specify sequence "
				"number.\n", __func__));
			m_freem(m);
			return 0;
		}

		if ((acq = key_getacqbyseq(mhp->msg->sadb_msg_seq)) == NULL) {
			/*
			 * the specified larval SA is already gone, or we got
			 * a bogus sequence number.  we can silently ignore it.
			 */
			m_freem(m);
			return 0;
		}

		/* reset acq counter in order to deletion by timehander. */
		acq->created = time_second;
		acq->count = 0;
		m_freem(m);
		return 0;
	}

	/*
	 * This message is from user land.
	 */

	/* map satype to proto */
	if ((proto = key_satype2proto(mhp->msg->sadb_msg_satype)) == 0) {
		ipseclog((LOG_DEBUG, "%s: invalid satype is passed.\n",
			__func__));
		return key_senderror(so, m, EINVAL);
	}

	if (mhp->ext[SADB_EXT_ADDRESS_SRC] == NULL ||
	    mhp->ext[SADB_EXT_ADDRESS_DST] == NULL ||
	    mhp->ext[SADB_EXT_PROPOSAL] == NULL) {
		/* error */
		ipseclog((LOG_DEBUG, "%s: invalid message is passed.\n",
			__func__));
		return key_senderror(so, m, EINVAL);
	}
	if (mhp->extlen[SADB_EXT_ADDRESS_SRC] < sizeof(struct sadb_address) ||
	    mhp->extlen[SADB_EXT_ADDRESS_DST] < sizeof(struct sadb_address) ||
	    mhp->extlen[SADB_EXT_PROPOSAL] < sizeof(struct sadb_prop)) {
		/* error */
		ipseclog((LOG_DEBUG, "%s: invalid message is passed.\n",	
			__func__));
		return key_senderror(so, m, EINVAL);
	}

	src0 = (struct sadb_address *)mhp->ext[SADB_EXT_ADDRESS_SRC];
	dst0 = (struct sadb_address *)mhp->ext[SADB_EXT_ADDRESS_DST];

	/* XXX boundary check against sa_len */
	KEY_SETSECASIDX(proto, IPSEC_MODE_ANY, 0, src0 + 1, dst0 + 1, &saidx);

	/*
	 * Make sure the port numbers are zero.
	 * In case of NAT-T we will update them later if needed.
	 */
	KEY_PORTTOSADDR(&saidx.src, 0);
	KEY_PORTTOSADDR(&saidx.dst, 0);

#ifndef IPSEC_NAT_T
	/*
	 * Handle NAT-T info if present.
	 */

	if (mhp->ext[SADB_X_EXT_NAT_T_SPORT] != NULL &&
	    mhp->ext[SADB_X_EXT_NAT_T_DPORT] != NULL) {
		struct sadb_x_nat_t_port *sport, *dport;

		if (mhp->extlen[SADB_X_EXT_NAT_T_SPORT] < sizeof(*sport) ||
		    mhp->extlen[SADB_X_EXT_NAT_T_DPORT] < sizeof(*dport)) {
			ipseclog((LOG_DEBUG, "%s: invalid message.\n",
			    __func__));
			return key_senderror(so, m, EINVAL);
		}

		sport = (struct sadb_x_nat_t_port *)
		    mhp->ext[SADB_X_EXT_NAT_T_SPORT];
		dport = (struct sadb_x_nat_t_port *)
		    mhp->ext[SADB_X_EXT_NAT_T_DPORT];

		if (sport)
			KEY_PORTTOSADDR(&saidx.src,
			    sport->sadb_x_nat_t_port_port);
		if (dport)
			KEY_PORTTOSADDR(&saidx.dst,
			    dport->sadb_x_nat_t_port_port);
	}
#endif

	/* get a SA index */
	SAHTREE_LOCK();
	LIST_FOREACH(sah, &V_sahtree, chain) {
		if (sah->state == SADB_SASTATE_DEAD)
			continue;
		if (key_cmpsaidx(&sah->saidx, &saidx, CMP_MODE_REQID))
			break;
	}
	SAHTREE_UNLOCK();
	if (sah != NULL) {
		ipseclog((LOG_DEBUG, "%s: a SA exists already.\n", __func__));
		return key_senderror(so, m, EEXIST);
	}

	error = key_acquire(&saidx, NULL);
	if (error != 0) {
		ipseclog((LOG_DEBUG, "%s: error %d returned from key_acquire\n",
			__func__, mhp->msg->sadb_msg_errno));
		return key_senderror(so, m, error);
	}

	return key_sendup_mbuf(so, m, KEY_SENDUP_REGISTERED);
}

/*
 * SADB_REGISTER processing.
 * If SATYPE_UNSPEC has been passed as satype, only return sabd_supported.
 * receive
 *   <base>
 * from the ikmpd, and register a socket to send PF_KEY messages,
 * and send
 *   <base, supported>
 * to KMD by PF_KEY.
 * If socket is detached, must free from regnode.
 *
 * m will always be freed.
 */
static int
key_register(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	struct secreg *reg, *newreg = NULL;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	/* check for invalid register message */
	if (mhp->msg->sadb_msg_satype >= sizeof(V_regtree)/sizeof(V_regtree[0]))
		return key_senderror(so, m, EINVAL);

	/* When SATYPE_UNSPEC is specified, only return sabd_supported. */
	if (mhp->msg->sadb_msg_satype == SADB_SATYPE_UNSPEC)
		goto setmsg;

	/* check whether existing or not */
	REGTREE_LOCK();
	LIST_FOREACH(reg, &V_regtree[mhp->msg->sadb_msg_satype], chain) {
		if (reg->so == so) {
			REGTREE_UNLOCK();
			ipseclog((LOG_DEBUG, "%s: socket exists already.\n",
				__func__));
			return key_senderror(so, m, EEXIST);
		}
	}

	/* create regnode */
	newreg =  malloc(sizeof(struct secreg), M_IPSEC_SAR, M_NOWAIT|M_ZERO);
	if (newreg == NULL) {
		REGTREE_UNLOCK();
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		return key_senderror(so, m, ENOBUFS);
	}

	newreg->so = so;
	((struct keycb *)sotorawcb(so))->kp_registered++;

	/* add regnode to regtree. */
	LIST_INSERT_HEAD(&V_regtree[mhp->msg->sadb_msg_satype], newreg, chain);
	REGTREE_UNLOCK();

  setmsg:
    {
	struct mbuf *n;
	struct sadb_msg *newmsg;
	struct sadb_supported *sup;
	u_int len, alen, elen;
	int off;
	int i;
	struct sadb_alg *alg;

	/* create new sadb_msg to reply. */
	alen = 0;
	for (i = 1; i <= SADB_AALG_MAX; i++) {
		if (ah_algorithm_lookup(i))
			alen += sizeof(struct sadb_alg);
	}
	if (alen)
		alen += sizeof(struct sadb_supported);
	elen = 0;
	for (i = 1; i <= SADB_EALG_MAX; i++) {
		if (esp_algorithm_lookup(i))
			elen += sizeof(struct sadb_alg);
	}
	if (elen)
		elen += sizeof(struct sadb_supported);

	len = sizeof(struct sadb_msg) + alen + elen;

	if (len > MCLBYTES)
		return key_senderror(so, m, ENOBUFS);

	MGETHDR(n, M_NOWAIT, MT_DATA);
	if (len > MHLEN) {
		if (!(MCLGET(n, M_NOWAIT))) {
			m_freem(n);
			n = NULL;
		}
	}
	if (!n)
		return key_senderror(so, m, ENOBUFS);

	n->m_pkthdr.len = n->m_len = len;
	n->m_next = NULL;
	off = 0;

	m_copydata(m, 0, sizeof(struct sadb_msg), mtod(n, caddr_t) + off);
	newmsg = mtod(n, struct sadb_msg *);
	newmsg->sadb_msg_errno = 0;
	newmsg->sadb_msg_len = PFKEY_UNIT64(len);
	off += PFKEY_ALIGN8(sizeof(struct sadb_msg));

	/* for authentication algorithm */
	if (alen) {
		sup = (struct sadb_supported *)(mtod(n, caddr_t) + off);
		sup->sadb_supported_len = PFKEY_UNIT64(alen);
		sup->sadb_supported_exttype = SADB_EXT_SUPPORTED_AUTH;
		off += PFKEY_ALIGN8(sizeof(*sup));

		for (i = 1; i <= SADB_AALG_MAX; i++) {
			struct auth_hash *aalgo;
			u_int16_t minkeysize, maxkeysize;

			aalgo = ah_algorithm_lookup(i);
			if (!aalgo)
				continue;
			alg = (struct sadb_alg *)(mtod(n, caddr_t) + off);
			alg->sadb_alg_id = i;
			alg->sadb_alg_ivlen = 0;
			key_getsizes_ah(aalgo, i, &minkeysize, &maxkeysize);
			alg->sadb_alg_minbits = _BITS(minkeysize);
			alg->sadb_alg_maxbits = _BITS(maxkeysize);
			off += PFKEY_ALIGN8(sizeof(*alg));
		}
	}

	/* for encryption algorithm */
	if (elen) {
		sup = (struct sadb_supported *)(mtod(n, caddr_t) + off);
		sup->sadb_supported_len = PFKEY_UNIT64(elen);
		sup->sadb_supported_exttype = SADB_EXT_SUPPORTED_ENCRYPT;
		off += PFKEY_ALIGN8(sizeof(*sup));

		for (i = 1; i <= SADB_EALG_MAX; i++) {
			struct enc_xform *ealgo;

			ealgo = esp_algorithm_lookup(i);
			if (!ealgo)
				continue;
			alg = (struct sadb_alg *)(mtod(n, caddr_t) + off);
			alg->sadb_alg_id = i;
			alg->sadb_alg_ivlen = ealgo->ivsize;
			alg->sadb_alg_minbits = _BITS(ealgo->minkey);
			alg->sadb_alg_maxbits = _BITS(ealgo->maxkey);
			off += PFKEY_ALIGN8(sizeof(struct sadb_alg));
		}
	}

	IPSEC_ASSERT(off == len,
		("length assumption failed (off %u len %u)", off, len));

	m_freem(m);
	return key_sendup_mbuf(so, n, KEY_SENDUP_REGISTERED);
    }
}

/*
 * free secreg entry registered.
 * XXX: I want to do free a socket marked done SADB_RESIGER to socket.
 */
void
key_freereg(struct socket *so)
{
	struct secreg *reg;
	int i;

	IPSEC_ASSERT(so != NULL, ("NULL so"));

	/*
	 * check whether existing or not.
	 * check all type of SA, because there is a potential that
	 * one socket is registered to multiple type of SA.
	 */
	REGTREE_LOCK();
	for (i = 0; i <= SADB_SATYPE_MAX; i++) {
		LIST_FOREACH(reg, &V_regtree[i], chain) {
			if (reg->so == so && __LIST_CHAINED(reg)) {
				LIST_REMOVE(reg, chain);
				free(reg, M_IPSEC_SAR);
				break;
			}
		}
	}
	REGTREE_UNLOCK();
}

/*
 * SADB_EXPIRE processing
 * send
 *   <base, SA, SA2, lifetime(C and one of HS), address(SD)>
 * to KMD by PF_KEY.
 * NOTE: We send only soft lifetime extension.
 *
 * OUT:	0	: succeed
 *	others	: error number
 */
static int
key_expire(struct secasvar *sav, int hard)
{
	int satype;
	struct mbuf *result = NULL, *m;
	int len;
	int error = -1;
	struct sadb_lifetime *lt;

	IPSEC_ASSERT (sav != NULL, ("null sav"));
	IPSEC_ASSERT (sav->sah != NULL, ("null sa header"));

	/* set msg header */
	satype = key_proto2satype(sav->sah->saidx.proto);
	IPSEC_ASSERT(satype != 0, ("invalid proto, satype %u", satype));
	m = key_setsadbmsg(SADB_EXPIRE, 0, satype, sav->seq, 0, sav->refcnt);
	if (!m) {
		error = ENOBUFS;
		goto fail;
	}
	result = m;

	/* create SA extension */
	m = key_setsadbsa(sav);
	if (!m) {
		error = ENOBUFS;
		goto fail;
	}
	m_cat(result, m);

	/* create SA extension */
	m = key_setsadbxsa2(sav->sah->saidx.mode,
			sav->replay ? sav->replay->count : 0,
			sav->sah->saidx.reqid);
	if (!m) {
		error = ENOBUFS;
		goto fail;
	}
	m_cat(result, m);

	/* create lifetime extension (current and soft) */
	len = PFKEY_ALIGN8(sizeof(*lt)) * 2;
	m = m_get2(len, M_NOWAIT, MT_DATA, 0);
	if (m == NULL) {
		error = ENOBUFS;
		goto fail;
	}
	m_align(m, len);
	m->m_len = len;
	bzero(mtod(m, caddr_t), len);
	lt = mtod(m, struct sadb_lifetime *);
	lt->sadb_lifetime_len = PFKEY_UNIT64(sizeof(struct sadb_lifetime));
	lt->sadb_lifetime_exttype = SADB_EXT_LIFETIME_CURRENT;
	lt->sadb_lifetime_allocations = sav->lft_c->allocations;
	lt->sadb_lifetime_bytes = sav->lft_c->bytes;
	lt->sadb_lifetime_addtime = sav->lft_c->addtime;
	lt->sadb_lifetime_usetime = sav->lft_c->usetime;
	lt = (struct sadb_lifetime *)(mtod(m, caddr_t) + len / 2);
	lt->sadb_lifetime_len = PFKEY_UNIT64(sizeof(struct sadb_lifetime));
	if (hard) {
		lt->sadb_lifetime_exttype = SADB_EXT_LIFETIME_HARD;
		lt->sadb_lifetime_allocations = sav->lft_h->allocations;
		lt->sadb_lifetime_bytes = sav->lft_h->bytes;
		lt->sadb_lifetime_addtime = sav->lft_h->addtime;
		lt->sadb_lifetime_usetime = sav->lft_h->usetime;
	} else {
		lt->sadb_lifetime_exttype = SADB_EXT_LIFETIME_SOFT;
		lt->sadb_lifetime_allocations = sav->lft_s->allocations;
		lt->sadb_lifetime_bytes = sav->lft_s->bytes;
		lt->sadb_lifetime_addtime = sav->lft_s->addtime;
		lt->sadb_lifetime_usetime = sav->lft_s->usetime;
	}
	m_cat(result, m);

	/* set sadb_address for source */
	m = key_setsadbaddr(SADB_EXT_ADDRESS_SRC,
	    &sav->sah->saidx.src.sa,
	    FULLMASK, IPSEC_ULPROTO_ANY);
	if (!m) {
		error = ENOBUFS;
		goto fail;
	}
	m_cat(result, m);

	/* set sadb_address for destination */
	m = key_setsadbaddr(SADB_EXT_ADDRESS_DST,
	    &sav->sah->saidx.dst.sa,
	    FULLMASK, IPSEC_ULPROTO_ANY);
	if (!m) {
		error = ENOBUFS;
		goto fail;
	}
	m_cat(result, m);

	/*
	 * XXX-BZ Handle NAT-T extensions here.
	 */

	if ((result->m_flags & M_PKTHDR) == 0) {
		error = EINVAL;
		goto fail;
	}

	if (result->m_len < sizeof(struct sadb_msg)) {
		result = m_pullup(result, sizeof(struct sadb_msg));
		if (result == NULL) {
			error = ENOBUFS;
			goto fail;
		}
	}

	result->m_pkthdr.len = 0;
	for (m = result; m; m = m->m_next)
		result->m_pkthdr.len += m->m_len;

	mtod(result, struct sadb_msg *)->sadb_msg_len =
	    PFKEY_UNIT64(result->m_pkthdr.len);

	return key_sendup_mbuf(NULL, result, KEY_SENDUP_REGISTERED);

 fail:
	if (result)
		m_freem(result);
	return error;
}

/*
 * SADB_FLUSH processing
 * receive
 *   <base>
 * from the ikmpd, and free all entries in secastree.
 * and send,
 *   <base>
 * to the ikmpd.
 * NOTE: to do is only marking SADB_SASTATE_DEAD.
 *
 * m will always be freed.
 */
static int
key_flush(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	struct sadb_msg *newmsg;
	struct secashead *sah, *nextsah;
	struct secasvar *sav, *nextsav;
	u_int16_t proto;
	u_int8_t state;
	u_int stateidx;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	/* map satype to proto */
	if ((proto = key_satype2proto(mhp->msg->sadb_msg_satype)) == 0) {
		ipseclog((LOG_DEBUG, "%s: invalid satype is passed.\n",
			__func__));
		return key_senderror(so, m, EINVAL);
	}

	/* no SATYPE specified, i.e. flushing all SA. */
	SAHTREE_LOCK();
	for (sah = LIST_FIRST(&V_sahtree);
	     sah != NULL;
	     sah = nextsah) {
		nextsah = LIST_NEXT(sah, chain);

		if (mhp->msg->sadb_msg_satype != SADB_SATYPE_UNSPEC
		 && proto != sah->saidx.proto)
			continue;

		for (stateidx = 0;
		     stateidx < _ARRAYLEN(saorder_state_alive);
		     stateidx++) {
			state = saorder_state_any[stateidx];
			for (sav = LIST_FIRST(&sah->savtree[state]);
			     sav != NULL;
			     sav = nextsav) {

				nextsav = LIST_NEXT(sav, chain);

				key_sa_chgstate(sav, SADB_SASTATE_DEAD);
				KEY_FREESAV(&sav);
			}
		}

		sah->state = SADB_SASTATE_DEAD;
	}
	SAHTREE_UNLOCK();

	if (m->m_len < sizeof(struct sadb_msg) ||
	    sizeof(struct sadb_msg) > m->m_len + M_TRAILINGSPACE(m)) {
		ipseclog((LOG_DEBUG, "%s: No more memory.\n", __func__));
		return key_senderror(so, m, ENOBUFS);
	}

	if (m->m_next)
		m_freem(m->m_next);
	m->m_next = NULL;
	m->m_pkthdr.len = m->m_len = sizeof(struct sadb_msg);
	newmsg = mtod(m, struct sadb_msg *);
	newmsg->sadb_msg_errno = 0;
	newmsg->sadb_msg_len = PFKEY_UNIT64(m->m_pkthdr.len);

	return key_sendup_mbuf(so, m, KEY_SENDUP_ALL);
}

/*
 * SADB_DUMP processing
 * dump all entries including status of DEAD in SAD.
 * receive
 *   <base>
 * from the ikmpd, and dump all secasvar leaves
 * and send,
 *   <base> .....
 * to the ikmpd.
 *
 * m will always be freed.
 */
static int
key_dump(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	struct secashead *sah;
	struct secasvar *sav;
	u_int16_t proto;
	u_int stateidx;
	u_int8_t satype;
	u_int8_t state;
	int cnt;
	struct sadb_msg *newmsg;
	struct mbuf *n;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	/* map satype to proto */
	if ((proto = key_satype2proto(mhp->msg->sadb_msg_satype)) == 0) {
		ipseclog((LOG_DEBUG, "%s: invalid satype is passed.\n",
			__func__));
		return key_senderror(so, m, EINVAL);
	}

	/* count sav entries to be sent to the userland. */
	cnt = 0;
	SAHTREE_LOCK();
	LIST_FOREACH(sah, &V_sahtree, chain) {
		if (mhp->msg->sadb_msg_satype != SADB_SATYPE_UNSPEC
		 && proto != sah->saidx.proto)
			continue;

		for (stateidx = 0;
		     stateidx < _ARRAYLEN(saorder_state_any);
		     stateidx++) {
			state = saorder_state_any[stateidx];
			LIST_FOREACH(sav, &sah->savtree[state], chain) {
				cnt++;
			}
		}
	}

	if (cnt == 0) {
		SAHTREE_UNLOCK();
		return key_senderror(so, m, ENOENT);
	}

	/* send this to the userland, one at a time. */
	newmsg = NULL;
	LIST_FOREACH(sah, &V_sahtree, chain) {
		if (mhp->msg->sadb_msg_satype != SADB_SATYPE_UNSPEC
		 && proto != sah->saidx.proto)
			continue;

		/* map proto to satype */
		if ((satype = key_proto2satype(sah->saidx.proto)) == 0) {
			SAHTREE_UNLOCK();
			ipseclog((LOG_DEBUG, "%s: there was invalid proto in "
				"SAD.\n", __func__));
			return key_senderror(so, m, EINVAL);
		}

		for (stateidx = 0;
		     stateidx < _ARRAYLEN(saorder_state_any);
		     stateidx++) {
			state = saorder_state_any[stateidx];
			LIST_FOREACH(sav, &sah->savtree[state], chain) {
				n = key_setdumpsa(sav, SADB_DUMP, satype,
				    --cnt, mhp->msg->sadb_msg_pid);
				if (!n) {
					SAHTREE_UNLOCK();
					return key_senderror(so, m, ENOBUFS);
				}
				key_sendup_mbuf(so, n, KEY_SENDUP_ONE);
			}
		}
	}
	SAHTREE_UNLOCK();

	m_freem(m);
	return 0;
}

/*
 * SADB_X_PROMISC processing
 *
 * m will always be freed.
 */
static int
key_promisc(struct socket *so, struct mbuf *m, const struct sadb_msghdr *mhp)
{
	int olen;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(mhp->msg != NULL, ("null msg"));

	olen = PFKEY_UNUNIT64(mhp->msg->sadb_msg_len);

	if (olen < sizeof(struct sadb_msg)) {
#if 1
		return key_senderror(so, m, EINVAL);
#else
		m_freem(m);
		return 0;
#endif
	} else if (olen == sizeof(struct sadb_msg)) {
		/* enable/disable promisc mode */
		struct keycb *kp;

		if ((kp = (struct keycb *)sotorawcb(so)) == NULL)
			return key_senderror(so, m, EINVAL);
		mhp->msg->sadb_msg_errno = 0;
		switch (mhp->msg->sadb_msg_satype) {
		case 0:
		case 1:
			kp->kp_promisc = mhp->msg->sadb_msg_satype;
			break;
		default:
			return key_senderror(so, m, EINVAL);
		}

		/* send the original message back to everyone */
		mhp->msg->sadb_msg_errno = 0;
		return key_sendup_mbuf(so, m, KEY_SENDUP_ALL);
	} else {
		/* send packet as is */

		m_adj(m, PFKEY_ALIGN8(sizeof(struct sadb_msg)));

		/* TODO: if sadb_msg_seq is specified, send to specific pid */
		return key_sendup_mbuf(so, m, KEY_SENDUP_ALL);
	}
}

static int (*key_typesw[])(struct socket *, struct mbuf *,
		const struct sadb_msghdr *) = {
	NULL,		/* SADB_RESERVED */
	key_getspi,	/* SADB_GETSPI */
	key_update,	/* SADB_UPDATE */
	key_add,	/* SADB_ADD */
	key_delete,	/* SADB_DELETE */
	key_get,	/* SADB_GET */
	key_acquire2,	/* SADB_ACQUIRE */
	key_register,	/* SADB_REGISTER */
	NULL,		/* SADB_EXPIRE */
	key_flush,	/* SADB_FLUSH */
	key_dump,	/* SADB_DUMP */
	key_promisc,	/* SADB_X_PROMISC */
	NULL,		/* SADB_X_PCHANGE */
	key_spdadd,	/* SADB_X_SPDUPDATE */
	key_spdadd,	/* SADB_X_SPDADD */
	key_spddelete,	/* SADB_X_SPDDELETE */
	key_spdget,	/* SADB_X_SPDGET */
	NULL,		/* SADB_X_SPDACQUIRE */
	key_spddump,	/* SADB_X_SPDDUMP */
	key_spdflush,	/* SADB_X_SPDFLUSH */
	key_spdadd,	/* SADB_X_SPDSETIDX */
	NULL,		/* SADB_X_SPDEXPIRE */
	key_spddelete2,	/* SADB_X_SPDDELETE2 */
};

/*
 * parse sadb_msg buffer to process PFKEYv2,
 * and create a data to response if needed.
 * I think to be dealed with mbuf directly.
 * IN:
 *     msgp  : pointer to pointer to a received buffer pulluped.
 *             This is rewrited to response.
 *     so    : pointer to socket.
 * OUT:
 *    length for buffer to send to user process.
 */
int
key_parse(struct mbuf *m, struct socket *so)
{
	struct sadb_msg *msg;
	struct sadb_msghdr mh;
	u_int orglen;
	int error;
	int target;

	IPSEC_ASSERT(so != NULL, ("null socket"));
	IPSEC_ASSERT(m != NULL, ("null mbuf"));

#if 0	/*kdebug_sadb assumes msg in linear buffer*/
	KEYDEBUG(KEYDEBUG_KEY_DUMP,
		ipseclog((LOG_DEBUG, "%s: passed sadb_msg\n", __func__));
		kdebug_sadb(msg));
#endif

	if (m->m_len < sizeof(struct sadb_msg)) {
		m = m_pullup(m, sizeof(struct sadb_msg));
		if (!m)
			return ENOBUFS;
	}
	msg = mtod(m, struct sadb_msg *);
	orglen = PFKEY_UNUNIT64(msg->sadb_msg_len);
	target = KEY_SENDUP_ONE;

	if ((m->m_flags & M_PKTHDR) == 0 || m->m_pkthdr.len != orglen) {
		ipseclog((LOG_DEBUG, "%s: invalid message length.\n",__func__));
		PFKEYSTAT_INC(out_invlen);
		error = EINVAL;
		goto senderror;
	}

	if (msg->sadb_msg_version != PF_KEY_V2) {
		ipseclog((LOG_DEBUG, "%s: PF_KEY version %u is mismatched.\n",
		    __func__, msg->sadb_msg_version));
		PFKEYSTAT_INC(out_invver);
		error = EINVAL;
		goto senderror;
	}

	if (msg->sadb_msg_type > SADB_MAX) {
		ipseclog((LOG_DEBUG, "%s: invalid type %u is passed.\n",
		    __func__, msg->sadb_msg_type));
		PFKEYSTAT_INC(out_invmsgtype);
		error = EINVAL;
		goto senderror;
	}

	/* for old-fashioned code - should be nuked */
	if (m->m_pkthdr.len > MCLBYTES) {
		m_freem(m);
		return ENOBUFS;
	}
	if (m->m_next) {
		struct mbuf *n;

		MGETHDR(n, M_NOWAIT, MT_DATA);
		if (n && m->m_pkthdr.len > MHLEN) {
			if (!(MCLGET(n, M_NOWAIT))) {
				m_free(n);
				n = NULL;
			}
		}
		if (!n) {
			m_freem(m);
			return ENOBUFS;
		}
		m_copydata(m, 0, m->m_pkthdr.len, mtod(n, caddr_t));
		n->m_pkthdr.len = n->m_len = m->m_pkthdr.len;
		n->m_next = NULL;
		m_freem(m);
		m = n;
	}

	/* align the mbuf chain so that extensions are in contiguous region. */
	error = key_align(m, &mh);
	if (error)
		return error;

	msg = mh.msg;

	/* check SA type */
	switch (msg->sadb_msg_satype) {
	case SADB_SATYPE_UNSPEC:
		switch (msg->sadb_msg_type) {
		case SADB_GETSPI:
		case SADB_UPDATE:
		case SADB_ADD:
		case SADB_DELETE:
		case SADB_GET:
		case SADB_ACQUIRE:
		case SADB_EXPIRE:
			ipseclog((LOG_DEBUG, "%s: must specify satype "
			    "when msg type=%u.\n", __func__,
			    msg->sadb_msg_type));
			PFKEYSTAT_INC(out_invsatype);
			error = EINVAL;
			goto senderror;
		}
		break;
	case SADB_SATYPE_AH:
	case SADB_SATYPE_ESP:
	case SADB_X_SATYPE_IPCOMP:
	case SADB_X_SATYPE_TCPSIGNATURE:
		switch (msg->sadb_msg_type) {
		case SADB_X_SPDADD:
		case SADB_X_SPDDELETE:
		case SADB_X_SPDGET:
		case SADB_X_SPDDUMP:
		case SADB_X_SPDFLUSH:
		case SADB_X_SPDSETIDX:
		case SADB_X_SPDUPDATE:
		case SADB_X_SPDDELETE2:
			ipseclog((LOG_DEBUG, "%s: illegal satype=%u\n",
				__func__, msg->sadb_msg_type));
			PFKEYSTAT_INC(out_invsatype);
			error = EINVAL;
			goto senderror;
		}
		break;
	case SADB_SATYPE_RSVP:
	case SADB_SATYPE_OSPFV2:
	case SADB_SATYPE_RIPV2:
	case SADB_SATYPE_MIP:
		ipseclog((LOG_DEBUG, "%s: type %u isn't supported.\n",
			__func__, msg->sadb_msg_satype));
		PFKEYSTAT_INC(out_invsatype);
		error = EOPNOTSUPP;
		goto senderror;
	case 1:	/* XXX: What does it do? */
		if (msg->sadb_msg_type == SADB_X_PROMISC)
			break;
		/*FALLTHROUGH*/
	default:
		ipseclog((LOG_DEBUG, "%s: invalid type %u is passed.\n",
			__func__, msg->sadb_msg_satype));
		PFKEYSTAT_INC(out_invsatype);
		error = EINVAL;
		goto senderror;
	}

	/* check field of upper layer protocol and address family */
	if (mh.ext[SADB_EXT_ADDRESS_SRC] != NULL
	 && mh.ext[SADB_EXT_ADDRESS_DST] != NULL) {
		struct sadb_address *src0, *dst0;
		u_int plen;

		src0 = (struct sadb_address *)(mh.ext[SADB_EXT_ADDRESS_SRC]);
		dst0 = (struct sadb_address *)(mh.ext[SADB_EXT_ADDRESS_DST]);

		/* check upper layer protocol */
		if (src0->sadb_address_proto != dst0->sadb_address_proto) {
			ipseclog((LOG_DEBUG, "%s: upper layer protocol "
				"mismatched.\n", __func__));
			PFKEYSTAT_INC(out_invaddr);
			error = EINVAL;
			goto senderror;
		}

		/* check family */
		if (PFKEY_ADDR_SADDR(src0)->sa_family !=
		    PFKEY_ADDR_SADDR(dst0)->sa_family) {
			ipseclog((LOG_DEBUG, "%s: address family mismatched.\n",
				__func__));
			PFKEYSTAT_INC(out_invaddr);
			error = EINVAL;
			goto senderror;
		}
		if (PFKEY_ADDR_SADDR(src0)->sa_len !=
		    PFKEY_ADDR_SADDR(dst0)->sa_len) {
			ipseclog((LOG_DEBUG, "%s: address struct size "
				"mismatched.\n", __func__));
			PFKEYSTAT_INC(out_invaddr);
			error = EINVAL;
			goto senderror;
		}

		switch (PFKEY_ADDR_SADDR(src0)->sa_family) {
		case AF_INET:
			if (PFKEY_ADDR_SADDR(src0)->sa_len !=
			    sizeof(struct sockaddr_in)) {
				PFKEYSTAT_INC(out_invaddr);
				error = EINVAL;
				goto senderror;
			}
			break;
		case AF_INET6:
			if (PFKEY_ADDR_SADDR(src0)->sa_len !=
			    sizeof(struct sockaddr_in6)) {
				PFKEYSTAT_INC(out_invaddr);
				error = EINVAL;
				goto senderror;
			}
			break;
		default:
			ipseclog((LOG_DEBUG, "%s: unsupported address family\n",
				__func__));
			PFKEYSTAT_INC(out_invaddr);
			error = EAFNOSUPPORT;
			goto senderror;
		}

		switch (PFKEY_ADDR_SADDR(src0)->sa_family) {
		case AF_INET:
			plen = sizeof(struct in_addr) << 3;
			break;
		case AF_INET6:
			plen = sizeof(struct in6_addr) << 3;
			break;
		default:
			plen = 0;	/*fool gcc*/
			break;
		}

		/* check max prefix length */
		if (src0->sadb_address_prefixlen > plen ||
		    dst0->sadb_address_prefixlen > plen) {
			ipseclog((LOG_DEBUG, "%s: illegal prefixlen.\n",
				__func__));
			PFKEYSTAT_INC(out_invaddr);
			error = EINVAL;
			goto senderror;
		}

		/*
		 * prefixlen == 0 is valid because there can be a case when
		 * all addresses are matched.
		 */
	}

	if (msg->sadb_msg_type >= nitems(key_typesw) ||
	    key_typesw[msg->sadb_msg_type] == NULL) {
		PFKEYSTAT_INC(out_invmsgtype);
		error = EINVAL;
		goto senderror;
	}

	return (*key_typesw[msg->sadb_msg_type])(so, m, &mh);

senderror:
	msg->sadb_msg_errno = error;
	return key_sendup_mbuf(so, m, target);
}

static int
key_senderror(struct socket *so, struct mbuf *m, int code)
{
	struct sadb_msg *msg;

	IPSEC_ASSERT(m->m_len >= sizeof(struct sadb_msg),
		("mbuf too small, len %u", m->m_len));

	msg = mtod(m, struct sadb_msg *);
	msg->sadb_msg_errno = code;
	return key_sendup_mbuf(so, m, KEY_SENDUP_ONE);
}

/*
 * set the pointer to each header into message buffer.
 * m will be freed on error.
 * XXX larger-than-MCLBYTES extension?
 */
static int
key_align(struct mbuf *m, struct sadb_msghdr *mhp)
{
	struct mbuf *n;
	struct sadb_ext *ext;
	size_t off, end;
	int extlen;
	int toff;

	IPSEC_ASSERT(m != NULL, ("null mbuf"));
	IPSEC_ASSERT(mhp != NULL, ("null msghdr"));
	IPSEC_ASSERT(m->m_len >= sizeof(struct sadb_msg),
		("mbuf too small, len %u", m->m_len));

	/* initialize */
	bzero(mhp, sizeof(*mhp));

	mhp->msg = mtod(m, struct sadb_msg *);
	mhp->ext[0] = (struct sadb_ext *)mhp->msg;	/*XXX backward compat */

	end = PFKEY_UNUNIT64(mhp->msg->sadb_msg_len);
	extlen = end;	/*just in case extlen is not updated*/
	for (off = sizeof(struct sadb_msg); off < end; off += extlen) {
		n = m_pulldown(m, off, sizeof(struct sadb_ext), &toff);
		if (!n) {
			/* m is already freed */
			return ENOBUFS;
		}
		ext = (struct sadb_ext *)(mtod(n, caddr_t) + toff);

		/* set pointer */
		switch (ext->sadb_ext_type) {
		case SADB_EXT_SA:
		case SADB_EXT_ADDRESS_SRC:
		case SADB_EXT_ADDRESS_DST:
		case SADB_EXT_ADDRESS_PROXY:
		case SADB_EXT_LIFETIME_CURRENT:
		case SADB_EXT_LIFETIME_HARD:
		case SADB_EXT_LIFETIME_SOFT:
		case SADB_EXT_KEY_AUTH:
		case SADB_EXT_KEY_ENCRYPT:
		case SADB_EXT_IDENTITY_SRC:
		case SADB_EXT_IDENTITY_DST:
		case SADB_EXT_SENSITIVITY:
		case SADB_EXT_PROPOSAL:
		case SADB_EXT_SUPPORTED_AUTH:
		case SADB_EXT_SUPPORTED_ENCRYPT:
		case SADB_EXT_SPIRANGE:
		case SADB_X_EXT_POLICY:
		case SADB_X_EXT_SA2:
#ifdef IPSEC_NAT_T
		case SADB_X_EXT_NAT_T_TYPE:
		case SADB_X_EXT_NAT_T_SPORT:
		case SADB_X_EXT_NAT_T_DPORT:
		case SADB_X_EXT_NAT_T_OAI:
		case SADB_X_EXT_NAT_T_OAR:
		case SADB_X_EXT_NAT_T_FRAG:
#endif
			/* duplicate check */
			/*
			 * XXX Are there duplication payloads of either
			 * KEY_AUTH or KEY_ENCRYPT ?
			 */
			if (mhp->ext[ext->sadb_ext_type] != NULL) {
				ipseclog((LOG_DEBUG, "%s: duplicate ext_type "
					"%u\n", __func__, ext->sadb_ext_type));
				m_freem(m);
				PFKEYSTAT_INC(out_dupext);
				return EINVAL;
			}
			break;
		default:
			ipseclog((LOG_DEBUG, "%s: invalid ext_type %u\n",
				__func__, ext->sadb_ext_type));
			m_freem(m);
			PFKEYSTAT_INC(out_invexttype);
			return EINVAL;
		}

		extlen = PFKEY_UNUNIT64(ext->sadb_ext_len);

		if (key_validate_ext(ext, extlen)) {
			m_freem(m);
			PFKEYSTAT_INC(out_invlen);
			return EINVAL;
		}

		n = m_pulldown(m, off, extlen, &toff);
		if (!n) {
			/* m is already freed */
			return ENOBUFS;
		}
		ext = (struct sadb_ext *)(mtod(n, caddr_t) + toff);

		mhp->ext[ext->sadb_ext_type] = ext;
		mhp->extoff[ext->sadb_ext_type] = off;
		mhp->extlen[ext->sadb_ext_type] = extlen;
	}

	if (off != end) {
		m_freem(m);
		PFKEYSTAT_INC(out_invlen);
		return EINVAL;
	}

	return 0;
}

static int
key_validate_ext(const struct sadb_ext *ext, int len)
{
	const struct sockaddr *sa;
	enum { NONE, ADDR } checktype = NONE;
	int baselen = 0;
	const int sal = offsetof(struct sockaddr, sa_len) + sizeof(sa->sa_len);

	if (len != PFKEY_UNUNIT64(ext->sadb_ext_len))
		return EINVAL;

	/* if it does not match minimum/maximum length, bail */
	if (ext->sadb_ext_type >= nitems(minsize) ||
	    ext->sadb_ext_type >= nitems(maxsize))
		return EINVAL;
	if (!minsize[ext->sadb_ext_type] || len < minsize[ext->sadb_ext_type])
		return EINVAL;
	if (maxsize[ext->sadb_ext_type] && len > maxsize[ext->sadb_ext_type])
		return EINVAL;

	/* more checks based on sadb_ext_type XXX need more */
	switch (ext->sadb_ext_type) {
	case SADB_EXT_ADDRESS_SRC:
	case SADB_EXT_ADDRESS_DST:
	case SADB_EXT_ADDRESS_PROXY:
		baselen = PFKEY_ALIGN8(sizeof(struct sadb_address));
		checktype = ADDR;
		break;
	case SADB_EXT_IDENTITY_SRC:
	case SADB_EXT_IDENTITY_DST:
		if (((const struct sadb_ident *)ext)->sadb_ident_type ==
		    SADB_X_IDENTTYPE_ADDR) {
			baselen = PFKEY_ALIGN8(sizeof(struct sadb_ident));
			checktype = ADDR;
		} else
			checktype = NONE;
		break;
	default:
		checktype = NONE;
		break;
	}

	switch (checktype) {
	case NONE:
		break;
	case ADDR:
		sa = (const struct sockaddr *)(((const u_int8_t*)ext)+baselen);
		if (len < baselen + sal)
			return EINVAL;
		if (baselen + PFKEY_ALIGN8(sa->sa_len) != len)
			return EINVAL;
		break;
	}

	return 0;
}

void
key_init(void)
{
	int i;

	for (i = 0; i < IPSEC_DIR_MAX; i++)
		TAILQ_INIT(&V_sptree[i]);

	V_key_lft_zone = uma_zcreate("IPsec SA lft_c",
	    sizeof(uint64_t) * 2, NULL, NULL, NULL, NULL,
	    UMA_ALIGN_PTR, UMA_ZONE_PCPU);

	LIST_INIT(&V_sahtree);
	V_sphashtbl = hashinit(SPHASH_NHASH, M_IPSEC_SP, &V_sphash_mask);
	V_savhashtbl = hashinit(SAVHASH_NHASH, M_IPSEC_SA, &V_savhash_mask);
	V_sahaddrhashtbl = hashinit(SAHHASH_NHASH, M_IPSEC_SAH,
	    &V_sahaddrhash_mask);

	for (i = 0; i <= SADB_SATYPE_MAX; i++)
		LIST_INIT(&V_regtree[i]);

	LIST_INIT(&V_acqtree);
	LIST_INIT(&V_spacqtree);

	if (!IS_DEFAULT_VNET(curvnet))
		return;

	SPTREE_LOCK_INIT();
	REGTREE_LOCK_INIT();
	SAHTREE_LOCK_INIT();
	ACQ_LOCK_INIT();
	SPACQ_LOCK_INIT();

#ifndef IPSEC_DEBUG2
	callout_init(&key_timer, 1);
	callout_reset(&key_timer, hz, key_timehandler, NULL);
#endif /*IPSEC_DEBUG2*/

	/* initialize key statistics */
	keystat.getspi_count = 1;

	if (bootverbose)
		printf("IPsec: Initialized Security Association Processing.\n");
}

#ifdef VIMAGE
void
key_destroy(void)
{
	struct secpolicy_queue drainq;
	struct secpolicy *sp, *nextsp;
	struct secacq *acq, *nextacq;
	struct secspacq *spacq, *nextspacq;
	struct secashead *sah, *nextsah;
	struct secreg *reg;
	int i;

	TAILQ_INIT(&drainq);
	SPTREE_WLOCK();
	for (i = 0; i < IPSEC_DIR_MAX; i++) {
		TAILQ_CONCAT(&drainq, &V_sptree[i], chain);
	}
	SPTREE_WUNLOCK();
	sp = TAILQ_FIRST(&drainq);
	while (sp != NULL) {
		nextsp = TAILQ_NEXT(sp, chain);
		KEY_FREESP(&sp);
		sp = nextsp;
	}

	SAHTREE_LOCK();
	for (sah = LIST_FIRST(&V_sahtree); sah != NULL; sah = nextsah) {
		nextsah = LIST_NEXT(sah, chain);
		if (__LIST_CHAINED(sah)) {
			LIST_REMOVE(sah, chain);
			free(sah, M_IPSEC_SAH);
		}
	}
	SAHTREE_UNLOCK();

	hashdestroy(V_sphashtbl, M_IPSEC_SP, V_sphash_mask);
	hashdestroy(V_savhashtbl, M_IPSEC_SA, V_savhash_mask);
	hashdestroy(V_sahaddrhashtbl, M_IPSEC_SAH, V_sahaddrhash_mask);

	REGTREE_LOCK();
	for (i = 0; i <= SADB_SATYPE_MAX; i++) {
		LIST_FOREACH(reg, &V_regtree[i], chain) {
			if (__LIST_CHAINED(reg)) {
				LIST_REMOVE(reg, chain);
				free(reg, M_IPSEC_SAR);
				break;
			}
		}
	}
	REGTREE_UNLOCK();

	ACQ_LOCK();
	for (acq = LIST_FIRST(&V_acqtree); acq != NULL; acq = nextacq) {
		nextacq = LIST_NEXT(acq, chain);
		if (__LIST_CHAINED(acq)) {
			LIST_REMOVE(acq, chain);
			free(acq, M_IPSEC_SAQ);
		}
	}
	ACQ_UNLOCK();

	SPACQ_LOCK();
	for (spacq = LIST_FIRST(&V_spacqtree); spacq != NULL;
	    spacq = nextspacq) {
		nextspacq = LIST_NEXT(spacq, chain);
		if (__LIST_CHAINED(spacq)) {
			LIST_REMOVE(spacq, chain);
			free(spacq, M_IPSEC_SAQ);
		}
	}
	SPACQ_UNLOCK();
	uma_zdestroy(V_key_lft_zone);
}
#endif

/*
 * XXX: maybe This function is called after INBOUND IPsec processing.
 *
 * Special check for tunnel-mode packets.
 * We must make some checks for consistency between inner and outer IP header.
 *
 * xxx more checks to be provided
 */
int
key_checktunnelsanity(struct secasvar *sav, u_int family, caddr_t src,
    caddr_t dst)
{
	IPSEC_ASSERT(sav->sah != NULL, ("null SA header"));

	/* XXX: check inner IP header */

	return 1;
}

/* record data transfer on SA, and update timestamps */
void
key_sa_recordxfer(struct secasvar *sav, struct mbuf *m)
{
	IPSEC_ASSERT(sav != NULL, ("Null secasvar"));
	IPSEC_ASSERT(m != NULL, ("Null mbuf"));
	if (!sav->lft_c)
		return;

	/*
	 * XXX Currently, there is a difference of bytes size
	 * between inbound and outbound processing.
	 */
	sav->lft_c->bytes += m->m_pkthdr.len;
	/* to check bytes lifetime is done in key_timehandler(). */

	/*
	 * We use the number of packets as the unit of
	 * allocations.  We increment the variable
	 * whenever {esp,ah}_{in,out}put is called.
	 */
	sav->lft_c->allocations++;
	/* XXX check for expires? */

	/*
	 * NOTE: We record CURRENT usetime by using wall clock,
	 * in seconds.  HARD and SOFT lifetime are measured by the time
	 * difference (again in seconds) from usetime.
	 *
	 *	usetime
	 *	v     expire   expire
	 * -----+-----+--------+---> t
	 *	<--------------> HARD
	 *	<-----> SOFT
	 */
	sav->lft_c->usetime = time_second;
	/* XXX check for expires? */

	return;
}

static void
key_sa_chgstate(struct secasvar *sav, u_int8_t state)
{
	IPSEC_ASSERT(sav != NULL, ("NULL sav"));
	SAHTREE_LOCK_ASSERT();

	if (sav->state != state) {
		if (__LIST_CHAINED(sav))
			LIST_REMOVE(sav, chain);
		sav->state = state;
		LIST_INSERT_HEAD(&sav->sah->savtree[state], sav, chain);
	}
}

/*
 * Take one of the kernel's security keys and convert it into a PF_KEY
 * structure within an mbuf, suitable for sending up to a waiting
 * application in user land.
 * 
 * IN: 
 *    src: A pointer to a kernel security key.
 *    exttype: Which type of key this is. Refer to the PF_KEY data structures.
 * OUT:
 *    a valid mbuf or NULL indicating an error
 *
 */

static struct mbuf *
key_setkey(struct seckey *src, u_int16_t exttype) 
{
	struct mbuf *m;
	struct sadb_key *p;
	int len;

	if (src == NULL)
		return NULL;

	len = PFKEY_ALIGN8(sizeof(struct sadb_key) + _KEYLEN(src));
	m = m_get2(len, M_NOWAIT, MT_DATA, 0);
	if (m == NULL)
		return NULL;
	m_align(m, len);
	m->m_len = len;
	p = mtod(m, struct sadb_key *);
	bzero(p, len);
	p->sadb_key_len = PFKEY_UNIT64(len);
	p->sadb_key_exttype = exttype;
	p->sadb_key_bits = src->bits;
	bcopy(src->key_data, _KEYBUF(p), _KEYLEN(src));

	return m;
}

/*
 * Take one of the kernel's lifetime data structures and convert it
 * into a PF_KEY structure within an mbuf, suitable for sending up to
 * a waiting application in user land.
 * 
 * IN: 
 *    src: A pointer to a kernel lifetime structure.
 *    exttype: Which type of lifetime this is. Refer to the PF_KEY 
 *             data structures for more information.
 * OUT:
 *    a valid mbuf or NULL indicating an error
 *
 */

static struct mbuf *
key_setlifetime(struct seclifetime *src, u_int16_t exttype)
{
	struct mbuf *m = NULL;
	struct sadb_lifetime *p;
	int len = PFKEY_ALIGN8(sizeof(struct sadb_lifetime));

	if (src == NULL)
		return NULL;

	m = m_get2(len, M_NOWAIT, MT_DATA, 0);
	if (m == NULL)
		return m;
	m_align(m, len);
	m->m_len = len;
	p = mtod(m, struct sadb_lifetime *);

	bzero(p, len);
	p->sadb_lifetime_len = PFKEY_UNIT64(len);
	p->sadb_lifetime_exttype = exttype;
	p->sadb_lifetime_allocations = src->allocations;
	p->sadb_lifetime_bytes = src->bytes;
	p->sadb_lifetime_addtime = src->addtime;
	p->sadb_lifetime_usetime = src->usetime;
	
	return m;

}
