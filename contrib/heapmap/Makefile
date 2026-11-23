# contrib/heapmap/Makefile

MODULE_big = heapmap
OBJS = \
	$(WIN32RES) \
	hm_am.o \
	hm_build.o \
	hm_exc.o \
	hm_insert.o \
	hm_model.o \
	hm_scan.o

EXTENSION = heapmap
DATA = heapmap--1.0.sql
PGFILEDESC = "heapmap access method - learned block-range index"
SHLIB_LINK += -lm
# -ffp-contract=off: build, insert and scan must compute identical predictions
PG_CFLAGS = -Wall -Wno-unused-parameter -ffp-contract=off

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/heapmap
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# every file shares the page layouts in heapmap.h
$(OBJS): heapmap.h
