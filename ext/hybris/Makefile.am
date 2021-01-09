plugin_LTLIBRARIES = libgsthybrissink.la

libgsthybrissink_la_SOURCES = gsthybrissink.c mirpool.c
libgsthybrissink_la_CFLAGS = $(GST_CFLAGS) $(GST_BASE_CFLAGS) $(GST_PLUGINS_BASE_CFLAGS) \
                               $(UBUNTU_PLATFORM_CFLAGS) \
                               $(MIR_CFLAGS) \
                               -I../../gst-libs/
libgsthybrissink_la_LIBADD = $(GST_PLUGINS_BASE_LIBS) $(GST_BASE_LIBS) $(GST_LIBS) \
                               -lgstvideo-$(GST_API_VERSION) \
                               $(top_builddir)/gst-libs/gst/mir/libgstmiralloc-$(GST_API_VERSION).la \
                               -lmedia \
                               $(EGL_LIBS) $(EGLGLES_LIBS) \
                               $(MIR_LIBS)
libgsthybrissink_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgsthybrissink_la_LIBTOOLFLAGS = $(GST_PLUGIN_LIBTOOLFLAGS)
include_HEADERS = mirpool.h gsthybrissink.h
noinst_HEADERS =
