/*-
 * Copyright (c) 2006 The FreeBSD Project.
 * Copyright (c) 2015 Andrey V. Elsukov <ae@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
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
 *
 * $FreeBSD$
 */

#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_enc.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <machine/bus.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_clone.h>
#include <net/if_types.h>
#include <net/pfil.h>
#include <net/route.h>
#include <net/netisr.h>
#include <net/bpf.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/in_var.h>

#ifdef INET6
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#endif

#include <netipsec/ipsec.h>
#include <netipsec/xform.h>

#define ENCMTU		(1024+512)

/* XXX this define must have the same value as in OpenBSD */
#define M_CONF		0x0400	/* payload was encrypted (ESP-transport) */
#define M_AUTH		0x0800	/* payload was authenticated (AH or ESP auth) */
#define M_AUTH_AH	0x2000	/* header was authenticated (AH) */

struct enchdr {
	u_int32_t af;
	u_int32_t spi;
	u_int32_t flags;
};
struct enc_softc {
	struct	ifnet *sc_ifp;
};
static VNET_DEFINE(struct enc_softc *, enc_sc);
#define	V_enc_sc	VNET(enc_sc)
static VNET_DEFINE(struct if_clone *, enc_cloner);
#define	V_enc_cloner	VNET(enc_cloner)

static int	enc_ioctl(struct ifnet *, u_long, caddr_t);
static int	enc_output(struct ifnet *, struct mbuf *,
    const struct sockaddr *, struct route *);
static int	enc_clone_create(struct if_clone *, int, caddr_t);
static void	enc_clone_destroy(struct ifnet *);
static int	enc_add_hhooks(struct enc_softc *);
static void	enc_remove_hhooks(struct enc_softc *);

static const char encname[] = "enc";

/*
 * Before and after are relative to when we are stripping the
 * outer IP header.
 */
static VNET_DEFINE(int, filter_mask_in) = IPSEC_ENC_BEFORE;
static VNET_DEFINE(int, bpf_mask_in) = IPSEC_ENC_BEFORE;
static VNET_DEFINE(int, filter_mask_out) = IPSEC_ENC_BEFORE;
static VNET_DEFINE(int, bpf_mask_out) = IPSEC_ENC_BEFORE | IPSEC_ENC_AFTER;
#define	V_filter_mask_in	VNET(filter_mask_in)
#define	V_bpf_mask_in		VNET(bpf_mask_in)
#define	V_filter_mask_out	VNET(filter_mask_out)
#define	V_bpf_mask_out		VNET(bpf_mask_out)

static SYSCTL_NODE(_net, OID_AUTO, enc, CTLFLAG_RW, 0, "enc sysctl");
static SYSCTL_NODE(_net_enc, OID_AUTO, in, CTLFLAG_RW, 0, "enc input sysctl");
static SYSCTL_NODE(_net_enc, OID_AUTO, out, CTLFLAG_RW, 0, "enc output sysctl");
SYSCTL_INT(_net_enc_in, OID_AUTO, ipsec_filter_mask,
    CTLFLAG_RW | CTLFLAG_VNET, &VNET_NAME(filter_mask_in), 0,
    "IPsec input firewall filter mask");
SYSCTL_INT(_net_enc_in, OID_AUTO, ipsec_bpf_mask,
    CTLFLAG_RW | CTLFLAG_VNET, &VNET_NAME(bpf_mask_in), 0,
    "IPsec input bpf mask");
SYSCTL_INT(_net_enc_out, OID_AUTO, ipsec_filter_mask,
    CTLFLAG_RW | CTLFLAG_VNET, &VNET_NAME(filter_mask_out), 0,
    "IPsec output firewall filter mask");
SYSCTL_INT(_net_enc_out, OID_AUTO, ipsec_bpf_mask,
    CTLFLAG_RW | CTLFLAG_VNET, &VNET_NAME(bpf_mask_out), 0,
    "IPsec output bpf mask");

static void
enc_clone_destroy(struct ifnet *ifp)
{
	struct enc_softc *sc;

	sc = ifp->if_softc;
	KASSERT(sc == V_enc_sc, ("sc != ifp->if_softc"));

	enc_remove_hhooks(sc);
	bpfdetach(ifp);
	if_detach(ifp);
	if_free(ifp);
	free(sc, M_DEVBUF);
	V_enc_sc = NULL;
}

