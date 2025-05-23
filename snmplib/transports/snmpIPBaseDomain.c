#include <net-snmp/net-snmp-config.h>
#include <net-snmp/types.h>
#include <net-snmp/output_api.h>
#include <net-snmp/library/system.h>
#include "snmpIPBaseDomain.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int netsnmp_isnumber(const char *cp)
{
    if (!*cp)
        return 0;

    while (isdigit((unsigned char)*cp))
        cp++;
    return *cp == '\0';
}

/**
 * Parse a Net-SNMP endpoint name.
 * @ep_str: Parsed endpoint name.
 * @endpoint: Endpoint specification in the format
 *   <address>[@<iface>]:[<port>], <address>[@<iface>] or <port>.
 *
 * Only overwrite those fields of *@ep_str that have been set in
 * @endpoint. Returns 1 upon success and 0 upon failure.
 */
int netsnmp_parse_ep_str(struct netsnmp_ep_str *ep_str, const char *endpoint)
{
    char *dup, *cp, *addrstr = NULL, *iface = NULL, *portstr = NULL;
    unsigned port;

    if (!endpoint)
        return 0;

    dup = strdup(endpoint);
    if (!dup)
        return 0;

    cp = dup;
    if (netsnmp_isnumber(cp)) {
        portstr = cp;
    } else {
        if (*cp == '[') {
            addrstr = cp + 1;
            cp = strchr(cp, ']');
            if (cp) {
                cp[0] = '\0';
                cp++;
            } else {
                goto err;
            }
        } else if (*cp != '@' && (*cp != ':' || cp[1] == ':')) {
            addrstr = cp;
            cp = strchr(addrstr, '@');
            if (!cp) {
                cp = strrchr(addrstr, ':');
                if (cp && strchr(dup, ':') < cp)
                    cp = NULL;
            }
        }
        if (cp && *cp == '@') {
            *cp = '\0';
            iface = cp + 1;
            cp = strchr(cp + 1, ':');
        }
        if (cp && *cp == ':') {
            *cp++ = '\0';
            portstr = cp;
            if (!netsnmp_isnumber(cp))
                goto err;
        } else if (cp && *cp) {
            goto err;
        }
    }

    if (addrstr) {
        ep_str->addr = strdup(addrstr);
        if (!ep_str->addr)
            goto err;
    }
    if (iface)
        strlcpy(ep_str->iface, iface, sizeof(ep_str->iface));
    if (portstr) {
        port = atoi(portstr);
        if (port <= 0xffff)
            strlcpy(ep_str->port, portstr, sizeof(ep_str->port));
        else
            goto err;
    }

    free(dup);
    return 1;

err:
    free(ep_str->addr);
    ep_str->addr = NULL;
    free(dup);
    return 0;
}

int netsnmp_bindtodevice(int fd, const char *iface)
{
    /* If no interface name has been specified, report success. */
    if (!iface || iface[0] == '\0')
        return 0;

#ifdef HAVE_SO_BINDTODEVICE
    {
        /*
         * +1 to work around the Linux kernel bug that the passed in name is not
         * '\0'-terminated.
         */
        int ifacelen = strlen(iface) + 1;
        int ret;

        ret = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface, ifacelen);
        if (ret < 0)
            snmp_log(LOG_ERR, "Binding socket to interface %s failed: %s\n",
                     iface, strerror(errno));
        return ret;
    }
#else
    errno = EINVAL;
    return -1;
#endif
}

int netsnmp_ipbase_session_init(struct netsnmp_transport_s *transport,
                            struct snmp_session *sess) {
    union {
        struct sockaddr     sa;
        struct sockaddr_in  sin;
        struct sockaddr_in6 sin6;
    } ss;
    socklen_t len = sizeof(ss);

    if (!sess) {
        DEBUGMSGTL(("netsnmp_ipbase", "session pointer is NULL\n"));
        return SNMPERR_SUCCESS;
    }

    if (getsockname(transport->sock, (struct sockaddr *)&ss, &len) == -1) {
        DEBUGMSGTL(("netsnmp_ipbase", "getsockname error %s\n", strerror(errno)));
        return SNMPERR_SUCCESS;
    }
    switch (ss.sa.sa_family) {
    case AF_INET:
        sess->local_port = ntohs(ss.sin.sin_port);
        break;
    case AF_INET6:
        sess->local_port = ntohs(ss.sin6.sin6_port);
        break;
    default:
        DEBUGMSGTL(("netsnmp_ipbase", "unsupported address family %d\n",
                    ss.sa.sa_family));
        return SNMPERR_SUCCESS;
    }

    DEBUGMSGTL(("netsnmp_ipbase", "local port number %d\n", sess->local_port));

    return SNMPERR_SUCCESS;
}
