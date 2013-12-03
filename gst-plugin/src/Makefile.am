# Note: plugindir is set in configure

##############################################################################
# TODO: change libgstplugin.la to something else, e.g. libmysomething.la     #
##############################################################################
plugin_LTLIBRARIES = \
	libgstacmv4l2.la \
	libgstacmaacdec.la \
	libgstacmh264dec.la \
	libgstacmfbdevsink.la \
	libgstacmalsasink.la \
	libgstacmaacenc.la \
	libgstacmh264enc.la
	

##############################################################################
# TODO: for the next set of variables, name the prefix if you named the .la, #
#  e.g. libmysomething.la => libmysomething_la_SOURCES                       #
#                            libmysomething_la_CFLAGS                        #
#                            libmysomething_la_LIBADD                        #
#                            libmysomething_la_LDFLAGS                       #
##############################################################################

# sources used to compile this plug-in
libgstacmv4l2_la_SOURCES = \
	gstacmv4l2bufferpool.h gstacmv4l2bufferpool.c \
	gstacmv4l2_util.h gstacmv4l2_util.c \
	gstacmdmabufmeta.h gstacmdmabufmeta.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstacmv4l2_la_CFLAGS = $(GST_CFLAGS)
libgstacmv4l2_la_LIBADD = $(GST_LIBS)
libgstacmv4l2_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstacmv4l2_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = \
	gstacmv4l2bufferpool.h \
	gstacmv4l2_util.h \
	gstacmdmabufmeta.h



# sources used to compile this plug-in
libgstacmaacdec_la_SOURCES = \
	gstacmaacdec.h gstacmaacdec.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstacmaacdec_la_CFLAGS = $(GST_CFLAGS)
libgstacmaacdec_la_LIBADD = $(GST_LIBS)
libgstacmaacdec_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS) -lgstaudio-1.0 -lgstacmv4l2
libgstacmaacdec_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS += \
	gstacmaacdec.h



# sources used to compile this plug-in
libgstacmh264dec_la_SOURCES = \
	gstacmh264dec.h gstacmh264dec.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstacmh264dec_la_CFLAGS = $(GST_CFLAGS)
libgstacmh264dec_la_LIBADD = $(GST_LIBS)
libgstacmh264dec_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS) -lgstvideo-1.0 -lgstcodecparsers-1.0 -lgstacmv4l2
libgstacmh264dec_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS += gstacmh264dec.h



# sources used to compile this plug-in
libgstacmfbdevsink_la_SOURCES = \
	gstacmfbdevsink.h gstacmfbdevsink.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstacmfbdevsink_la_CFLAGS = $(GST_CFLAGS)
libgstacmfbdevsink_la_LIBADD = $(GST_LIBS)
libgstacmfbdevsink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS) -lgstvideo-1.0 -lgstacmv4l2
libgstacmfbdevsink_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS += gstacmfbdevsink.h



# sources used to compile this plug-in
libgstacmalsasink_la_SOURCES = \
	acmalsasink/gstalsadeviceprobe.c \
	acmalsasink/gstalsasink.c 	\
	acmalsasink/gstalsa.c \
	acmalsasink/gstacmalsasink.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstacmalsasink_la_CFLAGS = $(GST_CFLAGS)
libgstacmalsasink_la_LIBADD = $(GST_LIBS)
libgstacmalsasink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS) -lgstaudio-1.0 -lgstacmv4l2  -lasound -lm -ldl -lpthread
libgstacmalsasink_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS += \
	acmalsasink/gstalsa.h \
	acmalsasink/gstalsadeviceprobe.h \
	acmalsasink/gstalsasink.h \
	acmalsasink/gstacmalsasink.h


#
# acmaacenc
#
# sources used to compile this plug-in
libgstacmaacenc_la_SOURCES = \
	gstacmaacenc.h gstacmaacenc.c \
	gstacm_util.h gstacm_util.c \
	gstacm_debug.h gstacm_debug.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstacmaacenc_la_CFLAGS = $(GST_CFLAGS)
libgstacmaacenc_la_LIBADD = $(GST_LIBS)
libgstacmaacenc_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS) -lgstaudio-1.0 -lgstacmv4l2
libgstacmaacenc_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS += \
	gstacmaacenc.h gstacm_util.h gstacm_debug.h


#
# acmh264enc
#
# sources used to compile this plug-in
libgstacmh264enc_la_SOURCES = \
	gstacmh264enc.h gstacmh264enc.c \
	gstacm_util.h gstacm_util.c \
	gstacm_debug.h gstacm_debug.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstacmh264enc_la_CFLAGS = $(GST_CFLAGS)
libgstacmh264enc_la_LIBADD = $(GST_LIBS)
libgstacmh264enc_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS) -lgstvideo-1.0 -lgstacmv4l2 -lgstpbutils-1.0
libgstacmh264enc_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS += \
	gstacmh264enc.h


CPPFLAGS=-I ../include
