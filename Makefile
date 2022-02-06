MODULE_big = sphinxlink
OBJS = sphinxlink.o

EXTENSION = sphinxlink
DATA = sphinxlink--1.2--1.3.sql sphinxlink--1.3.sql

MYSQL_CONFIG = mysql_config
PG_CPPFLAGS := $(shell $(MYSQL_CONFIG) --include)
SHLIB_LINK += $(shell $(MYSQL_CONFIG) --libs)

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
