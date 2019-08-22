/* SSDP responder
 *
 * Copyright (c) 2017  Joachim Nilsson <troglobit@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.a
 */

#include <config.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <paths.h>
#include <poll.h>
#include <stdio.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/if.h>
#include <sys/param.h>		/* MIN() */
#include <sys/socket.h>

#include "ssdp.h"
#include "queue.h"

struct ifsock {
	LIST_ENTRY(ifsock) link;

	int stale;
	int mod;

	/*
	 * Sockets for inbound and outbound
	 *
	 * - The inbound is the multicast socket, shared between all ifaces
	 * - The outbound is bound to the iface address and a random port
	 */
	int in, out;

	/* Interface address and netmask */
	struct sockaddr_storage addr;
	struct sockaddr_in mask;

	void (*cb)(int);
};

LIST_HEAD(, ifsock) il = LIST_HEAD_INITIALIZER();

static char *supported_types[] = {
	SSDP_ST_ALL,
	"upnp:rootdevice",
	DEVICE_TYPE,
	uuid,
	NULL
};

int      debug = 0;
int      running = 1;

char uuid[42];
char hostname[64];
char *os = NULL, *ver = NULL;
char server_string[64] = "POSIX UPnP/1.0 " PACKAGE_NAME "/" PACKAGE_VERSION;

/* Find interface in same subnet as sa */
static struct ifsock *find_outbound(struct sockaddr *sa)
{
	in_addr_t cand;
	struct ifsock *ifs;
	struct sockaddr_in *addr = (struct sockaddr_in *)sa;

	cand = addr->sin_addr.s_addr;
	LIST_FOREACH(ifs, &il, link) {
		in_addr_t a, m;

		const struct sockaddr_in *addr = (struct sockaddr_in *) &ifs->addr;
		const struct sockaddr_in *mask = (struct sockaddr_in *) &ifs->mask;
		a = addr->sin_addr.s_addr;
		m = mask->sin_addr.s_addr;
		if (a == htonl(INADDR_ANY) || m == htonl(INADDR_ANY))
			continue;

		if ((a & m) == (cand & m)) {
			return ifs;
		}
	}

	return NULL;
}

static struct ifsock *find_outbound6(struct sockaddr *sa)
{
	struct in6_addr cand;
	struct ifsock *ifs;
	struct sockaddr_in6 *addr = (struct sockaddr_in6 *)sa;

	cand = addr->sin6_addr;
	LIST_FOREACH(ifs, &il, link) {
		in_addr_t a, m;

		const struct sockaddr_in6 *addr = (struct sockaddr_in6 *) &ifs->addr;

		if (memcmp(&addr->sin6_addr, &in6addr_any, sizeof(struct in6_addr)) == 0)
			continue;

		if (memcmp(&addr->sin6_addr, &cand, sizeof(struct in6_addr)) == 0)
			return ifs;

		if (IN6_IS_ADDR_LINKLOCAL(&addr->sin6_addr))
			return ifs;
	}

	return NULL;
}

/* Exact match, must be same ifaddr as sa */
static struct ifsock *find_iface(struct sockaddr *sa)
{
	struct ifsock *ifs;

	if (!sa)
		return NULL;

	LIST_FOREACH(ifs, &il, link) {
		if (sa->sa_family == AF_INET) {
			struct sockaddr_in *addr = (struct sockaddr_in *) sa;
			const struct sockaddr_in *i_addr = (struct sockaddr_in *) &ifs->addr;
			if (i_addr->sin_addr.s_addr == addr->sin_addr.s_addr)
				return ifs;
		}
		else if (sa->sa_family == AF_INET6) {
			struct sockaddr_in6 *addr = (struct sockaddr_in6 *) sa;
			const struct sockaddr_in6 *i_addr = (struct sockaddr_in6 *) &ifs->addr;
			if (memcmp(&i_addr->sin6_addr, &i_addr->sin6_addr, sizeof(i_addr->sin6_addr)) == 0)
			/* ipv6 listen on in6addr_any */
			if (memcmp(&addr->sin6_addr, &i_addr->sin6_addr, sizeof(i_addr->sin6_addr)) == 0
				|| memcmp(&addr->sin6_addr, &in6addr_any, sizeof(i_addr->sin6_addr)) == 0)
				return ifs;
		}
	}

	return NULL;
}

