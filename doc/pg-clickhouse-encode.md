# pg-clickhouse-encode.h

PostgreSQL `Datum` to Native block. A writer buffers rows column by column,
then assembles a `chc_block_builder` over those buffers for
`chc_block_write` (or `chc_client_send_data`) to serialize.

Depends on [pg-clickhouse.h](pg-clickhouse.md). Exactly one TU defines
`PGCH_IMPLEMENTATION` before including.

## Model

The writer holds one buffer node per structural level of each column's CH
type: `Nullable` owns a null map, `Array` owns an offsets array,
`LowCardinality` owns raw rows the dictionary is built from at assembly,
and the leaf owns row-aligned values. Appends walk down that tree recording a
null bit at each `Nullable` crossed; assembly walks back up calling
`chc_build_*` on the same shape.

The CH type drives everything. Dispatch is on the pair (PG type OID, CH kind),
so a `Datum` is only ever encoded the way the destination column expects, and
a pair with no bridge raises instead of guessing. Supply types from
`chc_type_parse` over the structure you declared to ClickHouse, or from a
block the server sent you.

## Writer

```c
typedef struct pgch_col {
    const char     *name;
    size_t          name_len;
    const chc_type *type;
} pgch_col;

pgch_writer *pgch_writer_new(MemoryContext parent,
                             const pgch_col *cols, size_t ncols);
void         pgch_writer_free(pgch_writer *w);
```

`pgch_writer_new` allocates a child context of `parent` holding the writer,
its buffers and copies of the column names, so `name` need not outlive the
call. `type` must: the node tree borrows it for enum tables, decimal scale and
`DateTime64` precision. `pgch_writer_free` deletes that context, and so does
deleting `parent`.

Raises `ERRCODE_FDW_INVALID_DATA_TYPE` here, before any row is appended, for a
column type with no buffer shape: `Tuple`, `Map`, `Int128`, geo types, or
`LowCardinality` over anything but `String`.

## Appending rows

```c
void pgch_append_datum(pgch_writer *w, size_t col,
                       Datum val, Oid valtype, bool isnull);
```

The entry point. `valtype` is the PG type of `val`; a real PG array type is
flattened into the column's `Array` levels, and `ANYARRAYOID` means `val` is
already a `pgch_array`. Integer widths mix freely (`int2` into `Int64` and so
on) since ClickHouse's widths outnumber PG's. `bytea` goes into `String` and
`FixedString` verbatim, `json` / `jsonb` into `JSON`, `Object` or `String`.

A pair with no direct path falls back to a cast: `find_coercion_pathway` from
`valtype` to the PG type `pgch_native_oid` gives that level, cached there since
an incoming type is fixed for a statement's life. So `oid` or a domain over
`int4` reaches an `Int64` column without the caller pre-coercing, and `money`
reaches a `Decimal` one, PG having no `money` to `int8` cast. A missing cast
surfaces as `ERRCODE_DATATYPE_MISMATCH` on the first row needing it rather than
up front. Direct dispatch wins, so `json` into `JSON` and `bytea` into `String`
never route through a cast.

The level is the append target, not the column, so an `Array` element resolves
against its own element type: `money[]` and `interval[]` reach
`Array(String)` the same way their scalars reach `String`.

Into a `String`, `FixedString` or `Enum` column the fallback also takes
`COERCION_PATH_COERCEVIAIO`, running the source's output function and the
target's input function. That is the mirror of the rule decode already
documents, and it means a consumer never has to know which PG types this
header has a fast path for, only that a `String` column always works:
`interval`, `timetz`, `money`, `macaddr`, `cidr`, `bit`, `varbit`, `xml`,
`tsvector`, `jsonpath`, the geo types and every user enum all arrive that way.
`find_coercion_pathway` offers the same path for a string *source*, which is
deliberately not taken: `'x'::text` into `Int32` stays a type error rather than
becoming an input-syntax error somewhere in row 40000.

Each column takes exactly one append per row, in any order. There is no
end-of-row call: row counts come from the buffers, and
`pgch_writer_rows` reports the first column's, so a skipped column shows up as
a row-count mismatch at write time rather than as silently shifted data.

```c
void pgch_append_slot(pgch_writer *w, TupleTableSlot *slot);
```

A whole row out of a slot, attribute `i` into column `i`. Dropped and
generated attributes are skipped, the same ones
`pgch_structure_from_tupdesc` skips, so a writer built from that structure
lines up with a slot over the same descriptor.

## Policies

```c
void pgch_writer_set_null_array(pgch_writer *w, pgch_null_array policy);
void pgch_writer_set_nonfinite(pgch_writer *w, pgch_nonfinite policy);
```

NULL into a column ClickHouse declared NOT NULL raises
`ERRCODE_NOT_NULL_VIOLATION`. That includes a NULL PG array into `Array(T)`,
which ClickHouse has no representation for, `Nullable(Array(...))` being
prohibited server-side. A nullable PG array column is ordinary and a bulk load
cannot fail on one, so `PGCH_NULL_ARRAY_EMPTY` stores an empty array instead.

`PGCH_NONFINITE_KEEP`, the default, writes `NaN` and `Infinity` where the
column can hold them, which `Float32` / `Float64` can and `Decimal` cannot.
`PGCH_NONFINITE_NULL` and `_ZERO` substitute instead, for a load that should
not abort on row 40000 over one value; `_NULL` still raises on a NOT NULL
column, since the substitute has nowhere to go either.

```c
Datum pgch_array_from_pg(Datum arr, Oid elemtype,
                         int16 typlen, bool typbyval, char typalign);
```

