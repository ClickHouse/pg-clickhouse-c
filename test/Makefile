EXTENSION    = pgch_test
DATA         = pgch_test--1.0.sql
MODULE_big   = pgch_test
OBJS         = pgch_test.o
REGRESS      = roundtrip types errors
REGRESS_OPTS = --inputdir=. --load-extension=$(EXTENSION)
PG_CONFIG   ?= pg_config

# Override to build against a clickhouse-c checkout elsewhere.
CH_C_DIR    ?= ../vendor/clickhouse-c

# Library headers via -I; clickhouse-c and PG server headers via -isystem so
# their own warnings (eg -Wsign-compare inside PG's array.h macros) stay quiet.
PG_CPPFLAGS  = -I.. -isystem $(CH_C_DIR) \
               -isystem $(shell $(PG_CONFIG) --includedir-server) \
               -DPGCH_MSG_PREFIX='"pgch: "'
PG_CFLAGS    = -Wall -Wsign-compare -Wno-declaration-after-statement

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

$(OBJS): ../pg-clickhouse.h ../pg-clickhouse-decode.h ../pg-clickhouse-encode.h
