MODULE_big = sphinxlink
OBJS = sphinxlink.o

EXTENSION = sphinxlink
DATA = sphinxlink--1.2--1.3.sql sphinxlink--1.3.sql

ifeq ($(MYSQL_CONFIG),)
    _MYSQL_CONFIG = mysql_config
else
    _MYSQL_CONFIG = $(MYSQL_CONFIG)
endif

PG_CPPFLAGS := $(shell $(_MYSQL_CONFIG) --include)
SHLIB_LINK += $(shell $(_MYSQL_CONFIG) --libs)

ifdef USE_PGXS
    PG_CONFIG = pg_config
    PGXS := $(shell $(PG_CONFIG) --pgxs)
    include $(PGXS)
else
    subdir = contrib/sphinxlink
    top_builddir = ../..
    include $(top_builddir)/src/Makefile.global
    include $(top_srcdir)/contrib/contrib-global.mk
endif
