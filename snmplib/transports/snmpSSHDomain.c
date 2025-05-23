#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-features.h>

#include <net-snmp/library/snmp_assert.h>
#include <net-snmp/library/snmpSSHDomain.h>

#include <stdio.h>
#include <sys/types.h>
#include <errno.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include <pwd.h>

#ifndef MAXPATHLEN
#warning no system max path length detected
#define MAXPATHLEN 2048
#endif

#include <net-snmp/types.h>
#include <net-snmp/output_api.h>
#include <net-snmp/library/tools.h>
#include <net-snmp/library/system.h>
#include <net-snmp/library/default_store.h>

#include <net-snmp/library/snmp.h>
#include <net-snmp/library/snmp_transport.h>
#include "snmpIPBaseDomain.h"
#include <net-snmp/library/snmpSocketBaseDomain.h>
#include <net-snmp/library/read_config.h>
#include <net-snmp/library/snmp_secmod.h>
#include <net-snmp/library/snmptsm.h>

netsnmp_feature_require(transport_cache);
netsnmp_feature_require(user_information);

#define MAX_NAME_LENGTH 127

#define NETSNMP_SSHTOSNMP_VERSION1      1
#define NETSNMP_MAX_SSHTOSNMP_VERSION   1

#define DEFAULT_SOCK_NAME "sshdomainsocket"

typedef struct netsnmp_ssh_addr_pair_s {
    union {
        struct sockaddr_in in;
        struct sockaddr_in6 in6;
    } remote_addr;
    struct in_addr local_addr;
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    LIBSSH2_AGENT *agent;
    char username[MAX_NAME_LENGTH+1];
    struct sockaddr_un unix_socket_end;
    char socket_path[MAXPATHLEN];
    int remote_addr_len;
} netsnmp_ssh_addr_pair;

/* XX: looks like the wrong oid, see header */
const oid netsnmp_snmpSSHDomain[] = { TRANSPORT_DOMAIN_SSH_IP };
static netsnmp_tdomain sshDomain;

#define SNMPSSHDOMAIN_USE_EXTERNAL_PIPE 1

/*
 * Return a string representing the address in data, or else the "far end"
 * address if data is NULL.  
 */

static char *
netsnmp_ssh_fmtaddr(netsnmp_transport *t, const void *data, int len)
{
    const netsnmp_ssh_addr_pair *addr_pair = NULL;

    if (data != NULL && len == sizeof(netsnmp_ssh_addr_pair)) {
	addr_pair = (const netsnmp_ssh_addr_pair *) data;
    } else if (t != NULL && t->data != NULL) {
	addr_pair = (const netsnmp_ssh_addr_pair *) t->data;
    }

    if (addr_pair == NULL) {
        return strdup("SSH: unknown");
    } else {
        const struct sockaddr_in *to;
	char *tmp;

        to = (const struct sockaddr_in *) &(addr_pair->remote_addr);
        if (asprintf(&tmp, "SSH: [%s]:%hd",
                     inet_ntoa(to->sin_addr), ntohs(to->sin_port)) < 0)
            tmp = NULL;
        return tmp;
    }
}

static void netsnmp_ssh_get_taddr(struct netsnmp_transport_s *t,
                                  void **addr, size_t *addr_len)
{
    switch (t->remote_length) {
    case sizeof(struct sockaddr_in):
        netsnmp_ipv4_get_taddr(t, addr, addr_len);
        break;
    case sizeof(struct sockaddr_in6):
        netsnmp_ipv6_get_taddr(t, addr, addr_len);
        break;
    default:
        *addr = NULL;
        netsnmp_assert(0);
    }
}

/*
 * You can write something into opaque that will subsequently get passed back 
 * to your send function if you like.  For instance, you might want to
 * remember where a PDU came from, so that you can send a reply there...  
 */

