EXTENSION    = pgch_test
DATA         = pgch_test--1.0.sql
MODULE_big   = pgch_test
OBJS         = pgch_test.o
REGRESS      = roundtrip types pgtype errors type_table
REGRESS_OPTS = --inputdir=. --load-extension=$(EXTENSION)
PG_CONFIG   ?= pg_config

# Vendored submodule. Override to build against a local clickhouse-c checkout
CH_C_DIR    ?= ../clickhouse-c

# Treat dependency headers as system headers to exclude dependency warnings
PG_CPPFLAGS  = -I.. -isystem $(CH_C_DIR) \
               -isystem $(shell $(PG_CONFIG) --includedir-server) \
               -DPGCH_MSG_PREFIX='"pgch: "'
PG_CFLAGS    = -Wall -Wsign-compare -Wno-declaration-after-statement

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

$(OBJS): ../pg-clickhouse.h ../pg-clickhouse-decode.h ../pg-clickhouse-encode.h \
         $(CH_C_DIR)/clickhouse.h

# Clone vendored submodule when absent. Spelled out rather than via CH_C_DIR:
# an override points at a checkout git knows nothing about
../clickhouse-c/clickhouse.h:
	git submodule update --init
