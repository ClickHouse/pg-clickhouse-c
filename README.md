# pg-clickhouse-c

PostgreSQL bindings for [clickhouse-c]. Header-only, three headers, no
transport: turn a ClickHouse Native block into PostgreSQL `Datum`s and back.

clickhouse-c reads and writes the Native wire format over a caller-supplied
`chc_io`. This library supplies the PostgreSQL half: `palloc` behind
`chc_alloc`, `chc_err` mapped onto `ereport`, CH types mapped onto PG type
OIDs, and per-row `Datum` construction for every column shape the Native
format carries.

Extracted from [pg_clickhouse]'s binary driver, with the TCP client and FDW
plumbing left behind. Nothing here opens a socket, runs a query, or knows
whether the bytes came from a ClickHouse server, `clickhouse local`, or an
embedded [chDB].

## Why Native

Text formats (`TSV`, `CSV`) only carry the types PG and CH happen to
serialize identically. `Array`, `Nullable` inside `LowCardinality`,
`Decimal128`/`Decimal256`, nested arrays, `Enum`, `Tuple` all differ in
punctuation, escaping, or precision, so a text round trip either mangles them
or forces the whole column to `String`.

Native carries the schema in the block header and each column in its own
layout, so the mapping is explicit at both ends and arrays cost no escaping
at all.

## Quickstart

Encode PG values into one Native block:

```c
/* glue.c -- the one TU carrying both implementations */
#define CHC_IMPLEMENTATION
#define PGCH_IMPLEMENTATION
#include "clickhouse.h"

#include "pg-clickhouse-decode.h"
#include "pg-clickhouse-encode.h"

/* Types come from the structure you declared to ClickHouse. */
chc_type *t;
chc_err err = {};
if (chc_type_parse("Array(Int32)", 12, &pgch_alloc, &t, &err) != CHC_OK)
    pgch_raise(&err, ERRCODE_INVALID_PARAMETER_VALUE, "type parse: ");

pgch_col col   = { .name = "tags", .name_len = 4, .type = t };
pgch_writer *w = pgch_writer_new(CurrentMemoryContext, &col, 1);

for (int i = 0; i < nrows; i++)
    pgch_append_datum(w, 0, values[i], INT4ARRAYOID, nulls[i]);

/* Serialize to memory; hand the bytes to whatever consumes Native. */
pgch_buf out = {};
chc_io io;
pgch_buf_io(&out, &io);

chc_block_opts opts = {};   /* chDB / clickhouse-local: no BlockInfo */
if (chc_block_write(&io, pgch_writer_build(w), &opts, &err) != CHC_OK)
    pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ");

pgch_writer_reset(w);       /* out.data/out.len now hold the block */
```

Decode Native bytes into rows, driving block supply yourself:

```c
/* next_block hands ownership to the reader, which destroys each block. */
static const chc_block *
next_block(void *ud) {
    my_stream *s = ud;
    chc_block *b = NULL;
    chc_err err  = {};

    if (chc_block_read(s->in, &pgch_alloc, &s->opts, &b, &err) != CHC_OK) {
        s->error = pstrdup(err.msg);
        return NULL;
    }
    return b;   /* NULL at end of stream */
}

pgch_block_source src = { .ud = &s, .next_block = next_block, .error = my_error };
pgch_reader r;

pgch_reader_init(&r, &src);
while (pgch_reader_next(&r)) {
    /* r.values[i] / r.nulls[i] / r.coltypes[i], ncols = r.ncols */
}
if (r.error)
    ereport(ERROR, errcode(ERRCODE_FDW_ERROR), errmsg("%s", r.error));
pgch_reader_free(&r);
```

`r.values[i]` is typed by `r.coltypes[i]`, which is the OID
`pgch_datum_oid` assigns to the column's CH type. Composite columns arrive as
carriers rather than PG values: `pgch_convert` turns those into a real PG
array or record once you know the target type.

```c
/* Build the conversion state once, off the per-row context. */
void *cs = pgch_convert_init(r.values[i], r.coltypes[i], target_oid);

/* Then per row; NULL state means the Datum needs no conversion. */
values[i] = pgch_convert(cs, r.values[i]);
```

A complete working consumer, wired to nothing but memory, lives in
[test/pgch_test.c](test/pgch_test.c).

## Wiring to chDB

chDB streams Native bytes in both directions, so the seams are
`chdb_stream_append` on the way out and a `chc_io` over
`chdb_stream_fetch_result` on the way in. Declare the table structure with the
CH types you mean (`Array(Int64)`, not `String`), then parse those same type
names for the writer.

Out, one block per `pgch_writer_bytes` cut:

```c
void *stream = chdb_stream_insert(conn, query, "Native");

/* per row */
for (int i = 0; i < natts; i++)
    pgch_append_datum(w, i, values[i], atttypids[i], nulls[i]);

/* per block */
pgch_buf buf = {};
chc_io io;
pgch_buf_io(&buf, &io);
if (chc_block_write(&io, pgch_writer_build(w), &opts, &err) != CHC_OK)
    pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ");
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

    /* Refill from the next chunk when the current one is drained. */
    if (s->cursor == s->len) {
        CHECK_FOR_INTERRUPTS();
        chdb_result *chunk = chdb_stream_fetch_result(s->conn, s->result);
        /* chdb_result_error, then chdb_result_buffer / chdb_result_length;
           zero length is end of stream, so report *out_n = 0. */
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

PostgreSQL 13 and up. Both libraries are header-only. Exactly one TU defines `PGCH_IMPLEMENTATION`
before including; every other TU includes for declarations only. That TU
normally defines `CHC_IMPLEMENTATION` too, and must if it calls
`pgch_in_alloc` (`sizeof(chc_in)` is visible only where clickhouse-c's
implementation is compiled).

```make
CH_C_DIR ?= vendor/clickhouse-c
PGCH_DIR ?= vendor/pg-clickhouse-c

