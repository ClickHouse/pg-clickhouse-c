# pg-clickhouse.h

Core API shared by encoder and decoder. Include `postgres.h` and
`clickhouse.h` through this header.

Define `PGCH_IMPLEMENTATION` in exactly one translation unit:

```c
#define CHC_IMPLEMENTATION
#define PGCH_IMPLEMENTATION
#include "clickhouse.h"
#include "pg-clickhouse.h"
```

Include `pg-clickhouse.h` normally in all other translation units. Define
`CHC_IMPLEMENTATION` beside `PGCH_IMPLEMENTATION` when using
`pgch_in_alloc` or `pgch_reader_init_chunks`.

## Errors

```c
#define PGCH_MSG_PREFIX ""

#define pgch_error(sqlstate, msg)
#define pgch_errorf(sqlstate, fmt, ...)

pg_noreturn void pgch_raise(const chc_err *err, int sqlstate,
                            const char *what);
```

Define `PGCH_MSG_PREFIX` as a string literal in build flags to identify errors
from your extension:

```make
PG_CPPFLAGS += -DPGCH_MSG_PREFIX='"pg_chdb: "'
```

Keep this definition consistent across every translation unit. `pgch_error`
and `pgch_errorf` raise PostgreSQL `ERROR`. `pgch_raise` raises `ERROR` with
requested SQLSTATE, optional `what` text, and clickhouse-c error message.

```c
if (chc_block_write(&io, block, &opts, &err) != CHC_OK)
    pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ");
```

## Allocation

```c
extern const chc_alloc pgch_alloc;
chc_in *pgch_in_alloc(void);
```

`pgch_alloc` places allocations in `CurrentMemoryContext`. Switch to context
with required lifetime before calling clickhouse-c. Read blocks with
`pgch_alloc` before passing them to `pgch_reader`, because reader destroys
owned blocks with same allocator.

`pgch_in_alloc` returns zeroed clickhouse-c input parser in
`CurrentMemoryContext`. Symbol is available only when implementation
translation unit also defines `CHC_IMPLEMENTATION`.

## ClickHouse to PostgreSQL types

```c
extern const Oid pgch_kind_oids[CHC_KIND_COUNT];
extern const int64_t pgch_pow10[10];

Oid pgch_datum_oid(const chc_type *type);
Oid pgch_native_oid(const chc_type *type);
Oid pgch_native_oid_for(const chc_type *type, const char *what);

const chc_type *pgch_unwrap(const chc_type *type, bool *out_nullable);
```

`pgch_kind_oids` maps scalar `chc_kind` values to PostgreSQL OIDs. Wrapper and
unsupported kinds contain `InvalidOid`.

`pgch_datum_oid` returns OID produced by `pgch_read_value`:

- Scalar types return mapped scalar OID
- `Array` returns `ANYARRAYOID`, representing `pgch_array *`
- `Tuple` returns `RECORDOID`, representing `pgch_tuple *`
- `Map` returns `ANYARRAYOID` over `pgch_tuple *` pairs, as ClickHouse stores
  it as `Array(Tuple(K, V))`
- `Polygon`, `MultiPolygon`, and `MultiLineString` return `ANYARRAYOID`, having
  no PostgreSQL multi-geometry counterpart
- `Nullable` and `LowCardinality` return inner mapping
- `Nothing` and `Void` return `InvalidOid`

`pgch_native_oid` returns type suitable for a PostgreSQL column descriptor.
Unlike `pgch_datum_oid`, it resolves `Array` to PostgreSQL array OID for leaf
type, and `Map` to `record[]`. Unsupported mappings raise
`ERRCODE_FDW_INVALID_DATA_TYPE`.

`pgch_native_oid_for` behaves like `pgch_native_oid` and adds `what` to an
unsupported-type error. Pass `NULL` to omit context.

`pgch_unwrap` removes outer `Nullable`, `LowCardinality`, and nullable wrapper
inside `LowCardinality`. When `out_nullable` is not `NULL`, it reports whether
either nullable wrapper was present.

`pgch_pow10` contains powers from `10^0` through `10^9` for scaling `DateTime64`
and `Time64` values.

## PostgreSQL to ClickHouse types

```c
typedef struct pgch_type_opts {
    bool json_as_json;
    bool low_cardinality;
    bool numeric_as_string;
} pgch_type_opts;

char *pgch_ch_type_for(Oid typid, int32 typmod, bool notnull,
                       const pgch_type_opts *opts);
char *pgch_quote_ch_ident(const char *name);
bool  pgch_attr_is_streamed(Form_pg_attribute attr);
char *pgch_structure_from_tupdesc(TupleDesc desc,
                                  const pgch_type_opts *opts);
```

All returned strings are allocated with `palloc`.

`pgch_ch_type_for` returns full ClickHouse declaration, including
nullability. Pass `NULL` for default options:

