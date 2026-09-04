# pg-clickhouse-c

Turn a ClickHouse Native block into PostgreSQL `Datum`s and back.

clickhouse-c is used for Native format over a caller-supplied `chc_io`.
This library supplies the PostgreSQL half: `palloc` behind `chc_alloc`,
`chc_err` mapped onto `ereport`, CH types mapped onto PG type OIDs, and
of course `Datum` construction.

Nothing here opens a socket, runs a query, or knows whether bytes came
from a ClickHouse server, `clickhouse local`, or an embedded [chDB].

## Quickstart

Encode PG values into one Native block:

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

/* Serialize block into memory */
pgch_buf out = {};
chc_io io;
pgch_buf_io(&out, &io);

chc_block_opts opts = {};   /* Use local framing for chDB and clickhouse-local */
if (chc_block_write(&io, pgch_writer_build(w), &opts, &err) != CHC_OK)
    pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ", NULL);

pgch_writer_reset(w);       /* out now contains serialized block */
```

Decode Native bytes into rows, driving block supply yourself:

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

`r.values[i]` typed by `r.coltypes[i]`, which is OID `pgch_datum_oid` assigns
to column's CH type. Array and Tuple columns arrive as intermediate
representations rather than PG values: `pgch_convert` turns those into a real
PG array or record once you know target type.

```c
/* Build conversion state outside row context */
void *cs = pgch_convert_init(r.values[i], r.coltypes[i], target_oid, target_typmod);

/* Convert each row, NULL state passes Datum through */
values[i] = pgch_convert(cs, r.values[i]);
```

A complete working consumer, wired to nothing but memory, lives in
[test/pgch_test.c](test/pgch_test.c).

Use `pgch_pg_type_for` to build PostgreSQL column metadata from a type parsed
with `chc_type_parse`.

## Wiring to chDB

chDB streams Native bytes in both directions, so the seams are
`chdb_stream_append` on the way out and a `chc_io` over
`chdb_stream_fetch_result` on the way in. Declare the table structure with the
CH types you mean (`Array(Int64)`, not `String`), then parse those same type
names for the writer.

Out, one block per `pgch_writer_bytes` cut:

```c
void *stream = chdb_stream_insert(conn, query, "Native");

/* Append one row */
for (int i = 0; i < natts; i++)
    pgch_append_datum(w, i, values[i], atttypids[i], nulls[i]);

/* Flush one block */
pgch_buf buf = {};
chc_io io;
pgch_buf_io(&buf, &io);
if (chc_block_write(&io, pgch_writer_build(w), &opts, &err) != CHC_OK)
    pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ", NULL);
if (chdb_stream_append(stream, (char *) buf.data, buf.len) != CHDBSuccess)
    ereport(ERROR, ...);
pgch_writer_reset(w);
pgch_buf_reset(&buf);
```

In, a `chc_io.read` that pulls chunks and copies out of them, with
`chc_block_read` handling assembly across chunk boundaries:

```c
static int
chdb_read(void *ud, void *buf, size_t len, size_t *out_n, chc_err *err) {
    my_stream *s = ud;

    /* Fetch next chunk after consuming current chunk */
    if (s->cursor == s->len) {
        CHECK_FOR_INTERRUPTS();
        chdb_result *chunk = chdb_stream_fetch_result(s->conn, s->result);
        /* Check error, read buffer and length, report zero length as end */
    }
    *out_n = Min(len, s->len - s->cursor);
    memcpy(buf, s->data + s->cursor, *out_n);
    s->cursor += *out_n;
    return CHC_OK;
}
```

Then `chc_in_init` over that `chc_io`, `chc_block_read` inside the block
source, and `pgch_reader` on top: rows come out as `Datum`s ready for
`heap_form_tuple`, so the text-`COPY` parse on the PG side disappears along
with its escaping.

## Integration

PG14+. One TU defines `PGCH_IMPLEMENTATION` before including.

clickhouse-c is vendored here at `clickhouse-c/`, pinned to a commit these
headers compile against. It has no stable API, so take that pin rather than a
second checkout: clickhouse-c types are in this library's own signatures, and
two copies on the include path means whichever lands first wins silently.

```make
PGCH_DIR = vendor/pg-clickhouse-c
CH_C_DIR = $(PGCH_DIR)/clickhouse-c

