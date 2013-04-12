# debug
if DEBUG
DEBUG_CFLAGS = -Wextra -D_DEBUG
else
DEBUG_CFLAGS =
endif

# SW decoder
if SWDECODER
EXTRA_CFLAGS = -D_USE_SW_DECODER
else
EXTRA_CFLAGS =
endif

# name of your binary
bin_PROGRAMS = test_player

# list of source files
# the prefix is the name of the binary
test_player_SOURCES = main.c tp_config.c tp_cui.c tp_detect_media.c tp_gst_ctrl.c tp_property.c tp_term.c

# list of headers we're not going to install
noinst_HEADERS = tp_log_util.h tp_config.h tp_cui.h tp_detect_media.h tp_gst_ctrl.h tp_property.h tp_term.h

# our CFLAGS and LDFLAGS used for compiling and linking
# make sure you prefix these with the name of your binary
test_player_CFLAGS = $(GST_CFLAGS) $(DEBUG_CFLAGS) $(EXTRA_CFLAGS)
test_player_LDFLAGS = $(GST_LIBS)
