# name of your binary
bin_PROGRAMS = rtoaacdec rtoh264dec rtofbdevsink



# list of source files
# the prefix is the name of the binary
rtoaacdec_SOURCES = rtoaacdec.c

# list of headers we're not going to install
noinst_HEADERS = 

# our CFLAGS and LDFLAGS used for compiling and linking
# make sure you prefix these with the name of your binary
rtoaacdec_CFLAGS = $(GST_CFLAGS)
rtoaacdec_LDFLAGS = $(GST_LIBS) -lgstcheck-1.0 -lm



# list of source files
# the prefix is the name of the binary
rtoh264dec_SOURCES = rtoh264dec.c

# list of headers we're not going to install
noinst_HEADERS += 

# our CFLAGS and LDFLAGS used for compiling and linking
# make sure you prefix these with the name of your binary
rtoh264dec_CFLAGS = $(GST_CFLAGS)
rtoh264dec_LDFLAGS = $(GST_LIBS) -lgstcheck-1.0 -lm



# list of source files
# the prefix is the name of the binary
rtofbdevsink_SOURCES = rtofbdevsink.c

# list of headers we're not going to install
noinst_HEADERS += 

# our CFLAGS and LDFLAGS used for compiling and linking
# make sure you prefix these with the name of your binary
rtofbdevsink_CFLAGS = $(GST_CFLAGS)
rtofbdevsink_LDFLAGS = $(GST_LIBS) -lgstcheck-1.0 -lm

