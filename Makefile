
MODULE_big   = kv_fdw

PG_CPPFLAGS += -Wno-declaration-after-statement
SHLIB_LINK   = -lrocksdb
OBJS         = src/kv_fdw.o src/kv.o

EXTENSION    = kv_fdw
DATA         = sql/kv_fdw--0.0.1.sql

# Users need to specify their own path
PG_CONFIG    = /usr/bin/pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

src/kv.bc:
	$(COMPILE.cxx.bc) $(CCFLAGS) $(CPPFLAGS) -fPIC -c -o $@ src/kv.cc