# Treat dependency headers as system headers
PG_CPPFLAGS = -isystem $(CH_C_DIR) -isystem $(PGCH_DIR)
```

One submodule, cloned recursively:

```sh
git submodule add https://github.com/ClickHouse/pg-clickhouse-c vendor/pg-clickhouse-c
git submodule update --init --recursive
```

`PGCH_MSG_PREFIX` prefixes every message the library raises. Define it in the
build, not in a single TU, so every TU expanding `pgch_error` agrees:

```make
PG_CPPFLAGS += -DPGCH_MSG_PREFIX='"pg_chdb: "'
```

## Headers

| Header | Consumer API |
|---|---|
| [`pg-clickhouse.h`](doc/pg-clickhouse.md) | Errors, allocation, type mappings, query settings, intermediate representations, byte buffers |
| [`pg-clickhouse-decode.h`](doc/pg-clickhouse-decode.md) | Block and chunk sources, row reader, target-type conversion |
| [`pg-clickhouse-encode.h`](doc/pg-clickhouse-encode.md) | Writer lifecycle, Datum appends, arrays, block output |

Decode and encode each depend only on the core header; take one or both.

## Type mapping

`pgch_pg_type_for` maps a parsed ClickHouse type onto a PostgreSQL column. Every
name the parser resolves reaches this table or the omitted list `test/sql/type_table.sql`.

<!-- TYPE-TABLE-BEGIN -->
|     ClickHouse      |         PostgreSQL          |              Notes               |
|---------------------|-----------------------------|----------------------------------|
| Array(T)            | T[]                         | One PG array type per depth      |
| BFloat16            | real                        | Write drops low mantissa bits    |
| Bool                | boolean                     |                                  |
| Date                | date                        |                                  |
| Date32              | date                        |                                  |
| DateTime            | timestamp with time zone    |                                  |
| DateTime64(P)       | timestamp(P) with time zone | P over 6 caps at 6               |
| Decimal(P,S)        | numeric(P,S)                |                                  |
| Decimal32(S)        | numeric(9,S)                |                                  |
| Decimal64(S)        | numeric(18,S)               |                                  |
| Decimal128(S)       | numeric(38,S)               |                                  |
| Decimal256(S)       | numeric(76,S)               |                                  |
| Enum8               | text                        |                                  |
| Enum16              | text                        |                                  |
| FixedString(N)      | text                        | N counts CH bytes, PG characters |
| Float32             | real                        |                                  |
| Float64             | double precision            |                                  |
| IPv4                | inet                        |                                  |
| IPv6                | inet                        |                                  |
| Int8                | smallint                    |                                  |
| Int16               | smallint                    |                                  |
| Int32               | integer                     |                                  |
| Int64               | bigint                      |                                  |
| Int128              | numeric(39,0)               |                                  |
| Int256              | numeric(77,0)               |                                  |
| IntervalDay         | interval                    |                                  |
| IntervalHour        | interval                    |                                  |
| IntervalMicrosecond | interval                    |                                  |
| IntervalMillisecond | interval                    |                                  |
| IntervalMinute      | interval                    |                                  |
| IntervalMonth       | interval                    |                                  |
| IntervalNanosecond  | interval                    | Truncates to microsecond         |
| IntervalQuarter     | interval                    |                                  |
| IntervalSecond      | interval                    |                                  |
| IntervalWeek        | interval                    |                                  |
| IntervalYear        | interval                    |                                  |
| JSON                | jsonb                       | Also reads into json             |
| LineString          | path                        |                                  |
| LowCardinality(T)   | T                           |                                  |
| Map(K,V)            | record[]                    | One record per pair              |
| MultiLineString     | path[]                      |                                  |
| MultiPolygon        | polygon[][]                 |                                  |
| Nullable(T)         | T                           | Sets nullable on the column      |
| Point               | point                       |                                  |
| Polygon             | polygon[]                   |                                  |
| Ring                | polygon                     |                                  |
| String              | text                        | Also reads into bytea            |
| Time                | time without time zone      |                                  |
| Time64(P)           | time(P) without time zone   | P over 6 caps at 6               |
| Tuple(...)          | record                      | Pseudo type, no column takes it  |
| UInt8               | smallint                    |                                  |
| UInt16              | integer                     |                                  |
| UInt32              | bigint                      |                                  |
| UInt64              | numeric(20,0)               |                                  |
| UInt128             | numeric(39,0)               |                                  |
| UInt256             | numeric(78,0)               |                                  |
| UUID                | uuid                        |                                  |
<!-- TYPE-TABLE-END -->

## Testing

```sh
make -C test                 # clones clickhouse-c/ if absent
make -C test install         # needs write access to the PG install
make -C test installcheck    # needs superuser
```

[clickhouse-c]: https://github.com/ClickHouse/clickhouse-c
[pg_clickhouse]: https://github.com/ClickHouse/pg_clickhouse
[chDB]: https://github.com/chdb-io/chdb