static int
netsnmp_ssh_recv(netsnmp_transport *t, void *buf, int size,
		 void **opaque, int *olength)
{
    int rc = -1;
    netsnmp_tmStateReference *tmStateRef = NULL;
    netsnmp_ssh_addr_pair *addr_pair = NULL;
    int iamclient = 0;

    DEBUGMSGTL(("ssh", "at the top of ssh_recv\n"));
    DEBUGMSGTL(("ssh", "t=%p\n", t));
    if (t != NULL && t->data != NULL) {
	addr_pair = (netsnmp_ssh_addr_pair *) t->data;
    }

    DEBUGMSGTL(("ssh", "addr_pair=%p\n", addr_pair));
    if (t != NULL && addr_pair && addr_pair->channel) {
        DEBUGMSGTL(("ssh", "t=%p, addr_pair=%p, channel=%p\n",
                    t, addr_pair, addr_pair->channel));
        iamclient = 1;
	while (rc < 0) {
	    rc = libssh2_channel_read(addr_pair->channel, buf, size);
	    if (rc < 0) {  /* XXX: from tcp; ssh equiv?:  && errno != EINTR */
		DEBUGMSGTL(("ssh", "recv fd %d err %d (\"%s\")\n",
			    t->sock, errno, strerror(errno)));
		break;
	    }
	    DEBUGMSGTL(("ssh", "recv fd %d got %d bytes\n",
			t->sock, rc));
            /* XX: if we read zero bytes, server probably not running */
	}
    } else if (t != NULL) {

#ifdef SNMPSSHDOMAIN_USE_EXTERNAL_PIPE

        socklen_t       tolen = sizeof(struct sockaddr_un);

        if (t != NULL && t->sock >= 0) {
            struct sockaddr *to;
            to = (struct sockaddr *) SNMP_MALLOC_STRUCT(sockaddr_un);
            if (NULL == to) {
                *opaque = NULL;
                *olength = 0;
                return -1;
            }

            if(getsockname(t->sock, to, &tolen) != 0){
                free(to);
                *opaque = NULL;
                *olength = 0;
                return -1;
            };

            if (addr_pair && addr_pair->username[0] == '\0') {
                /* we don't have a username yet, so this is the first message */
                struct msghdr msg;
                struct iovec iov[1];
                char cmsg[2 * 4096];
                struct cmsghdr *cmsgptr;
                u_char *charbuf  = buf;

                iov[0].iov_base = buf;
                iov[0].iov_len = size;

                memset(&msg, 0, sizeof msg);
                msg.msg_iov = iov;
                msg.msg_iovlen = 1;
                msg.msg_control = &cmsg;
                msg.msg_controllen = sizeof(cmsg);
                
                rc = recvmsg(t->sock, &msg, MSG_DONTWAIT); /* use DONTWAIT? */
                if (rc <= 0) {
                    return rc;
                }

                /* we haven't received the starting info */
                if ((u_char) charbuf[0] > NETSNMP_SSHTOSNMP_VERSION1) {
                    /* unsupported connection version */
                    snmp_log(LOG_ERR, "received unsupported sshtosnmp version: %d\n", charbuf[0]);
                    return -1;
                }

                DEBUGMSGTL(("ssh", "received first msg over SSH; internal SSH protocol version %d\n", charbuf[0]));

                for (cmsgptr = CMSG_FIRSTHDR(&msg); cmsgptr != NULL; cmsgptr = CMSG_NXTHDR(&msg, cmsgptr)) {
#if defined(SCM_CREDENTIALS)
                    if (cmsgptr->cmsg_level == SOL_SOCKET && cmsgptr->cmsg_type == SCM_CREDENTIALS) {
                        /* received credential info */
		        struct ucred *remoteuser;
                        struct passwd *user_pw;

                        remoteuser = (struct ucred *) CMSG_DATA(cmsgptr);

                        if ((user_pw = getpwuid(remoteuser->uid)) == NULL) {
                            snmp_log(LOG_ERR, "No user found for uid %d\n",
                                remoteuser->uid);
                            return -1;
                        }
                        if (strlen(user_pw->pw_name) >
                            sizeof(addr_pair->username)-1) {
                            snmp_log(LOG_ERR,
                                     "User name '%s' too long for snmp\n",
                                     user_pw->pw_name);
                            return -1;
                        }
                        strlcpy(addr_pair->username, user_pw->pw_name,
                                sizeof(addr_pair->username));
                    }
#elif defined(SCM_CREDS)
                    if (cmsgptr->cmsg_level == SOL_SOCKET && cmsgptr->cmsg_type == SCM_CREDS) {
                        /* received credential info */
		        struct cmsgcred *remoteuser;
                        struct passwd *user_pw;

                        remoteuser = (void *)CMSG_DATA(cmsgptr);

                        if ((user_pw = getpwuid(remoteuser->cmcred_uid)) == NULL) {
                            snmp_log(LOG_ERR, "No user found for uid %d\n",
				     remoteuser->cmcred_uid);
                            return -1;
                        }
                        if (strlen(user_pw->pw_name) >
                            sizeof(addr_pair->username)-1) {
                            snmp_log(LOG_ERR,
                                     "User name '%s' too long for snmp\n",
                                     user_pw->pw_name);
                            return -1;
                        }
                        strlcpy(addr_pair->username, user_pw->pw_name,
                                sizeof(addr_pair->username));
                    }
#endif
		    DEBUGMSGTL(("ssh", "Setting user name to %s\n",
				addr_pair->username));
                }

                if (addr_pair->username[0] == '\0') {
                    snmp_log(LOG_ERR,
                             "failed to extract username from sshd connected unix socket\n");
                    return -1;
                }

                if (rc == 1) {
                    /* the only packet we received was the starting one */
                    t->flags |= NETSNMP_TRANSPORT_FLAG_EMPTY_PKT;
                    return 0;
                }

                rc -= 1;
                memmove(charbuf, &charbuf[1], rc);
            } else {
                while (rc < 0) {
                    rc = recvfrom(t->sock, buf, size, 0, NULL, NULL);
                    if (rc < 0 && errno != EINTR) {
                        DEBUGMSGTL(("ssh", "recv fd %d err %d (\"%s\")\n",
                                    t->sock, errno, strerror(errno)));
                        return rc;
                    }
                    *opaque = (void*)to;
                    *olength = sizeof(struct sockaddr_un);
                }
            }
            DEBUGMSGTL(("ssh", "recv fd %d got %d bytes\n",
                        t->sock, rc));
        }
        
#else /* we're called directly by sshd and use stdin/out */

        struct passwd *user_pw;

        iamclient = 0;
        /* we're on the server side and should read from stdin */
        while (rc < 0) {
            rc = read(STDIN_FILENO, buf, size);
            if (rc < 0 && errno != EINTR) {
                DEBUGMSGTL(("ssh",
                            " read on stdin failed: %d (\"%s\")\n",
                            errno, strerror(errno)));
                break;
            }
            if (rc == 0) {
                /* 0 input is probably bad since we selected on it */
                DEBUGMSGTL(("ssh", "got a 0 read on stdin\n"));
                return -1;
            }
            DEBUGMSGTL(("ssh", "read on stdin got %d bytes\n", rc));
        }

/* XXX: need to check the username, but addr_pair doesn't exist! */
        /*
        DEBUGMSGTL(("ssh", "current username=%s\n", c));
        if (addr_pair->username[0] == '\0') {
            if ((user_pw = getpwuid(getuid())) == NULL) {
                snmp_log(LOG_ERR, "No user found for uid %d\n", getuid());
                return -1;
            }
            if (strlen(user_pw->pw_name) > sizeof(addr_pair->username)-1) {
                snmp_log(LOG_ERR, "User name '%s' too long for snmp\n",
                         user_pw->pw_name);
                return -1;
            }
            strlcpy(addr_pair->username, user_pw->pw_name,
                    sizeof(addr_pair->username));
        }
        */

#endif /* ! SNMPSSHDOMAIN_USE_EXTERNAL_PIPE */
    }

    /* create a tmStateRef cache */
    tmStateRef = SNMP_MALLOC_TYPEDEF(netsnmp_tmStateReference);

    /* secshell document says were always authpriv, even if NULL algorithms */
    /* ugh! */
    /* XXX: disallow NULL in our implementations */
    tmStateRef->transportSecurityLevel = SNMP_SEC_LEVEL_AUTHPRIV;

    /* XXX: figure out how to get the specified local secname from the session */
    if (iamclient && 0) {
        /* XXX: we're on the client; we should have named the
           connection ourselves...  pull this from session somehow? */
        strlcpy(tmStateRef->securityName, addr_pair->username,
                sizeof(tmStateRef->securityName));
    } else {
#ifdef SNMPSSHDOMAIN_USE_EXTERNAL_PIPE
        strlcpy(tmStateRef->securityName, addr_pair->username,
                sizeof(tmStateRef->securityName));
#else /* we're called directly by sshd and use stdin/out */
        /* we're on the server... */
        /* XXX: this doesn't copy properly and can get pointer
           reference issues */
        const char *securityName = getenv("USER");
        if (!securityName) {
            snmp_log(LOG_ERR, "USER environment variable missing, no username available\n\n");
            return -1;
        }
	if (strlen(securityName) > (sizeof(tmStateRef->securityName) - 1)) {
            /* ruh roh */
            /* XXX: clean up */
            snmp_log(LOG_ERR, "User name '%s' too long for snmp\n",
                     securityName);
            return -1;
        }

        /* XXX: detect and throw out overflow secname sizes rather
           than truncating. */
        strlcpy(tmStateRef->securityName, getenv("USER"),
                sizeof(tmStateRef->securityName));
#endif /* ! SNMPSSHDOMAIN_USE_EXTERNAL_PIPE */
    }
    tmStateRef->securityName[sizeof(tmStateRef->securityName)-1] = '\0';
    tmStateRef->securityNameLen = strlen(tmStateRef->securityName);
    *opaque = tmStateRef;
    *olength = sizeof(netsnmp_tmStateReference);

    return rc;
}



