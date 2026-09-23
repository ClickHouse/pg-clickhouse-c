# pg-clickhouse.h

Shared setup, memory, and schema helpers. See [header](../pg-clickhouse.h)
for declarations and low-level helpers.

Define both implementation macros in one translation unit:

```c
#define CHC_IMPLEMENTATION
#define PGCH_IMPLEMENTATION
#include "clickhouse.h"
#include "pg-clickhouse.h"
```

Include headers without these definitions elsewhere. `CHC_IMPLEMENTATION`
is required alongside `PGCH_IMPLEMENTATION` for `pgch_in_alloc` and
`pgch_reader_init_chunks`.

## Errors

`pgch_error` and `pgch_errorf` raise PostgreSQL `ERROR`. Use `pgch_raise` to
report a clickhouse-c error with a SQLSTATE and optional context:

```c
if (chc_block_write(&io, block, &opts, &err) != CHC_OK)
    pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ", NULL);
```

Set `PGCH_MSG_PREFIX` in build flags to identify errors from your extension:

```make
PG_CPPFLAGS += -DPGCH_MSG_PREFIX='"pg_chdb: "'
```

## Allocation

`pgch_alloc` uses `CurrentMemoryContext`. Choose a context that outlives
allocated data. Use this allocator for blocks passed to `pgch_reader`, which
owns and frees them.

`pgch_in_alloc` allocates a clickhouse-c input parser in `CurrentMemoryContext`.

## ClickHouse to PostgreSQL columns

Use `pgch_pg_type_for` to build column metadata from a type parsed with
`chc_type_parse`. Pass optional `what` text for error context.

Result includes type OID, type modifier, array dimensions, and nullability.
`truncated` flags precision PostgreSQL cannot preserve: `DateTime64` or
`Time64` above six fractional digits, and `IntervalNanosecond`.

Call `pgch_pg_type_is_column` before using metadata in a table definition.
Choose a concrete fallback for anonymous `record` and `record[]` types.
See [read mappings](pg-clickhouse-decode.md#type-mapping).

`pgch_datum_oid` describes decoded values before conversion, which may differ
from column types. Use `pgch_convert` to produce target PostgreSQL values.

## PostgreSQL to ClickHouse types

Use `pgch_ch_type_for` for a ClickHouse type declaration, including nullability.
Pass `NULL` options for [default mappings](pg-clickhouse-encode.md#type-mapping),
or set these `pgch_type_opts` fields:

| Option | Effect |
|---|---|
| `json_as_json` | Map `json` and `jsonb` to `JSON` instead of `String` |
| `low_cardinality` | Wrap `String` in `LowCardinality` |
| `numeric_as_string` | Map unconstrained `numeric` to `String` instead of `Decimal256(38)` |

ClickHouse arrays cannot be NULL. Choose whether to reject NULL arrays or
write empty arrays with [array policy](pg-clickhouse-encode.md#null-arrays).
`JSON` also remains non-nullable.

NULL paths and polygons become empty `LineString` and `Ring` values.
Nullable `point`, `box`, `circle`, and `line` columns require
`allow_experimental_nullable_tuple_type` in query settings.

Use `pgch_structure_from_tupdesc` for comma-separated `name type` declarations:

```c
char *structure = pgch_structure_from_tupdesc(desc, &opts);
```

Dropped and generated attributes are skipped. Use `pgch_attr_is_streamed` in
matching row loops, or append rows with `pgch_append_slot`.
Use `pgch_quote_ch_ident` when building identifiers yourself.
All returned strings use `palloc`.

## Native settings and framing

Apply `PGCH_NATIVE_SETTINGS` to queries returning Native data. It requests
textual type names and JSON documents as strings. JSON output requires
ClickHouse 24.10 or later. Add settings for experimental types separately.

Use `pgch_block_opts_local` with chDB and `clickhouse-local`. For TCP server
traffic, set `has_block_info` and `has_custom_serialization` according to
negotiated server revision.

## Byte buffers

Zero-initialize `pgch_buf`. Use `pgch_buf_io` to append through a `chc_io`, or
`pgch_writer_flush` to serialize buffered rows directly:

```c
pgch_buf out = {};

pgch_writer_flush(writer, &out, NULL);
send_native(out.data, out.len);
pgch_buf_reset(&out);
```

Buffers allocate in `CurrentMemoryContext`. Keep buffer alive while its
`chc_io` is in use. `pgch_buf_reset` clears length and keeps storage for reuse.