int register_socket(int in, int out, struct sockaddr *addr, struct sockaddr *mask, void (*cb)(int sd))
{
	struct ifsock *ifs;
	struct sockaddr_in *address = (struct sockaddr_in *)addr;
	struct sockaddr_in *netmask = (struct sockaddr_in *)mask;

	ifs = calloc(1, sizeof(*ifs));
	if (!ifs) {
		char *host = inet_ntoa(address->sin_addr);

		logit(LOG_ERR, "Failed registering host %s socket: %s", host, strerror(errno));
		return -1;
	}

	ifs->in   = in;
	ifs->out  = out;
	ifs->mod  = 1;
	ifs->cb   = cb;
	ifs->addr = * (struct sockaddr_storage *) address;
	if (mask)
		ifs->mask = *netmask;
	LIST_INSERT_HEAD(&il, ifs, link);

	return 0;
}

static int open_socket(char *ifname, struct sockaddr *addr, int port)
{
	int sd, val, rc;

	if (addr->sa_family == AF_INET6) {
		struct ipv6_mreq mreq;
		struct sockaddr_in6 sin, *address = (struct sockaddr_in6 *)addr;
		char addr_string[INET6_ADDRSTRLEN];
		int ifid = if_nametoindex(ifname);

		inet_ntop(AF_INET6, &address->sin6_addr, addr_string, sizeof(addr_string));
		sd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		if (sd < 0)
			return -1;

		ENABLE_SOCKOPT(sd, IPPROTO_IPV6, IPV6_V6ONLY);
		ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEADDR);
		ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEPORT);

		sin.sin6_family = AF_INET6;
		sin.sin6_port = htons(port);
		sin.sin6_addr = address->sin6_addr;
		sin.sin6_scope_id = ifid;

		if (bind(sd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
			close(sd);
			logit(LOG_ERR, "Failed binding to [%s]:%d: %s", addr_string, port, strerror(errno));
			return -1;
		}

		memset(&mreq, 0, sizeof(mreq));
		inet_pton(AF_INET6, MC_SSDP_GROUP_IPV6, &mreq.ipv6mr_multiaddr);
		mreq.ipv6mr_interface = ifid;

		if (setsockopt(sd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq))) {
			close(sd);
			logit(LOG_ERR, "Failed joining group %s: %s", MC_SSDP_GROUP_IPV6, strerror(errno));
			return -1;
		}
		DISABLE_SOCKOPT(sd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP);

		rc = setsockopt(sd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifid, sizeof(ifid));
		if (rc < 0) {
			close(sd);
			logit(LOG_ERR, "Failed setting multicast interface: %s", strerror(errno));
			return -1;
		}

		logit(LOG_DEBUG, "Adding new interface %s with address %s", ifname, addr_string);
	}
	else {
		struct ip_mreqn mreq;
		struct sockaddr_in sin, *address = (struct sockaddr_in *)addr;

		sd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		if (sd < 0)
			return -1;

		ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEADDR);
		ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEPORT);

		sin.sin_family = AF_INET;
		sin.sin_port = htons(port);
		sin.sin_addr = address->sin_addr;
		if (bind(sd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
			close(sd);
			logit(LOG_ERR, "Failed binding to %s:%d: %s", inet_ntoa(address->sin_addr), port, strerror(errno));
			return -1;
		}
#if 0
		ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEADDR);
#ifdef SO_REUSEPORT
		ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEPORT);
#endif
#endif
		memset(&mreq, 0, sizeof(mreq));
		mreq.imr_address = address->sin_addr;
		mreq.imr_multiaddr.s_addr = inet_addr(MC_SSDP_GROUP);

		if (setsockopt(sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq))) {
			close(sd);
			logit(LOG_ERR, "Failed joining group %s: %s", MC_SSDP_GROUP, strerror(errno));
			return -1;
		}

		val = 2;		/* Default 2, but should be configurable */
		rc = setsockopt(sd, IPPROTO_IP, IP_MULTICAST_TTL, &val, sizeof(val));
		if (rc < 0) {
			close(sd);
			logit(LOG_ERR, "Failed setting multicast TTL: %s", strerror(errno));
			return -1;
		}

		DISABLE_SOCKOPT(sd, IPPROTO_IP, IP_MULTICAST_LOOP);

		rc = setsockopt(sd, IPPROTO_IP, IP_MULTICAST_IF, &address->sin_addr, sizeof(address->sin_addr));
		if (rc < 0) {
			close(sd);
			logit(LOG_ERR, "Failed setting multicast interface: %s", strerror(errno));
			return -1;
		}
		logit(LOG_DEBUG, "Adding new interface %s with address %s", ifname, inet_ntoa(address->sin_addr));
	}


	return sd;
}