static int
netsnmp_ssh_send(netsnmp_transport *t, const void *buf, int size,
		 void **opaque, int *olength)
{
    int rc = -1;

    netsnmp_ssh_addr_pair *addr_pair = NULL;
    netsnmp_tmStateReference *tmStateRef = NULL;

    if (t != NULL && t->data != NULL) {
	addr_pair = (netsnmp_ssh_addr_pair *) t->data;
    }

    if (opaque != NULL && *opaque != NULL &&
        *olength == sizeof(netsnmp_tmStateReference)) {
        tmStateRef = (netsnmp_tmStateReference *) *opaque;
    }

    if (!tmStateRef) {
        /* this is now an error according to my memory in the recent draft */
        snmp_log(LOG_ERR, "netsnmp_ssh_send wasn't passed a valid tmStateReference\n");
        return -1;
    }

    if (NULL != t && NULL != addr_pair && NULL != addr_pair->channel) {
        if ((NETSNMP_TM_SAME_SECURITY_NOT_REQUIRED == tmStateRef->sameSecurity) && (!tmStateRef->securityNameLen)) {
            /* first message sent */
            tmStateRef->securityNameLen = strlcpy(tmStateRef->securityName, addr_pair->username,
                    sizeof(tmStateRef->securityName));
        } else if (addr_pair->username[0] == '\0') {
            strlcpy(addr_pair->username, tmStateRef->securityName,
                    sizeof(addr_pair->username));
        } else if ((NETSNMP_TM_USE_SAME_SECURITY == tmStateRef->sameSecurity) &&
	           (strcmp(addr_pair->username, tmStateRef->securityName) != 0 ||
                   strlen(addr_pair->username) != tmStateRef->securityNameLen)) {
            /* error!  they must always match */
            snmp_log(LOG_ERR, "netsnmp_ssh_send was passed a tmStateReference with a securityName not equal to previous messages\n");
            return -1;
        }
	while (rc < 0) {
	    rc = libssh2_channel_write(addr_pair->channel, buf, size);
	    if (rc < 0) { /* XXX:  && errno != EINTR */
		break;
	    }
	}
    } else if (t != NULL) {
#ifdef SNMPSSHDOMAIN_USE_EXTERNAL_PIPE

	while (rc < 0) {
            rc = sendto(t->sock, buf, size, 0, NULL, 0);
            
            if (rc < 0 && errno != EINTR) {
                break;
            }
        }

#else /* we're called directly by sshd and use stdin/out */
        /* on the server; send to stdout */
	while (rc < 0) {
	    rc = write(STDOUT_FILENO, buf, size);
            fflush(stdout);
	    if (rc < 0 && errno != EINTR) { /* XXX:  && errno != EINTR */
		break;
	    }
	}
#endif
    }

    return rc;
}



