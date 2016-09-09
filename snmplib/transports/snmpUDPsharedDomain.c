/* UDPshared transport support functions
 */

#include <net-snmp/net-snmp-config.h>

#include <net-snmp/types.h>
#include <net-snmp/library/snmpUDPsharedDomain.h>

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <ctype.h>
#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#if HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#if HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#if HAVE_NETDB_H
#include <netdb.h>
#endif
#include <errno.h>

#include <net-snmp/types.h>
#include <net-snmp/library/snmp_debug.h>
#include <net-snmp/library/tools.h>
#include <net-snmp/library/snmp_assert.h>

#include <net-snmp/library/snmpSocketBaseDomain.h>
#include <net-snmp/library/snmpUDPDomain.h>
#include <net-snmp/library/snmpUDPIPv4BaseDomain.h>

netsnmp_feature_require(transport_cache)

oid    netsnmpUDPsharedDomain[] = { 1,3,6,1,2,1,100,1,999 }; /** made up */
size_t netsnmpUDPsharedDomain_len = OID_LENGTH(netsnmpUDPsharedDomain);

/* ***************************************************************************
 */
static int
_udpshared_close(netsnmp_transport *t)
{
    netsnmp_transport *b = t? t->base_transport : NULL;

    DEBUGMSGTL(("udpshared:close", "%p/%p\n", t,b ));

    /** we don't really have a socket - base transport does */
    if (NULL == t || NULL == b)
        return -1;

    t->base_transport = NULL;
    t->sock = -1; /* we just had a copy of base's fd */

    /** remove base transport. if it is still in use, return. */
    if (netsnmp_transport_cache_remove(b) == 1)
        DEBUGMSGTL(("udpshared:close", "keeping socket open for %d user(s)\n",
                    b->local_length));
    else
        b->f_close(b); /** close base transport */

    return 0;
}

static int
_udpshared_recv(netsnmp_transport *t, void *buf, int size,
                void **opaque, int *olength)
{
    if (NULL == t || NULL == t->base_transport)
        return -1;

    return t->base_transport->f_recv(t->base_transport, buf, size, opaque,
                                     olength);
}

static int
_udpshared_send(netsnmp_transport *t, void *buf, int size,
                void **opaque, int *olength)
{
    void *_opaque, **_opaque_p = &_opaque;
    int  _olength, *_olength_p = &_olength;

    if (NULL == t || NULL == t->base_transport)
        return -1;

    /*
     * opaque points to an address pair to use to send, overriding the
     * address pair in the transport. So if no address was specified by
     * the caller, use the udpshared transport address, overriding the
     * empty base_transport address.
     */
    if (NULL == opaque || NULL == *opaque) {
        _opaque = t->data;
        _olength = t->data_length;
    } else {
        _opaque_p = opaque;
        _olength_p = olength;
    }
    return t->base_transport->f_send(t->base_transport, buf, size, _opaque_p,
                                     _olength_p);
}

static char *
_udpshared_fmtaddr(netsnmp_transport *t, void *data, int len)
{
    if (NULL == t || NULL == t->base_transport)
        return NULL;

    return t->base_transport->f_fmtaddr(t->base_transport, data, len);
}

static int
_setup_session(netsnmp_transport *t, netsnmp_session *session)
{
    if (NULL == t || NULL == session)
        return -1;

    session->flags |= SNMP_FLAGS_SHARED_SOCKET;

    return 0;
}

static netsnmp_transport *
_transport_common(netsnmp_transport *t)
{
    void *save_data;
    int   save_data_len;

    DEBUGTRACETOK("9:udpshared");

    if (NULL == t)
        return NULL;

    /*
     * t->data contains remote addr, which can/will vary, so don't
     * copy it to base transport
     */
    save_data = t->data;
    save_data_len = t->data_length;
    t->data = NULL;
    t->data_length = 0;

    /** save base transport for clients; need in send/recv functions later */
    t->base_transport = netsnmp_transport_copy(t);
    if (NULL == t->base_transport) {
        free(save_data);
        netsnmp_transport_free(t);
        return NULL;
    }

    /** restore remote addr */
    t->data = save_data;
    t->data_length = save_data_len;

    /** Set UDPsharedDomain specifics */
    t->domain = netsnmpUDPsharedDomain;
    t->domain_length = netsnmpUDPsharedDomain_len;

    t->f_recv          = _udpshared_recv;
    t->f_send          = _udpshared_send;
    t->f_close         = _udpshared_close;
    t->f_fmtaddr       = _udpshared_fmtaddr;
    t->f_setup_session = _setup_session;
    t->flags = NETSNMP_TRANSPORT_FLAG_SHARED;

    return t;
}

netsnmp_transport *
netsnmp_udpshared_transport(struct sockaddr_in *addr, int local)
{
    netsnmp_transport *t = NULL;

    t = netsnmp_udp_transport(addr, local);
    if (NULL == t)
        return NULL;

    t = _transport_common(t);

    return t;
}