static int close_socket(void)
{
	int ret = 0;
	struct ifsock *ifs, *tmp;

	LIST_FOREACH_SAFE(ifs, &il, link, tmp) {
		LIST_REMOVE(ifs, link);
		if (ifs->out != -1)
			ret |= close(ifs->out);
		else
			ret |= close(ifs->in);
		free(ifs);
	}

	return ret;
}

static int filter_addr(struct sockaddr *sa)
{
	struct ifsock *ifs;

	if (!sa)
		return 1;

	if (sa->sa_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)sa;
		if (sin->sin_addr.s_addr == htonl(INADDR_ANY))
			return 1;

		if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
			return 1;

		ifs = find_outbound(sa);
		if (ifs) {
			const struct sockaddr_in *addr = (struct sockaddr_in *) &ifs->addr;
			if (addr->sin_addr.s_addr != htonl(INADDR_ANY))
				return 1;
		}
	}
	else if (sa->sa_family == AF_INET6) {
		struct sockaddr_in6 *sin = (struct sockaddr_in6 *)sa;

		if (memcmp(&sin->sin6_addr, &in6addr_any, sizeof(sin->sin6_addr)) == 0)
			return 1;

		if (memcmp(&sin->sin6_addr, &in6addr_loopback, sizeof(sin->sin6_addr)) == 0)
			return 1;

		if (!IN6_IS_ADDR_LINKLOCAL(&sin->sin6_addr))
			return 1;

		ifs = find_outbound6(sa);
		if (ifs) {
			const struct sockaddr_in6 *addr = (struct sockaddr_in6 *) &ifs->addr;
			if (memcmp(&addr->sin6_addr, &in6addr_any, sizeof(addr->sin6_addr)) != 0)
				return 1;
		}
	}
	else {
		return 1;
	}

	return 0;
}

static int filter_iface(char *ifname, char *iflist[], size_t num)
{
	size_t i;

	if (!num) {
		logit(LOG_DEBUG, "No interfaces to filter, using all with an IP address.");
		return 0;
	}

	logit(LOG_DEBUG, "Filter %s?  Comparing %zd entries ...", ifname, num);
	for (i = 0; i < num; i++) {
		logit(LOG_DEBUG, "Filter %s?  Comparing with %s ...", ifname, iflist[i]);
		if (!strcmp(ifname, iflist[i]))
			return 0;
	}

	return 1;
}

static void compose_addr(struct sockaddr_in *sin, char *group, int port)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin_family      = AF_INET;
	sin->sin_port        = htons(port);
	sin->sin_addr.s_addr = inet_addr(group);
}

static void compose_addr6(struct sockaddr_in6 *sin, char *group, int port)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin6_family      = AF_INET6;
	sin->sin6_port        = htons(port);
	inet_pton(AF_INET6, group, &sin->sin6_addr);
}

static void compose_response(char *type, char *host, char *buf, size_t len)
{
	char usn[256];
	char date[42];
	time_t now;

	/* RFC1123 date, as specified in RFC2616 */
	now = time(NULL);
	strftime(date, sizeof(date), "%a, %d %b %Y %T %Z", gmtime(&now));

	if (type) {
		if (!strcmp(type, uuid))
			type = NULL;
		else
			snprintf(usn, sizeof(usn), "%s::%s", uuid, type);
	}

	if (!type)
		strncpy(usn, uuid, sizeof(usn));

	snprintf(buf, len, "HTTP/1.1 200 OK\r\n"
		 "Server: %s\r\n"
		 "Date: %s\r\n"
		 "Location: http://%s:%d%s\r\n"
		 "ST: %s\r\n"
		 "EXT: \r\n"
		 "USN: %s\r\n"
		 "Cache-Control: max-age=%d\r\n"
		 "\r\n",
		 server_string,
		 date,
		 host, LOCATION_PORT, LOCATION_DESC,
		 type,
		 usn,
		 CACHE_TIMEOUT);
}

static void compose_search(char *type, char *buf, size_t len)
{
	snprintf(buf, len, "M-SEARCH * HTTP/1.1\r\n"
		 "Host: %s:%d\r\n"
		 "MAN: \"ssdp:discover\"\r\n"
		 "MX: 1\r\n"
		 "ST: %s\r\n"
		 "User-Agent: %s\r\n"
		 "\r\n",
		 MC_SSDP_GROUP, MC_SSDP_PORT,
		 type,
		 server_string);
}