static int
netsnmp_ssh_close(netsnmp_transport *t)
{
    int rc = -1;
    netsnmp_ssh_addr_pair *addr_pair = NULL;

    if (t != NULL && t->data != NULL) {
	addr_pair = (netsnmp_ssh_addr_pair *) t->data;
    }

    if (t != NULL && addr_pair && t->sock >= 0) {
        DEBUGMSGTL(("ssh", "close fd %d\n", t->sock));

        if (addr_pair->channel) {
            libssh2_channel_close(addr_pair->channel);
            libssh2_channel_free(addr_pair->channel);
            addr_pair->channel = NULL;
        }

        if (addr_pair->session) {
            libssh2_session_disconnect(addr_pair->session, "Normal Shutdown");
            libssh2_session_free(addr_pair->session);
            addr_pair->session = NULL;
        }

#ifndef HAVE_CLOSESOCKET
        rc = close(t->sock);
#else
        rc = closesocket(t->sock);
#endif
        t->sock = -1;

#ifdef SNMPSSHDOMAIN_USE_EXTERNAL_PIPE

        if (!addr_pair->session && !addr_pair->channel) {
            /* XXX: make configurable */
            unlink(addr_pair->socket_path);
        }

#else /* we're called directly by sshd and use stdin/out */

        /* on the server: close stdin/out */
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
#endif /* ! SNMPSSHDOMAIN_USE_EXTERNAL_PIPE */

    } else {

#ifndef SNMPSSHDOMAIN_USE_EXTERNAL_PIPE
        /* on the server: close stdin/out */
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
#endif /* ! SNMPSSHDOMAIN_USE_EXTERNAL_PIPE */

    }
    return rc;
}



static int
netsnmp_ssh_accept(netsnmp_transport *t)
{
#ifdef SNMPSSHDOMAIN_USE_EXTERNAL_PIPE

    /* much of this is duplicated from snmpUnixDomain.c */

    netsnmp_ssh_addr_pair *addr_pair;    
    int                    newsock   = -1;
    struct sockaddr       *farend    = NULL;
    socklen_t              farendlen = sizeof(struct sockaddr_un);


    if (t != NULL && t->sock >= 0) {
        addr_pair = SNMP_MALLOC_TYPEDEF(netsnmp_ssh_addr_pair);

        if (addr_pair == NULL) {
            /*
             * Indicate that the acceptance of this socket failed.
             */
            DEBUGMSGTL(("ssh", "accept: malloc failed\n"));
            return -1;
        }

        farend = (struct sockaddr *) &addr_pair->unix_socket_end;

        newsock = accept(t->sock, farend, &farendlen);

        if (newsock < 0) {
            DEBUGMSGTL(("ssh","accept failed rc %d errno %d \"%s\"\n",
                        newsock, errno, strerror(errno)));
            free(addr_pair);
            return newsock;
        }

#ifdef SO_PASSCRED
        /* set the SO_PASSCRED option so we can receive the remote uid */
        {
            int one = 1;
            setsockopt(newsock, SOL_SOCKET, SO_PASSCRED, (void *) &one,
                       sizeof(one));
        }
#elif defined(LOCAL_CREDS)
        {
            int one = 1;
            setsockopt(newsock, SOL_SOCKET, LOCAL_CREDS, (void *) &one,
                       sizeof(one));
        }
#endif

        if (t->data != NULL) {
            free(t->data);
        }

        DEBUGMSGTL(("ssh", "accept succeeded (farend %p len %d)\n",
                    farend, farendlen));
        t->data = addr_pair;
        t->data_length = sizeof(netsnmp_ssh_addr_pair);
        netsnmp_sock_buffer_set(newsock, SO_SNDBUF, 1, 0);
        netsnmp_sock_buffer_set(newsock, SO_RCVBUF, 1, 0);
        return newsock;
    } else {
        return -1;
    }

#else /* we're called directly by sshd and use stdin/out */
    /* we don't need to do anything; server side uses stdin/out */
    /* XXX: check that we're an ssh connection */
    
    return STDIN_FILENO; /* return stdin */
#endif /* ! SNMPSSHDOMAIN_USE_EXTERNAL_PIPE */

}

/*
 * Open a SSH-based transport for SNMP.  Local is TRUE if addr is the local
 * address to bind to (i.e. this is a server-type session); otherwise addr is 
 * the remote address to send things to.  
 */

