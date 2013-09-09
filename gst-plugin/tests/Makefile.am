# name of your binary
bin_PROGRAMS = acmaacdec acmh264dec acmfbdevsink



# list of source files
# the prefix is the name of the binary
acmaacdec_SOURCES = acmaacdec.c

# list of headers we're not going to install
noinst_HEADERS = 

# our CFLAGS and LDFLAGS used for compiling and linking
# make sure you prefix these with the name of your binary
acmaacdec_CFLAGS = $(GST_CFLAGS)
acmaacdec_LDFLAGS = $(GST_LIBS) -lgstcheck-1.0 -lm



# list of source files
# the prefix is the name of the binary
acmh264dec_SOURCES = acmh264dec.c

# list of headers we're not going to install
noinst_HEADERS += 

# our CFLAGS and LDFLAGS used for compiling and linking
# make sure you prefix these with the name of your binary
acmh264dec_CFLAGS = $(GST_CFLAGS)
acmh264dec_LDFLAGS = $(GST_LIBS) -lgstcheck-1.0 -lm



# list of source files
# the prefix is the name of the binary
acmfbdevsink_SOURCES = acmfbdevsink.c

# list of headers we're not going to install
noinst_HEADERS += 

# our CFLAGS and LDFLAGS used for compiling and linking
# make sure you prefix these with the name of your binary
acmfbdevsink_CFLAGS = $(GST_CFLAGS)
acmfbdevsink_LDFLAGS = $(GST_LIBS) -lgstcheck-1.0 -lm

