/* Micro web server for serving SSDP .xml file
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
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ssdp.h"

const char *xml =
	"<?xml version=\"1.0\"?>\r\n"
	"<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n"
	" <specVersion>\r\n"
	"   <major>1</major>\r\n"
	"   <minor>0</minor>\r\n"
	" </specVersion>\r\n"
	" <device>\r\n"
	"  <deviceType>" DEVICE_TYPE "</deviceType>\r\n"
	"  <friendlyName>%s</friendlyName>\r\n"
	"  <manufacturer>%s</manufacturer>\r\n%s"
	"  <modelName>%s</modelName>\r\n"
	"  <UDN>uuid:%s</UDN>\r\n"
	"  <presentationURL>http://%s</presentationURL>\r\n"
	" </device>\r\n"
	"</root>\r\n"
	"\r\n";


/* Peek into SOCK_STREAM on accepted client socket to figure out inbound interface */
static struct sockaddr *stream_peek(int sd, char *ifname)
{
	struct ifaddrs *ifaddr, *ifa;
	static struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);

	if (-1 == getsockname(sd, (struct sockaddr *) &ss, &len))
		return NULL;

	if (-1 == getifaddrs(&ifaddr))
		return NULL;

	for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
		size_t len = sizeof(struct in_addr);

		if (!ifa->ifa_addr)
			continue;

		if (ifa->ifa_addr->sa_family == AF_INET) {
			struct sockaddr_in *iin, *sin;
			iin = (struct sockaddr_in *) ifa->ifa_addr;
			sin = (struct sockaddr_in *) &ss;
			if (!memcmp(&sin->sin_addr, &iin->sin_addr, sizeof(struct sockaddr_in))) {
				strncpy(ifname, ifa->ifa_name, IF_NAMESIZE);
				break;
			}
		}
		else if (ifa->ifa_addr->sa_family == AF_INET6) {
			struct sockaddr_in6 *iin6, *sin6;
			iin6 = (struct sockaddr_in6 *) ifa->ifa_addr;
			sin6 = (struct sockaddr_in6 *) &ss;
			if (!memcmp(&sin6->sin6_addr, &iin6->sin6_addr, sizeof(struct sockaddr_in6))) {
				strncpy(ifname, ifa->ifa_name, IF_NAMESIZE);
				break;
			}
		}
		else {
			continue;
		}
	}

	freeifaddrs(ifaddr);

	return (struct sockaddr *) &ss;
}

static void respond(int sd, struct sockaddr *sin)
{
	char *head = "HTTP/1.1 200 OK\r\n"
		"Content-Type: text/xml\r\n"
		"Connection: close\r\n"
		"\r\n";
	char hostname[64], url[128] = "";
	char ip6[INET6_ADDRSTRLEN];
	char mesg[1024], *reqline[3];
	int rcvd, fd, bytes_read;
	struct sockaddr_in6 *sin6;

	memset(mesg, 0, sizeof(mesg));
	rcvd = recv(sd, mesg, sizeof(mesg), 0);
	if (rcvd <= 0) {
		logit(LOG_WARNING, "web recv() error: %s", strerror(errno));
		goto error;
	}

	logit(LOG_DEBUG, "%s", mesg);
	reqline[0] = strtok(mesg, " \t\n");
	if (strncmp(reqline[0], "GET", 4) == 0) {
		reqline[1] = strtok(NULL, " \t");
		reqline[2] = strtok(NULL, " \t\n");
		if (strncmp(reqline[2], "HTTP/1.0", 8) != 0 && strncmp(reqline[2], "HTTP/1.1", 8) != 0) {
			if (write(sd, "HTTP/1.1 400 Bad Request\r\n", 26) < 0)
				logit(LOG_WARNING, "Failed returning status 400 to client: %s", strerror(errno));
			goto error;
		}

		/* XXX: Add support for icon as well */
		if (!strstr(reqline[1], LOCATION_DESC)) {
			if (write(sd, "HTTP/1.1 404 Not Found\r\n", 24) < 0)
				logit(LOG_WARNING, "Failed returning status 404 to client: %s", strerror(errno));
			goto error;
		}

		gethostname(hostname, sizeof(hostname));
#ifdef MANUFACTURER_URL
		snprintf(url, sizeof(url), "  <manufacturerURL>%s</manufacturerURL>\r\n", MANUFACTURER_URL);
#endif
		logit(LOG_DEBUG, "Sending XML reply ...");
		send(sd, head, strlen(head), 0);

		sin6 = (struct sockaddr_in6 *) sin;
		if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
			struct in_addr *addr = ((struct in_addr *) (sin6->sin6_addr.s6_addr+12));
			inet_ntop(AF_INET, addr, ip6, sizeof(ip6));
		}
		else {
			inet_ntop(AF_INET6, &sin6->sin6_addr, ip6, sizeof(ip6));
		}
		snprintf(mesg, sizeof(mesg), xml,
			 hostname,
			 MANUFACTURER,
			 url,
			 MODEL,
			 uuid,
			 ip6);
		if (send(sd, mesg, strlen(mesg), 0) < 0)
			logit(LOG_WARNING, "Failed sending file to client: %s", strerror(errno));
	}

