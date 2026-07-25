# pg-clickhouse-encode.h

Encode PostgreSQL values into ClickHouse Native blocks. Include
[pg-clickhouse.h](pg-clickhouse.md), then this header.

Define `PGCH_IMPLEMENTATION` in exactly one translation unit. Writer does not
send data or execute queries; consumer chooses clickhouse-c client, socket,
chDB stream, or in-memory destination.

## Create writer

```c
typedef struct pgch_col {
    const char     *name;
    size_t          name_len;
    const chc_type *type;
} pgch_col;

typedef struct pgch_writer pgch_writer;

pgch_writer *pgch_writer_new(MemoryContext parent,
                             const pgch_col *cols, size_t ncols);
void pgch_writer_free(pgch_writer *w);
```

Parse same ClickHouse declarations used by destination:

```c
chc_type *type;
chc_err err = {};

if (chc_type_parse("Array(Int32)", 12, &pgch_alloc, &type, &err) != CHC_OK)
    pgch_raise(&err, ERRCODE_INVALID_PARAMETER_VALUE, "type parse: ");

pgch_col col = {
    .name = "items",
    .name_len = 5,
    .type = type,
};

pgch_writer *writer = pgch_writer_new(CurrentMemoryContext, &col, 1);
```

Writer copies column names and borrows `chc_type` pointers. Keep types alive
until `pgch_writer_free`. Writer owns a child memory context under `parent`;
freeing writer or deleting parent releases writer state.

`pgch_writer_new` raises `ERRCODE_FDW_INVALID_DATA_TYPE` when any ClickHouse
type cannot be encoded. Supported composite output is `Array`; `Tuple`, `Map`,
`Int128`, `Int256`, and unsupported `LowCardinality` forms are rejected.

## Append PostgreSQL Datums

```c
void pgch_append_datum(pgch_writer *w, size_t col,
                       Datum val, Oid valtype, bool isnull);
void pgch_append_slot(pgch_writer *w, TupleTableSlot *slot);
```

Call `pgch_append_datum` once for every column in every row. Column order
within row does not matter. `valtype` must describe `val`.

For PostgreSQL arrays, pass actual array OID. To pass `pgch_array`, use
`ANYARRAYOID`. Writer accepts compatible integer widths and PostgreSQL
explicit casts into destination PostgreSQL mapping. ClickHouse String-like
destinations also accept source type output representation. Missing
conversion raises `ERRCODE_DATATYPE_MISMATCH`.

`bytea` values map to ClickHouse `String` and `FixedString` without text
conversion. `json` and `jsonb` map to `JSON`, `Object`, or `String`.

NULL requires nullable destination. NULL passed to non-nullable destination
raises `ERRCODE_NOT_NULL_VIOLATION`, subject to array policy below.

`pgch_append_slot` appends one row from slot. It skips dropped and generated
attributes, matching `pgch_structure_from_tupdesc`. Build writer from same
descriptor and options to preserve positional alignment.

## Intermediate array representations

```c
Datum pgch_array_from_pg(Datum arr, Oid elemtype,
                         int16 typlen, bool typbyval, char typalign);
```

`pgch_append_datum` converts PostgreSQL arrays automatically. Use
`pgch_array_from_pg` when consumer already caches element type metadata:

```c
Datum value = pgch_array_from_pg(array, elemtype,
                                 typlen, typbyval, typalign);
pgch_append_datum(writer, col, value, ANYARRAYOID, false);
```

Return value is allocated in `CurrentMemoryContext`.

## Policies

```c
typedef enum pgch_null_array {
    PGCH_NULL_ARRAY_ERROR = 0,
    PGCH_NULL_ARRAY_EMPTY,
} pgch_null_array;

typedef enum pgch_nonfinite {
    PGCH_NONFINITE_KEEP = 0,
    PGCH_NONFINITE_NULL,
    PGCH_NONFINITE_ZERO,
} pgch_nonfinite;

void pgch_writer_set_null_array(pgch_writer *w, pgch_null_array policy);
void pgch_writer_set_nonfinite(pgch_writer *w, pgch_nonfinite policy);
```

Default `PGCH_NULL_ARRAY_ERROR` rejects NULL PostgreSQL arrays because
ClickHouse cannot represent nullable `Array` value. Set
`PGCH_NULL_ARRAY_EMPTY` to write empty array instead.