static int
enc_clone_create(struct if_clone *ifc, int unit, caddr_t params)
{
	struct ifnet *ifp;
	struct enc_softc *sc;

	sc = malloc(sizeof(struct enc_softc), M_DEVBUF,
	    M_WAITOK | M_ZERO);
	ifp = sc->sc_ifp = if_alloc(IFT_ENC);
	if (ifp == NULL) {
		free(sc, M_DEVBUF);
		return (ENOSPC);
	}
	if (V_enc_sc != NULL) {
		if_free(ifp);
		free(sc, M_DEVBUF);
		return (EEXIST);
	}
	V_enc_sc = sc;
	if_initname(ifp, encname, unit);
	ifp->if_mtu = ENCMTU;
	ifp->if_ioctl = enc_ioctl;
	ifp->if_output = enc_output;
	ifp->if_softc = sc;
	if_attach(ifp);
	bpfattach(ifp, DLT_ENC, sizeof(struct enchdr));
	if (enc_add_hhooks(sc) != 0) {
		enc_clone_destroy(ifp);
		return (ENXIO);
	}
	return (0);
}

static int
enc_output(struct ifnet *ifp, struct mbuf *m, const struct sockaddr *dst,
    struct route *ro)
{

	m_freem(m);
	return (0);
}

static int
enc_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{

	if (cmd != SIOCSIFFLAGS)
		return (EINVAL);
	if (ifp->if_flags & IFF_UP)
		ifp->if_drv_flags |= IFF_DRV_RUNNING;
	else
		ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
	return (0);
}

/*
 * One helper hook function is used by any hook points.
 * + from hhook_type we can determine the packet direction:
 *   HHOOK_TYPE_IPSEC_IN or HHOOK_TYPE_IPSEC_OUT;
 * + from hhook_id we can determine address family: AF_INET or AF_INET6;
 * + udata contains pointer to enc_softc;
 * + ctx_data contains pointer to struct ipsec_ctx_data.
 */
static int
enc_hhook(int32_t hhook_type, int32_t hhook_id, void *udata, void *ctx_data,
    void *hdata, struct osd *hosd)
{
	struct enchdr hdr;
	struct ipsec_ctx_data *ctx;
	struct enc_softc *sc;
	struct ifnet *ifp, *rcvif;
	struct pfil_head *ph;
	int pdir;

	sc = (struct enc_softc *)udata;
	ifp = sc->sc_ifp;
	if ((ifp->if_flags & IFF_UP) == 0)
		return (0);

	ctx = (struct ipsec_ctx_data *)ctx_data;
	/* XXX: wrong hook point was used by caller? */
	if (ctx->af != hhook_id)
		return (EPFNOSUPPORT);

	if (((hhook_type == HHOOK_TYPE_IPSEC_IN &&
	    (ctx->enc & V_bpf_mask_in) != 0) ||
	    (hhook_type == HHOOK_TYPE_IPSEC_OUT &&
	    (ctx->enc & V_bpf_mask_out) != 0)) &&
	    bpf_peers_present(ifp->if_bpf) != 0) {
		hdr.af = ctx->af;
		hdr.spi = ctx->sav->spi;
		hdr.flags = 0;
		if (ctx->sav->alg_enc != SADB_EALG_NONE)
			hdr.flags |= M_CONF;
		if (ctx->sav->alg_auth != SADB_AALG_NONE)
			hdr.flags |= M_AUTH;
		bpf_mtap2(ifp->if_bpf, &hdr, sizeof(hdr), *ctx->mp);
	}

	switch (hhook_type) {
	case HHOOK_TYPE_IPSEC_IN:
		if (ctx->enc == IPSEC_ENC_BEFORE) {
			/* Do accounting only once */
			if_inc_counter(ifp, IFCOUNTER_IPACKETS, 1);
			if_inc_counter(ifp, IFCOUNTER_IBYTES,
			    (*ctx->mp)->m_pkthdr.len);
		}
		if ((ctx->enc & V_filter_mask_in) == 0)
			return (0); /* skip pfil processing */
		pdir = PFIL_IN;
		break;
	case HHOOK_TYPE_IPSEC_OUT:
		if (ctx->enc == IPSEC_ENC_BEFORE) {
			/* Do accounting only once */
			if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
			if_inc_counter(ifp, IFCOUNTER_OBYTES,
			    (*ctx->mp)->m_pkthdr.len);
		}
		if ((ctx->enc & V_filter_mask_out) == 0)
			return (0); /* skip pfil processing */
		pdir = PFIL_OUT;
		break;
	default:
		return (EINVAL);
	}

	switch (hhook_id) {
#ifdef INET
	case AF_INET:
		ph = &V_inet_pfil_hook;
		break;
#endif
#ifdef INET6
	case AF_INET6:
		ph = &V_inet6_pfil_hook;
		break;
#endif
	default:
		ph = NULL;
	}
	if (ph == NULL || !PFIL_HOOKED(ph))
		return (0);
	/* Make a packet looks like it was received on enc(4) */
	rcvif = (*ctx->mp)->m_pkthdr.rcvif;
	(*ctx->mp)->m_pkthdr.rcvif = ifp;
	if (pfil_run_hooks(ph, ctx->mp, ifp, pdir, NULL) != 0 ||
	    *ctx->mp == NULL) {
		*ctx->mp = NULL; /* consumed by filter */
		return (EACCES);
	}
	(*ctx->mp)->m_pkthdr.rcvif = rcvif;
	return (0);
}