- Map `json` and `jsonb` to `String`
- Map string-like types to `String`
- Map unconstrained `numeric` to `Decimal256(38)`

Set options to change those defaults:

- `json_as_json` maps `json` and `jsonb` to `JSON`
- `low_cardinality` wraps `String` as `LowCardinality`
- `numeric_as_string` maps unconstrained `numeric` to `String`

Domains use base type mapping and typmod. PostgreSQL arrays map to ClickHouse
`Array`, elements always `Nullable`: a PostgreSQL `NOT NULL` constrains the
array and never its elements, and ClickHouse does not support
`Nullable(Array(...))`. Loading such a column needs
`pgch_writer_set_null_array` since a NULL array has no `Array` representation.
Types without dedicated mapping use `String`. `JSON` remains unwrapped because
ClickHouse rejects `Nullable(JSON)`.

Geometric types map onto the ClickHouse geo types and Tuples over them:

| PostgreSQL | ClickHouse                               |
| ---------- | ---------------------------------------- |
| `point`    | `Point`                                  |
| `lseg`     | `LineString`, two points                 |
| `path`     | `LineString`, closed repeating its first |
| `polygon`  | `Ring`                                   |
| `box`      | `Tuple(high Point, low Point)`           |
| `circle`   | `Tuple(center Point, radius Float64)`    |
| `line`     | `Tuple(a Float64, b Float64, c Float64)` |

`Ring` and `LineString` are `Array(Point)`, so they stay unwrapped like `JSON`
and carry a NULL as the empty ring or line, which PostgreSQL cannot spell.
Nullable `point`, `box`, `circle` and `line` columns are nullable Tuples, which
ClickHouse gates behind `allow_experimental_nullable_tuple_type`. Request it
alongside `PGCH_NATIVE_SETTINGS` when a query can produce one.

`pgch_quote_ch_ident` returns unquoted name when valid bare ClickHouse
identifier, otherwise returns quoted and escaped identifier.

`pgch_attr_is_streamed` returns false for dropped and generated attributes.
Use it in consumer loops that must stay positionally aligned with generated
structure.

`pgch_structure_from_tupdesc` returns comma-separated `name type` declarations
for streamed attributes:

```c
char *structure = pgch_structure_from_tupdesc(desc, &opts);
```

## Native settings and framing

```c
#define PGCH_NATIVE_SETTINGS \
    "output_format_native_encode_types_in_binary_format=0," \
    "output_format_native_write_json_as_string=1"

extern const chc_block_opts pgch_block_opts_local;
```

Apply `PGCH_NATIVE_SETTINGS` to queries returning Native data. First setting
keeps textual type names expected by clickhouse-c. Second serializes
ClickHouse `JSON` columns as document strings and requires ClickHouse 24.10 or
later. The macro carries only what the wire format needs; settings that gate a
type rather than its serialization stay with the query builder.

Use `pgch_block_opts_local` with chDB and `clickhouse-local`. For TCP server
traffic, set `has_block_info` and `has_custom_serialization` according to
negotiated server revision.

## Intermediate array and tuple representations

```c
typedef struct pgch_array {
    Datum  *datums;
    bool   *nulls;
    size_t  len;
    int     ndim;
    Oid     item_type;
    Oid     array_type;
} pgch_array;

typedef struct pgch_tuple {
    Datum      *datums;
    bool       *nulls;
    Oid        *types;
    size_t      len;
    const char *ch_type_name;
} pgch_tuple;
```

Decoder returns intermediate representations before consumer supplies target
PostgreSQL type. Convert them with APIs in
[pg-clickhouse-decode.h](pg-clickhouse-decode.md).

For `pgch_array`, `ndim` is at least one. With nested arrays, each datum points
to child `pgch_array` until leaf level. `item_type` is PostgreSQL leaf OID.
Decoded array representations also set `array_type`; representations created
for encoding may leave it `InvalidOid`.

For `pgch_tuple`, `types[i]` describes `datums[i]`.

## Byte buffers

```c
typedef struct pgch_buf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} pgch_buf;

void pgch_buf_reserve(pgch_buf *b, size_t need);
void pgch_buf_append(pgch_buf *b, const void *src, size_t n);
void pgch_buf_append_zero(pgch_buf *b, size_t n);
void pgch_buf_reset(pgch_buf *b);
void pgch_buf_io(pgch_buf *b, chc_io *out_io);
```

Zero-initialize buffers. Growth uses `CurrentMemoryContext`.
`pgch_buf_reset` sets length to zero and keeps allocation for reuse.

`pgch_buf_io` initializes write-only `chc_io` that appends to buffer. Buffer
must outlive `chc_io`.

```c
pgch_buf out = {};
chc_io io;

pgch_buf_io(&out, &io);
if (chc_block_write(&io, block, &opts, &err) != CHC_OK)
    pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ");

send_native(out.data, out.len);
pgch_buf_reset(&out);
```
