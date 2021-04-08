libraries += libutil

libutil_NAME = libnixutil

libutil_DIR := $(d)

libutil_SOURCES := $(wildcard $(d)/*.cc)

libutil_LDFLAGS = $(LIBLZMA_LIBS) -lbz2 -pthread $(OPENSSL_LIBS) $(LIBBROTLI_LIBS) $(LIBARCHIVE_LIBS) $(BOOST_LDFLAGS) $(BOOST_CONTEXT_LDFLAG)

ifeq ($(HAVE_LIBCPUID), 1)
	libutil_LDFLAGS += -lcpuid
endif