Default `PGCH_NONFINITE_KEEP` writes NaN and Infinity when destination supports
them and raises otherwise. `PGCH_NONFINITE_NULL` replaces them with NULL;
non-nullable destinations still raise. `PGCH_NONFINITE_ZERO` replaces them
with zero.

## Typed append API

Use typed functions when values are not PostgreSQL Datums:

```c
void pgch_append_int(pgch_writer *w, size_t col,
                     int64_t val, bool isnull);
void pgch_append_uint(pgch_writer *w, size_t col,
                      uint64_t val, bool isnull);
void pgch_append_bool(pgch_writer *w, size_t col,
                      bool val, bool isnull);
void pgch_append_double(pgch_writer *w, size_t col,
                        double val, bool isnull);
void pgch_append_float(pgch_writer *w, size_t col,
                       float val, bool isnull);
void pgch_append_bytes(pgch_writer *w, size_t col,
                       const void *p, size_t n, bool isnull);
void pgch_append_decimal(pgch_writer *w, size_t col,
                         const char *digits, bool isnull);
void pgch_append_uuid(pgch_writer *w, size_t col,
                      const uint8_t bytes[16], bool isnull);
void pgch_append_inet(pgch_writer *w, size_t col,
                      const uint8_t *addr_be, size_t addrlen, bool isnull);
```

`pgch_append_bytes` supports `String`, `FixedString`, `Enum`,
`LowCardinality(String)`, and `JSON`. `FixedString` pads short values with
NUL and truncates long values. Enum input must match declared name.

`pgch_append_decimal` accepts `[-]digits[.frac]`; destination column supplies
scale. PostgreSQL `numeric_out` output is suitable input.

`pgch_append_uuid` accepts PostgreSQL UUID byte order.

`pgch_append_inet` accepts address bytes in network order: four bytes for IPv4
and sixteen bytes for IPv6. Pass matching width for NULL values.

Date and time append APIs use these units:

```c
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

uint32_t pgch_column_datetime64_scale(const pgch_writer *w, size_t col);
```

- Date and DateTime accept seconds since Unix epoch
- Time accepts seconds since midnight
- DateTime64 and Time64 accept already-scaled wire integers

Use `pgch_column_datetime64_scale` with `pgch_pow10` when scaling source
values.

## Append arrays manually

```c
void pgch_array_begin(pgch_writer *w, size_t col);
void pgch_array_end(pgch_writer *w);
bool pgch_array_active(const pgch_writer *w);
chc_kind pgch_column_kind(const pgch_writer *w, size_t col);
```

Open array, append one value per element, then close array:

```c
pgch_array_begin(writer, col);
for (size_t i = 0; i < count; i++)
    pgch_append_int(writer, 0, values[i], nulls[i]);
pgch_array_end(writer);
```

While array context is active, append functions ignore `col` and target
current element column. Nest begin/end calls for nested arrays.
`pgch_column_kind` returns current element kind while array is open and
returns `CHC_STRING` for `LowCardinality(String)`.

Close every array context before building block.

## Inspect and write buffered rows

```c
size_t pgch_writer_rows(const pgch_writer *w);
size_t pgch_writer_bytes(const pgch_writer *w);

const chc_block_builder *pgch_writer_build(pgch_writer *w);
void pgch_writer_reset(pgch_writer *w);
void pgch_writer_flush(pgch_writer *w, pgch_buf *out,
                       const chc_block_opts *opts);
```

`pgch_writer_rows` returns first column row count.
`pgch_writer_bytes` returns buffered bytes across all columns. Use these to
choose block boundary.

`pgch_writer_build` returns block builder referencing writer buffers. Use it
before `pgch_writer_reset`. Reset releases built block and empties buffers
while retaining capacity.

```c
const chc_block_builder *block = pgch_writer_build(writer);

if (chc_block_write(&io, block, &opts, &err) != CHC_OK)
    pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ");

pgch_writer_reset(writer);
```

`pgch_writer_flush` appends serialized block to `pgch_buf`, then resets writer
after a successful write. Pass `NULL` options to use `pgch_block_opts_local`.

```c
pgch_buf out = {};

pgch_writer_flush(writer, &out, NULL);
send_native(out.data, out.len);
pgch_buf_reset(&out);
```
