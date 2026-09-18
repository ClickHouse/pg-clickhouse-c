EXTENSION    = pgch_test
DATA         = pgch_test--1.0.sql
MODULE_big   = pgch_test
OBJS         = pgch_test.o
REGRESS      = roundtrip types pgtype errors type_table encoding api
PG_CONFIG   ?= pg_config

# Append to user-provided options (used for testing alternate encodeings).
override REGRESS_OPTS += --inputdir=. --load-extension=$(EXTENSION) --dbname=contrib_regression

# Vendored submodule. Override to build against a local clickhouse-c checkout
CH_C_DIR    ?= ../clickhouse-c

# Treat dependency headers as system headers to exclude dependency warnings
PG_CPPFLAGS  = -I.. -isystem $(CH_C_DIR) \
               -isystem $(shell $(PG_CONFIG) --includedir-server) \
               -DUSE_ASSERT_CHECKING -DPGCH_MSG_PREFIX='"pgch: "'
PG_CFLAGS    = -Wall -Wsign-compare -Wno-declaration-after-statement

# -fkeep-*-functions keeps an unexercised helper visible as a miss instead of
# letting it vanish from coverage notes
ifeq ($(COVERAGE),1)
PG_CFLAGS += --coverage -O0 -g -fprofile-abs-path \
             -fkeep-inline-functions -fkeep-static-functions
SHLIB_LINK += --coverage
endif

EXTRA_CLEAN = ../coverage *.gcda *.gcno

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

$(OBJS): ../pg-clickhouse.h ../pg-clickhouse-decode.h ../pg-clickhouse-encode.h \
         $(CH_C_DIR)/clickhouse.h

# Clone vendored submodule when absent. Spelled out rather than via CH_C_DIR:
# an override points at a checkout git knows nothing about
../clickhouse-c/clickhouse.h:
	git submodule update --init

COVERAGE_CC ?= gcc
# Report of a single PostgreSQL major gates nothing, CI merges majors first
COVERAGE_MIN_LINE ?= 100
# PGXS carries its own GCOV, empty unless PostgreSQL was configured for coverage
GCOV         = gcov

.PHONY: coverage-build coverage-report coverage-reset

coverage-build:
	$(MAKE) clean
	$(MAKE) COVERAGE=1 CC=$(COVERAGE_CC) all

# Counters accumulate across installcheck runs, so report after the last one
coverage-report:
	GCOV=$(GCOV) COVERAGE_MIN_LINE=$(COVERAGE_MIN_LINE) \
		sh ../tools/coverage.sh $(CURDIR)

coverage-reset:
	rm -rf ../coverage *.gcda
