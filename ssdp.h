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
#ifndef SSDP_H_
#define SSDP_H_

#include <syslog.h>

/* Notify should be less than half the cache timeout */
#define NOTIFY_INTERVAL      300
#define REFRESH_INTERVAL     600
#define CACHE_TIMEOUT        1800
#define MAX_NUM_IFACES       100
#define MAX_PKT_SIZE         512
#define MC_SSDP_GROUP        "239.255.255.250"
#define MC_SSDP_GROUP_IPV6   "FF02::C"
#define MC_SSDP_PORT         1900
#define LOCATION_PORT        (MC_SSDP_PORT + 1)
#define LOCATION_DESC        "/description.xml"

#define SSDP_ST_ALL          "ssdp:all"

#define logit(lvl, fmt, args...) syslog(lvl, fmt, ##args)

#define SET_SOCKOPT(sd, level, opt, v)					\
        do {								\
                int val = v;						\
                if (setsockopt(sd, level, opt, &val, sizeof(val)) < 0)	\
                        warn("Failed enabling %s for web service", #opt); \
        } while (0);
#define ENABLE_SOCKOPT(sd, level, opt)	SET_SOCKOPT(sd, level, opt, 1)
#define DISABLE_SOCKOPT(sd, level, opt)	SET_SOCKOPT(sd, level, opt, 0)

extern int debug;
extern char uuid[];

void web_init(void);
int register_socket(int in, int out, struct sockaddr *addr, struct sockaddr *mask, void (*cb)(int sd));

#endif /* SSDP_H_ */
