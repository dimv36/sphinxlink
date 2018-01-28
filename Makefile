MODULE_big = sphinxlink
OBJS = sphinxlink.o

EXTENSION = sphinxlink
DATA = sphinxlink--1.0.sql

MYSQL_CONFIG = mysql_config
PG_CPPFLAGS := $(shell $(MYSQL_CONFIG) --include)
SHLIB_LINK += $(shell $(MYSQL_CONFIG) --libs)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
ifndef MAJORVERSION
MAJORVERSION := $(basename $(VERSION))
endif

else
subdir = contrib/sphinxlink
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