netsnmp_transport *
netsnmp_udpshared_transport_with_source(struct sockaddr_in *addr, int flags,
                                        struct sockaddr_in *src_addr)
{
    netsnmp_transport *t = NULL, *b = NULL;
    int                local = flags & NETSNMP_TSPEC_LOCAL;

    DEBUGMSGTL(("udpshared:create", "from addr with source\n"));

    /** init common parts of parent transport */
    t = netsnmp_udpipv4base_transport_init(addr, local);
    if (NULL == t)
        return NULL;

    _transport_common(t);

    if (!local && src_addr) {
        /** check for existing base transport */
        b = netsnmp_transport_cache_get(PF_INET, SOCK_DGRAM, local,
                                        (netsnmp_sockaddr_storage *)src_addr);
        if (NULL != b && NULL != b->local) {
            /*
             * uh-oh. we've assumed sharedudp is just for clients, and we're
             * using local_length as a reference count.
             */
            snmp_log(LOG_ERR,
                     "sharedudp transport is only for client/remote\n");
            netsnmp_transport_free(t);
            return NULL;
        }
    }

    /** if no base transport found, create one */
    if (NULL == b) {
        b = netsnmp_udpipv4base_transport_with_source(addr, local, src_addr);
        if (NULL == b) {
            netsnmp_transport_free(t);
            return NULL;
        }
    }
    ++b->local_length; /* reference count */
    t->base_transport = b;
    t->flags |= NETSNMP_TRANSPORT_FLAG_SHARED;

    /** get local socket address */
    if (!local) {
        t->sock = b->sock;
        netsnmp_udpipv4base_transport_get_bound_addr(t);
    }

    /** cache base transport for future use */
    if (!local && src_addr && 1 == b->local_length) {
        netsnmp_transport_cache_save(PF_INET, SOCK_DGRAM, local,
                                     (netsnmp_sockaddr_storage *)src_addr, b);
    }

    return t;
}

#ifdef NETSNMP_TRANSPORT_UDPIPV6_DOMAIN

/*
 * Open a shared UDP transport for SNMP.  Local is TRUE if addr is the local
 * address to bind to (i.e. this is a server-type session); otherwise addr is
 * the remote address to send things to.
 */
netsnmp_transport *
netsnmp_udpshared6_transport(struct sockaddr_in6 *addr, int local)
{
    netsnmp_transport *t = NULL;

    t = netsnmp_udp6_transport(addr, local);
    if (NULL != t)
        t = _transport_common(t);

    return t;
}

netsnmp_transport *
netsnmp_udpshared6_transport_with_source(struct sockaddr_in6 *addr6, int flags,
                                         struct sockaddr_in6 *src_addr6)
{
    netsnmp_transport *t = NULL, *b = NULL;
    int                local = flags & NETSNMP_TSPEC_LOCAL;

    DEBUGMSGTL(("udpshared:create", "from addr6 with source\n"));

    /** init common parts of parent transport */
    t = netsnmp_udp6_transport_init(addr6, local);
    if (NULL == t)
        return NULL;

    _transport_common(t);

    if (!local && src_addr6) {
        /** check for existing base transport */
        b = netsnmp_transport_cache_get(PF_INET6, SOCK_DGRAM, local,
                                        (netsnmp_sockaddr_storage *)src_addr6);
        if (NULL != b && NULL != b->local) {
            /*
             * uh-oh. we've assumed sharedudp is just for clients, and we're
             * using local_length as a reference count.
             */
            snmp_log(LOG_ERR,
                     "sharedudp transport is only for client/remote\n");
            netsnmp_transport_free(t);
            return NULL;
        }
    }

    /** if no base transport found, create one */
    if (NULL == b) {
        b = netsnmp_udp6_transport_with_source(addr6, local, src_addr6);
        if (NULL == b) {
            netsnmp_transport_free(t);
            return NULL;
        }
    }
    ++b->local_length; /* reference count */
    t->base_transport = b;
    t->flags |= NETSNMP_TRANSPORT_FLAG_SHARED;

    /** get local socket address */
    if (!local) {
        t->sock = b->sock;
        netsnmp_udp6_transport_get_bound_addr(t);
    }

    /** cache base transport for future use */
    if (!local && src_addr6 && 1 == b->local_length) {
        netsnmp_transport_cache_save(PF_INET6, SOCK_DGRAM, local,
                                     (netsnmp_sockaddr_storage *)src_addr6, b);
    }

    return t;
}
#endif /* NETSNMP_TRANSPORT_UDPIPV6_DOMAIN */