static netsnmp_transport *
netsnmp_ssh_transport(const struct netsnmp_ep *ep, int local, int domain)
{
    netsnmp_transport *t = NULL;
    netsnmp_ssh_addr_pair *addr_pair = NULL;
    int rc = 0;
    int i;
    const char *fingerprint;
    char *userauthlist;
    struct sockaddr_un *unaddr;
    const char *sockpath =
        netsnmp_ds_get_string(NETSNMP_DS_LIBRARY_ID,
                              NETSNMP_DS_LIB_SSHTOSNMP_SOCKET);
    char tmpsockpath[MAXPATHLEN];

#ifdef NETSNMP_NO_LISTEN_SUPPORT
    if (local) {
        return NULL;
    }
#endif /* NETSNMP_NO_LISTEN_SUPPORT */

    if (local && PF_UNIX != domain) {
        return NULL;
    }
    if (!local && (PF_INET != domain && PF_INET6 != domain)) {
        return NULL;
    }

    t = SNMP_MALLOC_TYPEDEF(netsnmp_transport);
    if (t == NULL) {
        return NULL;
    }

    t->domain = netsnmp_snmpSSHDomain;
    t->domain_length = netsnmp_snmpSSHDomain_len;
    t->flags = NETSNMP_TRANSPORT_FLAG_STREAM | NETSNMP_TRANSPORT_FLAG_TUNNELED;

    addr_pair = SNMP_MALLOC_TYPEDEF(netsnmp_ssh_addr_pair);
    if (addr_pair == NULL) {
        netsnmp_transport_free(t);
        return NULL;
    }
    t->data = addr_pair;
    t->data_length = sizeof(netsnmp_ssh_addr_pair);

    if (local) {
#ifndef NETSNMP_NO_LISTEN_SUPPORT
#ifdef SNMPSSHDOMAIN_USE_EXTERNAL_PIPE

        /* XXX: set t->local and t->local_length */


        t->flags |= NETSNMP_TRANSPORT_FLAG_LISTEN;

        unaddr = &addr_pair->unix_socket_end;

        /* open a unix domain socket */
        /* XXX: get data from the transport def for it's location */
        unaddr->sun_family = AF_UNIX;
        if (NULL == sockpath) {
            snprintf(tmpsockpath, sizeof(tmpsockpath), "%s/%s",
                     get_persistent_directory(), DEFAULT_SOCK_NAME);
            sockpath = tmpsockpath;
        }

        snprintf(unaddr->sun_path, sizeof(unaddr->sun_path), "%s", sockpath);
        snprintf(addr_pair->socket_path, sizeof(addr_pair->socket_path), "%s",
                 sockpath);

        t->sock = socket(PF_UNIX, SOCK_STREAM, 0);
        if (t->sock < 0) {
            netsnmp_transport_free(t);
            return NULL;
        }

#if defined(SO_PASSCRED)
        /* set the SO_PASSCRED option so we can receive the remote uid */
        {
            int one = 1;
            setsockopt(t->sock, SOL_SOCKET, SO_PASSCRED, (void *) &one,
                       sizeof(one));
        }
#elif defined(LOCAL_CREDS)
        {
            int one = 1;
            setsockopt(t->sock, SOL_SOCKET, LOCAL_CREDS, (void *) &one,
                       sizeof(one));
        }
#endif


        unlink(unaddr->sun_path);
        rc = bind(t->sock, (struct sockaddr *)unaddr, SUN_LEN(unaddr));
        if (rc != 0) {
            DEBUGMSGTL(("netsnmp_ssh_transport",
                        "couldn't bind \"%s\", errno %d (%s)\n",
                        unaddr->sun_path, errno, strerror(errno)));
            netsnmp_ssh_close(t);
            netsnmp_transport_free(t);
            return NULL;
        }


        /* set the socket permissions */
        {
            /*
             * Apply any settings to the ownership/permissions of the
             * Sshdomain socket
             */
            int sshdomain_sock_perm =
                netsnmp_ds_get_int(NETSNMP_DS_LIBRARY_ID,
                                   NETSNMP_DS_SSHDOMAIN_SOCK_PERM);
            int sshdomain_sock_user =
                netsnmp_ds_get_int(NETSNMP_DS_LIBRARY_ID,
                                   NETSNMP_DS_SSHDOMAIN_SOCK_USER);
            int sshdomain_sock_group =
                netsnmp_ds_get_int(NETSNMP_DS_LIBRARY_ID,
                                   NETSNMP_DS_SSHDOMAIN_SOCK_GROUP);

            DEBUGMSGTL(("ssh", "here: %s, %d, %d, %d\n",
                        unaddr->sun_path,
                        sshdomain_sock_perm, sshdomain_sock_user,
                        sshdomain_sock_group));
            if (sshdomain_sock_perm != 0) {
                DEBUGMSGTL(("ssh", "Setting socket perms to %d\n",
                            sshdomain_sock_perm));
                chmod(unaddr->sun_path, sshdomain_sock_perm);
            }

            if (sshdomain_sock_user || sshdomain_sock_group) {
                /*
                 * If either of user or group haven't been set,
                 *  then leave them unchanged.
                 */
                if (sshdomain_sock_user == 0 )
                    sshdomain_sock_user = -1;
                if (sshdomain_sock_group == 0 )
                    sshdomain_sock_group = -1;
                DEBUGMSGTL(("ssh", "Setting socket user/group to %d/%d\n",
                            sshdomain_sock_user, sshdomain_sock_group));
                if (chown(unaddr->sun_path,
                          sshdomain_sock_user, sshdomain_sock_group) < 0)
                    snmp_log_perror("SSH socket chown");
            }
        }

        rc = listen(t->sock, NETSNMP_STREAM_QUEUE_LEN);
        if (rc != 0) {
            DEBUGMSGTL(("netsnmp_ssh_transport",
                        "couldn't listen to \"%s\", errno %d (%s)\n",
                        unaddr->sun_path, errno, strerror(errno)));
            netsnmp_ssh_close(t);
            netsnmp_transport_free(t);
            return NULL;
        }
        

#else /* we're called directly by sshd and use stdin/out */
        /* for ssh on the server side we've been launched so bind to
           stdin/out */

        /* nothing to do */

        /* XXX: verify we're inside ssh */
        t->sock = STDIN_FILENO;
#endif /* ! SNMPSSHDOMAIN_USE_EXTERNAL_PIPE */
#else /* NETSNMP_NO_LISTEN_SUPPORT */
        netsnmp_transport_free(t);
        return NULL;
#endif /* NETSNMP_NO_LISTEN_SUPPORT */
    } else {
        char *username;
        char *keyfilepub;
        char *keyfilepriv;
        int agent;

        /* use the requested user name */
        username = netsnmp_ds_get_string(NETSNMP_DS_LIBRARY_ID,
                                         NETSNMP_DS_LIB_SSH_USERNAME);
        if (!username || 0 == *username) {
            username = getenv("USER");
        }
        if (!username || 0 == *username) {
            snmp_log(LOG_ERR, "You must specify a ssh username to use.  See the snmp.conf manual page\n");
            netsnmp_transport_free(t);
            return NULL;
        }

        /* username too long, complain */
        if (strlen(username) > (sizeof(addr_pair->username) - 1)) {
            snmp_log(LOG_ERR, "Your ssh username is longer than %d characters.\n", (int)(sizeof(addr_pair->username) - 1));
            netsnmp_transport_free(t);
            return NULL;
	}
	strlcpy(addr_pair->username, username, sizeof(addr_pair->username));

        /* should we attempt agent forwarding */
	agent = netsnmp_ds_get_boolean(NETSNMP_DS_LIBRARY_ID,
                                       NETSNMP_DS_LIB_SSH_AGENT);

        /* use the requested public key file */
        keyfilepub = netsnmp_ds_get_string(NETSNMP_DS_LIBRARY_ID,
                                           NETSNMP_DS_LIB_SSH_PUBKEY);
        if (!agent && (!keyfilepub || 0 == *keyfilepub)) {
            /* XXX: default to ~/.ssh/id_rsa.pub */
            snmp_log(LOG_ERR, "You must specify a ssh public key file to use.  See the snmp.conf manual page\n");
            netsnmp_transport_free(t);
            return NULL;
        }

        /* use the requested private key file */
        keyfilepriv = netsnmp_ds_get_string(NETSNMP_DS_LIBRARY_ID,
                                            NETSNMP_DS_LIB_SSH_PRIVKEY);
        if (!agent && (!keyfilepriv || 0 == *keyfilepriv)) {
            /* XXX: default to keyfilepub without the .pub suffix */
            snmp_log(LOG_ERR, "You must specify a ssh private key file to use.  See the snmp.conf manual page\n");
            netsnmp_transport_free(t);
            return NULL;
        }

        /*
         * Handle both IPv4 and IPv6 connections here.
         */

	t->sock = socket(domain, SOCK_STREAM, 0);
        if (t->sock < 0) {
            snmp_log(LOG_ERR,"Could not allocate socket for ssh: %s\n",
                     strerror(errno));
            netsnmp_transport_free(t);
            return NULL;
        }

        if (PF_INET == domain) {
            const struct sockaddr_in *addr = &ep->a.sin;

	    t->remote_length = sizeof(*addr);
            t->remote = netsnmp_memdup(addr, sizeof(*addr));

            memcpy(&(addr_pair->remote_addr), addr, t->remote_length);
        }
        else if (PF_INET6 == domain) {
            const struct sockaddr_in6 *addr = &ep->a.sin6;

            t->remote_length = sizeof(*addr);
            t->remote = netsnmp_memdup(addr, sizeof(*addr));

            memcpy(&(addr_pair->remote_addr), addr, t->remote_length);
        }

        if (!t->remote) {
            netsnmp_ssh_close(t);
            netsnmp_transport_free(t);
            return NULL;
        }

        /*
         * This is a client-type session, so attempt to connect to the far
         * end.  We don't go non-blocking here because it's not obvious what
         * you'd then do if you tried to do snmp_sends before the connection
         * had completed.  So this can block.
         */

        rc = connect(t->sock, t->remote, t->remote_length);

        if (rc < 0) {
            snmp_log(LOG_ERR,"Could not connect to ssh server: %s\n",
                     strerror(errno));
            netsnmp_ssh_close(t);
            netsnmp_transport_free(t);
            return NULL;
        }

        /*
         * Allow user to override the send and receive buffers. Default is
         * to use os default.  Don't worry too much about errors --
         * just plough on regardless.  
         */
        netsnmp_sock_buffer_set(t->sock, SO_SNDBUF, local, 0);
        netsnmp_sock_buffer_set(t->sock, SO_RCVBUF, local, 0);

        /* open the SSH session and channel */
        addr_pair->session = libssh2_session_init();
#ifdef HAVE_LIBSSH2_SESSION_HANDSHAKE
        if (libssh2_session_handshake(addr_pair->session, t->sock)) {
#else
        if (libssh2_session_startup(addr_pair->session, t->sock)) {
#endif
          shutdown:
            snmp_log(LOG_ERR, "Failed to establish an SSH session\n");
            netsnmp_ssh_close(t);
            netsnmp_transport_free(t);
            return NULL;
        }

        /* At this point we haven't authenticated, The first thing to
           do is check the hostkey's fingerprint against our known
           hosts Your app may have it hard coded, may go to a file,
           may present it to the user, that's your call
         */
        fingerprint =
            libssh2_hostkey_hash(addr_pair->session, LIBSSH2_HOSTKEY_HASH_MD5);
        DEBUGMSGTL(("ssh", "Fingerprint: "));
        for(i = 0; i < 16; i++) {
            DEBUGMSG(("ssh", "%02x",
                      (unsigned char)fingerprint[i]));
        }
        DEBUGMSG(("ssh", "\n"));

        /* check what authentication methods are available */
        userauthlist =
            libssh2_userauth_list(addr_pair->session,
                                  username, strlen(username));
        DEBUGMSG(("ssh", "Authentication methods: %s\n", userauthlist));

        /* XXX: allow other types */
	if (strstr(userauthlist, "publickey") != NULL) {

            int agents = 0, locals = 0;

            /* try agent supplied keys */
            if (agent) {

                struct libssh2_agent_publickey *identity, *prev_identity = NULL;

                /* Connect to the ssh-agent */
                addr_pair->agent = libssh2_agent_init(addr_pair->session);

                if (!addr_pair->agent) {
                    snmp_log(LOG_ERR, "SSH agent could not be initialised\n");
                    goto shutdown;
                }
                if (libssh2_agent_connect(addr_pair->agent)) {
                    snmp_log(LOG_ERR,"could not connect to SSH agent\n");
                    goto shutdown;
                }
                if (libssh2_agent_list_identities(addr_pair->agent)) {
                    snmp_log(LOG_ERR,"could not request identities from SSH agent\n");
                    goto shutdown;
                }

		while (1) {
                    rc = libssh2_agent_get_identity(addr_pair->agent, &identity, prev_identity);

                    if (rc == 1) {
                        agent = 0;
                        break;
                    } else if (rc < 0) {
                        snmp_log(LOG_ERR,"could not obtain identity from SSH agent\n");
                        goto shutdown;
                    } else if (libssh2_agent_userauth(addr_pair->agent, username, identity)) {
                        DEBUGMSGTL(("ssh", "\tAuthentication with username %s and public key %s failed\n",
                                    username, identity->comment));
                        agents++;
                    }
                    else {
                        DEBUGMSGTL(("ssh",
                                    "\tAuthentication with username %s and agent key %s succeeded.\n",
                                    username, identity->comment));
                        goto authenticated;
                    }
                    prev_identity = identity;
		}

	    }

            /* try local keys */
            if (!agent) {

                if (!keyfilepub || !*keyfilepub || !keyfilepriv || !*keyfilepriv) {
                    /* skip attempt */
		} else if (libssh2_userauth_publickey_fromfile(addr_pair->session,
                                                        username,
                                                        keyfilepub, keyfilepriv,
                                                        NULL)) {
                    locals++;
                } else {
                    DEBUGMSGTL(("ssh",
                              "\tAuthentication with username %s and local key %s succeeded.\n",
                              username, keyfilepriv));
                    goto authenticated;
                }
            }

	    /* no luck with login */
	    snmp_log(LOG_ERR,"Authentication by public key failed: %d agent key(s), %d local key(s)\n",
                     agents, locals);
	    goto shutdown;

        } else {
            snmp_log(LOG_ERR,"Authentication by public key not supported (%s)!\n", userauthlist);
            goto shutdown;
        }

        /* we've now authenticated both sides; continuing onward ... */
        authenticated:

        /* Request a channel */
        if (!(addr_pair->channel =
              libssh2_channel_open_session(addr_pair->session))) {
            snmp_log(LOG_ERR, "Unable to open a session\n");
            goto shutdown;
        }

        /* Request a terminal with 'vanilla' terminal emulation
         * See /etc/termcap for more options
         */
        /* XXX: needed?  doubt it */
/*         if (libssh2_channel_request_pty(addr_pair->channel, "vanilla")) { */
/*             snmp_log(LOG_ERR, "Failed requesting pty\n"); */
/*             goto shutdown; */
/*         } */
        if (libssh2_channel_subsystem(addr_pair->channel, "snmp")) {
            snmp_log(LOG_ERR, "Failed to request the ssh 'snmp' subsystem\n");
            goto shutdown;
        }
    }

    DEBUGMSG(("ssh","Opened connection.\n"));
    /*
     * Message size is not limited by this transport (hence msgMaxSize
     * is equal to the maximum legal size of an SNMP message).  
     */

    t->msgMaxSize = SNMP_MAX_PACKET_LEN;
    t->f_recv     = netsnmp_ssh_recv;
    t->f_send     = netsnmp_ssh_send;
    t->f_close    = netsnmp_ssh_close;
    t->f_accept   = netsnmp_ssh_accept;
    t->f_fmtaddr  = netsnmp_ssh_fmtaddr;
    t->f_get_taddr = netsnmp_ssh_get_taddr;

    return t;
}

netsnmp_transport *
netsnmp_ssh_transport_with_source(const struct netsnmp_ep *ep,
                                  int flags,
                                  const struct netsnmp_ep *src_addr,
                                  int domain)
{
    netsnmp_transport *t = NULL;
    int                local = flags & NETSNMP_TSPEC_LOCAL;

    DEBUGMSGTL(("ssh:create", "from addr with source\n"));

    if (!local && src_addr) {
        /** check for existing cached transport */
        t = netsnmp_transport_cache_get(domain, SOCK_DGRAM, local,
                                        (const void *)src_addr,
                                        sizeof(*src_addr));
    }

    /** if no cached transport found, create one */
    if (NULL == t) {
        t = netsnmp_ssh_transport(ep, local, domain);
        if (NULL == t) {
            netsnmp_transport_free(t);
            return NULL;
        }

        netsnmp_transport_cache_save(domain, SOCK_DGRAM, local,
                                     (const void *)src_addr,
                                     sizeof(*src_addr), t);
    }

    /** get local socket address */
#if 0
    if (!local) {
        netsnmp_udp6_transport_get_bound_addr(t);
    }
#endif

    return t;
}


netsnmp_transport *
netsnmp_ssh_create_tstring(const char *str, int local,
			   const char *default_target)
{
    struct netsnmp_ep ep = { 0 };
    netsnmp_transport *t;

    DEBUGMSGTL(("ssh:create", "from tstring %s\n", str));

    if (local) {
        return netsnmp_ssh_transport(NULL, local, PF_UNIX);
    }

    if (netsnmp_sockaddr_in3(&ep, str, default_target))
        t = netsnmp_ssh_transport(&ep, local, PF_INET);
#ifdef NETSNMP_TRANSPORT_UDPIPV6_DOMAIN
    else if (netsnmp_sockaddr_in6_3(&ep, str, default_target))
        t = netsnmp_ssh_transport(&ep, local, PF_INET6);
#endif
    else
        return NULL;

    return t;
}

static netsnmp_transport *
_tspec_v4(const struct netsnmp_ep *ep, netsnmp_tdomain_spec *tspec)
{
    int local = tspec->flags & NETSNMP_TSPEC_LOCAL;

    if (NULL != tspec->source) {
        struct netsnmp_ep src_addr;

        /** get sockaddr from source */
        if (!netsnmp_sockaddr_in3(&src_addr, tspec->source, NULL))
            return NULL;
        return netsnmp_ssh_transport_with_source(ep, local, &src_addr, PF_INET);
    }

    /** no source and default client address ok */
    return netsnmp_ssh_transport(ep, local, PF_INET);
}

#ifdef NETSNMP_TRANSPORT_UDPIPV6_DOMAIN
static netsnmp_transport *
_tspec_v6(const struct netsnmp_ep *ep, netsnmp_tdomain_spec *tspec)
{
    int local = tspec->flags & NETSNMP_TSPEC_LOCAL;

    if (NULL != tspec->source) {
        struct netsnmp_ep src_addr;

        /** get sockaddr from source */
        if (!netsnmp_sockaddr_in6_3(&src_addr, tspec->source, NULL))
            return NULL;
        return netsnmp_ssh_transport_with_source(ep, local, &src_addr, PF_INET6);
    }

    /** no source and default client address ok */
    return netsnmp_ssh_transport(ep, local, PF_INET6);
}
#endif /* NETSNMP_TRANSPORT_UDPIPV6_DOMAIN */

netsnmp_transport *
netsnmp_ssh_create_tspec(netsnmp_tdomain_spec *tspec)
{
    struct netsnmp_ep ep;
    int local;

    DEBUGMSGTL(("ssh:create", "from tspec\n"));

    if (NULL == tspec)
        return NULL;

    local = tspec->flags & NETSNMP_TSPEC_LOCAL;

    if (local) {
        return netsnmp_ssh_transport(NULL, local, PF_UNIX);
    }

    if (netsnmp_sockaddr_in3(&ep, tspec->target, tspec->default_target))
        return _tspec_v4(&ep, tspec);
#ifdef NETSNMP_TRANSPORT_UDPIPV6_DOMAIN
    else if (netsnmp_sockaddr_in6_3(&ep, tspec->target, tspec->default_target))
        return _tspec_v6(&ep, tspec);
#endif

    return NULL;
}

netsnmp_transport *
netsnmp_ssh_create_ostring(const void *o, size_t o_len, int local)
{
    struct netsnmp_ep ep;
    memset(&ep, 0, sizeof(ep));

    DEBUGMSGTL(("ssh:create", "from ostring\n"));

    if (local) {
        return netsnmp_ssh_transport(NULL, local, PF_UNIX);
    }

    /* XX: make order of IPv4/IPv6 configurable */
    if (netsnmp_ipv4_ostring_to_sockaddr(&ep.a.sin, o, o_len))
        return netsnmp_ssh_transport(&ep, local, PF_INET);
#ifdef NETSNMP_TRANSPORT_UDPIPV6_DOMAIN
    else if (netsnmp_ipv6_ostring_to_sockaddr(&ep.a.sin6, o, o_len))
        return netsnmp_ssh_transport(&ep, local, PF_INET6);
#endif
    else
        netsnmp_assert(0);
    return NULL;
}

void
sshdomain_parse_socket(const char *token, char *cptr)
{
    char *socket_perm, *socket_user, *socket_group;
    int uid = -1;
    int gid = -1;
    int s_perm = -1;
    char *st;

    DEBUGMSGTL(("ssh/config", "parsing socket info: %s\n", cptr));
    socket_perm = strtok_r(cptr, " \t", &st);
    socket_user = strtok_r(NULL, " \t", &st);
    socket_group = strtok_r(NULL, " \t", &st);

    if (socket_perm) {
        s_perm = strtol(socket_perm, NULL, 8);
        netsnmp_ds_set_int(NETSNMP_DS_LIBRARY_ID,
                           NETSNMP_DS_SSHDOMAIN_SOCK_PERM, s_perm);
        DEBUGMSGTL(("ssh/config", "socket permissions: %o (%d)\n",
                    s_perm, s_perm));
    }
    /*
     * Try to handle numeric UIDs or user names for the socket owner
     */
    if (socket_user) {
        uid = netsnmp_str_to_uid(socket_user);
        if ( uid != 0 )
            netsnmp_ds_set_int(NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_SSHDOMAIN_SOCK_USER, uid);
        DEBUGMSGTL(("ssh/config", "socket owner: %s (%d)\n",
                    socket_user, uid));
    }

    /*
     * and similarly for the socket group ownership
     */
    if (socket_group) {
        gid = netsnmp_str_to_gid(socket_group);
        if ( gid != 0 )
            netsnmp_ds_set_int(NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_SSHDOMAIN_SOCK_GROUP, gid);
        DEBUGMSGTL(("ssh/config", "socket group: %s (%d)\n",
                    socket_group, gid));
    }
}

void
netsnmp_ssh_ctor(void)    
{
    sshDomain.name = netsnmp_snmpSSHDomain;
    sshDomain.name_length = netsnmp_snmpSSHDomain_len;
    sshDomain.prefix = calloc(2, sizeof(char *));
    if (!sshDomain.prefix) {
        snmp_log(LOG_ERR, "calloc() failed - out of memory\n");
        return;
    }
    sshDomain.prefix[0] = "ssh";

    sshDomain.f_create_from_tstring_new = netsnmp_ssh_create_tstring;
    sshDomain.f_create_from_tspec       = netsnmp_ssh_create_tspec;
    sshDomain.f_create_from_ostring     = netsnmp_ssh_create_ostring;

    register_config_handler("snmp", "sshtosnmpsocketperms",
                            &sshdomain_parse_socket, NULL,
                            "socketperms [username [groupname]]");

    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "sshtosnmpsocket",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_SSHTOSNMP_SOCKET);

    netsnmp_ds_register_config(ASN_BOOLEAN, "snmp", "sshagent",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_SSH_AGENT);

    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "sshusername",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_SSH_USERNAME);

    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "sshpublickey",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_SSH_PUBKEY);

    netsnmp_ds_register_config(ASN_OCTET_STR, "snmp", "sshprivatekey",
                               NETSNMP_DS_LIBRARY_ID,
                               NETSNMP_DS_LIB_SSH_PRIVKEY);

    DEBUGMSGTL(("ssh", "registering the ssh domain\n"));
    netsnmp_tdomain_register(&sshDomain);
}


