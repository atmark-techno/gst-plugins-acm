# Note: plugindir is set in configure

##############################################################################
# TODO: change libgstplugin.la to something else, e.g. libmysomething.la     #
##############################################################################
plugin_LTLIBRARIES = \
	libgstrtov4l2.la \
	libgstrtoaacdec.la \
	libgstrtoh264dec.la \
	libgstrtofbdevsink.la \
	libgstacmalsasink.la
	

##############################################################################
# TODO: for the next set of variables, name the prefix if you named the .la, #
#  e.g. libmysomething.la => libmysomething_la_SOURCES                       #
#                            libmysomething_la_CFLAGS                        #
#                            libmysomething_la_LIBADD                        #
#                            libmysomething_la_LDFLAGS                       #
##############################################################################

# sources used to compile this plug-in
libgstrtov4l2_la_SOURCES = \
	gstv4l2bufferpool.h gstv4l2bufferpool.c \
	v4l2_util.h v4l2_util.c \
	gstrtodmabufmeta.h gstrtodmabufmeta.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstrtov4l2_la_CFLAGS = $(GST_CFLAGS)
libgstrtov4l2_la_LIBADD = $(GST_LIBS)
libgstrtov4l2_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstrtov4l2_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = \
	gstv4l2bufferpool.h \
	v4l2_util.h \
	gstrtodmabufmeta.h



# sources used to compile this plug-in
libgstrtoaacdec_la_SOURCES = \
	gstrtoaacdec.h gstrtoaacdec.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstrtoaacdec_la_CFLAGS = $(GST_CFLAGS)
libgstrtoaacdec_la_LIBADD = $(GST_LIBS)
libgstrtoaacdec_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS) -lgstaudio-1.0 -lgstrtov4l2
libgstrtoaacdec_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS += \
	gstrtoaacdec.h



# sources used to compile this plug-in
libgstrtoh264dec_la_SOURCES = \
	gstrtoh264dec.h gstrtoh264dec.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstrtoh264dec_la_CFLAGS = $(GST_CFLAGS)
libgstrtoh264dec_la_LIBADD = $(GST_LIBS)
libgstrtoh264dec_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS) -lgstvideo-1.0 -lgstcodecparsers-1.0 -lgstrtov4l2
libgstrtoh264dec_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS += gstrtoh264dec.h



# sources used to compile this plug-in
libgstrtofbdevsink_la_SOURCES = \
	gstrtofbdevsink.h gstrtofbdevsink.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstrtofbdevsink_la_CFLAGS = $(GST_CFLAGS)
libgstrtofbdevsink_la_LIBADD = $(GST_LIBS)
libgstrtofbdevsink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS) -lgstvideo-1.0 -lgstrtov4l2
libgstrtofbdevsink_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS += gstrtofbdevsink.h



# sources used to compile this plug-in
libgstacmalsasink_la_SOURCES = \
	acmalsasink/gstalsadeviceprobe.c \
	acmalsasink/gstalsasink.c 	\
	acmalsasink/gstalsa.c \
	acmalsasink/gstacmalsasink.c

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstacmalsasink_la_CFLAGS = $(GST_CFLAGS)
libgstacmalsasink_la_LIBADD = $(GST_LIBS)
libgstacmalsasink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS) -lgstaudio-1.0 -lgstrtov4l2  -lasound -lm -ldl -lpthread
libgstacmalsasink_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS += \
	acmalsasink/gstalsa.h \
	acmalsasink/gstalsadeviceprobe.h \
	acmalsasink/gstalsasink.h \
	acmalsasink/gstacmalsasink.h


