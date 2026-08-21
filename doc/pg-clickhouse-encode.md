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
    pgch_raise(&err, ERRCODE_INVALID_PARAMETER_VALUE, NULL, "column \"items\"");

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
type cannot be encoded. Supported composite output is `Array`, `Tuple`, `Map`,
and the geometric types built over them; `Int128`, `Int256`, and unsupported
`LowCardinality` forms are rejected.

## Append PostgreSQL Datums

```c
void pgch_append_datum(pgch_writer *w, size_t col,
                       Datum val, Oid valtype, bool isnull);
void pgch_append_slot(pgch_writer *w, TupleTableSlot *slot);
```

Call `pgch_append_datum` once for every column in every row. Column order
within row does not matter. `valtype` must describe `val`. This is the only
value-append entry point; per-type conversion lives inside writer.

For PostgreSQL arrays, pass actual array OID. To pass `pgch_array`, use
`ANYARRAYOID`. Writer accepts compatible integer widths and PostgreSQL
explicit casts into destination PostgreSQL mapping. ClickHouse String-like
destinations also accept source type output representation. Missing
conversion raises `ERRCODE_DATATYPE_MISMATCH`.

A `Map` takes an array of pairs, each pair an array of key and value, so a
two-dimensional `text[]` fills one. A `Tuple` takes one array of its fields.
One array carries one element type while fields take their own, so a text
item parses through the field type's input function.

`bytea` values map to ClickHouse `String` and `FixedString` without text
conversion. `json` and `jsonb` map to `JSON`, `Object`, or `String`.
`FixedString` pads short values with NUL and raises
`ERRCODE_STRING_DATA_RIGHT_TRUNCATION` on longer ones, as ClickHouse rejects
them too. `Enum` takes text matching a declared name. `numeric` scales to
destination `Decimal`, and values exceeding its width raise instead of
wrapping. `inet` family must match `IPv4` or `IPv6`.

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

## NULL arrays

```c
typedef enum pgch_null_array {
    PGCH_NULL_ARRAY_ERROR = 0,
    PGCH_NULL_ARRAY_EMPTY,
} pgch_null_array;

void pgch_writer_set_null_array(pgch_writer *w, pgch_null_array policy);
```

Default `PGCH_NULL_ARRAY_ERROR` rejects NULL PostgreSQL arrays because
ClickHouse cannot represent nullable `Array` value. Set
`PGCH_NULL_ARRAY_EMPTY` to write empty array instead.

NaN and Infinity reach the destination unchanged, so `Float32` and `Float64`
take them and `Decimal` raises.

## Append arrays and tuples manually

```c
void pgch_array_begin(pgch_writer *w, size_t col);
void pgch_array_end(pgch_writer *w);
void pgch_tuple_begin(pgch_writer *w, size_t col);
void pgch_tuple_end(pgch_writer *w);
bool pgch_nest_active(const pgch_writer *w);

chc_kind pgch_column_kind(const pgch_writer *w, size_t col);
uint32_t pgch_column_datetime64_scale(const pgch_writer *w, size_t col);
```

Open array, append one value per element, then close array:

```c
pgch_array_begin(writer, col);
for (size_t i = 0; i < count; i++)
    pgch_append_datum(writer, 0, values[i], INT8OID, nulls[i]);
pgch_array_end(writer);
```

Open tuple, append one value per field left to right, then close tuple.
`pgch_tuple_end` raises unless every field took a value:

```c
pgch_tuple_begin(writer, col);
pgch_append_datum(writer, 0, name, TEXTOID, false);
pgch_append_datum(writer, 0, Int64GetDatum(count), INT8OID, false);
pgch_tuple_end(writer);
```

Nest both calls freely. `Map(K, V)` writes as `Array(Tuple(K, V))`:

```c
pgch_array_begin(writer, col);
for (size_t i = 0; i < npairs; i++) {
    pgch_tuple_begin(writer, 0);
    pgch_append_datum(writer, 0, keys[i], TEXTOID, false);
    pgch_append_datum(writer, 0, Int64GetDatum(vals[i]), INT8OID, false);
    pgch_tuple_end(writer);
}
pgch_array_end(writer);
```

While either context is active, `pgch_append_datum` ignores `col` and targets
current array element or tuple field. `pgch_column_kind` returns that target's
kind and returns `CHC_STRING` for `LowCardinality(String)`.
`pgch_column_datetime64_scale` returns `DateTime64` or `Time64` scale, zero for
other types; use it with `pgch_pow10` to see what precision a value keeps.

Close every context before building block.

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
    pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ", NULL);

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
