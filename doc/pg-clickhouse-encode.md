# pg-clickhouse-encode.h

Encode PostgreSQL values into ClickHouse Native blocks. See
[setup](pg-clickhouse.md) and [header](../pg-clickhouse-encode.h).

Define `PGCH_IMPLEMENTATION` in exactly one translation unit. Supply your own
transport and query execution.

## Type mapping

These defaults come from `pgch_ch_type_for` for non-nullable columns.
Types without a dedicated mapping use `String`. Domains use their base type
and type modifier. Arrays wrap the element mapping in `Array`, with nullable
elements; the array itself cannot be nullable. Nullable scalar columns use
`Nullable` where ClickHouse supports it.

<!-- ENCODE-TABLE-BEGIN -->
|    PostgreSQL    |           Default ClickHouse           |                       Notes                        |
|------------------|----------------------------------------|----------------------------------------------------|
| boolean          | Bool                                   |                                                    |
| smallint         | Int16                                  |                                                    |
| integer          | Int32                                  |                                                    |
| bigint           | Int64                                  |                                                    |
| oid              | UInt32                                 |                                                    |
| xid8             | UInt64                                 |                                                    |
| oid8             | UInt64                                 |                                                    |
| real             | Float32                                | BFloat16 drops low mantissa bits                   |
| double precision | Float64                                |                                                    |
| numeric          | Decimal256(38)                         | numeric_as_string selects String                   |
| numeric(12,6)    | Decimal(12,6)                          | Precision selects Decimal width                    |
| text             | String                                 | low_cardinality selects LowCardinality(String)     |
| bytea            | String                                 | Writes raw bytes                                   |
| date             | Date32                                 |                                                    |
| time             | Time64(6)                              |                                                    |
| timestamp        | DateTime64(6, 'UTC')                   |                                                    |
| timestamptz      | DateTime64(6, 'UTC')                   |                                                    |
| interval         | String                                 | Interval destinations require whole unit counts    |
| uuid             | UUID                                   |                                                    |
| json             | String                                 | json_as_json selects JSON                          |
| jsonb            | String                                 | json_as_json selects JSON                          |
| inet             | String                                 | Override with IPv4 or IPv6 matching address family |
| point            | Point                                  |                                                    |
| lseg             | LineString                             | Two points                                         |
| path             | LineString                             | Closed paths repeat the first point                |
| polygon          | Ring                                   |                                                    |
| box              | Tuple(high Point, low Point)           |                                                    |
| circle           | Tuple(center Point, radius Float64)    |                                                    |
| line             | Tuple(a Float64, b Float64, c Float64) |                                                    |
<!-- ENCODE-TABLE-END -->

[Read conversions](pg-clickhouse-decode.md#type-mapping) follow separate
rules. Reading and writing may not preserve original types or values.

| Explicit destination | Conversion |
|----------------------|------------|
| Integer | Accept compatible integer widths |
| `String` | Accept source type's text representation |
| `Interval` | Interpret integers as destination unit counts |
| Other | Accept PostgreSQL explicit casts to [destination's PostgreSQL mapping](pg-clickhouse-decode.md#type-mapping) |

See [Append PostgreSQL Datums](#append-postgresql-datums) for structural conversions and value constraints.

## Create writer

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
`Nested`, and the geometric types built over them; `Dynamic`, `Variant`, and
unsupported `LowCardinality` forms are rejected.
`SimpleAggregateFunction(f, T)` writes as `T`.

## Append PostgreSQL Datums

Call `pgch_append_datum` once for every column in every row. Column order
within row does not matter. Pass source type OID as `valtype`; writer handles
conversion.

For PostgreSQL arrays, pass actual array OID. To pass `pgch_array`, use
`ANYARRAYOID`. Missing conversion raises `ERRCODE_DATATYPE_MISMATCH`.

Pass a `Tuple` as an array of fields. Pass a `Map` as a two-dimensional array
of key/value pairs, or `Nested` as an array of rows. Text elements are parsed
as destination field types.

`bytea` values map to ClickHouse `String` and `FixedString` without text
conversion. `json` and `jsonb` map to `JSON`, `Object`, or `String`.
`FixedString` pads short values with NUL and rejects longer values. `Enum`
requires a declared label. `numeric` scales to destination `Decimal` and
rejects overflow. `inet` family must match `IPv4` or `IPv6`.

`interval` conversion rejects precision loss, such as 18 months written as
`IntervalYear`. `Float32` and `Float64` accept NaN and Infinity; `Decimal`
rejects them.

NULL requires a nullable destination, except for arrays handled by policy below.

`pgch_append_slot` appends one row from slot. It skips dropped and generated
attributes, matching `pgch_structure_from_tupdesc`. Build writer from same
descriptor and options to preserve column order.

## NULL arrays

Default `PGCH_NULL_ARRAY_ERROR` rejects NULL PostgreSQL arrays because
ClickHouse cannot represent a nullable array. Call
`pgch_writer_set_null_array(w, PGCH_NULL_ARRAY_EMPTY)` to write an empty array
instead.

## Recover from row errors

Zero-initialize `pgch_checkpoint`. Call `pgch_writer_checkpoint` before
appending each row. Saving again replaces previous position. Successful
appends need no further checkpoint calls.

If append fails, call `pgch_writer_rollback` to discard everything written
since checkpoint.

Save checkpoints only at row boundaries, with no array or tuple open. Resetting
writer or rolling back invalidates all saved checkpoints. Call
`pgch_checkpoint_free` when checkpoint is no longer needed.

## Append arrays and tuples manually

Open array, append one value per element, then close array:

```c
pgch_array_begin(writer, col);
for (size_t i = 0; i < count; i++)
    pgch_append_datum(writer, 0, values[i], INT8OID, nulls[i]);
pgch_array_end(writer);
```

Open tuple, append one value per field left to right, then close tuple.
`pgch_tuple_end` requires a value for every field:

```c
pgch_tuple_begin(writer, col);
pgch_append_datum(writer, 0, name, TEXTOID, false);
pgch_append_datum(writer, 0, Int64GetDatum(count), INT8OID, false);
pgch_tuple_end(writer);
```

Nest these calls as needed. Write `Map(K, V)` as `Array(Tuple(K, V))` and
`Nested(fields)` as `Array(Tuple(fields))`.

While either context is active, `pgch_append_datum` ignores `col` and targets
current array element or tuple field.

Close every context before building block.

## Inspect and write buffered rows

`pgch_writer_rows` returns first column row count.
`pgch_writer_bytes` returns buffered bytes across all columns. Use these to
choose block boundary.

`pgch_writer_flush` appends serialized block to `pgch_buf`, then resets writer
after a successful write. Pass `NULL` options to use `pgch_block_opts_local`.

```c
pgch_buf out = {};

pgch_writer_flush(writer, &out, NULL);
send_native(out.data, out.len);
pgch_buf_reset(&out);
```

For another output destination, pass `pgch_writer_build` to `chc_block_write`.
Built block borrows writer buffers; finish writing before calling
`pgch_writer_reset`. Reset empties buffers and keeps storage for reuse.