error:
	shutdown(sd, SHUT_RDWR);
	close(sd);
}

void web_recv(int sd)
{
	int client;
	char ifname[IF_NAMESIZE] = "UNKNOWN";
	struct sockaddr *sin;

	client = accept(sd, NULL, NULL);
	if (client < 0) {
		logit(LOG_ERR, "accept() error: %s", strerror(errno));
		return;
	}

	sin = stream_peek(client, ifname);
	if (!sin) {
		logit(LOG_ERR, "Failed resolving client interface: %s", strerror(errno));
		return;
	}

	respond(client, sin);
	shutdown(client, SHUT_RDWR);
	close(client);
}

void web_init4(void)
{
	int sd;
	struct sockaddr sa;
	struct sockaddr_in *sin;

	memset(&sa, 0, sizeof(sa));
	sa.sa_family = AF_INET;
	sin = (struct sockaddr_in *)&sa;
	sin->sin_addr.s_addr = htonl(INADDR_ANY);
	sin->sin_port = htons(LOCATION_PORT);

	sd = socket(sa.sa_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (sd == -1)
		err(1, "Failed creating web socket");

        ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEADDR);
#ifdef SO_REUSEPORT
        ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEPORT);
#endif

	if (bind(sd, &sa, sizeof(sa)) < 0)
		err(1, "Failed binding web socket");

	if (listen(sd, 10) != 0)
		err(1, "Failed setting web listen backlog");

	register_socket(sd, -1, &sa, NULL, web_recv);
}

void web_init6(void)
{
	int sd;
	struct sockaddr_in6 serveraddr;

	sd = socket(AF_INET6, SOCK_STREAM, 0);
	if (sd == -1)
		err(1, "Failed creating web socket");

	memset(&serveraddr, 0, sizeof(serveraddr));
	serveraddr.sin6_family = AF_INET6;
	serveraddr.sin6_port = htons(LOCATION_PORT);
	serveraddr.sin6_addr = in6addr_any;

	DISABLE_SOCKOPT(sd, IPPROTO_IPV6, IPV6_V6ONLY);
	ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEADDR);
#ifdef SO_REUSEPORT
	ENABLE_SOCKOPT(sd, SOL_SOCKET, SO_REUSEPORT);
#endif

	if (bind(sd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
		err(1, "Failed binding web socket");

	if (listen(sd, 10) != 0)
		err(1, "Failed setting web listen backlog");

	register_socket(sd, -1, (struct sockaddr *)&serveraddr, NULL, web_recv);
}

void web_init(void)
{
//	web_init4();
	web_init6();
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