netsnmp_transport *
netsnmp_udpshared_create_ostring(const u_char * o, size_t o_len, int local)
{
    struct sockaddr_in addr;

    DEBUGMSGTL(("udpshared:create", "from ostring\n"));

    if (o_len == 6) {
        unsigned short porttmp = (o[4] << 8) + o[5];
        addr.sin_family = AF_INET;
        memcpy((u_char *) & (addr.sin_addr.s_addr), o, 4);
        addr.sin_port = htons(porttmp);
        return netsnmp_udpshared_transport(&addr, local);
    }
#ifdef NETSNMP_TRANSPORT_UDPIPV6_DOMAIN
    else if (o_len == 18) {
        struct sockaddr_in6 addr6;
        unsigned short porttmp = (o[16] << 8) + o[17];
        addr6.sin6_family = AF_INET6;
        memcpy((u_char *) & (addr6.sin6_addr.s6_addr), o, 4);
        addr6.sin6_port = htons(porttmp);
        return netsnmp_udpshared6_transport(&addr6, local);
    }
#endif
    return NULL;
}

netsnmp_transport *
netsnmp_udpshared_create_tstring(const char *str, int isserver,
                                 const char *default_target)
{
#ifdef NETSNMP_TRANSPORT_UDPIPV6_DOMAIN
    struct sockaddr_in6 addr6;
#endif
    struct sockaddr_in addr;
    netsnmp_transport *t;

    DEBUGMSGTL(("udpshared:create", "from tstring %s\n", str));

    if (netsnmp_sockaddr_in2(&addr, str, default_target))
        t = netsnmp_udpshared_transport(&addr, isserver);
#ifdef NETSNMP_TRANSPORT_UDPIPV6_DOMAIN
    else if (netsnmp_sockaddr_in6_2(&addr6, str, default_target))
        t = netsnmp_udpshared6_transport(&addr6, isserver);
#endif
    else
        return NULL;

    return t;
}

static netsnmp_transport *
_tspec_v4(struct sockaddr_in *addr, netsnmp_tdomain_spec *tspec)
{
    int local = tspec->flags & NETSNMP_TSPEC_LOCAL;

    if (NULL != tspec->source) {
        struct sockaddr_in src_addr, *srcp = &src_addr;
        /** get sockaddr from source */
        if (!netsnmp_sockaddr_in2(&src_addr, tspec->source, NULL))
            return NULL;
        return netsnmp_udpshared_transport_with_source(addr, local, srcp);
    } else {
        /** if no source and we do not want any default client address */
        if (tspec->flags & NETSNMP_TSPEC_NO_DFTL_CLIENT_ADDR)
            return netsnmp_udpshared_transport_with_source(addr, local, NULL);
    }

    /** no source and default client address ok */
    return netsnmp_udpshared_transport(addr, local);
}

#ifdef NETSNMP_TRANSPORT_UDPIPV6_DOMAIN
static netsnmp_transport *
_tspec_v6(struct sockaddr_in6 *addr, netsnmp_tdomain_spec *tspec)
{
    int local = tspec->flags & NETSNMP_TSPEC_LOCAL;

    if (NULL != tspec->source) {
        struct sockaddr_in6 src_addr, *srcp = &src_addr;
        /** get sockaddr from source */
        if (!netsnmp_sockaddr_in6_2(&src_addr, tspec->source, NULL))
            return NULL;
        return netsnmp_udpshared6_transport_with_source(addr, local, srcp);
    } else {
        /** if no source and we do not want any default client address */
        if (tspec->flags & NETSNMP_TSPEC_NO_DFTL_CLIENT_ADDR)
            return netsnmp_udpshared6_transport_with_source(addr, local, NULL);
    }

    /** no source and default client address ok */
    return netsnmp_udpshared6_transport(addr, local);
}
#endif /* NETSNMP_TRANSPORT_UDPIPV6_DOMAIN */

netsnmp_transport *
netsnmp_udpshared_create_tspec(netsnmp_tdomain_spec *tspec)
{
#ifdef NETSNMP_TRANSPORT_UDPIPV6_DOMAIN
    struct sockaddr_in6 addr6;
#endif
    struct sockaddr_in addr;

    DEBUGMSGTL(("udpshared:create", "from tspec\n"));

    if (NULL == tspec)
        return NULL;

    if (netsnmp_sockaddr_in2(&addr, tspec->target, tspec->default_target))
        return _tspec_v4(&addr, tspec);
#ifdef NETSNMP_TRANSPORT_UDPIPV6_DOMAIN
    else if (netsnmp_sockaddr_in6_2(&addr6, tspec->target,
                                    tspec->default_target))
        return _tspec_v6(&addr6, tspec);
#endif

    return NULL;
}

void
netsnmp_udpshared_ctor(void)
{
    static netsnmp_tdomain domain;
    static int done = 0;

    if (done)
        return;
    done = 1;

    domain.name = netsnmpUDPsharedDomain;
    domain.name_length = netsnmpUDPsharedDomain_len;

    domain.prefix = (const char**)calloc(2, sizeof(char *));
    domain.prefix[0] = "udpshared";

    domain.f_create_from_tstring     = NULL;
    domain.f_create_from_tstring_new = netsnmp_udpshared_create_tstring;
    domain.f_create_from_tspec       = netsnmp_udpshared_create_tspec;
    domain.f_create_from_ostring     = netsnmp_udpshared_create_ostring;

    netsnmp_tdomain_register(&domain);
}