Builds the `pgch_array` carrier from a PG array, one carrier level per PG
dimension. `pgch_append_datum` does this itself for a PG array type, paying
`get_typlenbyvalalign` per value; call it yourself with cached type info and
pass `ANYARRAYOID` to skip the lookup on a hot insert path.

## Typed appends

```c
void pgch_append_int(pgch_writer *w, size_t col, int64_t val, bool isnull);
void pgch_append_uint(pgch_writer *w, size_t col, uint64_t val, bool isnull);
void pgch_append_bool(pgch_writer *w, size_t col, bool val, bool isnull);
void pgch_append_double(pgch_writer *w, size_t col, double val, bool isnull);
void pgch_append_float(pgch_writer *w, size_t col, float val, bool isnull);
void pgch_append_bytes(pgch_writer *w, size_t col,
                       const void *p, size_t n, bool isnull);
void pgch_append_decimal(pgch_writer *w, size_t col,
                         const char *digits, bool isnull);
void pgch_append_uuid(pgch_writer *w, size_t col,
                      const uint8_t bytes[16], bool isnull);
void pgch_append_inet(pgch_writer *w, size_t col,
                      const uint8_t *addr_be, size_t addrlen, bool isnull);
void pgch_append_date_seconds(pgch_writer *w, size_t col,
                              int64_t seconds, bool isnull);
void pgch_append_datetime_seconds(pgch_writer *w, size_t col,
                                  int64_t seconds, bool isnull);
void pgch_append_datetime64_raw(pgch_writer *w, size_t col,
                                int64_t raw, bool isnull);
void pgch_append_time_seconds(pgch_writer *w, size_t col,
                              int64_t seconds, bool isnull);
void pgch_append_time64_raw(pgch_writer *w, size_t col,
                            int64_t raw, bool isnull);
```

The layer `pgch_append_datum` sits on, for values that are not PG Datums. Pass
`isnull` with any placeholder value to write a NULL; the leaf still gets a
zero-filled slot, because ClickHouse serializes a `Nullable` column's values
array in full alongside its null map.

`pgch_append_bytes` covers `String`, `FixedString` (right-padded or truncated
to width), `Enum8` / `Enum16` (name looked up in the column's table),
`LowCardinality(String)` and `JSON`. A NULL into a `Nullable(JSON)` writes
`{}` rather than an empty string, since ClickHouse parses a `Nullable` JSON
column's values even where the null map says to ignore them.

`pgch_append_decimal` takes decimal text, `[-]digits[.frac]`, and folds the
column's scale in; `numeric_out` output is the intended input. `NaN` and
`Infinity` raise unless `pgch_writer_set_nonfinite` says otherwise.

`pgch_append_inet` takes big-endian address bytes, PG's `inet` `ip_addr`
layout, 4 for `IPv4` and 16 for `IPv6`. Pass the expected width even when
NULL.

Date and DateTime take unix seconds, `Time` seconds since midnight.
`DateTime64` and `Time64` take the wire integer already scaled, which is
`pgch_column_datetime64_scale`'s job to tell you:

```c
uint32_t scale = pgch_column_datetime64_scale(w, col);
int64_t  raw   = seconds * pgch_pow10[scale] + fraction;
```

## Arrays

```c
void     pgch_array_begin(pgch_writer *w, size_t col);
void     pgch_array_end(pgch_writer *w);
bool     pgch_array_active(const pgch_writer *w);
chc_kind pgch_column_kind(const pgch_writer *w, size_t col);
```

Between `pgch_array_begin` and `pgch_array_end` every append targets the
element column and the `col` argument is ignored, so nesting for
`Array(Array(T))` is just nesting the pairs. `pgch_column_kind` reports the
element kind while a context is open, which is how a caller descends one level
at a time without walking the `chc_type` itself. It reports `CHC_STRING` for
`LowCardinality(String)`, since the PG side of that column is `text`.

One element append per item, and every open context closed before assembly:
an unbalanced pair leaves the offsets array short of the leaf row count, which
`chc_block_write` rejects as a row-count mismatch.

## Assembling a block

```c
size_t pgch_writer_rows(const pgch_writer *w);
size_t pgch_writer_bytes(const pgch_writer *w);

const chc_block_builder *pgch_writer_build(pgch_writer *w);
void                     pgch_writer_reset(pgch_writer *w);
```

`pgch_writer_build` assembles the `chc_column` tree and any `LowCardinality`
dictionary in a scratch context, and returns a builder referencing the live
buffers. It stays valid until `pgch_writer_reset`, which drops the scratch
context and empties the buffers while keeping their allocations for the next
block.

```c
const chc_block_builder *bb = pgch_writer_build(w);

if (chc_block_write(&io, bb, &opts, &err) != CHC_OK)
    pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ");
pgch_writer_reset(w);
```

`chc_block_opts` is the caller's: `pgch_block_opts_local` for chDB and
`clickhouse local`, `has_block_info` and `has_custom_serialization` per server
revision for the TCP path. A `chc_client` insert skips the io entirely and
passes the builder to `chc_client_send_data`.

```c
void pgch_writer_flush(pgch_writer *w, pgch_buf *out, const chc_block_opts *opts);
```

The same three calls when the destination is memory: build, write onto the end
of `out`, reset. NULL `opts` means `pgch_block_opts_local`. The buffer is the
caller's to hand on and reset.

`pgch_writer_bytes` is there to decide when to cut a block, since streaming a
long `COPY` should not accumulate every row in memory. The server coalesces
small blocks within one INSERT via `min_insert_block_size_rows` / `_bytes`, so
a cut-off in the tens of MiB costs nothing:

```c
if (pgch_writer_bytes(w) >= 64 * 1024 * 1024)
    flush(w);
```
