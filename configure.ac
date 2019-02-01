AC_INIT([ssdpd], [1.5], [https://github.com/troglobit/ssdp-responder/issues], [ssdp-responder])
AM_INIT_AUTOMAKE([1.11 foreign])

AC_CONFIG_SRCDIR([ssdpd.c])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_FILES([Makefile])

AC_PROG_CC
AC_PROG_INSTALL
AC_HEADER_STDC

AC_ARG_WITH([vendor],
	AS_HELP_STRING([--with-vendor=VENDOR], [Set a custom vendor string]),
	[vendor=$withval], [vendor="Troglobit Software Systems"])
AC_ARG_WITH([vendor-url],
	AS_HELP_STRING([--with-vendor-url=VENDOR], [Set vendor URL]))
AC_ARG_WITH([model],
	AS_HELP_STRING([--with-model=MODEL], [Set a custom model string]),
	[model=$withval], [model="Generic"])
AC_ARG_WITH([device-type],
	AS_HELP_STRING([--with-device-type=DEVICE_TYPE], [Set a custom device type]),
	[device_type=$withval], [device_type="urn:schemas-upnp-org:device:InternetGatewayDevice:1"])

AS_IF([test "x$with_vendor_url" != "xno" -a "x$with_vendor_url" != "xyes"], [
	AC_DEFINE_UNQUOTED(MANUFACTURER_URL, "$with_vendor_url", [Manufacturer URL in XML])])

AS_IF([test "x$with_vendor" != "xno"], [
	AS_IF([test "x$vendor" = "xyes"], [
		vendor="Troglobit Software Systems"])])
AC_DEFINE_UNQUOTED(MANUFACTURER, "$vendor", [Manufacturer in XML])

AS_IF([test "x$with_model" != "xno"], [
	AS_IF([test "x$model" = "xyes"], [
		model="Generic"])])
AC_DEFINE_UNQUOTED(MODEL, "$model", [Model name in XML])

AS_IF([test "x$with_device_type" != "xno"], [
	AS_IF([test "x$device_type" = "xyes"], [
		device_type="urn:schemas-upnp-org:device:InternetGatewayDevice:1"])])
AC_DEFINE_UNQUOTED(DEVICE_TYPE, "$device_type", [Device type name in XML])


AC_OUTPUT