static void compose_notify(char *type, char *host, char *buf, size_t len)
{
	char usn[256];

	if (type) {
		if (!strcmp(type, SSDP_ST_ALL))
			type = NULL;
		else
			snprintf(usn, sizeof(usn), "%s::%s", uuid, type);
	}

	if (!type) {
		type = usn;
		strncpy(usn, uuid, sizeof(usn));
	}

	snprintf(buf, len, "NOTIFY * HTTP/1.1\r\n"
		 "Host: %s:%d\r\n"
		 "Server: %s\r\n"
		 "Location: http://%s:%d%s\r\n"
		 "NT: %s\r\n"
		 "NTS: ssdp:alive\r\n"
		 "USN: %s\r\n"
		 "Cache-Control: max-age=%d\r\n"
		 "\r\n",
		 MC_SSDP_GROUP, MC_SSDP_PORT,
		 server_string,
		 host, LOCATION_PORT, LOCATION_DESC,
		 type,
		 usn,
		 CACHE_TIMEOUT);
}

size_t pktlen(unsigned char *buf)
{
	size_t hdr = sizeof(struct udphdr);

	return strlen((char *)buf + hdr) + hdr;
}

static void send_search(struct ifsock *ifs, char *type)
{
	ssize_t num;
	char buf[MAX_PKT_SIZE];
	struct sockaddr dest;

	memset(buf, 0, sizeof(buf));
	compose_search(type, buf, sizeof(buf));

	if (ifs->addr.ss_family == AF_INET)
		compose_addr((struct sockaddr_in *)&dest, MC_SSDP_GROUP, MC_SSDP_PORT);
	else if (ifs->addr.ss_family == AF_INET6)
		compose_addr6((struct sockaddr_in6 *)&dest, MC_SSDP_GROUP_IPV6, MC_SSDP_PORT);

	logit(LOG_DEBUG, "Sending M-SEARCH ...");
	num = sendto(ifs->out, buf, strlen(buf), 0, &dest, sizeof(struct sockaddr_storage));
	if (num < 0)
		logit(LOG_WARNING, "Failed sending SSDP M-SEARCH");
}

static void send_message(struct ifsock *ifs, char *type, struct sockaddr *sa)
{
	int s;
	size_t i, len, note = 0;
	ssize_t num;
	char host[NI_MAXHOST];
	char host6[NI_MAXHOST+2];
	char *host_out;
	char buf[MAX_PKT_SIZE];
	struct sockaddr_storage dest;

	gethostname(hostname, sizeof(hostname));
	s = getnameinfo((struct sockaddr *)&ifs->addr, sizeof(struct sockaddr_storage),
			host, sizeof(host), NULL, 0, NI_NUMERICHOST);
	if (s) {
		logit(LOG_WARNING, "Failed getnameinfo(): %s", gai_strerror(s));
		return;
	}

	if (ifs->addr.ss_family == AF_INET) {
		const struct sockaddr_in *addr = (struct sockaddr_in *) &ifs->addr;
		if (addr->sin_addr.s_addr == htonl(INADDR_ANY))
			return;
	}
	else if (ifs->addr.ss_family == AF_INET6) {
		const struct sockaddr_in6 *addr = (struct sockaddr_in6 *) &ifs->addr;
		if (memcmp(&addr->sin6_addr, &in6addr_any, sizeof(struct in6_addr)) == 0)
			return;
	}
	if (ifs->out == -1)
		return;

	if (!strcmp(type, SSDP_ST_ALL))
		type = NULL;

	host_out = host;
	if (ifs->addr.ss_family == AF_INET6) {
		char *pos = strchr(host, '%');
		if (pos) {
			*pos = '\0';
			snprintf(host6, NI_MAXHOST+2, "[%s]", host);
			host_out = host6;
		}
	}

	memset(buf, 0, sizeof(buf));
	if (sa)
		compose_response(type, host_out, buf, sizeof(buf));
	else
		compose_notify(type, host_out, buf, sizeof(buf));

	if (!sa) {
		note = 1;
		if (ifs->addr.ss_family == AF_INET)
			compose_addr((struct sockaddr_in *)&dest, MC_SSDP_GROUP, MC_SSDP_PORT);
		else if (ifs->addr.ss_family == AF_INET6)
			compose_addr6((struct sockaddr_in6 *)&dest, MC_SSDP_GROUP_IPV6, MC_SSDP_PORT);
		sa = (struct sockaddr *) &dest;
	}

	logit(LOG_DEBUG, "Sending %s from %s ...", !note ? "reply" : "notify", host);
	num = sendto(ifs->out, buf, strlen(buf), 0, sa, sizeof(struct sockaddr_storage));
	if (num < 0)
		logit(LOG_WARNING, "Failed sending SSDP %s, type: %s: %s", !note ? "reply" : "notify", type, strerror(errno));
}