static int
enc_add_hhooks(struct enc_softc *sc)
{
	struct hookinfo hki;
	int error;

	error = EPFNOSUPPORT;
	hki.hook_func = enc_hhook;
	hki.hook_helper = NULL;
	hki.hook_udata = sc;
#ifdef INET
	hki.hook_id = AF_INET;
	hki.hook_type = HHOOK_TYPE_IPSEC_IN;
	error = hhook_add_hook(V_ipsec_hhh_in[HHOOK_IPSEC_INET],
	    &hki, HHOOK_WAITOK);
	if (error != 0)
		return (error);
	hki.hook_type = HHOOK_TYPE_IPSEC_OUT;
	error = hhook_add_hook(V_ipsec_hhh_out[HHOOK_IPSEC_INET],
	    &hki, HHOOK_WAITOK);
	if (error != 0)
		return (error);
#endif
#ifdef INET6
	hki.hook_id = AF_INET6;
	hki.hook_type = HHOOK_TYPE_IPSEC_IN;
	error = hhook_add_hook(V_ipsec_hhh_in[HHOOK_IPSEC_INET6],
	    &hki, HHOOK_WAITOK);
	if (error != 0)
		return (error);
	hki.hook_type = HHOOK_TYPE_IPSEC_OUT;
	error = hhook_add_hook(V_ipsec_hhh_out[HHOOK_IPSEC_INET6],
	    &hki, HHOOK_WAITOK);
	if (error != 0)
		return (error);
#endif
	return (error);
}

static void
enc_remove_hhooks(struct enc_softc *sc)
{
	struct hookinfo hki;

	hki.hook_func = enc_hhook;
	hki.hook_helper = NULL;
	hki.hook_udata = sc;
#ifdef INET
	hki.hook_id = AF_INET;
	hki.hook_type = HHOOK_TYPE_IPSEC_IN;
	hhook_remove_hook(V_ipsec_hhh_in[HHOOK_IPSEC_INET], &hki);
	hki.hook_type = HHOOK_TYPE_IPSEC_OUT;
	hhook_remove_hook(V_ipsec_hhh_out[HHOOK_IPSEC_INET], &hki);
#endif
#ifdef INET6
	hki.hook_id = AF_INET6;
	hki.hook_type = HHOOK_TYPE_IPSEC_IN;
	hhook_remove_hook(V_ipsec_hhh_in[HHOOK_IPSEC_INET6], &hki);
	hki.hook_type = HHOOK_TYPE_IPSEC_OUT;
	hhook_remove_hook(V_ipsec_hhh_out[HHOOK_IPSEC_INET6], &hki);
#endif
}

static void
vnet_enc_init(const void *unused __unused)
{

	V_enc_sc = NULL;
	V_enc_cloner = if_clone_simple(encname, enc_clone_create,
	    enc_clone_destroy, 1);
}
VNET_SYSINIT(vnet_enc_init, SI_SUB_PROTO_IFATTACHDOMAIN, SI_ORDER_ANY,
    vnet_enc_init, NULL);

static void
vnet_enc_uninit(const void *unused __unused)
{

	if_clone_detach(V_enc_cloner);
}
VNET_SYSUNINIT(vnet_enc_uninit, SI_SUB_PROTO_IFATTACHDOMAIN, SI_ORDER_ANY,
    vnet_enc_uninit, NULL);

static int
enc_modevent(module_t mod, int type, void *data)
{

	switch (type) {
	case MOD_LOAD:
	case MOD_UNLOAD:
		break;
	default:
		return (EOPNOTSUPP);
	}
	return (0);
}

static moduledata_t enc_mod = {
	"if_enc",
	enc_modevent,
	0
};

DECLARE_MODULE(if_enc, enc_mod, SI_SUB_PROTO_IFATTACHDOMAIN, SI_ORDER_ANY);
