[![License Badge][]][License] [![Travis Status][]][Travis]

Simple Service Discovery Protocol daemon (SSDP) for networked Linux
devices.  Useful for small and embedded systems that want to announce
themselves to systems running Windows.

`ssdpd` is a stand-alone UNIX daemon with no external dependencies but
the standard C library.  It has a built-in web server for serving the
UPnP XML description which Windows use to present the icon, by default
an InternetGatewayDevice is announced.


Usage
-----

```
Usage: ssdpd [-dhv] [-i SEC] [IFACE [IFACE ...]]

    -d        Developer debug mode
    -h        This help text
    -i SEC    SSDP notify interval (30-900), default 300 sec
    -r SEC    Interface refresh interval (5-1800), default 600 sec
    -v        Show program version

Bug report address: https://github.com/troglobit/ssdp-responder/issues
```

See `configure --help` for some build time options.


Example
-------

The following example assumes the system `eth0` interface is connected
to an ISP and `eth1` to the LAN.  Every 300 sec the list of addresses
for `eth1` are updated, if a new address is added a `NOTIFY *` message
is sent, otherwise `NOTIFY *` messages are sent every 30 seconds.

```
ssdpd -i 30 -r 300 eth1
```


Origin
------

Cloned from [mrdisc](https://github.com/troglobit/mrdisc) and whacked at
with a bat until it replies to SSDP "MSEARCH *" messages used by Windows.

[License]:       https://en.wikipedia.org/wiki/ISC_license
[License Badge]: https://img.shields.io/badge/License-ISC-blue.svg
[Travis]:        https://travis-ci.org/troglobit/ssdp-responder
[Travis Status]: https://travis-ci.org/troglobit/ssdp-responder.png?branch=master