static void ssdp_recv(int sd)
{
	ssize_t len;
	struct sockaddr_storage sa;
	socklen_t salen = sizeof(sa);
	char buf[MAX_PKT_SIZE];

	memset(buf, 0, sizeof(buf));
	len = recvfrom(sd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *) &sa, &salen);
	if (len > 0) {
		buf[len] = 0;

		if (sa.ss_family != AF_INET && sa.ss_family != AF_INET6)
			return;

		if (strstr(buf, "M-SEARCH *")) {
			size_t i;
			char *ptr, *type;
			struct ifsock *ifs = NULL;

			char addr[INET6_ADDRSTRLEN];
			int port = -1;
			if (sa.ss_family == AF_INET) {
				struct sockaddr_in *sin = (struct sockaddr_in *)&sa;

				ifs = find_outbound((struct sockaddr *) &sa);
				inet_ntop(AF_INET, &sin->sin_addr, addr, INET_ADDRSTRLEN);
				port = ntohs(sin->sin_port);
			}
			else if (sa.ss_family == AF_INET6) {
				struct sockaddr_in6 *sin = (struct sockaddr_in6 *) &sa;

				ifs = find_outbound6((struct sockaddr *) &sa);
				inet_ntop(AF_INET6, &sin->sin6_addr, addr, INET6_ADDRSTRLEN);
				port = ntohs(sin->sin6_port);
			}

			if (!ifs) {
				logit(LOG_DEBUG, "No matching socket for client %s", addr);
				return;
			}
			logit(LOG_DEBUG, "Matching socket for client %s", addr);

			type = strcasestr(buf, "\r\nST:");
			if (!type) {
				logit(LOG_DEBUG, "No Search Type (ST:) found in M-SEARCH *, assuming " SSDP_ST_ALL);
				type = SSDP_ST_ALL;
				send_message(ifs, type, (struct sockaddr *) &sa);
				return;
			}

			type = strchr(type, ':');
			if (!type)
				return;
			type++;
			while (isspace(*type))
				type++;

			ptr = strstr(type, "\r\n");
			if (!ptr)
				return;
			*ptr = 0;

			for (i = 0; supported_types[i]; i++) {
				if (!strcmp(supported_types[i], type)) {
					logit(LOG_DEBUG, "M-SEARCH * ST: %s from %s port %d", type,
					      addr, port);
					send_message(ifs, type, (struct sockaddr *) &sa);
					return;
				}
			}

			logit(LOG_DEBUG, "M-SEARCH * for unsupported ST: %s from %s", type, addr);
		}
	}
}

static int multicast_init(void)
{
	int sd;
	struct sockaddr sa;
	struct sockaddr_in *sin = (struct sockaddr_in *)&sa;

	sd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (sd < 0) {
		logit(LOG_ERR, "Failed opening multicast socket: %s", strerror(errno));
		return -1;
	}

	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = inet_addr(MC_SSDP_GROUP);
	sin->sin_port = htons(MC_SSDP_PORT);

	ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEADDR);
	ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEPORT);

	if (bind(sd, &sa, sizeof(sa)) < 0) {
		close(sd);
		logit(LOG_ERR, "Failed binding to %s:%d: %s", inet_ntoa(sin->sin_addr), MC_SSDP_PORT, strerror(errno));
		return -1;
	}

	register_socket(sd, -1, &sa, NULL, ssdp_recv);

	return sd;
}