# -isystem keeps their warnings out of your -Werror build.
PG_CPPFLAGS = -isystem $(CH_C_DIR) -isystem $(PGCH_DIR)
```

Both are usually vendored as submodules:

```sh
git submodule add https://github.com/ClickHouse/clickhouse-c vendor/clickhouse-c
git submodule add https://github.com/ClickHouse/pg-clickhouse-c vendor/pg-clickhouse-c
```

`pgch_msg_prefix` prefixes every message the library raises; point it at your
extension's name from `_PG_init`:

```c
pgch_msg_prefix = "pg_chdb: ";
```

## Headers

| Header | Purpose |
|---|---|
| [`pg-clickhouse.h`](doc/pg-clickhouse.md) | Core: `pgch_alloc`, `pgch_raise`, type OID mapping, Array / Tuple carriers, `pgch_buf` as a `chc_io` write sink |
| [`pg-clickhouse-decode.h`](doc/pg-clickhouse-decode.md) | Block -> `Datum`: `pgch_read_value`, the `pgch_reader` row cursor, carrier-to-PG-value conversion |
| [`pg-clickhouse-encode.h`](doc/pg-clickhouse-encode.md) | `Datum` -> block: the `pgch_writer` buffer tree, typed appends, `chc_block_builder` assembly |

Decode and encode each depend only on the core header; take one or both.

## Type mapping

| ClickHouse | PostgreSQL |
|---|---|
| `Int8`, `Int16`, `UInt8` | `smallint` |
| `Int32`, `UInt16` | `integer` |
| `Int64`, `UInt32`, `UInt64` | `bigint` |
| `Bool` | `boolean` |
| `Float32` / `Float64` | `real` / `double precision` |
| `Decimal32/64/128/256` | `numeric` |
| `String`, `FixedString(N)`, `Enum8`, `Enum16` | `text` |
| `JSON`, `Object` | `jsonb` (or `json`, see below) |
| `Date`, `Date32` | `date` |
| `DateTime`, `DateTime64(P)` | `timestamptz` |
| `Time`, `Time64(P)` | `time` |
| `UUID` | `uuid` |
| `IPv4`, `IPv6` | `inet` |
| `Nullable(T)` | `T`, nullable |
| `LowCardinality(T)` | `T` |
| `Array(T)` | `T[]`, one PG dimension per `Array` layer |
| `Tuple(...)` | `record` |

`UInt64` above `2^63 - 1` raises rather than wrapping. `bytea` encodes into
`String` and `FixedString` verbatim. Decoding a `json` column instead of
`jsonb` keeps ClickHouse's verbatim document text: preset
`reader.coltypes[i] = JSONOID` after `pgch_reader_init`.

A PG type with no arm of its own reaches a column through a cast, so `money`
lands in `Decimal` and a domain lands wherever its base type does. Into a
`String` column that cast is the type's own output function, which is how
`interval`, `bit`, `macaddr`, the geo types and user enums get out; a `String`
column decodes back through the target's input function. `pgch_ch_type_for`
names the CH type to declare for a PG column, and
`pgch_structure_from_tupdesc` does it for a whole descriptor.

Encoding accepts a real PG array or an already-built `pgch_array` for
`Array(T)` columns. `Tuple` decodes but does not encode: there is no PG
composite path in. `Int128`/`Int256`, `Map`, `Variant`, `Dynamic`, `Nested`
and the geo types are unmapped in both directions.

## Required ClickHouse settings

Producing bytes this library can read needs, on the query:

```
output_format_native_encode_types_in_binary_format = 0
```

Without it the server writes binary type tags and `chc_block_read` fails with
`CHC_ERR_TYPE`. `JSON` columns additionally need

```
output_format_native_write_json_as_string = 1
```

which exists from 24.10 and makes the server serialize `JSON` as `String`,
the only `JSON` serialization either library handles. Both are
`PGCH_NATIVE_SETTINGS`, so a query builder can splice the pair in rather than
retype it and drift from the library it is compiled against.

## Testing

`test/` is a PostgreSQL extension that round trips values through Native
bytes in memory, with no server or chDB involved. It is also the compile
check for the headers, since it is the TU that instantiates both
implementations.

```sh
make -C test CH_C_DIR=/path/to/clickhouse-c
make -C test install         # needs write access to the PG install
make -C test installcheck    # needs superuser
```

## Non-goals

* Transport. Sockets, TCP packets, chDB handles and compression stay with the
  caller; see [`clickhouse-client.h`](https://github.com/ClickHouse/clickhouse-c/blob/main/doc/clickhouse-client.md)
  for the wire loop.
* Block framing policy. When to cut a block is the caller's call;
  `pgch_writer_bytes` and `pgch_writer_rows` are there to decide it.
* SQL generation. Deparsing, and where a `structure=` string goes in a query,
  are the consumer's. The type names in it are not: they are whatever these
  appenders accept, which is why `pgch_ch_type_for` and
  `pgch_structure_from_tupdesc` live here.
* Catalog integration. No FDW options, no matching a `TupleDesc` to a block by
  column name, no relation lookups.

[clickhouse-c]: https://github.com/ClickHouse/clickhouse-c
[pg_clickhouse]: https://github.com/ClickHouse/pg_clickhouse
[chDB]: https://github.com/chdb-io/chdb
