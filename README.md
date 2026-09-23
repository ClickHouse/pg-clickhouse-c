# pg-clickhouse-c

Convert ClickHouse Native blocks to PostgreSQL `Datum`s and back.

Built on [clickhouse-c], with PostgreSQL memory allocation, errors, and type
conversion. Supply your own transport and query execution.

## Quickstart

Encode PostgreSQL values into one Native block:

```c
/* Compile both implementations in one translation unit */
#define CHC_IMPLEMENTATION
#define PGCH_IMPLEMENTATION
#include "clickhouse.h"

#include "pg-clickhouse-decode.h"
#include "pg-clickhouse-encode.h"

/* Match writer types to declared ClickHouse structure */
chc_type *t;
chc_err err = {};
if (chc_type_parse("Array(Int32)", 12, &pgch_alloc, &t, &err) != CHC_OK)
    pgch_raise(&err, ERRCODE_INVALID_PARAMETER_VALUE, NULL, "column \"tags\"");

pgch_col col   = { .name = "tags", .name_len = 4, .type = t };
pgch_writer *w = pgch_writer_new(CurrentMemoryContext, &col, 1);

for (int i = 0; i < nrows; i++)
    pgch_append_datum(w, 0, values[i], INT4ARRAYOID, nulls[i]);

pgch_buf out = {};
pgch_writer_flush(w, &out, NULL);
/* Send out.data and out.len to destination */
```

Supply decoded blocks through a callback:

```c
/* Transfer each returned block to reader */
static const chc_block *
next_block(void *ud) {
    my_stream *s = ud;
    chc_block *b = NULL;
    chc_err err  = {};

    if (chc_block_read(s->in, &pgch_alloc, &s->opts, &b, &err) != CHC_OK) {
        s->error = pstrdup(err.msg);
        return NULL;
    }
    return b;   /* Return NULL at end of stream */
}

pgch_block_source src = { .ud = &s, .next_block = next_block, .error = my_error };
pgch_reader r;

pgch_reader_init(&r, &src);
while (pgch_reader_next(&r)) {
    /* Consume r.values, r.nulls, and r.coltypes */
}
if (r.error)
    ereport(ERROR, errcode(ERRCODE_FDW_ERROR), errmsg("%s", r.error));
pgch_reader_free(&r);
```

Convert decoded values to target PostgreSQL types before use. Strings arrive
as `bytea`; arrays and tuples use intermediate representations.

See [reader example](doc/pg-clickhouse-decode.md#complete-reader-example)
for row conversion and [tests](test/pgch_test.c) for a working consumer.

Use `pgch_pg_type_for` to build PostgreSQL column metadata from a type parsed
with `chc_type_parse`.

## Integration

Requires PostgreSQL 14 or later. Define `PGCH_IMPLEMENTATION` in one
translation unit.

Use vendored `clickhouse-c/`. Its API is unstable, so another version on your
include path may be incompatible.

```make
PGCH_DIR = vendor/pg-clickhouse-c
CH_C_DIR = $(PGCH_DIR)/clickhouse-c

# Treat dependency headers as system headers
PG_CPPFLAGS = -isystem $(CH_C_DIR) -isystem $(PGCH_DIR)
```

Add as a submodule:

```sh
git submodule add https://github.com/ClickHouse/pg-clickhouse-c vendor/pg-clickhouse-c
git submodule update --init --recursive
```

Set `PGCH_MSG_PREFIX` in build flags to prefix library errors consistently:

```make
PG_CPPFLAGS += -DPGCH_MSG_PREFIX='"pg_chdb: "'
```

## Headers

| Header | Consumer API |
|---|---|
| [`pg-clickhouse.h`](doc/pg-clickhouse.md) | Setup, errors, memory, schemas, query settings |
| [`pg-clickhouse-decode.h`](doc/pg-clickhouse-decode.md) | Read blocks or byte chunks and convert rows |
| [`pg-clickhouse-encode.h`](doc/pg-clickhouse-encode.md) | Convert rows and write blocks |

Include decoder, encoder, or both. Each includes core header.

## Type mapping

- [Read targets and conversions](doc/pg-clickhouse-decode.md#type-mapping)
- [Write defaults and conversions](doc/pg-clickhouse-encode.md#type-mapping)
- [chDB integration](doc/chdb.md)

## Testing

```sh
make -C test                 # clones clickhouse-c/ if absent
make -C test install         # needs write access to the PG install
make -C test installcheck    # needs superuser
```

## Coverage

GCC, matching `gcov`, and the regression suite report line coverage of
`pg-clickhouse*.h`:

```sh
initdb -D /tmp/pgch -A trust          # cluster owned by the build's user
pg_ctl -D /tmp/pgch -o '-k /tmp/pgch' start && export PGHOST=/tmp/pgch
make -C test coverage-build           # rebuilds with --coverage at -O0
sudo make -C test install PG_CONFIG="$(which pg_config)"
make -C test installcheck
make -C test installcheck REGRESS_OPTS='--no-locale --encoding=SQL_ASCII'
make -C test installcheck REGRESS_OPTS='--no-locale --encoding=EUC_KR'
pg_ctl -D /tmp/pgch stop
make -C test coverage-report
```

Run test server as your build user so coverage counters remain writable.
Counters accumulate across runs. Stop test server normally before reporting;
killed backends lose their counters.

[clickhouse-c]: https://github.com/ClickHouse/clickhouse-c