static int multicast_init6(void)
{
	int sd, ret;
	struct sockaddr_in6 sin;

	sd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (sd < 0) {
		logit(LOG_ERR, "Failed opening multicast socket: %s", strerror(errno));
		return -1;
	}

	memset(&sin, 0, sizeof(sin));

	sin.sin6_family = AF_INET6;
	sin.sin6_port = htons(MC_SSDP_PORT);
	/* better listen on :: on all interfaces, we don't need sin6_scope then */
//	inet_pton(AF_INET6, MC_SSDP_GROUP_IPV6, &sin.sin6_addr);
	sin.sin6_addr = in6addr_any;

	ENABLE_SOCKOPT(sd, IPPROTO_IPV6, IPV6_V6ONLY);
	ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEADDR);
	ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEPORT);

	if (bind(sd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
		close(sd);
		logit(LOG_ERR, "Failed binding to [%s]:%d: %s", MC_SSDP_GROUP_IPV6, MC_SSDP_PORT, strerror(errno));
		return -1;
	}

	register_socket(sd, -1, (struct sockaddr *) &sin, NULL, ssdp_recv);

	return sd;
}

static int multicast_join(int sd, struct sockaddr *sa)
{
	struct ip_mreqn mreq;
	struct sockaddr_in *sin = (struct sockaddr_in *)sa;

	memset(&mreq, 0, sizeof(mreq));
	mreq.imr_address = sin->sin_addr;
	mreq.imr_multiaddr.s_addr = inet_addr(MC_SSDP_GROUP);
        if (setsockopt(sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq))) {
		if (EADDRINUSE == errno)
			return 0;

		logit(LOG_ERR, "Failed joining group %s: %s", MC_SSDP_GROUP, strerror(errno));
		return -1;
	}

	return 0;
}

static int multicast_join6(int sd, struct sockaddr *sa, char *name)
{
	struct ipv6_mreq mreq;
	struct sockaddr_in *sin = (struct sockaddr_in *)sa;

	memset(&mreq, 0, sizeof(mreq));
	inet_pton(AF_INET6, MC_SSDP_GROUP_IPV6, &mreq.ipv6mr_multiaddr);

	if (setsockopt(sd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq))) {
		close(sd);
		logit(LOG_ERR, "Failed joining group %s: %s", MC_SSDP_GROUP_IPV6, strerror(errno));
		return -1;
	}

	return 0;
}

static void mark(void)
{
	struct ifsock *ifs;

	LIST_FOREACH(ifs, &il, link) {
		if (ifs->out != -1)
			ifs->stale = 1;
		else
			ifs->stale = 0;
	}
}

static int sweep(void)
{
	int modified = 0;
	struct ifsock *ifs, *tmp;

	LIST_FOREACH_SAFE(ifs, &il, link, tmp) {
		if (!ifs->stale)
			continue;

		modified++;
		char str[INET6_ADDRSTRLEN];
		if (ifs->addr.ss_family == AF_INET) {
			const struct sockaddr_in *addr = (struct sockaddr_in *) &ifs->addr;
			inet_ntop(AF_INET, &addr->sin_addr, str, INET_ADDRSTRLEN);
		}
		else if (ifs->addr.ss_family == AF_INET6) {
			const struct sockaddr_in6 *addr = (struct sockaddr_in6 *) &ifs->addr;
			inet_ntop(AF_INET6, &addr->sin6_addr, str, INET6_ADDRSTRLEN);
		}
		logit(LOG_DEBUG, "Removing stale ifs %s", str);

		LIST_REMOVE(ifs, link);
		close(ifs->out);
		free(ifs);
	}

	return modified;
}

