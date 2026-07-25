# pg-clickhouse.h

Core header. Everything the decode and encode headers share: the allocator,
error mapping, type mapping, the Array / Tuple carriers, and a byte buffer
that doubles as a `chc_io` write sink.

Depends on [clickhouse.h](https://github.com/ClickHouse/clickhouse-c/blob/main/doc/clickhouse.md)
and `postgres.h`. Exactly one TU defines `PGCH_IMPLEMENTATION` before
including.

## Errors

```c
extern const char *pgch_msg_prefix;     /* "" by default */

#define pgch_error(sqlstate, msg)
#define pgch_errorf(sqlstate, fmt, ...)

pg_noreturn void pgch_raise(const chc_err *err, int sqlstate, const char *what);
```

Every message the library raises starts with `pgch_msg_prefix`. Set it once
from `_PG_init` to a string literal that outlives the backend; it is read, not
copied.

`pgch_raise` turns a clickhouse-c `chc_err` into `ereport(ERROR)`, splicing
`what` between the prefix and `err->msg`:

```c
if (chc_block_write(&io, bb, &opts, &err) != CHC_OK)
    pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ");
/* ERROR:  pg_chdb: block write: <clickhouse-c message> */
```

The `chc_err` `server_code` is not consulted; pass the sqlstate you want.

## Allocator

```c
extern const chc_alloc pgch_alloc;
```

`palloc` / `repalloc_huge` / `pfree` against `CurrentMemoryContext` at the
moment of the call, with `MCXT_ALLOC_HUGE` so a decoded block can exceed the
1GB `palloc` cap. clickhouse-c never frees on its own schedule, so placement
is entirely yours: switch to a per-query context before `chc_block_read` and
the whole block, buffers included, lands there and dies with it.

Nothing in this library takes an allocator argument. It always uses
`pgch_alloc`, so a block handed to `pgch_reader` must have been read with
`pgch_alloc` too.

```c
extern chc_in *pgch_in_alloc(void);
```

Zeroed `chc_in` for your read loop, since `sizeof(chc_in)` is only visible
where clickhouse-c's implementation is compiled. Defined only when the
`PGCH_IMPLEMENTATION` TU also defines `CHC_IMPLEMENTATION`; if you keep them
apart and hit a link error on this symbol, that is why.

## Type mapping

```c
extern const Oid pgch_kind_oids[CHC_KIND_COUNT];
extern const int64_t pgch_pow10[10];

Oid pgch_datum_oid(const chc_type *type);
Oid pgch_native_oid(const chc_type *type);
Oid pgch_native_oid_for(const chc_type *type, const char *what);

const chc_type *pgch_unwrap(const chc_type *type, bool *out_nullable);
```

`pgch_kind_oids` is the flat scalar table, indexed by `chc_kind`, holding
`InvalidOid` for wrapper kinds and everything unmapped.

`pgch_datum_oid` describes what `pgch_read_value` actually returns: scalars
through the table, `Array` as `ANYARRAYOID` (a `pgch_array *`), `Tuple` as
`RECORDOID` (a `pgch_tuple *`), `Nullable` and `LowCardinality` transparent.
`Nothing` / `Void` map to `InvalidOid` and always decode NULL.

`pgch_native_oid` is the same except `Array` resolves to the real PG array
type of the leaf element, which is what you want when building a `TupleDesc`.
PG has one array type per element type regardless of dimensionality, so
`Array(Array(Int32))` and `Array(Int32)` both give `integer[]`.

Both raise `ERRCODE_FDW_INVALID_DATA_TYPE` on a kind with no mapping.
`pgch_native_oid_for` is the same lookup with a caller-supplied `what`, eg
`column "c2"`, appended in parentheses, for callers resolving a type deep
inside a row where the bare type name is not enough to locate the problem.

`pgch_unwrap` strips `Nullable`, then `LowCardinality` and any `Nullable`
inside it, reporting through `out_nullable` whether either was present.
ClickHouse puts `Nullable` inside `LowCardinality`, not above it, so a single
strip is not enough.

## PG type to CH type name

```c
typedef struct pgch_type_opts {
    bool json_as_json;       /* jsonb -> JSON rather than String */
    bool low_cardinality;    /* wrap String columns in LowCardinality */
    bool numeric_as_string;  /* unconstrained numeric -> String */
} pgch_type_opts;

char *pgch_ch_type_for(Oid typid, int32 typmod, bool notnull,
                       const pgch_type_opts *opts);
char *pgch_quote_ch_ident(const char *name);
char *pgch_structure_from_tupdesc(TupleDesc desc, const pgch_type_opts *opts);
```

The third face of the bridge. Encode dispatches on (PG type, CH kind), decode
on (CH kind, PG type), and this names the CH type to declare for a PG column
so the two have something to agree on. The result carries its own `Nullable`
wrapper, so the same string serves a `structure=` clause and `chc_type_parse`
for the writer's column type.

The rules are the appenders', not a matter of consumer taste:

* `bpchar(n)` and `varchar(n)` are `String`, because `FixedString(n)` counts
  bytes where PG counts characters.
* Unconstrained `numeric` is `Decimal256(38)`, or `String` under
  `numeric_as_string`, which is the only one of the two that does not silently
  truncate PG's 16383 digits of scale.
* `timestamp` is `DateTime64(6)` and `timestamptz` is `DateTime64(6,'UTC')`:
  Native carries an int64, and text rendering was the only reason either was
  ever `String`.
* `date` is `Date32`, since `Date` is unsigned days from 1970.
* `T[]` is `Array(<T nullable per notnull>)`, because `Nullable(Array(...))`
  is prohibited server side. The nullability lands on the element and a NULL
  array itself becomes `pgch_writer_set_null_array`'s problem.
* A domain takes its base type's mapping, typmod included.
* Everything else is `String`, reached through the type's output function on
  the way out and its input function on the way back: `interval`, `timetz`,
  `money`, `inet`, `cidr`, `macaddr`, `bit`, `varbit`, `xml`, `tsvector`, the
  geo types and every user enum.

`pgch_quote_ch_ident` double-quotes unless the name is already a bare
`[A-Za-z_][A-Za-z0-9_]*`, doubling embedded quotes and escaping backslashes,
which is what ClickHouse's SQL-style quoted identifiers want.

`pgch_structure_from_tupdesc` joins the two into `name type, ...` over a
descriptor, skipping dropped and generated attributes since neither carries a
value in a stream. `pgch_append_slot` skips the same ones, so writer column
`i` is structure column `i`. Where that string goes, and everything else about
the query, stays with the caller.

## Server settings

```c
#define PGCH_NATIVE_SETTINGS \
    "output_format_native_encode_types_in_binary_format=0," \
    "output_format_native_write_json_as_string=1"

extern const chc_block_opts pgch_block_opts_local;
```

The compatibility contract with the server, versioned with the library rather
than retyped into each consumer's query builder. Without the first setting the
server writes binary type tags and `chc_block_read` fails with
`CHC_ERR_TYPE`; the second exists from 24.10 and makes `JSON` serialize as
`String`, the only `JSON` serialization either library handles.

`pgch_block_opts_local` is the framing chDB and `clickhouse local` use: no
`BlockInfo`, no custom-serialization flag. The TCP path sets both per server
revision, so it builds its own.

## Array and Tuple carriers

```c
typedef struct pgch_array {
    Datum  *datums;
    bool   *nulls;
    size_t  len;
    int     ndim;        /* >= 1 */
    Oid     item_type;   /* leaf scalar PG type */
    Oid     array_type;  /* PG array type */
} pgch_array;

typedef struct pgch_tuple {
    Datum  *datums;
    bool   *nulls;
    Oid    *types;
    size_t  len;
    const char *ch_type_name;
} pgch_tuple;
```

Composite columns decode into these rather than into PG values, because
building an `ArrayType` needs the element type's `typlen` / `typbyval` /
`typalign` and a target type that only the consumer knows. `pgch_convert`
does that step.

For nested arrays each level is its own `pgch_array` with `ndim` one lower,
so `datums[i]` at `ndim > 1` is a `pgch_array *`, not an element. `len` is
that level's length; a ragged CH array (legal in ClickHouse, not in PG) is
detected during conversion, not here.

`array_type` is filled when decoding and `InvalidOid` when the encoder built
the carrier, which never needs it.

## Byte buffer

```c
typedef struct pgch_buf { uint8_t *data; size_t len; size_t cap; } pgch_buf;

void pgch_buf_reserve(pgch_buf *b, size_t need);
void pgch_buf_append(pgch_buf *b, const void *src, size_t n);
void pgch_buf_append_zero(pgch_buf *b, size_t n);
void pgch_buf_reset(pgch_buf *b);

void pgch_buf_io(pgch_buf *b, chc_io *out_io);
```

Doubling `palloc` buffer. It grows against `CurrentMemoryContext` at the time
of the append, so switch first and free by deleting that context;
`pgch_buf_reset` only rewinds `len`, keeping the allocation for the next
block.

`pgch_buf_io` fills a write-only `chc_io` over the buffer, which is how you
get `chc_block_write` output into memory rather than onto a socket. `read` and
`check_cancel` are NULL, so the result is a sink only, and `b` must outlive
`out_io`.

```c
pgch_buf buf = {};
chc_io io;

pgch_buf_io(&buf, &io);
chc_block_write(&io, bb, &opts, &err);
/* buf.data[0 .. buf.len) is one Native block */
```
