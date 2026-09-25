# pg-clickhouse-decode.h

Decode ClickHouse Native blocks into PostgreSQL Datums. See
[setup](pg-clickhouse.md) and [header](../pg-clickhouse-decode.h).

Define `PGCH_IMPLEMENTATION` in exactly one translation unit. Define
`CHC_IMPLEMENTATION` in same translation unit when using chunk source API.

## Type mapping

Default PostgreSQL types come from `pgch_pg_type_for`. `record` and
`record[]` describe anonymous composites, which cannot define table columns.
Consumers choose a concrete fallback, such as `text[]` for `Tuple` and
`text[][]` for `Map` and `Nested`.

Additional read targets list conversions beyond PostgreSQL explicit casts.
An empty cell still allows those casts. Values must fit target types.
Array elements convert individually; domains also enforce their constraints.
`Nullable` and `LowCardinality` use inner type conversions.

[Write conversions](pg-clickhouse-encode.md#type-mapping) follow separate
rules. Reading and writing may not preserve original types or values.

<!-- TYPE-TABLE-BEGIN -->
|          ClickHouse          |     Default PostgreSQL      |          Additional read targets          |                        Notes                         |
|------------------------------|-----------------------------|-------------------------------------------|------------------------------------------------------|
| Array(T)                     | T[]                         |                                           | One PG array type per depth                          |
| BFloat16                     | real                        |                                           |                                                      |
| Bool                         | boolean                     |                                           |                                                      |
| Date                         | date                        |                                           |                                                      |
| Date32                       | date                        |                                           |                                                      |
| DateTime                     | timestamp with time zone    | time                                      |                                                      |
| DateTime64(P)                | timestamp(P) with time zone | time                                      | P over 6 caps at 6                                   |
| Decimal(P,S)                 | numeric(P,S)                | xid8, oid8                                |                                                      |
| Decimal32(S)                 | numeric(9,S)                | xid8, oid8                                |                                                      |
| Decimal64(S)                 | numeric(18,S)               | xid8, oid8                                |                                                      |
| Decimal128(S)                | numeric(38,S)               | xid8, oid8                                |                                                      |
| Decimal256(S)                | numeric(76,S)               | xid8, oid8                                |                                                      |
| Enum8                        | text                        | bytea; input-compatible types             | Decodes label; PG enums with matching labels qualify |
| Enum16                       | text                        | bytea; input-compatible types             | Decodes label; PG enums with matching labels qualify |
| FixedString(N)               | text                        | bytea; input-compatible types             | Only bytea keeps trailing NULs                       |
| Float32                      | real                        |                                           |                                                      |
| Float64                      | double precision            |                                           |                                                      |
| IPv4                         | inet                        |                                           |                                                      |
| IPv6                         | inet                        |                                           |                                                      |
| Int8                         | smallint                    | boolean                                   | Zero is false; nonzero is true                       |
| Int16                        | smallint                    | boolean                                   | Zero is false; nonzero is true                       |
| Int32                        | integer                     |                                           |                                                      |
| Int64                        | bigint                      |                                           |                                                      |
| Int128                       | numeric(39,0)               | xid8, oid8                                |                                                      |
| Int256                       | numeric(77,0)               | xid8, oid8                                |                                                      |
| IntervalDay                  | interval                    | smallint, integer, bigint                 | Integers receive unit counts                         |
| IntervalHour                 | interval                    | smallint, integer, bigint                 | Integers receive unit counts                         |
| IntervalMicrosecond          | interval                    | smallint, integer, bigint                 | Integers receive unit counts                         |
| IntervalMillisecond          | interval                    | smallint, integer, bigint                 | Integers receive unit counts                         |
| IntervalMinute               | interval                    | smallint, integer, bigint                 | Integers receive unit counts                         |
| IntervalMonth                | interval                    | smallint, integer, bigint                 | Integers receive unit counts                         |
| IntervalNanosecond           | interval                    | smallint, integer, bigint                 | Integers keep ns; interval truncates to us           |
| IntervalQuarter              | interval                    | smallint, integer, bigint                 | Integers receive unit counts                         |
| IntervalSecond               | interval                    | smallint, integer, bigint                 | Integers receive unit counts                         |
| IntervalWeek                 | interval                    | smallint, integer, bigint                 | Integers receive unit counts                         |
| IntervalYear                 | interval                    | smallint, integer, bigint                 | Integers receive unit counts                         |
| JSON                         | jsonb                       | json, text, bytea; input-compatible types | jsonb normalizes document                            |
| LineString                   | path                        | lseg                                      | lseg requires two points                             |
| LowCardinality(T)            | T                           |                                           |                                                      |
| Map(K,V)                     | record[]                    | composite[], T[][], text                  | One record per pair                                  |
| MultiLineString              | path[]                      |                                           |                                                      |
| MultiPolygon                 | polygon[][]                 |                                           |                                                      |
| Nested(...)                  | record[]                    | composite[], T[][], text                  | One record per nested row                            |
| Nullable(T)                  | T                           |                                           | Sets nullable on the column                          |
| Point                        | point                       |                                           |                                                      |
| Polygon                      | polygon[]                   |                                           |                                                      |
| Ring                         | polygon                     | lseg                                      | lseg requires two points                             |
| SimpleAggregateFunction(f,T) | T                           |                                           | Stores values as T                                   |
| String                       | text                        | bytea; input-compatible types             | bytea keeps raw bytes                                |
| Time                         | time without time zone      |                                           |                                                      |
| Time64(P)                    | time(P) without time zone   |                                           | P over 6 caps at 6                                   |
| Tuple(...)                   | record                      | composite, T[], text; box, circle, line   | Match field order and types                          |
| UInt8                        | smallint                    | boolean                                   | Zero is false; nonzero is true                       |
| UInt16                       | integer                     |                                           |                                                      |
| UInt32                       | bigint                      |                                           |                                                      |
| UInt64                       | numeric(20,0)               | xid8, oid8                                |                                                      |
| UInt128                      | numeric(39,0)               | xid8, oid8                                |                                                      |
| UInt256                      | numeric(78,0)               | xid8, oid8                                |                                                      |
| UUID                         | uuid                        |                                           |                                                      |
<!-- TYPE-TABLE-END -->

Input-compatible types parse text through their PostgreSQL input function.
This includes PostgreSQL enums whose labels match ClickHouse Enum labels.
`bytea` preserves raw bytes; other targets drop trailing NUL padding.
Text targets apply configured encoding policy. JSON arrives as a document string.

For composite targets, match field order and types. `T[]` spreads tuple fields
into array elements when each field converts to `T` and no field is an array.
`Map` and `Nested` add an outer array dimension. Nested arrays must be
rectangular. A `text` target renders a PostgreSQL record or array literal.
Geometric tuple targets require matching coordinate shapes: two points for
`box`, a point and radius for `circle`, and three coefficients for `line`.

Empty rings and lines decode as NULL because PostgreSQL polygons and paths
require at least one point.

## Supply decoded blocks

```c
typedef struct pgch_block_source {
    void *ud;
    const chc_block *(*next_block)(void *ud);
    const char       *(*error)(void *ud);
} pgch_block_source;
```

`next_block` returns one block per call. Returning non-NULL transfers ownership
to reader. Reader destroys block with `pgch_alloc`, so source must read it with
same allocator.

Return `NULL` for end of stream. On failure, return `NULL` and make `error`
return message. `error` must be callable before first block, after every
`next_block`, and at end.

Reader runs `chc_column_validate` on every column of every block, so source
need not repeat it. Violation sets `reader.error` and ends stream.

Handle transport recovery in your source, including discarding connections
that cannot be reused after invalid data.

## Supply Native byte chunks

```c
typedef struct pgch_chunk_source {
    void *ud;
    bool (*next_chunk)(void *ud, const void **p, size_t *n, char **error);
    bool (*cancelled)(void *ud);
} pgch_chunk_source;

void pgch_reader_init_chunks(pgch_reader *r,
                             const pgch_chunk_source *src,
                             const chc_block_opts *opts);
```

Use chunk source when transport provides Native bytes instead of decoded
blocks. `next_chunk` returns pointer and length. Keep bytes valid until next
`next_chunk` call.

- Set `*n = 0` and return true for end of stream
- Set `*error` and return false for source failure
- Set `cancelled` to `NULL` when cancellation polling is not needed

Ending between blocks is clean end. Ending inside block produces truncation
error. Pass `NULL` options to use `pgch_block_opts_local`.

Blocks use memory context active when `pgch_reader_init_chunks` is called. Keep
this context alive until reader is finished. Callers can safely use a per-row
context while reading rows.

Chunk source API is available when implementation translation unit defines
both `PGCH_IMPLEMENTATION` and `CHC_IMPLEMENTATION`.

## Read rows

Initialize in memory context that should own reader state and error text.
Reader itself may be stack allocated.

Initialization reads first block and sets `ncols` and `coltypes`. Empty stream,
zero columns, unsupported schema, or source failure leaves reader done. Check
`reader.error` after initialization.

Call `pgch_reader_next` until false. Each successful call fills `values` and
`nulls`, both `ncols` long. Consume row before next call. Reader advances
across blocks and presents one continuous row stream.

Reader rejects unsupported column types, schema changes that would alter
returned Datum shape, and columns failing `chc_column_validate`. Those failures
set `reader.error`. Clean end leaves it `NULL`.

Value conversion can still raise PostgreSQL errors, including invalid JSON or
numeric text, text outside database encoding, values outside selected target
range, and payload that contradicts declared type.

`pgch_reader_free` releases current block and clears `reader.error`. Save error
pointer first when it must survive that call; storage remains valid until
initialization memory context is reset or deleted.

## Convert into target PostgreSQL types

See [Type mapping](#type-mapping) for supported targets and conversion rules.

Pass target `atttypmod`, or `-1` when the target carries none. Array
type modifiers apply to elements; pass them unchanged. Domains supply their
own. The modifier applies as PostgreSQL applies it on assignment:

| Target | Effect |
|--------|--------|
| `char(n)` | Pads to length |
| `varchar(n)` | Rejects values beyond length |
| `numeric(p,s)` | Rounds to scale |
| `time(n)`, `timestamp(n)` | Truncates to precision |
| domain | Own modifier over base conversion, then constraints per element; NULL rows skip conversion, caller enforces `NOT NULL` |

Use `pgch_reader_convert_init` to prepare each column before reading rows,
including columns containing only NULLs. Use `pgch_convert_init_type` for a
standalone ClickHouse type.

Set `reader.encoding_check` before preparing conversions to control invalid
text handling. Default policy raises an error. See `pgch_encoding_check` in
[header](../pg-clickhouse-decode.h) for alternatives. Use
`pgch_parse_encoding_check` to parse a string into a valid
`reader.encoding_check` value.

All initialization functions allocate state in `CurrentMemoryContext`. Build
state in context that outlives row loop. They return `NULL` when no conversion
is required. `pgch_convert` accepts `NULL` state and returns input unchanged.
`pgch_convert_free` releases top-level state; enclosing memory context owns
associated allocations.

## Fill target row

Use `pgch_reader_fill` to convert current row into caller arrays.
`values`, `nulls`, and optional
`states` must each hold `r->ncols` entries. Pass `NULL` for `states`, or use
`NULL` entries for columns requiring no conversion.

`pgch_reader_fill_map` writes column `i` to `dest[i]`. Allocate output arrays
for all destination attributes. Unmapped positions stay unchanged; initialize
them before filling:

```c
memset(slot->tts_isnull, true, desc->natts * sizeof(bool));
pgch_reader_fill_map(&reader, states, attnums,
                     slot->tts_values, slot->tts_isnull);
```

For text output, use `pgch_value_to_cstring`. It handles intermediate arrays
and tuples and applies your encoding policy. Use `bytea` to preserve raw bytes.

## Complete reader example

```c
pgch_reader reader;

pgch_reader_init(&reader, &source);
if (reader.error)
    ereport(ERROR, errmsg("%s", reader.error));

void **states = palloc0(reader.ncols * sizeof(*states));
Datum *values = palloc(reader.ncols * sizeof(*values));
bool *nulls = palloc(reader.ncols * sizeof(*nulls));

for (size_t i = 0; i < reader.ncols; i++)
    states[i] = pgch_reader_convert_init(&reader, i,
                                         TupleDescAttr(desc, i)->atttypid,
                                         TupleDescAttr(desc, i)->atttypmod);

while (pgch_reader_next(&reader)) {
    pgch_reader_fill(&reader, states, values, nulls);
    consume_tuple(heap_form_tuple(desc, values, nulls));
}

if (reader.error)
    ereport(ERROR, errmsg("%s", reader.error));

pgch_reader_free(&reader);
```