static int ssdp_init(int in, int in6, char *iflist[], size_t num)
{
	int modified;
	size_t i;
	struct ifaddrs *ifaddrs, *ifa;

	logit(LOG_INFO, "Updating interfaces ...");

	if (getifaddrs(&ifaddrs) < 0) {
		logit(LOG_ERR, "Failed getifaddrs(): %s", strerror(errno));
		return -1;
	}

	/* Mark all outbound interfaces as stale */
	mark();

	/* First pass, clear stale marker from exact matches */
	for (ifa = ifaddrs; ifa; ifa = ifa->ifa_next) {
		struct ifsock *ifs;

		char addr[128];
		if (ifa->ifa_addr->sa_family == AF_INET) {
			// create IPv4 string
			struct sockaddr_in *in = (struct sockaddr_in*) ifa->ifa_addr;
			inet_ntop(AF_INET, &in->sin_addr, addr, sizeof(addr));
		} else { // AF_INET6
			// create IPv6 string
			struct sockaddr_in6 *in6 = (struct sockaddr_in6*) ifa->ifa_addr;
			inet_ntop(AF_INET6, &in6->sin6_addr, addr, sizeof(addr));
		}
		/* Do we already have it? */
		ifs = find_iface(ifa->ifa_addr);
		if (ifs) {
			ifs->stale = 0;
			continue;
		}
	}

	/* Clean out any stale interface addresses */
	modified = sweep();

	/* Second pass, add new ones */
	for (ifa = ifaddrs; ifa; ifa = ifa->ifa_next) {
		int sd;

		/* Interface filtering, optional command line argument */
		if (filter_iface(ifa->ifa_name, iflist, num)) {
			logit(LOG_DEBUG, "Skipping %s, not in iflist.", ifa->ifa_name);
			continue;
		}

		/* Do we have another in the same subnet? */
		if (filter_addr(ifa->ifa_addr))
			continue;

		sd = open_socket(ifa->ifa_name, ifa->ifa_addr, MC_SSDP_PORT);
		if (sd < 0)
			continue;

		if (ifa->ifa_addr->sa_family == AF_INET) {
			multicast_join(in, ifa->ifa_addr);

			if (register_socket(in, sd, ifa->ifa_addr, ifa->ifa_netmask, ssdp_recv)) {
				close(sd);
				break;
			}
		}
		else if (ifa->ifa_addr->sa_family == AF_INET6) {
			multicast_join6(in6, ifa->ifa_addr, NULL);

			if (register_socket(in6, sd, ifa->ifa_addr, ifa->ifa_netmask, ssdp_recv)) {
				close(sd);
				break;
			}
		}
		modified++;
	}

	freeifaddrs(ifaddrs);

	return modified;
}

static void handle_message(int sd)
{
	struct ifsock *ifs;

	LIST_FOREACH(ifs, &il, link) {
		if (ifs->in != sd)
			continue;

		if (ifs->cb)
			ifs->cb(sd);
	}
}

static void wait_message(time_t tmo)
{
	int num = 1, timeout;
	size_t ifnum = 0;
	struct pollfd pfd[MAX_NUM_IFACES];
	struct ifsock *ifs;

	LIST_FOREACH(ifs, &il, link) {
		if (ifs->out != -1)
			continue;

		pfd[ifnum].fd = ifs->in;
		pfd[ifnum].events = POLLIN | POLLHUP;
		ifnum++;
	}

	while (1) {
		size_t i;

		timeout = tmo - time(NULL);
		if (timeout < 0)
			break;

		num = poll(pfd, ifnum, timeout * 1000);
		if (num < 0) {
			if (EINTR == errno)
				break;

			err(1, "Unrecoverable error");
		}

		if (num == 0)
			break;

		for (i = 0; num > 0 && i < ifnum; i++) {
			if (pfd[i].revents & POLLIN) {
				handle_message(pfd[i].fd);
				num--;
			}
		}
	}
}

static void announce(int mod)
{
	struct ifsock *ifs;

	logit(LOG_INFO, "Sending SSDP NOTIFY new:%d ...", mod);

	LIST_FOREACH(ifs, &il, link) {
		size_t i;

		if (mod && !ifs->mod)
			continue;
		ifs->mod = 0;

//		send_search(ifs, "upnp:rootdevice");
		for (i = 0; supported_types[i]; i++) {
			/* UUID sent in SSDP_ST_ALL, first announce */
			if (!strcmp(supported_types[i], uuid))
				continue;

			send_message(ifs, supported_types[i], NULL);
		}
	}
}

static void lsb_init(void)
{
	FILE *fp;
	char *ptr;
	char line[80];
	const char *file = "/etc/lsb-release";

	fp = fopen(file, "r");
	if (!fp) {
	fallback:
		logit(LOG_WARNING, "No %s found on system, using built-in server string.", file);
		return;
	}

	while (fgets(line, sizeof(line), fp)) {
		line[strlen(line) - 1] = 0;

		ptr = strstr(line, "DISTRIB_ID");
		if (ptr && (ptr = strchr(ptr, '=')))
			os = strdup(++ptr);

		ptr = strstr(line, "DISTRIB_RELEASE");
		if (ptr && (ptr = strchr(ptr, '=')))
			ver = strdup(++ptr);
	}
	fclose(fp);

	if (os && ver)
		snprintf(server_string, sizeof(server_string), "%s/%s UPnP/1.0 %s/%s",
			 os, ver, PACKAGE_NAME, PACKAGE_VERSION);
	else
		goto fallback;

	logit(LOG_DEBUG, "Server: %s", server_string);
}

/* https://en.wikipedia.org/wiki/Universally_unique_identifier */
static void uuidgen(void)
{
	FILE *fp;
	char buf[42];
	const char *file = _PATH_VARDB PACKAGE_NAME ".cache";

	fp = fopen(file, "r");
	if (!fp) {
		fp = fopen(file, "w");
		if (!fp)
			logit(LOG_WARNING, "Cannot create UUID cache, %s: %s", file, strerror(errno));

	generate:
		srand(time(NULL));
		snprintf(buf, sizeof(buf), "uuid:%8.8x-%4.4x-%4.4x-%4.4x-%6.6x%6.6x",
			 rand() & 0xFFFFFFFF,
			 rand() & 0xFFFF,
			 (rand() & 0x0FFF) | 0x4000, /* M  4 MSB version => version 4 */
			 (rand() & 0x1FFF) | 0x8000, /* N: 3 MSB variant => variant 1 */
			 rand() & 0xFFFFFF, rand() & 0xFFFFFF);

		if (fp) {
			logit(LOG_DEBUG, "Creating new UUID cache file, %s", file);
			fprintf(fp, "%s\n", buf);
			fclose(fp);
		}
	} else {
		if (!fgets(buf, sizeof(buf), fp)) {
			fclose(fp);
			goto generate;
		}
		buf[strlen(buf) - 1] = 0;
		fclose(fp);
	}

	strcpy(uuid, buf);
	logit(LOG_DEBUG, "URN: %s", uuid);
}

static void exit_handler(int signo)
{
	(void)signo;
	running = 0;
}

static void signal_init(void)
{
	signal(SIGTERM, exit_handler);
	signal(SIGINT,  exit_handler);
	signal(SIGHUP,  exit_handler);
	signal(SIGQUIT, exit_handler);
}

static int usage(int code)
{
	printf("Usage: %s [-dhv] [-i SEC] [IFACE [IFACE ...]]\n"
	       "\n"
	       "    -d        Developer debug mode\n"
	       "    -h        This help text\n"
	       "    -i SEC    SSDP notify interval (30-900), default %d sec\n"
	       "    -r SEC    Interface refresh interval (5-1800), default %d sec\n"
	       "    -v        Show program version\n"
	       "\n"
	       "Bug report address: %-40s\n", PACKAGE_NAME, NOTIFY_INTERVAL, REFRESH_INTERVAL, PACKAGE_BUGREPORT);

	return code;
}

int main(int argc, char *argv[])
{
	int i, c, sd, sd6;
	int log_level = LOG_NOTICE;
	int log_opts = LOG_CONS | LOG_PID;
	int interval = NOTIFY_INTERVAL;
	int refresh = REFRESH_INTERVAL;
	time_t now, rtmo = 0, itmo = 0;

	while ((c = getopt(argc, argv, "dhi:r:v")) != EOF) {
		switch (c) {
		case 'd':
			debug = 1;
			break;

		case 'h':
			return usage(0);

		case 'i':
			interval = atoi(optarg);
			if (interval < 30 || interval > 900)
				errx(1, "Invalid announcement interval (30-900).");
			break;

		case 'r':
			refresh = atoi(optarg);
			if (refresh < 5 || refresh > 1800)
				errx(1, "Invalid refresh interval (5-1800).");
			break;

		case 'v':
			puts(PACKAGE_VERSION);
			return 0;

		default:
			break;
		}
	}

	signal_init();

        if (debug) {
		log_level = LOG_DEBUG;
                log_opts |= LOG_PERROR;
	}

        openlog(PACKAGE_NAME, log_opts, LOG_DAEMON);
        setlogmask(LOG_UPTO(log_level));

	uuidgen();
	lsb_init();
	web_init();

	sd = multicast_init();
	if (sd < 0)
		err(1, "Failed creating multicast socket");

	sd6 = multicast_init6();
	if (sd6 < 0)
		err(1, "Failed creating multicast socket");

	while (running) {
		now = time(NULL);

		if (rtmo <= now) {
			if (ssdp_init(sd, sd6, &argv[optind], argc - optind) > 0)
				announce(1);
			rtmo = now + refresh;
		}

		if (itmo <= now) {
			announce(0);
			itmo = now + interval;
		}

		wait_message(MIN(rtmo, itmo));
	}

	closelog();
	return close_socket();
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
